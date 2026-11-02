# Chapter 11 — RISC-V OP-TEE Memory Management & TA Isolation

RISC-V OP-TEE Architecture & Porting · 核心内核架构专题 · Staff Engineer 深度

本章的主题是 Memory Management、MMU、TA Address Space Isolation 和多核地址转换一致性。

对于 OP-TEE 而言，内存管理不是单纯的物理内存分配、虚拟地址转换或者 Page Table 管理，而是 Trusted Execution Environment 安全架构的一部分。

尤其在 RISC-V 上，我们必须同时考虑三个不同的隔离边界：

1. Linux Domain 与 OP-TEE Domain 之间的隔离。
2. OP-TEE Core 与 User TA 之间的隔离。
3. 不同 User TA 之间的隔离。

这三个边界涉及不同的权限模型和不同的执行主体。如果不能明确区分它们，就很难真正理解 RISC-V OP-TEE 的内存管理。

本章基于 RISC-V RV64、Sv39/Sv48、OpenSBI Domain 以及 OP-TEE upstream 的源码架构展开。关键 RISC-V 实现已对照 2026 年 10 月的 upstream 源码；对于不同平台、构建配置和仍在推进中的安全增强功能，会明确区分已有实现与设计方案。

## 本章学习目标

完成本章后，你应该能够：

- 从硬件层面解释 PMP、MMU、PTE、ASID、TLB 如何共同完成 TEE 内存隔离。
- 解释 OP-TEE Core、User TA、Shared Memory 和 Pager 的完整内存架构。
- 追踪一个 TA 地址空间从创建、映射、运行、切换到销毁的生命周期。
- 解释多个 TA 如何在同一个 OP-TEE S-mode Kernel 内安全地运行。
- 深入分析 `satp`、页表、ASID 和 `SFENCE.VMA` 的实际代码实现。
- 在多 hart 系统中设计正确的 Page Table Update 与 TLB Shootdown。
- 理解 Shared Memory 和动态分页引入的安全风险。
- 面对真实 SoC 上的 MMU、PMP 和缓存一致性问题，能够独立设计调试、验证与修复方案。

# 11.0 从全局看 OP-TEE Memory Architecture

在开始正式讨论 11.1～11.10 之前，先建立整体模型。

## 11.0.1 OP-TEE 的内存保护有两个主要层次

假设一个 RISC-V SoC 具有以下软件组件：

- OpenSBI：运行在 M-mode。
- Linux：运行在一个 S-mode Domain。
- OP-TEE：运行在另一个 S-mode Domain。
- TA A、TA B：运行在 OP-TEE 管理的 U-mode。
- Secure RAM：用于 OP-TEE Core 和 TA。
- Non-secure DRAM：由 Linux 使用，也可以提供 REE/TEE Shared Memory。

整体结构如下：

Two-level Memory Isolation

Linux Domain

Linux S-mode

Kernel VA / User VA

Linux Page Tables

只能访问授权物理区域

OP-TEE Domain

OP-TEE S-mode

Core Memory Mapping

TA A

U-mode

TA B

U-mode

每个 TA 的私有映射隔离

物理访问权限检查

OpenSBI / M-mode Isolation Management

PMP + Platform Firewall / IOPMP 等隔离机制

Linux RAM

REE private

TEE RAM

Core / TA private

Shared RAM

Explicit sharing

这里要建立第一个重要结论：

PMP / 平台硬件隔离机制主要解决 Linux Domain 与 OP-TEE Domain 之间的物理资源隔离，而 OP-TEE 自己的 S-mode MMU 还要负责 Core、TA A、TA B 之间的虚拟地址空间及访问权限隔离。

这两种机制并不互相替代。

具体而言：

- 即使 Linux 完全控制自己的 `satp` 和页表，也不能仅靠修改页表绕过正确配置的 PMP 物理访问限制。
- 即使 OP-TEE 的物理 RAM 对 Linux 不可访问，也不能保证 TA A 无法访问 TA B，后者仍需要 OP-TEE 内部的页表和权限机制。
- 即使 CPU 的 PMP 配置正确，也不意味着 DMA、外设 Bus Master 自动受到相同保护。

## 11.0.2 四种内存访问必须分开分析

| 访问行为                             | 主要保护机制                             |
| -------------------------------- | ---------------------------------- |
| Linux S-mode → OP-TEE Private PA | PMP / Platform Isolation           |
| TA A U-mode → OP-TEE Core VA     | S-mode MMU PTE Permissions         |
| TA A U-mode → TA B Private VA/PA | TA Address Space + PTE Permissions |
| DMA Device → OP-TEE Private PA   | IOMMU / IOPMP / SoC Firewall       |

为什么要强调这一点？

假设有一个安全漏洞：

> Linux root 用户构造任意物理地址映射，尝试读取 OP-TEE 中的密钥。

如果系统隔离正确，Linux 可以将目标 PA 填入自己的页表，但是 CPU 最终的物理访问仍应被 PMP 阻止。

而如果攻击者控制的是 TA A：

> TA A 构造指针，尝试读取 TA B 的私有 Heap。

此时两者处于同一个 OP-TEE Security Domain，PMP 无法仅凭 Domain 区分它们。访问必须由 OP-TEE 的 MMU 映射策略阻止。

这就是整章的核心设计逻辑。

# 11.1 OP-TEE Memory Regions：Core RAM、TA RAM、Shared Memory

## 11.1.1 为什么 OP-TEE 需要自己的 Memory Layout？

对于普通 Linux 应用程序，内存管理可以理解成：

```
Application
    |
    | malloc() / mmap()
    v
Linux Virtual Memory
    |
    | Page Tables
    v
Physical Memory
```

但 OP-TEE 的情况复杂得多。

因为 OP-TEE 同时需要：

1. 运行自己的 S-mode Kernel。
2. 加载多个不完全相互信任的 U-mode TA。
3. 与不可信的 Linux 共享数据。
4. 为加密、密钥、系统调用和安全服务分配私有内存。
5. 在某些配置下支持分页。
6. 在 SMP 环境中同时管理多个 Trusted Thread。

因此必须有一套明确的 Memory Region 分类和访问策略。

## 11.1.2 四类重要内存

| 区域                    | 用途                                          | 谁应该能访问          |
| --------------------- | ------------------------------------------- | --------------- |
| Core RAM              | OP-TEE Core text、rodata、data、BSS、Heap、Stack | OP-TEE Core     |
| TA RAM                | User TA 的代码、数据、Heap、Stack                   | 对应 TA，以及可信 Core |
| REE/TEE Shared Memory | 参数传递、RPC、数据交换                               | 经授权的 REE 与 TEE  |
| Secure Device MMIO    | 可信外设、硬件加密模块等寄存器                             | 授权安全软件及设备       |

这里需要区分两个概念：

Secure RAM 是一个安全属性或物理资源类别，而 Core RAM 和 TA RAM 是 OP-TEE 内部的逻辑使用区域。

例如，64 MiB 的 TEE 专用 DRAM 可以在物理布局上分成：

```
Physical TEE RAM (示意)
0x9000_0000
+--------------------------------+
| OP-TEE Core Image              |
| .text / .rodata / .data / .bss |
+--------------------------------+
| Core Heap / Stacks              |
+--------------------------------+
| User TA Memory Pool            |
|                                |
| TA A pages                     |
| TA B pages                     |
+--------------------------------+
| Other Secure Allocations       |
+--------------------------------+
0x9400_0000
```

这只是示意地址，不是 QEMU virt 或某款 SoC 的固定布局。

需要注意，TA RAM 不一定按 TA A、TA B 分配成连续的物理区间。实际的物理页可以来自共享的安全内存池，通过不同 TA 的 VA Mapping 实现隔离。

## 11.1.3 Static Memory Map 与 Dynamic Allocation

OP-TEE Core 初始化时，必须建立基础静态 Memory Map。

其中包含：

- Core 自身的代码和数据。
- 核心堆、栈和页表。
- 需要永久访问的外设。
- Secure RAM 和 Shared Memory 对应的虚拟地址空间。

但运行期间，User TA 加载、卸载和调用还需要动态映射。

因此可以把 OP-TEE Memory Management 分成：

Static Core Mapping

Boot-time

主要负责 Core 的长期映射、固定内存资源和引导所需的页表。

Dynamic User Mapping

Runtime

负责 TA 地址空间、临时共享参数、User Memory Regions 和动态页表维护。

这两个方面对应不同的 OP-TEE 源码层次：

```
optee_os/
  core/
    mm/
      core_mmu.c
      vm.c
      mobj.c
      pgt_cache.c
      tee_pager.c

    arch/
      riscv/
        mm/
          core_mmu_arch.c
          tlb_helpers_rv.S
```

在架构上：

- `core_mmu.c` 管理通用 Core 内存映射逻辑。
- `vm.c` 管理 User TA Virtual Memory Region。
- `mobj.c` 抽象被映射的内存对象。
- `pgt_cache.c` 管理 User Page Table 资源。
- `core_mmu_arch.c` 负责 RISC-V 相关的页表格式、映射操作及地址转换控制。
- `tlb_helpers_rv.S` 提供 RISC-V 本地地址转换同步指令的汇编接口。

这些职责可以在当前 upstream 中直接对应到实际实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+4



## 11.1.4 Memory Type 不等于 Memory Permission

这是一个很容易混淆的细节。

OP-TEE 对内存需要同时描述：

Memory Type：这块内存是什么？

例如 Core RAM、TA RAM、MMIO 或 Shared Memory。

Memory Permission：当前上下文可以怎样访问？

例如：

- Read / Write / Execute。
- Privileged / User。
- Global / Non-global。
- Cached / Device memory attributes。

一块物理 RAM 可以在不同执行上下文中具有不同的 VA 和 PTE 属性。

但不能因为某个 VA 被标记为 TEE RAM，就认为其天然具备 Secure/Non-secure 的硬件属性。

尤其在 RISC-V 中，标准 Sv39/Sv48 PTE 并没有一个等价于 Arm TrustZone NS Bit 的通用安全世界选择位。

> 一个关键设计原则：不要将软件内存分类标记误认为硬件安全隔离机制。

# 11.2 Sv39 / Sv48 在 OP-TEE 中的使用

## 11.2.1 `satp` 是整个 RISC-V S-mode MMU 的控制入口

在 RV64 中：

RV64 `satp` CSR

MODE

Bits 63:60

4 bits

ASID

Bits 59:44

16 bits

PPN

Bits 43:0

44 bits

这是 RV64 的架构字段布局；实际硬件支持的 ASID 宽度可能小于 16 位。

`satp` 包含三个字段：

- `MODE`：地址转换模式，例如 Bare、Sv39、Sv48。
- `ASID`：Address Space Identifier。
- `PPN`：Root Page Table 的 Physical Page Number。

例如：

```
csrw satp, a0
sfence.vma x0, x0
```

这段代码表达的是写入 `satp` 并进行本地地址转换同步。

但是 `satp` 的写入本身并不自动完成所有 Page Table Update 所需的同步操作。 `SFENCE.VMA` 的使用范围、执行时机以及目标 hart，需要根据实际修改内容和映射生命周期决定。

## 11.2.2 Sv39 的三级 Page Table

Sv39 使用三级页表：

```
Virtual Address (39-bit canonical)

+-------+-------+-------+------------+
| VPN2  | VPN1  | VPN0  | PageOffset |
| 9 bit | 9 bit | 9 bit |   12 bit   |
+-------+-------+-------+------------+

   satp.PPN
       |
       v
  Level 2 PGT
       |
       +---- VPN2 ----> Level 1 PGT
                            |
                            +---- VPN1 ----> Level 0 PGT
                                                 |
                                                 +---- VPN0 --> PTE
                                                                  |
                                                                  v
                                                           Physical Page
```

对应映射粒度：

| Leaf PTE 所在层 | 映射粒度  |
| ------------ | ----- |
| Level 2      | 1 GiB |
| Level 1      | 2 MiB |
| Level 0      | 4 KiB |

Sv48 在此基础上增加一级 VPN3，能够覆盖更大的虚拟地址空间。

其中一个很重要的设计考虑是：

