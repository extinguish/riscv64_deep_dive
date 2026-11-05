# Chapter 21 — End-to-End RISC-V OP-TEE Porting Project

RISC-V OP-TEE Architecture & Porting · 系统级毕业设计 · Staff Engineer / SoC Security Architect

本章将把前面 20 章学习的知识整合到一个完整的工业级 SoC 项目中。

我们的目标不再是单独解释 OpenSBI Domain、PMP、OP-TEE MMU、RPMI 或 Trusted Thread，而是回答：

假设现在有一颗全新的 RISC-V 服务器 SoC，需要将 OP-TEE 移植上去，从芯片架构评估、Firmware Bring-up，到 Linux + OP-TEE 完整运行、安全验证和最终交付，应该如何设计和实现整个项目？

本章不仅讲技术实现，还会覆盖架构设计决策、软件模块划分、Bring-up 顺序、工程验收、实际故障定位与 Staff Engineer 级别的设计权衡。

## 本章目录

| Section | 主题                                | 核心交付物                      |
| ------- | --------------------------------- | -------------------------- |
| 21.1    | 目标 SoC 硬件与安全能力分析                  | Hardware Capability Matrix |
| 21.2    | Security Threat Model 与 TCB       | 安全边界及威胁模型                  |
| 21.3    | Boot Chain 与 Firmware Integration | 可验证的启动信任链                  |
| 21.4    | OpenSBI Domain 与硬件隔离              | Domain Policy + 隔离测试       |
| 21.5    | OP-TEE Core Boot 与 MMU            | OP-TEE 初始化及异常调试            |
| 21.6    | RPMI / MPXY / TEE Driver          | Linux ↔ OP-TEE 通信          |
| 21.7    | Shared Memory、RPC、User TA         | CA → TA 完整执行               |
| 21.8    | SMP、Interrupt、Hart Lifecycle      | 多核、异步中断及电源管理               |
| 21.9    | Secure Storage / HSM              | 密钥、存储和硬件安全服务               |
| 21.10   | 系统安全测试、性能与交付                      | 可复现的产品级 Port               |
| 21.11   | Staff Engineer 面试与设计题             | 系统设计、故障分析及参考答案             |

## 阅读前需要明确的技术基线

截至 2026 年 11 月 4 日，RISC-V OP-TEE 已具备 upstream RV64 基础支持，但其完整生态仍在持续演进。尤其是 Linux 端的 RPMI TEE Backend，2026 年 10 月 5 日的 RFC v2 已将通用 RPMI Transport 和 OP-TEE Backend 分层，并提出新的 Control ABI；该 RFC 当时明确指出对应的 OP-TEE OS 支持尚未完成。

因此，本章的完整系统方案是基于已实现机制和公开开发方向的工业移植设计，不能将其中所有组件视为已经在主线 Linux、OpenSBI 和 OP-TEE 中实现并互相兼容。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE+2



为了避免混淆，后文会区分：

- Upstream mechanism：已有公开源码、可以直接研究的机制。
- Integration design：具体 SoC 项目中需要设计和集成的部分。
- Illustrative code：用于阐释设计思想的代码，不保证可以直接编译。
- Validation requirement：必须通过测试证明的安全或功能特性。

# 21.1 目标 SoC 硬件与安全能力分析

## 21.1.1 为什么不能直接从 OP-TEE 代码移植开始？

假设你拿到一颗刚完成 RTL 设计的 RISC-V SoC，硬件团队告诉你：

> 这颗芯片支持 RV64、16 个 Hart、PMP、Sv48、AIA，还有一个独立的 Security Processor。希望把 OP-TEE 运行起来。

你能否直接从 `optee_os/core/arch/riscv/` 开始修改代码？

技术上当然可以尝试，但这种流程很容易陷入一个典型误区：

把 Linux 能启动、OP-TEE 能运行，误认为整个 SoC 已具备正确的 TEE 安全能力。

对于 OP-TEE 而言，真正的先决条件不是 CPU 能否执行 RV64 指令，而是：

1. 是否存在能够防止 REE 访问 TEE 物理内存的硬件机制？
2. 多个 Hart 的隔离配置是否一致？
3. DMA Master 是否能够绕过 CPU 的 PMP 访问 TEE？
4. 谁负责加载 OP-TEE Image，并保证它没有被替换？
5. 谁负责从 Linux Domain 安全切换至 TEE Domain？
6. TEE 使用的密码学密钥来自哪里？由谁保护？

这些问题如果在架构阶段无法回答，那么 Porting 工作可能从一开始就缺乏正确的安全前提。

## 21.1.2 建立本章参考 SoC

为了便于后续实现，我们定义以下假设平台：

Reference SoC — RV64 Server Security Platform

Application Processor Complex

Hart 0–7

RV64 M/S/U

Hart 8–15

RV64 M/S/U

Sv48 · PMP · AIA · Atomics · Optional FP/Vector

RSS / HSM

独立物理核心

Boot Verification

Key Management

Crypto / OTP

SoC Fabric

DRAM Controller

MMIO / Mailbox

Firewall / IOPMP

DMA / PCIe

System Memory / Peripherals

DDR · Boot Flash · UART · Timer · Storage · Crypto DMA

教学参考设计，并非某款芯片的实际硬件定义。RSS 是独立的安全处理器，不属于 Application Hart 集合。

我们假设：

| 组件                  | 本章参考配置               | 软件职责                      |
| ------------------- | -------------------- | ------------------------- |
| Application CPU     | 16 × RV64 Hart       | Linux / OP-TEE            |
| Privilege           | M/S/U                | OpenSBI / TEE Kernel / TA |
| Address Translation | Sv48                 | Linux/OP-TEE 虚拟内存         |
| CPU Isolation       | PMP                  | S/U 物理内存访问控制              |
| System Isolation    | IOPMP 或 SoC Firewall | DMA、外设及总线隔离               |
| Interrupt           | APLIC + IMSIC        | Interrupt Dispatch        |
| HSM/RSS             | 独立 Secure Core       | Root of Trust             |
| Memory              | DDR + SRAM           | Firmware / Kernel / TA    |
| Transport           | SBI MPXY / RPMI      | REE ↔ TEE                 |
| Debug               | UART / JTAG / Trace  | Bring-up 和问题定位            |

这里有一个非常重要的区别：

这些能力是我们为了分析而假定的平台特性，并不代表 RV64 SoC 天然拥有它们。

例如：

- 支持 PMP，不代表所有 DMA 都受到限制。
- 支持 Sv48，不代表 MMU 可以隔离两个处于不同时间执行的 S-mode Domain。
- 支持 AIA，不代表中断自然具备 REE/TEE 归属语义。
- 有独立 HSM，不代表 Main CPU 的启动代码必然经过认证。
- 支持 OP-TEE，不代表系统已经具备可证明的安全隔离能力。

## 21.1.3 Hardware Capability Matrix

在真实项目中，我建议第一份正式设计文档就是 Hardware Capability Matrix。

| 能力               | 必须确认的硬件细节                   | 如果缺失会怎样            |
| ---------------- | --------------------------- | ------------------ |
| M/S/U            | 是否所有应用 Hart 都支持所需 Privilege | 可能需要不同 TEE 执行模型    |
| PMP              | Entry 数量、粒度、锁定行为、覆盖范围       | 无法规划可靠的物理隔离        |
| DRAM             | 地址布局、ECC、初始化依赖              | 可能无法安全加载 TEE       |
| IOPMP / Firewall | 保护对象和可编程粒度                  | DMA 可绕过 CPU PMP    |
| AIA              | APLIC/IMSIC 拓扑与路由方式         | Secure IRQ 需要额外方案  |
| Boot ROM         | 签名认证与密钥根                    | 无法建立 Verified Boot |
| RSS/HSM          | IPC、身份校验、密钥句柄               | 密钥服务边界不明确          |
| Debug            | JTAG、Trace 是否可在量产模式关闭       | 可能绕过 TEE 隔离        |
| Reset            | Cold/Warm/Partial Reset 语义  | 安全状态可能遗留           |
| Power            | Hart Suspend、DRAM Retention | Resume 路径可能破坏安全    |

这张表必须由 Firmware、RTL、Verification 和 Security 团队共同确认。

### 一个重要的设计实例：PMP 不等于 SoC Firewall

假设 OP-TEE 的物理内存是：

```
TEE PA:
    0xF000_0000 - 0xF3FF_FFFF

Linux S-mode:
    PMP denies access to TEE PA
```

Linux CPU 执行：

```
li t0, 0xF0000000
ld t1, 0(t0)
```

如果该地址当前没有任何 S-mode 页表映射，首先可能发生 Page Fault；即使建立了有效的映射，最终的物理访问仍必须通过 PMP 权限检查。

但如果 Linux 可以控制一台 DMA Engine：

```
Linux
  |
  | Program DMA descriptor
  v
DMA Controller
  |
  | AXI / NoC transaction
  v
TEE Physical Memory
```

CPU 的 PMP 通常不约束这个独立 DMA Master。

如果 SoC 没有正确配置 IOPMP、IOMMU 或总线 Firewall，DMA 可能直接读写 TEE 物理内存。

因此，CPU-side Isolation 和 System-level Isolation 必须分别验证。这一点也是 OpenSBI RP016 Hardware Isolation Framework 试图解决的重要问题之一。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



## 21.1.4 如何确定是否允许进入 Porting 阶段？

建议设置明确的 Architecture Readiness Gate。

在进入实际 OP-TEE 移植之前，至少需要拿到以下证据：

- 所有参与执行 TEE 的 Hart 均具备可配置的 CPU 隔离机制。
- TEE 物理内存不会被未经授权的 CPU、DMA 或外设访问。
- Secure Boot 的完整信任链已定义。
- HSM 与 Main CPU 的通信和信任关系已明确。
- Domain Switch 由可信的 M-mode Firmware 控制。
- Secure Memory、Shared Memory、MMIO 的地址布局已经规划。
- 对当前无法解决的安全限制形成明确记录，而不是隐藏风险。

如果上述条件不能完全满足，可以继续进行功能原型开发，但不应宣称已经完成 Production-grade TEE Isolation。

# 21.2 SoC Security Threat Model 与 TCB

## 21.2.1 Threat Model 要先于代码设计

考虑以下两个系统：

- 系统 A：Linux 和 OP-TEE 都能启动，TA 可以正常执行。
- 系统 B：不仅能启动，还能保证恶意 Linux Kernel 无法读取 TA 密钥、不能替换 OP-TEE Kernel、不能通过 DMA 修改 Secure Memory。

哪一个更符合 TEE 的目标？

显然是系统 B。

因此我们首先定义本项目的核心安全目标：

> 即使 REE Linux Kernel 已经遭到攻击者控制，只要 Root of Trust、M-mode Firmware、OP-TEE 及所依赖的硬件隔离机制仍然可信，攻击者仍然不能越过定义的安全边界，直接读取或修改受保护的 TEE 数据。

这里的目标是保护机密性和完整性，不意味着可以完全阻止恶意 Linux 拒绝服务。

## 21.2.2 明确攻击者能力

为了避免含糊不清，我们假定攻击者可以：

1. 在 Linux 中获得 root 权限。
2. 修改 Linux Kernel 或加载恶意 Kernel Module。
3. 伪造 TEE Driver 传入的参数。
4. 构造畸形 Shared Memory、RPMI 消息。
5. 反复触发 ECALL、Interrupt、RPC。
6. 操纵 Linux 可控制的 DMA Master。
7. 修改非可信磁盘或 Flash 中的文件。
8. 发起并发请求、系统重启和异常恢复攻击。

但初始版本不把攻击者已经控制 M-mode OpenSBI、RSS/HSM Secure Firmware 或 Boot ROM 作为安全保证的前提，因为这些实体属于核心 TCB。相关实体的漏洞、供应链攻击和物理攻击可以作为独立的扩展威胁模型处理。

## 21.2.3 构建 Trust Boundary

Trusted Computing Base

Boot ROM / RSS

OpenSBI / Monitor

OP-TEE Core

HW Isolation Policy

特定可信 TA、固件驱动、密钥服务和必要的硬件控制器也属于相应安全目标的 TCB。

Untrusted Input Boundary

Untrusted REE

Linux Kernel

Linux Userspace

Shared Memory

Untrusted Storage

关键边界不在于代码运行在 S-mode 还是 U-mode，而在于访问权限由谁执行、谁能够改变它以及这些权限能否被不可信主体绕过。

这带来了一个很值得深究的问题：

OP-TEE 的安全性并不是只由 OP-TEE Kernel 自身决定的。

