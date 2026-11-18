# PART VI — 高级架构设计与最终实战

# Chapter 20：从零设计一套基于 SMMPT 的 RISC-V TEE 平台

RISC-V SMMPT Architecture & Engineering · Final Chapter

这一章是整个 SMMPT 系统课程的最终章节。

前面几个 Part，我们分别研究了 SMMPT 的设计动机、MPT 数据结构、CSR、Domain Switching、共享内存、DMA、Interrupt、Firmware 集成以及调试验证。

现在我们要完成一项更接近真实芯片研发工作的任务：

假设我们是 RISC-V Server SoC 的 Security Architect，需要从零设计一套支持 Linux + OP-TEE 的 Supervisor Domain 隔离系统。我们应该如何做？

与前面章节不同，本章不再以一个寄存器、一条指令或一个软件模块为中心，而是以完整系统设计为中心。

我们需要回答的不只是“如何实现”，还包括：

- 为什么选择这样的架构？
- 哪些模块必须可信，哪些模块可以不可信？
- 哪些安全属性由硬件保证，哪些依靠 Firmware？
- 在发生异常、并发竞争或部分硬件失效时，安全不变量是否仍然成立？
- 如何证明设计符合要求，而不是仅仅能够正常启动？

> 本章核心目标：建立从 Security Requirements → Architecture → Implementation → Verification 的完整思维方式，最终形成一份可以用于 SoC 架构评审的 SMMPT + OP-TEE 集成设计。

## 20.0 规范依据与当前实现状态

这里首先明确我们讨论的技术基线。

截至 2026 年 11 月，RISC-V Supervisor Domain Access Protection 规范已将原先分别讨论的部分机制整合到 `Smsd`（Supervisor Domains）扩展中。其中，MPT 负责细粒度物理内存访问控制，`mmpt` CSR 则包含当前 Hart 的 Domain 标识和 MPT Root 配置。

`Smsdia` 用于 Supervisor Domain Interrupt Assignment，I/O MPT Checker 则负责将相应保护扩展至 DMA 路径。

目前应以 v0.9 系列公开规范作为本章的主要参考，但不能将其视为已经完成全部产业化验证的稳定 ABI。2026 年 10 月的工作组记录仍涉及 Bare Mode 与 I/O MPT 等实现问题。

