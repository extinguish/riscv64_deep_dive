# RISC-V OP-TEE Architecture & Porting

系统性深度学习大纲 · 面向 Staff Engineer / OP-TEE Upstream Maintainer

我建议将这部分整理成一套完整的 《RISC-V OP-TEE 内核架构、OpenSBI Domain 与跨平台移植》 教程。

这套教程不再从 RISC-V 指令集基础或 GP TEE API 开始，而是直接进入 RISC-V 如何建立可信执行环境、OP-TEE 如何运行、如何与 Linux 协作，以及如何将整套架构移植到真实 SoC 这些问题。

我会按一本正式技术教材的标准来设计目录，既覆盖 Why、What、How，也要保证未来逐章展开时能深入到 C、汇编、CSR、Device Tree 和 QEMU 实验。

## 一、这套课程最终需要解决什么问题？

我认为学习的终点不是能够编译并启动 RISC-V OP-TEE，而是能够独立完成以下系统设计：

目标系统架构（概念图）

REE Domain

Linux / U-Boot

S-mode

TEE Domain

OP-TEE / TA

S-mode / U-mode

SBI MPXY / RPMI / Domain Switch

M-mode — OpenSBI / Trusted Firmware

Domain Management · Hart Context · PMP · HW Isolation

SoC Hardware Security Boundary

PMP · IOPMP / WorldGuard / SoC Firewall · DMA Protection

以当前 OpenSBI Domain + RISC-V OP-TEE 路线为基础的逻辑架构，不表示所有 SoC 均具备相同的硬件隔离能力。具体安全性依赖硬件及固件实现。

在这套系统中，你最终应该能够回答五个根本问题：

1. Security：RISC-V 没有 Arm TrustZone 式的标准 Secure/Normal Security State，为什么仍然可以建立一个具备隔离保障的 TEE？
2. Privilege：Linux 和 OP-TEE 都运行在 S-mode，为什么 Linux 无法任意访问 OP-TEE 内存？
3. Execution：一个 hart 如何在 Linux Domain 和 OP-TEE Domain 之间切换？究竟要保存哪些寄存器和系统状态？
4. Concurrency：多个 hart 同时进入 OP-TEE 时，Trusted Thread、SMP、TLB、Interrupt 和 Domain 状态如何保持一致？
5. Portability：从 QEMU virt 移植到实际 RISC-V SoC，哪些部分可以复用，哪些需要重新设计和实现？

截至 2026 年 10 月，RISE 的 RP016 和 RP022 已分别覆盖 OpenSBI TEE 基础设施及 RISC-V OP-TEE 增强工作。Linux 侧的 RPMI TEE 传输也已有 RFC 实现，因此我们可以将真实 upstream 代码和正在发展的设计作为教学案例，而不是凭空构造一套架构。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE

+2



# 二、完整课程结构

我建议分成 六个阶段、22 章。

1. Part I — RISC-V TEE 系统架构与工程环境

   Chapter 01–03 · 建立正确的整体认知模型
2. Part II — OpenSBI Domain & Hardware Isolation

   Chapter 04–07 · 理解 RISC-V TEE 最核心的硬件安全边界
3. Part III — RISC-V OP-TEE Core Kernel

   Chapter 08–12 · 深入异常入口汇编、Thread、MMU、TA 执行
4. Part IV — REE / TEE 通信与完整调用链

   Chapter 13–15 · SBI MPXY、RPMI、RPC、Shared Memory
5. Part V — Interrupt、SMP 与真实 SoC 移植

   Chapter 16–19 · 从 QEMU 扩展到真实多核安全平台
6. Part VI — Engineering、Security Validation & Upstream

   Chapter 20–22 · 完成可交付移植并形成社区贡献能力

## Part I — RISC-V TEE 系统架构与工程环境

目标：先掌握 RISC-V TEE 的设计理念，建立一张能够指导后续源码研究的完整架构图。

### Chapter 01 — RISC-V TEE Architecture：从 TrustZone 到 Domain Isolation

本章是整套课程最重要的理论基础。

