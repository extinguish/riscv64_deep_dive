# ELF / Linker / Loader 深度解析

从 ELF 文件结构、内存映射、重定位，到 AArch64 OP-TEE 的真实加载与执行流程

## 一、先建立整体认知：ELF Loading 究竟在做什么？

我认为，理解 ELF 最重要的不是记住 `Elf64_Ehdr`、`Elf64_Phdr` 等数据结构，而是回答一个根本问题：

编译器和链接器生成的机器代码，究竟经过了哪些操作，才能真正成为 CPU 可以执行的程序？

尤其是以下几个容易混淆的问题：

1. ELF 文件中的 `.text`、`.data`、`.bss` 究竟是什么？它们与实际内存中的代码段、数据段是什么关系？
2. Linker 已经确定了函数和变量的地址，为什么 Loader 还需要执行 Relocation？
3. ELF 里面有 `e_entry`，为什么把 PC 设置为这个地址，有时仍然无法执行？
4. Loader 把代码复制到内存之后，CPU 如何正确访问全局变量、调用函数、操作栈？
5. AArch64 的 `ADRP`、`ADD`、`LDR`、`BR`、`BLR` 与 ELF 加载有什么关系？
6. OP-TEE 为什么需要 `ldelf`？既然 OP-TEE Core 已经运行，为什么还要另外实现 ELF Loader？
7. OP-TEE 的用户态 TA 是如何从 `.ta` 文件最终变成运行在 S-EL0 的程序的？

我们会逐一回答。

首先，请记住一个总模型：

ELF 从源码到执行的生命周期

Source Code

C / C++ / Assembly

Compiler / Assembler

Relocatable Object Files

.o / ET_REL：Section、Symbol、Relocation

Static Linker：ld / lld

Linked ELF Image

ET_EXEC / ET_DYN：Program Headers + Entry Point

Loader：验证、映射、初始化、重定位

Runtime Address Space

Executable mappings / Data / BSS / Heap / Stack

CPU：PC、SP、页表、执行级别

Entry Assembly → Runtime → Application

\_start / TA Entry → 初始化 → 业务函数

这条链路可以分为三个阶段：

| 阶段              | 核心职责                                   |
| --------------- | -------------------------------------- |
| Build Time      | Compiler / Assembler 生成代码、符号和重定位信息     |
| Link Time       | Linker 合并 Section、解析符号、分配地址、生成可加载映像    |
| Load / Run Time | Loader 创建内存映射、完成剩余重定位、准备运行上下文，CPU 开始执行 |

特别需要注意：

Linker 决定程序在逻辑地址空间中的布局；Loader 负责让这种布局在真实运行环境中成立。

而 CPU 不懂 ELF，也不会主动解析 ELF Header。CPU 只知道从当前 PC 指向的地址取指、译码、执行。

因此，ELF Loading 的最终目的，就是让 CPU 看到一个地址关系正确、运行上下文完整的可执行内存映像。

# Part I：ELF 理论与底层实现

## 二、ELF 到底是什么？

ELF 全称是 Executable and Linkable Format。

它不是单纯的可执行文件格式，还可以表达：

- 可重定位目标文件
- 可执行程序
- 动态共享库
- Core Dump

最常见的三种类型是：

| ELF 类型        | `e_type`  | 用途                 |
| ------------- | --------- | ------------------ |
| Relocatable   | `ET_REL`  | `.o` 目标文件，等待链接     |
| Executable    | `ET_EXEC` | 具有固定链接地址的可执行映像     |
| Shared Object | `ET_DYN`  | `.so`，以及 PIE 可执行程序 |

这里有一个重要细节：

`ET_DYN` 不代表文件一定是共享库。现代 Linux 上使用 PIE（Position Independent Executable）的主程序也通常是 `ET_DYN`。

OP-TEE AArch64 用户 TA 的 ELF 加载机制也大量依赖 `ET_DYN`。

## 三、ELF 文件内部究竟包含什么？

ELF 有两个必须严格区分的视角：

- Linking View：面向 Linker，主要通过 Section Header Table 描述。
- Execution View：面向 Loader，主要通过 Program Header Table 描述。

Linking View

Section 组织方式

.text

.rodata

.data

.bss

.rela.\*

.symtab

.strtab

主要服务于符号解析和链接

Execution View

Segment 组织方式

PT_LOAD RX

可执行代码及相关只读区域

PT_LOAD R

只读数据等

PT_LOAD RW

已初始化数据 + BSS

主要服务于实际内存映射

一个很重要的结论：

Loader 通常以 Program Header 中的 `PT_LOAD` 为主要依据，而不是简单地遍历 `.text`、`.data`、`.bss` Section 进行加载。

Section 是链接层面的组织单位；Segment 是运行时加载层面的组织单位。

两者不是一一对应的。一个 Segment 可以包含多个 Section，而一个 Section 的内存布局也会受到对齐和 Segment 划分约束。

### 3.1 ELF Header：整个文件的入口

64 位 ELF 的主头为 `Elf64_Ehdr`：

```
typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;
```

重点关注以下字段：

| 字段          | 意义                         |
| ----------- | -------------------------- |
| `e_ident`   | Magic、ELF32/64、大小端等        |
| `e_machine` | 目标 ISA，例如 `EM_AARCH64`     |
| `e_type`    | ET_REL、ET_EXEC、ET_DYN      |
| `e_entry`   | ELF 定义的入口虚拟地址              |
| `e_phoff`   | Program Header Table 的文件偏移 |
| `e_phnum`   | Program Header 的数量         |
| `e_shoff`   | Section Header Table 的文件偏移 |

其中最容易误解的是 `e_entry`。

假设：

```
e_entry = 0x0000000000001840
```

这个值并不意味着 CPU 总要直接跳转到运行时地址 `0x1840`。

如果这是一个被加载到 `0x40000000` 的、以零为链接基准的 `ET_DYN` 映像，那么运行时入口可能是：

```
0x40000000 + 0x1840
= 0x40001840
```

也就是：

\\[ \text{Runtime Entry}=\text{Load Bias}+e\\\_entry \\]

对于典型的固定地址 `ET_EXEC`，加载偏移通常为零，入口地址就等于 `e_entry`。

### 3.2 Program Header：真正告诉 Loader 如何加载

看一下它的核心结构：

```
typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;
```

这八个字段尤其值得深入。

| 字段         | 具体作用                        |
| ---------- | --------------------------- |
| `p_type`   | Segment 的类型，`PT_LOAD` 表示可加载 |
| `p_flags`  | PF_R、PF_W、PF_X 权限           |
| `p_offset` | 数据在 ELF 文件中的偏移              |
| `p_vaddr`  | Segment 的链接虚拟地址             |
| `p_paddr`  | 目标物理地址，含义依赖平台               |
| `p_filesz` | Segment 中需要从文件提供的字节数        |
| `p_memsz`  | Segment 在内存中的总字节数           |
| `p_align`  | 地址与文件偏移的对齐要求                |

需要特别强调 `p_paddr`。

对于 Linux 用户态程序，普通 ELF Loader 通常主要使用 `p_vaddr`，而不是把 `p_paddr` 当成必须加载的物理地址。

但是在 Bare-metal、Bootloader、Firmware 场景，`p_paddr` 可能被用来表达装载物理地址，也可能完全不被使用。这由具体 Loader 的契约决定，不能仅凭 ELF 文件头判断。

## 四、第一次理解 ELF Loading：一个完整数字示例

假设我们有这样一个 ELF 文件：

```
ELF Header:
    e_type  = ET_DYN
    e_entry = 0x1100

Program Headers:

LOAD #0:
    p_offset = 0x0000
    p_vaddr  = 0x0000
    p_filesz = 0x2800
    p_memsz  = 0x2800
    p_flags  = R | X

LOAD #1:
    p_offset = 0x3000
    p_vaddr  = 0x4000
    p_filesz = 0x0600
    p_memsz  = 0x1000
    p_flags  = R | W
```

