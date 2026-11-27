# **Chapter 08 — OP-TEE Early Runtime、Stack 与 C Execution Environment**

OP-TEE OS Internals & Architecture · PART II: Boot & Initialization

这一章研究的是 OP-TEE 启动过程中非常关键、但也非常容易被忽略的一个阶段：

**CPU 已经跳转到 OP-TEE 的入口地址，但 OP-TEE 尚未拥有一个完整、可靠的 C 语言运行环境。**

我们要回答的不是简单的“如何设置 SP、清空 BSS、调用 main 函数”，而是：

> 在 MMU、内存分配器、异常处理、全局变量、线程系统都尚未完全初始化的情况下，OP-TEE 如何一步步建立一个可以安全执行普通 C 代码的运行环境？每一步依赖什么前提？如果改变执行顺序，会发生什么？

理解这部分后，你应该能够直接分析 Early Boot Assembly、Linker Script、Runtime Relocation，以及各种发生在第一条 Debug Log 出现之前的异常。

**源码基线：** OP-TEE 4.11.0（2026-10-16），主要以 AArch64 和 RV64 为例。

## **8.1 本章学习目标和知识地图**

本章覆盖原定 Chapter 08 的全部主题，并增加几个直接影响实际移植正确性的专题。

| 章节   | 核心内容                         | 需要建立的能力                          |
| ---- | ---------------------------- | -------------------------------- |
| 8.2  | Early Runtime 的本质            | 理解 Boot Contract 和初始化依赖          |
| 8.3  | ELF、Linker 与内存段              | 判断 `.data`、`.bss`、`.nozi` 的初始化责任 |
| 8.4  | Boot Stack 与 CPU-local State | 理解 SP、Per-CPU 数据和栈生命周期           |
| 8.5  | AArch64 Entry 源码             | 从汇编追踪到 C Runtime                 |
| 8.6  | RISC-V Entry 源码              | 掌握 `gp/tp/sp`、Hart 和早期初始化        |
| 8.7  | Relocation 与 MMU Transition  | 理解地址空间切换的一致性                     |
| 8.8  | Exception、Interrupt、Console  | 设计早期故障诊断和保护机制                    |
| 8.9  | 从 Early C 到 Kernel Runtime   | 分辨哪些内核服务何时可用                     |
| 8.10 | Boot 安全不变量与修改影响              | 形成 Staff Engineer 级设计能力          |
| 8.11 | GDB 与故障注入实验                  | 验证真实执行状态                         |
| 8.12 | Staff Engineer 综合面试题         | 考察跨模块推理和设计能力                     |

### **核心思维模型：Early Boot 是一个逐步建立依赖的过程**

**S0 — Firmware Handoff**

PC 有效、执行权限有效、镜像可访问

**S1 — Minimal Assembly Runtime**

保存入口参数、早期异常保护、基础寄存器

**S2 — Basic C Runtime**

数据段有效、临时栈、ABI、基本 CPU-local State

**S3 — Virtual-Memory Runtime**

MMU、Relocation、有效 VA、地址引用修正

**S4 — Kernel Runtime**

正式 Thread、Heap、Interrupt、Driver、Kernel Services

这是便于理解的依赖阶段，不是 OP-TEE 源码中的正式状态枚举；部分步骤可以在不同架构上交错进行。

其中最重要的一点是：

**能执行某个 C 函数，不意味着全部 C Runtime、Kernel Runtime 都已经可用。**

例如，`boot_mem_init()` 可以在完整 MMU 初始化之前调用；但是它能使用的内存服务、地址表达方式及其执行约束，都不同于 OP-TEE 正式运行时的普通 C 函数。

# **8.2 Early Runtime 的本质：究竟要初始化什么？**

## **8.2.1 C 语言为什么不能在 CPU Entry 后立即可靠执行？**

从 C 语言开发者角度来看，一段代码可能非常简单：

```
static int boot_count;
static int config_value = 42;

void early_init(void)
{
    int local = 10;

    boot_count++;
    config_value += local;
}
```

但是编译后的机器代码隐含了很多依赖。

首先，`local` 可能需要 Stack Space。

其次，`boot_count` 在程序启动时必须为零，而 `config_value` 必须已经被初始化为 42。

再次，编译器必须能够正确生成访问 Global Variable 的机器指令；这些指令可能依赖 PC-relative Addressing、GOT，或者 RISC-V `gp`。

最后，编译器生成的函数 Prologue/Epilogue 必须满足 ABI。若启用了 Stack Protector，还可能读取 `__stack_chk_guard`。

因此，调用这个 C 函数之前，至少必须满足以下条件：

| 运行环境要素                | 依赖条件                            | 不满足时的风险                |
| --------------------- | ------------------------------- | ---------------------- |
| Instruction Fetch     | Code 位于有效可执行地址                  | Instruction Fault      |
| Global Data           | 已初始化数据的值正确                      | 错误配置、错误指针              |
| Zero-initialized Data | BSS 已清零                         | 状态错误、未初始化指针            |
| Stack                 | SP 有效、有足够空间且对齐                  | Stack Corruption、Abort |
| Calling Convention    | 寄存器及 ABI 约束正确                   | 返回地址、参数损坏              |
| Address Resolution    | 地址引用与当前执行位置一致                   | 跳入错误地址                 |
| CPU Privileged State  | 当前代码需要的 CSR/System Register 可访问 | Illegal Instruction    |
| Exception State       | 早期异常不会误入未初始化 Handler            | Double Fault、死循环       |
| Compiler Runtime      | 所使用的编译器辅助机制已准备好                 | 隐藏的运行时依赖               |

注意这里的措辞是“所使用的编译器辅助机制已准备好”，而不是要求所有 Runtime Service 都必须初始化。

Early Boot C 可以使用非常有限的运行时设施，前提是严格控制其依赖。

## **8.2.2 Boot Firmware 和 OP-TEE 分别负责什么？**

这是第一个需要建立的架构边界。

假设一个典型 AArch64 平台采用：

```
Boot ROM
   |
   v
TF-A
   |
   v
OP-TEE Entry
```

TF-A 完成安全世界入口的准备工作，OP-TEE 再建立自身运行环境。

但具体责任不是完全固定的。

例如，镜像可能由 Bootloader 已经复制到正确 RAM 地址；某些平台则支持物理地址重定位。上一级 Firmware 可能已经设置部分系统寄存器，但 OP-TEE 仍需要建立符合自身预期的执行状态。

因此，Boot Contract 至少应明确：

| 要素                 | 必须明确的问题                          |
| ------------------ | -------------------------------- |
| Execution Mode     | 从哪个 EL/Privilege Mode 进入 OP-TEE？ |
| Entry Address      | PC 是物理入口、Identity VA，还是已有虚拟映射？   |
| Image Layout       | 镜像按什么地址布局加载？                     |
| Memory Security    | 哪些 RAM 已经受到硬件隔离保护？               |
| MMU/Cache          | 入口时的 MMU、I/D-Cache 状态是什么？        |
| Register Arguments | 哪些寄存器携带 DTB、Boot Parameters？     |
| CPU State          | Secondary CPU 是否可能同时进入？          |
| Interrupt State    | 哪些中断可能在初始化期间出现？                  |

对于 RISC-V，额外需要确定 OP-TEE 当前运行在 M-mode 还是 S-mode，以及相应的安全 Domain、PMP/系统级保护是否已经建立。

**运行在 S-mode 并不自动代表内存受保护。** 这是硬件隔离与软件初始化两个不同维度的问题。

## **8.2.3 为什么不能简单调用&#x20;**`main()`**？**

传统 C Runtime 通常包含类似这样的逻辑：

```
Reset / Entry
     |
     v
Initialize data sections
     |
     v
Clear BSS
     |
     v
Set stack
     |
     v
Run runtime initialization
     |
     v
main()
```

而 OP-TEE 的真实启动流程复杂得多。

它不仅需要 C Runtime，还需要处理：

- 可选的 Pager 与初始化代码搬移；
- Core 的物理加载位置和虚拟运行位置；
- Core ASLR；
- CPU-local Stack 与 Thread State；
- Identity Mapping 到正式 MMU Mapping 的切换；
- Secure Exception 和 Interrupt 的初始化；
- 后续从 Boot Thread 转换为正式 Trusted Thread Runtime。

所以，OP-TEE 没有必要模仿一个普通 Bare-metal 程序中的单一 `crt0 → main()` 模型。

真正的目标是建立一个**可以证明每一步执行前提都成立的初始化过程**。

# **8.3 ELF、Linker 与 Early Memory Layout**

要理解 Early Runtime，必须先理解 Linker 为它提供了什么。

