# **Chapter 12 — OP-TEE Platform Initialization & Device Tree Integration**

OP-TEE OS Internals · Advanced / Staff Engineer Level

**源码基线：** OP-TEE 4.11.0\
**研究范围：** Platform Abstraction、Device Tree、Driver Model、Memory Mapping、Hardware Resources、Security Boundary

## **本章的核心目标**

这一章要解决的，不仅仅是“OP-TEE 如何解析 Device Tree”，而是一个更本质的问题：

> **OP-TEE 作为一个安全操作系统，如何从平台描述中发现硬件资源，将资源转换为内核可使用的对象，并确保这一过程不会破坏整个 SoC 的安全隔离？**

我们将围绕四个核心概念展开：

1. **Platform Description：** 硬件资源在哪里、有哪些属性、与哪些组件相互依赖？
2. **Resource Management：** OP-TEE 如何初始化这些资源，并通过驱动向内核提供服务？
3. **Resource Ownership：** 哪些资源属于 Secure World，哪些属于 Normal World，哪些可以共享？
4. **Security Enforcement：** 平台配置和硬件访问控制之间，究竟由谁真正保证隔离？

第四点尤其重要。

对于 OP-TEE 这类安全内核，正确解析 Device Tree 并不等于系统已经安全。

## **12.0 先建立完整的架构认识**

假设一个 SoC 包含：

- 16 个 RISC-V Hart。
- 一个独立的安全控制器。
- Secure SRAM 和 Non-Secure DDR。
- UART、Crypto Engine、OTP、TRNG。
- Clock、Reset、Interrupt Controller。
- 支持安全隔离的系统级 Memory Protection Controller。

现在 OP-TEE 需要使用其中的 TRNG 和 Crypto Engine。

那么，OP-TEE 必须解决什么问题？

从硬件描述到安全服务

**Trusted Firmware / Boot Configuration**

建立硬件隔离 · 加载可信配置 · Handoff

**Device Tree / Platform Configuration**

Reg · IRQ · Clock · Reset · Device Ownership

**OP-TEE Platform & Driver Framework**

Parse · Match · Probe · Resolve Dependencies

MMU / MOBJ

Address Mapping

IRQ Framework

Interrupt Routing

Clock / Reset

Hardware Lifecycle

Driver Context

Resource Ownership

**OP-TEE Kernel Services**

Crypto · RNG · Secure Storage · PTA

请先记住这个重要区别：

| 机制                                | 它主要解决什么问题           |
| --------------------------------- | ------------------- |
| Device Tree                       | 描述硬件及其依赖            |
| Driver Framework                  | 发现、匹配、初始化硬件资源       |
| CPU MMU                           | 控制 CPU 的虚拟地址转换与访问权限 |
| Platform Firewall / PMP / SMMPT 等 | 在各自保护范围内限制硬件访问主体    |
| Secure Firmware                   | 建立启动期的权限配置与信任链      |
| OP-TEE Kernel                     | 使用资源、维护资源状态、执行安全服务  |

其中，Device Tree 提供的是**描述和配置输入**，而不是天然可信的访问授权。

这将是贯穿整个 Chapter 的第一条安全原则。

# **12.1 Platform Code 与 Generic Core 的职责边界**

## **12.1.1 为什么 OP-TEE 需要 Platform Layer？**

先比较一下 OP-TEE 和一个典型的 RTOS。

在 NuttX 或 Zephyr 中，BSP 往往负责：

- SoC Clock 和 Pinmux 初始化。
- Memory Initialization。
- Interrupt Controller 初始化。
- UART 和 Timer 初始化。
- Board-specific Peripheral Initialization。

OP-TEE 同样需要类似的基础能力，但有一个本质区别：

**OP-TEE 的平台初始化还必须维持安全世界和非安全世界之间的资源隔离约束。**

比如：

- Secure UART 可能只能由安全世界访问。
- Crypto Engine 可能对两个世界开放，但需要资源仲裁。
- Secure SRAM 必须防止 Normal World 访问。
- Secure Interrupt 必须路由到正确的安全执行环境。
- Clock/Reset Controller 可能同时服务安全设备和非安全设备。

因此，平台初始化不仅是让硬件工作，还要确保硬件在正确的权限和所有权模型下工作。

## **12.1.2 OP-TEE Platform Layer 的组织**

以 Arm 平台为例，典型源码结构是：

```
optee_os/
└── core/
    ├── arch/
    │   ├── arm/
    │   │   ├── kernel/
    │   │   ├── mm/
    │   │   ├── plat-*/
    │   │   │   ├── main.c
    │   │   │   ├── platform_config.h
    │   │   │   ├── conf.mk
    │   │   │   └── ...
    │   │   └── dts/
    │   └── riscv/
    │       └── ...
    ├── kernel/
    │   ├── dt.c
    │   ├── dt_driver.c
    │   └── console.c
    ├── drivers/
    └── include/
        └── kernel/
            ├── dt.h
            └── dt_driver.h
```

该结构体现了三个层次：

**Architecture Layer**

负责 CPU 特有的能力，例如 Trap、Exception、MMU、Cache、CPU Context、Interrupt Entry。

**Platform Layer**

负责具体 SoC 的硬件布局、启动入口约定、特定初始化流程、系统安全资源配置。

**Generic Kernel / Driver Layer**

提供通用内核服务、DT 解析、驱动匹配、Clock/Reset Provider、资源管理和共享的硬件驱动实现。

上游 `core/kernel/dt.c`、`core/kernel/dt_driver.c` 和 STM32MP 平台代码清晰展示了这样的职责拆分。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



## **12.1.3 哪些代码应该放在 Platform，哪些不应该？**

举个例子：你正在为新的 RISC-V SoC 增加 TRNG 支持。

可能有两种实现方案。

**方案 A：**

在 `core/arch/riscv/plat-xxx/main.c` 中：

- 查找 TRNG 寄存器。
- 映射 MMIO。
- 设置 Clock。
- 写 TRNG 控制寄存器。
- 读取随机数。
- 直接给 Crypto Framework 使用。

这种方案对于早期 Bring-up 看起来很直接。

但问题是：

- 将硬件功能和平台初始化紧密耦合。
- 其他 SoC 难以复用。
- Clock/Reset 等资源接口可能重复实现。
- 测试与错误注入困难。
- Crypto Framework 依赖具体平台实现。
- 后续硬件驱动升级需要修改 Platform Code。

**方案 B：**

```
Platform Configuration
        |
        v
Device Tree
        |
        v
TRNG Driver
        |
        v
Generic RNG / Crypto Interface
        |
        v
TEE Kernel Services
```

Platform Layer 负责：

- 指定 TRNG 所属的安全域。
- 描述寄存器、Clock、Reset、IRQ。
- 提供必要的 SoC-specific 初始化和安全控制。

TRNG Driver 负责：

- 设备探测与初始化。
- 寄存器访问。
- 硬件状态与错误检测。
- 从硬件获取随机数。

Generic Crypto/RNG Framework 负责：

- 向上层提供稳定接口。
- 执行算法或熵源管理所需的检查。
- 处理不同 Backend 的选择与集成。

**方案 B 通常更符合长期维护的方向。**

不过不能把这个原则绝对化。某些安全硬件必须在 Heap、MMU 或 DT Driver Framework 可用之前初始化，此时平台 Early Boot 代码仍然有其必要性。

真正应该追求的是：

> 将不可避免的平台差异保留在平台层，将可复用的机制放在 Generic Core 或 Driver Framework 中。

## **12.1.4 Platform Layer 本身也有安全边界**

还有一种常见误区：

“只要某个寄存器在 Secure World 中访问，就把它归到 OP-TEE Platform Code。”

这并不总是正确。

例如，一个控制全 SoC 内存安全归属的 Firewall Controller，其最终权限可能由更高特权级的安全固件管理。

如果 OP-TEE 无权修改这个 Firewall，那么正确的设计可能是：

```
Root of Trust / Secure Firmware
          |
          | Configure hardware permissions
          v
System Memory Protection
          |
          | Enforce access rules
          v
OP-TEE Driver / MMU
          |
          | Access permitted resource
          v
Secure Service
```

而不是让 OP-TEE 自己直接改变全部系统安全归属。

在 RISC-V 上，是否由 OpenSBI、M-mode Security Monitor、独立 HSM/PuC 或 OP-TEE 管理某个保护控制器，取决于 SoC 的特权和安全架构。

因此，**平台代码应该首先遵守 Firmware-to-OS Contract，而不是假定 OP-TEE 拥有所有安全控制权。**

# **12.2 Secure Device Tree 与 Non-Secure Device Tree 的信任模型**

这是本章最重要的部分之一。

## **12.2.1 Device Tree 不是只有一份**

很多开发者第一次使用 DT 时，形成的模型是：

```
Bootloader
    |
    v
device.dtb
    |
    v
Operating System
```

但在一个拥有多个执行环境的 SoC 中，情况要复杂得多。

```
                 Boot Firmware
                       |
              +--------+---------+
              |                  |
              v                  v
       Secure DTB           Non-Secure DTB
              |                  |
              v                  v
          OP-TEE                Linux
              |                  |
              v                  v
       Secure Drivers       Linux Drivers
```

这只是逻辑图。具体系统可能采用一份共享 DTB、嵌入式 Secure DTB、外部 DTB 或 Firmware Manifest，而不是一定有两份独立 DTB。

OP-TEE 官方文档明确区分了 Embedded Secure DT、Early Boot External DT，以及向 Non-Secure World 提供的 Device Tree Overlay。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **12.2.2 Secure DTB 为什么需要独立的信任来源？**

