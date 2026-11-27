# **Chapter 10 — OP-TEE Boot-time Memory Initialization**

OP-TEE OS Internals · PART II · Advanced / Staff Engineer Level

**主题：从 Firmware 提供的一块物理内存，到 OP-TEE 能够安全管理的完整内存系统**

本章基于 OP-TEE OS 4.11.0 上游源码。以 Arm 实现为主要分析对象，同时结合 RISC-V RV64 的启动实现进行对照。

## **一、本章要解决的核心问题**

这一章表面上讨论 Boot-time Memory Initialization，但实际上涉及一个操作系统最根本的问题：

**在 OS 自己的内存管理机制尚未建立之前，OS 是如何使用内存来初始化内存管理机制本身的？**

这个问题看起来简单，但会产生一系列相互依赖的难题。

例如：

- MMU 尚未启用，OP-TEE 如何执行 C 代码、访问 Global Variable、使用 Stack？
- Page Table 需要内存才能建立，那么最初的 Page Table Memory 从哪里来？
- `malloc()` 尚未初始化，谁给内存管理初始化代码分配内存？
- OP-TEE Core、TA RAM、Shared Memory 之间如何划分？
- 物理内存已经由上一级 Firmware 保护，为什么 OP-TEE 还需要重新建立 MMU Mapping？
- Boot 阶段分配的内存，什么时候可以被 Runtime Allocator 接管？
- 启用 Pager 时，初始化阶段的内存管理机制发生了什么变化？
- 如果启用 ASLR，Linker Address、Load Address 和 Runtime VA 如何保持一致？

这些问题不是独立存在的，而是构成了一个必须严格遵守顺序的 Bootstrap Dependency Chain。

## **10.1 首先建立正确的内存管理心智模型**

我希望先建立一个基本认识：

> OP-TEE 的内存初始化不只是配置 MMU，而是建立物理内存的安全边界、描述内存区域、构造地址映射、分配运行时资源，并最终完成从 Boot Allocator 到 Runtime Memory Manager 的资源管理权交接。

### **10.1.1 内存管理实际上有五个不同的维度**

| 维度                 | 解决的问题              | 典型组件                              |
| ------------------ | ------------------ | --------------------------------- |
| Physical Ownership | 谁真正有权访问物理地址        | Firmware、TZC、PMP、SoC Firewall     |
| Physical Layout    | 哪些地址属于 Core、TA、SHM | Platform Config、Memory Regions    |
| Virtual Mapping    | CPU 如何从 VA 访问 PA   | MMU、Page Table、TLB                |
| Memory Allocation  | 如何分配和释放可用内存        | `boot_mem`、`phys_mem`、`malloc`    |
| Memory Lifetime    | 不同阶段内存何时生效、释放      | Linker Section、Pager、Init Runtime |

它们必须分别理解。

例如，假设 OP-TEE Core 运行在：

```
PA: 0x9000_0000 - 0x91FF_FFFF
```

我们需要分别回答：

1. **Ownership：** Linux 是否能够访问这段 PA？
2. **Layout：** 其中哪些区域放 Core，哪些供 TA 使用？
3. **Mapping：** OP-TEE 访问这段 PA 时使用什么 VA？
4. **Allocation：** 当前哪部分已经占用、哪部分可以分配？
5. **Lifetime：** Boot 结束后，哪些临时内存可以释放？

第一个问题不应该仅由 OP-TEE 自己的 MMU 解决。

因为 Linux 并不使用 OP-TEE 的 Page Table。

这一点对于理解 Secure World 和 RISC-V Trusted Domain 尤其重要。

### **10.1.2 一个典型的物理内存布局**

假设某 SoC 有 256 MiB DDR，示例布局如下：

示例：Physical Address Map

仅用于教学说明，不代表某块开发板的实际配置

**Normal World DDR**

224 MiB

0x8000_0000 – 0x8DFF_FFFF

Linux / U-Boot / Normal World Allocations

**Secure DDR (TZDRAM)**

28 MiB

0x8E00_0000 – 0x8FBF_FFFF

OP-TEE Core

Code · RO Data · RW Data · Stack · Heap · Page Tables

TA RAM / Other Secure Allocations

TA Code · TA Data · User Stack · Memory Objects

**Reserved Non-Secure SHM**

4 MiB

0x8FC0_0000 – 0x8FFF_FFFF

Linux / OP-TEE Communication

这里有三个特别容易混淆的概念。

**TZDRAM 不等于 OP-TEE Core RAM。** TZDRAM 是安全物理内存区域的概念，其中可能包含 Core、TA 内存和其他安全资源。

**TA RAM 不等于 TA Virtual Address Space。** TA RAM 描述实际的安全物理内存来源；TA VA 是运行时为不同 TA 创建的地址映射。

**Shared Memory 不一定是静态保留的连续物理内存。** OP-TEE 也支持由 Normal World 提供并注册的动态共享内存，不能把所有 SHM 都理解成 `CFG_SHMEM_START` 定义的一块固定内存。

### **10.1.3 物理内存保护必须先于软件访问控制**

这对 RISC-V OP-TEE Porting 尤其重要。

传统 Arm TrustZone：

```
CPU Security State
       |
       v
SoC Security Attribution
       |
       v
TZC / Interconnect Protection
       |
       v
Secure DDR
```

RISC-V Trusted Domain：

```
Firmware / Trusted Domain Manager
       |
       v
PMP / SMMPT / SoC-specific Protection
       |
       v
Protected Physical Memory
       |
       v
OP-TEE CPU Page Tables
```

第二张图只是描述典型隔离层次，并不意味着 PMP 与 SMMPT 属于同一套地址转换机制，或者所有 RISC-V 平台都必须采用这一组合。

**重要结论：CPU 页表的权限只约束使用该页表的地址转换上下文，不能替代系统级的物理内存访问控制。**

因此，一个安全的 Boot Contract 必须确保 OP-TEE 内存的保护已经建立，或者明确规定由哪个可信阶段完成建立。在完成保护之前，不能让不可信执行主体获得访问机会。

## **10.2 OP-TEE 从 Boot 到 Runtime 的内存初始化时间线**

先看完整时间线：

1. **Stage 0 — Firmware Handoff**

   Firmware 加载 OP-TEE 镜像，确定物理内存边界和入口状态。
2. **Stage 1 — Early Assembly**

   建立临时 Stack、清理需要清零的 BSS、准备最小 C Runtime。
3. **Stage 2 — Boot Memory Allocator**

   使用确定存在的内存区域，支持早期临时和持久分配。
4. **Stage 3 — Build Initial MMU Map**

   收集物理内存区域，分配 VA，构造 Translation Tables。
5. **Stage 4 — Relocation & Enable MMU**

   完成需要的重定位和地址切换，让后续代码按照 Runtime VA 执行。
