# **Chapter 11 — SMP Boot & Per-CPU Kernel State**

OP-TEE OS Internals · Part II — Boot & Initialization

**本章主题：** Primary/Secondary CPU Boot、Per-CPU State、Trusted Thread、SMP Synchronization、CPU Power Management，以及多核系统中的安全不变量。

**源码基线：** OP-TEE OS 4.11.0，重点分析 Arm64 与 RISC-V 实现。

## **11.1 本章的核心目标**

这一章的重点不只是解释 OP-TEE 如何启动第二个 CPU，而是深入三个问题：

1. **Ownership：** 一个多核 OP-TEE 系统中，哪些状态属于 CPU，哪些属于 Thread，哪些属于整个 OS？
2. **Concurrency：** 多个 CPU 同时进入 Secure Kernel 时，如何保证全局对象、内存管理和 TA Runtime 的正确性？
3. **Lifecycle：** CPU Boot、Trusted Thread Execution、Suspend/Resume 与 CPU Hotplug 之间，分别有哪些状态需要初始化、保存和恢复？

我们先提出一个看似简单、但能够贯穿全章的问题：

> 假设一颗 SoC 包含 16 个 CPU Core，OP-TEE 配置了 8 个 Trusted Threads。当 16 个 Core 都完成安全侧初始化后，OP-TEE 内部究竟存在 16 个 OS 实例、16 个 Thread Pool，还是一个共享的 OP-TEE Kernel？

正确的理解是：

**在本章讨论的传统单 OP-TEE 实例、多核配置中，16 个 CPU 共享同一个 Secure Kernel 实例和全局 Trusted Thread Pool；每个 CPU 有自己的 Per-CPU 状态，而一个 Trusted Thread 并不永久绑定某个 CPU。**

因此：

- CPU 数量不等于 Trusted Thread 数量。
- Per-CPU State 不等于 Thread Context。
- CPU Boot 不等于 Trusted Thread Allocation。
- CPU Suspend 不等于 Trusted Thread Suspend。
- Primary CPU 负责全局初始化，但不意味着所有安全服务都必须在 Primary CPU 上执行。

理解这些区别，是后续分析 SMP Bug 的基础。

## **11.2 OP-TEE 的 SMP Architecture：从三个维度理解**

### **11.2.1 CPU、Thread 和 Global Kernel State**

我们先建立三个不同的状态域：

One OP-TEE Kernel / Multiple CPUs

CPU 0

Per-CPU State 0

Entry / Stack / Local State

CPU 1

Per-CPU State 1

Entry / Stack / Local State

CPU 2

Per-CPU State 2

Entry / Stack / Local State

CPU 3

Per-CPU State 3

Entry / Stack / Local State

Shared secure address space

Global Secure Kernel

Thread Pool

Thread 0…N-1

TA Runtime

Sessions · Contexts

Memory

MMU · MOBJ · Heap

Services

Crypto · Storage · Drivers

示意图采用 4 Core 的例子。CPU 与 Trusted Thread 是动态关联的，不表示所有 CPU 始终运行安全线程。

### **11.2.2 三类状态分别是什么？**

| State Scope      | 典型内容                                                 | 所有权               | 生命周期                |
| ---------------- | ---------------------------------------------------- | ----------------- | ------------------- |
| Global State     | Thread Pool、TA Context List、Kernel Heap、Driver State | 整个 OP-TEE 实例      | Core 初始化至实例结束       |
| Per-CPU State    | Current Thread ID、临时栈、异常栈、Per-CPU Interrupt State    | 某个物理 CPU          | CPU 初始化与在线期间        |
| Per-Thread State | Saved Registers、Thread Stack、User Mapping、RPC State  | 某个 Trusted Thread | Thread Slot 的重复使用周期 |

此外，还有 TA Context、Session、User Mode Context 等状态，它们既不是 Per-CPU，也不完全是 Per-Thread，而是由 TA Runtime 独立管理。

这是一个非常重要的区别。

例如：

```
struct thread_core_local {
    /* CPU-local execution state */
};

struct thread_ctx {
    /* Trusted Thread execution state */
};
```

前者回答：

> 当前 CPU 正在执行哪个 Trusted Thread？

后者回答：

> 某个 Trusted Thread 当前处于什么状态？如果它被挂起，需要恢复什么？

在 OP-TEE 4.11.0 中，`thread_core_local` 分别在 Arm 和 RISC-V 的架构头文件中定义，`thread_ctx` 则定义在通用的 `core/include/kernel/thread_private.h` 中。这反映了一个重要设计：**CPU-local 的寄存器和执行环境带有架构特性，而 Thread Lifecycle 的基本抽象可以跨架构共享。**&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



### **11.2.3 为什么不能将 CPU 和 Thread 永久绑定？**

假设：

- CPU 0 正在执行 Trusted Thread 2。
- Trusted Thread 2 因 RPC 返回 Normal World，进入 Suspended。
- CPU 0 随后可以处理其他请求。
- 当 RPC 完成时，恢复调用不一定在 CPU 0 上发生。

那么 Trusted Thread 2 可能在 CPU 3 上恢复执行。

因此，Thread Context 必须保存可以跨 CPU 恢复的执行状态，而不能把全部状态永久保存在 CPU 0 的 Per-CPU 结构中。

例如：

```
Time T0:
  CPU 0
    curr_thread = 2

  Thread 2
    state = ACTIVE

Time T1: Thread 2 suspends for RPC
  CPU 0
    curr_thread = INVALID

  Thread 2
    state = SUSPENDED
    saved registers
    saved kernel SP
    saved user mapping

Time T2: RPC resumes on CPU 3
  CPU 3
    curr_thread = 2

  Thread 2
    state = ACTIVE
```

这里的关键不变量是：

\\[ \forall t,\quad \operatorname{ActiveCPUCount}(t)\leq 1 \\]

即：**一个 Trusted Thread 在同一时刻，最多只能被一个 CPU 作为正在执行的 Thread 使用。**

它可以被挂起并迁移，但不能被两个 CPU 同时恢复。

更进一步：

\\[ \text{CPU-local state} \neq \text{Thread-owned state} \\]

这不是简单的代码风格问题，而是保证 SMP 正确性的根本约束。

## **11.3 理解配置参数：CPU 数量不等于 Thread 数量**

先看两个配置：

```
CFG_TEE_CORE_NB_CORE=16
CFG_NUM_THREADS=8
```

这里有两个独立的资源维度：

`CFG_TEE_CORE_NB_CORE` 控制当前配置所支持的 Secure CPU Core 数量上限，涉及 Per-CPU State、Per-CPU Stack、启动同步等资源。在支持 `CFG_DYN_CONFIG` 的配置中，实际运行数量可以在启动时确定，但不能超出配置上限。

`CFG_NUM_THREADS` 控制全局 Trusted Thread 资源的数量；它不是每个 CPU 的线程数量。在动态配置下，实际初始化的 Thread 数也可能小于编译上限。

当前通用接口 `thread_init_threads()` 和 `thread_init_thread_core_local()` 对静态与动态配置的区别都有明确约束。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



假设系统支持 16 个 CPU 和 8 个 Trusted Threads：

| 场景                     | CPU 数量 | Active Trusted Threads | 是否合理             |
| ---------------------- | ------ | ---------------------- | ---------------- |
| 启动完成、没有安全请求            | 16     | 0                      | 是                |
| 4 个 CPU 同时处理 TA 请求     | 16     | 4                      | 是                |
| 8 个 CPU 同时处理 TA 请求     | 16     | 8                      | 是                |
| 16 个 CPU 同时要求独立 Thread | 16     | 最多 8 个                 | 需要处理 Thread 资源不足 |
| 4 个 CPU 同时调用单实例 TA     | 16     | 最多 4 个用于这些请求           | 还受 TA 本身的并发约束    |