## **8.3.1 C 源码并不能决定自己被加载到哪里**

考虑下面的全局变量：

```
int global_initialized = 100;
int global_zero;
```

从 C 语言语义看：

`global_initialized` 在程序启动时必须为 100，`global_zero` 必须为 0。

但这并不意味着 CPU 会自动完成这些工作。

Linker 负责决定它们在最终镜像中的地址与 Section，镜像加载方和 Runtime 初始化代码负责使实际内存状态符合这些语义。

这也是为什么 Entry Assembly 必须与 Linker Script 严格匹配。

## **8.3.2&#x20;**`.text`**、**`.rodata`**、**`.data`**、**`.bss`**、**`.nozi`

| Section          | 内容                   | 启动阶段关注点         |
| ---------------- | -------------------- | --------------- |
| `.text`          | 可执行机器代码              | 加载位置、执行权限       |
| `.rodata`        | 常量、字符串、只读表           | 内容正确、地址正确       |
| `.data`          | 带初值的全局/静态变量          | 必须有正确初始值        |
| `.bss`           | 零初始化全局/静态变量          | 必须在使用前清零        |
| `.nozi`          | 特意不进行自动清零的存储区域       | 由使用者维护有效性       |
| `.identity_map*` | 早期需要恒等映射的代码/数据       | MMU 切换前后必须可访问   |
| `.init*`         | 初始化阶段使用的代码/数据        | 加载、搬移及生命周期      |
| `.pageable*`     | 可以参与 Paging 的代码/只读数据 | Pager 准备前不能随意依赖 |

实际 Section 名称和组织会受架构、Linker Script 及配置影响。

OP-TEE 官方文档明确区分 `unpaged`、`init/paged`、`paged` 和 `demand alloc` 等区域，并指出 `.nozi` 可用于栈和页表等不需要按普通 BSS 规则初始化的存储。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### **为什么&#x20;**`.nozi`**&#x20;很重要？**

假设 Linker 在 Secure RAM 中保留：

```
0x8000_0000  .text
0x8004_0000  .data
0x8004_8000  .bss
0x8005_0000  .nozi.stack
0x8005_8000  .nozi.page_table
```

这些地址仅用于教学示意。

如果 `.nozi.stack` 被错误地合并到一个启动期间会重复清零的区域，那么某个 CPU 已经在使用的 Stack 可能突然被清空。

类似地，如果一个用于早期同步的变量错误放在 `.bss`，而多个 CPU 在 BSS 清零之前就读取或修改它，初始化时序可能出现致命错误。

这也是后面研究 RISC-V Hart Lottery 时会遇到的实际问题。

## **8.3.3&#x20;**`.data`**&#x20;必须初始化，但不一定需要 OP-TEE 自己复制**

这里需要纠正一种常见误解：

“进入 C 之前必须执行一次 `.data` 从 Flash 到 RAM 的复制。”

对于直接从 Flash 执行、数据必须驻留 RAM 的 MCU，这可能是实际流程。

但对于由 Firmware Loader 加载的 OP-TEE ELF/Image，**初始化的&#x20;**`.data`**&#x20;可能已经被加载到了正确位置**。

这时再执行一遍传统 MCU 式的 `.data` Copy 并没有意义，甚至可能破坏地址布局。

在 OP-TEE AArch64 4.11.0 的 `entry_a64.S` 中，可以看到 Core Image 已加载部分和 Pager Init Code/Embedded Data 的搬移逻辑，并没有一个适用于所有配置的通用 Flash-to-RAM `.data` 初始化循环。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此必须区分：

**ELF 的加载职责** 与 **运行时额外搬移或初始化的职责**。

这是分析 Bootloader、Linker、OP-TEE 之间责任边界的重要基础。

# **8.4 Boot Stack、Temporary Stack 与 CPU-local State**

这是本章第一个需要深入理解的核心问题。

## **8.4.1 为什么 OP-TEE 不直接使用一个固定 Stack？**

从普通 Bare-metal 系统的角度，启动时只需要：

```
/* 教学示意：RV64 */
la sp, __stack_top
call kernel_main
```

但 OP-TEE 的执行环境明显更加复杂。

它需要支持不同 CPU 并行进入 Secure World，需要处理异常，还需要暂停 Trusted Thread、返回 Normal World，随后恢复执行。

因此，OP-TEE 会使用多种具有不同职责和生命周期的栈。

| Stack 类型                | 主要用途                | 典型所有权        |
| ----------------------- | ------------------- | ------------ |
| Boot / Temporary Stack  | 早期 Boot、普通入口与退出     | Per-CPU      |
| Abort / Exception Stack | 处理特定异常              | Per-CPU      |
| Trusted Thread Stack    | 执行 Secure Kernel 工作 | Per-Thread   |
| User TA Stack           | 执行用户态 TA            | Per-TA 执行上下文 |

这里有一个关键区分：

**Per-CPU Stack 与 Per-Thread Stack 不是同一个概念。**

CPU 是硬件执行资源。Thread 是软件执行上下文。

一个 Trusted Thread 在 RPC 后被暂停，后续可能在不同 CPU 上恢复，因此它的执行状态不能简单绑定到某个 CPU 的临时栈上。

## **8.4.2 AArch64 为什么同时使用 SP_EL0 和 SP_EL1？**

AArch64 OP-TEE 的栈模型非常值得研究。

在典型 S-EL1 实现中：

- `SP_EL1` 用作异常入口所需的 CPU-local 数据结构指针。
- `SP_EL0` 则可以作为当前 EL1 代码实际使用的栈指针。
- `SPSel` 决定当前指令使用哪个 SP。

这不是说 OP-TEE Kernel 在 EL0 运行。**使用&#x20;**`SP_EL0`**&#x20;不会改变当前异常级别。**

例如：

```
msr spsel, #1
mov sp, x1

msr spsel, #0
mov sp, x0
```

这是从真实入口代码中抽出的一个简化片段。

其含义是：

| 指令              | 作用                   |
| --------------- | -------------------- |
| `msr spsel, #1` | 使当前 `sp` 指向 `SP_EL1` |
| `mov sp, x1`    | 设置 `SP_EL1`          |
| `msr spsel, #0` | 使当前 `sp` 指向 `SP_EL0` |
| `mov sp, x0`    | 设置 `SP_EL0`          |

OP-TEE 的 AArch64 实现中，`SP_EL1` 与 `thread_core_local` 关联，`SP_EL0` 可用于临时栈或者活动 Trusted Thread 栈。官方文档对此有明确描述。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



可以把它理解为：

AArch64 OP-TEE CPU Stack Model

SP_EL1

thread_core_local

Current Thread ID

Temporary Stack VA

Abort Stack VA

CPU-local Flags

SP_EL0

Active Stack

Boot Temporary Stack

或 Trusted Thread Stack

由入口代码切换

示意的是 AArch64 OP-TEE 的内核栈约定，而不是处理器对 SP_EL0/SP_EL1 的通用强制用途。

这说明 `thread_core_local` 绝不只是一个记录 CPU ID 的小结构体。

它直接参与异常入口、Stack Selection、Thread State Management。

所以，在修改相关结构体字段或栈布局时，需要同步检查 Assembly 中使用的字段 Offset。

## **8.4.3&#x20;**`CFG_DYN_CONFIG`**&#x20;为什么让 Boot Stack 更复杂？**

在 OP-TEE 4.11.0 AArch64 入口中，启用 `CFG_DYN_CONFIG` 后，会先从 Boot 阶段可用的内存中预留空间，用于临时栈和临时 `thread_core_local`。

源码会初始化：

```
thread_core_local
    curr_thread = THREAD_ID_INVALID
    flags       = THREAD_CLF_TMP
    tmp_stack_va_end = ...
    abt_stack_va_end = ...
```

随后将初始 CPU-local 信息登记下来。

而没有启用该配置时，入口采用 `set_sp` 等预定义汇编宏建立对应的 CPU-local 栈环境。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



为什么会有这个差异？

因为动态配置下，某些最终资源规模和布局需要在 Boot 中进一步确定。

这意味着早期阶段会存在一个临时的 CPU-local 状态，之后需要切换到正式状态。

**所以，不仅 Stack 本身存在生命周期，记录 Stack 的元数据也存在生命周期。**

这是后续分析 ASLR 和 Per-CPU State Relocation 时非常重要的前提。

# **8.5 AArch64 Entry：从第一条汇编到 C Runtime**

我们现在开始对照真实源码。

源码：[entry_a64.S — OP-TEE 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/entry_a64.S?utm_source=chatgpt.com)

## **8.5.1 总体执行路径**

对于经典的 AArch64 Primary Entry，我们可以将主要流程概括为：