6. **Stage 5 — Initialize Runtime Memory Managers**

   初始化 Heap、保存 Memory Map、建立 Physical Memory Pools。
7. **Stage 6 — Optional Pager & Dynamic Allocators**

   初始化 Pager、动态 VA 分配器、回收或释放 Boot 阶段内存。
8. **Stage 7 — TEE Runtime Ready**

   完成 Thread、TA、Driver 等系统初始化，允许正常安全服务执行。

这里的 Stage 是为了方便理解而定义的逻辑阶段，并不对应上游八个名称完全一致的函数。不同架构和配置可能将这些操作拆分到不同位置。

### **10.2.1 为什么不能一开始就使用&#x20;**`malloc()`**？**

因为通用 `malloc()` 本身就需要有效的内存池、Allocator Metadata 和访问权限。

如果初始化 `malloc()` 又依赖于 `malloc()`，就构成了循环依赖：

```
malloc() requires initialized Heap
          |
          v
Heap initialization requires allocation
          |
          v
malloc()
```

因此必须存在一个更原始的内存分配机制。

这类机制通常只负责从一段确定可访问的内存中取出空间，可能不支持任意释放，也不需要复杂的 Free List 或多线程同步。

OP-TEE 4.11.0 中，`core/mm/boot_mem.c` 就承担了这个重要角色。它提供面向 Boot 阶段的分配及重定位支持。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们稍后会深入这个实现。

# **10.3 Secure Memory 的定义、保留与注册**

现在开始进入源码。

## **10.3.1 三个不同的内存配置来源**

OP-TEE 的初始内存布局通常涉及：

**第一层：Platform Configuration**

例如：

```
CFG_TZDRAM_START = ...
CFG_TZDRAM_SIZE  = ...
CFG_SHMEM_START  = ...
CFG_SHMEM_SIZE   = ...
```

具体哪些宏有效，取决于平台、构建配置及是否启用动态共享内存。

它们用于描述平台内存布局，而不是直接配置硬件安全控制器。

**第二层：Linker Script**

例如：

```
core/arch/arm/kernel/kern.ld.S
core/arch/riscv/kernel/kern.ld.S
```

负责安排：

- Core Code 所在地址。
- 只读和可写数据。
- `.bss`、`.nozi`。
- Heap、Stack。
- 初始化段。
- 需要的页对齐和镜像布局约束。

**第三层：Runtime Memory Registration**

例如：

```
register_phys_mem(MEM_AREA_IO_SEC,
                  UART_BASE, UART_SIZE);
```

这是 OP-TEE 的一种物理内存区域注册机制。

它告诉内存映射构建代码：

> 存在这样一块物理地址区域，它应该以给定的 Memory Type 纳入内存映射规划。

注意：`register_phys_mem()` **不是立即创建页表映射，也不是对硬件安全防火墙进行编程**。

它描述的是初始化时需要收集的内存区域。

## **10.3.2&#x20;**`core_mmu_phys_mem`**&#x20;的作用**

源码入口：

`core/include/mm/core_mmu.h`

核心数据结构可以概括为：

```
struct core_mmu_phys_mem {
    const char *name;
    enum teecore_memtypes type;
    /* physical address and size */
};
```

这里展示的是简化后的逻辑结构。实际定义还包含适配不同地址位宽的联合体。

它本质上是一个 **Physical Memory Descriptor**。

例如：

```
Name:   UART0
Type:   MEM_AREA_IO_SEC
PA:     0x84000000
Size:   0x10000
```

此时我们仅仅知道一块物理内存存在，尚未确定 OP-TEE 最终通过哪个 VA 来访问它。

## **10.3.3&#x20;**`teecore_memtypes`**&#x20;是核心抽象之一**

在 4.11.0 中，`core/include/mm/core_mmu.h` 定义了多种 Memory Type。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



重要类型包括：

| Memory Type                | 用途                           |
| -------------------------- | ---------------------------- |
| `MEM_AREA_TEE_RAM`         | Core 内存的通用映射类型               |
| `MEM_AREA_TEE_RAM_RX`      | Core 可读可执行内存                 |
| `MEM_AREA_TEE_RAM_RO`      | Core 只读内存                    |
| `MEM_AREA_TEE_RAM_RW`      | Core 可读写内存                   |
| `MEM_AREA_INIT_RAM_RX`     | 初始化阶段的 RX 内存                 |
| `MEM_AREA_INIT_RAM_RO`     | 初始化阶段的 RO 内存                 |
| `MEM_AREA_SEC_RAM_OVERALL` | 安全 RAM 的整体映射/管理描述            |
| `MEM_AREA_NSEC_SHM`        | 保留的 Non-Secure Shared Memory |
| `MEM_AREA_SHM_VASPACE`     | 动态 SHM 映射所需 VA 空间            |
| `MEM_AREA_TEE_DYN_VASPACE` | Core 动态映射所需 VA 空间            |
| `MEM_AREA_PAGER_VASPACE`   | Pager 管理的 VA 空间              |
| `MEM_AREA_IO_SEC`          | Secure MMIO                  |
| `MEM_AREA_IO_NSEC`         | Non-Secure MMIO              |

这里有个重要的设计思想：

**OP-TEE 通过 Memory Type 将“使用目的”和“最终硬件页表属性”分离。**

例如：

```
MEM_AREA_TEE_RAM_RX
        |
        v
core_mmu_type_to_attr()
        |
        v
TEE_MATTR_SECURE
TEE_MATTR_PRX
        |
        v
Architecture Backend
        |
        v
Arm Descriptor / RISC-V PTE
```

其中 `TEE_MATTR_PRX` 描述的是 Privileged Read/Execute 权限。

这种设计使通用内存管理代码不必直接处理 Arm AP/PXN/UXN 或 RISC-V PTE R/W/X 位。

但要特别注意：**不能推断所有 Core 内存都天然满足 W^X。**

4.11.0 中通用 `MEM_AREA_TEE_RAM` 仍可映射为 Privileged RWX；是否拆分成 RX、RO、RW 区域，要看实际的构建配置和平台支持，例如 `CFG_CORE_RWDATA_NOEXEC`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这也是为什么安全属性必须通过最终页表进行验证，不能仅凭 Section 名称推断。

## **10.3.4 Memory Region 需要满足哪些不变量？**

初始化阶段至少要确保：

1. 安全物理内存区域与不应重叠的 Non-Secure Shared Memory 不发生重叠。
2. Core 的代码、数据以及所需初始化内存位于允许的物理区域。
3. 地址和大小满足硬件映射粒度要求。
4. 地址区间的结束计算不能发生整数溢出。
5. 同一物理内存不能被意外赋予冲突的 Cache Attributes。
6. 已被 Boot Firmware、Core 或设备占用的内存不能错误地进入可自由分配的内存池。
7. 不可信 DT 信息不能未经验证就被用作扩大安全访问权限的依据。

