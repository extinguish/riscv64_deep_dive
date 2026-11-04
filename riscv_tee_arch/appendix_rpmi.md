# RISC-V RPMI：从体系架构到 OpenSBI 源码与工业实践

RISC-V Platform Management Interface · 系统架构专题

## 一、先建立整体认识：RPMI 究竟是什么？

RPMI（RISC-V Platform Management Interface）是 RISC-V 为应用处理器（Application Processor, AP）与平台管理微控制器（Platform Microcontroller, PuC）之间定义的一套标准化消息通信接口。

它主要用于解决服务器 SoC 中的：

- Hart 启动、停止与状态管理（HSM）
- 系统复位、关机、挂起与恢复
- CPU 性能、频率、电压及功耗管理
- 设备电源管理
- 系统管理、RAS（Reliability、Availability、Serviceability）等任务

RPMI v1.0 已于 2025 年 7 月 16 日正式批准（Ratified）。它属于 RISC-V 平台软件接口规范，而不是一组新增的 RISC-V CPU 指令。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org+1



理解 RPMI，我建议首先记住这个关系：

SBI 主要解决 OS 与 M-mode Firmware 的标准化交互；RPMI 主要解决 AP 与平台管理控制器之间的标准化交互。

两者能够结合，形成从 Linux 一直到 SoC 底层电源、复位控制硬件的完整管理调用链。

## 二、为什么 RISC-V 需要 RPMI？

### 2.1 从一个 16-Hart 的服务器 SoC 开始

假设我们设计一颗 RISC-V 服务器芯片：

- 16 个 RV64 Application Harts
- 每个 Hart 都支持 M/S/U privilege modes
- M-mode 运行 OpenSBI
- S-mode 运行 Linux
- 另有一颗独立的 RISC-V 管理微控制器，负责电源、时钟和复位控制

从软件角度看，Linux 可能希望启动 Hart 8。

但是这里存在一个关键问题：

Hart 0 上的 OpenSBI，如何真正让处于断电状态的 Hart 8 启动？

OpenSBI 是软件，并不意味着它天然能够直接控制 SoC 中所有的电源域、Clock Gate 和 Reset Controller。

在某些 SoC 上，OpenSBI 可以直接通过 MMIO 操作硬件控制器。

在另一些 SoC 上，这些硬件资源只允许独立的管理微控制器访问。

这时就需要一套标准化协议，让 OpenSBI 向管理控制器发起请求。

### 2.2 引入 RPMI 之后的架构

示例架构：SBI 与 RPMI 的配合（响应沿反方向返回）

现在调用过程就非常清晰了：

1. Linux 希望启动 Hart 8，调用 SBI HSM 扩展。
2. `ecall` 进入 OpenSBI。
3. OpenSBI 根据平台实现，向 RPMI Server 发送 HSM 请求。
4. PuC 操作真正的硬件电源、时钟和复位控制器。
5. Hart 8 被释放并进入相应启动流程。
6. OpenSBI 按照 SBI HSM 协议完成 Hart 启动状态管理。

这里需要特别强调：

RPMI 并不是 OpenSBI 的子模块，也不是 OpenSBI 的替代品。

RPMI 是独立的接口规范。OpenSBI 可以实现 RPMI Client，而独立管理固件实现 RPMI Server。事实上，RPMI 也允许其他特权级的软件客户端通过相应通道访问允许的服务。

它甚至不要求管理控制器必须是 RISC-V 架构；规范关注的是协议和服务，而不是 PuC 使用哪一种 CPU ISA。

## 三、RPMI 的核心架构：Transport、Protocol、Service

从规范设计角度看，RPMI 可以拆成三个相互独立的部分。

RPMI Service Groups

HSM / Reset / Suspend / Clock / CPPC / Power

定义具体提供什么功能

RPMI Messaging Protocol

Request / Acknowledgement / Notification

定义如何编码请求、响应和事件

RPMI Transport

Shared Memory / Queue / Doorbell

