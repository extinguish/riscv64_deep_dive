# Chapter 18 — RISC-V OP-TEE Device Tree & Platform Porting

RISC-V OP-TEE Architecture & Porting · 高级技术讲义 · Staff Engineer Level

本章主题：如何将 QEMU virt 上能够运行的 OP-TEE，移植到真实的 RISC-V 多核 SoC，并建立完整、可验证的安全执行环境。

本章是整套课程从 Architecture 进入真实 Engineering 的关键转折点。

之前我们已经讨论过 OpenSBI Domain、PMP、Trap/Exception、Trusted Thread、MMU、SBI MPXY、RPMI、Interrupt 和 SMP。

但是，在真实芯片上，理解这些机制并不代表它们能够协同工作。

例如，你可能遇到以下工业项目中的典型问题：

- OP-TEE Core 已经成功启动，但是 Linux 无法发现 TEE Device。
- Linux 和 OP-TEE 均正常运行，但 Linux 可以通过 DMA 读取 OP-TEE 的内存。
- QEMU 单 hart 测试全部通过，实际 16-hart SoC 在并发调用 TA 时偶发死锁。
- Linux 的 Device Tree 正确配置了 `reserved-memory`，但某些固件仍然覆盖 OP-TEE。
- OpenSBI Domain 配置正确，但切换到 OP-TEE 时出现 Instruction Access Fault。
- RSS/HSM 已经完成镜像认证，但 OP-TEE 使用的某些配置仍然可能被 Linux 修改。

这些问题的共同特点是：

它们不是单一 OP-TEE Kernel Bug，而是 Boot Firmware、Device Tree、Memory Map、Hardware Isolation、Interrupt Controller、Linux Driver 之间的系统集成问题。

本章将围绕这类问题展开。

## 本章学习目标

完成 Chapter 18 后，应当具备以下能力：

1. 理解 QEMU virt 和真实 SoC 的平台抽象差异。
2. 能够独立设计 OP-TEE 的物理内存布局和可信地址空间。
3. 能够编写和审查 OpenSBI Domain Device Tree 配置。
4. 能够明确 Linux DT、OpenSBI DT 和 OP-TEE DT 的职责边界。
5. 能够移植 UART、Timer、Interrupt Controller 和基础平台驱动。
6. 能够集成 SBI MPXY/RPMI TEE Transport。
7. 能够评估 PMP、IOPMP、WorldGuard、SoC Firewall 的实际安全保障。
8. 能够设计多级固件之间的安全配置交接。
9. 能够完成一个新的 RV64 SoC 的 OP-TEE Bring-up。
10. 能够针对实际项目提出安全性、可靠性及可维护性的架构设计方案。

本章遵循原定的全部十个小节，另增加完整设计案例、实操实验和 Staff Engineer 面试题。

> 源码与规范说明： 本章以截至 2026 年 11 月公开的 OP-TEE、OpenSBI、RISC-V RPMI 资料为基础。需要特别注意：RISC-V RPMI TEE 的 Linux Driver 与 DT Binding 仍有 RFC 设计演进，不能将 RFC 中的接口直接视为已经稳定的 ABI。示例中的 `AcmeRV64` 是虚构的教学 SoC；其地址、设备和部分接口代码仅用于说明设计，不代表某个实际芯片或现有 upstream 实现。

# 18.1 QEMU virt 与真实 RISC-V SoC 的差异

## 18.1.1 为什么我们不能简单复制 `plat-virt`？

先看 QEMU virt 的设计目的。

QEMU virt 本质是一个方便操作系统开发和验证的通用虚拟硬件平台，而不是任何一款具体 SoC 的准确复制。

它提供相对标准化的硬件接口，例如：

- RV64 CPU/Hart
- CLINT 或 ACLINT
- PLIC，或可选的 APLIC/IMSIC
- NS16550 UART
- VirtIO
- PCIe
- DRAM 和 Flash

QEMU 还会生成描述当前机器配置的 DTB，包括 CPU 数量、内存范围、中断控制器和设备地址。

