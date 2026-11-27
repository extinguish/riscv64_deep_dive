# **OP-TEE OS Internals**

## **Chapter 02 — OP-TEE 软件体系、模块边界与系统级协作**

Architecture · Module Boundaries · Trust Model · Control Flow · Data Flow · Impact Analysis

**源码基线：OP-TEE 4.11.0（2026-10-16）**

## **本章导读：理解整个系统，而不是记住各个组件的名字**

在 Chapter 01 中，我们讨论了 OP-TEE 为什么是一个 OS，以及它与 Linux、RTOS 的设计哲学有什么不同。

Chapter 02 要解决一个更实际的问题：

**OP-TEE 的整个软件体系究竟如何组织？为什么必须把功能拆分到不同组件？这些组件如何协作，又如何彼此制约？**

你可能已经非常熟悉这条调用链：

```
CA → libteec → Linux TEE Driver
   → SMC → OP-TEE OS → TA
```

但仅仅理解这条链，还远远达不到维护 OP-TEE Core 的要求。

例如：

- `libteec` 和 Linux TEE Driver 都负责通信，为什么不合并？
- `tee-supplicant` 运行在不可信的 Normal World，为什么 OP-TEE 还需要依赖它？
- 为什么 User TA 和 PTA 都可以通过 UUID 和 Session 调用，但安全性有本质区别？
- 为什么 OP-TEE Core 不直接实现一套 Linux 风格的文件系统？
- 如果需要增加一个新的 Secure Service，它应该放在 PTA、User TA、Driver，还是独立的 Secure Partition？
- 如果将 SMC Transport 换成 FF-A，OP-TEE 内部哪些部分应该保持不变？
- 如果 Normal World 完全被攻击者控制，OP-TEE 中哪些功能仍然应该保证安全？

这些问题表面上是在讨论模块职责，实际上涉及操作系统设计中四个最核心的概念：

**Separation of Concerns、Trust Boundary、Resource Ownership、Interface Contract。**

本章目标是建立一套能够用于真实架构决策和代码审查的思维模型。

### **学习完成后应该具备的能力**

1. 清楚解释 OP-TEE 各个软件组件的职责及其存在的理由。
2. 能够从 CA 到 TA 追踪完整的请求、返回及 RPC 路径。
3. 区分 API、ABI、Transport、Execution Domain 和 Security Boundary。
4. 理解 User TA、PTA、Secure Partition 的运行与隔离区别。
5. 能够识别 Control Plane、Data Plane、生命周期与资源所有权。
6. 给定一个新的安全功能，能够判断它应该放在哪一层。
7. 能够评估跨仓库、跨权限和跨架构的修改影响。

# **2.1 从整体软件体系开始**

## **2.1.1 OP-TEE 不是一个独立的二进制程序**

首先建立一个基本认识：

OP-TEE 并不仅仅是编译得到的 `tee.bin` 或 `tee.elf`。

严格来说，`optee_os` 是 Secure Side 的实现，而通常所说的 OP-TEE 完整运行环境，还需要 Normal World 的 Client Library、Linux Driver、RPC Helper，以及上一级 Firmware 的配合。

官方仓库也分别维护这些部分，其中 `optee_client` 包含 `libteec` 和 `tee-supplicant`，`optee_os` 主要承载可信侧实现。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



以常见 Arm TrustZone + Linux + OP-TEE 的部署为例：

完整 OP-TEE 软件系统

Normal World — Linux

Client Application

TEEC\_\* API

tee-supplicant

RPC Backends

libteec.so

GlobalPlatform Client API Implementation

Linux TEE Framework / OP-TEE Driver

ioctl · Session · SHM · RPC Dispatch · Transport

SMC / FF-A · Shared Memory

Secure World — OP-TEE

OP-TEE Core

Entry · Thread · TA Manager · Memory · Syscall

Storage · Crypto · Drivers · PTA

User TA

libutee · User Mode

ldelf

ELF Loader

Platform Firmware / Isolation Mechanisms

TF-A · Secure Monitor · CPU MMU · SoC Protection

以经典 Arm TrustZone 部署为例的逻辑图。不是严格的调用时序：PTA 在 Core 内执行，ldelf 是按需参与 TA 加载的程序；tee-supplicant 通过 Linux Driver 的 RPC 机制协作，不直接使用 libteec 完成每次请求。

这个系统可以按照职责划分成四层。

| 层次                     | 核心组件                      | 主要职责             |
| ---------------------- | ------------------------- | ---------------- |
| Client Layer           | CA、`libteec`              | 提供客户端接口、表达请求     |
| Transport Layer        | Linux TEE Driver、SMC/FF-A | 会话传递、共享内存、跨安全域通信 |
| Secure Execution Layer | OP-TEE Core、TA Runtime    | 执行可信代码、隔离资源、验证权限 |
| Supporting Services    | `tee-supplicant`、存储及设备服务  | 提供不必放入 TCB 的辅助能力 |

这里需要特别理解一个设计原则：

> **一个组件参与了安全业务，不意味着它本身必须可信。**

例如，`tee-supplicant` 可以参与 Secure Storage，但它不应该因为参与了这项服务，就自动获得明文数据或存储密钥。

这正是 OP-TEE 软件架构最值得学习的地方。

## **2.1.2 为什么要分成多个 Repository？**

从软件工程角度看，OP-TEE 各组件具有不同的构建环境、运行权限、部署周期和依赖关系。

| Repository / 项目      | 构建目标                                | 运行环境                   |
| -------------------- | ----------------------------------- | ---------------------- |
| `optee_os`           | Secure Kernel、TA Runtime、TA Dev Kit | Secure World           |
| `optee_client`       | `libteec`、`tee-supplicant`          | Linux User Space       |
| Linux Kernel         | TEE Framework 与 OP-TEE Driver       | Linux Kernel Space     |
| `optee_test`         | xtest CA 与各类测试 TA                   | Linux + OP-TEE         |
| `optee_examples`     | 示例 CA、User TA                       | Linux + OP-TEE         |
| `build` / `manifest` | 开发环境、依赖及镜像集成                        | Host Build Environment |

这是一种 **基于运行环境和稳定接口的代码分离**。

它带来几个直接好处。

首先，Linux 侧应用不必知道 Secure Kernel 如何管理 Thread、VM 或 Crypto Backend。

其次，OP-TEE Core 不需要依赖 Linux 的内部数据结构，比如 `task_struct`、`struct file` 或 Linux 的 VFS 实现。

第三，Secure Kernel 与 Linux 可以具有不同的构建配置和更新周期，但仍必须满足共同的 ABI Contract。

第四，测试代码可以相对独立地覆盖不同实现。

但是，Repository Separation 并不意味着完全独立。

例如，当修改 OP-TEE Message ABI 时，可能需要同步检查：

```
optee_os
   |
   +-- Public Message ABI
   |
   v
Linux OP-TEE Driver
   |
   v
optee_client
   |
   v
Applications / Tests
```

**仓库边界不是安全边界，也不是兼容性边界。真正重要的是组件之间的接口契约。**

# **2.2 四种经常被混淆的边界**

这是本章最重要的理论基础之一。

很多 OP-TEE 设计问题之所以难以理解，是因为我们把四种不同类型的边界混在一起了。

## **2.2.1 API Boundary**

API 定义的是一个模块向调用者提供什么功能。

例如：

```
TEEC_Result TEEC_OpenSession(...);
```

以及：

```
TEE_Result TEE_OpenPersistentObject(...);
```

前者是 Normal World Client API，后者是 User TA 使用的 TEE Internal Core API。

它们属于不同的 API Contract。

API 主要描述：

- 调用参数与返回值。
- 调用语义。
- 错误处理。
- 生命周期规则。
- 面向调用者的兼容性要求。

**API Boundary 并不一定是权限边界。**

一个普通 C 函数可以提供 API，但调用过程中完全不切换权限。

## **2.2.2 ABI Boundary**

ABI 定义的是不同二进制组件如何相互理解。

例如：

- Linux Driver 与 OP-TEE Core 共同理解的 Message Layout。
- SMC Function ID 与寄存器传参约定。
- TEE ioctl 数据结构。
- User TA Syscall 编号与调用约定。
- TA Binary Header 与签名格式。

例如，`struct optee_msg_arg` 是 Linux OP-TEE Driver 和 OP-TEE Core 通信时非常关键的消息结构。它与底层寄存器级 SMC ABI 属于不同层次。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



由此产生一个重要的架构思想：

**Transport 和 Message ABI 应尽可能解耦。**

更换 Transport 不一定需要更换整个上层 Message Protocol。

后面讲 FF-A 时，我们会看到这一原则如何体现。

## **2.2.3 Privilege Boundary**

Privilege Boundary 指的是执行代码拥有的硬件或内核权限差异。

例如：

```
OP-TEE Core
   |
   | Syscall / Exception
   |
User TA
```

User TA 即使运行在 Secure World，也不应直接拥有 Core 的所有权限。

而 PTA 不同：PTA 作为 Core 内部的特权服务实现，通常与 Secure Kernel 处于相同权限级别。

这意味着：

**Secure World 内部仍然需要特权隔离。**

不能因为两段代码都运行在 Secure World，就认为它们属于同一个可信主体。

## **2.2.4 Trust Boundary**

Trust Boundary 是安全架构真正关心的边界。

它定义了：

- 谁可以提供输入。
- 输入是否可信。
- 谁能够验证某种身份。
- 谁有权执行某种操作。
- 数据从一个安全域进入另一个安全域时，需要建立哪些不变量。

例如：

```
Normal World Buffer
        |
        | Untrusted Input
        v
OP-TEE Message Parser
        |
        | Validated Memory Reference
        v
User TA / Kernel Service
```

这个边界比单纯的函数调用更加重要。

即使地址已经能够映射，数据也不一定可信。

即使输入来自一个经过身份认证的 TA，也不意味着它有权访问所有 PTA 功能。

同样，即使代码运行在 Secure World，也不意味着它不会引入安全漏洞。

### **四种边界的对照**

| 边界        | 关键问题         | 典型实例                               |
| --------- | ------------ | ---------------------------------- |
| API       | 我能调用什么？      | `TEEC_InvokeCommand()`             |
| ABI       | 两个二进制组件怎样通信？ | `optee_msg_arg`                    |
| Privilege | 我有权执行什么？     | User TA → Kernel Syscall           |
| Trust     | 我为什么应该相信你？   | Normal World SHM → Core Validation |

这一章之后，在研究任何一个接口时，我建议你都问：

**我当前跨越的是哪一种边界？是否同时跨越了多种边界？**

这比单纯寻找函数调用关系更有助于发现设计问题。

# **2.3 OP-TEE 软件组件的职责与设计原理**