在本章参考架构中，至少有四类 TCB：

| TCB 类别       | 主要职责                              |
| ------------ | --------------------------------- |
| Hardware TCB | 隔离物理内存、设备访问与安全状态                  |
| Boot TCB     | 验证 Firmware、限制回滚并建立可信启动路径         |
| M-mode TCB   | 管理 Domain、Hart 状态及跨 Domain 切换     |
| TEE TCB      | TA 隔离、密钥服务、Secure Storage 和安全 API |

一个常见的系统设计错误是：

Linux 无法直接访问 OP-TEE 内存，但 Linux 能够修改一段 OpenSBI 正在信任的 Domain Policy。

这样一来，即使 PMP 自身实现完全正确，系统仍然可能被攻破。

原因是：

攻击者不需要绕过 PMP，只需要让可信 Firmware 配置一条不正确的 PMP Policy。

## 21.2.4 定义五项必须维持的安全不变量

我建议把 Security Invariants 明确写进项目设计文档，供后续 RTL、Firmware、Linux Driver 和测试团队共同使用。

| 编号     | Security Invariant                                        |
| ------ | --------------------------------------------------------- |
| INV-01 | 非可信 Linux 无法直接读写 TEE Private Memory                       |
| INV-02 | 非可信 DMA Master 无法绕过隔离访问 TEE Private Memory                |
| INV-03 | Domain Switch 不会把上一个 Domain 的敏感寄存器、CSR 或执行状态泄露给下一个 Domain |
| INV-04 | TEE 仅接受经过认证、授权且范围合法的内存共享及服务请求                             |
| INV-05 | 每次 Boot/Resume/Recovery 都必须重新建立正确的信任与隔离状态                 |

在实际评审中，应该要求每个不变量都能够指向一个具体的硬件配置、一段固件逻辑和至少一个正向/反向测试。

例如对 INV-03：

- 设计证据：OpenSBI Domain Context 保存结构及切换算法。
- 实现证据：GPR、CSR、FP、Vector 等状态的保存和恢复。
- 测试证据：两个 Domain 使用不同的敏感寄存器内容，反复切换并验证状态不泄露。
- 故障证据：故意破坏测试版本中的保存逻辑，测试应能够检测差异。

这才是工程上可验证的 Security Design。

需要注意的是，缓存侧信道、推测执行、物理探测、故障注入等风险不会因为五个不变量满足就自然消失，需要额外评估其攻击面和产品要求。

# 21.3 Boot Chain 与 Firmware Integration

## 21.3.1 先明确三个不同概念

在实际移植过程中，经常会听到三个说法：

- OP-TEE Image 已经加载到 DDR。
- OP-TEE Image 已经经过 Secure Boot Verification。
- OP-TEE 已经进入可信执行环境。

这三件事完全不同。

Image Loading 只表示把 Binary 放到了某个地址。

Image Authentication 表示验证 Image 的来源、完整性及相关安全策略。

Trusted Execution 则要求运行时的隔离、安全状态和权限策略已经建立。

一个 Image 即使通过签名认证，如果运行之前没有配置正确的硬件隔离，它仍然可能在运行时被 Linux 或恶意 DMA 修改。

因此，完整启动信任链需要同时解决代码可信和执行环境可信两个问题。

## 21.3.2 参考 SoC 的完整启动流程

我们假设 RSS/HSM 负责启动认证，Application Hart 运行 OpenSBI、OP-TEE 和 Linux。

```
SoC Power On / Reset
        |
        v
+-----------------------+
| Immutable Boot ROM    |
| Root of Trust         |
+-----------------------+
        |
        | Verify RSS Firmware
        v
+-----------------------+
| RSS / HSM Firmware    |
| Secure Boot Services  |
| Key / Policy Control  |
+-----------------------+
        |
        | Verify or authorize
        | AP firmware images
        v
+-----------------------+
| Application CPU       |
| Boot Firmware / M-mode|
+-----------------------+
        |
        v
+-----------------------+
| OpenSBI Initialization|
| Hart / Trap / Domain  |
+-----------------------+
        |
        | Establish isolation
        | Load / authorize TEE
        v
+-----------------------+
| OP-TEE Boot           |
| MMU / Thread / TA RAM |
+-----------------------+
        |
        | Domain ready
        v
+-----------------------+
| U-Boot / UEFI         |
| Boot Linux            |
+-----------------------+
        |
        v
+-----------------------+
| Linux Kernel          |
| TEE Driver / RPMI     |
+-----------------------+
        |
        v
+-----------------------+
| tee-supplicant / CA   |
| Trusted Application   |
+-----------------------+
```

这是逻辑依赖顺序，不要求每个实际平台的 CPU 指令执行顺序完全如此。

例如，OpenSBI 可能在初始化阶段创建并启动多个 Supervisor Domain；某些 Bootloader 和 OP-TEE 初始化可以按照平台约定交错执行。UEFI 也不一定存在。

### 为什么 OP-TEE 和 UEFI 不互相替代？

两者职责不同：

| 组件      | 核心职责                                     |
| ------- | ---------------------------------------- |
| OpenSBI | M-mode Runtime Services、Hart、Domain、底层隔离 |
| OP-TEE  | Trusted OS、TA 执行和安全服务                    |
| UEFI    | Boot Services、系统发现、镜像装载及 OS Boot         |
| RSS/HSM | Hardware RoT、认证、密钥及安全控制                  |
| Linux   | REE OS、设备驱动、普通应用                         |

UEFI 可以负责加载 Linux，并参与认证流程；但不能仅凭 UEFI 的存在代替运行时 Domain 隔离。

同样，OpenSBI 提供 SBI Runtime Services，也不意味着它天然完成了 UEFI 的 OS Boot 管理职责。

## 21.3.3 RSS/HSM 和 Application CPU 如何协作？

一个合理的硬件与 Firmware 设计可以采用以下模型：

```
RSS / HSM                          Application CPU
-----------                        ----------------

Secure Reset
    |
Boot ROM
    |
Verify HSM FW
    |
Initialize Security HW
    |
Verify AP Boot Images
    |
Program Initial Protection
    |
Release AP Reset  ---------------->  AP Reset Vector
                                         |
                                         v
                                      OpenSBI
                                         |
                                         v
                                   Domain Creation
                                         |
                                         v
                                   OP-TEE / Linux
```

这里存在三个非常容易被忽略的问题。

第一个：验证完成后，Image 是否还能被篡改？

假设 HSM 验证 DDR 中的 OP-TEE Image，但验证完成与执行开始之间，Linux 或其他 Bus Master 仍然能够写入 DDR：

```
HSM Verify Image
      |
      | Image is valid
      v
DMA overwrites Image
      |
      v
CPU executes modified Image
```

这种设计存在典型的 Time-of-Check to Time-of-Use 风险。

解决思路包括在认证前后建立受保护的加载区域、限制所有未授权写入者，并保证认证的对象就是后续实际执行的对象。

第二个：谁有权配置硬件 Firewall？

如果 Linux 运行后仍能通过 MMIO 修改 Security Firewall 配置，就可能撤销自己受到的限制。

因此，Security Control Register 必须被放置在可靠的权限边界中，写权限只交给真正可信的 Firmware/硬件实体。

第三个：Warm Reset 时安全状态怎么办？

冷启动时验证的内容，不一定能够自动覆盖 Warm Reset、Retention Resume 和部分电源域重启。

每条路径都应定义：

- 哪些硬件权限会被清除？
- 哪些内存内容得到保留？
- 哪些密钥或会话状态仍然有效？
- 是否需要重新认证？
- 如何防止一个过期 Domain Context 被恢复？

## 21.3.4 Firmware Image Manifest 设计

真实工程中，我建议使用版本化的 Firmware Manifest 描述 Image，而不要仅依赖打包脚本中的固定地址。

下面是一个概念示例：

```
# Illustrative format, not a standardized OP-TEE manifest

platform: rv64-server
security_policy_version: 3

images:
  - name: opensbi
    version: 12
    privilege: M
    load_address: 0x80000000
    hash_algorithm: sha384
    signature_required: true

  - name: optee
    version: 8
    privilege: S
    domain: tee
    load_address: 0xF0000000
    hash_algorithm: sha384
    signature_required: true

  - name: uefi
    version: 21
    privilege: S
    domain: ree
    signature_required: true

rollback_policy:
  require_monotonic_security_version: true
```

这个格式仅用于表达设计。真正的 Manifest 需要与 RSS Boot ROM、Firmware Packaging 和 Secure Boot 规范一致。

另外，`load_address` 不是单凭一份 Manifest 就可以信任的值。固件必须检查地址范围、溢出、重叠、入口点和执行权限。

### Boot Chain 的建议验收标准

| 测试                 | 预期结果             |
| ------------------ | ---------------- |
| 合法 OP-TEE Image    | 正常启动             |
| 修改 OP-TEE 一个 Byte  | 签名或哈希验证失败        |
| 使用已撤销的密钥           | 启动拒绝             |
| 使用低于安全版本的旧 Image   | Anti-rollback 拒绝 |
| Image 超出 TEE 预留区   | 拒绝加载             |
| 更改未经认证的入口地址        | 拒绝执行             |
| Warm Reset 后修改保护配置 | 保护应重新正确建立        |

这些用例应在正式 Linux Bring-up 之前就开始验证。

# 21.4 OpenSBI Domain 配置与硬件隔离

这是本章第一个实现难点。

## 21.4.1 为什么不把 OP-TEE 简单运行在 Linux 同一个 S-mode 环境中？

考虑以下情况：

```
Linux Kernel:  S-mode
OP-TEE Core:   S-mode

Both can access:
    DDR
    Device MMIO
    Privileged CSR
```

如果二者只是各自维护不同的 `satp`，但 Linux 能随意重建自己的 Page Table，那么 Linux 仍可能把 TEE 的物理内存映射进自己的虚拟地址空间。

因此：

Page Table Isolation 不能单独成为两个互不信任的 S-mode OS 之间的安全边界。

一种设计是利用可信 M-mode Firmware 管理不同 Supervisor Domain，并使用 PMP 或其他硬件机制约束各个 Domain 的 S-mode 物理访问。

## 21.4.2 为参考 SoC 设计内存布局

我们假设 DRAM 容量为 2 GiB，物理地址为：

```
DRAM:
    0x8000_0000 - 0xFFFF_FFFF
```

示例分配如下：

| 物理地址范围                    | 大小        | 用途                       |
| ------------------------- | --------- | ------------------------ |
| `0x8000_0000–0x801F_FFFF` | 2 MiB     | 示例 OpenSBI 保留区           |
| `0x8020_0000–0xEEFF_FFFF` | 其余普通 DRAM | Linux / Bootloader / REE |
| `0xEF00_0000–0xEFFF_FFFF` | 16 MiB    | Static Shared Memory     |
| `0xF000_0000–0xF3FF_FFFF` | 64 MiB    | OP-TEE Private RAM       |
| `0xF400_0000–0xFFFF_FFFF` | 其余普通 DRAM | Linux / REE              |

以上地址全部为教学示例，不是 QEMU virt 或实际 SoC 的强制布局。OpenSBI 的占用范围也必须按真实 ELF 和加载方式核对。

我们把这 64 MiB Private RAM 再逻辑分成：

```
TEE Private RAM: 64 MiB
    |
    +-- OP-TEE Core Text / RO Data
    |
    +-- OP-TEE Core RW Data / BSS
    |
    +-- Per-Hart Core Stacks
    |
    +-- Trusted Thread Stacks
    |
    +-- TA RAM / TA Heap
    |
    +-- Page Tables / Pager / Other Pools
```

这些子区域的实际大小应由 OP-TEE Build Configuration、Image Size、TA 数量、Thread 数量和安全机制共同决定，不应预先假设一定能装入 64 MiB。

## 21.4.3 理解 Domain Memory Policy

概念上我们希望达到：

| 资源                     | Linux Domain       | OP-TEE Domain     |
| ---------------------- | ------------------ | ----------------- |
| Linux RAM              | R/W/X，按 Linux 页表约束 | 默认不可访问，除显式共享      |
| OP-TEE Core Text       | Deny               | R/X               |
| OP-TEE Core Data       | Deny               | R/W，禁止执行          |
| OP-TEE TA RAM          | Deny               | 根据 OP-TEE MMU 管理  |
| Shared Memory          | 经授权可读写             | 经验证与映射后可访问        |
| Normal UART            | 允许                 | 通常不需要             |
| Secure Crypto MMIO     | Deny               | 按所有权策略允许          |
| Security Firewall MMIO | Deny               | 通常由 M-mode/HSM 控制 |

