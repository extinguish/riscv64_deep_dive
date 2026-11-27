# **Chapter 05 — OP-TEE Binary Layout、Linker 与 OS Image 深度解析**

OP-TEE OS Internals · Part I · Advanced / Staff Engineer

本章源码基线：**OP-TEE 4.11.0**，以 Arm64 Core 为主要分析对象，并在关键位置讨论 RISC-V 的实现差异。

## **本章的核心目标**

这一章要解决一个非常根本的问题：

**当 OP-TEE 的 C/Assembly 代码编译完成后，究竟发生了什么，才能让它成为一个能在 Secure World 中执行、使用 MMU、支持动态重定位和分页的安全操作系统？**

在研究内核时，我们容易把精力集中在运行期，例如 Thread、MMU、TA Loader。但实际上，很多运行时行为早在编译和链接阶段就已经被决定了。

例如：

- 为什么某些函数可以在 MMU 尚未开启时执行？
- 为什么 Pager 的代码不能被 Pager 自己换出？
- 为什么 `.bss` 不需要占用同等大小的镜像文件？
- 为什么修改 Linker Script 可能导致 Boot 正常、但运行几分钟后才出现 Page Fault？
- 为什么 `tee.elf` 可以用于 GDB 调试，却不能简单地把整个 ELF 文件当作 `bl32.bin` 使用？
- 为什么镜像加载成功不等于 Secure Boot 验证成功？
- 为什么 OP-TEE 的 Core ASLR 不等价于让 Bootloader 随便选择加载地址？

这些问题最终都与五个概念密切相关：

ELF

描述代码、数据、符号和加载信息

Linker Script

确定各部分地址和布局

Binary Image

把 ELF 转换成 Loader 使用的负载

Boot Contract

定义镜像如何被放置和启动

Runtime Mapping

决定内存最终如何被访问

我们会重点分析这些机制的相互依赖，而不只是记忆它们的定义。

## **5.1 从源代码到 Secure Kernel：完整构建链**

先建立整体模型。

```
C / Assembly Source
        |
        | Compiler / Assembler
        v
Object Files (.o)
        |
        | Linker
        |   +-- Linker Script
        |   +-- Libraries
        |   +-- Section Placement
        |   +-- Symbol Resolution
        |   +-- Relocations
        v
      tee.elf
        |
        | scripts/gen_tee_bin.py
        |
        +---- tee.bin
        |
        +---- tee-header_v2.bin
        +---- tee-pager_v2.bin
        +---- tee-pageable_v2.bin
        |
        +---- tee-raw.bin
                 |
                 v
         Boot Firmware / Loader
                 |
                 | Load / Verify / Handoff
                 v
          OP-TEE Entry Point
                 |
                 | Early Runtime Init
                 | MMU / Pager Setup
                 v
          OP-TEE Kernel Running
```

这是上游 Arm 构建路径的概念性表示。真实构建还涉及中间链接、自动生成的 Linker Fragment 和版本相关的构建逻辑。

OP-TEE 4.11.0 的 `core/arch/arm/kernel/link.mk` 明确展示了从 `tee.elf` 生成多个 Binary 的依赖关系，而 `scripts/gen_tee_bin.py` 负责将 ELF 内容组织成对应镜像格式。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



### **为什么要保留&#x20;**`tee.elf`**，而不是直接生成一个&#x20;**`.bin`**？**

因为它们负责不同的事情。

`tee.elf` 是供链接器、构建工具、调试器和后处理工具使用的结构化文件。

它可以保留：

- Section Header。
- Program Header。
- Symbol Table。
- Relocation Information。
- Debug Information。
- Entry Point。
- Section 与 Segment 的地址信息。

而最终被 Firmware 加载的 Raw Binary 通常不需要包含上述全部内容。

例如一个最简单的 Bare-metal Binary 可能只有：

```
[ Machine Code ][ RO Data ][ Initialized Data ]
```

Bootloader 已经通过预先约定知道它应该放到哪个地址，因此并不需要操作系统式的 ELF Loader。

但在复杂的 OP-TEE 系统里，事情没有这么简单。

有些平台要求 OP-TEE 被加载到固定物理地址，有些平台使用分离式 Pager Image，还有些配置需要执行重定位。

因此，OP-TEE 同时提供多种镜像格式，用于满足不同的 Boot Contract。

## **5.2 ELF：最容易被误解的两个视角**

理解 ELF 时，最重要的是区分两个概念：

**Section 是 Linker 的组织视角，Segment 是 Loader 的加载视角。**

### **5.2.1 Section：描述程序内容的逻辑分类**

例如：

```
.text
.rodata
.data
.bss
.nozi
.got
.rela.dyn
.symtab
.debug_info
```

Linker 通过 Section 来组织代码和数据。

### **5.2.2 Segment：描述程序应该如何加载**

ELF 的 Program Header 可以包含一个或多个 `PT_LOAD` Segment。

每个 `PT_LOAD` 通常携带：

| 字段         | 含义                           |
| ---------- | ---------------------------- |
| `p_offset` | 在 ELF 文件中的数据偏移               |
| `p_vaddr`  | 预期的虚拟地址                      |
| `p_paddr`  | 物理地址字段，语义受目标 ABI/Loader 约定影响 |
| `p_filesz` | 文件中需要提供的字节数量                 |
| `p_memsz`  | 加载后的内存占用量                    |
| `p_flags`  | R/W/X 属性                     |
| `p_align`  | 加载对齐要求                       |

注意：`p_paddr` 并不是一个跨平台、自动生效的硬件加载命令。最终是否使用它，由 Loader 与平台 Boot ABI 决定。

### **一个具体例子**

假设我们得到以下 ELF Program Header：

```
Type     Offset    VirtAddr     PhysAddr
LOAD     0x001000  0x10000000   0x10000000
         FileSiz   0x00018000
         MemSiz    0x00020000
         Flags     RWE
         Align     0x1000
```

这是一个教学用的简化示例，并不是 OP-TEE 的实际构建输出。

如果 Loader 按该 Segment 装载，它需要：

1. 将文件中 `0x18000` 字节复制到目标地址。
2. 将 Segment 的剩余 `0x8000` 字节初始化为零。
3. 在启用 MMU 后，按照安全策略建立访问属性。

重点是：

\\[ \text{MemorySize} \geq \text{FileSize} \\]

在这个示例中：

\\[ 0x20000-0x18000=0x8000 \\]

这部分没有对应的文件内容，但运行时需要占用内存。

它可能包含 `.bss` 等零初始化数据。

### **5.2.3 为什么不能只看 Section Header 判断内存是否安全？**

因为 CPU 根本不会“执行一个 Section Header”。

CPU 执行的是指令，MMU 控制的是实际地址映射和页权限。

Section 名称可以表达 Linker 设计意图，但 **真正的 R/W/X 隔离由最终映射和硬件保护机制实施**。

一个函数处于 `.text`，并不自动证明它所在的物理页不可写。

如果 `.text` 和 `.data` 在页粒度上重叠，最终页权限就可能无法同时满足 RX 和 RW 的要求。

这也是 OP-TEE Linker Script 存在各种页对齐规则的重要原因之一。

## **5.3 VMA、LMA 与物理加载地址**

这一节非常重要。

不少嵌入式开发者熟悉 Linker Script，但在 MMU、ASLR 和安全 Firmware 同时存在时，往往会把三个地址概念混淆。

| 概念      | 主要意义                                | 决定者                      |
| ------- | ----------------------------------- | ------------------------ |
| VMA     | 程序运行时符号所在的地址空间位置                    | Linker + Runtime Mapping |
| LMA     | 内容加载或初始化所依据的地址，通常由 Linker Script 指定 | Linker / 镜像布局约定          |
| 实际加载 PA | Loader 真正把内容放入的物理内存地址               | Bootloader / Firmware    |

最核心的原则是：

**Linker 决定程序内部的地址关系；Firmware 决定镜像实际放在哪里；MMU 决定 CPU 后续如何访问它。**

这三件事可以一致，但绝不是天然相同。

### **5.3.1 一个经典的 VMA ≠ LMA 示例**

假设我们设计一个固件：

```
ROM: 0x08000000
RAM: 0x20000000
```

示例 Linker Script：

```
MEMORY {
    ROM (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS {
    .text : {
        *(.text*)
        *(.rodata*)
    } > ROM

    .data : {
        *(.data*)
    } > RAM AT> ROM

    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    } > RAM
}
```

这里 `.data` 的：

- VMA 位于 RAM。
- LMA 位于 ROM。
- 初始值存储在 ROM。
- Boot Code 必须把初始值复制到 RAM。

但要特别注意：

**这是用于解释 ELF 的通用固件示例，而不是 OP-TEE Core 当前 Linker Layout 的直接实现。**

OP-TEE Arm Core 的实际布局更特殊。其 Linker Script 内明确说明，某些单 Segment Binary 配置下，`.data` 的初始映像地址和运行地址处于同一段地址布局中。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此，不能把所有嵌入式 Firmware 的 ROM→RAM `.data` 搬移过程机械地套在 OP-TEE 上。

### **5.3.2 OP-TEE 为什么需要特别关注链接地址？**

假设在某个固定地址配置中，Linker 认为：

```
_start = 0x30000000
```