下面开始逐个分析主要组件。我们关注的不仅是 *What it does*，更重要的是 *What it must not do*。

## **2.3.1 Client Application（CA）：业务发起者**

CA 是 Normal World 中请求 TEE 服务的应用程序。

一个典型例子：

```
TEEC_InitializeContext(NULL, &ctx);

TEEC_OpenSession(&ctx, &sess, &uuid,
                 TEEC_LOGIN_PUBLIC, NULL,
                 NULL, &origin);

TEEC_InvokeCommand(&sess, CMD_SIGN_DATA,
                   &op, &origin);

TEEC_CloseSession(&sess);
TEEC_FinalizeContext(&ctx);
```

它表达了四种不同的生命周期行为：

| 操作                | 语义           |
| ----------------- | ------------ |
| InitializeContext | 连接可用的 TEE 设备 |
| OpenSession       | 建立与指定可信服务的会话 |
| InvokeCommand     | 在会话中执行某项操作   |
| CloseSession      | 结束该会话并释放相关资源 |

这里容易出现一个认识偏差：

**CA 并没有创建 OP-TEE 的 Trusted Thread，也没有直接创建 User TA Context。**

CA 只是通过 API 提出了请求。

它并不知道 Secure Kernel 如何分配执行上下文、加载 TA、管理 Session 和处理并发。

这是有意设计的。

假设以后 OP-TEE 将 Thread Pool 实现完全改变，只要外部 API 和 ABI 语义不变，普通 CA 就不应该需要修改。

这就是 Architecture Encapsulation。

### **CA 的信任等级**

CA 运行在 Normal World。

在 OP-TEE 威胁模型中，不能因为 CA 是系统预装应用，或运行在 root 权限下，就自动相信它提供的参数。

一个恶意或被攻击的 CA 可能：

- 发送非法 UUID。
- 使用不正确的 Parameter Type。
- 传入越界的 Shared Memory Offset。
- 制造超大量 Session。
- 反复调用耗时命令。
- 在调用过程中修改共享数据。

所以 OP-TEE Core 不应该将客户端提供的 Buffer、Length、Command ID 直接作为可信事实。

这也是为什么 Message Validation 必须位于 Secure Boundary 的内部。

## **2.3.2 libteec：为什么还需要一个 User-Space Library？**

`libteec` 是 GlobalPlatform TEE Client API 在 Normal World 的实现。

核心源码位置：

`optee_client/libteec/src/tee_client_api.c`

它主要完成：

- Client API 到 Linux TEE ioctl 的转换。
- Session、Context 的用户态管理。
- UUID、Parameter 的编组和转换。
- Shared Memory 的分配、注册与释放。
- 临时 Buffer 的处理。
- 返回值和 Error Origin 的转换。

上游代码中，`TEEC_OpenSession()` 会构建 `tee_ioctl_open_session_arg` 以及对应的 `tee_ioctl_param`，然后通过 Linux TEE Framework 提交请求。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有一个很重要的问题：

**为什么不让每个 CA 自己调用&#x20;**`/dev/tee0`**？**

当然可以。Linux TEE ioctl 是真实的内核接口，理论上应用可以直接使用。

但这会带来三个问题。

首先，应用需要了解 Linux ioctl 的内部细节。

其次，用户态程序需要自己管理 Shared Memory、参数编组和错误转换。

第三，应用会直接耦合到某个操作系统的 TEE Driver ABI。

`libteec` 的价值在于将这些实现细节封装成一个标准化 Client API。

所以这里可以形成一个清晰的分层：

```
GlobalPlatform Client API
          |
          v
       libteec
          |
          v
     Linux ioctl ABI
          |
          v
     Linux TEE Driver
```

**libteec 不负责 Secure World 的权限判定，也不负责证明 TA 的可信性。**

它提供的是客户端编程接口，并不是安全策略的最终执行者。

## **2.3.3 Linux TEE Framework 与 OP-TEE Driver**

这是连接 Normal World 与 OP-TEE Core 的关键组件。

它运行在 Linux Kernel Space。

相关源码位于：

`linux/drivers/tee/`

以及：

`linux/drivers/tee/optee/`

在较新的 Linux 源码中，OP-TEE Driver 已经包含独立的 `smc_abi.c`、`ffa_abi.c`、`rpc.c`、`supp.c` 等实现文件。

