# Chapter 04 — OpenSBI Domain Architecture

《RISC-V OP-TEE Architecture & Porting》· Part II：OpenSBI Domain & Hardware Isolation

本章目标：从设计理念、核心数据结构、源码实现和实际安全模型四个层面，彻底理解 OpenSBI Domain Architecture。

本章基于截至 2026 年 10 月的 OpenSBI upstream 代码与官方 Domain 文档展开，重点使用以下源码：

- `include/sbi/sbi_domain.h`
- `lib/sbi/sbi_domain.c`
- `lib/utils/fdt/fdt_domain.c`
- `lib/sbi/sbi_domain_context.c`
- `lib/sbi/sbi_init.c`

需要先明确：OpenSBI Domain 是 upstream 已有的基础机制；利用 Domain 构建完整 TEE 所需的跨 Domain 执行切换、中断路由、设备隔离等能力，则涉及额外实现。

这两部分不能混为一谈。

## 本章内容

| Section | 主题                     | 核心问题                                    |
| ------- | ---------------------- | --------------------------------------- |
| 4.1     | 为什么需要 OpenSBI Domain   | M/S/U 已有特权隔离，为什么还需要 Domain？             |
| 4.2     | `struct sbi_domain`    | Domain 在源码里究竟是什么？                       |
| 4.3     | Domain 与 Hart          | `possible_harts`、`assigned_harts` 的本质区别 |
| 4.4     | Domain Memory Region   | 物理地址范围、权限、重叠规则如何工作？                     |
| 4.5     | Domain 生命周期            | 创建、注册、初始化、启动、切换                         |
| 4.6     | Domain Device Tree     | 如何通过 DTS 配置多个 Domain？                   |
| 4.7     | ROOT / Linux / OP-TEE  | 三者究竟是什么关系？                              |
| 4.8     | Multi-Domain 与时间复用     | 多个 Domain 是否可以共享同一个 Hart？               |
| 4.9     | Domain 与 Hypervisor VM | 为什么 OpenSBI Domain 不是普通虚拟机？             |

最后还会给出端到端案例、源码阅读路线、动手实验和 Staff Engineer 级别的问题。

# 4.1 为什么 OpenSBI 需要 Domain？

## 4.1.1 从最初的 RISC-V 特权模型说起

我们先不考虑 OP-TEE。

一个典型的 RV64 Linux 系统：

```
+----------------------------------+
| Linux Applications               |
| U-mode                           |
+----------------------------------+
| Linux Kernel                     |
| S-mode                           |
+----------------------------------+
| OpenSBI                           |
| M-mode                           |
+----------------------------------+
| RISC-V Hardware                  |
+----------------------------------+
```

你已经熟悉这种模式。

其中：

- M-mode 负责 Machine-level Firmware。
- S-mode 负责操作系统。
- U-mode 负责用户态程序。

Linux 通过 SBI ECALL 请求 OpenSBI 提供 HSM、IPI、RFENCE、Timer 等底层服务。

例如：

```
# 在 S-mode 调用 SBI
li a7, 0x48534D    # HSM extension ID
li a6, 0           # hart_start function ID
ecall
```

随后 Trap 进入 M-mode，由 OpenSBI 处理请求。

在这样的系统里，有一个隐含假设：

整个系统通常只有一个主要的 S-mode OS，并由它管理自己的 U-mode 软件及大部分系统资源。

假设引入下面的需求：

```
RISC-V SoC
    |
    +-- Linux
    |
    +-- OP-TEE
```

我们希望：

1. Linux 运行在 S-mode。
2. OP-TEE 同样运行在 S-mode。
3. Linux 即使被完全攻破，也不能访问 OP-TEE 私有内存。
4. OP-TEE 能够通过受控方式访问一些硬件安全资源。
5. Linux 与 OP-TEE 能够通过某种通信机制协同工作。

问题来了：S-mode 和 S-mode 之间如何建立安全隔离？

RISC-V 的基本 M/S/U 模型并没有类似 Arm TrustZone 的 `SCR_EL3.NS` Security State。

因此，仅靠 M/S/U 级别的差异，不能直接构建两个互相隔离的 S-mode 世界。

## 4.1.2 为什么仅靠 MMU 不够？

假设：

```
Linux S-mode
    satp = Linux page table

OP-TEE S-mode
    satp = OP-TEE page table
```

你可能会提出：

只要 Linux 和 OP-TEE 使用不同的 `satp`，不就隔离了吗？

答案是：对于不可信的 Linux Kernel，这样不能形成可靠的安全边界。

因为 Linux 本身运行在 S-mode，它能够修改自身的页表，也能够修改 `satp`。

假设：

```
Linux physical RAM:
    0x80000000 - 0x8FFFFFFF

OP-TEE physical RAM:
    0x90000000 - 0x90FFFFFF
```

Linux Kernel 被攻破以后，攻击者可以修改 Linux 的页表：

```
Linux Virtual Address       Physical Address

0xFFFF000010000000    --->  0x90000000
```

如果没有其他硬件保护，Linux 的 S-mode 访问可以通过这个映射访问 OP-TEE 物理内存。

所以：

> MMU 可以隔离不同进程、不同地址空间，但由不可信 OS 控制的 MMU，不能单独隔离该 OS 自身。

这是本章的第一个核心结论。

## 4.1.3 引入 Domain 的根本动机

现在我们换一种思路。

不要让 Linux 自己决定哪些物理内存可以访问，而是由更高特权的 M-mode Firmware 来管理。

```
                 M-mode
          +--------------------+
          |      OpenSBI       |
          |                    |
          | Domain Management  |
          | Memory Policy      |
          | Hart Ownership     |
          +--------------------+
                    |
          Hardware Enforcement
                    |
       +------------+------------+
       |                         |
       v                         v
+---------------+         +---------------+
| Domain A      |         | Domain B      |
|               |         |               |
| Linux         |         | OP-TEE        |
| S-mode        |         | S-mode        |
|               |         |               |
| Memory A      |         | Memory B      |
+---------------+         +---------------+
```

设计思想是：

将硬件资源的控制权从单个 S-mode OS 提升到 M-mode Firmware，并将不同 S-mode 软件组织进具有不同资源权限的 Domain。

这就是 OpenSBI Domain 的主要动机。

OpenSBI 官方给出的定义是：

Domain 是一个系统级的硬件资源分区，包含该 Domain 可以使用的内存区域、MMIO 设备与 Hart。OpenSBI 通过 PMP、ePMP、IOPMP 或平台级隔离机制实现尽可能可靠的 Domain 间隔离。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里要注意：

Domain 本身是一个软件资源管理抽象。

安全隔离并不是因为存在 `struct sbi_domain` 就自动成立，而是因为这个抽象所定义的策略最终被可靠的硬件机制执行。

## 4.1.4 Domain 要解决的三个核心问题

我将其归纳为三个问题。

第一：Resource Ownership

某个 Domain 可以使用哪些硬件资源？

```
Domain Linux:
    RAM:    Linux RAM
    Device: UART0, Ethernet
    Hart:   hart0, hart1

Domain OP-TEE:
    RAM:    Secure RAM
    Device: Secure Crypto Engine
    Hart:   hart0, hart1
```

这里的 Hart 集合表示可能使用这些 Hart，不代表两个 Domain 一定同时获得所有 Hart。

第二：Resource Access Control

假设 Linux 访问：

```
volatile uint32_t *p = (void *)0x90000000;
uint32_t value = *p;
```

如果该地址是 OP-TEE 的私有物理内存，在正确实现并启用的 PMP/系统级隔离策略下，Linux 即使构造了一个有效的虚拟地址映射，硬件访问仍应失败。

第三：Execution Ownership

当前 hart 究竟处于哪个 Domain？

例如：

```
时间 t0：
    hart0 -> Linux Domain

时间 t1：
    hart0 -> OP-TEE Domain

时间 t2：
    hart0 -> Linux Domain
```

Domain Context Switch 要负责改变当前执行环境以及与其关联的隔离状态。

注意，这里已经进入另一个层面：

- Domain 定义了资源和权限。
- Domain Context 定义了特定 hart 上的执行状态。
- Domain Switch 负责从一个执行上下文切换到另一个执行上下文。

这三个概念不能等同。

## 4.1.5 为什么 OpenSBI Domain 不是另一个 Privilege Mode？

这是一个非常容易混淆的地方。

你应该区分：

| 概念             | 由谁定义             | 本质                      |
| -------------- | ---------------- | ----------------------- |
| M/S/U          | RISC-V ISA       | 特权等级                    |
| PMP            | RISC-V ISA 扩展机制  | 物理内存访问保护                |
| OpenSBI Domain | OpenSBI Firmware | 硬件资源与权限的逻辑分区            |
| Domain Context | OpenSBI Firmware | 每个 Domain 在 Hart 上的执行状态 |
| OP-TEE         | TEE OS           | 可信内核和安全服务执行环境           |

