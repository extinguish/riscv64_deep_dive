下面这份我建议你把它当成 **RISC-V Privileged Architecture 的正式第 02 章**。这一章先不深入某一个具体 CSR 的所有 bit，而是先把 **CSR 整体架构、命名规律、访问方式、权限模型、寄存器族之间的关系** 建立起来。等这个 mental model 建好以后，后面的 Trap、Interrupt、PMP、Sv39 基本都会变成“在 CSR 框架上填内容”。

# RISC-V Architecture 系统学习 02：CSR Architecture

## 0. 这一章真正要解决什么问题

先给结论：

> **CSR（Control and Status Register）就是 RISC-V 用来承载“处理器架构控制状态”的专用寄存器空间。**

如果用你熟悉的 Arm 来理解：

```text
Arm:
SCTLR_EL1
TTBR0_EL1
VBAR_EL1
ESR_EL1
ELR_EL1
DAIF
SPSR_EL1
...

               ↓

RISC-V:
mstatus / sstatus
satp
mtvec / stvec
mcause / scause
mepc / sepc
mie / sie
...
```

但 RISC-V 相比 Arm 做了一件非常重要的事情：

> **它把这些 System Register 统一成了一个规则非常明确的 CSR address space，再通过统一的 CSR 指令访问。**

因此 CSR Architecture 并不只是“有哪些 CSR”。

它实际上包含五件事：

```text
CSR Architecture
      |
      +-- CSR address space
      |
      +-- privilege/access control
      |
      +-- CSR instruction
      |
      +-- CSR field semantics
      |
      +-- CSR families / naming model
```

官方当前 Privileged ISA 仍然把 CSR 作为整个 privileged architecture 的基础章节；标准 CSR 地址使用 12-bit 编码，也就是最多 4096 个 CSR，并且 privileged architecture 要求实现 Zicsr。

---

# 1. CSR 到底是什么？

假设一个 RISC-V hart 有：

```text
x0 ~ x31
pc
```

这些属于普通 architectural state。

但是 OS / firmware 还必须知道：

```text
当前 privilege 状态是什么？

中断开了吗？

发生 exception 时 PC 在哪里？

exception 原因是什么？

trap handler 在哪里？

MMU page table 在哪里？

哪些 interrupt enable？

哪些 interrupt pending？

当前 ISA 支持哪些 extension？
```

这些信息显然不能全部塞进：

```text
x0 ~ x31
```

于是存在另一组 architectural state：

```text
CSR
```

例如：

```text
mstatus
mtvec
mepc
mcause
mtval
mie
mip
satp
```

所以从 CPU architecture 的角度，可以画成：

```text
                  RISC-V Hart

        ┌─────────────────────────┐
        │ Integer Register File   │
        │                         │
        │ x0 ... x31              │
        └─────────────────────────┘

        ┌─────────────────────────┐
        │ PC                      │
        └─────────────────────────┘

        ┌─────────────────────────┐
        │ CSR State               │
        │                         │
        │ mstatus                 │
        │ mtvec                   │
        │ mepc                    │
        │ mcause                  │
        │ mie                     │
        │ mip                     │
        │ satp                    │
        │ ...                     │
        └─────────────────────────┘
```

注意一个非常重要的点：

> **CSR 是 per-hart architectural state。**

也就是说：

```text
Hart 0
    mstatus
    mepc
    mcause
    ...

Hart 1
    mstatus
    mepc
    mcause
    ...
```

并不是多个 hart 共用一份。

官方 Zicsr 规范明确把 CSR 描述为与每个 hart 相关联的独立 CSR address space。

这和 Arm 的：

```text
SCTLR_EL1
VBAR_EL1
ESR_EL1
```

本质上是同一种 CPU-local architectural state。

> 在继续介绍之前，首先讲解一下`Zicsr`规范是什么:
`Zicsr` 的正式名称是：

> **Zicsr — Control and Status Register (CSR) Instructions**

也就是：

> **控制与状态寄存器指令扩展**。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/zicsr.html?utm_source=chatgpt.com)

这里可以这样拆着记：

```text
Zicsr
│
├── Z    = standard extension，标准扩展的命名前缀
│
└── icsr = CSR-related instructions
           Control and Status Register Instructions
```

其中最核心的就是我们后面将要讲述的下面这 6 条：

```text
CSRRW
CSRRS
CSRRC

CSRRWI
CSRRSI
CSRRCI
```

然后：