1.1 TEE 的安全目标、Threat Model 与 Trusted Computing Base（TCB）

1.2 Arm TrustZone 的安全边界：NS/S、EL3、Secure EL1、Secure EL0

1.3 RISC-V M/S/U Privilege 为什么不能直接等价于 Normal/Secure World

1.4 RISC-V 实现 TEE 的不同架构模式：M-mode Security Monitor、PMP-based Isolation、Security Domain

1.5 OpenSBI Supervisor Domain 与 Security Domain 的关系

1.6 Linux 和 OP-TEE 同为 S-mode 时，安全隔离究竟建立在哪里？

1.7 PMP、MMU、SoC Firewall、DMA Protection 各自承担什么职责？

1.8 对比 Arm TrustZone 和 RISC-V OP-TEE 的 TCB、切换机制和系统安全模型

本章关键问题： 为什么运行在 S-mode 的 Linux 即使获得 root，也不应该能够访问另一个受硬件保护的 S-mode OP-TEE Domain？

### Chapter 02 — RISC-V OP-TEE System Boot Architecture

2.1 从 SoC Reset Vector 到 M-mode Firmware 的完整启动链

2.2 Boot ROM、Secure Boot、Root of Trust 与 OpenSBI 之间的关系

2.3 `fw_dynamic`、`fw_jump`、`fw_payload` 与 TEE 启动链设计

2.4 OpenSBI 的 coldboot / warmboot、Boot Hart 与 Secondary Hart

2.5 Domain 的创建、Hart Assignment 和 Boot Domain 配置

2.6 OP-TEE Core Image 的加载地址、内存布局与入口参数

2.7 Linux Domain 和 OP-TEE Domain 的初始化顺序

2.8 U-Boot、Linux、OP-TEE、OpenSBI 的 Device Tree 传递和使用

2.9 Core Image 的认证、加载和执行之间的信任关系

源码重点： OpenSBI 初始化代码、`core/arch/riscv/kernel/boot.c`、`entry.S`。

### Chapter 03 — RISC-V OP-TEE Development Environment

3.1 OP-TEE 多仓库结构：`optee_os`、`optee_client`、`optee_test`、Linux、OpenSBI

3.2 Upstream 分支与 RISE 开发分支的差异

3.3 WSL2 + QEMU RV64 + Buildroot 环境搭建

3.4 GNU / LLVM Toolchain、Linker Script 和 Build Config

3.5 建立完整 OpenSBI + OP-TEE + Linux + xtest 启动环境

3.6 QEMU GDB：跟踪 M-mode、S-mode、U-mode 的代码

3.7 反汇编、ELF Section、Symbol、Map File 分析

3.8 固定 commit、工具链和配置，构建可复现实验基线

实验 01：完成 RISC-V OP-TEE 的基础开发环境

运行完整启动链，分别确认 OpenSBI、OP-TEE 和 Linux 的启动日志，并运行基础 xtest；使用 GDB 分别在 OpenSBI 和 OP-TEE 的入口处设置断点。

官方参考：[RISE RP016 Build Instructions](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/build/?utm_source=chatgpt.com)。该环境基于 Buildroot 管理 OpenSBI、OP-TEE、U-Boot、Linux 和 QEMU。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



## Part II — OpenSBI Domain & Hardware Isolation

第一核心专题

这是我建议你投入最多精力的阶段之一。

它决定了你是停留在「OP-TEE 移植工程师」，还是有能力参与 RISC-V TEE 安全架构设计。

### Chapter 04 — OpenSBI Domain Architecture

4.1 为什么 OpenSBI 需要 Domain？

4.2 `struct sbi_domain` 的设计思想与核心字段

4.3 Domain 与 Hart 的归属、运行和资源分配关系

4.4 Domain Memory Region：物理内存描述及权限模型

4.5 Domain 的创建、注册、初始化与生命周期

4.6 Device Tree 中的 `opensbi-domains` 配置及其解析过程

4.7 Root Domain、Linux Domain、OP-TEE Domain 的关系

