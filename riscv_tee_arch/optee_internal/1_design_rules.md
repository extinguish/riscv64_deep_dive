# **Chapter 01 — OP-TEE 的设计哲学与 OS 定位**

OP-TEE OS Internals & Architecture · PART I · Chapter 01

**本章主题：为什么 OP-TEE 是一个操作系统？为什么它又不应该被设计成 Linux、NuttX 或 FreeRTOS？**

这一章是整个 60 Chapter 课程的理论基础。

我们暂时不急于分析 `thread.c`、`tee_ta_manager.c` 或 `vm.c` 的每一行代码，而是先建立一套思考 OP-TEE 架构的基本方法。这样，当后续遇到一个新的模块、新的安全服务或新的 RISC-V Porting 需求时，就能够理解它为什么应该采用某种设计，而不只是照搬当前的实现。

课程基线：**OP-TEE 4.11.0（2026-10-16）**。以官方架构文档和上游实现为依据，明确区分已有实现、设计原则以及我们自己的架构推演。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



## **本章学习目标**

学完这一章，应该能回答以下八个问题：

1. OP-TEE 到底提供了哪些 OS 级抽象？
2. 为什么 OP-TEE 没有采用传统的通用线程调度模型？
3. OP-TEE Secure Kernel、User TA 和 PTA 为什么必须分开？
4. OP-TEE 的 Trust Model 究竟是什么，谁可信，谁不可信？
5. 如何理解 TCB、Isolation Domain 和 Attack Surface 之间的关系？
6. OP-TEE 如何在不信任 Linux 的前提下使用 Linux 提供的资源？
7. 什么样的功能适合放在 Secure Kernel、PTA、User TA 或 Normal World？
8. 怎样根据这些设计原则，预测未来修改代码可能影响的模块和安全属性？

# **1.1 从本质出发：为什么需要 OP-TEE？**

要理解 OP-TEE 的设计，首先需要明确一个经常被忽略的问题：

**OP-TEE 究竟要解决什么普通操作系统解决不了的问题？**

## **1.1.1 从一个最基本的安全需求开始**

假设我们在设计一个包含 Linux 的 SoC。

Linux 中运行着一个加密应用程序：

```
Linux User Application
        |
        v
    OpenSSL
        |
        v
   Private Key
```

假设该程序持有一把设备身份私钥，私钥被用于 TLS 客户端身份认证。

系统最初可能通过以下手段保护私钥：

- 将私钥文件设置为仅 root 可读。
- 使用 Linux DAC、SELinux 等访问控制。
- 使用进程地址空间隔离。
- 使用 Linux Kernel Crypto Framework 或硬件加密模块。

这些机制各有价值，但共同存在一个问题：

**它们的安全性依赖 Linux 内核及其所信任的组件。**

假设攻击者已经完全控制 Linux Kernel。

此时攻击者可能：

- 修改进程的 Page Table。
- 读取用户态进程的内存。
- 修改系统调用的执行行为。
- 绕过 Linux 的文件权限检查。
- 劫持应用程序的系统调用和文件访问。
- 通过 DMA、驱动或其他硬件接口尝试访问敏感数据。

如果私钥明文位于普通 Linux 管理的内存中，那么单纯的用户态权限隔离通常已经不足以保护它。

于是，我们提出一个更强的需求：

> 即使 Normal World OS 已经被攻击者完全控制，设备身份私钥仍然不能被其直接读取或未经授权地使用。

注意这里有两个不同的安全目标：

**目标 A：** Linux 无法读取私钥明文。

**目标 B：** Linux 即使不能读取私钥，也不能任意让系统替它完成未经授权的签名操作。

目标 A 属于 Confidentiality。

目标 B 则涉及 Authorization、Integrity，以及服务是否被滥用。

这是两个不同的问题。仅仅把私钥搬进 Secure World，并不能自动保证目标 B。

## **1.1.2 为什么不能仅仅依赖 MPU/MMU？**

我们可能想到：既然问题是内存隔离，那就通过 MMU 或 PMP 保护私钥所在区域。

这是必要的，但仍然不完整。

考虑以下问题：

1. 谁负责设置保护规则？
2. Linux 是否有权限修改这些规则？
3. 谁负责检查每一次签名请求是否合法？
4. 私钥如何生成、加载和保存？
5. 多个应用同时请求使用私钥时，如何隔离各自的状态？
6. 请求失败或应用崩溃时，如何清理敏感资源？
7. 设备休眠后，私钥和相关硬件状态如何恢复？

仅仅提供一块受到保护的内存，并不能回答这些问题。

我们需要一个运行在受保护执行环境中的软件系统，负责协调这些机制。

```
               Untrusted World
       +--------------------------+
       | Linux Kernel             |
       | User Applications        |
       +-------------+------------+
                     |
                     | Service Request
                     v
               Trust Boundary
       +-------------+------------+
       | Trusted Execution System |
       |                          |
       | Identity / Authorization |
       | Memory Management        |
       | Program Loading          |
       | Cryptography             |
       | Secure Storage           |
       | Resource Management      |
       | Error Handling           |
       +-------------+------------+
                     |
                     v
            Protected Hardware
```

这个受信任的软件系统，正是 TEE OS 要承担的职责。

### **本质总结**

OP-TEE 的核心目标并不是提供一个功能更少的 Linux。

**它要在一个可能受到攻击的完整操作系统旁边，建立独立的受保护执行环境，并在其中提供可控的安全计算服务。**

这决定了它的很多架构选择。

# **1.2 OP-TEE 为什么能够被称为 OS？**

我们通常用以下特征判断一个软件系统是不是操作系统：

- 是否管理执行上下文？
- 是否管理内存资源？
- 是否提供隔离和权限控制？
- 是否管理程序生命周期？
- 是否提供内核级服务？
- 是否协调硬件资源？
- 是否处理异常与并发？

OP-TEE 都涉及这些方面。

但是，OP-TEE 的实现不需要与通用操作系统一一对应。

## **1.2.1 对照 Linux、RTOS 和 OP-TEE**

| 维度    | Linux            | 传统 RTOS       | OP-TEE                    |
| ----- | ---------------- | ------------- | ------------------------- |
| 首要目标  | 通用计算和资源共享        | 实时性、确定性       | 隔离安全服务                    |
| 执行单元  | Process / Thread | Task / Thread | Trusted Thread、TA Context |
| 调度    | 自有调度器            | 自有调度器         | 经典模型与 Normal World 调度关联   |
| 用户态隔离 | 完整支持             | 依实现而定         | User TA 与 Core 隔离         |
| 虚拟内存  | 完整 VM 子系统        | 可选            | Core MMU、TA VM、MOBJ 等     |
| 文件系统  | 多种通用 FS          | 可选            | Trusted Storage 抽象        |
| 驱动模型  | 通用设备管理           | SoC/系统相关      | 面向安全服务的设备管理               |
| 服务接口  | Syscall、IPC      | API、IPC       | GP API、Syscall、Session    |
| 信任假设  | 内核通常可信           | 依系统而定         | 明确考虑不可信 Normal World      |
| 运行目标  | 独立管理系统           | 控制嵌入式任务       | 保护安全计算和敏感资产               |

这里最值得注意的是：

**一个 OS 并不一定需要实现 Linux 风格的通用调度器。**

OS 的核心在于提供执行环境、资源管理、保护机制和系统服务。调度策略只是其中的一种设计选择。

## **1.2.2 传统 OS 与 OP-TEE 的执行模型差异**

Linux 的线程执行模型，大体是：

```
Linux Scheduler
       |
       +-- Thread A
       |
       +-- Thread B
       |
       +-- Thread C
       |
       +-- Thread D
```

Linux Kernel 主动管理这些线程的调度。

但是，经典 OP-TEE 的模型更接近：

```
Linux Task A
    |
    | Invoke TEE Service
    v
Trusted Thread 0
    |
    | Execute Secure Service
    v
Return to Linux Task A
```

OP-TEE 会为 Yielding Service 分配 Trusted Thread。遇到需要交给 Normal World 处理的事件或 RPC 时，该 Trusted Thread 可以挂起，待 Normal World 返回后恢复执行。

官方文档明确指出，经典 OP-TEE 并没有实现独立的通用线程调度器，而是让 Trusted Thread 的执行与对应调用方的 Linux 调度关联。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



我们后面会深入分析：

- Trusted Thread 为什么需要 Stack。
- 为什么需要 `CFG_NUM_THREADS`。
- 为什么需要 Suspend 和 Resume。
- 为什么一个 Suspended Thread 可以长期保留执行上下文。
- 为什么 Trusted Thread 并不等于 TA Context。

现在先理解架构动机。

## **1.2.3 为什么 OP-TEE 不设计一个独立调度器？**

假设我们给 OP-TEE 加入一个完整的、支持时间片和优先级的调度器。

从技术上看完全可以设计。

但它会带来新的责任：

- 自己管理 Ready Queue、Blocked Queue、Timer。
- 自己决定何时发生抢占。
- 处理 Secure Interrupt 和异步唤醒。
- 实现 Priority Inversion 的处理。
- 管理更多独立运行的安全任务。
- 建立自己的任务调度策略与公平性机制。
- 与 Linux 调度和电源管理进行协调。
- 处理安全任务长期独立执行所带来的资源占用和可用性问题。