```
_start
  |
  +-- Save boot argument registers
  |
  +-- Install early exception vector
  |
  +-- Initialize selected CPU control state
  |
  +-- Move boot/init image components if required
  |
  +-- Clear .bss
  |
  +-- Apply optional physical relocation
  |
  +-- Prepare temporary stack / core-local state
  |
  +-- Cache maintenance
  |
  +-- console_init()
  |
  +-- boot_save_args()
  |
  +-- boot_mem_init()
  |
  +-- core_init_mmu_map()
  |
  +-- Apply optional virtual relocation
  |
  +-- enable_mmu()
  |
  +-- Fix VA-dependent boot state
  |
  +-- boot_init_primary_early()
  |
  +-- boot_init_primary_late()
  |
  +-- Switch to boot thread stack
  |
  +-- boot_init_primary_runtime()
  |
  +-- boot_init_primary_final()
  |
  +-- Handoff / exit boot path
```

这是根据 `entry_a64.S` 整理的逻辑路径。具体配置可能增加、移除或重新安排部分操作，例如 FF-A、Virtualization、MEMTAG 和动态配置。

请注意：在这个流程中，**普通 C 函数的调用早于完整 Kernel Runtime 初始化**，例如 `console_init()` 和 `boot_mem_init()`。

## **8.5.2 第一阶段：保存 Boot Arguments**

入口首先保存 `x0–x3` 中的 Boot Arguments。

在真实代码中，使用 `x19–x22` 作为暂存寄存器，稍后再传递给 `boot_save_args()`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



为什么不立即调用 `boot_save_args()`？

因为此时栈和运行环境还没有完整建立。

另外，普通函数调用可能使用 `x0–x18` 中的 Caller-Saved Registers。

如果未按调用约定保存原始 Boot Arguments，经过多个函数调用之后，它们可能已不再有效。

这里的架构原则是：

> 跨越 Boot 阶段的数据必须拥有明确、受保护的存储位置，不能假设入口寄存器会永久保持不变。

## **8.5.3 第二阶段：安装 Early Exception Vector**

接下来，AArch64 入口设置：

```
adr x0, reset_vect_table
msr vbar_el1, x0
isb
```

这段代码有三个重要含义。

**第一，**`adr`**&#x20;使用 PC-relative Addressing。**

Early Boot 时不能随意假设最终的 VA Mapping 已经建立，所以能够在当前执行地址下解析的地址生成方式非常重要。

**第二，设置 VBAR_EL1。**

如果初始化过程中出现 Synchronous Exception，CPU 至少需要知道进入哪个异常向量。

**第三，执行 ISB。**

更新异常向量配置后，应确保后续指令在新的指令执行上下文中观察到这个变化。

但这里不能误以为：

“设置 VBAR_EL1 后，OP-TEE 已经具备完整异常恢复能力。”

实际上，`reset_vect_table` 中部分早期异常入口采用原地循环的方式，目的是避免跳入尚未建立的 Runtime Handler。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这在早期启动中非常合理。

异常向量可用，不代表异常可以被安全恢复。

## **8.5.4 第三阶段：搬移必要的初始化代码**

启用 Pager 的情况下，OP-TEE Boot Image 存在特殊的 Init/Paged 组织方式。

部分 Init Code 和 Embedded Data 需要在进入完整运行环境之前搬移到适当位置。

源码中的搬移操作会考虑 Source/Destination 重叠的情况，使用从尾部向前复制的方式处理。

为什么要这么做？

假设：

```
Source:       [---- Init Data ----]
Destination:      [---- Init Data ----]
```

如果采用不考虑重叠的前向复制，就可能在尚未读取原始数据之前覆盖 Source。

因此，一段看似普通的内存复制，在 Early Boot 阶段实际上有三个约束：

**Address Correctness、Overlap Safety 和 No Runtime Dependency。**

这部分逻辑与最终 Pager 初始化有关，后续 Chapter 24 会进一步分析。

## **8.5.5 第四阶段：清零&#x20;**`.bss`

AArch64 的 `.bss` 清理逻辑可以简化理解为：

```
/* 教学示意，与真实入口的地址计算思想一致 */
start = address_of(__bss_start)
end   = address_of(__bss_end)

while (start < end) {
    store_64bit_zero(start);
    start += 8;
}
```

实际入口使用 `str xzr` 完成写零操作，并依赖 Linker 保证 BSS 边界至少按照 8 字节对齐。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有一个很容易被忽略的事实：

**BSS 清零本身并不需要 Stack。**

这段实现仅使用寄存器、内存写入和条件分支。

因此，它完全可以安排在正式 SP 初始化之前。

不过，这个实现同时建立了三个隐含约束。

| 约束                     | 如果被破坏         |
| ---------------------- | ------------- |
| `__bss_start/end` 必须正确 | 清空错误内存区域      |
| BSS 边界必须满足粒度对齐         | 越界写或遗漏清零      |
| 清零期间不能有其他 CPU 使用该区域    | SMP Race、状态损坏 |

第三个约束尤其重要。

它不是普通的程序正确性问题，而是 Boot Synchronization 的一部分。

## **8.5.6 第五阶段：建立临时 Stack**

在完成必要的早期数据处理后，入口设置 CPU-local 状态和临时栈。

此时 OP-TEE 才逐步获得了执行更复杂 C 函数的能力。

但是请注意，这个 Stack 仍然不是最终 Trusted Thread Stack。

此时的环境更接近：

```
CPU 0
  |
  +-- Temporary Stack
  |
  +-- Temporary CPU-local State
  |
  +-- No ordinary TA context
  |
  +-- No fully initialized Thread Runtime
```

这个阶段可以执行符合约束的 C 代码，但不意味着能够使用所有内核 API。

## **8.5.7 第六阶段：Cache Maintenance 与 Console**

AArch64 源码中，在调用 `console_init()` 前，会对初始化阶段使用的特定内存范围执行 Data Cache Clean/Invalidate。

源码特别提醒，不能无差别地处理不属于 OP-TEE 的内存，因为它可能仍由前级 Firmware 使用。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这是典型的跨 Firmware Ownership 问题。

假设 TF-A 与 OP-TEE 在启动阶段使用过同一个 Cache Coherency Domain。

那么：

- OP-TEE 可以维护自己拥有的相关内存；
- 但不能假设整片 RAM 都属于自己；
- 更不能因为 Boot 时尚未完成正式内存管理，就忽略 Cache Ownership。

这对于存在独立 HSM、SCP、RSS 或多个 Firmware Execution Domain 的 SoC 特别重要。

## **8.5.8 第七阶段：从 Boot C Runtime 进入正式 Runtime**

AArch64 后续会调用：

```
boot_init_primary_early()
boot_init_primary_late()
```

然后，在典型非虚拟化启动路径中，切换到 Boot Thread Stack，再调用：

```
boot_init_primary_runtime()
boot_init_primary_final()
```

源码中可以看到，从临时栈切换到 `threads[0].stack_va_end`，同时修改 `thread_core_local` 标志，再执行 Runtime 初始化。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这是一个十分关键的转变：

**Early Runtime 已经能够执行 C，但真正意义上的 OP-TEE Kernel Runtime，需要 Thread、Memory、Interrupt 和 Core Services 等设施逐步建立之后才算准备好。**

# **8.6 RISC-V Entry：**`gp`**、**`tp`**、**`sp`**&#x20;和 Hart 的初始化**

现在切换到 RISC-V。

源码：[RISC-V entry.S — OP-TEE 4.11.0](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/riscv/kernel/entry.S?utm_source=chatgpt.com)

RISC-V 的 Early Runtime 与 Arm 整体目标一致，但具体实现存在显著差异。

## **8.6.1&#x20;**`_start`**&#x20;的第一件重要事情：初始化&#x20;**`gp`

真实入口使用了这样的指令序列：

```
.option push
.option norelax
lla gp, __global_pointer$
.option pop
```

为什么要显式使用 `.option norelax`？

这是一个很值得深入的问题。

RISC-V Toolchain 支持 Linker Relaxation。编译器和链接器可能会利用 `gp` 优化某些全局数据访问。

但是初始化 `gp` 的代码本身不能依赖一个已经有效的 `gp`。

否则就会出现循环依赖：

```
初始化 gp
    |
    +-- 需要读取全局数据
             |
             +-- 访问全局数据依赖 gp
```

如果链接器错误地将初始化 `gp` 的指令转换成依赖现有 `gp` 的形式，启动将无法可靠执行。

因此，这段 `.option norelax` 不是单纯的性能优化设置，而是一个非常实际的 Boot Correctness 约束。