```asm
csrr
csrw
csrs
csrc
```

这些通常是 assembler pseudo-instruction。

一个容易混淆的点是：**`Zicsr` 不等于“CSR 本身”**。更准确地说：

```text
CSR
=
Control and Status Register 架构状态

Zicsr
=
访问这些 CSR 的指令集扩展
```

例如：

```text
mstatus
mcause
satp
```

是 CSR；

而：

```asm
csrr a0, mstatus
csrw satp, a1
```

背后的 `CSRRS / CSRRW` 才属于 **Zicsr extension**。

另外，RISC-V Privileged Architecture 明确要求实现 `Zicsr`，因为 M/S/U privilege architecture 的大量状态都必须依靠 CSR 指令访问。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/priv-csrs.html?utm_source=chatgpt.com)

你可以直接记成一句：

> **Zicsr = 提供 CSR 读、写、置位、清位能力的标准 RISC-V 扩展。**

或者我们可以这样更简单的进行理解,即`CSR`代表的是一族寄存器，`Zicsr`是访问这些`CSR`寄存器的指令集扩展 -> 然后下面是更严谨的描述:

- CSR（Control and Status Register）：指 RISC-V 中用于保存和控制处理器架构状态的一类寄存器。
- Zicsr（Control and Status Register Instructions）：提供访问 CSR 的标准指令集扩展，包括读取、写入、置位和清位等操作。

可以用下面的关系来理解：

```
RISC-V Architecture
       |
       +-- CSR：架构状态寄存器
       |     |
       |     +-- mstatus：Machine 状态
       |     +-- mtvec：Machine Trap 入口
       |     +-- mepc：Machine Trap 返回地址
       |     +-- mcause：Trap 原因
       |     +-- satp：Supervisor 地址转换控制
       |
       +-- Zicsr：访问 CSR 的指令集扩展
             |
             +-- CSRRW：读取并写入
             +-- CSRRS：读取并置位
             +-- CSRRC：读取并清位
             +-- CSRRWI / CSRRSI / CSRRCI：对应的立即数版本
```

结合你熟悉的 Arm 架构，可以这样类比：

| 概念      | Arm                                       | RISC-V                    |
| ------- | ----------------------------------------- | ------------------------- |
| 架构状态寄存器 | System Registers，例如 `SCTLR_EL1`、`ESR_EL1` | CSR，例如 `mstatus`、`mcause` |
| 访问指令    | `MRS`、`MSR`                               | `CSRRW`、`CSRRS`、`CSRRC` 等 |
| 组织方式    | System Register 编码与访问规则                   | CSR 地址空间与 Zicsr 指令扩展      |

还有一个重要区别：CSR 不等于 Zicsr。

CSR 是寄存器及其架构语义；Zicsr 是访问这些寄存器的指令扩展。至于某个 CSR 是否存在、有哪些字段、具有什么访问权限，则由对应的 RISC-V 基础架构规范或扩展规范决定。

一句话总结：

> CSR 定义“处理器有哪些架构状态寄存器”，Zicsr 定义“软件通过哪些指令访问这些寄存器”。

好的，然后我们继续开始我们后续的CSR架构讲解.

---

# 2. CSR 和 MMIO Register 完全不是一回事

这是首先必须分清楚的。

例如一个 UART：

```text
UART_BASE + 0x00
UART_BASE + 0x04
UART_BASE + 0x08
```

这些属于：

```text
memory-mapped register
```

CPU 使用：

```asm
ld
sd
lw
sw
```

访问。

但是：

```text
mstatus
mepc
mcause
```

不在 physical address space 中。

你不能：

```asm
ld t0, 0x300
```

来读 `mstatus`。

CSR 有自己独立的：

```text
12-bit CSR address space
```

例如：

```text
mstatus = 0x300
mtvec   = 0x305
mepc    = 0x341
mcause  = 0x342
```

这里：

```text
0x300
```

不是 physical address。

而是：

```text
CSR number
```

CPU instruction decoder 会识别：

```asm
csrr a0, mstatus
```

然后访问内部 CSR state。

所以：

```text
                 CPU instruction
                       |
          +------------+-----------+
          |                        |
      load/store                CSR instruction
          |                        |
          v                        v
 Physical Address Space        CSR Space
          |                        |
     RAM / MMIO                mstatus
                               mepc
                               satp
                               ...
```

这是 CSR architecture 最基础的概念。

---

# 3. 为什么 RISC-V 要设计一个统一 CSR 空间？

