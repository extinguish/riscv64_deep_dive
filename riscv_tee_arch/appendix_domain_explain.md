# 如何从本质上理解 RISC-V 中的 Domain？

我建议你先建立一个非常重要的认知：

Domain（域）不是 RISC-V 基础 ISA 定义的一种特权级，也不是某个具体的硬件模块，而是一种用于划分资源、权限和管理边界的系统架构抽象。

你之所以会在 OpenSBI、PMP、TEE、IOMMU、WorldGuard 等不同技术中反复看到 Domain，是因为它们都在解决同一类问题：

> 在一套共享的硬件资源上，如何把不同的软件或硬件主体组织成彼此独立、具有不同权限和管理策略的集合？

但需要区分：这些技术所说的 Domain 不一定是同一种 Domain，也不一定处于同一层次。

## 一、先从最简单的实例理解 Domain

假设有一颗 RISC-V SoC：

```
RISC-V SoC
│
├── Hart 0
├── Hart 1
├── Hart 2
├── Hart 3
│
├── DDR Memory
├── UART
├── SPI
├── DMA
└── Crypto Engine
```

现在需要在同一颗 SoC 上运行两个彼此不信任的操作系统：

- Linux：处理普通应用程序。
- OP-TEE：负责安全服务和密钥管理。

如果没有资源隔离，Linux 和 OP-TEE 可能互相干扰。

例如，Linux 可以访问 OP-TEE 的物理内存，或者配置 DMA 读取其中的私钥。

因此，我们引入一个逻辑概念：Domain。

```
                  RISC-V SoC
                      │
           Trusted Domain Manager
                 (OpenSBI)
                      │
           ┌──────────┴──────────┐
           │                     │
       Domain A              Domain B
        Linux                 OP-TEE
           │                     │
       Linux RAM              TEE RAM
       UART / SPI            Crypto Engine
       Hart Resources        Hart Resources
```

在这里，Domain A 和 Domain B 分别表示两组具有独立资源权限与执行上下文的系统级环境。

真正关键的不是 Domain 的名称，而是：

1. 成员（Membership）：哪些 hart 或执行主体属于这个 Domain？
2. 资源（Resources）：这个 Domain 可以访问哪些物理内存和设备？
3. 权限（Permissions）：对这些资源可以执行 Read、Write、Execute 或其他什么操作？
4. 管理者（Authority）：谁有权创建 Domain、分配资源或修改权限？
5. 隔离机制（Enforcement）：谁保证一个 Domain 无法越权？
6. 生命周期（Lifecycle）：Domain 如何启动、运行、切换、停止和销毁？

你可以先把一个 Domain 抽象成：

\\[ \text{Domain} = \\{\text{成员、资源、权限、策略、生命周期}\\} \\]

其中管理者和硬件 Enforcement Mechanism 决定这些规则能否真正成为安全保证。

这比把 Domain 简单理解成「一个安全区域」更准确，因为 Domain 有时表示安全隔离边界，有时只表示资源组织或管理边界。

## 二、为什么说 Domain 是抽象概念，而不是一种硬件机制？

可以用你熟悉的 Linux 和 Arm TrustZone 作类比。

### 2.1 Linux Process 也可以理解为一种隔离域

例如：

```
Process A
    ├── Address Space A
    ├── File Descriptors A
    └── Credentials A

Process B
    ├── Address Space B
    ├── File Descriptors B
    └── Credentials B
```

它们实际上已经具有某种 Domain 的特征：

- 拥有各自的资源集合。
- 具有不同的访问权限。
- 由 Linux Kernel 统一管理。
- 通过 MMU、文件权限等机制实施隔离。

但是 Linux Process 的隔离不能抵抗恶意 Linux Kernel，因为 Kernel 本身拥有管理权。

这告诉我们：

一个 Domain 能抵抗谁，取决于谁管理它、谁实施隔离，以及攻击者能够控制什么。

### 2.2 Arm TrustZone 中的 Security State 也是安全域划分的一种方式

Arm 将系统划分为 Secure 和 Non-secure Security State。

可以从逻辑上把它们理解成两个安全域：

```
           EL3 Secure Monitor
                    |
          +---------+---------+
          |                   |
   Non-secure World      Secure World
          |                   |
       Linux               OP-TEE
```

不过这里 Arm 定义的是具有明确架构语义的 Security State，并不是采用 OpenSBI 的 `struct sbi_domain` 抽象。

因此不能得出「RISC-V Domain 就等于 Arm Security State」这样的结论。

### 2.3 Domain 和 Protection Mechanism 是两个独立维度

这是理解整个概念最重要的一点。

假设我们定义：

```
Domain A:
    Linux
    Memory: 0x80000000 - 0x8FFFFFFF

Domain B:
    OP-TEE
    Memory: 0x90000000 - 0x90FFFFFF
```

这只是一个资源与权限的逻辑定义。

要让定义真正生效，仍然需要具体的保护机制。

