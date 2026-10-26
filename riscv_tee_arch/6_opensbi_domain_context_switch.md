# Chapter 06 — OpenSBI Domain Context Switching

RISC-V OP-TEE Architecture & Porting · 核心架构深度课程

本章主题：从 RISC-V Trap Entry 到 OpenSBI Domain Switch，深入理解 Linux 与 OP-TEE 如何在同一个 hart 上安全地交替运行。

本章是整套 RISC-V OP-TEE 教程中最重要的章节之一。我们会从设计原理开始，深入到 OpenSBI 实际源码、RV64 CSR、Context 数据结构、硬件隔离、MMU/TLB 和异常处理。

## 一、首先建立本章的核心认知模型

先给出一个贯穿全章的结论：

OpenSBI Domain Context Switch 不是普通的线程上下文切换，而是一个由 M-mode Firmware 管理的、涉及执行状态与硬件安全边界的 Domain 切换。

它需要同时完成三件事情：

1. Execution Context Switch：让一个 hart 暂停执行 Linux Domain，转而执行 OP-TEE Domain。
2. Protection Context Switch：更换这个 hart 能够访问的硬件资源和内存权限。
3. State Isolation：保证两个 Domain 的寄存器、地址空间及其他敏感状态不会发生非授权共享。

这三件事必须共同成立。只保存寄存器并执行 `mret`，还不足以实现一个安全的 TEE。

### 1.1 通过一个具体例子理解

假设一个 RV64 SoC 具有 4 个 hart，运行 Linux 和 OP-TEE。

Hart 0 的执行时间线

简化的同步调用模型。实际 Domain Switch 经由 M-mode Trap Handler 完成，且还可能由外部中断等机制触发。

在 `t0`，Hart 0 执行 Linux。

在 `t1`，Linux 发起 TEE 请求，由 OpenSBI 接管切换。

在 `t2`，OP-TEE 处理完请求，Hart 0 返回 Linux。

在 `t3`，Linux 再次请求 OP-TEE。

这里有三个容易混淆的概念：

| 概念                            | 切换对象                          | 谁负责             |
| ----------------------------- | ----------------------------- | --------------- |
| Linux Task Context Switch     | Linux Task A ↔ Task B         | Linux Scheduler |
| OP-TEE Trusted Thread Switch  | OP-TEE 的 Trusted Thread 执行上下文 | OP-TEE Core     |
| OpenSBI Domain Context Switch | Linux Domain ↔ OP-TEE Domain  | M-mode OpenSBI  |

本章只重点研究第三种切换，但会解释它与前两种机制如何配合。

### 1.2 为什么它是一个安全架构问题？

假设系统采用以下物理内存布局：

| 物理地址范围                  | 所属区域               |
| ----------------------- | ------------------ |
| `0x80000000–0x801FFFFF` | OpenSBI Firmware   |
| `0x80200000–0x87FFFFFF` | Linux Domain RAM   |
| `0x88000000–0x88FFFFFF` | OP-TEE Secure RAM  |
| `0x89000000–0x890FFFFF` | 经授权的 Shared Memory |

以上为教学用的假设地址布局，不代表某块开发板。

当 Linux 执行时：

- Linux 必须无法直接读写 OP-TEE Secure RAM。
- Linux 不应该能修改 M-mode 安全配置。
- Linux 只能使用被授权的 Shared Memory。
- 其他总线 Master 的访问也需要受相应平台硬件策略约束。

当 OP-TEE 执行时：

- OP-TEE 可以访问自己的 Secure RAM。
- OP-TEE 对 Linux 内存的访问也应该受到明确的授权策略限制。
- OpenSBI 必须保证切换前后的隔离配置符合目标 Domain 的安全策略。

注意一个重要区别：

PMP 对 M-mode 与 S/U-mode 的访问权限规则不同；标准 PMP、Smepmp、WorldGuard 等机制也不完全等价。

因此，不能简单地认为所有 Domain Switch 都通过不断修改 PMP Entry 来实现。在具有更强系统级隔离硬件的 SoC 中，可能采用不同的身份标识、访问权限和硬件配置机制。

### 1.3 本章源码依据与实现边界

为了避免把规划方案当成已经合并的实现，我们将区分三个层次：

| 层次                        | 使用的依据                                                         |
| ------------------------- | ------------------------------------------------------------- |
| RISC-V Architecture       | RISC-V Privileged ISA、`ECALL`、`MRET`、`satp`、PMP、Fence         |
| OpenSBI Upstream          | `sbi_domain_context.c`、`sbi_trap.c`、Hart Protection Framework |
| RISC-V OP-TEE Integration | RISE RP016/RP022 的 Domain、Interrupt、WorldGuard、RPMI 设计        |

本章主要分析当前 OpenSBI upstream `master` 中能够核实的源码实现，并用 RISC-V OP-TEE 作为系统运行场景。对尚在开发和评审中的集成机制，将明确说明其状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+3



## 二、6.1 为什么 Domain Switch 不等于普通 Task Context Switch？

先从最容易误解的地方讲起。

### 2.1 普通 RTOS Context Switch 的工作

以 NuttX 为例，假设有两个任务：

```
Task A                           Task B
Priority 100                     Priority 150
    |                                |
    |  Running                       | Ready
    |                                |
    |-------- Context Switch -------->|
                                     |
                                  Running
```

一次普通任务切换主要需要保存：

- 通用寄存器和 Stack Pointer。
- 程序恢复地址及处理器状态。
- 根据 OS 的实现，保存 FPU、MMU 或其他扩展状态。

其核心目标是：

让 CPU 暂停执行任务 A，继续执行任务 B，并保证两个任务的执行状态互不破坏。

Linux 和 NuttX 的任务切换已经涉及地址空间隔离、用户/内核权限和 FPU 等问题，因此不能将任务切换简单理解为「只保存 GPR」。

但是，它们通常位于同一个 OS 的管理边界内。

### 2.2 Domain Switch 的特殊性

假设 Linux 和 OP-TEE 分别运行在两个隔离 Domain 中。

两者都有自己的：

- S-mode Kernel。
- Trap Vector。
- Stack。
- 页表根。
- 线程和内存管理。
- 中断状态。
- 安全策略。

那么切换的不是两个普通任务，而是两个彼此不信任的 Supervisor 运行环境。

Linux Domain

PC = Linux Instruction

SP = Linux Stack

satp = Linux Page Table

stvec = Linux Trap Handler

Physical Access = Policy A

OP-TEE Domain

PC = OP-TEE Instruction

SP = OP-TEE Stack

satp = OP-TEE Page Table

stvec = OP-TEE Trap Handler

Physical Access = Policy B

OpenSBI M-mode

Context + Address Space + Protection Policy

由此得到一个重要公式：

\\[ \text{Domain Switch} = \text{CPU State Switch} +\text{Address Space Switch} +\text{Protection Policy Switch} \\]

这里的加法表示需要共同满足的职责，而不是实际源码中的三个独立函数。

最关键的区别在于 Protection Policy Switch。

普通 Linux 任务切换通常不会重新设计整个 SoC 的硬件安全资源归属，而在独立 TEE Domain 之间切换时，资源授权就是安全正确性的一部分。

### 2.3 与 Arm TrustZone World Switch 比较

你可以复用 Armv8-A 的知识来理解。

| Arm TrustZone                      | RISC-V OpenSBI Domain      |
| ---------------------------------- | -------------------------- |
| Secure / Non-secure Security State | 不同 OpenSBI Domain          |
| EL3 Secure Monitor                 | M-mode OpenSBI / 可信固件      |
| `SCR_EL3.NS` 等安全状态控制               | PMP/Smepmp/平台安全域配置         |
| `ELR_EL3` + `SPSR_EL3`             | `mepc` + `mstatus.MPP`     |
| `ERET`                             | `MRET`                     |
| Secure/Non-secure EL1 寄存器银行等状态     | 由实现负责保存/恢复的 S-mode Context |
| TrustZone 内存及设备安全属性                | Domain 物理权限和平台隔离策略         |

但这并不是严格的一一对应。

Arm TrustZone 的 Security State 具有体系结构规定的语义；OpenSBI Domain 则是 Firmware 定义并由底层硬件机制强制执行的隔离抽象。

因此 RISC-V 的安全性需要逐项验证，不能因为软件里存在一个 `struct sbi_domain` 就认定两个 Domain 已经实现安全隔离。

