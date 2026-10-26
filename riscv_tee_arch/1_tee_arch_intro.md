# Chapter 01 — RISC-V TEE Architecture

从 Arm TrustZone 到 RISC-V Security Domain：可信执行环境的设计原理、硬件隔离和安全边界

课程定位： Advanced / Staff Engineer / OP-TEE Upstream Developer

本章核心目标： 从架构设计和安全模型出发，理解为什么 RISC-V 即使没有 Arm TrustZone 式的 Secure/Non-secure Security State，仍然可以通过 M-mode Firmware、PMP 和系统级硬件隔离构建可信执行环境。

本章覆盖之前规划的全部八节：

| Section | 主题                                  | 核心问题                                  |
| ------- | ----------------------------------- | ------------------------------------- |
| 1.1     | TEE Security Fundamentals           | TEE 究竟要保护什么？                          |
| 1.2     | Arm TrustZone Architecture          | Arm 的安全隔离机制是如何实现的？                    |
| 1.3     | RISC-V Privilege vs Security Domain | 为什么 M/S/U 不能对应 Secure/Normal？         |
| 1.4     | RISC-V TEE Architecture Models      | RISC-V 可以采用哪些不同的 TEE 设计？              |
| 1.5     | OpenSBI Supervisor Domain           | OpenSBI Domain 的设计理念是什么？              |
| 1.6     | Same-Privilege Isolation            | Linux 和 OP-TEE 都是 S-mode，凭什么隔离？       |
| 1.7     | Hardware Security Boundaries        | PMP、MMU、Firewall、DMA Protection 如何协作？ |
| 1.8     | Arm vs RISC-V Architecture          | 两种体系的安全模型、TCB 和切换机制有何本质区别？            |

需要特别强调一点：TEE 不是一个特权级，也不是一个操作系统名称。TEE 是一组由可信软硬件共同实施的安全保证。

理解这句话，是后面所有章节的基础。

## 1.1 TEE Security Fundamentals — TEE 究竟要解决什么问题？

### 1.1.1 Why：为什么有了 Linux 的 MMU，还需要 TEE？

我们先从一个真实产品场景开始。

假设在一台 Linux 设备上有一个支付服务，内部需要保存设备私钥：

```
Linux Application
    |
    v
Payment Service
    |
    | Sign(transaction)
    v
Private Key
    |
    v
Generate Signature
```

正常情况下，Linux 可以利用 MMU、进程隔离、用户权限、SELinux 等机制保护私钥。

但是，我们引入一个更强的攻击者：

假设 Linux Kernel 已经被完全攻破，攻击者取得了任意 Ring-0 等价的 S-mode Kernel 执行权限。

此时攻击者具备的能力包括：

- 修改 Linux Page Table。
- 修改 Linux Kernel Code 和 Data。
- 访问或映射普通物理内存。
- 控制 Linux Driver。
- Hook System Call、修改进程 Credentials。
- 控制绝大多数由 Linux 管理的外设。
- 伪造 Linux 提供的进程身份信息。

假设私钥保存在 Linux Kernel 能够访问的内存里，那么即使它属于某个受 SELinux 保护的进程，攻击者仍然可能通过控制内核来访问它。

原因很简单：

Linux MMU 和 Linux 安全机制的最终管理者，恰恰就是已经被攻破的 Linux Kernel。

这意味着，传统的进程权限隔离无法在内核本身不可信时继续提供独立的安全保证。

TEE 就是为了解决这一类问题而出现的。

### 1.1.2 What：TEE 必须具有独立于 REE 的安全控制权

TEE（Trusted Execution Environment）与 REE（Rich Execution Environment）的逻辑关系可以表示为：

```
             Untrusted REE
       +------------------------+
       | Linux Application      |
       | Linux Kernel           |
       | Linux Drivers          |
       +------------------------+
                   |
                   | Controlled Interface
                   v
       +------------------------+
       | Trusted Execution      |
       | Environment            |
       |                        |
       | OP-TEE Core            |
       | Trusted Applications   |
       | Secure Services        |
       +------------------------+
                   |
                   v
       +------------------------+
       | Hardware-Enforced      |
       | Security Boundary      |
       +------------------------+
```

请注意最后一层。

TEE 的真正安全基础不是 OP-TEE 这份软件，而是 Linux 无法控制的硬件安全边界。

OP-TEE 是利用这一边界提供安全执行服务的软件。

如果攻击者能够通过修改 Linux 的页表，直接映射并读取 OP-TEE 的物理内存，那么无论 OP-TEE 的代码质量有多高，它都无法提供预期的隔离保证。

因此，TEE 系统至少需要满足以下条件：

| 安全性质                     | 核心要求                       |
| ------------------------ | -------------------------- |
| Confidentiality          | REE 不能直接读取 TEE 私有数据        |
| Integrity                | REE 不能直接修改 TEE 代码、数据和执行上下文 |
| Controlled Entry         | REE 只能通过经过验证的入口调用 TEE 服务   |
| Execution Isolation      | TEE 执行状态不能被 REE 任意访问或控制    |
| Secure Bootstrapping     | 初始 TEE 代码及关键安全配置必须可信       |
| Trusted Resource Control | 密钥、专有外设、敏感存储等资产具有受信任的访问控制  |

在可信计算系统中，还应考虑新鲜性、回滚保护以及可用性。不过，并非所有 TEE 实现都能无条件保证这些性质。

例如，Linux 可以拒绝向 TEE 提供服务，甚至通过拒绝调度请求造成拒绝服务。单靠 CPU Memory Isolation 通常无法防止这种情况。

### 1.1.3 Threat Model：安全分析必须从攻击者能力开始

在架构设计中，我们不能笼统地说：

> OP-TEE 是安全的。

必须先定义：

1. 谁是攻击者？
2. 攻击者可以控制哪些组件？
3. 哪些组件被认为可信？
4. 攻击者试图窃取或破坏哪些资产？
5. 哪些攻击明确不在当前系统的保护范围内？

对本课程而言，我们采用以下基础 Threat Model：

| 组件或能力                             | 基础假设                  |
| --------------------------------- | --------------------- |
| Linux Application                 | 不可信                   |
| Linux Kernel / Root               | 允许被完全攻破               |
| Linux Driver                      | 不可信                   |
| Linux Page Table                  | 可被攻击者任意修改             |
| Linux 控制的普通 DMA Device            | 潜在恶意                  |
| OP-TEE Core                       | 可信，但可能存在需要分析的漏洞       |
| Trusted TA                        | 经过授权、验证，遵守相应信任假设      |
| OpenSBI / M-mode Security Monitor | 可信                    |
| PMP / Memory Protection Hardware  | 可信且配置正确               |
| Boot ROM / Boot Root of Trust     | 可信                    |
| Debug / Physical Attack           | 默认不在基础 Threat Model 内 |

这里有两个容易忽略的地方。

第一，Linux root 不可信，不代表攻击者自动拥有 M-mode 权限。

Linux 通常执行在 S-mode。它可以通过合法 SBI 接口请求 M-mode Firmware 提供服务，但不能直接读取或修改 M-mode 的 PMP CSR。

第二，TEE 的安全性依赖 OP-TEE 和 M-mode Firmware 本身没有被攻击者突破。

如果 OP-TEE 的 Shared Memory 处理存在越界访问漏洞，攻击者仍然可能通过合法的接口触发漏洞，获得 TEE 权限。

OP-TEE 官方的 Security Scope 也明确将跨隔离域的机密性、完整性、可用性破坏以及 Confused Deputy 问题视作关键安全分析对象。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### 1.1.4 TCB：为什么要尽量缩小 Trusted Computing Base？

TCB = Trusted Computing Base，可信计算基。

可以将它理解为：

只要其中任何关键组件被突破，就可能破坏系统安全保证的那部分软硬件集合。

在一个典型的 RISC-V OP-TEE 系统中，TCB 可能包含：

```
Hardware Root of Trust
          |
          v
Boot ROM / Verified Boot Chain
          |
          v
M-mode Trusted Firmware
    OpenSBI + Isolation Logic
          |
          v
OP-TEE Core
          |
          v
Trusted Applications
          |
          v
Trusted Keys / Security Services
```

这只是逻辑上的依赖图，不意味着每个 TA 对所有其他 TA 都拥有相同的信任关系。

从系统级的角度，还必须考虑 PMP、Memory Controller、外设隔离单元、调试锁定策略以及其他参与实现信任边界的硬件。

这里存在一个重要的架构权衡：

- 将全部 TEE 服务放在 M-mode，虽然拥有最高权限，但会扩大高权限代码的攻击面。
- 将 OP-TEE 运行在 S-mode，把安全隔离决策交给相对精简的 M-mode Firmware，可以减少必须在 M-mode 执行的业务逻辑。
- 但是这种分层设计又增加了 Domain Switching、状态管理、通信接口和跨层安全验证的复杂度。

这正是 OpenSBI + S-mode OP-TEE 设计路线的一个核心动机。

### 1.1.5 两种隔离目标：REE vs TEE，以及 TA vs TA

理解 OP-TEE 时，还应该把两种安全隔离明确分开：

Boundary A — REE / TEE Isolation

保护 OP-TEE 整个安全执行环境，防止被已经攻破的 Linux Kernel 访问或修改。

主要依靠：PMP、平台硬件隔离、可信 M-mode Firmware

Boundary B — TA / TA Isolation