OP-TEE 不应无条件将所有物理内存都映射成 4 KiB Pages。

因为 Core Image 中的连续区域可以考虑较大粒度映射，而 TA 的 Heap、Stack、Shared Memory 和独立权限区域通常需要更精细的粒度。

不过大页要求满足物理地址、虚拟地址对齐和权限一致等约束。不能为了减少页表或 TLB 消耗而跨越安全边界合并映射。

## 11.2.3 PTE 的权限位

RISC-V PTE 中最重要的基础标志：

| Flag | 意义       | 在 OP-TEE 中的用途    |
| ---- | -------- | ---------------- |
| `V`  | Valid    | 判断映射是否有效         |
| `R`  | Read     | 控制数据读取           |
| `W`  | Write    | 控制数据写入           |
| `X`  | Execute  | 控制取指             |
| `U`  | User     | 控制 U-mode 是否可访问  |
| `G`  | Global   | 标记与 ASID 无关的全局映射 |
| `A`  | Accessed | 标记页面曾被访问         |
| `D`  | Dirty    | 标记页面曾被写入         |

典型映射策略：

| 内存区域                  | PTE 权限示意      |
| --------------------- | ------------- |
| OP-TEE Core `.text`   | `V R X`，`U=0` |
| OP-TEE Core `.rodata` | `V R`，`U=0`   |
| OP-TEE Core `.data`   | `V R W`，`U=0` |
| TA `.text`            | `V R X U`     |
| TA `.rodata`          | `V R U`       |
| TA `.data`、Heap       | `V R W U`     |
| TA Stack              | `V R W U`     |
| Unmapped Guard Page   | `V=0`         |

表格展示的是期望的最终运行权限，而不是宣称所有平台的当前构建都已经对每一类 Core Section 实施完全一致的权限。

实际 RISC-V OP-TEE 架构代码中，`mattr_to_pte_bits()` 将通用的 `TEE_MATTR_*` 属性转成 RISC-V PTE 权限；对应的反向转换是 `pte_to_mattr()`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



一个很有意思的源码细节是：

```
/* 基于当前代码的简化逻辑，不是原函数完整拷贝 */

if (attr & TEE_MATTR_UR)
    bits |= PTE_R | PTE_U;

if (attr & TEE_MATTR_UW)
    bits |= PTE_W | PTE_U;

if (attr & TEE_MATTR_UX)
    bits |= PTE_X | PTE_U;
```

这个转换意味着：OP-TEE 通用 Memory Attribute 与 RISC-V 硬件 PTE Permission 并不是一一对应的独立权限集合。

RISC-V 的 `U` 位决定 User Page 类型，而 R/W/X 决定访问类型；Supervisor 对 User Page 的数据访问还受到 `sstatus.SUM` 等规则影响。

因此，在设计新的 Memory Permission 或对 Core/TA 权限进行安全审计时，不能简单地把 `TEE_MATTR_PRW`、`TEE_MATTR_URW` 当成硬件中两套完全独立的 Permission Bits。

## 11.2.4 `SUM`、`MXR` 与 Supervisor 访问 TA Memory

这里要引入两个很重要的 `sstatus` 位。

SUM — Supervisor User Memory Access

控制 S-mode 是否可以通过 Load/Store 访问标记 `PTE_U=1` 的页面。

通常情况下，S-mode 不应该随意访问 User Page。

当 OP-TEE Core 需要从 TA Buffer 中复制数据时，必须通过受控的访问机制处理这种权限关系。

即使 `SUM=1`，S-mode 也不能因此直接执行 U-mode 页面中的代码。

MXR — Make eXecutable Readable

如果开启，允许某些只具备执行权限的页面被读取。

这可以影响 Execute-only Memory 的实际保护效果。

因此，TA 权限设计不仅要审计 PTE，还要审计运行期间 `sstatus.SUM`、`sstatus.MXR` 的配置。

## 11.2.5 ASLR 与虚拟地址空间

ASLR（Address Space Layout Randomization）改变的是运行时虚拟地址布局。

例如，同一个 TA 的代码：

```
Boot / Load 1:
    TA Code VA = 0x0000000041000000

Boot / Load 2:
    TA Code VA = 0x0000000063000000
```

这两个地址只是用于说明 ASLR 效果。

ASLR 并不需要改变 TA 物理内存的安全归属，也不是 Domain Isolation 的替代品。

它主要提高内存破坏漏洞利用中的地址预测难度。

在当前 OP-TEE 上游中，Core ASLR 与 TA ASLR 都有实现，RISC-V 的运行时验证工作也已经推进。需要注意，配置选择了 ASLR 并不意味着随机化一定生效，实际还取决于启动随机种子等条件。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



# 11.3 Core MMU 与 User TA MMU 的权限边界

这是本章第一个必须从源码深入理解的核心问题。

## 11.3.1 OP-TEE 并没有两个物理 MMU

首先要纠正一种很容易形成的误解：

> Core MMU 与 User TA MMU 是两个独立的硬件 MMU。

这是不正确的。

在典型 RV64 hart 上，S-mode 和 U-mode 的地址转换共享同一套硬件地址转换机制。

所谓 Core MMU 与 User TA MMU，更多是在描述：

- Core Virtual Address Mapping。
- TA User Virtual Address Mapping。
- 不同映射区域及其生命周期。
- OP-TEE 如何管理这些映射。

因此，实际执行中使用的仍是当前 hart 的 `satp` 及其关联 Page Table。

## 11.3.2 OP-TEE 的 Page Table 如何组织？

一个关键实现细节是：当前 RISC-V OP-TEE 不必在每次 TA 切换时，将 `satp.PPN` 完整替换为另一套完全独立的 Root Page Table。

从当前 upstream 的 `core_mmu_arch.c` 可以看到：

- Core 为 hart 建立 Root Page Table。
- Core 和 User Mapping 的管理相互区分。
- 存在一个为 User VA Range 预留的 Page Table Entry。
- 当前 TA 的 User Page Table 可以挂接到该入口。
- `satp.ASID` 用于标识正在使用的 User Mapping。

其架构可简化为：

```
Hart 0 Root PGT
     |
     +---- Core Mapping
     |
     +---- User Mapping Slot ----> TA A Page Tables


Hart 1 Root PGT
     |
     +---- Core Mapping
     |
     +---- User Mapping Slot ----> TA B Page Tables
```

两个 hart 可以同时使用不同的 User Mapping，但是 Core Mapping 可以保持一致的逻辑布局。

这是一种将通用内核地址空间与当前 TA 用户地址空间组合的方案。

实际代码中可见 `root_pgt[CFG_TEE_CORE_NB_CORE]`、`user_pgts[CFG_NUM_THREADS]`，以及 `set_user_va_idx()` 和 `core_mmu_set_user_map()`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



## 11.3.3 `core_mmu_set_user_map()`：理解 TA 切换的关键函数

下面是根据当前 RISC-V 实现提取并简化的关键逻辑：

```
/* 教学简化版，省略部分安全检查和异常处理 */

void core_mmu_set_user_map(struct core_mmu_user_map *map)
{
    unsigned long satp = read_satp();
    struct mmu_pte *pte = get_user_mapping_entry();

    satp &= ~SATP_ASID_MASK;

    if (map && map->user_map) {
        unsigned long ptp =
            make_table_pte(map->user_map);

        set_pte(pte, ptp);
        page_table_write_barrier();

        satp |= map->asid << SATP_ASID_SHIFT;
        write_satp(satp);
    } else {
        set_pte(pte, 0);
        page_table_write_barrier();
    }

    tlbi_all_local();
}
```

实际函数还会屏蔽并恢复异常状态。上面的抽象用于突出其内存管理设计。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



现在逐步分析。

Step 1：找到当前 hart 的 User Mapping Entry

OP-TEE 并不是直接覆盖 Core Mapping，而是选择当前 hart 中预留给 User Address Space 的页表入口。

Step 2：将 TA Page Table 挂接到这个入口

例如：

```
Before:
    User PTE = Invalid

After:
    User PTE -> TA A Page Table
```

Step 3：更新当前 ASID

这样硬件可以区分不同地址空间的非全局地址转换缓存。

Step 4：执行本地 TLB Synchronization

因为页表入口改变了，旧地址转换不能继续被使用。

特别注意：这里的 `tlbi_all_local()` 是 local hart 操作。

为什么这时不一定需要 Remote TLB Shootdown？

因为这里改变的是当前 hart 专属的 User Mapping Slot。其他 hart 有各自的入口和当前 User Mapping。

但是，这个事实并不意味着 OP-TEE 所有页表修改都只需要 local TLB invalidation。

如果修改的是多个 hart 都可能使用的某个 TA Page Table，或共享的 Core Mapping，那就必须考虑 Remote Invalidation。

这是后面 11.9 的重点。

## 11.3.4 Core Mapping 与 User Mapping 的一个重要区别

正常情况下，User TA 只能访问当前 Page Table 中带有相应 User 权限的区域。

因此：

```
                  OP-TEE S-mode
                         |
       +-----------------+------------------+
       |                                    |
   Core Mapping                         TA Mapping
   U = 0                                U = 1
       |                                    |
   Core Text/Data                    TA Text/Data/Heap
       |                                    |
       +-----------------+------------------+
                         |
                   Current MMU
```

虽然 TA 可能在同一个页表树中看到 Core Mapping 对应的虚拟地址布局，但 U-mode 不能凭借知道地址，就访问 `U=0` 的 Core Pages。

更重要的是，另一 TA 的私有映射不应该同时无条件存在于当前 TA 可访问的 User Address Space 中。

## 11.3.5 与 Arm64 OP-TEE 的比较

你有 Armv7-A 和 Armv8-A 的 MMU 开发经验，因此可以利用下面的对应关系：

| Arm64                     | RISC-V RV64              | 概念               |
| ------------------------- | ------------------------ | ---------------- |
| `TTBR0_EL1` / `TTBR1_EL1` | `satp` 和 Page Table 层级组织 | 地址转换根            |
| `ASID`                    | `satp.ASID`              | 地址空间标识           |
| `TLBI`                    | `SFENCE.VMA`             | 地址转换同步           |
| EL1/EL0 Permission        | PTE `U/R/W/X`、`SUM` 等    | Privilege 访问控制   |
| Stage-1 Translation       | Sv39/Sv48 Translation    | 虚拟地址到物理地址        |
| Secure/Normal PA 安全属性     | PMP / Platform Isolation | 跨安全 Domain 的物理隔离 |

这只是功能上的比较，并非一一对应的寄存器或机制映射。

特别是 RISC-V `satp` 并不提供 Arm64 那样的 `TTBR0_EL1` / `TTBR1_EL1` 双根寄存器模型，因此 RISC-V OP-TEE 需要通过自身的 Page Table 组织方式实现 Core/User Mapping 的共存与切换。

# 11.4 `satp`、ASID、TLB：地址空间切换的三个核心机制

前面已经看到了 OP-TEE 如何将 TA Page Table 挂接到当前 hart 的 User Mapping Slot。

现在我们要深入一个更本质的问题：

为什么仅仅切换 Page Table 还不够？为什么还需要 ASID 和 TLB Synchronization？

## 11.4.1 TLB 解决的是什么问题？

如果没有 TLB，每次读取一个虚拟地址都可能进行多级 Page Table Walk。

以 Sv39 为例：

```
CPU: load from VA
    |
    v
Translate VA
    |
    +-- Read satp.PPN
    |
    +-- Access Level 2 PTE
    |
    +-- Access Level 1 PTE
    |
    +-- Access Level 0 PTE
    |
    +-- Check Permissions
    |
    v
Physical Address
    |
    v
Memory Access
```

多级页表访问成本较高，所以硬件可以缓存地址转换结果。

例如：

```
TLB Entry:
    VA Page = 0x40000000
    PA Page = 0x90010000
    Permission = RW
    ASID = 10
```

当同一个地址再次被访问时，CPU 可能直接使用已缓存的转换信息。

因此，一旦软件修改了页表，就出现一个问题：

Page Table 已经改变，但 CPU 可能还在使用旧的地址转换。

这也是为什么 TLB Synchronization 是安全问题，而不只是性能问题。

