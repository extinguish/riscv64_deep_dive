# **Chapter 07 — OP-TEE Boot Entry & Firmware Handoff**

OP-TEE OS Internals · Advanced / Staff Engineer Level

**源码基线：OP-TEE 4.11.0（2026-10-16）**

本章研究的是 OP-TEE 作为一个操作系统，在**正式拥有自己的运行环境之前**，如何从上一级 Firmware 接收控制权，并逐步建立一个可信赖、可执行、可初始化的 Secure Kernel。

这不仅是 Bootloader 的问题，更是整个 TEE OS 安全性和可维护性的起点。

## **7.0 本章的核心目标**

我们沿着以下路径研究：

**Boot ROM / Root of Trust**

镜像认证 · 安全启动策略

**Boot Firmware / Secure Monitor**

镜像加载 · 内存隔离 · CPU 启动环境

**OP-TEE Architecture Entry**

\_start · Boot Arguments · Exception Vector

**Early Boot Runtime**

Stack · BSS · Relocation · Early MMU

**OP-TEE Core Initialization**

Memory · Thread · Driver · Security Services

**Boot Completion & Runtime Entry**

返回上级调度组件 · 等待后续安全服务调用

注意最后一个阶段：

**OP-TEE Boot 完成，不等于 OP-TEE 开始运行一个普通 OS 的 Scheduler Loop。**

在经典的 OP-TEE SMC 架构中，初始化成功后，OP-TEE 会向负责安全域切换的组件报告完成，并提供后续运行时服务入口。其后，OP-TEE 主要响应来自 Normal World 的请求，而不是像 Linux 那样拥有一个持续运行的主调度循环。

在 FF-A、独立 Secure Partition 或 RISC-V Domain 架构中，Boot Completion 的接口可能不同，但是“初始化结束后转入由外部请求驱动的服务阶段”这个核心思想仍然重要。

本章要回答八个问题：

1. Firmware 和 OP-TEE 到底应该各自保证什么？
2. `_start` 执行前，CPU 和内存需要满足哪些条件？
3. 为什么 `_start` 一开始不能直接调用普通 C 函数？
4. Boot Arguments 是如何被保存、解释与验证的？
5. Primary CPU 与 Secondary CPU 的启动责任如何划分？
6. MMU 和 Exception Vector 在什么阶段建立？
7. 初始化完成后，OP-TEE 如何把控制权交还给外部组件？
8. 如果 Boot Contract 被破坏，怎样从最早的可观察状态开始定位问题？

# **7.1 首先理解 Boot Contract：Firmware 究竟交给 OP-TEE 什么？**

这是本章最重要的架构概念。

## **7.1.1 Boot Handoff 不是一次普通的函数调用**

在 C 程序中，假设函数 A 调用函数 B：

```
int result = function_b(arg1, arg2);
```

程序员通常可以假设调用约定是确定的：

- 有有效的栈。
- 代码和数据可以正常访问。
- C Runtime 已经初始化。
- 函数参数遵循 ABI。
- 调用者和被调用者共享适当的运行环境。

但在 Boot Firmware 将控制权交给 OP-TEE 时，上述条件不能直接成立。

例如，OP-TEE 需要考虑：

- 当前 CPU 是否处于预期的特权级？
- 当前代码地址到底是 PA 还是 VA？
- MMU 当前是否开启？
- `sp` 是否已经有效？
- CPU Cache 是否包含前一级 Firmware 留下的状态？
- 当前异常向量是否允许安全地处理异常？
- 是否可能有第二个 CPU 同时进入 `_start`？
- Boot Arguments 所指向的内存是否可信、可访问？
- OP-TEE 使用的 DRAM 是否已经受到硬件保护？

因此，我建议将 Boot Handoff 定义为：

> 上一级 Firmware 和 OP-TEE 之间的一份明确约定，规定在控制权转交瞬间，CPU 状态、内存状态、权限、输入参数、执行入口及后续控制流必须满足哪些条件。

这份约定就是 **Boot Contract**。

## **7.1.2 Boot Contract 应该包括哪些内容？**

我们可以建立一个相对完整的契约模型。

| 契约类别            | Firmware 侧职责                          | OP-TEE 侧职责               |
| --------------- | ------------------------------------- | ------------------------ |
| Image           | 将镜像加载到约定地址，完成必要的认证                    | 按照镜像布局进行初始化              |
| CPU Privilege   | 提供正确的 Security State / Privilege Mode | 验证并建立自身需要的执行状态           |
| Memory          | 保留安全内存，建立必要的硬件隔离                      | 建立内部 Memory Map 和 MMU    |
| Registers       | 按约定传递启动参数                             | 在覆盖寄存器前保存参数              |
| Stack           | 提供符合入口约定的执行条件                         | 建立自己的 Early Boot Stack   |
| Exception       | 避免未处理异常破坏启动                           | 尽早配置异常入口                 |
| SMP             | 按约定启动或保持其他 CPU                        | 建立 Primary/Secondary 同步  |
| Cache / MMU     | 提供已定义的初始转换和缓存状态                       | 完成必要的 Cache、TLB 和 MMU 处理 |
| Return Protocol | 提供启动完成后的返回机制                          | 注册运行时入口并报告完成             |
| Trust           | 保证启动链及访问控制的可信性                        | 验证输入并维护安全边界              |

这里有一个重要区别：

**Firmware 对 OP-TEE 镜像的认证责任，与 Firmware 对 OP-TEE 运行内存的隔离责任，是两个不同的安全问题。**

即使 OP-TEE 镜像已经完成签名校验，如果 Normal World 或不可信 DMA Master 仍然可以修改 OP-TEE 的代码和数据，那么这个系统仍然不能认为是安全的。

上游 OP-TEE Porting Guidelines 也明确要求平台完成 Root of Trust 和内存防火墙等硬件安全配置，而不是把它们视为 OP-TEE 软件自动提供的能力。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **7.1.3 两个经常被混淆的概念**

### **Secure Boot 不等于 Secure Execution**

Secure Boot 关注：

> 我们运行的镜像是不是预期的镜像？

Secure Execution 还需要关注：

> 在整个运行过程中，不可信实体是否能够修改 Secure Kernel 的代码、数据和安全设备状态？

因此，真正的安全启动链应当满足：

\\[ \text{Authenticated Image} + \text{Protected Runtime Memory} + \text{Correct CPU State} + \text{Valid Handoff Contract} \\]

当然，这只是安全启动的必要条件集合，而不是整个 TEE 安全性的充分证明。

### **Boot Success 不等于 Security Success**

假设 OP-TEE 能正常打印：

```
I/TC: OP-TEE version: ...
I/TC: Primary CPU initializing
```

甚至 Linux 中的 xtest 也可以正常运行。

这只能说明执行路径基本可用。

如果系统没有正确配置物理内存访问控制，那么 Normal World 仍有可能直接访问 Secure RAM。

这时功能验证通过，并不意味着安全目标已经达到。

**Staff Engineer 必须能够区分 Functional Correctness 和 Security Correctness。**

# **7.2 从 Firmware 架构看 OP-TEE 的入口**

同样叫 OP-TEE Boot，不同硬件平台之间可能存在非常不同的控制权转移方式。

这里我们首先从宏观上建立比较。

## **7.2.1 Arm：典型的 TF-A + OP-TEE Boot Chain**

在经典 Armv8-A TrustZone 平台中，常见启动架构为：

```
Boot ROM
    |
    v
TF-A BL2
    |-- Authenticate / Load BL31
    |-- Authenticate / Load BL32 (OP-TEE)
    `-- Load BL33 (U-Boot)
    |
    v
TF-A BL31 (EL3)
    |
    |  via Secure Payload Dispatcher
    v
OP-TEE BL32 (S-EL1)
    |
    |  Initialize Secure Kernel
    |
    |  Report initialization complete
    v
TF-A BL31
    |
    v
BL33 / Normal World
```

这里需要注意三个职责边界。

**BL2 是加载阶段。** 它通常负责准备 Firmware Image，并可以通过 TF-A 的认证框架实现可信启动。

**BL31 是 EL3 运行时 Firmware。** 在经典 SPD 模型中，它负责协调进入和退出安全负载、处理相应的安全域切换协议。

**BL32 是 OP-TEE。** 它在自己的运行环境中负责 Trusted Thread、TA、Memory、Crypto、Storage 等内核服务。

这意味着：

OP-TEE 并不需要自己实现全部 Secure Monitor 逻辑，也不需要承担 Linux Bootloader 的角色。

但是这个模型不是所有 Arm 平台的唯一实现。FF-A/SPMC 架构的启动与运行时通信机制不同，不能把经典 `opteed` 接口直接套用到 FF-A 上。

## **7.2.2 RISC-V：Privilege Mode 本身不是 Secure World**

现在换到 RISC-V。

一个典型的 RISC-V Firmware Stack 可能是：

```
Boot ROM / RSS / Root of Trust
    |
    v
