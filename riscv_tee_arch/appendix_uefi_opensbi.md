# RISC-V SoC 启动架构深度解析：OpenSBI、UEFI、RSS 与 OP-TEE 如何协同工作？

## 一、先给出结论：OpenSBI 和 UEFI 解决的是两类不同的问题

你产生这种疑惑，其实与 RISC-V 的固件架构设计有很大关系。

在 Arm 平台上，我们通常能够比较清晰地识别 TF-A、OP-TEE、U-Boot/UEFI 的职责。但在 RISC-V 平台上，OpenSBI 本身也会执行硬件初始化、启动其他 Hart、完成特权级切换，甚至负责加载下一个固件阶段，因此很容易误以为 OpenSBI 已经完成了 UEFI 的工作。

关键区别是：

- OpenSBI：为 S-mode 软件提供底层特权服务（Supervisor Execution Environment）。
- UEFI：为操作系统提供标准化的启动环境、固件接口、设备抽象和启动管理能力。
- RSS/eHSM：为系统提供硬件信任根（Root of Trust）、镜像认证、密钥保护等安全能力。
- OP-TEE：提供隔离的可信执行环境（TEE），运行安全服务和 Trusted Applications。

其中，OpenSBI 与 UEFI 不是替代关系，而是通常存在上下层协作关系。

## 1.1 一张图理解完整启动链

先假设我们的 SoC 满足这些条件：

- RV64 多 Hart 应用处理器，支持 M/S/U privilege。
- 独立的 RSS/eHSM 安全处理器，负责 Root of Trust。
- 使用 OpenSBI 提供 SBI 服务。
- 使用 EDK II 实现 UEFI。
- 引入经过平台适配的 OP-TEE，提供可信执行环境。
- 最终启动 Linux Server。

一种合理的启动架构如下：

POWER-ON → LINUX BOOT

RSS / eHSM

独立安全处理器

RoT / Authentication

启动链认证、密钥管理

Application Harts

RV64 主处理器

Boot ROM / Early Loader

硬件初始化、镜像装载

安全认证通过，允许执行已验证的固件

OpenSBI

M-mode Firmware / SBI Implementation

PMP、Trap、IPI、Timer、HSM、系统复位等

mret → S-mode

UEFI / EDK II

S-mode Platform Firmware

DXE Driver、ACPI/DT、Boot Manager、EFI Services

LoadImage / StartImage

Linux EFI Stub / Bootloader

EFI memory map、FDT/ACPI、ExitBootServices

Linux Kernel — S-mode

Scheduler、MMU、Driver、SMP、Processes

OP-TEE：通过平台隔离机制建立的独立可信域

这是一种概念性架构，不是所有 RISC-V SoC 必须遵循的固定顺序。其中 OP-TEE 往往需要在 Normal World Linux 启动前建立可信内存和执行环境；它不是简单地串行插入 OpenSBI、UEFI、Linux 之间的某个必经阶段。

最重要的是理解这三个时间范围：

| 组件       | 开始发挥作用          | Linux 启动之后                              |
| -------- | --------------- | --------------------------------------- |
| RSS/eHSM | 上电安全启动阶段        | 可继续提供密码学、密钥、认证服务                        |
| OpenSBI  | M-mode 固件初始化    | 通常持续驻留，处理 SBI 调用                        |
| UEFI     | 平台启动环境建立后       | Boot Services 退出，部分 Runtime Services 保留 |
| OP-TEE   | 可信执行环境初始化后      | 可持续提供 TEE 服务                            |
| Linux    | OS Kernel Entry | 接管操作系统管理                                |

所以：

OpenSBI 不会因为 UEFI 启动而消失；UEFI 也不会因为 Linux 启动就完全消失。它们具有不同的生命周期和运行接口。

这个区别是后续理解整个架构的基础。

## 二、从设计理念理解：为什么 RISC-V 同时需要 SBI 和 UEFI？

这是最值得深入理解的部分。

### 2.1 SBI 的设计目标：屏蔽 M-mode 的硬件差异

你已经熟悉 RISC-V privilege architecture，因此我们直接从 M/S privilege 的职责划分讨论。

假设一个 16-Hart RV64 SoC：

```
                  Linux Kernel
                      S-mode
                        |
                        | SBI ecall
                        v
                  OpenSBI
                      M-mode
                        |
          +-------------+-------------+
          |             |             |
        Timer          IPI           HSM
          |             |             |
        ACLINT       IMSIC/        Hart state
                     platform
                        |
                    Hardware
```

SBI 的核心设计问题是：

> Linux 作为一个通用操作系统，如何在不知道不同 SoC 的 M-mode 硬件实现细节的前提下，完成定时器配置、IPI 发送、Hart 启动和系统复位等操作？

RISC-V 的回答是标准化 SBI。

例如 Linux 想启动 Hart 5：

```
/* 逻辑示例：由 Linux SBI wrapper 发起 */
struct sbiret ret;

ret = sbi_ecall(
    SBI_EXT_HSM,
    SBI_EXT_HSM_HART_START,
    5,                 /* hartid */
    secondary_entry,   /* start_addr */
    opaque,            /* opaque */
    0, 0, 0
);
```

在 RV64 底层，SBI `ecall` 使用如下寄存器约定：

```
li a7, 0x48534D        # SBI HSM extension
li a6, 0               # hart_start function ID
li a0, 5               # target hart
la a1, secondary_entry # entry address
li a2, 0               # opaque
ecall
```

OpenSBI 接收到来自 S-mode 的 `ecall` 后，完成 Hart 启动操作。