4.8 多 Domain 与同一 hart 时间复用之间的区别

4.9 Domain 与 Hypervisor VM 的本质区别

源码重点：

`opensbi/lib/sbi/sbi_domain.c`

### Chapter 05 — PMP 与系统级 Hardware Isolation

5.1 PMP Address Matching、Region Priority 和访问权限回顾

5.2 为什么 PMP 能够保护 OP-TEE 的物理内存

5.3 PMP 与 S-mode MMU：物理权限和虚拟权限的组合

5.4 PMP Entry 数量有限时，如何设计内存布局？

5.5 PMP 不能保护哪些资源：DMA、外设、总线 Master

5.6 IOPMP、WorldGuard、SoC Firewall 的设计理念

5.7 System-Level Hardware Isolation Framework

5.8 Domain Exit/Enter 期间如何安全地更新保护配置

5.9 隔离策略配置错误可能引入哪些漏洞？

源码重点：

`opensbi/lib/sbi/sbi_hart_protection.c`

对应的 RP016 Milestone 4 已设计按 Domain 注册硬件隔离机制及执行生命周期回调的框架，适合作为深入源码研究案例。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### Chapter 06 — OpenSBI Domain Context Switching

重点攻克

6.1 为什么 Domain Switch 不等于普通的 Task Context Switch

6.2 从 S-mode ECALL 进入 M-mode 的实际执行路径

6.3 `mepc`、`mstatus`、`satp`、`stvec`、`sscratch` 等寄存器的保存和恢复

6.4 OpenSBI Per-Hart Domain Context 数据结构

6.5 Domain Switch 时的 GPR / CSR / FP / Vector Context

6.6 Domain Switch 与 PMP 重新配置的顺序

6.7 `SFENCE.VMA`、TLB 和 Address Space Synchronization

6.8 从 Linux Domain 切入 OP-TEE Domain

6.9 OP-TEE 返回 Linux Domain 的完整路径

6.10 Domain Switch 的安全不变量与失败恢复

核心代码：

`opensbi/lib/sbi/sbi_domain_context.c`

这是现有 OpenSBI 中实际存在的 Context Switch 实现，可用于分析 per-hart context、CSR、FP/Vector 状态保存以及 PMP 重新配置。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### Chapter 07 — Domain Context 的多核一致性与资源管理

7.1 Per-Hart Domain Context 与 Global Domain State

7.2 Domain Context 切换与 Hart Assignment 的同步

7.3 同一 Domain 同时运行多个 hart

7.4 PMP 配置的 Hart-local 性质与一致性问题

7.5 Domain Memory Policy 动态变化时的同步

7.6 跨 hart 切换、重新分配和热插拔的安全问题

7.7 Shared Resource、Lock、Race Condition 和 Deadlock

7.8 安全性验证：状态泄露、权限残留与错误回滚

实验 02：构造两个 OpenSBI Domain 并跟踪切换过程

分别为两个 Domain 分配不同的内存权限，观察 Domain Context、CSR 和隔离配置在切换前后的变化。

进阶实验：故意设计一个非授权物理内存访问，验证硬件是否能够真正阻止该访问。

参考：[RP016 Hardware Isolation Framework](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/milestone4/?utm_source=chatgpt.com)。

## Part III — RISC-V OP-TEE Core Kernel Architecture

第二核心专题

完成 Part II 后，我们就已经知道了 OpenSBI 如何给 OP-TEE 建立隔离的执行环境。

接下来的问题是：

OP-TEE 自己是如何作为一个内核运行的？

### Chapter 08 — OP-TEE Boot & Low-Level Assembly

8.1 RISC-V OP-TEE ELF、Linker Script 与 Memory Layout

8.2 从 OpenSBI 进入 OP-TEE Boot Entry

8.3 `entry.S`：Early Boot Assembly 的实际执行过程

8.4 初始化 `sp`、`gp`、`tp` 和早期执行环境

8.5 Primary Hart 与 Secondary Hart 的初始化差异

8.6 Early MMU / `satp` 配置及地址空间切换