假设下面的配置被 OP-TEE 用于初始化 Crypto Engine：

```
crypto@84000000 {
    compatible = "vendor,secure-crypto";
    reg = <0x0 0x84000000 0x0 0x10000>;
    status = "disabled";
    secure-status = "okay";
};
```

这段配置包含：

- 设备类型。
- MMIO 地址范围。
- Normal World 是否应该启用设备。
- OP-TEE 是否应该启用设备。

但它没有证明这些信息可信。

假设攻击者能够修改 OP-TEE 将要解析的 DTB，将 `reg` 改成另一个敏感的 MMIO 地址。

如果驱动简单地执行：

```
va = core_mmu_add_mapping(MEM_AREA_IO_SEC, pa, size);
```

那么就可能把攻击者选择的寄存器区域映射进 Secure Kernel。

这并不意味着映射一定成功，也不意味着硬件必然允许访问；但如果系统缺少额外的校验与隔离措施，就可能造成危险的访问路径。

**因此，DTB 的来源可信度与 DTB 本身的语法正确性，是两个完全不同的问题。**

## **12.2.3 OP-TEE 4.11 的&#x20;**`get_dt()`**&#x20;与&#x20;**`get_secure_dt()`

这两个接口值得专门研究。

源码：`core/kernel/dt.c`

其主要选择逻辑可以概括为：

```
/* 讲解性简化伪代码 */

void *get_dt(void)
{
    if (embedded_dt)
        return embedded_dt;

    if (external_dt)
        return external_dt;

    return manifest_dt;
}

void *get_secure_dt(void)
{
    if (embedded_dt)
        return embedded_dt;

    if (CFG_MAP_EXT_DT_SECURE && external_dt)
        return external_dt;

    return manifest_dt;
}
```

以上是概念性重写，不是逐字复制源码。

注意第二个函数：

**普通 External DT 并不会自动成为 Secure Driver 的可信配置源。**

在 4.11 实现中，`get_secure_dt()` 优先使用 Embedded DT；只有开启 `CFG_MAP_EXT_DT_SECURE` 才允许选择符合该配置要求的外部 DT，随后才考虑 Manifest DT。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



这里有一个非常重要的安全判断：

`CFG_MAP_EXT_DT_SECURE=y` 表示实现预期外部 DT 位于安全内存，并按照相应的方式使用。

**它不能单独证明 DT 的内容已经经过认证，也不能代替 Boot Chain 对 DT 的真实性和完整性的保证。**

例如：

- DTB 是否包含在已认证的 Boot Image 中？
- DTB 是否由可信 Firmware 写入？
- 写入完成后，是否有低可信实体仍能修改它？
- OP-TEE 解析期间是否存在并发修改的可能？
- Bootloader 的 DT 版本是否与 OP-TEE 驱动匹配？

这些问题必须由完整的信任链来回答。

## **12.2.4&#x20;**`status`**&#x20;与&#x20;**`secure-status`

OP-TEE 使用的 DT 状态语义值得特别注意。

| `status`   | `secure-status` | OP-TEE 解析后的基本含义      |
| ---------- | --------------- | -------------------- |
| `okay`     | `okay`          | 两个世界均标记为可用           |
| `okay`     | `disabled`      | 仅 Normal World 标记为可用 |
| `disabled` | `okay`          | 仅 Secure World 标记为可用 |
| `disabled` | `disabled`      | 两个世界均标记为不可用          |
| 缺省         | 缺省              | 默认视为两个世界均可用          |

其中，如果 `secure-status` 不存在，OP-TEE 的 `fdt_get_status()` 会使它默认继承普通 `status` 的有效状态。源码返回 `DT_STATUS_OK_SEC` 和 `DT_STATUS_OK_NSEC` 的组合。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此：

```
status = "disabled";
secure-status = "okay";
```

可以告诉 OP-TEE：

“这个设备应该由 Secure World 初始化，而普通 Linux 不应该绑定它。”

但是要特别注意：

**Linux 尊重&#x20;**`status = "disabled"`**，并不等于恶意 Linux 被硬件禁止访问该设备。**

Device Tree 控制的是软件发现和驱动行为。

真正的硬件访问限制，还必须依赖 SoC Security Controller、Memory Firewall、PMP 或其他对应的硬件保护机制。

这就是本章的第二条安全原则：

> Software Resource Description 与 Hardware Security Enforcement 必须分开理解。

## **12.2.5 一个安全设计应该满足什么条件？**

假设 Crypto Engine 完全属于 Secure World。

至少应该验证以下不变量：

| 不变量                         | 具体含义                              |
| --------------------------- | --------------------------------- |
| Authentic Configuration     | OP-TEE 使用的资源配置来自可信来源              |
| Exclusive Hardware Access   | 不允许的主体无法访问该硬件                     |
| Correct MMIO Mapping        | 驱动映射的物理地址确实属于目标设备                 |
| Consistent Device Ownership | Linux、Firmware、OP-TEE 对资源所有权的理解一致 |
| Controlled Reconfiguration  | 低可信实体不能随意改变 Clock、Reset、安全配置      |
| Safe Failure                | 配置错误时不允许静默退回到不安全状态                |

最后一条经常被忽略。

假设 Secure DT 丢失，系统自动改用 Linux 提供的 DT，并继续启动。

从可用性角度看似乎很好。

但从安全角度看，这可能意味着：**因为可信输入不可用，系统反而开始接受不可信输入。**

这类自动回退必须经过明确的威胁模型审查。

# **12.3 Early DT、Embedded DT 与 Boot Configuration**

## **12.3.1 为什么需要多个 DT 交付方式？**

平台初始化会跨越不同 Boot 阶段，而不同阶段拥有不同的资源能力。

例如：

| Boot 阶段             | 典型能力与限制                                      |
| ------------------- | -------------------------------------------- |
| Early Firmware      | 最早获得硬件和平台信息，但运行环境可能受限                        |
| Early OP-TEE        | Stack、MMU、Allocator、Driver Framework 尚未全部准备好 |
| OP-TEE Runtime Init | 可以进行完整 DT Parsing、Driver Probe 和资源注册         |
| Linux Boot          | 使用 Normal World DT 发现设备并初始化普通驱动              |

这意味着一份设备描述可能需要满足两种不同目标：

**启动期描述：** 为 Bootloader、Firmware 和 Linux 传递必要的系统信息。

**安全运行期描述：** 为 OP-TEE 提供可信的安全硬件资源配置。

两者可以重叠，但不应该无条件等同。

## **12.3.2 Early Boot External DT**

在经典 Arm Boot 流程中，上一级 Firmware 或 Bootloader 可以通过启动约定向 OP-TEE 提供一个 External DTB 地址。

官方文档描述了通过启动参数 `R2/X2` 或 `CFG_DT_ADDR` 传递外部 DTB 的方式。这是对应 Arm 启动接口的行为，不能直接推广为 RISC-V 的统一寄存器约定。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



例如：

```
TF-A / Bootloader
       |
       | External DTB PA
       v
OP-TEE Early Boot
       |
       | Map / Validate DTB
       v
Platform Initialization
       |
       | Prepare Normal World DT
       v
Linux Boot
```

Early DT 有一个重要的生命周期问题。

OP-TEE 官方文档说明，早期外部 DTB 主要在初始化期间使用，因为其所在的内存之后可能被其他运行时数据覆盖。

所以，不能简单假定：

“启动时拿到了一个指向 DTB 的指针，以后任意 Kernel Driver 都可以永久保存这个指针。”

如果一个 Runtime Driver 必须长期使用某项信息，应将经过验证的必要属性复制到由 Core 管理的持久对象中，或者使用生命周期明确保证的可信 DT 来源。

## **12.3.3 Embedded Secure DTB**

OP-TEE 也可以直接把 DTB 编译到自身镜像中。

相关配置为：

```
CFG_DT=y
CFG_EMBED_DTB=y
CFG_EMBED_DTB_SOURCE_FILE=<relative-dts-path>
```

`CFG_EMBED_DTB_SOURCE_FILE` 指定 `core/arch/$(ARCH)/dts` 下的 DTS 源文件，构建系统将其编译为 DTB 并嵌入 Core 的只读区域。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



这种方案的优势是：

- 配置能够跟随 OP-TEE 镜像统一发布。
- 可以纳入 OP-TEE 镜像认证链。
- 不依赖 Normal World 提供安全设备资源描述。
- 便于固定版本的 SoC 平台管理。

但也有不足：

- 硬件变体需要对应的 DT 构建或选择策略。
- 修改配置往往需要重新构建 OP-TEE。
- DT 与 Firmware 配置不匹配时可能发生硬件初始化错误。
- 已认证的 DT 仍然可能存在设计错误。

注意：**被认证过的配置不等于配置一定正确。**

Secure Boot 证明的是被加载内容符合信任策略，而不是证明每个 `reg`、Clock 或 IRQ 配置都符合真实硬件设计。

## **12.3.4 DT Overlay 的意义**

OP-TEE 可能需要向后续 Linux Boot 阶段传递资源信息，例如：

- OP-TEE Firmware Node。
- Secure Memory Reservation。
- 通信接口描述。
- 平台特定的 Firmware 资源。

官方文档列出了追加 OP-TEE 节点、增加保留内存区域，以及创建 Overlay 的机制。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



以共享系统为例：

```
OP-TEE Secure DT
       |
       +-- Secure Hardware Configuration
       |
       +-- Secure Memory Configuration

OP-TEE-generated NS DT information
       |
       +-- /firmware/optee
       |
       +-- /reserved-memory
       |
       v
U-Boot / Linux DT
```