需要注意 PMP 的天然权限粒度、M-mode 的权限语义，以及 OP-TEE Core 内更细粒度权限的实际支持情况。RP022 M8 正在推进通过 PMP 区分 OP-TEE Core 的只读、可执行和可写区域，并需要相应的 NAPOT 内存布局。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### Device Tree 如何描述保留内存？

下面的 Device Tree 示例只负责向 Linux 和相关消费者描述内存布局，不是 OpenSBI Domain 权限策略的完整实现：

```
/ {
    #address-cells = <2>;
    #size-cells = <2>;

    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;

        tee_private: tee@f0000000 {
            reg = <0x0 0xf0000000
                   0x0 0x04000000>;
            no-map;
        };

        tee_shm: shm@ef000000 {
            reg = <0x0 0xef000000
                   0x0 0x01000000>;
        };
    };
};
```

其中：

- `tee_private` 是 TEE 的私有区域。
- `no-map` 告知 Linux 不要将该区域放入普通线性映射。
- `tee_shm` 是示例中留给静态共享内存的区域。

一定要记住：Device Tree 的 `reserved-memory` 不是安全机制。

Linux Kernel 被攻击者控制后，完全可以忽略这一描述。真正的访问拒绝必须由 PMP、IOPMP 或其他硬件机制执行。

同时，具体共享内存节点的 `compatible`、分配器以及缓存属性，需要根据选定的 Linux TEE Transport 正式 ABI 决定。

## 21.4.4 OpenSBI Domain 如何切换？

在 OpenSBI 的实际实现中，可以重点阅读：

`lib/sbi/sbi_domain_context.c`

其中存在如下接口：

```
int sbi_domain_context_enter(struct sbi_domain *dom);
int sbi_domain_context_exit(void);
```

当前代码中的 `struct hart_context` 保存了 Trap Context 和 Supervisor CSR 状态，例如：

- `sstatus`、`sie`、`stvec`
- `sscratch`、`sepc`、`scause`、`stval`
- `satp`、`scounteren`、`senvcfg`
- FP/Vector Context
- 当前 Domain 和上一个 Context 的关系

其核心切换代码还涉及 Hart Assignment 更新和 PMP 重新配置。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



下面以概念伪代码重新组织这一逻辑：

```
/*
 * Illustrative pseudocode.
 * Not the literal OpenSBI implementation.
 */
int switch_domain(struct domain *next)
{
    struct domain *prev = current_domain();

    if (!authorized_transition(prev, next))
        return -EPERM;

    save_current_execution_state(prev);

    /*
     * Transition must not expose the destination
     * domain before isolation becomes effective.
     */
    apply_target_hardware_isolation(next);

    restore_target_execution_state(next);

    current_domain_set(next);

    return resume_target_domain();
}
```

这段代码强调的是责任，而不是一个可以照搬的编程顺序。

实际 OpenSBI 必须正确处理 Trap Context、权限更新、地址翻译同步和返回上下文。调用顺序还取决于某些控制寄存器是否需要在当前权限下修改。

特别需要关注的是：

Domain Switch 是 Trust Boundary Crossing，而不只是 Context Switching。

因为它至少承担三个任务：

1. Execution State Transfer
2. Hardware Protection Reconfiguration
3. Resource Ownership Transition

传统操作系统中的 Task Switch 主要解决第一项；TEE Domain Switch 必须同时满足另外两项。

## 21.4.5 OpenSBI Hardware Isolation Framework

RP016 M4 定义了一个比较有代表性的模型：不同硬件隔离机制分别注册自己的操作，并跟随 Domain 生命周期进行初始化和配置。

其接口包括：

`init()`、`domain_init()`、`domain_exit()`、`domain_enter()`、`domain_cleanup()`

相应的设计目标是让不同 SoC 能集成 WorldGuard 或其他硬件安全机制，而不用把每个厂商的 Firewall 寄存器操作写死到通用 Domain Context 代码中。该框架公开文档中的状态仍处于 upstream review 阶段，需要核对实际使用分支。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



可以把它理解为：

```
OpenSBI Domain Manager
          |
          v
Hardware Isolation Framework
          |
     +----+--------+---------+
     |             |         |
     v             v         v
   PMP        WorldGuard   SoC Firewall
  Driver         Driver      Driver
     |             |         |
     +-------------+---------+
                   |
                   v
        Target Domain Policy
```

但这里有一个重要设计约束：

不同隔离机制之间的配置不能产生短暂的权限扩大窗口。

例如：

```
Clear Source Firewall
        |
        | Temporary unrestricted access?
        |
Configure Destination Firewall
```

如果两步之间存在不受控的 DMA 访问，可能造成瞬时越权。

更安全的策略通常需要考虑 Fail-closed、阻断访问、等待 Outstanding Transaction 完成、应用目标策略和错误恢复，并由具体硬件文档确定合法顺序。

## 21.4.6 Domain Isolation 的实际测试方案

至少需要设计以下测试：

| Test ID | 测试场景                     | 预期行为                |
| ------- | ------------------------ | ------------------- |
| DOM-01  | Linux 读取 TEE PA          | Access Fault 或被硬件拒绝 |
| DOM-02  | Linux 写入 TEE PA          | Access Fault 或被硬件拒绝 |
| DOM-03  | TEE 访问授权 SHM             | 成功                  |
| DOM-04  | TEE 访问未授权 Linux PA       | 被拒绝                 |
| DOM-05  | Linux 发起非法 Domain Switch | 拒绝                  |
| DOM-06  | DMA 读取 TEE PA            | Firewall/IOMMU 拒绝   |
| DOM-07  | 多 Hart 不断切换 Domain       | 隔离持续成立              |
| DOM-08  | Warm Reset 后重新验证         | 隔离正确恢复              |

注意：CPU 访问测试需要构造有效 VA→PA 映射，避免把 Page Fault 错误解释为 PMP 拒绝。应结合 `scause`、`stval`、PTE 和硬件错误寄存器确认真实原因。

另外，完成这些正向和反向测试后，仍然需要进行内存权限覆盖审计：确认不存在未测试的物理地址别名、DMA 通道或可修改的 Security MMIO。

# 21.5 OP-TEE Core Boot 与 MMU 初始化

完成硬件隔离后，才进入 OP-TEE 自身的核心初始化。

## 21.5.1 OP-TEE Boot 的最小前置条件

从 OpenSBI 跳转到 OP-TEE 之前，至少需要确认：

| 项目              | 条件                                    |
| --------------- | ------------------------------------- |
| Instruction Set | 必需的 RV64 扩展可用                         |
| Privilege       | 进入正确的 S-mode                          |
| Image           | Image 已认证并放置到有效内存                     |
| Entry Point     | 入口地址处于可信可执行范围                         |
| Memory          | TEE RAM 已保留并隔离                        |
| Stack           | Early Boot 有可用的栈                      |
| Trap            | Early Trap Policy 已定义                 |
| Hart            | Primary / Secondary Boot Protocol 已定义 |
| DT / Handoff    | DTB 和启动参数来源、权限及生命周期明确                 |

其中一个容易忽略的问题是 `satp`。

不能假设在进入 OP-TEE 时 `satp` 必然为零，也不能假设 `stvec`、`sscratch`、`sstatus` 等 CSR 具有某个固定值。

这些状态必须由 Boot ABI 明确规定，或者在 OP-TEE Early Entry 代码中安全初始化。

## 21.5.2 完整 Early Boot 状态转换

概念流程如下：

```
M-mode OpenSBI
      |
      | Set target entry / privilege
      | Prepare handoff state
      v
mret
      |
      v
S-mode OP-TEE Entry
      |
      +-- Establish early stack
      |
      +-- Validate boot parameters
      |
      +-- Initialize BSS / runtime
      |
      +-- Setup initial memory mappings
      |
      +-- Configure satp
      |
      +-- Synchronize address translation
      |
      +-- Initialize core allocators
      |
      +-- Initialize trusted threads
      |
      +-- Initialize traps / interrupts
      |
      +-- Initialize TEE services
      |
      v
OP-TEE Ready
```

这是行为级流程，实际 `entry.S` 和 `boot.c` 的排列需要以当前版本源码为准；某些内存初始化和处理器状态设置可能在不同阶段交错完成。

### 从 OpenSBI 转入 S-mode 的最小汇编思想

下面代码展示的是 RISC-V 特权切换的关键原理，不是从项目中摘取的完整 OpenSBI Boot Code：

```
/* Illustrative RV64 M-mode handoff */

    /* Preserve all unrelated mstatus bits */
    csrr    t0, mstatus

    /* Set MPP = S-mode (binary 01) */
    li      t1, (3 << 11)
    not     t1, t1
    and     t0, t0, t1

    li      t1, (1 << 11)
    or      t0, t0, t1

    csrw    mstatus, t0

    /* Target S-mode entry point */
    la      t0, optee_entry
    csrw    mepc, t0

    mret
```

这段代码只演示 `mstatus.MPP`、`mepc` 和 `mret` 的关系。它没有处理真实的 Delegation、PMP、Interrupt、DTB、地址翻译状态或多 Hart 初始化，不能直接拿来替代生产固件。

需要重点理解：

`mret` 并不是普通的 `jalr`。

它会根据 `mstatus.MPP` 恢复目标 Privilege，并跳转到 `mepc`。因此，OpenSBI 必须提前把目标执行环境配置成合法状态。

## 21.5.3 OP-TEE MMU 为什么需要独立初始化？

虽然 OpenSBI 已经使用 PMP 约束 TEE 的物理访问范围，但 OP-TEE 仍然需要自己的 Page Table。

原因在于两个隔离层次不同：

- PMP：限制某个 Supervisor Domain 可以访问哪些物理资源。
- OP-TEE MMU：决定 OP-TEE Core 和 TA 如何使用各自的虚拟地址空间。

例如：

```
OP-TEE Core VA
      |
      | Sv48 Translation
      v
OP-TEE Core PA
      |
      | PMP Physical Permission Check
      v
Physical Memory
```

同样：

```
User TA VA
      |
      | TA Page Table
      v
TEE Private PA
      |
      | PMP Permission
      v
Physical Memory
```

CPU 执行内存访问时，这些机制必须共同允许该操作。

因此，即使 PMP 已允许 OP-TEE Domain 访问整个 TEE RAM，OP-TEE 自身也不能允许任意 TA 直接访问整个 TEE RAM。

TA 的细粒度隔离仍然依赖 OP-TEE 的 User VA Management 和 S/U Page Permission。

## 21.5.4 `satp` 与 TLB 的关键工程问题

在 RV64 Sv48 模式下，`satp` 包含：

- MODE
- ASID
- Page Table Root PPN

假设 OP-TEE 创建了一套新的 Page Table：

```
/*
 * Illustrative helper.
 * Assumes Sv48 is supported and root_pa is
 * a valid aligned page-table physical address.
 */
static uint64_t make_satp_sv48(uint64_t root_pa,
                               uint16_t asid)
{
    const uint64_t sv48 = 9ULL << 60;
    const uint64_t asid_field = (uint64_t)asid << 44;
    const uint64_t ppn = root_pa >> 12;

    return sv48 | asid_field | ppn;
}
```

切换页表根通常需要处理：

```
    csrw satp, a0
    sfence.vma
```

但这里有三个非常值得关注的细节。

第一：写 `satp` 并不天然完成所有需要的地址翻译同步。

必须根据 ASID、Page Table 修改情况以及 RISC-V Privileged Architecture 的规则确定 `SFENCE.VMA` 的范围和时机。

第二：`SFENCE.VMA` 是 Hart-local 的。

如果修改了另一个 Hart 正在使用的 Page Table，那么本地执行 `SFENCE.VMA` 并不自动让另一个 Hart 的 TLB 状态失效。

需要正确实现 Remote TLB Shootdown，或者保证不会使用已经失效的 Translation。

第三：不同 TA 的 ASID 复用需要严格管理。

如果一个 TA 结束，另一个 TA 复用了相同的 ASID，而没有执行必要的同步，旧的 TLB Translation 可能影响新地址空间。

这一类错误既可能是普通内存访问故障，也可能造成安全边界错误。

## 21.5.5 OP-TEE Core Boot 调试策略

实际 Bring-up 时，我推荐分成五个检查点。

1. BP1：进入 OP-TEE Entry

   检查 `pc`、`mstatus`、`sstatus`、`satp`、DTB/启动参数和当前 Hart ID。