[image](https://www.google.com/s2/favicons?domain=https://www.qemu.org\&sz=32)

QEMU documentation



真实 SoC 则有大量芯片特有的约束。

| 比较维度          | QEMU virt             | 真实 RISC-V SoC                   |
| ------------- | --------------------- | ------------------------------- |
| CPU/Hart      | 通用 RV64 模型            | 具体微架构、扩展、Errata                 |
| Boot ROM      | 简化或仿真启动行为             | 固化的 Boot ROM、启动介质、认证流程          |
| DRAM          | 预设的线性内存范围             | DDR Training、多 Bank、Hole、保留区    |
| Interrupt     | 标准化的 PLIC/APLIC/IMSIC | 实际拓扑、路由权限和硬件差异                  |
| PMP           | CPU 模型提供的实现           | Entry 数量、Granularity 和具体限制      |
| DMA           | 仿真设备行为                | 多 Bus Master、IOMMU、互连安全策略       |
| Root of Trust | 可以通过软件构造              | 真实 OTP、HSM、RSS、Secure Boot      |
| Power         | 简化模型                  | Reset Domain、Clock、Power Domain |
| Console       | 常见 UART               | 不同 UART IP、Clock、Pinmux         |
| Firmware      | 简化加载链                 | Boot ROM、RSS、SPL、OpenSBI、UEFI 等 |

这里有一个容易被忽视的问题：

在 QEMU 中能够运行 OP-TEE，只能证明当前模拟平台与软件组合满足了测试条件，不能直接证明真实 SoC 的安全隔离正确。

例如，QEMU 中由软件控制的内存访问可能受到 PMP 保护，但真实 SoC 中的 PCIe、DMA Engine 或其他 Master 能否访问同一块 DRAM，则取决于平台互连、DMA Remapping 和安全防火墙等硬件。

PMP 本身并不能自动约束所有非 CPU Master。

## 18.1.2 移植实际上有四个独立层次

我建议把 Platform Porting 拆成以下四层。

Layer 4 — OP-TEE Porting

Boot Entry、MMU、UART、Interrupt、Trusted Thread、TA RAM

Layer 3 — Firmware ABI Integration

SBI MPXY、RPMI、RPC、Memory Sharing、Linux Driver

Layer 2 — OpenSBI Platform Integration

Domain、Hart、PMP、Trap Routing、Hardware Isolation

Layer 1 — SoC Hardware & Boot Foundation

ROM、RSS/HSM、DDR、Clock、Reset、PLIC/AIA、Firewall、DMA

如果 Layer 1 的内存防火墙配置不正确，即使 Layer 4 中的 OP-TEE 完全没有软件缺陷，整个系统也不能认为具有充分的安全隔离。

如果 Layer 2 的 Domain 配置正确，但 Layer 3 的调用 ABI 不匹配，Linux 仍然无法正常访问 TEE 服务。

因此，我们必须把 Bring-up 与 Security Validation 分开：

- Bring-up correctness：系统能够启动、调用 TA、完成测试。
- Security correctness：即使 REE 被攻破，硬件和可信固件仍然能够维持预定的安全边界。

## 18.1.3 平台移植的第一份交付物不是代码

正式移植前，应先建立一份 Platform Integration Contract。

至少包含：

| 项目           | 必须明确                                 |
| ------------ | ------------------------------------ |
| ISA          | RV64、扩展集合、特权架构版本                     |
| Hart         | Hart ID、数量、启动顺序、可能的非连续编号             |
| Firmware     | ROM、RSS、OpenSBI、UEFI/U-Boot 的责任      |
| Memory       | DDR、SRAM、ROM、MMIO、可信区、共享区            |
| Domain       | Boot Domain、TEE Domain、REE Domain    |
| Interrupt    | PLIC/APLIC/IMSIC 拓扑、Source Ownership |
| Timer        | 时钟源、访问权限、定时器编程接口                     |
| Security     | PMP、IOPMP、Firewall、锁定与重置行为           |
| Transport    | SBI MPXY、RPMI、Mailbox、共享缓冲区          |
| Device Tree  | 每个固件阶段使用哪个 DTB                       |
| Verification | 预硅仿真和流片后测试的成功标准                      |

这份文档的重要性甚至高于最初几百行平台代码。

原因很简单：如果基础约束尚未达成一致，代码能够编译成功也不能证明系统设计是正确的。

# 18.2 OP-TEE Physical Memory Layout 与 Reserved Memory

这是实际 Porting 中最容易产生隐蔽安全漏洞的部分。

我建议先建立一个核心认识：

TEE Memory Layout 不只是内存分配问题，而是可信物理内存的所有权、生命周期和访问权限问题。

## 18.2.1 一个完整 SoC 为什么需要多个内存区域？

假设我们要移植到一款教学用 RV64 SoC：

- 16 个 Application Harts。
- 1 GiB DDR，起始地址 `0x80000000`。
- OpenSBI 运行在 M-mode。
- Linux 和 OP-TEE 使用不同的 S-mode Domain。
- OP-TEE 预留 16 MiB Trusted DDR。
- 另外预留 4 MiB Static Shared Memory。
- 有独立 RSS/HSM，提供 Secure Boot 和密钥服务。

可以设计以下内存布局。

教学 SoC 的 DDR 物理地址空间

Linux / REE DRAM

0x92000000 – 0xBFFFFFFF

880 MiB

Shared Memory Window

0x91000000 – 0x913FFFFF

4 MiB

OP-TEE Trusted DDR

0x90000000 – 0x90FFFFFF

16 MiB

Linux / REE DRAM

0x80000000 – 0x8FFFFFFF

256 MiB

概念示意，不按容量比例绘制。0x91400000–0x91FFFFFF 也分配给 REE，可根据平台需要细分为 DMA Pool 等；OpenSBI 的实际驻留内存还需单独从可用区排除并保护。全部地址为教学示例。

注意：我们讨论的是地址区间的逻辑所有权。并不意味着实际 DDR 物理分布必须完全按照图中的排列方式，也不代表 Shared Memory 一定需要静态预留。

## 18.2.2 OP-TEE Core RAM、TA RAM、Shared Memory 的区别

首先需要区分三个概念。

| 内存区域          | 主要用途                            | 典型可信属性    |
| ------------- | ------------------------------- | --------- |
| Core RAM      | OP-TEE Kernel 的代码、数据、Stack、Heap | Trusted   |
| TA RAM        | User TA 的代码、数据、Stack、Heap       | Trusted   |
| Shared Memory | REE/TEE 之间的参数和大块数据传递            | Untrusted |

### Core RAM

这里运行 OP-TEE Kernel。

典型内容包括：

- Text / Read-only Data
- Global Data / BSS
- Per-Hart Stack
- Trusted Thread Stack
- Core Heap
- MMU Page Tables

Core RAM 必须避免被 Linux 或未授权 DMA Master 修改，否则 REE 可以直接破坏 TEE Kernel。

### TA RAM

TA RAM 是 OP-TEE 为 User TA 提供的内存。

与 Core RAM 同样需要物理安全隔离，但内部还需要使用 S-mode 页表权限保护不同 TA。

例如：

```
Physical Trusted DDR
    |
    +-- OP-TEE Core
    |
    +-- TA A Memory
    |
    +-- TA B Memory
    |
    +-- Page Tables / Metadata
```

Linux 不得直接访问整个 Trusted DDR。

但是 TA A 与 TA B 之间的隔离，并不是由 PMP 自动完成的。

在同一个 OP-TEE Domain 内，它们都处于同一类物理安全边界之下，而 OP-TEE Kernel 需要使用 RISC-V MMU 和 U/S 权限建立更细粒度的 TA Isolation。

### Shared Memory

Shared Memory 的性质完全不同。

例如 Linux 向 OP-TEE 发送：

```
TEEC_InvokeCommand(session, CMD_SIGN, ...);
```

其中一个 Memref 可能由 Linux 用户空间创建，经过 `libteec`、Linux TEE Driver 映射后传递给 OP-TEE。

OP-TEE 可以访问这块内存，不代表它可以信任这块内存的内容。

Linux 可以在 OP-TEE 读取前后修改它，从而构造 TOCTOU 或 Double-Fetch 问题。

因此 Shared Memory 需要：

- 地址及长度校验。
- 权限和所有权检查。
- 对敏感参数采用必要的复制与快照。
- Cache Coherency 管理。
- 正确的生命周期和引用计数。

## 18.2.3 当前 RISC-V `plat-virt` 的真实配置

下面不是虚构代码，而是当前 OP-TEE `core/arch/riscv/plat-virt/conf.mk` 中的一部分实际配置：

```
$(call force,CFG_CORE_RESERVED_SHM,n)
$(call force,CFG_CORE_DYN_SHM,y)

CFG_TEE_CORE_NB_CORE ?= 1
CFG_NUM_THREADS ?= 1

CFG_TDDRAM_START ?= 0x8e000000
CFG_TDDRAM_SIZE  ?= 0x00f00000
CFG_TEE_RAM_VA_SIZE ?= 0x00200000
```

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

这几项值得深入理解。

`CFG_TDDRAM_START` / `CFG_TDDRAM_SIZE` 确定平台配置中的 Trusted DDR 范围。这个范围进一步用于 OP-TEE 内存布局。

`CFG_TEE_RAM_VA_SIZE` 表示 Core RAM 的虚拟地址空间相关配置，它不是 `TEE RAM` 与 `TA RAM` 的简单物理容量总和。

`CFG_CORE_DYN_SHM=y` 表示启用 Dynamic Shared Memory；并不要求所有 REE/TEE 共享数据都放在一块固定物理地址区域内。

因此，从 QEMU 移植到真实芯片时，不应该仅仅修改一个 `CFG_TDDRAM_START` 就认为 Memory Porting 已经完成。

必须同时确认：

1. TEE Image 的实际加载物理地址。
2. Linker 和 Runtime Memory Layout。
3. MMU 映射及属性。
4. Linux 的可分配内存和 Reserved Memory。
5. OpenSBI 的 Domain Memory Region。
6. DDR Firewall / IOPMP 的保护范围。
7. DMA 是否可以访问受保护的内存。

## 18.2.4 为什么 `reserved-memory` 不能代替 PMP？

考虑 Linux DT：

```
/ {
    #address-cells = <2>;
    #size-cells = <2>;

    memory@80000000 {
        device_type = "memory";
        reg = <0x0 0x80000000 0x0 0x40000000>;
    };

    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;

        optee_reserved: optee@90000000 {
            reg = <0x0 0x90000000
                   0x0 0x01000000>;
            no-map;
        };

        optee_shm: shm@91000000 {
            reg = <0x0 0x91000000
                   0x0 0x00400000>;
            no-map;
        };
    };
};
```

这段 DTS 表示：

- DRAM 从 `0x80000000` 开始，共 1 GiB。
- 其中 16 MiB 留给 OP-TEE。
- 另有 4 MiB 用作静态 Shared Memory。
- `no-map` 要求 Linux 不建立普通线性映射。

实际平台中，还需要根据所选 Static/Dynamic SHM Transport Binding 确定 Shared Memory 节点是否采用 `shared-dma-pool`、`reusable`、`no-map` 等属性，不能把这些属性随意组合。

现在考虑一个问题：

如果 Linux Kernel 被攻击者控制，攻击者可以忽略这个 Reserved Memory 描述吗？

可以。

Linux DT 主要告诉正常运行的操作系统如何使用资源，并不是硬件强制的安全策略。

攻击者可以尝试创建新的页表映射访问这段物理内存。

假设：

```
Linux VA
   |
   | S-mode MMU
   v
PA = 0x90001000
   |
   | PMP / SoC Protection Check
   v
Access Denied
```

只有底层硬件访问控制确实拒绝 REE 对该地址的访问时，才存在可依赖的物理隔离。

因此必须牢牢记住：

> `reserved-memory` 解决正常内存管理与资源协调的问题；PMP 和 SoC Firewall 才可能提供不依赖 Linux 自觉遵守的访问控制。

还有一个细节：Current RISC-V OP-TEE 的 `boot.c` 中确实包含 `mark_tddram_as_reserved()` 和 `update_external_dt()`，可向外部 DT 增加可信内存保留信息。这是一项协调行为，不是硬件权限配置。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 18.2.5 如何设计正确的安全内存布局？

建议采用以下检查原则。

第一：地址和大小必须满足全部硬件的对齐要求。

例如一个 16 MiB 的 PMP NAPOT 区域应具备 16 MiB 对齐的起始地址。

这里：

```
Base = 0x90000000
Size = 0x01000000
```

适合表达一个 16 MiB 对齐区域。

但是否能够使用一个 PMP Entry 实现，仍取决于具体 PMP 功能、可用 Entry 和访问权限布局。

第二：保护对象必须包括所有可信内存。

不仅是 OP-TEE Text，也包括：

- Core Data/BSS
- Trusted Thread Stack
- TA RAM
- Security Metadata
- 可信页表
- 与密钥有关的缓冲区

第三：加载阶段和运行阶段都必须安全。

在 OP-TEE 启动前，Loader 可能需要临时访问 TEE Image 目标地址。

需要设计清楚：

```
Image Authenticated
       |
Image Loaded
       |
Memory Protection Installed
       |
OP-TEE Entry
       |
Normal Operation
```

保护安装的时间点很关键，尤其需要分析是否存在并发 hart 或 DMA 可以访问尚未锁定区域的窗口。

第四：必须验证 Linux 不能重新配置硬件保护。

如果安全防火墙的控制寄存器仍由 Linux 可写，那么即使上电时权限正确，攻击者仍可能关闭保护。

这类问题应当被视为 SoC Security Architecture 的缺陷。

# 18.3 OpenSBI Domain Device Tree 定义

这是本章最重要的部分之一。

你之前已经学习过 OpenSBI Domain 的内部结构，例如：

```
struct sbi_domain;
struct sbi_domain_memregion;
```

现在我们重点回答：

怎样通过 Device Tree 描述这些 Domain，并由 OpenSBI 将描述转化成运行时的安全隔离配置？

## 18.3.1 Device Tree 在 OpenSBI 中的角色

传统情况下，我们通常把 DT 理解为 Linux 用于枚举和配置硬件的描述格式。

但是对 OpenSBI 而言，DT 还可以承载 Firmware Policy，包括：

- Domain 划分。
- Hart Assignment。
- 物理内存 Region。
- Domain 启动入口。
- Device Ownership。
- 部分系统控制权限。

OpenSBI 当前的 Domain Support 文档明确定义了 `opensbi,domain,config`、`opensbi,domain,memregion` 和 `opensbi,domain,instance` 这些节点类型。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此，在 RISC-V TEE 系统中，必须区分两种语义：

| DT 类型                | 核心用途                | 主要使用者                     |
| -------------------- | ------------------- | ------------------------- |
| Platform Hardware DT | 描述 CPU、内存、设备、中断拓扑   | OpenSBI、U-Boot/UEFI、Linux |
| Domain Policy DT     | 描述 Domain、资源归属和启动策略 | OpenSBI                   |
| Sanitized REE DT     | 只暴露 REE 可以使用的设备资源   | Linux                     |
| Trusted DT           | 描述 OP-TEE 可使用的可信资源  | OP-TEE                    |

这些不是 RISC-V 架构强制要求的四个独立 DTB 文件，而是建议从设计上区分的四种信息视图。

一个系统可以使用多个 DTB，也可以在早期以一个 DTB 作为输入，再由可信 Firmware 为不同 Domain 生成经过筛选的 DTB。

重点是：不能让不可信的 Linux 成为硬件安全策略的最终决策者。

## 18.3.2 OpenSBI Domain DT 的基本结构

OpenSBI 官方文档中的典型结构是：

```
/chosen
    |
    +-- opensbi-domains
           |
           +-- memory-region nodes
           |
           +-- domain instance nodes
```

具体示例：

```
/ {
    chosen {
        opensbi-domains {
            compatible = "opensbi,domain,config";

            tee_mem: tee-memory {
                compatible = "opensbi,domain,memregion";
                base = <0x0 0x90000000>;
                order = <24>;
            };

            tee_domain: tee-domain {
                compatible = "opensbi,domain,instance";
                possible-harts = <&cpu0>;
                regions = <&tee_mem 0x3f>;
                boot-hart = <&cpu0>;
                next-addr = <0x0 0x90000000>;
                next-mode = <0x1>;
            };
        };
    };
};
```

这是一个用于解释节点语义的局部示例，不是完整、可直接部署的双 Domain 安全策略。

其中的 `&cpu0` 必须在完整 DT 中已有对应节点和 Label；真实配置还必须描述合法的 REE 区域、其他设备资源、Hart 归属、域间共享区等。

### `tee_mem`

```
tee_mem: tee-memory {
    compatible = "opensbi,domain,memregion";
    base = <0x0 0x90000000>;
    order = <24>;
};
```

`order = <24>` 表示：

\\[ \text{Region Size} = 2^{24} = 16\ \text{MiB} \\]

`base` 指定区域起始地址。

这和 Linux `reserved-memory` 中直接使用 `reg = <base size>` 的方式不同。

OpenSBI Domain Memory Region 采用符合其内存区域和 PMP 表示要求的 Base + Order 模型。

### `tee_domain`

```
tee_domain: tee-domain {
    compatible = "opensbi,domain,instance";
    possible-harts = <&cpu0>;
    regions = <&tee_mem 0x3f>;
    boot-hart = <&cpu0>;
    next-addr = <0x0 0x90000000>;
    next-mode = <0x1>;
};
```

字段解析：

| 属性                         | 含义                      |
| -------------------------- | ----------------------- |
| `possible-harts`           | 允许归属于该 Domain 的 Hart 集合 |
| `regions`                  | Domain 可以访问的 Region 及权限 |
| `boot-hart`                | Domain 的启动 Hart         |
| `next-addr`                | 下一级 Image 的物理入口地址       |
| `next-mode`                | 下一级 Image 的特权级别         |
| `init-order`               | Domain 的启动初始化顺序         |
| `root-regions-inheritance` | 是否继承 Root Domain 的区域定义  |

这里 `next-mode = <0x1>` 代表 S-mode，而 `<0x0>` 代表 U-mode。

OpenSBI 官方文档对这些属性及其约束有完整定义。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 18.3.3 `regions` 权限位到底是什么意思？

这是需要重点理解的细节。

OpenSBI Domain DT 的 Region 权限可以使用一个 32-bit Bitmask。

| Bit | 含义               |
| --- | ---------------- |
| 0   | M-mode Read      |
| 1   | M-mode Write     |
| 2   | M-mode Execute   |
| 3   | S/U-mode Read    |
| 4   | S/U-mode Write   |
| 5   | S/U-mode Execute |
| 6   | Enforce / Lock   |

例如：

```
0x3F = 0b00111111
```

表示 M 和 S/U 访问权限位均包含 R/W/X。

再如：

```
0x1B = 0b00011011
```

对应 M-mode R/W，以及 S/U-mode R/W，但不允许 Execute。

不过，切勿把 `0x3F` 理解为“适合所有 TEE 内存的最终安全权限”。

对 OP-TEE Text、RO Data、RW Data 采用不同权限，是更严格的安全目标。

另外，Bit 6 涉及强制执行和 PMP 锁定，必须非常谨慎。在标准 PMP 实现中，锁定的 Entry 通常不能在 hart Reset 前修改，错误锁定可能直接破坏后续 Domain Switching。

OpenSBI DT 还限制在 Domain Instance 中直接配置只有 M-mode 访问位的 Region；M-only 区域需要由可信固件以适当方式提供，而不是随意构造一个 `regions = <&region 0x7>` 就当作合法配置。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 18.3.4 `possible-harts` 与 `assigned_harts` 不能混淆

考虑一个 16-hart SoC。

假设 Hart 0～15 在不同时间都可能执行 Linux 或 OP-TEE。

那么：

```
possible-harts = {0, 1, ..., 15}
```

可以表示这些 Hart 有资格执行某个 Domain。

但不意味着：

```
assigned_harts = {0, 1, ..., 15}
```

在任何时刻都同时成立。

OpenSBI 的 Domain Context 模型要求每个 Hart 在某一时刻归属于一个当前 Domain Context。

对于支持 Hart 在两个 Domain 之间动态切换的方案，软件还要维护可切换目标、正在运行的 Context 以及安全策略。

这直接影响 Chapter 17 的 SMP 问题。

例如，Hart 3 正在执行 Linux 时，能否收到 OP-TEE Domain 的 Interrupt？

不能仅凭 `possible-harts` 得出结论，还必须分析 Interrupt Routing、Domain Ownership 和 Firmware Switch 策略。

## 18.3.5 为什么推荐不同 Domain 使用不同 DT？

考虑把全部 Domain Policy 原封不动地传给 Linux：

```
chosen {
    opensbi-domains {
        ...
        tee-domain {
            ...
            next-addr = <...>;
        };
    };
};
```

这不一定直接破坏硬件隔离，因为 DT 本身不是 PMP。

但它可能暴露不必要的内部信息，并且允许 Linux 驱动误识别自己并不拥有的设备。

更重要的是，如果后续固件错误地把 Linux 修改后的 DT 当作新的可信 Policy，就可能引入配置注入问题。

OpenSBI 官方文档建议为不同 Domain 提供独立 DTB，或在把 DT 传给下一级时清理 Domain Configuration。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



我建议采用以下模型：

```
Trusted Boot Firmware
         |
         +-- Verified Platform / Domain Policy
         |          |
         |          v
         |       OpenSBI
         |          |
         |          +-- Install HW Isolation
         |          +-- Assign Hart Ownership
         |
         +-- Sanitized Linux DTB
         |          |
         |          v
         |        Linux
         |
         +-- Trusted OP-TEE DTB
                    |
                    v
                  OP-TEE
```

但这里还有一个更深层的安全问题：

DTB 的完整性由谁保证？

如果上一级 Bootloader 不可信，即使它传递的 DTB 格式完全合法，也不能让 OpenSBI 盲目相信其中的安全策略。

对涉及安全隔离的 DT 配置，应该采用可信来源、认证或强制验证机制，并在初始化后将 Policy 固化到不可由 REE 修改的状态。

这是 Device Tree 在 TEE 系统中的核心设计思想：

> DT 描述系统配置，但只有在可信启动链和硬件强制访问控制的配合下，它才能参与建立可信的安全策略。

# 18.4 Interrupt Controller、UART、Timer 等外设配置

先修正上一节内存示意图中的一个容量标签：`0x92000000–0xBFFFFFFF` 实际为 736 MiB，不是 880 MiB。整个 1 GiB 示例地址空间仍然成立，其余 `0x91400000–0x91FFFFFF` 为 12 MiB。

这一节讨论真正的 Platform Hardware Bring-up。

有一个很重要的工程顺序：

先确保 Console → Timer → Interrupt → SMP 正常，再启用完整的 TEE Communication 和 User TA。

因为后面的组件都会依赖前面的基础能力。

## 18.4.1 UART：为什么 Console 是第一优先级？

真实芯片的 Bring-up 中，UART 并不只是一个调试工具。

在 MMU、Interrupt 和 Trusted Thread 尚未正常运行之前，它常常是最可靠的故障观察渠道。

对于 OP-TEE，我们需要区分两个阶段：

```
Early Boot
    |
    | MMU disabled / limited mappings
    | Polling UART
    v
Normal Boot
    |
    | MMU enabled
    | UART mapped into VA
    | Console registered
    v
Runtime Logging
```

早期 Console 可能采用物理地址或者 Identity Mapping。

进入正常运行后，UART MMIO 需要具备有效的虚拟映射。

如果错误地把一个 Secure UART 配置为无效的 Physical Memory Type，可能出现：

- 早期串口打印正常。
- OP-TEE 开启 MMU 后立即无法打印。
- 修改 Trace 级别时触发异常。
- 多核日志输出交错或丢失。

### 实际源码：`plat-virt/main.c`

当前 OP-TEE RISC-V `plat-virt/main.c` 中，有下面这段代码：

```
#ifdef CFG_16550_UART
static struct ns16550_data console_data __nex_bss;

register_phys_mem_pgdir(MEM_AREA_IO_NSEC,
                        UART0_BASE,
                        CORE_MMU_PGDIR_SIZE);
#endif
```

以及：

```
#ifdef CFG_16550_UART
void plat_console_init(void)
{
    ns16550_init(&console_data, UART0_BASE,
                 IO_WIDTH_U8, 0);
    register_serial_console(&console_data.chip);
}
#endif
```

这里是对当前代码的缩进整理，符号与调用来自实际 upstream。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们来理解一下。

`register_phys_mem_pgdir()`

告诉 OP-TEE 的 MMU 初始化逻辑，这段物理 MMIO 地址需要被纳入内存映射。

`MEM_AREA_IO_NSEC`

这是 OP-TEE 内部的内存类型描述，在当前示例中该 UART 被作为 Non-Secure I/O 映射。

但需要注意：在 RISC-V 上，`MEM_AREA_IO_NSEC` 这个 OP-TEE 软件分类，并不等价于 Arm TrustZone 的硬件 NS Bit。

它本身不会自动把一个外设配置为安全或非安全设备。

真实 SoC 的外设保护仍依赖 PMP、SoC Firewall、Device Ownership 等机制。

`register_serial_console()`

将 UART 驱动注册为 OP-TEE Console。

因此 Console 移植并不只是替换 UART Base Address。

你需要检查：

1. UART 寄存器接口是否兼容 NS16550。
2. UART Clock 是否已经打开。
3. Pinmux 是否完成配置。
4. UART Base Address 是否可以由当前 Domain 访问。
5. MMU 启用后是否仍存在正确的映射。
6. 多个 Domain 是否共享 UART。
7. Reset/Suspend 后 UART 状态是否保持。

### 一个重要的安全问题：Linux 与 OP-TEE 共用 UART

如果 Linux 和 OP-TEE 使用同一个物理 UART：

```
Linux Domain -----+
                  |
                  v
              UART MMIO
                  ^
                  |
OP-TEE Domain ----+
```

那么这个 UART 并不具备天然的数据机密性。

Linux 可能读取或修改可访问的 UART 寄存器，观察日志流，或干扰 OP-TEE 输出。

因此，生产环境不应该通过共享 Console 输出密钥、用户机密数据或敏感安全状态。

更安全的选择通常是：

- Debug Build 使用 UART。
- Release Build 限制或关闭敏感日志。
- 如硬件支持，可使用独立 Trusted UART。
- 或将可信日志存储在受保护内存中，经授权接口读取。

## 18.4.2 Timer：为什么真实 SoC 上经常出问题？

RISC-V 的 Timer 设计和 Arm Generic Timer 有显著区别。

在 RISC-V 中，必须明确区分：

- 时间计数来源，例如 `time` CSR 可见的计数值。
- Timer Frequency。
- Timer Compare/Programming 机制。
- 计时器中断最终被递送到哪个特权级或 Domain。

对于运行在 S-mode 的软件，读取 `time` 并不等于拥有直接访问 M-mode Timer Compare 寄存器的权限。

在 OpenSBI 管理的系统中，S-mode 通常可以使用 SBI TIME Extension 请求设置下一次 Timer Event；如果平台支持 Sstc 等扩展，也可能有其他合法实现方式。

### 当前 `plat-virt` 的配置

```
$(call force,CFG_RISCV_TIME_SOURCE_RDTIME,y)
CFG_RISCV_MTIME_RATE ?= 10000000
```

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

这里要特别关注 `CFG_RISCV_MTIME_RATE`。

假设真实 SoC 的 `time` 计数频率为 25 MHz，而 OP-TEE 错误使用 10 MHz。

对于 1 ms 的预期延时，软件可能错误地计算成：

```
Expected ticks:
25,000,000 / 1000 = 25,000

Configured ticks:
10,000,000 / 1000 = 10,000
```

如果计数器实际运行在 25 MHz，那么使用 10,000 ticks 对应的实际时间只有：

\\[ \frac{10000}{25000000}=0.0004s=0.4ms \\]

这类错误可能造成：

- Timeout 不准确。
- Watchdog 意外触发。
- TA 超时判定异常。
- Driver Polling 时间过短。
- 某些同步问题仅在真实芯片上出现。

同时，Timer Frequency 还可能由 DT 的 `/cpus/timebase-frequency` 描述，并被 Linux/OpenSBI 等使用。

因此，必须保证所有软件组件对实际计数器频率具有一致的理解。

## 18.4.3 Interrupt Controller：PLIC 与 AIA 的平台差异

当前 RISC-V OP-TEE `plat-virt` 已经包含 PLIC、APLIC Direct Mode，以及 APLIC MSI + IMSIC 的构建配置。

例如：

```
CFG_RISCV_PLIC ?= y
CFG_RISCV_APLIC ?= n
CFG_RISCV_APLIC_MSI ?= n
CFG_RISCV_IMSIC ?= n
```

在实际 `main.c` 中：

```
#ifdef CFG_RISCV_PLIC
void boot_primary_init_intc(void)
{
    plic_init(PLIC_BASE);
}

void boot_secondary_init_intc(void)
{
    plic_hart_init();
}
#endif
```

以及：

```
void interrupt_main_handler(void)
{
    if (IS_ENABLED(CFG_RISCV_PLIC))
        plic_it_handle();
    else if (IS_ENABLED(CFG_RISCV_APLIC))
        aplic_it_handle();
    else if (IS_ENABLED(CFG_RISCV_APLIC_MSI) &&
             IS_ENABLED(CFG_RISCV_IMSIC))
        imsic_it_handle();
}
```

这些都是当前 upstream 中实际存在的代码。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里有几个重要的设计细节。

### Primary Initialization 与 Secondary Initialization

为什么要区分：

```
boot_primary_init_intc();
boot_secondary_init_intc();
```

因为 Interrupt Controller 初始化可能有两类状态：

- Global Controller State：由 Primary Hart 初始化。
- Per-Hart State：每个 Hart 都需要单独初始化。

以 AIA/IMSIC 为例，每个 Hart 可能具有不同的 Interrupt File、Threshold、Enable 和 Pending 状态。

如果只在 Primary Hart 初始化，Secondary Hart 即使成功启动，也未必具备正确处理中断的能力。

### Interrupt Source Ownership

假设真实 SoC 有：

```
UART IRQ  = 10
Crypto IRQ = 35
HSM IRQ    = 42
PCIe IRQ   = 64
```

我们不能简单地把所有 IRQ 都开放给 Linux。

应该先定义：

| IRQ    | Owner            | 允许的接收者              |
| ------ | ---------------- | ------------------- |
| UART   | REE 或共享          | 由 Console Policy 决定 |
| Crypto | TEE              | OP-TEE 或可信 Firmware |
| HSM    | Trusted Firmware | 特定可信接收端             |
| PCIe   | REE              | Linux / Hypervisor  |

之后才配置 PLIC/APLIC/IMSIC 的路由和访问权限。

### 一个真正的安全问题

如果 Linux 可以直接访问 APLIC 的关键路由寄存器，那么它可能修改 Secure Device 的 Interrupt Destination。

例如：

```
Secure Crypto Engine
        |
        | IRQ
        v
      APLIC
        |
        +----> OP-TEE (expected)
        |
        +----> Linux (malicious reroute)
```

是否能够成功重定向取决于硬件的权限和 Delegation 规则。

如果硬件允许不可信软件操作这些安全敏感寄存器，就说明 Security Architecture 存在缺陷。

因此在真实 SoC 上，Interrupt Ownership 必须和 MMIO Ownership 一起设计。

## 18.4.4 Device Tree 中的 Interrupt Topology

以标准 PLIC 为例，一个典型 Linux DT 节点可能类似：

```
plic: interrupt-controller@c000000 {
    compatible = "riscv,plic0";
    reg = <0x0 0x0c000000 0x0 0x04000000>;

    interrupt-controller;
    #interrupt-cells = <1>;

    interrupts-extended =
        <&cpu0_intc 11>,
        <&cpu1_intc 11>;

    riscv,ndev = <95>;
};
```

这只是教学示意，`cpu0_intc`、`cpu1_intc` 需要在完整 CPU DT 中正确定义，实际中断上下文列表也必须符合对应 SoC 的 PLIC Integration 和 Binding。

其中需要区分：

- `reg`：PLIC MMIO 地址范围。
- `interrupt-controller`：声明该节点是中断控制器。
- `interrupts-extended`：描述关联的 CPU Interrupt Context。
- `riscv,ndev`：硬件支持的外部 Interrupt Source 数量。

对于更现代的 SoC，如果使用 APLIC + IMSIC，DT 拓扑会不同。不能简单把 PLIC 的 Compatible 替换成 APLIC 就完成移植。

建议：Bring-up 期间从实际 DT 解析出 Hart/Interrupt Context 映射，并与硬件平台手册逐项对照。

# 18.5 RPMI / MPXY Transport 的 Device Tree 描述

这一节需要结合 Chapter 13 和 Chapter 15 的内容理解。

前面我们已经知道：

```
Linux
    |
    | SBI ECALL
    v
OpenSBI
    |
    | MPXY / RPMI TEE
    v
OP-TEE Domain
```

那么 Device Tree 到底需要描述什么？

## 18.5.1 MPXY 的本质不是一个普通 MMIO Mailbox

传统 Mailbox Driver 往往是：

```
Client
   |
   v
Mailbox Controller Driver
   |
   v
MMIO Doorbell / Shared Registers
```

SBI MPXY 则提供 Supervisor Software 可以通过 SBI 调用访问的消息代理接口。

RPMI 是在这类传输之上承载的平台服务消息协议之一。

RISC-V RPMI 规范明确允许 M-mode Firmware 将受限的 RPMI Transport 通过 SBI MPXY 提供给 S-mode。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



它的关键价值在于：

Linux 不需要直接获得所有底层 Mailbox 或安全控制寄存器的访问权限。

请求可以由 M-mode Firmware 中介，根据 Channel、Service Group 和安全策略处理。

## 18.5.2 当前 Linux DT Binding 有过明显演进

这个细节值得了解，因为你今后可能直接参与 upstream 开发。

2026 年 9 月的 RFC 曾提出：

```
compatible = "riscv,rpmi-mpxy-tee";
```

并在单个节点中描述 Per-Hart Mailbox Channel 及可选通知中断。

但 2026 年 10 月 8 日的 RFC v2 已提出不同的设计：使用 `riscv,rpmi-tee`，只要求一个 Mailbox Reference，通知信息通过 Firmware Feature Discovery 获取。

[image](https://www.google.com/s2/favicons?domain=https://marc.info\&sz=32)

MARC+1



10 月 RFC v2 的示例是：

```
rpmi-tee {
    compatible = "riscv,rpmi-tee";
    mboxes = <&mpxy_mbox 0x3000 0x0>;
};
```

这里 `&mpxy_mbox` 代表 MPXY Mailbox Provider，后面的 Cells 需要按照该 Provider 的 Binding 解读。

`0x3000` 等参数来自 RFC 示例，不应该在实际项目中直接硬编码而不核实。

### 为什么强调 RFC 状态？

因为如果你在真实 SoC 中直接复制某一版 RFC 的 Binding，后续 Kernel Upgrade 时可能遇到：

```
DTB built for RFC v1
        |
        v
Linux Driver updated to RFC v2
        |
        v
Compatible / Mailbox Binding mismatch
        |
        v
TEE Device probe failed
```

因此，对于仍在演进的 Transport，必须锁定一组匹配的 Firmware、Linux Driver 和 DT Binding 版本。

## 18.5.3 OP-TEE 如何发现 MPXY Channel？

当前 `optee_os` 已经有对应代码：

```
core/arch/riscv/kernel/
    sbi_mpxy.c
    sbi_mpxy_rpmi.c
```

在 `sbi_mpxy_rpmi.c` 中，实际存在：

```
void sbi_mpxy_rpmi_probe_channels(void);
```

其主要流程可以概括为：

```
Probe MPXY Shared Memory Size
        |
Get MPXY Channel Count
        |
Get Channel IDs
        |
Read Channel Attributes
        |
Check Protocol == RPMI
        |
Read RPMI Attributes
        |
Allocate Notification Buffer
        |
Register Usable Channels
```

这里的函数调用路径与实际源码一致。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



值得进一步研究的地方是：

为什么不能只根据 Device Tree 中存在一个 TEE 节点，就认为 Transport 已经可用？

因为还必须确认：

- SBI MPXY Extension 存在。
- 所需 Message Protocol 被支持。
- RPMI Service Group 可用。
- Channel ID 合法。
- Mailbox Shared Memory 能正确配置。
- Transport ABI 版本兼容。
- 请求能到达预期 Domain。
- Per-Hart 运行模型符合 Firmware 约束。

因此，TEE Device Probe 应当被设计为一个具备明确失败状态的过程，而不是简单返回成功。

## 18.5.4 一个常见的 Per-Hart Transport Bug

假设有 16 个 Hart，其中 Hart 0～7 可以使用一个 TEE Transport，但 Firmware 的 MPXY Channel 绑定具有 Per-Hart 约束。

错误设计：

```
/* Conceptual incorrect design */
global_channel = get_channel_for_hart(0);

void tee_call(void)
{
    send_message(global_channel, ...);
}
```

如果 Hart 3 上的 Linux Task 使用 Hart 0 的 Channel，可能出现：

- Firmware 拒绝请求。
- Channel Busy。
- 请求响应关联错误。
- 竞争条件。
- 不同 Hart 的通知状态错乱。

正确设计需要根据实际协议、Firmware Channel Binding 和 Linux Mailbox Driver 的语义，明确每个 Hart 应当使用什么 Channel，并制定任务迁移和同步策略。

在涉及 Domain Switch 的 Transport 中，还需要考虑：

```
Task starts on Hart 3
       |
Transport request submitted
       |
Hart enters OP-TEE Domain
       |
Transport response returns
       |
Task may later migrate to Hart 5
```

如果协议要求一个同步调用链绑定到发起 Hart，那么必须在相应执行阶段保证这个条件，而不能假设 Linux Scheduler 永远不会迁移任务。

这就是为什么 TEE Transport 设计会涉及 Kernel Scheduler、Per-CPU State、SBI 和 Domain Switching，而不只是 Mailbox Driver。

# 18.6 PMP / WorldGuard / IOPMP Policy 配置

前面讨论了 OpenSBI Domain Device Tree，但真正决定系统安全性的，是底层硬件是否能够执行这些权限策略。

这一节的核心问题是：

当 OpenSBI 宣称 Linux Domain 无权访问 OP-TEE Memory 时，硬件究竟如何保证这个承诺？

## 18.6.1 CPU PMP 与系统级隔离之间的区别

考虑下面这个系统：

```
                 SoC Interconnect
                       |
        +--------------+--------------+
        |              |              |
      CPU Hart       DMA Engine      PCIe RC
        |              |              |
       PMP          IOMMU /        IOMMU /
        |           IOPMP          Firewall
        |              |              |
        +--------------+--------------+
                       |
                    DDR RAM
                       |
              OP-TEE Trusted DDR
```

对 CPU Hart 发起的 S/U-mode Memory Access，PMP 可以提供物理地址访问控制。

但对 DMA Engine、PCIe Endpoint 等其他 Master 发起的 Memory Access，CPU PMP 通常不在其数据通路上。

因此，需要平台特定的其他保护机制。

例如：

- IOPMP：可以为 I/O Master 的事务提供访问权限控制。
- IOMMU：用于 DMA 地址翻译与访问限制，是否具备完整安全隔离能力取决于其配置权限和系统集成。
- WorldGuard：若 SoC 实现该类硬件隔离机制，可以根据资源和 World 标识实施访问控制。
- SoC Memory Firewall：根据 Master ID、地址、Privilege 或 Security Attribute 限制访问。

这些机制在不同 SoC 中不一定同时存在，也不能假定其安全特性完全等价。

### 关键区别

| 机制                      | 主要保护对象          | 主要防御范围             |
| ----------------------- | --------------- | ------------------ |
| MMU / PTE               | 当前虚拟地址空间        | 地址空间和页级权限          |
| PMP                     | CPU Hart 的物理访问  | 限制 S/U-mode 物理地址访问 |
| IOMMU                   | DMA 地址空间        | DMA 映射和设备访问范围      |
| IOPMP                   | I/O Master 物理访问 | 外设发起的物理访问权限        |
| SoC Firewall            | 片上互连及目标资源       | Master 到内存/外设的访问   |
| Linux `reserved-memory` | 正常内存分配          | 避免合作式软件使用预留区域      |

在 Staff Engineer 级别的架构讨论中，应当避免说出：

> 我们使用 PMP 保护 Trusted DDR，所以整个 SoC 的 DMA 也已经安全。

这在没有额外硬件保障和验证之前，是不能成立的。

## 18.6.2 如何从 Threat Model 设计隔离策略？

建议首先列出攻击者能够控制的实体。

假设 REE 已经完全被攻破，攻击者拥有 Linux Kernel Privilege。

那么他可能控制：

- S-mode 页表。
- 普通 DMA Driver。
- PCIe 设备控制。
- 非可信外设 MMIO。
- 普通中断的路由配置。
- Linux Device Tree 的运行时副本。
- REE Shared Memory。

你的安全目标至少包括：

1. Linux CPU 不能读写 OP-TEE Private RAM。
2. Linux DMA 不能访问 OP-TEE Private RAM。
3. Linux 不能修改安全硬件的访问策略。
4. Linux 不能通过非可信中断控制器重定向关键安全中断。
5. Linux 不能通过 Shared Memory 让 OP-TEE 越界访问。
6. Linux 不能通过正常 Power/Reset 接口绕过隔离或泄露敏感状态。

因此，平台权限设计不应该只根据软件组件命名，而应该采用资源所有权矩阵。

| 资源                    | OpenSBI     | OP-TEE   | Linux | DMA   |
| --------------------- | ----------- | -------- | ----- | ----- |
| OpenSBI RAM           | 受保护的 M-mode | 拒绝       | 拒绝    | 拒绝    |
| OP-TEE Private RAM    | 可信初始化或受控访问  | 允许       | 拒绝    | 默认拒绝  |
| Linux RAM             | 受控访问        | 按需访问     | 允许    | 按设备策略 |
| Shared Memory         | 协议需要时访问     | 允许       | 允许    | 按需要开放 |
| HSM Control Registers | 可信管理        | 通常通过服务接口 | 拒绝    | 拒绝    |
| Secure Crypto MMIO    | 可信管理        | 允许       | 拒绝    | 视硬件设计 |
| REE UART              | 平台控制        | 可选       | 允许    | 通常无关  |

这里的“允许”不代表一定给予 R/W/X 全部权限，而表示按最小权限原则设计具体访问权。

## 18.6.3 Domain Switch 时权限如何变化？

假设一个 Hart 从 Linux Domain 切换到 OP-TEE Domain。

概念上：

```
Linux Domain Active
        |
        | Linux can access Linux RAM
        | Linux cannot access TEE RAM
        v
Trap to M-mode Firmware
        |
        | Save Linux Context
        | Apply TEE Domain Isolation
        | Restore TEE Context
        v
TEE Domain Active
        |
        | OP-TEE can access TEE RAM
        | OP-TEE accesses SHM through approved mapping
```

但这里的真实实现会受到硬件能力和 Firmware Framework 的约束。

不能仅仅说“重新写 PMP Entry 即可”。

你需要考虑：

问题 A：PMP Entry 是否已经 Locked？

若使用无法修改的 Locked PMP Entry，就不能指望后续 Domain Switch 可以随意改变它。

问题 B：保护操作有没有时间窗口？

如果 Firmware 在解除旧保护后、建立新保护前允许中断或不可信执行，就可能产生安全窗口。

问题 C：其他 Hart 的 PMP 是否一致？

标准 PMP 配置具有 Hart-local 特征。Hart 0 的 PMP 配置不会自动保证 Hart 1 已经拥有相同策略。

问题 D：DMA 权限是否随着 Domain Switch 改变？

不一定。

某些 SoC 的 IOPMP/Firewall 权限可以独立于 CPU Domain 持续生效，部分平台可能需要进行额外的资源所有权交接。

这必须根据硬件架构专门设计。

## 18.6.4 更重要的问题：硬件如何知道请求来自哪个 Domain？

假设 Linux 和 OP-TEE 都运行在 S-mode，并且可能在同一个 Hart 上交替运行。

如果互连只识别：

```
Master = CPU Hart 0
Privilege = S-mode
```

那么互连是否能够区分当前访问来自 Linux Domain 还是 OP-TEE Domain？

答案是：未必能够。

这正是 Domain Isolation 在硬件集成时必须明确的问题。

有两类典型做法。

方式 A：由可信 M-mode 固件切换保护配置。

每次 Domain Switch 时重新配置相应的权限，使同一 Hart 在不同时间具有不同的可访问范围。

方式 B：由硬件传播额外的安全 Domain 标识。

例如某些 World-based 隔离机制可以使 Memory Transaction 携带额外的资源访问标识。

但这需要 CPU、互连、外设及安全控制模块之间有一致的硬件支持。

不能把软件中的 `domain_id` 自动等同于硬件总线上的 Security ID。

## 18.6.5 Hardware Isolation Framework 的移植职责

在真实平台上，OpenSBI Platform Code 至少需要解决：

- 如何枚举可用的 Hardware Protection Devices。
- 如何给每个 Domain 分配合法的资源范围。
- 如何在 Domain Entry/Exit 时调用对应保护回调。
- 多 hart 下何时配置和同步 Hart-local 权限。
- 如何处理硬件访问失败。
- 如何确保非可信软件无法覆盖策略。
- 如何验证所有 DMA Master 均受到必要保护。

对于不能动态修改的硬件保护策略，则应采用启动阶段的静态配置，并确保其与后续 Domain 切换模型兼容。

## 18.6.6 最重要的验证：Negative Security Test

建议至少做如下测试：

```
Test 1:
Linux CPU direct read OP-TEE RAM
Expected: Access denied

Test 2:
Linux CPU direct write OP-TEE RAM
Expected: Access denied

Test 3:
Untrusted DMA reads OP-TEE RAM
Expected: Transaction blocked

Test 4:
Linux modifies Firewall registers
Expected: Access denied

Test 5:
Linux accesses Secure Crypto MMIO
Expected: Access denied

Test 6:
TEE Domain accesses its permitted RAM
Expected: Success
```

注意，“Access denied”的具体表现可能是 Access Fault、Bus Error、IOMMU/Firewall Fault 或其他平台定义的错误响应。

这些测试的意义远大于单纯运行 `xtest`。

因为它们验证的是：

即使不可信软件主动攻击系统，硬件安全策略是否仍然有效。

# 18.7 OP-TEE Platform Driver 与 Device Initialization

现在进入真实的 OP-TEE Core Porting。

我们已经知道硬件地址、中断和安全策略。

接下来必须将这些信息转换为 OP-TEE 的 Platform Implementation。

## 18.7.1 为什么要有 `plat-<platform>`？

OP-TEE 的通用内核不应该直接依赖具体 SoC 的 UART Base Address、Interrupt Controller Registers 或 DDR Layout。

因此其代码结构大体分为：

```
optee_os/
  core/
    arch/
      riscv/
        kernel/
        mm/
        plat-virt/
        plat-sifive/
        ...
    kernel/
    mm/
    drivers/
```

Platform-specific Code 负责把 SoC 的实际硬件配置接入架构无关的通用机制。

当前 `plat-virt` 的核心文件包括：

```
core/arch/riscv/plat-virt/
    conf.mk
    platform_config.h
    main.c
    sub.mk
```

这些文件及其内容已经与当前 upstream 核实。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+3



文件职责：

| 文件                  | 主要职责                                    |
| ------------------- | --------------------------------------- |
| `conf.mk`           | 编译特性、Core 数量、内存与驱动配置                    |
| `platform_config.h` | 物理地址、外设参数、Platform 常量                   |
| `main.c`            | Memory Registration、Console、Interrupt 等 |
| `sub.mk`            | 平台源码及头文件构建规则                            |

这里需要避免一种常见错误：

不要把所有新的 SoC 初始化代码直接写到 RISC-V Architecture Common Code 中。

如果某些逻辑仅在一款芯片上成立，应留在 Platform 层或者独立的 Platform Driver 中。

只有真正跨平台通用的机制才适合进入 Architecture Common Layer。

## 18.7.2 一个新 Platform 的最小目录设计

假设目标平台称为 `acme`。

建议从下面的结构开始：

```
core/arch/riscv/plat-acme/
    conf.mk
    platform_config.h
    main.c
    sub.mk
```

根据硬件复杂度，后续可以再拆分：

```
    intc.c
    console.c
    security.c
    power.c
```

不要刚开始就拆出十几个文件。首先应该复用当前架构接口和现有驱动。

### `conf.mk` 示例

下面是适用于教学 SoC 的配置草案，实际需要根据目标分支和已支持的构建选项调整：

```
$(call force,CFG_RV64_core,y)
$(call force,CFG_RISCV_S_MODE,y)
$(call force,CFG_RISCV_M_MODE,n)

CFG_TEE_CORE_NB_CORE ?= 16
CFG_NUM_THREADS ?= 8

CFG_DT ?= y

CFG_CORE_RESERVED_SHM ?= n
CFG_CORE_DYN_SHM ?= y

CFG_RISCV_PLIC ?= y
CFG_16550_UART ?= y

CFG_RISCV_SBI ?= y
CFG_RISCV_SBI_MPXY ?= y
CFG_RISCV_SBI_MPXY_RPMI ?= y

CFG_TDDRAM_START ?= 0x90000000
CFG_TDDRAM_SIZE ?= 0x01000000
CFG_TEE_RAM_VA_SIZE ?= 0x00200000

supported-ta-targets = ta_rv64
```

`CFG_NUM_THREADS=8` 只是一个初始容量选择，不代表 16-hart 系统中 8 个 Trusted Thread 一定足够。

应该结合并发请求模型、RPC/Yield 行为、内存开销和性能测试确定。

另外，示例中使用 `?=` 的配置项不一定具有 `plat-virt` 中 `force` 的相同安全约束。迁移实际项目时，需要区分可调参数与必须强制的架构配置。

### `platform_config.h` 示例

```
#ifndef PLATFORM_CONFIG_H
#define PLATFORM_CONFIG_H

#include <mm/generic_ram_layout.h>
#include <riscv.h>

#define DRAM_BASE       0x80000000
#define DRAM_SIZE       0x40000000

#define UART0_BASE      0x10000000

#define PLIC_BASE       0x0c000000
#define PLIC_REG_SIZE   0x04000000

#define PLAT_THREAD_EXCP_FOREIGN_INTR \
    (CSR_XIE_EIE | CSR_XIE_TIE | CSR_XIE_SIE)

#define PLAT_THREAD_EXCP_NATIVE_INTR (0)

#endif
```

这使用了与当前 RISC-V `plat-virt` 相同的部分接口形式，但地址仍是教学假设。

对于真实 SoC，还必须检查：

- DRAM 是否连续。
- MMIO 是否超出 32-bit Physical Address。
- PLIC 地址空间大小。
- Interrupt Exception Routing。
- RISC-V ISA 扩展。
- Cache Line Size。
- PMP Granularity。
- Firmware 是否保证相关访问权限。

## 18.7.3 `main.c` 中的主要初始化职责

参考当前 `plat-virt` 实现，可以从以下代码模式入手：

```
#include <console.h>
#include <drivers/ns16550.h>
#include <drivers/plic.h>
#include <kernel/boot.h>
#include <platform_config.h>

/* Register physical memory regions */
register_ddr(DRAM_BASE, DRAM_SIZE);

static struct ns16550_data console_data __nex_bss;

register_phys_mem_pgdir(MEM_AREA_IO_NSEC,
                        UART0_BASE,
                        CORE_MMU_PGDIR_SIZE);

void plat_console_init(void)
{
    ns16550_init(&console_data, UART0_BASE,
                 IO_WIDTH_U8, 0);
    register_serial_console(&console_data.chip);
}

void boot_primary_init_intc(void)
{
    plic_init(PLIC_BASE);
}

void boot_secondary_init_intc(void)
{
    plic_hart_init();
}
```

这是一份根据当前 upstream 平台接口组合的教学示例，不是直接从某个现有 `plat-acme` 文件复制而来。

`register_ddr()` 只是向 OP-TEE 的内存管理框架登记 DDR 信息，并不自动保证整个 DDR 均属于可信 RAM，也不会自动配置 SoC Firewall。

## 18.7.4 初始化顺序为什么非常重要？

当前 RISC-V OP-TEE `boot.c` 中，Primary 初始化路径包含这样的关键顺序：

```
Early Boot / MMU Initialization
        |
        v
thread_init_primary()
        |
        v
boot_primary_init_core_ids()
        |
        v
boot_primary_init_intc()
        |
        v
hart_features_init()
        |
        v
init_tee_runtime()
        |
        v
sbi_mpxy_init()
        |
        v
sbi_mpxy_rpmi_probe_channels()
```

后续 Final Initialization 包含 Driver Initcall 等阶段。这里抽取的是与本章有关的关键步骤，并非全部启动指令。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这个顺序很有意义。

### 为什么要先初始化 Hart ID，再初始化 Interrupt Controller？

因为 Interrupt Controller 的某些 Per-Hart Context 需要依据 Hart ID 或逻辑 CPU Index 建立映射。

如果实际 SoC 的 Hart ID 是：

```
0, 1, 4, 5, 8, 9, 12, 13
```

但 Platform Driver 错误假设：

```
hart_id == cpu_index
```

那么部分 Hart 的中断配置可能被写入错误的硬件 Context。

这种问题在 QEMU 中不一定容易暴露，因为测试环境可能恰好使用连续的 Hart ID。

### 为什么要晚于基础 Runtime 初始化再 Probe RPMI？

RPMI Probe 需要使用动态内存、日志、SBI 通信及其他运行时基础设施。

如果把相关操作移动到过早阶段，就可能在 MMU、Heap 或 Core-local State 尚未准备好的情况下访问这些资源。

因此，一个 Platform Port 的 Initcall 必须同时满足两个条件：

1. 它依赖的服务已经初始化。
2. 所依赖资源的安全权限已经配置完成。

## 18.7.5 如何设计 Platform-specific Driver？

假设新 SoC 有一个独立的 Secure Crypto Engine。

我建议将驱动分成三部分。

第一部分：Hardware Access

```
crypto_hw.c

- register read/write
- engine reset
- DMA descriptor
- interrupt acknowledge
- device status
```

第二部分：Security & Resource Management

```
crypto_platform.c

- claim device ownership
- apply firewall policy
- map MMIO
- initialize clock/reset
- register interrupt
```

第三部分：OP-TEE Crypto Integration

```
crypto_provider.c

- key handle management
- symmetric / asymmetric operations
- hardware operation submission
- return TEE_Result
```

这种设计能够减少对 OP-TEE Core 的侵入。

同时有利于将 Hardware Driver 与 Crypto Framework Adapter 分别维护。

### MMIO 注册不是硬件授权

假设代码中执行：

```
register_phys_mem_pgdir(MEM_AREA_IO_SEC,
                        CRYPTO_BASE,
                        CRYPTO_SIZE);
```

这并不能证明当前 Domain 已经具备访问 Crypto Engine 的硬件权限。

它只是 OP-TEE 内存映射过程中的一项声明。

如果 OpenSBI 或 SoC Firewall 不允许该访问，OP-TEE 仍可能遭遇 Load/Store Access Fault。

反过来，如果 MMU 没有映射一个硬件允许访问的 MMIO 地址，软件也可能发生 Page Fault。

因此必须区分：

```
MMU Translation Permission
          +
Physical Hardware Permission
          =
Effective Access Result
```

这是排查 MMIO Fault 的基础方法。

# 18.8 Trusted Firmware、U-Boot/UEFI、Linux 之间的数据交接

这一节将 Platform Porting 上升到完整 Boot Architecture 层面。

对真实 RISC-V 服务器 SoC，尤其是具有独立 RSS/HSM 的平台，这一点非常重要。

## 18.8.1 多级 Firmware 需要传递什么？

假设使用如下启动链：

```
SoC Reset
    |
    v
Boot ROM
    |
    v
RSS / HSM Firmware
    |
    | Authenticate firmware
    | Establish root-of-trust state
    v
Primary CPU Boot Firmware
    |
    | DDR initialization
    | Load OpenSBI / OP-TEE / UEFI
    v
OpenSBI (M-mode)
    |
    | Build Domain isolation
    | Initialize Hart / Protection
    v
UEFI (S-mode REE Domain)
    |
    | Boot services
    | OS image loading
    v
Linux
    |
    | TEE Transport
    v
OP-TEE Domain
```

这是合理的概念架构，但不代表所有平台都按这个顺序执行。

OP-TEE Domain 可能在 UEFI 启动之前完成初始化，也可能采用不同的生命周期和调用模式。

最重要的是明确各阶段之间的数据交接。

| 数据                       | 产生者                     | 消费者                  | 安全注意点              |
| ------------------------ | ----------------------- | -------------------- | ------------------ |
| Firmware Entry           | 上级可信 Loader             | OpenSBI/UEFI/OP-TEE  | 必须指向合法且经过验证的 Image |
| Memory Layout            | SoC 配置、可信 Boot Firmware | OpenSBI、OP-TEE、Linux | 三者必须一致             |
| Hart Topology            | Platform Firmware / DT  | OpenSBI、OP-TEE、Linux | Hart ID 与逻辑索引不可混淆  |
| Domain Policy            | 可信 Firmware             | OpenSBI              | 必须防止不可信修改          |
| Linux DTB / ACPI         | Firmware                | Linux                | 不应将不可信表格作为安全授权     |
| OP-TEE DTB               | 可信 Firmware             | OP-TEE               | 安全敏感字段须验证          |
| Random Seed              | 可信随机源                   | OP-TEE               | 防止固定、重放或泄露         |
| Secure Boot Status       | RSS/HSM                 | 后续 Firmware          | 需要可信来源与防篡改传递       |
| Event Log / Measurements | 可信启动链                   | Attestation Service  | 必须绑定到认证与测量链        |

这里还要注意：RISC-V UEFI 系统可以采用 ACPI 作为 OS 的硬件发现机制，因此不能假设最终 Linux 一定消费 Device Tree。

OpenSBI 使用 DT 描述 Domain，并不代表 UEFI 启动的 Linux 必然使用同一份 DT。

## 18.8.2 RISC-V Firmware Entry ABI：`a0` 和 `a1`

这是必须深入到汇编层面理解的地方。

在当前 OP-TEE RISC-V `entry.S` 中，`_start` 的代码明确采用：

```
/* a0: hart ID */
/* a1: system DTB address */

mv s0, a0
mv s1, a1
```

并且在定义 `CFG_DT_ADDR` 时，可以使用配置指定的 DT 地址替代传入的 `a1`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这意味着 OP-TEE 入口对 Boot Firmware 有明确的接口预期。

你需要验证：

第一：Entry Privilege 是否正确？

如果 OP-TEE 配置运行在 S-mode，而实际 Boot Firmware 却以错误的 Privilege Mode 进入，后续 CSR 和 Trap 行为可能不符合预期。

第二：`a0` 是否是真实 Hart ID？

不能简单假设 `a0` 是 0～N-1 的逻辑 CPU 编号。

第三：`a1` 指向的 DTB 是否有效？

需要检查：

- 指针是否是有效物理地址。
- DTB 是否位于可访问内存。
- DTB 内容是否完整。
- DTB 是否会在初始化完成前被覆盖。
- DTB 是否来自可信 Firmware。
- 是否存在不一致的 Endianness 或地址 Cell 描述。

第四：是否正确处理 Secondary Hart？

多个 Hart 不能在没有同步机制的情况下同时执行仅允许 Primary Hart 完成的全局初始化过程。

## 18.8.3 实际代码如何保存 DTB 地址？

当前 OP-TEE `boot.c` 中具有如下代码：

```
static unsigned long boot_arg_fdt __nex_bss;

void __weak boot_save_args(
    unsigned long a0 __unused,
    unsigned long a1,
    unsigned long a2 __unused,
    unsigned long a3 __unused,
    unsigned long a4 __unused)
{
    boot_arg_fdt = a1;
}
```

这是实际 upstream 函数，格式经过简化。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里的 `__weak` 表示平台可以在需要时提供自己的实现。

但在自定义实现时，需要谨慎：

- 不要丢失 Firmware 传递的有效信息。
- 不要在 DTB 尚未可访问时使用复杂解析。
- 不要把 Linux 后续可修改的 DTB 当作不可变的安全配置。
- 不要使用未经确认的物理地址进行内存访问。

## 18.8.4 RSS/HSM 如何参与这个过程？

假设 SoC 的 RSS 是一个独立物理 Core，它作为 Root of Trust 负责镜像认证和密钥服务。

应当把它与 OpenSBI 运行的 Application Harts 区分开。

```
Dedicated RSS Core
    |
    +-- Authenticate OpenSBI
    +-- Authenticate OP-TEE
    +-- Authenticate UEFI
    +-- Manage Device Keys
    +-- Provide Attestation Root
    |
    | Trusted Boot Authorization
    v
Application Hart Cluster
    |
    +-- OpenSBI
    +-- OP-TEE
    +-- UEFI / Linux
```

需要特别注意：

RSS 验证了 OP-TEE Image，不代表整个运行环境天然安全。

还必须确保：

1. 被执行的 Image 与认证通过的 Image 一致。
2. Image 加载后没有被非可信 DMA 修改。
3. Domain Policy 没有被替换。
4. OP-TEE 启动地址与链接、加载地址一致。
5. Security Firewall 已经启用。
6. 其他 Hart 不会在安全初始化完成前访问可信资源。

这些属于 Secure Boot 与 Secure Runtime Isolation 的衔接问题。

## 18.8.5 Trusted Handoff 的正确设计

我建议采用一个由可信 Firmware 管理的 Handoff Contract，例如：

```
/*
 * Illustrative internal firmware contract.
 * NOT a standardized OP-TEE ABI.
 */
struct trusted_handoff {
    uint32_t version;
    uint32_t size;

    uint64_t tee_entry;
    uint64_t tee_mem_base;
    uint64_t tee_mem_size;

    uint64_t trusted_fdt;
    uint64_t boot_measurements;

    uint32_t security_policy_version;
    uint32_t flags;
};
```

这里仅是概念示例。

实际实现可以使用已有标准协议、DT 属性或平台定义的结构，不应该因为方便而引入一个与上游兼容性冲突的私有 ABI。

如果采用自定义 Handoff，需要至少考虑：

- 数据结构版本。
- Size 和 Range Validation。
- 内存所在区域的访问权限。
- TOCTOU 防护。
- 与 Secure Boot Measurement 的绑定。
- 向前、向后兼容。
- 失败时的安全处置。

值得特别注意的是，单纯添加 CRC32 并不能提供安全认证。

若攻击者能够修改 Handoff 数据，他同样可以重新计算 CRC32。

Handoff 的可信性应来自受保护的传递路径、可信 Boot Chain 或密码学认证机制。

# 18.9 最小化 RISC-V OP-TEE Platform Port

接下来将前面的技术组合成一套可执行的工程方案。

这里我们使用虚构的 `AcmeRV64`。

## 18.9.1 目标平台规格

| 属性                  | 教学平台                               |
| ------------------- | ---------------------------------- |
| CPU                 | 16 × RV64 Harts                    |
| Boot Firmware       | ROM + RSS + OpenSBI                |
| REE                 | UEFI + Linux                       |
| TEE                 | OP-TEE                             |
| Privilege           | OpenSBI M-mode；Linux/OP-TEE S-mode |
| DDR                 | 1 GiB @ `0x80000000`               |
| Trusted DDR         | 16 MiB @ `0x90000000`              |
| Shared Memory       | 4 MiB 静态窗口（第一阶段）                   |
| Interrupt           | PLIC                               |
| UART                | NS16550-compatible                 |
| TEE Transport       | SBI MPXY + RPMI                    |
| Hardware Protection | PMP + SoC Firewall                 |

为了降低 Bring-up 风险，初始阶段建议只使用一个 Hart 和最基本的 Console，再逐渐启用全部 Hart。

## 18.9.2 设计步骤一：固定完整的 Boot Contract

首先明确：

```
OpenSBI Image
    |
    +-- load address
    +-- entry address
    +-- expected privilege
    +-- initial DTB
    |
    +-- OP-TEE Domain
    |      +-- TEE entry
    |      +-- TEE memory
    |      +-- trusted DTB
    |
    +-- Linux Domain
           +-- UEFI entry
           +-- REE memory
           +-- Linux DTB / ACPI
```

这一步必须与负责 ROM/RSS、UEFI 和 SoC RTL 的工程师共同确认。

尤其要确保 `next-addr` 与实际加载的 Image Entry 一致。

## 18.9.3 设计步骤二：建立 Memory Layout Contract

建议输出一份机器可检查的地址区域表，最少包含：

```
Region        Base        Size        Owner
------------------------------------------------
DDR           80000000    40000000    System
TEE Private   90000000    01000000    OP-TEE
TEE SHM       91000000    00400000    REE + TEE
```

真实配置还需要加入：

- OpenSBI Firmware Region
- RSS/HSM Shared Region
- Bootloader Region
- DMA Pool
- UEFI Runtime Region
- PCIe Aperture
- MMIO Range

并做严格的重叠检查。

关键要求：

```
TEE Private ∩ REE Allocatable = ∅

TEE Private ∩ Untrusted DMA Access = ∅

TEE Private ∩ Untrusted MMIO = ∅
```

后两项是需要由硬件权限验证的安全属性，并不能只检查地址是否重叠。

## 18.9.4 设计步骤三：建立 OpenSBI Domain Policy

至少包含：

```
REE Domain
    |
    +-- Allowed Harts
    +-- REE Memory
    +-- REE Devices
    +-- UEFI Entry

TEE Domain
    |
    +-- Allowed Harts
    +-- Trusted Memory
    +-- Secure Devices
    +-- Shared Memory
    +-- OP-TEE Entry
```

对于 Secure Memory，应当保证 REE Domain 没有最终有效的访问权限。

具体配置可依据当前 OpenSBI Domain DT Binding、Root Region Inheritance 和 Hardware Isolation Framework 实现。

不要只因为某个 Region 没有出现在 DT 中，就假定硬件一定会拒绝该访问。

应该读取并验证实际生效的 PMP / Firewall Policy。

## 18.9.5 设计步骤四：建立基础 Platform Code

以当前 `plat-virt` 为参考，实现：

```
plat-acme/
    conf.mk
    platform_config.h
    main.c
    sub.mk
```

第一阶段只保留：

- Memory Registration
- UART Console
- Hart ID Mapping
- Interrupt Controller
- Timer / SBI Support
- Boot Configuration

不要一开始就实现 HSM、Secure Storage、Hardware Crypto 和所有高级特性。

## 18.9.6 设计步骤五：分层 Bring-up

建议严格按照以下顺序推进。

1. Milestone A：Firmware + OP-TEE Entry

   OP-TEE 能进入 `_start`，设置临时 Stack，并初始化基本 C Runtime。

   成功判据：GDB 可停在 `_start`；Early Console 正常。
2. Milestone B：MMU + Core Initialization

   初始化 Trusted Memory、页表、Core Stack 和 Runtime。

   成功判据：MMU 启用后仍然可以打印；无早期 Page/Access Fault。
3. Milestone C：Single-Hart TEE Execution

   完成一个 Hart 上的 TEE Service 调用链。

   成功判据：能完成最小 TEE 请求和返回。
4. Milestone D：Linux TEE Integration

   Linux Driver、MPXY/RPMI、Shared Memory、OP-TEE 对接。

   成功判据：TEE Device 枚举成功，基本 CA/TA 调用通过。
5. Milestone E：SMP

   启用 Secondary Harts，验证 Per-Hart State、HSM、RFENCE 和并发调用。

   成功判据：多 Hart 连续运行、并发 xtest、TLB/Cache/Interrupt 测试通过。
6. Milestone F：Hardware Security Validation

   验证 REE CPU、DMA、MMIO、Interrupt Routing 对可信资源的权限。

   成功判据：明确的 Negative Security Tests 全部达到预期。

这里有一个容易忽视的实际限制：

当前 RISC-V OP-TEE `boot.c` 在多 Hart 配置中会检查 SBI HSM 和 RFENCE 等能力；例如没有必需的 RFENCE Extension 时会阻止多 Hart 操作继续运行。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



因此，当 16-hart Bring-up 失败时，不应该只检查 OP-TEE 的 Thread 数量，还要检查 OpenSBI 侧是否真正实现了多 Hart 所需的 SBI 功能。

# 18.10 Build、Debug 与 Platform Porting 验证

原大纲的最后一点是最小化 Platform Port。为了保证这章具有独立可操作性，这里进一步补充构建与故障定位流程。

## 18.10.1 建议使用的 QEMU 参考环境

RISE RP016 提供基于 Buildroot 的开发环境，包含 OpenSBI、OP-TEE、U-Boot、Linux 和 QEMU。

其文档中的构建流程为：

```
git clone \
  https://gitlab.com/riseproject/riscv-optee/buildroot.git \
  -b rp016_m1

cd buildroot

make qemu_riscv64_virt_optee_defconfig

make -j"$(nproc)"
```

启动：

```
./output/images/start-qemu.sh
```

启动 Linux 后：

```
xtest
```

这是项目公开文档中的参考构建路径。实际开发需要根据后续 RP016/RP022 分支组合锁定版本，不应默认 `rp016_m1` 已经包含所有最新特性。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



## 18.10.2 如何阅读真实 RISC-V Platform Code？

我建议按以下顺序阅读：

```
1. core/arch/riscv/plat-virt/conf.mk

2. core/arch/riscv/plat-virt/platform_config.h

3. core/arch/riscv/plat-virt/main.c

4. core/arch/riscv/kernel/entry.S

5. core/arch/riscv/kernel/boot.c

6. core/arch/riscv/mm/core_mmu_arch.c

7. core/arch/riscv/kernel/sbi_mpxy.c

8. core/arch/riscv/kernel/sbi_mpxy_rpmi.c
```

这些源码的学习目标不是简单知道函数名，而是回答：

- 哪些参数由 `conf.mk` 决定？
- 哪些信息由 Firmware/DTB 提供？
- 哪些内存区域最终进入 OP-TEE 页表？
- 哪些初始化发生在 MMU 使能前？
- 哪些初始化是 Primary Only？
- 哪些必须在每个 Hart 上执行？
- 哪些访问由 M-mode Firmware 中介？
- 哪些 MMIO 需要获得真正的硬件授权？

## 18.10.3 推荐的 GDB / Log 调试方法

最基础的 ELF 检查：

```
riscv64-linux-gnu-readelf -h tee.elf
riscv64-linux-gnu-readelf -l tee.elf
riscv64-linux-gnu-nm -n tee.elf | less
riscv64-linux-gnu-objdump -dr tee.elf | less
```

需要确认：

- ELF Entry。
- Link Address。
- Loadable Segments。
- `.text` / `.rodata` / `.data` / `.bss`。
- Core Stack、Page Table 的位置。
- Linker 生成的关键 Symbol。

对于已经开启 ASLR 的构建，还必须处理运行时重定位偏移，不能直接把链接地址当作实际执行地址。

### 关键断点

建议在以下位置建立断点：

```
_start

boot_save_args

boot_primary_init_core_ids

boot_primary_init_intc

plat_console_init

sbi_mpxy_rpmi_probe_channels
```

随后记录：

```
PC
SP
TP
satp
sstatus
stvec
sepc
scause
stval
```

这里的 `sepc/scause/stval` 只有在相应 Trap 语境下才具有诊断意义，不能把任意时刻读取的值都解释为最新异常。

## 18.10.4 真实项目中的 Fault 诊断矩阵

| 现象                             | 优先排查项                                       |
| ------------------------------ | ------------------------------------------- |
| `_start` 无法到达                  | Boot Address、Image Load、Security Protection |
| 刚启动就 Instruction Access Fault  | Entry Address、PMP Execute、Image Location    |
| MMU 启动后串口消失                    | MMIO Mapping、UART Clock、Device Permissions  |
| UART 正常但 Linux 无法发现 TEE        | Linux Driver、DT Binding、SBI MPXY/RPMI       |
| 只在 Secondary Hart 崩溃           | Hart ID Mapping、Per-Hart Stack、HSM          |
| TA 访问内存失败                      | TA Page Table、User Permission、PMP           |
| `xtest` 偶发 Timeout             | Trusted Thread、RPC、Timer、Scheduling         |
| 16-hart 下出现 TLB 相关问题           | SBI RFENCE、Shootdown、ASID                   |
| Linux CPU 无法读 TEE RAM，但 DMA 可以 | IOMMU/IOPMP/Firewall                        |
| 只有 Release Build 异常            | Cache、Barrier、Undefined Behavior、优化差异       |

故障排查时最有价值的方法，是建立从硬件权限到软件调用链的分层证据。

例如出现 Store Access Fault 时：

1. 检查错误地址和访问来源。
2. 检查当前 `satp` 以及相应 PTE。
3. 检查 PMP Access Policy。
4. 检查 Firewall/Bus Error。
5. 检查当前 Hart 所属 Domain。
6. 检查是否存在并发 Domain Switch。
7. 最后检查调用者的 Pointer 和 Buffer 生命周期。

注意 RISC-V 的 Page Fault 和 Access Fault 并不等价，前者通常涉及地址翻译和页级权限，后者还可能涉及 PMP、物理内存访问及总线等问题。

# 18.11 综合实操：从 Device Tree 到安全隔离验证

为了把前面的十个小节真正串起来，我建议设计一项综合实验。

## 实验目标

在 RISC-V QEMU 环境下，完成以下工作：

- 分析 QEMU 生成的 DTB。
- 查看 OpenSBI Domain Policy。
- 追踪 OP-TEE 的 Memory Registration。
- 检查 OP-TEE Core 和 User TA 的内存布局。
- 跟踪一次 RPMI/MPXY TEE 调用。
- 设计并执行基础的非法访问测试。

这里把实验分成静态分析与运行验证两部分。

## 实验 A：检查和修改 Device Tree

安装工具：

```
sudo apt install device-tree-compiler
```

对已经取得的 DTB 进行反编译：

```
dtc -I dtb -O dts \
    -o platform.dts platform.dtb
```

重点检查：

```
/cpus
/memory
/reserved-memory
/chosen
/interrupt-controller
```

再检查是否存在：

```
opensbi-domains
```

以及相关：

```
opensbi,domain,config
opensbi,domain,memregion
opensbi,domain,instance
```

需要理解一个细节：OpenSBI 输入的原始 DTB 与最终传递给 Linux 的 DTB 可能不同，因此不能仅凭 Linux 运行时 DT 中不存在 `opensbi-domains`，就判断系统没有配置 Domain。

### 实验问题

如果两个 Domain 的同一物理 Memory Region 在逻辑上具有不同的权限，那么最终必须由哪个组件来执行权限转换？

参考结论：

由可信 Firmware 协调并由具备相应能力的硬件执行，不能依赖 Device Tree 自己。

## 实验 B：检查 OP-TEE Memory Mapping

重点阅读：

```
core/arch/riscv/mm/core_mmu_arch.c
```

寻找并分析：

```
struct mmu_pte;
struct mmu_pgt;
```

同时理解：

- PTE 的 V/R/W/X/U 标志。
- `satp` 如何引用 Page Table Root。
- ASID 如何管理。
- Core MMU 与 User TA MMU 的映射差异。
- `SFENCE.VMA` 如何确保页表更新生效。

当前源码已包含 RISC-V 页表结构、ASID 管理和地址翻译相关实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



进一步观察：

```
OP-TEE S-mode VA
        |
        | Sv39/Sv48
        v
OP-TEE PA
        |
        | PMP / Isolation Policy
        v
Trusted DDR
```

要能够明确：

一次内存访问能否成功，不能只由页表权限决定。

## 实验 C：检查 RPMI Channel Discovery

使用 GDB 或 Trace 观察：

```
sbi_mpxy_rpmi_probe_channels()
```

建议记录：

```
SBI MPXY available?
Channel count?
Channel IDs?
Protocol ID?
RPMI service group?
Shared memory size?
Returned status?
```

对于系统 Bring-up，应当让每一步失败都能够输出明确错误。

如果 Linux 报告无法建立 TEE Transport，但 OP-TEE 已经初始化成功，那么应优先调查 Linux Driver、SBI MPXY、RPMI Protocol/Channel 以及 Firmware 中的服务暴露状态，而不是立即怀疑 TA。

## 实验 D：安全边界验证

建议首先在隔离的 QEMU 或预硅验证环境中开展。

| 测试                    | 预期           | 验证对象                    |
| --------------------- | ------------ | ----------------------- |
| REE CPU 访问 TEE RAM    | 访问被拒绝        | PMP/Domain              |
| TEE 访问自身 RAM          | 成功           | PMP/MMU                 |
| REE 访问 Secure MMIO    | 访问被拒绝        | MMIO Policy             |
| TA 越界访问 Core Memory   | 被阻止          | User TA MMU             |
| REE 修改 Shared Buffer  | TEE 按不可信输入处理 | Shared Memory           |
| Secondary Hart 运行 TEE | 正常           | Hart/HSM/Per-Hart State |

对于真实 DMA 防护，需要在具有真实 IOMMU/IOPMP/Firewall 行为的 RTL 仿真、仿真加速器或开发板上验证；纯 QEMU 不能自动证明物理 SoC 的 DMA Isolation 正确。

### 实验验收标准

实验结束时，应该能够独立提交以下四项材料：

1. 一份完整的 Platform Memory Map。
2. 一份 REE/TEE Domain 资源所有权矩阵。
3. 一份 Boot + Device Tree Handoff 流程图。
4. 一份带有执行结果的 Positive/Negative Security Test Report。

如果能在这四份材料基础上进一步提交可复现的 upstream Bugfix 或 Test Patch，就已经进入了真正有价值的 OP-TEE Platform Engineering 工作。

# 18.12 Staff Engineer 级面试题与参考分析

以下设计了 12 道针对真实 SoC Firmware/TEE 工程的综合题。

重点不是记忆 API，而是检查是否能够从体系结构、硬件安全、Firmware、Linux 和 OP-TEE 多个维度进行系统级分析。

## Q1. Linux 已配置 `reserved-memory`，为什么仍然能够访问 OP-TEE RAM？

场景

一款 RV64 SoC 预留了 32 MiB OP-TEE Trusted DDR。

Linux DT 中正确配置了：

```
optee@90000000 {
    reg = <0x0 0x90000000
           0x0 0x02000000>;
    no-map;
};
```

但安全团队发现，通过修改 Linux Kernel 代码建立新的内存映射，仍能读到这段内存。

请解释根本原因并设计修复方案。

参考分析

这个问题说明 `reserved-memory` 被错误地当成了硬件隔离机制。

`no-map` 是 Linux 正常内存管理层面的约定，不会阻止被攻破的 Linux 修改页表并尝试访问物理地址。

正确的架构需要使 CPU 的非可信物理访问受到 PMP 或其他硬件保护机制约束，并保证安全配置由可信固件维护。

修复时应当确认：

- OpenSBI Domain 的有效 Region 权限。
- PMP 是否真正拒绝 Linux 的 S-mode Access。
- 是否存在过宽的匹配区域或优先级配置错误。
- 是否存在不受保护的 Alias。
- 防止 Linux 修改安全策略。
- 同时检查 DMA/IOPMP/Firewall 保护。

Staff-level 加分点： 不仅分析 CPU Physical Access，还要检查非 CPU Master 的访问路径，以及启动和 Domain Switch 期间的权限窗口。

## Q2. 16-hart SoC 的 Hart ID 不连续，OP-TEE 在 Secondary Hart 启动时崩溃，如何定位？

场景

SoC 拥有 8 个 Application Harts，Hart ID 分别是：

```
0, 1, 4, 5, 8, 9, 12, 13
```

OP-TEE Primary Hart 启动正常，但是 Hart 8 启动时崩溃，日志中显示 Stack Pointer 异常。

参考分析

需要首先区分：

```
Hart ID
```

与：

```
Logical Core Index
```

在 OP-TEE 中，Per-Hart Stack、Thread Core Local 等结构通常以逻辑索引进行分配。

如果错误地使用 Hart ID 作为连续数组下标，则可能访问越界。

诊断步骤：

1. 检查 DT `/cpus` 的 Hart ID。
2. 检查 `boot_primary_init_core_ids()` 获取的 Hart 列表。
3. 检查 Hart ID 到 Logical Index 的转换。
4. 检查汇编入口设置 `sp`、`tp` 的逻辑。
5. 检查 `CFG_TEE_CORE_NB_CORE` 和实际支持数量。
6. 检查 SBI HSM 的 Hart Assignment 和启动状态。

当前 RISC-V `entry.S` 中确实使用 Scratch Register 保存内部 Hart Index，并据此建立 Stack/Thread Core Local；`boot.c` 也负责 Hart ID 枚举与 Secondary Hart 启动。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



Staff-level 加分点： 考虑 Boot Hart 不是 Hart 0，以及部分 Hart 被其他 Domain 占用的情况。

## Q3. OpenSBI Domain 配置了正确的 PMP Region，但 OP-TEE 切换时仍发生 Instruction Access Fault，可能是什么原因？

参考分析

应从三层进行定位。

第一层：地址和镜像。

确认 `next-addr`、实际 Image Load Address、ELF Entry、ASLR Relocation 是否一致。

第二层：访问权限。

检查当前 Domain 的 PMP 执行权限、Region Matching Priority、Locked Entry、是否存在错误的继承策略。

第三层：执行上下文。

检查：

- 当前 `mstatus.MPP` 或 Domain Entry Privilege。
- `mepc`。
- `satp`。
- TLB 和 Instruction Cache 同步。
- Domain Context 是否完整恢复。

尤其需要区分 Instruction Page Fault 和 Instruction Access Fault。

前者往往涉及页表或页级权限；后者还可能涉及 PMP 或 Physical Access。

如果只有第二次 Domain Switch 失败，而第一次成功，则应重点检查上下文恢复和权限更新顺序。

## Q4. 如果 SoC 的 PMP Entry 只有 8 个，如何设计 Linux/OP-TEE Memory Isolation？

场景

平台拥有 1 GiB DDR，需要保护 OP-TEE Core、TA RAM、OpenSBI RAM 和 Shared Memory。

参考分析

不能简单地按每个软件 Section 创建一个 PMP Entry。

因为 PMP Entry 数量有限，还要考虑：

- Region Alignment。
- NAPOT/TOR 编码。
- Overlap Priority。
- OpenSBI 自身保护。
- Domain 动态切换。
- Hardware Locking。
- 各 Hart 的一致性。

建议首先划分较大的物理 Ownership Region，例如将 OP-TEE Core 与 TA RAM 放在相邻且可以合并的 Trusted DDR 区域内。

再通过 OP-TEE 的 S-mode MMU 对 Core Text/Data/TA Memory 实现更细粒度的权限。

如果需要在 CPU 级别使用更多安全 Region，应该评估 PMP Entry 是否足够，以及是否可以借助其他系统级保护硬件。

Staff-level 加分点： 不只是提出内存布局，还要证明所有访问权限都能够由目标硬件编码，并考虑 Locked PMP 对 Domain Switch 的影响。

## Q5. Secure Boot 验证了 OP-TEE Image，为什么安全团队仍然认为启动链存在风险？

场景

RSS/HSM 对 OP-TEE Image 完成签名验证。

但是 Loader 在校验后，将 Image 写入普通 DDR，再经过其他 Firmware 初始化，最终执行 OP-TEE。

参考分析

镜像签名验证只证明某一时刻的数据完整性和来源，不自动保证后续执行的数据没有变化。

需要分析：

```
Verify Image
     |
     | Potential modification window
     v
Load Image
     |
     | Potential modification window
     v
Execute Image
```

可能存在：

- Untrusted DMA 修改 Image。
- 其他 Hart 修改内存。
- 不可信 Firmware 修改加载内容。
- Image Entry 被替换。
- 安全隔离在启动过程中安装过晚。

可采用受保护的 Staging/Load Memory、可信 Loader、及时安装安全权限、重新验证或其他符合平台信任模型的方式消除窗口。

Staff-level 加分点： 将 Boot Authentication、Runtime Memory Isolation、Anti-Rollback 和 Attestation 视为不同但相关的安全机制。

## Q6. Linux 在 UEFI 启动后使用 ACPI，而 OpenSBI 使用 DT，如何维护一致的硬件资源描述？

参考分析

这是实际服务器平台经常出现的系统集成问题。

OpenSBI 可能通过 Firmware DT 取得 Hart、内存和 Domain 配置。

UEFI 则可能向 Linux 提供 ACPI Tables。

因此必须建立统一的 Platform Resource Source of Truth，而不是允许两个独立团队手工维护互不关联的资源表。

需要至少保证：

- CPU/Hart Topology 一致。
- RAM/Reserved Memory 一致。
- Interrupt Controller 配置一致。
- Security Ownership 一致。
- 可信内存不能作为 Linux 普通可用内存暴露。
- Firmware 管理的设备不被 Linux 重复初始化或错误接管。

Staff-level 加分点： 建议通过机器可读的配置生成流程，自动生成 Firmware DT、UEFI/ACPI 配置及安全验证用资源表，并在 CI 中执行一致性检查。

## Q7. OP-TEE 单 Hart 完全正常，但 16-hart 并发运行 `xtest` 时偶发 Deadlock，如何系统性定位？

参考分析

不能简单将问题归结为 Spinlock。

应按以下维度建立 Trace：

| 维度        | 重点                                   |
| --------- | ------------------------------------ |
| Hart      | 当前 Hart ID、Domain、运行状态               |
| Thread    | Trusted Thread ID、状态                 |
| RPC       | 是否有 Outstanding RPC                  |
| Lock      | Lock Owner、等待路径                      |
| Interrupt | 是否被 Mask、Pending、路由异常                |
| MMU       | 是否等待 Remote TLB Shootdown            |
| SBI       | HSM、IPI、RFENCE 是否完成                  |
| Transport | MPXY Channel Busy / Response Pending |

尤其要检查：

- 是否存在某 Hart 持有锁后执行 RPC/Yield。
- Domain Switch 时是否禁用了必须用于 Forward Progress 的中断。
- Remote RFENCE 是否依赖另一个无法进入处理状态的 Hart。
- Per-Hart Transport 是否使用错误的 Channel。
- Thread Pool 是否耗尽。

Staff-level 加分点： 构造一张包含 Lock、Hart、Domain、RPC、IPI 的等待依赖图，寻找循环等待，而不是靠反复加日志碰运气。

## Q8. Linux TEE Driver 无法 Probe，但 OP-TEE Core 完全正常，如何判断是 DT、MPXY 还是 RPMI 的问题？

参考分析

应该分层验证。

第一步，确认 Linux Driver 是否匹配当前 DT Compatible。

第二步，确认 Device Tree 中 Mailbox Provider 和 Channel 描述合法。

第三步，确认 SBI MPXY Extension 是否存在。

第四步，枚举 Channel，检查 Message Protocol ID 和 RPMI Service Group。

第五步，执行最小协议发现请求，确认 Firmware 能够返回合法的 Endpoint/Service 信息。

第六步，才进入 Shared Memory、TEE Session 和 TA 调用调试。

这里必须特别注意 2026 年不同版本的 RPMI TEE RFC 使用过不同的 DT Binding 设计，因此混用版本可能造成设备探测失败。

[image](https://www.google.com/s2/favicons?domain=https://marc.info\&sz=32)

MARC+1



Staff-level 加分点： 将 Firmware/Driver/Binding 版本兼容性检查纳入构建系统，而不是在运行失败后再人工排查。

## Q9. 为什么 Linux 无法直接访问 Trusted DDR，PCIe DMA 却能够读出其中的数据？

参考分析

这几乎是最典型的 SoC TEE Security Architecture 问题之一。

CPU PMP 限制的是 CPU 对物理内存的访问，并不能自动约束独立 DMA Master。

需要检查 PCIe Endpoint 发起 DMA 后，其 Request 是否经过：

- IOMMU。
- IOPMP。
- SoC Firewall。
- Interconnect Access Control。

同时还要检查：

- IOMMU Page Table 是否由不可信 Linux 完全控制。
- 是否有设备绕过 IOMMU 的路径。
- PCIe Device ID/Stream ID 是否被正确识别。
- Firewall 是否存在默认允许策略。
- 设备 Reset 或 Suspend 后权限是否保持。
- DMA Translation Cache Invalidation 是否正确。

修复目标应该是：即使 Linux 配置任意 DMA Descriptor，未授权设备也无法访问 Trusted DDR。

## Q10. OP-TEE 使用一个 Secure Crypto Engine，但 Linux 仍能通过 MMIO 修改其控制寄存器，如何设计资源所有权？

参考分析

需要从硬件和软件两层处理。

硬件侧应提供可信的 MMIO Access Control，并将 Secure Crypto Engine 的控制权限授予 OP-TEE Domain 或可信管理实体。

软件侧需要：

- OP-TEE Driver。
- 正确的 MMIO Mapping。
- 可信的 Clock/Reset 管理。
- Interrupt Ownership。
- DMA Buffer Protection。
- Key Slot Ownership。

还需要研究 Crypto Engine 是否支持多个独立 Security Context。

如果硬件无法将同一 Engine 的不同 Key Slot 或 Operation Context 隔离，那么不应该在 REE 和 TEE 之间不受控地共享它。

Staff-level 加分点： 除寄存器访问控制，还要分析 DMA、Interrupt、Reset、Side Channel、Fault Injection 和 Key Zeroization。

## Q11. Linux 与 OP-TEE 使用 Shared Memory 通信，如何防御恶意 Linux 的 TOCTOU 攻击？

场景

Linux 向 TA 传递一个包含操作类型、数据长度和缓冲区地址的共享结构。

OP-TEE 验证长度合法后，稍后再次读取这个长度进行内存拷贝。

参考分析

攻击者可能在两次读取之间修改 Shared Memory。

例如：

```
First read: length = 128
        |
        | Validation passes
        |
Linux modifies length = 65535
        |
Second read: length = 65535
        |
        v
Potential out-of-bounds access
```

因此要建立明确的 Untrusted Input Boundary。

对长度、指针、偏移等安全敏感元数据，应按需要复制到可信内存后进行一次性校验，并在后续使用同一份可信快照。

同时检查：

- Integer Overflow。
- Buffer Bounds。
- Shared Memory Registration。
- Memory Ownership。
- Page Pinning/映射生命周期。
- Cache Coherency。
- 错误路径资源释放。

Staff-level 加分点： 不只考虑 CPU 并发修改，还应考虑 DMA 修改 Shared Memory，以及共享页生命周期和物理页重新分配造成的问题。

## Q12. 你负责一款新 RISC-V 服务器 SoC 的 OP-TEE Porting，如何制定两阶段交付计划？

场景

SoC RTL 尚未冻结，只有 QEMU 和 Palladium/ZeBu 预硅平台。

计划三个月后流片，安全团队要求证明 Linux 与 TEE 隔离有效。

请制定工程计划和成功判据。

参考分析

我建议分成两个阶段。

### 阶段 A：Pre-Silicon Functional + Security Validation

交付：

- 固定的 Boot/Firmware Version Matrix。
- Platform Memory Map。
- OpenSBI Domain Policy。
- OP-TEE Platform Port。
- Linux TEE Transport Integration。
- 基础 CA/TA 测试。
- 单 Hart 与多 Hart 测试。
- MMIO/PMP/Firewall Negative Test。
- DMA Isolation Test（以验证平台具备的硬件模型为准）。

其中必须区分 QEMU 与 RTL 仿真各自能够验证的内容。

例如，真实 DDR Firewall 的正确性不能只靠 QEMU 的软件仿真通过来证明。

### 阶段 B：Post-Silicon Bring-up + Security Qualification

交付：

- 实际 Boot ROM/RSS/OP-TEE 启动链验证。
- DDR/Cache/Interrupt/Timer/Hart 行为验证。
- PMP、IOMMU/IOPMP/Firewall 实测。
- Secure Boot、Anti-Rollback 和密钥保护验证。
- SMP Stress Test。
- Power/Reset/Suspend/Resume 测试。
- 异常和故障注入。
- Release Build 安全配置审查。

Staff-level 加分点： 在 RTL Freeze 前识别软件无法弥补的硬件安全设计缺陷，例如未受保护的 DMA 路径、可由 Linux 修改的 Firewall 配置寄存器，以及缺乏足够 Domain 权限标识的片上互连。

这类风险若直到流片后才发现，可能无法通过 Firmware Patch 修复。

# 18.13 本章知识归纳

本章最值得掌握的，不是某个 `conf.mk` 选项或具体的 DT 属性，而是下面五条系统级原则。

原则一：Device Tree 描述资源，但不自动建立安全边界。

真正的安全边界必须由可信固件和硬件访问控制执行。

原则二：OP-TEE Porting 不等于 OP-TEE Core 能启动。

必须集成 Boot Firmware、OpenSBI Domain、Linux、TEE Transport 和 Hardware Isolation。

原则三：CPU Memory Protection 不等于 SoC Memory Protection。

必须同时考虑 CPU、DMA、PCIe、HSM、其他 Processor 和外设访问路径。

原则四：Firmware Handoff 必须具备完整的信任链。

Authenticated Image、Trusted DT、Memory Policy、Entry Address 和硬件保护状态必须保持一致。

原则五：完整的 Porting 必须包含 Negative Security Validation。

能够正常访问授权资源，以及无法访问未授权资源，必须同时得到验证。

## 最终的工程判断标准

对于一个新的 RISC-V OP-TEE Platform，我建议把完成状态划分为：

| 等级                           | 达成条件                            |
| ---------------------------- | ------------------------------- |
| Level 1 — Bootable           | OP-TEE Core 成功启动                |
| Level 2 — Functional         | Linux 能成功调用 TA                  |
| Level 3 — SMP Ready          | 多 Hart、Interrupt、RFENCE 和并发运行通过 |
| Level 4 — Security Validated | CPU、DMA、MMIO、Firmware 隔离得到验证    |
| Level 5 — Production Ready   | 具备完整测试、故障恢复、安全配置与可维护交付流程        |

Level 2 不能自动视为 Level 4，Level 4 也不能自动等同于产品已满足全部认证和运行可靠性要求。

## 本章结束后的建议

Chapter 18 的所有十个大纲要点已经覆盖，从 QEMU virt 的差异一直到完整的 SoC Platform Porting。

对于你目前关注的 RISC-V OP-TEE + OpenSBI + 独立 RSS/HSM 系统，我认为最有价值的下一项实操，是把 Chapter 18 沉淀成一个 最小 RISC-V Secure SoC Reference Platform。

不需要一开始就有真实硬件。可以先在 WSL2 中完成 QEMU 版本，把内存布局、Domain Policy、Linux DT、OP-TEE Platform Config 和 Trace 全部关联起来，再将硬件安全验证部分迁移到预硅仿真环境。

如果后续准备把这一方向作为 OP-TEE upstream 的长期贡献领域，我尤其建议优先研究并改进 Platform DT Sanitization、Domain/Memory Policy Validation，以及跨固件配置一致性测试。

因为这些问题处在 OpenSBI、OP-TEE 和 Linux 的交界处，既需要体系结构知识，又需要实际 Kernel/Firmware 开发能力，能够真正体现 Staff Engineer 在系统设计、可靠性和安全性方面的深度。
