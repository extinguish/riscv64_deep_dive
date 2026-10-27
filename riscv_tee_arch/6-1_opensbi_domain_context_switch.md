# Chapter 06 — OpenSBI Domain Context Switching

RISC-V OP-TEE Architecture & Porting · 核心架构篇 · Staff Engineer / Upstream Maintainer Level

## 本章目标

本章研究的问题是：

在同一个 RISC-V Hart 上，如何安全地暂停 Linux Domain 的执行，将 CPU 交给 OP-TEE Domain，然后准确恢复 Linux 原来的执行现场？

这不是简单地执行一次 `ecall`，也不是保存 32 个通用寄存器那么简单。它涉及五个彼此关联的状态：

1. Execution State：PC、GPR、FP/Vector Registers、Trap Context。
2. Privilege State：`mstatus`、`sstatus`、异常返回权限及相关控制寄存器。
3. Memory State：`satp`、页表、TLB、PMP 及其他平台隔离策略。
4. Interrupt State：中断使能、挂起状态、路由和不同 Domain 的 Interrupt Ownership。
5. Domain State：当前 Hart 属于哪个 Domain、目标 Domain 是否已初始化、如何返回调用者。

本章会结合当前 OpenSBI upstream 的实际实现展开，重点研究：

- `lib/sbi/sbi_domain_context.c`
- `lib/sbi/sbi_domain.c`
- `lib/sbi/sbi_hart_protection.c`
- `include/sbi/sbi_trap.h`
- RISC-V Trap Entry / Return Assembly

主线采用 Linux → OpenSBI → OP-TEE → OpenSBI → Linux。其中 OpenSBI 的通用 Domain Context Manager 已存在于 upstream，而 Linux 经 RPMI TEE Service 调用 OP-TEE 的完整路径仍涉及正在推进的集成工作。因此，讲解时会严格区分现有代码、目标架构和用于教学的设计伪代码。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+2



## 本章学习地图

| Section | 核心内容                                                  |
| ------- | ----------------------------------------------------- |
| 6.1     | 为什么 Domain Switch 不等于普通 Task Context Switch           |
| 6.2     | S-mode `ecall` 如何进入 M-mode，Trap Entry 如何保存上下文         |
| 6.3     | `mepc`、`mstatus`、`satp`、`stvec` 等关键 CSR               |
| 6.4     | OpenSBI Per-Hart Domain Context 数据结构                  |
| 6.5     | GPR / CSR / FP / Vector Context 的保存恢复                 |
| 6.6     | Domain Switch 与 PMP、硬件隔离配置顺序                          |
| 6.7     | `SFENCE.VMA`、TLB、ASID 与 Address Space Synchronization |
| 6.8     | Linux Domain 切换到 OP-TEE Domain                        |
| 6.9     | OP-TEE 返回 Linux Domain                                |
| 6.10    | Domain Switch 的安全不变量与异常恢复                             |
| 实验      | OpenSBI 源码追踪、QEMU/GDB、故障注入                            |
| 考核      | Staff Engineer 级工业场景与设计题                              |

# 6.1 为什么 Domain Switch 不等于普通 Task Context Switch？

## 6.1.1 从熟悉的 RTOS Task Switch 开始

假设 NuttX 中有两个 Task：

```
Task A                        Task B
  |                             |
  | running                     | ready
  |                             |
  +------ scheduler ------------+
            |
            | save Task A registers
            | restore Task B registers
            | switch stack/context
            v
Task A                        Task B
  |                             |
  | ready                       | running
```

对于普通的、同一内核地址空间内的 Task Switch，通常需要保存：

- Stack Pointer
- Callee-saved Registers
- Resume PC
- 与任务相关的其他 CPU Context

对于有独立用户地址空间的进程切换，还可能涉及页表、ASID、FPU 等状态。

但这类切换的基本前提是：

两个 Task 或 Process 仍然受同一个可信内核管理。

例如两个 Linux 用户进程：

```
Process A
    |
    | Linux Scheduler
    v
Process B
```

即使 Process A 与 Process B 之间有虚拟地址空间隔离，Linux Kernel 仍然能够管理二者的页表与内存。

这里的 Linux Kernel 本身是共同的可信管理者。

## 6.1.2 Domain Switch 的根本区别

现在考虑：

```
Linux Domain                   OP-TEE Domain
    |                               |
    |  S-mode                       | S-mode
    |                               |
    |         M-mode OpenSBI        |
    |                |              |
    +--------------->|------------->|
                     |
               Domain Switch
```

和传统 Task Switch 不同，这里有一个特别重要的安全目标：

Linux Kernel 即使遭到攻击者完全控制，也不能由 Linux 自己决定访问 OP-TEE 的私有内存和执行状态。

因此，必须由更高信任级别的组件负责：

1. 保存 Linux 的完整执行状态。
2. 限制 Linux 对 OP-TEE 资源的访问。
3. 恢复 OP-TEE 独立的执行状态。
4. 为 OP-TEE 配置有效的硬件隔离权限。
5. 防止 Linux 通过残留状态影响 OP-TEE。
6. 在切换结束后恢复 Linux 的执行环境。

在当前讨论的 OpenSBI 架构中，这个管理者是 M-mode Firmware。

## 6.1.3 三种 Context Switch 必须严格区分

| 对比项           | Task Switch      | Domain Switch          | Privilege Transition    |
| ------------- | ---------------- | ---------------------- | ----------------------- |
| 目的            | 切换调度实体           | 切换隔离执行环境               | 进入异常处理或返回               |
| 典型管理者         | OS Scheduler     | M-mode Context Manager | CPU Trap 机制与 Handler    |
| GPR 保存        | 根据调度/异常路径        | 保存完整可恢复执行状态            | 硬件不自动完整保存               |
| Address Space | 可切换              | 通常需要隔离切换               | 不一定切换                   |
| PMP/Firewall  | 通常不涉及            | 可能需要重新配置               | 不会自动重新配置                |
| 信任边界          | 一般在内核管理范围内       | 通常跨越安全隔离边界             | 是否跨越取决于使用方式             |
| 示例            | Linux Task A → B | Linux → OP-TEE         | S-mode `ecall` → M-mode |

特别注意：

`ecall` 仅仅提供从当前特权模式进入 Trap Handler 的机制，并不等于 CPU 自动完成了 Domain Switch。

同样，`mret` 也不会自动恢复某个 Domain 的全部内存和寄存器状态。

你可以将三者抽象为：

\\[ \text{Domain Switch} = \text{Execution Context Switch} + \text{Security Policy Switch} + \text{Resource Ownership Switch} \\]

其中，Security Policy Switch 是它与普通 OS 任务切换最本质的区别。

### 一个更深入的推论

假设 Linux 和 OP-TEE 都运行在 S-mode，并且 Linux 发生了完全的 Kernel Compromise。

攻击者可以任意修改 Linux 自己的 `satp`。

如果仅仅依靠 MMU 隔离，那么攻击者理论上可以构造页表，映射 OP-TEE 的物理内存。

因此：

在这种安全模型下，OP-TEE 的物理内存隔离不能建立在 Linux 自己控制的页表之上。

必须通过 Linux 无法任意修改的 PMP 或其他硬件安全机制，对物理访问实施强制限制。

这就是 Domain Context Switching 必须与硬件隔离设计结合，而不能仅仅当成一个汇编 Context Switch 来研究的原因。

# 6.2 从 S-mode ECALL 进入 M-mode 的实际执行过程

现在正式进入汇编和源码。

这一节先不考虑 Domain Switch，而是解决更基础的问题：

Linux 执行一条 `ecall` 指令后，CPU 如何进入 OpenSBI？原本的 Linux 执行现场存放在哪里？

## 6.2.1 先理解 CPU 硬件自动完成的动作

假设 Linux 当前执行在 S-mode：

```
li a7, SBI_EXT_ID
li a6, SBI_FUNC_ID
ecall
```

这里 `SBI_EXT_ID` 和 `SBI_FUNC_ID` 是示意符号。SBI 规范中通常使用 `a7` 表示 Extension ID，`a6` 表示 Function ID，`a0`～`a5` 传递参数。

我们假设此时：

```
Current Privilege = S-mode

PC       = 0x80201000
sp       = 0xFFFFFFC000010000
a0       = request_arg0
a7       = SBI_EXT_ID

mtvec    = OpenSBI Trap Entry
```

当 `ecall` 触发进入 M-mode 的异常时（S-mode ECALL 按标准进入 M-mode，不是由 `medeleg` 委托给 S-mode），RISC-V 硬件完成以下主要操作。

ECALL Trap Entry — 硬件动作

| `mepc`         | 保存 `ecall` 指令地址                      |
| -------------- | ------------------------------------ |
| `mcause`       | 设置为 9，即 Environment Call from S-mode |
| `mstatus.MPP`  | 记录 Trap 前运行在 S-mode                  |
| `mstatus.MPIE` | 保存先前 `MIE` 状态                        |
| `mstatus.MIE`  | 被清零，暂时关闭 M-mode 全局中断使能               |
| `pc`           | 跳转到 `mtvec` 指定的 M-mode Trap 入口       |

可以将这一过程理解为：

```
Before ECALL                    After ECALL
─────────────────               ─────────────────
Privilege = S                   Privilege = M

PC = Linux ECALL Addr           PC = mtvec.BASE
                                mepc = Linux ECALL Addr

mstatus.MIE = old               MPIE = old MIE
                                MIE  = 0
                                MPP  = S
```

注意几个经常容易误解的地方：

- RISC-V 硬件不会自动把全部 GPR 压栈。
- 硬件不会自动保存整个 Linux Stack。
- 硬件不会因为进入 M-mode 就自动修改 `satp`。
- 硬件不会自动重新配置 PMP。
- 硬件也不会自动切换到一个新的 OpenSBI Stack Pointer。

这些都需要软件自行处理。

对你熟悉的 Arm 架构来说，这一点类似于 Arm A-profile 异常入口仅通过相关异常寄存器保存异常返回状态，完整 GPR Context 仍由异常入口软件负责保存。

不过必须区分：Arm 的 Security State 切换和 RISC-V 的 OpenSBI Domain 切换不是相同的硬件机制。

## 6.2.2 OpenSBI 的实际 Trap Entry

在当前 OpenSBI upstream 中，通用 Firmware 的 M-mode Trap 汇编入口位于：