这会增加代码、状态、并发关系和维护成本。

对于大量请求驱动的安全服务，例如：

```
OpenSession
InvokeCommand
CloseSession
```

这些服务通常天然具有：

```
Request → Execute → Return
```

的生命周期。

因此，可以利用调用方的执行上下文，而不必重新实现一套完整的线程调度系统。

这是一种具有明确代价收益的架构选择。

### **但是，这种设计也有代价**

例如，假设一个 TA 需要执行持续数分钟的后台计算，而且不希望依赖某个 Linux 进程继续调用它。

经典 OP-TEE 的执行模型就不一定适合这个需求。

我们必须考虑：

- 是否改为请求驱动、分段执行？
- 是否引入异步事件和 Notification？
- 是否由独立的 Secure Partition 处理？
- 是否应该使用专用安全处理器？
- 是否根本不应该把这项任务放在 OP-TEE 中？

不能简单得出“OP-TEE 缺少调度器，所以给它加一个调度器”的结论。

### **本节需要掌握的设计思想**

**操作系统架构不应该追求功能越完整越好，而应该追求能够满足目标需求的最小、清晰、可验证的机制集合。**

# **1.3 OP-TEE 的职责边界：它是安全服务执行系统，而不是整个系统的所有者**

为了理解这一点，需要把完整 SoC 软件系统分层。

典型 Arm SoC 的 OP-TEE 系统边界

Normal World

Linux Apps · libteec · Linux Kernel · tee-supplicant

Standard TEE Communication

OP-TEE Secure World

OP-TEE Core

Trusted OS Kernel · PTA

User TA

Unprivileged Secure Service

Trusted Firmware Interface

Higher-privilege Firmware / Monitor

Typical Armv8-A: TF-A EL3 · World Switch · Platform Controls

本图表示一种典型 Armv8-A 配置，不代表所有平台。RISC-V 的 OpenSBI、Domain 管理和平台安全隔离需要另行映射其权限模型。

这里有一个非常重要的设计原则：

**OP-TEE 不一定是系统中权限最高的组件，也不应该负责所有安全硬件功能。**

在典型 Armv8-A 系统中：

- EL3 Firmware 负责更高特权级的安全控制和世界切换。
- OP-TEE 运行于 Secure EL1，提供 TEE Kernel 和安全服务。
- User TA 在更低特权级运行。
- Linux 负责 Normal World 的进程、设备和通用服务。

但是不同 SoC、FF-A/SPMC 配置或 RISC-V 安全架构，可能有完全不同的权限安排。

因此，需要严格区分：

| 概念                  | 含义                    |
| ------------------- | --------------------- |
| Hardware Isolation  | 硬件阻止未经授权的访问           |
| Firmware Privilege  | 更高特权 Firmware 拥有哪些控制权 |
| TEE OS              | 管理安全应用与服务的受保护执行环境     |
| Trusted Application | 在 TEE 中实现特定业务逻辑       |
| Root of Trust       | 作为认证、测量或密钥信任起点的组件或资产  |

它们可能相互协作，但不能被当作同一个概念。

## **1.3.1 一个很重要的反例：TEE 并不等于 Root of Trust**

假设某 SoC 内部有一个独立的 RSS/HSM Core。

RSS 负责：

- Secure Boot。
- Firmware Authentication。
- Root Key Management。
- 平台生命周期状态管理。

而 OP-TEE 运行在应用处理器的某个受保护 Domain。

那么即使 OP-TEE 是一个完整的 Trusted OS，它也不一定应该直接掌握 Root Key。

合理的架构可能是：

```
OP-TEE User TA
      |
      v
OP-TEE Security Service
      |
      v
Authenticated Firmware Protocol
      |
      v
RSS / HSM
      |
      v
Non-exportable Root Key
```

这种情况下：

- RSS 是某些信任链的根。
- OP-TEE 是受保护的服务执行环境。
- TA 是具体安全业务的实现。
- Root Key 可能始终不离开 RSS。

这是职责分离，而不是架构缺陷。

实际上，把所有安全功能集中到一个拥有最大权限的软件系统，往往反而会扩大攻击面。

# **1.4 OP-TEE 的 Trust Model：真正的核心是什么？**

本章最需要深入理解的内容是 Trust Model。

假如不清楚系统到底信任谁，就无法严肃地讨论安全性。

## **1.4.1 Trust 不等于 Secure World**

一个非常容易出现的误解是：

> 只要代码运行在 Secure World，它就是可信的。

这个说法不严谨。

Secure World 描述的是硬件保护和执行状态，不等于其中每个软件组件都不存在漏洞。

例如：

- 一个 User TA 可能存在 Buffer Overflow。
- 一个 PTA 可能存在错误的访问控制。
- 一个 Crypto Driver 可能错误地处理 DMA 地址。
- 一个安全存储服务可能存在资源泄露。
- 一个高权限 Firmware 可能存在安全缺陷。

因此，Trust Model 应该从以下问题建立：

1. 系统有哪些需要保护的资产？
2. 谁被授权访问这些资产？
3. 谁可能被攻击者控制？
4. 哪些机制阻止非授权访问？
5. 哪些组件一旦被攻破，会导致保护失效？

## **1.4.2 OP-TEE 的典型信任关系**

| 组件或资源                                   | 基本信任定位          | 需要关注的问题                  |
| --------------------------------------- | --------------- | ------------------------ |
| Secure Boot / Higher-privilege Firmware | TCB 的基础组成部分     | 是否正确认证、配置隔离              |
| OP-TEE Core                             | TCB 核心          | Kernel 漏洞可能破坏整体隔离        |
| User TA                                 | 有限权限的受保护执行单元    | 仍需防范漏洞和越权                |
| PTA                                     | Core 特权级服务      | Bug 直接扩大内核风险             |
| Linux Kernel                            | 默认不信任           | 可以发送恶意输入和控制 REE 资源       |
| `tee-supplicant`                        | Normal World 服务 | 响应不可信、服务可能中断             |
| Shared Memory                           | 不可信输入/交换媒介      | 越界、篡改、并发修改               |
| Secure-only Memory                      | 依赖硬件保障的可信资源     | 必须验证 SoC 隔离配置            |
| Hardware Crypto / RNG                   | 依平台安全保证信任       | Key Slot、DMA、Entropy 可靠性 |

官方 OP-TEE Security Scope 明确说明：Core 信任安全侧更高特权级的软件、相关安全硬件接口及分配给它的可信内存，同时把跨隔离域的机密性、完整性和可用性破坏作为安全问题的重要判断依据。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



需要额外注意，官方安全漏洞受理范围与我们进行系统威胁建模时的范围并非完全相同。例如，上游当前的安全受理政策认为有签名授权的恶意 TA 涉及既定信任边界，但工程设计仍然需要尽可能降低已签名 TA 因 Bug 造成横向影响的风险。

## **1.4.3 关键概念：Trust Boundary 与 Isolation Boundary**

两者不能混为一谈。

**Trust Boundary** 是对哪些主体、数据或行为可以被依赖的信任划分。

**Isolation Boundary** 是限制一个主体可以访问哪些资源的技术约束。

例如，Normal World Linux 与 OP-TEE Core 之间存在明显的信任边界。

但在 Secure World 内部，User TA 与 OP-TEE Kernel 之间同样应该存在权限隔离。

```
Untrusted Linux
      |
      | Trust Boundary A
      v
OP-TEE Entry / Input Validation
      |
      v
OP-TEE Kernel
      ^
      | Privilege / Isolation Boundary B
      v
Unprivileged User TA
```

我们不能因为 User TA 运行在 Secure World，就允许它读取任意 Secure Kernel 内存。

这正是 OP-TEE 要实现 User TA 与 Kernel 权限隔离的原因。

## **1.4.4 理解 Confused Deputy**

这也是 Staff Engineer 必须掌握的安全架构问题。

假设 OP-TEE Core 拥有访问 Secure Key 的权限，而 Linux 没有。

Linux 向一个 PTA 发起请求：

```
Linux requests:
    "Sign this buffer using secure key #3"
```

PTA 只检查：

```
if (key_exists(key_id))
    sign_with_key(key_id, message);
```

表面看来，没有将私钥泄露给 Linux。

但如果 Linux 本不应该使用 `key_id = 3`，那么这个 PTA 就成了 **Confused Deputy（被误导而滥用权限的代理）**。

攻击者没有直接访问 Key 的权限，却诱导一个有权限的组件替其完成操作。

正确的安全问题应该是：

- 请求者是谁？
- 这个身份由谁认证？
- 请求者被授权使用哪一把密钥？
- 允许执行哪些操作？
- 是否绑定用途、算法、调用上下文和平台状态？
- 权限检查应该发生在哪一层？
- 请求在校验后是否可能被修改？

因此：

**安全不是“秘密没有被读取”这么简单。安全还要求高权限组件不能被低权限主体诱导执行未经授权的行为。**

这会成为后续研究 PTA、Identity、Session、Crypto 和 Storage 时的核心原则。