Arm 的设计更像：

```text
MRS x0, SCTLR_EL1
MRS x0, ESR_EL1
MRS x0, TTBR0_EL1
MSR VBAR_EL1, x0
```

这些都是 System Register，但是每一个 system register 有自己的 architectural encoding。

RISC-V 把它统一成：

```text
CSR address
+
统一 CSR instruction
```

例如：

```asm
csrr t0, mstatus
csrr t1, mepc
csrr t2, mcause
```

底层实际上使用：

```text
CSRRS
CSRRW
...
```

整个 CSR number 直接编码在 instruction 中。

CSR instruction 使用一个 **12-bit csr field**，位于 instruction bits `[31:20]`，因此天然形成：

```text
2^12 = 4096
```

个 CSR 编码空间。

概念上：

```text
31                         20
+---------------------------+
|        CSR address        |
+---------------------------+
```

所以 CPU decode：

```text
instruction
   |
   +-- opcode = SYSTEM
   |
   +-- CSR number = 0x300
                    |
                    v
                 mstatus
```

这个设计非常 RISC-V：

> **用少量规则，统一大量 architectural state。**

---

# 4. CSR 地址本身就编码了一部分权限信息

这是 RISC-V CSR 设计里一个非常漂亮的地方。

CSR address：

```text
csr[11:0]
```

其中高 4 bit：

```text
csr[11:8]
```

具有结构化含义。

大致可以先这样理解：

```text
csr[11:10]
     |
     +-- CSR 是否 read-only

csr[9:8]
     |
     +-- 最低访问 privilege
```

官方规范规定：

```text
csr[11:10]

00
01
10       → read/write

11       → read-only
```

而：

```text
csr[9:8]
```

则用于区分 CSR 所属的最低 privilege domain；其中 `10` 还用于 hypervisor CSR 空间。

因此你看到典型地址：

```text
0x100
0x300
0xC00
0xFxx
```

其实不是随便分配的。

例如：

```text
mstatus = 0x300
```

binary 高位近似：

```text
0x300
=
0011 xxxx xxxx

  ^^
  ||
  |+---- privilege encoding
  +----- access encoding
```

于是硬件在执行 CSR instruction 时，不需要查一个巨大权限表才能做最基础的检查。

可以直接从 CSR number 的高位得到一部分信息。

这是一个非常典型的 RISC-V 设计思想：

> **让 instruction encoding 本身携带 architecture metadata。**

---

# 5. CSR privilege model

现在把 CSR 和上一章 M/S/U privilege 联系起来。

假设：

```text
         M-mode
           |
           v
         S-mode
           |
           v
         U-mode
```

CSR 也存在相应的分层。

典型：

```text
Machine CSRs

mstatus
mtvec
mepc
mcause
mtval
mie
mip
medeleg
mideleg
...
```

Supervisor：

```text
sstatus
stvec
sepc
scause
stval
sie
sip
satp
...
```

Hypervisor：

```text
hstatus
hedeleg
hideleg
hie
hip
hgatp
...
```

Virtual Supervisor：

```text
vsstatus
vstvec
vsepc
vscause
vstval
vsie
vsip
vsatp
...
```

从命名你实际上已经能看懂整个 hierarchy：

```text
mxxx  → Machine
sxxx  → Supervisor
hxxx  → Hypervisor
vsxxx → Virtual Supervisor
```

---

# 6. 高 privilege 可以访问低 privilege CSR

RISC-V 的基本规则是：

> 某 privilege level 对应的 CSR，也可以由更高 privilege level 访问。

例如：

```text
M-mode
   |
   +--> mstatus
   |
   +--> sstatus
   |
   +--> sepc
   |
   +--> satp
```

而：

```text
S-mode
```

不能直接访问：

```text
mstatus
mepc
mcause
```

因此：

```text
              mstatus
                 ▲
                 │
                M-mode
                 ▲
                 │
                S-mode    X
                 ▲
                 │
                U-mode    X
```

如果 privilege 不满足：

```asm
csrr a0, mstatus
```

例如在 U-mode 执行，就会触发 exception，而不是简单返回 0。

通常是：

```text
illegal instruction exception
```

在虚拟化相关场景，还可能出现 virtual-instruction exception。官方规范明确规定，访问不存在的 CSR、权限不足的 CSR 或写只读 CSR，都可能产生相应 trap。

---

# 7. CSR 指令：整个架构实际上只有三种核心操作

这一部分非常重要。