但有一点特别重要：

OP-TEE 向 Linux 提供的 Reserved Memory 描述，主要用于协调正常工作的软件不去使用这些区域。

它本身不能阻止一个已被攻陷的 Linux Kernel 通过物理访问或 DMA 尝试攻击安全内存。

因此，需要同时建立：

1. 正确的软件 Memory Reservation。
2. 独立有效的硬件 Memory Protection。

## **12.3.5&#x20;**`reserved-memory`**&#x20;不是硬件隔离**

举个例子：

```
reserved-memory {
    #address-cells = <2>;
    #size-cells = <2>;
    ranges;

    secure_ram: region@90000000 {
        reg = <0x0 0x90000000
               0x0 0x01000000>;
        no-map;
    };
};
```

这里 `no-map` 的主要作用是表达 OS 不应为该区域建立通常的线性映射。

它不能证明：

- Linux 绝对无法访问该物理地址。
- 任何 DMA Master 都无法访问该物理地址。
- 对应 CPU 特权级已经被硬件拒绝访问。
- SoC Firewall 已正确配置。

所以，**Device Tree Memory Reservation 是资源协作机制，不是 Hardware Security Boundary。**

# **12.4 Device Tree Driver Framework：从 DT Node 到 Driver Probe**

现在进入本章最重要的源码分析。

前面我们已经明确：Device Tree 负责描述硬件资源。

接下来的问题是：

**OP-TEE 如何根据这些描述，找到对应的驱动，并完成初始化？**

## **12.4.1 为什么需要 Driver Framework？**

先考虑一种最简单的实现：

```
void platform_init(void)
{
    init_clock();
    init_reset();
    init_uart();
    init_crypto();
    init_rng();
}
```

这种设计有一个优点：简单。

但随着平台复杂度增加，它会出现三个问题。

**第一，硬件配置和驱动逻辑耦合。**

每增加一种硬件变体，Platform Code 都可能需要修改。

**第二，初始化顺序难以维护。**

例如：

```
Crypto Engine
    |
    +-- requires Clock Controller
    |
    +-- requires Reset Controller
    |
    +-- requires Power Domain
    |
    +-- requires Secure Memory Mapping
```

如果 Crypto Driver 在 Clock Controller 初始化前执行，就可能失败。

**第三，资源依赖关系无法自然表达。**

例如：

- Clock A 被 Crypto 和 TRNG 同时使用。
- Reset Controller 为多个设备提供复位控制。
- 一个 Interrupt Controller 管理多个外设。
- 一个 Firewall Controller 管理多个 Memory Region。

这些设备之间实际上形成了依赖图，而不是简单的线性调用列表。

因此 OP-TEE 引入了通用的 DT Driver Framework。

## **12.4.2 核心数据结构：**`struct dt_driver`

源码：

`core/include/kernel/dt_driver.h`

OP-TEE 4.11 中，`struct dt_driver` 的核心字段包括：

```
struct dt_driver {
    const char *name;
    enum dt_driver_type type;
    const struct dt_device_match *match_table;
    const void *driver;
    TEE_Result (*probe)(const void *fdt,
                        int node,
                        const void *compat_data);
};
```

这一结构体在源码中确实存在，相关 Driver 注册使用 `DEFINE_DT_DRIVER()`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



逐个分析。

### `name`

驱动名称，主要用于识别和诊断。

它不是 Hardware Device Instance 的唯一标识。

一个驱动可以匹配多个设备实例。

### `type`

描述 Driver Framework 中的设备类别。

例如：

```
DT_DRIVER_UART
DT_DRIVER_CLK
DT_DRIVER_RSTCTRL
DT_DRIVER_INTERRUPT
DT_DRIVER_GPIO
DT_DRIVER_FIREWALL
```

这个类型非常重要，因为 Device Tree 中的某个 `compatible` 字符串可能涉及多个不同用途的驱动组件。

Driver Type 允许框架按照资源类别查找 Provider。

### `match_table`

负责将 DT 中的 `compatible` 字符串匹配到对应的驱动。

例如：

```
compatible = "vendor,soc-crypto-v2";
```

与：

```
static const struct dt_device_match crypto_matches[] = {
    { .compatible = "vendor,soc-crypto-v2" },
    { }
};
```

这里是一个教学示例，`vendor,soc-crypto-v2` 并非上游现有设备的真实 Binding。

### `driver`

可指向特定类型的 Driver Operations。

它不等于已经初始化完成的 Hardware Device Context。

例如，UART Console 采用一套 `serial_driver` 操作接口。

### `probe`

用于在发现匹配设备后初始化对应设备。

典型任务包括：

- 解析寄存器地址。
- 获取 Clock/Reset。
- 建立 MMIO Mapping。
- 初始化内部状态。
- 注册硬件服务。
- 返回成功或需要延迟初始化。

特别要注意：**不是每个&#x20;**`dt_driver`**&#x20;都直接使用&#x20;**`probe`**&#x20;Callback。**

我们后面将分析的 PL011 Console 就是一个例子。

## **12.4.3 Driver Registration 的实现**

OP-TEE 不是简单通过运行时调用 `register_driver()` 把所有驱动存入某个动态列表。

它使用了编译期的 Scattered Array 机制。

例如：

```
DEFINE_DT_DRIVER(example_driver) = {
    .name = "example",
    .type = DT_DRIVER_NOTYPE,
    .match_table = example_matches,
    .probe = example_probe,
};
```

宏 `DEFINE_DT_DRIVER()` 最终通过 `SCATTERED_ARRAY_DEFINE_PG_ITEM()` 将 Driver Descriptor 放入相应集合。

框架可以通过：

```
for_each_dt_driver(drv) {
    /* examine a registered driver */
}
```

遍历所有已经编译进 Core 的驱动描述符。

这里有一个很好的 OS 设计思想：

**静态注册 Driver Definition，动态初始化 Device Instance。**

它们并不矛盾。

```
Compile Time
    |
    v
Driver Descriptors
    |
    v
Linker / Scattered Array
    |
    v
OP-TEE Runtime
    |
    v
Match DT Nodes
    |
    v
Probe Device Instances
```

这种设计可以减少驱动注册过程中的动态管理开销，同时保留根据平台配置选择设备的能力。

## **12.4.4 真正的 Probe 流程**

关键文件：

`core/kernel/dt_driver.c`

主要流程可以简化为：

```
get_secure_dt()
       |
       v
parse_node()
       |
       v
dt_driver_maybe_add_probe_node()
       |
       v
Match compatible
       |
       v
dt_driver_probe_list
       |
       v
process_probe_list()
       |
       v
probe_driver_node()
       |
       +-- Success
       |      |
       |      v
       |   Ready List
       |
       +-- Defer
       |      |
       |      v
       |   Probe List
       |
       +-- Failure
              |
              v
           Failed List
```

这些阶段对应 4.11 的真实实现，包括 Probe、Ready、Failed 三类列表以及重复处理被延迟设备的流程。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



最重要的是 `probe_driver_node()` 的返回结果。

| Probe 返回                      | 框架行为                   |
| ----------------------------- | ---------------------- |
| `TEE_SUCCESS`                 | 将设备放入 Ready List       |
| `TEE_ERROR_DEFER_DRIVER_INIT` | 记录延迟次数，重新放回待 Probe 列表  |
| `TEE_ERROR_NODE_DISABLED`     | 不继续按普通失败设备处理           |
| 其他错误                          | 放入 Failed List，供后续失败处理 |

这里需要区分 `TEE_ERROR_DEFER_DRIVER_INIT` 与普通错误。

它表达的是：

**当前不满足初始化前置条件，但依赖的其他设备完成初始化后，当前设备可能成功。**

这不是简单的“初始化失败后随便重试”。

## **12.4.5 Deferred Probe 为什么重要？**

假设 Crypto Engine 需要 Clock Controller。

```
clock_ctrl: clock-controller@81000000 {
    compatible = "vendor,soc-clock";
    reg = <0x0 0x81000000 0x0 0x1000>;
    #clock-cells = <1>;
};

crypto@84000000 {
    compatible = "vendor,soc-crypto-v2";
    reg = <0x0 0x84000000 0x0 0x10000>;

    clocks = <&clock_ctrl 7>;

    status = "disabled";
    secure-status = "okay";
};
```

注意，这只是展示 Provider/Consumer 关系的示例，实际 Clock Binding 需要符合目标 SoC 规范。

假设 Crypto Driver 先被 Probe。

```
Crypto Probe
    |
    v
Get Clock
    |
    v
Clock Provider not ready
    |
    v
TEE_ERROR_DEFER_DRIVER_INIT
```

Crypto Driver 不应该立即把缺失的 Clock 当成永久性硬件故障。

相反，Framework 可以继续处理其他设备。

```
Clock Provider Probe
    |
    v
Register Clock Provider
    |
    v
Crypto Probe Again
    |
    v
Get Clock Successfully
    |
    v
Crypto Initialization
```

实际 `process_probe_list()` 会在初始化取得进展时继续处理待 Probe 节点；如果没有新的设备成功初始化，也没有新的节点加入，则停止重试并向上报告未解决依赖。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这解决了驱动初始化依赖顺序的问题。

但也引入一个值得深入思考的新问题：

**如果两个 Driver 之间形成循环依赖怎么办？**

例如：

```
Clock Driver
    |
    v
Power Domain Driver
    |
    v
Clock Driver
```

如果双方都无法先独立完成初始化，那么单纯依靠 Deferred Probe 并不能解决问题。

