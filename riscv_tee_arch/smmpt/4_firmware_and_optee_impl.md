# RISC-V SMMPT 深度课程 · PART IV

## Firmware Architecture & OP-TEE Integration

Chapter 14–16：从 SoC Reset、M-mode RDSM，到 SMMPT 驱动的 OP-TEE Secure Domain

这一部分是整套 SMMPT 课程从硬件架构转向软件实现的关键阶段。

前面三个 Part 主要回答：

- SMMPT 为什么存在，以及它解决什么问题。
- MPT、MPTE、`mmpt`、`MFENCE.PA` 等硬件机制如何工作。
- SMMPT 如何与 MMU、PMP、IOMMU、Interrupt Architecture 共同建立系统级隔离。

PART IV 则要回答一个更实际的问题：

给定一颗支持 SMMPT 的 RV64 SoC、一份 OpenSBI 源码和一份 OP-TEE 源码，我们究竟应该修改哪些模块、按照什么顺序初始化，才能让 Linux 与 OP-TEE 真正运行在两个隔离的 Supervisor Domain 中？

三章的知识目标如下。

| Chapter                          | 核心目标                | 最终掌握的能力                                   |
| -------------------------------- | ------------------- | ----------------------------------------- |
| 14. Boot & Domain Initialization | 从 Reset 建立第一道安全边界   | 独立设计安全启动、MPT 初始化和多 Hart 启动流程              |
| 15. OpenSBI & RDSM               | 构建可信 Domain Manager | 理解 OpenSBI 源码并设计安全的 Domain Switch         |
| 16. OP-TEE Integration           | 实现完整的 TEE Runtime   | 将 Memory、Thread、Trap、RPC、SMP 与 SMMPT 串联起来 |

本讲义会严格区分三类内容：

- 规范机制： 来自 RISC-V Supervisor Domain Access Protection / Smsd / Smmpt 的架构约束。
- 已存在的开源实现： 可以在 OpenSBI、OP-TEE 和 Linux 实际源码中定位的功能。
- 参考集成设计： 为了实现 SMMPT + OP-TEE 而提出的接口、数据结构和伪代码，不将其冒充上游已经合并的实现。

需要特别说明：公开 SMMPT 规范仍在演进。2026 年的 RISC-V OP-TEE 社区也正在推进 RPMI TEE Transport，相关 Linux 驱动与协议仍有 RFC 阶段的工作。因此，以下课程给出的是有规范依据的工程设计方法，而不是声称某个现成的上游版本已经具备全部功能。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



# Chapter 14 — 从 SoC Reset 到第一个 Supervisor Domain

Boot Architecture · Chain of Trust · Memory Ownership · MPT Initialization · Hart Bring-up

## 14.1 本章的核心问题

假设我们的目标 SoC 具有以下硬件和软件组件：

- 16 个 RV64 Application Hart，支持 M/S/U Mode。
- 支持 `Smsd` 和 `Smmpt43`。
- 一个独立 RSS/eHSM，作为硬件 Root of Trust。
- OpenSBI 负责 M-mode Firmware Runtime。
- OP-TEE 运行在独立的 S-mode Domain。
- UEFI 和 Linux 运行在 Host Domain。
- IOMMU 与可选的 I/O MPT Checker。

我们希望系统启动之后满足：

\\[ \operatorname{Access}\_{Linux}(TEE\_{Private})=Deny \\]

\\[ \operatorname{Access}\_{OPTEE}(TEE\_{Private})=Allow \\]

\\[ \operatorname{Write}\_{Linux}(MPT)=Deny \\]

其中的 Access 表示经过所有有效硬件访问控制机制后的最终授权结果。

最值得注意的是第三条：Linux 不仅不能读取 OP-TEE 的私有内存，还不能通过修改 MPT 来间接获得访问权限。

但这里会产生一个初始化问题：

系统刚刚 Reset 时，MPT 尚未构建，那么是谁负责保护 MPT 本身，又如何避免在建立安全边界之前暴露敏感内存？

这就是本章的主线。

## 14.2 首先理解整个 Boot Architecture

### 14.2.1 将启动链分成三个阶段

我建议不要一开始就将所有 Firmware 混在一起，而是先分成三个阶段。

Stage A — Hardware Root of Trust

SoC Reset → BootROM → RSS/eHSM → Firmware Authentication

Stage B — M-mode Trusted Firmware

OpenSBI / RDSM → PMP → MPT → Domain Creation

Trusted Domain

OP-TEE S-mode

Host Domain

UEFI → Linux S-mode

参考启动架构。实际镜像加载顺序、RSS 与 Application Hart 的协作方式由平台决定。

三个阶段承担三种不同的职责。

| 层级             | 主要职责                                  | 不应该承担的职责                     |
| -------------- | ------------------------------------- | ---------------------------- |
| RSS / BootROM  | 密钥保护、镜像认证、建立启动可信链                     | Linux 任务调度、OP-TEE Thread 管理  |
| OpenSBI / RDSM | Supervisor Domain 创建、MPT 配置、Hart 安全切换 | 管理普通 Linux 进程的 VA Page Table |
| OP-TEE / Linux | 各自 Domain 内部的 Kernel、Thread、应用和虚拟内存管理 | 任意修改对方的 MPT 权限               |

这里非常关键的一点是：RSS 不必运行在 Application Hart 的 M-mode 中。

它可能是独立的安全处理器，通过 Mailbox、MMIO 或其他 SoC 内部 IPC 与 Application Hart 交互。

因此：

- RSS 负责建立 Firmware 的真实性与启动信任。
- RDSM 负责运行期间 Supervisor Domain 的隔离策略。
- 两者可能共享安全策略或密钥服务，但不是同一个概念。

### 14.2.2 为什么必须先建立隔离，再运行 Host？

假设启动顺序错误：

```
BootROM
   |
   v
OpenSBI
   |
   v
Linux starts
   |
   v
Create MPT
   |
   v
Start OP-TEE
```

这个设计存在根本问题。

Linux 在 MPT 正式生效之前，就可能访问那些计划将来作为 OP-TEE Private Memory 的物理地址。

因此不能依赖 Linux 启动之后，再通过软件约定去“补上”安全隔离。

正确的安全原则是：

> 在不可信 Supervisor 软件获得执行权之前，可信 Firmware 必须已经建立必要的资源访问边界。

这不代表必须初始化所有运行期资源，而是至少保证 Host 无法访问受保护的 TEE Image、TEE Data、MPT 和 Root-owned Control Resources。

## 14.3 Secure Boot 与 SMMPT 解决的是不同问题

这个区别非常重要。

Secure Boot 要回答：

正在执行的软件是不是被允许执行的那个软件？

SMMPT 要回答：

已经在执行的软件，能够访问哪些资源？

Secure Boot 解决的是身份与完整性问题，SMMPT 解决的是运行期间的空间隔离问题。

假设 Linux 已经通过签名验证，其镜像是合法的。

这并不意味着 Linux 必须得到读取 OP-TEE Key Storage 的权限。

即使 Linux 是经过验证的，运行期间仍可能因为驱动漏洞被攻击者控制。

因此二者是组合关系，而不是替代关系：

```
Secure Boot
    |
    +-- Authenticate Firmware
    |
    +-- Authenticate OP-TEE Image
    |
    +-- Authenticate UEFI / Linux
    |
    v
Runtime Isolation
    |
    +-- MPT protects TEE private memory
    |
    +-- PMP / Smepmp protects RDSM
    |
    +-- I/O protection restricts DMA
    |
    +-- Interrupt isolation restricts routing
```

另外，SMMPT 本身不提供镜像认证、Attestation 或 Memory Encryption。这些功能需要其他机制共同提供。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 14.4 Boot Memory Layout 的设计

### 14.4.1 为什么 Memory Layout 必须先于 MPT 初始化？

因为 MPT 中的权限是针对物理地址制定的。

如果还不知道 OP-TEE 被加载到哪个物理地址、运行期占用哪些区域、哪些地址需要共享，就无法正确建立 MPT。

我们先设计一份简化的地址布局。

以下地址为教学示例，不代表任何真实 SoC 的地址分配。

| Physical Address | Size    | Owner     | 用途                     |
| ---------------- | ------- | --------- | ---------------------- |
| `0x8000_0000`    | 2 MiB   | Root      | M-mode Firmware        |
| `0x8100_0000`    | 2 MiB   | Root      | MPT、Domain Metadata    |
| `0x9000_0000`    | 64 MiB  | TEE       | OP-TEE Code、Data、Stack |
| `0x9400_0000`    | 16 MiB  | Shared    | Linux ↔ OP-TEE         |
| `0xA000_0000`    | 512 MiB | Host      | UEFI / Linux           |
| 其他 DDR           | 按需      | Host/Root | Normal RAM、保留资源        |

现在根据这些区域，分别建立两个 Domain 的 Memory Permission Map。

### 14.4.2 Linux Domain 的 MPT

| Region         | R | W | X   |
| -------------- | - | - | --- |
| Root Firmware  | 0 | 0 | 0   |
| MPT Tables     | 0 | 0 | 0   |
| OP-TEE Private | 0 | 0 | 0   |
| Shared Memory  | 1 | 1 | 0   |
| Host Memory    | 1 | 1 | 按映射 |
| Host UART      | 1 | 1 | 0   |

### 14.4.3 OP-TEE Domain 的 MPT

| Region               | R    | W    | X  |
| -------------------- | ---- | ---- | -- |
| Root Firmware        | 0    | 0    | 0  |
| MPT Tables           | 0    | 0    | 0  |
| OP-TEE Private       | 1    | 按段   | 按段 |
| Shared Memory        | 1    | 1    | 0  |
| Host Memory          | 默认 0 | 默认 0 | 0  |
| TEE Dedicated Device | 按需   | 按需   | 0  |

这里需要遵循两个安全原则。

原则一：MPT 应该采用 Default Deny。

未明确授权的内存区域，默认不可访问。

原则二：OP-TEE 也不应该拥有整个物理地址空间的访问权限。

OP-TEE 属于 Trusted Domain，并不意味着它应该具有任意读取 Host 内存和设备的能力。允许它访问 Host 资源应当基于明确的 Shared Memory、RPC 或受控设备操作需求。

同样，OP-TEE 私有区域内的 Code 应当尽量采用 RX、Data/Stack 采用 RW、不可执行，而不是统一 RWX。

### 14.4.4 为什么 Device Tree 的 reserved-memory 不等于安全隔离？

例如：

```
reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;
    ranges;

    optee_core: optee@90000000 {
        reg = <0x0 0x90000000
               0x0 0x04000000>;
        no-map;
    };
};
```

该节点可以告诉 Linux：

这块物理内存不应纳入普通内存管理和线性映射。

但它不能阻止恶意 Linux Kernel 故意建立其他映射，再执行 Load/Store。

因此：

\\[ DTReservedMemory \neq HardwareIsolation \\]

真正阻止恶意访问的是 PMP/MPT/I/O Protection 等硬件机制，而不是 FDT 中的声明。

## 14.5 SMMPT 初始化的正确顺序

现在进入本章最关键的部分。

### 14.5.1 一套参考初始化流程

