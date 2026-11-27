# **Chapter 14 — OP-TEE Trusted Thread：执行模型、生命周期、SMP 与调度机制**

OP-TEE OS Internals · Advanced / Staff Engineer Level

**源码基线：OP-TEE OS 4.11.0（2026-10-16）**

这一章是理解 OP-TEE OS 内核架构最重要的章节之一。

如果将 OP-TEE 看成一个完整操作系统，那么 Memory Management 解决的是资源隔离问题，TA Manager 解决的是可信应用管理问题，而 **Trusted Thread 解决的是整个安全内核如何承载执行流，以及如何在 Normal World 和 Secure World 之间保存、暂停、恢复执行流的问题。**

我们将从系统设计出发，逐步深入 C、Assembly、数据结构、状态机、多核并发和实际故障场景。

## **本章学习目标**

完成本章后，你应该具备以下能力：

1. 解释 OP-TEE 为什么需要 Trusted Thread，却不需要传统 RTOS 风格的完整线程调度器。
2. 清楚区分 Linux Task、OP-TEE Trusted Thread、CPU Core、TA Context、TA Session。
3. 解释 `CFG_NUM_THREADS` 的真正含义，以及它与 CPU 数量、TA 数量和 Linux 并发数之间的关系。
4. 从 `OPTEE_SMC_CALL_WITH_ARG` 开始，沿源码追踪 Trusted Thread 的分配、执行、RPC Suspend、Resume 和最终释放。
5. 深入理解 Thread Context 保存的内容、Thread Stack 与 Entry Stack 的区别。
6. 解释 SMP 情况下一个 Trusted Thread 如何与 CPU Core 关联，以及哪些状态必须是 per-CPU，哪些必须是 per-thread。
7. 判断 Thread Exhaustion、RPC Deadlock、Context Corruption 等故障背后的根本原因。
8. 在修改 Thread Framework 时，提前识别内存管理、TA Runtime、RPC、异常处理、锁和架构 Backend 的影响范围。

本章还会特别关注三个问题：

**Who executes? Who schedules? Who owns the execution context?**

理解这三个问题，才能真正理解 OP-TEE 的执行模型。

# **14.1 设计理念：为什么 OP-TEE 需要 Trusted Thread？**

先从一个操作系统最基本的问题开始。

假设 Linux 中的一个 Client Application 发起：

```
TEEC_InvokeCommand(&session, cmd_id, params, &origin);
```

它希望 OP-TEE 执行某项安全服务。

这个服务可能是：

- 一次 AES-GCM 加解密。
- 一次密钥签名。
- 一次 Secure Storage 读取。
- 一次 User TA 加载。
- 一次需要 Hardware Interrupt 的安全外设操作。

这些操作可能耗时很长，也可能中途需要 Normal World 帮忙。

例如 Secure Storage：

```
Linux Application
       |
       v
InvokeCommand()
       |
       v
OP-TEE
       |
       v
Secure Storage
       |
       v
需要 Linux 文件服务
       |
       v
tee-supplicant
       |
       v
Linux Filesystem
       |
       v
返回 OP-TEE
       |
       v
继续 Secure Storage
       |
       v
完成 InvokeCommand()
```

这里存在一个非常重要的矛盾：

**OP-TEE 的安全服务可能尚未执行完毕，但 CPU 已经需要离开 Secure World。**

如果离开时不保存执行上下文，那么之前的 C 函数调用栈、局部变量、寄存器以及 TA 的执行状态都会丢失。

因此，必须引入一种抽象：

> Trusted Thread 表示一段尚未完成的安全服务执行上下文。即使暂时离开 Secure World，它依然可以保留自己的执行状态，并在适当的时候继续运行。

这就是 Trusted Thread 最核心的设计动机。

但它与 Linux Task 或 NuttX TCB 又存在本质区别。

## **14.1.1 OP-TEE 的线程模型与传统 OS 的区别**

| 对比维度      | Linux / NuttX    | 经典 OP-TEE Trusted Thread         |
| --------- | ---------------- | -------------------------------- |
| 创建目的      | 承载独立的程序执行任务      | 承载一项安全服务请求                       |
| 调度机制      | OS Scheduler     | 主要随 Normal World 调用方调度           |
| 时间片       | 可支持抢占式时间片调度      | 不提供通用 Thread 时间片调度               |
| Run Queue | 通常存在             | 没有对应的通用 Trusted Thread Run Queue |
| 生命周期      | 从创建到退出           | 从请求分配到服务完成                       |
| 阻塞方式      | 内核等待队列、调度其他线程    | 典型等待通过 RPC 让出 CPU                |
| 上下文切换     | Task → Task      | Secure/Normal World 交接、暂停与恢复     |
| 并行执行      | 多 CPU 并行         | 多 CPU 可并行执行 Trusted Thread       |
| 调度实体      | Kernel Scheduler | Normal World 的调度与调用路径            |

这里的结论适用于传统 GP TEE yielding-call 执行模型；Secure Partition 的调度语义需要结合具体 FF-A/SPMC 配置另外分析。

上游官方文档直接说明：OP-TEE 传统 Trusted Thread 不实现独立的线程调度器，其执行跟随发起请求的 Normal World 线程，多个 Trusted Thread 可以在 SMP 系统中并行运行。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **14.1.2 为什么不直接在 OP-TEE 中实现一个完整调度器？**

理论上当然可以。

例如设计：

```
Secure Scheduler
    |
    +-- Trusted Thread 0
    +-- Trusted Thread 1
    +-- Trusted Thread 2
    +-- Trusted Thread 3
```

那么还需要：

- Secure Timer 和 Time Slice。
- Runnable / Blocked / Sleeping Queue。
- Priority Scheduling。
- Wakeup、Preemption、Timer Tick。
- CPU Affinity、Load Balancing。
- 与 Linux Scheduler 的调度协调。
- 更多的 Context Switch、同步和状态管理。

这将显著扩大 OP-TEE Core 的复杂度和 TCB。

而传统 OP-TEE 有一个天然的外部执行驱动：**Normal World 发起安全服务请求，OP-TEE 处理请求并返回。**

因此，可以选择一种更简单的模型：

```
Linux Scheduler
      |
      v
Linux Task A
      |
      v
Secure Call
      |
      v
OP-TEE Trusted Thread
      |
      v
Complete / Yield
      |
      v
Linux Scheduler
```

Linux 已经承担了客户端线程的调度，因此 OP-TEE 没必要再为这些服务请求建立一套完全独立的通用调度体系。

这种设计有三个直接好处：

- **减少 TCB：** 不需要完整的安全内核调度策略和相关实现。
- **减少调度协调：** 避免在 Secure World 和 Normal World 之间引入两套相互竞争的普通任务调度器。
- **降低内存成本：** 通过有限数量的 Trusted Thread Context 承载并发服务，而不是为每个 TA Session 常驻一个 Kernel Thread。

但代价同样明确：

**安全服务的执行进展，与 Normal World 的调度和 RPC 响应能力存在耦合。**

这意味着 OP-TEE 能保护机密性和完整性，并不等于它天然能够保证 Normal World 不配合时的服务可用性。

这是后面分析 DoS、Thread Exhaustion 和安全实时性时非常重要的前提。

# **14.2 首先分清五种不同的执行与资源管理对象**

在深入源码前，必须把下面五个概念彻底分开。

| 对象              | 管理者                    | 本质                 |
| --------------- | ---------------------- | ------------------ |
| Linux Task      | Linux Kernel           | Normal World 的调度实体 |
| CPU Core / Hart | Hardware + OS/Firmware | 实际执行指令的物理处理器       |
| Trusted Thread  | OP-TEE Core            | 可挂起、恢复的安全执行上下文     |
| TA Context      | OP-TEE TA Manager      | 一个可信应用的运行时实例       |
| TA Session      | OP-TEE TA Manager      | 客户端与 TA 的逻辑会话      |

这几个对象没有简单的一一对应关系。

## **14.2.1 Trusted Thread 不等于 CPU Core**

假设：

```
CPU:                 4 cores
CFG_NUM_THREADS:     8
User TA Instances:   3
TA Sessions:        20
Linux Tasks:        40
```

是否说明可以同时执行八个 OP-TEE Trusted Thread？

不能。

因为一个 CPU Core 在任一瞬间只能执行一个普通的指令流。假设没有 SMT，并且这四个 CPU 都可用，那么同一时刻最多只有四个 Trusted Thread 能够真正并行执行。

但八个 Trusted Thread Context 可以同时被占用，其中一部分可能是 `SUSPENDED`。

例如：

示例：4 CPUs / 8 Trusted Thread Slots

CPU 0

Thread 0

Executing

CPU 1

Thread 1

Executing

CPU 2

Thread 2

Executing

CPU 3

Thread 3

Executing

Thread Pool — 其余 4 个 Slot

T4

Suspended

T5

Suspended

T6

Free

T7

Free

说明性快照，非真实运行数据。此时 6 个 Thread Slot 被占用，其中 4 个正在执行、2 个挂起；还有 2 个可供新请求分配。

这里有两个不同的容量概念：

\\[ N\_{\text{executing}} \leq \min(N\_{\text{CPU}},N\_{\text{threads}}) \\]

以及：

\\[ N\_{\text{occupied}}=N\_{\text{active}}+N\_{\text{suspended}} \\]

关键结论：

**CPU 数量决定并行执行上限，而 Trusted Thread Pool 大小决定能够同时保留多少个未完成的请求上下文。**

这也是为什么单核 OP-TEE 仍然有理由配置多个 Trusted Thread。

## **14.2.2 Trusted Thread 不等于 TA Context**

假设存在一个 Single Instance TA：

```
User TA: crypto_service
    |
    +-- TA Context A
           |
           +-- Session 1
           +-- Session 2
           +-- Session 3
```

可能发生：

```
Linux Task A
    |
    +-- Trusted Thread 0
            |
            +-- TA Context A / Session 1

Linux Task B
    |
    +-- Trusted Thread 1
            |
            +-- TA Context A / Session 2
```

两个 Trusted Thread 可以引用相同 TA Context，但执行并发是否真正进入 TA、哪些调用需要串行化，仍取决于 TA Flags 和 TA Manager 的执行约束。

因此：

- Thread 代表当前执行流。
- TA Context 代表 Trusted Application Instance。
- Session 代表客户端的逻辑服务关系。

它们是三种独立的生命周期。

一次 TA Session 可以持续很久，其中不同的 InvokeCommand 请求并不需要永久绑定同一个 Trusted Thread ID。

反过来，同一次 Trusted Thread 执行也可能因嵌套服务调用而经过多个 TA Session。

## **14.2.3 Trusted Thread 与 Linux Task 也不是永久绑定**

官方文档使用 Linux 调度 Trusted Thread 来描述这个模型，但这里不能误解成：

```
Linux PID 100 -> 永久绑定 Trusted Thread 0
Linux PID 101 -> 永久绑定 Trusted Thread 1
```

实际关系更接近：

```
Linux Task 100
    |
    +-- Call A -> Trusted Thread 2 -> Complete
    |
    +-- Call B -> Trusted Thread 5 -> Complete
```

每次新的 Yielding Call，可以分配不同的空闲 Thread Slot。

只有当某个请求尚未完成，并且该请求需要从 Suspend 状态继续执行时，才必须找到原先保存的 Trusted Thread Context。