源码中可以看到部分对应检查：

```
check_sec_nsec_mem_config();
check_mem_map(&mem_map);
```

它们负责不同层次的内存配置一致性检查，但不应把它们理解为对整个 SoC 硬件隔离配置的完整证明。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1

### **一个值得深入思考的案例**

假设 Firmware 把下面这段内存配置为 Non-Secure：

```
0x9000_0000 - 0x90FF_FFFF
```

而 OP-TEE Platform Config 错误地把相同范围定义为 Secure RAM。

那么可能出现三种结果：

- 如果硬件权限禁止 Secure World 对该区域的特定访问，初始化阶段可能产生 Access Fault。
- 如果硬件允许 Secure World 访问，但 Normal World 同样可以访问，那么 OP-TEE 即使正常启动，也可能已经失去了机密性。
- 如果只有 OP-TEE 的软件 Memory Map 与真实硬件配置不一致，错误可能在运行某个 TA 或启用 DMA 后才暴露。

这说明：

**Boot 成功不是内存安全隔离正确的充分条件。**

下一步我们要研究，OP-TEE 如何把这些 Memory Descriptor 转换成真正的 CPU Page Table。

# **10.4 Boot-time Memory Allocator：内存管理如何完成自举？**

这是本章第一个需要深入理解的源码模块。

源码：

[core/mm/boot_mem.c — OP-TEE 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/mm/boot_mem.c?utm_source=chatgpt.com)

## **10.4.1 为什么不直接使用静态数组？**

最简单的 Early Allocator 可以这样实现：

```
static uint8_t early_heap[64 * 1024];
static size_t offset;

void *early_alloc(size_t size)
{
    void *p = &early_heap[offset];

    offset += size;
    return p;
}
```

它确实能实现内存分配，但在真正的 Kernel Boot 中存在几个问题。

首先，内核可能需要调整实际映射范围和执行地址。早期分配器内部保存的指针可能需要参与 Relocation。

其次，Boot 初始化会同时存在两类对象：

- 需要在初始化完成后继续存在的对象，例如某些 Core-local 数据。
- 只用于建立初始页表的临时对象，例如构造 Memory Map 时使用的数组。

如果统一向上分配，临时对象散布在永久对象之间，后续就很难把这些空间安全地回收。

因此，OP-TEE 的 Boot Allocator 使用了更有意思的设计。

## **10.4.2&#x20;**`boot_mem_desc`**：从两端进行分配**

4.11.0 中的核心数据结构：

```
struct boot_mem_desc {
    vaddr_t orig_mem_start;
    vaddr_t orig_mem_end;
    vaddr_t mem_start;
    vaddr_t mem_end;
    struct boot_mem_reloc *reloc;
    struct boot_mem_padding *padding;
};
```

它描述了一段 Boot Memory Range，并且维护两个分配方向。

Boot Memory 双端分配模型

Persistent Boot Allocations

从低地址向高地址增长 →

**Free Space**

mem_start ... mem_end

Temporary Boot Allocations

← 从高地址向低地址增长

orig_mem_start

orig_mem_end

对应的两个接口：

```
void *boot_mem_alloc(size_t len, size_t align);

void *boot_mem_alloc_tmp(size_t len, size_t align);
```

区别不是“安全”与“不安全”，而是 **内存生命周期不同**。

`boot_mem_alloc()` 为 Boot 期间需要保留下来的对象分配空间。

`boot_mem_alloc_tmp()` 为可以在初始化完成后统一释放的临时对象分配空间。

这类设计相比通用 Heap 更简单，也更适合初始化阶段。

### **为什么要记录 Padding？**

例如：

```
Allocation A: 100 bytes
Alignment B: 4096 bytes
```

对齐操作会在两个对象之间形成未使用的内存。

这些空间并不一定必须浪费。

OP-TEE 使用 `boot_mem_padding` 描述部分可利用的对齐空隙，在 Boot 初始化的适当阶段将它们加入运行时分配池。

可以在源码中找到：

```
boot_mem_foreach_padding(...)
```

这里反映的是一种非常典型的嵌入式 OS 设计思想：

**Boot 阶段使用简单、确定性的分配算法；进入 Runtime 后，再把已经不需要的 Boot 空间交给更复杂的分配器。**

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

## **10.4.3 以 RISC-V RV64 的真实启动路径为例**

在 `core/arch/riscv/kernel/entry.S` 中，Primary Hart 初始化时首先建立基础执行环境，然后进入 Boot Allocator 和 MMU 初始化。

为了看清逻辑，我们将真实源码路径抽象为：

```
reset_primary
    |
    +-- Clear BSS
    |
    +-- Disable address translation (satp = 0)
    |
    +-- Setup temporary SP / TP
    |
    +-- Save boot arguments
    |
    +-- Early platform initialization
    |
    +-- boot_mem_init(...)
    |
    +-- core_init_mmu_map(...)
    |
    +-- Optional ASLR relocation
    |
    +-- enable_mmu()
    |
    +-- boot_init_primary_early()
```

这是基于 4.11.0 RISC-V `entry.S` 的阶段性梳理，不是逐条指令的转录。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里有两个非常值得关注的细节。

**第一，**`boot_mem_init()`**&#x20;在&#x20;**`core_init_mmu_map()`**&#x20;之前。**

因为初始化 Memory Map 本身需要一些临时存储空间，而这个时候还不能依赖完整的 Runtime Allocator。

**第二，启用 MMU 之前的指针，与启用 MMU 之后的指针不一定相同。**

尤其是 `CFG_CORE_ASLR=y` 时。

因此，RISC-V 启动实现需要处理 Relocation，调整必要的 Stack/Core-local 指针，并调用：

```
boot_mem_relocate();
```

这个问题比“如何填写 `satp`”更值得深入。

因为 CPU 成功执行地址空间切换，只说明硬件开始使用 Page Table。

**它并不保证软件保存的每一个指针都已经与新地址空间一致。**

# **10.5 Initial MMU Map：从 Memory Descriptor 到真实 Page Table**

现在进入本章最重要的部分。

源码：

[core/mm/core_mmu.c — OP-TEE 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/mm/core_mmu.c?utm_source=chatgpt.com)

## **10.5.1 MMU 初始化需要解决三个问题**

第一，**What to map？**

哪些物理区域需要被映射？

第二，**Where to map？**

每一个 PA Range 应该映射到哪个 VA Range？

第三，**How to map？**

对应的权限、Memory Type、Page Size 应该是什么？

OP-TEE 把这三个问题分离处理。

### **内存区域描述**

核心运行时结构可以概括为：