## 三、6.2 从 S-mode ECALL 进入 M-mode：实际汇编执行路径

这是理解 Context Switch 的第一段核心代码。

假设 Linux 正运行于 S-mode，并通过一个受支持的 SBI 服务触发后续 Domain 切换。

### 3.1 RISC-V ECALL 的基本行为

典型 SBI 调用遵循这样的参数约定：

| 寄存器      | 内容               |
| -------- | ---------------- |
| `a0–a5`  | 调用参数             |
| `a6`     | SBI Function ID  |
| `a7`     | SBI Extension ID |
| `a0`（返回） | SBI Error Code   |
| `a1`（返回） | SBI Return Value |

S-mode 调用代码可以写成：

```
/* 示意：S-mode 发起一次 SBI 调用 */

li a0, 0              /* argument 0 */
li a6, FUNC_ID        /* function ID */
li a7, EXT_ID         /* extension ID */
ecall

/* SBI 返回后继续执行 */
```

`FUNC_ID` 和 `EXT_ID` 这里是教学占位符，不是公开定义的 Domain Switch SBI Extension 编号。

这点很重要：`sbi_domain_context_enter()` 是 OpenSBI 内部 C 接口，不意味着任意 Linux 应用能够通过一个通用 SBI Extension ID 自由选择目标 Domain。

实际 RISC-V OP-TEE 的请求路由需要通过受支持且具备授权检查的通信机制，例如相应集成版本中的 MPXY/RPMI。

### 3.2 执行 ECALL 后，硬件自动完成什么？

假设调用发生在：

```
Current Privilege = S-mode
PC                = 0x80201234
mstatus.MIE       = 1
```

执行 `ecall` 后，如果该异常未被更高层机制特殊处理，按通常的 M-mode 接收路径：

```
mepc          = 0x80201234
mcause        = 9    (Environment Call from S-mode)
mstatus.MPP   = S
mstatus.MPIE  = previous MIE
mstatus.MIE   = 0
PC            = mtvec
Privilege     = M-mode
```

注意几个容易混淆的细节。

首先，`mepc` 保存的是触发 ECALL 的地址，硬件不会自动加 4。

其次，M-mode 进入 Trap 后，处理器会更新 `mstatus` 的相关字段。

最后，硬件不会自动保存 x1–x31 这些通用寄存器，也不会自动切换到新的软件栈。

这些工作需要 Trap Entry Assembly 自己完成。

### 3.3 OpenSBI 实际汇编入口

在 OpenSBI upstream 中，相关代码位于：