这里需要进一步区分“CPU 正在执行 Secure World 代码”与“CPU 正在运行一个 Active Trusted Thread”。

Fast Call、Exception Handling 和某些初始化路径可以使用特殊的栈与执行上下文，不一定占用一个普通 Trusted Thread Slot。

**因此，不能用&#x20;**`CFG_NUM_THREADS`**&#x20;直接限制所有 CPU 进入安全侧的次数，也不能仅通过增加 CPU 数量解决 Thread Exhaustion。**

这将成为 Chapter 14 Trusted Thread 和 Chapter 17 Synchronization 的重要基础。

# **11.4 Primary CPU Boot：为什么全局初始化只能执行一次？**

## **11.4.1 首先区分两种初始化**

从 OS 设计的角度，初始化可以分成两类。

**Global Initialization**

只应对整个 OP-TEE 实例执行一次，包括：

- 建立全局内存管理结构。
- 初始化 Kernel Heap。
- 初始化 Trusted Thread Pool。
- 初始化 TA Manager 和全局服务。
- 初始化全局 Driver Context。
- 注册 Initcall、配置全局共享资源。

**Per-CPU Initialization**

每个参与 Secure World 执行的 CPU 都需要完成，包括：

- 建立 CPU-local Stack。
- 设置 CPU-local Exception Vector。
- 初始化 CPU-local Interrupt Controller Interface。
- 初始化本 CPU 所需的安全控制寄存器。
- 建立 Current Thread 指针或索引。
- 根据架构和配置恢复 FPU、SIMD 等 CPU-local 状态。

二者最大的区别不是初始化代码复杂程度，而是**它们的执行次数与数据所有权不同**。

一个全局资源可以被多个 CPU 使用，但这不意味着每个 CPU 都应该重新初始化它。

例如：

```
/* 示意代码：不是 OP-TEE 原样实现 */

void init_primary_cpu(void)
{
    init_global_heap();
    init_global_thread_pool();
    init_global_services();

    init_this_cpu();
}

void init_secondary_cpu(void)
{
    init_this_cpu();
}
```

这个示例有一个非常重要的隐含要求：

> Secondary CPU 开始访问全局对象时，Primary CPU 必须已经完成必要的初始化，并且相关写入必须对 Secondary CPU 可见。

这既是初始化顺序问题，也是 Memory Ordering 问题。

## **11.4.2 Arm64 的真实初始化路径**

基于 OP-TEE 4.11.0，我们可以将 Arm Primary CPU 的核心路径简化成：

```
core/arch/arm/kernel/entry_a64.S
    _start
      |
      +-- Early CPU setup
      +-- BSS initialization
      +-- Initial stack / MMU setup
      |
      +-- boot_init_primary_early()
      |      |
      |      +-- init_primary()
      |             Memory / Pager preparation
      |             Per-CPU early setup
      |
      +-- boot_init_primary_late()
      |      |
      |      +-- Device Tree setup
      |      +-- thread_init_threads()
      |      +-- thread_init_boot_thread()
      |      +-- thread_init_thread_core_local()
      |
      +-- boot_init_primary_runtime()
      |      |
      |      +-- thread_init_primary()
      |      +-- boot_primary_init_intc()
      |      +-- init_tee_runtime()
      |
      +-- boot_init_primary_final()
             |
             +-- Driver initcalls
             +-- Final initcalls
```

这是典型非虚拟化配置下的主线；具体顺序、条件分支和额外操作应以对应配置的实际执行路径为准。

注意 `CFG_NS_VIRTUALIZATION` 会改变 Thread 初始化的时机：在相关配置中，Thread 属于 Guest-associated TEE State，并不是简单地在 Primary Boot 时初始化同一个全局 Thread Pool。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



### **为什么&#x20;**`thread_init_boot_thread()`**&#x20;很重要？**

研究以下源码：

`core/kernel/thread.c`

```
void thread_init_boot_thread(void)
{
    struct thread_core_local *l = thread_get_core_local();

    l->curr_thread = 0;
    threads[0].state = THREAD_STATE_ACTIVE;
}
```

这是 4.11.0 中的实际实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



它做了两件事：

1. 将当前 CPU 的 `curr_thread` 设置为 0。
2. 将全局 Thread 0 的状态设置为 ACTIVE。

为什么 Boot 过程需要一个 Trusted Thread？

因为进入较完整的 Kernel Runtime 后，某些操作可能依赖当前存在合法的 Thread Context。

例如，代码可能需要调用：

```
thread_get_id();
thread_get_tsd();
thread_get_ctx_regs();
```

如果当前 CPU 没有合法的 Thread Context，这些操作就可能不成立。

因此，OP-TEE 借用了 Thread 0 作为 Boot-time Execution Context。

这并不意味着 Thread 0 永久属于 Primary CPU。

当初始化完成时：

```
void thread_clr_boot_thread(void)
{
    struct thread_core_local *l = thread_get_core_local();

    /* Assertions omitted here */
    threads[l->curr_thread].state = THREAD_STATE_FREE;
    l->curr_thread = THREAD_ID_INVALID;

    print_stack_limits();
}
```

Thread 0 被释放回普通 Thread Pool。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里有一个值得思考的问题：

**如果你在 Boot Thread 中启动了一个异步操作，而该操作引用了 Boot Thread 的 Thread-Specific Data，会怎样？**

即使 CPU 初始化已经结束，这个异步操作也不一定完成。

如果初始化流程直接释放 Thread 0，另一个 Trusted Thread 可能重新使用这个 Slot。

这可能造成：

- Lifetime Violation。
- Use-After-Reuse。
- 错误的 Session Context。
- 不安全的异步回调状态。

因此，Boot Thread 所执行的初始化路径不能随意把 Thread-owned State 泄漏给晚于 Boot 阶段执行的任务。

这是一个典型的 OS 生命周期设计问题。

# **11.5 Secondary CPU Boot：从 Firmware 到 OP-TEE**

## **11.5.1 谁负责真正启动 Secondary CPU？**

这里需要区分两个概念：

**Physical CPU Bring-up**：让一个 CPU 从 Reset/Power-Off/Stopped State 进入执行状态。

**OP-TEE Secondary Initialization**：在该 CPU 上建立能够安全执行 OP-TEE 的本地环境。

二者可能由不同的 Firmware 组件负责。

对于常见 Armv8-A + TF-A 系统：

```
Linux CPU_ON request
        |
        v
TF-A / EL3 PSCI Implementation
        |
        +-- Power on target CPU
        +-- Initialize EL3 context
        +-- Restore secure/non-secure setup
        |
        v
OP-TEE Secondary Entry
        |
        +-- CPU-local stack
        +-- CPU-local registers
        +-- Exception vectors
        +-- Interrupt controller CPU interface
        |
        v
Return to EL3 / Normal World
```

这里并不是 OP-TEE 自己完成所有 Power Domain 控制。

在 TF-A 管理 PSCI 的配置下，OP-TEE 负责自身的安全侧初始化，EL3 Firmware 负责更高权限的 CPU Power Management 协调。

对于部分 Arm32 或没有外部 TF-A Secure Monitor 的旧平台，职责可能不同。因此不能笼统地说所有 Arm 平台都由 TF-A 启动 Secondary CPU。

## **11.5.2 实际源码：Arm Secondary Initialization**

我们看：

`core/arch/arm/kernel/boot.c`

核心代码如下，保留与本章有关的逻辑：

```
static void init_secondary_helper(void)
{
    IMSG("Secondary CPU %zu initializing",
         get_core_pos());

    thread_set_exceptions(THREAD_EXCP_ALL);

    secondary_init_cntfrq();
    thread_init_per_cpu();
    boot_secondary_init_intc();

    init_vfp_sec();
    init_vfp_nsec();

    IMSG("Secondary CPU %zu switching to normal world boot",
         get_core_pos());
}
```

这一段与 4.11.0 上游一致。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们逐步分析。

