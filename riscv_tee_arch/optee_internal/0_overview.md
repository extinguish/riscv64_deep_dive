# **OP-TEE OS Internals — 完整架构与源码深度学习路线**

Advanced / Staff Engineer Level · 基于 OP-TEE 4.11.0

## **一、课程定位：从 OP-TEE Porting Engineer 走向 OP-TEE Core Engineer**

这次我建议我们重新设计一套完整的 **OP-TEE OS Internals & Architecture** 课程。

这套课程的目标不是让你重新熟悉 OP-TEE 的 API，也不是再次介绍 Arm TrustZone 或 RISC-V TEE 架构，而是让你真正理解：

**OP-TEE 作为一个操作系统，是如何被设计、组织、初始化、运行、管理资源、隔离程序、处理并发、提供安全服务，以及保证自身安全性的。**

最终达到这样一种能力：

> 当你在 OP-TEE OS 任意位置修改一段代码时，能够理解它在整个系统中的职责、它与其他子系统的依赖关系、它所维护的状态和安全不变量，以及修改后可能影响哪些功能，需要补充哪些测试。

这比单纯知道某个函数如何工作更重要。

### **1.1 为什么需要重新组织学习？**

结合你之前的 OP-TEE 移植经验，以及现在围绕 RISC-V、OpenSBI、SMMPT 和机密计算的研究，我认为你已经具备了相当不错的架构基础。

但是，从 **移植一个 OS** 到 **真正理解并长期维护这个 OS**，中间还有不少内容需要补齐。

例如：

| 过去的关注点             | 这次需要进一步掌握                                          |
| ------------------ | -------------------------------------------------- |
| OP-TEE 如何启动        | 所有初始化阶段、依赖关系与失败恢复策略                                |
| 如何进入 Secure World  | OP-TEE Thread 生命周期、同步与上下文管理                        |
| 如何创建 TA            | TA Loader、Session Manager、Context 与资源所有权           |
| MMU 如何配置           | 虚拟内存子系统、MOBJ、VM、Pager、内存生命周期                       |
| 如何调用 TEE API       | `libutee`、Syscall Dispatcher、内核服务实现                |
| 如何访问安全存储           | 对象模型、文件系统、加密、原子性与回滚保护                              |
| 如何实现 Crypto Driver | Crypto Framework、Key Management 与 Hardware Backend |
| 如何移植硬件驱动           | Driver Framework、DT、时钟、电源、DMA 与并发                  |
| 如何运行 xtest         | 测试组织、覆盖率、故障注入和安全回归                                 |
| 如何完成一个功能           | 如何判断跨模块影响，以及安全不变量是否被破坏                             |

这里存在一个非常关键的认知转变：

**不再按照功能 API 学习，而是按照 OS 的内部机制、状态流转、资源所有权和模块边界学习。**

### **1.2 以哪个版本为学习基线？**

我核对了上游 OP-TEE 的发布记录和源码组织。截至 2026 年 11 月，最新正式发布版本为 **OP-TEE 4.11.0（2026 年 10 月 16 日）**。上游依然保留了 `core/`、`ldelf/`、`lib/`、`ta/` 等主要代码区域，但架构内容已经覆盖 Secure Partitions、FF-A、Virtualization、Subkeys、Notifications 等不能忽略的主题。

本课程建议：

- **理论基线：** OP-TEE 4.11.0，保证学习结论相对稳定。
- **源码研究：** 以 `4.11.0` tag 为主；特定专题再检查 `master` 的后续变化。
- **架构原则：** 优先研究 Architecture-Neutral 代码，再分别考察 Arm 和 RISC-V 实现。
- **实验环境：** 首选上游支持较成熟的 Arm QEMU 环境建立功能基线，之后在 RISC-V 平台做移植对照。
- **版本演进：** 专门安排章节研究旧版到新版的接口和架构变化。

这次我不建议为了追求最新代码而直接把整个课程绑定到不断变化的 `master`。

## **二、先建立 OP-TEE 的整体架构地图**

在制定 Chapter 之前，我们先把 OP-TEE 当成一个完整的 OS 来看待。

OP-TEE End-to-End Architecture

Normal World

Client Application → libteec → Linux TEE Driver

tee-supplicant · RPC Services · REE Storage

SMC / FF-A / Platform-specific Transport

OP-TEE Secure Kernel

Entry & Execution Layer

Entry · Thread · Trap · Interrupt · RPC

TA Runtime

TA Manager · Session · Context · Loader

Kernel Services

Syscall · Storage · Crypto · Time

Memory & Isolation

MMU · VM · MOBJ · Pager · Heap

Kernel Infrastructure

Locks · DT · Drivers · PM · Debug

Architecture Abstraction

Arm / RISC-V · CPU State · MMU · Interrupts · Platform

Trusted Application Execution

User TA · PTA · Early/Embedded TA · Secure Partition

libutee · ldelf · libutils · libdl

逻辑分层图，而不是严格的调用栈或特权级图。PTA 通常在 Core 内执行；User TA 和 Secure Partition 的运行模型不同，也并不一定属于同一个执行域。

这张图说明，OP-TEE 不仅仅是一个 Secure Monitor 后面的服务程序。

它包含了完整的安全执行环境所需要的一系列内部机制：执行上下文、内存管理、程序加载、用户态与内核态隔离、系统调用、存储、加密、安全设备管理和异常处理。

但是，它又不是一个传统的通用 Linux 式 OS。例如，上游文档明确指出，经典 OP-TEE Trusted Thread 模型并不实现独立的通用线程调度器，而是与发起调用的 Normal World 线程调度关联。这是我们后续研究 Thread、RPC 和 Concurrency 时必须建立的基本认识。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



### **2.1 学习的七个层次**

我们会从七个层次逐渐深入：

1. **Architecture：** 为什么要有这个子系统，它要解决什么根本问题？
2. **Abstraction：** 它向其他模块提供什么抽象和接口？
3. **Data Structures：** 它管理什么数据对象，谁拥有这些对象？
4. **Execution Flow：** 一次真实操作如何经过这个模块？
5. **Implementation：** 具体源码、关键函数、C/Assembly 和状态变化。
6. **Invariants：** 哪些前置条件、安全规则和并发约束绝对不能破坏？
7. **Impact Analysis：** 如果修改它，哪些调用路径、其他模块和测试需要跟随改变？

最后两个层次，将是整个课程与普通源码讲解最大的区别。

# **三、正式课程大纲：10 Parts / 60 Chapters**

整个课程分为十个部分，每个 Part 代表一个相对独立的知识域。

课程总览

PART I

01–06

整体架构、工程组织与设计原则

PART II

07–12

OS Boot、初始化与平台启动

PART III

13–18

执行模型、Thread、异常与并发

PART IV

19–24

Memory Management & Isolation

PART V

25–30

TA Runtime、Loader 与 Session

PART VI

31–36

Syscall、GP API 与 Crypto

PART VII

37–42

Secure Storage 完整体系

PART VIII

43–48

Driver、Hardware 与平台服务

PART IX

49–54

FF-A、Virtualization 与安全架构

PART X

55–60

Debug、Testing、维护与架构演进

这里有意将 **Architecture-Neutral Core** 放在大部分学习内容的中心，把 Arm / RISC-V 作为两个实现后端进行对照。

下面是每个 Chapter 的具体教学大纲。

# **PART I — OP-TEE OS Architecture & Engineering Foundations**

Chapter 01–06 · 整体架构与基础设施

本部分的目标是建立完整的 OS 全景图，并弄清楚工程组织方式、模块职责以及技术选择背后的原因。

## **Chapter 01 — OP-TEE 的设计哲学与 OS 定位**

**核心问题：为什么 OP-TEE 被称为 OS？为什么它又不像 Linux、NuttX 或 FreeRTOS？**

详细内容：

- 1.1 OP-TEE 的历史背景、GlobalPlatform TEE 模型与设计目标。
- 1.2 TEE OS 和 General-Purpose OS 的区别：调度、进程、资源与执行模式。
- 1.3 Secure Kernel、User TA、PTA、Normal World 的相互关系。
- 1.4 OP-TEE 的最小 Trusted Computing Base（TCB）应该包含什么。
- 1.5 OP-TEE Kernel 的职责和 TA Runtime 的职责边界。
- 1.6 Security First 的系统设计原则：最小权限、隔离、认证和资源限制。
- 1.7 OP-TEE 在 SoC Firmware Stack 中的定位。
- 1.8 哪些功能应该放进 OP-TEE Core，哪些不应该。

**重点输出：** 能从 OS 设计者视角解释 OP-TEE 为什么选择当前的执行模型，而不是直接照搬 Linux 的调度器、进程模型和驱动框架。

## **Chapter 02 — OP-TEE 软件体系与模块边界**

**核心问题：OP-TEE 的各个组件分别负责什么，整个系统如何协作？**

详细内容：

- 2.1 `optee_os`、`optee_client`、`optee_test`、`build`、`manifest` 的职责。
- 2.2 Secure Kernel、Normal World Driver、`libteec`、`tee-supplicant`。
- 2.3 Core、PTA、User TA、Secure Partition、Standalone MM 的关系。
- 2.4 Control Plane 和 Data Plane 的划分。
- 2.5 SMC、FF-A、Shared Memory、RPC 等接口的抽象位置。
- 2.6 从 Normal World 请求到 Secure World 服务的完整路径。
- 2.7 生命周期管理、资源所有权和跨模块依赖。
- 2.8 Architecture-Specific 与 Architecture-Neutral 实现的隔离。

**实践：** 绘制 OP-TEE 全栈组件关系图，并为每个模块标明其运行权限、信任级别和输入输出接口。

## **Chapter 03 — OP-TEE OS 源码树与核心数据结构全景**

**核心问题：面对几十万行源码，如何迅速找到真正的实现位置？**

详细内容：