# **1.5 Trusted Computing Base：为什么 TCB 的大小如此重要？**

现在我们讨论 OP-TEE 架构设计中最重要的一条原则：

**Minimize the Trusted Computing Base。**

## **1.5.1 TCB 到底是什么？**

TCB（Trusted Computing Base）不是一个特定的软件模块，而是实现某项安全目标时必须信任的硬件、固件和软件机制的集合。

举个例子。

我们希望证明：

> Linux Kernel 即使被攻破，也无法读取 OP-TEE 内部保存的私钥。

要让这条安全保证成立，至少需要依赖：

- Boot Firmware 正确初始化系统安全状态。
- CPU 特权级和内存隔离机制正确工作。
- Secure Memory 不被 Normal World CPU 访问。
- DMA Master 不能绕过内存隔离。
- OP-TEE Kernel 不会错误地映射或泄露私钥。
- Crypto/Storage 相关实现正确处理密钥。
- 平台安全配置不能被 Linux 随意修改。

这些机制都属于该安全目标的信任依赖。

注意：**TCB 是相对于安全目标定义的。**

如果我们的安全目标是“私钥不能被导出”，而私钥始终保存在独立 HSM 内，那么 HSM 固件、密钥访问策略和通信认证就非常重要；OP-TEE 的角色和所需信任程度，则取决于它是否有能力滥用该私钥以及 HSM 是否独立实施授权。

这意味着 TCB 不应该简单理解成“所有 Secure World 代码”。

## **1.5.2 TCB 越大，安全性为什么可能越差？**

假设我们设计两个方案。

方案 A：大 Kernel

Secure Kernel

Key Service

Certificate Parser

Network Protocol

Filesystem Logic

Business Policy

Hardware Driver

大量业务代码与协议解析均在高权限内核中执行

方案 B：小 Kernel

Secure Kernel

Memory · Identity · Crypto Driver · Minimal API

User TA

Certificate Parser · Business Policy

Normal World: Non-sensitive Services

方案 A 看起来很方便：

- 所有代码都能够访问 Kernel API。
- 不需要频繁执行 User/Kernel Transition。
- 无需复杂的 Syscall 接口。
- 开发时似乎更容易共享各种内部对象。

但它的安全缺点非常明显。

例如，Certificate Parser 有一个越界写入漏洞。

在方案 A 中，该漏洞可能直接影响 Secure Kernel 的内存和控制流。

而方案 B 中，如果 Certificate Parser 被放在具备适当隔离的 User TA 内，越界写入首先受到 TA 自身地址空间和权限机制约束。

它当然仍然可能危害这个 TA，但攻击者不应该因此自动获得 Core Memory 或其他 TA 的访问权限。

这就是 **Fault Containment（故障遏制）**。

需要注意：减少 Kernel 代码量只是降低风险的一种手段，不能简单地用代码行数衡量实际安全性。高权限驱动的 DMA 行为、Syscall 接口设计以及 TA 的权限配置同样决定隔离是否真正有效。

## **1.5.3 一个更深层次的问题：功能放在 User TA，是否就一定不属于 TCB？**

不一定。

假设 User TA 保存一把关键私钥，负责设备身份认证。

即使它运行在低权限模式，它自身的代码仍然是该私钥安全目标的关键依赖。

所以：

- 将代码移出 Kernel，可以减少 Kernel 权限范围和故障传播。
- 但不能自动消除该代码对业务安全目标的影响。
- 如果 User TA 拥有敏感密钥，它仍然需要完整的安全审查。

真正值得追求的不是简单的“减少 Trusted Code”，而是：

> 尽可能减少每个组件拥有的权限，以及一个组件失效后能够影响的安全资产范围。

这种设计方式可称为 **Compartmentalization（分舱隔离）**。

## **1.5.4 从设计原则推导 OP-TEE 的模块组织**

这也是我们后续学习各个子系统时要始终关注的问题。

| 子系统                | 为什么要存在于 Core  | 如何限制影响范围                  |
| ------------------ | ------------- | ------------------------- |
| Thread Manager     | 需要控制核心执行状态    | 不向 User TA 暴露内部 Thread 对象 |
| Memory Manager     | 需要管理安全映射和访问权限 | User TA 只能操作授权的内存         |
| TA Manager         | 负责认证、加载和隔离 TA | 分离 TA Context、Session 和资源 |
| Syscall Dispatcher | 提供跨权限边界服务     | 严格验证参数与调用身份               |
| Crypto Framework   | 需要访问安全能力和驱动   | 限制密钥与 Operation 的访问       |
| Driver Framework   | 访问特权设备和 MMIO  | 控制设备权限及 DMA               |
| Storage Framework  | 维护受保护数据的安全语义  | 对不可信 Backend 验证完整性        |

这里的核心思想是：

**Core 可以拥有最高权限，但不意味着其对外接口应该直接暴露这种权限。**

# **1.6 OP-TEE 的六项核心设计原则**

接下来我把前面的讨论收敛成六条可以用于实际架构设计和 Code Review 的原则。

## **原则一：Least Privilege — 最小权限**

任何组件只获得完成其职责所需要的权限。

例如，一个负责解析证书的 TA：

- 可能需要读取证书数据。
- 可能需要执行证书链验证。
- 可能需要调用安全 Crypto API。

但它通常不应该拥有：

- 修改任意 Secure Page Table 的权限。
- 访问所有 Secure MMIO 的权限。
- 读取其他 TA 私有内存的权限。
- 直接读取 Device Root Key 的权限。
- 更改系统隔离策略的权限。

架构设计时，应该先明确允许的操作，而不是先授予一个很大的权限集合，再尝试用代码约束其行为。

## **原则二：Explicit Trust Boundaries — 明确信任边界**

任何跨信任边界的数据都应按照其来源进行验证。

例如：

```
Linux
  |
  | Shared Memory
  v
OP-TEE
```

不能因为 Buffer 已经映射到 Secure World，便认为它自动变成了可信数据。

假设 Linux 提供：

```
struct request {
    uint32_t operation;
    uint32_t key_id;
    uint64_t buffer_size;
};
```

即使 `request` 已经被映射到 OP-TEE 虚拟地址空间，它也仍然可能被 Linux 修改。

如果 OP-TEE 第一次读取 `buffer_size` 进行验证，然后稍后再次读取它进行 Memory Copy，攻击者就可能在两次读取之间修改该字段。

这是一类经典 TOCTOU 问题。

一种正确的设计思路是：

1. 将请求所需的标量元数据复制到 Core 控制的私有内存。
2. 验证字段、大小及其相互关系。
3. 后续逻辑使用经过验证的私有副本。
4. 对仍然指向共享内存的 Payload，根据操作语义选择 Snapshot、复制或专门的共享内存同步协议。

但是不能机械地认为“复制一次就解决所有 TOCTOU”。例如，设备 DMA、异步 RPC 或共享 Buffer 的后续修改仍然需要专门处理。

## **原则三：Defense in Depth — 纵深防御**

不要让一项安全属性完全依赖单一检查。

例如，为防止 Linux 访问私钥：

```
Hardware Memory Isolation
            +
OP-TEE Kernel/User Separation
            +
TA Identity / Authorization
            +
Key Usage Restrictions
            +
No-export Key Handling
```

这些机制在不同层次保护同一类安全资产。

但纵深防御必须关注机制是否真正独立。

如果两项权限检查都信任一个由 Linux 自行填写且未经认证的身份字段，那么它们可能存在同一个共同失效点。

## **原则四：Fail Securely — 安全失败**

安全失败与普通功能失败不同。

举例：

OP-TEE 在 Boot 时无法获取真正的 Hardware Unique Key（HUK）。

我们可以设计两个策略：

**策略 A：**

```
HUK unavailable
    |
    v
Use hardcoded development key
    |
    v
Continue Boot
```

**策略 B：**

```
HUK unavailable
    |
    v
Disable affected secure services
    |
    v
Report security initialization failure
```

在开发环境中，策略 A 可能有其便利性。

但在生产环境中，固定密钥可能破坏设备间的密钥隔离，使多台设备的存储数据面临共同的安全风险。

因此，在没有经过明确威胁评估和产品策略定义的情况下，不应该静默地将关键安全能力降级。

OP-TEE Secure Storage 文档也专门指出，部分平台不能正确获取 HUK 时可能采用固定密钥，导致设备绑定和数据保护失去预期安全性。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



但是 Fail Securely 并不等于所有错误都必须让系统 Panic。

更合理的策略是根据安全影响范围决定：

- 仅拒绝当前请求。
- 禁用当前 TA。
- 禁用某项安全服务。
- 将特定资源隔离并停止使用。
- 在无法保证核心安全不变量时停止整个 Trusted OS。

## **原则五：Minimize Trusted State — 减少必须维护的可信状态**

这一点对 OS 设计特别重要。

假设一个服务需要管理三类状态：

- 请求临时数据。
- Session 数据。
- 长期持久化数据。

不应该全部放在全局静态变量中。

原因是每种状态的生命周期不同。

```
Request State
   |
   +-- Exists during one invocation

Session State
   |
   +-- Exists from OpenSession to CloseSession

Persistent State
   |
   +-- Survives TA and system restarts
```

