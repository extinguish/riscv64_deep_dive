# Chapter 08 — RISC-V OP-TEE Boot & Low-Level Assembly

RISC-V OP-TEE Architecture & Porting · Core Kernel 深度课程 · Staff Engineer Level

本章研究一个非常核心的问题：

当 OpenSBI 将控制权交给 OP-TEE 后，一个尚未建立完整运行环境的 RISC-V hart，究竟如何通过几十到数百条汇编指令，逐步建立栈、内存管理、per-hart 状态和 C Runtime，最终让 OP-TEE 成为能够处理 Trusted Application 请求的操作系统？

这不仅是 Bootloader 问题，也是 OP-TEE Kernel 的执行基础。

我们将以 OP-TEE upstream 的 RISC-V 实现为主，重点研究：

- `core/arch/riscv/kernel/entry.S`
- `core/arch/riscv/kernel/boot.c`
- `core/arch/riscv/kernel/kern.ld.S`
- `core/arch/riscv/kernel/link.mk`
- `core/arch/riscv/mm/`
- `core/kernel/initcall.c`

源码依据为 2026 年 10 月 27 日核对的 upstream `master`。本章展示的短代码片段部分进行了简化，会明确区分实际代码和教学示意。后续使用时应固定同一仓库 commit，以避免不同版本的启动顺序或配置选项发生变化。

## 本章学习目标

完成本章后，应能独立解释以下九个主题。

| Section | 主题                              | 核心问题                   |
| ------- | ------------------------------- | ---------------------- |
| 8.1     | ELF、Linker Script、Memory Layout | OP-TEE 被构造成什么镜像？       |
| 8.2     | OpenSBI → OP-TEE Entry          | 固件如何将控制权交给 OP-TEE？     |
| 8.3     | `entry.S` Early Boot            | 第一条汇编指令开始做了什么？         |
| 8.4     | `sp`、`gp`、`tp`                  | 没有 C Runtime，寄存器如何初始化？ |
| 8.5     | Primary / Secondary Hart        | 多核启动由谁协调？              |
| 8.6     | Early MMU / `satp`              | 怎样从物理地址执行切换到 MMU 地址空间？ |
| 8.7     | BSS、Stack、Heap、Per-Hart         | 运行时内存和数据结构怎样建立？        |
| 8.8     | Assembly → C Runtime            | 为什么终于可以安全地调用 C 函数？     |
| 8.9     | Initcall / Subsystem Init       | OP-TEE 如何完成全部内核初始化？    |

## 8.0 先建立全局执行模型

在阅读任何源码之前，先把完整的执行链串起来。

RISC-V OP-TEE Cold Boot Execution Flow

Boot ROM / Trusted Boot Firmware

加载、认证固件镜像，建立初始信任链（具体由平台决定）

OpenSBI / M-mode Firmware

Hart 管理、SBI、Domain、底层硬件隔离

OP-TEE `_start` — `entry.S`

读取 Hart ID / DTB · Primary Election

Primary Hart

BSS → SP/TP → MMU → Core Init → Runtime → Initcalls

Secondary Harts

等待 Primary → MMU → Per-Hart Init → 同步

OP-TEE Ready

完成线程入口及服务初始化，向底层 Monitor / Domain 返回就绪状态

这里首先要建立三个认知。

第一，进入 OP-TEE 并不意味着 OP-TEE 已经完成初始化。

刚进入 `_start` 时，可能还没有：

- 可供普通 C 函数使用的有效栈。
- 可用的 OP-TEE 页表。
- 初始化完成的 Trusted Thread。
- 可使用的动态内存分配器。
- 可用的 User TA Runtime。

第二，Boot 阶段与 Runtime 阶段的执行环境不同。

Early Boot 使用专门设计的初始化代码、临时栈和有限的基础设施。Runtime 才会具有常规的 Thread、异常处理、内存分配和安全服务能力。

第三，RISC-V OP-TEE 不是简单地将 Arm64 的 EL1 入口汇编改写为 RISC-V 汇编。

它涉及一套不同的特权、固件和隔离模型：

| Arm64 OP-TEE          | RISC-V OP-TEE                    |
| --------------------- | -------------------------------- |
| Secure EL1            | 通常运行于隔离的 S-mode Domain           |
| EL3 Monitor / SMC     | M-mode Monitor / SBI / Domain 机制 |
| `SPSR_EL1`、`ELR_EL1`  | `sstatus`、`sepc`                 |
| `TTBR0_EL1`、`TCR_EL1` | `satp`                           |
| `VBAR_EL1`            | `stvec`                          |
| MPIDR Affinity        | Hart ID / Hart Index             |
| `ERET`                | `SRET` 或通过固件接口完成 Domain 返回       |

最后一行尤其需要注意：`SRET` 是 RISC-V 的 S-mode Trap Return 指令，本身不是跨安全 Domain 切换的通用指令。跨 Domain 的权限和执行环境转换通常需要更高可信固件参与。

# 8.1 OP-TEE ELF、Linker Script 与 Memory Layout

## 8.1.1 为什么研究 Boot 必须先研究 Linker Script？

很多工程师学习启动代码时喜欢直接打开 `_start`，却忽略了一个关键事实：

汇编中的地址、符号、BSS 边界、Boot Stack 位置，以及可以安全执行的代码范围，大部分都需要 Linker Script 提供基础。

例如汇编中：

```
lla t0, __bss_start
lla t1, __bss_end
```

`__bss_start` 和 `__bss_end` 并不是 RISC-V 架构规定的寄存器或指令。

它们是 Linker Script 生成的符号。

同样，后面会用到：

```
__text_start
__text_end
__bss_start
__bss_end
__heap1_start
__heap1_end
__vcore_free_start
__vcore_free_end
```

这些符号共同描述了 OP-TEE 的运行时内存布局。

因此，理解启动流程必须首先回答：

> 镜像被链接到哪里？加载到哪里？CPU 在哪里取指？变量在哪个地址被访问？MMU 开启后这些地址会不会改变？

## 8.1.2 ELF 文件本身并不等于内存映像

OP-TEE RISC-V 的链接过程由 `link.mk` 和 `kern.ld.S` 等文件控制。

当前 `link.mk` 生成的主要产物包括：

| 产物               | 用途                         |
| ---------------- | -------------------------- |
| `tee.elf`        | 带 ELF 元数据和符号信息的 Core Image |
| `tee.bin`        | 从 ELF 转换的原始二进制映像           |
| `tee.map`        | 链接后的地址与符号映射                |
| `tee.dmp`        | `objdump` 生成的反汇编和符号信息      |
| `tee.symb_sizes` | 用于分析符号大小与内存占用              |

这些目标在当前 `core/arch/riscv/kernel/link.mk` 中可以直接找到。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



一个很容易混淆的地方是：

- ELF Section 主要用于链接组织与属性表达。
- ELF Program Header 描述 Loader 应如何将段放入内存。
- 原始 Binary 通常没有 ELF Program Header、符号表等信息，加载地址必须通过平台配置或镜像封装规则确定。

例如，`.bss` 属于运行时需要占用内存但不需要在文件中存放完整零字节内容的区域。

使用原始 `tee.bin` 的 Loader，不能因为文件中缺少 BSS 数据就认为 BSS 不需要内存。

## 8.1.3 Linker Script 怎样决定 OP-TEE Entry？

当前 RISC-V `kern.ld.S` 中存在以下关键语句：

```
OUTPUT_FORMAT(CFG_KERN_LINKER_FORMAT)
OUTPUT_ARCH(CFG_KERN_LINKER_ARCH)

ENTRY(_start)

SECTIONS
{
    . = TEE_LOAD_ADDR;

    __text_start = .;

    .text : {
        KEEP(*(.text._start))

        /* Other code sections */
    }
}
```

上面省略了部分 Section 处理逻辑，但几个关键符号都来自实际源码。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



逐一解释。

`OUTPUT_FORMAT`

指定输出 ELF 格式。例如 RV64 Linux ABI 相关的 ELF64 RISC-V 格式。

`OUTPUT_ARCH`

指定目标机器架构为 RISC-V。

`ENTRY(_start)`

将 `_start` 设为 ELF Entry Point。

需要注意，`ENTRY()` 只是告诉链接器哪个符号是入口，并不自动保证该符号处于文件的第一个字节。

因此源码还使用：

```
KEEP(*(.text._start))
```

将 `_start` 所在 Section 保留并安排到预期位置。

`KEEP()` 还可以防止 `--gc-sections` 把启动入口意外丢弃。

`. = TEE_LOAD_ADDR`

设置链接地址计数器的位置。

假设教学平台配置：

```
TEE_LOAD_ADDR = 0x80000000
```

那么如果没有额外的重定位和特殊段布局，链接器会以这个地址作为相应 Section 的起始链接位置。

但要强调：

Link Address、Load Address、Physical Address、Virtual Address 不应被无条件视为同一个地址。

后面开启 MMU 时，这一点尤其重要。

## 8.1.4 OP-TEE Core 的主要 Section

可以先用下面的逻辑图理解其布局。

OP-TEE Core Memory Layout（示意，非实际地址比例）

.text / identity_map

入口汇编、可执行代码、MMU 切换代码

.rodata

常量、字符串、只读表

.rela.dyn / .dynsym

启用重定位时使用的元数据

.data / .sdata

有初值的全局和静态变量

.bss / .sbss

需要由启动代码清零的变量

.heap1

Early/Runtime Heap 资源

.nozi / .nozi_stack

不由常规 BSS 清零逻辑初始化的区域

Free Core VA Space

后续内存管理使用的地址空间

具体布局取决于编译配置。以上反映 RISC-V Linker Script 的主要内存区域及其逻辑用途。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

这里重点强调 `.nozi`。

它与 `.bss` 的差异非常重要：

- `.bss` 需要在启动时清零。
- `.nozi` 是不由通常的 BSS 清零逻辑初始化的区域。

`.nozi` 可能用于需要单独初始化或保留特殊状态的内存。

在真实 SoC 上，如果错误地扩大 BSS 清零范围，把 `.nozi`、已使用的 Boot Stack 或其他固件共享状态覆盖掉，可能产生极其隐蔽的启动问题。

## 8.1.5 为什么 `.identity_map` 非常重要？

这是 RISC-V OP-TEE Boot 的第一个架构级问题。

假设 OP-TEE 初始执行地址为：

```
Physical Address = 0x80000000
```

此时 MMU 关闭：

```
satp.MODE = Bare
```

CPU 使用物理地址执行指令。

后来 OP-TEE 希望运行在另一个虚拟地址布局中：

```
Virtual Address = 0xFFFFFFC000000000
Physical Address = 0x80000000
```

这时就出现一个问题：

执行 `csrw satp, ...` 的那一刻，下一条指令的地址能否被新页表正确翻译？

如果新地址空间中当前 PC 没有有效映射，那么 MMU 切换后会立即发生 Instruction Page Fault，甚至在 Trap Handler 尚未初始化的情况下直接失去调试能力。

因此，常见的安全切换设计是在 MMU 开启前后，让一段过渡代码保持能够被正确执行。

OP-TEE 的 RISC-V Linker Script 将 `.identity_map` 等相关 Section 放置在专门的连续区域中；后续 `enable_mmu` 也位于 `.identity_map`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