真正的 CSR instruction 核心只有三类：

```text
CSRRW
CSRRS
CSRRC
```

以及 immediate 版本：

```text
CSRRWI
CSRRSI
CSRRCI
```

不要被名字吓到。

把它们展开：

```text
RW = Read + Write

RS = Read + Set bits

RC = Read + Clear bits
```

所以：

```text
CSRRW = Atomic Read/Write
CSRRS = Atomic Read and Set
CSRRC = Atomic Read and Clear
```

官方定义它们为对单个 CSR 的 atomic read-modify-write 操作。

---

# 8. CSRRW：读取旧值，同时写入新值

例如：

```asm
csrrw a0, mstatus, a1
```

逻辑可以理解成：

```c
old = mstatus;
mstatus = a1;
a0 = old;
```

所以：

```text
Before:

mstatus = 0x100
a1      = 0x200

        csrrw a0, mstatus, a1

After:

a0      = 0x100
mstatus = 0x200
```

最关键的是：

> read + write 是一条 architectural operation。

不是：

```asm
read
...
write
```

两条独立操作。

---

# 9. CSRRS：read + set bits

例如：

```asm
csrrs a0, mstatus, a1
```

逻辑：

```c
old = mstatus;

mstatus =
    mstatus | a1;

a0 = old;
```

假设：

```text
mstatus = 0010
a1      = 0101
```

结果：

```text
           0010
OR         0101
----------------
           0111
```

所以：

```text
mstatus = 0111
a0      = 0010
```

这特别适合：

> **设置某些 CSR bit，而不破坏其他 bit。**

例如：

```asm
li    t0, MSTATUS_MIE
csrrs zero, mstatus, t0
```

概念上就是：

```c
mstatus |= MSTATUS_MIE;
```

---

# 10. CSRRC：read + clear bits

相反：

```asm
csrrc a0, mstatus, a1
```

等价于：

```c
old = mstatus;

mstatus =
    mstatus & ~a1;

a0 = old;
```

典型场景：

```text
关闭某个 interrupt enable bit
```

例如概念上：

```asm
li    t0, MSTATUS_MIE
csrrc zero, mstatus, t0
```

相当于：

```c
mstatus &= ~MSTATUS_MIE;
```

---

# 11. 为什么这里需要 atomic CSR RMW？

假设没有：

```text
CSRRS
CSRRC
```

你可能需要：

```asm
csrr t0, mstatus

or   t0, t0, mask

csrw mstatus, t0
```

也就是：

```text
read
  |
modify
  |
write
```

但是架构状态可能在不同操作之间发生变化。

因此 CSR instruction 提供：

```text
read-modify-write
```

作为一个 architectural operation。

不过一定注意：

> **“CSR instruction 是 atomic RMW”不代表它是 memory atomic，也不代表它是 memory barrier。**

它只保证：

```text
针对这个 CSR 的 read-modify-write
```

是一个完整 architectural operation。

不要把它和：

```text
AMO
LR/SC
FENCE
```

混淆。

---

# 12. 你看到的 csrr / csrw 其实是 pseudo-instruction

这是读 RISC-V assembly 时非常重要的一点。

你经常看到：

```asm
csrr a0, mstatus
```

实际上不存在一个独立硬件 opcode 叫：

```text
CSRR
```

assembler 会转换成：

```asm
csrrs a0, mstatus, x0
```

为什么？

因为：

```text
x0 = 0
```

所以：

```text
mstatus | 0
```

不会修改 mstatus。

于是：

```text
CSRRS rd, csr, x0
```

就变成：

```text
read CSR only
```

因此：

```asm
csrr a0, mstatus
```

等价于：

```asm
csrrs a0, mstatus, x0
```

同样：

```asm
csrw mstatus, a0
```

实际是：

```asm
csrrw x0, mstatus, a0
```

也就是：

```text
write CSR

不要旧值
        ↓
      rd=x0
```

官方 assembler pseudo-instruction 定义也是这样：`CSRR` 由 `CSRRS rd, csr, x0` 编码，`CSRW` 则由 `CSRRW x0, csr, rs1` 编码。

因此你真正应该记：

```text
硬件：

CSRRW
CSRRS
CSRRC


Assembler convenience：

CSRR
CSRW
CSRS
CSRC
```

---

# 13. CSR immediate 指令

还有：

```text
CSRRWI
CSRRSI
CSRRCI
```

例如：

```asm
csrsi mstatus, 8
```

不需要：