而 Firmware 实际把镜像装入：

```
Physical Address = 0x31000000
```

如果没有相应的重定位支持，程序内部某些绝对地址引用仍可能指向旧地址。

即便第一条指令能够执行，后续也可能出现：

- 全局变量访问错误。
- 函数指针错误。
- Exception Vector 地址不正确。
- MMU Translation Table 建立错误。
- Initcall 或 Driver Registration 引用错误。

注意，PC-relative 指令可能在整体移动后仍然正确，但并不意味着整个镜像天然位置无关。

真正需要分析的是生成代码中哪些引用依赖绝对地址、哪些依赖相对地址，以及运行时是否处理相应的 Relocation。

## **5.4 OP-TEE Linker Script 的内部设计**

现在进入本章的第一个源码重点。

核心文件：

[core/arch/arm/kernel/kern.ld.S — OP-TEE 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/kern.ld.S?utm_source=chatgpt.com)

这个文件不只是决定 Section 顺序，还承担了若干操作系统级职责：

1. 确定 Boot Entry 的位置。
2. 确定 Unpaged Kernel 的布局。
3. 分离初始化阶段与可分页代码。
4. 定义内存分配器和栈的预留空间。
5. 导出供 Boot、MMU 和 Pager 使用的边界符号。
6. 在链接阶段检查部分内存容量和地址约束。

### **5.4.1 从真正的 Linker Entry 开始**

上游源码中可以找到以下关键片段（省略了无关内容）：

```
OUTPUT_FORMAT(CFG_KERN_LINKER_FORMAT)
OUTPUT_ARCH(CFG_KERN_LINKER_ARCH)

ENTRY(_start)

SECTIONS
{
    . = TEE_LOAD_ADDR;

    ASSERT(!(TEE_LOAD_ADDR & (SMALL_PAGE_SIZE - 1)),
           "text start should be page aligned")

    __text_start = .;

    .text : {
        KEEP(*(.text._start))
        ...
    }
}
```

逐行分析。

`OUTPUT_FORMAT()`

决定最终 ELF 的目标格式，与所选架构、ABI 和工具链配置有关。

`OUTPUT_ARCH()`

定义目标机器架构，用于链接器检查和处理目标文件。

`ENTRY(_start)`

设置 ELF Entry Point 为 `_start` 的地址。

请特别注意：这条命令不等于“CPU Reset 后自动跳转 `_start`”。

CPU 是否进入 OP-TEE、入口时的寄存器状态以及执行权限，仍由上一级 Firmware/Bootloader 的 Handoff Contract 决定。

`. = TEE_LOAD_ADDR`

这里的 `.` 是 Linker Location Counter，表示当前输出地址。

它告诉 Linker：从 `TEE_LOAD_ADDR` 这个地址开始安排输出内容。

`ASSERT()`

在 Link Time 检查对齐要求。

这体现了一个很好的 Firmware Engineering 思想：

> 能在编译或链接阶段发现的错误，尽量不要留到运行阶段才发现。

`KEEP(*(.text._start))`

即使开启了 `--gc-sections`，也要求 Linker 保留入口代码。

这是为了避免 Section Garbage Collection 错误删除关键 Boot Code。

### **5.4.2 Linker Symbol 不是普通的 C 全局变量**

例如：

```
__text_start = .;
```

它的核心作用是产生一个地址符号。

C 或 Assembly 可以通过这个符号获得对应的地址边界。

示意代码：

```
extern char __text_start[];
extern char __text_end[];

size_t text_size =
    (uintptr_t)__text_end -
    (uintptr_t)__text_start;
```

这里的关键是：

- `__text_start` 表示地址。
- 它不一定对应一个实际存在的 C 对象。
- 访问 `__text_start[0]` 就是读取该地址的内容。
- 不能把它理解为某个普通的 `char *` 全局变量，需要先从变量中加载出指针值。

对于 Linker Symbol，建议始终区分：

**Symbol Address** 和 **Memory Content at Symbol Address**。

否则，阅读早期 MMU 初始化代码时很容易出现概念混淆。

### **5.4.3 OP-TEE 的几个关键地址边界**

以下符号可以在当前 Arm `kern.ld.S` 中找到。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



| Symbol                  | 作用               |
| ----------------------- | ---------------- |
| `__text_start`          | Core 初始链接地址边界    |
| `__text_end`            | 主 Text 区域结束      |
| `__rodata_start`        | 只读数据区域开始         |
| `__data_start`          | 已初始化可写数据开始       |
| `__data_end`            | 已初始化可写数据结束       |
| `__bss_start`           | 零初始化数据区域开始       |
| `__bss_end`             | 零初始化数据区域结束       |
| `__nozi_start`          | 不进行常规清零的预留区域开始   |
| `__init_start`          | 初始化用代码/数据区域开始    |
| `__init_end`            | 初始化区域结束          |
| `__init_size`           | 初始化区域大小          |
| `__pageable_part_start` | 独立 Pageable 部分开始 |
| `__pageable_part_end`   | Pageable 部分结束    |

注意，具体的符号、布局和含义会随 `CFG_*` 条件编译而变化。例如在 Pager 关闭时，某些边界有特殊处理。

不能仅凭符号名称推断所有配置都共享同一布局。

### **5.4.4&#x20;**`__flatmap_*`**&#x20;符号有什么意义？**

OP-TEE Linker Script 中还存在：

```
__flatmap_unpg_rx_start
__flatmap_unpg_rx_size

__flatmap_unpg_ro_start
__flatmap_unpg_ro_size

__flatmap_unpg_rw_start
__flatmap_unpg_rw_size

__flatmap_init_rx_start
__flatmap_init_rx_size
```

它们表达的是某些连续地址区间在 MMU Flat Mapping 阶段的布局及权限需求。

这比单纯使用 `.text`、`.data` 更接近操作系统实际需要的内存模型。

例如：

```
Region A:
  Start = __flatmap_unpg_rx_start
  Size  = __flatmap_unpg_rx_size
  Intended Permission = Read / Execute

Region B:
  Start = __flatmap_unpg_rw_start
  Size  = __flatmap_unpg_rw_size
  Intended Permission = Read / Write
```

但这些 Linker Symbol 不会直接修改页表。

真正的工作链是：

```
Linker Script
    |
    v
Region Boundary Symbols
    |
    v
Boot / Core MMU Initialization
    |
    v
Translation Table Entries
    |
    v
CPU MMU Permission Enforcement
```

这也是为什么修改 Linker Script 时，必须同时检查 MMU 初始化代码。

### **5.4.5 为什么 R/W/X Region 需要页对齐？**

假设一页为 4 KiB。

```
VA 0x10000000 ───────────────────
              .text
              ...
VA 0x10000F00 ───────────────────
              .rodata
              ...
VA 0x10001000 ───────────────────
```

如果 `.text` 与 `.rodata` 处在相同的物理页，则硬件不能在 4 KiB 页权限粒度上同时把同一页划分为两个独立权限区间。

如果再把 `.data` 混入同一页，问题更严重。

因此，配置了 `CFG_CORE_RODATA_NOEXEC`、`CFG_CORE_RWDATA_NOEXEC` 时，OP-TEE Linker Script 会在相关边界插入页对齐，从而使不同权限区域可以独立映射。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这说明：

**Linker Layout 是 Memory Protection 机制的一部分，不只是空间布局优化。**

## **5.5 OP-TEE 的核心 Section：运行期语义与生命周期**

这里要区分两类划分方式：

- **ELF Section 属性：** 可分配、可执行、可写、是否包含文件数据。
- **OP-TEE 生命周期：** Always Resident、Init-only、Pageable、动态分配。

这两个维度有关联，但不是同一个概念。

### **5.5.1&#x20;**`.text`**、**`.rodata`**、**`.data`

| Section   | 内容          | 初始化来源   | 典型运行期权限 |
| --------- | ----------- | ------- | ------- |
| `.text`   | 指令          | 镜像中的机器码 | RX      |
| `.rodata` | 常量、字符串、只读表  | 镜像      | R       |
| `.data`   | 初始化的全局或静态变量 | 镜像      | RW      |

在这里，RX/R/RW 表示期望权限。是否实现严格的只读与不可执行隔离，需要结合平台 MMU 配置验证。

例如：

```
int counter = 10;
const char banner[] = "OP-TEE";
```

`counter` 通常属于已初始化可写数据。

`banner` 可能进入只读数据区，但最终布局仍需以编译输出为准。

### **5.5.2&#x20;**`.bss`**：为什么不需要保存初始零值？**

例如：

```
static uint8_t large_buffer[64 * 1024];
```

这个变量需要 64 KiB Runtime Memory。

但编译器没有必要在 Binary 中保存 64 KiB 的零。

一般会把它放入 `SHT_NOBITS` 类型的 Section。

这样：

```
ELF File:
  No initialized payload for BSS

Runtime:
  64 KiB zero-initialized memory
```

在 ELF 的通用加载模型中，`PT_LOAD` 的 `p_memsz > p_filesz` 可以表达需要清零的空间。

但 OP-TEE 的 Raw Image 不会自动携带一个 ELF Loader。

它必须由自己的启动代码与加载约定保证相关零初始化语义。