若将三者混为一谈，容易产生：

- 不同客户端的状态串扰。
- 错误地复用先前会话中的认证结果。
- Session 结束后敏感数据没有释放。
- 并发访问中的数据竞争。
- 系统重启后的状态不一致。

因此，良好的系统设计应明确：

**State 的 Owner、Lifetime、Scope、Synchronization 和 Destruction。**

这五个维度会成为后续分析每个 OP-TEE 数据结构时的固定问题。

## **原则六：Separate Mechanism from Policy — 机制与策略分离**

这一点在 OP-TEE 中非常有价值，但并不是所有安全规则都能完全独立于机制。

例如：

- Memory Manager 提供建立安全映射的机制。
- Identity Framework 提供识别调用方的机制。
- Crypto Framework 提供加密计算的机制。
- 应用服务定义谁能使用哪些密钥的业务策略。

假设我们实现一个设备身份认证 TA：

不应该把业务规则直接硬编码到通用 `core_mmu`、`tee_ta_manager` 或 Crypto Primitive 的内部。

否则，任何业务策略调整都可能导致核心内存或执行管理代码发生变化。

更合理的设计是：

```
Business Security Policy
           |
           v
Authenticated Service Interface
           |
           v
OP-TEE Generic Mechanisms
           |
           v
Hardware / Software Backend
```

当然，有些强制策略必须由 Core 实施，例如 TA 不能访问任意 Core Memory；如果策略的执行者可以被攻击者绕过，它就不能保护相应边界。

所以，正确的问题不是简单地问“策略是否应放在 TA”，而是：

> 哪一层具有可靠执行该策略所需的权限、身份信息和安全机制？

# **1.7 一个最容易被误解的问题：为什么 OP-TEE 可以依赖不可信的 Linux？**

我们已经反复强调，Normal World Linux 在 OP-TEE 的核心安全模型中不受信任。

然而，实际系统中 OP-TEE 却经常需要使用 Linux 提供的资源：

- 加载 TA 文件。
- 访问普通文件系统。
- 使用 `tee-supplicant`。
- 获取某些平台服务或非安全时间信息。
- 进行存储设备操作。
- 接收 Normal World 分配的共享内存。

这看起来是不是非常矛盾？

答案是：

**不信任一个组件的正确性或诚实性，并不意味着不能使用它提供的功能。**

关键在于：哪些结果必须被验证，以及哪些安全属性无法由 OP-TEE 单独保证。

## **1.7.1 一次 TA 加载**

假设 User TA 文件保存在 Linux 文件系统中：

```
/lib/optee_armtz/<UUID>.ta
```

Linux 可以：

- 删除文件。
- 替换文件。
- 修改文件内容。
- 拒绝返回文件。
- 返回错误数据。
- 返回旧版本文件。

但对于经过适当签名的 REE FS TA，OP-TEE 会在使用前验证其真实性与完整性。

因此，Linux 可以控制数据的传递，却不能仅凭修改文件内容就让未经授权的 TA 代码得到认证。

官方文档说明，REE FS TA 可以被签名、可选加密，并由 `tee-supplicant` 传递给 Secure World 进行验证和加载。文档也特别区分签名验证与 Rollback Protection：有效的版本降级防护还依赖相应安全存储配置。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



这里形成了重要的架构模式：

```
Untrusted Storage
       |
       | Bytes
       v
Trusted Authentication
       |
       | Verified Image
       v
Trusted Execution
```

但是需要避免一个错误结论：

> 只要 TA 有签名，就一定可以安全地执行。

签名只能证明文件满足相应的认证策略，不能证明 TA 代码不存在漏洞，也不能证明签名密钥本身没有被滥用。

## **1.7.2 一次 Secure Storage 写入**

假设 TA 要保存一个私密数据对象。

OP-TEE 可能通过 `tee-supplicant` 把加密后的数据写入 Linux Filesystem。

Linux 可以看到文件操作，甚至修改密文。

但是，如果数据经过正确的加密和完整性保护，Linux 不应能够在不掌握密钥的情况下恢复明文，或构造一个被 OP-TEE 接受的任意伪造数据版本。

不过，**Confidentiality、Integrity 和 Freshness 是不同的安全属性。**

举个例子：

```
Day 1: Secure Object = Balance 100
Day 2: Secure Object = Balance 200
```

Linux 保存了 Day 1 的旧文件，并在 Day 2 之后替换当前文件。

如果旧文件本身具有合法的认证标签，那么普通完整性校验并不能证明它是最新版本。

这是 Rollback Attack。

所以：

- 加密可以保护机密性。
- 完整性认证可以检测未经授权的内容修改。
- Anti-Rollback 还需要可信的版本、计数器或状态锚点。

官方存储文档明确区分了 REE FS 与具有 Replay Protection 能力的 RPMB 后端。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **1.7.3 为什么不能保证 Linux 一定提供服务？**

这里涉及 OP-TEE 的一个重要限制：Availability。

假设 Linux 被攻击者完全控制。

攻击者让 `tee-supplicant` 永远不响应一次 RPC。

OP-TEE 可以：

- 检测部分错误。
- 执行超时处理。
- 清理相关资源。
- 拒绝或终止请求。

但它无法凭空强制一个已被攻击者控制的 Linux 服务诚实地执行请求。

同理，如果经典 OP-TEE 的执行依赖调用方 Linux Task 被调度，Linux 也可能故意不调度这个 Task。

于是我们得到下面的威胁模型结论。

| 安全属性            | OP-TEE 能够提供的保证              | 重要前提或限制              |
| --------------- | --------------------------- | -------------------- |
| Confidentiality | 保护安全内存和正确管理的密钥              | 硬件隔离、Core 和相关 TCB 可信 |
| Integrity       | 验证 TA、数据与服务参数               | 认证策略和密钥管理正确          |
| Authorization   | 控制调用方可使用哪些服务                | 身份来源可信、策略实现正确        |
| Isolation       | 限制 User TA、Kernel 和 REE 的访问 | 隔离机制配置正确             |
| Anti-Rollback   | 在具备可信状态锚点时防止降级              | 需要计数器或防重放存储等支撑       |
| Availability    | 可以进行局部错误恢复和资源控制             | 不能保证恶意 REE 一定调度或响应   |

这张表非常重要。

我们应该习惯为每一项安全保证写出它的**前提条件**。

安全设计中最危险的错误之一，就是把“系统希望实现的安全属性”当成“架构已经无条件保证的事实”。

# **1.8 为什么要区分 Secure Kernel、PTA、User TA 和 Normal World？**

这是本章需要形成实际设计判断的地方。

假设你要为 OP-TEE 新增一个安全功能：

> 提供证书链校验服务，支持访问设备身份密钥，并返回一个经过签名的认证结果。

应该把这个功能放在哪里？

在回答之前，先弄清各类实现的真实区别。

## **1.8.1 四种常见实现位置**

| 实现位置         | 典型权限与隔离                 | 主要优势         | 主要代价          |
| ------------ | ----------------------- | ------------ | ------------- |
| OP-TEE Core  | Secure Kernel Privilege | 可直接访问内核与底层资源 | TCB 和内核攻击面增加  |
| PTA          | 与 Core 同等特权             | 易于暴露内核安全服务   | 业务漏洞可能影响 Core |
| User TA      | 低于 Core 的执行权限           | 更好的权限和内存隔离   | 需使用受控 API     |
| Normal World | 不在 TEE 信任域内             | 丰富库、资源和系统服务  | 不适合直接保管高价值秘密  |

这里有一个关键事实：

**PTA 不应该被理解为一种独立的低权限 TA。**

官方文档把 PTA 描述为 OP-TEE Core 暴露的服务接口；PTA 实现在 Core 中，具有与 Core 相同的执行特权。相比之下，User TA 通常在较低 CPU 特权级运行，并通过 `libutee` 使用受控系统调用访问 Core 服务。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



这是两种非常不同的安全设计。

## **1.8.2 场景一：新增一个证书解析器**

假设这是纯粹的 X.509 证书解析功能。

解析过程可能涉及：

- ASN.1 DER Parsing。
- 长度字段与嵌套结构。
- 复杂的 Extension。
- 不可信证书输入。
- 大量分配和释放。
- 较复杂的第三方代码库。

这类代码最容易暴露复杂输入处理漏洞。

因此，一般不应该因为它属于“安全业务”，就直接放进 PTA。

更合理的选择可能是：

- 放在 User TA 中。
- 如果不需要保密的解析过程，可以考虑放在 Linux 中，但安全侧必须验证自己最终依赖的数据和验证结果，不能无条件信任 Linux 给出的“证书有效”结论。
- 如果必须提供内核服务，则将高权限部分压缩为最小接口。

## **1.8.3 场景二：读取硬件 HUK**

假设只有 OP-TEE Core 能访问 HUK 所在硬件寄存器。

如果设计成：

```
TEE_Result get_device_huk(void *output, size_t len);
```

并允许任意 User TA 直接读取完整 HUK，那么即使实现放在低权限 TA 中，仍然破坏了 Root Key 的保护目的。

问题不在于函数有没有越界检查，而在于 API 本身赋予了过大的权限。