8.7 BSS、Stack、Heap、Per-Hart Data 初始化

8.8 从汇编入口进入 C Runtime

8.9 Initcall 与 OP-TEE Core 子系统初始化

核心源码：

`core/arch/riscv/kernel/entry.S`、`boot.c`

### Chapter 09 — RISC-V Trap / Exception Architecture in OP-TEE

9.1 `stvec`、`scause`、`sepc`、`stval`、`sstatus`

9.2 RISC-V S-mode Trap Entry Assembly

9.3 Exception Entry 时如何使用 `sscratch` 与 Stack

9.4 GPR 保存、Exception Frame 和嵌套异常

9.5 OP-TEE 如何处理 Page Fault、Illegal Instruction

9.6 U-mode TA Trap 如何进入 S-mode OP-TEE

9.7 M-mode Trap 和 OP-TEE S-mode Trap 的边界

9.8 Fatal Exception、Panic、Debug Dump 与 Recovery

9.9 从 Trap Handler 返回原执行上下文

核心源码：

`core/arch/riscv/kernel/entry.S`、`abort.c`

### Chapter 10 — Trusted Thread & Context Switching

重点攻克

10.1 为什么 OP-TEE 不采用传统 RTOS 的 Scheduler 模型？

10.2 Trusted Thread 与 NuttX Task / pthread 的区别

10.3 Trusted Thread State Machine

10.4 Per-Hart Entry Stack 与 Trusted Thread Stack

10.5 Trusted Thread 的创建、获取和释放

10.6 Thread Suspend / Resume 如何实现

10.7 OP-TEE 的 Context 保存结构和架构相关字段

10.8 RPC 为什么会导致 Trusted Thread Suspend

10.9 Trusted Thread 与 OpenSBI Domain Context 的区别

10.10 Trusted Thread、TA Context、Linux Task 三者的关系

源码重点：

`core/arch/riscv/kernel/thread_rv.S`、`thread_arch.c` 及通用 Thread 接口。

这一章要非常深入，尤其是寄存器、栈、恢复地址和生命周期。

### Chapter 11 — Memory Management & TA Isolation

11.1 OP-TEE 的内存区域：Core RAM、TA RAM、Shared Memory

11.2 Sv39/Sv48 在 OP-TEE 中的使用方式

11.3 Core MMU 与 User TA MMU 的权限边界

11.4 `satp` / ASID / TLB 的管理模型

11.5 User TA 地址空间的创建与销毁

11.6 TA A 与 TA B 的页表隔离

11.7 OP-TEE Core 与 TA 之间的 U/S 权限隔离

11.8 Shared Memory Mapping 与非可信内存验证

11.9 `SFENCE.VMA` 和 SMP TLB Shootdown

11.10 Pager、ASLR、Memory Protection 与 Security Hardening

源码重点：

`core/arch/riscv/mm/`、`core/mm/`

### Chapter 12 — User TA Execution & Architecture Context

12.1 RISC-V User TA 的 ELF / PIE / Loader 设计

12.2 OP-TEE Core 如何进入 U-mode 运行 TA

12.3 User TA 的 Stack、TLS、GP/TP 和 ABI

12.4 TA 如何调用 TEE Syscall

12.5 Syscall Trap 和从 U-mode 返回 S-mode

12.6 TA Session / Context Lifecycle

12.7 FP/Vector Register Context 管理

12.8 Lazy Context Switch 和 Eager Context Switch 的取舍

12.9 TA 切换过程中的状态泄露风险

12.10 TA Crash、异常终止和资源清理

RP022 的现有工作已经包括 RISC-V TA FP/Vector Context Management、ASLR、Control-Flow Integrity 等，这些内容非常适合与真实 upstream 改动结合学习。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



实验 03：跟踪一个 RISC-V User TA

从 OP-TEE 的入口开始，使用 GDB 跟踪 TA 的加载、U-mode 入口、Syscall、S-mode 返回，并记录整个过程中 `satp`、`sstatus`、`sepc`、SP 与关键上下文结构的变化。