Early Platform Firmware
    |
    v
OpenSBI / M-mode Firmware
    |
    +---- Normal Domain
    |        |
    |        +---- U-Boot / UEFI
    |        `---- Linux
    |
    `---- Trusted Domain
             |
             `---- OP-TEE
```

但有一个关键区别：

**标准 RISC-V M/S/U 特权级本身，并不等价于 Arm TrustZone 的 Secure/Non-Secure State。**

换句话说：

- `S-mode` 比 `U-mode` 具有更高权限。
- 但两个不同的 S-mode 软件环境，不能仅仅因为都运行在 S-mode 就自动实现相互隔离。
- 如果 OP-TEE 和 Linux 都在 S-mode，系统必须有额外机制建立两者的执行和物理资源隔离。

这种机制可能涉及：

- M-mode Firmware 控制的 PMP/ePMP。
- RISC-V Domain 机制。
- SMMPT 或其他系统级 Memory Protection。
- 特定 SoC 的 Secure Bus、Device Firewall。
- 合适的 Secure Monitor 或 Domain Switch 设计。

因此，RISC-V OP-TEE 的 Boot Contract 通常更依赖平台具体的安全域实现。

上游 4.11.0 的 RISC-V 入口代码支持不同编译配置，包括 M-mode 和 S-mode 相关分支；S-mode 路径还明确使用 SBI HSM 管理 Secondary Hart。不能简单假定“所有 RISC-V OP-TEE 都由 OpenSBI 直接通过一次普通 `mret` 启动，然后与 Linux 共用 S-mode”。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **7.2.3 Arm 与 RISC-V 的真正可移植边界**

| 机制                | Arm A-profile                    | RISC-V                           |
| ----------------- | -------------------------------- | -------------------------------- |
| Trusted Execution | TrustZone Secure State，或相应安全分区模型 | 平台定义的 Trusted Domain/隔离机制        |
| 常见 OP-TEE 权限      | S-EL1                            | M-mode 或受保护的 S-mode              |
| 上级 Firmware       | TF-A / SPM 等                     | M-mode Firmware / Secure Monitor |
| Exception Vector  | `VBAR_EL1` 等                     | `stvec` / `mtvec`                |
| MMU 控制            | `TTBR*`、`TCR`、`SCTLR`            | `satp` 等                         |
| 返回外部域             | SMC / FF-A 接口                    | 平台定义的 Domain Switch / SBI 相关机制   |
| Secondary CPU     | PSCI / 平台启动机制                    | SBI HSM / 平台启动机制                 |

这张表真正要传递的思想是：

**OP-TEE Core 的服务模型可以跨架构复用，但是建立安全执行域、完成特权上下文切换的能力，必须由 Architecture Backend 和底层 Firmware/Hardware 共同提供。**

# **7.3 进入真实源码：Arm AArch64 的&#x20;**`_start`

现在开始把前面的 Boot Contract 映射到真实的 OP-TEE 源码。

本章优先分析 AArch64 的经典 TF-A/OP-TEE 启动路径，然后对照 RISC-V 实现。

## **7.3.1 源码定位**

OP-TEE 4.11.0 中，我们重点阅读：

```
optee_os/
└── core/
    └── arch/
        └── arm/
            └── kernel/
                ├── entry_a64.S
                ├── boot.c
                ├── thread.c
                └── thread_a64.S
```

源码：

[Arm64 entry_a64.S — OP-TEE 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/entry_a64.S?utm_source=chatgpt.com)[Arm boot.c — OP-TEE 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/boot.c?utm_source=chatgpt.com)

`entry_a64.S` 负责最初的汇编环境建立；`boot.c` 负责更高级的 Core 初始化；`thread.c` 等文件负责建立正式的运行时执行环境。

两者之间不是简单的一次调用，而是随着内存和执行环境逐步建立，在 Assembly 与 C 之间多次切换。

## **7.3.2 第一阶段：保存 Firmware 提供的寄存器参数**

真实源码在 `_start` 开始处保存：

```
mov x19, x0
mov x20, x1
mov x21, x2
mov x22, x3
```

这一段非常短，但包含一个很重要的 Boot 设计原则。

**在建立自己的 Runtime Environment 之前，先保存外部传递的信息。**

为什么使用 `x19` 到 `x22`？

根据 AArch64 Procedure Call Standard，这些寄存器属于 Callee-Saved Registers，而 `x0` 到 `x7` 通常承担函数参数及返回值传递职责。

因此，后续代码可以自由使用 `x0` 到 `x3` 执行初始化，而不立即丢失原始 Boot Arguments。

注意：这里选择 Callee-Saved 寄存器，并不意味着 `_start` 已经建立完整的 C ABI 环境，也不意味着当前栈可用。

我们稍后会看到，保存这些参数后，OP-TEE 还需要做大量工作才能安全进入普通 C 代码。

## **7.3.3 第二阶段：尽早建立异常向量**

紧随其后，源码执行了类似下面的操作：

```
adr x0, reset_vect_table
msr vbar_el1, x0
isb
```

逐条分析。

`adr x0, reset_vect_table`

使用 PC-relative Addressing 计算当前代码中 `reset_vect_table` 的地址。

这类寻址方式在 Early Boot 特别有价值，因为此时完整的虚拟地址映射尚未建立。

`msr vbar_el1, x0`

把异常向量表地址写入 `VBAR_EL1`。

从这一刻开始，后续路由到当前 EL1 向量表的异常将依据新的向量表处理。

但是，设置一个有效的异常向量地址，并不等于系统已经能够安全处理所有异常。

异常处理程序还可能依赖栈、CPU Local State、MMU Mapping 和其他 Runtime Infrastructure。

`isb`

Instruction Synchronization Barrier 确保后续指令在新的相关系统寄存器配置下执行。

这里最值得思考的问题是：

> 为什么必须在 MMU 初始化之前，就考虑异常向量的有效性？

因为 Early Boot 本身就可能产生异常。

例如：

- 访问未映射或不存在的物理地址。
- 指令执行权限不满足要求。
- 执行当前架构不支持的指令。
- 读取某些在当前 Exception Level 不允许访问的寄存器。
- 一次错误的 Relocation 导致访问非法地址。

假如此时异常向量仍然指向未知位置，最初的一个错误就可能使系统进入无法诊断的状态。

因此，Early Exception Vector 本身也属于 Boot Contract 的一部分。

## **7.3.4 第三阶段：初始化 CPU 控制寄存器**

接下来，`entry_a64.S` 会执行 `set_sctlr_el1` 等宏。

我们不需要在 Chapter 07 展开每一个 `SCTLR_EL1` Bit，但是必须理解它们的设计目的。

这些配置包括：

| 控制内容                         | 设计目的                     |
| ---------------------------- | ------------------------ |
| Instruction Cache            | 明确指令缓存行为                 |
| Alignment Check              | 检测特定不对齐访问                |
| WXN                          | 限制可写区域同时可执行              |
| PAN                          | 限制特权代码访问低权限映射            |
| BTI / Pointer Authentication | 在支持的配置中建立控制流安全能力         |
| MTE                          | 在支持的配置中配置 Memory Tagging |

这里需要注意，不是所有安全扩展都在 `_start` 的同一个阶段完全启用。

例如，Pointer Authentication、Memory Tagging 可能涉及额外的密钥初始化、Memory Tag 初始化或 Per-CPU 配置。

**因此，不能看到&#x20;**`set_sctlr_el1`**&#x20;就认为 CPU Security Configuration 已经全部完成。**

正确的源码阅读方法是同时记录：

- 哪些寄存器位在 `_start` 配置。
- 哪些状态在 `boot_init_primary_*()` 配置。
- 哪些状态需要 Secondary CPU 分别配置。
- 哪些能力直到 Runtime 才真正启用。

这是评审 Architecture Patch 时容易忽视的地方。

## **7.3.5 第四阶段：初始化最小内存环境**

接下来，汇编代码完成几个重要动作：

1. 在需要时移动或准备初始化数据。
2. 对 `.bss` 等指定区域清零。
3. 处理物理加载地址变化带来的 Relocation。
4. 建立 Temporary Stack 和 Per-CPU 早期状态。
5. 初始化 Early Console。
6. 保存完整 Boot Arguments。
7. 初始化 Early Boot Memory Allocator。

这里有一个经常被忽略的问题。

**为什么不能把所有 Memory Region 都清零？**

因为不是所有数据都属于普通 `.bss`。

例如：