此外，RISC-V Core ASLR 启用后，`gp` 还可能需要在虚拟地址切换阶段根据新的 Mapping Offset 调整。

## **8.6.2&#x20;**`tp`**&#x20;在 RISC-V OP-TEE 中承担什么角色？**

熟悉 Linux 的工程师往往知道 `tp` 与 Thread Local Storage 有关系。

但 RISC-V OP-TEE 内核中的 `tp` 在这里承担了 CPU-local 状态寻址的重要职责。

在 4.11.0 的 Early Boot 路径中：

```
mv sp, t0
mv tp, t1
```

其中 `sp` 指向临时栈，`tp` 指向早期 `thread_core_local`。

这个结构中包括：

```
thread_core_local
    |
    +-- Current Thread ID
    +-- Temporary Stack VA
    +-- Abort Stack VA
    +-- Core-local Flags
    +-- Hart ID / Index
```

对应源码明确初始化 `THREAD_ID_INVALID`、`THREAD_CLF_TMP`、Hart ID 和 Hart Index 等字段。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这说明 `tp` 初始化错误并不一定立刻导致 Illegal Instruction。

更可能出现的是：

某个 CPU 成功执行了一段 Early Boot 代码，但稍后查询 CPU-local State 时访问错误地址。

例如，Stack Pointer 是对的，而 `tp` 指向了错误 CPU 的 `thread_core_local`。

此时可能产生非常隐蔽的跨 CPU 状态污染。

## **8.6.3 Primary Hart 是如何确定的？**

RISC-V 4.11.0 Entry 中采用 Hart Lottery 机制。

其核心指令之一是：

```
amoadd.w a3, a2, (a3)
```

简化逻辑如下：

```
/* 教学伪代码：原子更新 */
hart_index = atomic_fetch_add(&hart_lottery, 1);

if (hart_index == 0)
    reset_primary();
else
    reset_secondary();
```

这里需要特别注意两点。

第一，**Primary Hart 不一定是物理 Hart ID 为零的那个 Hart**。

这里由原子操作的返回值确定 Primary Boot Role。

第二，源码把得到的 Index 放到 `CSR_XSCRATCH` 中使用。它在此处承担的是启动期间的 Hart Index 角色，不能与真实 `mhartid` 混为一谈。

这一机制要求原子操作可用，而且多 Hart 访问该同步变量时具有正确的内存一致性和可见性。

## **8.6.4 为什么 Hart Lottery 变量不能随意放进 BSS？**

这是本章一个非常重要的 Staff Engineer 级问题。

注意执行顺序：

```
Initialize gp
    |
    v
Atomic Hart Lottery
    |
    v
Choose Primary / Secondary
    |
    v
Primary clears BSS
```

也就是说，Hart Lottery 发生在清理 BSS 之前。

如果把 `hart_lottery` 放在普通的 BSS 内，会出现什么风险？

一种可能是：

```
Hart 0: atomic_fetch_add() => 0
Hart 1: atomic_fetch_add() => 1

Hart 0: clear .bss

Hart 2: atomic_fetch_add() => 0
```

Hart 2 可能再次认为自己是 Primary。

实际 OP-TEE 4.11.0 将 `hart_lottery` 放在 `.identity_map.data`，而不是依赖尚未清零的普通 BSS。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



这个设计体现了 Boot Runtime 中特别重要的原则：

> 任何用于建立初始化顺序的状态，本身必须在该初始化顺序建立之前就已经有效。

不过，仅仅把变量放入 `.data` 还不够。Boot Contract 仍需保证该变量具有正确的初始值，并且多 Hart 可以一致地观察其原子更新。

## **8.6.5 Primary Hart 的 Early Runtime 初始化**

简化路径如下：

```
reset_primary
    |
    +-- Optional early relocation
    |
    +-- Clear BSS
    |
    +-- Initialize SATP state
    |
    +-- Initialize SP / TP
    |
    +-- Initialize thread_core_local
    |
    +-- boot_save_args()
    |
    +-- Optional SBI initialization
    |
    +-- plat_primary_init_early()
    |
    +-- console_init()
    |
    +-- boot_mem_init()
    |
    +-- core_init_mmu_map()
    |
    +-- Optional ASLR relocation
    |
    +-- enable_mmu()
    |
    +-- Fix relocated runtime pointers
    |
    +-- boot_init_primary_early()
```

这些操作可以在 `entry.S` 中逐一找到。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



其中 `csrw CSR_SATP, zero` 特别值得注意。

它为后续构建正式的地址空间建立一个明确的转换状态。但必须结合当前运行的 Privilege Mode 和前级 Firmware Contract 理解这一步；不能把“将 SATP 清零”当作适用于所有 RISC-V Firmware 的通用启动模板。

## **8.6.6 Hart ID、Hart Index、CPU-local State 为什么必须区分？**

假设一颗 SoC 的 Hart ID 为：

```
Hart ID: 4, 8, 12, 16
```

但软件只需要为四个 Hart 分配 CPU-local 数组：

```
struct thread_core_local cpu_state[4];
```

如果代码直接用 Hart ID 作为数组下标：

```
cpu_state[hartid]
```

就会立即产生越界访问。

因此，软件通常需要建立：

```
Hardware Hart ID
      |
      v
Software Hart Index
      |
      v
Per-CPU State
```

在 RISC-V 4.11.0 中，启动早期的 Lottery Index、DT 中的 Hart ID 列表以及正式 CPU-local 管理之间存在不同阶段的关联和初始化过程。

作为 Maintainer，我们要特别注意这三个值是否具有相同语义，以及什么时候完成映射。

不能只因为变量名字都包含 `hart`，就认为它们可以互换。

# **8.7 Relocation、Core ASLR 与 MMU 切换**

这是整章技术深度最高的部分之一。

## **8.7.1 必须区分三个地址概念**

在 Early Boot 中，至少要区分：

**Physical Load Address、Link-time Address 和 Runtime Virtual Address。**

假设：

| 概念                    | 示例                      |
| --------------------- | ----------------------- |
| Link-time VA          | `0xFFFF_0000_0000_0000` |
| Physical Load Address | `0x9000_0000`           |
| Runtime VA            | `0xFFFF_0000_0020_0000` |

这些是人为构造的示意地址，不代表某个具体 OP-TEE 平台。

如果发生 Core ASLR，可能存在：

\\[ \Delta\_{\text{VA}}=VA\_{\text{runtime}}-VA\_{\text{link}} \\]

在例子中，VA Offset 为 `0x200000`。

但是，不能由此得出“所有指针都加 0x200000”这个结论。

原因是系统中的地址值可能表示：

- 物理地址；
- 运行时虚拟地址；
- Linker Symbol；
- PC-relative Offset；
- MMIO Base；
- Identity-mapped Address；
- 尚未完成 Relocation 的数据指针。

这些对象需要采用不同处理方式。

## **8.7.2 什么是 Relocation？**

考虑这个程序：

```
extern int shared_state;
int *state_ptr = &shared_state;
```

如果 `state_ptr` 的初始值取决于最终加载地址或运行地址，Linker/Loader/Runtime 就需要正确处理这一引用。

一个典型的 Relative Relocation 可以抽象为：

\\[ \text{RelocatedValue}=\text{Addend}+\text{LoadBias} \\]

其中 `LoadBias` 的含义由 ELF 类型和具体 Relocation 处理约定决定。

RISC-V OP-TEE 的 `relocate()` 实现会遍历 Dynamic Relocation 信息，处理诸如 `R_RISCV_RELATIVE` 以及其他被支持的绝对地址 Relocation 类型。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



注意：Relocation 并不意味着盲目遍历所有内存中的指针并添加 Offset。

它是依据有明确语义的 Relocation Entries 更新指定位置。

## **8.7.3 物理重定位与虚拟地址随机化不是一回事**

AArch64 4.11.0 的启动代码中可以看到两类不同的处理需求：

首先，在配置允许物理加载地址变化时，计算实际 Load Address 相对编译预期 Load Address 的差异。

之后，启用 Core ASLR 时，再处理虚拟映射偏移导致的地址修正。

这两项操作是不同的问题。

| 机制                        | 要解决的问题                          |
| ------------------------- | ------------------------------- |
| Physical Relocation       | 镜像实际加载的 PA 与原本预期不同              |
| Virtual Relocation / ASLR | Core 最终执行的 VA 与 Link-time VA 不同 |

源码也明确要求某些 Relocation 必须在 MMU 启用前完成，因为后续部分内存可能被设置为只读。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



因此修改 Relocation 时，必须同时检查 Linker Layout 和最终 Memory Permission。

## **8.7.4 RISC-V 中 MMU 开启后为什么要更新&#x20;**`gp/tp/sp/ra`**？**