定义如何真正传输消息

这里的设计思想与网络协议栈相似：Transport 不需要理解某个 Hart 为什么要启动；Service 不需要关心底层消息是通过 SRAM 还是其他方式传输。

### 3.1 RPMI Transport：共享内存通信机制

RPMI v1.0 标准化的是 Shared Memory Transport。

其核心并不是简单地约定一块共享内存，而是定义了一套完整的双向消息队列机制。

AP / OpenSBI

Application Processor

PuC / Firmware

Platform Microcontroller

A2P REQ

AP → PuC

Request Queue

P2A ACK

PuC → AP

Response Queue

P2A REQ

PuC → AP

Request / Notification Queue

A2P ACK

AP → PuC

Response Queue

A2P 通道必须实现；P2A 通道可选。每个逻辑通道由请求队列和响应队列组成。

这种机制支持两类场景：

场景 A：AP 主动请求。 OpenSBI 请求 PuC 关闭 CPU 电源，PuC 处理完之后返回状态。

场景 B：PuC 主动通知。 管理控制器检测到某个系统事件，通过 P2A 通道通知 AP，例如由相应服务定义的管理事件。

要注意，P2A 不仅可以发送 Notification，也可以发送需要 AP 响应的 Request。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org+1



### 3.2 为什么有 Shared Memory，还需要 Doorbell？

Shared Memory 解决数据交换，Doorbell 解决通知问题。

例如，OpenSBI 已经将 RPMI Request 写入 SRAM，PuC 怎么知道来了新消息？

一种实现是 PuC 不断轮询消息队列。

另一种实现是 AP 写一个专用的 MMIO Doorbell 寄存器，让 PuC 收到中断。

典型实现逻辑如下：

```
/* 示意代码：忽略了错误处理和锁 */
write_request_to_shared_memory(req);

/* 保证消息内容在 tail 更新之前可见 */
memory_write_barrier();

update_queue_tail();

/* 通知 PuC：队列中存在新消息 */
writel(doorbell_value, A2P_DOORBELL_REG);
```

从源码角度，OpenSBI 的 `fdt_mailbox_rpmi_shmem.c` 中的 `__smq_tx()` 就具有类似流程，包括消息拷贝、`smp_wmb()`、tail 更新和 Doorbell 写入。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



注意：Doorbell 是通知手段，不承担实际消息数据传输；规范允许接收端使用轮询机制。

### 3.3 Shared Memory Queue 的内部设计

RPMI 将每个 Queue 划分成若干个固定大小的 Slot。

例如，假设：

- Slot Size = 64 Bytes
- Queue Total Slots = 8
- Queue Size = 512 Bytes

其布局可以理解为：

Head

64 B

Tail

64 B

Msg 0

64 B

Msg 1

64 B

Msg 2

64 B

Msg 3

64 B

Msg 4

64 B

Msg 5

64 B

Head 和 Tail 分别占一个 Slot，剩余六个是消息 Slot。

这里有几个对 Firmware Engineer 非常重要的细节。

第一，Head 和 Tail 由不同的角色修改。

Producer 修改 Tail，Consumer 修改 Head，避免两个处理器同时写同一个队列指针。

第二，队列必须处理缓存一致性。

因为 AP 和 PuC 不一定处于同一个 Cache Coherency Domain，因此不能默认两侧缓存一致。RPMI v1.0 要求共享内存至少具备双方一致可见、非缓存的 I/O 类型访问属性，数据采用 Little Endian。

第三，Slot 必须对齐。

Slot Size 必须是 2 的整数次幂，最小 64 Bytes。Head 与 Tail 使用独立 Slot，也有利于避免两者共享 Cache Line 造成的维护问题。

第四，实际可用队列容量不等于消息 Slot 数量。

规范规定：

\\[ Empty: head=tail \\]

\\[ Full:(tail+1)\bmod N=head \\]