```
struct tee_mmap_region {
    enum teecore_memtypes type;
    paddr_t pa;
    vaddr_t va;
    size_t size;
    size_t region_size;
    uint32_t attr;
};
```

这是用于讲解的简化字段集合，成员顺序及其他细节以实际版本头文件为准。

请注意它与 `core_mmu_phys_mem` 的不同：

| `core_mmu_phys_mem` | `tee_mmap_region` |
| ------------------- | ----------------- |
| 描述物理内存区域            | 描述内存映射            |
| 关注 PA 与区域类型         | 关注 PA、VA、属性、粒度    |
| 在映射规划前注册            | 构建初始映射时形成         |
| 不代表硬件页表已建立          | 用于构造和管理页表         |

而真正的 Arm/RISC-V Translation Table 是第三层数据结构。

因此存在如下转换关系：

```
Physical Memory Registration
          |
          v
core_mmu_phys_mem
          |
          v
tee_mmap_region
          |
          v
Architecture Translation Table Builder
          |
          v
PTE / MMU Descriptor
          |
          v
CPU MMU
```

## **10.5.2 深入&#x20;**`core_init_mmu_map()`

这是 4.11.0 中建立初始 Core MMU Map 的关键入口之一：

```
core_init_mmu_map(seed, &boot_mmu_config);
```

其内部可以整理为如下逻辑：

```

/* 教学伪代码：表达调用关系，不是原函数逐行转录 */

void initialize_boot_mmu(unsigned long seed)
{
    check_secure_memory_config();

    memory_map = allocate_boot_memory_map();

    collect_mem_ranges(memory_map);
    assign_mem_granularity(memory_map);

    if (pager_enabled)
        reserve_pager_virtual_space(memory_map);

    assign_virtual_addresses(memory_map, seed);

    check_mem_map(memory_map);

    build_arch_translation_tables(memory_map);

    prepare_arch_mmu_registers();

    save_boot_mapping_and_relocations();
}

```

实际代码可按以下函数进行追踪：

```
core_init_mmu_map()
    |
    +-- check_sec_nsec_mem_config()
    |
    +-- boot_mem_alloc_tmp()
    |
    +-- init_mem_map()
    |      |
    |      +-- collect_mem_ranges()
    |      +-- assign_mem_granularity()
    |      +-- add_pager_vaspace()  [optional]
    |      +-- assign_mem_va()
    |
    +-- check_mem_map()
    |
    +-- core_init_mmu()
    |
    +-- core_init_mmu_regs()
```

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1

这里我建议特别研究 `init_mem_map()` 内部几个操作的设计目的。

### **A.&#x20;**`collect_mem_ranges()`

此函数把不同来源的内存描述汇集到一个 Memory Map。

包括：

- OP-TEE Core 相关区域。
- 平台注册的 MMIO。
- Shared Memory。
- 保留的 VA 空间。
- 可选的 Pager VA Space。
- 特定配置下的 NEX 或 Dynamic VA Space。

一个关键细节是：**有些区域只有 VA Space，没有立即对应的物理页。**

例如：

```
MEM_AREA_SHM_VASPACE
MEM_AREA_TEE_DYN_VASPACE
MEM_AREA_PAGER_VASPACE
```

这些区域是为未来的动态映射预留的虚拟地址范围。

不能因为它们出现在 Memory Map 中，就认为已经分配了等量的物理内存。

### **B.&#x20;**`assign_mem_granularity()`

不是所有 Memory Region 都应该使用相同粒度。

例如：

```
Core Code / Data:
    需要 4 KiB 级别的细粒度权限控制

Large Device Region:
    可能使用较大的 Translation Block
```

4.11.0 中，Core RAM 类型通常使用小页映射粒度；其他区域可以按 Page Directory 层级的粒度规划，最终仍取决于架构实现和实际映射条件。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么需要这样做？

因为映射粒度直接决定：

- Page Table Memory Footprint。
- Mapping Flexibility。
- Permission Granularity。
- TLB Reach。
- 后续动态映射和拆分的成本。

例如，对于一块 2 MiB 的代码区域：

```
方案 A：512 × 4 KiB Pages

方案 B：1 × 2 MiB Block
```

方案 B 可以减少页表项和潜在的 TLB 压力。

但是，如果其中只有 4 KiB 需要不同权限，Block Mapping 就可能不再适合。

在安全 OS 中，性能优化不能突破权限边界。

### **C.&#x20;**`assign_mem_va()`

这一步解决 VA Placement。

例如：

```
Physical Address:
    0x9000_0000

Runtime Virtual Address:
    0xFFFF_0000_1000_0000
```

这里的地址是教学示例。

它说明 PA 与 VA 是两个独立的命名空间。

`core_init_mmu_map()` 还会结合 ASLR Seed 选择合适的 VA Layout，并在必要时建立临时 Identity Mapping。

## **10.5.3 为什么启用 MMU 时需要 Identity Mapping？**

考虑一段代码：

```
PC = 0x9000_2000
```

现在 CPU 尚未启用 MMU，PC 被直接用于物理地址访问。

假设即将启用 MMU，正式映射为：

```
VA 0xFFFF_0000_1000_2000
    |
    v
PA 0x9000_2000
```

问题是：

**当 MMU Enabled 的瞬间，CPU 下一条指令应该在哪里取指？**

如果当前 PC 所处的地址在新 Page Table 中不存在有效映射，就可能立刻产生 Instruction Fault。

因此必须设计安全的过渡机制。

常见做法是保留一段受限的临时 Identity Mapping：

```
VA 0x9000_2000
    |
    v
PA 0x9000_2000
```

使 MMU 打开前后的关键代码仍然可以连续执行。

然后再切换到目标 Runtime VA。

MMU Enable Transition

Before MMU

PC: Physical Address

PA → Instruction Fetch

Transition

Identity Mapping

VA = PA → Same Code

**Runtime Virtual Address Space**

Runtime VA → Page Table → Physical Memory

对 Arm 来说，还涉及 Translation Register、MAIR、TCR、SCTLR、Instruction Synchronization 等配置与指令序列。

对 RISC-V 来说，重点是：

```
csrw satp, a0
sfence.vma
```

这两条只是一个概念性示例，不代表所有实现都只需要这两条指令。准确的寄存器写入、Fence、控制流转移必须对照实际架构代码。

### **关键思考**

Identity Mapping 并不是最终目标。

我们希望它满足三个原则：

1. 只覆盖完成 Transition 所必需的区域。
2. 只提供真正必需的访问权限。
3. 如果启动流程允许，应在不再需要后移除不必要的别名映射。

否则可能出现同一物理代码页面在多个 VA 上长期可访问的情况，增加攻击面并使安全分析复杂化。

# **10.6 Runtime Heap、Physical Memory Pool 与 Boot Allocator 的交接**