```asm
li t0, 8
csrs mstatus, t0
```

而是立即数直接编码进 instruction。

但 immediate 只有：

```text
5 bit
```

因为它复用了 instruction 中原本的 `rs1` field。

所以范围：

```text
0 ~ 31
```

比较适合操作：

```text
small bit mask
```

而不是写任意 XLEN value。

---

# 14. x0 在 CSR 指令中有特殊且非常重要的语义

这个地方值得单独强调。

例如：

```asm
csrrw x0, mstatus, a0
```

表示：

```text
write mstatus

不要 read result
```

而：

```asm
csrrs a0, mstatus, x0
```

表示：

```text
read mstatus

不要修改 CSR
```

这不仅仅是编译器优化。

在 CSR side-effect 语义中，它具有 architectural 意义。

例如 CSRRS/CSRRC：

```text
rs1 = x0
```

意味着：

> **不执行 CSR write。**

因此即使某个 CSR write 具有 side effect，也不会触发 write side effect。

官方 Zicsr 对这一点有明确区分。

所以：

```asm
csrr a0, csr
```

并不是：

```text
read + write same value
```

而是真正：

```text
read only
```

这个细节写 kernel / firmware 时非常重要。

---

# 15. CSR naming：其实比 Arm 更容易记

下面开始建立一个极重要的命名模型。

不要一个一个死记：

```text
mstatus
mtvec
mepc
mcause
mtval
mscratch
mie
mip
```

应该把它们拆开：

```text
m + status

m + tvec

m + epc

m + cause

m + tval

m + scratch

m + ie

m + ip
```

这里：

```text
m
=
Machine
```

而：

```text
status  = status
tvec    = trap vector
epc     = exception program counter
cause   = trap cause
tval    = trap value
scratch = scratch register
ie      = interrupt enable
ip      = interrupt pending
```

于是换成：

```text
s
```

自然就是：

```text
sstatus
stvec
sepc
scause
stval
sscratch
sie
sip
```

所以你应该记的是这个模板：

```text
             xstatus
                |
              xtvec
                |
              xepc
                |
             xcause
                |
              xtval
                |
            xscratch
                |
           xie / xip
```

其中：

```text
x = privilege domain
```

这是理解 CSR 最重要的规律之一。

---

# 16. 用 Arm 寄存器体系映射一下

你会立刻发现非常熟悉。

```text
Arm                         RISC-V
------------------------------------------------

SPSR_ELx / PSTATE          xstatus

VBAR_ELx                   xtvec

ELR_ELx                    xepc

ESR_ELx                    xcause

FAR_ELx                    xtval

interrupt mask/control     xie

interrupt pending state    xip
```

例如发生一个 page fault：

Arm：

```text
ESR_EL1
FAR_EL1
ELR_EL1
```

RISC-V：

```text
scause
stval
sepc
```

Mental model：

```text
                  Trap

        +----------+----------+
        |          |          |
        v          v          v

      sepc       scause      stval

       PC          why       extra info
```

这三个 CSR 以后会贯穿整个 Trap Architecture。

---

# 17. CSR family：把几百个 CSR 压缩成几类

实际学习时，不要按 CSR number 背。

应该按照功能分组。

第一组：

```text
Hart identity / capability

misa
mvendorid
marchid
mimpid
mhartid
```

第二组：

```text
Trap setup

mstatus
mtvec
medeleg
mideleg
mie
```

第三组：

```text
Trap handling

mscratch
mepc
mcause
mtval
mip
```

第四组：

```text
Memory protection

pmpcfg*
pmpaddr*
```

第五组：

```text
Supervisor VM

satp
```

第六组：

```text
Counters

cycle
time
instret

mcycle
minstret
```

第七组：

```text
Hypervisor

hstatus
hedeleg
hideleg
hie
hip
hgatp
...
```

以后每学习一个 RISC-V subsystem，只需要再展开那一组。

---

# 18. 一个非常重要的概念：CSR shadow / restricted view

这里开始出现 RISC-V 和 Arm 思路不同的地方。

例如：

```text
mstatus
```

和：

```text
sstatus
```

并不是：

```text
两个完全独立的 status register
```

最好的 mental model 是：

```text
                mstatus

  +------------------------------------+
  | M-mode fields                      |
  |                                    |
  | S-mode visible fields              |
  |   +----------------------------+   |
  |   |                            |   |
  |   +----------------------------+   |
  +------------------------------------+

                 ▲
                 |
              sstatus
           restricted view
```