上游 `gen_tee_bin.py` 生成 Raw Payload 时明确跳过 `SHT_NOBITS` 类型的 Section。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### **5.5.3&#x20;**`.nozi`**：为什么存在“不初始化成零”的内存？**

这个概念值得特别研究。

在当前 Arm Linker Script 中：

```
.nozi (NOLOAD) : {
    __nozi_start = .;

    KEEP(*(.nozi .nozi.*))

    . = ALIGN(16);

    __nozi_end = .;

    ...
}
```

相关源码注释将其描述为运行期不应进行普通清零的数据区域。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



典型用途包括：

- Early Entry Stack。
- 特定 Page Table。
- Boot 期间需要保留或特殊初始化的内存。
- 其他不能按照普通 `.bss` 处理的资源。

为什么不直接放入 `.bss`？

考虑 Boot 的早期执行顺序：

```
CPU Entry
    |
    v
Establish Temporary Stack
    |
    v
Run Early Initialization
    |
    v
Initialize BSS
    |
    v
Continue Boot
```

如果 Early Stack 与 `.bss` 清零范围重叠，那么在使用栈的过程中执行 BSS 清零，就可能破坏当前函数调用帧、返回地址或保存寄存器。

因此，某些早期资源必须具有不同的初始化生命周期。

这里更深层的设计思想是：

> OS 的内存初始化，不仅取决于变量应该是什么初始值，还取决于初始化动作发生时，哪些资源已经处于使用状态。

但还需要注意安全边界：

`.nozi`**&#x20;不保证数据安全，也不保证 Warm Boot 后保留的数据仍然可信。**

如果一块未清零内存可能包含上一次执行留下的密钥、指针或异常状态，就必须另外设计清理、重新初始化和可信性验证机制。

### **5.5.4 Heap 不一定对应镜像文件中的实际字节**

假设：

```
.heap2 (NOLOAD) : {
    __heap2_start = .;

    . += CFG_CORE_HEAP_SIZE;

    __heap2_end = .;
}
```

这说明 Linker 在地址空间中预留了区域。

但它不意味着 Raw Binary 中一定包含相同长度的实际数据。

一定要区分：

\\[ \text{Image File Size} \\]

\\[ \text{Linked Address Space Size} \\]

\\[ \text{Runtime Resident Memory} \\]

这三个量在 Pager、Heap、BSS 存在的情况下，通常明显不同。

例如，一个较小的 OP-TEE Core Binary，也可能需要更多的 Secure RAM 才能完成初始化。

## **5.6 OP-TEE Pager 的三层代码布局**

这一节是本章最重要的部分之一。

官方文档明确将 OP-TEE Core 的部分区域分成 Unpaged、Init/Paged、Paged 和 Demand Allocation 等类型。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



首先理解为什么要这样设计。

### **5.6.1 为什么 OP-TEE Core 需要 Pager？**

假设一个 SoC 的安全 SRAM 只有 256 KiB，而完整 OP-TEE Kernel 的代码、静态数据、Heap、Stack、Page Tables 总需求远远超过这个容量。

一种选择是把所有内容放入 Secure DDR。

但另一个选择是：

**只让必须长期驻留的内核代码和数据保留在高安全等级的内存中，允许非关键路径的代码按需进入物理内存。**

这样就需要 Pager。

然而，Pager 也引入了一个典型的操作系统自举问题：

> 如果处理 Page Fault 的代码本身发生 Page Fault，应该由谁处理？

因此必须划分不同的代码生命周期。

OP-TEE Core 的逻辑内存布局（Pager Enabled）

Unpaged — 始终驻留

`.text` · `.rodata` · `.data` · `.bss` · `.nozi` · Core Heap 等

早期入口、异常基础设施、Pager 自身及其必要的传递依赖

Init / Paged — 初始化时驻留

`.text_init` · `.rodata_init`

启动 Pager、建立初期系统环境；初始化完成后可转为按需分页

Pageable — 按需装载

`.text_pageable` · `.rodata_pageable`

由 Pager 根据 Page Fault 和映射状态管理

Demand Alloc — 按需分配

动态物理页资源，如特定配置下的 Trusted Thread Stack

示意图表示访问和生命周期分类，而不是固定物理地址或固定大小；具体区域取决于构建配置。

### **5.6.2 Unpaged Kernel 必须满足什么条件？**

最基本的不变量：

**处理 Page Fault 所必需的代码、数据和执行资源，不得依赖此次 Page Fault 才能建立的映射。**

例如：

```
Page Fault
    |
    v
Exception Entry
    |
    v
Pager Fault Handler
    |
    v
Page Allocator
    |
    v
Backing Store Access
    |
    v
Hash Verification
    |
    v
Page Table Update
    |
    v
Return From Exception
```

如果其中某个必要函数被错误地链接到了 Pageable 区域，就有可能触发递归 Page Fault。

但仅将 Fault Handler 函数本身标记为 Unpaged 还不够。

它调用的函数、读取的常量、通过函数指针访问的函数，以及需要的栈和关键页表，都必须满足可访问性要求。

这是一项**跨整个依赖闭包的约束**。

### **5.6.3 Init/Paged 为什么不是普通的 Init-only Code？**

这个设计特别值得思考。

传统操作系统可能在初始化完成后释放 Init Section 的物理内存。

OP-TEE 的 Pager 模型更特殊：初始化期间使用的部分内存随后可以纳入 Pager 管理，相关 Init/Paged 内容并非简单地被永久丢弃。

其目标是减少永久保留的物理内存。

所以要区分：

- Init 阶段必须物理驻留。
- Pager 建立后，不一定继续永久占用同一物理页。
- 后续是否还能访问对应内容，由 Pager 生命周期和映射规则决定。

这与 Linux 中典型的 `__init` 代码使用结束后直接释放，不应简单视为完全相同的语义。

## **5.7 为什么 OP-TEE 的 Link 过程需要多次分析？**

这是理解上游实现的一把钥匙。

很多人认为 Linker 只需要执行一次：

```
all .o → ld → tee.elf
```

但对于支持 Pager 的 OP-TEE，这个模型不够。

它面临一个问题：

**如何自动判断哪些函数必须进入 Unpaged，哪些函数属于 Init，哪些函数可以 Pageable？**

最简单的办法是开发者逐个手工标注。

但一个大型内核有大量函数调用关系，维护难度非常高。

因此，OP-TEE 使用了基于依赖关系分析的链接策略。

### **5.7.1 简化后的多阶段链接流程**

```
All Object Files
        |
        v
     all_objs.o
        |
        +------------------------+
        |                        |
        v                        v
  Identify Unpaged        Identify Init
  Root Symbols            Root Symbols
        |                        |
        v                        v
  Dependency GC           Dependency GC
        |                        |
        v                        v
    unpaged.o                init.o
        |                        |
        v                        v
  text_unpaged.ld.S       text_init.ld.S
  rodata_unpaged.ld.S     rodata_init.ld.S
        |                        |
        +-----------+------------+
                    |
                    v
             Final Linker Script
                    |
                    v
                 tee.elf
```

这是对上游 `link.mk` 链接流程的抽象；4.11.0 的实际构建规则中能够看到 `all_objs.o`、`unpaged.o`、`init.o` 以及上述自动生成的 Linker Fragment。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### **5.7.2 通过&#x20;**`--gc-sections`**&#x20;获取依赖闭包**

假设：

```
void pager_fault_handler(void)
{
    pager_alloc_page();
    pager_update_pte();
}

void pager_alloc_page(void)
{
    allocator_get_page();
}
```

逻辑调用图：

```
pager_fault_handler()
       |
       +-- pager_alloc_page()
       |       |
       |       +-- allocator_get_page()
       |
       +-- pager_update_pte()
```

如果 `pager_fault_handler()` 是一个必须常驻的 Root，依赖分析就需要把必要的代码和数据保留下来。

`--gc-sections` 的作用不是“生成 Pager”，而是基于保留的 Root 和可识别的 Section Reference，把不相关的 Section 排除。

这样得到的 Section 集合可以帮助构建系统划分 Unpaged / Init / Pageable 区域。

### **5.7.3 真实&#x20;**`link.mk`**&#x20;如何提取 Root？**

在 4.11.0 中可以看到类似的逻辑：

```
$(NMcore) $< | \
    $(AWK) '/ ____keep_pager/ { printf "-u%s ", $$3 }'
```

生成的 `-u` 参数用于让 Linker 强制保留相应符号，再结合 `--gc-sections` 计算相关 Section 集合。

随后：

```
$(READELFcore) -S -W $< | \
    $(PYTHON3) ./scripts/gen_ld_sects.py .text.
```

生成用于最终链接的 Section 列表。Init 区域采用类似方式处理。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### **5.7.4 这个机制有什么潜在风险？**

最有意思的问题是：Linker 的依赖分析不等于完整的程序语义分析。

例如：

```
typedef void (*handler_t)(void);

handler_t handler_table[] = {
    handle_page_fault,
    handle_rpc,
};
```

如果函数地址以特殊的方式存入表中，或者某些关系依赖 Assembly、动态生成的入口或特殊的函数指针布局，就必须确认 Linker 是否能正确保留必要依赖。

再比如：

```
void unpaged_handler(void)
{
    trace_fault_event();
}
```