关键思想不是「所有内核代码都要 Identity Mapping」，而是：

> 切换地址空间时，至少必须保证过渡代码、访问的数据、当前栈以及返回路径在切换前后的语义保持有效。

这也就是 Bootstrapping MMU 的经典设计。

## 8.1.6 本节源码分析练习

你可以在编译 OP-TEE 后执行：

```
riscv64-linux-gnu-readelf -h tee.elf
riscv64-linux-gnu-readelf -l tee.elf
riscv64-linux-gnu-readelf -S tee.elf

riscv64-linux-gnu-nm -n tee.elf \
    | grep -E '_start|__bss|__text|__heap'

riscv64-linux-gnu-objdump -d tee.elf \
    | less
```

重点核对：

1. `e_entry` 是否指向预期的 `_start`？
2. 哪些 Section 属于具备 Load 语义的 ELF Segment？
3. `.bss` 如何体现在文件大小与内存大小的差异中？
4. `enable_mmu` 位于什么 Section？
5. `__global_pointer$` 在什么位置？
6. 开启 `CFG_CORE_ASLR` 前后，ELF 类型和重定位信息有何变化？

完成这六项，你就初步具备了调试 OP-TEE Core Loading 和 Early Boot 的能力。

# 8.2 从 OpenSBI 到 OP-TEE Boot Entry

## 8.2.1 谁负责将 OP-TEE 加载到内存？

这里一定要区分 Loader 与 Runtime Monitor。

在 RISC-V SoC 中：

- Boot ROM 或后续 Bootloader 负责加载固件，可能还负责认证镜像。
- OpenSBI 提供 M-mode 固件服务，在支持 Domain 的实现中还可以承担 Domain 管理。
- OP-TEE 是最终运行的 Trusted OS。

这三者不是同一个东西。

也不能简单地理解为：

```
OpenSBI 一定负责加载 OP-TEE ELF
```

因为固件镜像可能在 OpenSBI 执行之前，就已经由 Bootloader 加载到了内存。

更准确的抽象是：

```
Loader
  |
  | Place OP-TEE image into memory
  v
OpenSBI / Trusted Monitor
  |
  | Prepare execution environment
  | Select entry point
  | Set privilege and isolation state
  v
OP-TEE _start
```

## 8.2.2 通过 `MRET` 进入 S-mode

假设 OpenSBI 当前在 M-mode，准备启动 S-mode 的 OP-TEE。

RISC-V 的基本机制可以概括为：

```
# Teaching example: not copied from OpenSBI.

li    t0, OPTEE_ENTRY_PA
csrw  mepc, t0

csrr  t1, mstatus
li    t2, MSTATUS_MPP_MASK
not   t2, t2
and   t1, t1, t2

li    t2, MSTATUS_MPP_S
or    t1, t1, t2

csrw  mstatus, t1

mret
```

这段伪代码展示了两个关键动作：

1. 设置 `mepc`，指定退出 M-mode 后的执行位置。
2. 设置 `mstatus.MPP = S`，指定 `MRET` 后的目标特权级别。

真正的 OpenSBI 切换路径还需要考虑 Domain Context、寄存器状态、内存权限、Delegation 和中断状态，因此不能把这段代码当作可直接用于产品的完整实现。

`MRET` 的基本效果包括：

```
PC   ← mepc
Mode ← mstatus.MPP

MIE  ← MPIE
MPIE ← 1
MPP  ← least-privileged supported mode
```

由此，CPU 才可能开始以 S-mode 执行 OP-TEE 的 `_start`。

这里再强调一次：

`MRET` 完成 Privilege Transition，不意味着它自动建立了 Domain Isolation。

后者是 M-mode Firmware 配合 PMP、系统级 Firewall 等机制需要提供的安全保证。

## 8.2.3 OP-TEE Entry 的参数从哪里来？

当前 RISC-V `entry.S` 对 `_start` 的寄存器约定写得很清楚：

```
a0 = Hart ID（如由前级传入）
a1 = System DTB Address（如由前级传入）
```

并将它们临时保存到：

```
mv s0, a0
mv s1, a1
```

如果是特定 M-mode 构建，代码会通过 `mhartid` 获取 Hart ID；如果配置了 `CFG_DT_ADDR`，则使用编译期给定的 DT 地址。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这些参数的意义是：

Hart ID 告诉 OP-TEE 当前是谁，DTB 告诉 OP-TEE 当前运行在什么硬件平台上。

例如，一个 16-hart SoC 的 DTB 可能描述：

```
cpus {
    #address-cells = <1>;
    #size-cells = <0>;

    cpu@0 {
        device_type = "cpu";
        reg = <0>;
        status = "okay";
    };

    cpu@8 {
        device_type = "cpu";
        reg = <8>;
        status = "okay";
    };
};
```

这是简化的 DT 示例，真实平台还需要符合相应 binding 的其他属性。

不要把 Hart ID 直接当成 C 数组下标，因为真实 SoC 的 Hart ID 未必从 0 连续编号。

这会直接影响多核 Boot Stack 和 per-hart 数据结构的设计。

## 8.2.4 进入 OP-TEE 前需要满足什么条件？

对于可靠的工业实现，建议将 Boot Entry Contract 明确定义为：

| 项目        | 前级固件需要保证的条件                 |
| --------- | --------------------------- |
| Privilege | 与构建配置相匹配的执行模式               |
| Entry PC  | 指向可执行的 OP-TEE 入口            |
| Image     | 镜像已正确加载或映射                  |
| Memory    | OP-TEE 使用的内存区域没有被其他实体非法占用   |
| Hart ID   | 能够可靠识别当前 hart               |
| DTB       | 地址和内容满足预期的传递约定              |
| Interrupt | 初始中断状态满足入口代码的假设             |
| Isolation | OP-TEE 所需物理内存权限已建立或有可信的建立流程 |
| Cache     | 镜像加载后的可见性与取指一致性得到保证         |

最后两项特别容易成为真实芯片 bring-up 的问题。

例如，Loader 在一个核上将 OP-TEE Image 写入 DRAM，却没有完成必要的指令缓存同步，那么另一个 hart 可能无法立即取到正确的新指令。

又例如，OP-TEE Image 已经成功加载，但当前 Domain 的 PMP Policy 禁止读取对应内存，那么最终仍然无法正常启动。

加载成功和能够安全执行，是两个不同的工程验收条件。

# 8.3 `entry.S`：Early Boot Assembly 的实际执行过程

这一节开始真正进入 OP-TEE 源码。

## 8.3.1 `_start` 做的第一件事是什么？

不是初始化 C Runtime，也不是打开 MMU。

当前 RISC-V OP-TEE 的 `_start` 首先处理全局指针、Hart ID、Device Tree 参数，然后进行 Primary Hart Election。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



首先看 `gp` 初始化：

```
.option push
.option norelax

lla gp, __global_pointer$

.option pop
```

为什么这里要使用 `.option norelax`？

因为 RISC-V Linker Relaxation 可以将某些符号地址计算优化为基于 `gp` 的寻址形式。

但此时 `gp` 还没有被初始化。

如果初始化 `gp` 本身的指令被错误地放松为依赖 `gp` 的指令，就会产生循环依赖。

因此需要禁止这一段的 Relaxation。

这也是启动汇编中经常能看到 `norelax` 的根本原因。

## 8.3.2 保存启动参数

实际源码的关键逻辑可以归纳为：

```
#ifdef CFG_RISCV_M_MODE
    csrr a0, CSR_MHARTID
#endif

mv s0, a0

#if defined(CFG_DT_ADDR)
    li s1, CFG_DT_ADDR
#else
    mv s1, a1
#endif
```

为什么选择 `s0`、`s1`？

它们属于 RISC-V ABI 中的 Callee-saved Registers。

后续代码可以在遵守 ABI 的调用链中使用它们保存较长生命周期的值。

不过在进入常规 C Runtime 之前，这种寄存器使用依然由汇编代码自身负责。

## 8.3.3 Primary Hart Election：为什么需要 Atomic？

这是当前实现中一个非常值得深入研究的设计。

源码核心片段：

```
lla a3, hart_lottery
li  a2, 1

amoadd.w a3, a2, (a3)

csrw CSR_XSCRATCH, a3

bnez a3, reset_secondary
jal  reset_primary
```

其作用是通过 Atomic Memory Operation 选择 Primary Hart。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



假设有四个 hart 同时执行 `_start`。

`hart_lottery` 初始值为 0。

Hart Lottery 示例

| Hart ID | `amoadd.w` 返回旧值 | 启动角色      |
| ------- | --------------- | --------- |
| Hart 8  | 0               | Primary   |
| Hart 0  | 1               | Secondary |
| Hart 4  | 2               | Secondary |
| Hart 12 | 3               | Secondary |

以上是一种可能的执行顺序，用于说明原子操作的语义，并不表示硬件保证 Hart 8 最先执行。

`amoadd.w` 可以抽象为：

```
old = atomic_fetch_add(&hart_lottery, 1);
```

执行结果：

```
old == 0  → Primary
old != 0  → Secondary
```

因此 Primary Hart 不一定是 Hart ID 为 0 的那个 hart。

这是一个非常重要的架构结论：

> Hardware Hart ID、OP-TEE Boot Hart 和 OP-TEE Logical Core Index 是三个不同的概念。

这部分也会影响后续 `get_core_pos()`、per-hart Stack、Interrupt Controller Context，以及多核调试。

## 8.3.4 为什么不能简单地使用普通 Load/Store？

假设使用以下代码：

```
# Incorrect multiprocessor election

lw t0, 0(a0)
bnez t0, secondary

li t1, 1
sw t1, 0(a0)
```

两个 hart 可能同时读取到 0。

于是两个 hart 都进入 Primary 初始化流程。

可能产生的结果包括：

- 重复清零 BSS。
- 重复初始化 Heap。
- 两个 hart 同时修改页表。
- 初始化 Thread Pool 时发生竞争。
- 一个 hart 清除另一个 hart 正在使用的数据。
- 系统在启动阶段随机崩溃。

使用 AMO 保证竞争的 Read-Modify-Write 操作具有原子性。

但原子性不等于所有的内存排序问题都得到解决。Secondary Hart 观察 Primary 初始化结果，还需要明确的同步与可见性保证。

当前源码还提供 `CFG_BOOT_SYNC_CPU` 的同步逻辑，使用启动状态数组和 Memory Fence。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 8.3.5 `_start` 如何分流启动过程？

总体结构很清晰：

```
_start
   |
   +-- Initialize gp
   |
   +-- Save hart ID / DTB
   |
   +-- Atomic Hart Election
          |
          +-- old == 0
          |       |
          |       v
          |   reset_primary
          |
          +-- old != 0
                  |
                  v
              reset_secondary
```

然后 Primary 进入：

```
reset_primary
    |
    +-- Relocation (when enabled)
    +-- Clear BSS
    +-- Prepare SP/TP
    +-- Save Boot Arguments
    +-- Initialize SBI / Console
    +-- Initialize Boot Memory
    +-- Construct MMU Mapping
    +-- Enable MMU
    +-- Initialize Kernel
```

Secondary 则等待 Primary 建立共享状态，随后初始化自己的 MMU、Stack、Per-Hart 数据与硬件上下文。

