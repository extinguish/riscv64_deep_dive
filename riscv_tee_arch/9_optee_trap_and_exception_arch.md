# Chapter 09 — RISC-V Trap / Exception Architecture in OP-TEE

RISC-V OP-TEE Architecture & Porting · 核心内核架构系列 · Staff Engineer 深度教程

## 本章目标与定位

本章要解决的核心问题是：

当运行在 RISC-V U-mode 的 Trusted Application，或者运行在 S-mode 的 OP-TEE Kernel 发生异常、中断、系统调用时，CPU、汇编入口和 OP-TEE 内核分别负责什么？它们如何配合，才能保证执行正确性和安全隔离？

对有 Arm TrustZone、TF-A 和 RTOS Kernel 开发经验的工程师来说，理解 RISC-V Trap 指令和 CSR 并不难。真正困难的是把它们放到 OP-TEE 的执行模型中，理解以下关系：

- Hardware Trap Entry：CPU 自动完成哪些状态转换？
- Assembly Trap Prologue：为什么必须先解决寄存器和栈的保存问题？
- Exception Dispatch：如何区分 TA Syscall、Page Fault、Interrupt 和 Kernel Fatal Exception？
- Trusted Thread Context：异常现场、Thread Context、TA Context 之间是什么关系？
- Privilege and Security Boundary：U-mode TA 发生异常，为什么不能破坏 S-mode TEE Kernel？
- Exception Return：如何恢复被打断的执行现场，或者安全地终止故障 TA？

本章按照原大纲的九个主题完整展开，并增加完整执行案例、源码研究方法、实验和 Staff Engineer 面试题。

本章源码基线与范围

截至 2026 年 10 月 28 日，已对照 OP-TEE `master` 分支中的 RISC-V 实现进行核查。实际工程使用时，应固定具体 commit，因为这部分代码仍在演进。

最重要的发现是：当前 RISC-V Trap 主入口主要位于 `core/arch/riscv/kernel/thread_rv.S` 的 `thread_trap_vect`，而不是 Boot Entry 所在的 `entry.S`。异常处理的 C 代码主要在 `abort.c` 与 `thread_arch.c`。

当前代码通过 `CSR_X*`、`XRET` 等架构抽象同时支持不同配置。下文以常见的 RV64 S-mode OP-TEE + U-mode TA 作为主要讲解对象，另外单独分析 M-mode 及 OpenSBI Domain 的边界。

源码依据：

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+3

## 本章知识路线

TA / OP-TEE Kernel 正在运行

U-mode / S-mode

1\. Hardware Trap

sepc / scause / stval / sstatus / stvec

2\. Assembly Trap Entry

sscratch / tp / Stack / Register Save

3\. Exception Classification

Interrupt / Syscall / Abort

Syscall

TEE Service

Page Fault

Recovery / TA Panic

Interrupt

IRQ / Yield

4\. Restore / Resume / Terminate

sret · Resume Context · Abort TA · Kernel Panic

# 9.1 RISC-V Supervisor Trap Architecture：先理解硬件到底做了什么

## 9.1.1 Trap、Exception、Interrupt 的本质区别

在 RISC-V 中，Trap 是一个统一的控制流转移机制。

它主要包含两类事件：

| 类型                     | 典型来源                                            | 是否与当前指令同步 |
| ---------------------- | ----------------------------------------------- | --------- |
| Synchronous Exception  | Illegal Instruction、Page Fault、ECALL、Breakpoint | 是         |
| Asynchronous Interrupt | Timer、Software、External Interrupt               | 否         |

例如，运行在 U-mode 的 TA 执行：

```
ld a0, 0(a1)
```

假设 `a1` 指向一个当前地址空间无法读取的页面。

CPU 检查地址翻译与权限后发现无法完成本次 Load，于是产生异常。

如果该异常被配置为交给 S-mode 处理，CPU 就会进入 S-mode Trap Handler。

注意：产生 Page Fault 不意味着一定会进入 M-mode，也不意味着一定会发生 OpenSBI Domain Switch。

这是本章首先要建立的概念。

## 9.1.2 Trap Target 与 Delegation

以没有启用 H Extension 的普通 M/S/U 系统为例：

```
                 Exception / Interrupt
                          |
                          v
                    Delegation?
                     /         \
             Not delegated    Delegated
                  |               |
                  v               v
               M-mode          S-mode
               mtvec           stvec
                  |               |
             M Trap Handler   S Trap Handler
```

M-mode 通过 `medeleg` 和 `mideleg` 决定哪些低特权级事件可以交由 S-mode 处理。

例如：

- `medeleg[8]` 可用于将 U-mode ECALL 委托给 S-mode。
- `medeleg[12]`、`[13]`、`[15]` 对应 Instruction、Load、Store/AMO Page Fault。
- `mideleg` 控制可委托中断的路由。

需要记住两个非常重要的规则：

规则一： M-mode 产生的异常不会因为 `medeleg` 设置而被向下委托给 S-mode。

规则二： 当 S-mode 正在执行时，能够由 S-mode 自己处理的 Trap 可以直接进入其 Trap Handler，不需要先进入 M-mode 再返回。

因此，对于普通 TA Syscall：

```
OP-TEE TA (U-mode)
      |
      | ecall
      v
OP-TEE Kernel (S-mode)
```

在正确配置了异常委托的情况下，不需要经过 OpenSBI。

但如果 Trap 未委托给 S-mode，CPU 会进入 M-mode；由 M-mode Firmware 按自身策略处理，不能假定它会自动转发给 OP-TEE。

另外，当启用 Hypervisor Extension 时，还需要考虑 VS-mode、HS-mode 的 Delegation 与虚拟化 Trap 控制。这不是本章主要平台模型，但分析任何真实 SoC 时都必须先确定是否启用了 H Extension。

## 9.1.3 S-mode Trap 相关 CSR

下表是本章最重要的一组寄存器。

| CSR        | 全称                                            | 核心职责                 |
| ---------- | --------------------------------------------- | -------------------- |
| `sstatus`  | Supervisor Status                             | Trap 前后特权级及中断状态      |
| `stvec`    | Supervisor Trap Vector                        | Trap Handler 入口地址及模式 |
| `sscratch` | Supervisor Scratch                            | Trap 入口临时上下文         |
| `sepc`     | Supervisor Exception PC                       | 被打断执行的位置             |
| `scause`   | Supervisor Trap Cause                         | 异常或中断原因              |
| `stval`    | Supervisor Trap Value                         | Fault Address 或附加信息  |
| `sie`      | Supervisor Interrupt Enable                   | S-mode 中断源使能         |
| `sip`      | Supervisor Interrupt Pending                  | S-mode 中断待处理状态       |
| `satp`     | Supervisor Address Translation and Protection | 地址翻译模式、ASID、页表根      |