现在进入第二个关键问题：

**OP-TEE 打开 MMU 后，内存管理是否已经初始化完成？**

答案是：没有。

MMU 负责地址转换，而运行时内存分配还需要建立 Allocator State。

## **10.6.1 三种不同的 Allocator**

| Allocator               | 资源类型                | 典型用途                          |
| ----------------------- | ------------------- | ----------------------------- |
| `boot_mem`              | Boot 可用的特定 VA/PA 区域 | 早期元数据和临时分配                    |
| `malloc` / `nex_malloc` | 已初始化的 Heap          | Core 对象、服务元数据                 |
| `phys_mem` / `tee_mm`   | 受管理的物理内存区间          | TA Memory、Physical Allocation |

其中：

`malloc()` 不等于 `phys_mem_alloc()`。

前者通常为 C 对象分配可访问的虚拟内存；后者关注物理内存资源及其占用管理。

同样，`tee_mm` 也会被用于管理 VA Range，其 Pool 并不都对应同一种物理内存。

## **10.6.2&#x20;**`init_primary()`**&#x20;中的实际初始化顺序**

以 Arm 4.11.0 的 `core/arch/arm/kernel/boot.c` 为例：

```
init_primary()
    |
    +-- Initialize early runtime environment
    |
    +-- malloc_add_pool() / nex_malloc_add_pool()
    |
    +-- Optional boot_mem_alloc()
    |
    +-- core_mmu_save_mem_map()
    |
    +-- core_mmu_init_phys_mem()
    |
    +-- boot_mem_foreach_padding()
    |
    +-- boot_mem_release_unused()
    |
    +-- Optional page_alloc_init()
    |
    +-- Optional init_pager_runtime()
    |
    +-- thread_init_canaries()
    |
    +-- thread_init_per_cpu()
```

这是重要的真实初始化路径，顺序能够直接对应到上游源码。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们重点看其中四个函数。

### **A.&#x20;**`malloc_add_pool()`

它把 Linker Script 预先保留的 Heap Memory 注册到运行时分配器。

例如：

```
malloc_add_pool(__heap1_start,
                __heap1_end - __heap1_start);
```

在这里应当区分：

**为 Heap 保留内存** 与 **初始化 Heap 的管理状态**。

前者可以由 Linker Script 在构建期间完成，后者必须在运行时完成。

这也是为什么 Heap 区域的地址已经存在，但初始化完成前仍不能随意调用 `malloc()`。

### **B.&#x20;**`core_mmu_save_mem_map()`

初始 Memory Map 可能位于 Boot Allocator 管理的临时内存中。

一旦初始化结束，这些临时内存可能被释放。

那么运行时 Core 仍然需要使用的 Memory Map 怎么办？

答案是：**把需要长期保留的数据结构迁移到 Runtime Heap。**

4.11.0 的这个函数将初始 Mapping Metadata 复制到通过运行时分配器获得的空间，并切换后续 Memory Map 扩展所使用的分配策略。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里出现一个特别重要的 OS 设计原则：

> 数据结构的创建地点，不等于它最终的生命周期归属。

Boot Allocator 可以创建初始 Memory Map，但其最终所有权必须交给适当的 Runtime Memory Manager。

如果忘记完成这一交接，就可能出现 Boot 内存已经释放，而 Runtime 代码仍引用旧地址的 Use-After-Free。

### **C.&#x20;**`core_mmu_init_phys_mem()`

这个函数建立物理内存分配体系。

在没有启用 Pager 的典型配置下，核心逻辑包括：

```
Initialize Physical Memory Pool
       |
       v
Reserve / Carve Out OP-TEE Core Memory
       |
       v
Reserve Other Special Memory
       |
       v
Remaining Physical Memory
       |
       v
Available for Runtime Allocation
```

为什么需要 Carve Out？

假设：

```
Secure RAM:
    0x9000_0000 - 0x91FF_FFFF

OP-TEE Core:
    0x9000_0000 - 0x903F_FFFF
```

如果直接把全部 Secure RAM 注册为空闲物理内存，那么某次 TA Allocation 可能会拿到：

```
PA 0x9000_1000
```

而这个地址可能正是 OP-TEE 的代码或数据所在区域。

于是不同使用者获得了相同 Physical Page。

这可能导致：

- Core Code 被破坏。
- Kernel Data 被覆盖。
- 内存隔离失效。
- 难以解释的随机 Boot 或 Runtime Crash。

因此，必须明确区分：

```
Physical Memory Exists

≠

Physical Memory Is Free
```

在 OP-TEE 4.11.0 的非 Pager 路径中，`core_mmu_init_phys_mem()` 会先初始化物理内存池，再通过类似 `carve_out_core_mem()` 的机制保留 Core 已占用的区域。

而 Pager 和 Virtualization 配置走的是不同的内存初始化分支，不能机械套用同一套 Carve Out 流程。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### **D.&#x20;**`boot_mem_release_unused()`

这是 Boot Allocator 生命周期的交接点之一。

它处理 Boot 阶段不再需要的空间，并在适用配置下将部分未使用的映射撤销，使底层物理内存能够进入正确的资源管理状态。

随后还有：

```
boot_mem_release_tmp_alloc();
```

负责释放剩余临时分配资源。

这两个步骤的作用并不相同，也不一定在同一个初始化阶段执行。

尤其在无 Pager 与有 Pager 的配置下，回收行为有所区别。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



## **10.6.3&#x20;**`CFG_DYN_CONFIG`**&#x20;改变了什么？**

4.11.0 中还需要理解 `CFG_DYN_CONFIG`。

其重要意义在于，部分内存管理和早期运行时状态能够在启动过程中通过动态分配和重新组织来建立，而不是完全依赖编译期间的静态布局。

例如，可以观察到：

```
boot_mem_alloc(...);
page_alloc_init();
```

用于不同阶段的资源初始化。

在 RISC-V `entry.S` 中，也能看到启用 `CFG_DYN_CONFIG` 时使用临时 `thread_core_local` 和临时 Stack，后续再切换到新分配的 Core-local 数据结构。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



但要注意，`CFG_DYN_CONFIG` 不应被简单理解为“所有内存都可以动态发现”，更不意味着能任意改变硬件安全内存边界。

它仍然受到实际平台配置、可用内存和隔离机制的约束。

## **10.6.4 Virtualization 为什么要引入&#x20;**`nex_malloc`**？**

OP-TEE 支持虚拟化时，需要区分：

- 可以跨虚拟化分区共享的执行基础设施。
- 与某个虚拟分区相关联的 TEE Runtime State。

在相关配置下，Core 中出现 `nex_malloc()` 等接口。

可以把它理解为一种更明确区分 Nexus/共享管理层资源与分区相关资源的设计。