更合理的 API 可能是：

```
TEE_Result derive_service_key(
    uint32_t purpose,
    const void *context,
    size_t context_len,
    key_handle_t *handle);
```

这里是概念性 API，不代表 OP-TEE 已存在这一接口。

重点是：

- 不直接导出 Root Key。
- 使用 Key Derivation。
- 将密钥使用范围绑定到 Purpose 和 Context。
- 按调用方身份限制可访问的 Key Domain。
- 返回受约束的 Key Handle，而不是裸密钥。
- 在真正执行 Crypto Operation 时再次实施授权。

这些策略中的一部分必须由控制密钥的可信组件执行，不能仅依赖不可信调用方遵守。

## **1.8.4 场景三：实现硬件 Crypto Engine Driver**

假设硬件加速器只能通过 Secure MMIO 访问。

那么底层硬件控制通常需要 Core 或相应特权组件参与。

但不能因此得出“全部 Crypto Business Logic 都应该放进 Core”。

可以设计：

```
User TA
   |
   | Cryptographic Request
   v
TEE Internal API
   |
   v
Kernel Crypto Framework
   |
   v
Secure Hardware Driver
   |
   v
Crypto Accelerator
```

这种结构把：

- 业务逻辑。
- 密码操作抽象。
- 硬件驱动。
- 密钥访问策略。

分离成不同层次。

今后新增硬件加速器时，理论上就不需要修改所有上层 TA。

## **1.8.5 面对一个新功能，如何判断应该放在哪一层？**

可以使用下面的架构决策流程。

**New Security Feature**

**Q1：是否真的需要在受保护环境内执行？**

如果不需要保密状态、可信决策或不可由 REE 伪造的结果，优先考虑 Normal World。

**Q2：是否必须使用 Kernel-only 权限？**

如果不需要，优先考虑 User TA；如果需要，继续寻找能否通过最小接口隔离特权操作。

**Q3：它属于通用机制还是业务策略？**

通用机制适合沉入 Core；复杂业务策略尽量与 Core 分离。

**Q4：如果这个模块被攻破，会影响什么？**

评估 Kernel、其他 TA、Root Key、设备以及系统完整性的最坏影响。

**Final Placement Decision**

Normal World / User TA / Minimal PTA / Kernel Driver / Independent Security Processor

有时最好的答案不是四选一，而是把一个服务拆到不同层次。

例如：

```
Certificate Parsing       -> User TA
Device Identity Policy    -> User TA / Trusted Policy Service
Crypto Primitive          -> Crypto Framework
Hardware Key Operations   -> Kernel Driver or HSM
Filesystem I/O            -> Normal World
```

这种设计通常比将整个系统做成一个大型 PTA 更容易维护和审查。

# **1.9 从一次真实安全服务调用，看 OP-TEE 的设计哲学如何落地**

前面讲述的都是架构原则。

现在我们用一个完整的安全服务案例，把这些原则串联起来。

## **1.9.1 场景：设备身份认证**

假设我们正在开发一个 RISC-V 服务器 SoC。

SoC 内部包含：

- Application Harts，运行 Linux。
- 一个受硬件隔离保护的 OP-TEE Domain。
- 一个独立 RSS/HSM，管理 Device Root Key。
- 一个硬件 Crypto Accelerator。
- 不可信的 Normal World Filesystem。

业务需求是：

**Linux 应用能够申请设备身份证明，但无法读取设备私钥，也不能伪造未经授权的设备身份证明。**

注意：这是一种架构分析场景，并不代表上游 OP-TEE 4.11.0 已经提供了这套特定 SoC 的完整实现。

先设计一种合理的系统：

**Linux Application**

Request Device Attestation

**Linux TEE Client / Driver**

Session · Parameters · Shared Memory

OP-TEE Secure Domain

**Trusted Service / User TA**

Validate Request · Authorization · Format Result

**Core Security Service**

Kernel-only HSM Interface · Resource Validation

**RSS / HSM**

Trusted Key Policy · Root Key · Signing Operation

下面按照实际执行流程分析。

### **Step 1 — Linux Application 发起请求**

Linux 调用：

```
TEEC_OpenSession(...);
TEEC_InvokeCommand(...);
```

这里涉及 GlobalPlatform TEE Client API，通过 Context、Session 和 Command ID 组织客户端与 TA 的交互。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



架构问题：

**Linux 是否有权决定自己是什么身份？**

不能简单地相信 Linux 请求里携带的某个 `client_id` 字段。

假设 Linux 已经被攻击者控制，那么 Linux 可以伪造任何普通用户态提供的身份信息。

这意味着：

- TEE 侧必须明确身份声明来源。
- 如果只依赖 Linux Kernel 报告进程身份，那么该身份对恶意内核不具备独立可信性。
- 对于必须抵御恶意 Linux 的密钥操作，应考虑由安全侧认证的凭据、HSM 实施的访问策略，或者其他独立可信的授权机制。

这里有一个容易忽略的区分：

**TA UUID 是服务目标标识，不是调用者身份的安全证明。**

### **Step 2 — OP-TEE 解析请求参数**

假设请求携带一个 Shared Memory Buffer。

OP-TEE 需要检查：

- 参数类型。
- 参数大小和 Offset。
- 地址和长度是否溢出。
- 对应内存是否允许被此服务引用。
- 是否允许在处理期间被其他主体修改。

这些检查不能简单地认为 Linux Driver 已经做过。

因为在此威胁模型下，Linux Driver 本身就属于不可信组件。

此时 OP-TEE 的角色不是“执行 Linux 提交的指令”，而是：

**将一个不可信请求转换为具有明确边界、参数和权限的安全操作。**

### **Step 3 — TA 进行授权决策**

User TA 可能需要验证：

- 请求中是否包含可信凭据。
- 凭据是否允许申请设备身份证明。
- 请求是否绑定特定 Challenge。
- 是否存在有效期或使用次数限制。
- 所请求的操作是否符合设备当前安全状态。

然而这里又有一个难点：

如果真正的授权策略只存在于 User TA 中，而 User TA 能够随意请求 HSM 为任何内容签名，那么 HSM 实际上依赖这个 TA 正确实施授权。

如果某项关键密钥的安全目标要求“即使 TA 代码受到攻击，也不能执行任意签名”，那么仅靠 TA 进行授权还不够。

此时应由 HSM 或其可信服务接口独立实施密钥用途限制。

这就涉及 **Policy Enforcement Point（策略执行点）** 的选择。

### **Step 4 — OP-TEE Core 调用 HSM**

Core 与 HSM 通信时，不应该提供一种完全开放的接口：

```
hsm_execute_arbitrary_command();
```

更合理的是定义有限的操作集合：

```
hsm_sign_device_attestation(...);
hsm_derive_service_key(...);
hsm_get_public_identity(...);
```

这些只是设计示例。

关键在于把对高价值安全资产的访问转化为用途明确的能力，而不是暴露所有底层权限。

需要进一步验证：

- HSM 能否认证请求确实来自授权的安全组件？
- 通信共享内存是否会被 Linux 或其他 DMA Master 篡改？
- Command 是否具备 Replay Protection？
- 请求失败或 HSM Reset 后如何处理未完成的操作？
- Root Key 是否始终保留在 HSM 中？

这些问题将直接决定系统安全性。

### **Step 5 — 返回签名结果**

假设最终得到了一个签名结果。

它可以返回到不可信 Linux，因为数字签名本身通常不需要保密。

但返回之前仍需考虑：

- 输出 Buffer 是否足够大。
- 是否会泄露未初始化的 Core Memory。
- 是否可能由于错误的长度计算发生越界。
- 错误码是否暴露不应该暴露的内部状态。
- 操作中分配的敏感临时数据是否已经安全释放。

这说明即使是最普通的返回路径，也涉及 Kernel Memory、Shared Memory 和 Security Boundary。

## **1.9.2 这个案例揭示了什么？**

一个看似简单的签名服务，实际横跨了多个层次。

| 层次           | 主要职责       | 应维持的安全不变量          |
| ------------ | ---------- | ------------------ |
| Linux        | 发起请求、传输数据  | Linux 不被默认信任       |
| TEE Entry    | 解析请求、确定边界  | 不接受未经校验的内存描述       |
| TA Runtime   | 管理调用与服务上下文 | 不跨 Session 泄露状态    |
| User TA      | 处理业务安全策略   | 不越权调用其他安全服务        |
| Core Service | 管理特权资源     | 不被诱导滥用特权           |
| HSM          | 实施密钥用途限制   | Root Key 不可非法导出或使用 |
| Return Path  | 返回数据、释放资源  | 不泄露内存、不留下错误状态      |

这就是本课程后续要反复研究的思想：

**真正的系统安全不是一个模块独立实现出来的，而是多个模块共同维持一组端到端的安全不变量。**

# **1.10 为什么 OP-TEE 的架构必须持续演进？**

OP-TEE 最初的核心使用场景主要围绕 GlobalPlatform TEE 服务、Trusted Application 和传统的 Secure/Normal World 通信。

但是，现代 SoC 的安全需求正在变化：