Staff Engineer 需要思考的不是“把哪个 Driver 的顺序调整一下”，而是：

- 是否存在错误的依赖建模？
- 是否需要拆分 Early Clock 与 Full Clock Provider？
- 是否必须由更早的 Firmware 提供 Bootstrap Resource？
- 是否可以将某个依赖从初始化期转移到运行期？
- 是否存在不必要的循环资源所有权？

这已经是 OS 架构设计问题，而不是简单的驱动问题。

## **12.4.6 Provider / Consumer 模型**

前面 Crypto Driver 依赖 Clock Controller 的关系，在 OP-TEE 中是典型的 Provider/Consumer 模型。

Provider 不仅仅是一个 DT Node，它还代表了一个已经初始化并可解析资源引用的服务提供方。

核心 API 包括：

```
dt_driver_register_provider(...)
dt_driver_device_from_node_idx_prop(...)
```

源码定义在 `core/include/kernel/dt_driver.h`，实现位于 `core/kernel/dt_driver.c`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



概念上：

```
Provider Driver
       |
       | Register Provider
       v
Provider Registry
       ^
       |
       | Resolve phandle
       |
Consumer Driver
       |
       | clocks = <&clock_ctrl 7>
       v
Clock Instance
```

从设计角度看，`phandle` 不应该被理解成一个“地址指针”。

它是 DT 中描述资源引用关系的标识。

Consumer 通过 Provider 接口把资源描述转换成 OP-TEE 内部的资源对象。

因此，一个优秀的驱动应该尽量依赖：

```
struct clk *
struct rstctrl *
struct interrupt *
```

等相应抽象，而不是直接操作另一个驱动的内部状态。

具体接口类型与实现会因子系统而不同。

## **12.4.7 初始化阶段与失败策略**

4.11 中 Driver Framework 相关初始化有如下注册：

```
early_init_late(probe_dt_drivers_early);
driver_init(probe_dt_drivers);
```

早期阶段尝试建立 Probe List 并处理部分依赖；之后 Driver Init 阶段继续完成尚未完成的 Probe。

在最终 Probe 阶段，如果仍存在无法完成的依赖或失败记录，相关实现会输出诊断信息并触发 `panic()`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这不是一个应该随意修改的细节。

例如，某位工程师觉得：

“一个无关紧要的设备 Probe 失败，为什么要让整个 OP-TEE 启动失败？直接忽略不就好了？”

这个想法从通用 Linux 驱动角度看可能合理，但在 TEE 内核中必须先明确：

- 这个设备是不是安全边界的必要组成部分？
- 它是否负责保护某块内存？
- 它是否负责提供安全随机数？
- 它是否负责控制 Secure Device Ownership？
- 它失败后是否会留下部分已经启用的硬件状态？
- 其他驱动是否会误以为该安全服务已经建立？

对于安全关键组件，**无法建立安全约束时直接停止启动，可能比继续运行更加正确。**

对于明确可选的功能，则可以通过可靠的能力声明和降级设计处理。

关键不是统一选择 Panic 或 Continue，而是建立正式的 Failure Policy。

# **12.5 MMIO、Memory Mapping 与资源所有权**

我们接下来分析 Device Tree 如何转化为实际的 MMIO Access。

## **12.5.1 DT 中的&#x20;**`reg`**&#x20;描述什么？**

假设：

```
crypto@84000000 {
    compatible = "vendor,soc-crypto-v2";

    reg = <0x0 0x84000000
           0x0 0x00010000>;

    status = "disabled";
    secure-status = "okay";
};
```

如果其父节点定义：

```
#address-cells = <2>;
#size-cells = <2>;
```

那么：

- Physical Base = `0x84000000`
- Size = `0x10000`，即 64 KiB。

注意几个概念。

**Physical Address Range：** 硬件地址译码窗口。

**Register Offset：** 某个寄存器相对于 Device Base 的偏移。

**Virtual Mapping：** OP-TEE Core 能够访问该物理区域的虚拟地址映射。

它们是三个不同的概念。

例如，64 KiB 的 MMIO 窗口并不意味着设备有 64 KiB 的有效寄存器。

这个窗口可能只包含几十个寄存器，剩余部分作为地址空间预留。

另外，如果设备位于带 `ranges` 地址转换的总线下面，最终物理地址还需要按照对应总线 Binding 进行转换；不能无条件把 `reg` 第一个地址单元当成 CPU Physical Address。

## **12.5.2&#x20;**`dt_map_dev()`**&#x20;的实际工作**

源码：

`core/kernel/dt.c`

函数：

```
int dt_map_dev(const void *fdt,
               int offs,
               vaddr_t *base,
               size_t *size,
               enum dt_map_dev_directive mapping);
```

OP-TEE 4.11 的核心处理可以分成四步。

**第一步：检查设备状态。**

```
fdt_get_status()
        |
        v
Device Disabled?
        |
        +-- Yes → Error
        |
        +-- No → Continue
```

**第二步：读取物理地址与映射大小。**

通过 `fdt_reg_info()` 获取设备物理地址区间。

**第三步：决定映射类别。**

`DT_MAP_AUTO` 会根据 Device Tree Status 决定使用 Secure 或 Non-Secure IO Mapping。

在当前实现中，仅 Secure 状态可用而 Non-Secure 状态不可用时，自动选择 `MEM_AREA_IO_SEC`；其他可用组合会走 `MEM_AREA_IO_NSEC`。也可以通过 `DT_MAP_SECURE` 或 `DT_MAP_NON_SECURE` 显式指定。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



**第四步：建立映射。**

核心调用：

```
core_mmu_add_mapping(mtype, pbase, sz);
```

随后把映射后的虚拟地址返回给驱动。

### **关键问题：**`MEM_AREA_IO_SEC`**&#x20;究竟意味着什么？**

它表示 Core MMU Framework 使用对应的内存类型和映射属性管理该区域。

但我们必须清楚区分：

- OP-TEE CPU 访问时使用何种映射。
- 其他 CPU 或 Non-Secure World 是否能够访问设备。
- 其他 DMA Master 是否能够访问设备。

后面两个问题不可能仅由 OP-TEE 自己的 Page Table 解决。

在 Arm TrustZone SoC 中，还需要相关安全互联和设备访问控制。

在 RISC-V SoC 中，则需要根据具体架构选择 PMP、IOMMU、SMMPT 或其他系统级隔离机制。

## **12.5.3 一个经典错误场景**

假设：

```
core_mmu_add_mapping(MEM_AREA_IO_SEC,
                     0x84000000,
                     0x10000);
```

返回成功。

工程师据此认为：

“Secure Crypto Engine 已经完全隔离。”

这个结论是不成立的。

真正需要验证的是：

| 验证项目                | 判据                    |
| ------------------- | --------------------- |
| CPU Mapping         | OP-TEE 能正确访问目标寄存器     |
| Hardware Isolation  | 不被授权的 CPU/Domain 无法访问 |
| DMA Isolation       | 未授权 DMA Master 无法访问   |
| Interrupt Ownership | 中断不会被错误路由到低可信执行环境     |
| Clock/Reset Control | 低可信组件不能未经授权破坏设备状态     |
| Address Validation  | MMIO 地址和长度符合可信硬件描述    |

其中 Clock/Reset 的安全性也不能忽略。

如果 Normal World 无法访问 Crypto Engine 的寄存器，但可以随意 Reset Crypto Engine，那么它仍然可能造成：

- 安全服务拒绝服务。
- 运行中状态破坏。
- 密钥操作中断。
- 错误处理流程进入未测试的异常状态。

因此，资源的安全控制不应该仅仅理解为“是否能访问数据寄存器”。

# **12.6 Interrupt、Clock、Reset 与硬件依赖管理**

这一节重点讨论：设备初始化所需要的资源如何组织，以及由谁负责管理它们的生命周期。

## **12.6.1 Clock Controller**

一个 Crypto Engine 可能需要：

```
clocks = <&clkc 7>;
```

这意味着设备依赖 Clock Provider 提供的某一路时钟。

一般情况下，驱动不能仅仅保存一个 Clock ID，然后在任意位置直接操作 Clock Controller Register。

更合理的设计是通过 Clock Framework 获得对应资源对象。

其生命周期可能包括：

```
Resolve Clock
      |
      v
Enable Clock
      |
      v
Access Hardware
      |
      v
Suspend / Resume
      |
      v
Disable or Retain Clock
```

为什么需要资源对象和引用管理？

考虑两个设备：

```
Crypto Engine ----+
                  |
                  v
               Clock A
                  ^
                  |
TRNG -------------+
```

如果 Crypto 完成一次运算后直接关闭 Clock A，而 TRNG 仍在使用该 Clock，就可能发生运行时故障。

因此，Clock 的使用者、使能状态、共享关系和关闭条件，都必须纳入统一的资源生命周期模型。

## **12.6.2 Reset Controller**

Reset 的问题更复杂。

假设两个设备共享一条 Reset Line：

```
Reset Line R0
      |
      +-- Crypto Engine
      |
      +-- Security Controller
```

Crypto Driver 遇到 Timeout 后想直接执行 Reset。

但如果 Reset R0 同时影响 Security Controller，可能导致系统安全隔离配置被清除或发生短暂的非预期状态。

这意味着一个驱动不能仅仅因为拥有某个 Reset Handle，就必然有权在任何时刻执行复位。

需要考虑：

- 该 Reset 是否独占。
- 复位影响哪些硬件模块。
- 是否需要全局同步。
- 是否允许在 Secure Operation 过程中复位。
- Reset 后安全配置是否需要重新建立。
- 是否有更高特权级 Firmware 负责仲裁。

