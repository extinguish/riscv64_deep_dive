# RISC-V SMMPT 深度课程 · Part V

Chapter 17–19 · 系统集成、底层调试与安全验证

Part V 是整套 SMMPT 课程从架构研究转向实际 SoC Bring-up 的关键部分。

前面的章节主要解决了：

- SMMPT 为什么存在，以及它如何建立 Supervisor Domain 的物理隔离边界。
- MPT、MPTE、`mmpt`、SDID、权限检查和 Domain Switching 的实现原理。
- OpenSBI/RDSM 与 OP-TEE 如何配合，管理不同的安全域。

接下来我们需要解决一个更困难的问题：

一套理论上正确的 SMMPT 隔离方案，如何在真实的 Linux + UEFI + OpenSBI + OP-TEE 系统中成功启动、稳定运行，并且证明隔离确实有效？

这就是 Part V 的核心。

## 本部分的学习目标

| 章节         | 核心目标                            | 最终交付                        |
| ---------- | ------------------------------- | --------------------------- |
| Chapter 17 | 将 SMMPT 与完整 Firmware/OS 软件栈连接起来 | 系统集成设计、Boot Memory Contract |
| Chapter 18 | 从异常、CSR、MPT、Hart 和 DMA 状态定位故障   | Debug 方法与典型案例               |
| Chapter 19 | 建立可重复、可追踪、有安全意义的验证体系            | Test Matrix、测试框架、验收标准       |

全文围绕一套假设的 RV64 多 Hart 平台展开。示例物理地址、平台服务接口及部分代码是教学用途的参考设计，不代表 SMMPT 规范强制规定这些接口。

规范基线说明： 以 RISC-V Supervisor Domain Access Protection 规范 v0.9.0 为主要参考。`Smsd` 是 Supervisor Domain 相关的架构扩展，`Smmpt` 是其中的 Memory Protection Table 机制。当前的 SMMPT 软件生态仍在演进，因此不能默认主线 Linux、OpenSBI、OP-TEE、QEMU 已经提供了完整的 SMMPT 集成。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



# Chapter 17 — Linux、UEFI、Device Tree 与 SMMPT 系统集成

## 17.1 本章目标：理解三套相互独立的内存管理视图

想象以下场景：

SoC 有 16GB DDR，其中 512MB 分配给 OP-TEE，64MB 分配给 Root Firmware，其他内存交给 Linux 管理。同时，Linux 和 OP-TEE 之间还有一块共享内存。

RDSM 已经配置完成 MPT：

- Linux Domain 不能访问 OP-TEE Private Memory。
- OP-TEE 可以访问自己的内存。
- 两个 Domain 都可以访问指定的 Shared Memory。

从硬件安全角度看，已经完成隔离。

但问题是：Linux 是如何知道哪些 DDR 可以分配的？

如果 Linux 不知道某一段 DDR 属于 OP-TEE，就可能把它当作普通物理内存交给 Buddy Allocator。

即便 MPT 最终会拒绝 Linux 的访问，系统也可能在早期启动时发生 Access Fault。

这引出了本章最重要的架构思想：

> 硬件安全权限、Firmware 资源所有权、OS 内存资源描述，必须彼此一致，但它们并不是同一种东西。

我们可以将其分成三个层次：

| 层次                   | 负责者                           | 解决什么问题          |
| -------------------- | ----------------------------- | --------------- |
| Security Ownership   | RDSM / MPT                    | 谁有权实际访问这段物理内存   |
| Resource Description | Firmware / UEFI / FDT         | 哪些内存属于系统，哪些需要保留 |
| Software Allocation  | Linux / OP-TEE Memory Manager | OS 能把哪些内存分配给谁   |

接下来所有内容，都围绕这三个层次展开。

## 17.2 构建贯穿三章的 SoC Memory Map

我们设计一个用于教学的 RV64 SoC：

| 物理地址范围                    | 大小      | 所属资源                  |
| ------------------------- | ------- | --------------------- |
| `0x8000_0000–0x83FF_FFFF` | 64 MiB  | Root Firmware / RDSM  |
| `0x9000_0000–0xAFFF_FFFF` | 512 MiB | OP-TEE Private        |
| `0xB000_0000–0xB0FF_FFFF` | 16 MiB  | Linux / OP-TEE Shared |
| DDR 其他可用区域                | 剩余      | Linux General RAM     |

以上是一个假设地址布局，并未列出 Boot Image、MMIO、其他 Reserved Region 等全部资源。Linux 的可用 RAM 并非连续的一整段。

### 17.2.1 先理解硬件与软件看到的内存

同一段 DDR 的三个视图

Shared Memory 的 Linux 侧具体管理方式取决于采用静态保留池还是动态分配/授权模型。

这张图解释了为什么不能仅靠 Device Tree 实现安全隔离。

如果恶意 Linux 修改自己的 FDT 或页表：

- RDSM 的 MPT 权限不会因此自动变化。
- Linux 仍无法通过正常 S/U-mode 访存绕过 MPT。
- 但 Linux 自身可能因错误使用不可访问内存而崩溃。

反过来，如果 RDSM 把 OP-TEE 私有内存错误地授权给 Linux，即使 FDT 设置了 `no-map`，也无法防止获得内核执行权限的攻击者建立自己的映射并访问该区域。

所以安全不变量是：

\\[ \text{MemoryDescription} \neq \text{SecurityAuthorization} \\]

软件资源描述负责避免误用，硬件访问控制负责阻止越权。

## 17.3 Linux 如何感知 Supervisor Domain 的内存布局？

### 17.3.1 Device Tree 的 `/memory` 和 `/reserved-memory`

在非 UEFI 直接启动路径中，Linux 通常通过 FDT 获得内存信息。

假设平台 DTS 如下：

```
/ {
    #address-cells = <2>;
    #size-cells = <2>;

    memory@80000000 {
        device_type = "memory";
        reg = <0x0 0x80000000
               0x4 0x00000000>;
    };

    reserved-memory {
        #address-cells = <2>;
        #size-cells = <2>;
        ranges;

        root_fw@80000000 {
            reg = <0x0 0x80000000
                   0x0 0x04000000>;
            no-map;
        };

        optee_private@90000000 {
            reg = <0x0 0x90000000
                   0x0 0x20000000>;
            no-map;
        };

        tee_shm@b0000000 {
            reg = <0x0 0xb0000000
                   0x0 0x01000000>;
            no-map;
        };
    };
};
```

这是用于演示地址保留语义的 DTS 片段，不是新的 SMMPT 标准 DT Binding；节点命名与完整 Schema 还需要遵循具体平台规范。

特别注意 `no-map`：

它要求 OS 不要把这段内存纳入常规 System RAM 直接映射，也不得对其进行未经受控的推测访问。

但它本身不是硬件防护开关。

另外，`reusable` 的语义是允许 OS 在一定限制下使用和归还内存，不能把私有 Secure DDR 简单设置成 `reusable`。通用 DT Reserved Memory 规范也不允许将 `no-map` 与 `reusable` 同时指定。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



### 17.3.2 Linux 内核如何处理保留内存？

我们沿着 RISC-V Linux 的相关源码路径进行分析：

```
Linux Entry
    |
    v
setup_arch()
    |
    v
Device Tree Early Scan
    |
    v
setup_bootmem()
    |
    v
memblock 初始化及保留区域处理
    |
    v
setup_vm_final()
    |
    v
Buddy Allocator / Normal Memory Management
```

核心代码建议阅读：

- `arch/riscv/mm/init.c`
- `arch/riscv/kernel/setup.c`
- `drivers/of/fdt.c`
- `drivers/of/of_reserved_mem.c`
- `mm/memblock.c`

其中几个函数的边界非常值得掌握。

`setup_bootmem()` 参与建立早期的物理内存布局，而 `setup_vm_final()` 负责建立最终内核虚拟映射。Linux RISC-V Boot 文档明确说明，在最终线性映射建立前，内存发现、早期 Page Table 和 Fixmap 都有特殊约束。

