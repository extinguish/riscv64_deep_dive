# Chapter 13 — SBI MPXY / RPMI TEE Communication Architecture

RISC-V OP-TEE Architecture & Porting · Part IV：REE / TEE 通信与完整调用链

本章主题：从 SBI ECALL 到跨安全 Domain 的 TEE Service 调用

本章将系统性覆盖原定的 13.1～13.10，深入到协议架构、CSR/汇编、消息格式、Linux Driver、OpenSBI、OP-TEE 接口、并发约束以及工业级故障处理，并在最后提供 Staff Engineer 级别的场景题与参考答案。

## 0. 本章的核心目标

前面的 Chapter 04～12 已经分别建立了三个重要基础：

- OpenSBI Domain Architecture：如何在 RISC-V 上定义并隔离不同的执行环境。
- OP-TEE Core Kernel：Trusted Thread、Trap、Context Switch、TA Isolation 如何工作。
- Hardware Isolation：PMP、MMU、系统级保护如何限制不同 Domain 对内存和外设的访问。

现在出现一个新的问题：

即使 Linux Domain 与 OP-TEE Domain 已经被安全隔离，它们仍然必须进行通信。

例如 Linux 中的 CA 调用：

```
TEEC_Result res = TEEC_InvokeCommand(
    &session,
    command_id,
    &operation,
    &error_origin
);
```

这个调用最终需要进入 OP-TEE，执行 Trusted Application，再把结果返回 Linux。

在 Arm TrustZone 平台上，通常可以依靠 SMC、Secure Monitor 和相应的 OP-TEE ABI 实现。

但 RISC-V 没有等价于 Arm SMC 的标准指令，也没有标准化的 Arm Secure/Normal World 切换机制。

因此，本章最重要的架构问题是：

> 如何在不破坏 RISC-V 标准 SBI 模型的前提下，实现 Linux Domain 与 OP-TEE Domain 之间安全、可扩展、支持 SMP 和 RPC 的通信？

### 0.1 本章涉及的四层协议

OP-TEE Protocol / Control ABI

Session · Invoke · RPC · Shared Memory References

RPMI — TEE Service Group

TEE_CALL · Endpoint · Service UUID · Memory Parcel · Signal

SBI MPXY — Message Proxy

Channel ID · Message ID · Per-Hart Buffer · Transport

RISC-V ISA — ECALL / Trap

a0–a7 · scause · mepc · M-mode Firmware

注意这里的分层关系：

- ECALL 是处理器提供的陷入机制。
- SBI 是 Supervisor 软件调用更高特权固件的标准接口。
- MPXY 是 SBI 中的通用消息代理扩展。
- RPMI 定义消息的服务语义。
- OP-TEE ABI 定义一次 TEE 调用具体表达什么。

任何一个层次都不应简单地等同于 TEE Domain Switch。 Domain Switch 是固件为实现通信与安全执行而进行的底层动作，不是消息协议本身。

### 0.2 2026 年 11 月的实现状态

学习本章前，需要区分三种状态。

| 技术                        | 当前状态                          | 学习定位                 |
| ------------------------- | ----------------------------- | -------------------- |
| RISC-V SBI MPXY           | SBI v3.0 已发布规范                | 规范基础                 |
| RPMI TEE Service Group    | RPMI v2.0 开发规范，非已发布 v1.0 的一部分 | 设计依据，仍可能变化           |
| Linux OP-TEE RPMI Backend | 2026 年 9～10 月 RFC 持续演进        | 实际设计与 Code Review 案例 |

尤其值得注意：

2026 年 9 月 10 日的 RFC 倾向于将现有 OP-TEE SMC ABI 封装进 RPMI `TEE_CALL`；而 10 月 5 日另一组 RFC v2 已经采用独立 RPMI Transport + Service Bus + 新的 OP-TEE Control ABI，明确表示对应 OP-TEE OS 端的支持还未完成。10 月 7 日还有配套的 RPMI Transport v2。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

OpenWall Lists+2



因此本章会明确标识：

- [SPEC] 已发布规范或明确的规范定义。
- [UPSTREAM] 当前公开源码中能够核实的实现。
- [RFC] 正在讨论、尚不能视为稳定生产 ABI 的实现。
- [DESIGN] 为解释问题或提出工程方案而设计的示例。

这样可以避免在工业项目中错误地把某个 RFC 的数据结构作为长期稳定接口。

# 13.1 为什么 RISC-V 无法直接照搬 Arm SMCCC/SMC 模型？

## 13.1.1 首先区分三个容易混淆的概念

在 Arm 平台上，我们经常把 SMC、SMCCC 和 OP-TEE SMC ABI 放在一起讨论。

但它们不是同一个东西。

| 概念             | 本质             | 职责                      |
| -------------- | -------------- | ----------------------- |
| SMC            | Arm 指令         | 触发进入适当的 Monitor/更高异常级处理 |
| SMCCC          | 软件调用约定         | 规定 Function ID、参数和返回寄存器 |
| OP-TEE SMC ABI | OP-TEE 定义的接口语义 | 描述 TEE 调用、RPC、共享内存等操作   |

以典型的 Armv8-A TrustZone 系统为例：

```
Linux / EL1 / Non-secure
    |
    | SMC
    v
EL3 Monitor (TF-A)
    |
    | Save Normal World context
    | Restore Secure World context
    | Switch security state
    v
OP-TEE / S-EL1
```

这里有两个关键硬件条件：

第一，Arm 架构定义了安全状态以及用于管理跨安全状态转换的机制。

第二，SMC 是具有相应架构语义的指令，而不是普通函数调用。

因此可以利用 Secure Monitor 实现 Security State 的转换。

RISC-V 不具备完全相同的模型。

RISC-V 标准特权架构提供的是 M/S/U 等特权级，以及可选的虚拟化相关模式。仅仅从 S-mode 进入 M-mode，并不会自动发生所谓的 Normal World → Secure World 切换。

## 13.1.2 RISC-V 的 ECALL 是什么？

`ECALL` 的全称是 Environment Call。

它是一条同步异常指令。

当 S-mode 软件执行：

```
ecall
```

在典型的 M-mode SBI 实现中：

```
S-mode executes ECALL
        |
        v
Exception: Environment call from S-mode
        |
        v
M-mode trap handler
        |
        v
OpenSBI SBI dispatcher
```

在 S-mode ECALL 实际陷入 M-mode 的前提下，处理器会保存异常 PC 到 `mepc`，更新 `mcause`、`mstatus` 等相关状态，并跳转到 `mtvec` 所指定的处理入口。

其中：

- `mcause = 9` 表示 Environment Call from S-mode。
- `mepc` 保存触发 ECALL 的指令地址。
- `mstatus.MPP` 记录陷入前的特权模式。
- 固件完成处理后，通常通过 `mret` 返回适当的执行环境。

但需要特别注意：

ECALL 本身并不知道什么是 Linux Domain、OP-TEE Domain，也不知道什么是 TEE。

它只提供特权陷入机制。

是否要切换 Domain、是否允许访问 OP-TEE，以及使用哪个上下文，都需要由 OpenSBI 或其他受信任的 M-mode Firmware 决定。

## 13.1.3 一个最小 SBI ECALL 汇编示例

[SPEC] 标准 SBI v0.2 及之后的典型调用约定是：

| 寄存器       | 作用           |
| --------- | ------------ |
| `a0`–`a5` | 最多六个输入参数     |
| `a6`      | Function ID  |
| `a7`      | Extension ID |
| 返回 `a0`   | SBI Error    |
| 返回 `a1`   | SBI Value    |

例如：

```
/* Read SBI implementation ID */

/* SBI Base Extension ID */
li a7, 0x10

/* Get SBI implementation ID: FID 1 */
li a6, 1

ecall

/* a0 = error
 * a1 = implementation ID on success
 */
```

对应的可复用 C/汇编封装可以写成：

```
struct sbiret {
    long error;
    long value;
};

static inline struct sbiret
sbi_call(unsigned long eid,
         unsigned long fid,
         unsigned long arg0,
         unsigned long arg1,
         unsigned long arg2)
{
    register unsigned long a0 asm("a0") = arg0;
    register unsigned long a1 asm("a1") = arg1;
    register unsigned long a2 asm("a2") = arg2;
    register unsigned long a6 asm("a6") = fid;
    register unsigned long a7 asm("a7") = eid;

    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a2), "r"(a6), "r"(a7)
        : "memory"
    );

    return (struct sbiret) {
        .error = (long)a0,
        .value = (long)a1
    };
}
```

这是教学版封装，只使用三个参数。实际 Linux/OpenSBI 的完整封装还涉及 `a3`～`a5`、编译器约束、架构宽度以及平台代码约定。

这里最重要的是：

SBI 标准只规定 `a0` 和 `a1` 作为返回寄存器。不能假设其他输入参数寄存器也承载标准 SBI 返回值。

这就引出了 OP-TEE 移植中的一个真实兼容性问题。

## 13.1.4 为什么不能简单用自定义 ECALL 代替 SMC？

假设我们设计一个私有 TEE SBI Extension：

```
a7 = Vendor TEE Extension ID
a6 = Function ID
a0 = OP-TEE Command
a1 = Parameter Address
a2 = Other argument
a3 = Other argument

ecall
```

然后 OpenSBI 收到请求，切换到 OP-TEE Domain。

这种设计并非完全不能工作。早期参考平台和私有固件确实可以采用类似机制。

但它存在三个问题。

问题一：标准化和可移植性。

该接口可能只适用于某家厂商的 OpenSBI Fork。

问题二：调用约定。

某些 OP-TEE SMC 返回操作可能需要超过标准 SBI 双寄存器返回约定所提供的返回信息；不能假定自定义 ECALL 的多寄存器返回天然符合 SBI。

问题三：后续能力扩展。

当协议需要支持多 TEE Endpoint、Memory Parcel、异步通知、服务发现、ABI Negotiation 时，一个简单的 `TEE_SWITCH` ECALL 很快会变得难以维护。

2026 年 9 月的相关 Linux RFC 也明确指出了私有 TEE SBI EID 和非标准多寄存器返回约定的问题。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

OpenWall Lists



因此，较新的标准化设计倾向于：

```
ECALL
  → SBI MPXY
    → RPMI TEE Service
      → Firmware Domain Switch
        → OP-TEE
```

13.1 的结论：RISC-V 并不缺少进入 M-mode 的机制，真正需要标准化的是如何表达、传输并安全执行跨 Domain TEE 请求。

# 13.2 SBI ECALL 与传统 SBI Extension

理解 MPXY 之前，必须理解 SBI 原有设计为什么适合简单固件服务，却不适合作为复杂消息协议的全部抽象。

## 13.2.1 SBI 是什么？

SBI 全称 Supervisor Binary Interface。

它定义 Supervisor 软件与更高特权固件服务之间的接口。

常见 SBI Extension：

| Extension | EID          | 功能                        |
| --------- | ------------ | ------------------------- |
| BASE      | `0x10`       | 版本、扩展发现                   |
| TIME      | `0x54494D45` | 设置 Timer                  |
| IPI       | `0x735049`   | Inter-Processor Interrupt |
| RFENCE    | `0x52464E43` | 远程 Fence / TLB 同步         |
| HSM       | `0x48534D`   | Hart State Management     |
| MPXY      | `0x4D505859` | 通用 Message Proxy          |

这些 Extension 通过统一的 ECALL ABI 进入 SBI 实现，但各自提供不同的固件服务。

以 HSM 为例：

```
struct sbiret ret =
    sbi_call(
        SBI_EXT_HSM,
        SBI_EXT_HSM_HART_START,
        hartid,
        start_addr,
        opaque
    );
```

它适合描述一个简洁的操作：

> 请启动指定 hart，并从指定地址执行。

但一次 OP-TEE 调用可能携带更复杂的信息：

- Trusted Application UUID。
- Session ID。
- Command ID。
- 多个 Value / Memory 参数。
- Shared Memory 的生命周期和访问权限。
- RPC 或 Resume 状态。
- 错误来源及返回参数。

这些信息不适合全部塞进 `a0`～`a5`。

## 13.2.2 传统 SBI 接口的扩展方式

如果没有 MPXY，可以不断增加新的 SBI Function：

```
TEE_OPEN_SESSION
TEE_CLOSE_SESSION
TEE_INVOKE_COMMAND
TEE_REGISTER_SHM
TEE_UNREGISTER_SHM
TEE_GET_VERSION
TEE_RPC_RESUME
...
```

这种做法的问题是把两层本来应该独立的东西混在了一起：

- SBI 的固件调用和传输机制。
- 某个具体 TEE 系统的高级消息协议。

例如 OP-TEE、另一种 TEE OS 或独立安全服务未必具有相同的 Session 和 Command 语义。

将这些接口全部固化进 SBI，会使 ABI 演进更加困难。

## 13.2.3 MPXY 的核心设计思想

MPXY 全称 Message Proxy。

它的设计是：

由 SBI 提供统一的消息收发、Channel 发现、缓冲区和通知接口，具体消息语义由上层协议决定。

因此 SBI 不再需要了解一个 TEE Service 的内部命令含义。

这与 Linux 中的分层思想类似：

```
Linux TEE Client
        |
        v
OP-TEE Protocol
        |
        v
RPMI Protocol
        |
        v
SBI MPXY Transport
        |
        v
OpenSBI Firmware
```