2. BP2：建立 Early Stack

   检查 Stack PA/VA、Stack Bounds、SP Alignment，以及是否与其他 Hart 的 Stack 重叠。
3. BP3：MMU Enable / Page Table Switch

   检查 Root Page Table、PTE 权限、`satp`、`sfence.vma` 和新的 VA→PA 映射。
4. BP4：Trusted Thread Initialization

   检查 Trusted Thread Pool、Per-Hart State、Thread Stack 和 Trap Vector。
5. BP5：OP-TEE Ready

   检查安全服务初始化、Shared Memory 配置、Transport Endpoint 和后续调用入口。

一个简单但有价值的 GDB 操作是：

```
# Read architectural status
info registers pc sp ra a0 a1

# Read a CSR if the GDB target exposes it
p/x $satp
p/x $sstatus
p/x $stvec
p/x $sepc
p/x $scause
p/x $stval

# Inspect current instructions
x/16i $pc

# Inspect stack
x/16gx $sp
```

具体 CSR 是否可通过 GDB 读取、读取哪个 Hart、采用何种调试权限，取决于 QEMU、GDB Server 和实际 JTAG Debug Module。

### 典型问题：打开 MMU 后立刻崩溃

经常看到类似现象：

```
OP-TEE early log...
MMU enable
<no further output>
```

正确的排查思路不是先怀疑 UART Driver。

应该先检查：

1. `satp` 指向的 Page Table 是否正确。
2. 当前 PC 所在的地址是否有可执行映射。
3. 当前 Stack 是否有可读写映射。
4. 当前 UART MMIO 是否仍然可访问。
5. `stvec` 是否指向有效可执行地址。
6. 是否发生重复 Page Fault，导致异常入口再次异常。
7. PMP 是否拒绝了新的物理地址访问。

如果 `scause` 和 `stval` 能够保存下来，就可以将问题定位到更准确的层次。

# 21.6 RPMI / MPXY / TEE Driver 集成

这是整个项目中最容易出现跨团队集成问题的部分。

OP-TEE Core 可能已经能够启动，但是 Linux 端仍然无法成功执行：

```
TEEC_InitializeContext(NULL, &context);
```

这种问题未必出在 OP-TEE Core，而可能发生在 Linux Driver、RPMI Transport、OpenSBI Channel、Domain Switch 或 ABI 协商中的任意环节。

## 21.6.1 先分清三个不同的协议层次

在传统 Arm TrustZone OP-TEE 中，最常见的结构是：

```
Linux OP-TEE Driver
        |
        | OP-TEE Message Protocol
        v
SMC ABI / SMCCC
        |
        v
Secure Monitor
        |
        v
OP-TEE Core
```

在我们采用的 RISC-V 架构中，推荐按以下抽象理解：

```
Linux Userspace
        |
        v
Generic Linux TEE Subsystem
        |
        v
OP-TEE Linux Driver
        |
        | TEE session / request
        v
RPMI TEE Backend
        |
        | TEE service messages
        v
RPMI Transport / SBI MPXY
        |
        | SBI ECALL
        v
M-mode OpenSBI
        |
        | Service routing
        | Domain Context Switch
        v
OP-TEE S-mode Domain
        |
        v
Trusted Application
```

三个容易混淆的概念分别是：

| 层次                           | 用途                      |
| ---------------------------- | ----------------------- |
| SBI MPXY                     | 在 SBI 体系下提供消息代理通道       |
| RPMI TEE Service             | 定义 TEE 消息、服务发现、内存共享等语义  |
| OP-TEE Message / Control ABI | 定义具体的 OP-TEE 请求、参数及返回方式 |

MPXY 不等于 OP-TEE，RPMI 也不等于 OpenSBI Domain Context Switch。

它们位于不同层次，必须通过兼容的实现连接起来。

2026 年 10 月的 Linux RFC v2 就把通用 RPMI Transport 从 OP-TEE Backend 中分离，并重用已有的 OP-TEE Session、RPC、Shared Memory 和 Call Queue 逻辑。