**Reset 是状态破坏性操作，不应简单视为普通设备寄存器写入。**

## **12.6.3 Interrupt Controller**

对于一个设备：

```
crypto@84000000 {
    compatible = "vendor,soc-crypto-v2";
    reg = <0x0 0x84000000 0x0 0x10000>;

    interrupt-parent = <&intc>;
    interrupts = <42>;

    status = "disabled";
    secure-status = "okay";
};
```

`interrupts` 的 Cell 格式必须由对应的 Interrupt Controller Binding 决定。这里的 `<42>` 只是示例，并不代表 GIC、PLIC 或 APLIC 都采用这个格式。

初始化链路可以抽象为：

```
Crypto Driver Probe
         |
         v
Parse Interrupt Specifier
         |
         v
Resolve Interrupt Controller
         |
         v
Create / Register Interrupt Handler
         |
         v
Configure Routing and Enable
         |
         v
Wait for Hardware Completion
```

但对于安全内核来说，需要进一步回答：

**这个中断究竟应该投递到哪个 CPU、哪个特权级、哪个安全域？**

在 Arm 上，可能涉及 GIC Security Groups 和 Secure Interrupt Routing。

在 RISC-V 上，可能涉及 PLIC、APLIC、IMSIC、AIA 或平台特定的中断路由机制。

然而 Device Tree 仍然只是软件描述。

实际中断路由是否能够受到低可信软件的篡改，取决于 Interrupt Controller 的访问权限和配置保护。

## **12.6.4 Provider/Consumer 依赖图**

综合来看，一个安全设备的依赖可能是：

```
Crypto Driver
    |
    +-- Clock Provider
    |
    +-- Reset Provider
    |
    +-- Interrupt Provider
    |
    +-- Power Domain Provider
    |
    +-- Secure Memory / MMU
    |
    +-- Crypto Framework
```

其中部分依赖通过 DT Provider/Consumer 模型处理，另一些依赖需要由 Initcall、Core Framework 或 Platform Firmware 保证。

不应该把全部依赖都假设为 Device Tree Phandle。

例如：

- Crypto Core 自身初始化完成是软件服务依赖。
- MMU 就绪是执行环境依赖。
- Security Firewall 已经锁定是安全前置条件。
- TRNG 健康状态通过是运行时安全能力依赖。

这些不同类型的依赖，需要分别建立管理机制。

在 OP-TEE 4.11 中，DT Driver Framework 甚至提供了 `dt_driver_get_crypto()`，用于在 Crypto Core 尚未准备好时返回 Deferred Probe 状态。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这说明 Driver Framework 的依赖管理已经不局限于硬件设备之间。

# **12.7 源码实战：从 PL011 Device Tree 到真实 UART 寄存器访问**

下面使用 OP-TEE 4.11 中实际存在的 PL011 UART Driver 讲解。

这个例子很有价值，因为它展示了一种容易被忽略的情况：

**Driver 通过 DT Framework 注册，但不一定通过通用&#x20;**`probe`**&#x20;Callback 完成初始化。**

## **12.7.1 实际涉及的源码**

| 文件                                | 主要职责                            |
| --------------------------------- | ------------------------------- |
| `core/drivers/pl011.c`            | PL011 UART Driver 与 DT 注册       |
| `core/kernel/console.c`           | 解析 Console DT 并选择 Serial Driver |
| `core/kernel/dt.c`                | 解析 DT 属性、映射设备内存                 |
| `core/kernel/dt_driver.c`         | DT Driver 匹配机制                  |
| `core/include/kernel/dt_driver.h` | DT Driver Framework Interface   |

## **12.7.2 Device Tree 输入**

例如：

```
/ {
    compatible = "example,board";

    #address-cells = <2>;
    #size-cells = <2>;

    aliases {
        serial0 = &uart0;
    };

    chosen {
        stdout-path = "serial0";
    };

    uart0: serial@9000000 {
        compatible = "arm,pl011";
        reg = <0x0 0x09000000
               0x0 0x1000>;
        status = "okay";
    };
};
```

这里只展示 Console 路径需要理解的属性；实际平台可能还需要 Clock、Interrupt 和其他配置。

## **12.7.3 第一步：驱动注册**

在 `core/drivers/pl011.c` 中，可以看到：

```
static const struct dt_device_match pl011_match_table[] = {
    { .compatible = "arm,pl011" },
    { 0 }
};
```

还存在一个 PL011 Driver Descriptor：

```
DEFINE_DT_DRIVER(pl011_dt_driver) = {
    .name = "pl011",
    .type = DT_DRIVER_UART,
    .match_table = pl011_match_table,
    .driver = &pl011_driver,
};
```

注意它使用的是：

```
.driver = &pl011_driver
```

而不是通用 `.probe`。

这里的 `pl011_driver` 为 Console Framework 提供 Serial Driver Operations。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## **12.7.4 第二步：Console Framework 查找设备**

源码：

`core/kernel/console.c`

关键函数：

```
configure_console_from_dt()
```

主要流程如下：

```
configure_console_from_dt()
       |
       v
get_dt()
       |
       v
get_console_node_from_dt()
       |
       | Parse /chosen/stdout-path
       v
dt_find_compatible_driver()
       |
       | Match "arm,pl011"
       v
struct dt_driver
       |
       v
struct serial_driver
       |
       v
dev_alloc()
       |
       v
dev_init()
       |
       v
register_serial_console()
```

这与源代码中的 Console 初始化路径相对应。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里值得注意的是 `get_dt()`。

Console Configuration 可以根据平台策略使用不同的 DT 来源，它不一定走安全驱动集中 Probe 使用的 `get_secure_dt()`。

因此，**Console 初始化路径与普通 Secure Device Driver Probe 路径并不完全相同。**

这就是为什么我们不能仅仅掌握一个统一的 Driver Probe 模板，就认为理解了所有设备的初始化。

## **12.7.5 第三步：解析物理地址并建立 MMIO Mapping**

PL011 的 DT 初始化实现使用：

```
dt_map_dev(fdt, offs, &vbase, &size, DT_MAP_AUTO);
```

然后还会检查：

```
size != 0x1000
```

也就是说，当前 PL011 驱动对该 DT `reg` 窗口大小有明确的预期，并不会把任何大小的 MMIO Window 都视为正确配置。

接下来会进行 PL011 的初始化工作。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这揭示一个很重要的事实：

**DT 描述的资源范围，还必须满足驱动本身的硬件契约。**

不能因为 `fdt_reg_info()` 成功解析出地址，就认为配置已经正确。

## **12.7.6 第四步：访问寄存器**

假设驱动最终使用：

```
Physical Base = 0x09000000
Mapped VA     = 0xXXXXXXXX
```

后续寄存器访问的概念是：

```
/* 示意代码 */
io_write32(value, mapped_va + register_offset);
```

CPU 通过 MMU 将 VA 转换到对应的物理设备地址。

完整路径可以整理为：

```
DTS
 |
 | compatible / reg / stdout-path
 v
DTB
 |
 v
configure_console_from_dt()
 |
 v
dt_find_compatible_driver()
 |
 v
PL011 Serial Driver
 |
 v
dt_map_dev()
 |
 v
core_mmu_add_mapping()
 |
 v
PL011 Initialization
 |
 v
MMIO Register Access
```

这就是一次完整的：

**Device Tree Description → Driver Matching → Resource Mapping → Hardware Access。**

## **12.7.7 从这个案例能够得到什么结论？**

至少有四点：

1. Driver 注册不意味着硬件已经完成初始化。
2. 所有 DT Driver 不一定都使用统一 `.probe` Callback。
3. DT `reg` 的合法性必须由设备驱动进一步验证。
4. 设备是否真正安全，还需要分析映射属性和硬件访问权限，而不能只看 Driver 是否能够访问寄存器。

# **12.8 Platform Initialization 的失败处理与安全设计**

到这里，我们已经完成了从描述到硬件访问的主线。

现在进一步考虑：如果初始化中途失败，会发生什么？

## **12.8.1 初始化不是一个天然原子的操作**

假设 Crypto Driver 执行：

```
1. Enable Clock
2. Deassert Reset
3. Map MMIO
4. Configure Interrupt
5. Initialize Crypto Hardware
6. Register Crypto Backend
```

如果第五步失败，系统可能处于：

```
Clock Enabled
Reset Deasserted
MMIO Mapped
Interrupt Partially Configured
Crypto Backend Not Registered
```

这不是一个完全初始化的设备，也不是一个完全没有副作用的设备。

因此驱动初始化需要考虑 Partial Initialization。

## **12.8.2 正确的错误处理策略**

一般来说，应该在设计阶段就明确：

- 每项资源何时获得。
- 获得资源后由谁负责释放。
- 哪些操作可以回滚。
- 哪些操作具有不可逆副作用。
- 失败后硬件应该进入什么状态。
- 是否允许其他安全服务继续运行。

例如：

```
Probe
  |
  +-- Acquire Clock
  |
  +-- Acquire Reset
  |
  +-- Map MMIO
  |
  +-- Configure Hardware
  |
  +-- Register Service
  |
  v
Ready
```

失败时不能简单假设所有步骤都可以逆序撤销。

例如某个硬件安全锁定位，一旦写入就可能只有 Cold Reset 才能改变。

在这种情况下，初始化操作的顺序需要重新设计，尽量先完成可能失败的检查，再执行不可逆的安全配置。

## **12.8.3 为什么普通 DT 错误可能演变成安全问题？**

