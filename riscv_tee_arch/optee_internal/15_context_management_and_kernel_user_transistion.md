# **Chapter 15 — Context Management & Kernel/User Transition**

OP-TEE OS Internals · Advanced / Staff Engineer Level · OP-TEE 4.11.0

## **本章目标：理解 OP-TEE 如何安全地暂停、恢复和切换执行上下文**

这一章是整个 OP-TEE OS Internals 课程中非常重要的一章。

前面的 Chapter 14 研究了 Trusted Thread 的执行模型。本章要解决一个更底层的问题：

**当 OP-TEE 从 Secure Kernel 进入 User TA、从 TA 进入 Syscall、从 Secure World 返回 Normal World，或者从 RPC 恢复执行时，到底需要保存和恢复哪些状态？**

这个问题远不止保存 `x0~x30` 或 `ra/sp` 那么简单。

因为一个完整的执行上下文至少涉及：

- CPU General-Purpose Registers 与 Exception Return State。
- Kernel Stack、User Stack 和 Exception Stack。
- 当前 Trusted Thread 的执行状态。
- 当前 TA Context、Session 和调用身份。
- MMU Address Space、ASID 与内存访问权限。
- FPU、SIMD、TLS，以及其他架构扩展状态。
- Interrupt Mask、异常状态和中断相关上下文。
- Normal World 和 Secure World 之间的状态隔离。

其中的核心设计问题是：

> **在切换执行权限、执行主体或物理 CPU 时，什么状态必须保存？由谁保存？保存在哪里？又如何证明恢复后的状态仍然满足安全约束？**

我们将以 OP-TEE 4.11.0 的 Arm64 实现为主要源码基线，并从 Architecture Abstraction 的角度讨论 RISC-V RV64 应如何实现相同语义。源码中的真实函数与用于解释机制的简化代码会明确区分。

## **本章知识结构**

| 节次    | 主题                   | 重点                       |
| ----- | -------------------- | ------------------------ |
| 15.1  | Context Taxonomy     | 建立完整上下文分类                |
| 15.2  | Execution Model      | 理解 Thread、TA、Session 的关系 |
| 15.3  | Kernel/User Stacks   | 栈的所有权与切换                 |
| 15.4  | Enter User Mode      | Arm64 源码与异常返回            |
| 15.5  | Syscall Entry/Return | EL0 → EL1 → EL0          |
| 15.6  | RPC Suspension       | 保存状态与跨核恢复                |
| 15.7  | Address Space        | MMU 与上下文切换               |
| 15.8  | Extended State       | FPU、SIMD、TLS、PAUTH       |
| 15.9  | Fault & Cleanup      | 异常、Panic 和资源清理           |
| 15.10 | RISC-V Design        | RV64 Backend 的设计方法       |
| 15.11 | Change Impact        | 修改上下文管理的影响               |
| 15.12 | Hands-on             | GDB 和实验                  |
| 15.13 | Staff Assessment     | 高难度场景题与参考答案              |

# **15.1 首先建立正确的 Context 概念**

在讨论源码之前，必须先区分几个经常被混在一起的概念。

## **15.1.1 Context 不是单一的数据结构**

在一个传统 RTOS 中，我们经常用类似下面的数据结构表示线程上下文：

```
struct arch_context {
    unsigned long sp;
    unsigned long pc;
    unsigned long regs[NUM_REGS];
};
```

当发生线程调度时：

```
Thread A
   |
   | Save CPU registers
   v
Scheduler
   |
   | Restore CPU registers
   v
Thread B
```

这很容易让人产生一个直觉：

**Context Switch 就是保存当前线程的寄存器，然后恢复另一个线程的寄存器。**

但是这个理解对于 OP-TEE 是不完整的。

OP-TEE 可能发生以下不同类型的上下文变化：

| 事件                        | 改变的内容                            | 不一定改变的内容                  |
| ------------------------- | -------------------------------- | ------------------------- |
| Kernel → User TA          | EL、Stack、PC、Access Context       | Trusted Thread ID         |
| User TA → Syscall         | EL、Stack、Execution Handler       | TA Context、Session        |
| TA-A → TA-B               | Current Session、TA Address Space | Trusted Thread ID         |
| Secure RPC → Normal World | World、运行 CPU、活动 Thread           | 原有 Trusted Thread Context |
| RPC Resume → Secure World | CPU Context、活动 Thread、Memory Map | 原有 TA 调用语义                |
| Secure Interrupt          | Exception Context、Interrupt Mask | 被中断的 Session              |
| TA Panic                  | Execution Flow、TA 生命周期           | 物理 CPU 不一定改变              |

最值得注意的是：

**一次 Context Switch 不一定意味着 Thread Switch。**

反过来，一次 Trusted Thread 的 Suspend/Resume，也不一定涉及 TA Context 的创建或销毁。

## **15.1.2 六类核心上下文**

OP-TEE Context Hierarchy

**Physical CPU / Hart**

Live Registers · Privilege State · Exception State

**Trusted Thread Context**

Saved Registers · Kernel SP · State · TSD

TA / User Mode Context

User VM · Entry · Stack · Extended State

Session Context

Caller · Identity · Cancellation · Call Chain

Memory Context

Page Tables · ASID · Memory Permissions

Extended CPU State

FP/SIMD · TLS · Architecture Extensions

这是概念上的关系图，不表示所有状态都是逐层嵌套分配的 C 结构体。一个 TA Context 可以被多个调用关联；Session 也不等同于某个永久绑定的 CPU 或线程。

### **第一类：CPU Architectural Context**

即 CPU 实际执行指令所依赖的寄存器状态。

对于 AArch64：

```
General-purpose:
    X0 - X30

Execution:
    PC / ELR_EL1
    PSTATE / SPSR_EL1

Stack:
    SP_EL0
    SP_EL1
    SP selection

Exception:
    ESR_EL1
    FAR_EL1
    VBAR_EL1

Memory:
    TTBR0_EL1
    TCR_EL1
    MAIR_EL1

Extended:
    FP/SIMD
    TPIDR_EL0
    Pointer Authentication keys
```

需要强调，这并不是每次 Context Switch 都必须完整保存的寄存器清单。

例如：

- 通用寄存器通常属于具体执行路径。
- `ELR_EL1`、`SPSR_EL1` 描述异常返回目标和状态。
- `TTBR0_EL1` 属于当前地址转换上下文。
- `VBAR_EL1` 通常由内核管理，而不是每个 TA 单独拥有。
- `TCR_EL1`、`MAIR_EL1` 多数情况下属于初始化后的内核 MMU 配置，不应简单视为 TA 可变私有状态。

**CPU 存在多少种寄存器，不等于 Context Switch 必须保存多少个寄存器。**

### **第二类：Trusted Thread Context**

它记录 OP-TEE 一个 Trusted Thread 被暂停后如何继续执行。

典型信息包括：

- 保存的 CPU 寄存器。
- Kernel Stack Pointer。
- Thread State。
- Thread-specific Data。
- User Mapping 状态。
- Extended Register State。
- RPC Resume 所需状态。

这是 Thread Suspend/Resume 的核心。

### **第三类：TA Context**

它代表一个可信应用实例，而不是单次执行。

例如：

```
TA-A Context
    |
    +-- Loaded code
    +-- Private data
    +-- Heap
    +-- User virtual address space
    +-- User stack configuration
    +-- Crypto / Storage resources
    +-- TA properties
```

同一个 TA Context 的生命周期可能跨越多次 `InvokeCommand`。

### **第四类：Session Context**

Session 描述一次 Client 与 TA 之间建立的逻辑会话。

Session 需要记录：

- 调用关系和调用者身份。
- 目标 TA。
- Session 状态。
- Cancellation 状态。
- Session-specific 数据及资源引用。

它不等于 CPU 上的一组寄存器。

### **第五类：Memory Context**

例如：

```
TA-A
    User VA 0x100000
        -> Physical Page A

TA-B
    User VA 0x100000
        -> Physical Page B
```

即便两个 TA 使用相同的 VA，也必须根据当前运行的 TA Address Space 得到正确的物理页面。

因此执行 TA-A 时，不能意外保留 TA-B 的地址转换上下文。

### **第六类：Extended CPU State**

主要包括：

- FP/SIMD。
- Thread-local Storage 寄存器。
- Pointer Authentication 状态。
- 其他可由用户态使用的架构扩展。

这些状态往往非常容易在 Porting 或性能优化中被遗漏。

尤其是 FP/SIMD：它既可能涉及大量寄存器，又可能包含用户 TA 的敏感数据。

## **15.1.3 最重要的设计原则：Context 必须按所有权管理**

可以用下面的表作为全章的基础。

| 状态                 | 逻辑所有者                    | 典型保存位置                       |
| ------------------ | ------------------------ | ---------------------------- |
| 当前运行 GPR           | 当前执行路径                   | CPU / Exception Frame        |
| 暂停的 Trusted Thread | Trusted Thread           | `thread_ctx` 及其 Kernel Stack |
| TA VM              | TA/User Context          | `user_mode_ctx` / VM         |
| TA Session         | 调用关系                     | `ts_session` / TA Session    |
| 当前 Session Stack   | Trusted Thread           | Thread-specific Data         |
| 当前 CPU 的活动线程 ID    | Per-CPU                  | `thread_core_local`          |
| User FP/SIMD       | User Context 或线程相关保存状态   | 架构相关 Extended State          |
| Kernel FP/SIMD     | Kernel Execution Context | 架构相关保存状态                     |