其中 \\(N=M-2\\) 为消息 Slot 数量。采用这种保留一个空位的环形队列设计，上述六个消息 Slot 实际最多容纳五条未消费消息。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



对于 16-Hart 的 SoC，这直接涉及多个 Hart 同时发送 RPMI 请求时的并发、队列容量和超时设计。

## 四、RPMI Messaging Protocol：消息长什么样？

RPMI 的消息包含固定 8 Bytes Header 和可变长度 Data。

RPMI Message Header（64 bits）

FLAGS

8 bits

SERVICE_ID

8 bits

SERVICEGROUP_ID

16 bits

TOKEN

16 bits

DATALEN

16 bits

Message Data（Variable Length）

Request Parameters / Status / Response

各字段意义如下：

| 字段              | 作用                         |
| --------------- | -------------------------- |
| FLAGS           | 消息类型及 Transport 标志         |
| SERVICEGROUP_ID | 功能组编号，例如 HSM = `0x0005`    |
| SERVICE_ID      | 功能组内的具体操作                  |
| TOKEN           | 关联请求与响应                    |
| DATALEN         | Payload 的字节长度，按 4 Bytes 对齐 |

RPMI 定义四种实际消息形式。

| 消息              | 含义                   |
| --------------- | -------------------- |
| Normal Request  | 请求后需要 ACK            |
| Posted Request  | 发送请求，无须 ACK          |
| Acknowledgement | 请求处理结果               |
| Notification    | PuC 主动发送的异步事件，无须 ACK |

特别值得关注的是 `TOKEN`。

假设 Hart 0 和 Hart 1 同时向同一个 RPMI Channel 发送 HSM 请求。响应到达的顺序可能不同于请求顺序，因此客户端需要通过 Token 判断某条 ACK 对应哪条 Request。

在工业实现中，Token 的生命周期、共享 Queue 的锁以及超时后迟到的 ACK 都需要谨慎处理。

另外，RPMI ACK 的 Payload 第一个 32-bit Word 必须是 `STATUS`，错误码包括 `RPMI_SUCCESS`、`RPMI_ERR_BUSY`、`RPMI_ERR_DENIED`、`RPMI_ERR_TIMEOUT` 等。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



## 五、RPMI Service Groups：究竟能够管理哪些资源？

这是 RPMI 真正提供平台能力的部分。

以下是已批准的 RPMI v1.0 Service Groups：

| Group ID | Service Group         | 主要职责               |
| -------- | --------------------- | ------------------ |
| `0x0001` | BASE                  | 版本、能力发现            |
| `0x0002` | SYSTEM_MSI            | 系统 MSI 配置与管理       |
| `0x0003` | SYSTEM_RESET          | 系统复位、关机            |
| `0x0004` | SYSTEM_SUSPEND        | 系统挂起               |
| `0x0005` | HART_STATE_MANAGEMENT | Hart 启停、休眠         |
| `0x0006` | CPPC                  | CPU 性能控制           |
| `0x0007` | VOLTAGE               | 电压管理               |
| `0x0008` | CLOCK                 | 时钟管理               |
| `0x0009` | DEVICE_POWER          | 设备电源管理             |
| `0x000A` | PERFORMANCE           | 性能域管理              |
| `0x000B` | MANAGEMENT_MODE       | 固件 Management Mode |
| `0x000C` | RAS_AGENT             | RAS 管理             |
| `0x000D` | REQUEST_FORWARD       | 请求转发               |

其中只有 BASE 是必选的，其余 Service Groups 都是可选的。

需要注意，官方 GitHub 主分支还在推进 RPMI 2.0 的草案，包括 TEE、Logging、System IRQ 等新 Service Groups。这些不能直接当作已批准的 v1.0 能力。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org+1



### 5.1 HART_STATE_MANAGEMENT：最适合从 OpenSBI 理解的 Service Group

这是我建议优先深入的模块。

RPMI HSM 定义的主要操作包括：

