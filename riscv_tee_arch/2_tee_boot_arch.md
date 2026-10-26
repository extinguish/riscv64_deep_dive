# Chapter 02 — RISC-V OP-TEE System Boot Architecture

RISC-V OP-TEE Architecture & Porting · 深度技术教程 · Chapter 02 / 22

本章主题：从 SoC Reset 到 OpenSBI、OP-TEE 与 Linux 完整启动，深入理解多 Domain 系统的 Boot Architecture。

本章承接 Chapter 01 的 RISC-V TEE Security Architecture，重点转向实际工程实现。

我们不仅要知道系统是怎样启动的，更要回答：

- 为什么需要这样的启动顺序？
- 谁负责加载和认证 OP-TEE？
- OpenSBI 在什么阶段创建和初始化 Domain？
- Linux 与 OP-TEE 如何分别进入自己的 S-mode 环境？
- 多个 hart 如何完成初始化？
- Device Tree 如何在不同固件之间传递？
- Bootloader 如何确保 Linux 无法覆盖或访问 OP-TEE？

本章将结合 OpenSBI、OP-TEE upstream 源码及 RISE 的 RISC-V TEE 工程实现展开。

## 本章学习目标

完成本章后，你应该能够独立完成以下任务：

1. 画出 RISC-V SoC 的完整 Boot Chain，解释各阶段的 Privilege、执行实体和职责。
2. 从 OpenSBI 的 `fw_base.S`、`sbi_init.c` 追踪到下一级 S-mode Firmware。
3. 解释 coldboot/warmboot、Boot Hart、Hart Assignment 和 Domain Initialization。
4. 从 OP-TEE 的 `entry.S` 开始，分析早期栈、`gp/tp`、BSS、MMU 和 Core Runtime 初始化。
5. 为 Linux、OP-TEE、OpenSBI 设计内存布局和 Device Tree。
6. 理解 Secure Boot Chain 与 Runtime Domain Isolation 如何共同保障可信执行环境。

本章源码以 2026 年 10 月 23 日查阅的公开 upstream `master` 及 RISE 项目文档为参考。涉及开发分支的功能会特别标注，避免将尚未合并的设计当成通用实现。

# 2.1 从 SoC Reset 到 M-mode Firmware 的完整启动链

## 2.1.1 首先建立整个启动系统的认知模型

在 Arm TrustZone 平台，你熟悉的启动链可能是：

```
Boot ROM
   |
   v
BL1 / BL2
   |
   v
BL31 (EL3 Runtime Firmware)
   |
   +--> BL32: OP-TEE (S-EL1)
   |
   +--> BL33: U-Boot (NS-EL2/EL1)
                       |
                       v
                     Linux
```

其中，BL31 提供了 EL3 Runtime Services，负责特定的安全世界切换和电源管理等功能。

在 RISC-V 的 OpenSBI Domain 方案中，整体结构可以设计为：

```
                    SoC Reset
                        |
                        v
                Boot ROM / RoT
                     M-mode
                        |
                        v
               Early Bootloader
               (platform-specific)
                        |
                        v
                OpenSBI Firmware
                     M-mode
                        |
             +----------+----------+
             |                     |
             v                     v
       OP-TEE Domain          Linux Domain
          S-mode                 S-mode
             |                     |
             v                     v
         OP-TEE Core              U-Boot
             |                     |
             v                     v
           User TA                Linux
           U-mode                 S-mode
```

注意：这是系统架构图，不代表 OP-TEE 与 U-Boot 必须同时运行，也不代表 OpenSBI 在所有产品中都直接加载这两个镜像。

真实系统需要进一步区分三个动作：

- Load：谁将镜像复制到目标内存？
- Initialize：谁完成系统软件的初始化？
- Enter：谁将 CPU 控制权交给镜像入口？

这三个动作完全可能由不同实体完成。

例如：

Bootloader 加载 OP-TEE 镜像；OpenSBI 配置隔离并进入 OP-TEE；OP-TEE 初始化完成后，OpenSBI 再将控制权切回 Linux Domain。

这与「OpenSBI 自己读取 Flash 并加载所有镜像」不是一回事。

## 2.1.2 RISC-V 架构究竟规定了什么？

这是理解 RISC-V Boot Architecture 时非常重要的一个区分。

RISC-V 特权架构规定的是处理器特权机制及相关行为，而不是统一的 SoC 启动固件拓扑。

对于实现 M-mode 的 hart，Reset 后会进入 M-mode 的复位入口。但以下事项通常属于平台实现：

- Reset Vector 的具体地址。
- 哪些 hart 在 Reset 后立即开始执行。
- Boot ROM 是 ROM、片上 SRAM 中的固件，还是仿真器提供的启动代码。
- DRAM 是否已经初始化。
- 是否使用 Bootloader。
- 如何认证、加载 OpenSBI。
- 是否需要一个独立的 HSM/RoT Core。

因此，不应该认为：

> 所有 RISC-V hart 在 Reset 后都会直接执行 OpenSBI。

这并不是 RISC-V ISA 的要求。

更准确的理解是：

OpenSBI 是一种常见的 M-mode Runtime Firmware，位于平台早期启动固件与 S-mode 系统软件之间。

只有在平台将控制权交给 OpenSBI 时，hart 才开始执行 OpenSBI。

## 2.1.3 一个实际 SoC 的典型启动过程

假设我们设计一颗 RV64 多核 SoC：

- 4 个 Application Harts；
- 一个独立的 RISC-V HSM Core；
- Application Harts 支持 M/S/U-mode；
- 具备 PMP 和平台级 DMA/外设隔离机制；
- Linux 和 OP-TEE 作为两个受隔离的 S-mode Domain。

可以采用以下过程：

1. Boot ROM：建立最初的可信执行点

   Application Boot ROM 或 HSM 负责最早期的启动认证。硬件 RoT 提供信任锚，决定下一阶段代码是否可以执行。
2. Early Bootloader：完成硬件初始化

   初始化必要时钟、SRAM、DDR、Flash 控制器，准备将 OpenSBI、OP-TEE 和 U-Boot 等镜像放入内存。
3. OpenSBI：初始化 M-mode Runtime

   完成 per-hart scratch、Trap、HSM、Domain、SBI Extension、PMP 等必要初始化。
4. OP-TEE：初始化可信执行环境

   配置自身页表、内存管理、Trusted Thread、TA Runtime、TEE 服务及必要外设。
5. U-Boot：启动 Linux

   准备 Linux Kernel、Initramfs 和 Linux Device Tree，最后将执行权交给 Linux。
6. Linux：进入正常运行阶段

   初始化内核及 TEE Driver，通过受支持的通信协议访问 OP-TEE，而不能直接读取它的私有内存。

其中的 OP-TEE 初始化与 U-Boot 启动可以由 M-mode 的 Domain 调度策略进行组织，具体次序及是否重叠由平台设计决定。

## 2.1.4 Boot ROM 与 OpenSBI 的职责不能混淆

| 功能                    | Boot ROM / Early Firmware | OpenSBI                  |
| --------------------- | ------------------------- | ------------------------ |
| 硬件 Root of Trust      | 通常在这一侧建立                  | 不自动提供                    |
| 镜像签名认证                | 由可信启动设计决定                 | 标准 OpenSBI 不自动验证所有下一级镜像  |
| DDR 初始化               | 通常由早期固件承担                 | 取决于平台，但不是通用 OpenSBI 核心职责 |
| M-mode SBI Services   | 通常不是核心职责                  | 核心职责                     |
| Hart State Management | 可能提供早期控制                  | 负责运行时 HSM 服务             |
| Domain / PMP 配置       | 可以建立初始安全状态                | 实现运行时隔离策略                |
| Linux SBI ECALL       | 通常不处理                     | 负责处理                     |