- 3.1 `core/kernel/`：执行管理、TA 管理、内核基础服务。
- 3.2 `core/mm/`：内存管理抽象与分配。
- 3.3 `core/tee/`：GlobalPlatform 内核服务。
- 3.4 `core/crypto/`：Cryptographic Framework。
- 3.5 `core/drivers/` 与 `core/pta/`。
- 3.6 `core/arch/arm/` 与 `core/arch/riscv/` 的实现边界。
- 3.7 `ldelf/`、`lib/libutee/`、`lib/libdl/`、`ta/`。
- 3.8 `mk/`、`scripts/`、`keys/`、`core/include/`。
- 3.9 重要头文件如何定义模块契约。
- 3.10 使用符号引用、调用图、数据结构引用图快速阅读大型 C 工程。

**实践：** 为 OP-TEE 创建 Source Map，标记主要模块、关键结构体、初始化入口和运行时入口。

## **Chapter 04 — Build System、Configuration 与 Dependency Management**

**核心问题：修改一个配置选项，为什么可能影响完全不同的运行时模块？**

详细内容：

- 4.1 OP-TEE Makefile 组织、`sub.mk`、条件编译。
- 4.2 `CFG_*` 编译配置如何决定运行时能力。
- 4.3 `PLATFORM`、`PLATFORM_FLAVOR`、编译器和架构配置。
- 4.4 Core、TA、`ldelf` 的分别构建流程。
- 4.5 静态库、共享库和 TA Development Kit 的构建。
- 4.6 C、Assembly、Linker Script、Python 构建脚本的协作。
- 4.7 依赖于架构的功能如何选择 Backend。
- 4.8 不同配置组合下的可构建性与测试矩阵。

**源码入口：** `Makefile`、`mk/`、`core/sub.mk`、`ta/mk/`。

**实践：** 修改一个 `CFG_*`，追踪其对编译对象、导出头文件、最终镜像和运行时功能的影响。

## **Chapter 05 — Binary Layout、Linker 与 OS Image**

**核心问题：OP-TEE OS 最终如何被编译成一个可以启动的安全内核镜像？**

详细内容：

- 5.1 Core ELF、TA ELF、`ldelf` ELF 的差异。
- 5.2 VMA、LMA、物理地址、虚拟地址与链接地址。
- 5.3 `.text`、`.rodata`、`.data`、`.bss`、`.nozi`。
- 5.4 Unpaged、Init、Paged 段的划分。
- 5.5 Linker Script、符号导出、Section Garbage Collection。
- 5.6 Relocation、Position Independence 与 ASLR 的关系。
- 5.7 OP-TEE Boot Image 的打包、头部及认证边界。
- 5.8 `readelf`、`objdump`、`nm`、Map File 的源码定位方法。

**实践：** 对照 ELF 与 Linker Script，解释每个主要内存段在 Boot 阶段如何被初始化。

## **Chapter 06 — 完整系统生命周期与端到端调用链**

**核心问题：如何用少量真实执行链串联起整个 OP-TEE OS？**

重点研究六条路径：

1. Boot Firmware → OP-TEE Core → 初始化完成。
2. Client → OpenSession → TA Loader → TA Entry。
3. TA → Syscall → Crypto → 返回 TA。
4. TA → Secure Storage → RPC → REE Filesystem。
5. Secure Interrupt → OP-TEE → Trusted Thread Resume。
6. CloseSession → TA Context Teardown → 资源释放。

为每条路径建立：

- Call Graph
- Context / State Transition
- Memory Ownership
- Security Boundary
- Error Handling
- Test Entry

**本部分验收标准：** 不打开源码，也能清楚描述 OP-TEE 的核心模块和六条关键路径；打开源码后，能够定位每一层的实现。

# **PART II — OP-TEE OS Boot & Initialization**

Chapter 07–12 · 内核从镜像变成运行中系统的全过程

本部分只把 Arm 和 RISC-V 作为两种不同 Boot Backend，重点研究 OP-TEE Core 自身如何初始化。

## **Chapter 07 — Boot Entry 与 Firmware Handoff**

- 7.1 OP-TEE 的 Boot Contract：上一级 Firmware 需要保证什么。
- 7.2 Core Entry Point、CPU 状态、栈和运行地址。
- 7.3 Boot 参数、共享信息、Device Tree 传递。
- 7.4 Cold Boot 与 Secondary CPU Entry。
- 7.5 Arm 与 RISC-V 在硬件执行权限和入口实现上的差异。
- 7.6 Early Exception Vector 和 Interrupt Masking。
- 7.7 Early Boot Failure 的诊断方式。

**源码方向：** `core/arch/*/kernel/`、`core/arch/*/plat-*/`。

**实践：** 从第一条汇编指令追踪到第一个架构无关 C 函数。

## **Chapter 08 — Early Runtime、Stack 与 C 环境建立**

- 8.1 Reset 后如何建立可靠的 C Execution Environment。
- 8.2 Boot Stack、Temporary Stack、Entry Stack 的区别。
- 8.3 `.bss` 清零和 `.data` 初始化。
- 8.4 Relocation 与运行时地址修正。
- 8.5 CPU 特权配置、异常向量和寄存器初始化。
- 8.6 Early Console 与 Debug 信息。
- 8.7 Boot Code 中不能使用的运行时服务及其原因。

**实践：** 分析一个 Early Boot 中的栈或内存覆盖问题，并说明为什么普通 C 调试手段可能无法使用。

## **Chapter 09 — Initcall Framework 与初始化依赖**

- 9.1 OP-TEE 初始化阶段的组织方式。
- 9.2 Initcall 注册、排序与执行。
- 9.3 内存、Console、Crypto、Driver、TA Service 的初始化依赖。
- 9.4 Early、Service、Driver、Final 等初始化层次及其实际配置。
- 9.5 初始化失败、Panic 与平台无法继续启动的条件。
- 9.6 可重入初始化与只应执行一次的初始化。
- 9.7 如何安全地增加一个新的初始化模块。

**源码方向：** `core/kernel/` 中的 Boot/Initcall Framework，以及 `core/include/initcall.h` 等接口。

**实践：** 增加一个实验性 Initcall，证明它与 Crypto、DT 和 Driver 初始化之间的先后依赖。

## **Chapter 10 — Boot-time Memory Initialization**

- 10.1 Secure Memory 的发现、保留与初始化。
- 10.2 Core Code、Core Heap、TA RAM、Shared Memory 的布局。
- 10.3 Early Page Table 与正式 Page Table。
- 10.4 Virtual Address Transition。
- 10.5 Initial Memory Pool、Allocator、Pager 初始化。
- 10.6 Memory Attribute、Cacheability、Execute Permission。
- 10.7 Early Memory Mapping 修改的安全风险。

**实践：** 建立完整 Boot Memory Map，并解释每一个映射属于什么安全域。

## **Chapter 11 — SMP Boot 与 Per-CPU Kernel State**

- 11.1 Primary CPU 与 Secondary CPU 的职责。
- 11.2 Per-CPU Variable、Per-CPU Stack、Core-local State。
- 11.3 Per-CPU Thread State 的初始化。
- 11.4 CPU Online、Suspend、Resume。
- 11.5 Global State 与 Per-CPU State 的同步。
- 11.6 SMP 初始化时序与竞态问题。
- 11.7 多核安全中断和共享资源初始化。
- 11.8 Arm PE 与 RISC-V Hart 的抽象对照。

**实践：** 为多核启动路径绘制状态机，分析两个 CPU 同时初始化共享服务时可能产生的竞态。

## **Chapter 12 — Platform Initialization 与 Device Tree Integration**

- 12.1 平台代码和通用 Core 的接口。
- 12.2 Secure DT、Non-Secure DT 的信任边界。
- 12.3 Early DT 和 Embedded DT。
- 12.4 DT Parsing、Driver Registration。
- 12.5 MMIO 地址窗口、Memory Reservation。
- 12.6 Interrupt Controller、Clock、Reset 配置。
- 12.7 平台安全参数的可信来源。
- 12.8 错误 DT 配置如何造成安全漏洞。

**实践：** 从一项 DT 属性开始，追踪到驱动初始化、MMIO Mapping 和实际硬件寄存器访问。

**本部分验收标准：** 能从 OP-TEE Entry Assembly 开始，一直分析到 Core 初始化结束，判断一个新子系统应该在哪个初始化阶段完成配置。

# **PART III — OP-TEE Kernel Execution Model & Concurrency**

Chapter 13–18 · 整套课程的第一个核心重点

这是最需要突破传统 RTOS/Linux 思维的部分。

**重点不是熟悉 Thread API，而是理解 OP-TEE 到底如何执行请求，为什么采用这种模型，以及为什么一个线程可能执行到一半就返回 Normal World。**

## **Chapter 13 — Entry Dispatch 与 Communication Protocol**

- 13.1 Fast Call 与 Yielding Call 的设计区别。
- 13.2 OP-TEE 标准消息、Command、Parameter 格式。
- 13.3 调用参数如何解析、验证和 Dispatch。
- 13.4 SMC Transport 与 FF-A Transport 的边界。
- 13.5 Entry Stack 与 Thread Stack 的切换。
- 13.6 Return Status、Origin 和错误传播。
- 13.7 非法请求与恶意 Normal World 输入的处理。

**源码方向：** `core/tee/entry_std.c`、`core/arch/*/kernel/`、相关 Message ABI 头文件；具体分发层按配置区分。

**实践：** 完整追踪一个 OpenSession 请求从 Entry 到 TA Manager 的路径。

## **Chapter 14 — Trusted Thread 的真正工作模型**

- 14.1 Trusted Thread 的概念及其与 Linux Thread 的本质区别。
- 14.2 `CFG_NUM_THREADS`、静态资源与线程池。
- 14.3 Thread Allocate、Active、Suspend、Resume、Release。
- 14.4 Thread State 与 Call Context。
- 14.5 Thread 如何与调用方 Linux Task 对应。
- 14.6 Thread Exhaustion 的处理。
- 14.7 为什么 OP-TEE 不需要 Linux 风格的通用调度器。
- 14.8 SMP 系统中的 Parallel Trusted Thread Execution。
- 14.9 Thread 生命周期与 TA Session 生命周期的区别。

**源码方向：** `core/arch/arm/kernel/thread.c`、`core/include/kernel/thread.h`、对应架构实现。