[image](https://www.google.com/s2/favicons?domain=https://kernel.googlesource.com\&sz=32)

Git at Google



它的主要职责可以划分成五类。

### **第一类：Device 和 Context Management**

Linux TEE Framework 提供设备抽象，例如 `/dev/tee0`。

设备节点编号在不同系统中可能有所不同，不能将 `/dev/tee0` 永久固定为唯一正确路径。

当应用打开 TEE 设备时，Linux Driver 需要管理与当前打开实例关联的状态。

当应用退出、关闭设备或发生异常时，还需要清理相关资源。

但这里管理的是 **Linux 侧 Context**。

它不等价于 OP-TEE Core 中的 `tee_ta_ctx`。

这两个对象虽然有关联，却属于不同的生命周期。

### **第二类：Shared Memory Management**

Linux 负责为跨安全域通信准备 Shared Memory。

它可能需要：

- 分配 Buffer。
- Pin 或引用用户态内存页。
- 将用户态参数转换成内核可处理的形式。
- 构造物理页列表。
- 注册 Shared Memory。
- 管理 DMA-BUF 或 TEE SHM 生命周期。

但 Linux 能够管理这些内存，不代表它拥有 Secure Kernel 内存的访问权。

反过来，OP-TEE 也不能因为 Linux 提供了某个物理地址，就直接相信这个地址合法。

### **第三类：Transport**

Linux Driver 负责通过平台支持的 Transport 进入 OP-TEE。

常见 Arm 部署中包括：

- SMC Based Transport。
- FF-A Based Transport。

这里要建立非常重要的区分：

**Transport 解决“请求如何到达”；Message Protocol 解决“到达的请求是什么意思”。**

这两个问题不应该混为一谈。

### **第四类：RPC Handling**

当 OP-TEE Core 请求 Normal World 提供服务时，Linux Driver 负责识别 RPC 返回并执行相应处理。

有些 RPC 可以在 Kernel 内完成。

另一些需要转发给 `tee-supplicant`。

因此，不能简单地把 RPC 理解成“OP-TEE 调用 tee-supplicant”。

严格来说：

```
OP-TEE Core
     |
     | RPC Request
     v
Linux OP-TEE Driver
     |
     +-- Kernel-side RPC Handling
     |
     +-- tee-supplicant
```

### **第五类：Session 与 Linux-side Resource Cleanup**

Linux Driver 还负责管理客户端会话对应的内核状态，并在文件描述符关闭时清理其持有的资源。

但是，它不能直接释放 OP-TEE Core 内部的 TA Context。

它必须通过约定的接口请求 Secure World 执行清理。

这说明：

**同一个逻辑 Session 可能横跨多个组件，但没有任何单一组件直接拥有全部内部状态。**

## **2.3.4 tee-supplicant：不可信系统为什么需要辅助服务？**

这是一个非常值得深入理解的设计。

`tee-supplicant` 是 Normal World 的用户态守护进程。

源码主要位于：

`optee_client/tee-supplicant/`

它提供的能力包括 REE 文件访问、TA 文件加载、RPMB 请求转发等，具体启用哪些服务取决于配置与版本。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



问题来了：

为什么不把这些功能全部放进 OP-TEE Core？

表面上看，将文件系统和设备访问放入 Secure World，似乎更加安全。

但实际上可能带来相反的效果。

假设将 Linux VFS、Block Layer、MMC Stack 和大量 Storage Driver 都引入 Secure Kernel。

那么 OP-TEE 的 TCB 将显著扩大。

每个新引入的文件系统 Parser、设备驱动和复杂协议实现，都可能带来新的漏洞。

OP-TEE 采用了另一种设计：

> **尽量将不需要可信执行的复杂机制放在 Normal World，将真正需要可信执行的安全决策留在 Secure World。**

例如 Secure Storage：

```
User TA
   |
   | TEE_WriteObjectData()
   v
OP-TEE Storage Service
   |
   | Encryption / Integrity
   v
TEE File System
   |
   | RPC
   v
tee-supplicant
   |
   | Linux file I/O
   v
REE File System
```

在这条路径上：

- Secure World 负责密钥、安全对象语义、数据加密和完整性验证。
- Normal World 负责执行普通文件读写、调度和存储设备操作。

Normal World 可以删除文件、拒绝服务、返回错误甚至重放旧数据。

但在正确的安全设计与密钥保护前提下，它不应该能够直接获得安全对象明文或无检测地篡改受完整性保护的内容。

需要注意，**完整性验证不等价于 Anti-Rollback**。若没有可信单调状态或相应硬件支持，旧的有效数据仍可能被重放。

OP-TEE 官方文档也明确说明了 REE FS 和 RPMB 的差别，以及通过 RPC 委托 Linux 服务的实现方式。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### **这里的真正设计思想**

不是：

“Secure World 不会操作文件，所以让 Linux 帮忙。”

而是：

“文件读写本身不需要成为可信操作；只有安全语义必须保留在 Secure World。”

这是非常典型的 **Mechanism / Policy Separation**。

你在设计新的 OP-TEE 服务时，也应该优先考虑能否采用类似的分层方式。

# **2.4 OP-TEE Core 内部的职责分解**

前面讨论了外部组件，现在开始进入 Secure Kernel。

本章不深入每一个数据结构的全部字段，但需要建立模块之间的依赖关系。

根据上游源码树，主要模块包括 `core/kernel/`、`core/mm/`、`core/tee/`、`core/crypto/`、`core/drivers/`、`core/pta/`、`core/arch/` 等。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



需要注意：源码目录并不是严格的 OS Layer。例如，`core/tee/` 也包含 Entry Dispatch 和 Syscall 相关实现，并不是所有入口代码都位于 `core/kernel/`。

## **2.4.1 Execution Management**

首先是执行管理。

它回答：

**当一个外部请求进入 OP-TEE 时，谁来执行它？**

涉及：

- Entry Handler。
- Trusted Thread。
- CPU Context。
- Interrupt/Exception。
- Suspend/Resume。
- RPC Exit。
- Synchronization。

例如，一个 CA 提交 `InvokeCommand` 请求之后，OP-TEE 需要分配或恢复 Trusted Thread。

这时管理的是 CPU 执行上下文。

不应该在这一层直接决定具体 TA 的业务语义。

这就是 Execution Management 与 Service Management 的区别。

## **2.4.2 TA Management**

TA Manager 回答另一个问题：

**当前请求应该由哪个可信应用或服务执行？**

其职责包括：

- 根据 UUID 定位可信服务。
- 创建、查找与销毁 TA Context。
- 建立 Session。
- 管理 TA Busy State。
- 保存 Client Identity。
- 处理并发调用和异常退出。

对应关键文件：

`core/kernel/tee_ta_manager.c`

在 4.11.0 中，`tee_ta_open_session()` 会调用 Session 初始化流程，检查相关参数，再操作 TA Context 和调用者身份。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有一个很重要的区别：

```
Trusted Thread
      !=
TA Session
      !=
TA Context
```

它们分别代表三个不同的概念。

| 对象             | 本质            | 生命周期                       |
| -------------- | ------------- | -------------------------- |
| Trusted Thread | 一条安全执行上下文     | 一次或多次请求的执行与挂起过程            |
| TA Session     | 调用者与服务之间的逻辑会话 | OpenSession 到 CloseSession |
| TA Context     | TA 实例及其运行时资源  | TA 实例创建到销毁                 |

它们不具有简单的一对一关系。

例如，一个 Single Instance TA 可能被多个 Session 使用，而这些 Session 的调用可能在不同的 Trusted Thread 中执行。

这意味着如果你在 Thread Layer 直接保存一个全局 TA Session，通常就是不合理的设计。

因为 Thread 是执行资源，而 Session 是服务交互状态。

两者的生命周期不同，所有权也不同。

## **2.4.3 Memory Management**

Memory Manager 负责处理：

- Core Memory。
- User TA Address Space。
- Memory Object。
- Shared Memory。
- Dynamic Mapping。
- Pager。
- Memory Protection。

Memory Management 不是单纯的页表管理。

例如，Linux 通过 Shared Memory 传来一个 Buffer，OP-TEE 需要回答：

1. 这块 Buffer 是否属于合法的共享内存范围？
2. 它对应哪些物理页？
3. 是否允许当前 TA 访问？
4. 映射是否仍然有效？
5. Buffer 在什么时候可以释放？
6. 是否存在多个 Session 同时引用？

因此，Memory Manager 与 Entry、TA Manager、Syscall、RPC 都存在依赖关系。

这也是为什么 `mobj` 是如此重要的内部抽象。

它不是一个普通的地址转换辅助模块，而是连接物理内存对象与多个安全执行上下文的基础设施。

## **2.4.4 Kernel Services**

OP-TEE Core 还提供一系列 Kernel Services，例如：

- Storage Service。
- Crypto Service。
- Property、Time、Random。
- Internal TA Management Services。
- Platform Security Services。

这些服务通过两种主要方式被访问：

一是 User TA 使用 `libutee`，通过 Syscall 进入 Core。

二是内核中的其他服务直接调用相应的 Kernel API。

这里需要注意：**用户态 API 与内核服务 API 并不相同**。

比如 User TA 通过：

```
TEE_GenerateRandom(buf, size);
```

请求随机数。

它最终会进入 Core 的对应服务，但并不意味着 PTA 或普通 Kernel Module 也应该经过 `libutee` 再访问随机数。

PTA 和其他 Core 代码通常直接使用 Kernel 内部接口。

## **2.4.5 Driver & Platform Layer**

Driver Layer 负责硬件访问，而 Platform Layer 负责描述特定 SoC 的实际资源和能力。

例如：

```
Crypto Service
     |
     v
Crypto Driver Interface
     |
     v
SoC Crypto Engine Driver
     |
     v
Secure MMIO / DMA / IRQ
```

一个好的设计应该让 Crypto Service 尽量少感知具体 SoC 的寄存器布局。

比如：

- Key Management 属于安全服务语义。
- AES Operation 属于 Crypto API 语义。
- DMA Descriptor 属于硬件实现细节。
- MMIO Register 是具体 SoC 的设备接口。

如果将这些内容混入一个大型 PTA，那么代码会很快变得难以维护。

这不是因为大型 PTA 一定不能工作，而是因为它破坏了不同层次之间的职责划分。

# **2.5 User TA、PTA 与 Secure Partition：三种不同的设计选择**

这是很多有移植经验的工程师也容易忽略的地方。

User TA、PTA 和 Secure Partition 都可以提供安全服务，但不能把它们当成三种等价的包装方式。

## **2.5.1 User TA**

User TA 运行在 OP-TEE 的非特权可信应用环境中。

通常具备：

- 独立的 User Address Space。
- GlobalPlatform Internal Core API。
- TA Context 与 Session。
- 通过 Syscall 访问内核服务。
- TA 异常与 Core 之间的隔离边界。

一个 User TA 出现普通的非法内存访问，通常不应因此直接破坏整个 Secure Kernel。

这是 User/Kernel Isolation 的基本目标。

此外，User TA 可以来自不同的存储源。例如 REE FS TA 和 Early TA 主要区别是装载来源，而不是它们拥有不同的 CPU 特权级。官方文档明确说明，Early TA 被嵌入 Core 镜像，但仍按照 User TA 模型加载运行。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **2.5.2 Pseudo TA（PTA）**

PTA 的本质不是一个独立进程。

根据 OP-TEE 官方定义，PTA 是由 OP-TEE Core 提供、但通过 TA 风格接口暴露的特权服务。PTA 代码直接编译进 Core，与 Core 运行在相同的特权执行环境中。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



这意味着：

- PTA 代码通常不经过 `ldelf` 动态加载。
- PTA 不拥有普通 User TA 那样的独立用户态隔离。
- PTA 能直接访问内核提供的内部接口。
- PTA 代码错误可能直接影响 Secure Kernel。

所以 PTA 实际上是：

**Kernel Service exposed through a TA-compatible interface.**

这和 Linux 中直接把某项功能放入 Kernel Module，在安全含义上有相似之处。

当然二者的模块管理机制不同，不能完全等同。

### **为什么 OP-TEE 还需要 PTA？**

因为某些服务确实需要执行特权操作。

例如：

- 管理 Core 内部状态。
- 访问只能由内核使用的硬件资源。
- 提供受权限保护的固件服务。
- 暴露安全存储或设备的特定内核管理能力。

但这并不意味着所有涉及硬件的功能都应该做成 PTA。

假设一个用户态 TA 需要调用 AES Hardware Engine。

完全可以采用：

```
User TA
   |
   | TEE Cipher API
   v
Crypto Kernel Service
   |
   v
Hardware Crypto Driver
```

而不是让 User TA 通过一个特殊 PTA 自己操作所有寄存器。

前者能够复用 Crypto Framework，减少重复设计和特权接口暴露。

## **2.5.3 Secure Partition（SP）**

Secure Partition 主要与 Arm FF-A 和 SPMC 模型相关。

它不仅仅是一个使用不同编译选项的 User TA。

SP 的执行和通信由 Partition Manager 进行管理，涉及 Endpoint ID、FF-A Message、Manifest、SP Context、Memory Transaction 等抽象。

在 OP-TEE 作为 S-EL1 SPMC 的配置中，可以支持 S-EL0 Secure Partition，并使用专门的 SP Message Handler 处理消息。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



其逻辑结构可以简化为：

```
FF-A Endpoint
      |
      v
SPMC
      |
      +-- Endpoint Routing
      +-- SP Context
      +-- Memory Transaction
      +-- Message Handling
      |
      v
Secure Partition
```

这与 GP Client API 的 Session/Command Model 不完全相同。

### **三种执行方式的对比**

| 属性     | User TA          | PTA                  | Secure Partition    |
| ------ | ---------------- | -------------------- | ------------------- |
| 主要接口   | GP TA API        | TA-style API         | FF-A / SP Interface |
| 执行权限   | 非特权              | Core 特权              | 取决于 SP 架构           |
| 独立隔离   | 通常有独立用户地址空间      | 无独立用户态隔离             | 由 SPMC 管理           |
| 典型调用机制 | Session / Invoke | Session / Invoke     | FF-A Messaging      |
| 管理者    | TA Manager       | Core / PTA Framework | SPMC                |
| TCB 影响 | 隔离于 Core 的代码     | 直接扩大 Core 代码范围       | 扩大相关安全组件与管理代码范围     |
| 主要适用场景 | 应用级可信服务          | 必须由 Core 执行的特权服务     | 独立安全组件、固件服务         |

**一个非常值得坚持的设计原则：优先选择满足需求的最低权限执行环境。**

但最低权限并不是唯一标准，还要同时考虑：

- 是否需要独立的更新和启动生命周期。
- 是否必须使用某个标准通信协议。
- 是否需要跨安全组件共享资源。
- 当前平台是否实际支持对应模型。
- 该服务故障后应该如何恢复。

因此，User TA、PTA 和 SP 是一个架构设计问题，而不是仅仅由功能名称决定。

# **2.6 Control Plane 与 Data Plane：为什么要分别理解？**

这是一个非常实用的架构分析方法。

Control Plane 和 Data Plane 并不是 OP-TEE Core 官方源码中两个严格独立的目录，而是我们用来分析通信与执行职责的两个逻辑视角。

## **2.6.1 Control Plane 管理“做什么”**

例如一次 `OpenSession` 请求，需要传递：

- Command Type。
- TA UUID。
- Client Identity。
- Login Method。
- Session Identifier。
- Parameter Type。
- Result 与 Error Origin。

它们主要控制系统行为。

在传统 OP-TEE Message ABI 中，`struct optee_msg_arg` 承载 Command、Session 和参数描述等内容。

概念上可以表示为：

```
/* 教学用简化结构，非源码原样定义 */
struct simplified_msg {
    uint32_t cmd;
    uint32_t session;
    uint32_t cancel_id;
    uint32_t ret;
    uint32_t ret_origin;
    uint32_t num_params;
    struct msg_param params[];
};
```

一个完整的 Message Protocol 必须能够表达请求类型及其参数。

但这些信息来自 Normal World 时，都应按不可信输入进行处理。

尤其要注意：

**Control Plane 中的身份字段，也不等于具有密码学证明的身份。**

如果身份信息是由 Linux Driver 根据当前任务 UID/GID 等状态形成的，那么它的可信程度取决于对 Linux 的信任假设。

如果系统威胁模型允许攻击者完全控制 Linux Kernel，就不能仅凭 Linux 声明的某个 UID 来证明一个高价值安全主体的身份。

## **2.6.2 Data Plane 管理“处理什么数据”**

Data Plane 主要包括：

- CA Input/Output Buffer。
- Shared Memory Buffer。
- TA Binary。
- RPC Payload。
- Storage Ciphertext。
- 加密运算的数据输入输出。

例如，一个 CA 希望对一块大 Buffer 进行运算：

```
Command:
  CMD_PROCESS_BUFFER

Parameters:
  Input Memref
  Output Memref
```

不需要将几 MB 的数据全部塞进 SMC 寄存器。

通常会使用 Shared Memory 传递数据，而寄存器和 Message Structure 负责传递地址、句柄和元数据。

其逻辑结构是：

```
Command / Metadata
       |
       v
OP-TEE Entry
       |
       | Validate
       v
Memory Reference
       |
       v
Shared Data Buffer
```

但这里存在一个很容易被忽略的安全问题。

假设 Linux 提供：

```
Buffer PA = 0x90000000
Length    = 4096
```

OP-TEE 不能仅仅检查：

```
0x90000000 属于 Non-Secure DDR
```

然后就认为这次访问完全安全。

还要考虑：

- 是否发生整数溢出或越界。
- 对应的物理页是否在整个使用期间有效。
- 谁负责引用和释放这块 Shared Memory。
- 是否允许当前 TA 使用这种 Memory Type。
- Linux 是否可能同时修改 Buffer。
- 在需要稳定输入时，是否应该复制到 Secure Memory。

**地址合法性、内存所有权和数据可信性，是三个不同的问题。**

OP-TEE 4.11.0 的官方文档明确区分静态共享内存与动态非连续共享内存，并要求对后者进行适当注册和描述。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **2.6.3 为什么这两个 Plane 必须分开思考？**

假设现在设计一个用于安全签名的 PTA：

```
CMD_SIGN
    |
    +-- Key ID
    +-- Input Buffer
    +-- Output Buffer
```

Control Plane 的重点是：

- 命令是否合法。
- 当前 Client 是否有签名权限。
- Key ID 是否可以使用。
- Output Size 是否正确。

Data Plane 的重点则是：

- 输入 Buffer 如何读取。
- 输入是否会在验证后被修改。
- 输出写入是否可能越界。
- 输出数据是否需要保密。
- 共享 Buffer 的生命周期是否足够长。

如果只完成 Command ID 和 Key ID 的权限检查，而没有考虑 Input Buffer 的并发修改，就可能出现 TOCTOU 风险。

如果只保证 Buffer 不越界，却允许任意 CA 使用受保护的签名密钥，则是授权模型错误。

**完整的安全性需要两条路径同时正确。**

# **2.7 从真实源码追踪一次 CA → User TA OpenSession**

现在进入本章最重要的实际执行场景。

我们选择：

```
TEEC_OpenSession(...)
```

假设目标是一个位于 REE Filesystem 的 Signed User TA，并且当前尚未加载到 Secure World。

为了清晰理解整个流程，先给出执行时序。

OpenSession 的关键执行阶段

1. **CA / libteec**

   用户态建立请求，准备 UUID、参数和 ioctl 数据
2. **Linux OP-TEE Driver**

   构造 `optee_msg_arg`、处理 Shared Memory，通过 Transport 进入 Secure World
3. **OP-TEE Entry**

   检查消息结构，解析命令与参数，分派给 OpenSession Handler
4. **TA Manager**

   查找 TA 实例、初始化 Session，必要时触发 User TA 加载
5. **TA Loader / RPC**

   获取并认证 TA 镜像，使用 ldelf 建立可执行映射
6. **User TA**

   进入 TA 初始化及 OpenSession Entry Point，建立会话私有状态
7. **Return Path**

   返回 Session ID、执行结果及 Error Origin，最终传递到 CA

下面沿着源码逐层分析。

## **2.7.1 第一阶段：CA → libteec**

应用调用：

```
TEEC_OpenSession(&ctx, &sess, &uuid,
                 TEEC_LOGIN_PUBLIC, NULL,
                 &operation, &origin);
```

上游 `libteec` 对应的实现函数是：

```
TEEC_Result TEEC_OpenSession(
    TEEC_Context *ctx,
    TEEC_Session *session,
    const TEEC_UUID *destination,
    uint32_t connection_method,
    const void *connection_data,
    TEEC_Operation *operation,
    uint32_t *ret_origin);
```

函数定义可以在 [tee_client_api.c](https://github.com/OP-TEE/optee_client/blob/master/libteec/src/tee_client_api.c?utm_source=chatgpt.com) 中找到。

函数需要转换的是：

```
TEEC_Operation
      |
      v
tee_ioctl_param[]
      |
      v
TEE_IOC_OPEN_SESSION
```

这一层的关键职责是参数编组，而不是 TA Loader。

### **需要特别关注 Client Identity**

`connection_method` 可以表达不同的 Client Login Method。

但在 Linux 侧实现中，某些身份信息由 Linux Kernel 填充或验证。

例如，用户身份和组成员身份需要借助 Linux 的 Credential。

这意味着：

**Linux Driver 可以根据 Linux 安全模型建立客户端身份，但它不能凭空提供抵抗恶意 Linux Kernel 的强身份保证。**

如果未来设计一个高安全等级的 Key Management Service，就不能仅凭 `TEEC_LOGIN_USER` 意味着 UID 为特定数值来防御完全被控制的 Linux。

需要根据实际 Threat Model 设计额外的认证机制。

## **2.7.2 第二阶段：Linux Driver → OP-TEE Message ABI**

Linux Driver 收到 ioctl 请求后，会转换成 OP-TEE 的消息格式。

以 SMC-based Transport 为例：

```
TEE_IOCTL_OPEN_SESSION
         |
         v
Linux OP-TEE Driver
         |
         v
OPTEE_MSG_CMD_OPEN_SESSION
         |
         v
struct optee_msg_arg
         |
         v
SMC Call With Argument
```

Linux 主线实现中，`drivers/tee/optee/call.c` 包含对应的请求构造路径，相关代码也会把登录元数据放入消息参数。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里的设计很好地体现了一个分层原则：

`tee_ioctl_open_session_arg` 是 Linux ioctl ABI 的一部分。

`optee_msg_arg` 则是 Linux OP-TEE Driver 与 OP-TEE Core 之间的消息 ABI。

不能直接把二者当作相同结构。

而底层 SMC/FF-A 又负责另一层 Transport Contract。

### **为什么要有两次转换？**

因为这允许系统维持相对独立的接口：

```
Client API
    |
    v
Linux TEE ioctl ABI
    |
    v
OP-TEE Message ABI
    |
    v
SMC / FF-A Transport
```

理论上，只要 Message Semantics 不发生变化，就可以替换底层 Transport。

同样，Linux Driver 的内部实现可以演进，而不要求所有 CA 重新理解 Secure Kernel。

当然，在真正更换 Transport 时，仍然要检查共享内存注册、句柄与返回语义等差异。

不能只替换一条进入 Secure World 的指令。

## **2.7.3 第三阶段：OP-TEE Entry Dispatcher**

进入 Secure World 后，OP-TEE 需要把 Message 解析成 Core 内部请求。

在 OP-TEE 4.11.0 中，可以重点阅读：

`core/tee/entry_std.c`

其关键函数包括：

```
tee_entry_std(...)
__tee_entry_std(...)
entry_open_session(...)
```

4.11.0 的 `__tee_entry_std()` 包含根据 `arg->cmd` 分派 Open、Close、Invoke、Cancel 等请求的逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



将相关代码逻辑简化，可以得到：

```
/* 教学用伪代码 */
switch (arg->cmd) {
case OPTEE_MSG_CMD_OPEN_SESSION:
    entry_open_session(arg, num_params);
    break;

case OPTEE_MSG_CMD_INVOKE_COMMAND:
    entry_invoke_command(arg, num_params);
    break;

case OPTEE_MSG_CMD_CLOSE_SESSION:
    entry_close_session(arg, num_params);
    break;

default:
    reject_command();
}
```

这里非常值得注意一个架构边界：

**Entry Dispatcher 不应该直接实现 TA 的业务功能。**

它应该：

1. 验证消息的基本格式。
2. 解释 Message Protocol。
3. 建立对应的内核参数对象。
4. 调用负责该业务的 Core 子系统。
5. 将执行结果转换回外部 Message。

这是 Protocol Handling 与 Business Logic 的分离。

### **为什么需要两层 Validation？**

第一层是 Entry 的 Message Validation。

例如：

- 参数数量是否合法。
- Message Buffer 是否在合法区域。
- 参数类型是否支持。
- Offset/Size 是否越界。

第二层是 Service-specific Validation。

例如：

- 当前 Client 是否有访问某个 Key 的权限。
- 某个 TA 是否允许接收 Secure Data Path Memory。
- 某个 Session 是否仍然有效。

第一层保证消息可以安全解释。

第二层保证这个请求被允许执行。

**一个通过 Message Validation 的请求，仍然可能因为权限不足而被 Service 拒绝。**

## **2.7.4 第四阶段：TA Manager 查找目标 TA**

经过 Entry Dispatcher 后，请求会到达 TA Manager。

关键函数：

```
tee_ta_open_session(...)
```

源码：

`core/kernel/tee_ta_manager.c`

它需要判断目标 TA 是否已经存在。

简化之后：

```
OpenSession(UUID)
      |
      v
Lookup existing TA Context
      |
      +-- Found
      |      |
      |      v
      |   Check sharing / busy state
      |
      +-- Not found
             |
             v
         Create TA Context
             |
             v
          Load TA
```

这并不意味着每次 OpenSession 都会创建一个新 TA Context。

是否复用实例取决于 TA Flags、当前实例状态以及相关管理策略。

所以：

**Session 的创建和 TA Context 的创建是两个独立问题。**

这里就是我们之后学习 Chapter 29 的核心。

## **2.7.5 第五阶段：TA Loader 与 tee-supplicant 协作**

如果 TA 没有加载，且目标是 REE FS TA，则需要通过 Normal World 获取 TA Binary。

我们可以继续研究：

`core/kernel/ree_fs_ta.c`

在 4.11.0 中，该文件的 `rpc_load()` 使用：

```
thread_rpc_cmd(OPTEE_RPC_CMD_LOAD_TA, ...);
```

向 Normal World 请求相应的 TA 镜像。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这一步揭示了 OP-TEE 很重要的一个设计特点：

**在处理 CA 请求的过程中，OP-TEE Core 可能需要暂时退出 Secure World，要求 Normal World 完成辅助工作。**

整个执行关系可以表示为：

```
OP-TEE Core
    |
    | Need TA binary
    v
RPC Request
    |
    v
Linux OP-TEE Driver
    |
    v
tee-supplicant
    |
    | Read UUID.ta
    v
REE File System
    |
    | TA Binary
    v
tee-supplicant
    |
    v
Linux OP-TEE Driver
    |
    v
RPC Resume
    |
    v
OP-TEE Core
```

但这里还有一个关键的安全边界：

`tee-supplicant` 提供 TA 镜像，不意味着它有权决定该镜像是否可信。

OP-TEE 的可信侧需要执行相应的签名、格式、密钥及版本等验证。

之后才进入后续 Loader 和 TA Runtime 初始化阶段。

### **为什么需要 ldelf？**

因为 TA 加载并不只是复制一段 Binary。

完整的 ELF Loading 还需要考虑：

- Program Headers。
- Segment Layout。
- Memory Mapping。
- Relocation。
- 动态链接。
- User Stack。
- TA Entry Point。

OP-TEE 使用单独的用户态 ELF Loader `ldelf` 处理相关工作。

这使复杂的 ELF Loading Logic 不必全部在特权内核中执行。

但这并不意味着 ldelf 本身就是信任根。安全验证仍必须由正确的可信链路保证。

## **2.7.6 第六阶段：User TA 开始执行**

当 TA 已经正确加载并完成运行环境准备后，OP-TEE 可以进入 User TA。

对于 OpenSession，涉及 TA 的初始化和：

```
TA_OpenSessionEntryPoint(...)
```

成功后，Session 将获得可用于后续 InvokeCommand 的标识。

注意 TA Context、Session 和 User TA 内部保存的 Session Data 并不相同。

例如：

```
OP-TEE Session Object
         |
         v
User TA OpenSession
         |
         v
void *sessionContext
```

User TA 通过 Session Context 保存业务状态。

而 Kernel Session Object 负责内核级会话管理、身份和执行状态。

因此，如果 User TA 将某个 C 指针返回给 Kernel 当作普通 Session Context，并不代表 Kernel 会自动理解这个指针指向的业务数据。

它只需要维护对应的生命周期契约。

## **2.7.7 第七阶段：返回 CA**

请求完成后：

```
User TA
   |
   v
TA Manager
   |
   v
OP-TEE Entry
   |
   v
Transport Return
   |
   v
Linux OP-TEE Driver
   |
   v
libteec
   |
   v
Client Application
```

最终 CA 获取：

- `TEEC_Result`。
- `returnOrigin`。
- Session ID。
- 输出参数。

这里还有一个容易被忽略的区别：

**Operation Result 和 Error Origin 必须分别理解。**

例如：

`TEE_ERROR_BAD_PARAMETERS` 可以发生在不同层次。

如果是 CA 参数编组出错，错误可能源于 API Layer。

如果是 Secure Kernel 发现非法共享内存，可能属于 TEE Core Layer。

如果 User TA 自己判断业务参数不合法，则属于 Trusted Application Layer。

Error Origin 用来帮助定位错误发生的抽象层，但它本身不是访问控制依据，也不能代替调用链诊断。

# **2.8 RPC：OP-TEE 架构中最值得深思的边界**

我们再深入一步。

一个很有意思的问题是：

**OP-TEE 既然是一个 OS，为什么很多时候要让 Linux 来帮助实现等待、唤醒和外部服务？**

答案要回到 OP-TEE 的执行模型。

经典 OP-TEE Trusted Thread 不是由一个 Linux 风格的内部调度器独立进行时间片调度。

在很多同步等待场景中，OP-TEE 会暂停 Trusted Thread，借助 Normal World 的调度能力完成等待或服务请求。

官方 Core 文档中，Mutex/Condition Variable 等阻塞操作就可能通过 RPC 让 Normal World 执行等待或唤醒。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **2.8.1 RPC 不意味着 Trusted Thread 被销毁**

假设一个 TA 需要执行 Storage Read：

```
Trusted Thread #2
      |
      v
Storage Service
      |
      v
RPC to Normal World
```

此时通常发生：

1. 保存 Trusted Thread 的执行状态。
2. 将该 Thread 标记为可恢复的挂起状态。
3. 返回 Normal World 执行 RPC。
4. Linux 调度并执行必要服务。
5. Normal World 使用约定机制重新进入 Secure World。
6. OP-TEE 恢复原 Trusted Thread。
7. 从之前的 RPC 调用点继续执行。

因此：

```
RPC != TA Context Destroy
RPC != Session Close
RPC != Trusted Thread Release
```

一个正在等待 RPC 的 Trusted Thread 仍然占有其相关执行资源。

这对后续性能和资源管理非常重要。

## **2.8.2 RPC 为什么容易形成死锁？**

考虑：

```
Trusted Thread A
    |
    | Acquire Lock L
    v
RPC
    |
    | Wait for Normal World
    v
Normal World
```

如果 RPC 后续的完成路径，需要另一个 Trusted Thread 再次取得 Lock L，就可能形成：

```
Thread A holds L
       |
       v
Wait for RPC
       |
       v
Thread B needs L
       |
       v
Cannot progress
```

这里的真正问题不是 RPC 本身。

而是：

**一个跨执行域的等待操作，可能把局部锁依赖扩展成跨系统的等待图。**

这也是 OP-TEE Kernel Locking 不能简单照搬 Linux 的原因之一。

注意：并不是所有持锁期间的 RPC 都必然错误，而是必须分析具体锁的语义、潜在重入和等待依赖。对不允许睡眠的上下文，还存在更严格的限制。

## **2.8.3 RPC 为系统引入什么样的失败模型？**

我们应该区分：

| 失败                 | 对应的安全或功能后果             |
| ------------------ | ---------------------- |
| tee-supplicant 不运行 | 部分服务无法完成               |
| RPC 返回非法长度         | 必须拒绝越界访问               |
| RPC 返回错误文件数据       | 应触发认证/完整性失败            |
| RPC 长时间不返回         | Availability 受到影响      |
| Linux 修改共享数据       | 可能引入数据竞争/TOCTOU        |
| Linux 重放旧的有效数据     | 需要独立的 Anti-Rollback 机制 |
| Linux 进程重启         | 需要明确恢复或失败策略            |

因此，OP-TEE 可以依赖 Normal World 的机制，却不能无条件依赖它的正确性。

这是一个非常值得记住的结论：

> **Secure World 可以将工作委托给 Untrusted World，但不能将安全决策委托给 Untrusted World。**

# **2.9 现代 Transport：SMC、FF-A 与架构独立性**

这里我们要将本章内容与你之前研究的 Arm/RISC-V 架构连接起来。

## **2.9.1 SMC 和 FF-A 不应该被看成同一层级的替代品**

SMC 是 Arm 的架构调用机制。

FF-A 则定义了更高层次的 Firmware Framework 协作模型，涵盖 Endpoint、Messaging、Memory Sharing 等。

FF-A 调用在 Arm 系统上仍可能通过 SMC 指令承载。

因此：

**FF-A 与 SMC 不是简单的“使用 FF-A 后不再执行 SMC 指令”。**

更准确地说，系统改变了通信协议、参与角色和内存共享模型。

对于传统 SMC-based OP-TEE Message ABI：

```
Linux OP-TEE Driver
       |
       | SMC call + message arg
       v
OP-TEE Core
```

对于 FF-A-based 通信：

```
Linux OP-TEE Driver
       |
       | FF-A Transport
       v
SPM / SPMC Infrastructure
       |
       v
OP-TEE Endpoint
```

具体是否存在独立的 SPMC，以及 OP-TEE 自身承担怎样的 SPMC 职责，取决于系统架构和配置。

在 4.11.0 中，FF-A Shared Memory 的注册与使用可以涉及 FF-A Memory Handle、`FFA_MEM_SHARE` 和 `FFA_MEM_RETRIEVE_REQ` 等操作。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **2.9.2 更换 Transport 后，哪些模块应当保持稳定？**

从理想架构上看：

| 模块                         | 是否应直接受 Transport 改动影响 |
| -------------------------- | --------------------- |
| `libutee` GP API           | 原则上不应                 |
| User TA Business Logic     | 原则上不应                 |
| Crypto Backend             | 原则上不应                 |
| Storage Object Semantics   | 原则上不应                 |
| TA Manager                 | 尽量不应                  |
| Message Entry              | 可能需要适配                |
| Shared Memory Registration | 通常需要仔细适配              |
| Thread Entry/Resume        | 可能需要适配                |
| Linux OP-TEE Driver        | 需要适配                  |
| Firmware/SPMC Interface    | 需要适配                  |

这里要区分两个概念：

**Architecture Goal** 与 **Actual Implementation**。

从架构目标上看，Transport 不应该污染 TA Manager 或 Crypto Framework。

但在实际工程中，Transport 变化可能改变 Shared Memory 的生命周期和权限语义，进而要求调整下层公共代码。

所以不能简单认为只修改 Driver 的 `smc_abi.c` 就足够了。

## **2.9.3 对 RISC-V OP-TEE Porting 的启发**

RISC-V 平台具有不同的特权架构和 Firmware 调用路径。

不应该机械地将 Arm SMC 的所有实现细节照搬过去。

更值得研究的是：

```
Architecture-Neutral TEE Services
              |
              v
Common Message / TA Runtime
              |
              v
Entry & Transport Abstraction
              |
              v
RISC-V-specific Backend
```

例如，在设计 RISC-V 的新通信路径时，可以先提出三个要求。

**第一，尽可能复用现有 TA Runtime。**

不应该因为进入 OP-TEE 的方式不同，就重新实现 Session Manager。

**第二，保持可信身份及参数校验规则。**

Transport 改变不能无意中使某个 Client 获得更高权限。

**第三，保持 Shared Memory 的安全不变量。**

即使底层内存保护从 Arm 的机制切换到 PMP、SMMPT 或其他平台级保护机制，OP-TEE 对 Memory Ownership、Reference Lifetime 和授权的基本要求仍然存在。

但必须特别说明：Arm FF-A 和 RISC-V 的 Firmware/TEE 通信机制不是直接对应的标准，不能假定当前 RISC-V OP-TEE Backend 已完整支持 Arm FF-A/SPMC 的语义。

这正是为什么我们应该先明确通用软件职责，再讨论具体架构实现。

# **2.10 资源生命周期与所有权：为什么跨模块 Bug 如此难查？**

很多复杂 OS Bug，并不是算法本身写错，而是不同子系统对资源生命周期的理解不一致。

我们用一个 Session 来分析。

## **2.10.1 一个 Session 横跨哪些对象？**

```
Client Application
    |
    +-- TEEC_Context
    +-- TEEC_Session
    +-- TEEC_SharedMemory
               |
               v
Linux TEE Driver
    |
    +-- Device Context
    +-- Session Bookkeeping
    +-- Shared Memory References
               |
               v
OP-TEE Core
    |
    +-- tee_ta_session
    +-- tee_ta_ctx / ts_ctx
    +-- Trusted Thread
    +-- Memory Objects
               |
               v
User TA
    |
    +-- TA Instance State
    +-- Session Context
    +-- User Heap / Stack
```

这里每一个对象都有自己独立的所有权规则。

比如：

- `TEEC_Session` 属于 CA 和 `libteec` 的编程模型。
- Linux Driver 维护它能够识别的会话状态。
- `tee_ta_session` 是 Secure Kernel 内部对象。
- `tee_ta_ctx` 代表可信应用实例。
- User TA Session Context 是 TA 自己管理的业务状态。

它们之间存在关联，但不能混为一谈。

## **2.10.2 为什么 CloseSession 可能很复杂？**

假设两个 Trusted Thread 正在引用同一个 Session。

与此同时，第三个线程请求关闭这个 Session。

如果 CloseSession 立即释放 Kernel Session Object，就可能导致：

```
Thread A: using Session
Thread B: using Session
Thread C: free(Session)
```

最终造成 Use-After-Free。

因此，正确的 Session Teardown 必须考虑：

- Session 是否仍然 Active。
- 是否还有其他 Thread 在引用。
- 是否正在调用 TA。
- 是否存在等待 Lock 的线程。
- TA 是否已经 Panic。
- 是否应调用 TA Close Entry Point。
- TA Context 是否需要销毁。

上游 `tee_ta_manager.c` 中包含 Session Reference Count、Active Flag、Condition Variable 等机制，用于协调上述状态。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### **为什么这个例子重要？**

因为它说明：

**函数返回并不等于资源生命周期结束。**

同样：

- RPC 返回不意味着 Shared Memory 可以立即释放。
- TA CloseSession 不一定意味着整个 TA Context 销毁。
- Linux Client 崩溃不意味着 OP-TEE Core 中的状态已经自动清除。
- Secure Driver 完成一次 Command 不一定意味着 DMA Buffer 已经安全解除映射。

对于一个 Core Maintainer，资源生命周期通常比单个函数流程更重要。

# **2.11 从安全架构角度重新审视组件关系**

现在可以把本章所有组件放进一个统一的信任模型。

## **2.11.1 信任不等于运行位置**

我们可以按照攻击面和权限区分：

| 组件              | 信任假设                  | 主要安全风险                       |
| --------------- | --------------------- | ---------------------------- |
| CA              | 不可信                   | 恶意请求、身份滥用                    |
| libteec         | Normal World Software | 参数编组错误、兼容性                   |
| Linux Driver    | Normal World Kernel   | 地址欺骗、资源滥用、错误消息               |
| tee-supplicant  | 不可信辅助服务               | 数据篡改、拒绝服务、重放                 |
| OP-TEE Entry    | Secure Kernel TCB     | Message Parser 漏洞            |
| TA Manager      | Secure Kernel TCB     | Identity、Session、Lifetime 错误 |
| User TA         | 受隔离的可信应用代码            | TA 漏洞、越权调用                   |
| PTA             | Core TCB              | 特权代码漏洞                       |
| Hardware Driver | Core TCB              | MMIO、DMA、设备权限错误              |
| SPMC/SP         | 依据具体执行模型              | Endpoint、Memory、隔离错误         |

这里的“可信应用代码”并不是说 User TA 永远不会出现漏洞，而是它在系统安全模型中有被认证的代码身份，并通常受到 Core 的隔离和权限限制。

而且 User TA 也不能天然信任其他 User TA。

不同 TA 之间同样需要遵守访问控制与对象隔离规则。

## **2.11.2 用安全不变量建立可靠的架构边界**

对于 OP-TEE，我建议从本章开始建立以下七个系统级安全不变量。

| ID     | Security Invariant                                |
| ------ | ------------------------------------------------- |
| INV-01 | Normal World 不得仅通过提供地址或参数获得 Secure Kernel 私有内存访问权 |
| INV-02 | User TA 不得绕过 Kernel 权限检查直接使用 Core 特权资源            |
| INV-03 | PTA 暴露的接口必须对调用者执行符合安全策略的授权                        |
| INV-04 | Session、Context、MOBJ 等资源在被引用期间必须保持有效              |
| INV-05 | RPC 返回的数据必须按照不可信数据验证                              |
| INV-06 | TA Code Authentication 不得依赖不可信文件系统对文件内容的声明        |
| INV-07 | Transport 替换不得无意中改变安全服务的身份、权限或内存隔离语义              |

这些不是对 OP-TEE 所有具体配置的形式化证明，而是我们在阅读和修改源码时应维护的设计目标。

以后读到某个函数时，不要只关心它做什么，还要关心它正在维护哪一条 Invariant。

例如 `entry_open_session()`：

它不仅负责创建 Session。

它也处于 INV-01、INV-03、INV-04、INV-07 等多项约束的交界处。

因此，一次看起来很小的 OpenSession 参数处理修改，也可能变成安全问题。

# **2.12 本章源码研究清单**

下面这些是建议你在 Chapter 02 阶段真正阅读的文件。

注意，我们这一章的目标是理解模块关系和调用边界，而不是逐行读完所有内部实现。

| 源码位置                                               | 本章阅读重点                  |
| -------------------------------------------------- | ----------------------- |
| `optee_client/libteec/src/tee_client_api.c`        | Client API 如何转为 ioctl   |
| `linux/drivers/tee/optee/call.c`                   | Linux Driver 如何构建消息     |
| `linux/drivers/tee/optee/smc_abi.c`                | SMC-based Transport     |
| `linux/drivers/tee/optee/ffa_abi.c`                | FF-A-based Transport    |
| `linux/drivers/tee/optee/rpc.c`                    | RPC 的 Kernel-side 实现    |
| `optee_client/tee-supplicant/src/tee_supplicant.c` | Supplicant RPC Dispatch |
| `optee_os/core/tee/entry_std.c`                    | Message Entry Dispatch  |
| `optee_os/core/kernel/tee_ta_manager.c`            | TA Context 与 Session    |
| `optee_os/core/kernel/user_ta.c`                   | User TA Runtime         |
| `optee_os/core/kernel/ree_fs_ta.c`                 | REE TA Loading          |
| `optee_os/ldelf/`                                  | ELF Loader              |
| `optee_os/core/mm/`                                | Memory Object / VM      |
| `optee_os/core/pta/`                               | Privileged TEE Services |
| `optee_os/core/tee/tee_svc.c`                      | Kernel Syscall Service  |
| `optee_os/lib/libutee/`                            | User TA Internal API    |

建议将源码版本固定在 `4.11.0`，Linux Driver 选择一个确定的 Kernel Tag，并额外记录当前所用的 `optee_client` Commit ID。这样才能避免不同仓库版本不一致造成分析偏差。

## **实验一：建立 OpenSession Cross-Repository Call Graph**

在具备本地源码的环境中，可以执行：

```
# optee_client
rg -n 'TEEC_OpenSession|TEE_IOC_OPEN_SESSION' \
  libteec/

# Linux kernel
rg -n 'OPTEE_MSG_CMD_OPEN_SESSION|optee_open_session' \
  drivers/tee/optee/

# optee_os
rg -n 'entry_open_session|tee_ta_open_session' \
  core/

# TA loading
rg -n 'OPTEE_RPC_CMD_LOAD_TA|rpc_load|init_with_ldelf' \
  core/
```

目标不是收集一大堆搜索结果。

而是最终回答：

1. 哪个函数完成 API → ioctl 转换？
2. 哪个函数完成 ioctl → OP-TEE Message 转换？
3. 哪个函数接收 OpenSession Message？
4. 哪个函数创建和管理 TA Session？
5. 哪个模块获取 TA Binary？
6. 哪一层完成认证？
7. 哪一层负责映射和执行 TA？
8. OpenSession 失败后，谁清理已经创建的资源？

## **实验二：设计故障注入矩阵**

选择同一个 CA→TA OpenSession 请求，在不同阶段注入故障。

| 故障注入位置                       | 预期观察重点                  |
| ---------------------------- | ----------------------- |
| libteec 参数错误                 | ioctl 是否发出，错误来源         |
| Linux Shared Memory 分配失败     | Linux 资源是否释放            |
| OP-TEE Message Validation 失败 | 是否触及 TA Manager         |
| TA File Missing              | RPC 与 TA Loader 错误路径    |
| TA Signature Invalid         | 是否在执行之前被拒绝              |
| TA Memory Allocation Failure | VM 与 TA Context Cleanup |
| TA OpenSession Entry Panic   | Session、Context 与返回语义   |
| tee-supplicant 不可用           | RPC、Thread 和错误/等待行为     |

完整的实验报告不只记录错误码，还应该记录：

**最后一个成功完成的阶段，以及失败后仍然存活的对象。**

## **实验三：绘制一张资源所有权地图**

针对 OpenSession，至少标记：

```
TEEC_Context
TEEC_Session
Linux TEE Context
Linux SHM
optee_msg_arg
Trusted Thread
tee_ta_session
tee_ta_ctx
User TA VM
User TA Session Context
RPC Shared Memory
```

然后给每个对象填写：

- Creator。
- Owner。
- Reference Holder。
- Destruction Trigger。
- Cleanup Function。
- Failure Path。

能够完成这个实验，说明你已经开始以 OS Maintainer 的方式阅读源码，而不再只是跟着函数调用一路向下。

# **2.13 Staff Engineer 级别综合考核**

这部分我会刻意避免简单的知识记忆题。

所有题目都围绕本章的主题——**OP-TEE 软件体系、模块边界与系统级协作**——但允许延伸到安全架构、并发、内存、Firmware、系统设计、协议演进和真实工程问题。

每道题目都没有唯一标准答案。

我更关注你的推理过程，尤其是以下四种能力：

- 能否识别题目中没有明确说明的系统假设？
- 能否提出多种方案，并解释为什么选择其中一种？
- 能否发现正常执行流程之外的安全与异常问题？
- 能否说明如何验证自己的设计，而不是停留在理论上？

建议先独立思考题目，再看后面的参考分析要点。

## **Question 01 — 当 Normal World 完全被攻破，OP-TEE 的安全边界还剩下什么？**

Security Architecture

### **场景**

你负责设计一个 Secure Key Service。

一个 CA 通过 OP-TEE 请求使用设备私钥签名。

系统采用以下设计：

```
Android System Service
       |
       v
libteec
       |
       v
Linux OP-TEE Driver
       |
       v
Secure Key PTA
       |
       v
Hardware Key Storage
```

PTA 根据 CA 的 UID 判断是否允许使用某个密钥。

目前功能正常，也通过了全部功能测试。

但安全团队提出：

> 如果 Linux Kernel 已被攻击者完全控制，我们仍然希望攻击者不能冒充授权应用使用设备私钥。

请回答：

1. 目前设计是否满足这个安全目标？
2. Secure World 能否相信 Linux Driver 提供的 Client Identity？
3. `TEEC_LOGIN_USER` 与真正的 Strong Client Authentication 有什么区别？
4. 如果不能信任 Linux，如何重新设计身份认证与授权？
5. 哪些验证必须在 Secure World 完成？
6. 如果无法可靠认证 CA，是否应该改变服务的安全目标？

### **参考分析要点**

最重要的是识别 **Identity Provenance**。

如果 UID 由 Linux Kernel 提供，那么完全控制 Linux 的攻击者通常可以伪造与 UID 相关的可信假设。

TEE 不能通过重复检查一个来自 Linux 的身份字段，将它变成硬件认证的身份。

可行方案需要依赖更强的信任来源，例如受保护的身份凭据、可信启动/度量链、独立认证协议或其他硬件支持的身份机制。

而且需要认真讨论：如果应用本身在被攻击者控制的 Linux 上执行，那么即使拥有应用级密钥，它能否防止恶意系统代替应用发起操作？

**评判重点：** 能否区分 Client Identification、Authentication 和 Authorization，以及认识到 TEE 无法凭空创造一个可信的客户端身份来源。

## **Question 02 — 一个新功能到底应该做成 User TA、PTA，还是 Secure Partition？**

Architectural Decision

### **场景**

公司准备开发一个 Secure Firmware Update Service。

需求如下：

- 验证 Firmware Image Signature。
- 读取安全版本计数器。
- 控制 Flash Controller。
- 支持 Firmware Rollback Protection。
- 向 UEFI 暴露更新状态。
- 支持未来独立升级安全服务。
- 某些 Hardware Register 只能在最高安全权限下访问。

架构团队提出了三个方案：

A. 全部实现为一个大型 PTA。

B. 全部实现为 User TA，必要时增加 Syscall。

C. 将功能拆成 User TA + Kernel Service，或独立 Secure Partition + Hardware Backend。

请你作为 Staff Engineer：

1. 比较三个方案的 TCB、权限、可维护性和升级成本。
2. 设计合理的 Module Boundary。
3. 判断 Firmware Signature Verification 应放在哪一层。
4. 判断 Anti-Rollback Counter 应由谁管理。
5. 说明如何避免 User TA 直接获得任意 Flash MMIO 访问权。
6. 说明如何处理更新过程中断电。
7. 如果目标 SoC 没有 FF-A/SPMC，设计应该如何调整？

### **参考分析要点**

较合理的思路通常是拆分：

```
Update Policy
      |
      v
Secure Application / Partition
      |
      v
Narrow Privileged Service
      |
      v
Hardware Backend
```

但这不是强制要求使用 User TA 或 SP。

关键是特权最小化、升级边界和故障隔离。

Firmware 更新策略、镜像解析、硬件写入控制和安全计数器并不一定应该放在同一个特权模块中。

如果某种方案要求将复杂、不可信的 Firmware Parser 直接放入 Core，需要认真评估 TCB 扩大产生的风险。

**评判重点：** 是否能够把业务策略、认证、设备访问和资源管理拆成清晰的责任边界，并识别安全启动链与更新链之间的依赖。

## **Question 03 — 一次看似简单的零拷贝优化为什么可能成为安全漏洞？**

Memory Ownership & TOCTOU

### **场景**

当前 CA 每次向 TA 发送 4 MB Buffer，都要经历一次额外的数据复制。

为了提高性能，工程师提议修改 OP-TEE Entry：

> 只要传入地址属于 Non-Secure DDR，就直接映射进 TA Address Space，不再复制，也不再使用额外的 Memory Object 引用。

性能测试显示吞吐量显著提升。

请分析：

1. 仅验证物理地址范围有什么问题？
2. 为什么仍然需要 Memory Ownership 和 Lifetime 管理？
3. 如果 Linux 在 TA 执行过程中释放或修改 Buffer，会有什么风险？
4. 使用动态共享内存时如何避免非法映射与 UAF？
5. 如果这块 Buffer 同时被 DMA 使用，还需要哪些约束？
6. 如果你负责评审这个 Patch，至少要求哪些测试？
7. 有没有一种架构能既保持 Zero-Copy 的性能，又不牺牲安全属性？

### **参考分析要点**

首先需要认识到：

**Zero-Copy 不意味着 Zero-Validation。**

物理地址范围验证只是第一层。

还需要正确处理 Memory Reference、Page Lifetime、权限、引用计数、长度验证和并发状态。

同时，Zero-Copy 无法自动保证 Buffer 内容在使用期间不发生变化。如果某项安全决策需要稳定的 Input Snapshot，可能仍然需要复制或者其他受保护的数据所有权机制。

不能因为 CPU VA 映射有效，就推断设备 DMA 访问也是安全的。

**评判重点：** 能否同时考虑 Spatial Safety、Temporal Safety 和 Data Consistency，并能建立端到端的 Buffer Lifecycle。

## **Question 04 — tee-supplicant 是不是 OP-TEE 整个安全模型中的薄弱环节？**

Delegation & Threat Modeling

### **场景**

安全团队发现，某个 Secure Storage 实现依赖 `tee-supplicant` 提供文件读取服务。

他们提出：

> tee-supplicant 运行在 Linux User Space，Linux 可以被攻击。因此应该将所有文件系统功能移入 OP-TEE Core。

请评估这个提议。

进一步考虑：

1. 将 VFS、Block Layer、MMC Driver 全部移入 Core 有什么代价？
2. 哪些操作确实必须由 Secure World 完成？
3. 加密和完整性验证是否足以保证 Storage Security？
4. Linux 返回旧的合法文件内容时，OP-TEE 应如何识别？
5. Linux 长期拒绝提供 Storage 服务时，能否保证 Availability？
6. 如果使用 RPMB，Linux 仍作为数据转发者，为什么仍可能实现受硬件支持的防重放？
7. 怎样通过架构而非简单增加检查，降低攻击面？

### **参考分析要点**

核心是 **TCB Minimization** 和安全性质的分离。

需要分别考虑：

- Confidentiality。
- Integrity。
- Freshness / Rollback Resistance。
- Availability。
- Atomicity。

将所有 Storage Mechanism 移入 Secure Kernel，并不自动使这五种安全性质全部得到保证。

TEE 可以利用密码学和硬件信任锚，降低对不可信文件系统的安全依赖。

但不可信 Normal World 仍可以拒绝服务。

**评判重点：** 能否证明委托不可信组件是一个有意识的架构选择，而不只是为了节省开发工作。

## **Question 05 — 从 SMC 迁移到 FF-A，为什么不只是改几个 Transport 函数？**

Protocol Evolution

### **场景**

一个现有产品使用传统 SMC-based OP-TEE 通信。

架构团队决定迁移到 FF-A，以便未来支持多个 Secure Partition。

某位工程师认为：

> 只要把 Linux Driver 的 SMC 调用替换成 FF-A 调用，OP-TEE Core 不需要任何变化。

你需要进行 Architecture Review。

请回答：

1. SMC Transport、OP-TEE Message ABI、FF-A Protocol 有什么不同？
2. 哪些接口应该保持稳定？
3. Shared Memory Registration 的语义是否会变化？
4. Memory Handle 的 Ownership 和 Lifetime 谁负责？
5. 是否可能引入新的 Endpoint Identity 和访问控制要求？
6. 如果发生 RPC Suspend，Thread 恢复链路是否仍然正确？
7. 如何保证旧版 User TA 和客户端不需要修改？
8. 如何建立新旧 Transport 的等价性测试？

### **参考分析要点**

理想目标是保持 TA-facing API 和业务语义稳定。

但 FF-A 不只是另一种寄存器传参方法，还引入内存事务、Endpoint 和消息路由等语义。

最容易忽略的是 Shared Memory 的生命周期与权限管理。

不能在新的 Transport 中，只要取得一个 Handle 就假定可以永久访问该内存。

**评判重点：** 能否识别协议语义变化，而不是只关注函数调用路径替换。

## **Question 06 — 一个跨 Normal World 与 Secure World 的死锁**

Concurrency & Scheduling

### **场景**

某个 OP-TEE 平台配置：

```
CFG_NUM_THREADS = 2
```

系统运行两个并发请求。

```
Trusted Thread A
  holds Secure Lock L
  -> requests RPC operation X

Trusted Thread B
  -> waits for Secure Lock L
```

RPC operation X 的完成又需要某个安全服务 Y 被执行。

但 Y 必须通过第三个 Normal World 请求重新进入 OP-TEE，才能完成。

结果系统停止响应。

请分析：

1. 这里可能形成什么 Wait-For Graph？
2. 是否存在 Thread Pool Exhaustion？
3. 为什么普通 Linux 调度器无法解决所有问题？
4. 增加 `CFG_NUM_THREADS` 能否彻底解决？
5. 如果需要重新设计 Locking，应该从哪里入手？
6. 能否把 RPC 分成同步和异步阶段？
7. 如何证明改造后不存在同类资源依赖循环？

### **参考分析要点**

关键不是“锁等待时间太长”。

而是形成了一个资源依赖环：

```
A holds L
  -> waits for X
      -> X needs Y
          -> Y needs free trusted thread / L
```

如果 Y 无法获得所需执行资源或锁，就无法推进 X，A 也无法释放 L。

增加 Thread 数量可能延迟问题出现，却不解决设计上的循环等待。

可能的方案包括缩小临界区、禁止特定持锁 RPC、拆分操作、消除重入依赖、提前预留资源等。

**评判重点：** 能否从跨组件等待依赖的角度分析死锁，并提供具备终止性依据的设计。

## **Question 07 — OP-TEE 与 Linux 不同步升级时，如何设计兼容性？**

ABI & System Evolution

### **场景**

一个产品采用独立更新机制：

- Boot Firmware 可以单独升级。
- OP-TEE OS 可以单独升级。
- Linux Kernel 可以单独升级。
- `libteec` 和 `tee-supplicant` 可能由不同团队维护。
- User TA 可以通过 OTA 动态更新。

新版本 OP-TEE 增加了新的 Message Parameter Type 和新的 Shared Memory Feature。

请设计一套系统级兼容策略。

要求回答：

1. 哪些接口是稳定的 Public ABI？
2. 应如何进行 Feature Discovery？
3. 老版本 Linux Driver 遇到新 Feature 时怎么办？
4. 旧版 tee-supplicant 缺少新 RPC Command 时怎么办？
5. 新版 User TA 依赖旧版 Core 不支持的 Syscall 时怎么办？
6. 安全功能不受支持时，是否允许 Fallback？
7. 如何设计升级、回滚与兼容性测试矩阵？
8. 如何防止攻击者利用旧版本组件绕过安全策略？

### **参考分析要点**

需要把接口分成：

- Stable API。
- Cross-component ABI。
- Private Implementation Detail。
- Capability-dependent Feature。

只有明确接口级别，才能合理决定是否允许独立升级。

新功能应该具备明确的 Feature Detection、版本约束和 Unsupported Behavior。

但是对于安全关键功能，Fallback 不一定安全。

例如新版本要求强制 Rollback Protection，旧版本不支持时，直接回退到无保护模式可能构成 Security Downgrade。

**评判重点：** 能否将兼容性、升级原子性和 Security Downgrade 放到同一个设计模型中。

## **Question 08 — 当 CA 崩溃时，整个跨组件 Session 应该如何清理？**

Resource Lifecycle

### **场景**

一个 CA 创建 Session，提交了一个耗时的 TA Command。

执行期间：

- TA 已经分配 Secure Memory。
- Linux Driver 持有 Shared Memory 引用。
- Trusted Thread 正在等待 RPC。
- 另外一个 Trusted Thread 可能正在引用相同的 TA Context。

此时 CA 进程被强制终止。

请分析：

1. Linux 如何感知 CA 的退出？
2. Linux Driver 应该释放哪些资源？
3. 哪些资源必须由 OP-TEE Core 清理？
4. 被挂起的 Trusted Thread 如何处理？
5. CloseSession 与 InvokeCommand 并发时，如何避免 UAF？
6. 如果 tee-supplicant 也崩溃了怎么办？
7. TA 的 Panic Cleanup 与正常 CloseSession Cleanup 有什么不同？
8. 如何设计测试证明没有资源泄漏？

### **参考分析要点**

必须首先明确各层资源的 Ownership 和 Reference Holder。

不能假定 CA 被杀死时，OP-TEE Core 就能够立即知道并释放所有 Session。

也不能在 Linux Driver 的 `release()` 路径中直接释放 Secure Kernel 对象。

需要通过适当的协议请求 Secure World 完成清理，并考虑已有 Invocation、RPC 和 Session Reference 的状态。

对不能立即中断的执行路径，应有明确的延迟清理或失败处理策略。

**评判重点：** 能否建立完整的分布式资源生命周期状态机，而不是简单回答“调用 CloseSession 即可”。

## **Question 09 — 多 Guest VM 共享 OP-TEE 时，现有 Session Model 还能直接使用吗？**

Virtualization & Isolation

### **场景**

一台服务器运行两个 Guest VM：

```
Guest VM-A
Guest VM-B
     |
     v
Hypervisor
     |
     v
OP-TEE
```

两个 VM 都运行了同一个 UUID 的 CA，尝试访问同一个 Secure Service。

系统架构团队准备复用现有的 OP-TEE Session Manager。

请评估：

1. UUID 能否作为区分不同 Guest 的身份？
2. 两个 Guest 是否应该共享 TA Context？
3. Session ID 是否需要具有 VM 级命名空间？
4. 如何防止一个 Guest 使用另一个 Guest 的 Session ID？
5. Shared Memory 如何绑定到 Guest Memory Ownership？
6. Guest 被销毁时，谁负责 OP-TEE 内部资源清理？
7. Guest Reset 与 OP-TEE Thread Resume 之间可能有什么竞态？
8. 如果 Hypervisor 自身不可信，现有方案是否仍然成立？

### **参考分析要点**

最重要的是区分：

```
TA Identity
Client Identity
VM Identity
Session Identity
Memory Ownership
```

它们是不同层次的概念。

UUID 标识目标 TA，不应该被用于区分 Guest 身份。

多 VM 模型需要明确哪个组件可以可信地提供 VM Identity，以及 Session、Memory 和生命周期如何关联到对应 VM。

如果 Hypervisor 不在 TCB 中，就需要进一步讨论是否具备硬件支持的独立认证与隔离机制。

**评判重点：** 能否识别单一 Normal World 模型下隐藏的身份和资源所有权假设，以及这些假设在虚拟化后是否仍成立。

## **Question 10 — 设计一个可移植的 RISC-V OP-TEE 安全服务架构**

Capstone Architecture Review

### **场景**

一个 RISC-V Server SoC 包含：

- 多个 Application Harts。
- OpenSBI。
- Linux。
- OP-TEE。
- 一个独立的 RSS/HSM Processor。
- SMMPT 或等效系统级内存保护机制。
- 一个 Secure Crypto Engine。
- 一个提供 Firmware Variable 服务的安全存储设备。

现在希望提供三个安全能力：

A. User TA 使用设备安全密钥完成签名。

B. UEFI 使用受保护的 Variable Storage。

C. Linux 通过标准 TEE Client API 调用 Secure Service。

请给出一份完整的软件架构方案。

要求讨论：

1. OP-TEE Core、User TA、PTA、RSS/HSM 应如何分工？
2. 哪些接口属于 GP API，哪些可能需要 Firmware-specific Protocol？
3. HSM 应该把原始密钥暴露给 OP-TEE，还是提供不可导出的 Key Handle？
4. Linux、OpenSBI、OP-TEE、HSM 之间谁应该拥有访问控制的最终决定权？
5. CPU Memory Isolation 与 Device Memory Isolation 如何组合？
6. 如何避免一个普通 User TA 访问其他 TA 的密钥？
7. 如果 OP-TEE 的执行权限不是传统 Arm S-EL1，哪些抽象仍然可以复用？
8. 如果 HSM 不响应，OP-TEE 应如何处理正在执行的 Session？
9. 如果未来采用不同的 RISC-V TEE Domain 实现，如何降低迁移成本？
10. 如何测试整个架构的 Confidentiality、Integrity、Availability、Isolation 和 Recovery？

### **参考分析要点**

高质量方案不应该将所有安全职责都放在 OP-TEE Core 中。

可以考虑：

```
Linux CA                 UEFI
   |                       |
   v                       v
GP Client API       Firmware Service ABI
   |                       |
   +-----------+-----------+
               |
               v
        OP-TEE Services
               |
       +-------+-------+
       |               |
       v               v
   User TA       Privileged Service
                       |
              +--------+--------+
              |                 |
              v                 v
        Crypto Backend     Storage Backend
              |                 |
              v                 v
          RSS / HSM       Protected Storage
```

但这个逻辑结构还必须进一步回答：

- OP-TEE 如何认证请求 HSM 服务的主体？
- HSM 如何确定 OP-TEE 发来的请求可信？
- HSM Key Handle 是否绑定具体服务或身份？
- UEFI 与 Linux 的身份体系是否相同？
- 系统级内存权限由谁维护？
- 权限变化如何与 CPU、设备缓存和转换状态同步？
- OP-TEE Core Crash 后是否需要 HSM Re-Synchronization？

在这里，“安全地调用 HSM”并不只是发送一个 Mailbox Message。

它需要完整的 Identity、Authorization、State Synchronization 和 Error Recovery 机制。

**评判重点：** 能否建立一个具有明确 TCB、权限边界、协议契约和可验证安全不变量的系统，而不仅是一张软件模块调用图。

# **2.14 Staff Engineer 考核评分标准**

这些题目不适合按照“是否与参考答案一致”评分。

更合理的方式是从四个维度评估。

综合能力评估

每个维度按其最高权重评分

# **0 / 100**

Further practice recommended

Architecture Reasoning

0 / 30

能够划分职责、解释抽象、发现隐藏依赖与不合理耦合

Security & Failure Analysis

0 / 30

能够识别信任假设、攻击面、异常状态和安全不变量

Trade-off & Decision Quality

0 / 20

能够提出多种方案，解释代价并给出清晰的决策依据

Verification Strategy

0 / 20

能够提出可以实际实施的验证、测试和故障注入方法

建议进一步强化系统建模与源码实践

重置

建议的评分方式：

- **90–100：** 能够主动质疑需求中的安全假设，对多种方案进行系统级权衡，并提出可验证的架构。
- **75–89：** 能够正确处理主要模块边界、资源生命周期和异常情况，具备较强的跨模块分析能力。
- **60–74：** 对基本架构有理解，但容易遗漏特定信任边界或并发失败路径。
- **60 以下：** 可能仍主要依靠经验和局部知识作答，需要强化系统级推理。

这里的分数不是 OP-TEE 社区对工程师等级的官方定义，而是我们学习时使用的自评框架。

# **2.15 Chapter 02 总结**

本章最值得记住的，不是十几个组件的名字，而是以下五条设计原则。

**第一：模块划分的本质是职责分离。**

`libteec`、Linux Driver、OP-TEE Core、TA Runtime 和 tee-supplicant 分别负责不同层次的问题。代码组织应该尽可能反映这些责任边界。

**第二：接口边界不等于信任边界。**

API、ABI、Privilege、Trust 是四种不同的概念。一个跨模块调用可能同时跨越多种边界。

**第三：安全操作可以依赖不可信机制，但不能依赖不可信安全决策。**

例如通过 Linux 访问存储设备，并不意味着允许 Linux 决定安全密钥、数据完整性或访问授权。

**第四：跨模块协作最难的部分通常是状态与资源生命周期。**

Thread、Session、TA Context、Memory Object 和 RPC Buffer 都有各自的生命周期。理解这些对象的所有权，是维护 OP-TEE Core 的基本能力。

**第五：架构的好坏最终需要通过异常路径检验。**

正常执行时能够成功调用 TA，并不能证明系统设计合理。真正的考验发生在并发、崩溃、资源耗尽、恶意输入、升级和设备异常时。

## **本章核心源码参考**

- [OP-TEE 4.11.0 — Entry Dispatcher](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/tee/entry_std.c?utm_source=chatgpt.com)
- [OP-TEE 4.11.0 — TA Manager](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/tee_ta_manager.c?utm_source=chatgpt.com)
- [OP-TEE 4.11.0 — REE TA Loader](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/ree_fs_ta.c?utm_source=chatgpt.com)
- [OP-TEE Client — libteec & tee-supplicant](https://github.com/OP-TEE/optee_client?utm_source=chatgpt.com)
- [OP-TEE Core Architecture Documentation](https://optee.readthedocs.io/en/4.11.0/architecture/core.html?utm_source=chatgpt.com)
- [Trusted Applications Architecture](https://optee.readthedocs.io/en/4.11.0/architecture/trusted_applications.html?utm_source=chatgpt.com)

### **下一章预告**

**Chapter 03 — OP-TEE OS 源码树与核心数据结构全景**

本章我们已经建立了软件模块之间的关系。

下一章将把这些抽象真正映射到 OP-TEE 源码上，特别是 `thread_ctx`、`tee_ta_session`、`tee_ta_ctx`、`ts_ctx`、`mobj`、`user_ta_ctx` 等关键结构体之间的组织关系。

届时我们不再只追踪函数，而要建立 **Object Relationship Graph、Ownership Graph 和 Execution Context Graph**。

这是从“知道 OP-TEE 如何调用”走向“理解 OP-TEE 内部如何维护整个运行状态”的关键一步。