来看 4.11.0 RISC-V 的 `enable_mmu()` 逻辑。

核心步骤为：

```
Obtain boot_mmu_config
      |
      v
Load configured SATP value
      |
      v
Write SATP
      |
      v
Execute SFENCE.VMA
      |
      v
Apply ASLR offset to:
    gp
    tp
    sp
    ra
      |
      v
Return to relocated VA
```

这是一个非常好的 OS Architecture 案例。

假设 MMU 打开前：

```
sp = 0x9001_0000
```

MMU 打开后，原先的地址可能仅在早期 Identity Mapping 中有效，而正式 Stack 需要通过新的 VA 访问。

如果不更新 `sp`，下一次函数调用或者函数返回时就可能触发 Page Fault。

但仅修正 `sp` 还不够。

`tp` 指向的 CPU-local 数据、`gp` 相关的全局地址引用、`ra` 中的返回地址，也都可能需要与新的运行时 VA 保持一致。

真实源码正是这样修正这些寄存器。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## **8.7.5 为什么必须保留一段 Identity-mapped Code？**

假设我们执行：

```
csrw satp, a2
```

此时 CPU 开始使用新的地址转换配置。

问题是：执行这条指令之后，CPU 还要继续从当前 PC 获取指令。

如果新的页表没有覆盖当前正在执行的地址，CPU 可能立刻发生 Instruction Page Fault。

因此，早期 MMU Transition 必须保证执行的连续性。

RISC-V 4.11.0 官方 ASLR 文档明确指出，Core MMU 初始化会保留早期 Identity-mapped 初始化区域，供 MMU 启用时继续执行，然后转移到最终随机化的虚拟地址区域。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



这个过程可以抽象为：

MMU Transition：地址空间的连续性

MMU OFF

Early Code

PC = PA

Early Stack

Physical Address

MMU ON

Identity Window

VA = PA（过渡阶段）

Final Core Mapping

VA → Secure PA

过渡代码继续可执行，再切换到正式 VA

这里还必须考虑 TLB、Instruction Fetch Synchronization 和架构规定的地址转换顺序，不能只关注 Page Table 内容。

## **8.7.6 为什么&#x20;**`boot_mem_relocate()`**&#x20;也需要执行？**

假设在 MMU 初始化前，OP-TEE 从 Boot Allocator 中分配了一个结构体：

```
struct boot_device {
    void *regs;
    void *context;
};
```

这些指针可能记录了某种早期地址。

即使 `gp/tp/sp/ra` 全部修正，这个结构体中的地址也不一定会自动变正确。

对于需要修正的 Boot Allocation 指针，OP-TEE 提供显式的 Relocation Registration/Update 机制。

在 RISC-V 路径中，可以观察到：

```
enable_mmu()
    |
    v
Update CPU-local stack metadata
    |
    v
boot_mem_relocate()
    |
    v
console_init()
```

为什么还要重新调用 `console_init()`？

因为 Console Driver 早期可能登记了基于 PA 或 Identity Mapping 的地址，而启用正式 MMU/ASLR 后，需要更新为适用的 VA。

真实代码对此有明确说明。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这给我们一个非常重要的结论：

**MMU Transition 不只是 Page Table 和 CSR 的变化，还包括软件世界所有存活地址引用的一致性问题。**

这将直接影响 Boot Allocator、Console、Driver、CPU-local State 和早期数据结构的设计。

# **8.8 CPU Privilege State、Exception、Interrupt 与 Early Console**

## **8.8.1 早期异常为什么特别危险？**

一个已经正常运行的 OP-TEE Kernel，发生 Data Abort 时，可以利用：

- Exception Vector；
- Abort Stack；
- Register Context；
- Page Table；
- Panic Handler；
- Console；
- Symbol Information。

但 Early Boot 中，这些设施并不一定已经准备好。

例如：

```
Early Boot
    |
    v
Access invalid memory
    |
    v
Synchronous Exception
    |
    v
Early Exception Vector
    |
    v
No reliable stack / console
```

此时，最好的行为有时并不是“恢复执行”，而是进入一个可被调试器观察的稳定失败状态。

这也是为什么 Early Exception Handler 往往与正式 Kernel Exception Handler 分离。

## **8.8.2 为什么不应过早打开中断？**

假设 Temporary Stack 已经建立，但是正式 Trusted Thread Runtime 尚未初始化。

如果此时接收到一个 Secure Interrupt，而 Handler 试图：

```
thread_get_current();
mutex_lock(&some_lock);
schedule_work();
```

可能会出现严重问题。

因为正常 Thread Context、Locking Infrastructure、Interrupt Handling State 可能还没有建立。

RISC-V 4.11.0 的 `init_primary()` 中有一个重要步骤：在切换到正式 Thread Vector 之前，对异步异常进行屏蔽，并说明临时栈执行期间需要满足 Thread Subsystem 的相关约束。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里的架构原则是：

> 中断是否允许发生，不由“CPU 已经能执行 Handler”单独决定，而由 Handler 的整个运行时依赖是否满足决定。

另外还需要区分同步异常和异步中断。

即便中断被屏蔽，访问非法地址仍可能触发同步异常。所以 Early Boot 必须有最低限度的同步异常处理策略。

## **8.8.3 Early Console 不是普通 Driver**

普通 Linux Driver 可以依赖 Memory Allocator、Device Model、Clock Framework 和 Interrupt Infrastructure。

但 Early Console 不行。

它往往必须在这些设施尚未初始化时提供最基本的 Debug 能力。

因此 Early Console 设计通常需要满足：

| 要求                 | 原因                           |
| ------------------ | ---------------------------- |
| 极少的外部依赖            | 避免初始化循环依赖                    |
| 明确的 MMIO 地址模型      | 区分 PA 与 VA                   |
| 可控的 Clock/Reset 前提 | UART 不一定已经完成初始化              |
| 不使用未初始化的锁          | Early Runtime 不一定允许 Blocking |
| 不依赖动态资源            | Heap 可能尚未可用                  |
| 可以重新初始化            | MMU/ASLR 后地址可能变化             |

特别需要注意：**Console 可以重新初始化，并不代表所有初始化操作都可以无条件执行两次。**

例如，重复 Register 同一个 Driver、重复初始化 Lock、重复分配资源，都可能导致状态损坏。

因此，Early Console 应把必要的可重配置状态与一次性初始化逻辑明确分开。

## **8.8.4 Stack Canary 和编译器插桩为什么影响 Early Boot？**

现代编译器可能在函数入口和退出时自动插入 Stack Protector 代码。

一个看似普通的 C 函数：

```
void early_service(void)
{
    char buf[64];

    /* ... */
}
```

编译器可能生成隐含的 Guard Load、Stack Canary Check 和失败处理逻辑。

如果 `__stack_chk_guard` 尚未正确初始化，或者失败处理函数依赖尚未可用的 Runtime，就可能让 Early Boot 产生新的故障。

在 OP-TEE 4.11.0 中，Core 的 Stack Protector 与 Thread Stack Boundary Canary 是不同机制。

RISC-V 入口会在 `boot_init_primary_final()` 之后，依据配置更新编译器使用的 `__stack_chk_guard`。官方文档还说明了 `CFG_NS_VIRTUALIZATION` 下默认 Canary 生成路径的限制。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



因此，修改 Early Boot C 函数时，要检查的不仅是源码，还包括编译器最终生成的指令。

同样的原则适用于：

- Sanitizer Instrumentation；
- Stack Probe；
- Compiler-generated `memcpy/memset`；
- Floating-point/SIMD 指令；
- TLS 或其他编译器 Runtime Helper。

并不是说这些能力永远不能在 Early Boot 中使用，而是必须证明它们此时的依赖已经满足。

# **8.9 从 Early C 到正式 OP-TEE Kernel Runtime**

## **8.9.1 什么时候才算 C Runtime 完成初始化？**

这个问题没有一个适用于所有 OS 的固定答案。

我们需要按能力划分，而不是按某个函数名称划分。

| 阶段                  | 已经具备的能力                          | 尚不能默认具备                     |
| ------------------- | -------------------------------- | --------------------------- |
| Minimal Assembly    | 寄存器、PC-relative 执行、直接内存访问        | Stack、普通 C ABI 环境           |
| Basic C             | Stack、Global Data、受控的 C 调用       | 通用 Heap、Thread、Lock         |
| Early Boot Services | Boot Memory、Early Console、部分平台服务 | 所有正式 Kernel API             |
| MMU Runtime         | 正式 VA、必要的地址修正                    | 所有 Driver、Crypto、TA Service |
| Full Kernel Runtime | 已完成相应初始化的内核服务                    | 仍可能存在配置相关功能限制               |