如果新加入的 `trace_fault_event()` 依赖某个 Pageable Function 或 Pageable Read-only Data，那么修改可能扩大 Unpaged 依赖闭包，增加永久驻留内存。

更危险的情况是某种未被正确识别的间接引用导致运行期访问 Pageable 资源。

因此，对这个模块的修改要同时评估：

- 功能是否正确。
- Unpaged Dependency Closure 是否完整。
- Unpaged Memory 是否显著增长。
- Init/Paged 边界是否发生变化。
- 对应 Section 是否仍然满足权限和对齐要求。

**这也是一个看似普通的 Logging Patch，却可能影响 Boot Memory Footprint 的原因。**

## **5.8 从&#x20;**`tee.elf`**&#x20;到真正的 OP-TEE Boot Image**

现在回到一个非常具体的问题：

**假设&#x20;**`tee.elf`**&#x20;已经生成，为什么还需要&#x20;**`gen_tee_bin.py`**？它到底对 ELF 做了什么？**

核心源码：

[scripts/gen_tee_bin.py — OP-TEE 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/scripts/gen_tee_bin.py?utm_source=chatgpt.com)

### **5.8.1 首先理解几个不同的 Binary**

在当前 Arm 构建流程中，主要产物如下。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



| 产物                    | 内容与用途                           |
| --------------------- | ------------------------------- |
| `tee.elf`             | 包含链接结果、符号、Section 等的主 ELF       |
| `tee.bin`             | 包含传统 OP-TEE Header 的组合镜像        |
| `tee-header_v2.bin`   | 描述后续分离式镜像的 Header               |
| `tee-pager_v2.bin`    | Unpaged 部分、Init 部分以及必要元数据       |
| `tee-pageable_v2.bin` | 其余 Pageable Payload             |
| `tee-raw.bin`         | 不包含传统 OP-TEE Header 的 Raw Image |
| `tee.map`             | Linker Map，用于地址与符号分析            |
| `tee.dmp`             | `objdump` 生成的 ELF 反汇编及信息转储      |

它们不是彼此独立编译出来的多个 OS。

它们主要来自同一个 `tee.elf`，按照不同 Loader 的要求重新组织内容。

这里必须特别区分：

- `tee-pager_v2.bin` 并不是只有 Pager Handler 的 Binary。
- `tee-pageable_v2.bin` 也不包含完整的初始化状态。
- `tee-header_v2.bin` 本身不是可执行的 OP-TEE Kernel。

### **5.8.2&#x20;**`gen_tee_bin.py`**&#x20;如何提取 Section？**

我们来看经过简化但保留真实判断逻辑的代码：

```
def get_sections(elffile, pad_to, dump_names):
    last_end = 0
    bin_data = bytearray()

    for section in elffile.iter_sections():
        section_name = get_name(section)

        if (section['sh_type'] == 'SHT_NOBITS' or
            not (section['sh_flags'] & SH_FLAGS.SHF_ALLOC) or
            not dump_names.match(section_name)):
            continue

        # Copy section contents and fill address gaps

    return bin_data
```

实际源码的处理逻辑是：

1. 遍历 ELF Section。
2. 排除 `SHT_NOBITS`。
3. 排除不需要在运行时分配的 Section。
4. 根据 Section 名称筛选对应的区域。
5. 根据地址关系填充必要的 Padding。
6. 生成连续的 Raw Payload。

**关键点：这里依赖的是 Linker 已经形成的 Section Layout。**

也就是说，`gen_tee_bin.py` 并不是重新决定哪些函数属于 Unpaged。

这项工作已经由前面介绍的多阶段链接流程完成。

### **5.8.3&#x20;**`tee-pager_v2.bin`**&#x20;的真实组织方式**

4.11.0 中可以看到以下关键逻辑的简化表达：

```
pager_bin = get_pager_bin(elffile)
pageable_bin = get_pageable_bin(elffile)
embdata_bin = get_embdata_bin(elffile)

outf.write(pager_bin)
outf.write(pageable_bin[:init_bin_size])
outf.write(embdata_bin)
```

因此，`tee-pager_v2.bin` 的实际内容可以表示成：

```
+----------------------------------+
| Unpaged Core                     |
|  - Text / RO Data                |
|  - Initialized RW Data           |
+----------------------------------+
| Init Portion                     |
|  - Init Text                     |
|  - Init RO Data                  |
+----------------------------------+
| Embedded Metadata                |
|  - Page Hashes                   |
|  - Relocation Information        |
+----------------------------------+
```

对应的 `tee-pageable_v2.bin` 包含：

```
+----------------------------------+
| Remaining Pageable Content       |
|  - Pageable Text                 |
|  - Pageable RO Data              |
+----------------------------------+
```

这个分界可以从 `output_pager_v2()`、`output_pageable_v2()` 和 `get_embdata_bin()` 的实现直接得到。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



这里的 Embedded Metadata 特别值得关注。

因为它除了包含 Page Hash，还包含 Relocation Information。

换句话说，**OP-TEE 的 Image Generator 不仅输出机器码，还要输出供早期运行时使用的内核元数据。**

### **5.8.4 为什么&#x20;**`tee-pageable_v2.bin`**&#x20;可以被加载到其他位置？**

这是 Pager 架构带来的一个重要性质。

对于传统固定地址 Binary，Load Address 与后续执行往往有直接关系。

但 Pageable Payload 本质上是 Backing Store 内容。

它不是要求 CPU 直接从 Loader 放置的每个物理地址执行指令，而是通过 Pager 将内容映射到对应的运行时虚拟地址。

因此，Bootloader 可以在满足平台内存约束的前提下，将 Pageable Payload 放在一个可用的物理内存区域中。

后续由 OP-TEE 建立并管理正确的映射。

但这不意味着：

“可以让 Normal World 随便指定任何内存地址，然后 Secure Kernel 就直接信任。”

Pageable Payload 的来源、可访问范围、覆盖关系和完整性，仍必须满足安全约束。

## **5.9 OP-TEE Image Header 的设计**

理解 Image Header 最好的方式，不是记住字段，而是理解它到底在为 Loader 提供哪些信息。

### **5.9.1 传统 V1 Header**

官方文档定义了以下格式：

```
#define OPTEE_MAGIC 0x4554504f

struct optee_header {
    uint32_t magic;
    uint8_t version;
    uint8_t arch;
    uint16_t flags;

    uint32_t init_size;

    uint32_t init_load_addr_hi;
    uint32_t init_load_addr_lo;

    uint32_t init_mem_usage;
    uint32_t paged_size;
};
```

这是传统 Header 的核心结构定义。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



字段的主要用途：

| 字段                 | 设计目的                       |
| ------------------ | -------------------------- |
| `magic`            | 判断输入是否符合预期 OP-TEE 格式       |
| `version`          | 识别 Header 格式版本             |
| `arch`             | 标识目标 CPU 架构                |
| `flags`            | 格式约定的标志位                   |
| `init_size`        | Loader 需要放置的初始化 Payload 大小 |
| `init_load_addr_*` | 初始化 Payload 的目标加载地址        |
| `init_mem_usage`   | 初始化阶段需要的内存容量信息             |
| `paged_size`       | 后续 Pageable Payload 的大小    |

注意：

**Header 描述的是加载协议，不是密码学上的安全证明。**

仅仅因为 `magic` 正确，绝不能认定镜像可信。

### **5.9.2 V2 Header：为什么需要重新设计？**

V1 的问题不是无法启动 OP-TEE。

而是当整个镜像包含具有不同加载位置需求的 Payload 时，将它们装在一个整体容器里不利于 Boot Firmware 灵活管理。

因此，V2 定义了 Image Descriptor：

```
struct optee_image {
    uint32_t load_addr_hi;
    uint32_t load_addr_lo;
    uint32_t image_id;
    uint32_t size;
};

struct optee_header_v2 {
    uint32_t magic;
    uint8_t version;
    uint8_t arch;
    uint16_t flags;
    uint32_t nb_images;
    struct optee_image optee_image[];
};
```

`image_id` 区分：

```
#define OPTEE_IMAGE_ID_PAGER 0
#define OPTEE_IMAGE_ID_PAGED 1
```

这意味着 Loader 可以根据多个 Descriptor 分别处理 Payload。

一个概念性示例：

```
tee-header_v2.bin
    |
    +-- Image 0
    |     ID = PAGER
    |     Size = ...
    |     Load Address = Fixed Address
    |
    +-- Image 1
          ID = PAGED
          Size = ...
          Load Address = Flexible
```

对于 Pageable Image，Header 中可能使用 `0xFFFFFFFF` 作为特殊的 Load Address 表示方式，具体约定由该格式定义及 Loader 实现解释。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



在 4.11.0 的生成逻辑中，若没有独立 Pageable Payload，Header 可以只描述一个 Image。

### **5.9.3 与 TF-A FIP 的关系**

在典型 Arm Trusted Firmware-A 集成中，可以看到这样的构建参数：

```
BL32=tee-header_v2.bin
BL32_EXTRA1=tee-pager_v2.bin
BL32_EXTRA2=tee-pageable_v2.bin
```

这表示 TF-A 构建流程可以把 OP-TEE Header 与多个 Payload 放进 FIP。