这里我们建立第一个安全不变量：

> **Invariant 15-1：任何一次执行上下文切换，都必须让 CPU 可观察到的权限、寄存器状态、内存视图以及当前软件执行身份保持一致。**

这意味着，即便寄存器保存正确，如果恢复了错误的 TA Address Space，整个 Context Switch 仍然是错误的。

# **15.2 从真实源码理解 OP-TEE 的 Context Hierarchy**

## **15.2.1&#x20;**`thread_ctx`**：可挂起执行上下文的核心**

我们先看 `core/include/kernel/thread_private.h`。

在 4.11.0 中，`struct thread_ctx` 的关键字段可以概括为：

```
/* 教学摘录：保留关键字段，省略条件编译和其他成员 */
struct thread_ctx {
    struct thread_ctx_regs regs;
    enum thread_state state;

    vaddr_t stack_va_end;
    uint32_t flags;

    struct core_mmu_user_map user_map;
    bool have_user_map;

    vaddr_t kern_sp;   /* ARM64 / RV64 */

    /* FP state, RPC buffers, cache... */
    struct thread_specific_data tsd;
};
```

这不是一个传统意义上的 `task_struct`。

首先，它记录了当前 Trusted Thread 的寄存器和栈状态。

其次，它记录了该线程暂停时正在使用的 User Mapping。

第三，它通过 `tsd` 维护当前执行路径的 Session Stack 和其他线程特定信息。