所以这里有两个时间尺度：

- **一次 Invocation 内：** 必须保持正确的执行上下文连续性。
- **多次 Invocation 之间：** 不保证 Trusted Thread ID 不变。

这一点对 Thread Local Storage、Session Context 和 RPC Cache 的设计非常重要。

# **14.3 从源码理解 Trusted Thread 的数据结构**

这一部分采用 4.11.0 上游实际定义。

主要源码：

- [core/include/kernel/thread_private.h](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/include/kernel/thread_private.h?utm_source=chatgpt.com)
- [core/kernel/thread.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/thread.c?utm_source=chatgpt.com)
- [Arm thread_arch.h](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/include/kernel/thread_arch.h?utm_source=chatgpt.com)

需要注意一个演进细节：在 4.11.0 中，Thread 的通用数据结构、线程池初始化、栈管理和部分公共基础设施已经集中在 `core/kernel/thread.c` 中，并不是所有实现都放在 Arm 的 `thread.c`。

这对 RISC-V Porting 非常有意义，因为不应该把全部 Thread Management 复制成一套 RISC-V 私有实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **14.3.1&#x20;**`struct thread_ctx`**：每个 Trusted Thread 的运行状态**

以下是根据 4.11.0 的真实字段整理的简化结构示意，省略条件编译细节：

```
struct thread_ctx {
    struct thread_ctx_regs regs;

    enum thread_state state;

    vaddr_t stack_va_end;
    uint32_t flags;

    struct core_mmu_user_map user_map;
    bool have_user_map;

    vaddr_t kern_sp; /* RV64 / ARM64 */

    /* Optional architecture state */

    void *rpc_arg;
    struct mobj *rpc_mobj;

    struct thread_shm_cache shm_cache;
    struct thread_specific_data tsd;
};
```

这里最重要的不是记住字段，而是理解为什么它们必须属于同一个对象。

| 字段                     | 用途                             | 为什么需要                          |
| ---------------------- | ------------------------------ | ------------------------------ |
| `regs`                 | 保存恢复执行所需的 CPU Register Context | 离开 Secure World 后仍可继续执行        |
| `state`                | Free / Active / Suspended      | 防止重复分配及错误 Resume               |
| `stack_va_end`         | Trusted Thread Kernel Stack    | 保留 C 函数调用链                     |
| `flags`                | 记录退出/恢复等行为                     | 区分 RPC 与 Foreign Interrupt 等路径 |
| `user_map`             | 保存 User Address Space Mapping  | 恢复正确的 TA 映射                    |
| `have_user_map`        | 指示是否保存用户映射                     | 避免无条件恢复错误的地址空间                 |
| `kern_sp`              | User TA 执行期间保存 Kernel SP       | 从用户态异常返回 Kernel 时继续使用正确栈       |
| `rpc_arg` / `rpc_mobj` | RPC 相关内存对象                     | 管理跨世界通信资源                      |
| `shm_cache`            | Thread 相关 Shared Memory Cache  | 减少重复分配开销                       |
| `tsd`                  | Thread Specific Data           | 保存服务执行相关的逻辑上下文                 |

这里可以提炼出一个通用架构原则：

> 任何需要在 Suspend → Resume 之间继续存在的状态，不能仅保存在当前 CPU 的临时寄存器或临时栈中。

否则，CPU 一旦离开 Secure World，后续便无法保证该状态仍然有效。

### **特别研究&#x20;**`thread_specific_data`

4.11.0 中，`thread_specific_data` 包含：

```
struct thread_specific_data {
    TAILQ_HEAD(, ts_session) sess_stack;
    struct ts_ctx *ctx;

    /* Abort information */
    /* Syscall recursion */
    /* Optional tracing / fault mitigation state */
};
```

其中最值得研究的是：

```
sess_stack
```

为什么一个 Thread 需要 Session Stack，而不仅仅是一个当前 Session 指针？

因为实际执行过程中可能出现：

```
CA -> TA-A
        |
        +-> TA-B
                |
                +-> PTA
```

这种嵌套调用并没有简单地结束上一个 Session，而是需要保留上层调用关系，以便从 PTA 返回 TA-B，再返回 TA-A。

因此，Thread 的执行上下文不仅包含 CPU Registers，还包含当前的可信服务调用链。

`thread_get_tsd()` 在上游的公共实现中，正是通过当前 Thread ID 找到 `threads[id].tsd`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **14.3.2&#x20;**`struct thread_core_local`**：每个 CPU 的本地状态**

这是另一个非常关键的数据结构。

简化后的 Arm 版本：

```
struct thread_core_local {
    /* architecture scratch registers */

    vaddr_t tmp_stack_va_end;
    long kcode_offset;

    short int curr_thread;
    uint32_t flags;

    vaddr_t abt_stack_va_end;

    /* optional debug / architecture fields */
};
```

最重要的字段是：

```
curr_thread
```

它表示 **这个 CPU 当前正在执行哪一个 Trusted Thread**。

例如：

```
CPU 0:
    core_local[0].curr_thread = 3

CPU 1:
    core_local[1].curr_thread = 6

CPU 2:
    core_local[2].curr_thread = -1
```

第三个 CPU 可能正在 OP-TEE 的临时入口上下文中，而没有绑定到普通 Trusted Thread。

这里要记住：

`thread_core_local`**&#x20;是 CPU-local 的，**`thread_ctx`**&#x20;是执行流自身的状态。**

二者的关系是动态的。

OP-TEE 的公共 `thread_get_core_local()` 会根据当前 Core Position 找到对应的 CPU-local 对象；源码还明确要求在访问它时满足 Foreign Interrupt Masking 约束，防止在获取 CPU-local 地址过程中发生不安全的迁移。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **14.3.3 Per-CPU 和 Per-Thread 为什么必须分离？**

考虑这样一个场景：

```
Time T0:

CPU 0 executes Trusted Thread 2

core_local[0].curr_thread = 2
```

随后 Thread 2 需要执行 RPC：

```
Time T1:

Thread 2 -> SUSPENDED

CPU 0 -> Normal World

core_local[0].curr_thread = INVALID
```

后来它被另一个 CPU 恢复：

```
Time T2:

CPU 1 resumes Trusted Thread 2

core_local[1].curr_thread = 2
```

这里的 CPU 迁移是执行模型允许的一种情况；是否能实际迁移，还取决于平台、传输层和所有架构上下文的保存恢复是否完整。

如果把 Thread 2 的寄存器状态、用户映射、Session Stack 全部保存在 `core_local[0]` 中，那么它在 CPU 1 上将无法正确恢复。

因此：

- `thread_ctx` 中保存可随执行流迁移的状态。
- `thread_core_local` 中保存某个 CPU 当前入口、异常栈以及正在执行哪个 Thread 等状态。
- 必须通过严格的 Suspend/Resume 协议连接两者。

这个设计与 Linux `task_struct` 和 per-CPU data 的思想有相似性，但不能简单认为两者的调度模型相同。

# **14.4 Trusted Thread 状态机**

OP-TEE 4.11.0 的 `thread_private.h` 定义了三个基本状态：

```
enum thread_state {
    THREAD_STATE_FREE,
    THREAD_STATE_SUSPENDED,
    THREAD_STATE_ACTIVE,
};
```

注意，没有一个独立的 `READY` 或 `RUNNABLE` 状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **14.4.1 状态转换图**

概念状态机。实际状态转换受 Global Spinlock、当前 CPU-local 状态和传输协议的约束。

三个状态的真正含义：

| 状态        | 是否占用 Thread Slot | 是否有尚未完成的执行流 | 是否能被新请求分配 |
| --------- | ---------------- | ----------- | --------- |
| FREE      | 否                | 否           | 是         |
| ACTIVE    | 是                | 是           | 否         |
| SUSPENDED | 是                | 是           | 否         |

**最容易理解错误的地方就是 SUSPENDED。**

当一个 Thread 被挂起时，它只是释放了 CPU 的执行机会，**没有释放 Trusted Thread Slot，也没有释放执行该请求所需要的 Kernel Stack。**

因此，一个系统即使没有任何 CPU 正在执行 Trusted Thread，也可能因为全部 Trusted Thread 都处于 Suspended 状态而无法接受新的请求。

## **14.4.2 每个状态转换必须满足什么条件？**

| 转换                 | 前置条件                         | 转换后的安全不变量               |
| ------------------ | ---------------------------- | ----------------------- |
| FREE → ACTIVE      | 有空闲 Slot，取得分配锁               | 同一个 Slot 只能被一个执行流占用     |
| ACTIVE → SUSPENDED | 当前 CPU 正在执行该 Thread          | 寄存器、栈、映射、执行上下文可恢复       |
| SUSPENDED → ACTIVE | Resume ID 合法，当前状态是 Suspended | 不允许两个 CPU 同时恢复同一 Thread |
| ACTIVE → FREE      | 服务执行完成，进入释放流程                | 不再保留未完成的执行流，Slot 可重用    |

这里的锁不是为了让整个 Trusted Thread 一直独占全局资源。

相反，全局锁只应保护必要的状态转换和共享元数据。

例如，在 Arm 实现中，`alloc_thread()` 在持有 Global Thread Lock 时扫描 Free Slot 并将其改为 Active，而实际 TA 运行不在这个锁内执行。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



如果把 Global Lock 一直保持到 TA 完成，那么多核并发执行几乎就被串行化了。

这就是典型的“锁保护状态转换，而不是锁保护整个服务执行”的设计原则。

# **14.5 从真实源码追踪一次 Trusted Thread 分配**

现在开始进入源码。

使用 Arm64 传统 SMC 路径作为参考。

主要文件：

```
core/arch/arm/kernel/
    thread_optee_smc_a64.S
    thread_optee_smc.c
    thread.c
    thread_a64.S

core/kernel/
    thread.c

core/include/kernel/
    thread.h
    thread_private.h
```

## **14.5.1 从 Secure Monitor 进入 OP-TEE**

Linux 侧一次标准调用通常会通过 OP-TEE Driver 准备好消息参数，然后使用 SMC 调用安全世界。

典型调用链：

```
Linux OP-TEE Driver
    |
    | SMC / OPTEE_SMC_CALL_WITH_ARG
    v
Secure Monitor (EL3)
    |
    | World Switch
    v
vector_std_smc_entry
    |
    v
thread_handle_std_smc()
    |
    v
thread_alloc_and_run()
    |
    v
thread_resume()
    |
    v
thread_std_smc_entry
    |
    v
__thread_std_smc_entry()
    |
    v
std_smc_entry()
    |
    v
tee_entry_std()
    |
    v
TA Manager / Kernel Service
```

这里展示的是正常新请求的代表性链路，具体包含预注册参数、FF-A 或其他传输时入口会不同。

### **为什么一开始不能直接执行 TA Manager？**

因为请求刚进入 OP-TEE 时，还没有分配普通 Trusted Thread。

这时执行环境是：

```
CPU:          CPU 0
Stack:        Per-CPU Temporary Stack
Current TID:  INVALID
Interrupts:   Masked as required by entry
```

如果直接执行可能阻塞的 TA Manager，那么当服务需要 RPC 时，就无法正确保存该服务的持久执行上下文。

因此必须先完成：