但要小心两个不同的概念：

**FIP 是固件镜像容器；OP-TEE Header 是 OP-TEE Payload 的加载描述。**

二者职责并不相同。

```
Firmware Image Package
        |
        +-- BL31
        |
        +-- BL32 Header
        |      |
        |      +-- OP-TEE Image Descriptors
        |
        +-- BL32 Extra1
        |
        +-- BL32 Extra2
        |
        +-- Other Firmware
```

它还涉及 Firmware 认证与分发，而不只是 Linker 布局。

## **5.10 Relocation、PIE 与 OP-TEE Core ASLR**

这是本章第二个需要深入到工程实现细节的主题。

### **5.10.1 为什么 Core 需要 Relocation？**

假设 OP-TEE 编译时使用基准地址：

```
Link Address: 0x30000000
```

而实际映射地址为：

```
Runtime VA: 0x40000000
```

这里产生偏移：

\\[ \Delta = 0x40000000 - 0x30000000 \\]

对于某个需要重定位的绝对地址引用，必须根据这个偏移修正引用结果。

但不是所有指令都需要 Relocation。

例如，在 AArch64 中：

```
bl target_function
```

可能通过 PC-relative Branch 完成跳转。

当代码整体移动并保持内部相对位置不变时，这类跳转可能无需修改。

但全局地址表、初始化函数指针或某些绝对引用仍然可能需要修正。

因此：

**位置无关指令的存在，不意味着镜像中的全部数据引用天然位置无关。**

### **5.10.2 OP-TEE 在 Link 阶段如何支持 Relocation？**

4.11.0 的 `link.mk` 包含以下条件：

```
ifeq ($(call cfg-one-enabled, \
    CFG_CORE_ASLR CFG_CORE_PHYS_RELOCATABLE),y)

link-ldflags += -pie -Bsymbolic -z norelro \
                $(ldflag-apply-dynamic-relocs)

endif
```

AArch64 的相关配置还使用 `-z text` 这样的限制。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里分别涉及：

- `-pie`：生成支持位置无关执行的链接布局。
- `-Bsymbolic`：控制内部符号绑定行为。
- `-z text`：要求不产生需要在只读 Text 中执行的动态重定位。
- Relocation 处理：确保需要修正的引用能够在运行时正确调整。

但请注意：

**PIE 是一种构建属性；ASLR 是一种运行时地址随机化机制。**

只有具备随机化决策、正确加载与重定位流程，才能真正实现 ASLR。

### **5.10.3&#x20;**`gen_tee_bin.py`**&#x20;如何组织 Relocation？**

我们来研究一个非常有代表性的实现细节。

在 4.11.0 的 `get_reloc_bin()` 中：

```
if get_arch_id(elffile) == 0:
    exp_rel_type = ENUM_RELOC_TYPE_ARM['R_ARM_RELATIVE']
else:
    exp_rel_type = ENUM_RELOC_TYPE_AARCH64['R_AARCH64_RELATIVE']
```

随后遍历 ELF Relocation。

其中：

```
if rel['r_info_type'] != exp_rel_type:
    # Unexpected relocation type
    sys.exit(1)
```

这个检查的意义非常重要。

它说明 Image Generator 并不是通用的 ELF Dynamic Linker。

它只接受当前实现支持的 Relocation Type。

接下来，工具把需要修正的位置转换为相对链接基址的 Offset，并生成更紧凑的 Relocation Metadata。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



简化模型：

```
ELF Relocation Entries
        |
        v
Validate Supported Relocation Type
        |
        v
Convert Target Addresses to Offsets
        |
        v
Sort Offsets
        |
        v
Embedded Relocation Table
        |
        v
OP-TEE Early Runtime Relocation
```

这是一种很典型的 Firmware 设计：

**在构建阶段尽量消化复杂的 ELF 信息，让 Boot 阶段只需完成最小的重定位工作。**

它减少了 Early Boot 对完整 ELF Loader 的依赖。

### **5.10.4 为什么不能随便新增 Relocation Type？**

假设升级 GCC 或 LLVM 后，某个编译选项导致新的 Relocation Type 出现。

可能出现两类问题。

第一类：Image Generator 在生成阶段直接报错。

这相对容易发现。

第二类更危险：工具或运行时处理不充分，但镜像仍然成功生成。

这样可能导致：

- 某个函数指针引用错误。
- Initcall Table 地址不正确。
- Exception Handler 指针错误。
- 某种特定配置下才触发非法访问。

因此，在升级编译器、改变 Linker Flag、增加特殊 Assembly 时，需要检查最终 `.rel*`、`.rela*` 的实际内容。

### **5.10.5 物理重定位与虚拟 ASLR 不是一回事**

对于 SoC Firmware 设计，我特别建议区分：

| 能力                    | 主要解决的问题          |
| --------------------- | ---------------- |
| Physical Relocation   | 镜像能否放到不同物理地址     |
| Virtual ASLR          | 运行时虚拟地址布局是否随机化   |
| PIE                   | 链接后的引用能否支持位置变化   |
| Relocation Processing | 需要修正的绝对引用是否被正确修正 |
| MMU Mapping           | 最终 CPU 地址翻译是否正确  |

即使能够动态选择物理加载地址，也不自动代表支持随机化的虚拟地址空间。

反过来，虚拟地址随机化也不必然要求物理加载地址变化。

**这两个自由度可以分离，但必须由 Boot、Linker、Relocation 和 MMU 共同支持。**

### **5.10.6 一个真实源码中很有意思的细节：为什么需要撤销 Init Relocation？**

这是非常值得研究的地方。

OP-TEE 的 Pager 为只读页面建立 Hash，以确认从 Backing Store 恢复的数据没有被篡改。

但问题来了。

假设 Init Code 在 Early Boot 时已经完成 Relocation。

原始镜像：

```
Pointer = 0x30001234
```

运行时重定位后：

```
Pointer = 0x40001234
```

如果直接对重定位后的 Init Page 计算 Hash，那么结果显然与构建阶段针对原始镜像计算的 Hash 不一致。

如何解决？

在 4.11.0 的 `boot.c` 中，存在：

```
static void undo_init_relocation(...)
```

它用于将需要保留到 Pager Backing Store 的 Init 内容调整回适合完整性验证的原始形式。随后再进行 Page Hash 检查。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这展示了一个很重要的架构原则：

> Runtime Relocation 改变了内存中的内容，因此必须明确完整性验证究竟针对原始镜像，还是针对重定位后的运行时表示。

不能简单地把“代码已经经过 Hash 校验”和“代码已经完成 Relocation”看成完全独立的两件事。

两者之间存在直接的数据依赖。

## **5.11 从 Image 到 Running Kernel：一条真实启动链**

现在我们把前面的知识串起来。

以下以常见的 Arm + TF-A + OP-TEE Core 为背景，描述概念性流程。

```
Boot ROM
    |
    v
Authenticated Firmware Stages
    |
    v
Load OP-TEE Header / Payloads
    |
    v
Place Init Image at Expected Address
    |
    v
Handoff to OP-TEE _start
    |
    v
Early Assembly
    |
    +-- Establish CPU Execution State
    +-- Early Stack
    +-- Essential MMU Mapping
    +-- Relocation, if configured
    |
    v
Early Core Initialization
    |
    +-- BSS / Runtime State
    +-- Memory Initialization
    +-- Pager Initialization
    |
    v
Verify and Prepare Pageable Backing Store
    |
    v
Runtime Mapping / Pager Active
    |
    v
OP-TEE Normal Runtime
```

真实的细节顺序会受到平台入口、MMU 配置及 Boot Mode 影响，因此不能把这个抽象流程当作所有架构的逐指令顺序。

### **5.11.1 Page Hash 是如何工作的？**

在 `gen_tee_bin.py` 中，Pageable Payload 会按页面计算 SHA-256。

核心逻辑可简化为：

```
for n in range(0, len(pageable_bin), small_page_size):
    page = pageable_bin[n:n + small_page_size]
    data += hashlib.sha256(page).digest()
```

这意味着每个 4 KiB 页面都有相应的 Hash。

在运行时，当需要把页面从 Backing Store 读入时，可以根据对应 Hash 验证内容。

例如：

```
Page Fault
    |
    v
Find Corresponding Backing Store Page
    |
    v
Copy / Prepare Page
    |
    v
SHA-256 Verification
    |
    +-- Mismatch → Reject / Panic
    |
    v
Apply Required Relocation, if any
    |
    v
Update Translation Table
    |
    v
Resume Execution
```

这里需要区分早期对整个 Pageable Backing Store 的验证，以及后续 Pager 按需管理页面的过程。

在 4.11.0 中，`init_pager_runtime()` 会准备 Backing Store、复制或移动相关内容，并验证 Page Hash。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



### **5.11.2 有 Page Hash 是否代表 Secure Boot 已经安全？**

**不代表。**

这是一个非常经典的安全架构问题。

假设攻击者同时修改：

```
Malicious Page
    +
SHA256(Malicious Page)
```

如果 Hash 本身没有可信的来源，那么 Hash 校验只能证明：

“当前 Page 与当前存储的 Hash 一致。”

却无法证明：

“当前 Page 是设备厂商认可的代码。”

因此，必须建立完整信任链：