| Service                 | 用途            |
| ----------------------- | ------------- |
| `HSM_GET_HART_STATUS`   | 查询 Hart 状态    |
| `HSM_GET_HART_LIST`     | 查询受管理的 Hart   |
| `HSM_GET_SUSPEND_TYPES` | 查询支持的休眠模式     |
| `HSM_GET_SUSPEND_INFO`  | 获取休眠延迟、功耗相关属性 |
| `HSM_HART_START`        | 启动 Hart       |
| `HSM_HART_STOP`         | 停止当前 Hart     |
| `HSM_HART_SUSPEND`      | 进入低功耗状态       |

这里有个很重要的区别：

Hart Stop 不等同于物理断电。

对于 RPMI 来说，Stopped 表示该 Hart 不再执行指令，底层可以采用 Power Gating，也可以使用支持的深度低功耗状态。

同样，Hart Suspend 可以是 Retentive，也可以是 Non-retentive。

在 Non-retentive Suspend 中，处理器部分上下文可能丢失，需要依赖指定的 Resume Address 完成恢复。

这些状态定义与 SBI HSM 保持协调，避免 OS、OpenSBI、PuC 三方对处理器状态产生不同理解。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



### 5.2 CPPC 与 PERFORMANCE：为什么需要两套性能管理机制？

CPPC 即 Collaborative Processor Performance Control。

它提供与 CPU 性能控制相关的抽象寄存器访问，便于操作系统按照性能目标进行控制，而不必直接理解某个 CPU 的 PLL 或电压寄存器。

PERFORMANCE 则面向更一般化的 Performance Domain 管理。

例如一个 CPU Cluster 由四个 Hart 组成，四个 Hart 可能共享 PLL。

即使 OS 希望 Hart 0 提高频率，实际上也可能必须调整整个 Cluster 的性能等级。

这就涉及 Performance Domain：

\\[ PerformanceDomain \neq Hart \\]

另外，RPMI 还定义了 Fast Channel，以避免频繁的性能调整都经过完整 Request/ACK 消息路径。Fast Channel 的共享内存格式不同于常规 RPMI 消息格式，适合对交互开销敏感的性能控制。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



## 六、结合 OpenSBI 源码：一次真正的 Hart Start

这一部分是理解 RPMI 的关键。

我们直接结合当前 OpenSBI 上游源码来分析，而不只是停留在架构图。

### 6.1 Linux 调用 SBI HSM

在 Linux 需要启动 Hart 8 时，可以抽象为：

```
sbi_hart_start(
    8,                  /* target hart */
    linux_entry_addr,   /* S-mode entry */
    opaque
);
```

这会通过 SBI `ecall` 进入 M-mode OpenSBI。

随后 OpenSBI 的 `sbi_hsm_hart_start()` 处理目标 Hart 的启动。

### 6.2 OpenSBI 先维护自己的 HSM 状态

关键源码位于：

[OpenSBI — lib/sbi/sbi_hsm.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hsm.c?utm_source=chatgpt.com)

在 `sbi_hsm_hart_start()` 中，OpenSBI 不会直接将 Linux 提供的 S-mode 地址交给 RPMI。

它首先检查目标 Hart、所属 Domain、启动地址及当前状态。

然后为目标 Hart 设置 per-Hart Scratch Context：

```
rscratch->next_arg1 = arg1;
rscratch->next_addr = saddr;
rscratch->next_mode = smode;
```

接着通过原子状态转换将 Hart 变为 `START_PENDING`。

这里就体现出你之前学习的 `sbi_scratch` 的价值。

`next_addr` 保存将来需要进入的 S-mode 地址，而 `warmboot_addr` 是 OpenSBI 自己的 M-mode 恢复入口。

这两个地址的职责完全不同。

### 6.3 OpenSBI 调用 RPMI HSM 驱动

底层驱动的实际位置是：

[OpenSBI — fdt_hsm_rpmi.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/utils/hsm/fdt_hsm_rpmi.c?utm_source=chatgpt.com)