[image](https://www.google.com/s2/favicons?domain=https://lists.infradead.org\&sz=32)

lists.infradead.org+1



## 21.6.2 先冻结 ABI，再开始 Linux Driver 集成

真实移植项目里，我非常建议建立一张 ABI Compatibility Matrix：

| ABI 项目                | 必须明确                            |
| --------------------- | ------------------------------- |
| RPMI Version          | 实际支持的服务规范版本                     |
| TEE Service Discovery | 如何发现 OP-TEE Endpoint            |
| Control ABI           | 请求/响应消息格式                       |
| Message Size          | 固定头部、最大 Payload、对齐方式            |
| Endianness            | 线格式及多字节字段编码                     |
| Shared Memory         | Static SHM 还是 Memory Parcel     |
| Parcel Identity       | Handle、Nonce、Owner、Receiver     |
| RPC Resume            | 恢复 Token 及生命周期                  |
| Error Code            | Transport Error 与 TEE Error 的区别 |
| Notification          | 同步 RPC、异步 Signal / Doorbell     |

为什么要这样做？

因为 2026 年 9 月和 10 月的公开 RFC 中，已经出现过不同的协议设计选择。

例如，一种方案偏向在 RPMI TEE_CALL 中承载现有 SMC ABI；另一种方案引入独立的 RPMI Backend 和固定宽度的 Control Message，使用 Parcel ID、Nonce 和 Opaque Resume Token。

这意味着：

不能拿一个 RFC 的 Linux Driver，默认就可以和另一版本的 OpenSBI/OP-TEE 通信。

[image](https://www.google.com/s2/favicons?domain=https://lists.trustedfirmware.org\&sz=32)

lists.trustedfirmware.org+1



在工程实践中，我建议固定如下软件版本：

```
Integration Baseline:
    Linux      : <commit SHA>
    OpenSBI    : <commit SHA>
    OP-TEE OS  : <commit SHA>
    Buildroot  : <commit SHA>
    RPMI Spec  : <revision>
    SBI Spec   : <revision>
    QEMU/Model : <version>
```

任何 Backend 或 ABI 的升级，都必须重新验证整条调用链。

## 21.6.3 从 Linux 到 OP-TEE 的执行过程

假设应用执行一次 TA Command：

```
CA calls TEEC_InvokeCommand()
       |
       v
Linux TEE IOCTL
       |
       v
OP-TEE Driver
       |
       | Prepare arguments
       | Prepare shared memory parcel
       v
RPMI TEE_CALL
       |
       v
SBI MPXY ECALL
       |
       v
OpenSBI M-mode
       |
       | Validate request
       | Identify target endpoint/domain
       | Save Linux domain context
       | Apply target protection
       v
OP-TEE S-mode
       |
       | Assign / resume trusted thread
       | Process message
       v
Trusted Application
```

调用完成后反向返回。

但如果 TA 需要读取 REE 文件，执行路径会不同：

```
OP-TEE
   |
   | RPC needed
   v
Suspend Trusted Thread
   |
   v
Return / Yield to REE
   |
   v
Linux Driver
   |
   v
tee-supplicant
   |
   | Handle filesystem request
   v
Linux Driver
   |
   | Resume suspended request
   v
OpenSBI
   |
   v
OP-TEE
   |
   v
Resume Trusted Thread
```

这里有一个极其重要的区分：

RPC Resume Token 不等于普通的 Linux Thread ID，也不等于任意一个 OpenSBI Domain ID。

Token 应代表特定的未完成安全调用上下文，必须防止伪造、重复恢复、错误会话绑定和过期复用。

## 21.6.4 为什么需要特别关注 Hart Affinity？

早期 RISC-V RPMI TEE RFC 采用过 Per-Hart MPXY Channel 的方案，因为一次调用通常需要让调用 Hart 从 Linux Domain 切换到 OP-TEE Domain。

这引入一个经典并发问题：

```
Linux Task on Hart 3
        |
Prepare TEE request
        |
Task migrates to Hart 7
        |
Issue request through Hart 3 channel?
```

如果底层协议将 Channel、Hart Context 和 Resume State 绑定到同一个物理 Hart，这种迁移可能引发严重错误。

但这不表示所有 RPMI Backend 都必须永久采用 Per-Hart Channel。后续 RFC 已调整 Transport 分层和所有权管理。

所以项目需要先回答：

- Channel 是 Per-Hart、Per-CPU，还是 Per-Endpoint？
- 调用期间能否迁移？
- 哪个层次负责保持 CPU Affinity？
- RPC 返回后是否必须恢复原 Hart？
- CPU Offline 时，挂起中的 Call 应如何处理？

绝不能简单地认为 Linux 自动处理好所有这些约束。

## 21.6.5 建议分四个阶段完成 Transport Bring-up

| 阶段 | 目标                           | 成功判据              |
| -- | ---------------------------- | ----------------- |
| T1 | SBI MPXY 基础调用                | 能发现和使用合法 Channel  |
| T2 | RPMI TEE Service Discovery   | 正确识别 Endpoint 和能力 |
| T3 | 最小 TEE Request/Response      | OP-TEE 收到消息并返回    |
| T4 | Shared Memory / RPC / Resume | 能完成一项实际 TA 操作     |

不要一开始就调试完整 xtest。

如果 T1 失败，首先关注 Linux ↔ OpenSBI。

如果 T2 失败，关注 RPMI Version、Service Discovery、Channel 和权限。

如果 T3 失败，关注 Domain Switch、OP-TEE Entry 和 Control ABI。

如果 T4 失败，关注 Parcel、Trusted Thread、RPC 和 Linux Supplicant。

这样可以把一个高度耦合的问题拆成多个能独立验证的子问题。

# 21.7 Shared Memory、RPC 与 User TA

## 21.7.1 为什么 REE 和 TEE 必须共享内存？

Linux 和 OP-TEE 虽然处于不同的安全域，但它们需要交换数据。

例如 CA 调用 TA 加密 1 MiB 的数据。

不适合把整个 1 MiB 放进 SBI ECALL 的寄存器里。

因此，需要支持：

```
Linux User Buffer
        |
        v
Linux TEE Driver
        |
        v
Shared Memory Object
        |
        | Authorized mapping
        v
OP-TEE Core
        |
        v
Trusted Application
```

这里的重点不是共享内存能不能被两个 Domain 同时访问，而是：

共享内存必须被视为不可信输入，即使它可以被 OP-TEE 读取。

Linux 可以在任意时刻修改 Shared Memory。TEE 对它的访问不能以 Linux 会遵守协议为安全前提。

## 21.7.2 Static Shared Memory 与 Dynamic Shared Memory

我们可以从两个模型理解。

Static Shared Memory：

系统启动时预留固定地址区域。

```
DRAM Shared Pool
0xEF00_0000 ... 0xEFFF_FFFF
```

Linux 和 OP-TEE 在约定的规则下共享这个区域。

优点是简单、适合早期 Bring-up；缺点是空间弹性较差，而且不能由「预留地址」自动推出细粒度授权和生命周期管理。

Dynamic Shared Memory / Memory Parcel：

Linux 动态建立 Shared Memory 对象，通过 Transport 将其授权给 OP-TEE 使用。

在 RPMI TEE 设计中，可以使用 Memory Parcel 来表达共享授权和内存生命周期。部分公开 RFC 还使用 Parcel ID、Nonce、Offset 和 Size 作为参数。

[image](https://www.google.com/s2/favicons?domain=https://lists.infradead.org\&sz=32)

lists.infradead.org



这个模型允许软件维护：

```
Parcel State Machine

CREATED
   |
   v
AUTHORIZED / SHARED
   |
   v
IN_USE
   |
   v
RELEASED
   |
   v
RECLAIMED
```

状态名称是概念化的，不代表特定 RFC 中的完整枚举定义。

重要的是所有权和回收过程必须符合实际 Transport 协议。

### Parcel 的安全校验

对于每个共享内存请求，OP-TEE 至少需要验证：

- Parcel/Buffer 是否存在且有效。
- 请求者是否具有使用权限。
- Offset、Size 相加是否发生整数溢出。
- 请求范围是否完全位于授权区域。
- 当前生命周期是否允许访问。
- 是否存在未完成操作期间的提前回收。
- 是否会重叠到 TEE Private RAM 或其他受保护内存。
- 缓存属性与映射权限是否符合要求。

下面是一个适合放进代码审计中的 C 语言范围检查思路：

```
#include <stdint.h>
#include <stdbool.h>

static bool range_within(uint64_t offset,
                         uint64_t size,
                         uint64_t total)
{
    if (offset > total)
        return false;

    if (size > total - offset)
        return false;

    return true;
}
```

使用 `size > total - offset`，而不是先计算 `offset + size`，可以避免加法溢出。

但实际验证还必须包括 Parcel Ownership、状态、映射属性及请求方身份等，不能认为一个范围检查函数就足够。

## 21.7.3 一个经典的 TOCTOU 漏洞

假设 TA 希望验证一个命令：

```
struct command {
    uint32_t operation;
    uint32_t length;
    uint64_t data_address;
};
```

错误实现：

```
if (cmd->length <= MAX_BUFFER_SIZE) {
    process_data(cmd->data_address,
                 cmd->length);
}
```

`cmd` 指向不可信 Shared Memory。

Linux 可以在检查 `length` 后、使用它之前修改该字段。

这可能导致检查通过的长度和最终使用的长度不一致。

更安全的设计是：

```
struct command local_cmd;

/*
 * Copy untrusted metadata into trusted memory.
 * Validate source bounds before the copy.
 */
copy_from_untrusted(&local_cmd,
                    cmd,
                    sizeof(local_cmd));

if (local_cmd.length > MAX_BUFFER_SIZE)
    return ERROR_BAD_PARAMETERS;

if (!valid_shared_buffer(local_cmd.data_address,
                         local_cmd.length))
    return ERROR_BAD_PARAMETERS;

return process_data(local_cmd.data_address,
                    local_cmd.length);
```

这仍是概念伪代码；真实实现需要安全的 User/Shared Memory Copy API、Parcel 权限检查，并考虑数据内容在处理期间继续被修改的问题。

如果业务要求输入数据在处理期间不可变，还需要复制完整输入、使用授权的内存所有权转移机制，或采用能够保证一致性的协议。

仅复制元数据不能自动解决所有共享数据竞争。

## 21.7.4 RPC 的执行模型

OP-TEE 的一个重要特点是：Trusted Thread 可以在等待 REE 服务时暂停执行。

例如读取 TA Binary：

```
User requests OpenSession
        |
        v
OP-TEE checks TA availability
        |
        | TA not loaded
        v
Request TA loading via RPC
        |
        v
Linux / tee-supplicant
        |
        | Read signed TA file
        v
Return TA binary
        |
        v
OP-TEE validates TA
        |
        v
Load and create TA context
```

这里要注意：

REE 可以提供 TA Binary，但不应该决定这个 Binary 是否可信。

对于普通签名 User TA，OP-TEE 必须执行正确的完整性及签名验证，或者通过其他可信分发/验证机制建立信任。

Linux 可以拒绝提供 TA Binary，造成 DoS；但不能因此被允许任意替换 TA 代码。

这是一个非常典型的设计原则：

Untrusted World 可以提供数据或执行服务，但不能因提供这些资源而自动获得安全决策权。

## 21.7.5 实现一个最小 CA → TA 验证实验

下面是 CA 端的一个简化示例，假定已有与之匹配的 TA UUID 和 Command ID：

```
#include <stdio.h>
#include <tee_client_api.h>

/* Replace with the UUID of the test TA */
static const TEEC_UUID test_uuid = {
    0x12345678, 0x1234, 0x5678,
    {0x12, 0x34, 0x56, 0x78,
     0x90, 0xab, 0xcd, 0xef}
};

#define CMD_TEST_ECHO 0

int main(void)
{
    TEEC_Context ctx;
    TEEC_Session sess;
    TEEC_Operation op = { 0 };
    TEEC_Result res;
    uint32_t origin = 0;

    char buffer[64] = "hello optee";

    res = TEEC_InitializeContext(NULL, &ctx);
    if (res != TEEC_SUCCESS)
        return 1;

    res = TEEC_OpenSession(&ctx, &sess,
                           &test_uuid,
                           TEEC_LOGIN_PUBLIC,
                           NULL, NULL, &origin);
    if (res != TEEC_SUCCESS) {
        TEEC_FinalizeContext(&ctx);
        return 2;
    }

    op.paramTypes =
        TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INOUT,
                         TEEC_NONE,
                         TEEC_NONE,
                         TEEC_NONE);

    op.params[0].tmpref.buffer = buffer;
    op.params[0].tmpref.size = sizeof(buffer);

    res = TEEC_InvokeCommand(&sess,
                             CMD_TEST_ECHO,
                             &op,
                             &origin);

    printf("result=0x%x, origin=0x%x\n",
           res, origin);

    TEEC_CloseSession(&sess);
    TEEC_FinalizeContext(&ctx);

    return res == TEEC_SUCCESS ? 0 : 3;
}
```

这个程序只覆盖 CA 侧。需要配套实现一个 Echo TA，并确保它正确验证参数类型、缓冲区大小和访问权限。示例 UUID 不是现有 TA 的标识符。

这个实验真正重要的不是回显字符串，而是在不同层次设置 Trace Point。

建议记录：

| Trace Point     | 观察内容                           |
| --------------- | ------------------------------ |
| CA Entry        | UUID、Command ID、Parameter Type |
| Linux TEE IOCTL | Session 与 Shared Memory        |
| RPMI TEE_CALL   | Endpoint、Request ID            |
| OpenSBI         | 当前 Hart、Source/Target Domain   |
| OP-TEE Entry    | Trusted Thread ID              |
| TA Entry        | TA Session、参数                  |
| TA Return       | Return Code                    |
| Linux Return    | Error Origin、耗时                |

最后应能把整条路径还原成一个完整的 Sequence Diagram。

## 21.7.6 需要额外验证哪些安全问题？

除了功能测试，还应重点验证：

| 测试                          | 目的                   |
| --------------------------- | -------------------- |
| Invalid Parcel ID           | 拒绝不存在的共享对象           |
| Invalid Offset / Size       | 拒绝越界访问               |
| Premature Reclaim           | 防止正在使用的对象被错误释放       |
| Repeated Resume Token       | 防止重复恢复或状态混乱          |
| Two Sessions Sharing Buffer | 验证并发与所有权语义           |
| Linux 修改 Shared Memory      | 验证 TOCTOU 防护         |
| Corrupted TA Binary         | 验证 TA Authentication |
| tee-supplicant 不响应          | 验证超时、阻塞与资源控制         |

Linux TEE Subsystem 将 RPC 交给 Driver 或 `tee-supplicant` 处理，并利用 Shared Memory 交换数据；这些通用的执行语义也是 RISC-V Backend 应当尽量复用的部分。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



# 21.8 SMP、Interrupt 与 Hart Lifecycle

如果前面的系统只在单 Hart 上完成了 TA Invoke，那么它仍然只是一个基本 Porting Prototype。

对于我们的 16-Hart 服务器 SoC，真正复杂的问题出现在：

多个 Hart 可以并发使用 OP-TEE，且不同 Hart 可能处于不同的 Domain、Interrupt 和 Lifecycle 状态。

## 21.8.1 先区分三个不同的并发概念

### 第一：Linux SMP

Linux 在多个 Hart 上调度普通 Task。

```
Hart 0: Linux Task A
Hart 1: Linux Task B
Hart 2: Linux Task C
...
```

Linux Scheduler 负责 Linux Task 的调度。

### 第二：OpenSBI Per-Hart Context

每个 Hart 都有自己的 M-mode 执行状态、`mscratch`、SBI Scratch、Trap Context 和可能的 Domain Context。

OpenSBI 的 Domain Context 实现按照 Domain 和 Hart 管理相应状态，不是把所有 Hart 的寄存器上下文放进一个全局的唯一 Context。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 第三：OP-TEE Trusted Thread

一个 Linux Task 发起 TEE 请求后，在 OP-TEE 中可能分配一个 Trusted Thread。

因此：

```
Linux Task != OpenSBI Domain
Linux Task != OP-TEE Trusted Thread
OpenSBI Hart Context != OP-TEE TA Context
```

这是必须分清楚的三个层次。

## 21.8.2 多 Hart 并发的执行模型

例如：

```
Hart 0:
    Linux Domain
        |
        | TEE Invoke A
        v
    OP-TEE Domain
        |
        | TA A
        v
    Linux Domain

Hart 1:
    Linux Domain
        |
        | TEE Invoke B
        v
    OP-TEE Domain
        |
        | TA B
        v
    Linux Domain

Hart 2:
    Linux Domain
        |
        | No TEE request
        v
    Linux continues running
```

三个 Hart 的执行路径可以不同。

这就要求：

- M-mode Domain Context 是正确的 per-hart 设计。
- OP-TEE Thread Pool 能正确处理并发请求。
- TA Session / Shared Data 有合适的同步。
- Memory Protection Policy 对每个 Hart 都正确。
- TLB Synchronization 考虑所有相关 Hart。
- 中断归属不会因为 Domain 切换而混乱。

## 21.8.3 Domain Switch 期间发生 Interrupt 怎么办？

这是 Staff Engineer 必须能够回答的问题。

假设 Hart 0 正在 OP-TEE Domain 中执行 TA：

```
Hart 0:
    OP-TEE Domain
       |
       | TA running
       |
       +---- UART IRQ arrives
```

UART 属于 Linux Domain。

此时系统应该如何处理？

不能简单地让 OP-TEE 直接处理所有 UART Interrupt，因为 UART 可能由 Linux 拥有。

一种 Domain-aware Interrupt 方案是：

```
UART IRQ
    |
    v
APLIC / M-mode Interrupt
    |
    v
OpenSBI IRQ Routing
    |
    | Identify target domain
    v
Linux-owned IRQ
    |
    | Save current OP-TEE context
    | Queue / route interrupt
    | Switch to Linux if necessary
    v
Linux Domain IRQ Handler
    |
    | Clear device condition
    | Complete IRQ
    v
Return to OP-TEE Domain
```

这是可能采用的路由模型，不是所有 AIA 平台的固定处理路径。

RP016 M3 已经设计了基于 M-mode IRQ、Per-Domain/Per-Hart Pending Queue 和 SEIP Notification 的 VIRQ Couriering。后续 RP022 M1 则继续推进基于 RPMI `SYSTEM_IRQ` 的 Claim 及 Yield 路径。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE+1



因此，真实 SoC 需要明确选择：

1. 是否使用 M-mode 中断接管和转发。
2. 哪些 IRQ 可以直接委托至 S-mode。
3. 哪些 IRQ 具有 Domain Ownership。
4. 当前 Domain 与 IRQ Owner 不同时如何处理。
5. 何时需要 Domain Switch，何时只需要排队。
6. Claim、Device Clear、Completion 的正确顺序。

## 21.8.4 重点：Interrupt Completion 与 Lost Interrupt

假设一个 Level-triggered IRQ：

```
Device asserts IRQ
        |
        v
OpenSBI masks HWIRQ
        |
        v
Queue VIRQ to Linux
        |
        v
Linux handles device
        |
        v
Complete VIRQ
        |
        v
OpenSBI unmasks HWIRQ
```

如果 Device Condition 没有清除，而 OpenSBI 已经重新 Unmask，那么可能立即再次触发同一个 IRQ，导致 Interrupt Storm。

如果 VIRQ 被错误地移出 Pending Queue，但 Linux 没有得到处理通知，又可能造成 Lost Interrupt。

因此应该验证的不是仅仅看到 IRQ Counter 增加，而是：

Assert → Claim → Route → Handle → Clear → Complete → Re-enable 的完整生命周期。

## 21.8.5 SMP 下的 TLB Shootdown

考虑两个 Hart：

```
Hart 0:
    TA A Page Table updated
            |
            v
    sfence.vma (local)

Hart 1:
    Still executing TA A
            |
            v
    Potential stale TLB entry
```

如果 Hart 1 仍然使用旧地址翻译，那么可能发生：

- 已回收物理页继续被访问。
- 旧 PTE Permission 继续被使用。
- 已经解除共享的内存仍然可见。
- TA Context 切换后出现错误地址翻译。

所以必须设计跨 Hart 的 Translation Synchronization。

一般过程包括：

1. 对 Page Table 执行正确更新。
2. 确定哪些 Hart 可能持有旧 Translation。
3. 通过合适的 IPI/SBI Remote Fence 等机制通知相关 Hart。
4. 各 Hart 执行必要的同步。
5. 在确认旧映射不再被使用后，才安全回收相关物理页。

具体算法要结合 OP-TEE Memory Manager 和使用的 ASID 策略分析。

## 21.8.6 Hart Lifecycle 和 Power Management

在服务器 SoC 中，Hart 不可能一直保持 Running。

需要覆盖：

```
STOPPED
   |
   | SBI HSM Hart Start
   v
START_PENDING
   |
   v
STARTED
   |
   | Suspend / Stop
   v
SUSPENDED / STOPPED
```

OP-TEE 必须参与考虑以下情况：

- Secondary Hart 首次启动。
- Linux 请求 CPU Offline。
- Hart 正在 TEE Domain 时请求 Stop。
- 某个 Trusted Thread 正在处理 RPC。
- 系统 Suspend/Resume。
- Warm Reset 或局部 Power Domain Reset。

例如：

如果 Hart 3 正在 OP-TEE Domain 执行 TA，而 Linux 希望 Offline CPU 3，能否立即停止这个 Hart？

答案通常不能是简单的「可以」。

必须先确定该 Hart 是否有未完成的 TEE 执行上下文、是否允许迁移、如何处理 Pending RPC 和 Secure Interrupt，以及停止后如何恢复。

OpenSBI HSM 状态管理、Linux CPU Hotplug 和 OP-TEE Thread 模型必须协调，才能避免停止一个仍承担安全服务的 Hart。

## 21.8.7 SMP Bring-up 建议顺序

| 阶段    | Hart 数量 | 测试重点                        |
| ----- | ------- | --------------------------- |
| SMP-1 | 1       | 基础 TEE Invoke               |
| SMP-2 | 2       | 并发调用、锁、上下文保存                |
| SMP-3 | 4       | Interrupt、RPC、Shared Memory |
| SMP-4 | 8       | TLB / SMP Stress            |
| SMP-5 | 16      | Full Hart Concurrency       |
| SMP-6 | 动态变化    | Hotplug、Suspend、Resume      |

建议在达到 16 Hart 之前，就引入自动化的随机测试：

- 随机选择 Hart 发起 TA 调用。
- 随机进行 Session Open/Close。
- 随机取消请求、触发 RPC。
- 随机制造无效 Parcel。
- 在支持的条件下进行 CPU Offline/Online。
- 检查数据完整性、锁等待时间和状态泄露。

不要把「16 个 Hart 同时启动成功」当成 SMP 验收完成的标志。

# 21.9 Secure Storage、HSM 与硬件密码学

这一节要解决的问题是：

当 OP-TEE 已经成功运行时，它的密钥、随机数、可信存储和硬件加密服务到底从哪里来？

## 21.9.1 HSM 和 OP-TEE 的职责边界

在我们的参考 SoC 中，RSS/HSM 是一个单独的物理处理器。

因此，它不应该被误认为 OpenSBI 管理下的第 17 个 Application Hart。

其与 OP-TEE 之间的关系更适合建模成：

```
OP-TEE Trusted OS
        |
        | Authenticated secure request
        v
Secure IPC / Mailbox Driver
        |
        v
RSS / HSM Firmware
        |
        +-- Key Derivation
        |
        +-- Key Import / Wrap
        |
        +-- Signing / Verification
        |
        +-- Random Number Generation
        |
        +-- Device Identity
        |
        +-- Security Lifecycle Control
```

这个架构的好处是能够将部分密钥生命周期留在独立硬件安全边界内。

但有一个前提：

OP-TEE 到 HSM 的服务请求必须具备可信的调用者身份和访问控制。

如果 HSM 只是通过 Shared Mailbox 接收命令：

```
COMMAND = READ_SECRET_KEY
KEY_ID = 0
```

却不验证请求者是谁，那么 Linux 可能直接伪造相同请求。

因此 HSM 必须能够区分：

- Linux 发起的请求。
- OP-TEE 发起的请求。
- Boot Firmware 发起的请求。
- 特定可信服务发起的请求。

这种身份区分必须依赖不可由 Linux 伪造的硬件身份、通道所有权或受保护的 Firmware 授权机制，而不能仅信任 IPC 消息中的 `caller_id` 字段。

## 21.9.2 Secure Storage 的分层密钥模型

OP-TEE 原有 Secure Storage 采用分层密钥机制。

其典型关系是：

```
Hardware Unique Key (HUK)
        |
        v
Secure Storage Key (SSK)
        |
        | TA UUID
        v
Trusted Application Storage Key (TSK)
        |
        | Wrap / protect
        v
File Encryption Key (FEK)
        |
        v
Encrypted Persistent Object
```

对于独立 HSM 的 SoC，有两种常见实现方向。

模式 A：HSM 提供受保护的密钥派生服务

OP-TEE 请求 HSM 派生一个专用于 OP-TEE Storage 的 Key Material，再按照 Storage 的内部设计派生 TA 级密钥。

模式 B：HSM 保留不可导出密钥

OP-TEE 使用 Key Handle 请求 HSM 执行加密、解密、认证或派生操作。

模式 B 可以减少原始密钥暴露到 Application CPU 内存的机会，但也可能增加 IPC 延迟，且未必能直接对接 OP-TEE 现有的所有软件 Crypto API。

两种方案的选择需要考虑性能、安全需求、API 形态和 HSM 硬件能力。

官方 Secure Storage 文档给出了 SSK、TSK、FEK 及 REE FS/RPMB 的内部设计。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 21.9.3 REE FS 和 RPMB 的安全区别

REE FS Secure Storage

加密文件存放在 Linux 管理的文件系统中，由 OP-TEE 负责相应的机密性与完整性保护。

但需要明确：

Linux 可以删除文件、拒绝服务，还可能回放一个较旧但当时有效的加密文件版本。

因此，REE FS 的加密和完整性校验不等于强硬件级 Anti-rollback。

RPMB Secure Storage

如果设备具备真实、可靠的 RPMB 机制，OP-TEE 可以通过认证写入和 Replay Protection 提供更强的回滚保护。

但服务器 SoC 未必配备 eMMC RPMB。产品可能使用其他可信计数器、RPMB 等效硬件或独立 HSM 的安全存储机制。

因此：

不要把 `CFG_RPMB_FS=y` 当成一项可以脱离硬件和设备生命周期直接打开的配置。

尤其不能在生产系统中随意启用开发用的固定 RPMB Test Key 或不安全的自动密钥烧写流程。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 21.9.4 RISC-V Hardware Crypto Acceleration

RISC-V 平台可能提供两种不同的硬件加密路径。

第一种是 Application CPU 上的 RISC-V Cryptographic ISA Extension，例如部分 Vector Crypto 扩展。

第二种是独立 Crypto Engine/HSM，通过寄存器或 IPC 提供服务。

两者实现方式不同。

```
OP-TEE Crypto API
        |
        v
Crypto Abstraction
        |
        +-- Software Backend
        |
        +-- RISC-V Vector Crypto
        |
        +-- SoC Crypto Engine
        |
        +-- HSM Secure Service
```

RISC-V OP-TEE RP022 M4 已提交以 RVV 1.0 Vector Crypto 实现选定 AES、SHA-256/512 加速接口的方案，设计上依赖编译时特性配置，目前属于公开评审工作。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



如果要为实际 SoC 增加 HSM Backend，我建议检查以下问题：

- Key 是否需要离开 HSM？
- DMA Buffer 是否位于 Secure Memory？
- 同一个 Key Handle 是否可以被 Linux 使用？
- Driver 能否在 Atomic/Interrupt Context 中调用？
- HSM 执行超时或 Reset 时如何恢复？
- 软件 Fallback 是否会意外暴露密钥？
- Cache Maintenance 和 DMA Mapping 是否正确？
- 性能是否真的比软件实现更好？

## 21.9.5 Secure Storage / HSM 测试

| Test ID | 测试                                 | 目的                  |
| ------- | ---------------------------------- | ------------------- |
| SEC-01  | 写入并重启后读取                           | 持久化正确性              |
| SEC-02  | 修改加密文件一个 Byte                      | 完整性保护               |
| SEC-03  | 回放旧版本文件                            | Rollback Protection |
| SEC-04  | TA A 访问 TA B 的对象                   | TA 隔离               |
| SEC-05  | Linux 伪造 HSM 请求                    | 请求身份认证              |
| SEC-06  | HSM Reset during request           | 故障处理                |
| SEC-07  | Crypto DMA 指向 TEE 非授权区域            | DMA 权限              |
| SEC-08  | 异常掉电时更新对象                          | Atomicity           |
| SEC-09  | Debug/Production Security State 切换 | 密钥及生命周期隔离           |

其中 SEC-03 应按照真实 Storage Backend 的保证定义结果：没有可信防回滚锚点的 REE FS 不应被误判为必须阻止一切旧版本回放。

# 21.10 系统级安全测试、性能验证与最终交付

经过前面的工作，系统已经具备以下预期能力：

```
Verified Boot
     |
OpenSBI Domain Isolation
     |
OP-TEE Core Initialization
     |
RPMI TEE Communication
     |
Trusted Application
     |
Secure Storage / HSM
     |
SMP + Interrupt Support
```

现在需要解决最后一个问题：

如何证明这是一套可交付、可维护、可复现的 SoC TEE 解决方案，而不是一次偶然成功的 Boot Demo？

## 21.10.1 定义分阶段的 Bring-up Gates

我建议采用以下八个工程里程碑。

1. G0 — Architecture Ready

   Hardware Capability Matrix、Memory Map、Security Threat Model、Boot Chain 已完成评审。

   成功标准：关键安全能力有设计证据，不存在未说明的致命硬件隔离缺口。
2. G1 — M-mode Firmware Ready

   OpenSBI 初始化、Trap、Hart、Domain Configuration 正常。

   成功标准：能够进入预期 Domain，并通过基础 SBI 测试。
3. G2 — Isolation Ready

   CPU PMP、System Firewall、Domain Switch Protection 正确。

   成功标准：非法 CPU/DMA 访问被拒绝，有独立的负向测试证据。
4. G3 — OP-TEE Core Ready

   OP-TEE Early Boot、MMU、Trusted Thread、Trap 初始化完成。

   成功标准：OP-TEE Core 能稳定进入通信服务状态。
5. G4 — REE / TEE Communication Ready

   RPMI、MPXY、TEE Backend 与最小请求响应功能正常。

   成功标准：Linux TEE Driver 能完成协议协商和基本 TEE 调用。
6. G5 — User TA Ready

   Shared Memory、RPC、TA Loading、Session、Invoke 正常。

   成功标准：基础 CA → TA → CA 调用和错误路径通过。
7. G6 — Security & SMP Ready

   16-Hart 压力测试、Memory Isolation、Interrupt、HSM、Secure Storage 测试完成。

   成功标准：并发测试满足正确性要求，关键安全测试全部通过。
8. G7 — Production Release Candidate

   Regression、Performance、Fault Injection、Firmware Packaging、Security Review 和回滚方案完成。

   成功标准：交付物具有确定版本、测试报告、残余风险和明确的维护责任人。

这些 Gate 的价值在于让各团队知道系统目前处于什么阶段。

例如：

```
G3 Passed
G4 Failed
```

说明 OP-TEE Core 已经初始化完成，但 Linux 与 OP-TEE 通信尚未贯通。

此时不应该报告整个 OP-TEE Porting 已经完成，也不应该直接让测试团队执行所有 xtest。

## 21.10.2 建立可复现的 QEMU Baseline

对于开发前期，我建议使用公开的 RISE Buildroot 环境建立一个可复现的 QEMU 基线。

下面是 RP016 M1 文档中的构建路径：

```
git clone \
  https://gitlab.com/riseproject/riscv-optee/buildroot.git \
  -b rp016_m1

cd buildroot

make qemu_riscv64_virt_optee_defconfig

make -j"$(nproc)"
```

构建完成后：

```
./output/images/start-qemu.sh
```

在对应的串口终端进入 Linux 后：

```
xtest
```

这套流程可以作为早期参考环境；M1 是特定历史里程碑，不代表它包含后来 RP016/RP022 的全部功能。正式 Chapter 21 集成实验需要进一步选择彼此兼容的 Linux、OpenSBI、OP-TEE 和 RPMI 版本。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### 为什么要保留可复现 QEMU Baseline？

假设真实 SoC 的 Palladium 环境中，某次修改导致 OP-TEE 无法启动。

如果没有 QEMU Baseline，你很难判断：

- 是通用 OP-TEE 内核修改引入的问题？
- 是 OpenSBI Domain 代码的问题？
- 是 SoC 特定 PMP/Firewall 配置的问题？
- 是 RTL 改动引入的问题？
- 还是某个构建产物没有更新？

如果同一组公共源码改动可以在 QEMU 中复现，问题就更可能位于通用软件路径。

如果 QEMU 完全正常，而硬件仿真失败，则可以重点分析平台相关的硬件假设、Firmware Integration 和 RTL 行为。

当然，这不是严格的因果证明：QEMU 和真实 SoC 的硬件模型不同，QEMU 测试通过不意味着硬件实现安全。

## 21.10.3 建议建立四级测试体系

| 测试层级                 | 测试对象                                         | 测试方式                    |
| -------------------- | -------------------------------------------- | ----------------------- |
| L1 — Unit            | Page Table、Range Check、Crypto、Domain Helper  | Unit Tests              |
| L2 — Component       | OpenSBI Context、OP-TEE Kernel、RPMI Transport | QEMU / SBI Tests        |
| L3 — Integration     | Linux ↔ OpenSBI ↔ OP-TEE ↔ TA                | xtest / Custom Tests    |
| L4 — System Security | DMA、Interrupt、Warm Reset、HSM、Fault           | RTL 仿真 / FPGA / Silicon |

四级测试的价值不是单纯增加覆盖率，而是为每类失败提供准确的责任边界。

例如，OP-TEE Secure Storage 加密算法通过了 Unit Test，并不能证明 HSM 的 IPC 认证正确，也不能证明 DMA Firewall 不存在权限漏洞。

## 21.10.4 完整 Security Test Matrix

建议形成如下测试清单：

| 分类               | 关键验证点                                |
| ---------------- | ------------------------------------ |
| Secure Boot      | 签名错误、版本回滚、修改后的 Image                 |
| Domain           | 非授权 Domain Transition                |
| PMP              | Linux CPU 读写 TEE Private PA          |
| System Isolation | 非可信 DMA 访问 TEE PA                    |
| MMU              | Illegal VA、权限错误、ASID/TLB             |
| Thread           | Suspend/Resume、Stack、FP/Vector State |
| RPC              | 超时、重复恢复、错误 Token                     |
| Shared Memory    | 越界、回收、并发、TOCTOU                      |
| TA               | 跨 TA 内存访问、非法 Syscall、Crash           |
| Interrupt        | Lost IRQ、Storm、错误 Domain Routing     |
| SMP              | 16-Hart 并发、IPI、TLB Shootdown         |
| HSM              | Caller Authentication、密钥权限           |
| Storage          | Integrity、Rollback、Atomic Write      |
| Reset            | Warm Reset、Partial Reset、Power Loss  |

其中最重要的一点是：

所有 Security Requirement 都应至少具有一个能够验证保护有效性的测试，并尽可能具有能使保护失败的故障注入测试。

例如，想证明 PMP 配置正确，不能只运行一个合法的内存访问程序。

还应该尝试在已知有合法页表映射的前提下访问受禁止的物理地址，并结合 PMP 配置和异常原因证明访问确实被硬件阻止。

## 21.10.5 性能验证应该关注什么？

对 TEE 而言，单纯测量整个 TA Invoke 的平均时间是不够的。

建议进行分层测量：

\\[ T\_{\text{invoke}} = T\_{\text{Linux driver}} + T\_{\text{transport}} + T\_{\text{domain switch}} + T\_{\text{OP-TEE}} + T\_{\text{TA}} + T\_{\text{return}} \\]

这只是解释性分解；实际异步请求、RPC 和等待时间还可能跨越多个执行阶段。

建议关注以下指标：

| 指标                        | 为什么重要                          |
| ------------------------- | ------------------------------ |
| Domain Switch Latency     | 衡量 M-mode 上下文和硬件隔离切换开销         |
| Empty TA Invoke Latency   | 测量最小 TEE 调用成本                  |
| RPC Round-trip Latency    | 反映 REE/TEE 多次切换和 Supplicant 开销 |
| Secure Storage Latency    | 评估 HSM/RPMB/REE FS 性能          |
| Concurrent TEE Throughput | 评估多 Hart 和 Thread Pool         |
| P50 / P95 / P99 Latency   | 识别长尾问题                         |
| Interrupt Latency         | 评估 TEE 执行对系统实时性的影响             |
| Context Switch Overhead   | 评估 FP/Vector 保存和恢复成本           |

例如：

```
TEE Invoke Total
    |
    +-- Linux Driver
    |
    +-- RPMI / MPXY
    |
    +-- OpenSBI Context Switch
    |
    +-- OP-TEE Trusted Thread
    |
    +-- TA Execution
    |
    +-- Return Path
```

各段测量应使用能够关联的 Trace ID，并确保时间戳位于可比较的时钟域。

对于 Palladium、ZeBu 等预硅平台，还需要特别区分：

- 功能验证时间
- 仿真器运行时间
- 模型内 CPU Cycle
- 真实 Silicon Wall-clock Latency

仿真器的宿主机运行耗时不能直接当作芯片的真实性能指标。可以用仿真统计、模型周期和时钟参数估计某些路径的开销，但应明确假设条件，并在 Silicon 上重新验证。

## 21.10.6 建议建立一份 Debug Decision Tree

真实工程中，以下定位流程非常实用：

```
System Boot Failed?
        |
        +-- No early ROM / RSS log
        |       |
        |       +-- Reset / Clock / BootROM
        |       +-- Debug or Boot Strap
        |
        +-- OpenSBI not entered
        |       |
        |       +-- Firmware authentication
        |       +-- Image placement
        |       +-- AP reset release
        |
        +-- OpenSBI entered, no OP-TEE log
        |       |
        |       +-- Domain configuration
        |       +-- Entry address / privilege
        |       +-- PMP / memory permission
        |
        +-- OP-TEE early log, then crash
        |       |
        |       +-- Stack / BSS
        |       +-- MMU / satp / PTE
        |       +-- Trap vector
        |
        +-- OP-TEE ready, Linux cannot connect
        |       |
        |       +-- Linux TEE driver
        |       +-- SBI MPXY
        |       +-- RPMI ABI mismatch
        |
        +-- TEE connection works, TA fails
        |       |
        |       +-- Shared Memory / Parcel
        |       +-- tee-supplicant
        |       +-- TA loader / signature
        |
        +-- Single Hart works, SMP fails
                |
                +-- Per-Hart Context
                +-- Interrupt Routing
                +-- Lock / Memory Barrier
                +-- TLB Synchronization
```

这个诊断树可以直接放到项目 Wiki 中，并不断补充实际遇到的 Fault Signature。

## 21.10.7 最终交付物不应该只有 Firmware Binary

我建议最终交付目录至少包含：

```
riscv-optee-port/
|
+-- docs/
|   +-- architecture.md
|   +-- hardware-capabilities.md
|   +-- threat-model.md
|   +-- memory-map.md
|   +-- boot-chain.md
|   +-- domain-policy.md
|   +-- transport-abi.md
|   +-- security-review.md
|
+-- firmware/
|   +-- manifests/
|   +-- configs/
|   +-- patches/
|
+-- dts/
|   +-- soc/
|   +-- opensbi-domain/
|
+-- scripts/
|   +-- build.sh
|   +-- package.sh
|   +-- run-qemu.sh
|   +-- collect-logs.sh
|
+-- tests/
|   +-- boot/
|   +-- domain/
|   +-- mmu/
|   +-- smp/
|   +-- rpmi/
|   +-- hsm/
|   +-- security/
|
+-- release/
    +-- image-manifest.json
    +-- dependency-lock.txt
    +-- test-results/
    +-- known-issues.md
    +-- release-notes.md
```

这只是建议的工作区组织方式，不是 OP-TEE 官方目录要求。

其中最重要的交付材料包括：

1\. Dependency Lock

准确记录每个代码仓库的 commit SHA、Toolchain、Build Configuration 和 Spec Revision。

2\. Hardware Security Configuration

包含 Memory Regions、PMP Policy、Firewall Policy、DMA Master Permission、Interrupt Ownership。

3\. Verification Evidence

保留测试日志、运行环境、设备版本和 Test ID，做到可以重复测试。

4\. Known Limitations

明确尚未实现的安全能力、已有漏洞风险和临时 Workaround。

5\. Upstream Delta

区分可以提交 upstream 的通用修改与必须留在私有 SoC BSP 中的定制实现。

对于面向社区长期维护的项目，最后这一点尤其重要。

例如：

- 通用的 RISC-V Context Switch Bugfix：优先 upstream。
- 通用 PMP/Domain Framework 改进：优先讨论 upstream。
- 新增可复用的 Test Case：优先 upstream。
- 私有 SoC 的 Firewall MMIO Base 和内部寄存器定义：视硬件公开性、维护安排和社区接受情况处理。

# 21.11 Staff Engineer 级别面试题与参考分析

这一部分将按照真实工业项目中的 Architecture Review、Technical Interview 和复杂故障分析的标准设计。

不以记忆函数名为主要考核目标，而是考察：

安全模型是否正确、是否能够跨组件推理、能否识别隐藏风险、能否制定可执行的验证方案。

## Question 01 — PMP 已经配置正确，为什么仍然发生 TEE 内存泄露？

场景：

一颗 RISC-V 服务器 SoC 已实现 OpenSBI Domain 和 PMP。Linux Domain 对 TEE PA 的 CPU 直接访问能够正确产生 Access Fault。

然而安全测试发现，Linux Root 用户可以利用一个设备驱动读取到 TEE 中的密钥内容。

要求分析攻击路径，并给出系统级解决方案。

参考分析：

首先怀疑 CPU PMP 之外的 Bus Master，包括 DMA Engine、PCIe Device、GPU、Storage Controller 或其他加速器。

CPU PMP 通常只对其适用的 Hart 访问进行约束，不自动约束所有 SoC Master。

需要确认：

1. 数据读取是否经由 DMA。
2. DMA Master 的 Transaction 是否经过 IOPMP 或其他 Firewall。
3. IOMMU 映射是否允许指向 TEE PA。
4. Firewall 的所有权和配置是否可信。
5. 是否存在地址别名或安全属性丢失。
6. 是否可能通过调试端口或其他主设备绕过保护。

修复方案应建立所有非可信 Master 到 TEE PA 的拒绝策略，并通过真实 DMA Transaction 进行验证。

考察重点： CPU 级隔离和系统级隔离的区别。

## Question 02 — Linux 和 OP-TEE 都运行在 S-mode，为什么还能相互隔离？

场景：

评审者认为，既然 Linux 和 OP-TEE 同为 S-mode，那么 Linux 一定有能力访问 OP-TEE 的全部内存。

你如何证明这一观点并不必然成立？

参考分析：

RISC-V Privilege 决定软件能够执行哪些特权操作，但内存访问还受到其他机制约束。

在 M-mode 管理的 Domain 方案中，OpenSBI 可以针对当前 Domain 配置 PMP 或平台隔离机制，使 Linux S-mode 无法访问 OP-TEE PA。

即使 Linux 自行修改 `satp` 建立指向该 PA 的映射，也必须满足物理访问权限。

因此要区分：

- S-mode Privilege
- Virtual Address Translation
- Physical Memory Protection
- Domain Policy Ownership

关键安全前提是 Linux 无法控制用于撤销隔离的 M-mode 策略或硬件控制器。

## Question 03 — Domain Switch 后 FP/Vector Register 泄露

场景：

Linux Domain 和 OP-TEE Domain 交替使用 Hart 0。运行一段时间后，Linux 用户程序偶尔能够观察到之前 TA 的部分 Vector Register 数据。

原因可能是什么？

参考分析：

要检查 Domain Context Switch 是否保存和恢复了完整的 FP/Vector Architectural State。

包括：

- F0–F31 及相关浮点状态。
- V0–V31 及 `vl`、`vtype`、`vstart`、`vcsr` 等相关状态。
- `sstatus.FS/VS`。
- 可能影响执行和信息暴露的其他扩展状态。
- 异常、中断及嵌套切换路径是否遵循相同规则。

同时应区分两层不同的切换：

1. OpenSBI Linux Domain ↔ OP-TEE Domain。
2. OP-TEE TA A ↔ TA B。

二者都可能需要保存 FP/Vector Context，但它们由不同的软件层负责。

对于机密计算场景，需要确认上下文完整性与隔离，而不能只依赖运行结果没有崩溃。RP022 M7 的实际设计也明确关注 Eager FP/Vector Context Switching。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



## Question 04 — 修改 TA Page Table 后，另一个 Hart 仍然访问旧内存

场景：

Hart 0 在 OP-TEE 中解除了一段 TA Memory Mapping，并回收物理页。

Hart 1 随后仍然能够访问旧虚拟地址，而另一个 TA 又申请到了同一个物理页。

请分析可能发生什么。

参考分析：

这是经典的 Remote TLB Synchronization 问题。

需要确认 Page Table 修改、相关 Hart 集合、ASID 语义、Remote Fence，以及物理页回收顺序。

正确的设计不应该在无法保证旧 Translation 已失效时立即把原物理页重新分配给另一个安全上下文。

必须证明：

```
Old mapping revoked
        |
        v
All relevant stale translations invalidated
        |
        v
Physical page safely reused
```

还应检查是否有其他地址空间别名、Page Table Walker 观察顺序及并发更新问题。

考察重点： `SFENCE.VMA`、SMP、Memory Ordering、Page Reclamation。

## Question 05 — RPMI TEE_CALL 在 16 Hart 并发下偶尔超时

场景：

单 Hart 上所有 xtest 均正常，但是 16 Hart 并发执行 TEE Invoke 时，会出现随机 Timeout 和 Resume Failure。

请制定排查策略。

参考分析：

优先分析：

- RPMI MPXY Channel 与 Hart/Endpoint 的绑定。
- 请求是否被 Linux CPU Migration 破坏。
- OpenSBI Per-Hart Domain Context 是否正确。
- Trusted Thread Pool 是否耗尽。
- RPC Resume Token 是否错误绑定到另一调用。
- Shared Memory Parcel 的并发生命周期。
- Interrupt Routing 是否造成 Call 无法恢复。
- Linux Driver Call Queue 和 Lock 是否存在 Deadlock。

实验上应降低并发度，从 2、4、8、16 Hart 逐步扩大，并为每次调用记录 Request ID、Hart ID、Domain ID 和 Thread ID。

不要把所有 Timeout 都解释为 Thread Pool 太小。

## Question 06 — Secure Boot 已经通过，Linux 仍能替换 OP-TEE

场景：

HSM 在 Boot 时成功验证 OP-TEE Image。

但安全团队发现，Linux 可以通过 DMA 修改 OP-TEE Core 的某个代码段。

请解释为什么 Secure Boot 不足以阻止攻击。

参考分析：

Secure Boot 解决的是启动时的软件真实性和完整性。

Runtime Isolation 解决的是运行时未经授权的访问。

因此，只有签名验证还不够，还需要：

- Image 认证期间的访问控制。
- 认证与执行之间的 TOCTOU 防护。
- 运行时 CPU 和 DMA 隔离。
- OP-TEE Core Text 的只读与执行权限。
- 安全配置的权限锁定和恢复。

应审查从 Image Verification 到 Runtime Execution 的整个时间窗口，而不仅仅是 Boot ROM 的验签函数。

## Question 07 — Level-triggered IRQ 引发 Domain Switch Storm

场景：

UART IRQ 属于 Linux，但实际触发时 Hart 正在运行 OP-TEE。

OpenSBI 能够把 IRQ 转发至 Linux，但随后出现持续的 Domain Switch，系统无法继续执行 TA。

如何定位？

参考分析：

重点确认：

1. HWIRQ 是否在路由期间正确 Mask。
2. Linux ISR 是否真正清除了 Device IRQ Condition。
3. VIRQ Completion 和硬件 EOI 的语义是否正确。
4. 重新 Unmask 是否发生在 Device Condition 清除之前。
5. 是否重复向 Pending Queue 插入同一 IRQ。
6. 是否存在多个 Hart 同时处理同一硬件源的问题。

对于 Level-triggered IRQ，设备侧的中断条件没有清除，重新使能后就可能再次触发。

应使用 IRQ Assert、Claim、Route、Complete、Unmask 的事件序列进行定位。

## Question 08 — 一个安全的 HSM Mailbox Driver 应该如何设计？

场景：

RSS/HSM 是独立核心，既要为 Bootloader 提供认证服务，也要为 OP-TEE 提供 Key Derivation。

Linux 也需要访问 HSM 中部分非敏感服务。

如何设计调用身份与授权？

参考分析：

不能仅根据 Mailbox Request 中的软件 `client_id` 进行授权。

应建立：

- 不可由 Linux 伪造的 Caller Identity。
- 硬件或可信 Firmware 控制的 Channel Ownership。
- Service-Level Access Control。
- Key Handle 的归属与生命周期。
- Request Replay、Timeout、Reset Recovery。
- Shared Buffer Protection。
- Production/Debug Lifecycle Policy。

例如，可以让不同的安全主体使用独立受控的通道或硬件身份，并由 HSM 根据可信通道上下文判定调用者。

考察重点： 如何将物理 Trust Boundary 转化成安全的软件服务接口。

## Question 09 — REE Secure Storage 已使用 AES-GCM，为什么仍无法防止 Rollback？

场景：

TA 使用 OP-TEE Secure Storage 保存一个单调增长的授权计数器。

攻击者控制 Linux 文件系统，备份旧文件，然后在计数器更新后恢复旧文件。

如果旧文件仍能通过密码学验证，系统可能发生什么？

参考分析：

AES-GCM 等认证加密能够保护数据机密性和完整性，但一个历史上合法的旧密文仍然可能具有合法认证 Tag。

如果缺乏可信的新鲜度状态，OP-TEE 无法单凭密文判断它是不是一个旧版本。

解决方向包括：

- RPMB 认证计数器。
- HSM 保护的 Monotonic Counter。
- 可信持久化的版本锚点。
- 与更新协议绑定的 Rollback Protection。

还要考虑掉电、写入原子性、计数器耗尽和设备恢复等工程问题。

## Question 10 — 为什么 OP-TEE 不应该直接访问所有 Linux RAM？

场景：

架构团队认为 OP-TEE 是可信的，因此应该允许 OP-TEE Domain 对整个 DRAM 具有读写权限，以简化 Shared Memory 实现。

你是否同意？

参考分析：

不建议将这种设计作为默认生产安全策略。

即使 OP-TEE 比 Linux 更可信，仍应遵守 Least Privilege。

允许 OP-TEE 访问全部 Linux RAM 会扩大：

- OP-TEE Bug 的影响范围。
- Shared Memory 参数验证的复杂度。
- 非预期数据访问及侧信道攻击面。
- 未来多 Domain 及多租户扩展的风险。

更理想的方案是明确 Private Memory、Authorized Shared Memory 和 Device MMIO 的边界，并在硬件与 MMU 两层实施。

但最终需要根据实际 Transport/Parcel 机制及硬件权限粒度评估。

## Question 11 — Warm Reset 后 OP-TEE 偶尔无法启动

场景：

Cold Boot 始终成功，但 Warm Reset 后 OP-TEE 有时在 `satp` 初始化附近发生异常。

请列出排查方向。

参考分析：

需要关注 Reset Domain 和状态保留语义：

- CPU 和 PMP 是否被 Reset？
- DRAM 是否 Retain？
- Page Table 是否仍有效？
- `satp`、`stvec`、`sscratch` 是否重新设置？
- Cache/TLB 和外设状态是否被正确同步？
- Secondary Hart 是否仍有旧上下文？
- Secure Memory 是否需要重新清零？
- HSM 是否已重新建立安全授权？

尤其要排除 Warm Reset 路径错误重用了已经失效的 Page Table 或 Domain Context。

正确的解决方案不是简单增加 Delay，而是建立明确的 Reset State Contract。

## Question 12 — QEMU 全部通过，Palladium 上 DMA Isolation Test 失败

场景：

QEMU 上 Linux 访问 TEE RAM 被正确拒绝，但 Palladium 上一个 DMA Engine 可以读出同一区域。

你如何组织 Firmware、RTL 和 Security 团队解决问题？

参考分析：

首先区分 QEMU 的 CPU PMP 模型与真实 RTL 中的 DMA/NoC 行为。

建议同时收集：

- DMA Master ID / Stream ID。
- 访问的 PA、方向及属性。
- IOMMU/IOPMP/Firewall 配置。
- Boot 时是否初始化了所有 Master Permission。
- Domain Switch 时是否重新配置相关安全状态。
- NoC Transaction 是否具有正确的 Security Tag。
- 访问拒绝日志是否能在硬件监控器中看到。

如果问题由硬件缺陷导致，应提出可复现的 Transaction 级测试，而不是通过 Linux Driver 中增加软件地址检查来假装修复安全边界。

考察重点： 能否跨越 ISA、Firmware、Bus Fabric 和 RTL 进行系统分析。

## Question 13 — OP-TEE Trusted Thread 数量是否应该等于 Hart 数量？

场景：

SoC 有 16 个 Hart，团队建议把 OP-TEE Trusted Thread Pool 固定配置为 16 个，认为这样可以获得最大性能。

你是否接受？

参考分析：

不能简单把 Hart 数量等同于 Trusted Thread 数量。

Thread Pool 大小需要考虑：

- 同时进入 TEE 的请求数。
- RPC Suspend 后占用的 Thread。
- Secure Memory 和 Stack 内存成本。
- TA Session Concurrency。
- 可能的阻塞服务。
- 实际瓶颈位于 CPU、锁、Transport、HSM 还是存储。

如果 16 个 Thread 都在等待一个串行 HSM Service，增加 Thread 数量可能并不能提高吞吐量，反而增加内存和锁竞争。

应该通过并发度、P99 Latency、Throughput、Thread Pool Occupancy 和 RPC Block Time 测试确定合理配置。

## Question 14 — 你如何证明一个 RISC-V OP-TEE Port 可以进入量产？

场景：

系统已经启动 Linux，执行一遍 `xtest` 没有发现错误。

项目经理希望马上发布。

作为 Staff Engineer，你还需要哪些证据？

参考分析：

我会从四个维度判断。

Architecture Evidence

硬件隔离、Boot Trust、Memory Map、Domain Policy、HSM Trust Boundary 是否经过完整评审。

Functional Evidence

Boot、TEE Invoke、RPC、Shared Memory、TA、Storage、SMP、Interrupt、Suspend/Resume 是否完成验证。

Security Evidence

非法访问、DMA、Rollback、ABI Fuzzing、TOCTOU、Fault Injection 等测试是否覆盖关键威胁模型。

Release Evidence

所有组件的 commit、Toolchain、配置、测试结果、已知风险、恢复流程及维护责任是否完整。

如果某项安全特性缺失，应明确记录产品风险并由对应的安全决策方评估，不能用一次成功的 xtest 替代安全认证和系统级验证。

# 21.12 本章总结：Staff Engineer 应该建立的系统级思维

通过 Chapter 21，我们把 RISC-V OP-TEE 的多个核心子系统串联成了一套工程设计。

Hardware Root of Trust

Boot ROM · RSS/HSM · Image Authentication

M-mode Security Foundation

OpenSBI · Domain · PMP · System Firewall

TEE Operating System

OP-TEE Core · MMU · Thread · TA Isolation

REE Integration

Linux Driver · RPMI · Shared Memory · RPC

Verification & Product Delivery

Security Tests · SMP · Performance · Regression

整套设计最重要的不是某个源码文件如何实现，而是以下五项架构原则：

1. Security First：TEE 的第一需求是可信隔离，不是启动成功。
2. Explicit Ownership：每一段内存、设备、Interrupt、Key 都必须有明确的所有者。
3. Controlled Transition：每一次跨 Domain 切换都必须维护寄存器、权限和执行状态的安全不变量。
4. Untrusted Communication：Linux、RPC、Shared Memory 提供数据和服务，但不能自动成为安全决策的可信来源。
5. Verifiable Engineering：任何安全声明都应该能够追溯到设计、实现和测试证据。

## 建议完成的毕业设计

在进入下一章之前，我最推荐完成一个可以长期迭代的练习项目：

RV64 QEMU + OpenSBI + OP-TEE + Linux + CA/TA 的端到端可验证系统。

第一阶段完成正常 Boot 和 TA Invoke；第二阶段加入 PMP/Domain 非法访问测试；第三阶段加入双 Hart、RPC、Shared Memory 和异常路径；第四阶段将通用代码与某个真实 SoC 的 Memory Map、Firewall、Interrupt 和 HSM Driver 对接。

这项练习最有价值的成果不是一个运行截图，而是一份能够回答以下问题的设计与实现记录：

> 给定一次 Linux 对 OP-TEE 的调用，究竟由谁建立信任、谁实施隔离、谁执行 Domain Switch、谁管理 TA Context，以及谁能够证明这条执行链没有越过预期的安全边界？

如果能够从 Boot ROM 一直回答到 TA，再从 TA 返回 Linux，且能深入到 OpenSBI C 代码、RISC-V 汇编、CSR、PTE 和硬件权限配置，就真正完成了从单模块 Firmware Developer 到系统级 TEE Architecture Engineer 的关键跨越。

后续源码实践建议优先以 [OpenSBI Domain Context](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com) 和 [OP-TEE OS Upstream](https://github.com/OP-TEE/optee_os?utm_source=chatgpt.com) 为起点，并持续核对公开 RISC-V RPMI TEE RFC 与具体软件集成基线。