```
Hardware Root of Trust
        |
        v
Authenticated Boot Firmware
        |
        v
Authenticated OP-TEE Image
        |
        v
Trusted Page Hash Metadata
        |
        v
Verified Pageable Content
```

这里的重点不是一定要对每个页面单独做数字签名。

如果完整镜像或承载 Hash 的元数据已经被上一级可信 Firmware 正确认证，而且在使用过程中受到适当保护，那么 Hash 可以作为 Page 内容完整性的验证机制。

反之，只要 Hash Metadata 或整个 Image 可以被攻击者无约束地替换，单纯的 SHA-256 就不足以建立真实性。

### **5.11.3 为什么 Bootloader 不应该随意解析并修改 OP-TEE 内容？**

Bootloader 在这里承担两项职责：

- 将 Payload 放置到满足约定的内存。
- 根据可信启动链要求执行镜像认证及控制权交接。

但它不能随意改变镜像内部 Section 顺序、Padding、Relocation Metadata 或 Hash Table。

因为：

**Image Generation 和 Runtime Interpretation 之间存在精确的数据格式契约。**

一个字段偏移、一段 Padding，甚至一个未被正确清零的区域，都可能破坏 OP-TEE 初始化。

对于 RISC-V + RSS/HSM + OpenSBI 的平台，这个原则完全一样。

只是上一级 Firmware Chain、认证组件、加载协议和执行 Domain 可能不同，不能直接照搬 TF-A 的 BL32 文件名称和 Header 处理流程。

# **5.12 实战：如何真正研究一次 OP-TEE Binary Layout？**

这一部分建议在你本地的 OP-TEE 源码仓库和已有构建环境里操作。

不需要额外开发一个复杂 Demo。

目标是把真实构建产物与前面的理论逐一对应。

## **实验一：从 ELF 研究全部核心 Section**

首先定位构建输出。

以下使用常见 Arm 构建目录作为示例；实际 `out/` 目录名称取决于 Platform 和配置。

```
ls out/arm-plat-*/core/
```

然后：

```
ELF=out/arm-plat-xxx/core/tee.elf

readelf -hW "$ELF"
readelf -SW "$ELF"
readelf -lW "$ELF"
readelf -sW "$ELF"

objdump -h "$ELF"
```

需要特别关注：

```
ELF Entry Point

Section:
  .text
  .rodata
  .data
  .bss
  .nozi
  .text_init
  .rodata_init
  .text_pageable
  .rodata_pageable

Segment:
  PT_LOAD
  p_vaddr
  p_paddr
  p_filesz
  p_memsz
  p_flags
```

如果某个 Section 不存在，不要立即断定编译错误。

先确认相应 `CFG_*` 配置是否启用。

### **实验的关键问题**

比较：

```
ELF File Size
Sum of Allocated Section Sizes
PT_LOAD Total Memory Requirements
Raw Binary Size
```

解释为什么四个数不一定相同。

特别是 `.bss`、`.nozi`、Heap 和 Debug Section，会造成很大差异。

## **实验二：研究 Linker Symbol 与实际内存区域**

使用：

```
nm -n "$ELF" | grep -E \
'__text_|__data_|__bss_|__nozi_|__init_|__pageable_|__flatmap_'
```

再用：

```
grep -E \
'__text_|__data_|__bss_|__nozi_|__init_|__pageable_' \
out/arm-plat-xxx/core/tee.map
```

建立一张实际的 Address Layout 表。

例如：

| Region          | Start     | End       | Size        |
| --------------- | --------- | --------- | ----------- |
| Unpaged Text    | 实际 Symbol | 实际 Symbol | End − Start |
| Unpaged RO Data | 同上        | 同上        | 同上          |
| RW Data         | 同上        | 同上        | 同上          |
| BSS             | 同上        | 同上        | 同上          |
| Init            | 同上        | 同上        | 同上          |
| Pageable        | 同上        | 同上        | 同上          |

然后对照：

```
CFG_WITH_PAGER
CFG_CORE_ASLR
CFG_CORE_RODATA_NOEXEC
CFG_CORE_RWDATA_NOEXEC
CFG_CORE_PHYS_RELOCATABLE
```

比较不同配置下的布局。

**验收标准：** 不只是说明 Section 地址，还要判断每个区间的加载方式、初始化责任、运行时权限和生命周期。

## **实验三：手工解析 V2 Header**

这部分非常适合实际操作，因为格式比较简单。

可以使用下面这段 Python 代码解析已经生成的 `tee-header_v2.bin`：

```
import struct
from pathlib import Path

data = Path("tee-header_v2.bin").read_bytes()

magic, version, arch, flags, count = \
    struct.unpack_from("<IBBHI", data, 0)

assert magic == 0x4554504f
assert version == 2
assert len(data) >= 12 + count * 16

print(f"magic:   0x{magic:08x}")
print(f"version: {version}")
print(f"arch:    {arch}")
print(f"images:  {count}")

for i in range(count):
    hi, lo, image_id, size = \
        struct.unpack_from("<IIII", data, 12 + i * 16)

    addr = (hi << 32) | lo

    print(f"Image {i}:")
    print(f"  id   = {image_id}")
    print(f"  addr = 0x{addr:016x}")
    print(f"  size = 0x{size:x}")
```

运行后，再比较：

```
stat -c '%n %s bytes' \
    tee-header_v2.bin \
    tee-pager_v2.bin \
    tee-pageable_v2.bin
```

需要验证：

1. Descriptor 的数量是否与 Payload 一致。
2. Image Size 是否与文件长度一致。
3. Load Address 是否符合平台约定。
4. Pager 关闭时 Header 如何变化。
5. 如果修改 `TEE_LOAD_ADDR`，哪些 Header 字段发生变化。

更进一步，可以阅读 `get_init_load_addr()`，验证它如何从 ELF 中的 `_start` 得到加载地址。

## **实验四：研究编译选项如何改变 Binary**

建议至少比较以下三个构建配置：

| 实验 | Pager    | ASLR     | 目标                       |
| -- | -------- | -------- | ------------------------ |
| A  | Disabled | Disabled | 建立最简单的固定地址布局             |
| B  | Enabled  | Disabled | 研究 Unpaged/Init/Pageable |
| C  | 依平台支持    | Enabled  | 研究 Relocation 和地址变化      |

这不是保证所有平台都支持这些组合；需要先通过配置校验。

重点观察：

```
readelf -SW tee.elf
readelf -rW tee.elf
nm -n tee.elf
```

以及：

```
tee.map
tee-pager_v2.bin
tee-pageable_v2.bin
tee.symb_sizes
```

最终回答：

- Pager 对永久驻留内存有什么影响？
- ASLR 对 Relocation Metadata 有什么影响？
- 代码段地址是否发生变化？
- Runtime RAM 需求是否等于 Binary Size？
- 哪些配置改变了最终内存访问权限？

### **一个额外的 Staff 级实验**

给 `pager_fault_handler()` 的某个关键路径增加一条日志。

然后检查：

- Unpaged Text Size。
- Unpaged RO Data Size。
- `tee-pager_v2.bin` Size。
- 函数调用依赖闭包是否变化。
- Pager 开启时是否仍然可以执行。
- 相关早期日志功能是否引入新的初始化依赖。

这个实验能够把 Code Change 和 Binary Layout 真正关联起来。

# **5.13 修改 Linker 或 Binary Layout 时，如何做影响分析？**

我们用一个实际开发场景进行总结。

假设你需要在 OP-TEE Core 中增加一个新的安全测量功能。

要求它在系统早期启动时对某个 Region 进行 Hash 测量。

你需要新增：

```
void measure_secure_kernel(void);
```

并且希望它在 Pager 初始化之前运行。

表面上只是增加一个函数。

但我们不能只考虑函数是否可以编译。

### **第一层：Execution Dependency**

必须确认：

```
measure_secure_kernel()
    |
    +-- hash_init()
    +-- hash_update()
    +-- hash_final()
```

这些函数是否都能在 Early Boot 阶段执行。

例如，如果 Hash Backend 依赖尚未初始化的硬件 Crypto Driver，就会产生初始化顺序问题。

### **第二层：Memory Dependency**

必须确认：

- 函数本身是否位于可访问区域。
- Hash Algorithm 的常量表是否位于可访问区域。
- 所需的 Stack 是否可用。
- 使用的 Memory Allocator 是否已经初始化。
- 目标测量 Region 是否已经被映射。

### **第三层：Relocation Dependency**

如果测量的是已经完成 Relocation 的 Runtime Image：

**得到的 Digest 可能不等于针对原始 Binary 计算的 Digest。**

此时需要明确测量语义：

- 测量 Original Image？
- 测量 Relocated Runtime Image？
- 测量经过某些规范化处理的 Image？
- 使用什么方式将测量结果与信任链绑定？

这些选择没有一个能脱离 Threat Model 单独判断正确。

### **第四层：Security Dependency**

需要保证：

- 待测量内容在测量之前没有被不可信参与者修改。
- 测量范围准确覆盖预期代码和数据。
- 没有遗漏 Runtime Patch、Relocation 或可执行映射。
- 测量结果有可信的存储、签名或消费路径。

### **第五层：Compatibility**

需要确认：