1. 选择可用 Thread Slot。
2. 初始化 Thread Register Context。
3. 配置它的 Kernel Stack。
4. 建立 CPU-local 到 Thread 的关联。
5. 切换到 Trusted Thread 的执行上下文。

之后才能进入正常的 Yielding Service。

## **14.5.2&#x20;**`thread_handle_std_smc()`**：分配还是恢复？**

4.11.0 源码包含两个入口分支。

逻辑简化如下：

```
if (smc_function_id == RETURN_FROM_RPC) {
    resume_existing_thread(thread_id, rpc_result);
} else {
    allocate_and_run_new_thread(smc_args);
}
```

对应真实函数：

```
thread_resume_from_rpc(...);
thread_alloc_and_run(...);
```

这个分支极其重要：

**Return From RPC 不是创建一个新 Thread，而是恢复以前保存的 Thread。**

否则，原来的 Kernel Stack、执行 PC 和 Session Stack 全部无法延续。

另外，真实源码中有一个非常值得注意的行为：`thread_resume_from_rpc()` 和 `thread_alloc_and_run()` 成功后通常不通过普通的 C `return` 回到 Dispatcher；它们通过恢复寄存器上下文进入新的执行路径，最后由 Thread Exit 或 RPC Exit 返回 Normal World。只有错误场景才从这两个函数正常返回。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这类代码对于熟悉普通 C 调用栈的工程师来说，往往是理解困难的第一处。

## **14.5.3&#x20;**`alloc_thread()`**：从 Thread Pool 取得 Slot**

以下是依据 4.11.0 核心逻辑改写的伪代码：

```
static struct thread_ctx *alloc_thread(uint32_t flags)
{
    struct thread_core_local *local;
    struct thread_ctx *selected = NULL;

    local = thread_get_core_local();

    assert(local->curr_thread == THREAD_ID_INVALID);

    thread_lock_global();

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        if (threads[i].state == THREAD_STATE_FREE) {
            threads[i].state = THREAD_STATE_ACTIVE;
            selected = &threads[i];
            break;
        }
    }

    thread_unlock_global();

    if (!selected)
        return NULL;

    local->curr_thread = thread_index(selected);
    selected->flags = flags;

    return selected;
}
```

这里值得逐点分析。

**第一，为什么检查&#x20;**`curr_thread == INVALID`**？**

因为这个函数用于分配新的执行上下文，不应该在当前 CPU 已有普通 Trusted Thread 的情况下直接替换其关联。

如果不保证这一点，原有 Thread 的执行状态可能被覆盖。

**第二，为什么扫描整个 Thread Pool？**

因为 Thread Slot 是全局共享的，并非每个 CPU 预留一个固定 Slot。

CPU 0 可以选择 Thread 3，CPU 1 可以选择 Thread 0。

**第三，为什么必须使用 Global Spinlock？**

考虑两个 CPU 同时执行：

```
CPU 0: checks threads[2].state == FREE
CPU 1: checks threads[2].state == FREE
```

如果没有同步，两个 CPU 都可能把同一个 Thread Slot 标为 ACTIVE。

之后两个 CPU 使用同一个 Kernel Stack，就会发生严重内存破坏。

所以这里的 `FREE → ACTIVE` 本质是一个互斥资源所有权转移。

**第四，为什么要尽早释放锁？**

因为后续操作可能运行数毫秒甚至更久，某些服务还需要 RPC。

如果一直持有 Spinlock，将导致整个系统无法正常并发执行，而且可能触发异常路径的锁约束。

**第五，分配失败意味着什么？**

意味着没有可用的 Thread Slot。

在传统 SMC ABI 中，入口会向 Normal World 返回 `OPTEE_SMC_RETURN_ETHREAD_LIMIT`，而不是直接释放其他正在执行或挂起的 Thread。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



这里有个版本细节：4.11.0 还支持 `CFG_DYN_CONFIG` 的部分动态配置模式。因此分析实际平台时，应核实有效 Thread Count 和构建配置，不要把所有配置都机械地视为固定静态数组。

## **14.5.4&#x20;**`init_regs()`**：准备第一次进入 Trusted Thread**

分配到 Thread 后，代码将构造初始寄存器状态。

对 Arm64 而言，主要是：

```
regs.pc     = thread_std_smc_entry
regs.sp     = thread->stack_va_end

regs.x0     = a0
regs.x1     = a1
...
regs.x7     = a7

regs.cpsr   = initial execution status
```

注意：

**这里没有通过 Linux 风格的 Scheduler 选择一个已有 Runnable Task。**

而是在准备一个全新的安全服务执行入口，让 CPU 从新的 PC 和 SP 开始执行。

因此，从汇编视角看，Trusted Thread 的启动更接近：

```
Prepare CPU Register Frame
            |
            v
Restore Register Frame
            |
            v
Exception Return
            |
            v
Start Thread Entry
```

而不是传统 RTOS 的：

```
Scheduler Selects Next TCB
            |
            v
Switch Previous/Next Context
```

## **14.5.5&#x20;**`thread_resume()`**：真正完成执行上下文切换**

Arm64 的实际汇编入口位于：

`core/arch/arm/kernel/thread_a64.S`

关键操作可以抽象为：

```
/* Conceptual AArch64 steps, not verbatim source */

restore_saved_general_registers

mov  sp, saved_sp
msr  elr_el1, saved_pc
msr  spsr_el1, saved_pstate

restore_remaining_registers

eret
```

这里要理解三个重要寄存器：

| 寄存器        | 作用                             |
| ---------- | ------------------------------ |
| `SP`       | 恢复 Trusted Thread Kernel Stack |
| `ELR_EL1`  | 指定异常返回后的 PC                    |
| `SPSR_EL1` | 指定异常返回后的 PSTATE                |

在第一次进入时，`ELR_EL1` 指向 `thread_std_smc_entry`。

在后续 RPC Resume 时，保存的返回位置则用于继续先前暂停的执行。

`eret` 之后，CPU 将按照保存的异常返回状态继续执行。

实际源码还需要处理 TPIDR、PAuth Key、寄存器恢复次序以及从 EL0 返回等特殊情况，不能把上面的示意当成完整汇编实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **这里有一个非常深层的设计思想**

**创建一个 Thread，并不一定需要调用传统的&#x20;**`create_thread()`**&#x20;/&#x20;**`schedule()`**。**

只要能够构造一份有效的 CPU Context，并在合适的权限状态下恢复它，就能够建立新的执行流。

这也是为什么 OP-TEE 的 Trusted Thread Framework 与 Assembly、Exception Frame、Stack Layout 之间耦合很深。

# **14.6 Thread Exit：服务完成之后如何释放 Thread？**

正常情况下：

```
User TA completes InvokeCommand
        |
        v
TA Manager returns
        |
        v
__thread_std_smc_entry() returns
        |
        v
thread_std_smc_entry (Assembly)
        |
        v
Switch to Temporary Stack
        |
        v
thread_state_free()
        |
        v
SMC Return to Normal World
```

这里最值得解释的是：

**为什么不能直接在当前 Thread Stack 上将 Thread 标记为 FREE？**

假设：

```
CPU 0:
    Running on Thread 3 Stack
```

如果直接：

```
threads[3].state = THREAD_STATE_FREE;
```

另一个 CPU 就可能立即分配 Thread 3。

```
CPU 1:
    Allocate Thread 3
    Starts using Thread 3 Stack
```

而 CPU 0 此时可能还没有离开 Thread 3 的栈。

结果：

```
CPU 0 ──┐
        ├── Same Kernel Stack
CPU 1 ──┘
```

这就是一个非常严重的 Stack Ownership Race。

因此，正确的策略是：

**先停止使用该 Thread 的栈，切换到 CPU-local Temporary Stack，再释放 Thread Slot。**

在 Arm64 上游代码中，`thread_std_smc_entry` 会先取得临时栈并切换 SP，然后调用 `thread_state_free()`，最后向 Monitor 发出完成返回。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **14.6.1&#x20;**`thread_state_free()`**&#x20;的核心动作**

函数主要完成：

- 恢复或清理相关扩展寄存器状态。
- 释放 Pager 管理的相应物理栈资源。
- 获取 Global Thread Lock。
- 确认当前 Thread 处于 ACTIVE。
- 将 Thread 标记为 FREE。
- 清除 Thread Flags。
- 把 CPU-local `curr_thread` 设置为 INVALID。
- 处理启用 Virtualization 时的相关状态。
- 释放 Global Thread Lock。

并不是释放整个 `struct thread_ctx`，也不是销毁整个 Thread Stack 的虚拟地址范围。

正常静态 Thread Pool 中，Slot 后续还会被新请求复用。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **一个重要的资源生命周期判断**

**Trusted Thread FREE，并不意味着这个 Slot 关联的所有缓存和内存对象都会被物理销毁。**

例如，线程相关的 RPC Shared Memory Cache 有独立的管理逻辑。

因此在 Debug 内存泄漏时，不能简单地把所有长期存在的 Thread-local 缓存视为 Leak；需要先确认是有意保留以供复用，还是已经失去所有权但无法回收。

# **14.7 RPC Suspend & Resume：整个 Thread Framework 的核心**

这一节是本章最重要的源码分析。

我们从一个实际场景开始。

Linux Application 请求 User TA 读取 Secure Storage。

TA 最终调用到 OP-TEE Secure Storage Service，服务又需要 `tee-supplicant` 访问 Linux Filesystem。

## **14.7.1 完整时序**

一次 RPC 的执行路径

简化了 Linux Driver 内部 RPC Handler、Wait Queue、SMC 返回值编码和 Buffer 传递。

最重要的执行语义是：

```
Thread 2:
    ACTIVE
       |
       v
    SUSPENDED
       |
       | Normal World handles RPC
       |
       v
    ACTIVE
       |
       v
    FREE
```

**RPC 发生时没有创建一个新的安全线程替代 Thread 2。恢复的是同一个 Thread 2 所代表的执行流。**

## **14.7.2 RPC 前到底保存了什么？**

从架构层次来看，至少需要处理：

- 必须保存的通用寄存器。
- 当前继续执行的位置。
- Kernel Stack Pointer。
- Exception/PSTATE。
- 当前 User TA 的内存映射。
- VFP 或其他扩展状态。
- Thread Flags。
- RPC 返回所需的参数。
- 当前 TA Session/Context 等逻辑状态。

其中部分状态保存在 `thread_ctx.regs`，部分原本就留在 Kernel Stack，另一部分保存在 `thread_ctx` 相关字段和 TSD 中。

注意不要误以为必须把当前函数的所有局部变量逐个复制出来。

**保留完整的 Kernel Stack，再恢复正确的 SP、PC 和寄存器，就可以延续原先的 C 调用链。**

例如：

```
TEE_Result secure_storage_read(...)
{
    struct file_context ctx;
    size_t bytes_read = 0;

    // Prepare context...

    result = thread_rpc_cmd(...);

    // After resume, execution continues here.

    return result;
}
```

对调用者来说，`thread_rpc_cmd()` 的行为看起来可能只是一个普通的同步函数调用。

但内部实际上发生了一次：

```
Secure Kernel
    |
    v
Suspend
    |
    v
Normal World
    |
    v
Resume
    |
    v
Secure Kernel
```

这是一种非常有意思的 **Continuation** 设计。

它并没有保存一个高级语言闭包，而是直接保存 CPU Execution Context 和 Stack，从而实现低层次的 Continuation。