假设 Loader 选择：

```
Load Bias = 0x40000000
Page Size = 4KB
```

### 4.1 第一个 Segment：加载代码

对于第一个 `PT_LOAD`：

```
File Offset:    0x0000
Virtual Offset: 0x0000
File Size:      0x2800
Memory Size:    0x2800
Permission:     R-X
```

Loader 需要建立以下关系：

```
ELF 文件 [0x0000, 0x2800)
             |
             v
运行内存 [0x40000000, 0x40002800)
```

对于支持文件映射的 OS，它可以通过 `mmap()` 建立文件到内存的映射。

对于简单 Firmware Loader，则可能直接使用 `memcpy()`。

但要注意：使用 `mmap()` 时，映射地址和文件偏移都需要满足页对齐要求；实际映射范围通常会扩大到完整页面。

### 4.2 第二个 Segment：加载 DATA 和 BSS

第二个 `PT_LOAD`：

```
File Offset:    0x3000
Virtual Offset: 0x4000
File Size:      0x0600
Memory Size:    0x1000
Permission:     RW-
```

它的目标运行地址为：

```
0x40000000 + 0x4000
= 0x40004000
```

于是 Loader 要完成：

```
文件 [0x3000, 0x3600)
             |
             v
内存 [0x40004000, 0x40004600)

然后清零：
内存 [0x40004600, 0x40005000)
```

最终虚拟内存布局

低地址

Load Bias = 0x40000000

0x40000000 – 0x40002800

Code / Read-only，RX，来自 ELF 文件

Entry = 0x40001100

0x40002800 – 0x40004000：未使用的虚拟地址区间及 Segment 对齐空间

0x40004000 – 0x40004600

Initialized Data，RW，文件数据

0x40004600 – 0x40005000

Zero-fill / BSS，RW，不需要对应的文件字节

示意图省略页粒度映射细节，独立的 Heap / Stack 由运行环境另行建立。

这正是下面两个字段的实际用途：

```
p_filesz = 0x600;
p_memsz  = 0x1000;
```

它意味着：

- 前 `0x600` 字节来自 ELF 文件。
- 剩余 `0xA00` 字节必须初始化为零。
- 因为整个 Segment 的内存需求是 `0x1000` 字节。

这就是 `.bss` 不必完整占用 ELF 文件空间的根本原因。

这里还要澄清一个易错点：`p_memsz - p_filesz` 表示 Segment 末尾需要补零的区域，但它不一定严格等同于 `.bss` Section 的大小。

### 4.3 最终入口地址

根据 `e_entry`：

```
e_entry   = 0x1100
load_bias = 0x40000000

Runtime PC = 0x40001100
```

Loader 必须确保这个地址所在的页面：

- 已经被映射；
- 具有执行权限；
- 指向正确的机器指令；
- 当前 CPU 的页表和执行状态允许访问。

至此，才具备进入第一条指令的基本条件。

但是，这仍然不代表程序一定可以正常运行，因为还有一个非常关键的问题没有解决：代码和数据内部保存的地址是否正确。

## 五、Linker 与 Loader 为什么都涉及 Relocation？

Relocation 是理解 ELF 最关键、也最容易混淆的机制。

先看一个 C 语言例子：

```
int global_value = 100;

int get_value(void)
{
    return global_value;
}
```

编译器生成的代码必须能够找到 `global_value`。

问题是：

```
编译阶段：
    global_value 的最终地址可能尚未确定。

链接阶段：
    Linker 可以确定它在映像中的位置。

加载阶段：
    如果映像被加载到其他地址，
    代码或数据中的部分绝对地址仍可能需要修正。
```

所以，Relocation 至少有两个层次。

### 5.1 Link-time Relocation

假设汇编源文件有：

```
bl get_value
```

在 `.o` 文件阶段，Assembler 可能并不知道 `get_value` 的最终位置。

它会生成：

- 暂时的指令编码；
- 对应的 Relocation Entry；
- 对目标 Symbol 的引用。

当 Linker 合并目标文件时，才能知道：

```
Call Site:  0x1000
get_value: 0x2000
```

然后计算跳转位移：

```
0x2000 - 0x1000 = 0x1000
```

对于 AArch64 `BL`，它把相对于当前指令地址的跳转偏移编码到 26 位立即数字段中，再按指令定义放大为字节偏移。

Linker 完成这种修正后，相关的 Link-time Relocation 就已经处理完了。

这样的 `BL` 通常不需要 Loader 再次重定位。

原因是：如果整个代码映像统一平移，两个指令地址之间的差值不会变化。

### 5.2 Load-time Relocation

再考虑另一个例子：

```
int value = 123;
int *global_ptr = &value;
```

假设 Linker 计算：

```
value      = 0x2000
global_ptr = 0x3000
```

`global_ptr` 存储的数据本身是一个地址。

在 PIE / ET_DYN 映像中，如果实际加载基址为 `0x40000000`：

```
value 的运行地址      = 0x40002000
global_ptr 的运行地址 = 0x40003000
```

因此，`global_ptr` 的内容必须成为：

```
*(uint64_t *)0x40003000 = 0x40002000
```

否则，当执行：

```
return *global_ptr;
```

CPU 就可能访问错误的地址。

这时就需要动态重定位。

### 5.3 AArch64 最重要的重定位类型：R_AARCH64_RELATIVE

它的计算公式是：

\\[ \boxed{\operatorname{Store64}(B+P)=B+A} \\]

其中：

- \\(B\\)：Load Bias，也就是运行时加载偏移。
- \\(P\\)：Relocation Entry 中的 `r_offset`。
- \\(A\\)：`r_addend`。
- \\(B+P\\)：需要修正的实际内存地址。
- \\(B+A\\)：要写入的最终指针值。

举例：

```
B = 0x40000000
P = 0x3000
A = 0x2000
```

结果：

```
Write Address = 0x40003000
Write Value   = 0x40002000
```

对应 Loader 逻辑为：

```
uint64_t *where = (uint64_t *)(B + P);

*where = B + A;
```

后面我们会看到，OP-TEE 的 AArch64 `ldelf/start_a64.S` 几乎就是在汇编层面执行这样的操作。

### 5.4 为什么 AArch64 的 ADRP + ADD 不一定需要 Load-time Relocation？

看下面这段代码：

```
adrp x0, global_value
add  x0, x0, :lo12:global_value
ldr  w1, [x0]
```

在没有跨映像符号绑定等额外问题、且链接器已解析静态重定位的典型 PIC 场景中：

- `ADRP` 形成相对于当前 PC 所在页面的偏移。
- `ADD` 补上目标地址的低 12 位。
- `LDR` 从得到的地址读取数据。

假设 Linker 知道：

```
Instruction Page = 0x1000
global_value     = 0x3008
```

`ADRP` 需要的页面相对差值是：

```
0x3000 - 0x1000 = 0x2000
```

如果整个 ELF 被加载到 `0x40000000`：

```
Instruction Page = 0x40001000
global_value     = 0x40003008
```

页面相对差值仍然是：

```
0x40003000 - 0x40001000 = 0x2000
```

因此，对这种已经完成静态重定位的 PC-relative 指令序列，Loader 无需再修改机器码。

而直接存储在 `.data` 中的绝对指针，通常没有这样的天然平移特性。

这就解释了为什么现代 PIC/PIE 代码能够大幅减少运行时对 `.text` 的修改。

## 六、ELF Relocation Entry 到底如何表示？

AArch64 常用显式加数的 `Elf64_Rela`：

```
typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;
```

每个 Entry 是 24 字节。

其中：

```
r_offset   // 需要修改的位置
r_info     // Symbol Index + Relocation Type
r_addend   // 用于地址计算的加数
```

对于 ELF64，通常通过：

```
ELF64_R_SYM(r_info)
ELF64_R_TYPE(r_info)
```

提取符号索引和重定位类型。