需要注意，以上是梳理后的执行逻辑，具体顺序由不同编译配置决定。下一节会继续追踪实际代码。

# 8.4 Early Boot 中的 `sp`、`gp`、`tp` 初始化

如果要深入到汇编级别理解一个内核的启动，我认为这三个寄存器必须掌握。

在普通应用程序中，它们通常由运行环境或编译器生成的代码正确维护，因此容易忽视。

但 OP-TEE 刚开始执行时，必须自己建立这些寄存器的有效语义。

## 8.4.1 三个寄存器分别解决什么问题？

| 寄存器     | ABI 名称         | 在 OP-TEE 中的主要作用                    |
| ------- | -------------- | ---------------------------------- |
| `x2/sp` | Stack Pointer  | 指向当前使用的栈                           |
| `x3/gp` | Global Pointer | 支持全局及 Small Data 寻址                |
| `x4/tp` | Thread Pointer | 在 OP-TEE Core 中用于访问当前 hart 的本地执行状态 |

这里有一个你应该特别注意的区别。

RISC-V Linux User Space 使用 `tp` 实现 TLS，但 OP-TEE Core 可以赋予 `tp` 不同的内核内部用途。

在 OP-TEE RISC-V Core 中，`tp` 与 `struct thread_core_local` 密切相关。

这与 Linux Kernel 的 per-CPU Context，以及 Arm64 Kernel 中通过专用寄存器访问 CPU-local 数据的设计理念有相似之处。

但不要把它直接等同于普通 pthread 的 TLS。

## 8.4.2 `gp`：为什么必须在最早期初始化？

回到真实源码：

```
.option push
.option norelax
lla gp, __global_pointer$
.option pop
```

其目的就是让 `gp` 指向链接器确定的 Global Pointer Anchor。

RISC-V 的 Global Pointer Relaxation 可能将原本使用 PC-relative Address Calculation 的代码，优化成 `gp`-relative Memory Access。

例如，概念上：

```
# General address calculation
auipc t0, %pcrel_hi(global_var)
ld    t1, %pcrel_lo(...)(t0)
```

某些满足范围与重定位条件的访问可被优化为基于 `gp` 的形式。

因此，如果 `gp` 值不正确，可能出现一种很难理解的故障：

- CPU 已经进入 `_start`。
- 部分指令能正确执行。
- 一旦访问特定全局变量，立即访问错误地址。
- 在 Debug Build 中可能正常，而 Release Build 使用 Linker Relaxation 后反而出现异常。

这也是为什么在 Boot Assembly 中需要仔细管理 `.option norelax`。

## 8.4.3 `sp`：为什么不能直接使用 OpenSBI 的 Stack？

OpenSBI 和 OP-TEE 有不同的执行环境。

即使 OpenSBI 进入 OP-TEE 时，`sp` 碰巧包含某个有效地址，OP-TEE 也不能默认将其作为自己的 Kernel Stack。

原因包括：

1. 该 Stack 可能属于 M-mode Firmware。
2. 它不一定处于 OP-TEE Domain 有权访问的物理内存中。
3. 它可能没有按照 OP-TEE 的异常处理模型布局。
4. 它可能不能满足后续 MMU 切换后的虚拟地址要求。
5. 它不一定满足 OP-TEE 所需的安全隔离及 Stack Protection 约束。

因此，OP-TEE 必须初始化自己的 Stack。

### 静态配置下的 `set_sp`

当前 `entry.S` 中的核心宏采用如下地址计算：

```
sp = stack_tmp
   + (hart_index + 1) * stack_tmp_stride
   - STACK_TMP_GUARD
```

源码实际通过 `CSR_XSCRATCH` 获取启动阶段的 hart index，并计算栈顶。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



假设：

```
stack_tmp_base   = 0x90000000
stack_tmp_stride = 0x4000
STACK_TMP_GUARD  = 0x100
```

那么四个 hart 对应的理论栈顶为：

| Hart Index | Stack Top    |
| ---------- | ------------ |
| 0          | `0x90003F00` |
| 1          | `0x90007F00` |
| 2          | `0x9000BF00` |
| 3          | `0x9000FF00` |

地址和大小为教学示例，不是 OP-TEE virt 平台的固定配置。

这里的设计思想是：

每个 hart 拥有独立的临时栈，不允许两个 hart 在 Early Boot 阶段共享同一片 Stack Memory。

为什么？

假设 Hart 0 和 Hart 1 共享 `sp`，两者同时执行：

```
addi sp, sp, -16
sd   ra, 8(sp)
sd   s0, 0(sp)
```

那么一个 hart 可能覆盖另一个 hart 保存的 `ra`。

随后执行 `ret` 时，CPU 就可能跳转到完全错误的地址。

这种故障从表现上看可能像非法指令、随机 PC 跳转，甚至像 MMU Fault，但真正的原因是多核 Stack Collision。

## 8.4.4 `tp`：OP-TEE Per-Hart Data 的关键

实际源码中的 `set_tp` 宏可归纳为：

```
csrr t0, CSR_XSCRATCH
li   t1, THREAD_CORE_LOCAL_SIZE

mul  t2, t1, t0

lla  tp, thread_core_local
LDR  tp, 0(tp)

add  tp, tp, t2
```

其中 `LDR` 是源码中的汇编抽象宏，而不是 RISC-V ISA 原生指令。

其核心公式：

\\[ tp = \text{thread\\\_core\\\_local\\\_base}      + \text{hart\\\_index}\times\text{sizeof(thread\\\_core\\\_local)} \\]

当前实现还把 Hart ID 和 Hart Index 写入对应的 `thread_core_local`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



可以把 `tp` 理解为：

> 当前 hart 访问 OP-TEE Kernel 自身执行上下文的快速入口。

后续 Trusted Thread、异常栈、当前 Thread ID 等机制都需要这一类 per-hart 状态。

### 为什么 `tp` 不应该简单绑定到 Hart ID？

假设实际 Hart ID 为：

```
Hart IDs = {0, 4, 8, 12}
```

如果直接使用：

```
per_hart[hartid]
```

那么只有四个 hart，却可能需要至少 13 个数组元素。

而且如果 Hart ID 比较大，可能造成严重内存浪费甚至数组越界。

因此实际内核常维护两个概念：

```
Physical Hart ID:
    0, 4, 8, 12

Logical Hart Index:
    0, 1, 2, 3
```

当前启动代码使用 Lottery Index 作为早期索引，并在后续初始化中建立相应关系。

### `CSR_XSCRATCH` 为什么很重要？

在当前 RISC-V OP-TEE 实现中，`CSR_XSCRATCH` 是根据构建模式选择的架构相关 Scratch CSR，典型 S-mode 配置对应 `sscratch`。

在 Early Boot 阶段，它临时保存 Hart Index：

```
csrw CSR_XSCRATCH, a3
```

随后 `set_sp`、`set_tp` 和 `enable_mmu` 都能利用这个 Index。

但这并不意味着 `sscratch` 在整个 OP-TEE 生命周期都必须永久保存 Hart Index。

实际代码在 `thread_init_per_cpu()` 中重新初始化 Trap Vector，并将 `CSR_XSCRATCH` 清零，让正常 Trap Handler 可以按照预期区分异常来源。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



所以这里存在一个非常典型的系统设计：

同一个 CSR 在不同执行阶段，可以承担不同用途；关键是必须保证其生命周期和所有消费者的假设一致。

## 8.4.5 动态配置：为什么 `sp/tp` 初始化还有另一条路径？

当前源码中有：

```
#if defined(CFG_DYN_CONFIG)
    ...
#else
    set_sp
    set_tp
#endif
```

这意味着 Boot 不只有静态配置一种模式。

在动态配置下，Primary Hart 可以从 `__vcore_free_end` 附近预留临时内存，构建临时 `thread_core_local` 和 Stack。

等到 OP-TEE 后续确定 hart 数量与内存资源后，再将它们切换到正式的 per-hart 数据结构。

源码在这一阶段还设置：

```
THREAD_ID_INVALID
THREAD_CLF_TMP
TMP_STACK_VA_END
ABT_STACK_VA_END
HART_ID
HART_INDEX
```

这些成员分别服务于当前线程状态、临时栈标记、异常栈、Hart 身份等目的。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



本节结论： `gp` 让全局数据访问正确，`sp` 让函数调用和异常保存具备有效栈，`tp` 让内核能够区分并管理各个 hart 的执行状态。三者都是进入完整 C Runtime 的必要基础。

# 8.5 Primary Hart 与 Secondary Hart 的初始化差异

这是 RISC-V OP-TEE Boot 中非常有工业价值的一部分。

一个 16-hart SoC 并不意味着可以让 16 个 hart 同时、独立地完成整套 OP-TEE 初始化。

必须明确哪些工作只执行一次，哪些工作必须每个 hart 执行一次。

## 8.5.1 Global Initialization 与 Per-Hart Initialization

先把 OP-TEE 初始化工作分成两个集合。

| Global / One-time Initialization | Per-Hart Initialization          |
| -------------------------------- | -------------------------------- |
| 清零共享 BSS                         | 设置当前 hart 的 Stack                |
| 初始化 Global Heap                  | 设置当前 hart 的 `tp`                 |
| 初始化共享 MMU 数据结构                   | 写入当前 hart 的 `satp`               |
| 初始化 Trusted Thread Pool          | 安装当前 hart 的 Trap Vector          |
| 解析公共 Device Tree                 | 初始化 Hart-local Interrupt Context |
| 注册安全服务                           | 初始化 per-hart SBI/MPXY 状态         |
| 初始化系统级驱动                         | 标记该 hart 已准备好                    |

当然，具体设备驱动可能同时具有全局资源和 per-hart 资源，实际初始化会比这张表复杂。

但这个分类足以解释为什么必须区分 Primary 和 Secondary。

## 8.5.2 Primary 的真正职责是什么？

从源码角度，Primary 主要负责：

```
Primary Hart
    |
    +-- Initialize shared runtime memory
    |
    +-- Build initial MMU mapping
    |
    +-- Initialize global kernel services
    |
    +-- Create Trusted Thread Pool
    |
    +-- Discover available harts
    |
    +-- Initialize shared and primary-local resources
    |
    +-- Start secondary harts
    |
    +-- Synchronize and complete boot
```

尤其需要注意：当前 `reset_primary` 不是一开始就启动所有 Secondary。

实际实现先完成大量 Core 初始化，随后执行：

```
#ifdef CFG_RISCV_S_MODE
    jal boot_start_secondary_cores
#endif
```

之后才进行 Ready 状态同步。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此 Secondary 是通过既定的 Boot Protocol 参与系统初始化的，而不是简单地和 Primary 竞争访问各种共享资源。

## 8.5.3 Secondary Hart 是如何被启动的？

当前 `boot.c` 中的 `boot_start_secondary_cores()` 非常值得深入。

其核心逻辑如下：

```
/* Simplified from upstream logic */

for_each_candidate_hart(hartid) {
    if (hartid == current_hartid)
        continue;

    rc = sbi_hsm_hart_get_status(hartid, &status);

    if (rc || status != SBI_HSM_STATE_STOPPED)
        continue;

    rc = sbi_hsm_hart_start(hartid, start_addr, 0);

    if (rc)
        panic();

    started++;
}

boot_hart_count = started + 1;
```

