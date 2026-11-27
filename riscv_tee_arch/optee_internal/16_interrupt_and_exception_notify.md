# **Chapter 16 — OP-TEE Interrupt、Exception 与 Notification 深度解析**

OP-TEE OS Internals & Architecture · Advanced / Staff Engineer Level

**研究基线：** OP-TEE 4.11.0，以 Arm64 经典 TEE 执行模型为主要源码研究对象，并补充 Arm32、FF-A/SPMC 和 RISC-V 的设计差异。

本章是理解 OP-TEE Kernel Execution Model 的关键章节之一。它将 Chapter 14 的 Trusted Thread、Chapter 15 的 Context Management，与后续的 Synchronization、RPC、Pager 和 Driver Framework 串联起来。

## **一、本章的目标：理解异步事件如何改变 OS 的执行状态**

如果只从 CPU Architecture 的角度理解 Interrupt，那么我们关注的是：

```
Hardware Interrupt
       |
       v
CPU Exception Entry
       |
       v
Save Registers
       |
       v
Interrupt Handler
       |
       v
Restore Registers
       |
       v
Exception Return
```

这是正确的，但对于 OP-TEE 来说还远远不够。

因为 OP-TEE 运行在一个特殊的系统环境中：

- 它不是 CPU 上唯一的 OS。
- 它与 Normal World 共享物理 CPU。
- 它的 Trusted Thread 执行往往依赖 Normal World 调用方的调度。
- 并不是所有被 Secure World 捕获的中断都应该由 OP-TEE 处理。
- 有些中断发生时甚至不存在一个可供继续执行普通内核服务的 Trusted Thread。
- 有些安全设备的中断需要触发复杂操作，但中断上下文本身不能执行这些操作。

因此，本章真正要研究的问题是：

> 当一个异步事件到来时，OP-TEE 如何判断应该由谁处理、在哪个执行上下文处理、是否需要切换世界或挂起 Trusted Thread，以及如何在不破坏系统安全不变量的前提下恢复执行？

我们将完整覆盖原定大纲的九个主题，并进一步增加源码执行路径、状态机、工程案例和高难度考题。

### **16.1 本章知识地图**

OP-TEE Event & Exception Architecture

**CPU / SoC Event**

IRQ · FIQ · Synchronous Exception · Notification Request

Interrupt Classification

Native / Foreign

Exception Classification

Syscall / Fault / Abort

**OP-TEE Context & State Management**

CPU State · Entry Stack · Thread State · Exception Mask · Locks

Immediate Handling

IRQ Handler · Pager · Syscall

Deferred Handling

Notification · Bottom Half · RPC

**Resume / Return / Terminate**

Exception Return · Thread Resume · TA Panic · Core Panic

### **16.2 首先建立五个基本概念**

| 概念                    | 核心含义               | 关键问题                    |
| --------------------- | ------------------ | ----------------------- |
| Native Interrupt      | 由 OP-TEE 负责处理的中断   | 应该执行哪个安全驱动 Handler？     |
| Foreign Interrupt     | 不由 OP-TEE 处理的中断    | 如何交还给正确的执行环境？           |
| Synchronous Exception | 当前指令引起的同步异常        | 是 Syscall、可恢复缺页，还是非法访问？ |
| Notification          | 通知另一个执行上下文某个事件已经发生 | 如何传递和消费事件而不丢失？          |
| Deferred Processing   | 将不能在中断上下文完成的工作推迟执行 | 谁来运行它，如何保证最终执行？         |

在 OP-TEE 中，**Native 和 Foreign 是从“谁负责处理”来分类的，而不是简单从硬件 IRQ/FIQ 信号分类。**

官方文档明确将 Native Interrupt 定义为由 OP-TEE OS 处理的中断；Foreign Interrupt 则包括面向 Normal World 或由 EL3 处理、但不由 OP-TEE OS 处理的中断。GICv2 与 GICv3 还可能使用不同的中断路由方式，因此不能机械地认为 FIQ 永远代表 Native、IRQ 永远代表 Foreign。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### **16.3 一个非常重要的架构认识**

需要特别区分以下三个概念：

**Interrupt Delivery、Interrupt Handling 和 Thread Scheduling。**

它们是三种不同的问题。

Interrupt Delivery 解决中断如何到达目标 Exception Level 或执行环境。

Interrupt Handling 解决接收到事件之后，执行哪个 Handler、如何清除中断状态。

Thread Scheduling 则解决被打断的执行任务什么时候继续运行。

例如，Linux 负责处理一个 Non-Secure UART Interrupt，完全不意味着 OP-TEE 必须先把该中断交给 Linux；如果当前已经在 Normal World 执行，硬件路由可能直接让 Linux 接收该中断。

相反，如果同样的中断在 OP-TEE 正执行一个 TA 时到来，就可能涉及 Foreign Interrupt Forwarding、Trusted Thread Suspend 和后续 Resume。

这里的关键变化并不是 UART 本身，而是 **中断发生时 CPU 处于哪个安全状态，以及谁拥有该中断的处理权。**

## **二、16.1 — Secure Interrupt 与 Foreign Interrupt 的设计理念**

### **16.1.1 为什么需要两类中断？**

传统 OS 通常认为：

```
Device -> Interrupt Controller -> OS -> Device Driver
```

但在 TrustZone 系统中，CPU 同时运行 Normal World 与 Secure World，因此中断控制器和系统固件需要保证：

1. Secure Device 的中断只能由授权的 Secure Handler 处理。
2. Non-Secure Device 的中断应由 Normal World 处理。
3. 无论当前运行的是哪个世界，中断都应在正确的安全域内处理。
4. 不允许 Non-Secure OS 通过虚假的中断路由破坏 Secure World。
5. Secure World 不能长期占用 CPU 导致 Normal World 的正常中断得不到处理。

最后一点容易被忽略。

假设一个 User TA 执行 RSA 运算需要 100ms，而 Linux 正在维护网络、存储和实时任务。

如果 OP-TEE 在整个 100ms 内完全屏蔽所有 Non-Secure Interrupt，就可能严重影响系统实时性。

因此，OP-TEE 必须提供一种机制，使一个执行中的 Secure Service 能够被临时挂起，在 Normal World 处理完成后继续执行。

这就是 **Foreign Interrupt Forwarding + Trusted Thread Suspend/Resume** 的主要用途。

### **16.1.2 从 OP-TEE 视角分类**

Interrupt Routing Decision

**Interrupt arrives**

Native

OP-TEE owns the handler

Secure IRQ dispatch

Driver acknowledgement

Return or defer work

Foreign

Another world owns it

Save trusted context

Exit or forward

Resume when requested

要注意：这个图体现的是逻辑职责。真实中断路由可能由 Interrupt Controller、EL3 Firmware、SPMC 或架构特定机制完成，OP-TEE 不一定亲自接收每一个 Foreign Interrupt。

### **16.1.3 GICv2 与 GICv3 的差异**

在经典 Arm 平台中，常见关系可以概括为：

| 组件                       | GICv2 常见模式               | GICv3 常见模式                   |
| ------------------------ | ------------------------ | ---------------------------- |
| Native Secure Interrupt  | Secure Group 0，常以 FIQ 表示 | 典型为 Group 1 Secure，路由到 S-EL1 |
| Non-Secure Interrupt     | Group 1 Non-Secure       | Group 1 Non-Secure           |
| EL3 Firmware Interrupt   | 取决于平台配置                  | 通常涉及 Group 0                 |
| OP-TEE Interrupt Handler | Secure IRQ Framework     | Secure IRQ Framework         |
| World Switch             | Secure Monitor / TF-A    | EL3/FF-A 配置相关                |

不能仅凭中断最终表现为 IRQ 还是 FIQ，判断它是不是 Secure Interrupt。

真正应该检查的是：

- Interrupt Security Group。
- 中断的目标 Exception Level。
- GIC CPU Interface 和 Distributor/Redistributor 配置。
- EL3 的 Interrupt Routing Policy。
- OP-TEE 的异常入口和驱动注册状态。

### **16.1.4 一条重要的安全不变量**

> **Interrupt Ownership Invariant：只有被授权的执行环境可以确认、处理和完成相应的设备中断。**

例如，Secure Crypto Engine 的中断即使错误配置成 Non-Secure Group，也不能因为 Linux 看到了中断号就认为系统仍然安全。

错误的 Interrupt Ownership 可能导致设备状态泄露、非法寄存器访问、拒绝服务，以及 DMA 操作状态与 Secure Kernel 不一致。

因此，Interrupt Security Configuration 也是 SoC 安全隔离设计的一部分，而不是单纯的驱动配置问题。

## **三、16.2 — Native Interrupt：从 Exception Vector 到 Driver Handler**

我们先研究一条最直接的路径：一个 Secure Device 产生中断，最终由 OP-TEE 内部驱动处理。

### **16.2.1 OP-TEE Interrupt Subsystem 的分层设计**

在 4.11.0 中，可以把 Native Interrupt 的处理分为四层：

```
Layer 1: Architecture Exception Entry
         core/arch/arm/kernel/thread_a64.S
                  |
                  v
Layer 2: Main Interrupt Controller
         core/drivers/gic.c
                  |
                  v
Layer 3: Generic Interrupt Framework
         core/kernel/interrupt.c
                  |
                  v
Layer 4: Interrupt Consumer Driver
         Crypto / UART / Timer / Other Device
```

这里最值得注意的是，OP-TEE 并不要求每一个设备驱动直接处理 GIC 寄存器。

它通过 `struct itr_chip`、`struct itr_handler` 和 Interrupt Framework 解耦。

这种设计与 Linux 的 IRQ Domain / IRQ Chip 思想有一定相似之处，但 OP-TEE 的抽象更精简，不能认为两者具有完全一致的调度和资源管理语义。

### **16.2.2 从 Arm64 Exception Vector 开始分析**

在 OP-TEE 4.11.0 的：

[core/arch/arm/kernel/thread_a64.S](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread_a64.S?utm_source=chatgpt.com)

中，可以看到 `thread_excp_vect` 组织了 Arm64 的异常向量表。

例如：

```
el1_irq_sp0:
    store_xregs sp, THREAD_CORE_LOCAL_X0, 0, 3
    b elx_irq

el1_fiq_sp0:
    store_xregs sp, THREAD_CORE_LOCAL_X0, 0, 3
    b elx_fiq
```

这是源码中的关键片段。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



逐层理解：

**第一层：CPU 根据 Exception Class 和当前栈模式选择向量表入口。**

这里的 `sp0` 并不是随便命名的，而是指当前 EL1 使用 `SP_EL0` 时相应的 Exception Vector Slot。

**第二层：保存进入异常时不能丢失的寄存器。**

`store_xregs` 是项目定义的汇编宏。其作用是把指定的通用寄存器保存到当前约定的内核状态区域。

**第三层：跳转到通用 IRQ/FIQ 处理入口。**

`elx_irq` 和 `elx_fiq` 会继续处理 CPU Context、异常来源和中断路由。