注意，这里包含三个不同的概念：

- Linux 决定什么时候启动 Hart 5，以及让它执行什么。
- OpenSBI 提供标准化的 Hart 启动接口，并负责底层操作。
- 硬件最终实现 Hart 的唤醒和执行。

OpenSBI 并不负责 Linux 的任务调度、负载均衡。 SBI HSM 也不等于 RSS/eHSM；前者是 Hart State Management，后者是安全硬件模块。

SBI 规格明确将其定位为 S-mode/VS-mode 软件与更高特权执行环境之间的标准接口，并且明确不承担硬件发现机制的定义。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation+1



### 2.2 UEFI 的设计目标：屏蔽操作系统启动平台的差异

UEFI 要解决的是另一个问题。

假设你是 Linux 发行版厂商，需要支持多家 RISC-V Server 厂商：

```
Vendor A SoC
    NVMe + PCIe + ACPI
           |
           v
         UEFI  --------+
                       |
Vendor B SoC           +---> Generic OS Loader
    SPI Flash + SATA   |            |
           |           |            v
           v           |       Linux Kernel
         UEFI  --------+
                       |
Vendor C SoC           |
    Network Boot       |
           |           |
           v           |
         UEFI  --------+
```

如果没有标准化的 Boot Firmware，每个 SoC 可能都需要自定义：

- 从哪里读取 Linux Kernel？
- 怎么访问 NVMe、SATA、网络或文件系统？
- 如何描述可用内存和保留内存？
- 如何发现 PCIe 设备？
- 如何传递 ACPI/Device Tree？
- 如何选择启动设备？
- 如何管理固件设置和启动项？
- 如何验证 OS Loader？
- 如何通知操作系统固件已退出启动阶段？

UEFI 将其中大量平台差异封装为统一的固件接口。

它不是单纯的 Bootloader，而是一个完整的 Pre-OS Firmware Execution Environment。

### 2.3 一个非常重要的概念：SBI 与 UEFI 是两套 ABI

| 对比维度    | SBI / OpenSBI                   | UEFI / EDK II                     |
| ------- | ------------------------------- | --------------------------------- |
| 标准      | RISC-V SBI Specification        | UEFI Specification                |
| 主要消费者   | S-mode OS、Hypervisor、Bootloader | OS Loader、UEFI Application、OS     |
| 核心目的    | 特权操作抽象                          | 启动环境与平台服务抽象                       |
| 常见实现位置  | M-mode                          | S-mode（常见部署）                      |
| 接口形式    | `ecall` + 寄存器 ABI               | C ABI + System Table/Protocol     |
| CPU 管理  | SBI HSM、IPI 等                   | 启动期 CPU 枚举和相关协议                   |
| 定时器     | SBI TIME 等                      | 启动环境 Timer/Event Services         |
| 设备访问    | 少量固件/平台服务                       | UEFI Driver Model、Block I/O 等     |
| 硬件描述    | 不定义通用发现机制                       | ACPI/DT Configuration Tables      |
| 内存管理    | PMP、Domain 等底层隔离                | UEFI Memory Map、内存分配              |
| OS 启动管理 | 不提供通用 Boot Manager              | `Boot####`、Boot Manager、EFI Image |
| OS 启动后  | 继续提供 SBI 服务                     | 仅 Runtime Services 等继续有效          |

这里需要特别注意：OpenSBI 是 SBI 的一种实现，EDK II 是 UEFI/PI 固件的一种实现，不能将规范和实现混为一谈。

UEFI 的基本 ABI 通过 `EFI_SYSTEM_TABLE` 暴露；其中包含 Boot Services、Runtime Services 和配置表。