**实践：** 构造多个并发 CA 请求，观察 Trusted Thread 的分配、挂起、恢复与耗尽行为。

## **Chapter 15 — Context Management 与 Kernel/User Transition**

- 15.1 CPU Context 与 Trusted Thread Context。
- 15.2 Thread Context、TA Context、Session Context 的区别。
- 15.3 Kernel Stack、User Stack 和异常栈。
- 15.4 Secure Kernel → User TA 的权限切换。
- 15.5 User TA → Kernel Syscall 的寄存器保存与恢复。
- 15.6 Thread Suspend 时哪些状态必须保留。
- 15.7 FPU、SIMD、Extension State 的保存策略。
- 15.8 Context Switch 与 Address Space Switch 的关系。
- 15.9 错误的 Context Restoration 可能导致什么漏洞。

**实践：** 选择一个 TA Syscall，从汇编 Trap/Exception Entry 追踪到返回用户态，绘制寄存器和栈变化图。

## **Chapter 16 — Interrupt、Exception 与 Notification**

- 16.1 Secure/Foreign Interrupt 的抽象。
- 16.2 Native Interrupt 与 Exception Handling。
- 16.3 Interrupt Handler 运行时的限制。
- 16.4 Foreign Interrupt 为什么可能导致 Trusted Thread Suspend。
- 16.5 同步与异步 Notification。
- 16.6 Top Half、Bottom Half 与 Deferred Processing。
- 16.7 IRQ Context 与 Thread Context 的同步。
- 16.8 Abort、Page Fault、Panic 的不同处理。
- 16.9 中断嵌套、锁和死锁风险。

**实践：** 从 Secure Interrupt 到 Deferred Processing，再到 TA 或 Driver 工作继续执行，建立完整事件时序。

## **Chapter 17 — Kernel Synchronization Primitives**

- 17.1 Spinlock 的实现与应用边界。
- 17.2 Mutex、Condition Variable、等待和唤醒。
- 17.3 Thread Context 与 Interrupt Context 下的锁约束。
- 17.4 RPC、Notification 和阻塞机制的关系。
- 17.5 Lock Ordering、Deadlock、Lockdep。
- 17.6 Per-CPU Data 与共享数据竞争。
- 17.7 原子变量、Memory Barrier、Cache Coherency。
- 17.8 高并发下的 Session、Storage 和 Crypto 资源竞争。
- 17.9 如何进行并发安全 Code Review。

**源码方向：** `core/kernel/` 中的 Mutex、Spinlock、Condition Variable、Lockdep 实现。

**实践：** 制造一个 Lock Ordering 错误，分析其调用链及正确的锁层次设计。

## **Chapter 18 — RPC Framework 与 Normal World Service**

- 18.1 为什么 Secure Kernel 还需要 Normal World 提供服务。
- 18.2 RPC 的完整生命周期。
- 18.3 `tee-supplicant` 与 Linux Driver 如何协作。
- 18.4 RPC 时 Trusted Thread 如何 Suspend。
- 18.5 Normal World 文件访问和 TA 加载请求。
- 18.6 RPC Message Buffer、共享内存、对象引用。
- 18.7 Timeout、Cancellation、Service Failure。
- 18.8 同步 RPC 和异步 Notification 的区别。
- 18.9 Untrusted RPC Result 的验证。
- 18.10 RPC 重入、等待循环与死锁。

**实践：** 追踪一次 TA 发起的 Storage RPC，分析 Linux 侧服务阻塞时 OP-TEE 的 Thread、Lock 和 Memory 状态。

**本部分验收标准：** 能解释一个请求为什么被挂起、如何恢复、由谁调度，以及在 RPC、Interrupt 和锁竞争同时发生时可能产生哪些问题。

# **PART IV — Memory Management & Memory Isolation**

Chapter 19–24 · 整套课程的第二个核心重点

这一部分不仅要讲 MMU，还要把它扩展成 **完整的内存子系统**。

你之前已经研究过 Arm 和 RISC-V 的 Page Table，因此本部分重点会放在虚拟内存抽象、对象生命周期、不同映射之间的关系，以及隔离和资源管理。

## **Chapter 19 — OP-TEE 的内存模型与隔离原则**

- 19.1 Secure、Non-Secure、Shared Memory 的定义。
- 19.2 Core Memory、TA Memory、Device Memory。
- 19.3 Physical Memory Ownership 与 Memory Accessibility。
- 19.4 Kernel/User Address Space。
- 19.5 不同 TA 之间的地址空间隔离。
- 19.6 CPU MMU、SoC Firewall、PMP、SMMPT 等机制的职责边界。
- 19.7 Memory Lifetime、Reference Ownership 与 Zeroization。
- 19.8 不同安全域之间 Memory Sharing 的约束。

**实践：** 为一次 CA→TA 调用标出全部内存对象、各自的所有者和访问权限。

## **Chapter 20 — Core MMU Framework 与 Translation Table Management**

- 20.1 `core_mmu` 的设计目标。
- 20.2 Memory Region Registration。
- 20.3 Physical-to-Virtual 与 Virtual-to-Physical Mapping。
- 20.4 Static Mapping 与 Dynamic Mapping。
- 20.5 Kernel Translation Table 的构建与维护。
- 20.6 Memory Attribute、Access Permission、Execute Permission。
- 20.7 Page Table Allocation 与 Translation Table Lifetime。
- 20.8 TLB Invalidation、ASID 与跨核同步。
- 20.9 Arm 与 RISC-V Backend 的接口比较。

**源码方向：** `core/mm/`、`core/include/mm/`、`core/arch/*/mm/`。

**实践：** 增加一块受保护 MMIO 区域，追踪它从注册、映射到访问的完整过程。

## **Chapter 21 — User TA Virtual Memory Manager**

- 21.1 为什么 TA 需要独立虚拟地址空间。
- 21.2 TA VM Region、Memory Context、Mapping Metadata。
- 21.3 Code、RO Data、RW Data、Stack、Heap 的映射。
- 21.4 `vm_map()`、`vm_unmap()` 等相关接口与数据结构。
- 21.5 TA 加载时如何建立 Address Space。
- 21.6 TA 切换时如何激活对应的 Memory Context。
- 21.7 共享内存如何映射到 TA VA。
- 21.8 TA 销毁时如何释放虚拟内存与物理内存资源。
- 21.9 W^X、ASLR 和访问权限约束。

**源码方向：** `core/mm/vm.c`、`core/mm/` 以及 TA Loader 的内存映射接口。

**实践：** 从一次 TA Load 开始，画出其 Virtual Address Layout 和每个 VM Region 的生命周期。

## **Chapter 22 — Memory Objects（MOBJ）与 Shared Memory**

- 22.1 为什么 OP-TEE 引入 MOBJ 抽象。
- 22.2 MOBJ 与普通物理内存、VA Mapping 的区别。
- 22.3 Physically Contiguous 与 Non-Contiguous Memory。
- 22.4 Registered Shared Memory 与动态共享内存。
- 22.5 Memory Reference、Offset、Size Validation。
- 22.6 MOBJ Reference Count 与资源释放。
- 22.7 Secure Copy、Memory Ownership、Cache Attribute。
- 22.8 非可信 Linux 内存页如何被安全引用。
- 22.9 Fragmented Memory 和边界检查攻击。

**源码方向：** `core/mm/mobj.c`、`core/include/mm/mobj.h`、Message 参数处理路径。

**实践：** 构造一个跨越多个非连续物理页的 Shared Buffer，研究 MOBJ 如何描述并映射它。

## **Chapter 23 — Kernel Heap、TA Heap、Stack 与内存分配器**

- 23.1 Core Heap 与 TA Heap 为什么必须分开。
- 23.2 OP-TEE Allocator 的类型和适用场景。
- 23.3 `malloc()` 与内核专用分配接口。
- 23.4 Boot-time Allocator 和 Runtime Allocator。
- 23.5 Alignment、Fragmentation、Allocation Failure。
- 23.6 Stack Guard、Stack Overflow Detection。
- 23.7 Secure Memory Zeroization 与敏感数据生命周期。
- 23.8 Session/TA/Thread 清理中的 Memory Leak。
- 23.9 高并发下的分配器锁与性能问题。

**实践：** 研究长期重复 OpenSession/CloseSession 后内存是否完全回收，并设计内存压力测试。

## **Chapter 24 — Pager 与 Demand Paging**

- 24.1 为什么安全内核会需要 Pager。
- 24.2 Unpaged、Paged、Init Memory。
- 24.3 Page Fault → Pager → Mapping Update。
- 24.4 Pager 的 Physical Page Cache。
- 24.5 Read-Only Code Page 与 Data Page 的处理。
- 24.6 Page Eviction 与 Reclaim。
- 24.7 Page Integrity Verification。
- 24.8 Page Table Cache 与 Pager 的协作。
- 24.9 Pager 自身为什么不能依赖被换出的代码。
- 24.10 多核、TLB 和 Page Eviction 的同步问题。
- 24.11 Pager 对 Performance、Latency 和安全性的影响。

**实践：** 在支持的配置中分析一次真实 Page Fault 的完整恢复路径，并验证页表更新后不会重用陈旧映射。

**本部分验收标准：** 给出一个随机虚拟地址或 MOBJ，能够判断它属于哪个安全域、谁创建它、谁能访问它、何时释放，以及其映射变更可能影响哪些 CPU 和 TA。

# **PART V — Trusted Application Runtime & Lifecycle**

Chapter 25–30 · 整套课程的第三个核心重点

本部分把 TA 当成真正的用户态程序来研究，而不是只学习 `TA_InvokeCommandEntryPoint()` 这样的 API。

## **Chapter 25 — TA 类型、执行模型与权限边界**

- 25.1 User TA 与 PTA 的本质区别。
- 25.2 REE FS TA、Early TA、其他嵌入式 TA 来源。
- 25.3 User TA 的特权级与地址空间。
- 25.4 PTA 为什么本质上是 Kernel Service Interface。
- 25.5 TA 与 Secure Partition 的区别。
- 25.6 TA UUID、Manifest/Properties、Flags。
- 25.7 TA-to-TA 与 CA-to-TA 的调用模型。
- 25.8 PTA 和普通 User TA 应如何选择。