- Pager Enabled/Disabled。
- ASLR Enabled/Disabled。
- Arm32/Arm64。
- 不同平台的早期 Crypto Backend。
- 不同 Bootloader 加载协议。

最后，应形成这样的 Change Impact Report：

| 分析维度         | 主要风险             | 验证手段                     |
| ------------ | ---------------- | ------------------------ |
| Linker       | 早期代码被放进 Pageable | Map / Section Analysis   |
| Boot         | 调用早于依赖初始化        | Early Boot Trace         |
| Memory       | 地址或映射不可访问        | MMU / GDB                |
| Relocation   | Hash 基准不一致       | Compare Original/Runtime |
| Crypto       | Backend 未就绪      | Init Dependency Test     |
| Security     | 测量来源不可信          | Threat Model Review      |
| Architecture | 不同 Backend 假设不同  | Cross-Arch Build / Test  |

这就是本章要建立的能力：

**从一次看似局部的 C 代码修改，推导出整个 Binary Layout、Boot、Memory 和 Security Chain 中可能受到的影响。**

# **5.14 Staff Engineer 级别综合面试题**

下面这 12 道题不以记忆函数名为目标，而是考察系统建模、根因分析、设计权衡和安全推理。

每道题都配有主要考察点和参考分析方向，但不会把问题简化成唯一的标准答案。

建议先独立思考，再查看参考分析。

## **Question 01 — 一个只改了日志的 Patch，为什么让系统无法启动？**

**难度：高 · Dependency / Boot / Linker**

某位工程师为了调试 OP-TEE 的 Pager Fault，在异常处理路径中增加：

```
EMSG("Pager fault: %s",
     decode_fault_reason(reason));
```

原先系统正常启动。

现在：

- `CFG_WITH_PAGER=n` 时正常。
- `CFG_WITH_PAGER=y` 时启动阶段出现递归 Page Fault。
- 将日志删除后恢复正常。

要求：

1. 给出至少三种可能的根因。
2. 如何区分是代码位置、只读数据位置还是初始化依赖的问题？
3. 为什么 `--gc-sections` 没有自动保证所有场景安全？
4. 如果必须保留日志，如何修改架构？
5. 如何设计防止此问题再次发生的构建检查？

参考分析方向

首先建立 Exception Path 的依赖闭包。需要检查 `decode_fault_reason()` 的代码位置、Format String、日志 Backend、锁、Per-CPU State 和 Stack。然后比较两种配置下的 Section Map，判断是否发生了不允许的 Pageable 访问。最后需要考虑动态函数指针和早期 Console 生命周期，而不只是给某个函数增加 Unpaged Annotation。

## **Question 02 — 一个镜像可以在任意物理地址启动，就代表它支持 ASLR 吗？**

**难度：高 · Relocation / MMU / Security**

假设一个厂商宣称其 OP-TEE Port 支持 ASLR，理由是：

“我们已经让 Bootloader 可以把 OP-TEE 加载到 Secure DDR 中任意一个 2 MiB 对齐地址。”

要求：

1. 你认可这个判断吗？
2. 如何区分 Physical Relocation 和 Virtual ASLR？
3. 需要哪些 Relocation 能力？
4. 如何检查实际地址是否具有随机性？
5. 如何评估 Randomness Source 的安全性？
6. 什么情况下虽然物理地址变化了，但攻击者仍然能够预测 Kernel VA？

参考分析方向

物理加载灵活性只是一个条件，不等于地址随机化。需要区分 PA、VA、Link Address、Load Offset、Runtime Mapping、Entropy 和 Information Leakage。还需要确认 Linker 产生的 Relocation Type 是否被支持，以及随机化是否在任何可利用的内存破坏发生之前生效。

## **Question 03 — 如何证明一个 OP-TEE Kernel 可以安全地释放 Init Memory？**

**难度：非常高 · Lifetime / Linker / Resource Ownership**

你希望减少 OP-TEE 常驻内存，于是考虑在初始化完成后复用所有 Init Region 对应的物理页。

你会如何证明该方案安全？

需要考虑：

- Initcall Function Pointer。
- Deferred Work。
- Interrupt Handler。
- Driver Callback。
- Shared RO Data。
- Static Constructor。
- Function Pointer Table。
- SMP Secondary CPU Initialization。
- Suspend/Resume。

参考分析方向

关键不只是确认 Init Function 已经返回，还要证明所有可能在未来引用该区域的执行路径都已结束。需要建立 Reachability、Callback Lifetime、SMP State 和 Power Management 约束。如果某些 Secondary Boot 或 Resume 路径仍然依赖对应代码，就不能简单复用该内存。还应区分 Pager 模式下 Init/Paged 区域的特殊生命周期。

## **Question 04 — Secure Boot 已经验证了 OP-TEE 镜像，是否还需要 Pager Hash？**

**难度：非常高 · Threat Model / Cryptography**

一个 SoC 使用 Hardware RoT 验证 TF-A 和 OP-TEE Image。

安全团队提出：

“既然 Boot 已经验证了 Image，就没有必要再保存每页 SHA-256 Hash，可以去掉以节省 Secure RAM。”

你是否同意？

要求分析：

1. Boot-time Authentication 保障什么？
2. Runtime Pager Integrity 保障什么？
3. Backing Store 是否可能被不可信实体修改？
4. Image Authentication 与 Page Hash 之间是什么关系？
5. 什么时候可以安全地取消 Page Hash？
6. 什么情况下这种优化会破坏系统信任模型？

参考分析方向

核心是区分时间点与攻击面。启动时验证的内容，在运行时可能仍处于可被修改的存储介质中。是否需要重新验证，取决于 Backing Store 的物理保护、攻击者能力、数据使用方式以及是否存在其他持续完整性机制。仅以“启动过一次验证”作为理由并不充分。

## **Question 05 — 为什么&#x20;**`tee.elf`**&#x20;中看不到问题，**`tee.bin`**&#x20;却可能启动失败？**

**难度：高 · ELF / Image Generator**

某个 OP-TEE Core 构建完成。

你发现：

- `readelf` 可以正常解析 ELF。
- 所有 Symbol Address 看起来都正确。
- `objdump` 可以正常反汇编。
- `tee.bin` 可以生成。
- 但 Bootloader 加载后立即异常。

请提出一套有顺序的调查方案。

特别考虑：

- ELF Section 与 Raw Image 的差异。
- Section Ordering 和 Padding。
- BSS / NOLOAD。
- Entry Point 与 Load Address。
- V1/V2 Header 混用。
- Firmware 实际加载的 Image。
- Cache Maintenance。
- Memory Overlap。

参考分析方向

首先确认 Bootloader 实际加载的文件和 Header 格式，再验证物理地址与加载长度。其次比较生成工具选取的 Section、Padding 和实际内存内容。最后分析 CPU Entry State、Cache、MMU、Link Address 和 Runtime Relocation。能够被 ELF 工具解析并不保证 Raw Image 与 Loader 契约匹配。

## **Question 06 — 如何安全地为 RISC-V OP-TEE 设计可重定位 Core Image？**

**难度：极高 · Cross-Architecture Design**

假设你负责一个 RISC-V RV64 OP-TEE Port。

设计要求：

- Secure Domain 的物理内存地址可配置。
- Boot Firmware 可能将 Core 加载到不同的 Secure RAM。
- 需要支持 Kernel Virtual Address Randomization。
- OP-TEE 可能由 RSS/HSM 协助认证和加载。
- 后续需要支持 SMP。

你会如何设计完整方案？

请覆盖：

1. Linker Script。
2. ELF Program Header 与 Section Layout。
3. RISC-V Relocation Type。
4. Early Boot 与 Runtime Address。
5. `satp`、Early Page Table 和地址映射。
6. Secondary Hart 的启动约束。
7. Image Generator 与 Loader ABI。
8. Secure Boot 的认证边界。
9. Runtime Integrity。
10. 如何验证不同装载地址下的正确性。

参考分析方向

最关键的是先定义 Boot Contract，然后确定 Link Address、PA、VA 三者之间的关系。需要设计可以在 MMU 启用前运行的最小位置无关入口，明确 Relocation 的应用时机，并防止 Secondary Hart 读取未完成初始化的全局状态。Arm 的 `gen_tee_bin.py` 不能未经架构适配就直接作为 RV64 方案，因为 Relocation Type、Header Arch 和启动协议均需要验证。

## **Question 07 — Linker Script 中所有 Section 都正确对齐，能证明 W^X 吗？**

**难度：非常高 · Memory Protection**

假设安全审计报告声称：

“我们已经将 `.text`、`.rodata`、`.data` 按 4 KiB 对齐，因此整个 OP-TEE Core 满足 W^X。”

你认为这个结论是否充分？

进一步考虑：

- `PT_LOAD` Flags。
- MMU Page Permissions。
- Early Identity Mapping。
- Temporary RWX Mapping。
- Relocation Window。
- DMA Access。
- 多个 VA 映射同一个 PA。
- Page Table 被恶意或错误修改。

参考分析方向

Section Alignment 只是在物理页和权限粒度上实现隔离的前提。需要验证所有有效 Mapping，而不只是某个 ELF Section。尤其要检查同一物理页是否通过不同 VA 形成 RW 与 RX 双重别名，以及 Boot 和 Relocation 阶段的临时权限是否被及时收紧。