注意，即使已经进入 `boot_init_primary_early()`，也不能假设所有普通内核服务都可用。

## **8.9.2 Boot Memory Allocator 和普通 Heap 的区别**

RISC-V 入口的执行链中有：

```
boot_mem_init()
    |
    v
core_init_mmu_map()
    |
    v
enable_mmu()
    |
    v
boot_init_primary_early()
```

而在 `boot.c` 的 `init_primary()` 中，会进一步设置早期普通 Heap Pool，例如将 `__heap1_start` 到 `__heap1_end` 的区间加入分配器。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这说明两种 Allocation 机制所承担的职责不同。

**Boot Allocator** 的特点是初始化依赖少，服务于启动阶段特定的资源分配。

**Runtime Heap Allocator** 则需要完整考虑生命周期、锁、并发和资源回收。

如果一个 Early Boot 函数改为使用普通 `malloc()`，那么仅确认代码能够成功编译是不够的。

还需要检查：

调用时 Heap Pool 是否已经注册？有没有锁依赖？是否可能触发 Pager？失败路径能否处理 OOM？是否会在 MMU 切换后遗留旧地址？

这是一处很小的改动，却可能影响整个 Boot Graph。

## **8.9.3 为什么初始化顺序应该用 Dependency Graph 表示？**

假设我们有四个服务：

```
A = Memory Allocator
B = Console
C = Crypto RNG
D = Secure Storage
```

如果 D 依赖 C，C 依赖 A，B 也依赖 A，那么初始化顺序就不能随意调整。

更复杂的是，A 可能需要 Early Console 输出错误，而 Early Console 又不能使用 A。

因此，Early Boot 中往往需要设计一个能力受限的服务子集，避免形成循环依赖。

可以把初始化顺序建模成一个有向无环图：

```
Entry Register State
      |
      v
Minimal Stack
      |
      v
Basic C
      |
      +----> Early Console
      |
      v
Boot Memory
      |
      v
MMU
      |
      v
Runtime Heap
      |
      +----> Crypto
      |
      v
Thread Runtime
      |
      v
Kernel Services
```

这是一张逻辑依赖图，不表示上游所有平台严格采用这个线性先后顺序。

对于 Maintainer 来说，重要的是识别每个节点所声明的前置条件，而不是凭经验决定调用顺序。

# **8.10 Early Runtime 的核心安全不变量**

到这里，我们已经可以总结本章最重要的架构规则。

| 不变量                         | 适用范围                   | 违反后果              |
| --------------------------- | ---------------------- | ----------------- |
| 入口使用的内存必须可访问                | 所有 Boot 阶段             | 同步异常              |
| BSS 清零不能破坏其他执行者状态           | SMP Boot               | 双 Primary、数据竞争    |
| 当前 SP 必须对应有效的内存映射           | 所有 C 调用                | Stack Fault       |
| CPU-local State 必须指向正确 CPU  | SMP/Thread             | 跨 CPU 状态污染        |
| Relocation 只能作用于语义明确的地址引用   | ASLR/加载                | 非法指针、控制流错误        |
| MMU 切换必须保持代码执行连续性           | Translation Transition | Instruction Fault |
| MMU 切换后的存活指针必须保持有效          | Boot Memory/Driver     | 隐蔽内存损坏            |
| 早期异常不能依赖尚未初始化的服务            | Exception              | Nested Fault      |
| 初始化代码不能提前使用未就绪的 Runtime API | Early C                | 死锁、Panic          |
| 初始化不能破坏前级 Firmware 的资源      | Firmware Handoff       | 跨 Domain 数据损坏     |

下面通过三个真实工程中非常具有代表性的场景，把这些原则联系起来。

## **场景 A：打开 Core ASLR 后，OP-TEE 偶发启动失败**

假设现象如下：

```
CFG_CORE_ASLR=n:
    Boot always succeeds

CFG_CORE_ASLR=y:
    Boot fails intermittently

Failure location:
    Early MMU transition
```

第一反应可能是怀疑 Page Table 配置错误。

但一位熟悉 OP-TEE Core 的工程师应该至少检查四个方面。

**首先是 Relocation。** 是否所有需要修正的地址引用都被正确处理？

**其次是 Stack/CPU-local State。** `sp` 和 `tp` 修正后，记录在内存中的 `tmp_stack_va_end` 是否也同步更新？

**第三是 Boot Allocation。** 是否有结构体保存了旧 VA，但没有经过 `boot_mem_relocate()`？

**第四是 Console/Driver。** 是否有早期 MMIO Mapping 在 ASLR 后继续使用旧地址？

这里最有价值的排查方法，是在 MMU 切换前后记录同一个关键对象的地址：

```
Before:
  pointer value
  physical target
  virtual mapping

After:
  pointer value
  physical target
  virtual mapping
```

检查的是“指向的对象是否保持相同”，而不是只比较数值是否增加固定 Offset。

## **场景 B：单核正常，多核偶尔在 BSS 初始化附近死机**

这个问题看起来像 Cache Coherency Bug。

但真正原因也可能是 Primary/Secondary 选择与 BSS 清理之间发生竞态。

建议按以下逻辑检查：

**第一步：** Primary 选择变量位于什么 Section？

**第二步：** 它什么时候被初始化？由谁初始化？

**第三步：** Secondary Hart 在 Primary 清 BSS 期间会访问哪些共享变量？

**第四步：** 有没有通过 Atomic、Fence、Boot Semaphore 建立正确的顺序？

**第五步：** Cache 开启与关闭状态下，这些同步变量是否具有正确的内存属性？

这种问题不应该通过简单延时解决。

因为延时最多改变 Race Condition 的出现概率，并不会建立正式的 Happens-Before 关系。

## **场景 C：增加了一条 Debug Log，Early Boot 反而无法启动**

假设增加：

```
IMSG("Entering early service");
```

然后系统直接死机。

这里至少可能涉及：

- Console 尚未初始化；
- Log 函数依赖尚未准备好的 CPU-local 状态；
- Format/Logging 路径使用了过深的栈；
- 编译器在该路径生成了新的 Runtime Helper；
- 输出寄存器访问使用了失效的 MMIO Mapping；
- Debug Log 改变时序，使原有竞态暴露。

这个场景体现了一个很重要的事实：

**Debug Code 本身也是系统的一部分，它有资源依赖，也可能改变被观察系统的行为。**

因此，真正的 Early Boot Debug 应采用分层方法，而不是无条件加入越来越多的日志。

# **8.11 实践：用 GDB 和故障注入研究 Early Runtime**

建议在上游 OP-TEE 4.11.0 的 QEMU 环境中进行。下面的操作以已经拥有可用的构建和 QEMU 启动配置为前提，调试命令用于说明方法，不代表完整的一键启动脚本。

## **实验一：静态分析 ELF 与 Linker Layout**

先定位镜像的各个 Section。

```
readelf -h out/arm-plat-*/core/tee.elf
readelf -lW out/arm-plat-*/core/tee.elf
readelf -SW out/arm-plat-*/core/tee.elf

nm -n out/arm-plat-*/core/tee.elf \
    | grep -E '__bss_|__data_|__init_|_start'
```

输出路径需要根据实际构建目录调整。

这次实验需要回答：

| 问题                                  | 验证方法                    |
| ----------------------------------- | ----------------------- |
| ELF Entry Point 在哪里？                | `readelf -h`            |
| 代码和数据由哪些 Program Header 加载？         | `readelf -lW`           |
| BSS 是否占用文件大小？                       | 对比 `FileSiz` 与 `MemSiz` |
| `__bss_start` 与 `__bss_end` 的地址是什么？ | `nm`                    |
| 是否存在特殊 `.nozi`、`.init` 区域？          | `readelf -SW`           |
| 栈及临时内存是否与 BSS 重叠？                   | Linker Map / Script     |

重点是建立：

**Linker Symbol → ELF Segment → Physical Memory → Runtime Mapping**

之间的对应关系。

## **实验二：观察从&#x20;**`_start`**&#x20;到第一条 C 函数的变化**

AArch64 可以通过 QEMU GDB Stub 进行早期调试。

例如：

```
target remote :1234

symbol-file out/arm-plat-*/core/tee.elf

break *_start
continue

info registers
x/16i $pc
```

注意：如果使用了 Core ASLR 或物理地址重定位，必须按照当前真实运行地址修正 Symbol Loading。不能假设 ELF 中的 Symbol Address 与当前 PC 一定相同。

建议重点记录四个 Checkpoint：