这里还有一个重要的安全原则：

即使 Boot ROM 已经认证 OpenSBI，如果 OpenSBI 仍然可以被未受信任的软件修改，那么 Secure Boot Chain 就没有真正完成安全交接。

因此，固件的 Load、Authentication、Isolation 和 Execution 必须放在一个完整的安全模型中考虑。

# 2.2 Boot ROM、Secure Boot、Root of Trust 与 OpenSBI 的关系

这一节重点回答：OP-TEE 最终为什么可以被信任？

## 2.2.1 OP-TEE 的可信性不能由 OP-TEE 自己建立

假设我们直接从 Flash 加载一个 `tee.bin`：

```
Flash
  |
  | Load tee.bin
  v
RAM
  |
  v
Execute OP-TEE
```

这样做只能说明 CPU 执行了一份名叫 OP-TEE 的镜像，不能证明该镜像没有被篡改。

即使 OP-TEE 启动之后通过 PMP 将自己隔离起来，也不能解决启动前的可信性问题。

因为攻击者完全可能在 PMP 启用之前，替换这份镜像。

由此可以得到：

Runtime Isolation 不能代替 Secure Boot；Secure Boot 也不能代替 Runtime Isolation。

两者解决的问题不同。

| 安全机制              | 解决的问题                     |
| ----------------- | ------------------------- |
| Secure Boot       | 正在执行的固件是否经过授权？            |
| Runtime Isolation | 运行之后，其他软件是否能修改或读取受保护资源？   |
| Measured Boot     | 哪些代码和配置实际参与了本次启动？         |
| Attestation       | 能否向外部验证者证明设备状态或特定执行环境的属性？ |

## 2.2.2 从 Root Key 到 OP-TEE 的信任链

假设使用独立 HSM Core 提供 Root of Trust。

Hardware Root of Trust

OTP / eFuse / ROM Public Key Hash

Verify signature / metadata

Trusted First-Stage Firmware

Authenticate next-stage loader and manifest

Verify next-stage components

Authenticated Boot Manifest

OpenSBI / OP-TEE / U-Boot / DT Policy

Protected Runtime

M-mode OpenSBI + Isolated OP-TEE Domain

这里使用 Boot Manifest 是一种可选的产品设计方式，而不是 OpenSBI 强制要求的格式。

一个概念性的认证策略如下：

```
/* Pseudocode: illustrative secure boot policy */

int authenticate_and_load(
    const struct image_manifest *manifest)
{
    if (!verify_manifest_signature(manifest))
        return -1;

    if (!check_rollback_version(manifest))
        return -1;

    for_each_image(manifest, image) {
        if (!check_load_range(image))
            return -1;

        if (!load_image(image))
            return -1;

        if (!verify_image_digest(image))
            return -1;
    }

    if (!install_memory_isolation_policy(manifest))
        return -1;

    return 0;
}
```

这段代码不是 OP-TEE 或 OpenSBI 的现有 API，而是为说明安全启动设计而给出的伪代码。

它展示了几个非常重要的约束。

首先，签名不仅需要覆盖镜像内容，还需要可信地绑定镜像身份、目标地址、版本、安全配置等关键元数据，否则会出现合法镜像被错误加载到其他安全位置的问题。

其次，安全策略不能直接信任一份未经认证的 Device Tree。因为攻击者可以通过修改内存范围、设备所有权或 DMA 权限配置，破坏原本受保护的 TEE。

最后，要保证：

```
Authentication
    |
    v
Load and verify
    |
    v
Install protection
    |
    v
Execute protected image
```

在认证与隔离配置完成之前，不应该允许不可信 Domain 运行并修改已认证镜像。

## 2.2.3 如果独立 HSM Core 负责 Secure Boot，OpenSBI 还需要做什么？

这是实际 SoC 产品设计中非常值得讨论的问题。

假设 HSM 已经认证了 OpenSBI 和 OP-TEE。

这并不意味着 OpenSBI 可以跳过所有安全初始化。

HSM 的认证只能证明镜像满足指定的签名及版本策略；OpenSBI 仍然需要建立运行期的安全边界，例如：

- 限制 Linux 对 OP-TEE 私有内存的访问；
- 配置或协调 PMP 与平台防火墙；
- 维护 Domain 和 Hart 的运行状态；
- 管理必要的 M-mode 服务；
- 防止 Domain Switch 时留下不应暴露的硬件状态。

如果 Linux 能通过 DMA 读取 TEE 内存，那么即使整个 Secure Boot Chain 完全正确，运行时仍可能发生信息泄露。

所以：

> HSM/RoT 负责建立启动可信性；OpenSBI 和硬件隔离机制负责将可信边界延续到运行阶段；OP-TEE Core 进一步为 TA 提供执行隔离和可信服务。

这三层应该作为一个整体设计。

# 2.3 OpenSBI 的 FW_DYNAMIC、FW_JUMP 与 FW_PAYLOAD

现在进入真正的 OpenSBI 源码和启动机制。

一个很容易产生的误解是：

> OpenSBI 是 Bootloader，它负责决定从 Flash 加载哪个镜像。

实际上，OpenSBI 主要提供 M-mode Runtime Services。它的 Firmware 封装形式决定了前一级固件如何告诉 OpenSBI 下一级执行入口在哪里。

OpenSBI 官方提供三种主要 Firmware 形式。