## **14.7.3 深入 Arm64 Assembly：**`thread_rpc_spsr()`

4.11.0 的真实函数位于：

`core/arch/arm/kernel/thread_optee_smc_a64.S`

从源码可以提取出以下关键阶段：

**阶段 A：禁止相关异步异常并保存寄存器。**

RPC Exit 是跨执行域切换，必须在受控的异常屏蔽状态下进行。

**阶段 B：取得当前 Thread Register Context。**

调用 `thread_get_ctx_regs()`，找到当前 Thread 的 `regs`。

**阶段 C：保存恢复执行所需的 SP 和寄存器。**

其中包括被调用约定要求保留的寄存器和相关架构状态。

**阶段 D：切换到 Temporary Stack。**

此前所有操作使用的仍是 Trusted Thread Stack；之后要为 Suspended 状态完成收尾。

**阶段 E：设置恢复 PC。**

RPC 返回后的继续位置由汇编代码中的 `.thread_rpc_return` 标签确定。

这点非常关键。保存的 PC 不是 TA 最初的入口，也不是 `thread_std_smc_entry`，而是 RPC 完成后应继续的位置。

**阶段 F：调用&#x20;**`thread_state_suspend()`**。**

这一步会将 Thread 状态改为 SUSPENDED，并解除它与当前 CPU 的执行绑定。

**阶段 G：执行 SMC，返回 Normal World。**

将 RPC 请求以及 Resume 所需的 Thread ID 交给 Normal World 侧处理。

具体源码中甚至有一个容易忽略的细节：RPC 参数需要在某些虚拟化相关映射被解除之前提前读入寄存器，否则原先保存参数的内存可能已经不可访问。这说明 Suspend 路径还会牵涉到地址空间生命周期。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **14.7.4&#x20;**`thread_state_suspend()`**&#x20;为什么不只修改&#x20;**`state`**？**

源码的关键工作包括：

```
1. Identify current Trusted Thread
2. Check Stack / Profiling State
3. Save or finalize extension state
4. Acquire Global Thread Lock
5. Save Resume PC and PSTATE
6. Mark Thread as SUSPENDED
7. Save active User Mapping if present
8. Remove active User Mapping
9. Update Secure Partition state if needed
10. Clear CPU-local curr_thread
11. Release Global Thread Lock
12. Return Thread ID
```

其中第 7 和第 8 步非常值得深入。

假设当前 Thread 正在执行 TA-A：

```
Thread 2
    |
    +-- TA-A Address Space
            |
            +-- Code
            +-- Data
            +-- Stack
            +-- Shared Memory
```

如果 Thread 2 Suspend，当前 CPU 后续可能执行其他 Trusted Thread，进入 TA-B。

那么不能继续认为 CPU 当前使用的是 TA-A 的 User Mapping。

因此，挂起时需要保存 User Mapping，并解除当前活动映射，以免它错误地成为后续执行流的地址空间。

源码中的：

```
core_mmu_get_user_map(...);
core_mmu_set_user_map(NULL);
```

正体现了这个意图。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这还没有结束：映射切换必须与架构对应的 TLB/ASID 管理保持一致。具体哪些寄存器和 TLB 操作由 MMU Backend 完成，是 Chapter 20–22 的研究重点。

## **14.7.5 Resume：为什么不能直接根据 Thread ID 恢复？**

入口最终会调用：

```
thread_resume_from_rpc(thread_id, ...);
```

其状态检查包含：

```
thread_id < CFG_NUM_THREADS
```

以及：

```
threads[thread_id].state == THREAD_STATE_SUSPENDED
```

取得 Global Lock 后，合法的 Suspended Thread 被转换为 ACTIVE。

然后：

- 当前 CPU 的 `curr_thread` 被设置为该 Thread ID。
- 根据需要恢复 User Mapping。
- 更新 User Mode Execution Timing。
- 根据 Flags 决定是否复制 RPC 返回参数。
- 准备相关扩展寄存器状态。
- 调用 `thread_resume()`。

如果 Thread ID 无效或者 Thread 已经不是 Suspended 状态，恢复会失败。

特别值得注意的是：

```
SUSPENDED -> ACTIVE
```

必须在 Global Lock 保护下完成。

否则两个 CPU 可能同时收到指向同一个 Thread ID 的 Resume 请求，并同时恢复它。

这将造成同一个 Kernel Stack 被并发执行的严重问题。

在 4.11.0 的实现中，这个检查和状态转换确实位于 `thread_resume_from_rpc()` 的锁保护区内。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **一个需要单独强调的安全问题**

Thread ID 是用于标识某个待恢复执行流的索引，**不是用于证明客户端身份的安全凭证**。

传统 SMC 路径中，Resume 的入口检查和相应传输约束不能替代更高层的 Identity、Session、MOBJ 和 RPC Result Validation。

尤其在存在多个 Guest 或不完全可信的通信代理时，不能简单认为“Thread ID 合法”就意味着“发起 Resume 的实体拥有所有相关资源”。

# **14.8 Native Interrupt、Foreign Interrupt 与 RPC 的区别**

到这里已经知道，RPC 会使 Trusted Thread Suspend。

但还有一类重要事件：Interrupt。

OP-TEE 区分 Native Interrupt 和 Foreign Interrupt。

## **14.8.1 Native Interrupt**

假设 OP-TEE 正在执行：

```
Trusted Thread 2
    |
    v
Crypto Processing
```

此时安全 Crypto Engine 产生 Secure Interrupt。

如果中断由 OP-TEE 自己处理，那么它可以：

```
Trusted Thread 2
       |
       v
Native Interrupt
       |
       v
Interrupt Handler
       |
       v
Return from Exception
       |
       v
Trusted Thread 2 continues
```

这里不一定需要将 Trusted Thread 标记为 SUSPENDED。

因为 CPU 仍在 Secure World 内部，处理完成后可以直接回到被中断的执行位置。

这与传统 OS 处理同步执行上下文中的普通中断比较类似。

## **14.8.2 Foreign Interrupt**

假设 OP-TEE 正在执行时，Linux 需要处理一个非安全中断。

OP-TEE 不负责替 Linux 完成全部中断处理。

因此，传统 Arm 模型中可以：

```
Trusted Thread 2
        |
        v
Foreign Interrupt
        |
        v
Save Thread Context
        |
        v
SUSPENDED
        |
        v
Return to Normal World
        |
        v
Linux handles interrupt
        |
        v
Return From RPC
        |
        v
Resume Trusted Thread 2
```

Foreign Interrupt Exit 的路径与普通 RPC 相似，但两者有一个重要差异。

普通 RPC 会从 Normal World 获取服务结果。

Foreign Interrupt Resume 并不意味着可以直接接受 Normal World 任意提供的结果参数。

源码通过 `THREAD_FLAGS_COPY_ARGS_ON_RETURN` 等 Flags 区分这些行为，并且注释明确指出 Foreign Interrupt 返回不应从 Non-Secure World 接收普通 RPC 返回参数。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **14.8.3 Fast Call 为什么不同？**

Fast SMC 不使用普通 Trusted Thread 执行长时间服务。

它通常在 Entry Stack 上完成，保持要求的异常屏蔽状态，并快速返回。

如果让 Fast Call：

- 访问需要 RPC 的文件服务。
- 获取一个可能长期等待的 Mutex。
- 执行可能进入 User TA 的复杂操作。

那么它就破坏了 Fast Call 的执行约束。

因此，判断一个新服务是否能在 Fast Call 中执行，不是简单看它是否“通常很快”。

真正应该考虑：

**它是否在所有允许的执行路径中，都不会发生不受控等待、Thread Suspend 或依赖普通 Trusted Thread 才能完成的操作。**

上游 `thread_handle_fast_smc()` 在处理结束后仍会检查要求的异常屏蔽状态，正是为了维持此类执行约束。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



# **14.9 SMP：多个 CPU 如何共享 Trusted Thread Pool？**

现在我们讨论一个更接近真实 SoC 的场景。

假设：

```
CPU Core Count:       4
CFG_NUM_THREADS:      2
Linux Concurrent CA:  8
```

很多工程师最初会问：

既然有四个 CPU，为什么 OP-TEE 只有两个 Trusted Thread？它们是绑定在 CPU 0 和 CPU 1 上的吗？

答案是：**不是。**

## **14.9.1 全局 Thread Pool 与 CPU-local State**

逻辑关系如下：

```
               Global Thread Pool
             +--------------------+
             | Thread 0           |
             | Thread 1           |
             +--------------------+
                       ^
                       |
               Global Spinlock
                       |
          +------------+------------+
          |            |            |
       CPU 0         CPU 1        CPU 2 / 3
          |            |
          v            v
   core_local[0]  core_local[1]
          |            |
       Thread 1      Thread 0
```

Thread Slot 由全局资源池分配。

一个 CPU 可以执行任意适合当前调用的空闲 Thread Slot，而不是绑定某个固定 Slot。

这使得：

```
CPU 0 -> Thread 0
CPU 1 -> Thread 1
```

和：

```
CPU 2 -> Thread 0
CPU 3 -> Thread 1
```

都可以成为不同时间点的执行情况。

前提是执行流已按协议挂起并恢复，且架构状态允许这样的迁移。

## **14.9.2 为什么必须考虑跨 CPU Resume？**

来看一个具体时序：

| 时间 | CPU 0       | CPU 1         | Thread 0        |
| -- | ----------- | ------------- | --------------- |
| T0 | 进入 OP-TEE   | Linux         | ACTIVE on CPU 0 |
| T1 | RPC Suspend | Linux         | SUSPENDED       |
| T2 | Linux       | Linux 处理 RPC  | SUSPENDED       |
| T3 | Linux       | Resume OP-TEE | ACTIVE on CPU 1 |
| T4 | Linux       | 继续 TA         | ACTIVE on CPU 1 |
| T5 | Linux       | 完成            | FREE            |

这里不存在 CPU 0 必须负责恢复 Thread 0 的普遍性要求。

真正决定恢复正确性的，是：

**所有属于 Thread 的必要状态必须可恢复，而所有属于 CPU 的临时状态必须重新正确建立。**

具体需要考虑：

- General-Purpose Registers。
- SP、PC、Processor Status。
- User Mapping。
- TLS/TP 状态。
- Floating Point / SIMD / Vector 状态。
- Architecture-Specific Control Registers。
- Interrupt Mask State。
- Per-CPU 与 Per-Thread 的资源引用关系。

对于 Arm64，实际 `thread_resume()` 除了恢复通用寄存器，还涉及 `TPIDR_EL0` 和配置启用时的 Pointer Authentication 状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **14.9.3 为什么不能随便把数据放进 Per-CPU Variable？**

假设你在 OP-TEE Core 中新增一个字段：

```
struct thread_core_local {
    ...
    struct crypto_request *current_request;
};
```

用于记录当前正在执行的安全 Crypto Request。

这看上去很合理，因为每个 CPU 任一时刻只执行一个普通 Trusted Thread。

但需要问：

如果 Crypto Request 执行过程中发生 RPC Suspend 呢？

如果同一执行流在另一个 CPU 上恢复呢？

若这个字段没有在 Suspend 时同步转移到 Thread Context，那么就可能发生：