这段是教学化改写，但函数和主要判断对应真实实现。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里涉及 SBI HSM（Hart State Management）Extension。

重要的接口包括：

```
sbi_hsm_hart_start()
sbi_hsm_hart_get_status()
```

其意义是：

OP-TEE 在 S-mode 请求 M-mode Firmware 启动指定的 hart，而不是直接假设所有 hart 都能由 S-mode 任意复位并启动。

同时，代码并不简单地启动 Device Tree 中列出的所有 hart。

它会检查 Hart 状态；未分配给当前 Domain、不可访问或不是 STOPPED 状态的 hart，不会被按这个流程启动。

这一点对多 Domain SoC 特别重要。

### 真实工业场景

假设有一个 16-hart SoC：

```
Hart 0–11  → Linux Domain
Hart 12–15 → TEE Domain
```

如果 OP-TEE 不考虑 Domain Ownership，错误地要求启动 Linux Domain 的 Hart 3，会发生什么？

正确的 M-mode Firmware 不应该允许它无授权地控制其他 Domain 的 hart。

OP-TEE 必须处理 Hart Start 被拒绝或状态不符合预期的情况，而不是认为所有物理存在的 hart 都属于自己。

## 8.5.4 Secondary 进入 `_start` 后发生什么？

当前 `reset_secondary` 的核心路径：

```
reset_secondary
    |
    +-- wait_primary
    |
    +-- satp = 0
    |
    +-- enable_mmu
    |
    +-- Initialize own tp / sp
    |
    +-- boot_init_secondary
    |
    +-- cpu_is_ready
    |
    +-- Return / Park
```

代码中有：

```
wait_primary

csrw CSR_SATP, zero
jal  enable_mmu
```

之后根据动态或静态配置重新设置 `tp` 和 `sp`，再调用 `boot_init_secondary`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



对应的 C 函数继续完成：

```
thread_set_exceptions(THREAD_EXCP_ALL);
thread_init_per_cpu();
boot_secondary_init_intc();
sbi_mpxy_init();
```

这些内容在 `init_secondary_helper()` 中可以直接观察。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



其中值得特别研究的是 `thread_init_per_cpu()`。

它会建立当前 hart 的 Trap Vector 并处理相应的 CSR 状态。

为什么每个 hart 都要执行一次？

因为 `stvec`、`satp`、`sstatus` 等都是 Hart-local Architectural State。一个 hart 写入自己的 CSR，并不会自动修改其他 hart 的同名 CSR。

## 8.5.5 Multi-Hart Boot Barrier

当前 OP-TEE 通过 `CFG_BOOT_SYNC_CPU` 支持启动阶段同步。

简化结构：

```
Primary
    |
    | Initialize shared state
    v
sem_cpu_sync[0] = READY
    |
    +----------------------------+
    |                            |
    v                            v
Secondary 1                 Secondary 2
    |                            |
    | Observe READY              | Observe READY
    v                            v
Local Init                   Local Init
    |                            |
    v                            v
sem_cpu_sync[1] = READY     sem_cpu_sync[2] = READY
    |                            |
    +-------------+--------------+
                  |
                  v
         Primary observes all READY
                  |
                  v
              Continue
```

这不是普通的 Wait Loop 那么简单。

当前源码还使用：

```
fence rw, w
```

以及：

```
fence r, rw
```

用于同步前后的内存访问排序。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



理解这些 Fence 时需要区分三件事：

- Atomic Operation 解决竞争更新的原子性。
- Memory Ordering 约束共享数据访问与 Ready Flag 的可观察顺序。
- Cache Coherency 及一致性内存系统使各 hart 能够正确观察共享写入。

`FENCE` 不能替代硬件缓存一致性，也不能修复对非一致性内存区域采用错误同步方式的问题。

### 一个必须注意的细节：`boot_hart_count`

假设 Device Tree 中列出了四个候选 hart，但只有两个成功启动。

如果 Primary 永远等待四个 hart，系统就会挂死。

当前代码在启动 Secondary 后，将：

```
boot_hart_count = started + 1;
```

用于记录实际参与本次启动同步的 hart 数量。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这是一个非常好的工程设计细节。

同步条件应当依据实际参与者，而不能简单依赖静态理论核数。

## 8.5.6 Primary 与 Secondary 不一定代表 Linux 的 CPU0/CPU1

这是本章希望特别强调的结论。

```
Physical Hart ID       = SoC 提供的硬件标识
OP-TEE Hart Index      = OP-TEE 内部索引
OP-TEE Primary         = 执行共享初始化的 hart
Linux Logical CPU      = Linux 内核自己的 CPU 编号
```

这些不应直接画等号。

在实际平台移植中，Boot Hart 的选择、CPU DTB 排序和 M-mode Domain Assignment 必须一起核对。

# 8.6 Early MMU / `satp`：从物理地址执行到虚拟地址空间

本章的另一个核心难点。建议结合 `entry.S`、Linker Script 和 GDB 反复验证。

## 8.6.1 为什么 OP-TEE 不能一直在 Bare Mode 下执行？

`Bare Mode` 意味着常规 S/U-mode 地址转换被关闭。

在 RV64 中：

```
satp.MODE = 0
```

代表 Bare Mode。

如果始终使用 Bare Mode，那么 OP-TEE 无法依赖 S-mode 页表实现正常的虚拟地址管理及页级隔离。

例如：

- 不能依靠页表权限建立 Core Code RO/X 和 Data RW/NX 的精细保护。
- 不能用不同 User TA Address Space 实现预期的用户态虚拟内存隔离。
- 无法方便地提供 ASLR、Virtual Memory Mapping 等机制。
- 无法使用标准虚拟地址空间模型来组织 Kernel Runtime。

需要强调，Bare Mode 并不是完全不存在安全保护。更高特权级配置的 PMP 或 SoC 隔离机制仍然可能生效。

但那并不能替代 OP-TEE 自己的 MMU 功能。

## 8.6.2 `satp` 的硬件语义

对于 RV64，常见的 `satp` 字段为：

RV64 `satp` Register Layout

- MODE：指定地址转换模式，例如 Sv39、Sv48。
- ASID：Address Space Identifier，用于区分地址空间的 TLB 状态。
- PPN：Root Page Table 的 Physical Page Number。

例如 Sv39：

```
MODE = 8
```

Sv48：

```
MODE = 9
```

假设：

```
Root Page Table PA = 0x82000000
```

那么：

```
PPN = 0x82000000 >> 12
    = 0x82000
```

一个示意性的 Sv39 `satp` 值可以构造为：

```
uint64_t satp =
    (8ULL << 60) |
    (0ULL << 44) |
    (0x82000000ULL >> 12);
```

但实际运行中，还必须确认 CPU 支持该地址转换模式，以及 Root Page Table 是正确构造并可访问的。

## 8.6.3 OP-TEE 如何构造初始 MMU Mapping？

回到 `reset_primary`。

实际代码顺序：

```
jal boot_mem_init

/* Choose ASLR seed if enabled */

lla a1, boot_mmu_config
jal core_init_mmu_map

/* Apply relocation when needed */

jal enable_mmu
```

这段省略了寄存器参数准备和编译条件，但总体顺序与 upstream 一致。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有一个重要设计：

`core_init_mmu_map()` 并不是简单地向 `satp` 写一个值。

它需要在软件侧准备好描述内存属性和页表的信息，包括：

- Kernel Code 的地址范围及权限。
- RO Data 的地址范围。
- RW Data、BSS、Stack 和 Heap。
- 必须使用 Identity Mapping 的启动过渡区域。
- 设备寄存器映射。
- 必要的共享内存映射。
- Page Table Root 的物理地址。
- 各个 hart 最初要使用的 `satp` 配置。

当前源码使用 `boot_mmu_config` 保存后续 MMU 切换需要的数据，包括每 hart 的 `satp` 条目和 ASLR Mapping Offset。

## 8.6.4 真正开启 MMU 的关键汇编

这是当前 `entry.S` 中最有价值的一段代码之一。

`enable_mmu` 中的关键逻辑可以简化为：

```
/* Determine current hart index */
csrr a0, CSR_XSCRATCH

/* Locate boot_mmu_config */
lla  a1, boot_mmu_config

/* Locate current hart's satp entry */
addi a1, a1, CORE_MMU_CONFIG_SATP

li   a2, CORE_MMU_CONFIG_SATP_SIZE
mul  a0, a0, a2
add  a1, a1, a0

/* Load and install satp */
LDR  a2, 0(a1)
csrw CSR_SATP, a2

sfence.vma zero, zero
```

这部分对应实际的 `enable_mmu` 代码。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



逐条分析。

`csrr a0, CSR_XSCRATCH`

取得当前 hart 的早期索引。

`lla a1, boot_mmu_config`

取得 MMU 配置数据的地址。

`CORE_MMU_CONFIG_SATP`

由生成的结构偏移宏指向 `boot_mmu_config` 中保存的 `satp` 数据区域。

`CORE_MMU_CONFIG_SATP_SIZE`

用于计算每个 hart 对应 `satp` 条目的步长。

`csrw CSR_SATP, a2`

安装该 hart 的 Root Page Table 和地址转换模式。

`sfence.vma zero, zero`

对本 hart 的地址转换状态执行必要的同步，使前面的页表更新与后续地址转换遵循架构规定。

注意：`SFENCE.VMA` 是本 hart 的操作，不会自动让其他 hart 的 TLB 全部失效。涉及远程 TLB 时需要额外的跨 hart 协调机制，例如 SBI RFENCE。

当前 OP-TEE 在多 hart 配置下还会检查 SBI RFENCE Extension 是否可用，否则无法保证正常的跨 hart Translation Invalidation。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 8.6.5 关键问题：写入 `satp` 后，CPU 为什么没有立即崩溃？

我们设计一个简化实例。

初始状态：

```
PC = 0x80200000
satp.MODE = Bare
```

CPU 正在执行：

```
Physical Address = 0x80200000
```

假设新页表含有以下映射：

| VA                   | PA           | 作用                        |
| -------------------- | ------------ | ------------------------- |
| `0x80200000`         | `0x80200000` | Identity-mapped Boot Code |
| `0xFFFFFFC000000000` | `0x80200000` | Kernel Virtual Address    |

当执行：

```
csrw satp, a2
```

之后，当前 PC 附近的地址仍有一条有效的 Identity Mapping：

```
VA 0x80200000
    |
    v
PA 0x80200000
```

因此能够继续执行。

随后执行 `sfence.vma`，并通过预先设计好的跳转或返回地址转移到正常的 Kernel Virtual Address 区域。

这就是 Bootstrap Address Space 的意义。

### 为什么 Return Address 也需要处理？

假设：

```
Before ASLR:
    ra = 0x80201234

ASLR Offset:
    +0x200000
```

新的执行地址布局要求：

```
After ASLR:
    ra = 0x80401234
```

如果 `ra` 没有相应调整，那么函数执行 `ret` 时，可能返回到旧地址空间。

如果旧映射已经失效，就可能发生 Instruction Page Fault。

OP-TEE 当前 `enable_mmu` 在启用 Core ASLR 时会调整：

```
add gp, gp, a3
add tp, tp, a3
add sp, sp, a3
add ra, ra, a3
```