```
1. Trusted Firmware execution begins
         |
         v
2. Establish trusted M-mode stack
         |
         v
3. Validate platform capabilities
         |
         v
4. Establish Root-owned memory protection
         |
         v
5. Allocate and initialize MPT storage
         |
         v
6. Build Linux and OP-TEE MPT
         |
         v
7. Initialize Domain Metadata
         |
         v
8. Configure and synchronize MPT
         |
         v
9. Configure per-Hart mmpt / Domain state
         |
         v
10. Enable Supervisor execution
         |
         v
11. Start OP-TEE and Host software
```

这是一套参考顺序，并非规范规定的唯一 Boot Flow。具体还需要处理镜像加载、内存分配器初始化以及冷启动 Hart 与其他 Hart 的同步。

最重要的先后约束是：

1. MPT 存储必须来自可信管理的内存。
2. 在启用 MPT 保护之前，其表项必须满足规范要求。
3. 不能让不可信软件修改 MPT。
4. 不能在隔离尚未建立时，将受保护的 TEE 资源暴露给 Host。
5. 多 Hart 必须分别完成自身所需的保护配置。

### 14.5.2 第一个关键动作：保护 M-mode Firmware

考虑以下情况。

RDSM 决定将 `0x8100_0000` 作为 MPT 存储区域。

如果 Linux 可以写入它，就能够把：

```
Linux MPT:
    TEE Private = Deny
```

直接修改为：

```
Linux MPT:
    TEE Private = Allow
```

此时 MPT Checker 完全按照被篡改的权限执行，系统隔离已经失效。

因此 MPT 的保护依赖一个更基本的信任条件：

MPT 配置及其存储区域必须处于 RDSM 的可信控制之下。

通常需要：

- 通过 PMP / Smepmp 等机制保护 M-mode Firmware 和根控制资源。
- 禁止 Host 和 TEE Domain 写入 MPT。
- 对 MPT 的更新只开放经过授权验证的 RDSM 接口。
- 保证 MPT 自身的硬件 Page Walk 不因错误 PMP 配置而失败。

这也引出一个容易忽略的架构细节：

MPT 的硬件隐式读取，需要受到对应的 PMA、PMP/Smepmp 访问约束。不能因为软件禁止 S-mode 读取表页，就误把 MPT Walker 自身的合法读取也禁止掉。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 14.6 创建 Root MPT

为了将规范转化为代码，我们设计一个简化的 Firmware MPT 管理接口。

以下是参考设计，不是 OpenSBI 已有 API：

```
struct sd_mpt {
    unsigned long root_pa;
    unsigned long mode;
    unsigned long generation;
};

struct sd_domain {
    unsigned int domain_id;
    struct sd_mpt mpt;

    unsigned long entry_pa;
    unsigned long fdt_pa;

    unsigned long hart_mask;
};

int mpt_init_allocator(void);

int mpt_create(struct sd_mpt *mpt,
               unsigned long mode);

int mpt_map_region(struct sd_mpt *mpt,
                   unsigned long pa,
                   unsigned long size,
                   unsigned int perms);

int sd_create_domain(struct sd_domain *dom);
```

注意三个关键设计点。

第一，`root_pa` 保存的是物理地址。

因为 `mmpt.PPN` 指向 MPT Root Physical Page，硬件不会使用 Firmware 的虚拟地址查找 MPT Root。

第二，`domain_id` 应当是软件管理的逻辑 ID。

它不能简单等同于 `mmpt.SDID`。规范中的 Hart-local `SDID` 是缓存及本地访问控制关联标识，不保证全球唯一，且实现允许只提供部分 SDID 位，甚至实现零个 SDID 位。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



第三，`generation` 用于支持软件版本管理。

如果 MPT 被修改，Firmware 需要知道对应 Hart 是否已经完成必要的 MPT Synchronization。

这不是硬件自动提供的功能，而是推荐 Firmware 自己实现的管理策略。

### 14.6.1 构建两套 MPT

我们可以概念性地编写：

```
/* Illustrative pseudo-code */

mpt_create(&host.mpt, MPT_MODE_SMMPT43);
mpt_create(&tee.mpt,  MPT_MODE_SMMPT43);

mpt_map_region(&host.mpt,
               HOST_RAM_PA, HOST_RAM_SIZE,
               MPT_R | MPT_W);

mpt_map_region(&tee.mpt,
               TEE_RAM_PA, TEE_RAM_SIZE,
               MPT_R | MPT_W);

mpt_map_region(&host.mpt,
               SHM_PA, SHM_SIZE,
               MPT_R | MPT_W);

mpt_map_region(&tee.mpt,
               SHM_PA, SHM_SIZE,
               MPT_R | MPT_W);
```

这里为了简化使用了 Region Mapping API，但真正写入 MPTE 时，必须考虑：

- MPT Page Level 和 Leaf 格式。
- `V/L/N` 位的编码。
- 16 个 4KiB 子页权限如何编码到一个 Smmpt43 Leaf MPTE。
- 大页面权限映射。
- Reserved Bits 必须按规范清零。
- MPTE 地址和 MPT Root 对齐。
- 修改有效表项时的同步需求。

这些是 Chapter 05 和 Chapter 08 的基础知识在 Firmware 中的实际应用。

## 14.7 初始化每个 Hart 的 `mmpt`

在 RV64 的 `Smsd` 中，`mmpt` 是 M-mode CSR，编号为 `0x382`。

它包含：

| Field  | 作用                                 |
| ------ | ---------------------------------- |
| `MODE` | 选择 Smmpt43/52/64 或 Bare            |
| `SDID` | 当前 Hart-local Supervisor Domain 标识 |
| `PPN`  | 当前 Domain 的 MPT Root PPN           |

不应该将 `mmpt` 简单理解为只需在系统启动时写入一次的全局寄存器。

它是 Hart-local 的当前执行上下文状态。

假设 Hart 0 当前运行 Linux，而 Hart 1 当前运行 OP-TEE：

```
Hart 0:
    mmpt.PPN  = Linux MPT Root
    mmpt.SDID = Linux local SDID

Hart 1:
    mmpt.PPN  = OP-TEE MPT Root
    mmpt.SDID = OP-TEE local SDID
```

虽然是两个独立 Hart，但各自 MPT 可以引用相同的共享物理页面，也可以通过策略形成完全不同的权限视图。

### 14.7.1 参考汇编

```
/* M-mode only; conceptual sequence */

    /* t0 contains a validated mmpt value */
    csrw    0x382, t0

    /*
     * If required, synchronize changed MPT state.
     * The exact fence operands and placement depend
     * on MPT updates and SDID reuse.
     */
```

为什么这里不直接写死 `MFENCE.PA` 的参数？

因为需要先判断：

- 是创建全新的 MPT，还是修改已有 MPT？
- 是否存在该 MPT 的旧缓存状态？
- SDID 是否复用？
- 之前的 MPTE Stores 是否已经对当前 Hart 可见？
- 是否有其他 Hart 仍然使用旧权限？

规范明确规定，写入 `mmpt` 本身并不自动保证 MPT 更新与后续访问之间的同步顺序。 因此必须根据具体操作选择正确的 Fence Protocol。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 14.8 从 M-mode 跳转到 S-mode

现在 MPT 配置完成，准备启动 OP-TEE。

在普通 RISC-V 平台上，M-mode 跳到 S-mode 的基本原理是：

1. 设置 `mepc`。
2. 设置 `mstatus.MPP = S`。
3. 建立必要的目标 Domain 上下文。
4. 执行 `mret`。

简化汇编如下：

```
    /* Set S-mode entry address */
    la      t0, optee_entry
    csrw    mepc, t0

    /* Set MPP = S */
    csrr    t1, mstatus
    li      t2, (3 << 11)
    not     t2, t2
    and     t1, t1, t2

    li      t2, (1 << 11)
    or      t1, t1, t2
    csrw    mstatus, t1

    /* Enter S-mode */
    mret
```

上述代码只是解释 MPP 和 `mret` 的核心机制，不是可以直接用于生产环境的完整安全切换程序。

实际 Firmware 必须在 `mret` 之前正确处理：

- `mmpt` 与对应的 MPT 同步。
- `satp`、S-mode Trap CSRs 等状态。
- GPR、FPU、Vector 和其他扩展状态。
- 安全的 Stack 和入口参数。
- Interrupt Pending/Enable 状态。
- Hart 的 Domain Ownership。
- PMP/Smepmp 等其他保护机制。

其中 `mret` 并不自动帮我们保存 Linux 的 Thread Context，也不会自动配置 OP-TEE 的 MPT。

在 OpenSBI 中，真实的模式切换代码可参考 `lib/sbi/sbi_hart.c` 中的 `sbi_hart_switch_mode()`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 14.9 Cold Boot、Warm Boot 与 Secondary Hart

### 14.9.1 16 个 Hart 不应该同时创建 Root MPT

假设 16 个 Hart 几乎同时从 Reset 入口开始执行。

我们不希望 16 个 Hart 各自创建一份互相冲突的全局 Domain 数据结构。

建议采用：

```
Hart 0                    Hart 1..15
  |                           |
  v                           v
Cold Boot Owner           Warm Boot Path
  |                           |
  +-- Initialize Root TCB      |
  +-- Initialize MPT           |
  +-- Create Domains           |
  +-- Publish Ready State      |
  |                           |
  |                     Wait for Ready
  |                           |
  |                     Initialize Local State
  |                           |
  |                     Configure Local mmpt
  |                           |
  +---------- Launch ----------+
```

全局元数据通常由一个 Cold Boot Owner 初始化。

但每个 Hart 自己的 CSR、Trap State、Stack 和硬件保护配置仍需要在本 Hart 上完成。

### 14.9.2 从 OpenSBI 源码理解

可重点研究：

- `lib/sbi/sbi_init.c`
  - `sbi_init()`
  - `init_coldboot()`
  - `init_warmboot()`
  - `init_warm_startup()`
  - `init_warm_resume()`
- `lib/sbi/sbi_hsm.c`
  - HART State Machine 与 Start/Stop/Resume。
- `lib/sbi/sbi_hart.c`
  - Hart-local CSR 初始化。

OpenSBI 的初始化代码明确区分 Cold Boot 与 Warm Boot，也会在 Hart 进入下一阶段之前配置相应的 Hart Protection。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



### 14.9.3 一个容易被忽略的安全问题

假设 Hart 3 被 Linux 通过 SBI HSM 停止，之后重新启动。

如果 Hart 3 重新进入 S-mode 时，仍然保留了上一次 OP-TEE Domain 的 MPT 配置，就可能出现错误授权。

因此需要保证：

\\[ Domain(Hart_3)=Host \\]

与：

\\[ MPT(Hart_3)=MPT\_{Host} \\]

在启动进入 Host 的时刻保持一致。

不能仅依赖“上一次运行的 Domain”推断当前安全配置。

## 14.10 Boot Success Criteria

仅仅看到 OP-TEE 的启动日志，不代表 SMMPT 集成成功。

建议设计如下验收矩阵。

| 验证项目                 | 成功判据                            |
| -------------------- | ------------------------------- |
| Root Firmware        | 验证通过，正确执行 M-mode Runtime        |
| MPT Initialization   | 两套 MPT 已建立，内容与预期一致              |
| `mmpt` Configuration | 每个 Hart 当前配置与 Domain 对应         |
| OP-TEE Boot          | Secure Domain 完成 Kernel 初始化     |
| Linux Boot           | Host Domain 正常启动                |
| Isolation Test       | Linux 访问 TEE Private Memory 被拒绝 |
| MPT Protection       | Host 无法修改 MPT                   |
| SMP                  | 所有参与的 Hart 均遵守对应 Domain 策略      |
| DMA                  | 非可信设备无法绕过 CPU 隔离策略              |
| Warm Boot            | Hart 重新启动不会继承错误的 Domain 权限      |