```
CPU 0:
    current_request = Request A

Thread A suspended

CPU 1:
    resume Thread A

CPU 1:
    current_request = NULL
```

此时无法再找到原先的 Request。

更糟糕的是，CPU 0 后续可能执行 Thread B，仍然读到 Request A 的指针，导致资源所有权混淆。

因此，要在设计阶段就决定：

| 状态类型                   | 应该放在哪里                     |
| ---------------------- | -------------------------- |
| 当前 CPU 的异常入口信息         | Per-CPU                    |
| 当前 CPU 临时栈             | Per-CPU                    |
| 当前 CPU 正执行的 Thread ID  | Per-CPU                    |
| 可跨 RPC 保存的请求 Context   | Per-Thread 或请求拥有者          |
| TA Session Stack       | Per-Thread                 |
| TA Instance State      | TA Context                 |
| 全局共享 Device State      | Driver Global Context      |
| 可跨 CPU 使用的 DMA Request | 有独立生命周期和同步的 Request Object |

这是一种非常重要的系统设计能力：**区分执行位置与资源所有权。**

# **14.10 Thread Exhaustion：为什么 CPU 空闲但 OP-TEE 仍然 Busy？**

现在讨论实际调试中非常常见的一类问题。

假设：

```
CFG_NUM_THREADS = 2
```

两个 Linux Client 同时发起 Secure Storage 请求：

```
Linux Task A -> Trusted Thread 0 -> RPC -> SUSPENDED

Linux Task B -> Trusted Thread 1 -> RPC -> SUSPENDED
```

这时候 CPU 可能全部在 Normal World 中执行。

第三个 Linux Task C 发起：

```
TEEC_InvokeCommand()
```

结果是什么？

Thread Pool 已经被占满：

```
Thread 0: SUSPENDED
Thread 1: SUSPENDED
```

没有 FREE Slot。

因此新请求无法分配 Thread，而传统 SMC 接口可以返回：

```
OPTEE_SMC_RETURN_ETHREAD_LIMIT
```

注意，这不是因为 CPU 没有时间执行任务，而是因为 **所有可保存执行流的资源都被占用**。

## **14.10.1 Linux Driver 如何处理 Thread Exhaustion？**

Linux OP-TEE SMC Driver 中可以看到以下典型结构：

```
do_call_with_arg()
    |
    +-- invoke SMC
    |
    +-- ETHREAD_LIMIT ?
    |       |
    |       +-- wait on call queue
    |       +-- retry
    |
    +-- RPC ?
    |       |
    |       +-- handle RPC
    |       +-- invoke SMC again
    |
    +-- completed
            |
            +-- finalize call
            +-- wake a waiter
```

这里有两个与普通函数调用不同的行为。

第一，**Normal World 负责处理 Thread Limit 的等待与重试**，不是 OP-TEE 在内部 Run Queue 里将 Task C 排入等待状态。

第二，等待线程需要在其他调用完成之后得到唤醒机会。

在 Linux 当前的 `smc_abi.c` 实现中，可以直接看到 `OPTEE_SMC_RETURN_ETHREAD_LIMIT`、Call Queue Wait、RPC Dispatch 和 Finalize Wakeup 的组合逻辑。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **14.10.2 Thread Exhaustion 不一定需要增加&#x20;**`CFG_NUM_THREADS`

假设有一个性能问题：

```
System Load:
    20 clients

CFG_NUM_THREADS:
    2

Observed:
    High latency
    Frequent ETHREAD_LIMIT
```

第一反应可能是：

```
CFG_NUM_THREADS = 8
```

这可能会改善问题，但也可能只是隐藏另一个设计缺陷。

例如：

- `tee-supplicant` 被阻塞。
- REE Filesystem 访问异常缓慢。
- Secure Storage Backend 持有某个锁时间过长。
- TA 存在长时间运行但不主动让出执行机会的代码。
- RPC 服务依赖新的 TEE Call，从而形成循环等待。
- 异常恢复路径没有释放某些已完成请求的 Thread Slot。

因此，正确排查顺序应该是：

1. 观察 FREE、ACTIVE、SUSPENDED 各有多少。
2. 确认 ACTIVE Thread 正在执行什么。
3. 确认 SUSPENDED Thread 正在等待什么。
4. 检查 RPC 是否能够继续推进。
5. 检查 Mutex、Session 和 Driver 是否存在循环依赖。
6. 判断真正的瓶颈是 CPU、Thread Slot、Backend 还是调度延迟。
7. 最后才决定是否增加 Thread Pool。

## **14.10.3 如何估算合理的 Thread 数量？**

从性能建模角度，可以使用 Little's Law：

\\[ L = \lambda W \\]

其中：

- \\(L\\)：平均正在系统中占用执行上下文的请求数量。
- \\(\lambda\\)：单位时间内完成的请求数。
- \\(W\\)：每个请求平均占用 Thread Slot 的时间。

注意，\\(W\\) 不仅包含真正执行 Secure Code 的时间，还包含 RPC Suspend 期间该 Thread Slot 被占用的时间。

假设一个稳定负载中：

- 每秒 100 次安全服务请求。
- 每个请求平均占用 20 ms Thread Slot。

那么：

\\[ L=100\times0.020=2 \\]

这意味着平均就有两个 Thread Slot 被占用。

如果只配置两个 Slot，遇到突发流量或长尾 RPC 延迟时，很容易形成等待。

不过，这个模型仅用于初步容量分析。实际还必须考虑请求到达的突发性、服务时间分布、Linux Call Queue、专用系统服务请求及不同配置的资源保留策略。

## **14.10.4 Thread 数量的内存成本**

Thread 数量不是免费的。

增加 Thread Slot 可能增加：

- Thread Context 结构体内存。
- Kernel Stack 的虚拟地址空间。
- 不启用 Pager 时的常驻栈物理内存。
- 用户页表及相关缓存资源。
- RPC Cache。
- 并发 TA 运行和安全服务的峰值资源消耗。

在启用 Pager 的配置下，Thread Stack 的虚拟空间和实际常驻物理页数量需要区分，不能简单地认为每个 Thread 的整个 Stack 在任何时刻都固定占用等量物理内存。

此外，OP-TEE 配置中 `CFG_PGT_CACHE_ENTRIES` 的默认计算还会参考 `CFG_NUM_THREADS`，所以调大 Thread Pool 可能间接影响 Page Table Cache 的规模。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



真正合理的配置，应该来自并发模型和实际的资源测量。

# **14.11 Concurrency、Locking 与 Deadlock**

这一部分我们不详细重复 Chapter 17 的锁实现，而是专门讨论锁与 Trusted Thread 执行模型的关系。

## **14.11.1 为什么 OP-TEE 的 Blocking Mutex 与普通 Spinlock 不同？**

考虑一个安全服务正在执行：

```
Thread A
    |
    v
Acquire Mutex
    |
    v
Resource Unavailable
```

传统 RTOS 可以通过 Scheduler 将 Thread A 置为 BLOCKED，然后运行另一个 Ready Thread。

而传统 OP-TEE 没有这样一个完整的通用 Scheduler。

因此，其等待机制需要与 Normal World 的执行机会和通知机制协作。

典型逻辑：

```
Thread A tries Mutex
       |
       v
Mutex unavailable
       |
       v
Arrange wait / notification
       |
       v
RPC to Normal World
       |
       v
Linux waits
       |
       v
Another Thread unlocks
       |
       v
Notification
       |
       v
Linux resumes request
       |
       v
Thread A retries / proceeds
```

官方文档给出了类似的 Mutex 等待与通知示例，说明如何借助 Normal World 的等待能力管理冲突的 Trusted Thread。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



这意味着 OP-TEE 的 Mutex Waiting 并不是一个纯粹的 Secure World 内部行为。

## **14.11.2 为什么 Spinlock 不能跨 RPC？**

考虑：

```
spin_lock(&global_lock);

thread_rpc_cmd(...);

spin_unlock(&global_lock);
```

这段代码在设计上存在严重问题。

因为：

```
CPU 0:
    holds spinlock
    |
    v
    RPC Suspend
    |
    v
    Normal World
```

当另一个 CPU 尝试获取相同 Spinlock：

```
CPU 1:
    spin_lock(&global_lock)
    |
    v
    Busy Wait
```

可能发生：

- CPU 1 长时间忙等。
- 中断屏蔽状态无法按预期恢复。
- Lock Ownership 与实际运行 CPU 不一致。
- RPC 返回后如果在其他 CPU Resume，Spinlock 相关 CPU-local 假设被破坏。
- 在其他代码需要该锁才能推进 RPC 或释放资源时形成死锁。

因此，正确原则是：

**Spinlock Critical Section 必须非常短，并且不能跨越可能发生 RPC、Sleeping 或不受控执行域切换的操作。**

OP-TEE 在部分异常屏蔽接口中显式检查持有 Spinlock 时不允许解除 Foreign Interrupt Mask；这说明 Thread、Interrupt 和 Locking 之间存在严格的契约。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **14.11.3 真正复杂的死锁来自多个执行域**

我们来看一个更困难的场景。

```
Trusted Thread A:
    holds Secure Mutex M
    |
    v
    RPC Request X
    |
    v
    waits for Linux Service
```

与此同时：

```
Linux Service X:
    needs another TEE operation
    |
    v
    Secure Thread B
    |
    v
    tries Secure Mutex M
```

形成：

```
Thread A holds M
      |
      v
Waits for Service X
      |
      v
Service X waits for Thread B
      |
      v
Thread B waits for M
      |
      +----------> Thread A
```

这是一个跨 Secure/Normal World 的 Wait-for Cycle。

关键点在于：

**如果你只审查 Secure World 内部锁顺序，可能完全发现不了这个死锁。**

因为死锁涉及：

- Trusted Thread A。
- Linux RPC Worker。
- Trusted Thread B。
- Secure Mutex。
- Normal World Call Queue。

更严重的是，即使完全没有明显的锁错误，也可能由于 Thread Slot 全部被某些等待请求占用，导致完成这些请求所需的依赖调用无法进入 OP-TEE。

这种情况可以称为 Thread Pool Starvation Deadlock。

需要特别指出：上述是用于推导风险的系统设计场景，能否在某个具体实现中复现，还取决于 RPC 服务的重入行为和 Linux Driver 的系统线程处理策略。

# **14.12 RISC-V 移植时，Thread Framework 哪些部分需要重新设计？**

虽然这一章以 Arm64 为例，但对于 RISC-V，最重要的是把共性与架构差异分开。

## **14.12.1 可以复用的通用设计**

以下概念不依赖 Arm：

- Thread Pool。
- Thread State Machine。
- Thread Specific Data。
- Global Allocation Lock。
- TA Session Stack。
- RPC 和 Thread Context 的关联。
- Thread Stack Lifecycle。
- 用户态内存映射保存与恢复的抽象接口。
- Thread Exhaustion Policy。

这些机制应该尽量放在 Architecture-Neutral Core。

## **14.12.2 RISC-V 需要适配的架构状态**

4.11.0 的 RISC-V `thread_arch.h` 已定义相关上下文结构，例如：

```
struct thread_ctx_regs {
    unsigned long ra;
    unsigned long sp;
    unsigned long gp;
    unsigned long tp;

    /* General purpose registers */

    unsigned long epc;
    unsigned long status;
    unsigned long ie;
};
```

其中：