其中 `a3` 保存 Mapping Offset。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这几个寄存器正是前面章节讨论的关键 Runtime State。

换句话说：

MMU 切换不只是写入 `satp`，还必须确保所有与执行相关的指针都遵循新的地址空间语义。

## 8.6.6 Core ASLR 的实际实现

当前 OP-TEE 的 `CFG_CORE_ASLR` 支持 RISC-V Core Virtual Address Randomization。

开启后，编译阶段使用 Position-Independent 机制，链接阶段引入 PIE 和动态重定位信息。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



其基本流程：

```
Primary Boot
    |
    +-- Determine ASLR seed
    |
    +-- Construct randomized VA mapping
    |
    +-- Apply dynamic relocations
    |
    +-- Enable MMU
    |
    +-- Adjust relevant register pointers
    |
    +-- Execute at randomized VA
```

当前默认的 `get_aslr_seed()` 路径会优先考虑启用且可用的 RISC-V Zkr 硬件随机源，并在相应条件下回退到 Device Tree 或平台实现提供的种子。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有两个需要特别注意的安全问题。

第一，ASLR 是安全加固，不是安全隔离。

即使攻击者知道 OP-TEE 的虚拟地址布局，也不应该因此获得访问 OP-TEE 物理内存的能力。根本隔离仍需要可靠的安全边界。

第二，种子的来源及其保密性决定 ASLR 的实际价值。

如果 ASLR Seed 来自 REE 可任意控制的数据，且缺少可信约束，那么随机化本身可能失去预期的安全意义。

## 8.6.7 MMU Bring-up 的常见故障分类

| 故障现象                    | 最可能首先检查的内容                            |
| ----------------------- | ------------------------------------- |
| `satp` 写入后立即崩溃          | Identity Mapping、根页表、MODE             |
| `sfence.vma` 后异常        | 页表属性、PTE 有效性、取指权限                     |
| 第一次 `ret` 后崩溃           | `ra` 是否仍指向有效 VA                       |
| 第一次访问全局变量崩溃             | `gp`、重定位、RW Mapping                   |
| 第一次进入 C 函数崩溃            | Stack VA、对齐、ABI                       |
| Primary 正常，Secondary 崩溃 | 每 hart `satp`、`tp/sp` 和同步             |
| 只在开启 ASLR 后崩溃           | Relocation、Pointer Adjustment、MMIO VA |

这也是我建议你优先使用 GDB 在 `enable_mmu` 前后设置断点的原因。

# 8.7 BSS、Stack、Heap 与 Per-Hart Data 初始化

## 8.7.1 BSS 为什么一定需要清零？

根据 C 语言静态存储期对象的初始化规则：

```
static int counter;
static void *global_pointer;
```

这些未显式初始化的对象应具有零初始化值。

在 Embedded Kernel 中，不能假设内存上电后天然为零。

尤其是 DRAM，可能保留之前运行的数据，或者在尚未初始化/校验之前包含不确定内容。

因此 Boot 必须显式满足这个初始化要求。

### 实际 BSS 清零汇编

`reset_primary` 包含：

```
lla t0, __bss_start
lla t1, __bss_end

beq t0, t1, 1f

0:
    STR zero, (t0)
    add t0, t0, RISCV_XLEN_BYTES
    bne t0, t1, 0b

1:
```

这里 `STR` 是源码宏，根据 XLEN 选择对应的 Store 操作。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



对 RV64 来说，核心思想相当于使用 64-bit Store：

```
sd zero, 0(t0)
```

然后每次增加 8 字节。

源码的链接布局保证了对应边界满足该循环的预期对齐及长度要求。

### 为什么只有 Primary 清零 BSS？

如果多个 hart 同时清零共享 BSS，就可能覆盖另一个 hart 已经初始化的全局变量。

例如：

```
static bool mmu_initialized;
```

Primary 将其设置为 `true`，而 Secondary 随后又执行 BSS 清零。

结果可能导致 Primary 再次认为 MMU 未初始化。

更严重的是，如果 BSS 中包含已初始化的指针、锁或状态机变量，就可能导致内存破坏。

所以：

Global BSS 初始化属于 One-time Initialization，而不是每个 hart 都需要重复的工作。

## 8.7.2 为什么 Hart Lottery 不能放在普通 BSS 中随意清零？

这是一个很好的源码设计问题。

当前 `hart_lottery` 被放在：

```
.section .identity_map.data
```

而不是普通的 `.bss` 中。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



原因是 `_start` 最早就通过原子操作修改了它。

如果随后 Primary 清零 BSS，恰好把这个 Lottery 状态也清掉，Secondary 又进入 `_start`，就可能错误地认为自己是第一个 hart。

最终可能产生两个 Primary。

因此 Lottery 的存放位置、初始值和重启生命周期，必须与 BSS 初始化严格区分。

这一点还能延伸到 Warm Reset：

如果 `.data` 区域没有被重新加载，或者复位域没有清除 Lottery 状态，那么下一次启动就可能失去正确的 Primary Election 语义。

这是一个真实芯片平台需要明确验证的问题。

## 8.7.3 OP-TEE 为什么需要不止一种 Stack？

与简单 Bare-metal 程序相比，OP-TEE 需要应对更复杂的执行场景。

包括：

- Boot 期间执行初始化代码。
- Thread 正常运行。
- 执行 Trap Handler。
- Trusted Thread Suspend/Resume。
- 发生异常或崩溃时保存状态。

因此不能只定义一个全局 Stack。

可以从逻辑上区分：

| Stack                   | 使用场景                       | 特点                  |
| ----------------------- | -------------------------- | ------------------- |
| Temporary Stack         | Early Boot、无正常 Thread 的上下文 | 与 Hart 关联           |
| Abort / Exception Stack | 异常处理相关路径                   | 避免不安全地继续使用故障 Stack  |
| Trusted Thread Stack    | 正常 Kernel Thread 执行        | 与 Trusted Thread 关联 |
| User TA Stack           | TA 的 U-mode 执行             | 受 User TA MMU 隔离    |

需要注意，真实 Trap Handler 是否立即切换到专门异常栈，以及哪些中断路径继续使用当前栈，要以具体的架构实现为准。

不能假设每发生一次 Trap 都必定切换到完全独立的栈。

### Stack Canary

当前 `boot.c` 还包含：

```
thread_init_canaries();
```

以及在运行时初始化中调用：

```
thread_update_canaries();
```

分别用于初始化和更新 Stack Canary 相关保护数据。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



这里可以研究一个安全设计问题：

为什么有些保护机制需要在 Early Boot 先建立，然后等 Crypto RNG 可用后重新初始化？

因为 Boot 早期可能尚未完成加密服务初始化，不能默认所有高质量随机源已经可用。

## 8.7.4 Heap 如何从不可用变成可用？

我们前面说过，`_start` 不能随便调用 `malloc()`。

但当 OP-TEE 进入更高层初始化时，动态内存管理已经是必要能力。

当前 `init_primary()` 中存在：

```
malloc_add_pool(
    __heap1_start,
    __heap1_end - __heap1_start
);
```

用于将初始内存区域加入 Allocator。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



此外，启动路径还使用 `boot_mem_init()` 建立专门的 Boot Memory 管理机制。

为什么要分别考虑 Boot Memory 和 Runtime Heap？

因为不同阶段的约束不同。

Early Boot Allocation

需要尽可能简单、可预测，有时要求避免复杂锁和 Thread 状态依赖。

Runtime Allocation

需要支持多个调用者、内存释放、不同安全服务和更完整的分配策略。

当前代码还会把早期分配阶段产生的可用 Padding 等资源加入 Heap，并释放不再需要的 Boot 临时分配区域。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这体现了一个很好的设计理念：

> 内核启动过程不仅逐步创建资源，也应该在资源完成使命后释放不再需要的临时状态。

## 8.7.5 Per-Hart Data 何时正式建立？

当前动态配置中，Early Boot 使用临时 `thread_core_local`，后续再为多个 hart 分配正式数组。

其初始化过程包含：

```
thread_init_threads(CFG_NUM_THREADS);
thread_init_boot_thread();
thread_init_thread_core_local(CFG_TEE_CORE_NB_CORE);
```

这是 `boot_init_primary_late()` 的实际代码。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



之后在 `entry.S` 的动态配置分支，更新 `thread_core_count`、`thread_core_local` 指针，并重新计算当前 hart 的 `tp` 和临时栈位置。

这意味着：

同一个 Primary Hart 的 `tp` 在 Boot 过程中也可能发生变化。

不能因为 GDB 第一次看到 `tp` 指向某个地址，就假设直到系统运行结束它都不会改变。

## 8.7.6 什么时候可以认为 Thread 子系统已经初始化？

至少需要区分三类状态：

1. Thread Pool 的共享数据结构已经建立。
2. 当前 hart 的 Thread-local / Hart-local 执行状态已经建立。
3. Normal Trusted Thread Runtime 已经可以接受请求。

这三个条件不一定在同一时间成立。

尤其是 Boot Thread：

```
thread_init_boot_thread();
```

与正常处理 REE 请求时获取的 Trusted Thread，在生命周期和使用目的上存在区别。

不要把完成 `thread_init_threads()` 等同于整个 OP-TEE 已经能够安全处理所有 TEE 调用。

# 8.8 从 Assembly 进入 C Runtime

到这里我们已经建立了：

- 有效的 `gp`。
- 当前 hart 的 `sp` 和 `tp`。
- 初始化后的 BSS。
- 可执行的 Kernel VA Mapping。
- 初步可用的内存分配设施。
- 能够继续初始化内核的基本环境。

现在终于可以讨论一个问题：

为什么 Early Boot 可以安全地进入普通 C 函数？

## 8.8.1 RISC-V 上调用 C 函数需要什么条件？

先回顾 RISC-V psABI。

调用普通 C 函数时，至少要保证：

| 条件                  | 原因                                        |
| ------------------- | ----------------------------------------- |
| `sp` 有效且满足 ABI 对齐要求 | 编译器需要创建栈帧                                 |
| `gp` 正确             | 代码可能使用 Global Pointer Relative Addressing |
| `tp` 符合运行时约定        | Core 代码可能需要访问 Hart-local 状态               |
| `.data` 可访问         | 已初始化的全局变量需要正常工作                           |
| `.bss` 已初始化         | 静态存储期对象需要满足 C 语言语义                        |
| 代码和数据 Mapping 正确    | CPU 必须能够取指、读写                             |
| ABI 与 Toolchain 一致  | 参数传递、栈布局、调用约定必须匹配                         |
| 必要的异常处理能力已经建立       | 避免运行阶段发生异常时失去控制                           |

对于常见的 RV64 LP64 ABI，Stack Pointer 在函数调用边界通常需要满足 16-byte Alignment。

但需要注意，在完成全部 Trap Runtime 初始化之前，Early C Code 依然有约束：

它不能假设完整中断体系、Scheduler、Trusted Thread 或所有驱动都已经可用。

所以「能够执行 C 代码」不代表「能够调用任何内核 API」。

## 8.8.2 `jal` 指令如何进入 C 函数？

例如：

```
jal boot_init_primary_early
```

其概念性行为为：

```
ra ← address of next instruction
PC ← address of boot_init_primary_early
```

对于目标函数：

```
void boot_init_primary_early(void)
{
    init_primary();
}
```