- 某些早期同步变量必须在 `.bss` 清零之前被使用。
- 某些栈和 Translation Table 位于 `nozi` 等特定 Section。
- 某些 Firmware 提供的 Boot Parameter 必须保留。
- 某些数据位于其他 Firmware 仍然使用的共享物理区域。

因此，Early Boot 的内存初始化必须严格遵守 Linker Script 定义的边界。

这也是为什么 Entry Assembly、Linker Script 和 `boot.c` 必须一起阅读。

## **7.3.6 第五阶段：建立 MMU 并转换到新的虚拟地址**

源码核心调用关系可概括为：

```
_start
  |
  +-- Prepare early memory
  |
  +-- boot_mem_init()
  |
  +-- core_init_mmu_map()
  |
  +-- Relocation, when required
  |
  +-- enable_mmu()
  |
  +-- boot_init_primary_early()
```

在 `enable_mmu()` 中，AArch64 路径设置：

- `TCR_EL1`
- `MAIR_EL1`
- `TTBR0_EL1`
- TLB 状态
- `SCTLR_EL1.M`

而在启用 MMU 后，还要考虑：

- 更新 `VBAR_EL1`。
- 更新栈指针。
- 更新返回地址。
- 进行必要的 Cache/TLB 同步。
- 保证当前执行代码在地址空间切换前后都可以访问。

这些步骤在 4.11.0 的 `enable_mmu` Assembly 中有明确实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **为什么 MMU 启用瞬间最危险？**

假设当前 CPU 在物理地址：

`PA = 0x1000_0000`

执行某条汇编指令。

当 MMU 开启后，CPU 不再简单地把 PC 当作直接访问物理内存的地址。

而是依据当前 Translation Table，对虚拟地址进行转换。

如果当前 PC 所指向的 VA 没有有效映射，那么 CPU 可能立即发生 Instruction Abort。

因此，OP-TEE 使用 Identity-Mapped 的启动代码区域，并在合适的阶段完成地址转换。

这就是为什么在 Early Boot 的 MMU 代码中，需要特别关注：

```
Current PC
Current SP
Current Exception Vector
Current Translation Table
Next Instruction Address
```

这五项必须在同一个时间点满足一致的映射关系。

### **ASLR 带来的额外问题**

如果开启 `CFG_CORE_ASLR`，不仅需要建立正确的页表，还要处理：

\\[ VA\_{\text{runtime}} = VA\_{\text{base}} + \Delta \\]

其中 \\(\Delta\\) 为选定的地址随机化偏移。

需要特别关注：

- 已经写入内存中的绝对指针。
- Stack Pointer。
- Return Address。
- Exception Vector。
- Per-CPU 结构体指针。
- MMIO 地址。
- 已记录的其他 Boot-time 地址。

因此，ASLR 不只是修改一个 MMU 基地址，而是一次需要维护多个状态一致性的地址迁移。

在 4.11.0 中，ASLR 路径还包含在启用 MMU 后重新初始化 Console 的操作，因为早期 Console 注册可能使用物理地址，而运行时访问需要对应的虚拟地址。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



# **7.4 从汇编进入 C：OP-TEE Core 初始化的真正边界**

我们已经看到 Assembly 如何建立最小执行环境。

接下来关注 C 层初始化。

## **7.4.1 为什么不直接调用一个&#x20;**`main()`**？**

OP-TEE 不是一个独立启动、独占 CPU 并持续运行主循环的普通应用。

它需要分阶段建立运行环境，所以源码使用了多个初始化入口：

```
boot_init_primary_early()
boot_init_primary_late()
boot_init_primary_runtime()
boot_init_primary_final()
```

这些名字本身比具体实现更重要，因为它们反映了一个设计思想：

**不同的初始化阶段能够依赖的内核服务并不相同。**

在 Arm 4.11.0 中，这几个函数的主要职责如下。

| 阶段        | 核心工作                                                     |
| --------- | -------------------------------------------------------- |
| `early`   | 初始化基本 Memory Runtime、Per-CPU 状态、Pager 等                  |
| `late`    | 初始化外部 DT、Thread 和 Boot Thread                            |
| `runtime` | 初始化 Primary Thread、Interrupt Controller、Runtime Services |
| `final`   | Driver Initcalls、Final Calls、启动完成准备                      |

这是常见非虚拟化路径的概括；实际顺序和处理内容受 Pager、FF-A、Virtualization 等编译配置影响。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



## **7.4.2 Boot Thread 为什么是一个很重要的设计？**

在真正处理用户请求之前，OP-TEE 已经需要使用一些 Thread Infrastructure。

例如，初始化某些服务可能依赖：

- 当前 Thread Context。
- Mutex。
- Thread-local / Per-CPU State。
- 某些需要使用 Kernel Stack 的操作。

但是，此时还没有来自 Linux 的普通 CA Request。

怎么办？

OP-TEE 可以建立 Boot Thread，把初始化过程临时关联到一个有效的 Thread Context。

在完成初始化后，再将其释放或清理为可供后续使用的状态。

这说明：

**Boot Thread 并不是由 Normal World 发起的普通 Trusted Thread，也不是系统的永久主线程。**

它的价值在于让初始化阶段可以安全使用部分正式的内核运行时抽象。

这一点也反映在真实代码中。

AArch64 的 `_start` 在调用 `boot_init_primary_runtime()` 之前，会将当前栈切换到 `threads[0]` 对应的栈，并调整 Core-local Flags；初始化完成后再恢复 Temporary Stack，最后清理 Boot Thread 状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **7.4.3 为什么还要区分 Temporary Stack 和 Thread Stack？**

这涉及一个重要的执行环境不变量。

Temporary Stack 主要服务于：

- CPU 初次进入 OP-TEE。
- 尚未完成 Thread Context 分配的阶段。
- 某些 Entry/Exit 状态转换。

而正式 Thread Stack 则用于：

- 需要正常 Kernel Thread Context 的操作。
- 可能涉及更复杂调用链的 Runtime Service。
- 允许使用相应线程同步和内核资源管理机制的场景。

不能简单地把两种栈互换。

例如，某些内核代码会根据 Per-CPU Flags 判断当前是否处于 Temporary Stack Context，并据此限制能够调用的函数。

如果错误地在 Temporary Stack Context 使用了依赖正式 Thread 的服务，可能导致：

- 当前 Thread ID 不合法。
- 锁或等待操作违反上下文约束。
- 递归进入异常处理。
- 无效的 Thread-local State。
- 栈空间不足。

我们将在 Chapter 14 和 15 深入这些细节。

但在 Chapter 07，必须先建立这个认识：

**Boot Entry 不仅要完成 CPU 初始化，还要把执行环境逐步升级成 OP-TEE Runtime 能够正常工作的状态。**

# **7.5 Boot Arguments：OP-TEE 如何接收并验证 Firmware 信息？**

这是一个非常适合从 OS Internals 角度深入的主题。

因为 Boot Arguments 看起来只是几个寄存器，但它们可能决定：

- OP-TEE 的内存布局。
- 外部 DT 的地址。
- Pager Backing Store。
- Secure Partition 的 Manifest。
- Normal World 的运行参数。
- 后续 OS 能否找到 OP-TEE 的服务接口。

## **7.5.1 Arm64 Boot Arguments 的实际含义**

在 OP-TEE 4.11.0 的 `boot_save_args()` 中，启动参数的解释取决于配置。

以非 FF-A、使用 TF-A 的常规 AArch64 路径为例：

| 寄存器  | 常见含义                             |
| ---- | -------------------------------- |
| `x0` | Pager 配置下的 Pageable Part Address |
| `x1` | 此路径通常不使用                         |
| `x2` | System DTB Address               |
| `x3` | 此路径通常不使用                         |

但这只是其中一种 Boot ABI。

对于 FF-A/SPMC 配置，`x0` 可能指向 TOS Firmware Config 或 FF-A Boot Information Blob。

如果启用 `CFG_TRANSFER_LIST`，参数采用另一套传递约定，通过指定的寄存器组合提供 Transfer List Signature、版本和地址。

这些分支已经明确出现在 4.11.0 的 `boot_save_args()` 源码中。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



因此，阅读 Boot Arguments 时，首先要回答：

> 当前编译配置到底选择了哪一种 Handoff ABI？

而不是直接假定 `x0`、`x2` 一定具有某种固定含义。

## **7.5.2 为什么要保存 Boot Arguments，而不是一直直接使用？**

考虑一个具体场景。

Firmware 在 `x2` 中传入：

```
x2 = 0x8800_0000
```

表示外部 DTB 所在的物理地址。

OP-TEE 刚进入 `_start` 时，能够把这个值看作一个 Physical Address。

但是当 MMU 打开后：

```
PA = 0x8800_0000
```

不一定能够再作为：

```
void *fdt = (void *)0x88000000;
```

直接解引用。

因为这时候地址解释已经发生变化。