## 11.4.2 为什么需要 ASID？

假设 TA A 和 TA B 都使用虚拟地址 `0x40000000`。

但是：

```
TA A:
    VA 0x40000000
        ->
    PA 0x90010000

TA B:
    VA 0x40000000
        ->
    PA 0x90030000
```

如果 TLB 只使用虚拟地址作为区分依据，就可能错误地使用 TA A 的物理地址转换结果执行 TA B 的访问。

ASID 允许 CPU 区分这两个地址空间。

```
TLB Entry A:
    VA   = 0x40000000
    ASID = 10
    PA   = 0x90010000

TLB Entry B:
    VA   = 0x40000000
    ASID = 11
    PA   = 0x90030000
```

在某些实现条件下，硬件可以同时缓存不同 ASID 的转换结果，避免每次切换 TA 都彻底清空 TLB。

但有一个重要前提：

ASID 必须仍然属于当前正确的地址空间生命周期，不能错误地复用旧 ASID 对应的转换。

## 11.4.3 当前 OP-TEE 如何分配 ASID？

当前 RISC-V `core_mmu_arch.c` 维护了一个 ASID Bitmap。

实现中的关键定义：

```
#define RISCV_MMU_NUM_ASIDS U(256)

static bitstr_t bit_decl(
    g_asid, RISCV_MMU_NUM_ASIDS);
```

分配时，`asid_alloc()` 查找 Bitmap 中空闲的位置。

其简化逻辑为：

```
unsigned int asid_alloc(void)
{
    int i;

    lock(g_asid_spinlock);

    i = find_first_clear_bit(g_asid);

    if (i < 0) {
        unlock(g_asid_spinlock);
        return 0;
    }

    set_bit(g_asid, i);

    unlock(g_asid_spinlock);

    return i + 1;
}
```

这里的 `0` 用作未获得有效 User ASID 的返回值，分配给 TA Context 的编号从 1 开始。实际源码使用 `bit_ffc()`、`bit_set()` 和 `cpu_spin_lock_xsave()`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



注意，256 是这一实现的软件配置，不是 RV64 架构规定所有硬件必须支持 256 个 ASID。

RV64 的 ASID 字段在架构上是 16 位，但硬件可以实现更少的有效 ASID 位。

当前代码也专门讨论了实际 `ASIDLEN` 小于字段最大宽度的情况。

## 11.4.4 为什么 ASID 释放需要 TLB Invalidation？

这是一个非常重要的安全问题。

假设：

```
t0: TA A allocated ASID 10

t1: TA A maps
    VA 0x40000000 -> PA 0x90010000

t2: TA A exits

t3: ASID 10 is freed

t4: TA B receives ASID 10

t5: TA B maps
    VA 0x40000000 -> PA 0x90030000
```

如果在 `t3` 之前没有正确处理旧 TLB Entry，那么后续 ASID 复用可能导致错误的地址转换。

OP-TEE 在 `vm_info_final()` 中有明确的处理逻辑：

```
/* 取自实际实现的关键调用序列 */

pgt_flush(uctx);
tee_pager_rem_um_regions(uctx);

tlbi_asid(uctx->vm_info.asid);

asid_free(uctx->vm_info.asid);
uctx->vm_info.asid = 0;
```

这段代码非常值得研究。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



它体现的是：

1. 释放相关 Page Table 资源。
2. 清理 Pager 所管理的 User Regions。
3. 使该 ASID 对应的旧转换结果失效。
4. 最后才归还 ASID。

这里最重要的不是记住函数顺序，而是理解其安全不变量：

> 一个 ASID 重新分配给新的地址空间之前，必须保证此前属于该 ASID、可能被再次使用的旧地址转换不会泄露到新的地址空间中。

在多核环境下，单纯使当前 hart 的 TLB 失效是不够的。

还要保证其他可能缓存相同 ASID 转换的 hart 也完成了适当的同步。

## 11.4.5 Global Mapping 为什么需要单独考虑？

RISC-V PTE 中的 `G` 位表示 Global Mapping。

对于这样的映射，地址转换不以某个特定 ASID 为身份。

这适用于多个地址空间都使用相同的共享 Kernel Mapping。

但会带来一个问题：

如果一个 Global Mapping 被修改，只执行针对某个 ASID 的 TLB Invalidation，可能不足以清除该 Global Mapping 的旧转换。

因此通常需要区分：

```
# 当前 hart，刷新所有地址空间（包含全局映射）
sfence.vma x0, x0

# 当前 hart，针对指定 ASID
# a0 = ASID，排除 Global Translation
sfence.vma x0, a0

# 当前 hart，指定 VA，所有 ASID
sfence.vma a0, x0

# 当前 hart，指定 VA + ASID
sfence.vma a0, a1
```

后两种操作的具体作用范围，需要结合页表叶子层级以及 Global Mapping 的规则判断。

不能简单认为“我知道 ASID，所以所有修改都能使用 ASID-specific Flush”。

## 11.4.6 `satp` 的几个设计陷阱

陷阱一：认为修改 `satp` 就自动刷新了 TLB。

不正确。需要结合转换缓存、页表更新顺序及 `SFENCE.VMA` 规则处理。

陷阱二：认为 ASID 相同就一定代表相同地址空间。

不正确。ASID 存在复用生命周期。

陷阱三：认为执行一次 local `SFENCE.VMA` 就完成了 SMP TLB Shootdown。

不正确。它只对执行该指令的 hart 生效。

陷阱四：认为 TLB Flush 解决所有缓存一致性问题。

不正确。TLB 处理地址转换相关的缓存状态，普通数据缓存、DMA 可见性和设备一致性还需要另外分析。

# 11.5 User TA Address Space 的创建与销毁

本节要回答：

当 OP-TEE 加载一个新的 User TA 时，它如何从一个 ELF 文件构造出真正可执行、受到硬件保护的地址空间？

## 11.5.1 从 TA ELF 到运行时 Address Space

假设 Linux CA 调用：

```
TEEC_OpenSession(...);
```

如果对应的 User TA 尚未加载，OP-TEE 可能需要通过 TA Loader 获取并验证 TA 镜像。

这里涉及的执行链可以抽象成：

```
CA: TEEC_OpenSession()
        |
        v
Linux TEE Driver
        |
        v
OP-TEE Session Management
        |
        v
TA Loader / Signature Verification
        |
        v
Create TA Context
        |
        v
Initialize User VM
        |
        +-- Allocate ASID
        |
        +-- Allocate VA Regions
        |
        +-- Allocate Physical Memory
        |
        +-- Build Page Tables
        |
        +-- Map Code / ROData / Data
        |
        +-- Map Stack / Heap
        |
        +-- Apply Relocations
        |
        +-- Apply Runtime Permissions
        |
        v
TA Entry Point
```

具体的加载顺序和内存分配策略还受到 TA 类型、Loader、Pager 及构建配置影响。

但无论采用何种方式，都必须维护一个核心约束：

TA 最终能够访问哪些地址，必须由经过验证的 User VM Mapping 决定，而不是由 TA ELF 自己声明就能获得任意权限。

## 11.5.2 `struct user_mode_ctx` 与 `struct vm_info`

OP-TEE 通用 User Memory 管理以 `struct user_mode_ctx` 作为重要的上下文对象。

其中关联了：

- TA 的 Virtual Memory Information。
- ASID。
- Virtual Memory Region List。
- Page Table Cache。
- 对应的 Trusted Service Context。

可以概念性地表示为：

```
/* 教学模型，不是完整源码定义 */

struct user_mode_ctx {
    struct vm_info {
        uint32_t asid;
        struct vm_region_list regions;
    } vm_info;

    struct pgt_cache page_tables;

    struct ts_ctx *ts_ctx;
};
```

其中 `vm_info` 描述逻辑虚拟地址空间，而 Page Table Cache 提供将这些逻辑映射落实为实际硬件 PTE 的资源。

## 11.5.3 `vm_info_init()`：地址空间的初始化入口

在当前 upstream 的 `core/mm/vm.c` 中：

```
TEE_Result vm_info_init(struct user_mode_ctx *uctx,
                        struct ts_ctx *ts_ctx)
{
    TEE_Result res;
    uint32_t asid = asid_alloc();

    if (!asid)
        return TEE_ERROR_GENERIC;

    memset(uctx, 0, sizeof(*uctx));

    TAILQ_INIT(&uctx->vm_info.regions);
    SLIST_INIT(&uctx->pgt_cache);

    uctx->vm_info.asid = asid;
    uctx->ts_ctx = ts_ctx;

    res = map_kinit(uctx);

    if (res)
        vm_info_final(uctx);

    return res;
}
```

以上保留了实际函数的主要结构，省略了日志等非核心语句。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



逐步分析。

第一步：分配 ASID

这一地址空间获得独立的 ASID 身份。

第二步：初始化 Virtual Memory Region List

初始时地址空间中还没有完整的 TA 代码、堆和栈映射。

第三步：初始化 Page Table Cache

为后续映射提供页表管理结构。

第四步：保存 Trusted Service Context

把 User VM 与上层 TA/Trusted Service 生命周期关联起来。

第五步：建立必要的 Kernel-Assisted Mapping

`map_kinit()` 建立 User Context 中需要的特定内核辅助映射。

这一点很有意思：即使是 User TA Address Space，也不代表里面只能出现 TA 自己的 ELF Section。

某些用于受控用户态交互的辅助映射可能同样存在。

## 11.5.4 `vm_region`：把虚拟空间与物理内存联系起来

我们可以把一个 `vm_region` 理解成以下映射描述：

```
Virtual Region
    |
    +-- VA Start
    |
    +-- Size
    |
    +-- Access Attributes
    |
    +-- Region Flags
    |
    +-- Memory Object (MOBJ)
    |
    +-- Offset in MOBJ
```

例如：

```
TA A Code Region

VA Start:  0x40000000
Size:      0x00020000
Permission: User RX
MOBJ:      Secure Code Memory
Offset:    0
```

这里的 `MOBJ` 非常重要。

因为一个 Virtual Memory Region 不应该直接假设自己的底层物理内存连续。

## 11.5.5 为什么 OP-TEE 要设计 `MOBJ`？

`MOBJ`（Memory Object）是 OP-TEE 内存管理抽象中的关键对象。

其设计目的是把：

- 某段内存是什么。
- 该内存如何定位物理页。
- 该内存如何获取 VA。
- 该内存如何被引用和释放。

与具体的 User Virtual Mapping 分开。

这样，VM Subsystem 就不需要为每一种内存来源重新实现一套 Page Table 操作。

举例：

```
                 VM Region
                     |
                     v
                    MOBJ
                     |
       +-------------+-------------+
       |             |             |
       v             v             v
   Physical      Noncontiguous    Paged
   Memory        Shared Memory    Memory
```

实际上，`mobj_phys` 等实现通过 `mobj_ops` 提供访问物理地址和相关属性的接口。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这种抽象带来一个明显的架构好处：

VA Region 负责“怎么映射”，MOBJ 负责“映射的内存对象是什么、在哪里”。

## 11.5.6 `set_um_region()`：将 VM Region 写入 Page Table

在 `core/mm/vm.c` 中，`set_um_region()` 将 User Region 对应的物理内存映射落实到页表中。

其中会使用：

- `mobj_get_pa()`：得到 Memory Object 对应的物理地址。
- `core_mmu_set_entry()`：写入页表项。
- `r->attr`：决定相应访问属性。

概念执行过程：

```
vm_region:
    VA = 0x40000000
    Size = 0x20000
    MOBJ = secure_object_A
         |
         v
    mobj_get_pa()
         |
         v
    Physical Pages
         |
         v
    core_mmu_set_entry()
         |
         v
    RISC-V PTEs
```

这解释了为什么 `vm_region` 本身并不等同于一个硬件 PTE：一个 Region 可以覆盖多个 Page，并且在 Pager 配置下还可能按需建立具体 PTE。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 11.5.7 TA 加载时为什么需要区分临时权限和最终权限？

考虑一个 TA ELF 的 `.text` Section。

最终权限应该是 RX，而不是 RWX。