**实践：** 将一个简单的 PTA 功能设计成 User TA，对照二者的能力、性能和安全风险。

## **Chapter 26 — TA Binary Format、Signing 与 Loading Sources**

- 26.1 User TA 的 ELF 文件结构。
- 26.2 TA Metadata、UUID 和 Header。
- 26.3 Signed TA、Encrypted TA。
- 26.4 签名校验的 Trust Anchor。
- 26.5 Subkeys 与 TA Signing Key 的分层授权。
- 26.6 Early TA 与 REE FS TA 的加载来源。
- 26.7 TA 认证、版本及配置的安全约束。
- 26.8 为什么加载来源可信和执行代码可信不是一回事。
- 26.9 TA Image Validation Failure 的处理。

**源码方向：** `scripts/sign_encrypt.py`、`core/kernel/` 中的 TA 加载逻辑、`ldelf/`。

**实践：** 修改 TA 文件的不同区域，观察认证失败发生在什么阶段。

## **Chapter 27 —&#x20;**`ldelf`**：用户态 ELF Loader 完整实现**

- 27.1 为什么 OP-TEE 有单独的 `ldelf`。
- 27.2 Loader 的运行权限、地址空间与执行上下文。
- 27.3 ELF Program Header、Segment Mapping。
- 27.4 PIE、Relocation、GOT、PLT。
- 27.5 Symbol Resolution。
- 27.6 Dynamic Library Loading 与 `libdl`。
- 27.7 Loader 使用的专有 Syscall。
- 27.8 Loader → User TA 的控制权转移。
- 27.9 Loading Failure 和 Partial Mapping Cleanup。
- 27.10 ASLR 对 Loader 的影响。

**源码方向：** `ldelf/`、`lib/libdl/`、`core/kernel/user_ta.c`。

**实践：** 从一个 `.ta` 文件加载开始，追踪到 TA 第一条用户态指令，覆盖 ELF Segment、Relocation、页表和寄存器。

## **Chapter 28 — TA Context Creation、Execution 与 Destruction**

- 28.1 TA Context 的创建条件。
- 28.2 `user_ta_ctx` 与通用 TA Context。
- 28.3 TA 初始化时的内存及资源分配。
- 28.4 TA Entry Point、Parameter、User Stack。
- 28.5 TA Syscall 和返回内核的完整路径。
- 28.6 TA Panic、异常终止与清理。
- 28.7 TA Unload 的条件。
- 28.8 TA Code/Data 与 Session State 的生命周期差异。
- 28.9 资源释放的顺序及异常路径。

**实践：** 在 TA Create、Invoke、Panic、Destroy 的关键位置跟踪所有内核对象状态。

## **Chapter 29 — TA Session Manager 与并发调用语义**

- 29.1 Session 和 TA Context 的区别。
- 29.2 `tee_ta_session` 与 Session Ownership。
- 29.3 OpenSession、InvokeCommand、CloseSession。
- 29.4 `TA_FLAG_SINGLE_INSTANCE`。
- 29.5 `TA_FLAG_MULTI_SESSION`。
- 29.6 `TA_FLAG_INSTANCE_KEEP_ALIVE`。
- 29.7 多客户端并发访问一个 TA。
- 29.8 TA Busy、Session Reference Count。
- 29.9 Session Cancellation、Timeout。
- 29.10 Session Deadlock、Race Condition、Panic Cleanup。

**核心源码：** `core/kernel/tee_ta_manager.c`。

**实践：** 对不同 TA Flags 的组合进行并发测试，解释为什么某些 Session 可以共享 TA Context，而另一些不能。

## **Chapter 30 — TA-to-TA、PTA Dispatch 与复杂调用链**

- 30.1 User TA 之间的 Client/Service 关系。
- 30.2 PTA Registration 与 UUID Dispatch。
- 30.3 User TA 调用 PTA 时的身份与权限。
- 30.4 TA 嵌套调用与 Session Stack。
- 30.5 Client Identity、Login Method、Authorization。
- 30.6 同一个 Trusted Thread 内的嵌套执行。
- 30.7 跨 TA 传递 Memory Reference。
- 30.8 循环调用、死锁与资源所有权。
- 30.9 TA Panic 对上层调用者的影响。
- 30.10 如何设计安全的 PTA Service ABI。

**实践：** 构造 CA → TA-A → TA-B → PTA 的调用链，逐层跟踪 Session、Context、Identity 与 Memory Reference。

**本部分验收标准：** 任意给定一个 TA 调用场景，能够解释 TA 是否需要加载、是否会创建新实例、是否复用现有 Session、在哪个地址空间执行，以及失败后哪些资源需要清理。

# **PART VI — System Call、GlobalPlatform API & Cryptographic Framework**

Chapter 31–36 · 从用户态 API 一直深入到安全内核实现

本部分的关键是把“一个 API 调用”转化成完整的实现链路。既要理解接口语义，也要了解如何验证参数、管理 Handle、分配对象并调用底层 Backend。

## **Chapter 31 — OP-TEE Syscall Architecture**

- 31.1 为什么 User TA 需要 Syscall。
- 31.2 `utee_*` Syscall ABI。
- 31.3 User Mode → Kernel Mode 的调用机制。
- 31.4 Architecture-Specific Syscall Entry。
- 31.5 Syscall Number、Dispatch Table 和 Handler。
- 31.6 用户指针检查、Memory Access Validation。
- 31.7 Kernel 如何为 TA Syscall 建立正确的调用上下文。
- 31.8 Handle、Resource Ownership 和权限检查。
- 31.9 Syscall 返回值、异常、Panic 与错误码。
- 31.10 新增一个 Syscall 对 TA ABI 和兼容性的影响。

**源码方向：** `core/tee/tee_svc.c`、`core/arch/*/` 中的 Syscall Entry、`lib/libutee/`。

**实践：** 选择一个简单的 `TEE_*` API，从 TA C 函数深入到 Syscall Assembly 和 Kernel Handler。

## **Chapter 32 —&#x20;**`libutee`**&#x20;与 GlobalPlatform Internal Core API**

- 32.1 GlobalPlatform TEE Internal Core API 的设计。
- 32.2 `libutee` 在 TA 用户态的职责。
- 32.3 TEE Entry Point、User TA Runtime Startup。
- 32.4 Memory API、Property API、Time API。
- 32.5 Transient Object、Persistent Object API。
- 32.6 Cryptographic Operation API。
- 32.7 Session、Cancellation API。
- 32.8 `libutee` API 和 Kernel Syscall 的映射。
- 32.9 API Compatibility、扩展和版本管理。
- 32.10 如何新增安全、可维护的用户态服务接口。

**实践：** 制作一张 `TEE_* API → libutee → utee syscall → Kernel Service` 对照表。

## **Chapter 33 — Crypto Framework Architecture**

- 33.1 为什么需要独立的 Crypto Framework。
- 33.2 GlobalPlatform Crypto Operation 与底层 Crypto Primitive 的分层。
- 33.3 Crypto Operation Context 的创建与销毁。
- 33.4 Hash、Cipher、MAC、AEAD 的操作模型。
- 33.5 Asymmetric Crypto、Signature、Key Agreement。
- 33.6 Key Type、Key Object、Operation State。
- 33.7 `crypto_*()` Interface 与 Backend。
- 33.8 软件实现和硬件加速之间的选择。
- 33.9 Sensitive Buffer 的安全管理。
- 33.10 错误处理、状态重置与资源释放。

**源码方向：** `core/crypto/`、`core/tee/tee_svc_cryp.c`、`core/include/crypto/`。

**实践：** 从 `TEE_DigestDoFinal()` 追踪到具体软件或硬件 Hash 实现。

## **Chapter 34 — Software Crypto Backend 与算法实现**

- 34.1 OP-TEE 如何集成 Mbed TLS、LibTomCrypt 等实现。
- 34.2 Crypto Abstraction 和算法实现库之间的职责。
- 34.3 AES、SHA、RSA、ECC 的典型调用路径。
- 34.4 Symmetric/Asymmetric Operation Context。
- 34.5 Scatter-Gather Buffer、Alignment 与数据长度。
- 34.6 Constant-Time、Side-Channel 和临时密钥数据。
- 34.7 Crypto 库版本升级造成的 ABI、配置和安全影响。
- 34.8 Crypto Known Answer Test 与 Regression Test。

**实践：** 选择 AES-GCM 或 ECDSA，追踪从 User TA API 到具体算法实现的完整调用路径。

## **Chapter 35 — Hardware Crypto Acceleration 与 Key Management**

- 35.1 Hardware Crypto Driver 的核心接口。
- 35.2 Crypto Framework 如何选择硬件 Backend。
- 35.3 Hardware Key Slot、Key Ladder、不可导出密钥。
- 35.4 Secure DMA Buffer 与 Cache Management。
- 35.5 DMA Completion、Interrupt、Timeout。
- 35.6 Crypto Driver 与 OP-TEE Thread/Lock 的相互作用。
- 35.7 Hardware Accelerator Reset、Power Management。
- 35.8 Hardware Fault、Fallback 和安全降级策略。
- 35.9 Driver 修改对 Crypto、Storage 和 TA 的影响。

**实践：** 设计一个 SHA/AES Hardware Backend 的调用和错误处理流程，包括并发、超时与 DMA 安全校验。

## **Chapter 36 — RNG、Secure Time、OTP 与其他安全基础服务**

- 36.1 TRNG、Entropy Source、DRBG。
- 36.2 RNG 初始化与安全健康检查。
- 36.3 HUK、OTP、Device Unique Identity。
- 36.4 Secure Time 与 REE Time 的信任区别。
- 36.5 Monotonic Counter、Anti-Replay 相关能力。
- 36.6 密钥生命周期与安全擦除。
- 36.7 平台 Secure Services 对 Storage 和 Crypto 的依赖。
- 36.8 不具备某项硬件安全能力时的行为与安全限制。
- 36.9 Platform-Specific Security Service 的接口设计。