`firmware/fw_base.S`

这个文件中可以找到 `_trap_handler`，以及若干 Trap Save/Restore 宏。

实际汇编入口可以概括为：

```
_trap_handler
    |
    +-- TRAP_SAVE_AND_SETUP_SP_T0
    |
    +-- TRAP_SAVE_MEPC_MSTATUS
    |
    +-- TRAP_SAVE_GENERAL_REGS_EXCEPT_SP_T0
    |
    +-- TRAP_SAVE_INFO
    |
    +-- TRAP_CALL_C_ROUTINE
    |      |
    |      +-- sbi_trap_handler()
    |
    +-- TRAP_RESTORE_GENERAL_REGS_EXCEPT_A0_T0
    |
    +-- TRAP_RESTORE_MEPC_MSTATUS
    |
    +-- TRAP_RESTORE_A0_T0
    |
    +-- mret
```

这就是当前代码中的真实宏调用关系。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 第一阶段：切换至安全的 M-mode Trap Stack

最值得注意的就是：

`TRAP_SAVE_AND_SETUP_SP_T0`

它的设计思路是：

1. 利用 `mscratch` 找到当前 hart 对应的 OpenSBI Scratch。
2. 暂存必要的临时寄存器。
3. 根据 Trap 来源判断使用哪个 Stack。
4. 在 M-mode Trap Stack 上分配 Trap Context。
5. 保存原始 SP 和临时寄存器。

关键汇编指令之一是：

```
csrrw tp, mscratch, tp
```

这是一条原子的 CSR Read/Write 指令。

它将原来的 `tp` 和 `mscratch` 进行交换，使 OpenSBI 能在尚未建立完整 Trap Frame 时取得对应的 Scratch Pointer，同时保留原来的 `tp`。

实际代码接下来还会用 `mstatus.MPP` 判断 Trap 是否来自 M-mode，从而选择已有 M-mode Stack 或 Hart 对应的异常栈。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么不能直接使用 Linux 的 `sp`？

因为 Linux 是不可信的。

如果 OpenSBI 在执行敏感逻辑时，使用 Linux 可以控制的 Stack，那么攻击者可能通过构造 Stack 内容、栈指针或内存映射影响 M-mode Firmware。

因此需要由 OpenSBI 管理自己的受保护 Stack。

### 第二阶段：保存完整 GPR Context

对应：

`TRAP_SAVE_GENERAL_REGS_EXCEPT_SP_T0`

OpenSBI 将 GPR 保存到 `struct sbi_trap_regs`。

为什么要保存 Caller-saved 和 Callee-saved 的全部相关寄存器？

因为 Trap 是异步于普通 C 调用约定的控制流转移。不能假设所有 CPU 寄存器都符合正常函数调用时的保存约定。

特别是 `a0`～`a7`，它们既包含原有执行状态，也构成 SBI 调用接口。

### 第三阶段：调用 C Handler

汇编入口最终进入：

```
sbi_trap_handler()
    |
    +-- Check mcause
    |
    +-- Interrupt Handler
    |
    +-- ECALL Handler
    |
    +-- Illegal Instruction Handler
    |
    +-- Access Fault Handler
    |
    +-- Exception Redirection
```

其中 S-mode ECALL 路径为：

```
sbi_trap_handler()
    |
    +-- CAUSE_SUPERVISOR_ECALL
            |
            v
        sbi_ecall_handler()
            |
            v
        SBI Extension Handler
```

这是源码中的实际 Dispatch 路径。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



随后，如果某个已注册的 SBI Service Handler 确实要求切换 Domain，就可以通过相应的 Context Manager 进入目标 Domain。

但要特别强调：

不是每次 SBI ECALL 都会发生 Domain Switch。

例如普通的 SBI Timer 或 Hart State Management 调用，通常由 OpenSBI 完成操作后返回原来的 Domain。

只有需要将控制权交给另一个 Domain 的服务，才涉及我们本章研究的 Domain Context Switching。

## 6.2.3 `mret` 为什么可以返回另一个 Domain？

这是本章最关键的一个知识点。

很多人看到：

```
mret
```

会直觉地认为：

「它一定返回刚刚触发 `ecall` 的 Linux。」

实际上并非如此。

`mret` 的核心职责是按照当前 `mepc` 和 `mstatus.MPP` 所描述的状态完成异常返回。

假设 OpenSBI 保存了 Linux 原来的 Trap Frame，随后将当前 Trap Frame 替换为 OP-TEE 的 Trap Frame：

```
Before Domain Switch:

Current Trap Frame:
    mepc = Linux Resume PC
    mstatus.MPP = S
    GPR = Linux Registers
```

切换之后：

```
After Domain Switch:

Current Trap Frame:
    mepc = OP-TEE Resume PC
    mstatus.MPP = S
    GPR = OP-TEE Registers
```

随后汇编退出路径从这个更新后的 Trap Frame 恢复寄存器、`mepc` 和 `mstatus`。

最后：

```
mret
```

就可以返回 OP-TEE，而不是 Linux。

这里的核心设计思想是：

> Domain Switch 的实现不需要让 `mret` 理解 Domain。它只需要由 M-mode Firmware 在异常返回前准备好目标 Domain 的 CPU Context 和安全隔离环境。

这个思想与你熟悉的 RTOS Exception Return 有一定相似之处：异常返回机制负责恢复执行，而由可信软件决定最终恢复哪一个上下文。

但 Domain Switch 还必须处理物理访问权限等安全状态。

# 6.3 Domain Context 必须保存哪些 CSR？

不能简单地回答「保存所有 CSR」。

我们需要按照职责分层。

## 6.3.1 Machine Trap Context

第一组寄存器描述的是：进入 M-mode 前 CPU 执行在哪里，以及应该怎样退出 M-mode。

| CSR        | 主要意义                      | 重要性         |
| ---------- | ------------------------- | ----------- |
| `mepc`     | M-mode Trap 返回地址          | 极高          |
| `mstatus`  | MPP、MPIE、MIE 等异常与权限状态     | 极高          |
| `mcause`   | M-mode Trap 原因            | 主要用于处理、诊断   |
| `mtval`    | Trap 附加信息                 | 主要用于异常处理    |
| `mscratch` | OpenSBI Per-Hart Scratch  | M-mode 自身管理 |
| `mtvec`    | M-mode Trap Handler Entry | M-mode 自身管理 |

这里尤其需要区分：

`mepc` 不等于 `sepc`。

- `mepc` 对应 M-mode Trap 的返回状态。
- `sepc` 对应 S-mode Trap 的返回状态。

假设 Linux 在处理中断时进入了 OpenSBI：

```
Linux:
    sepc = Interrupted Linux PC

OpenSBI:
    mepc = Address of Linux ECALL
```

此时两个 CSR 同时具有各自的意义。

如果 Domain Switch 只保存 `mepc` 而没有保存 `sepc`，Linux 再次恢复后，其原有 S-mode Trap 状态可能被 OP-TEE 的状态覆盖。

## 6.3.2 Supervisor Architectural Context

第二组寄存器属于各自 Domain 的 S-mode 执行状态。

当前 `struct hart_context` 中保存了以下内容：

| CSR          | 作用                                   | 如果遗漏恢复会怎样                   |
| ------------ | ------------------------------------ | --------------------------- |
| `sstatus`    | S-mode 状态、权限及扩展状态                    | 权限位、FP/Vector 状态不一致         |
| `sie`        | Supervisor Interrupt Enable          | 中断使能状态错误                    |
| `stvec`      | S-mode Trap Handler                  | Trap 跳入另一个 Domain 的 Handler |
| `sscratch`   | S-mode Trap Scratch                  | 栈和 Per-Hart 指针混乱            |
| `sepc`       | S-mode Trap Return PC                | 异常返回到错误地址                   |
| `scause`     | S-mode Trap Cause                    | 原始 Trap 信息丢失                |
| `stval`      | Trap 附加信息                            | Fault 诊断和处理错误               |
| `sip`        | Supervisor Interrupt Pending         | 挂起中断视图不正确                   |
| `satp`       | 地址转换模式、ASID、页表根                      | 访问错误地址空间                    |
| `scounteren` | Counter 访问控制                         | Counter 权限不一致               |
| `senvcfg`    | Supervisor Environment Configuration | 环境行为或权限改变                   |
| `srmcfg`     | 可选 Supervisor Resource Management 配置 | 对应扩展状态错误                    |

这些字段来自当前 OpenSBI 的真实 `hart_context` 定义。部分字段具有架构版本或扩展存在性条件，并且 CSR 的可写位受 WARL/WPRI 等规则限制。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 为什么 `stvec` 必须按 Domain 保存？

假设：

```
Linux Domain:
    stvec = 0x80200000

OP-TEE Domain:
    stvec = 0x90001000
```

切换到 OP-TEE 后发生 S-mode Trap。

如果 `stvec` 仍然指向 Linux：

```
OP-TEE Trap
    |
    v
stvec = Linux Handler
```

可能导致执行流错误，甚至产生安全边界问题。

因此 `stvec` 是必须纳入 Domain Context 的执行状态。

### 为什么 `satp` 必须切换？

假设：

```
Linux Domain:
    satp.PPN = Linux Root Page Table

OP-TEE Domain:
    satp.PPN = OP-TEE Root Page Table
```

如果切换到 OP-TEE 之后仍然使用 Linux 的 `satp`，那么 OP-TEE 的指令取址、数据访问和 Stack Access 可能会在完全错误的页表中解析。

因此 `satp` 需要切换到目标 Domain 的地址转换上下文。

不过：

`satp` 切换和 TLB 同步是两个不同的问题。

RISC-V 并不保证写入 `satp` 就自动完成所需的地址转换同步。

这一点我们会在 6.7 详细分析。

## 6.3.3 哪些 CSR 不应该简单地视为每个 Domain 的独立寄存器？

例如：

- `mtvec`
- `mscratch`
- `medeleg`
- `mideleg`
- `mstatus.MPRV`
- 平台特定的 M-mode Security Control Registers

这些状态不能一律按 `hart_context` 的 S-mode CSR 交换方式处理。

OpenSBI 的 `mtvec`、`mscratch` 通常由可信 M-mode Firmware 自身管理。