但在加载或 Relocation 阶段，Loader 可能需要修改被加载的内容。

因此合理的设计是：

```
Loading Phase
    |
    | Allocate Memory
    | Copy ELF Segments
    | Apply Relocations
    v
Finalize Permissions
    |
    +-- .text   -> RX
    +-- .rodata -> R
    +-- .data   -> RW
    +-- stack   -> RW
    |
    v
Execute TA
```

这里的关键安全原则是 W^X（Writable XOR Executable）。

即不要长期让不可信代码可以修改同时又能够执行的页面。

不过，不同平台或加载方式的中间阶段可能采用不同权限策略，不能简单假设现有代码在任意瞬间都满足最严格的 W^X。

Staff Engineer 在 Review 这里的代码时，需要检查：

- RWX Window 是否存在？
- 临时可写映射是否在进入 TA 前撤销？
- 撤销权限后是否执行适当的 TLB Synchronization？
- 是否存在同一 PA 的 Writable Alias 和 Executable Alias？
- SMP 下其他 hart 是否仍保留旧权限？

最后一点尤其容易被遗漏。

## 11.5.8 TA 生命周期结束时需要清理什么？

一个 User TA Context 的销毁不仅是释放 Heap。

还要考虑：

1. 撤销 User VM Region。
2. 解除具体 PTE 映射。
3. 处理已缓存的 Page Table。
4. 清理相关 TLB Translation。
5. 释放 ASID。
6. 解除 Memory Object 引用。
7. 释放私有物理内存。
8. 确保敏感数据不会被下一使用者读取。

其中“清理地址转换”与“擦除敏感数据”是两个不同的要求。

例如，TA A 的 Heap 被释放后重新分配给 TA B：

即使 TA B 的 Page Table 完全正确，如果物理页中仍包含 TA A 的密钥数据，TA B 仍可能读到敏感信息。

所以 Page Table Isolation 不等于 Memory Sanitization。

可靠的安全内存分配策略还必须考虑初始清零、释放擦除、缓存状态以及物理页复用。

# 11.6 TA A 与 TA B 的页表隔离

这一节是理解 OP-TEE User TA 安全性的核心。

## 11.6.1 不同 TA 可以使用相同的虚拟地址

假设：

```
TA A Address Space

VA                 PA
0x40000000  --->   0x90010000
0x40001000  --->   0x90011000
0x40002000  --->   0x90012000
```

而 TA B：

```
TA B Address Space

VA                 PA
0x40000000  --->   0x90030000
0x40001000  --->   0x90031000
0x40002000  --->   0x90032000
```

这里的 VA 完全相同。

但它们对应完全不同的物理内存。

在 OP-TEE 中，TA 的地址空间由对应 User Context 管理，当前运行时再把相应的映射应用到 hart。

因此：

```
            Hart 0

        OP-TEE Core
             |
             v
      Active User Map
             |
      +------+------+
      |             |
   TA A PGT       TA B PGT
   (active)      (inactive)
```

切换后，当前 hart 可以使用 TA B 的 User Mapping，而不应该继续使用 TA A 的旧 User Mapping。

## 11.6.2 为什么不能只依赖 ASID？

ASID 的功能主要是区分地址转换缓存的所属空间。

但如果两套 Page Table 中错误地写入了相同的私有 PA：

```
TA A:
    VA_A -> PA_SECRET

TA B:
    VA_B -> PA_SECRET
```

即使它们的 ASID 不同，TA B 也仍可以通过合法 PTE 读取 `PA_SECRET`。

所以真正的 TA Isolation 需要共同满足：

- Page Table 中只包含该 TA 获准访问的 Private Pages。
- User / Supervisor Permission 正确。
- ASID 生命周期管理正确。
- TLB 不保留跨 TA 的旧映射。
- 物理页分配和回收不会泄露数据。
- Shared Mapping 必须经过明确授权。

因此，ASID 是实现地址空间切换效率与正确性的辅助机制，而不是授权策略本身。

## 11.6.3 是否可以让两个 TA 共享一块 Secure Memory？

可以，但必须有受控的共享语义。

例如某个 TA Framework 支持共享只读代码或某类明确授权的数据对象。

关键不是“物理内存共享一定不安全”，而是：

共享必须有清晰的对象所有权、访问权限、生命周期和同步协议。

如果两个不可信 TA 都能读写同一块物理内存，则需要解决：

- 谁负责分配和释放？
- 其中是否包含任何一方的敏感数据？
- 一方结束后另一方是否还能访问？
- 同一 PA 是否被多个不同 Cache Attribute 映射？
- 映射撤销如何同步？
- 是否存在 TOCTOU Race？

这些问题往往比简单的 Page Table Mapping 更复杂。

## 11.6.4 三种隔离失败的典型场景

场景 A：Page Table Alias 泄露

TA B 的页表错误映射到 TA A 的私有物理页。

即使 `satp.ASID` 正确，也无法阻止越权访问。

场景 B：Stale TLB Translation

TA A 的页面已被取消映射，某个 hart 却仍能使用缓存中的旧翻译。

此时软件视角已经撤销权限，硬件执行视角却可能尚未同步。

场景 C：Physical Page Reuse

TA A 的 Heap 释放后，物理页被分配给 TA B，但敏感数据没有清除。

即使所有 PTE 都正确，仍可能造成信息泄露。

这些场景说明，OP-TEE 的内存隔离不是单靠某个硬件功能实现的，而是跨越 VM、Memory Allocator、Page Table、TLB 和 Thread 生命周期的整体安全属性。

# 11.7 OP-TEE Core 与 TA 之间的 U/S Permission Isolation

## 11.7.1 U-mode TA 为什么不能直接访问 Core？

在 RISC-V 中，OP-TEE Core 通常运行在 S-mode，而 User TA 运行在 U-mode。

如果 Core Page Table Mapping 设置为：

```
PTE_V = 1
PTE_R = 1
PTE_W = 1
PTE_X = 0
PTE_U = 0
```

那么 U-mode 不能直接访问该页面。

不论 TA 是正常代码、恶意代码，还是因为内存破坏跳转到了 Core Data VA，硬件都应该阻止不符合权限的访问。

这是最基本的 Privilege Isolation。

## 11.7.2 运行一次非法访问会发生什么？

假设 TA A 中存在：

```
volatile uint64_t *ptr =
    (volatile uint64_t *)0xffff000000001000ULL;

uint64_t value = *ptr;
```

这里的地址仅作错误访问示例。

如果该 VA 属于不允许 User 读取的 Core Mapping，或没有合法映射，CPU 会产生相应的异常。

典型 RISC-V Page Fault Cause：

| `scause` Exception Code | 说明                     |
| ----------------------- | ---------------------- |
| 12                      | Instruction Page Fault |
| 13                      | Load Page Fault        |
| 15                      | Store/AMO Page Fault   |

需要和：

| `scause` Exception Code | 说明                       |
| ----------------------- | ------------------------ |
| 1                       | Instruction Access Fault |
| 5                       | Load Access Fault        |
| 7                       | Store/AMO Access Fault   |

加以区分。

Page Fault 通常指向虚拟地址转换或页权限相关问题；Access Fault 还可能涉及 PMP、PMA、总线或其他物理访问约束。

实际硬件错误的异常归类需要依照架构规范、具体访问过程及平台实现判断。

OP-TEE 异常处理逻辑可以从：

`core/arch/riscv/kernel/abort.c`

入手。

当前实现中会记录 Cause、EPC、Fault VA 和 Status，并区分用户态异常与可能需要 Panic 的 Core Fault。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 11.7.3 `stval` 为什么特别重要？

当发生 Fault 时：

- `sepc`：保存触发异常的指令位置。
- `scause`：说明异常原因。
- `stval`：对于相关地址异常，通常包含导致异常的虚拟地址。
- `sstatus.SPP`：可以辅助判断 Trap 前的 Privilege Mode。

例如：

```
scause = 13
sepc   = 0x40001234
stval  = 0xffff000000001000
```

可能意味着 TA 在 `0x40001234` 这条 Load 指令附近访问了非法目标地址。

注意，`stval` 的具体有效信息受到异常类型和实现规则约束，不能不加判断地将其解释为始终有效的 Fault VA。

对于 OP-TEE 内核开发，重要的不只是能够打印这些寄存器，而是能够将它们与当前 User VM Region 和 Page Table 内容关联。

## 11.7.4 `SUM=1` 为什么可能引入攻击面？

考虑一个 TA 传入指针：

```
TEE_Result handle_user_buffer(void *user_ptr, size_t len);
```

OP-TEE Core 需要访问这块 TA 内存。

如果实现直接、长期打开 `sstatus.SUM`，那么 S-mode 的某些 Load/Store 就可能访问 User Pages。

这会扩大 Kernel 误访问 User Pointer 的风险。

例如：

- Kernel 中存在未经验证的用户指针。
- Kernel 中存在指针越界。
- Kernel 发生控制流破坏。
- User Page 与 Kernel Page 存在错误 Alias。

因此，安全的 User Memory Access 需要具备：

1. 地址范围检查。
2. 长度溢出检查。
3. Read / Write Permission 检查。
4. 合法 Context 检查。
5. 受控的数据访问机制。
6. 正确处理访问期间的 Fault。
7. 避免在不合适的异常、重入或并发路径中留下扩大权限的状态。

尤其需要理解：

TA 指针是一个不可信输入。

即使 TA 执行在受 OP-TEE 管理的 U-mode，也不意味着 TA 传入的每一个指针都可以在 S-mode 中直接解引用。

## 11.7.5 为什么 Kernel Pointer 也不能完全信任？

假设 OP-TEE 的某个系统调用处理函数接收 TA 传入的地址，然后将其转换成 Kernel VA。

如果没有验证该地址属于正确的 User Mapping，可能出现：

- TA Pointer 指向 Core Memory。
- TA Pointer 指向其他 TA Memory。
- 整数溢出导致长度检查绕过。
- 地址转换期间映射被撤销。
- User Buffer 与共享内存对象不匹配。

对于安全敏感的 Kernel API，必须同时验证地址的形式、映射、权限、对象身份以及生命周期。

这和 Linux Kernel 的 `copy_from_user()` 安全设计有相似之处，但 OP-TEE 的威胁模型更加敏感。

# 11.8 Shared Memory Mapping 与非可信内存验证

这是 OP-TEE 安全审计中极有价值的一节，因为 Shared Memory 是 Linux 与 OP-TEE 之间最直接的数据交换边界。

## 11.8.1 为什么需要 Shared Memory？

假设 CA 调用：

```
TEEC_InvokeCommand(
    &session,
    CMD_ENCRYPT,
    &operation,
    &origin);
```

它需要向 TA 传入一个 1 MiB Buffer。

如果完全依赖 RISC-V 寄存器传参，是不现实的。

因此需要一块可由 REE/TEE 双方访问的数据内存。

```
Linux CA
    |
    | Buffer
    v
Shared Memory
    |
    | Explicit Mapping
    v
OP-TEE Core
    |
    | User Parameter Mapping
    v
User TA
```

这里最重要的事实是：

Shared Memory 是通信载体，不是可信存储。

Linux 可以控制该内存的内容，也可能在 TA 正在访问它时修改数据。

## 11.8.2 Static Shared Memory 与 Dynamic Shared Memory

### Static Shared Memory

平台预留一块物理连续的 RAM：

```
Physical DRAM

0x88000000
+-----------------------+
| Static Shared Memory  |
|                       |
| 8 MiB Reserved        |
+-----------------------+
0x88800000
```

这块内存被配置为 REE/TEE 可共享。

优点是结构简单，地址范围容易管理。

缺点是容量固定，可能造成空间浪费，也不适合所有动态零拷贝使用场景。

### Dynamic Shared Memory

Linux 可以将自身管理的、符合条件的物理页面注册为 Shared Memory。

例如：

```
Linux Buffer (Virtual)
    |
    +-- VA page 0 -> PA 0x81002000
    |
    +-- VA page 1 -> PA 0x85004000
    |
    +-- VA page 2 -> PA 0x82009000
```

这些 PA 可以不连续。

OP-TEE 通过共享内存描述获取页面列表，并建立自己的映射。