**实践：** 分析 HUK 或 RNG 不可用时，哪些上层服务必须失败，哪些功能可以安全地采用其他 Backend。

**本部分验收标准：** 能够从一个用户态 TEE API 出发，准确识别 ABI、Kernel Handler、Crypto/Platform Backend、资源生命周期和潜在安全边界。

# **PART VII — Secure Storage Architecture & Data Integrity**

Chapter 37–42 · 一套完整的小型安全存储系统

这部分建议按照真正的文件系统学习，而不是停留在“通过 RPC 把加密文件写入 Linux”。

OP-TEE 的 Secure Storage 不仅涉及加密，还涉及持久化对象、文件元数据、密钥派生、完整性树、崩溃一致性和 Replay Protection。官方文档也明确区分 REE FS 与 RPMB 两种 Backend，并规定了持久化操作的原子性语义。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



## **Chapter 37 — Trusted Storage Object Model**

- 37.1 GP Persistent Object 与 Transient Object。
- 37.2 Object Handle、Object ID、属性和访问权限。
- 37.3 Object Create、Open、Read、Write、Rename、Delete。
- 37.4 Storage Syscall 与内部文件系统接口。
- 37.5 Per-TA Object Namespace 与隔离。
- 37.6 Object Enumeration 和 Handle Lifetime。
- 37.7 Concurrent Access 与对象锁。
- 37.8 Error Handling 与 Resource Cleanup。

**源码方向：** `core/tee/tee_svc_storage.c`、`core/tee/tee_obj.c` 等。

**实践：** 追踪一个 Persistent Object 从创建到删除的所有内核对象。

## **Chapter 38 — REE FS Secure Storage**

- 38.1 为什么能够在不可信 Linux Filesystem 上保存安全数据。
- 38.2 OP-TEE Core 与 `tee-supplicant` 的分工。
- 38.3 REE FS 文件格式、Metadata、目录组织。
- 38.4 File Operation Interface。
- 38.5 数据加密与认证。
- 38.6 Integrity Hash Tree。
- 38.7 文件扩展、随机读写、目录更新。
- 38.8 RPC Failure 和文件系统异常。
- 38.9 为什么加密与完整性认证并不自动提供 Rollback Protection。

**源码方向：** `core/tee/tee_ree_fs.c`、`core/tee/fs_htree.c`、`tee-supplicant` 相关文件系统服务。

**实践：** 修改 REE 文件的密文和元数据，区分完整性失败与历史状态重放。

## **Chapter 39 — RPMB Secure Storage**

- 39.1 RPMB 的安全硬件模型。
- 39.2 RPMB Authentication Key。
- 39.3 Write Counter 与 Replay Protection。
- 39.4 OP-TEE RPMB Filesystem Layout。
- 39.5 FAT、Data Area、Metadata。
- 39.6 RPMB Read/Write Command。
- 39.7 可靠写入与原子更新。
- 39.8 Secure World、Linux MMC Driver、硬件之间的协作。
- 39.9 RPMB 并发访问、错误恢复和密钥配置。
- 39.10 没有 RPMB 的平台如何选择合适的安全存储设计。

**源码方向：** `core/tee/tee_rpmb_fs.c`、`core/tee/tee_fs_key_manager.c`。

**实践：** 设计一次 RPMB Write 中途掉电的测试，说明哪个状态是权威状态，如何判断更新成功。

## **Chapter 40 — Storage Key Hierarchy、Hash Tree 与完整性验证**

- 40.1 Hardware Unique Key → Storage Key 的密钥链。
- 40.2 SSK、TSK、FEK 及其派生关系。
- 40.3 每个 TA 为什么需要独立的安全存储密钥域。
- 40.4 文件内容的加密与解密。
- 40.5 Authentication Tag、Hash Tree。
- 40.6 Metadata Integrity。
- 40.7 Root Hash 的可信存储和验证。
- 40.8 HUK 变更、设备迁移和数据可用性。
- 40.9 密钥算法、格式升级的兼容性。
- 40.10 Key Leakage、Key Reuse、IV/Nonce Management。

**实践：** 从一个新文件创建开始，逐层跟踪 Storage Key 到实际 Data Encryption Key 的使用。

## **Chapter 41 — Atomicity、Crash Consistency 与 Rollback Security**

- 41.1 什么叫做原子更新。
- 41.2 Out-of-Place Update 的原理。
- 41.3 Write/Truncate/Rename/Delete 的崩溃一致性。
- 41.4 Metadata 与 Data 的写入顺序。
- 41.5 中途断电的恢复逻辑。
- 41.6 完整性保护和 Anti-Rollback 的本质区别。
- 41.7 RPMB Counter 与 REE FS 的协同。
- 41.8 TOCTOU、File Replacement、Replay Attack。
- 41.9 双 Backend 共存时的数据一致性。
- 41.10 如何设计故障注入与安全回归测试。

**实践：** 在文件更新的不同阶段注入失败，验证旧状态或新状态是否保持一致。

## **Chapter 42 — Storage Lifecycle、Recovery 与 Migration**

- 42.1 Persistent Object 的生命周期。
- 42.2 TA 升级与旧数据兼容。
- 42.3 文件格式变更与迁移。
- 42.4 Storage Full、Out-of-Memory 和 Wear。
- 42.5 多线程同时修改安全对象。
- 42.6 REE 文件丢失、损坏、回滚的处理策略。
- 42.7 Reset、Factory Provisioning、Key Rotation。
- 42.8 安全存储性能分析。
- 42.9 如何编写 Storage Regression Test。
- 42.10 如何评审一次涉及存储格式的 OP-TEE Patch。

**实践：** 设计一个旧版本与新版本之间的 Storage Compatibility Test Suite。

**本部分验收标准：** 能够判断一个存储修改是否破坏 Confidentiality、Integrity、Atomicity、Rollback Resistance 或旧版本数据的兼容性。

# **PART VIII — Hardware Drivers & Platform Services**

Chapter 43–48 · 让内核安全地管理 SoC 资源

这部分会与你目前的 SoC 研发工作有很强的关联，但学习重点仍然是 OP-TEE 的通用软件设计，而非重复某种架构的硬件寄存器规范。

## **Chapter 43 — OP-TEE Driver Framework 与 Device Tree**

- 43.1 OP-TEE Driver Model 与 Linux Driver Model 的差异。
- 43.2 Driver Initialization 与 Registration。
- 43.3 DT Matching、Probe、Provider/Consumer。
- 43.4 MMIO Mapping 与 Memory Attribute。
- 43.5 Secure/Non-Secure Device Ownership。
- 43.6 Device Resource 的生命周期。
- 43.7 Driver Dependencies 和初始化顺序。
- 43.8 DT 中不可信配置与安全资源配置的边界。
- 43.9 如何编写符合上游风格的设备驱动。

**实践：** 实现一个简化的 Secure MMIO Driver，完成 DT Parsing、Mapping、初始化与访问控制。

## **Chapter 44 — Interrupt、Clock、Reset 与同步设备管理**

- 44.1 Secure IRQ Registration。
- 44.2 Interrupt Top Half 与 Deferred Processing。
- 44.3 Clock Provider 与 Clock Consumer。
- 44.4 Reset Controller。
- 44.5 Secure Timer、Watchdog。
- 44.6 Driver Context 与 Kernel Thread Context。
- 44.7 Driver Locking 和 Atomic Context。
- 44.8 Hardware Timeout、Error Recovery。
- 44.9 多个 TA 竞争硬件服务时的资源协调。
- 44.10 Secure Driver 对 Normal World 的依赖风险。

**实践：** 设计一个需要 Secure Interrupt 的硬件服务，处理并发、超时和设备复位。

## **Chapter 45 — DMA、Cache Coherency、IOMMU 与系统级内存保护**

- 45.1 为什么 CPU MMU 不能独立完成 DMA 隔离。
- 45.2 Secure DMA Buffer Allocation。
- 45.3 Buffer Ownership、Cache Maintenance。
- 45.4 Device Physical Address、CPU PA、IOVA 的区别。
- 45.5 DMA Mapping 与 Unmapping 的生命周期。
- 45.6 Non-Coherent DMA 的同步策略。
- 45.7 IOMMU、PMP、SMMPT 等系统级保护机制的角色。
- 45.8 Secure DMA 与 Normal World Shared Memory 的交互。
- 45.9 修改 Memory Permission 后设备端缓存/转换状态的同步。
- 45.10 TOCTOU、DMA Attack、非法设备访问。

**实践：** 设计一套安全 DMA Buffer Lifecycle，并验证 CPU、Device、Normal World 之间的权限转换和同步顺序。

这一章会成为后续将 RISC-V SMMPT 与 OP-TEE 集成时非常重要的基础。

## **Chapter 46 — Power Management、Suspend/Resume 与 System Services**

- 46.1 OP-TEE 在 CPU Power Management 中的职责。
- 46.2 PSCI、SBI 等平台服务与 OP-TEE 的职责边界。
- 46.3 System Suspend、CPU Suspend、CPU Off。
- 46.4 Suspend 前的 Thread、Driver、Crypto 状态。
- 46.5 Resume 时的 MMU、Cache、Interrupt Controller。
- 46.6 Secure Hardware State 的保存与恢复。
- 46.7 多核 Power State Coordination。
- 46.8 Cold Reset 与 Warm Reset 的差异。
- 46.9 Suspend/Resume Failure 和 Recovery。
- 46.10 休眠过程中安全密钥、计数器及存储状态的处理。

**实践：** 从一次 CPU/System Suspend 请求开始，分析 OP-TEE、Firmware、Driver 各自的处理职责。

## **Chapter 47 — Architecture Abstraction Layer**

- 47.1 OP-TEE 的 Architecture-Neutral Design。
- 47.2 Core 与 Architecture Backend 的接口。
- 47.3 Trap、Thread、MMU、Interrupt 的架构相关实现。
- 47.4 Memory Barrier、Atomic Operation。
- 47.5 Cache、TLB、User Access Helpers。
- 47.6 Platform-Dependent vs Architecture-Dependent 代码。
- 47.7 Arm 与 RISC-V 的实现映射。
- 47.8 如何将一项新功能拆成通用 Core 与 Backend。
- 47.9 如何避免架构代码反向污染通用核心。