考虑以下几种错误。

| 错误                  | 可能造成的后果            |
| ------------------- | ------------------ |
| 错误的 `reg` 地址        | 访问错误设备或敏感寄存器       |
| 错误的 `reg` 大小        | 越界 MMIO 映射或设备初始化失败 |
| 错误的 `interrupts`    | 中断丢失、错误路由、服务中断     |
| 错误的 `clocks`        | 设备无法启动、运行中硬件状态异常   |
| 错误的 `resets`        | 意外复位其他安全组件         |
| 错误的 `secure-status` | 软件资源归属与实际硬件权限不一致   |
| 错误的 `memory-region` | 共享/安全内存冲突          |
| 错误的 `phandle`       | 依赖无法解析或错误资源绑定      |

这些错误中，有些仅影响设备可用性。

但如果影响到安全资源的归属、映射、密钥状态或权限配置，就可能演变成安全漏洞。

## **12.8.4 Fail Closed 是重要原则，但不是简单地全部 Panic**

例如 TRNG 未通过健康检查。

如果这个 TRNG 是系统唯一的安全随机数来源，那么继续提供密钥生成服务可能是不安全的。

此时应阻止相关安全能力使用。

但这不一定意味着整个 OS 必须停止，也不意味着允许使用低安全性的随机数源作为静默回退。

更合理的做法是建立显式的 Capability State：

```
TRNG Health Check
       |
       +-- PASS
       |     |
       |     v
       |   RNG Service Ready
       |
       +-- FAIL
             |
             v
        RNG Unavailable
             |
             v
      Dependent Services Fail
```

能否继续运行其他服务，需要结合实际依赖关系与安全策略决定。

**真正的 Fail Closed，是确保安全不变量不被破坏，而不是机械地让系统在所有错误上都 Panic。**

# **12.9 完整工程实验：增加一个安全硬件驱动**

为了将前面的理论落地，我建议做一个综合实验。

我们假设需要在 OP-TEE 中增加一个新 Secure Crypto Controller。

## **实验目标**

能够证明自己理解：

- Secure DT 的选择。
- DT Driver 的注册与匹配。
- Provider/Consumer 依赖。
- MMIO Mapping。
- Driver Initialization。
- 错误处理。
- Resource Ownership。
- Hardware Security Enforcement。

## **Step 1 — 建立源码分析基线**

```
git clone https://github.com/OP-TEE/optee_os.git
cd optee_os
git checkout 4.11.0
```

先阅读：

```
core/kernel/dt.c
core/kernel/dt_driver.c
core/kernel/console.c
core/drivers/pl011.c
core/include/kernel/dt.h
core/include/kernel/dt_driver.h
```

不建议一开始就修改大量代码。

先建立 Call Graph：

```
get_secure_dt
      |
      v
parse_node
      |
      v
dt_driver_maybe_add_probe_node
      |
      v
process_probe_list
      |
      v
probe_driver_node
```

然后找出每个函数的输入、输出以及错误返回语义。

## **Step 2 — 编写实验 DTS**

```
/ {
    #address-cells = <2>;
    #size-cells = <2>;

    crypto@84000000 {
        compatible = "example,secure-crypto";
        reg = <0x0 0x84000000
               0x0 0x10000>;

        status = "disabled";
        secure-status = "okay";
    };
};
```

这是独立的教学 DTS，不是可以直接在任意 SoC 上启用的真实硬件配置。

可使用 Device Tree Compiler 检查：

```
dtc -I dts -O dtb \
    -o experiment.dtb \
    experiment.dts

dtc -I dtb -O dts \
    -o experiment.dump.dts \
    experiment.dtb
```

然后逐项验证：

- `compatible` 是否正确。
- `reg` 是否正确。
- Secure Status 是否与预期一致。
- DTB 是否满足基本结构要求。

注意：`dtc` 不会替你证明硬件隔离正确；对于生产级 DT，还需要对应 Binding Schema 和平台安全验证。

## **Step 3 — 实现实验性 Driver Descriptor**

以下是简化示意：

```
static const struct dt_device_match test_matches[] = {
    { .compatible = "example,secure-crypto" },
    { }
};

static TEE_Result test_probe(const void *fdt,
                             int node,
                             const void *data)
{
    /* Validate resource description */

    /* Acquire dependencies */

    /* Map registers */

    /* Initialize hardware */

    /* Register service */

    return TEE_SUCCESS;
}

DEFINE_DT_DRIVER(test_driver) = {
    .name = "test-crypto",
    .type = DT_DRIVER_NOTYPE,
    .match_table = test_matches,
    .probe = test_probe,
};
```

这段代码仅用于展示 Driver Registration 架构。

它缺少真实资源校验、驱动状态、清理路径、配置开关和对应硬件实现，不能直接作为安全驱动投产。

## **Step 4 — 增加 Dependency**

进一步给设备增加：

```
clocks = <&clock_ctrl 7>;
```

验证：

- Clock Provider 未完成初始化时是否正确 Deferred Probe。
- Clock Provider 完成初始化后 Consumer 能否继续。
- Provider 永远不存在时是否能正确诊断。
- Provider 返回无效资源时是否能区分配置错误和暂时不可用。

## **Step 5 — 故障注入**

建议至少覆盖以下场景。

| 场景                | 期望验证           |
| ----------------- | -------------- |
| 删除 `compatible`   | 设备不能被错误驱动匹配    |
| `reg` 长度错误        | 拒绝不合法映射        |
| 缺失 Clock Provider | 正确处理依赖无法满足     |
| Provider 暂时未就绪    | Deferred Probe |
| MMIO Mapping 失败   | 释放已获得资源        |
| Hardware Timeout  | 不发布未初始化服务      |
| Reset 失败          | 不进入不确定设备状态     |
| Secure DT 缺失      | 不静默采用低可信配置     |
| Secure Status 错误  | 能检测所有权冲突       |
| 两个 Consumer 共享资源  | 不错误关闭仍在使用的资源   |

## **Step 6 — Hardware Security Verification**

如果有真实 SoC 或支持安全访问控制的仿真环境，还应验证：

```
Secure Access → Expected Success

Unauthorized Non-Secure Access
              |
              v
        Expected Rejection
```

这里不能只观察 Linux 是否加载对应 Driver。

必须测试硬件访问控制本身。

例如在具有相关保护能力的平台上，检查：

- Non-Secure CPU Access。
- Untrusted DMA Access。
- 不同 Hart/Domain 的访问权限。
- Reset 后安全控制状态。
- Suspend/Resume 后资源是否仍被正确保护。

同时，需要确认测试本身不会破坏关键硬件状态。

# **12.10 修改影响范围：Staff Engineer 应该怎样审查 Platform Patch？**

现在考虑一个真实 Patch：

> 将某个 Secure Driver 从静态 MMIO 地址初始化，改为通过 Device Tree 获取地址并初始化。

表面上只涉及几行代码。

但实际影响范围可能如下。

| 影响领域                | 需要回答的问题                  |
| ------------------- | ------------------------ |
| Build Configuration | 哪些平台启用 `CFG_DT`？         |
| Boot                | 目标 DT 在初始化阶段是否已经可用？      |
| Trust               | DT 是否来自可信源？              |
| Memory              | 新 MMIO Mapping 是否正确？     |
| Security            | 是否与硬件访问权限保持一致？           |
| Driver              | 是否会与原有静态初始化重复？           |
| Dependencies        | Clock/Reset/IRQ 是否已经准备好？ |
| SMP                 | 初始化是否只执行一次？              |
| Power Management    | Suspend/Resume 后是否正确恢复？  |
| Runtime             | 新资源对象的生命周期是否足够长？         |
| Compatibility       | 没有 DT 的平台是否仍能构建和启动？      |
| Testing             | 是否覆盖配置错误、异常和硬件失败？        |

这张表应该成为后续审查类似 Patch 的基本模板。

我还想强调一个相当容易被忽略的问题：

**从静态平台配置迁移到 DT 并不必然意味着设计更好。**

如果某个安全控制寄存器必须在 MMU、Heap、DT Framework 初始化之前配置，而它的物理地址在 SoC 中固定，那么继续使用受控的静态 Early Boot 配置，可能是合理的设计。

架构改进的判断标准不应该是“代码是否更加通用”，而应该是：

- 初始化依赖是否更清晰。
- 安全边界是否更明确。
- 驱动是否更易于复用。
- 错误路径是否可验证。
- 整体复杂度是否在可控范围内。

# **12.11 本章核心知识总结**

完成本章之后，建议将以下六条原则作为长期理解 OP-TEE Platform Architecture 的基础。

**原则一：Platform Description 不是 Security Enforcement。**

Device Tree 描述资源，真正的访问限制需要硬件和可信配置共同保证。

**原则二：Secure DT 的可信度取决于来源、完整性和生命周期。**

一个 DTB 位于安全内存，不自动意味着其内容可信。

**原则三：Driver Definition、Device Instance 和 Resource Provider 是不同的抽象。**

`dt_driver` 静态注册之后，仍然需要匹配、初始化、建立依赖和管理设备状态。

**原则四：Driver Probe 是资源生命周期的开始，而不是一组简单寄存器初始化操作。**

需要考虑部分初始化、回滚、并发、Runtime 和 Suspend/Resume。

**原则五：Deferred Probe 解决的是可被满足的依赖，而不是错误的架构依赖。**

循环依赖或永远不存在的 Provider 需要从设计层面解决。

**原则六：平台代码修改必须进行 Cross-Layer Impact Analysis。**