在 `rpmi_hsm_start()` 中，可以看到核心操作：

```
req.hartid = hartid;
req.start_addr_lo = resume_addr;
req.start_addr_hi = (u64)resume_addr >> 32;

return rpmi_normal_request_with_status(
    rpmi->chan,
    RPMI_HSM_SRV_HART_START,
    &req, rpmi_u32_count(req), rpmi_u32_count(req),
    &resp, rpmi_u32_count(resp), rpmi_u32_count(resp));
```

这是从上游实现摘取的核心代码逻辑。

最值得注意的是 `resume_addr`。

OpenSBI 的 `sbi_hsm_hart_start()` 在需要执行底层 Hart 启动时，调用的是：

```
hsm_device_hart_start(
    hartid,
    scratch->warmboot_addr
);
```

也就是说，RPMI Server 收到的启动地址通常是 OpenSBI 的 M-mode Warmboot Entry，而不是 Linux 的 S-mode Entry。&#x20;

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



这点非常关键！

### 6.4 PuC 如何真正启动 Hart 8？

下面是一个 SoC 级的示意实现，并非 RPMI 规范要求的固定硬件序列：

```
int platform_hart_start(
    uint32_t hartid,
    uint64_t start_addr)
{
    /* 1. 验证 hartid 和启动地址 */

    /* 2. 打开目标 CPU 电源域 */

    /* 3. 使能目标 Cluster 的 Clock */

    /* 4. 配置目标 Hart 的 Reset Vector */

    /* 5. 释放目标 Hart 的 Reset */

    /* 6. 按硬件状态确认启动结果 */

    return 0;
}
```

真正的实现通常还必须考虑 Power Dependency、PLL Lock、Reset 顺序、Cluster Shared Resources 和超时。

PuC 完成相应硬件操作后，Hart 8 从 OpenSBI Warmboot Entry 开始执行。

OpenSBI 再恢复自身所需的 Hart Context、执行相应初始化，最终切换到 Linux 指定的 S-mode Entry。

整个过程可以画成：

```
Hart 0 / Linux
    |
    | SBI HSM_HART_START(8, linux_entry)
    v
Hart 0 / OpenSBI
    |
    | 保存 Linux Entry 到 Hart 8 Scratch
    | 将 Hart 8 标记为 START_PENDING
    |
    | RPMI HSM_HART_START(8, opensbi_warmboot)
    v
PuC / Platform Management Firmware
    |
    | Power / Clock / Reset Sequencing
    v
Hart 8 / OpenSBI Warmboot
    |
    | 恢复并初始化 M-mode Context
    | 读取已配置的下一阶段入口
    v
Hart 8 / Linux S-mode Entry
```

这也解释了为什么 OpenSBI 的 Hart 管理和 RPMI HSM 不是简单的功能重复：

OpenSBI 负责其软件可见的 HSM 状态机、Domain 权限和向 S-mode 交付执行权；PuC 负责按 SoC 实现操作真实的底层电源、时钟和复位硬件。

## 七、S-mode 是否可以直接使用 RPMI？

可以，但必须区分两种架构。

### 7.1 架构 A：M-mode 独占 RPMI，S-mode 通过 SBI

这是很容易理解的一种方式。

```
Linux
  |
  | SBI CPPC / SBI HSM / SBI SRST
  v
OpenSBI
  |
  | RPMI Messages
  v
PuC
```

Linux 无须直接了解 RPMI Transport 的物理细节。

### 7.2 架构 B：通过 SBI MPXY 代理 RPMI

SBI MPXY（Message Proxy Extension） 是 RISC-V SBI 提供的通用消息代理机制。

当平台只有少量 RPMI Transport，或者希望 M-mode 统一管理底层接口时，可以通过 MPXY 对 S-mode 暴露经过授权的 RPMI Service Group。

```
Linux RPMI Client
       |
       | SBI MPXY
       v
OpenSBI Message Proxy
       |
       | RPMI Transport
       v
PuC RPMI Server
```