进一步构造 TA 非法访问和异常退出测试，确认隔离和恢复行为。

## Part IV — Linux / OpenSBI / OP-TEE Communication

这个阶段将 Part II 的 Domain 模型和 Part III 的 OP-TEE Kernel 真正连接起来。

### Chapter 13 — SBI MPXY / RPMI TEE Communication Architecture

13.1 为什么 RISC-V 无法直接照搬 Arm SMCCC/SMC 模型

13.2 SBI ECALL 与传统 SBI Extension

13.3 MPXY（Message Proxy）的设计动机

13.4 RPMI（RISC-V Platform Management Interface）的职责

13.5 RPMI TEE Service Group

13.6 Linux 如何通过 RPMI TEE_CALL 请求 OP-TEE

13.7 Per-Hart MPXY Channel 与 Domain Context 的关系

13.8 参数传递、返回值、错误处理与协议版本

13.9 OP-TEE 原有 SMC ABI 与新 Transport 的兼容问题

13.10 Transport 抽象的选择与版本演进

这里有一个很好的真实架构讨论案例：2026 年 9 月出现过不同的 Linux RFC 实现路线，一种强调在现有 SMC ABI 层加入 RISC-V Conduit，另一种借鉴 FF-A Backend 设计新 Transport。因此课程会比较这些设计取舍，而不会将某一 RFC 当作已经完全稳定的规范。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

Openwall Lists

+1



### Chapter 14 — Shared Memory & RPC Architecture

14.1 为什么 REE / TEE 通信不能只依靠寄存器传参

14.2 OP-TEE Message ABI

14.3 Static Shared Memory 与 Dynamic Shared Memory

14.4 RPMI Memory Parcel 与共享内存生命周期

14.5 Linux `tee_shm` 与 OP-TEE `mobj` 映射关系

14.6 RPC 为什么需要暂时返回 REE

14.7 `tee-supplicant` 在整体架构中的作用

14.8 RPC 返回后如何恢复 Trusted Thread

14.9 Cache Coherency、DMA 与 Shared Memory

14.10 TOCTOU、Double Fetch 和非可信参数验证

### Chapter 15 — Complete TA Invocation & Return Path

15.1 从 `TEEC_InvokeCommand()` 进入 Linux TEE Driver

15.2 Linux Driver → SBI MPXY → RPMI TEE_CALL

15.3 M-mode Firmware → OP-TEE Domain

15.4 OP-TEE Trusted Thread 分配与执行

15.5 TA Syscall 与 Core Service

15.6 RPC/Yield 时的执行状态保存

15.7 从 Linux 返回并恢复 OP-TEE 执行

15.8 TA 完成后返回 Linux

15.9 超时、错误、取消和异常退出路径

15.10 与 Arm64 OP-TEE SMC 执行链逐层对照

实验 04：实现全链路 Trace

从 Linux CA 的一次 `TEEC_InvokeCommand()` 开始，在 Linux TEE Driver、OpenSBI 和 OP-TEE Kernel 记录关键状态，重建完整的 TA Invoke、RPC 和 Return 调用链。

最终交付一份跨越 U-mode、S-mode 和 M-mode 的寄存器及调用栈分析。

## Part V — Interrupt、SMP 与真实 SoC 移植

这一阶段决定你是否具备处理工业级复杂平台的能力。

### Chapter 16 — Interrupt Architecture & Domain-Aware Routing

16.1 PLIC、APLIC、IMSIC 在 TEE 平台中的职责

16.2 Interrupt Delegation 与 Domain Isolation 的相互影响

16.3 M-mode / S-mode Interrupt Ownership

16.4 外部中断到达时如何确定目标 Domain

16.5 HWIRQ / VIRQ Mapping 与 Interrupt Routing

16.6 Foreign Interrupt 与 OP-TEE Yield

16.7 中断驱动的 Domain Context Switch

16.8 Interrupt Priority、Masking、Pending 与 Completion

16.9 Interrupt Storm、Lost Interrupt 和错误恢复