这不是单纯为了提高分配性能，而是为了实现：

**不同生命周期、不同隔离域中的资源不应该被错误地放入同一个分配和回收体系。**

如果把一个必须跨 VM 存活的对象错误地放到 VM-specific Heap，当 VM 被销毁时，它可能被错误释放。

反过来，如果把 VM 私有状态放入跨 VM 的长期分配池，则可能造成状态泄漏或隔离失效。

# **10.7 Pager 初始化与 Boot Memory 的关系**

这一部分必须讲清楚，即使完整的 Pager 内部实现会在 Chapter 24 再次深入。

## **10.7.1 为什么 OP-TEE 需要 Pager？**

假设某 SoC 提供：

```
Secure SRAM: 512 KiB
Secure DRAM: 16 MiB
```

OP-TEE Core 的代码和数据超过了 Secure SRAM 大小。

一种方案是把整个 Core 放到 Secure DRAM。

另一种方案是：

**只把必须常驻的 Core 代码、数据和 Page Tables 保存在有限的常驻物理内存中，把部分代码或只读数据按照需要映射到运行空间。**

这就是 Pager 设计的重要动机。

它本质上利用了以下事实：

```
Virtual Address Space Size

can be larger than

Currently Resident Physical Memory
```

但这种设计也引入新的限制：

Pager 本身、Fault Handler、必要的 Translation State 不能依赖在处理 Fault 时才需要换入的不可用页面。

否则就会出现：

```
Page Fault
    |
    v
Pager Handler
    |
    v
Pager Handler itself is unavailable
    |
    v
Another Page Fault
```

这是必须在架构层面避免的循环依赖。

## **10.7.2&#x20;**`init_pager_runtime()`**&#x20;的初始化流程**

在 Arm 4.11.0 中，可以沿着以下路径研究：

```
init_pager_runtime()
    |
    +-- Determine pageable region
    |
    +-- Locate pageable image and hash metadata
    |
    +-- Initialize required storage
    |
    +-- Verify pageable contents
    |
    +-- Initialize pager virtual memory pool
    |
    +-- Register pageable region
    |
    +-- Add physical pages to pager
```

对应源码：

`core/arch/arm/kernel/boot.c`

其中 `init_pager_runtime()` 会计算初始化段和可分页段的大小，对 Pageable Content 进行哈希检查，并注册 Pager Region 与 Physical Pages。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



可以将其表示为：

Pager Boot Model

Pageable Image

Code · RO Data · Hashes

Backing Storage

Pager VA Region

Reserved Virtual Addresses

Not all pages resident

Page Fault / Mapping / Eviction

**Resident Physical Page Pool**

Only necessary pages occupy physical memory

## **10.7.3 为什么需要验证 Pageable Image 的完整性？**

分页机制意味着某个页面可能在启动之后才被映射为可执行代码。

如果 Backing Store 中的代码已经遭到修改，Pager 如果直接将其映射为可执行页面，就可能执行被篡改的代码。

因此必须建立：

```
Trusted Boot Image
       |
       v
Authenticated Page Metadata
       |
       v
Page Content Verification
       |
       v
Executable Mapping
```

注意：

**哈希检查只有在预期哈希值可信时才有安全意义。**

如果攻击者可以同时修改页面内容和用于验证它的哈希值，那么单纯比较 SHA-256 并不能提供可信启动保障。

所以 Pager Security 不仅仅是 Pager 模块自己的问题，还依赖 Secure Boot、镜像认证和可信元数据来源。

## **10.7.4 Boot Init Memory 能否回收？**

需要按 Section 和对象生命周期具体判断。

例如：

- Early Boot 使用但 Runtime 不再引用的代码和数据。
- 必须长期存在的 Unpaged Runtime Code。
- Pager 的元数据及其必要的执行路径。
- 仍然被后续初始化或驱动使用的对象。

不能仅凭 Section 名字包含 `init` 就直接释放对应的物理页。

正确条件是：

> 只有确定所有执行路径不再使用该区域，并且相关指针、映射、缓存和其他引用已经正确处理，才能将资源交给其他管理者。

这与 Linux 的 `__init` Memory Reclamation 有类似的生命周期设计思想，但不能假定二者实现机制完全相同。

# **10.8 Memory Attribute、Cacheability 与 Execute Permission**

这一部分实际上是整个 Boot Memory Security 的基础。

## **10.8.1 内存类型和访问权限不是一回事**

例如：

```
Normal Cached Memory
Device Memory
Non-Cacheable Memory
```

描述的是内存访问行为。

而：

```
Read
Write
Execute
Privileged
User
Secure
```

描述的是访问权限或安全属性。

两者必须独立配置。

一个 Device MMIO Region 通常不应该被当作普通 Cacheable RAM 映射。

如果把 UART Register 映射成 Normal Write-Back Memory，那么：

```
writel(value, UART_TX);
```

可能无法获得设备寄存器访问所需要的顺序、可见性或副作用语义。

问题并不只是“写入什么时候能到达 UART”，还包括处理器是否允许合并、重排、推测等行为。

## **10.8.2 一个 Page Table Entry 实际表达什么？**

从 OP-TEE 的角度，可以把 PTE 理解为：

```
VA → PA

+

Access Permission

+

Memory Attribute

+

Translation Granularity

+

Architecture-specific State
```

Arm 和 RISC-V 的底层描述不同。

| 概念          | Arm AArch64   | RISC-V RV64              |
| ----------- | ------------- | ------------------------ |
| 页表根         | TTBR          | `satp.PPN`               |
| 地址空间标识      | ASID          | `satp.ASID`              |
| 读写执行        | AP、UXN、PXN 等  | PTE R/W/X/U              |
| Memory Type | MAIR/AttrIndx | PMA/PBMT 等机制             |
| TLB 同步      | TLBI、DSB、ISB  | `sfence.vma` 等           |
| 特权访问限制      | EL、安全状态与映射属性  | Privilege Mode、PMP 与相关机制 |

注意，RISC-V 的 PMA、PBMT、PMP 不是同一类机制；也不能把 RISC-V PTE 的 U 位直接等同于 Arm 的全部 EL 和 Security State 概念。

### **关键设计原则**

对于 Core Memory：

- 代码应尽可能采用 Read/Execute。
- 只读数据应采用 Read-Only。
- 可写数据应采用 Read/Write、不可执行。
- Page Table Memory 不应无故具备执行权限。
- Shared Memory 不应因为可由 Linux 写入就被赋予 Core Execute Permission。

但是正如前面提到的，必须检查实际配置是否实现了相应的权限拆分。

**安全建议和代码当前行为，需要明确区分。**

## **10.8.3 为什么 Cache Attribute Alias 很危险？**

假设同一 PA：

```
PA = 0x9000_1000
```

同时被映射为：