[image](https://www.google.com/s2/favicons?domain=https://docs.kernel.org\&sz=32)

The Linux Kernel documentation+1



从概念上，可以将 `memblock` 理解为：

```
struct memblock {
    struct memblock_type memory;
    struct memblock_type reserved;
    /* ... */
};
```

例如：

```
memory:
    DDR 所有 Linux 可管理候选区域

reserved:
    Firmware
    OP-TEE
    Static SHM
    Kernel Image
    FDT
    其他 Reserved Region
```

这里不能简单地把 `memblock.memory - memblock.reserved` 当成最终所有可用页面的精确数学定义，因为 `nomap`、内存范围移除、架构过滤、CMA 和后续页分配器初始化存在不同处理路径。

但这个模型足以帮助理解：Linux 正常的页面分配路径不应获得属于 OP-TEE Private Memory 的 PFN。

### 17.3.3 推荐的实际调试方法

在 Linux 正常启动后，可以检查：

```
cat /proc/iomem

dmesg | grep -iE \
  'reserved|memory|memblock|efi|optee'

ls /sys/firmware/devicetree/base/reserved-memory/
```

如果开启相关 Kernel Debug 功能，还可以使用：

```
memblock=debug
```

观察早期 Memory Region 的加入与保留。

但不能仅凭 `/proc/iomem` 就证明 MPT 隔离正确。这只能证明 Linux 对资源的理解，不能证明硬件真正拒绝越权访问。

因此，Chapter 19 中会专门设计安全隔离负向测试。

## 17.4 UEFI 如何影响 SMMPT 的内存管理？

这是 Chapter 17 里非常重要的另一个知识点。

在 RISC-V 上，我们需要区分两种典型启动方式。

### 17.4.1 直接 DT Boot

```
ROM / Root Firmware
        |
        v
OpenSBI / RDSM
        |
        v
Bootloader
        |
        | Linux Image + DTB
        v
Linux
```

对常规 RISC-V Linux 启动入口，内核要求：

- `a0`：当前启动 Hart ID。
- `a1`：DTB 的物理地址。
- `satp = 0`：Linux 入口时 MMU 关闭。

这些是 Linux Boot Protocol 约束，不是说所有 Firmware 阶段都必须关闭 MMU。

[image](https://www.google.com/s2/favicons?domain=https://www.kernel.org\&sz=32)

The Linux Kernel documentation+1



### 17.4.2 经过 UEFI Boot

```
ROM / Root Firmware
        |
        v
OpenSBI / RDSM
        |
        v
UEFI / EDK II
        |
        | GetMemoryMap()
        |
        | ExitBootServices()
        v
Linux EFI Stub
        |
        v
Linux Kernel
```

UEFI 的引入改变了 Linux 获取内存资源信息的主要来源。

使用 UEFI 启动时，RISC-V Linux 以 EFI Memory Map 作为填充系统内存的依据。

UEFI 需要正确转换 FDT 中 `/reserved-memory` 的相关信息，以免安全保留区域被错误当成 General-purpose RAM。

[image](https://www.google.com/s2/favicons?domain=https://www.kernel.org\&sz=32)

The Linux Kernel documentation+1



这一点非常重要。

假如：

```
RDSM:
    0x90000000 - 0xAFFFFFFF
    belongs to OP-TEE

UEFI:
    same range reported as usable RAM

Linux:
    allocates pages from this range
```

会发生什么？

Linux 可能分配到安全内存，并尝试在其中放置页表、Kernel Data 或其他数据。

于是 CPU 发起 Store：

```
Linux Store
    |
    v
VA -> PA
    |
    v
MPT Check
    |
    v
Access Fault
```

从 Linux 看，这可能像是某个非常早期的 Memory Corruption 或无法解释的启动异常。

但真正根因是 Firmware Memory Map 与硬件安全权限不一致。

### 17.4.3 建立统一的 Boot Memory Contract

在工程上，我强烈建议让 RDSM 与 UEFI、FDT 生成流程共享一套受控的内存布局定义。

例如：

```
# Conceptual platform memory contract
regions:
  - name: root_fw
    base: 0x80000000
    size: 0x04000000
    owner: root
    linux: reserved_no_map

  - name: optee_private
    base: 0x90000000
    size: 0x20000000
    owner: optee
    linux: reserved_no_map

  - name: tee_shm
    base: 0xB0000000
    size: 0x01000000
    owner: shared
    linux: dedicated_pool
```

这里 YAML 是自定义构建输入，并非规范 ABI。

基于这份定义，构建流程可以自动生成或校验：

- RDSM 的初始内存授权策略。
- DTS 的 `reserved-memory`。
- UEFI Platform Memory Configuration。
- OP-TEE 的 Memory Layout Configuration。
- 共享内存池的大小与边界。
- 测试环境的 Expected Memory Map。

但动态授权仍然要由可信 RDSM 处理，不能由静态构建文件替代。

这种设计的收益是防止多个工程各自维护一组地址宏，随着版本演进逐渐不一致。

尤其是在预硅环境里，这类问题很容易表现为“换一个镜像就突然无法启动”。

## 17.5 Linux Memory Allocator 与 SMMPT 的职责边界

接下来考虑更复杂的情况：

Linux 通过 `kmalloc()` 分配一块内存，然后希望把它共享给 OP-TEE。

```
void *buf = kmalloc(size, GFP_KERNEL);
```

这里有三个非常容易混淆的问题。

第一：`kmalloc()` 返回的是 Linux Kernel Virtual Address。

第二：这个 Virtual Address 最终映射到的物理页面，最初属于 Linux Domain。

第三：这块内存能否被 OP-TEE 访问，取决于相关跨域共享协议与 MPT 授权，而不是 `kmalloc()` 本身。

合理的共享流程是：

```
Linux Allocates Memory
          |
          v
Validate Pages / Pin Pages
          |
          v
Request Shared Memory Authorization
          |
          v
RDSM Validates Ownership & Range
          |
          v
Update OP-TEE MPT Permissions
          |
          v
Synchronize MPT State
          |
          v
OP-TEE Accesses Memory
```

如果采用支持 Memory Parcel 的传输协议，流程会出现 Parcel 创建、映射授权和回收；并不一定暴露原始 MPT 操作给 Linux。

### 17.5.1 为什么不能直接共享任意 Linux 内存？

假设 Linux 将某段仍包含 Kernel Secret 的物理内存授权给另一个 Domain，就可能主动泄露数据。

而如果某个页面在共享期间被 Linux 释放，并重新用于其他任务，则可能发生：

```
Linux Page A
    |
    +-- Granted to OP-TEE
    |
    +-- Freed prematurely
    |
    +-- Reallocated as Page B
    |
    +-- OP-TEE still accesses it
```

这就是典型的共享内存生命周期错误。

因此，一套安全的内存共享机制必须管理：

| 对象              | 必须维护的状态        |
| --------------- | -------------- |
| Physical Page   | 当前所有者          |
| Grant           | 允许哪些 Domain 访问 |
| Pin / Reference | 是否允许回收         |
| Mapping         | 当前 VA→PA 映射    |
| MPT Permission  | 硬件实际授权         |
| DMA State       | 是否存在活动中的设备访问   |
| Revoke State    | 是否已完成同步和回收     |

特别注意：撤销 OP-TEE 的访问权限，并不等于 Linux 可以立刻安全重用该页面。

还必须处理 OP-TEE 正在执行的访问、并发共享引用、潜在 DMA、缓存与安全数据清理。

## 17.6 OP-TEE Driver、SMC ABI 与 RISC-V 调用接口

在 Arm TrustZone 架构中，一次典型调用为：

```
CA
 |
 v
TEE Client Driver
 |
 v
SMC Instruction
 |
 v
Secure Monitor
 |
 v
OP-TEE
```

但 RISC-V 并没有 Arm 的 `SMC` 指令。

因此，将 OP-TEE 放到独立 Supervisor Domain 后，需要定义如何请求 RDSM 执行 Domain Switch。

一个概念上的路径是：

```
Linux CA
   |
   v
Linux OP-TEE Driver
   |
   v
RISC-V Transport / SBI ecall
   |
   v
M-mode Firmware / RPMI TEE Framework
   |
   v
RDSM Domain Switch
   |
   v
OP-TEE Entry
   |
   v
Trusted Application
```

这是一条逻辑路径，不表示每一层必然对应一次额外 Trap 或上下文切换。

尤其需要注意当前真实软件的发展情况。

2026 年公开的 Linux RISC-V OP-TEE RFC 已经探索通过 RPMI TEE Service Group 和 SBI MPXY 作为通信基础。10 月 13 日的另一套 RFC v3 将通用 RPMI TEE Transport 与 OP-TEE Backend 解耦，并以 Memory Parcel、Service Discovery 和独立控制 ABI 组织通信；该 RFC 也明确指出配套的 OP-TEE OS 实现尚未完成。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

OpenWall+1



所以本课程不应把以下接口当作已经稳定的 Linux UAPI：

```
sbi_smmpt_grant_memory(...);
sbi_smmpt_switch_domain(...);
```

这些只能作为平台参考设计的函数名。

我们应当进一步区分：

- Control Plane： RDSM 管理 Domain 创建、授权、撤销、销毁。
- Transport Plane： Linux 和 OP-TEE 通过约定的通信协议交换请求。
- Data Plane： CPU 和 DMA 对实际物理内存进行访问。
- Enforcement Plane： MPT 与相关 I/O 保护硬件执行权限检查。

这四者应该分离，否则 Linux 驱动很容易和平台私有的安全实现深度耦合。

## 17.7 Device Tree 中是否应该描述 SMMPT？

答案不是简单的“需要”或“不需要”。

要区分哪些内容必须被 Linux 知道，哪些内容应该对 Linux 隐藏。

### Linux 需要知道的

通常包括：

- 可用物理内存。
- 预留内存。
- 共享内存池（如果使用固定池）。
- 相关设备和中断资源。
- 发现 OP-TEE 服务和通信接口所需的信息（依所选 ABI）。

### Linux 不应该直接管理的

通常包括：

- RDSM 内部的 MPT Page Table。
- 其他 Domain 的私有 Memory Ownership Database。
- 直接可写的 MPT Root PPN 控制。
- RDSM 的安全策略密钥与可信状态。

特别是不能仅仅为了方便，让普通 Linux S-mode Driver 去改写受保护的 MPT。

`mmpt` 是 M-mode CSR，Root MPT 管理应处在可信权限边界内。规范也指出，典型部署由 RDSM 独占管理 MPT 的写入。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## 17.8 多 Hart 启动、CPU Hotplug 与 Domain Ownership

假设系统有 16 个 Hart，Linux Domain 当前占用 12 个 Hart，OP-TEE 使用其中部分 Hart 的时间片，通过 Domain Switching 执行可信调用。

那么 Hart 的物理资源并不一定静态专属于单一 Domain。

需要分别管理：

- 当前 Hart 所运行的 Domain。
- Hart 对应的 `mmpt` / SDID 状态。
- 当前执行上下文和返回上下文。
- Linux 的 CPU Online/Offline 状态。
- Secure Interrupt 与 Pending Interrupt。
- Hart Warm Boot 和重新加入执行域时的初始化。

Linux 的 SBI HSM 操作负责其定义范围内的 Hart State 管理，但不自动保证平台私有的全部 SMMPT 状态都已经正确初始化。

因此建议建立一条强制启动不变量：

> Hart 被允许执行某个 Supervisor Domain 的任何 S/U-mode 指令之前，必须已经建立与该 Domain 一致的安全配置。

否则可能出现 Cold Boot 正常、CPU Hotplug 后偶发故障的情况。

## 17.9 Chapter 17 实战：设计跨工程的集成验收

推荐先建立下列检查表。

| 检查点                    | 验证目标                          |
| ---------------------- | ----------------------------- |
| Firmware Memory Map    | Root、TEE、Shared、Linux 区域无非法重叠 |
| DTS `/reserved-memory` | Linux 不误分配 Secure DDR         |
| UEFI Memory Map        | 保留区域未作为通用 RAM 暴露              |
| OpenSBI/RDSM           | 在 Linux 启动前完成初始授权             |
| Linux Boot             | 正常建立 Kernel Linear Mapping    |
| OP-TEE Boot            | Private Memory 正常映射和访问        |
| Shared Memory          | 双方只访问授权页面                     |
| SMP                    | 新上线 Hart 使用正确 Domain Context  |
| Negative Access        | Linux 对 TEE Private 的访问被拒绝    |
| Reboot / Kexec         | 不遗留可被错误复用的安全状态                |

这张表将成为 Chapter 19 系统验证的起点。

### Chapter 17 — Staff Engineer 测试题

建议先独立回答，再展开参考要点。

1\. 系统启动早期出现 Access Fault，但 RDSM 的 MPT 配置看起来正确。为什么仍可能是 UEFI 问题？

2\. 为什么在 DTS 中设置 no-map，不能替代 MPT？

3\. 一块 Linux 页面通过 kmalloc 分配后交给 OP-TEE 使用，是否立即满足共享条件？

4\. Linux 已停止使用某个共享页面，是否可以立即释放回 Buddy Allocator？

5\. 如果 OP-TEE Private Memory 被 UEFI 描述为可用 RAM，但 Linux 因为偶然没有分配到这段内存而启动成功，系统是否安全？

6\. 设计一种无需给 Linux 开放 MPT CSR 的共享内存机制。

7\. 为什么 OpenSBI sbi_domain 不应该被自动视为 SMMPT 的完整实现？

8\. CPU Hotplug 后，某个新启动 Hart 能读取 OP-TEE Private Memory，首先检查哪些环节？

9\. 为什么跨 Domain 的共享内存实现必须独立于 Linux 内存分配器？

10\. 如果设计一套可维护五年以上的 RISC-V OP-TEE 平台，你会如何管理 DTS、UEFI、RDSM、OP-TEE 中重复的物理内存配置？

显示 10 道题的参考答案

本章掌握标准： 能独立解释为什么 Linux Memory Map 正确不等于 SMMPT 安全有效，能设计一套一致的 Boot Memory Contract，并能把一次跨域共享内存请求追踪到资源所有权和硬件授权层面。

# Chapter 18 — SMMPT 的底层调试与故障定位

## 18.1 本章目标：把“Access Fault”变成可定位的具体问题

真实 SoC Bring-up 中，最让人头痛的日志往往只有几行：

```
S-mode Trap
scause = 0x0000000000000005
sepc   = 0x0000000080201234
stval  = 0x0000000090001000
```

很多工程师看到 `scause = 5`，第一反应会是：

“某个地址访问越界了。”

这只是现象，不是根因。

在引入 SMMPT 之后，至少要考虑：

- VA→PA 转换得到的地址是否正确。
- PMP/PMA 是否允许访问。
- 当前 Hart 是否使用正确的 Domain。
- `mmpt` 是否指向正确的 MPT。
- MPTE 是否有效、权限是否足够。
- MPT Page Walk 是否能够读取其自身的数据结构。
- 修改后的权限是否被 MPT Cache 正确观察到。
- 是否存在多 Hart 同步问题。

因此，本章的核心不是背诵 CSR，而是建立一套能缩小故障范围的诊断方法。

## 18.2 首先区分三种不同的 Fault

RISC-V 常见同步异常代码如下。

| Exception Code | 含义                       | 重点排查                          |
| -------------- | ------------------------ | ----------------------------- |
| 1              | Instruction Access Fault | Instruction Fetch、PMP、MPT、PMA |
| 5              | Load Access Fault        | Load 路径物理访问权限                 |
| 7              | Store/AMO Access Fault   | Store/AMO 路径物理访问权限            |
| 12             | Instruction Page Fault   | 虚拟地址转换及页表权限                   |
| 13             | Load Page Fault          | VA Translation / Page Table   |
| 15             | Store/AMO Page Fault     | 页表的写权限、Dirty 等机制              |

一定要理解：

MPT 权限失败报告的是对应原始访问类型的 Access Fault，而不是普通的 Page Fault。

例如：

```
Linux Load VA
      |
      v
Sv39 Translation
      |
      +-- PTE Invalid
      |       |
      |       v
      |    Load Page Fault
      |
      v
Physical Address
      |
      v
MPT Read Permission Check
      |
      +-- Permission Denied
              |
              v
         Load Access Fault
```

但 Access Fault 并不意味着故障一定来自 MPT。

PMP、PMA、总线或其他物理访问约束也可能导致 Access Fault。规范还明确规定，如果 MPT Walk 自身因 PMP/PMA 检查失败，或者遇到无效及保留编码的 MPTE，也会按原始访存类型报告 Access Fault。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



### 18.2.1 `scause`、`sepc`、`stval` 的正确解读

假设：

```
scause = 5
sepc   = 0x80201234
stval  = 0x90001000
```

我们可以确定：

1. 当前 Trap 报告的是 Load Access Fault。
2. `sepc` 指示发生异常的指令位置。
3. `stval` 在实现提供故障地址时，能够帮助定位相关地址。

但需要注意：

`stval` 不一定是已经完成转换的 Physical Address，其具体有效内容还取决于异常类型及实现。

对于一次虚拟内存访问，正确的调试手段是结合：

- Faulting Instruction
- 原始 VA
- `satp` 与页表
- 当前执行模式
- 经过翻译得到的 SPA/PA
- 相应 Domain 的 MPT

逐步还原访问过程，而不是直接把 `stval` 当成物理地址查表。

## 18.3 M-mode、S-mode Trap Handler 应该记录什么？

在实际系统中，我建议建立两层日志。

### 第一层：S-mode OS Fault Log

用于确认失败的是哪条指令，以及 OS 当前的地址转换上下文。

```
struct s_fault_snapshot {
    unsigned long hartid;
    unsigned long sepc;
    unsigned long scause;
    unsigned long stval;
    unsigned long sstatus;
    unsigned long satp;
};
```

这只是最小快照。实际还需要 GPR、任务上下文、内核栈以及有虚拟化时的 `hstatus`、`htval` 等信息。

### 第二层：M-mode RDSM Security Log

用于确认当前运行的安全域与 MPT 配置。

```
struct rdsm_fault_snapshot {
    uint64_t hartid;
    uint64_t domain_id;
    uint64_t mmpt;
    uint64_t mstatus;
    uint64_t mepc;
    uint64_t mcause;
    uint64_t mtval;
    uint64_t mpt_generation;
};
```

这里 `domain_id` 和 `mpt_generation` 是我们建议由 RDSM 软件维护的字段，并不是 RISC-V 标准 CSR。

需要强调：MPT Access Fault 不一定直接由 M-mode 接收。异常可能被委托给 S-mode。因此，RDSM 快照可以通过受控 Debug 接口、故障转储或专门测试插桩取得，而不是假设 `mcause` 永远保存最近的 S-mode Fault。

同样，不应该在普通 Linux S-mode 中直接执行：

```
csrr a0, mmpt
```

`mmpt` 是 M-mode CSR，这类访问在不具备足够权限时不能合法读取。需要通过 M-mode 的受控诊断机制获得。

### 18.3.1 一次理想的故障日志

在受保护的开发环境中，可以输出：

```
[FAULT] hart=3
[FAULT] domain=linux
[FAULT] pc=0x80201234
[FAULT] cause=load-access-fault
[FAULT] va=0xFFFFFFC090001000
[FAULT] translated_pa=0x90001000

[RDSM] mmpt.mode=Smmpt43
[RDSM] mmpt.root=0x88000000
[RDSM] sdid=1
[RDSM] mpt_generation=27

[MPT] leaf_pa=0x88009000
[MPT] slot=1
[MPT] permission=---
[MPT] request=R
[MPT] result=DENIED
```

这里所有值均为模拟日志。实际 MPT Table Walk 还需要按每一级表项的 PPN 计算，不能只凭这个 `leaf_pa` 作为规范示例。

正式生产版本还应对敏感地址、Domain 元数据和安全日志进行访问控制与必要的脱敏。

## 18.4 手动分析一次 Smmpt43 MPT Walk

这一部分非常值得真正掌握。

对 RV64 的 Smmpt43，物理地址组织为：

```
 42            34 33        25 24        16 15       0
+----------------+------------+------------+----------+
|     pn[2]      |   pn[1]    |   pn[0]    |  offset  |
|     9 bits     |   9 bits   |   9 bits   |  16 bits |
+----------------+------------+------------+----------+
```

每个 Table Entry 为 8 Byte。普通非 NAPOT Leaf 中包含 16 组 `XWR` 权限，分别对应 16 个 4KB 页面。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



假设 Faulting Physical Address 是：

```
PA = 0x90001000
```

我们计算：

```
pa = 0x90001000pn2 = (pa >> 34) & 0x1ffpn1 = (pa >> 25) & 0x1ffpn0 = (pa >> 16) & 0x1ffslot = (pa >> 12) & 0xfprint(hex(pn2), hex(pn1),      hex(pn0), slot)
```

结果为：

```
pn2 = 0x0
pn1 = 0x48
pn0 = 0x0
slot = 1
```

所以当三个层级都采用 Non-leaf → Non-leaf → Leaf 的常规结构时：

```
mmpt.PPN
    |
    v
Root MPT Base
    |
    +-- Entry[0x0]
             |
             v
         Next MPT Base
             |
             +-- Entry[0x48]
                       |
                       v
                  Leaf Table Base
                       |
                       +-- Entry[0x0]
                                |
                                v
                           XWR[1]
```

最后检查 `XWR[1]` 是否包含此次访问需要的 Read 权限。

如果该位置的权限是：

```
XWR[1] = 000
```

那就应该拒绝 Load。

这里还必须分清：我们为了教学选择了最底层 Leaf；Smmpt43 允许更高层出现 Leaf，并支持更大粒度的权限区域，所以实际调试时要首先检查 `L`、`N` 等字段，而不能永远假定一定进行三级查询。

## 18.5 典型故障一：MPT Root Address 配置错误

假设在 RDSM 中执行了：

```
csrw mmpt, t0
```

随后 OS 一访问内存就发生 Access Fault。

第一件事不是修改 Linux Page Table，而是检查：

```
mmpt.MODE
mmpt.PPN
MPT Table Alignment
MPT Table Physical Accessibility
MPTE Valid Bits
```

常见错误包括：

错误 1：把 Virtual Address 当成 Physical Address。

`mmpt.PPN` 代表物理页号，不是完整物理地址，更不是 M-mode 代码中的虚拟地址。

错误 2：表地址未满足对齐。

例如 Smmpt43 Root Table 需要满足相应页面对齐要求；其他模式可能还有特殊要求，例如 Smmpt64 对 Root Table 的 32KiB 对齐限制。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



错误 3：MPT 内存没有初始化为有效结构。

如果 Root Entry 中包含随机数据，保留位可能被置位，从而导致 Access Fault。

错误 4：MPT Walk 读取表本身失败。

即使 Root PPN 正确，如果访问 MPT Table 时违反 PMP/PMA，仍然可能失败。

所以本章一定要建立一种意识：

> MPT 自己也是存储在物理内存里的数据结构。硬件必须能安全地读取这些数据结构，才能保护其他内存。

## 18.6 典型故障二：OP-TEE 可以访问 Shared Memory，Linux 却访问失败

假设：

```
Shared PA = 0xB0001000
```

OP-TEE 可以正常访问，但 Linux 报告 Load Access Fault。

定位流程：

1. 检查 Linux 得到的物理地址是否确实为 `0xB0001000`。
2. 检查该物理页面是否属于约定的共享区域。
3. 读取 Linux Hart 当前的 `mmpt`，确认其 Domain。
4. 检查 Linux Domain 对该地址的 MPTE Read 权限。
5. 检查对应 MPT 更新是否已同步。
6. 如果权限正确，再检查 PMP/PMA、映射属性及总线访问状态。

一个常见的根因是只修改了 OP-TEE Domain 的 MPT，却忘记了 Linux Domain 也需要被授权访问。

SMMPT 不使用一个全局通用的 Page Permission 表来替代所有 Domain；各个 Domain 的权限视图需要分别满足预期策略。

## 18.7 典型故障三：修改 MPT 后偶发访问失败

这个场景最值得从架构层面理解。

假设：

```
Hart 0:
    Update MPTE
    Allow Linux Read

Hart 1:
    Linux accesses the page
```

虽然内存里的 MPTE 已经修改，但 Hart 1 仍然可能使用旧的 MPT 缓存状态。

关键在于：

修改内存中的 MPTE，与让每个相关 Hart 按新权限执行访问，不是一回事。

### 18.7.1 MFENCE.PA 的含义

规范的 `MFENCE.PA` 用于同步 MPT 更新与 Supervisor Domain 的执行。

它是 M-mode 指令，可以根据物理地址和 SDID 对同步范围进行限定。更重要的是，其保证是针对执行该指令的 Hart；跨 Hart 场景需要软件额外协调相关 Hart。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



可以把正确的软件流程抽象为：

```
Hart 0 / RDSM
    |
    +-- Update MPTE
    |
    +-- Publish the update
    |
    +-- Local MPT Synchronization
    |
    +-- Request Remote Synchronization
                    |
                    v
               Hart 1
                    |
                    +-- Enter M-mode
                    |
                    +-- Perform MPT Synchronization
                    |
                    +-- Acknowledge
    |
    v
RDSM observes required acknowledgments
    |
    v
Permission Transition Complete
```

这是一个高层软件协议示例，而不是单条 `MFENCE.PA` 自动完成的全部操作。

### 18.7.2 为什么 `SFENCE.VMA` 不够？

因为：

```
SFENCE.VMA
    -> Virtual Memory Translation State

MFENCE.PA
    -> Memory Protection Table State
```

两者维护的缓存与权限状态不同。

另外，`fence rw,rw` 解决的是相应内存访问排序问题，它也不能直接替代 MPT 缓存同步。

如果访问涉及 DMA，还需要按照 I/O MPT Checker、IOMMU 和设备的相关协议执行同步。

### 18.7.3 一个真正危险的竞态

考虑撤销 Linux 对某页面的 Read 权限：

```
T0: Linux Read Allowed
T1: RDSM writes MPTE = No Access
T2: Hart 1 still has cached old permission
T3: Linux on Hart 1 reads page
T4: Hart 1 processes MPT synchronization
```

如果系统在 T2 就宣布权限撤销完成，那么 T3 可能造成安全违规。

因此撤销操作需要明确的完成语义：不能在相关执行者仍可能依据旧权限访问时，把页面交给新的安全所有者。

这也是为什么安全系统设计里，撤销权限比授予权限更容易出错。

## 18.8 典型故障四：Cold Boot 正常，Warm Boot 异常

假设：

```
Cold Boot:
    hart0 -> Linux
    hart1 -> OP-TEE
    OK

Warm Boot:
    hart1 restarted
    OP-TEE accesses private page
    Load Access Fault
```

应重点检查 Hart 的 Per-Hart 状态。

| 检查对象         | 可能的问题                |
| ------------ | -------------------- |
| `mmpt.MODE`  | Warm Boot 后未重新启用 MPT |
| `mmpt.PPN`   | 指向旧表、错误表             |
| SDID         | 与当前运行 Domain 不匹配     |
| `satp`       | 恢复了错误 Page Table     |
| PMP          | Warm Boot 初始化缺失      |
| Trap Context | 恢复了错误 S-mode 上下文     |
| MPT Cache    | 旧权限状态未按规范同步          |

尤其不要假定所有 CPU Reset、Hart Stop/Start 或低功耗恢复过程具有相同的 CSR 保留语义。必须核对目标硬件的 Reset Domain 和平台 Firmware 协议。

## 18.9 DMA 越权访问：为什么没有 `scause`？

这是非常常见的认知误区。

假设 DMA Engine 试图把数据写入 OP-TEE Private Memory。

即使 I/O MPT Checker 阻止了访问，也不意味着 Linux 当前 Hart 会发生 Store Access Fault。

原因是：

```
CPU Load / Store
    -> Hart Exception Path

DMA Transaction
    -> IOMMU / I/O MPT / Interconnect Error Path
```

DMA 不是由某个 Linux CPU 指令直接执行的普通 Load/Store。

所以实际定位要检查：

- Device Requester Identity。
- DMA IOVA / PA。
- IOMMU Translation。
- I/O Domain Classification。
- I/O MPT Check。
- I/O MPT Cache Invalidation Completion。
- 设备错误状态与相关中断。
- DMA 是否存在 In-flight Transaction。

I/O MPT 规范定义了基于请求身份的 Domain Classification、权限检查，以及 MPT 更新后的 I/O Cache 失效机制。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



如果 DMA 失败却没有 Linux CPU Trap，反而可能是符合架构预期的。

## 18.10 GDB、TRACE32、QEMU、Palladium 如何分工？

我建议按问题所在的层次选择调试工具。

| 工具                   | 适合定位的问题                        | 主要限制                      |
| -------------------- | ------------------------------ | ------------------------- |
| QEMU + GDB           | Trap、CSR、控制流、软件数据结构            | 目标 QEMU 必须确实支持所需 SMMPT 扩展 |
| TRACE32              | 真实 Hart 的指令流、CSR、SMP 调试        | 取决于芯片 Debug 支持和权限配置       |
| Palladium / ZeBu     | Firmware Boot、RTL 关联、SoC 访存事务  | 可观测性取决于仿真配置、Probe 和模型     |
| SoC Trace / Waveform | MPT Check、总线请求、Hart/Device ID  | 往往需要硬件团队提供信号定义            |
| Firmware Trace       | Domain Switch、MPT Update、Fence | 必须预先设计可信且安全的日志机制          |

### 18.10.1 GDB 能解决什么？

例如在支持相应 CSR 的目标中，通过 GDB 查看：

```
info registers
x/12i $pc
x/16gx 0x88000000
```

你可以检查：

- 当前 PC。
- 执行到哪条指令。
- 当前栈与通用寄存器。
- MPT 数据结构的物理内存内容（前提是调试访问路径支持）。
- 当前正在运行的 Hart。

但要注意：GDB 查看某个地址时，不一定天然通过被调试 Hart 的普通 S-mode 访存路径。因此 GDB 能读到 Secure Memory，并不意味着 Linux 一定能读到。

此外，调试访问必须经过授权，不能把生产安全策略中禁用的接口作为隔离正确性的证据。

### 18.10.2 Palladium 上应该怎样定位？

假设 Linux 在第一个 OP-TEE 调用时发生 Access Fault。

我建议同时获取三个层次的证据：

软件层：

```
Linux Fault PC
Linux scause/stval
RDSM Domain Switch Log
OP-TEE Entry Log
```

CPU/CSR 层：

```
Hart ID
Privilege Mode
mepc / mstatus
satp
mmpt.MODE / PPN / SDID
```

硬件事务层：

```
Effective PA
MPT Walk Request
MPTE Data
Permission Check Result
Fault / Abort Response
```

如果这些信息能够通过统一的 Timestamp 或 Transaction ID 关联起来，那么排查效率会显著提高。

相比仅仅反复修改 Boot Image，这种分层取证能够快速区分软件配置错误和 RTL 行为错误。

## 18.11 推荐的标准化故障定位流程

\#chatgpt-mermaid-\_r_842\_{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:14px;fill:rgb(255, 255, 255);}@keyframes edge-animation-frame{from{stroke-dashoffset:0;}}@keyframes dash{to{stroke-dashoffset:0;}}#chatgpt-mermaid-\_r_842\_ .edge-animation-slow{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 50s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_842\_ .edge-animation-fast{stroke-dasharray:9,5!important;stroke-dashoffset:900;animation:dash 20s linear infinite;stroke-linecap:round;}#chatgpt-mermaid-\_r_842\_ .error-icon{fill:rgba(255, 255, 255, 0.03);}#chatgpt-mermaid-\_r_842\_ .error-text{fill:rgb(255, 255, 255);stroke:rgb(255, 255, 255);}#chatgpt-mermaid-\_r_842\_ .edge-thickness-normal{stroke-width:1px;}#chatgpt-mermaid-\_r_842\_ .edge-thickness-thick{stroke-width:3.5px;}#chatgpt-mermaid-\_r_842\_ .edge-pattern-solid{stroke-dasharray:0;}#chatgpt-mermaid-\_r_842\_ .edge-thickness-invisible{stroke-width:0;fill:none;}#chatgpt-mermaid-\_r_842\_ .edge-pattern-dashed{stroke-dasharray:3;}#chatgpt-mermaid-\_r_842\_ .edge-pattern-dotted{stroke-dasharray:2;}#chatgpt-mermaid-\_r_842\_ .marker{fill:rgba(255, 255, 255, 0.498);stroke:rgba(255, 255, 255, 0.498);}#chatgpt-mermaid-\_r_842\_ .marker.cross{stroke:rgba(255, 255, 255, 0.498);}#chatgpt-mermaid-\_r_842\_ svg{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:14px;}#chatgpt-mermaid-\_r_842\_ p{margin:0;}#chatgpt-mermaid-\_r_842\_ .label{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;color:rgb(255, 255, 255);}#chatgpt-mermaid-\_r_842\_ .cluster-label text{fill:rgb(255, 255, 255);}#chatgpt-mermaid-\_r_842\_ .cluster-label span{color:rgb(255, 255, 255);}#chatgpt-mermaid-\_r_842\_ .cluster-label span p{background-color:transparent;}#chatgpt-mermaid-\_r_842\_ .label text,#chatgpt-mermaid-\_r_842\_ span{fill:rgb(255, 255, 255);color:rgb(255, 255, 255);}#chatgpt-mermaid-\_r_842\_ .node rect,#chatgpt-mermaid-\_r_842\_ .node circle,#chatgpt-mermaid-\_r_842\_ .node ellipse,#chatgpt-mermaid-\_r_842\_ .node polygon,#chatgpt-mermaid-\_r_842\_ .node path{fill:rgb(26, 40, 61);stroke:rgb(31, 78, 148);stroke-width:1px;}#chatgpt-mermaid-\_r_842\_ .rough-node .label text,#chatgpt-mermaid-\_r_842\_ .node .label text,#chatgpt-mermaid-\_r_842\_ .image-shape .label,#chatgpt-mermaid-\_r_842\_ .icon-shape .label{text-anchor:middle;}#chatgpt-mermaid-\_r_842\_ .node .katex path{fill:#000;stroke:#000;stroke-width:1px;}#chatgpt-mermaid-\_r_842\_ .rough-node .label,#chatgpt-mermaid-\_r_842\_ .node .label,#chatgpt-mermaid-\_r_842\_ .image-shape .label,#chatgpt-mermaid-\_r_842\_ .icon-shape .label{text-align:center;}#chatgpt-mermaid-\_r_842\_ .node.clickable{cursor:pointer;}#chatgpt-mermaid-\_r_842\_ .root .anchor path{fill:rgba(255, 255, 255, 0.498)!important;stroke-width:0;stroke:rgba(255, 255, 255, 0.498);}#chatgpt-mermaid-\_r_842\_ .arrowheadPath{fill:rgba(255, 255, 255, 0.498);}#chatgpt-mermaid-\_r_842\_ .edgePath .path{stroke:rgba(255, 255, 255, 0.498);stroke-width:1px;}#chatgpt-mermaid-\_r_842\_ .flowchart-link{stroke:rgba(255, 255, 255, 0.498);fill:none;}#chatgpt-mermaid-\_r_842\_ .edgeLabel{background-color:rgb(24, 24, 24);text-align:center;}#chatgpt-mermaid-\_r_842\_ .edgeLabel p{background-color:rgb(24, 24, 24);}#chatgpt-mermaid-\_r_842\_ .edgeLabel rect{opacity:0.5;background-color:rgb(24, 24, 24);fill:rgb(24, 24, 24);}#chatgpt-mermaid-\_r_842\_ .labelBkg{background-color:rgba(24, 24, 24, 0.5);}#chatgpt-mermaid-\_r_842\_ .cluster rect{fill:rgba(255, 255, 255, 0.03);stroke:rgba(255, 255, 255, 0.082);stroke-width:1px;}#chatgpt-mermaid-\_r_842\_ .cluster text{fill:rgb(255, 255, 255);}#chatgpt-mermaid-\_r_842\_ .cluster span{color:rgb(255, 255, 255);}#chatgpt-mermaid-\_r_842\_ div.mermaidTooltip{position:absolute;text-align:center;max-width:200px;padding:2px;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:12px;background:rgba(255, 255, 255, 0.03);border:1px solid rgba(255, 255, 255, 0.082);border-radius:2px;pointer-events:none;z-index:100;}#chatgpt-mermaid-\_r_842\_ .flowchartTitleText{text-anchor:middle;font-size:18px;fill:rgb(255, 255, 255);}#chatgpt-mermaid-\_r_842\_ rect.text{fill:none;stroke-width:0;}#chatgpt-mermaid-\_r_842\_ .icon-shape,#chatgpt-mermaid-\_r_842\_ .image-shape{background-color:rgb(24, 24, 24);text-align:center;}#chatgpt-mermaid-\_r_842\_ .icon-shape p,#chatgpt-mermaid-\_r_842\_ .image-shape p{background-color:rgb(24, 24, 24);padding:2px;}#chatgpt-mermaid-\_r_842\_ .icon-shape .label rect,#chatgpt-mermaid-\_r_842\_ .image-shape .label rect{opacity:0.5;background-color:rgb(24, 24, 24);fill:rgb(24, 24, 24);}#chatgpt-mermaid-\_r_842\_ .label-icon{display:inline-block;height:1em;overflow:visible;vertical-align:-0.125em;}#chatgpt-mermaid-\_r_842\_ .node .label-icon path{fill:currentColor;stroke:revert;stroke-width:revert;}#chatgpt-mermaid-\_r_842\_ .node .neo-node{stroke:rgb(31, 78, 148);}#chatgpt-mermaid-\_r_842\_ [data-look="neo"].node rect,#chatgpt-mermaid-\_r_842\_ [data-look="neo"].cluster rect,#chatgpt-mermaid-\_r_842\_ [data-look="neo"].node polygon{stroke:url(#chatgpt-mermaid-\_r_842\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_842\_ [data-look="neo"].swimlane.cluster rect{filter:none;}#chatgpt-mermaid-\_r_842\_ [data-look="neo"].node path{stroke:url(#chatgpt-mermaid-\_r_842\_-gradient);stroke-width:1px;}#chatgpt-mermaid-\_r_842\_ [data-look="neo"].node .outer-path{filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_842\_ [data-look="neo"].node .neo-line path{stroke:rgb(31, 78, 148);filter:none;}#chatgpt-mermaid-\_r_842\_ [data-look="neo"].node circle{stroke:url(#chatgpt-mermaid-\_r_842\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_842\_ [data-look="neo"].node circle .state-start{fill:#000000;}#chatgpt-mermaid-\_r_842\_ [data-look="neo"].icon-shape .icon{fill:url(#chatgpt-mermaid-\_r_842\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_842\_ [data-look="neo"].icon-shape .icon-neo path{stroke:url(#chatgpt-mermaid-\_r_842\_-gradient);filter:drop-shadow( 1px 2px 2px rgba(185,185,185,1));}#chatgpt-mermaid-\_r_842\_ .node text{font-size:14px;font-weight:600;letter-spacing:normal;fill:rgb(153, 206, 255);}#chatgpt-mermaid-\_r_842\_ .edgeLabels text{font-size:13px;font-weight:600;letter-spacing:-0.08px;fill:rgb(153, 206, 255);}#chatgpt-mermaid-\_r_842\_ .node tspan[font-weight="normal"],#chatgpt-mermaid-\_r_842\_ .edgeLabels tspan[font-weight="normal"]{font-weight:600;}#chatgpt-mermaid-\_r_842\_ .edgeLabel .label rect{opacity:1;rx:13px;ry:13px;fill:rgb(0, 14, 26);stroke:rgb(26, 62, 95);stroke-width:1px;}#chatgpt-mermaid-\_r_842\_ .node rect,#chatgpt-mermaid-\_r_842\_ .node circle,#chatgpt-mermaid-\_r_842\_ .node ellipse,#chatgpt-mermaid-\_r_842\_ .node polygon,#chatgpt-mermaid-\_r_842\_ .node path{fill:rgb(0, 40, 77);stroke:rgba(255, 255, 255, 0.1);stroke-width:1px;}#chatgpt-mermaid-\_r_842\_ .node rect{rx:16px;ry:16px;}#chatgpt-mermaid-\_r_842\_ .mermaid-decision .label-container{fill:rgb(0, 14, 26);stroke:rgb(26, 62, 95);stroke-dasharray:2,2;}#chatgpt-mermaid-\_r_842\_ .edgePaths .flowchart-link{stroke:rgba(255, 255, 255, 0.498);stroke-width:1px;stroke-linecap:round;stroke-linejoin:round;}#chatgpt-mermaid-\_r_842\_ .marker{fill:rgba(255, 255, 255, 0.498);stroke:rgba(255, 255, 255, 0.498);}#chatgpt-mermaid-\_r_842\_ :root{--mermaid-font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;}发现访问失败故障来自 CPU？收集 TrapCSR、PC、Hart、Domain读取 IOMMU / I/O MPT /Device ErrorPage Fault？检查 satp、页表和 VATranslation还原 PA、PMP/PMA、MPTMPT 权限与当前 Domain正确？检查 Root、MPTE、Boot 配置检查缓存同步、PMP/PMA、总线检查 DeviceClassification、DMAPA、权限及失效修复并构建可重复测试YesNoYesNo: Access FaultNoYes

这套流程并不意味着必须严格按图依次检查每一项，而是帮助团队在定位初期就区分不同层次的责任。

### Chapter 18 — Staff Engineer 测试题

1\. Linux 报 scause=5，能否直接断定是 SMMPT 拒绝了访问？

2\. 为什么 stval=0x90001000 不能直接证明访问的是 PA 0x90001000？

3\. Smmpt43 下，PA=0x90001000 的三级索引与 4KB 子页面索引分别是什么？

4\. MPTE 的 XWR 已允许 Read，但 Linux 仍然报 Load Access Fault，应如何判断？

5\. MPT 更新后已经执行 fence rw,rw，为什么仍可能沿用旧权限？

6\. Hart 0 已执行 MFENCE.PA，Hart 1 是否必然使用新权限？

7\. 为什么在撤销权限时仅修改 MPTE=Invalid 不满足安全要求？

8\. 为什么 DMA 越权访问时没有发生 CPU scause？

9\. Cold Boot 正常、CPU Hotplug 后安全域访问异常，最可能遗漏了哪些状态？

10\. GDB 可以直接 dump OP-TEE Private Memory，但 Linux 的恶意 Load 被拒绝，是否存在矛盾？

显示 10 道题的参考答案

本章掌握标准： 遇到 Access Fault 时，不仅能定位错误指令，还能追踪到 Domain、PA、具体 MPTE、缓存同步及硬件拒绝访问的原因；可以为硬件验证团队提供明确的信号和事务排查需求。

# Chapter 19 — SMMPT 验证体系与安全测试

## 19.1 本章目标：从“功能正常”走向“安全属性已验证”

假设我们已经完成了：

- Linux 正常启动。
- OP-TEE 正常启动。
- CA 可以调用 TA。
- 多 Hart 系统可以稳定运行。
- Shared Memory 读写正常。

能否宣布 SMMPT 集成完成？

不能。

因为这些测试主要证明正常业务流程可以运行，却没有证明安全隔离足够可靠。

例如：

```
Linux -> OP-TEE InvokeCommand()
```

成功返回，只说明调用链是通的。

并不能说明恶意 Linux 无法通过另一个地址映射访问 OP-TEE Private Memory，也无法证明 DMA 设备无法绕过保护。

所以本章必须建立两个概念：

Functional Correctness：

合法操作应该成功。

Security Correctness：

非法操作必须被拒绝，而且任何失败路径都不能意外扩大权限。

这两部分缺一不可。

## 19.2 建立 SMMPT 的系统验证分层

我建议把验证体系划分为五个层次。

L5

System Security

Linux / OP-TEE / DMA / Multi-domain

L4

Integration

OpenSBI / RDSM / UEFI / Driver

L3

Concurrency

Multi-Hart / Fence / In-flight DMA

L2

Architecture

CSR / MPT Walk / MPTE / Trap

L1

Model & Unit Test

Encoding / State Machine / Reference Model

这五层有不同的验证目标：

| 层次 | 核心问题      | 推荐手段                          |
| -- | --------- | ----------------------------- |
| L1 | 权限算法是否正确  | Software Oracle、Unit Test     |
| L2 | 硬件是否符合规范  | ISA Test、Directed Test        |
| L3 | 并发与同步是否正确 | SMP Stress、Race Injection     |
| L4 | 软件栈能否正确协同 | Boot、RPC、Shared Memory        |
| L5 | 恶意主体能否越权  | Negative Test、Security Review |

我们先从最容易建立、且价值很高的 Reference Model 开始。

## 19.3 建立一个 MPT Reference Model

Reference Model 是验证工作的一个核心工具。

它并不是重新实现一个完整 SoC，而是以软件形式定义：

给定 Domain、Physical Address 和 Access Type，按照规范，最终应该允许还是拒绝。

例如：

```
enum access_type {
    ACCESS_READ,
    ACCESS_WRITE,
    ACCESS_EXEC
};

enum access_result {
    ACCESS_ALLOW,
    ACCESS_DENY,
    ACCESS_INVALID_ENTRY
};
```

可以建立一个独立的接口：

```
enum access_result
mpt_reference_check(
    const struct mpt_model *model,
    uint64_t root_pa,
    uint64_t address,
    enum access_type access);
```

实现时需要遵循对应 Smmpt 模式的 Page Walk 算法，包括：

- 各级索引。
- Leaf 与 Non-leaf。
- `V/L/N`。
- 保留位及非法编码。
- NAPOT 权限。
- `XWR`。
- 超出物理地址宽度的异常。

这个 Reference Model 应当独立于被测 RDSM 的 MPTE 操作代码。

否则，测试程序与真实 Firmware 使用同一个错误的编码函数，可能出现“错误实现验证错误实现，但所有测试都通过”的问题。

### 19.3.1 Differential Testing

假设输入：

```
Domain = Linux
PA = 0x90001000
Access = Read
```

Reference Model 返回：

```
DENY
```

硬件访问得到：

```
Load Access Fault
```

说明两者在这次测试中的行为一致。

反过来：

```
Reference Model = DENY
Hardware = READ SUCCESS
```

这是需要立即调查的安全违规。

而：

```
Reference Model = ALLOW
Hardware = ACCESS FAULT
```

同样是失败，但要继续判断是否另有 PMP/PMA 限制，或是测试环境的其他约束造成了 Access Fault。

因此，严格的 Differential Testing 还必须固定其他影响访问的硬件状态，而不是简单地把所有 Access Fault 都归因于 MPT。

## 19.4 MPTE Encoding 与边界测试

SMMPT 的访问权限是由 MPTE 中的编码字段决定的。

因此必须测试合法编码和非法编码。

### 19.4.1 基础 Encoding 测试

| 测试                  | 输入        | 预期                 |
| ------------------- | --------- | ------------------ |
| Invalid MPTE        | `V=0`     | Access Fault       |
| Valid Non-leaf      | `V=1,L=0` | 继续 Table Walk      |
| Valid Leaf          | `V=1,L=1` | 检查对应 XWR           |
| Invalid Combination | `L=0,N=1` | Access Fault       |
| Reserved Bits       | 置位保留字段    | Access Fault       |
| Read-only Leaf      | `R=1,W=0` | Read 允许，Write 拒绝   |
| No-access Leaf      | `XWR=000` | R/W/X 拒绝           |
| Unsupported MODE    | 写入不支持的模式  | 按 CSR WARL 规则观察实际值 |

### 19.4.2 最容易遗漏的边界

在 Smmpt43 的普通底层 Leaf 中，一个 MPTE 描述 16 个 4KB 页。

因此必须测试：

```
Leaf Entry:
    Page[0]
    Page[1]
    ...
    Page[14]
    Page[15]
```

以及下一条 MPTE 的：

```
Page[0]
```

尤其应该构造：

```
Page[14] = Read Allowed
Page[15] = No Access
Next MPTE Page[0] = Read Allowed
```

验证硬件没有发生：

- 子页面索引偏移。
- Bit Shift 错误。
- 权限位串扰。
- 相邻 MPTE 边界错误。

这种测试尤其适合 FPGA/RTL 验证，因为逻辑错误往往具有确定性。

## 19.5 Fault Injection：主动制造错误

Fault Injection 不应只是随机修改寄存器，而是有目的地验证异常路径。

我建议设计四类。

### A. MPTE Fault Injection

故意修改：

- Valid Bit。
- Leaf Bit。
- Reserved Bits。
- XWR Permission。
- Next-level PPN。

然后验证：

1. 硬件是否正确拒绝访问。
2. 异常类型是否正确。
3. Firmware 能否保留足够诊断信息。
4. 错误是否被限制在预期影响范围内。

### B. Domain Context Fault Injection

例如：

```
Expected:
    Hart 2 -> Linux Domain

Injected:
    Hart 2 uses incorrect SDID or MPT root
```

这类注入应该在受控的 Test Firmware 中执行。

测试目的不是证明错误配置后系统仍能正常运行，而是确保 Domain Context 恢复、验证和错误处理机制符合安全设计，且不会在生产路径中暴露不受保护的切换窗口。

### C. Synchronization Fault Injection

例如故意遗漏一个相关 Hart 的 MPT Cache Synchronization。

测试是否能观察到旧权限继续被使用。

这是专门用于验证测试框架敏感度的实验，不能将“故意漏同步”本身视为合法 Firmware 实现。

### D. DMA Fault Injection

例如：

```
Device A
    |
    v
DMA Write to OP-TEE Private PA
```

预期：

```
DMA Denied
```

还必须确认：

- 目标物理内存未被修改。
- I/O Fault 或设备错误能够被检测。
- 后续合法 DMA 不受永久性损坏。
- 失败事务的恢复策略符合平台要求。

负向测试应该始终在隔离的验证环境或可恢复的平台上进行，避免让破坏性测试影响共享的开发资源。

## 19.6 Domain Isolation Negative Tests

这一部分是安全验证的核心。

假设：

```
Domain A = Linux
Domain B = OP-TEE
```

我们构建下面的权限矩阵：

| 发起者    | 目标内存           | Read  | Write | Execute |
| ------ | -------------- | ----- | ----- | ------- |
| Linux  | Linux Private  | Allow | Allow | 按映射策略   |
| Linux  | OP-TEE Private | Deny  | Deny  | Deny    |
| OP-TEE | OP-TEE Private | Allow | Allow | 按映射策略   |
| OP-TEE | Linux Private  | 按协议   | 按协议   | 通常不授权   |
| Linux  | Shared Memory  | Allow | Allow | Deny    |
| OP-TEE | Shared Memory  | Allow | Allow | Deny    |

这是教学示例中的安全策略，不是 SMMPT 强制规定的全局权限矩阵。

### 19.6.1 如何验证 Linux 无法读取 OP-TEE Private？

一种方法是在专用测试环境中：

1. 在 Linux Domain 建立指向目标 PA 的受控测试映射。
2. 确保 VA Translation 本身合法。
3. 确保测试不会因为其他内存保护机制提前失败。
4. 从测试执行上下文发起 Load。
5. 捕获 Fault。
6. 验证 Fault 类型和对应地址。
7. 将执行状态恢复到测试框架。

预期：

```
Load Access Fault
```

但是必须提醒：

如果 VA Translation 本身失败，返回的是 Page Fault，那么该测试并没有证明 MPT 的拒绝能力。

同样，如果 PMP 在 MPT 之前就已经拒绝该访问，也不能据此证明 SMMPT 单独工作正确。

理想的架构级测试应当控制其他权限条件，使被测 MPT 成为决定性约束。

## 19.7 Memory Ownership Transfer 的生命周期测试

我们设计一个标准状态机：

```
FREE
 |
 v
OWNED_BY_LINUX
 |
 v
SHARED_WITH_OPTEE
 |
 v
REVOKING
 |
 v
OWNED_BY_LINUX
 |
 v
FREE
```

对于每次状态转换，测试程序都应该验证三个维度。

Software State：

```
Owner
Grant List
Reference Count
Pinned State
```

Hardware State：

```
Linux MPT Permission
OP-TEE MPT Permission
I/O MPT Permission
```

Execution State：

```
Active Hart
Outstanding Access
In-flight DMA
Pending Synchronization
```

### 19.7.1 一个危险的状态迁移

假设：

```
T0: Linux owns page
T1: Linux shares page with OP-TEE
T2: Linux requests revoke
T3: Firmware updates MPTE
T4: Linux reuses page
```

如果 T3 和 T4 之间没有等待所有必要的同步完成，就可能出现 Use-after-Revoke。

因此测试需要验证以下不变量：

\\[ \text{ReuseAllowed} \Rightarrow \text{RevocationComplete} \\]

也就是说，页面所有权返回 Linux 之后，不能还存在旧 Domain 可合法继续使用的授权状态。

对于 DMA，需要将设备停止、队列耗尽、IOMMU/I/O MPT 失效等条件纳入撤销完成判定。

## 19.8 多 Hart 并发与 Race Condition 测试

SMMPT 的很多严重 Bug 不会出现在单 Hart 上。

因此建议使用至少三个 Hart 构造并发场景：

```
Hart 0:
    Repeatedly updates MPT permissions

Hart 1:
    Repeatedly reads target memory

Hart 2:
    Repeatedly executes Domain Switching
```

测试期间记录：

```
Current Domain
Current MPT Generation
Request Type
Access Result
Permission Transition State
```

必须明确一点：

并发测试不能简单规定“MPTE 写入之后所有 Hart 立即按照新权限访问”。

因为硬件权限缓存具有规范规定的同步和可见性语义。

所以测试应把时间划分为：

```
BEFORE_UPDATE
UPDATE_IN_PROGRESS
SYNCHRONIZATION_IN_PROGRESS
AFTER_COMPLETION
```

真正需要严格验证的是：

> 一旦 Firmware 向调用方报告权限转移完成，所有处于该完成协议约束内的相关访问者必须符合新的安全权限。

### 19.8.1 如何证明不存在旧权限泄漏？

我建议引入 `generation` 机制：

```
struct domain_protection_state {
    uint64_t generation;
    uint64_t completed_generation;
};
```

每次修改 MPT：

```
generation++
    |
    v
Update MPT
    |
    v
Synchronize all relevant Harts
    |
    v
Synchronize relevant I/O components
    |
    v
completed_generation = generation
```

该结构是参考 Firmware 设计，不是标准规定的数据结构。

测试时，只有 `completed_generation` 达到对应值，才允许断言新权限已经全局生效。

即便如此，还要区分相关访问者是否进入和完成同步、是否仍有 In-flight Transaction，以及同步完成前后各个观察者能够看到什么。

## 19.9 DMA 安全验证矩阵

前面已经讲过：CPU MPT 测试不能代替 DMA 测试。

建议至少覆盖以下场景：

| 测试     | 发起者           | 目标                       | 预期                |
| ------ | ------------- | ------------------------ | ----------------- |
| DMA-01 | Linux Device  | Linux Buffer             | Allow             |
| DMA-02 | Linux Device  | OP-TEE Private           | Deny              |
| DMA-03 | Secure Device | Secure Buffer            | Allow             |
| DMA-04 | Secure Device | Unauthorized Buffer      | Deny              |
| DMA-05 | Device        | Revoked Shared Page      | 完成撤销后 Deny        |
| DMA-06 | Device        | Invalid IOVA             | Translation Fault |
| DMA-07 | Device        | Valid IOVA, denied MPT   | MPT Check Deny    |
| DMA-08 | Device        | Boundary-crossing Buffer | 只允许授权范围内访问        |

这些测试的前提是平台实际实现了对应的 I/O 隔离机制。单纯具备 CPU 侧 `Smsd` 并不意味着上述 DMA 保护天然成立。

尤其要分别验证：

```
IOMMU Translation Success
    +
I/O MPT Permission Denied
```

因为这种情况最能证明 I/O MPT 的独立安全作用，而不是普通地址转换故障。

## 19.10 Performance Benchmark：如何评价 SMMPT 的成本？

SMMPT 引入多级 Table Walk、权限缓存与同步操作，必然需要考虑性能开销。

但性能测试不能只看某个单独 Load 的平均耗时。

建议分别测量：

### 19.10.1 Memory Access Overhead

对比：

```
Baseline Platform
        vs
SMMPT-enabled Platform
```

分别运行：

- Sequential Read。
- Random Read。
- Sequential Write。
- Random Write。
- Memory Intensive Workload。

注意：比较时需要控制 CPU Frequency、Cache State、Memory Placement、编译参数及其他隔离机制，否则无法归因于 MPT。

### 19.10.2 Domain Switch Latency

测量：

```
Linux -> OP-TEE -> Linux
```

将时间拆为：

\\[ T\_{\text{call}} = T\_{\text{transport}} +T\_{\text{switch}} +T\_{\text{TEE}} +T\_{\text{return}} \\]

进一步分解：

```
Trap / Transport Overhead
Save Context
Program Domain Context
Protection Synchronization
Restore Context
Return to Supervisor
```

注意不同实现可能有重叠工作，分解测量时要避免重复计时。

### 19.10.3 Permission Update Cost

测试：

```
Grant 1 Page
Grant 16 Pages
Grant 256 Pages
Revoke 1 Page
Revoke 256 Pages
```

对比：

- 单个 MPTE 修改。
- Batched MPTE 修改。
- Local Synchronization。
- Remote Hart Synchronization。
- I/O MPT Synchronization。

这可以帮助决定哪些接口应该批量授权、哪些操作需要异步处理，以及哪些操作必须同步完成。

### 19.10.4 多 Hart 扩展性

测量：

```
1 Hart
2 Harts
4 Harts
8 Harts
16 Harts
```

观察：

- MPT Update Throughput。
- Synchronization Latency。
- Domain Switch Tail Latency。
- Shared Memory Grant/Revoke Latency。

不仅看平均值，还应关注 P95/P99 等尾延迟。

因为一次跨 Hart 同步可能被一个响应慢的 Hart 拖延，导致 TEE 调用的尾延迟显著增加。

性能验收阈值应由目标 SoC 需求确定，而不是凭空设定一个固定数字。

## 19.11 建立系统 Regression Test Matrix

下面给出一份可以直接用于项目 Test Plan 的缩略矩阵。

| Test ID    | 模块             | 场景                          | 优先级  |
| ---------- | -------------- | --------------------------- | ---- |
| MPT-001    | CSR            | MODE/PPN/SDID 配置与读回         | P0   |
| MPT-002    | Encoding       | Invalid / Reserved MPTE     | P0   |
| MPT-003    | Walk           | 三级 Table Walk               | P0   |
| MPT-004    | Permission     | R/W/X 正负向权限                 | P0   |
| MPT-005    | Boundary       | 4KB / 64KB 权限边界             | P0   |
| DOM-001    | Domain         | Linux → OP-TEE Switch       | P0   |
| DOM-002    | Domain         | OP-TEE → Linux Return       | P0   |
| DOM-003    | SMP            | Hart Warm Boot 与 Domain 状态  | P0   |
| MEM-001    | Shared Memory  | Grant/Revoke                | P0   |
| MEM-002    | Shared Memory  | 访问撤销完成后的页面                  | P0   |
| MEM-003    | Security       | Linux 访问 TEE Private        | P0   |
| SYNC-001   | MPT Cache      | Local Synchronization       | P0   |
| SYNC-002   | SMP            | Remote Hart Synchronization | P0   |
| DMA-001    | I/O Isolation  | DMA 访问 TEE Private          | P0\* |
| DMA-002    | I/O Isolation  | Revoke 后 DMA                | P0\* |
| BOOT-001   | Boot           | Cold Boot                   | P0   |
| BOOT-002   | Boot           | Warm Boot                   | P0   |
| BOOT-003   | Boot           | Linux + UEFI Memory Map     | P0   |
| PERF-001   | Performance    | Domain Switch Latency       | P1   |
| PERF-002   | Performance    | Grant/Revoke Throughput     | P1   |
| STRESS-001 | Stability      | Multi-Hart Stress           | P1   |
| FAULT-001  | Error Handling | Fault Injection             | P1   |

\*DMA 项目中的 P0 以平台具备相应 I/O 隔离功能为前提。如果目标设计需要 DMA 安全隔离但硬件尚未具备，则应该记录为架构能力缺口，不能简单将测试标记为通过。

### 19.11.1 一个重要的测试规范：Test Pass 不能只看 Return Code

例如：

```
Test: Linux accesses OP-TEE Private
Result: Linux returned error
```

这还不够。

必须同时确认：

1. Load/Store 真的被发起了。
2. VA Translation 没有提前拒绝。
3. 目标 PA 属于预期安全区域。
4. MPT 权限本身是拒绝状态。
5. Fault 符合预期访问类型。
6. Secure Memory 没有被意外读取或修改。
7. 测试后的安全状态没有被破坏。

否则，“测试失败”或“内核返回错误”可能被误判为 SMMPT 通过。

### 19.11.2 推荐的测试结果格式

```
test_id: MEM-003
name: linux_read_optee_private

environment:
  platform: rv64_soc
  mmpt_mode: Smmpt43
  harts: 16

input:
  domain: linux
  physical_address: 0x90001000
  access: READ

expected:
  mpt_result: DENY
  cpu_exception: LOAD_ACCESS_FAULT

observed:
  mpt_result: DENY
  cpu_exception: LOAD_ACCESS_FAULT
  unexpected_memory_change: false

result: PASS
```

这里的 `mpt_result` 是测试框架或硬件 Trace 得到的结果，不是普通 Linux 必然可读取的标准硬件寄存器。

实际测试记录还应保存：

- Spec Revision。
- RTL Revision。
- Firmware Commit。
- Linux/OP-TEE Commit。
- MPT 配置哈希。
- Test Seed。
- Trace Reference。
- Timestamp。
- 判定依据。

当以后某个版本发生 Regression 时，这些信息能够帮助快速重现当时的具体环境。

## 19.12 如何把测试体系接入 CI？

我建议采用三个阶段。

Stage A：每次 Commit 必须运行

MPTE Encoding、Reference Model、Static Analysis、基本 Firmware Unit Test、Memory Contract 一致性检查。

目标是快速发现确定性逻辑错误。

Stage B：夜间系统回归

Linux Boot、OP-TEE Boot、共享内存、Domain Switch、SMP、异常注入、授权与撤销测试。

目标是验证完整集成行为。

Stage C：版本发布前安全验证

DMA Isolation、Multi-Hart Long Stress、Cache Synchronization、Fault Injection、Performance Baseline 和安全架构评审。

目标是判断版本是否具备发布条件。

一个合格的安全验证报告应区分：

- PASS： 在已定义的环境与覆盖范围内满足预期。
- FAIL： 已观察到违反要求的行为。
- BLOCKED： 测试无法执行，缺少依赖。
- NOT SUPPORTED： 当前硬件或 Firmware 不支持该能力。
- NOT TESTED： 尚未验证。

尤其不要把 `NOT SUPPORTED` 或 `BLOCKED` 转换成 `PASS`。

## 19.13 如何判断 SMMPT 集成已经达到交付标准？

建议从下面四个层次判断。

第一层：Architecture Compliance

明确实现的 SMMPT/Smsd 模式、可选扩展、CSR 行为和规格版本，并完成适用的架构级验证。

第二层：Functional Integration

Linux、OP-TEE、RDSM 能正常协作，支持预期的启动、调用、共享内存和多 Hart 功能。

第三层：Security Assurance

正向和负向测试均通过，特别是跨域访问拒绝、动态撤销以及 DMA 隔离；已明确记录未覆盖的威胁和平台能力限制。

第四层：Operational Readiness

具备可重复构建、可重现故障、运行期监测和 Regression Test 能力。重大问题可以通过日志和硬件 Trace 定位。

只有同时满足与项目威胁模型对应的这些要求，才能认为系统具备工程交付基础。

### Chapter 19 — Staff Engineer 测试题

1\. Linux 与 OP-TEE 都能够正常运行，xtest 也全部通过，能否证明 SMMPT 隔离可靠？

2\. Reference Model 与 RTL 使用同一个 MPTE Encoding 函数，测试全部通过。这个验证设计有什么缺陷？

3\. 如何设计测试证明 SMMPT 的 Load Deny 生效，而不是 Linux 页表提前触发 Page Fault？

4\. 一次 Revoke 返回 Success 后，另一个 Hart 仍能读取旧页面。问题应归到哪个测试类别？

5\. 为什么对 Smmpt43 需要分别测试一个 Leaf 中的 16 个 4KB 页面？

6\. DMA Test 只验证设备无法访问错误 IOVA，是否足以证明 I/O MPT 隔离正确？

7\. 多 Hart Race Test 为什么必须引入 Permission Transition Completion 的概念？

8\. 某个 Negative Test 返回预期异常，但测试结束后 Secure Memory 被修改了。这应当如何判定？

9\. 如何评估 SMMPT 的性能开销而不受 Cache、CPU 频率和其他平台机制干扰？

10\. 如果要求对 SMMPT 项目签署安全交付验收报告，你最看重哪五份证据？

显示 10 道题的参考答案

本章掌握标准： 能为一个有真实安全隔离要求的 RISC-V SoC 设计从 Reference Model、架构测试、集成测试到多 Hart/DMA 安全验证的完整体系，并能够向架构评审委员会解释每一项测试究竟证明了什么、没有证明什么。

# Part V 总结：从规范理解转向真正的工程能力

回顾这三个章节，可以发现它们其实不是三个独立的话题。

Chapter 17 — Integration

确保 Linux、UEFI、RDSM、OP-TEE 对资源划分具有一致理解

Chapter 18 — Debug

当运行结果与预期不一致时，找到具体失效的权限、状态或同步环节

Chapter 19 — Verification

用可重复的实验和证据证明安全属性，在未来版本中持续检测回归

我认为本部分最值得真正记住的，是三个工程原则。

第一，Memory Ownership 必须有唯一的安全权威。 Linux、UEFI 和 OP-TEE 可以拥有各自的资源视图，但对跨 Domain 物理访问权限的最终裁决必须受可信硬件和 RDSM 管理。

第二，权限更新不是一个简单的 MPTE Store。 它涉及缓存、Hart、DMA 和同步协议。尤其在权限撤销时，必须把完成性定义清楚。

第三，安全隔离不能仅通过正常业务成功来证明。 必须有明确的负向测试、可观察证据和对未覆盖威胁的说明。

## 建议你在实际项目里完成的三个练习

1. 整理一份真实 SoC Memory Ownership Map。 把 DDR 分区、Boot Image、RDSM、OP-TEE、Linux、Shared Memory、UEFI/FDT 和 MPT 权限统一整理，检查是否存在矛盾。
2. 亲手追踪一次 Access Fault。 从异常指令开始，拿到 VA、PA、Hart、Domain、`mmpt`，手动解析 MPT，直到确认是哪一级权限导致失败。
3. 实现一组最小负向验证。 至少验证 Linux Read/Write TEE Private 被拒绝、合法 Shared Memory 可访问，以及一次 Revoke 完成后的访问被拒绝。

完成这三件事，意味着你对 SMMPT 的理解已经不只是 CSR 和 MPTE，而是开始真正掌握 从 Firmware、OS、Hardware Enforcement 到安全验证的完整闭环。

接下来的 Part VI — Chapter 20，就可以以这些成果为基础，完整设计并评审一套 RISC-V SMMPT + OP-TEE 安全平台。