### **Step 1：**`thread_set_exceptions(THREAD_EXCP_ALL)`

Secondary CPU 初始化时，需要控制异步异常的到达。

原因是此时可能还没有：

- 完整的 Thread Context。
- 正确的异常处理 Stack。
- 初始化完成的 Interrupt Controller Interface。
- 可以安全访问的全局状态。

如果在这个阶段允许不可控的异步中断进入，异常处理程序可能访问尚未初始化的状态。

因此，首先屏蔽相关异步异常。

这不是说整个 Boot 阶段都不能发生任何异常；同步异常仍可能产生，但一般意味着需要通过早期异常路径诊断严重的初始化问题。

### **Step 2：**`secondary_init_cntfrq()`

这是一个非常有代表性的 Per-CPU Hardware State Initialization。

当前源码中，在启用 `CFG_SECONDARY_INIT_CNTFRQ` 的配置下：

```
static void secondary_init_cntfrq(void)
{
    assert(cntfrq != 0);
    write_cntfrq(cntfrq);
}
```

对应的 Primary 路径先读取已经由前级 Firmware 设置的 `CNTFRQ`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



它体现了一种常见设计：

- 系统级参数由 Primary 或上一级 Firmware 确定。
- 每个 CPU 都需要拥有正确的本地寄存器状态。
- Secondary CPU 不应该自行生成与其他 CPU 不一致的配置。

这里的安全不变量是：

\\[ \forall c \in \text{OnlineCPUs},\quad \operatorname{CNTFRQ}(c)=\operatorname{SystemFrequency} \\]

如果 CPU 0 和 CPU 1 使用了不一致的 Timer Frequency，时间换算、Timeout 和 Profiling 都可能受到影响。

### **Step 3：**`thread_init_per_cpu()`

在 Arm64 上，该函数最终会设置当前 CPU 的异常向量表，此外根据配置还可能调整用户态性能计数器访问能力。

当前 `thread_init_per_cpu()` 的一个重要调用是：

```
thread_init_vbar(get_excp_vect());
```

这里为什么是每个 CPU 都需要执行？

因为 `VBAR_EL1` 是 CPU-local System Register。

Primary CPU 为自己设置 `VBAR_EL1`，并不会自动替 Secondary CPU 完成设置。

即使各 CPU 使用同一个 Exception Vector Table 地址，也必须分别配置对应的 CPU 寄存器。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **Step 4：**`boot_secondary_init_intc()`

这个函数是一个 Weak Platform Hook。

不同平台可以提供自己的实现，例如：

```
void boot_secondary_init_intc(void)
{
    gic_init_per_cpu();
}
```

这类实现可以在 OP-TEE 的 TI K3 平台代码中看到。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



为什么 Primary 初始化 Interrupt Controller 后，Secondary 还需要执行？

因为 Interrupt Controller 通常既有 Global/Distributor State，也有 CPU-local Interface 或 Redistributor State。

以 Arm GICv3 为例：

- Distributor 管理全局中断属性和路由。
- Redistributor 包含与单个 PE 相关的中断状态。
- CPU Interface 包含与当前 PE 相关的 System Register State。

不同硬件配置还可能涉及 Secure Group、Priority Mask、SGI/PPI、Redistributor Power State 等。

**初始化 Interrupt Controller，不代表每个 CPU 的 Interrupt Interface 都已经准备就绪。**

### **Step 5：VFP/SIMD 状态**

`init_vfp_sec()` 和 `init_vfp_nsec()` 处理当前 CPU 的相关执行状态。

因为这些寄存器属于 CPU 执行环境，不是一个全局 C 结构体就能够替代的。

这还引出一个更复杂的问题：

如果一个 Secure Thread 在 CPU 0 上使用过 Floating-Point Registers，随后迁移到 CPU 1，那么如何防止 CPU 1 原有状态泄漏给该线程？

这属于 Thread Context Switch 与 CPU-local Register State 的交界处，也是 OP-TEE Lazy VFP Save/Restore 设计需要解决的问题。

# **11.6 Per-CPU State 的真实数据结构**

我们来仔细研究最重要的结构体。

## **11.6.1 Arm64：**`struct thread_core_local`

源码：

`core/arch/arm/include/kernel/thread_arch.h`

在 4.11.0 中，可以提取出如下关键字段：

```
struct thread_core_local {
#ifdef ARM64
    uint64_t x[4];
#endif

#ifdef CFG_CORE_PAUTH
    struct thread_pauth_keys keys;
#endif

    vaddr_t tmp_stack_va_end;
    long kcode_offset;

    short int curr_thread;
    uint32_t flags;

    vaddr_t abt_stack_va_end;

#ifdef CFG_TEE_CORE_DEBUG
    unsigned int locked_count;
#endif

    /* Additional conditional fields omitted */
};
```

这是简化后的实际结构，而不是一个独立的教学定义。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们按功能分类。

| 字段                 | 作用                               | 关键风险               |
| ------------------ | -------------------------------- | ------------------ |
| `x[]`              | 在低层入口/异常路径暂存寄存器                  | 嵌套异常覆盖             |
| `tmp_stack_va_end` | CPU-local Temporary Stack        | 栈越界、错误 Core Index  |
| `curr_thread`      | 当前 CPU 的 Trusted Thread ID       | 双重分配、错误恢复          |
| `flags`            | 当前 CPU 的 Stack/Exception 模式      | 错误 Stack Selection |
| `abt_stack_va_end` | CPU-local Abort Stack            | 异常处理时栈无效           |
| `locked_count`     | Debug 模式下的锁持有检查                  | 无法正确追踪锁状态          |
| `keys`             | 配置启用时的 CPU-local PAuth Key State | Key 初始化或恢复错误       |

这里有两个值得单独深入的字段。

### `curr_thread`**：CPU 与 Thread 的桥梁**

假设：

```
thread_core_local[2].curr_thread = 5;
```

它表示：

> 当前逻辑 CPU 2 正在使用 Trusted Thread Slot 5。

通过这个关系，低层异常代码可以找到：

```
threads[5].regs;
threads[5].stack_va_end;
threads[5].tsd;
```

但请注意：

`curr_thread` 不是一个保存完整 Thread Context 的地方。

它只是当前 CPU 与全局 Thread Context 之间的关联索引。

如果你曾经研究过 NuttX 的 `current_task`、Linux 的 per-CPU `current`，可以用它们来帮助理解，但不能直接套用 Linux 的抢占调度模型。

### `flags`**：为什么需要知道当前使用哪种 Stack？**

在 Exception Handler 中，不是所有时候都可以安全使用当前 Thread Stack。

例如：

```
CPU 2
  |
  +-- No active trusted thread
  |      |
  |      +-- Temporary Stack
  |
  +-- Active trusted thread
  |      |
  |      +-- Thread Stack
  |
  +-- Kernel Abort
         |
         +-- Abort Stack
```

如果异常发生时当前线程栈已经溢出，再继续在同一个栈上进行错误处理，可能造成更严重的内存破坏。

所以 OP-TEE 需要区分当前处于普通 Thread Execution、Temporary Stack 还是 Abort Handling。

在源码中，`THREAD_CLF_TMP`、`THREAD_CLF_ABORT` 等标志就是这一类机制的基础。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **11.6.2 Arm64 一个很有特点的设计：SP_EL1 保存 Per-CPU State**

看 `entry_a64.S` 的 `set_sp` Macro。

其核心行为是：

```
msr spsel, #0
add sp, x1, x0

bl  thread_get_core_local

msr spsel, #1
mov sp, x0

msr spsel, #0
```

对应含义：

1. 选择 `SP_EL0`，设置 Temporary Stack。
2. 获取当前 CPU 的 `thread_core_local` 地址。
3. 切换到 `SP_EL1`。
4. 把 `SP_EL1` 设置为 Per-CPU State 地址。
5. 再切回 `SP_EL0`。