官方明确规定：

> `sstatus` 是 `mstatus` 的一个子集；直接实现中，访问 `sstatus` 中某字段等价于访问 `mstatus` 中对应字段。

这非常重要。

你不能简单认为：

```text
mstatus = EL3 status
sstatus = EL1 status
```

这样会误导。

更好的理解：

```text
mstatus
=
完整 machine architectural status

sstatus
=
允许 S-mode 看到和控制的那部分 architectural status
```

这种思想本质上是：

> **hardware exposes a restricted architectural view to lower privilege software.**

---

# 19. 为什么要这么设计？

因为 RISC-V 希望 S-mode OS 不需要知道：

```text
上面到底有没有 M-mode firmware？
```

从 OS 的角度：

```text
Linux
  |
  +-- sstatus
  +-- stvec
  +-- sepc
  +-- scause
  +-- satp
```

就已经拥有一个完整 OS execution environment。

S-mode 不需要关心：

```text
M-mode firmware
OpenSBI
platform detail
```

官方 Supervisor Architecture 也特别强调，S-mode 可见的 CSR state 应该只反映 supervisor OS 应该看到的信息，不应要求它知道更高 privilege 层的存在。

这就是为什么：

```text
sstatus
sie
sip
...
```

这一整套接口很重要。

本质上是在构造：

```text
Supervisor architectural abstraction
```

---

# 20. CSR field 并不都是普通 RW bit

这是看 RISC-V spec 时一定会遇到的：

```text
WARL
WPRI
WLRL
```

如果不知道这些词，你看寄存器图会非常痛苦。

---

## 20.1 WPRI

全称：

```text
Writes Preserve Values,
Reads Ignore Values
```

典型用途：

```text
reserved field
```

假设：

```text
mstatus

63       20 19      16 15       0
+----------+----------+----------+
| reserved | feature  | other    |
+----------+----------+----------+
     ↑
    WPRI
```

软件原则：

```text
read:
    不要依赖这个值

write:
    尽量保持原值
```

为什么？

因为未来：

```text
reserved bit
```

可能被 architecture extension 使用。

所以：

```text
old = read(csr)

old &= ~FIELD_MASK
old |= new_field

write(csr, old)
```

而不要无脑：

```text
write(csr, constant)
```

官方对 WPRI 的定义就是：软件应忽略读值，并在修改同一 CSR 的其他字段时保存这些 reserved field 的值，以保持 forward compatibility。

---

# 21. WARL：这是 RISC-V 最值得理解的 CSR 概念之一

WARL：

```text
Write Any,
Read Legal
```

意思是：

> 软件可以写任意 bit pattern，但硬件只保证读回来的是它支持的合法值。

例如假设：

```text
satp.MODE
```

理论定义：

```text
Bare
Sv39
Sv48
Sv57
```

但是你的 CPU 只实现：

```text
Bare
Sv39
```

你写：

```text
Sv48
```

hardware 不一定保存：

```text
Sv48
```

而可能最终 read back：

```text
Bare
```

或者另一个合法值，具体由该字段规则决定。

因此一个 WARL 字段很常见的探测方式就是：

```text
write desired value

        ↓

read back

        ↓

did hardware accept it?
```

所以：

```c
write_csr(field, desired);

actual =
    read_csr(field);
```

这是 RISC-V 硬件 capability discovery 中很典型的思想。

官方定义 WARL 时明确要求：可以写任意值，但 read 必须得到合法编码，而且 unsupported write 不应因此产生 illegal-instruction exception。

---

# 22. WLRL

WLRL：

```text
Write/Read
Only Legal values
```

它比 WARL 更严格。

含义：

> 软件只应该写 architecture 定义的合法 encoding。

也就是说：

```text
WARL:

你乱写
↓
hardware 帮你映射到 legal value
```

而：

```text
WLRL:

software 本来就不应该乱写
```

如果写 illegal value，architecture 对后续读值的保证会弱很多，实现甚至可能选择产生 illegal-instruction exception。

所以你可以简单记：

```text
WPRI
=
reserved，别碰


WARL
=
可以试着写
然后 read back


WLRL
=
只写 architecture-defined legal value
```

---

# 23. 为什么 RISC-V 特别强调 WARL？

因为 RISC-V 非常强调：

```text
implementation flexibility
```

例如一个 CPU：

```text
只支持 Sv39
```

另一个 CPU：

```text
支持 Sv39 + Sv48
```