在 OP-TEE Domain 内隔离多个 User TA，并将它们与 OP-TEE Core 区分开。

主要依靠：RISC-V U/S 权限、MMU、TA Loader、OP-TEE Kernel

这两种隔离通常不是同一套机制。

例如：

- 防止 Linux 读取 TA 私钥，通常依靠 REE/TEE 的物理内存隔离。
- 防止 TA-A 读取 TA-B 的私钥，依靠 OP-TEE 内部的地址空间和权限隔离。

这种分层对于后续分析 PMP 与 MMU 至关重要。

Section 1.1 的核心结论：

TEE 并不是一个「比 Linux 权限更高的操作系统」，而是一个能够在 Linux 不可信的情况下仍维持特定安全保证的执行环境。其根基在于独立的硬件保护机制与可信的隔离管理者。

## 1.2 Arm TrustZone Architecture — 为什么 Arm 能够直接建立两个 World？

在学习 RISC-V 之前，我们先分析 Arm 已经相对成熟的方案。

这既可以复用你已有的 TrustZone、TF-A 和 OP-TEE 经验，也能帮助我们发现 RISC-V 与 Arm 在设计理念上的本质区别。

### 1.2.1 Arm 的核心设计：Privilege Level 与 Security State 分离

在 Armv8-A 中，CPU 不只有 EL0、EL1、EL2、EL3 这几个 Exception Level。

它还具有独立的 Security State。

简化来看：

Non-secure World

NS-EL2 · Hypervisor

NS-EL1 · Linux

NS-EL0 · Applications

Secure World

S-EL2 · Optional

S-EL1 · OP-TEE

S-EL0 · Trusted TA

EL3 — Secure Monitor / TF-A BL31

Security State Switch · Secure Monitor Context

以传统双 Security State 的 Armv8-A TrustZone 模型为例；S-EL2 是可选能力，Arm CCA 等扩展引入的其他状态不在此图中。

其核心思想是：

EL 描述软件在某个 Security State 内的特权，而 Security State 则用于区分不同安全域的硬件访问权限。

因此，虽然 Linux 和 OP-TEE 通常都运行在 EL1，但：

`NS-EL1` ≠ `S-EL1`

它们不是同一个安全执行环境。

Arm 的架构规范明确说明，Security State 的切换需要经过 EL3，并由 Secure Monitor 管理相应的状态转换。

