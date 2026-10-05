可以。先抓住一个最重要的规律：

> **RISC-V CSR 的名字通常就是：`特权级前缀 + 功能缩写`。**

例如：

```text
s + cause  = scause
m + epc    = mepc
s + tvec   = stvec
m + ideleg = mideleg
h + gatp   = hgatp
vs + atp   = vsatp
```

一旦这样拆开，你会发现它其实比 Arm 的命名还规则。

另外先纠正一个小点：你前面看到的是 **TOR**，不是 TOP。TOR = **Top Of Range**。

---

## 1. 先记住 RISC-V 的“前缀语法”

| 前缀 | 全称 | 含义 | 大致 Arm 类比 |
|---|---|---|---|
| `m` | Machine | M-mode | EL3 仅可粗略类比 |
| `s` | Supervisor | S-mode | EL1 |
| `u` | User | U-mode | EL0 |
| `h` | Hypervisor | Hypervisor extension | EL2 相关功能 |
| `vs` | Virtual Supervisor | Guest Supervisor | Guest EL1 |
| `vu` | Virtual User | Guest User | Guest EL0 |

因此以后看到：

```text
mcause
mstatus
mtvec
mepc

scause
sstatus
stvec
sepc
```

你不用逐个背。

其实就是：

```text
mxxx = Machine xxx
sxxx = Supervisor xxx
```

---

# 2. Trap / Exception 相关寄存器

这一组是最重要的。

| CSR | 完整名称 | 拆解 | 作用 | Arm 类比 |
|---|---|---|---|---|
| `mstatus` | Machine Status Register | m + status | M-mode 状态 | SPSR/SCTLR 部分概念 |
| `sstatus` | Supervisor Status Register | s + status | S-mode 状态 | SPSR_EL1 部分概念 |
| `mtvec` | Machine Trap Vector Base Address Register | m + trap vector | M-mode trap handler 地址 | VBAR_EL3 |
| `stvec` | Supervisor Trap Vector Base Address Register | s + trap vector | S-mode trap handler 地址 | VBAR_EL1 |
| `mepc` | Machine Exception Program Counter | m + EPC | trap 返回 PC | ELR_EL3 |
| `sepc` | Supervisor Exception Program Counter | s + EPC | trap 返回 PC | ELR_EL1 |
| `mcause` | Machine Cause Register | m + cause | trap 原因 | ESR_ELx 的 EC/cause 部分 |
| `scause` | Supervisor Cause Register | s + cause | trap 原因 | ESR_EL1 |
| `mtval` | Machine Trap Value | m + trap value | trap 附加信息 | FAR/ESR 附加信息 |
| `stval` | Supervisor Trap Value | s + trap value | trap 附加信息 | FAR_EL1 等 |
| `mscratch` | Machine Scratch Register | m + scratch | trap handler 临时保存 | 无直接对应 |
| `sscratch` | Supervisor Scratch Register | s + scratch | S-mode trap 临时数据 | 无直接对应 |

这里最值得记的是：

```text
tvec
= Trap Vector

epc
= Exception Program Counter

cause
= Cause

tval
= Trap Value
```

所以：

```text
stvec   = Supervisor Trap Vector
sepc    = Supervisor Exception PC
scause  = Supervisor Cause
stval   = Supervisor Trap Value
```

非常直接。

---

# 3. `epc` 为什么叫 Exception Program Counter？

你可以直接把：

```text
sepc
```

理解成 RISC-V 世界里的：

```text
ELR_EL1
```

发生 exception：

```text
PC = 0x80001234

       exception
           ↓

sepc = 0x80001234
scause = exception原因
stval = additional information
PC = stvec
```

处理完：

```asm
sret
```

然后：

```text
PC ← sepc
```

恢复执行。

所以：

> **EPC = Exception Program Counter，本质就是“异常返回地址”。**

---

# 4. `cause / tval` 怎么理解

这两个经常一起出现。

例如 page fault：

```text
scause
  =
Load Page Fault
```

同时：

```text
stval
  =
0x12345678
```

这里 `0x12345678` 可能就是发生 fault 的地址。

所以：

```text
scause
   ↓
发生了什么？

stval
   ↓
具体和什么值/地址有关？
```

和 Arm 思维很像：

```text
ESR_EL1
  ↓
发生什么异常？

FAR_EL1
  ↓
哪个地址导致异常？
```

只不过 `tval` 的用途比 FAR 更一般，所以叫：

> **Trap Value**

而不是 Fault Address。

---

# 5. Interrupt 相关 CSR

这组也非常规律：