OP-TEE 的通用实现支持这两种模式；当前 RISC-V `plat-virt/conf.mk` 明确配置了 `CFG_CORE_RESERVED_SHM=n`、`CFG_CORE_DYN_SHM=y`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## 11.8.3 Dynamic Shared Memory 的完整过程

以传统 OP-TEE Message ABI 的动态共享内存为例：

```
Linux
    |
    | Register Shared Buffer
    v
OP-TEE Driver
    |
    | Build Noncontiguous Page List
    |
    | OPTEE_MSG_CMD_REGISTER_SHM
    v
OP-TEE Core
    |
    | Validate Physical Ranges
    |
    | Create MOBJ
    |
    | Associate Cookie / Handle
    v
Registered Shared Memory
    |
    | TEEC_InvokeCommand()
    v
TA Parameter Mapping
    |
    v
TA Accesses Shared Buffer
```

在当前 RISC-V RPMI 传输方案中，还需要根据所选版本的通信协议处理 Shared Memory 的表示和生命周期。不能将 Arm SMC Transport 的所有细节直接视为 RISC-V RPMI 的固定 ABI。

## 11.8.4 为什么要验证 Shared Memory 的 PA Range？

假设 Linux 构造如下 Page List：

```
Page 0: 0x81002000  <- Valid REE DRAM
Page 1: 0x81003000  <- Valid REE DRAM
Page 2: 0x90010000  <- OP-TEE Private Memory
```

如果 OP-TEE 不验证 Page List，就可能错误地将自己的 Private Memory 当成来自 Linux 的 Shared Memory。

即使 PMP 本身能够阻止 Linux 直接读取 TEE Memory，错误注册物理页仍可能导致：

- 错误的内存对象归属。
- Privileged Code 错误读写 Secure Data。
- Internal Buffer 被导出。
- 权限混淆或数据破坏。
- 同一物理页的错误 Alias。

因此必须验证：

- 所有页面是否属于允许共享的物理区域。
- 是否与 Secure RAM 或保留 MMIO 交叠。
- Range 和 Size 运算是否溢出。
- 所有描述的 Granularity 是否合法。
- 非连续页面列表是否有效。
- 注册后的对象生命周期是否受到正确管理。

OP-TEE 的通用内存代码通过内存分类及缓冲区区域检查等接口实现这类保护。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## 11.8.5 为什么 Shared Memory 容易出现 TOCTOU？

TOCTOU 是 Time Of Check To Time Of Use。

考虑：

```
Time T0:
    Linux writes shared_buffer.length = 128

Time T1:
    OP-TEE validates length <= 256

Time T2:
    Linux changes length = 4096

Time T3:
    OP-TEE uses shared_buffer.length
```

如果 OP-TEE 的使用阶段重新读取不可信字段，就可能造成越界访问。

即使最初的 Validation 完全正确，也没有解决问题。

这就是典型的 TOCTOU。

## 11.8.6 正确的解决思路

对于需要在可信逻辑中保持一致的描述符，应将其复制到受保护的 Memory 中，再进行验证和使用。

例如：

```
struct request {
    uint32_t offset;
    uint32_t size;
};

TEE_Result process_request(
    const struct request *shared_req,
    size_t buffer_size)
{
    struct request req;

    /*
     * 示例前提：
     * shared_req 已经过合法映射和基本范围验证。
     */
    memcpy(&req, shared_req, sizeof(req));

    /*
     * 后续只使用可信本地快照。
     */
    if (req.offset > buffer_size)
        return TEE_ERROR_BAD_PARAMETERS;

    if (req.size > buffer_size - req.offset)
        return TEE_ERROR_BAD_PARAMETERS;

    return process_validated_request(
        req.offset, req.size);
}
```

这段代码说明的是结构体快照和溢出安全检查的思想，并非完整的 OP-TEE 安全 Buffer Access API。

实际代码还需要确保 `shared_req` 的映射和长度在复制期间合法，并处理访问错误。

为什么这里使用减法而不是简单比较 `offset + size`？

因为：

```
offset + size
```

本身可能发生整数溢出。

例如：

```
offset = 0xfffffff0
size   = 0x100
```

32 位无符号加法可能发生回绕，从而使错误范围通过检查。

这类问题非常值得在真实 OP-TEE PR Review 中关注。

## 11.8.7 Cache Coherency 也是 Shared Memory 的一部分

假设 Linux 和 OP-TEE 共享一个 Cacheable Buffer。

需要确认：

- 两个执行 Domain 是否处于硬件 Cache Coherent Domain。
- 不同 Alias 是否使用兼容的 Memory Attributes。
- CPU 与 DMA Device 之间是否具备一致性。
- IPC 协议如何保证生产者写入发生在消费者读取之前。
- 如果硬件非一致性，谁负责 Cache Clean / Invalidate？

特别是含有独立 HSM Core 的 SoC：

```
Linux CPU
    |
    v
Shared DRAM
    ^
    |
HSM Core
```

即使 Linux CPU 内部的多个 hart 之间保持 Cache Coherency，也不意味着独立 HSM Core 与应用处理器 Cache 之间自动保持一致。

因此必须区分：

CPU SMP Coherency、跨处理器 IPC Coherency、DMA Coherency 是三个不同的工程问题。

# 11.9 `SFENCE.VMA` 与 SMP TLB Shootdown

本章最重要的源码专题

这一节我建议深入到可以独立修改 upstream 的程度。

因为它将 RISC-V ISA、OpenSBI、OP-TEE MMU、SMP 和 Security Isolation 全部关联起来。

## 11.9.1 先看一个真实的工业问题

假设一个 SoC 内部有四个 RISC-V hart，OP-TEE 可以使用其中两个 hart。

```
Hart 0:
    Running TA A

Hart 1:
    Running TA A
```

TA A 的某个页：

```
VA = 0x40000000

Old Mapping:
    PA = 0x90010000
    Permission = RW
```

现在 Hart 0 由于某个内存管理操作，需要将其更改为只读：

```
New Mapping:
    PA = 0x90010000
    Permission = R
```

在 Hart 0 上修改了 PTE：

```
pte->flags &= ~PTE_W;
```

然后执行：

```
sfence.vma x0, x0
```

是否就完成了整个系统的权限修改？

没有。

因为 Hart 1 可能仍然缓存了旧的 RW Translation。

这意味着软件已经认为这个 Page 是 Read-only，但 Hart 1 可能仍在基于旧缓存进行写入。

这就是典型的 Cross-Hart TLB Synchronization 问题。

## 11.9.2 `SFENCE.VMA` 只作用于当前 Hart

在 RISC-V 中：

```
sfence.vma rs1, rs2
```

通过 `rs1` 和 `rs2` 选择地址范围与 ASID 范围，对执行该指令的 hart 上相关地址转换进行同步。

注意这里的核心限制：

它不是一个直接广播到所有 hart 的硬件 TLB Shootdown 指令。

例如：

```
Hart 0:
    Update PTE
    SFENCE.VMA
         |
         +---- Hart 0 synchronized

Hart 1:
    Still has stale translation
```

因此，在 SMP 系统中需要额外协调其他 hart。

## 11.9.3 OpenSBI RFENCE 的作用

RISC-V SBI RFENCE Extension 提供了跨 hart 的 Remote Fence 服务。

例如：

- `sbi_remote_sfence_vma()`
- `sbi_remote_sfence_vma_asid()`

其逻辑可以表示为：

```
OP-TEE Hart 0
    |
    | Update Page Table
    |
    | Synchronize PTE Writes
    |
    | SBI Remote Fence Request
    v
OpenSBI M-mode
    |
    | Coordinate Target Harts
    |
    +--------------------+
    |                    |
    v                    v
Hart 0               Hart 1
Local Fence          Remote Fence
    |                    |
    +---------+----------+
              |
              v
       Fence Completion
              |
              v
       Continue Execution
```

在真实实现中，OpenSBI 负责协调目标 hart 执行相应的同步操作，并提供请求完成语义。

这里有个非常重要的权限设计：

OP-TEE 不应该无条件获得控制整个 SoC 所有 hart 的能力。

在基于 Domain Isolation 的系统中，Remote Fence 的可见 Hart 集合需要与 Domain 权限模型协调。

## 11.9.4 直接阅读 RISC-V OP-TEE 的 `tlb_helpers_rv.S`

当前实现的本地接口非常简洁：

```
/* Local TLBI helpers */

FUNC tlbi_all_local , :
    sfence.vma zero, zero
    ret
END_FUNC tlbi_all_local

FUNC tlbi_va_allasid_local , :
    sfence.vma a0, zero
    ret
END_FUNC tlbi_va_allasid_local

FUNC tlbi_asid_local , :
    sfence.vma zero, a0
    ret
END_FUNC tlbi_asid_local

FUNC tlbi_va_asid_local , :
    sfence.vma a0, a1
    ret
END_FUNC tlbi_va_asid_local
```

这些函数与当前 RISC-V upstream 源码对应。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



其设计思想是：

- 汇编层提供最小化的 local architecture primitives。
- C 层负责选择同步范围。
- Cross-Hart Synchronization 由 SBI RFENCE 协助完成。

这样将 ISA Primitive 与 Platform Coordination 分开，是很合理的系统架构设计。

## 11.9.5 RISC-V OP-TEE 如何实现 Remote TLB Invalidation？

在 `core_mmu_arch.c` 中，可以看到一个 `tlbi_remote()` 实现。

以下保留关键逻辑：

```
static void tlbi_remote(vaddr_t va,
                        size_t len,
                        unsigned long asid,
                        bool with_asid)
{
#ifdef CFG_RISCV_SBI
    int rc = SBI_SUCCESS;

    if (CFG_TEE_CORE_NB_CORE == 1)
        return;

    if (with_asid)
        rc = sbi_remote_sfence_vma_asid(
            0,
            SBI_HART_MASK_BASE_ALL,
            va,
            len,
            asid);
    else
        rc = sbi_remote_sfence_vma(
            0,
            SBI_HART_MASK_BASE_ALL,
            va,
            len);

    if (rc)
        panic();
#endif
}
```

这是基于 upstream 源码的关键部分摘录。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里有四个重要细节。

第一：只有多核配置才需要执行这一路径中的 Remote Fence。

```
if (CFG_TEE_CORE_NB_CORE == 1)
    return;
```

这是当前实现的配置优化，而不是意味着单 hart 系统不需要任何 TLB Synchronization。

第二：根据是否指定 ASID，选择不同的 RFENCE 服务。

有时需要刷新某个 ASID 的转换，有时必须覆盖所有 ASID。

第三：`SBI_HART_MASK_BASE_ALL` 表示面向 SBI 可见的目标 Hart 集合。

在当前源码设计中，Domain-isolated OP-TEE 假设 SBI 对其暴露的 Hart 集合正是它获准使用的 Domain Hart 集合。

实际 SoC Porting 时，这个假设必须得到验证。

第四：Remote Fence 失败时不能静默继续。

当前代码会记录错误并 Panic。

原因是：如果权限撤销或地址转换同步不能完成，继续运行可能使安全不变量无法保证。

## 11.9.6 `tlbi_va_range_asid()` 的完整逻辑

这是一个值得单独精读的函数。

它的简化逻辑：

```
void tlbi_va_range_asid(vaddr_t va,
                        size_t len,
                        size_t granule,
                        uint32_t asid)
{
    /* 1. 确保之前的相关写入具有所需顺序 */
    mb();

    /* 2. Local Hart Fence */
    while (len) {
        tlbi_va_asid_local(va, asid);

        va += granule;
        len -= granule;
    }

    /* 3. Remote Hart Fence */
    tlbi_remote(original_va,
                original_len,
                asid,
                true);

    /* 4. 进一步的顺序约束 */
    mb();
}
```

代码是为解释执行思想而简化的；当前实现确实具有 Memory Barrier、Local Fence、一次范围型 Remote Fence 及后续 Barrier。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这四个步骤分别解决不同问题。

| 步骤                   | 目的              |
| -------------------- | --------------- |
| PTE Write Ordering   | 确保页表修改按要求可见     |
| Local `SFENCE.VMA`   | 同步当前 hart 的地址转换 |
| SBI Remote Fence     | 同步其他目标 hart     |
| Completion / Barrier | 确保后续访问满足必要的同步关系 |