| RISC-V 状态    | 作用             |
| ------------ | -------------- |
| `ra`         | 返回地址           |
| `sp`         | 当前执行栈          |
| `gp`         | Global Pointer |
| `tp`         | Thread Pointer |
| `s0`–`s11` 等 | 需要保存的通用寄存器     |
| `epc`        | 需要恢复的执行位置      |
| `status`     | 特权和中断控制状态      |
| `ie`         | 中断使能相关状态       |

RISC-V 版本的 `thread_core_local` 还包含 Hart ID、Hart Index 等字段，因此在 RISC-V Porting 中，需要将 Thread 与实际 Hart 拓扑正确关联。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **三类状态必须严格区分**

**第一类：Execution Context**

例如 `ra`、`sp`、`epc`、通用寄存器。

它们决定“从哪里继续执行”。

**第二类：Privilege / Interrupt Context**

例如相关的 `sstatus`、`sie` 状态。

它们决定“以什么执行权限和中断状态继续执行”。

**第三类：Memory / Isolation Context**

例如 Address Space、`satp` 相关映射以及平台实现的 TEE Domain 隔离状态。

它们决定“执行过程中可以访问哪些资源”。

这三类状态不一定全部直接存储在 `thread_ctx_regs` 中；有的需要通过 MMU、Domain 或其他上下文管理机制恢复。

## **14.12.3 RISC-V Domain Switch 与 Trusted Thread Resume**

假设在你的 RISC-V SoC 设计中：

```
Normal S Domain
       |
       | Domain Switch
       v
TEE S Domain
       |
       v
OP-TEE Trusted Thread
```

现在 Trusted Thread 需要 RPC Suspend。

完整操作不仅仅是：

```
save GPR
save SP
save EPC
return
```

还必须确认：

1. TEE Domain 的资源访问权限是否被正确保留或撤销。
2. Normal Domain 是否可能访问保留中的 Trusted Thread Stack。
3. 恢复时是否已经重新建立正确的 TEE Domain 访问权限。
4. MMU、TLB 和系统级权限表是否保持一致。
5. 另一个 Hart 是否能够恢复相同的执行状态。
6. 运行时扩展状态是否与该 Hart 的硬件能力匹配。

这里必须注意区分：**OP-TEE Trusted Thread Framework、OpenSBI Domain、CoVE Domain Switch 以及 SoC 特定的 SMMPT 保护机制，并不是同一个软件组件。**

实际需要根据具体 RISC-V TEE 架构决定哪些切换由 OpenSBI、Domain Manager 或 OP-TEE 自身执行。

不能直接把 Arm 的 `eret` 和 World Switch 机制逐行替换成 RISC-V 的 `sret` 就认为移植完成。

## **14.12.4 FPU / Vector 状态为什么是重要挑战？**

考虑：

```
Hart 0:
    executes Trusted Thread A
    |
    v
    uses Vector Registers
    |
    v
    Suspends
```

如果只保存通用寄存器，随后 Hart 1 恢复 Thread A，那么原先 Vector Register 的内容可能已经不存在。

结果可能表现为：

- 加密计算结果错误。
- 用户 TA 中的浮点运算异常。
- 不同 TA 之间扩展寄存器状态泄漏。
- Normal World 与 Secure World 的敏感状态串扰。

因此，RISC-V Thread Porting 需要明确处理 FP、Vector 以及未来扩展的上下文保存策略。

这并非单纯的理论问题：2026 年 OP-TEE 社区已有针对 RISC-V Floating-Point/Vector Context Tracking 的相关讨论和实现工作。研究具体功能时应区分已经合入的支持与仍处于 RFC 或评审阶段的代码。