上游源码直接通过这种方式把 `SP_EL1` 用作定位 CPU-local 数据的机制。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么这么设计？

因为在 AArch64 中：

- Secure Kernel 通常运行于 S-EL1。
- 可以选择使用 `SP_EL0` 或 `SP_EL1`。
- OP-TEE 的普通执行路径可以使用 `SP_EL0` 作为运行栈。
- 另一个栈指针寄存器可作为高效访问 Per-CPU State 的入口。

这属于特定架构下对寄存器资源的巧妙利用。

但不要将此理解为所有 AArch64 OS 都会这样做。

Linux、TF-A、OP-TEE 对异常级、Stack Pointer 和 Per-CPU State 的设计并不相同。

# **11.7 Thread Pool 与 Per-CPU State 如何协作？**

现在来看一个非常重要的操作：多个 CPU 同时分配 Trusted Thread。

假设 Thread Pool 包含 4 个 Slot：

```
threads[0] FREE
threads[1] FREE
threads[2] FREE
threads[3] FREE
```

此时 CPU 0 和 CPU 1 同时进入：

```
thread_alloc_and_run(...);
```

如果没有同步，就可能发生：

```
CPU 0                        CPU 1
  |                            |
  v                            v
read threads[0].state       read threads[0].state
  |                            |
FREE                         FREE
  |                            |
  v                            v
select Thread 0             select Thread 0
```

最终两个 CPU 都认为自己拥有 Thread 0。

这是非常严重的错误，因为二者可能同时修改：

- Thread 0 的 Saved Registers。
- Thread 0 的 Kernel Stack。
- Thread 0 的 User Mapping。
- Thread 0 的 Thread-Specific Data。

结果不仅可能是 Crash，还可能破坏 TA 之间的隔离。

## **11.7.1 OP-TEE 如何防止这种情况？**

在 Arm 的 Thread Allocation 实现中，核心逻辑是：

```
/* 保留关键步骤的简化代码 */

thread_lock_global();

for (n = 0; n < CFG_NUM_THREADS; n++) {
    if (threads[n].state == THREAD_STATE_FREE) {
        threads[n].state = THREAD_STATE_ACTIVE;
        found_thread = true;
        break;
    }
}

thread_unlock_global();
```

这个 Global Lock 保护的是 Thread Pool 的共享状态，而不是某个 CPU-local 变量。

这说明：

**Per-CPU Data 可以减少局部状态的共享，但不能自动消除全局资源分配中的竞争。**&#x20;

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



关键的临界区是：

```
Check FREE
    +
Select Slot
    +
Mark ACTIVE
```

三个步骤必须作为一个受同步保护的状态转换。

## **11.7.2 为什么不能一直持有 Global Lock？**

假设分配 Thread 以后继续持有 Global Lock，然后执行 TA：

```
lock_global();

allocate_thread();
execute_ta();

unlock_global();
```

这显然是不合理的。

原因包括：

- TA 执行可能持续很长时间。
- TA 可能调用其他 Secure Services。
- TA 可能进行 RPC。
- 其他 CPU 可能需要 Thread Pool。
- 有些服务可能间接再次获取相关锁。

这种设计容易造成高锁竞争，甚至 Deadlock。

因此，我们需要明确：

**Global Lock 保护的是特定状态转换，而不是整个安全请求的执行过程。**

这也是一个值得牢记的并发设计原则：

\\[ \text{Lock Scope} \approx \text{Invariant Protection Scope} \\]

即：锁的持有范围应由需要保证原子性的状态决定，而不是简单覆盖整个操作流程。

# **11.8 RISC-V SMP Boot：从 Hart Lottery 到 Per-Hart State**

这一部分结合 RISC-V 版本的真实源码来分析。

在 OP-TEE 4.11.0 中，RISC-V 已有独立的 Boot Entry、Hart Initialization、SBI HSM 和多 Hart 同步实现。下面直接以这部分代码为依据，而不是把 Arm 的执行流程简单换成 RISC-V 指令。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **11.8.1 RISC-V 的 Primary Hart Selection**

源码：

`core/arch/riscv/kernel/entry.S`

核心代码：

```
lla     a3, hart_lottery
li      a2, 1
amoadd.w a3, a2, (a3)

csrw    CSR_XSCRATCH, a3

bnez    a3, reset_secondary
jal     reset_primary
```

这段代码很值得深入。

### **第一行**

```
lla a3, hart_lottery
```

获取 `hart_lottery` 变量的地址。

这个变量用于对进入该入口的 Hart 进行原子编号。

### **第二行**

```
li a2, 1
```

准备一个增量值 `1`。

### **第三行**

```
amoadd.w a3, a2, (a3)
```

这是一条 Atomic Memory Operation。

它的逻辑效果是：

```
old = atomic_fetch_add(&hart_lottery, 1);
a3 = old;
```

因此，假设三个 Hart 依次赢得原子操作：

```
Hart A: old = 0
Hart B: old = 1
Hart C: old = 2
```

只有第一个获得 `0` 的 Hart 会执行 Primary Boot。

注意这里的 Primary Hart 不一定是 `mhartid == 0`。

这是一个十分重要的设计细节。

**Primary 的身份由参与启动的 Hart 之间的原子选举决定，不是直接由物理 Hart ID 决定。**

### **第四行**

```
csrw CSR_XSCRATCH, a3
```

将这个逻辑索引暂存到当前执行模式所使用的 Scratch CSR 中。

这里的 `CSR_XSCRATCH` 由 RISC-V 构建配置决定，可对应相应特权模式下的 Scratch CSR。

### **最后两行**

```
bnez a3, reset_secondary
jal  reset_primary
```

- 逻辑索引为 0，进入 `reset_primary`。
- 其他索引进入 `reset_secondary`。

这种方式把物理 Hart Identification 和 OP-TEE 内部 Logical Core Index 分开。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **11.8.2 为什么 Hart ID 与 Hart Index 必须区分？**

假设 SoC 的物理 Hart ID 为：

```
Hart IDs:
  0
  2
  4
  8
```

如果使用：

```
thread_core_local[hartid]
```

那么最后一个 Hart 会访问：

```
thread_core_local[8]
```

但整个系统实际上只有 4 个 Hart。

这样的数组访问方式既浪费资源，也可能越界。

因此正确的设计通常需要：

```
Physical Hart ID     Logical Core Index
       0       --->        0
       2       --->        1
       4       --->        2
       8       --->        3
```

在 OP-TEE 的 RISC-V `thread_core_local` 中，我们可以看到：

```
struct thread_core_local {
    unsigned long x[4];

    uint32_t hart_id;
    uint32_t hart_index;

    vaddr_t tmp_stack_va_end;
    short int curr_thread;
    uint32_t flags;
    vaddr_t abt_stack_va_end;

    /* Conditional fields omitted */
};
```

这里同时保存：

- `hart_id`：物理架构 Hart Identifier。
- `hart_index`：OP-TEE 内部的逻辑索引。

这两个值不能混为一谈。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



不过还需要区分另一个问题：上面的 `hart_lottery` 分配的是**启动到达顺序索引**，而 `hartids[]` 可以来自 Device Tree 描述。真实平台必须确认二者的映射和启动顺序约定是否满足软件需求。

如果某个 Hart 被跳过、提前启动或被重复启动，就需要重新审查这种索引分配机制是否仍然成立。

## **11.8.3 RISC-V 如何建立 Per-Hart State？**

我们研究 `entry.S` 的 `set_tp` Macro：

```
.macro set_tp
    csrr t0, CSR_XSCRATCH

    li   t1, THREAD_CORE_LOCAL_SIZE
    mul  t2, t1, t0

    lla  tp, thread_core_local
    LDR  tp, 0(tp)

    add  tp, tp, t2

    sw   s0, THREAD_CORE_LOCAL_HART_ID(tp)
    sw   t0, THREAD_CORE_LOCAL_HART_INDEX(tp)
.endm
```