```
VA1 → PA
Attribute: Normal Cacheable
```

以及：

```
VA2 → PA
Attribute: Device / Non-Cacheable
```

这样可能破坏体系结构对别名映射和一致性的要求，并导致不可预测的可见性和访问行为。

更复杂的情况是 DMA：

```
CPU Cache
    |
    v
Secure DDR
    ^
    |
Device DMA
```

如果设备不与 CPU Cache 硬件一致，那么需要由软件在正确的 Ownership Transition 位置执行 Cache Maintenance。

这个问题在 Boot 阶段同样存在。

例如：

- Firmware 加载镜像后是否清理了必要的 Cache？
- OP-TEE 修改页表后是否保证页表写入对 Page Table Walker 可见？
- 新映射的可执行代码是否需要指令缓存同步？
- 多核情况下其他 CPU 是否持有旧的 Translation State？

这些问题与 Chapter 20 的 MMU 实现以及 Chapter 45 的 DMA/SMMPT 会直接衔接。

# **10.9 一个完整的源码级场景分析**

现在我们把前面的内容串联起来。

## **场景：Boot Firmware 把 OP-TEE 加载进安全 DDR**

假设：

```
Secure DDR:
    0x9000_0000 - 0x91FF_FFFF

Core Load PA:
    0x9000_0000

Core Runtime VA:
    0xFFFF_0000_1000_0000
```

这三个范围是假设的教学配置。

### **Step 1：Firmware 完成内存保护**

首先由可信 Boot Chain 确认：

```
OP-TEE Image Authentication: PASS
Secure DDR Attribution:     Configured
Core Entry Address:         Valid
```

如果没有建立真正的 Secure Physical Isolation，后续软件 MMU 配置不能单独补救整个安全边界。

### **Step 2：进入 Early Assembly**

CPU 建立：

```
Temporary Stack
Global Pointer / Core-local Pointer
Exception/Trap Prerequisites
```

并处理 `.bss` 等必要初始化。

此时不能依赖完整的 Runtime Thread、TA 或普通 Heap 机制。

### **Step 3：初始化 Boot Allocator**

以安全且可访问的内存为基础，建立 Boot Allocation State：

```
Free Boot Memory
    |
    +-- Permanent Boot Allocations
    |
    +-- Temporary Boot Allocations
```

### **Step 4：构造 Memory Map**

调用：

```
core_init_mmu_map(seed, &boot_mmu_config);
```

完成：

```
Physical Regions
     |
     v
Memory Mapping Description
     |
     v
Virtual Address Assignment
     |
     v
Translation Table
```

其中会保留必要的地址转换过渡映射。

### **Step 5：切换到 Runtime VA**

完成需要的 Relocation 后：

```
Current PC / Stack
       |
       v
MMU Transition
       |
       v
Runtime VA Execution
```

重点是保证 PC、SP、核心指针、页表和当前执行权限之间保持一致。

### **Step 6：初始化 Runtime Heap**

通过：

```
malloc_add_pool(...);
```

初始化后续 C Runtime 可以使用的 Heap。

### **Step 7：初始化 Physical Memory Pool**

通过：

```
core_mmu_init_phys_mem();
```

建立物理内存资源管理体系，并保留已经被 Core 和特殊用途占用的区域。

### **Step 8：完成 Boot Allocator 交接**

通过：

```
core_mmu_save_mem_map();
boot_mem_release_unused();
```

将长期使用的 Memory Metadata 与临时 Boot Allocation 解耦，并处理可回收内存。

### **Step 9：初始化其他可选 Memory Manager**

根据配置：

```
Pager
Dynamic VA Allocator
Virtualization-specific Allocator
```

分别完成其自身初始化。

### **Step 10：系统进入正常 Runtime**

Thread、TA Runtime、Device Driver 等开始在已经建立的内存管理基础上运行。

到这里，Boot-time Memory Initialization 的核心职责才算完成。

# **10.10 如何分析 Boot Memory 修改的影响范围？**

这一节直接对应本课程的最终目标：维护和修改 OP-TEE Core。

假设你需要扩大 Core 的 Heap Size。

直觉上可能只需要修改：

```
CFG_CORE_HEAP_SIZE = ...
```

但实际上应该做完整的影响分析。

| 检查方向               | 需要回答的问题                                |
| ------------------ | -------------------------------------- |
| Physical Layout    | 增加 Heap 是否挤占 TA RAM 或其他 Secure Memory？ |
| Linker Script      | Section Layout、Alignment 和断言是否仍成立？     |
| Initial MMU Map    | 是否需要新增页表项或扩大 Mapping？                  |
| Boot Allocator     | Boot 阶段的可用空间是否被挤占？                     |
| Physical Allocator | 新增 Heap 是否可能被错误地重复分配？                  |
| Security           | 新区域的 RW、NX 和 Secure 属性是否正确？            |
| Pager              | 是否影响 Resident Page Pool 或初始化区域？        |
| SMP                | Per-CPU 数据和 Stack 是否发生布局变化？            |
| ASLR               | Relocation 与 VA Placement 是否仍有效？       |
| Testing            | 哪些配置组合必须重新验证？                          |

这里最容易忽视的是 **修改后的系统可能仍然可以启动，但其资源管理不变量已经被破坏**。

例如：

```
Physical Page P
    |
    +-- Used by Core Heap
    |
    +-- Also allocated to User TA
```

如果没有对物理页面进行完整 Ownership Analysis，单纯观察 Boot Log 是无法证明不存在这种问题的。

## **10.10.1 本章需要掌握的安全不变量**

建议把下面这些规则记录到自己的 OP-TEE Invariant Notebook 中。

| Invariant                | 要求                                   |
| ------------------------ | ------------------------------------ |
| Physical Isolation       | 不可信主体不能越权访问安全物理内存                    |
| Mapping Validity         | 每项 VA Mapping 必须对应允许访问的 PA           |
| No Conflicting Ownership | 同一物理页不能被无约束地重复分配                     |
| Correct Attributes       | 映射的 Cache/Memory Type 必须与使用目的和架构要求一致 |
| Executable Integrity     | 可执行映射只能承载已获授权的代码                     |
| Bootstrap Safety         | 早期分配器不得访问尚不可用的资源                     |
| Lifetime Safety          | 释放 Boot 内存前不得存在未处理的有效引用              |
| SMP Consistency          | 其他 CPU 不能继续使用失效的转换或内存状态              |
| Failure Containment      | 关键初始化失败必须安全终止或进入明确受限状态               |

**理解这些不变量，比记住所有初始化函数的名称更重要。**

# **10.11 Hands-on Lab：建立 OP-TEE Boot Memory Map**

本章建议进行三个实验。

## **Lab 1：从 ELF 恢复真实 Memory Layout**