[image](https://www.google.com/s2/favicons?domain=https://qemu.googlesource.com\&sz=32)

qemu.googlesource.com

+3



## 2.3.1 FW_DYNAMIC：运行时决定下一级入口

典型启动过程：

```
Boot ROM
    |
    v
Early Bootloader
    |
    +-- Load OpenSBI at address A
    |
    +-- Load U-Boot at address B
    |
    +-- Prepare fw_dynamic_info
    |
    v
OpenSBI FW_DYNAMIC
    |
    | Read next_addr from fw_dynamic_info
    v
U-Boot
```

这里下一级入口地址并不固定在 OpenSBI 编译结果中。

前一级固件通过 `a2` 传入 `struct fw_dynamic_info` 的地址。

这个结构的重要信息包括：

- Magic / Version；
- Next-stage Address；
- Next-stage Privilege Mode；
- Boot Hart；
- Firmware Options。

因此，FW_DYNAMIC 比较适合具有完整 Bootloader 或复杂镜像管理系统的 SoC。

例如 UEFI 或一个具备镜像验证能力的早期 Firmware 已经完成加载工作，只需要告诉 OpenSBI：

「下一阶段镜像已经位于指定物理地址，请初始化 M-mode Runtime 后进入它。」

## 2.3.2 FW_JUMP：使用预先指定的入口地址

这种方式更直接：

```
Early Firmware
    |
    +-- Load OpenSBI
    +-- Load Next-stage Image
    |
    v
OpenSBI FW_JUMP
    |
    | Jump to configured address
    v
Next Stage
```

典型构建参数：

```
make PLATFORM=generic \
     FW_JUMP=y \
     FW_JUMP_ADDR=0x80200000
```

其中 `0x80200000` 只是一个示例地址。

FW_JUMP 不会因此自动将镜像加载到该地址。

它要求前一级已经完成了加载工作，并确保入口有效。

## 2.3.3 FW_PAYLOAD：将下一级镜像打包在一起

```
fw_payload.elf
    |
    +-- OpenSBI Runtime
    |
    +-- Embedded Next-stage Payload
```

例如：

```
make PLATFORM=generic \
     FW_PAYLOAD=y \
     FW_PAYLOAD_PATH=/path/to/u-boot.bin
```

OpenSBI 构建系统将指定的 Payload 与 Firmware 一起构建。

这对于 QEMU 实验、固件镜像分发以及较简单的启动平台很方便。

但是要特别注意：

FW_PAYLOAD 并不等于 Secure Boot。

将两个镜像打包在一起，不会自动证明二者都经过可信认证。仍然需要由前面的 Boot Chain 建立认证与防回滚策略。

## 2.3.4 三种模式的本质区别

| 模式         | 下一阶段入口如何确定    | Payload 是否内嵌 | 典型用途                   |
| ---------- | ------------- | ------------ | ---------------------- |
| FW_DYNAMIC | 上一级通过运行时参数指定  | 否            | 完整 Bootloader / 复杂 SoC |
| FW_JUMP    | 编译时指定或配置固定偏移  | 否            | 固定启动布局                 |
| FW_PAYLOAD | 随 Firmware 打包 | 是            | QEMU、简单部署              |

这里有一个关键结论：

这三种模式决定的是 Firmware Packaging 和 Initial Handoff，不是 Linux/OP-TEE Domain 的运行时切换机制。

因此，后面 Chapter 06 中的 Domain Context Switch 不应该与 FW_DYNAMIC/FW_JUMP 混为一谈。

# 2.4 OpenSBI 的 Coldboot、Warmboot 与多 Hart 启动

现在我们深入 OpenSBI 最底层。

## 2.4.1 为什么 Coldboot 和 Warmboot 必须区分？

假设我们的 RISC-V SoC 有四个 hart：

```
Hart 0
Hart 1
Hart 2
Hart 3
```

若所有 hart 同时进入 OpenSBI，并执行：

```
initialize_heap();
initialize_domains();
initialize_global_interrupt_controller();
initialize_global_sbi_state();
```

那么这些全局资源可能被重复初始化。

例如 Hart 0 正在初始化 Domain List，而 Hart 1 也在修改 Domain List，很容易导致内存破坏或状态竞争。

因此，OpenSBI 将初始化分为两种角色：

- Coldboot Hart：负责一次性的 Global Initialization。
- Warmboot Hart：主要执行必要的 Per-Hart Initialization。

要注意，OpenSBI 所说的 warmboot 并不等同于整个 SoC 从低功耗状态恢复。它也用于区分没有执行 Global Initialization 的其他 hart 的初始化路径。

## 2.4.2 从 `fw_base.S` 开始

源码：

[opensbi/firmware/fw_base.S](https://github.com/riscv-software-src/opensbi/blob/master/firmware/fw_base.S?utm_source=chatgpt.com)

我们需要关注：

```
_start
    |
    v
_start_real
    |
    +-- Establish firmware execution environment
    |
    +-- Determine hart index
    |
    +-- Allocate per-hart scratch/stack
    |
    +-- Configure mscratch
    |
    +-- Configure trap entry
    |
    v
sbi_init()
```

这里最值得理解的两个设计是 `mscratch` 和 Per-Hart Scratch。

在现有实现中，OpenSBI 根据 Hart Index 定位各自的 Scratch Space，并设置 `mscratch`，同时准备栈和异常入口。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



概念如下：

```
Hart 0:
    mscratch -> sbi_scratch[0]
    sp       -> hart0 stack

Hart 1:
    mscratch -> sbi_scratch[1]
    sp       -> hart1 stack

Hart 2:
    mscratch -> sbi_scratch[2]
    sp       -> hart2 stack
```

这样，当任意 hart 因为 SBI ECALL 或其他 Trap 进入 M-mode 时，OpenSBI 可以快速恢复其 Per-Hart Context。

这与你之前研究的 NuttX per-CPU 数据结构非常相似。

区别在于 OpenSBI 需要跨越 M/S Privilege Boundary，所以 `mscratch` 是极为重要的硬件入口寄存器。

## 2.4.3 `sbi_init()` 如何分流？

源码：

[opensbi/lib/sbi/sbi_init.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_init.c?utm_source=chatgpt.com)

核心结构可以概括为以下伪代码：

```
void sbi_init(struct sbi_scratch *scratch)
{
    bool coldboot;

    coldboot = select_coldboot_hart();

    if (coldboot)
        init_coldboot(scratch, hartid);
    else
        init_warmboot(scratch, hartid);
}
```

注意，`select_coldboot_hart()` 是解释性的伪函数，实际选举过程还涉及 Boot Hart 选择、支持的 Next Privilege Mode、平台 Hook 和 Coldboot Lottery。

### Coldboot Path

真正的初始化流程比上面的伪代码复杂。

按照源码，关键部分是：

```
init_coldboot()
    |
    +-- sbi_scratch_init()
    |
    +-- sbi_heap_init()
    |
    +-- sbi_domain_init()
    |
    +-- sbi_hsm_init()
    |
    +-- Wake other harts
    |
    +-- Hart / platform / driver initialization
    |
    +-- sbi_domain_finalize()
    |
    +-- sbi_platform_final_init()
    |
    +-- sbi_ecall_init()
    |
    +-- sbi_domain_startup()
    |
    +-- Configure final hart protection
    |
    v
sbi_hsm_hart_start_finish()
```

这是经过精简的关键节点，并非包含所有源码调用的完整列表。

特别值得关注三个函数之间的顺序：

` sbi_domain_init()` → `sbi_domain_finalize()` → `sbi_domain_startup()`

以及 `sbi_domain_startup()` 后才执行最终的 Hart Protection Configuration。源码明确考虑了 Domain Finalization 和 PMP 配置的先后约束。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+2



### Warmboot Path

Warmboot Hart 会等待必要的 Global Initialization 完成，再根据自己的 HSM 状态进入启动或恢复路径。

需要特别注意：

Secondary Hart 可能仍然需要经过 OpenSBI。

原因是它必须建立正确的 M-mode Context、Hart State 和访问控制配置，然后才能进入自己的 S-mode Payload。

一个常见的运行时路径如下：

```
Linux CPU1
   |
   | sbi_hart_start()
   v
OpenSBI HSM
   |
   | Prepare secondary hart
   v
Secondary Hart enters M-mode firmware
   |
   | Restore / initialize hart-local state
   | Configure protection
   v
Linux secondary entry
```

具体是否重新执行某段固件入口代码，取决于平台 HSM 实现和硬件 Hart Start 方式。

## 2.4.4 `coldboot_done` 为什么使用 Release/Acquire？

源码中存在类似以下同步关系：

```
Coldboot Hart                   Warmboot Hart

Initialize global state
        |
        v
store_release(coldboot_done=1)
                                      |
                                      v
                           load_acquire(coldboot_done)
                                      |
                                      v
                           Access initialized state
```

这是一个经典的多核 Publish/Consume 场景。

仅仅使用普通的：

```
coldboot_done = 1;
```

不能充分表达跨 hart 的同步要求。

Release/Acquire 要保证 Warmboot Hart 观察到 Coldboot 完成时，也能正确观察到此前已发布的初始化结果。

这体现了为什么 Firmware 启动问题不仅涉及 Assembly，还涉及 RISC-V Memory Model。

# 2.5 OpenSBI Domain 的创建、初始化与 Hart Assignment

这是本章与后续 Chapter 04～07 的连接点。

## 2.5.1 Domain 是什么时候创建的？

首先明确：

OpenSBI Domain 不是 Linux 的 Process，也不是传统 Hypervisor VM。

OpenSBI Domain 是 OpenSBI 管理的一组资源和运行权限，包括：

- Memory Regions；
- Possible Harts；
- Assigned Harts；
- Boot Hart；
- Next-stage Entry；
- Next-stage Privilege；
- System Reset/Suspend Policy。

OpenSBI 官方 Domain 文档对这些属性有明确描述。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



在启动时，OpenSBI 先建立默认的 Root Domain，再由平台代码初始化其他 Domain。

我们可以将它分为三个阶段。

Step 1 — `sbi_domain_init()`

初始化 Domain 框架及默认 Root Domain，并建立基础资源描述。

Step 2 — `sbi_domain_finalize()`

通过平台 Domain Initialization 填充/注册 Domain，完成必要检查并冻结初始化阶段的配置。

Step 3 — `sbi_domain_startup()`

根据各 Domain 的 Boot Hart 和 Next-stage Entry，启动对应执行环境。

这里还必须特别注意：

Root Domain 不是 Secure World 的同义词。

Root Domain 是 OpenSBI 的默认 Domain。不能因为它的名字叫 Root，就认为它可以直接等价于 Arm Secure World。

## 2.5.2 `sbi_domain_startup()` 实际做了什么？

源码：

[opensbi/lib/sbi/sbi_domain.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain.c?utm_source=chatgpt.com)

该函数遍历 Domain，并检查：

1. Domain 的 Boot Hart 是否有效。
2. Boot Hart 是否属于 Domain 的 Possible Harts。
3. 该 Hart 当前是否被分配给这个 Domain。
4. 是否应该从当前 Coldboot Hart 直接进入，还是需要启动另一个 Hart。

核心逻辑可以抽象为：

```
/* Illustrative pseudocode */

for_each_domain(dom) {
    if (!domain_boot_hart_is_valid(dom))
        continue;

    if (dom->boot_hartid == coldboot_hartid) {
        scratch->next_addr = dom->next_addr;
        scratch->next_mode = dom->next_mode;
        scratch->next_arg1 = dom->next_arg1;
    } else {
        start_domain_boot_hart(dom);
    }
}
```

这解释了一个很重要的问题：

为什么 OpenSBI 可以启动多个 S-mode Domain？

因为每个 Domain 不只描述其内存范围，还包含其 Boot Hart 与 Next-stage Execution Context。

例如：

| Domain       | Boot Hart | Next Address | Next Mode |
| ------------ | --------- | ------------ | --------- |
| TEE Domain   | Hart 0    | OP-TEE Entry | S-mode    |
| Linux Domain | Hart 1    | U-Boot Entry | S-mode    |

这是一个可能的静态分配示例。

如果使用同一个 hart 时间复用两个 Domain，则还需要相应的 Domain Context Switching 机制，不能仅靠 Boot Entry 配置完成。

## 2.5.3 OpenSBI 最终怎样跳到 S-mode？

这里需要深入到实际寄存器。

我们选择下面这个函数：

`opensbi/lib/sbi/sbi_hart.c`

```
sbi_hart_switch_mode(
    arg0,
    arg1,
    next_addr,
    next_mode,
    next_virt);
```

该函数是真实存在的 OpenSBI 接口，其工作包括：

- 验证目标 Privilege Mode；
- 设置 `mstatus.MPP`；
- 设置 `mepc`；
- 初始化目标模式相关 CSR；
- 设置 `a0`、`a1`；
- 执行 `mret`。

源码见 [sbi_hart_switch_mode()](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart.c?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 从寄存器层面理解

假设：

```
next_addr = 0x90000000
next_mode = PRV_S
```

OpenSBI 需要完成等效于下面的处理：

```
# Schematic RV64 assembly, not verbatim OpenSBI source

# Configure MPP = S-mode, preserve other mstatus fields.
csrr    t0, mstatus
li      t1, ~(3 << 11)
and     t0, t0, t1
li      t1, (1 << 11)
or      t0, t0, t1
csrw    mstatus, t0

li      t0, 0x90000000
csrw    mepc, t0

# Prepare target a0/a1 according to boot ABI.

mret
```

为便于理解，上面仅展示了 `MPP` 和 `mepc` 的核心关系，省略了实际源码中其他 CSR、Interrupt、Virtualization 和状态初始化。

`mret` 的关键行为是：

```
Previous Mode: M-mode

mstatus.MPP = S-mode
mepc        = OP-TEE Entry

        mret
          |
          v

Current Mode: S-mode
PC          = OP-TEE Entry
```

最需要记住的是：

`mret` 不是普通函数返回，而是一个由特权状态决定目标 Privilege 的异常返回指令。

因此，OpenSBI 能通过 `mret` 进入 Linux，也能进入 OP-TEE。

但是：

进入 OP-TEE S-mode，并不自动保证已建立了完整的 TEE 安全边界。

在此之前还必须保证 Domain 所需的访问控制已经正确配置。

# 2.6 OP-TEE Core Image 加载、内存布局与入口参数

现在终于进入 OP-TEE 自身。

本节重点解释：

OP-TEE 镜像为什么能够在指定地址执行，以及它如何建立自己的 Runtime Environment。

## 2.6.1 加载地址和运行地址不是同一个概念

在实际 Firmware Porting 中，至少需要区分：

| 名称                | 含义                    |
| ----------------- | --------------------- |
| Load Address      | Bootloader 将镜像放入内存的位置 |
| Entry Address     | 开始执行镜像的 PC            |
| Link Address      | 链接器布局符号和代码地址时采用的地址    |
| Physical Address  | 实际物理内存地址              |
| Virtual Address   | MMU 启用后软件使用的地址        |
| Relocation Offset | 镜像重定位相关偏移             |

假设一个示例内存布局：

```
Physical DRAM
0x8000_0000
    |
    +-- OpenSBI firmware / runtime area
    |
0x8020_0000
    +-- U-Boot / Linux load area
    |
    +-- Linux usable memory
    |
0x9000_0000
    +-- OP-TEE Private Memory
    |       text
    |       rodata
    |       data
    |       bss
    |       stacks
    |       heap
    |
0x9100_0000
    +-- Other reserved / shared / Linux memory
```

这是刻意简化的示例，不是 QEMU virt 的官方默认地址分配。

实际工程必须检查内存区间是否重叠，以及 OP-TEE 加载区间与 Runtime Memory Region 是否一致。

特别是：

不能仅仅因为 Linux Device Tree 中标记某块内存为 Reserved Memory，就认为它已经对 Linux 不可访问。

`reserved-memory` 主要是一种操作系统内存使用约定。

真正的安全隔离仍然需要 PMP、WorldGuard、IOPMP 或 SoC Firewall 等硬件机制执行访问控制。

## 2.6.2 OP-TEE 的入口汇编

源码：

[optee_os/core/arch/riscv/kernel/entry.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/entry.S?utm_source=chatgpt.com)

当前 RISC-V OP-TEE 的 `_start` 会处理：

- `gp` 初始化；
- 保存 Hart ID 与 DTB 地址；
- 选举 OP-TEE Primary Hart；
- Primary/Secondary 入口分流。

源码中主要采用 `a0` 表示 Hart ID，`a1` 表示 System DTB 地址；在 M-mode 构建等特殊配置下存在不同处理。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 第一阶段：初始化 `gp`

在 RISC-V ABI 中，`gp` 是 Global Pointer。

早期汇编通过 `__global_pointer$` 初始化它。

这是为了确保相关 Global Data 访问能够正确进行。

尤其是 Firmware 初始阶段，不能假设此前执行的软件已经为当前镜像正确设置了 `gp`。

### 第二阶段：保存入口参数

大致过程：

```
a0 = hart ID
a1 = Device Tree address
```

然后保存在可跨越后续初始化代码的寄存器中。

这与 Linux RISC-V Boot Protocol 使用 `a0/a1` 的习惯相似，但不应该直接认为两者完全相同；仍须检查具体 OP-TEE Boot ABI 及固件配置。

### 第三阶段：OP-TEE Primary Hart Election

源码中通过 Atomic Operation 进行 Hart Lottery。

例如关键指令：

```
amoadd.w a3, a2, (a3)
```

该操作返回原来的计数值，并原子地完成增加。

如果返回零，该 hart 成为执行 Primary Boot Sequence 的 hart；其他 hart 进入 Secondary Path。

这一点尤其值得注意：

OpenSBI Coldboot Hart 与 OP-TEE Primary Hart 是两个不同软件层次的概念。

它们可以是同一个物理 hart，但不能将其视为必然相同。

## 2.6.3 为什么早期要执行 `satp = 0`？

在 OP-TEE 的早期入口中，可以看到：

```
csrw CSR_SATP, zero
```

这一步的目的，是使早期启动环境进入明确的 Bare Address Translation 状态，再由 OP-TEE 建立自己的内核页表。

之后通过：

`core_init_mmu_map()` → `enable_mmu()`

完成 MMU Mapping 及启用。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



其基本过程为：

```
OpenSBI hands over execution
          |
          v
OP-TEE Early Assembly
          |
          | satp = Bare
          v
Initialize Early Stack / Data
          |
          v
Create OP-TEE Page Tables
          |
          v
Configure satp
          |
          v
Enable Virtual Address Translation
          |
          v
OP-TEE Runtime Virtual Memory
```

这里有一个安全与架构问题：

如果使用 ASLR，OP-TEE 还可能执行相应的 Relocation，调整其虚拟地址映射关系。

因此不能机械地认为：

```
Physical Load Address == Virtual Runtime Address
```

是否相同取决于 Build Configuration 和 MMU 映射方案。

## 2.6.4 OP-TEE Early Stack、`tp` 与 Per-Hart Context

这是你具有 NuttX 经验后应该重点掌握的细节。

在 RISC-V OP-TEE 中，`tp` 不仅仅与通用用户态 TLS 概念相关。在 Core Kernel 的特定执行阶段，它也被用来快速定位当前 hart 的 `thread_core_local`。

可以理解为：

```
Hart 0:
    tp -> thread_core_local[0]

Hart 1:
    tp -> thread_core_local[1]

Hart 2:
    tp -> thread_core_local[2]
```

这样内核能够快速获取当前 Hart ID、Thread State、Exception/Temporary Stack 等执行状态。

对于启用动态配置的启动路径，`entry.S` 还会在早期构造临时的 `thread_core_local` 和栈，待后续内存及线程子系统初始化后再完成切换。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这也是为什么你在分析源码时必须跟踪：

- `sp`；
- `tp`；
- Hart Index；
- Current Thread；
- Temporary Stack；
- Abort Stack。

单纯跟踪 C 函数调用关系是不够的。

## 2.6.5 从汇编进入 OP-TEE C Runtime

当前源码中的主要流程可以简化为：

```
_start
   |
   v
reset_primary
   |
   +-- Initialize BSS
   +-- Setup early sp / tp
   +-- Save boot arguments
   +-- Initialize early SBI support
   +-- Initialize console
   +-- Initialize early memory
   +-- Construct MMU map
   +-- Enable MMU
   |
   v
boot_init_primary_early()
   |
   v
boot_init_primary_late()
   |
   +-- Initialize Device Tree
   +-- Discover non-secure memory
   +-- Update external Device Tree
   +-- Initialize Trusted Threads
   +-- Initialize per-hart thread data
   |
   v
boot_init_primary_runtime()
   |
   +-- Core / interrupt initialization
   +-- Initcalls
   +-- SBI MPXY / RPMI discovery
   |
   v
boot_init_primary_final()
   |
   +-- Driver initcalls
   +-- Final initcalls
   |
   v
Return boot completion to monitor/domain
```

这里的执行节点均可对应到当前 RISC-V `entry.S` 与 `boot.c`，包括 `boot_init_primary_late()`、`init_tee_runtime()` 和 `sbi_mpxy_rpmi_probe_channels()`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+2



最后一步也很重要。

OP-TEE Core 初始化完成之后，不一定直接继续启动 Linux。

例如当前源码的特定配置中，OP-TEE 会通过 `thread_return_to_udomain` 将初始化完成状态交还给对应的 Domain/Monitor 交互路径。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



从设计角度理解：

OP-TEE Boot Completion 是一个受管理的执行状态转换，而不应简单地理解成调用 Linux Kernel Entry。

# 2.7 Linux Domain 与 OP-TEE Domain 的初始化顺序

现在将前面的 OpenSBI 与 OP-TEE 启动过程串起来。

## 2.7.1 两种典型 Boot Topology

我们可以区分两种架构。

### 模型 A：不同 hart 负责不同 Domain

```
                    OpenSBI
                       |
             +---------+---------+
             |                   |
           Hart 0              Hart 1
             |                   |
             v                   v
         TEE Domain         Linux Domain
             |                   |
             v                   v
         OP-TEE Core            U-Boot
                                 |
                                 v
                               Linux
```

这种方式比较容易理解。

两个 Domain 可以在各自的 Hart 上初始化，但需要确保此前的全局 Firmware Initialization 已经完成。

同时，还需要保证设备与内存资源之间不存在未经管理的冲突。

例如 Linux 与 OP-TEE 如果都直接控制同一个 UART，可能发生寄存器状态冲突。

### 模型 B：同一 hart 在不同 Domain 间切换

```
            Hart 0
               |
               v
            OpenSBI
               |
               v
          OP-TEE Domain
               |
               | Initialize OP-TEE
               |
               v
           M-mode Switch
               |
               v
           Linux Domain
               |
               v
              Linux
               |
               | TEE Request
               v
            OpenSBI
               |
               v
          OP-TEE Domain
```

这更接近你熟悉的 TrustZone 执行模型，但 RISC-V 没有将 Secure/Normal 状态切换标准化为 Arm 那样的硬件 Security State。

该切换由 M-mode Firmware 与硬件隔离机制共同实现。

尤其需要注意：

如果多个 Domain 共享 Hart，就要确保切换时维护完整的 S-mode Context，包括：

- GPR；
- `sepc`；
- `sstatus`；
- `stvec`；
- `sscratch`；
- `satp`；
- `sie`、`sip` 等中断状态；
- Floating-point / Vector Context；
- 与平台隔离策略相关的状态。

这将是 Chapter 06 的核心内容。

## 2.7.2 为什么通常需要先完成 OP-TEE 初始化？

假设 Linux 已经启动并加载了 OP-TEE Driver。

Linux 发起第一个 TEE Request：

```
TEEC_OpenSession()
       |
       v
Linux TEE Driver
       |
       v
TEE Transport
       |
       v
OP-TEE
```

如果 OP-TEE 的 Trusted Thread、Memory Manager 和 Secure Services 还没有准备好，调用就无法正常进行。

因此，系统需要有明确的 Ready State 和初始化依赖。

但是从工程设计上说，并不一定要求 OP-TEE 的所有初始化步骤都在 Linux 第一条指令之前完成。

某些设计可以并行初始化，再在 TEE 通信接口启用前完成同步。

真正必须保证的是：

Linux 进入可用 TEE Service 阶段之前，OP-TEE 的通信和执行环境已经 Ready。

不能依赖 Linux 启动比较慢这种偶然条件。

## 2.7.3 Domain Boot Order 应如何表达？

OpenSBI 的 Domain 结构已经包含 `init_order` 等字段。

Domain Boot Hart、Next-stage Entry 以及初始化顺序可以由平台配置控制。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



但需要区分：

- Domain 启动顺序；
- Domain 初始化完成顺序；
- TEE Service Ready；
- Linux Kernel Ready。

这四个概念并不等价。

对于工业项目，我建议显式设计一个状态模型：

```
TEE_NOT_LOADED
      |
      v
TEE_AUTHENTICATED
      |
      v
TEE_ISOLATED
      |
      v
TEE_BOOTING
      |
      v
TEE_RUNTIME_READY
      |
      v
TEE_SERVICE_READY
```

这些是建议的系统设计状态，不是现有 OP-TEE upstream 中同名的枚举。

这样更容易处理异常情况。

例如：

- OP-TEE 验签失败；
- Domain Isolation Configuration 失败；
- OP-TEE 初始化 Panic；
- TEE 通信 Channel 不存在；
- Linux 过早调用 TEE。

如果缺少明确状态设计，Boot Flow 很容易依赖隐式时序。

# 2.8 U-Boot、Linux、OP-TEE 与 OpenSBI 的 Device Tree

这一节是实际 SoC Porting 中非常容易出问题的地方。

Device Tree 已经不再只是 Linux 描述外设的机制。它还可能成为多个 Firmware 共享的 Hardware Description 和 Boot Configuration 载体。

但是，它本身不是安全机制。

## 2.8.1 先区分三个不同用途的 Device Tree

在一个具有多个 Domain 的系统中，通常需要区分：

| Device Tree            | 主要消费者        | 主要职责                 |
| ---------------------- | ------------ | -------------------- |
| Firmware / Platform DT | OpenSBI、早期固件 | 描述平台硬件、Domain、资源策略   |
| Trusted Domain DT      | OP-TEE       | 描述 TEE 可访问设备、内存、Hart |
| Linux DT               | U-Boot、Linux | 描述 Linux 可访问设备与保留内存  |

它们可以来自同一份基础描述，也可以经过固件修改、过滤或重新生成。

不要求必须是三个物理上不同的 DTB 文件。

但其包含的信息必须与目标软件的权限和使用需求保持一致。

## 2.8.2 OpenSBI Domain Device Tree

OpenSBI 支持从 DT 中读取 Domain 配置。

例如下面是一个经过简化的示意：

```
chosen {
    opensbi-domains {
        compatible = "opensbi,domain,config";

        tee_mem: tee-memory {
            compatible = "opensbi,domain,memregion";
            base = <0x0 0x90000000>;
            order = <24>;  /* 16 MiB */
        };

        full_mem: full-memory {
            compatible = "opensbi,domain,memregion";
            base = <0x0 0x0>;
            order = <64>;
        };

        tee_domain: trusted-domain {
            compatible = "opensbi,domain,instance";

            possible-harts = <&cpu0>;
            boot-hart = <&cpu0>;

            regions = <&tee_mem 0x3f>;

            next-addr = <0x0 0x90000000>;
            next-mode = <0x1>;
        };

        linux_domain: normal-domain {
            compatible = "opensbi,domain,instance";

            possible-harts = <&cpu1>;
            boot-hart = <&cpu1>;

            regions = <&full_mem 0x3f>,
                      <&tee_mem 0x0>;

            next-addr = <0x0 0x80200000>;
            next-mode = <0x1>;
        };
    };
};
```

这是帮助理解 Domain 绑定的非完整 DTS 片段，不是可以直接启动的生产配置。

它省略了实际必需的设备权限、DTB 入口参数、内存和 Firmware Region 配置，以及可能存在的额外硬件隔离策略。

其中：

- `possible-harts` 定义 Domain 可以使用的 hart；
- `boot-hart` 指定启动 Domain 的 hart；
- `next-addr` 指定该 Domain 的下一阶段入口；
- `next-mode = <0x1>` 表示 S-mode；
- `regions` 描述访问权限。

特别要注意 `0x3f`：

这是 OpenSBI Domain Region 权限位的组合，包含 M-mode 及 S/U-mode 对应的 R/W/X 权限位，不能简单理解为 Linux 页表中的 `rwx`。

实际权限还取决于 Domain Region 的重叠规则、Root Region 继承和底层 PMP 编程结果。

完整定义见 [OpenSBI Domain Device Tree Bindings](https://github.com/riscv-software-src/opensbi/blob/master/docs/domain_support.md?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 2.8.3 为什么 Linux 和 OP-TEE 不应该共用未经处理的 Domain DT？

考虑一个问题。

如果 Linux 能读取完整的 Firmware DT：

```
OP-TEE Memory Address
OP-TEE Device Ownership
PMP Configuration
Domain Boot Information
```

这是否立即导致安全漏洞？

不一定。

因为配置内容的保密性不是硬件访问控制的根本保证。

但更重要的问题是：

Linux 是否能够通过修改这些配置，影响下一次 Domain 初始化或 Runtime Policy？

如果可以，就可能影响系统安全。

因此 OpenSBI 文档建议各 Domain 使用适合自身的 Device Tree，并在必要时过滤不应暴露的 Domain 配置信息。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



真正的设计原则是：

Policy 只能由可信实体建立或修改，而不是依赖隐藏地址来保证安全。

这也是 Chapter 01 所强调的 Threat Model 思想。

## 2.8.4 Linux Reserved Memory 与 OP-TEE Private Memory

在 Linux Device Tree 中，可以通过 `reserved-memory` 描述不应交给常规 Linux 内存分配器的区域。

例如：

```
reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;
    ranges;

    optee_reserved: memory@90000000 {
        reg = <0x0 0x90000000
               0x0 0x01000000>;
        no-map;
    };
};
```

含义是告诉 Linux：这 16 MiB 区间不应作为普通内存建立直接映射。

但必须强调：

`no-map` 不是 PMP。

即使 Linux 遵守 `no-map`，恶意内核仍可能尝试构造其他映射或发起物理访问。

只有受可信 Firmware 控制的硬件权限才能构成实际安全边界。

当前 OP-TEE RISC-V `boot.c` 中也存在向外部 DT 添加 Reserved Memory 描述的路径，例如为 Trusted Domain DDR 和特定 Shared Memory 配置保留区域。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 2.8.5 Device Tree 的信任生命周期

真正的工程设计应该回答：

```
Who creates the DTB?
       |
       v
Who authenticates the DTB?
       |
       v
Who modifies the DTB?
       |
       v
Who consumes the DTB?
       |
       v
Can untrusted software affect security policy?
```

例如：

- Early Bootloader 生成 DTB；
- HSM 认证包含安全配置的 Manifest；
- OpenSBI 解析可信 Domain Policy；
- OP-TEE 使用经过约束的 Hardware Description；
- Linux 获取自己的运行时 DTB。

但若 OpenSBI 将来重新解析一份已被 Linux 修改过的 DTB，且不重新验证安全关键内容，就可能产生配置篡改风险。

所以在实际产品中，安全策略的来源、修改权限和使用时机必须清楚地划分。

# 2.9 Core Image 认证、加载与执行之间的信任关系

这一节把整章知识归纳为一个系统安全问题。

## 2.9.1 Secure Boot 不是单纯的 Signature Verification

我们从实际产品角度看。

假设 HSM 已正确验证了：

```
OpenSBI Image
OP-TEE Image
U-Boot Image
```

这是否就意味着整个系统安全了？

答案是否定的。

接下来还需要验证：

- 镜像被加载到了允许的地址吗？
- Entry Address 是否与已认证的镜像一致？
- 加载之后、保护启用之前，是否有攻击者能够修改镜像？
- 已认证的 OP-TEE 是否会读取未经认证的安全配置？
- Linux 能否覆盖 OP-TEE 的物理内存？
- DMA Master 能否访问 TEE 私有内存？
- Secondary Hart 是否继承正确的权限配置？
- 系统恢复后是否仍然保留原本的隔离规则？

这说明 Secure Boot 不能简单地理解为：

```
Verify Signature -> Jump
```

更完整的设计应该是：

```
Authenticate Firmware
         |
         v
Validate Image Metadata
         |
         v
Validate Load / Execution Regions
         |
         v
Load Authenticated Image
         |
         v
Establish Hardware Isolation
         |
         v
Configure Domain / Hart State
         |
         v
Transfer Execution
         |
         v
Maintain Isolation During Runtime
```

## 2.9.2 为什么镜像加载完成后还不能立刻启动 Linux？

假设 OP-TEE 被加载到：

```
0x90000000 ~ 0x90FFFFFF
```

如果 Linux 获得 S-mode 控制权时，这块物理内存仍允许普通 S-mode 读写，则它可能：

1. 直接修改 OP-TEE Code；
2. 读取 OP-TEE Data；
3. 破坏 OP-TEE Stack；
4. 篡改密钥材料；
5. 影响之后的安全服务。

所以必须先安装相应的 Hardware Isolation Policy，再允许不可信 Linux 获得执行权。

对于基于 Domain Context Switching 的 TEE，后续每一次 Domain Switch 也需要确保权限配置与目标 Domain 一致。

这里还有一个容易忽略的问题：

权限切换不能只关注当前 hart 的 PMP，还必须考虑共享 Cache、DMA、Interrupt Controller 及平台级安全状态。

例如，Hart 0 已经完成 TEE 内存隔离，但 Hart 1 仍然保留更宽松的访问配置，那么整个 SoC 的安全边界仍然可能不成立。

## 2.9.3 Trust Boundary 的交接需要可验证

如果设计一套 Staff-level 的 RISC-V OP-TEE Boot Validation，我建议检查以下不变量：

| 不变量                | 验证目标                        |
| ------------------ | --------------------------- |
| Boot Authenticity  | 执行的镜像满足签名和防回滚策略             |
| Memory Ownership   | 每个 Domain 仅具有授权的物理内存访问权限    |
| Entry Integrity    | 目标 PC 指向经过验证的入口             |
| Hart Protection    | 所有可运行 hart 都配置了正确的保护策略      |
| Device Ownership   | DMA、MMIO 和 Interrupt 访问符合授权 |
| Runtime Readiness  | TEE Driver 调用前 OP-TEE 已准备好  |
| Recovery Integrity | 异常、重启、Resume 不得绕过验证和隔离      |

这些不变量不是单个 OpenSBI API 可以独立保证的。

它们需要 Boot ROM、HSM、OpenSBI、OP-TEE 和 SoC Security Hardware 共同满足。

# 三、完整启动流程：将前面九节串起来

现在我们建立整章的最终执行链。

下面假设采用：

- RV64 多核 Application Processor；
- 可信 Early Bootloader；
- M-mode OpenSBI；
- OP-TEE 与 Linux 两个 S-mode Domain；
- OP-TEE 先初始化，再提供 Linux TEE 服务。

```
[SoC Reset]
     |
     v
[Boot ROM / Hardware Root of Trust]
     |
     | Authenticate Early Firmware
     v
[Early Bootloader]
     |
     | Initialize DDR and basic hardware
     | Authenticate OpenSBI
     | Authenticate OP-TEE
     | Prepare Linux/U-Boot images
     | Prepare trusted configuration
     v
[OpenSBI fw_base.S]
     |
     | Initialize gp / sp
     | Initialize per-hart scratch
     | Configure mscratch / mtvec
     v
[sbi_init()]
     |
     +---------------------------+
     |                           |
     v                           v
[Coldboot Hart]             [Warmboot Harts]
     |                           |
     | Global Initialization     | Wait / Per-Hart Init
     |                           |
     v                           |
[sbi_domain_init()]               |
     |                           |
     v                           |
[Platform Domain Configuration]  |
     |                           |
     v                           |
[sbi_domain_finalize()]          |
     |                           |
     v                           |
[sbi_domain_startup()]            |
     |                           |
     v                           |
[Configure Hart Protection]      |
     |                           |
     +--------------+------------+
                    |
                    v
          [Enter S-mode Domain]
                    |
          +---------+---------+
          |                   |
          v                   v
    [OP-TEE Domain]      [Linux Domain]
          |                   |
          v                   v
    [OP-TEE entry.S]      [U-Boot Entry]
          |                   |
          |                   v
          |                [Linux]
          v
    [Primary Boot]
          |
          | Initialize Stack/TP
          | Initialize BSS
          | Configure MMU
          v
    [OP-TEE boot.c]
          |
          | Initialize Core Memory
          | Initialize Threads
          | Initialize Interrupts
          | Initialize Runtime Services
          | Probe SBI MPXY/RPMI
          v
    [TEE Runtime Ready]
          |
          +--------------------+
                               |
                               v
                       [Linux TEE Driver]
                               |
                               | TEE Invocation
                               v
                       [OpenSBI Domain Switch]
                               |
                               v
                       [OP-TEE Trusted Thread]
                               |
                               v
                           [User TA]
```

这里的 Linux/OP-TEE 两侧可以根据设计采用不同 Hart 启动，也可以通过支持的 Domain Context Switching 机制在 Hart 上进行时间复用。

这张图是系统设计级执行链；不同平台的具体加载、Domain Startup、初始化完成通知和调度策略可能不同。

# 四、结合真实源码验证本章知识

我建议优先阅读下面六个源码位置。

| 阅读顺序 | 源码                                                                                                                            | 关注点                                      |
| ---- | ----------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------- |
| 1    | [OpenSBI fw_base.S](https://github.com/riscv-software-src/opensbi/blob/master/firmware/fw_base.S?utm_source=chatgpt.com)      | M-mode 入口、Per-Hart Scratch、`mscratch`    |
| 2    | [OpenSBI sbi_init.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_init.c?utm_source=chatgpt.com)     | Coldboot/Warmboot、全局初始化顺序                |
| 3    | [OpenSBI sbi_domain.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain.c?utm_source=chatgpt.com) | Domain Init、Finalize、Startup             |
| 4    | [OpenSBI sbi_hart.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart.c?utm_source=chatgpt.com)     | `mstatus`、`mepc`、`mret`                  |
| 5    | [OP-TEE entry.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/entry.S?utm_source=chatgpt.com)        | `_start`、Primary Election、Early MMU      |
| 6    | [OP-TEE boot.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/boot.c?utm_source=chatgpt.com)          | Trusted Thread、DT、Runtime Initialization |

建议不要直接从 `boot.c` 开始。

应该先确认是谁将执行权交给 OP-TEE，并记录交接时的 `a0`、`a1`、Privilege Mode 和目标 PC，然后再深入 OP-TEE 的 `_start`。

# 五、Hands-on：在 WSL2 中验证启动链

这一章的实验目标不是编写 TA，而是复现和观察整个 Firmware Boot Flow。

RISE RP016 项目提供了基于 Buildroot 的集成环境，包含 OpenSBI、OP-TEE、U-Boot、Linux 和 QEMU。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



## 实验 1：构建与启动

在 WSL2 Ubuntu 中安装基础依赖，例如 Git、GNU Make、Python3、编译器及常见构建工具。然后使用该项目公开的 Buildroot 分支：

```
git clone \
  https://gitlab.com/riseproject/riscv-optee/buildroot.git \
  -b rp016_m1

cd buildroot

make qemu_riscv64_virt_optee_defconfig

make -j"$(nproc)"
```

这套流程会通过 Buildroot 下载、配置及构建多个组件。

构建完成后：

```
./output/images/start-qemu.sh
```

项目文档中的配置使用独立的串口连接，因此可能还需要在另一个终端连接：

```
telnet localhost 64320
```

Linux 启动后运行：

```
xtest
```

需要说明的是：这里给出的是 RP016 公开文档中的实验基线，并非声称该分支包含截至今天的所有 RISC-V OP-TEE 最新功能。其部分开发特性需要对应的集成分支。

## 实验 2：建立源码级启动日志

我建议在以下位置设置断点或添加临时调试日志：

```
OpenSBI:
    _start
    sbi_init
    init_coldboot
    init_warmboot
    sbi_domain_init
    sbi_domain_finalize
    sbi_domain_startup
    sbi_hart_switch_mode

OP-TEE:
    _start
    reset_primary
    boot_init_primary_early
    boot_init_primary_late
    boot_init_primary_runtime
    boot_init_primary_final
```

在 QEMU 支持的 Debug 配置下，可以通过 GDB 跟踪不同的 hart 和 Privilege。

推荐记录下表：

| 断点位置                 | Hart ID | Privilege | PC  | SP  | 关键 CSR             |
| -------------------- | ------- | --------- | --- | --- | ------------------ |
| OpenSBI `_start`     | 待观察     | M         | 待观察 | 待观察 | `mstatus`          |
| `sbi_init`           | 待观察     | M         | 待观察 | 待观察 | `mscratch`         |
| `sbi_domain_startup` | 待观察     | M         | 待观察 | 待观察 | Domain Context     |
| OP-TEE `_start`      | 待观察     | S         | 待观察 | 待观察 | `sstatus` / `satp` |
| OP-TEE Runtime       | 待观察     | S         | 待观察 | 待观察 | `stvec` / `satp`   |

注意：普通 GDB 对远程目标的 Privilege、CSR 访问以及多 Hart 单步支持会受 QEMU/GDB 版本和调试配置影响，应以实际可见状态为准。

## 实验 3：验证 OP-TEE 的 MMU Transition

在 OP-TEE：

`core/arch/riscv/kernel/entry.S`

定位：

```
core_init_mmu_map
enable_mmu
```

分析两个时间点：

MMU Enable 之前：

- `satp` 是什么？
- 当前 PC 使用什么地址？
- Stack 的物理内存在哪里？
- Global Data 如何访问？

MMU Enable 之后：

- `satp.MODE` 是什么？
- Root Page Table 位于哪里？
- PC 的 VA 与 PA 是否相同？
- `tp` 和 Stack 什么时候切换到最终地址？

这个实验可以直接验证你对 Link Address、Load Address、MMU Mapping 与 Relocation 的理解。

## 实验 4：验证多 Hart Boot

将 QEMU 配置为多个 hart，分析：

- 哪个 hart 成为 OpenSBI Coldboot Hart？
- 其他 Hart 为什么进入 Warmboot Path？
- 哪个 hart 成为 OP-TEE Primary Hart？
- OP-TEE 如何启动 Secondary Harts？
- 哪些资源只初始化一次？
- 哪些资源必须每个 Hart 各自初始化？

这里还有一个值得验证的实际源码细节。

当前 OP-TEE `boot.c` 对于多 hart 运行，会检查 SBI RFENCE Extension 是否可用，以避免跨 hart 地址转换同步能力不足的问题；启动 Secondary Harts 则需要 HSM 支持。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



这说明多核 OP-TEE Boot 不只是简单增加 CPU 数量，还需要检查相应的 SBI 服务能力。

# 六、Staff Engineer 深度思考题

下面八道题用于验证你是否真正理解了本章的设计理念。

逐题思考，再展开参考答案

显示全部答案

1\. 为什么 OpenSBI 的 Coldboot Hart 不一定等于 OP-TEE 的 Primary Hart？

查看参考答案

2\. 假设 OP-TEE 的签名已经验证，是否可以先运行 Linux，再配置 PMP？

查看参考答案

3\. FW_DYNAMIC 与 Domain Context Switch 的根本区别是什么？

查看参考答案

4\. 为什么 Domain 的 Next Address 正确并不意味着 Domain 一定能够启动？

查看参考答案

5\. Linux DTB 中配置 no-map，能否阻止 root 用户控制的内核读取 OP-TEE 物理内存？

查看参考答案

6\. 如果 OpenSBI 已将一个 hart 切换到 S-mode，为什么 Linux 仍然能够通过 SBI ECALL 回到 OpenSBI？

查看参考答案

7\. OP-TEE 与 Linux 均运行于 S-mode，二者能否在同一个 hart 上拥有完全不同的 satp？

查看参考答案

8\. 如果 SoC 有独立 HSM Core，并且 HSM 已认证所有镜像，OpenSBI 是否可以不再维护任何隔离策略？

查看参考答案

# 七、本章总结

我们从 RISC-V SoC Reset 出发，已经建立了完整的系统启动认知模型。

这章最需要记住的是四条技术原则：

第一，Bootloader、OpenSBI 和 OP-TEE 的职责不同。

Bootloader/可信早期固件负责加载及相应的认证流程；OpenSBI 提供 M-mode Runtime 和 Domain 管理；OP-TEE 在受保护的环境中提供 Trusted Execution Services。

第二，OpenSBI Domain Startup 不等于 OP-TEE Kernel Initialization。

OpenSBI 负责决定进入哪个 Domain、在哪个 Hart、从哪个入口开始执行；进入 OP-TEE 后，OP-TEE 仍然要独立完成栈、MMU、线程及核心服务初始化。

第三，Boot Authenticity 与 Runtime Isolation 必须同时成立。

经过认证的 OP-TEE，只有在启动及后续运行过程中都受到充分的硬件保护，才能对 Linux 等不可信软件形成有效隔离。

第四，真正的 RISC-V OP-TEE Porting 是一次跨 Firmware 的系统集成。

Boot Chain、Device Tree、Memory Layout、Hart Assignment、Interrupt Controller、PMP、SBI HSM/RFENCE 和 TEE Transport 都是整体的一部分。

因此，本章的核心结论是：

> RISC-V OP-TEE 的启动，并不是简单地让 OpenSBI 跳转到一个 S-mode 的 OP-TEE Image；而是在可信启动链基础上，为 OP-TEE 建立正确的硬件隔离、执行上下文和内核运行环境，并将其与 Linux 的启动及运行时通信协调起来。

下一章 Chapter 03 — RISC-V OP-TEE Development Environment 将把本章的理论转换为完整可复现的 WSL2 + QEMU 工程环境。完成之后，我们就可以以真实源码和调试实验为基础，深入 Chapter 04 的 `struct sbi_domain`、内存权限模型及 Domain Lifecycle。