Platform、Boot、MMU、Driver、Firmware、Hardware Protection、Linux DT 之间的关系，都需要纳入影响范围分析。

# **12.12 Staff Engineer 级别面试题**

下面提供 12 道题目，并附参考分析。

这些题目不是记忆测试。

每道题的目的都是观察你是否能够：

- 建立准确的系统模型。
- 明确问题的安全边界。
- 识别隐含前提。
- 分析异常和并发行为。
- 在多个实现方案之间进行权衡。
- 给出可验证的工程方案。

建议先独立思考，再阅读参考分析。

## **Question 1 — 一个经过 Secure Boot 的 DTB 就一定可信吗？**

**场景：**

某 SoC 使用 Secure Boot 验证 OP-TEE Core 镜像。

OP-TEE 使用 Embedded DTB。

一位架构师认为：

“既然 OP-TEE 镜像已经通过签名校验，那么所有 Device Tree 配置都已经可信，不需要再做资源校验。”

你是否同意？如何设计完整的安全验证方案？

**参考分析**

不同意。

签名认证证明的是镜像来自被授权的签名实体，并且没有发生未授权修改。

它不证明 Device Tree 中描述的硬件资源必然正确。

即使 DTB 完全可信，也可能存在：

- 硬件版本与 DT 不匹配。
- `reg` 指向错误的寄存器窗口。
- MMIO Region 与其他资源重叠。
- Clock/Reset Phandle 配置错误。
- 实际硬件安全归属与 DT 描述不一致。

因此，需要至少三层验证：

**Authenticity：** DTB 来源与完整性符合信任策略。

**Semantic Validity：** 资源范围、Binding、版本和依赖关系正确。

**Hardware Enforcement：** 安全访问控制的实际状态与 DT 描述一致。

进一步还要考虑 DTB 认证后是否存在 TOCTOU 修改窗口。

这里真正的 Staff 级思考是：**认证数据和验证数据代表的安全事实，是两个不同层次的工作。**

## **Question 2 — 为什么&#x20;**`status = "disabled"`**&#x20;不能保护 Secure Device？**

**场景：**

某 Crypto Engine 的 DT 定义为：

```
status = "disabled";
secure-status = "okay";
```

Linux 不会加载其 Driver。

硬件团队认为设备已经由 Secure World 独占。

你如何证明这一结论？

**参考分析**

不能根据 Linux Driver 是否加载来证明硬件独占。

我会建立 Hardware Access Matrix：

| 主体                     | 预期访问结果 |
| ---------------------- | ------ |
| OP-TEE Secure CPU      | Allow  |
| Linux CPU              | Deny   |
| Untrusted DMA Master   | Deny   |
| 特权级不足的 Firmware Client | Deny   |

然后检查：

- Security Firewall / PMP / SMMPT 配置。
- 设备地址译码和访问控制。
- 中断路由的保护。
- Clock/Reset 配置的访问限制。
- Reset/Suspend 后策略是否仍然有效。

还应该实际进行 Negative Access Test，而不是只看寄存器配置值。

因为某些硬件权限可能在系统 Reset、低功耗恢复或错误的安全域切换后发生变化。

**核心考核点：** 区分 Software Visibility 和 Hardware Accessibility。

## **Question 3 — Driver A 和 Driver B 互相依赖，应该如何解决？**

**场景：**

Crypto Driver 依赖 Clock Provider。

Clock Provider 初始化又需要读取 OTP。

OTP Driver 则需要 Crypto Service 来解密初始化数据。

于是形成：

```
Crypto → Clock → OTP → Crypto
```

所有 Driver 都返回 Deferred Probe。

你如何解决？

**参考分析**

首先不应该盲目增加 Retry Count。

这里存在真正的 Dependency Cycle。

需要判断：

1. 哪些依赖是启动期的硬依赖。
2. 哪些依赖可以延迟到运行期。
3. 哪些能力可以拆分为 Bootstrap Capability。
4. 是否应由更早一级 Firmware 提供必要状态。

一种可能方案：

```
Early Clock Bootstrap
         |
         v
Minimal OTP Access
         |
         v
Clock Provider
         |
         v
Crypto Engine
         |
         v
Full OTP Service
```

也可能应把 OTP 的解密工作移到已认证的前一级固件，或者重新设计初始化依赖。

但不能为了打破循环而引入不可信临时密钥或绕过必要的安全检查。

**核心考核点：** Deferred Probe 是依赖解析机制，不是架构错误的补救工具。

## **Question 4 — Driver Probe 执行到一半失败，系统是否应该继续启动？**

**场景：**

Crypto Driver 初始化：

```
Enable Clock
Deassert Reset
Configure Secure Access
Map MMIO
Initialize Hardware
Register Backend
```

在 Initialize Hardware 阶段返回 Timeout。

此时访问权限已经配置，设备可能处于未知状态。

你如何定义处理策略？

**参考分析**

需要首先区分三类资源：

- 可回滚的软件资源，例如普通 Mapping、Heap Allocation。
- 可逆的硬件状态，例如某些 Clock Enable。
- 可能不可逆或影响安全边界的配置，例如 Security Lock Bit。

不能简单地统一调用 `free()` 和 `reset()`。

我会先定义 Device State Machine：

```
UNINITIALIZED
     |
     v
PREPARED
     |
     v
SECURITY_CONFIGURED
     |
     v
HARDWARE_READY
     |
     v
REGISTERED
```

对每个 Transition 明确：

- 哪些操作可能失败。
- 失败后允许停留在哪个状态。
- 是否允许再次 Probe。
- 是否需要平台级 Reset。
- 是否需要终止 Boot。

如果 Crypto Driver 是安全启动或安全存储的关键依赖，那么失败后继续运行可能不符合安全策略。

**核心考核点：** 初始化必须具有明确的状态机和安全失败语义。

## **Question 5 — 如何安全地迁移一个静态 MMIO Driver 到 DT Driver？**

**场景：**

一个 SoC Driver 原来使用：

```
#define CRYPTO_BASE 0x84000000
```

现在准备改为：

```
dt_map_dev(...)
```

Patch 很小，而且 QEMU 测试成功。

你会怎样 Review？

**参考分析**

我会从以下几个角度检查：

**输入信任：** DT 来源是否可信？

**资源校验：** 地址和长度是否在硬件允许的窗口中？是否需要处理总线地址转换？

**配置兼容：** 不支持 DT 的 Platform 是否仍然可用？

**初始化顺序：** Driver Probe 时 Clock、MMU、Heap 是否就绪？

**并发：** 是否可能与原来的静态初始化同时发生？

**安全：** MMU Mapping 类型是否正确，硬件访问控制是否一致？

**验证：** 在 QEMU 成功是否只是因为 QEMU 对硬件访问控制模拟不完整？

如果该驱动必须在 Early Boot 阶段使用，则还需要检查 DT Framework 是否能满足这一初始化时序。

最终应提供 Build Matrix、Runtime Test 和 Hardware Security Test，而不只是一个成功启动的日志。

**核心考核点：** 代码改动小，并不意味着影响范围小。

## **Question 6 — 如何防止不可信 DT 修改造成越权 MMIO？**

**场景：**

一个 RISC-V SoC 的 OP-TEE 使用外部 DTB。

攻击者能够修改其中一部分内容，但不能修改 OP-TEE Core。

攻击者将某个设备的 `reg` 指向系统安全控制器寄存器。

OP-TEE Driver 随后尝试映射这个区域。

你如何防止攻击？

**参考分析**

首先必须明确 DT 的可信边界。

如果这份 DT 可以被攻击者修改，那么不能直接将它作为安全资源授权依据。

可能采用：

- 使用经过认证的 Embedded DTB。
- 由可信 Firmware 提供不可被低可信实体修改的配置。
- 对关键 MMIO Region 使用可信资源白名单。
- 检查地址范围和设备类型是否与平台约定一致。
- 将硬件访问控制建立在 OP-TEE 解析 DT 之前。
- 避免未验证的 DT 信息直接驱动安全敏感的 Mapping 操作。

进一步需要检查 TOCTOU：

**即使第一次解析时 DTB 内容正确，之后攻击者是否仍能改变它？**

关键参数应该在可信验证后固化到受保护的状态中。

**核心考核点：** Trust Boundary、TOCTOU、Confused Deputy、Hardware Protection。

## **Question 7 — 为什么一个 DT Driver 初始化成功之后，仍然可能发生严重的安全故障？**

**场景：**

某 Crypto Engine 已经成功 Probe。

系统运行正常。

随后 Linux 执行 System Suspend，再恢复运行。

恢复之后出现偶发的 Secure DMA Fault。

你如何定位？

**参考分析**

我不会首先假定 Crypto Driver 本身存在普通内存错误。

因为故障发生在 Suspend/Resume 后，需要检查硬件状态是否得到正确恢复。

重点包括：

- MMU 映射是否仍有效。
- 设备 DMA Address 是否仍正确。
- IOMMU/SMMPT 等转换和权限状态是否保持。
- 相关 TLB 或 Device Translation Cache 是否需要同步。
- Clock 和 Power Domain 恢复顺序。
- Security Firewall 是否随硬件 Reset 丢失配置。
- 是否存在其他 CPU/Hart 在恢复过程中提前访问设备。
- 驱动是否错误地认为 Probe 成功就代表硬件状态永久有效。

正确方案需要建立 Suspend/Resume 的依赖顺序及状态恢复机制。

**核心考核点：** Hardware Resource Lifecycle 不止于 Driver Probe。

## **Question 8 — 如果两个安全设备共享 Clock/Reset，但属于不同的管理域，如何设计？**

**场景：**