[image](https://www.google.com/s2/favicons?domain=https://lists-ec2.trustedfirmware.org\&sz=32)

lists.trustedfirmware.org+1



# **14.13 Trusted Thread 的安全不变量（Security Invariants）**

这是我认为最应该从本章带走的知识之一。

我们不只记录函数做了什么，还要记录系统绝对不能破坏的规则。

| 编号  | 安全不变量                                    | 违反后的风险                        |
| --- | ---------------------------------------- | ----------------------------- |
| I1  | 一个 ACTIVE Thread 同一时刻不能被两个 CPU 执行        | Stack/Data Corruption         |
| I2  | SUSPENDED Thread 的执行状态必须完整保存             | Resume 崩溃或状态错误                |
| I3  | FREE Thread 的旧执行流必须已经结束                  | Use-After-Free、执行流混淆          |
| I4  | Thread Context 不得错误绑定其他 TA Address Space | 跨 TA 数据泄漏                     |
| I5  | Per-CPU 状态不能被误当成可迁移的 Per-Thread 状态       | CPU Migration Failure         |
| I6  | Thread 释放前必须停止使用其 Kernel Stack           | Stack Ownership Race          |
| I7  | RPC Resume 不得接受不属于当前协议的返回状态              | 状态混淆                          |
| I8  | Foreign Interrupt Exit 不得破坏原执行流          | Context Corruption            |
| I9  | Suspend/Resume 必须遵守锁和异常屏蔽契约              | Deadlock / Race               |
| I10 | User TA 和 Normal World 的寄存器状态不得越权泄漏      | Confidentiality Breach        |
| I11 | Virtualization 下 Guest 隔离不能因 Thread 重用失效 | Cross-Guest Isolation Failure |
| I12 | 新增硬件扩展状态必须有明确的保存与恢复责任                    | FP/Vector/Key State Leakage   |

这张表可以作为未来审查 Thread Framework Patch 的基础。

尤其是 I1、I2、I4 和 I6，任何违反都可能导致直接的安全内存破坏。

# **14.14 实战：如何通过实验真正理解 Trusted Thread？**

这一章我建议安排五个实验，全部围绕实际状态转换，而不是只打印函数调用链。

## **Lab 1 — Trusted Thread Allocation Trace**

**目标：** 证明 Thread 是全局分配的，而不是绑定 Linux PID 或 CPU。

建议观测：

```
timestamp
cpu_id
thread_id
previous_state
new_state
smc_function_id
```

重点埋点：

```
alloc_thread()
thread_state_suspend()
thread_resume_from_rpc()
thread_state_free()
```

执行：

- 单个 CA 连续 Invoke。
- 多个 CA 并发 Invoke。
- 不同 CPU 上发起 Invoke。

期望能够观察到 Thread Slot 的复用和全局分配行为。

**注意：** 不要在持有 Global Spinlock 时加入可能阻塞或产生额外 RPC 的日志操作；尽量在锁外记录快照，或者使用轻量跟踪机制。

## **Lab 2 — RPC Suspend / Resume Trace**

**目标：** 验证一项安全服务跨越 RPC 后仍能够继续原有 C 函数调用链。

推荐使用涉及 REE FS 的 Storage Test，确保实际触发 RPC。

关键记录：

```
Thread ID
Kernel SP
Saved Resume PC
Thread State
CPU ID
User Mapping Presence
RPC Command
```

验证：

- Suspend 前后 Thread ID 保持一致。
- Suspend 前后 Kernel Stack 连续性得到保持。
- Resume PC 指向正确的继续执行位置。
- CPU ID 不应被当作恢复正确性的唯一依据。
- 最终正常释放 Thread。

## **Lab 3 — Thread Pool Exhaustion**

**目标：** 分析 `CFG_NUM_THREADS` 对并发调用的影响。

可以在实验性构建中配置：

```
CFG_NUM_THREADS=2
```

设计多个并发且存在受控等待的请求，逐渐增加并发数。

观察：

- `OPTEE_SMC_RETURN_ETHREAD_LIMIT`。
- Linux Call Queue Wait。
- 请求延迟分布。
- Thread Slot 占用情况。
- RPC 时延变化。

然后调整：

```
CFG_NUM_THREADS=4
```

比较吞吐和长尾时延，并确认是否发生真正的性能改善。

**注意：** 如果瓶颈本来就是一个串行的安全硬件或 Mutex，仅增加 Thread 数量可能没有明显收益。

## **Lab 4 — SMP Suspend / Resume**

**目标：** 验证 Thread Context 不依赖固定 CPU。

在多核 QEMU 环境中：

1. 运行一个可能触发 RPC 的 TA。
2. 记录 Suspend 前 CPU ID、Thread ID、SP 和 PC。
3. 调整 Normal World 的 CPU Affinity 或请求调度条件。
4. 观察 Resume 是否可能发生在不同 CPU。
5. 验证 User Mapping、Thread Context 和结果仍然正确。

这里要注意：改变 Affinity 不能保证一定产生跨核恢复。应先确认 Linux Driver 和 QEMU 配置允许相关迁移，再通过多次运行和跟踪收集证据。

## **Lab 5 — Fault Injection 与状态机验证**

**目标：** 验证异常输入无法破坏 Thread State Machine。

测试方向：

- 无效 Resume Thread ID。
- 对非 Suspended Thread 发起 Resume。
- 两个 CPU 并发尝试恢复同一 Thread。
- RPC 服务失败。
- TA Panic。
- 请求正常退出后重新使用 Thread Slot。

对异常和恶意输入类实验，优先使用隔离的 QEMU 测试构建，不应直接在生产 TEE 环境中尝试破坏内部状态。

## **建议的源码阅读顺序**

1. [Thread State / Context 定义](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/include/kernel/thread_private.h?utm_source=chatgpt.com)
2. [Thread Pool、Stack 与公共状态管理](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/thread.c?utm_source=chatgpt.com)
3. [SMC Dispatcher / RPC](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread_optee_smc.c?utm_source=chatgpt.com)
4. [Arm Thread Allocation / Suspend / Resume](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread.c?utm_source=chatgpt.com)
5. [Arm64 RPC Exit / Return Assembly](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread_optee_smc_a64.S?utm_source=chatgpt.com)
6. [Arm64 Register Context Restore](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread_a64.S?utm_source=chatgpt.com)

# **14.15 Staff Engineer Level — 深度架构与系统思考测试**

下面安排 12 道综合题。

这些题目的目的不是检查你是否记住了 `thread_resume_from_rpc()` 这样的函数名称，而是要求你在没有现成标准答案的工业场景中，依靠架构理解进行推导。

每道题都提供参考分析方向。但真正值得练习的是：**在阅读参考答案前，先自己提出假设、安全不变量、设计方案和验证方式。**

## **Question 1 — 有 16 个 CPU，为什么 OP-TEE 吞吐量仍然很低？**

主题：Thread Pool · SMP · Performance Modeling

某 RISC-V SoC 有 16 个 Hart，Linux 已经能够使用全部 Hart。

OP-TEE 配置：

```
CFG_TEE_CORE_NB_CORE = 16
CFG_NUM_THREADS      = 4
```

性能测试显示：

```
Linux CPU utilization:  20%
OP-TEE throughput:      Low
P99 Invoke latency:     150 ms
```

将 `CFG_NUM_THREADS` 从 4 改成 16 后，吞吐量几乎没有提升。

要求：

1. 解释为什么 CPU 使用率低与 Secure Service 吞吐量低可以同时发生。
2. 至少提出四个不同的瓶颈假设。
3. 如何判断是否需要增加 Thread 数量？
4. 如果某个 Secure Hardware Crypto Engine 只能串行处理请求，应该怎样重新设计？

### **参考分析**

不能把 Thread Pool 大小直接等同于处理能力。

整个系统的性能可能受限于：

\\[ T\_{\text{system}} \leq \min(T\_{\text{thread}},T\_{\text{crypto}}, T\_{\text{rpc}},T\_{\text{storage}}) \\]

其中每个 \\(T\\) 表示某一资源限制下的最大服务吞吐。

可能的瓶颈包括：

- OP-TEE Thread Pool 不足。
- Crypto Engine 只能串行执行。
- TA Manager 对同一实例的调用实行串行化。
- RPC 服务延迟。
- Secure Storage Mutex Contention。
- Linux Driver Call Queue。
- TEE Domain Switch 的固定开销。

应增加状态观测，而不是首先扩大配置。

如果 Crypto Engine 串行化才是真正瓶颈，增加 Thread 只会让更多请求停留在等待状态，还可能增加内存占用和锁竞争。

更合理的方案可能是为硬件建立 Request Queue，由 Driver 明确管理其生命周期、调度和 Completion。

**考察重点：能否区分计算资源瓶颈、并发上下文瓶颈和串行设备瓶颈。**

## **Question 2 — 设计一个不会发生 Thread Pool Starvation Deadlock 的 RPC Framework**

主题：Deadlock · RPC · Resource Dependency

假设：

```
CFG_NUM_THREADS = 2
```

两个 Trusted Thread 都已经 Suspend，分别等待两个 RPC。

但 RPC Service 要完成任务，还需要重新调用 OP-TEE。

由于没有 FREE Thread Slot，新的 TEE Call 无法开始。

请回答：

1. 用 Wait-for Graph 证明是否存在真正的 Deadlock。
2. 是否应该为 RPC Service 预留一个 Trusted Thread？
3. 如果增加到三个 Thread，能否彻底解决？
4. 如何从架构设计上避免这种死锁？
5. 如果需求包含无限递归式 TEE/RPC 调用，是否存在一个固定大小的 Thread Pool 可以保证进展？

### **参考分析**

首先建立依赖图：

```
Thread 0 -> waits for RPC A
RPC A    -> waits for new TEE call
New call -> waits for FREE thread
```

如果没有其他执行路径能释放 Slot，就形成循环等待。

增加一个保留 Thread 可能打破当前环，但不能自动证明系统不再发生死锁。

如果 RPC 可以继续递归调用 TEE，那么新的 Thread 最终也可能耗尽。

因此，应从协议层约束：

- 哪些 RPC 允许重入 TEE。
- 最大嵌套深度。
- 哪些请求需要专用执行资源。
- 哪些服务必须能够不依赖新增 Thread 而完成。
- 如何避免循环的同步等待关系。
- 系统服务请求是否需要独立的资源配额。

必要时需要重新设计为异步状态机，而不是无限增加 Thread 数量。

**更深层的结论：资源数量的增加不等于依赖图的正确性证明。**

## **Question 3 — 为什么“同一个 Thread 同时只能被一个 CPU Resume”不是充分的安全保证？**

主题：Concurrency · Identity · Cross-Guest Isolation

假设 `thread_resume_from_rpc()` 已经正确使用 Global Lock，并且保证：

```
SUSPENDED -> ACTIVE
```

是原子转换。

是否就意味着 Resume 路径是安全的？

考虑以下情况：

- Normal World 是恶意的。
- 系统启用了多个 Guest VM。
- RPC 返回参数可能被攻击者修改。
- 不同客户端同时有挂起的请求。

### **参考分析**

原子状态转换只解决其中一个问题：

**同一个 Thread 不会被两个 CPU 同时执行。**

但还需要考虑：

- Resume 请求是否属于正确的 Guest。
- 返回结果是否来自允许的通信上下文。
- RPC Buffer 和 MOBJ 是否仍然有效。
- 被恢复的执行流是否仍然对应正确的 Session 和内存映射。
- 请求是否被取消或进入其他生命周期状态。
- 传输层的 Endpoint / VM Identity 是否正确。

此外，Thread ID 自身不能作为安全身份凭证。

需要将安全不变量分层：

```
Thread State Correctness
         +
Transport Context Correctness
         +
Memory Ownership Correctness
         +
Session / Identity Correctness
```

锁只能证明状态转换的原子性，不能自动证明调用者身份、数据完整性和资源所有权。

**Staff 级考察重点：不要把局部并发正确性等同于系统整体安全性。**

## **Question 4 — 一个非常隐蔽的 Thread Free Race**

主题：Assembly · Stack Ownership · Memory Ordering

某工程师为了减少 Thread Exit 的汇编复杂度，修改了现有流程：

原先：

```
Switch to Temporary Stack
        |
        v
Mark Thread FREE
```

改成：

```
Mark Thread FREE
        |
        v
Switch to Temporary Stack
```

单核测试全部通过。

但是双核压力测试偶尔出现内存损坏。

要求分析：

1. 为什么单核测试可能发现不了？
2. 最坏情况下会发生什么？
3. 仅在 FREE 前增加一个 Memory Barrier 是否足够？
4. 如何证明修复后的方案正确？

### **参考分析**

该问题的本质不是 Memory Ordering，而是 Stack Ownership。

当 CPU 0 标记 FREE 后，CPU 1 就有权重新分配这个 Thread。

此时 CPU 0 仍在使用该 Thread 的 Stack。

即使加入：

```
smp_mb();
```

也不能解决问题，因为 CPU 0 在逻辑上仍然使用一个已经转让所有权的资源。

Memory Barrier 不能替代资源生命周期约束。

必须保证：

\\[ \text{Last Use of Stack} \prec \text{Mark Thread FREE} \\]

其中 \\(\prec\\) 表示必须先发生。

验证应包括：

- 双核并发分配和释放。
- 高频进入退出。
- Stack Canary。
- Thread State Trace。
- 必要的汇编级单步验证。
- 对 Thread Slot 重用时序的检查。

**这一题考察能否区分内存排序问题与资源所有权问题。**

## **Question 5 — 一个 Trusted Thread 可以在持有 Secure Mutex 的情况下 RPC Suspend 吗？**

主题：Locking · Blocking · Cross-World Dependency

假设 Thread A：

```
mutex_lock(&storage_mutex);

storage_rpc_write(...);

mutex_unlock(&storage_mutex);
```

`storage_rpc_write()` 内部发生 RPC Suspend。

请判断：

1. 这种写法一定错误吗？
2. 与持有 Spinlock 跨 RPC 的区别是什么？
3. 在什么情况下会出现死锁？
4. 如果不能修改 RPC 协议，该如何降低风险？

### **参考分析**

不能简单断言“任何 Mutex 都不允许跨 RPC”。

关键要区分 Mutex 与 Spinlock 的语义及具体实现约束。

某些设计允许一条逻辑执行流在等待外部服务时继续保持对某项资源的互斥所有权。

但是这样会把锁占用时间扩大到 Normal World 服务延迟。

必须分析：

- RPC 是否会重新进入需要该 Mutex 的代码。
- RPC Service 是否依赖另一个 Trusted Thread。
- 多个 Thread 是否可能形成循环等待。
- Normal World 是否可以无限延迟 RPC。
- Mutex 的 Owner 是否以 Thread Identity 表示。
- 是否存在不可控的长时间持锁。

常见改进方法是缩小锁粒度，将事务准备和结果提交与可能阻塞的 RPC 分离。

不过，如果中间释放锁会破坏事务一致性，就需要引入事务状态、版本校验或其他同步机制，而不是简单地在 RPC 前解锁。

**优秀回答应明确：减少锁持有时间和维持数据一致性之间存在真实的设计权衡。**

## **Question 6 — RISC-V 上跨 Hart Resume 之后偶尔出现 Crypto 数据错误**

主题：RISC-V · Context Switch · FP/Vector · Security

某 RISC-V SoC 将 OP-TEE 移植到 Supervisor TEE Domain。

测试发现：

```
Same Hart Resume:
    PASS

Different Hart Resume:
    Intermittent Crypto Data Corruption
```

普通 GPR、`sepc`、`sstatus` 都已经正确保存。

请给出排查方法。

### **参考分析**

不能仅检查通用寄存器。

应构建完整的 Architected State Inventory：

- GPR。
- FP Register、`fcsr`。
- Vector Register 与相关控制状态。
- `sstatus.FS`、`sstatus.VS`。
- `tp`、`gp` 等 ABI 相关状态。
- 地址空间和 `satp` 相关状态。
- TLB 与 ASID 管理。
- Cache Coherency。
- Domain 隔离权限。
- 使用的硬件加速器是否存在 Per-Hart State。

排查首先应确定：

**错误与 Hart Migration 有因果关系，还是仅仅与并发增加有关？**

实验上可以：

- 固定 Resume Hart。
- 允许迁移但关闭 FP/Vector 相关操作。
- 单独验证 FP/Vector 保存恢复。
- 比较迁移前后的相关寄存器状态。
- 检查是否错误复用了 Hart-local Scratch。
- 验证所有 Hart 都满足相同的扩展功能要求。

不能一开始就将问题归因于 Cache Coherency。

这道题的核心是：**上下文完整性的定义必须覆盖该执行流实际使用的所有架构状态。**

## **Question 7 — Linux Task 被 Kill，Suspended Trusted Thread 应如何处理？**

主题：Cancellation · Lifetime · Fault Recovery

假设：

```
Linux Task A
    |
    v
TEE Invoke
    |
    v
Trusted Thread 0
    |
    v
RPC Suspend
```

在 RPC 期间，Linux Task A 被终止。

要求：

1. OP-TEE 是否应该立即释放 Trusted Thread 0？
2. 谁有责任清理这个执行流？
3. 如果关联的 Shared Memory 被释放，会有什么风险？
4. 如何设计可靠的 Cancellation/Recovery Protocol？

### **参考分析**

不能因为 Linux Task 退出，就直接将 Trusted Thread 0 标记 FREE。

它仍然拥有：

- 未完成的 Kernel Stack。
- 寄存器和 Resume PC。
- RPC State。
- 可能存在的 TA Session 引用。
- MOBJ 和其他资源引用。

如果直接释放，可能导致 Use-After-Free 或资源泄漏。

可靠方案需要定义：

- 请求取消是怎样传达到 TEE 的。
- Cancellation 是即时生效还是只在安全检查点生效。
- 未完成 RPC 是否可以取消。
- Shared Memory 是否仍被引用。
- Session 和 TA Context 如何释放。
- 当 Normal World 永远不返回时，系统如何处理资源耗尽。

不能假设 Linux Task 的死亡就能自动取消所有 Secure World 资源。

还必须考虑恶意 Normal World 永不配合的情况，以及 OP-TEE 在当前安全模型下能保证什么、不能保证什么。

**这道题本质是在设计一个跨信任域的分布式资源回收协议。**

## **Question 8 — 如何为 Trusted Thread 增加优先级？**

主题：Scheduling Policy · RTOS Design · Priority Inversion

某项目希望支持高优先级安全任务：

```
High Priority:
    Secure Watchdog Service

Medium Priority:
    Key Management

Low Priority:
    File Storage
```

要求 OP-TEE 优先处理 Watchdog 请求。

是否应该直接给 `struct thread_ctx` 增加：

```
int priority;
```

### **参考分析**

只增加 Priority 字段并不能实现优先级调度。

因为经典 OP-TEE 没有负责从 Runnable Thread Queue 中挑选任务的 Scheduler。

你需要先确定问题到底是什么：

- 希望新的请求优先获得 Thread Slot？
- 希望高优先级任务能够抢占正在执行的安全服务？
- 希望 RPC 等待时仍能够优先恢复高优先级请求？
- 希望 Secure Interrupt 获得更高响应优先级？
- 希望提供可证明的最坏响应时间？

这些需求对应不同的架构改造。

例如，仅实现 Thread Slot Priority Allocation，不等于实现执行时的抢占调度。

如果需要真正的 Secure Real-Time Scheduler，还需重新考虑：

- Secure Timer。
- Preemption。
- Priority Inheritance。
- CPU Affinity。
- Deadline。
- 安全中断模型。
- 跨世界调度协调。
- 最坏执行时间分析。

**Staff 级答案应该先澄清需要保证的调度语义，再选择实现策略，而不是首先增加优先级字段。**

## **Question 9 — 为什么&#x20;**`CFG_NUM_THREADS`**&#x20;增大后 P99 延迟反而上升？**

主题：Performance · Contention · Queueing Theory

测试结果：

| Thread 数 | Throughput  | P99 Latency |
| -------- | ----------- | ----------- |
| 2        | 1,000 req/s | 30 ms       |
| 4        | 1,500 req/s | 45 ms       |
| 8        | 1,550 req/s | 100 ms      |
| 16       | 1,560 req/s | 240 ms      |

这些数据为假设性测试数据。

请解释这种现象。

### **参考分析**

当线程数量增加时，能够同时进入系统的请求更多。

但是，如果底层瓶颈是某个串行资源，那么增加并发反而会增加等待队列长度。

可能出现：

- Mutex Contention 增大。
- RPC Worker 饱和。
- Secure Storage 请求堆积。
- Crypto Engine Queue 延迟增长。
- Cache Miss 和 TLB Pressure。
- Thread Stack / Page Table 内存压力。
- 更多的上下文和地址空间切换。

因此，吞吐量逐渐达到饱和，而 P99 Latency 却持续增长。

正确优化方向可能是：

- Admission Control。
- Backpressure。
- Request Batching。
- 减少关键路径持锁时间。
- 将同步操作异步化。
- 改善 Backend 并发能力。
- 为关键请求设置独立资源预算。

这道题要求将 OS 执行模型与性能排队理论结合，而不是简单认为“线程多就应该更快”。

## **Question 10 — 安全 DMA Engine 执行 100 ms，应该一直占用 Trusted Thread 吗？**

主题：Asynchronous I/O · DMA · Secure Interrupt · Resource Lifetime

假设某 Crypto Engine：

```
DMA Submit:      10 us
Hardware Execute: 100 ms
DMA Completion:   Secure Interrupt
```

有三种设计方案：

A. Trusted Thread 一直 Polling，等待硬件完成。

B. Trusted Thread 等待某种通知机制，让出 CPU，之后恢复。

C. 使用独立的异步 Request Framework，由其他调用获取结果。

如何选择？

### **参考分析**

方案 A 实现简单，但会长时间占用 CPU，尤其不适合有限 Thread Pool 和高并发场景。

方案 B 可以释放 CPU 的执行机会，但不一定释放 Thread Slot。

如果等待期间 Thread 仍处于 SUSPENDED，就仍然占用一个 Slot。

方案 C 能够将硬件请求生命周期与 Trusted Thread 生命周期分离：

```
Submit Request
      |
      v
Hardware Request Object
      |
      v
Return Handle
      |
      v
Hardware Completion
      |
      v
Get Result / Notification
```

但它要求重新设计：

- Request Ownership。
- Request Authentication。
- DMA Buffer Pinning。
- Secure Memory Mapping Lifetime。
- Device Completion。
- Cancellation。
- Hardware Reset。
- Key Material Lifetime。
- Timeout。
- Request ID 重用问题。

还需要考虑 GP API 的同步调用语义：如果 API 要求一次调用内完成，单纯返回异步 Handle 就改变了接口契约。

因此，没有简单的“C 一定最好”。

最佳设计取决于服务 API、调用频率、硬件延迟、安全要求和可接受的复杂度。

## **Question 11 — 虚拟化系统中，恶意 Guest 占满全部 Trusted Thread**

主题：Virtualization · Isolation · Availability · Fairness

某 SoC 运行三个 Guest：

```
Guest A: Android
Guest B: Linux
Guest C: Safety-critical OS
```

它们共享 OP-TEE 服务。

Guest A 持续发起大量可能长时间挂起的安全服务请求，导致 Trusted Thread Pool 被占满。

Guest C 无法得到服务。

请设计一个可实施的资源隔离方案。

### **参考分析**

这个问题表明：

**内存隔离和身份隔离并不自动提供服务可用性隔离。**

即使三个 Guest 的 Session、内存和调用身份完全正确，仍然可能共享一个有限的 Thread Pool。

设计可以考虑：

- 每个 Guest 的最大 Outstanding Request。
- Per-Guest Quota。
- Admission Control。
- 对 Critical Guest 的资源保留。
- Guest-specific Timeout。
- 优先级与公平性策略。
- 对不可取消请求的安全处理。
- 对恶意 Guest 的 Rate Limiting。

但不能仅在外层对新请求限流。

还必须考虑：

- Guest A 已有的 Suspended Thread 是否长期占用资源。
- Guest C 是否依赖 Guest A 拥有的共享服务。
- Thread Pool 配额是否与安全硬件和 Memory Quota 一致。
- 内部高优先级服务是否需要不依赖 Guest 配额的执行路径。

如果需要严格的实时性保证，可能还需要分离独立的执行环境。

## **Question 12 — 设计一个可以被证明正确的 Thread Suspend/Resume Framework**

主题：Formal Reasoning · State Machine · Architecture Review

最后一道题，不再限制你使用当前 OP-TEE 实现。

假设你要为一个新的 RISC-V TEE 架构重新设计 Trusted Thread Framework。

要求：

- 16 Hart。
- 支持多个 TEE Domain。
- User TA。
- RPC。
- Secure Interrupt。
- FP/Vector。
- 可跨 Hart Resume。
- 支持 Suspend/Resume。
- 支持恶意 Normal World。

请定义：

1. Thread State Machine。
2. Thread Ownership Model。
3. Per-Hart State。
4. Per-Thread State。
5. Domain Switch Contract。
6. Memory Isolation Contract。
7. Recovery Protocol。
8. Test Strategy。

### **参考分析：首先定义系统不变量**

例如：

**Invariant A — Unique Execution Ownership**

\\[ \forall t,\quad \text{ExecutingCPUs}(t)\leq1 \\]

同一执行流不得同时在两个 Hart 上运行。

**Invariant B — Thread Slot Ownership**

\\[ \text{state}(t)=FREE \Rightarrow \text{NoLiveExecution}(t) \\]

只有当不存在尚未完成的执行流，并且相关资源清理达到安全要求时，才允许重用 Slot。

**Invariant C — Domain Correctness**

\\[ \text{Run}(t,h) \Rightarrow \text{Domain}(h)=\text{RequiredDomain}(t) \\]

当某个 Hart 执行 Thread 时，它必须处于允许该 Thread 运行的正确隔离域。

**Invariant D — Context Integrity**

从 Suspend 到 Resume，所有继续执行所需的状态必须保持一致。允许受控变化，但不能出现未定义的寄存器、内存映射或权限状态。

**Invariant E — Memory Lifetime**

任何仍然可被 Thread、DMA Engine 或异步服务访问的内存对象，都不能提前被释放或转让给不兼容的所有者。

### **然后设计协议，而不是直接写代码**

例如：

```
Request Arrives
       |
       v
Validate Domain / Request
       |
       v
Allocate Thread
       |
       v
Bind to Hart
       |
       v
Restore Execution State
       |
       v
Execute
       |
       +---- Complete ----> Safe Release
       |
       +---- Yield -------> Save Context
                                  |
                                  v
                             Suspend
                                  |
                                  v
                         Validate Resume
                                  |
                                  v
                          Restore Domain
                                  |
                                  v
                         Restore Execution
```

随后为每个状态转换定义：

- 前置条件。
- 需要持有的锁。
- 资源所有权变化。
- Memory Ordering。
- 跨 Hart 同步。
- 失败恢复。
- 可以观测的后置条件。

最后利用以下测试进行验证：

- Concurrent Allocation。
- Duplicate Resume。
- Cross-Hart Migration。
- Foreign Interrupt。
- RPC Timeout。
- Guest Crash。
- Domain Switch Failure。
- FP/Vector State Integrity。
- DMA Completion During Suspend。
- Thread Slot Reuse。
- Malicious Resume Parameters。

这道题没有唯一的标准实现。

我更关注的是：你能否把一个复杂的 OS 执行机制，拆解成一组能够被分析、验证和维护的状态转换协议。

这才是架构设计能力。

# **14.16 本章最终总结**

回到最初的问题：

**OP-TEE Trusted Thread 到底是什么？**

它既不是传统 Linux Task，也不是一个简单的函数调用，更不是 CPU Core 的另一种称呼。

它是 OP-TEE 为承载可能跨越 Secure/Normal World 切换的安全服务，而设计的一种 **可暂停、可恢复、有限数量、具有独立执行上下文的内核执行流抽象**。

理解 OP-TEE Trusted Thread，我认为最重要的是掌握下面六个结论。

| 关键结论                           | 对后续源码修改的意义                           |
| ------------------------------ | ------------------------------------ |
| Thread 不等于 CPU                 | 不能错误依赖固定 CPU 或 Hart                  |
| Thread 不等于 TA Context/Session  | 不能混淆执行流和服务对象生命周期                     |
| SUSPENDED 不等于 FREE             | 必须正确管理未完成请求和资源占用                     |
| RPC 是跨执行域的 Continuation        | 必须保存完整的执行上下文                         |
| Thread State Transition 需要严格同步 | 必须防止双重 Resume 和 Stack Ownership Race |
| OP-TEE 依赖 Normal World 调度进展    | 必须考虑可用性、DoS 和跨世界死锁                   |

## **最后一个思考题**

我想用一个没有直接给出答案的问题作为本章结束。

> **如果未来的 RISC-V TEE 希望拥有独立于 Linux 的调度能力，你认为应该继续扩展 OP-TEE 当前的 Trusted Thread Framework，还是引入一个新的 Secure Scheduler，并让当前 Trusted Thread 成为它调度的对象？**

这实际上涉及三种不同的设计理念：

- OP-TEE：Caller-driven Execution。
- 传统 RTOS：Scheduler-driven Execution。
- 机密计算架构：由多级安全管理组件协作控制执行域与资源。

你需要思考的不是哪种设计在理论上最先进，而是如何在 **执行自主性、TCB 大小、实时性、并发性能、隔离强度以及系统复杂度** 之间选择合理的架构。

**Chapter 14 的真正学习成果，就是开始能够从这种系统级角度判断 OP-TEE 的 Thread Framework 为什么这样设计、哪些假设支撑它，以及当这些假设改变时应该怎样演进。**

下一章 Chapter 15 将继续深入 CPU Register Context、Kernel/User Stack、Exception Entry、Syscall Transition 和 User TA 的执行上下文保存恢复。它会把这一章讨论的执行模型进一步落实到 CPU 寄存器和汇编指令层面。