这里需要理解两个不同的 Shared Memory：

`Linux ↔ OpenSBI` 使用的是 MPXY 定义的 per-Hart Shared Memory。

`OpenSBI ↔ PuC` 使用的是 RPMI Transport Shared Memory。

这两块内存并不相同。

更重要的是，S-mode 并不能通过 MPXY 任意访问 M-mode 专属服务。例如 RPMI HSM、SYSTEM_RESET 和 SYSTEM_SUSPEND 属于 M-mode-only Service Groups。

RPMI 与 SBI MPXY 的整合规范还对可代理的 Service Group 作出了进一步限制。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org+1



## 八、RPMI 与 RSS、HSM、OP-TEE 的关系

这部分对 RISC-V 服务器安全架构尤其重要。

先看一个包含独立安全控制器的示例：

```
               Application Harts
  +--------------------------------------+
  | Linux / UEFI / OP-TEE / OpenSBI      |
  +------------------+-------------------+
                     |
             RPMI / Other IPC
                     |
         +-----------+-----------+
         |                       |
  Platform Management       RSS / eHSM
       Controller          Security RoT
         |                       |
  Power / Clock / Reset    Key / Crypto / Boot
```

上面的 PuC 与 RSS 是两个逻辑角色，既可以由不同物理控制器实现，也可能由同一管理子系统在隔离的执行环境中提供。

但这两个角色的职责不应该混淆。

| 模块         | 核心职责                                  |
| ---------- | ------------------------------------- |
| OpenSBI    | M-mode 运行时、SBI 服务、Hart/Domain 管理      |
| RPMI       | 标准化平台管理消息接口                           |
| PuC        | 平台资源的底层管理与控制                          |
| RSS / eHSM | 安全根、密钥与密码服务、启动认证等                     |
| OP-TEE     | Trusted Execution Environment 与 TA 服务 |
| UEFI       | 平台启动策略、Boot Services、OS Loader 等      |

RPMI 不等于 Secure IPC，更不天然提供 TrustZone 式的安全域隔离。

如果将 PuC 与 RSS 合并实现，需要额外设计消息来源认证、权限控制、共享内存访问隔离、防重放策略及对安全资产的访问控制。

对于 OP-TEE 也一样：即使未来有 TEE 相关 RPMI Service Group，它也不会自动替代 OP-TEE 的 SMC ABI、TA 生命周期或安全内存隔离机制。

真正决定安全属性的，是 SoC 的访问控制硬件、可信固件及隔离策略，而不只是采用了某一种消息协议。

## 九、从工程角度，还应该掌握哪些关键问题？

RPMI 的规范并不复杂，真正的难点通常在 SoC 集成和调试阶段。

| 工程问题                 | 应重点检查的内容                                  |
| -------------------- | ----------------------------------------- |
| RPMI 初始化失败           | DT/ACPI 描述、共享内存、Doorbell、BASE 探测          |
| RPMI Request Timeout | PuC 是否运行、Queue Head/Tail、内存属性、Doorbell    |
| Hart Start 失败        | HSM State、Reset Vector、Power/Clock 顺序     |
| 并发请求混乱               | Locking、Token、Queue Ownership             |
| Suspend 无法唤醒         | Wakeup Source、Resume Address、Context 保存   |
| 不同固件版本不兼容            | RPMI Spec Version、Service Group Discovery |
| 安全隔离失败               | M/S 权限、PMP/IOPMP、共享内存和访问控制                |

我特别建议关注两个问题。

第一：PuC 比 AP 更早启动，并不意味着 RPMI 在 AP Reset 释放时就已经可用。

平台必须明确规定 PuC Ready、共享内存初始化、通道建立与 AP 侧 OpenSBI 初始化之间的顺序。

否则，很容易出现 OpenSBI 在冷启动阶段发送 BASE Request，却因 PuC 尚未准备好而超时的问题。

第二：RPMI Timeout 不等于操作一定没有执行。