`medeleg`、`mideleg` 决定 Trap Delegation Policy，它们涉及更高层级的异常路由，而不是普通 S-mode 进程的寄存器状态。

如果平台设计需要 Domain-specific Delegation Policy，就必须在可信固件层明确设计、验证其切换或虚拟化机制，不能假设当前通用 Domain Context Manager 已自动处理。

类似地，开启 H Extension、AIA 或其他平台安全扩展后，还需要审查：

- `hstatus`、`hgatp`、`vsatp` 等虚拟化相关状态。
- IMSIC Guest File、Interrupt Routing。
- Debug、Trigger、Performance Monitoring 的访问控制。
- 可能带有安全敏感性的架构扩展寄存器。

其中哪些需要保存、哪些应该保持 M-mode 专有，取决于目标 Domain 的实际执行模式、可访问权限及 SoC 安全设计。

这也是移植到工业 SoC 时不能只复制现有 Context Structure 的原因。

# 6.4 OpenSBI Per-Hart Domain Context 数据结构

现在进入 OpenSBI 的核心 C 实现。

## 6.4.1 为什么是 Per-Hart × Per-Domain？

考虑一个四核 SoC：

```
               Linux Domain     OP-TEE Domain
                    |                 |
Hart 0         Linux Context 0   TEE Context 0
Hart 1         Linux Context 1   TEE Context 1
Hart 2         Linux Context 2   TEE Context 2
Hart 3         Linux Context 3   TEE Context 3
```

为什么不能只为 Linux 保存一个 Context、为 OP-TEE 保存一个 Context？

因为不同 Hart 有独立的 CPU Architectural State。

例如：

```
Hart 0:
    Linux PC = 0x80201200

Hart 1:
    Linux PC = 0x80208000
```

它们的 GPR、Trap State、Stack 和 CSR 也不一样。

因此每个 hart、每个可运行的 Domain，都可能需要一份独立 Context。

数学上：

\\[ Context = f(Domain,\ Hart) \\]

而不是：

\\[ Context = f(Domain) \\]

## 6.4.2 `struct hart_context` 的真实结构

OpenSBI 在 `lib/sbi/sbi_domain_context.c` 中定义了 `struct hart_context`。

为了便于理解，我们将实际结构按职责整理为以下布局（不是逐字复制源码）：

```
struct hart_context
|
+-- trap_ctx
|     |
|     +-- GPR[0..31]
|     +-- mepc
|     +-- mstatus
|     +-- Trap information
|
+-- Supervisor CSR Context
|     |
|     +-- sstatus
|     +-- sie / sip
|     +-- stvec / sscratch
|     +-- sepc / scause / stval
|     +-- satp
|     +-- scounteren / senvcfg / srmcfg
|
+-- Extended CPU Context
|     |
|     +-- fp_ctx
|     +-- vec_ctx
|
+-- Domain Metadata
      |
      +-- dom
      +-- prev_ctx
      +-- initialized
```