选择已经构建成功的 OP-TEE `tee.elf`，执行：

```
readelf -S tee.elf
readelf -l tee.elf

nm -n tee.elf | grep -E \
  '__heap|__nozi|__init|__pageable|__vcore'
```

分析以下问题：

- Linker Script 中哪些 Section 占用镜像文件空间？
- 哪些是 `NOLOAD`，只在运行时占用内存？
- `.bss` 和 `.nozi` 的初始化策略为什么不同？
- Heap 与 Core Code 的地址范围是否重叠？
- 这些地址是 Linker VMA，还是最终的 Runtime VA？

最后绘制：

```
ELF VMA Layout
       |
       v
Boot Load PA Layout
       |
       v
Runtime VA Layout
```

特别注意启用 ASLR 时，静态 ELF 地址与运行时地址可能不同。

## **Lab 2：追踪 Boot Allocator 生命周期**

在 `boot_mem.c` 的关键函数添加调试信息：

```
boot_mem_init
boot_mem_alloc
boot_mem_alloc_tmp
boot_mem_release_unused
boot_mem_release_tmp_alloc
```

记录：

```
Allocation Size
Alignment
Returned VA
Current Free Range
```

然后尝试回答：

**为什么临时分配的对象不能在&#x20;**`boot_mem_release_tmp_alloc()`**&#x20;之后继续被引用？**

进一步使用调用关系分析，找出哪些临时对象需要在释放前被复制到新的管理内存中。

## **Lab 3：对照 RISC-V 的初始页表**

在 RISC-V 版本上重点观察：

```
core_init_mmu_map()
       |
       v
Architecture MMU Backend
       |
       v
satp
       |
       v
Page Table Walk
```

使用 GDB 检查：

```
info registers pc sp
p/x boot_mmu_config
x/16gx <page_table_address>
```

最后一条命令中的地址需要替换为实际页表地址。

如果目标是 RV64 Sv39，应当从 `satp.PPN` 找到 Root Page Table，再手动完成至少一个 VA→PA 的三级页表遍历。

但实验不能止步于找到正确的 PA。

还需要检查：

- PTE 的 R/W/X/U 位。
- 是否存在多余的 Identity Mapping。
- 是否存在不应该允许 User Mode 访问的页面。
- 是否与平台提供的 Physical Memory Protection 一致。
- 是否有可能出现不同 VA 对同一 PA 的冲突属性映射。

**实验成功标准：** 能用 ELF、Memory Map、Page Table、CSR 和实际访问结果互相验证同一块物理内存的状态。

# **10.12 Staff Engineer Level — 深度面试与架构思考题**

下面准备 12 道题目。

它们并不以记忆函数名为核心，而是要求结合系统设计、安全隔离、并发、源码修改和真实工业故障进行推理。

建议先独立回答，再展开参考分析。

Staff Engineer Assessment

7 / 12

## **Q7 — 两个 Hart 同时执行 Boot 初始化会怎样？**

Primary/Secondary Hart Boot Race 导致两个 Hart 都调用 phys_mem_init()，其中一个偶发 Panic。仅增加全局 Spinlock 是否足以修复？

显示参考分析

&#x20;上一题下一题&#x20;

## **面试题应该如何评分？**

这些问题没有必要要求和参考答案完全一致。

我建议采用以下方式评估：

| 维度                     | 权重  | 重点               |
| ---------------------- | --- | ---------------- |
| Architecture Reasoning | 30% | 是否真正理解设计约束与依赖关系  |
| Root Cause Analysis    | 25% | 能否提出可验证的假设和排查路径  |
| Security Invariants    | 20% | 是否能识别真正的安全边界     |
| Engineering Trade-offs | 15% | 是否能解释性能、复杂度与安全取舍 |
| Verification Strategy  | 10% | 能否设计具有证明力的测试     |

Staff Engineer 不一定需要立刻记住所有具体函数。

但是应该能够根据系统事实提出合理假设，并知道如何通过源码、实验和安全不变量验证这些假设。

例如，Q4 的高质量回答不应该只是：

“检查 `satp`、Page Table、执行 `sfence.vma`。”

而应该解释：

**在 MMU 切换的准确时刻，硬件实际使用什么地址、哪个 Translation Root、哪些已写入的 PTE，以及为什么下一条指令应该能被正确翻译和执行。**

这才是真正的深度。

# **十一、本章总结：需要真正理解的五个设计思想**

完成 Chapter 10 后，应该建立以下认识。

**第一，Boot Memory Initialization 是一项资源管理权交接过程。**

它要将 Firmware 提供的安全物理内存，逐步转化为 OP-TEE Core 可以安全使用和分配的内存资源。

**第二，Physical Ownership、VA Mapping、Memory Allocation 是完全不同的概念。**

CPU 可以映射一块 PA，并不意味着这块 PA 属于某个 Allocator，更不意味着其他总线 Master 无法访问它。

**第三，Boot Allocator 是为了解决内存管理自举问题。**

它不需要像 Runtime Allocator 那么通用，但必须具备明确的生命周期和足够可靠的地址管理能力。

**第四，MMU Enable 不只是写一个寄存器。**

它涉及地址过渡、Translation Tables、Relocation、Stack、Global Pointers 和缓存/同步状态的一致性。

**第五，Memory Initialization 的正确性需要跨越整个系统来证明。**

一个正确的 Memory Map 需要同时满足：

```
Correct Physical Protection
          +
Correct Resource Ownership
          +
Correct Virtual Mapping
          +
Correct Memory Attributes
          +
Correct Lifetime
          +
Correct Synchronization
          =
Safe Boot Memory State
```

这不是一个简单的数学等式，而是对架构验证条件的归纳。

## **下一章的连接点**

Chapter 11 将讨论 SMP Boot 和 Per-CPU Kernel State。

这意味着我们需要把本章建立的内存初始化模型进一步扩展：

**当一个 CPU 已经完成内存初始化，其他 CPU 何时可以安全地使用这些内存状态？**

这将涉及：

- Page Table 的跨核共享。
- Per-CPU Stack 和 Core-local Memory。
- Bootstrap Synchronization。
- Memory Ordering。
- TLB Shootdown。
- Secondary CPU 的初始化与恢复。

本章理解得越深入，后续分析这些 SMP 问题时就越不会局限于单个函数或 CSR。

**我建议 Chapter 10 最重要的学习成果，是能够独立重建一次完整的&#x20;**`boot_mem → MMU Map → Runtime Heap → Physical Memory Pool`**&#x20;的资源交接过程，并证明整个交接期间不存在重叠分配、失效引用和权限漏洞。**

如果能够做到这一点，对 OP-TEE Boot-time Memory 的理解就已经超出了普通 Porting 层面，进入了 Core Architecture 和 Maintainer 所需要的深度。
