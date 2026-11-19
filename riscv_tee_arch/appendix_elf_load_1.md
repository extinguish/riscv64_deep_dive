# OP-TEE ldelf：为什么 Loader 嵌入 Kernel，却运行在 User Space？

你的理解基本正确。`scripts/gen_ldelf_hex.py` 确实用于把编译好的 `ldelf.elf` 转换成 C 语言字节数组，然后作为 OP-TEE Core 的一部分参与链接。

但这里有一个关键区别：

ldelf 的代码作为数据存储在 Kernel Image 中，并不意味着这些代码运行时必须以 Kernel Privilege 执行。

OP-TEE 在 TA 初始化阶段，会把嵌入 Core 的 ldelf 代码复制到新建的用户态地址空间，建立对应的用户执行权限，然后通过 AArch64 `ERET` 从 S-EL1 切换到 S-EL0 执行。

我对照了当前 OP-TEE 主线中的 `gen_ldelf_hex.py`、`ldelf_loader.c` 和官方 TA 加载架构文档。下面按照 Build Time → Load Time → Execution Time 展开。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



## 一、先看整个架构

Build Time

编译主机

`ldelf/*.c` + `start_a64.S`

`ldelf.elf`

`scripts/gen_ldelf_hex.py`

提取 PT_LOAD，生成 C 数组及入口信息

`ldelf_hex.c` → `tee.elf` / `tee.bin`

Run Time

Secure World

OP-TEE Core — S-EL1

保存 ldelf_data 原始字节

`ldelf_load_ldelf()`

创建用户映射、复制代码、配置权限

设置 User PC / SP / PSTATE，执行 ERET

ldelf — S-EL0

在 User Space 中执行

自重定位 → 解析 TA ELF → 映射 → 重定位

TA — S-EL0

TA Runtime 开始执行

可以用三个概念理解它：

| 概念                   | ldelf 的实际情况                       |
| -------------------- | --------------------------------- |
| Build-time Ownership | ldelf 是独立编译、链接的 ELF 程序            |
| Storage Ownership    | ldelf 的初始化字节被嵌入 OP-TEE Core Image |
| Execution Privilege  | ldelf 被 Core 映射成用户程序，运行于 S-EL0    |

注意：这里的 User Space 是 Secure World 的 S-EL0，不是 Normal World 的 Linux EL0。

## 二、gen_ldelf_hex.py 具体做了什么？