| Domain 需求          | 实施机制                               |
| ------------------ | ---------------------------------- |
| 限制 CPU 物理内存访问      | PMP / ePMP                         |
| 限制 User TA 的虚拟地址空间 | MMU                                |
| 限制 DMA 读取敏感内存      | IOPMP / 可信 IOMMU 策略 / SoC Firewall |
| 控制安全外设访问           | 外设安全控制器 / World ID                 |
| 管理 hart 的归属        | OpenSBI Domain/HSM 管理逻辑            |
| 切换不同 Domain        | 可信 Monitor + Context Switch        |

所以：

Domain 是要实现的隔离与资源管理模型；PMP、MMU 等是实施该模型的机制。

RISC-V Security Reference Model 也采用了类似的抽象定义：Domain 表示包含执行状态与资源的逻辑安全边界，某个 Domain 中的资源对至少某些其他 Domain 不可访问。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 三、回到 OpenSBI：Domain 在源码里究竟是什么？

这时我们就可以从抽象设计进入实际实现。

OpenSBI 官方将 Domain 定义为底层硬件的一种系统级分区，包含自己的内存区域、MMIO 设备和 Hart 集合。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



在代码中，核心对象是：

`struct sbi_domain`

你可以将它的关键内容理解成：

```
/* 教学用简化模型，不是完整的实际结构体定义 */

struct domain {
    const char *name;

    hartmask_t possible_harts;
    hartmask_t assigned_harts;

    struct memory_region *regions;

    unsigned long boot_hartid;
    unsigned long next_addr;
    unsigned long next_mode;

    bool system_reset_allowed;
    bool system_suspend_allowed;
};
```

关键在于它管理的是一个执行环境的资源与权限，不只是内存。

### 3.1 一个 Domain 不一定独占一个 Hart

假设有四个 Hart。

示例：Hart 与 Domain 的动态归属

切换前切换后

Hart 0

TEE

Hart 1

Linux

Hart 2

Linux

Hart 3

Linux

Hart 0 已切换到 OP-TEE Domain

Linux Domain

Hart 1, 2, 3

TEE Domain

Hart 0

交互式概念示例：展示同一个 Hart 在不同时刻可以属于不同 Domain。实际切换仍取决于 OpenSBI 对该 Hart 的允许集合和系统策略。

注意，这里有一个很重要的事实：

Domain 既不等于 Hart，也不等于一组固定的 CPU Core。

一个 Domain 可以拥有多个 Hart。一个 Hart 也可以在允许的情况下，先后运行不同 Domain 的执行上下文。

OpenSBI 的 `possible_harts` 与 `assigned_harts` 正是用于区分允许归属和当前归属；官方文档规定，每个 Hart 在任一时刻都归属于一个 OpenSBI Domain Context。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 3.2 Domain 切换时，到底变了什么？

以 Linux Domain → OP-TEE Domain 为例：

```
Linux S-mode
    |
    | TEE Request
    v
OpenSBI M-mode
    |
    +-- 保存 Linux 的执行上下文
    |
    +-- 切换当前 Hart 的 Domain 归属
    |
    +-- 恢复 OP-TEE 的执行上下文
    |
    +-- 配置 PMP / 平台安全状态
    |
    +-- 执行必要的同步操作
    |
    v
OP-TEE S-mode
```

最终变化的不是一个简单的 `domain_id`：

- 执行状态可能改变，例如 `sepc`、`satp`、`stvec`、`sscratch`。
- 当前 Hart 的权限可能改变，例如 PMP 配置。
- 某些平台还需要更新 World ID、外设访问策略等硬件状态。
- 相关资源的访问与管理策略随目标 Domain 改变。

这些操作的具体次序及原子性要求属于后续 Chapter 06 的内容。

你可以把 Domain Switch 理解为切换一个 Hart 当前所属的受保护执行环境。它比一般 Task Context Switch 多了跨安全边界的含义。

## 四、为什么 RISC-V 各处都会出现 Domain？

因为不同子系统都需要对自己管理的资源进行分组。

但它们划分资源的目的并不完全一样。

| 你遇到的 Domain            | 主要含义                        | 管理或实施机制                      |
| ---------------------- | --------------------------- | ---------------------------- |
| OpenSBI Domain         | 系统级的 Hart、Memory、MMIO 和权限分区 | M-mode OpenSBI               |
| Security Domain        | 具有特定信任边界的执行环境               | Security Manager + Hardware  |
| WorldGuard World       | 用 World ID 标识执行主体及访问策略      | World ID + Hardware Checker  |
| IOMMU Domain           | 一组设备共享的 DMA 地址空间与映射策略       | IOMMU / OS / Hypervisor      |
| Linux IRQ Domain       | 硬件中断编号到 Linux IRQ 编号的映射关系   | Linux IRQ Subsystem          |
| Power Domain           | 可以共同管理供电状态的硬件资源集合           | PM Controller                |
| Cache Coherency Domain | 参与特定缓存一致性协议的硬件主体范围          | Cache Coherency Interconnect |

需要特别区分三个概念。