## 11.9.7 为什么 PTE Write 和 TLB Fence 之间还需要顺序约束？

考虑：

```
Hart 0:
    Store new PTE
    Send remote fence

Hart 1:
    Execute SFENCE.VMA
    Perform page walk
```

如果 Hart 1 执行 Page Walk 时还无法按要求观察到新的 PTE，就可能继续获取旧的地址转换。

因此安全的设计不能只问：

“其他 hart 有没有执行 `SFENCE.VMA`？”

还要问：

“在它执行同步并重新翻译时，是否能够看到预期的新页表内容？”

这涉及 RVWMO、Memory Ordering、Cache Coherency 和 RFENCE 的接口语义。

在标准 RISC-V 一致性平台上，可以利用软件 Memory Barrier 和 SBI Remote Fence 的规定行为协调。

但在真实 SoC 上，还应验证：

- Page Table Memory 是否处于正确的一致性域。
- 是否存在非一致性硬件组件。
- 是否有错误的 Cache Attribute。
- SBI RFENCE 是否真的覆盖所有相关 hart。
- 平台固件是否正确实现了同步完成语义。

## 11.9.8 Page Table Permission Update 的安全顺序

假设要撤销某个页面的 Write Permission。

一个正确的高层操作流程应包括：

```
Acquire Mapping Lock
        |
        v
Validate Current Mapping
        |
        v
Update PTE Permissions
        |
        v
Ensure PTE Update Visibility
        |
        v
Local Translation Synchronization
        |
        v
Remote Translation Synchronization
        |
        v
Confirm Completion
        |
        v
Release / Reuse Resources if Allowed
        |
        v
Release Lock
```

这里还有一个经常被忽视的问题：

TLB Shootdown 完成，不代表操作自动具备对所有并发 User Access 的线性化语义。

必须同时设计 Mapping Lock、TA Execution Context、Page Table Lifetime 和访问撤销协议。

例如，如果另一个 hart 已经开始执行某个访问，必须依照架构和同步协议确定它何时可以完成，以及在什么时候允许释放底层物理页。

## 11.9.9 为什么不能直接 Free 旧物理页？

假设：

```
Hart 0:
    Remove PTE
    Free Physical Page

Hart 1:
    Uses old cached VA -> PA
```

Hart 1 可能继续访问已经归还给 Memory Allocator 的物理页。

如果这一物理页立即分配给 TA B，就可能出现：

```
Old Owner: TA A
New Owner: TA B

Hart 1 still accesses old PA
```

这是典型的 Use-After-Free / Cross-Context Memory Corruption。

正确的安全生命周期必须保证：

旧映射被所有相关执行主体有效撤销之后，才能按协议复用对应物理资源。

这也是 TLB Shootdown 和内存释放生命周期必须放在一起分析的原因。

## 11.9.10 为什么 QEMU 单核测试通常发现不了这种问题？

因为：

```
CFG_TEE_CORE_NB_CORE = 1
```

时，真正的 Remote Fence 分支可能不会执行。

即使编译完全成功、TA 能运行、基本 xtest 通过，也无法证明 SMP TLB Shootdown 的正确性。

当前 RISC-V `plat-virt` 配置中，`CFG_TEE_CORE_NB_CORE` 和 `CFG_NUM_THREADS` 都默认设置为 1，因而多核验证必须显式配置并提供匹配的 hart 与 Domain 运行环境。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



对 Staff Engineer 来说，测试应至少覆盖：

- 两个 hart 并发运行。
- 其中一个 hart 修改共享映射。
- 另一个 hart 已经缓存旧转换。
- 改变权限和删除映射两种情况。
- ASID Reuse。
- 高频 TA Switch。
- 多次远程同步下的竞争和失败路径。

这类测试具有真实的 upstream 贡献价值。

# 11.10 Pager、ASLR、Memory Protection 与 Security Hardening

这一节讨论三个经常一起出现、但本质不同的机制：

- Pager：在物理内存资源有限时管理页面的驻留与换入换出。
- ASLR：随机化代码或数据在虚拟地址空间中的位置。
- Memory Protection / Hardening：限制内存访问行为并提高漏洞利用难度。

## 11.10.1 Pager 为什么对 OP-TEE 特别有意义？

普通 Linux 使用 Demand Paging，主要是为了提高物理内存使用效率并支持大规模虚拟内存。

OP-TEE 使用 Pager 的一个重要背景是：

某些安全平台只有少量受到强硬件保护的 SRAM。

例如：

```
Protected SRAM:
    256 KiB

OP-TEE Core:
    600 KiB

TA Memory:
    Additional Secure RAM Required
```

如果所有 Core Code 和 TA Data 都必须常驻在有限的受保护 SRAM 中，就可能无法运行。

Pager 允许部分内存按需进入受保护的执行内存。

其一般逻辑：

```
Execution accesses VA
        |
        v
Page Fault
        |
        v
TEE Pager
        |
        +-- Locate Backing Object
        |
        +-- Allocate / Reuse Protected Page
        |
        +-- Restore Page Contents
        |
        +-- Verify Integrity
        |
        +-- Update PTE
        |
        +-- Synchronize Address Translation
        |
        v
Resume Execution
```

在这种设计中，Pager 不只是性能组件，也是安全机制。

因为 Backing Store 可能位于不可信的 REE DRAM 中。

## 11.10.2 为什么不能直接信任 Backing Store？

假设 OP-TEE Core 的某个 Code Page 被换出到 REE DRAM。

Linux 可以读取或修改这块 DRAM。

如果 Pager 重新加载 Code Page 时不验证完整性，Linux 就可能替换将要执行的代码。

因此 Pager 可能需要：

- 对只读代码页面进行完整性校验。
- 对包含敏感数据的可写页面进行加密和认证。
- 对 Page Metadata、Nonce、IV 和 Tag 进行安全管理。
- 防止页面内容和页面身份被错误组合。
- 保护已经换出的敏感数据不被 REE 直接读取。

例如：

```
Secure Page
    |
    | Encrypt + Authenticate
    v
Untrusted Backing Store
    |
    | Load
    v
Verify Authentication Tag
    |
    | Success
    v
Restore Secure Page
```

如果使用 AEAD，通常需要将 Page Identity、Offset 或其他上下文作为认证约束，避免不同页面之间的错误替换。

还需要注意：

加密与完整性认证本身不自动解决所有 Rollback Attack。

如果攻击者能够回放旧版本的密文与对应的合法认证 Tag，则还需要其他可信版本状态或协议机制才能识别回滚。

## 11.10.3 OP-TEE Core Paging 与 User TA Paging 的区别

| 维度   | Core Paging         | User TA Paging       |
| ---- | ------------------- | -------------------- |
| 主要对象 | Core 可分页代码和数据       | TA 代码、数据、堆等          |
| 地址空间 | OP-TEE Core Mapping | 每个 TA 的 User Mapping |
| 权限   | Supervisor          | User                 |
| 主要目标 | 降低受保护内存常驻需求         | 提高 TA 内存利用率          |
| 安全要求 | Core 完整性及状态保护       | TA 间隔离、数据保密和完整性      |
| 页表管理 | Core 相关映射           | User Context 动态页表    |

OP-TEE 官方架构文档对 Core Paging、User TA Paging、Page Table Cache 和 Backing Store 均有详细解释。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### 重要：当前 RISC-V 实现与 Arm 的区别

这里必须澄清前文源码目录中的一个细节。

OP-TEE 的成熟 Pager 实现主要位于：

`core/arch/arm/mm/tee_pager.c`

而不是前面概念目录中列出的 `core/mm/tee_pager.c`。

我核查了当前 RISC-V `core/arch/riscv/mm/sub.mk`，其构建入口包含 `core_mmu_arch.c` 和 `tlb_helpers_rv.S`，并没有对应的 RISC-V Pager 实现文件。因此，不能将 Arm Pager 的完整功能直接视为 RISC-V upstream 已经具备的能力。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



后续对 RISC-V Pager 的研究应该区分：

1. 通用 OP-TEE VM/MOBJ/Pager 接口。
2. Arm 平台已经成熟的 Pager 实现。
3. RISC-V 当前已支持的内存管理功能。
4. 如果需要在 RISC-V 上实现 Pager，还缺少哪些架构相关机制和验证。

这本身就是一个值得深入评估的 upstream 研究方向。

## 11.10.4 RISC-V Pager Porting 需要解决哪些问题？

假设我们要在 RV64 平台支持 User TA Demand Paging。

至少需要处理以下关键问题。

第一：RISC-V Page Fault Classification

区分：

- Page Not Present。
- Permission Fault。
- Invalid User Access。
- 可恢复的 Pager Fault。
- 不可恢复的 Kernel Fault。

不能把所有 `scause=13` 或 `scause=15` 都当成可以自动分配页面的 Fault。

第二：Page Fault Context

Pager 需要知道：

- Fault VA。
- Faulting PC。
- 当前 TA Context。
- 访问类型。
- 当前 ASID。
- 当前 hart。
- 是否处于允许触发 Pager 的执行上下文。

第三：Page Table Manipulation

需要建立或恢复对应 PTE，并确保：

- PTE Permissions 正确。
- 物理页内容正确。
- TLB/Address Translation 同步正确。

第四：Pager 本身不能依赖可分页区域

否则会产生类似下面的问题：

```
Page Fault
    |
    v
Pager Handler
    |
    | Access Pageable Memory
    v
Another Page Fault
    |
    v
Pager Handler ...
```

最终可能造成递归异常或死锁。

因此 Pager Critical Path、锁、必要栈和元数据需要具备明确的常驻内存保障。

第五：并发安全

如果两个 hart 同时对同一 TA 的同一个 Page 触发 Fault：

```
Hart 0: Page Fault at VA X
Hart 1: Page Fault at VA X
```

就必须避免：

- 重复分配不同物理页。
- 两次 Page Table 更新相互覆盖。
- 一个 hart 使用未初始化页面。
- Page Cache 引用计数错误。
- 锁死或重复释放物理资源。

这些都是 Pager Porting 时需要考虑的架构问题。

## 11.10.5 ASLR 是怎样实现安全增强的？

ASLR 的目的不是改变访问权限，而是降低地址预测能力。

例如：

```
Without ASLR:

TA Code VA = 0x40000000
TA Heap VA = 0x50000000


With ASLR:

Run 1:
    TA Code VA = 0x42000000
    TA Heap VA = 0x68000000

Run 2:
    TA Code VA = 0x4a000000
    TA Heap VA = 0x72000000
```

这些地址仅为示意。

如果攻击者掌握某个内存破坏漏洞，固定的代码地址可能帮助其构造 ROP/JOP 等攻击。

ASLR 增加了地址的不确定性。

但它的有效性依赖：

- 随机种子质量。
- 随机化范围。
- 实际 Entropy。
- 地址信息是否泄露。
- TA 加载和地址空间复用策略。
- 是否存在可预测的固定映射。

对于 RISC-V OP-TEE，RP022 Milestone 3 已经推进 Core/TA ASLR 运行时验证，目的正是避免仅通过构建配置推断安全特性已经真正生效。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



## 11.10.6 W^X、NX、RO Data 与 PMP

我们已经讨论了 User TA 的 PTE Permission。

但是，OP-TEE Core 自身也需要尽量满足：

```
Core .text:
    Read + Execute

Core .rodata:
    Read Only

Core .data:
    Read + Write

Core Stack:
    Read + Write
    Non-executable
```

在 RISC-V 中，MMU 可以实现 S/U 地址空间中的页级访问控制。

同时，可以考虑通过 M-mode 管理的 PMP 进一步限制物理内存权限。

例如，将 Core `.text` 和 `.data` 分成不同物理权限区域。

目前 RP022 Milestone 8 的目标正是进一步强化 RISC-V OP-TEE Core 的物理权限，包括将 `.text` 与 `.data` 布局调整为适合 NAPOT 表示的区域，并通过 OpenSBI Domain 配置不同的权限。该工作截至当前公开计划仍在推进，不能视为所有平台的既有保障。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE+1



