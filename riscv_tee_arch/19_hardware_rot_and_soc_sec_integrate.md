# Chapter 19 — Hardware Root of Trust & SoC Security Integration

RISC-V OP-TEE Architecture & Porting · 深度技术讲义 · Staff Engineer Level

本章研究的是整个 RISC-V SoC 的安全体系，而不仅仅是 OP-TEE 本身。

在前面的课程中，我们重点研究了 OP-TEE 如何启动、如何建立 Trusted Thread、如何实现 Domain Isolation、如何处理中断，以及如何与 Linux 交互。

但当我们真正将 OP-TEE 移植到一颗用于服务器或安全计算的 RISC-V SoC 时，会遇到一个更加根本的问题：

即使 OP-TEE 的代码实现完全正确，我们凭什么相信当前运行的 OP-TEE 是可信的？

这个问题会引出一系列更深层次的系统问题：

- 谁负责认证 OpenSBI、UEFI 和 OP-TEE 镜像？
- HSM 是独立的物理 Core，还是主 CPU 内部的安全执行环境？
- 设备的 Hardware Unique Key 究竟存放在哪里？
- OP-TEE 如何使用 HSM 中的密钥，却不暴露密钥本身？
- Linux 被完全攻破后，DMA、IPC、Secure Storage 是否仍然安全？
- Suspend/Resume、Warm Reset 和 Firmware Update 是否可能破坏安全隔离？

这些问题共同构成 SoC Security Architecture。

本章将以一套具体的参考设计贯穿全部内容：

> 一颗 RV64 多 Hart Application Processor，运行 OpenSBI、Linux、UEFI 和 OP-TEE；SoC 内部另有一个独立 RISC-V HSM Core，作为平台的 Root of Trust（RoT）。

这是一种教学参考架构，并非 OP-TEE upstream 强制要求的唯一硬件实现。

## 本章学习目标

完成本章后，应具备以下六项能力：

1. 能独立设计 SoC Boot Chain of Trust，并解释 Secure Boot、Measured Boot、Attestation 的区别。
2. 能明确划分 Boot ROM、HSM、OpenSBI、OP-TEE、Linux 各自的安全职责和信任边界。
3. 能为 OP-TEE 设计硬件密钥获取、密钥派生、Secure Storage 和 Crypto Acceleration 方案。
4. 能分析 DMA、MMIO、IOMMU、IOPMP、SoC Firewall 对 TEE 安全性的影响。
5. 能设计具备认证、授权、重放保护和异常恢复能力的 HSM/OP-TEE IPC。
6. 能应对真实芯片项目中的 Firmware Update、Reset、Power Management、Bring-up 和安全验证。

## 19.0 首先建立完整的 SoC Security Architecture

先从整体系统开始。

```
                 RISC-V SoC
+------------------------------------------------+
|                                                |
|  Application Processor Cluster                 |
|  +------------------------------------------+  |
|  | Hart 0 ... Hart N                        |  |
|  |                                          |  |
|  |   M-mode: OpenSBI / Security Monitor     |  |
|  |                                          |  |
|  |   S-mode: Linux Domain                   |  |
|  |   S-mode: OP-TEE Domain                  |  |
|  |   U-mode: Normal Apps / Trusted Apps     |  |
|  +------------------------------------------+  |
|                       |                        |
|                Secure IPC                      |
|                       |                        |
|  +--------------------v---------------------+  |
|  | Independent RISC-V HSM Core              |  |
|  |                                          |  |
|  | Secure Firmware                          |  |
|  | - Image Authentication                   |  |
|  | - Key Derivation / Key Management        |  |
|  | - Crypto Services                        |  |
|  | - Device Identity / Attestation          |  |
|  | - Security Lifecycle                     |  |
|  +------------------------------------------+  |
|                                                |
|  Boot ROM --- OTP / eFuse --- TRNG             |
|                                                |
|  Shared DRAM / Secure SRAM / NVM               |
|                                                |
|  Interconnect / IOMMU / IOPMP / Firewall       |
|                                                |
|  DMA / PCIe / Ethernet / Crypto Accelerator    |
+------------------------------------------------+
```

注意：图中的 Secure IPC 必须依赖真实的硬件隔离、经过设计的通信协议和可信端点身份确认，不能因为它连接了 HSM 就自动被认为安全。

### 19.0.1 五个必须区分的安全概念

| 概念                                 | 核心含义                  | 典型实现                                      |
| ---------------------------------- | --------------------- | ----------------------------------------- |
| Root of Trust（RoT）                 | 系统中最初被信任的硬件或固件能力      | Boot ROM、OTP、HSM                          |
| Chain of Trust（CoT）                | 信任通过认证关系逐级传递          | ROM 认证 Bootloader                         |
| Trusted Computing Base（TCB）        | 其失效可能破坏指定安全目标的组件集合    | HSM Firmware、Security Monitor、OP-TEE Core |
| Trusted Execution Environment（TEE） | 受保护的代码与数据执行环境         | OP-TEE                                    |
| Hardware Security Module（HSM）      | 提供密钥保护及安全操作的独立或隔离硬件实体 | 独立 RISC-V Secure Core                     |

这五个概念存在关系，但并不等价。

例如：

HSM 可以是平台 Root of Trust 的重要组成部分，但 HSM 不必运行 OP-TEE；OP-TEE 也不必承担 Secure Boot 的第一阶段认证。

更重要的是，Trust 必须相对于具体安全目标定义。

如果某设计要求“Linux 不能窃取设备身份私钥”，那么 HSM Firmware、密钥存储硬件、IPC 权限配置和相关互连保护可能属于这个目标的 TCB。

但 Linux 本身不应该属于这个 TCB。

这就是安全架构设计最重要的思想之一：

尽量缩小 TCB，把可能被攻破的组件排除在安全保证所依赖的最小集合之外。

# 19.1 Boot ROM、OTP、Secure Boot 与 OP-TEE 启动信任链

## 19.1.1 为什么只有 Secure Boot 才能建立最初的软件可信性？

假设 OP-TEE 自身具有完美的内存隔离能力。

但攻击者能够修改存放在 Flash 中的 OP-TEE Image，把代码替换成：

```
void secure_service(void)
{
    export_all_secret_keys();
}
```

然后让 SoC 启动这个修改后的镜像。

即使 PMP、MMU 和 Domain Isolation 工作完全正常，恶意 OP-TEE 仍然能够利用自己的合法安全权限窃取密钥。

原因是：

运行环境的隔离性，不能证明运行在其中的代码是经过授权的代码。

因此必须先建立 Boot Chain of Trust。

一种典型的设计是：

```
Immutable Boot ROM
       |
       | Verify HSM Firmware
       v
HSM Secure Firmware
       |
       | Verify Main Boot Firmware
       v
Main CPU Bootloader
       |
       | Verify OpenSBI
       | Verify OP-TEE
       | Verify UEFI
       v
Trusted Firmware Components
       |
       | Verify / Measure OS images
       v
Linux
```

这只是示例。在实际平台中，HSM 可以直接认证多个镜像，而不必严格按照图中的线性链条逐级认证。

例如，Boot ROM 可以验证 HSM Firmware，而 HSM 可以对 OpenSBI、OP-TEE 和 UEFI 分别执行认证；这些认证构成的是一张有明确信任关系的图，不一定是一条简单链。

### 19.1.2 Root of Trust 为什么通常需要不可变性？

我们希望第一级验证者不能被攻击者任意替换。

因此通常有两个基础元素：

- Immutable Boot Code：固化于 ROM，或受到等效硬件写保护的第一阶段代码。
- Root of Trust Public Key（ROTPK）：固化的公钥、公钥哈希，或由可信硬件保护的验证根。

验证过程可以抽象为：

\\[ H\_{\text{calculated}}=\operatorname{Hash}(\text{Image}) \\]

然后由受信任的公钥验证签名与镜像内容及受保护元数据是否对应。

如果验证成功，才允许镜像进入后续加载和执行流程。

这里需要特别注意：

Signature Verification 必须绑定镜像内容、镜像类型、目标设备/平台、版本以及必要的加载策略。

仅仅验证一个镜像的哈希并不等于完整的安全启动策略。

Trusted Firmware-M 的 Secure Boot 文档也明确要求保护第一阶段代码和 Root Public Key 的可信性。