- 更多的 CPU Core 和复杂的 SMP 系统。
- 虚拟机与 Hypervisor。
- FF-A 和 Secure Partitions。
- UEFI Standalone MM。
- 独立 HSM / RSS / SCP。
- Confidential Computing。
- 更复杂的 DMA 和系统级内存隔离。
- 多个不同厂商的软件安全组件共享硬件。

上游 OP-TEE 文档已经包含 SPMC、Virtualization 等架构专题。例如，SPMC 实现涉及 Secure Partition 生命周期、FF-A 通信和内存管理，远不只是传统 TA Service 的另一种 API。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



这意味着 OP-TEE 的部分早期架构假设可能需要扩展。

## **1.10.1 最典型的变化：从单一 TEE 服务到多个安全隔离主体**

传统系统通常可以简化为：

```
Normal World
     |
     v
OP-TEE
     |
     v
User TAs
```

但现代平台可能包含：

```
Linux / Hypervisor
         |
         v
Secure Partition Manager
         |
         +-- OP-TEE Services
         |
         +-- Standalone MM
         |
         +-- Other Secure Partitions
         |
         +-- Platform Security Services
```

这会带来新的抽象需求：

- 不同 Endpoint 的身份。
- 不同安全组件的资源所有权。
- Secure Memory Sharing。
- 不同安全组件之间的消息传递。
- 安全组件独立的初始化和异常处理。
- 一个组件 Crash 后如何防止影响其他组件。

也就是说，现代 OP-TEE 的架构不再能够只用“Linux 调用 TA”这一个模型解释所有场景。

## **1.10.2 对 RISC-V Porting 的启示**

你之前重点研究过 RISC-V 上的 Domain Switch、OpenSBI、PMP 和 SMMPT。

现在需要把这些知识转换为 OS 级问题。

例如，一个新的 RISC-V TEE 实现可能需要：

```
Domain Manager
       |
       +-- Normal Domain
       |
       +-- OP-TEE Domain
       |
       +-- Other Secure Domains
```

但设计者不能只证明“Domain Switch 之后页表已经切换”。

还必须回答：

- OP-TEE 是否仍然可以管理多个互相隔离的 User TA？
- Trusted Thread 的执行上下文如何保存？
- Secure Interrupt 的所有权由谁管理？
- TA 对外部内存的引用如何约束？
- Domain Switch 是否可能保留旧的设备访问权限？
- OP-TEE 的 Trust Model 是否依赖某个新的高权限组件？
- Domain 管理协议能否表达 Core 需要的全部安全语义？

这里有一个非常重要的工程方法：

**移植 OS 时，不应该仅仅把架构相关函数替换为另一种指令实现，而应该先明确 OS 依赖的抽象契约，再判断新架构是否能够满足这些契约。**

例如：

```
OP-TEE Required Invariant:
    User TA cannot access Core private memory
```

在 Arm 和 RISC-V 中可能采用不同的 MMU、特权级或系统隔离机制。

但是无论底层机制如何变化，这项不变量都必须始终成立。

这就是 Architecture-Neutral Design 的真正价值。

# **1.11 如何用本章的思想分析一个真实的 OP-TEE Patch？**

虽然 Chapter 01 主要是架构理论，但我们仍然要建立最基础的源码研究习惯。

本章建议选择三个代码位置：

| 代码位置                           | 研究目标                     |
| ------------------------------ | ------------------------ |
| `core/kernel/tee_ta_manager.c` | 谁管理 TA Context 和 Session |
| `core/pta/`                    | PTA 为什么属于 Core 特权服务      |
| `core/tee/entry_std.c`         | 不可信请求如何进入安全服务            |

这次不要求逐行理解代码，只要求从架构角度回答问题。

## **实验 A：观察 TA Manager 的职责**

可以先获取固定版本的源码：

```
git clone --branch 4.11.0 --depth 1 \
    https://github.com/OP-TEE/optee_os.git

cd optee_os
```

然后：

```
rg -n 'tee_ta_open_session|tee_ta_close_session' \
    core/
```

继续观察：

```
rg -n 'tee_ta_mutex|tee_ta_ctx|tee_ta_session' \
    core/kernel/tee_ta_manager.c
```

现在不必完全理解代码，只思考：

1. 为什么 TA Context 不能直接等于 Session？
2. 为什么会有全局对象列表和锁？
3. 为什么打开 Session 可能涉及 TA 加载？
4. 为什么关闭 Session 不一定立即销毁 TA Context？
5. 如果 TA Manager 使用的全局锁出现死锁，会影响哪些安全服务？

注意最后一个问题。

它已经把普通的代码阅读转换成了系统级的影响范围分析。

## **实验 B：观察 PTA 的职责**

```
ls core/pta/
```

选择一个简单的 PTA，例如 `stats.c`。

思考：

- 它为什么需要运行在 Core 中？
- 如果把它实现成 User TA，会失去哪些能力？
- 它是否可能向 Normal World 暴露敏感的内核状态？
- 如果需要扩展一个新的统计接口，哪些字段应该被隐藏？
- 该功能是否应该在生产环境下启用？

重点不是 PTA 的 API，而是：

**即使只是一个调试或统计服务，只要它运行在高权限 Core 内，就必须按照内核级安全接口审查。**

## **实验 C：假设新增一个 Kernel API**

现在假设我们为 User TA 新增：

```
/* Conceptual example, not an upstream OP-TEE API */
TEE_Result TEE_ReadSecureRegister(
    uint32_t register_id,
    uint32_t *value);
```

它允许 User TA 请求读取某些 Secure MMIO Register。

请先不要写代码。

使用本章原则进行架构评审：

- 为什么 User TA 需要这个接口？
- 哪些 Register 可以访问？
- 是否存在读操作就会产生副作用的寄存器？
- 能否通过 Register ID 访问其他安全域？
- 是否应该直接返回原始寄存器值？
- 如果未来 SoC 版本变更 Register Layout，ABI 如何兼容？
- 是不是应该设计为语义明确的服务 API，而不是暴露裸寄存器？

例如，可能更适合定义：

```
TEE_Result TEE_GetDeviceSecurityState(
    device_security_state_t *state);
```

这只是示例。

它体现了机制与接口设计之间的区别：

**上层应该请求一个受授权的安全服务，而不是获得能够访问任意底层硬件资源的能力。**

# **1.12 本章的设计原则与安全不变量总结**

在进入 Staff Engineer 测试题之前，我建议把本章收敛为一张可以长期使用的架构判断表。

| 原则                                 | 核心问题            | 违反后的典型后果           |
| ---------------------------------- | --------------- | ------------------ |
| Clear Trust Model                  | 到底信任谁？          | 把恶意输入当作可信数据        |
| Least Privilege                    | 组件是否拥有过多权限？     | 局部漏洞导致全局失陷         |
| TCB Minimization                   | 哪些代码必须拥有特权？     | 内核攻击面不必要地扩大        |
| Isolation                          | 不同主体是否真正隔离？     | TA 间横向访问           |
| Explicit Ownership                 | 谁拥有并释放资源？       | UAF、泄露、Double Free |
| State Lifetime                     | 状态应该存在多久？       | 会话间状态泄露            |
| Secure Failure                     | 错误时还能保证什么？      | 静默降级和安全状态破坏        |
| Separation of Mechanism and Policy | 策略应该在哪里实施？      | 通用 Core 被业务耦合      |
| Untrusted Service Validation       | 能否安全使用不可信服务？    | TOCTOU、伪造结果        |
| End-to-End Security                | 所有依赖是否共同维持安全目标？ | 单一检查正确但整体仍不安全      |

下面用更具挑战性的题目检验是否真正理解这些原则。

# **1.13 Staff Engineer 级别综合测试题**

共 10 题 · 以系统设计、威胁建模、架构权衡、故障分析为核心

这些题目不要求机械背诵答案，也没有必要把所有问题简化为唯一正确的选项。

真正优秀的回答应该具备以下特征：

- 主动声明自己的假设与信任边界。
- 识别题目中可能存在的隐含前提。
- 能够提出多种方案并讨论代价。
- 明确系统必须维持的安全不变量。
- 能够给出验证或证伪自己方案的方法。

## **Question 01 — OP-TEE 是否真的需要作为一个 OS 存在？**

Architecture Design

**场景：**

你正在设计一款极简 IoT SoC，只有一个 CPU Core，运行 Linux。

唯一的安全需求是：Linux 可以申请使用一把设备私钥完成 ECDSA 签名，但不能读取私钥。

硬件已经提供了：

- Secure-only Key Storage。
- Secure Crypto Engine。
- 硬件权限控制。
- 一个独立的 Secure Monitor。

CTO 提出：

“我们真的需要引入完整 OP-TEE OS 吗？直接在 Secure Monitor 中实现一个签名接口不就行了？”

**请从设计原则出发，判断 CTO 的方案是否合理。**

需要考虑性能、TCB、扩展性、硬件隔离、攻击面、生命周期及未来维护成本。

参考分析方向

CTO 的方案不一定错误。

如果需求非常有限，而且可以用少量可审计代码完成认证、授权、加密操作和异常处理，一个小型安全固件服务可能比引入完整 TEE OS 更合适。