最终要证明的是：既能正常运行，也无法越权运行。

## 14.11 Chapter 14 — Staff Engineer 级测试题

建议先独立思考，再通过源码、规范或实验验证。这里不直接附标准答案，以便真正测试掌握程度。

1\. Boot Race： 一颗 16-Hart SoC 在 BootROM 验证 OpenSBI 之后，多个 Hart 同时执行 M-mode 入口。设计一套安全的 Cold/Warm Boot Protocol，保证只有一个 Hart 初始化 MPT，同时不会让其他 Hart 提前进入不可信 S-mode。

2\. Protection Bootstrap： SMMPT 的 MPT 也是普通物理内存。说明在 MPT 尚未启用时，应该如何保护 MPT 存储，以及启用之后如何保证硬件 MPT Walk 仍能够读取这些表页。

3\. Memory Layout： OP-TEE 占用 64MiB，Linux 占用 8GiB，双方共享 16MiB。设计两套 MPT 权限，并说明如何处理 OP-TEE Code/Data/Stack 的不同权限。

4\. Secure Boot vs Runtime Isolation： 如果 Linux 已通过 Secure Boot 验证，为什么仍然不能直接给予它访问 OP-TEE Memory 的权限？从威胁模型、TCB 和运行期攻击三个角度解释。

5\. MMPT Initialization： 写入一个新的 `mmpt` 值是否就意味着新的 MPT 权限立即正确生效？区分新 MPT、修改 MPTE、复用 SDID 三种情况。

6\. Warm Boot： Hart 5 曾经执行 OP-TEE Domain，随后被关闭并重新启动进入 Linux。哪些状态必须重新初始化？如何保证没有残留的 MPT、MMU、Interrupt 或 FPU/Vector State？

7\. Security Failure： Linux 可以读取 MPT，但不能写入 MPT。这是否一定是安全的？请区分隔离正确性、信息泄漏、内存布局暴露及攻击面。

8\. Root of Trust： 如果 RSS 是独立的 HSM Core，而 RDSM 运行在 Application Hart 的 M-mode 中，谁负责镜像认证，谁负责 Runtime Policy？二者应通过什么信任机制交换启动结果？

9\. Fault Debug： OP-TEE 第一次执行到 `0x90001000` 附近时发生 Instruction Access Fault。给出一套从 `mepc`、`mcause`、`mtval`、PMP 到 MPT Root、MPTE 的完整排查流程。

10\. Architecture Review： 某设计方案让 Linux 先启动，之后再通过 SBI 创建 OP-TEE Domain，但在创建之前 TEE 内存并无硬件保护。请判断能否用于真实安全产品；如果不能，提出可以接受的修正设计。

通过标准： 不仅能够解释初始化顺序，还要能够说清每一步建立了什么安全不变量、哪个组件有权限执行、失败后会留下什么风险。

# Chapter 15 — OpenSBI 与 RDSM：Domain 管理如何真正实现？

OpenSBI Source Code · Domain Management · SBI Ecall · Context Switch · MPT Sync · SMP

## 15.1 本章的核心问题

前面已经建立了两套 MPT：

```
Linux Domain:
    MPT Root = MPT_LINUX

OP-TEE Domain:
    MPT Root = MPT_OPTEE
```

现在假设 Hart 0 正在运行 Linux。

用户态应用执行：

```
TEEC_InvokeCommand(...);
```

Linux 最终需要请求 OP-TEE 执行一个 Trusted Application。

这里的问题是：

Hart 0 不能同时执行 Linux 和 OP-TEE。那么应该由谁控制 Hart 0 从 Linux Domain 切换到 OP-TEE Domain？又应该由谁恢复原来的 Linux Context？

答案是：需要一个独立于两个 S-mode Domain、具备更高信任等级的 Domain Manager。

在 RISC-V Supervisor Domain 架构中，这个角色叫作 Root Domain Security Manager，简称 RDSM。

但 RDSM 并不一定意味着在 OpenSBI 下面还要再添加一层 Firmware。规范将 RDSM 定义为负责管理 Supervisor Domain 的 M-mode 可信固件整体。它可以与 OpenSBI 集成，也可以由其他受信任的 M-mode 组件承担相应职责。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 15.2 首先澄清三个容易混淆的概念

### 15.2.1 OpenSBI Domain

OpenSBI 已有 Domain 管理能力。

它将若干 Hart、Memory Region、Device 与启动信息组合成一个系统级分区，并借助 PMP、ePMP 等硬件机制建立隔离。

核心数据结构包括：

- `struct sbi_domain`
- `struct sbi_domain_memregion`
- `struct sbi_hartmask`