这里有个设计细节：

异常入口必须先建立可靠的寄存器和栈环境，再执行复杂 C 代码。

否则，如果在保存寄存器之前就调用 C Handler，C 函数可能覆盖被打断程序的寄存器，导致异常返回后程序状态损坏。

另一个值得留意的细节是，来自 TA 用户态和来自 Secure Kernel 的异常入口并不完全相同。

例如，`el0_irq_a64` 在进入后还涉及恢复内核映射以及 Pointer Authentication 相关状态的处理。它意味着 **处理 IRQ 所需要的地址空间，不一定就是被中断的 TA 正在使用的地址空间**。

### **16.2.3 Generic Interrupt Framework 的核心对象**

主要源码：

[core/kernel/interrupt.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/interrupt.c?utm_source=chatgpt.com)

在设计上，`struct itr_chip` 表示一个 Interrupt Controller 实例，其操作集合 `itr_ops` 提供中断使能、屏蔽、配置等能力。

`struct itr_handler` 则表示一个设备中断的处理者。它与中断号、所属 Controller、回调函数和驱动私有状态关联。

可以把两者理解成：

```
itr_chip
   |
   +-- Interrupt Controller Operations
   |      |
   |      +-- enable / disable
   |      +-- mask / unmask
   |      +-- configure
   |      +-- raise / affinity ...
   |
   +-- Registered Handlers
          |
          +-- itr_handler #1
          +-- itr_handler #2
          +-- itr_handler #3
```

这样带来一个非常有用的能力：

一个设备驱动不需要关心自己的中断究竟来自 GIC、另一个级联 Controller，还是其他支持该抽象的硬件中断控制器。

它只要获得正确的 Interrupt Descriptor，并注册 Consumer Handler。

### **16.2.4 真实源码：**`interrupt_call_handlers()`

OP-TEE 4.11.0 中的关键逻辑可以简化为：

```
void interrupt_call_handlers(struct itr_chip *chip,
                             size_t itr_num)
{
    struct itr_handler *h = NULL;
    bool was_handled = false;

    SLIST_FOREACH(h, &chip->handlers, link) {
        if (h->it == itr_num) {
            if (h->handler(h) == ITRR_HANDLED)
                was_handled = true;
            else if (!(h->flags & ITRF_SHARED))
                break;
        }
    }

    if (!was_handled) {
        EMSG("Mask unhandled interrupt ...");
        interrupt_mask(chip, itr_num);
    }
}
```

为突出控制流，省略了少量诊断参数和断言；逻辑对应 4.11.0 源码。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

我们来分析几个设计点。

**第一：为什么遍历 Handler List？**

因为一个 Interrupt Controller 可以管理多个 Consumer，而且一个 Interrupt Number 可能采用 Shared Handler 模型。

**第二：为什么返回值是&#x20;**`ITRR_HANDLED`**&#x20;或&#x20;**`ITRR_NONE`**？**

这是让框架区分“已经处理”和“当前 Handler 不能处理”。

如果共享一个中断源，某个 Handler 发现中断不是自己的，可以返回 `ITRR_NONE`，让框架继续遍历其他 Handler。

**第三：为什么所有 Handler 都没有处理时，要 Mask Interrupt？**

设想一个 Level-triggered Interrupt。

设备一直保持 Interrupt Pending。如果没有 Handler 清除设备状态，CPU 退出异常后会立即再次进入同一异常。

结果就是 Interrupt Storm。

因此上游对未处理的中断采取 Mask 策略，这是一种避免系统无限重复进入异常的防御机制。

但它也带来一个工程问题：某个 Handler 错误地返回 `ITRR_NONE`，可能导致该 IRQ 被屏蔽，之后设备再也不能正常工作。

这类问题经常表现为“设备只工作了一次，第二次永远没有中断”。

### **16.2.5 GIC Driver 负责什么？**

在：

[core/drivers/gic.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/drivers/gic.c?utm_source=chatgpt.com)

中，Native Interrupt 的典型逻辑是：

```
/* 基于源码逻辑的简化示例 */

iar = gic_read_iar(gd);
id = iar & GICC_IAR_IT_ID_MASK;

if (is_special_interrupt(id))
    return;

if (id <= gd->max_it)
    interrupt_call_handlers(&gd->chip, id);
else
    EMSG("Unhandled interrupt");

gic_write_eoir(gd, iar);
```

这个片段对应 GIC 驱动的 `gic_native_itr_handler()` 控制流。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



需要注意三个层次：

1. **Interrupt Controller Acknowledge**：取得中断 ID，更新 Controller 状态。
2. **Device Interrupt Acknowledge**：由设备 Handler 根据硬件规范清理设备的 Interrupt Cause。
3. **Interrupt Completion**：执行 EOI / Deactivation 等 Controller 完成操作。

它们不是同一件事。

以 Level-triggered UART 为例，如果只向 GIC 写 EOI，而没有读取 UART FIFO 或清理设备状态，那么中断可能立即再次触发。

以 Edge-triggered DMA 为例，如果错误清除或覆盖了 Completion Status，也可能造成丢失完成事件。

因此，一个高质量的驱动必须同时理解 **Device Interrupt Protocol 和 Interrupt Controller Protocol**。

## **四、16.3 — Foreign Interrupt：为什么要 Suspend Trusted Thread？**

这是本章最值得深入的一条执行链。

### **16.3.1 构造一个真实场景**

假设：

- CPU0 正在执行一个 User TA。
- TA 调用 `TEE_AEUpdate()`，内部进入 Secure Kernel。
- Kernel 正在使用软件 Crypto Backend 处理一段较大的数据。
- Linux 的 Non-Secure Timer Interrupt 此时到来。

如果当前平台将该 Foreign Interrupt 交给 OP-TEE 的异常入口进行转发，那么 OP-TEE 必须保证：

- Crypto 运算可以被安全暂停。
- Kernel Stack 和执行寄存器不会丢失。
- 已持有的 Kernel State 不会被破坏。
- Normal World 能够处理 Timer Interrupt。
- 后续恢复时可以在原有位置继续运算。

### **16.3.2 完整的 Foreign Interrupt Forwarding**

Foreign IRQ 的典型执行时间线

1. **CPU 正在执行 Secure Service**

   Trusted Thread #2 处于 `ACTIVE`，可能正在执行 User TA 或 Kernel Service。
2. **Foreign Interrupt 到来**

   根据 GIC 和 EL3 路由配置进入对应的异常路径。
3. **保存 Execution Context**

   汇编入口保存通用寄存器和异常返回状态，并由 Thread 管理逻辑保存必要的上下文。
4. **Trusted Thread 状态转换**

   `ACTIVE → SUSPENDED`，撤销当前 CPU 对该 Thread 的运行占用，并根据需要保存用户地址空间状态。
5. **退出 Secure World**

   切换到安全的 Entry Context，向 Monitor/Normal World 返回 Foreign Interrupt 提示。
6. **Linux 处理中断**

   Normal World 运行对应 IRQ Handler，也可能进行任务调度。
7. **Normal World 再次进入 OP-TEE**

   提交 Continue/Resume 请求，OP-TEE 校验挂起状态并恢复 Execution Context。
8. **恢复被打断的指令流**

   Trusted Thread 重新变成 `ACTIVE`，从原来的 Exception Return 位置继续执行。

这与 Linux 通过调度器切换两个内核线程不同。

在经典 OP-TEE 模型中，OP-TEE 维护的是一个可以挂起和恢复的 Secure Execution Context，其恢复通常仍受发起请求的 Normal World 调用方调度影响。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### **16.3.3 关键源码：**`thread_state_suspend()`

现在来研究实际代码。

文件：

`core/arch/arm/kernel/thread.c`

函数：

```
int thread_state_suspend(uint32_t flags,
                         uint32_t cpsr,
                         vaddr_t pc);
```

我将 4.11.0 真实函数的关键逻辑整理成下面这个简化形式：

```
int thread_state_suspend(uint32_t flags,
                         uint32_t cpsr,
                         vaddr_t pc)
{
    struct thread_core_local *l = thread_get_core_local();
    int ct = l->curr_thread;

    assert(ct != THREAD_ID_INVALID);

    /* 保存必要的执行状态 */

    thread_lock_global();

    assert(threads[ct].state == THREAD_STATE_ACTIVE);

    threads[ct].flags |= flags;
    threads[ct].regs.cpsr = cpsr;
    threads[ct].regs.pc = pc;
    threads[ct].state = THREAD_STATE_SUSPENDED;

    threads[ct].have_user_map =
        core_mmu_user_mapping_is_active();

    if (threads[ct].have_user_map) {
        core_mmu_get_user_map(&threads[ct].user_map);
        core_mmu_set_user_map(NULL);
    }

    l->curr_thread = THREAD_ID_INVALID;

    thread_unlock_global();

    return ct;
}
```

为教学省略了实际函数中的 Stack/Pager、VFP、Profiling、Secure Partition 和 Virtualization 逻辑；不可直接替代原函数。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

这个函数体现了 OP-TEE Kernel 很重要的几个设计理念。

### **16.3.4 为什么需要 Global Thread Lock？**

因为 Trusted Thread 的 State 是全局共享资源。

在 SMP 系统中，CPU0 和 CPU1 可能并发尝试分配或恢复不同的 Thread Context。

假设没有同步保护：

```
CPU0                         CPU1
----                         ----
Check Thread 2 is suspended
                             Check Thread 2 is suspended
Mark Thread 2 active
                             Mark Thread 2 active
Restore Thread 2
                             Restore Thread 2
```

两个 CPU 同时运行同一个 Thread Context，会直接破坏 Kernel Stack、寄存器状态以及 TA 地址空间。

因此，State Transition 必须是原子的。

但请注意：这里的锁保护的是 **Thread Ownership 和状态一致性**，不是要持有它直到 Trusted Thread 完成运行。

### **16.3.5 为什么要保存 User Mapping？**

因为 Secure Thread 恢复时，可能继续执行 User TA。

假设被打断之前使用：

```
User VA 0x10000000
          |
          v
TA-A Page Table
          |
          v
Secure PA 0x48000000
```

CPU 回到 Entry Context 后，不能让之前的 TA User Mapping 被无条件保留为当前通用执行上下文。

而且，Thread 后续恢复时也不一定在原来的 CPU 上进行。

所以必须保留对应的 User Mapping 描述，并在恢复 Thread 时重新激活。

其核心安全约束是：

> 恢复执行上下文与恢复内存访问上下文必须保持一致。

否则就可能出现 CPU Registers 属于 TA-A、但有效 User Mapping 属于 TA-B 的严重问题。

### **16.3.6 为什么需要把&#x20;**`curr_thread`**&#x20;设为 Invalid？**

因为当前 CPU 已经不再执行这个 Trusted Thread。

需要区分：

- Thread Context 仍然存在。
- Thread State 为 `SUSPENDED`。
- 当前 CPU 不再拥有该 Thread 的执行权。
- 该 Thread 的 Stack、Session 等运行状态仍被保存。