编译器可能生成如下形式的函数序言和返回代码：

```
addi sp, sp, -16
sd   ra, 8(sp)

call init_primary

ld   ra, 8(sp)
addi sp, sp, 16
ret
```

这只是用于解释 C ABI 的教学反汇编，实际优化后可能进行 Tail Call、函数内联或省略栈帧。

但它揭示了一个重要事实：

一旦开始普通的 C 函数调用，`sp` 和 `ra` 的正确性就成为系统执行的基本前提。

如果 `sp` 错了，保存 `ra` 时就可能破坏内存；如果 `ra` 错了，函数返回时就会跳到错误地址。

## 8.8.3 RISC-V OP-TEE 实际从哪里进入 C？

根据当前源码，Primary 的关键执行顺序如下。

```
entry.S: reset_primary
    |
    +-- boot_save_args()
    |
    +-- sbi_init() [conditional]
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
    |      |
    |      +-- init_primary()
    |
    +-- boot_init_primary_late()
    |
    +-- Switch to boot thread stack
    |
    +-- boot_init_primary_runtime()
    |
    +-- boot_init_primary_final()
    |
    +-- Start secondary harts
    |
    +-- Synchronization / Return
```

这里有一点需要澄清：并不是所有 C 函数都在 MMU 开启之后才执行。

当前代码在 MMU 开启之前就已经调用了 `boot_save_args()`、`sbi_init()`、`console_init()`、`boot_mem_init()` 等函数。

但这些属于受 Early Boot 环境约束的代码。

完成 MMU 切换后，代码才逐步进入更加完整的内核初始化阶段。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



这也解释了为什么 OP-TEE 初始化不是简单的：

```
_start → main()
```

而是分为多个阶段。

## 8.8.4 `boot_init_primary_early()` 做什么？

当前实现：

```
void boot_init_primary_early(void)
{
    init_primary();
}
```

其中 `init_primary()` 执行：

- Mask 相关异步异常。
- 初始化基础 Heap。
- 检查多 hart SBI RFENCE 能力。
- 保存和建立内存管理状态。
- 初始化 Stack Canary。
- 初始化 Hart-local Thread State。

其中：

```
thread_init_per_cpu();
```

非常重要。

它会安装当前 hart 的 Trap Vector，并建立正常 Trap Runtime 所需要的 CSR 状态。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



### 为什么还要重新配置 Trap Vector？

因为 Early Boot 与正常 Runtime 的异常处理需求不同。

在 Early Boot 中，可能还没有完整可用的异常上下文。

进入 Runtime 后，需要处理：

```
User TA ECALL
Page Fault
Illegal Instruction
External Interrupt
Timer Interrupt
Kernel Exception
```

其中不同异常可能具有不同的处理逻辑。

因此，必须建立正常运行阶段的 Trap Entry。

不过 Trap/Exception 的具体保存路径我们将在 Chapter 09 深入，这里先掌握初始化顺序。

## 8.8.5 `boot_init_primary_late()` 做什么？

这个函数实际包含以下关键调用：

```
init_external_dt(fdt, CFG_DTB_MAX_SIZE);
discover_nsec_memory();
update_external_dt();

thread_init_threads(CFG_NUM_THREADS);
thread_init_boot_thread();
thread_init_thread_core_local(
    CFG_TEE_CORE_NB_CORE);
```

它体现了两个工作方向。

一是完成 Platform / Memory Discovery。

需要解析 Device Tree、发现非可信内存区域，并根据 OP-TEE 的布局要求处理相关 DT 信息。

注意，Device Tree 中的 Reserved Memory 描述，不等于硬件已经通过 PMP/Firewall 建立了安全隔离。

Device Tree 是资源描述；真正的隔离必须由可信硬件策略保障。

二是完成 Thread Runtime 的基础数据结构初始化。

包括 Thread Pool、Boot Thread 和 Hart-local State。

这为后续切换到正式 Boot Thread Stack 提供基础。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 8.8.6 为什么还需要切换到 Boot Thread Stack？

这是本章一个容易被忽略的重要细节。

在 `boot_init_primary_late()` 返回后，当前汇编代码会做类似以下操作：

```
mv s2, sp

lla a0, threads
LDR a0, 0(a0)
LDR a0, THREAD_CTX_STACK_VA_END(a0)

mv sp, a0
```

这段代码的意思是：

1. 将当前临时 Stack Pointer 保存到 `s2`。
2. 取得 Boot Thread 对应的 Stack Top。
3. 将 `sp` 切换到该 Stack。
4. 继续调用后续 Runtime Initialization。

随后执行：

```
jal boot_init_primary_runtime
jal boot_init_primary_final
```

当前源码还会调整 `THREAD_CLF_TMP` 等状态，用来表示当前 hart 是否仍在临时栈上下文中。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



为什么要这样设计？

因为有些内核操作要求当前正在使用正常的 Trusted Thread Context。

如果仍处于 Early Temporary Stack，部分 Thread API 的假设可能不成立。

所以切换 Stack 不只是内存位置变化，它也是一次执行上下文状态转换。

这一点与你在 NuttX/RTOS 中接触过的 Idle Task、Bootstrap Task 和 Scheduler Initialization 很相似。

## 8.8.7 为什么不直接调用 `main()`？

传统 Bare-metal Firmware 常见：

```
Reset
  |
  v
Startup Assembly
  |
  v
main()
```

但 OP-TEE 是带有多种安全执行上下文的内核，其初始化具有更复杂的依赖关系。

例如：

```
Page Table
    ↓
Heap
    ↓
Thread Pool
    ↓
Boot Thread
    ↓
Crypto / Services
    ↓
Drivers
    ↓
Ready
```

这并不是说所有初始化工作严格按照这张图中的顺序执行，而是说明它们之间存在需要管理的资源依赖。

如果将所有逻辑放进一个 `main()`，很容易造成：

- 初始化顺序混乱。
- Platform Hook 与 Generic Core Code 耦合。
- 不同平台重复实现同样的初始化步骤。
- 新增 Driver 时难以判断安全的初始化位置。

因此，OP-TEE 使用分阶段 Boot Initialization 和 Initcall 框架来处理这一问题。

# 8.9 Initcall 与 OP-TEE Core 子系统初始化

这是从「启动汇编」走向「真正可用的 TEE Kernel」的最后一步。

## 8.9.1 什么是 Initcall？

Initcall 是一种内核初始化注册机制。

它的核心思想不是由一个函数直接硬编码调用所有模块：

```
void kernel_init(void)
{
    init_a();
    init_b();
    init_c();
}
```

而是让各子系统使用宏将初始化函数注册到指定的 Initcall Section。

例如：

```
static TEE_Result my_service_init(void)
{
    return TEE_SUCCESS;
}

service_init(my_service_init);
```

这样初始化框架就能在 Service Init 阶段调用该函数。

当前 OP-TEE 使用 `scattered_array` 等机制来注册有序的 Initcall 项。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 8.9.2 Initcall 数据结构是什么？

在 `core/include/initcall.h` 中，可以看到：

```
struct initcall {
    TEE_Result (*func)(void);

#if TRACE_LEVEL >= TRACE_DEBUG
    int level;
    const char *func_name;
#endif
};
```

其核心数据是一个函数指针。

Debug 配置下还可以保存 Initcall Level 和函数名称，以便 Trace。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



从本质上来说，这是一组由 Linker 和构建系统组织的初始化函数记录。

因此，Initcall 机制同时涉及：

- C Function Pointer。
- Macro Expansion。
- ELF Section。
- Linker Layout。
- Runtime Iterator。

这也是为什么学习内核源码时，理解 Linker Script 特别重要。

## 8.9.3 OP-TEE 的 Initcall 分为哪些阶段？

当前主要阶段如下：

| 阶段           | 典型注册宏            | 作用          |
| ------------ | ---------------- | ----------- |
| Preinit      | `preinit()`      | 非常早期的子系统初始化 |
| Early Init   | `early_init()`   | 依赖较少的早期内核模块 |
| Service Init | `service_init()` | 安全服务及相关基础设施 |
| Driver Init  | `driver_init()`  | 驱动和设备相关初始化  |
| Final Init   | `boot_final()`   | 启动末期工作      |

此外，还存在：

```
preinit_early()
preinit_late()

early_init_late()

service_init_crypto()
service_init_late()

driver_init_late()
release_init_resource()
```

等用于进一步明确顺序的宏。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 为什么 Crypto 需要独立考虑初始化顺序？

假设某项安全服务需要调用：

```
crypto_rng_read(...)
```

那么它就依赖 Crypto Framework 和 RNG 相关初始化已经完成。

如果在 Crypto Framework 初始化之前使用该 API，可能失败，甚至在错误实现中产生安全问题。

因此，Initcall 顺序不仅是软件工程问题，还可能影响 Security Properties。

例如，Stack Canary 的随机化就可能依赖 Crypto RNG 的可用性。

## 8.9.4 RISC-V OP-TEE 的实际 Initcall 顺序

在当前 `boot.c` 中：

```
void init_tee_runtime(void)
{
    call_preinitcalls();
    call_early_initcalls();
    call_service_initcalls();

    thread_update_canaries();
}
```

之后在 `boot_init_primary_final()` 中：

```
call_driver_initcalls();
call_finalcalls();
```

这是目前代码中的实际组织方式。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



可以整理为：

```
boot_init_primary_runtime()
    |
    +-- thread_init_primary()
    |
    +-- Hart Discovery
    |
    +-- Interrupt Controller Init
    |
    +-- Hart Features Init
    |
    +-- init_tee_runtime()
    |       |
    |       +-- preinitcalls
    |       +-- early_initcalls
    |       +-- service_initcalls
    |       +-- Update Stack Canaries
    |
    +-- SBI MPXY Init / RPMI Probe

boot_init_primary_final()
    |
    +-- driver_initcalls
    |
    +-- finalcalls
    |
    +-- Boot Completion
```

这里还有两个值得注意的问题。

第一，Primary Runtime 先执行 Initcall，之后再完成部分 SBI MPXY 探测工作。

因此，如果某个新服务依赖该特定初始化结果，不能直接假设它已经在 Service Init 阶段可用。

第二，Initcall 分组本身只是约定与排序机制，不会自动进行复杂的依赖解析。

如果模块 A 依赖模块 B，那么必须由开发者在初始化阶段、优先级及 Platform Configuration 中明确处理。

## 8.9.5 Initcall 具体怎么执行？

当前 `core/kernel/initcall.c` 的关键逻辑如下：

```
for (call = begin; call < end; call++) {
    ret = call->func();

    if (ret) {
        /* Log initialization failure */
    }
}
```

这是从实际代码提取的简化逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



需要特别强调：

当前 `do_init_calls()` 对返回错误的 Initcall 会记录日志，并继续遍历后续 Initcall，而不是统一直接 `panic()`。

这一点在安全系统设计中非常值得讨论。

假设一个与 Root of Trust 密切相关的初始化函数失败：

```
TEE_Result secure_key_init(void)
{
    if (!hardware_key_available())
        return TEE_ERROR_SECURITY;

    return TEE_SUCCESS;
}
```

如果框架仅记录错误并继续执行，那么后续模块是否仍可能以不完整的安全状态提供服务？