相关说明位于 OpenSBI 的 [Domain Support 文档](https://github.com/riscv-software-src/opensbi/blob/master/docs/domain_support.md?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 15.2.2 SMMPT Supervisor Domain

这是 Supervisor Domain Access Protection 架构中的隔离实体。

它的物理内存访问权限可以由对应的 MPT 表达，并由 M-mode 配置 `mmpt`。

在 SMMPT 中，重点不再是有限数量的 PMP Region，而是通过内存中的多级 MPT 来表达细粒度权限。

### 15.2.3 RDSM

RDSM 不是一个 CSR，也不是硬件模块的名称。

它是管理 Supervisor Domain 生命周期、隔离策略与可信切换的 Firmware 软件角色。

对照来看：

| 概念                      | 类型             | 主要职责                 |
| ----------------------- | -------------- | -------------------- |
| OpenSBI Domain          | 软件分区抽象         | 管理 Hart、内存区域、启动参数等   |
| SMMPT Supervisor Domain | 架构信任域          | 定义相互隔离的物理资源权限        |
| RDSM                    | M-mode 软件角色    | 创建、切换、管理和销毁 Domain   |
| `mmpt`                  | Hart-local CSR | 指定当前 MPT Root 和 SDID |
| MPT                     | 内存中的保护表        | 定义物理地址的 R/W/X 权限     |

OpenSBI Domain 可以作为构建 SMMPT Supervisor Domain 的软件基础，但两者并不是天然一一等价。

这是本章最重要的架构前提。

## 15.3 从 OpenSBI 源码理解现有 Domain 设计

### 15.3.1 第一处源码：`sbi_domain.c`

建议阅读：

[OpenSBI — sbi_domain.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain.c?utm_source=chatgpt.com)

该模块负责维护 Domain 实例与基本生命周期信息。

从源码可以看到，OpenSBI 维护一个 Domain List，并存在默认的 Root Domain。

这意味着 OpenSBI 并不是每个 Domain 都需要一套独立的 Firmware Image。

它可以维护多个 Domain 对象，并将 Hart 分配给不同 Domain。

### 15.3.2 第二处源码：`sbi_domain_context.c`

更值得深入的是：

[OpenSBI — sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)

其中的 `struct hart_context` 包含：

| 状态                          | 用途                          |
| --------------------------- | --------------------------- |
| `trap_ctx`                  | GPR、Trap、`mepc`、`mstatus` 等 |
| `sstatus`                   | Supervisor 状态               |
| `sie` / `sip`               | S-mode 中断 Enable/Pending    |
| `stvec`                     | S-mode Trap Vector          |
| `sscratch`                  | S-mode 临时上下文                |
| `sepc` / `scause` / `stval` | S-mode Trap 状态              |
| `satp`                      | S-mode MMU 地址空间             |
| `scounteren` / `senvcfg`    | 扩展环境状态                      |
| FP / Vector Context         | 浮点和向量上下文                    |
| `dom` / `prev_ctx`          | 当前 Domain 与返回关系             |

这些字段不是凭空设计的，OpenSBI 当前源码确实已经具备对应的 Supervisor Context 保存与恢复逻辑。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



接下来我们从架构角度分析这份代码。

## 15.4 第一次深入：Context 为什么必须按 Domain × Hart 管理？

假设系统包含：

```
Domain 0: Linux
Domain 1: OP-TEE

Hart 0
Hart 1
Hart 2
Hart 3
```

每个 Hart 在不同时间可能进入不同 Domain。

我们不应该只创建：

```
struct hart_context linux_ctx;
struct hart_context optee_ctx;
```

因为 Hart 0 与 Hart 1 可能同时执行 OP-TEE 内的不同线程。

两者的 GPR、SP、Trap State 和当前执行位置并不相同。

因此推荐使用二维逻辑模型：

\\[ Context = f(Domain,\ Hart) \\]

例如：

```
                   Linux       OP-TEE

Hart 0          Context 00   Context 01
Hart 1          Context 10   Context 11
Hart 2          Context 20   Context 21
Hart 3          Context 30   Context 31
```

OpenSBI 的 `hart_context_get()` 正是沿着 Domain 和 Hart Index 查找对应 Context。

这是一个可以直接复用的设计思想。

但是要注意：

Domain × Hart Context 并不等于 OP-TEE Trusted Thread Context。

前者保存 Hart 在跨 Domain 切换时的执行状态。

后者是 OP-TEE 自己管理的逻辑 Trusted Thread、TA Session 和用户态 TA Context。

一个 Hart 上的同一个 Domain，可以在不同时间运行多个 Trusted Thread。

因此，整个系统实际存在两个调度层次：

```
M-mode RDSM
    |
    +-- Select Supervisor Domain
            |
            v
S-mode OP-TEE
    |
    +-- Select Trusted Thread
            |
            +-- Select User TA Context
```

如果把这两层上下文混为一谈，后续会在 SMP、RPC 和 Thread Resume 场景中出现严重的设计错误。

## 15.5 第二次深入：真实 Domain Switch 的源码逻辑

OpenSBI 中已经存在以下函数：

```
int sbi_domain_context_enter(struct sbi_domain *dom);

int sbi_domain_context_exit(void);
```

其内部进一步调用 `switch_to_next_domain_context()`。

后者执行的核心操作包括：

1. 更新当前 Hart 的 Domain 归属。
2. 保存当前 Supervisor CSR。
3. 恢复目标 Domain 的 Supervisor CSR。
4. 保存并恢复 FP/Vector 等状态。
5. 保存并恢复 Trap Context。
6. 重新配置当前 Hart 的 PMP 保护。
7. 处理目标 Domain 尚未初始化的情况。

以上操作都可以在当前 OpenSBI 源码中追踪。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



但这里有一个架构上的缺口：

现有基于 PMP 的 Domain Context Switch，并不能自动等价于完整的 SMMPT Domain Switch。

为了支持 SMMPT，我们至少还要管理：

- 目标 Domain 的 MPT Root。
- Hart-local SDID。
- `mmpt` CSR。
- MPT Cache Synchronization。
- `msdcfg` 中由可选扩展定义的状态。
- 跨 Hart 的 MPT 权限更新。
- 与 I/O Domain 相关的安全配置。

下面逐项分析。

## 15.6 为 SMMPT 设计 Domain 数据结构

我建议区分三类对象。

### 15.6.1 全局 Domain 对象

保存不会随着 Hart 切换而改变的全局属性。

```
/* Reference design, not an upstream OpenSBI struct */

struct rdsm_domain {
    uint32_t logical_id;

    uint64_t mpt_root_pa;
    uint64_t mpt_generation;

    uint64_t allowed_harts;

    uint64_t entry_pa;
    uint64_t fdt_pa;

    enum {
        SD_CREATED,
        SD_READY,
        SD_RUNNING,
        SD_QUIESCING,
        SD_DESTROYED
    } state;
};
```

其中：

- `logical_id` 是 Firmware 分配的软件逻辑标识。
- `mpt_root_pa` 指向该 Domain 的 MPT。
- `mpt_generation` 用于记录表项更新代际。
- `allowed_harts` 是 Domain 可运行的 Hart 集合。
- `state` 控制 Domain Lifecycle。

### 15.6.2 Per-Hart Domain Context

```
struct rdsm_hart_context {
    uint64_t gpr[32];

    uint64_t mepc;
    uint64_t mstatus;

    uint64_t sstatus;
    uint64_t satp;
    uint64_t stvec;
    uint64_t sscratch;

    uint64_t mmpt;
    uint64_t msdcfg;

    /* Other architectural states as required */
};
```

这里仅展示代表性字段。

真实实现应当复用 OpenSBI 已有的 Trap Context，并根据目标 ISA 扩展决定哪些额外状态需要保存。

不能把上面的简化结构直接当作可用的生产级 Context。

### 15.6.3 Per-Hart Runtime State

```
struct rdsm_hart_state {
    uint32_t hart_id;

    uint32_t current_domain_id;

    uint64_t observed_mpt_generation;

    struct rdsm_hart_context *active_context;
};
```

为什么还需要这个结构？

因为我们需要同时回答三个问题：

1. 当前 Hart 运行哪个 Domain？
2. 它的硬件 CSR 对应哪个 Domain？
3. 它是否已经观察到最新的 MPT 权限更新？

尤其是第三点，将直接影响动态内存共享和撤销的安全性。

## 15.7 如何设计一个安全的 Domain Switch？

首先给出安全不变量。

对于即将运行的目标 Domain \\(D\\)，必须满足：

\\[ MPT\_{ActiveHart}=MPT_D \\]

\\[ SupervisorContext\_{ActiveHart}=Context_D \\]

\\[ ProtectionState\_{ActiveHart}=Policy_D \\]

并且在返回 S-mode 时，当前 Hart 不得继续使用另一个 Domain 留下的敏感执行状态。

### 15.7.1 参考切换算法

以下是表达执行顺序的伪代码，具体寄存器保存、同步和 Trap Return 必须由底层汇编实现。

```
int rdsm_switch_domain(struct rdsm_domain *target)
{
    struct rdsm_hart_state *hs = this_hart_state();
    struct rdsm_domain *current = current_domain();

    /* 1. Validate requested transition */
    if (!rdsm_transition_allowed(current, target))
        return -1;

    /* 2. Preserve current execution context */
    save_supervisor_context(current, hs);

    /* 3. Enter a safe M-mode switch state */
    prepare_switch_environment();

    /* 4. Apply required memory protection updates */
    synchronize_domain_mpt(target, hs);

    /* 5. Restore target architecture context */
    restore_supervisor_context(target, hs);

    /* 6. Install target MPT configuration */
    install_domain_mmpt(target, hs);

    /* 7. Apply interrupt and auxiliary protection */
    configure_domain_aux_state(target, hs);

    /* 8. Commit software bookkeeping */
    hs->current_domain_id = target->logical_id;

    /* 9. Return to target Supervisor context */
    return_to_supervisor_context();

    __builtin_unreachable();
}
```

这段代码表达的是安全切换所需完成的逻辑阶段，不是对所有平台都成立的唯一硬件指令顺序。

例如，在 M-mode 中保存/恢复 `satp`、写入 `mmpt` 和执行 Fence 的相对顺序，要结合目标 CPU 的缓存、地址转换和中断状态设计。

真正的 Commit Point 是：

即将执行的第一条目标 S-mode 指令，必须已经受到目标 Domain 的正确保护策略约束。

### 15.7.2 为什么需要 Safe Switch Environment？

假设我们直接执行：

```
Current Domain = Linux

1. Restore OP-TEE satp
2. Restore OP-TEE GPR
3. Write OP-TEE mmpt
4. Return to S-mode
```

这种写法表面看起来没问题，但还需要考虑：

- 当前是否在受保护的 M-mode Stack 上执行？
- 恢复目标上下文过程中是否可能发生 M-mode Interrupt？
- 是否已经建立正确的 Trap Return Context？
- 是否有不属于目标 Domain 的敏感状态未清理？
- 是否存在对旧权限的缓存引用？
- 如果第 3 步失败，当前状态是否还能够安全回滚？

推荐的方法是将 Domain Switch 设计为一个可验证的状态机：

```
RUNNING_SOURCE
      |
      v
ENTER_ROOT
      |
      v
SAVE_SOURCE
      |
      v
VALIDATE_TARGET
      |
      v
INSTALL_PROTECTION
      |
      v
RESTORE_TARGET
      |
      v
RUNNING_TARGET
```

错误处理也必须由 RDSM 完成，而不是直接返回到一个已经部分修改的 S-mode Context。

这里需要注意：系统保护配置不一定能在失败后安全回滚。如果已经部分修改了跨 Hart 权限，可能必须终止目标 Domain 或使其进入不可执行的失败状态。

## 15.8 Domain Switch 的 CSR 级细节

假设从 Linux 切换到 OP-TEE。

### 15.8.1 切换之前

```
Hart 0

Privilege Mode: S

satp:
    Linux page table

mmpt:
    PPN  = Linux MPT Root
    SDID = Linux local SDID

stvec:
    Linux trap vector

sscratch:
    Linux per-hart scratch
```

### 15.8.2 `ecall` 进入 M-mode

在通常由 M-mode 处理的 S-mode `ecall` 路径上，处理器会更新：

- `mepc`
- `mcause`
- `mstatus` 中相关 Trap 状态
- PC → `mtvec` 所指定的入口

随后 OpenSBI Trap Handler 保存 GPR 并进入 C 语言 Trap Dispatcher。

真实源码可以从：

`lib/sbi/sbi_trap.c`

开始追踪 `sbi_trap_handler()` 和 `sbi_ecall_handler()`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 15.8.3 切换到 OP-TEE

RDSM 需要安装：

```
satp:
    OP-TEE page table

mmpt:
    PPN  = OP-TEE MPT Root
    SDID = OP-TEE local SDID

stvec:
    OP-TEE trap vector

sscratch:
    OP-TEE per-hart scratch
```

同时还要恢复 OP-TEE 之前的 `sepc`、GPR 和 Thread Entry 状态等信息。

不能只更新 `mmpt`，然后直接 `mret`。

否则即使内存保护已经切换到了 OP-TEE，CPU 仍然可能继续执行 Linux 的指令流。

### 15.8.4 切换完成后

```
Hart 0

Privilege Mode: S
Current Domain: OP-TEE

CPU Virtual Address Space:
    OP-TEE

MPT Physical Permission Space:
    OP-TEE

Supervisor Trap State:
    OP-TEE
```

这里可以看出：

- `satp` 解决 VA → PA 的转换。
- `mmpt` 解决 PA 的访问权限。
- RDSM 负责保证两者与目标 Domain 一致。

另外，`msdcfg` 不一定在所有 SoC 上都有非零字段。规范允许它在未实现相关可选 Supervisor Domain 扩展时读为零，因此不能假设每个 Domain 都必须依赖非零 `msdcfg` 才能正常工作。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 15.9 SBI Ecall 与 RDSM 的关系

普通 SBI 调用一般采用：

```
a7 = SBI Extension ID
a6 = SBI Function ID
a0..a5 = Arguments
ecall
```

Firmware 根据 EID/FID 进行分发。

这里有一个重要问题：

是否应该直接发明一个新的 SBI Extension，例如 SBI_EXT_SWITCH_TO_OPTEE？

从平台原型角度，私有 SBI Extension 可能很容易实现。

但从可移植性角度，这并不理想，因为：

- 私有接口不保证不同 SoC 之间兼容。
- Linux Driver 需要知道平台特定 EID。
- 不同平台可能产生不同 Calling Convention。
- RPC、Resume 和 Shared Memory 语义难以统一。

因此更合理的方向是：

将 RDSM 的 Domain Switching 能力与标准化 Transport/Service ABI 解耦。

例如：

```
Linux TEE Driver
        |
        v
Standardized Transport / Service ABI
        |
        v
OpenSBI / M-mode Service Dispatcher
        |
        v
RDSM Domain Manager
        |
        v
OP-TEE Domain
```

2026 年提出的 RPMI TEE Transport 正是朝着这一方向演进。后面的 Chapter 16 会详细研究它。

## 15.10 Memory Ownership 的动态管理

Runtime Domain Manager 除了切换 Hart，还必须管理物理内存所有权。

假设某一页面最初属于 Linux，随后被安全地转交给 OP-TEE。

不能简单执行：

```
mpt_allow(tee, page);
```

因为这样可能造成：

```
Linux: RW
OP-TEE: RW
```

如果目标是独占私有页面，这显然违反隔离要求。

正确思路应当是一个受控的 Ownership Transition：

```
HOST_OWNED
    |
    v
REVOKING_HOST
    |
    v
QUIESCED
    |
    v
SCRUBBED / VALIDATED
    |
    v
TEE_OWNED
```

### 15.10.1 撤销与授权为什么必须分阶段？

考虑以下竞态：

```
Hart 0: RDSM updates Host MPT

Hart 1: Linux continues writing old page

Hart 2: OP-TEE begins reading same page
```

即使 RDSM 已经在 Hart 0 上修改了 MPTE，也不代表 Hart 1 已经停止使用旧缓存权限。

因此安全的所有权迁移，至少需要：

1. 通过全局 Ownership Manager 锁定目标页面状态。
2. 阻止新的 Host 使用或共享申请。
3. 移除 Host 对该物理页面的相关权限。
4. 完成所有相关 Hart 的 MPT 同步。
5. 根据平台要求处理 In-flight DMA、设备缓存及其他访问路径。
6. 等待旧访问完成。
7. 清理或验证页面内容。
8. 建立 OP-TEE 的新权限。
9. 完成对应的 MPT 同步。
10. 将页面状态发布为 TEE-owned。

这里的核心是：

Revoke 必须先真正生效，Grant 才能被视为安全生效。

### 15.10.2 `MFENCE.PA` 并不是广播指令

规范定义的 `MFENCE.PA` 作用于执行它的 Hart。

如果其他 Hart 可能缓存了相关 MPT 状态，RDSM 必须通过额外的跨 Hart 协作机制，使这些 Hart 各自完成必要的同步。

可以借鉴 OpenSBI 已有的 IPI/RFENCE 基础设施，但要注意：

现有 SBI RFENCE 不能直接等价于 MPT Fence。

后者需要一个专门遵守 MPT 架构语义的实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 15.10.3 SDID 复用的问题

假设 Hart 0 曾经运行 Domain A：

```
SDID = 3
MPT  = A
```

Domain A 被销毁后，RDSM 为 Domain B 分配：

```
SDID = 3
MPT  = B
```

如果仍然存在属于旧 Domain A 的缓存权限状态，可能造成错误访问控制。

因此逻辑 Domain ID 和硬件 SDID 的生命周期必须分开管理。

`SDID` 复用需要相应的缓存失效及其他架构状态清理，不能简单地重新分配整数 3。

## 15.11 多 Hart 下的 RDSM Synchronization

推荐将同步分成三个层次。

| 层次                    | 管理对象                       | 典型实现                            |
| --------------------- | -------------------------- | ------------------------------- |
| Global Domain Lock    | Ownership、Domain Lifecycle | Spinlock / 状态机                  |
| Per-Hart Context Lock | Hart 当前执行归属                | Per-Hart State / IPI            |
| MPT Synchronization   | MPTE 更新和权限缓存               | MFENCE.PA + Remote Coordination |

### 一个简化的同步协议

```
Hart 0: RDSM Owner
    |
    +-- Change MPTE
    |
    +-- Send Sync Requests
    |       |
    |       +-- Hart 1 executes required MPT fence
    |       +-- Hart 2 executes required MPT fence
    |       +-- Hart 3 executes required MPT fence
    |
    +-- Wait for acknowledgements
    |
    +-- Verify device-side revocation if relevant
    |
    +-- Publish new ownership
```

对于安全敏感的 Revoke，不能只等待 IPI 被发送。

要等待足以证明旧权限不能继续被使用的完成事件。

而对共享 Page，如果存在设备直接访问，还必须在 CPU Hart 之外处理 I/O 权限和正在进行的 DMA。

## 15.12 RDSM 与 OpenSBI 的推荐软件分层

我更推荐将 SMMPT 支持抽象成几个逻辑模块，而不是在现有 `sbi_domain_context.c` 中直接塞入大量平台特定代码。

```
OpenSBI / RDSM
|
+-- SBI Interface
|    +-- Ecall Dispatch
|    +-- MPXY / RPMI Service Integration
|
+-- Domain Manager
|    +-- Domain Lifecycle
|    +-- Domain Policy
|    +-- Hart Assignment
|
+-- Context Manager
|    +-- GPR / CSR State
|    +-- FP / Vector State
|    +-- Domain Switch
|
+-- Memory Protection Manager
|    +-- MPT Allocator
|    +-- MPT Mapping
|    +-- Ownership Manager
|    +-- MPT Synchronization
|
+-- Platform Security Backend
     +-- PMP / Smepmp
     +-- I/O Protection
     +-- Interrupt Domain
     +-- RSS / RoT Interface
```

其中 Memory Protection Manager 应提供统一接口，例如：

```
struct rdsm_mpt_ops {
    int (*create)(struct rdsm_domain *dom);

    int (*grant)(struct rdsm_domain *dom,
                 uint64_t pa,
                 uint64_t size,
                 uint32_t perms);

    int (*revoke)(struct rdsm_domain *dom,
                  uint64_t pa,
                  uint64_t size);

    int (*sync)(struct rdsm_domain *dom,
                uint64_t pa,
                uint64_t size);
};
```

这只是参考的软件抽象。真正实现时还需要定义错误码、权限策略、原子事务、内存所有权、同步责任、回滚规则等。

推荐尽量将硬件相关细节与上层 Domain Lifecycle 分离。

这样既有利于维护，也能在不同 SoC 上使用不同的保护后端，例如早期采用 PMP，具备相应硬件后改为 SMMPT。

但不能假设两个后端在粒度、性能、扩展能力和安全语义上完全一致。

## 15.13 Debug：Domain Switch 后为什么可能立即异常？

假设 Linux 调用 OP-TEE，RDSM 已经恢复了 OP-TEE 的 PC 和 SP，但执行第一条指令就发生 Access Fault。

我建议按以下顺序定位：

| 排查项                  | 可能原因              |
| -------------------- | ----------------- |
| `mepc`               | 目标 PC 是否正确        |
| `mstatus.MPP`        | 是否即将进入 S-mode     |
| `mmpt`               | 是否装载了 OP-TEE MPT  |
| `satp`               | 是否装载了 OP-TEE 地址空间 |
| MPTE                 | 对应 Code 页是否允许执行   |
| PMP/Smepmp           | 是否存在额外权限拒绝        |
| `stvec` / `sscratch` | 后续 Trap 是否进入正确上下文 |
| Fence State          | 是否仍使用旧的权限缓存       |
| Hart Context         | 是否恢复了错误的 Domain   |
| Image / Relocation   | 代码物理地址与实际加载地址是否一致 |

必须记住：MPT 不负责 VA 到 PA 的转换，所以仅看到 MPT 允许某一物理页执行，并不足以证明 `satp` 映射是正确的。

## 15.14 Chapter 15 — Staff Engineer 级测试题

1. Architecture Mapping： OpenSBI 已有 `sbi_domain`，为什么不能直接宣称支持了 SMMPT Supervisor Domain？请从硬件权限、Context Switch、SDID 和 Dynamic Memory Management 四个方面说明。
2. Context Isolation： 两个 Hart 同时运行 OP-TEE Domain，但分别执行不同 Trusted Thread。应该有多少个 Domain Context、Hart Context 和 OP-TEE Thread Context？说明它们之间的生命周期关系。
3. Source Analysis： 阅读 `switch_to_next_domain_context()`，分析当前 PMP-based Domain Switching 已保存哪些状态。如果改为 SMMPT，必须增加哪些功能？哪些原有功能仍然需要保留？
4. SDID Reuse： 假设某 Hart 的 `SDIDLEN=2`，但系统管理了 12 个逻辑 Domain。是否意味着最多只能创建四个 Domain？如何设计 SDID 分配与复用策略？
5. Security Race： Hart 0 撤销 Linux 对某个物理页的写权限，但 Hart 1 仍然运行 Linux。只在 Hart 0 执行 `MFENCE.PA` 是否足够？如果不够，设计完整同步协议。
6. Switch Failure： 在 Domain Switch 中，新的 `satp` 已被装载，但是新的 `mmpt` 配置失败。系统应该恢复源 Domain、终止目标 Domain，还是直接 Panic？给出安全决策条件。
7. SBI Architecture： 为什么不推荐把 OP-TEE 入口简单封装成私有 SBI EID？如果使用标准化 RPMI TEE Service，RDSM 与 Transport Driver 的职责边界应该如何划分？
8. Interrupt Security： Linux Domain 切换到 OP-TEE 的过程中，某个 Linux 设备中断到达。如何防止该中断在错误的 Supervisor Context 中被处理？请结合 `sie`、`sip`、`stvec` 和 Interrupt Domain 进行设计。
9. Performance： 每次 TEE 调用都全局执行 MPT Fence 是否必要？尝试设计基于 Domain Generation、SDID、Per-Hart Cache State 的优化方案，并给出正确性证明条件。
10. TCB Review： 如果一个共享的 M-mode RDSM 同时负责 Linux 与 OP-TEE 的所有内存分配和上下文切换，它会成为整个系统的安全关键组件。如何减少其代码量、攻击面、复杂度和信任依赖？

通过标准： 能够画出完整的 M-mode Domain Switch 时序图，并且对每一次 CSR 更新、内存权限变化、同步操作给出安全性解释。

# Chapter 16 — OP-TEE 与 SMMPT 的完整集成设计

OP-TEE Porting · RISC-V MMU · SBI MPXY · RPMI TEE · Trusted Threads · RPC · SMP

这是 PART IV 最重要的一章。

前两章回答了如何建立 Domain，以及如何在两个 Domain 之间进行切换。

但是到目前为止，我们还没有解决最核心的工程问题：

OP-TEE 本身应该做什么？OpenSBI/RDSM 应该做什么？它们如何配合，又该如何避免把原本已经成熟的 OP-TEE 架构重新设计一遍？

我的建议是采用一个重要原则：

> 尽量保留 OP-TEE 原有的 Thread、Session、TA、MMU、RPC 和 Shared Memory 架构，把 Supervisor Domain 隔离交给可信 M-mode Firmware，双方只通过清晰的调用 ABI 与内存授权机制协作。

这将是本章的设计主线。

## 16.1 首先明确 OP-TEE 的执行模式

OP-TEE 在 Arm TrustZone 平台通常运行于 Secure World。

而 RISC-V 没有与 Arm Security State 完全等价的传统硬件划分。

因此，RISC-V OP-TEE 的部署方式需要根据平台保护能力决定。

典型选择包括：

| 模式                     | 特征                              | 适用情形              |
| ---------------------- | ------------------------------- | ----------------- |
| M-mode OP-TEE          | Trusted OS 本身处于 M-mode          | 简化或特定平台方案         |
| S-mode OP-TEE + PMP 隔离 | M-mode Firmware 管理独立 Domain     | PMP 资源能够满足隔离需求的系统 |
| S-mode OP-TEE + SMMPT  | RDSM 管理 MPT 和 Supervisor Domain | 需要细粒度、动态内存隔离的系统   |

本章以第三种模式为目标。

其核心条件是：

OP-TEE 运行在 S-mode，但它并不与 Linux 共享同一个物理内存权限视图。

这使 Linux 和 OP-TEE 在同一个 RISC-V Privilege Level 下，仍然可以建立不同的信任边界。

## 16.2 完整的软件架构

Linux User Space

Client Application · libteec · TEE Client API

Linux Kernel · Host Domain

TEE Driver · OP-TEE Driver · Transport

SBI MPXY / RPMI TEE · Proposed Standard Transport

M-mode · OpenSBI / RDSM

Service Dispatch · Domain Switch · MPT Manager

S-mode · OP-TEE Domain

OP-TEE Core · Trusted Thread · TA Manager

U-mode Trusted Applications

SMMPT 在 Hart 的物理访问路径上强制执行 Domain 权限；并不是上图中另外一个可调用的软件层。

需要明确：上图中的 Linux RPMI TEE Backend 与 RDSM 集成是正在演进的方案，不能直接认为所有上游 OpenSBI/OP-TEE 版本均已实现。

不过这个分层揭示了我们真正需要解决的三个接口：

1. Linux 如何向 OP-TEE 发起请求？
2. RDSM 如何完成可信 Domain Switching？
3. OP-TEE 如何在自己的 Domain 中继续原有的 Thread/TA 执行模型？

## 16.3 从 OP-TEE 现有 RISC-V 源码开始

建议首先建立以下源码索引。

| 源码位置                                         | 研究目标                             |
| -------------------------------------------- | -------------------------------- |
| `core/arch/riscv/kernel/entry.S`             | RISC-V Boot Entry、Stack、Hart 初始化 |
| `core/arch/riscv/kernel/boot.c`              | Boot、SBI、Secondary Hart、Core 初始化 |
| `core/arch/riscv/mm/core_mmu_arch.c`         | Sv Page Table、PTE、`satp`         |
| `core/arch/riscv/include/mm/core_mmu_arch.h` | RISC-V MMU 参数                    |
| `core/mm/`                                   | 通用 Memory Management             |
| `core/kernel/`                               | Trusted Thread 与运行时管理            |
| `core/tee/`                                  | TA 和 OP-TEE 服务逻辑                 |

这些是实际存在的 OP-TEE 源码组织。RISC-V Architecture Port 也已经包含 MMU、SBI 和多 Hart 初始化方面的实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+3



### 16.3.1 第一处代码：`entry.S`

从 `_start` 开始。

在当前 RISC-V Boot 代码中，可以看到：

- 使用 `a0` 接收 Hart ID，`a1` 接收 Device Tree Address。
- 初始化 `gp`。
- 使用 Atomic Operation 选出 Primary Hart。
- 通过 Scratch Register 保存 Hart Index。
- 分别进入 Primary/Secondary Boot Path。

其中 Primary Hart 随后会：

```
reset_primary
    |
    +-- Initialize BSS
    |
    +-- Initialize Temporary Stack
    |
    +-- Save Boot Arguments
    |
    +-- Initialize SBI-related support
    |
    +-- Initialize Boot Memory
    |
    +-- core_init_mmu_map()
    |
    +-- enable_mmu()
    |
    +-- OP-TEE Core Initialization
```

这是从真实 `entry.S` 抽象出的调用流程，而不是虚构一个新的 Boot Loader。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



注意这里的 `core_init_mmu_map()`。

它引出本章一个非常重要的问题：

OP-TEE 已经拥有自己的 MMU Page Table 管理，那么接入 SMMPT 后是不是需要把原来的 MMU 逻辑全部替换？

答案是：不需要，而且不应该这样做。

## 16.4 OP-TEE MMU 与 SMMPT 的职责边界

### 16.4.1 二者保护不同的东西

OP-TEE MMU 负责：

- Core Virtual Address 到 Physical Address 的转换。
- Trusted Application 的 User Address Space。
- OP-TEE Code/Data/Stack 的虚拟内存权限。
- 不同 TA Context 之间的页表隔离。

SMMPT 负责：

- 当前 Supervisor Domain 对物理地址的 R/W/X 权限。
- 保护 OP-TEE Private Physical Memory 不被 Host Domain 访问。
- 在内存所有权改变时，由 RDSM 更新跨域权限。

可以用下面的访问路径理解：

```
OP-TEE Trusted Thread
         |
         v
VA = 0xFFFF_...
         |
         v
OP-TEE satp / Sv39-Sv48
         |
         v
PA = 0x9000_1000
         |
         v
OP-TEE Domain MPT
         |
         +-- Allowed ---> Physical Memory
         |
         +-- Denied ----> Access Fault
```

这里：

\\[ Allowed = MMU\_{Allow}\land MPT\_{Allow}\land OtherProtection\_{Allow} \\]

如果 OP-TEE 的 `satp` 建立了某个物理地址映射，但其当前 Domain MPT 不允许访问，最终仍然会被拒绝。

反过来，如果 MPT 允许访问某个物理地址，但 OP-TEE 当前页表没有有效映射，访问依然无法成功。

### 16.4.2 从真实 PTE 代码理解

当前 RISC-V OP-TEE 使用类似下面的结构表达页表：

```
struct mmu_pte {
    unsigned long entry;
};

struct mmu_pgt {
    struct mmu_pte entries[RISCV_PTES_PER_PT];
};
```

实际实现中还有 `mmu_partition` 用于管理 Root Page Table、Page Table Pool、用户页表与 ASID。

关键函数包括：

- `core_mmu_entry_is_valid()`
- `core_mmu_entry_is_leaf()`
- `core_mmu_pte_create()`
- `core_mmu_ptp_create()`
- `core_mmu_pgt_to_satp()`

其中 `core_mmu_pgt_to_satp()` 会根据 Root Page Table 的 PPN、ASID 和 MMU Mode 构造 `satp`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这些函数管理的是 RISC-V MMU PTE，而不是 SMMPT MPTE。

尽管二者都采用页表形式，但语义完全不同。

| 对比    | OP-TEE MMU PTE  | SMMPT MPTE           |
| ----- | --------------- | -------------------- |
| 地址索引  | Virtual Address | Physical Address     |
| 地址转换  | 是               | 否                    |
| 权限范围  | 当前虚拟地址空间        | 当前 Supervisor Domain |
| 管理者   | OP-TEE MMU      | RDSM                 |
| 核心寄存器 | `satp`          | `mmpt`               |
| 同步机制  | `SFENCE.VMA` 等  | `MFENCE.PA` 等        |

因此我的设计建议是：

不要把 SMMPT 的 MPTE 管理直接整合进 OP-TEE `core_mmu_arch.c`。

应该在 RDSM 中建立独立的 MPT Manager。

OP-TEE 需要的是受控的内存授权服务接口，而不是直接操作 `mmpt`。

## 16.5 OP-TEE 内存区域应该如何映射？

我们可以将 OP-TEE 需要管理的内存划分为三类。

### A. TEE Private Memory

包括：

- OP-TEE Core Code/Data。
- Kernel Heap 和 Stack。
- TA Code/Data。
- Trusted Thread Context。
- Trusted Key Material。
- 运行时安全状态。

这些物理页面对 Linux Domain 默认 Deny。

同时，OP-TEE 内部仍然利用 `satp` 建立不同 TA 之间的隔离。

也就是说：

```
Domain Isolation:
    Linux 不能访问 OP-TEE Private Memory

TA Isolation:
    TA-A 不能访问 TA-B Private Virtual Memory
```

第一层由 SMMPT 等跨域保护机制承担，第二层由 OP-TEE 自己的 Kernel、MMU 与 TA 生命周期管理承担。

### B. Shared Memory

用于：

- OP-TEE Message Arguments。
- Linux ↔ OP-TEE 参数传递。
- RPC 数据交换。
- 需要共享的 Input/Output Buffer。

常见访问策略：

```
Linux MPT:
    Shared Memory = RW

OP-TEE MPT:
    Shared Memory = RW
```

但 Shared Memory 不应该包含需要维持机密性的 TA Private Key、Secure Stack 等对象。

### C. Device Memory

例如：

- TEE 专用 UART。
- Secure Storage Controller。
- Crypto Accelerator。
- 安全设备 MMIO。
- 受控的 Mailbox。

这些 MMIO 区域可以由 MPT 限制 CPU 访问；如果设备具备 DMA，还需要配合 IOMMU 或 I/O MPT Checker。

重点是不能因为 CPU 访问被保护，就推断 DMA 也已经被保护。

## 16.6 一个完整的 Linux → OP-TEE 调用

现在进入本章最重要的实战。

假设 Linux 用户态应用执行：

```
TEEC_InvokeCommand(&session,
                   command_id,
                   &operation,
                   &origin);
```

我们的目标是将整个调用过程从用户态一直追踪到 OP-TEE Trusted Application。

### 16.6.1 第一个阶段：Linux User Space

应用调用 libteec。

libteec 通过 Linux TEE Framework 访问 OP-TEE Driver。

概念上的调用路径：

```
Client Application
      |
      v
libteec
      |
      v
/dev/tee*
      |
      v
Linux TEE Subsystem
      |
      v
OP-TEE Driver
```

在这一阶段，Linux 负责管理 Normal World 的 Session Handle、Buffer，以及和 TEE Driver 的 ioctl 通信。

但它尚未进入 OP-TEE Domain。

### 16.6.2 第二个阶段：调用 Transport

这部分必须区分现有方案和新的演进方向。

在当前 RISC-V OP-TEE 源码中，能够看到诸如 `SBI_EXT_TEE` 这样的平台 TEE SBI 接口定义，以及与 M-mode Security Manager 配合的代码路径。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



这是一个可以分析的现有实现基础，但它不能直接视为所有平台统一支持的标准 TEE ABI。

更值得关注的是 2026 年提出的 RPMI TEE 方案。

### 16.6.3 2026 年的 RPMI TEE 方案

这里需要说明一下技术演进：

- 2026 年 9 月的 RFC v1 尝试将 OP-TEE 原有类似 SMC 的寄存器调用通过 RPMI TEE_CALL 传递。
- 2026 年 10 月的后续 RFC 改为独立的 RPMI TEE Transport 与 OP-TEE Backend。
- 2026 年 10 月 13 日的 RFC v3 采用固定宽度的控制消息、Memory Parcel、Resume Token 等机制，已经不再简单地封装原来的 `a0–a7` 寄存器参数。

v3 的说明还明确指出：对应的 OP-TEE OS 支持已在本地实现，但 OpenSBI 集成和端到端验证仍待完成。因此不能把它当成已经完成的生产级上游调用链。

[image](https://www.google.com/s2/favicons?domain=https://lists.trustedfirmware.org\&sz=32)

lists.trustedfirmware.org



我建议以 v3 的职责划分作为架构分析对象。

```
Linux OP-TEE Backend
        |
        v
RPMI TEE Service Driver
        |
        v
RPMI TEE Transport
        |
        v
SBI MPXY
        |
        v
M-mode Firmware / RPMI Framework
        |
        v
RDSM / TEE Endpoint
```

其中：

- MPXY 提供 SBI 消息代理机制。
- RPMI TEE Transport 负责服务发现、调用和内存共享等通信基础能力。
- OP-TEE Backend 管理 OP-TEE 相关的协议和会话。
- RDSM 负责可信执行环境之间的切换和安全策略。

这种分层有利于在不同 TEE 实现之间复用底层 Transport。

## 16.7 从 `ecall` 到真正的 Domain Switch

为了理解核心机制，我们先暂时忽略 RPMI 消息的具体字节布局。

下面展示一个 同 Hart 同步调用模型。它是理解 TEE Domain Switching 的有效参考场景，但具体平台是否采用该模型，还需要由选定的 Transport ABI 与 Firmware 实现确认。

```

Linux (S-mode)
     |
     | 1. Prepare TEE request
     |
     | 2. SBI MPXY / ecall
     v
M-mode Firmware
     |
     | 3. Authenticate and validate request
     |
     | 4. Save Linux Hart Context
     |
     | 5. Install OP-TEE protection state
     |
     | 6. Restore OP-TEE Hart Context
     |
     | 7. Trap return to S-mode
     v
OP-TEE (S-mode)
     |
     | 8. Dispatch Trusted Thread
     |
     | 9. Process TA request
     |
     | 10. Return or Yield
     v
M-mode Firmware
     |
     | 11. Save OP-TEE Context
     |
     | 12. Restore Linux protection state
     |
     | 13. Restore Linux Hart Context
     v
Linux (S-mode)

```

需要特别注意第 5 步。

它不只是简单的：

```
csrw 0x382, a0
```

真正的 Domain Switch 还必须保证：

- 目标 MPT 是可信的。
- 对旧权限的撤销已经完成必要的同步。
- 当前 Hart 不再使用错误的 Domain Translation State。
- Interrupt 和 Supervisor State 已正确切换。
- 当前 Hart 的目标 Context 已经准备好。
- `mret` 的目标地址和权限状态一致。

只有这样才能形成完整的安全边界。

## 16.8 OP-TEE Trusted Thread 如何接入？

这里需要区分三个层次。

### 16.8.1 Domain Context

由 RDSM 管理。

负责：

```
Linux Domain <-> OP-TEE Domain
```

### 16.8.2 Trusted Thread Context

由 OP-TEE 管理。

负责：

```
OP-TEE Thread 0
OP-TEE Thread 1
OP-TEE Thread 2
...
```

### 16.8.3 TA Context

负责 OP-TEE 内部的 User TA 地址空间、Session、执行状态与资源管理。

因此，完整的执行关系应该是：

```
RDSM switches to OP-TEE Domain
        |
        v
OP-TEE resumes Trusted Thread
        |
        v
Trusted Thread identifies Session
        |
        v
TA Manager selects Trusted Application
        |
        v
TA executes in its user context
```

这意味着 SMMPT 不应该介入每一次 Trusted Thread 的内部调度。

例如：

```
OP-TEE Thread 0 -> OP-TEE Thread 1
```

只要仍然处于同一个 Supervisor Domain，通常不需要因为切换了 OP-TEE Thread 就重新配置整个 Domain 的 MPT。

不过 `satp` 和 TA 用户态上下文可能需要按照 OP-TEE 原有的调度机制更新。

这正是 Domain Context 与 Thread Context 分离带来的优势。

## 16.9 如何处理 RPC？

在 OP-TEE 中，Trusted Application 的执行不一定能够完全在 Secure Domain 内完成。

例如 TA 需要访问 Normal World 文件系统。

这时候 OP-TEE 可能需要通过 `tee-supplicant` 协助执行操作。

概念性流程：

```
OP-TEE TA
    |
    v
OP-TEE Core
    |
    | RPC Request
    v
RDSM / Transport
    |
    v
Linux OP-TEE Driver
    |
    v
tee-supplicant
    |
    | Perform REE FS operation
    v
Linux OP-TEE Driver
    |
    v
RDSM / Transport
    |
    v
Resume OP-TEE Thread
```

这里的重点是：

一次 TA Command 不一定只经历一次 Linux → OP-TEE Domain Switch。

如果它包含多次 RPC，就可能发生多次跨域调度。

因此设计时需要考虑：

- 可信 Thread Context 是否能够挂起和恢复。
- RPC 参数如何在 Shared Memory 中表示。
- Host 是否能够伪造 RPC Response。
- 原始 Session 和 Pending Request 如何关联。
- Resume Token、RPC Cookie 或等价上下文标识如何防止混淆。
- 超时、取消、Host 崩溃后如何清理挂起状态。

新提出的 RPMI TEE Backend 就将 Memory Parcel、RPC 和 Resume Token 作为重要组成部分。

[image](https://www.google.com/s2/favicons?domain=https://lists.trustedfirmware.org\&sz=32)

lists.trustedfirmware.org



### 16.9.1 一次具体的 TA 文件操作

例如：

```
TA wants to read an object
       |
       v
OP-TEE Secure Storage Layer
       |
       v
REE FS RPC
       |
       v
Linux tee-supplicant
       |
       v
Linux Filesystem
```

这里 Linux 可以负责实际的数据持久化。

但如果安全目标是保护存储内容的机密性和完整性，就不能直接信任 Linux 文件系统返回的数据。

相应的保护应由 OP-TEE Secure Storage 的密码学和元数据机制承担。

SMMPT 保护的是运行期间跨 Domain 的物理访问权限，它不能替代这些存储安全机制。

## 16.10 Shared Memory 的安全管理

这是 OP-TEE + SMMPT 集成中最容易发生架构错误的地方。

假设 Linux 希望向 OP-TEE 传递一个 4KB Buffer。

最简单的做法是双方映射同一物理页：

```
Linux VA
   |
   v
PA = 0x9400_0000
   ^
   |
OP-TEE VA
```

从地址转换角度看，这没有问题。

但仍然需要考虑：

- Linux 是否可以在 OP-TEE 读取过程中修改内容？
- OP-TEE 是否需要将部分数据复制到 Private Memory？
- OP-TEE 如何确认 Buffer 的长度及物理来源？
- Buffer 是否跨越多个物理页？
- Buffer 能否被其他 Linux 进程同时修改？
- 在 OP-TEE 尚未使用完 Buffer 之前，能否回收？
- Buffer 是否可能包含不允许的 MMIO 或 Root-owned 地址？

### 16.10.1 为什么不能只检查地址范围？

例如：

```
if (pa >= SHM_START && pa < SHM_END)
    return true;
```

这个检查不足以保证一个 Buffer 安全。

因为还需要验证：

1. `pa + size` 是否溢出。
2. 整个 Buffer 是否都在允许的共享区域内。
3. 是否跨越多个不连续的物理页面。
4. 内存类型是否允许普通 CPU 访问。
5. Shared Memory 的 Ownership 是否有效。
6. 是否已经完成必要的共享授权。
7. Buffer 的生命周期是否仍然有效。
8. 是否存在并发撤销。

一个更合理的抽象接口是：

```
struct shared_memory_ref {
    uint64_t object_id;
    uint64_t offset;
    uint64_t size;
};

int validate_shared_memory(
    const struct shared_memory_ref *ref,
    uint32_t required_access);
```

这里使用 Object ID 而不是直接相信任意 Host PA，是为了将 Shared Memory 的身份、授权和生命周期放在可信管理机制中。

对于 RPMI TEE 的 Memory Parcel 方案，可以利用 Parcel Identity、Nonce/Generation 和访问权限描述实现类似管理。

### 16.10.2 Memory Parcel 的设计价值

在 2026 年的 RPMI TEE Transport 设计中，Memory Parcel 支持类似：

```
Create / Share / Lend / Reclaim
```

这样的内存共享生命周期。

它的价值不只是方便传输地址，而是为内存所有权和共享关系提供可以管理的对象。

例如：

```
HOST_OWNED
    |
    v
PARCEL_CREATED
    |
    v
SHARED_WITH_TEE
    |
    v
IN_USE
    |
    v
RECLAIM_PENDING
    |
    v
HOST_OWNED
```

但应特别注意：

Memory Parcel 协议对象并不自动意味着底层硬件 MPT 权限已经正确更新。

真正的平台实现必须让 Parcel Lifecycle 与 RDSM 的 MPT 更新保持一致，否则可能出现软件认为共享已经结束、硬件却仍然保留访问权限的情况。

## 16.11 动态授权接口应该放在哪里？

我建议采取以下设计原则：

OP-TEE 请求资源，RDSM 决定是否授权并修改 MPT。

也就是说，不建议在 OP-TEE 中直接加入：

```
/* This is not a proposed OP-TEE API */
void optee_write_mmpt_csr(...);
```

因为 OP-TEE 运行在 S-mode，`mmpt` 是 M-mode CSR。

更合理的是：

```
OP-TEE
    |
    | Request a protected memory operation
    v
Trusted RDSM Service
    |
    +-- Check Domain Identity
    |
    +-- Validate Ownership
    |
    +-- Validate Range and Permissions
    |
    +-- Update MPT
    |
    +-- Synchronize Relevant Harts/Devices
    |
    v
Return Result
```

一个参考性的软件接口可以是：

```
enum rdsm_mem_operation {
    RDSM_MEM_SHARE,
    RDSM_MEM_LEND,
    RDSM_MEM_RECLAIM,
};

struct rdsm_mem_request {
    uint64_t object_id;
    uint64_t target_domain;
    uint64_t offset;
    uint64_t size;
    uint32_t permissions;
};

int rdsm_memory_operation(
    enum rdsm_mem_operation operation,
    const struct rdsm_mem_request *request);
```

这段代码不是已经标准化的 SBI Interface，只是表达需要的能力。

真正接入时，应该优先复用选定的标准 Transport 和 Memory Parcel 语义，避免同时维护两套互不兼容的 Ownership Protocol。

同时，RDSM 不能因为请求来自 OP-TEE 就无条件接受。

即使是可信 Domain，也必须检查：

- 请求资源的当前 Owner。
- 目标 Domain 是否存在。
- 授权是否合法。
- 是否存在冲突的共享关系。
- 是否涉及 Root-owned Memory。
- 是否正在销毁或回收。
- 是否满足最小权限原则。

## 16.12 OP-TEE SMP 与 SMMPT 的协作

假设有 16 个 Hart：

```
Hart 0: Linux
Hart 1: Linux
Hart 2: OP-TEE
Hart 3: Linux
...
```

在 SMMPT 架构下，同一个 Hart 可以在不同时间执行不同 Domain，但不能把这件事与 OP-TEE 内部多核支持混为一谈。

### 16.12.1 OP-TEE 的 Secondary Hart 初始化

实际 OP-TEE RISC-V 源码中可以看到：

`core/arch/riscv/kernel/boot.c`

包含 `boot_start_secondary_cores()`。

S-mode 配置下，它利用 SBI HSM 查询 Hart Status，并对符合条件的 Hart 执行 Hart Start。对于多 Hart 的 RISC-V SBI 场景，源码也检查 RFENCE 扩展能力，以支持远端地址转换缓存同步。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这提供了重要的工程基础。

但是必须将 HSM 和 SMMPT Domain Membership 联系起来考虑。

### 16.12.2 一个危险场景

假设：

```
Hart 2 currently belongs to Linux
```

OP-TEE 直接请求：

```
HSM Hart Start(Hart 2)
```

如果 RDSM 没有进行 Domain Ownership 检查，就可能让 OP-TEE 不受控地改变其他 Domain 的执行资源。

因此，推荐由 RDSM 根据明确的 Hart Assignment Policy 决定是否允许相应操作。

对于同一 Hart 的跨域调用，也不能简单套用 HSM Hart Start：HSM 解决 Hart 生命周期，Domain Switch 解决同一个运行中 Hart 的信任域转换，两者职责不同。

### 16.12.3 Per-Hart MPT Synchronization

假设 Hart 0 与 Hart 2 都可以执行 OP-TEE。

RDSM 修改了 OP-TEE 的 MPT：

```
TEE MPT:
    Page X: RW -> Deny
```

即使 Hart 0 已经完成 MPT Fence，Hart 2 仍可能保留旧的访问权限缓存。

因此在权限撤销完成之前，必须确保所有可能使用旧映射的 Hart 都已经完成对应的同步。

如果 Hart 2 处于暂停状态，也需要确保它下次恢复时不会使用旧权限状态。

这个问题不能只依赖 OP-TEE 的普通 `SFENCE.VMA` 解决。

## 16.13 Interrupt 与 RPC 的协同

在 Domain Switch 中，中断问题实际上可以分成三类。

### 类型一：Root / M-mode Interrupt

由 M-mode Firmware 或相关安全控制逻辑处理。

例如可能影响整个 SoC 的平台安全事件。

### 类型二：OP-TEE Domain Interrupt

由允许在 OP-TEE Domain 内处理的 Interrupt Source 发起。

需要通过可信的 Interrupt Domain 配置保证路由正确。

### 类型三：Host Domain Interrupt

例如 Linux 管理的普通 Device Interrupt。

如果当前 Hart 正在 OP-TEE Domain 执行，不能简单让它通过 OP-TEE 当前的 `stvec` 当作安全中断处理。

正确策略取决于平台的 Interrupt Architecture，可以选择：

- 将中断保持 Pending。
- 通过 RDSM 安全地退出或 Yield 到 Host Domain。
- 使用支持 Supervisor Interrupt Domain 的硬件机制进行路由或隔离。

如果正在执行的是 OP-TEE Trusted Thread，还要考虑是否需要保存和恢复其执行状态。

对于这种场景，我建议设计两个独立的 State Machine：

```
RDSM Domain State
    |
    +-- HOST
    +-- TEE
    +-- SWITCHING
```

```
OP-TEE Thread State
    |
    +-- FREE
    +-- ACTIVE
    +-- SUSPENDED
    +-- RPC_WAIT
```

这样可以避免把“Domain 不在运行”和“Trusted Thread 处于 Suspend”视为同一件事情。

## 16.14 MPT Fault 如何处理？

假设 Linux 故意执行：

```
volatile uint64_t *p =
    (uint64_t *)0x90000000;

uint64_t value = *p;
```

假设 Linux 已经建立了相应的 VA 映射，且实际访问的 PA 为 OP-TEE Private Memory。

硬件 MPT Checker 发现当前 Host Domain 没有该物理地址的 Read Permission。

根据 SMMPT 的访问控制规则，将产生对应访问类型的 Access Fault；Supervisor Domain 访问违反 MPT 时应由 M-mode RDSM 接收和处理该安全事件。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



推荐 RDSM 的处理步骤：

```
M-mode Trap
    |
    +-- Determine Faulting Hart
    |
    +-- Identify Current Domain
    |
    +-- Capture Trap Information
    |
    +-- Classify Access Fault
    |
    +-- Check MPT and Other Protection State
    |
    +-- Apply Security Policy
    |
    +-- Log / Terminate / Report / Recover
```

需要特别注意两个问题。

第一，Access Fault 不一定就是 MPT Fault。

PMP、PMA、Bus Error 等其他机制同样可能产生 Access Fault。

所以调试时必须结合当前 MPT、PMP、PA、Trap State 和可能的实现专用诊断寄存器，而不能只看到 `mcause` 就断定是 SMMPT。

第二，RDSM 不应该不加判断地将安全违规重新委托给 Linux。

对于真正的跨域访问违规，默认应采取符合平台安全策略的处理，例如拒绝访问、记录并终止违规 Domain，或者进入受控恢复流程。

这应当与普通的 OS Page Fault 分开设计。

## 16.15 OP-TEE 集成的参考模块边界

从可维护性和上游贡献角度出发，我建议将代码改动分成以下部分。

| Component             | 推荐改动                           | 是否应直接操作 MPT |
| --------------------- | ------------------------------ | ----------- |
| OpenSBI / RDSM        | Domain、MPT、Ownership、Switching | 是，可信路径      |
| OP-TEE RISC-V Entry   | Boot ABI、Domain Entry/Return   | 否           |
| OP-TEE Thread         | 跨域 Yield/Resume 适配             | 否           |
| OP-TEE Core MMU       | 保留并完善原有 Sv Page Table          | 否           |
| OP-TEE Shared Memory  | 验证和跟踪共享对象                      | 通过 RDSM     |
| Linux OP-TEE Driver   | 适配 Transport/Service ABI       | 否           |
| RPMI TEE Transport    | 消息、Parcel、信号与调用                | 不直接承担策略     |
| I/O Protection Driver | 设备访问权限配置                       | 根据可信管理架构    |

理想情况下，OP-TEE 不需要知道 Smmpt43 中每个 MPTE 的具体编码。

它只需要知道：当前内存区域能否被合法共享、是否可以访问、如何撤销，以及相关操作是否成功。

这样能够将复杂的硬件隔离实现约束在 Firmware 和 Hardware Backend 内部。

## 16.16 建议采用分阶段的集成策略

真正实现时，不建议第一阶段就同时引入动态 MPT、SMP、DMA、RPMI Parcel 和全部 RPC 特性。

我建议分为五个可验证的阶段。

Phase 1

Boot 与静态隔离

一个 Hart，静态 TEE/Host MPT；验证两边正常启动及非法访问被拒绝

Phase 2

最小跨域调用

实现一条可重复的 Linux → RDSM → OP-TEE → Linux 请求/返回路径

Phase 3

Shared Memory 与 RPC

参数验证、Session、TA Invoke、RPC Yield/Resume，补充共享权限生命周期

Phase 4

SMP 与动态 MPT

多 Hart、远端 MPT 同步、SDID 复用、内存 Revoke、竞态测试

Phase 5

SoC 安全完整性

DMA/IOMMU、Interrupt Domain、Warm Boot、Reset、压力及安全回归

每个阶段都有独立验收标准。

例如 Phase 1 的最低标准应该是：

1. Linux 能完整启动。
2. OP-TEE 能完整初始化。
3. Linux 无法读取或写入 TEE Private Memory。
4. Linux 无法修改 MPT。
5. OP-TEE 可以访问自己的 Code、Data 和 Stack。
6. 两个 Domain 都能访问经过明确授权的 Shared Memory。
7. 故意构造的非法访问能被可信 Firmware 正确处理。

达到这些条件以后，再推进动态 Memory Ownership 和复杂的 RPMI TEE 交互，调试难度会明显降低。

## 16.17 从源码入手的推荐阅读路径

建议按照下面的顺序进行真实代码阅读：

Step 1 — OP-TEE Boot

`entry.S` → `boot.c` → `core_init_mmu_map()` → `enable_mmu()`

Step 2 — OP-TEE Memory Management

`core_mmu_arch.c` → `core/mmu` 相关实现 → TA 页表切换

Step 3 — OpenSBI Domain

`sbi_domain.c` → `sbi_domain_context.c` → `sbi_hart_protection_reconfigure()`

Step 4 — Trap / Domain Switch

`sbi_trap.c` → `sbi_ecall_handler()` → Context Switch → `sbi_hart_switch_mode()` / Trap Return

Step 5 — TEE Transport

Linux OP-TEE Driver → RPMI TEE RFC → SBI MPXY → Firmware Service Dispatcher

Step 6 — SMMPT Backend

MPT Allocator → MPTE Update → `mmpt` → `MFENCE.PA` → Remote Synchronization

这里我特别建议每次研究一个函数时，记录四件事情：

- 它运行在哪个 Privilege Mode？
- 当前 Hart 属于哪个 Supervisor Domain？
- 该函数能够访问哪些物理内存？
- 它是否改变了其他 Hart 可以观察到的安全状态？

长期坚持这样阅读源码，往往比单纯记录函数调用链更容易抓住体系结构的本质。

## 16.18 Chapter 16 — Staff Engineer 级测试题

1. Architecture Boundary： OP-TEE 已经拥有完善的 MMU Page Table 实现，为什么仍然需要 SMMPT？如果将 MPTE 管理直接加入 `core_mmu_arch.c`，会带来哪些架构问题？
2. Complete Call Path： 从 Linux User Application 的 `TEEC_InvokeCommand()` 开始，完整描述请求如何通过 TEE Driver、Transport、M-mode RDSM 到达 OP-TEE Trusted Thread，再进入 TA。
3. Domain vs Thread： 一个 Hart 从 Linux Domain 进入 OP-TEE Domain，并恢复之前暂停的 Trusted Thread。哪些上下文由 RDSM 保存，哪些由 OP-TEE 保存？如何避免重复保存或遗漏状态？
4. Memory Attack： Linux 传入一个长度为 8KiB、跨越两个物理页的 Shared Memory Buffer，其中第二页属于 Root-owned Memory。OP-TEE 和 RDSM 应该如何验证并拒绝请求？
5. RPC Security： OP-TEE TA 正在执行 Secure Storage 操作，因 REE FS RPC 暂停。此时 Linux 被攻击者控制并返回恶意数据。SMMPT 能保护哪些内容，哪些风险需要 OP-TEE 的其他机制处理？
6. SMP Correctness： 两个 Hart 同时运行 OP-TEE，其中一个 Hart 发起 Shared Memory Reclaim，另一个 Hart 正在访问同一 Buffer。设计一套避免 Use-after-Revoke 的安全同步协议。
7. RPMI Design Review： 比较私有 SBI TEE Extension、2026 年 RPMI TEE RFC v1 的寄存器语义封装，以及 RFC v3 的独立 Transport/Control ABI。说明各自的兼容性、维护性和安全边界差异。
8. Interrupt / Yield： Hart 4 正在执行 OP-TEE Trusted Thread，期间出现 Host Device Interrupt。如何安全地切换回 Linux 并在之后恢复原 Trusted Thread？如果多个 RPC 同时 Pending，应如何关联上下文？
9. Fault Diagnosis： Linux 调用 OP-TEE 后立即发生 Load Access Fault。请设计从 Linux TEE Driver、Transport、RDSM、`satp`、`mmpt`、MPTE、PMP 到 OP-TEE 页表的完整调试流程。
10. Architecture Ownership： 你负责将 SMMPT 集成到公司的 OP-TEE 平台。哪些代码应该进入 OpenSBI/RDSM，哪些应该进入 OP-TEE，哪些是纯粹的 SoC Platform Code？如何组织模块，使未来能够向 OP-TEE 或 OpenSBI 上游提交独立补丁？

通过标准： 可以将一次 TEE 调用分解为 Transport、Domain Switching、Trusted Thread、Memory Ownership、RPC 五条相互配合但职责独立的主线，并给出可信的代码实现方案。

# PART IV 总结：从三个 Chapter 建立完整软件架构

三个 Chapter 的逻辑关系如下：

```
Chapter 14: Establish Trust
--------------------------------
Secure Boot
  -> Root Firmware
  -> MPT Initialization
  -> Supervisor Domains
  -> Boot Linux / OP-TEE

Chapter 15: Manage Trust Boundaries
--------------------------------
OpenSBI / RDSM
  -> Domain Lifecycle
  -> Hart Context
  -> MPT Ownership
  -> Secure Domain Switching
  -> Multi-Hart Synchronization

Chapter 16: Execute Trusted Workloads
--------------------------------
Linux TEE Driver
  -> Transport
  -> RDSM
  -> OP-TEE Trusted Thread
  -> TA
  -> RPC / Return
```

归根结底，PART IV 要建立的是三种不同层次的能力。

第一层：建立隔离。

通过可信启动过程、MPT 初始化和 Root Domain 管理，在不可信 Supervisor 运行之前建立安全边界。

第二层：维护隔离。

通过 RDSM、Hart Context、Memory Ownership、Fence 和可靠的 Domain Switching，在运行期间持续维护这些边界。

第三层：利用隔离。

通过 OP-TEE 内部的 Thread、Session、TA、RPC 和共享内存机制，将硬件隔离真正转化为可供应用使用的可信服务。

一个成熟的实现，不仅需要三层分别正确，更需要三层之间的接口具有明确的责任归属。

## 最终工程检查清单

PART IV 自我评估

0 / 10

能够解释 RSS、OpenSBI、RDSM、UEFI、OP-TEE 和 Linux 的启动职责

能够独立设计 Host/TEE 两套物理内存权限

能够在源码中找到 OpenSBI 的 Domain Context Switching 路径

能够解释 satp、mmpt、PMP 与 MPT Fence 之间的关系

能够设计带权限撤销和跨 Hart 同步的 Memory Ownership Protocol

能够区分 Domain Context、Trusted Thread Context 和 TA Context

能够解释 SBI MPXY、RPMI TEE Transport 与 RDSM 的边界

能够分析一次完整的 Linux → OP-TEE → RPC → Linux 调用

能够区分 MMU Fault、MPT Fault、PMP Fault 与 DMA Fault

能够提出分阶段实施和验证 SMMPT + OP-TEE 的工程计划

重置评估

如果这些知识点都能够独立解释，并且能够结合实际源码完成关键路径追踪，就说明已经跨越了“了解 SMMPT 规范”的阶段，开始具备从 SoC 系统架构层面设计 SMMPT + OP-TEE 安全平台的能力。

下一阶段 PART V 应当进一步把这些设计转换成可观察、可复现的 Firmware 日志、寄存器状态、硬件仿真结果以及安全回归测试。真正做到不仅能够设计正确的隔离，而且能够用实验证明隔离确实正确。