几种关键类型：

| 类型                    | 用途                        |
| --------------------- | ------------------------- |
| `R_AARCH64_RELATIVE`  | 基于 Load Bias 修正地址         |
| `R_AARCH64_ABS64`     | 写入经符号解析得到的 64 位绝对地址       |
| `R_AARCH64_GLOB_DAT`  | 为全局符号引用填充地址               |
| `R_AARCH64_JUMP_SLOT` | 为函数调用的 PLT/GOT 路径填充目标地址   |
| `R_AARCH64_IRELATIVE` | 根据运行时 resolver 的结果完成间接重定位 |

并不是所有 Loader 都支持这些类型。

不同 ELF 执行环境有不同的 ABI 契约。例如，一个仅支持静态 PIE 固件的 Loader，可能只实现 `R_AARCH64_RELATIVE`。

而完整的动态链接器还需要处理动态符号查找、依赖库加载、重定位顺序、TLS 等问题。

## 七、ELF Loader 的完整执行算法

现在先忽略 Linux 或 OP-TEE 的具体差异，抽象出一个通用 Loader。

### 7.1 第一阶段：验证 ELF

检查：

```
e_ident[0..3] == "\x7fELF"
e_ident[EI_CLASS] == ELFCLASS64
e_machine == EM_AARCH64
e_type == ET_EXEC || e_type == ET_DYN
```

实际代码还必须检查：

- 文件与 Header 的边界；
- Program Header Table 是否越界；
- 各 Segment 是否溢出或重叠；
- `p_filesz <= p_memsz`；
- `p_align` 和地址约束；
- 入口地址是否合法；
- 可执行 Segment 权限是否符合策略；
- 是否存在不支持的动态特性。

安全 Loader 还需要验证映像身份与完整性。

### 7.2 第二阶段：解析 Program Header

核心伪代码：

```
for (i = 0; i < ehdr->e_phnum; i++) {
    phdr = get_phdr(i);

    if (phdr->p_type != PT_LOAD)
        continue;

    prepare_load_segment(phdr);
}
```

这里很重要的一点是：

不是每一个 Program Header 都表示需要独立分配内存并复制的 Segment。

例如：

- `PT_LOAD`：描述可加载内存区域。
- `PT_DYNAMIC`：提供动态链接相关信息。
- `PT_INTERP`：指定解释器路径，常见于 Linux 动态程序。
- `PT_TLS`：描述 TLS 初始化模板。
- `PT_GNU_RELRO`：指定完成重定位后需要收紧权限的范围。
- `PT_GNU_STACK`：表达栈执行权限需求。

### 7.3 第三阶段：建立运行时内存映射

简化的教学代码如下：

```
for_each_load_segment(phdr) {
    void *dst = (void *)(load_bias + phdr->p_vaddr);

    allocate_and_map(dst, phdr->p_memsz);

    memcpy(dst,
           elf_file + phdr->p_offset,
           phdr->p_filesz);

    memset((uint8_t *)dst + phdr->p_filesz,
           0,
           phdr->p_memsz - phdr->p_filesz);
}
```

这是一个抽象算法，不是可以直接用于生产环境的完整 Loader。

真正实现时，必须处理：

- 页面对齐与非页对齐区间；
- 多个 Segment 共享边界页面；
- 内存保护和 W^X；
- 物理页分配与虚拟映射；
- 文件映射和按需缺页加载；
- 缓存一致性及指令缓存维护；
- 加载地址随机化；
- 加载失败后的资源清理。

特别是 `allocate_and_map()`，在 OS Loader 中意味着建立页表映射；在 Bare-metal Loader 中可能意味着选择目标物理内存并复制数据。

它并不总是分配一段连续的物理内存。

### 7.4 第四阶段：完成 Load-time Relocation

最简单的 `RELA` 修正逻辑：

```
for (size_t i = 0; i < rela_count; i++) {
    Elf64_Rela *r = &rela[i];

    if (ELF64_R_TYPE(r->r_info) == R_AARCH64_RELATIVE) {
        uint64_t *where =
            (uint64_t *)(load_bias + r->r_offset);

        *where = load_bias + r->r_addend;
    }
}
```

真实动态链接器还需要解析 `DT_NEEDED`、符号表、GOT/PLT、TLS、重定位依赖等。

有些环境会在此后把 RELRO 区域设为只读。

### 7.5 第五阶段：完成运行时环境初始化

Loader 需要准备适用于目标程序的 CPU 和内存上下文。

通常包括：

- Stack；
- Initial Registers；
- Page Tables / Address Space；
- Exception Level / Privilege Mode；
- Runtime 参数；
- TLS；
- 需要的异常、系统调用或返回机制。

但这些动作不一定全部由 ELF Loader 自己完成。实际系统中可能由 OS Kernel、Runtime Loader、C Runtime Startup Code 分工完成。

这一点在 OP-TEE 中尤为明显。

### 7.6 第六阶段：转移控制权

最终入口计算：

```
entry = load_bias + ehdr->e_entry;
```

可以把 CPU 控制权转移抽象成：

```
mov x0, #initial_arg
mov sp, x1
br  x2
```

其中 `x2` 保存入口地址。

不过，这段只是说明寄存器准备与分支的概念。

如果涉及 AArch64 EL1 → EL0 的切换，不能仅使用 `BR`，必须通过异常返回机制设置目标状态并执行 `ERET`。

这就是我们接下来分析 OP-TEE 时要特别关注的地方。

## 八、Loader 完成映射后，CPU 怎样真正开始执行？

这是 ELF 理论的最后一环，也是后面分析汇编代码的基础。

需要区分三件事情：

1. ELF Loader 把程序装入内存。
2. 操作系统或执行框架把 CPU 上下文设置好。
3. CPU 从指定入口地址开始执行，并通过 Runtime 进入应用逻辑。

它们并不等价。

### 8.1 为什么 `e_entry` 通常不是 `main()`？

以一个典型的 Linux AArch64 程序为例：

```
execve()
    |
    v
Linux ELF Loader
    |
    | 解析 ELF
    | 映射 PT_LOAD
    | 建立用户态 Stack
    | 准备 argc/argv/envp/auxv
    | 如果有 PT_INTERP，加载动态链接器
    v
EL0 Entry
    |
    v
ld.so 入口（动态程序）
    |
    | 解析共享库依赖
    | 完成动态重定位
    | 初始化 TLS 等
    v
Executable _start
    |
    v
C Runtime Startup
    |
    v
main(argc, argv, envp)
```

如果是动态链接程序，Linux Kernel 初始交出执行权的地址通常是 `PT_INTERP` 指定的动态链接器入口，而不是主程序自己的 `e_entry`。

动态链接器完成工作后才转移到主程序的入口。

如果是没有解释器的静态可执行程序，Kernel 可以直接从主程序的入口开始运行。

同样，操作系统并不知道 `main()` 的语言语义。`main()` 是 C Runtime Startup 调用的。

### 8.2 AArch64 的执行上下文

假设现在 OP-TEE Core 运行在 S-EL1，准备执行某个 S-EL0 TA。

需要设置的关键寄存器包括：

| 寄存器         | 作用                      |
| ----------- | ----------------------- |
| `ELR_EL1`   | 异常返回后的 PC               |
| `SPSR_EL1`  | 异常返回后恢复的 PSTATE，包括目标 EL |
| `SP_EL0`    | EL0 使用的 Stack Pointer   |
| `TTBR0_EL1` | 参与 EL0 地址转换的页表基址        |
| `TCR_EL1`   | 地址转换控制配置                |
| `MAIR_EL1`  | 内存属性编码                  |
| `SCTLR_EL1` | MMU、Cache 等系统控制         |
| `VBAR_EL1`  | EL1 异常向量表地址             |

不同状态有些已经由 Core 初始化完成，不需要每次进入 TA 都重新设置。

在已经准备好地址空间的前提下，关键步骤可以用下面这段教学汇编表示：

