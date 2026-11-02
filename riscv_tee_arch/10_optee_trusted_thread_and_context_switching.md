# Chapter 10 — OP-TEE Trusted Thread & Context Switching

RISC-V OP-TEE Architecture & Porting · Core Kernel 深度教程 · Staff Engineer Level

## 本章定位与学习目标

在前面的课程中，我们已经讨论了 RISC-V TEE 的安全隔离架构、OpenSBI Domain、PMP、Boot Flow、Trap/Exception 等基础机制。

现在要解决一个更深入的问题：

当 Linux 发起一个 TEE 请求，OP-TEE 究竟是如何执行它、保存它、挂起它，并在后续重新恢复执行的？

这就是 Trusted Thread 机制要解决的核心问题。

本章覆盖原定 Chapter 10 的全部十项内容，并进一步加入 RISC-V 汇编实现、跨 hart 恢复、安全不变量、调试实验和 Staff Engineer 级工业场景分析。

| 章节    | 主题                                       | 重点                           |
| ----- | ---------------------------------------- | ---------------------------- |
| 10.1  | Trusted Thread 的设计动机                     | 为什么不需要传统 RTOS Scheduler      |
| 10.2  | 与 RTOS Task / pthread 的区别                | 三种不同的线程模型                    |
| 10.3  | Trusted Thread State Machine             | FREE、ACTIVE、SUSPENDED        |
| 10.4  | Per-Hart Stack Architecture              | Temporary、Thread、Abort Stack |
| 10.5  | Thread Allocation & Lifecycle            | 分配、运行、释放                     |
| 10.6  | Suspend / Resume                         | 汇编级保存和恢复                     |
| 10.7  | Context Data Structures                  | GPR、CSR、SP、FP、TA 地址空间        |
| 10.8  | RPC Suspend / Resume                     | Linux 服务调用及恢复机制              |
| 10.9  | Trusted Thread vs OpenSBI Domain Context | 两套相互独立的切换机制                  |
| 10.10 | Trusted Thread / TA Context / Linux Task | 三者关系及完整执行模型                  |

源码基线： 本章以 2026 年 10 月 28 日核查的 `OP-TEE/optee_os` upstream `master` 为主要依据。具体文件包括：

- `core/kernel/thread.c`：跨架构通用 Thread 基础设施。
- `core/include/kernel/thread_private.h`：Thread 状态和 Context。
- `core/arch/riscv/include/kernel/thread_arch.h`：RISC-V 寄存器和 Per-Hart 结构。
- `core/arch/riscv/kernel/thread_arch.c`：RISC-V Thread 生命周期。
- `core/arch/riscv/kernel/thread_rv.S`：Trap、寄存器与 Context Restore。
- `core/arch/riscv/kernel/thread_optee_abi_rv.S`：特定 OP-TEE ABI 的入口及 RPC 路径。

需要特别注意：RISC-V OP-TEE 的通信接口和平台集成仍在演进。Trusted Thread 的运行原理与某一种 Linux/OpenSBI 通信协议不是同一层概念。 本章会区分当前源码中的具体 ABI 实现和可应用于不同 Transport 的内核机制。

# 10.1 为什么 OP-TEE 不采用传统 RTOS Scheduler？

## 10.1.1 首先回答：Trusted Thread 到底是什么？

可以把 OP-TEE Trusted Thread 理解为：

> 一个由 OP-TEE Kernel 管理的、用于执行和保存安全侧请求状态的执行上下文。它能够在跨越 REE/TEE 交互时挂起，并在之后恢复原来的执行流程。

重点是 Execution Context，而不一定是一个由独立调度器管理的任务。

先看一个典型场景：

```
Linux Application
      |
      | TEEC_InvokeCommand()
      v
Linux TEE Driver
      |
      | Enter TEE
      v
OP-TEE Kernel
      |
      | Allocate Trusted Thread #2
      v
Trusted Thread #2
      |
      | Invoke TA
      v
Trusted Application
      |
      | Return result
      v
Trusted Thread #2
      |
      | Release Thread #2
      v
Linux Application
```

在这一过程中，OP-TEE 不必再决定：

- 哪个用户进程应当运行？
- 哪个进程具有最高优先级？
- 哪个任务的时间片已经耗尽？
- 下一个该调度哪个普通应用线程？

这些问题通常已经由 Linux Scheduler 解决。

OP-TEE 关心的是：当前进入 TEE 的请求应该绑定哪一个可用的 Trusted Thread Context，以及如何安全完成或暂停该请求。

OP-TEE 的架构文档明确说明：传统的 OP-TEE Trusted Thread 模型不提供独立的线程调度器，而是随 REE 调用线程的调度进度执行。