**实践：** 选择一段现有的 Arm-specific 实现，设计其 Architecture-Neutral Interface 和 RISC-V Backend。

## **Chapter 48 — OP-TEE 与完整 Firmware / Linux 系统的集成**

- 48.1 OP-TEE 与 TF-A、OpenSBI、Bootloader 的接口。
- 48.2 Linux OP-TEE Driver 的 Discovery 与 Probe。
- 48.3 Linux Shared Memory Registration。
- 48.4 `libteec`、`tee-supplicant` 的初始化。
- 48.5 TA Binary Delivery 与 Secure Storage RPC。
- 48.6 Core、Client、Linux Driver 的 ABI 兼容。
- 48.7 Firmware 更新后的 Boot/Runtime 兼容性。
- 48.8 Buildroot、Linux Rootfs、TA 安装位置。
- 48.9 QEMU、仿真器和真实 SoC 的集成差异。
- 48.10 全栈 Debug 与问题责任边界划分。

**实践：** 从 Linux Driver Probe 一直追踪到第一个 TA Invoke 成功，完成 Cross-Repositories Integration Map。

**本部分验收标准：** 面对一个新的 SoC 硬件资源，能够判断它应该由哪一层管理、如何暴露给 TA、如何进行隔离，以及 Suspend、SMP、DMA 和异常恢复需要考虑哪些问题。

# **PART IX — Advanced Architecture, Virtualization & Security**

Chapter 49–54 · 理解现代 OP-TEE 的扩展能力与安全边界

这一部分是为了避免课程仅停留在传统的 GlobalPlatform TEE 模型。

现代 OP-TEE 已经覆盖 FF-A、SPMC、Secure Partition，以及虚拟化等能力。其中，OP-TEE 作为 S-EL1 SPMC 时，不仅要处理传统 GP TEE 服务，还需要管理 Secure Partition 生命周期、Endpoint 通信和内存共享。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



## **Chapter 49 — FF-A Transport 与 SPMC Architecture**

- 49.1 为什么传统 SMC 通信模型需要扩展。
- 49.2 FF-A Endpoint、Message、Memory Transaction。
- 49.3 SPMD、SPMC、Secure Partition 的职责。
- 49.4 OP-TEE 作为 SPMC 的执行模型。
- 49.5 FF-A Message Routing。
- 49.6 Direct Messaging 与 Indirect Messaging。
- 49.7 Memory Share、Lend、Donate、Retrieve、Relinquish。
- 49.8 FF-A 与传统 GP Session 模型的集成。
- 49.9 FF-A Transport 对 Thread、Shared Memory、RPC 的影响。
- 49.10 不同 FF-A 组件之间的版本和 ABI 兼容。

**实践：** 从一个 FF-A Direct Message 进入 OP-TEE 开始，分析 Message Dispatch、Endpoint Context 和返回路径。

## **Chapter 50 — Secure Partition Runtime & Lifecycle**

- 50.1 Secure Partition 和 User TA 的本质区别。
- 50.2 SP Manifest 与资源描述。
- 50.3 Secure Partition Image Loading。
- 50.4 SP Initialization、Execution、Context Saving。
- 50.5 SP Memory Mapping 和隔离。
- 50.6 Endpoint-to-Endpoint Communication。
- 50.7 SP 与 OP-TEE Core 的共享服务。
- 50.8 Boot-time SP 与 Runtime User TA 的生命周期区别。
- 50.9 Secure Partition Fault Handling。
- 50.10 Secure Partition 的测试和安全边界。

**实践：** 比较同一个安全服务作为 User TA 和 Secure Partition 时的完整运行模型。

## **Chapter 51 — OP-TEE Virtualization Architecture**

- 51.1 为什么多个虚拟机可能需要共享 TEE 服务。
- 51.2 Hypervisor 与 OP-TEE 的职责划分。
- 51.3 VM Identity、VM Context、TEE Mediation。
- 51.4 VM 之间的 TA Context 隔离。
- 51.5 Guest Shared Memory 的地址转换。
- 51.6 VM-specific Resource Ownership。
- 51.7 VM Creation、Destroy、Context Cleanup。
- 51.8 Guest Crash 和异常清理。
- 51.9 Cross-VM Isolation 和 Confused Deputy。
- 51.10 Virtualization 与 SPMC/FF-A 的关系。

**实践：** 设计两个 Guest VM 同时访问 OP-TEE 时的 Session、Memory、Identity 隔离模型。

这里需要区分上游已经支持的实验性虚拟化能力与一般的 TEE 虚拟化架构设计，不能默认每个架构 Backend 都具备同样功能。

## **Chapter 52 — Standalone MM 与 Firmware Security Services**

- 52.1 UEFI Standalone MM 的设计目标。
- 52.2 OP-TEE 支持 Standalone MM 的运行方式。
- 52.3 Standalone MM 与 User TA、Secure Partition 的异同。
- 52.4 Variable Service、Protected Storage。
- 52.5 MM Communication Buffer。
- 52.6 UEFI Variable Authentication。
- 52.7 Firmware Service 与 TEE Service 的资源复用。
- 52.8 使用 FF-A 或其他 Transport 的不同实现。
- 52.9 与 RISC-V SBI MPXY/RPMI 的可能集成架构。
- 52.10 Firmware Update 与 Persistent Variable 安全。

**实践：** 分析 `SetVariable()` 到 Protected Storage 的完整请求生命周期，并明确哪些环节属于已实现的软件路径、哪些属于平台适配设计。

## **Chapter 53 — OP-TEE Security Architecture & Hardening**

- 53.1 OP-TEE 的完整 Trust Model。
- 53.2 Normal World 与 User TA 的不同威胁能力。
- 53.3 Kernel Attack Surface、Syscall Attack Surface。
- 53.4 PTA Privilege 和 Identity Spoofing。
- 53.5 Memory Corruption、Use-After-Free、Double Free。
- 53.6 ASLR、Stack Canary、NX/W^X。
- 53.7 BTI 等架构相关控制流保护。
- 53.8 Pointer Validation 与 TOCTOU。
- 53.9 Side-Channel、Timing、Cache 与 Spectre 类风险。
- 53.10 Sensitive Data Lifetime、Zeroization。
- 53.11 Secure Boot 和 TA Signing 的边界。
- 53.12 Panic、Crash Dump、Debug Interface 的信息泄露风险。

**实践：** 对一个复杂 Syscall Handler 进行安全 Code Review，从输入验证、资源管理、并发和权限四个方向分析潜在风险。

## **Chapter 54 — OP-TEE 安全服务扩展与生态集成**

- 54.1 PKCS#11 TA 与密码学服务。
- 54.2 Trusted Keys、Secure Key Service。
- 54.3 KeyMint/Keymaster、AVB 等集成场景。
- 54.4 Secure Storage 与 Device Provisioning。
- 54.5 Rust TA 与用户态 SDK 的接口关系。
- 54.6 Attestation、Measured Boot 与 OP-TEE 的潜在集成点。
- 54.7 机密计算与传统 TEE 的边界。
- 54.8 CoVE/RISC-V Confidential Computing 与 OP-TEE 的架构共存。
- 54.9 新增安全服务应该采用 PTA、User TA 还是独立安全组件。
- 54.10 功能扩展对 TCB 和可信启动链的影响。

**实践：** 选择一个新的安全服务需求，编写 Architecture Decision Record，解释其权限、接口、存储、密钥和升级策略。

需要强调：这一章研究的是 OP-TEE 已有的生态集成和可以设计的扩展边界，不意味着 OP-TEE Core 自身已经实现所有 Attestation 或机密计算协议。

**本部分验收标准：** 能够在传统 GP TEE 之外，理解 OP-TEE 如何适应现代 Firmware、安全分区和虚拟化系统，并识别新功能带来的 TCB 扩张与攻击面。

# **PART X — Debugging, Testing, Evolution & Maintainer Engineering**

Chapter 55–60 · 从理解源码到有能力维护源码

这是整套课程最终要落地的部分。

前九个 Part 让你理解 OP-TEE 是怎样工作的。这一部分训练你如何在真实项目中修改它、验证它、优化它，并向社区提交能够接受审查的 Patch。

## **Chapter 55 — OP-TEE Debugging & Observability**

- 55.1 OP-TEE Debug Configuration。
- 55.2 `DMSG`、`IMSG`、`EMSG` 等日志机制。
- 55.3 Panic、Abort、Exception Dump。
- 55.4 Call Stack、Symbolization、Stack Unwinding。
- 55.5 Core ELF 与 TA ELF 的符号定位。
- 55.6 Ftrace、Gprof。
- 55.7 QEMU/GDB 跨 Secure Kernel、User TA 的调试。
- 55.8 SMP Debug 与 Per-CPU Context。
- 55.9 RPC Hang、Deadlock、Pager Fault 的定位。
- 55.10 非侵入式 Debug 与日志对时序的影响。
- 55.11 仿真环境与真实硬件调试的差异。

**实践：** 分别诊断一次 TA Crash、Secure Kernel Abort 和 Trusted Thread Deadlock。

## **Chapter 56 — OP-TEE Test Architecture 与 xtest**

- 56.1 `optee_test` 的项目组织。
- 56.2 CA、User TA、PTA、Core Test Code 的测试分工。
- 56.3 xtest Case Number 与实际目标代码的映射。
- 56.4 GP API Compliance Tests。
- 56.5 Regression Tests。
- 56.6 Concurrent Session Tests。
- 56.7 Shared Memory、Pager、Storage Tests。
- 56.8 Crypto Known Answer Tests。
- 56.9 Fault Injection 与 Negative Testing。
- 56.10 如何为一个 Core Patch 添加测试。
- 56.11 如何验证测试覆盖了真正修改的代码路径。
- 56.12 如何构建自动化 Regression Suite。

**实践：** 修改一个 Kernel Service，在 xtest 中增加对应的正向、异常参数和并发测试。

## **Chapter 57 — Build Validation、Hardening Tools 与性能分析**