源码位置：[thread_private.h](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/include/kernel/thread_private.h?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **关键字段分别解决什么问题？**

`regs` 负责保存暂停点的 CPU State。对于 RPC Suspend，它需要让线程从原来被暂停的指令位置继续执行。

`state` 表示这个线程是否可以被分配或恢复，其主要状态为：

```
FREE
  |
  | Allocate
  v
ACTIVE
  |
  | RPC / Foreign Interrupt
  v
SUSPENDED
  |
  | Resume
  v
ACTIVE
  |
  | Complete
  v
FREE
```

`kern_sp` 记录进入用户态时内核的调用栈位置。它对于 Syscall 返回至原内核执行流十分重要，后面会深入。

`user_map` 和 `have_user_map` 则是一个非常值得关注的设计。

想象 OP-TEE 正在运行 TA-A：

```
Trusted Thread 3

CPU registers:
    Current PC = TA-A instruction

Address translation:
    TA-A Page Tables

Current Session:
    Session to TA-A
```

随后发生 Foreign Interrupt，线程必须返回 Normal World。

这时候，如果 OP-TEE 只保存 `PC` 和通用寄存器，却没有记录使用的是 TA-A 的 Memory Context，那么以后恢复该线程时，它可能在错误的页表下继续运行。

因此，`thread_ctx` 的状态和 Memory Mapping 必须一起恢复。

这就是为什么 `thread_ctx` 中会存在 `user_map`。

## **15.2.2&#x20;**`thread_core_local`**：为什么需要 Per-CPU Context？**

再看 `core/arch/arm/include/kernel/thread_arch.h`。

其中的 `struct thread_core_local` 包含：

```
/* 教学化摘录 */
struct thread_core_local {
    /* Architecture-dependent scratch registers */

    vaddr_t tmp_stack_va_end;
    short int curr_thread;
    uint32_t flags;
    vaddr_t abt_stack_va_end;

    /* Optional security and debug fields */
};
```

关键字段是：

```
curr_thread
```

它表示**当前 CPU 上正在执行哪个 Trusted Thread**，而不是系统中的线程总数。

假设有四个 CPU：

| CPU  | curr_thread | 状态                  |
| ---- | ----------- | ------------------- |
| CPU0 | 2           | 执行 TA-A             |
| CPU1 | -1          | 使用 Temporary Stack  |
| CPU2 | 5           | 处理 TA-B Syscall     |
| CPU3 | -1          | 没有活动 Trusted Thread |

当 CPU0 上的 Thread 2 被 Suspend 时：

```
CPU0:
    curr_thread = INVALID

Thread 2:
    state = SUSPENDED
```

之后 Thread 2 可以在满足实现约束的情况下由其他 CPU 恢复。

因此：

- `thread_core_local` 是 **Per-CPU State**。
- `thread_ctx` 是 **Per-Trusted-Thread State**。
- 二者不是一一永久绑定的关系。

这也是 OP-TEE 与某些简单的 Per-Core Secure Service 实现之间的重要差异。

## **15.2.3&#x20;**`thread_specific_data`**：正在执行谁的服务？**

`thread_specific_data` 中的关键部分：

```
struct thread_specific_data {
    TAILQ_HEAD(, ts_session) sess_stack;
    struct ts_ctx *ctx;

    /* Abort, debug, recursion metadata... */
};
```

这里有两个需要理解的成员：

`ctx` 表示当前关联的 Trusted Service Context。

`sess_stack` 则表示当前 Trusted Thread 的嵌套调用关系。

例如：

```
Linux CA
   |
   v
TA-A
   |
   | TEE_OpenTASession()
   v
TA-B
   |
   | Invoke PTA service
   v
PTA-C
```

对于这个调用过程，Trusted Thread 可以保持不变，但 Session Stack 会变化：

```
Top -> PTA-C Session
       TA-B Session
       TA-A Session
```

这里需要注意，PTA 在 Kernel 内执行，并不因此获得自己的 User VA Mapping。

实际 `ts_manager.c` 中，`update_current_ctx()` 会考虑当前 Session 的类型，如果栈顶是 PTA，它会向调用链下方寻找合适的 User Mode Context，从而避免错误切换到不存在的 PTA 用户页表。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这个细节非常值得学习。

它说明：

**Current Session 不一定等于 Current User Address Space 的所有者。**

例如 User TA 调用 PTA 时，当前 Session 可以是 PTA，但内存上下文仍与调用方 User TA 相关。

## **15.2.4&#x20;**`user_mode_ctx`**：用户程序长期存在的运行环境**

`core/include/kernel/user_mode_ctx_struct.h` 中的结构体主要包括：

| 字段                | 作用                              |
| ----------------- | ------------------------------- |
| `vm_info`         | User VM Regions、ASID 等          |
| `regions`         | Pager 管理的用户内存区域                 |
| `pgt_cache`       | Page Table Cache                |
| `vfp`             | 可选的 User FP/SIMD State          |
| `keys`            | 可选的 Pointer Authentication Keys |
| `ts_ctx`          | 对应的 Trusted Service Context     |
| `entry_func`      | 用户态入口                           |
| `stack_ptr`       | 用户栈指针配置                         |
| `is_32bit`        | 用户执行模式                          |
| `ldelf_stack_ptr` | Loader 相关栈                      |
| `bbuf`            | Bounce Buffer 相关状态              |

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

这个结构体的生命周期，与 `thread_ctx` 很不一样。

假设：

```
Single Instance TA-A
   |
   +-- Session 1
   +-- Session 2
   +-- Session 3
```

TA-A 可以维持同一个 User Mode Context，但不同的调用并不意味着拥有同一个 Trusted Thread。

**同一个 TA Context 可以跨多个请求复用；Trusted Thread 是处理请求的执行载体。**

如果一个 TA Context 被多个执行路径共享，则必须依赖 TA Session/Concurrency 机制保证合法的并发行为。不能因为它属于 Secure World，就假定天然线程安全。

# **15.3 Kernel Stack、User Stack 与 Exception Stack**

这一节是本章最容易在汇编级别产生混淆的地方。

## **15.3.1 OP-TEE 为什么需要多种 Stack？**

OP-TEE 典型存在三类内核栈：

| Stack                 | 所有权        | 用途                          |
| --------------------- | ---------- | --------------------------- |
| Temporary Stack       | Per-CPU    | 尚未进入正常 Trusted Thread 的入口执行 |
| Trusted Thread Stack  | Per-Thread | 处理请求与内核 C 调用                |
| Abort/Exception Stack | Per-CPU    | 特定异常及 Abort 处理              |

此外，User TA 拥有自己的 User Stack。

需要注意：Exception 的入口并不是无条件立即切换到独立的 Abort Stack。具体使用何种栈，取决于异常来源、异常类别和架构入口实现。

## **15.3.2 AArch64 中最容易理解错的地方：SP_EL0 不一定只用于 User Mode**

Armv8-A 中有两个与 EL1 相关的栈指针：

```
SP_EL0
SP_EL1
```

一种常见的理解是：

```
EL0 -> SP_EL0
EL1 -> SP_EL1
```

但这不是绝对规则。

AArch64 允许 EL1 通过 `SPSel` 选择当前使用 `SP_EL0` 还是 `SP_EL1`。

在 OP-TEE Arm64 的实现中，有一个比较特殊的安排：

**正常 Thread Kernel Execution 可以在 EL1 使用 SP_EL0 作为工作栈指针，而 SP_EL1 用于关联 Per-CPU Core-local 状态。**

因此不能简单认为：

```
SP_EL0 == TA User Stack
SP_EL1 == Kernel Thread Stack
```

在不同执行阶段，`SP_EL0` 的实际含义可能发生变化。

## **15.3.3 真实的栈切换过程**

以一次普通 TA 调用为例：

AArch64 Kernel/User Stack Transition

**阶段 A：EL1 内核执行**

Current SP = Kernel Thread Stack

准备用户入口、参数和 User Mode Context

**阶段 B：准备进入 EL0**

保存 Kernel SP → `thread_ctx.kern_sp`

设置 `SP_EL0` = TA User SP

配置 `ELR_EL1` 和 `SPSR_EL1`

**阶段 C：EL0 User TA**

Current SP = TA User Stack

用户态无法访问内核私有映射

**阶段 D：SVC Exception**

临时保存 User SP

恢复 `thread_ctx.kern_sp`

建立内核 Syscall Frame

**阶段 E：Syscall 返回**

恢复 User SP、ELR、SPSR

`eret` 返回 EL0

真正需要保留的不是单纯的 Stack Pointer，而是：

```
Kernel Continuation
    = Kernel SP
    + Saved Callee Registers
    + Return Address
    + Calling Context
```

否则即使能够从 EL0 回到 EL1，也无法保证继续执行原来的 C 函数。

## **15.3.4 为什么不能直接在 User Stack 上处理 Syscall？**

假设恶意 TA 把 User SP 设置成：

```
SP = unmapped address
```

或者：

```
SP = memory close to guard page
```

如果 Kernel 在处理 SVC 时直接在这个栈上压入内部数据，就存在严重问题。

User Stack：

- 由不可信 User TA 操作。
- 可能没有足够可用空间。
- 可能不可写或没有映射。
- 不能用于存储内核敏感状态。

所以异常入口必须尽快获得一个可信的内核栈。

在 AArch64 中，硬件异常入口和 OP-TEE 的汇编代码共同完成这一转换；在 RISC-V 中则需要显式的软件栈交换和 Scratch CSR 设计。

# **15.4 Secure Kernel → User TA：从 C 到汇编的完整路径**

我们现在开始真正追踪源码。

## **15.4.1 第一站：**`user_ta_enter()`

源码：

`core/kernel/user_ta.c`

这个函数负责让当前 Trusted Thread 开始执行某个 User TA 的入口。

关键流程如下：

```
user_ta_enter(session, func, cmd)
    |
    +-- 检查 Syscall/TA 调用递归深度
    |
    +-- vm_map_param()
    |      映射 TA 本次调用参数
    |
    +-- ts_push_current_session()
    |      更新 Session Stack 和 User Mapping
    |
    +-- 准备 User Stack 上的 utee_params
    |
    +-- thread_enter_user_mode()
    |      切换到 User TA
    |
    +-- User TA 返回
    |
    +-- thread_user_clear_vfp()
    |
    +-- 处理 TA Panic / Result
    |
    +-- vm_clean_param()
    |
    +-- ts_pop_current_session()
    |
    +-- 清理 Cancellation State
```

这是根据 4.11.0 `user_ta.c` 的实际控制流程整理的。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里有三个关键观察。

**第一，用户参数映射发生在进入用户态之前。**

因为 User TA 不能直接使用内核 VA，也不能任意访问共享内存物理地址。

**第二，Session Stack 的更新发生在进入用户态之前。**

这样 User TA 执行 Syscall 时，内核可以根据当前 Session 找到正确的服务身份及 Context。

**第三，TA Panic 后必须沿另一条路径返回和清理。**

用户态退出并不意味着一定正常完成调用。

## **15.4.2 第二站：**`thread_enter_user_mode()`

在 Arm64 中继续进入：

`core/arch/arm/kernel/thread.c`

核心过程可以抽象为以下伪代码：

```
/* 教学伪代码，不是上游源码逐字复制 */
uint32_t enter_ta_user_mode(...)
{
    uint32_t target_status;
    struct thread_ctx_regs *regs;

    target_status = prepare_el0_status();

    mask_interrupts_during_transition();

    regs = get_current_thread_saved_regs();

    initialize_user_registers(regs,
                              args,
                              user_stack,
                              user_entry,
                              target_status);

    result = arch_enter_user_mode(regs);

    restore_previous_interrupt_state();

    return result;
}
```

真正的实现还有调用时间统计、PAUTH Key 和异常处理逻辑。

但有一个非常重要的安全细节：

**上游会先清空将用于用户入口的寄存器保存区，再填入指定的用户参数和执行状态。**

这不是普通的代码风格选择。

它避免旧的 Trusted Thread、TA 或 Kernel Register Contents 被无意暴露给新的用户执行流。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



如果你进行 RISC-V Porting，这条原则同样必须满足。

例如不能只初始化：

```
a0-a3
sp
sepc
sstatus
```

却让 `t0-t6`、`s0-s11` 或 `gp` 保留上一段安全服务的敏感数据，并在返回 U-mode 时暴露。

## **15.4.3 第三站：**`__thread_enter_user_mode()`

真正的 Arm64 汇编实现位于：

`core/arch/arm/kernel/thread_a64.S`

它的主要过程是：

```
1. 在 Kernel Stack 中建立 User Mode Record
2. 保存 Kernel Callee-saved Registers
3. 保存 Kernel SP 到 thread_ctx.kern_sp
4. 切换到异常入口需要的 Stack Selection
5. 设置 SP_EL0
6. 设置 ELR_EL1
7. 设置 SPSR_EL1
8. 恢复 User GPRs
9. 准备 EL0 Address Translation / PAUTH
10. ERET
```

具体可以重点观察这些真实指令：

| 指令                 | 目的              |
| ------------------ | --------------- |
| `msr spsel, #1`    | 选择 SP_EL1       |
| `msr sp_el0, xN`   | 设置将要使用的 User SP |
| `msr elr_el1, xN`  | 设置异常返回目标 PC     |
| `msr spsr_el1, xN` | 设置异常返回后的 PSTATE |
| `eret`             | 按已配置的异常返回状态执行   |

其中设置 `ELR_EL1` 与 `SPSR_EL1` 是关键。

### **为什么不直接用&#x20;**`br entry_func`**？**

因为普通 `br` 只改变 PC，不会执行从 EL1 到 EL0 的特权级转换。

执行以下代码：

```
br x0
```

不会自动获得：

```
EL1 → EL0
```

正确的机制是通过 Exception Return。

`ERET` 从对应的 Exception Link Register 和 Saved Program Status Register 中恢复目标执行位置及状态。

假设：

```
ELR_EL1  = 0x400100
SPSR_EL1 = EL0t configuration
```

执行 `eret` 后：

```
Current EL = EL0
PC         = 0x400100
SP         = SP_EL0
```

这是用户态执行权限转换的硬件基础。实际代码还要配合寄存器恢复、页表、安全扩展状态及中断屏蔽处理。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **15.4.4 第四站：**`thread_unwind_user_mode()`**&#x20;的特殊设计**

这里涉及一个非常巧妙的实现。

上层 C 代码这样调用：

```
result = thread_enter_user_mode(...);
```

但 `thread_enter_user_mode()` 并不是简单地执行一个子函数，然后立即 `ret` 回来。

它进入了 User Mode。

User TA 运行期间可能执行成千上万条指令，并发生大量 Syscall。

最终 User TA 调用特定的 Return/Panic Syscall，内核决定结束当前 User Execution。

这时它会通过：

```
thread_unwind_user_mode()
```

恢复先前保存在 Kernel Stack 上的调用状态。

最终效果看起来就像：

```
result = thread_enter_user_mode(...);
```

这个 C 函数正常返回了一样。

但在底层，实现实际上完成了：

```
Kernel C Call
    |
    v
Save Kernel Continuation
    |
    v
Enter EL0
    |
    v
User TA Execution
    |
    v
SVC Return
    |
    v
Restore Kernel Continuation
    |
    v
Return to Kernel C Caller
```

这类似于一种受严格控制的跨权限执行 Continuation。

需要特别理解：

**User TA 的正常退出并不是直接跳到 Kernel C Caller，而是通过内核 Syscall Handler 有控制地恢复先前保存的内核调用状态。**

这种设计为什么重要？

因为 User TA 不能决定 Kernel Return Address、Kernel SP 或恢复哪些内核寄存器。

这些信息必须保存在 User TA 不可修改的内核区域中。

# **15.5 User TA → Kernel Syscall → User TA**

接下来研究反方向。

这是与 OS 权限隔离关系最直接的一条路径。

## **15.5.1 从&#x20;**`TEE_*()`**&#x20;到异常入口**

选择一个典型的 TEE API：

```
TEE_GenerateRandom(buffer, size);
```

逻辑路径为：

```
User TA
   |
   v
TEE_GenerateRandom()
   |
   v
libutee
   |
   v
_utee_cryp_random_number_generate()
   |
   v
AArch64 Syscall Stub
   |
   v
SVC
   |
   v
EL1 Exception Vector
   |
   v
el0_svc
   |
   v
thread_scall_handler()
   |
   v
scall_handle_user_ta()
   |
   v
syscall_cryp_random_number_generate()
```

这是跨越 User Library、Architecture Trap 和 Kernel Service 的调用链。

但有一点要分清楚：

`SVC`**&#x20;不是由 Linux 处理的，也不是交给 EL3 Secure Monitor 处理。**

在这个典型的 Secure EL0 / Secure EL1 配置中，User TA 的 `SVC` 被 Secure EL1 的 OP-TEE Kernel 接收。

这与 CA 通过 Linux TEE Driver 进入 Secure World 是两个不同的转换。

## **15.5.2 真实 Syscall Exception 发生了什么？**

假设 User TA 正在执行：

```
svc #0
```

当同步异常进入 EL1 时，硬件负责建立异常入口的基本架构状态，例如：

```
ELR_EL1  = 返回 User TA 的 PC
SPSR_EL1 = User TA 的 PSTATE
ESR_EL1  = 异常原因
```

然后进入 OP-TEE 的异常向量。

在 `thread_a64.S` 中，入口会检查 `ESR_EL1` 的 Exception Class，判断是否属于 AArch64 或 AArch32 的 SVC，再进入 `el0_svc`。

`el0_svc` 的关键过程：

**EL0 Trap Entry**

Hardware captures Exception Return State

**Trusted Stack Recovery**

Load `thread_ctx.kern_sp` and build Syscall Frame

**Save User Execution State**

GPR · User SP · ELR · SPSR · TLS / PAUTH as applicable

**Syscall Dispatch**

Current Session → Handler → Kernel Service

**Restore & Return**

Restore Context · Permissions · `eret`

注意其中两个不同的保存对象：

- `thread_ctx_regs` 主要服务于完整线程恢复及用户入口准备。
- `thread_scall_regs` 主要服务于当前 Syscall Exception 的返回与处理。

二者不能在设计中混成同一个东西。

## **15.5.3 为什么需要&#x20;**`thread_scall_regs`**？**

对于普通 SVC，并不需要把所有 User GPR 都永久保存到 TA Context 中。

因为 Syscall 是一个同步调用。

Kernel 只需要保存本次调用期间会被覆盖、并且返回 User Mode 时仍然需要恢复的状态。

可以用类似下面的概念来理解：

```
/* 概念结构，非实际布局 */
struct syscall_frame {
    unsigned long arguments[...];

    unsigned long user_sp;
    unsigned long user_pc;
    unsigned long user_status;

    unsigned long preserved_registers[...];
};
```

Syscall Frame 的生命周期通常短于 Trusted Thread Context。

例如：

```
User TA executes
    |
    +-- SVC #1
    |      create syscall frame
    |      handle syscall
    |      restore frame
    |
    +-- SVC #2
    |      create another frame
    |      handle syscall
    |      restore frame
    |
    +-- Continue TA execution
```

由于 Kernel C 函数会遵循 ABI 的 Caller/Callee Saved 约定，因此实际的 Syscall Frame 可以根据 ABI 和汇编路径进行优化，而不需要不加区别地保存每个寄存器。

**但是优化保存寄存器之前，必须证明它们在所有可发生的执行路径中确实不会丢失。**

尤其是 Syscall Handler 可能进一步调用其他 TA、执行 RPC，或者在处理过程中发生中断。

## **15.5.4&#x20;**`thread_scall_handler()`**&#x20;如何找到正确的服务？**

位于：

`core/arch/arm/kernel/thread.c`

核心逻辑可以归纳为：

```
/* 教学伪代码 */
void syscall_exception_handler(struct syscall_frame *regs)
{
    struct ts_session *session;

    prepare_interrupt_state();
    save_user_extended_state_if_needed();

    session = current_session();

    if (session->handle_scall(regs)) {
        /* Restore user execution */
    } else {
        /* Finish user execution and unwind kernel continuation */
        prepare_unwind_to_kernel(regs);
    }
}
```

这里的关键不是某个 `switch (syscall_number)`。

真正重要的是：

```
session->handle_scall(regs)
```

为什么需要通过当前 Session Dispatch？

因为 OP-TEE 中存在不同的用户执行主体。

例如：

- User TA。
- `ldelf`。
- 某些 Secure Partition 执行模型。

它们不一定具有完全相同的 Syscall 权限或 Dispatch Table。

在 4.11.0 的 `user_ta_ops` 中，可以看到 `.handle_scall = scall_handle_user_ta`；而 `scall.c` 还维护单独的 `ldelf_syscall_table`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



这是一项非常重要的安全设计：

**Syscall 权限不能只根据数字解释，还必须由当前执行主体的身份和类型共同决定。**

## **15.5.5 User TA Return 并不是普通 Syscall Return**

在 `core/kernel/scall.c` 中，`scall_handle_user_ta()` 对普通 Syscall 和特殊 Return/Panic Syscall 进行区分。

其返回语义可以理解为：

| Syscall 类型        | 返回行为                        |
| ----------------- | --------------------------- |
| 普通 TEE API        | 返回原 User TA 执行流             |
| `TEE_SCN_RETURN`  | 结束当前 User Entry             |
| `TEE_SCN_PANIC`   | 结束当前 User Entry，携带 Panic 状态 |
| 非法 Syscall Number | 按未支持的 Syscall 处理            |

对于普通 API：

```
SVC -> Kernel Handler -> ERET -> User TA
```

对于 TA Return：

```
SVC -> Kernel Handler
    -> thread_unwind_user_mode
    -> Kernel C Caller
```

`thread_scall_handler()` 会通过 `setup_unwind_user_mode()` 为第二种情况准备返回状态，使 Exception Return 不再回到 User TA，而是回到内核的 `thread_unwind_user_mode()`。

这也解释了为什么不能把所有 SVC 都简单实现为：

```
Save registers
Handle syscall
Restore registers
Return to user
```

因为某些 Syscall 根本不应该返回原来的 User PC。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **15.5.6 嵌套 TA 调用为什么尤其复杂？**

研究如下场景：

```
CA
 |
 v
TA-A
 |
 | TEE_InvokeTACommand()
 v
TA-B
 |
 | Return
 v
TA-A
 |
 | Return
 v
CA
```

假设只使用一个 Trusted Thread。

那么 Kernel Stack 中可能包含：

```
Kernel Thread Stack

+----------------------------+
| CA -> TA-A Kernel Frame    |
+----------------------------+
| TA-A Enter User Record     |
+----------------------------+
| TA-A Syscall Frame         |
+----------------------------+
| TA-B Session/Invoke Frames |
+----------------------------+
| TA-B Enter User Record     |
+----------------------------+
| TA-B Execution ...         |
+----------------------------+
```

上图是逻辑示意，不代表实际连续地址从低到高的布局。

最重要的是 `kern_sp` 的变化：

```
进入 TA-A：
    kern_sp = KSP_A

TA-A 调用 TA-B：
    内核创建嵌套执行路径
    kern_sp = KSP_B

TA-B 返回：
    恢复 TA-A 的 Syscall Continuation
    kern_sp = KSP_A

TA-A 继续运行
```

Arm64 `el0_svc` 中有专门的注释和代码，说明在 Syscall Handler 返回后需要更新保存的 Kernel SP，原因正是嵌套 TA 调用会修改这个位置。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这也是我建议你实际拿 GDB 观察的重点。

一个很容易犯的错误是：

> 只在第一次进入 TA 时保存 Kernel SP，之后所有嵌套用户态执行都复用同一个保存值。

这样在简单的 CA → TA 场景下可能正常，但在 TA-A → TA-B → TA-A 场景下，就有机会破坏 Kernel Stack Continuation。

# **15.6 RPC Suspend/Resume：Context 如何跨越 Normal World**

这一节把前面讨论的 Thread、Stack、MMU 和寄存器状态连接起来。

## **15.6.1 RPC 不是普通函数调用**

假设 TA 发起一次安全存储操作：

```
TA-A
  |
  v
Secure Storage Service
  |
  v
RPC to tee-supplicant
  |
  v
Linux Filesystem
```

当 OP-TEE 发出 RPC 时，并不意味着 TA 已经执行完成。

实际上：

```
Trusted Thread = SUSPENDED
TA Context     = Still Alive
Session        = Still Alive
Kernel Stack   = Must be resumable
```

OP-TEE 需要将当前 Trusted Thread 的执行位置和关键状态保存起来，然后允许 Normal World 执行 RPC 服务。

这类似一个跨世界 Continuation。

## **15.6.2 Suspend 时保存什么？**

以 Arm64 的 `thread_state_suspend()` 为例，它会处理：

```
Current Trusted Thread
    |
    +-- Check Stack Canary
    +-- Handle Pager Stack Resources
    +-- Save User FP State if needed
    +-- Preserve Resume PC/PSTATE
    +-- Record Thread Flags
    +-- Record User Mapping
    +-- Deactivate Current User Mapping
    +-- Set Thread State = SUSPENDED
    +-- Clear Per-CPU curr_thread
```

对于 `SUSPENDED` 的线程：

```
Thread 2:
    regs       = saved execution state
    state      = SUSPENDED
    have_user_map = true
    user_map   = TA-A mapping
    kern_sp    = saved continuation
    tsd        = session stack
```

这里最重要的是：

```
CPU current thread ID != logical Trusted Thread identity
```

即使 CPU0 不再运行 Thread 2，Thread 2 仍然是一个有效的可恢复执行上下文。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **15.6.3 Resume 时的顺序**

`thread_resume_from_rpc()` 中，一个关键执行顺序是：

```
1. 验证 Thread ID 与 SUSPENDED State
2. 将 Thread State 切回 ACTIVE
3. 在当前 CPU 上设置 curr_thread
4. 恢复 User Mapping（如果存在）
5. 恢复相关时间统计与 RPC 返回状态
6. 处理 Non-Secure FP/SIMD 状态
7. thread_resume(&regs)
```

这里的顺序不能随意调整。

例如：

```
错误做法：
    恢复 TA PC
    再恢复 TA Page Tables
```

这种实现可能在错误地址空间下执行 TA 指令。

同样不能在恢复 `curr_thread` 之前，让会查询当前 Trusted Thread ID 的代码开始执行。

上游通过受锁保护的状态转移以及相应的 MMU 恢复流程，实现上述生命周期。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **15.6.4 为什么必须考虑跨核恢复？**

考虑：

```
CPU0:
    Thread 2 enters RPC
    Thread 2 becomes SUSPENDED

Normal World:
    RPC is serviced

CPU1:
    Resume request for Thread 2
```

OP-TEE 不能依赖 CPU0 的实时 GPR 保存着 Thread 2 的状态，因为 CPU0 在这段时间可能早已执行其他工作。

所以恢复状态必须来自可被合法访问的持久化 Thread Context，而不是特定 CPU 的瞬时寄存器。

需要迁移或重建的状态包括：

| 状态               | 恢复要求                |
| ---------------- | ------------------- |
| Saved GPR        | 从 Thread Context 恢复 |
| Resume PC/PSTATE | 恢复精确暂停点             |
| Kernel SP        | 必须指向该 Thread 的可信栈   |
| User Map         | 在当前 CPU 上重新激活       |
| FPU/SIMD         | 处理当前 CPU 原有寄存器所有者   |
| Interrupt State  | 恢复正确的异常屏蔽语义         |
| Per-CPU State    | 由当前 CPU 重建          |
| Session Stack    | 继续使用该 Thread 的调用关系  |

更深入地说，所谓“Thread 可以跨核恢复”，并不是只要内存全局可见就自然成立。

仍然要求：

- 正确的状态同步。
- 兼容的 CPU 架构能力。
- 对 MMU/TLB 状态的正确管理。
- 对 Extended State 的保存恢复。
- 不依赖原 CPU 的不可迁移资源。

这是所有跨核 Context Switch 的本质约束。

## **15.6.5 Normal World 的 RPC 返回参数为什么不能完全信任？**

OP-TEE 在处理 RPC Resume 时，必须区分两类状态：

1. Normal World 可以合法提供的 RPC 结果。
2. Normal World 绝对不应能够修改的 Secure Execution Context。

例如，Normal World 可以返回一次文件访问结果，但不能通过 RPC 返回内容指定：

```
Secure PC = arbitrary address
Secure SP = attacker-controlled address
User TA identity = another TA
```

源码中 `THREAD_FLAGS_COPY_ARGS_ON_RETURN` 之类的标志，就是用于控制什么时候允许将外部返回值复制到保存的寄存器上下文。

这属于一项很重要的安全边界：

> **Invariant 15-2：RPC Result 可以作为数据进入 Secure World，但不能被直接提升为可信的执行控制状态。**

必须同时留意一个更一般的问题：只检查 Thread ID 是否处于 `SUSPENDED`，并不等于已经完成了所有身份认证或防重放要求。部署于多 Guest、FF-A 或其他 Transport 时，还需要根据相应协议设计验证 Resume Token、Endpoint、Guest Identity 和 Request Ownership。

# **15.7 Address Space Switch：寄存器恢复正确还远远不够**

## **15.7.1 TA-A → TA-B 时，最先要保证什么？**

假设：

```
TA-A:
    Virtual 0x100000 -> Physical A

TA-B:
    Virtual 0x100000 -> Physical B
```

如果从 TA-A 切到 TA-B 时只修改 PC 和 GPR，不修改 Memory Context，那么 TA-B 的第一条指令可能会访问 TA-A 的私有内存。

因此，必须满足：

\\[ \text{Active Execution Context} \Rightarrow \text{Correct Address Space} \\]

在 OP-TEE 中，当前 Session Stack 与 User Mapping 的切换并不是互不相关的两个操作。

## **15.7.2&#x20;**`ts_push_current_session()`**&#x20;与&#x20;**`vm_set_ctx()`

源码：

`core/kernel/ts_manager.c`

执行：

```
ts_push_current_session(session);
```

不仅意味着将一个 Session 加入当前 Trusted Thread 的调用栈。

它还会触发当前 Trusted Service Context 的更新。

如果当前 User Mode Context 发生变化，则进入 `vm_set_ctx()`。

在 `core/mm/vm.c` 中，`vm_set_ctx()` 的关键工作为：

```
Deactivate previous User Mapping
    |
    v
Release/recycle old Context Page Tables as appropriate
    |
    v
Create new User Mapping
    |
    v
Activate new User Mapping
    |
    v
Assign Pager Tables
    |
    v
Update Current TSD Context
```

真实实现还会根据目标 Context 类型判断是否应该建立 User Mapping。

这就把 Session 和 MMU 连接起来了。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **15.7.3 为什么切换页表时必须关注 ASID 和 TLB？**

CPU 的 TLB 会缓存地址转换结果。

假设：

```
TA-A:
    ASID = 10
    VA X -> PA A

TA-B:
    ASID = 11
    VA X -> PA B
```

如果 ASID 和 Page Table 设计正确，两个上下文可以合法使用相同 VA，而不会混淆 TLB Entry。

但是假设：

```
TA-A 被销毁
ASID 10 被释放

TA-C 获得 ASID 10
```

这时系统不能继续保留此前仍可能被使用的 TA-A 地址转换记录。

否则 TA-C 可能观察到 TA-A 的陈旧映射。

因此需要维护：

> **Invariant 15-3：ASID 的复用与 Page Table 生命周期必须协调，不能允许过期的地址转换继续影响新的执行主体。**

实际实现中还要考虑：

- Local TLB 与 Remote TLB。
- Page Table 修改与转换缓存同步。
- ASID 回收。
- Pager 更新。
- 多核同时使用同一 TA Context。
- Instruction/Data Cache 与可执行权限变更。

RISC-V 平台下对应的问题是：

```
satp
ASID
sfence.vma
```

如果还涉及 Hypervisor 或系统级 Memory Domain，则需要额外考虑相应转换层次及硬件保护机制。

但要特别区分：

**CPU Page Table 切换、SMMPT/IOMMU 权限更新、Supervisor Domain 切换，是相关但不同的操作。**

不能把执行 `sfence.vma` 当成对所有设备 Translation Cache 和系统级 Protection State 都已经完成同步的证明。

## **15.7.4 Kernel Mapping 是否始终对 User TA 可见？**

不能一概而论。

OP-TEE 的 Arm64 实现存在：

```
CFG_CORE_UNMAP_CORE_AT_EL0
```

相关配置用于实现用户执行阶段减少 Kernel Mapping 可见性的保护策略。

在 `thread_a64.S` 的 `eret_to_el0` 路径中，能够观察到：

- 切换到精简映射下仍可访问的异常向量。
- 切换 User ASID/Mapping。
- 对地址转换状态进行必要同步。
- 最后 `eret` 进入 EL0。

从 EL0 进入异常时，又必须先经过一段保持可映射的安全入口代码，恢复 Kernel Mapping，才能执行完整 Kernel Handler。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这项设计说明：

**用户态不能访问 Kernel 数据，与用户态的页表里是否完全不存在 Kernel 映射，是两个不同层次的隔离要求。**

前者依赖权限检查，后者进一步减少不必要的地址映射暴露。

安全加固时，必须同时考虑两者。

# **15.8 FPU、SIMD、TLS 与 Extended CPU State**

这部分非常重要，因为它体现了一个经常被忽视的事实：

**保存通用寄存器，并不等于保存了程序的全部可观察状态。**

## **15.8.1 为什么 FP/SIMD Context 特别复杂？**

对于 AArch64，FP/SIMD 包括：

```
V0 - V31
FPCR
FPSR
```

假设 TA-A 使用 SIMD 处理一个敏感密钥，然后 TA-B 也使用 SIMD。

如果系统没有正确保存或清理这些寄存器，TA-B 就有可能观察到 TA-A 的中间计算结果。

但是，如果 OP-TEE 每次 SVC、RPC 和中断都完整保存所有 SIMD 寄存器，又会引入明显的性能成本。

这就是 Lazy FP Context Management 的设计动机。

### **典型 Lazy Save 过程**

```
TA-A
   |
   | Uses FP/SIMD
   v
CPU FP Registers owned by TA-A
   |
   | SVC
   v
Kernel
   |
   | Mark TA-A FP state for lazy save
   |
   +-- Kernel does not need FP
   |       |
   |       +-- Avoid full save
   |
   +-- Kernel needs FP
           |
           +-- Materialize TA-A state
           +-- Switch FP ownership
```

这里的 Lazy Save 不表示可以随意丢弃旧状态。

它意味着只在真正需要覆盖寄存器时完成完整保存，同时必须维护正确的所有权。

## **15.8.2 OP-TEE 如何表示这些状态？**

在 Arm 的架构相关结构中，能够看到：

```
/* 教学简化 */
struct thread_user_vfp_state {
    struct vfp_state vfp;
    bool lazy_saved;
    bool saved;
};
```

理解两个标志：

- `lazy_saved`：已进入延迟保存状态。
- `saved`：实际寄存器内容已经被物化到保存区。

状态机可以抽象为：

```
FP State Active
    |
    | Lazy save requested
    v
Lazy Saved
    |
    | Another context needs registers
    v
Fully Saved
    |
    | Original owner resumes
    v
Restored
```

实际的 `thread_user_enable_vfp()`、`thread_user_save_vfp()` 和相关函数负责管理这些状态。

但要注意，某些异常（例如 FP Trap）本身也可能是 Lazy State Management 的组成部分，不能把所有 FP Trap 都认作 TA 程序错误。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **15.8.3 为什么必须区分 Normal World FP State 与 Secure User FP State？**

考虑以下场景：

```
Linux Process
    |
    | Uses SIMD
    v
SMC
    |
    v
OP-TEE
    |
    v
User TA
    |
    | Uses SIMD
    v
RPC
    |
    v
Linux resumes
```

这里涉及两个不同的 FP State Owner：

```
Normal World FP State
Secure User TA FP State
```

如果 OP-TEE 允许 TA 使用 FP/SIMD，那么必须保证 Normal World 原有状态不会被破坏，也不能让 Normal World 获得 TA 的敏感寄存器内容。

因此：

> **Invariant 15-4：FP/SIMD 的状态所有权必须与执行主体同步切换，不能只依赖当前 CPU 的特权级判断寄存器属于谁。**

如果以后实现 RISC-V Vector Extension，这个问题会更加复杂。

因为 RVV 的寄存器状态大小取决于架构配置，例如 VLEN，而且除了 Vector Registers，还有 `vl`、`vtype`、`vstart`、`vcsr` 等状态需要管理。

对于一个支持不同 VLEN 的异构 Hart 平台，是否允许跨 Hart 迁移也必须明确规定，不能假设能够无损恢复。

## **15.8.4 TLS：为什么&#x20;**`TPIDR_EL0`**&#x20;也必须被管理？**

User TA 可能使用 Thread-Local Storage。

AArch64 的 `TPIDR_EL0` 可以被用户态软件用作线程指针。

在 OP-TEE Arm64 的上下文保存中，可以观察到：

```
EL0 → EL1:
    Save TPIDR_EL0

EL1 → EL0:
    Restore TPIDR_EL0
```

例如 `thread_a64.S` 的 `el0_svc` 路径会读取并保存它，而 `thread_resume()` 也会恢复它。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



为什么这很重要？

因为即使 GPR 和 SP 都恢复正确，如果 TLS Pointer 错了，TA 的 C Runtime 仍然可能访问错误的线程局部变量。

更值得注意的是，TLS 的所有权和生命周期不一定等于整个 TA Context 的生命周期。

这需要结合 TA Runtime 的具体线程与执行模型分析。

## **15.8.5 Pointer Authentication 等扩展状态**

在支持的 Arm64 配置中，OP-TEE 还需要管理 Pointer Authentication Keys。

可以在源码中看到：

```
CFG_TA_PAUTH
CFG_CORE_PAUTH
```

对应的上下文切换代码会处理 TA 和 Core 所使用的 Authentication Key。

这里的设计原则是：

**任何影响控制流合法性或访问安全性的架构状态，都应纳入 Context Switch 的安全模型。**

对于新架构适配，必须建立一张完整的 Extended State 清单，明确：

| 状态                  | User 可修改？ | 是否需保存？ | 谁负责初始化？              |
| ------------------- | --------- | ------ | -------------------- |
| TLS Pointer         | 通常可以      | 根据执行模型 | TA Runtime           |
| FP/SIMD Registers   | 配置允许时     | 是      | Context Manager      |
| PAUTH Keys          | 受配置与权限约束  | 是      | Secure Kernel        |
| RISC-V `sstatus.FS` | 受特权规则控制   | 是      | Architecture Backend |
| RISC-V Vector State | 扩展支持时     | 是      | Architecture Backend |

RISC-V 的 `sstatus.FS/VS` 以及可能存在的 Dirty State，仅仅告诉软件某类扩展状态的使用情况；它本身并不会自动保存全部 FP/Vector 寄存器。

# **15.9 Abort、Panic 与异常恢复**

正常返回路径只是 Context Management 的一半。

另一半是：

**当 User TA 无法继续执行时，Kernel 如何安全地收回控制权？**

## **15.9.1 User Abort 与 Kernel Abort 必须区分**

假设 TA 执行：

```
volatile uint32_t *p = (void *)0xdeadbeef;
uint32_t value = *p;
```

如果地址没有合法映射，就可能触发同步 Data Abort。

此时 OP-TEE 不应该直接认为整个 Secure Kernel 必须终止。

需要先判断：

```
Fault occurred
    |
    v
Determine Fault Origin
    |
    +-- User TA
    |      |
    |      +-- Pager can resolve?
    |      |       |
    |      |       +-- Yes: resume execution
    |      |
    |      +-- Unrecoverable
    |              |
    |              +-- Mark TA panic
    |              +-- Unwind to Kernel
    |
    +-- Kernel
           |
           +-- Recoverable kernel fault?
           |
           +-- Otherwise kernel panic
```

OP-TEE 的 `abort.c` 明确区分 User Mode Panic、User FP Trap、Pageable Fault、External Abort 等类别，并对不同类型采用不同处理路径。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **15.9.2 为什么不能简单&#x20;**`longjmp()`**&#x20;回 Kernel？**

这是一个很有价值的设计问题。

假设 User TA 运行过程中发生异常。

我们想直接：

```
longjmp(kernel_context, 1);
```

这样在概念上似乎可以恢复 Kernel C Stack。

但真正需要恢复的不只是 C Stack，还包括：

- Kernel SP。
- 异常返回权限。
- 正确的地址空间。
- FP/SIMD 所有权。
- Interrupt State。
- Architecture-specific Security State。
- 调用链与 Panic 状态。

`longjmp()` 本身并不负责完整的特权级和架构状态转换。

因此 OP-TEE 需要一个受控制的 Unwind Path。

## **15.9.3&#x20;**`thread_unwind_user_mode()`**&#x20;在 Panic 中的作用**

Arm64 的异常处理代码会在确定某个 User Fault 不可恢复时，将返回状态改为指向：

```
thread_unwind_user_mode()
```

并恢复相应的 Kernel SP 与执行状态。

结果相当于：

```
TA crashed
    |
    v
Kernel Exception Handler
    |
    v
Record Panic Information
    |
    v
Prepare Kernel Continuation
    |
    v
thread_unwind_user_mode
    |
    v
user_ta_enter() returns
    |
    v
TA Manager handles TARGET_DEAD
```

`user_ta_enter()` 再检查 TA 的 Panic 状态，向上层返回 `TEE_ERROR_TARGET_DEAD`，并按照资源所有权执行后续清理。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



这里需要区分两个概念。

**Execution Context Unwind：** 让当前 Trusted Thread 从异常的 User Execution 中恢复到可信 Kernel 执行流。

**TA Resource Teardown：** 由 TA Manager 等模块决定如何清理 Session、TA Context、Crypto Handle、Storage Object 和 VM Resources。

它们不能被合并成同一个不分层次的操作。

## **15.9.4 Panic Cleanup 为什么可能出现二次 Fault？**

考虑：

```
TA-A crashes
    |
    v
Kernel begins cleanup
    |
    +-- Close Storage Handles
    +-- Free Crypto Context
    +-- Unmap User Memory
    +-- Release Page Tables
    +-- Destroy TA Context
```

如果清理顺序错误，例如：

```
Free User Mapping
    |
    v
Attempt to read user-side cleanup metadata
```

就可能导致 Kernel Cleanup 过程中再次访问无效地址。

因此：

> **Invariant 15-5：任何会在异常退出后继续使用的资源，都必须在对应的引用关系和权限仍然有效时完成访问或复制。**

对于 Staff Engineer，这意味着不能只审查成功路径，还需要审查每一个可能发生 Panic、OOM、Cancellation 或 RPC Failure 的位置。

# **15.10 RISC-V RV64：如何实现同样的 Context Management？**

这部分和你的 RISC-V OP-TEE 研究可以直接连接起来。

需要特别指出：OP-TEE 4.11.0 中已经有实际的 RISC-V Thread、Trap、Syscall 和用户态切换实现。本节不是从零虚构一个 RISC-V Backend，而是对照这些实现理解架构抽象。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



## **15.10.1 Arm64 与 RV64 的核心对应关系**

| 功能                       | Arm64                | RISC-V RV64     |
| ------------------------ | -------------------- | --------------- |
| Kernel/User Privilege    | EL1 / EL0            | S-mode / U-mode |
| User Trap                | SVC                  | ECALL           |
| Exception Return         | ERET                 | SRET            |
| Return PC                | ELR_EL1              | sepc            |
| Saved Status             | SPSR_EL1             | sstatus         |
| Trap Cause               | ESR_EL1              | scause          |
| Trap Vector              | VBAR_EL1             | stvec           |
| Trap Scratch             | SP_EL1/Core-local 机制 | sscratch 等      |
| User Address Translation | TTBR / ASID          | satp / ASID     |
| TLB Synchronization      | TLBI                 | SFENCE.VMA      |

这里的 RV64 对应关系假设 OP-TEE 运行在具有 S-mode Trap 处理能力的执行域。具体 Domain 切换、保护机制及更高特权级软件，需要以平台 Firmware Contract 为准。

## **15.10.2 一个非常有价值的源码入口：**`thread_trap_vect`

在 RISC-V 的：

`core/arch/riscv/kernel/thread_rv.S`

中，Trap Vector 开头使用了基于 Scratch CSR 与 `tp` 的交换机制。

源码逻辑可概括为：

```
# 原理示意
csrrw tp, sscratch, tp
```

这里的 `sscratch` 用于说明普通 S-mode 场景；上游采用的是 `CSR_XSCRATCH` 抽象。

为什么要这样做？

因为 RISC-V 不像 AArch64 那样具备完全相同的硬件栈选择机制。

当 Trap 从 User Mode 进入 Kernel 时，Kernel 不能直接信任当前 `sp`。

所以必须提前安排一个可信的方式，获得 Kernel Per-Hart Data 和可信栈。

在上游的实现中，Trap Entry 会利用 Scratch CSR 以及 `tp` 的交换结果，区分来自 User Mode 和 Kernel Mode 的 Trap，然后进入不同路径。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这项设计依赖严格的入口不变量：

```
Kernel Trap Entry:
    Must identify trusted Per-Hart State

User Trap Entry:
    Must recover trusted Per-Hart State
    Must preserve User TP
```

如果初始化或者恢复逻辑破坏了 Scratch CSR 的约定，Trap Handler 可能连可信栈都无法定位。

## **15.10.3 为什么 RISC-V 必须保存&#x20;**`gp`**&#x20;和&#x20;**`tp`**？**

对于普通 RISC-V C 代码：

```
gp = Global Pointer
tp = Thread Pointer
```

两者都是编译器和 ABI 可能依赖的重要寄存器。

当 User TA 触发 ECALL 时：

```
User gp != Kernel gp
User tp != Kernel Per-Hart Pointer
```

因此 Trap Handler 需要在正确的时机：

- 保存 User `gp` 和 `tp`。
- 恢复 Kernel `gp`。
- 建立可信的 Per-Hart Context。
- 使用 Kernel Stack 运行 C Handler。
- 返回 User Mode 时恢复用户的寄存器值。

上游 `trap_from_user` 中，能够看到保存用户 `gp/tp` 并恢复内核 `__global_pointer$` 的路径。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这也是 RISC-V Porting 中经常出现“汇编看起来正常，一进入 C 函数就崩溃”的原因之一。

如果 Kernel `gp` 错误，编译器生成的 Global Data 访问就可能读取错误地址。

## **15.10.4 RISC-V ECALL 与 AArch64 SVC 的返回 PC 差异**

这是一个需要在汇编级别牢记的语义区别。

在 RISC-V 中，ECALL Trap 保存的 `sepc` 指向 ECALL 指令本身。

对于成功处理且需要继续返回 User Mode 的普通 ECALL，Handler 必须推进 PC。

在当前 OP-TEE RISC-V 源码中：

```
/* 摘录关键操作 */
regs->epc += 4;
```

可以在 `thread_arch.c` 的 `thread_scall_handler()` 中看到这一步。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



而 AArch64 SVC 的异常返回 PC 通常已经指向 SVC 后的下一条指令，因此不能机械复制相同的 PC 更新逻辑。

如果移植代码没有推进 RISC-V `sepc`：

```
ECALL
  |
  v
Trap Handler
  |
  v
SRET
  |
  v
Same ECALL
```

就可能陷入无限 Trap Loop。

反过来，如果在 AArch64 上错误地额外推进 ELR，也可能跳过用户程序的合法指令。

**这体现了 Architecture Abstraction 的一个核心原则：对上层保持相同的 Syscall 语义，底层可以采用完全不同的硬件返回机制。**

## **15.10.5 RISC-V 的 User Entry 与 Kernel Unwind**

RISC-V 的 `thread_arch.c` 中，`thread_enter_user_mode()` 会准备：

```
User Entry Arguments
User SP
User Entry PC
Target Privilege State
Interrupt Enable State
```

随后调用：

```
__thread_enter_user_mode()
```

底层汇编：

```
Save Kernel Return Address
Save Kernel Callee-saved Registers
Save Kernel SP
Prepare Scratch CSR
Restore User Registers
Set Exception Return CSR State
SRET
```

而正常结束 User Execution 后，`thread_unwind_user_mode()` 负责恢复此前的 Kernel Continuation。

这与 Arm64 的设计理念非常相似，但寄存器及具体指令不同。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **15.10.6 RISC-V Domain Switch 不等于 User/Kernel Switch**

这点与你正在研究的 RISC-V Secure Domain 很有关。

考虑两个不同的操作：

```
S-mode → U-mode
```

以及：

```
Normal Domain → Secure Domain
```

它们不一定由同一个硬件机制完成。

前者是 Privilege Transition，后者是 Security Domain Transition。

在某些 RISC-V TEE 架构中，Domain Switch 还可能涉及：

- Security Domain Identity。
- Memory Protection Table。
- PMP 或系统级访问保护。
- Device Access Permissions。
- Interrupt Ownership。
- Domain-specific Confidential State。

因此，如果 OP-TEE 在一个独立 Secure Domain 中运行，需要明确区分两层 Context Management。

**Normal Domain**

Linux / Host Execution

**Firmware / Domain Switching Layer**

Save/Restore Domain State · Security Isolation

OP-TEE Secure Domain

**S-mode Secure Kernel**

Trusted Thread · Kernel Context

**U-mode User TA**

User Context · User Page Tables

这里存在两组不同的不变量。

**Domain Switch Invariant：** 不允许 Normal Domain 获得 Secure Domain 的机密资源。

**User/Kernel Invariant：** 不允许 Secure User TA 越权访问 Secure Kernel 或其他 TA 的资源。

即使 Domain Switch 实现完全正确，如果 OP-TEE 自身的 U/S 隔离存在缺陷，Secure Domain 内部仍然可能被恶意 TA 攻击。

同样，OP-TEE 内部隔离再完善，也无法补救底层 Domain Isolation 的根本性缺陷。

# **15.11 Change Impact Analysis：修改 Context Management 时如何评估风险？**

现在把本章知识转化成实际工程能力。

假设要修改：

```
thread_enter_user_mode()
```

为 RISC-V 增加某种新的扩展寄存器保存支持。

只修改一个函数显然不够。

必须分析下面的影响矩阵。

| 影响维度       | 应评估的问题                    | 验证方向           |
| ---------- | ------------------------- | -------------- |
| User Entry | 新寄存器是否正确初始化？              | 新 TA 第一次进入     |
| Syscall    | ECALL 是否破坏扩展状态？           | 高频 Syscall     |
| Nested TA  | TA-A/B 是否出现寄存器串扰？         | 嵌套调用           |
| RPC        | Suspend 是否保存完整状态？         | RPC 前后比较       |
| SMP        | 跨 Hart 恢复是否完整？            | 并发、多核          |
| Fault      | 异常路径是否能保存状态？              | Abort/Panic    |
| MMU        | 新状态是否影响地址转换？              | User Mapping   |
| Interrupt  | Interrupt Handler 是否覆盖状态？ | IRQ Stress     |
| Cleanup    | 敏感状态是否在释放时清理？             | TA Close/Panic |
| ABI        | 是否破坏旧的汇编布局与结构体偏移？         | 全配置构建          |

特别要关注：

**C 结构体 Layout 和 Assembly Offset 的一致性。**

例如 `struct thread_ctx_regs` 增加字段之后：

```
C struct layout changes
    |
    v
Generated Assembly Offsets change
    |
    v
Assembly Save/Restore changes
    |
    v
Context Switch Behavior changes
```

如果某个 Assembly Offset 没有同步更新，就可能出现非常隐蔽的寄存器覆盖。

这类错误甚至可能只在特定编译器配置或开启某个安全扩展后才出现。

## **五个必须维持的核心不变量**

| ID     | 不变量                                                  |
| ------ | ---------------------------------------------------- |
| INV-C1 | Resume PC、Privilege State、Stack 必须属于合法的恢复目标          |
| INV-C2 | 当前 Session Identity 必须与有效的 User Context/权限保持一致       |
| INV-C3 | 任何 Extended State 都不能在不同安全主体之间泄漏                     |
| INV-C4 | Suspend/Resume 不得造成丢失、重复恢复或并发运行同一 Thread             |
| INV-C5 | 异常退出不得保留悬空引用、错误 Page Tables 或失效的 Kernel Continuation |

这些不变量应该成为相关 Patch Review 的主要审查标准。

# **15.12 Hands-on Lab：真正观察一次 Context Switch**

我建议做三个递进实验。

## **Lab A — Arm64 User/Kernel Transition**

目标：通过 GDB 看到 TA 从 EL1 进入 EL0，再通过 SVC 返回 EL1。

推荐使用上游 Arm64 QEMU 环境，开启必要的 Debug Symbols。

在 GDB 中设置断点：

```
break user_ta_enter
break thread_enter_user_mode
break __thread_enter_user_mode
break thread_scall_handler
break scall_handle_user_ta
break thread_unwind_user_mode
```

注意，架构汇编符号是否能直接设置断点取决于构建的符号信息。

关键观察：

```
info registers
p/x $pc
p/x $sp
p/x $elr_el1
p/x $spsr_el1
p/x $sp_el0
p/x $sp_el1
```

具体系统寄存器能否直接显示，取决于所用 QEMU/GDB 的寄存器描述支持。

需要记录四个时刻：

| 时刻           | Current EL | SP 所有权        | PC 所属   |
| ------------ | ---------- | ------------- | ------- |
| Enter User 前 | EL1        | Kernel Thread | Kernel  |
| ERET 之后      | EL0        | User TA       | User TA |
| SVC Handler  | EL1        | Kernel Thread | Kernel  |
| SVC Return 后 | EL0        | User TA       | User TA |

重点不是记录寄存器值，而是解释为什么它们发生变化。

## **Lab B — Nested TA Context Switch**

构造：

```
CA → TA-A → TA-B → TA-A → CA
```

在以下位置打断点：

```
break ts_push_current_session
break ts_pop_current_session
break vm_set_ctx
break thread_enter_user_mode
```

观察：

```
Current Trusted Thread ID
Current Session
Current User Context
User ASID
Kernel SP
User SP
```

需要证明：

1. 同一个 Trusted Thread 可以跨越多个 TA Context。
2. Session Stack 随嵌套调用而变化。
3. User Mapping 与当前有效的 User Context 一致。
4. 从 TA-B 返回后，TA-A 的 Kernel Continuation 和 User Mapping 均正确恢复。

这个实验非常适合验证你对本章核心概念的理解。

## **Lab C — RPC Suspend/Resume**

构造一个会触发 REE FS RPC 的 TA 请求。

在以下位置设置断点：

```
break thread_state_suspend
break thread_resume_from_rpc
break thread_resume
```

对每个暂停/恢复事件记录：

```
Thread ID
CPU ID
Thread State
Saved PC
Saved SP
Have User Map
Active User Map
Current Session
```

如果在 QEMU SMP 配置中能稳定复现跨 CPU Resume，更值得进一步观察。

完成实验后，尝试回答：

**假如我把&#x20;**`thread_resume_from_rpc()`**&#x20;中恢复 User Mapping 的操作移动到&#x20;**`thread_resume()`**&#x20;之后，究竟会出现什么问题？**

注意 `thread_resume()` 通常不返回，因此这是一个机制分析题，而不是建议实际提交这种修改。

# **15.13 Staff Engineer Assessment — 高难度综合考察**

下面的题目将不局限于 Context Management 的局部源码。

它们会扩展到安全架构、虚拟化、异构多核、内存管理、Firmware Interface 和性能优化，但主题始终围绕 **Context Ownership、Transition Correctness 与 State Isolation**。

建议先独立作答，再展开参考分析。

10 道 Staff Engineer 深度考核题

展开所有解析

01

Very Hard

**问题 1：跨 Hart 恢复中的隐蔽状态丢失**\
一个 RV64 SoC 有 16 个 Hart。TA-A 在 Hart 0 执行过程中发起 RPC，恢复时调度到了 Hart 7。大部分测试通过，但开启 FP 或 Vector 后偶尔出现密码学结果错误。你作为架构负责人，如何证明究竟遗漏了什么状态？

查看参考解析

02

Very Hard

**问题 2：TA-A 调用 TA-B 后，返回地址正确却仍然崩溃**\
CA → TA-A → TA-B 的调用链中，TA-B 正常返回，GDB 显示 TA-A 的 PC 和 SP 都符合预期，但恢复执行后读取了 TA-B 的私有数据。为什么保存寄存器没有解决这个问题？

查看参考解析

03

Hard

**问题 3：能否让 OP-TEE 完全省略 GPR Save/Restore 来提高性能？**\
团队提出：由于 TA 的 Syscall 遵守标准 ABI，只需保留 Callee-saved Registers，所有 Caller-saved Registers 都无需保存。这样能显著降低 SVC 开销。你是否接受？

查看参考解析

04

Very Hard

**问题 4：Normal World 能否伪造一个 RPC Resume？**\
恶意 Linux Kernel 反复发送某个历史 Thread ID 的 Resume 请求，并尝试替换返回参数。在支持多个 Guest 的系统中，你如何设计可验证的安全协议？

查看参考解析

05

Very Hard

**问题 5：Lazy FP State 为什么可能导致跨 TA 的密钥泄漏？**\
TA-A 用 SIMD 处理机密密钥，之后 TA-B 第一次使用 SIMD。一个性能优化 Patch 去掉了新 Context 的扩展寄存器清零，但保留了 Lazy Save。你如何分析风险？

查看参考解析

06

Research Level

**问题 6：Secure Domain Switch 与 User/Kernel Switch 能合并吗？**\
RISC-V SoC 使用独立 Secure Domain 运行 OP-TEE。硬件团队希望 M-mode Firmware 直接完成 Normal Domain → Secure User TA 的一次性切换，以避免两次特权转换。你如何评估？

查看参考解析

07

Very Hard

**问题 7：单实例 TA 的并发安全与 Context Switch 有什么关系？**\
两个 Trusted Thread 在不同 CPU 上同时执行同一个 Single Instance TA。团队认为只要每个 Thread 保存独立的 GPR 和 SP，就不会发生冲突。你怎么看？

查看参考解析

08

Hard

**问题 8：User TA 异常后，为什么不能立即释放所有 TA 页面？**\
你希望优化 TA Crash 清理流程，在 Abort Handler 中直接释放 TA 的整个 User VM，随后再返回 TA Manager。看似能够尽早回收内存，这项修改有什么风险？

查看参考解析

09

Research Level

**问题 9：新增 CPU 扩展功能时，如何做一个可被社区接受的 Context Management Patch？**\
准备为 RISC-V OP-TEE 增加某项新的用户态可见 ISA 扩展。当前没有通用的 Extended Context Save/Restore 接口。请给出可上游化的设计方案。

查看参考解析

10

Research Level

**问题 10：如何形式化描述一次 Context Switch 的正确性？**\
不允许只依赖 xtest 通过。你需要给出一组可用于代码审查、模型检查甚至形式化验证的 Context Switch Invariants，并说明如何证明它们成立。

查看参考解析

### **如何评价这些题目？**

这十道题没有唯一的标准实现答案。

真正的优秀回答，需要体现以下四种能力：

**Architecture Reasoning：** 能从设计目标出发，判断某个优化为什么可行或者不可行。

**State Ownership：** 能识别所有相关状态，以及它们在时间和执行主体之间的所有权变化。

**Security Reasoning：** 能明确指出可能被破坏的安全边界，并提供不变量证明。

**Verification Strategy：** 能设计证伪自己方案的测试，而不是仅寻找支持自己方案的测试。

如果一个候选人只是知道 `thread_ctx_regs` 保存了什么、`ERET` 执行什么、`SRET` 修改了什么权限级别，这只能说明他了解架构知识。

如果能针对跨核、异构扩展、Lazy Save、Nested TA、RPC、Memory Context 和 Domain Isolation 提出完整的设计与验证方案，才真正体现出 Staff Engineer 的系统思考能力。

# **本章最终总结**

回顾整个 Chapter 15，我希望你建立五个核心认识。

**第一，OP-TEE 的 Context 不等于 CPU Registers。**

它是 CPU Execution、Trusted Thread、TA Context、Session、User VM 和 Extended State 的组合。

**第二，Kernel/User Transition 不等于 Trusted Thread Switch。**

一次普通 SVC 可以维持相同 Trusted Thread 和 TA Context；一次 RPC 可以暂停 Trusted Thread，却不销毁 TA。

**第三，Context Management 的核心不是保存，而是正确的恢复。**

保存一组寄存器可能只是若干条汇编指令。真正复杂的是证明恢复后权限、地址空间、用户身份、栈和扩展状态仍然一致。

**第四，异常路径比正常路径更容易暴露设计缺陷。**

包括 RPC Resume、TA Panic、跨核恢复、Nested TA、Foreign Interrupt 和 Lazy FP State。

**第五，架构移植必须先定义统一的 Context Contract，再实现架构特定的寄存器切换。**

对于 RISC-V OP-TEE，既要理解 S/U Privilege Transition，也要理解底层 Secure Domain Switching 与 OP-TEE 内部 Context Management 之间的职责边界。

最后用一个公式概括本章最重要的设计思想：

\\[ \begin{aligned} \text{Correct Resume} ={}& \text{Execution State}\\\ &+\text{Memory State}\\\ &+\text{Security Identity}\\\ &+\text{Extended State}\\\ &+\text{Ownership Consistency} \end{aligned} \\]

它不是简单的加法关系，而是表示以上条件必须**同时成立**。

只要其中任何一个条件不成立，即使 CPU 已经恢复到了正确的 PC，系统仍然可能处于错误或不安全的状态。

### **推荐继续阅读的核心源码**

- [Arm64 Thread Entry / Resume / Syscall Assembly](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread_a64.S?utm_source=chatgpt.com)
- [Arm Thread Context 与执行状态管理](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread.c?utm_source=chatgpt.com)
- [User TA Runtime Execution](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/user_ta.c?utm_source=chatgpt.com)
- [Session Stack 与 Current Context](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/ts_manager.c?utm_source=chatgpt.com)
- [User VM Context Switch](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/mm/vm.c?utm_source=chatgpt.com)
- [RISC-V Trap、Thread Resume 与 User Mode Assembly](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/riscv/kernel/thread_rv.S?utm_source=chatgpt.com)

**本章的真正毕业标准是：给你一次任意的 OP-TEE 上下文切换，你不仅能说出它保存了什么，还能准确解释它为什么保存、由谁拥有、如何恢复、哪些状态不能恢复，以及如何证明这个切换不会跨越错误的安全边界。**