architecture 不希望：

```text
每种实现都重新定义一个 CSR
```

所以可以统一：

```text
satp
```

但是字段内部：

```text
WARL
```

这样软件就可以探测实现能力。

这和 RISC-V：

```text
base ISA
+
optional extension
+
implementation flexibility
```

的整体哲学完全一致。

---

# 24. CSR access 并不天然等价于 memory barrier

这是一个容易踩坑的高级问题。

假设：

```asm
sw      a0, 0(a1)

csrw    some_csr, a2
```

不能泛化地认为：

```text
CSR write
=
DMB
=
everything before completed
```

RISC-V 对 CSR ordering 有单独规则。

对当前 hart 而言，如果后续 instruction 的执行语义依赖该 CSR，CSR 状态通常按 program order 生效。

但是从：

```text
other hart
device
memory global ordering
```

角度看，CSR access 默认并不是万能 barrier；CSR 与普通 memory access 之间必要时仍需要 `FENCE`。官方 Zicsr 明确指出，在 RVWMO 下 CSR access 默认是 weakly ordered，并不是天然与所有显式 memory access 排序。

所以一定记住：

```text
CSR atomic
        ≠
memory atomic


CSR write
        ≠
FENCE
```

以后学习：

```text
PMP
satp
interrupt
IPI
```

时会再次遇到 ordering 问题。

---

# 25. CSR 和 Trap Architecture 是怎么连接起来的？

这是 CSR 真正开始“活起来”的地方。

假设：

```text
U-mode program
```

执行非法指令。

CPU 要进入 S-mode trap handler。

硬件至少需要保存：

```text
哪里出错？
为什么出错？
附加错误信息是什么？
trap handler 在哪里？
```

CSR 正好提供：

```text
sepc
scause
stval
stvec
```

于是：

```text
           Illegal instruction
                   |
                   v
             Trap hardware
                   |
         +---------+---------+
         |         |         |
         v         v         v
       sepc     scause     stval
         |
         |
         +----------------------+
                                |
                                v
                              stvec
                                |
                                v
                       Supervisor handler
```

这就是为什么 CSR architecture 是后续 Trap Architecture 的基础。

---

# 26. status CSR 又负责什么？

以：

```text
mstatus
sstatus
```

为例。

它们不是单纯：

```text
“CPU 当前状态”
```

而是大量 privilege control state 的集合。

例如：

```text
interrupt enable

previous interrupt enable

previous privilege

FP state

Vector state

memory access behavior
...
```

因此 Trap entry 时，硬件会修改：

```text
xstatus
```

Trap return：

```text
MRET
SRET
```

又会根据：

```text
xstatus
```

恢复一部分 privilege state。

所以：

```text
xstatus
+
xepc
+
xcause
+
xtvec
```

共同形成：

```text
RISC-V Trap State Machine
```

这一部分我们下一章详细拆。

---

# 27. 一个你以后经常会使用的 CSR mental model

我建议以后看到一个 CSR，固定问六个问题：

```text
1. 谁拥有它？
   M / S / VS / H / U？

2. 谁可以访问？
   privilege requirement 是什么？

3. 它是什么类型？
   RW / RO / WARL / WPRI / WLRL？

4. 谁修改它？
   software？
   hardware？
   两者都会？

5. 什么时候使用？
   trap？
   MMU？
   interrupt？
   context switch？

6. 它和哪些 CSR 构成一个 state machine？
```

例如：

```text
sepc
```

立即回答：

```text
owner:
    S-mode architecture

access:
    S及以上

purpose:
    保存 trap return PC

written by:
    trap hardware
    software 也可以修改

used by:
    SRET

family:
    stvec
    scause
    stval
    sstatus
```

做到这一步，你就不是在“背寄存器”。

而是在：

> **理解 architecture state machine。**

---

# 28. 和 Arm system register architecture 的本质对比

你已经很熟悉 Arm，所以最后把两边压缩一下。

Arm：

```text
                    EL1

SCTLR_EL1
TTBR0_EL1
VBAR_EL1
ESR_EL1
ELR_EL1
SPSR_EL1
...

使用：

MRS
MSR
```

RISC-V：

```text
                    S-mode

sstatus
satp
stvec
scause
sepc
stval
...

使用：

CSRRW
CSRRS
CSRRC
```

最大差异不是：

```text
名字不同
```

而是组织哲学不同。

Arm 更像：

```text
大量 architecture-defined system registers
+
MRS/MSR
```