- 57.1 Debug/Release Configuration 差异。
- 57.2 Compiler Warnings 和 Static Analysis。
- 57.3 ASan、UBSan 等工具在支持配置中的应用。
- 57.4 Stack Usage、Memory Consumption。
- 57.5 Code Size 和 Linker Map 分析。
- 57.6 Boot Time、TA Loading Latency。
- 57.7 Syscall、RPC、Storage、Crypto Performance。
- 57.8 Lock Contention、Thread Exhaustion。
- 57.9 Benchmark 设计与实验干扰。
- 57.10 Security Hardening 和 Performance 的权衡。

**实践：** 优化一个真实的 Hot Path，提供修改前后的性能数据与正确性证明。

## **Chapter 58 — OP-TEE 源码演进、兼容性与社区开发流程**

- 58.1 OP-TEE Release、Tag、Branch 的管理。
- 58.2 从旧版本到 4.11.0 的源码演进研究方法。
- 58.3 API、ABI、Binary Format、Storage Format 的兼容性。
- 58.4 上游代码风格和模块划分。
- 58.5 `git blame`、`git log`、`git range-diff`。
- 58.6 如何从 Commit History 理解设计决策。
- 58.7 GitHub Issue、Pull Request、Maintainer Review。
- 58.8 Patch Scope、Commit Message、测试说明。
- 58.9 如何进行跨架构 Review。
- 58.10 Security Fix 与普通 Bug Fix 的区别。
- 58.11 如何保持长期维护分支与上游同步。

**实践：** 选择一个真实上游 Patch，重建其设计动机、修改范围、兼容性风险和验证策略。

## **Chapter 59 — Change Impact Analysis：修改影响范围评估方法**

这是我建议你特别深入的一章，也是全课程最终能力的核心。

- 59.1 如何从一个 Function Change 开始分析影响。
- 59.2 Call Graph 与 Reverse Call Graph。
- 59.3 Data Structure Usage Analysis。
- 59.4 State Ownership 与 Lifetime Analysis。
- 59.5 Initialization Dependency Analysis。
- 59.6 Concurrency and Locking Impact。
- 59.7 Privilege Boundary 和 Memory Ownership Impact。
- 59.8 Error Handling 与 Cleanup Path。
- 59.9 ABI、Build Config、Architecture Backend 影响。
- 59.10 Test Coverage 与 Regression Risk。
- 59.11 如何编写 Change Impact Report。
- 59.12 如何在 Code Review 前证明修改的安全性。

**实践：** 任意选取一个 OP-TEE Core Patch，不运行代码，仅通过源码分析构建完整影响范围图，再用实验验证判断是否正确。

## **Chapter 60 — 完整实战：设计、实现与上游维护一个 OP-TEE Core Feature**

最终毕业项目。

要求选择一个真实需求，例如：

- 增加一个安全服务接口。
- 优化 TA Runtime 的某个资源管理路径。
- 修复 Shared Memory 或 MOBJ 的边界问题。
- 改进 Secure Storage 的错误处理。
- 为 RISC-V 实现一个 Architecture Backend。
- 为新硬件增加可靠的 OP-TEE Driver。

完整实施过程必须覆盖：

1. Requirement & Threat Model。
2. Architecture Design。
3. Interface & Data Structure Design。
4. Execution and State Transition。
5. Memory & Concurrency Analysis。
6. Implementation。
7. Unit/Integration/Regression Tests。
8. Cross-Architecture Impact Analysis。
9. Documentation。
10. Patch Review and Upstream Submission Preparation。

**最终成果：** 一份符合真实开源社区评审要求的 OP-TEE Core 设计与实现方案。

# **四、每个 Chapter 应该采用什么教学方式？**

这次我建议不要采用传统的“先讲概念，然后罗列重要函数”的方式。

每个 Chapter 都采用统一的八层讲解框架。

1. **Why — 设计动机与历史背景**

   这个模块解决了什么问题？引入它之前，系统面临什么限制？
2. **What — 架构、抽象与接口**

   它有哪些职责？与其他模块如何交互？为什么选择这种设计？
3. **Data Structures — 内部状态与资源所有权**

   深入重要结构体、字段、状态转换、Reference Count 与 Lifetime。
4. **How — 关键执行流程与源码分析**

   沿着真实调用链阅读 C、Assembly、Syscall、Page Table 和 Driver 代码。
5. **Failure & Security — 异常路径和安全不变量**

   分析 OOM、Timeout、Crash、Concurrency、Malicious Input 等场景。
6. **Hands-on — 可重复的实验**

   通过日志、GDB、源码修改和 xtest 证明理解，而不只停留在阅读。
7. **Impact Analysis — 修改后的影响范围**

   分析直接调用方、间接依赖、跨模块状态及回归测试要求。
8. **Staff Engineer Assessment — 综合考察**

   每章安排约 10 道工业场景级别的问题，并给出参考答案和分析要点。

### **举个具体例子：Chapter 29 — TA Session Manager**

假设我们正在研究：

```
tee_ta_open_session()
```

普通教程可能会讲解函数参数、函数调用和 Session 的建立过程。

但我们这套课程会进一步回答：

- 为什么 Session 和 TA Context 必须使用不同的数据结构？
- 一个 TA Context 可以被多少个 Session 共享？
- 当两个 CPU 同时打开同一个 Single Instance TA 时，如何避免重复创建？
- 什么时候需要持有 `tee_ta_mutex`？为什么不能一直持有？
- Session 的 Reference Count 和 TA Context 的 Reference Count 分别负责什么？
- TA 执行过程中发生 Panic，应由谁释放 Session 和 Context？
- 如果在已有锁保护下执行 TA 的 Close Entry Point，会发生什么？
- 修改 `tee_ta_close_session()` 是否可能影响 PTA、Storage、Crypto 或内部嵌套调用？
- 应该编写哪些并发和异常退出测试？

更进一步，上游 `tee_ta_manager.c` 中已经包含处理 TA Busy、Close Session、Panic Cleanup 和锁相关死锁风险的逻辑，因此我们可以直接用真实代码，而不是虚构一个简化模型。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



我希望每个 Chapter 最后都能形成类似的深度。

# **五、整个课程最重要的跨模块分析方法**

为了实现你提出的目标——“修改某处代码后能够迅速判断影响范围”，我建议每个 Chapter 都必须维护一张 **Dependency & Impact Matrix**。

例如：

| 修改位置               | 直接影响                    | 可能的跨模块影响                          | 关键回归验证                     |
| ------------------ | ----------------------- | --------------------------------- | -------------------------- |
| `tee_ta_manager.c` | Session、TA Context      | Identity、PTA、嵌套 TA 调用、锁           | xtest Session/Concurrency  |
| `mobj.c`           | Memory Object、Reference | Shared Memory、VM、RPC、DMA          | SHM、越界、并发、释放               |
| `vm.c`             | TA Address Space        | `ldelf`、Pager、Syscall User Access | TA Load、Page Fault、Memory  |
| `thread.c`         | Trusted Thread          | RPC、Interrupt、Locking             | SMP、RPC、Thread Exhaustion  |
| `tee_svc_cryp.c`   | Crypto Syscall          | Key Object、Backend、User TA        | Crypto Tests、错误路径          |
| `tee_ree_fs.c`     | REE Storage             | RPC、Key Manager、Hash Tree         | Storage、Power Failure      |
| `ldelf/`           | ELF Loading             | TA VM、ASLR、Dynamic Library        | Load、Relocation、TA Startup |
| Driver Framework   | Secure Hardware         | DT、MMU、PM、Interrupt、DMA           | Boot、Driver、Suspend        |

这张表只是起点。实际分析时，还要考虑版本、配置和架构差异，不能把“可能依赖”直接认定为“所有配置都会受影响”。

## **一个可以反复使用的修改评估模型**

假设以后要修改 `mobj` 的一项内存引用验证逻辑。

我们不会只看这个函数，而是沿着下面的路径分析：

**修改入口：MOBJ Validation**

数据结构

MOBJ 类型、偏移、引用计数

直接调用

Shared Memory、参数解析

间接依赖

TA VM、RPC、DMA

安全边界

NS Memory、越界、权限

并发风险

Unmap、Release、Race

平台差异

MMU、Cache、IOMMU

**Regression Test Plan**

Basic · Boundary · Malicious Input · Concurrency · Cross-Architecture

最终，每次涉及 Core 的修改，都应该能回答五个核心问题：

1. **Ownership：** 被修改的对象属于谁，由谁负责释放？
2. **Invariant：** 修改前后哪些安全和状态约束必须保持不变？
3. **Concurrency：** 哪些状态可能被多个 CPU、Thread 或 Session 同时访问？
4. **Compatibility：** 哪些 ABI、调用者、配置和 Architecture Backend 会受到影响？
5. **Verification：** 哪些测试可以真正证明修改正确，而不只是代码能够成功编译？

这是我认为从资深 Porting Engineer 成长为 Core Maintainer 最重要的分析习惯。

# **六、建议的实际学习顺序与优先级**

虽然整个计划有 60 个 Chapters，但我并不建议对每章平均分配时间，也不建议从 Chapter 01 一直线性读到 Chapter 60 才开始修改代码。

最好采用 **先建立整体地图，再突破 Core，随后扩展各子系统，最后完成综合项目** 的方式。

## **6.1 分阶段的里程碑**

1. **Milestone 1 — 完整理解 OS 执行模型**

   Chapters 01–18

   成果：可以解释 OP-TEE Boot、Thread、Interrupt、RPC 和 Concurrency；能独立分析 Trusted Thread 的各种状态。
2. **Milestone 2 — 完整理解内存与 TA Runtime**

   Chapters 19–30

   成果：能沿着 TA Load、Session、Syscall 和 VM 映射执行路径，追踪全部核心对象。
3. **Milestone 3 — 完整理解内核安全服务**

   Chapters 31–42

   成果：可以从 API 到 Backend 分析 Crypto 与 Storage，并能评估安全和数据兼容性风险。
4. **Milestone 4 — 具备平台及现代架构设计能力**

   Chapters 43–54

   成果：可以设计新的驱动和安全服务，对 FF-A、SPMC、Virtualization 和 Firmware Integration 形成系统理解。