16.10 Secure Interrupt 与普通 Interrupt 的隔离要求

这里可以直接研究 RP016 Milestone 2/3：APLIC M-mode Interrupt Handling、VIRQ Mapping、Per-Domain Pending Queue 和中断触发的 Domain Switch。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE

+1



### Chapter 17 — SMP & Multi-Hart TEE Execution

重点攻克

17.1 Hart、Per-Hart State 与 Trusted Thread Pool

17.2 多个 hart 同时执行 Linux 和 OP-TEE 的不同情况

17.3 Domain Switch 对每个 hart 的影响

17.4 SMP 下的 Spinlock 和 Memory Barrier

17.5 Shared Memory Cache Coherency

17.6 RISC-V Memory Model 与 TEE Synchronization

17.7 Page Table Modification 与 Remote TLB Shootdown

17.8 Hart Start/Stop、IPI 与 HSM

17.9 CPU Hotplug / Suspend / Resume 对 TEE 的影响

17.10 Race、Deadlock、Livelock 和 Worst-case Latency

17.11 跨 Domain 状态泄露与并发安全验证

### Chapter 18 — Device Tree & Platform Porting

18.1 `qemu virt` 平台和真实 SoC 的差异

18.2 `plat-virt` 的 Platform Configuration

18.3 OP-TEE Physical Memory Layout 与 Reserved Memory

18.4 OpenSBI Domain Device Tree 定义

18.5 Interrupt Controller、UART、Timer 等外设配置

18.6 RPMI / MPXY Transport 的 Device Tree 描述

18.7 PMP / WorldGuard / IOPMP Policy 配置

18.8 OP-TEE Platform Driver 与 Device Initialization

18.9 Trusted Firmware、U-Boot 和 Linux 之间的数据交接

18.10 构建一个最小化的新 RISC-V OP-TEE Platform Port

### Chapter 19 — Hardware Root of Trust & SoC Security Integration

19.1 Boot ROM、OTP、Secure Boot 与 OP-TEE 启动信任链

19.2 独立 RISC-V HSM Core 与主 CPU 的信任边界

19.3 OP-TEE 如何使用 HSM 提供的密钥与加密服务

19.4 Hardware Unique Key、Device Identity 与 Key Derivation

19.5 Hardware Crypto Engine Driver

19.6 Secure Storage、RPMB 和密钥持久化

19.7 DMA Firewall、Peripheral Ownership 与 Bus Security

19.8 SoC Reset、Power Domain 与安全状态保持

19.9 HSM / OpenSBI / OP-TEE 之间的 IPC 安全

19.10 从 Firmware Porting 到完整 SoC Security Architecture

## Part VI — Engineering、Security Validation & Upstream

### Chapter 20 — Debugging、Testing & Security Validation

20.1 OP-TEE Logging、Trace 与 Panic Dump

20.2 OpenSBI Trap Dump 与 Domain Context Debug

20.3 GDB 跨 M/S/U Privilege 调试

20.4 xtest、SBI Unit Test 与 Architecture Test

20.5 MMU、PMP、Interrupt 和 Context Switch 的回归测试

20.6 SMP Stress Test 与 Race Detection

20.7 Exception Injection、Fault Injection 和 Fuzzing

20.8 Context Leakage、Invalid Pointer、TOCTOU Security Test

20.9 QEMU CI、Build Matrix 与 Test Automation

20.10 如何把一次实际 Bug 修复转化为高质量 upstream Patch

### Chapter 21 — End-to-End RISC-V OP-TEE Porting Project

这是整个课程的毕业设计。

21.1 目标 SoC 的硬件和安全能力分析

21.2 SoC Security Threat Model 与 TCB 定义

21.3 Boot Chain 和 Firmware Integration

21.4 OpenSBI Domain 配置和硬件隔离

21.5 OP-TEE Core Boot 与 MMU 初始化

21.6 RPMI/MPXY/TEE Driver 集成

21.7 Shared Memory、RPC 与 User TA

21.8 SMP、Interrupt 与 Hart Lifecycle

21.9 Secure Storage / HSM 集成