可以把 MPXY 理解为一个由 SBI 暴露的消息承载层。

但不要将它简单理解成一个实际硬件 Mailbox Controller：MPXY 是软件 ABI 抽象，底层既可能转发到硬件 Mailbox，也可能转发到同一 hart 上的另一个执行 Domain，或由固件自身处理。

这一点已在 SBI v3.0 的 MPXY 规范中明确说明。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



# 13.3 MPXY（Message Proxy）的设计动机与体系结构

## 13.3.1 MPXY 试图解决什么问题？

设想一款 16-hart RISC-V Server SoC：

```
16 RISC-V Harts
       |
       +--- Linux
       |
       +--- OpenSBI / M-mode
       |
       +--- OP-TEE Domain
       |
       +--- Platform Management Services
       |
       +--- Independent HSM / Microcontroller
```

Linux 可能需要访问不同的固件服务。

如果每种服务都定义专属的 SBI Extension，就会增加平台依赖和 ABI 维护成本。

MPXY 试图统一以下机制：

1. 如何发现可用的消息 Channel。
2. 如何识别 Channel 对应的消息协议。
3. 如何查询 Channel 支持的能力。
4. 如何使用共享缓冲区传输消息。
5. 如何进行同步请求与返回。
6. 如何收发异步通知。

而消息具体是什么，留给 RPMI 等上层协议。

## 13.3.2 MPXY 的核心数据模型

MPXY 中有三个非常重要的 ID：

| 标识           | 宽度          | 作用                |
| ------------ | ----------- | ----------------- |
| Extension ID | SBI 定义的 EID | 指定 MPXY Extension |
| Channel ID   | `uint32_t`  | 标识一个消息通道          |
| Message ID   | `uint32_t`  | 指定该通道上消息的类型或操作    |

对于 RPMI over MPXY：

- `EID = 0x4D505859`。
- Channel Attribute 指定 `MSG_PROT_ID = 0`（RPMI）。
- RPMI Channel 的协议属性指定 Service Group。
- `message_id` 对应 RPMI 的 Service ID。

例如 TEE Channel 对应 `SERVICEGROUP_ID = 0x0010`，执行 `TEE_CALL` 时 `message_id = 0x13`。

这些数值分别来自 SBI v3.0 MPXY 规范与 RPMI v2.0 TEE Service Group 开发规范。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation+2



注意：

Channel ID 不等于 Hart ID，也不等于 TEE Endpoint ID。

它只是由平台或固件暴露的传输通道标识。

## 13.3.3 MPXY Channel 的发现机制

[SPEC] SBI MPXY 提供以下标准函数：

| FID | API                             | 用途               |
| --- | ------------------------------- | ---------------- |
| 0   | `get_shmem_size`                | 查询每 hart 缓冲区大小   |
| 1   | `set_shmem`                     | 设置调用 hart 的共享缓冲区 |
| 2   | `get_channel_ids`               | 枚举可用 Channel     |
| 3   | `read_attributes`               | 查询 Channel 属性    |
| 4   | `write_attributes`              | 配置可写属性           |
| 5   | `send_message_with_response`    | 同步请求及响应          |
| 6   | `send_message_without_response` | 无响应消息            |
| 7   | `get_notification_events`       | 获取通知事件           |

标准的 Channel Attribute 包括：

```
MSG_PROT_ID
MSG_PROT_VERSION
MSG_DATA_MAX_LEN
MSG_SEND_TIMEOUT
MSG_COMPLETION_TIMEOUT
CHANNEL_CAPABILITY
```

而 RPMI 进一步定义：

```
SERVICEGROUP_ID
SERVICEGROUP_VERSION
IMPLEMENTATION_ID
IMPLEMENTATION_VERSION
```

因此一个正确的 TEE Transport 初始化过程应当能够验证：

```
MPXY extension available?
        |
        v
Compatible channel available?
        |
        v
MSG_PROT_ID == RPMI?
        |
        v
SERVICEGROUP_ID == TEE?
        |
        v
Compatible versions and capabilities?
        |
        v
Initialize transport
```

特别需要注意：MPXY 的 `send_message_with_response` 虽然是标准定义的 FID #5，但该函数在 MPXY 规范中属于可选能力。不能仅凭 MPXY Extension 存在，就假设所有 Channel 都支持同步调用。

## 13.3.4 MPXY Shared Memory

这是理解 MPXY 的第一个关键技术难点。

[SPEC] MPXY 要求 Supervisor 软件为每个 hart 配置一块 Shared Memory。

该缓冲区是 Linux 与 SBI 实现之间交换消息数据的传输缓冲区。

典型结构：

```
Hart 0:
    MPXY SHMEM 0

Hart 1:
    MPXY SHMEM 1

Hart 2:
    MPXY SHMEM 2

...
```

每个 Hart 可以有各自的传输缓冲区，因此不必让全部 hart 争用同一个消息区域。

规范要求 MPXY Shared Memory 大小：

- 对所有 hart 相同。
- 至少 4096 Bytes。
- 为 4096 Bytes 的整数倍。
- 不小于平台暴露的最大 Channel 消息数据长度。

对应接口：

```
struct sbiret
sbi_mpxy_get_shmem_size(void);

struct sbiret
sbi_mpxy_set_shmem(
    unsigned long shmem_phys_lo,
    unsigned long shmem_phys_hi,
    unsigned long flags
);
```

通常初始化过程：

```
Linux CPU/Hart Initialization
        |
        v
Get MPXY SHMEM size
        |
        v
Allocate page-aligned buffer
        |
        v
Set MPXY SHMEM on current hart
        |
        v
Query channels and attributes
        |
        v
Transport ready
```

这里的 `shmem_phys_lo`、`shmem_phys_hi` 使用的是 SBI 的物理地址参数编码，并非 Linux Kernel Virtual Address。

另外：

MPXY Shared Memory 与 OP-TEE TA 使用的 Shared Memory 并不是同一个概念。

前者是 SBI 消息传输缓冲区；后者用于向 TEE 传递 Operation 参数或共享数据，通常有完全不同的大小、生命周期和权限管理机制。之后在 13.7 和 13.8 会进一步拆解。

## 13.3.5 一次 MPXY 同步调用

[SPEC] FID #5 的接口为：

```
struct sbiret
sbi_mpxy_send_message_with_response(
    uint32_t channel_id,
    uint32_t message_id,
    unsigned long message_data_len
);
```

执行过程：

```
Linux / S-mode
    |
    | Write request data into MPXY SHMEM
    |
    | a7 = 0x4D505859
    | a6 = 5
    | a0 = channel_id
    | a1 = message_id
    | a2 = request_size
    |
    | ECALL
    v
OpenSBI / M-mode
    |
    | Validate channel and request
    | Route message
    | Execute corresponding service
    |
    | Write response into MPXY SHMEM
    |
    | Return SBI_SUCCESS
    v
Linux / S-mode
    |
    | a0 = SBI error
    | a1 = response length
    |
    v
Parse response data
```

它解决了一个重要问题：

虽然标准 SBI 只保证 `a0`、`a1` 的返回语义，但通过共享缓冲区，协议可以返回结构化的可变长数据，而不需要自定义额外寄存器返回 ABI。

同时要注意：

- `a0 == SBI_SUCCESS` 表示 MPXY 层完成消息发送并收到了响应。
- `a1` 表示响应字节数。
- 真正的 RPMI Service Status 位于响应缓冲区内。
- 真正的 OP-TEE Command 结果还可能位于更高层。

也就是说，一次 TEE 调用存在多层错误状态，不能只检查 `a0`。

这将是 13.8 的重点内容。

# 13.4 RPMI：RISC-V Platform Management Interface 的职责

## 13.4.1 为什么已经有 SBI，还需要 RPMI？

理解 RPMI 的关键是区分 接口调用机制 与 平台服务协议。

SBI 主要解决：

> Supervisor 软件如何向 SBI Firmware 请求服务？

RPMI 主要解决：

> 不同平台管理实体之间，应该使用什么消息协议描述服务和资源操作？

RPMI 全称 RISC-V Platform Management Interface。

它的应用并不局限于 TEE，还包括系统复位、电源、时钟、性能管理、RAS 等平台服务。

例如，一个独立的管理 MCU 可能拥有控制 SoC 电源域的能力，而 Application Processor 上的 OpenSBI 需要通过某种消息传输与它交互。

这时可以使用 RPMI。

但 RPMI 不要求底层传输必须是 SBI。它也可以基于平台专用的 Mailbox 或其他传输机制。

在 Linux S-mode 与 OpenSBI M-mode 之间，SBI MPXY 则提供了一种标准化承载 RPMI 消息的方式。

所以有：

```
SBI     = Supervisor → Firmware interface

MPXY    = Message transport abstraction over SBI

RPMI    = Platform service protocol

TEE SG  = RPMI protocol 中的 TEE Service Group

OP-TEE  = TEE Service 的具体实现者之一
```

注意这里的 RPMI 并不是专门给独立 HSM Core 设计的协议。TEE Service 可以由同一物理 hart 上的安全 Domain 处理，也可以涉及其他安全实体；具体拓扑取决于平台实现。

## 13.4.2 什么是 RPMI Service Group？

Service Group 可以理解为一族相关的平台服务。

例如：

```
RPMI
 |
 +-- BASE
 |
 +-- SYSTEM_MSI
 |
 +-- SYSTEM_RESET
 |
 +-- HART_STATE_MANAGEMENT
 |
 +-- CLOCK
 |
 +-- DEVICE_POWER
 |
 +-- ...
 |
 +-- TEE
```

注意，RPMI 的 `HART_STATE_MANAGEMENT` Service Group 与 SBI 的 HSM Extension 是不同接口，不应该因为名字类似而混为一谈。

每个 RPMI Service Group 都有：

- Service Group ID。
- Service Group Version。
- 多个 Service ID。
- 每种 Service 的 Request/Response Format。
- 相应的错误和能力协商机制。

因此，MPXY Channel 与 RPMI Service Group 的关系通常是：

```
MPXY Channel
      |
      | MSG_PROT_ID = RPMI
      |
      | SERVICEGROUP_ID = 0x0010
      v
RPMI TEE Service Group
      |
      +-- TEE_PROBE_FEATURES
      +-- TEE_PROBE_SYSTEM
      +-- TEE_CALL
      +-- TEE_MEMORY_PARCEL_CREATE
      +-- TEE_MEMORY_PARCEL_ACCEPT
      +-- TEE_MEMORY_PARCEL_RELEASE
      +-- TEE_MEMORY_PARCEL_RECLAIM
      +-- TEE_SIGNAL_RAISE
      +-- TEE_SIGNAL_RETRIEVE
      +-- ...
```

这并不意味着 MPXY 必须理解每种 TEE 操作。

对 MPXY 来说，它只需要知道：

- 这是一个有效的 Channel。
- 该 Channel 允许什么类型的 Message。
- 输入数据在什么位置。
- 数据长度是否有效。
- 如何把消息交给对应的处理程序。

由 RPMI Framework 进一步解释 Service ID 和服务数据。

## 13.4.3 RPMI TEE Service Group 的核心服务

[RFC / 开发规范] RPMI TEE Service Group 使用 `SERVICEGROUP_ID = 0x0010`。

下表选出最重要的服务。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



| Service ID      | Service Name                      | 作用                        |
| --------------- | --------------------------------- | ------------------------- |
| `0x01`          | `TEE_ENABLE_NOTIFICATION`         | 服务通知控制                    |
| `0x02`          | `TEE_PROBE_FEATURES`              | 查询 Framework 能力           |
| `0x03`          | `TEE_PROBE_SYSTEM`                | 查询 Endpoint、Service 和系统拓扑 |
| `0x04`          | `TEE_EXIT`                        | Endpoint 生命周期管理           |
| `0x05` / `0x06` | `TEE_SIGNAL_BUS_SETUP/TEARDOWN`   | 建立和拆除异步信号通道               |
| `0x07` / `0x08` | `TEE_SIGNAL_RAISE/RETRIEVE`       | 发送和接收异步信号                 |
| `0x09`          | `TEE_MEMORY_PARCEL_CREATE`        | 创建内存共享/借用/捐赠事务            |
| `0x0A`          | `TEE_MEMORY_PARCEL_ACCEPT`        | 接收内存事务                    |
| `0x0B`          | `TEE_MEMORY_PARCEL_RELEASE`       | 接收方释放内存                   |
| `0x0C`          | `TEE_MEMORY_PARCEL_RECLAIM`       | 创建方回收内存                   |
| `0x0D` / `0x0E` | `TEE_MEMORY_SEGMENT_SEND/RECEIVE` | 分段传递内存描述                  |
| `0x13`          | `TEE_CALL`                        | 同步发送 TEE 服务请求             |

这里需要特别关注三个概念：Endpoint、Memory Parcel 和 TEE_CALL。

### Endpoint：消息接收实体的身份

假设系统中存在：

```
Endpoint A: Linux REE

Endpoint B: OP-TEE

Endpoint C: Another TEE
```

RPMI Framework 可以为它们分配不同的 Endpoint ID。

要注意：

Endpoint ID 不等于 Hart ID，也不等于 OpenSBI Domain ID。

它们分别属于不同的命名空间。