[image](https://www.google.com/s2/favicons?domain=https://uefi.org\&sz=32)

UEFI Specification 2.11 documentation+1



## 三、按真实启动时间线逐阶段分析

下面以实际开发中常见的架构为例：

```
RSS/eHSM + Boot ROM + Early Loader
              |
              v
           OpenSBI
              |
              v
         EDK II / UEFI
              |
              v
            Linux
```

### Stage 0：RSS/eHSM 建立系统 Root of Trust

假设 RSS 是一个独立的 RISC-V 安全处理器，具有：

- 自己的 Boot ROM。
- 独立的 SRAM。
- OTP/eFuse/密钥存储能力。
- 密码学硬件加速能力。
- 与应用处理器之间的 Mailbox/IPC。
- 控制应用处理器 Reset Release 的能力。

一种典型的 Secure Boot 流程是：

```
RSS power-on
     |
     v
Execute immutable RSS ROM
     |
     v
Read Root Public Key Hash from OTP
     |
     v
Load Firmware Manifest
     |
     v
Verify Firmware Signature
     |
     +-- Failure --> Hold AP in Reset / Recovery
     |
     v
Authorize next firmware stage
     |
     v
Release Application Processor
```

在这种方案里，RSS 不需要运行 UEFI。

RSS 只需要确保系统接下来执行的固件被授权，且满足版本与回滚保护策略。

例如，RSS 可以分别验证：

```
OpenSBI image
UEFI firmware image
OP-TEE image
Platform configuration
```

这里必须区分两种验证机制：

Verified Boot / Platform Secure Boot

从硬件 RoT 开始，对早期固件和启动配置建立信任链。

UEFI Secure Boot

通常通过 UEFI 的 PK、KEK、db、dbx 等授权数据库，对 EFI Image（例如 OS Loader）执行验证。

两者并不等价。

即使 RSS 已经验证 UEFI Firmware，也不意味着 Linux EFI Loader 已经经过认证；反过来，仅启用 UEFI Secure Boot，也不能证明 OpenSBI 或 UEFI 自身没有在启动前被篡改。

对于你的项目，我更建议设计一条连续的信任链：

```
Hardware RoT
    |
    v
RSS ROM
    |
    v
Authenticated Platform Firmware
    |
    v
Authenticated OpenSBI / OP-TEE / UEFI
    |
    v
UEFI Secure Boot
    |
    v
Authenticated OS Loader
    |
    v
Linux Kernel + OS Security Policy
```

其中 OS Kernel 的完整信任链还需要考虑 EFI Stub、签名覆盖范围、initramfs、内核命令行、模块和 RootFS 验证，例如 UKI、dm-verity 等，不能只依赖一个已签名的 Bootloader。

### Stage 1：Boot ROM / Early Loader 装载固件

经过 RSS 授权后，应用处理器可以进入早期启动阶段。

典型职责包括：

- 初始化必要的时钟与 Reset Controller。
- 初始化 DDR Controller 和 PHY。
- 加载 OpenSBI。
- 加载 UEFI Firmware Image。
- 加载必要的 OP-TEE Image。
- 提供 Device Tree 或其他平台启动信息。
- 确保固件映像之间没有地址重叠。

这些操作不一定由一个独立 Bootloader 完成。它们也可能由 RSS 控制的加载引擎、Boot ROM 或多级固件共同执行。

### Stage 2：OpenSBI 初始化

OpenSBI 在这里承担两类职责。

第一类：一次性初始化。

例如：

- 建立 per-Hart `sbi_scratch`。
- 设置 M-mode Trap Handler。
- 初始化 SBI extension。
- 配置 PMP 与平台隔离资源。
- 初始化 Hart State Management。
- 初始化平台定时器和中断相关设施。
- 设置下一个执行阶段的入口地址与特权级。

第二类：运行时服务。

例如在 Linux 启动之后：

```
Linux S-mode
    |
    | ecall
    v
OpenSBI M-mode
    |
    +-- Timer
    +-- IPI
    +-- Hart Start/Stop
    +-- System Reset
    +-- RFENCE
    +-- Other implemented SBI Extensions
    |
    v
Return to Linux
```

OpenSBI 的一些早期初始化确实与 UEFI 固件初始化存在重叠，但这是平台实现的职责分配问题。

对于常见的 OpenSBI + S-mode EDK II 方案，OpenSBI 将平台准备到可以运行下一阶段的状态，而 UEFI 随后继续建立 OS 所要求的启动环境。

### Stage 3：OpenSBI 将 CPU 控制权交给 UEFI

这是你问题中最关键的转折点。

OpenSBI 有多种组织下阶段固件的方法：

| 固件类型         | 作用                            |
| ------------ | ----------------------------- |
| `FW_DYNAMIC` | 由上一阶段动态传入 UEFI 入口与相关信息        |
| `FW_JUMP`    | 跳转到预先确定的 UEFI 入口地址            |
| `FW_PAYLOAD` | 将下一阶段映像作为 OpenSBI 固件的 Payload |

以上都是 OpenSBI 官方提供的固件组织模式。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



假设 UEFI 已装载到 `0x80200000`，我们可以采用 `FW_JUMP` 的模型。

在 OpenSBI 的 `firmware/fw_jump.S` 中，有如下关键逻辑：

```
fw_next_addr:
    # 返回下一阶段入口地址
    # 具体读取方式取决于 FW_JUMP 配置
    ...

fw_next_mode:
    li a0, PRV_S
    ret
```

实际源码参见 [OpenSBI firmware/fw_jump.S](https://github.com/riscv-software-src/opensbi/blob/master/firmware/fw_jump.S?utm_source=chatgpt.com)。

这里返回了两个重要信息：

```
next_addr = UEFI entry address
next_mode = PRV_S
```

在典型的 M→S 特权级切换路径中，可以将底层逻辑简化为：

```
# 示意代码，不是完整 OpenSBI 实现

# Set mstatus.MPP = S
csrr t0, mstatus
li   t1, ~(3 << 11)
and  t0, t0, t1
li   t1, (1 << 11)
or   t0, t0, t1
csrw mstatus, t0

# Configure next PC
li   t0, 0x80200000
csrw mepc, t0

# Return to S-mode
mret
```

执行 `mret` 后：

```
Before:
    Privilege = M-mode
    PC = OpenSBI

After:
    Privilege = S-mode
    PC = UEFI Entry
```

请特别注意，这里 `mret` 是一次控制权移交，并不是 OpenSBI 自身被销毁。

OpenSBI 仍然保留在它的驻留内存中；S-mode 的 UEFI 和之后的 Linux 仍然可以通过 SBI 调用进入 M-mode。

### Stage 4：UEFI 开始执行——它真正的工作从这里展开

UEFI 接管控制权后，开始构建供 OS Loader 使用的标准化环境。

在 EDK II 的经典 Platform Initialization 模型中，可以将主要阶段分为：

SEC

Security

初始执行环境、早期信任与平台准备

PEI

Pre-EFI Initialization

早期资源初始化、PEIM、HOB

DXE

Driver Execution Environment

UEFI 驱动、设备协议、System Table

BDS

Boot Device Selection

Boot Manager、启动设备与 OS Loader

TSL

Transient System Load

EFI Loader、Boot Services 交接

RT

Runtime

运行时固件服务

这是一种经典的阶段模型，并不意味着每个 EDK II 平台都必须完整执行 SEC→PEI→DXE。

尤其值得注意：TianoCore 官方的 RISC-V QEMU virt EDK II 实现，就是以 OpenSBI 的 S-mode Payload 形式运行，而且明确采用了 PEI-less 设计。&#x20;

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这恰好说明：

UEFI 并非一定要从 Reset Vector 开始工作，也不一定需要自己执行所有早期平台初始化。

UEFI 可以作为 OpenSBI 后面的高层固件执行环境。

接下来我们看 UEFI 在这个阶段究竟完成哪些不可替代的工作。

### 4.1 DXE：UEFI 开始提供设备抽象

假设 Linux 存放在 NVMe SSD 中。

Linux 要启动，必须先从 SSD 读取 Kernel Image。

那么在 Linux 还没有运行时，谁来驱动 NVMe Controller？

这就是 UEFI 可以承担的工作。

```
UEFI DXE Core
     |
     +-- PCI Host Bridge Driver
     |       |
     |       +-- PCI Enumeration
     |       +-- BAR Allocation
     |       +-- PCI Resource Assignment
     |
     +-- NVMe Driver
     |       |
     |       +-- Block I/O Protocol
     |
     +-- Partition Driver
     |       |
     |       +-- GPT
     |
     +-- FAT Driver
             |
             +-- Simple File System Protocol
                     |
                     v
              EFI System Partition
                     |
                     v
                Linux EFI Image
```

注意这一系列操作和 OpenSBI 的区别。

OpenSBI 提供的 SBI 并不定义通用的 NVMe Block I/O、文件系统访问和 EFI Image 装载等接口。

UEFI 的 `EFI_BLOCK_IO_PROTOCOL` 则提供了标准设备访问抽象。

例如，一个 UEFI Application 只需要通过 `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` 等接口，就可以读取磁盘上的文件。

它不需要关心下面究竟是 NVMe、SATA 还是 VirtIO Block。

这就是 UEFI 最重要的价值之一：让不同平台提供统一的 Pre-OS I/O 环境。

### 4.2 UEFI Memory Map：操作系统凭什么知道内存能不能使用？

这部分与你的 OP-TEE、SMMPT 集成直接相关。

假设 SoC 有 16 GiB DDR，其中存在：

```
Physical Memory
+--------------------------------+
| OpenSBI Resident Memory        |
+--------------------------------+
| OP-TEE Secure Memory           |
+--------------------------------+
| UEFI Boot Services Memory      |
+--------------------------------+
| UEFI Runtime Services Memory   |
+--------------------------------+
| Linux Usable Memory            |
+--------------------------------+
```

这些区域并不能全部交给 Linux。

例如：

- OpenSBI 驻留内存不应该被 Linux 分配并覆盖。
- OP-TEE 安全内存不应该被 Linux 当成 Normal RAM。
- UEFI Runtime Services 使用的代码和数据在 Linux 启动后仍需保留。
- 部分 UEFI Boot Services 内存可以在 `ExitBootServices()` 之后回收。

UEFI 通过 `GetMemoryMap()` 暴露这些内存属性。

例如：

```
EFI_STATUS Status;
EFI_MEMORY_DESCRIPTOR *MemoryMap;

/* 省略获取缓冲区大小及分配内存的过程 */

Status = gBS->GetMemoryMap(
    &MemoryMapSize,
    MemoryMap,
    &MapKey,
    &DescriptorSize,
    &DescriptorVersion
);
```

这里的 `EFI_MEMORY_DESCRIPTOR.Type` 包含：

| 内存类型                          | 含义           |
| ----------------------------- | ------------ |
| `EfiConventionalMemory`       | 可用的普通系统内存    |
| `EfiBootServicesCode/Data`    | 启动期固件内存      |
| `EfiRuntimeServicesCode/Data` | 需要保留的运行时固件内存 |
| `EfiReservedMemoryType`       | 保留内存         |
| `EfiMemoryMappedIO`           | MMIO 区域      |

实际内存布局还需要考虑 `EFI_MEMORY_RUNTIME` 等属性，以及 ACPI、MMIO 和平台特有的安全内存描述。

尤其关键的是：

RISC-V Linux 官方启动要求明确指出，在 UEFI Boot 路径下，内核使用 EFI Memory Map 建立系统内存信息，而不是简单地从 Device Tree 的 `/memory` 节点推导全部可用 RAM。&#x20;

[image](https://www.google.com/s2/favicons?domain=https://cdn.kernel.org\&sz=32)

The Linux Kernel documentation



这对你当前项目意味着什么？

假设：

```
OpenSBI PMP:
    OP-TEE secure RAM inaccessible to Linux

SMMPT:
    Additional isolation rules

UEFI Memory Map:
    OP-TEE secure RAM wrongly marked usable
```

即使 PMP 或 SMMPT 阻止了 Linux 访问这块内存，Linux 仍可能认为它是可分配内存，随后出现访问异常、DMA Fault、内存初始化错误。

因此必须同时确保：

1. 物理隔离机制正确。
2. UEFI Memory Map 正确。
3. Device Tree/ACPI 描述与固件策略一致。
4. Linux 最终内存布局没有冲突。

硬件安全隔离与操作系统内存资源描述是两个不同但必须保持一致的层面。

### 4.3 ACPI / Device Tree：UEFI 在服务器平台中的另一项重要工作

你之前学习 Device Tree 时，已经接触到 Linux、OpenSBI、UEFI 都会使用 Device Tree 的情况。

这里需要区分两件事：

固件内部使用 Device Tree，与UEFI 向 OS 提供硬件描述，不是同一个概念。

例如：

```
Bootloader
     |
     v
OpenSBI
     |
     | Firmware DT
     v
UEFI
     |
     +-- Device Tree based OS handoff
     |
     +-- ACPI based OS handoff
             |
             v
           Linux
```

在传统嵌入式系统中，Device Tree 常常是硬件描述的核心。

而在面向通用 Linux 发行版的 RISC-V Server 平台中，ACPI、UEFI、SMBIOS 等标准化接口通常更加重要。

RISC-V BRS-I（Boot and Runtime Services — Interoperability）明确组合了 UEFI、ACPI、SBI、SMBIOS 等要求，以支持跨厂商的通用操作系统。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation+1



不过不能把 ACPI 理解为所有 RISC-V 平台的强制选择。BRS-B 等嵌入式配置允许不同的硬件描述策略。

对于你的 RISC-V Server 项目，建议尽早确定目标到底是：

- Server Interoperability： 面向 BRS-I，优先考虑标准 UEFI + ACPI。
- 定制化 Linux Platform： 可采用更简单的 UEFI + Device Tree。
- 早期 Pre-silicon Bring-up： 可以先只通过 Device Tree 启动 Linux，再逐步完善正式平台固件接口。

这三种目标的 UEFI 适配工作量可能相差很大。

### 4.4 BDS：UEFI 开始选择操作系统

BDS = Boot Device Selection。

例如：

```
BootOrder:
    Boot0001 -> NVMe Linux
    Boot0002 -> Network PXE Boot
    Boot0003 -> UEFI Shell
```

UEFI Boot Manager 根据固件变量与启动策略选择启动项。

假设选择 NVMe 上的 Linux：

```
UEFI Boot Manager
       |
       v
Read Boot0001
       |
       v
Resolve Device Path
       |
       v
Open EFI System Partition
       |
       v
Locate EFI executable
       |
       v
LoadImage()
       |
       v
StartImage()
```

Linux 的 EFI Stub 可以使内核映像作为 EFI 可执行映像进入 UEFI 启动流程，因此不一定需要 GRUB。

UEFI 的 Image Services 定义了 `LoadImage()`、`StartImage()`、`ExitBootServices()` 等标准接口。

[image](https://www.google.com/s2/favicons?domain=https://uefi.org\&sz=32)

UEFI Specification 2.10 documentation



### 4.5 ExitBootServices：整个启动架构中的第二个重要交接点

之前第一个交接点是：

```
OpenSBI --mret--> UEFI
```

第二个交接点是：

```
UEFI Boot Environment
        |
        | ExitBootServices()
        v
OS-controlled Environment
```

这是两种完全不同的操作。

OpenSBI → UEFI 是控制权与特权级的交接。

`ExitBootServices()` 则是固件启动资源所有权向操作系统移交。

在 EFI Loader 准备好内存布局、内核入口和启动参数后，它调用：

```
Status = gBS->ExitBootServices(
    ImageHandle,
    MapKey
);
```

其中 `MapKey` 必须与适当时机取得的内存映射对应。实际代码要处理内存映射变化导致的 `EFI_INVALID_PARAMETER`，因此不能简单地在很早获取一次 MapKey 后直接使用。

成功调用后：

- UEFI Boot Services 不再可用。
- OS 接管系统资源管理。
- 可回收的启动期内存可交给操作系统。
- UEFI Runtime Services 仍可通过规定的接口调用。
- OpenSBI 仍继续提供 SBI 服务。

这也是 UEFI 规范明确规定的生命周期分界线。

[image](https://www.google.com/s2/favicons?domain=https://uefi.org\&sz=32)

UEFI Specification 2.11 documentation



## 四、Linux 启动之后，OpenSBI、UEFI、OP-TEE 究竟如何共存？

这是整个架构里最容易产生误解的地方。

它们不是三个同时运行的普通线程，而是三个具有不同执行模型的固件/安全服务环境。

### 4.1 Linux 调用 OpenSBI

例如 Linux 需要启动另一个 Hart：

```
Linux
  |
  | SBI HSM ecall
  v
M-mode Trap
  |
  v
OpenSBI HSM Handler
  |
  v
Platform Hart Startup
  |
  v
Return to Linux
```

这是通过 RISC-V 特权级机制完成的。

### 4.2 Linux 调用 UEFI Runtime Services

例如 Linux 需要读取 UEFI Variable：

```
Linux Kernel
     |
     | EFI Runtime Services API
     v
UEFI Runtime Code
     |
     +-- Variable Storage
     |
     +-- Platform-specific Backend
     |
     v
Return to Linux
```

这里有一个很重要的区别：

UEFI Runtime Service 调用并不天然意味着进入 M-mode。

在常见 S-mode EDK II 架构中，UEFI Runtime Code 可以作为操作系统保留、映射并调用的固件代码运行。若某项操作需要更高特权或安全硬件服务，其实现可以再调用 SBI、固件通信接口或 RSS Mailbox。

例如：

```
Linux
  |
  | UEFI SetVariable()
  v
UEFI Runtime Variable Driver
  |
  | Platform Secure Storage Request
  v
RSS Mailbox / Secure Service
  |
  v
eHSM Protected Storage
```

这是一种可以设计的方案，不是 UEFI 标准规定的唯一实现。

对于服务器系统，还有一个值得注意的具体要求：RISC-V BRS-I 要求实现 `ResetSystem()` Runtime Service，并规定 OS 在符合该规范的系统上应优先通过 UEFI 进行复位，而不是直接使用 SBI SRST，以便固件能够完成变量持久化、Capsule 等必要操作。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



这也说明，即使 SBI 与 UEFI 都提供了某些功能，例如 Reset，它们仍然不完全等价。

### 4.3 Linux 调用 OP-TEE

这一部分需要特别区分 Arm 和 RISC-V。

Arm TrustZone 平台典型的调用链是：

```
Linux
  |
  | SMC
  v
EL3 Monitor
  |
  v
OP-TEE (S-EL1)
```

但 RISC-V 并没有直接对应 Arm Secure/Non-secure World 的通用执行状态，也没有 Arm 语义的 `SMC` 指令。

在一种基于 M-mode Domain 隔离的 RISC-V OP-TEE 方案中：

```
Linux (S-mode, Normal Domain)
         |
         | TEE request
         v
OpenSBI / M-mode Security Monitor
         |
         | Save Normal Domain Context
         | Configure Domain Isolation
         | Restore Secure Domain Context
         v
OP-TEE (S-mode, Secure Domain)
         |
         | Return / RPC
         v
M-mode Security Monitor
         |
         v
Linux (Normal Domain)
```

这里 Linux 和 OP-TEE 可以都在 S-mode 执行，但属于不同的隔离域。

值得关注的是，2026 年 9 月 10 日发布的 Linux RFC Patch Set 已经提出通过 SBI MPXY 与 RPMI TEE Service Group，为 RISC-V OP-TEE 提供标准化通信通道。

该方案由 M-mode 固件负责隔离域切换，并复用 OP-TEE 既有的消息协议。不过该补丁系列当时仍处于 RFC 阶段，相关 RPMI TEE Service Group 也尚未作为已发布的 RPMI v1.0 特性标准化，不能直接当成所有现有平台都具备的能力。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

Openwall



这对于你的 RISC-V OP-TEE Porting 工作有直接参考价值。

## 五、把这些组件组织成一个真正的 RISC-V SoC Firmware Architecture

我建议你将整个系统分成三个独立的架构平面。

### 5.1 Boot Plane：负责启动流程

```
RSS / Boot ROM
      |
      v
Early Loader
      |
      v
OpenSBI
      |
      v
UEFI
      |
      v
Linux EFI Stub
      |
      v
Linux Kernel
```

这是时间意义上的启动链。

### 5.2 Runtime Service Plane：负责运行期服务

```
                  Linux
                    |
       +------------+------------+
       |            |            |
    SBI API      UEFI RT       TEE API
       |            |            |
       v            v            v
    OpenSBI     UEFI Runtime    OP-TEE
       |            |            |
       +------------+------------+
                    |
             Platform Services
```

这里三类接口的调用链不一定经过同一个组件，也不要求最终指向 RSS；图中只是展示它们可能共同使用底层平台服务。

### 5.3 Security Plane：负责系统安全

```
               RSS / eHSM
                    |
          Root of Trust
                    |
          +---------+---------+
          |                   |
    Secure Boot          Key Services
          |                   |
          v                   v
  Authenticated FW      Trusted Storage
          |
          v
   OpenSBI / UEFI
          |
          +---- OP-TEE Secure Domain
          |
          +---- Linux Normal Domain
```

最重要的架构原则是：

Boot Flow、Runtime Service、Security Isolation 是三个不同的设计维度，不应该简单地通过一条串行启动链来表达全部关系。

RSS 提供的 Root of Trust 与 OP-TEE 提供的隔离执行环境也不一样：前者建立并维护信任基础，后者在得到硬件或固件保护的隔离域中执行可信软件。

对你当前的 SMMPT 集成工作，尤其需要确保 CPU、DMA Master、SMMPT 以及其他系统级访问控制机制共同实现 OP-TEE 隔离，而不是仅依靠 UEFI 内存表。

## 六、UEFI 是 RISC-V SoC 的必需组件吗？

不是。

这个问题反而最能解释 OpenSBI 与 UEFI 的职责区别。

### 方案 A：不使用 UEFI

```
Boot ROM
    |
    v
OpenSBI
    |
    v
Linux Kernel
```

只要前面的阶段准备好了 Linux 所需要的内容，例如：

- Kernel Image 已经正确装载。
- Device Tree/硬件描述可用。
- Hart 启动状态正确。
- `satp = 0`。
- Kernel Entry ABI 满足 Linux 要求。
- SBI 服务正常。

那么 Linux 可以直接启动，不一定需要 UEFI。

类似地，也可以使用：

```
Boot ROM → SPL → OpenSBI → U-Boot → Linux
```

U-Boot 官方还提供了 RISC-V Falcon Mode 等跳过完整 U-Boot 阶段的启动方式。

[image](https://www.google.com/s2/favicons?domain=https://docs.u-boot.org\&sz=32)

Das U-Boot unknown version documentation



### 方案 B：使用 UEFI

```
Boot ROM
    |
    v
OpenSBI
    |
    v
UEFI
    |
    v
EFI OS Loader
    |
    v
Linux
```

多引入 UEFI 的主要原因不是 Linux 无法在没有 UEFI 的情况下启动，而是希望获得：

- 标准化的 OS Boot Interface。
- 标准化的固件变量和启动项管理。
- Secure Boot。
- ACPI/SMBIOS。
- UEFI Firmware Update/Capsule。
- 更丰富的固件设备驱动与网络启动能力。
- 跨不同服务器平台的操作系统兼容性。

对于面向通用生态的 RISC-V Server，这些通常非常重要。

## 七、与你实际项目结合：UEFI 应该如何适配到 RSS + OpenSBI + OP-TEE？

下面是我认为比较合理的工程方向。

### 7.1 建议优先考虑 S-mode EDK II Payload 方案

如果你们已有可运行的 OpenSBI，我建议首先评估：

```
RSS Secure Boot
       |
       v
Platform Early Loader
       |
       v
OpenSBI (M-mode)
       |
       v
EDK II (S-mode)
       |
       v
Linux
```

理由是职责边界清晰：

| 固件           | 建议承担的主要责任                                             |
| ------------ | ----------------------------------------------------- |
| RSS          | 硬件 RoT、镜像认证、密钥管理                                      |
| Early Loader | DDR、镜像加载、启动参数                                         |
| OpenSBI      | SBI、M-mode 初始化、Domain 隔离                              |
| OP-TEE       | 可信执行域和安全服务                                            |
| EDK II       | UEFI Boot Manager、设备协议、Memory Map、ACPI/DT、RT Services |

这样可以避免让 EDK II 重复承担大量已经由 OpenSBI 和 Early Loader 完成的早期工作。

不过这不意味着 EDK II 不能集成 OpenSBI Library，或者执行部分 M-mode 初始化。UEFI 规范允许不同平台有不同的固件组织方式；这里只是优先推荐更有利于模块化维护的设计。

### 7.2 UEFI 和 RSS 真正需要集成哪些功能？

我认为有五个值得优先定义的交互接口。

第一：Authenticated Firmware Handoff

RSS 负责验证 UEFI Image，并将可信的启动状态传递给后续阶段。

核心问题是：UEFI 如何确定自己是从合法启动链进入的？

这需要有明确的信任边界，而不能只依靠 Normal World 可写的一段状态标志。

第二：Secure Variable Storage

UEFI 的 Boot Variables、Secure Boot Variables 需要可靠的持久化存储。

如果通过 RSS 提供 Backend，需要设计：

```
UEFI Variable Driver
       |
       v
Platform Secure Storage Protocol
       |
       v
RSS Mailbox
       |
       v
Authenticated / Protected NVM
```

同时必须设计访问授权、原子更新、断电恢复和防回滚，而不只是实现 Mailbox 读写。

第三：Cryptographic Services

UEFI 可能需要：

- Hash。
- Signature Verification。
- RNG。
- Certificate Processing。

这些功能可以使用软件密码库、主处理器硬件加速器或者 RSS 服务。

但不建议未经评估就将所有密码计算转发到 RSS，因为 Mailbox 延迟、吞吐量、并发、错误恢复都会成为系统设计问题。

第四：Firmware Update

如果支持 UEFI Capsule Update，那么需要明确：

```
UEFI UpdateCapsule
       |
       v
Firmware Update Service
       |
       v
RSS Authentication
       |
       v
Image Staging / A-B Slot
       |
       v
Secure Commit / Reboot
```

UEFI 负责提供标准更新接口，而 RSS 可以作为最终授权与安全写入链中的重要环节。

第五：Trusted Execution Integration

这部分主要涉及 OP-TEE：

- 谁负责加载 OP-TEE Image？
- 谁负责建立 OP-TEE Secure Domain？
- Linux 通过哪个标准或平台自定义接口进入 OP-TEE？
- RSS 是否参与密钥供应和可信状态证明？
- OP-TEE 的 Secure Memory 如何体现在 UEFI Memory Map 中？
- DMA 与 IOMMU/SMMPT 如何保证相同的隔离策略？

这里有一个架构原则：不建议让普通的 UEFI DXE Driver 自行决定 OP-TEE 的安全域切换策略。 安全域隔离应由受信任的更高特权固件和硬件机制控制。

### 7.3 建议的实施顺序

P0

OpenSBI → EDK II

确认 RV64 S-mode 入口，确保 UEFI Shell 可以启动

P1

EDK II → Linux

完成 Boot Manager、EFI Stub、DT/ACPI 和内存交接

P2

RSS Secure Boot

建立受认证的固件加载与信任链

P3

UEFI Runtime Services

完成 Variable、Reset、Time、存储持久化

P4

OP-TEE / SMMPT

保证隔离域切换和所有内存映射一致

P5

Server Compliance

验证 BRS-I、Secure Boot、ACPI、SMP、更新与恢复

这属于工程推进顺序，不是正式产品的信任链要求。

在 Pre-silicon 阶段，可以先通过受控测试环境验证功能启动链，再逐步打通 RSS 认证。但是正式生产系统不能允许未认证路径绕过 Secure Boot 策略。

## 八、从源码层面，应该重点看哪些地方？

现在你已经有 OpenSBI 和 OP-TEE 基础，建议不要从 EDK II 所有模块开始研究，而是优先研究固件交接路径。

1\. OpenSBI Firmware Handoff

重点理解 `FW_DYNAMIC`、`FW_JUMP`、`FW_PAYLOAD`、next_addr/next_mode 以及 M→S 跳转。

[OpenSBI Firmware Documentation](https://github.com/riscv-software-src/opensbi/blob/master/docs/firmware/fw.md?utm_source=chatgpt.com)

2\. EDK II RISC-V Platform

`OvmfPkg/RiscVVirt/RiscVVirtQemu.dsc`：理解平台组成、LibraryClasses、DXE Drivers、BDS 和 Runtime。

[RiscVVirtQemu.dsc](https://github.com/tianocore/edk2/blob/master/OvmfPkg/RiscVVirt/RiscVVirtQemu.dsc?utm_source=chatgpt.com)

3\. EDK II DXE Core 与 BDS

研究 `MdeModulePkg/Core/Dxe`、`MdeModulePkg/Universal/BdsDxe`，理解 EFI System Table 如何建立，以及如何选择并启动 EFI Image。

[EDK II DXE Core](https://github.com/tianocore/edk2/tree/master/MdeModulePkg/Core/Dxe?utm_source=chatgpt.com)

4\. Runtime Services 与 RISC-V CPU Driver

`UefiCpuPkg/CpuDxeRiscV64`、`RuntimeDxe`、`VariableRuntimeDxe`。重点理解 CPU 初始化、Runtime Memory 与变量 Backend。

[RISC-V CPU DXE](https://github.com/tianocore/edk2/tree/master/UefiCpuPkg/CpuDxeRiscV64?utm_source=chatgpt.com)

5\. Linux RISC-V EFI Boot

查看 `drivers/firmware/efi/libstub`、`arch/riscv/kernel/efi.c` 及内核早期启动流程，理解 EFI Stub、Memory Map、Boot Hart ID。

[Linux RISC-V Boot Requirements](https://docs.kernel.org/arch/riscv/boot.html?utm_source=chatgpt.com)

在官方 RiscVVirt DSC 中，可以直接看到 DXE Core、RISC-V CPU DXE、Variable Runtime、Secure Boot、安全验证以及 BdsDxe 等模块被集成到同一个 Firmware Build 中。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这比从 UEFI 规范第一章一路阅读，更容易快速建立工程层面的整体理解。

## 九、再用 Armv8-A 的经验帮助理解一次

你有 TF-A、OP-TEE、TrustZone 和 PSCI 背景，可以用下面这个对照建立直觉：

| Armv8-A 常见组件        | RISC-V 对应角色                             |
| ------------------- | --------------------------------------- |
| BL1 / BL2           | Boot ROM / Early Loader / RSS Boot Flow |
| BL31 / TF-A Runtime | OpenSBI 提供的部分底层运行期服务                    |
| PSCI                | SBI HSM/SRST 等相关服务                      |
| SMC ABI             | SBI `ecall`（并非完全等价）                     |
| BL32 / OP-TEE       | RISC-V 隔离域中的 OP-TEE                     |
| BL33 / UEFI         | S-mode EDK II / UEFI                    |
| EL1 Linux           | S-mode Linux                            |

这个对应关系只能帮助理解功能职责，不能直接进行一对一的架构映射。

尤其是：

- Arm EL3 具有 Secure Monitor 的体系结构语义。
- RISC-V M-mode 并不天然等同于 Arm Secure World。
- Arm OP-TEE 通常运行在 S-EL1。
- RISC-V OP-TEE 需要额外依赖 PMP、Domain、SMMPT 等机制建立隔离。
- UEFI 在 Arm 和 RISC-V 上承担的主要标准固件职责则比较接近。

可以这样理解：

你在 Arm 上习惯的 TF-A + OP-TEE + UEFI 分层，迁移到 RISC-V 后，大致变成了 OpenSBI + Secure Domain Framework/OP-TEE + UEFI，但两者的底层安全架构并不相同。

## 十、最后用三个问题判断自己是否真正理解了这套架构

问题一：OpenSBI 已经初始化了 Timer 和 IPI，为什么 EDK II 还要 CPU Timer Driver？

因为 OpenSBI 负责提供低层硬件服务，而 EDK II 需要为 UEFI 环境提供 Timer/Event 等抽象。EDK II 可以通过 SBI 或其他平台接口实现自己的计时服务，不意味着必须重新完成 OpenSBI 的硬件初始化。

问题二：UEFI 和 OP-TEE 都使用 S-mode，是否会冲突？

不一定。

如果 OP-TEE 被放在独立的 Secure Domain 中，并由可信的高特权固件实施隔离和上下文切换，那么它可以与 S-mode UEFI、S-mode Linux 共存。

但具体能否实现，取决于系统隔离架构和 Domain Switch 机制，而不是单纯取决于 S-mode 这个 privilege level。

问题三：如果已经有 RSS/eHSM，UEFI Secure Boot 还有必要吗？

对面向通用 Server OS 的产品，通常仍然有价值。

RSS 负责平台固件信任链，UEFI Secure Boot 负责 OS Loader 等 EFI Image 的启动授权。两者可以共同建立从 Hardware Root of Trust 到操作系统的完整信任链。

## 总结

回到最初的问题：OpenSBI 是否已经把 UEFI 的工作做完了？

答案是没有。

OpenSBI 让 S-mode 操作系统能够安全、可移植地使用底层特权硬件功能；UEFI 让不同操作系统能够通过标准化的方式发现平台资源、加载启动映像、管理固件设置并完成启动交接。

在你的 RSS + OpenSBI + OP-TEE + UEFI + Linux 架构中，我认为最值得深入研究的下一步是：

以一个真实的 16-Hart RISC-V Server SoC 为例，从 RSS Release AP Reset 开始，跟踪 OpenSBI `mret` 进入 EDK II，再跟踪 EDK II 的 DXE、BDS、EFI Stub、`ExitBootServices()`，直到 Linux 建立页表和启动 Secondary Harts。

把这个过程贯通后，UEFI 在整个 SoC Firmware Architecture 中的定位就会非常清晰，而且能够直接转化为实际的固件适配、启动调试与安全设计能力。