| CSR | 全称 | 含义 |
|---|---|---|
| `mie` | Machine Interrupt Enable | M-mode interrupt enable bits |
| `sie` | Supervisor Interrupt Enable | S-mode interrupt enable bits |
| `mip` | Machine Interrupt Pending | M-mode pending interrupts |
| `sip` | Supervisor Interrupt Pending | S-mode pending interrupts |

这里：

```text
ie = Interrupt Enable
ip = Interrupt Pending
```

因此看到：

```text
mie
```

立即拆成：

```text
m + ie
Machine Interrupt Enable
```

看到：

```text
sip
```

就是：

```text
s + ip
Supervisor Interrupt Pending
```

---

# 6. Delegation：RISC-V 特别重要的一组

| CSR | 全称 |
|---|---|
| `medeleg` | Machine Exception Delegation Register |
| `mideleg` | Machine Interrupt Delegation Register |
| `hedeleg` | Hypervisor Exception Delegation Register |
| `hideleg` | Hypervisor Interrupt Delegation Register |

这里：

```text
deleg
=
Delegation
=
委托 / 下放
```

因此：

```text
m e deleg
│ │   │
│ │   └── delegation
│ └────── exception
└──────── machine
```

即：

> **Machine Exception Delegation**

而：

```text
m i deleg
```

就是：

> **Machine Interrupt Delegation**

这个命名规律非常漂亮。

例如：

```text
medeleg
```

决定：

```text
某些 exception

M-mode
   |
   | delegate
   v
S-mode
```

---

# 7. MMU 相关：`satp` 是最重要的

### `satp`

全称：

> **Supervisor Address Translation and Protection**

拆开：

```text
s + atp

s   = Supervisor
A   = Address
T   = Translation
P   = Protection
```

所以：

```text
satp
```

可以直接记成：

> **Supervisor 地址转换和保护寄存器**

它大致承担：

```text
satp
├── MODE
├── ASID
└── PPN
```

例如：

```text
satp.MODE = Sv39
satp.PPN  = root page table
```

因此从 Arm 角度，可以粗略理解：

```text
satp
≈
TTBR_EL1 + TCR_EL1 的部分功能
```

不是严格 1:1。

---

# 8. MMU 中几个常见缩写

| 缩写 | 全称 | 含义 |
|---|---|---|
| `VA` | Virtual Address | 虚拟地址 |
| `PA` | Physical Address | 物理地址 |
| `VPN` | Virtual Page Number | 虚拟页号 |
| `PPN` | Physical Page Number | 物理页号 |
| `PTE` | Page Table Entry | 页表项 |
| `ASID` | Address Space Identifier | 地址空间 ID |
| `TLB` | Translation Lookaside Buffer | 地址翻译缓存 |

例如 Sv39：

```text
VA
│
├── VPN[2]
├── VPN[1]
├── VPN[0]
└── page offset
```

---

# 9. `Sv39 / Sv48`

可以这样记：

```text
S  = Supervisor
v  = virtual-memory
39 = 39-bit virtual address
```

所以：

> **Sv39 = Supervisor 39-bit Virtual Memory Scheme**

通常官方描述为：

> page-based 39-bit virtual-memory system

同理：

```text
Sv48
```

就是：

> 48-bit virtual address translation scheme

因此：

```text
Sv39 → 3-level page table
Sv48 → 4-level page table
```

---

# 10. PTE 中的 V/R/W/X/U/G/A/D

这个你以后看 MMU spec 会天天碰到：

| Bit | 全称 | 含义 |
|---|---|---|
| `V` | Valid | PTE 是否有效 |
| `R` | Read | 可读 |
| `W` | Write | 可写 |
| `X` | Execute | 可执行 |
| `U` | User | U-mode 是否可以访问 |
| `G` | Global | Global mapping |
| `A` | Accessed | 页面是否被访问 |
| `D` | Dirty | 页面是否被写 |

你熟悉 Arm page table 后，这套基本没有学习成本。

---

# 11. PMP：你刚才特别问到的

`PMP` 全称：

> **Physical Memory Protection**

即：

```text
P = Physical
M = Memory
P = Protection
```

这是一个非常重要的名字：

> **物理内存保护机制。**

它不是：

```text
Page Memory Protection
```

而明确是：

```text
Physical Memory Protection
```

这也揭示了它与 MMU 的根本区别：

```text
MMU
VA → PA

PMP
PA → 是否允许访问
```

非常适合用你熟悉的 MPU 思维理解。

---

# 12. PMP CSR

| CSR | 完整名称 |
|---|---|
| `pmpcfg` | Physical Memory Protection Configuration |
| `pmpaddr` | Physical Memory Protection Address |