所以正确的设计需要区分：

```
Boot Register Value
        |
        v
Physical Address
        |
        v
Mapping / Validation
        |
        v
Runtime Virtual Address
        |
        v
Device Tree Parser
```

一个特别重要的原则是：

**Boot 参数是上一级组件传入的信息，不意味着它已经成为当前 OS 可直接访问的对象。**

## **7.5.3 Device Tree 是不是可信输入？**

这里要区分两件事：

**谁提供 DTB？**

**DTB 中的哪些信息能够用于制定安全策略？**

例如，外部 DTB 可能位于 Normal World Memory。

OP-TEE 会在初始化期间读取它，并可能向其中添加 OP-TEE 节点、Reserved Memory 节点等信息，供后续 Normal World 使用。

上游文档对此有明确说明。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



但不能因此得出：

> DTB 中包含 `secure-status = "okay"`，所以对应设备就一定是安全设备。

这是错误的安全推导。

设备是否真正受到保护，必须由硬件访问控制、可信 Firmware 和 OP-TEE 自身配置共同保证。

如果攻击者能够修改某些决定 Secure Memory 或 Secure Peripheral Ownership 的配置，那么单纯解析 DTB 不能建立可信性。

### **一个具体的威胁模型**

假设某个平台错误地允许不可信 Bootloader 控制：

```
secure-memory-base
secure-memory-size
```

而 OP-TEE 直接根据这两个值配置自己的内存区域。

攻击者可能将 Secure Memory 配置到：

- 实际未被硬件保护的 DRAM。
- 与 Normal World RAM 重叠的区域。
- 部分被 DMA Master 访问的地址范围。

此时即使 Kernel MMU 对该区域设置为 Privileged-only，也无法阻止系统级不可信 DMA 访问。

因此，设计时应该区分：

| 信息                       | 是否适合从可变外部 DT 获取    |
| ------------------------ | ------------------ |
| Console Device           | 可以，但需考虑权限及使用阶段     |
| 普通可发现设备信息                | 根据平台信任模型决定         |
| 可用的 Non-Secure DRAM      | 可以参与发现，但必须验证边界     |
| Secure DRAM 的安全归属        | 必须由可信配置和硬件隔离保证     |
| Secure Device Ownership  | 必须与可信硬件配置一致        |
| ASLR Seed                | 需要可信熵源或可信提供者       |
| Secure Boot Trust Anchor | 不应由攻击者可修改的 DT 任意决定 |

另外还需要注意 **TOCTOU**。

即使 OP-TEE 在某一瞬间验证了外部 DTB，如果验证后到使用期间，其他实体能够修改同一块共享内存，仍可能产生安全问题。

因此，重要配置可以通过复制到受保护内存、限制使用时段、固定映射或重新验证等方式降低风险，具体策略取决于实际数据的所有权。

## **7.5.4 Boot Argument Validation 需要哪些检查？**

我们可以把它抽象成以下检查流程：

```
Firmware Input
    |
    v
Is the pointer format valid?
    |
    v
Is the address properly aligned?
    |
    v
Is the memory range accessible?
    |
    v
Are length and boundaries valid?
    |
    v
Does the structure have a valid version?
    |
    v
Is the supplied information trusted
for its intended security purpose?
    |
    v
Can it be safely mapped and consumed?
```

这并不是说 OP-TEE 的所有 Boot ABI 都使用完全相同的检查函数。

例如，对于 Arm Transfer List，4.11.0 的相关处理会验证 Register Convention、Alignment、Header，并检查传递的 DTB 地址与 Transfer List 中对应 Entry 的一致性。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这也体现了一项架构演进：

相比仅仅约定某个寄存器包含某个地址，结构化的 Boot Handoff 格式更容易加入版本、长度及兼容性检查。

不过，结构化格式也有代价：Parser 变复杂，必须严格处理越界、整数溢出、Entry 数量、对齐以及未知字段。

# **7.6 RISC-V 的真实&#x20;**`_start`**：Hart Election、Stack 与 CSR**

现在我们对照 RISC-V 实现。

这部分非常值得认真阅读，因为它既体现了 RISC-V Architecture Backend 的特点，也暴露了一些与 Arm Boot Flow 不同的设计权衡。

## **7.6.1 源码入口**

4.11.0 的主要文件为：

```
core/arch/riscv/kernel/
    entry.S
    boot.c
```

[RISC-V entry.S — 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/riscv/kernel/entry.S?utm_source=chatgpt.com)[RISC-V boot.c — 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/riscv/kernel/boot.c?utm_source=chatgpt.com)

我们先研究 `_start` 的核心状态机：

**所有进入入口的 Hart**

设置 gp · 保存 hart ID / DTB

**Atomic Hart Lottery**

获得 Boot-time Hart Index

Primary

初始化 BSS、MMU、Runtime 和全局状态

Secondary

等待 Primary · 建立本地 CPU 状态

**Boot Synchronization**

各 Hart 达到规定的初始化状态

**Boot Completion / Return to Domain Manager**

## **7.6.2 Hart ID 与 Hart Index 为什么必须区分？**

这是第一处很重要的实现细节。

RISC-V Hart ID 由平台定义。

假设系统中有四个 Hart：

```
Hart ID = 0, 2, 4, 8
```

但 OP-TEE 内部可能需要把它们映射为连续索引：

```
Core Index = 0, 1, 2, 3
```

为什么？

因为很多内核数据结构是数组：

```
thread_core_local[core_index]
```

假如直接使用 `hartid` 作为数组索引，则可能造成：

- 数组越界。
- 内存浪费。
- Per-Hart State 错位。
- 栈地址计算错误。
- Secondary CPU 初始化错误。

因此需要明确两种身份：

\\[ \text{Hart ID} \neq \text{Core Index} \\]

4.11.0 的 RISC-V `_start` 使用原子操作 `amoadd.w` 完成 Hart Lottery，并将返回的旧值保存为 Boot-time Hart Index。

源码中的关键操作为：

```
amoadd.w a3, a2, (a3)
csrw CSR_XSCRATCH, a3
```

其中，`CSR_XSCRATCH` 是与当前配置相关的宏，不应该直接把它理解为固定的 `mscratch`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **为什么要使用 Atomic，而不是普通 Load/Store？**

假设两个 Hart 同时执行：

```
index = hart_lottery;
hart_lottery++;
```

可能发生：

```
Hart A reads 0
Hart B reads 0
Hart A writes 1
Hart B writes 1
```

结果两个 Hart 都认为自己是 Primary。

这种错误可能产生灾难性后果：

- 两个 Hart 同时清零 `.bss`。
- 两个 Hart 同时初始化 Heap。
- 两个 Hart 同时构建全局 Page Table。
- 两个 Hart 同时初始化 Driver。
- 两个 Hart 同时修改 Runtime State。

因此，Primary Election 不是普通的软件协调细节，而是 **Global Initialization Exactly-Once** 的基础。

### **这里还有一个隐藏前提**

Hart Lottery 本身也依赖一个可靠的内存初始状态。

例如，`hart_lottery` 的初始值必须为 0，并且所有参与 Lottery 的 Hart 必须访问同一个、一致可见的内存位置。

4.11.0 将它放在 `.identity_map.data` 中，以便在非常早的 Boot 阶段使用，而不是依赖之后的普通 `.bss` 初始化。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



如果一个平台发生 Warm Reset，但这个同步变量的值没有恢复到预期状态，会发生什么？

这就是本章 Staff 考核会深入讨论的问题。

## **7.6.3 RISC-V&#x20;**`_start`**&#x20;为什么首先初始化&#x20;**`gp`**？**

RISC-V 的 `_start` 在最初阶段会建立 `gp`：

```
lla gp, __global_pointer$
```

并且通过 `.option norelax` 等机制控制汇编器/链接器的 Relaxation 行为。

这关系到 RISC-V Global Pointer 的初始化安全性。

有些全局数据访问可能通过 GP-relative Addressing 进行。

如果 `gp` 未初始化，而链接器生成了依赖 `gp` 的数据访问，那么早期代码可能访问错误位置。

因此，`gp` 初始化与 ARM64 的 Early Position-Relative Addressing 具有类似的目标：

**让最初的代码可以在尚未建立完整 Runtime 环境时，可靠地访问关键静态数据。**

## **7.6.4&#x20;**`reset_primary`**&#x20;做了什么？**

以 4.11.0 为基线，可以将主要路径概括为：