但不能只比较代码量，还需要分析：

- Monitor 的特权是否过高，把业务解析放进去会不会增加高特权 TCB。
- 后续是否需要多个独立安全服务或应用。
- 签名请求如何认证、限权、同步和取消。
- 是否需要安全存储、固件升级与独立服务生命周期。
- 安全固件是否支持严格的错误处理与资源管理。

一个优秀的架构方案可以选择最小安全服务，但应该同时定义未来何种需求增长会触发引入 TEE OS。

**最关键的判断：** 不应为了“完整性”而引入一个不必要的 OS，也不能因功能目前简单就忽略未来的安全边界和生命周期。

## **Question 02 — 如果 Linux 被完全控制，OP-TEE 到底还能保证什么？**

Threat Modeling

**场景：**

一个手机设备已经开启 Secure Boot，OP-TEE 正常运行。

攻击者成功取得了 Linux Kernel 的完整控制权。

现在设备通过 User TA 提供指纹认证、密钥签名和安全存储服务。

攻击者能做什么，不能做什么？

更难的问题：

如果该设备所有认证请求都来自 Linux，攻击者是否能够绕过用户身份验证，通过 TEE 获取合法签名？

参考分析方向

不应回答“OP-TEE 完全安全”或“Linux 被攻破后 TEE 也没有意义”。

正确分析需要划分不同资产和信任路径。

攻击者通常可以控制 Linux 发起的调用、参数、调用顺序、共享内存，以及 REE Storage 和 RPC 的行为，并可以影响服务可用性。

但在硬件隔离及 TEE Core 正确工作的前提下，攻击者不应因此直接读取安全内存或非导出私钥。

关键的进一步问题是：

**TEE 的授权判断是否拥有独立于恶意 Linux 的可信证据？**

如果 Linux 只是传入 `authenticated = true`，那么 TEE 对这个字段的信任毫无安全价值。

如果指纹匹配结果由可信传感器路径和受保护组件认证，且 TA/HSM 强制执行相应策略，那么攻击者可能无法伪造授权。

应区分读取私钥、使用私钥、重放合法授权、阻断服务和诱导高权限代理执行操作这几类完全不同的威胁。

## **Question 03 — 为了降低攻击面，是否应该把所有 PTA 都改成 User TA？**

Security Architecture

**场景：**

一次内部安全审计发现，公司的 OP-TEE 平台新增了约 20 个 PTA。

安全团队要求：

“PTA 都是 Kernel Privilege，为了尽可能减少 TCB，必须将所有 PTA 转换为 User TA。”

你作为 Staff Engineer 是否支持？

参考分析方向

不能机械地支持这个要求。

首先应审计每个 PTA 的职责：

- 是否访问 Kernel-only 状态。
- 是否配置 Secure MMIO。
- 是否管理 Memory Isolation。
- 是否是跨特权级服务的必要入口。
- 是否包含大量复杂的业务逻辑。
- 是否直接处理不可信输入。

对纯业务逻辑，迁移到 User TA 通常有明显隔离收益。

对必须使用 Kernel-only 资源的功能，正确方案往往不是简单迁移，而是拆分为：

```
User TA Business Logic
        |
        v
Minimal Privileged Interface
        |
        v
Kernel Mechanism
```

还应考虑拆分后新增的接口攻击面、数据复制、上下文切换和生命周期复杂度。

**高级答案应关注隔离收益是否大于新增复杂性，而不是只看 PTA 数量是否减少。**

## **Question 04 — 如果要为 OP-TEE 增加独立调度器，你如何评审？**

OS Architecture

**场景：**

某个新 SoC 需要 OP-TEE 处理一类长期运行的安全监控任务。

该任务必须在没有 Linux 主动调用的情况下持续处理安全事件。

一名工程师建议：

“我们可以把 FreeRTOS Scheduler 移植到 OP-TEE Core，这样 Trusted Thread 就能独立调度了。”

你如何评价？

参考分析方向

首先应该挑战需求，而不是立即设计 Scheduler。

必须明确：

- 是否真的需要持续后台执行，还是只需要中断驱动的周期性处理。
- 是否存在实时性或硬件响应期限要求。
- 是否应该由 Secure Partition、HSM 或独立控制处理器处理。
- 任务是否需要与普通 TA 共享资源。
- 新 Scheduler 是否会影响既有 Trusted Thread 和 RPC 模型。
- 是否需要为 Timer、Preemption、Priority、CPU Affinity 建立新的状态管理。

增加 Scheduler 可能要求重新审查锁、Stack、Thread State、Interrupt Handling、Suspend/Resume 以及调试机制。

它不只是加入一个 Ready Queue。

也不能绝对排斥独立调度器：对于真正具有独立安全实时需求的平台，它可能是合理的架构选择。

**最佳答案应先给出替代方案与边界，再判断是否值得改变 OP-TEE 的核心执行模型。**

## **Question 05 — TCB Minimization 是否可能反而降低安全性？**

Architecture Trade-off

**场景：**

团队提出一个优化：

将原本在 OP-TEE Core 中进行的部分安全验证逻辑迁移到 Linux，以减少 Secure Kernel 的代码量和执行时间。

迁移后，Linux 提前完成参数解析和完整性验证，OP-TEE 只负责调用 Crypto Engine。

团队声称：

“我们减少了 TCB，所以安全性提高了。”

这个结论成立吗？

参考分析方向

**不一定成立。**

如果迁移出去的是不影响最终安全决策的预处理逻辑，可以降低 Core 的代码复杂度。

但如果将关键授权验证或完整性检查完全交给恶意 Linux，虽然 Core 的代码量减少，实际安全保证却可能已经消失。

应区分：

- 可以外包的计算。
- 可以外包但必须验证结果的计算。
- 必须由可信组件独立实施的安全决策。

一个很好的例子是密码学签名验证。

Linux 可以帮助解析证书、构造证书链或预处理数据，但如果 OP-TEE 的安全决策依赖签名有效性，那么最终必须拥有可信的验证结果及其上下文约束。

这道题的核心是：

**TCB 的真正目标是最小化必须被信任的依赖，而不是机械地最小化 Secure Kernel 的代码行数。**

## **Question 06 — 从安全角度，如何设计 Zero-Copy Shared Memory？**

Memory Security

**场景：**

一个 OP-TEE Crypto Service 需要处理 64 MB 数据。

目前实现会将整个 Shared Memory Buffer 复制到 Secure Memory，再进行加密。

为了提升性能，工程师建议改为 Zero-Copy：

```
Linux Buffer
     |
     v
Shared Memory Mapping
     |
     v
Secure Crypto Driver
```

这样可以大幅减少内存复制。

但它是否安全？

请分析 CPU Access、DMA、Cache、并发修改和生命周期问题，并设计一个安全的 Zero-Copy Protocol。

参考分析方向

不能因为 Shared Memory 已经正确映射，就认定 Zero-Copy 安全。

首先要区分 Crypto Operation 的语义。例如，输入被 Linux 在执行过程中修改，是否只是导致计算结果变化，还是能够破坏权限判断或产生跨界访问？

需要分析：

- Buffer 地址、长度和物理页面的合法性。
- Memory Ownership 是否可在执行期间改变。
- DMA Master 是否能够访问该地址。
- Linux 是否仍能同时写入 Buffer。
- Cache Coherency 是否正确。
- Buffer 是否可能在 DMA Completion 前被释放。
- Error/Cancel Path 是否会遗漏 Unmap 和权限撤销。
- 输出数据是否包含不应暴露的安全中间状态。

可选择的策略包括 Copy-In、固定并受控的共享页、严格的 DMA Mapping 生命周期、不可变输入契约或专门的 Buffer Ownership Transfer。

如果硬件无法禁止 Linux 修改某些共享页，那么设计必须明确这种并发修改被允许造成什么结果，不能声称仅凭软件检查就实现了独占所有权。

**优秀答案应给出一个完整的 Buffer 状态机，并指出状态转换的同步点。**

## **Question 07 — 将安全存储交给 Linux，是否违反 Zero Trust？**

Security Guarantees

**场景：**

团队计划在新 RISC-V 服务器芯片上实现 Secure Storage。

平台没有 RPMB，但有普通 NVMe SSD。

一名工程师提出：

“我们可以使用 AES-GCM 加密所有数据，再通过 Linux 写入 NVMe，因此我们的 Secure Storage 是安全的。”

你如何评估？

参考分析方向

首先应询问所承诺的安全属性究竟是什么。

AES-GCM 可以在正确使用密钥、Nonce 和认证机制的前提下提供机密性与完整性保护。

但它本身不能证明数据是最新版本。

攻击者可以保存旧的合法密文，并通过替换文件触发 Rollback。

因此，需要判断：

- 是否有可信 Monotonic Counter。
- 是否有受保护的 Root Hash 或版本元数据。
- 是否有独立 HSM/RSS 可维护可信持久状态。
- 是否允许在存储状态回退时拒绝服务。
- 掉电恢复和更新原子性如何实现。
- 如果可信计数器资源有限，如何批量更新或降低写入成本。

此外，不能把“Linux 拒绝服务”误判为 AES-GCM 失败。