例如：

```text
pmpcfg0
pmpcfg1
...

pmpaddr0
pmpaddr1
...
```

一个 PMP entry 基本由：

```text
pmpcfgN
+
pmpaddrN
```

组成。

---

# 13. PMP 的 TOR / NA4 / NAPOT

这是第一次看 RISC-V 最难记的一组，其实知道全称以后特别简单。

### TOR

```text
TOR
=
Top Of Range
```

表示：

> 当前 `pmpaddr` 表示这个区域的上边界。

概念上：

```text
previous pmpaddr
      |
      v
+----------------+
| protected area |
+----------------+
      ^
      |
current pmpaddr
Top Of Range
```

---

### NA4

```text
NA4
=
Naturally Aligned 4-byte region
```

也就是：

> 自然对齐的 4-byte 区域。

---

### NAPOT

这个特别经典：

```text
NAPOT
=
Naturally Aligned Power-Of-Two
```

即：

> **自然对齐、2 的幂大小区域。**

例如：

```text
8 B
16 B
32 B
4 KB
...
```

并要求相应 alignment。

你可以把 NAPOT 拆成：

```text
NA + POT

NA  = Naturally Aligned
POT = Power Of Two
```

这样以后永远不会忘。

---

# 14. PMP 配置字段

常见：

```text
R = Read
W = Write
X = Execute

A = Address-matching mode
L = Lock
```

其中 `A` 决定：

```text
OFF
TOR
NA4
NAPOT
```

采用哪一种地址匹配方式。

---

# 15. 虚拟化：VS / VU / HS

这组如果知道全称，也会好理解很多。

### VS

```text
VS
=
Virtual Supervisor
```

也就是：

> 虚拟机内部的 Supervisor mode

大致：

```text
Guest kernel
≈
VS-mode
```

---

### VU

```text
VU
=
Virtual User
```

即：

```text
Guest application
≈
VU-mode
```

---

### HS

一般称：

> **Hypervisor-extended Supervisor mode**

可以理解成：

```text
HS
=
S-mode
+
Hypervisor Extension
```

也就是：

```text
Hypervisor

运行于 HS-mode
```

所以对应关系：

```text
Arm                     RISC-V

Host EL2                HS
Guest EL1               VS
Guest EL0               VU
```

---

# 16. `vsatp`

这个现在应该很好猜了：

```text
vs + atp
```

即：

> **Virtual Supervisor Address Translation and Protection**

作用于：

```text
Guest VA
   |
   | vsatp
   v
Guest PA
```

也就是 guest stage 的地址翻译。

---

# 17. `hgatp`

这个名字也可以直接拆：

```text
h + gatp
```

其中：

```text
H    = Hypervisor
G    = Guest
ATP  = Address Translation and Protection
```

即：

> **Hypervisor Guest Address Translation and Protection**

负责：

```text
Guest Physical Address
        |
        | hgatp
        v
Host Physical Address
```

因此非常好记：

```text
vsatp
Guest VA → Guest PA

hgatp
Guest PA → Host PA
```

这两个就构成虚拟化 two-stage translation。

---

# 18. Trap 返回指令

### `MRET`

可以记：

```text
M RET
=
Machine Return
```

准确来说是：

> 从 M-mode trap handler 返回。

类似：

```text
Arm ERET
```

但针对 Machine mode。

---

### `SRET`

```text
S RET
=
Supervisor Return
```

即：

> 从 Supervisor trap handler 返回。

所以：

```text
MRET
SRET
```

其实很好记。

---

# 19. ECALL

```text
ECALL
=
Environment Call
```

这个名字很值得记。

它不是：

```text
System Call
```

而是更加通用的：

> 请求 execution environment 提供服务。

例如：

```text
U-mode
  |
  | ECALL
  v
S-mode kernel
```

可以作为 syscall。

又例如：

```text
S-mode Linux
  |
  | ECALL
  v
M-mode OpenSBI
```

可以作为 SBI call。

所以它才不叫 syscall，而叫：

> **Environment Call**

这个命名实际上非常合理。

---

# 20. EBREAK

```text
EBREAK
=
Environment Breakpoint
```

主要用于：

```text
debugger
breakpoint
```

类似软件断点。

---

# 21. CSR

这个以后会高频出现：

```text
CSR
=
Control and Status Register
```

你可以直接类比 Arm system register：

```text
SCTLR_EL1
TCR_EL1
TTBR_EL1
ESR_EL1
```

在 RISC-V 世界大量都属于：

```text
CSR
```