不能因为 CPU 不再执行 Thread，就认为 Thread 可以释放。

这也是为什么 `SUSPENDED` 和 `FREE` 在语义上完全不同。

### **16.3.7 Resume 时如何防止状态被破坏？**

对应函数：

```
thread_resume_from_rpc(...)
```

4.11.0 中，恢复路径会验证 Thread ID 和 `THREAD_STATE_SUSPENDED`，将状态切换回 `ACTIVE`，重新建立 CPU 的 Current Thread 关系及 User Mapping，然后恢复寄存器执行状态。

其中有一个特别值得关注的注释：

```
/*
 * Return from RPC to request service of a foreign
 * interrupt must not get parameters from non-secure world.
 */
```

这意味着 OP-TEE 并不会简单地信任 Normal World 在 Resume 时提交的所有寄存器参数。

只有满足特定标志条件，才会将 RPC 返回参数复制到已保存的寄存器上下文。

这个区别非常重要：

- Normal RPC 的返回数据是协议规定的输入，需要正确验证和使用。
- Foreign Interrupt Resume 主要是恢复原本的执行状态，不应让 Normal World 任意修改被中断程序的通用寄存器。

这是防止 Normal World 利用 Resume 通道注入 Secure Execution State 的一项关键约束。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### **16.3.8 为什么不能简单保存 CPU 寄存器就退出？**

因为 OP-TEE 不只是一个裸机程序。

它还需要考虑：

| 状态                     | 为什么需要考虑                   |
| ---------------------- | ------------------------- |
| GPR、PC、PSTATE          | 恢复正确的指令流                  |
| Kernel Stack           | 保留函数调用链、局部变量和返回地址         |
| User Mapping           | 恢复正确 TA 地址空间              |
| FP/SIMD State          | 避免不同世界或 TA 的计算状态相互污染      |
| Thread State           | 防止重复分配和双重恢复               |
| Session Context        | 保留当前正在执行的 TA 调用关系         |
| Lock State             | 保证跨 Suspend 的共享状态仍然一致     |
| Pager State            | 确保被打断的页仍然具有合法的恢复语义        |
| Virtualization Context | 避免不同 Guest 之间的 Context 混淆 |

其中 FP/SIMD 的实际保存策略可能采用 Lazy Saving，不能理解成每次异常都无条件保存所有寄存器。

### **16.3.9 一个需要思考的问题**

假设 CPU0 的 Trusted Thread #2 被 Foreign Interrupt 挂起。

随后，Linux 把原调用线程迁移到了 CPU1。

那么，Trusted Thread #2 能不能在 CPU1 上恢复？

**原则上可以由支持相应迁移语义的 Thread/Platform 实现恢复，但绝对不能简单恢复 GPR 后就执行。**

必须保证：

- Thread 不再由其他 CPU 运行。
- 新 CPU 安装正确的 TA User Mapping。
- Per-CPU Data 使用 CPU1 的状态。
- FP/SIMD 等 Architecture State 正确处理。
- 中断屏蔽状态与运行模式正确。
- 不存在与旧 CPU 绑定、却仍被当作本地状态使用的资源。

这就是为什么在 OP-TEE 中，Thread Context 和 Per-CPU Context 必须严格分离。

## **五、16.4 — Synchronous Exception：Syscall、Page Fault 和 Abort**

在这一部分，我们需要建立一个特别重要的认识：

**不是所有 Exception 都意味着系统出了错误。**

OP-TEE 有大量合法功能正是依靠 Synchronous Exception 实现的。

### **16.4.1 三类不同的同步异常**

| Exception                    | 触发原因                        | OP-TEE 应采取的动作                 |
| ---------------------------- | --------------------------- | ----------------------------- |
| Syscall / SVC                | User TA 主动请求 Kernel Service | Dispatch 到合法 Syscall          |
| Recoverable Page Fault       | 合法 VA 尚未建立当前有效映射            | Pager 修复映射，继续执行               |
| Invalid Access / Fatal Fault | 非法地址、权限违规或不可恢复硬件错误          | TA Termination 或 Kernel Panic |

这是从语义上进行的分类，而不是按照 CPU Exception Code 简单一一对应。

例如，两次 Data Abort 都可能表现为 Translation Fault，但一次发生在合法的 Pageable Region，另一次发生在非法用户地址。

是否可以恢复，取决于 Memory Manager 对 Fault Address 和 Access Type 的判断。

### **16.4.2 Page Fault 的完整路径**

假设一个 User TA 正在读取尚未装载的代码或数据页。

```
User TA
   |
   v
Access Virtual Address
   |
   v
MMU Translation Fault
   |
   v
Arm64 Synchronous Exception
   |
   v
OP-TEE Abort Entry
   |
   v
abort_handler()
   |
   v
Fault Classification
   |
   +-- Recoverable?
   |      |
   |      v
   |   tee_pager_handle_fault()
   |      |
   |      v
   |   Update Mapping
   |      |
   |      v
   |   Resume Faulting Instruction
   |
   +-- Unrecoverable?
          |
          v
       TA Panic / Core Panic
```

这里最值得深入研究的是 `abort_handler()`。

### **16.4.3 真实源码：**`abort_handler()`

文件：

[core/arch/arm/kernel/abort.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/abort.c?utm_source=chatgpt.com)

4.11.0 中，它通过 `get_fault_type()` 判断异常类型，然后进行不同处理。

逻辑可以概括为：

```
void abort_handler(uint32_t abort_type,
                   struct thread_abort_regs *regs)
{
    struct abort_info ai;
    bool handled;

    set_abort_info(abort_type, regs, &ai);

    switch (get_fault_type(&ai)) {
    case FAULT_TYPE_USER_MODE_PANIC:
        handle_user_mode_panic(&ai);
        break;

    case FAULT_TYPE_USER_MODE_VFP:
        handle_user_mode_vfp();
        break;

    case FAULT_TYPE_PAGEABLE:
        handled = tee_pager_handle_fault(&ai);

        if (!handled) {
            if (!abort_is_user_exception(&ai))
                panic("unhandled pageable abort");

            handle_user_mode_panic(&ai);
        }
        break;

    /* 其他 fault 类型分别处理 */
    }
}
```

控制流简化；实际函数还包含 External Abort、Ignore、VFP Context、诊断记录等逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

这个函数的设计本质是：

**先分类，再判断恢复策略，最后决定故障隔离范围。**

### **16.4.4 为什么 User TA Fault 和 Kernel Fault 的结果不同？**

考虑两种情形。

**情形 A：User TA 访问非法地址。**

```
volatile uint32_t *p = (uint32_t *)0xdead0000;
uint32_t value = *p;
```

如果这个 VA 不合法，异常无法通过 Pager 恢复，那么合理的结果是让 TA Panic，并通过 TA Manager 完成必要的资源清理。

不能因为一个 User TA 出错就默认让整个 Secure Kernel 崩溃。

**情形 B：Secure Kernel 访问非法地址。**

假设 Crypto Driver 的内核态实现错误访问某个非法指针。

这种情况不能通过简单杀死 TA 来保证安全，因为 Kernel 已经可能破坏共享状态、锁、内存对象或设备状态。

因此，对于无法恢复的核心 Kernel Fault，更合理的默认策略是触发 Kernel Panic，而不是继续运行一个可能已经不可信的系统。

这体现的是：

> Fault Containment Boundary：一个执行环境的故障，能否被限制在该环境内部？

User TA 和 Secure Kernel 的故障隔离能力并不相同。

### **16.4.5 为什么 Pager 不能尝试修复所有 Translation Fault？**

假设 User TA 正在读取：

```
VA = 0x70000000
```

该地址不在 TA 的任何合法 VM Region 内。

如果 Pager 只因为出现 Translation Fault 就自动分配物理页，等于让 User TA 通过访问任意地址请求新的映射。

这将破坏 VM Region 的访问控制和资源分配规则。

因此，Page Fault Handling 必须受到以下约束：

- Fault VA 属于合法的 Memory Region。
- 当前 Access Type 被允许。
- 目标对象的生命周期合法。
- 页表变更不破坏不同 TA 的隔离。
- Pager 所依赖的关键代码和数据能够安全执行。
- 失败时不能留下部分不一致的映射状态。

### **16.4.6 一个特别有价值的跨模块问题**

假设一个 Secure Interrupt 在 Pager 正修改页表时到来。

你需要分析：

1. Pager 正持有哪些锁？
2. Interrupt Handler 是否会访问同样的 Page Table 或 Memory Allocator？
3. Interrupt Handler 的代码是否常驻内存？
4. Handler 是否可能自己触发 Page Fault？
5. 如果需要 TLB Invalidation，是否影响正在并发运行的其他 CPU？

这也是 OP-TEE 为什么对 Interrupt Handler 和相关数据的内存驻留性有特殊要求。

真实的 `interrupt.c` 在注册 Handler 时会检查 Handler 对象及其回调代码的 Unpaged 属性。这不是一个可有可无的优化，而是在分页配置下维持安全中断处理路径可执行性的必要条件。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## **六、16.5 — Interrupt Context：为什么不能像普通 Kernel Code 一样执行？**

这是写 OP-TEE Secure Driver 时非常容易出错的地方。

### **16.5.1 Interrupt Context 不等于 Trusted Thread Context**

假设我们有两个入口：

```
/* 普通 Trusted Thread Context */
TEE_Result crypto_do_operation(...)
{
    mutex_lock(&crypto_mutex);

    /* 执行操作 */

    mutex_unlock(&crypto_mutex);
    return TEE_SUCCESS;
}
```

以及：

```
/* Native Interrupt Context */
static enum itr_return crypto_irq_handler(struct itr_handler *h)
{
    /* 处理 Crypto Interrupt */
    return ITRR_HANDLED;
}
```

这两个函数虽然都执行在 Secure Kernel 内部，但它们的执行约束完全不同。

| 属性                | Trusted Thread Context  | Native IRQ Context       |
| ----------------- | ----------------------- | ------------------------ |
| 当前执行身份            | 有关联的 Trusted Thread     | 不一定有可用 Trusted Thread    |
| 运行时间              | 可以处理相对较长的操作             | 必须尽可能短                   |
| Sleeping Mutex    | 符合上下文约束时允许              | 不允许                      |
| RPC               | 可以在受支持的路径中执行            | 不允许                      |
| Page Fault        | 某些条件下可由 Pager 恢复        | Handler 自身不能依赖普通缺页恢复     |
| Memory Allocation | 可使用允许阻塞的分配方式            | 不能随意调用可能阻塞的分配器           |
| Interrupt Mask    | 由 Thread Context 和锁管理决定 | 通常处于中断屏蔽的 Atomic Context |
| Stack             | 受 Thread 执行模型管理         | 由异常入口和 Per-CPU 异常环境管理    |

这里的本质区别是：

**Trusted Thread 是可以保存并恢复的业务执行上下文，而 Interrupt Handler 是必须在特定硬件事件和异常状态下迅速完成的处理路径。**