## **Question 08 — Page Hash 为什么可能在 ASLR 开启后失效？**

**难度：非常高 · Relocation / Integrity**

有一个修改：

“为了简化 Boot，删除 `undo_init_relocation()`。”

测试结果：

- ASLR Disabled：通过。
- ASLR Enabled：Pager Hash Verification 失败。

请解释完整原因。

进一步回答：

1. 原始 Hash 计算针对哪一份数据？
2. Runtime Relocation 改变了什么？
3. 为什么不能直接忽略 Init Region 的 Hash 失败？
4. 为什么还原数据后需要保证不破坏运行时实际代码？
5. 如果重新设计 Hash Scheme，有哪些可能方案，各有什么风险？

参考分析方向

需要从 Build-time Page Hash、Runtime Relocation、Backing Store Preparation 三个阶段建立一致性模型。最关键的是区分正在执行的 Init Memory 与复制到 Backing Store 的内容。可以考虑原始表示的 Hash、规范化表示的 Hash 或可信的运行时度量，但必须明确验证基准和被认证的元数据。

## **Question 09 — 如何在不看完整源码的情况下估算 OP-TEE 的内存占用？**

**难度：高 · Capacity Planning**

给你一个实际构建：

```
tee.elf              10 MiB
tee-pager_v2.bin     280 KiB
tee-pageable_v2.bin  900 KiB
```

并告诉你：

```
Secure SRAM: 512 KiB
Secure DDR:  16 MiB
```

请问：

1. 能否认为 OP-TEE 一定能启动？
2. 512 KiB 是否足以容纳全部 Unpaged Runtime State？
3. 如何估算 Pager Page Pool？
4. 如何考虑 Thread Stack、Page Table、Heap、Hash Metadata？
5. 多核情况下额外需要什么？
6. 应该在编译阶段做哪些容量约束？
7. 哪些内存需求无法仅凭静态 ELF 精确判断？

参考分析方向

`tee.elf` 大小可能包含大量调试信息，而 `tee-pager_v2.bin` 的大小也不等于全部永久占用内存。需要结合 Linker Symbol、NOLOAD、Heap、Stack、Page Table、Hash Table、Pager Page Pool 和运行时配置。还需要检查物理地址布局和不同内存区域的实际可用性，不能只比较两个文件大小。

## **Question 10 — 如何处理 Firmware 更新后的 OP-TEE Image ABI 不兼容？**

**难度：极高 · Compatibility / Firmware Architecture**

假设原有 Bootloader 只支持传统 OP-TEE V1 Header。

你将 OP-TEE 升级到一个使用 V2 Image Descriptor 的构建，并计划同时启用新的 Pager 配置。

问题：

1. 什么是 Bootloader 与 OP-TEE 的真正 ABI？
2. 哪些字段属于 Load Contract？
3. 如何判断旧 Bootloader 是否兼容新 Image？
4. 如何设计版本升级与回滚策略？
5. 如何确保多个 Payload 属于同一可信发布版本？
6. 如果 Header 正确而 Pageable Payload 来自旧版本，会发生什么？
7. 安全团队应该验证的是哪些组合，而不只是单个文件？

参考分析方向

应把 Header、Payload、Firmware Loader、认证策略和内存布局视为一个版本化的整体。需要考虑格式协商、镜像完整性、跨文件一致性和 Anti-Rollback。不能以“Bootloader 能读取新的 Magic/Version”为唯一成功判据。

## **Question 11 — 如果你是 OP-TEE Maintainer，怎样审查一个 Linker Script Patch？**

**难度：极高 · Code Review / System Engineering**

有一份 Patch 修改：

```bash
.rodata : {
    ...
}
```

理由是：

“优化 Section Alignment，将 OP-TEE Binary 减少 12 KiB。”

Patch 的作者表示：

- 已成功编译。
- 已在一个 QEMU 环境启动。
- 未观察到异常。

你是否愿意批准？

请设计一套完整的 Review Checklist。

至少包含：

- 链接结果差异。
- W^X 约束。
- Section Boundary。
- Page Hash。
- Relocation。
- Pager Enabled/Disabled。
- ASLR。
- Init Function。
- Cross-Architecture。
- Runtime Mapping。
- Regression Testing。
- 真实优化收益和可维护性。

参考分析方向

重点不是立即拒绝优化，而是要求证明优化不会破坏不变量。必须明确节省的 12 KiB 属于 File Size、Virtual Address Space 还是 Resident Physical Memory；这些指标的工程价值可能完全不同。同时应验证涉及权限与分页边界的配置组合。

## **Question 12 — 重新设计 OP-TEE Binary Format，你会怎么做？**

**难度：架构设计题 · Principal/Staff Level**

这是本章最开放，也最有价值的一道题。

假设完全不受当前 V1/V2 Header 格式限制。

需要为新一代多架构 TEE OS 设计一种 Image Format，要求：

- 同时支持 Arm64 与 RISC-V RV64。
- 支持多个 Secure Payload。
- 支持 PIE/ASLR。
- 支持分离式加载和按需分页。
- 支持 Secure Boot。
- 支持 Measured Boot。
- 支持多级固件加载器。
- 支持部分镜像升级。
- 限制 Boot Firmware 的解析复杂度。
- 避免不必要地增大 Secure TCB。

请给出：

1. Image Format 结构。
2. Header 与 Payload Descriptor。
3. 加载地址约定。
4. Authentication / Measurement Metadata。
5. Relocation Metadata。
6. Memory Permission Metadata。
7. Versioning 与 Rollback Policy。
8. 如何应对 Malformed Image。
9. Bootloader 和 TEE Core 各自负责什么。
10. 为什么选择这个架构，而不是直接把 ELF 交给 Secure Firmware 加载？

参考分析方向

这道题没有唯一答案。一个成熟方案需要在格式表达能力、Loader 复杂度、可信解析器规模、升级灵活性和安全可验证性之间进行权衡。关键是提出可验证的不变量，例如 Payload 不重叠、所有执行内容都受认证、地址范围经过检查、权限在控制权交接前确定、Relocation 受限制、版本与授权主体绑定。

# **5.15 本章的最终知识模型**

完成本章后，应该形成这样一套心智模型：

```
                  Source Code
                       |
                       v
                Compiler / Assembler
                       |
                       v
                  Object Files
                       |
                       v
              Linker + Linker Script
                       |
          +------------+------------+
          |            |            |
          v            v            v
     Symbols       Sections     Relocations
          |            |            |
          +------------+------------+
                       |
                       v
                    tee.elf
                       |
                       v
                Image Generator
                       |
          +------------+------------+
          |            |            |
          v            v            v
        Header       Init       Pageable
          |            |            |
          +------------+------------+
                       |
                       v
             Secure Boot / Loader
                       |
                       v
               Physical Memory
                       |
                       v
             Early OP-TEE Runtime
                       |
          +------------+------------+
          |            |            |
          v            v            v
     Relocation       MMU         Pager
          |            |            |
          +------------+------------+
                       |
                       v
                 Running OS
```

这条链上有几项必须始终成立的核心不变量。

| Invariant                  | 核心要求                                                        |
| -------------------------- | ----------------------------------------------------------- |
| Entry Correctness          | Firmware 必须以符合约定的 CPU 状态、入口和内存布局交接                          |
| Address Correctness        | Link Address、Load Address、Runtime Mapping 和 Relocation 必须一致 |
| Initialization Correctness | 所有运行时数据在首次使用前必须完成正确初始化                                      |
| Residency Correctness      | Fault/Pager 必需的代码和资源必须始终可访问                                 |
| Memory Protection          | Section Layout 与最终 MMU/SoC Permission 必须共同满足隔离要求            |
| Integrity                  | 运行时使用的代码与数据必须符合已建立的可信验证模型                                   |
| Lifetime Correctness       | Init、Pageable、Heap、Stack 等内存复用不能破坏尚存活的引用                    |
| Compatibility              | Image Format、Loader、Firmware 和运行时必须遵守同一版本化契约                |

## **最终总结**

我希望 Chapter 05 留下的最重要认识是：

**Linker Script 是 OP-TEE 内核架构的一部分，而不是一个简单的编译配置文件。**

它决定的不只是函数地址，还影响 Code Residency、初始化生命周期、MMU Permission、Pager、ASLR、内存容量，以及上一级 Firmware 能否正确加载整个安全内核。

对一般应用程序来说，Linker 和 Loader 经常被操作系统隐藏起来。

但对于 OP-TEE 这样的 Firmware/OS，Linker Layout 与 Boot Protocol、Memory Isolation、Runtime Security 是直接耦合的。

当你未来修改 OP-TEE 的启动代码、MMU、Pager、Crypto Backend 或新增一个 Early Service 时，就应该习惯性地检查这条链上的每一个关键约束。

**本章最值得真正动手完成的是实验二、实验三和 Question 01、06、08、11、12。** 这些内容能够将 ELF/Linker 知识转化为面对真实复杂固件问题时的系统分析能力。

下一章 Chapter 06 将从完整 OS 生命周期的角度，把本章的 Binary/Boot 基础与 Thread、TA Loader、VM、Syscall、RPC 和 Storage 等运行期模块连接起来。