```
reset_primary
  |
  +-- Relocation preparation
  |
  +-- Clear BSS
  |
  +-- Set initial SATP state
  |
  +-- Setup SP / TP
  |
  +-- boot_save_args()
  |
  +-- sbi_init()       [when enabled]
  |
  +-- plat_primary_init_early()
  |
  +-- console_init()
  |
  +-- boot_mem_init()
  |
  +-- core_init_mmu_map()
  |
  +-- enable_mmu()
  |
  +-- boot_init_primary_early()
  |
  +-- boot_init_primary_late()
  |
  +-- boot_init_primary_runtime()
  |
  +-- boot_init_primary_final()
  |
  +-- Start / synchronize secondary harts
  |
  `-- Return boot completion
```

这不是所有编译配置下逐条一致的执行列表，而是按源码条件分支整理的典型流程。

源码中值得重点分析的几个操作：

### `csrw CSR_SATP, zero`

在当前 S-mode MMU 语义下，将 `satp` 清零，以建立一个已知的地址转换状态。

这里要注意：

`CSR_SATP` 是源码中的抽象写法，实际与硬件 `satp` 相关；但如果系统处于 M-mode，M-mode 自身的指令/数据访问并不通过普通 S-mode `satp` 翻译。

因此，不能把此操作简单描述成“无论 OP-TEE 当前处于什么特权级，都通过清零 satp 来关闭自己的 MMU”。

### `sp`**&#x20;与&#x20;**`tp`**&#x20;的初始化**

RISC-V 中：

- `sp` 用于当前 Stack Pointer。
- `tp` 在 OP-TEE 的这条实现路径中用于访问 Core-local State。
- `CSR_XSCRATCH` 保存当前 Boot-time Hart Index。

`tp` 对早期 C Runtime 和 Per-Hart State 非常重要。

它不是单纯的 TLS 细节。

如果 `tp` 指向错误的 Core-local Structure，后续代码就可能在错误的 Hart Context 中执行。

### `boot_save_args()`

RISC-V 路径保存 Hart ID 与 DTB Address。

对于默认实现，DTB 地址被保存到 Boot Argument State 中；后续再通过 Device Tree 初始化逻辑使用。

### `sbi_init()`

这一步也很值得关注。

对于依赖 SBI 的 RISC-V OP-TEE 路径，SBI 不是单纯用于某个驱动的附加接口。

它可能参与：

- Hart Management。
- RFENCE。
- 其他跨 Hart Firmware 服务。
- MPXY/RPMI 等平台服务。

因此，SBI 的可用性是 Boot Contract 的一部分，而不是启动成功之后才可以考虑的功能。

## **7.6.5 RISC-V S-mode 的 Secondary Hart 启动**

4.11.0 中，`boot_start_secondary_cores()` 明确使用 SBI HSM。

它首先遍历 Hart ID 列表：

```
For each candidate hart
    |
    +-- Skip current hart
    |
    +-- SBI HSM: Get Hart Status
    |
    +-- Is hart in an allowed state?
    |       |
    |       +-- No: skip
    |
    +-- SBI HSM: Hart Start
    |
    `-- Update started count
```

一个特别值得注意的实现细节：

默认代码只对 HSM 状态为 `SBI_HSM_STATE_STOPPED` 的目标 Hart 发起 Start。

在完成启动请求后，设置参与 Boot Barrier 的 Hart 数量，避免因为其他 Hart 没有实际启动而无限等待。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



但这里也产生新的问题：

**如果某个 Hart 不处于 STOPPED 状态，意味着它不属于当前 Trusted Domain，还是仅仅意味着它已经被其他 Firmware 启动？**

这两种情况在安全架构上的含义完全不同。

所以 Hart Status 只能作为运行时状态信息。

Trusted Domain 的成员资格和访问权限，仍必须由可信 Firmware 与硬件安全机制保证。

### **另一个重要的源码细节：RFENCE**

4.11.0 的 RISC-V `boot.c` 在使用 SBI 的多 Hart 配置下，会检查 `SBI_EXT_RFENCE` 是否可用。

缺少 RFENCE 时，源码会拒绝继续多 Hart 初始化。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这是一个非常好的 OS 设计案例。

为什么不是简单地打印 Warning，然后继续启动？

因为多个 Hart 上的地址转换状态可能出现不一致。

假设：

```
Hart 0 changes a page table entry
Hart 0 invalidates its local TLB

Hart 1 still has an old TLB entry
```

那么 Hart 1 可能继续使用已经失效的访问权限或映射。

这不是单纯的性能问题，而可能破坏内存隔离的正确性。

因此，**多核环境中的 TLB Coherency 能力，也是 Boot Contract 的组成部分。**

# **7.7 Primary/Secondary CPU：谁负责全局初始化？**

我们已经看到 RISC-V 的 Hart Lottery 和 HSM。

现在进一步分析整个 OS 的 SMP Boot 原则。

## **7.7.1 全局状态与本地状态必须分开**

OP-TEE Boot 中有两类初始化工作。

| Global State            | Per-CPU / Per-Hart State     |
| ----------------------- | ---------------------------- |
| Global Heap             | CPU-local Stack              |
| TA/Thread Pool Metadata | Core-local Thread State      |
| Global Memory Map       | 当前 CPU 的 Translation State   |
| Driver Registry         | Local Interrupt State        |
| Global Crypto Services  | CPU-specific Extension State |
| Initialization Calls    | Per-CPU Exception Context    |

一个非常自然的设计原则是：

**Global State 由指定的初始化协调者负责；Per-CPU State 则由每个 CPU 分别建立。**

如果在 Secondary CPU Entry 中再次执行 Global Heap 初始化，就可能破坏 Primary CPU 已经建立的内存分配状态。

反过来，如果只有 Primary CPU 完成 Per-CPU State 初始化，其他 CPU 仍然可能使用未初始化的 Context。

因此，两类初始化都不可缺少。

## **7.7.2 常见的 SMP Boot 时序**

```
Primary CPU                     Secondary CPU
    |                                |
    |                                | Wait / Park
    | Initialize global memory       |
    |                                |
    | Initialize thread pool         |
    |                                |
    | Initialize global services     |
    |                                |
    | Publish boot state             |
    |                                |
    |------------------------------->|
    |                                | Initialize local stack
    |                                | Initialize local MMU state
    |                                | Initialize local interrupt state
    |                                |
    |<-------------------------------|
    |      Secondary ready           |
    |                                |
    v                                v
 Boot completed                  Ready for service
```

上图是抽象时序。实际平台可能采用不同的 Secondary Boot Policy。

例如：

- Firmware 启动时就使全部 CPU 进入 OP-TEE Entry。
- Secondary CPU 由 PSCI CPU_ON 触发进入 Secure World。
- Primary Hart 通过 SBI HSM 启动 Trusted Domain 内的其他 Hart。
- Secure Monitor 负责独立管理 Domain 的 CPU 成员。

不能假定所有平台都必须使用同一种时序。

## **7.7.3 SMP Boot 中的三个不变量**

**Invariant A：Global Initialization 必须保持 Exactly-Once 语义。**

所有 CPU 必须看到一致的初始化结果，不能重复建立关键全局资源。

**Invariant B：Secondary CPU 进入运行时服务前，必须完成其 Per-CPU State 初始化。**

包括：

- Stack。
- Core-local State。
- Exception/Interrupt Context。
- 必要的 MMU 状态。
- 架构扩展状态。

**Invariant C：同步点必须保证内存可见性。**

某个 CPU 写入：

```
boot_ready = 1;
```

并不能在所有架构和所有缓存环境下，自动证明其他 CPU 一定能够按照预期顺序看到此前全部初始化写入。

需要分析：

- Release/Acquire Ordering。
- Cache Coherency。
- Memory Barrier。
- 是否存在不一致的内存属性。
- 是否有未进入相同一致性域的 CPU。

所以 SMP Boot 的状态同步不能只研究一个 `while (!ready)` 循环。

它必须和硬件 Memory Model 一起分析。

# **7.8 Boot 阶段的 Exception、Interrupt 与 Trap**

这个主题在很多教程中讲得比较少，但它直接影响真正的 Pre-Silicon Debug。

## **7.8.1 Early Exception 和 Runtime Exception 不是一回事**

在 Runtime 阶段，OP-TEE 已经建立：

- Exception Vector。
- Kernel Stack。
- Thread Context。
- Memory Management。
- Console。
- Abort/Panic Reporting。

因此，一个 Abort 可能产生 Stack Trace、Faulting Address 和上下文信息。

但是在 Early Boot 阶段，上述设施可能尚未全部建立。

这时异常处理程序不能假定自己可以：

```
malloc(...);
mutex_lock(...);
IMSG(...);
```

因为这些操作可能依赖尚未初始化的服务。

所以 Early Exception Handler 的设计原则是：

**尽可能少依赖 Runtime Infrastructure，并优先保存最关键的故障状态。**

## **7.8.2 Arm AArch64**

在 Arm64 中，我们关注：