| Checkpoint       | 需要检查                           |
| ---------------- | ------------------------------ |
| `_start`         | PC、EL、SP、MMU State             |
| BSS 清理后          | BSS 范围与全局变量                    |
| Stack Setup 后    | SP、Stack Range、CPU-local State |
| `enable_mmu()` 后 | PC、SP、Page Table、虚拟地址          |

对于 RISC-V，可以另外检查：

```
info registers
p/x $pc
p/x $sp
p/x $gp
p/x $tp

x/16i $pc
```

以及 GDB/模拟器支持的 `satp`、Trap CSR 和 Hart 状态。

目标是验证这些寄存器在 MMU Transition 前后的变化。

## **实验三：故障注入**

建议在 QEMU 环境中完成以下测试：

| 实验 | 修改                          | 预期考察点                |
| -- | --------------------------- | -------------------- |
| A  | 故意使 Early Stack 地址无效        | 第一处 Stack Fault 如何出现 |
| B  | 故意跳过 BSS 清理                 | 哪些全局状态首先失效           |
| C  | ASLR 后故意遗漏一个地址修正            | 是否能定位错误引用            |
| D  | 在 Early C 中调用未初始化的服务        | 依赖错误如何表现             |
| E  | 修改 RISC-V Hart Lottery 变量布局 | 多核竞态与初始化顺序           |

每个实验都应该提交一份简短的分析记录，包括触发条件、最初异常状态、根因、修复方案和需要增加的回归测试。

## **实验四：完成一份 Early Runtime Dependency Map**

建议最终整理出以下内容：

```
Symbol / API
    |
    +-- Who initializes it?
    |
    +-- Initialization stage?
    |
    +-- Address type?
    |
    +-- Per-CPU or Global?
    |
    +-- Allowed before MMU?
    |
    +-- Allowed before Heap?
    |
    +-- Allowed before Thread Runtime?
    |
    +-- Reinitialized after ASLR?
    |
    +-- Relevant regression tests?
```

选择 `console_init()`、`boot_mem_init()`、`thread_core_local`、`boot_mmu_config` 四个对象作为分析起点。

这项实验最能体现本章的 Maintainer Engineering 价值。

# **8.12 Staff Engineer 级别面试题**

下面安排 10 道综合考题。

题目不以记忆 OP-TEE 函数名为核心，而是要求建立系统模型、设计验证方案、推理失效机制，并对技术选择进行权衡。

每道题都附上考察方向和参考分析要点，但不是唯一正确答案。尤其是设计题，是否能够提出可验证的假设，比答案是否恰好采用某一种具体实现更重要。

## **Question 1 — 完全没有日志的 Early Boot Crash**

**场景：**

某 RISC-V SoC 在 QEMU 中可以稳定启动 OP-TEE，但在真实芯片上始终没有任何 OP-TEE Log。

硬件团队确认 CPU 已经跳转到了 OP-TEE Entry，且 `_start` 中的第一条指令可以执行。

你不能使用 printf，只能使用 JTAG/GDB 读取 CPU Register 和 Memory。

**问题：**

你会如何设计一个分阶段、最小侵入的 Debug Strategy？如何在不依赖完整 C Runtime 的情况下，逐步定位是哪一个初始化前提被破坏？

**考察方向：** Boot Contract、ELF/Linker、CSR、SP/GP/TP、Memory Attribute、MMU、Exception。

**高水平答案应体现的思考：**

先建立能够验证的 Checkpoint，而不是直接猜测 Bug。

从 PC 所在地址、入口特权模式、当前 `satp`、`gp`、`sp`、`tp`、Data Segment 内容开始验证，随后观察 BSS 清理、Stack Setup、首次 C Call 以及 MMU 切换。

进一步应提出如何通过 Data Breakpoint、硬件断点、CSR Fault State 和最小汇编 Marker，定位首次破坏不变量的指令。

如果最终发现实际硬件与 QEMU 的 Cache 或 Memory Ordering 行为不同，还需要解释为什么模拟器未能暴露问题。

## **Question 2 — ASLR 开启后只有某些启动随机种子会失败**

**场景：**

OP-TEE Core ASLR 在固定 Seed A 下正常，在 Seed B 下出现 Data Abort。

错误发生在 MMU 开启后，第一次调用某个 C 函数时。

你已经确认两种情况下 Page Table 都包含有效的 Core Code Mapping。

**问题：**

如何证明故障是 Relocation、Stack Pointer、Global Pointer、函数指针还是其他内部地址引用造成的？

**考察方向：** Link-time VA、Runtime VA、ELF Relocation、GOT、Identity Mapping、存活对象引用。

**高水平答案应体现的思考：**

不能因为 PC 所在页有效就断言 MMU 没有问题。

应分别检查 Instruction Fetch、Stack Access 和 Global Data Access，并追踪 faulting instruction 的寻址方式。

应检查 Relocation Entries 是否完整、Kernel Stack Metadata 是否修正、Boot Allocator 是否存在未登记的存活指针，以及某种 ASLR Offset 是否跨越了 Page Table 或 VA Region 边界。

最后，设计能够覆盖不同 Randomized Base 的自动化测试，而不是只选择一个能够通过的固定 Seed。

## **Question 3 — 16 Hart SMP 启动中出现两个 Primary Hart**

**场景：**

一个 16-Hart RISC-V 平台在极低概率下发生双 Primary Boot。

Primary Boot 之间共享 `hart_lottery` 和一些 Boot Semaphore。

代码审查发现这些变量使用了普通全局静态变量。

**问题：**

除了改成 Atomic，你认为还必须验证什么？如何证明这套 Primary Election 机制是正确的？

**考察方向：** Initialization Order、Section Placement、Memory Ordering、Cache Coherency、Atomicity、Happens-Before。

**高水平答案应体现的思考：**

必须检查 Lottery Variable 的 Section、初始值、BSS 清零时序、多 Hart 可见性及原子扩展支持。

还应考虑 Reset/Re-entry 语义：Warm Boot 是否会错误复用前一次的状态？不同 Hart 是否经过同一个共享 Coherency Domain？Memory Attribute 是否允许正确的原子访问？

真正完整的设计，还需要明确 Secondary Hart 何时允许访问已经由 Primary 初始化的结构，并建立 Release/Acquire 或其他有效的同步机制。

## **Question 4 — 更换编译器后，Early Boot 在第一条 C 调用处失败**

**场景：**

原来使用 GCC 的 OP-TEE 可以稳定启动。切换到新版本 Clang 后，启动在第一个 Early C 函数中失败。

源码没有明显变化。

**问题：**

如何系统判断这是 ABI、Stack Alignment、Compiler Instrumentation、Linker Relaxation 还是 Runtime Helper 问题？

**考察方向：** ABI、Toolchain、Compiler-generated Code、ELF、Stack Protector、Relocation。

**高水平答案应体现的思考：**

不能仅比较 C 源码。

应比较 Object Code、Disassembly、Relocation Table、Stack Prologue/Epilogue、编译参数及 Linker Script Output。

特别检查是否生成了隐藏的 `memcpy`、Stack Protector、Stack Probe、TLS Access 或不适合该阶段的指令。

RISC-V 还需要考虑 `gp` 初始化和 Relaxation，Arm 则需要检查 AAPCS64 的栈对齐和架构 Feature 相关指令。

最终应能够提出一个最小测试用例，隔离 Toolchain 与 OP-TEE 配置的差异。

## **Question 5 — 如何设计一个支持任意物理加载地址的 OP-TEE Core？**

**场景：**

某 SoC 的 Bootloader 无法保证 OP-TEE 每次加载到同一个 PA。

安全团队要求 OP-TEE 支持 Physical Relocation，同时保持 Core ASLR。

**问题：**

请设计完整的地址处理流程。哪些地址由 Bootloader 负责，哪些由 OP-TEE 负责？如何保证所有早期指针正确？

**考察方向：** Physical Relocation、Virtual ASLR、ELF、Boot Contract、MMU、Security Boundary。

**高水平答案应体现的思考：**

首先把 Physical Load Address 与 Runtime VA 分开建模。

然后确认 Entry Code 是否能在不依赖固定绝对地址的情况下执行；Relocation 应在适当时机完成，且不能破坏已经受保护的内存。

同时设计对 Entry Stack、BSS、MMU Table、Console 和 Boot Allocator 的兼容策略。

还应讨论镜像认证是针对哪些字节进行的、Relocation 在认证前还是认证后发生，以及动态加载地址是否会影响受保护 RAM 的硬件访问权限。

最后给出一套包含不同加载地址、不同 ASLR Offset、SMP 和异常注入的 Test Matrix。

## **Question 6 — Temporary Stack 是否应该支持动态分配？**

**场景：**

某平台当前在 Linker Script 中静态保留所有 CPU 的 Temporary Stack，消耗较多 Secure SRAM。