这段汇编做了几件事情。

首先：

```
csrr t0, CSR_XSCRATCH
```

获取之前保存的 Logical Hart Index。

随后：

```
li  t1, THREAD_CORE_LOCAL_SIZE
mul t2, t1, t0
```

计算：

\\[ \text{offset} = \text{hart\\\_index} \times \operatorname{sizeof}(\text{thread\\\_core\\\_local}) \\]

再通过：

```
lla tp, thread_core_local
LDR tp, 0(tp)
add tp, tp, t2
```

得到当前 Hart 对应的 Per-Hart State 地址。

最后：

```
sw s0, THREAD_CORE_LOCAL_HART_ID(tp)
sw t0, THREAD_CORE_LOCAL_HART_INDEX(tp)
```

保存物理 Hart ID 和内部索引。

**这里的&#x20;**`tp`**&#x20;并不是 TA 用户态的 TLS 指针，而是在 OP-TEE Kernel 入口环境中用来定位 Per-Hart State 的寄存器。**

当进入 User TA 后，`tp` 可能具有用户态上下文意义，因此 Trap Entry 必须正确区分和保存两种状态。

RISC-V 的 `thread_rv.S` 中通过 `CSR_XSCRATCH` 与 `tp` 交换，并结合 Trap 来源判断，处理 Kernel 与 User Context 的切换。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



这一点很重要，因为它同时涉及：

- Per-Hart Kernel State。
- User TLS State。
- Trap Entry。
- Register Preservation。
- Secure Kernel 与 User TA 的特权切换。

如果 Porting 时只关注 `sp` 和 `sepc`，却忽略 `tp` 与 Scratch CSR 的约定，就可能出现非常难排查的异常恢复问题。

# **11.9 Multi-CPU Boot Synchronization：为什么其他 CPU 必须等待 Primary？**

现在我们进入本章非常重要的一部分：**Boot Synchronization 与 Memory Ordering**。

## **11.9.1 最危险的启动竞态**

假设存在下面两个 CPU：

```
Primary CPU                    Secondary CPU
    |                               |
    v                               v
Clear .bss                      Enter OP-TEE
    |                               |
Initialize Heap                 Access global heap
    |                               |
Initialize Thread Pool          Read thread_core_local[]
    |                               |
Initialize MMU                  Enable interrupts
    |                               |
Global Init Done
```

问题在于，Secondary CPU 可能在全局数据尚未准备好时开始访问它。

这种情况下，可能出现：

- Heap Metadata 尚未初始化。
- Thread Pool 中的状态不可用。
- Page Table 尚未完成。
- Interrupt Controller 的全局路由状态尚未建立。
- Global Lock 本身尚未初始化。
- Secondary CPU 看到了旧的内存内容。

因此我们必须建立一个正式的启动状态机。

SMP Boot Synchronization

Primary CPU

Early Boot

Global Init

Publish READY

Wait Secondaries

Secondary CPU

Early Entry

Wait for READY

Acquire State

Per-CPU Init

逻辑时序示意。具体平台可能先由 Firmware 停放 Secondary CPU，随后再启动；不一定在 OP-TEE 内部等待。

## **11.9.2 实际 RISC-V 实现：**`wait_primary`

4.11.0 的 `core/arch/riscv/kernel/entry.S` 定义了：

```
.macro wait_primary
#ifdef CFG_BOOT_SYNC_CPU
    lla t0, sem_cpu_sync
    li  t2, 1

1:
    lw  t1, 0(t0)
    bne t1, t2, 1b

    fence r, rw
#endif
.endm
```

关键问题不是这个循环如何跳转，而是：

**为什么需要最后的&#x20;**`fence`**？**

Primary 在发布 READY 时使用了写方向的 Fence：

```
fence rw, w
sw    t2, 0(t1)
```

它表达的意图是：

1. Primary 先完成此前必要的读写。
2. 再发布 READY。
3. Secondary 看到 READY 后，对之后的内存访问建立正确的顺序约束。

这一生产者/消费者式同步采用了 RISC-V Fence 指令。上述实现来自 4.11.0 的 `entry.S`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



需要注意：Fence 只解决对应内存访问顺序问题。完整的可见性证明还依赖内存类型、缓存一致性、原子访问约定和平台的执行模型。不能认为“加一条 Fence 就解决所有 SMP 竞态”。

## **11.9.3 为什么&#x20;**`volatile`**&#x20;不够？**

假设有人设计：

```
volatile int ready;

void primary(void)
{
    initialize_global_data();
    ready = 1;
}

void secondary(void)
{
    while (!ready)
        ;

    use_global_data();
}
```

仅仅将 `ready` 声明为 `volatile`，不能在 C 语言内存模型下保证线程间同步，也不能自动提供跨 CPU 的 Acquire/Release Ordering。

更合适的抽象模型是：

```
/* C11 内存模型示意，不是 OP-TEE Boot 代码 */

atomic_store_explicit(&ready, 1, memory_order_release);
```

消费者：

```
while (!atomic_load_explicit(&ready,
                            memory_order_acquire))
    ;
```

这代表：

\\[ \operatorname{Initialize}(G) \\;\xrightarrow{\text{release/acquire}}\\; \operatorname{Use}(G) \\]

也就是说，消费者看到 READY 后，应当同时看到生产者在发布 READY 前完成的相关初始化。

在 OP-TEE 的 Early Assembly 阶段，通常需要通过架构特定的内存屏障和启动协议实现这种语义，而不是直接套用普通 C Runtime。

# **11.10 RISC-V SBI HSM 与 OP-TEE 多 Hart 启动**

这部分与你当前研究的 OpenSBI、RPMI 和 RISC-V TEE Domain 有直接关系。

## **11.10.1 OpenSBI HSM 和 OP-TEE Boot 之间的关系**

我们先区分两种架构：

**方案 A：OpenSBI 管理 Hart State**

在具备适当隔离条件的 RISC-V S-mode OP-TEE 部署中：

```
OP-TEE Primary Hart
        |
        | SBI HSM hart_start
        v
OpenSBI / M-mode
        |
        | Validate request
        | Prepare hart state
        | Start target hart
        v
OP-TEE Secondary Hart
        |
        | Secondary Entry
        | Per-Hart initialization
        v
Secure execution environment ready
```

**方案 B：上一级 Secure Monitor 管理 Domain Entry**

在其他 TEE 架构中，M-mode Secure Monitor 可能承担 Domain Switch、Secure Hart Entry 和安全侧上下文管理。

因此，OP-TEE 不一定直接拥有 Hart Start 的最终控制权。

**SBI HSM 负责 Hart 生命周期接口，而 OP-TEE 负责自己的 Per-Hart Kernel State。**

此外，SBI HSM 接口本身不等于安全隔离机制。Hart 能否执行 OP-TEE，以及内存和设备访问是否被限制，仍需要由可信 Firmware 和硬件保护机制共同保证。

## **11.10.2 真实源码：**`boot_start_secondary_cores()`

源码：

`core/arch/riscv/kernel/boot.c`

在 `CFG_RISCV_S_MODE` 配置下可以看到：

```
void boot_start_secondary_cores(void)
{
    uint32_t curr_hartid =
        thread_get_core_local()->hart_id;

    /* Other declarations omitted */

    assert(get_core_pos() == 0);

    if (hartids_count > 1 &&
        !sbi_ext_available(SBI_EXT_HSM)) {
        panic();
    }

    for (i = 0; i < hartids_count; i++) {
        hartid = hartids[i];

        if (hartid == curr_hartid)
            continue;

        rc = sbi_hsm_hart_get_status(hartid, &status);

        if (rc || status != SBI_HSM_STATE_STOPPED)
            continue;

        rc = sbi_hsm_hart_start(
            hartid, start_addr, 0);

        if (rc)
            panic();

        started++;
    }

    boot_hart_count = started + 1;
}
```