- `VBAR_EL1`
- `SPSR_EL1`
- `ELR_EL1`
- `ESR_EL1`
- `FAR_EL1`
- `DAIF`
- 当前 Stack Pointer Selection

发生异常时，需要重点确定：

```
Exception Type
    |
    v
Faulting PC
    |
    v
Exception Syndrome
    |
    v
Fault Address
    |
    v
Current Stack / Exception Level
```

特别是当异常发生在 MMU Enable 附近时，`FAR_EL1` 指出的地址与此前的物理地址可能不再属于同一地址空间语义。

### **为什么 Boot 阶段经常 Mask Interrupt？**

因为即使 Interrupt Controller 已经产生 Pending Interrupt，OP-TEE 也可能尚未建立可靠的运行时中断处理环境。

因此，初始化阶段必须避免在不安全的时间接收或处理某些异步异常。

但是，Mask Interrupt 也不是无条件正确的万能方案。

如果某个硬件流程必须依赖中断完成，例如一次必须在 Boot 阶段执行的异步硬件操作，就需要保证其等待与中断策略能够工作。

这也是为什么 Boot Driver 有时会选择 Polling 模式，而不是立即使用完整的 Interrupt-driven 模型。

## **7.8.3 RISC-V**

对于 RISC-V，需要关注：

- `stvec` / `mtvec`
- `sepc` / `mepc`
- `scause` / `mcause`
- `stval` / `mtval`
- `sstatus` / `mstatus`
- `sie` / `mie`
- `sip` / `mip`

但这里还有一个 RISC-V 特别需要注意的问题。

即使 OP-TEE 运行在 S-mode，也不能只研究 `stvec`。

因为某些 Trap 可能不会直接交给 OP-TEE，而是进入上一级 M-mode Firmware。

这取决于：

- Trap Delegation。
- Interrupt Routing。
- Trusted Domain 的运行架构。
- OpenSBI/Secure Monitor 的职责。

因此，排查一次异常时应该首先问：

> 这个 Trap 在硬件层面到底应该由谁接收？

而不是看到 OP-TEE 没有 Trap Log，就直接认为 OP-TEE 的 Trap Handler 有问题。

# **7.9 Boot Completion：OP-TEE 初始化结束后发生了什么？**

现在回到本章最初提出的问题。

为什么 OP-TEE 完成 Boot 后还要返回外部组件？

## **7.9.1 Arm 经典 SMC 路径**

4.11.0 中，Arm64 非 FF-A 的 `_start` 在完成初始化后，会准备 Runtime Vector，并通过返回协议报告初始化完成。

核心流程为：

```
boot_init_primary_final()
          |
          v
Restore Temporary Stack
          |
          v
Clear Boot Thread State
          |
          v
Prepare Runtime Vector
          |
          v
TEESMC_OPTEED_RETURN_ENTRY_DONE
          |
          v
SMC to EL3 Dispatcher
```

这一点可以直接在 `entry_a64.S` 的尾部看到。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么要提供 Runtime Vector？

因为上一级 Dispatcher 在初始化完成后，需要知道后续如何进入 OP-TEE 的运行时服务。

换句话说，Boot Entry 和 Runtime Entry 的含义不同。

```
Boot Entry:
    Establish TEE OS environment

Runtime Entry:
    Handle an already initialized TEE OS request
```

如果错误地把 Boot Entry 当成 Runtime Entry，可能导致再次清零内存、重复初始化全局状态或破坏正在运行的 Session。

## **7.9.2 FF-A 路径**

FF-A 模式不是通过完全相同的 `TEESMC_OPTEED_RETURN_ENTRY_DONE` 协议工作。

4.11.0 的 Arm64 入口在相应条件下会注册 Secondary Entry Point，然后进入 FF-A Message Wait 相关路径。

因此：

**对于 FF-A，不能只复用经典 SMC Dispatcher 的 Boot Completion 语义。**

需要重新理解 Endpoint、SPMC、消息调度以及 CPU On 相关接口。

## **7.9.3 RISC-V 路径**

在 4.11.0 的 RISC-V `entry.S` 中，Primary 完成初始化后准备 `TEEABI_OPTEED_RETURN_ENTRY_DONE` 与 `thread_vector_table`，再调用 `thread_return_to_udomain`。

这说明 RISC-V Backend 也需要向外部的 Domain Management / Switch 机制交付 Runtime Entry 信息。

但具体的 Privilege Transition、寄存器恢复以及 Domain Switch 的安全性，必须结合对应 Secure Monitor 和平台 Firmware 一起验证，不能仅从返回函数名称推断整个硬件安全切换过程。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **一个重要的架构抽象**

从三个模式可以总结：

| Boot Mode             | Boot 完成时交给谁              | 后续如何运行                       |
| --------------------- | ------------------------ | ---------------------------- |
| Arm + 经典 TF-A SPD     | EL3 Dispatcher           | Runtime SMC Vector           |
| Arm + FF-A            | SPMC / FF-A 调度机制         | FF-A Message Handling        |
| RISC-V Trusted Domain | 平台 Domain/Secure Monitor | Trusted Domain Runtime Entry |

接口不同，但共同目标都是：

**完成 Boot-time Ownership 到 Runtime Service Ownership 的转换。**

# **7.10 从安全架构角度审视 Boot Handoff**

到这里，我们已经能追踪 Boot 源码。

但 Staff Engineer 需要进一步评估：系统真的安全了吗？

## **7.10.1 启动链中的五类核心安全不变量**

| 安全不变量                   | 必须保证的条件                          | 常见失败后果           |
| ----------------------- | -------------------------------- | ---------------- |
| Image Authenticity      | 只有授权镜像可以进入安全执行域                  | 恶意 Secure Kernel |
| Execution Privilege     | OP-TEE 在预期的特权/安全域执行              | 非法 CSR 访问、隔离失效   |
| Physical Isolation      | 不可信 CPU/设备不能任意访问 Secure RAM      | 代码和密钥遭篡改         |
| Address Mapping         | 运行时 VA/PA、权限、Memory Attribute 一致 | Abort、越权映射       |
| Initialization Ordering | 所有共享和本地状态遵守依赖关系                  | Race、状态损坏、死锁     |

这里需要特别强调：

**OP-TEE 自己配置的 CPU MMU 权限，并不等价于整个 SoC 的物理资源隔离。**

例如在 RISC-V 平台中：

```
RISC-V CPU
    |
    +-- CPU Translation / Page Permission
    |
    +-- PMP / Domain Protection
    |
    +-- System Memory Protection
    |
    `-- DMA / Device Access Control
```

不同层级解决的问题不同。

只有正确配置所有相关访问路径，才有可能建立完整的 Memory Isolation。

## **7.10.2 Firmware Handoff 中最危险的信任错误**

我认为有三类尤其值得关注。

### **错误一：把 Boot Parameter 当作安全策略**

Bootloader 传来 Secure Memory 的描述，并不意味着这个描述天然可信。

必须明确传递者是否处于可信启动链中，数据能否在 Handoff 前后被不可信主体修改。

### **错误二：只验证 CPU 访问，不验证 DMA 访问**

即使 Linux 不能通过普通 CPU Load/Store 访问 Secure RAM，某个不受约束的 PCIe、DMA 或其他 Bus Master 仍可能访问。

因此，需要审查 Secure RAM 的 System-level Protection，而不只是 Page Table。

### **错误三：忽略 Warm Boot 与 Secondary Entry**

Cold Boot 已经建立了完整的安全状态。

但如果 Secondary CPU 或 Resume 路径没有恢复等效的安全状态，就可能出现：

- 某个 CPU 使用陈旧的 Translation Table。
- 某个 Hart 进入错误的 Domain。
- 某个 CPU 的 Exception Vector 不正确。
- 某个设备在 Resume 后恢复了不安全的访问权限。

因此，安全性不能只在 Primary Cold Boot 路径中验证。

# **7.11 工业场景：OP-TEE Early Boot 问题应该如何定位？**

下面我们不再按照函数名称讲，而是按照真实工程中的症状进行分析。

这也是你在 QEMU、ZeBu、Palladium 或真实 SoC 上更容易遇到的场景。

## **场景 A：OP-TEE 完全没有任何 Console 输出**

假设启动停在：

```
TF-A: Entering BL32
```

之后再没有任何 OP-TEE 日志。

很多人的第一反应是 UART 配置错误。

但作为负责 Boot 架构的工程师，应该建立以下假设：

| 假设             | 需要验证的内容                               |
| -------------- | ------------------------------------- |
| H1：没有进入 OP-TEE | Firmware 跳转地址、EL/Privilege State      |
| H2：进入后立即异常     | 当前 PC、Exception Vector、Fault Register |
| H3：早期访问无效      | Boot PA、Image Layout、Memory Mapping   |
| H4：栈未建立        | SP、Stack Boundary、Linker Symbol       |
| H5：Console 问题  | UART Clock、Reset、MMIO Mapping         |
| H6：其他 CPU 干扰   | Primary Election、BSS 初始化、同步           |
| H7：镜像与配置不匹配    | Load Address、Relocation、Linker Script |

### **推荐调试顺序**

```
1. Check the entry PC
        |