这就涉及两种错误处理策略：

- Fail Open：出现错误后继续提供某种程度的服务。
- Fail Closed：无法建立必要安全保证时，拒绝进入不安全的运行状态。

对于关键安全服务，必须明确失败后的行为，不能简单地认为返回非零值就必然阻止系统启动。

在实际设计中，可能需要通过显式状态检查、依赖项失败传播或必要时的 `panic()` 来确保关键安全前提得到满足。

## 8.9.6 如何判断 OP-TEE 已经 Ready？

不能仅凭串口打印：

```
OP-TEE initialized
```

就认为完整 TEE 功能一定可用。

应该根据不同层次定义验收条件。

| 层次                  | 成功判据                                       |
| ------------------- | ------------------------------------------ |
| Boot Entry          | 已正确执行 `_start`                             |
| Core MMU            | 页表启用且 Kernel Code/Data 可访问                 |
| Thread Runtime      | Boot Thread、Thread Pool、Per-Hart State 已建立 |
| Interrupt / Trap    | 正常 Trap Vector 已安装                         |
| Service Runtime     | 关键 Initcall 成功                             |
| Monitor Integration | OP-TEE 完成与底层固件的入口交接                        |
| Linux Integration   | REE 能发现并连接 TEE                             |
| Functional          | 基础 TA 调用和 xtest 能成功                        |
| Security            | 隔离策略和非法访问测试通过                              |

当前 Primary 完成初始化后，会通过 `thread_return_to_udomain` 路径向相应的底层执行环境返回状态；其中不同配置下可能涉及 OP-TEE ABI 或 M-mode Monitor 的特定协议。

因此，Boot Completion 不应被理解为普通 C 程序中的 `return 0`。

# 8.10 将 Chapter 08 串成完整源码执行链

现在把前面所有部分综合起来。

```
OpenSBI / Trusted Firmware
    |
    | Prepare S-mode execution
    | Set target Entry / privilege / domain policy
    v
OP-TEE: _start
    |
    | Initialize gp
    | Save hart ID and DTB
    | Atomic Primary Election
    |
    +------------------------------+
    |                              |
    v                              v
reset_primary                 reset_secondary
    |                              |
    | Clear BSS                    | Wait Primary
    | Initialize sp/tp             |
    | Save Args                    |
    | SBI / Console                |
    | Boot Memory                  |
    | Construct MMU Map            |
    |                              |
    v                              |
enable_mmu                        enable_mmu
    |                              |
    | satp / sfence.vma             | Own satp
    | Relocation adjustments       | Own sp/tp
    v                              |
boot_init_primary_early()         |
    |                              |
    | Heap / Canaries / Per-Hart    |
    v                              |
boot_init_primary_late()          |
    |                              |
    | Device Tree / Thread Pool    |
    v                              |
Switch to Boot Thread Stack       |
    |                              |
    v                              |
boot_init_primary_runtime()       |
    |                              |
    | Hart / INTC / Initcall       |
    v                              |
boot_init_primary_final()         |
    |                              |
    | Driver Initcalls             |
    v                              |
Start Secondary Harts ------------+
    |                              |
    |                        boot_init_secondary()
    |                              |
    |                        Per-Hart Trap / INTC
    |                              |
    +-------------+----------------+
                  |
                  v
          Boot Synchronization
                  |
                  v
         OP-TEE Runtime Ready
                  |
                  v
        Monitor / Domain Return
```

这里最重要的是能够分辨四种初始化状态：

- Global State：整个 OP-TEE Core 共享。
- Per-Hart State：每个 hart 独立。
- Per-Thread State：每个 Trusted Thread 独立。
- Per-TA State：每个 User TA 或 TA Context 独立。

Chapter 08 主要处理前两种；Chapter 10、11、12 将深入后两种。

# 8.11 实战实验：从 ELF Entry 跟踪到 OP-TEE Ready

我建议把本章学完后的实践设计成四个连续实验。

## Lab 1 — ELF / Linker Inspection

目标：证明你真正理解 OP-TEE 的链接和加载模型。

在已经完成编译的 RISC-V OP-TEE 源码目录中：

```
ELF=out/core/tee.elf

riscv64-linux-gnu-readelf -h "$ELF"
riscv64-linux-gnu-readelf -l "$ELF"
riscv64-linux-gnu-readelf -S "$ELF"

riscv64-linux-gnu-nm -n "$ELF" \
  | grep -E '_start|enable_mmu|__bss|__heap'

riscv64-linux-gnu-objdump -dr "$ELF" \
  > /tmp/optee-riscv-disasm.txt
```

`out/core/tee.elf` 是常见的输出路径；如果你的构建使用其他 `O=` 或构建系统，应以实际生成路径为准。

验收条件：

能够明确解释 `_start` 的 ELF Entry、Identity-mapped Code、BSS、Heap 和 `enable_mmu` 的关系。

## Lab 2 — 逐指令跟踪 `_start`

假设 QEMU 使用 GDB Stub：

```
qemu-system-riscv64 \
    ... \
    -S -gdb tcp::1234
```

这里省略实际 Machine、内存、OpenSBI 和镜像参数；应使用 Chapter 03 已完成并可启动的 QEMU 命令。

连接 GDB：

```
gdb-multiarch out/core/tee.elf
```

```
target remote :1234

break _start
break reset_primary
break reset_secondary

continue
```

如果某些符号是 Local Symbol，或者符号运行地址与实际加载地址存在重定位差异，需要改用实际地址断点。

重点观察：

```
info registers

p/x $a0
p/x $a1

p/x $gp
p/x $sp
p/x $tp

p/x $satp
p/x $sstatus

x/12i $pc
```

验收条件：

能够解释从 `_start` 到 `reset_primary` 之间，每条关键指令为什么存在。

## Lab 3 — MMU Transition Debug

在 `core_init_mmu_map` 和 `enable_mmu` 附近设置断点。

建议分别记录：

```
Current PC
Current SP
Current GP
Current TP

satp before
satp after

Root Page Table PA
ASLR Offset

Identity Mapping
Kernel VA Mapping
```

逐步执行：

```
stepi
```

在执行 `csrw satp` 和 `sfence.vma` 前后，观察 PC、Stack 和相关 Memory Mapping。

注意：GDB 对地址翻译和内存访问的显示方式可能受到 QEMU Stub 行为影响。必要时需要结合 QEMU Monitor 或实际内存数据确认，不能只根据 GDB 是否能读取某地址就断言安全域访问权限正确。

验收条件：

能够解释 为什么 `satp` 改变后仍能够继续执行，以及 `ra/sp/gp/tp` 在 ASLR 下如何保持有效。

## Lab 4 — Multi-Hart Boot

使用多个 QEMU hart，确保 OP-TEE 的构建配置也与之匹配。

例如在合适的构建及固件支持下，可以设置：

```
CFG_TEE_CORE_NB_CORE = 4
CFG_NUM_THREADS = 4
CFG_BOOT_SYNC_CPU = y
```

具体如何传递变量及最终支持的 hart 数量，需要与你使用的 Buildroot/OpenSBI/OP-TEE 分支配套。

重点验证：

- Primary Hart 的实际 Hart ID 是多少？
- Primary 的 Logical Index 是否为 0？
- Secondary 是否经过 SBI HSM 启动？
- 每个 hart 是否拥有不同的 `sp/tp`？
- 每个 hart 是否独立写入 `satp`？
- 是否执行 `thread_init_per_cpu()`？
- Boot Barrier 是否使用实际启动的 hart 数量？

再尝试故障注入：修改候选 hart 配置，使部分 hart 不可启动，观察系统是正确跳过、报错还是死锁。

工业价值： 这一实验与你未来在 16-hart RISC-V Server SoC 上处理固件启动问题高度相关。

# 8.12 Staff Engineer 级别面试题与工业场景分析

下面设计 15 道问题。难度不是考察某个宏的名称，而是要求综合 RISC-V Architecture、OpenSBI、OP-TEE Kernel 和实际硬件执行行为。

每道题后面提供参考分析要点，便于自测。

## A. Low-Level Boot & ABI

### Q1. OP-TEE 可以执行 `_start`，但进入第一个 C 函数时立即发生 Store Access Fault。你如何排查？

参考分析：

首先确认 `sp` 是否指向有效的可写地址，并检查栈对齐、当前 PMP/Domain Memory Permission、MMU 状态与 Stack VA Mapping。

其次，通过反汇编确定出错 Store 是保存 `ra`、`s0`，还是普通全局数据访问。

如果出错指令是：

```
sd ra, 8(sp)
```

优先检查 Stack Mapping。

如果是写全局变量，则检查 `gp`、符号重定位和 RW Memory Permission。

还应确认：

- 前级固件是否正确配置安全内存权限。
- Stack 是否与其他 hart 重叠。
- Linker Script 的地址与实际加载地址是否一致。
- Trap Dump 中的 `scause`、`stval`、`sepc` 是否与故障指令一致。

高水平回答必须根据实际 Faulting Instruction 和 Fault Address 缩小范围，而不是笼统地归因于 MMU。

### Q2. 为什么 `gp` 初始化必须使用 `.option norelax`？如果移除，什么情况下会出问题？

参考分析：

Linker Relaxation 可能将部分符号寻址转换成依赖 `gp` 的形式。

初始化 `gp` 的代码如果本身依赖尚未建立的 `gp`，就可能计算出错误的全局指针。

问题可能受 Toolchain、Optimization、Linker Flags 和符号布局影响，导致 Debug 与 Release Build 表现不同。

正确的诊断应包括对比最终 ELF 的反汇编，而不仅是查看汇编源文件。

### Q3. 一个 Boot Image 的 ELF Entry Point 为 `0x80200000`，实际 Loader 却将原始 Binary 加载在 `0x80400000`。系统是否一定无法启动？

参考分析：

不一定，关键取决于镜像是否支持该加载位置，以及实际执行时的地址计算与重定位处理。

如果代码使用绝对链接地址而没有相应重定位，通常会出现错误。

如果镜像是支持正确重定位的 Position-Independent Image，且 Loader/Boot Protocol 与其匹配，则可以在不同位置运行。

需要区分：

- Link-time VMA。
- Load-time Physical Address。
- ELF Entry 与 Binary Entry Offset。
- Relocation Base。
- Identity Mapping。
- ASLR Virtual Offset。

不能仅因镜像配置了 ASLR，就认定任意改变物理加载地址都是安全的。

## B. MMU & Address Space Transition

### Q4. 在 `enable_mmu` 执行 `csrw satp` 后立即出现 Instruction Page Fault。请设计完整的排查流程。

参考分析：

应该分层检查：

1. `satp.MODE` 是否为硬件支持的模式。
2. Root Page Table PPN 是否有效。
3. 当前过渡指令的 VA 是否能够在新页表中翻译。
4. PTE 的 V/R/W/X/U 等权限是否合法。
5. Root Page Table 物理内存是否可由当前 Domain 读取。
6. 页表内存写入与地址转换同步是否正确。
7. 如果执行中开启了 ASLR，Mapping Offset 是否正确。

再结合 `sepc`、`stval`、`scause` 以及 Root Page Table Walk，明确是映射错误、权限错误，还是执行地址错误。

如果当前 Trap Vector 尚不可用，可能无法获得完整异常日志，因此需在 MMU 切换之前通过 GDB 或其他可靠调试机制保存必要状态。