假设 OpenSBI 发送 Hart Start Request 后等待 ACK 超时，但 PuC 实际上已经释放了目标 Hart。

若不检查实际 Hart 状态就重复发起启动命令，就可能出现重复请求和状态不一致的问题。

OpenSBI 的 RPMI Shared Memory Mailbox 实现也专门涉及响应超时和迟到消息的处理。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这些问题比单纯知道某个 RPMI Service ID 更接近 Staff Engineer 在芯片 Bring-up 中需要解决的问题。

## 十、和 Arm 的 PSCI / SCMI 如何类比？

利用已有的 Arm 架构经验，可以更快地建立概念。

| RISC-V           | Arm 中相近的概念        | 关系                  |
| ---------------- | ----------------- | ------------------- |
| SBI HSM          | PSCI CPU_ON/OFF   | OS 面向固件的 CPU 生命周期接口 |
| SBI System Reset | PSCI SYSTEM_RESET | 系统复位接口              |
| RPMI             | SCMI              | 与平台管理实体通信的协议        |
| PuC              | SCP/MCP           | 管理控制器角色             |
| SBI MPXY         | 固件消息代理层           | 提供上层受控消息访问          |

注意这是一种功能上的类比，并非一一对应。RPMI 本身涵盖了部分在 Arm 系统中通常由 PSCI 相关固件路径承担的操作，也包含 SCMI 风格的平台管理服务。

最重要的是把 OS-to-Firmware Interface 与 Firmware-to-Platform-Management Interface 分开理解。

## 十一、推荐的源码学习顺序

建议按下面的顺序阅读当前 OpenSBI 上游实现：

第一阶段：HSM 主流程

[lib/sbi/sbi_hsm.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hsm.c?utm_source=chatgpt.com)

理解 SBI HSM 状态机和 `warmboot_addr` 的作用。

第二阶段：RPMI HSM Client

[lib/utils/hsm/fdt_hsm_rpmi.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/utils/hsm/fdt_hsm_rpmi.c?utm_source=chatgpt.com)

理解 OpenSBI 如何通过 Mailbox 将 HSM 操作转换为 RPMI 请求。

第三阶段：底层传输

[lib/utils/mailbox/fdt_mailbox_rpmi_shmem.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/utils/mailbox/fdt_mailbox_rpmi_shmem.c?utm_source=chatgpt.com)

深入 Shared Memory Ring Queue、Memory Barrier、Token、Timeout。

第四阶段：RPMI Server Reference Implementation

[riscv-software-src/librpmi](https://github.com/riscv-software-src/librpmi?utm_source=chatgpt.com)

了解平台管理控制器如何解析消息、分发 Service 和调用底层 HAL。

`librpmi` 是一个独立的参考实现，可以用于平台微控制器固件，也能用于仿真器和虚拟化环境，并不局限于真实的物理 PuC。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 十二、总结：最应该形成的三个认知

1\. RPMI 解决的是平台资源控制的标准化，而不是 CPU 特权架构本身。 它让 OpenSBI 等客户端不必直接依赖各种 SoC 私有的电源、时钟和复位寄存器布局。

2\. RPMI 的核心价值是分层解耦。 Linux 发起操作，OpenSBI 维护 SBI 语义和 Hart 状态，RPMI 完成消息通信，PuC 执行真正的硬件操作。这些责任边界在复杂服务器 SoC 中尤其重要。

3\. 想真正掌握 RPMI，就必须把 Message Protocol、Shared Memory Transport、HSM State Machine 和 SoC Power/Reset Sequencing 串成一条完整调用链。

后续深入时，我最推荐从 `Linux SBI HSM → OpenSBI sbi_hsm.c → RPMI HSM Driver → Shared Memory Mailbox → PuC Server → Hart Warmboot` 这条路径开始。这样可以把 RISC-V 特权架构、OpenSBI、SMP 启动、IPC、内存一致性和真实硬件控制完整连接起来。