这里还有一个重要问题：

为什么 PMP 与 PTE Permission 可以同时存在？

因为二者解决不同层次的问题：

- PTE Permission：当前 S/U 地址空间允许怎样访问页面。
- PMP：当前执行上下文对特定物理地址范围的访问限制。

二者可以形成 Defense in Depth，但仍需要正确处理共享资源和平台硬件隔离配置。

## 11.10.7 Guard Page 与 Stack Canary

这两个机制经常被放在一起讨论，但作用不同。

Guard Page

在 Stack 边界设置不可访问页面。

例如：

```
+----------------------+
| TA Stack             |
| RW, U=1              |
+----------------------+
| Guard Page           |
| PTE Invalid          |
+----------------------+
| Other Region         |
+----------------------+
```

当连续 Stack Overflow 穿越边界时，可以触发 Fault。

Guard Page 不能检测所有非连续的越界写入，但对越界进入受保护空洞的访问十分有效。

Stack Canary

编译器在相关 Stack Frame 中放入保护值。

如果某次 Stack Overflow 修改了 Canary，在函数返回时可检测破坏。

Stack Canary 与 Guard Page 是不同层次的防护。

ASLR、W^X、Guard Page 和 Canary 相互补充，但都不能代替正确的内存隔离和边界验证。

# 11.11 将整章串联：一次 TA 切换到底涉及哪些内存管理操作？

我们现在把本章所有机制连接起来。

假设：

```
Hart 0:
    TA A 正在执行

Hart 1:
    OP-TEE 正在处理其他请求
```

Hart 0 准备从 TA A 返回 Core，然后执行 TA B。

可能涉及以下步骤。

1. TA A U-mode → OP-TEE S-mode

   发生 Syscall 或其他受控 Trap。CPU 保存异常状态，OP-TEE 恢复适当的 S-mode 执行上下文。
2. 保存 TA A Execution Context

   保存需要保留的 GPR、系统状态，以及按照配置管理的 FP/Vector State。
3. 解除当前 User Mapping

   当前 hart 不应该继续暴露与下一 TA 无关的 TA A User Mapping。
4. 同步相关地址转换

   根据实际修改范围执行本地 TLB Synchronization。若修改了共享页表，还需要相应的跨 hart 同步。
5. 准备 TA B User Context

   取得 TA B 的 VM Regions、Page Tables、ASID 和执行状态；必要时创建或恢复映射。
6. 切换当前 User Mapping

   更新当前 hart 的 User Mapping Slot 和相关 ASID。
7. 恢复 TA B Execution Context

   恢复寄存器和执行状态，并确保相关权限配置正确。
8. S-mode → U-mode

   使用 `sret` 等架构机制，开始或恢复 TA B 的执行。

这是一条概念执行路径，而不是固定的 OP-TEE 调度顺序。实际 Trusted Thread、TA Session 和 User Context 的生命周期并不意味着每次调用都要销毁并重建所有 TA Page Tables。

另外，这条执行链有一个很容易混淆的地方。

## 11.11.1 三种 Context Switch 不要混在一起

| 切换类型                         | 负责组件           | 是否改变安全 Domain | 主要管理对象               |
| ---------------------------- | -------------- | ------------- | -------------------- |
| Linux ↔ OP-TEE Domain        | OpenSBI / 隔离固件 | 是             | Domain、CSR、硬件隔离状态    |
| OP-TEE Trusted Thread Switch | OP-TEE Core    | 通常否           | Trusted Thread 上下文和栈 |
| TA A ↔ TA B User Context     | OP-TEE Core    | 通常否           | User VM、ASID、寄存器状态   |

这三种切换可能在一次完整的 TA Invoke/RPC 流程中相互嵌套，但绝对不是同一个操作。

尤其要记住：切换 TA 并不等于重新配置 OpenSBI Domain 的 PMP。

当两个 TA 都位于同一个 OP-TEE Domain 时，它们之间的隔离主要由 OP-TEE 内部的执行和内存管理机制负责。

# 11.12 工业项目中如何调试 OP-TEE MMU 问题？

这里给出一个适用于 RISC-V SoC Bring-up 的调试方法。

## 11.12.1 首先判断 Fault 属于哪一层

假设串口输出显示：

```
User TA abort
scause = 13
sepc   = 0x40002300
stval  = 0x60001000
```

不要立刻认为是 Page Table 错误。

应该分层排查。

第一层：软件内存对象

- Fault VA 是否属于某个合法 VM Region？
- 对应的 MOBJ 是否仍然存在？
- Region 是否已经被释放？
- 是否发生整数溢出或越界？

第二层：Page Table

- 预期 PTE 是否存在？
- `V/R/W/X/U` 是否正确？
- VA 是否满足 Sv39/Sv48 Canonical Address 规则？
- Page Table 层级、指针和 Leaf 是否正确？
- PTE 的 PA 是否属于正确的物理页？

第三层：运行状态

- 当前 `satp` 是否指向预期 Root Page Table？
- 当前 ASID 是否正确？
- User Mapping Slot 是否已经切换？
- `sstatus.SUM/MXR/SPP` 是否符合预期？

第四层：Hardware Isolation

- 目标 PA 是否在允许的 PMP Region？
- SoC Firewall 是否阻止了访问？
- 页表本身所在的物理内存是否可以被 Page Table Walker 访问？

第五层：SMP

- Fault 是否只在多核出现？
- 是否存在旧 TLB Entry？
- Remote Fence 是否被执行和确认？
- Page Table 是否被其他 hart 并发修改？
- 是否存在已释放页表或物理页被复用的问题？

这五层分析非常适合真实项目排障。

## 11.12.2 建议准备的 GDB 观察内容

在 OP-TEE 的相关断点处记录：

```
info registers
p/x $satp
p/x $sstatus
p/x $scause
p/x $sepc
p/x $stval
```

具体 GDB 对 CSR 的访问和名称支持与版本、target description 有关。如果 `$satp` 等寄存器不可直接访问，需要使用支持相应 CSR 的工具链、模拟器接口或临时诊断代码。

同时读取：

- 当前 hart ID。
- Trusted Thread ID。
- `vm_info.asid`。
- 当前 `vm_region`。
- `mobj`。
- 相关 PTE。
- `core_mmu_user_mapping_is_active()` 状态。

比单独分析 `scause` 更有效的方法，是把 Fault VA → VM Region → MOBJ → Physical Page → PTE → Current ASID 串成一条完整证据链。

## 11.12.3 推荐的四个源码实验

Lab 1 — TA Address Space Creation

在 `vm_info_init()`、`set_um_region()`、`core_mmu_create_user_map()` 处设置断点。

比较 TA 加载前后 ASID、VM Region 和 User Page Table 的变化。

验收：能画出一个真实 TA 的 VA → PTE → PA 映射结构。

Lab 2 — Illegal User Memory Access

在自己的测试 TA 中构造只读页面写入、无效地址访问及权限非法访问。

观察 `scause`、`sepc`、`stval`、PTE 和 TA 的异常处理结果。

验收：能解释每种 Fault 的硬件原因以及 OP-TEE 的处理决策。

Lab 3 — ASID Lifecycle

反复创建、销毁 TA Context，跟踪 ASID Allocation、Free 和 TLB Invalidation。

可以使用调试构建模拟较小的 ASID 资源池，以提高复用场景出现的概率，但必须与硬件 ASID 能力和测试机制相匹配。

验收：证明旧 ASID 映射不会被错误复用。

Lab 4 — SMP TLB Shootdown

在支持多 hart OP-TEE 的测试平台上，观察 `tlbi_remote()`、SBI RFENCE 和跨 hart 页表变更。

验收：能验证权限撤销后，目标 hart 不再使用旧映射。

可以先在源码仓库中执行以下检索，建立阅读路径：

```
cd optee_os

# User address-space lifecycle
git grep -n 'vm_info_init'
git grep -n 'vm_info_final'
git grep -n 'core_mmu_set_user_map'

# RISC-V address translation
git grep -n 'asid_alloc' -- core/arch/riscv
git grep -n 'tlbi_remote' -- core/arch/riscv
git grep -n 'sbi_remote_sfence_vma'

# Memory object and mapping
git grep -n 'mobj_get_pa'
git grep -n 'vm_map(' -- core
git grep -n 'TEE_MATTR_URW'
```

如果要构建完整的 RISC-V QEMU 运行环境，建议使用 [RISE RP016 Build Instructions](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/build/?utm_source=chatgpt.com) 的匹配版本组合，不要把不同开发分支的 OpenSBI、Linux、OP-TEE 和传输协议实现随意组合。

# 11.13 Chapter 11 知识体系总结

本章可以总结成以下六个核心对象及其关系。

OP-TEE Memory Protection Model

Security Domain

OpenSBI / PMP / Platform Isolation

OP-TEE Core Kernel

Core MMU / Memory Allocator / Trusted Thread

TA A Context

VM Regions / ASID A

TA B Context

VM Regions / ASID B

Page Tables & MOBJ

VA → PTE → PA · Permissions · Memory Lifetime

TLB / Memory Synchronization

Local SFENCE.VMA · Remote SBI RFENCE

其中最重要的六条结论：

1. PMP 与 S-mode MMU 解决不同的隔离问题。 Linux/TEE Domain 的隔离不能简单等价于不同 TA 的隔离。
2. User TA Address Space 是软件 VM 对象、物理内存对象和硬件 Page Table 的结合。 `vm_region`、`mobj`、`pgt` 各有不同职责。
3. ASID 不是安全授权机制。 它负责标识地址空间中的转换，正确性依赖分配、复用和同步生命周期。
4. PTE 修改并不意味着所有 hart 的硬件转换立即更新。 SMP 下必须设计正确的 TLB Shootdown。
5. Shared Memory 永远属于需要特别验证的信任边界。 Linux 可以在 TA 处理期间修改共享数据。
6. Memory Isolation、Memory Sanitization、Memory Integrity 和 ASLR 互不替代。 一个安全的 TEE 需要综合处理这些不同性质的保护。

# 11.14 Staff Engineer 级别面试与架构评审题

下面设计 15 道问题，涵盖实际 SoC Porting、Kernel Architecture、SMP、Security Vulnerability 和性能设计。

这些题目不是简单的知识点记忆题，而是按照实际设计评审或疑难故障排查的形式提出。

每道题后面附上回答时必须触及的关键技术点，便于自行评估答案深度。

## Part A — Memory Architecture & Isolation

### Q1. 如果 Linux 和 OP-TEE 都运行在 S-mode，为什么 Linux 不能通过构造 Sv39 Page Table 访问 OP-TEE 的物理内存？

假设：

- 16-hart RISC-V SoC。
- OpenSBI 运行在 M-mode。
- Linux 和 OP-TEE 位于不同的 Supervisor Domain。
- Linux 已被攻击者完全控制。
- 攻击者可以修改 Linux `satp`、页表和 S-mode CSR。

请解释：

1. Linux 能否将 OP-TEE Private PA 写入自己的 PTE？
2. CPU 为什么仍可能拒绝这个访问？
3. PMP 是在地址转换的哪个阶段约束访问的？
4. 如果 SoC 内有支持 DMA 的 PCIe 设备，是否需要额外保护？
5. 如果 OpenSBI Domain 的内存策略错误，OP-TEE Core MMU 能否补救？

考察重点： PMP、S-mode MMU、Physical Access Control、DMA Isolation、TCB 和 Threat Model。

### Q2. 请详细解释 RISC-V OP-TEE 为什么要使用 Per-Hart Root Page Table 与 User Mapping Slot，而不是简单地为每个 TA 独立切换整个 `satp.PPN`。

要求：

1. 说明 Core Mapping 与 TA Mapping 的组织关系。
2. 分析这种设计的性能优势。
3. 解释多 hart 如何同时执行不同 TA。
4. 分析它的 TLB Synchronization 要求。
5. 讨论这种设计中 User Mapping Slot 的并发约束。

考察重点： RISC-V OP-TEE `core_mmu_arch.c`、`satp`、User Mapping、Per-Hart State。