例如：

```text
sstatus
stvec
scause
sepc
satp
```

都是 CSR。

---

# 22. CSR 指令

RISC-V 专门提供 CSR instructions。

### CSRRW

```text
CSR Read / Write
```

即：

```text
Control and Status Register
Read / Write
```

例如：

```asm
csrrw a0, sstatus, a1
```

概念：

```text
a0 ← old sstatus
sstatus ← a1
```

---

### CSRRS

```text
CSR Read and Set
```

一般用于：

```text
读取 CSR
+
set 某些 bits
```

---

### CSRRC

```text
CSR Read and Clear
```

就是：

```text
读取
+
clear bits
```

所以：

```text
CSRRW = Read/Write
CSRRS = Read/Set
CSRRC = Read/Clear
```

规律特别强。

---

# 23. FENCE

```text
FENCE
=
Fence
```

它本身不是复杂缩写，含义就是：

> memory ordering fence

相当于你熟悉的 memory barrier 概念。

粗略：

```text
Arm DMB
   ↕
RISC-V FENCE
```

当然具体 memory model 不完全相同。

---

# 24. `SFENCE.VMA`

这是 RISC-V MMU 中非常关键的一条。

这个名字可以这样记：

```text
S FENCE . VMA

S     = Supervisor
FENCE = synchronization fence
VMA   = Virtual Memory Address
```

通常直接记作：

> **Supervisor memory-management fence**

用途是：

```text
修改 page table
      ↓
SFENCE.VMA
      ↓
保证后续地址翻译看到新 page table
```

所以从你 Arm 的角度：

```text
TLBI + barrier 相关操作
       ↕
SFENCE.VMA
```

虽然实现语义不能完全简单等同。

---

# 25. `aq / rl`

原子指令中经常看到：

```text
aq
rl
```

它们就是：

```text
aq = Acquire
rl = Release
```

例如：

```text
AMO ... .aq
AMO ... .rl
AMO ... .aqrl
```

表示对应的：

```text
Acquire
Release
Acquire + Release
```

memory ordering。

你之前已经研究过 Arm：

```text
LDAR
STLR
```

所以这里概念可以直接迁移。

---

# 26. Base ISA 中你已经看到的指令

知道英文以后基本就不用背了：

| 指令 | 全称 |
|---|---|
| `LUI` | Load Upper Immediate |
| `AUIPC` | Add Upper Immediate to PC |
| `JAL` | Jump And Link |
| `JALR` | Jump And Link Register |
| `BEQ` | Branch if Equal |
| `BNE` | Branch if Not Equal |
| `BLT` | Branch if Less Than |
| `BGE` | Branch if Greater than or Equal |
| `LW` | Load Word |
| `LD` | Load Doubleword |
| `SW` | Store Word |
| `SD` | Store Doubleword |
| `SLL` | Shift Left Logical |
| `SRL` | Shift Right Logical |
| `SRA` | Shift Right Arithmetic |
| `SLT` | Set if Less Than |

例如：

```text
JAL
=
Jump And Link
```

和 Arm：

```text
BL
=
Branch with Link
```

其实思路一模一样。

---

# 27. ISA Extension 名字

这些也建议知道。

| Extension | 名称 |
|---|---|
| `I` | Integer base ISA |
| `M` | Integer Multiplication and Division |
| `A` | Atomic Instructions |
| `F` | Single-Precision Floating-Point |
| `D` | Double-Precision Floating-Point |
| `C` | Compressed Instructions |
| `V` | Vector Extension |
| `B` | Bit-Manipulation Extensions |

所以：

```text
RV64IMAFDC
```

可以读作：

```text
RISC-V
64-bit

Integer
Multiply/Divide
Atomic
Float
Double
Compressed
```

不是一个神秘的“版本号”。

---

# 28. `Zxxx` 是什么意思？

这个也容易困惑。

RISC-V extension naming 中：

```text
Z...
```

通常表示：

> 标准 extension，但不是单字母 major extension。

例如：

```text
Zicsr
```

可以拆成：

```text
Zi + csr
```

核心就是：

> CSR instructions extension

而：

```text
Zifencei
```

就是和：

```text
FENCE.I
```

相关的 extension。

所以看到：

```text
Zba
Zbb
Zbs
```

不要试图按版本号理解。

它们是：

```text
Z + extension name
```

---

# 29. Interrupt Controller 相关缩写

这组也建议一起记。

### PLIC

```text
PLIC
=
Platform-Level Interrupt Controller
```

也就是：

> Platform-level 外部中断控制器。

你可以先粗略类比：