TRNG 和 Crypto Engine 共享同一个 Clock Controller。

其中 Crypto Engine 由 OP-TEE 管理。

TRNG 由独立 HSM 固件管理。

两个执行环境都可能改变 Clock 状态。

你如何设计跨执行环境的资源管理？

**参考分析**

这已经超出了普通 Driver Framework 的能力边界。

因为 Provider/Consumer 引用管理通常只在单个 Kernel 或管理实例的内部有效。

两个独立的安全执行环境之间，需要更高层次的资源仲裁。

可选方案：

- 将 Clock 所有权集中到一个可信管理实体。
- 通过 RPMI、SCMI 或平台专用安全协议代理资源操作。
- 使用硬件支持的独立 Clock Gate。
- 建立明确的 Power/Clock Reference Protocol。
- 对跨域 Reset 定义安全和恢复协议。

不能简单地让双方分别维护本地 Reference Count。

因为两个局部 Reference Count 并不等于一个一致的全局资源状态。

**核心考核点：** 分布式资源所有权和跨固件一致性。

## **Question 9 — 如何设计可供多个安全域共享的设备？**

**场景：**

一块 Crypto Accelerator 要同时服务：

- OP-TEE User TA。
- UEFI Standalone MM。
- 一个独立的 Secure Partition。

三个客户端具有不同的权限和生命周期。

请设计 Hardware Resource Ownership Model。

**参考分析**

首先要分清两类硬件：

**支持多 Context 隔离的设备。**

可以通过硬件 Context ID、Queue、DMA Protection、Key Slot 等机制实现并发隔离。

**不支持多 Context 隔离的设备。**

需要一个统一可信的 Broker 或 Owner，对请求进行串行化、状态隔离和安全擦除。

无论哪种方案，都应该考虑：

- Client Identity。
- Key Ownership。
- DMA Buffer Ownership。
- Hardware Context Switch。
- Interrupt Attribution。
- Fairness 与 Denial of Service。
- Reset 和 Fault Recovery。
- Client Crash Cleanup。
- Secure Partition 生命周期。

特别需要防止上一个 Client 的敏感状态泄露给下一个 Client。

**核心考核点：** 多租户安全资源隔离，而不是普通 Driver 复用。

## **Question 10 — 如果要设计一个现代化的 OP-TEE Driver Framework，你会如何改进？**

**场景：**

假设你成为 OP-TEE Core Maintainer。

有人建议将 OP-TEE Driver Framework 设计得和 Linux Driver Model 一样完整，包括动态 Device Registration、Runtime PM、Device Hotplug、复杂的 Bus Model 和通用 Driver Binding。

你是否支持？

**参考分析**

不能仅凭 Linux Framework 更成熟就决定全面引入。

需要先明确 OP-TEE 的目标：

- TCB 应尽量可审计。
- 资源模型相对静态。
- 硬件种类和运行需求与通用 Linux 不同。
- Secure Kernel 的内存和执行环境可能更受限制。
- 新的动态机制会增加代码复杂度与攻击面。

我会以真实需求驱动设计：

**值得考虑的功能：**

- 更明确的 Device Resource Lifetime。
- 更好的 Dependency Diagnostics。
- 安全资源独占验证。
- 可靠的错误恢复。
- 测试与故障注入接口。

**需要谨慎的功能：**

- 复杂动态设备管理。
- 大规模通用 Runtime PM。
- 不必要的 Hotplug。
- 难以审计的通用事件机制。

**核心考核点：** 好的系统架构不是功能最多，而是在需求、安全、复杂度和维护成本之间取得合理平衡。

## **Question 11 — 如何为 Secure DT 设计抗回滚机制？**

**场景：**

某产品允许升级 OP-TEE 和 Secure DT。

攻击者不能伪造签名，但是能够将旧版本的、签名有效的 Secure DT 恢复到设备上。

旧版 DT 会使某些安全设备采用已淘汰的权限配置。

你如何设计防回滚方案？

**参考分析**

单独的签名验证不能防止有效旧版本的重放。

需要将安全配置纳入版本管理策略。

可能的方案：

- OP-TEE Image 与 Secure DT 使用兼容版本组合。
- Boot Manifest 包含配置版本和依赖关系。
- 通过受保护的 Monotonic Version 或 Anti-Rollback Counter 限制旧配置。
- 对重要安全配置使用版本绑定。
- 对 Firmware/DT/Driver 的升级建立原子兼容关系。

还要考虑升级失败后的恢复策略：

如果新 Firmware 更新成功，但 DT 更新失败，设备应该如何恢复？

如果只能允许更高版本，那么安全更新过程本身需要具备 Crash Consistency。

**核心考核点：** Authentication、Anti-Rollback 和 Firmware Update Atomicity 是三项不同的性质。

## **Question 12 — 如何证明一个 Platform Patch 没有扩大 OP-TEE 的攻击面？**

**场景：**

你提交一个 Patch，增加一种新的 DT Driver，并允许通过 DT 控制寄存器区域和中断配置。

Maintainer 问：

“这个 Patch 是否扩大了 OP-TEE 的攻击面？你有什么证据？”

请设计完整的评审材料。

**参考分析**

首先应承认：

**新增驱动通常意味着新增可执行代码和新的输入处理路径，因此不能未经分析就声称攻击面完全没有增加。**

我会提供五类证据。

**1. Trust Boundary Analysis**

说明 DT 来源、权限和可修改性，以及任何低可信输入如何进入 Driver。

**2. Resource Boundary Analysis**

明确 MMIO、IRQ、Clock、Reset、DMA 的权限和 Owner。

**3. Code Safety Analysis**

审查整数溢出、长度验证、内存错误、资源释放和错误处理。

**4. Concurrency & Lifecycle Analysis**

验证 SMP、Interrupt、Suspend/Resume、Error Recovery。

**5. Verification Evidence**

提供：

- Static Analysis。
- Build Matrix。
- Positive/Negative Tests。
- Fault Injection。
- Hardware Access Negative Tests。
- Regression Tests。

更重要的是，我会区分：

**已经通过测试证明的性质**，与**仅通过架构推理认为应该成立的性质**。

两者都重要，但不能混为一谈。

**核心考核点：** 能否将安全设计主张转化成可以检查、复现和评审的工程证据。

## **Staff Engineer 考核评分建议**

**自我考核评分表**

# **0 / 25**

需要继续强化分析

架构建模能力

0 / 5

能否识别所有关键组件及职责边界

安全推理能力

0 / 5

能否区分配置、权限和硬件强制执行

故障分析能力

0 / 5

是否覆盖部分失败、恢复和并发

设计取舍能力

0 / 5

是否解释方案的成本、复杂度及限制

验证能力

0 / 5

能否提出可复现的安全与功能证据

0 = 未掌握；5 = 可以独立设计并审查

重置评分

# **本章推荐源码阅读顺序**

为了便于后续自己在本地研究，我建议按照下面的顺序阅读，而不是从某个 DT Driver 开始随机跳转。

| 顺序 | 源码文件                                                                                                                             | 研究目标                           |
| -- | -------------------------------------------------------------------------------------------------------------------------------- | ------------------------------ |
| 1  | [kernel/dt.h](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/include/kernel/dt.h?utm_source=chatgpt.com)                    | DT 访问接口、来源与映射抽象                |
| 2  | [kernel/dt.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/dt.c?utm_source=chatgpt.com)                            | DT Parsing、Status、MMIO Mapping |
| 3  | [kernel/dt_driver.h](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/include/kernel/dt_driver.h?utm_source=chatgpt.com)      | Driver/Provider/Consumer 数据结构  |
| 4  | [kernel/dt_driver.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/dt_driver.c?utm_source=chatgpt.com)              | Match、Deferred Probe、依赖管理      |
| 5  | [kernel/console.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/console.c?utm_source=chatgpt.com)                  | DT 与 Console 初始化               |
| 6  | [drivers/pl011.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/drivers/pl011.c?utm_source=chatgpt.com)                    | 真实驱动注册与初始化                     |
| 7  | [plat-stm32mp1/main.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/plat-stm32mp1/main.c?utm_source=chatgpt.com) | 平台初始化与硬件资源管理                   |

另外可参考 [OP-TEE 4.11.0 官方 Device Tree 文档](https://optee.readthedocs.io/en/4.11.0/architecture/device_tree.html?utm_source=chatgpt.com)。

# **Chapter 12 结束语**

这一章涉及的源码数量并不算多，但其架构意义很重要。

如果只是把 OP-TEE 的 DT Driver 理解为：

```
Find compatible → Probe → MMIO
```

那么还只是掌握了一个驱动开发流程。

真正需要理解的是：

```
Trusted Configuration
        |
        v
Hardware Resource Description
        |
        v
Dependency Resolution
        |
        v
Resource Ownership
        |
        v
Hardware Security Enforcement
        |
        v
Safe Runtime Lifecycle
```

**OP-TEE 作为安全 OS 的一项重要职责，就是把可信的硬件资源描述转换成可管理、可隔离、可验证的内核资源。**

而 Platform Layer、Driver Framework 和 Device Tree 正处于软件资源抽象与 SoC 硬件安全边界的交汇处。

当你以后审查一个 OP-TEE Platform Patch 时，最重要的问题不应仅仅是“这个驱动能不能工作”，而应该是：

**它所依赖的配置可信不可信？它使用的硬件资源到底属于谁？它是否正确维护资源生命周期？它的异常行为会不会破坏其他安全组件的状态？以及，我们怎样证明这些结论？**

能够系统地回答这些问题，才意味着真正掌握了本章。