21.10 系统级安全测试、性能验证与交付文档

最终项目要求形成能复现、能测试、可以接受 Code Review 的 Port，而不只是输出一个可以启动的镜像。

### Chapter 22 — OP-TEE Upstream Contribution & Maintainer Practice

22.1 OP-TEE 和 OpenSBI 的代码治理及维护边界

22.2 如何选择适合 upstream 的 Issue

22.3 Coding Style、Commit Message、DCO、Patch 拆分

22.4 如何设计测试以证明 Patch 的正确性

22.5 Bugfix、Feature、Refactoring 的不同 Review 标准

22.6 如何识别 ABI 与 Security Compatibility 风险

22.7 如何阅读并参与 RFC

22.8 Cross-Architecture Review：Arm 与 RISC-V 的代码复用

22.9 持续维护一个子系统的工作内容

22.10 从 Contributor 走向 Reviewer / Maintainer

# 三、这 22 章应该按照什么顺序学习？

虽然我按照 Part I～VI 组织了完整大纲，但不建议严格依照章节次序学完所有内容之后才开始实践。

更高效的方式是沿着 三条核心技术主线交叉深入。

主线 A：硬件隔离与 Domain Architecture

Ch 01

Ch 04

Ch 05

Ch 06

Ch 07

Ch 19

解决 TEE 在 RISC-V 上为什么安全，以及安全隔离由谁负责的问题。

主线 B：OP-TEE Kernel Execution

Ch 08

Ch 09

Ch 10

Ch 11

Ch 12

解决 OP-TEE 如何进入、运行、处理中断、切换上下文以及隔离 TA 的问题。

主线 C：完整系统交互与 SoC Porting

Ch 13

Ch 14

Ch 15

Ch 16

Ch 17

Ch 18

Ch 21

解决如何将一个 TEE Kernel 集成到完整的 Linux + OpenSBI + 多核 SoC 系统中。

我个人建议的第一次学习顺序是：

01 → 02 → 03 → 04 → 05 → 06 → 08 → 09 → 10 → 13 → 15 → 11 → 12

这个顺序有两个优点：首先，能够尽早贯通完整的 Linux → OpenSBI → OP-TEE → TA 执行链；其次，可以在理解了 Domain Switch 和 Trusted Thread 之后，再研究最复杂的 MMU/SMP 问题，减少概念混淆。

# 四、整套课程的学习深度标准

为了避免课程变成「介绍了一堆 Feature，但缺乏深度」，我建议每一章都采用统一的七层结构。

| 层次                       | 教学内容                     | 应达到的能力       |
| ------------------------ | ------------------------ | ------------ |
| L1 — Why                 | 设计背景、原始问题、约束和取舍          | 能解释为什么需要这项机制 |
| L2 — Architecture        | 组件、职责、信任边界、数据流           | 能画出准确的设计图    |
| L3 — Data Structures     | 关键数据结构、生命周期和状态机          | 能解释运行时状态如何维护 |
| L4 — Source Code         | 实际函数调用、关键分支、代码层级         | 能独立跟踪代码      |
| L5 — Assembly / Hardware | 汇编、CSR、MMU、PMP、Interrupt | 能解释实际硬件执行行为  |
| L6 — Hands-on            | QEMU/GDB、测试程序、故障注入       | 能独立复现和验证     |
| L7 — Engineering         | 性能、安全、回归、Review 和 Patch  | 能设计并交付可靠改动   |

特别是 L4 和 L5，我希望我们在后续正式课程中坚持几个原则：

- 涉及 Context Switch 时，必须给出寄存器保存/恢复的具体过程。
- 涉及 Memory Isolation 时，必须讲清 VA、PA、PTE、PMP Entry 如何共同决定访问权限。
- 涉及 Interrupt 时，必须区分发生 Trap 的 Privilege、负责路由的实体和实际处理 Interrupt 的 Domain。
- 涉及 SMP 时，必须分析 per-hart 与 global 数据结构、锁和 Memory Barrier。
- 涉及源码时，必须区分 upstream 已合并实现、开发分支和 RFC，不使用虚构的函数冒充实际代码。