这里为了突出逻辑，省略了日志和部分声明。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们重点分析五件事。

### **第一：Primary Hart 必须具有 Logical Index 0**

```
assert(get_core_pos() == 0);
```

这是一项重要的 Boot Invariant。

它不能简单理解为：

```
mhartid == 0
```

真正的含义是当前 Hart 被选为 OP-TEE 的 Primary。

### **第二：必须判断 HSM Extension 是否可用**

```
sbi_ext_available(SBI_EXT_HSM)
```

如果需要 OP-TEE 主动启动 Secondary Hart，就必须具备相应接口。

但这里还要注意更深层的设计问题：

**Extension 存在不代表目标 Hart 一定属于当前 Trusted Domain。**

Firmware 还必须验证调用者是否有权操作该 Hart。

### **第三：读取 Hart State**

```
sbi_hsm_hart_get_status(hartid, &status);
```

如果 Hart 已经处于 Started 或其他不符合预期的状态，不能直接对它重复执行启动操作。

因为不同 Hart State 对应不同的执行上下文和恢复约束。

### **第四：启动 Secondary Hart**

```
sbi_hsm_hart_start(hartid, start_addr, 0);
```

这里的 `start_addr` 指向启动入口。

目标 Hart 在完成 Firmware 层面的启动准备后，进入 OP-TEE 对应的 Secondary Boot Path。

### **第五：记录实际启动数量**

```
boot_hart_count = started + 1;
```

注意它表达的是实际参加本次启动同步的 Hart 数量，不一定等于全部配置的 Hart 数量。

这里隐藏了一个值得 Staff Engineer 深入讨论的问题：

如果 Hart ID 不连续，或者有某些 Hart 因状态不满足条件而被跳过，**Boot Barrier 所使用的索引集合是否仍然与真正启动的 Hart 集合一致？**

不能只根据 `boot_hart_count` 的数值判断正确性，还要同时检查 `hart_lottery`、`hart_index`、`sem_cpu_sync[]` 的关系。

这是很好的真实源码审查切入点。

## **11.10.3 为什么 OP-TEE 还检查 SBI RFENCE？**

在同一个文件中，4.11.0 的 RISC-V Primary 初始化包含如下检查：

```
if (CFG_TEE_CORE_NB_CORE > 1 &&
    !sbi_ext_available(SBI_EXT_RFENCE)) {
    EMSG("SBI RFENCE extension required for multi-hart operation");
    panic();
}
```

源码注释解释了原因：某些 TLB Invalidation 和 Instruction Cache 相关操作需要通过 SBI RFENCE 扩展影响其他 Hart，否则可能只完成本地刷新。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么这很重要？

设想以下情景：

1. CPU 0 修改了一个 TA 的页表。
2. CPU 1 曾经使用过旧的地址转换结果。
3. CPU 0 完成了本地 `sfence.vma`。
4. CPU 1 没有完成对应的转换失效处理。
5. CPU 1 随后仍然使用旧的映射。

如果旧映射具有已经被撤销的权限，就可能产生安全隔离问题。

因此：

\\[ \text{Page Table Updated} \not\Rightarrow \text{All CPUs Observe New Translation} \\]

必须另外建立跨 Hart Translation Synchronization。

这和你之前研究 CoVE 时的 MPT 修改、G-stage Translation、TLB 同步问题有相同的架构思维：**访问控制发生变化后，必须确定哪些缓存的授权状态需要同步失效。**

但 SBI RFENCE 仅解决其覆盖范围内的 Hart 相关同步，不自动刷新 IOMMU、SMMPT 或设备侧 Translation Cache。那些机制还需要各自的失效协议。

# **11.11 SMP 系统中的 Interrupt 与 Shared State**

## **11.11.1 为什么多核 OP-TEE 不需要一个中央 CPU 来分发所有中断？**

一个常见误解是：

> 既然 Primary CPU 负责初始化，是不是所有 Secure Interrupt 都应该先由 Primary CPU 接收，然后由它转发给其他 CPU？

并不是。

现代中断控制器通常有自己的 Interrupt Routing 能力。

例如：

- Arm GIC 可以将中断路由到指定 PE。
- RISC-V APLIC/IMSIC 可以按硬件支持的目标路由和投递 MSI。
- CPU-local Interrupt Controller Interface 负责当前 CPU 的 Pending、Enable、Priority 等状态。

因此，Primary CPU 不一定参与每次中断的处理。

真正需要设计的是：

**哪些中断由哪个 CPU 接收，以及中断处理程序如何安全访问共享 Kernel State。**

## **11.11.2 一个 Secure Device 中断案例**

假设：

```
Secure Crypto Engine
       |
       | completion IRQ
       v
CPU 3
       |
       v
OP-TEE IRQ Handler
       |
       v
Crypto Driver Shared State
       |
       v
Wake waiting Trusted Thread
```

我们需要回答：

- 中断是否只可能发给 CPU 3？
- 如果目标 CPU Offline，谁负责重新路由？
- 中断 Handler 能否直接唤醒等待线程？
- Handler 是否允许使用 Mutex？
- DMA 数据是否已经对 CPU 可见？
- 等待的 Trusted Thread 是否必须在 CPU 3 上恢复？
- 如果 Device Reset 与 Completion IRQ 并发，谁拥有最后的资源释放权？

这些问题不能仅从 GIC/APLIC 寄存器手册中得到完整答案。

它们涉及：

- Interrupt Framework。
- Synchronization。
- Driver State Machine。
- Thread Resume。
- Memory Ordering。
- CPU Power State。

## **11.11.3 为什么 Interrupt Mask 不能替代 SMP Lock？**

假设：

```
disable_local_interrupts();

global_counter++;

enable_local_interrupts();
```

在单核系统中，这可能避免某些本地中断与当前代码竞争。

但是在 SMP 系统中：

```
CPU 0                     CPU 1
Interrupts disabled      Interrupts enabled
      |                        |
global_counter++         global_counter++
```

CPU 1 仍然可以并发修改 `global_counter`。

因此：

**禁止当前 CPU 的中断并不等于对整个 SMP 系统加锁。**

真正需要的可能是：

- Spinlock。
- Atomic Operation。
- Per-CPU Counter + Aggregation。
- 更高层的对象锁。

设计时还需要考虑锁是否会在 Interrupt Context 中获取，避免本地中断打断一个已经持锁的执行上下文并产生 Deadlock。

# **11.12 CPU Power Management：Boot、Suspend 与 Resume**

这部分需要明确区分几个经常混淆的生命周期。

## **11.12.1 CPU State 与 Trusted Thread State 是不同维度**

| 事件          | CPU State          | Trusted Thread State |
| ----------- | ------------------ | -------------------- |
| CPU Boot    | Offline → Online   | 不一定创建新 Thread        |
| TA Invoke   | Online             | FREE → ACTIVE        |
| RPC         | CPU 可能继续运行其他工作     | ACTIVE → SUSPENDED   |
| RPC Resume  | 目标 CPU 在线          | SUSPENDED → ACTIVE   |
| CPU Suspend | Online → Suspended | 必须保证没有不安全的本地执行状态     |
| CPU Resume  | Suspended → Online | 恢复必要 CPU-local 状态    |
| CPU Off     | Online → Off       | 必须安全处理当前执行和资源依赖      |

这里需要特别强调：

`thread_state_suspend()`**&#x20;和&#x20;**`CPU_SUSPEND`**&#x20;完全不是同一层次的操作。**

`thread_state_suspend()` 是将 Trusted Thread 执行上下文挂起，常见于 RPC 或 Foreign Interrupt 等路径。

CPU Suspend 则涉及硬件执行核心的电源状态、Local Interrupt Controller、Timer、CPU Context 和 Firmware 协调。

## **11.12.2 为什么 CPU Resume 不能直接假设寄存器都还在？**