源文件：[scripts/gen_ldelf_hex.py](https://github.com/OP-TEE/optee_os/blob/master/scripts/gen_ldelf_hex.py?utm_source=chatgpt.com)

### 2.1 输入不是普通的 HEX 文件

脚本接收：

```
python3 scripts/gen_ldelf_hex.py \
    --input ldelf.elf \
    --output ldelf_hex.c
```

这里的输出虽然叫 `hex`，实际上并不是 Intel HEX 或 Motorola S-record 这样的固件烧录格式。

它生成的是可被 C 编译器编译的 C 源文件，其中的机器码字节以 `0x..` 常量表示。

### 2.2 从 ELF 中提取 PT_LOAD

脚本中的核心逻辑是：

```
load_segments = [    s for s in elffile.iter_segments()    if s['p_type'] == 'PT_LOAD']
```

这意味着它不是简单地把完整 `ldelf.elf` 转成文本。

它有意识地提取 ELF 中的 Loadable Segments，只保留运行时需要的内容，同时根据 Segment 布局处理填充和零初始化区域。

它还检查 Segment 的排列约定：不可写的加载段在前，可写的加载段在后。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



### 2.3 生成的 C 文件中包含什么？

主线源码中的核心生成语句是：

```
outf.write(b'const uint8_t ldelf_data[%d]' %           round_up(load_size, 4096))
```

并带有 4KB 对齐属性。

还会输出：

```
const unsigned int ldelf_code_size = ...;
const unsigned int ldelf_data_size = ...;
const unsigned long ldelf_entry = ...;
```

其中 `ldelf_entry` 来自原始 ELF Header 的 `e_entry`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



为了直观理解，假设生成结果类似：

```
/* ldelf_hex.c：示意，非实际生成数据 */

const uint8_t ldelf_data[] __aligned(4096) = {
    0x1f, 0x20, 0x03, 0xd5,
    0xfd, 0x7b, 0xbf, 0xa9,
    /* ...其余机器码与初始化数据... */
};

const unsigned int ldelf_code_size = 0x5000;
const unsigned int ldelf_data_size = 0x2000;
const unsigned long ldelf_entry = 0x400;
```

当 Core 编译并链接这个 C 文件时：

```
tee.elf
   |
   +-- OP-TEE Kernel .text
   +-- OP-TEE Kernel .rodata
   |      |
   |      +-- ldelf_data[]  ← ldelf 的初始化映像
   |
   +-- OP-TEE Kernel .data
   +-- OP-TEE Kernel .bss
```

因此，ldelf 的机器码字节现在确实成为了 OP-TEE Core 映像的一部分。

但是这些字节此时只是 Core 中保存的数据，并不是已在用户态运行的代码。

`gen_ldelf_hex.py` 解决的是 ldelf 如何嵌入 Kernel Image 的问题，而不是它如何进入 User Mode 的问题。

后者由 `ldelf_loader.c` 和 AArch64 线程切换代码解决。

## 三、Core 如何把 ldelf 放入 User Space？

这是第二个问题的核心。

源码：[core/kernel/ldelf_loader.c](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/ldelf_loader.c?utm_source=chatgpt.com)

关键函数：

```
TEE_Result ldelf_load_ldelf(struct user_mode_ctx *uctx);
```

其中 `uctx` 表示用户态执行上下文，包含虚拟内存映射、入口地址、Stack 等执行状态。

### 3.1 为 ldelf 分配用户地址空间

实际代码包含：

```
res = alloc_and_map_fobj(
    uctx,
    ldelf_code_size,
    TEE_MATTR_PRW,
    VM_FLAG_LDELF,
    &code_addr);
```

这里 `alloc_and_map_fobj()` 内部使用：

```
fobj_ta_mem_alloc(...)
mobj_with_fobj_alloc(...)
vm_map(...)
```

分配底层内存对象并建立虚拟地址映射。

注意初始权限：

```
TEE_MATTR_PRW
```

意味着这段内存先被赋予 Privileged Read/Write 权限，方便 Core 写入 ldelf 的机器码。

随后再建立 ldelf 的可写数据映射：

```
rw_addr = ROUNDUP(code_addr + ldelf_code_size,
                  SMALL_PAGE_SIZE);

res = alloc_and_map_fobj(
    uctx,
    ldelf_data_size,
    TEE_MATTR_URW | TEE_MATTR_PRW,
    VM_FLAG_LDELF,
    &rw_addr);
```

这两段映射都属于 `uctx` 管理的地址空间。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 3.2 把 Kernel 保存的代码复制进去

这部分最直接地回答了你的疑问。

```
vm_set_ctx(uctx->ts_ctx);

memcpy((void *)code_addr,
       ldelf_data,
       ldelf_code_size);

res = copy_to_user(
    (void *)rw_addr,
    ldelf_data + ldelf_code_size,
    ldelf_data_size);
```

发生了什么？

Kernel 映像中的源数据 — S-EL1

`ldelf_data[]`

位于 OP-TEE Core 的初始化映像中，保存原始机器码与数据

Core 分配内存并复制

新建的 ldelf 用户态映像 — S-EL0

code_addr

代码及只读区域，最终用户权限 RX

rw_addr

可写数据、BSS 等，用户权限 RW

stack_addr

独立的用户态 Stack

这不是修改 Kernel 中原来那份 `ldelf_data[]` 的执行权限，然后直接让 S-EL0 跳进去。

这里实际创建了一个新的用户映像，并把 Core 保存的初始化内容复制到对应内存中。

`vm_set_ctx()` 让 Core 在处理用户虚拟地址时使用正确的地址空间上下文；而 `copy_to_user()` 则提供受控的用户内存复制。

### 3.3 修改代码页权限，使 S-EL0 能执行

复制完成以后：

```
prot = TEE_MATTR_URX;

if (IS_ENABLED(CFG_CORE_BTI))
    prot |= TEE_MATTR_GUARDED;

res = vm_set_prot(
    uctx,
    code_addr,
    ROUNDUP(ldelf_code_size, SMALL_PAGE_SIZE),
    prot);
```

这里才是让 ldelf 代码真正成为用户可执行映射的关键动作。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



从 Armv8-A MMU 角度理解，`TEE_MATTR_URX` 最终需要转化为合适的 Stage-1 页表描述符，包括用户读权限和执行权限控制。

在典型 AArch64 Secure EL1/EL0 配置下：

- S-EL0 可以取指执行这段代码。
- S-EL0 不可以随意修改已经保护为只读的代码页。
- S-EL0 不能获得 S-EL1 Kernel 的特权访问能力。
- Kernel 的私有代码与数据仍受页表权限和地址空间策略保护。

注意，是否存在 Core 映射、以及是否进一步移除 EL0 期间的完整 Core 映射，还受到 `CFG_CORE_UNMAP_CORE_AT_EL0` 等配置影响。

因此真正提供隔离的并不是“代码从 Kernel 复制到了 User Space”这个动作本身，而是内存映射属性、CPU 当前异常级别和 Kernel 管理的访问控制。

## 四、ldelf 的入口地址如何确定？

在刚才的 `ldelf_load_ldelf()` 中，还有一行：

```
uctx->entry_func = code_addr + ldelf_entry;
```

其中：

- `code_addr`：本次映射的用户虚拟地址基准。
- `ldelf_entry`：`gen_ldelf_hex.py` 从原始 ELF 提取的 `e_entry`。
- `uctx->entry_func`：CPU 最终进入 ldelf 时需要使用的入口 VA。

这个计算依赖 ldelf 的链接布局约定，使原始入口可以作为相对于映像基址的偏移使用。

假设：

```
code_addr   = 0x40000000
ldelf_entry = 0x00000100

entry_func  = 0x40000100
```

这里要进一步区分：

入口地址确定，不等于 ldelf 已经执行。

它只是告诉 Core：进入用户态时，PC 应该从哪里开始。

真正的 CPU 权限切换还需要下一步。

## 五、AArch64 如何让 ldelf 真正运行在 S-EL0？

源码中的下一关键函数是：

```
TEE_Result ldelf_init_with_ldelf(...);
```

它准备参数之后调用：

```
res = thread_enter_user_mode(
    (vaddr_t)arg, 0, 0, 0,
    usr_stack,
    uctx->entry_func,
    is_32bit,
    &panicked,
    &panic_code);
```

此时大致需要构造：

```
User x0 = ldelf_arg 用户态指针
User SP = usr_stack
User PC = uctx->entry_func
User PSTATE = S-EL0 的目标状态
```

同时，源码还临时设置：

```
sess->handle_scall = scall_handle_ldelf;
```

使 ldelf 执行期间的系统调用进入对应的处理路径。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 5.1 追踪到真正的汇编

打开：

[core/arch/arm/kernel/thread_a64.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/arm/kernel/thread_a64.S?utm_source=chatgpt.com)

重点关注：

```
__thread_enter_user_mode:
```

核心汇编包含：

```
load_xregs x0, THREAD_CTX_REGS_SP, 1, 3

msr sp_el0, x1
msr elr_el1, x2
msr spsr_el1, x3
```

这里的 `x1`、`x2`、`x3` 是从线程保存的寄存器上下文中加载的值。

这些指令分别完成：

| 指令                 | 实际功能                     |
| ------------------ | ------------------------ |
| `msr sp_el0, x1`   | 指定 S-EL0 的 Stack Pointer |
| `msr elr_el1, x2`  | 指定异常返回之后的 PC             |
| `msr spsr_el1, x3` | 指定异常返回后的 PSTATE          |

随后：

```
b eret_to_el0
```

进入实际的异常返回路径。

该路径还会处理相应的用户地址空间、ASID 和可选的 Core 映射保护，最终通过异常返回指令进入 S-EL0。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



### 5.2 为什么不能直接使用 BR？

这点对于理解 Arm TrustZone 与 User/Kernel Privilege 很重要。

假设现在：

```
Current EL = S-EL1
x0 = ldelf entry address
```

如果直接：

```
br x0
```

CPU 只是修改 PC，仍然运行在 S-EL1。

这样就会让 ldelf 以 Kernel 权限执行，失去了用户态隔离的目的。

而通过：

```
msr elr_el1, x0
msr spsr_el1, x1
eret
```

这样的机制（示意），CPU 可以从 S-EL1 异常返回到 S-EL0。

因此：

让 ldelf 成为 User Mode 程序的决定性因素，是目标地址空间权限和 `ERET` 恢复的 CPU 执行状态，而不是这个程序最初存放在什么文件或哪一段内存里。

## 六、ldelf 进入 S-EL0 后，为什么仍需要 Self-Relocation？

因为它现在已经处于新的运行虚拟地址。

虽然 `gen_ldelf_hex.py` 保留了 ldelf 的可加载代码和数据，但它并没有在 Build Time 为某个最终运行地址完成所有 Load-time Relocation。

所以进入 ldelf 后，入口汇编还需要修正部分指针。

源码：

[ldelf/start_a64.S](https://github.com/OP-TEE/optee_os/blob/master/ldelf/start_a64.S?utm_source=chatgpt.com)

其 `_ldelf_start` 包含：

```
adr x4, _ldelf_start
```

计算运行时加载偏移。

然后逐个读取 `Elf64_Rela`，针对 `R_AARCH64_RELATIVE` 执行：

```
add x7, x7, x4
add x9, x9, x4
str x9, [x7]
```

即：

\\[ \*(B+r\\\_offset)=B+r\\\_addend \\]

这正是此前讲过的 ELF Relative Relocation。

特别值得注意的是，源码明确要求这些初始自重定位发生在 Read/Write Memory，而不需要修改只读、可执行代码页。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此，完整执行路径是：

```
OP-TEE Core / S-EL1
    |
    +-- 保存 ldelf 原始字节
    +-- 分配用户内存
    +-- 拷贝 ldelf
    +-- 设置用户执行权限
    +-- 设置入口 PC、Stack
    |
    v
ERET
    |
    v
_ldelf_start / S-EL0
    |
    +-- 自重定位
    +-- 调用 ldelf()
    +-- 解析 TA ELF
    +-- 通过受控 Syscall 请求 Kernel 映射内存
    +-- 加载并重定位 TA
    |
    v
返回 Core
    |
    +-- 获取 TA Entry、Stack 等信息
    |
    v
后续进入 TA / S-EL0
```

## 七、这样做为什么更安全？

这里要纠正一个容易产生的理解：

把 ldelf 放在 User Mode 中，并不意味着 ELF Loading 天然安全；它意味着最复杂、最容易接触不可信输入的部分，不必拥有 Kernel Privilege。

ELF Parser 需要处理大量复杂的数据：

- 不可信的 Header、Program Header。
- Section 和 Segment 边界。
- 符号表、动态链接信息。
- 各种 Relocation Type。
- 整数溢出、越界偏移。
- 地址映射和内存权限需求。

假设一个恶意 ELF 利用了 ldelf 中的解析漏洞。

如果 ldelf 在 S-EL1 执行，漏洞可能直接破坏 Core 的特权内存。

如果 ldelf 在 S-EL0 执行，攻击者首先获得的是受限的用户态执行能力。进一步破坏 Secure Kernel 则还需要突破系统调用、内存权限或其他信任边界。

这也解释了为什么 OP-TEE 把涉及特权资源的操作保留在 Core 中，由 ldelf 通过系统调用请求。

而且 Core 并不会无条件信任 ldelf 返回的数据。比如 `ldelf_init_with_ldelf()` 在收到用户态返回的 TA Flags 后，还会重新检查部分标志的合法性。源码注释明确说明：由于 ELF Loader 在用户态执行，因此不能完全信任其检查结果。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



另一个容易忽略的细节是：ldelf 通常不会在完成初始加载之后立即被完全销毁。 官方文档指出，它还会保留在 TA 的用户地址空间中，用于后续 `dlopen()` 等操作及部分调试、错误处理功能。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 八、建议你实际验证的四个源码断点

为了把这条路径彻底理解，建议在 AArch64 OP-TEE + QEMU/GDB 环境中，按照下面顺序观察：

| 断点                         | 重点检查                                                  |
| -------------------------- | ----------------------------------------------------- |
| `ldelf_load_ldelf()`       | `ldelf_data`、`code_addr`、`rw_addr`、`uctx->entry_func` |
| `ldelf_init_with_ldelf()`  | 用户 Stack、入口、参数和系统调用处理方式                               |
| `__thread_enter_user_mode` | `ELR_EL1`、`SPSR_EL1`、`SP_EL0` 以及地址空间切换                |
| `_ldelf_start`             | 当前 EL、PC、Load Bias、RELA 修正前后的指针                       |

尤其推荐在 `_ldelf_start` 断点处检查：

```
info registers pc sp x0 x4
x/8i $pc
```

然后比较当前 PC 与前面 `code_addr + ldelf_entry` 的计算结果。

如果能够在这里亲眼确认 CPU 已经运行于 S-EL0，并追踪到自重定位循环，那么对 ldelf 这个机制的理解就会非常扎实。

## 最后总结

你的两个问题实际上指向 OP-TEE 的同一个架构设计：

Build Time 把 ldelf 作为数据嵌入 Core；Run Time 由 Core 创建独立的用户态映像；Execution Time 通过 AArch64 异常返回机制以 S-EL0 权限执行。

`gen_ldelf_hex.py` 负责第一阶段，`ldelf_load_ldelf()` 负责第二阶段，`thread_enter_user_mode()` 及 `thread_a64.S` 负责第三阶段，而 `ldelf/start_a64.S` 再完成运行时自重定位。

还有一个特别值得继续深入的问题：

既然 ldelf 和 TA 最终都运行在 S-EL0，甚至处于同一个 TA 用户虚拟地址空间中，那么 ldelf 如何保护自己不被 TA 修改，以及 TA 如何通过受控接口使用 ldelf 的 `dlopen()` 能力？

这个问题会进一步涉及 `VM_FLAG_LDELF`、用户态内存映射管理、Loader 与 TA 的信任边界，比单纯理解如何启动 ldelf 更接近 OP-TEE 的真实安全架构。