### Q5. OP-TEE 已开启 MMU，但为什么 Linux 仍然不能仅凭页表机制就被认为与 OP-TEE 安全隔离？

参考分析：

因为不同 S-mode Domain 可以独立控制自己的页表。

如果 Linux 有能力访问 OP-TEE 的物理内存，那么它可以创建映射指向 OP-TEE 内存的 PTE。

因此必须有 Linux 无法修改或绕过的更高可信隔离机制，例如由 M-mode 管理的 PMP Policy，或相应的 SoC Hardware Isolation。

另外还需要考虑 DMA Master 和外设访问，不能只保护 CPU 的 Load/Store。

### Q6. 为什么修改 OP-TEE 的 Root Page Table 后不能只调用 `SFENCE.VMA` 就保证所有 hart 都看到了更新？

参考分析：

`SFENCE.VMA` 的作用范围是本 hart 的地址转换状态。

其他 hart 可能仍然持有旧 TLB Entry。

因此在 SMP 下，需要结合 Memory Ordering 和 Remote TLB Invalidation Protocol。

RISC-V 平台可以通过 SBI RFENCE 等机制完成远程同步。

但安全的正确性还要求分析页表修改与失效操作的先后顺序，尤其是撤销 Mapping/权限时，不能允许其他 hart 继续使用过期授权。

## C. SMP & Primary/Secondary Boot

### Q7. 一个 16-hart SoC，Hart ID 为 `{0, 2, 4, ..., 30}`。OP-TEE 启动代码直接使用 Hart ID 计算 Boot Stack Offset。可能出现什么问题？

参考分析：

如果用 Hart ID 直接作为数组索引，就可能访问超过实际分配的 Per-Hart Stack Array。

例如只为 16 个 hart 分配 Stack，却试图访问 Index 30。

正确方案是定义并验证 Hardware Hart ID 到 Logical Core Index 的映射。

同时需要考虑：

- Primary Election 顺序。
- Device Tree 的 Hart 列表。
- Hart Ownership。
- 动态 Hart Assignment。
- 异常处理中的 Hart-local 状态寻址。

### Q8. Primary Hart 初始化完成并开始等待 Secondary，但 Secondary 永远没有进入 OP-TEE。你如何诊断？

参考分析：

先确定 Primary 是否调用了 `sbi_hsm_hart_start()`。

检查：

- SBI HSM Extension 是否支持。
- Hart 是否属于当前 Domain。
- Hart 当前是否处于 STOPPED 状态。
- Start Address 是否有效。
- Secondary 是否在 OpenSBI 层 Trap 或异常。
- Secondary 的 PMP Policy 是否允许读取 OP-TEE Image。
- Secondary 是否卡在 `wait_primary` 或 MMU 初始化。

之后检查 `boot_hart_count` 和 Ready Flag。

一个很关键的设计问题是：Boot Barrier 是否包含从未成功启动的 hart？

### Q9. 如果 Primary 清零 BSS 与 Secondary 初始化并发执行，会发生什么？如何设计同步？

参考分析：

Secondary 可能读取到未初始化或被重新清零的共享状态。

尤其是 Global Pointer、Thread Pool、页表、锁和 Per-Hart Array。

需要保证 Secondary 使用共享状态之前，Primary 已经完成必要的初始化，并通过具备正确 Memory Ordering 语义的同步机制发布 Ready 状态。

仅凭一个普通循环读取 Flag，不一定足以在所有内存模型和内存属性下形成正确的同步协议。

### Q10. 某平台使用 Software Reset 而不是全芯片 Power-on Reset。第二次启动时 OP-TEE 偶尔出现两个 Primary。你认为最值得怀疑什么？

参考分析：

首先检查 `hart_lottery` 的初始值和所在的 Memory Section。

如果 Reset 没有重新加载初始化数据，或者 Lottery 在 Warm Reset 后保留旧值，那么 Primary Election 语义就可能失效。

同时检查：

- 各 hart 是否同时重新进入 `_start`。
- 旧的 Secondary 是否仍在执行。
- Reset Domain 是否覆盖相关 DRAM。
- PMP、MMU 和 Cache 状态是否被重置。
- 是否存在旧的 per-hart 状态残留。

应该将 Cold Boot 与 Warm Boot 定义成不同的系统状态转换，并明确入口合同。

## D. Security & Runtime Initialization

### Q11. 某个 HSM Driver 的 `service_init()` 返回 `TEE_ERROR_SECURITY`，但是系统依然打印 OP-TEE Ready。这是否正常？会造成什么风险？

参考分析：

根据当前 Initcall 框架，初始化函数返回错误后，框架会记录错误并继续执行后续项。

因此仅仅返回失败值，不一定导致系统停止启动。

其风险取决于这个 Driver 是否为强制安全依赖。

如果某个服务在 HSM 不可用时仍然继续执行，而且缺少明确失败检查，就可能破坏预期的密钥保护或安全策略。

Staff Engineer 需要定义：

- 哪些服务是强制依赖。
- 哪些失败允许降级。
- 降级是否影响安全承诺。
- 如何在系统 Readiness 和 Attestation 中表达实际状态。
- 是否需要 Fail-closed。

### Q12. OP-TEE 的 Stack Canary 为什么不能一开始就依赖完整 Crypto RNG？如何保证 Early Boot 安全？

参考分析：

因为 Early Boot 期间可能尚未完成 RNG、Crypto Framework、Secure Driver 及其他依赖初始化。

如果 Stack Protection 先于 RNG 可用，就需要区分临时保护机制与最终的高质量随机保护状态。

必须明确：

- 什么时候随机源可用。
- 什么时候重新更新 Canary。
- 哪些栈会被更新。
- 更新过程中是否有并发执行的 Thread。
- 随机源失败如何处理。

还需要认识到 Stack Canary 只是缓解特定类型内存破坏的措施，不能替代正确的内存权限和边界检查。

### Q13. 在 `thread_init_per_cpu()` 中，为什么需要重新初始化 `CSR_XSCRATCH` 和 Trap Vector？

参考分析：

因为 Boot 阶段与 Runtime 阶段的 CSR 使用约定不同。

Early Boot 可能使用 Scratch CSR 保存 Hart Index。

正常 Trap Runtime 则需要根据 Trap Origin 和约定识别上下文，并通过相应入口进行寄存器保存和异常处理。

如果没有进行正确切换，Trap Handler 可能错误地解释 Scratch CSR 的值。

这类问题通常不会在正常执行中立即表现出来，而可能在第一次异步中断或 User TA ECALL 时触发。

### Q14. 一个 SoC 中 HSM 已经完成 OP-TEE 镜像认证，为什么 OP-TEE Boot 仍然可能不安全？

参考分析：

Image Authentication 只能证明被认证内容符合可信的签名或哈希策略，不能自动保证整个执行环境安全。

还需要考虑：

- 镜像认证后是否可能被非可信 Master 修改。
- 从认证到执行是否存在 TOCTOU。
- OP-TEE 物理内存是否受到硬件保护。
- Boot Parameters 和 Device Tree 是否可信。
- DMA 是否可以访问受保护内存。
- M-mode Firmware 是否自身可信。
- Secure Boot Chain 是否正确验证 OpenSBI 与其他参与执行的固件。

安全启动的核心目标不仅是「加载了正确镜像」，还包括执行过程中维持相应的信任边界。

### Q15. 如果让你负责一个 16-hart RISC-V Server SoC 的 OP-TEE Bring-up，你如何定义可验收的 Boot Completion？

参考分析：

我建议将验收分成四级。

Level 1 — Architectural Correctness

验证每个 hart 的 Entry Mode、`satp`、`stvec`、Stack、Hart ID/Index 等基础状态正确。

Level 2 — Kernel Correctness

验证 Primary/Secondary 生命周期、MMU、Thread Pool、Initcall、SBI HSM 和远程 TLB 同步机制。

Level 3 — Functional Correctness

验证 Linux TEE Driver、RPC、TA Load/Invoke、Secure Storage 以及多 hart 并发请求。

Level 4 — Security Correctness

验证 Domain Isolation、非授权物理内存访问、DMA/Peripheral Policy、异常上下文恢复、Reset Lifecycle 和敏感状态清理。

最终交付应包括：

- 固定的 Firmware/Kernel/OP-TEE Commit。
- 完整 Build Configuration。
- 受控的 Boot Log。
- 测试记录与失败复现方法。
- 关键安全假设。
- 已知限制和尚未实现的硬件特性。
- 回归测试清单。

只有这样，才是真正的工业级 Porting 验收，而不只是通过串口看到了 OP-TEE Banner。

# 8.13 本章最终总结

Chapter 08 最重要的收获是理解：

OP-TEE Boot 本质上是一系列从弱执行环境逐步建立强执行环境的状态转换。

可以总结成：

1. Image State

   ELF 已正确链接，镜像被加载到可信的预期位置。
2. Boot Assembly State

   `_start` 执行，完成基础寄存器及 Primary Election。
3. Memory State

   BSS、Stack、Heap、Boot Memory 和 MMU 逐步建立。
4. Kernel State

   Thread Pool、Trap、Per-Hart State 和内核初始化框架具备正常执行条件。
5. Runtime State

   核心安全服务、驱动和相关通信设施完成初始化。
6. System Integration State

   完成多核同步和 Monitor/Domain 交接，进入可处理 TEE 请求的运行环境。

这里还有一条架构级原则值得记住：

> 每一个 Boot 阶段只能依赖之前已经建立并验证的执行条件；而任何新引入的全局状态、硬件权限、寄存器约定和同步协议，都必须明确自己的初始化时机及生命周期。

未来不论你研究的是 OpenSBI、OP-TEE、UEFI、Linux Early Boot，还是独立 HSM Firmware，这一原则都同样适用。

## 推荐源码阅读顺序

结合本章，建议按这个顺序逐个深入，而不是随机跳转：

1. [kern.ld.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/kern.ld.S?utm_source=chatgpt.com)：弄清镜像布局和入口。
2. [entry.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/entry.S?utm_source=chatgpt.com)：重点跟踪 `_start`、`reset_primary`、`reset_secondary` 和 `enable_mmu`。
3. [boot.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/boot.c?utm_source=chatgpt.com)：理解所有 Boot 阶段的 C 函数。
4. [core_mmu_arch.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/mm/core_mmu_arch.c?utm_source=chatgpt.com)：进一步研究页表构建和 `satp`。
5. [thread_arch.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_arch.c?utm_source=chatgpt.com)：理解 Per-Hart 初始化和正常 Trap Runtime。
6. [initcall.c](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/initcall.c?utm_source=chatgpt.com)：分析 Initcall 执行及错误处理。

Chapter 08 的真正完成标准： 你能够在 GDB 中从 `_start` 单步执行到 `boot_init_primary_runtime()`，解释其中每次寄存器、Stack、MMU 和执行上下文状态转换的原因，并且能够独立诊断 Primary/Secondary Hart 启动失败的问题。

下一章 Chapter 09 将直接在此基础上研究 RISC-V OP-TEE Trap/Exception Architecture，尤其是 `stvec`、`sscratch`、Trap Frame、S-mode/U-mode 切换与完整汇编级 Exception Entry/Return。