最关键的理解是：

Domain 没有创造一个新的 CPU Privilege Mode，而是在现有的 S/U-mode 之上，利用 M-mode 管理的硬件隔离策略创造不同的执行与资源边界。

# 4.2 `struct sbi_domain` 的设计思想与核心字段

这一节我们开始深入 OpenSBI 源码。

源码位置：

[include/sbi/sbi_domain.h](https://github.com/riscv-software-src/opensbi/blob/master/include/sbi/sbi_domain.h?utm_source=chatgpt.com)

## 4.2.1 从架构需求推导数据结构

假设你来设计 Domain Framework。

一个 Domain 至少应该描述：

- Domain 的身份与名称；
- 可运行在哪些 Hart 上；
- 当前拥有或分配了哪些 Hart；
- 可以访问哪些物理内存和外设；
- 从哪个入口地址启动；
- 使用什么 Privilege Mode 启动；
- 是否有权限复位或挂起整个系统。

这恰好也是 `struct sbi_domain` 的主要职责。

我把当前 upstream 的实际结构整理为下面的教学视图，保留主要成员，省略部分内部状态及注释：

```
/* Teaching excerpt: not a complete struct definition */

struct sbi_domain {
    struct sbi_dlist node;
    struct sbi_domain_state_priv state_priv;

    u32 index;
    spinlock_t assigned_harts_lock;
    struct sbi_hartmask assigned_harts;

    char name[64];
    u32 init_order;

    const struct sbi_hartmask *possible_harts;
    struct sbi_domain_memregion *regions;

    u32 boot_hartid;

    unsigned long next_arg1;
    unsigned long next_addr;
    unsigned long next_mode;

    bool system_reset_allowed;
    bool system_suspend_allowed;
    bool fw_region_inited;
};
```

这些成员来自当前 upstream 的 `sbi_domain.h`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 4.2.2 将结构体拆成五组

Identity & Management

`node`, `state_priv`, `index`, `name`, `init_order`

Domain 身份、列表管理、内部状态及初始化优先级。

CPU Ownership

`possible_harts`, `assigned_harts`, `assigned_harts_lock`

描述 Domain 能够在哪些 Hart 上运行，以及当前分配状态。

Memory Protection

`regions`, `fw_region_inited`

描述物理内存资源、权限与 Firmware Region 的初始化状态。

Boot Context

`boot_hartid`, `next_addr`, `next_mode`, `next_arg1`

描述 Domain 的启动入口和启动参数。

System Permissions

`system_reset_allowed`, `system_suspend_allowed`

限制 Domain 能否请求具有全局影响的系统操作。

这种分类非常重要，因为它揭示了 OpenSBI Domain 的设计理念：

它不仅仅描述内存隔离，也负责 CPU 归属、启动配置及部分系统级访问控制。

## 4.2.3 为什么需要 `init_order`？

假设两个 Domain 都希望使用 hart0：

```
Domain A:
    possible_harts = {hart0, hart1}
    init_order = 10

Domain B:
    possible_harts = {hart0, hart2}
    init_order = 20
```

那么初始分配时，谁优先获得 hart0？

在当前 OpenSBI 的注册实现中，较小的 `init_order` 具有更高的初始 Hart 分配优先级。

实际代码的核心判断是：

```
if (tdom->init_order > dom->init_order)
    sbi_hartmask_clear_hartindex(
        i, &tdom->assigned_harts);
else
    continue;
```

随后更新 Hart 对应的 Domain，并将其加入新的 `assigned_harts`。

源码位于 `sbi_domain_register()`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此，上面的例子里，hart0 最终将优先分配给 Domain A。

需要强调：

`init_order` 不是 Linux Scheduler 的任务优先级，也不是运行时调度 Domain 的优先级。

它主要参与 Domain 初始化与启动次序及初始资源归属。

## 4.2.4 `next_addr` 与 `next_mode` 的真实作用

你可能很自然地把这两个字段理解为 Linux Entry 或 OP-TEE Entry。

但是它们的设计是通用的。

```
unsigned long next_addr;
unsigned long next_mode;
unsigned long next_arg1;
```

它们表示：

- `next_addr`：Domain 下一阶段执行入口。
- `next_mode`：下一阶段使用的特权模式，通常是 S-mode 或 U-mode。
- `next_arg1`：传递给下一阶段的 `a1` 参数。

例如：

```
Linux Domain:
    next_addr = Linux/U-Boot entry
    next_mode = S-mode
    next_arg1 = Linux DTB address

OP-TEE Domain:
    next_addr = OP-TEE entry
    next_mode = S-mode
    next_arg1 = Platform-specific boot parameter
```

这里只是说明字段用途。OP-TEE 实际启动参数必须符合对应平台的 Boot ABI，不能直接假定和 Linux 相同。

## 4.2.5 Domain 结构中为什么没有直接保存全部 CSR？

这里有一个能够体现 OpenSBI 架构分层的关键问题。

`struct sbi_domain` 主要描述 Domain 的静态配置与管理状态。

而 `sepc`、`sstatus`、`satp`、`stvec` 等执行状态属于特定 hart 上的 Domain Context。

同一个 Domain 可能同时在多个 Hart 上运行，因此不能简单在全局 Domain 结构中放一份 CSR Context。

你应该在脑中建立：

```
struct sbi_domain
    |
    +-- Domain identity
    +-- Memory policy
    +-- Possible / assigned harts
    +-- Boot information
    |
    +-- Domain-associated state
            |
            +-- hart0 context
            +-- hart1 context
            +-- hart2 context
```

这是我们后续 Chapter 06 学习 `sbi_domain_context.c` 的基础。

# 4.3 Domain 与 Hart 的归属、运行和资源分配

这一节的重要程度不亚于 `struct sbi_domain` 本身。

## 4.3.1 `possible_harts` 与 `assigned_harts` 为什么要分开？

直接看例子。

一个 SoC 有 4 个 Hart：

```
hart0   hart1   hart2   hart3
```

定义：

```
Linux Domain
    possible_harts = {0,1,2,3}

OP-TEE Domain
    possible_harts = {0,1,2,3}
```

这意味着：

两个 Domain 理论上都支持在四个 Hart 上运行。

但不能据此认为：

```
Linux Domain owns hart0,1,2,3
OP-TEE Domain owns hart0,1,2,3
```

因为 `possible_harts` 表示允许的 Hart 集合，并不等于当前实际分配情况。

而 `assigned_harts` 是 OpenSBI 当前维护的 Hart 分配集合。

例如某一时刻：

```
Linux Domain
    assigned_harts = {0,1,2}

OP-TEE Domain
    assigned_harts = {3}
```

Domain 的 Hart 归属可以在允许范围内变化，但必须由可信的 OpenSBI 机制进行管理。

## 4.3.2 为什么 OpenSBI 需要 Hart → Domain Mapping？

一个很现实的场景：

Linux 在 hart0 上执行：

```
sbi_remote_fence_i(...);
```

OpenSBI 怎么知道 Linux 被允许对哪些 Hart 发起 Remote Fence？

如果没有 Domain：

```
Linux
    -> SBI RFENCE
        -> potentially other harts
```

引入 Domain 后，OpenSBI 必须检查调用来源和目标 Hart 是否符合 Domain 的管理边界。

否则，一个不可信 Domain 可能借助高权限 SBI 服务干扰其他 Domain。

所以 OpenSBI 需要支持：

```
struct sbi_domain *
sbi_hartindex_to_domain(u32 hartindex);
```

以及：

```
#define sbi_domain_thishart_ptr() \
    sbi_hartindex_to_domain(current_hartindex())
```

当前源码确实通过 Hart Index 到 Domain 的映射，获取当前 Hart 所属的 Domain。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



它是 SBI 服务实施 Domain-aware 访问控制的重要基础。

## 4.3.3 Domain 不仅影响内存，也影响 SBI 服务

官方文档明确列出了 Domain 对 SBI 调用的影响：

| SBI 服务         | Domain-aware 行为             |
| -------------- | --------------------------- |
| IPI            | 限制目标 Hart 的可访问范围            |
| RFENCE         | 限制跨 Hart Remote Fence 的目标范围 |
| HSM            | 限制对其他 Domain Hart 状态的操作     |
| System Reset   | 根据 Domain 权限控制是否允许          |
| System Suspend | 根据 Domain 权限控制是否允许          |

这些限制不是 CPU ISA 自动提供的，而是 OpenSBI Firmware 的访问控制逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



举一个例子：

```
Linux Domain:
    assigned_harts = {0,1}

OP-TEE Domain:
    assigned_harts = {2,3}
```

Linux 在 hart0 上请求：

```
SBI HSM Hart Stop/Status for hart3
```

如果 OpenSBI 不检查 Domain，Linux 就可能干扰 OP-TEE 所运行的 Hart。

因此，OpenSBI 不仅要隔离地址空间，还必须隔离系统管理能力。

这也解释了为什么 Domain 是 System-Level Partition，而不是一个纯粹的内存权限表。

## 4.3.4 为什么需要 `assigned_harts_lock`？

观察：

```
spinlock_t assigned_harts_lock;
struct sbi_hartmask assigned_harts;
```

这里有一个典型 SMP 问题：

`assigned_harts` 是共享状态，多个 Hart 可能并发访问或修改。

例如：

```
hart0:
    Domain A -> Domain B

hart1:
    查询 Domain A 的 assigned_harts
```

如果没有同步保护，读者可能看到尚未完成的集合修改。

因此 OpenSBI 使用 Spinlock 维护这组共享数据。

但这里需要更深入理解：

保护 Hart Mask 的锁，不等于自动保证整个 Domain Switch 原子、安全。

完整的 Domain Switch 还涉及：

- Hart-local Domain Context；
- CPU 寄存器；
- PMP / 硬件隔离配置；
- Interrupt State；
- 目标 Domain Context；
- 共享数据与状态转换顺序。

这正是 Chapter 06、Chapter 07 要解决的问题。

# 4.4 Domain Memory Region：物理内存描述与权限模型

这是 Chapter 04 技术深度最高的部分之一。

理解这一节之后，后续学习 PMP 时，你就能清楚地知道：

PMP 是硬件实现机制，而 OpenSBI Domain Memory Region 是定义隔离策略的软件抽象。

## 4.4.1 `struct sbi_domain_memregion`

源码：

`include/sbi/sbi_domain.h`

其核心结构非常简单：

```
struct sbi_domain_memregion {
    unsigned long order;
    unsigned long base;
    unsigned long flags;
};
```

它由三个信息描述一个物理地址区域。

| 字段      | 作用                      |
| ------- | ----------------------- |
| `base`  | 物理地址起始位置                |
| `order` | 区域大小，等于 \\(2^{order}\\) |
| `flags` | 内存类型及 M/S/U 访问权限        |

例如：

```
base  = 0x90000000
order = 24

size  = 2^24 = 16 MiB

Physical Region:
    0x90000000 - 0x90FFFFFF
```

为什么选择 `order` 而不是任意 `size`？

因为它能够方便地描述与 PMP NAPOT 模型相容的 2 的幂次方大小、按区域大小对齐的物理地址范围。

例如：

```
base  = 0x90000000
order = 24
```

符合对齐要求。

但：

```
base  = 0x90001000
order = 24
```

不符合 16 MiB 对齐要求。

OpenSBI 也提供 `sbi_domain_memregion_init()`，用地址、大小和标志构造 Region；更大的非规则地址范围可以根据需要拆分成多个符合要求的区域。

## 4.4.2 Region Permission 的编码

当前 `sbi_domain.h` 定义了 M-mode 和 S/U-mode 权限位。

| Bit | 宏对应权限             | 意义               |
| --- | ----------------- | ---------------- |
| 0   | `M_READABLE`      | M-mode Read      |
| 1   | `M_WRITABLE`      | M-mode Write     |
| 2   | `M_EXECUTABLE`    | M-mode Execute   |
| 3   | `SU_READABLE`     | S/U-mode Read    |
| 4   | `SU_WRITABLE`     | S/U-mode Write   |
| 5   | `SU_EXECUTABLE`   | S/U-mode Execute |
| 6   | `ENF_PERMISSIONS` | 要求强制实施相应 PMP 权限  |

这些位属于 OpenSBI 软件策略表示，不是 `pmpcfg` 的直接位布局。实际写入 PMP 时需要根据标准 PMP 或 Smepmp 的语义进行转换。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



例如：

```
flags = 0x3F

Binary:
    0011 1111
```

表示六个 M/SU R/W/X 权限位全部置位。

再例如：

```
flags = 0x18

Binary:
    0001 1000
```

表示 S/U-mode Read + Write，没有 S/U-mode Execute 权限，也没有显式设置 M-mode 访问位。

这里有一个容易忽略的细节：

在没有设置 PMP Lock/Enforcement 的普通 PMP 模型下，M-mode 的访问行为与 S/U-mode 并不相同。

未锁定的 PMP Entry 通常不限制 M-mode 的访问。因此，不能仅从 `flags` 的 M 权限位推断物理访问的全部结果，还必须结合具体 PMP 实现、Lock 状态和 Smepmp 配置。

有关这些细节将在 Chapter 05 进一步展开。

## 4.4.3 为什么 S 和 U 共用一组 Domain Permission？

这也是一个非常重要的设计点。

你可能会问：

为什么只有：

```
M permissions
SU permissions
```

而不是：

```
M permissions
S permissions
U permissions
```

因为 Domain Memory Region 的核心职责是限制某个 Supervisor Domain 可以访问的物理资源。

它不是用来取代 S-mode 页表的。

在一个 OP-TEE Domain 中：

```
OP-TEE Core:
    S-mode

User TA:
    U-mode
```

Domain-level PMP 负责限制该 Domain 整体可以访问的物理地址。

而 OP-TEE 内核进一步通过 U/S 权限和页表机制隔离：

- OP-TEE Core；
- User TA A；
- User TA B。

因此存在两层不同的隔离：

```
Layer 1: Domain Isolation
    Linux Domain ↔ OP-TEE Domain
    Enforced by PMP / system security hardware

Layer 2: TA Isolation
    OP-TEE Core ↔ TA A ↔ TA B
    Enforced by S/U privilege and MMU
```

这种分层与 Arm TrustZone 的 Security State + Secure EL1/EL0 隔离理念有相似之处，但底层硬件实现并不相同。

## 4.4.4 一个实际例子：如何保护 OP-TEE 内存？

假设系统物理内存如下：

简化 SoC Physical Memory Map

Linux RAM

256 MiB

0x80000000 – 0x8FFFFFFF

OP-TEE Secure RAM

16 MiB

0x90000000 – 0x90FFFFFF

其他 MMIO / Memory

Platform-dependent

按设备和平台策略配置

我们希望：

| 访问来源           | Linux RAM      | OP-TEE RAM     |
| -------------- | -------------- | -------------- |
| Linux S-mode   | R/W/X，按页表进一步限制 | Deny           |
| OP-TEE S-mode  | 默认 Deny，按需共享   | R/W/X，按页表进一步限制 |
| OpenSBI M-mode | 由 M-mode 策略决定  | 由 M-mode 策略决定  |

为什么不能直接：

```
Linux Domain:
    Only allow Linux RAM

OP-TEE Domain:
    Only allow OP-TEE RAM
```

当然可以这样设计。

但真实系统还需要 MMIO、OpenSBI Firmware、共享内存、启动参数及可能的设备驱动支持。

此外，OpenSBI 为了兼容不同平台，还支持以较大范围作为默认规则、以小区域覆盖默认规则的设计。

这就引出了 Region Overlap。

## 4.4.5 Region Overlap：为什么允许嵌套区域？

假设 Linux 的 Region 设计为：

```
Region A:
    base  = 0x00000000
    order = 64
    flags = allow SU access

Region B:
    base  = 0x90000000
    order = 24
    flags = deny SU access
```

Region B 完全位于 Region A 内。

它表达的策略是：

```
全地址空间默认允许
    |
    +-- OP-TEE RAM 例外：禁止访问
```

因此，对一个地址进行权限查询：

```
Address = 0x80001000

    Region B? No
    Region A? Yes

    => Allow
```

而：

```
Address = 0x90001000

    Region B? Yes

    => Deny
```

这就是 Most Specific Region Wins，即优先使用更小、匹配更具体的 Region。

OpenSBI 对 Region 有明确约束：

- 相互重叠的区域必须是包含关系，不能只是部分交叉；
- 相同大小的重叠区域不允许互相冲突；
- 需要维护 OpenSBI Firmware 的保护区域；
- 更小的匹配 Region 应优先决定权限。

当前 `sanitize_domain()` 会检查 Region 并整理顺序，`sbi_domain_check_addr()` 则按排序后的区域查找适用权限。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



但是一定要区分：

软件权限查询的结果，不自动等价于硬件 PMP 的实际结果。

PMP 的 Entry Priority、Entry 数量、地址匹配方法及 Lock 状态，都可能影响某个策略能否正确实现。

特别是 PMP Entry 数量有限时，一个合法的软件 Region 集合也未必能够直接映射为可用的硬件保护配置。

## 4.4.6 软件权限检查到底是什么作用？

OpenSBI 提供：

```
bool sbi_domain_check_addr(
    const struct sbi_domain *dom,
    unsigned long addr,
    unsigned long mode,
    unsigned long access_flags);
```

假设：

```
allowed = sbi_domain_check_addr(
    linux_domain,
    0x90000000,
    PRV_S,
    SBI_DOMAIN_READ);
```

对于前面配置的 Linux Domain，预期是返回 `false`。

这个函数完成的是：

```
Domain Policy
    |
    v
Memory Region Lookup
    |
    v
Privilege + Access Type Check
    |
    v
Allow / Deny
```

例如 OpenSBI 某项 SBI 服务接收到 S-mode 提供的物理地址时，可以使用这种检查，判断该 Domain 对相关地址是否具有所需访问权限。

但即使这里返回 `false`，如果硬件 PMP 或其他隔离机制配置错误，Linux 仍可能绕过 OpenSBI 服务直接发起物理内存访问。

所以正确的系统应当同时具备：

1. Software Policy Validation：OpenSBI 检查请求是否符合 Domain 权限。
2. Hardware Enforcement：硬件独立限制 S/U-mode 实际能访问的物理地址。

前者不是后者的替代品。

这也解释了为什么 Domain Framework 和 PMP Driver 是两个不同层次的组件。

# 4.5 Domain 的创建、注册、初始化与生命周期

现在我们从静态数据结构进入动态执行过程。

## 4.5.1 OpenSBI Domain 的生命周期

你可以先记住如下状态链：

Early OpenSBI Initialization

建立 ROOT Domain、Per-Hart 基础状态

Domain Configuration Parsing

解析 FDT、构造 Domain 及 Region

Domain Registration

Sanity Check、排序、Hart Assignment、State Setup

Domain Finalization & Startup

关闭注册阶段、启动各 Domain 的 Boot Hart

Runtime

Domain Context Execution、SBI、Interrupt、Switch

这里的 Runtime 并不意味着 Domain 必须不断切换。静态分配 Hart 的 Domain 可以长期运行在各自的 Hart 上。

## 4.5.2 第一步：创建 ROOT Domain

在 OpenSBI 初始化的早期阶段，Domain Framework 会创建 ROOT Domain。

关键入口：

```
int sbi_domain_init(
    struct sbi_scratch *scratch,
    u32 cold_hartid);
```

ROOT Domain 有以下特性：

```
index = 0
name  = "root"

possible_harts:
    All valid HARTs

initial assigned_harts:
    All valid HARTs
```

同时建立 ROOT Domain 的 Firmware Protection Region 和基础内存权限配置。

源码中还会初始化 Domain Context 支持、分配 Hart-local Domain 指针存储区域以及 ROOT 的 Region 数组。

这一点在 `sbi_domain_init()` 中可以直接看到。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里 ROOT Domain 的作用首先是为系统建立一个默认运行与资源管理环境。

它并不等于安全意义上的 Root of Trust，也不能把 `root` 理解成 Linux 的 root 用户。

## 4.5.3 第二步：发现并注册 Domain

在当前 Generic FDT Platform 中，Domain 可以通过 Device Tree 描述。

其主要代码路径是：

```
OpenSBI initialization
    |
    v
sbi_domain_finalize()
    |
    v
sbi_platform_domains_init()
    |
    v
Platform-specific Domain Initialization
    |
    v
fdt_domains_populate()
    |
    v
__fdt_parse_domain()
    |
    v
sbi_domain_register()
```

这里需要注意实际执行顺序：

`sbi_domain_init()` 首先创建 ROOT Domain；平台特定 Domain 的解析与注册发生在后续 `sbi_domain_finalize()` 阶段，而不是把所有工作都放在初始的 `sbi_domain_init()` 中。

当前源码确实在 `sbi_domain_finalize()` 中调用平台的 Domain 初始化入口，并在完成后设置 `domain_finalized`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 4.5.4 第三步：`sbi_domain_register()` 做了什么？

核心源码：

[sbi_domain.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain.c?utm_source=chatgpt.com)

概念上可以表示为：

```
/* Simplified conceptual flow, not literal source */

int register_domain(struct sbi_domain *dom)
{
    if (domain_configuration_finalized())
        return ERROR;

    if (duplicate_domain_or_init_order(dom))
        return ERROR;

    if (!validate_and_sort_regions(dom))
        return ERROR;

    add_domain_to_global_list(dom);

    initialize_assigned_hartmask(dom);

    assign_eligible_harts(dom);

    initialize_domain_state(dom);

    return SUCCESS;
}
```

实际 `sbi_domain_register()` 还涉及错误回退等逻辑。这里用伪代码突出设计顺序。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



其中 `sanitize_domain()` 是一个很值得阅读的函数。

它不仅检查 `possible_harts` 是否有效、Memory Region 是否合理，还会对 Region 进行排序、消除兼容且被覆盖的冗余区域，检查 Firmware Protection Region 和 Boot Configuration。

你可以把它理解为：

OpenSBI 在接受一个 Domain 描述之前，对资源和权限配置执行结构性验证。

但它不能代替平台级威胁建模，也不能证明所有硬件隔离机制都正确启用。

## 4.5.5 第四步：Finalize

`domain_finalized` 是一个很有意思的设计。

在初始化阶段，OpenSBI 可以持续建立和补充 Domain Configuration。

但到某一个时间点，它需要结束这个配置阶段。

此后不能再通过正常的启动期注册路径任意增加 Domain 或修改 ROOT Region。

这有两个目的：

一是减少启动后资源布局被意外更改的可能性。

二是让后续 Hardware Protection Configuration 面对一个相对稳定的初始策略集合。

不过：

Finalization 不代表运行时 Domain Context 永远不能变化。

它限制的是启动期 Domain 定义和注册流程。运行时仍可以在许可范围内切换 Hart 所属 Domain，以及保存恢复执行上下文。

## 4.5.6 第五步：Startup

接下来：

```
sbi_domain_startup(scratch, cold_hartid);
```

对每一个 Domain，OpenSBI 会检查：

- `boot_hartid` 是否有效；
- 该 Hart 是否属于 `possible_harts`；
- 该 Hart 是否实际分配给当前 Domain。

符合条件后：

如果 Boot Hart 就是 Coldboot Hart，OpenSBI 可以更新 `sbi_scratch` 中的下一阶段启动信息。

如果是其他 Hart，则通过 HSM 的启动流程启动相应 Hart。

这一过程可以在 `sbi_domain_startup()` 中直接看到。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有一个非常重要的细节：

创建并注册 Domain，不保证它会在系统启动时立即执行。

例如：

```
Domain A:
    possible_harts = {0,1}
    assigned_harts = {0,1}

Domain B:
    possible_harts = {0,1}
    assigned_harts = {}
```

虽然 Domain B 已经注册，但它没有获得自己的 Boot Hart，因此不会仅因为存在 Domain Configuration 就自动启动。

它能否在后续通过 Context Switch 首次进入，还取决于相应机制的实现。

## 4.5.7 从创建过程推导一个重要结论

到此，我们应该区分：

| 操作          | 本质                            |
| ----------- | ----------------------------- |
| Create      | 构造一个 Domain 数据结构              |
| Register    | 将其加入 OpenSBI Domain Framework |
| Assign Hart | 建立 Hart 当前归属关系                |
| Startup     | 启动 Domain 的初始软件               |
| Enter/Exit  | 运行时进行 Domain Context 切换       |
| Finalize    | 结束启动期 Domain 配置和注册阶段          |

这是 OpenSBI Domain Lifecycle 的核心。

尤其需要避免把 `sbi_domain_register()` 理解为 Linux 的 `fork()` 或创建一个普通 Task。

注册一个 Domain 不会自动为它创建类似 Linux Process 的完整运行上下文。

# 4.6 Domain Device Tree 配置及解析

这一节会结合你已经学习过的 Device Tree 设计理念，深入理解 OpenSBI 如何通过 FDT 描述硬件资源分区。

## 4.6.1 为什么 Domain 适合使用 Device Tree？

Device Tree 的主要设计理念之一是：

将硬件描述与通用软件逻辑分离。

假设我们要支持两个 SoC：

```
SoC A:
    2 HART
    Secure RAM = 16 MiB
    PMP-based isolation

SoC B:
    8 HART
    Secure RAM = 64 MiB
    PMP + platform firewall
```

如果所有信息都硬编码在 OpenSBI 中，就会产生大量 Platform-specific 分支。

通过 Device Tree：

```
Hardware Configuration
        |
        v
Domain Description
        |
        v
Generic OpenSBI Domain Framework
```

可以保留通用 Domain 初始化和管理代码，让不同平台提供不同的资源描述。

当然，Device Tree 可以描述策略，不代表其中任意数据都可信。TEE 场景中的 DTB 本身必须来自可信的启动路径，并且需要确保攻击者无法在 OpenSBI 消费策略之前篡改它。

## 4.6.2 OpenSBI Domain 的 Device Tree Schema

官方定义了三类关键节点：

| 节点类型            | Compatible                 | 作用                       |
| --------------- | -------------------------- | ------------------------ |
| Domain Config   | `opensbi,domain,config`    | Domain 配置容器              |
| Memory Region   | `opensbi,domain,memregion` | 定义物理资源范围                 |
| Domain Instance | `opensbi,domain,instance`  | 定义 Domain 的 Hart、权限和启动信息 |

推荐将 Domain 配置放在 `/chosen/opensbi-domains` 下。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 4.6.3 一个双 Domain DTS 示例

我们构造一个教学用 SoC：

```
hart0 -> Linux Domain
hart1 -> OP-TEE Domain

Linux:
    0x80000000 - 0x8FFFFFFF

OP-TEE:
    0x90000000 - 0x90FFFFFF
```

假定对应镜像已经由可信启动阶段加载到指定地址。

以下是 Device Tree 的核心片段，可作为现有 SoC DTS 的扩展部分；`cpu0`、`cpu1` 假定已经在 `/cpus` 中定义。

```
/ {
    chosen {
        opensbi-domains {
            compatible = "opensbi,domain,config";

            tee_ram: tee-ram {
                compatible = "opensbi,domain,memregion";
                base = <0x0 0x90000000>;
                order = <24>;
            };

            allmem: all-memory {
                compatible = "opensbi,domain,memregion";
                base = <0x0 0x0>;
                order = <64>;
            };

            linux_domain: linux-domain {
                compatible = "opensbi,domain,instance";

                init-order = <10>;
                possible-harts = <&cpu0>;
                boot-hart = <&cpu0>;

                root-regions-inheritance = "m-only";

                regions =
                    <&tee_ram 0x00>,
                    <&allmem  0x3f>;

                next-addr = <0x0 0x80200000>;
                next-mode = <1>;
                next-arg1 = <0x0 0x88000000>;

                system-reset-allowed;
            };

            tee_domain: tee-domain {
                compatible = "opensbi,domain,instance";

                init-order = <20>;
                possible-harts = <&cpu1>;
                boot-hart = <&cpu1>;

                root-regions-inheritance = "m-only";

                regions = <&tee_ram 0x3f>;

                next-addr = <0x0 0x90000000>;
                next-mode = <1>;
                next-arg1 = <0x0 0x91000000>;
            };
        };
    };
};
```

这是一段演示 Domain Schema 的示例，而不是能够直接启动实际 OP-TEE 的完整生产 DTS。 特别是启动入口、DTB 地址、外设、MMIO、共享内存和 OP-TEE Boot ABI，需要按平台补齐；Linux 和 OP-TEE 的入口地址也不能仅由本例证明正确。

这段示例中值得关注的是：

`root-regions-inheritance = "m-only"`

这个配置表示继承 ROOT Domain 的 M-mode-only/Firmware 保护区域，而不是无差别继承 ROOT Domain 的全部 S/U 权限。

当前 upstream 的 FDT Parser 默认就采用 `m-only` 继承模式，同时也提供 `all` 模式。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 4.6.4 逐项解释这段 DTS

首先：

```
tee_ram: tee-ram {
    compatible = "opensbi,domain,memregion";
    base = <0x0 0x90000000>;
    order = <24>;
};
```

表示建立一个 16 MiB 的物理内存 Region。

这里的 `tee_ram` 是 Device Tree Label。

它不是 Domain 名称，而是可以被其他节点引用的 Memory Region。

随后：

```
regions =
    <&tee_ram 0x00>,
    <&allmem  0x3f>;
```

这里的 `regions` 不是普通地址列表，而是由：

```
<Memory Region Phandle, Permission Flags>
```

组成的条目集合。

对于 Linux：

```
tee_ram:
    SU Permission = Deny

allmem:
    SU Permission = RWX
```

由于更小的 `tee_ram` 优先匹配，Linux 对 Secure RAM 应被禁止访问。

再看：

```
regions = <&tee_ram 0x3f>;
```

表示 TEE Domain 具有该 Region 的相应访问权限。

对于未明确允许的其他地址，不能仅凭 Linux Domain 的权限推断 TEE Domain 也有权限。

实际配置中还需要为 OP-TEE 加入必要的启动资源、安全设备、共享内存等区域。

## 4.6.5 `next-addr` 和 `next-arg1` 为什么是两个不同的属性？

考虑：

```
next-addr = <0x0 0x80200000>;
next-arg1 = <0x0 0x88000000>;
```

在典型 RV64 Linux Boot Protocol 中：

```
a0 = Hart ID
a1 = Device Tree Address
PC = Kernel Entry
```

于是：

```
PC = 0x80200000
a1 = 0x88000000
```

OpenSBI 通过 Domain Boot Configuration 知道下一阶段在哪里，以及需要传递什么参数。

但是对 OP-TEE 来说，不能自动假设 `a1` 也是同样含义。

它需要遵循 OP-TEE 在当前 RISC-V 平台上的实际 Boot ABI。

## 4.6.6 为什么应该给不同 Domain 准备不同的 DTB？

假设完整的 SoC DTB 包含：

```
CPU
RAM
Secure RAM
UART
Crypto Engine
Network
Domain Configuration
```

如果 Linux 可以看到完整的 SoC 拓扑，它可能发现一些本不应该由它访问的安全设备。

因此建议：

```
Master / Trusted DTB
        |
        +-- Linux DTB
        |     Linux-visible resources
        |
        +-- OP-TEE DTB
              Secure resources
```

但要注意，隐藏 Device Tree 节点本身不是安全保护。

它只是帮助 Domain 内的软件理解可使用的资源，避免误配置。

真正的安全性仍由硬件权限控制保证。

官方文档同样建议优先为 Domain 提供独立 DTB，或者在传递给下一阶段之前去除 Domain 私有配置。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 4.6.7 Device Tree 是如何被 OpenSBI 转换为 Domain 的？

核心源码：

[lib/utils/fdt/fdt_domain.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/utils/fdt/fdt_domain.c?utm_source=chatgpt.com)

重要执行链：

```
fdt_domains_populate()
        |
        v
fdt_iterate_each_domain_ro()
        |
        v
__fdt_parse_domain()
        |
        +-- Allocate struct sbi_domain
        |
        +-- Parse name / init-order
        |
        +-- Parse possible-harts
        |
        +-- Parse memory regions
        |
        +-- Inherit ROOT memory regions
        |
        +-- Parse boot-hart
        |
        +-- Parse next-addr / next-mode
        |
        +-- Parse system permissions
        |
        v
sbi_domain_register()
```

`__fdt_parse_domain()` 在当前 upstream 中完成了上述解析步骤。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



可以看到 Device Tree 的作用是向 OpenSBI 提供配置数据，而具体的结构验证、Domain 注册和资源归属管理仍由 OpenSBI 的核心框架负责。

## 4.6.8 如何调试 Domain Device Tree？

这是一个很实用的工程方法。

假如你有完整的 `board.dtb`：

```
sudo apt install device-tree-compiler

dtc -I dtb -O dts \
    -o board.dump.dts board.dtb
```

然后检查：

```
grep -n -A100 'opensbi-domains' board.dump.dts
```

建议你重点核对：

- `possible-harts` 是否对应正确的 CPU phandle；
- `boot-hart` 是否包含在 `possible-harts` 中；
- 每个 Domain 的 `init-order` 是否唯一；
- Region 的 `base` 是否满足 \\(2^{order}\\) 对齐；
- Linux 是否被明确禁止访问 Secure RAM；
- Firmware Region 是否保留；
- `next-addr` 是否位于允许执行的区域内；
- 是否错误地将完整敏感 DTB 暴露给 Linux。

在 OpenSBI 启动日志中，还可以观察 `sbi_domain_dump_all()` 输出的 Domain 信息，用来核对解析后的运行时结果，而不是仅仅相信 DTS 源文件。

# 4.7 ROOT Domain、Linux Domain 与 OP-TEE Domain 的关系

这是本章需要特别澄清的一个概念。

首先，ROOT Domain、Linux Domain 和 OP-TEE Domain 并不是三个不同的 RISC-V 特权等级。

从 OpenSBI 的视角看，它们都是 `struct sbi_domain` 管理的对象，但承担不同的角色。

## 4.7.1 ROOT Domain 到底是什么？

根据 OpenSBI 官方定义，ROOT Domain 是系统初始化时创建的默认 Domain。

典型特点：

```
ROOT Domain

    index = 0
    name = "root"

    possible_harts = all valid harts

    initially assigned_harts = all valid harts

    memory:
        OpenSBI firmware protection
        default broad-access region

    system_reset_allowed = true
    system_suspend_allowed = true
```

ROOT Domain 的 `init_order` 为 `0xffffffff`，因此在初始分配竞争中，显式定义且拥有更小 `init_order` 的 Domain 能优先获得符合条件的 Hart。

这里特别需要注意：

ROOT Domain 不等于 M-mode，也不等于 Hardware Root of Trust。

ROOT Domain 只是 OpenSBI 的默认 Domain 实例。

例如，ROOT Domain 的下一阶段软件也可以运行在 S-mode。

即：

```
M-mode:
    OpenSBI

S-mode:
    ROOT Domain's next-stage software
```

这与：

```
M-mode:
    ROOT Domain
```

完全不同。

后者是错误理解。

## 4.7.2 ROOT Domain 是不是一定运行 Linux？

不是。

在传统没有配置其他 Domain 的系统中，ROOT Domain 通常承载 U-Boot/Linux 等下一阶段软件。

但是建立多个 Domain 后，可以有以下结构：

```
                    OpenSBI
                     M-mode
                        |
              Domain Management
                        |
          +-------------+-------------+
          |             |             |
       ROOT Domain   Domain 1      Domain 2
          |             |             |
       Fallback        Linux        OP-TEE
                      S-mode        S-mode
```

这里 ROOT Domain 主要承担默认归属和兼容性角色，而 Linux、OP-TEE 可以分别归属于单独的 Domain。

具体哪些 Domain 会启动，取决于其 Boot Hart 和初始 Assignment，而不是由 Domain 名称决定。

## 4.7.3 ROOT Domain 能不能访问 OP-TEE 内存？

这个问题不能用简单的是或否回答。

因为：

ROOT Domain 的实际访问权限由它的 Region Policy 和硬件隔离配置决定。

默认 ROOT Domain 具有广泛的 S/U 物理地址访问范围，同时保留 OpenSBI Firmware 保护规则。

因此，如果系统在创建可信 Domain 后，仍让一个不可信 Linux 长期运行在拥有宽泛权限的 ROOT Domain，并且没有额外限制 OP-TEE 私有内存，那么隔离可能是不成立的。

在设计真实的 TEE 系统时，应当检查：

1. Linux 当前实际上属于哪个 Domain？
2. Linux Domain 是否继承了 ROOT 的宽泛 S/U 权限？
3. OP-TEE 私有内存是否受到显式保护？
4. 安全硬件是否真正实施了相应访问控制？
5. 是否存在未被隔离的 DMA Master？

这一点也体现了：

> 一个 Domain 被称为 Trusted 或 Secure，并不自动意味着它安全；安全性来自正确的资源授权、不可绕过的硬件隔离以及可信的软件生命周期。

## 4.7.4 为什么 OP-TEE Domain 通常也运行在 S-mode？

因为 OP-TEE 本身是一个具备内核能力的 TEE OS。

在一个采用 Supervisor Domain 模型的 RV64 系统中，可以设计为：

```
                OpenSBI
                 M-mode
                    |
          +---------+---------+
          |                   |
      Linux Domain       TEE Domain
          |                   |
      Linux Kernel        OP-TEE Core
        S-mode              S-mode
          |                   |
      Linux Apps           User TA
        U-mode              U-mode
```

这个模型有一个很值得注意的设计思想：

M-mode 负责强制执行 Domain 间的系统级安全隔离，而 Domain 内部由各自的 S-mode Kernel 进一步管理 U-mode 资源。

因此：

- OpenSBI 不需要管理每个 Linux 进程的页表。
- OpenSBI 也不需要管理每个 TA 的虚拟地址空间。
- OP-TEE 自己需要处理 TA 的创建、执行、Syscall 和内存隔离。

这形成了两层不同的权限管理。

第一层：

```
OpenSBI -> Domain Isolation
```

第二层：

```
OP-TEE -> TA Isolation
```

如果 Linux 被攻破，攻击者可以控制 Linux 的页表，但仍应该受到 Domain-level 的硬件物理访问限制。

如果某个 User TA 被攻破，则 OP-TEE Core 依靠 S/U 权限和页表机制限制它对内核和其他 TA 的访问。

# 4.8 多 Domain 与同一 Hart 时间复用的区别

这一节对 RISC-V OP-TEE 的实际运行模型非常关键。

我们需要研究两个完全不同的场景。

## 4.8.1 模式 A：Spatial Partitioning

即空间分区。

例如：

```
4-HART SoC

             OpenSBI
                |
       +--------+--------+
       |                 |
   Linux Domain      TEE Domain
       |                 |
   hart0, hart1      hart2, hart3
```

在这个架构中：

```
Linux Domain:
    possible_harts = {0,1}
    assigned_harts = {0,1}

TEE Domain:
    possible_harts = {2,3}
    assigned_harts = {2,3}
```

两个 Domain 使用独立的 Hart 集合。

优点：

- 不需要为了运行两个 Domain 而频繁切换 Hart；
- 执行上下文切换开销较小；
- 对实时性和确定性可能更有利；
- 简化了部分 CPU 状态隔离问题。

缺点：

- 需要预留 Hart 资源；
- OP-TEE 空闲时，其专用 Hart 可能未被充分利用；
- 资源利用率可能低于共享 Hart 的方案。

这类架构适用于具有相对固定安全服务负载的系统，但并不是所有 OP-TEE SoC 都适合采用它。

## 4.8.2 模式 B：Temporal Partitioning

即时间复用。

这是 RISC-V OP-TEE 更值得深入研究的一种模型。

假设：

```
Linux Domain:
    possible_harts = {0,1}

TEE Domain:
    possible_harts = {0,1}
```

系统可以在不同时间复用同一个 Hart：

同一个 Hart 的时间复用示意

M 表示 M-mode Firmware 介入切换过程；时间宽度不代表真实执行耗时。

在这个模型中，每次从 Linux Domain 切换到 OP-TEE Domain 时，OpenSBI 需要协调：

```
Current Domain = Linux

1. Save Linux execution context
2. Update current Hart domain ownership
3. Save/restore necessary architectural state
4. Reconfigure applicable hardware isolation
5. Restore OP-TEE execution context
6. Return to OP-TEE S-mode
```

这些操作需要按具体源码定义的安全顺序执行，不能任意交换。

## 4.8.3 OpenSBI 如何实现 Domain Context Switch？

现在的 upstream 已经有明确的函数：

```
int sbi_domain_context_enter(struct sbi_domain *dom);

int sbi_domain_context_exit(void);
```

并由内部的：

```
switch_to_next_domain_context(...)
```

执行核心切换。

该实现会处理当前 Hart 的 Domain 归属变化，以及 CSR 等执行上下文状态。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



我们用源码中的三个概念建立认知模型：

```
Current Hart
    |
    +-- Current Domain Context
    |       Linux registers/state
    |
    +-- Target Domain Context
            OP-TEE registers/state
```

`enter()` 的基本逻辑是：

- 检查目标 Domain 是否有效；
- 确认当前 Hart 属于目标 Domain 的 `possible_harts`；
- 获取或初始化当前 Hart 的 Domain Context；
- 保存调用来源关联；
- 切换至目标 Context。

`exit()` 则可以根据此前的调用关系，选择恢复上一 Domain Context；相关初次进入与初始化逻辑还需要根据实际 Context State 处理。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



注意：

OpenSBI Domain Context Switch 并不等于 OpenSBI 内建了一个完整、通用的时间片调度器。

这个 Framework 提供上下文管理与切换能力，但实际由什么事件触发切换、如何处理 RPC、Interrupt、等待和恢复，还取决于上层协议与系统集成方式。

## 4.8.4 为什么 Domain Switch 比普通 Context Switch 更复杂？

对比你熟悉的 NuttX。

典型 NuttX Task Switch：

```
Task A
   |
   | Save Task A registers
   |
   | Select Task B
   |
   | Restore Task B registers
   v
Task B
```

它主要关注调度和 CPU 执行上下文。

而跨安全 Domain 切换还有额外要求。

| 切换状态              | NuttX Task Switch | OpenSBI Domain Switch |
| ----------------- | ----------------- | --------------------- |
| GPR               | 需要                | 需要                    |
| PC / Status       | 需要                | 需要                    |
| Stack             | 根据 Task Context   | 根据 Domain Context     |
| MMU Context       | 依架构与构建模式          | 可能必须切换                |
| FP/Vector Context | 按 RTOS 策略         | 必须保证跨 Domain 隔离       |
| PMP               | 普通任务切换通常不需要完整重配置  | 按隔离策略可能需要             |
| 其他安全硬件            | 通常非调度器职责          | 可能需要重配置               |
| Domain Ownership  | 不适用               | 需要                    |
| 跨安全域信息泄露          | 非典型 Task 模型       | 核心安全要求                |

这就是为什么 Domain Switch 不能简单通过修改 `mepc` 再执行 `mret` 完成。

尤其是 Floating-point/Vector 状态。

假设：

```
Linux:
    v0 = secret_from_linux

Switch to OP-TEE:
    v0 not saved/cleared/restored
```

那么目标 Domain 可能观察到来源 Domain 遗留的寄存器值，或者执行时意外破坏来源 Domain 的状态。

OpenSBI 的 RISE RP016 Milestone 6 已将 FP/Vector Context Switching 的 Eager 模式支持推进并合入 upstream，目的正是保证多 Domain 场景下的状态隔离与确定性。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



## 4.8.5 为什么 SMP 会进一步增加复杂度？

假设：

```
hart0:
    Linux Domain

hart1:
    OP-TEE Domain
```

此时两个 Domain 正在不同 Hart 上并发运行。

如果两个 Hart 都支持 PMP，它们的 PMP 配置通常是各 Hart 的本地架构状态。

因此可以存在：

```
hart0 PMP:
    Linux RAM  -> Allow
    TEE RAM    -> Deny

hart1 PMP:
    Linux RAM  -> Deny
    TEE RAM    -> Allow
```

这解释了为什么两个 Domain 可以在不同 Hart 上并发执行。

但同时带来一个问题：

如果 SoC 还存在 DMA Engine，PMP 本身不保证 DMA Master 遵守 Hart 的 PMP 策略。

所以在 SMP + DMA + Security Domain 的真实系统中，还必须考虑：

- IOPMP 或其他设备访问控制；
- 多 Hart 之间的共享状态；
- Interrupt Routing；
- Cache Coherency；
- TLB Synchronization；
- 资源归属变化时的隔离更新。

这些将在 Chapter 07、16、17 继续深入。

# 4.9 OpenSBI Domain 与 Hypervisor VM 的本质区别

这是本章最后一个重要架构问题。

你的 Arm 和 RISC-V Hypervisor 背景可以帮助理解二者的差异。

## 4.9.1 为什么 Domain 看起来很像 VM？

因为 Domain 也能描述：

```
CPU Resources
Memory Regions
Device Access
Boot Entry
Execution Context
```

而一个 VM 也需要这些信息。

所以很容易得出：

> OpenSBI Domain 就是一种轻量级虚拟机。

这个说法有一定直观意义，但在架构上并不准确。

两者的设计目标、抽象层次和硬件虚拟化机制不同。

## 4.9.2 Hypervisor VM 解决的核心问题

传统 Hypervisor 主要解决：

多个 Guest OS 如何在共享物理硬件上运行，并且相信自己拥有一个可用的虚拟硬件平台？

例如：

```
             Hypervisor
                 |
       +---------+---------+
       |                   |
      VM A                VM B
       |                   |
    Guest OS            Guest OS
       |                   |
  Guest Memory       Guest Memory
```

在 RISC-V H Extension 中，我们通常有：

- HS-mode：Hypervisor 软件。
- VS-mode：Guest Supervisor。
- VU-mode：Guest User。

通过 VS-stage 与 G-stage Translation 配合，实现 Guest Virtual Address 到 Host Physical Address 的转换与隔离。

例如：

```
Guest Virtual Address
        |
        | VS-stage
        v
Guest Physical Address
        |
        | G-stage
        v
Host Physical Address
```

Hypervisor 负责管理虚拟机的内存映射、虚拟 CPU 状态、设备和中断虚拟化等。

## 4.9.3 OpenSBI Domain 解决的核心问题

OpenSBI Domain 并不是以虚拟硬件作为主要抽象。

它描述的是：

```
Domain
    |
    +-- Allowed Physical Memory
    +-- Allowed Devices
    +-- Possible HARTs
    +-- Boot Configuration
    +-- Domain Execution Context
```

某个 Domain 可以直接运行一个 S-mode OS。

它并不要求存在 Guest Physical Address 这样的额外地址空间，也不要求必须使用 H Extension。

所以我们可以总结：

| 维度               | OpenSBI Domain               | Hypervisor VM         |
| ---------------- | ---------------------------- | --------------------- |
| 核心目标             | 资源分区、安全隔离与执行上下文管理            | 硬件虚拟化与多个 Guest OS     |
| 主要管理者            | OpenSBI M-mode Firmware      | Hypervisor            |
| 软件运行模式           | 通常 S/U-mode                  | 常见为 VS/VU-mode        |
| 内存机制             | PMP、平台隔离，配合自身 MMU            | Guest 页表、G-stage、硬件隔离 |
| 是否需要 H Extension | 不一定                          | 标准 RISC-V 硬件虚拟化通常需要   |
| 是否提供虚拟设备         | 不作为基础 Domain 的通用保证           | 常见的重要职责               |
| 是否适合承载 TEE       | 可以，但依赖正确安全架构                 | 取决于 VM 保护模型与硬件能力      |
| 典型场景             | Linux/TEE/独立 Firmware Domain | 多 OS 虚拟化、云计算、隔离工作负载   |

## 4.9.4 为什么 OP-TEE 更关注 Domain，而不是直接做成 VM？

因为两者的安全设计目标不同。

OP-TEE 希望：

即使 Linux 被攻击者完全控制，Linux 也无法突破 TEE 的硬件安全边界。

而普通 Hypervisor 设计通常假定 Hypervisor 属于 TCB，但不自动保证 Guest OS 对宿主特权软件保密。

因此，需要具体分析：

```
Linux compromised:
    Can Linux access TEE memory?

Hypervisor compromised:
    Can Hypervisor access guest memory?

M-mode compromised:
    Can M-mode access TEE memory?
```

对于采用 OpenSBI Domain 的 TEE：

M-mode Firmware 往往处于高信任级别，因为它管理 Domain 和隔离配置。

这意味着 M-mode Firmware 的缺陷可能破坏整个 TEE 的安全性。

它和 Arm TrustZone 中 Secure Monitor 被攻破的风险，在安全设计层面具有可比性。

不过，实际 TCB 还包括 Boot ROM、启动认证链、硬件隔离配置及相关安全固件，不能简单视为 OpenSBI 一个组件。

# 五、端到端案例：从启动到访问控制

现在我们把前面九个 Section 串联起来。

假设有一个双核 RV64 SoC：

```
hart0 -> Linux
hart1 -> OP-TEE
```

内存：

```
Linux RAM:
    0x80000000 - 0x8FFFFFFF

TEE RAM:
    0x90000000 - 0x90FFFFFF
```

下面追踪整个过程。

## Step 1 — Boot ROM 启动 OpenSBI

Boot ROM 或前一级 Bootloader 将经过相应信任链检查的 OpenSBI 加载并执行。

OpenSBI 在 M-mode 运行。

## Step 2 — 初始化 ROOT Domain

执行：

```
sbi_domain_init(scratch, cold_hartid);
```

创建默认 ROOT Domain，初始化 Domain Context 基础设施及 Firmware Protection Region。

## Step 3 — 解析 Domain Device Tree

执行：

```
sbi_domain_finalize()
    |
    v
Platform Domain Init
    |
    v
fdt_domains_populate()
```

解析出两个 Domain：

```
Linux Domain
    possible_harts = {0}
    RAM = Linux RAM

TEE Domain
    possible_harts = {1}
    RAM = TEE RAM
```

为了使前面的教学 DTS 更适合真实系统，应将宽泛的 `allmem` 权限收敛为明确的 Linux RAM 和必要 MMIO Region，并逐个验证设备授权。另外，示例 TEE `next-arg1 = 0x91000000` 位于所列 TEE RAM 之外；实际应改为平台 Boot ABI 要求的有效参数，若是内存指针，必须指向已分配且有权限的区域。这个数值不能直接用于启动。

## Step 4 — 注册并分配 Hart

执行：

```
sbi_domain_register(...);
```

检查各 Domain 配置并完成初始 Hart Assignment。

最终：

```
hart0 -> Linux Domain
hart1 -> TEE Domain
```

## Step 5 — 启动各 Domain

执行：

```
sbi_domain_startup(scratch, cold_hartid);
```

根据各自 Boot Configuration 启动：

```
hart0:
    Linux/U-Boot entry

hart1:
    OP-TEE entry
```

这只是一个专用 Hart 的演示架构，不代表完整 OP-TEE 平台一定使用这种方式。

## Step 6 — Linux 访问 OP-TEE 物理内存

假设 Linux 被攻破，攻击者建立了 VA → PA 映射：

```
VA 0xFFFF000010000000
      |
      v
PA 0x90000000
```

Linux 执行：

```
ld a0, 0(t0)
```

其中 `t0` 保存对应虚拟地址。

此时：

```
Linux VA
    |
    v
Linux Sv39 Page Table
    |
    v
PA 0x90000000
    |
    v
PMP / Hardware Isolation
    |
    v
Access Denied
```

在硬件保护已经正确实施的条件下，CPU 应产生访问异常，而不是读取 Secure RAM。

这就是 Domain 内存权限策略真正产生安全作用的地方。

关键不在于 Linux 是否知道 OP-TEE 的物理地址，而在于 Linux 是否有能力访问它。

# 六、本章源码阅读路线

你最终应该能够自己在 OpenSBI 中追踪下面的调用链。

建议阅读顺序

1. `include/sbi/sbi_domain.h`

   理解 `sbi_domain`、`sbi_domain_memregion`、Hart Mask、权限位和公开接口。
2. `lib/sbi/sbi_domain.c`

   重点 `sbi_domain_init()`、`sanitize_domain()`、`sbi_domain_register()`、`sbi_domain_finalize()`、`sbi_domain_startup()`。
3. `lib/utils/fdt/fdt_domain.c`

   追踪 DTS 如何转换为实际 Domain，并特别注意 Region 继承规则。
4. `lib/sbi/sbi_init.c`

   理解初始化次序、Domain Finalize、Startup 及保护配置的衔接。
5. `lib/sbi/sbi_domain_context.c`

   预习 `sbi_domain_context_enter()`、`exit()` 和内部切换路径，为 Chapter 06 做准备。
6. `lib/sbi/sbi_hart_protection.c`

   观察 Domain 的内存策略最终如何与 Hart PMP 保护配置结合，为 Chapter 05 做准备。

## 推荐在 WSL2 中执行的源码分析命令

```
git clone https://github.com/riscv-software-src/opensbi.git
cd opensbi

git rev-parse HEAD
```

建议保存这个 Commit ID，保证后续课程使用可复现的源码版本。

然后：

```
rg -n "struct sbi_domain" include/sbi
rg -n "sbi_domain_(init|register|finalize|startup)" lib/
rg -n "fdt_domains_populate|__fdt_parse_domain" lib/
rg -n "sbi_domain_context_(enter|exit)" lib/
rg -n "sbi_domain_check_addr" lib/
```

如果没有 `rg`，可以安装：

```
sudo apt install ripgrep
```

接下来使用 `git grep` 也可以完成类似搜索。

源码阅读时，我建议你亲自完成一份调用图：

```
sbi_init.c
    |
    +-- sbi_domain_init
    |
    +-- sbi_domain_finalize
    |       |
    |       +-- platform domains_init
    |               |
    |               +-- fdt_domains_populate
    |                       |
    |                       +-- sbi_domain_register
    |
    +-- sbi_domain_startup
    |
    +-- configure hart protection
    |
    +-- next-stage execution
```

上图是教学用的主要阶段示意，不是 `sbi_init.c` 中逐行连续出现的函数调用列表；中间还包括 HSM、平台初始化、ECALL 和其他固件初始化过程。

# 七、动手实验：观察 Domain Policy 和 Hart Ownership

我建议 Chapter 04 至少完成两个实验。

### 实验 A：观察 Domain 初始化

使用 RISE RP016 的 QEMU/Buildroot 环境：

[RP016 Build Instructions](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/build/?utm_source=chatgpt.com)

在下面几个函数设置断点：

```
sbi_domain_init
sbi_domain_register
sbi_domain_finalize
sbi_domain_startup
```

观察：

- 系统创建了多少个 Domain？
- ROOT Domain 的 `index`、`possible_harts`、`assigned_harts` 是什么？
- Linux Domain 和 TEE Domain 的 `init_order` 分别是多少？
- 每个 Domain 的 `regions` 包含哪些范围？
- Domain Startup 之后，哪些 Hart 实际被启动？

实验结束时，要能解释为什么 `possible_harts` 不等于 `assigned_harts`。

### 实验 B：验证 Memory Region 匹配逻辑

目标是构造：

```
Region A:
    Entire address space = Allow

Region B:
    0x90000000 - 0x90FFFFFF = Deny
```

在 OpenSBI 中使用 `sbi_domain_check_addr()` 检查：

| 测试地址         | 预期结果              |
| ------------ | ----------------- |
| `0x80001000` | Allow             |
| `0x8FFFFFFF` | Allow             |
| `0x90000000` | Deny              |
| `0x90001000` | Deny              |
| `0x90FFFFFF` | Deny              |
| `0x91000000` | Allow（按演示的宽泛默认规则） |

本实验先验证软件权限判断。进一步验证硬件实际访问异常，必须先确认 PMP 配置真实生效，并且只在可恢复的测试环境中执行。

我更推荐你先将 Region Matching 的六个测试用例做成 OpenSBI Unit Test，然后再进行 PMP Fault Injection。

这符合 upstream 开发习惯，也更容易形成有价值的测试贡献。

# 八、Staff Engineer 级别的理解测试

10 道关键问题

显示全部答案

01为什么仅使用不同的 satp 无法在两个 S-mode OS 之间建立可信的安全隔离？

02为什么 possible_harts 和 assigned_harts 必须拆分？

03Linux Domain 和 TEE Domain 都包含 hart0，是否意味着两个 Domain 同时拥有 hart0？

04为什么不能将 sbi_domain_check_addr() 直接视为安全隔离已经生效？

05为什么 Domain 的内存权限中 S-mode 和 U-mode 可以共用一组标志？

06如果系统只有 PMP，但还有可访问 Secure RAM 的 DMA Engine，TEE 是否安全？

07为什么 Domain Switch 不能只保存 GPR、设置 mepc 然后执行 mret？

08ROOT Domain 是否比 Linux Domain 或 TEE Domain 拥有更高的硬件 Privilege？

09Domain 和 H Extension VM 的根本差异是什么？

10如果要移植到真实 SoC，除了配置 Domain Memory Regions，你还必须验证哪些系统条件？

# 九、本章总结：用一张认知图串联所有知识

```
                    RISC-V Hardware
                           |
                           v
                      M-mode OpenSBI
                           |
                    Domain Framework
                           |
          +----------------+----------------+
          |                |                |
      Domain Config    Hart Management  Memory Policy
          |                |                |
          |         possible_harts          |
          |         assigned_harts          |
          |                |                |
          +----------------+----------------+
                           |
                      Domain Instances
                           |
               +-----------+-----------+
               |                       |
          Linux Domain            OP-TEE Domain
               |                       |
             S-mode                  S-mode
               |                       |
          Linux Processes           User TAs
             U-mode                  U-mode
```

这张图背后有五个重要结论：

1. OpenSBI Domain 是 System-Level Resource Partition，不是新的 RISC-V Privilege Mode。
2. Domain 通过 Memory Region 定义物理资源访问策略，通过 Hart Mask 定义可运行的 CPU 集合和当前归属。
3. Domain Framework 的安全性依赖 PMP、平台级内存/设备保护以及正确的固件配置，而不是仅仅依赖数据结构。
4. Linux 和 OP-TEE 可以拥有同等 S-mode 特权，但仍通过更高信任级别管理的 Domain Isolation 实现互相隔离。
5. Domain Configuration、Domain Context 和 Domain Switch 是不同层次：资源定义、执行状态、运行时切换，缺一不可。

## 下一章衔接：Chapter 05 — PMP 与系统级 Hardware Isolation

Chapter 04 已经解释了：

> OpenSBI Domain 用软件数据结构规定「谁能访问哪些物理资源」。

但还有一个至关重要的问题：

OpenSBI 如何将 `struct sbi_domain_memregion` 中的权限策略转换成真正能够限制 CPU 物理内存访问的硬件配置？

Chapter 05 将沿着这条主线展开：

```
Device Tree Region
        |
        v
struct sbi_domain_memregion
        |
        v
OpenSBI Hart Protection
        |
        v
PMP Entry Allocation / Encoding
        |
        v
pmpaddrN / pmpcfgN
        |
        v
Hardware Address Matching
        |
        v
Allow / Deny / Access Fault
```

核心是从实际源码解释 PMP Address Matching、Entry Priority、Region Overlap、PMP 数量限制，以及为什么 CPU PMP 不能替代系统级 IOPMP/WorldGuard。

这将为后面的 Chapter 06 — Domain Context Switching 建立完整的硬件隔离基础。