[image](https://www.google.com/s2/favicons?domain=https://optee-docs.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 10.1.2 从 RTOS Scheduler 的设计目的理解

假设你正在设计一个 NuttX Kernel。

系统拥有三个任务：

```
Task A: UART processing
Task B: Audio processing
Task C: Network processing
```

你必须实现 Scheduler，因为系统需要自主决定：

```
CPU ownership:

Time 0: Task A
Time 1: Task B
Time 2: Task A
Time 3: Task C
```

这需要：

- Runnable / Ready Queue。
- Priority Management。
- Context Switch。
- Tick / Preemption。
- Blocking / Wakeup。
- CPU Affinity。
- SMP Load Balancing。

但传统 OP-TEE 的情况不同。

一个 TA 通常不是像 Linux Process 那样自主地争取 CPU 时间，而是响应一个进入 TEE 的请求。

```
Linux Task A
    |
    | Invoke TA-1
    v
OP-TEE Trusted Thread #0

Linux Task B
    |
    | Invoke TA-2
    v
OP-TEE Trusted Thread #1
```

Linux Task A 和 Task B 何时运行，主要由 Linux Scheduler 决定。

Linux Task A 获得 CPU 并进入 OP-TEE，Trusted Thread #0 才获得继续执行的机会。

因此，一个重要结论是：

OP-TEE 需要 Thread Context Management，但不一定需要传统意义上的 Task Scheduling。

这两项职责在操作系统设计中应当严格区分。

## 10.1.3 为什么不能简单地在 TEE 中执行完所有工作再返回？

假设 TA 发起 Secure Storage 操作：

```
TA
 |
 | TEE_WriteObjectData()
 v
OP-TEE Secure Storage
 |
 | Need REE filesystem access
 v
Linux tee-supplicant
 |
 | Filesystem write()
 v
Return to OP-TEE
 |
 v
Continue TA
```

TEE 执行环境未必具有独立访问 Linux Filesystem 的能力。

因此 OP-TEE 需要临时退出 TEE，让 Linux 完成文件操作，然后继续执行原来的 TA。

假如没有 Trusted Thread Context，那么退出时的这些信息如何保存？

- 当前调用栈在哪里？
- 函数执行到哪一条指令？
- `s0`～`s11` 等寄存器保存在哪里？
- 正在操作哪个 TA Session？
- 当前 TA 的地址空间是哪一个？
- RPC 返回后如何继续原来的函数？

Trusted Thread 就是为了保存这样的连续执行状态。

## 10.1.4 为什么不能直接使用 Linux Task Context？

因为 Linux Task Context 属于 REE 的管理范围。

即使 Linux 负责决定调用线程何时执行，也不意味着 Linux 能够直接保存和恢复 OP-TEE 安全内存中的寄存器状态、内核栈和 TA 上下文。

安全侧状态不能因为调度的便利性而交给不可信的 Linux Kernel 管理。

因此可以把二者理解为：

```
Linux Scheduler
    |
    | Determines when REE caller executes
    v
REE/TEE Invocation
    |
    v
OP-TEE Trusted Thread Management
    |
    | Maintains secure execution continuity
    v
TA Execution
```

这个设计把普通系统调度与可信执行状态的管理分离开来。

### 10.1.5 这是否意味着 OP-TEE 完全没有并发和同步问题？

当然不是。

OP-TEE 仍然需要处理：

- 多个 Trusted Thread 同时运行。
- 多个 TA Session 共享一个 TA Instance。
- Mutex、Spinlock 和 Wait Queue。
- SMP 中的临界区。
- RPC 期间的 Thread Suspension。
- Shared Memory 的生命周期。
- 不同 TA 之间的访问隔离。

没有独立 Scheduler，不代表没有并发控制。

更准确地说，OP-TEE 将 Thread 的运行时机主要交给调用方所处的系统调度，而自己负责 Trusted Thread Context、同步和安全执行。

# 10.2 Trusted Thread 与 NuttX Task / pthread 的区别

这一节我们从操作系统实现角度进行比较。

## 10.2.1 三种模型

| 特性              | NuttX Task       | Linux pthread      | OP-TEE Trusted Thread    |
| --------------- | ---------------- | ------------------ | ------------------------ |
| 核心抽象            | Kernel Task      | 用户执行线程             | TEE 请求执行上下文              |
| 独立 Scheduler    | NuttX Scheduler  | Linux Scheduler    | 通常无                      |
| Ready Queue     | 有                | Kernel 中有          | 无传统任务 Ready Queue        |
| Stack           | 每任务              | 每线程                | 每 Trusted Thread         |
| Context Save    | 调度/Trap          | 调度/Trap            | TEE Exit、RPC、Interrupt 等 |
| Thread 创建       | 动态或预配置           | `pthread_create()` | 静态/配置的 Context Pool      |
| 生命周期            | Task 自主管理        | 线程函数生命周期           | 关联请求的执行与恢复               |
| SMP             | Scheduler 分配 CPU | Scheduler 分配 CPU   | 可在不同 hart 上恢复            |
| Blocking        | Scheduler 切换     | Scheduler 切换       | 根据具体等待/交互机制处理            |
| Security Domain | 内核/用户隔离          | 内核/用户隔离            | 在 TEE 安全域内运行             |

需要特别强调最后几项：

Trusted Thread 不等于 TA Instance。

同一个 Trusted Thread 可以在执行过程中调用多个 TA，也可能通过 Session Stack 进入其他可信服务。

反过来，同一个 TA Instance 也可能在其并发模型允许的情况下，由不同 Trusted Thread 执行不同的请求。

## 10.2.2 对照 NuttX TCB

在 NuttX 中，你很熟悉这样的模型：

```
/* Conceptual NuttX-style TCB */
struct tcb_s {
    /* Scheduler state */
    int task_state;
    int sched_priority;

    /* CPU execution context */
    void *xcp_regs;

    /* Stack management */
    void *stack_base_ptr;
    size_t adj_stack_size;

    /* Scheduling relationship */
    /* Ready/blocked queue linkage */
};
```

这是结构示意，并非对应某一 NuttX 版本的完整定义。

NuttX TCB 同时服务于两项职责：

1. 保存任务的执行上下文。
2. 使任务参与 Scheduler 的运行状态管理。

而 OP-TEE 的 `struct thread_ctx` 不需要承担完整的独立任务调度职责。

例如它没有一个用于 Linux 式 CFS 调度的虚拟运行时间字段，也没有普通 RTOS 所需要的任务优先级调度队列。

## 10.2.3 Trusted Thread 的特殊价值：Suspended Context

假设 Trusted Thread #2 正在执行：

```
TA
  -> TEE_WriteObjectData()
     -> tee_fs_write()
        -> thread_rpc()
```

此时 Linux 必须处理 RPC。

Trusted Thread #2 的执行状态可以变成：

```
ACTIVE -> SUSPENDED
```

与此同时，OP-TEE 不会丢弃它的 Kernel Stack。

之后 Linux 发起 RPC Resume 请求：

```
SUSPENDED -> ACTIVE
```

此时原来的调用栈被重新激活，执行过程能够从 RPC 函数之后继续。

从 C 代码视角看，这非常接近一次普通函数调用：

```
/* Teaching example */
TEE_Result secure_storage_write(...)
{
    prepare_rpc();

    result = request_ree_service();

    /* Execution resumes here after the RPC round trip */
    verify_rpc_result();

    return result;
}
```

但是在 `request_ree_service()` 内部，控制流实际上离开过 OP-TEE，Linux 甚至可能调度了其他用户进程。

对 OP-TEE 上层 C 代码隐藏跨安全域的执行中断，是 Trusted Thread 设计的重要价值。

# 10.3 Trusted Thread State Machine

现在开始进入真实 upstream 源码。

## 10.3.1 真实状态定义

首先阅读：

`core/include/kernel/thread_private.h`

当前代码定义了三个基础状态：

```
enum thread_state {
    THREAD_STATE_FREE,
    THREAD_STATE_SUSPENDED,
    THREAD_STATE_ACTIVE,
};
```

这三个状态非常少，却足以支持传统 OP-TEE 的 Trusted Thread 模型。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



注意：没有 `READY`、`RUNNABLE`、`SLEEPING` 等传统 Scheduler 状态。

原因正好对应我们刚刚讨论的运行模型。

## 10.3.2 状态转换图

逻辑状态机：新请求 FREE → ACTIVE；RPC 等需要退出时 ACTIVE → SUSPENDED；恢复后 SUSPENDED → ACTIVE；请求完成后 ACTIVE → FREE。

### 状态 A：FREE

表示这个 Trusted Thread Context 当前未被请求占用。

它通常可以由下一次新的 Standard Request 重新分配。

注意：

`FREE` 不一定表示 Thread Stack 内的数据已经被清零，也不意味着这份 Context 从物理内存中消失。

它表示一个可复用的 Thread Slot。

### 状态 B：ACTIVE

表示 Thread Context 已经被某个请求占用，正在参与安全侧执行。

例如：

```
Hart 0:
    thread_core_local[0].curr_thread = 2

threads[2].state = ACTIVE
```

此时 Thread #2 是 Hart 0 当前执行的 Trusted Thread。

一个重要不变量是：

同一个 ACTIVE Trusted Thread 不应该同时被两个 hart 执行。

因此 Thread Slot 的分配、状态转换和当前 hart 的绑定必须受到并发控制。

### 状态 C：SUSPENDED

表示 Thread 已经保存了继续执行所需的状态，目前没有在 TEE 内继续运行。

例如：

```
threads[2].state = SUSPENDED

Hart 0:
    curr_thread = INVALID
```

但 Thread #2 的执行上下文仍然被保留。

它的 Stack、寄存器现场、TA Session 信息和必要的地址空间描述并未因为离开 TEE 而自动销毁。

### 10.3.3 四条重要转换路径

| 转换                 | 触发原因                      | 相关函数                       |
| ------------------ | ------------------------- | -------------------------- |
| FREE → ACTIVE      | 新的标准请求                    | `thread_alloc_and_run()`   |
| ACTIVE → SUSPENDED | RPC / Foreign Interrupt 等 | `thread_state_suspend()`   |
| SUSPENDED → ACTIVE | RPC Resume                | `thread_resume_from_rpc()` |
| ACTIVE → FREE      | 请求完成                      | `thread_state_free()`      |

当前 RISC-V 的转换逻辑主要在 `thread_arch.c`，公共资源与线程池管理则在 `core/kernel/thread.c`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



## 10.3.4 为什么没有 SUSPENDED → FREE 的普通转换？

因为一个 Suspended Thread 往往还拥有尚未结束的执行过程。

例如它可能停在：

```
tee_fs_write()
  -> thread_rpc()
     [SUSPENDED]
```

如果直接释放它，未完成的 C 调用栈、RPC 状态、Session 引用和相关资源就可能失去管理。

因此正常生命周期是：

```
SUSPENDED
    |
    | Resume
    v
ACTIVE
    |
    | Complete / unwind
    v
FREE
```

异常取消或平台重置必须通过具备完整资源清理语义的专门路径处理，不能仅仅修改状态枚举。

## 10.3.5 Thread 数量和 CPU 数量不是一回事

一个非常重要的配置参数是：

`CFG_NUM_THREADS`

它指定传统静态配置下可以存在多少个 Trusted Thread Context。

而：

`CFG_TEE_CORE_NB_CORE`

描述的是 OP-TEE 支持的 CPU Core/Hart 数量上限。

这两个值不必相等。

例如：

```
SoC:
    16 harts

OP-TEE configuration:
    CFG_TEE_CORE_NB_CORE = 16
    CFG_NUM_THREADS      = 8
```

此时可以拥有 16 套 Per-Hart 基础环境，但同时最多只能有 8 个被分配的 Trusted Thread Context。

反过来，在单 hart 平台上也可以配置多个 Trusted Thread，使某个请求在 RPC 阶段 Suspended 时，其他请求仍然有机会进入 TEE。

这正是 Thread Context 与物理 CPU 并非一一绑定的体现。

实际并发上限还会受到 Hart 数、通信协议、平台隔离策略和服务锁的约束。

# 10.4 Per-Hart Entry Stack 与 Trusted Thread Stack

本节是本章最重要的基础之一。

要真正理解 Trusted Thread Context Switching，首先必须理解：

OP-TEE 不是只有一种 Stack，而是根据当前运行状态使用不同的 Stack。

## 10.4.1 为什么一个 Stack 不够？

考虑三种情况：

情况 A：Linux 刚刚进入 TEE。

此时尚未分配 Trusted Thread。

如果连 Trusted Thread 都没有，应该使用哪一个 Stack？

显然不能立即使用某个尚未分配的 Trusted Thread Stack。

情况 B：Trusted Thread 正在执行 TA。

此时需要保存持续存在的函数调用栈，例如：

```
TEE_InvokeCommand()
  -> TA_InvokeCommandEntryPoint()
     -> TEE_WriteObjectData()
        -> tee_fs_write()
           -> thread_rpc()
```

这个 Stack 必须在 RPC Suspend 期间保持有效。

情况 C：Trusted Thread 发生严重异常。

如果异常由 Stack Overflow 造成，原 Thread Stack 可能已经损坏。

异常处理不能继续无条件依赖这个 Stack。

因此 OP-TEE 需要不同用途的 Stack。

## 10.4.2 三种核心 Kernel Stack

Temporary Stack

Per-Hart

Thread Stack

Per-Thread

Abort Stack

Per-Hart

不同 Stack 分离入口处理、长期请求执行和异常恢复职责。

| Stack           | 所属对象                      | 主要职责                | 是否在 RPC 后保留     |
| --------------- | ------------------------- | ------------------- | --------------- |
| Temporary Stack | Hart                      | 入口、出口、快速处理、切换过渡     | 不是请求的持久 Stack   |
| Thread Stack    | Trusted Thread            | OP-TEE Core 的持续函数执行 | 是               |
| Abort Stack     | Hart                      | 异常处理、错误诊断           | 不作为请求恢复栈        |
| User TA Stack   | TA User Execution Context | U-mode TA 执行        | 由 TA Context 管理 |

第四种 User TA Stack 不是 OP-TEE Core Kernel Stack，但必须把它纳入整体模型。

## 10.4.3 实际源码：Stack 的数量关系

在：

`core/kernel/thread.c`

可以找到 `stack_tmp`、`stack_abt`、`stack_thread` 等资源的分配逻辑。

在静态、非 Pager 等相应配置下，资源数量关系可以概括为：

```
Temporary Stack Count
    = CFG_TEE_CORE_NB_CORE

Abort Stack Count
    = CFG_TEE_CORE_NB_CORE

Thread Stack Count
    = CFG_NUM_THREADS
```

如果启用了 Dynamic Configuration 或 Pager，实际分配方式会有所不同，但 Per-Hart / Per-Thread 的抽象区别不会因此消失。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



在当前 RISC-V `thread_private_arch.h` 中，还可以看到相应的 Stack Size 定义。默认宏值对应：

- Thread Stack：8192 bytes，调试栈检查配置下为 10240 bytes，另可能增加配置的额外空间。
- Abort Stack：4096 bytes。
- Temporary Stack：通常为 2048 bytes，部分编译配置使用更大的基数。

这里还可能有 Stack Canary、Alignment 和 Debug Extra Space，因此实际分配占用不应直接等同于上述有效栈大小。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 10.4.4 `thread_core_local`：每个 hart 的运行状态

RISC-V OP-TEE 有一个非常重要的结构：

`struct thread_core_local`

其核心信息可抽象为：

```
/* Simplified teaching model, not a verbatim source definition */
struct thread_core_local {
    uint32_t hart_id;
    uint32_t hart_index;

    void *tmp_stack_top;
    void *abort_stack_top;

    int curr_thread;
    uint32_t flags;

    /* Additional per-hart fields */
};
```

真实结构还包含异常入口使用的临时寄存器保存空间等字段。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



它解决的是一个核心问题：

当前 hart 正在执行哪个 Trusted Thread，以及在没有 Trusted Thread 时应当使用什么运行环境？

例如：

```
Hart 0:
    hart_index   = 0
    curr_thread  = 3
    tmp_stack    = TMP0

Hart 1:
    hart_index   = 1
    curr_thread  = INVALID
    tmp_stack    = TMP1

Hart 2:
    hart_index   = 2
    curr_thread  = 5
    tmp_stack    = TMP2
```

其中 Hart 1 没有 ACTIVE Trusted Thread，但仍然拥有自己的 Temporary Stack 和基础异常处理环境。

## 10.4.5 为什么 RISC-V 的 `tp` 寄存器非常重要？

你已经学习过 RISC-V `tp`（Thread Pointer）。

在普通 Linux User Space 中，`tp` 主要用于 TLS（Thread Local Storage）。

但在当前 RISC-V OP-TEE Kernel 实现中，S-mode Kernel 执行期间会使用 `tp` 指向当前 hart 的 `thread_core_local`。

可以理解为：

```
S-mode OP-TEE:

tp
 |
 v
thread_core_local[hart_index]
 |
 +-- hart_id
 +-- hart_index
 +-- curr_thread
 +-- tmp_stack_va_end
 +-- abt_stack_va_end
```

因此，汇编代码能够快速获得当前 hart 的数据。

在 `entry.S` 中，`set_tp` 宏负责根据 hart index 找到对应的 `thread_core_local`。

这也说明了一个架构级约束：

Kernel `tp` 是与当前 hart 绑定的，而 Trusted Thread Context 可能在不同 hart 之间迁移。

因此，不能简单地将旧 hart 的 Kernel `tp` 原样恢复到另一个 hart。

这个问题将在 10.6 中结合实际源码展开。

## 10.4.6 `sscratch` 如何帮助区分 User / Kernel Trap？

在 `thread_rv.S` 的 Trap Vector 中，当前实现使用一个非常巧妙的设计：

- 当 Kernel 正在执行时，`sscratch` 用于表示 Kernel Trap 场景，通常被设置为 0。
- 当准备进入 U-mode TA 时，`sscratch` 保存 Kernel 的 `thread_core_local` 指针。
- U-mode TA 自己使用 `tp` 作为用户线程指针。

Trap 入口通过 `csrrw` 交换 `tp` 与 `sscratch` 的内容。

以下是教学化简：

```
# RV64 S-mode trap entry concept
csrrw tp, sscratch, tp

# tp == 0: trap came from kernel
# tp != 0: tp now points to kernel per-hart data
```

其逻辑如下：

Kernel Trap

进入前：

`tp = core_local`

`sscratch = 0`

交换后：

`tp = 0`

`sscratch = core_local`

User Trap

进入前：

`tp = user TLS`

`sscratch = core_local`

交换后：

`tp = core_local`

`sscratch = user TLS`

对应 `thread_rv.S::thread_trap_vect()`；后续入口代码会恢复或保存相应的状态，不能只看这一个交换指令。

具体来说，Kernel Trap 的代码会再进行必要的交换，重新取得 Kernel `tp`。

User Trap 则获得可信的 Per-Hart 数据指针，进而切换到 Kernel Stack、保存 User `tp`，并建立可嵌套异常处理的 Kernel 状态。

这是一个非常漂亮的 RISC-V Trap Entry 设计实例。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 为什么这项设计值得深入？

因为 RISC-V 没有专门的 Kernel Stack Pointer Bank，也没有规定所有 S-mode Kernel 必须如何保存 User TLS。

内核需要通过 `sscratch`、`tp` 和手工编写的 Trap Entry Assembly，建立自己的执行约定。

OP-TEE 将这一约定与 Per-Hart 数据结构融合了起来。

# 10.5 Trusted Thread 的创建、获取与释放

这一节直接进入 RISC-V OP-TEE 的 C 实现。

## 10.5.1 Trusted Thread 是否通过 `malloc()` 动态创建？

通常不是。

在传统静态 Thread Pool 模式下，OP-TEE 预先建立多个 Trusted Thread Context：

```
threads[]

+--------------------+
| Thread Context #0  |
+--------------------+
| Thread Context #1  |
+--------------------+
| Thread Context #2  |
+--------------------+
| Thread Context #3  |
+--------------------+
```

Thread Pool 的大小由配置决定。

例如：

```
CFG_NUM_THREADS = 4
```

Thread Context 在请求之间被重复使用。

这与 `pthread_create()` 创建一个独立的、具有自身生命周期的用户线程完全不同。

## 10.5.2 新请求如何分配 Trusted Thread？

关键源码：

`core/arch/riscv/kernel/thread_arch.c`

函数：

`__thread_alloc_and_run()`

它的主要流程可以用下面的等价逻辑伪代码说明：

```
void allocate_and_run(request)
{
    local = thread_get_core_local();

    assert(local->curr_thread == INVALID);

    lock_global_thread_pool();

    id = find_first_free_thread();

    if (id >= 0)
        threads[id].state = ACTIVE;

    unlock_global_thread_pool();

    if (id < 0)
        return_thread_limit_error();

    local->curr_thread = id;

    init_register_context(&threads[id], request);

    prepare_fp_context();

    restore_arch_context(&threads[id].regs);

    /* Does not normally return */
}
```

注意，这不是直接复制 upstream C 代码，而是保留关键控制流的教学版本。

当前真实实现中，分配过程通过 `thread_lock_global()` 保护 Thread Pool；找到 FREE Slot 后设置为 ACTIVE，随后绑定当前 hart、初始化寄存器并调用 `thread_resume()` 进入新的执行现场。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 10.5.3 为什么必须使用 Global Lock？

假设有两个 hart 同时进入 OP-TEE：

```
Hart 0                     Hart 1

Find FREE Thread #2        Find FREE Thread #2
          |                           |
          v                           v
Mark ACTIVE                  Mark ACTIVE
          |                           |
          v                           v
Run Thread #2                Run Thread #2
```

如果没有同步保护，就可能发生同一个 Thread Context 被两个 hart 同时占用的情况。

结果是什么？

两个 hart 可能同时使用 Thread #2 的 Kernel Stack。

这不是普通的数据竞争，而可能直接导致：

- Stack Corruption。
- Return Address 被破坏。
- Trusted Thread Context 失效。
- TA 数据交叉污染。
- 严重情况下破坏安全隔离。

因此：

```
FREE -> ACTIVE
```

必须是一个受到并发保护的状态转换。

但需要注意，Global Lock 只保护 Thread Pool 的关键元数据操作，并不意味着整个 TA 执行过程都持有这个 Lock。

如果整个 TA Invoke 都持有 Global Lock，OP-TEE 就无法获得合理的 SMP 并行性。

## 10.5.4 `init_regs()` 做了什么？

这是理解新 Thread 如何启动的关键。

它不是从某个已经运行过的 Thread 中复制寄存器。

一个新 Thread 的入口上下文是人为构造出来的。

RISC-V 实现中的初始化包含：

| 字段        | 初始化目的                          |
| --------- | ------------------------------ |
| `epc`     | 新 Thread 的入口 PC                |
| `status`  | 设置返回执行的 Privilege 和状态          |
| `ie`      | 初始 Interrupt Enable 状态         |
| `sp`      | 指向该 Thread 的 Kernel Stack      |
| `gp`      | OP-TEE Kernel 的 Global Pointer |
| `tp`      | 当前 Hart 的 `thread_core_local`  |
| `a0`～`a7` | 请求入口参数                         |

源码中，`thread_alloc_and_run()` 使用的入口是 `thread_std_abi_entry`。`init_regs()` 则通过 `xstatus_for_xret()` 构造进入 S-mode 的状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



因此你可以把一个新 Thread 看成：

不是由 Scheduler 选中的已有 Task，而是由 OP-TEE 人工构造的可恢复 CPU Context。

这与 RTOS 中构造初始任务栈帧的原理有相似性。

区别是 OP-TEE 不需要在一个独立 Scheduler 的 Ready Queue 中调度它。

## 10.5.5 为什么 `thread_resume()` 是 `noreturn` 风格？

因为调用：

```
thread_resume(&threads[id].regs);
```

在语义上不是一次普通 C 函数调用。

该函数会恢复保存或构造好的 CPU 执行现场，最终通过 RISC-V 的异常返回指令转移到目标 PC。

它不会像普通函数那样：

```
call function
    |
    v
function body
    |
    v
return to caller
```

而是：

```
call thread_resume
    |
    v
Restore GPR / CSR
    |
    v
SRET / MRET
    |
    v
Execute at restored EPC
```

如果编译为 S-mode Kernel，通常会走 `sret`。

这种恢复原语本质上是一种控制流切换机制，而不是普通函数返回。

## 10.5.6 请求完成后如何释放 Thread？

正常完成路径中，RISC-V 的 `thread_std_abi_entry` 会切换回 Temporary Stack，然后调用：

`thread_state_free()`

这个函数负责：

1. 确认存在当前 ACTIVE Thread。
2. 完成相关架构状态恢复。
3. 获取 Thread Pool 的 Global Lock。
4. 将 Thread State 设置为 FREE。
5. 清理 Thread Flags。
6. 将当前 Hart 的 `curr_thread` 设置为 INVALID。
7. 释放 Global Lock。

源码可在 `thread_optee_abi_rv.S` 和 `thread_arch.c` 中对应找到。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



为什么要先切换回 Temporary Stack，再释放 Thread？

因为 不能在继续依赖某个 Thread Stack 的情况下，就宣布该 Thread 已经可以被其他 hart 重新分配。

否则可能出现非常危险的时间窗口：

```
Hart 0:
    Still running on Thread #2 stack

Thread #2:
    Already marked FREE

Hart 1:
    Allocate Thread #2
    Start using the same stack
```

因此，离开持久 Thread Stack 和释放 Thread Slot 之间的顺序，是一个必须验证的生命周期不变量。

# 10.6 Thread Suspend / Resume 的完整实现

这是本章的技术核心。

我们不仅要知道函数名，还要从 Stack、GPR、CSR、Memory Mapping 和 Hart Migration 五个维度理解一次 Suspend/Resume。

## 10.6.1 Suspend 的本质是什么？

Suspend 并不是简单地将状态设置为：

```
thread->state = SUSPENDED;
```

真正的 Suspend 必须保证：

在未来任意一个被允许的恢复时刻，CPU 可以从已经保存的安全执行现场继续运行，并且观察到与挂起前一致的执行状态。

这要求保留的状态至少包括：

- 恢复地址。
- 必要的通用寄存器。
- Stack Pointer。
- Privilege 状态。
- Interrupt 相关状态。
- 当前 TA 地址空间描述。
- FP/Vector 等扩展架构状态。
- Thread Session 和关联资源。

还需要一个重要条件：

保存上下文的内存和其执行依赖资源，在挂起期间必须保持有效。

## 10.6.2 Suspend 不等于 Save All Registers

这里有一个很有意思的优化。

假设我们是在一个普通 C 函数中主动发起 RPC。

根据 RISC-V ABI：

| 寄存器         | ABI 语义              |
| ----------- | ------------------- |
| `a0`～`a7`   | 参数/返回值，Caller-saved |
| `t0`～`t6`   | 临时寄存器，Caller-saved  |
| `s0`～`s11`  | Callee-saved        |
| `ra`        | Return Address      |
| `sp`        | Stack Pointer       |
| `gp` / `tp` | 特殊约定的全局/线程指针        |

对于这种主动挂起，编译器已经按照函数调用约定处理了 Caller-saved 寄存器的生命周期。

因此保存一个可从函数边界恢复的现场，不一定要像异步 Interrupt 一样，把所有 GPR 都保存下来。

相比之下，异步 Interrupt 可能发生在任意指令之间。

此时所有被中断代码依赖的寄存器都可能是活跃的。

所以：

Cooperative Suspend 和 Asynchronous Preemption 的寄存器保存需求不同。

这是后面解释 `thread_rpc_xstatus()` 和 Foreign Interrupt Handler 的关键。

## 10.6.3 RPC Suspend 的完整执行路径

在当前 RISC-V Legacy OP-TEE ABI 路径中，可以看到：

`thread_rpc_xstatus()`

其核心执行过程可以概括为：

```
OP-TEE Thread Stack
        |
        | RPC requested
        v
thread_rpc_xstatus()
        |
        | Save calling context
        | Save return address
        | Preserve callee-saved registers
        v
thread_get_ctx_regs()
        |
        | Prepare Temporary Stack
        v
thread_get_tmp_sp()
        |
        | Switch SP
        v
thread_state_suspend()
        |
        | ACTIVE -> SUSPENDED
        | Save status and resume PC
        | Preserve user mapping
        | Clear current thread
        v
Return to Untrusted Domain
```

这条路径可以在 `thread_optee_abi_rv.S` 中找到实际实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 第一阶段：保存 C 调用现场

在普通 RPC 调用路径中，保存内容包括：

- `ra`。
- `sp`。
- `gp`、`tp`。
- `s0`～`s11`。
- RPC 返回所需的局部状态。

这里的目标不是保存整个异常现场，而是构造一个能够在恢复后继续从 `thread_rpc_xstatus()` 返回的函数调用现场。

### 第二阶段：切换到 Temporary Stack

为什么？

因为即将将 Trusted Thread 标记为 SUSPENDED。

之后离开 OP-TEE 的动作不应该再依赖该 Thread 的 Kernel Stack 执行。

因此先获取当前 hart 的 Temporary Stack，再将 SP 切换过去。

### 第三阶段：调用 `thread_state_suspend()`

这个函数会完成状态转换。

它不是负责保存所有 GPR 的函数；部分寄存器现场已经由先前的汇编代码准备好。

实际源码中的 `thread_state_suspend()` 主要完成：

- Stack Canary 检查。
- 根据被挂起的执行模式处理 FP 状态。
- 设置恢复 `status` 和 `epc`。
- 更新 `THREAD_STATE_SUSPENDED`。
- 保存 TA User Mapping（如果存在）。
- 移除当前 hart 的 User Mapping。
- 清空当前 hart 的 `curr_thread`。

源码中可以看到保存地址空间状态的 `have_user_map` 和 `user_map` 字段。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 10.6.4 为什么挂起时必须处理 TA Mapping？

假设：

```
Thread #2
    |
    +-- TA-A
          |
          +-- User VA mapping A
```

Thread #2 正在处理 TA-A 的请求。

此时需要执行 RPC，离开当前安全执行环境。

如果只保存 GPR、SP 和 EPC，不保存 TA-A 的地址空间描述，那么恢复时可能会在错误的 User Mapping 下继续执行。

尤其在 SMP 和多个 TA 并发情况下，这可能直接造成地址空间错误。

因此：

```
Suspend:
    Save User Mapping A
    Deactivate User Mapping

Resume:
    Restore User Mapping A
    Restore Register Context
```

这里的 `user_map` 主要表示 OP-TEE 自身管理的 User TA 地址空间，并不等于 OpenSBI Domain 的 PMP 隔离配置。

二者属于不同层次的内存保护。

## 10.6.5 Thread 恢复过程

现在考虑 Linux 已完成 RPC，发起返回请求。

对应 RISC-V 实现：

`thread_resume_from_rpc()`

其逻辑可以概括为：

```
void resume_from_rpc(thread_id, result)
{
    local = thread_get_core_local();

    lock_global_thread_pool();

    if (!valid_id(thread_id) ||
        threads[thread_id].state != SUSPENDED) {
        unlock_global_thread_pool();
        return_error();
    }

    threads[thread_id].state = ACTIVE;

    unlock_global_thread_pool();

    local->curr_thread = thread_id;

    restore_user_mapping_if_needed();
    fix_arch_per_hart_context_if_needed();
    restore_rpc_return_arguments();
    prepare_fp_context();

    restore_arch_context(&threads[thread_id].regs);
}
```

这个模型与当前 `thread_resume_from_rpc()` 的主要控制流对应。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



注意一个并发细节：

多个 hart 可能几乎同时尝试恢复相同的 Thread ID。

只有一个 hart 应该成功完成：

```
SUSPENDED -> ACTIVE
```

其余请求不能再次恢复同一 Thread Context。

否则就会造成同一个 Kernel Stack 被多个 hart 同时使用。

## 10.6.6 最关键的汇编函数：`thread_resume()`

源码：

`core/arch/riscv/kernel/thread_rv.S`

在当前实现中，`thread_resume()` 主要恢复：

1. EPC。
2. Status。
3. Interrupt Enable。
4. `sscratch` 或对应模式的 Scratch 状态。
5. GPR。
6. SP。
7. 最终执行架构异常返回指令。

用一个教学化简的 RV64 S-mode 示例说明关键动作：

```
# a0 = address of saved context
mv      sp, a0

# Restore the saved program counter
ld      t0, EPC_OFFSET(sp)
csrw    sepc, t0

# Restore remaining status and GPRs
# ... omitted ...

# Restore original stack pointer last
ld      sp, SP_OFFSET(sp)

sret
```

这是示意代码，`EPC_OFFSET`、`SP_OFFSET` 为占位符；实际源码还需要正确处理 `sstatus.FS`、`sie`、`sscratch` 和所有必要的 GPR。

它想说明一个关键技术点：

`thread_resume()` 把保存的 Context 重新变成真实 CPU 状态，并通过 `sret` 在 EPC 所指定的位置恢复执行。

因此最终的 `sret` 并不是从 OP-TEE 返回 Linux。

它也可能只是从 OP-TEE Kernel 的恢复原语，返回到 OP-TEE Kernel 之前的执行位置。

这取决于保存的 EPC 和 Privilege Context。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 10.6.7 为什么必须最后恢复 SP？

因为在恢复其他寄存器时，代码还需要通过当前 SP 访问保存的 Context 数据。

假设提前恢复 SP：

```
SP = Original Thread Stack
```

后续的：

```
Load saved s0
Load saved s1
Load saved a0
...
```

就不能继续以原先的 Context 地址为基址访问。

因此汇编采用了一个很常见的恢复策略：

```
Use SP as Context Base
    |
    +-- Restore EPC
    +-- Restore STATUS
    +-- Restore GPRs
    |
    +-- Restore Original SP LAST
    |
    v
SRET
```

在 RISC-V 裸机、RTOS Scheduler、Trap Handler 中，这都是很值得掌握的汇编设计思想。

## 10.6.8 最重要的 SMP 问题：跨 hart 恢复

假设：

```
Time T0:
    Hart 0 executes Trusted Thread #2

Time T1:
    Thread #2 becomes SUSPENDED

Time T2:
    Linux resumes caller on Hart 3

Time T3:
    Hart 3 resumes Trusted Thread #2
```

这个场景要求 Trusted Thread Context 不能永远绑定 Hart 0。

但有一个问题。

前面已经知道：

```
Kernel tp = pointer to thread_core_local[current_hart]
```

如果把 Hart 0 保存的 `tp` 原封不动恢复到 Hart 3，那么：

```
Hart 3 executing

tp -> thread_core_local[0]
```

这显然是错误的。

它可能导致：

- 获得错误的 Hart Index。
- 读取错误的 `curr_thread`。
- 使用错误的 Temporary Stack。
- 操作其他 hart 的 Per-Hart State。
- 造成难以复现的 SMP 故障。

因此当前 RISC-V 实现中，`thread_resume_from_rpc()` 专门包含针对跨 hart 恢复的处理：

如果恢复目标是 OP-TEE Kernel Context，则将保存的 Kernel `tp` 调整为当前 hart 的 `tp`。

实际代码还通过 `is_user_mode()` 判断是否需要这样处理，因为 U-mode 的 `tp` 是 TA 的用户线程状态，不能随意改写。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这是本章最值得记住的一处 RISC-V 特有实现。

## 10.6.9 为什么不能直接修改所有恢复 Context 的 `tp`？

因为：

- S-mode Kernel `tp` 指向当前 hart 的 `thread_core_local`。
- U-mode TA `tp` 具有用户 TLS 语义。

对于 Kernel Context 的跨 hart 恢复，`tp` 必须反映新的 hart。

对于 TA User Context 的恢复，`tp` 必须保留 TA 自身的用户状态。

二者看起来使用同一个物理通用寄存器，但语义完全不同。

这也是为什么 Context Switching 不能仅仅理解为一个无差别的 `memcpy(registers)` 操作。

# 10.7 Trusted Thread Context 的数据结构

现在我们把整个模型落实到数据结构。

## 10.7.1 `struct thread_ctx`

在：

`core/include/kernel/thread_private.h`

它的关键成员可以整理为：

| 成员                     | 用途                           |
| ---------------------- | ---------------------------- |
| `regs`                 | 保存执行寄存器现场                    |
| `state`                | FREE、ACTIVE、SUSPENDED        |
| `stack_va_end`         | Thread Kernel Stack 边界       |
| `flags`                | 特定 Suspend/Resume 行为         |
| `user_map`             | TA User Mapping              |
| `have_user_map`        | 是否保存了 User Mapping           |
| `kern_sp`              | 从 User TA 执行进入内核时的 Kernel SP |
| `vfp_state`            | FP 相关上下文，取决于配置               |
| `rpc_arg` / `rpc_mobj` | RPC 参数及共享对象                  |
| `shm_cache`            | Shared Memory Cache          |
| `tsd`                  | Thread-Specific Data         |

其中 `kern_sp` 的定义包含 RV64。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这说明一个 Thread Context 不仅仅是一组寄存器。

它还包含继续执行一次可信请求所需要的内核资源和关联状态。

## 10.7.2 `struct thread_ctx_regs`

当前 RISC-V 定义中，`thread_ctx_regs` 保存的主要信息包括：

```
General-purpose registers:
    ra
    sp
    gp
    tp
    t0-t6
    s0-s11
    a0-a7

Control state:
    epc
    status
    ie
```

也就是说，它包含整数寄存器现场以及执行恢复所需的基本控制状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



但要小心：

`thread_ctx_regs` 不是完整的 RISC-V Hart Architectural State。

它不包含所有可能的 CSR，也不意味着 FP/Vector Context 已经被完整保存在其中。

这些额外状态需要通过各自的管理机制处理。

## 10.7.3 为什么要单独保存 EPC、STATUS 和 IE？

先回忆 RISC-V Trap 的硬件行为。

当发生 S-mode Trap 时，硬件会更新：

- `sepc`：异常返回地址。
- `scause`：异常或中断原因。
- `sstatus.SPP`：先前 Privilege。
- `sstatus.SPIE`：先前 Interrupt Enable。
- `sstatus.SIE`：进入 Trap 后相应的中断状态。

软件还可能需要单独管理：

\- `sie`：各类 Supervisor Interrupt Enable 位。

当 OP-TEE 要在未来恢复执行时，不能只恢复普通 GPR。

它还必须保证：

```
Execution resumes at correct PC
Privilege mode is correct
Interrupt state is correct
```

否则可能出现：

- 恢复到错误的指令。
- 返回到错误的 Privilege。
- 中断过早使能。
- 中断始终无法恢复。
- Trap 嵌套状态损坏。

尤其需要区分：

`SIE`（在 `sstatus` 中）和 `sie` CSR 并不是同一个字段。

一个控制 S-mode 的全局中断允许状态，另一个控制特定中断源的允许状态。

## 10.7.4 一个特别容易误解的问题：Suspend 后如何保存全部 Context？

答案是：不同执行路径采用不同的保存粒度。

| 触发路径                   | 保存特点                                             |
| ---------------------- | ------------------------------------------------ |
| 普通 RPC 主动退出            | 利用 RISC-V ABI 和函数调用边界                            |
| Asynchronous Interrupt | 必须保存被中断点所依赖的寄存器                                  |
| User TA Syscall        | 保存 User Calling Context，并建立 Kernel Syscall Frame |
| User TA Abort          | 保存异常现场，用于故障处理和必要的恢复                              |
| OpenSBI Domain Switch  | 由更高特权层处理跨 Domain 状态                              |

当前 `thread_rv.S` 中可以看到 `thread_scall_regs`、`thread_abort_regs` 和 `thread_ctx_regs` 的不同使用场景。

例如从 TA U-mode 发生 `ecall` 时，OP-TEE 会使用其内核栈建立 `thread_scall_regs`，然后进入 Syscall Handler；这与 RPC 期间保存 Thread Context 不是相同的路径。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



因此不能认为：

```
Every context switch = Save all registers into thread_ctx_regs
```

这种理解过于简单。

## 10.7.5 FP / Vector Context 的安全问题

RISC-V 的浮点和向量寄存器具有独立的架构状态。

例如：

```
FP:
    f0-f31
    fcsr

Vector:
    v0-v31
    vl
    vtype
    vstart
    vcsr-related state
```

其中 Vector 寄存器的实际状态大小还与 VLEN 相关。

如果：

```
TA-A uses FP registers
    |
    | Switch
    v
TA-B uses FP registers
```

但 OP-TEE 没有正确保存、恢复或清理 FP 状态，就可能发生跨 TA 的敏感数据泄露。

类似地，当 OP-TEE 与 Linux 在同一个 hart 上交替执行时，还必须考虑跨 Domain 的 FP/Vector 状态保护。

因此至少存在三个彼此关联、但责任不同的层次：

1. TA-A ↔ TA-B 的 User FP/Vector Context 管理。
2. OP-TEE Kernel 与 TA User 的 FP/Vector 使用管理。
3. Linux Domain ↔ OP-TEE Domain 的跨安全域扩展寄存器隔离。

当前 RISC-V OP-TEE 已有 `CFG_WITH_VFP` 相关 FP 状态管理代码。虽然名称保留了 `VFP` 这一历史术语，但 RISC-V 实现中的实际含义是管理该架构的 FP 执行状态。Vector 机制还需要结合目标分支的支持状态进行单独核查。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### Lazy Save 为什么更复杂？

Lazy Save 的目标是减少不必要的扩展寄存器保存与恢复。

例如某次 TA 调用完全没有使用 FP，那么执行一次完整 FP Context Save/Restore 可能是不必要的。

但这种优化意味着安全性依赖于：

- 硬件状态是否正确禁用。
- 第一次使用时是否能够正确捕获。
- 前一个所有者的状态是否被保存。
- 新所有者是否可能读到旧状态。
- Cross-Hart Resume 是否重新建立正确状态。

这也是为什么 FP/Vector Context Switching 的代码不能只从性能角度 Review。

正确性和机密性必须先于性能优化。

# 10.8 RPC 为什么会导致 Trusted Thread Suspend？

之前已经研究了 Suspend/Resume 的机械实现。

这一节要解决另一个问题：

为什么 RPC 必须将 Trusted Thread 的执行状态保存下来，而不是像普通函数一样直接调用 Linux 服务？

## 10.8.1 RPC 的架构意义

RPC = Remote Procedure Call。

在 OP-TEE 中，RPC 通常表示安全侧请求非安全侧提供某些服务，例如：

- REE Filesystem 访问。
- TA 文件加载。
- Shared Memory 分配和释放。
- REE Time 服务。
- 部分设备或系统服务。

OP-TEE 的安全模型并不要求这些服务提供者可信。

例如 REE Filesystem 可以保存经过 OP-TEE 加密和完整性保护的数据，但文件系统返回的原始数据仍需要按照安全协议验证。

因此 RPC 是一种跨越安全信任边界的通信机制。

## 10.8.2 一次完整的 Secure Storage RPC

考虑这样的 TA：

```
/* Teaching example: User TA */
TEE_Result TA_InvokeCommandEntryPoint(
    void *session_context,
    uint32_t command_id,
    TEE_Param params[4])
{
    TEE_ObjectHandle object = TEE_HANDLE_NULL;

    /* Open a persistent object */
    return TEE_OpenPersistentObject(
        TEE_STORAGE_PRIVATE,
        "demo", 4,
        TEE_DATA_FLAG_ACCESS_READ,
        &object);
}
```

这段代码只展示触发 Secure Storage 请求的示意，不是完整可运行 TA。

完整的执行链可以抽象为：

```
Linux Application
        |
        | TEEC_InvokeCommand()
        v
Linux TEE Driver
        |
        | Enter TEE
        v
OP-TEE Trusted Thread #2
        |
        | Execute User TA
        v
TEE_OpenPersistentObject()
        |
        | TEE syscall
        v
OP-TEE Secure Storage
        |
        | Need REE FS operation
        v
thread_rpc()
        |
        | Save Trusted Thread Context
        | Thread #2 -> SUSPENDED
        v
Return to REE
        |
        | Linux handles RPC
        v
tee-supplicant
        |
        | Filesystem operation
        v
RPC Response
        |
        | Resume Thread #2
        v
OP-TEE Secure Storage
        |
        | Validate and process result
        v
TEE_OpenPersistentObject() returns
        |
        v
TA continues
```

OP-TEE 官方架构文档也把 TA Loading、REE 时间请求和动态 Shared Memory 作为 RPC 服务的典型场景。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 10.8.3 为什么在 RPC 期间仍然保留 Thread Stack？

注意，`thread_rpc()` 不是将后续执行完全交给 Linux。

它的目标是暂时退出，并在之后继续原来的 C 调用链。

当 Thread #2 进入 SUSPENDED 时，仍然可能存在这样的调用栈：

```
High Address
+----------------------------------+
| OP-TEE Thread Stack #2           |
+----------------------------------+
| ...                              |
| tee_ta_invoke_command()          |
| tee_svc_storage_obj_open()       |
| tee_ree_fs_open()                |
| thread_rpc_cmd()                 |
| thread_rpc()                     |
+----------------------------------+
Low Address
```

这个示意并不是某个固定编译版本的完整 Backtrace，而是展示跨 RPC 保留调用栈的设计目的。

如果此时释放 Thread Stack，就相当于把未完成的函数调用链直接丢弃。

之后即使知道：

```
Saved EPC = rpc_return_label
```

也不足以安全地恢复执行。

因为原有 Stack Frame 中还包含：

- Local Variables。
- Saved Return Address。
- Function Arguments。
- 被编译器 Spill 到栈上的寄存器值。
- 函数间传递的对象引用。

所以：

Suspend 保存的不只是寄存器快照，还包括继续执行所依赖的持久调用栈和资源。

## 10.8.4 RPC 恢复为什么像一次正常函数返回？

当前 RISC-V 的 `thread_rpc_xstatus()` 中，关键思路是：

1. 保存函数返回所需的状态。
2. 将一个 RPC 返回后的汇编标签作为恢复地址。
3. 挂起当前 Trusted Thread。
4. 离开 TEE。
5. 在 RPC 完成后恢复寄存器现场。
6. 跳转到之前保存的 RPC 返回标签。
7. 将 RPC 结果写回参数数组。
8. 恢复栈和状态。
9. 通过普通 `ret` 返回上层 C 代码。

其中，实际源码使用 `.thread_rpc_return` 作为 RPC 返回后的继续执行位置。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这与常见的协程思想有一定相似性：

```
Execute
   |
   v
Suspend at continuation
   |
   | External work
   v
Resume at continuation
   |
   v
Execute next statement
```

但需要指出，OP-TEE 并不是基于普通语言级 coroutine 实现 Thread 管理，而是由特定汇编、内核上下文和跨安全域 ABI 共同完成这一机制。

## 10.8.5 RPC Resume 的身份和安全问题

现在提出一个 Staff-level 问题。

假设：

```
Thread #2 = SUSPENDED
```

Linux 发起一个 Resume 请求：

```
Resume(thread_id = 2)
```

为什么 OP-TEE 不能简单地认为这个请求一定合法？

因为 Linux 本身不在 TEE 的可信边界内。

一个恶意或被攻破的 Linux 可能尝试：

- 重复 Resume。
- 使用错误的 Thread ID。
- 对已经 FREE 的 Thread 进行 Resume。
- 提交伪造的 RPC 返回参数。
- 在多 hart 上并发提交同一 Resume。
- 在启用虚拟化时混淆不同 Client 的上下文。

因此必须分清两层校验：

第一层：Thread 状态合法性。

当前 RISC-V `thread_resume_from_rpc()` 会检查 Thread ID 范围以及 `SUSPENDED` 状态，并通过 Global Lock 保护状态转换。

第二层：请求来源与所属上下文的合法性。

这取决于具体的通信协议、虚拟化支持以及安全监控固件如何管理 Client 身份和 Resume 请求。

单靠一个整数 Thread ID 不能证明请求来源具有授权。

尤其当一个 SoC 支持多个不可信 Guest、多个 Domain 或异步 Message Channel 时，必须明确：

```
Which caller owns this suspended request?
```

以及：

```
Can another untrusted caller resume it?
```

这是 Platform Integration 必须审查的安全约束，而不是 `thread_ctx` 状态机本身能够独立解决的问题。

## 10.8.6 Foreign Interrupt 与 RPC 有什么区别？

这两个机制在某些退出行为上很相似，但触发原因不同。

| 项目             | RPC                | Foreign Interrupt |
| -------------- | ------------------ | ----------------- |
| 触发原因           | OP-TEE 主动需要 REE 服务 | 外部中断要求返回 REE 处理   |
| 触发位置           | 通常位于已知函数调用路径       | 可能是异步执行点          |
| GPR 保存         | 可以利用 ABI           | 需要完整保存被中断现场       |
| Trusted Thread | 通常 SUSPENDED       | 可能 SUSPENDED      |
| 返回 REE         | 是                  | 是                 |
| Resume         | RPC 完成后            | REE 中断处理及后续恢复后    |
| 返回参数           | RPC 服务结果           | 不应无条件信任外部返回寄存器    |

在 `thread_rv.S` 中可以看到 `thread_foreign_interrupt_handler` 使用 `THREAD_FLAGS_EXIT_ON_FOREIGN_INTR` 挂起 Thread。

而普通 RPC 使用另一组 Resume Flags。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



一个非常重要的安全细节是：

Foreign Interrupt Resume 不应把来自非可信侧的普通返回参数，当成原来被中断代码的寄存器状态覆盖进去。

当前 `thread_resume_from_rpc()` 使用 `THREAD_FLAGS_COPY_ARGS_ON_RETURN` 控制是否复制 RPC 返回寄存器。

因此 RPC 返回值更新和异步中断后的原始现场恢复是不同的语义。

## 10.8.7 Thread Pool Exhaustion 与死锁

考虑一个真实工业场景。

假设：

```
CFG_NUM_THREADS = 4
```

四个 Trusted Thread 都进入 Secure Storage RPC：

```
Thread #0 -> RPC -> SUSPENDED
Thread #1 -> RPC -> SUSPENDED
Thread #2 -> RPC -> SUSPENDED
Thread #3 -> RPC -> SUSPENDED
```

此时：

```
FREE Thread Count = 0
```

一个新的 Linux TEE 请求希望获取 Thread：

```
New Request -> No FREE Thread
```

对于当前 RISC-V OP-TEE ABI 实现，Thread 分配失败会沿标准请求入口返回 `OPTEE_ABI_RETURN_ETHREAD_LIMIT`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



但是：

四个 Thread 全部 SUSPENDED 并不必然构成死锁。

只要 Linux 侧还能完成 RPC 并恢复已有 Thread，系统就仍然能够前进。

真正危险的是形成循环等待。

例如：

```
All OP-TEE threads are waiting for RPC
            |
            v
Linux RPC worker needs a new TEE invocation
            |
            v
New TEE invocation needs a FREE thread
            |
            v
No free trusted thread
            |
            +------ circular wait ------+
```

Linux 的 OP-TEE Driver 已有 Call Queue、Waiter 和 System Thread 相关资源管理机制，说明 Thread Exhaustion 是需要在 REE/TEE 系统集成层面处理的实际问题。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



对于 Staff Engineer 来说，设计时必须评估：

- 是否存在 Recursive TEE Invocation？
- RPC Worker 是否依赖另一条 TEE 请求？
- 是否应当为系统服务保留资源？
- 请求被阻塞时，哪些锁仍然被持有？
- 是否存在等待资源的循环依赖？
- 超时与取消是否能保证资源最终释放？

不能仅仅通过无限增大 `CFG_NUM_THREADS` 来解决架构性的死锁。

# 10.9 Trusted Thread Context 与 OpenSBI Domain Context 的区别

这是整个 RISC-V TEE 课程中必须彻底分清的一组概念。

它们都涉及 Context Switching，但职责、管理者和安全边界完全不同。

## 10.9.1 三种经常被混淆的 Context Switch

在一个典型的 RISC-V TEE 系统中，至少可能存在三种切换：

```
Level 1:
    Linux Domain <-> OP-TEE Domain
    Managed by M-mode Security Firmware

Level 2:
    OP-TEE Trusted Thread #0 <-> #1
    Managed by OP-TEE Trusted Thread subsystem

Level 3:
    TA-A <-> TA-B
    Managed by OP-TEE TA / User Context subsystem
```

更准确地说，Level 2 的 Thread 切换通常是伴随 TEE 调用退出与重新进入而完成的，并不意味着 OP-TEE 内有一个像 Linux CFS 那样的调度器。

## 10.9.2 Domain Switch 的安全职责

假设：

```
Domain A:
    Linux
    S-mode

Domain B:
    OP-TEE
    S-mode
```

两个 Domain 都可以运行 S-mode 代码。

如果发生：

```
Linux Domain -> OP-TEE Domain
```

M-mode Security Firmware 需要保证：

- 切换执行环境。
- 建立新的物理内存访问权限。
- 防止旧 Domain 的硬件状态泄露到新 Domain。
- 保持 Domain 的 Memory Ownership。
- 处理必要的 Interrupt 和 CSR 状态。
- 确保返回时重新建立原 Domain 的安全边界。

这些职责主要由 OpenSBI Domain Context 和硬件隔离机制承担。

它不是 OP-TEE `thread_resume()` 可以替代的。

因为 OP-TEE 自己可能运行在 S-mode，无法凭空获得 M-mode 才能执行的保护配置权限。

## 10.9.3 Trusted Thread Switch 的职责

在 OP-TEE Domain 内部，Thread Switch 主要关注：

- 哪一个 Trusted Thread 执行当前请求？
- 当前 Thread 是否需要 Suspend？
- 如何保存 Kernel Stack？
- 如何恢复 GPR 和 CSR？
- 如何处理 TA Mapping？
- 如何保留 RPC 和 Session State？

它并不负责决定 Linux Domain 可以访问哪些物理内存。

这一点极其重要：

Trusted Thread Context Management 是 TEE 内核执行机制；Domain Context Management 是安全执行环境的底层隔离机制。

## 10.9.4 两层 Context Switch 的嵌套关系

下面用一次 RPC 的完整执行过程说明：

一次 RPC 的双层 Context Switch

Linux Domain

Linux Thread A enters TEE

OpenSBI Domain A → B

OP-TEE Domain

Allocate Trusted Thread #2

FREE → ACTIVE → TA execution

RPC requested

Suspend Trusted Thread #2

ACTIVE → SUSPENDED

OpenSBI Domain B → A

Linux Domain

Handle RPC · Scheduler may migrate caller

OpenSBI Domain A → B

OP-TEE Domain

Resume Trusted Thread #2

SUSPENDED → ACTIVE → Continue TA

这是一种使用 M-mode Domain Switch 的架构模型。具体的 RPMI/MPXY 或其他 Transport 会改变域间消息传递和进入方式，但不改变两种 Context 的职责划分。

观察上述流程：

第一次切换：

```
Linux -> OP-TEE
```

是 Domain Context Switch。

随后：

```
Thread #2 ACTIVE -> SUSPENDED
```

是 Trusted Thread State Transition。

然后：

```
OP-TEE -> Linux
```

又是 Domain Switch。

当 RPC 完成时：

```
Linux -> OP-TEE
```

先完成底层 Domain 切换，再由 OP-TEE 恢复原来的 Trusted Thread。

## 10.9.5 对比表：Domain Context vs Trusted Thread Context

| 维度              | OpenSBI Domain Context | OP-TEE Trusted Thread Context |
| --------------- | ---------------------- | ----------------------------- |
| 管理者             | M-mode Firmware        | OP-TEE Kernel                 |
| 核心职责            | 跨安全域隔离与执行切换            | 安全请求执行状态保存                    |
| 特权层次            | 主要在 M-mode             | OP-TEE S-mode                 |
| Context 范围      | Domain 执行及必要硬件状态       | Thread Stack、Regs、Session 等   |
| PMP / Firewall  | 必须考虑                   | 不直接承担底层隔离                     |
| User TA Mapping | 通常不管理具体 TA             | 需要保存和恢复                       |
| 生命周期            | Domain 生命周期和运行切换       | 请求及 Suspend/Resume 生命周期       |
| 可跨 hart         | 取决于 Domain 和 Hart 设计   | 支持满足约束的跨 hart 恢复              |
| 设计重点            | 安全不变量                  | 执行连续性与并发正确性                   |

### 特别重要的安全问题

如果 OpenSBI 的 Domain Switch 正确，但 OP-TEE 的 Trusted Thread Context Save/Restore 错误，会怎么样？

可能导致：

- TA 运行错误。
- Trusted Thread Stack 损坏。
- Session 错乱。
- TA-A 与 TA-B 之间的资源或数据泄露。
- OP-TEE Core Panic。

反过来，如果 OP-TEE Trusted Thread 实现完全正确，但 OpenSBI Domain Isolation 错误，会怎么样？

Linux 可能访问本应受到保护的 OP-TEE 内存。

这会从根本上破坏 TEE 安全边界。

因此，两套机制的正确性缺一不可，但不能互相替代。

# 10.10 Trusted Thread、TA Context 与 Linux Task 三者的关系

本节将完整执行模型串联起来。

## 10.10.1 三者不是一一对应关系

首先看三个概念：

Linux Task：由 Linux Kernel 管理和调度的普通线程。

Trusted Thread：OP-TEE 用于执行和保存安全侧请求的 Thread Context。

TA Context：OP-TEE 对某个 Trusted Application 的加载状态、地址空间、资源和运行环境的描述。

它们存在关联，但没有固定的一对一关系。

## 10.10.2 一个 Linux Task 可以多次使用不同 Trusted Thread

例如：

```
Linux Task A
    |
    +-- Invoke #1
    |      |
    |      +-- Trusted Thread #0
    |
    +-- Invoke #2
    |      |
    |      +-- Trusted Thread #3
    |
    +-- Invoke #3
           |
           +-- Trusted Thread #1
```

在每次独立的请求完成后，Thread Slot 都可以被复用。

但如果某次请求在 RPC 时被挂起，它的 Resume 必须能够找回那一次具体调用所关联的 SUSPENDED Thread。

因此需要区分：

- 新请求的 Thread Allocation。
- 未完成请求的 Thread Resume。

不能把它们都处理为「随便找一个 FREE Thread」。

## 10.10.3 一个 Trusted Thread 可以调用多个 TA

假设：

```
Linux Task A
    |
    v
Trusted Thread #2
    |
    v
TA-A
    |
    | TA_OpenTASession()
    v
TA-B
    |
    | Return
    v
TA-A
```

这个过程中可能仍然使用同一个 Trusted Thread。

但不同 TA 的 Session、User Address Space 和安全资源需要由 OP-TEE 正确管理。

因此：

Trusted Thread Context 不等于 TA Context。

Thread 关注的是执行连续性；TA Context 关注的是 Trusted Application 的身份、状态及资源。

## 10.10.4 同一个 TA 可以由多个 Trusted Thread 调用吗？

可以，但实际并发模型取决于 TA 类型和属性。

对于普通 GP User TA，需要考虑：

- Single Instance。
- Multi Session。
- Keep Alive。
- 是否允许并发执行。
- TA 内部是否具有同步保护。

例如：

```
Linux Task A -> Trusted Thread #0 -> TA-X
Linux Task B -> Trusted Thread #1 -> TA-X
```

两条调用路径可能关联同一个 TA 实例，也可能对应不同实例，具体取决于 TA 配置和管理策略。

即使存在多个 Session，也不应自动推断 TA 的代码能够并发执行。

TA Manager 可能通过 Lock/Mutex 对调用进行串行化。

## 10.10.5 多个 Trusted Thread 能否同时运行在多个 hart？

可以，在平台和运行条件允许时。

例如：

```
Hart 0:
    Trusted Thread #0 -> TA-A

Hart 1:
    Trusted Thread #1 -> TA-B

Hart 2:
    Trusted Thread #2 -> PTA

Hart 3:
    Temporary Stack / No active thread
```

但必须保证：

1. 每个 ACTIVE Trusted Thread 只有一个执行所有者。
2. Thread Context 的分配与释放同步正确。
3. TA Address Space 的切换正确。
4. Per-Hart `tp` 正确。
5. 内存可见性和必要的 TLB Synchronization 正确。
6. 共享资源有适当的 Lock。
7. FP/Vector Context 不交叉泄露。

## 10.10.6 将所有层次组合起来

考虑一个 16-hart RISC-V SoC：

```
Physical SoC
    |
    +-- Hart 0
    +-- Hart 1
    +-- ...
    +-- Hart 15
    |
    v
OpenSBI / M-mode Isolation
    |
    +-- Linux Domain
    |
    +-- OP-TEE Domain
            |
            +-- Per-Hart Core Local
            |      |
            |      +-- Current Thread ID
            |      +-- Temporary Stack
            |      +-- Abort Stack
            |
            +-- Trusted Thread Pool
            |      |
            |      +-- Thread #0
            |      +-- Thread #1
            |      +-- ...
            |
            +-- TA Context Manager
                   |
                   +-- TA-A
                   +-- TA-B
                   +-- PTA
```

这里至少有四种不同的「所有权」：

| 资源                     | 主要所有者            |
| ---------------------- | ---------------- |
| Hart 的硬件执行资源           | 受 M-mode 固件和平台控制 |
| Per-Hart Local State   | 对应 hart          |
| Trusted Thread Context | 当前执行请求           |
| TA Context             | 对应 TA 实例和安全会话管理  |

把这些所有权关系区分清楚，后续设计 SMP、Interrupt Routing、Suspend/Resume 才不会陷入混乱。

# 十一、综合场景：从 Linux 调用到跨 hart RPC 恢复

现在将整章内容组织成一个工业场景。

## 11.1 场景设定

假设某 RISC-V SoC 有：

```
8 harts
OpenSBI M-mode
Linux S-mode Domain
OP-TEE S-mode Domain

CFG_TEE_CORE_NB_CORE = 8
CFG_NUM_THREADS      = 4
```

Linux 应用发起一个 Secure Storage 请求。

第一次调用发生在 Hart 1。

之后由于 Linux Scheduler 的调度，RPC Resume 发生在 Hart 5。

## 11.2 时间线

| 时刻 | Hart | Domain | Trusted Thread | 事件                   |
| -- | ---- | ------ | -------------- | -------------------- |
| T0 | 1    | Linux  | 无              | CA 发起 Invoke         |
| T1 | 1    | OP-TEE | #2 ACTIVE      | 分配 Thread #2         |
| T2 | 1    | OP-TEE | #2 ACTIVE      | TA 进入 Secure Storage |
| T3 | 1    | OP-TEE | #2 SUSPENDED   | 触发 RPC               |
| T4 | 1    | Linux  | 无              | 处理 RPC               |
| T5 | 5    | Linux  | 无              | Linux 调度了调用线程        |
| T6 | 5    | OP-TEE | #2 ACTIVE      | 恢复 Thread #2         |
| T7 | 5    | OP-TEE | #2 ACTIVE      | TA 继续执行              |
| T8 | 5    | OP-TEE | #2 FREE        | 请求完成                 |
| T9 | 5    | Linux  | 无              | 返回 CA                |

这是一个需要实际平台支持对应 hart 切换和 Resume 语义的设计场景。

## 11.3 T3：Suspend 时需要保存什么？

至少需要分析：

Thread execution state

- GPR。
- Resume PC。
- Status / Interrupt State。
- Kernel Stack。

TA state

- TA Address Space。
- Session Stack。
- FP/Vector Context。
- 必要的资源引用。

Platform state

- 当前 Domain。
- Hart-local State。
- Domain Isolation 配置。

其中 Platform State 不应错误地全部保存在 Trusted Thread 中。

它由更底层的 Domain 管理机制负责。

## 11.4 T6：跨 hart Resume 时必须完成什么？

恢复过程至少需要满足：

```
OpenSBI:
    Ensure Hart 5 enters correct domain
    Restore domain isolation
    Restore required domain CPU state
              |
              v
OP-TEE:
    Validate thread resume
    Set Thread #2 ACTIVE
    Bind it to Hart 5
    Restore TA mapping
    Rebind kernel tp to Hart 5
    Restore FP state as required
    Restore GPR/CSR
    Resume execution
```

如果任何一步违反安全或执行不变量，都可能造成错误。

特别要注意：OP-TEE 的 `thread_resume()` 不负责替 OpenSBI 完成 M-mode 的硬件隔离配置。

# 十二、源码阅读与 QEMU/GDB 实验

这一节提供可操作的源码研究方法。

## 12.1 推荐源码阅读顺序

建议按以下顺序逐个函数跟踪：

```
core/include/kernel/
    thread.h
    thread_private.h

core/kernel/
    thread.c

core/arch/riscv/include/kernel/
    thread_arch.h
    thread_private_arch.h

core/arch/riscv/kernel/
    thread_arch.c
    thread_rv.S
    thread_optee_abi.c
    thread_optee_abi_rv.S
```

具体阅读顺序：

1. `thread_init_threads()` / `thread_init_thread_core_local()`。
2. `thread_alloc_and_run()`。
3. `init_regs()`。
4. `thread_resume()`。
5. `thread_rpc_xstatus()`。
6. `thread_state_suspend()`。
7. `thread_resume_from_rpc()`。
8. `thread_state_free()`。

每研究一个函数，应明确它的输入、输出、当前 Stack、当前 hart、`tp`、`sscratch`、`status` 和 Thread State。

## 12.2 实验一：验证 Thread Pool State Machine

实验目标：

验证一次新请求从 FREE 到 ACTIVE，再到 FREE 的状态变化。

如果 QEMU 环境允许，可以在 GDB 中设置断点：

```
break thread_alloc_and_run
break thread_state_free
break thread_state_suspend
break thread_resume_from_rpc
```

部分符号的实际名称、调用路径和断点命中情况会受到编译优化、链接以及所用 RISC-V Transport 影响。

在断点命中时检查：

```
p thread_count
p thread_core_count
p threads[0].state
p threads[1].state
p threads[2].state
```

并在适当的 Thread Context 中检查：

```
p *thread_get_core_local()
```

建议记录一张表：

| Breakpoint | Hart | Thread ID | State Before | State After | SP |
| ---------- | ---- | --------- | ------------ | ----------- | -- |
| Allocate   | ?    | ?         | FREE         | ACTIVE      | ?  |
| Suspend    | ?    | ?         | ACTIVE       | SUSPENDED   | ?  |
| Resume     | ?    | ?         | SUSPENDED    | ACTIVE      | ?  |
| Free       | ?    | ?         | ACTIVE       | FREE        | ?  |

这样才能把抽象状态机与真实运行行为建立对应关系。

## 12.3 实验二：汇编级跟踪 `thread_resume()`

在 GDB 中：

```
disassemble thread_resume
```

然后跟踪执行，重点记录：

```
info registers
p/x $sp
p/x $tp
p/x $sepc
p/x $sstatus
p/x $sscratch
```

还需要观察 `sie` 等相关 CSR，具体 GDB 寄存器名称和访问支持取决于目标及版本。

建议回答：

1. 恢复前 SP 指向什么？
2. 为什么将 Context 指针作为临时访问基址？
3. EPC 从哪里加载？
4. `sscratch` 为什么根据返回 U-mode 或 Kernel Mode 不同而设置？
5. 为什么 SP 最后恢复？
6. `sret` 执行后返回到哪一条指令？

## 12.4 实验三：跨 hart RPC Resume

这是我最建议完成的实验。

配置至少两个 hart。

测试过程：

1. 让 Hart 0 进入 OP-TEE。
2. 触发一次可控的 RPC。
3. 记录 Thread ID、Stack、`tp`、Hart Index。
4. 尝试让 Linux 侧后续 Resume 运行在 Hart 1。
5. 观察恢复前后 Kernel `tp` 是否对应当前 Hart。
6. 观察 User TA Context 是否保持正确。

注意 Linux CPU Affinity 可能影响调用线程被调度到哪个 hart，但不保证实际会发生所需的迁移；具体仍要通过日志和断点证明。

如果当前平台无法支持这个迁移场景，可以先设计一个带日志的测试补丁验证多 hart 调用，再评估完整迁移测试的前置条件。

## 12.5 实验四：Thread Exhaustion / RPC Deadlock

测试设计：

```
CFG_NUM_THREADS = 2
```

设计两个并发的 TA 请求，使它们进入可控 RPC 阻塞，再发起第三个请求。

观察：

- 是否返回 Thread Limit。
- Linux Driver 是否等待或重试。
- RPC 完成后是否能够恢复旧 Thread。
- 是否存在长期无法释放的 Thread。
- 是否存在递归 TEE 请求形成的循环等待。

这个实验非常适合最终沉淀为 upstream Regression Test。

## 12.6 实验五：Context Corruption Fault Injection

在可恢复的隔离测试环境中，通过调试或测试补丁人为引入下列错误，观察结果：

- 保存错误的 EPC。
- 不正确地恢复 `tp`。
- 提前释放 Thread Slot。
- 错误地恢复 TA User Mapping。
- 不正确地恢复 FP Context。

每个 Fault Injection 都应配置独立测试、预期行为和恢复机制，不应在实际生产平台中直接尝试。

目标不仅是观察 Crash，更是判断：

这种错误会停留在正确性问题，还是可能破坏 TEE 的安全隔离？

# 十三、Staff Engineer 级面试题与工业案例

以下题目按 Architecture、Kernel Implementation、SMP、Security 和 System Debugging 五个方向组织。

每道题附有参考分析框架，便于在同一章中完成自测。

## Q1. 为什么 OP-TEE 没有传统 Scheduler，但又存在多个 Trusted Thread？

Architecture · 中高难度

场景： 一个团队计划把现有的 OP-TEE Trusted Thread 机制重构为完整的优先级抢占式 Scheduler，声称这样可以显著提高 TA 并发性能。

你作为 Staff Engineer，如何评估这个提议？

参考解析：

首先需要确认系统瓶颈到底是什么。

传统 OP-TEE 的 Trusted Thread 是请求驱动的执行上下文，主要跟随 REE 调用线程的调度进度。引入独立 Scheduler 会改变原有的执行与等待模型，并增加新的复杂度。

需要评估：

- OP-TEE 是否存在可以独立运行的任务负载？
- CPU 利用率低是由于 Trusted Thread 数量、TA 锁，还是 REE 调度造成？
- 是否有需要抢占的长时间安全侧任务？
- 是否会引入跨 hart 调度、优先级反转和异步抢占的安全问题？
- 是否增加 TCB 和攻击面？

如果仅仅是 Thread Pool Exhaustion，正确的方向可能是改进 RPC 流程、Call Queue、服务并发模型，而不是增加一个完整 Scheduler。

Staff-level 核心：不要在没有量化瓶颈与明确需求的情况下，改变系统的执行模型。

## Q2. 一个 Trusted Thread 能否从 Hart 0 迁移到 Hart 3？

SMP / Context Switch · 高难度

场景： OP-TEE 在 Hart 0 上进入 RPC Suspension，随后 Linux 在 Hart 3 上恢复调用。

请给出完整的迁移正确性条件。

参考解析：

可以在架构允许且实现支持的情况下跨 hart 恢复，但必须满足：

1. 原 Thread 已经进入 SUSPENDED，且不存在另一 hart 同时执行它。
2. 恢复时通过同步机制原子地将它转为 ACTIVE。
3. Kernel `tp` 必须指向 Hart 3 的 `thread_core_local`。
4. 恢复正确的 Kernel Stack、EPC、GPR 和 CSR。
5. 恢复正确的 TA User Mapping。
6. 确保 FP/Vector 等扩展状态得到正确处理。
7. 满足共享内存、Cache 和地址转换同步要求。
8. 平台 Domain/Hart 资源分配允许当前 hart 执行目标 TEE Domain。

最容易漏掉的是第 3 点：Kernel `tp` 不是普通的线程 TLS，而是 Per-Hart 运行环境指针。

## Q3. 为什么 RPC Suspend 不需要像异步中断一样无条件保存所有 GPR？

RISC-V Assembly / ABI · 高难度

场景： 一位工程师发现 `thread_rpc_xstatus()` 保存寄存器的过程与异步 Interrupt Handler 不同，认为这是一个 Bug。

如何判断？

参考解析：

要区分调用边界保存和任意指令位置的异步抢占。

在标准 RISC-V ABI 中，普通函数调用由 Caller/Callee 协议约束寄存器活跃状态。

主动 RPC 退出发生在已知的函数调用路径，可以利用已有 ABI 约定保存恢复所需的状态。

而中断可发生在任意执行位置，因此需要保留被中断代码所依赖的全部机器现场，不能假设 Caller-saved 寄存器已经无用。

判断是否存在 Bug，需要检查实际恢复点、保存寄存器集合、编译器 ABI 和执行状态。

## Q4. 为什么 Trusted Thread 释放前必须离开它自己的 Kernel Stack？

Concurrency / Lifetime · 高难度

场景： 团队为了简化代码，把 `thread_state_free()` 提前到 Thread Stack 上执行，之后再切换到 Temporary Stack。

为什么危险？

参考解析：

这可能造成 Use-after-release 型的 Stack 复用问题。

如果 Thread Slot 已标记 FREE，另一个 hart 可以分配它并开始使用对应 Stack。

而原 hart 仍可能在该 Stack 上运行函数尾部代码、访问局部变量或 Return Address。

因此应确保：

- 原执行路径已经不再依赖这份 Stack。
- Thread Slot 的可复用状态只在安全时刻发布。
- 释放状态受到必要的同步保护。

这是典型的资源生命周期与并发发布顺序问题。

## Q5. 如果两个 hart 同时 Resume 同一个 SUSPENDED Thread，会发生什么？

SMP / Atomicity · 高难度

场景： Linux 中存在一个错误，导致两个 CPU 并发提交相同 Thread ID 的 Resume 请求。

你如何设计 OP-TEE 的防御机制？

参考解析：

核心是不允许同一个 Thread Context 同时存在两个执行所有者。

应在锁保护下验证：

```
Thread ID is valid
Thread State == SUSPENDED
```

并完成：

```
SUSPENDED -> ACTIVE
```

状态转换成功之后才允许继续恢复执行。

第二个请求应当失败，而不能重复执行。

还需要评估 Thread ID 复用问题：旧请求的延迟 Resume 是否可能错误地指向后来复用的 Thread Slot？

如果跨域协议存在此风险，需要在 ABI 层设计足够的请求关联和授权机制，而不只是依赖 Thread Index。

## Q6. 一个 Trusted Thread 在 RPC 期间，为什么不应继续保留 TA User Mapping 为当前 Hart 的活动地址空间？

MMU / Isolation · 高难度

场景： 为了降低 Resume Latency，一位工程师建议 RPC Suspend 后不撤销当前 TA 的 User Mapping。

你如何评估？

参考解析：

需要考虑当前 Hart 之后可能运行什么代码。

在 Suspended Thread 离开后，Hart 可能处理另一个 TEE 请求，或者准备执行不同 TA。

如果遗留了旧 TA 的活动地址空间，可能导致：

- 错误地址转换。
- TA Context 混淆。
- 旧映射中的内存意外可访问。
- ASID/TLB 语义错误。
- Security Isolation Failure。

因此必须将 Mapping 的生命周期与 Thread 执行状态正确关联。

即使存在性能优化，也必须证明不同 TA、不同 Thread、不同 Hart 之间不会发生权限残留。

## Q7. Linux Domain 与 OP-TEE Domain 都运行在 S-mode，Trusted Thread 能否保证 Linux 不能访问 OP-TEE 内存？

Security Architecture · 高难度

参考解析：

不能。

Trusted Thread 负责 OP-TEE 内部的执行上下文管理。

它不是跨 Domain 的底层隔离机制。

Linux 与 OP-TEE 的物理内存隔离必须由更高特权层控制的 PMP/平台硬件隔离配置，以及相应的安全固件共同保证。

如果 OpenSBI Domain Switch 没有正确维护这些保护，Linux 仍可能突破隔离边界。

因此需要把：

- OP-TEE Thread Correctness。
- OP-TEE User TA Isolation。
- OpenSBI Domain Isolation。
- SoC DMA / Peripheral Isolation。

分别验证，不能把这些问题混为一谈。

## Q8. 为什么 OP-TEE 的 Kernel `tp` 与 TA User `tp` 需要不同的管理策略？

Assembly / Privilege · 高难度

场景： 团队统一了 Context Restore 代码，不区分 U-mode 和 S-mode，恢复时总是把 `tp` 改写为当前 Hart 的 Per-Hart 指针。

结果部分 TA TLS 操作异常。

为什么？

参考解析：

RISC-V 的 `tp` 是通用寄存器，其 ABI 语义由执行环境决定。

在当前 OP-TEE RISC-V Kernel 中：

```
S-mode Kernel tp:
    thread_core_local pointer
```

而在 User TA 中：

```
U-mode tp:
    User TLS pointer
```

如果恢复 U-mode 时将 `tp` 改成 Kernel Per-Hart Pointer，就破坏了用户 TLS。

反过来，如果 S-mode Context 跨 hart 恢复时保留旧 Kernel `tp`，则会错误访问旧 Hart 的 Per-Hart State。

正确实现需要根据被恢复的执行模式选择策略。

## Q9. FP Lazy Save 如何引入跨 TA 数据泄露风险？

Security / Architecture Extension · 高难度

场景： TA-A 在 FP 寄存器中处理敏感数据，随后 TA-B 启动。

由于性能优化，系统选择 Lazy FP Context Switching。

TA-B 第一次使用 FP 时可能发生什么风险？

参考解析：

如果 Lazy Context Management 不正确，TA-B 可能观察到 TA-A 遗留的 FP 寄存器内容。

必须保证：

- 正确管理 FS 状态。
- 新 TA 不能直接访问旧 TA 的 FP 状态。
- 必要时保存旧所有者的寄存器。
- 新 Context 首次使用时初始化或恢复正确状态。
- Kernel 与 User FP 使用路径不会破坏所有权。
- 跨 hart 恢复不能依赖错误的 Hart-local FP 状态。

Vector Context 需要采用类似但更复杂的策略，因为 Vector Register File 可能非常大。

## Q10. `CFG_NUM_THREADS = 4`，四个 Thread 都 SUSPENDED，系统一定死锁吗？

Resource Management · 中高难度

参考解析：

不一定。

SUSPENDED Thread 不需要继续占用当前 hart 的执行时间，但仍然占用 Thread Slot。

如果 REE 能够独立完成 RPC 并发起 Resume，那么已有 Thread 仍然可以继续运行并最终释放资源。

真正的死锁需要存在循环等待。

例如：

```
TEE Thread waits for REE RPC
       |
       v
REE RPC Worker waits for a new TEE Thread
       |
       v
All TEE Threads are suspended
```

解决方向应包括 Call Queue、System Thread 资源管理、RPC 依赖分析、超时和取消语义，而不只是增大 Thread Pool。

## Q11. 为什么同一个 TA Instance 不一定能够被多个 Trusted Thread 并发执行？

TA Lifecycle / Concurrency · 中高难度

参考解析：

Trusted Thread 描述的是请求执行上下文。

TA Instance 描述的是 Trusted Application 的加载状态和相关资源。

同一个 TA Instance 可能被多个 Session 使用，但其并发行为依赖 TA 属性和 OP-TEE 的 TA Manager 策略。

例如某些 TA 可以被多个客户端共享同一个实例，但其入口执行仍可能被串行化。

必须区分：

```
Multiple sessions
```

和：

```
Concurrent execution
```

二者并非同一概念。

如果错误地假定同一 TA 必然能够并发运行，可能导致数据竞争和资源一致性问题。

## Q12. 生产 SoC 上，偶发出现 RPC Resume 后 `sscratch` 或 `tp` 错误，你会如何定位？

Industrial Debugging · 高难度

场景： 8-hart RISC-V SoC，在单核测试下工作正常，但长时间 SMP Stress Test 后偶尔发生 OP-TEE Panic。

Panic 前经常经历 RPC Resume。

参考解析：

首先建立问题假设：

- 跨 hart 恢复错误。
- Per-Hart Context 指针错误。
- `sscratch` 与 User/Kernel Trap 约定被破坏。
- 异步 Interrupt 发生在未正确保护的切换窗口。
- Stack Corruption。
- FP Context 或 TLB 问题。

然后建立最小 Trace：

```
Timestamp
Hart ID
Thread ID
Thread State
Domain
SP
TP
SSCRATCH
SEPC
SSTATUS
SATP
```

重点分析：

1. Suspend 发生于哪个 hart？
2. Resume 发生于哪个 hart？
3. 是否正确修改 Kernel `tp`？
4. `sscratch` 是否与当前 Privilege 状态一致？
5. 是否发生递归 Trap？
6. 原 Thread Stack 是否仍然完整？
7. 是否有两个 hart 同时执行同一 Thread？

最后通过限制单核、禁止迁移、控制 RPC 时机、增加异常注入逐步缩小问题范围。

Staff Engineer 的关键不是第一时间猜出哪条指令错了，而是设计出能够把多个竞争性假设逐一排除的验证方法。

## Q13. Domain Switch 和 Trusted Thread Switch 都涉及 CSR 保存，能否统一成一个通用 Context Switch Library？

Architecture Design / Code Review · 高难度

参考解析：

可以考虑提取不涉及安全边界差异的低级辅助函数，但不能简单地把两个机制合并。

因为二者的安全职责不同。

OpenSBI Domain Switch：

- 在 M-mode 中管理隔离。
- 维护跨 Domain 的安全状态。
- 可能需要管理 PMP 和其他硬件资源。

OP-TEE Trusted Thread Switch：

- 在 TEE 内保存请求执行状态。
- 管理 TA User Mapping。
- 管理 Thread Stack、RPC、Session。

二者保存的状态集合可能重叠，但不变量不同。

一个好的设计应当共享真正可复用的机制，而不是抹平架构层次。

## Q14. 如果 OP-TEE 在 Suspend 期间仍持有 Spinlock，会有哪些风险？

Concurrency / Deadlock · 高难度

参考解析：

首先考虑 Spinlock 的所有权和被保护的数据范围。

如果一个 Thread 持有 Spinlock 后离开 TEE，它可能需要等待 REE 完成 RPC 才能重新恢复执行。

与此同时，另一个 Trusted Thread 可能尝试获取同一把 Lock。

如果该 Lock 的释放依赖于已挂起 Thread，而它的恢复又依赖另一个被阻塞的执行路径，就可能形成死锁。

此外，跨 hart 恢复还可能与基于 CPU 的锁所有权假设冲突。

因此，设计时需要检查：

- 在进入 RPC/Suspend 前是否仍有 Spinlock。
- Lock 是否可跨 Thread Suspension 保留。
- 是否存在锁顺序反转。
- 是否依赖当前 Hart 的不可迁移状态。
- 是否需要将长时间等待移到 Mutex 或其他可睡眠同步模型。

当前 RISC-V 的 `thread_set_exceptions()` 也有在允许 Foreign Interrupt 前检查 Spinlock 持有状态的防御逻辑。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## Q15. 如何设计一个足以进入 OP-TEE Upstream 的 Trusted Thread Regression Test？

Staff Engineering / Upstream · 高难度

场景： 你修复了一个只在 SMP + RPC + Cross-Hart Resume 时触发的 `tp` 错误。

现在需要设计一个长期可维护的回归测试。

参考解析：

应该从可复现性和可验证性出发，而不是只提交一个压力脚本。

建议测试设计包括：

1. 多 hart QEMU 或目标平台配置。
2. 能够触发 RPC 的最小 TA。
3. 可控的 REE RPC 响应延迟。
4. 尽可能增加跨 hart Resume 的触发概率。
5. 明确记录 Hart ID、Thread ID 和状态转换。
6. 检查恢复后 Kernel Per-Hart State 是否正确。
7. 验证 TA 返回值、地址空间和数据完整性。
8. 多次重复及并发执行。
9. 失败时提供足够的日志用于定位。
10. 确保测试在没有对应硬件特性的配置中能够正确跳过。

更进一步，应考虑测试是否有稳定的断言，而不是依赖概率才能偶然发现故障。

对于不能稳定强制跨 hart 迁移的平台，可以考虑引入 Test-only Hook，或在支持的仿真环境中提供可控调度。

最后，应将 Bugfix 与测试形成明确关联：测试在旧实现上能够捕获问题，在修复后通过。

这才是一个高质量 upstream Patch Series 应该具备的工程完整性。

# 十四、本章总结：必须形成的五个核心认知

完成这一章后，最重要的不是记住全部函数名，而是能够建立以下五个设计模型。

第一，Trusted Thread 是执行上下文，而不是传统独立调度任务。

OP-TEE 的 Thread Pool 管理请求执行和保存状态，REE Scheduler 主要决定调用线程何时获得 CPU。

第二，Thread Context 和 Hart Context 不同。

Thread Context 可以在符合条件的情况下跨 hart 恢复；`thread_core_local`、Temporary Stack 等资源属于当前 hart。

尤其要记住 RISC-V Kernel `tp` 在跨 hart Resume 时的处理。

第三，Suspend/Resume 的本质是保存和恢复执行连续性。

不仅要保存 GPR、EPC 和 SP，还必须维护 Kernel Stack、TA Mapping、Session、FP/Vector 状态和资源生命周期。

第四，Trusted Thread Switch 与 OpenSBI Domain Switch 属于不同的安全层次。

前者保证 TEE Kernel 请求执行的连续性，后者保证不同 Domain 之间的硬件安全隔离和执行环境切换。

第五，Context Switching 是安全敏感代码。

在普通 RTOS 中，Context Switch 错误可能造成 Crash、死锁或数据损坏。

在 TEE 中，Context Switch 错误还可能导致：

- 跨 TA 数据泄露。
- 非可信 Domain 获得不应有的执行状态。
- 错误地址空间下执行。
- 敏感寄存器残留。
- 线程所有权和安全身份混淆。

因此必须按照安全内核代码的标准来设计和 Review。

## 本章推荐的 Upstream 实践项目

我尤其建议你以 RISC-V OP-TEE Trusted Thread Cross-Hart Resume Correctness 为题，开展一次独立的源码研究与测试工作。

研究范围包括：

`thread_resume_from_rpc()`、`thread_resume()`、`thread_trap_vect()`、`thread_core_local`、`tp/sscratch` 和 TA User Mapping。

最终成果可以是：

- 一份带寄存器和状态变化的完整 Execution Trace。
- 一套双 hart 或多 hart 的 RPC Resume 测试。
- 对异常、重复 Resume、Thread ID 复用和跨 hart 状态恢复的安全性分析。
- 如果发现实际缺陷，则形成带 Regression Test 的最小化修复补丁。

这个专题同时覆盖 RISC-V Architecture、汇编、SMP、OP-TEE Kernel 和 Security Review，非常适合长期朝 OP-TEE Core Contributor / Maintainer 方向深入。

Chapter 10 的最终掌握标准：能够独立从 `thread_alloc_and_run()` 开始，逐条追踪 RV64 汇编寄存器变化，完整解释 Trusted Thread 的启动、Suspend、跨 hart Resume 与最终释放，并证明每一次状态转换都满足执行正确性和安全隔离要求。