RISC-V 更像：

```text
一个结构化 CSR namespace

        +

统一 read/modify/write operation

        +

privilege-encoded CSR address

        +

大量 extension 向 CSR namespace 添加 state
```

所以学习 RISC-V CSR 的正确方法不是：

> 把 300 个 CSR 名字全部背下来。

而是：

> 学会 CSR architecture，然后按 subsystem 展开 CSR family。

---

# 29. 这一章最重要的总图

最后把整章压缩成这一张图。

```text
                         RISC-V Hart
                              |
          +-------------------+-------------------+
          |                                       |
          v                                       v

 General Registers                         CSR Address Space
   x0 ~ x31                                 12-bit / 4096
                                                   |
                              +--------------------+--------------------+
                              |                    |                    |
                              v                    v                    v

                           M CSR                  S CSR                H/VS CSR

                          mstatus                sstatus              hstatus
                          mtvec                  stvec                hgatp
                          mepc                   sepc                 vsatp
                          mcause                 scause               ...
                          mie                    sie
                          mip                    sip
                          ...
                              |
                              |
                     Zicsr instructions
                              |
              +---------------+---------------+
              |               |               |
              v               v               v

            CSRRW           CSRRS           CSRRC
          read/write      read/set        read/clear

              |
              +---------- immediate variants
              |
              +---------- pseudo instructions
                          csrr
                          csrw
                          csrs
                          csrc
```

再往上连接 architecture：

```text
CSR
 |
 +-- Trap
 |     stvec
 |     sepc
 |     scause
 |     stval
 |
 +-- Interrupt
 |     sstatus
 |     sie
 |     sip
 |
 +-- MMU
 |     satp
 |
 +-- PMP
 |     pmpcfg
 |     pmpaddr
 |
 +-- SMP
 |     mhartid
 |     interrupt-related CSR
 |
 +-- Hypervisor
       hstatus
       hgatp
       vsstatus
       vsatp
```

因此可以给 CSR Architecture 一个非常简洁但准确的定义：

> **CSR 是 RISC-V 用统一地址空间和统一原子 read-modify-write 指令暴露 hart architectural control/state 的机制；Privilege、Trap、Interrupt、MMU、PMP、Virtualization 等系统级功能，本质上都是通过不同 CSR family 把相应的 architecture state 暴露给软件。**

---

# 30. 本章你真正应该掌握到什么程度

完成 CSR Architecture 这一章以后，我认为你应该能做到：

```text
看到：

csrr t0, scause

马上知道：
读取 S-mode trap cause。


看到：

csrw stvec, t0

马上知道：
配置 S-mode trap vector。


看到：

csrs sstatus, t0

马上知道：
原子 set 某些 sstatus bit。


看到：

csrrc t0, sstatus, t1

马上知道：
读取旧 sstatus，
同时 clear mask 指定的 bit。


看到：

satp

知道它属于：
Supervisor address translation CSR。


看到：

WARL

知道：
write 可以尝试任意值，
实际支持能力要 read back。


看到：

sstatus

知道：
它不是“独立于 mstatus 的 EL1 状态寄存器”，
而是 supervisor 可见 architectural state 的受限视图。
```

如果这些已经形成直觉，那么 CSR Architecture 就已经真正掌握了。

---

# 31. 下一章应该学习什么？

有了这一章以后，最自然的下一步不是继续背更多 CSR，而是：

```text
03 Trap / Exception Architecture
```

因为那一章会第一次真正让下面这些寄存器全部运动起来：

```text
xstatus
xtvec
xepc
xcause
xtval
xscratch
```

我们可以沿着一次完整事件：

```text
U-mode
   |
   | illegal instruction / ECALL / page fault
   v
hardware trap entry
   |
   +--> xepc
   +--> xcause
   +--> xtval
   +--> xstatus
   |
   v
xtvec
   |
   v
trap handler
   |
   v
SRET / MRET
```

逐 clock/architectural state 地拆解。

这样你会真正理解：

> **CSR 并不是一堆配置寄存器，而是 RISC-V privilege state machine 的载体。**

我建议下一步就直接进入 **03 Trap / Exception Architecture**。那一章我会重点讲清楚 `xtvec → xepc → xcause → xtval → xstatus → xRET` 这一整套状态机，并和你熟悉的 **Arm `VBAR_EL1 / ESR_EL1 / ELR_EL1 / SPSR_EL1 / ERET`** 做逐项映射。