2. Confirm current privilege state
        |
3. Confirm instruction fetch succeeds
        |
4. Single-step early assembly
        |
5. Check stack and global pointer
        |
6. Check first C function call
        |
7. Check MMU initialization
        |
8. Only then investigate Console
```

这里最重要的方法是：

**不根据最后一条日志猜测故障点，而是确认最后一个已经成功执行的状态转换。**

例如，如果 PC 已经进入 `console_init()`，则可以缩小范围。

如果 PC 甚至没有进入 `_start`，就没有必要先研究 OP-TEE 的 UART Driver。

## **场景 B：单核正常，多核启动死锁**

假设：

```
1 Hart  : OP-TEE boots successfully
4 Harts : OP-TEE hangs during boot
```

首先建立假设矩阵：

| 可能问题             | 具体排查方向                                 |
| ---------------- | -------------------------------------- |
| Primary Election | 是否出现多个 Primary                         |
| Hart Index       | 是否错误地使用 Hart ID 作为 Index               |
| BSS Race         | Secondary 是否访问了未初始化的全局数据               |
| Boot Barrier     | 等待数量是否与实际启动数量一致                        |
| Memory Ordering  | 共享状态是否可见                               |
| Per-Hart Stack   | 是否分配了重叠的 Stack                         |
| Local Interrupt  | Secondary 是否在错误阶段接收中断                  |
| RFENCE           | 是否缺乏跨 Hart Translation Synchronization |
| Domain Ownership | Secondary 是否真正属于 Trusted Domain        |

这里最具有 Staff Engineer 价值的设计问题是：

> 如果实际能够进入 Trusted Domain 的 Hart 数量小于 Firmware/DTB 声明的数量，系统应该 Fail Closed，还是允许部分 Hart 启动？

答案不能仅根据性能需求决定。

还需要区分：

- 是否存在系统级安全要求，必须所有 Hart 都参与保护。
- 是否可以在减少 CPU 数量后保证所有安全不变量。
- 共享资源是否依赖固定的 Hart 拓扑。
- 失败的 Hart 是否可能继续访问 Trusted Memory。
- 软件是否支持动态调整参与 Boot Barrier 的 CPU 数量。

只有回答这些问题，才能合理选择降级启动还是拒绝启动。

## **场景 C：打开 MMU 后立即发生 Abort**

假设在 RISC-V 平台上：

```
core_init_mmu_map(): success

enable_mmu():
    csrw satp, ...
    sfence.vma

Immediate instruction fault
```

需要验证：

1. 当前 PC 是否位于有效的 Identity Mapping。
2. `satp` 是否指向正确的 Root Page Table。
3. Page Table 的内存是否已经初始化并对当前 Hart 可见。
4. 入口代码的 Execute Permission 是否正确。
5. Core ASLR 是否改变了 VA。
6. `gp`、`sp`、`tp`、`ra` 是否正确迁移。
7. PMP 或其他安全机制是否阻止了 Page Table Walk。
8. 失败是否发生在第一个高地址跳转处。

还要注意，当代码位于 M-mode 时，需要按照 M-mode 真实的地址访问和转换语义进行分析，而不是机械套用 S-mode `satp` 的行为。

这个场景的核心方法是：

**同时检查 CPU Translation State、Memory Protection State 和当前执行地址。**

仅仅检查 Page Table 内容不足以解释整个问题。

## **场景 D：OP-TEE 初始化成功，但 Normal World 无法运行**

假设日志显示：

```
I/TC: Primary CPU switching to normal world boot
```

但 Linux 完全没有启动。

此时需要检查两个子系统的边界：

**OP-TEE 是否正确完成 Boot Completion？**

例如：

- Runtime Vector Address 是否正确。
- Boot Thread 是否已经清理。
- 是否仍然处于错误的 Exception Mask 状态。
- 返回协议是否与 Dispatcher 一致。
- 是否需要转换 Runtime Vector 的 VA/PA。

**上一级 Firmware 是否正确接收完成信号？**

例如：

- 返回 Function ID 是否正确。
- Secure/Normal World Context 是否有效。
- Normal World Entry Point 是否正确。
- 是否正确恢复 CPU Security State。
- PSCI/SBI/Domain Switch 的状态是否一致。

这个问题绝不能简单归结为“OP-TEE 启动成功，所以问题一定在 Linux”。

因为 OP-TEE 的 Boot Completion 本身就是一个跨组件协议。

# **7.12 实战练习：从第一条 Assembly 一直追踪到 C**

为了真正掌握本章，我建议完成以下三个实验。

## **Lab 1 — 静态源码执行路径还原**

目标：不依赖实际硬件，先建立准确的 Boot Call Graph。

在 4.11.0 源码目录执行：

```
git checkout 4.11.0

rg -n 'FUNC _start' core/arch/
rg -n 'boot_init_primary_' core/arch/
rg -n 'boot_save_args' core/arch/
rg -n 'enable_mmu' core/arch/
rg -n 'thread_clr_boot_thread' core/arch/
```

然后分别绘制：

```
Arm64:
_start
  |
  +-- ...
  |
  `-- Boot Completion
```

```
RISC-V:
_start
  |
  +-- Primary / Secondary
  |
  `-- Boot Completion
```

### **验收要求**

需要标记：

- 每个关键函数运行时 MMU 是否开启。
- 当前使用的 Stack 类型。
- 当前 CPU 的特权状态。
- 当前是否具备有效 Thread Context。
- 是否允许普通 Kernel Memory Allocation。
- 是否允许处理 Interrupt。
- 当前访问的地址属于 PA 还是 VA。
- 哪些函数只允许 Primary CPU 调用。

这里建议采用一个明确配置进行分析，例如 `CFG_CORE_FFA=n`、`CFG_NS_VIRTUALIZATION=n` 的 Arm64 经典启动路径；再单独研究不同配置的差异。

否则很容易把不同编译分支的代码混成一条不存在的执行路径。

## **Lab 2 — 使用 GDB 观察 Early Boot**

如果使用已经配置好的 QEMU Arm64 OP-TEE 实验环境，可以通过 QEMU GDB Stub 调试。

典型命令形式：

```
qemu-system-aarch64 ... -S -s
```

GDB：

```
set architecture aarch64
file path/to/tee.elf
target remote :1234

break _start
continue
```

实际能否命中 `_start` 取决于 QEMU 的启动方式、镜像装载地址、符号与当前 PC 对应关系；必要时应在 Firmware Handoff 的实际地址设置硬件断点，而不是直接假定 ELF Symbol 地址就是运行时 PA。

建议观察：

```
info registers
info registers sp
x/12i $pc
x/16gx $sp
```

在 AArch64 支持的 GDB Target 上，还可以观察：

```
p/x $vbar_el1
p/x $sctlr_el1
p/x $ttbr0_el1
```

不同 GDB Stub 暴露的系统寄存器可能不同，如果不能直接读取，应使用固件辅助调试、Monitor 指令或其他调试接口。

### **实验重点**

分别在以下三个位置暂停：

```
Before MMU Enable

Immediately After MMU Enable

Before boot_init_primary_runtime()
```

记录：

| 状态               | MMU 前 | MMU 后 | Runtime 前 |
| ---------------- | ----- | ----- | --------- |
| PC               |       |       |           |
| SP               |       |       |           |
| Exception Vector |       |       |           |
| Page Table Root  |       |       |           |
| Boot Arguments   |       |       |           |
| Current Thread   |       |       |           |

把这张表填完整，比单纯让 OP-TEE 启动成功更有学习价值。

## **Lab 3 — RISC-V Hart Boot State Machine**

目标：理解 Hart Election、Boot Barrier 与 SBI HSM 的关系。

建议先通过源码静态分析建立：

```
Hart ID
  |
  v
Hart Lottery
  |
  v
Core Index
  |
  v
Per-Hart Stack / TP
  |
  v
Primary or Secondary
  |
  v
Boot Barrier
  |
  v