```
Hart ID
    → RISC-V 执行资源

OpenSBI Domain
    → 固件隔离和资源管理对象

RPMI Endpoint ID
    → 消息协议中的通信端点身份

MPXY Channel ID
    → 消息传输接口中的通道身份
```

一个 TEE Domain 可以包含多个 hart；一个 TEE OS 也可以代理其他逻辑 Endpoint。

这种身份划分对虚拟化和多个安全服务共存非常重要。

### Service UUID：指定提供什么服务

当系统存在多个 TEE Service 时，仅知道目标 Endpoint 可能还不够。

例如，一个 TEE 可以提供：

- OP-TEE Service。
- 一个专门的密钥服务。
- 一项厂商扩展的可信服务。

RPMI `TEE_CALL` 因此携带 16 字节 Service UUID，以区分目标服务。

它和 TA UUID 不是同一个层次。

```
RPMI Service UUID
    → 选择哪一种 TEE Service / Backend

TA UUID
    → 由 OP-TEE 内部选择哪个 Trusted Application
```

如果把二者混淆，就会错误地把所有 TA 都暴露成 RPMI Framework 必须管理的 Service。

通常没有这个必要。

### Memory Parcel：受管理的共享内存事务

Memory Parcel 不仅是一个物理地址。

它描述：

- 哪个 Endpoint 创建该 Parcel。
- 哪些物理页属于该 Parcel。
- 哪些 Endpoint 可以获得访问权限。
- 每个参与者拥有的 R/W/X 权限。
- 这是 Share、Lend 还是 Donate。
- Parcel 的 Accept、Release、Reclaim 生命周期。

这使 RPMI Framework 能够在消息协议层面表达内存权限转移。

但规范明确区分了权限描述与硬件执行：实际权限是否能够得到保障，取决于平台提供的硬件隔离能力。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 13.4.4 `TEE_CALL` 是如何工作的？

[RFC / 开发规范] `TEE_CALL` 的 Service ID 为 `0x13`。

它的核心语义是：

发送方同步调用指定 TEE Service，目标 TEE 在发起调用的同一个 hart 上执行，直到返回响应。

例如：

```
Hart 3:

Linux Domain
      |
      | TEE_CALL
      v
OpenSBI / RPMI Framework
      |
      | Save Linux Context
      | Select OP-TEE Domain
      v
OP-TEE
      |
      | Process request
      v
OpenSBI / RPMI Framework
      |
      | Restore Linux Context
      v
Linux Domain
```

注意，TEE 的实际执行不必意味着启动一个新的物理 Core。

这里通常是同一个 hart 上进行执行上下文的时间复用。

规范还强调，`TEE_CALL` 不提供一个由 TEE 同步反向调用 REE 的对等机制，因为 REE/TEE 的调度模型不同。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这就要求 OP-TEE 的 RPC、Yield、Resume 机制必须与这一调度模型配合。

### 一个容易误解的问题

既然 `TEE_CALL` 是同步的，为什么 OP-TEE 还需要 RPC？

同步指的是当前这一次 RPMI 消息交换具有 Request/Response 语义，并不意味着整个 TA Command 不会中途挂起。

举个例子：

```
Linux calls OP-TEE
      |
      v
OP-TEE needs REE FS
      |
      | Suspend trusted execution
      v
Return RPC request
      |
      v
Linux tee-supplicant executes file operation
      |
      v
Linux sends resume request
      |
      v
OP-TEE continues original TA command
      |
      v
Final response
```

整个 `TEEC_InvokeCommand()` 在应用层可能仍然表现为同步调用。

但它在底层可能包含多次跨 Domain 的消息交换。

这意味着必须区分：

- RPMI TEE_CALL Transaction：一次请求与响应。
- OP-TEE Trusted Thread Execution：可能跨越多次进入和退出。
- TA Command Lifecycle：用户最终发起的一次逻辑命令。

这三个生命周期不一定相同。

## 13.4.5 为什么不使用 RPMI MANAGEMENT_MODE？

这个问题很适合用于架构 Review。

RPMI 已经存在 `MANAGEMENT_MODE` Service Group，是否能够直接拿来运行 OP-TEE？

技术上可以强行复用消息承载机制，但接口语义不匹配。

`MANAGEMENT_MODE` 面向 UEFI PI Management Mode 的通信模型。

OP-TEE 则具有不同的执行环境、Session、RPC 和内存共享管理要求。

因此，将 OP-TEE 直接伪装成 Management Mode 服务，会导致：

- 原有规范语义被扭曲。
- 真实 Management Mode 用户发生接口冲突。
- TEE 的安全特性和能力发现无法得到合理表达。
- 后续标准化和跨平台兼容性困难。

2026 年 9 月的 RFC 已明确讨论过这一取舍。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

OpenWall Lists



13.4 的核心结论：RPMI TEE Service Group 解决的是 TEE 通信的协议语义，而不只是提供另一个进入 M-mode 的入口。

# 13.5 RPMI TEE Service Group：执行模型与安全边界

理解了 Service Group，接下来要追问：

到底是谁执行一次 TEE_CALL？

这涉及我们之前已经学习过的 OpenSBI Domain Context Switching。

## 13.5.1 一次 TEE_CALL 涉及哪些执行实体？

假设 Hart 2 当前运行 Linux：

```
Hart 2
------------------------------------------------
Linux S-mode
    |
    | TEE_CALL
    v
OpenSBI M-mode
    |
    | RPMI Framework
    |
    | Domain Context Switch
    v
OP-TEE S-mode
    |
    | Trusted Thread
    |
    | TA Execution
    v
OpenSBI M-mode
    |
    | Restore REE Domain
    v
Linux S-mode
------------------------------------------------
```

这里有两个关键角色。

RPMI Framework 负责消息级别的处理和路由，例如识别 Endpoint、UUID、权限和请求类型。

OpenSBI Domain Framework 负责执行环境的管理与隔离。

在具体实现中，这两个模块可以紧密集成，但概念上应该分开。

例如：

```
RPMI TEE Framework:
    Is this caller allowed to call this endpoint?
    Is this service supported?
    What message is being sent?

Domain Framework:
    Which S-mode context is active?
    Which address-space state must be restored?
    Which memory permissions must be applied?
    How does execution transfer safely?
```

这是为什么 Chapter 06 的 Domain Context Switching 是理解本章的基础。

## 13.5.2 一个简化的 M-mode 处理流程

下面是 [DESIGN] 教学伪代码，不对应某个确定版本的 OpenSBI 函数实现。

```
int rpmi_tee_call(struct tee_request *req,
                  struct tee_response *rsp)
{
    struct domain_context *ree;
    struct domain_context *tee;

    /* 1. Identify authenticated caller */
    ree = current_domain_context();

    /* 2. Validate requested endpoint/service */
    if (!tee_endpoint_access_allowed(ree, req))
        return RPMI_ERR_DENIED;

    /* 3. Find target execution domain */
    tee = find_target_tee_domain(req);

    if (!tee)
        return RPMI_ERR_NOT_SUPPORTED;

    /* 4. Bind request to validated caller */
    prepare_tee_request(ree, tee, req);

    /*
     * 5. Transfer execution to TEE.
     *
     * The actual implementation involves architecture
     * context, protection state and trap handling.
     */
    switch_to_tee_domain(tee);

    /*
     * 6. Execution returns here through the
     * configured firmware control path.
     */
    return process_tee_response(tee, rsp);
}
```

这个模型虽然简单，却体现了一个重要安全原则：

不能因为 Linux 在 Request 里写了某个 Sender ID，就认为它具有对应的身份。

Sender Identity 必须由可信的 Framework 根据调用来源进行验证。

例如，Linux 不能通过篡改 Request 声称自己是 OP-TEE Endpoint，从而获取更高权限。

## 13.5.3 Domain Switch 不等于函数调用

在普通 C 函数调用中：

```
caller()
    |
    v
callee()
    |
    v
return
```

通常只需满足当前 ABI 的寄存器保存约定。

但跨 Domain Switch 的安全要求远高于普通函数调用。

可能需要维护：

```
General Purpose Registers
Program Counter
Privilege Status
S-mode CSR State
Address Space / satp
Trap Entry / stvec
Scratch State / sscratch
Interrupt State
FP/Vector State
Per-Hart Domain Context
Memory Protection Configuration
```

在 RISC-V 中，不应该只保存 C ABI 定义的 callee-saved 寄存器。

例如：即使 `a0`～`a7` 是 caller-saved，Linux Domain 中的寄存器仍可能包含敏感数据。安全切换过程必须避免将不应该暴露的状态带入其他 Domain。

还需要考虑：

- Switch 时是否处于中断关闭状态。
- PMP/Firewall 更新与恢复的顺序。
- 新 Domain 的 `satp` 生效后需要什么同步。
- FP/Vector 状态是 Eager Save 还是 Lazy Save。
- 是否可能存在跨 Domain 的 Speculative State 泄露。
- 当 Domain Switch 失败时如何 fail closed。

这些已经超出了普通 SBI Extension 的接口定义。

## 13.5.4 `TEE_CALL` 和 OpenSBI Domain Switch 的关系

一个正确的抽象是：

```
TEE_CALL
    = 发起跨安全域通信的协议操作

Domain Switch
    = 为执行该操作提供隔离环境切换的实现机制
```

不是：

```
TEE_CALL == Domain Switch
```

为什么？

因为一次 `TEE_CALL` 可能因为 Endpoint 不存在、Service 不支持、没有可用执行资源等原因，被 Framework 直接拒绝。

这种情况下根本不需要真正进入 OP-TEE Domain。

另外，执行一个 TEE Command 期间也可能出现中断、RPC 或 Resume，并产生多个独立的执行转移。

因此一个高质量的实现通常需要分离：

1. Service Dispatch。
2. Endpoint Authorization。
3. Execution Context Management。
4. Hardware Isolation。
5. Result Marshalling。

这种模块边界在大型 SoC Security Firmware 中尤其重要。

# 13.6 Linux 如何通过 RPMI TEE_CALL 请求 OP-TEE？

这是本章非常关键的实际源码分析部分。

我们从 Linux 进程的行为开始，一直深入到 SBI ECALL。

## 13.6.1 Linux OP-TEE Driver 的职责

当 CA 调用：

```
TEEC_InvokeCommand(...);
```

libteec 通常使用 Linux TEE 子系统提供的接口，最终通过 OP-TEE Driver 执行请求。

对于 RISC-V，理想的软件分层是：

```
Userspace CA
    |
    | TEEC_InvokeCommand()
    v
libteec
    |
    | ioctl(/dev/teeX)
    v
Linux TEE Core
    |
    v
OP-TEE Common Driver
    |
    | Prepare optee_msg_arg
    | Session / SHM / RPC management
    v
OP-TEE RPMI Backend
    |
    | Encode control request
    | Reference RPMI Memory Parcel
    v
Linux RPMI TEE Transport
    |
    | TEE_CALL / Service UUID
    v
SBI MPXY Mailbox
    |
    | ECALL
    v
OpenSBI M-mode
    |
    v
OP-TEE Domain
```

这里有一个非常重要的架构思想：

Linux TEE Core 不应该知道一次 OP-TEE 调用最终是 SMC、FF-A 还是 RPMI。

它只关心：

- 打开一个 TEE Context。
- 建立 Session。
- Invoke Command。
- 注册与释放 Shared Memory。
- 返回操作结果。

这使 Linux 上层 API 可以保持稳定，而底层 Transport 不断演进。

## 13.6.2 Linux Driver 初始化需要完成什么？

在能够真正发送 TEE_CALL 之前，Driver 至少需要完成一组 Discovery 与 Capability Negotiation。

建议按以下逻辑理解：

```
Linux Boot
    |
    v
SBI MPXY Driver initialization
    |
    v
Setup per-hart MPXY SHMEM
    |
    v
Discover RPMI TEE Service Group
    |
    v
Validate RPMI protocol/version/capabilities
    |
    v
Discover TEE endpoint and service UUID
    |
    v
Initialize OP-TEE backend
    |
    v
Negotiate OP-TEE control ABI/features
    |
    v
Register /dev/teeX
```

这里每一层都可能失败。

例如：

- OpenSBI 不支持 MPXY。
- Channel 存在，但绑定的是 RPMI CLOCK Service Group。
- RPMI TEE Service Group 版本不兼容。
- 系统中没有 OP-TEE Endpoint。
- OP-TEE Endpoint 存在，但不支持期望的 Control ABI。
- Memory Sharing 能力不足。
- 当前固件不支持所需的同步调用或信号机制。

因此，成功发现 MPXY 不等于可以使用 OP-TEE。

## 13.6.3 2026 年 10 月 RFC v2 的架构变化

[RFC] 10 月 5 日的 OP-TEE Backend v2 将 RPMI 通用传输与 OP-TEE 专用逻辑分开。

Linux 侧模块大致分成：

```
drivers/mailbox/
    riscv-sbi-mpxy-mbox.c

drivers/firmware/riscv_rpmi_tee/
    bus.c
    discovery.c
    driver.c

drivers/tee/optee/
    rpmi_abi.c
    optee_rpmi.h
    call.c
    core.c
```

其中 10 月 7 日的 RPMI Transport v2 提出将 Endpoint/Service UUID 绑定到 RPMI TEE Service Bus，使不同的 TEE Service Driver 可以各自绑定对应服务。