[firmware/fw_base.S](https://github.com/riscv-software-src/opensbi/blob/master/firmware/fw_base.S?utm_source=chatgpt.com)

核心入口名称是 `_trap_handler`。

它的执行结构可以抽象为：

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
    |       |
    |       +-- sbi_trap_handler()
    |
    +-- TRAP_RESTORE_GENERAL_REGS_EXCEPT_A0_T0
    |
    +-- TRAP_RESTORE_MEPC_MSTATUS
    |
    +-- TRAP_RESTORE_A0_T0
    |
    +-- mret
```

这与 upstream 汇编中实际使用的宏和函数名称一致。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 3.4 为什么 OpenSBI 需要 `mscratch`？

这部分非常值得从汇编设计思想理解。

当 S-mode 触发 Trap 时：

```
sp = Linux Kernel Stack
tp = Linux Thread Pointer
```

但是进入 M-mode 后，不能继续使用 Linux 的 Stack 和 Thread Pointer。

原因包括：

- Linux Stack 不属于 OpenSBI 所控制的固件内存。
- OpenSBI 不应该相信低权限 Domain 传入的 Stack Pointer。
- 在 Smepmp 等配置下，M-mode 甚至不一定有权访问任意 S-mode 内存。
- 进入 Trap 的第一批指令必须安全地找到当前 hart 的固件栈。

OpenSBI 使用 `mscratch` 保存与当前 hart 相关的可信 Scratch 信息。

其实际入口汇编包含：

```
csrrw tp, mscratch, tp
```

这条指令实现交换：

```
Before:
    tp       = Linux TP
    mscratch = OpenSBI Scratch Pointer

After:
    tp       = OpenSBI Scratch Pointer
    mscratch = Linux TP
```

接下来，汇编代码可以通过可信 `tp` 找到异常栈，并在完成必要状态保存后交换回原始 `tp`。

对应的 `TRAP_SAVE_AND_SETUP_SP_T0` 还会读取 `mstatus.MPP`，区分 Trap 是否来自较低特权级，从而选择合适的 Exception Stack。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这是一个非常典型的架构设计：

> 在尚未完成通用寄存器保存之前，利用一个受保护的 CSR 建立可信的执行上下文。

你可以把它与 Arm64 的 `SP_EL3`、异常向量入口、Per-CPU Data 访问联系起来理解，但具体硬件机制不同。

### 3.5 从汇编进入 C Handler

汇编执行：

```
call sbi_trap_handler
```

随后进入：

`lib/sbi/sbi_trap.c`

在对应的 S-mode ECALL 处理中，OpenSBI 会调用：

```
sbi_trap_handler()
    |
    +-- sbi_ecall_handler()
            |
            +-- Find SBI Extension
            |
            +-- Invoke Extension Handler
```

这也是实际 upstream 中存在的调用关系。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



如果某个经过授权的 SBI 服务需要转入另一个 Domain，其 Handler 可以进一步调用 OpenSBI 的 Domain Context 接口。

有一个细节必须理解：

普通 SBI ECALL 在返回时，OpenSBI 需要更新保存的 `mepc`，跳过已经执行过的 ECALL 指令。

因为普通 ECALL 指令为 32 位，通常表现为：

```
Saved mepc = 0x80201234

After successful ECALL handling:
Saved mepc = 0x80201238
```

否则恢复 S-mode 后，将再次执行同一条 ECALL，形成重复异常。

当前 upstream 的 `sbi_ecall_handler()` 在常规返回路径中完成这一更新，同时支持 Handler 跳过默认寄存器更新的机制。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



对于真正改变执行上下文的服务，还需要协调目标 Domain 的 Trap Frame 和返回寄存器，不能无条件把普通 SBI 返回逻辑套上去。

## 四、6.3 OpenSBI 究竟需要保存哪些 Context？

这一节我们直接结合实际代码数据结构。

### 4.1 核心数据结构：`struct hart_context`

源码：

[lib/sbi/sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)

当前 upstream 在文件内部定义了 `struct hart_context`。

将其按功能重新组织，可以得到以下逻辑结构（教学抽象，非可直接编译的原始 C 定义）：

```
struct hart_context_model {
    /* CPU trap return state */
    struct sbi_trap_context trap;

    /* Supervisor execution state */
    unsigned long supervisor_csrs[10];

    /* Extended register state */
    struct sbi_fp_context fp;
    struct sbi_vector_context *vector;

    /* Domain management */
    struct sbi_domain *domain;
    struct hart_context_model *previous;
    bool initialized;
};
```

实际实现使用单独的具名字段，而不是 `supervisor_csrs[10]` 数组。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里有一个非常重要的设计思想：

OpenSBI 没有把整个 Domain Context 简单保存到一个与 Linux Task 一样的 TCB 中，而是把不同 Domain 在各个 hart 上的执行状态分别保存。

因此它并不是一个典型的全局单实例 Domain Context。

### 4.2 Context 的两级索引模型

可以将实际的访问方式理解为：

```
                 OpenSBI Domain
                       |
           +-----------+-----------+
           |                       |
      Linux Domain             TEE Domain
           |                       |
       Hart Table              Hart Table
           |                       |
      +----+----+             +----+----+
      |         |             |         |
    Hart 0    Hart 1        Hart 0    Hart 1
      |         |             |         |
  LinuxCtx0 LinuxCtx1      TeeCtx0   TeeCtx1
```

因此：

```
Context = f(Domain, Hart Index)
```

而不是：

```
Context = f(Domain)
```

为什么要这样设计？

因为 Linux 可能在 Hart 0 上运行到 PC_A，同时在 Hart 1 上运行到 PC_B。

即使两个 hart 都属于 Linux Domain，也不能共享同一个 GPR 和 PC 恢复上下文。

实际的 `hart_context_get()` 就采用 Domain 的状态存储以及 Hart Index 来定位对应 Context。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 4.3 Supervisor CSR Context

当前 upstream 保存的主要 Supervisor CSR 包括：

| CSR          | 全称或含义                                         | 保存的原因                       |
| ------------ | --------------------------------------------- | --------------------------- |
| `sstatus`    | Supervisor Status                             | 中断使能、前一特权级及相关状态             |
| `sie`        | Supervisor Interrupt Enable                   | 各类 S-mode Interrupt Enable  |
| `stvec`      | Supervisor Trap Vector Base                   | 每个 Domain 有自己的 Trap Handler |
| `sscratch`   | Supervisor Scratch                            | Trap 入口的临时上下文               |
| `sepc`       | Supervisor Exception PC                       | S-mode Exception Return 地址  |
| `scause`     | Supervisor Cause                              | S-mode Trap Cause           |
| `stval`      | Supervisor Trap Value                         | Trap 附加信息                   |
| `sip`        | Supervisor Interrupt Pending                  | 相关软件可写的 Pending State       |
| `satp`       | Supervisor Address Translation and Protection | 页表根、地址翻译模式、ASID             |
| `scounteren` | Supervisor Counter Enable                     | 计数器访问策略                     |
| `senvcfg`    | Supervisor Environment Configuration          | 环境特性配置                      |
| `srmcfg`     | Supervisor Resource Management Configuration  | 资源管理配置，依赖扩展                 |

实际代码中，后几个字段有 ISA 版本或扩展支持检查，并不是所有处理器都需要相同操作。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里的 `sstatus` 不是完全独立于 `mstatus` 的寄存器；它是 Supervisor 可见和可操作的相关状态视图。这在后面分析 Trap Frame 恢复时尤其需要注意。

### 4.4 为什么 `stvec` 一定需要隔离？

假设 Linux 和 OP-TEE 各自具有：

```
Linux:
    stvec = 0x80210000

OP-TEE:
    stvec = 0x88010000
```

现在 OpenSBI 已经切入 OP-TEE，但忘记恢复 `stvec`。

那么 OP-TEE 执行期间，如果出现一个委托给 S-mode 的 Trap，CPU 将跳转到 Linux 的 Trap Handler 地址。

此时可能发生：

- 目标地址在 OP-TEE 页表下不存在，导致异常。
- 地址碰巧可访问，却执行了错误的处理逻辑。
- 如果权限配置存在缺陷，可能进一步造成安全问题。

同理，如果 `sscratch` 没有恢复，OP-TEE 的 Trap Handler 可能拿到错误的 Stack 或 Per-Hart 指针。

所以 Supervisor CSR 保存并不仅仅是让 OS 恢复运行，也是 防止两个内核执行状态混淆的必要措施。

### 4.5 `mepc` 和 `sepc` 为什么都要考虑？

这个问题特别容易混淆。

- `mepc`：M-mode Trap Return 时使用的 PC。
- `sepc`：S-mode Trap Return 时使用的 PC。

假设 Linux 在 S-mode 通过 ECALL 进入 M-mode：

```
Linux PC = 0x80201234
    |
    | ECALL
    v
M-mode:
    mepc = 0x80201234
```

如果 OpenSBI 要恢复到 OP-TEE：

```
M-mode:
    mepc = OP-TEE resume PC
    mstatus.MPP = S
    |
    | MRET
    v
OP-TEE S-mode
```

而 OP-TEE 自己在处理 U-mode TA Exception 时，则可能需要：

```
OP-TEE S-mode:
    sepc = TA resume PC
    |
    | SRET
    v
TA U-mode
```

因此两者分别属于不同层级的异常控制。

在 OpenSBI 中，`mepc` 和 `mstatus` 主要作为 M-mode Trap Frame 的组成部分保存，`sepc` 和其他 Supervisor CSR 则作为 Domain 的 S-mode 状态保存。

这一分层是理解后续 Context Swap 的关键。

## 五、6.4 Per-Hart Domain Context 的保存、恢复与生命周期

现在我们进入 OpenSBI Domain Context Switching 的核心实现。

整个机制围绕四个函数展开：

```
sbi_domain_context_init()
    |
    +-- Register per-domain state

sbi_domain_context_enter(target)
    |
    +-- hart_context_init()  [first use]
    |
    +-- switch_to_next_domain_context()
            |
            +-- Save current
            +-- Restore target

sbi_domain_context_exit()
    |
    +-- Resolve previous context
    |
    +-- switch_to_next_domain_context()
```

### 5.1 `sbi_domain_context_init()`：注册 Context 状态

OpenSBI 需要为 Domain × Hart 的组合建立存储空间。

其思路是：

```
For each Domain:
    Allocate per-hart context pointer table

For each eligible Hart:
    Allocate hart_context
    Bind context to corresponding Domain
```

严格地说，当前源码采用 Domain State 机制注册表项，并在各 hart 首次需要切换时创建具体 Context。不是在 `sbi_domain_context_init()` 内立刻为每个 hart 创建所有 `hart_context`。

这也是为什么第一次执行 `sbi_domain_context_enter()` 时，可能需要调用 `hart_context_init()`。

其真正价值在于：

把 Domain 的全局配置与某个 hart 在该 Domain 中的动态执行状态分离。

### 5.2 `sbi_domain_context_enter()`：进入目标 Domain

当前 upstream 的核心流程是：

```
Enter(target_domain)
    |
    +-- Validate target != current
    |
    +-- Validate hart belongs to target.possible_harts
    |
    +-- Ensure contexts exist
    |
    +-- current_ctx = get(current_domain, hart)
    |
    +-- target_ctx = get(target_domain, hart)
    |
    +-- target_ctx.prev_ctx = current_ctx
    |
    +-- switch_to_next_domain_context(
              current_ctx, target_ctx)
```

这里要重点理解 `possible_harts` 与 `assigned_harts` 的区别。

- `possible_harts` 决定某个 hart 是否有资格运行该 Domain。
- `assigned_harts` 表示当前分配给这个 Domain 的 hart 集合。

这与 Linux 的 CPU Affinity 和实际运行 CPU 有些相似，但用途和管理边界不同。

从源码看，`sbi_domain_context_enter()` 会检查目标 Domain 的 `possible_harts`，防止把当前 hart 切换到不支持的 Domain。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 5.3 `prev_ctx` 的设计为什么重要？

假设：

```
Linux Domain
    |
    | enter(TEE)
    v
TEE Domain
    |
    | exit()
    v
Linux Domain
```

OP-TEE 处理完请求后，OpenSBI 怎么知道应该恢复哪个 Context？

答案就是建立当前 Context 与调用方 Context 的关联：

```
TEE_HartContext.prev_ctx = Linux_HartContext
```

之后退出时：

```
next = current.prev_ctx
```

这就是 `prev_ctx` 的设计动机。

它不是简单地保存一个返回地址，而是保存 调用方的整个 Domain Execution Context。

当前 upstream 的 `sbi_domain_context_exit()` 优先选择 `prev_ctx`；如果没有前驱 Context，则可以寻找尚未初始化的目标 Domain，并依据初始化顺序启动。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



需要特别注意：`prev_ctx` 是一个指针，不是完整的任意深度上下文栈。对于嵌套调用和重新进入同一 Domain 的情况，仍须验证上层协议和生命周期是否允许，不能仅凭这一字段就认为支持任意递归 Domain Switch。

### 5.4 首次进入与恢复执行的区别

这也是一个十分重要的源码分支。

当 `target_ctx->initialized == false` 时：

```
Target Domain hasn't run before
    |
    +-- Is this target domain's boot hart?
            |
            +-- Yes: enter target.next_addr/next_mode
            |
            +-- No: hart stop / startup path
```

而如果已经初始化：

```
Target context initialized
    |
    +-- Restore saved trap state
    |
    +-- Return to target's previous execution point
```

这说明 Domain Context Switch 实际上承担两个不同任务：

首次启动 Domain 和 恢复已经运行过的 Domain。

第一次启动需要使用预先设置的 `next_addr`、`next_mode` 等启动参数。

而恢复则使用此前保存的寄存器和执行状态。

当前 OpenSBI 的 `switch_to_next_domain_context()` 通过 `initialized` 分支区分这两种情况，并可能调用 `sbi_hart_switch_mode()` 或 `sbi_hsm_hart_stop()`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这个设计能让 Domain 同时具备 Boot 生命周期与运行时 Context 生命周期。

## 六、6.5 实际源码：寄存器、CSR、FP、Vector 如何切换？

接下来重点分析：

`switch_to_next_domain_context()`

这实际上是 Chapter 06 最需要深入理解的函数。

### 6.1 整个函数的执行顺序

按照当前 upstream 的代码，能够整理出以下顺序：

1. 确定当前 Domain 和目标 Domain
2. 更新当前 hart 的 Domain Assignment
3. 保存当前 Supervisor CSR，恢复目标 CSR
4. 保存和恢复 FP / Vector Context
5. 保存当前 M-mode Trap Context，恢复目标 Trap Context
6. 重新配置目标 Domain 的硬件保护策略
7. 处理目标 Domain 首次启动或已初始化恢复
8. 由上层 Trap 返回汇编恢复寄存器并执行 `mret`

这个顺序非常值得研究。

特别是：软件的 Domain Assignment 更新先于硬件隔离配置切换。

因此，在安全分析时必须考虑两者之间是否存在可观察或可重入的过渡状态，以及异常、失败或中断是否可能使系统在中间状态下继续运行。

### 6.2 为什么使用 `csr_swap()`？

当前源码使用的一个典型语句是：

```
ctx->satp = csr_swap(CSR_SATP, dom_ctx->satp);
```

这个操作可以理解为：

```
old = read_csr(satp)
write_csr(satp, target.satp)
current.satp = old
```

其底层原理就是 RISC-V 的 CSR Read/Write 指令，例如：

```
csrrw t0, satp, t1
```

该指令的语义是：

```
t0 = old satp
satp = t1
```

因此，一个操作就实现了：

- 取出当前 Domain CSR。
- 写入目标 Domain CSR。
- 将旧值保存到当前 Domain 的 Context。

这比单纯执行 `csr_write()` 更方便实现对称的 Save/Restore 逻辑。

但要注意，这里没有自动实现任何 MMU/TLB 同步操作。

`csr_swap(CSR_SATP, ...)` 只完成寄存器值交换，不等于整个地址空间切换已经满足所有内存一致性要求。

### 6.3 GPR 为什么没有在这个 C 函数中逐个保存？

观察 `switch_to_next_domain_context()` 会发现一个非常有趣的现象：

它并没有显式保存：

```
x1 ra
x2 sp
x3 gp
...
x31 t6
```

为什么？

因为在进入 C Handler 之前，OpenSBI 的 Trap Entry Assembly 已经保存了 GPR。

GPR 当前位于 `sbi_trap_context` 所包含的 Trap Frame 中。

因此 Domain Context Switch 只需要完成逻辑上的 Trap Context 替换：

```
Active M-mode Trap Frame
    |
    +-- Copy into Current Domain Context
    |
    +-- Copy Target Domain Context
            into Active Trap Frame
```

这与 NuttX 中通过异常上下文切换来切换任务的设计思想有一定相似性。

尤其值得注意的是：

我们不是在 `switch_to_next_domain_context()` 内直接跳转到 OP-TEE 的 PC。

对于已经初始化的目标 Context，该函数将目标 Trap Frame 放入当前的异常返回位置，然后让 Trap Handler 按通用的返回流程继续执行。

最后由恢复汇编加载目标寄存器，并通过 `mret` 进入目标 Domain。

### 6.4 FP Context：为什么需要 Eager Save/Restore？

当前 OpenSBI 中，FP Context 由 `struct sbi_fp_context` 表示，包含浮点寄存器以及 `fcsr`。

源码在硬件支持 F/D 扩展时调用：

```
sbi_fp_save(current.fp_ctx)
sbi_fp_restore(target.fp_ctx)
```

这里采用的是 Eager Context Switch 思路，即在切换 Domain 的过程中保存和恢复扩展状态，而不是完全依赖后续首次使用时才恢复。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



为什么安全系统更需要关注这个问题？

考虑以下场景：

```
Linux FP Registers:
    f0 = Secret_A
    f1 = Secret_B

Switch to OP-TEE

OP-TEE:
    Reads f0 / f1
```

如果没有正确隔离 FP State，两个 Domain 就可能读取对方遗留的敏感状态。

这不只是功能性 Bug，而是信息泄露风险。

当然，实际 FP 访问受到 FS 状态和相关异常机制约束，但这不能替代正确的 Context 生命周期管理。

### 6.5 Vector Context：比 FP 更复杂

RISC-V Vector 的复杂性主要来自：

- `v0–v31` Vector Register。
- `VLEN` 可能随硬件实现变化。
- `vl`、`vtype`、`vstart` 等执行状态。
- `vcsr` 相关控制状态。
- `sstatus.VS` 管理。
- 保存和恢复的内存及时间成本可能较大。

当前 OpenSBI 的 Vector Context 采用动态大小存储，`struct sbi_vector_context` 包含控制状态以及可变长度的 `vregs[]`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么不直接定义：

```
uint64_t v[32];
```

因为这只能表示每个寄存器 64 位的情况，而 Vector Register 的大小并不固定为 64 位。

如果某个实现具有：

\\[ VLEN = 256\ \text{bits} \\]

则仅 32 个 Vector Register 的数据总量就达到：

\\[ 32 \times 256 / 8 = 1024\ \text{bytes} \\]

还不包括其他状态。

因此 Vector Context Switch 对性能、内存分配与安全性均有影响。

### 6.6 Eager 和 Lazy Context Switch 的取舍

| 方案                | 优势                        | 风险和成本                       |
| ----------------- | ------------------------- | --------------------------- |
| Eager             | 行为直接、状态隔离容易审计             | 即使扩展状态未变化也可能产生保存成本          |
| Lazy              | 可以避免不必要的保存恢复              | 必须精确管理 ownership、Trap 和首次使用 |
| Conditional Eager | 依据 Dirty/Enabled State 优化 | 状态机较复杂，需要可靠验证               |

对于普通 OS，Lazy FPU Context Switch 更多地是性能优化问题。

而对于安全 Domain，Lazy Context Switch 还必须回答：

切换后，目标 Domain 是否可能在扩展状态尚未正确隔离之前观察到前一个 Domain 的寄存器内容？

因此，优化前必须先证明隔离不变量，而不能只是证明浮点计算测试通过。

另外，`sstatus.FS` 和 `sstatus.VS` 与 `mstatus` 中的相关状态存在体系结构联系。Context Restore 必须保证保存的 Trap 状态与实际扩展寄存器状态一致。

## 七、6.6 PMP 重新配置为什么是 Domain Switch 的安全核心？

这一节承接 Chapter 05，但我们现在从真实执行顺序看问题。

### 7.1 首先理解 Context 与 Protection 的区别

假设执行完 CSR 和 GPR 保存/恢复之后：

```
GPR      = OP-TEE Context
satp     = OP-TEE Page Table
stvec    = OP-TEE Trap Handler
mepc     = OP-TEE Resume PC
```

但硬件 PMP 仍然是 Linux Domain 的访问权限配置。

此时虽然处理器马上要进入 OP-TEE，但它实际拥有的物理访问权限仍然可能不正确。

可能结果包括：

- OP-TEE 无法访问自己的 Secure RAM。
- 某些 Linux 权限意外残留。
- OpenSBI 或目标 Domain 触发 Access Fault。
- 在错误策略下发生越权访问。

因此，恢复寄存器之后，还必须在返回目标 S-mode 前落实目标 Domain 的硬件访问权限。

### 7.2 实际硬件隔离调用关系

当前 upstream 具备以下调用层次：

```
switch_to_next_domain_context()
    |
    v
sbi_hart_protection_reconfigure()
    |
    +-- select registered protection mechanism(s)
    |
    +-- unconfigure(current_domain)
    |
    +-- configure(target_domain)
             |
             +-- PMP / Smepmp implementation
             |
             +-- Other registered protection mechanisms
```

对应代码：

[sbi_hart_protection.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart_protection.c?utm_source=chatgpt.com)

在这套框架中，OpenSBI 可以注册不同类型的保护机制，例如基于普通 PMP 或 Smepmp 的 Memory Protection Backend。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



这意味着 OpenSBI Domain Layer 不需要为所有硬件实现都写一套特定的保护配置流程。

它只负责请求：

```
Switch protection from Domain A to Domain B
```

由底层 Backend 决定使用哪一种可用硬件机制。

### 7.3 普通 PMP 与 Smepmp 的关键区别

这一点对安全移植极其重要。

普通 PMP 的配置中：

- M-mode 对非锁定 PMP Entry 所限制的内存通常不受其权限约束。
- `L` Bit 被置位后，该 Entry 的配置通常不能随意修改，直到发生相应 Reset。
- M-mode Firmware 必须根据实际硬件能力选择可动态切换的配置。

Smepmp 引入额外的机器模式安全配置语义，包括 `mseccfg` 及其相关控制位，使 M-mode 本身的权限管理更加细致。

所以不能简单采用：

```
Domain A:
    PMP Entry L=1

Domain Switch:
    Rewrite same locked PMP Entry for Domain B
```

并假设一定成功。

当前 OpenSBI 的 Smepmp Backend 对 Firmware 专属区域、保留 PMP Entry、可配置状态及 Fence 都有专门处理。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 7.4 为什么 Protection Switch 需要同时考虑 WorldGuard？

以 SoC 中的 DMA 为例：

```
DMA Controller
    |
    | Read Physical Address
    v
OP-TEE Secure RAM
```

CPU 的 PMP 主要约束 CPU Hart 的物理访问，并不能自动保证 DMA Master 也遵守相同规则。

如果 DMA Controller 的访问路径并不受等价的硬件保护策略限制，那么 Linux 可能通过 DMA 间接访问 OP-TEE 内存。

因此，在支持 WorldGuard 的平台上，可以通过 World ID 及相应 Checker 对 CPU 和其他支持该机制的总线主体进行系统级资源授权。

RISE RP016 Milestone 5 的设计便涉及：

- 启动时配置 WorldGuard Checker。
- Domain 切换时更新 Hart 对应的 WorldGuard 状态。
- 保持不同 Domain 的 World ID 与权限策略一致。

  [image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

  RISC-V OP-TEE



注意：WorldGuard 并非每个 RISC-V SoC 的标准必备组件，且必须验证每个相关 Bus Master 是否受其策略覆盖。

### 7.5 Domain Switch 时的安全顺序

从安全设计角度看，我们希望满足：

```
1. Enter trusted M-mode execution
2. Stop untrusted domain execution
3. Save source context
4. Prepare destination execution context
5. Transition hardware protection
6. Verify destination protection is valid
7. Restore/commit destination execution
8. MRET into destination domain
```

这是我们用于安全分析的理想状态转换模型，并非当前 OpenSBI 源码逐语句的执行顺序。

实际 upstream 会先更新 Domain Assignment 和部分 CSR，再重新配置硬件保护。

为什么能够这样做？

因为在这段关键路径中，当前 hart 应保持在可信 M-mode 控制下，尚未返回低权限 Domain 执行。

但我们仍然需要分析 M-mode 嵌套异常、硬件配置失败、共享状态被观察等条件。

本质上：

> 软件的 Domain 所属信息先更新，并不等于低权限代码已经获得目标 Domain 的物理访问能力；最终必须以硬件授权配置为准。

这也说明仅凭源码中更新了 `assigned_harts`，不能证明安全切换已经完成。

## 八、6.7 `satp`、`SFENCE.VMA` 与跨 Domain TLB 隔离

这部分直接联系你之前学习过的 Sv39/Sv48 和 Memory Model。

### 8.1 `satp` 不只是一个普通 CSR

RV64 在 Sv39 模式下，`satp` 主要包含：

```
63       60 59       44 43                        0
+----------+-----------+--------------------------+
|   MODE   |    ASID   |           PPN            |
+----------+-----------+--------------------------+
```

其中：

- `MODE`：地址转换模式。
- `ASID`：Address Space Identifier。
- `PPN`：根页表所在物理页号。

假设：

```
Linux Domain:
    satp = Sv39 | ASID 1 | RootPA_A

OP-TEE Domain:
    satp = Sv39 | ASID 2 | RootPA_B
```

Domain Switch 时，需要恢复目标 Domain 的 `satp`。

但只恢复 `satp`，并不必然满足地址转换缓存的同步要求。

### 8.2 为什么写 `satp` 不等于 Flush TLB？

RISC-V Privileged Architecture 明确规定，写 `satp` 本身不隐含对页表修改和后续地址转换的同步，也不要求自动使地址转换缓存全部失效。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V 文档



原因主要是性能。

如果每次地址空间切换都必须无条件 Flush 所有 TLB，那么 ASID 的性能优势就很难发挥。

因此 RISC-V 把：

```
Select address space
```

和：

```
Synchronize address translations
```

作为两个相关但不同的操作。

### 8.3 三个操作的区别

| 操作               | 主要作用                 |
| ---------------- | -------------------- |
| `csrw satp, ...` | 选择新的地址翻译模式、ASID 和页表根 |
| `sfence.vma`     | 同步本 hart 的地址转换相关状态   |
| SBI RFENCE       | 请求远端 hart 执行相应地址转换同步 |

这里需要纠正一个常见误解：

`SFENCE.VMA` 不是普通的 D-Cache Flush。

它关注的是页表修改与隐式地址转换访问之间的顺序，以及本地地址转换缓存状态。

它同样不等于对其他所有 hart 执行了 Flush。

### 8.4 一个实际的跨 Domain TLB 风险

假设两个 Domain 在某个 hart 上复用了同一个 ASID：

```
Linux:
    ASID = 0
    VA 0x40000000 -> PA 0x80200000

OP-TEE:
    ASID = 0
    VA 0x40000000 -> PA 0x88000000
```

如果切换时不进行必要的地址转换同步，处理器可能保留与旧地址空间相关的缓存状态。

这就是为什么 ASID 的隔离策略、生命周期与 Fence 规则必须一起设计。

不能简单地认为 `satp.PPN` 不同，就必然不可能出现旧地址转换被使用的问题。

### 8.5 OpenSBI 的实际 Fence 机制

当前 PMP Backend 会在相关保护配置完成后执行 `sbi_hart_pmp_fence()`。

它会在具备 S-mode 的情况下执行本地地址转换同步；如果支持 H Extension，还会考虑相应的 Hypervisor 地址转换缓存同步。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



同时，OpenSBI 还有：

`lib/sbi/sbi_tlb.c`

其中实现了本地 `SFENCE.VMA` 和面向多 hart 的 TLB 同步基础设施。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



必须区分两件事情：

第一，Domain Switch 时的本 hart 同步。

目标是保证当前 hart 进入目标 Domain 时，地址转换和保护配置正确。

第二，多个 hart 共享某套页表时的跨 hart TLB Shootdown。

目标是确保其他 hart 不会继续使用已失效的地址转换信息。

举例：

```
Hart 0                          Hart 1
  |                               |
  | Modify OP-TEE PTE             | Running OP-TEE
  |                               |
  | Request Remote Fence -------> |
  |                               | sfence.vma
  | <----------- Completion       |
  |                               |
  | Continue                      |
```

普通 `sfence.vma` 只对执行它的 hart 生效。

如果目标页表同时被其他 hart 使用，修改方必须通过恰当的跨 hart 协议保证同步完成。

这一点在真正的 OP-TEE SMP 支持中非常关键。

### 8.6 为什么不能简单使用 `fence rw,rw` 代替？

因为：

```
fence rw, rw
```

主要约束显式数据内存访问的顺序。

而：

```
sfence.vma x0, x0
```

专门负责地址转换相关的顺序语义。

二者关注的对象并不相同。

即使修改页表的 Store 对其他 hart 已经可见，也不能直接推导出它们的地址转换缓存已经失效。

反过来，执行本地 `sfence.vma` 也不自动替你完成共享页表的跨 hart 通知。

这正是 RISC-V Memory Model 与 MMU 架构必须联合理解的地方。

## 九、6.8 从 Linux Domain 切换到 OP-TEE Domain：完整执行过程

现在我们将前面学到的所有机制串起来。

这一节重点分析 一次已经初始化的 OP-TEE Domain 被 Linux 请求唤醒 的情况。

### 9.1 以 RPMI TEE_CALL 为例

2026 年 9 月的 Linux RFC 方案使用 SBI MPXY 传输 RPMI TEE Service。其设计是在 Linux 发起 TEE 请求时，让 M-mode 固件将调用 hart 切换到 OP-TEE Domain，等待安全侧返回响应。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

开放墙列表

+1



下面的流程是该方案的逻辑执行链，不能等同于当前所有 upstream 分支都已具有的完整实现。

```
Linux User Space
    |
    | TEEC_InvokeCommand()
    v
Linux TEE Driver
    |
    | RPMI TEE_CALL over SBI MPXY
    v
S-mode ECALL
    |
    | Trap
    v
OpenSBI M-mode
    |
    | Validate request and target domain
    |
    | Save Linux execution state
    |
    | Restore OP-TEE execution state
    |
    | Reconfigure hardware isolation
    |
    | MRET
    v
OP-TEE S-mode
    |
    | Receive TEE request
    |
    | Trusted Thread / TA execution
    v
TEE Service
```

### 9.2 具体到 Hart 0 的状态变化

假设切换前：

| 状态                   | Linux Domain        |
| -------------------- | ------------------- |
| Privilege            | S-mode              |
| `sp`                 | `0x80F00000`        |
| `stvec`              | `0x80210000`        |
| `satp.PPN`           | `0x00080200`        |
| `mepc`（ECALL Trap 后） | `0x80201234`        |
| Active Domain        | Linux               |
| Hardware Policy      | Linux access policy |

上面的数值只用于教学，不保证对应真实平台的有效页表布局。

进入 OpenSBI 后：

Step 1 — 保存 Linux Trap Context

OpenSBI Trap Entry 汇编保存 GPR、`mepc` 和 `mstatus`，将它们组织成 Trap Frame。

Step 2 — 找到目标 Context

OpenSBI 根据当前 Hart Index 找到：

```
linux_ctx = Context[Linux][Hart0]
tee_ctx   = Context[OPTEE][Hart0]
```

Step 3 — 建立返回关系

```
tee_ctx.prev_ctx = linux_ctx
```

Step 4 — 保存 Linux Supervisor State

保存 Linux 的：

```
sstatus, sie, stvec, sscratch,
sepc, scause, stval, sip,
satp, ...
```

Step 5 — 恢复 OP-TEE Supervisor State

将 OP-TEE 保存的 Supervisor CSR 写回当前 hart。

Step 6 — 保存和恢复 FP/Vector

将 Linux 的 FP/Vector 状态保存到 `linux_ctx`，恢复 OP-TEE 的对应状态。

Step 7 — 替换 Trap Frame

这是整个执行流的关键一步：

```
Active Trap Frame
    |
    +-- Save as Linux trap context
    |
    +-- Replace with OP-TEE trap context
```

现在，后续的 Trap Return 将恢复 OP-TEE 的寄存器和 PC，而不是 Linux 的。

Step 8 — 切换 Hardware Isolation Policy

应用 OP-TEE Domain 的硬件访问权限。

Step 9 — MRET

OpenSBI 恢复目标上下文并执行：

```
mret
```

假设目标 Trap Frame 已设置：

```
mepc        = OP-TEE Resume PC
mstatus.MPP = S
```

那么 `mret` 之后：

```
Privilege = S-mode
PC        = OP-TEE Resume PC
```

处理器由此恢复 OP-TEE 执行。

### 9.3 一个非常重要的问题：OP-TEE 的 Stack 从哪里恢复？

答案是：

来自目标 Domain 的 Trap Frame 中保存的 `sp`。

不是 OpenSBI 临时使用的 M-mode Trap Stack。

在切换过程中实际上存在三个不同的 Stack：

```
Linux Kernel Stack
       |
       | ECALL
       v
OpenSBI M-mode Trap Stack
       |
       | Restore target trap frame
       v
OP-TEE Kernel Stack
```

这里有一个容易忽视的细节。

当执行 `mret` 时，OpenSBI 必须已经恢复了 OP-TEE 所需的 Stack Pointer。

否则处理器虽然跳到了 OP-TEE 的 PC，但后续任何依赖 Stack 的指令都可能执行失败。

因此 Domain Switch 的正确性不是仅检查：

```
mepc == expected_pc
```

还必须检查：

```
sp     == expected_stack
satp   == expected_address_space
stvec  == expected_trap_vector
policy == expected_domain_policy
```

这些状态共同定义了一个可以安全继续执行的 S-mode 环境。

## 十、6.9 OP-TEE 返回 Linux Domain：Context Resume 的实现

现在考虑 OP-TEE 完成请求后的反向过程。

### 10.1 OP-TEE 为什么不能直接跳回 Linux？

假设 OP-TEE 直接执行：

```
jr linux_resume_pc
```

会有什么问题？

这条指令只修改 PC，不会自动：

- 切换到 Linux 页表。
- 恢复 Linux `sp`、`tp`、GPR。
- 恢复 Linux `stvec`。
- 恢复 Linux 的安全访问权限。
- 恢复 Linux 的 FP/Vector Context。

更重要的是，OP-TEE 不能仅靠自己在 S-mode 中修改寄存器，就重新配置受 M-mode 控制的全部硬件隔离策略。

所以返回过程也必须通过可信的 Domain Context Switch。

### 10.2 OpenSBI `sbi_domain_context_exit()`

该函数会取得当前 Context 的 `prev_ctx`。

```
Current Context = TEE_HartContext
    |
    +-- prev_ctx = Linux_HartContext
    |
    +-- Clear current prev_ctx
    |
    +-- switch_to_next_domain_context(
                TEE_HartContext,
                Linux_HartContext)
```

这里的 `prev_ctx` 表示上次进入目标 Domain 时记录的调用方。

它让 OP-TEE 返回时不需要重新构造整个 Linux 执行状态。

这是因为 Linux Context 在上一次 Domain Switch 时就已经完整保存了。

### 10.3 恢复 Linux 的 `mepc`

假设 Linux 原来执行：

```
ecall
addi s0, s0, 1
```

那么从 TEE 返回后，Linux 应该继续执行 `addi`，而不是再次执行 `ecall`。

这要求 Linux 保存的返回 PC 已经符合对应 SBI 调用协议的恢复语义。

对于普通 SBI ECALL，OpenSBI 的通用处理路径通常会推进保存的 PC；对于能够替换 Trap Frame 的 Domain 切换服务，还必须确认 ECALL Handler 的寄存器更新与 Context 保存顺序正确。

不能把：

```
Save Linux PC
```

和：

```
Prepare Linux ECALL Return PC
```

混为一谈。

前者是执行状态保存，后者是调用协议语义。

如果顺序处理错误，就可能出现：

- 同一个 ECALL 重复执行。
- 返回后跳过不应该跳过的指令。
- Linux 接收到错误的 SBI 返回值。
- RPC 恢复时请求状态混乱。

### 10.4 RPC 为什么使问题更加复杂？

一个 TA 调用不一定马上结束。

例如 OP-TEE 需要 Linux `tee-supplicant` 提供文件系统服务：

```
Linux
    |
    | Invoke TA
    v
OP-TEE
    |
    | Need REE filesystem service
    v
Linux
    |
    | tee-supplicant RPC
    v
OP-TEE
    |
    | Continue original TA
    v
Linux
    |
    | Return to CA
```

因此，一次 TA Invoke 可能发生多次 Domain Switch。

但必须区分：

- OpenSBI 保存的是 Domain 在该 hart 上的执行状态。
- OP-TEE 保存的是自己的 Trusted Thread / TA 执行状态。
- Linux 负责自己的进程和线程上下文。

OpenSBI 不需要了解 TA 内部所有 Syscall 或 Thread State，它只需要正确保存和恢复对应 Domain 的架构状态。

这体现了优秀的架构分层：

> OpenSBI 管理 Domain；OP-TEE 管理 Trusted Execution；Linux 管理 REE Tasks。每层只维护自己负责的执行上下文。

## 十一、6.10 Domain Switch 安全不变量与失败恢复

这一节我建议作为 Staff Engineer 水平的重点内容。

前面讨论的主要是正常路径，但真正决定架构成熟度的往往是异常路径。

### 11.1 什么是 Security Invariant？

Invariant（不变量）指系统在允许的状态转换过程中必须持续保持的正确性条件。

对于 Domain Switch，至少需要定义五类不变量。

| 不变量       | 必须满足的条件                           |
| --------- | --------------------------------- |
| Execution | 恢复的 PC、SP、GPR 和 CSR 属于目标 Domain   |
| Memory    | 目标 Domain 不能获得源 Domain 未授权的内存访问能力 |
| Identity  | 当前 Hart 所属 Domain 与实际硬件保护状态相符     |
| Isolation | FP/Vector、Trap 和其他敏感状态不会发生非授权泄露   |
| Recovery  | 失败后不得以未知或部分授权的状态进入不可信代码           |

### 11.2 必须重点审查的源码问题：Protection Reconfigure 失败

这里有一个值得你实际跟进的 Review 点。

在当前 upstream 的 `switch_to_next_domain_context()` 中，可以看到它调用：

```
sbi_hart_protection_reconfigure(
    scratch, current_dom, target_dom);
```

而这个调用点没有使用返回值作进一步的显式错误处理。

与此同时，`sbi_hart_protection_reconfigure()` 本身返回 `int`，其 Backend 配置失败时可以返回错误码。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

+1



这意味着存在一个值得深入验证的问题：

如果目标 Domain 的硬件保护配置失败，调用方是否仍会沿着后续 Context Restore 路径执行？

从代码结构上看，这是需要审查的错误传播与失败恢复问题。

但仅凭这一段代码还不能认定存在一个可利用的安全漏洞。还需要确认 Backend 的失败条件、调用时的硬件状态、平台先决条件和实际 Trap Return 行为。

从安全设计角度，我们希望：

```
Protection Reconfiguration
            |
            v
        Success?
        /     \
      Yes      No
       |        |
       v        v
   Commit    Fail Closed
   Switch    / Recovery
       |
       v
      MRET
```

而不是：

```
Protection Reconfiguration failed
            |
            v
        Ignore error
            |
            v
           MRET
```

### 11.3 失败恢复为什么不是简单 Rollback？

假设切换过程已经执行：

```
1. Current Domain Assignment updated
2. Some target CSRs restored
3. Source PMP configuration removed
4. Target PMP configuration failed
```

如果此时简单返回 Linux：

```
return SBI_EFAIL;
```

就一定安全吗？

不一定。

因为源 Domain 的部分硬件保护可能已经被清除，而目标 Domain 的新配置还没有完成。

所以此时至少存在三种设计策略：

1. Rollback：恢复可信的源 Domain 安全策略，并验证恢复成功。
2. Fail Closed：进入已知的安全状态，不再返回不可信 S-mode。
3. Fatal Reset / Halt：在无法确保隔离的情况下终止当前执行环境。

不能未经分析就选择第一种。

因为有些硬件安全配置具有不可逆或一次性锁定语义，或者重新配置失败意味着原始安全策略也无法可靠恢复。

真正的工程原则是：

如果无法证明源 Domain 的安全状态已完整恢复，就不能允许它重新获得执行。

### 11.4 一个正确的设计应该怎样组织状态机？

我建议在 Code Review 时引入如下状态模型：

```
RUNNING_SOURCE
      |
      v
TRAPPED_IN_MONITOR
      |
      v
SOURCE_CONTEXT_SAVED
      |
      v
TARGET_CONTEXT_PREPARED
      |
      v
PROTECTION_TRANSITION
      |
      +------ Failure ------> SAFE_FAILURE
      |
      v
TARGET_POLICY_VALID
      |
      v
TARGET_CONTEXT_COMMITTED
      |
      v
RUNNING_TARGET
```

注意：这只是用于设计和验证的逻辑状态机，不是现有 OpenSBI 的正式状态枚举。

它的价值在于帮助你逐个检查：

- 哪些操作可能失败？
- 失败之后当前属于哪个 Domain？
- 哪些寄存器已经改变？
- 哪些硬件权限已经改变？
- 此时是否允许发生中断？
- 还能安全恢复到哪个 Context？
- 是否可能出现部分更新状态？

### 11.5 还需要考虑哪些异常场景？

场景 A：目标 Context 不存在

例如当前 hart 并不属于目标 `possible_harts`。

应在修改状态前拒绝切换。

场景 B：FP/Vector Context Allocation 失败

如果首次分配 Context 时内存不足，必须保证不会使用未初始化的扩展状态缓冲区。

场景 C：Domain Switch 期间发生 M-mode Nested Trap

需要确认当前 Trap Stack、`mscratch`、保存的 `mstatus` 和硬件权限仍然可靠。

场景 D：目标 Domain 恢复了错误的 `stvec`

如果目标 S-mode 马上发生 Trap，系统可能进入错误的 Trap Handler。

场景 E：目标 Domain 的 `satp` 与硬件隔离策略不匹配

可能产生 Access Fault，也可能在其他配置缺陷存在时扩大安全风险。

场景 F：Hart 0 正在更新共享 Domain 状态，Hart 1 同时执行相关代码

需要明确保护锁、原子操作、Hart Assignment 与跨 Hart 同步的职责。

### 11.6 再进一步：不只是 GPR 和 CSR

一个安全的 Domain Switch 设计，还应根据具体硬件考虑如下状态。

| 状态类别                     | 需要分析的问题                            |
| ------------------------ | ---------------------------------- |
| FP / Vector              | 扩展寄存器是否完整隔离                        |
| PMP / Smepmp             | 是否存在短暂的权限扩大                        |
| WorldGuard               | WID 和其他 Bus Master 权限是否一致          |
| Debug / Trigger          | 调试寄存器是否能够跨 Domain 观察状态             |
| PMU / Counter            | 性能计数器是否可能泄露跨 Domain 信息             |
| Hypervisor State         | 如果支持 H Extension，相关 HS/VS 状态是否需要处理 |
| Cache / Branch Predictor | 是否具有跨安全域的微架构侧信道风险                  |
| Timer / Interrupt        | 哪个 Domain 拥有中断，是否会错误投递             |
| DMA / I/O                | 非 CPU Master 能否绕过隔离                |

必须强调：上表是完整安全审计清单，不表示当前 OpenSBI 的 `hart_context` 已保存所有这些状态。

其中部分机制由平台配置、架构扩展、固件生命周期或其他子系统负责，而不一定由 Domain Context Switch 函数直接处理。

RP016 Milestone 6 对 FP/Vector Eager Context Switch 的设计讨论尤其值得阅读。该功能已经合入 upstream；其早期 Lazy 方案被认为无法可靠依赖 S-mode 可修改的 FS/VS 状态来判断寄存器使用情况，因此最终采用 Eager 模型。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



这就是一个很好的例子：有些看似合理的 OS Context Switch 优化，放到不同安全域之间就未必成立。

## 十二、综合实例：一次完整 Domain Switch 的寄存器追踪

我们最后用一个真实开发中可采用的分析方法，将核心知识串起来。

假设 Hart 0 已经运行过 Linux 和 OP-TEE，两个 Domain 的 Context 都已经初始化。

### 12.1 切换前的状态

Hart 0 · Context Snapshot

| 状态        | Linux Context | OP-TEE Context |
| --------- | ------------- | -------------- |
| Privilege | S-mode        | S-mode         |
| Resume PC | 0x80201238    | 0x88001200     |
| SP        | 0x80F00000    | 0x88F00000     |
| stvec     | 0x80210000    | 0x88010000     |
| satp root | Linux root    | TEE root       |
| FP state  | Linux FP      | TEE FP         |
| Policy    | Linux         | OP-TEE         |

示例数值为教学构造，不是实际采集的调试日志。

当 Linux 发起请求后：

```
Step 1: ECALL -> M-mode
        |
        v
Step 2: Save Linux trap frame
        |
        v
Step 3: Swap S-mode CSR context
        |
        v
Step 4: Save Linux FP/Vector
        Restore TEE FP/Vector
        |
        v
Step 5: Replace active trap frame
        |
        v
Step 6: Apply target hardware isolation
        |
        v
Step 7: Restore TEE GPR/mepc/mstatus
        |
        v
Step 8: MRET -> OP-TEE S-mode
```

切换成功后，应检查：

```
Current Hart: 0

Privilege:
    S-mode

PC:
    OP-TEE resume PC

SP:
    OP-TEE saved stack pointer

stvec:
    OP-TEE trap vector

satp:
    OP-TEE address space

Hardware Protection:
    OP-TEE domain policy

Active Context:
    OP-TEE Hart 0
```

如果仅仅 PC 正确，但页表或硬件安全策略没有正确切换，仍然不能认定 Domain Switch 成功。

这就是本章最重要的检查原则。

## 十三、QEMU + GDB 实验：实际观察 Domain Context Switch

我建议你将本章的实验分成三个等级。

### 实验 A：分析 OpenSBI Trap Entry

准备一个带调试符号的 OpenSBI/QEMU 开发环境。

QEMU 可以采用暂停启动并开放 GDB Server 的方式：

```
# 在现有 QEMU 启动参数中增加
-S -s
```

随后使用 GDB 连接：

```
gdb-multiarch path/to/fw_dynamic.elf
```

其中 ELF 路径需要替换为你实际构建的 OpenSBI ELF。

```
target remote :1234

set pagination off

break _trap_handler
break sbi_trap_handler
break sbi_ecall_handler

continue
```

在 Trap Entry 处检查：

```
info registers

p/x $mstatus
p/x $mepc
p/x $mcause
p/x $mscratch
p/x $mtvec

x/16i $pc
```

实验目标是观察：

1. ECALL 进入 M-mode 之前和之后的状态。
2. `mstatus.MPP` 是否为 S-mode。
3. `mepc` 是否指向 ECALL。
4. OpenSBI 如何通过 `mscratch` 建立可信 Trap Stack。
5. Trap Handler 如何构造 `sbi_trap_context`。

实际 GDB 对 CSR 的寄存器命名支持取决于 GDB/QEMU 版本。如果无法直接读取特定 CSR，可通过反汇编或临时调试代码确认。

### 实验 B：观察 OpenSBI Domain Context Switch

在具备多 Domain 切换功能的固件中，设置：

```
break sbi_domain_context_enter
break sbi_domain_context_exit
break switch_to_next_domain_context

continue
```

第三个函数是 `static`，需要相应调试符号。若未保留符号，可通过文件行号设置断点。

进入 `switch_to_next_domain_context()` 后：

```
bt
info args
info locals

p/x current_hartid()
p/x current_dom
p/x target_dom
```

然后观察：

```
p/x ctx->satp
p/x dom_ctx->satp

p/x ctx->stvec
p/x dom_ctx->stvec

p/x ctx->trap_ctx.regs.mepc
p/x dom_ctx->trap_ctx.regs.mepc
```

逐步执行并重点记录：

- Domain Assignment 更新前后。
- CSR 交换前后。
- Trap Context 交换前后。
- Hardware Protection Reconfigure 前后。
- 最终 `mret` 前后。

建议将结果组织成这种调试表格：

| 检查点                   | Current Domain | `satp` | Resume PC | Protection |
| --------------------- | -------------- | ------ | --------- | ---------- |
| Before Switch         | Linux          | Linux  | Linux     | Linux      |
| After CSR Swap        | Transitional   | OP-TEE | 尚在切换      | 旧策略        |
| After Trap Frame Swap | Transitional   | OP-TEE | OP-TEE    | 旧策略        |
| After Protection      | OP-TEE         | OP-TEE | OP-TEE    | OP-TEE     |
| After `mret`          | OP-TEE         | OP-TEE | OP-TEE    | OP-TEE     |

以上是按源码关键阶段划分的逻辑检查点，不代表所有中间状态都能够用普通 GDB 单步观察。

### 实验 C：Protection Failure Injection

这是最有机会转化为 upstream 改进工作的实验。

目标不是主动绕过系统安全性，而是在你可控制的 QEMU 测试平台中验证错误处理。

建议的实验：

1. 找到 `sbi_hart_protection_reconfigure()` 对应的 Backend。
2. 在测试构建中模拟一次 `configure()` 返回错误。
3. 观察 `switch_to_next_domain_context()` 对返回值的处理。
4. 检查目标 Domain 是否仍然会被恢复执行。
5. 比较实际软件状态和硬件权限配置。
6. 设计能确保 Fail Closed 的处理策略及回归测试。

其中最有价值的输出不是单纯得到一个 Panic Log，而是形成完整的技术说明：

```
Problem:
    Domain protection reconfiguration can fail.

Expected:
    Failed isolation transition must not resume
    an untrusted domain in an undefined state.

Test:
    Inject a protection backend failure.

Verify:
    No insecure domain execution after failure.
    Hardware and software state remain consistent.

Proposed Fix:
    Propagate failure and define a safe
    rollback/fatal-recovery path.
```

提交 PR 之前，应先核对目标 commit、已有 Issue、实际配置的保护 Backend，以及该异常是否已由其他层保证不可发生。

## 十四、Staff Engineer 级测试题

下面这些题目用于验证你是否真正掌握本章的设计理念，而不是只熟悉函数名。

10 道架构与源码分析题

展开所有答案

Q1 · 为什么 OpenSBI Domain Context 必须是 per-domain、per-hart 的结构，而不能只保存一个全局 Domain Context？

Q2 · 为什么 \`sbi_domain_context_enter()\` 必须校验目标 Domain 的 \`possible_harts\`？

Q3 · 为什么 OpenSBI 的 \`switch_to_next_domain_context()\` 不需要逐条保存 x1–x31？

Q4 · 为什么恢复 \`satp\` 和 \`stvec\` 属于安全问题，而不只是功能问题？

Q5 · 为什么 OpenSBI Domain Switch 比普通 Linux Task Context Switch 更强调硬件保护策略？

Q6 · 为什么不能只根据 \`sstatus.FS/VS\` 判断是否需要保存 FP/Vector？

Q7 · 为什么写入目标 \`satp\` 之后仍可能需要 \`SFENCE.VMA\`？

Q8 · 如果 Hart 0 修改了两个 Domain 共用的映射，Hart 1 是否会因为 Hart 0 执行 \`SFENCE.VMA\` 而自动同步？

Q9 · 如果 Protection Backend 配置失败，能否立即回滚到 Linux？

Q10 · 假设 OP-TEE 的 PC、SP、satp 都正确恢复，但 WorldGuard WID 仍为 Linux，能否执行 \`mret\`？

这里给出每道题的核心判断依据；正式代码级解答还需要结合目标 ISA、硬件保护 Backend 和实际执行路径。

## 十五、本章总结：必须建立的五条设计原则

完成 Chapter 06 后，最重要的不是记住 `sbi_domain_context_enter()` 等函数，而是建立以下认知：

原则一：Trap Entry 是 Context Switch 的执行基础。

GPR 保存与 M-mode Trap Frame 的建立，决定了后续能否安全恢复另一个 Domain。

原则二：Domain Context 是 Domain 与 Hart 的组合。

一个 Domain 可以在多个 hart 上运行，而每个 hart 的运行状态必须独立保存。

原则三：执行状态切换和安全权限切换是两项不同的工作。

只有二者都正确，才能称为成功的 Domain Switch。

原则四：CSR Restore、FP/Vector Restore 和 Memory Synchronization 是相互关联的。

不能只检查寄存器值，还需要关注地址翻译缓存、扩展状态和多核一致性。

原则五：任何不能证明安全完成的 Protection Transition，都不允许继续执行不可信 Domain。

这决定了 Domain Context Switch 的异常路径必须被视为安全架构的一部分，而不是普通错误处理。

### 推荐的本章源码阅读顺序

1. [fw_base.S](https://github.com/riscv-software-src/opensbi/blob/master/firmware/fw_base.S?utm_source=chatgpt.com) — M-mode Trap Entry、GPR Save/Restore 与 `mret`。
2. [sbi_trap.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_trap.c?utm_source=chatgpt.com) — Trap Dispatch。
3. [sbi_ecall.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_ecall.c?utm_source=chatgpt.com) — SBI Extension Dispatch 与 ECALL 返回。
4. [sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com) — Domain Context Save/Restore 核心。
5. [sbi_hart_protection.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart_protection.c?utm_source=chatgpt.com) — 硬件隔离配置框架。
6. [sbi_hart_pmp.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart_pmp.c?utm_source=chatgpt.com) — PMP/Smepmp 实际实现。
7. [RISE RP016 Milestone 6](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/milestone6/?utm_source=chatgpt.com) — FP/Vector Context Switch 的设计历史和测试。

对后续深入研究最有价值的一个问题是：

> 在 OpenSBI Domain Switch 的整个执行过程中，能否严格证明「被恢复的执行上下文」与「实际生效的硬件安全策略」始终相匹配，且不存在错误返回、状态泄露或跨 hart 不一致？

能够从代码、ISA 规范、硬件机制和回归测试四个层面回答这一问题，就已经从普通的移植和调试工作，进入了真正的 TEE Architecture Engineering 领域。

下一章 Chapter 07 将在这个基础上进一步讨论 Per-Hart Domain Context、Global Domain State、SMP 同步、并发 Domain Switch 和多核硬件隔离一致性。