假设 CPU 3 进入某个低功耗状态。

根据实际 SoC 和 Power State，可能发生：

- CPU-local Register State 丢失。
- Local Timer 配置需要恢复。
- Interrupt Controller CPU Interface 需要重新配置。
- TLB 内容丢失或需要处理。
- Cache 状态变化。
- Secure Execution Context 需要 Firmware 恢复。

因此，Resume 路径必须先明确硬件保留了什么。

最容易犯的错误是把：

```
Secondary Cold Boot
```

和：

```
Secondary CPU Resume
```

当成完全相同的流程。

两者并不必然相同。

Cold Boot 要建立初始化状态；Resume 可能需要恢复之前保存的状态，或者重新初始化那些会在低功耗状态中丢失的硬件资源。

如果对已经有效的全局 Kernel State 再执行一次初始化，还可能破坏正在被其他 CPU 使用的资源。

## **11.12.3 Arm PSCI 与 RISC-V SBI HSM 的对照**

| 维度          | Arm PSCI                     | RISC-V SBI HSM                    |
| ----------- | ---------------------------- | --------------------------------- |
| 核心目标        | CPU Power/Execution State 管理 | Hart State Management             |
| 常见 Firmware | TF-A BL31 / EL3              | OpenSBI / M-mode                  |
| 启动接口        | `CPU_ON`                     | `hart_start`                      |
| 停止接口        | `CPU_OFF`                    | `hart_stop`                       |
| 挂起接口        | `CPU_SUSPEND`                | `hart_suspend`，取决于支持情况            |
| OP-TEE 责任   | Secure CPU Context 和局部状态维护   | Trusted Domain 内的 Hart-local 状态维护 |
| 核心风险        | EL3/TEE Context 不一致          | Domain/Hart State 不一致             |

这里要特别区分两个层次：

**Firmware State Machine** 负责 CPU/Hart 的执行与电源状态。

**OP-TEE Kernel State Machine** 负责 Kernel 与 Thread 状态的正确性。

任何跨越这两个状态机的操作都必须定义清楚 Handoff Contract。

# **11.13 从真实工业问题理解 SMP 初始化竞态**

现在分析一个可能出现在 SoC Bring-up 中的问题。

## **场景：8 核系统，偶发 Secondary CPU 启动失败**

系统条件：

- 8 个 RISC-V Hart。
- OP-TEE 作为 Trusted Domain 中的 Secure OS。
- OpenSBI 提供 SBI HSM。
- Primary Hart 能够正常完成 Boot。
- 当 Secondary Hart 启动时，偶尔出现 Illegal Instruction 或 Page Fault。
- 降低 CPU 频率后，错误发生概率明显降低。

如何分析？

### **第一阶段：不要立即假设是 Page Table 错误**

先建立时间线：

```
Primary:
  Build page tables
  Initialize thread_core_local[]
  Publish state
  Start secondary harts

Secondary:
  Enter _start
  Determine logical index
  Wait for primary
  Enable MMU
  Setup tp/sp
  Initialize local services
```

然后定位具体错误发生在哪个阶段。

例如：

- 在 MMU 启用前？
- 在 MMU 启用后？
- 在 `set_tp` 前？
- 在 `thread_init_per_cpu()` 内？
- 在 `boot_secondary_init_intc()` 内？

不同阶段对应完全不同的风险集合。

### **第二阶段：检查 CPU Identification**

需要确认：

```
hart_id
hart_index
CFG_TEE_CORE_NB_CORE
hartids[]
hartids_count
```

是否存在：

- 重复 Hart Index。
- Hart Index 越界。
- Physical Hart ID 和 Logical Index 混用。
- 某个 Hart 没有合法 Per-Hart Stack。
- 启动顺序与 Boot Barrier 假设不一致。

这些问题可能表现为随机 Memory Corruption，而不一定立刻发生显式越界异常。

### **第三阶段：检查 Memory Ordering**

重点查看：

- `sem_cpu_sync[]`。
- Primary 发布 READY 的时机。
- Secondary 检测 READY 的方式。
- Fence 和 Cache Coherency。
- Early Page Table 的可见性。
- MMU Enable 前后的地址空间约定。

如果只有降低 CPU 频率后错误概率下降，说明存在时序敏感问题，但并不能因此断定一定是缺少 Memory Barrier。

也可能是：

- 设备时钟尚未稳定。
- CPU Power Sequencing 不满足要求。
- Cache Coherency 尚未建立。
- Boot Entry PA 不正确。
- 共享状态被并发覆盖。

### **第四阶段：最小化实验**

建议依次做：

1. 固定只启动一个 Secondary Hart。
2. 逐步增加 Hart 数量。
3. 对启动阶段加入低开销的状态标记。
4. 记录每个 Hart 的 Entry、Stack、TP 和 SATP 状态。
5. 比较发生异常与正常情况下的状态差异。
6. 检查失败是否与特定 Hart ID、启动顺序或 CPU 频率相关。

最重要的是，不能简单通过增加 Delay 来宣称修复完成。

Delay 只能改变竞态出现概率，不能建立正确的 Happens-Before 关系。

# **11.14 如何为 SMP Boot 设计完整的验证方案？**

一次成功打印：

```
Secondary CPU initialized
```

远远不能证明 SMP 系统已经正确。

建议把验证分为四层。

## **第一层：Boot Correctness**

- 1 / 2 / 4 / 8 / 16 Core 启动。
- 不同 Primary Hart。
- 不连续 Hart ID。
- Secondary Hart 启动失败。
- 重复启动请求。
- 部分 Hart Offline。

## **第二层：Execution Correctness**

- 所有 CPU 并发执行 TA 请求。
- Trusted Thread Pool Exhaustion。
- RPC Suspend/Resume。
- Thread 跨 CPU 恢复。
- 多个 CPU 同时操作 TA Session。
- Native/Foreign Interrupt 并发。

## **第三层：Memory & Isolation Correctness**

- Shared Memory 并发访问。
- TA Page Table 更新与跨 CPU TLB Invalidation。
- MOBJ Reference Count 压力。
- DMA Buffer 权限变化。
- 异常发生时的 Kernel Stack Isolation。
- TA Panic 与资源清理。

## **第四层：Power & Recovery Correctness**

- Secondary CPU Suspend/Resume。
- CPU Off/On。
- 不同 CPU 组合下的系统休眠。
- Interrupt Pending 时进入低功耗状态。
- Crypto/DMA 任务运行期间的 Suspend。
- 连续几千次 CPU Power Cycle 与 Secure Service 调用。

这里最重要的测试策略是：**先测试每个状态转换，再测试状态转换之间的交错。**

例如，在 `Thread Resume`、`CPU Suspend` 和 `Page Table Update` 三个操作之间设计交错测试，其价值可能远高于简单重复一万次正常 TA Invoke。

# **11.15 Chapter 11 修改影响范围分析**

回到我们整个课程的主目标：修改 OP-TEE 的一处代码后，迅速判断哪些地方可能受到影响。

针对 SMP Boot，可以建立以下矩阵。

| 修改内容                         | 直接影响                  | 潜在跨模块风险                            | 必须验证                    |
| ---------------------------- | --------------------- | ---------------------------------- | ----------------------- |
| `thread_core_local` Layout   | Per-CPU Context       | Assembly Offset、Exception Entry    | Boot、Trap、SMP           |
| `get_core_pos()`             | CPU Indexing          | Stack、Interrupt、TLB Target         | 全部 CPU 拓扑               |
| Secondary Boot Entry         | CPU Bring-up          | MMU、Timer、Firmware Handoff         | Cold Boot、Resume        |
| `thread_init_per_cpu()`      | Local CPU State       | Exception、Tracing、Security Feature | 多核异常测试                  |
| `boot_secondary_init_intc()` | Local Interrupt Setup | IRQ Routing、Driver、PM              | Secure IRQ、CPU Offline  |
| `thread_init_threads()`      | Thread Pool           | TA Runtime、RPC、Stack               | 高并发、OOM                 |
| Boot Barrier                 | 全局初始化可见性              | 所有 Global State                    | 启动压力测试                  |
| SBI HSM Integration          | Hart Lifecycle        | OpenSBI Domain、CPU State           | Hart Start/Stop/Suspend |
| Cross-Hart TLB Sync          | MMU Translation       | TA Isolation、Memory Ownership      | 映射撤销、跨核访问               |