```text
Arm GIC
```

但 PLIC 能力要简单很多。

---

### AIA

```text
AIA
=
Advanced Interrupt Architecture
```

是后来更加完整的 RISC-V interrupt architecture。

---

### APLIC

```text
APLIC
=
Advanced Platform-Level Interrupt Controller
```

拆：

```text
A  = Advanced
PLIC = Platform-Level Interrupt Controller
```

---

### IMSIC

```text
IMSIC
=
Incoming MSI Controller
```

而：

```text
MSI
=
Message-Signaled Interrupt
```

IMSIC 可以理解成：

> 接收 MSI 的 per-hart interrupt controller。

---

# 30. SBI

这个非常重要：

```text
SBI
=
Supervisor Binary Interface
```

注意是：

```text
Binary Interface
```

而不是：

```text
Boot Interface
```

它定义：

```text
S-mode software
       |
       | SBI
       v
execution environment
```

例如：

```text
Linux S-mode
      |
      | ECALL
      v
OpenSBI M-mode
```

可以把它类比为：

```text
PSCI + 一部分 firmware interface
```

但 SBI 的范围更广。

---

# 31. OpenSBI

`OpenSBI`：

> Open-source implementation of the RISC-V SBI specification

也就是说：

```text
SBI
=
标准 / interface

OpenSBI
=
这个标准的一种具体 firmware 实现
```

就像：

```text
PSCI
≠
TF-A
```

类似：

```text
SBI
≠
OpenSBI
```

这个区别非常重要。

---

# 32. IOMMU

这个与 Arm 一样：

```text
IOMMU
=
Input/Output Memory Management Unit
```

负责：

```text
Device address
      ↓
IOMMU
      ↓
Physical memory
```

与 Arm SMMU 属于类似领域：

```text
Arm SMMU
≈
RISC-V IOMMU
```

---

# 33. Hart

这个 RISC-V 术语以后你会非常高频看到：

```text
hart
=
hardware thread
```

所以：

```text
hart 0
hart 1
hart 2
hart 3
```

不一定严格等同于：

```text
CPU core
```

更准确是：

> **一个独立的 hardware execution context。**

例如一个 core 如果支持 SMT，理论上可以包含多个 hart。

---

# 34. 最后帮你总结成一个“命名词根表”

以后你看 spec，最值得背的其实不是整个寄存器名，而是下面这些词根：

| 词根 | 全称 / 含义 |
|---|---|
| `m` | Machine |
| `s` | Supervisor |
| `u` | User |
| `h` | Hypervisor |
| `vs` | Virtual Supervisor |
| `status` | Status |
| `tvec` | Trap Vector |
| `epc` | Exception Program Counter |
| `cause` | Trap Cause |
| `tval` | Trap Value |
| `ie` | Interrupt Enable |
| `ip` | Interrupt Pending |
| `deleg` | Delegation |
| `atp` | Address Translation and Protection |
| `cfg` | Configuration |
| `addr` | Address |
| `PMP` | Physical Memory Protection |
| `TOR` | Top Of Range |
| `NA4` | Naturally Aligned 4-byte |
| `NAPOT` | Naturally Aligned Power-Of-Two |
| `CSR` | Control and Status Register |
| `EPC` | Exception Program Counter |
| `SBI` | Supervisor Binary Interface |
| `PLIC` | Platform-Level Interrupt Controller |

因此以后你碰到：

```text
vscause
```

甚至不用查文档：

```text
vs + cause
=
Virtual Supervisor Cause
```

碰到：

```text
vsepc
```

就是：

```text
Virtual Supervisor
Exception Program Counter
```

碰到：

```text
vstvec
```

就是：

```text
Virtual Supervisor
Trap Vector
```

这才是最值得建立的 RISC-V 记忆方式。

---

## 用 Arm 的方式记 RISC-V

你现在其实可以建立这样一组非常牢固的映射：

```text
Arm                       RISC-V

ELR_EL1                   sepc
Exception Link Register   Supervisor Exception PC

ESR_EL1                   scause
Exception Syndrome        Supervisor Cause

FAR_EL1                   stval
Fault Address             Supervisor Trap Value

VBAR_EL1                  stvec
Vector Base Address       Supervisor Trap Vector

TTBR/TCR                  satp
Translation Table         Address Translation
                          and Protection

ERET                      SRET

SVC                       ECALL

DMB                       FENCE

TLBI                      SFENCE.VMA
```

**你真正需要记住的其实只有十几个英文词根。** 一旦这些词根建立起来，RISC-V 的 CSR 命名会从“完全没有规律”变成非常容易推导。