Runtime Entry
```

然后选择以下实验条件：

| 测试条件            | 预期研究目标               |
| --------------- | -------------------- |
| 1 Hart          | 基础启动                 |
| 2 Harts         | Primary/Secondary 时序 |
| 非连续 Hart ID     | ID 与 Index 的正确性      |
| 一个 Hart 未启动     | Boot Barrier 是否能正常处理 |
| SBI HSM 不可用     | 多 Hart 初始化是否正确失败     |
| SBI RFENCE 不可用  | 是否拒绝不安全的多 Hart 运行    |
| Secondary 初始化异常 | 是否出现永久等待，如何诊断        |

可以先进行代码审查和故障模型推演，再使用支持相应 OP-TEE RISC-V 配置的模拟或仿真环境验证。并不是所有通用 QEMU RISC-V 配置都直接提供所需的 Trusted Domain 隔离和外部 Domain Switch 能力。

### **Lab 最终提交物**

不需要写非常长的实验报告，但建议完成：

**一张 Boot State Machine、一张 Register Transition Table、一张 Memory Ownership Map，以及一份故障注入结果。**

# **7.13 Chapter 07 — Staff Engineer 面试与架构思考题**

12 道 · 架构设计、源码推理、安全评估与工业故障分析

下面这些问题不以记忆函数名称为主要目标。

每道题都尽量考察四种能力：

- 能否识别系统真正的约束。
- 能否提出多个合理的技术假设。
- 能否在不同架构方案之间进行权衡。
- 能否设计可以验证假设的实验或证据链。

建议先独立思考，再展开参考分析。

点击题目展开参考分析与考核重点

展开全部

01设计一个跨 Arm/RISC-V 的最小 Boot ContractArchitecture Design你负责设计一套新的 OP-TEE Boot ABI，同一套通用 Core 要支持 Arm TrustZone、RISC-V M-mode Secure Monitor 和 RISC-V S-mode Trusted Domain。你会保留哪些必需契约，哪些属于可选扩展？如果不同 Firmware 提供不同能力，如何保证长期 ABI 兼容？

02全部 CPU 同时进入 \_start，怎样证明只执行一次全局初始化？SMP & Memory Model一个 64-Hart SoC 没有传统的 Primary CPU，64 个 Hart 可能同时进入 OP-TEE Entry。你需要设计 Primary Election、Core Index 分配和初始化屏障。平台还可能发生 Warm Reset。应该怎样建立正确性证明？

03OP-TEE 通过所有功能测试，但 Secure RAM 实际可由 DMA 访问Security Architecture项目已经完成 OP-TEE 启动，Linux 与 xtest 均正常。安全测试发现某个 Non-Secure DMA Master 可以读取 OP-TEE Heap。谁应该修复？你如何界定责任，怎样防止下次回归？

04MMU Enable 后的第一条指令发生 FaultArchitecture DebuggingOP-TEE 一直执行到 enable_mmu()，写入 Translation Control 后立即 Fault。你不能增加 Printk，也不能依赖完整异常栈。请给出最小诊断方案。

05一个恶意 Bootloader 是否可以通过 DTB 攻击 OP-TEE？Threat Modeling上一级 Bootloader 可以修改 DTB，但无法修改已认证的 OP-TEE 镜像。DTB 包含 RAM Layout、UART 和安全设备信息。你认为攻击者仍可能获得什么能力？如何设计可靠的输入边界？

06为什么不能复用 Firmware 的 Stack 和 Page Table？OS Architecture为了缩短 Boot Latency，有人建议 OP-TEE 继续使用 BL31/OpenSBI 已经创建的 Stack 和 Page Table，而不再建立自己的执行环境。你是否支持？在什么严格条件下可能可行？

07RISC-V Trusted Domain 中 Secondary Hart 无法启动Firmware / SBI Integration四个 Hart 中，Primary 正常，Secondary 调用 SBI HSM 时返回错误。Linux 仍能使用所有 Hart。你怎样判断问题属于 OP-TEE、OpenSBI、Trusted Domain 配置还是硬件？

08多核启动时缺失 SBI RFENCE，是否可以降级运行？Correctness vs Availability系统部署时发现 Firmware 不支持 RFENCE，但产品必须启动。有人建议跳过 RFENCE 检查，改为每个 Hart 执行本地 sfence.vma。你是否批准？

09Boot Completion 已经返回成功，但后续 Runtime Entry 无法工作Cross-component ABIOP-TEE 首次初始化成功并通知 Secure Monitor，但 Linux 第一次发起 OpenSession 就导致异常。你如何区分 Boot 初始化错误、Runtime Vector 错误与 Domain Switch 错误？

10新增一个 Secure Monitor Handoff Protocol，如何保证未来兼容？Interface Evolution现有 Boot ABI 使用四个寄存器，已经不足以传递新的 Secure Domain Information、Memory Map 和 Measurement Log。你准备设计一个新版本。采用更多寄存器还是内存描述符？如何防止版本不匹配造成安全问题？

11Warm Resume 时，为什么不能重新执行完整 Cold Boot？System State & Lifecycle系统支持低功耗。Resume 后有人建议直接跳转到 OP-TEE \_start，复用 Cold Boot 初始化流程。这样是否正确？如果你要设计真正可靠的 Resume Path，需要保留和恢复哪些状态？

12如何证明 OP-TEE Boot Chain 的安全性，而不是只证明它能启动？End-to-End Security Assurance你作为 SoC Security Lead，需要在芯片流片前完成 OP-TEE Boot Handoff 的验收。验证团队只提供了“启动成功、xtest 通过”的报告。你会要求补充哪些证据？

## **如何使用这些题目考察自己？**

建议采用一个比普通面试更严格的评分标准。

**Staff Engineer 自评模型**

# **8 / 16**

综合得分

问题建模

2 / 4

是否识别了全部关键前提、责任边界及不可忽略的约束

架构推导

2 / 4

是否能够从原理推导出方案，而不是只复述现有实现

权衡能力

2 / 4

是否分析了性能、复杂度、兼容性与安全性的取舍

验证能力

2 / 4

是否提出可执行的实验、可观察状态和成功判据

需要进一步提高系统性推导和风险分析能力

0 分：无法分析；1 分：列出概念；2 分：解释主要机制；3 分：系统分析并权衡；4 分：能够提出可验证的设计与完整证据链。

# **7.14 本章总结：Boot Entry 的本质到底是什么？**

通过这一章，我们应该形成一个完整的架构认识。

OP-TEE Boot 不是简单执行：

```
_start -> main -> init
```

而是一个有明确前提条件和安全边界的状态转换过程：

\\[ \text{Firmware Environment} \rightarrow \text{Minimal Execution Environment} \rightarrow \text{Protected Kernel Environment} \rightarrow \text{Runtime Service Environment} \\]

每次转换，都必须考虑：

- 当前 CPU 有什么权限。
- 当前内存是否得到硬件保护。
- 当前能够调用哪些内核服务。
- 当前可以访问哪些地址。
- 当前使用哪个栈和执行上下文。
- 哪些状态只允许初始化一次。
- 哪些状态需要逐 CPU 建立。
- 如何处理失败和异常。
- 如何证明转换前后的安全不变量仍然成立。

我希望你在完成 Chapter 07 之后，能够特别牢固地掌握三个结论。

**第一，Firmware Handoff 是一份安全契约，而不是一次普通函数调用。** OP-TEE 的正确性依赖调用方与被调用方对 CPU、Memory、Privilege 和 ABI 的一致理解。

**第二，Early Boot 是逐步建立运行时能力的过程。** Stack、MMU、Exception、Thread、Heap 等机制有严格的依赖关系，不能提前调用尚未建立的服务。

**第三，Boot 正确性的最高标准不是“系统成功启动”，而是从 Boot ROM 到 OP-TEE Runtime 的每一次状态转换，都能够证明其行为、安全边界和资源所有权符合预期。**

## **本章源码阅读清单**

| 文件                                 | 建议掌握程度                             |
| ---------------------------------- | ---------------------------------- |
| `core/arch/arm/kernel/entry_a64.S` | 逐阶段追踪 Assembly                     |
| `core/arch/arm/kernel/boot.c`      | 理解 Boot Initialization Call Graph  |
| `core/arch/riscv/kernel/entry.S`   | 理解 Hart Election 与早期 CSR           |
| `core/arch/riscv/kernel/boot.c`    | 理解 SBI、Secondary 和 Boot Completion |
| `core/include/kernel/boot.h`       | 理解 Boot Interface                  |
| `core/mm/core_mmu.c`               | 理解 Early MMU 配置的输入输出               |
| `core/arch/*/kernel/thread*`       | 理解 Boot Thread 与 Runtime Entry     |

**下一章 Chapter 08 — Early Runtime、Stack 与 C 环境建立**，可以在本章基础上进一步研究一个非常有价值的问题：

为什么有时一段普通 C 代码在 Runtime 完全正确，但放进 Early Boot 就会崩溃？

这将带我们从汇编、C ABI、Linker、Global Pointer、Stack、BSS、Relocation、Compiler Instrumentation 和异常处理依赖关系的角度，真正理解 OS 如何从“几条能执行的指令”演化为“能够可靠执行 C 程序的内核”。