规范参考：[RISC-V Privileged CSR Definitions](https://docs.riscv.org/reference/isa/priv/priv-csrs.html?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



### `stvec`：为什么需要 BASE 和 MODE？

`stvec` 包含两个部分：

```
stvec
+--------------------------------------+------+
|                 BASE                 | MODE |
+--------------------------------------+------+
                                       2 bits
```

常见模式有：

| MODE | 名称       | Trap 入口                                           |
| ---- | -------- | ------------------------------------------------- |
| 0    | Direct   | `BASE`                                            |
| 1    | Vectored | Exception → `BASE`；Interrupt → `BASE + 4 × cause` |

注意：Vectored 模式主要针对 Interrupt Vector；同步异常仍然从 `BASE` 进入。

如果你采用统一 Trap 入口，然后在汇编里读 `scause` 分发，那么 Direct 模式通常更直观。

如果使用 Vectored 模式，需要保证向量地址对齐、每个槽位的跳转代码以及中断号和入口布局相匹配。

### `sepc`：为什么它不是普通的返回地址？

当 TA 执行：

```
ecall
```

产生 Trap 时：

```
sepc = ECALL 指令地址
```

不是 ECALL 的下一条指令地址。

所以如果 Syscall Handler 处理完毕后直接执行 `sret`，又没有调整 `sepc`，就可能再次执行同一条 ECALL，形成死循环。

对于 32-bit ECALL，正常返回需要将保存的 EPC 前移 4 字节。

但对 Page Fault，不能无条件进行 `sepc += 4`，因为修复页表后可能需要重新执行原来的访存指令。

### `scause`：如何区分 Interrupt 与 Exception？

在 RV64 中：

```
scause[63]     = Interrupt Flag
scause[62:0]   = Exception / Interrupt Code
```

典型同步异常编码：

| Cause Code | 含义                             |
| ---------- | ------------------------------ |
| 0          | Instruction Address Misaligned |
| 1          | Instruction Access Fault       |
| 2          | Illegal Instruction            |
| 3          | Breakpoint                     |
| 4          | Load Address Misaligned        |
| 5          | Load Access Fault              |
| 6          | Store/AMO Address Misaligned   |
| 7          | Store/AMO Access Fault         |
| 8          | Environment Call from U-mode   |
| 9          | Environment Call from S-mode   |
| 12         | Instruction Page Fault         |
| 13         | Load Page Fault                |
| 15         | Store/AMO Page Fault           |

典型中断编码：

| Interrupt Code | 含义                            |
| -------------- | ----------------------------- |
| 1              | Supervisor Software Interrupt |
| 5              | Supervisor Timer Interrupt    |
| 9              | Supervisor External Interrupt |

例如：

```
scause = 0x000000000000000d
```

表示 Load Page Fault。

而：

```
scause = 0x8000000000000009
```

表示 Supervisor External Interrupt。

在软件中可以这样解码：

```
uint64_t cause = read_csr(scause);

bool is_interrupt = (cause >> 63) != 0;
uint64_t code = cause & ~(1ULL << 63);
```

这里仅演示 RV64；跨 RV32/RV64 实现应根据 XLEN 使用对应宽度。

### `stval`：Fault Address 不总是有效

`stval` 经常用于保存 Faulting Virtual Address，例如 Load Page Fault。

但是不能机械地把任何 `stval` 都解释成访问地址。

对于不同的 Exception，`stval` 可能包含不同的信息，也可能为零；某些 Illegal Instruction 场景下还可以包含指令相关信息。

因此正确的解析顺序是：

1. 判断 `scause` 是否为 Interrupt。
2. 解析 Cause Code。
3. 根据 Cause 类型决定 `stval` 的语义。

### `sstatus`：Trap 进入前后的状态转换

我们重点关注：

| 位      | 含义                                   |
| ------ | ------------------------------------ |
| `SIE`  | Supervisor Interrupt Enable          |
| `SPIE` | Supervisor Previous Interrupt Enable |
| `SPP`  | Supervisor Previous Privilege        |
| `SUM`  | Supervisor User Memory Access        |
| `FS`   | Floating-point State                 |
| `VS`   | Vector State                         |

假设 Trap 发生前：

```
Privilege = U-mode
SIE       = 1
```

在进入 S-mode Trap 时，硬件执行的关键状态变化可概括为：

```
SPIE <- SIE
SIE  <- 0
SPP  <- U-mode
sepc <- Faulting PC
scause <- Trap Cause
PC <- stvec
```

注意 `SPIE` 保存的是先前 S-mode 中断使能状态；U-mode 是否允许相应中断递送，还受到特权级关系等规则影响。

`SIE=0` 并不意味着所有硬件中断都已经被全局屏蔽：它不会阻止更高特权级自身能够接收的中断。

当执行 `sret` 时：

```
Privilege <- SPP
SIE       <- SPIE
SPIE      <- 1
SPP       <- 0
PC        <- sepc
```

这里的 `SPP <- 0` 是正常 SRET 的状态更新，而非软件额外清理。

从 Arm 的角度理解：

- `sepc` 类似部分场景下的 `ELR_EL1`。
- `scause` 提供类似 `ESR_EL1` 中 Exception Classification 的信息。
- `stval` 与某些场景下的 `FAR_EL1` 具有类似用途。
- `sret` 与 Arm `eret` 都属于异常返回机制。

但 RISC-V 把不同信息分散在多组 CSR 中，其编码和状态保存规则不能简单一一对应。

# 9.2 RISC-V S-mode Trap Entry Assembly：从硬件入口到 OP-TEE 汇编代码

现在进入本章最重要的源码部分。

## 9.2.1 为什么 Trap Entry 必须用汇编实现？

假设 OP-TEE 的某个 User TA 正在执行：

```
li a0, 0x1234
li a1, 0x5678
ld a2, 0(a1)
```

最后一条 `ld` 因无效地址产生 Load Page Fault。

CPU 根据异常委托配置进入 S-mode，并开始执行 `stvec` 指向的 Trap Handler。

但在这个瞬间，有三个关键问题：

第一，CPU 不会自动保存全部 GPR。

`a0`、`a1`、`a2`、`ra`、`sp`、`tp` 等寄存器仍然保存着 TA 的值。

第二，CPU 不会自动切换到 OP-TEE Kernel Stack。

虽然已经进入 S-mode，但 `sp` 仍指向 TA 的 User Stack。

第三，CPU 不会自动切换页表。

U-mode → S-mode 的 Trap 不会自动修改 `satp`。因此 Trap Entry 必须能够在当前地址空间下被取指，并且能够安全访问入口所需的内核数据。

这意味着：

```
void trap_handler(void)
{
    // 不可以假设已经有安全的 C 运行环境
}
```

不能直接作为硬件 Trap 的原始入口。

即使 C 编译器可以产生函数 Prologue，函数本身也要使用 `sp`、`gp` 和其他寄存器。在这些状态被正确初始化或保存之前，普通 C ABI 并不成立。

所以真正的执行链应当是：

```
Hardware Trap
      |
      v
Assembly Entry
      |
      +-- Identify source privilege
      +-- Obtain per-hart state
      +-- Select trusted stack
      +-- Save GPRs / CSRs
      +-- Establish kernel ABI
      |
      v
C Exception Handler
```

这个原则与你在 Cortex-M 中研究 PendSV，以及在 Arm64 中研究 Exception Vector 和 `SP_ELx` 的经验相通。

不过 Arm64 硬件提供了 SP_ELx 等栈相关机制，RISC-V 常规 Trap Entry 不具备同等的自动栈切换能力。

## 9.2.2 OP-TEE 的 Trap Vector 在哪里安装？

当前 OP-TEE RISC-V 代码中：

```
/* core/arch/riscv/kernel/thread_arch.c */

static vaddr_t get_trap_vect(void)
{
    return (vaddr_t)thread_trap_vect;
}

void thread_init_tvec(void)
{
    unsigned long tvec = get_trap_vect();

    write_csr(CSR_XTVEC, tvec);
    assert(read_csr(CSR_XTVEC) == tvec);
}
```

`thread_init_per_cpu()` 调用 `thread_init_tvec()`，然后初始化 `XSCRATCH`。

这里的 `CSR_XTVEC` 是一个编译期抽象：

```
#if defined(CFG_RISCV_M_MODE)
#define XRET mret
#elif defined(CFG_RISCV_S_MODE)
#define XRET sret
#endif
```

对于 S-mode Build：

```
CSR_XTVEC    -> stvec
CSR_XSCRATCH -> sscratch
CSR_XCAUSE   -> scause
CSR_XEPC     -> sepc
XRET         -> sret
```

对于 M-mode Build，则对应 M-mode 的 Trap CSR 和 `mret`。

这些映射可以在 `core/arch/riscv/include/riscv.h` 中找到。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



需要特别注意，`stvec` 是 per-hart CSR。

即使所有 hart 使用同一段 `thread_trap_vect` 代码，也必须确保每个可能运行 OP-TEE 的 hart 都已完成自己的 Trap Vector 和 Core-local 状态初始化。

这与多 hart 启动和热插拔直接相关。

## 9.2.3 `thread_trap_vect`：一段非常值得深入研究的汇编

当前 upstream 的入口代码如下：

```
FUNC thread_trap_vect , :
    csrrw tp, CSR_XSCRATCH, tp
    bnez  tp, 0f

    /* Read tp back */
    csrrw tp, CSR_XSCRATCH, tp
    j trap_from_kernel

0:
    /* Now tp is thread_core_local */
    j trap_from_user

thread_trap_vect_end:
END_FUNC thread_trap_vect
```

这段代码非常短，但体现了精巧的内核设计。

它解决的问题是：

> 没有安全的 Stack，也不能破坏 TA 的寄存器，怎样用尽可能少的指令判断 Trap 来自 U-mode 还是 S-mode？

答案是通过 `sscratch` 和 `tp` 的交换。

### 情况一：Trap 来自 User TA

假设 Trap 之前：

```
Privilege = U-mode

tp       = 0x0000000012345000
sscratch = 0x0000000080010000
```

其中：

- `tp` 是 TA 的 Thread Pointer。
- `sscratch` 保存 OP-TEE 当前 hart 的 `thread_core_local` 指针。

执行：

```
csrrw tp, sscratch, tp
```

后：

```
tp       = 0x0000000080010000
sscratch = 0x0000000012345000
```

此时：

```
bnez tp, 0f
```

分支成立。

进入：

```
trap_from_user
```

后，`tp` 已经是可信的 Core-local 指针，可以用于访问 OP-TEE 内核维护的 Per-Hart 数据结构。

原来的 TA `tp` 暂时保存在 `sscratch`，并不会丢失。

### 情况二：Trap 来自 OP-TEE Kernel

假设原本运行在 S-mode：

```
tp       = 0x0000000080010000
sscratch = 0
```

第一次交换后：

```
tp       = 0
sscratch = 0x0000000080010000
```

由于 `tp == 0`：

```
bnez tp, 0f
```

不跳转。

随后再次执行：

```
csrrw tp, sscratch, tp
```

恢复：

```
tp       = 0x0000000080010000
sscratch = 0
```

然后进入：

```
trap_from_kernel
```

因此我们可以归纳出 OP-TEE 的一个约定：

| 正在执行的环境       | `tp`               | `sscratch`         |
| ------------- | ------------------ | ------------------ |
| OP-TEE S-mode | Core-local Pointer | 0                  |
| TA U-mode     | TA Thread Pointer  | Core-local Pointer |

这里不是硬件自动判断 `SPP`，而是 OP-TEE 通过自己维护的 `sscratch` 状态进行快速入口分类。

这就引出一个非常重要的安全约束：

在将控制权交给 User TA 之前，必须正确设置 `sscratch`；返回 Kernel 运行时，也必须恢复 `sscratch=0` 的约定。

否则 Trap Entry 可能将 User Context 错误地解释为 Kernel Context，造成内核指针错误或严重异常。

对应真实源码包括：

- `thread_trap_vect`
- `__thread_enter_user_mode`
- `thread_resume`
- `trap_from_user`
- `trap_from_kernel`

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+2

## 9.2.4 Trap Entry 对地址空间有什么特殊要求？

这是很容易被忽视的问题。

假设 TA 正在运行：

```
satp = TA_A_PageTable
Privilege = U-mode
```

发生 Trap 后：

```
Privilege -> S-mode
PC        -> stvec
satp      -> TA_A_PageTable (unchanged)
```

那么问题来了：

如果 TA 页表中完全没有映射 `thread_trap_vect`，CPU 如何执行异常入口？

答案是：无法正常执行。

因此在典型的 OP-TEE 地址空间设计中，User TA 的执行映射需要同时支持 TA 的用户空间，以及处理 Trap 所需的 Supervisor 侧内核映射。

但 U-mode 不能随意访问这些内核页。

这要求页表正确使用：

```
PTE.U
PTE.R
PTE.W
PTE.X
```

其中 `PTE.U=0` 的 Supervisor 页面通常不能被 U-mode 直接访问。

而且，`SUM` 即使允许 S-mode 访问特定 User 页面，也不能使 U-mode 获得对 Supervisor 页面的访问权。

Trap Entry 所需的内存映射可以表达为：

```
Current satp
    |
    +-- User TA Text      U=1, RX
    +-- User TA Data      U=1, RW
    +-- User TA Stack     U=1, RW
    |
    +-- OP-TEE Kernel     U=0, kernel permissions
    +-- Trap Entry        U=0, RX
    +-- Trusted Stack     U=0, RW
    +-- Core-local Data   U=0, RW
```

这是安全页表布局的概念示意。实际权限、映射区域及是否使用不同内核映射，还应以目标构建配置为准。

这一点也解释了为什么：

Trap Entry 是否能够执行，与 `stvec` 配置正确、当前 `satp` 有效以及相关 PMP 权限正确，三者都有关系。

# 9.3 `sscratch`、Trap Frame、Stack Switching 与 Nested Exception

本节要回答一个比普通 Trap 处理更深入的问题：

当异常发生时，如何安全地保存执行现场？

## 9.3.1 为什么不能直接使用 TA Stack？

假设 TA 执行：

```
li sp, 0
ld a0, 0(sp)
```

发生 Load Page Fault。

如果 Trap Handler 直接：

```
addi sp, sp, -256
sd ra, 0(sp)
```

那么 `sp` 仍然属于 TA，甚至可能是非法地址。

结果可能是：

1. 第一个 Page Fault 进入 Trap Handler。
2. Trap Handler 使用非法的 TA Stack。
3. 保存寄存器时再次触发 Page Fault。
4. 第二个 Trap 覆盖原有 Trap CSR。
5. 原始异常现场丢失，系统无法可靠恢复。

因此安全的 Trap Handler 必须先选择可信 Stack。

## 9.3.2 OP-TEE 有哪些不同用途的 Stack？

当前 RISC-V 实现中，可以识别以下不同用途的 Stack：

| Stack                       | 主要用途                         | 典型状态                     |
| --------------------------- | ---------------------------- | ------------------------ |
| Temporary Stack             | 初始入口、短暂执行和部分中断处理             | Per-Hart                 |
| Abort Stack                 | 异常处理及故障现场保存                  | Per-Hart                 |
| Trusted Thread Kernel Stack | Trusted Thread 内核执行与 Syscall | Per-Thread               |
| User TA Stack               | TA 的用户态代码执行                  | Per-TA Context/Execution |

其中 Per-Hart Stack 和 Per-Thread Stack 是不同的概念。

一个 hart 在不同时间可以执行不同 Trusted Thread；一个 Trusted Thread 也可能在挂起后由具备相应支持的执行环境恢复。

因此不能简单地将：

```
hart_id == trusted_thread_id
```

作为设计前提。

## 9.3.3 OP-TEE 为什么需要 `thread_core_local`？

可以把 `thread_core_local` 理解为 OP-TEE 对当前 hart 的局部执行环境描述。

简化后的概念结构：

```
struct thread_core_local_concept {
    unsigned int hart_id;
    unsigned int hart_index;

    int current_thread;
    unsigned int flags;

    uintptr_t tmp_stack_end;
    uintptr_t abort_stack_end;

    uintptr_t saved_sp;
    uintptr_t saved_a0;
    uintptr_t saved_a1;
};
```

这是教学用的数据结构示意，不是 upstream 的精确 C 声明。

为什么需要这些字段？

因为 Trap 进入的第一时间，连调用 C 函数所需的寄存器都还没有保存。

OP-TEE 必须有一个可靠的 Per-Hart 数据区，作为汇编 Prologue 的临时存储位置。

例如真实代码：

```
trap_from_user:
    store_xregs tp, THREAD_CORE_LOCAL_X0, REG_SP
    store_xregs tp, THREAD_CORE_LOCAL_X1, REG_A0, REG_A1
```

表示先将 User `sp`、`a0`、`a1` 放入可信的 Core-local 临时空间。

然后再根据 Trap 类型选择正式保存位置。

这比直接用未经验证的 User Stack 安全得多。

## 9.3.4 为什么需要三种不同的 Trap Frame？

OP-TEE 不同的执行路径，需要保存的寄存器集合并不完全一致。

当前源码中可以看到三种不同的 Frame：

| Frame          | 代表性结构               | 典型用途                    |
| -------------- | ------------------- | ----------------------- |
| Thread Context | `thread_ctx_regs`   | 完整 Trusted Thread 暂停与恢复 |
| Syscall Frame  | `thread_scall_regs` | User TA 调用内核服务          |
| Abort Frame    | `thread_abort_regs` | 保存和诊断异常现场               |

这背后体现了一个重要优化：

Trap Frame 不是越大越好，而是必须满足该执行路径真正需要的 ABI 与上下文恢复要求。

例如，普通 Syscall 可以利用 ABI 的 Caller-saved/Callee-saved 约定减少不必要的状态保存。

但异步中断不能假定被打断代码位于函数调用边界，因此通常需要保存足够完整的 GPR 状态。

而异常诊断不仅需要 GPR，还需要：

```
EPC
CAUSE
TVAL
STATUS
IE
```

这些系统状态。

## 9.3.5 Kernel Exception Entry 的真实汇编过程

当前 `thread_rv.S` 的 `exception_from_kernel` 大致遵循以下流程：

```
exception_from_kernel
    |
    v
Save core-local flags
    |
    v
Select Abort Stack
    |
    v
Allocate thread_abort_regs
    |
    v
Save GPRs
    |
    v
Save XIE / XSTATUS / XEPC / XTVAL / XCAUSE
    |
    v
abort_handler()
    |
    v
Restore State
    |
    v
XRET
```

最关键的部分是：

```
/* Select abort stack */
load_xregs tp, THREAD_CORE_LOCAL_ABT_STACK_VA_END, REG_A1

/* Set stack */
mv sp, a1

/* Allocate abort frame */
addi sp, sp, -THREAD_ABT_REGS_SIZE
```

接着保存：

```
csrr t0, CSR_XSTATUS
store_xregs sp, THREAD_ABT_REG_STATUS, REG_T0

csrr t0, CSR_XEPC
store_xregs sp, THREAD_ABT_REG_EPC, REG_T0

csrr t0, CSR_XTVAL
store_xregs sp, THREAD_ABT_REG_TVAL, REG_T0

csrr a0, CSR_XCAUSE
store_xregs sp, THREAD_ABT_REG_CAUSE, REG_A0
```

然后：

```
mv a1, sp
call abort_handler
```

需要注意 `abort_handler()` 的第一个参数来自 `a0`，第二个参数是指向 Abort Frame 的 `a1`。

这符合 RISC-V C ABI。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1

## 9.3.6 Nested Exception：OP-TEE 如何避免重复使用 Abort Stack？

当 Kernel 第一次发生异常时，OP-TEE 使用 Abort Stack。

但考虑异常处理过程中又发生新的异常：

```
Kernel Code
   |
   | Page Fault #1
   v
Abort Stack
   |
   | Page Fault #2
   v
?
```

如果第二次仍然将 `sp` 指向 Abort Stack 顶部，原来的 Frame 可能被覆盖。

因此代码通过 `THREAD_CORE_LOCAL_FLAGS` 保存异常处理状态，识别是否已经处于 Abort 处理上下文。

当发现已经在 Abort Stack 上发生异常时，会选择 Temporary Stack 作为另一条紧急路径。

真实代码有相应分支：

```
/* Already handling an abort? */
bnez a1, sel_tmp_sp

/* First abort */
load_xregs tp, THREAD_CORE_LOCAL_ABT_STACK_VA_END, REG_A1
j set_sp

sel_tmp_sp:
/* Abort while using abort stack */
load_xregs tp, THREAD_CORE_LOCAL_TMP_STACK_VA_END, REG_A1
```

这里要区分两件事：

- 选择备用 Stack，目的是避免直接覆盖先前的异常现场。
- 能够保存第二次异常现场，并不意味着系统可以无限制地嵌套 Trap，更不意味着所有 Double Fault 都可恢复。

对于真实 SoC，还必须关注最新的 RISC-V `Ssdbltrp` 扩展。它通过 `sstatus.SDT` 等机制识别某些不可安全重入的 Trap 情况，在启用相关功能时可以将 S-mode 的意外双重 Trap 报告给更高特权级。具体行为受扩展实现和固件配置影响。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



Staff Engineer 需要关注的安全问题：

如果 Trap Handler 在尚未保存原始 `sepc`、`scause`、`stval` 之前再次触发异常，后一个 Trap 可能覆盖同一组 CSR。

所以真正的目标不是简单地准备两个 Stack，而是尽可能缩短 Trap CSR 尚未转存到可信内存的危险窗口。

# 9.4 OP-TEE Exception Dispatch：从 `scause` 到具体处理路径

## 9.4.1 为什么先区分 User Trap 与 Kernel Trap？

对于一般操作系统，Trap Handler 最终要解决的是系统调用、缺页、中断和用户进程退出。

对于 TEE Kernel，额外存在一个非常重要的安全要求：

User TA 的异常不得任意破坏 TEE Core，更不能引发不必要的整个安全系统崩溃。

因此有必要将不同来源分开：

```
thread_trap_vect
        |
        +-- trap_from_user
        |       |
        |       +-- Interrupt
        |       +-- ECALL
        |       +-- Abort
        |
        +-- trap_from_kernel
                |
                +-- Interrupt
                +-- Kernel Exception
```

在当前实现中，这种区分首先通过 `sscratch` 完成，随后再使用 `XCAUSE` 区分 Interrupt 和 Exception。

## 9.4.2 用 `scause` 的最高位快速分流

当前 upstream 使用：

```
csrr a0, CSR_XCAUSE

/* Interrupt flag is in the MSB */
bge a0, zero, exception_from_user
```

为什么一条 `bge` 就可以判断？

因为 RISC-V 的 Interrupt Flag 位于 XLEN 最高位。

对于 RV64：

```
Interrupt:
scause[63] = 1
```

将 `scause` 按有符号整数解释时，该值为负数。

而同步异常的最高位为零，是非负数。

因此：

```
bge a0, zero, exception_from_user
```

如果 `scause >= 0`，就进入同步异常处理。

否则，进入 Interrupt Handler。

这个设计简单高效，避免了单独构造 64-bit Mask 的操作。

同样的模式也用于 `trap_from_kernel`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 9.4.3 为什么 OP-TEE 要区分 Native Interrupt 和 Foreign Interrupt？

这是 OP-TEE 中很重要的一个概念。

Native Interrupt 是由当前 OP-TEE 执行环境负责处理的中断。

Foreign Interrupt 则属于当前 OP-TEE 执行环境以外的处理实体，需要通过适当的退出或交接机制处理。

例如一种平台设计可能是：

```
OP-TEE 正在处理 TA 请求
           |
           | Foreign Interrupt
           v
Suspend Trusted Thread
           |
           v
Exit TEE Domain
           |
           v
Linux / Other Interrupt Owner
           |
           v
Resume OP-TEE
```

但有一点需要特别澄清：

Native/Foreign 是 OP-TEE 的中断归属与处理模型，不是 RISC-V ISA 定义的 IRQ/FIQ Security State。

不要因为 OP-TEE Core 内部使用 FIQ 相关名称，就认为 RISC-V 原生提供 Arm TrustZone 式的 Secure FIQ。

当前 RISC-V 实现通过中断 Cause Code 构造相应 Interrupt Pending Bit，再与 `THREAD_EXCP_FOREIGN_INTR` 进行比较：

```
li a2, CSR_XCAUSE_INTR_FLAG
sub a2, a1, a2

li a3, 1
sll a3, a3, a2

li a2, THREAD_EXCP_FOREIGN_INTR
and a2, a3, a2

beqz a2, native_interrupt_from_user
```

这段代码可以理解为：

```
Cause Code
    |
    v
1 << Interrupt ID
    |
    v
Interrupt Policy Mask
    |
    +-- Foreign
    |
    +-- Native
```

然后：

```
Native:
thread_native_interrupt_handler()

Foreign:
thread_foreign_interrupt_handler()
```

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1

具体哪个中断归为 Native、哪个归为 Foreign，需要结合平台配置、委托关系和 OP-TEE 的中断屏蔽策略分析。

## 9.4.4 Native Interrupt 的实际 C 处理代码

当前 `thread_arch.c` 的主要逻辑如下：

```
void thread_native_interrupt_handler(
    struct thread_ctx_regs *regs,
    unsigned long cause)
{
    switch (cause & LONG_MAX) {
    case IRQ_XTIMER:
        clear_csr(CSR_XIE, CSR_XIE_TIE);
        break;

    case IRQ_XSOFT:
        thread_unhandled_trap(regs, cause);
        break;

    case IRQ_XEXT:
        interrupt_main_handler();
        break;

    default:
        thread_unhandled_trap(regs, cause);
    }
}
```

这是对实际实现作的等价整理。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



需要分别理解：

- Timer Interrupt：当前处理方式会清除相应 Timer Interrupt Enable Bit，避免该来源持续触发；后续的定时器管理取决于平台集成。
- Software Interrupt：当前代码将该路径交给 Unhandled Trap。
- External Interrupt：进入 OP-TEE 通用中断框架 `interrupt_main_handler()`。

这里还必须区分两层：

第一层：CPU Trap Cause

例如：

```
Supervisor External Interrupt = 9
```

第二层：Interrupt Controller 内部的具体设备 IRQ ID

例如：

```
UART IRQ = 42
Crypto Engine IRQ = 57
```

上面的数值只是示例。

`scause=9` 并不能直接告诉你中断来自哪个设备。需要通过 PLIC/APLIC/IMSIC 等控制器的驱动路径确定具体中断来源。

## 9.4.5 Foreign Interrupt 如何影响 Trusted Thread？

当前 `thread_rv.S` 的 `thread_foreign_interrupt_handler()` 会调用：

```
thread_state_suspend(
    THREAD_FLAGS_EXIT_ON_FOREIGN_INTR,
    status,
    pc
);
```

随后通过平台 ABI 返回不可信 Domain。

其关键思想是：

```
Trusted Thread Running
          |
          v
Foreign Interrupt
          |
          v
Save Thread Context
          |
          v
Thread -> SUSPENDED
          |
          v
Return to Untrusted Domain
```

当 Linux 或相应 Interrupt Owner 完成处理后，系统可在合适的调用路径恢复先前挂起的 Trusted Thread。

需要特别理解：

一个 Interrupt 可以导致 OP-TEE Trusted Thread 暂停，但并不意味着该 Trusted Thread 已被销毁。

而且，是否发生 OpenSBI Domain Switch、如何重新进入 OP-TEE，取决于底层 Firmware Transport 和平台 ABI 的具体实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

# 9.5 U-mode TA Trap：System Call、Illegal Instruction 与 Page Fault

这是本章最具有 OP-TEE 内核特色的部分。

## 9.5.1 User TA 如何调用 TEE Core？

假设 TA 执行：

```
TEE_GenerateRandom(buffer, length);
```

这个 API 本身不是 CPU 指令。

其实现通过 User TA Library 调用相应的 TEE Syscall Interface，最终触发 RISC-V `ecall`，进入 OP-TEE Kernel。

逻辑上：

```
User TA
    |
    | TEE_GenerateRandom()
    v
User TA Library
    |
    | Syscall Wrapper
    v
ecall
    |
    v
thread_trap_vect
    |
    v
trap_from_user
    |
    v
ecall_from_user
    |
    v
thread_scall_handler
    |
    v
Current Session's handle_scall()
    |
    v
TEE Syscall Implementation
```

关键点是：这是 U-mode TA → S-mode OP-TEE 的调用，不等于 Linux REE → OpenSBI → TEE Domain 的调用。

这两条执行链千万不能混淆。

## 9.5.2 `ecall_from_user` 的真实分发逻辑

当前代码通过 `CAUSE_USER_ECALL` 区分 Syscall：

```
exception_from_user:
    li a1, CAUSE_USER_ECALL
    bne a0, a1, abort_from_user

ecall_from_user:
    ...
```

如果是 U-mode ECALL，进入专门的 Syscall 路径。

否则，进入 `abort_from_user`。

为什么 Syscall 需要独立路径？

因为 Syscall 是有意触发的同步异常，不是错误。

它有明确的 ABI：

```
Syscall ID
Input Arguments
Return Value
Caller-saved Registers
```

因此与 Fault Diagnostic 所需的 Abort Frame 不同。

## 9.5.3 Syscall 如何切换回 Kernel Stack？

当前代码：

```
/* Get current Trusted Thread context */
get_thread_ctx a0, a1

/* Restore kernel stack pointer */
load_xregs a0, THREAD_CTX_KERN_SP, REG_SP

/* Allocate syscall frame */
addi sp, sp, -THREAD_SCALL_REGS_SIZE
```

这里的 `THREAD_CTX_KERN_SP` 是非常重要的字段。

它保存当前 Trusted Thread 在进入 TA 之前的 Kernel Stack Pointer。

这使得 TA 进入 OP-TEE 时，不需要建立一个新的 Kernel Thread，而是能够回到当前 Trusted Thread 的既有内核执行上下文。

这个机制与 Linux System Call 在用户栈和 Kernel Stack 之间切换的概念相似，但 OP-TEE 需要额外维护 Trusted Thread 与 TA Session 的关系。

## 9.5.4 Syscall Frame 为什么不保存全部 GPR？

当前代码会保存 User `sp`、`gp`、`tp`，以及相关 Caller-saved Registers。

例如：

```
store_xregs sp, THREAD_SCALL_REG_RA, REG_RA
store_xregs sp, THREAD_SCALL_REG_T0, REG_T0, REG_T2
store_xregs sp, THREAD_SCALL_REG_A0, REG_A0, REG_A7
store_xregs sp, THREAD_SCALL_REG_T3, REG_T3, REG_T6
```

它没有像一般异步 Interrupt 一样，直接保存所有的 `s0`～`s11` 到这个 Syscall Frame。

因为 RISC-V ABI 中：

- `a0`～`a7` 是参数/返回值相关寄存器。
- `t0`～`t6` 是 Caller-saved。
- `s0`～`s11` 是 Callee-saved。

Syscall 的执行路径可以依据自身 ABI、内核调用约定以及已建立的上下文管理机制，选择保存集合。

但设计这个优化需要特别谨慎：

不能因为普通 C 函数 ABI 规定 `s0`～`s11` 为 Callee-saved，就忽略异步中断、用户态切换以及内核辅助代码对这些寄存器的影响。

`thread_scall_regs` 和 `thread_ctx_regs` 的保存职责不同。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

## 9.5.5 `thread_scall_handler()` 的真正职责

当前 C 代码中的关键逻辑：

```
void thread_scall_handler(struct thread_scall_regs *regs)
{
    struct ts_session *sess = NULL;

    /* Configure interrupt handling ... */

    sess = ts_get_current_session();

    assert(sess && sess->handle_scall);

    if (sess->handle_scall(regs)) {
        regs->epc += 4;
    } else {
        setup_unwind_user_mode(regs);
    }
}
```

这是省略了中断状态与 FP Context 操作后的关键逻辑。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



特别注意：

```
sess->handle_scall(regs)
```

与：

```
regs->epc += 4;
```

之间的关系。

这里 `handle_scall()` 为真时表示正常返回 User Mode，所以下一条指令应该是 ECALL 后面的指令。

为什么是 `+4`？

因为标准 RISC-V `ecall` 指令占 4 字节。

即使处理器支持 RVC 压缩指令扩展，也不能因此把 ECALL 当成 2 字节指令。

### Syscall 失败、TA 退出和直接 Unwind

如果 `handle_scall()` 返回另一种控制流结果，OP-TEE 不再简单返回原 TA 的下一条指令，而是通过：

```
setup_unwind_user_mode(regs);
```

重定向执行状态，返回 Trusted Thread 的内核执行路径。

这与普通函数：

```
return result;
```

不同。

OP-TEE 可以修改保存的 Trap Frame，从而改变 `sret` 之后的执行位置和 Privilege。

这种设计也用于后面将要研究的异常终止。

## 9.5.6 Illegal Instruction：并不是所有非法指令都意味着 TA 出错

这是非常值得深入研究的一个细节。

假设 User TA 执行：

```
fadd.d fa0, fa1, fa2
```

但当前：

```
sstatus.FS = Off
```

即使 CPU 支持 F/D Extension，也会因浮点单元当前不可用而产生 Illegal Instruction Exception。

这时候内核可以有两种处理方式：

方案 A：直接终止 TA

认为该指令不可执行，返回 TA Panic。

方案 B：识别 Lazy FP Context Switching

内核判断该 TA 是第一次使用 FP，保存其他执行上下文的 FP 状态，为当前 TA 恢复或初始化 FP Context，然后重新执行原指令。

当前 RISC-V OP-TEE 已有相关的 Lazy FP Handling 实现。

在 `abort.c` 中：

```
static bool is_vfp_fault(struct abort_info *ai)
{
    if (ai->abort_type != ABORT_TYPE_UNDEF ||
        vfp_is_enabled())
        return false;

    return true;
}
```

识别到符合条件的异常后，可以调用：

```
thread_user_enable_vfp(...);
```

启用正确的 TA FP Context。

然后返回原始 `sepc`，重新执行造成异常的指令。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里还有一个值得 Staff Engineer 警惕的设计细节：

当前检测策略通过异常类型以及 FP Unit 是否启用进行初步判断，并不等同于对 Faulting Instruction 进行了完整解码。

所以需要理解这种实现如何区分真正的 FP 首次使用与一般非法指令，以及重复异常后如何终止。

对于 RISC-V Vector Extension，也存在类似的 Context Ownership、Lazy Switch、安全清理以及跨 Domain 状态泄露问题，但不能假定 FP 和 Vector 已具有完全相同的实现成熟度。

## 9.5.7 TA Page Fault：为什么不一定可以恢复？

假设两个 TA：

```
TA A -> Memory Region A
TA B -> Memory Region B
```

TA A 执行：

```
li a0, 0x90000000
ld a1, 0(a0)
```

假设这个地址属于 TA B，并且当前页表不允许 TA A 读取。

于是：

```
scause = 13  (Load Page Fault)
stval  = 0x90000000
SPP    = 0   (U-mode)
```

CPU 进入 OP-TEE S-mode。

关键问题：内核应该如何处理？

对于一般 Linux 系统，Page Fault 可能通过 demand paging、COW 等机制修复。

但对于 TEE Kernel，必须首先判断：

- 此次访问是否属于有效的 TA 虚拟内存区域？
- 是否有允许此次访问的 VMA/Region？
- 故障是否仅由于尚未建立合法映射？
- 是否属于非法越界、跨 TA 访问或权限违例？

如果 TA A 无权访问 Region B，内核绝对不能为了消除 Page Fault 而直接建立可读映射。

### 当前 RISC-V 实现的实际行为

在当前 `abort.c` 中，User Mode Fault 通常会进入：

```
FAULT_TYPE_USER_MODE_PANIC
```

之后保存故障信息，并通过修改 Trap Frame 终止 TA 执行。

Kernel Page Fault 在缺乏可用恢复路径时，会进入 Kernel Panic。

这是当前代码中实际可以观察到的行为，不能直接把 Arm OP-TEE 的 Pager 处理能力视为 RISC-V 已完整具备的能力。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



本章后面还会具体解释 TA 如何被安全终止。

# 9.6 M-mode Trap 与 OP-TEE S-mode Trap 的边界

这是 RISC-V OP-TEE 与传统 Arm TrustZone OP-TEE 之间一个非常重要的区别。

## 9.6.1 首先区分两种完全不同的 Trap

### 场景 A：TA 调用 OP-TEE Syscall

```
TA (U-mode)
   |
   | ECALL from U-mode
   | scause = 8
   v
OP-TEE (S-mode)
   |
   | Handle TEE Syscall
   v
TA (U-mode)
```

前提是 U-mode ECALL 已正确委托给 S-mode。

此时：

- Trap Target 是 OP-TEE S-mode。
- 使用 `stvec`。
- 使用 OP-TEE `thread_trap_vect`。
- 使用 `sepc/scause/stval`。
- 使用 `sret` 返回。

这完全属于 OP-TEE Domain 内部的执行机制。

### 场景 B：Linux 通过 SBI 请求 M-mode Firmware

```
Linux (S-mode)
   |
   | ECALL from S-mode
   | cause = 9
   v
OpenSBI (M-mode)
   |
   | Dispatch SBI Extension
   v
Firmware Service
   |
   v
Linux 或其他目标 Domain
```

此时：

- Trap Target 是 M-mode。
- 使用 `mtvec`。
- 使用 `mepc/mcause/mtval`。
- 使用 `mret` 完成相应的 M-mode 返回流程。

如果调用对应的是 TEE Transport，M-mode Firmware 可能还需要执行 Domain Context Switch。

所以 ECALL 是一种 Trap 指令，不是专门为 TEE 设计的 World Switch 指令。

其具体语义取决于发起者的 Privilege、Trap Delegation，以及上层软件定义的 ABI。

## 9.6.2 同一个物理 hart，能否同时拥有 Linux 和 OP-TEE 的 `stvec`？

这实际上是一个非常深入的架构问题。

RISC-V 的 Supervisor Trap CSR 是每 hart 的硬件状态。

对于一个没有使用额外虚拟化机制的 hart：

```
stvec
sscratch
sepc
scause
sstatus
satp
```

并不是 Linux Domain 和 OP-TEE Domain 各拥有一套硬件复制品。

它们在不同时间复用同一组硬件寄存器。

因此，如果一个 hart 在时间上先后执行：

```
Linux S-mode -> OP-TEE S-mode -> Linux S-mode
```

那么 Domain Switch 必须管理域私有的 Supervisor State。

例如：

```
Linux Domain Context
    stvec_linux
    sscratch_linux
    satp_linux
    sstatus_linux
    ...
          |
          | Domain Switch
          v
OP-TEE Domain Context
    stvec_optee
    sscratch_optee
    satp_optee
    sstatus_optee
    ...
```

如果返回 Linux 时没有恢复 `stvec_linux`，下一次 Linux 的 Trap 就可能进入 OP-TEE Trap Handler。

如果没有恢复 `satp_linux`，Linux 可能继续使用错误的地址空间。

如果没有正确管理 `sscratch`，Kernel Trap Entry 可能错误识别上下文。

因此，`stvec` 与 `sscratch` 不只是异常处理寄存器，也属于跨 Domain Context Switch 必须考虑的硬件状态。

这正是 Chapter 06 和本章之间的关键联系。

## 9.6.3 为什么 M-mode Firmware 不能直接使用 OP-TEE 的 Trap Frame？

原因在于它们属于不同层次。

| 机制          | M-mode Firmware         | OP-TEE S-mode             |
| ----------- | ----------------------- | ------------------------- |
| Trap CSR    | `mepc/mcause/mtval`     | `sepc/scause/stval`       |
| Trap Vector | `mtvec`                 | `stvec`                   |
| Return      | `mret`                  | `sret`                    |
| Context     | Firmware/Domain Context | Trusted Thread/TA Context |
| 主要职责        | 平台控制、SBI、Domain 管理      | TEE Kernel、TA、TEE Service |
| 隔离权限        | 可以控制底层保护策略              | 受底层硬件隔离策略限制               |

特别是 `thread_abort_regs`：

它是 OP-TEE 为处理自己领域的异常而定义的执行现场结构，并不是整个 SoC 所有 Privilege Mode 共用的标准 Trap Frame。

Linux 和 OpenSBI 都有各自的上下文保存结构。

## 9.6.4 Domain Switch 与 Trap Nesting 是两件不同的事

假设：

```
Linux S-mode
    |
    | ECALL
    v
OpenSBI M-mode
    |
    | Domain Switch
    v
OP-TEE S-mode
    |
    | TA Syscall
    v
OP-TEE S-mode Trap Handler
```

前一次 ECALL 导致的是进入 M-mode Firmware 的 Trap。

而后一次 ECALL 导致的是 OP-TEE Domain 内的 U→S Trap。

这两个 Trap 不是简单地在同一异常栈上嵌套。

当 OpenSBI 完成 Domain Switch，并通过 `mret` 将执行权交给 OP-TEE 后，之前的 M-mode Trap Handling 已经完成了其预期的返回动作。

后面的 TA Syscall 是 OP-TEE 域内部新的 Trap。

所以分析跨 Domain 调用时应同时维护两张图：

一张是 Firmware/Domain 执行切换图；另一张是 OP-TEE Kernel/TA Trap 执行图。

如果把它们混成同一条普通的函数调用链，很容易在调试时误判 Stack、CSR 和 PC。

# 9.7 Fatal Exception、TA Panic、Kernel Panic 与 Recovery

本节是异常处理与 TEE Security 的直接交汇点。

## 9.7.1 为什么 User TA 异常不应该导致整个 TEE 崩溃？

TEE Kernel 与 TA 具有不同的信任等级。

在典型架构中：

```
Trusted Computing Base
    |
    +-- OP-TEE Core (S-mode)
    |
    +-- User TA (U-mode, restricted)
```

这里的 U-mode TA 虽然属于可信应用，但不应默认拥有内核权限。

如果一个 TA 因为非法指针、非法指令或者自身 Bug 产生异常，理想结果是：

```
Faulting TA -> Terminate
Other TA    -> Continue
OP-TEE Core -> Continue
Linux       -> Receive Error
```

当然，这要求故障始终被硬件限制在 TA 的权限范围内，且 OP-TEE Kernel 的异常处理和资源回收路径是可靠的。

如果真正的 Kernel 数据结构已经被破坏，简单继续运行可能反而不安全。

## 9.7.2 OP-TEE 如何识别异常来自 U-mode？

真正用于判断异常原始 Privilege 的字段是：

```
sstatus.SPP
```

当前源码：

```
bool abort_is_user_exception(struct abort_info *ai)
{
    return (ai->regs->status & CSR_XSTATUS_SPP) == 0;
}
```

其中：

```
SPP = 0 -> Previous Privilege = U
SPP = 1 -> Previous Privilege = S
```

这里要注意两个概念：

- `sscratch` 是 Trap Entry 快速识别路径使用的约定。
- `SPP` 是 CPU 记录的 Trap 前 Privilege 信息。

它们应该保持一致，但作用不同。

在设计更高可靠性的异常诊断时，检查二者是否符合预期，是一个值得考虑的方向。

## 9.7.3 `abort_handler()` 的 Fault Classification

当前 `abort.c` 会根据 Cause 将异常分类为几种不同结果：

```
enum fault_type {
    FAULT_TYPE_USER_MODE_PANIC,
    FAULT_TYPE_USER_MODE_VFP,
    FAULT_TYPE_PAGE_FAULT,
    FAULT_TYPE_IGNORE,
};
```

对应处理原则：

| Fault Type      | 处理意图           |
| --------------- | -------------- |
| USER_MODE_PANIC | 终止故障 User TA   |
| USER_MODE_VFP   | 尝试处理 FP 首次使用   |
| PAGE_FAULT      | 根据当前实现决定是否可处理  |
| IGNORE          | 特定可忽略事件，之后恢复执行 |

不要将这里的 `FAULT_TYPE_IGNORE` 理解为所有未知错误都可以安全忽略。

对于会在重新执行同一指令时继续发生的 Fault，如果没有修正 Cause 或执行状态，忽略异常可能造成无限 Trap Loop。

## 9.7.4 一个非常关键的设计：通过修改 Trap Frame 终止 TA

假设 TA 发生：

```
Load Page Fault
sepc = 0x40001234
```

内核希望终止 TA，而不是回到：

```
0x40001234
```

它可以通过修改保存的 Trap Frame 实现控制流重定向。

当前 OP-TEE RISC-V 代码中：

```
static void handle_user_mode_panic(struct abort_info *ai)
{
    ai->regs->a0 = TEE_ERROR_TARGET_DEAD;
    ai->regs->a1 = true;
    ai->regs->a2 = 0xdeadbeef;

    ai->regs->epc = (vaddr_t)thread_unwind_user_mode;
    ai->regs->sp = thread_get_saved_thread_sp();

    ai->regs->status = xstatus_for_xret(true, PRV_S);
    ai->regs->ie = 0;
}
```

这段代码是本章必须理解的核心实现之一。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



它完成了三个重要动作。

第一，设置错误返回信息。

```
a0 = TEE_ERROR_TARGET_DEAD
```

告知上层执行路径 TA 已经无法继续正常运行。

第二，修改返回 PC 和 Stack。

```
epc = thread_unwind_user_mode
sp  = thread_get_saved_thread_sp()
```

不再返回 TA 的 Faulting PC，而是返回内核中的安全退出路径。

第三，修改异常返回目标 Privilege。

```
status = xstatus_for_xret(true, PRV_S)
```

让后续 `sret` 返回 S-mode Kernel，而不是 U-mode TA。

用执行图表示：

```
User TA
   |
   | Load Page Fault
   v
OP-TEE Trap Entry
   |
   v
abort_handler()
   |
   | Mark TA dead
   | Modify EPC
   | Modify SP
   | Modify SPP
   v
Restore Trap Frame
   |
   | sret
   v
thread_unwind_user_mode()
   |
   v
Trusted Thread Kernel Context
   |
   v
TA Session Error Handling
```

为什么不直接在 `abort_handler()` 里调用一个普通的 `return_to_TA_manager()` 函数？

因为异常发生时，执行环境并不是正常 C 函数调用链。

Handler 正运行在特殊的 Abort Stack，并且异常现场中可能包含 User Stack、User GP/TP、不同 Privilege 的返回状态。

通过修改 Trap Frame，OP-TEE 可以复用已经存在的异常返回机制，在正确的 Privilege 和 Kernel Stack 下回到可信执行路径。

这是异常处理代码中一个很典型的 Kernel Control-Flow Engineering 技巧。

## 9.7.5 Kernel Fatal Exception 应该怎么办？

假设 OP-TEE Core 执行：

```
struct secure_object *obj = NULL;

obj->state = ACTIVE;
```

产生 Store Page Fault。

此时：

```
SPP = 1
```

表明异常来自 S-mode Kernel。

如果缺少合法的恢复机制，不能像普通 TA 一样直接将 Kernel 宣告为 TARGET_DEAD 并继续运行其他操作。

因为内核已经发生不可预期的错误，其共享状态可能不再可信。

当前 RISC-V `abort_handler()` 对无法处理的 Kernel Page Fault 会打印异常现场并调用 `panic()`。

如果异常发生时甚至没有有效的 Trusted Thread Context，也会进入 Fatal Path。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

但是，Kernel Panic 并不等于必须立即重启整个 SoC。

还要区分：

- OP-TEE Kernel 的软件 Fatal Policy。
- Secure Domain 的恢复策略。
- M-mode Firmware 的异常处理。
- 独立 HSM、Watchdog 或系统安全控制器的 Recovery Policy。

例如在服务器级 SoC 中，系统可能通过 Watchdog、Error Record、Domain Reset、整机 Reset 等机制处理不可恢复错误。

这些行为没有一个统一的 RISC-V OP-TEE 默认方案，需要在 SoC Security Architecture 中明确设计。

# 9.8 Panic Dump、Debugging 与故障定位方法

本节偏向工业项目实际使用。

作为 Staff Engineer，面对异常日志，不能仅仅看见 `Page Fault` 就认为是 MMU 问题。

需要能够从 CPU 状态还原执行现场。

## 9.8.1 一份典型 RISC-V OP-TEE Abort 信息

假设日志包含：

```
Core data-abort at address 0x0000000090001000

cpu     #3
cause   000000000000000d
epc     ffffffc000123456
tval    0000000090001000
status  0000000200000120

ra      ffffffc000125678
sp      ffffffc000500000
gp      ...
tp      ...
```

以上是用于教学的模拟日志，不代表某次实际构建输出。

分析顺序：

### 第一步：确定哪个 hart 发生异常

```
cpu #3
```

首先确认这是 OP-TEE 的逻辑 Core Index，还是原始硬件 Hart ID。

两者在很多 SoC 中并不相同。

例如：

```
Hart IDs = 0, 2, 4, 6
Core Index = 0, 1, 2, 3
```

如果错误地把 Core Index 当作 Hart ID，后续查看 OpenSBI Per-Hart State 时可能找到错误的上下文。

### 第二步：解码 `scause`

```
cause = 0xD
```

说明是 Load Page Fault。

### 第三步：确定 Faulting VA

```
tval = 0x90001000
```

尝试读取的虚拟地址是 `0x90001000`。

### 第四步：确定 Faulting PC

```
epc = 0xffffffc000123456
```

通过对应的 ELF Symbol、Map 和 DWARF 信息定位具体指令。

如果启用了 ASLR 或 PIE，必须使用正确的运行时 Load Base 和 Relocation Offset 还原地址，不能直接把运行时 PC 当作未重定位 ELF 地址。

### 第五步：检查 `sstatus.SPP`

如果保存的 `status.SPP=1`，说明发生异常之前运行在 S-mode。

如果 `SPP=0`，说明来自 User TA。

这个判断直接决定后续是 TA Fault 还是 Kernel Fault。

### 第六步：检查 `satp` 和 PTE

对于 Sv39/Sv48：

```
VA
 |
 v
VPN / Offset
 |
 v
Page Table Walk
 |
 v
PTE.V / R / W / X / U / A / D
 |
 v
Physical Address
```

需要检查：

- PTE 是否有效。
- 是否有合法的 Leaf PTE。
- 当前 Privilege 是否满足 U/S 权限。
- 是否满足 Read/Write/Execute 权限。
- A/D 位处理是否符合平台配置。
- 是否因 TLB 旧状态而发生异常。
- 最终物理访问是否可能受到 PMP/系统防火墙限制。

最后一点要注意：PMP 拒绝物理访问通常体现为 Access Fault，而不是普通的 Page Fault。

因此，不能仅凭 `stval` 就断定是 PMP 导致的异常。

## 9.8.2 Page Fault 与 Access Fault 的根本区别

可以用这张表区分：

| Exception                    | 常见含义           | 首要调查方向                   |
| ---------------------------- | -------------- | ------------------------ |
| Instruction Page Fault (12)  | 取指的页表翻译或权限失败   | PTE、PC、X 权限              |
| Load Page Fault (13)         | 读取的页表翻译或权限失败   | PTE、R/U/SUM、A            |
| Store Page Fault (15)        | 写入的页表翻译或权限失败   | PTE、W/U、D                |
| Instruction Access Fault (1) | 取指物理访问/访问保护等错误 | PMP、Memory System        |
| Load Access Fault (5)        | 读取物理访问/访问保护等错误 | PMP、Bus、Physical Mapping |
| Store Access Fault (7)       | 写入物理访问/访问保护等错误 | PMP、Bus、Device           |

这只是常见解释；实际还需要考虑 PMP、页表遍历过程中访问页表内存失败、硬件扩展及具体实现。

例如：

假设 OP-TEE 访问：

```
VA = 0x90001000
```

页表把它映射到：

```
PA = 0x80001000
```

但 PMP 禁止当前 S-mode 访问该物理地址。

那么：

```
VA Translation may succeed
PMP Access Check fails
```

通常会产生相应的 Access Fault。

对于 OpenSBI Domain Isolation，区分 Page Fault 和 Access Fault 是确定错误位于 TEE 内部页表 还是 底层物理隔离机制 的重要线索。

## 9.8.3 GDB 应该观察什么？

如果使用 QEMU + GDB 调试 OP-TEE，可以在已经具备相应符号和 Remote Debug 环境的情况下设置：

```
break thread_trap_vect
break abort_handler
break thread_scall_handler
```

当进入 Trap 后查看：

```
info registers
```

在支持对应 CSR 访问的 GDB/QEMU 组合中，可以进一步查看：

```
p/x $scause
p/x $sepc
p/x $stval
p/x $sstatus
p/x $satp
p/x $sscratch
```

实际 GDB 对 CSR 的命名和暴露程度可能因版本不同而变化；不能读取时，可以用临时汇编诊断函数或日志进行验证。

对于执行时序，我特别建议使用：

```
x/16i $pc
si
```

单步分析。

但是不要从 `abort_handler()` 直接开始。

应从 `thread_trap_vect` 开始，观察：

```
sscratch
tp
sp
XCAUSE
XSTATUS
```

变化过程。

因为到 `abort_handler()` 时，汇编入口已经修改过一批寄存器，所看到的状态不再完全等于 Trap 发生瞬间的原始状态。

## 9.8.4 如何定位一个错误的 TA 指针？

假设 TA 调用：

```
TEE_MemMove(dst, src, len);
```

在 OP-TEE 内核中发生异常。

你需要区分：

情况 A：TA 自己直接访问非法地址。

Trap 来源为 U-mode，理论上可以通过 User TA Fault Policy 处理。

情况 B：TEE Core 试图使用未经验证的 TA 指针。

例如内核直接解引用了一个用户提供的非法地址，发生 S-mode Fault。

这是一个完全不同、而且可能具有安全影响的问题。

情况 C：TEE Core 在复制用户内存时使用了专门的 Fault Recovery 机制。

如果该平台和代码路径有合法的安全用户复制/故障恢复机制，则可能将地址错误转化为错误返回，而不是整个 Kernel Panic。

因此，工业级调试不能只看 Faulting Address，还要检查：

```
TA Parameter
    |
    v
TEE Syscall Boundary
    |
    v
Pointer Validation
    |
    v
User Memory Access
    |
    v
Fault / Success
```

核心安全要求是：来自 User TA 的指针不可信，Kernel 不能因为调用者是 Trusted Application 就省略权限验证。

## 9.8.5 如何排查重复异常与异常风暴？

假设日志连续出现：

```
scause=13
sepc=0x40001234
stval=0x90001000
```

且完全相同。

这通常意味着：

```
Faulting Instruction
        |
        v
Trap Handler
        |
        v
Return to same PC
        |
        v
Faulting Instruction
```

需要检查：

1. 是否应该修改 `sepc`。
2. 如果应当重新执行指令，原有 Fault Condition 是否已经被修复。
3. 页表修改后是否执行了必要的 `SFENCE.VMA`。
4. 是否重新建立了正确的映射。
5. 用户/内核执行权限是否依旧不满足。
6. 是否因 PTE/A-D 状态更新不完整而重复 Fault。

同理，对于 External Interrupt，也需要检查 Interrupt Source 是否被 ACK/Clear，以及中断控制器是否已经完成所要求的 Claim/Complete 或相应 EOI。

否则 `sret` 之后可能立即再次进入 Trap。

# 9.9 Trap Return：从异常现场回到正确的执行环境

本节把前面所有内容连接起来。

## 9.9.1 `sret` 之前究竟需要恢复什么？

典型同步异常的返回过程：

```
C Handler Returns
      |
      v
Restore Saved EPC
      |
      v
Restore Saved STATUS / Interrupt State
      |
      v
Restore GPRs
      |
      v
Restore SP / TP
      |
      v
sret
```

但这只是高层逻辑。

实际顺序受到寄存器依赖影响。例如：

- 在恢复原始 `sp` 前，通常需要完成 Stack Frame 上其他寄存器的读取。
- 在切换回 User `tp` 前，不能再依赖内核 `tp` 去访问 Core-local 数据。
- `sscratch` 必须满足下一次 Trap Entry 的约定。
- 中断使能状态必须与恢复路径一致。
- `sepc`、`sstatus` 必须匹配目标执行环境。

当前 RISC-V OP-TEE 的 `thread_resume` 和 `trap_from_user` 等路径通过这些步骤完成 Context Restore。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



## 9.9.2 为什么返回 U-mode 与返回 S-mode 的处理不同？

假设 `sret` 的目标是 TA：

```
Previous Privilege = U
```

则需要：

```
sscratch = OP-TEE thread_core_local pointer
tp       = TA Thread Pointer
gp       = TA Global Pointer
sp       = TA Stack Pointer
```

而如果目标是 Kernel：

```
Previous Privilege = S
```

则需要：

```
sscratch = 0
tp       = OP-TEE thread_core_local pointer
gp       = Kernel Global Pointer
sp       = Trusted Kernel Stack
```

这就是为什么当前汇编在返回前检查 `SPP`，并根据目标 Privilege 决定是否恢复 User `gp` 和 `tp`。

其中：

```
csrw CSR_XSCRATCH, tp
```

用于返回 U-mode 前重新建立下一次 Trap 所需的 Core-local Pointer。

而：

```
csrw CSR_XSCRATCH, zero
```

用于返回 Kernel 执行路径。

Trap Return 不仅是恢复 PC，更是恢复符合目标 Privilege、ABI 和内核约定的一整套运行环境。

## 9.9.3 为什么 Syscall 返回和 Page Fault 返回要区别对待？

看三个具体例子：

| 场景                 | `sepc` 应如何处理？        | 目标          |
| ------------------ | -------------------- | ----------- |
| 正常 ECALL           | `sepc += 4`          | 返回下一条 TA 指令 |
| 合法且已修复的 Page Fault | 保持 Faulting PC       | 重新执行原指令     |
| 不可恢复的 TA Fault     | 修改为 Kernel Unwind 入口 | 终止故障 TA     |

这三个场景使用同一个异常返回机制，却实现完全不同的控制流。

所以 `sret` 只是机制，最终恢复到哪里由软件准备的 Trap Frame 和 CSR 状态决定。

## 9.9.4 Interrupt Return 为什么不能简单 `sepc += 4`？

假设中断发生于：

```
ld a0, 0(a1)
add a2, a2, a0
```

这不是程序主动执行 ECALL。

中断对应的 `sepc` 是体系结构定义的恢复位置，可能是尚未执行的 `ld`，也可能是之后的有效指令边界。

Interrupt Handler 的职责是维护被打断程序的精确执行语义。

如果简单：

```
sepc += 4;
```

可能直接跳过一条原本应该执行的指令。

因此对于普通 Interrupt，恢复时应使用保存的 PC，而不应无条件调整 PC。

## 9.9.5 多 hart 下的 Context Restore 问题

再考虑一个更复杂的情形：

```
Hart 0:
    Execute OP-TEE Trusted Thread #2
        |
        | Foreign Interrupt
        v
    Suspend Thread #2

Hart 1:
    Resume Trusted Thread #2
```

这种恢复模型可能要求将 Thread #2 的执行上下文转移到另一 hart。

但哪些寄存器与 Hart 有关，哪些与 Thread 有关？

可以从下面的分类思考：

| 状态                          | 主要归属                           |
| --------------------------- | ------------------------------ |
| User TA GPR / PC            | Trusted Thread / TA Execution  |
| User TA FP/Vector Context   | TA/Thread Execution Context    |
| OP-TEE Kernel Stack         | Trusted Thread                 |
| `thread_core_local`         | Hart-local                     |
| `stvec` / `sscratch` 硬件 CSR | Hart-local 执行状态                |
| 当前 Hart Index               | Hart-local                     |
| TA Page Table Mapping       | TA Context / 运行时映射管理           |
| Kernel `tp`                 | 当前执行 Hart 的 Core-local Pointer |

特别注意 Kernel `tp`：

如果 Trusted Thread 原来运行在 Hart 0：

```
tp = &thread_core_local[0]
```

当它在 Hart 1 恢复时，必须保证：

```
tp = &thread_core_local[1]
```

不能把旧 hart 的 Core-local Pointer 原样恢复。

当前 `thread_resume_from_rpc()` 的实现中确实考虑了这一问题：如果恢复的是 Kernel Context，会将保存的 `tp` 更新为当前 hart 的 `tp`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这是典型的 Per-Thread State 与 Per-Hart State 之间的边界问题。

类似地，FP/Vector、ASID、TLB 和 Interrupt State 的跨 hart 恢复也必须结合平台能力验证，不能仅靠恢复 GPR 就假定整个执行环境已经安全恢复。

# 十、端到端案例：把整个 Trap Architecture 串联起来

前面的内容已经覆盖了 9.1～9.9。现在通过四个案例，把分散的机制组织成完整执行链。

## Case 1 — User TA 正常执行一次 ECALL

假设 TA 调用某个 TEE Internal API。

1. TA U-mode

   执行 `ecall`，`tp` 保存 User Thread Pointer，`sscratch` 保存当前 Hart 的 Core-local Pointer。
2. CPU Hardware Trap

   更新 `sepc`、`scause=8`、`sstatus`，从 `stvec` 进入 S-mode。
3. `thread_trap_vect`

   交换 `tp` 与 `sscratch`，识别为 User Trap。
4. `exception_from_user`

   检测 `CAUSE_USER_ECALL`，进入 `ecall_from_user`。
5. Syscall Prologue

   切换到 Trusted Thread Kernel Stack，构造 `thread_scall_regs`，保存必要寄存器。
6. `thread_scall_handler`

   获取当前 TA Session，调用对应的 Syscall Handler。
7. 正常返回准备

   修改保存的 `epc += 4`，恢复 User Context。
8. `sret`

   返回 TA U-mode，从 ECALL 后面的下一条指令继续执行。

整个过程中不必进入 OpenSBI，前提是 U-mode ECALL 已委托给 S-mode。

## Case 2 — User TA 非法访问 OP-TEE Kernel Memory

假设：

```
TA U-mode
    |
    | Load Kernel Address
    v
Load Page Fault
    |
    v
OP-TEE S-mode Trap Entry
    |
    v
abort_from_user
    |
    v
abort_handler
    |
    v
FAULT_TYPE_USER_MODE_PANIC
    |
    v
Modify Trap Frame
    |
    v
sret -> thread_unwind_user_mode
    |
    v
TA Terminated
```

这里的安全关键不是发生了 Page Fault，而是：

1. 硬件禁止 U-mode 访问没有 User 权限的内存。
2. 内核正确识别 Fault 来源。
3. 内核不把非法地址变成合法映射。
4. 内核通过安全的 Unwind 机制结束 TA。
5. 不应让异常处理路径破坏其他 Trusted Thread 的状态。

这五个条件构成了一个完整的 TA Isolation Exception Test。

## Case 3 — OP-TEE Kernel 发生异常，为什么可能直接 Panic？

假设：

```
Trusted Thread (S-mode)
      |
      | Bad Kernel Pointer
      v
Kernel Page Fault
      |
      v
thread_trap_vect
      |
      v
trap_from_kernel
      |
      v
exception_from_kernel
      |
      v
Abort Stack
      |
      v
abort_handler
      |
      v
Kernel Panic
```

这里不应该继续执行已经损坏的内核状态。

更复杂的是，如果 Panic Diagnostic 自己访问了非法内存：

```
Kernel Fault #1
     |
     v
Abort Handler
     |
     | Fault #2
     v
Nested Abort
```

就进入了异常重入问题。

因此工业级 TEE 的 Fatal Diagnostic 应尽可能减少对动态内存、复杂锁、不可重入驱动以及 REE 服务的依赖。

特别是 Secure Storage、Crypto Engine 或 Shared Memory 已处于错误状态时，Panic Path 不能随意假定它们仍然可靠。

## Case 4 — 16-Hart SoC 上的 Foreign Interrupt 与跨核恢复

这是最接近服务器级 RISC-V SoC 的案例。

假设：

```
Hart 3                         Hart 7

OP-TEE Thread #5
       |
       | Foreign Interrupt
       v
Save Thread Context
       |
       v
Thread #5 -> SUSPENDED
       |
       v
Exit TEE Domain
                                  |
                                  | Resume Request
                                  v
                           OP-TEE Domain Entry
                                  |
                                  v
                           Thread #5 -> ACTIVE
                                  |
                                  v
                           Restore User Mapping
                                  |
                                  v
                           Update Hart-local tp
                                  |
                                  v
                           thread_resume()
```

这个过程中存在多个需要严格验证的安全约束。

| 约束                      | 风险                            |
| ----------------------- | ----------------------------- |
| Thread #5 只能被一个 hart 激活 | 同一 Trusted Thread 并发执行        |
| Kernel `tp` 必须指向 Hart 7 | 错误访问 Hart 3 的 Core-local Data |
| TA User Mapping 必须正确恢复  | 访问错误 TA 地址空间                  |
| FP/Vector 状态必须正确管理      | 不同执行上下文间的数据泄露                 |
| `sscratch` 必须按目标模式设置    | 下一次 Trap Entry 分类错误           |
| Interrupt State 必须正确恢复  | 丢失中断、错误重入或中断风暴                |
| M-mode Domain 状态必须正确    | Linux/TEE 隔离被破坏               |

这是 Staff Engineer 需要能够从整体上论证正确性的复杂问题，而不只是确保 `thread_resume()` 执行成功。

# 十一、Hands-on Lab：使用 QEMU/GDB 验证本章机制

下面的实验设计适用于已经完成 Chapter 03 中 RV64 OP-TEE 开发环境搭建的情况。建议使用固定版本的 upstream 和可复现的 QEMU 镜像。

## Lab 1 — 验证 `sscratch` / `tp` Trap Entry 协议

目标：验证本章 9.2.3 中讲述的汇编机制。

在 GDB 中：

```
break thread_trap_vect
continue
```

当命中断点时：

```
info registers tp sp pc
p/x $sscratch
p/x $sstatus
p/x $scause
```

随后执行：

```
x/8i $pc
si
info registers tp
p/x $sscratch
```

分别记录 User Trap 和 Kernel Trap 时的交换结果。

需要最终验证：

| 检查项目           | 预期                                |
| -------------- | --------------------------------- |
| User Trap 前    | `sscratch` 是可信 Core-local Pointer |
| 第一次交换后         | `tp` 变成 Core-local Pointer        |
| Kernel Trap 前  | `sscratch=0`                      |
| Kernel Trap 入口 | 两次交换恢复 Kernel `tp`                |
| 返回 U-mode 前    | 恢复 User `tp` 和 `sscratch`         |

## Lab 2 — 验证 ECALL 的 `sepc += 4`

在 TA 执行一个正常 TEE Syscall 时设置：

```
break thread_scall_handler
continue
```

记录：

```
EPC_before
EPC_after
```

正常返回路径应该观察到：

```
EPC_after = EPC_before + 4
```

然后通过反汇编确认 ECALL 后面的下一条指令确实位于该地址。

另外应测试 TA 主动退出的路径，验证并非所有 `handle_scall` 返回分支都简单将 EPC 增加 4。

## Lab 3 — 注入一个 User TA Page Fault

在测试用 TA 中使用下列代码触发非法访问：

```
static void trigger_user_page_fault(void)
{
    asm volatile(
        "li t0, 0\n"
        "ld t1, 0(t0)\n"
        :
        :
        : "t0", "t1", "memory"
    );
}
```

这是针对 RV64 的故障注入示例，应仅用于测试环境。

假设虚拟地址零未映射，预期：

```
TA U-mode
  -> Load Page Fault
  -> abort_from_user
  -> abort_handler
  -> User TA Termination
```

需要观察：

- `scause` 是否为 13。
- `stval` 是否对应预期地址。
- `SPP` 是否为 0。
- `abort_handler()` 是否保存故障信息。
- `epc` 是否重定向到 Unwind Path。
- 其他 TA 是否仍能正常运行。

如果结果是 Access Fault 而不是 Page Fault，应该继续调查 MMU、PMP 和平台配置，而不是假定测试一定失败。

## Lab 4 — 验证 Kernel Fault 与 User Fault 的不同处理策略

分别构造：

```
User TA Invalid Access
Kernel Invalid Access
```

对于 Kernel Fault，应在专门的可丢弃测试环境或故障注入分支中执行，避免损坏生产系统。

核心观察目标：

| 观察项目               | User Fault       | Kernel Fault         |
| ------------------ | ---------------- | -------------------- |
| Previous Privilege | U                | S                    |
| Trap Entry Path    | `trap_from_user` | `trap_from_kernel`   |
| Fault Handler      | `abort_handler`  | `abort_handler`      |
| 典型恢复策略             | TA Unwind        | Panic                |
| 系统可继续执行的范围         | 非故障 TA/TEE Core  | 依赖 Fatal Recovery 策略 |

## Lab 5 — 验证跨 Hart 恢复

在多核 QEMU 或支持相应机制的仿真平台中：

1. 让 Trusted Thread 在一个 hart 执行。
2. 触发支持的 Foreign Interrupt 或 RPC Suspend。
3. 尝试通过另一个 hart 的合法入口恢复它。
4. 比较恢复前后的 Thread Context 和 Hart-local Context。
5. 验证 TA 内存映射与返回 PC 正确。
6. 检查是否存在 FP/Vector 状态异常或重复激活。

这个实验需要底层 Firmware Transport、平台 ABI 和调度行为支持跨 Hart Resume；不要为了实验直接修改 Thread State 绕开正常同步机制。

# 十二、Staff Engineer 级面试与架构设计题

18 道题 · 覆盖 ISA、汇编、内核、隔离、SMP、安全与真实 SoC 故障场景

这些不是简单的知识记忆题。每道题都要求候选人结合寄存器、代码路径、时序、安全边界和工程验证进行分析。

## Part A — Trap Entry 与汇编基础机制

### Q1. OP-TEE Trap Entry 为什么使用 `csrrw tp, sscratch, tp`？

场景：有人提交 PR，建议改成：

```
csrr tp, sscratch
```

理由是只需要获取 Kernel Core-local Pointer。

请分析这种修改为什么可能破坏 User TA 的执行状态。要求同时解释 `sscratch` 保存的内容、User `tp` 的恢复方式，以及 Kernel Trap 的特殊处理。

考核：异常入口寄存器所有权、Scratch Protocol、U/S-mode ABI。

### Q2. Trap 进入后能否立即调用普通 C 函数？

场景：某工程师希望在 `stvec` 指向的入口放置：

```
void exception_entry(void)
{
    printk("trap\n");
}
```

请解释其潜在错误，并设计一个安全的最小汇编 Prologue。

要求考虑 `sp`、`gp`、`tp`、GPR 保存、地址空间、嵌套异常和中断状态。

考核：ABI、可信栈、Trap Frame、异常入口原子性。

### Q3. 如果 TA 修改自己的 `tp` 为任意数值，能否欺骗 OP-TEE Trap Handler？

场景：一个恶意 TA 执行：

```
li tp, 0xffffffffffffffff
ecall
```

请分析实际 Trap 入口依赖什么信息识别 User Context。

进一步分析如果 `sscratch` 被错误初始化，可能出现哪些安全问题。

考核：User-controlled State 与 Supervisor-controlled State 的边界。

### Q4. 如果 Trap 入口的第一条访存指令再次产生 Page Fault，会发生什么？

场景：`stvec` 正确，但 Trap Entry 所依赖的 Core-local Memory 没有映射到当前 TA 页表。

请给出从硬件到软件的可能执行过程，并解释为什么仅增加一个 Abort Stack 不一定能够解决问题。

考核：精确异常、Nested Trap、CSR 覆盖、Ssdbltrp 与最小可信入口。

## Part B — Exception Dispatch 与 User TA

### Q5. 为什么 TA Syscall 要将 EPC 增加 4，而 Page Fault 通常不应该这样做？

场景：某开发者把 `sepc += 4` 添加到通用异常分发函数中，导致部分 TA 的随机行为异常。

请分析其根本错误，并设计一个能覆盖 ECALL、Page Fault、Breakpoint 和 Interrupt 的 EPC 更新策略。

考核：Precise Exception、指令长度、故障重试与 Control-Flow Integrity。

### Q6. TA 发生 Page Fault，是否总应该 Terminate？

场景：一个新的 RISC-V OP-TEE Port 计划支持 Demand Paging。

某 TA 访问尚未建立映射但属于合法地址区域的页面。

请设计 Fault Classification 和 Recovery Workflow，并说明如何避免把非法地址变成合法映射。

考核：VMA、Pager、PTE、权限验证、Fault Retry、TLB Synchronization。

### Q7. 为什么某些 Illegal Instruction 不应该直接终止 TA？

场景：开启 Floating-point Lazy Context Switching 后，一个 TA 第一次执行 `fadd.d` 产生 Illegal Instruction。

请解释产生异常的硬件原因，设计正确的 FP Context Ownership 和恢复策略，并讨论如何避免跨 TA 泄露浮点寄存器内容。

考核：`sstatus.FS`、Lazy FP、状态归属、寄存器清理、安全性。

### Q8. 修改 Trap Frame 能否安全实现 TA Termination？

场景：TA 发生不可恢复的异常，Kernel 希望返回 TA Manager 而不是重新执行原指令。

请解释如何通过 EPC、SP、SPP、返回参数重定向控制流，并分析为什么不能简单调用一个普通的 C Return Function。

考核：`thread_unwind_user_mode`、异常返回、栈一致性、执行模式转换。

## Part C — OpenSBI 与跨 Domain Trap

### Q9. 为什么 Linux 与 OP-TEE 同样处于 S-mode，却能有不同的 Trap Vector？

场景：一个 SoC 在同一个 Hart 上轮流执行 Linux Domain 和 OP-TEE Domain。

请设计完整的 Supervisor CSR 保存和恢复策略，并分析遗漏 `stvec`、`sscratch` 或 `satp` 会产生什么后果。

考核：Per-Hart CSR、OpenSBI Domain Context、内存隔离、Trap Ownership。

### Q10. 如果 Linux ECALL 被错误委托给 S-mode，会出现什么现象？

场景：Linux 通过 SBI 调用 OpenSBI，但系统意外进入 Linux 自身的 Trap Handler。

请从 `medeleg`、Trap Target、`stvec`、`mtvec` 和 ECALL Cause Code 分析问题，并设计必要的 Firmware Boot Validation。

考核：Exception Delegation、Privilege Routing、SBI ABI、Boot Configuration。

### Q11. OP-TEE 中出现 Load Access Fault，但 Page Table 看起来完全正确，应该怎样定位？

场景：

```
scause = 5
stval  = 0x90001000
```

目标 VA 在页表中具有合法的 RW 映射。

请设计从 Page Table Walk、Physical Address、PMP、SoC Firewall 到总线访问错误的排查流程。

考核：Page Fault/Access Fault 区分、PMP、Physical Memory、硬件隔离。

## Part D — SMP、Interrupt 与 Context Switching

### Q12. Foreign Interrupt 使 Trusted Thread 挂起，为什么必须保存完整执行状态？

场景：TA 正在执行多条计算指令，发生 Foreign Interrupt。

请解释为什么不能仅保存 `ra`、`sp` 和 Caller-saved Registers。

进一步分析 FP、Vector、ASID 和地址空间如何影响恢复正确性。

考核：异步 Trap、完整上下文、寄存器保存集合、恢复不变量。

### Q13. Trusted Thread 在 Hart 0 暂停，Hart 7 恢复，哪些状态绝对不能直接复制？

场景：服务器 SoC 具有 16 个 hart，Trusted Thread 允许跨 Hart Resume。

请列出所有需要重新绑定或验证的 Hart-local State，并说明可能出现的数据泄露和并发问题。

考核：`tp`、`sscratch`、Per-Hart Interrupt State、TLB、FP/Vector、Context Ownership。

### Q14. 一次 TLB Shootdown 是否可能与 Trap Return 发生竞态？

场景：Hart 0 修改了 User TA 的页表，Hart 1 正准备从 OP-TEE Trap Handler 返回同一个 TA。

请分析可能的时间窗口，提出同步方案，并描述如何验证其正确性。

考核：RISC-V Memory Model、`SFENCE.VMA`、跨 Hart 同步、PTE 生命周期。

### Q15. Secure Interrupt 为何不能仅通过 `scause` 判断设备身份？

场景：一个 Crypto Engine 产生中断，但 `scause` 只显示 Supervisor External Interrupt。

请解释 PLIC/APLIC/IMSIC 的角色，以及 OP-TEE 如何判断这个中断是否属于当前 TEE。

考核：Interrupt Source、Interrupt Domain、路由、Claim/Complete、权限控制。

## Part E — 工业级可靠性与安全设计

### Q16. OP-TEE 在处理 Kernel Panic 时再次发生异常，应该如何设计恢复？

场景：Panic Handler 正在打印日志，访问 UART MMIO 时发生第二次 Access Fault。

请设计分级 Fatal Error Handling，包括备用 Stack、最小诊断路径、错误记录、Watchdog 和 Recovery Policy。

考核：Double Trap、可靠性、不可重入资源、SoC Recovery Architecture。

### Q17. 一个恶意 TA 能否利用异常处理造成整个 TEE 的拒绝服务？

场景：恶意 TA 持续执行非法指令、触发 Page Fault、创建并销毁 Session，尝试耗尽 Trusted Thread 或 Stack 资源。

请设计安全测试、资源上限、故障隔离策略和 Rate Limiting 机制，并分析哪些限制必须由 OP-TEE Core 实现。

考核：DoS、资源隔离、生命周期管理、Trusted Thread Exhaustion。

### Q18. 设计一个能证明 RISC-V OP-TEE Trap Handler 安全性的验证矩阵

场景：你负责一个即将流片的 RISC-V Server SoC，集成 16 个 Hart、OpenSBI Domains、OP-TEE 和独立 HSM。

你需要为 Trap Handling 提供一次系统级 Sign-off。

请设计验证计划，要求覆盖：

- U-mode 与 S-mode 的 Exception。
- M/S Delegation。
- Page Fault / Access Fault / Illegal Instruction。
- Native / Foreign Interrupt。
- Nested Trap 与异常 Stack。
- FP/Vector Context。
- 16-Hart 并发与跨 Hart Resume。
- Domain Isolation 与 PMP。
- Fatal Error、Debug 和 Recovery。
- 功能测试、压力测试和 Fault Injection。

考核：完整的 Security Validation Strategy、测试覆盖分析、硬件/固件职责、回归与签核标准。

# 十三、Staff Engineer 的参考考核标准

我建议对上述面试题不使用简单的「答案正确/错误」，而是按照四个能力维度评价。

| 级别                         | 评价标准                                                 |
| -------------------------- | ---------------------------------------------------- |
| Senior Engineer            | 能准确解释 CSR、Trap Entry、保存和恢复流程，并完成典型故障定位               |
| Strong Senior              | 能将 Trap 与 Thread Context、TA Isolation、MMU、SMP 结合起来分析 |
| Staff Engineer             | 能提出完整安全不变量、架构设计、故障恢复与系统验证方案                          |
| Staff+ / Architecture Lead | 能识别跨 OpenSBI/OP-TEE/Linux/硬件的系统性风险，并主导设计取舍与上游改进      |

尤其是 Q9、Q13、Q14、Q16 和 Q18，比较适合作为 Staff Engineer 架构深度评估。

这些题目没有统一的简短标准答案。较好的回答必须说明自己的硬件和软件假设，以及如何用实验验证结论。

# 十四、本章总结：真正需要掌握的五个核心设计思想

## 1. Trap 是硬件机制，Context Management 是软件责任

RISC-V CPU 只负责按照规范完成最小的 Trap 状态转换。

可信 Stack、完整 GPR 保存、FP/Vector Context、Thread State、Fault Recovery，都需要 OP-TEE 自己实现。

## 2. `sscratch` 是早期 Trap Entry 的关键桥梁

它解决了一个非常基础但容易被忽略的问题：

> 当 CPU 刚刚进入 S-mode，还没有任何可信 GPR 和 Stack 可供使用时，如何安全地找到当前 Hart 的内核上下文？

当前 OP-TEE 通过 `tp`、`sscratch` 和 `thread_core_local` 建立了一个精简的入口协议。

## 3. Trap Frame 既保存执行状态，也可以改变后续控制流

通过修改：

```
EPC
STATUS
SP
Return Arguments
```

内核既可以正常恢复 TA，也可以结束 TA 并返回 Kernel。

因此异常处理机制不仅用于处理错误，也是 TEE Syscall、用户态切换和 Trusted Thread Context Management 的基础。

## 4. Trap Boundary 与 Domain Boundary 不相同

TA U→S Trap 属于 OP-TEE Domain 内部的权限切换。

Linux S→M SBI Trap 可能涉及 OpenSBI Domain Management。

两者可能发生在同一个物理 hart 上，但由不同层次的固件和上下文结构管理。

不能把它们当作同一条普通的函数调用链。

## 5. 安全的 Trap Handler 必须维持系统级不变量

单纯能运行一个 Syscall、处理一个 Page Fault 并不能证明 Trap Handler 安全。

真正需要保证的是：

- User TA 无法通过异常入口提升权限。
- 所有被打断的寄存器状态可以正确保存和恢复。
- 异常不会错误地破坏其他 TA 或 Trusted Thread。
- 错误处理路径不会扩大不可信输入的权限。
- 跨 Domain 和跨 Hart 的状态不会泄露。
- 无法恢复的 Kernel 错误不会被伪装成正常执行。

## 建议保留的源码研究清单

后续继续研究 Chapter 10 和 Chapter 11 时，建议将以下文件作为固定参考。

| 文件                                                                                                                          | 核心关注点                                                     |
| --------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------- |
| [thread_rv.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_rv.S?utm_source=chatgpt.com)     | Trap Entry、Context Save/Restore、ECALL、Foreign Interrupt   |
| [thread_arch.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_arch.c?utm_source=chatgpt.com) | Syscall Dispatch、Interrupt Dispatch、Thread Suspend/Resume |
| [abort.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/abort.c?utm_source=chatgpt.com)             | Fault Classification、TA Panic、Kernel Panic、Unwind         |
| [entry.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/entry.S?utm_source=chatgpt.com)             | Boot Entry、Primary/Secondary Hart、Early Stack             |
| [riscv.h](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/include/riscv.h?utm_source=chatgpt.com)            | CSR Abstraction、XRET、RISC-V Register Definitions          |
| [core_mmu_arch.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/mm/core_mmu_arch.c?utm_source=chatgpt.com) | Page Table、ASID、TA Mapping、PTE Access Permissions         |
| [RISC-V Privileged Architecture](https://docs.riscv.org/reference/isa/priv/priv-csrs.html?utm_source=chatgpt.com)           | CSR 与 Trap 的 ISA 规范依据                                     |

Chapter 09 的最终验收目标： 你应该能够不看高级流程图，从 `thread_trap_vect` 的第一条 `csrrw` 开始，逐条解释一次 U-mode ECALL、一次 User Page Fault 和一次 Kernel Fatal Exception 的寄存器、栈、CSR、Privilege 与执行流变化，并能据此设计真实多核 SoC 的异常处理测试。

下一章 Chapter 10 — Trusted Thread & Context Switching 将在此基础上进一步解释 `thread_ctx_regs`、Trusted Thread 状态机、RPC Suspend/Resume，以及完整的跨 Hart Context Restore。