有人建议改为 Boot 阶段从动态内存池分配。

**问题：**

你是否支持这项修改？应该考虑哪些设计约束？

**考察方向：** Stack Lifetime、Early Allocation、Per-CPU State、Memory Security、Error Handling、SMP。

**高水平答案应体现的思考：**

首先识别 Bootstrap Problem：在第一个 Stack 可用之前，动态分配器本身如何运行？

可以考虑一个最小静态 Bootstrap Stack，再由可靠的 Boot Allocator 创建正式 Per-CPU Stack。

但仍需明确异常路径、分配失败、内存权限、栈边界 Canary、CPU 数量、Secondary Boot 以及 Suspend/Resume 行为。

尤其要证明原有栈在切换之后不再被任何中断或重入路径引用，才允许释放或重新利用。

## **Question 7 — 为什么 MMU 切换代码必须放在 Identity-mapped Region？**

**场景：**

一个工程师认为，只要新 Page Table 中包含 OP-TEE Code 的最终 VA Mapping，就不再需要 Identity Mapping。

他提议在启用 MMU 之前删除所有 Identity Mapping。

**问题：**

这个设计为什么可能失败？有没有其他可行方案？你如何证明 MMU 切换序列的正确性？

**考察方向：** Instruction Fetch、Translation Context、TLB、Pipeline、PC、Branch、Memory Permission。

**高水平答案应体现的思考：**

需要明确在地址转换切换瞬间，当前 PC 使用什么地址语义，以及后续指令如何取得。

不能只证明最终目标 VA 可执行，还必须证明当前执行流能够从旧地址环境连续进入新地址环境。

可以讨论架构允许的跳转和映射过渡设计，以及必须执行的同步指令。

还应考虑当前 Stack、全局数据引用和异常向量的访问连续性，而不是仅研究代码页。

## **Question 8 — 在 Early Boot 中启用 Stack Protector 是否一定更安全？**

**场景：**

安全团队要求所有 OP-TEE Core C 文件启用 `-fstack-protector-all`，包括最早执行的 C Runtime。

**问题：**

你会如何评审这个要求？应怎样设计安全、可验证的启用顺序？

**考察方向：** Security Hardening、Compiler Instrumentation、Entropy、Bootstrap Dependency、Threat Model。

**高水平答案应体现的思考：**

需要先检查 Guard Initialization、Failure Handler、Entropy Source 和 Stack Runtime 是否可用。

如果早期只能使用固定 Canary，那么它能够防御哪些漏洞，又不能防御哪些漏洞？

还要明确编译器生成的 Canary Check 在函数入口/返回时访问了什么内存，以及改变 Guard 值是否会影响仍然活动的 Stack Frame。

最后必须区分 Compiler Stack Protector 与 Thread Stack Boundary Canary，不应把两者混为一谈。

## **Question 9 — 如何防止 Secondary CPU 观察到未完全初始化的 Kernel State？**

**场景：**

Primary CPU 在初始化 Page Table、CPU-local Array、Interrupt Controller 之后，会允许 Secondary CPU 进入 OP-TEE Runtime。

但某些 Secondary CPU 偶尔读取到旧状态。

**问题：**

请设计一个能够在 Arm 和 RISC-V 上实现的 Boot Synchronization Protocol，并解释需要哪些 Memory Ordering 和 Cache Coherency 保证。

**考察方向：** SMP、Memory Model、Cache Coherency、Atomic State、Boot Barrier、Interrupt。

**高水平答案应体现的思考：**

需要明确每个共享对象的初始化者和消费者。

Primary 写入共享状态后，必须通过有效的同步协议发布 Ready State；Secondary 在观察到 Ready 后，才允许使用对应的数据结构。

如果涉及不同 Coherency Domain，还需要明确 Cache Maintenance 和内存屏障的具体要求。

进一步应检查 Page Table 更新的 TLB 同步、Instruction Cache 可见性、Secondary Stack 独立性，以及 Primary 初始化失败时 Secondary 的处理策略。

一个高水平方案还应能证明不会出现无限等待和错误重入。

## **Question 10 — 设计一个可维护、可验证的 Early Boot Framework**

**场景：**

你负责一款新的 RISC-V 服务器 SoC 的 OP-TEE Core Port。

系统包含多个 Hart、OpenSBI、独立 Root of Trust 处理器、受保护内存和若干 Secure Device。

原有 Early Boot Code 充满平台专用逻辑，初始化顺序依赖工程师经验，修改一处代码经常影响其他平台。

**问题：**

请从 Staff Engineer 的视角提出一个新的 Early Boot Framework 设计。

要求兼顾正确性、安全性、可测试性、跨架构可维护性和后续上游合并。

**考察方向：** Architecture Design、Abstraction、Dependency Graph、Boot Contract、Maintainability、Security Review。

**高水平答案应体现的思考：**

首先给出清晰的 Boot Contract，明确上一级 Firmware 应负责哪些硬件安全属性和初始 CPU 状态。

其次分离 Architecture-Specific Assembly、Platform-Specific Early Setup 和 Architecture-Neutral Core Initialization。

再定义显式的初始化依赖与能力边界，例如哪些阶段允许使用 Stack、Boot Memory、MMU、Console、Heap 和 Thread Service。

还需要设计可重复的启动 Trace、故障码、异常注入和 SMP 状态验证机制。

最后，不能只给出一个大型重构 Patch，而应提出可逐步合并的迁移策略，保证旧平台行为不变，并通过多配置、多架构测试证明修改的正确性。

# **8.13 本章总结：需要真正理解的五个问题**

完成 Chapter 08 后，希望你形成以下五种认知。

**第一，Early Boot 并不等于几段初始化汇编。** 它是一系列从弱运行环境到强运行环境的状态转换，任何 C 代码都必须满足对应阶段的依赖约束。

**第二，Stack、Global Data、CPU-local State 和地址引用必须作为一个整体研究。** 初始化一个 SP 并不意味着完成了整个执行上下文的初始化。

**第三，Relocation 和 MMU Transition 不仅是修改 Page Table。** 它需要保证 CPU 寄存器、内存中的存活指针、控制流和虚拟地址语义始终一致。

**第四，SMP Boot 的正确性依赖真实同步关系。** Atomic Operation、Section Placement、Initialization Order 和 Cache Coherency 是同一个问题的不同方面。

**第五，Early Runtime 的设计质量决定后续系统能否被可靠诊断和维护。** 当 Console、Thread、Heap 都还不可用时，必须仍然具备一种可验证的最小错误定位机制。

## **本章源码阅读索引**

| 文件                                                                                                                          | 建议重点                               |
| --------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| [Arm64 entry_a64.S](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/entry_a64.S?utm_source=chatgpt.com) | Entry、BSS、SP、Relocation、MMU        |
| [RISC-V entry.S](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/riscv/kernel/entry.S?utm_source=chatgpt.com)      | GP、TP、Hart Lottery、SATP、Relocation |
| [Arm boot.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/boot.c?utm_source=chatgpt.com)             | C Boot Initialization              |
| [RISC-V boot.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/riscv/kernel/boot.c?utm_source=chatgpt.com)        | Hart、Early Runtime、Init            |
| [Arm kern.ld.S](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/kern.ld.S?utm_source=chatgpt.com)       | ELF、Section、Memory Layout          |
| [RISC-V kern.ld.S](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/riscv/kernel/kern.ld.S?utm_source=chatgpt.com)  | Identity Mapping、Boot Sections     |
| [官方 ASLR 设计文档](https://optee.readthedocs.io/en/4.11.0/architecture/aslr.html?utm_source=chatgpt.com)                        | Relocation、VA、Boot Address         |
| [官方 Stack Canary 文档](https://optee.readthedocs.io/en/4.11.0/architecture/stack_canaries.html?utm_source=chatgpt.com)        | Compiler Instrumentation、安全加固      |

**我对 Chapter 08 最重要的评价标准是：**

假设现在拿到一个全新的 OP-TEE 平台，给你 `_start`、Linker Script 和 Boot Firmware 的 Handoff Contract，你能否独立判断：每一个初始化步骤是否具备正确的执行前提，哪些数据可以被安全访问，哪些错误可能被隐藏，以及如何通过最少的实验验证整条启动路径？

如果能做到这一点，才算真正掌握了本章，也具备了审查 Early Boot Core Patch 的基础。

下一章 **Chapter 09 — Initcall Framework 与初始化依赖** 将继续研究：在 Early Runtime 建立之后，OP-TEE 如何有组织地初始化各个 Kernel Service，以及怎样评估新增或调整一个 Initcall 对整个系统的影响。