```
/* x0 = User entry VA
 * x1 = User stack top
 */

msr elr_el1, x0

mov x2, #0
msr spsr_el1, x2    // 简化的 EL0t 状态示例

msr sp_el0, x1

isb
eret
```

`ERET` 的意义是：

```
Before ERET:
    Current EL = S-EL1

After ERET:
    Current EL = S-EL0
    PC         = ELR_EL1
    PSTATE     = SPSR_EL1
    SP         = SP_EL0
```

这就解释了一个重要问题：

ELF Header 只负责描述程序的入口，但 CPU 究竟如何进入这个入口，完全取决于执行环境。

在 OP-TEE 中，核心实际上是通过 `thread_enter_user_mode()` 和底层 AArch64 异常返回汇编实现这个过程。

后面我们会直接分析这段真实代码。

### 8.3 站在 CPU 视角，Loader 做了什么？

CPU 并不知道前面有一个 ELF 文件。

当 CPU 进入目标程序时，它面对的是：

```
PC = 0x40001100

Address Translation:
    VA 0x40001100
       |
       v
    Page Table Walk
       |
       v
    Physical Memory
       |
       v
    AArch64 Instruction

Fetch → Decode → Execute
```

执行期间：

- `BL` 调用函数；
- `RET` 从函数返回；
- `LDR/STR` 访问全局变量与栈；
- `ADRP/ADD` 构造数据地址；
- `SVC` 触发系统调用；
- `ERET` 将控制权返回到较低异常级别。

从这一刻开始，ELF Loader 的初始加载职责就基本完成了，但动态加载、按需缺页、延迟符号绑定等运行时机制仍可能继续发生。

# Part II：深入 OP-TEE AArch64 的真实 ELF Loading 源码

## 九、OP-TEE 中实际上存在两条不同的加载路径

理解 OP-TEE 的 ELF 加载，第一件事就是区分：

OP-TEE Core 本身的启动 与 OP-TEE 用户态 TA 的加载。

二者虽然都涉及 ELF，但完全不是同一类问题。

| 对比            | OP-TEE Core                      | User TA                            |
| ------------- | -------------------------------- | ---------------------------------- |
| 典型构建产物        | `tee.elf`、`tee.bin` 等            | ELF TA，经打包或签名形成 `.ta`              |
| 加载者           | 先前的 Boot Firmware / 平台启动机制       | OP-TEE Core + `ldelf`              |
| 运行级别          | 典型配置为 S-EL1                      | S-EL0                              |
| 运行方式          | 平台固件启动，Core 自身完成初始化              | 加载 ELF、重定位、创建用户上下文                 |
| 入口            | `_start`                         | TA ELF Entry / TA Runtime Entry    |
| Linker Script | `core/arch/arm/kernel/kern.ld.S` | `ta/arch/arm/ta_arm64.lds` 等构建相关文件 |

实际工程需要根据配置区分是否使用 `CFG_CORE_SEL2_SPMC`、分页、ASLR、启动打包格式等。

### 9.1 OP-TEE Core 的启动路径

简化如下：

```
Boot ROM
    |
    v
BL2 / Platform Boot Firmware
    |
    | 认证、搬运、准备 OP-TEE 映像
    v
BL31 / Secure Monitor（典型 TF-A 方案）
    |
    | 切换到 Secure World
    v
OP-TEE Core
    |
    v
_start（entry_a64.S）
    |
    v
早期地址映射、栈、CPU 环境初始化
    |
    v
OP-TEE Kernel Runtime
```

这里要注意：

`tee.elf` 不一定会在启动时作为完整 ELF 文件被解析。

很多平台真正交给 Bootloader 的是 `tee.bin` 或经过封装的 OP-TEE 镜像。构建时利用 ELF 的 Section、Segment、Symbol 等信息生成映像，实际启动阶段可能是按照平台镜像格式完成装载。

所以不能简单地认为：

> OP-TEE Core 启动时一定有一个通用 ELF Loader 读取 `tee.elf`，遍历 Program Header，再跳转到入口。

这种理解在很多真实固件场景中并不成立。

### 9.2 User TA 的加载路径

这才是完整研究 ELF Loader 的最佳切入点。

OP-TEE User TA 的加载与执行链路

Normal World

阶段 1

CA → libteec → Linux OP-TEE Driver

World Transition

阶段 2

SMC / FF-A → OP-TEE Core

OP-TEE Kernel

阶段 3

创建 TA Context、加载 ldelf

S-EL0 / ldelf

阶段 4

解析 TA ELF、映射、重定位、创建 Stack

OP-TEE Kernel

阶段 5

接收入口地址与 Stack，完成初始化

S-EL0 / User TA

阶段 6

TA Runtime → TA_CreateEntryPoint / OpenSession / InvokeCommand

这里有一个很值得注意的架构设计：

OP-TEE Core 并没有把所有 ELF 解析、动态链接逻辑都放在自己的 Kernel 中，而是通过用户态组件 `ldelf` 执行大量 ELF 加载工作。

这降低了 Core 内核直接处理复杂 ELF 逻辑的需求，同时由 Core 提供受控的映射、存储访问和系统调用服务。

OP-TEE 官方文档也明确说明，User TA 的加载由 `ldelf` 完成，TA 可能来自不同存储后端，但通过统一接口进入加载流程。