源码：[sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



它可以分成四个部分。

第一部分：Trap Context

负责恢复发生 Domain Switch 前的 GPR 和 M-mode Trap Return State。

第二部分：Supervisor CSR Context

负责保存一个 S-mode Domain 自身的中断、异常、地址转换等状态。

第三部分：FP/Vector Context

确保浮点和向量寄存器不会在 Domain 之间混用。

第四部分：Domain Metadata

包括 Context 属于哪个 Domain、上一个调用者是谁，以及 Context 是否初始化。

## 6.4.3 `prev_ctx` 的设计为什么重要？

假设：

```
Linux Domain
    |
    | enter OP-TEE
    v
OP-TEE Domain
    |
    | exit
    v
Linux Domain
```

在进入 OP-TEE 时，需要记录：

```
OPTEE_CTX.prev_ctx = LINUX_CTX
```

退出 OP-TEE 时就知道需要恢复哪个 Context。

你可以把它理解为一种 Continuation（执行续体）关系：

```
Current Domain Context
    |
    +-- previous caller context
```

但是千万不要直接将 `prev_ctx` 理解为传统操作系统的 Task Parent Pointer。

它表达的是 Domain Context Enter/Exit 之间的调用恢复关系。

在支持嵌套进入的设计中，这类关系需要受到严格约束，否则可能产生错误返回、重入甚至循环调用的问题。

当前 `sbi_domain_context_exit()` 的基本行为是：

- 存在 `prev_ctx`：尝试返回原调用者。
- 不存在 `prev_ctx`：根据 Domain 初始化状态选择下一步运行的 Domain。
- 不存在合适 Context：返回错误。

当前源码在没有 `prev_ctx` 时会考虑尚未初始化 Domain 的 `init_order`，体现了 Domain Context Manager 还承担启动编排的职责。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 6.4.4 `initialized` 为什么不能省略？

第一次进入 OP-TEE 与再次进入 OP-TEE，执行语义不同。

First Entry

```
initialized = false
      |
      v
Boot OP-TEE
      |
      v
OP-TEE Startup
```

目标 Domain 尚未拥有可恢复的历史执行状态。

Resume Entry

```
initialized = true
      |
      v
Restore Context
      |
      v
Resume OP-TEE
```

目标 Domain 曾经运行过，已具有可恢复的执行状态。

如果首次进入时误把未初始化的 Context 当作有效 Context 恢复，可能恢复到无效 PC、无效 Stack，或者错误的特权状态。

当前实现会在首次启动目标 Domain 时，通过 `sbi_hart_switch_mode()` 使用该 Domain 配置的下一阶段入口信息；非 Boot Hart 则可能进入 HSM Stop 路径。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 6.4.5 `possible_harts` 和 `assigned_harts` 的区别

这个区别也值得深入理解：

- `possible_harts`：某个 Domain 允许在哪些 hart 上运行。
- `assigned_harts`：某个 Domain 当前拥有哪些 hart。

例如：

```
Domain OP-TEE:
    possible_harts = {0, 1, 2, 3}
    assigned_harts = {2}

Domain Linux:
    possible_harts = {0, 1, 2, 3}
    assigned_harts = {0, 1, 3}
```

一个 hart 从 Linux 切入 OP-TEE 时，当前归属需要更新。

这并不是简单的统计信息。

OpenSBI 某些 HSM、IPI 或 Domain 管理操作会依赖这些数据判断 Hart Ownership 和操作权限。

因此：

Domain Context Switch 不只是 CPU Context 的变化，也是 Hart Ownership 的变化。

# 6.5 GPR、CSR、FP 和 Vector Context 的完整保存恢复

我们开始分析最核心的 `switch_to_next_domain_context()`。

## 6.5.1 从源码理解切换顺序

当前 upstream 的核心函数是：

```
switch_to_next_domain_context(current, target)
```

这里用重新整理过的伪代码表达执行逻辑，保留真实的操作顺序：

```
int domain_switch_model(current, target)
{
    /* 1. 更新当前 hart 的 Domain 归属 */
    update_hart_ownership(current, target);

    /* 2. 保存当前 S-mode CSR，恢复目标 CSR */
    swap_supervisor_csrs(current, target);

    /* 3. 保存当前 FP 状态，恢复目标 FP 状态 */
    switch_fp_context(current, target);

    /* 4. 保存当前 Vector，恢复目标 Vector */
    switch_vector_context(current, target);

    /* 5. 保存并替换 M-mode Trap Frame */
    swap_trap_context(current, target);

    /* 6. 重新配置硬件隔离 */
    reconfigure_domain_protection(current, target);

    /* 7. 更新初始化状态 */
    current->initialized = true;

    /* 8. 目标尚未启动时执行 Boot Path */
    if (!target->initialized)
        boot_or_stop_target_hart(target);

    return 0;
}
```

这段是对真实源码的教学抽象，不是一个可直接编译的 OpenSBI 函数。

对应实际实现见 `sbi_domain_context.c`，而且后续我们会分析这段顺序中的重要安全审查点。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 6.5.2 `csr_swap()` 的设计思想

源码针对许多 Supervisor CSR 使用交换操作。

我们用等价的概念代码说明：

```
unsigned long old_value = read_csr(CSR);
write_csr(CSR, target->value);
current->value = old_value;
```

交换前：

```
Hardware CSR = Linux Value

Linux Context = previous saved value

TEE Context = TEE Value
```

交换后：

```
Hardware CSR = TEE Value

Linux Context = Linux Value
```

这个设计的优势是：

- 保存当前硬件状态。
- 恢复目标硬件状态。
- 不必针对每个 CSR 写两套处理逻辑。

但它依赖两个前提：

第一，目标 Context 中的 CSR 值已经正确初始化，或者即将走明确的 First Boot 路径。

第二，恢复的 CSR 不会导致切换过程进入不可控的异常或访问状态。

对于安全敏感 CSR，还需要检查当前架构版本支持哪些位、哪些位可写，以及是否需要额外的同步操作。

## 6.5.3 为什么 GPR 不是在同一个循环中逐个 `csr_swap`？

因为 GPR 的保存方式和 CSR 不同。

回忆 6.2：

```
S-mode ECALL
    |
    v
M-mode Trap Entry
    |
    +-- Save GPR
    +-- Save mepc
    +-- Save mstatus
    |
    v
sbi_trap_handler()
```

当执行到 `sbi_domain_context_enter()` 时，原始 GPR 已经被保存到当前 Trap Frame。

所以 Context Manager 不必再从正在执行 C 代码的寄存器里提取 Linux 的 GPR。

它需要做的是：

```
M-mode Trap Frame
    |
    | copy out
    v
Linux hart_context.trap_ctx


OP-TEE hart_context.trap_ctx
    |
    | copy in
    v
M-mode Trap Frame
```

这样当 OpenSBI 最终从 Trap Handler 返回时，底层汇编从当前 Trap Frame 恢复的就是目标 Domain 的 GPR。

这是一种非常重要的设计：

通过修改 Trap Frame 来决定异常返回目标，而不是在 C 函数里直接跳转到另一个 Domain。

它将 Domain Context Manager 与底层的汇编恢复过程解耦。

## 6.5.4 FP Context 为什么需要独立保存？

假设 Linux 使用浮点寄存器：

```
Linux:
    f0 = 1.234
    f1 = 5.678
```

切换到 OP-TEE：

```
OP-TEE:
    f0 = Sensitive Intermediate Value
    f1 = Cryptographic Calculation Data
```

如果 FP 状态没有在 Domain 切换时隔离，那么下一次恢复 Linux 时，Linux 可能观察到不属于自己的 FP 状态。

这会成为潜在的信息泄露路径。

在当前 OpenSBI 中，`sbi_domain_context.c` 对支持 F/D 扩展的 hart 使用 FP Save/Restore Helper，采取 Eager Context Switching。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里的 Eager 是指：

无论当前 Domain 是否刚刚实际使用 FP，只要执行该切换路径并满足相应硬件扩展条件，就执行其状态管理操作。

这会产生性能开销，但实现逻辑相对直接，也更容易建立隔离不变量。

不过工业级实现仍然需要检查 FP 状态的初始化、扩展可用性和 `FS` 状态位处理，不能只看到调用 Save/Restore Helper 就认定所有安全条件已经满足。

## 6.5.5 Vector Context 为什么更复杂？

RISC-V Vector 状态不仅包括 `v0`～`v31`。

还包括：

- `vl`
- `vtype`
- `vstart`
- `vcsr`，包括舍入和饱和相关状态
- 与 Vector 状态可用性有关的 `VS` 位

Vector Context 的保存成本还随 VLEN 增长。

例如，只计算 32 个向量寄存器的数据载荷：

| VLEN      | 32 个 Vector Register 数据总量 |
| --------- | ------------------------- |
| 128 bits  | 512 bytes                 |
| 256 bits  | 1 KiB                     |
| 512 bits  | 2 KiB                     |
| 1024 bits | 4 KiB                     |

这还不包括控制寄存器、Context 结构及额外管理开销。

因此，有时从性能角度会考虑 Lazy Context Switching，但安全系统中的 Lazy Switch 必须严格处理状态所有权、禁用访问、Trap 后恢复及首次初始化，否则可能引发跨 Domain 数据泄露。

### 工程判断

对于一个希望先把 RISC-V OP-TEE bring-up 做稳定的团队：

我更倾向于先采用便于验证的 Eager Switch，建立 FP/Vector 状态隔离测试，再通过性能测量判断是否值得引入 Lazy 机制。

这不是说 Eager 永远更好，而是安全边界正确性需要优先于未经测量的性能优化。

# 6.6 Domain Switch 与 PMP 重新配置的顺序

前面已经分析了 CPU Execution Context 的保存和恢复。

但到目前为止，我们其实只解决了：

「如何让 CPU 从 Linux 的代码切换到 OP-TEE 的代码？」

还没有解决：

为什么切换后两个 Domain 仍然具有各自独立的安全权限？

这就需要研究 Physical Memory Protection。

## 6.6.1 从具体内存布局理解问题

假设某个 RV64 SoC 的物理内存布局如下。

地址仅用于教学，不代表实际平台布局。

```
Physical Address Space

0x80000000
    |
    | Linux Memory
    | 512 MiB
    |
0xA0000000
    |
    | OP-TEE Private Memory
    | 64 MiB
    |
0xA4000000
    |
    | Shared Memory
    | 16 MiB
    |
0xA5000000
```

我们希望实现以下访问控制：

| 物理区域                  | Linux Domain | OP-TEE Domain |
| --------------------- | ------------ | ------------- |
| Linux Memory          | R/W/X，按需分区   | 经授权访问         |
| OP-TEE Private Memory | 禁止访问         | 按用途 R/W/X     |
| Shared Memory         | R/W          | 受限 R/W        |
| OpenSBI Firmware      | 禁止访问         | 禁止直接访问        |

注意，表中的 R/W/X 是抽象的 Domain 权限，并不是建议对所有页面同时设置 RWX。

在实际平台中，还需要对代码、只读数据、堆、栈以及设备 MMIO 实施细粒度权限控制。

## 6.6.2 关键问题：`satp` 无法代替 PMP

假设 Linux 被攻击者完全控制。

攻击者可以修改 Linux Page Table：

```
VA: 0xFFFF000000100000

        |
        v

PA: 0xA0000000
    OP-TEE Private RAM
```

仅仅从地址翻译角度来说，如果没有更底层的限制，Linux 可以尝试构造这种映射。

但 PMP 可以进一步限制 S-mode 对物理地址的访问。

```
Linux Virtual Address
        |
        v
      Sv39
        |
        v
Physical Address = OP-TEE RAM
        |
        v
      PMP Check
        |
        +-- Linux Domain: DENY
        |
        v
   Access Fault
```

这里最关键的设计思想是：

MMU 负责解释虚拟地址如何映射到物理地址，而 PMP 提供独立于不可信 S-mode 页表的物理访问限制。

PMP 对页表遍历产生的隐式物理访问同样适用，并不只检查最终 Load/Store 的目标物理地址。

[image](https://www.google.com/s2/favicons?domain=https://riscv-specs.timhutt.co.uk\&sz=32)

riscv-specs.timhutt.co.uk



## 6.6.3 为什么切换时需要重新配置 PMP？

因为 PMP 的配置是与 Hart 相关的 CPU 权限状态，而不是自动跟随 OpenSBI Domain 对象变化的硬件实体。

假设 Hart 0 正在执行 Linux：

```
Hart 0 PMP Policy

Linux RAM      -> Allow
OP-TEE RAM     -> Deny
Shared RAM     -> Allow
```

当 Hart 0 准备进入 OP-TEE：

```
Hart 0 PMP Policy

Linux RAM      -> Restricted
OP-TEE RAM     -> Allow
Shared RAM     -> Allow
```

因此 Domain Switch 必须使硬件保护状态与目标 Domain 的权限定义保持一致。

OpenSBI 当前使用 `sbi_hart_protection_reconfigure()` 执行相关的硬件保护重新配置。这个接口支持注册不同类别的 Hart Protection Mechanism，而不再只是把整个抽象固定在直接写 PMP CSR 上。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



实际需要执行的动作取决于选择的机制，例如 PMP 或其他平台特定的保护实现。

## 6.6.4 正确的切换顺序是什么？

这个问题比表面看起来复杂。

一种符合安全目标的抽象流程是：

```
Linux S-mode
     |
     | ECALL
     v
OpenSBI M-mode
     |
     | 1. Save Linux Trap Context
     |
     | 2. Validate Target Domain
     |
     | 3. Save Linux S-mode CSR Context
     |
     | 4. Prepare OP-TEE CPU Context
     |
     | 5. Reconfigure Hardware Isolation
     |
     | 6. Synchronize MMU/PMP State
     |
     | 7. Commit Target Domain State
     |
     | 8. Restore OP-TEE Trap Context
     |
     | MRET
     v
OP-TEE S-mode
```

这是一份安全事务设计示意，不是对当前 OpenSBI C 语句顺序的逐行复述。

实际源码把 Hart Ownership 更新、CSR 交换、FP/Vector 恢复、Trap Context 交换和 Protection Reconfiguration 分成了相应的操作阶段。

这里需要明确一条安全不变量：

\\[ \text{Return to Domain B} \implies \text{Hardware Protection == Policy(B)} \\]

也就是说，在 CPU 返回 OP-TEE S-mode 之前，目标 Domain 的权限必须已经有效。

如果某个配置操作失败，绝不能直接把 CPU 返回到已经准备好的 OP-TEE PC，假装切换成功。

### 安全审查点：错误处理是否完整？

当前源码值得特别注意的一处是：

`switch_to_next_domain_context()` 会调用 `sbi_hart_protection_reconfigure()`，后者具有 `int` 返回值，可报告配置失败；但在所检索的源码版本中，调用处没有显式检查其返回值。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



这本身不能直接证明存在可利用的漏洞，因为还要检查实际 Backend 的错误路径以及是否允许返回。但是，它确实是一个值得开展 upstream Review 的问题。

需要回答：

1. 保护配置可能因什么原因失败？
2. 如果失败，旧 Domain 的保护配置是否仍然完整？
3. 是否存在旧策略已撤销、新策略尚未建立的中间状态？
4. 失败后应该 Rollback、停止当前 Hart，还是触发平台级 Fatal Error？
5. 是否能够编写 Fault Injection Test 证明失败时仍然安全？

这是一个很典型的 Staff Engineer 问题：不是看接口正常时是否工作，而是关注失败时是否仍满足安全不变量。

## 6.6.5 PMP 更新过程中会出现短暂的安全窗口吗？

有可能存在需要审查的中间状态，但不能直接推断它必然可被攻击。

需要区分：

- 执行环境：进行策略更新时 CPU 正在 M-mode 执行可信代码。
- 策略中间态：PMP/Firewall 的一系列寄存器更新不一定具有整体原子性。
- 异常和并发：NMI、嵌套 Trap、其他 hart、DMA 等可能拥有不同的观察路径。

因此工程上需要保证：

在策略切换完成之前，不允许不可信 S/U-mode 在过宽权限下执行。

对于系统级 Firewall，还要验证不同 Bus Master 的权限更新顺序和硬件完成语义。

同时，不应采用一个没有证据的通用假设：

「先禁止所有访问，再重新允许访问，就一定安全。」

如果硬件配置寄存器本身所在的总线访问也受相同策略约束，错误的撤销顺序可能导致无法继续完成配置。

正确顺序应由硬件机制、可信代码执行条件和故障恢复模型共同决定。

# 6.7 `SFENCE.VMA`、TLB 与 Address Space Synchronization

这一节是我认为 RISC-V OP-TEE 移植中最值得深入掌握的内存体系结构知识之一。

## 6.7.1 为什么修改 `satp` 后还需要考虑 TLB？

我们先建立一个简化模型：

```
Virtual Address
      |
      v
   TLB Lookup
      |
      +-- Hit --> Physical Address
      |
      +-- Miss
            |
            v
        Page Table Walk
            |
            v
        Physical Address
```

对于 RV64 Sv39：

```
satp
 |
 +-- MODE = Sv39
 |
 +-- ASID = Address Space Identifier
 |
 +-- PPN  = Root Page Table PPN
```

假设 Linux 和 OP-TEE 使用两个不同的页表：

```
Linux:
    satp.PPN = 0x10000
    satp.ASID = 0

OP-TEE:
    satp.PPN = 0x20000
    satp.ASID = 0
```

二者使用相同 ASID，却对应不同的虚拟地址空间。

如果 TLB 中仍存在 Linux 的旧地址翻译，并且没有进行必要的同步，就可能出现旧翻译被错误复用的情况。

例如：

```
Linux VA 0x400000
    -> Linux PA 0x81000000

OP-TEE VA 0x400000
    -> OP-TEE PA 0xA0100000
```

两者虚拟地址相同，但物理地址不同。

在 ASID 不能区分这两个翻译上下文的情况下，切换必须进行恰当的 TLB 同步。

## 6.7.2 `SFENCE.VMA` 的本质

`SFENCE.VMA` 全称是 Supervisor Memory-Management Fence。

其作用不应简单理解为「清空 TLB」。

它的架构语义包括：

将软件对内存管理数据结构的更新，与当前 hart 后续的隐式地址翻译访问建立必要的顺序和可见性约束。

它也用于使本地地址转换缓存中的相关条目失效。

常见形式：

```
sfence.vma x0, x0
```

表示执行全局范围的本地地址转换同步，包括 Global Mapping。

以及：

```
sfence.vma x0, a0
```

其中 `a0` 携带指定 ASID，用于针对该 ASID 的同步；不包含 Global Mapping 的失效。

对应的具体失效范围还与 `rs1` 指定的虚拟地址有关。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V 文档

+1



## 6.7.3 写入 `satp` 是否自动使 TLB 失效？

答案：不是。

RISC-V 的设计允许使用 ASID 减少 Context Switch 时不必要的 TLB Flush。

我们分析三个场景：

| 场景                        | 是否需要同步        |
| ------------------------- | ------------- |
| 切换到不同地址空间，使用同一 ASID       | 通常需要处理旧翻译     |
| 切换到不同地址空间，使用不同且正确管理的 ASID | 不一定需要全量 Flush |
| 修改了某个地址空间的 PTE            | 需要按规范处理对应翻译同步 |

例如：

```
Linux:
    ASID = 10

OP-TEE:
    ASID = 20
```

如果地址空间和 ASID 生命周期正确管理，切换时可以保留不属于当前地址空间的 TLB Entry，并通过 ASID 区分它们。

但如果 ASID 发生复用，就需要执行对应的同步操作。

注意，ASID 的含义是 Hart-local 的，因此多核系统需要独立考虑每个 Hart 的地址空间与 ASID 管理。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V 文档



## 6.7.4 修改 PMP 后为什么也要执行 `SFENCE.VMA`？

这是一个经常被忽略的知识点。

RISC-V 允许地址转换结构和相关权限信息被缓存。

当 M-mode 修改 PMP 配置后，旧的地址转换或权限相关缓存状态可能不再符合新的硬件策略。

因此，在实现分页虚拟内存的 Hart 上，PMP 更新需要执行相应的 `SFENCE.VMA`，即使当前虚拟内存并未启用。

尤其当 PMP 更新影响页表所在的物理区域，或者影响页表指向的物理内存时，这项同步要求非常关键。

RISC-V Privileged Architecture 对此已有明确要求。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V 文档

+1



这意味着 Domain Switch 并不是：

```
Write PMP
    |
    v
MRET
```

而需要保证：

```
Write PMP / Hardware Protection
    |
    v
Required Translation Synchronization
    |
    v
MRET
```

当前 OpenSBI 的 Domain Context Switch 源码也明确要求保护配置路径完成相应的 SFENCE/HFENCE 同步。具体执行方式需要在选用的 Protection Backend 中检查，不能只看调用处的注释。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 6.7.5 多核场景：为什么本地 `SFENCE.VMA` 不够？

假设：

```
Hart 0                       Hart 1

Modify shared Page Table     Running with old TLB
        |
        |
SFENCE.VMA (Hart 0)
        |
        v
Hart 0 sees new mapping      Hart 1 may still see old mapping
```

`SFENCE.VMA` 的同步范围是本地 Hart。

因此，如果某个 Page Table 修改影响了多个正在使用该地址空间的 hart，还需要 Remote TLB Shootdown。

典型流程：

```
Hart 0
    |
    | Update PTE
    |
    | Publish page-table changes
    |
    | IPI / SBI RFENCE
    v
Hart 1
    |
    | Receive request
    |
    | SFENCE.VMA
    |
    | Ack completion
    v
Hart 0
    |
    | Continue after required acknowledgements
```

这里有两类不同问题：

Domain Switch

当前 Hart 从 Linux 切换到 OP-TEE，需要使该 Hart 的 Address Translation 和保护状态正确。

Page Table Modification

某个地址空间的映射内容改变，可能要求所有相关 Hart 更新其地址转换缓存。

不能把二者混为一谈。

## 6.7.6 一个非常典型的安全问题

假设：

1. Hart 0 修改了一个 TA 的 PTE，撤销对某个内存页面的访问。
2. Hart 1 同时运行该 TA。
3. Hart 0 只执行本地 `SFENCE.VMA`。
4. Hart 1 的旧 TLB 仍然保留原权限。

那么 Hart 1 可能继续依据旧的地址翻译和权限访问该页，直到相应同步完成。

对普通 Linux，这可能是内存管理正确性问题；对于 TEE，它还可能构成隔离边界问题。

更进一步，在解除映射、清理内存并将页面分配给另一个 TA 之前，必须保证旧访问路径不再有效。

这就是为什么内存回收、页表同步和安全隔离需要放在同一套证明逻辑里。

# 6.8 Linux Domain 切入 OP-TEE Domain：完整执行链

现在将前面所有内容串起来。

这一节描述 RPMI TEE_CALL + SBI MPXY 方案的目标执行路径。我们只对已核实的 OpenSBI 通用代码给出确定结论，不把 RFC 中的每个细节描述成已经完成 upstream 合并。

## 6.8.1 一次正常的 TEE 调用

```
Linux User Space
     |
     | TEEC_InvokeCommand()
     v
libteec
     |
     | ioctl()
     v
Linux OP-TEE Driver
     |
     | Prepare OP-TEE Message
     |
     | RPMI TEE_CALL
     v
SBI MPXY Transport
     |
     | ECALL
     v
OpenSBI M-mode
     |
     | Validate request
     | Identify target TEE Domain
     |
     v
Domain Context Manager
     |
     +-- Save Linux Context
     |
     +-- Restore OP-TEE Context
     |
     +-- Configure Isolation
     |
     v
MRET
     |
     v
OP-TEE S-mode
     |
     | Process request
     |
     v
Trusted Thread / TA
```

这项 Linux 传输方案的公开 RFC 描述了使用 RPMI TEE Service 承载原有 OP-TEE ABI，并通过 SBI MPXY 进入 M-mode、在当前 Hart 上切换 TEE Domain 的设计。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

Openwall



## 6.8.2 为什么不是 Linux 直接调用 `sbi_domain_context_enter()`？

因为这个函数是 OpenSBI 内部的 C 接口。

Linux S-mode 无法直接执行 M-mode Firmware 中的普通 C 函数。

需要先通过一个可调用的、经过授权的 SBI Service 进入 OpenSBI，再由对应的 M-mode Service Handler 判断是否允许切换。

这里存在一个十分重要的安全条件：

不能允许 Linux 仅仅通过提供任意 Domain ID，就要求 M-mode 跳转到某个安全 Domain。

OpenSBI 必须确认：

- 调用者属于允许发起该服务的 Domain。
- 目标 Domain 是该服务允许访问的对象。
- 请求符合预期的 ABI 和协议状态。
- Shared Memory 不会导致非法资源访问。
- 进入目标 Domain 的行为符合当前生命周期与 Hart 状态。

否则 Domain Manager 会成为攻击者绕过隔离的入口。

## 6.8.3 `sbi_domain_context_enter()` 的行为

实际入口函数位于：

`lib/sbi/sbi_domain_context.c`

它的核心检查可以归纳为：

```
sbi_domain_context_enter(target)
    |
    | target != NULL ?
    |
    | target != current_domain ?
    |
    | current hart in target.possible_harts ?
    |
    | Current context allocated ?
    |
    | Target context exists ?
    |
    | Set target.prev_ctx = current_ctx
    |
    v
switch_to_next_domain_context()
```

这里的 Hart 检查非常重要。

即使某个 Domain 存在，也不意味着所有 Hart 都允许进入。

例如：

```
Linux Domain:
    possible_harts = {0,1,2,3}

OP-TEE Domain:
    possible_harts = {0,1}
```

如果 Hart 3 请求切换到 OP-TEE，那么通用 Context Manager 会拒绝该请求。

这种检查是资源和执行能力边界的一部分，而不只是防止空指针错误。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 6.8.4 最终 CPU 究竟发生了什么变化？

我们用一个假想的执行现场：

| 状态         | Linux              | OP-TEE              |
| ---------- | ------------------ | ------------------- |
| Privilege  | S-mode             | S-mode              |
| PC         | `0x80201000`       | `0x90002000`        |
| Stack      | Linux Kernel Stack | OP-TEE Stack        |
| `satp`     | Linux Root PT      | OP-TEE Root PT      |
| `stvec`    | Linux Trap Handler | OP-TEE Trap Handler |
| PMP Policy | Linux Domain       | OP-TEE Domain       |
| GPR        | Linux Register Set | OP-TEE Register Set |

当 `mret` 完成后，CPU 恢复的是右侧的执行状态。

非常关键的是：

CPU 不需要增加一个名叫 Secure S-mode 的特殊 Privilege。

二者都可以处于 S-mode，安全边界由 M-mode Firmware 管理的硬件隔离策略建立。

不过，这并不意味着所有能够运行两个 S-mode Domain 的平台都自动满足 TEE 安全要求。DMA、MMIO、Interrupt Routing、Context Leakage 等问题仍然必须被完整解决。

# 6.9 OP-TEE 返回 Linux Domain 的完整路径

我们接下来分析反向过程。

## 6.9.1 OP-TEE 并不总是在 TA 完成时才返回 Linux

与 Arm64 OP-TEE 类似，常见返回类型可能包括：

1. Request Completed：TA 请求处理结束。
2. RPC Request：需要 Linux 或 `tee-supplicant` 协助。
3. Yield / Interrupt：因为特定事件需要让出执行控制权。
4. Error / Abort：执行过程中出现需要向调用者报告的异常。

这些场景在更高层的 OP-TEE ABI 中有不同语义，而底层 Domain Switch 只负责切换执行上下文。

不能把「请求已完成」与「离开 OP-TEE Domain」视为同一个状态。

## 6.9.2 从 OP-TEE 返回 OpenSBI

概念执行链：

```
OP-TEE S-mode
     |
     | Prepare return / RPC / Yield
     |
     | RPMI transport or relevant firmware ABI
     v
ECALL
     |
     v
OpenSBI M-mode
     |
     | Process return status
     |
     v
sbi_domain_context_exit()
     |
     | Find previous caller context
     |
     v
switch_to_next_domain_context()
```

在此前 Linux 进入 OP-TEE 时，OpenSBI 已经记录：

```
OPTEE_CTX.prev_ctx = LINUX_CTX
```

因此 Exit 可以找到需要恢复的 Linux Context。

但还需要强调：

实际的 OP-TEE Transport Handler 必须正确提供返回消息、错误码和协议状态。单独调用 `sbi_domain_context_exit()` 只能表达上下文离开，不等于完成全部 RPMI 响应处理。

## 6.9.3 为什么 Linux 可以继续从原来位置运行？

因为之前保存的 Linux Trap Context 中包含：

```
Linux Saved Context
    |
    +-- GPR
    +-- mepc / mstatus
    +-- sstatus / stvec / sscratch
    +-- sepc / scause / stval
    +-- satp
    +-- FP / Vector
```

现在重新恢复这些状态，再执行 `mret`。

对于一个普通 SBI ECALL，OpenSBI 的 `sbi_ecall_handler()` 通常会将返回 PC 前移 4 字节，并填写 ABI 规定的返回寄存器；该通用逻辑还提供 `skip_regs_update`，允许具有特殊语义的处理路径跳过默认寄存器更新。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这个细节在 Domain Switch 设计中值得特别注意。

假设 Linux 的 `ecall` 位于：

```
0x80201000: ecall
0x80201004: next instruction
```

那么普通完成返回时，Linux 应该从下一条指令继续执行，而不是无限重复 `ecall`。

但是如果某个特殊 Domain Switch Handler 在 `sbi_ecall_handler()` 返回前已经替换了活动 Trap Frame，就必须明确规定：

- 哪个 Frame 的 `mepc` 需要前移？
- 哪个 Frame 的 `a0/a1` 需要更新？
- `skip_regs_update` 应不应该设置？
- 被暂停的调用者的返回值存放在哪里？
- 恢复目标 Domain 时，是否可能意外修改其 Resume PC？

这些都是 ABI 实现的正确性要求。

## 6.9.4 RPC 为什么需要额外的状态管理？

考虑：

```
Linux
  |
  | Invoke TA
  v
OP-TEE
  |
  | Need REE filesystem
  v
Linux / tee-supplicant
  |
  | RPC completed
  v
OP-TEE
  |
  | Resume TA
  v
Linux
```

这里会发生多次 Domain 切换。

但 TA 的 Trusted Thread 不应该因为暂时返回 Linux 就被销毁。

所以需要同时保存两层不同 Context：

OpenSBI Domain Context

负责让 CPU 在 Linux 和 OP-TEE 之间切换。

OP-TEE Trusted Thread Context

负责让 OP-TEE 下次被调用时，能够恢复此前被挂起的 TEE Thread。

两个机制相互配合，但绝不能混为一谈。

特别是在多个 Linux Thread 并发调用 OP-TEE 时，M-mode 的 Hart Context 与 OP-TEE 内部的 Thread ID、RPC 状态和 Session Context 还需要正确对应。

这里会成为 Chapter 10 和 Chapter 15 的交叉重点。

# 6.10 Domain Switch 的安全不变量与失败恢复

这一节从「代码如何实现」转向「怎样证明这套机制在工业产品中足够可靠」。

这是从 Senior Engineer 走向 Staff Engineer 必须跨越的一步。

能够解释正常情况下的 Domain Switch，只能证明理解了正常执行路径。

真正具备架构设计能力，还必须回答：

> 如果在 Domain Switch 的每一个阶段发生异常、资源分配失败、异步中断或者硬件配置错误，整个系统是否仍然安全？

## 6.10.1 六条核心安全不变量

我建议把 Domain Context Switch 的正确性建立在以下六条不变量上。

| 不变量                        | 要求                                            |
| -------------------------- | --------------------------------------------- |
| I1 — Context Isolation     | 不同 Domain 不得通过遗留 CPU 状态观察对方的私有数据              |
| I2 — Memory Isolation      | 返回目标 Domain 前，目标硬件保护策略必须已经有效                  |
| I3 — Execution Integrity   | PC、SP、Trap Context 必须属于预期目标，且具备合法的执行状态        |
| I4 — Ownership Consistency | Hart Ownership、Context 指针和实际运行的 Domain 必须保持一致 |
| I5 — Return Integrity      | Domain Exit 只能返回经过授权的调用者或有效的启动目标              |
| I6 — Failure Safety        | 切换失败不得让 CPU 在错误权限下继续执行不可信代码                   |

其中最值得强调的是：

\\[ \text{Security} \neq \text{Functional Correctness} \\]

一个 Domain Switch 实现可以在正常情况下连续成功执行一百万次，但这并不证明它没有安全漏洞。

因为某些安全问题只发生在极少见的错误或并发路径中。

## 6.10.2 把 Domain Switch 视为一个 Transaction

从设计理念上，我建议将 Domain Switch 视为安全事务，而不只是普通函数调用。

它可以分成四个阶段：

1. Prepare

   验证目标 Domain、Hart 权限、Context 有效性、硬件能力与必要资源。
2. Save and Configure

   保存当前上下文，准备目标上下文，更新硬件保护和地址转换相关状态。
3. Commit

   验证目标安全状态完整建立，使软件记录的 Domain Ownership 与硬件状态一致。
4. Resume

   从可信 M-mode 返回目标 Domain，并恢复其执行。

注意，实际芯片的硬件配置未必支持真正的原子事务或完整回滚。这里采用 Transaction 是为了定义设计和验证原则。

如果某个阶段失败：

- 能安全回滚时，恢复原 Domain 的硬件权限和 CPU Context。
- 无法证明可以安全回滚时，采用 Fail-Closed 策略。
- 对于不可恢复的隔离故障，可以选择 Halt Hart、进入安全错误处理或触发平台规定的恢复机制。
- 不允许在权限状态未知的情况下直接恢复不可信 S-mode 执行。

## 6.10.3 真实工业问题一：硬件隔离配置失败

假设使用 WorldGuard 作为系统级隔离机制。

在 Linux → OP-TEE 的切换过程中：

```
Step 1: Save Linux CPU Context       OK
Step 2: Restore OP-TEE CPU Context   OK
Step 3: Update PMP                  OK
Step 4: Update WorldGuard WID       FAILED
Step 5: Return OP-TEE               ?
```

第 5 步应该怎么处理？

不能只依据 PMP 成功就直接恢复 OP-TEE。

因为实际安全策略可能要求 CPU 访问权限和系统级 WorldGuard 资源权限共同生效。

尤其是 DMA、MMIO 等资源，可能完全不由 CPU PMP 保护。

RISE RP016 Milestone 5 已针对 WorldGuard 实现 Domain Context Switch 期间的硬件隔离状态更新，并通过 QEMU WorldGuard Model 进行测试，这是一份很好的工程参考。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



这里应该设计相应的 Fault Injection，检查：

- WorldGuard 配置失败后的有效硬件状态。
- Hart Ownership 是否与实际状态一致。
- 当前是否已经替换 Trap Frame。
- 是否允许安全恢复之前的 Domain。
- 是否有日志与错误状态帮助诊断，同时不泄露敏感内容。

## 6.10.4 真实工业问题二：Domain Switch 时发生中断

假设：

```
Hart 0
    |
    | Linux -> OP-TEE
    |
    | Save Context
    |
    | Update PMP
    |
    +-- External Interrupt Arrives
```

这个中断应该交给谁？

问题不只是 CPU 能否响应中断，还涉及：

- 中断属于哪个 Domain？
- 当前是否处于安全切换的临界阶段？
- 物理 Interrupt Controller 怎样路由？
- 是否需要排队并延迟交付？
- 如果目标 Domain 不在当前 Hart 上运行，是否需要 Context Switch？
- 中断 Pending State 如何保持正确？

进入 M-mode Trap 后，普通 M-mode 中断使能通常会被关闭，但这不意味着所有类型的异步事件都天然不会影响安全切换。

例如 NMI、平台级异常以及其他 Hart 或 DMA 的并发活动，仍然需要独立分析。

RP016 的 VIRQ 子系统已经引入 Domain-aware Interrupt Routing 和按 Domain/Hart 维护的 Pending Queue，并设计了 Interrupt-driven Domain Context Switching。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



这部分将在 Chapter 16 深入展开。

## 6.10.5 真实工业问题三：Domain Switch 产生敏感数据泄露

假设：

```
OP-TEE executes Vector Crypto
    |
    | Sensitive data in Vector Registers
    |
    | Yield to Linux
    v
Linux resumes
```

如果 Vector Context 没有正确保存、恢复或清理，Linux 可能观察到敏感数据残留。

需要检查的对象不仅有 Vector Registers：

- GPR
- FP Registers
- Vector Registers
- Trap Scratch
- Stack 中的敏感暂存数据
- Debug / Trigger State
- 可被不可信软件访问的 Shared Memory
- 可观察的 Microarchitectural State

最后一项还涉及更复杂的 Cache Side Channel、Branch Predictor 等问题。

不过要明确区分：完整保存 CPU 架构寄存器是 Domain Switch 的基本正确性要求，并不自动构成对所有微架构侧信道攻击的防护保证。

# 6.11 从 OpenSBI 源码验证本章理论

这部分是我建议你亲自完成的实验。

既然你的目标是最终成为 OP-TEE 社区的 Maintainer，那么不能仅仅阅读文章，应该亲自跟踪和验证代码。

## 实验 A：梳理 Domain Context 的真实数据流

首先在 WSL2 中获取 OpenSBI：

```
git clone https://github.com/riscv-software-src/opensbi.git
cd opensbi

git rev-parse HEAD
```

记录 Commit Hash，保证后续实验具有可复现性。

使用以下命令定位关键函数：

```
rg -n \
  'sbi_domain_context_enter|sbi_domain_context_exit|switch_to_next_domain_context' \
  lib/ include/

rg -n \
  'sbi_hart_protection_reconfigure|sbi_hart_protection_configure' \
  lib/ include/

rg -n \
  '_trap_handler:|TRAP_SAVE_AND_SETUP_SP_T0|TRAP_RESTORE_MEPC_MSTATUS' \
  firmware/fw_base.S
```

重点观察三个数据流：

```
Data Flow 1:
Hart ID
   -> Hart Index
   -> Per-Domain Context Pointer

Data Flow 2:
Current Trap Frame
   -> Save to Current Domain
   -> Load Target Domain Trap Frame

Data Flow 3:
Current Domain Policy
   -> Protection Reconfigure
   -> Target Domain Policy
```

需要绘制一张能够精确表示这三个数据流的架构图。

## 实验 B：分析 `sbi_domain_context_enter()`

建议沿下面的顺序单步跟踪：

```
sbi_domain_context_enter()
    |
    +-- Validate target domain
    |
    +-- Validate current hart
    |
    +-- hart_context_init()
    |
    +-- hart_context_get()
    |
    +-- Set prev_ctx
    |
    +-- switch_to_next_domain_context()
```

每一步记录：

| 状态                 | Before                      | After                        |
| ------------------ | --------------------------- | ---------------------------- |
| Current Domain     | Linux                       | OP-TEE                       |
| `assigned_harts`   | Linux contains current Hart | OP-TEE contains current Hart |
| `satp`             | Linux Page Table            | OP-TEE Page Table            |
| `stvec`            | Linux Trap Handler          | OP-TEE Trap Handler          |
| Trap Frame         | Linux                       | OP-TEE                       |
| Hardware Isolation | Linux Policy                | OP-TEE Policy                |

重点验证一个问题：

当前 CPU 的 GPR 到底在哪一步完成了真正的恢复？

答案不应该仅仅是 `switch_to_next_domain_context()`。

你还需要继续单步跟踪返回路径，直到 `firmware/fw_base.S` 的 Restore Macro 和最终 `mret`。

## 实验 C：首次进入与再次进入目标 Domain

分别观察：

```
Case 1:
    target.initialized = false
```

和：

```
Case 2:
    target.initialized = true
```

对比：

- `sbi_hart_switch_mode()` 是否被调用。
- 哪个 PC 最终被恢复。
- 哪个 Trap Frame 被使用。
- Hart 是否进入 HSM Stop。
- `prev_ctx` 在进入和退出后如何变化。

这是理解 Domain Boot 和 Domain Resume 的关键。

## 实验 D：故障注入

建议设计三个测试：

| 测试                 | 注入条件                         | 预期                 |
| ------------------ | ---------------------------- | ------------------ |
| Invalid Target     | 不合法的目标 Domain                | 在任何危险切换前拒绝         |
| Unauthorized Hart  | 当前 Hart 不属于 `possible_harts` | 返回错误，当前执行环境保持可用    |
| Protection Failure | 模拟硬件保护配置失败                   | 不允许不可信代码在错误权限下继续运行 |

第三项需要修改测试用的 OpenSBI Backend，不能在生产固件中随意注入故障。

特别推荐检查 `sbi_hart_protection_reconfigure()` 失败后，上层是否能够感知并正确处理错误。

如果你能建立清晰的失败复现场景、补充回归测试并形成合理的修复方案，它就可能成为有价值的 upstream 贡献。

## 实验 E：性能分析

在保证隔离正确后，再测量 Domain Switch 的性能。

可以将总延迟分解为：

\\[ T\_{switch} = T\_{trap}+ T\_{save}+ T\_{csr}+ T\_{fpvec}+ T\_{isolation}+ T\_{sync}+ T\_{restore} \\]

这里是工程上的分项模型，具体实现中有些操作会重叠或相互影响。

建议进行下列对比：

- FP/Vector 扩展开启或关闭。
- 不同 VLEN 配置。
- PMP 与额外系统级隔离机制。
- 不同 ASID 管理策略。
- 单 Hart 与多 Hart 并发请求。
- Cold Entry 与 Warm Resume。

测量时不要只给出平均延迟。

还应该观察 P50、P95、P99、Worst Observed Latency，并分析最坏时延是否具有清楚的上界。

对 TEE 而言，Domain Switch 的性能会直接影响高频 TA 调用、密码服务和安全设备访问。

# 6.12 Staff Engineer 级别测试题

下面的题目不是普通的知识点考察。

每一道题都要求你结合实际 RISC-V Privileged Architecture、OpenSBI 内部实现和工业场景进行分析。

建议先独立设计答案，再查看源码验证。

## Part A — Architecture & Context Switching

### Q1. 两个 S-mode Domain 为什么能够实现强隔离？

难度：★★★★☆ · Security Architecture

假设一款四核 RISC-V SoC：

```
Domain A = Linux
Domain B = OP-TEE
Both run in S-mode

Protection = PMP + Sv39
```

攻击者已经取得 Linux root 权限，可以任意修改 Linux Kernel、页表和 S-mode CSR。

请回答：

1. 为什么 Linux 不能通过修改 `satp` 读取 OP-TEE RAM？
2. 如果攻击者能控制一个 DMA Master，PMP 还足够吗？
3. OpenSBI 的 TCB 包含哪些组件？
4. 如果 OpenSBI 的 Domain ID 可以由 Linux 任意指定，会产生什么安全问题？
5. 你如何证明这套架构满足 TEE 的隔离要求？

考察重点： Security State、PMP、DMA、TCB、Threat Model。

### Q2. 解释 `mret` 为什么能够切换 Domain

难度：★★★★☆ · Assembly / Trap

给定：

```
Linux:
    mepc = 0x80201004
    satp = Linux Root PT

OP-TEE:
    mepc = 0x90002000
    satp = OP-TEE Root PT
```

请从 `ecall` 开始，解释 OpenSBI 如何使用 Trap Frame 替换实现 Domain Switch。

要求：

- 解释 `mstatus.MPP`。
- 说明 Trap Frame 在哪里创建。
- 说明 `mepc` 和 `sepc` 的区别。
- 解释 GPR 如何恢复。
- 解释为什么 `mret` 自己并不知道 Domain。

### Q3. Domain Context 为什么必须是 Per-Hart 的？

难度：★★★★☆ · SMP Architecture

假设有四个 Hart，OP-TEE Domain 允许同时在四个 Hart 上执行。

一名工程师为了降低内存开销，提出：

「每个 Domain 只保存一个全局 `hart_context`，不再为每个 Hart 分配 Context。」

请分析这个方案。

进一步回答：

- 是否可以共享只读 Context？
- 哪些数据一定是 Per-Hart？
- 哪些数据可以是 Per-Domain Global？
- Hart Migration 和 Hart-local TLB/ASID 会产生什么问题？

### Q4. 分析 `prev_ctx` 的安全设计

难度：★★★★★ · Reentrancy / State Machine

假设出现以下调用链：

```
Linux Domain
    |
    v
OP-TEE Domain
    |
    v
Service Domain C
    |
    v
OP-TEE Domain
    |
    v
Linux Domain
```

请设计一个可靠的 Domain Context 返回机制。

要求讨论：

- Nested Enter
- Recursive Enter
- Context Stack
- Caller Authorization
- Return Integrity
- Context Lifecycle
- Stack Overflow / Depth Limit

并说明现有 `prev_ctx` 设计在允许复杂重入时需要哪些额外约束。

## Part B — Memory Protection & Security

### Q5. `satp` 已经切换，为什么仍然发生错误内存访问？

难度：★★★★★ · MMU / TLB

真实工业场景：

一款 RV64 SoC 上运行 Linux 和 OP-TEE。两者都使用 Sv39，ASID 始终为 0。

当 Linux 高频调用 OP-TEE 时，系统偶发：

```
Instruction Access Fault
Load Access Fault
Unexpected memory content
```

问题只发生在部分 CPU 上，QEMU 不容易稳定复现。

你会从哪些方向排查？

要求分析：

- `satp` Write
- `SFENCE.VMA`
- PMP Synchronization
- ASID Reuse
- Speculative Address Translation
- Remote TLB Shootdown

并说明如何建立可稳定复现的实验。

### Q6. PMP Reconfiguration 失败之后如何恢复？

难度：★★★★★ · Fault Recovery / Security

假设 OpenSBI 已经执行：

```
Save Linux Context      SUCCESS

Restore OP-TEE CSR      SUCCESS

Restore FP/Vector       SUCCESS

Reconfigure PMP         FAILED
```

此时：

- Linux Context 已保存。
- OP-TEE Context 已被准备。
- Hardware Protection State 不确定。

请设计一套 Fail-Closed Recovery 方案。

要求同时考虑：

1. 能否 Rollback。
2. 哪些状态需要恢复。
3. 如何避免陷入递归 Fault。
4. 是否需要直接 Halt Hart。
5. 哪些错误应该触发 SoC Reset。
6. 如何通过测试证明没有越权访问窗口。

### Q7. 为什么修改 PMP 后仍然需要 `SFENCE.VMA`？

难度：★★★★★ · ISA Memory Semantics

一位工程师认为：

「PMP 属于物理地址保护机制，TLB 属于虚拟地址转换机制，两者互不相关。因此 PMP 更新后不需要执行 `SFENCE.VMA`。」

请用 RISC-V Privileged Architecture 的地址转换和权限检查机制反驳或修正这一观点。

进一步说明：

- PMP 对隐式页表读取是否生效？
- 硬件能否缓存与 PMP 有关的权限判断？
- 没有分页支持的 Hart 是否有相同要求？
- H Extension 开启后需要考虑哪些额外 Fence？

### Q8. OP-TEE RAM 被 DMA 读取，根本原因是什么？

难度：★★★★★ · SoC Security

某芯片团队已经正确配置 PMP。

Linux Kernel 无法访问 OP-TEE RAM，但攻击者通过控制一个 DMA Controller 成功读取了 OP-TEE 中的敏感数据。

请设计系统级修复方案。

要求：

- 解释 CPU PMP 的适用范围。
- 分析 DMA 与物理内存总线的关系。
- 比较 IOPMP、WorldGuard、SoC Firewall。
- 分析不同 Domain 共用 DMA Engine 时的资源归属。
- 设计安全验证矩阵。

## Part C — FP/Vector & Interrupt

### Q9. Vector Context 泄露问题

难度：★★★★★ · Data Leakage / Vector ISA

一款 RV64 SoC 支持 VLEN=512。

OP-TEE 使用 RVV 优化密码算法。

测试发现，在某些 Domain Switch 之后，Linux 可以观察到之前 OP-TEE 使用过的 Vector Register 数据。

请设计问题定位流程。

要求覆盖：

- `v0`～`v31`
- `vl`、`vtype`、`vstart`
- `sstatus.VS`
- Eager / Lazy Context Switching
- Nested Trap
- First Entry Initialization
- Sensitive Data Clearing

并评估修复的性能影响。

### Q10. Interrupt Arrives During Domain Switch

难度：★★★★★ · Interrupt / Concurrency

假设 Domain Switch 执行到：

```
Save Linux Context
      |
      v
Update PMP
      |
      v
Interrupt Arrives
      |
      v
?
```

请分析：

1. 如果是 M-mode Interrupt，应该如何处理？
2. 如果是 Delegated S-mode Interrupt，何时能够交付？
3. 如果 Interrupt 属于 Linux Domain，但 Hart 正在运行 OP-TEE，应该怎么处理？
4. 如果 Interrupt 属于 OP-TEE Domain，但 OP-TEE 尚未初始化完成，怎么办？
5. 如何避免重复交付和 Lost Interrupt？
6. 中断是否可能引起 Nested Domain Switch？

### Q11. FP/Vector Eager Context Switching 是否值得优化？

难度：★★★★☆ · Performance Architecture

一款高频调用 OP-TEE 的 IoT SoC 出现明显的 TEE Call Latency Regression。

Profiling 发现 FP/Vector Context Saving 占据很大的时间。

工程师提出使用 Lazy FP/Vector Context Switching。

请分析：

- 为什么 Lazy 可能改善性能？
- 哪些 CPU 状态需要配合？
- 如何区分当前状态属于哪个 Domain？
- 首次访问 FP/Vector 发生 Trap 时如何恢复？
- 遇到 Interrupt、Nested Trap 时如何保持安全？
- 如何证明新实现不会泄露寄存器？

要求提供性能测试方案和安全验证方案。

## Part D — Engineering & Upstream Design

### Q12. 审查 OpenSBI `switch_to_next_domain_context()`

难度：★★★★★ · Open-Source Code Review

请直接阅读：

[OpenSBI sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)

针对以下方面给出完整 Code Review：

1. Hart Ownership 更新是否有正确的同步。
2. Context Allocation 是否可能失败。
3. Context Initialization 是否足够安全。
4. CSR Swap 是否存在隐含假设。
5. FP/Vector State 是否覆盖所有相关状态。
6. Hardware Protection Failure 是否被正确处理。
7. 首次 Boot 与 Resume 是否具有明确状态区分。
8. 当前实现还可以增加哪些测试。

要求最后形成一份适合 Upstream Review 的技术评审意见，而不是只给出「代码基本没问题」的结论。

### Q13. 设计一个工业级 Domain Context Manager

难度：★★★★★ · System Design

设计一款面向汽车或工业控制 SoC 的 Domain Context Manager。

硬件条件：

```
8 x RV64 Harts

M-mode OpenSBI

Linux Domain

OP-TEE Domain

Safety Domain

PMP + WorldGuard

APLIC + IMSIC

DMA / Crypto Engine
```

要求：

- 支持多个隔离 Domain。
- 支持 Per-Hart Context。
- 支持 FP/Vector。
- 支持 Interrupt-aware Switching。
- 支持错误恢复。
- 支持资源授权与撤销。
- 支持多核并发。
- 满足可验证的安全与实时性要求。

请给出 Architecture、Data Structure、State Machine、Locking Strategy、Failure Model 和 Test Plan。

### Q14. Domain Switch 性能出现 P99 长尾

难度：★★★★☆ · Performance / SMP

某设备的 Domain Switch 测量结果：

| 指标  | 延迟     |
| --- | ------ |
| P50 | 12 μs  |
| P90 | 18 μs  |
| P99 | 190 μs |
| Max | 500 μs |

这些数字是假设的测试数据。

请设计 Profiling 和 Root Cause Analysis。

必须区分：

- Trap Entry/Exit Latency
- FP/Vector Save
- PMP/Firewall Reconfiguration
- TLB/Fence
- Lock Contention
- Interrupt Interference
- Cache Effects
- Cross-Hart Coordination

最后给出优化优先级，并说明如何避免优化破坏隔离安全性。

### Q15. 从 QEMU Port 到真实 SoC 后出现偶发死机

难度：★★★★★ · Platform Bring-up

一款 RISC-V OP-TEE Port 在 QEMU 上通过所有基础测试。

但在 Palladium 或真实 FPGA/ASIC 平台上，运行 xtest Stress Test 时出现：

```
Hart 2: unexpected trap

mcause = Load Access Fault
mepc   = OpenSBI Context Switch Path
```

故障仅在 SMP 开启时出现，且不易重现。

请给出系统性的定位计划。

至少分析：

- Per-Hart Stack / Scratch
- Domain Context Allocation
- Hart ID 与 Hart Index
- Memory Protection Policy
- PMP Entry 配置
- Cache Coherency
- Memory Barrier
- Concurrent Context Switch
- TLB Shootdown
- Hardware Errata

要求给出至少五种可独立验证的故障假设，并为每种假设设计能支持或排除它的证据。

### Q16. 如何把 Domain Switch 的设计变成一个可被维护的 Upstream Subsystem？

难度：★★★★★ · Staff / Maintainer

你加入一个开发团队，负责将已有的 RISC-V OP-TEE Domain Switching 从特定 SoC 私有实现推广为 OpenSBI 上游可复用框架。

你会怎样组织开发工作？

要求覆盖：

- 安全模型和硬件依赖边界。
- Generic Abstraction 与 Platform Backend。
- ABI Stability 和 Compatibility。
- Patch Series 的拆分。
- Unit Test / Integration Test。
- QEMU Reference Platform。
- 性能回归。
- 代码审查及长期维护责任。

重点不在于你能写多少代码，而在于你能否把系统需求转化为可审查、可验证、可长期维护的架构。

## 6.13 Staff Engineer 题目的评价标准

针对上述 16 道题，我建议不要按照能否背出定义打分，而是使用如下标准。

架构理解

20 分

能解释组件分层、信任边界和设计取舍

ISA 与源码深度

25 分

能够定位实际 CSR、汇编入口和关键数据结构

正确性与安全性

25 分

主动考虑异常路径、并发、状态残留和失效行为

工程验证

20 分

能够设计稳定复现、测试矩阵和性能测量

技术决策

10 分

给出合理的架构选择、优先级和维护策略

我的建议是：

- 60～74 分：熟悉架构，有一定的源码开发能力。
- 75～89 分：能够独立负责相关子系统开发、调试和维护。
- 90～100 分：能够提出完整的架构设计，系统性论证安全性和正确性，具备较强的 Staff-level 技术判断能力。

这是一套学习用的参考标准，而不是任何公司的官方面试评分体系。

# 6.14 本章总结：真正应该建立的架构认知

本章最重要的并不是记住 OpenSBI 的某一个函数名。

而是理解以下执行模型：

```
                   Linux S-mode
                        |
                        | ECALL
                        v
               OpenSBI M-mode Trap
                        |
                        | Save Trap Frame
                        v
                Domain Context Manager
                        |
             +----------+----------+
             |                     |
             v                     v
       CPU Context            Security State
             |                     |
       GPR / CSR               PMP / Firewall
       FP / Vector             TLB / Fence
       Trap State              Hart Ownership
             |                     |
             +----------+----------+
                        |
                        | Restore Target
                        | MRET
                        v
                   OP-TEE S-mode
                        |
                        | TEE Work
                        |
                        | Exit / RPC / Yield
                        v
               OpenSBI M-mode Trap
                        |
                        | Restore Linux
                        v
                   Linux S-mode
```

你应该从这一章得出五个核心结论：

第一，Domain Switch 不是 CPU 自动提供的功能。 RISC-V 的 `ecall`/`mret` 提供异常入口和返回机制，而 OpenSBI 需要在软件层建立安全 Domain Context Switching。

第二，CPU Context 和 Security Context 是两种不同的状态。 保存 GPR/CSR 只解决执行连续性，PMP 和系统级隔离机制才决定低特权代码可以访问哪些物理资源。

第三，Domain Context 是 Per-Hart 的。 同一个 Domain 在不同 Hart 上具有独立的执行现场，而 Domain 的策略和资源描述可以包含全局数据。

第四，Domain Switch 必须理解完整的内存系统。 `satp`、ASID、PMP、`SFENCE.VMA` 和 SMP TLB Shootdown 各自具有不同职责，不能简单依靠一次 CSR Write 认为地址转换和权限已经同步。

第五，安全的 Domain Switch 必须包含正确的失败处理。 在工业产品中，任何没有检查的硬件权限配置错误，都值得作为潜在的安全设计风险进行审查。

## 本章建议达到的最终能力

完成 Chapter 06 后，我希望你不仅能够阅读 `sbi_domain_context.c`，还能够在纸上独立画出：

一次 Linux → OP-TEE → Linux 的完整切换，涵盖 M-mode Trap Frame、Per-Domain CSR Context、FP/Vector Context、PMP 配置、TLB 同步以及异常返回。

如果你还能进一步对其中的状态一致性、错误路径和中断并发提出合理的测试与修复设计，那么你对这个专题的掌握就已经超越了普通 Porting Engineer 的范畴，开始具备维护 OpenSBI / RISC-V OP-TEE 核心架构代码所需要的判断能力。

本章建议优先复查的源码：

- [fw_base.S — M-mode Trap Entry/Exit Assembly](https://github.com/riscv-software-src/opensbi/blob/master/firmware/fw_base.S?utm_source=chatgpt.com)
- [sbi_domain_context.c — Domain Context Manager](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)
- [sbi_hart_protection.c — Hardware Protection Framework](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart_protection.c?utm_source=chatgpt.com)
- [RISC-V Privileged ISA — Supervisor-Level Architecture](https://docs.riscv.org/reference/isa/priv/supervisor.html?utm_source=chatgpt.com)

我尤其建议你优先完成 Q2、Q5、Q6、Q12 和 Q13。这五道题组合起来，基本覆盖了 Domain Context Switching 最核心的汇编机制、内存一致性、安全失败路径、源码审查和系统架构设计能力。