## **特别重要：修改&#x20;**`thread_core_local`**&#x20;可能影响汇编 ABI**

假设你增加了一个字段：

```
struct thread_core_local {
    ...
    uint64_t new_feature_state;
    ...
};
```

这看起来只是一个普通的 C 结构体修改。

但它可能影响：

- Structure Size。
- Field Offset。
- Alignment。
- Assembly 使用的 Generated Offset。
- Per-CPU Stack 或 Boot-time Temporary Allocation。
- Exception Entry 读取字段的方式。

RISC-V `entry.S` 直接使用了：

```
THREAD_CORE_LOCAL_SIZE
THREAD_CORE_LOCAL_HART_ID
THREAD_CORE_LOCAL_HART_INDEX
THREAD_CORE_LOCAL_TMP_STACK_VA_END
```

这类宏通常由 C 结构布局生成或对应的汇编定义提供。

因此修改结构体后，必须确认所有汇编访问仍与新布局一致。

仅仅成功编译 C 代码，并不能证明低层异常入口的行为正确。

这也是为什么 OS Core 开发需要同时理解：

**C Data Structure、Compiler ABI、Assembly、Linker 与 CPU Execution State。**

# **11.16 本章关键源码阅读清单**

以下路径均以 OP-TEE 4.11.0 为基线。

| 文件                                             | 建议重点研究                                          |
| ---------------------------------------------- | ----------------------------------------------- |
| `core/kernel/thread.c`                         | Global Thread Pool、Per-CPU State Initialization |
| `core/include/kernel/thread.h`                 | 公开的 Thread Kernel Interface                     |
| `core/include/kernel/thread_private.h`         | `thread_ctx`、Thread State                       |
| `core/arch/arm/include/kernel/thread_arch.h`   | Arm `thread_core_local`                         |
| `core/arch/arm/kernel/entry_a64.S`             | Arm Boot Entry、Per-CPU Stack                    |
| `core/arch/arm/kernel/boot.c`                  | Primary/Secondary Init                          |
| `core/arch/arm/kernel/thread.c`                | Arm Thread Allocation、Suspend、Resume            |
| `core/arch/riscv/include/kernel/thread_arch.h` | RISC-V Per-Hart State                           |
| `core/arch/riscv/kernel/entry.S`               | Hart Lottery、Boot Barrier                       |
| `core/arch/riscv/kernel/thread_rv.S`           | Trap 与 Kernel/User Context                      |
| `core/arch/riscv/kernel/boot.c`                | SBI HSM、Hart Bring-up                           |

建议用以下命令建立索引：

```
git checkout 4.11.0

git grep -n 'thread_init_thread_core_local'
git grep -n 'thread_init_per_cpu'
git grep -n 'boot_init_secondary'
git grep -n 'boot_start_secondary_cores'
git grep -n 'sem_cpu_sync'
git grep -n 'thread_state_suspend'
git grep -n 'thread_core_local'
```

重点不是打印出多少匹配结果，而是针对每个符号回答：

- 谁调用它？
- 它修改什么状态？
- 谁拥有这个状态？
- 它在哪种 Execution Context 中运行？
- 是否允许多个 CPU 同时调用？
- 修改后哪些其他状态需要同步更新？

# **Chapter 11 — Staff Engineer Assessment**

10 道深度设计、故障分析与安全架构题。重点考察推理过程、边界条件与验证能力，而不是背诵源码。

建议每道题先独立思考，再阅读参考分析。部分问题没有唯一的最优方案，判断依据是是否建立了完整的状态模型，并能够论证安全性。

Staff Engineer Interview Simulator

1 / 10

选择考题Q1 — 16-Core OP-TEE 为什么不应该设计成 16 个完全独立的 Kernel 实例？

Q1 — 16-Core OP-TEE 为什么不应该设计成 16 个完全独立的 Kernel 实例？

假设有人提出：为了避免锁竞争，每个 CPU 独立运行一份 OP-TEE OS，各自拥有 TA Context、Storage Cache 和 Thread Pool，但共享 Secure DRAM。请分析其可能带来的优势、隐藏的安全问题，以及是否存在合理的多实例架构。

查看参考分析与答题要点

上一题下一题

## **如何评价 Staff Engineer 级别的答案？**

我建议每道题按以下五项评分，而不是只判断最后结论是否正确。

| 维度     | 权重  | 评价标准               |
| ------ | --- | ------------------ |
| 架构理解   | 20% | 能否识别真正的模块职责和边界     |
| 状态与不变量 | 25% | 能否建立严格的对象生命周期和安全条件 |
| 并发与异常  | 25% | 是否考虑竞态、失败和恢复路径     |
| 技术权衡   | 15% | 能否比较多个设计方案及其代价     |
| 验证方法   | 15% | 能否提出可以证伪自身假设的测试    |

对于 Staff 级工程师而言，能够给出一个设计方案只是起点。

更有价值的能力是：

> 主动寻找自己方案中最脆弱的假设，指出可能破坏安全不变量的并发交错，并设计实验去证伪这个方案。

如果一个方案只有成功路径，没有清晰的失败路径、Rollback Strategy 和验证方法，那么即使代码写得再漂亮，也不能算完整的系统设计。

# **本章总结：SMP 的本质是正确管理并发的状态所有权**

通过 Chapter 11，我们应该建立六个核心认识。

**第一，OP-TEE 的多核能力不依赖于多个相互独立的 OS 实例。** 传统单实例模型中，共享 Kernel、共享 Thread Pool 与 CPU-local State 可以共同支持并行执行。

**第二，CPU 与 Trusted Thread 之间不存在永久的一对一绑定。** Thread 的执行可能被挂起，并在另一个 CPU 上恢复。因此必须严格区分 CPU-owned 与 Thread-owned State。

**第三，Primary Boot 和 Secondary Boot 有完全不同的初始化职责。** Global Initialization 只应在规定的初始化路径执行，而每个 CPU 必须正确建立自己的寄存器、Stack、Interrupt 和异常处理环境。

**第四，SMP 的正确性不只是通过 Spinlock 实现。** Boot Barrier、Acquire/Release、TLB Shootdown、Cache Coherency、Interrupt Routing 以及 Firmware Handoff，都是并发正确性的一部分。

**第五，CPU Power State 与 Trusted Thread State 是两个不同的状态机。** CPU Suspend/Resume 不能简单等同于 Thread Suspend/Resume；任何跨状态机操作都必须保证资源所有权和执行上下文不会丢失。

**第六，SMP 安全问题往往跨越多个模块。** 一个看似只修改 Secondary Boot 或 Per-CPU State 的 Patch，可能最终影响 TA Runtime、Crypto、Storage、Memory Isolation，甚至 Trusted Domain 的安全边界。

我希望你在本章真正掌握的不是 `thread_init_per_cpu()` 的实现，而是下面这个更通用的原则：

\\[ \boxed{\text{SMP Correctness} = \text{Ownership} + \text{Ordering} + \text{Isolation} + \text{Lifecycle}} \\]

当一个设计能够清楚定义这四件事，并且在正常执行、并发、异常和电源状态转换过程中始终维持相关不变量时，才算真正掌握了一个多核内核子系统。

**这也是 Chapter 11 向后续 Chapter 14（Trusted Thread）、Chapter 15（Context Management）和 Chapter 17（Synchronization）过渡的最重要基础。**