[image](https://www.google.com/s2/favicons?domain=https://optee-docs.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



## 十、首先追踪 TA 初始化的真正入口

源码：

[core/kernel/user_ta.c](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/user_ta.c?utm_source=chatgpt.com)

在 `tee_ta_complete_user_ta_session()` 中，能看到下面的关键调用关系：

```
res = ldelf_load_ldelf(&utc->uctx);

if (!res)
    res = ldelf_init_with_ldelf(
        &s->ts_sess,
        &utc->uctx);
```

这两行代码非常重要。

它实际上包含了两次不同的加载行为：

第一步，`ldelf_load_ldelf()`：

将 ELF Loader 自己的可执行代码和数据放入用户态内存。

第二步，`ldelf_init_with_ldelf()`：

让 CPU 开始执行 `ldelf`，随后再由 `ldelf` 解析和加载真正的 TA ELF。

因此：

```
OP-TEE Core
    |
    | 先把 ELF Loader 本身加载好
    v
ldelf
    |
    | 再加载目标 TA
    v
User TA
```

源码中的这两个调用可以直接验证。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



### 10.1 为什么还要先加载 ldelf？

因为 `ldelf` 本身也是需要执行的代码。

假设 OP-TEE Core 已经存在于 S-EL1 的虚拟地址空间，而 `ldelf` 要运行在 S-EL0。

Core 至少需要：

1. 建立 `ldelf` 可执行代码映射。
2. 建立 `ldelf` 可写数据映射。
3. 分配 `ldelf` 的用户态栈。
4. 计算 `ldelf` 入口地址。
5. 准备 S-EL0 的 CPU 上下文。

然后 `ldelf` 才有能力加载其他 ELF 文件。

这里的关键并不是 `ldelf` 这个名称，而是引导加载器（bootstrap loader）与目标程序加载器之间的职责关系。

## 十一、深入 ldelf_load_ldelf()：Loader 自己如何被装入内存？

源码：

[core/kernel/ldelf_loader.c](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/ldelf_loader.c?utm_source=chatgpt.com)

入口函数是：

```
TEE_Result ldelf_load_ldelf(
    struct user_mode_ctx *uctx);
```

我们关注五个关键动作。

### 11.1 建立 ldelf 的内存布局

源码中会为以下内容分配用户虚拟地址空间：

```
ldelf User Address Space

+------------------------+
| ldelf Executable Code  |  URX
+------------------------+
| ldelf RW Data / BSS    |  URW
+------------------------+
| ldelf Stack            |  URW
+------------------------+
| Temporary Buffers      |  按用途设置权限
+------------------------+
```

其中：

- `URX` 表示用户可读、可执行。
- `URW` 表示用户可读、可写。
- `PRW` 表示特权级读写权限。

OP-TEE 实际采用的内存属性是 `TEE_MATTR_*`。

这里还有一个特别有价值的实现细节：Core 不是通过通用 ELF 解析流程再次加载完整的 ldelf ELF 文件，而是使用构建时嵌入的 ldelf 代码和数据。

### 11.2 ldelf 的代码是如何复制进去的？

实际源码中存在：

```
memcpy((void *)code_addr,
       ldelf_data,
       ldelf_code_size);
```

另外再将对应的数据区域复制到可写映射。

随后调用 `vm_set_prot()`，把代码区域设置为用户可执行属性。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



为什么先用写权限映射，再修改为执行权限？

因为 Core 需要先把代码写到对应的物理内存中，然后才能让用户态执行。

这也是 W^X（Writable XOR Executable）安全设计的典型思路：避免不必要的同时可写、可执行映射。

### 11.3 ldelf 的入口地址如何计算？

源码中：

```
uctx->entry_func = code_addr + ldelf_entry;
```

这里：

```
code_addr:
    ldelf 被实际映射到的虚拟基址

ldelf_entry:
    构建时确定的入口偏移

entry_func:
    真正运行时的入口虚拟地址
```

假设：

```
code_addr   = 0x12000000
ldelf_entry = 0x00000400
```

则：

```
entry_func = 0x12000400
```

你会发现，这和前面讨论的 ELF Load Bias 完全一致。

但这里使用的 `ldelf_entry` 是构建时生成并嵌入 Core 的入口信息，而不是运行时通过重新解析原始 ELF Header 获取的。

### 11.4 如何开始执行 ldelf？

`ldelf_init_with_ldelf()` 中有这样一个关键调用：

```
thread_enter_user_mode(
    (vaddr_t)arg, 0, 0, 0,
    usr_stack, uctx->entry_func,
    is_32bit, &panicked, &panic_code);
```

其核心含义：

| 参数                 | 作用                    |
| ------------------ | --------------------- |
| 第一个参数              | 传入 `ldelf_arg` 的用户态地址 |
| 后续三个参数             | 此处为零                  |
| `usr_stack`        | ldelf 用户态 Stack       |
| `uctx->entry_func` | ldelf 代码入口            |
| `is_32bit`         | 选择执行架构状态              |

从 AArch64 调用约定看，进入用户态时，第一个入口参数会成为 `x0`。

也就是：

```
x0 = struct ldelf_arg *
PC = ldelf entry
SP = ldelf user stack
```

这就是 Core 向 Loader 传递任务描述信息的方法。

至此，OP-TEE Core 已经完成了对 ELF Loader 的初始装载工作。

# 十二、真正进入 AArch64 汇编：ldelf 如何完成自己的重定位？

这是整条加载链中最值得详细研究的汇编文件。

[ldelf/start_a64.S](https://github.com/OP-TEE/optee_os/blob/master/ldelf/start_a64.S?utm_source=chatgpt.com)

入口符号是：

```
_ldelf_start:
```

它做的第一件重要工作，并不是立即开始解析 TA ELF。

而是先完成 ldelf 自己的 Relocation。

为什么？

因为 ldelf 代码虽然已经被 Core 复制到用户态虚拟地址空间，但 ldelf 内部某些地址引用仍然需要适配实际加载基址。

### 12.1 ldelf 的自重定位算法

把源码整理成下面的等价算法：

```
void relocate_ldelf(uint64_t load_bias,
                    Elf64_Rela *begin,
                    Elf64_Rela *end)
{
    for (Elf64_Rela *r = begin; r < end; r++) {
        uint32_t type = ELF64_R_TYPE(r->r_info);

        if (type != R_AARCH64_RELATIVE)
            panic();

        uint64_t *where =
            (uint64_t *)(load_bias + r->r_offset);

        *where = load_bias + r->r_addend;
    }
}
```

这不是源码原样复制，而是对其核心汇编算法的 C 语言还原。

注意，目前这个自重定位逻辑只支持 `R_AARCH64_RELATIVE`。

### 12.2 汇编指令逐步解析

先看：

```
adr x4, reloc_begin_rel
```

`ADR` 用来构造相对于当前 PC 的地址。

其主要意义是：在 ldelf 自己尚未完成重定位时，仍然能够找到自己的重定位元数据。

为什么不用一个尚待修正的全局指针？

因为使用这个全局指针可能本身就需要先重定位，形成循环依赖。

这正是 Bootstrap Relocation 的设计关键。

随后源码读取重定位区域的起止偏移，转换为当前运行地址。

接着再使用：

```
adr x4, _ldelf_start
```

取得自身入口符号的实际运行地址，在这个映像的链接布局下，它被用作加载偏移。

假设：

```
ldelf linked base = 0
ldelf runtime base = 0x12000000
```

那么：

```
x4 = 0x12000000
```

### 12.3 读取 Elf64_Rela

对每个 `Elf64_Rela`，汇编依次读取：

```
r_offset  : 8 bytes
r_info    : 8 bytes
r_addend  : 8 bytes
```

核心指令是：

```
ldp x7, x8, [x5], #16
ldr x9, [x5], #8
```

解释：

`LDP` 是 Load Pair，一次读取两个 64 位值。

执行后：

```
x7 = r_offset
x8 = r_info
x5 = x5 + 16
```

随后 `LDR` 读取 Addend：

```
x9 = r_addend
x5 = x5 + 8
```

因此，一轮读取结束后：

```
Relocation Pointer += 24
```

这恰好对应一个 `Elf64_Rela` 的大小。

### 12.4 提取 Relocation Type

源码通过掩码从 `r_info` 中提取低 32 位 Relocation Type，然后与 `R_AARCH64_RELATIVE` 比较。

如果不是支持的类型，就进入错误处理路径。

为什么这里只支持相对重定位？

因为 ldelf 的构建方式限制了早期自举阶段所需的重定位类型。它需要在尚无完整动态链接服务时，通过非常简单、确定的逻辑完成初始化。

这也说明：

任何需要自重定位的 Firmware / Runtime，最好尽量减少 Bootstrap 阶段的重定位依赖。

### 12.5 真正执行重定位

核心逻辑对应以下三条汇编操作：

```
add x7, x7, x4
add x9, x9, x4
str x9, [x7]
```

假设：

```
x4 = 0x12000000     Load Bias
x7 = 0x00003000     r_offset
x9 = 0x00002000     r_addend
```

执行结果：

```
x7 = 0x12003000
x9 = 0x12002000

[0x12003000] = 0x12002000
```

AArch64 RELATIVE Relocation 的寄存器变化

修正前 x7

## 0x3000

r_offset

修正前 x9

## 0x2000

r_addend

分别加上 Load Bias 0x12000000

最终写入地址 x7

## 0x12003000

最终写入值 x9

## 0x12002000

`STR` 将 x9 中的指针值写入 x7 指向的内存

这就是 Load-time Relocation 的底层本质。

并不存在什么特殊的硬件“重定位模式”。

它就是普通 CPU 指令对内存进行读写，把原来的地址引用修正为可以在新位置使用的地址。

源码中的循环完成后，通过 `BL` 调用 C 函数 `ldelf()`，开始真正的 TA 加载流程。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



# 十三、ldelf 如何加载真正的 TA ELF？

现在终于进入目标 TA 的 ELF 加载。

源码：

[ldelf/main.c](https://github.com/OP-TEE/optee_os/blob/master/ldelf/main.c?utm_source=chatgpt.com)

主函数是：

```
void ldelf(struct ldelf_arg *arg);
```

在这一层，核心执行顺序是：

```
ldelf()
    |
    +-- 初始化 Loader 自己的 Heap
    |
    +-- ta_elf_load_main()
    |      |
    |      +-- 读取 TA ELF
    |      +-- 解析 ELF Header
    |      +-- 解析 Program Header
    |      +-- 建立 Segment 映射
    |      +-- 加载 Data / BSS
    |      +-- 解析依赖
    |      +-- 建立 TA Stack
    |
    +-- ta_elf_load_dependency()
    |      |
    |      +-- 加载依赖的共享 ELF
    |
    +-- ta_elf_relocate()
    |      |
    |      +-- 修正内部与外部符号引用
    |
    +-- ta_elf_finalize_mappings()
    |
    +-- ta_elf_finalize_load_main()
    |      |
    |      +-- 确定最终 Entry
    |      +-- 返回 Load Address
    |
    +-- sys_return_cleanup()
           |
           v
        OP-TEE Core
```

这条调用路径可以直接在 `ldelf/main.c` 的主函数中验证。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



## 13.1 解析 ELF Header

具体代码在：

[ldelf/ta_elf.c](https://github.com/OP-TEE/optee_os/blob/master/ldelf/ta_elf.c?utm_source=chatgpt.com)

AArch64 路径中使用 `e64_parse_ehdr()`。

它检查的核心条件包括：

```
EI_CLASS  == ELFCLASS64
EI_DATA   == ELFDATA2LSB
e_type    == ET_DYN
e_machine == EM_AARCH64
```

这里有一个重要信息：

当前代码要求 AArch64 TA ELF 使用 ET_DYN 格式。

这并不意味着 TA 是普通 Linux `.so`，而是说明它采用适合重定位加载的 ELF 类型。

代码还会保存：

```
e_entry
e_phoff
e_shoff
e_phnum
e_shnum
e_phentsize
e_shentsize
```

这些信息后续都会用到。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub Raw+1



## 13.2 解析 Program Header

函数：

```
parse_load_segments(elf);
```

核心逻辑是扫描 `Elf64_Phdr`。

对于每个 `PT_LOAD`，把以下属性记录下来：

```
p_offset
p_vaddr
p_filesz
p_memsz
p_flags
p_align
```

而对于 `PT_TLS`，会记录 TLS 初始化模板与大小。

对于符合条件的 `PT_GNU_PROPERTY`，还可能提取 AArch64 BTI 相关信息。

这里就把前面讲过的 ELF Program Header 真正串联到 OP-TEE 代码中了。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub Raw+1



## 13.3 实际建立 Segment 映射

核心函数：

```
populate_segments(elf);
```

该函数有一个很值得关注的设计：

对于可写 Segment 和不可写 Segment，OP-TEE 使用不同的加载方法。

| Segment 类型  | 典型加载操作                                    | 原因                             |
| ----------- | ----------------------------------------- | ------------------------------ |
| 可写 Segment  | `sys_map_zi()` + `sys_copy_from_ta_bin()` | 需要私有可写内存                       |
| 不可写 Segment | `sys_map_ta_bin()`                        | 可以使用受控、可共享的只读映射                |
| 需要额外零填充的区域  | 零初始化映射                                    | 满足 `p_memsz` 超过 `p_filesz` 的需求 |

真实逻辑还包含对齐、重映射、共享文件页和部分页面重叠等处理，不能简单理解为每个 Segment 都独立执行一次 `memcpy()`。

### 13.4 为什么这个实现很有价值？

假设有两个 TA 都需要同一份只读代码或共享库。

如果每次都：

```
malloc();
memcpy();
```

就可能重复占用安全内存。

如果底层支持安全地共享只读映射，则不同虚拟地址可以引用同一组底层只读页面。

但是，`.data`、`.bss` 等可写状态必须满足各 TA 实例的隔离需求。

因此：

```
Read-only ELF Segment:
    允许在满足安全策略时共享

Writable Segment:
    需要适当的私有可写映射
```

同时，代码会依据 ELF Segment Flags 推导执行权限，并拒绝不满足其格式要求的 Segment。

这也是 ELF Loader 和 Secure Memory Management 之间的实际结合点。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub Raw+1



# 十四、TA ELF 的动态重定位是如何完成的？

源码：

[ldelf/ta_elf_rel.c](https://github.com/OP-TEE/optee_os/blob/master/ldelf/ta_elf_rel.c?utm_source=chatgpt.com)

OP-TEE 的 TA 重定位并不只有 `R_AARCH64_RELATIVE`。

还包含：

- `R_AARCH64_ABS64`
- `R_AARCH64_GLOB_DAT`
- `R_AARCH64_JUMP_SLOT`
- TLS 相关重定位

这些都可以在 `e64_relocate()` 中找到。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub Raw+1



### 14.1 先找到需要修改的位置

在重定位过程中，代码首先根据：

```
elf->load_addr
rela->r_offset
```

计算需要修正的地址。

等价于：

```
uint64_t *where =
    (uint64_t *)(elf->load_addr + rela->r_offset);
```

随后根据 Relocation Type 选择不同处理方法。

### 14.2 R_AARCH64_RELATIVE

这部分与刚才 ldelf 自重定位的算法基本一致：

```
*where = rela->r_addend + elf->load_addr;
```

这里的 `elf->load_addr` 是目标 TA 或当前共享 ELF 的加载偏移。

注意，不再是 ldelf 自己的加载地址。

因此一个进程空间中如果有：

```
ldelf:
    load_addr = 0x12000000

Main TA:
    load_addr = 0x40000000

Shared Library:
    load_addr = 0x50000000
```

它们必须分别使用自己的 Load Bias 进行相对重定位。

### 14.3 R_AARCH64_GLOB_DAT 和 GOT

接下来讨论 AArch64 另一个很重要的地址访问模式。

假设一个共享对象引用外部变量：

```
extern int global_value;

int read_value(void)
{
    return global_value;
}
```

编译器可能生成使用 GOT 的指令序列。

示意：

```
adrp x0, :got:global_value
ldr  x0, [x0, :got_lo12:global_value]
ldr  w1, [x0]
```

解释：

第一步：

```
adrp x0, :got:global_value
```

构造 GOT Entry 所在页面的地址。

第二步：

```
ldr x0, [x0, :got_lo12:global_value]
```

从 GOT Entry 读取一个指针。

第三步：

```
ldr w1, [x0]
```

从这个指针指向的实际变量地址读取变量值。

关键在于 GOT Entry 本身。

假设：

```
GOT Entry VA = 0x40008000
global_value = 0x50002000
```

动态链接器需要把：

```
[0x40008000] = 0x50002000
```

这样代码才能通过 GOT 找到外部变量。

所以：

GOT 不是一个硬件机制，它是 Linker / Loader 协作维护的一张地址表。

### 14.4 PLT 与函数调用

对于跨共享对象的函数调用，也可能经过 PLT 和 GOT：

```
Caller
   |
   v
PLT Stub
   |
   v
GOT Entry
   |
   v
Resolved Function Address
   |
   v
Target Function
```

在这种路径中，`R_AARCH64_JUMP_SLOT` 可以用于修正函数调用目标地址。

但要注意，并不是所有共享函数调用都必须经过 PLT；链接器优化、符号可见性和编译选项都会影响最终指令序列。

同样，也不能假定 OP-TEE 与 Linux/glibc 一样实现所有形式的 Lazy Binding。

### 14.5 OP-TEE 为什么还需要 Section Header？

前面提到，通用 ELF Loader 主要依据 Program Header 加载 Segment。

但是 OP-TEE 的 `ta_elf_relocate()` 还会扫描 Section Header，查找 `SHT_RELA` 等重定位 Section。

这不矛盾。

因为：

- Segment 决定代码和数据如何进入内存。
- Section Header 在 OP-TEE 的这个实现中还用于查找重定位元数据。

Linux 常见动态链接器更倾向于通过 `PT_DYNAMIC` 所提供的 `DT_RELA`、`DT_RELASZ` 等动态信息找到重定位表。

所以不能认为所有 ELF Loader 都必须在运行时依赖 Section Header，也不能认为所有 Loader 都完全不使用它。

这是具体实现选择，而不是 ELF 格式本身的硬性要求。

# 十五、真正执行 User TA：AArch64 如何从 S-EL1 进入 S-EL0？

现在假设：

```
TA ELF 已经加载完成
Relocation 已完成
Stack 已创建
Entry Address 已确定
```

接下来，OP-TEE Core 需要真正执行 TA。

这时会再次经过 `thread_enter_user_mode()`，但这一次入口是 TA 本身，而不是 ldelf。

## 15.1 最终入口地址来自哪里？

看：

```
ta_elf_finalize_load_main(...)
```

对于非 Legacy TA，核心计算关系是：

```
entry = elf->e_entry + elf->load_addr;
```

随后 `ldelf` 把这个地址返回给 Core。

Core 将它保存为：

```
uctx->entry_func
```

此外还保存 Stack Pointer、Load Address 等信息。

源码中还存在 Legacy TA 的兼容分支，它可能使用旧的 `ta_head` 入口字段。这里我们以现代非 Legacy ELF TA 为主。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub Raw+1



## 15.2 真正执行 TA 的调用点

在 `core/kernel/user_ta.c` 中，用户态 TA 的执行函数最终会通过：

```
thread_enter_user_mode(...);
```

使用 TA 的入口和栈。

这一步可能对应不同 TA 操作：

```
TA Create
TA OpenSession
TA InvokeCommand
TA CloseSession
TA Destroy
```

具体调用会通过传入的功能标识及参数由 TA Runtime 分发。

也就是说，加载完成后，TA 入口并非只会执行一次。后续 OpenSession、InvokeCommand 等请求依然可能通过同一个 TA Runtime 入口进入用户态。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 15.3 AArch64 汇编中的真正执行路径

源码：

[core/arch/arm/kernel/thread_a64.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/arm/kernel/thread_a64.S?utm_source=chatgpt.com)

重点函数：

```
__thread_enter_user_mode:
```

在这个函数中，首先保存 Kernel 的一些执行上下文。

然后，为进入用户态准备寄存器。

其中的三条关键操作是：

```
msr sp_el0, x1
msr elr_el1, x2
msr spsr_el1, x3
```

注意，这里的 `x1`、`x2`、`x3` 是汇编在读取线程寄存器上下文结构之后得到的值，而不是上一层 C 函数原始参数的直接对应关系。

其意义分别是：

```
SP_EL0   = User Stack Pointer
ELR_EL1  = User Entry Address
SPSR_EL1 = User PSTATE
```

随后恢复其他用户寄存器，并进入 `eret_to_el0` 路径。

真正的 `ERET` 会根据准备好的寄存器恢复 EL0 执行状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub Raw+1



这才是从内核进入用户态执行 ELF 的关键架构步骤。

### 15.4 TA 通过 SVC 重新进入 Core

反方向也要理解。

当 TA 执行系统调用：

```
svc #0
```

CPU 从 EL0 进入 EL1 异常处理路径。

此时硬件保存相应的异常返回状态，OP-TEE 异常处理代码再保存通用寄存器、恢复内核执行上下文，并调用系统调用处理函数。

在 `thread_a64.S` 中可以追踪到：

```
el0_svc
    |
    v
thread_scall_handler
```

处理完成后，再通过异常返回恢复 TA 的执行。

因此，TA 的 ELF 执行不只是一次进入用户态，而是完整的运行时交互循环：

```
OP-TEE Core
    |
    | ERET
    v
User TA
    |
    | SVC
    v
OP-TEE Core Syscall Handler
    |
    | ERET
    v
User TA
```

当 TA 操作结束时，会通过相应的返回服务把结果交回 Core；不能把用户态函数结束简单理解为直接 `RET` 回 S-EL1 的 C 调用栈。

这部分是理解 OP-TEE TA Runtime、System Call、EL Transition 的关键连接点。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub Raw



# 十六、回到 OP-TEE Core：为什么启动时不一定需要 ELF Loader？

前面我们专门学习了 User TA 的 ELF 加载。

现在再看另一条路径，可以进一步理解 ELF、Linker 和 Firmware Boot 的职责区别。

源码：

[core/arch/arm/kernel/kern.ld.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/arm/kernel/kern.ld.S?utm_source=chatgpt.com)

Linker Script 中有：

```
ENTRY(_start)
```

以及：

```
. = TEE_LOAD_ADDR;
```

这意味着 Linker 使用指定的布局基准组织 OP-TEE Core 的地址空间，并将 `_start` 标记为 ELF Entry。

但是，真正启动时还必须满足平台的加载地址契约。

如果 Bootloader 使用原始二进制镜像，那么它需要知道把镜像放在哪里，以及应当从哪里开始执行。

Linker Script、镜像打包规则、Bootloader 三者必须保持一致。

### 16.1 OP-TEE Core 的 \_start

源码：

[core/arch/arm/kernel/entry_a64.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/arm/kernel/entry_a64.S?utm_source=chatgpt.com)

在早期入口中，首先会保存启动参数寄存器。

随后使用：

```
adr x0, reset_vect_table
msr vbar_el1, x0
isb
```

来设置异常向量。

`ADR` 能够获得当前 PC 附近符号的运行地址，因此非常适合早期阶段使用。

随后还有早期内存布局调整、BSS 清零、栈初始化、MMU 初始化等操作。

### 16.2 BSS 为什么要在汇编中显式清零？

看真实源码的 BSS 初始化逻辑：

```
clear_bss:
    str xzr, [x0], #8
    cmp x0, x1
    b.lt clear_bss
```

假设：

```
x0 = __bss_start
x1 = __bss_end
```

每一轮：

```
str xzr, [x0], #8
```

都会：

1. 将零寄存器 `xzr` 的值写到 `x0` 指向的内存。
2. 写入 8 字节。
3. 将 `x0` 增加 8。
4. 重复，直到完成清零。

这就是裸机启动代码常见的 BSS 初始化过程。OP-TEE Core 的实际实现也正是如此。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



为什么用户 TA 不需要自己执行相同的汇编？

因为 TA 的 BSS 零初始化职责已由加载映射机制承担。

因此：

```
OP-TEE Core:
    Early Boot Assembly 清理 BSS

User TA:
    Loader 建立零初始化的数据区
```

两者最后都让运行时看到正确的零初始化变量，但实现时机不同。

这也是为什么学习 ELF Loader 时，不能忽略启动汇编和 C Runtime 的原因。

# Part III：通过动手实验把理论和机器码连接起来

## 十七、实验：亲眼观察 AArch64 Relocation

可以在 WSL2 / Ubuntu 中使用 AArch64 交叉编译器完成实验，不需要真正运行在 Arm 机器上。

### 17.1 准备测试文件

```
/* demo.c */

int global_value = 123;

int *global_ptr = &global_value;

int get_value(void)
{
    return *global_ptr;
}

void _start(void)
{
    volatile int value = get_value();

    (void)value;

    while (1)
        __asm__ volatile("nop");
}
```

注意我们故意定义了：

```
int *global_ptr = &global_value;
```

目的是制造一个初始化指针，从而观察动态重定位。

### 17.2 编译为 AArch64 PIE ELF

```
aarch64-linux-gnu-gcc \
    -nostdlib \
    -fpie -pie \
    -Wl,-e,_start \
    -Wl,--no-dynamic-linker \
    -o demo.elf demo.c
```

编译成功后依次执行：

```
aarch64-linux-gnu-readelf -h demo.elf
aarch64-linux-gnu-readelf -l demo.elf
aarch64-linux-gnu-readelf -S demo.elf
aarch64-linux-gnu-readelf -Wr demo.elf
aarch64-linux-gnu-objdump -dr demo.elf
```

这组命令能够分别回答：

| 命令            | 需要观察的内容                      |
| ------------- | ---------------------------- |
| `readelf -h`  | ELF Type、Machine、Entry Point |
| `readelf -l`  | PT_LOAD、FileSiz、MemSiz、Flags |
| `readelf -S`  | .text、.data、.bss、.got 等      |
| `readelf -Wr` | `R_AARCH64_RELATIVE` 等重定位    |
| `objdump -dr` | AArch64 访问全局变量的指令序列          |

建议重点寻找：

```
R_AARCH64_RELATIVE
```

观察其 `Offset` 和 `Addend`。

然后假设：

```
Load Bias = 0x50000000
```

手动计算：

```
Relocation Target = Load Bias + Offset
Relocation Value  = Load Bias + Addend
```

最后在反汇编中寻找：

```
adrp
add
ldr
bl
```

理解代码究竟如何访问全局指针。

具体指令序列可能随编译器、优化选项和链接器版本不同而变化。

## 十八、OP-TEE 源码阅读与调试顺序

我建议严格按照下面的顺序阅读，而不是直接从 `ldelf/ta_elf.c` 的第一行开始看。

推荐源码追踪顺序

1. `core/kernel/user_ta.c`

   理解为什么要进入 ldelf、TA Context 在哪里建立。
2. `core/kernel/ldelf_loader.c`

   理解 ldelf 的 Code / Data / Stack 如何分配并映射。
3. `ldelf/start_a64.S`

   逐条学习 AArch64 自重定位汇编，重点 `ADR`、`LDP`、`STR`。
4. `ldelf/main.c`

   理解 Loader 的总体执行顺序。
5. `ldelf/ta_elf.c`

   分析 ELF Header、Program Header、Segment Loading、Stack。
6. `ldelf/ta_elf_rel.c`

   理解 RELATIVE、ABS64、GOT、外部符号和 TLS。
7. `core/arch/arm/kernel/thread_a64.S`

   追踪 `ELR_EL1`、`SPSR_EL1`、`SP_EL0`、`ERET`、`SVC`。
8. `core/arch/arm/kernel/entry_a64.S` 与 `kern.ld.S`

   对比 OP-TEE Core 自身的链接布局和早期启动过程。

实际调试时，还建议把一个真实 TA 的以下文件放在一起比较：

```
<uuid>.elf
<uuid>.stripped.elf
<uuid>.ta
```

这些产物是否全部存在取决于构建配置。

注意：签名或加密封装后的 `.ta` 通常不是可以直接交给 `readelf` 解析的纯 ELF 文件。应该优先分析构建阶段保留的 ELF 产物。

此外，调试加载地址随机化时，不能仅使用静态的 `e_entry` 判断程序运行时 PC。必须同时考虑实际 Load Bias。

# Part IV：你需要真正掌握的几个核心结论

## 十九、几个常见误区

| 容易产生的理解                             | 更准确的理解                                           |
| ----------------------------------- | ------------------------------------------------ |
| Loader 逐个加载 ELF Section             | Loader 通常基于 PT_LOAD 建立内存映射                       |
| Linker 已经算出地址，Loader 不需要 Relocation | 如果运行时地址改变，部分地址引用仍需要修正                            |
| `e_entry` 就是最终 PC                   | ET_DYN 一般需要结合 Load Bias                          |
| `e_entry` 就是 `main()`               | 通常先进入 Runtime Startup                            |
| PIC 表示完全不需要重定位                      | PIC 主要减少位置相关代码，不排除数据和动态符号重定位                     |
| `.bss` 占据完整的文件空间                    | BSS 一般通过内存零填充实现                                  |
| ELF Loader 必须复制所有 Segment           | 可以使用文件映射、共享只读页、按需分页等机制                           |
| CPU 根据 ELF Header 启动程序              | CPU 仅根据已经建立的执行上下文和 PC 取指                         |
| OP-TEE Core 和 TA 使用同一种 Loader       | Core Firmware Boot 与 User TA ELF Loading 是两套不同机制 |

## 二十、Staff Engineer 级别思考题

以下四道问题值得结合 OP-TEE 源码真正研究。

问题 1：为什么 ldelf 可以在完成自重定位之前执行汇编代码？

重点回答：

- PC-relative Addressing。
- Bootstrap Relocation。
- 如何避免自举阶段依赖尚未修正的绝对地址。
- 为什么只支持 `R_AARCH64_RELATIVE` 可以简化问题。
- 如何验证早期代码不会通过 GOT 访问未初始化的数据指针。

问题 2：假设 TA 的代码段正确映射了，但执行到访问全局变量时发生 Translation Fault，该如何排查？

重点考虑：

- `ADRP` 的计算结果是否正确。
- GOT Entry 是否完成修正。
- Load Bias 是否错误。
- Relocation Table 是否完整。
- `TTBR0_EL1` 和页表映射。
- `FAR_EL1`、`ESR_EL1`、`ELR_EL1`。
- 是缺页问题、权限问题，还是错误指针。

问题 3：如何设计一个安全的 RISC-V OP-TEE ELF Loader？

结合目前的学习重点，从以下方面展开：

- `ET_DYN`、`EM_RISCV` 的 ELF 验证。
- `R_RISCV_RELATIVE` 等 Relocation。
- `PT_LOAD` 映射。
- U-mode 地址空间。
- 页表与 Secure Domain 的内存隔离。
- ASLR、W^X、ELF 数据验证。
- I-Cache 同步以及 `FENCE.I` 相关要求。
- 通过 `sret` 进入目标用户态 Entry。

问题 4：为什么 OP-TEE 选择把复杂 ELF 加载逻辑放进 ldelf，而不是全部集成到 Core Kernel？

可以从以下角度分析：

- TCB 大小和攻击面。
- ELF Parser 的复杂度。
- User Mode Isolation。
- 必须由内核提供的安全能力。
- 恶意 ELF 文件的边界检查。
- 内存管理与动态链接职责划分。
- Loader 和 Core 之间的信任边界。

这道题已经超越单纯的 ELF 解析，进入 Security Architecture 设计层面。

## 二十一、总结：把整个 ELF Loading 过程彻底串起来

最终可以用下面这条完整路径来理解 OP-TEE AArch64 User TA：

```
Compile / Assemble
    |
    v
Linker
    |
    +-- Resolve Symbols
    +-- Apply Link-time Relocations
    +-- Generate PT_LOAD
    +-- Generate Runtime Relocations
    +-- Define ELF Entry
    |
    v
TA ELF → TA Package
    |
    v
OP-TEE Core
    |
    +-- Create TA Context
    +-- Load ldelf Code / Data
    +-- Allocate ldelf Stack
    |
    v
ERET → ldelf at S-EL0
    |
    +-- Bootstrap Relocation
    +-- Parse TA ELF
    +-- Map PT_LOAD Segments
    +-- Zero-fill BSS
    +-- Load Dependencies
    +-- Apply AArch64 Relocations
    +-- Allocate TA Stack
    +-- Resolve TA Entry
    |
    v
Return to OP-TEE Core
    |
    v
ERET → User TA at S-EL0
    |
    +-- TA Runtime
    +-- TA_CreateEntryPoint()
    +-- TA_OpenSessionEntryPoint()
    +-- TA_InvokeCommandEntryPoint()
    |
    v
SVC → OP-TEE Core
```

如果只记住一个核心思想，我希望是：

ELF 是一份关于程序代码、数据、地址布局和修正规则的描述；Linker 负责建立这些规则，Loader 负责让它们在运行内存中成立，而 CPU 最终只执行已经准备好的机器指令。

结合 OP-TEE 来看，最值得反复研究的三个位置是 `ldelf/start_a64.S` 的 Bootstrap Relocation、`ldelf/ta_elf_rel.c` 的 TA 动态重定位，以及 `thread_a64.S` 的 `ERET` 执行切换。

这三个位置分别回答了：地址如何修正、程序如何完成加载、CPU 如何真正开始执行。把它们连接起来，ELF Loading 就不再是一组抽象概念，而是一条可以从 C 代码追踪到 AArch64 汇编指令的完整执行链。