[image](https://www.google.com/s2/favicons?domain=https://developer.arm.com\&sz=32)

Arm开发者



### 1.2.2 World Switch 如何发生？

传统 Arm64 OP-TEE 调用的大体过程如下：

```
Linux NS-EL1
    |
    | SMC
    v
EL3 / TF-A BL31
    |
    | Save Non-secure Context
    | Select Secure Context
    | Update Security State
    | Restore Secure Context
    |
    | ERET
    v
OP-TEE S-EL1
```

对应的核心架构状态包括：

- `SCR_EL3.NS`：选择返回到 Secure 还是 Non-secure Security State。
- `ELR_EL3`：保存 EL3 异常返回地址。
- `SPSR_EL3`：保存异常返回需要恢复的 PSTATE。
- `SP_ELx`：不同异常级别的栈上下文。
- Security State 所关联的系统寄存器和异常处理配置。

这里需要区分两个步骤：

`SMC` 的作用是触发进入 Monitor 的受控异常路径。

真正的 World Switch 是 Monitor 在保存、选择和恢复上下文之后，通过异常返回完成的。

### 1.2.3 为什么 Non-secure Linux 无法读取 Secure DRAM？

假设 OP-TEE 的 Secure DRAM 中有一个私钥：

```
Physical Address: 0x90000000
Content: TEE Private Key
```

攻击者已经完全控制 Linux Kernel。

攻击者可以修改 Linux 的页表：

```
Linux VA
    |
    | PTE -> PA 0x90000000
    v
Secure DRAM
```

但是能建立 VA→PA 映射，并不代表最终物理访问一定被允许。

在一个正确配置的 TrustZone SoC 上：

```
CPU (Non-secure)
      |
      | Memory Request
      | Security Attribute = Non-secure
      v
Interconnect / Memory Protection Controller
      |
      | Target PA = Secure DRAM
      v
Access Denied
```

总线事务所携带的安全属性，以及内存控制器的安全区域配置，共同阻止了这种访问。以典型 Arm SoC 为例，这类机制可以涉及 TZASC/TZC、系统互连和外设安全控制器。

[image](https://www.google.com/s2/favicons?domain=https://ssg.aalto.fi\&sz=32)

ssg.aalto.fi



因此：

Linux 无法读取 Secure DRAM，并不是因为它不知道地址，也不是因为 Linux 页表没有映射，而是因为更底层的安全权限检查拒绝了访问。

这里也可以看出 TrustZone 的一个重要优点：

Security State 可以沿着 SoC 的互连和设备访问链路传播，因此它不仅影响 CPU 内存访问，还可以参与系统级安全资源隔离。

当然，实际保护效果仍取决于 SoC 是否正确配置了 Secure RAM、DMA、外设权限和调试接口。

### 1.2.4 TrustZone 的局限：Security State 并不等于完整 TEE

虽然 TrustZone 提供 Secure/Non-secure 的硬件分离，但它本身不能解决以下问题：

- 两个 Trusted Application 之间的安全隔离。
- OP-TEE Core 中的软件漏洞。
- TA 私钥的生命周期管理。
- 不可信 Shared Memory 参数的校验。
- 不可信 CA 的身份认证。
- Secure Storage 的回滚问题。
- 所有微架构侧信道。

因此：

Arm TrustZone 负责提供关键的硬件安全机制，OP-TEE 负责在其上构建安全操作系统及服务。

这也是我们后面比较 RISC-V 时必须保留的区别。

## 1.3 RISC-V Privilege vs Security Domain — 为什么 M/S/U 不能直接对应 Secure/Normal World？

这是整个 Chapter 01 中需要重点解决的概念问题。

你已经熟悉 RISC-V 的 M/S/U Privilege Architecture，因此这里不再重复基本 CSR 的含义，而是从安全架构角度重新理解它们。

### 1.3.1 RISC-V Privilege Architecture 解决的是什么问题？

RISC-V 定义的典型特权层次为：

```
          M-mode
     Machine Firmware
     OpenSBI / Monitor
             |
             v
          S-mode
       Linux Kernel
             |
             v
          U-mode
      User Application
```

这套体系首先解决的是：

不同特权级的软件，谁能够执行哪些特权操作，以及发生 Trap 时如何转移控制权？

例如：

| 特权操作         | M-mode      | S-mode   | U-mode |
| ------------ | ----------- | -------- | ------ |
| 修改 `mstatus` | 允许          | 不允许      | 不允许    |
| 修改 `pmpcfg`  | 允许          | 不允许      | 不允许    |
| 修改 `satp`    | 通常可访问对应 CSR | 允许，受配置约束 | 不允许    |
| 修改 `stvec`   | 允许          | 允许       | 不允许    |
| 执行 `mret`    | 允许          | 不允许      | 不允许    |
| 执行 `sret`    | 允许          | 通常允许     | 不允许    |
| 执行 `ecall`   | 允许          | 允许       | 允许     |

具体还受到可选 ISA 扩展、CSR 状态及架构限制的影响。

这些规则建立了一种纵向的 Privilege Hierarchy。

但是，请看另一个问题：

两个彼此不信任的操作系统，能不能都运行在 S-mode？

答案是可以。

例如：

```
            M-mode
      Trusted Firmware
          /      \
         /        \
        v          v
  Linux Kernel   OP-TEE Core
    S-mode         S-mode
```

虽然它们具有相同的 CPU Privilege Level，但我们希望它们属于不同的 Security Domain。

这就引出了一个非常重要的结论：

Privilege Level 描述执行权限，而 Security Domain 描述安全资源的所有权和访问边界。两者是不同的概念。

RISC-V Privileged Architecture 本身也明确指出，M-mode 可以用于管理安全执行环境，但 ISA 的基础特权级模型主要是传统的分层保护模型，并不等于一个预先定义好的双 World 安全体系。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



### 1.3.2 为什么 M-mode 不应该简单理解成 Secure World？

一种常见的理解方式是：

```
Arm                     RISC-V

Secure World   <---->   M-mode
Normal World   <---->   S-mode / U-mode
```

这个类比可以辅助入门，但在架构上并不准确。

原因之一是，M-mode 更接近系统最高权限的可信管理层，而不是必须承载全部安全应用的 Secure World。

我们可以把 OP-TEE 部署在 S-mode，让 OpenSBI 在 M-mode 提供隔离和切换服务。

此时：

```
M-mode:
    OpenSBI

S-mode:
    Linux
    OP-TEE

U-mode:
    Linux Applications
    Trusted Applications
```

Security Domain 与 Privilege Level 之间是交叉关系，而不是简单的一一映射。

### 1.3.3 为什么 Linux 和 OP-TEE 不能共享同一个不受限制的 S-mode 执行环境？

假设有如下系统：

```
Physical Memory:

0x80000000 - 0x87FFFFFF  Linux RAM
0x88000000 - 0x88FFFFFF  OP-TEE RAM
```

如果仅仅分别运行两个 S-mode Kernel，但没有额外的硬件物理内存隔离，会出现什么问题？

Linux 可以修改自己的 `satp` 和 Page Table：

```
Linux Virtual Address:
    0xFFFF_FFC0_1000_0000

             |
             | PTE
             v

Physical Address:
    0x88000000
```

此时 Linux 就可能访问 OP-TEE RAM。

Linux 的访问甚至不需要知道 OP-TEE 使用什么虚拟地址。

只要目标物理地址可以访问，它就可以建立自己的页表映射。

所以问题的本质是：

Linux 和 OP-TEE 都是 S-mode，不能依赖各自可修改的页表来相互隔离。

必须存在一个权限高于双方、而且能够控制物理资源访问的可信管理者。

这正是 M-mode Firmware 和 PMP 发挥作用的地方。

### 1.3.4 从纵向隔离扩展到横向隔离

可以将 RISC-V TEE 的安全结构理解为两个维度。

两个正交的隔离维度

- 纵向隔离：M-mode、S-mode、U-mode 之间的权限管理。
- 横向隔离：Linux Domain、OP-TEE Domain 之间的资源和安全边界。

最终 RISC-V TEE 系统需要同时利用这两个维度。

### 1.3.5 H Extension 能否代替这种隔离？

RISC-V H Extension 提供 HS/VS/VU 等虚拟化执行环境。

例如：

```
M-mode
   |
HS-mode (Hypervisor)
   |
   +---- VS-mode Guest Linux
   |
   +---- VS-mode Guest OS
```

Hypervisor 可以通过 Stage-2 Translation 控制 Guest 所能访问的物理内存。

但是：

虚拟化隔离不自动等于可信执行环境。

如果运行在 HS-mode 的 Hypervisor 已经被攻破，那么它可能控制 Guest 的 Stage-2 Page Table，从而读取 Guest Memory。

因此，常规 H Extension 的典型信任关系是：

```
Guest trusts Hypervisor
```

而真正希望抵抗 Host/Hypervisor 攻击的 Confidential Computing 系统，必须引入相对于不可信 Host 独立的隔离控制机制。

这也是为什么不能认为「RISC-V 有 H Extension，所以天然就有 TEE」。

## 1.4 RISC-V TEE Architecture Models — 有哪些不同的实现路线？

RISC-V 的设计相比 Arm TrustZone 更具组合性。

CPU Privilege Architecture 提供基本机制，但具体的 TEE Architecture 可以通过不同方式构建。

目前可以把常见路线概括成四类。

### 1.4.1 Model A：M-mode Trusted Firmware / Secure Service

第一种设计是直接让可信服务运行在 M-mode。

```
            M-mode
    +----------------------+
    | Secure Firmware      |
    | Key Management       |
    | Security Services    |
    +----------------------+
                ^
                | ECALL / Controlled Interface
                v
            S-mode
    +----------------------+
    | Linux                |
    +----------------------+
```

这种设计的优点是：

- 安全服务位于最高特权级。
- 可以直接访问受 M-mode 控制的安全资源。
- 对于简单嵌入式设备，实现可能比较直接。

缺点也很明显。

M-mode 拥有极高系统权限，如果在其中实现大量复杂的安全业务逻辑，会扩大最高权限层的攻击面。

此外，M-mode 不适合无条件地承载一个复杂的传统应用执行环境。

因此，这种架构很适合较小的 Trusted Firmware、Security Monitor 或特定安全服务，但不一定是部署完整 OP-TEE OS 的最佳选择。

### 1.4.2 Model B：M-mode Security Monitor + S-mode TEE

这是本课程重点采用的架构。

```
               M-mode
      +-----------------------+
      | OpenSBI               |
      | Domain Management     |
      | PMP / HW Isolation    |
      | Domain Context Switch |
      +-----------------------+
                / \
               /   \
              v     v
          S-mode   S-mode
         +------+ +--------+
         |Linux | | OP-TEE |
         +------+ +--------+
            |         |
            v         v
          U-mode    U-mode
           Apps       TA
```

这种架构的设计理念是：

把 Security Policy 的实施和安全执行服务分离。

M-mode 负责：

- 定义和管理 Security Domains。
- 管理跨 Domain 的切换。
- 配置物理资源的访问权限。
- 保护关键安全上下文。
- 管理必要的 Hart 和 Interrupt 路由。

OP-TEE S-mode 负责：

- Trusted Thread。
- User TA。
- TEE Internal API。
- TA Memory Isolation。
- Secure Storage、Cryptography 等安全服务。

这样 OP-TEE 不需要成为系统最高特权软件。

不过，需要注意一个重要区别：

OpenSBI 原本是 SBI Firmware，并不是只为 OP-TEE 设计的 Security Monitor。

要可靠支持 S-mode TEE，需要额外的 Domain Switching、Interrupt Management、Hardware Isolation 和 TEE 通信能力。

RISE RP016 的目标正是增强 OpenSBI，使其具备运行典型 S-mode TEE 所需的基础设施；RP022 则继续完善 RISC-V OP-TEE 本身的能力。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE

+1



### 1.4.3 Model C：Enclave-based TEE

另一个值得了解的方案是 Keystone Enclave。

[Keystone Enclave | Luca Giorgino](https://images.openai.com/static-rsc-4/2XbWjPqLaKuDRCNqJlKjXRHAOBl7V4BO5lW16CuwUDBV_eJce9BWnzP0EFQIEfqfOk_Sc0RZ4XzA30ZY8PUqMnyRbcPrLnWGAbAFVsrYcHMtDahJPymSeOC1d1tn1EuzMVdbgnxpHHcdvR7Q9isTdF7X2gXwVp8C47WHqzSM3Mc?purpose=inline)

Keystone 的核心思想是：

允许在不可信操作系统之外建立一个受 M-mode Security Monitor 保护的 Enclave。

例如：

```
         Untrusted Domain
      +-------------------+
      | Linux             |
      | Host Application  |
      +-------------------+
                |
                | SBI / SM Call
                v
      +-------------------+
      | M-mode            |
      | Keystone SM       |
      | PMP Management    |
      +-------------------+
                |
                v
         Trusted Enclave
      +-------------------+
      | Enclave Runtime   |
      | Enclave App       |
      +-------------------+
```

Keystone 与 OP-TEE 有相似的安全目标，但不是相同的编程模型。

Keystone 更强调独立 Enclave 的生命周期、内存隔离和 Attestation；OP-TEE 更强调提供完整的 TEE OS 与 GlobalPlatform 风格的安全服务环境。

Keystone 的官方 Security Monitor 设计同样以 M-mode 和 PMP 为隔离基础，还包括 Enclave Context Management、Attestation 等能力。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



另一个例子是 Penglai Enclave，其不同实现路线使用 PMP/sPMP 或更细粒度的内存隔离机制。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



值得注意的是，不能因为某个系统使用了「Enclave」这个名称，就认为它自动拥有完整 TEE 的所有保证。仍然需要检查硬件隔离、可信启动、上下文清理、密钥来源和 Attestation 等具体实现。

### 1.4.4 Model D：Dedicated Secure Processor / HSM Core

还有一种方案是使用独立的安全处理器。

例如你比较熟悉的集成 HSM 的 SoC，可以设计成：

```
Application CPU Cluster
    |
    +-- Linux
    |
    +-- OP-TEE (optional)
    |
    | Secure IPC / Mailbox
    v
Independent Security Core
    |
    +-- Secure Firmware
    +-- Key Management
    +-- Cryptography
    +-- Boot Authentication
```

此时安全边界可以主要来自两个不同的物理执行实体，以及它们之间的内存访问控制、总线权限、Mailbox 和专有密钥存储。

这种方案特别适合 Root of Trust、Secure Boot、Device Identity 等功能。

但需要注意：

独立 HSM Core 不必运行 OP-TEE，也不代表应用 CPU 上的全部软件就自动获得 TEE 隔离。

如果未来你的实际工作涉及独立 RISC-V HSM Core 和主 CPU 的 OP-TEE 集成，就需要同时分析两个不同的安全边界：

1. Linux ↔ OP-TEE。
2. OP-TEE ↔ HSM。

这属于完整的 SoC Security Architecture，而不只是 OP-TEE 的平台移植。

### 1.4.5 四种设计路线如何选择？

| 架构                    | 主要隔离机制                                 | 典型优点               | 主要代价                   |
| --------------------- | -------------------------------------- | ------------------ | ---------------------- |
| M-mode Secure Service | Privilege + M-mode Protection          | 简单、直接、可信路径短        | M-mode 攻击面可能扩大         |
| S-mode OP-TEE         | M-mode Domain + PMP/Hardware Isolation | 可复用 OP-TEE、分离管理与服务 | Domain Switch、系统集成复杂   |
| Enclave-based         | M-mode Monitor + PMP/其他隔离              | 适合细粒度可信工作负载        | Runtime 和 Enclave 管理复杂 |
| Dedicated HSM         | 独立处理器 + SoC Security                   | 硬件安全边界强、可独立保护密钥    | IPC、硬件成本、集成复杂          |

对于我们的课程，接下来全部以 Model B：OpenSBI + S-mode OP-TEE 为主要研究对象。

## 1.5 OpenSBI Supervisor Domain — OpenSBI 如何建立 Security Domain？

我们先理解 OpenSBI Domain 最初要解决的问题。

### 1.5.1 Why：为什么 OpenSBI 需要 Domain？

假设有一颗具有四个 hart 的 RISC-V SoC。

我们想让两个 hart 运行 Linux，另外两个 hart 运行一个独立的实时系统：

```
Hart 0 / Hart 1       Hart 2 / Hart 3
      |                     |
      v                     v
 Linux Domain         RTOS Domain
      |                     |
      +----------+----------+
                 |
                 v
               OpenSBI
```

这时就会出现一些问题：

- Linux 是否能够访问 RTOS 的 RAM？
- Linux 是否能够通过 SBI HSM 停止 RTOS 所使用的 hart？
- Linux 是否能够向 RTOS 的 hart 发送 SBI IPI？
- Linux 是否可以要求对 RTOS 所管理的地址空间执行 Remote Fence？
- 两个系统能否分别拥有不同的设备？

OpenSBI Domain 的目标，就是将硬件资源和 Hart Ownership 组织成不同的系统级执行分区。

OpenSBI 官方定义的 Domain 包含自己的 Memory Regions、设备和 Hart 集合，并利用 PMP、ePMP、IOPMP 等平台机制实施隔离。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 1.5.2 What：一个 OpenSBI Domain 包含哪些内容？

从架构上看，可以抽象为：

```
/* 概念结构，不是源码的逐字段复制 */

struct security_domain {
    int domain_id;

    hart_set_t possible_harts;
    hart_set_t assigned_harts;

    memory_region_t *regions;

    physical_addr_t boot_entry;
    privilege_mode_t next_mode;

    bool reset_allowed;
    bool suspend_allowed;
};
```

这个模型表达了三类重要信息。

第一类：Domain Identity

用于表示一个独立的运行环境。

第二类：Resource Ownership

规定 Domain 可以访问哪些内存和设备，以及哪些 hart 可以运行这个 Domain。

第三类：Execution Policy

描述这个 Domain 如何启动、允许哪些系统级操作，以及如何管理 Hart Lifecycle。

实际 OpenSBI 使用 `struct sbi_domain`、`struct sbi_domain_memregion` 等数据结构。可以在 [OpenSBI Domain Support](https://github.com/riscv-software-src/opensbi/blob/master/docs/domain_support.md?utm_source=chatgpt.com) 中查看完整定义。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 1.5.3 OpenSBI Domain 不是简单的 Linux Process

这个区别很重要。

Linux Process 隔离主要依靠 MMU：

```
Process A -> Page Table A
Process B -> Page Table B
```

Linux Kernel 是二者的共同管理者。

而 OpenSBI Domain 隔离的是 S-mode 软件环境本身：

```
Domain A
    Linux Kernel
    Linux Page Table
    Linux User Processes

Domain B
    OP-TEE Kernel
    OP-TEE Page Table
    Trusted Applications
```

每一个 Domain 都可以拥有自己的 S-mode Runtime。

而控制不同 Domain 物理权限和切换的，则是更高可信层级的 M-mode Firmware。

因此，OpenSBI Domain 更接近一个可以具有独立操作系统内核的物理资源安全分区，而不是 Linux 进程。

### 1.5.4 为什么必须区分 Assigned Hart 与 Possible Hart？

这里是 OpenSBI Domain 架构中非常值得理解的细节。

假设：

```
Domain Linux:
    possible_harts = {0,1,2,3}
    assigned_harts = {0,1}

Domain OP-TEE:
    possible_harts = {0,1,2,3}
    assigned_harts = {2,3}
```

`possible_harts` 表示哪些 hart 允许运行该 Domain。

`assigned_harts` 表示某个时间点哪些 hart 当前归属于该 Domain。

当发生 Domain Switch 时，Hart 0 可以从 Linux Domain 切换到 OP-TEE Domain，只要策略允许：

```
Before:
    Hart 0 -> Linux Domain

After:
    Hart 0 -> OP-TEE Domain
```

因此，Domain 并不总是对 CPU 核心做静态划分。

它还可以在一个 hart 上进行时间复用。

这里有一个非常重要的设计关系：

一个 hart 在某个时刻只能处于一个 OpenSBI Domain Context，但一个 Domain 可以同时运行多个 hart。&#x20;

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 1.5.5 Domain Context Switch 是什么？

假设 Hart 0 正在执行 Linux。

Linux 发起一个 TEE 请求。

在基于 OpenSBI 的方案中，执行链可以表示为：

```
             Hart 0
                |
                v
        Linux S-mode
                |
                | ECALL
                v
        OpenSBI M-mode
                |
                | Save Linux Context
                | Select OP-TEE Domain
                | Restore OP-TEE Context
                | Configure PMP / HW Isolation
                | Synchronize Address Translation
                |
                | MRET
                v
        OP-TEE S-mode
```

请注意这里最重要的一点：

Domain Switch 不仅要切换软件上下文，还要切换硬件资源访问权限。

如果只是切换了 `satp`、`sp` 和 `sepc`，但没有更新必要的硬件安全配置，就无法保证真正的 Domain Isolation。

### 1.5.6 对照实际 OpenSBI 源码

你可以查看：

[OpenSBI — sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)

其中实际存在以下函数：

```
sbi_domain_context_enter(...)
sbi_domain_context_exit(...)

switch_to_next_domain_context(...)
```

在 `switch_to_next_domain_context()` 的实现中，可以观察到以下关键行为：

```
Update Domain / Hart Assignment
             |
             v
Save / Restore S-mode CSRs
             |
             v
Save / Restore FP Context
             |
             v
Save / Restore Vector Context
             |
             v
Save / Restore Trap Context
             |
             v
Reconfigure PMP Protection
             |
             v
Continue Target Domain
```

源码中确实会操作 `sstatus`、`sie`、`stvec`、`sscratch`、`sepc`、`scause`、`stval`、`sip`、`satp` 等 CSR，并调用 `sbi_hart_protection_reconfigure()`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这说明 OpenSBI Domain 不是一个纯粹的配置概念，而是一个实际参与 Hart Execution Context Management 的内核机制。

特别值得思考：

为什么 Domain Switch 除了 `satp` 之外，还必须保存 `stvec`？

因为 Linux 和 OP-TEE 各自拥有自己的 S-mode Trap Handler。

如果恢复了 OP-TEE 的代码执行状态，却仍使用 Linux 的 `stvec`，那么 OP-TEE 发生 Trap 时就可能跳到错误的异常处理入口。

同理，`sscratch`、`sstatus` 以及 FP/Vector State 也属于安全上下文的一部分。

### 1.5.7 一个重要边界：OpenSBI Domain 不自动等于安全 TEE

这里必须指出一个容易误解的事实。

OpenSBI 支持 Domain，并不意味着只要创建两个 Domain，就已经完成一个安全的 OP-TEE 实现。

例如，OpenSBI 默认的 ROOT Domain 允许访问广泛的 S/U 物理地址空间（保留对 OpenSBI 固件的保护区域）。如果没有正确配置独立 Domain 和物理资源访问规则，仅有 Domain 名称并不能保护 OP-TEE 内存。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



要达到 TEE 的要求，还需要确保：

- REE Domain 无法访问 TEE Private Memory。
- 不可信外设无法通过 DMA 绕过保护。
- Domain Switch 时不会泄露寄存器和其他硬件状态。
- Linux 无法通过 SBI 接口改变 OP-TEE 的安全策略。
- TEE 的初始代码、页表和权限配置是可信的。
- 中断、SMP 和 Device Ownership 满足安全约束。

这也是 RP016 后续引入 System-Level Hardware Isolation Framework 的原因。

它允许平台级保护机制参与 Domain 初始化以及 `domain_exit`、`domain_enter` 等生命周期操作。相关工作在 2026 年公开文档中仍有处于 upstream review 的部分，不能简单认为全部硬件后端已进入稳定主线。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



Section 1.5 的核心结论：

OpenSBI Domain 是一个 M-mode 管理的系统级资源和执行上下文抽象。它为建立隔离的 S-mode TEE 提供基础，但 TEE 的实际安全保证还取决于 PMP、系统级外设保护、可信启动和完整的 Domain Switch 实现。

## 1.6 Same-Privilege Isolation — Linux 和 OP-TEE 都是 S-mode，为什么仍然可以隔离？

这是本章最核心的技术问题。

我们现在把之前的概念串联起来，通过一个实际的内存访问案例，完整分析 RISC-V TEE 的保护机制。

### 1.6.1 首先建立一个具体的物理内存布局

假设 SoC 具有以下物理地址布局：

示例 SoC Physical Memory Map

Linux RAM

0x80000000–0x8FFFFFFF

256 MiB · REE Private

OP-TEE RAM

0x90000000–0x90FFFFFF

16 MiB · TEE Private

Shared Memory

0xA0000000–0xA00FFFFF

1 MiB · Shared Buffer

教学用的假设内存布局，非特定 SoC 的真实内存映射；暂时忽略其他固件 RAM、MMIO 和 Reserved Regions。

我们的设计目标是：

- Linux 可以访问 Linux RAM 和 Shared Memory。
- OP-TEE 可以访问 OP-TEE RAM 和经过授权的 Shared Memory。
- Linux 无法直接访问 OP-TEE RAM。
- Shared Memory 被明确视为不可信数据。
- M-mode Firmware 负责实施这些权限。

接下来分析一次实际内存访问。

### 1.6.2 攻击：Linux Kernel 修改 `satp` 和 PTE，尝试读取 OP-TEE RAM

攻击者已经取得 Linux Kernel S-mode 执行权限。

他可以构造这样的页表：

```
Linux VA:
    0xFFFFFFC010000000
            |
            | PTE:
            |   PPN -> 0x90000
            |   R=1, W=1, X=0
            v
Physical Address:
    0x90000000

Target:
    OP-TEE Private RAM
```

这里 PPN 为示意，假设使用 4 KiB 页。

从 Linux MMU 的角度看，这个映射完全可能被建立。

它甚至具有合法的 R/W PTE Permission。

但 CPU 最终访问内存，并不只经过 MMU。

### 1.6.3 关键机制：PMP 对物理内存访问进行独立检查

简化后的 CPU 内存访问流程：

```
          Linux S-mode
                |
                | load VA
                v
          +-----------+
          |  Sv39 MMU |
          | VA -> PA  |
          +-----------+
                |
                | PA = 0x90000000
                v
          +-----------+
          | PMP Check |
          | S-mode?   |
          | PA? R/W/X?|
          +-----------+
                |
          +-----+-----+
          |           |
       Allowed      Denied
          |           |
          v           v
       Memory      Trap
```

PMP = Physical Memory Protection。

它最大的特点是：

PMP 检查的是 CPU 实际要访问的物理地址，而不是 Linux 页表中声明的访问意图。

因此，即使 Linux 自己把一个虚拟地址映射到 OP-TEE RAM，只要当前 hart 的 PMP 不允许 S-mode 访问那个物理范围，CPU 就必须拒绝这种访问。

RISC-V Privileged Specification 明确规定，PMP 检查适用于 S/U-mode 的指令取指、数据访问，以及地址翻译过程中隐式的页表访问。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



这是 RISC-V TEE 隔离成立的关键硬件基础。

### 1.6.4 PMP 权限由谁配置？

重点来了。

PMP 使用的主要 CSR 包括：

```
pmpcfg0
pmpcfg2
pmpaddr0
pmpaddr1
...
```

对于 RV64，`pmpcfg` 寄存器的有效编号方式与 RV32 不完全相同；这里不展开寄存器编码细节。

这些 PMP CSR 由 M-mode 管理。

Linux 运行在 S-mode，没有权限直接修改它们。

例如，Linux 即使执行：

```
li   t0, 0
csrw pmpcfg0, t0
```

也不能成功关闭 PMP 保护，通常会因执行不允许的 CSR 访问而产生 Illegal Instruction Exception。

因此，存在一个重要的不对称关系：

```
Linux:
    Can modify its own Page Tables
    Cannot directly modify PMP

OpenSBI:
    Can configure PMP
    Must enforce Domain Security Policy
```

这意味着 Linux 不再是物理内存权限的最高管理者。

真正决定 Linux 能访问哪些物理地址的是可信 M-mode Firmware 配置的硬件保护策略。

### 1.6.5 Linux Domain 和 OP-TEE Domain 的 PMP Policy 可以不同

假设 Domain A 是 Linux，Domain B 是 OP-TEE。

对这两个 Domain，我们希望实现以下权限：

| Physical Region   | Linux Domain | OP-TEE Domain |
| ----------------- | ------------ | ------------- |
| Linux RAM         | R/W/X，按策略    | 默认不授予，必要时受控共享 |
| OP-TEE RAM        | Deny         | R/W/X，按内部区域细分 |
| Shared Memory     | R/W          | R/W           |
| OpenSBI RAM       | Deny         | Deny          |
| Secure Peripheral | Deny         | 按业务授权         |

这里的 R/W/X 是物理访问的授权上限；实际执行权限通常还会由 MMU 页表进一步约束。OP-TEE Core 的真实代码、只读数据和可写数据也不应该全部无差别授权为 RWX。

当 Hart 0 运行 Linux 时：

```
Hart 0 PMP:

Linux RAM    -> Allow
OP-TEE RAM   -> Deny
Shared RAM   -> Allow
```

当 Hart 0 切换到 OP-TEE 时：

```
Hart 0 PMP:

Linux RAM    -> Restricted / as needed
OP-TEE RAM   -> Allow
Shared RAM   -> Allow
```

因此，同一个 hart 在不同 Domain Context 下，具有不同的物理访问权限。

这就是软件配置的 Domain Policy 转化为硬件隔离效果的过程。

### 1.6.6 PMP 不是一个全系统共享的全局寄存器组

这点值得特别注意。

PMP Configuration 是 Per-Hart 的。

假设 Hart 0 运行 Linux，Hart 1 正在执行 OP-TEE：

```
Hart 0                      Hart 1
Linux S-mode                OP-TEE S-mode
    |                            |
    v                            v
PMP Config A               PMP Config B
    |                            |
    v                            v
No Access                  Access
to TEE RAM                 to TEE RAM
```

这两个 hart 可以同时执行不同的 Domain。

因此，不能简单理解成：

> 只要某个 hart 进入 OP-TEE，整个 SoC 的 PMP 就切换到 TEE Mode。

更准确地说：

每个 hart 的 PMP 决定这个 hart 当前 CPU 内存访问的权限。

这就引出了后面 SMP 的核心挑战：

- Hart 0 切换 Domain 时，Hart 1 是否需要同步？
- 某个物理内存 Region 的 Ownership 改变时，其他 hart 的 PMP 是否仍保留旧权限？
- Per-Hart PMP 与 System-Level Firewall 如何保持一致？
- Shared Memory 的授权和回收何时生效？

这些问题不会仅仅因为配置了 PMP 就自动消失。

### 1.6.7 为什么 PMP 切换需要同步地址翻译状态？

现在继续深入一点。

假设一个 hart 已经缓存了某个 VA→PA 翻译，随后 M-mode 修改 PMP。

RISC-V 架构允许实现对地址翻译结果以及与 PMP 有关的状态进行缓存。

因此，修改 PMP 后，必须按照 ISA 要求同步这些状态。

在启用分页的情况下，规范要求在修改 PMP CSR 后执行适当的 `SFENCE.VMA` 同步；对于 H Extension，还要考虑相应的虚拟化地址翻译同步要求。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



在 OpenSBI 的 Domain Context Switch 中，这种同步不是可选的性能优化，而是安全正确性要求的一部分。

例如在实际源码中，`switch_to_next_domain_context()` 会调用：

```
sbi_hart_protection_reconfigure(
    scratch,
    current_dom,
    target_dom
);
```

其周围的源码注释说明，该路径还承担与 PMP 更新相关的地址翻译同步。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里不需要你现在记住所有实现细节，但必须理解：

Security Domain 切换涉及 CPU Translation/Protection State 的重新生效，而不仅是修改几个 CSR。

### 1.6.8 TEE Domain 内部为什么仍然需要 MMU？

前面已经说过，OP-TEE 可以通过 PMP 获得对 TEE RAM 的访问权限。

但是，这意味着 OP-TEE Domain 中的所有代码都可以自由访问 TEE RAM 吗？

不是。

我们继续将执行环境分成：

```
OP-TEE Domain
    |
    +-- OP-TEE Core (S-mode)
    |
    +-- TA A (U-mode)
    |
    +-- TA B (U-mode)
```

当 TA A 运行时，它需要被限制在自己获准访问的 User Address Space 中。

这时，OP-TEE 使用 MMU 来完成细粒度权限管理。

例如：

```
TA A:
    VA_A -> PA_A

TA B:
    VA_B -> PA_B

OP-TEE Core:
    VA_Core -> PA_Core
```

即使这些 PA 都位于 PMP 允许当前 OP-TEE Domain 访问的物理范围内，U-mode TA 仍受到页表 U/R/W/X Permission 的约束。

所以我们实际上形成了两层保护：

第一层：PMP / Domain Isolation

保护整个 OP-TEE Domain，抵抗被攻破的 Linux Kernel。

第二层：MMU / U-S Isolation

保护 OP-TEE 内部不同的 TA，以及 TA 与 OP-TEE Core 的隔离。

注意，OP-TEE Core 作为 S-mode 内核，本身具有管理 TA 页表的权限，所以 TA 对 Core 的信任关系与 Linux 对 TEE 的关系并不对称。

### 1.6.9 真实攻击推演：Linux 修改 `satp` 后会发生什么？

我们现在把整个过程走一遍。

攻击者首先控制 Linux Kernel，并希望读取：

```
TEE Private Key:
    PA = 0x90001000
```

攻击步骤：

1. Linux 修改 PTE，让某个 Linux VA 指向 PA `0x90001000`。
2. Linux 执行必要的地址翻译同步。
3. Linux 执行 `ld`，读取该 VA。
4. MMU 根据 PTE 翻译到 PA `0x90001000`。
5. CPU 对最终物理访问执行 PMP 检查。
6. 当前 Hart 属于 Linux Domain，PMP 不允许访问该 PA。
7. CPU 拒绝访问，产生访问错误异常。

这里预期是 Access Fault，而不是单纯的 Page Fault，因为假设页表权限和翻译本身是合法的，失败来自 PMP 物理访问控制。

从这个过程可以得到一个非常关键的架构结论：

> Linux 即使控制了自己的全部 VA→PA 映射，也无法绕过由 M-mode 配置且不受 Linux 控制的物理内存访问限制。

这就是 RISC-V 在相同 S-mode 下建立可信隔离的根本原理。

但这里我们只证明了这条 CPU Load/Store 访问路径可以被隔离；还没有证明 DMA、设备、调试接口、缓存侧信道等全部攻击路径都已经受控。

这些问题属于下一节。

## 1.7 PMP、MMU、SoC Firewall、DMA Protection 究竟如何协作？

我认为这是从 Firmware Engineer 走向 SoC Security Architect 必须掌握的内容。

前面我们主要考虑了 CPU 访问内存的情形。

但是，一个真正的 SoC 不只有 CPU。

### 1.7.1 CPU PMP 保护不了所有总线 Master

假设 SoC 内部有：

```
CPU Cluster
    |
    +-- Hart 0
    +-- Hart 1
    +-- Hart 2
    |
    +-- DMA Controller
    +-- PCIe Device
    +-- USB Controller
    +-- Ethernet Controller
    +-- Hardware Crypto Engine
    |
    v
SoC Interconnect
    |
    v
DDR Controller
```

Linux 虽然不能直接访问 OP-TEE RAM，但它可能控制一个 DMA Controller。

攻击者就可以尝试：

```
Linux Driver
    |
    | Program DMA:
    |
    | Source = 0x90000000
    | Target = 0x80000000
    v
DMA Controller
    |
    | Read OP-TEE RAM
    | Write Linux RAM
    v
Data Exfiltration
```

这里最重要的问题是：

DMA Controller 发出的内存访问，并不是 Hart 0 的 S-mode Load/Store 指令。

因此，不能默认 CPU 的 PMP 会检查这类 DMA 访问。

如果 SoC 没有其他硬件保护，那么即使 Linux 不能通过 CPU 直接读 TEE RAM，仍可能通过 DMA 间接读取。

这会彻底破坏 TEE 的 Confidentiality。

### 1.7.2 为什么需要系统级 Hardware Isolation？

为了解决这个问题，我们需要在 CPU 之外建立另一层可信访问控制：

```
           CPU Cluster
                |
                | CPU Load / Store
                v
            CPU PMP
                |
                v
            SoC Fabric <---------- DMA / PCIe / USB
                |                       |
                |                       v
                |                 Device Access
                v
        System-Level Security
          Firewall / IOPMP
                |
                v
            DDR Memory
                |
          +-----+-----+
          |           |
      Linux RAM   OP-TEE RAM
```

这是一张逻辑图。具体 SoC 中，PMP 通常在 Hart/CPU 访问路径内实施，系统级隔离单元可能位于多个不同的总线入口或目标访问路径，而不是只有一个统一的 Firewall。

核心目标是：

无论访问由哪个硬件 Master 发起，都必须根据其可信身份和目标资源权限决定是否允许访问。

对于 RISC-V 平台，可能涉及：

- IOPMP。
- RISC-V IOMMU。
- WorldGuard。
- SoC Vendor-specific Memory Firewall。
- Interconnect Access Controller。
- Peripheral Security Attribution。

这些机制并非完全可互换。

### 1.7.3 PMP、IOMMU、IOPMP 的职责区别

| Mechanism    | 控制对象               | 主要用途                |
| ------------ | ------------------ | ------------------- |
| PMP          | Hart 的物理内存访问       | 约束 S/U-mode CPU 访问  |
| MMU          | VA→PA 翻译及页表权限      | 进程、TA、Kernel 地址空间隔离 |
| IOMMU        | 设备 DMA 地址转换与权限     | 限制设备可访问的内存范围        |
| IOPMP        | I/O Master 的物理访问权限 | 控制 DMA、外设等发起的物理访问   |
| SoC Firewall | 平台互连或设备访问          | 实施平台级安全资源策略         |

其中 IOMMU 需要特别小心。

Linux 可以管理一个普通的 IOMMU，并不意味着该 IOMMU 就能够为 TEE 提供可信的 DMA 隔离。

如果 Linux 能任意更改保护 TEE 所依赖的 IOMMU Page Table，攻击者仍然可以把设备 DMA 映射到 TEE RAM。

因此，真正有意义的不是设备是否存在 IOMMU，而是：

谁控制 DMA 的安全策略？不可信 Linux 能不能修改这项策略？

TEE 所依赖的 DMA 防护策略，必须由受信任的硬件或固件强制实施。

### 1.7.4 WorldGuard 的设计思想

WorldGuard 是一个值得研究的 RISC-V 平台安全方案。

其核心理念是利用硬件身份标签来表达某个 Master 或执行环境属于哪个安全 World/Domain，并让相应 Checker 决定访问是否被授权。

简化模型：

```
Hart / Bus Master
    |
    | World ID
    v
Access Request
    |
    v
WorldGuard Checker
    |
    | Compare:
    |   World ID
    |   Target Resource Policy
    v
Allow / Deny
```

从架构思想上看，它与 TrustZone 对总线访问使用安全属性存在相似之处。

但是二者不能直接画等号：

- Arm TrustZone 定义了对应架构中的 Security State 语义。
- WorldGuard 属于具体平台可实现的 World ID 和访问控制机制。
- 它依赖相应的硬件、固件集成以及策略配置，并不是所有 RISC-V SoC 都强制具备的统一 TrustZone 实现。

为什么 OpenSBI 需要支持 WorldGuard？

因为安全 Domain 的切换可能不仅意味着更换 PMP，还意味着更换当前 Hart 的硬件 World Identity。

RISE RP016 Milestone 5 就围绕 OpenSBI Domain Context Switch 时的 WorldGuard 状态切换开展开发，包含 Device Tree Policy、Checker 配置以及 World ID 相关状态管理。该功能在项目公开状态中仍处于 upstream review 阶段。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### 1.7.5 为什么需要 Hardware Isolation Framework？

我们现在考虑一个工程问题。

假设某款 SoC 使用 IOPMP，另一款使用 WorldGuard，第三款使用厂商自研 Firewall。

难道每个平台都要重写 OpenSBI Domain Context Switch 吗？

更合理的设计是让 Domain Management 与 Hardware Isolation Mechanism 解耦：

```
       OpenSBI Domain Manager
                 |
                 v
       Hardware Isolation API
                 |
       +---------+---------+
       |         |         |
       v         v         v
      PMP      WorldGuard  SoC FW
                 |
                 v
          Resource Protection
```

这种设计可以把不同平台的硬件隔离操作挂接到统一的 Domain Lifecycle。

例如：

```
/* RP016 M4 设计中的接口形式，节选 */

struct sbi_hwiso_ops {
    const char *name;

    int (*init)(void *fdt);

    int (*domain_init)(void *fdt,
                       int domain_offset,
                       struct sbi_domain *dom,
                       void **ctx);

    void (*domain_exit)(const struct sbi_domain *src,
                        const struct sbi_domain *dst,
                        void *ctx);

    void (*domain_enter)(const struct sbi_domain *dst,
                         const struct sbi_domain *src,
                         void *ctx);

    void (*domain_cleanup)(struct sbi_domain *dom,
                           void *ctx);
};
```

这组接口来自 RP016 Milestone 4 的公开设计，并非声称已经全部合入你本地使用的 OpenSBI 版本。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



这里的设计理念很值得你深入体会：

Domain Manager 不应该了解每一个厂商的 Firewall Register Layout，它只需要在正确的生命周期调用硬件隔离机制。

例如，当 Linux Domain 切入 OP-TEE Domain 时：

```
OpenSBI Domain Switch
        |
        +-- Save Linux Context
        |
        +-- Exit Linux Domain
        |      |
        |      +-- HW Isolation Exit
        |
        +-- Reconfigure CPU PMP
        |
        +-- Enter OP-TEE Domain
        |      |
        |      +-- HW Isolation Enter
        |
        +-- Restore OP-TEE Context
        |
        +-- Resume OP-TEE
```

这是用于解释模块职责的逻辑流程，不是对当前源码指令顺序的逐行复刻。实际实现必须确保在任何过渡窗口中，不可信执行体都不能利用旧权限，且失败时不会留下越权状态。

从 Upstream Engineering 角度看，这正是非常有价值的设计领域：统一机制、平台扩展、错误恢复、可验证性和安全不变量。

### 1.7.6 除了内存访问，还需要保护什么？

我们再把视野扩大。

一个完整 TEE 的隔离对象不仅是内存。

| 资源              | 可能的攻击方式            | 需要的保护                           |
| --------------- | ------------------ | ------------------------------- |
| Secure RAM      | CPU 或 DMA 读取       | PMP + System Firewall           |
| Secure MMIO     | Linux 访问安全寄存器      | MMIO Permission / Firewall      |
| Crypto Engine   | Linux 窃取结果或破坏状态    | 安全 Device Ownership             |
| Interrupt       | 伪造、中断劫持、错误路由       | Domain-aware Interrupt Routing  |
| CPU Register    | Domain Switch 遗留秘密 | Context Save/Restore/Scrub      |
| Cache / TLB     | 权限残留、信息泄露          | Synchronization / Hardening     |
| Debug Interface | 外部调试器读取受保护状态       | Lifecycle / Debug Authorization |
| Secure Storage  | 篡改或回放旧数据           | Integrity + Anti-Rollback       |
| Boot Firmware   | 恶意替换安全代码           | Verified Boot / Root of Trust   |

其中还有两个经常被忽略的问题。

第一：CPU Context State 本身也可能泄露秘密。

假设 OP-TEE 使用 Vector Register 暂存敏感密钥材料。

如果切换回 Linux 时没有正确恢复或清理相应的状态，那么 Linux 可能通过后续 Vector 指令观察到不该看到的数据。

这不是 PMP 可以解决的问题。

第二：Domain Integrity 不等于 Secure Boot Integrity。

即使运行期间的物理隔离是正确的，如果启动时加载的 OP-TEE Image 已经被篡改，那么系统一开始就不可信。

所以安全启动链还需要确保：

```
Trusted Root
    |
    v
Authenticate M-mode Firmware
    |
    v
Authenticate OP-TEE Image
    |
    v
Establish Isolation Policy
    |
    v
Transfer Execution
```

以上是信任依赖关系，不意味着每个产品都采用完全相同的逐镜像验证顺序；真实流程也可能涉及独立的 HSM、安全启动管理固件、Manifest 或度量机制。

Section 1.7 的核心结论：

PMP 是 RISC-V TEE CPU 内存隔离的重要机制，但它不是整个 SoC 的安全边界。真正可靠的 TEE 必须同时考虑 CPU、DMA、总线 Master、MMIO、Interrupt、Context State、Debug 和启动信任链。

## 1.8 Arm TrustZone vs RISC-V TEE — 两种架构的本质区别

经过前面七节的分析，我们现在可以真正从架构设计的角度比较 Arm 和 RISC-V。

### 1.8.1 第一层区别：安全隔离的定义方式

Arm TrustZone 的典型设计是：

架构定义 Security State，硬件根据 Security State 实施对应的访问规则。

RISC-V OpenSBI Domain 的典型设计是：

M-mode Trusted Firmware 定义 Domain 资源策略，再利用 PMP 和平台硬件机制实施隔离。

对比来看：

```
Arm TrustZone
-------------------------

Security State
    |
    +-- Secure
    |
    +-- Non-secure
    |
    v
Architectural Security
Attributes
    |
    v
SoC Resource Protection
```

```
RISC-V OpenSBI TEE
-------------------------

M-mode Trusted Firmware
    |
    v
Domain Policy
    |
    +-- Linux Domain
    |
    +-- OP-TEE Domain
    |
    v
PMP / HW Isolation Policy
    |
    v
SoC Resource Protection
```

这里的区别并不是 Arm 使用硬件而 RISC-V 使用软件。

两种方案最终都必须由硬件强制实施隔离。区别在于安全域的架构表达方式及其管理模型。

### 1.8.2 第二层区别：World Switch 与 Domain Switch

Arm World Switch 的核心是 Secure/Non-secure Security State 之间的转换。

RISC-V Domain Switch 的核心是不同受保护执行上下文之间的转换。

具体对比如下：

| 比较维度           | Arm TrustZone OP-TEE                | RISC-V OpenSBI OP-TEE           |
| -------------- | ----------------------------------- | ------------------------------- |
| Kernel 运行位置    | 通常 S-EL1                            | S-mode                          |
| Linux 运行位置     | NS-EL1                              | S-mode                          |
| 可信 Monitor     | EL3 / TF-A                          | M-mode / OpenSBI                |
| 安全域属性          | 架构定义的 Security State                | Firmware 管理的 Domain             |
| 进入 Monitor     | `SMC` 等异常路径                         | `ECALL` / SBI                   |
| 返回机制           | `ERET`                              | `MRET`                          |
| 内存保护           | Security Attribute + SoC Controller | PMP + 系统级保护机制                   |
| Domain Context | Secure/Non-secure Context           | Per-Hart Domain Context         |
| 安全中断           | GIC Security Group 等机制              | 依赖 Interrupt Routing 和隔离实现      |
| DMA Isolation  | TrustZone-aware SoC Protection      | IOPMP/WorldGuard/SoC Firewall 等 |
| User TA 隔离     | S-EL0 + MMU                         | U-mode + MMU                    |

这里的两个重要区别是：

第一，RISC-V `ECALL` 本身不携带 Secure/Non-secure State 的切换语义；进入 TEE 需要由受信任的 M-mode Firmware 处理相应请求并进行 Domain 切换。

第二，OpenSBI Domain 切换不仅是恢复 CPU 寄存器，还需要确保切换后的硬件访问权限与目标 Domain 一致。

当前 RISC-V Linux OP-TEE 的相关 RFC 已采用 RPMI TEE Service over SBI MPXY 的通信设计，其目标是为这条调用链建立更规范的传输接口；该 RFC 在 2026 年 9 月仍依赖开发中的 RPMI v2.0 TEE Service Group，因此不能把这一传输协议的全部细节当作已冻结标准。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

Openwall Lists



### 1.8.3 第三层区别：TCB 有何不同？

传统 Arm TrustZone OP-TEE 的关键 TCB 通常涉及：

```
Arm Platform
    |
    +-- Root of Trust / Boot Firmware
    +-- EL3 Trusted Firmware
    +-- OP-TEE Core
    +-- Trusted TA
    +-- Secure Memory / Peripheral Hardware
```

采用 OpenSBI 的 RISC-V OP-TEE 则可能是：

```
RISC-V Platform
    |
    +-- Root of Trust / Boot Firmware
    +-- OpenSBI M-mode Trusted Firmware
    |      |
    |      +-- Domain Management
    |      +-- Context Switching
    |      +-- PMP / HW Isolation
    |      +-- TEE Communication
    |
    +-- OP-TEE Core
    +-- Trusted TA
    +-- System-Level Security Hardware
```

虽然从抽象上看 EL3 与 M-mode Firmware 承担了相似的可信管理职责，但二者仍有架构差异。

例如，Arm 的安全状态以及相关寄存器、互连属性有明确的架构语义。

RISC-V 的 OpenSBI Domain 是由 Firmware 在 Privileged Architecture 之上构建的系统级抽象。

所以，RISC-V 的优点是实现方案可以灵活组合；代价是需要对每一种 SoC 的具体安全机制做更完整的审查。

特别是在没有足够的系统级硬件隔离时，仅凭 OpenSBI 和 PMP 无法证明所有 DMA Master 都无法访问 TEE 内存。

### 1.8.4 第四层区别：RISC-V 的 M-mode 拥有什么信任地位？

在这一模型中，OpenSBI 是非常关键的 TCB 组成部分。

如果 OpenSBI 被完全攻破，那么攻击者可能获得 M-mode 的控制权，改变 Domain 隔离规则。

传统 PMP 默认并不把 M-mode 作为不可信执行环境。对于某些受锁定规则保护的区域，M-mode 也可能受 PMP 约束；Smepmp（ePMP）进一步增强了 M-mode 的访问约束能力。

例如，Smepmp 引入 `mseccfg` 中的 `MML`、`MMWP`、`RLB` 等配置，以支持更严格的 Machine-mode Memory Protection 规则。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



但是，要注意：

Smepmp 并不等于能够在攻击者完全控制 M-mode 的情况下，自动提供一个独立于 M-mode 的 TEE。

它可以用于缩小 M-mode 软件的攻击面，实施更严格的代码与数据权限；具体保证取决于锁定机制、配置时机以及实际威胁模型。

类似地，Arm 的 EL3 被攻破通常也会破坏传统 TrustZone 系统的主要安全保证。

因此，评价 TEE 不能只关注 Linux 能不能访问 OP-TEE，还要关注谁保护最底层的 Security Monitor。

### 1.8.5 第五层区别：哪一种架构更安全？

不能简单说 Arm TrustZone 更安全，也不能说 RISC-V Domain 架构一定更灵活所以更安全。

从架构评价的角度，应该分别分析：

| 评价因素                 | 需要检查的问题                        |
| -------------------- | ------------------------------ |
| Hardware Isolation   | 是否能阻止所有不可信 Master 访问安全资源？      |
| Privilege Separation | 是否能阻止低特权代码改变隔离策略？              |
| Context Isolation    | 寄存器、CSR、FP/Vector 状态是否正确隔离？    |
| Secure Boot          | Firmware、TEE Image 和策略是否可信？    |
| Attack Surface       | 安全入口、消息处理、Driver、SBI 接口是否足够受控？ |
| TCB Size             | 哪些组件必须可信？代码规模与复杂度如何？           |
| SMP Correctness      | 不同 hart 的权限和资源所有权能否保持一致？       |
| Security Validation  | 能否通过测试、审计、形式化推理等方式验证关键安全不变量？   |

安全性不是架构名称的属性，而是具体实现能否在既定 Threat Model 下维持安全不变量的结果。

这也是未来你参与 OP-TEE Maintainer 工作时应该采用的分析方式。

# 九、综合案例：设计一个可以抵抗 Linux Root 攻击的 RISC-V Payment TEE

前面内容比较广，我们现在通过一个完整案例，将 Chapter 01 的所有设计理念串联起来。

## 9.1 系统需求

假设需要实现一个 Payment TA。

目标：

- Linux 运行支付 CA。
- OP-TEE 运行 Payment TA。
- Payment TA 使用设备私钥签名支付请求。
- Linux Kernel 即使被攻破，也不能直接读取私钥。
- 不可信 DMA 不能访问 Payment TA 的私有内存。
- Linux 只能通过定义好的服务接口请求签名。

假设硬件具备能够实施上述隔离的 PMP 和系统级访问控制能力。

## 9.2 系统架构

```
               RISC-V SoC
+--------------------------------------+
|                                      |
|   REE Domain                         |
|   +------------------------------+   |
|   | Payment CA                   |   |
|   | Linux Kernel                 |   |
|   +------------------------------+   |
|                   |                  |
|                   | ECALL / TEE      |
|                   | Request          |
|                   v                  |
|   +------------------------------+   |
|   | M-mode Trusted Firmware      |   |
|   | OpenSBI                      |   |
|   |                              |   |
|   | Domain Policy                |   |
|   | PMP / HW Isolation           |   |
|   | Context Switch               |   |
|   +------------------------------+   |
|                   |                  |
|                   v                  |
|   TEE Domain                         |
|   +------------------------------+   |
|   | OP-TEE Core                  |   |
|   | Payment TA                   |   |
|   | Private Key                  |   |
|   +------------------------------+   |
|                                      |
+--------------------------------------+
```

系统需要同时满足两类权限限制。

第一类是 REE/TEE 之间的物理内存隔离。

第二类是 OP-TEE 内部 TA 与 Core 之间的权限隔离。

## 9.3 正常签名流程

```
Payment CA
    |
    | Sign(transaction)
    v
Linux TEE Driver
    |
    v
OpenSBI M-mode
    |
    | Validate Request
    | Switch Domain
    v
OP-TEE S-mode
    |
    | Locate Payment TA
    | Validate Parameters
    | Check Authorization
    v
Payment TA U-mode
    |
    | Use Private Key
    | Generate Signature
    v
OP-TEE Core
    |
    v
OpenSBI M-mode
    |
    | Switch Domain
    v
Linux
    |
    v
Signature
```

假设私钥只存储在 OP-TEE Domain 获准访问的安全内存中，或者存储在具有可信访问控制的安全硬件里。

Linux 只能获取授权签名操作的结果。

## 9.4 攻击者取得 Linux Root 后，可以做什么？

我们将攻击行为分成四类。

| 攻击                  | 是否应被阻止  | 由谁负责                            |
| ------------------- | ------- | ------------------------------- |
| 直接读取 TEE RAM        | 是       | PMP / Hardware Isolation        |
| 通过 DMA 读取私钥         | 是       | IOPMP / Firewall 等              |
| 修改 OP-TEE Core Code | 是       | Memory Protection / Secure Boot |
| 通过 CA 请求非法签名        | 取决于授权机制 | Payment TA / Backend            |

最后一种情况很重要。

即使前三种保护全部正确，也不能得出「攻击者无法滥用 Payment TA」的结论。

因为被攻破的 Linux 仍然可以调用公开的 TEE Service Interface。

如果 Payment TA 仅凭 Linux 提供的 UID 判断调用者是否合法，攻击者可能伪造相应身份信息。

因此，Payment TA 需要与 REE 不可伪造的授权凭据或可信后端验证机制相结合，并明确防止 Replay、Privilege Confusion 和 Confused Deputy。

Memory Isolation 解决的是攻击者不能直接拿到私钥，而不是攻击者一定无法诱使 TA 使用私钥。

这是系统安全架构中非常重要的一条分界线。

## 9.5 这个案例对实际 OP-TEE Porting 的意义

当你移植 RISC-V OP-TEE 时，第一步不应该是直接开始修改汇编入口或 Device Tree。

应该先把以下问题回答清楚：

- TEE RAM 由谁预留、谁验证、谁配置访问权限？
- Linux 是否存在通过其他路径访问该 RAM 的可能？
- OpenSBI 能否安全地管理 Domain Context 和隔离策略？
- Secure Monitor 的入口是否受到严格约束？
- OP-TEE Image 在执行前是否经过可信认证？
- 当前 SoC 的 DMA Master 是否能够绕过 CPU PMP？
- Shared Memory 是否会被错误地作为可信输入使用？

这些问题得到明确答案之后，后续 Boot、MMU、Trap、SBI Communication 的设计才有可信的基础。

# 十、Chapter 01 的 Staff Engineer 级测试题

为了验证是否真正掌握了本章，而不是只记住了一些概念，我设计了十个问题。

建议先自己推导答案，再展开参考要点。

思考题与参考答案

展开全部答案

1为什么 Linux 和 OP-TEE 都运行在 S-mode，却仍然可能实现可信隔离？

2Linux 已取得 Root，攻击者把 VA 映射到 TEE RAM，为什么仍然可能无法读取？

3为什么 PMP 对 CPU 有效，仍然不足以保证完整的 TEE 安全？

4OpenSBI Domain 与 Linux Process 的根本区别是什么？

5Domain Switch 为什么需要同时切换 CSR 和 PMP？

6Hart 0 进入 OP-TEE 后，Hart 1 的 PMP 是否自动改变？

7RISC-V H Extension 的 Stage-2 Translation 为什么不能自动提供抗 Hypervisor 攻击的 TEE？

8为什么 OP-TEE Domain 内还需要 MMU，既然 PMP 已经允许访问 TEE RAM？

9如果 M-mode OpenSBI 被攻破，PMP 能保证 OP-TEE 仍然安全吗？

10为什么安全的 Payment TA 不应仅依靠 Linux 提供的 UID 授权签名操作？

这十道问题中，我尤其建议你重点掌握第 2、3、5、6、9 题，因为它们对应了未来 RISC-V OP-TEE 移植中的实际系统安全问题。

# 十一、本章源码阅读任务

虽然 Chapter 01 主要讲架构原理，但我仍然建议用一组实际源码文件验证设计理念。

| 阅读顺序 | 文件                                                                                                                                      | 主要目的                     |
| ---- | --------------------------------------------------------------------------------------------------------------------------------------- | ------------------------ |
| 1    | [domain_support.md](https://github.com/riscv-software-src/opensbi/blob/master/docs/domain_support.md?utm_source=chatgpt.com)            | 理解 Domain 的正式定义与资源模型     |
| 2    | [sbi_domain.h](https://github.com/riscv-software-src/opensbi/blob/master/include/sbi/sbi_domain.h?utm_source=chatgpt.com)               | 理解 Domain 数据结构           |
| 3    | [sbi_domain.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain.c?utm_source=chatgpt.com)                   | 理解 Domain 注册、资源和权限管理     |
| 4    | [sbi_hart_protection.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart_protection.c?utm_source=chatgpt.com) | 理解 PMP 和底层保护配置           |
| 5    | [sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)   | 理解不同 Domain 的 Context 切换 |

在第一次阅读时，不必把所有函数都研究透彻。

建议带着三个问题阅读：

第一个问题：一个 Domain 的安全资源边界是如何描述的？

重点关注 `struct sbi_domain`、`struct sbi_domain_memregion`。

第二个问题：描述出来的 Resource Policy 是如何转化成硬件权限的？

重点关注 PMP 配置和 `sbi_hart_protection_reconfigure()`。

第三个问题：一个 Hart 从 Domain A 切换到 Domain B 时，哪些状态会发生变化？

重点关注 `switch_to_next_domain_context()`。

这三个问题能够把「Domain 是什么」「Domain 为什么安全」「Domain 怎样执行」完整串联起来。

# 十二、Chapter 01 总结：建立一个能够指导后续学习的认知模型

我们最后把所有内容抽象成一个统一模型。

TEE Security Architecture

安全目标：在 REE 被攻破后保护 TEE 资产

Security Policy

Domain / Resource / Ownership

Trusted Execution

OP-TEE / TA / Secure Service

Trusted Security Monitor — M-mode

OpenSBI · Domain Management · Context Switch

Hardware-Enforced Isolation

PMP · MMU · IOPMP · WorldGuard · SoC Firewall

Trusted Boot / Root of Trust

确保初始软件、硬件配置和安全策略可信

我希望你从 Chapter 01 真正掌握的，不是一个具体的 RISC-V TEE 实现方案，而是以下四条可以反复使用的架构原则：

原则一：Privilege Separation 不等于 Security Domain Isolation。

Linux 和 OP-TEE 可以处于相同的 S-mode，只要存在不受 Linux 控制、由硬件实施的隔离边界。

原则二：安全资源的管理权必须位于攻击者的信任边界之外。

如果 Linux 可以修改保护 OP-TEE RAM 的全部权限配置，那么这道隔离边界就失去了独立性。

原则三：TEE 的安全边界必须覆盖整个 SoC，而不是只覆盖 CPU Load/Store。

DMA、MMIO、Interrupt、Debug、共享状态和启动流程，都可能成为攻击路径。

原则四：Security Domain 的设计必须同时考虑 Policy、Enforcement、Execution Context 和 Lifecycle。

只定义了内存区域，并不意味着已经建立安全隔离。必须确保权限在启动、切换、多核并发和错误恢复期间始终成立。

## 下一章衔接

Chapter 01 主要解决 Why 和 What：RISC-V TEE 为什么能成立，以及需要哪些可信机制。

接下来的 Chapter 02 — RISC-V OP-TEE System Boot Architecture 将把这些抽象概念落实到完整启动链：

```
SoC Reset
    |
    v
Boot ROM / Root of Trust
    |
    v
OpenSBI M-mode Boot
    |
    v
Initialize PMP / Domains
    |
    +----> OP-TEE Domain Boot
    |
    +----> Linux Domain Boot
                  |
                  v
              TEE Request
                  |
                  v
            Domain Switching
```

我们将特别深入到 OpenSBI 的真实启动汇编、Coldboot/Warmboot、Domain 初始化、PMP 建立、Secondary Hart 初始化和 OP-TEE Boot Entry，并解释一个核心问题：

系统上电时所有 Hart 并没有天然被划分为 Linux Domain 和 OP-TEE Domain，那么 OpenSBI 究竟是在什么时候、依据什么配置、通过哪些代码和寄存器操作，建立起这两个具有安全隔离能力的执行环境的？

理解这条启动过程，就能把 Chapter 01 的安全架构原则与实际的 RISC-V Firmware 实现真正连接起来。