[image](https://www.google.com/s2/favicons?domain=https://lists.infradead.org\&sz=32)

lists.infradead.org+1



架构责任因此更加清晰：

| 模块                  | 主要责任                                        |
| ------------------- | ------------------------------------------- |
| MPXY Mailbox Driver | MPXY Channel、Per-Hart 缓冲区、SBI 消息传输          |
| RPMI TEE Transport  | Service Discovery、Memory Parcel、Signal、同步调用 |
| RPMI TEE Bus        | Endpoint 与 Service Driver 绑定                |
| OP-TEE RPMI Backend | OP-TEE Control ABI、Session、RPC、SHM 参数       |
| OP-TEE Common Code  | 通用 TA Command、Call Queue、Session 等逻辑        |

这种分层很有价值，因为不同 TEE 实现可以复用 RPMI Transport，而不需要每一种 TEE OS 都重新实现 MPXY。

## 13.6.4 具体的消息发送过程

[RFC / DESIGN] 以 Linux 希望调用 OP-TEE Service 为例：

```
STEP 1
Linux OP-TEE Backend prepares command

    command buffer
    RPC buffer
    memory parcel references

STEP 2
RPMI Transport builds TEE_CALL request

    sender endpoint
    target endpoint
    service UUID
    service-specific payload

STEP 3
RPMI Transport selects MPXY channel

    associated with current hart/context

STEP 4
MPXY writes request to per-hart SHMEM

STEP 5
SBI MPXY FID 5

    ECALL → M-mode

STEP 6
OpenSBI processes RPMI message

    validate
    route
    switch execution domain

STEP 7
OP-TEE processes request

STEP 8
M-mode returns response through MPXY

STEP 9
Linux parses RPMI and OP-TEE results
```

这里最重要的代码路径可以概括为：

```
/* Teaching pseudocode, not upstream API */

int optee_rpmi_invoke(struct optee_call *call)
{
    struct rpmi_request req = { };
    struct rpmi_response rsp = { };

    /*
     * Encode OP-TEE control request.
     * Shared buffers are referenced through parcels.
     */
    build_optee_control_message(call, &req);

    /*
     * RPMI transport owns the MPXY details.
     */
    int rc = rpmi_tee_service_call(
        call->endpoint,
        call->service_uuid,
        &req,
        &rsp
    );

    if (rc)
        return rc;

    return decode_optee_control_response(
        call, &rsp
    );
}
```

请注意这个函数是解释接口边界的教学伪代码，不是声称 Linux upstream 中存在 `rpmi_tee_service_call()` 这个确切名称。

真正阅读 RFC v2 时，重点应放在 `rpmi_abi.c` 与 `drivers/firmware/riscv_rpmi_tee/` 之间的接口分工。

## 13.6.5 为什么同步 TEE_CALL 不能简单复用普通异步 Mailbox？

这是整个 Chapter 13 最值得深入的工业问题之一。

假设 Linux MPXY Mailbox Driver 内部这样做：

```
spin_lock_irqsave(&controller->lock, flags);

send_rpmi_tee_call();

spin_unlock_irqrestore(&controller->lock, flags);
```

而 `send_rpmi_tee_call()` 最终可能导致 OP-TEE 执行一个需要相当长时间的 TA Command。

这会产生严重问题。

问题 A：全局串行化。

如果该锁由全部 hart 共享：

```
Hart 0:
    lock
    enter OP-TEE
    execute long operation
    unlock

Hart 1:
    waiting for same lock
```

Hart 1 即使拥有独立的 MPXY SHMEM，也无法并行调用。

问题 B：长时间持有 Spinlock。

这会破坏 Linux 对锁持有时间的预期，导致系统延迟显著恶化。

问题 C：长时间屏蔽本地中断。

如果使用 `irqsave`，则在本 hart 的调用路径上，本地中断状态可能长期被限制。安全执行环境如果没有合理的 Foreign Interrupt/Yield 策略，会进一步放大实时性问题。

2026 年的 RFC 正是因为普通 Mailbox 提交路径可能在 Spinlock 下执行长时间的 TEE_CALL，才引入了直接同步发送机制。10 月的 Transport v2 进一步提出 `mbox_send_message_sync()`。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

OpenWall Lists+1



### 正确的设计思路

一个合理的方向是：

- 不在共享 Mailbox Controller Spinlock 下执行完整的 TEE Command。
- 将锁保护范围限制在必要的 Channel 状态操作。
- 对每 hart Buffer 的并发访问实施独立保护。
- 保证调用在适当的 CPU/Hart 上完成。
- 明确 IRQ、Preemption、RPC 和 Foreign Interrupt 的处理方式。
- 将长时间执行的 TEE Command 与短时间的消息传输操作区分处理。

但要注意：

不能因为希望减少中断延迟，就随意允许 Linux 在 ECALL 所属的 Per-Hart SHMEM 事务尚未完成时迁移或复用该缓冲区。

这就引出下一节。

# 13.7 Per-Hart MPXY Channel 与 Domain Context 的关系

这一节会把 MPXY 的通信模型与前面 Chapter 06、Chapter 17 的 Domain Context/SMP 知识串联起来。

## 13.7.1 为什么 MPXY 要使用 Per-Hart Shared Memory？

先看一个实际场景。

假设同一个 SoC 有四个 hart，Linux 中有四个不同的线程，同时发起 OP-TEE 调用：

```
Hart 0                     Hart 1
Linux Thread A             Linux Thread B
     |                          |
     v                          v
TEE_CALL A                 TEE_CALL B
     |                          |
     v                          v
OP-TEE Domain              OP-TEE Domain


Hart 2                     Hart 3
Linux Thread C             Linux Thread D
     |                          |
     v                          v
TEE_CALL C                 TEE_CALL D
     |                          |
     v                          v
OP-TEE Domain              OP-TEE Domain
```

如果所有 hart 使用同一个 MPXY Buffer：

```
Global MPXY Buffer
    |
    +-- Hart 0 writes request A
    +-- Hart 1 writes request B
    +-- Hart 2 writes request C
```

那么多个 hart 必然会争用相同内存。

可以增加一把 Global Lock，但是所有调用都被串行化，导致 SMP 扩展能力下降。

Per-Hart SHMEM 可以避免这一问题。

```
MPXY_SHMEM[0] → Hart 0
MPXY_SHMEM[1] → Hart 1
MPXY_SHMEM[2] → Hart 2
MPXY_SHMEM[3] → Hart 3
```

这意味着不同 hart 可以独立准备和发送消息。

但需要明确两件事：

第一，MPXY 规范要求 Per-Hart Shared Memory，并不要求 Channel ID 必须与 Hart ID 一一对应。

第二，Per-Hart Buffer 解决的是消息缓冲区的冲突，不自动保证 OP-TEE Kernel 本身具备完全的 SMP 并行执行能力。

例如 OP-TEE 可能仍然受 Trusted Thread 数量、TA Session Lock、Secure Storage Lock 或其他全局资源限制。

## 13.7.2 Per-Hart 的实际含义

RISC-V 中，一个 hart 通常拥有各自的：

```
Current Execution Context
Current Privilege Mode
CSR State
Trap State
satp
PMP Configuration
```

在传统同一 hart Domain Switch 模型中：

```
Hart 0:

Linux Context
    |
    | ECALL
    v
OpenSBI Context
    |
    | Domain Switch
    v
OP-TEE Context
```

整个调用都发生在 Hart 0。

因此 MPXY 的缓冲区必须和实际发出 ECALL 的 hart 一致。

例如：

```
/* Conceptual Linux code */

struct mpxy_per_hart {
    void *shmem_va;
    phys_addr_t shmem_pa;
    size_t shmem_size;
};
```

这不是 upstream 真实的数据结构，而是说明 per-hart buffer 与硬件执行上下文的关系。

在 Linux 中可以使用 Per-CPU 数据管理这类缓冲区，但必须确保 Linux CPU ID 与底层 Hart ID 的映射正确，且发送路径没有发生不被允许的 CPU Migration。

## 13.7.3 一个典型的 CPU Migration Bug

假设一个 Linux Kernel Thread 执行如下逻辑：

```
CPU 0:
    get_cpu()
    get MPXY_SHMEM[0]
    write request

    (thread preempted)

CPU 1:
    execute ECALL
```

此时：

```
Request Data → MPXY_SHMEM[0]

ECALL executes on Hart 1

OpenSBI reads MPXY_SHMEM[1]
```

结果 OpenSBI 看不到正确请求。

可能出现：

- 消息格式解析失败。
- 旧数据被重新处理。
- 请求被错误路由。
- 信息泄露。
- 系统死锁或崩溃。

因此，凡是使用 MPXY Per-Hart SHMEM 的代码，都必须确保：

> Request Buffer 的选择、填充、ECALL 执行和 Response 消费，与实际使用的 Per-Hart 状态保持一致。

### 如何解决？

一种设计是利用 CPU-local 临界区确保当前事务在同一个 CPU 上运行。

但不能简单将整个可能执行很长时间的 TEE Operation 都放进传统 Spinlock 临界区。

更合适的设计需要协调：

1. CPU Affinity 和 Preemption。
2. Per-Hart Buffer Ownership。
3. 本地中断对同一 Buffer 的访问。
4. TEE Yield / RPC 后的恢复。
5. CPU Offline / Suspend 期间的资源管理。

这里必须特别区分两种状态。

同一次 MPXY Request/Response Transaction

要求共享缓冲区和执行 hart 一致，并且在传输期间不能被错误覆盖。

同一个 TA Command 的多个 TEE_CALL Transaction

可能由于 RPC 导致多次进入和退出 OP-TEE。

是否允许下一次 Resume 在其他 hart 上执行，要由具体 OP-TEE Control ABI、Trusted Thread 状态和 Firmware Domain Context 的设计共同决定，不能仅根据 MPXY 的规范推导。

## 13.7.4 Per-Hart Buffer 与 Nested Call

考虑一个更困难的场景：

```
Hart 0
    |
    | Linux TEE_CALL
    v
OpenSBI
    |
    | OP-TEE executes
    v
Foreign Interrupt
    |
    v
Linux Interrupt Handler
    |
    | Another firmware request
    v
MPXY access
```

如果新请求覆盖了当前仍在使用的 MPXY Shared Memory，那么前一次请求的内容就可能损坏。

因此必须分析：

- 原请求是否已经完成 MPXY Message Transaction。
- Firmware 是否仍持有对缓冲区的引用。
- 中断处理是否会调用相同的 MPXY Interface。
- 是否允许在新的调用前临时替换 SHMEM。
- 是否需要保存旧 Buffer 状态。

SBI MPXY 定义了 `OVERWRITE` 和 `OVERWRITE-RETURN` 两种 SHMEM Setup 模式。

`OVERWRITE-RETURN` 可以在切换到新的 SHMEM 时，把原共享缓冲区物理地址返回给调用者，方便运行时固件等不同软件层临时使用 MPXY 后恢复先前配置。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



但是：

`OVERWRITE-RETURN` 并不自动提供事务级并发保护，也不能代替锁、生命周期管理或安全域访问控制。

它只提供共享内存配置状态的替换与恢复能力。

如果存在嵌套调用，就必须由更高层保证资源所有权和调用顺序。

## 13.7.5 TEE_CALL 与 Linux Interrupt Latency

还有一个更深的问题。

如果一个 TEE_CALL 在同一个 hart 上执行很长时间，那么此时 Linux 是否还能够及时处理 IRQ？

答案取决于：

- TEE 执行期间允许哪些中断。
- 中断由 M-mode、S-mode 或其他 Firmware 捕获。
- Foreign Interrupt 是否会导致 OP-TEE Yield。
- Firmware 是否能够暂时恢复 REE Domain。
- Linux IRQ Handler 执行完毕后如何恢复 TEE。
- 内核同步调用路径对中断、抢占和调度的限制。

这不是 MPXY 本身能够完全解决的问题。

例如，若为了保护 Per-Hart SHMEM，直接在 Linux 中长期禁用中断，那么即使 M-mode 具备 Domain Switch 能力，也可能难以满足正常系统的延迟要求。

反过来，若在 OP-TEE 执行期间允许 Foreign Interrupt 导致 REE 恢复，又必须确保：

- OP-TEE 安全上下文被完整保存。
- PMP/Firewall 正确恢复为 Linux Domain 状态。
- OP-TEE 的敏感执行状态不会暴露给 Linux。
- 再次进入 OP-TEE 时能够恢复原来的执行链。

这与 Chapter 16 的 Domain-Aware Interrupt Routing 紧密相关。

### 本节最重要的安全不变量

同一个 hart 上的安全调用应满足：

`Active MPXY Transaction` 与 Per-Hart Transport State 一致。

`Active Domain Context` 与当前 Hardware Protection State 一致。

`Resume Token / Trusted Thread Context` 与允许恢复的安全执行状态一致。

三者不能随意互换，更不能简单把 Channel ID、Hart ID 和 Trusted Thread ID 当作同一个值。

# 13.8 参数传递、返回值、错误处理与协议版本

这一节会进一步深入协议的二进制表示。

## 13.8.1 一次 TEE_CALL 到底携带哪些数据？

首先看 RPMI TEE_CALL 的标准化消息外壳。

[RFC / RPMI v2.0 开发规范] TEE_CALL Request 的数据布局如下：

| Word Offset | 字段                 | 大小       | 说明              |
| ----------- | ------------------ | -------- | --------------- |
| 0           | `SENDER_ID`        | 32 bits  | 发送方 Endpoint    |
| 1           | `TARGET_ID`        | 32 bits  | 目标 Endpoint     |
| 2～5         | `SERVICE`          | 128 bits | 目标 Service UUID |
| 6           | `SERVICE_DATA_LEN` | 32 bits  | 服务数据字节长度        |
| 7 起         | `SERVICE_DATA`     | N Bytes  | 服务专用 Payload    |

其二进制结构可以理解为：

```
Offset    Data
------------------------------------------------
0x00      SENDER_ID             4 bytes
0x04      TARGET_ID             4 bytes
0x08      SERVICE UUID         16 bytes
0x18      SERVICE_DATA_LEN      4 bytes
0x1C      SERVICE_DATA         variable
------------------------------------------------
```

这里的 `SERVICE_DATA` 是重要的扩展点。

RPMI Framework 并不要求所有 TEE 使用相同的内部 Command ABI。

例如：

```
SERVICE_DATA = OP-TEE specific control message
```

或者另一种 TEE 实现：

```
SERVICE_DATA = Vendor TEE specific message
```

Framework 只需要根据 Endpoint 和 Service UUID 正确路由消息。

对应 Response：

| Word Offset | 字段                | 大小      |
| ----------- | ----------------- | ------- |
| 0           | `STATUS`          | 32 bits |
| 1           | `SERVICE_RSP_LEN` | 32 bits |
| 2 起         | `SERVICE_RSP`     | N Bytes |

这些字段来自 RPMI TEE Service Group 的 `TEE_CALL` 定义。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 13.8.2 用一个实际例子理解三层参数

假设 Linux 要调用某个 OP-TEE Trusted Application 的 `CMD_SIGN_DATA`。

简化后的逻辑是：

```
CA Request
---------------------------------
TA UUID
Command = CMD_SIGN_DATA
Input Buffer = 1024 bytes
Output Buffer = 256 bytes
---------------------------------
```

Linux OP-TEE Driver 首先构造 TEE 命令参数。

之后 OP-TEE RPMI Backend 将它转换为具体的 Control Message，并通过 Memory Parcel 引用相关共享数据。

RPMI TEE_CALL 再将该 Control Message 包装为：

```
RPMI Request
---------------------------------
Sender Endpoint = Linux
Target Endpoint = OP-TEE
Service UUID = OP-TEE Service
Service Data Length = N
Service Data =
    OP-TEE Control Message
---------------------------------
```

最后 MPXY 负责传输。

可以把它理解为三层封装：

```
+----------------------------------------+
| MPXY                                    |
| channel_id, message_id, data_length    |
|                                        |
| +------------------------------------+ |
| | RPMI TEE_CALL                      | |
| | sender, target, service UUID      | |
| |                                    | |
| | +--------------------------------+ | |
| | | OP-TEE Control Message         | | |
| | | Invoke/Resume/SHM References   | | |
| | +--------------------------------+ | |
| +------------------------------------+ |
+----------------------------------------+
```

这里最值得注意的是：

1024 Bytes 的 TA Input Buffer 不一定需要全部放进 MPXY Shared Memory。

更合理的设计通常是通过 Memory Parcel 指向已经授权共享的内存区域。

MPXY 只承载相对较小的 Control Message。

这样既避免了频繁复制大数据，也避免 MPXY 传输 Buffer 成为大型 TA 操作的性能瓶颈。

## 13.8.3 详细分析 `TEE_CALL` 的消息编码

下面给出一个 [DESIGN] 教学版编码示例。

它仅展示 RPMI `TEE_CALL` 外层消息的构造方法，并不声称是当前 Linux RFC v2 的完整代码。

```
#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct tee_call_header {
    uint32_t sender_id;
    uint32_t target_id;
    uint8_t  service_uuid[16];
    uint32_t service_data_len;
};

/*
 * Wire format uses little-endian 32-bit fields.
 * The example assumes that the caller handles endian
 * conversion and bounds checks.
 */
```

假设：

```
sender_id       = 1
target_id       = 2
service_uuid    = 16-byte identifier
service_data_len = 64
```

则：

```
28 bytes RPMI TEE_CALL header
+
64 bytes OP-TEE control data
=
92 bytes Request Data
```

接下来将 Request Data 写入当前 hart 的 MPXY Shared Memory，然后执行：

```
/* Teaching-level API usage */

struct sbiret ret;

ret = sbi_mpxy_send_message_with_response(
    tee_channel_id,
    0x13,       /* RPMI TEE_CALL */
    92          /* Request data length */
);
```

其底层 SBI ECALL 寄存器约定是：

```
a7 = 0x4D505859   MPXY EID
a6 = 5            Send with Response
a0 = channel_id
a1 = 0x13         RPMI TEE_CALL
a2 = 92           Data Length

ecall
```

ECALL 返回：

```
a0 = SBI Error
a1 = Response Length
```

但不能根据 `a0 == SBI_SUCCESS` 就认为 TA 成功执行。

还必须继续解析 RPMI 和 OP-TEE 层的状态。

另外，真正实现 Encoder/Decoder 时，应使用明确的 `put_le32()`、`get_le32()` 或相应内核接口处理字段，不能仅通过 C 结构体强制类型转换来假定 Padding、Alignment 和 Endianness 都正确。

## 13.8.4 理解四层错误状态

这是很多 Firmware/Driver 开发者容易忽略的部分。

假设一次 TEE 请求返回：

```
SBI_SUCCESS
```

它仅表示 MPXY 消息传输成功完成并收到了响应。

它不代表：

```
RPMI_SUCCESS
```

更不代表：

```
TEEC_SUCCESS
```

我们需要逐层处理。

| 层次                 | 典型错误                     | 可能原因                    |
| ------------------ | ------------------------ | ----------------------- |
| SBI MPXY           | `SBI_ERR_NO_SHMEM`       | 当前 hart 未配置共享 Buffer    |
| SBI MPXY           | `SBI_ERR_TIMEOUT`        | 消息响应超时                  |
| SBI MPXY           | `SBI_ERR_NOT_SUPPORTED`  | Channel/FID/Message 不支持 |
| RPMI Framework     | `RPMI_ERR_INVALID_PARAM` | Endpoint/Service 参数无效   |
| RPMI Framework     | `RPMI_ERR_BUSY`          | 目标 Endpoint 忙           |
| RPMI Framework     | `RPMI_ERR_IO`            | TEE 处理失败或异常退出           |
| OP-TEE Control ABI | ABI-specific error       | 协议、消息类型或执行状态异常          |
| GP TEE API         | `TEEC_ERROR_*`           | CA/TA 操作结果错误            |

例如：

```
SBI_SUCCESS
    |
    v
RPMI_SUCCESS
    |
    v
OP-TEE Control Response = COMMAND_COMPLETED
    |
    v
TA Result = TEE_ERROR_ACCESS_DENIED
```

这表示：

- 消息运输成功。
- RPMI Framework 成功路由并完成事务。
- OP-TEE 正常执行了命令。
- 但 TA 拒绝了操作。

这是合法的业务失败，不能把它报告为 Mailbox I/O 错误。

反过来：

```
SBI_ERR_TIMEOUT
```

表示传输层没有得到预期的响应。

不能推断目标 TA 一定没有执行。

## 13.8.5 为什么 Timeout 后不能盲目重试？

考虑这样一个实际场景：

```
CA:
    Please increment secure monotonic counter.

Linux:
    Send TEE_CALL

OP-TEE:
    Counter incremented successfully.

Return Path:
    Response lost / timeout.
```

此时 Linux 只知道发生了 Timeout。

它不知道：

- OP-TEE 是否收到请求。
- 是否已经执行。
- 是否已经修改 Secure Storage。
- 是否已经把结果写入 Output Buffer。
- 是否正在等待 RPC。
- 是否发生了 Domain Recovery。

如果直接重试：

```
Counter incremented twice!
```

就可能破坏安全状态。

因此，必须区分两类操作：

幂等操作

例如纯粹查询 Firmware Version，重复执行通常不会改变系统状态。

非幂等操作

例如密钥生成、RPMB 写入、Counter Increment、TA 安全状态更新。

对非幂等操作，超时后的安全恢复需要基于协议保证、Transaction ID、请求去重、状态查询或经过设计的重置/恢复流程。

不能凭空认为再次发送相同 `TEE_CALL` 就是安全的。

这涉及分布式系统中经典的 at-most-once / at-least-once execution 语义，即使所有组件都运行在同一个 SoC 上，也同样存在。

## 13.8.6 协议版本应该如何协商？

这里至少有四个版本：

```
SBI Version
    |
    v
MPXY Extension and Channel Capabilities
    |
    v
RPMI Protocol / TEE Service Group Version
    |
    v
OP-TEE Control ABI Version
```

它们不应该被混为一谈。

例如：

```
SBI v3.0
    +
MPXY supported
    +
RPMI TEE SG supported
```

仍不能保证 Linux 与 OP-TEE 使用相同的 Control Message Format。

设想：

```
Linux Backend expects:
    OP-TEE RPMI Control ABI v2

OP-TEE OS implements:
    OP-TEE RPMI Control ABI v1
```

如果双方没有 Capability Negotiation，却直接解释二进制数据，则可能产生严重问题。

例如：

- 控制消息长度理解不一致。
- Resume Token 位置错误。
- Memory Parcel Reference 解码错误。
- 错误将不可信 Offset 作为物理地址。
- Unsupported Feature 被错误当作 Supported。

因此我建议在设计新的 OP-TEE Backend 时，明确规定：

1. 协议 Major/Minor Version。
2. 最低支持的功能集合。
3. 必选与可选特性。
4. Message Size 上限。
5. 向后兼容原则。
6. 未知字段和保留位的处理规则。
7. 版本不兼容时的 Fail-Closed 行为。

兼容性不能依赖“双方碰巧使用同一个源码分支”。

# 13.9 OP-TEE SMC ABI 与 RPMI Transport 的兼容问题

这是 Chapter 13 最有架构设计价值的一节。

它不仅解释当前为什么存在不同 RFC，还涉及如何为未来几年维护一个稳定的 TEE Backend。

## 13.9.1 先区分 ABI 与 Transport

我们回忆 Arm OP-TEE 的传统调用模型：

```
Linux
    |
    | OPTEE_SMC_CALL_WITH_ARG
    v
SMC ABI
    |
    | SMC
    v
Secure Monitor
    |
    v
OP-TEE
```

这里传统 SMC ABI 有两个特点：

1. 一部分操作通过寄存器交换控制数据。
2. 复杂的 TA 参数通过 OP-TEE Message Argument 和 Shared Memory 表达。

例如 `OPTEE_SMC_CALL_WITH_ARG` 并不意味着所有 TA 参数都放在 SMC 寄存器里。

很多实际参数存在 `optee_msg_arg` 及其关联的共享内存区域。

因此移植到 RISC-V 时有两种架构思路。

### 方案 A：保留 OP-TEE SMC ABI，替换底层 Conduit

```
Linux OP-TEE Driver
        |
        v
Existing SMC ABI
        |
        v
RISC-V RPMI Conduit
        |
        v
RPMI TEE_CALL
        |
        v
OP-TEE
```

这是 2026 年 9 月 10 日相关 RFC 的思路。

它把原来需要通过 SMC 寄存器表达的操作封装进 RPMI TEE_CALL 的 Service Data。

优点：

- 原有 OP-TEE SMC ABI 的大部分逻辑可以复用。
- 对既有 OP-TEE Driver 的改动相对集中。
- RPC 和传统 Shared Memory 模型的适配路径较直接。

但缺点也很明显：

- 需要定义额外的寄存器参数封装。
- 容易保留过多针对 Arm SMC 的设计假设。
- 与 RPMI 的 Memory Parcel 和 Service Discovery 模型结合不一定自然。
- 如果未来实现多个 TEE Backend，可能出现抽象层次不够清晰的问题。

注意这并不是说 SMC ABI 完全不可复用，而是需要权衡长期维护成本。

### 方案 B：建立独立 RPMI OP-TEE Control ABI

```
Linux TEE Core
        |
        v
OP-TEE Common Driver
        |
        +-------------------+
        |                   |
        v                   v
SMC Backend           RPMI Backend
                            |
                            v
                      RPMI Transport
                            |
                            v
                        SBI MPXY
```

这是 2026 年 10 月 5 日的 RFC v2 采用的方向。

它不再要求 RPMI Backend 模拟 Arm 的寄存器调用形式，而是设计适合 RPMI 的消息结构。

这种方向与 Arm FF-A Backend 的分层思想更接近，但不意味着 RPMI 与 FF-A 有相同的 ABI。

[image](https://www.google.com/s2/favicons?domain=https://lists.infradead.org\&sz=32)

lists.infradead.org



## 13.9.2 对比三种 TEE Transport

| 维度   | Arm SMC                 | Arm FF-A                    | RISC-V RPMI                     |
| ---- | ----------------------- | --------------------------- | ------------------------------- |
| 底层入口 | `SMC`                   | FF-A 调用机制                   | SBI MPXY / `ECALL`              |
| 通信对象 | Secure Monitor/TEE      | Secure Partition / Endpoint | TEE Endpoint / Service          |
| 服务发现 | 传统 OP-TEE SMC Discovery | FF-A Endpoint Discovery     | RPMI Endpoint/Service Discovery |
| 共享内存 | OP-TEE SHM              | FF-A Memory Management      | RPMI Memory Parcel              |
| 通知   | SMC/RPC 等机制             | FF-A Notification           | RPMI Signal                     |
| 安全隔离 | TrustZone               | TrustZone/SPMC 等            | 由 RISC-V 平台隔离机制保证               |
| 成熟度  | 成熟                      | 已有实际实现                      | 正在演进                            |

需要特别强调：

FF-A 的 Memory Handle 不应该被简单地当成 RPMI Memory Parcel ID 的同义词。

二者的生命周期思路可以借鉴，但权限语义、Nonce、消息布局和固件实现细节不能不经验证就直接复用。

## 13.9.3 Memory Parcel 与 OP-TEE Shared Memory

考虑一个具体例子。

Linux 希望向 OP-TEE 共享三页物理内存：

```
Page A
Page B
Page C
```

这三页可能并不是物理连续的。

在 RPMI 模型中，Linux 可以创建一个 Memory Parcel，描述它们的物理页列表以及需要授予目标 TEE 的权限。

```
Linux SHM
    |
    | Scatter-Gather Page List
    v
RPMI MEMORY_PARCEL_CREATE
    |
    | Creator
    | Receiver
    | Access Rights
    | Physical Memory Blocks
    | Nonce
    v
Memory Parcel ID
    |
    v
OP-TEE Control Message
    |
    | Parcel ID + Nonce + Offset/Size
    v
OP-TEE
```

具体生命周期：

```
REE creates parcel
        |
        v
Framework validates and applies policy
        |
        v
TEE accepts parcel
        |
        v
TEE maps and accesses memory
        |
        v
TEE releases parcel
        |
        v
REE reclaims parcel
```

需要特别理解：

- `CREATE` 不等于 TEE 已经完成 `ACCEPT`。
- `RELEASE` 不等于 REE 已经完成 `RECLAIM`。
- `SHARE`、`LEND`、`DONATE` 有不同的访问和所有权语义。
- `NONCE` 有助于减少 Parcel ID 重用引发的误匹配，但本身不应该被视为替代 Endpoint 权限验证的加密认证凭据。
- Framework 必须防止创建者授予接收者超出自身授权的权限。

RPMI 的内存管理规范详细规定了这些状态转换及权限控制要求。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



### 一个安全隐患：Parcel ID 重用

假设：

```
Parcel ID = 27
```

TEE 首次使用完成后，Linux 已经 Reclaim 并销毁了这个 Parcel。

随后 Framework 又创建了新的 Parcel，也获得 ID 27。

如果 OP-TEE 内部还缓存了旧的 Parcel Reference，就可能误用新的 Memory Parcel。

因此 RFC v2 使用了：

```
Parcel ID
+
Nonce
+
64-bit Offset
+
64-bit Size
```

来表达共享内存引用。

[image](https://www.google.com/s2/favicons?domain=https://lists.infradead.org\&sz=32)

lists.infradead.org



不过即使有 Nonce，也必须验证：

- Parcel 是否仍然有效。
- 当前 Endpoint 是否有权访问。
- Offset/Size 是否在 Parcel 范围内。
- 权限是否满足读写要求。
- 对应物理内存是否已经释放或重分配。
- 是否存在并发 Reclaim 与 TA 访问。

这实际上是典型的 Capability 和 Resource Lifetime 安全问题。

## 13.9.4 为什么需要 Opaque Resume Token？

假设执行过程如下：

```
TEE_CALL #1
    |
    v
OP-TEE Trusted Thread 5
    |
    | Needs RPC
    v
Suspend
    |
    v
Return to Linux
```

Linux 完成 RPC 后需要 Resume 原来的 Trusted Thread。

如果传入：

```
thread_id = 5
```

会有什么问题？

Linux 完全不可信。如果它任意修改 Thread ID，是否可能恢复其他 Session 的 Thread？

因此新控制协议应当把 Resume 设计成由可信侧管理的授权操作。

例如使用 Opaque Token：

```
Linux receives opaque resume token
        |
        v
Executes RPC
        |
        v
TEE_CALL with resume token
        |
        v
OP-TEE validates token and context
        |
        v
Resume original execution
```

这个 Token 至少需要在协议安全设计中明确：

- 绑定到哪个 Pending Call。
- 是否绑定到特定 REE Endpoint。
- 是否绑定到原 Session。
- 是否一次性使用。
- 是否有明确的有效期。
- 是否允许跨 hart Resume。
- Context Abort 后是否立即失效。
- 是否存在重放风险。

这里尤其重要的一点是：

Opaque 不自动等于 Secure。

Opaque 只表示调用者不应该解析其内部结构。

真正安全的 Resume 还依赖不可预测性或完整性保护、可信侧状态验证和严格的生命周期管理。

在某些设计中，Token 可以只是一个不透明的索引，但必须保证索引的查找和授权检查不可被不可信调用者绕过。

## 13.9.5 为什么 RFC v2 采用固定宽度 Little-Endian 消息？

这里实际上涉及跨架构 ABI 设计。

假设直接复用 C 结构：

```
struct optee_rpmi_request {
    unsigned long command;
    unsigned long param;
};
```

对于 RV64：

```
unsigned long = 64 bits
```

对于 RV32：

```
unsigned long = 32 bits
```

如果消息协议直接使用这种结构体，那么同一个 Protocol 在 RV32 和 RV64 平台上就可能形成不同的二进制布局。

这是一个严重的 ABI 设计问题。

因此，在独立 Control ABI 中，更合理的是使用：

```
uint32_t
uint64_t
uint8_t[]
```

等固定宽度类型，配合明确的：

- Endianness。
- Alignment。
- Version。
- Payload Length。
- Reserved Fields。
- Offset/Size Encoding。

这样消息的含义不会因为编译器或 XLEN 不同而变化。

2026 年 10 月 RFC v2 明确选择 Fixed-width Little-endian Control Messages，而不是简单复制 FF-A/SMC 的寄存器布局。

[image](https://www.google.com/s2/favicons?domain=https://lists.infradead.org\&sz=32)

lists.infradead.org



### 13.9 总结：新 ABI 应当稳定什么？

从架构设计的角度，我认为应该尽可能稳定：

```
GP TEE Client API
        |
        v
Linux TEE Core API
        |
        v
OP-TEE Common Semantics
```

而把需要适应不同平台的机制放在下层：

```
SMC / FF-A / RPMI Transport

Memory Handle / Memory Parcel

Endpoint Discovery

Notification and RPC Transport
```

但在实现时，应该根据真实的语义差异设计 Backend，而不是强制不同协议共享所有内部数据结构。

# 13.10 Transport 抽象的选择与版本演进

最后一节我们从 Staff Engineer 的视角考虑：

假设要为一款新的 RISC-V Server SoC 设计 OP-TEE Transport，怎样选择架构？

## 13.10.1 四种方案的对比

| 方案                   | 优点                        | 缺点                    | 建议         |
| -------------------- | ------------------------- | --------------------- | ---------- |
| 私有 SBI TEE Extension | 简单、可以快速 Bring-up          | ABI 私有、长期维护困难         | 适合已有平台兼容   |
| RPMI 封装 SMC ABI      | 复用已有 OP-TEE 逻辑            | 可能引入 Arm 特有假设         | 适合已有实现迁移评估 |
| RPMI 独立 Backend      | 结构清晰、支持 Service/Parcel 模型 | 初期实现和测试成本高            | 长期标准化方向    |
| 私有 Mailbox 直连安全 Core | 灵活、硬件适配直接                 | 平台依赖强、不天然具备标准 TEE ABI | 适用于特殊硬件体系  |

这里并不是说所有平台都应该立刻切换到 RPMI。

当前仍需要考虑：

- OpenSBI 固件版本。
- RPMI TEE Service Group 是否已经稳定。
- Linux Driver 和 OP-TEE OS 是否匹配。
- 实际硬件是否具备所需 Memory Isolation 能力。
- SoC 是否使用独立 HSM/安全处理器。
- 现有产品 ABI Compatibility 要求。
- 目标是工业产品还是 Upstream Reference Platform。

## 13.10.2 如何设计干净的 Transport Interface？

我建议把 OP-TEE 对 Transport 的依赖限制在一组清晰的抽象上。

以下是 [DESIGN] 教学接口：

```
struct tee_transport_ops {
    int (*discover)(void *ctx);

    int (*call)(void *ctx,
                const void *req,
                size_t req_len,
                void *rsp,
                size_t *rsp_len);

    int (*share_memory)(void *ctx,
                        const void *memory,
                        size_t size,
                        uint64_t *handle);

    int (*reclaim_memory)(void *ctx,
                          uint64_t handle);

    int (*setup_notification)(void *ctx);

    int (*shutdown)(void *ctx);
};
```

实际 Linux RPMI Transport RFC 的接口并不等同于这个结构。

这个例子主要用于说明设计原则：

Transport API 应该表达通信服务，而不是直接暴露 OpenSBI 的全部内部实现细节。

例如 Linux OP-TEE Backend 不应该需要知道：

```
Which PMP entry is programmed?

Which CSR stores the current domain?

Which M-mode stack is active?
```

这些都属于受信任 Firmware 的实现细节。

Linux 应该关心：

```
Which endpoint is available?

Which protocol is supported?

How do I send a TEE request?

How do I share/reclaim memory?

How do I handle RPC and notifications?
```

这就是合理的层次划分。

## 13.10.3 如何保证长期 ABI Compatibility？

对于工业级 SoC，我建议必须明确下面几类兼容性：

Source Compatibility

Linux Driver、OP-TEE OS、OpenSBI 的源码版本是否能一起编译。

Binary Compatibility

已构建的 Linux、Firmware 和 OP-TEE 镜像是否遵守一致的二进制消息协议。

Feature Compatibility

一个版本可能支持 Memory Share，却不支持 Memory Lend。

Security Compatibility

旧版 Firmware 即使能够处理消息，是否满足新版 Driver 所假定的内存隔离和生命周期约束。

Performance Compatibility

新版 Transport 是否引入过长的同步等待、Spinlock 持有时间、CPU Migration 限制或 RPC 调用开销。

尤其应该注意：

能够成功编译和启动，不代表协议版本正确，也不代表安全边界正确。

## 13.10.4 面向工业平台的推荐演进方式

假设已有 SoC 使用私有 SBI Extension 与 OP-TEE 交互。

我建议采用四个阶段推进。

1. 建立现有 ABI 基线

   固定 OpenSBI、Linux、OP-TEE 的版本，完整记录现有接口的命令编号、参数布局、共享内存、错误处理和 RPC 行为。
2. 建立 Transport Abstraction

   在不修改上层 TA 行为的前提下，将现有私有 SBI 实现封装为独立 Backend，并补齐回归测试。
3. 引入 RPMI Backend

   按照明确的协议版本和 Capability Negotiation 实现新 Backend，确保新旧 Transport 的运行模式可以独立验证，而不是在一次调用中混合两种不兼容的协议。
4. 验证、冻结和逐步迁移

   对比 xtest、RPC、Memory Sharing、SMP、异常恢复、启动时间、调用延迟和安全隔离，再决定是否弃用旧接口。

注意，支持双 Backend 不意味着应当让不可信 Linux 根据自己的喜好，动态选择较弱的安全路径。

对于安全敏感操作，应由可信启动配置和固件策略确定允许的 Transport 集合，避免出现 Downgrade Attack。

# 14. 结合实际源码：如何阅读当前 RISC-V OP-TEE Transport 实现？

这一部分作为 Chapter 13 的源码实训补充。

为了避免单纯根据协议画图，我建议按下面的三个代码仓库顺序研究。

## 14.1 OP-TEE OS：当前已存在的 RISC-V 代码

最重要的是：

[thread_optee_abi_rv.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_optee_abi_rv.S?utm_source=chatgpt.com)

该文件中能够看到 `thread_return_to_udomain()` 对 `CFG_RISCV_WITH_M_MODE_SM` 的处理，以及私有 `SBI_EXT_TEE` ECALL 返回路径。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



关键汇编逻辑可以概括为：

```
/* Existing upstream RISC-V path, excerpt */

li a7, SBI_EXT_TEE
li a6, 0
ecall
```

这段代码非常重要，因为它体现了早期 RISC-V OP-TEE Port 所采用的 M-mode Security Monitor 通信方式。

而当前正在讨论的 RPMI Backend 可能需要新的控制入口、返回和 Resume 机制。

你应该重点分析：

- `thread_return_to_udomain()` 如何返回非可信 Domain。
- 返回过程中 `a0`～`a5` 承载哪些值。
- RPC 期间 Thread Context 如何保存。
- 旧 `SBI_EXT_TEE` 路径是否可以与新的 RPMI Control ABI 共存。
- 新 Backend 如何避免把旧的 ECALL 返回假设硬编码到通用 Kernel 路径。

## 14.2 OP-TEE OS：SBI MPXY / RPMI 基础代码

继续阅读：

[sbi.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/sbi.c?utm_source=chatgpt.com)[sbi_mpxy_rpmi.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/sbi_mpxy_rpmi.c?utm_source=chatgpt.com)

当前 `sbi_mpxy_rpmi.c` 中的 `sbi_mpxy_rpmi_probe_channels()` 已实现 MPXY Shared Memory 大小查询、Channel 枚举、协议 ID 和 Channel Attribute 的读取等逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



例如源码中存在：

```
ret = sbi_mpxy_get_shmem_size(&mpxy_shmem_size);
```

以及 Channel Protocol 检查：

```
if (channel->attrs.msg_proto_id !=
    SBI_MPXY_MSGPROTO_RPMI_ID) {
    /* Ignore non-RPMI channels */
}
```

这部分可用于掌握 OP-TEE 自身调用 SBI MPXY/RPMI 服务的基础设施。

但必须明确：

OP-TEE OS 内已有 `sbi_mpxy_rpmi.c`，不等于 Linux → OP-TEE 的新 RPMI TEE Control ABI 已在 OP-TEE OS 完整实现。

这是两个不同的代码成熟度问题。

## 14.3 Linux：从 RFC 中学习实际架构设计

建议比较下面三组公开讨论：

| 时间         | 方案                                                                                                                          | 重点                                      |
| ---------- | --------------------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| 2026-09-10 | [SMC ABI over RPMI RFC](https://lists.openwall.net/linux-kernel/2026/09/10/82?utm_source=chatgpt.com)                       | 保留 SMC ABI 并添加 RPMI Conduit             |
| 2026-10-05 | [OP-TEE RPMI Backend v2](https://lists.infradead.org/pipermail/linux-riscv/2026-October/100709.html?utm_source=chatgpt.com) | 独立 Control ABI、Parcel、Resume Token      |
| 2026-10-07 | [RPMI TEE Transport v2](https://lists.infradead.org/pipermail/linux-riscv/2026-October/100918.html?utm_source=chatgpt.com)  | Service Bus、同步 Mailbox、Discovery、Signal |

阅读时，重点关注架构为什么发生变化，而不只是比较新增了哪些源文件。

这也是深入参与 upstream Review 的有效训练方式。

# 15. Hands-on：本章需要完成的四个实验

这些实验可以在 WSL2 中准备源码并分析实现。涉及完整 RPMI TEE_CALL 的端到端实验，需要一套实现了相应 RFC 协议的 OpenSBI、Linux 与 OP-TEE OS 匹配环境；不能假设普通发行版的 QEMU virt 镜像已经支持它。

## 实验 A：完整验证 SBI MPXY 的寄存器 ABI

目标：

用 QEMU + OpenSBI 或实际 RISC-V 平台，确认 MPXY Extension 是否存在。

建议观察：

```
a7 = SBI_EXT_BASE
a6 = SBI_EXT_BASE_PROBE_EXT
a0 = SBI_EXT_MPXY
ecall
```

检查 Extension Probe 的返回结果。

如果 MPXY 存在，再读取 Shared Memory Size。

用 GDB 观察：

```
Before ECALL:
    a0, a1, a6, a7
    sepc / sstatus

After ECALL:
    a0 = SBI Error
    a1 = SBI Value
```

验收： 能够明确解释 `ECALL` 的硬件行为，以及 MPXY 数据为什么不全部通过寄存器返回。

## 实验 B：分析 Per-Hart MPXY Buffer

基于具有多个 hart 的 QEMU/SoC：

- 在每个 hart 上设置独立的 MPXY Shared Memory。
- 观察 Channel Enumeration 结果。
- 分别发送支持的 RPMI 消息。
- 验证当前 hart 与 Buffer 的对应关系。
- 注入错误的 Channel ID、长度和未初始化 Shared Memory。

验收： 能解释 Per-Hart Buffer 与 Channel ID 的差异，并理解并发使用的约束。

## 实验 C：实现一个最小 RPMI TEE_CALL Encoder/Decoder

这是一个适合在 WSL2 中直接用 C 写的独立实验。

建立：

```
rpmi_tee/
    main.c
    encode.c
    decode.c
    test.c
```

需要实现：

```
encode_tee_call_request();
decode_tee_call_request();

encode_tee_call_response();
decode_tee_call_response();
```

要求覆盖：

- 16 Bytes Service UUID。
- Fixed-width Little-Endian Fields。
- 不同 Payload Length。
- 长度为零。
- 超出 Buffer 边界。
- 恶意构造的 `SERVICE_DATA_LEN`。
- Integer Overflow。
- 不正确的保留字段。
- 不兼容的协议版本。

特别注意这种检查不能写成：

```
if (header_size + data_len > buffer_size)
    return ERROR;
```

如果加法可能溢出，就会绕过检查。

更安全的形式是：

```
if (buffer_size < header_size)
    return ERROR;

if (data_len > buffer_size - header_size)
    return ERROR;
```

这里 `header_size` 必须来自协议定义，不能简单相信未经约束的 `sizeof(struct)`。

验收： 实现一个能够处理无效输入、可做 Fuzzing 的 RPMI TEE_CALL 消息解析器。

## 实验 D：分析一次真实 RFC 的锁与并发设计

研究 Linux 中：

```
drivers/mailbox/mailbox.c

drivers/mailbox/riscv-sbi-mpxy-mbox.c
```

以及 RFC 新增的同步调用逻辑。

设计如下测试场景：

```
Hart 0:
    OP-TEE call takes 100 ms

Hart 1:
    OP-TEE call takes 1 ms
```

观察如果使用全局 Spinlock，两次操作是否被串行化。

然后对比：

```
Global lock during TEE execution
```

与：

```
Per-Hart transport ownership
+
Minimal locking
+
Synchronous call execution
```

两种设计的差异。

验收： 能够说明在保证 Buffer/Domain 状态正确的前提下，怎样避免不必要的跨 hart 串行化，并设计中断延迟、并发和性能回归测试。

# 16. Staff Engineer 级面试题与参考答案

以下题目不是简单的概念记忆题。

它们主要考察你能否在真实 SoC 项目中分析安全边界、设计稳定 ABI、解决 SMP 并发问题，以及对 Firmware、Linux 和 TEE 的跨层故障进行定位。

## Question 01 — 为什么不能直接将 Arm OP-TEE 的 SMC 改成 RISC-V ECALL？

场景： 一个工程师提出，可以直接把 `smc #0` 替换成 `ecall`，原来的 `a0`～`a7` 参数定义全部保留，从而快速完成 RISC-V OP-TEE 移植。你如何评价？

参考答案：

这个方案只有在受控的私有调用约定下才可能成立，不能认为它天然兼容 RISC-V SBI。

首先，Arm SMC 与 RISC-V ECALL 的架构语义不同。SMC 配合 Arm Security State 和 Secure Monitor；ECALL 只是陷入更高特权处理环境的机制，不会自动建立或切换 TEE Domain。

其次，标准 SBI 使用 `a7` 表示 Extension ID、`a6` 表示 Function ID，并仅定义 `a0`、`a1` 的返回语义。不能将原有 OP-TEE SMC 返回寄存器约定原封不动套到 SBI 上。

最后，ECALL 进入 M-mode 后还需要明确：

- 如何识别并验证调用来源。
- 如何选择 OP-TEE Domain。
- 如何完成 Domain Context Switch。
- 如何恢复 Linux Context。
- 如何处理 RPC、Foreign Interrupt 和多 hart 并发。

因此不能仅从指令替换的角度评价移植工作。正确方法是先定义 Transport/Control ABI，再设计跨 Domain 的执行路径。

## Question 02 — 已经有 SBI，为什么还要引入 MPXY 与 RPMI？

场景： 项目经理认为 RPMI 使软件层次过于复杂，建议直接增加五六个私有 SBI TEE Function，减少开发成本。

参考答案：

应该区分短期 Bring-up 成本与长期 ABI 维护成本。

私有 SBI Extension 在已有固定硬件、固定 Firmware 的环境中可以迅速实现最小 TEE 通信，但长期存在以下问题：

- 与 RISC-V 标准 SBI Extension 体系的兼容性有限。
- 复杂数据依赖私有寄存器编码或额外 Shared Memory 协议。
- 新增 Endpoint、Memory Parcel、Signal、Discovery 等能力时需要不断扩展私有 ABI。
- 不同 Linux 版本和不同 TEE OS 之间难以复用。

MPXY 提供通用消息传输接口，RPMI 提供平台服务语义，OP-TEE Control ABI 再定义具体的 TEE 操作。

这种分层能够降低未来更换 OpenSBI 实现、增加安全服务或维护不同 TEE Backend 的成本。

但如果产品周期极短，并且现有私有接口已经验证稳定，也可以保留现有方案，同时将其隔离在独立 Backend 中，避免污染通用 Kernel 逻辑。

因此这是一个兼容性、标准化收益、性能与交付风险之间的工程决策，而不是简单的“层次越多越好”。

## Question 03 — 16-hart 系统中，为什么 Per-Hart MPXY SHMEM 仍然可能发生 Race Condition？

场景： 平台给每个 hart 分配了独立的 4KB MPXY Buffer，但 SMP Stress Test 仍然会偶发错误。

参考答案：

Per-Hart SHMEM 只解决不同 hart 写入同一个 Buffer 的一类冲突问题。

它不能自动解决同一个 hart 内多个执行上下文的资源重入问题。

需要重点检查：

CPU Migration

Linux Thread 是否在选择 Per-CPU Buffer 后迁移到了另一个 CPU，导致 ECALL 所使用的 SHMEM 与 Request Buffer 不一致？

Interrupt Reentrancy

是否有中断处理程序或运行时固件在同一个 hart 上重新配置或使用 MPXY SHMEM？

Concurrent Channel Use

是否存在多个调用者同时使用当前 hart 的 MPXY Buffer？

Nested Firmware Calls

原始请求尚未完成时，是否出现了新的消息操作，覆盖原有数据？

CPU Hotplug

CPU Offline/Online 是否改变了 Buffer 的生命周期或物理地址配置？

排查时应当在 Buffer 分配、消息提交和完成路径记录 CPU ID、Hart ID、Channel ID、Buffer Physical Address 和 Transaction Identifier。

注意不要记录 TA Secret 或其他敏感 Payload。

最终应建立明确的 Per-Hart Transaction Ownership 机制，并配套针对 CPU Migration、Interrupt 和 Hotplug 的压力测试。

## Question 04 — 一次 TEE_CALL 执行 100ms，导致 Linux 丢失时延要求，如何解决？

场景： 一款 16-hart Server SoC 中，某 TA 执行硬件加密操作需要 100ms。Linux 侧使用普通 Mailbox API 发送 TEE_CALL，其他 hart 的固件消息调用也出现明显延迟。

参考答案：

首先要分清两类延迟：

1. 当前 hart 上的 TEE Execution Latency。
2. 其他 hart 因不必要的共享锁而产生的延迟。

如果 TEE_CALL 在全局 Mailbox Spinlock 下执行，则必须将长时间的 TEE 执行从全局临界区移出，改用适当的同步发送接口，并保持 Per-Hart Buffer 的正确保护。

但这只能解决跨 hart 串行化问题。

当前 hart 长时间执行 TEE 时，还需要评估：

- 是否允许 Foreign Interrupt。
- 如何进行 Yield。
- Linux 是否需要短暂恢复执行。
- OP-TEE 是否支持安全的 Suspend/Resume。
- 是否存在中断被长时间屏蔽的问题。
- 是否需要将长耗时硬件操作拆分为多个阶段。

不能简单为了降低延迟就允许 Linux 随意抢占 OP-TEE，而不保存安全上下文。

性能验证应包括：

```
P50 / P95 / P99 TEE_CALL latency
IRQ latency
Cross-hart scalability
RPC frequency
Domain Switch overhead
Lock contention
```

同时评估安全性，因为任何中断优化都不能破坏 Domain Isolation。

## Question 05 — SBI 返回 `SUCCESS`，为什么 `TEEC_InvokeCommand()` 仍可能失败？

场景： Linux 日志显示：

```
SBI MPXY: SUCCESS
RPMI: SUCCESS
TEEC_InvokeCommand(): ACCESS_DENIED
```

工程师认为是通信协议发生了错误。

参考答案：

并不一定存在通信故障。

这是三个不同的结果层次。

`SBI_SUCCESS` 表示 MPXY 传输完成并收到了响应。

`RPMI_SUCCESS` 表示 RPMI Framework 完成了服务交互。

`TEEC_ERROR_ACCESS_DENIED` 表示 TEE/TA 拒绝了应用请求。

因此此时首先应检查 TA 的授权逻辑、Session 权限、参数合法性和相关安全策略。

只有当 SBI 或 RPMI 层返回错误时，才优先考虑 Channel、Endpoint、Transport、Firmware 或 Domain Switch 故障。

Staff Engineer 需要建立分层错误归属模型，而不是把所有非零结果都当作 Communication Failure。

## Question 06 — 为什么 OP-TEE RPC 不能被理解成普通的函数回调？

场景： 工程师试图在 `TEE_CALL` 的 M-mode Handler 内直接调用 Linux 的一个函数来完成文件系统操作。

参考答案：

这种思路混淆了两个独立执行 Domain 的运行模型。

Linux 和 OP-TEE 具有不同的地址空间、系统状态和安全边界。M-mode Firmware 也不应该把 Linux Kernel 内部函数视为可直接、安全调用的子程序。

正确模型是：

```
OP-TEE requests RPC
       |
       v
Suspend secure execution
       |
       v
Return control to REE
       |
       v
Linux processes RPC
       |
       v
Resume original trusted execution
```

其关键是 Trusted Thread 的 Suspension 与 Resume，而不是跨 Domain 的 C 函数指针调用。

还必须保护：

- Pending RPC 的归属。
- Trusted Thread State。
- Resume Token。
- RPC 参数和 Shared Memory。
- 失败及取消处理。
- 并发执行中的状态隔离。

这也是为什么 `TEE_CALL` 的同步消息语义，不能简单等同于整个 TA Command 生命周期。

## Question 07 — 如何防止伪造 RPMI Endpoint Identity？

场景： Linux 完全不可信。攻击者通过 Kernel Module 修改 `TEE_CALL` 的 `SENDER_ID`，声称自己是另一安全服务 Endpoint。

参考答案：

RPMI Framework 不能相信 Request 中声明的 Sender ID。

它应该从可信执行环境状态中获得当前调用者身份，并验证其是否与请求声明一致。

如果当前调用来自 Linux REE Domain，则其允许使用的 Endpoint Identity 应由 Framework 的可信 Domain/Endpoint Mapping 决定。

对 `TEE_CALL` 还应验证：

- 当前 Endpoint 是否允许访问 Target Endpoint。
- 请求的 Service UUID 是否对该 Endpoint 可用。
- 使用的 Memory Parcel 是否属于被授权的资源。
- 相关权限是否与当前执行 Domain 一致。

同理，TEE 不能仅仅根据 Linux 传递的 Parcel ID 就获得任意物理内存访问权。

认证调用来源和验证请求内容是两个独立步骤，缺一不可。

## Question 08 — Memory Parcel 已经 Reclaim，为什么 TA 仍能访问旧内存？

场景： 某平台在 Memory Parcel Reclaim 后，将对应物理页分配给其他 Linux 进程。但原 TA 仍能通过已有 VA 访问这些内存。

参考答案：

这是一个非常严重的 Shared Memory Lifetime 与硬件权限一致性问题。

应检查：

1. TEE 是否已经 Release 对应 Parcel。
2. Framework 是否确认允许 Reclaim。
3. 目标 Domain 的物理访问权限是否已经撤销。
4. OP-TEE 的 VA Mapping 是否已经失效。
5. 是否已经进行必要的 TLB Synchronization。
6. 是否存在其他 hart 仍保留旧 Mapping。
7. 相关 DMA/IOMMU 映射是否已经撤销。
8. Linux 是否在 Reclaim 完成之前提前复用了页面。

正确的生命周期应当保证：在物理页被重新分配给无关主体之前，先前获授权的 Endpoint 不再能通过旧 VA、TLB 或 DMA 路径访问该内存。

这种问题不能只靠更新 `Memory Parcel State` 的软件变量解决。

协议状态、页表状态和实际硬件隔离状态必须一致。

## Question 09 — 一次 TEE_CALL Timeout 后，如何保证 Secure Counter 不会被重复递增？

场景： TA 收到命令后成功递增了安全计数器，但返回响应时发生超时。Linux 自动重试导致 Counter 增加两次。

参考答案：

这是典型的执行结果不确定问题。

不能认为 Timeout 就代表原操作未执行。

建议首先明确协议的执行语义：

- 请求是否可能在超时后继续执行。
- 是否有 Transaction ID。
- 是否支持状态查询。
- 是否可以安全取消。
- 是否存在持久化的去重记录。
- 是否能够区分已提交和未提交的状态。

对非幂等操作，可以通过可信侧维护事务标识或提供查询最终状态的机制，避免重复提交。

如果硬件或协议无法提供足够的保证，就应该把执行状态报告为 Unknown，并由上层执行安全的恢复流程，而不是盲目重复发送。

这里体现了一个重要事实：

即使全部运行在一个 SoC 内，跨安全 Domain 的请求仍然具有分布式事务的一些基本性质。

## Question 10 — 一个新的 Linux RPMI Driver 可以启动，但无法与旧版 OP-TEE 通信，如何排查？

场景： Linux 升级为新 RPMI Backend 后，MPXY Channel Discovery 正常，但 OP-TEE 无法初始化 Session。

参考答案：

应该从下到上建立兼容性检查矩阵。

首先确认 SBI Extension、Channel 和 SHMEM 是否正常。

然后检查：

- MPXY Message Protocol ID。
- RPMI Service Group ID。
- Service Group Version。
- Endpoint 和 Service Discovery。
- OP-TEE Service UUID。
- Memory Sharing 能力。
- OP-TEE Control ABI Version。
- Request/Response Layout。
- Resume/Notification 能力。

尤其需要检查旧版 OP-TEE 是否仍使用私有 `SBI_EXT_TEE` 调用和返回方式。

因为 RPMI Channel 可用，并不意味着 OP-TEE 已经实现 RPMI Control ABI。

在这种情况下，正确行为应当是在 Capability Negotiation 阶段明确报告不兼容，而不是尝试按未知二进制布局发送消息。

## Question 11 — 如何设计安全的 Resume Token？

场景： OP-TEE 需要在 RPC 完成后 Resume 原来的 Trusted Thread。Linux 可任意篡改 Resume Token。

参考答案：

首先应把 Resume Token 视为不可信调用者持有的引用，而不是无条件可信的权限证明。

可信侧必须维护 Pending Call State，并在 Resume 时验证：

- Token 是否存在且有效。
- 是否属于当前 REE Endpoint。
- 是否与原有 Session/Command 匹配。
- 是否处于允许 Resume 的状态。
- 是否已被使用或取消。
- 是否允许当前 hart 执行 Resume。
- 是否存在并发 Resume。
- 是否存在 Token 重用。

如果使用可预测的 Handle，那么更需要严格的可信侧授权检查和 Generation Counter。

若设计采用 Token 作为 Bearer Capability，则还必须保证其不可被无授权主体猜测、伪造或重放。

重要的是不能仅靠：

```
thread = thread_table[token];
resume(thread);
```

就恢复 Trusted Thread。

这会把不可信数据直接转化为安全执行控制权。

## Question 12 — 同一个 SoC 同时存在 OP-TEE 和独立 HSM Core，RPMI 应如何设计？

场景： Application CPU 运行 Linux + OP-TEE，另一颗独立 RISC-V HSM Core 负责 Secure Boot、Key Management 和 Crypto Service。

参考答案：

首先要明确不同执行实体的职责和信任边界。

例如：

```
Application Harts
    |
    +-- Linux REE
    |
    +-- OP-TEE Domain
           |
           | Secure Service Request
           v
        HSM Interface
           |
           v
Dedicated HSM Core
```

这里至少存在两种不同的通信关系：

Linux → OP-TEE

可以采用 RPMI TEE Service Group over SBI MPXY，并通过 OpenSBI Domain 切换执行。

OP-TEE → HSM

可以采用安全 Mailbox、专用 RPC、共享队列或平台定义的其他接口。

不能假定 HSM Core 自然属于 MPXY Per-Hart Domain Switch 的目标。

HSM 可能是独立物理处理器，其固件、地址空间、Interrupt、Reset 和 DMA 隔离机制都不同。

因此需要单独设计：

- HSM Endpoint Identity。
- IPC 消息认证与授权。
- Key Handle 的访问控制。
- 请求重放保护。
- HSM Reset/Timeout 处理。
- 并发请求排序。
- OP-TEE 与 HSM 的信任关系。

架构上应避免让 Linux 直接绕过 OP-TEE/HSM 授权策略访问敏感 Key Operation。

## Question 13 — RISC-V Hypervisor 加入后，TEE_CALL 的安全模型会如何变化？

场景： 原有系统仅有 Linux 与 OP-TEE 两个 Domain。后来平台引入 H Extension 和多个 Linux Guest VM。

参考答案：

关键变化是调用者不再只有一个简单的 REE。

需要区分：

```
Physical Hart
Hypervisor Context
Guest VM Identity
REE Endpoint
TEE Endpoint
Memory Parcel
```

不能将所有 Guest VM 的 TEE_CALL 都视为同一个可信客户端。

可能需要由 Hypervisor 或 Firmware 对 Guest 的消息进行代理和隔离。

必须解决：

- Guest VM 如何获得合法的 TEE Service。
- Endpoint Identity 如何绑定 Guest。
- Guest Physical Address 如何转换及验证。
- Memory Parcel 是否属于特定 VM。
- Resume Token 是否绑定 VM Identity。
- VM Reset/Destroy 时如何释放未完成的 TEE 调用。
- 一个 VM 是否可能影响另一个 VM 的 TA Session。
- Device Assignment 和 DMA 是否绕过内存隔离。

这里的核心原则是：

虚拟化不能削弱原有 TEE 安全边界。

即使多个 Guest 共享一个 OP-TEE Endpoint，也必须在 TEE Control ABI 或服务授权层维护足够的调用者隔离信息。

## Question 14 — 如何评估采用私有 SBI TEE Extension 还是标准化 RPMI Backend？

场景： 你担任一个 RISC-V Server SoC 的 Security Software Staff Engineer。芯片距离流片还有六个月，现有私有 SBI TEE 通道已经能够运行 OP-TEE，而新的 RPMI Backend 还处于 RFC 阶段。团队希望你决定是否立即迁移。

参考答案：

这不是纯粹的技术偏好问题，需要从交付风险和系统演进两个维度评估。

首先建立目前的交付基线：

- 私有 SBI TEE ABI 是否已经稳定。
- 是否运行完整 xtest。
- RPC、Shared Memory、SMP 是否验证通过。
- 是否存在安全架构缺陷。
- 现有代码能否通过架构清晰的 Backend 封装。

再评估 RPMI：

- RPMI v2.0 TEE Service Group 是否稳定。
- Linux Transport 是否已经进入可合并状态。
- 对应 OP-TEE OS 端是否已实现。
- OpenSBI 是否支持所需的 Domain 和 Memory Parcel 机制。
- 硬件是否支持 Parcel 权限动态变化。
- 是否存在明确的 Upstream 维护方。
- 回归测试是否完整。

在这种时间约束下，我倾向于保留已经验证的私有 SBI ABI 作为流片前基线，同时将其抽象到独立 Transport Backend，持续跟进 RPMI Upstream。

新的 RPMI Backend 可以在独立分支和 QEMU/仿真环境中推进，而不是直接替换关键交付路径。

只有在接口、Firmware 实现、Linux Driver、OP-TEE OS 和系统测试都达到可接受水平之后，再进行受控迁移。

这既不否定标准化，也能避免使用尚在变化的 RFC 阻塞 SoC Bring-up。

# 17. 本章知识验收

完成 Chapter 13 后，建议检查自己是否真正掌握以下能力：

Chapter 13 Mastery Checklist

0/10

能够解释 ECALL、SBI、MPXY、RPMI 和 OP-TEE Control ABI 的层次关系

能够从 RV64 a0–a7 寄存器追踪一次 MPXY 同步调用

能够正确解析 RPMI TEE_CALL Request 和 Response 布局

能够区分 Hart、Domain、MPXY Channel、Endpoint 和 Service UUID

能够解释 Per-Hart SHMEM、CPU Migration 和 Nested Call 的并发风险

能够区分 MPXY SHMEM、OP-TEE SHM 和 RPMI Memory Parcel

能够设计 RPC Suspend/Resume、Token 验证和异常恢复流程

能够独立评审 Linux Mailbox 中长时间同步调用带来的锁与中断风险

能够识别四层错误状态及非幂等操作的 Timeout 风险

能够比较 SMC Conduit 与独立 RPMI Backend 的架构取舍

重置进度

## 18. 本章最重要的六个结论

第一，ECALL 不等于 TEE Domain Switch。

ECALL 只负责特权陷入；是否切换 Domain，以及如何完成安全切换，是 M-mode Firmware 的责任。

第二，MPXY 不等于 RPMI。

MPXY 负责消息代理与传输，RPMI 定义平台服务的协议语义。

第三，`TEE_CALL` 不等于一次完整的 TA Command。

同一个 TA Command 可能因为 RPC、Foreign Interrupt 或其他 Yield 行为而跨越多次 `TEE_CALL` Transaction。

第四，Per-Hart SHMEM 不等于没有并发风险。

CPU Migration、Interrupt Reentrancy、Nested Call、Hotplug 和 Transport Ownership 仍然需要严格设计。

第五，Memory Parcel 不等于普通 Shared Memory Pointer。

它涉及 Endpoint Identity、物理内存权限、资源生命周期以及实际硬件隔离状态。

第六，Transport 成功不等于 TA 成功。

必须独立处理 SBI、RPMI、OP-TEE Control ABI 和 GP TEE API 的错误状态。

最后，再强调一个贯穿整套课程的核心思想：

> RISC-V OP-TEE 的安全性不来自某条特殊调用指令，而来自可信固件对执行环境、内存权限、Endpoint 身份和跨域状态转换的统一管理。SBI MPXY 与 RPMI 为这种架构提供标准化的通信契约。

这也是 Chapter 13 与之前 Domain Isolation、Context Switching、Trusted Thread、MMU 章节之间最重要的联系。

下一章 Chapter 14 将继续深入 Shared Memory & RPC Architecture，重点研究 `tee_shm`、`mobj`、Memory Parcel、`tee-supplicant`、Cache Coherency、TOCTOU，以及一次 RPC 如何在保持 Trusted Thread 安全状态的情况下返回 Linux 并最终恢复执行。