5. **Milestone 5 — 具备独立维护 Core 的能力**

   Chapters 55–60

   成果：能够完成问题定位、设计、修改、跨模块影响分析、测试和上游 Patch 准备。

## **6.2 基于你的技术背景，我建议采用以下优先级**

| 学习方向                           | 优先级 | 原因                    |
| ------------------------------ | --- | --------------------- |
| Thread、Context、RPC、Concurrency | P0  | OP-TEE 非传统 OS 执行模型的核心 |
| Memory、MOBJ、VM、Pager           | P0  | 决定隔离正确性和资源生命周期        |
| TA Manager、Loader、Session      | P0  | 核心安全服务运行时             |
| Syscall、User/Kernel Interface  | P0  | 最大的跨权限边界之一            |
| Build、Boot、Initcall            | P1  | 理解模块初始化与架构依赖          |
| Storage、Crypto                 | P1  | 最典型的安全服务实现            |
| Driver、DMA、Power               | P1  | 与 SoC 实际研发紧密相关        |
| Security Hardening、Testing     | P1  | 直接决定修改是否可靠            |
| FF-A、SPMC、Standalone MM        | P2  | 现代架构的重要扩展             |
| Virtualization、生态集成            | P2  | 高级专题，可按实际项目深入         |

这里的 P2 并不是说内容不重要，而是在当前以 OP-TEE OS Internals 为主线时，不需要让这些扩展主题抢占对 Kernel、Memory 和 TA Runtime 的研究时间。

### **推荐的第一条源码深入路线**

我尤其建议先完成下面这条链：

```
CA / Linux Driver
       |
       v
OP-TEE Entry
       |
       v
Trusted Thread
       |
       v
TA Session Manager
       |
       v
User TA Loader (ldelf)
       |
       v
TA VM / Memory Mapping
       |
       v
TA User Mode Execution
       |
       v
libutee / Syscall
       |
       v
Kernel Service
       |
       v
RPC / Crypto / Storage
       |
       v
Cleanup / Return
```

这条路径能够把大部分 Core 重要模块串联起来。

理解它之后，再学习各种独立的 Driver、Crypto Backend 或 Storage Backend，就不会觉得它们是彼此孤立的代码。

# **七、建议建立一套自己的 OP-TEE 内核研究知识库**

考虑到这套课程有 60 章，而且你的目标是未来能够持续开发和维护 OP-TEE，我建议课程讲义之外，还维护五种技术资料。

| 资料                   | 应包含的内容                        | 用途                |
| -------------------- | ----------------------------- | ----------------- |
| Architecture Map     | 每个子系统的职责、接口、依赖                | 快速建立整体认识          |
| Source Index         | 重要结构体、函数、源码位置                 | 快速定位代码            |
| Execution Traces     | Boot、TA Load、RPC、Storage 等调用链 | 理解真实运行行为          |
| Invariant Notebook   | 锁、引用计数、权限、对象生命周期              | 防止修改破坏安全属性        |
| Impact & Test Matrix | 修改路径、依赖、风险和测试                 | 日常开发与 Code Review |

我非常推荐把 **Invariant Notebook** 作为这次学习的新重点。

例如，学习 TA Manager 时，不仅记录 `tee_ta_open_session()` 调用了哪些函数，还要记录：

- 一个 Session 在什么时候可以引用 TA Context。
- 谁负责增加和减少 Reference Count。
- Session Close 与 TA Panic 并发时的状态约束。
- 什么条件下允许执行 Close Entry Point。
- 什么时候可以从全局 TA Context List 中移除对象。
- 哪些调用路径允许 Sleeping/RPC，哪些路径绝对不允许。

这类知识在日常开发中比单纯保存函数调用图更有价值。

# **八、课程的实验与考核标准**

这次建议将每章的掌握程度分成四个等级。

**Chapter 掌握程度自检**

0 / 4

Level 1 — Architecture Understanding

能够解释该子系统为什么存在、解决什么问题，以及与其他组件的边界。

Level 2 — Source Understanding

能够从入口追踪到完整实现，解释关键数据结构、调用链与状态机。

Level 3 — Debug & Implementation

能够复现问题、定位原因，并完成局部代码修改和实验验证。

Level 4 — Maintainer Understanding

能够评估跨模块影响、解释安全不变量，完成测试方案和有说服力的 Code Review。

建议只有达到 Level 3，才认为核心章节已真正掌握；P0 章节应以 Level 4 为目标。

每章最终至少提供四种考核材料：

- 一个完整的源码路径追踪题。
- 一个涉及异常、并发或权限的真实 Debug 场景。
- 一个需要做技术权衡的小型设计题。
- 约 10 道 Staff Engineer 级别的综合测试题，附参考答案与考核要点。

对于关键章节（例如 Thread、MOBJ、VM、TA Manager），建议再增加一个可复现的小型 Patch 实验。

# **九、整套课程的最终综合项目**

我建议课程最后不要只用一份笔试题来验收，而是选择一个能够贯穿主要子系统的真实问题。

结合你后续在 RISC-V OP-TEE 上的工作，可以考虑：

## **Capstone：为 OP-TEE 设计一个安全的 Shared Memory & Device Access Framework**

这是一个架构研究项目，不预设上游已经存在完全对应的实现。

假设有以下需求：

> 一个 User TA 需要使用安全硬件 Crypto Engine，必须通过 OP-TEE Core 管理 DMA Buffer，确保 Normal World 无法越权访问安全缓冲区，同时支持多核、多 Session 并发和设备异常恢复。

需要设计的内容：

Capstone Architecture

**User TA**

GP Crypto API / User Buffer

**Syscall & Kernel Service**

Validation · Identity · Handle Ownership

MOBJ / VM

Allocation · Mapping · Refcount

Crypto Driver

Hardware Context · Locks

DMA / SMMPT

Device Access Permissions

IRQ / Thread

Completion · Wait · Resume

**Secure Hardware Accelerator**

DMA · Interrupt · Error / Reset

为了通过这个项目的评审，需要解释：

1. User TA 为什么不能直接拥有任意 DMA Address。
2. Syscall 如何验证 User Buffer。
3. MOBJ 的引用关系如何与 DMA 生命周期绑定。
4. 多个 Trusted Thread 如何安全访问同一个 Crypto Engine。
5. DMA 完成前是否可以释放或取消映射 Buffer。
6. 设备 Reset 时如何清理正在运行的请求。
7. SMMPT/IOMMU 权限需要在什么阶段建立和撤销。
8. Cache、TLB、Device Translation Cache 应如何同步。
9. Suspend/Resume 时如何维护安全状态。
10. 哪些修改可以归入通用 Core，哪些必须留在平台 Backend。

这个项目基本上会使用到本课程大部分关键知识，并且与真实 SoC 安全软件工程问题有非常直接的关联。

# **十、最终学习建议**

这 60 个 Chapter 是一套完整的覆盖型课程，而不是必须按固定节奏完成的任务表。

我建议把学习分成两轮：

**第一轮：建立完整的 OS 心智模型。** 每章先理解架构、数据结构和主要调用链，掌握模块间的依赖，不必立即逐行阅读所有源码。

**第二轮：深入 P0/P1 子系统。** 选择 Thread、Memory、TA Runtime、Syscall、Storage 等重点章节，结合实际代码、GDB、xtest 和小型 Patch，达到能够分析异常路径与修改影响的程度。

其中有三项知识最值得反复研究：

- **Execution Context：** OP-TEE 正在代表谁执行？处于哪个 CPU、Thread、TA Context 和 Session 中？
- **Resource Ownership：** 每个内存对象、Handle、Session 和 Hardware Resource 属于谁？谁来清理？
- **Security Invariant：** 在所有成功、失败、并发和异常路径中，什么条件必须始终成立？

这三项将贯穿整套课程。

## **参考基线与上游资源**

本次课程规划主要参考以下官方资料与源码：

- [OP-TEE OS — GitHub 上游源码](https://github.com/OP-TEE/optee_os?utm_source=chatgpt.com)
- [OP-TEE 4.11.0 官方文档](https://optee.readthedocs.io/en/4.11.0/?utm_source=chatgpt.com)
- [OP-TEE Core Architecture](https://optee.readthedocs.io/en/latest/architecture/core.html?utm_source=chatgpt.com)
- [OP-TEE Source Tree Structure](https://optee.readthedocs.io/en/latest/architecture/file_structure.html?utm_source=chatgpt.com)
- [Trusted Applications Architecture](https://optee.readthedocs.io/en/latest/architecture/trusted_applications.html?utm_source=chatgpt.com)
- [OP-TEE Security Scope](https://optee.readthedocs.io/en/latest/general/security_scope.html?utm_source=chatgpt.com)

后续逐章深入时，应该优先使用对应版本的实际源文件，而不是仅根据文档中的旧路径或旧实现推断当前行为。

## **结束语**

我认为，这次学习与之前的 RISC-V OP-TEE Porting 系列课程可以形成很好的互补：

**之前的课程解决“如何把 OP-TEE 运行在一种硬件架构上”。**

**这次的课程解决“OP-TEE 作为一个操作系统，内部为什么这样工作，以及怎样安全地修改它”。**

前者深入 CPU、Firmware、MMU、Trap 和硬件隔离；后者深入 Kernel、Memory Object、Thread、TA Runtime、Syscall、Storage、Crypto 以及整个 OS 的内部依赖关系。

将两条知识主线结合起来，才更接近一位能够审查 Architecture Patch、定位复杂跨模块问题，并长期维护 OP-TEE Core 的工程师所需要的能力。

**我的建议是：就按这 10 个 Parts、60 个 Chapters 作为正式教学大纲。后续每章以“架构设计 → 核心数据结构 → 真实源码 → 执行路径 → 异常与并发 → 修改影响 → 实验与 Staff 测试题”的顺序完整展开。**

最终衡量学习效果的标准，不是读过多少行 OP-TEE 源码，而是当一段你从未见过的 OP-TEE Core 代码需要修改时，你能否迅速建立正确的系统模型，判断风险，并拿出能够验证正确性的方案。