**关键是按属性分别评估 Confidentiality、Integrity、Freshness、Atomicity 和 Availability，而不是笼统地宣布存储安全。**

## **Question 08 — RSS/HSM 与 OP-TEE 谁应该拥有 Root Key？**

System Security Design

**场景：**

你正在设计一个 RISC-V 服务器 SoC。

其中包含：

- 独立 RSS Core。
- 多个 Application Harts。
- OpenSBI。
- OP-TEE。
- Linux。
- 硬件 Crypto Engine。

现在要实现 Secure Boot、Device Identity、Key Derivation 和 Secure Storage。

团队内部有两种观点：

**方案 A：** RSS 只负责 Secure Boot，启动完成后把 Root Key 交给 OP-TEE。

**方案 B：** Root Key 永久留在 RSS，OP-TEE 只能通过受授权的命令请求密钥派生与相关操作。

请比较两种架构。

参考分析方向

如果 Root Key 是多个安全目标共同依赖的高价值资产，方案 B 往往具有更清晰的权限边界。

但需要继续分析：

- RSS 是否有足够的计算能力和服务吞吐量。
- RSS 是否能够独立认证 OP-TEE 的身份和请求上下文。
- 如何避免 OP-TEE 使用开放式 Derive 接口产生等效于 Root Key 的能力。
- 密钥操作是否需要强绑定 Purpose、Domain、VM 或设备生命周期。
- RSS Reset 和固件升级如何处理。
- RSS 通信协议是否有 Replay、TOCTOU、DMA 等风险。
- RSS 是否会成为单点性能瓶颈或系统可用性依赖。

方案 A 可能带来性能和实现便利性，但显著扩大 Root Key 的暴露范围。

方案 B 也不是无条件安全。如果 OP-TEE 可以向 RSS 发出任意签名或任意派生命令，密钥虽然没有被导出，但可能仍然可以被滥用。

**真正重要的不是密钥放在哪里，而是每个安全主体被赋予了什么能力，以及能力是否受到独立实施的策略约束。**

## **Question 09 — 如果 OP-TEE Kernel 发生 Panic，应该如何处理？**

Fault Containment

**场景：**

某个 Crypto Driver 因为非法 DMA Completion 状态，触发了一个 Kernel Assertion。

现在有两个产品需求：

- 安全团队希望系统立即停止，避免继续运行于未知安全状态。
- 产品团队希望尽量保持 Linux 可用，不影响用户业务。

请设计一个 Kernel Fault Policy。

参考分析方向

不应该预设所有错误都需要整体 Reset，也不能为了保持可用性而无条件继续执行。

需要区分故障的影响范围。

例如：

- 单个 TA 发生用户态异常，可以考虑仅终止其 Context。
- Crypto 请求超时，可能可以撤销资源并返回错误。
- DMA 状态未知但硬件可以可靠复位和隔离，可考虑禁用当前设备。
- Core Memory 被破坏、锁或资源所有权不再可信，可能已经无法安全恢复。

必须定义不可恢复状态的判据。

此外还要注意，安全恢复本身需要验证：

- DMA 是否真正停止。
- 敏感数据是否清除。
- 其他请求是否仍持有该设备引用。
- 是否存在未完成的 Key Operation。
- 是否存在攻击者能够反复触发的跨域 DoS。

优秀的方案应该具有分层错误处理策略，而不是只使用 `panic()` 或忽略错误这两个极端选项。

## **Question 10 — 如果重新设计一个现代版 OP-TEE，你会保留什么、改变什么？**

Principal / Staff Architecture

**场景：**

假设你要从零设计一套面向未来十年的 TEE OS。

平台目标包含：

- Arm 和 RISC-V。
- 多核处理器。
- 多个安全隔离 Domain。
- Confidential Computing。
- 独立 HSM。
- Secure Partition。
- 虚拟化。
- 异构安全加速器。
- 可升级的安全服务。

你可以复用 OP-TEE 的设计思想，但不需要完全遵循当前的代码结构。

**请设计这个 TEE OS 的 Architecture。**

至少需要明确：

- Execution Model。
- Thread / Task Model。
- Address Space Model。
- Trusted Application Model。
- IPC / RPC Model。
- Security Policy。
- Memory Ownership。
- Hardware Abstraction。
- Fault Isolation。
- Upgrade Strategy。

参考分析方向

这是一道没有唯一答案的架构设计题。

我认为一个优秀的设计首先应该提出几条不可妥协的约束：

**第一，安全隔离应独立于业务逻辑。** 不允许业务服务随意修改 CPU Memory Isolation、Device Ownership 或 Root Key Policy。

**第二，跨 Domain 通信必须显式表达身份、权限和资源所有权。** IPC 不能只是简单地传递一段未经约束的内存地址。

**第三，执行模型应该根据服务类型分层。** 请求驱动的短服务可以延续轻量化模型，需要独立运行的安全组件则可以采用适当的调度或事件机制。

**第四，架构无关的资源抽象必须清晰。** 例如 Memory Object、Execution Context、Isolation Domain 和 Trusted Endpoint。

**第五，设备和 CPU 的内存访问权限应属于统一安全策略的一部分。** 不能只更新 CPU Page Table，却忽略 DMA/IOMMU/SMMPT。

**第六，应支持故障隔离和最小化恢复。** 某个安全服务 Crash 不应默认破坏所有其他服务，除非已经无法证明隔离仍然成立。

**第七，版本升级是核心架构问题。** 需要考虑服务 ABI、持久化状态、签名密钥轮换、密钥用途和安全固件更新。

但必须反思一个潜在问题：

如果设计得过度通用，引入大量 Runtime Policy、复杂调度和动态机制，新的 TEE OS 是否会逐渐变成难以审计的大型通用 OS？

所以，最后需要在 Extensibility 与 Verifiability 之间找到合理的平衡。

这是我希望你在完成整套课程后能够独立论证的问题。

# **1.14 面试考核：怎样判断回答是否达到 Staff Engineer 水平？**

前面的题目不是为了考察是否记住了某个 OP-TEE 函数。

我们真正希望观察以下五种能力。

Staff Engineer 自评矩阵

## **0/25**

综合得分

架构抽象

0 / 5

能够从具体业务需求抽象出执行、隔离、资源和服务模型

威胁建模

0 / 5

能识别攻击者权限、可信前提和安全保证的边界

权衡能力

0 / 5

能够比较多个可行方案，而不是依赖唯一标准答案

系统推演

0 / 5

能够预判故障、并发、生命周期与跨模块影响

验证思维

0 / 5

能够指出什么证据会证明自己的方案是错误的

建议：先完整分析两到三个场景，再重新评估

重置自评

对于资深工程师，一个很值得培养的习惯是：

当你提出一个设计方案后，不要立即尝试证明它正确。

反而应该主动思考：

> 在什么条件下，我的方案会失败？攻击者可以通过哪些方式破坏我的假设？如果我的判断错了，系统会产生什么后果？

这种习惯能够显著提升安全系统设计与 Code Review 的深度。

# **1.15 Chapter 01 总结**

经过这一章，我们应该对 OP-TEE 建立以下核心认识。

**第一，OP-TEE 是一个 OS，不是因为它具备 Linux 的全部功能，而是因为它能够独立管理安全执行环境中的执行上下文、内存、程序生命周期和核心资源。**

**第二，OP-TEE 的架构设计以安全边界为中心。** Thread、TA、Syscall、MOBJ、Crypto、Storage 等并非相互孤立的功能模块，而是共同实现可信执行与资源隔离的机制。

**第三，PTA、User TA、Kernel Driver 的区别首先是权限与职责边界问题，而不是代码组织问题。** 一个功能应该放在哪里，需要根据其可信依赖、权限和复杂度决定。

**第四，OP-TEE 不应该试图成为整个 SoC 的最高控制者。** 它需要与 Root of Trust、Higher-privilege Firmware、HSM 和硬件隔离机制建立清晰的职责划分。

**第五，不可信不等于不可使用。** Linux 可以提供文件、内存和设备服务，但安全侧必须独立验证对安全决策有影响的数据和结果。

**第六，TCB 最小化的本质是降低权限和故障传播范围。** 不是简单地减少代码行数，也不是把所有逻辑都搬出 Kernel。

**第七，安全系统设计必须明确假设和不变量。** 如果不知道哪些条件绝对不能被破坏，后续就不可能有把握地修改 Core。

## **下一章衔接**

Chapter 02 将从本章的设计哲学进入真正的软件体系：

**OP-TEE 软件组件、模块边界与端到端依赖关系。**

重点会把：

`optee_os → Linux TEE Driver → optee_client → tee-supplicant → TA Runtime → Core Services`

这些组件完整串联起来，并对每条路径分析控制流、数据流、信任边界、异常处理和资源所有权。

这也是随后深入 Thread、Memory、TA Manager 和 Syscall 的基础。

最后我想强调一件事：

**掌握 OP-TEE Internals 的最高层次，不是记住某个函数做了什么，而是能够解释这个函数为什么必须存在、为什么只能拥有这些权限、为什么维护这些状态，以及如果改变它，会破坏哪些系统约束。**

这正是我们这套课程应该始终追求的能力。