[image](https://www.google.com/s2/favicons?domain=https://tf-m.docs.trustedfirmware.org\&sz=32)

Trusted Firmware-M Unknown documentation



## 19.1.3 Secure Boot 和 Measured Boot 不是一回事

它们经常被混淆。

| 项目      | Secure Boot           | Measured Boot        |
| ------- | --------------------- | -------------------- |
| 核心目标    | 阻止未经授权的代码执行           | 记录实际执行了什么            |
| 主要操作    | 验证签名与安全策略             | 测量镜像并保存测量结果          |
| 镜像不符合期望 | 拒绝启动、Recovery 或其他受控处理 | 记录实际状态，是否拒绝启动取决于额外策略 |
| 典型用途    | Firmware Authenticity | Attestation、审计、状态证明  |

Measured Boot 常常采用递推扩展式测量。

例如：

\\[ PCR\_{n+1}=H(PCR_n\parallel H(Image_n)) \\]

这里的 PCR 是一种受保护的测量寄存器概念，不意味着每个 RISC-V HSM 都实现了 TPM PCR。

真正重要的是：测量结果必须被可信地保存，并且攻击者不能任意删除、替换或伪造测量历史。

## 19.1.4 Secure Boot 必须考虑 Anti-Rollback

假设 OP-TEE v5 修复了一个严重安全漏洞。

攻击者虽然无法给恶意镜像重新签名，但手中有一个合法签名的 OP-TEE v3。

如果 Boot ROM/HSM 只检查签名：

```
OP-TEE v3:
    Signature = Valid
    Security Vulnerability = Present
```

那么旧镜像仍然可能通过验证。

因此必须有 Anti-Rollback Policy。

可以在受保护的 Manifest 中定义：

```
image_id        = OPTEE
security_version = 5
image_hash      = ...
signature       = ...
```

同时在受保护存储中维护：

```
minimum_security_version[OPTEE] = 5
```

验证条件之一是：

\\[ Version\_{\text{image}}\geq Version\_{\text{minimum}} \\]

其中 Version 必须是适用于安全策略的版本或单调安全计数，而不是简单比较人类可读的版本字符串。

真正难的地方在于 Firmware Update。

如果刚写入新镜像就不可逆地提高最低安全版本，但新镜像无法启动，设备可能无法回退到旧版本，造成无法恢复的故障。

因此工业级方案通常需要组合：

- A/B Firmware Slot。
- Trial Boot。
- Health Check。
- 受保护的 Update Metadata。
- 受控提交的 Anti-Rollback Counter。
- Recovery Firmware。

Anti-Rollback Counter 在何时推进，是安全性与设备可恢复性之间必须认真设计的事务边界。

[image](https://www.google.com/s2/favicons?domain=https://tf-m.docs.trustedfirmware.org\&sz=32)

Trusted Firmware-M Unknown documentation+1



## 19.1.5 HSM 验证了 OP-TEE，是否就意味着运行中的 OP-TEE 一定可信？

不一定。

这是一个非常重要的 Staff Engineer 级别问题。

假设 HSM 验证了 DRAM 中的一段 OP-TEE Image：

```
HSM:
    Verify OP-TEE image in DRAM
    -> Success

Main CPU:
    Jump to verified address
```

在验证结束与主 CPU 开始执行之间，如果 Linux、DMA 或其他 Bus Master 可以修改那段 DRAM，就会产生 TOCTOU（Time-of-Check to Time-of-Use）漏洞。

```
Time T1: HSM verifies address X
Time T2: DMA overwrites address X
Time T3: CPU executes address X
```

所以成功的镜像认证并不够。

还必须确保：

1. 镜像验证期间没有未授权写入。
2. 验证成功之后、执行之前，镜像内容不会被修改。
3. 后续运行期间，安全代码及数据的权限受到保护。
4. 域切换、复位、恢复等路径不会破坏已有的保护设置。

这意味着：

Secure Boot 与 Runtime Isolation 必须形成连续的安全保证。

从 Boot ROM 的第一条指令，到 OP-TEE 真正进入受保护执行状态，不能存在任何攻击者可以改变安全代码或安全配置的空窗期。

# 19.2 独立 RISC-V HSM Core 与主 CPU 的信任边界

这一节非常重要，因为很多 SoC Security Architecture 的错误，都来自对 HSM 的理解不准确。

## 19.2.1 HSM Core 和 OP-TEE Domain 是两种不同的隔离机制

考虑下面两种设计。

设计 A：主 CPU 内部运行 OP-TEE

同一组应用处理器 Hart 可以在不同 Domain 之间切换，隔离依赖执行上下文与硬件访问控制。

设计 B：SoC 内部另有一个独立 HSM Core

HSM 拥有独立的执行实体、固件和可能独立的受保护存储，通过 Mailbox/IPC 与主 CPU 通信。

物理独立和安全隔离之间存在区别。

即使 HSM 是独立的 CPU，如果它的 SRAM、Debug Port、Mailbox 控制寄存器和密钥寄存器都可以被 Linux 自由访问，那么它仍然不能提供可靠的安全边界。

因此必须区分：

- Physical Separation：独立的物理 Core。
- Privilege Separation：不同权限等级。
- Memory Isolation：不同实体无法访问彼此敏感内存。
- Control Isolation：非授权实体无法修改 HSM Firmware 或安全配置。
- Key Isolation：密钥材料不会因其他处理器被攻破而泄露。

真正可靠的 HSM 必须满足适用的安全目标，而不是简单地具有一个独立 Core。

## 19.2.2 推荐的 SoC 信任层级

针对独立 HSM + RISC-V 主 CPU 的设计，可以采用下面的职责分层。

| 组件                         | 核心职责                           | 是否应该保存根密钥                 |
| -------------------------- | ------------------------------ | ------------------------- |
| Boot ROM / OTP             | 建立第一阶段启动信任                     | 可以保存密钥根或公钥哈希              |
| HSM Secure Firmware        | 根密钥管理、认证、授权、生命周期控制             | 管理根密钥，优先使用硬件不可导出实现        |
| OpenSBI / Security Monitor | CPU Domain、PMP、Hart、Trap 的安全管理 | 通常不保存设备主根密钥               |
| OP-TEE Core                | TA 执行、隔离、可信服务、存储、加密 API        | 可以管理限定用途的派生密钥或 Key Handle |
| User TA                    | 实现具体可信业务                       | 尽量只获得业务所需的密钥权限            |
| Linux                      | 设备驱动、REE 服务、应用与资源调度            | 不能依赖其保守密能力                |

这里有一个重要的设计原则：

根密钥的管理权限，不应该因为 OP-TEE 需要使用某项加密服务，就自动下放给 OP-TEE。

例如 OP-TEE 想要签署设备身份证明。

不应该设计成：

```
OP-TEE:
    GET_DEVICE_PRIVATE_KEY
    -> HSM returns raw private key
    -> OP-TEE performs ECDSA
```

更好的设计是：

```
OP-TEE:
    SIGN_ATTESTATION_REPORT
           |
           v
HSM:
    Verify policy
    Select protected key handle
    Execute signing
           |
           v
    Return signature only
```

只要 HSM 内部的密钥保护是真实有效的，即使主 CPU 软件发生某些漏洞，设备根私钥仍然可以保持不可导出。

当然，这不意味着攻击者不能滥用 HSM 签名能力。

如果 HSM 无条件接受所有签名请求，攻击者仍然可以把它当成一个 Signing Oracle。因此必须有授权策略、消息结构限制和使用范围约束。

## 19.2.3 HSM 应该直接相信 OP-TEE 吗？

这里存在一个容易忽略的问题。

假设 Linux 被攻击者完全控制，它直接通过 Mailbox 向 HSM 发送：

```
CMD = DERIVE_KEY
PURPOSE = DEVICE_ATTESTATION
```

如果 HSM 只检查命令是否合法，而不验证调用者身份，那么 Linux 很可能获得原本只有 OP-TEE 才应该享有的权限。

因此 HSM 必须区分：

消息从哪里到达，以及 消息代表谁发出。

这不是同一件事。

尤其在主 CPU 上同时运行 Linux 和 OP-TEE 的系统中：

- HSM 的 Mailbox Hardware 可能只能识别来自 Application Processor 的 Bus Master ID。
- Linux 与 OP-TEE 可能共享同一个主 CPU。
- 即便硬件能标识某个 Domain，也未必能识别当前是哪一个 Trusted Application。

因此一个安全的设计至少需要两级身份：

```
Hardware / Firmware Identity
    |
    +-- Trusted OP-TEE Domain
             |
             +-- TA A
             +-- TA B
             +-- TA C
```

HSM 需要可靠确认请求确实由经过认证的 OP-TEE Core 发起，而 TA 身份应由 OP-TEE 的可信 TA Manager 确认后，以受保护的方式传给 HSM。

Linux 不能通过自己填写一个 `ta_uuid` 字段就获得 TA 权限。

## 19.2.4 三种 HSM 访问模型

| 模型                                      | 设计方式                          | 安全与工程特点                       |
| --------------------------------------- | ----------------------------- | ----------------------------- |
| Dedicated Secure Mailbox                | 只有可信 Domain 可访问 HSM 命令通道      | 访问控制较直接，但依赖互连和外设权限硬件          |
| Shared Mailbox + Authenticated Protocol | Linux、OP-TEE 共享传输资源，但请求需认证和授权 | 灵活，但需要安全会话、重放保护和可信身份建立        |
| HSM Frontend Service                    | Linux 通过 OP-TEE PTA 请求 HSM 服务 | API 易于管理，但 OP-TEE 必须正确限制请求者权限 |

对于第三种模式，调用链可以是：

```
Linux CA
    |
    | TEEC_InvokeCommand()
    v
OP-TEE
    |
    | PTA: HSM Service
    |
    | Validate caller/session
    v
HSM Driver
    |
    | Authenticated request
    v
HSM Firmware
    |
    | Check authorization
    | Execute crypto operation
    v
HSM Response
```

PTA 是 Pseudo Trusted Application，通常运行于 OP-TEE Core 侧，而不是一个独立加载的 User TA。

这种模式非常适合以 OP-TEE 作为安全服务入口的产品，但需要小心区分来自 REE 的 CA 请求和来自可信 User TA 的内部服务请求。

## 19.2.5 HSM 与 OP-TEE 谁才是最终的信任权威？

建议采用如下划分：

HSM 负责根信任和不可导出根资产，OP-TEE 负责受保护的业务执行和 TA 生命周期。

更具体地说：

- HSM 判断哪些 Firmware 可以启动，以及哪些底层密钥操作被允许。
- OpenSBI/可信 Monitor 负责维持 Main CPU 的安全执行边界。
- OP-TEE 判断哪些 TA 可以加载、哪些 TA 可以访问特定的业务服务。
- Linux 提供不需要被信任的资源和运行环境。

这不是说 OP-TEE 就没有安全决策权，而是说它的权限应当是经过明确授权的。

还有一条非常重要的区别：

如果安全目标是“Linux 被攻破也不能窃取根私钥”，那么 OP-TEE 可以是可信系统的重要组成部分。

但如果安全目标进一步提高到“即使 OP-TEE Core 被攻破，根私钥仍不能被导出”，那么必须将根私钥和相关策略保留在 HSM 的独立信任边界内。

这两种威胁模型的设计要求不同。

# 19.3 OP-TEE 如何使用 HSM 提供的密钥与加密服务

理解了 HSM 和 OP-TEE 的职责分界之后，下一步就是设计实际接口。

一个可靠的系统不应把所有安全操作都简化为 `HSM_CRYPTO()` 这样的万能接口。

## 19.3.1 首先区分三类密钥

| 类型            | 示例                                   | 最重要的要求               |
| ------------- | ------------------------------------ | -------------------- |
| Root Key      | Device Root Secret、HUK               | 极高保密性、长期稳定、不可任意导出    |
| Derived Key   | Storage Key、IPC Key、Key-Wrapping Key | 用途隔离、可管理的生命周期        |
| Ephemeral Key | Session Key、临时 ECDH 密钥               | 具有新鲜性、及时清理、不能意外跨会话复用 |

其中 Root Key 通常来自 OTP/eFuse、PUF 派生或 HSM 内部受保护的密钥槽。

需要强调：

Device ID 不是 HUK。

例如：

```
Chip ID = 0x123456789ABCDEF0
```

如果它是可读取的芯片标识，那么它可以参与密钥派生，但不应被单独当作保密根密钥。

HUK 必须具备足够的秘密熵，并且其访问路径受到保护。

## 19.3.2 为什么不应该直接用 HUK 加密文件？

假设 HUK 为 \\(K\_{\text{root}}\\)。

有人可能提出：

```
AES-Encrypt(K_root, SecureStorageData)
```

这个设计存在多个问题：

第一，所有用途都依赖同一根密钥，没有实现用途隔离。

第二，一旦某个子系统因为 API 或实现错误泄露了等效密钥材料，其他用途也可能受到影响。

第三，密钥轮换和不同安全域之间的权限管理很困难。

因此推荐采用分层派生：

```
Device Root Key (HSM protected)
          |
          v
       KDF
          |
          +-- OP-TEE Storage Root
          |
          +-- HSM IPC Authentication Key
          |
          +-- Attestation Key Seed
          |
          +-- Key Wrapping Key
          |
          +-- Other Dedicated Roots
```

例如，一个推荐的设计形式是：

\\[ K\_{\text{service}}= \operatorname{HKDF} (K\_{\text{root}},Salt,Info) \\]

其中：

```
Info =
    "SOC/OPTEE/STORAGE/v1" ||
    device_context ||
    key_domain
```

这里的字符串是设计示例，并不是 OP-TEE upstream 已经定义的 Key Derivation Label。

最重要的是 Domain Separation。

不同用途必须使用不冲突的标签，防止不同子系统意外得到相同的派生密钥。

## 19.3.3 OP-TEE upstream 的 HUK API

这里可以直接进入真实源码。

OP-TEE 提供了平台相关的接口：

```
struct tee_hw_unique_key {
    uint8_t data[HW_UNIQUE_KEY_LENGTH];
};

TEE_Result tee_otp_get_hw_unique_key(
    struct tee_hw_unique_key *hwkey);
```

源文件：

`core/include/kernel/tee_common_otp.h`

平台需要实现真实的密钥获取方式。开发环境可以使用不安全的测试实现，但生产环境不能使用固定常量替代 HUK。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



OP-TEE 同时具有一个重要的密钥派生接口：

```
TEE_Result huk_subkey_derive(
    enum huk_subkey_usage usage,
    const void *const_data,
    size_t const_data_len,
    uint8_t *subkey,
    size_t subkey_len);
```

其定义位于：

`core/include/kernel/huk_subkey.h`

当前实现中，默认的 `__huk_subkey_derive()` 使用 HUK 作为 HMAC-SHA256 密钥，将用途编号与额外上下文数据参与派生；还保留了兼容旧版 Storage/RPMB 派生行为的配置逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



因此，不能简单认为当前所有 OP-TEE 版本都始终使用同一条历史 SSK 派生公式。

### 结合真实代码理解 HUK 派生路径

以下是经过简化的执行结构：

```
/* Simplified conceptual flow */

TEE_Result derive_subkey(uint32_t usage,
                         const void *context,
                         size_t context_len,
                         uint8_t *output,
                         size_t output_len)
{
    struct tee_hw_unique_key huk = { 0 };
    TEE_Result res;

    res = tee_otp_get_hw_unique_key(&huk);
    if (res)
        return res;

    /*
     * Conceptual:
     * output = HMAC-SHA256(
     *     huk,
     *     usage || context
     * )
     *
     * Real code uses the OP-TEE Crypto API.
     */
    res = secure_kdf_hmac(huk.data,
                          sizeof(huk.data),
                          usage,
                          context,
                          context_len,
                          output,
                          output_len);

    memzero_explicit(&huk, sizeof(huk));
    return res;
}
```

这里的 `secure_kdf_hmac()` 是解释算法结构的伪函数，并非 upstream API。

实际代码可阅读：

[core/kernel/huk_subkey.c](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/huk_subkey.c?utm_source=chatgpt.com)。

注意两点：

1. 清除局部 HUK 缓冲区可以减少暴露，但不保证编译器、CPU Cache、DMA 或所有旁路通道中都不存在历史数据。
2. 即使将 HUK 读取限制在 OP-TEE Core 中，也不等价于 HUK 永远不离开 HSM。具体取决于 `tee_otp_get_hw_unique_key()` 的平台实现。

## 19.3.4 独立 HSM 平台应该如何适配这个 API？

假设 HSM 内部的 Device Root Secret 无法被主 CPU 读取。

这是一项很有价值的安全特性。

此时有两种主要设计。

方案 A：HSM 派生一个仅供 OP-TEE 使用的根子密钥

```
OP-TEE
    |
    | DERIVE_OPTEE_HUK
    v
HSM
    |
    | K_optee = KDF(K_root, "OPTEE-HUK")
    v
Return K_optee
    |
    v
OP-TEE HUK API
```

优点是更容易适配现有 OP-TEE 的 HUK 派生和 Storage 代码。

缺点是派生出的 `K_optee` 必须进入 OP-TEE 的内存。若 OP-TEE Core 被攻破，攻击者可能获得这一级密钥。

因此它保护的是 HSM 的原始 Root Key，而不能提供所有派生密钥均不可导出的保证。

方案 B：Key Handle + HSM 内部派生

```
OP-TEE
    |
    | DERIVE_KEY(
    |     parent_handle,
    |     usage,
    |     context
    | )
    v
HSM
    |
    | Derive internally
    | Store in protected key slot
    v
Return opaque key handle
```

在这个模型中，OP-TEE 不直接获得密钥字节，而是获得一个受 HSM 管理的 Key Handle。

后续可以调用：

```
AES_GCM_ENCRYPT(handle, ...)
HMAC_SIGN(handle, ...)
SIGN_REPORT(handle, ...)
```

但是这种设计不能无条件替换现有 `huk_subkey_derive()`。

因为该接口的现有调用者期望获得实际的派生密钥字节，而 Key Handle 并不是相同的数据类型。

如果选择完整的 Key-Handle 模型，需要设计新的 Crypto/Storage Provider 或适配层，并处理某些算法要求直接访问密钥字节的限制。

对具有独立 HSM 的 SoC，我更倾向于第二种模型作为长期方向，但实际选择必须结合性能、现有 OP-TEE 兼容性与平台安全目标。

# 19.4 Hardware Crypto Engine Driver

这一节讨论一个很实际的问题：

SoC 内部已经有 AES/SHA/RSA/ECC 硬件加速器，OP-TEE 应该如何使用它？

首先，需要区分两种硬件。

- Crypto Accelerator：主要提供算法计算能力，例如 AES、SHA、RSA、ECC。它可能允许软件直接加载密钥。
- HSM / Secure Element：除了加密计算，还可能管理不可导出密钥、访问权限、设备身份和安全生命周期。

某个 Crypto Engine 即使很快，也不意味着它是一个安全的 HSM。

## 19.4.1 OP-TEE Crypto API 的基本分层

实际软件调用路径可以理解为：

```
User TA
    |
    | TEE_AEEncryptFinal()
    | TEE_DigestDoFinal()
    | TEE_AsymmetricSignDigest()
    v
libutee
    |
    | Syscall
    v
OP-TEE Core
    |
    | tee_svc_cryp.c
    v
Crypto Abstraction
    |
    +-----------------------+
    |                       |
    v                       v
Software Provider      Hardware Provider
LibTomCrypt            Crypto Driver
    |                       |
    v                       v
CPU Instructions       Crypto Engine / HSM
```

官方文档中的真实入口包括：

- `lib/libutee/tee_api_operations.c`
- `core/tee/tee_svc_cryp.c`
- `core/include/crypto/crypto.h`
- `core/crypto/`
- `core/drivers/crypto/crypto_api/`

OP-TEE 提供了 Generic Cryptographic Driver 机制，便于将支持的硬件算法接入 Core Crypto Service。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 19.4.2 一个 AES-GCM 硬件操作的完整执行过程

假设 TA 请求对一段 4096 字节数据进行 AES-GCM 加密。

典型路径如下：

```
TA:
    TEE_AEEncryptFinal()
        |
        v
OP-TEE Syscall
        |
        | Validate memory and parameters
        v
Crypto Provider
        |
        | Select AES-GCM backend
        v
Hardware Crypto Driver
        |
        | Prepare DMA buffers
        | Configure algorithm and key handle
        | Load IV / AAD
        | Start crypto operation
        v
Crypto Hardware
        |
        | Execute AES-GCM
        v
Completion / Interrupt
        |
        | Read status and authentication tag
        | Synchronize output buffers
        v
Return to TA
```

这里硬件驱动远远不只是操作几个 MMIO 寄存器。

它必须处理多项安全与工程问题。

### 第一：输入参数不能因为来自 TA 就被无条件信任

例如：

```
struct crypto_request {
    uint64_t input_addr;
    uint64_t output_addr;
    uint32_t length;
};
```

如果这些地址来自 User TA，就不能直接把它们转换成 DMA 地址后交给 Crypto Engine。

需要校验：

- 调用者对内存范围是否具备合法访问权限。
- 缓冲区地址及长度是否溢出。
- 输入和输出是否允许重叠。
- DMA 地址是否确实属于授权的物理内存范围。
- 驱动是否可能访问超出 Buffer 的其他内存。
- 操作期间原 TA 内存是否可能解除映射或被重新分配。

若硬件只能使用 DMA，则还需要安全的 Buffer Mapping、Pinning 或 Bounce Buffer 策略。

### 第二：Cache Coherency

假设 Crypto Engine 不与 CPU Cache 保持硬件一致性。

CPU 已经修改输入数据，但没有清理 Cache。

```
CPU Cache: new plaintext
DRAM:     old plaintext
DMA:      reads DRAM
```

Crypto Engine 实际加密的可能是旧数据。

因此典型的非一致性 DMA 路径需要：

```
CPU writes input
       |
       v
Clean input cache as required
       |
       v
Memory / device ordering
       |
       v
Program DMA descriptors
       |
       v
Start accelerator
       |
       v
Wait for completion
       |
       v
Invalidate output cache as required
       |
       v
CPU reads result
```

具体 Cache Maintenance 和 Memory Barrier 应依赖平台 DMA 一致性模型、内存属性与驱动抽象，不能机械地在所有平台执行相同指令序列。

### 第三：AES-GCM 的 Authentication Tag 必须被正确处理

AES-GCM 不仅产生 Ciphertext，还会产生 Authentication Tag。

加密操作通常输出：

\\[ (C,T)=\operatorname{GCMEncrypt}(K,IV,AAD,P) \\]

其中：

- \\(K\\)：密钥。
- \\(IV\\)：初始化向量。
- \\(AAD\\)：附加认证数据。
- \\(P\\)：明文。
- \\(C\\)：密文。
- \\(T\\)：认证标签。

解密时必须先验证 Authentication Tag，不能将未经认证的明文作为可信结果交给调用者使用。

尤其要处理硬件可能在认证完成前就写出暂存明文的情况：这类输出必须保留在受控缓冲区，认证失败时不得对调用者可见。

### 第四：错误和超时处理

硬件不一定总能完成请求。

可能出现：

- DMA Bus Error。
- Crypto Engine Timeout。
- Hardware Reset。
- Invalid Key Handle。
- Input Buffer Access Fault。
- Authentication Failure。
- Power Domain 意外关闭。

驱动必须能够安全结束当前操作，并处理硬件内部可能残留的密钥、DMA Descriptor、缓冲区以及状态寄存器。

## 19.4.3 如果 Crypto Engine 归 HSM 管理怎么办？

如果 Crypto Engine 只能由独立 HSM Core 访问，那么主 CPU 不能直接写 Crypto MMIO。

此时架构会变成：

```
OP-TEE Crypto Service
        |
        v
HSM Crypto Backend
        |
        | Secure IPC
        v
HSM Firmware
        |
        | Validate operation
        | Resolve key handle
        | Manage crypto hardware
        v
AES / SHA / ECC Engine
```

这是将算法服务远程化的一种实现。

需要注意：原本同步运行的 Crypto API，现在可能依赖一个异步硬件请求。

例如 HSM 很忙，或 HSM IPC 需要较长时间。

在 OP-TEE 中，必须考虑：

- 当前 Trusted Thread 是否可以等待。
- 是否允许在持有 Spinlock 时调用 HSM。
- 是否允许在 Interrupt Context 中发起该请求。
- 是否可能发生 HSM 反过来等待 OP-TEE 的循环依赖。
- 超时之后是否可以安全重试。
- 如何处理操作已经完成、但应答丢失的情况。

这里有一个非常实用的原则：

不要在持有不能跨阻塞点持有的 Lock 时，执行可能阻塞的 HSM IPC。

否则容易引入跨安全域 Deadlock。

## 19.4.4 硬件加速是否一定比软件快？

不一定。

可以建立一个简单性能模型：

\\[ T\_{\text{hardware}} = T\_{\text{IPC}}+ T\_{\text{Queue}}+ T\_{\text{DMA}}+ T\_{\text{Compute}}+ T\_{\text{Complete}} \\]

而纯软件路径大致为：

\\[ T\_{\text{software}} = T\_{\text{CPU Compute}} \\]

小数据量时，IPC 和 DMA Setup 可能占据主要开销。

例如，假设某平台的典型延迟为：

| 操作                     | 示例延迟  |
| ---------------------- | ----- |
| HSM 往返与请求设置            | 35 μs |
| DMA 建立及完成              | 10 μs |
| Hardware AES 数据处理      | 3 μs  |
| 总计                     | 48 μs |
| 同样规模的 CPU Software AES | 7 μs  |

这些只是用于解释计算方法的假设值，不是某款 SoC 的实测结果。

这种情况下，Software AES 反而可能更快。

但如果算法需要使用 HSM 中不可导出的 Key Handle，软件加密可能根本不具备相同的安全能力。

因此 Crypto Backend 的选择需要同时考虑：

Performance、Key Isolation、Algorithm Support、Power、Side-Channel Resistance 和 Failure Recovery。

# 19.5 Secure Storage、RPMB 与密钥持久化

这一节将 OP-TEE 的存储体系与 HSM 的 Root Key 连接起来。

核心问题是：

Linux 文件系统不可信，为什么 OP-TEE 还可以借助它实现 Secure Storage？

## 19.5.1 OP-TEE Secure Storage 的两个典型后端

OP-TEE 官方文档主要描述两种实现：

| 后端      | 配置项             | 特点                              |
| ------- | --------------- | ------------------------------- |
| REE FS  | `CFG_REE_FS=y`  | 利用 Linux 文件系统保存加密认证后的数据         |
| RPMB FS | `CFG_RPMB_FS=y` | 利用 eMMC RPMB 的认证写入和计数器等机制提供重放保护 |

两个后端可以共存，但需要注意：在同时启用且使用通用 `TEE_STORAGE_PRIVATE` 的情况下，OP-TEE 默认优先选择 REE FS。是否具有所需的 Rollback Protection，必须按照实际选择的 Storage ID 和后端配置判断。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 19.5.2 REE FS 为什么可以保存可信数据？

首先，我们不能信任 Linux 不会修改文件。

因此 OP-TEE 要把安全能力建立在密码学上。

```
Trusted Application
        |
        | TEE_WriteObjectData()
        v
OP-TEE Storage Service
        |
        | Encrypt
        | Authenticate
        | Protect metadata
        v
Encrypted Storage Data
        |
        | RPC
        v
tee-supplicant
        |
        v
Linux File System
        |
        v
SSD / eMMC / Flash
```

Linux 可以看到密文，甚至可以删除或者修改这些文件。

但只要密钥没有泄露，且加密认证机制正确：

- Linux 无法直接读出明文。
- Linux 任意篡改数据通常会导致认证失败。
- OP-TEE 可以对文件结构进行完整性保护。

OP-TEE REE FS 采用了加密和 Hash Tree 等机制。官方文档描述了对应的 FEK、AES-GCM、Hash Tree 和原子更新模型。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



但是有一个问题：

如果 Linux 保存了一份昨天的合法密文，今天把当前文件整体替换成昨天的版本呢？

昨天的密文本身具有合法的 Authentication Tag。

因此：

加密与完整性校验不能自动阻止旧版本数据的重放。

这就是 Secure Storage Rollback 问题。

## 19.5.3 HUK → SSK → TSK → FEK 密钥层级

OP-TEE Secure Storage 使用了分层密钥设计。

```
Hardware Unique Key
        |
        | Device-bound derivation
        v
Secure Storage Key (SSK)
        |
        | TA UUID
        v
Trusted Application Storage Key (TSK)
        |
        | Protect per-file key
        v
File Encryption Key (FEK)
        |
        v
Encrypt / Authenticate File Data
```

其中：

SSK：Secure Storage Key

每设备的存储根密钥。

TSK：Trusted Application Storage Key

从 SSK 结合 TA 身份派生，用于区分不同 TA 的存储保护域。

FEK：File Encryption Key

每个存储对象使用的文件加密密钥。

其设计思想是，文件加密与设备根密钥之间存在清晰的层次，而不是所有文件都直接使用 HUK。

OP-TEE 文档给出 TSK 派生形式：

\\[ TSK=\operatorname{HMAC}\_{SHA256}(SSK,TA\\\_UUID) \\]

同时，现有 `tee_fs_key_manager.c` 使用 `huk_subkey_derive()` 获取存储派生密钥，底层要结合前面介绍的兼容配置理解。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



对你的 HSM 方案而言，这里引出一个很重要的架构决策：

HSM 是否需要管理每一个 FEK？

不一定。

一种实用方案是：

```
HSM:
    Protect Root Key
    Derive OP-TEE Storage Root

OP-TEE:
    Manage SSK/TSK/FEK hierarchy
    Perform file encryption

REE:
    Store ciphertext
```

这样可以减少每次读写 Secure Storage 都访问 HSM 的开销。

但安全边界也随之变化：OP-TEE 持有的派生密钥必须由 OP-TEE 自己保护。

另一种方案是将 Key Handle 和密钥操作保留在 HSM，但这会增加 IPC 和存储后端适配复杂度。

## 19.5.4 RPMB 为什么能够提供 Rollback Protection？

RPMB（Replay Protected Memory Block）是某些存储设备提供的特殊受保护区域。

典型 eMMC RPMB 具有：

- Authentication Key。
- Message Authentication Code。
- Write Counter。
- 受保护的读写协议。

写入时可以抽象为：

```
OP-TEE
    |
    | Request current write counter
    v
RPMB Device
    |
    | Counter = N
    v
OP-TEE
    |
    | Construct authenticated write
    | Include expected counter N
    v
RPMB Device
    |
    | Verify MAC
    | Verify counter
    | Commit write
    | Advance counter
    v
Return result
```

攻击者不能简单地重放一条过去的认证写请求来覆盖当前数据，因为计数器不再匹配。

但这里需要特别区分：

RPMB Authentication 主要保证请求认证和重放保护，不等于自动保证数据保密。

存储数据仍然需要适当的加密机制。

另外，RPMB 并不能完全消除所有可用性攻击。

例如 Linux 可以拒绝转发 RPMB 请求，或者让 Storage Device 不可用，这仍然会造成 Denial of Service。

## 19.5.5 为什么 RPMB 通过 Linux 转发仍然可能是安全的？

OP-TEE 的 RPMB 路径可以经过：

```
OP-TEE
    |
    | Authenticated RPMB request
    v
RPC / Shared Memory
    |
    v
tee-supplicant
    |
    v
Linux MMC Driver
    |
    v
eMMC RPMB
```

即便 Linux 不可信，只要：

- RPMB Key 没有泄露。
- 请求和响应都经过正确认证。
- Nonce、Counter 和设备身份得到正确处理。
- OP-TEE 不接受伪造的成功响应。

那么 Linux 可以充当不可信的 Transport。

这是安全架构里很常见的思想：

Transport 不一定可信，安全属性可以由端到端的密码学保护建立。

OP-TEE 官方文档也明确说明了通过 `tee-supplicant` 转发 RPMB 请求的实现。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 19.5.6 生产环境中 RPMB Key Provisioning 的风险

这是实际项目中非常容易出现事故的地方。

RPMB Key 往往具有一次性写入属性。若首次写入错误的密钥，后续可能无法正常使用对应 RPMB 区域。

OP-TEE 提供了：

```
CFG_RPMB_TESTKEY
CFG_RPMB_WRITE_KEY
```

但不能把开发环境中方便的配置直接用于生产。

特别是 `CFG_RPMB_WRITE_KEY=y`，官方文档警告该配置可能通过非安全侧转发路径暴露 RPMB Key，因此不应作为普通量产设备的默认安全配置。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



生产阶段需要明确：

- RPMB Key 何时生成或派生？
- 谁负责首次 Provisioning？
- 是否已经完成 Secure Boot Lifecycle Transition？
- RPMB Key 如何绑定具体 eMMC 设备？
- 更换 eMMC 后如何恢复？
- 生产失败后如何处理设备？
- 设备进入 RMA 状态后如何限制密钥访问？

还有一个细节：

如果 RPMB Key 派生过程包含 eMMC CID，那么更换存储设备可能意味着派生出的 Key 发生变化。

这就不是简单重新烧录 OP-TEE 能解决的问题。

Secure Storage 的 Key Lifecycle、设备维修策略和量产流程必须共同设计。

## 19.5.7 同时使用 REE FS 和 RPMB 的设计

一种常见的优化方向是：

```
Large Encrypted Data
       |
       v
REE File System
       |
       | Authenticated state commitment
       v
Small trusted state
       |
       v
RPMB
```

OP-TEE 文档描述了在同时启用 REE FS 和 RPMB FS 时，使用 RPMB 中的 `dirfile.db.hash` 保存 REE FS 状态哈希的机制。

该设计将高容量存储和受保护的持久状态结合起来。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



不过，要分析完整安全性还必须研究 Hash Tree、原子提交、根哈希更新以及掉电恢复逻辑，不能仅仅认为“在 RPMB 放一个 Hash 就完全安全”。

特别是跨两个存储后端进行提交时，会出现这样的事务问题：

```
Write encrypted data to REE FS
       |
       v
Power loss
       |
       X
Update trusted root in RPMB
```

系统重启后应该接受哪个状态？

是否允许回退到旧的已提交版本？

新旧数据和可信根之间如何匹配？

这些需要通过明确的 Atomic Update Protocol 解决。

# 19.6 DMA Firewall、Peripheral Ownership 与 Bus Security

这一节是整个 Chapter 19 中最重要的硬件安全问题之一。

因为很多工程师已经熟练掌握了 PMP 和 MMU，却仍然容易忽略：

CPU 无法读取某段内存，并不代表 DMA Engine 也无法读取。

## 19.6.1 PMP 为什么不能自动保护 DMA？

假设我们有：

```
Secure DRAM:
    0x80000000 - 0x80FFFFFF

Linux DRAM:
    0x90000000 - 0xBFFFFFFF
```

OpenSBI 已经配置了 PMP，让 Linux Domain 不能访问 Secure DRAM。

CPU 访问过程：

```
Linux S-mode
    |
    | Load address 0x80001000
    v
CPU MMU
    |
    v
CPU PMP Check
    |
    X Access Denied
```

看起来很安全。

但是如果 Linux 可以控制 PCIe DMA 设备：

```
Linux
    |
    | Program DMA:
    | src = 0x80001000
    | dst = 0x90001000
    v
DMA Engine
    |
    | Bus Read
    v
Secure DRAM
```

DMA 的访问请求不一定经过执行 Linux 的那个 hart 上的 PMP。

因此，即使 CPU PMP 正确配置，DMA 也可能绕过这一安全边界。

RISC-V IOPMP 的设计正是面向非 CPU Bus Requester 的物理地址访问控制。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 19.6.2 CPU PMP、IOMMU、IOPMP 有什么区别？

| 机制           | 主要保护对象                         | 核心功能                      |
| ------------ | ------------------------------ | ------------------------- |
| CPU PMP      | Hart 发出的物理内存访问                 | 对不同 Privilege 的访问进行物理权限控制 |
| CPU MMU      | CPU 虚拟地址空间                     | VA→PA、页级权限、地址空间隔离         |
| IOMMU        | DMA Device / I/O Address Space | IOVA→PA、设备上下文、DMA 映射权限    |
| IOPMP        | Bus Requester 的物理访问            | 按请求来源、物理地址及访问权限过滤         |
| SoC Firewall | 互连、内存控制器或外设访问                  | 平台特定的安全访问策略               |

RISC-V IOMMU 1.0 已经是批准的规范，支持设备上下文、地址转换和访问限制。

而截至 2026 年 11 月，IOPMP 的公开规范仓库仍明确将其标记为开发中的规范，不能把所有草案功能当成每颗 SoC 已具备的标准硬件能力。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation+1



## 19.6.3 一个完整的 DMA 安全模型

CPU 与 DMA 的不同访问控制路径

示意图：两种访问路径需要分别受到有效保护；还必须验证控制这些保护机制的配置权限。

假设 DMA 设备经过 IOMMU，Linux 能否修改 IOMMU Page Table，让 DMA 映射 Secure DRAM？

如果 IOMMU 的根配置完全由被攻破的 Linux 控制，而不存在其他强制性的安全保护，那么答案可能是可以。

因此，仅仅在 SoC 上集成 IOMMU，不代表已经建立了 OP-TEE 所需的安全 DMA 隔离。

要建立可靠边界，需要保证：

- Linux 无法通过 IOMMU 重新开放 Secure DRAM。
- Linux 无法关闭或绕过关键的 DMA 隔离机制。
- Linux 无法重新配置 Secure Device 的 Bus Requester Identity。
- DMA 设备不能伪装成其他安全 Master。
- 保护规则不仅存在于软件检查中，还能够由硬件实际强制执行。

这就是 Control Plane Protection 的问题。

## 19.6.4 IOPMP 如何区分不同 DMA Requester？

IOPMP 草案采用 Requester Role ID（RRID）等概念来区分请求来源，再结合 Memory Domain 和访问权限表决定是否允许某次访问。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



例如，假设有：

```
Requester ID 1: PCIe Device
Requester ID 2: Ethernet DMA
Requester ID 3: HSM DMA
```

我们可以制定安全策略：

| Requester              | Linux DRAM    | OP-TEE DRAM | HSM SRAM  |
| ---------------------- | ------------- | ----------- | --------- |
| PCIe Device            | 授权映射范围内允许     | 禁止          | 禁止        |
| Ethernet DMA           | 授权映射范围内允许     | 禁止          | 禁止        |
| HSM DMA                | 按指定 Buffer 允许 | 按安全策略允许     | 按安全策略允许   |
| Main CPU Linux Domain  | 允许            | 禁止          | 禁止        |
| Main CPU OP-TEE Domain | 按需允许          | 允许          | 禁止直接访问根秘密 |

这个表是我们希望系统实现的最终安全策略，具体由 PMP、IOMMU、IOPMP、Firewall 等机制分别落实，而不是只由 IOPMP 实现所有规则。

尤其要注意：

Requester ID 必须来自可信的硬件标识机制，不能仅仅相信软件传递的字段。

如果 Linux 可以任意伪造 HSM 的 Requester ID，那么基于这个 ID 的权限控制就失去了安全意义。

## 19.6.5 Peripheral Ownership 不能只靠 Device Tree

假设 Device Tree 中包含：

```
crypto@10000000 {
    compatible = "vendor,secure-crypto";
    reg = <0x0 0x10000000 0x0 0x10000>;
    status = "disabled";
};
```

这可能告诉 Linux 不要加载某个驱动，但不是硬件安全保护。

被攻破的 Linux 可以忽略 Device Tree，直接访问 MMIO。

因此：

Device Tree 描述资源，硬件访问控制执行安全策略。

对于 Secure Peripheral，需要确定：

- 哪个 Domain 拥有设备。
- 谁可以配置设备 MMIO。
- 谁能配置它的 DMA。
- 谁能接收设备 Interrupt。
- 谁能执行 Reset 或 Power-Gate。
- 谁可以修改设备的 Security Configuration。

还要考虑是否存在设备的多个 Bus Master、多个 Register Window、Debug Interface 或旁路访问通道。

## 19.6.6 实际 SoC Bring-up 时必须做的负向测试

除了证明 OP-TEE 能正常访问 Secure RAM，还应该验证 Linux 无法越权访问。

| 测试                               | 预期结果            |
| -------------------------------- | --------------- |
| Linux CPU 读取 OP-TEE Private RAM  | 拒绝访问或发生预期 Fault |
| Linux CPU 写 OP-TEE Code          | 不允许修改           |
| Linux DMA 读取 OP-TEE Private RAM  | 被硬件拒绝           |
| Linux 写 HSM Private SRAM         | 被硬件拒绝           |
| Linux 修改 Security Firewall 配置    | 被拒绝             |
| Linux 关闭 Secure Peripheral Clock | 被拒绝，或只能经过授权路径   |
| Linux 访问 Secure Debug Registers  | 被拒绝             |
| Linux 使用合法 Shared Memory         | 在规定权限内成功        |

这些测试应该在具备相应安全硬件模型的仿真环境中执行。仅凭 QEMU 上的 CPU PMP 测试，无法证明实际 SoC 的 DMA 和互连隔离符合要求。

# 19.7 Device Identity、密钥 Provisioning 与 Attestation

在前面的章节中，我们已经建立了 HUK、SSK 和硬件密钥的概念。

接下来要解决另一个问题：

设备身份从哪里来？其他系统如何确认它确实是一台可信设备？

## 19.7.1 Device Identity 不等于 Hardware Unique Key

假设芯片出厂时具有：

```
Device Serial Number = SN00012345
Hardware Unique Key  = K_root
Attestation Key       = K_attest
Device Certificate    = Cert_device
```

这四个东西的职责不同。

| 资产                      | 主要作用             | 是否必须保密  |
| ----------------------- | ---------------- | ------- |
| Serial Number           | 标识一台设备           | 通常不要求保密 |
| HUK / Root Secret       | 密钥派生、硬件信任基础      | 必须严格保护  |
| Attestation Private Key | 对可信状态证明签名        | 必须严格保护  |
| Device Certificate      | 将公钥及身份声明绑定到可信发行方 | 通常可以公开  |

因此，不能因为一台设备具有不可修改的 Chip ID，就认为它具有密码学意义上的 Device Identity。

可靠的身份通常需要设备对某个受保护私钥具有独占控制权，并有可验证的身份绑定机制。

## 19.7.2 Device Provisioning 的典型生命周期

一个工业级流程可以分为：

```
Silicon Manufacturing
        |
        v
Provision Root Key / PUF Helper Data
        |
        v
Provision Boot Verification Root
        |
        v
Install or Generate Device Identity
        |
        v
Configure Lifecycle State
        |
        v
Lock Security Configuration
        |
        v
Production Device
```

注意，不是所有芯片都需要将原始对称根密钥直接写入 eFuse。

有的设计通过硬件 TRNG 生成并封装，有的通过 PUF 重建，也有的平台使用 HSM 内部的不可导出密钥槽。

关键要求是：根密钥的创建、读取、写入及生命周期转换必须有明确的安全策略。

## 19.7.3 生命周期状态对安全策略的影响

可以采用如下状态机作为设计参考：

```
UNPROVISIONED
     |
     v
MANUFACTURING
     |
     | Program keys / Verify
     v
PROVISIONED
     |
     | Lock security configuration
     v
PRODUCTION
     |
     +------> RMA / SERVICE
     |
     +------> DECOMMISSIONED
```

这里的状态名是示例，真实硬件生命周期可能包含更多状态或不同转换约束。

通常需要做到：

Manufacturing State

允许受控的 Key Provisioning、诊断和安全配置操作。

Production State

限制 Debug Access，禁止读取 Root Key，禁止未经授权的 Firmware，限制或锁定关键安全配置。

RMA State

只开放经过授权的诊断和恢复能力。对高价值密钥，往往需要先销毁或失效相关密钥，才能进入具有更大调试权限的状态。

Decommissioned State

确保设备身份及业务密钥无法再用于生产环境。

生命周期控制不能只依赖一个由 Linux 保存的配置文件。关键状态需要由可信硬件或同等保护的持久状态实施。

## 19.7.4 Attestation 是如何建立的？

Attestation 的目的不是简单地说“这台设备运行了 OP-TEE”。

而是提供一个可以被外部 Verifier 验证的声明：

> 我是某个可信身份的设备，并且我正在运行满足特定安全策略的 Firmware/Software 状态。

可以使用如下结构：

```
Verifier
    |
    | Request attestation
    | nonce = random challenge
    v
Application / OP-TEE
    |
    | Request report
    v
HSM
    |
    | Retrieve trusted measurements
    | Include nonce
    | Include device/TCB claims
    | Sign with protected key
    v
Attestation Report
    |
    v
Verifier
    |
    | Verify certificate chain
    | Verify signature
    | Verify nonce
    | Verify measurements/policy
    v
Trust decision
```

报告中可以包含：

```
device_identity
boot_measurements
opensbi_measurement
optee_measurement
security_lifecycle
security_version
nonce
signature
```

这些字段只是一个参考设计，不代表 OP-TEE 当前默认提供这样一个统一的 Attestation Report ABI。

## 19.7.5 HSM 如何知道 OP-TEE 的 Measurement？

这是架构设计里最有价值的问题之一。

如果 HSM 直接相信 Linux 提交的：

```
OPTEE_MEASUREMENT = trusted_hash
```

那么恶意 Linux 可以填写任何希望 HSM 签署的值。

所以可信 Measurement 必须由受信任的测量路径产生，并且通过受保护的方式传递给 HSM。

一种方案是：

```
HSM authenticates OP-TEE image
         |
         v
HSM records actual measurement
         |
         v
OP-TEE boots under isolation
         |
         v
OP-TEE requests attestation
         |
         v
HSM signs its own recorded measurement
```

更复杂的方案还可以包括运行时动态测量和 TA Measurements，但这要求可信的测量更新机制。

即使有 HSM，也不能让 Linux 随意修改可信的 Measurement Log。

另外，必须区分：

- 证明已加载了什么。
- 证明加载后未被修改。
- 证明某个特定 TA 正在执行。
- 证明系统当前没有被攻破。

这四个安全声明的强度不同。

一个只有 Boot Measurement 的系统通常不能自动证明运行期间没有发生漏洞利用。

## 19.7.6 密钥与 Firmware 版本是否应该绑定？

假设将 Secure Storage Key 派生为：

\\[ K\_{\text{storage}}=\operatorname{KDF}(K\_{\text{root}},H(OPTEE\ Image)) \\]

这样做看起来更安全，因为不同 OP-TEE 版本使用不同密钥。

但问题是：

每次升级 OP-TEE，Image Hash 都会改变。

因此：

```
OP-TEE v1:
    K_storage_v1

OP-TEE v2:
    K_storage_v2
```

升级之后，OP-TEE v2 可能无法读取原先用 v1 密钥加密的数据。

所以需要区分：

短期的 Session Key / Attestation Context 可以绑定当前 Firmware Measurement。

长期的 Persistent Storage Root 通常需要在符合安全策略的升级之间保持稳定，或者设计显式、安全、可恢复的密钥迁移协议。

这也是为什么 Key Lifecycle 不能只用一个 KDF 公式就解决。

# 19.8 SoC Reset、Power Domain 与安全状态保持

很多平台的 OP-TEE 移植在 Cold Boot 下运行正常，但一旦加入 Suspend/Resume、CPU Hotplug 或 HSM Reset，就会出现非常复杂的问题。

原因是：

安全状态不仅存在于内存中，还存在于 PMP、IOMMU、IOPMP、Security Firewall、Crypto Engine、Mailbox 和其他硬件寄存器中。

## 19.8.1 首先理解 Reset Domain

大型 SoC 通常具有多种 Reset：

| Reset 类型                  | 可能影响                            |
| ------------------------- | ------------------------------- |
| Hart-local Reset          | 只有一个主 CPU Hart 重新初始化            |
| Application Cluster Reset | 主 CPU Cluster 状态重置              |
| HSM Reset                 | HSM Firmware、会话和内部 SRAM 可能丢失    |
| Peripheral Reset          | 某个 DMA/Crypto/IO Controller 被重置 |
| Warm System Reset         | 部分状态保留，部分重新初始化                  |
| Cold / Power-on Reset     | 更大范围的硬件重新初始化                    |

不同 SoC 的 Reset Tree 可能完全不同。

不能直接假设：

```
Main CPU Reset == HSM Reset
```

也不能假设：

```
Warm Reset == All security registers reset
```

这些行为必须由 RTL、SoC Integration Specification 和实际测试确定。

## 19.8.2 最危险的情形：安全配置丢失，但 DRAM 仍然保留秘密

假设：

```
Before Suspend:
    OP-TEE Private DRAM protected
    Firewall enabled

After Partial Reset:
    OP-TEE Private DRAM retained
    Firewall reset to permissive state
```

如果 Linux 在安全 Firewall 重新建立之前获得执行机会，就可能读取 OP-TEE Private DRAM。

因此恢复流程必须保证：

```
Resume / Reset Entry
        |
        v
Hold untrusted masters
        |
        v
Establish safe security defaults
        |
        v
Restore PMP / IOPMP / Firewall
        |
        v
Validate retained secure state
        |
        v
Restore OP-TEE execution
        |
        v
Release Linux / Devices
```

如果硬件在 Reset 后默认允许非可信访问，而软件只能在 Linux 已经运行之后才重新配置 Firewall，那么这个方案存在严重的安全空窗期。

要么硬件提供 Fail-Closed Default，要么在解除复位前由可信控制器先恢复安全配置。

正确的硬件复位默认状态，对 Secure Boot 和 Runtime Isolation 同样重要。

## 19.8.3 Suspend-to-RAM 时密钥怎么办？

Suspend-to-RAM 可能保留部分 DRAM，而部分 CPU 和外设掉电。

需要明确不同资产的安全策略：

| 安全资产                 | 可选处理策略                |
| -------------------- | --------------------- |
| HSM Root Key         | 保留在始终受保护的硬件中，或恢复时重新建立 |
| IPC Session Key      | 失效并重新协商               |
| TA 临时密钥              | 按 Suspend 威胁模型销毁或安全保留 |
| Secure Storage Root  | 从稳定根密钥重新派生            |
| DMA Descriptor       | 终止、排空或验证后重建           |
| Key Handle           | 与 HSM Reset Epoch 绑定  |
| Trusted Thread State | 仅在保持完整性与隔离的条件下恢复      |

需要考虑的并不只是 Confidentiality。

如果系统恢复时使用了一份被篡改的 Trusted Thread Context，那么即使所有密钥没有直接泄露，也可能造成控制流劫持。

所以 Retained Secure Memory 同样需要完整性保证，并且必须在恢复前重新验证相应保护机制是否有效。

## 19.8.4 HSM Reset 与主 CPU Reset 不同步

假设 OP-TEE 正在等待 HSM 的一次 AES-GCM 请求。

```
OP-TEE             HSM
   |                 |
   | AES_GCM_REQ     |
   |---------------->|
   |                 |
   |             Processing
   |                 |
   |             HSM Reset
   |                 X
   |
   | Timeout
```

现在出现几个问题：

- OP-TEE 怎么知道 HSM 已经 Reset？
- 旧的 Key Handle 是否还能使用？
- 请求是否已经完成部分数据处理？
- 原来的 IPC Session Key 是否仍然有效？
- 如果重试，会不会执行两次？
- OP-TEE 是否可以在不重启 Linux 的情况下恢复服务？

建议引入 HSM Boot Epoch：

```
HSM Boot Epoch = E
```

每次 HSM Reset 后，创建新的可验证会话状态。

例如：

```
Old handle:
    epoch = 100
    key_id = 42

After HSM Reset:
    epoch = 101
```

旧的 Handle 必须被拒绝或经过明确的恢复机制重新建立。

Epoch 本身不一定需要是跨所有设备冷启动都持久单调的计数，但必须保证在相关会话安全模型下不能接受过期凭据。通常需要结合随机会话 Nonce、硬件身份认证和密钥重新协商。

如果采用可持久的 Key Handle，还需要区分 Persistent Object ID 和当前 Boot Session Handle，不能混为一谈。

## 19.8.5 CPU Hotplug 和多 Hart 的特殊问题

假设 Hart 0 正在 OP-TEE Domain 执行，而 Hart 3 被 Linux 请求 Offline。

如果 Hart 3 曾经在 OP-TEE 中执行过，可能存在：

- 尚未清理的 GPR/FP/Vector 状态。
- TLB 中的旧地址转换。
- Per-Hart Security State。
- Pending Secure Interrupt。
- 尚未完成的 DMA 或 Crypto Request。
- Per-Hart Trusted Thread Stack。

因此 CPU Hotplug 不只是执行 `SBI_HSM_HART_STOP`。

还必须验证在 Hart Stop/Start 期间，安全状态不会丢失、残留或泄露。

一些重要的设计问题包括：

1. 哪个组件负责清理 Hart-local 敏感上下文？
2. 被 Offline 的 Hart 是否可能在恢复时继承旧的 Domain？
3. 新启动的 Hart 在进入 Linux 前是否已经配置了正确的 PMP？
4. Pending Interrupt 如何处理？
5. OP-TEE 是否允许 Hart 在持有 Trusted Lock 时停止？

这些要求应结合 OpenSBI Hart 生命周期、Domain Context 管理和 OP-TEE SMP 设计共同实现。

## 19.8.6 Reset/Power 的安全验收标准

不能仅仅以“Resume 后 Linux 继续运行”为成功标准。

还需要验证：

- 恢复过程没有未受保护的访问窗口。
- 所有保留的 Secure State 均具有有效的隔离与完整性保证。
- 旧 Session Key、Key Handle 和 Request ID 不被错误复用。
- 未完成的 Crypto/DMA 操作被正确中止或恢复。
- HSM Reset 不导致根密钥泄露。
- 异常恢复路径不能绕过 Secure Boot 和 Lifecycle Policy。

# 19.9 HSM / OpenSBI / OP-TEE 之间的 IPC 安全

这是本章最接近真实 Firmware Development 的内容之一。

我们需要解决三个不同的问题：

1. Linux 如何进入 OP-TEE？
2. OP-TEE 如何请求独立 HSM 提供安全服务？
3. HSM 如何确认这些请求是可信且经过授权的？

它们可能共用部分底层通信机制，但不能被视为同一条逻辑安全通道。

## 19.9.1 首先区分两条通信链

链路 A：Linux ↔ OP-TEE

Linux / REE Domain

SBI MPXY / RPMI TEE

OpenSBI / Domain Manager

OP-TEE Domain

链路 B：OP-TEE ↔ Independent HSM

OP-TEE Crypto / Key Service

Protected Mailbox / Secure IPC

HSM Secure Firmware

第一条链用于进入 OP-TEE 及管理 Domain Execution。

第二条链用于 HSM 的独立安全服务。

RISC-V RPMI 2.0 定义了 TEE Service Group，Linux RISC-V OP-TEE 的相关 RFC 也采用 SBI MPXY 承载 TEE_CALL。但这不意味着 HSM 内部的 Key Management、Crypto Service 或 Mailbox Protocol 自动采用相同接口。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



尤其需要注意：独立 HSM Core 的 M-mode 与主 CPU 的 M-mode 是不同物理执行实体上的特权模式。

主 CPU 执行 `ecall` 进入本 Hart 的 M-mode，并不会因此自动进入独立 HSM Core。

访问 HSM 仍需要跨处理器通信机制。

## 19.9.2 HSM IPC 的基本执行过程

假设 OP-TEE 想让 HSM 使用一个不可导出的 Key Handle 对消息生成 HMAC。

可以设计如下流程：

```
OP-TEE                         HSM
   |                             |
   | HSM_HMAC_REQUEST            |
   |---------------------------->|
   |                             |
   |                      Validate message
   |                      Authenticate caller
   |                      Check key policy
   |                      Resolve key handle
   |                      Execute HMAC
   |                             |
   | HSM_HMAC_RESPONSE           |
   |<----------------------------|
   |                             |
   | Validate response           |
   | Return to TA                |
```

看起来很简单。

但要构建可靠的安全协议，必须回答以下问题：

- 谁能发送 Request？
- 谁有权使用指定的 Key Handle？
- Linux 能否伪造同样的 Request？
- 旧的 Request 能否被 Replay？
- Shared Memory 会不会被修改？
- HSM Reset 后，旧的 Session 是否有效？
- 如果执行成功但 Response 丢失，该如何处理？
- 多个 Hart 同时访问时，谁管理 Queue 和 Lock？

下面逐项展开。

## 19.9.3 建议的 IPC Message Format

可以定义一个教学用的消息结构：

```
struct hsm_msg_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;

    uint32_t command;
    uint32_t flags;

    uint64_t request_id;
    uint64_t session_id;
    uint64_t sequence;

    uint32_t payload_len;
    uint32_t status;
};
```

实际协议还需要定义：

```
Message Header
    |
    +-- Protocol Version
    +-- Command Type
    +-- Request ID
    +-- Session Identifier
    +-- Sequence Number
    +-- Payload Length
    +-- Authenticated Metadata
    |
    +-- Payload
    |
    +-- MAC / AEAD Authentication Tag
```

以上是推荐设计的示例，不是 OP-TEE 或 RPMI 已有的标准 HSM IPC 结构。

这里特别强调三个字段。

Request ID

用于关联请求与应答，尤其是异步调用。

Session ID

用于标识一个已经建立的可信通信会话。

Sequence Number

用于检测消息重放和乱序，具体规则需要结合协议是否允许并发请求。

一个正确的实现还应处理整数溢出：

```
if (payload_len > MAX_PAYLOAD_SIZE)
    return ERROR_INVALID_LENGTH;
```

如果消息中包含 Offset：

```
if (offset > buffer_size ||
    payload_len > buffer_size - offset)
    return ERROR_INVALID_RANGE;
```

第二种写法避免直接计算 `offset + payload_len` 可能发生的整数溢出。

## 19.9.4 如何建立可信的 IPC Session？

如果硬件已经提供真正可信的 Domain Identity，并且独占地保护了 HSM Mailbox，那么可能不需要对每条消息再额外执行软件 MAC。

但在共享通道、跨不可信 Transport 或威胁模型要求更高的情况下，可以建立认证会话。

概念流程：

```
OP-TEE                         HSM
   |                             |
   | ClientHello                 |
   | Nonce_OPTEE                 |
   |---------------------------->|
   |                             |
   |                   Verify caller context
   |                   Generate Nonce_HSM
   |                             |
   | ServerHello                 |
   | Nonce_HSM                   |
   | Authenticated identity      |
   |<----------------------------|
   |                             |
   | Verify HSM identity         |
   | Derive session keys         |
   |                             |
   | Authenticated request       |
   |---------------------------->|
```

一个会话密钥的概念性派生可以写作：

\\[ K\_{\text{session}}= \operatorname{HKDF} (K\_{\text{shared}},Salt,Context) \\]

其中 Context 应当包含必要的角色身份、协议版本、双方 Nonce 和用途。

但这里有一个核心前提：

双方必须事先拥有可靠的信任锚或经过验证的密钥协商能力。

如果 Linux 能伪造 HSM Identity、替换公钥和会话协商消息，而 OP-TEE 又没有验证机制，那么仅仅使用 ECDH 并不能防止中间人攻击。

建议分别派生 Request Authentication Key 和 Response Authentication Key，以避免双向消息共用同一安全上下文导致协议混淆。

## 19.9.5 为什么 MAC 还不够？

假设攻击者录制了一条合法 HSM Request：

```
CMD = SIGN
KEY = K_attest
MESSAGE = ...
MAC = Valid
```

之后原样重发。

MAC 仍然正确。

这说明 MAC 可以帮助验证消息完整性与密钥持有情况，但不能单独提供 Freshness。

因此需要结合：

- Sequence Number。
- Session Nonce。
- Replay Window。
- Session Expiration。
- Reset Epoch。
- 必要时的受保护持久 Counter。

对于跨重启重放保护，不能仅在每次重启后把 Sequence Number 重置为零，然后复用原来的 Session Key。

正确方案通常是每次重新建立独立的、不可预测且经过认证的 Session Key，或者使用受保护的持久单调状态。

## 19.9.6 HSM 如何授权来自不同 TA 的请求？

假设有两个 TA：

```
TA A: Payment Service
TA B: Device Attestation
```

TA A 可以调用某个支付业务 Key，但不应该使用设备的 Attestation Private Key。

因此授权策略应至少包含：

```
Caller Security Domain
Caller TA Identity
Requested Operation
Key Handle
Key Policy
Security Lifecycle
Session State
```

关键是：这些身份与策略不能完全由请求者自己填写。

可以采用这样的责任划分：

```
OP-TEE TA Manager
    |
    | Authenticates TA identity
    v
Trusted Service / PTA
    |
    | Constructs trusted caller context
    v
HSM Security Service
    |
    | Verifies protected request origin
    | Enforces key policy
    v
Authorized Operation
```

HSM 接收到经过可信通道传来的 TA 身份后，仍然需要判断该身份是否被允许执行具体操作。

对于极高安全等级的用途，甚至可以避免 HSM 对任意输入进行签名，而要求 HSM 自己构造或验证受限格式的声明。

例如 Attestation Service 应签署经过授权、来源可信的 Measurement Claims，而不是随意对 Linux 提交的字节执行设备身份私钥签名。

## 19.9.7 IPC 与 Shared Memory 的 TOCTOU 问题

假设 OP-TEE 把一个 Request Descriptor 放在共享内存中：

```
Command = HMAC
Address = 0x90001000
Length = 128
```

HSM 读取 Header 之后，Linux 修改该共享内存中的 Address 或 Length。

如果 HSM 后续使用被修改后的数据，就可能发生 TOCTOU。

对于可被非可信实体修改的 Buffer，需要采取适当机制：

- 在可信内存中复制并验证小型 Descriptor。
- 对消息内容进行端到端认证。
- 使用经过验证的 Buffer Mapping。
- 在操作期间固定相关映射与所有权。
- 防止检查前后由不同安全主体修改关键字段。

如果使用 Zero-Copy DMA，则必须设计清楚谁拥有 Buffer、什么时候完成所有权转移、何时允许解除映射。

不能简单以性能为由省略这些保护。

## 19.9.8 多 Hart 下的 IPC 并发

假设 16 个 Application Hart 可以同时访问 OP-TEE。

其中 8 个 Trusted Thread 同时向 HSM 发出请求。

那么需要明确：

```
Request Queue Depth
Maximum In-Flight Requests
Request Ownership
Per-Session Ordering
Key Handle Locking
Completion Notification
Cancellation
Timeout
Backpressure
```

例如同一个 Key Handle 是否允许并发使用？

对 HMAC 一次性操作，可能可以并发。

但对某些有内部状态的 Streaming Hash、Key Rotation、Counter Update 操作，并发顺序可能决定结果是否正确。

建议把 HSM API 分成三类：

| 操作类别       | 示例                                         | 并发与重试要求      |
| ---------- | ------------------------------------------ | ------------ |
| 无状态或可重算    | Hash、部分 Verification                       | 通常较容易并行和重试   |
| 有状态但可限定作用域 | Streaming Crypto Session                   | 需要会话与状态隔离    |
| 有持久副作用     | Provision Key、Increment Counter、Delete Key | 必须考虑幂等性和事务恢复 |

例如，执行：

```
INCREMENT_MONOTONIC_COUNTER
```

如果 HSM 已经成功增加计数器，但 Response 丢失，OP-TEE 再次提交同一个请求可能会导致计数器增加两次。

所以有持久副作用的操作应该设计可查询的结果、事务标识或受保护的去重状态。

这是跨处理器 RPC 中非常典型的分布式系统问题。

## 19.9.9 HSM IPC 的安全不变量

最后，可以用几个不变量检验协议设计。

Invariant 1 — Origin

非授权 Domain 无法成功冒充已授权的安全端点。

Invariant 2 — Authorization

一个有效请求也只能执行其权限允许的操作。

Invariant 3 — Freshness

旧会话或旧请求不能被重新使用以获得新的授权效果。

Invariant 4 — Integrity

Command、Key Handle、Length、Buffer Mapping 和安全关键参数不会在验证后被非授权修改。

Invariant 5 — Key Isolation

受保护的根密钥不会因普通 IPC 调用而离开预定的安全边界。

Invariant 6 — Recoverability

超时、重置和部分执行不能让安全状态进入不一致、不可解释的状态。

Invariant 7 — Availability Boundary

恶意 Linux 可以造成一定程度的 DoS，但不能通过资源耗尽使 HSM 绕过安全检查或破坏其他可信服务。

这七个不变量比单纯定义几十个 IPC Command 更重要。

# 19.10 从 Firmware Porting 到完整 SoC Security Architecture

前面讲述了多个子系统。

现在我们把它们全部连接起来。

## 19.10.1 参考项目：16-Hart RISC-V Server SoC

假设目标平台具有以下硬件结构。

| 硬件资源               | 参考配置                                    |
| ------------------ | --------------------------------------- |
| Application CPU    | 16 个 RV64 Hart                          |
| Trusted Monitor    | OpenSBI，运行于主 CPU M-mode                 |
| REE                | Linux，运行于 S-mode                        |
| TEE                | OP-TEE，运行于隔离的 S-mode Domain             |
| Boot Firmware      | UEFI                                    |
| Security Processor | 独立 RISC-V HSM Core                      |
| Security Root      | Boot ROM + OTP + HSM Firmware           |
| 内存                 | DDR + HSM Private SRAM                  |
| 外设                 | PCIe、Ethernet、Crypto Engine、eMMC        |
| 安全硬件               | PMP + 平台隔离机制 + IOMMU/IOPMP 或等效 Firewall |

这是参考系统，而不是对某款真实芯片实现的断言。

我们希望建立的安全目标是：

> 即使 Linux 内核被完全攻破，攻击者也不能读取 HSM Root Key、OP-TEE 私有内存或未授权的 TA Secret，也不能任意修改已经建立的安全配置。

同时允许某些不可避免的可用性限制，例如非可信 Linux 拒绝提供存储服务可能导致部分安全业务无法完成。

## 19.10.2 推荐的整体启动顺序

一种合理的参考设计是：

```
Power-on Reset
       |
       v
HSM Boot ROM
       |
       | Authenticate HSM Firmware
       v
HSM Secure Firmware
       |
       | Initialize root services
       | Establish protected boot configuration
       v
Authenticate Main Boot Images
       |
       | OpenSBI
       | OP-TEE
       | UEFI
       v
Configure Initial Hardware Isolation
       |
       v
Release Application Processor
       |
       v
OpenSBI Initialization
       |
       | Hart init
       | Domain init
       | Security policy
       v
OP-TEE Initialization
       |
       | Core MMU
       | Trusted threads
       | Crypto service
       | HSM communication
       | Secure storage
       v
UEFI / Boot Manager
       |
       | Boot verification / measurement policy
       v
Linux
       |
       | TEE Driver
       | tee-supplicant
       v
Trusted Application Services
```

需要强调两个细节。

第一，这里的顺序只是一种可行的参考架构。真实系统中的 OpenSBI、UEFI 和 OP-TEE 可以有不同的启动与交接安排，UEFI 不一定必须在 OP-TEE 初始化之后才开始。

第二，镜像的认证者不一定是直接执行镜像的处理器。HSM 可以执行认证，主 CPU 负责加载或执行，但两者之间必须保证认证对象与实际执行对象一致。

## 19.10.3 整套安全体系由谁负责？

这是一个适合实际项目进行责任划分的表格。

| 安全需求                        | 主要责任组件                     | 必须验证的事项                         |
| --------------------------- | -------------------------- | ------------------------------- |
| HSM Firmware Authenticity   | Boot ROM / HSM RoT         | 签名、版本、加载约束                      |
| Main Firmware Authenticity  | HSM / Trusted Bootloader   | OpenSBI、UEFI、OP-TEE 认证链         |
| Domain Isolation            | OpenSBI + Hardware         | PMP、Domain State、资源权限           |
| TA Isolation                | OP-TEE                     | TA MMU、Syscall、Memory Ownership |
| Device Root Key             | HSM                        | 不可导出性、授权、Provisioning           |
| Storage Confidentiality     | OP-TEE / HSM               | 密钥管理、加密实现                       |
| Storage Rollback Protection | RPMB / 可信存储服务              | Counter、Atomicity、恢复            |
| DMA Isolation               | IOMMU/IOPMP/Firewall       | Requester ID、权限与控制权             |
| Secure IPC                  | OP-TEE + HSM               | 身份、认证、重放保护                      |
| Suspend/Resume Security     | PM Controller + Trusted FW | 状态保持、重新建立保护                     |
| Device Attestation          | HSM + Measured Boot        | 身份、测量、报告可信性                     |

一个 Staff Engineer 不应该只关注自己负责的某一块驱动是否通过测试。

还需要确认整个系统中，安全责任是否存在无人负责的空白。

例如：

- HSM 认为 OpenSBI 负责配置 DMA Firewall。
- OpenSBI 认为硬件 Reset Default 已经配置完成。
- RTL 设计人员认为由 HSM Firmware 负责初始化。
- 最终三个组件都没有在释放 Linux 之前完成配置。

这种集成风险比很多单独的代码 Bug 更危险。

## 19.10.4 建议采用 Security Invariant 驱动设计

对于整个 SoC，可以定义以下顶层约束：

```
S1:
No untrusted CPU/domain can read HSM root secrets.

S2:
No untrusted bus master can access OP-TEE private RAM.

S3:
Only authenticated firmware may execute with
trusted-domain privileges.

S4:
No untrusted software can alter locked security
configuration.

S5:
No stale HSM session may survive a relevant reset
without revalidation.

S6:
No trusted storage state may be accepted if its
required integrity/freshness checks fail.

S7:
No untrusted client may obtain an operation beyond
its authorized key policy.
```

这些不变量应落实到三个层次。

Architecture Specification

定义安全需求、威胁模型和组件职责。

Implementation

在 RTL、Firmware、OS 和驱动中建立强制机制。

Verification

通过仿真、故障注入、回归测试和实际硬件验证证明约束在预期威胁模型下成立。

## 19.10.5 在 ZeBu/Palladium 预硅环境中应该验证什么？

针对 Firmware/OS Engineer，我建议把本章中的安全验证拆成几个具体阶段。

Pre-Silicon Security Bring-up Checklist

交互式验收清单

Boot Trust

破坏 OpenSBI / OP-TEE 镜像签名，验证 Secure Boot 拒绝行为

Anti-Rollback

启动合法签名但 Security Version 过旧的镜像，验证版本策略

CPU Isolation

从 Linux Domain 越权读取 OP-TEE Private RAM

DMA Isolation

配置 PCIe/Ethernet DMA 访问 Secure RAM，验证硬件拒绝

HSM Access

尝试从非授权 Domain 直接访问 HSM MMIO/Private SRAM

IPC Authentication

伪造 HSM Request、重放合法请求、修改共享 Buffer

Key Isolation

验证 HUK/Root Key 不出现在非授权内存或普通调试接口

Storage Rollback

回放旧的 Secure Storage 数据并验证拒绝或受控恢复

Reset Security

分别测试 Main CPU Reset、HSM Reset 和外设 Reset

SMP Stress

多个 Hart 同时调用 HSM Crypto Service 并注入异常

Power Management

Suspend/Resume 后检查隔离、Key Handle 和未完成请求

Firmware Update

注入掉电，验证 A/B Recovery 与 Anti-Rollback Counter 策略

已验证 0 / 12

重置

其中，DMA 与互连测试最好与 RTL Verification Engineer 联合完成，因为需要确认访问被拒绝的位置、错误返回、总线事务处理和安全事件上报。

仅仅在 Linux 中看到 Access Fault，不能证明所有 Bus Master 都受到了保护。

## 19.10.6 从 upstream 的角度，哪些工作值得贡献？

本章有些内容高度依赖具体 SoC，例如：

- OTP/eFuse 驱动。
- 专有 HSM Mailbox。
- SoC Firewall Register。
- 厂商专用 Crypto Engine。

这些不一定都适合作为 OP-TEE 通用代码直接提交。

但仍然有一些很有价值的 upstream 方向：

- 更完善的 RISC-V HUK Platform Abstraction。
- Hardware Crypto Backend 的功能和错误处理测试。
- HSM Reset / Timeout / Cancellation 的抽象接口。
- Crypto Key Handle 的生命周期及访问控制。
- RISC-V Secure Memory 与 DMA Mapping 的回归测试。
- 安全 IPC 的 Buffer Validation。
- Secure Storage 异常恢复和负向测试。
- Device Tree 绑定、通用驱动与可复用的平台适配。

提交前还需要确认某一能力到底应由 OP-TEE、OpenSBI、Linux 还是 HSM Firmware 实现。

从长远来看，明确不同 upstream 项目的架构边界，并把通用机制和 SoC-specific 实现拆分开，是 Maintainer 级工程能力的重要表现。

# Chapter 19 — Staff Engineer 级别面试与架构设计题

下面设计 16 道题。

这些问题不是单纯测试术语记忆，而是模拟实际 SoC Security Design Review、Firmware Bring-up、Security Incident 和跨团队架构讨论。

每道题都包含分析方向及参考答案。

## Question 1 — HSM 已经验证 OP-TEE Image，为什么仍可能发生安全攻击？

场景：

HSM 在 DRAM 中验证 OP-TEE Image 的 RSA Signature，验证成功之后，通知 Application CPU 执行该镜像。

系统有 16 个 Hart 和若干 PCIe DMA 设备。

如果你是负责 Security Architecture 的 Staff Engineer，你会如何评审这个设计？

### 参考答案

首先判断是否存在 TOCTOU。

认证的对象是某一时刻 DRAM 中的镜像内容，但实际执行可能发生在之后。

如果 DMA 或其他 Hart 能够在认证后修改镜像，则认证结果失去了意义。

需要进一步检查：

1. 加载镜像时，谁控制目标 DRAM？
2. HSM 是否通过安全 DMA 将镜像复制到受保护区域？
3. CPU 和外设是否能够在认证期间修改镜像？
4. 验证成功后是否立即建立只读或隔离权限？
5. 在 Application CPU 被释放之前，相关 Firewall 是否已锁定？
6. OP-TEE Runtime 是否能维持 Code Integrity？

可能的安全设计是将镜像放入已经受保护的区域，在保证内容稳定的条件下执行认证，并在执行期间持续维护相应的写保护。

关键结论：

Secure Boot 不只是验证数字签名，还包括对认证对象、加载过程、执行地址和运行时权限的完整保护。

## Question 2 — 仅使用 PMP 是否能够实现工业级 RISC-V TEE？

场景：

SoC 有 16 个 Hart，已经通过 PMP 将 Linux 与 OP-TEE 的物理内存分开。

安全团队宣布已完成 TEE Isolation。

你是否认同？

### 参考答案

不能直接认同。

PMP 对 Hart 发出的相关物理地址访问进行保护，但不自动覆盖所有 DMA、外设和调试访问路径。

需要评审：

- PCIe、Ethernet、GPU 等 DMA Master 是否可以访问 Secure DRAM。
- HSM 的 Private SRAM 是否受到独立硬件保护。
- 非可信软件是否能重新配置 IOMMU/Firewall。
- Debug Module 是否能访问敏感内存。
- 不同 Hart 的 PMP 是否在进入非可信代码之前正确初始化。
- Domain Switch 是否可能留下旧的权限。
- Shared Memory 的映射是否严格受控。

如果上述问题尚未解决，不能宣称建立了完整的 SoC 级 TEE 隔离。

应当从完整的 Physical Access Graph 出发，枚举能够访问敏感资产的所有 Bus Master，并逐个验证硬件保护。

## Question 3 — HSM 的 Root Key 不可导出，但 OP-TEE 需要 Secure Storage，应该如何设计？

场景：

HSM 支持：

```
DERIVE_KEY
AES_ENCRYPT
AES_DECRYPT
HMAC
```

Root Key 不能被导出。

OP-TEE upstream 当前的 Storage 代码期望获得密钥字节。

请提出两种架构方案，并说明取舍。

### 参考答案

方案一是让 HSM 派生一个限定用途的 OP-TEE Storage Root Key，并将其返回到 OP-TEE 安全内存。

OP-TEE 再基于现有的 HUK/SSK/TSK/FEK 结构进行 Secure Storage。

它比较容易与现有实现兼容，也能避免每次文件读写都通过 HSM IPC。

缺点是如果 OP-TEE Core 被攻破，这一级密钥可能泄露。

方案二是 HSM 保留派生密钥，仅向 OP-TEE 返回 Key Handle。

OP-TEE 通过 HSM Crypto Backend 使用密钥。

这样可以增强密钥不可导出性，但会增加 IPC 开销、Storage Provider 改造、Buffer Mapping 和 Reset 恢复的复杂度。

应该先明确安全要求究竟是“保护设备根密钥”，还是“即使 OP-TEE 被攻破，也不得导出业务密钥”。

再结合性能和已有代码选择方案。

## Question 4 — 为什么不能直接用 OP-TEE Image Hash 派生 Secure Storage Key？

场景：

某工程师提出：

\\[ K\_{\text{storage}}=\operatorname{HMAC}(HUK,H(OPTEE)) \\]

认为这样可以确保不同版本的 OP-TEE 不能访问彼此的存储数据。

这个设计有什么问题？

### 参考答案

它将 Persistent Storage Key 与具体 Firmware Binary 绑定。

每次 OP-TEE 升级，镜像哈希都会发生变化。

旧的 Secure Storage 数据可能无法被新版本解密，导致正常升级后数据不可用。

此外，回滚、Recovery 和 A/B Boot 可能使密钥状态更加复杂。

正确做法是区分：

- 长期稳定的 Storage Root。
- 与 Firmware Measurement 相关的短期密钥。
- 版本化的 Key Policy。
- 显式的密钥迁移机制。

如果产品确实要求不同安全版本之间进行密码学隔离，就应设计明确的 Migration Protocol，而不是无条件把完整 Image Hash 加入所有持久密钥的派生上下文。

## Question 5 — Linux 可以控制 IOMMU，是否意味着 DMA Isolation 仍然安全？

场景：

SoC 具有符合 RISC-V IOMMU 规范的控制器。

但是 IOMMU 的 Root Configuration 和 Page Table 完全由 Linux 管理。

Linux 已经被攻击者控制。

是否能够依靠当前 IOMMU 保护 OP-TEE RAM？

### 参考答案

不能仅凭 IOMMU 的存在就认定安全。

攻击者可能通过控制 IOMMU 映射，将 Secure DRAM 映射给受控 DMA 设备。

需要一个 Linux 无法绕过的硬件保护边界。

例如：

- 上层独立 SoC Firewall。
- 受可信 Firmware 控制的 IOPMP。
- 硬件锁定的安全地址范围。
- 可信的设备所有权与 Requester Identity 机制。

另外必须验证 IOMMU 本身的配置接口是否受保护。

关键原则：

保护内存访问的控制机制，必须由不低于其保护目标信任等级的实体管理。

## Question 6 — 如果 Linux 回放昨天的 Secure Storage 文件，AES-GCM 为什么无法阻止？

场景：

TA 使用 REE FS 保存一个单调增加的业务 Counter。

Linux 保存了 Counter=10 时的整个存储快照。

当 Counter 增加到 100 后，Linux 把旧快照恢复。

### 参考答案

AES-GCM 可以检测密文和关联元数据是否遭到未经授权的修改。

但是昨天保存的旧密文和 Authentication Tag 本身仍然有效。

因此仅检查认证标签，无法知道这是一份旧数据。

需要可信的新鲜性状态，例如：

- RPMB Write Counter 和认证机制。
- HSM 保护的 Monotonic Counter。
- 受保护的全局 Root Hash 与提交协议。

还要关注跨文件一致性、Atomic Update 和掉电恢复。

否则虽然阻止了单个数据块的篡改，仍可能无法阻止整体状态回放。

## Question 7 — HSM 在操作过程中发生 Reset，OP-TEE 是否应该自动重试？

场景：

OP-TEE 向 HSM 发出：

```
INCREMENT_SECURE_COUNTER
```

HSM 完成操作，但还未发送 Response 就发生 Reset。

OP-TEE 超时。

### 参考答案

不能无条件重试。

因为该操作存在持久副作用。

如果再次执行，Counter 可能增加两次。

需要设计可恢复的事务语义。

例如：

- 为操作分配唯一 Request ID。
- 在受保护存储中记录操作结果或事务状态。
- Reset 后允许查询先前请求是否已经完成。
- 确保同一 Request ID 不会重复产生副作用。
- 如果无法判断执行结果，应显式返回不确定状态并启动受控恢复，而不是盲目重试。

这类设计涉及 Exactly-Once Effect 的工程实现问题。

实际上无法仅依靠“Request 发送一次”来保证“操作只执行一次”。

## Question 8 — Secure Boot 的 Anti-Rollback Counter 什么时候更新？

场景：

设备当前运行 OP-TEE v4。

准备升级到修复漏洞的 OP-TEE v5。

v5 的最低 Security Version 高于 v4。

但 v5 可能因为硬件兼容问题无法启动。

### 参考答案

不能只关注签名验证成功。

如果在镜像第一次启动之前就不可逆地提高最低 Security Version，随后 v5 启动失败，设备可能无法回退到 v4。

一种常见设计是：

1. 将 v5 安装到非活动 Slot。
2. 对镜像及其安全版本进行验证。
3. 进入 Trial Boot。
4. 执行规定的 Health Check。
5. 确认新版本可以正常运行。
6. 提交新 Slot，并推进相应 Anti-Rollback Counter。

但是否允许回退到 v4，仍取决于安全策略。

如果 v4 已知存在不可接受的漏洞，不能为了可恢复性而无限期允许不安全版本运行。

因此需要设计可信 Recovery Image，以及明确定义的 Firmware Version Policy。

## Question 9 — HSM 和主 CPU 使用同一套 Shared Memory Mailbox，有哪些风险？

场景：

Linux 与 OP-TEE 都可以访问一段共享 SRAM。

HSM 从该 SRAM 读取请求。

Linux 不能直接访问 HSM Private SRAM。

### 参考答案

隔离 HSM Private SRAM 并不能自动保证 IPC 安全。

主要风险包括：

- Linux 伪造 OP-TEE Request。
- 修改 Request Descriptor。
- Replay 已经认证的请求。
- 通过 Length/Offset 造成越界访问。
- 使用共享 Buffer 引入 TOCTOU。
- 将 HSM 变成任意签名或密钥派生 Oracle。
- 利用 Queue Flooding 进行 DoS。

应当通过硬件端点身份、受保护会话、消息认证、严格长度校验和授权策略建立安全通信。

尤其不能把请求中的 `caller_id` 当成可信身份，除非它由可靠的安全机制提供或认证。

## Question 10 — Suspend/Resume 后 Secure DRAM 泄露，如何定位根因？

场景：

Cold Boot 时安全测试全部通过。

但系统进入 Suspend-to-RAM 再恢复后，Linux DMA 偶尔能够读取 OP-TEE Private DRAM。

### 参考答案

首先排查 Reset/Power Domain 之间的关系。

可能的根因包括：

- IOPMP/Firewall 在 Suspend 时失去配置。
- DRAM 保留数据，但访问权限寄存器复位。
- Linux DMA 恢复早于安全策略恢复。
- 某些设备的 Requester ID 发生变化。
- Resume 路径遗漏部分 DMA Master 的权限配置。
- 恢复后的旧 IOMMU 映射仍然生效。
- 保护配置已写入，但相关同步和完成确认不足。

应在仿真环境中记录：

```
Suspend entry
Power transition
Firewall reset
DRAM retention
Resume entry
Security reconfiguration
Linux release
```

重点寻找是否存在短暂的非法访问窗口。

修复方向通常要求在释放非可信 Master 之前建立或恢复访问控制，而不是在 Linux Driver 的 Resume Callback 中才进行关键安全配置。

## Question 11 — Hardware AES Engine 能访问 Secure RAM，为什么仍需要评审其 DMA？

场景：

一个硬件 Crypto Engine 只由 OP-TEE 驱动调用，Linux 没有直接访问它的驱动。

安全团队认为不需要对 Crypto Engine 的 DMA 做额外限制。

### 参考答案

这仍然需要评审。

原因包括：

1. 没有 Linux Driver 不等于 Linux 无法直接访问设备 MMIO。
2. Crypto Engine 的 DMA 可能能够访问整个 DRAM。
3. OP-TEE 驱动本身可能存在 Buffer Validation 错误。
4. 共享 Descriptor 可能被篡改。
5. 设备的 Reset、Clock 和 Debug Interface 可能由其他实体控制。
6. Crypto Hardware 可能缓存敏感密钥或数据。
7. 错误 DMA 地址可能破坏其他 TA 或 OP-TEE Core 内存。

正确方案应限制 DMA 的物理访问范围，确保 Descriptor 可信、密钥权限受控，并在错误与 Reset 路径清理状态。

## Question 12 — HSM 签署了正确的 OP-TEE Measurement，是否证明系统没有被攻破？

场景：

远程服务器收到一个有效的设备证明：

```
OPTEE_HASH = expected_hash
SIGNATURE = valid
```

服务器认为当前 TEE 完全安全。

### 参考答案

这个判断过于宽泛。

首先需要确认 Measurement 的来源可信。

其次，Boot Measurement 只能说明启动过程中记录的镜像状态。

它通常不能证明：

- 运行期间不存在内存破坏漏洞。
- 某个 TA 没有被利用。
- HSM/OP-TEE 的运行时状态未发生任何异常。
- DMA 或其他硬件隔离没有失效。

如果 Attestation 只覆盖 Boot State，就不能宣称它证明整个 Runtime State 始终安全。

还要验证：

- 设备证书链。
- Nonce 和 Freshness。
- Security Lifecycle。
- Measurement Policy。
- Anti-Rollback State。
- HSM 的签名授权路径。

Attestation 是基于特定声明和威胁模型的证据，而不是通用的“系统绝对安全证明”。

## Question 13 — OP-TEE Core 被攻破后，独立 HSM 还能保护什么？

场景：

攻击者通过 OP-TEE Core 漏洞获得了 OP-TEE 内核执行权限。

但 HSM Firmware 没有被攻破。

### 参考答案

需要根据资产位置和授权设计进行判断。

如果 Root Key 始终保存在 HSM 的不可导出密钥槽中，攻击者可能仍无法直接导出该 Root Key。

但如果 HSM 允许 OP-TEE 不受限制地执行：

```
SIGN_ARBITRARY_DATA
DECRYPT_ARBITRARY_DATA
DERIVE_ARBITRARY_KEY
```

那么即使 Root Key 不可导出，攻击者仍可能滥用密钥能力。

因此需要在 HSM 内保留足够细粒度的 Key Usage Policy。

例如：

- 设备身份密钥只能签署规定格式的 Attestation Claims。
- Storage Key 只能用于指定的 Storage Service。
- 某些 Key Handle 只能在特定 Security Lifecycle 下使用。
- 敏感操作需要经过受保护的授权过程。

所以：

Key Non-Exportability 是重要的安全属性，但不足以替代对密钥操作的授权限制。

## Question 14 — 16 个 Hart 同时使用 HSM，怎样避免并发和死锁问题？

场景：

16 个 Hart 同时调用 OP-TEE。

其中多个 Trusted Thread 使用同一个 HSM Mailbox 和 Crypto Engine。

有时出现系统 Hang。

### 参考答案

首先区分三层并发状态：

OP-TEE 层

Trusted Thread、TA Context、锁和等待条件。

Transport 层

Mailbox Queue、Request ID、Completion、Interrupt。

HSM 层

Key Handle、Crypto Engine、Operation Context 和资源锁。

需要排查是否存在循环等待，例如：

```
OP-TEE:
    Hold crypto lock
    Wait for HSM

HSM:
    Wait for resource controlled by OP-TEE
```

或者：

```
Thread A:
    Hold mailbox lock
    Wait completion

Completion path:
    Need mailbox lock
```

建议明确锁顺序、阻塞规则及中断上下文可调用性。

对于多个并发请求，应通过 Request ID 区分响应，避免用全局单一 Completion State 错误唤醒其他线程。

还需要设计 Queue Backpressure、Timeout、Cancellation、Reset Recovery 和 Key Handle 并发策略。

## Question 15 — 一台生产设备需要进入 RMA 模式，能否直接打开 JTAG？

场景：

设备在客户现场发生故障。

售后工程师希望启用完整 JTAG 调试，同时保留设备中的所有用户数据和 HSM Root Key。

### 参考答案

不能默认接受这种设计。

开放完整调试访问可能破坏生产状态下的根密钥、TA 数据、HSM SRAM 或其他敏感资产。

首先需要区分：

- Debug Access 能够访问哪些处理器和内存。
- 是否能够读取 HSM 密钥寄存器。
- 是否能够修改安全启动配置。
- 是否能够绕过 PMP/Firewall。
- 是否存在安全授权的 Debug Unlock。
- 从 Production 转换到 RMA 是否需要销毁或失效密钥。

高安全等级产品通常应当设计受控 RMA Lifecycle。

例如先完成密钥销毁或使密钥不可用，再启用某些诊断能力。

如果确实要求保留敏感数据，就必须设计受限制且经过认证的调试机制，不能直接假设完整 JTAG 与秘密保密性可以同时成立。

## Question 16 — 如何评审一个声称“RISC-V OP-TEE Porting 已完成”的项目？

场景：

项目团队提供如下结果：

```
OpenSBI boot OK
OP-TEE boot OK
Linux boot OK
xtest passed
```

项目经理认为已经具备量产条件。

你会提出哪些问题？

### 参考答案

首先肯定这些结果已经证明了软件启动及部分功能路径正确。

但它们不足以证明 SoC Security Architecture 完整。

需要进一步评审：

Boot Trust

是否完整验证 HSM Firmware、OpenSBI、OP-TEE、UEFI 等镜像？是否有 Anti-Rollback？

Isolation

CPU PMP、DMA、IOMMU、Firewall、Secure Peripheral 和 Debug 是否满足威胁模型？

Key Management

HUK 是否真实且每设备独立？生产环境是否禁用了测试密钥和不安全 Provisioning 路径？

Secure Storage

是否验证数据保密性、完整性、回滚保护和掉电恢复？

IPC

HSM/OP-TEE 是否具有可信身份、授权、消息完整性与重放保护？

Concurrency

多 Hart、Interrupt、Crypto Queue、Domain Switch 是否经过压力测试？

Lifecycle

Manufacturing、Production、RMA、Decommissioning 是否有可信状态转换？

Power/Reset

Suspend、Warm Reset、HSM Reset、CPU Hotplug 后安全状态是否能够正确恢复？

Firmware Update

是否具有安全的升级、试运行、提交和 Recovery 机制？

Verification

是否有可重复的 Negative Test、Fault Injection、RTL/Emulation Test 和 Security Review Evidence？

最终应该将：

```
Functional Bring-up Complete
```

与：

```
Security Architecture Verified
```

明确区分。

真正的量产验收需要后者的证据，而不是只根据启动日志和功能测试通过作出判断。

# 本章实践建议：三个综合实验

为了将这一章转化为实际能力，建议优先完成以下三个实验。

### Lab 19-A — HUK / Secure Storage 密钥派生路径分析

从以下源码开始：

- [huk_subkey.c](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/huk_subkey.c?utm_source=chatgpt.com)
- [otp_stubs.c](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/otp_stubs.c?utm_source=chatgpt.com)
- [tee_fs_key_manager.c](https://github.com/OP-TEE/optee_os/blob/master/core/tee/tee_fs_key_manager.c?utm_source=chatgpt.com)

分析并记录：

```
Platform HUK
     |
     v
huk_subkey_derive()
     |
     v
Storage Root
     |
     v
TSK
     |
     v
FEK
```

尝试在仅用于测试的环境中更改 HUK，观察旧 Secure Storage 是否仍然可读。

再比较两种实现方案：

- HSM 返回限定用途的 OP-TEE Root Subkey。
- HSM 保存 Key Handle，仅提供加密服务。

重点形成一份能够解释安全取舍的设计文档。

### Lab 19-B — 模拟不可信 DMA 的安全访问

在支持所需隔离模型的仿真环境中：

1. 为 Linux 和 OP-TEE 分配不同物理内存区域。
2. 配置 CPU 访问权限。
3. 从 Linux 发起 CPU 非授权访问。
4. 从 DMA Master 发起相同的物理访问。
5. 观察两个访问是否都被正确拒绝。
6. 故意移除某个 DMA 保护规则，验证测试是否能够检测隔离失效。

这个实验尤其适合在能够观察 Bus Transaction、Firewall Reject 和 RTL Signal 的预硅环境中完成。

### Lab 19-C — HSM Mailbox Fault Injection

如果真实 HSM 环境暂时不方便修改，可以在测试环境中实现一个模拟 HSM Service。

定义三个命令：

```
HSM_DERIVE_KEY
HSM_HMAC
HSM_GET_DEVICE_STATUS
```

然后测试：

- 无效 Session。
- 错误 Key Handle。
- 消息 Length 越界。
- 请求重放。
- Response 丢失。
- HSM Reset。
- 两个 Hart 同时访问。
- 请求执行到一半被取消。

最终为每种情况定义可观察的预期结果，而不是只依赖“不能 Crash”作为测试标准。

# 本章总结：从单个 TEE 组件走向整个 SoC 安全架构

本章的核心逻辑可以归结为以下这条完整的信任链：

Root of Trust

ROM · OTP · HSM · Lifecycle

Trusted Boot

Signature · Measurement · Anti-Rollback

Runtime Isolation

OpenSBI · PMP · MMU · DMA Firewall

Trusted Services

OP-TEE · TA · Crypto · Secure Storage

Lifecycle & Recovery

Key Management · IPC · Reset · Firmware Update

真正需要建立的不是五个相互独立的功能模块，而是一条在启动、运行、通信、休眠、复位、升级和设备退役期间都能够持续成立的安全保证。

尤其是对具有独立 HSM/RSS 安全处理器的 RISC-V SoC，我认为最值得深入研究的是以下三个问题：

第一，HSM 怎样把 Root of Trust 可靠地延伸到 OpenSBI 和 OP-TEE？

这要求掌握 Secure Boot、Image Loading、Domain Initialization、Hardware Isolation 和 Attestation。

第二，OP-TEE 怎样使用 HSM 的能力，同时不破坏 HSM 的密钥隔离？

这要求掌握 HUK/KDF、Crypto Backend、Key Handle、IPC Authentication、Secure Storage 和 Key Lifecycle。

第三，整个 SoC 在异常情况下能否继续维持安全边界？

这要求掌握 DMA Protection、Reset Tree、Power Domain、Firmware Update、并发同步和故障恢复。

对于想深入 OP-TEE 内核并参与 upstream 的工程师而言，这一章真正重要的收获是：

> OP-TEE 是可信计算系统中的核心执行组件，但 OP-TEE 的可信性必须由启动信任链、硬件隔离、密钥管理和安全生命周期共同保证。

一名优秀的 OP-TEE Porting Engineer 可以让 TEE 在新的 SoC 上运行；而 Staff 级 Security Architecture Engineer 还需要证明：即使非可信软件失效、多个 Hart 并发运行、DMA 发起恶意访问、HSM 发生复位，系统依然满足事先定义的安全边界。

这也正是从 Firmware/OS 开发走向完整 SoC Security Architecture 的关键跃迁。