### **16.5.2 为什么不能在 IRQ Handler 中调用&#x20;**`mutex_lock()`**？**

设想 CPU0：

```
Trusted Thread #1
    |
    +-- mutex_lock(&crypto_mutex)
    |
    +-- 正在修改 Crypto 状态
            |
            +-- Secure IRQ arrives
                    |
                    +-- crypto_irq_handler()
                            |
                            +-- mutex_lock(&crypto_mutex)
```

由于 Mutex 已经由被中断的 Thread 持有，而这个 Thread 只有在 IRQ Handler 结束后才有机会继续运行，因此 IRQ Handler 无法等待它释放 Mutex。

形成死锁：

```
IRQ Handler waits for Mutex
           ^
           |
Trusted Thread holds Mutex
           ^
           |
Trusted Thread waits for IRQ return
```

即使 Mutex 由其他 CPU 持有，IRQ Handler 也不能因此假设阻塞等待是安全的。

原因是其他 CPU 可能正等待当前 CPU 完成某个跨核操作。

### **16.5.3 Spinlock 是否一定安全？**

也不一定。

Spinlock 只能解决某一类并发问题，不能自动保证 Lock Ordering 和 Interrupt Preemption Safety。

考虑：

```
CPU0                         CPU1
----                         ----
Holds Lock A                  Holds Lock B

IRQ arrives
IRQ waits for Lock B
                              Waits for Lock A
```

这仍然是死锁。

另外，如果普通 Thread 在持有某个 Spinlock 时没有正确屏蔽可能使用同一把锁的 Secure Interrupt，那么中断可能在同一个 CPU 上抢占 Thread，随后等待 Thread 自己持有的锁。

因此，在 OP-TEE 中审查一把 Spinlock 时，应同时回答：

1. Lock 是否允许在 IRQ Context 使用？
2. Thread Context 获取 Lock 时是否需要保存并屏蔽相关异常？
3. 是否存在跨 CPU 的 Lock Ordering 问题？
4. 临界区内是否会触发 Pager 或 RPC？
5. 临界区运行时间是否有可接受的上界？

一个典型的模式是：

```
uint32_t state = cpu_spin_lock_xsave(&lock);

/* 仅访问受锁保护的短小状态 */

cpu_spin_unlock_xrestore(&lock, state);
```

注意：这只展示 OP-TEE 提供的锁使用形式，并不代表所有需要同步的驱动都应该采用这一把锁。

### **16.5.4 IRQ Handler 的代码与数据为什么必须 Unpaged？**

假设 IRQ Handler 位于 Pageable Memory。

```
Secure IRQ
    |
    v
Exception Entry
    |
    v
Jump to Driver Handler
    |
    v
Handler Code Page Not Present
    |
    v
Page Fault
    |
    v
Pager?
```

现在的问题是：Pager 是否一定能够在当前异常栈、锁状态和 MMU 状态下安全运行？

不能假设可以。

因此，IRQ Handler、必要的 Interrupt Controller 操作以及执行过程中直接依赖的关键状态，必须满足实际运行路径的内存驻留要求。

上游 `interrupt.c` 在注册 Handler 时对 Handler 对象和回调入口执行 `is_unpaged()` 检查，就是为了约束这类风险。

但还需要审查回调的传递调用：**仅证明入口函数位于 Unpaged Section，并不自动证明它调用的全部代码和访问的数据也安全。**

### **16.5.5 IRQ Handler 的时间约束**

假设一个 Crypto Engine 以每秒数千次的频率产生 Completion Interrupt。

如果每次 Handler 执行 2ms，并且在执行期间屏蔽其他中断，那么系统可能迅速出现严重的中断延迟。

这不仅是 Performance 问题，还可能影响：

- Watchdog。
- Secure Timer。
- Power Management。
- 其他 Secure Device。
- Foreign Interrupt 的转发时机。
- 系统中具有实时要求的 Normal World 任务。

因此通常应把 IRQ Handler 限制在：

```
Read Status
    |
    v
Acknowledge / Mask Device IRQ
    |
    v
Record Completion State
    |
    v
Signal Deferred Work
    |
    v
Return
```

耗时的数据处理、复杂状态机及可能需要 Sleeping/RPC 的操作，应放到允许 Yielding 的上下文。

## **七、16.6 — Notification Framework：为什么 OP-TEE 需要另一套事件通知机制？**

这一部分是很多旧版 OP-TEE 经验容易遗漏的内容。

传统的 OP-TEE 设计中，Secure World 主要通过 Normal World 主动调用来获得执行机会。

但是，当 OP-TEE 内部发生一个异步事件时，例如 Secure Device Interrupt，可能需要通知 Normal World 安排一个可以执行较长操作的安全服务上下文。

**于是，Notification 成为连接 Interrupt Context 和 Yielding Context 的关键机制。**

### **16.6.1 Synchronous Notification**

Synchronous Notification 通常依赖 OP-TEE RPC。

例如两个 Trusted Thread 竞争同一个 Mutex：

```
Trusted Thread A             Trusted Thread B
       |                            |
       v                            v
   Lock Mutex                   Lock Mutex
       |                            |
       |                         Mutex Busy
       |                            |
       |                         RPC Wait
       |                            |
       v                            v
 Unlock Mutex                 Linux Sleeps
       |
       v
 RPC Notify
       |
       v
 Linux Wakes Thread B
                                    |
                                    v
                              Resume Thread B
                                    |
                                    v
                               Retry Lock
```

这种设计让等待者能够在 Normal World 中睡眠，而不需要持续占用 CPU 执行 Spin Waiting。

其关键价值是：OP-TEE 没有必要自行实现完整的通用 Thread Scheduler，却能够复用 Normal World 的线程阻塞和唤醒能力。

但它有一个重要限制：

**Synchronous Notification 依赖 RPC，必须在允许 Yielding 的执行上下文中使用。**

因此不能直接用于 Secure IRQ Handler。

### **16.6.2 Asynchronous Notification**

如果 Notification 发生在 Secure IRQ Context，就不能简单使用 RPC。

于是 OP-TEE 采用：

```
Secure World
    |
    v
Record Notification Value
    |
    v
Trigger Non-Secure Interrupt
    |
    v
Linux OP-TEE IRQ Handler
    |
    v
Fast SMC: Get Notification Value
    |
    v
Wake Waiter or Schedule Bottom Half
```

对应 Linux 侧的主要接口包括：

- `OPTEE_SMC_GET_ASYNC_NOTIF_VALUE`
- `OPTEE_SMC_ASYNC_NOTIF_VALUE_DO_BOTTOM_HALF`
- `OPTEE_MSG_CMD_DO_BOTTOM_HALF`

Linux 内核文档明确指出，Asynchronous Notification 的实际 Value 通过 Fast Call 获取，一个中断可以对应多个待处理 Value；Value 0 被用于请求执行 Bottom Half。