### Q3. 如果某个 TA 的 `PTE_U=1`、`PTE_R=1`，OP-TEE S-mode 是否可以直接读取它？

进一步讨论：

1. `sstatus.SUM` 的作用。
2. 如果 `SUM=0`，S-mode 尝试读取 User Page 会怎样？
3. 如果 `SUM=1`，是否意味着 S-mode 可以执行 User Page 上的代码？
4. 如何设计安全的 User Buffer Copy？
5. `MXR` 又会怎样影响 Execute-only Page？

考察重点： RISC-V Privilege Architecture、PTE Permissions、Secure User Access。

## Part B — TA Memory Lifecycle

### Q4. 一个 TA 退出后，它的物理页被分配给另一个 TA。如何保证数据不会泄露？

假设：

```
TA A:
    Holds a 256-bit private key in Heap

TA A exits

TA B:
    Receives same physical page
```

要求设计完整的 Memory Reclamation Strategy。

特别说明：

- PTE Unmapping。
- TLB Invalidation。
- Physical Page Sanitization。
- Memory Allocator Ownership。
- 多核并发访问。
- DMA 或其他硬件 Master 对页面的残留访问。

考察重点： Memory Lifetime、Use-After-Free、Confidentiality、Isolation。

### Q5. 你负责 Review 一个 TA Loader Patch。开发者希望提高性能，在 TA 初始化期间保持 `.text` 页可写可执行，直到 TA 第一次退出时再恢复 RX。你是否接受？

请说明：

1. 哪些攻击面会被扩大？
2. 为什么加载完成后应尽早恢复最终权限？
3. 需要哪些 TLB 或 Instruction Cache 同步？
4. 如果两个 VA Alias 指向同一个 PA，如何保证 W^X？
5. 怎样设计可验证的回归测试？

考察重点： W^X、Executable Mapping、Loader Security、Cache/TLB Synchronization。

### Q6. 系统限制只能同时分配 32 个有效 ASID，但是允许创建 100 个 TA Context。应当如何设计 ASID Management？

请讨论：

- ASID Allocation Policy。
- ASID Recycling。
- ASID Exhaustion。
- Lazy ASID Assignment。
- Generation Counter 的适用性。
- 全局与局部 TLB Synchronization。
- 如何证明 ASID Reuse 不会产生 Cross-TA Data Leakage。

考察重点： Address Space Lifetime、TLB Tag、Resource Management、Performance。

## Part C — SMP & TLB Shootdown

### Q7. 多 hart 环境中发生一个只在压力测试下偶发的 TA Memory Corruption。如何确认是否由 Stale TLB 引起？

系统场景：

```
Hart 0: TA A is running

Hart 1: TA A is running

Hart 0:
    Unmaps Page X
    Reuses physical page for TA B

Hart 1:
    Occasionally writes old Page X
```

请设计：

1. 故障复现方法。
2. TLB Shootdown 日志。
3. 必须观察的 CSR、ASID 和 PTE。
4. 如何将物理页回收延迟到正确时机。
5. 如何验证修复不仅在 QEMU 上有效，也适用于真实 SoC。

考察重点： Cross-Hart Synchronization、Memory Reclamation、Race Condition、Debugging Methodology。

### Q8. 请解释以下两段代码在多核环境中的区别。

代码 A：

```
pte->entry = new_entry;

asm volatile("sfence.vma x0, x0");
```

代码 B：

```
pte->entry = new_entry;

publish_pte_update();

local_sfence();

remote_sfence();

wait_for_completion();
```

问题：

1. 什么情况下 A 足够？
2. 什么情况下 B 才是合适的设计？
3. Remote Fence 目标 hart 如何选择？
4. 如果 Remote Fence 失败，能否继续释放对应物理页？
5. 全局 PTE、ASID-specific PTE 和非叶子 PTE 的更新在同步范围上有何区别？

考察重点： RISC-V Memory Ordering、`SFENCE.VMA`、SBI RFENCE、Page Table Consistency。

### Q9. 为什么在 `core_mmu_set_user_map()` 中执行 local TLBI 通常是合理的，但在 `vm_info_final()` 的 ASID 回收路径中可能需要 Remote TLBI？

这题需要结合真实 OP-TEE 源码回答。

要求区分：

- Per-Hart Root PGT。
- Private User Mapping Slot。
- Shared TA Page Tables。
- ASID Lifetime。
- 多 hart TLB Cache。
- 当前 User Context 的归属与执行状态。

考察重点： 不只是理解架构规范，还必须理解 OP-TEE 的实际实现约束。

## Part D — Shared Memory & Security Vulnerabilities

### Q10. Linux 已被完全攻陷，攻击者能否通过 Dynamic Shared Memory 让 OP-TEE 泄露密钥？

攻击者可以：

- 任意构造 Shared Memory Page List。
- 修改 Shared Buffer 内容。
- 高频注册和注销 Shared Memory。
- 伪造非法 Size、Offset 和 Cookie。
- 在 OP-TEE 使用 Buffer 时并发修改内容。

请列出可能的攻击面，并设计 OP-TEE Core 的验证策略。

特别分析 TOCTOU、Page Alias、Physical Range Validation 和 Object Lifetime。

考察重点： Untrusted Memory、Memory Objects、TOCTOU、Attack Surface。

### Q11. OP-TEE TA 调用一个 HSM 加密服务，使用 Shared Memory 传入明文和接收密文。该 SoC 的 HSM Core 不参与主 CPU 的 Cache Coherent Interconnect。应该如何设计内存同步？

要求讨论：

1. CPU Cache Clean/Invalidate。
2. Device Memory Barrier。
3. Producer / Consumer Ownership。
4. HSM 是否可以访问 TA Private Memory。
5. DMA Buffer 的物理地址合法性。
6. 如果 HSM 操作失败、超时或者重置，如何回收 Buffer。

考察重点： SoC Cache Coherency、Secure DMA、HSM IPC、Physical Ownership。

### Q12. 如果某个 TA 把一个 Shared Memory Buffer 的 Pointer 直接保存到自己的持久化 Context，并在未来的另一个 Session 中继续使用，会发生什么问题？

讨论：

- Shared Memory Registration Lifetime。
- User VM Region Lifetime。
- TA Session Boundary。
- Memory Object Reference Counting。
- 重新映射导致的 VA/PA 变化。
- 如何设计正确的 Buffer Handle API。

考察重点： Object Lifetime、Dangling Pointer、Session Management。

## Part E — Pager、Security Hardening 与架构设计

### Q13. 你负责将 Arm OP-TEE Pager 移植到 RV64，目标 SoC 只有 256 KiB 受硬件保护的 SRAM。请给出完整设计方案。

要求覆盖：

1. 哪些代码必须常驻 SRAM？
2. Backing Store 放在哪里？
3. 如何保护 Code Page 完整性？
4. 如何保护 Data Page 机密性与完整性？
5. 如何处理同一页面上的并发 Page Fault？
6. 如何避免 Pager Recursive Fault？
7. 如何处理 MMU、ASID、TLB 和 Page Table Cache？
8. 如何设计 Fault Injection 和性能测试？

考察重点： OS Demand Paging、TEE Security、Memory Integrity、Architecture Porting。

### Q14. OP-TEE Core 已启用 ASLR、Stack Canary 和 W^X，但安全审计仍发现攻击者可以通过 REE 控制某些 TEE 执行路径。为什么这些安全特性不一定能防御该攻击？

请从：

- Shared Memory Data Validation。
- ABI 参数边界。
- Memory Corruption。
- Control-Flow Integrity。
- Physical Isolation。
- Address Disclosure。
- DMA Attack。
- TCB 中的其他组件。

这些方面进行分析。

考察重点： Defense in Depth、Security Boundary、Mitigation Limits。

### Q15. Staff Engineer System Design：为一款 16-Hart RISC-V Server SoC 设计完整 OP-TEE Memory Protection Architecture。

系统配置：

Target SoC Specifications

| 项目              | 配置                            |
| --------------- | ----------------------------- |
| Application CPU | 16 × RV64 Hart                |
| MMU             | Sv48                          |
| Interrupt       | AIA                           |
| Firmware        | OpenSBI M-mode                |
| TEE             | OP-TEE S-mode                 |
| Normal OS       | Linux S-mode                  |
| Security        | PMP + SoC Firewall            |
| HSM             | 独立 RISC-V Core                |
| Memory          | DRAM + Secure Reserved Region |
| TEE Workload    | 多 TA 并发、Secure Storage、Crypto |

以上为假设性设计题。

设计要求：

1. 定义 Linux/OP-TEE/HSM 的 Trust Boundary。
2. 设计 Secure RAM、TA RAM、Shared RAM 的 Physical Layout。
3. 设计 Core/User Virtual Address Space。
4. 说明每个 hart 的 Root Page Table、ASID 和 User Mapping 如何组织。
5. 设计 PMP/SoC Firewall Permission Policy。
6. 设计 TA Memory Allocation / Reclamation。
7. 设计 TLB Shootdown 与 SMP Synchronization。
8. 设计 HSM Shared Buffer 与 DMA Coherency。
9. 设计 Panic、Fault Recovery 和安全状态保持。
10. 设计测试矩阵，证明隔离、性能和并发正确性。

考察重点： 能否将 RISC-V ISA、OpenSBI、OP-TEE Kernel、SoC Hardware 和 Security Engineering 统一到一个可靠的设计之中。

## 11.15 如何判断自己是否真正达到 Staff Engineer 水平？

这 15 道题中，我认为最值得作为核心验收标准的是：

Q2、Q7、Q9、Q10、Q13、Q15。

如果只能够解释：

- `satp` 是什么。
- ASID 是什么。
- `SFENCE.VMA` 是什么。
- PMP 是什么。

那还停留在架构知识层面。

如果能够进一步回答：

- 为什么 RISC-V OP-TEE 使用 per-hart User Mapping。
- 为什么某个 TLB Flush 可以是 Local，而另一个必须是 Remote。
- Page Table 更新完成后，何时能够安全地释放物理页。
- 为什么合法 Shared Memory 仍然可能造成 TOCTOU。
- 如何将 Pager 移植到 RISC-V，并给出可靠的测试方案。
- 如何在 16-hart SoC 中设计完整的 TEE Memory Isolation。

那么就开始真正具备 Staff-level OP-TEE Architecture & Porting 的思维能力了。

## 本章源码阅读优先级

建议按以下顺序完成源码精读：

1. [core_mmu_arch.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/mm/core_mmu_arch.c?utm_source=chatgpt.com) — RISC-V 页表、ASID、User Mapping 和 TLB。
2. [vm.c](https://github.com/OP-TEE/optee_os/blob/master/core/mm/vm.c?utm_source=chatgpt.com) — User TA VA Region 与生命周期。
3. [pgt_cache.c](https://github.com/OP-TEE/optee_os/blob/master/core/mm/pgt_cache.c?utm_source=chatgpt.com) — 页表资源管理。
4. [mobj.c](https://github.com/OP-TEE/optee_os/blob/master/core/mm/mobj.c?utm_source=chatgpt.com) — Memory Object 抽象。
5. [tlb_helpers_rv.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/mm/tlb_helpers_rv.S?utm_source=chatgpt.com) — 本地 `SFENCE.VMA`。
6. [abort.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/abort.c?utm_source=chatgpt.com) — RISC-V 异常和访问错误处理。
7. [Arm tee_pager.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/arm/mm/tee_pager.c?utm_source=chatgpt.com) — 研究成熟 Pager 的参考实现。

同时建议将 [RISC-V Privileged Architecture Specification](https://docs.riscv.org/reference/isa/_attachments/riscv-privileged.pdf?utm_source=chatgpt.com) 中的 Supervisor Address Translation、PTE、`satp` 和 `SFENCE.VMA` 作为最终的硬件行为依据。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



下一步最值得深入的专题是：`core_mmu_set_user_map()`、`asid_alloc()`、`tlbi_remote()` 和 `vm_info_final()` 四个函数如何配合，在多个 hart 上实现可证明正确的 TA Address Space Isolation。

这四个函数几乎贯穿了本章最重要的概念，也非常适合作为下一次汇编级、源码级深度分析的主线。