## 五、最应该优先攻克的六章

如果你未来的时间有限，我认为下面六章具有最高的投入产出比。

Chapter 04

OpenSBI Domain Architecture

TEE 隔离抽象的基础

Chapter 05

PMP & Hardware Isolation

安全边界与真实硬件保护

Chapter 06

Domain Context Switching

深入 OpenSBI 汇编、CSR 与隔离切换

Chapter 10

OP-TEE Trusted Thread

理解 OP-TEE 核心执行模型

Chapter 11

MMU & TA Isolation

内存管理、地址空间和 SMP 正确性

Chapter 17

SMP & Multi-Hart TEE

复杂平台移植的关键能力

这六章形成一个很有价值的闭环：

Domain 定义安全隔离范围 → PMP/硬件机制落实隔离 → Domain Switch 切换安全执行环境 → Trusted Thread 组织 OP-TEE 执行 → MMU 隔离 TA → SMP 保证多 hart 的执行正确性与安全性。

把这条链彻底掌握，你基本上就具备了分析 RISC-V OP-TEE 核心架构问题的能力。

# 六、建议长期使用的参考资料

这些是后续每章展开时应该优先依赖的资料：

| 资料                                                                                                 | 用途                                                             |
| -------------------------------------------------------------------------------------------------- | -------------------------------------------------------------- |
| [OP-TEE OS Upstream](https://github.com/OP-TEE/optee_os?utm_source=chatgpt.com)                    | Core Kernel、RISC-V Architecture、TA、MMU                         |
| [OpenSBI Upstream](https://github.com/riscv-software-src/opensbi?utm_source=chatgpt.com)           | Domain、Trap、Hart、PMP、Context Switch                            |
| [RISE RP016](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/introduction/?utm_source=chatgpt.com) | OpenSBI TEE Infrastructure、Interrupt、Domain、Hardware Isolation |
| [RISE RP022](https://riscv-optee-docs-c9fea0.gitlab.io/rp022/introduction/?utm_source=chatgpt.com) | RISC-V OP-TEE 后续增强、TA Context、Security Hardening               |
| [OP-TEE Documentation](https://optee.readthedocs.io/?utm_source=chatgpt.com)                       | OP-TEE 通用内核、安全和测试架构                                            |
| [OP-TEE Pull Requests](https://github.com/OP-TEE/optee_os/pulls?utm_source=chatgpt.com)            | 实际 Upstream Review、设计讨论和 Bugfix                                |

## 七、完成这套课程后应该达到什么水平？

我希望最终通过四项综合考核，而不是通过记忆零散知识点来评价。

综合能力验收

目标清单

完整执行链

能从 Linux 调用开始，一直追踪到 OpenSBI Domain Switch、OP-TEE Trusted Thread、TA 执行和返回

架构级安全分析

能识别 PMP、MMU、DMA、Interrupt、SMP 的信任边界与潜在漏洞

实际平台移植

能够独立完成一个新的 RISC-V SoC OP-TEE 平台设计、Bring-up 和基础验证

Upstream Engineering

能提出合理的架构改进，编写测试、提交 PR，并对相关核心子系统进行 Review

已掌握 0 / 4

重置

我建议从 Chapter 01 — RISC-V TEE Architecture 正式开始。

虽然你已经对 Arm TrustZone、RISC-V Privilege、PMP、OpenSBI 有一定经验，但第一章仍然极其重要。它要解决的不是指令和寄存器的基本知识，而是一个更本质的架构问题：

> 在没有 Arm TrustZone Security State 的情况下，如何从 RISC-V 的特权模型、物理内存权限、Domain 管理和安全固件出发，建立一个真正可靠的 Trusted Execution Environment？

一旦这个问题建立了完整的认知模型，后面的 Domain Switching、Trusted Thread、TA Isolation、Interrupt 和 SMP 就不再是零散的技术细节，而会成为一套逻辑统一、可以推导和验证的系统架构。