[image](https://www.google.com/s2/favicons?domain=https://www.kernel.org\&sz=32)

The Linux Kernel documentation



这里的设计很巧妙：

**Secure World 不需要在 IRQ Handler 中执行复杂操作，只需要把工作请求传递给 Normal World，由后者安排一次新的 Yielding Call。**

### **16.6.3 Notification 的值空间**

在 OP-TEE 4.11.0 的 `core/include/kernel/notif.h` 中，可以看到：

```
#if defined(CFG_CORE_ASYNC_NOTIF)
#define NOTIF_ASYNC_VALUE_MAX  U(63)
#define NOTIF_SYNC_VALUE_BASE \
    (NOTIF_ASYNC_VALUE_MAX + U(1))
#else
#define NOTIF_SYNC_VALUE_BASE 0
#endif

#define NOTIF_VALUE_MAX \
    (NOTIF_SYNC_VALUE_BASE + CFG_NUM_THREADS)

#define NOTIF_VALUE_DO_BOTTOM_HALF 0
```

并且公开了：

```
void notif_send_async(uint32_t value,
                      uint16_t guest_id);

TEE_Result notif_send_sync(uint32_t value);
TEE_Result notif_wait(uint32_t value);
TEE_Result notif_wait_timeout(uint32_t value,
                              uint32_t timeout_ms);
```

这些值空间用于区分支持异步传递的 Notification，以及主要用于 Thread Wait/Wakeup 的同步 Notification。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有一个值得注意的细节：

**Notification Value 并不等于一个 Linux Thread ID 或一个完整的 Event Object。**

它只是 Notification Protocol 内的标识。

对于高并发设计，还需要明确 Value 的分配、释放、复用、所属 Guest，以及消费者如何判断事件是否仍然有效。

### **16.6.4 现代 Notification Framework 的 Driver Callback**

4.11.0 中包含 `struct notif_driver`，其主要思想是向驱动提供两类 Callback：

```
struct notif_driver {
    void (*atomic_cb)(...);
    void (*yielding_cb)(...);
    /* 其他字段 */
};
```

两种回调有不同的执行契约：

- `atomic_cb`：在 Atomic Context 下处理通知状态事件，不允许进行 Yielding 操作。
- `yielding_cb`：在正常 Trusted Thread Context 中运行，可以使用允许阻塞的同步操作和 RPC。

真实 `core/kernel/notif.c` 中，`notif_deliver_atomic_event()` 使用 Spinlock 序列化 Atomic Callback，而 `notif_deliver_event()` 使用 Mutex 序列化 Yielding Callback，并且在调用 Yielding Callback 前释放内部 Spinlock。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



这里可以看到一个很好的设计原则：

> 内核框架不应在持有短临界区 Spinlock 时，调用可能执行任意较长操作的 Driver Callback。

否则，驱动内部的 Mutex、RPC 或复杂操作可能导致死锁。

### **16.6.5 Notification 与 Event Queue 的区别**

这个问题值得特别强调。

Notification 解决的是：“有事情需要处理。”

Event Queue 解决的是：“具体发生了哪些事情，以及每件事情的数据和顺序是什么。”

假设一个 Crypto Engine 连续完成 100 个 DMA Operation。

如果只发送同一个 Notification Value，并不能据此断言 Normal World 一定能收到 100 次独立事件。

因此，更可靠的设计通常是：

```
Secure IRQ
    |
    v
Read Hardware Completion
    |
    v
Record Completion in Secure State
    |
    v
Notify Work Pending
    |
    v
Bottom Half
    |
    v
Drain Secure Completion Records
```

Notification 主要承担 Wakeup/Trigger 的作用。

真正决定有多少任务完成、哪些任务完成、完成顺序和状态的，是受保护的 Driver State 或 Completion Queue。

## **八、16.7 — Top Half / Bottom Half：一条真正的跨世界异步执行链**

现在把前面的知识串联起来。

### **16.7.1 真实需求：Secure UART 或 Crypto Completion**

假设 OP-TEE 接收到一个 Secure UART RX Interrupt。

设备已经收到了大量数据。如果 Handler 在 IRQ Context 内完整处理所有输入，可能阻塞其他中断。

因此采用两阶段处理：

**Top Half：** 最小化处理中断、记录状态、屏蔽必要的中断源、请求后续工作。

**Bottom Half：** 在可 Yielding 的上下文中处理数据、更新复杂状态、执行必要的内核服务，最后重新使能设备中断。

### **16.7.2 端到端时序**

Secure IRQ → Normal World → Secure Bottom Half

1. **Secure Peripheral Interrupt**

   硬件中断到来，OP-TEE Native IRQ Handler 被调用。
2. **Top Half**

   读取中断状态，必要时暂时关闭 RX IRQ，并记录待处理数据。
3. **Asynchronous Notification**

   调用 `notif_send_async(NOTIF_VALUE_DO_BOTTOM_HALF, guest_id)`。
4. **Non-Secure IRQ**

   Linux OP-TEE Driver 接收到 Notification Interrupt，并通过 Fast Call 读取 Pending Value。
5. **Linux Worker**

   当 Value 表示需要 Bottom Half 时，Linux 安排一个可睡眠的线程上下文。
6. **Yielding Call**

   Linux 发起 `OPTEE_MSG_CMD_DO_BOTTOM_HALF`，进入 OP-TEE。
7. **Secure Bottom Half**

   OP-TEE 调用相应 Driver 的 `yielding_cb`，执行较长操作，并恢复设备中断配置。
8. **Completion**

   Bottom Half 返回，后续设备事件可以继续触发新的中断。

这个模型在上游的 `plat-vexpress/main.c` 中也有实际示例：Console IRQ Handler 在异步通知可用时禁用 RX Interrupt 并触发 Bottom Half Notification，Yielding Callback 则处理 Console Data，随后重新使能 RX Interrupt。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### **16.7.3 设计一个 Secure Crypto Driver**

下面我们设计一个教学用状态机，体现真正需要保护的内容。

Secure Crypto Operation State Machine

**IDLE**

**RUNNING**

DMA active · IRQ enabled

**COMPLETION_PENDING**

IRQ acknowledged · Work recorded

**PROCESSING**

Secure Bottom Half

**COMPLETED / FAILED**

Wake waiter · Reclaim resources

对应 Top Half 的概念性伪代码：

```
static enum itr_return crypto_irq_handler(
    struct itr_handler *h)
{
    struct crypto_dev *dev = h->data;
    uint32_t status;
    uint32_t flags;

    status = crypto_read_irq_status(dev);

    if (!status)
        return ITRR_NONE;

    crypto_mask_irq(dev);
    crypto_ack_irq(dev, status);

    flags = cpu_spin_lock_xsave(&dev->lock);
    dev->pending_status |= status;
    cpu_spin_unlock_xrestore(&dev->lock, flags);

    notif_send_async(NOTIF_VALUE_DO_BOTTOM_HALF,
                     dev->guest_id);

    return ITRR_HANDLED;
}
```

教学伪代码，并非 OP-TEE 上游已有的 Crypto Driver；省略了初始化、错误处理、控制器注册、虚拟化和资源回收。设备状态寄存器的实际确认顺序必须遵守具体硬件协议。

对应 Bottom Half：

```
static void crypto_bottom_half(struct crypto_dev *dev)
{
    uint32_t pending;
    uint32_t flags;

    flags = cpu_spin_lock_xsave(&dev->lock);
    pending = dev->pending_status;
    dev->pending_status = 0;
    cpu_spin_unlock_xrestore(&dev->lock, flags);

    if (pending)
        crypto_process_completion(dev, pending);

    crypto_unmask_irq(dev);
}
```

这个模型只是基础版本，真正实现仍然有很多问题需要进一步处理。

### **16.7.4 第一个竞态：Event Arrives Before Bottom Half**

时间线：

```
CPU0 / Secure IRQ             Linux Worker
----------------             ------------

IRQ #1 arrives
Record Pending = 1
Send Notification

                              Worker scheduled

IRQ #2 arrives
Record Pending |= 2

                              Read Pending
                              Process 1 and 2
```

如果 Pending State 是 Bitmask，并且每一种 Bit 只表示一种事件类别，这样也许能够工作。

但如果两次中断属于两个不同 DMA Request，而代码简单写成 `pending_status |= COMPLETE_BIT`，就会丢失“究竟完成了几个请求”的信息。

所以要根据业务语义选择 Counter、Queue、Completion Descriptor 或 Bitmask，而不是一律使用单个 Pending Flag。

### **16.7.5 第二个竞态：Lost Wakeup**

设想：

```
Bottom Half                     Secure IRQ
-----------                     ----------

Read pending = 0
                                New event arrives
                                Mark pending = 1
                                Send notification

Clear pending
Return
```

如果清理状态的时序错误，可能覆盖新到达的事件。

更安全的思路是：

- 在锁保护下原子地提取一批 Pending Work。
- 只处理已经提取的内容。
- 对后续新产生的事件重新进行检查。
- 避免把通知边沿本身当成唯一真实状态。
- 在停止和重新使能中断时维护严格的设备状态协议。

### **16.7.6 第三个竞态：Notification 已经发出，但 Linux 不运行 Worker**

假设 Normal World 负载很高，Bottom Half 被延迟数百毫秒。

这时必须明确：

- DMA Buffer 是否仍然被安全地保留？
- Device 是否允许持续产生新请求？
- Secure Memory 是否可能被耗尽？
- Timeout 是否由可信 Timer 判定？
- 如果 Linux 永远不调度 Worker，安全状态能否保持？

这是一个安全系统非常重要的区别：

**Normal World 不可信，并不只意味着它可能发送恶意参数，也意味着它可能拒绝提供服务。**

因此，对于需要 Normal World 协助才能完成的安全操作，通常不能承诺对恶意 Normal World 的无条件 Liveness。

但仍然应当保证，即使 Normal World 不提供服务，也不会因此导致 Secure Memory Corruption、越权 DMA 或密钥泄露。

### **16.7.7 Bottom Half 不是一个拥有独立调度器的安全线程**

虽然文档中经常出现 Bottom Half Thread 的说法，但不应该把它等同于 Linux `kthread` 或 RTOS Worker Thread。

更准确的理解是：

- Secure IRQ 产生 Work Request。
- Normal World 收到 Notification。
- Linux 选择何时安排 Yielding Call。
- OP-TEE 获得一个可以执行相应工作的 Trusted Thread Context。
- Bottom Half 在这个受控上下文中运行。

因此，它仍然受到 OP-TEE Thread Pool、Linux Scheduling 和 Transport Protocol 的共同影响。

## **九、16.8 — Nested Interrupt、Lock Ordering 与 Exception Safety**

现在进入真正复杂的多核场景。

### **16.8.1 OP-TEE 中的 Nested Interrupt 是什么意思？**

至少存在三种不同的嵌套概念：

**第一种：Interrupt Controller 层次上的嵌套。**

例如：

```
GIC
 |
 +-- Secure Peripheral Interrupt Controller
       |
       +-- Crypto IRQ
       +-- Timer IRQ
       +-- Security Monitor IRQ
```

Generic IRQ Framework 可以通过级联 Controller 分发不同的中断。

**第二种：CPU Exception 的抢占嵌套。**

例如，在某个异常处理过程中，允许一个优先级更高的中断再次进入异常处理。

这会影响 Stack、Exception Mask、Priority Mask、寄存器保存和锁设计。

**第三种：跨世界执行层面的嵌套。**

例如：

```
Linux
  |
  +-- Enter OP-TEE
         |
         +-- Foreign IRQ
                |
                +-- Return Linux
                       |
                       +-- Secure IRQ
                              |
                              +-- Enter OP-TEE IRQ Entry
```

第三种情况下，不能简单地使用一个普通 Kernel Call Stack 来描述整个执行过程。

还必须考虑 Monitor Context、Thread Suspend、Entry Stack 和 Resume Protocol。

### **16.8.2 为什么需要区分这些嵌套？**

因为第一种嵌套不必然意味着 CPU 允许 Exception Handler 被另一个 IRQ 抢占。

多个级联 Interrupt Controller 可以在一次顶层 IRQ Handler 中依次完成分发。

而第三种也不意味着同一个 Trusted Thread 被嵌套执行了两次。

它可能只是 CPU 在已有 Thread 挂起期间，通过另一个异常入口处理 Native Interrupt。

### **16.8.3 异常上下文中的锁层次**

对于一个安全驱动，可以采用这样的锁依赖模型：

```
Trusted Thread
    |
    +-- Device Mutex
    |      |
    |      +-- Short Device State Spinlock
    |
    +-- DMA State Lock
```

但是 IRQ Handler 只能参与允许在 Atomic Context 下使用的短临界区。

不能设计成：

```
IRQ Handler
    |
    +-- Wait for Device Mutex
```

也不应该让某个 Spinlock 的 Critical Section 调用：

```
RPC
Pageable Function
Sleeping Allocator
Blocking Device Operation
```

真正需要审查的不是锁的名称，而是 **所有持锁路径中的执行上下文和传递调用**。

### **16.8.4 Per-CPU State 与 Global State**

OP-TEE 中通常同时存在：

- Per-CPU Exception State。
- Per-CPU Current Thread。
- Global Thread Table。
- Global Interrupt Handler Registry。
- Device-specific Lock。
- Global Notification State。

如果一个中断可能路由到任何 Online CPU，那么驱动不能在没有同步协议的情况下，假设某个全局状态始终由固定 CPU 更新。

另外，Per-CPU 并不等于天然无竞争。

如果 CPU0 的一个 Thread 在进入 Foreign Interrupt Forwarding 前使用某个 Per-CPU 状态，而恢复时改在 CPU1 上执行，那么先前依赖 CPU0 本地资源的设计可能不再成立。

这需要在 Thread Migration、Interrupt Affinity 和 Resource Lifetime 三个层面共同分析。

### **16.8.5 一个死锁案例**

假设：

```
CPU0: Thread A
  lock(GlobalState)
  ...
  foreign IRQ arrives

CPU1: Thread B
  lock(DeviceState)
  ...
  lock(GlobalState)

CPU0 later:
  resume Thread A
  ...
  lock(DeviceState)
```

即使 OP-TEE 能成功保存和恢复 Thread A，整个系统仍可能出现：

```
Thread A holds GlobalState
Thread A waits DeviceState

Thread B holds DeviceState
Thread B waits GlobalState
```

这不是 Foreign Interrupt Forwarding 自己导致的死锁，而是已有的 Lock Ordering 错误，被跨世界调度暴露出来。

因此：

**Context Suspend/Resume 的正确性，不能证明 Kernel Concurrency 的正确性。**

## **十、16.9 — 一套统一的异常处理设计原则**

完成前面的讨论之后，可以把 OP-TEE 的异常处理归纳成四个决策步骤。

### **16.9.1 Decision 1：这是同步异常还是异步事件？**

同步异常通常需要检查：

- 当前异常指令。
- Exception Syndrome。
- Fault Address。
- Faulting Privilege Level。
- Memory Mapping 与 Access Permission。

异步中断通常需要检查：

- Interrupt Ownership。
- Interrupt Routing。
- Interrupt Controller State。
- Per-CPU Exception State。
- Interrupt Mask 与优先级。

### **16.9.2 Decision 2：当前上下文是否可以处理它？**

例如：

| 场景                     | 是否能直接完成                  |
| ---------------------- | ------------------------ |
| Native IRQ 读取和清理设备状态   | 通常可以                     |
| Native IRQ 执行文件系统 RPC  | 不可以                      |
| User TA 合法 Pager Fault | 在 Pager 支持和上下文允许时可以      |
| User TA 非法地址访问         | 应执行 User TA 故障处理         |
| Kernel 不可恢复非法访存        | 通常需要 Kernel Panic        |
| Foreign IRQ 属于 Linux   | 应按平台协议转发/交还              |
| Secure IRQ 需要复杂处理      | Top Half + Deferred Work |

### **16.9.3 Decision 3：如何维持安全不变量？**

可以把安全不变量整理为六类：

| 不变量                 | 要保证什么                            |
| ------------------- | -------------------------------- |
| Context Integrity   | 异常返回后寄存器、栈、特权状态正确                |
| Memory Isolation    | 不能恢复错误 TA/Guest 的 Memory Mapping |
| Interrupt Ownership | 不允许错误安全域处理或完成中断                  |
| Resource Lifetime   | DMA、Thread、Buffer 等资源不会提前释放      |
| Lock Correctness    | 没有等待环、睡眠型锁误用或持锁 RPC              |
| Fault Containment   | User TA 故障不能无约束地污染 Kernel        |

### **16.9.4 Decision 4：异常之后应该继续、挂起还是终止？**

Exception Handling Outcome

Resume

已处理的 Native IRQ 或合法可恢复 Page Fault

Suspend

需要 Foreign Interrupt Forwarding 或 RPC

TA Termination

不能恢复的 User TA Fault

Core Panic

无法安全恢复的核心内核故障

这套决策框架是日常排查异常问题时非常有用的第一层思考模型。

## **十一、16.10 — 三个真实工程级场景：如何从现象追踪到根因**

前面的内容介绍了各个子系统。现在开始用真实工程问题的方法把它们组合起来。

### **案例 A：Secure Crypto Engine 第一次正常，第二次永远不再中断**

**故障现象**

- 第一次 AES 请求成功。
- 第二次 AES 请求超时。
- DMA 设备状态显示已完成。
- OP-TEE 没有再次调用对应的 IRQ Handler。

这种问题首先不要怀疑 Crypto Algorithm。

按照执行链逐层排查：

```
Crypto Request
     |
     v
DMA Started?
     |
     v
Device IRQ Status Set?
     |
     v
Interrupt Controller Pending?
     |
     v
Interrupt Routed to Secure World?
     |
     v
OP-TEE Exception Entry?
     |
     v
interrupt_call_handlers()?
     |
     v
Driver IRQ Callback?
     |
     v
Bottom Half Scheduled?
```

重点观察四项状态：

| 状态                     | 排查目的                     |
| ---------------------- | ------------------------ |
| Device IRQ Status      | 硬件是否真的完成                 |
| Controller Enable/Mask | 中断是否被错误屏蔽                |
| `ITRR_HANDLED` 返回      | Framework 是否把 IRQ 判定为未处理 |
| Bottom Half Completion | 是否忘记重新使能设备中断             |

特别需要检查 `interrupt_call_handlers()` 的未处理分支。

如果 Handler 返回 `ITRR_NONE`，而没有其他 Handler 成功处理，中断可能被框架屏蔽。

如果 Top Half 关闭了 IRQ，却依赖 Bottom Half 重新打开，那么 Linux Worker 没有成功运行，也会出现类似现象。

这两个问题虽然都表现为“第二次没中断”，但对应的修复完全不同。

### **案例 B：开启 Pager 后，Secure IRQ 偶尔导致 OP-TEE Panic**

**故障现象**

- `CFG_WITH_PAGER=n` 时系统稳定。
- `CFG_WITH_PAGER=y` 时随机异常。
- Panic Stack 中出现 `abort_handler()` 或 Pager 相关函数。
- 故障与高频 Secure Interrupt 有关联。

可能原因之一是，IRQ Handler 的传递调用路径访问了不允许在 IRQ Context 下缺页的代码或数据。

排查步骤：

1. 确认 Fault PC、Fault VA、异常来源和当前异常栈。
2. 判断 Fault 来自 IRQ Handler 本身还是被中断的 Trusted Thread。
3. 检查 Handler 和它调用的全部关键函数的 Section。
4. 检查 Driver Private Data 是否位于合适的内存区域。
5. 检查中断期间是否访问需要 Pager 的动态映射。
6. 确认 IRQ Handler 是否错误调用了普通分配器或其他可阻塞服务。

**架构修复原则：** 让 Atomic Context 所必需的代码和数据始终可访问；把复杂操作转移到 Yielding Context。

不能仅仅通过增大 Pager Cache 来“降低问题发生概率”，因为这没有解决执行上下文约束。

### **案例 C：SMP 平台上偶发 TA Context Corruption**

**故障现象**

- 单核测试正常。
- 多核高并发下出现随机 TA Crash。
- 有时表现为 Page Fault，有时表现为 Session Handle 错误。
- Stack Trace 显示异常发生在 Foreign Interrupt 后的 Resume 路径。

此时应该怀疑的不是某一条孤立的汇编指令，而是 Thread Resume 过程中是否维持了完整的状态一致性。

需要确认：

```
Thread ID
   |
   +-- Thread State == SUSPENDED?
   |
   +-- No CPU currently owns it?
   |
   +-- Correct TA User Mapping?
   |
   +-- Correct Saved Registers?
   |
   +-- Correct Exception State?
   |
   +-- Correct FP/SIMD Context?
   |
   +-- Correct Guest / Partition Context?
```

还要检查 Resume Protocol 输入是否被正确验证。

例如，在支持多个 Normal World Guest 的系统中，只有 Thread ID 正确还不够。还必须遵守该实现对 Guest Identity 与 Resume Ownership 的约束。

真正的问题可能是：

**系统恢复了一个看似合法的 Thread，却没有恢复其完整的安全执行身份。**

这类错误往往比普通的空指针访问更危险。

## **十二、16.11 — Arm 与 RISC-V：哪些设计可以复用？**

你的主要工作方向涉及 RISC-V，因此我们还需要判断这一章的哪些设计属于 Architecture-Neutral，哪些依赖 Arm TrustZone。

| 设计问题              | Arm OP-TEE                  | RISC-V 适配时的关注点                   |
| ----------------- | --------------------------- | -------------------------------- |
| 异常入口              | EL1 Exception Vector        | `stvec` / 对应 Domain 的 Trap Entry |
| 异常原因              | ESR、Interrupt Exception     | `scause`、`stval` 等               |
| 异常返回              | `eret`                      | `sret` 或特定 Domain Return         |
| Interrupt Routing | GIC、EL3 Firmware            | PLIC/APLIC/IMSIC、SBI/安全域机制       |
| Context Save      | GPR、PSTATE、系统寄存器            | GPR、`sstatus`、`sepc` 及相关扩展状态     |
| Native IRQ        | Secure Interrupt Handler    | 安全域专属 Interrupt Routing          |
| Foreign IRQ       | Monitor/Protocol Forwarding | Domain Switch 或安全固件定义的转发机制       |
| Notification      | Async IRQ + Transport       | 需要根据平台 Transport 设计              |
| Bottom Half       | Yielding Thread             | 可复用概念，但必须实现合法的调度入口               |
| Fault Containment | User TA / Secure Core       | User TA / Secure Domain 的隔离和恢复策略 |

这里一定要特别强调：

**RISC-V 标准 S-mode 并不天然提供 Arm Secure/Non-Secure World 的对应机制。**

因此，不能仅凭 `sret`、`stvec` 或 `sstatus` 就认为已经实现了 OP-TEE 所需的安全域中断隔离。

必须额外解决：

- Secure Domain Ownership。
- Interrupt Isolation。
- Domain Context Switching。
- Secure Memory Protection。
- Trusted/Untrusted Interrupt Routing。
- Domain State Restoration。

这些可能依赖平台安全扩展、M-mode Firmware、独立安全控制器或其他安全域实现。

### **一条值得深入研究的 RISC-V 路径**

假设你要为 RISC-V OP-TEE 支持安全 DMA Completion Interrupt。

那么首先需要建立这样的路径：

```
Secure DMA Device
        |
        v
Interrupt Controller
        |
        v
Secure Domain Routing
        |
        v
OP-TEE Trap Entry
        |
        v
Generic Interrupt Framework
        |
        v
Secure DMA Driver
        |
        v
Completion State
        |
        v
Notification / Deferred Processing
```

研究重点不是先实现一个 ISR，而是先证明：

**无论 CPU 当前执行 Normal Domain 还是 Secure Domain，这个中断都只会被授权的 Handler 接收，并且切换前后的权限和状态仍然一致。**

这个问题与 RISC-V SMMPT 的系统级设备访问控制、OpenSBI/RDSM 的执行域管理以及 DMA Memory Ownership 是相关的，但三者不能互相替代。

## **十三、16.12 — 本章源码阅读和动手实验**

建议在 4.11.0 Tag 下完成下面三组实验。

### **实验一：Native IRQ Handler 的完整调用链**

**目标：** 证明从 Exception Vector 到设备 Handler 的每个步骤。

源码入口：

```
core/arch/arm/kernel/thread_a64.S
core/kernel/interrupt.c
core/drivers/gic.c
core/arch/arm/plat-vexpress/main.c
```

实验方法：

1. 选择上游平台已有、确实能够触发的 Secure IRQ。
2. 在 `interrupt_main_handler()` 或对应 Controller Entry 设置断点。
3. 观察中断 ID、所属 Controller 和 Handler List。
4. 进入 `interrupt_call_handlers()`。
5. 检查 Handler 返回值和 Controller EOI。
6. 观察中断处理前后的 Current Thread、Exception Mask 和 Stack。

实验必须输出一条完整的 Call Trace，并说明中断发生时 CPU 是否正在执行 Trusted Thread。

### **实验二：Foreign IRQ Suspend/Resume**

**目标：** 理解 Trusted Thread 并非始终连续占有 CPU。

重点函数：

```
thread_state_suspend()
thread_resume_from_rpc()
```

需要记录：

```
Thread ID
Thread State
Current CPU
Current Thread
Saved PC / PSTATE
User Mapping
Exception Mask
```

在可能支持的实验平台上，让一个 TA 执行相对较长的计算，观察 Foreign Interrupt 造成的挂起与恢复。

关键验收标准不是看到调用了 Resume 函数，而是能够验证：

- Resume 前 Thread 为 `SUSPENDED`。
- 恢复时没有其他 CPU 同时执行同一个 Thread。
- 正确恢复了 TA User Mapping。
- TA 最终结果与没有发生挂起时一致。

### **实验三：Async Notification + Bottom Half**

**目标：** 验证 Secure IRQ → Notification → Linux Worker → Secure Bottom Half。

可以研究 VExpress/QEMU 平台相关的 Console IRQ 示例，或在具备相应硬件/中断支持的平台上实现一个实验驱动。

关键测试包含：

- 单次事件。
- 高频连续事件。
- 两个 CPU 同时产生事件。
- Notification 已发送但 Worker 延迟。
- Bottom Half 执行期间再次发生事件。
- Driver Disable/Remove 与 Pending Event 竞争。
- Notification Stop/Start 期间的并发状态变化。

尤其需要关注 Shutdown/Stop 过程中的 Pending Work。不能在 Driver Resource 已经释放之后，仍允许 Bottom Half 使用原来的 Private Data。

## **十四、Change Impact Analysis：如果要修改 IRQ/Notification Core，如何判断影响范围？**

本章可以形成以下修改影响矩阵：

| 修改对象                        | 直接影响                 | 跨模块风险                           | 必要测试                         |
| --------------------------- | -------------------- | ------------------------------- | ---------------------------- |
| Exception Vector            | 异常入口与寄存器保存           | Syscall、Pager、TA 返回、IRQ         | User/Kernel Exception、SMP    |
| Foreign IRQ Forwarding      | Thread Suspend       | Linux Scheduling、RPC、TA Context | 长时间 TA、IRQ Stress            |
| `thread_state_suspend()`    | Thread Lifecycle     | VM、VFP、SPMC、Guest Context       | SMP Resume、Context Integrity |
| `interrupt_call_handlers()` | Driver Dispatch      | 级联 Controller、Shared IRQ        | IRQ Storm、Shared IRQ         |
| `abort_handler()`           | Fault Classification | Pager、TA Panic、Kernel Panic     | Page Fault、非法访问              |
| Notification Framework      | Event Delivery       | Linux Driver、Mutex、Bottom Half  | 丢事件、重复事件、Stop/Start          |
| Driver Bottom Half          | Deferred Work        | DMA、Memory Lifetime、Power       | Concurrency、Reset、Timeout    |

一个尤其重要的设计规则是：

> 修改 Interrupt Framework 时，不能只测试“中断能不能正常收到”，还必须测试异常入口、执行上下文、错误恢复和资源释放。

因为最严重的 Bug 往往不是中断没有执行，而是中断执行了，但在错误的安全上下文中访问了错误的状态。

# **十五、Staff Engineer 级面试与架构评审题**

下面是本章最重要的考核内容。

我不建议把它们当成普通的知识问答题。每道题都模拟一个真实工程决策或疑难故障，要求分析设计权衡、安全不变量、失败模式和可验证性。

共 12 道题，按综合难度由浅入深排列。每题给出考察重点与参考推理方向，但不限定唯一答案。

## **Question 1 — OP-TEE 为什么不直接实现完整的 Secure Thread Scheduler？**

**场景**

你正在设计一个新的 RISC-V TEE OS。团队中的一名工程师认为，OP-TEE 依赖 Normal World Scheduling 是设计缺陷，应该直接引入一个完整的 Secure Scheduler，并允许 Secure Interrupt 唤醒一个安全线程执行任意任务。

**问题**

请你从 Security、TCB、Resource Ownership、Liveness、Preemption、SMP 和 Real-Time Latency 七个角度评估这项提议。

假设系统还必须支持不可信 Linux 在高负载下持续拒绝调度 OP-TEE。

考察重点：能够区分功能必要性、可信调度能力与 TCB 复杂度。

**参考分析：**

引入 Secure Scheduler 可以改善不依赖 Normal World 的异步工作能力，也可能提高某些安全实时任务的可预测性。

但是，需要付出的代价包括：

- 实现 Secure Thread Runqueue、Priority 和 Preemption。
- 为 Timer、Context Switch 和 CPU Affinity 建立完整机制。
- 扩大 Kernel TCB。
- 处理 Secure Task 与 Normal World Task 之间的 CPU 时间分配。
- 解决 Priority Inversion 和可能的跨世界资源等待。
- 重新设计 RPC、Sleep、Wakeup 和 Bottom Half 模型。

更深层的问题是：**增加一个 Secure Scheduler，是否能够保证安全服务的 Liveness？**

如果安全服务仍然依赖 Normal World 的存储、网络或其他 RPC，那么独立调度器并不能完全消除可用性依赖。

因此，是否引入 Secure Scheduler，必须由需求驱动，而不是简单认为传统 RTOS 的设计一定更完整。

## **Question 2 — Foreign IRQ 之后，Trusted Thread 在另一个 CPU 上恢复是否安全？**

**场景**

CPU0 正在运行 TA-A，期间发生 Foreign Interrupt。Linux 后续把调用线程迁移到 CPU3，并尝试恢复原来的 Trusted Thread。

系统偶尔出现 VFP 运算错误或非法 Page Fault。

**问题**

请给出你认为必须恢复或验证的全部硬件与软件状态，并设计一套验证方案，证明迁移前后的执行行为等价。

考察重点：跨 CPU Context Migration 的完整性，而不是简单的寄存器保存。

**参考分析：**

必须考虑 GPR、PC、PSTATE、FP/SIMD、Thread Ownership、Kernel Stack、TA Mapping、ASID/TLB、Per-CPU State、Exception Mask 和 Guest Identity。

还要区分两种状态：

- 可随 Trusted Thread 迁移的状态。
- 物理上属于 CPU0、不能简单复制到 CPU3 的状态。

验证方案应包含连续计算结果校验、频繁 Foreign IRQ、CPU Migration Stress、跨核地址空间切换和 FP/SIMD 上下文污染测试。

最重要的结论是：**恢复逻辑必须维护一个完整的 Execution Domain，而不只是恢复 CPU Registers。**

## **Question 3 — 一个高频 Secure IRQ 导致 Linux 延迟超过 100ms，应该如何优化？**

**场景**

硬件 Crypto Engine 每秒产生数千个 Completion Interrupt。

Secure IRQ Handler 中有大量日志、DMA Buffer Cleanup 和 Hash Validation。

业务团队希望通过增大 IRQ Priority 解决问题。

**问题**

请提出端到端的 Latency Optimization 方案，并解释为什么简单提高中断优先级可能让系统变得更糟。

考察重点：Worst-Case Latency、Interrupt Masking、Batching 和跨世界协作。

**参考分析：**

首先测量 Interrupt Arrival → Handler Entry → Handler Exit → EOI → Bottom Half → Completion 的分段延迟。

优化方向包括减少 IRQ Context 工作、Batch Completion、限制日志、使用 Deferred Processing，以及验证 DMA 中断是否支持 Coalescing。

提高 IRQ Priority 可能进一步延迟低优先级安全中断或其他系统事件，甚至放大 Starvation。

关键不是追求单个 Handler 最快，而是控制整个系统的 Worst-Case Interrupt Latency 和 Work Completion Latency。

## **Question 4 — 如何设计一个绝不丢失 DMA Completion 的 Notification 系统？**

**场景**

一个安全 DMA Engine 支持 32 个并发 Request。

Completion 可以乱序到来。Notification 允许合并，Normal World 可能延迟处理，设备可能发生 Reset。

**问题**

设计整个 Completion Protocol，包括请求 ID、状态机、Notification、Buffer Lifetime、Reset Recovery 和 Duplicate Completion Handling。

考察重点：通知与真实事件的分离、Exactly-Once Completion 的工程语义。

**参考分析：**

不能把 Notification Count 当成 Completion Count。

每个 Request 应有稳定的 ID、Generation、Owner 和 State，例如：

```
SUBMITTED -> RUNNING -> COMPLETED -> REAPED
                     \-> FAILED
                     \-> CANCELLED
```

需要在受保护的 Completion Queue 中维护状态，并在锁保护下处理状态迁移。

为防止设备 Reset 后旧 Completion 与新 Request 混淆，可以使用 Generation Counter 或类似机制。

Buffer 必须在 DMA 不再访问之后才能释放。Notification 可以重复，但完成状态转换应具有幂等性。

## **Question 5 — Pager 与 Secure IRQ 为什么可能产生循环依赖？**

**场景**

你引入一个新 Secure IRQ Driver。它的 IRQ Handler 使用了一个内存分配接口，该分配接口可能触发 Page Fault。

启用 Pager 后系统偶发 Panic。

**问题**

请给出完整的依赖分析。如何证明哪些代码必须 Unpaged？如果确实需要访问 Pageable Resource，应该如何重新设计 Driver？

考察重点：异常上下文对内存系统的依赖约束。

**参考分析：**

需要建立 IRQ Entry 的传递调用图和内存访问图，覆盖 Controller、Handler、Driver State、Lock、Callback 与其他服务。

Unpaged 约束必须覆盖运行时实际调用路径，而非仅仅入口函数。

可以将需要访问 Pageable Resource 的操作移到 Bottom Half；IRQ Handler 只读写常驻的固定状态。

同时应构造压力测试，使 Page Cache 紧张，并主动诱发相应的 IRQ。

## **Question 6 — TA 发生 Page Fault 时，什么情况下应该 Panic 整个 OP-TEE？**

**场景**

一个 User TA 在访问合法 VM Region 时出现 Translation Fault。Pager 尝试修复，但内部发现 Page Metadata 不一致。

团队有人主张无论什么错误都只杀死 TA，以保证系统可用性。

**问题**

你是否同意？请建立一套 Fault Containment Policy，并解释如何判断问题是 TA-local 还是 Kernel-global。

考察重点：Fault Containment 与可信内核完整性。

**参考分析：**

不能因为 Fault 最初来自 User TA，就假设所有后续问题都可以限制在 TA 内。

如果只是非法用户地址，终止 TA 通常合理。

但是，如果 Kernel Pager 的全局元数据、页表所有权或内存分配结构已经损坏，继续执行可能违反其他 TA 的隔离。

需要根据状态是否还能证明一致，决定恢复或 Panic。

目标不是最大化继续运行的概率，而是在可证明安全的前提下保持可用性。

## **Question 7 — Secure IRQ 和 Foreign IRQ 同时发生，谁应先处理？**

**场景**

CPU 正在 OP-TEE 中执行一个长时间 TA Operation。

此时 Non-Secure Timer 和 Secure Watchdog 同时 Pending。

**问题**

设计处理优先级和执行流程，并解释其中哪些选择由硬件规定、哪些由 Firmware 决定、哪些由 OP-TEE 决定。

考察重点：Interrupt Routing 与 Policy 的职责边界。

**参考分析：**

不能单纯回答“Secure IRQ 优先”。

必须检查 GIC Priority、Interrupt Group、Mask、Routing Register、EL3 Policy 和 CPU 当前 Exception State。

如果 Secure Watchdog 有真实时间约束，其配置必须确保在指定运行状态下可以及时得到处理。

同时要评估 Foreign Interrupt Forwarding 是否可能导致 Watchdog 响应超时，以及长时间屏蔽中断是否违反系统实时要求。

正确方案应通过硬件配置和实测 Worst-Case Latency 共同证明。

## **Question 8 — RISC-V 上实现 OP-TEE Native Interrupt，需要哪些真正的架构前提？**

**场景**

一个 16-Hart RISC-V SoC 有 OpenSBI、SMMPT 和独立的安全域机制。

工程师认为只要在 OP-TEE 中实现 `stvec`、读取 `scause`，再接入 IMSIC，就完成了安全中断支持。

**问题**

请从 Interrupt Ownership、Domain Isolation、Context Switching、SMP、DMA 和 Device MMIO Access 的角度评估这一判断。

考察重点：软件异常处理与硬件安全隔离之间的区别。

**参考分析：**

`stvec` 和 `scause` 只是 Trap Handling 的一部分。

还需要证明：

- Secure Interrupt 只能被授权 Domain 接收。
- Normal World 不能修改相关 Routing 和 Pending State。
- Domain Switch 前后的中断使能及 Pending State 一致。
- 不同 Hart 的 Interrupt Context 正确隔离。
- 设备 DMA 和 MMIO 权限属于正确的安全域。
- 不会因为转发或恢复机制而让 Normal World 注入 Secure Context。

SMMPT 可以承担特定系统级访问保护职责，但不能独立代替中断路由和执行上下文隔离。

## **Question 9 — Async Notification 的 Stop/Start 竞态如何设计？**

**场景**

Linux 正在关闭 OP-TEE Driver，准备停止 Asynchronous Notifications。

与此同时，一个 Secure IRQ 正在请求 Bottom Half。另一个 CPU 又发起新的 Notification Start。

**问题**

请设计 Shutdown State Machine，说明 Pending Work、Driver Resource 和 Guest Identity 应该如何处理。

考察重点：生命周期、异步事件、回调重入和并发协议。

**参考分析：**

首先定义状态：

```
STOPPED
   |
   v
STARTING
   |
   v
RUNNING
   |
   v
STOPPING
   |
   v
STOPPED
```

但真实实现也可能采用更精简的状态表示。

核心是不允许：

- 已释放的 Driver Resource 被 Pending Bottom Half 使用。
- STOPPING 之后产生没有消费者的无界 Work。
- 新的 START 覆盖尚未结束的 STOP 清理。
- 一个 Guest 的 Notification 被另一个 Guest 消费。

需要通过锁、State Generation 和 Pending Work Drain/Cancel Protocol 建立明确的生命周期关系。

## **Question 10 — 如何证明一个 Secure IRQ Handler 不存在死锁？**

**场景**

你负责审查一个包含 20 个函数调用的 Secure IRQ Handler。

它会读取多个硬件寄存器、更新共享状态、调用 Crypto Framework，并可能通知其他 CPU。

**问题**

请说明你的 Code Review 方法。仅通过搜索 `mutex_lock()` 是否足够？如何检查 Transitive Call Graph、Lock Ordering、Pager Dependency、Cross-CPU 等待和 Memory Barrier？

考察重点：系统化的并发安全证明方法。

**参考分析：**

不能仅通过搜索 Mutex API 来证明安全性。

需要建立：

- 完整调用图。
- 可能睡眠的函数集合。
- Lock Dependency Graph。
- IRQ Mask 与 Preemption 状态。
- 可能触发 Page Fault 的访问集合。
- 跨 CPU Message/Wait 关系。
- Driver 与 Controller 的状态同步约束。

所有可达路径都应该满足 Atomic Context 的执行限制。

必要时使用静态分析、断言、锁检查和压力测试验证假设。

但测试只能增加信心，不能代替对关键同步不变量的推导。

## **Question 11 — Normal World 可以恶意拒绝调度 Bottom Half，安全设计应如何应对？**

**场景**

OP-TEE 通过异步通知请求 Linux 安排 Secure Bottom Half。

攻击者完全控制 Linux，可以任意延迟、丢弃或重排相关服务请求。

Secure Crypto Engine 同时持有一个包含敏感数据的 DMA Buffer。

**问题**

如何设计一个安全机制，使 Normal World 不配合时最多造成可接受的 Denial of Service，而不会引入 Memory Corruption、Key Leakage 或错误 DMA？

考察重点：Safety 与 Liveness 的分离，以及不可信 OS 下的资源管理。

**参考分析：**

应将 DMA Buffer 的安全生命周期绑定到受保护的 Device Operation State，而不是绑定到 Linux Notification 是否到达。

Normal World 不调度时，Device 需要保持受控状态，不能无限制消耗 Secure Resources。

可以根据硬件能力设计 Secure Timeout、Abort、Reset、Quota 和 Cleanup。

但必须证明 Reset 后 DMA 已真正停止，才能撤销 Buffer Mapping 或重新分配内存。

并且不能将 Normal World 提交的 Completion Result 直接作为硬件真实完成的可信证据。

最终要保证 Safety Property 不依赖 Normal World 的合作；Liveness 则应明确描述依赖条件。

## **Question 12 — 设计一个跨 OP-TEE、OpenSBI、SMMPT 的安全异步执行架构**

**最终综合题**

你的 SoC 设计如下：

```
16 x RISC-V Harts
       |
       +-- Linux / Normal Domain
       |
       +-- OP-TEE / Secure Domain
       |
       +-- OpenSBI / Security Domain Manager
       |
       +-- Secure DMA Crypto Engine
       |
       +-- SMMPT / System-level Protection
```

要求：

- Secure Crypto Engine 只能访问授权 Secure Memory。
- Linux 可以请求 Crypto Service，但不能直接控制 Engine 的安全状态。
- Crypto Completion 通过异步中断通知 OP-TEE。
- OP-TEE 不允许在 IRQ Context 执行复杂算法或阻塞操作。
- 16 个 Hart 可以并发执行不同的 TA。
- 允许 CPU Suspend、Device Reset 和 DMA Failure。
- Normal World 可以被恶意控制。
- 要求尽量降低 TCB 和复杂度。

**问题**

请设计完整的软件架构，并给出：

1. Interrupt Routing Ownership。
2. Secure Domain Entry/Exit Protocol。
3. Trusted Thread 与 Bottom Half Execution Model。
4. DMA Buffer Ownership 和生命周期。
5. SMMPT Permission Transition。
6. Memory Barrier / TLB / Device Translation Synchronization。
7. Secure Device Reset Recovery。
8. SMP Locking。
9. Threat Model。
10. Verification Strategy。

考察重点：能否把安全操作系统、特权架构、SoC 硬件和分布式执行协议统一为一个可验证的设计。

**参考设计思路：**

我会先分解成三个彼此独立但必须协作的控制体系。

**体系一：Execution Control**

由安全域切换机制保证 CPU 只在正确的安全域中执行 OP-TEE，并保证 Trap/Interrupt/Thread Context 的完整性。

**体系二：Memory Access Control**

由 MMU、PMP、SMMPT 或其他实际存在的硬件保护机制共同完成 CPU 与 Device 的访问权限控制。

不能把 CPU Memory Mapping 等同于 Device DMA Permission。

**体系三：Async Work Control**

由 Interrupt Controller 递送 Secure Completion Event，OP-TEE Top Half 记录可信设备状态，再通过安全的 Deferred Processing 机制完成后续工作。

三个体系之间必须遵守共同的状态机。

例如，某个 DMA Request 从 `RUNNING` 进入 `COMPLETED` 后，并不意味着可以立即释放 Buffer。

只有在证明 Device 不再访问目标内存、相关的内存同步与访问权限撤销已经完成之后，才允许回收或复用资源。

整个方案的验收不能仅依靠“Linux + OP-TEE + Crypto Test 可以跑通”，还需要包括：

- 恶意 Normal World 输入。
- Interrupt Storm。
- Cross-Hart Race。
- Reset During DMA。
- Cancel During Completion。
- Suspend During Operation。
- Stale Mapping。
- Notification Loss。
- Buffer Reuse Attack。
- 异常恢复失败。

这一题的难点，在于没有一个单独的 CPU 指令、Kernel API 或硬件寄存器能够解决所有问题。

需要将多个子系统组织成一套具备明确状态、资源所有权和安全不变量的协议。

## **十六、本章最终总结**

这章最需要记住的不是 `interrupt_main_handler()`、`thread_state_suspend()` 或 `notif_send_async()` 等函数名称。

而是下面这几个贯穿整个 OP-TEE 的架构思想。

**第一，Interrupt 是执行权交接的问题。**

决定由谁处理事件、在哪个安全域处理，比简单知道 CPU 如何进入 Exception Vector 更重要。

**第二，Exception 是状态一致性的问题。**

无论是 Syscall、Page Fault 还是 Foreign Interrupt，异常前后的 CPU Context、Memory Context 和 Kernel State 必须始终保持合法。

**第三，Notification 是跨执行上下文协调的问题。**

Notification 不是完整的事件数据库，它只解决异步通知和唤醒。真实完成状态需要由受保护的 Kernel/Driver State 管理。

**第四，Bottom Half 是对执行上下文限制的架构性解决方案。**

它让 Secure IRQ Handler 可以保持短小，同时让需要阻塞、复杂同步或 RPC 的工作在正确的 Trusted Thread Context 中完成。

**第五，Safety 和 Liveness 必须分别设计。**

Normal World 不可信，可能拒绝协助 OP-TEE 工作。安全设计必须承认这个现实：不能保证所有服务一定及时完成，但应尽量保证在拒绝服务期间也不会破坏 Secure World 的机密性和完整性。

最后，用一句话概括 Chapter 16：

> **OP-TEE 的 Interrupt、Exception 和 Notification 子系统，本质上是一套用于协调多个安全域、多个 CPU 和多种执行上下文的状态管理机制，其核心目标是在异步事件和异常发生时，仍然保持执行上下文、资源所有权与安全隔离的一致性。**

当你能够理解、实现并验证这些约束时，才真正具备了修改 OP-TEE Kernel 异常处理与中断框架的能力。

### **本章主要源码参考**

- [Arm64 Exception Vector & Thread Assembly](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread_a64.S?utm_source=chatgpt.com)
- [Trusted Thread Context Management](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread.c?utm_source=chatgpt.com)
- [Generic Interrupt Framework](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/interrupt.c?utm_source=chatgpt.com)
- [Notification Framework](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/notif.c?utm_source=chatgpt.com)
- [Abort & Page Fault Handling](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/abort.c?utm_source=chatgpt.com)
- [OP-TEE 4.11 Core Architecture 官方文档](https://optee.readthedocs.io/en/4.11.0/architecture/core.html?utm_source=chatgpt.com)

**Chapter 16 完成。** 下一章 Chapter 17 的 Kernel Synchronization Primitives，可以直接接续本章的 IRQ Context、Lock Ordering、Notification 与跨 CPU 并发问题，深入 OP-TEE 内部 Spinlock、Mutex、Condition Variable 和 Lockdep 的真实实现。