1\. OpenSBI Domain 不等于 WorldGuard World。

OpenSBI Domain 是 Firmware 层面的系统分区抽象；WorldGuard 使用硬件 World ID 表达访问主体的安全上下文。某个平台可以使用 WorldGuard 来实施 OpenSBI Domain 的资源隔离，但两者并不天然一一对应。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



2\. IOMMU Domain 不等于 OpenSBI Domain。

IOMMU 主要关心设备的 DMA 地址空间。RISC-V IOMMU 规范通过 Device Context 和可选的 Process Context 管理设备地址转换。系统可以协调 IOMMU 与 OpenSBI Domain 的策略，但它们的对象和管理层级不同。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V 文档



3\. 名称包含 Domain，不代表它就是安全域。

例如 Power Domain 主要解决电源管理问题，而 Linux IRQ Domain 解决的是中断编号映射问题。它们都具有资源分组的思想，但本身不代表独立的安全边界。

这也是为什么你看到 Domain 这个词时，必须结合具体的子系统理解。

## 五、一个很容易产生的误解：ROOT Domain 是不是 M-mode？

不是。

OpenSBI 有一个默认的 `ROOT Domain`，但不能把它理解成 M-mode 本身。

在 OpenSBI 的实现中：

- ROOT Domain 是一个默认的 OpenSBI Domain 实例。
- M-mode 是 RISC-V Architecture 定义的 CPU 特权级。
- OpenSBI 运行在 M-mode，负责管理 Domain。
- ROOT Domain 最初可以包含全部 Hart，并具有广泛的 S/U 物理地址访问权限，但仍会保护 OpenSBI 自身的固件区域。

如果平台没有创建其他 Domain，OpenSBI 默认就可以让所有 Hart 使用 ROOT Domain。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此，你可以建立这样一个层次模型：

```
RISC-V Hardware
    |
    +-- M-mode
    |     |
    |     +-- OpenSBI
    |           |
    |           +-- Domain Manager
    |                 |
    |                 +-- ROOT Domain
    |                 |
    |                 +-- Linux Domain
    |                 |
    |                 +-- OP-TEE Domain
    |
    +-- S-mode / U-mode
          |
          +-- 当前 Hart 对应 Domain 的执行环境
```

这张图表达管理关系，不是说 ROOT Domain 必须始终作为 Linux 和 OP-TEE Domain 的父安全域。OpenSBI 实际的 Root Region Inheritance、Hart Assignment 和访问权限由 Domain 配置决定。

## 六、以后如何快速判断一个 Domain 的真实含义？

我建议你遇到任何一种 Domain，都用下面四个问题判断。

第一：它划分的是什么资源？

是 Hart、物理内存、虚拟地址空间、DMA Device、Interrupt，还是电源资源？

第二：谁管理 Domain？

是 Linux Kernel、Hypervisor、OpenSBI，还是硬件 Security Controller？

第三：谁实施隔离或管理规则？

是 MMU、PMP、IOMMU、WorldGuard，还是纯软件机制？

第四：攻击者突破某个组件后，还能否改变这个 Domain 的权限？

这是判断它是否能作为可信安全边界的核心。

例如，对于 OpenSBI Domain：

| 判断维度 | 答案                           |
| ---- | ---------------------------- |
| 资源   | Hart、RAM、MMIO、系统级操作权限        |
| 管理者  | OpenSBI M-mode Firmware      |
| 隔离机制 | PMP/ePMP 与平台级安全硬件            |
| 信任边界 | 正确配置时，可以限制已被攻破的 S-mode Linux |

而对于 Linux Process：

| 判断维度 | 答案                       |
| ---- | ------------------------ |
| 资源   | 虚拟地址空间、文件和内核对象           |
| 管理者  | Linux Kernel             |
| 隔离机制 | MMU 和 Linux 权限体系         |
| 信任边界 | 通常不能抵抗已被攻破的 Linux Kernel |

## 七、最后总结

我建议你以后将 Domain 理解成一个带有资源和权限规则的逻辑执行范围。

然后再记住三层关系：

```
         Domain
     定义隔离和资源边界
             |
             v
     Domain Manager
     决定资源分配和权限
             |
             v
     Enforcement Mechanism
     实际执行访问控制
```

对于你当前研究的 RISC-V OP-TEE，三者分别是：

- Domain：Linux Domain / OP-TEE Domain。
- Domain Manager：OpenSBI M-mode Firmware。
- Enforcement Mechanism：PMP + 平台级硬件安全机制。

最关键的认识是：Domain 并不是 RISC-V 独有的基础硬件概念，而是贯穿操作系统、虚拟化和安全架构的设计思想。OpenSBI 把这种思想具体落实成了一个可由 M-mode 管理的系统级分区模型。

理解了这一点，你后续阅读 `sbi_domain.c`、`sbi_domain_context.c`、PMP、WorldGuard，甚至 RISC-V IOMMU 时，就能够区分「定义资源边界的抽象」与「真正执行隔离的硬件机制」，不容易再把不同层次的 Domain 混淆。