[image](https://www.google.com/s2/favicons?domain=https://riscv.atlassian.net\&sz=32)

RISC-V Tech Hub+2



软件方面有一个很值得关注的进展：2026 年 10 月 31 日，OpenSBI 社区出现了 `[RFC PATCH v6 0/6] Add Smsd supervisor domain protection` 系列，提供了与 SBI Domain 结合的 MPT 管理和 Hart 配置代码，但作者明确指出，v0.9 对应的 QEMU 适配和多 Domain 测试仍需继续完善。因此，本章会引用这些真实实现，同时把尚未完成的 OP-TEE 集成描述为参考架构设计，而不是声称已有完整上游产品。

[image](https://www.google.com/s2/favicons?domain=https://lore-kernel.gnuweeb.org\&sz=32)

lore-kernel.gnuweeb.org



本章中采用以下标记：

| 类型   | 含义                             |
| ---- | ------------------------------ |
| 规范机制 | 架构已经定义的行为                      |
| 参考设计 | 为假设 SoC 选择的一种实现方案              |
| 伪代码  | 用于展示逻辑和接口，不代表现有标准 ABI          |
| 验证要求 | 需要在具体 RTL、Firmware 和芯片环境中证明的属性 |

## 20.1 系统需求分析：我们要设计怎样的 SoC？

### 20.1.1 假设的硬件平台

先定义系统，而不是直接开始设计 MPT。

| 组件                | 假设配置                               |
| ----------------- | ---------------------------------- |
| CPU               | 16 × RV64 Hart                     |
| Privilege         | M/S/U，支持 Sv48                      |
| Supervisor Domain | 支持 Smsd / SMMPT                    |
| Memory            | 16 GiB DDR                         |
| Memory Protection | PMP/Smepmp + SMMPT                 |
| Interrupt         | AIA / IMSIC / APLIC，目标支持 Smsdia    |
| DMA               | IOMMU + I/O MPT Checker            |
| Boot Security     | 独立 RSS/HSM Root of Trust           |
| M-mode Firmware   | OpenSBI + RDSM                     |
| Normal World      | UEFI → Linux                       |
| Trusted World     | OP-TEE OS                          |
| Validation        | QEMU、RTL 仿真、Palladium/ZeBu、Silicon |

这是一个参考平台配置，不表示上述组件只要存在就必然能够共同工作。例如，IOMMU 支持并不等于已经具备完整的 I/O MPT Checker。

### 20.1.2 目标系统架构

参考架构图：展示信任管理层、两个 S-mode Domain 及硬件保护层。并非严格的总线连接图；实际 CPU、IOMMU、I/O MPT Checker 的位置取决于 SoC 设计。

这里最重要的设计决策是：

Linux 和 OP-TEE 都运行在 S-mode，但 Linux 不是 OP-TEE 的特权上级。

它们之间的保护不是利用传统的 S-mode/U-mode 权限差异建立的，而是由 M-mode 配置的 Supervisor Domain 物理资源隔离机制建立的。

这和经典 Arm TrustZone 中 Linux 处于 Non-secure EL1、OP-TEE 处于 Secure EL1 的安排在目标上类似，但实现机制并不相同。不能简单地把 RISC-V 的 S-mode Domain 直接当作 Arm Secure State。

### 20.1.3 功能需求与安全需求分离

系统需要同时满足两种需求。

功能需求：

- Linux 能正常启动并管理 16 个 Hart。
- OP-TEE 能初始化、运行 Trusted Application。
- Linux Client Application 能通过驱动调用 Trusted Application。
- Secure Storage 和安全设备可以由 Trusted Domain 使用。
- Linux、OP-TEE 可以协作完成共享内存通信。
- 系统支持正常 Suspend、Resume、Reset、CPU Hotplug。

安全需求：

- Linux 即使完全被攻陷，也无法读写 OP-TEE Private Memory。
- Linux 不能修改 RDSM 的代码、MPT 或安全配置。
- Linux 不能通过 DMA 间接访问 OP-TEE 内存。
- Linux 不能修改或伪造其他 Domain 的权限。
- Domain Switching 不能泄漏前一个 Domain 的敏感 CPU 状态。
- 内存撤销后，旧 Domain 不得继续访问相应资源。
- 安全状态不能由于 Warm Boot、异常重入或局部复位而被意外清除。

功能需求关心“能不能运行”，安全需求关心“什么条件下仍然必须无法越权”。

这两类需求必须分开验证。

## 20.2 Security Threat Model：安全架构设计的起点

### 20.2.1 先明确攻击者是谁

本章采用的攻击者模型：

攻击者已获得 Linux Kernel 的最高控制权。

也就是说，攻击者可以：

- 修改 Linux 页表和内核代码。
- 控制 Linux 的内存分配器。
- 执行任意 S-mode 可执行指令。
- 调用 Linux 可访问的 SBI 接口。
- 控制普通 DMA 设备的描述符。
- 发送恶意或畸形的 OP-TEE 请求。
- 尝试修改 Linux 可访问的中断控制器寄存器。
- 同时使用多个 Hart 发起并发攻击。

但我们暂不假设攻击者能够直接修改芯片 RTL、破解硬件 Root of Trust 或在生产策略下获得 M-mode Debug 权限。

因此我们的安全目标很明确：

> 即使 Normal World 完全失陷，也必须依靠低于它的硬件隔离机制和可信 Firmware 来保护 Trusted World。

### 20.2.2 系统的 Trusted Computing Base

不是所有安全相关模块都必须放入 TCB。

| 组件                  | TCB 归属                  | 原因                       |
| ------------------- | ----------------------- | ------------------------ |
| RSS / Boot ROM      | Root TCB                | 建立可信启动根                  |
| RDSM                | Root TCB                | 控制 Domain 与物理权限          |
| CPU MPT Enforcement | Hardware TCB            | 执行 CPU 物理访问控制            |
| I/O MPT、IOMMU、相关互连  | Hardware TCB            | 执行 DMA 和设备隔离             |
| OP-TEE OS           | Trusted Domain TCB      | 管理 TA、Secrets 和服务        |
| Linux Kernel        | 不纳入 Trusted Domain TCB  | 按照可被攻陷设计                 |
| UEFI                | Boot-chain 可信组件，范围依部署而定 | 负责启动链；不应默认拥有运行期全系统权限     |
| 普通 Device Driver    | 不纳入 Trusted Domain TCB  | 不能依赖其执行 Secure Memory 隔离 |

有一个非常值得深入理解的设计理念：

Reduce TCB，并不是简单地减少代码量，而是减少哪些代码一旦出现漏洞就能够破坏整个系统的安全性。

例如：

如果 Linux 能够直接修改任意 MPTE，那么 Linux 实际上已经进入安全隔离机制的 TCB。即使你把 OP-TEE 移植得再好，安全架构也没有意义。

所以 MPTE 管理权应由可信 M-mode Firmware 持有，Linux 只能提交受限的资源请求。

### 20.2.3 信任边界在哪里？

本平台至少具有五类边界。

| 边界                          | 谁保护它                   |
| --------------------------- | ---------------------- |
| Linux ↔ OP-TEE              | SMMPT + RDSM           |
| S-mode ↔ Root Firmware      | PMP/Smepmp + MPT 策略    |
| CPU ↔ DMA                   | IOMMU + I/O MPT        |
| Domain A ↔ Domain B CPU 状态  | RDSM Context Switching |
| Firmware ↔ Secure Boot Root | RSS/HSM + 签名验证与测量      |

这些边界各自独立。

例如，SMMPT 可以阻止 Linux 的 CPU Load 指令读取 Trusted Memory，但并不能因此证明一个恶意 DMA 设备也无法读取该内存。

同样，SMMPT 只负责访问权限，不等于提供内存加密、缓存侧信道隔离或远程证明。规范明确将内存加密等能力视为独立于该访问控制机制的技术。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



### 20.2.4 定义必须始终成立的安全不变量

我建议在架构设计文档中明确以下八条不变量。

| ID     | Security Invariant                           |
| ------ | -------------------------------------------- |
| INV-01 | 非 Root Domain 不能修改 RDSM、MPT 和安全元数据           |
| INV-02 | Linux Domain 永远不能读取或修改 OP-TEE Private Memory |
| INV-03 | 任意 Private Page 的访问权限必须符合 Ownership Policy   |
| INV-04 | DMA 的最终访问权限不能超过对应 Domain 的授权                 |
| INV-05 | Domain Switch 后不得残留可被新 Domain 利用的旧执行状态       |
| INV-06 | 内存撤销完成后，旧授权在所有相关 Hart 和 I/O 路径上失效            |
| INV-07 | 授权失败、配置异常或安全状态不明时，系统必须 Fail Closed           |
| INV-08 | 不可信软件无法通过伪造资源请求获得其他 Domain 的权限               |

后面每一个设计步骤，都应当明确它维护了哪几条不变量。

例如，讨论 `MFENCE.PA` 时，不能只问“为什么要刷新缓存”，还应该问：

如果没有执行正确的同步操作，会违反哪条 Security Invariant？

答案很可能是 INV-06。

## 20.3 Supervisor Domain 划分与资源模型

### 20.3.1 我们需要几个 Domain？

本章的初始设计采用两个 S-mode Domain 加一个 M-mode Root Security Manager。

| 域           | 示例逻辑 ID    | 执行权限     | 职责          |
| ----------- | ---------- | -------- | ----------- |
| Root / RDSM | Root 管理上下文 | M-mode   | 可信资源管理      |
| Domain A    | 1          | S/U-mode | Linux、UEFI  |
| Domain B    | 2          | S/U-mode | OP-TEE 与 TA |

这里逻辑 Domain ID 和 `mmpt.SDID` 需要区别。

`SDID` 是 Hart-local 的标识，用来标记 Hart 内部与 Domain 相关的状态；规范明确要求不能直接将这一标识当作在整个 SoC 上全局唯一的下游事务标签。

例如，一次来自 PCIe DMA 的事务需要经过 I/O Domain Classifier 的识别，而不能假设把某个 Hart 的 `SDID=2` 直接发送到总线就足够了。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



因此参考设计中会维护两类标识：

```
typedef uint32_t domain_handle_t; /* RDSM 全局逻辑标识 */

/* 仅作为设计示例 */
struct domain_mapping {
    domain_handle_t handle;
    unsigned int hart_local_sdid[16];
    uintptr_t mpt_root_pa;
};
```

这样的抽象使软件的 Domain 生命周期不依赖特定 Hart 的 SDID 编码方式。

### 20.3.2 Domain 和 Hart 是什么关系？

这是一个典型的架构设计问题。

假设有 16 个 Hart，并不意味着需要创建 16 个 Domain，也不意味着 Linux 与 OP-TEE 必须各占用固定数量的 Hart。

可以考虑三种方案：

| 方案             | 原理                         | 优点       | 缺点                |
| -------------- | -------------------------- | -------- | ----------------- |
| Dedicated Hart | OP-TEE 独占指定 Hart           | 状态切换简单   | CPU 利用率可能较低       |
| Shared Hart    | Linux/OP-TEE 在同一 Hart 分时运行 | 资源利用率高   | Context Switch 复杂 |
| Hybrid         | 关键服务使用固定 Hart，其他服务动态调度     | 能平衡隔离与性能 | 调度及资源管理复杂         |

我建议课程的最终设计采用 Shared Hart 模型，因为它最能体现 SMMPT 和 RDSM 的架构价值。

但这只是我们的目标设计。前期 Bring-up 应优先采用 Dedicated Hart，先证明静态隔离正确，再引入动态 Domain Switching。

### 20.3.3 Domain 内部资源如何描述？

我建议 RDSM 至少维护以下结构。

```
/* 参考设计：非现有 OpenSBI 结构定义 */

struct secure_domain {
    uint32_t domain_handle;

    /* MPT 配置 */
    uintptr_t mpt_root_pa;
    uint32_t mpt_mode;

    /* 每个 Hart 的当前或预分配 SDID */
    uint32_t sdid[16];

    /* CPU 与设备资源 */
    uint64_t allowed_harts;
    uint64_t assigned_devices;

    /* 内存所有权 */
    struct memory_region *regions;
    size_t nr_regions;

    /* Interrupt */
    uint32_t interrupt_domain_id;

    /* Lifecycle */
    enum domain_state state;
};
```

其中最值得强调的是：Domain 不是一张 MPT。

一张 MPT 只定义内存访问权限，而 Domain 在系统架构中还关联了 CPU 执行上下文、设备、中断、内存所有权及安全策略。

因此：

```
Supervisor Domain
    |
    +-- Memory Access Policy
    |      +-- MPT
    |
    +-- CPU Context
    |      +-- Hart state
    |      +-- CSR state
    |
    +-- Device Ownership
    |      +-- IOMMU / I/O MPT
    |
    +-- Interrupt Routing
    |      +-- AIA / IMSIC
    |
    +-- Domain Lifecycle
           +-- Create
           +-- Run
           +-- Suspend
           +-- Destroy
```

如果将它设计成一个单纯的 `mmpt.PPN` 切换模块，后面大概率会在 SMP、DMA 和 Interrupt 设计中遇到无法解决的耦合问题。

## 20.4 物理内存布局与 MPT 权限设计

现在进入实际工程实现。

### 20.4.1 定义物理内存布局

假设 DDR：

```
DDR Base : 0x0000_0000_8000_0000
DDR Size : 16 GiB
DDR End  : 0x0000_0004_8000_0000
```

采用以下参考划分：

| 区域             | 起始地址          | 大小      | 用途                         |
| -------------- | ------------- | ------- | -------------------------- |
| Root Firmware  | `0x8000_0000` | 32 MiB  | RDSM、MPT、Root Metadata     |
| OP-TEE Private | `0x8200_0000` | 512 MiB | OP-TEE Core、TA、Secure Heap |
| Shared Memory  | `0xA200_0000` | 64 MiB  | Linux ↔ OP-TEE             |
| Linux Memory   | `0xA600_0000` | 剩余 DDR  | Linux Kernel、Userspace     |

16 GiB DDR 物理地址空间示意（不按比例）

RDSM / MPT

32 MiB

0x8000_0000 – 0x8200_0000

OP-TEE Private

512 MiB

0x8200_0000 – 0xA200_0000

Shared Memory

64 MiB

0xA200_0000 – 0xA600_0000

Linux DDR

剩余约 15.41 GiB

0xA600_0000 – 0x4_8000_0000

示例中的区域以半开区间 [start, end) 表示。实际系统还需要另外配置 MMIO、SRAM、Secure Peripheral 等地址范围。

这里划分了 64 MiB 固定 Shared Memory，但这并不意味着以后只能采用静态共享。

初始设计使用固定共享区域便于验证；系统稳定之后，再增加动态 Memory Ownership Transfer。

### 20.4.2 MPT 的权限矩阵

这是最重要的设计产物之一。

下表描述每个 S-mode Domain 的 MPT 所允许的访问，M-mode RDSM 不通过这两张表获得权限。

| Physical Region         | Linux Domain | OP-TEE Domain |
| ----------------------- | ------------ | ------------- |
| RDSM / MPT              | ---          | ---           |
| Linux Text              | R-X          | ---           |
| Linux Data              | RW-          | ---           |
| OP-TEE Text             | ---          | R-X           |
| OP-TEE Data             | ---          | RW-           |
| Shared Memory           | RW-          | RW-           |
| Linux UART MMIO         | RW-          | ---           |
| Trusted Peripheral MMIO | ---          | RW-           |

这体现了一个基础原则：

不同 Domain 使用不同的 MPT Root，对同一个物理地址可能得到完全不同的访问权限。

例如，假设 OP-TEE Private Memory 中有一段：

```
Physical Address: 0x8300_0000
```

Linux Domain 的 MPT：

```
0x8300_0000 -> NO ACCESS
```

OP-TEE Domain 的 MPT：

```
0x8300_0000 -> RW-
```

即使 Linux 将该地址映射到自己的 `satp` 页表，最终的物理访问仍然无法绕过 MPT 检查。

但请注意：一块内存可执行，并不表示其中每个字节都应该可执行。生产设计需要把 OP-TEE Text、RO Data、Data、Heap、Page Table、TA Memory 进一步区分，尽可能遵循 W^X。

### 20.4.3 使用 Smmpt43 还是 Smmpt52？

我们的物理地址空间位于 43-bit PA 范围内，因此参考实现选择 `Smmpt43`，前提是 SoC 所有需要受保护的 Supervisor Physical Address，包括相关 MMIO，都落入该模式支持的范围，且硬件实际实现了该模式。

它的特点是：

- RV64 使用三层 MPT。
- MPTE 大小为 8 字节。
- 每个最低层非 NAPOT Leaf MPTE 包含四组独立的 4 KiB 页面权限。
- 高层 Leaf 能覆盖更大的物理地址范围。
- MPT 的层级和规模不同于普通 Sv39/Sv48 页表。

规范中的 Smmpt43 使用 43-bit 地址空间，并通过三层表及扩展权限编码降低保护表的内存开销。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么不直接选择 Smmpt52？

因为更大的物理地址空间通常意味着更多层的 Table Walk、更大的映射结构以及更复杂的硬件实现。

当然，如果 SoC 的真实地址映射超出 Smmpt43 范围，就必须根据硬件支持选择更合适的模式，不能为了性能忽略架构要求。

### 20.4.4 如何管理 MPT 的内存？

这里容易出现一个循环依赖：

RDSM 需要 MPT 保护 OP-TEE，但 MPT 本身又存放在物理内存里。

如果 Linux 能修改 MPT，那么保护立即失效。

因此，我们把 MPT Table Pool 放在 Root-owned Memory：

```
Root Firmware Region
    |
    +-- RDSM Text / RO Data
    +-- RDSM Data
    +-- MPT Table Pool
    +-- Domain Metadata
    +-- Per-Hart Root Context
    +-- Root Stack
```

从安全策略看：

```
Linux        -> MPT Pool: Deny
OP-TEE       -> MPT Pool: Deny
RDSM M-mode  -> MPT Pool: Authorized
```

这里还必须考虑一个容易忽略的问题：MPT Table Walk 是由硬件隐式完成的访问。

规范明确要求，MPT Walk 本身的访问要通过相应的 PMP/PMA 检查。因此不能简单地把 MPT Pool 对所有访问一律关闭，否则硬件也可能无法正常读取 MPTE。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



正确设计是：阻止 S-mode 直接访问，而允许所需的 M-mode 隐式 Table Walk 访问。

### 20.4.5 初始化 MPT 的算法

一个简化的初始化流程如下：

```
/* 参考伪代码：所有函数均为设计接口 */

int init_domain_mpt(struct secure_domain *dom)
{
    int rc;

    dom->mpt_root_pa = root_alloc_mpt_pages();

    if (!dom->mpt_root_pa)
        return -ENOMEM;

    /*
     * 从默认拒绝开始。
     * 必须确保所有未显式授权的区域不能访问。
     */
    rc = mpt_init_deny_all(dom);
    if (rc)
        return rc;

    /*
     * 根据已经验证的资源所有权策略
     * 建立相应的 MPT 映射。
     */
    for_each_authorized_region(dom, region) {
        rc = mpt_map_region(dom,
                            region->pa,
                            region->size,
                            region->perms);
        if (rc)
            return rc;
    }

    rc = mpt_validate_policy(dom);
    if (rc)
        return rc;

    return mpt_publish_and_sync(dom);
}
```

这里刻意采用 Default Deny。

这和某些便于 Boot 的 Firmware 配置策略可能不同，但对于要把 Linux 视为不可信对象的安全设计，我更倾向于：

除非已经明确授权，否则不能因为某个地址没有出现在 Region List 中就默认开放。

实现细节还必须处理多级表内存分配失败、部分修改回滚、Leaf Split/Merge、MPTE 保留编码、页对齐和同步等问题。否则这段伪代码只能实现功能，不能满足生产级安全要求。

## 20.5 Firmware Boot Flow：如何建立第一个可信 Domain？

### 20.5.1 不能把 Boot 成功等同于安全初始化成功

在一个普通 RISC-V Linux 系统中，我们可能主要关注：

```
ROM → OpenSBI → U-Boot/UEFI → Linux
```

但是在支持 SMMPT 的系统中，Boot Flow 还需要完成另外一项工作：

在不可信 Linux 获得执行权限之前，建立完整的安全访问边界。

因此我们将启动过程分成六个阶段。

1. Stage 0 — Hardware Reset / RSS

   初始化 Root of Trust，验证后续 Firmware、初始化密钥和安全生命周期状态。
2. Stage 1 — Early M-mode

   初始化 Root Stack、PMP/Smepmp、可信内存布局以及早期异常处理。
3. Stage 2 — RDSM / MPT Initialization

   探测 Smsd 能力，分配 MPT，建立默认拒绝的 Domain 权限，初始化资源管理元数据。
4. Stage 3 — Trusted Domain Initialization

   创建 OP-TEE Domain，配置 Private Memory、Secure Device 和初始执行上下文，启动 OP-TEE。
5. Stage 4 — Normal Domain Initialization

   创建 Linux Domain，建立共享内存与普通外设权限，启动 UEFI，再进入 Linux。
6. Stage 5 — Multi-Hart Bring-up

   逐 Hart 初始化 Domain Context、MPT 状态和 Interrupt Routing，完成 SMP 启动与运行期策略锁定。

这里的启动顺序是本章的一种参考方案，不是规范规定的唯一顺序。

### 20.5.2 RSS 与 RDSM 如何建立信任关系？

假设 RSS 是独立的 HSM 核心，作为平台 Root of Trust。

那么启动时必须有一条可信关系：

```
Hardware RoT
    |
    +-- Verify RDSM Image
    |
    +-- Verify OP-TEE Image
    |
    +-- Verify UEFI Image
    |
    +-- Establish Measurements
    |
    v
Release Application Harts
```

RSS 可以完成签名验证与测量，但这些操作是否覆盖 MPT Policy、OP-TEE 配置和 Device Assignment，需要由具体的启动及证明协议定义。

例如，仅验证 OP-TEE Image，并不能证明运行期 Linux 无法修改 OP-TEE Memory。

这还必须依靠 RDSM 和硬件访问控制。

Secure Boot 解决的是可信软件从哪里来；SMMPT 解决的是可信软件运行时如何避免被其他 Domain 破坏。

两者互相补充，不能替代。

### 20.5.3 OpenSBI 初始化 SMMPT 的实际代码切入点

2026 年 10 月 31 日的 OpenSBI Smsd RFC 提供了一个很好的真实代码参考。

它增加了以下主要源码：

```
OpenSBI
├── include/sbi/
│   └── sbi_hart_mpt.h
│
├── lib/sbi/
│   ├── sbi_mpt.c
│   ├── sbi_hart_mpt.c
│   ├── sbi_domain.c
│   ├── sbi_domain_state.c
│   └── sbi_hart_protection.c
│
└── platform/generic/
    └── platform.c
```

RFC 中最重要的三个变化是：

1. 在 Cold Boot 路径初始化 SMMPT。
2. 根据 SBI Domain 的 Memory Region 创建对应的 MPT。
3. 在 Hart Protection 配置过程中，将对应的 MPT Root 和 SDID 写入 `mmpt`。

其中真实出现的函数包括：

```
sbi_mpt_init();

sbi_mpt_hart_activate(sdid);

sbi_mpt_hart_activate_for_domain(sbi_dom);

sbi_mpt_query_access(sdid, pa, &xwr);
```

这些是 RFC 中的实现接口，而不是已经标准化的跨 Domain SBI ABI。

[image](https://www.google.com/s2/favicons?domain=https://lore-kernel.gnuweeb.org\&sz=32)

lore-kernel.gnuweeb.org



特别值得关注的是 `sbi_mpt_hart_activate()` 的逻辑。RFC 的关键路径可概括为：

```
/* 以下为真实 RFC 代码逻辑的缩写 */

dom = sbi_mpt_domain_get(sdid);

ppn = dom->root_pa >> SBI_MPT_PAGE_SHIFT;

csr_write(SBI_CSR_MMPT,
          dom->mode->encode_mmpt(ppn, sdid));

sbi_mpt_fence_sdid(sdid);
```

这里完成了两个关键动作：

- 为当前 Hart 选择目标 Supervisor Domain 的 MPT 和 SDID。
- 对相关 MPT 状态进行同步。

实际函数还检查控制器状态、Domain 有效性，并维护 Per-Hart Scratch 数据。

[image](https://www.google.com/s2/favicons?domain=https://lore-kernel.gnuweeb.org\&sz=32)

lore-kernel.gnuweeb.org



注意，完整的 Domain Switch 还需要处理其他 CPU 上下文，不能把这个函数单独等同于完整切换实现。

### 20.5.4 关键 CSR 配置

在我们的参考平台上：

```
Domain A:
    SDID     = 1
    MPT ROOT = MPT_ROOT_A
    MODE     = Smmpt43

Domain B:
    SDID     = 2
    MPT ROOT = MPT_ROOT_B
    MODE     = Smmpt43
```

当某个 Hart 要进入 Domain B 时，RDSM 需要给该 Hart 配置对应的 `mmpt`。

从架构层面看：

```
mmpt
    |
    +-- MODE
    +-- SDID
    +-- PPN (MPT Root)
```

这是一个 per-Hart 的 M-mode CSR。

MPT Root 指向内存中的表，而不是存放完整 MPT。

RDSM 不应在未验证新状态和执行必要同步之前，就直接返回到目标 S-mode 软件。

另外要注意：`mmpt` 对有效特权模式为 M 的访问不执行普通的 MPT 权限查找。因此不能认为一张 MPT 可以独立约束 RDSM 自身的全部行为。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 20.5.5 多 Hart 启动时如何保证安全？

假设 Hart 0 完成初始化，另外 15 个 Hart 还没有启动。

一个危险实现是：

```
Hart 0:
    MPT 已初始化
    Linux 已启动

Hart 1:
    Firmware 尚未初始化 Domain State
    直接跳入 S-mode Linux
```

此时可能存在一段没有正确配置 MPT 的执行窗口。

参考设计要求：

```
Secondary Hart Wakeup
    |
    v
Enter M-mode Firmware
    |
    v
Establish Root Stack / Scratch
    |
    v
Verify Domain Assignment
    |
    v
Install MPT Root + SDID
    |
    v
Restore Interrupt Policy
    |
    v
Required Fence / Synchronization
    |
    v
mret → S-mode
```

对 Warm Boot、Resume 和 CPU Hotplug 同样必须保证这个顺序。

这背后的安全原则是：

任何 Hart 在进入不可信 Domain 之前，都必须已经具备该 Domain 的完整硬件保护配置。

否则系统即使在绝大部分时间安全，也无法满足真正的隔离要求。

## 20.6 Domain Switch Protocol：最核心的运行期机制

现在进入本章技术深度最高的部分。

### 20.6.1 为什么不是简单修改一个 SDID？

假设 Hart 3 正在运行 Linux：

```
Hart 3

Privilege : S-mode
Domain    : Linux
SDID      : 1
satp      : Linux Page Table
```

现在 Linux 中的 CA 请求执行一个 OP-TEE Trusted Application。

最终 Hart 3 可能需要变为：

```
Hart 3

Privilege : S-mode
Domain    : OP-TEE
SDID      : 2
satp      : OP-TEE Page Table
```

乍一看，只需要修改：

```
mmpt.SDID
mmpt.PPN
satp
```

但这是不完整的。

因为一个运行中的 Supervisor Domain 还拥有：

- 通用寄存器和程序计数器。
- S-mode Trap Vector、Exception State。
- Supervisor Status 和 Interrupt Enable。
- Supervisor Page Table。
- FPU、Vector 执行状态。
- Timer、Interrupt Controller 相关上下文。
- 与 Hypervisor 扩展有关的状态（如果实现 H-extension）。
- 与 Domain 相关的 Debug、Trace、QoS 配置。

如果没有正确处理这些状态，新的 Domain 可能继承旧 Domain 的中断、地址转换或敏感寄存器内容。

### 20.6.2 Domain Switch 的完整逻辑

我们设计如下切换流程：

Linux Domain → OP-TEE Domain（同一 Hart）

逻辑时序。实际底层实现还需要遵循不同 CSR、扩展状态和失效指令的架构顺序。

这个流程中的最关键阶段不是 `mret`，而是：

从保存旧 Domain State 到激活新 Domain State 之间的安全状态迁移。

### 20.6.3 参考数据结构

可以设计如下 Per-Hart Context：

```
/* 参考伪代码：省略扩展 CSR 和安全管理细节 */

struct supervisor_context {
    uint64_t gpr[32];

    uint64_t epc;
    uint64_t sstatus;
    uint64_t sie;
    uint64_t stvec;
    uint64_t sscratch;
    uint64_t satp;

    struct fp_context fp;
    struct vector_context vector;

    uint32_t domain_handle;
    uint32_t sdid;

    uintptr_t mpt_root_pa;

    uint64_t security_generation;
};
```

这里的 `security_generation` 是软件设计的版本号，用来识别过期授权或过期 Context，不是规范 CSR。

需要特别强调：`sip` 等 CSR 可能包含只读或硬件控制的状态，不能简单地通过保存一个整数，再原样写回来，就认为所有中断状态都已恢复。某些状态需要由 Interrupt Controller 和 RDSM 的专门流程处理。

### 20.6.4 参考 Context Switch 实现

我们把切换设计成 M-mode 中的一个受控事务。

```
/* 架构伪代码，不是现有 OpenSBI 的实际函数 */

int rdsm_switch_domain(struct hart_state *hart,
                       domain_handle_t target_id)
{
    struct secure_domain *target;
    struct secure_domain *current;

    current = hart->current_domain;
    target  = rdsm_lookup_domain(target_id);

    if (!target || target->state != DOMAIN_RUNNABLE)
        return -EINVAL;

    if (!rdsm_authorize_switch(current, target))
        return -EPERM;

    /*
     * 当前已处于可信 M-mode。
     * 进入受控临界区并阻止不安全的重入。
     */
    rdsm_enter_switch_critical(hart);

    /* 旧 Domain 状态保存到 Root-owned Memory */
    rdsm_save_context(hart, current);

    /* 切断旧 Domain 的执行上下文关联 */
    rdsm_quiesce_domain_state(hart, current);

    /* 安装目标 Domain 的访问控制配置 */
    rdsm_install_mpt_context(hart, target);
    rdsm_sync_mpt_if_needed(hart, target);

    /* 恢复地址转换及其他 Supervisor State */
    rdsm_restore_supervisor_context(hart, target);

    /* 恢复目标 Domain 的中断路由与授权 */
    rdsm_restore_interrupt_policy(hart, target);

    hart->current_domain = target;

    /*
     * 清理临时敏感状态，随后通过受控汇编路径
     * 返回目标 S-mode。这里不是普通 C return。
     */
    rdsm_resume_domain(hart, target);
}
```

这是一个逻辑框架，实际还需要考虑 Context 保存期间的异步异常、Nest Trap、FPU/Vector 状态、未初始化上下文、切换失败恢复和跨域共享状态。

特别是如果在修改了 `mmpt` 之后发生错误，不能简单返回旧 Domain：原本的访问权限和异常恢复环境可能已经发生变化。

因此生产实现必须设计明确的 Rollback 或 Fatal Recovery 机制。

### 20.6.5 深入到 RISC-V 汇编层

假设 RDSM 已经验证目标 Context，并准备切换到 OP-TEE。

一个简化的汇编骨架：

```
/*
 * 架构示例：
 * t0 = target mmpt CSR value
 * t1 = target S-mode entry PC
 *
 * 未展示完整 Trap Frame、寄存器恢复、
 * 同步操作和中断状态管理。
 */

    csrw 0x382, t0       /* mmpt */

    /* 在此之前或之后按实际状态执行必要的
       MPT synchronization，例如 MFENCE.PA */

    csrw mepc, t1

    csrr t2, mstatus

    li   t3, (3 << 11)   /* MSTATUS.MPP mask */
    not  t3, t3
    and  t2, t2, t3

    li   t3, (1 << 11)   /* MPP = S-mode */
    or   t2, t2, t3

    csrw mstatus, t2

    /* 恢复目标通用寄存器及其他必要状态 */

    mret
```

注意：

- `0x382` 是规范给出的 `mmpt` CSR 地址。
- `mstatus.MPP` 决定 `mret` 之后的目标权限模式。
- `mepc` 决定目标程序计数器。
- `mret` 不会自动恢复 `satp`、`stvec`、FPU/Vector 等 Domain 状态。
- 修改 `mmpt` 本身不会自动完成必要的内存保护同步。

因此，这段代码只能理解为最终进入 S-mode 的关键指令骨架，不能直接作为可执行的完整安全切换例程。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 20.6.6 一个真实 OpenSBI 源码参考

OpenSBI 已经存在 `lib/sbi/sbi_domain_context.c`，其中包括：

```
sbi_domain_context_enter()
sbi_domain_context_exit()
```

以及内部的：

```
switch_to_next_domain_context()
```

相关实现维护 Per-Hart 的 `hart_context`，保存和恢复一系列 Supervisor CSR、FP/Vector State、Trap Context，并通过 Hart Protection 机制重新配置 Domain 的访问权限。

这非常适合用于研究 SMMPT 集成中的 Context Switch。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



但需要注意，它是 OpenSBI 的通用 Domain Context 框架，而不是一份已经完成 OP-TEE/Smsd 所有安全审计的 RDSM 参考实现。

我建议重点研究下面这条代码关系：

```
sbi_domain_context_enter()
    |
    v
switch_to_next_domain_context()
    |
    +-- Save Supervisor CSRs
    +-- Restore Target CSRs
    +-- Save / Restore FP & Vector State
    +-- Switch Trap Context
    |
    v
sbi_hart_protection_reconfigure()
    |
    +-- PMP / Smepmp Backend
    +-- SMMPT Backend（RFC 扩展方向）
```

这里正好体现了一条非常优秀的软件设计原则：

把 Domain Context 管理与具体 Memory Protection Backend 解耦。

未来无论硬件采用 PMP、Smepmp 还是 SMMPT，都不应该要求上层 Domain Scheduler 重写整个 Context Switch 流程。

## 20.7 Linux → OP-TEE：完整可信调用链设计

### 20.7.1 先解决一个最常见的误解

OP-TEE 是一个 Trusted OS，但它本身并不意味着可以无条件运行在一个普通 RISC-V S-mode 中并获得安全隔离。

我们需要额外解决三个问题：

1. Linux 如何向 Trusted Domain 发起调用？
2. RDSM 如何安全地切换到 OP-TEE？
3. OP-TEE 如何接收参数并最终返回 Linux？

Arm OP-TEE 常见方案使用 SMC 和 Secure Monitor。

而在本章参考架构中，我们设计一个通过 RISC-V `ecall` 进入 M-mode RDSM 的 Transport。

这里的 `ecall` 只是陷入路径，具体的 SBI Extension ID、Function ID、参数格式及 OP-TEE 调用约定都需要明确设计，不应假定它们是已经标准化的 RISC-V TEE ABI。

### 20.7.2 完整调用时序

```
Linux Userspace
    |
    | TEEC_InvokeCommand()
    v
libteec
    |
    | ioctl(/dev/tee0)
    v
Linux OP-TEE Driver
    |
    | Build OPTEE_MSG_ARG
    | Prepare Shared Memory
    v
RISC-V TEE Transport
    |
    | SBI ecall (Proposed Extension)
    v
M-mode RDSM
    |
    | Authenticate Caller
    | Validate Shared Memory
    | Save Linux Context
    | Install OP-TEE Domain
    v
OP-TEE S-mode Entry
    |
    | Validate Request
    | Dispatch Trusted Thread
    v
OP-TEE Core
    |
    | Locate TA Session
    | Dispatch Command
    v
Trusted Application
    |
    | Execute Security Service
    v
OP-TEE Core
    |
    | Prepare Result
    | Return through RDSM
    v
Linux Driver
    |
    v
Linux Userspace
```

注意这个架构中有两个不同的保护维度：

控制流边界： Linux 必须经由 RDSM 才能进入 Trusted Domain。

数据访问边界： Linux 和 OP-TEE 只有对经过授权的 Shared Memory 才能共同访问。

仅保证其中一个是不够的。

### 20.7.3 RDSM 不应该直接信任 Linux 传递的物理地址

假设 Linux 传入：

```
request_pa = 0xA200_1000
request_size = 0x1000
```

RDSM 必须检查：

- 地址是否处于允许共享的区域？
- `pa + size` 是否溢出？
- 请求是否跨越了不允许访问的页面？
- 对应页面是否已经处于 Shared 状态？
- 是否被其他 Domain 独占？
- 目标 Domain 是否存在？
- 该请求是否符合当前调用会话的权限？
- 是否可能通过并发撤销形成 TOCTOU？

不能因为请求来自 Linux Kernel，就认为它一定安全。

例如，Linux 也可能恶意传入：

```
request_pa = 0x8300_0000
```

而这个地址正好是 OP-TEE Private Memory。

即便 RDSM 不直接读取这块内存，如果错误地将它当作 Shared Buffer，仍然可能产生严重的逻辑漏洞。

### 20.7.4 Shared Memory 是数据通道，不是所有权授权凭证

我建议将共享内存的访问验证分成两层：

第一层：RDSM

验证调用者 Domain、共享区域和 MPT Policy。

第二层：OP-TEE

验证消息格式、长度、类型、Session、TA 权限和业务逻辑。

例如 OP-TEE 收到一个 Buffer：

```
struct tee_request {
    uint32_t cmd;
    uint32_t flags;
    uint64_t buffer_pa;
    uint64_t buffer_size;
};
```

不能仅仅验证 `buffer_pa` 属于 Shared Memory。

还需要在安全侧重新检查长度、偏移、嵌套结构和实际使用的权限。

尤其不能将 Linux 提供的任意数值当成 OP-TEE 内部的可信指针。

### 20.7.5 TOCTOU：共享内存最容易被忽略的问题

假设 OP-TEE 执行：

```
if (req->length <= MAX_LEN) {
    memcpy(dst, req->buffer, req->length);
}
```

如果 `req` 位于 Linux 可写的 Shared Memory 中，那么 Linux 可以在检查后修改 `length`。

于是检查时：

```
length = 100
```

使用时：

```
length = 0xFFFFFFFF
```

这就是典型的 Time-of-Check-to-Time-of-Use 问题。

安全侧应当采用固定边界检查并复制到可信私有内存的策略：

```
/* 参考伪代码 */

struct tee_request local;

if (!validate_shared_range(req_pa, sizeof(local)))
    return TEE_ERROR_BAD_PARAMETERS;

copy_from_shared(&local, req_pa, sizeof(local));

if (local.length > MAX_LEN)
    return TEE_ERROR_BAD_PARAMETERS;

if (!validate_shared_range(local.buffer_pa,
                           local.length))
    return TEE_ERROR_BAD_PARAMETERS;

/* 对需要稳定快照的数据，复制到 Secure Private Buffer */
copy_payload_to_secure_buffer(&local);
```

这里还要对复制过程中可能变化的外部数据、对齐、整数溢出和嵌套指针做进一步处理。

SMMPT 能确保 Linux 无法访问 OP-TEE Private Memory，但不能自动保证 Linux 共享数据的可信性。

这正是硬件隔离与 Trusted OS 软件安全检查之间的职责边界。

## 20.8 Memory Ownership：如何安全地转移物理内存？

这是本章最有工程价值的内容之一。

因为在真实系统中，内存不一定从启动到关机都固定属于某一个 Domain。

例如：

- Linux 分配一块 Buffer，临时交给 OP-TEE。
- OP-TEE 将部分内存作为 Shared Memory 暴露给 Linux。
- Confidential VM 创建时，Host 把物理页面交给 Trusted Domain。
- Trusted Domain 销毁时，将内存重新归还给 Host。

如果架构只能在 Boot 时初始化 MPT，而无法安全地动态修改权限，就无法很好地支持这些场景。

### 20.8.1 Ownership 不等于访问权限

我们先区分三个概念。

Owner： 谁拥有内存资源的管理权。

Access Permission： 哪些 Domain 可以读取、写入或执行。

Physical Mapping： 某个 Domain 的页表是否将这块内存映射到了其地址空间。

这三者不是同一个概念。

例如：

```
Physical Page P

Owner:
    OP-TEE

Permissions:
    Linux  -> ---
    OP-TEE -> RW-

Mappings:
    Linux  -> 可能仍存在旧映射
    OP-TEE -> VA 0xFFFF...
```

即使 Linux 的页表中保留旧映射，MPT 仍然应该拒绝它的物理访问。

而即使我们允许 Linux 读取某个 OP-TEE 管理的 Buffer，也不表示 Linux 自动获得该 Buffer 的所有权。

### 20.8.2 设计 Memory Ownership 状态机

这是 A → B 私有页面转移的简化状态机。SHARED 是另一种带明确授权集合和生命周期的状态，不应简单等同于 OWNED_B。

一个典型的 Memory Transfer 可以定义为：

```
OWNED_A
    |
    | Revoke A
    v
REVOKING
    |
    | CPU / IOMMU / DMA Quiescence
    v
QUARANTINED
    |
    | Sanitize & Reinitialize
    v
OWNED_B
```

注意，这种状态机的安全性不取决于状态名称，而取决于状态转换过程中是否存在旧访问权限尚未失效、新访问权限已经生效的窗口。

### 20.8.3 为什么不能直接修改两张 MPT？

假设页面 P 当前属于 Linux：

```
Domain A MPT:
    P -> RW-

Domain B MPT:
    P -> ---
```

现在我们希望将它转移给 OP-TEE。

一个天真的实现可能是：

```
mpt_set_permission(domain_a, P, NO_ACCESS);
mpt_set_permission(domain_b, P, READ_WRITE);
```

看起来没有问题。

但考虑多 Hart：

```
Hart 0: 修改 Domain A 的 MPTE
Hart 1: 仍使用旧的 MPT Cache
Hart 2: 正在执行读取页面 P 的指令
Device: 正在 DMA 读取 P
```

即使内存中的 MPTE 已经更新，其他执行主体仍然可能保留旧权限或存在尚未完成的内存事务。

因此必须有完整的撤销协议。

### 20.8.4 正确的撤销与授权顺序

建议采用分阶段提交。

阶段 1：冻结资源

- 将页面状态改为 `REVOKING`。
- 拒绝新的授权或转移请求。
- 阻止设备提交新的访问事务。
- 确保软件元数据修改受到 Root-owned Lock 保护。

阶段 2：撤销旧权限

- 更新 Domain A 的 MPT。
- 令页面对 Domain A 不可访问。
- 对所有可能保留旧 MPT 状态的 Hart 执行必要同步。
- 同步 I/O MPT Checker 与 IOMMU 相关缓存。
- 等待已经发出的 DMA 和访存操作达到规定的完成或隔离边界。

阶段 3：资源重新初始化

- 确认已经没有有效的旧访问授权。
- 根据所有权转移协议保留、复制或清除数据。
- 对需要交付给新 Domain 的私有页面执行数据清理或可信初始化。
- 完成必要的 Cache Maintenance。
- 确认资源不再依赖旧 Domain 的翻译或设备状态。

阶段 4：建立新权限

- 更新 Domain B 的 MPT。
- 执行新权限生效所需的同步。
- 更新 Root-owned Ownership Metadata。
- 将资源状态正式转换成 `OWNED_B`。

阶段 5：发布完成状态

只有在所有必须的处理器和设备确认操作完成后，才向请求方报告转移成功。

这里有两个不同的场景：

- 如果是 Donate/Transfer，旧 Domain 应永久失去相应权限，除非后续执行新的授权流程。
- 如果是 Share/Lend，原 Owner 可能保留部分访问权限，但必须严格定义授权集合、同步和归还条件。

共享数据的可信性仍需要由接收方验证，不能因为发生了 Ownership Transfer 就认为其内容一定安全。

### 20.8.5 `MFENCE.PA` 在其中起什么作用？

规范定义的 `MFENCE.PA` 用于同步当前 Hart 的 MPT 更新和硬件隐式访问。

例如：

```
/* 当前 Hart 的所有 SDID 与地址 */
mfence.pa x0, x0
```

在支持相应编码的汇编器中，也可以通过寄存器参数指定相关物理地址或 SDID，缩小同步范围。

但是有一个非常重要的区别：

在 Hart 0 上执行 `MFENCE.PA`，不等于所有其他 Hart 都已经完成相同的同步，也不等于所有 DMA 已经排空。

因此 RDSM 必须建立自己的跨 Hart 协调协议。

例如：

```
Hart 0 (RDSM)
    |
    +-- Update MPTE
    |
    +-- Synchronize local MPT
    |
    +-- Send IPI to relevant Harts
    |       |
    |       +-- Hart 1: MFENCE.PA → ACK
    |       +-- Hart 2: MFENCE.PA → ACK
    |       +-- Hart 3: MFENCE.PA → ACK
    |
    +-- Synchronize I/O protection
    |
    +-- Wait for all required ACKs
    |
    +-- Commit Ownership Transition
```

这里还必须解决正在 Stop/Suspend 的 Hart 如何在 Resume 时完成补充同步，以及 `MFENCE.PA` 对 MPTE 非 Leaf 变化和 SDID 复用的具体要求。不能把所有 MPT 更新都简化为对单个 PA 的 Leaf Fence。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 20.8.6 用两个版本号解决什么问题？

我建议在软件架构中引入：

```
struct memory_object {
    uint64_t object_id;

    domain_handle_t owner;

    uint64_t generation;
    uint64_t authorization_generation;

    enum memory_state state;

    uintptr_t pa;
    size_t size;
};
```

其中：

- `generation` 用于识别资源是否经历过回收和重新分配。
- `authorization_generation` 用于识别授权策略的变化。

假设攻击者保留一个旧的 Shared Memory Handle：

```
Handle = (object_id=100, generation=7)
```

而该物理内存已经重新分配：

```
object_id=100, generation=8
```

那么旧 Handle 不能再用于访问新的资源。

这属于软件层面的 Capability/Handle 防重放机制，不属于 SMMPT 硬件本身。

它和 MPT 的关系是：

- MPT 阻止物理内存越权。
- Generation/Capability 检查阻止软件层面的旧授权重用。

生产架构中二者都非常重要。

## 20.9 DMA 与 Interrupt：把隔离扩展到 CPU 之外

### 20.9.1 为什么 CPU 隔离通过了，系统仍然可能不安全？

假设：

```
Linux CPU -> OP-TEE DDR
```

已经被 SMMPT 拒绝。

但 Linux 可以控制一块 DMA-capable 设备：

```
Linux
   |
   v
DMA Controller
   |
   v
OP-TEE DDR
```

如果这个 DMA Controller 可以直接通过总线访问目标物理地址，而没有相应的 I/O Protection，那么 CPU 端 MPT 根本不能阻止这次访问。

因此我们的威胁模型要求：

每条能够访问敏感 DDR 的 DMA 路径，都必须经过适当的访问控制机制。

### 20.9.2 DMA 请求的保护链

理想的 I/O MPT 流程：

```
DMA Request
    |
    | Requester / Device Identity
    v
Supervisor Domain Classifier
    |
    | Identify Domain + IOMMU
    v
IOMMU
    |
    | IOVA → Supervisor Physical Address
    v
I/O MPT Checker
    |
    | Evaluate Domain's MPT Permission
    v
System Interconnect
    |
    v
DDR
```

规范中的 I/O MPT Checker 逻辑上包含 Supervisor Domain Classifier（SDCL）和 MPT Check 两部分，并明确指出 DMA 设备可以在其所属 Supervisor Domain 未被任何 Hart 调度时持续运行。因此，对应的 I/O 权限检查不能简单依赖当前 Hart 的 `mmpt`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里最需要检查的不是“有没有 IOMMU”，而是三件事：

1. DMA Request 是否能够被可靠关联到正确的 Domain？
2. 地址转换完成后，是否执行了对应 Domain 的物理权限检查？
3. IOMMU、I/O MPT 和设备缓存中的旧授权是否能够安全失效？

### 20.9.3 Device Assignment 设计

我们定义以下策略：

| Device          | Owner   | DMA 权限           |
| --------------- | ------- | ---------------- |
| Normal NVMe     | Linux   | Linux Memory     |
| Normal Ethernet | Linux   | Linux Memory     |
| Secure Crypto   | OP-TEE  | OP-TEE/授权 Buffer |
| Secure Storage  | OP-TEE  | Secure/授权 Buffer |
| Debug DMA       | Root 管控 | Production 默认禁止  |

最关键的是：

一个设备即使属于 Linux，也不能因此获得所有物理地址的访问权限。

此外，设备 MMIO 寄存器的保护也十分重要。

如果 Linux 可以修改 Secure Crypto 的 DMA Descriptor Base 或 IOMMU 配置，那么即使 OP-TEE 可以安全访问该外设，也不能确保设备实际运行在可信配置下。

### 20.9.4 中断路由设计

我们要求 Secure Device 的中断不能被 Linux 直接劫持。

假设：

```
Secure Crypto
    |
    v
APLIC
    |
    v
IMSIC
    |
    v
OP-TEE Domain
```

`Smsdia` 为 Supervisor Domain 提供对应的 Interrupt Domain 选择机制。

这里区分两个 ID：

- `SDID`：用于 Supervisor Domain 的 Hart-local 状态及内存保护。
- `SIDN`：用于选取 Supervisor Interrupt Domain。

二者不能当作同一种标识。

当 RDSM 切换 Domain 时，除了 `mmpt`，还可能需要更新 `msdcfg.SIDN`，确保目标 S-mode 访问的是正确的 IMSIC Interrupt File。规范还讨论了 `mideleg`、`hgeip/hgeie` 等与中断委托有关的状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 20.9.5 如果没有 Smsdia 怎么办？

不能认为没有 Smsdia 就完全不能做隔离。

一个保守的过渡方案是：

- 将 Secure Device Interrupt 路由到 M-mode。
- RDSM 确认对应中断的 Owner。
- 将事件传递给 OP-TEE Domain。
- OP-TEE 被调度后处理相关事件。

缺点是 M-mode 中断处理、调度和通知会增加延迟。

但相对于让 Linux 直接管理 Secure Device 的中断，安全边界更清晰。

生产实现必须进一步验证中断重放、丢失、优先级、Pending State 和跨 Hart 投递行为。

## 20.10 SMP、缓存与一致性：16 个 Hart 如何同时保证隔离？

这是高并发场景下最难设计正确的部分之一。

### 20.10.1 Per-Hart 与 Global State

我们把系统状态分为两类。

| Per-Hart State      | Global State              |
| ------------------- | ------------------------- |
| `mmpt`              | Domain Ownership Database |
| `satp`              | MPT Memory                |
| Trap Context        | Device Assignment         |
| FPU / Vector State  | Memory Object Registry    |
| Active Domain       | Global Policy             |
| Interrupt Selection | Shared Memory Lifecycle   |

为什么要区分？

因为 Per-Hart State 由各 Hart 独立维护，而 Global State 可能同时被多个 Hart 修改和观察。

例如：

```
Hart 0: Linux Domain
Hart 1: Linux Domain
Hart 2: OP-TEE Domain
Hart 3: OP-TEE Domain
```

即使两个 Hart 正在运行相同 Domain，它们仍拥有各自独立的 CPU Context 和相关硬件缓存状态。

### 20.10.2 多 Hart 修改 MPT 的并发问题

考虑：

```
Hart 0: Grant Shared Page P
Hart 1: Revoke Shared Page P
Hart 2: Access P
```

如果没有控制并发操作顺序，可能出现：

```
Hart 0 writes MPTE = RW
Hart 1 writes MPTE = NO_ACCESS
Hart 0 completes fence
Hart 2 observes inconsistent policy
```

因此 MPT Manager 必须定义明确的并发模型。

我建议：

- 对每个 Memory Object 使用独立的 Ownership Lock。
- 对 MPT Tree 的结构性修改使用专门的 Table Lock。
- 对 MPTE Permission Update 使用版本号或序列化提交。
- 对 Remote Fence 维护目标 Hart Mask 和完成确认。
- 避免任意 CPU 直接绕过 RDSM 修改 MPTE。

对于 Grant/Revoke 并发，最简单且安全的早期方案是按 Memory Object 完全串行化。

后期可以通过分区锁和批量操作优化性能，但必须保留原有安全不变量。

### 20.10.3 为什么单独执行 `SFENCE.VMA` 不够？

因为系统中存在两种不同的缓存语义。

地址转换缓存：

```
VA → PA
```

主要由 `satp`、`SFENCE.VMA` 等机制控制。

MPT 保护状态缓存：

```
PA + Domain → Access Permission
```

由 MPT 相关指令同步。

因此：

```
sfence.vma
```

不应该被理解为“已经更新了所有物理访问权限缓存”。

同样：

```
mfence.pa x0, x0
```

也不是对所有 Supervisor VA Translation 和 I/O Translation 的通用同步替代品。

### 20.10.4 安全同步应该覆盖四个层次

| 层次              | 必须考虑什么                                    |
| --------------- | ----------------------------------------- |
| CPU Translation | VA/GPA 转换相关失效                             |
| CPU MPT         | MPT 权限更新和 Remote Fence                    |
| I/O Translation | IOMMU/IOTLB/Device Translation Cache      |
| Memory System   | In-flight Access、Cache Coherency、Ordering |

其中最后一层容易被忽略。

例如，即使设备的未来 DMA 被禁止，也不能假设之前提交的 DMA Write 已经完成。

因此对真实 SoC，必须明确总线事务的完成语义、设备 Quiesce 机制、IOMMU Fence 和缓存一致性模型。

### 20.10.5 性能优化不能破坏同步顺序

一种常见优化是批量修改 MPT：

```
Update Page A
Update Page B
Update Page C
Update Page D
       |
       v
Batch Synchronization
```

与每页都发起一次全局同步相比，批量操作可能显著降低跨 Hart 通信成本。

但前提是：

在批次提交完成前，不能向其他 Domain 宣告相关授权或撤销已经生效。

对于需要频繁安全转换的工作负载，可以采用：

- Batch Grant / Revoke。
- MPT Table Preallocation。
- Granularity-aware Mapping。
- 尽量减少跨 Domain Switch。
- Per-Domain Per-Hart Context Cache。
- 选择性 Remote Fence。

这里的优化收益和安全性均需要实际测量验证，不能仅凭软件执行的 Fence 数量判断。

## 20.11 从架构到代码：模块分层与接口设计

现在我们要完成架构设计里非常重要的一步：

将硬件机制与安全策略转化为可以维护、测试和替换的软件模块。

### 20.11.1 推荐的软件分层

我建议把 M-mode 侧的软件分成以下层次：

```
                 RDSM / Security Manager
                           |
          +----------------+----------------+
          |                |                |
   Domain Manager    Memory Manager    Device Manager
          |                |                |
          +----------------+----------------+
                           |
                  Protection Abstraction
                           |
              +------------+------------+
              |                         |
        PMP / Smepmp                  SMMPT
                                        |
                              MPT Table Manager
                                        |
                              CSR / Fence Backend
                                        |
                                     Hardware
```

这种分层的目的不只是代码整洁，而是让每个组件的安全责任独立、可审计。

例如：

Domain Manager

- 决定是否允许创建 Domain。
- 维护 Domain 生命周期。
- 检查 Domain Switch 权限。

Memory Manager

- 维护物理资源的 Owner。
- 管理 Shared Memory。
- 处理 Grant、Revoke、Donate。

MPT Table Manager

- 创建 MPTE。
- 维护多级 MPT。
- 处理映射粒度变化。
- 维护内部锁和版本状态。

Hardware Backend

- 编程 `mmpt`。
- 执行 `MFENCE.PA`。
- 查询 SoC 支持的 MPT Mode。
- 处理 Hart-local 配置。

将业务策略和 CSR 操作混在一起，会使后续安全审计和平台移植非常困难。

### 20.11.2 推荐的 RDSM 内部 API

```
/* 设计接口：不是标准 SBI API */

struct rdsm_memory_desc {
    uintptr_t pa;
    size_t size;

    uint32_t owner;
    uint32_t permissions;
    uint32_t flags;
};

int rdsm_domain_create(
    const struct domain_manifest *manifest,
    domain_handle_t *handle);

int rdsm_domain_destroy(
    domain_handle_t handle);

int rdsm_memory_grant(
    domain_handle_t source,
    domain_handle_t target,
    const struct rdsm_memory_desc *desc);

int rdsm_memory_revoke(
    domain_handle_t target,
    uintptr_t pa,
    size_t size);

int rdsm_memory_donate(
    domain_handle_t source,
    domain_handle_t target,
    uintptr_t pa,
    size_t size);

int rdsm_domain_enter(
    domain_handle_t target);
```

这些接口在 M-mode 内部使用。

对外暴露的 SBI 接口应该更小，并且必须为不同类型的调用者设计权限约束。

例如，Linux 可以发起“请求共享某个 Buffer”，但不应该直接获得：

```
mpt_set_permission(arbitrary_domain,
                   arbitrary_pa,
                   arbitrary_permissions);
```

否则 RDSM 等于将最核心的安全权限控制权交回了不可信系统。

### 20.11.3 与现有 OpenSBI 代码如何对应？

下面这张表可以作为实际阅读源码时的索引。

| 子系统             | OpenSBI 代码位置                    | 建议研究重点                  |
| --------------- | ------------------------------- | ----------------------- |
| SBI Domain      | `lib/sbi/sbi_domain.c`          | Domain/Memory Region 管理 |
| Domain State    | `lib/sbi/sbi_domain_state.c`    | 每个 Domain 的扩展状态         |
| Domain Context  | `lib/sbi/sbi_domain_context.c`  | Per-Hart 上下文切换          |
| Hart Protection | `lib/sbi/sbi_hart_protection.c` | 保护机制抽象                  |
| MPT Core（RFC）   | `lib/sbi/sbi_mpt.c`             | MPT 分配、映射和授权            |
| Hart MPT（RFC）   | `lib/sbi/sbi_hart_mpt.c`        | MPT Mode、MPTE 与硬件操作     |
| Hart 初始化        | `lib/sbi/sbi_hart.c`            | Cold Boot 与 Hart 配置     |

现有 OpenSBI Domain 文档描述了 Domain、Region、Assigned Hart、Boot Hart、下一阶段入口等基础抽象。这些可以被复用，但新的安全策略、内存所有权和跨 Domain 通信仍需要补充。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



尤其值得研究 RFC 中的：

```
sbi_mpt_domain_add_region()
sbi_mpt_domain_remove_region()

sbi_mpt_share_region()
sbi_mpt_unshare_region()
```

这些函数已经开始体现动态 MPT 管理思路。

[image](https://www.google.com/s2/favicons?domain=https://lore-kernel.gnuweeb.org\&sz=32)

lore-kernel.gnuweeb.org



不过，我建议在阅读源码时追问：

- API 是否验证调用方？
- 修改 MPTE 前是否验证 Ownership？
- MPT 更新失败能否安全回滚？
- 是否同步所有相关 Hart？
- I/O MPT 与 DMA 的状态是否一起更新？
- Domain Destroy 时是否清理所有仍然有效的授权？
- 相关安全策略是否只是依赖上层调用者正确使用 API？

这些才是真正的架构评审问题。

### 20.11.4 OP-TEE 侧应该新增什么抽象？

在我们的参考设计中，OP-TEE 不应该直接操作 M-mode 的 `mmpt`，因为它运行在独立的 S-mode Domain。

我建议将 OP-TEE 侧集成集中在几个方向：

| OP-TEE 模块          | 集成内容                      |
| ------------------ | ------------------------- |
| Architecture Entry | RDSM → OP-TEE 的入口和返回      |
| Thread Management  | Trusted Thread 运行与跨域返回    |
| Core MMU           | Secure VA/PA 映射管理         |
| Shared Memory      | 验证、映射和释放共享 Buffer         |
| Interrupt          | Secure Interrupt Dispatch |
| RPC                | 通过受控调用返回 Normal World     |
| Platform           | 设备、内存和启动配置                |

注意这里有两套非常不同的内存管理：

```
OP-TEE core/mmu
    |
    +-- Secure VA Mapping
    +-- TA Memory Mapping
    +-- Page Table Permissions

RDSM / SMMPT
    |
    +-- Domain Physical Isolation
    +-- Cross-domain Authorization
    +-- Memory Ownership
```

OP-TEE `core/mmu` 负责本 Domain 内的地址空间管理。

SMMPT 负责 Domain 之间的物理访问控制。

二者可以协作，但不应该把 MPT 的全部权限逻辑直接塞进 OP-TEE 的普通 Page Table Manager。

### 20.11.5 如何保持架构可移植？

我建议 OP-TEE 尽量不感知底层是否采用 SMMPT。

例如提供：

```
/* 设计示例 */

struct tee_domain_transport_ops {
    int (*enter_secure)(struct tee_call *call);

    int (*return_normal)(struct tee_result *result);

    int (*register_shared_memory)(
        uintptr_t pa, size_t size);

    int (*unregister_shared_memory)(
        uintptr_t pa, size_t size);
};
```

实现可以是：

```
TEE Transport
    |
    +-- Arm SMCCC Backend
    |
    +-- RISC-V RDSM/SBI Backend
    |
    +-- Other Platform Backend
```

真正的难点是，不能只考虑函数接口相同。

还必须明确不同 Backend 对 CPU Context、共享内存所有权、异步中断、RPC 和返回路径提供什么保证。

否则软件 API 看起来统一，实际安全语义却不一致。

## 20.12 性能、可维护性与安全性：如何做设计取舍？

### 20.12.1 我们需要测量哪些开销？

不建议只测一个 Domain Switch 的平均时间。

至少需要四类 Benchmark。

| Metric                  | 测量目的                   |
| ----------------------- | ---------------------- |
| Domain Switch Latency   | Linux → OP-TEE 的基础切换成本 |
| MPT Update Latency      | Grant/Revoke 一批页面的成本   |
| Remote Fence Latency    | 不同 Hart 数量下同步延迟        |
| I/O Protection Overhead | DMA 吞吐与尾延迟变化           |

此外还应测量：

- Trusted Application 的端到端调用延迟。
- Secure Storage 的实际读写性能。
- 共享 Buffer 大小变化对吞吐的影响。
- 高并发 TA 调用时的 P95/P99 延迟。
- MPT Table Memory Overhead。
- Secure Interrupt 的响应时间。
- Linux 在 Trusted Domain 活跃时受到的性能影响。

### 20.12.2 一个值得做的性能实验

假设测试两种方案：

方案 A：每次共享一个页面，立即执行同步。

方案 B：预先验证 128 个页面，批量修改 MPT 并完成同步。

理论上，方案 B 可以减少固定同步开销。

但我们不能直接判断 B 一定更快。

因为还要考虑：

- 批量操作占用的 Lock 时间。
- MPT Leaf Split/Merge。
- 远端 Hart 数量。
- 是否触发大范围权限缓存失效。
- 设备侧 IOTLB 同步。
- CPU 上下文切换和调度。

更合理的测量方法是：

```
Batch Size:
    1 / 4 / 16 / 64 / 128 / 512 pages

Hart Count:
    1 / 2 / 4 / 8 / 16

Metrics:
    Average Latency
    P99 Latency
    Throughput
    Remote Fence Time
    Memory Overhead
```

最后根据真实结果确定是否需要 Batch Optimization。

### 20.12.3 优化策略的优先级

我建议按以下顺序考虑优化：

1. 先保证权限设计正确。
2. 测量并找到真正的瓶颈。
3. 减少不必要的 Domain Switch。
4. 减少重复的 MPT Update 和同步。
5. 尽量复用已经验证的 Shared Memory。
6. 优化大范围内存映射。
7. 最后再考虑更复杂的 Lock-free、RCU 等并发机制。

对于安全系统来说，最危险的做法之一就是：

在尚未证明撤销与同步正确的情况下，为了降低延迟先减少 Fence。

## 20.13 完整验证设计：如何证明隔离真的有效？

系统正常启动只是验证工作的第一步。

如果 Linux 和 OP-TEE 都能运行，却没有测试越权访问是否被拒绝，那么最多只能说明功能集成成功。

### 20.13.1 正向功能验证

首先保证基本软件链正常：

```
[PASS] RDSM Initialization
[PASS] Linux Domain Creation
[PASS] OP-TEE Domain Creation
[PASS] Linux Boot
[PASS] OP-TEE Boot
[PASS] TEEC_OpenSession
[PASS] TEEC_InvokeCommand
[PASS] Secure Storage
[PASS] 16-Hart SMP
[PASS] Domain Switch / Return
```

这些是功能验收项。

### 20.13.2 负向安全验证矩阵

| ID     | 测试场景                       | 预期结果                     |
| ------ | -------------------------- | ------------------------ |
| SEC-01 | Linux 读取 OP-TEE Private PA | Access Fault             |
| SEC-02 | Linux 写 OP-TEE Private PA  | Access Fault             |
| SEC-03 | Linux 执行 OP-TEE Private PA | Instruction Access Fault |
| SEC-04 | Linux 修改 MPT Pool          | 访问被拒绝                    |
| SEC-05 | Linux 伪造 Target Domain ID  | RDSM 拒绝                  |
| SEC-06 | Linux 伪造 Shared Memory PA  | RDSM/OP-TEE 拒绝           |
| SEC-07 | Linux DMA 读取 Secure DDR    | I/O 保护拒绝、记录 Fault        |
| SEC-08 | Revoke 后再次读取旧页面            | 旧授权失效                    |
| SEC-09 | 多 Hart 并发 Grant/Revoke     | 无越权窗口                    |
| SEC-10 | Domain Switch 中故障注入        | 无保护失效，安全恢复或终止            |
| SEC-11 | Secondary Hart Warm Boot   | 正确恢复 Domain 权限           |
| SEC-12 | Secure Interrupt 注入/路由     | 仅授权 Domain 处理            |

这里的 Fault 结果需要结合实际 Trap、RDSM 记录及硬件错误状态确认。

例如，MPT 权限失败通常表现为对应访问类型的 Access Fault，而不是把它当作普通的页表 Page Fault。实际 SoC 还需要提供足够的诊断信息来区分 MPT、PMP、PMA 等可能来源。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 20.13.3 Fault Injection：验证最容易出现漏洞的边界

假设 RDSM 正在执行：

```
Revoke Page P
    |
    v
Update MPTE
    |
    v
Remote Fence
    |
    v
Update Ownership
```

我们刻意在不同位置注入异常：

- 更新一半 MPTE 后发生错误。
- Remote Hart 不回应同步请求。
- Domain 在等待 Fence 时被 Suspend。
- IOMMU 仍存在旧翻译。
- DMA Descriptor 已经提交，尚未完成。
- Root-owned Metadata 更新失败。
- Grant 操作与 Revoke 操作并发。

然后验证系统是否：

1. 继续保持旧 Domain 不可越权；
2. 将相关页面保留在不可授权状态；
3. 能恢复到明确的安全状态；
4. 不会错误地向请求方报告成功。

一个安全的失败状态，往往比一次成功执行更加有架构价值。

### 20.13.4 Palladium/ZeBu 等预硅环境重点验证什么？

在具备 RTL 或硬件跟踪能力的环境中，应该观察：

```
Hart / Domain ID
    |
    v
Physical Address
    |
    v
MPT Permission Lookup
    |
    v
Final Access Permission
    |
    v
Interconnect Transaction
```

对 DMA 还要观察：

```
Requester Identity
    |
    v
SDCL Classification
    |
    v
IOMMU Translation
    |
    v
I/O MPT Check
    |
    v
DDR Transaction
```

如果软件报告一次访问被拒绝，最好同时确认：

- CPU 的实际权限请求。
- 参与匹配的 Domain/MPT 状态。
- 硬件是否生成了预期的 Fault。
- DDR Controller 侧是否没有发生被禁止的访问。

软件层面通过测试，并不总能证明底层没有发送过不该发送的事务。

### 20.13.5 规范一致性测试与安全测试必须分开

例如：

规范一致性测试可能检查：

```
When MPTE.XWR = 000:
    Load must fault.
```

安全测试则需要检查：

```
When Domain A loses ownership:
    No A Hart or assigned device can access P,
    including concurrent, delayed and resumed accesses.
```

第二类测试涉及跨模块、跨时序和跨执行主体的联合行为。

这也是为什么 Staff Engineer 级别的系统安全验证不能只依赖 ISA Compliance Test。

## 20.14 最终架构评审：这套方案能否进入 Silicon？

在正式流片之前，我会对这套方案进行如下审查。

### 20.14.1 架构评审表

| 评审领域                | 核心审查问题                     |
| ------------------- | -------------------------- |
| Trust Boundary      | Linux 是否有任何路径能够修改 Root 状态？ |
| Memory Isolation    | 所有私有物理页面是否默认拒绝其他 Domain？   |
| MPT Management      | 是否支持安全的动态 Grant/Revoke？    |
| Domain Switch       | 是否完整保存、恢复、隔离所有必要状态？        |
| Interrupt Isolation | 安全中断能否被误路由或注入？             |
| DMA Isolation       | 所有 DMA 路径是否都受到对应保护？        |
| SMP                 | 权限撤销是否覆盖全部相关 Hart？         |
| Boot Security       | Linux 执行前是否已建立隔离？          |
| Recovery            | 中途失败能否保持安全状态？              |
| Validation          | 是否有足够的正向、负向和故障注入测试？        |

### 20.14.2 最值得警惕的五个风险

风险一：MPT 配置正确，但 DMA 路径没有等效保护。

必须核对真实 SoC 的所有 DMA Master 和总线连接关系。

风险二：Single-Hart 测试通过，Multi-Hart Revoke 存在旧权限窗口。

必须验证 Remote Fence、Hart Resume 和 In-flight Transaction。

风险三：Domain Switch 只替换 `mmpt`，没有完整处理 Supervisor Context。

必须审计 FPU/Vector、Interrupt、MMU、Trap 和扩展 CSR。

风险四：不可信 Linux 可以绕过授权 API 修改 MPT。

必须将 MPT 与安全策略元数据放入 Root-owned Memory。

风险五：安全启动成功，但 Reset/Resume/Debug 生命周期存在保护缺口。

必须审查所有能够改变 Domain、Hart 和安全配置的状态转换路径。

还需要额外说明：缓存、分支预测器、性能计数器、共享资源争用等可能引入微架构侧信道。SMMPT 的权限隔离不能自动解决这些问题。对于要求更高的 Confidential Computing 系统，还需要单独制定 Side-channel Threat Model。

### 20.14.3 最终 Architecture Design Document 应包含什么？

如果将本章真正落实成项目文档，我建议输出以下内容：

```
SMMPT_OPTEE_ARCHITECTURE/
|
+-- 01_System_Requirements
+-- 02_Threat_Model
+-- 03_Trust_Boundary
+-- 04_Domain_Resource_Model
+-- 05_Memory_Layout_and_MPT
+-- 06_Boot_and_RoT
+-- 07_Domain_Switch_Protocol
+-- 08_TEE_Communication_ABI
+-- 09_DMA_and_Interrupt_Isolation
+-- 10_SMP_and_Synchronization
+-- 11_Performance_and_Tradeoffs
+-- 12_Verification_and_Security_Tests
```

每份文档都需要明确：

- 架构约束和假设。
- Hardware/Firmware/OS 的职责。
- 数据结构和模块接口。
- 正常流程和错误流程。
- 安全不变量。
- 验收标准。

当这十二部分能够互相引用、无明显冲突，并且有实际测试覆盖时，才可以说我们形成了一套相对完整的 SMMPT + OP-TEE 安全系统方案。

## 20.15 Staff Engineer 级综合测试题（10 道）

下面的题目不再是单个概念的记忆测试，而是模拟实际项目中的架构评审、代码审查、故障定位和安全设计决策。

建议先独立思考，再阅读参考答案。

技术校正：20.4.3 中有关 Smmpt43 Leaf MPTE 的描述应精确为：每个最低层非 NAPOT Leaf MPTE 包含 16 组 XWR 权限，每组对应一个 4 KiB Page，共覆盖 64 KiB。规范中的 `NUMPGINRANGE=4` 指的是选择 16 组权限需要 4 个索引位，而不是只包含 4 个页面。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### Q1. 物理地址权限查询：为什么 Linux 能建立页表映射，却仍然无法访问 OP-TEE 内存？

Architecture / MMU / MPT

场景：

一个 RV64 SoC 使用 Smmpt43，Linux Domain 和 OP-TEE Domain 分别拥有独立 MPT。

物理地址 `0x8300_5000` 位于 OP-TEE Private Memory。

Linux 已经成功建立了一条从虚拟地址 `0xFFFF_0000_1000_0000` 到该物理地址的映射，Linux PTE 具有 R/W 权限，但执行 Load 依然触发异常。

要求：

1. 解释整个权限检查链。
2. 计算 Smmpt43 的 MPT 各级索引。
3. 说明如何判断是 MPT Fault 还是普通 Page Fault。
4. 如果 Linux 修改 `satp` 或执行 `SFENCE.VMA`，能否绕过隔离？

参考答案与考察重点：

Smmpt43 将 Supervisor Physical Address 分成三级索引和 16-bit Range Offset。

对于 `0x8300_5000`：

```
pn[2] = (PA >> 34) & 0x1FF = 0
pn[1] = (PA >> 25) & 0x1FF = 65
pn[0] = (PA >> 16) & 0x1FF = 256

XWR tuple index = (PA >> 12) & 0xF = 5
```

硬件从 `mmpt.PPN` 指向的 Root MPT 开始逐级查询，最终读取该地址对应的 XWR Permission。

如果 Linux MPT 的对应权限为 `000`，则这次 Load 无法成功。

因此，即使 Linux MMU 已经完成地址转换，Linux 仍然不能绕过 MPT。

判断异常时，需要首先查看 Trap Cause 是否为 Access Fault，再结合 MPT、PMP、PMA 和地址转换的状态排查。不能仅仅因为触发 Access Fault 就断言一定来自 MPT。

`SFENCE.VMA` 同步的是相关地址转换状态，不是对其他 Domain 的物理内存访问授权。

Staff 级加分点： 能进一步解释硬件进行隐式页表访问时的权限检查，以及 MPT Table Walk 对 PMP/PMA 的依赖。

### Q2. 多 Hart 权限撤销：为什么更新 MPTE 后仍然发生越权读取？

SMP / Race / Synchronization

场景：

16-Hart 系统中，Hart 0 通过 RDSM 撤销 Linux 对某页面的读取权限。

RDSM 已经写入新的 MPTE，并在 Hart 0 执行了 `MFENCE.PA`。

但测试发现，Hart 7 偶尔仍然能够读取旧页面。

要求：

1. 提出至少三个可能原因。
2. 设计一个正确的跨 Hart 撤销协议。
3. 解释为什么仅靠 Memory Barrier 不够。
4. 如果 Hart 7 正处于 Suspend，应该怎么办？

参考答案与考察重点：

可能原因包括：

- Hart 7 保留了旧的 MPT Permission Cache。
- 跨 Hart IPI 没有正确执行或完成。
- MPTE Store 对目标 Hart 尚未建立规定的可见性。
- 相关的 In-flight Access 在撤销前已经获得授权。
- Hart 7 在 Resume 时恢复了过期的上下文或缓存状态。

正确的撤销流程需要：

```
Lock Object
    |
    v
Mark REVOKING
    |
    v
Update MPT
    |
    v
Establish Required Ordering
    |
    v
Remote MPT Synchronization
    |
    v
Wait Required ACKs
    |
    v
Quiesce Outstanding Accesses
    |
    v
Commit Revocation
```

对于 Suspend Hart，RDSM 必须保证它在重新进入相应 Domain 之前完成必要的同步，不能因为当前不运行就忽略其潜在旧状态。

单纯的 Memory Barrier 无法替代 MPT 缓存失效协议。

Staff 级加分点： 能区分指令已发起、权限检查已完成、内存事务已完成三个不同的时间点，并为 Revocation Completion 定义可验证的线性化边界。

### Q3. DMA 绕过：CPU 隔离全部通过，为什么 Secure Key 仍然泄漏？

SoC Security / DMA / IOMMU

场景：

Linux 的 CPU 无法读取 OP-TEE Private Memory。

但是攻击者通过 Linux 控制的一块 PCIe 设备执行 DMA，成功读取了 Secure Key 所在内存。

系统已经集成 IOMMU。

要求：

1. 分析为什么 IOMMU 的存在仍不足以证明隔离。
2. 解释 I/O MPT Checker 的职责。
3. 给出 SoC 安全审查清单。
4. 如果设备支持 ATS，应重点检查哪些行为？

参考答案与考察重点：

IOMMU 主要负责设备地址转换和相关权限，但如果 Linux 可以控制 IOMMU 映射策略，或者系统没有将设备关联到正确的安全 Domain，那么 IOMMU 仍可能允许设备访问某个物理地址。

需要检查：

```
Device Identity
    |
    v
Domain Classification
    |
    v
IOMMU Translation
    |
    v
I/O MPT Permission Check
```

重点包括：

- Requester ID 是否可信。
- 设备是否能够伪造其他 Domain 身份。
- IOMMU 配置寄存器是否受保护。
- I/O MPT 是否真正覆盖最终访问。
- ATS 缓存的旧 Translation 是否能正确失效。
- 是否存在未经过检查的旁路 DMA Master。
- DMA In-flight 请求如何完成安全撤销。

Staff 级加分点： 能将所有 Bus Master 列成矩阵，并从真实互连拓扑证明没有绕过 I/O 保护的访问路径。

### Q4. Shared Memory 攻击：为什么已经有 SMMPT，OP-TEE 仍然发生内存破坏？

OP-TEE / TOCTOU / API Security

场景：

Linux 与 OP-TEE 使用合法的 Shared Memory。

OP-TEE 对 Buffer Size 做了边界检查，但随后发生了 Heap Overflow。

要求：

1. 解释 SMMPT 为什么无法阻止该问题。
2. 分析至少两种可能的 TOCTOU 攻击。
3. 设计安全的 Buffer Validation 方案。
4. 如何处理包含嵌套指针的 Request？

参考答案与考察重点：

SMMPT 控制的是 Domain 对物理内存的权限，而不负责保证共享内存内容可信。

Linux 可以合法修改 Shared Memory，甚至在 OP-TEE 检查前后修改其中的字段。

如果 OP-TEE 两次读取长度字段：

```
if (req->size <= MAX_SIZE)
    memcpy(dst, req->src, req->size);
```

两次 `req->size` 的值可能不同。

处理方式包括：

- 将请求元数据复制到 Secure Memory。
- 对复制后的稳定快照做边界验证。
- 对 Offset、Size、End 进行溢出检测。
- 对嵌套结构进行递归验证和必要的深复制。
- 在访问共享 Payload 时保持明确的并发语义。
- 不信任来自 Linux 的物理地址、Session Handle 和权限声明。

Staff 级加分点： 能解释为什么只复制顶层结构体仍可能不够，以及如何防止嵌套 Buffer、异步回调和 RPC 带来的二次 TOCTOU。

### Q5. Domain Switch 漏洞：修改了 `mmpt`，却泄漏了 Linux 的执行状态

Context Switch / CSR / Security

场景：

一位工程师实现了 Linux → OP-TEE Domain Switching。

代码主要做了：

```
csrw mmpt, t0
csrw satp, t1
csrw mepc, t2
mret
```

单次调用看起来能够成功。

要求：

1. 指出这个实现至少五个问题。
2. 说明哪些状态需要保存和恢复。
3. 解释为什么需要专门的 M-mode Root Stack。
4. 如果在切换中途发生中断，应该如何处理？

参考答案与考察重点：

问题包括：

- 没有完整保存和恢复 GPR。
- 没有处理 Supervisor Trap Context。
- 没有考虑 FPU/Vector 状态。
- 没有处理 Interrupt Routing。
- 没有执行必要的 MPT/Translation 同步。
- 没有明确目标 Domain 的合法性检查。
- 没有处理切换中途异常。
- 没有处理旧 Domain 敏感状态残留。

Root Stack 必须由 RDSM 控制，不能依赖 Linux 提供的栈来保存可信 Context。

同时，中断或异常可能发生在部分 CSR 已更新、另一部分尚未更新的阶段。因此 RDSM 需要严格定义切换临界区、异常重入状态以及失败恢复路径。

Staff 级加分点： 能针对 16-Hart、FPU、Vector、H-extension 和 AIA 的组合列出完整 CSR/Context 审计矩阵，并识别哪些状态不能通过简单的 CSR Save/Restore 解决。

### Q6. 内存 Donation 失败：为什么 Fail Closed 比 Rollback 更重要？

Memory Ownership / Fault Recovery

场景：

Linux 将 2 MiB 物理内存交给 OP-TEE。

RDSM 已经撤销 Linux 权限，但在为 OP-TEE 建立新权限时发生 MPT Table Allocation Failure。

要求：

1. 页面此时应属于什么状态？
2. 是否应立即恢复 Linux 权限？
3. 如果部分页面已经成功转移，如何处理？
4. 如何保证系统状态一致？

参考答案与考察重点：

发生失败时，不能简单地执行：

```
mpt_set_permission(linux, P, READ_WRITE);
```

因为页面可能已经经过清理、重新初始化或部分授权给 OP-TEE。

正确做法是首先将相关 Memory Object 保留在不可访问的 `QUARANTINED` 或等价状态。

然后根据事务日志、版本号和同步完成情况，判断是否能够安全回滚。

只有在确认：

- OP-TEE 没有获得有效访问；
- 所有相关 Hart 和 DMA 路径已经同步；
- 页面内容符合重新归还 Linux 的策略；
- Root-owned Metadata 与实际权限一致；

之后，才能考虑重新授权给 Linux。

Staff 级加分点： 能设计支持部分成功、嵌套失败、重复请求及电源中断的事务式 Ownership Manager。

### Q7. Boot 安全问题：为什么 Linux 只在 CPU Hotplug 之后才能绕过隔离？

Firmware / HSM / Boot / SMP

场景：

SoC 首次启动后，Linux 无法读取 Secure DDR。

但是经过以下操作：

```
Linux Boot
    |
    v
CPU Offline
    |
    v
CPU Online
```

某个 Secondary Hart 偶尔可以访问之前被禁止的物理地址。

要求：

1. 可能是什么初始化顺序错误？
2. Cold Boot 和 Warm Boot 必须分别恢复哪些状态？
3. 为什么不能默认 Boot Hart 的 MPT 配置会自动继承到其他 Hart？
4. 如何设计回归测试？

参考答案与考察重点：

首先检查 Secondary Hart 的 Warm Boot Entry。

每个 Hart 都有自己的 `mmpt` 及其他相关 CSR，不能因为 Boot Hart 已正确配置，就认为 Secondary Hart 也已经配置。

需要检查：

- Hart 从 Reset 或低功耗状态恢复后的实际 CSR 状态。
- M-mode Early Entry 是否建立可信执行环境。
- 是否恢复了正确的 SDID 与 MPT Root。
- 是否在进入 S-mode 之前完成同步。
- 是否恢复了正确的 Interrupt Assignment。
- 是否存在过期的 Domain Context。
- Resume 路径是否允许绕过 RDSM。

回归测试应该覆盖随机 Hart Offline/Online、16-Hart 并发启动、Suspend/Resume，以及启动时的权限违规检测。

Staff 级加分点： 能把 Reset Domain、Power Domain、Hart-local CSR Retention 和 MPT Cache Retention 的硬件属性与 Firmware 恢复流程关联起来。

### Q8. 规范与硬件不匹配：如果 `SDIDLEN=0` 或仅支持 Bare Mode，如何设计降级方案？

ISA Discovery / Compatibility / Security Policy

场景：

Firmware 已经能够编译并包含 SMMPT 支持，但实际芯片发现：

- 某些 Hart 的 `SDIDLEN=0`。
- 某些 Hart 不支持 Smmpt43。
- 某些配置只允许 `mmpt.MODE=Bare`。

要求：

1. Firmware 应当如何检测硬件能力？
2. 是否能够继续启动两个隔离的 S-mode Domain？
3. 什么情况下允许降级？
4. 什么情况下必须拒绝启动？

参考答案与考察重点：

Firmware 首先应通过扩展发现机制和 CSR WARL 行为检查硬件能力，包括实际可用的 SDID 位数、MPT Mode 及相关 Hart 的兼容性。

`SDIDLEN=0` 不意味着所有 Memory Protection 完全无效，但不能假设具有多个可区分 SDID 的能力。

`Bare` Mode 也不能提供本章所依赖的细粒度 MPT 权限控制。

如果安全产品的需求明确要求 Linux 和 OP-TEE 使用 SMMPT 形成独立隔离，那么不满足这些前提时应 Fail Closed，拒绝进入不可信运行环境。

若产品架构允许使用另一种已经完成安全评估的 PMP/Smepmp 隔离方案，则可以选择经验证的替代方案。

Staff 级加分点： 能区分功能兼容性降级和安全属性降级，并保证 Attestation/平台配置正确反映当前实际采用的隔离机制。

### Q9. Secure Interrupt 故障：为什么 OP-TEE 设备中断错误地进入了 Linux？

AIA / IMSIC / RDSM

场景：

Secure Crypto Engine 被分配给 OP-TEE。

在 Domain Switching 后，OP-TEE 可以正常访问设备 MMIO，但中断偶尔进入 Linux。

要求：

1. 解释 Device Ownership 与 Interrupt Ownership 为什么需要分开管理。
2. 分析 `SDID`、`SIDN`、APLIC 和 IMSIC 的关系。
3. 提出完整的 Debug 路径。
4. 如何防止不可信 Domain 伪造 Secure Interrupt？

参考答案与考察重点：

MPT 只控制对应的物理内存访问权限，并不能自动保证中断路由正确。

需要检查：

```
Device
    |
    v
APLIC Source Configuration
    |
    v
MSI Routing
    |
    v
IMSIC Interrupt Domain
    |
    v
Current Hart's msdcfg.SIDN
```

还要检查：

- `mideleg` 是否符合目标策略。
- 中断是否使用了错误的 Interrupt File。
- Domain Switch 是否切换了必要的 Interrupt State。
- Pending Interrupt 是否来自旧 Domain。
- Linux 是否拥有中断控制器的配置权限。
- MSI 写入目标地址是否受到有效的设备与域隔离。

Staff 级加分点： 能对 Domain 未运行时到来的中断设计正确的排队、通知和重新调度协议，并说明如何避免丢失、重复投递或跨域错误投递。

### Q10. 最终架构评审：你会批准这套设计进入 Silicon 吗？

Principal / Staff Architecture Review

场景：

一个 16-Hart RISC-V SoC 已经完成以下功能：

- OpenSBI + SMMPT Boot。
- Linux 正常启动。
- OP-TEE 正常启动。
- Linux 可以调用 Trusted Application。
- CPU 端的 Secure Memory 访问测试全部通过。
- 两个 Domain 的 Context Switch 可以正常执行。

团队认为可以进入流片。

但你发现：

1. 尚未完成 I/O MPT 的 DMA 负向测试。
2. Remote Fence 只有单 Hart 测试结果。
3. Secure Interrupt 的 Domain Assignment 尚未稳定。
4. Suspend/Resume 没有安全回归测试。
5. MPT 更新失败时的恢复逻辑不完整。

作为 Staff Engineer，你会如何决策？

参考答案与考察重点：

我不会仅仅因为 Linux、OP-TEE 和 Trusted Application 可以启动就认为这套安全架构可以进入生产使用。

首先要把功能通过与安全保证分离。

目前系统已经证明：

```
Functional Integration: Partially Completed
```

但尚未证明：

```
Security Isolation: Complete
```

我会将问题分成两类。

第一类：直接影响安全不变量的阻断问题。

包括：

- DMA 越权风险。
- Remote MPT 权限同步不完整。
- 安全中断越权路由。
- Suspend/Resume 产生未受保护的执行窗口。
- MPT 事务失败后产生权限不一致。

这些问题直接影响 Secure Domain 的保密性、完整性或隔离边界，不能仅仅作为普通功能缺陷处理。

第二类：可以通过明确约束或功能缩减解决的问题。

例如：

- 暂时禁用动态 Memory Donation。
- 禁止未完成验证的 DMA 设备访问敏感地址。
- 暂时采用 Dedicated Hart 模型。
- 禁用未经验证的 Suspend/Resume。
- 采用保守的 M-mode 中断路由。
- 将未完成验证的特性从安全产品配置中移除。

但是，这种降级只有在硬件能够强制执行限制、没有留下绕过路径，并且新的安全目标仍然成立时才可以接受。

最终决定应该依据：

1. Threat Model 是否完整。
2. 所有 Security Invariant 是否有对应实现。
3. 每条安全边界是否有实证验证。
4. 未解决风险是否有可强制执行的缓解措施。
5. 是否满足产品安全需求及流片退出标准。

如果缺失的验证依赖 RTL 修改才能完成，且无法在流片后通过 Firmware 修复，那么这些问题尤其需要在 Tape-out 前解决。

Staff 级加分点： 能够把以上风险转化为 RTL/Firmware/Verification 联合行动项，定义责任人、阻断条件、验收证据和架构决策记录，而不只是笼统要求“继续测试”。

## Chapter 20 综合掌握标准

如果能够独立完成本章，应该具备以下能力：

掌握情况自查

0 / 8

独立定义 Linux/OP-TEE/RDSM 的威胁模型和安全不变量

设计 Supervisor Domain、内存布局与 MPT 权限矩阵

解释 Boot、Domain Switch、Shared Memory 的完整软件路径

设计安全的 Memory Grant、Revoke 和 Ownership Transfer

分析 DMA、Interrupt、SMP 对隔离架构的影响

使用 OpenSBI 源码和硬件状态定位跨 Domain 问题

设计正向、负向、并发和故障注入验证体系

完成一份能够接受 Staff/Principal Engineer 评审的方案

## 本章结束语：从理解规范到设计安全系统

完成 Chapter 20，也就完成了这套 SMMPT 课程的全部 20 个章节。

如果回顾整个课程，可以发现我们其实经历了三个层次的变化。

第一个层次：理解机制。

理解 PMP、MPT、CSR、SDID、MPTE、Table Walk 和 Fence，知道硬件为什么提供这些机制，以及它们如何工作。

第二个层次：理解系统。

开始将 SMMPT 与 OpenSBI、OP-TEE、MMU、IOMMU、Interrupt、SMP 和 Root of Trust 连接起来，真正理解安全隔离如何依赖多个硬件和软件组件的协作。

第三个层次：理解安全架构的本质。

不再仅仅问“这段代码怎么实现”，而是进一步问：

- 安全保证建立在哪些假设之上？
- 哪个组件拥有最终决策权？
- 如果某个组件被攻陷，安全边界还成立吗？
- 如果某个操作中途失败，系统是否仍然安全？
- 我们如何用实际测试或硬件证据证明设计符合要求？

我认为第三个层次，才是真正将一个优秀的 Firmware Engineer 与具备系统安全架构能力的 Staff Engineer 区分开的关键。

尤其对于 RISC-V 这样的开放架构，理解规范只是起点。真正有价值的工作，是把规范中定义的安全原语转化为可靠、可维护、可验证的软件系统，并且能够把发现的问题反馈到 OpenSBI、OP-TEE、模拟器和架构规范的社区中。

到这里，SMMPT 理论课程正式结束。但下一步最有价值的工作，是选择一个固定的公开规范版本和可运行的 OpenSBI 实验分支，将 Chapter 20 的参考架构逐项落实成可以调试、可以触发 Fault、可以验证安全不变量的实际代码。

这才是从“掌握 SMMPT”迈向“能够真正负责一套 RISC-V TEE 安全架构”的关键一步。
