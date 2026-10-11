# RISC-V Architecture 系统学习 04：Interrupt

## 0. 先给结论：RISC-V Interrupt 最核心的 mental model

如果只记住这一章的一张图，我建议记住下面这张：

```text
                         RISC-V Hart
                            │
       ┌────────────────────┼────────────────────┐
       │                    │                    │
 Software Interrupt     Timer Interrupt      External Interrupt
     MSI/SSI              MTI/STI              MEI/SEI
       │                    │                    │
       │                    │              PLIC / AIA
       │                    │                    │
       └────────────────────┼────────────────────┘
                            │
                           xip
                     interrupt pending
                            │
                           xie
                     interrupt enable
                            │
                       delegation
                        mideleg
                            │
                     privilege check
                            │
                     mstatus/sstatus
                         MIE/SIE
                            │
                            ▼
                       Trap taken
                            │
              ┌─────────────┼─────────────┐
              │             │             │
            xepc          xcause        xtvec
              │             │             │
              └─────────────┼─────────────┘
                            ▼
                     interrupt handler
                            │
                         xRET
```

**RISC-V 中断机制的本质不是“某个中断控制器”。**

真正的架构模型是：

> **Interrupt Source → Pending → Enable → Delegation → Privilege Arbitration → Trap Entry**

其中：

```text
mip/sip    = pending
mie/sie    = enable
mideleg    = interrupt 属于 M 还是 S
mstatus    = 当前 privilege 是否允许接收
mtvec      = handler 在哪里
mcause     = 为什么进 handler
mepc       = 被打断的位置
mret/sret  = 返回
```

PLIC、APLIC、IMSIC 都只是其中 **External Interrupt Source → Hart** 这一部分。

这是理解 RISC-V interrupt 最重要的一点。

---

# 1. 先把 Arm 思维切换过来

你有 Armv8-A / Armv7-A / Armv8-M 经验，所以很容易产生一个自然联想：

```text
Device
  ↓
 GIC
  ↓
 IRQ
  ↓
 CPU
```

然后会想：

> RISC-V 对应的 GIC 是什么？

如果从这个问题开始，就很容易把 RISC-V interrupt 理解窄了。

更加准确的比较应该是：

```text
Arm
=================================================

Timer ─────────┐
SGI ───────────┤
PPI ───────────┤
SPI ───────────┤
               ▼
              GIC
               │
               ▼
           IRQ/FIQ
               │
               ▼
              CPU


RISC-V
=================================================

software interrupt ───────────┐
timer interrupt ──────────────┤
                              ├──→ Hart interrupt logic
local interrupt ──────────────┤
                              │
Device → PLIC/APLIC/IMSIC ────┘
```

所以：

> **RISC-V 把 Hart 本身的 interrupt architecture 和 platform interrupt controller 分得更开。**

这正是为什么你会看到：

```text
mip
mie
mideleg
mtvec
```

这些是 ISA / Privileged Architecture 的一部分；

而：

```text
PLIC
APLIC
IMSIC
```

则属于 platform/system interrupt architecture。

当前官方 Privileged ISA 仍以 `mip/mie`、`sip/sie` 等作为 hart interrupt architecture 的基础，而 AIA 1.0 在此之上扩展 local interrupts、MSI、APLIC/IMSIC 和虚拟化能力。:chatgpt-content-reference{index="0"}

---

# 2. Interrupt 和 Exception 在 RISC-V 中为什么分得这么清楚？

我们在前面的 Trap 章节已经建立过：

```text
Trap
├── Exception
│    synchronous
│
└── Interrupt
     asynchronous
```

例如：

```text
lw a0, 0(a1)
```

如果产生 page fault：

```text
instruction
   │
   └── 当前这条 instruction 直接导致
           ↓
        Exception
```

而 UART 收到数据：

```text
CPU 正在执行：

add
sub
ld
...
           UART RX
              │
              ▼
          Interrupt
```

Interrupt 和当前执行哪条 instruction 没有直接因果关系。

所以 RISC-V 在 `mcause/scause` 中明确保存：

```text
XLEN-1                       0
+----------+------------------+
|Interrupt | Exception Code   |
+----------+------------------+
```

例如 RV64：

```text
bit 63 = 1
```

表示 interrupt。

```text
bit 63 = 0
```

表示 synchronous exception。

官方规范也明确规定 `mcause/scause` 的最高 Interrupt bit 用于区分这两类 trap。:chatgpt-content-reference{index="1"}

---

# 3. RISC-V 最基本的三类 interrupt

在不考虑 AIA/H extension/local extensions 的情况下，最重要的是：

```text
                 Interrupt
                     │
          ┌──────────┼──────────┐
          │          │          │
       Software     Timer     External
          │          │          │
      IPI 等       scheduler   UART/SPI/PCIe...
```

而它们又分：

```text
Machine level

MSI = Machine Software Interrupt
MTI = Machine Timer Interrupt
MEI = Machine External Interrupt


Supervisor level

SSI = Supervisor Software Interrupt
STI = Supervisor Timer Interrupt
SEI = Supervisor External Interrupt
```

注意这里一个非常容易混淆的地方：

> 这里的 `MSI` 是 **Machine Software Interrupt**。

它和 PCIe 世界中的：

> **Message Signaled Interrupt**

恰好缩写也叫 MSI。

两者完全不同。

AIA 文档里提到的 MSI 通常是：

```text
Message Signaled Interrupt
```

而：

```text
mip.MSIP
```

这里的 MSI 是：

```text
Machine Software Interrupt
```

读规范时一定注意上下文。

---

# 4. 六个核心 interrupt cause

这是必须熟悉的一张表：

| Cause | Interrupt |
|---:|---|
| 1 | Supervisor software interrupt |
| 3 | Machine software interrupt |
| 5 | Supervisor timer interrupt |
| 7 | Machine timer interrupt |
| 9 | Supervisor external interrupt |
| 11 | Machine external interrupt |

因此可以很有规律地记：

```text
Software:
S = 1
M = 3

Timer:
S = 5
M = 7

External:
S = 9
M = 11
```

你甚至可以看到一个规律：

```text
          S       M
Software  1       3

Timer     5       7

External  9      11
```

而 H extension 又插入：

```text
VS software = 2
VS timer    = 6
VS external = 10
```

于是：

```text
Software:   S=1   VS=2   M=3
Timer:      S=5   VS=6   M=7
External:   S=9   VS=10  M=11
```

这套编号非常值得记住。官方 `mcause/scause` 表就是按照这套 major interrupt identity 编码。:chatgpt-content-reference{index="2"}

---

# 5. interrupt 最重要的 CSR：`mip` + `mie`

理解 RISC-V interrupt，我建议首先完全理解这两个寄存器。

## 5.1 `mip`：Machine Interrupt Pending

全称：

```text
mip
Machine Interrupt-Pending Register
```

可以把它理解为：

> **哪些 interrupt 已经来了？**

典型 bit：

```text
mip

11     9      7      5      3      1
│      │      │      │      │      │
MEIP   SEIP   MTIP   STIP   MSIP   SSIP
```

例如：

```text
UART interrupt
      ↓
    PLIC
      ↓
MEIP = 1
```

或者：

```text
mtime >= mtimecmp
        ↓
      MTIP = 1
```

---

# 6. `mie`：Interrupt Enable

全称：

```text
mie
Machine Interrupt-Enable Register
```

可以简单理解：

```text
mip = interrupt 是否 pending

mie = 我想不想接收这个 interrupt
```

对应关系非常漂亮：

```text
mip               mie

MEIP <---------> MEIE
MTIP <---------> MTIE
MSIP <---------> MSIE

SEIP <---------> SEIE
STIP <---------> STIE
SSIP <---------> SSIE
```

因此：

```text
mip.MTIP = 1
```

只表示：

> timer interrupt 已经 pending。

但如果：

```text
mie.MTIE = 0
```

CPU 不会因此进入 M-mode timer interrupt handler。

所以 interrupt 的基本条件是：

```text
pending
AND
enable
```

即：

```text
mip[i] && mie[i]
```

官方规范也是如此定义：interrupt cause `i` 对应 `mip[i]` 和 `mie[i]`。:chatgpt-content-reference{index="3"}

---

# 7. 还有第三层开关：`mstatus.MIE`

这里是很多初学者第一次容易混乱的地方。

你已经有：

```text
mip.MTIP
```

和：

```text
mie.MTIE
```

为什么还有：

```text
mstatus.MIE
```

？

因为：

```text
mie
```

相当于：

> per-interrupt enable

而：

```text
mstatus.MIE
```

相当于：

> M-mode global interrupt enable

例如：

```text
             Individual enable
                   mie.MTIE
                      │
MTIP pending ─────────AND──────┐
                               │
                               AND → take interrupt
                               │
                 mstatus.MIE ──┘
                 Global enable
```

这就很像你熟悉的：

```text
NVIC interrupt enable
+
PRIMASK/BASEPRI
```

但机制不完全相同。

---

# 8. 一个极其重要的 privilege 规则

这里是 RISC-V interrupt architecture 中很漂亮的一点。

假设：

```text
当前正在 M-mode
```

那么 M interrupt 是否能进入取决于：

```text
mstatus.MIE
```

但是假设：

```text
当前正在 S-mode
```

此时如果出现一个**没有 delegated 的 M-mode interrupt**：

```text
MEI
```

那么：

> 它不需要看 `mstatus.MIE` 是否为 1，就可以抢占 S-mode 进入 M-mode。

原因是：

```text
M > S
```

高 privilege interrupt 本身就有资格 preempt lower privilege execution。

因此 M interrupt 的核心判断可以记成：

```text
pending
&&
enabled
&&
not delegated
&&
(
     current_mode < M
     ||
     (current_mode == M && mstatus.MIE)
   )
```

官方规范明确采用了这个条件。:chatgpt-content-reference{index="4"}

这个设计你可以类比：

```text
EL1 正在执行

EL3 interrupt/event
```

高 privilege 的事件不应该被低 privilege 的 global mask 控制。

---

# 9. S-mode 也有一套镜像

对应：

```text
mip     → sip
mie     → sie

mstatus → sstatus

MIE     → SIE

mtvec   → stvec

mepc    → sepc

mcause  → scause

mret    → sret
```

所以 S-mode 的 interrupt mental model 完全一样：

```text
sip.SEIP
   │
sie.SEIE
   │
sstatus.SIE
   │
   ▼
S-mode interrupt
```

但有一个非常重要的前提：

> 这个 interrupt 必须属于 S-mode。

谁决定？

答案：

```text
mideleg
```

---

# 10. `mideleg`：Interrupt Delegation

这正是上一章 Trap 中 delegation 在 interrupt 上的体现。

全称：

```text
mideleg
Machine Interrupt Delegation Register
```

例如：

```text
mideleg[9] = 1
```

代表：

```text
Supervisor External Interrupt
```

被 delegate 给 S-mode。

此时：

```text
SEIP
 ↓
sip.SEIP
 ↓
sie.SEIE
 ↓
sstatus.SIE
 ↓
stvec
```

而不是进入：

```text
mtvec
```

---

# 11. 为什么需要 delegation？

考虑一个 Linux 系统：

```text
M-mode
OpenSBI

S-mode
Linux
```

如果所有设备 interrupt 都先进入：

```text
OpenSBI
```

然后：

```text
M-mode
   ↓
分析 UART interrupt
   ↓
重新通知 Linux
   ↓
S-mode
```

那么路径就会非常低效：

```text
Device
 ↓
M-mode
 ↓
OpenSBI
 ↓
S-mode
 ↓
Linux
```

而 delegation 允许：

```text
Device
 ↓
S external interrupt
 ↓
Linux
```

M-mode 只负责：

```text
policy
configuration
protection
```

而不必参与每一次 interrupt。

这就是 delegation 的根本价值：

> **让低 privilege OS 可以直接处理属于它的 interrupt，而无需高 privilege software 做 trap forwarding。**

---

# 12. `sip/sie` 本质上是什么？

一个非常值得理解的地方：

> `sip/sie` 并不是完全独立于 `mip/mie` 的第二套硬件。

更准确地说，它们是：

```text
mip/mie
   │
   │ delegation/filter
   ▼
sip/sie
```

官方规范称 `sip/sie` 是 `mip/mie` 的 restricted view；一个 interrupt 被 `mideleg` 委托到 S-mode 后，相应 interrupt state 才对 S-mode 可见、可通过 `sie` mask。:chatgpt-content-reference{index="5"}

因此：

```text
              mip/mie
                 │
              mideleg
                 │
            ┌────┴────┐
            │         │
        remain M    delegated
            │         │
            │       sip/sie
            │         │
          M-mode    S-mode
```

这是一个很好的 mental model。

---

# 13. Interrupt 到底什么时候进入 trap？

现在把所有条件组合起来。

假设一个 S-mode external interrupt。

硬件大致判断：

```text
SEIP == 1
   &&
SEIE == 1
   &&
interrupt belongs to S-mode
   &&
(
    current_priv < S

    OR

    current_priv == S && sstatus.SIE == 1
)
```

那么：

```text
Trap → S-mode
```

如果当前正在：

```text
M-mode
```

那么 S interrupt **不能反过来打断 M-mode**。

因为：

```text
M > S
```

所以 privilege hierarchy 永远先起作用。

你可以把整个 arbitration 理解成：

```text
               pending?
                  │
             enable?
                  │
             delegated?
                  │
        target privilege level
                  │
       compare current privilege
                  │
          global enable check
                  │
                trap
```

---

# 14. Trap Entry：CPU 自动干了什么？

假设当前：

```text
S-mode
```

收到 S interrupt。

CPU 并不会像 Cortex-M exception entry 那样自动把：

```text
r0-r3
r12
LR
PC
xPSR
```

压 stack。

RISC-V 的 philosophy 更接近 Arm A-profile：

> **硬件只保存最小 architectural state，其余 register 由 software handler 保存。**

发生 S-mode interrupt 后，核心动作大致是：

```text
sepc    ← interrupted PC

scause  ← interrupt + cause

sstatus.SPP
        ← previous privilege

sstatus.SPIE
        ← sstatus.SIE

sstatus.SIE
        ← 0

pc      ← stvec
```

然后 software handler 自己：

```asm
addi sp, sp, -FRAME_SIZE

sd ra, ...
sd t0, ...
sd t1, ...
...
```

保存 context。

这点和：

```text
Cortex-M
```

差异很大。

Cortex-M：

```text
hardware stacking
```

RISC-V：

```text
software context save
```

AIA 1.0 甚至明确指出，其目标不是提供自动寄存器 stacking 或基于优先级的自动 interrupt nesting；这些实时系统优化不属于当前 AIA 的基础设计。:chatgpt-content-reference{index="6"}

---

# 15. 为什么进入 handler 后自动把 SIE/MIE 清零？

假设：

```text
SIE = 1
```

发生 interrupt。

如果进入 handler 后依然：

```text
SIE = 1
```

那么还没保存现场：

```asm
handler:
    sd ra, ...
```

另一中断又进来了：

```text
Interrupt A
   ↓
handler
   ↓
还没保存 context
   ↓
Interrupt B
```

软件实现会非常困难。

因此硬件自动：

```text
SPIE ← SIE
SIE  ← 0
```

也就是：

```text
进入 interrupt handler：

remember old state
+
temporarily disable same-level interrupts
```

之后如果 OS 想允许 interrupt nesting：

```asm
csrsi sstatus, SSTATUS_SIE
```

可以显式重新打开。

---

# 16. `SPIE/MPIE` 是什么？

可以理解为：

```text
Previous Interrupt Enable
```

例如：

```text
SIE = 1
```

interrupt 进入：

```text
SPIE = 1
SIE  = 0
```

然后：

```text
sret
```

自动：

```text
SIE ← SPIE
```

所以：

```text
SPIE
```

就是 interrupt entry 时对原始 SIE 状态的一个“硬件小备份”。

M-mode：

```text
MIE
MPIE
MPP
```

S-mode：

```text
SIE
SPIE
SPP
```

和上一章 Exception 完全共享同一套 trap machinery。

---

# 17. `mtvec/stvec`：Interrupt handler 在哪里？

`xtvec`：

```text
Trap Vector Base Address Register
```

有两个特别重要的模式：

```text
Direct

Vectored
```

---

## Direct Mode

```text
MODE = 0
```

所有：

```text
Exception
Interrupt
```

都进入：

```text
BASE
```

即：

```text
                 exception
                    │
                    │
interrupt ──────────┼───────────→ BASE
                    │
                    │
```

handler 再读取：

```text
mcause/scause
```

判断原因。

---

# 18. Vectored Mode

```text
MODE = 1
```

对于 interrupt：

```text
PC = BASE + 4 × cause
```

例如：

```text
Machine Timer Interrupt

cause = 7
```

那么：

```text
PC = BASE + 4 * 7
   = BASE + 28
```

S timer：

```text
cause = 5

PC = BASE + 20
```

官方 `stvec` 规范就是这样定义；同步 exception 在 Vectored mode 下仍然进入 BASE，只有 asynchronous interrupt 使用 `BASE + 4*cause`。:chatgpt-content-reference{index="7"}

所以典型 vector table：

```asm
trap_vector:

    j exception_handler     # BASE + 0

    j s_soft_handler        # cause 1

    j reserved              # cause 2

    j m_soft_handler        # cause 3

    j reserved

    j s_timer_handler       # cause 5

    j reserved

    j m_timer_handler       # cause 7

    j reserved

    j s_ext_handler         # cause 9

    j reserved

    j m_ext_handler         # cause 11
```

你会发现每个：

```asm
j xxx
```

刚好：

```text
4 bytes
```

因此：

```text
BASE + 4*cause
```

非常自然。

---

# 19. 这和 Arm Vector Table 有什么区别？

Arm AArch64：

```text
VBAR_EL1

+0x000 current EL SP0 sync
+0x080 current EL SP0 IRQ
+0x100 current EL SP0 FIQ
...

+0x400 lower EL AArch64 sync
+0x480 lower EL AArch64 IRQ
...
```

vector table 结构由架构固定。

RISC-V：

```text
stvec.BASE
```

配合：

```text
Direct
or
Vectored
```

结构简单很多。

RISC-V Vectored：

```text
interrupt cause number
        ↓
BASE + cause*4
```

这体现了 RISC-V 很典型的：

> **minimal hardware + software policy**

设计思想。

---

# 20. Timer Interrupt：必须深入理解

RISC-V 基础 timer 模型非常值得认真理解。

核心是两个东西：

```text
mtime

mtimecmp
```

其中：

```text
mtime
```

是持续增长的 64-bit real-time counter。

```text
mtimecmp
```

是当前 hart 的 timer compare。

条件：

```text
mtime >= mtimecmp
```

则：

```text
MTIP = 1
```

也就是：

```text
Machine Timer Interrupt Pending
```

官方 Privileged ISA 就是这样定义 machine timer。:chatgpt-content-reference{index="8"}

逻辑非常简单：

```text
                     +----------------+
                     |     mtime      |
                     +----------------+
                             │
                             │ >=
                             ▼
                     +----------------+
                     |   mtimecmp     |
                     +----------------+
                             │
                             ▼
                          MTIP=1
                             │
                         mie.MTIE
                             │
                             ▼
                   Machine timer trap
```

---

# 21. timer interrupt 是怎么清除的？

一个很容易误解的地方：

你通常不是：

```text
clear MTIP
```

而是重新设置：

```text
mtimecmp
```

例如：

```text
mtime = 1000

下一次 tick：
mtimecmp = 1100
```

当：

```text
mtime < mtimecmp
```

以后：

```text
MTIP
```

自然 deassert。

所以 timer ISR：

```c
void timer_interrupt(void)
{
    uint64_t now = mtime;

    mtimecmp = now + TICK_INTERVAL;

    scheduler_tick();
}
```

这是 compare timer 的典型设计。

和：

```text
ARM Generic Timer
CNTPCT
CNTP_CVAL
```

在设计思路上非常相似。

---

# 22. 那 Linux S-mode 怎么使用 timer？

这是非常关键的一点。

基础 Privileged Architecture 的传统模型中：

```text
mtime/mtimecmp
```

是 Machine timer facility。

但是 Linux 跑在：

```text
S-mode
```

于是传统路径是：

```text
Linux
S-mode
   │
   │ SBI set_timer
   ▼
OpenSBI
M-mode
   │
   │ program mtimecmp
   ▼

mtime >= mtimecmp
   │
   ▼
MTIP
   │
   ▼
M-mode
   │
   │ arrange S timer
   ▼
STIP
   │
   ▼
Linux timer handler
```

也就是说：

> 早期/基础实现中 Supervisor timer service 需要 M-mode firmware 帮忙。

---

# 23. `Sstc`：让 S-mode 自己设置 timer

后来加入：

```text
Sstc
Supervisor-mode Timer Interrupt extension
```

引入：

```text
stimecmp
```

于是：

```text
S-mode Linux
     │
     │ write stimecmp
     ▼

time >= stimecmp
     │
     ▼
STIP
     │
     ▼
S-mode timer interrupt
```

不再需要：

```text
S → SBI → M
```

来设置每一次 timer。

这显著减少 timer programming 的 privilege transition overhead。官方 Sstc 规范的设计动机正是消除 S/HS timer 由 M-mode 模拟所带来的开销。:chatgpt-content-reference{index="9"}

这点如果你以后分析 Linux RISC-V timer code，非常重要。

---

# 24. Software Interrupt：本质上主要用于 IPI

再来看：

```text
Software Interrupt
```

这个名字一开始容易令人误解。

它不是：

```text
software exception
```

也不是：

```text
ECALL
```

而是真正的：

```text
asynchronous interrupt
```

只不过它通常由：

```text
software on another hart
```

触发。

典型场景：

```text
Hart 0
   │
   │ send IPI
   ▼
Hart 1
   │
   ▼
software interrupt
```

也就是：

```text
Inter-Processor Interrupt
```

---

# 25. IPI 为什么重要？

SMP kernel 到处需要 IPI：

```text
scheduler reschedule

TLB shootdown

stop CPU

call function on another CPU

RCU

CPU hotplug
```

例如：

```text
Hart 0

modify page table
      │
      │
      ├──────→ local sfence.vma
      │
      │
      └── IPI → Hart 1
                   │
                   ▼
               sfence.vma
```

这就是：

```text
TLB shootdown
```

你在 Linux / SMP 中已经非常熟悉。

---

# 26. Machine software interrupt 和 Supervisor software interrupt

分别是：

```text
MSIP
Machine Software Interrupt Pending

SSIP
Supervisor Software Interrupt Pending
```

对于 OS 而言主要关注：

```text
SSIP
```

而 M-mode firmware 可以提供：

```text
SBI IPI
```

例如：

```text
Linux Hart0
   │
   │ SBI send_ipi
   ▼
OpenSBI
   │
   ▼
target Hart1
   │
   ▼
SSIP
   │
   ▼
Linux IPI handler
```

所以你以后看到：

```text
SBI
IPI
SSIP
```

就应该马上把它们联系起来。

---

# 27. External Interrupt 才轮到 PLIC

前面的：

```text
software interrupt
timer interrupt
```

本身不需要 PLIC。

而设备：

```text
UART
SPI
Ethernet
PCIe
GPIO
```

通常属于：

```text
External Interrupt
```

传统 RISC-V 平台使用：

```text
PLIC
Platform-Level Interrupt Controller
```

官方 Ratified PLIC 规范为 v1.0.0。:chatgpt-content-reference{index="10"}

架构：

```text
UART IRQ ───────┐
SPI IRQ ────────┤
Ethernet IRQ ───┤
GPIO IRQ ───────┤
                ▼
              PLIC
                │
         priority/routing
                │
         ┌──────┴──────┐
         ▼             ▼
      Hart 0          Hart 1
      context         context
```

---

# 28. PLIC 的几个核心概念

你只需要先掌握：

```text
Source
Priority
Pending
Enable
Threshold
Context
Claim
Complete
```

完整路径：

```text
Device
  │
  │ IRQ
  ▼
PLIC Gateway
  │
  ▼
Pending
  │
Priority
  │
Enable
  │
Threshold
  │
  ▼
Hart Context
  │
  ▼
MEIP / SEIP
```

PLIC 规范定义了 priority、pending、per-context enable、threshold 以及 claim/complete 等 MMIO register blocks。:chatgpt-content-reference{index="11"}

---

# 29. PLIC `context` 是什么？

这是一个容易忽略但非常重要的概念。

PLIC 的 target 并不简单等价于：

```text
CPU
```

而是：

```text
Hart Context
```

典型可以理解：

```text
Hart 0 M-mode context
Hart 0 S-mode context

Hart 1 M-mode context
Hart 1 S-mode context
```

例如：

```text
UART
 ↓
PLIC
 ↓
Hart0 S context
 ↓
SEIP
```

所以：

> PLIC 可以针对“hart + privilege context”进行中断 delivery。

PLIC 规范也把 interrupt target 定义成 hart context，典型就是某 hart 的某 privilege mode。:chatgpt-content-reference{index="12"}

这比单纯说：

```text
route interrupt to CPU0
```

更准确。

---

# 30. 为什么 PLIC 需要 Priority 和 Threshold？

假设：

```text
UART    priority 1
Ethernet priority 3
Storage  priority 5
```

Hart context：

```text
threshold = 2
```

那么：

```text
UART      priority 1    blocked

Ethernet  priority 3    delivered

Storage   priority 5    delivered
```

条件可以粗略理解：

```text
interrupt priority > threshold
```

才有资格通知 target context。

而 priority 0 在 PLIC 中代表：

```text
never interrupt
```

官方 PLIC priority 规范就是如此规定。:chatgpt-content-reference{index="13"}

---

# 31. PLIC Claim / Complete

这是 PLIC 最值得记住的 programming model。

假设：

```text
UART
Ethernet
SPI
```

都连到 PLIC。

CPU 只看到：

```text
SEIP = 1
```

问题来了：

> 到底哪个 device interrupt？

所以 handler：

```text
read PLIC_CLAIM
```

PLIC 返回：

```text
interrupt ID
```

例如：

```text
10
```

OS：

```c
irq = plic_claim();

irq_table[irq]();

plic_complete(irq);
```

流程：

```text
Device
   ↓
PLIC
   ↓
SEIP
   ↓
stvec
   ↓
read CLAIM
   ↓
IRQ ID = 10
   ↓
UART ISR
   ↓
write COMPLETE(10)
```

这是 PLIC 的核心。

---

# 32. 为什么 `scause` 不能直接告诉你是 UART？

非常重要。

如果是 Supervisor External Interrupt：

```text
scause
```

只告诉你：

```text
interrupt = 1

cause = 9
```

即：

```text
Supervisor External Interrupt
```

它不知道：

```text
UART?
SPI?
Ethernet?
```

所以需要第二级 identification：

```text
scause = 9
        │
        ▼
"External interrupt"
        │
        ▼
      PLIC
        │
        ▼
     claim ID
        │
        ▼
      UART
```

这就是：

> **major interrupt identity + external-controller minor identity**

AIA 官方文档现在也明确使用 major/minor identity 这个模型：hart 的 `mcause/scause` 给出 major identity，而外部控制器再区分具体 device/source。:chatgpt-content-reference{index="14"}

这对理解 AIA 非常重要。

---

# 33. 一个完整 UART IRQ 例子

现在把前面所有东西连起来。

假设：

```text
Linux
S-mode

UART IRQ = PLIC source 10
```

初始化：

```text
PLIC priority[10] = 1

PLIC enable[S-context][10] = 1

PLIC threshold[S-context] = 0

sie.SEIE = 1

sstatus.SIE = 1

stvec = trap_vector
```

UART 收到字符：

```text
UART RX ready
     │
     ▼
UART IRQ asserted
     │
     ▼
PLIC source 10 pending
     │
priority > threshold
     │
source enabled
     │
     ▼
S context external interrupt
     │
     ▼
SEIP = 1
     │
sie.SEIE = 1
     │
sstatus.SIE = 1
     │
     ▼
Trap into S-mode
```

hardware：

```text
sepc   ← interrupted PC

scause ← Interrupt=1, Code=9

SPIE   ← SIE
SIE    ← 0

PC     ← stvec
```

software：

```text
trap_vector
    ↓
read scause
    ↓
cause == SEI
    ↓
PLIC claim
    ↓
irq = 10
    ↓
uart_isr()
    ↓
clear UART interrupt source
    ↓
PLIC complete(10)
    ↓
sret
```

最终：

```text
resume interrupted code
```

如果你能够完全理解这条路径，那么传统 RISC-V external interrupt 已经掌握大半。

---

# 34. Interrupt 的“清除”其实有好几个层次

这是实际驱动开发中极其重要的一点。

假设 UART：

```text
UART RX interrupt
```

你可能涉及三层状态：

```text
UART device
     │
     ▼
PLIC pending
     │
     ▼
sip.SEIP
```

不能简单认为：

```text
清 SEIP
```

就结束。

真正应该处理的是：

```text
1. service / clear device interrupt source

2. PLIC complete

3. PLIC重新评估 pending source

4. SEIP eventually deassert
```

如果 UART condition 一直存在：

```text
RX FIFO non-empty
```

那么你即使 complete：

```text
IRQ
```

也可能很快再次 pending。

这个思维与你做 NuttX device driver 时完全一样：

> **首先处理真正 interrupt source，而不是只想着清 CPU interrupt pending bit。**

---

# 35. RISC-V 有 IRQ / FIQ 吗？

**没有 Arm 那种 architectural IRQ/FIQ 二分。**

Arm：

```text
IRQ
FIQ
```

是 CPU exception type。

RISC-V 标准 interrupt architecture 更像：

```text
interrupt identity
+
privilege level
+
priority
```

例如：

```text
MEI
MSI
MTI
SEI
SSI
STI
```

所以不存在：

```text
"This device uses FIQ"
```

这样的标准架构概念。

但并不意味着 RISC-V 没有：

```text
high priority interrupt
```

它可以通过：

```text
higher privilege

interrupt priority

PLIC priority

AIA priority

local interrupt

NMI
```

来实现不同的 urgency。

---

# 36. NMI 又是什么？

NMI：

```text
Non-Maskable Interrupt
```

与：

```text
IRQ/FIQ
```

又不是一回事。

RISC-V 目前还有标准化的：

```text
Smrnmi
```

即：

```text
Resumable Non-Maskable Interrupt
```

它引入：

```text
mnepc
mncause
mnstatus
mnscratch
MNRET
```

使 RNMI 可以拥有独立的 trap state，避免覆盖普通 M-mode trap 的 `mepc/mcause`。当前 Privileged ISA v20260120 已包含 ratified `Smrnmi`。:chatgpt-content-reference{index="15"}

所以可以粗略建立：

```text
Arm FIQ
≠
RISC-V NMI
```

千万不要直接对应。

---

# 37. Interrupt priority 到底在哪里？

这是 RISC-V interrupt architecture 一个容易令人困惑的地方，因为有**多个 priority domain**。

至少要区分：

```text
Hart major interrupt priority

vs

PLIC external interrupt priority
```

例如：

```text
MTI
MSI
MEI
```

之间有 hart-level priority。

而：

```text
UART
SPI
Ethernet
```

之间有 PLIC priority。

这是两层 priority：

```text
              Hart

       MEI > MSI > MTI
        │
        │
        ▼
       PLIC
        │
        ├── UART priority
        ├── SPI priority
        └── Ethernet priority
```

传统 Privileged Architecture 对同时进入 M-mode 的基本 interrupts 给出的默认顺序是：

```text
MEI
 >
MSI
 >
MTI
 >
SEI
 >
SSI
 >
STI
```

Supervisor 同级常规顺序则是：

```text
SEI > SSI > STI
```

官方 Privileged spec 明确规定了这些默认顺序。:chatgpt-content-reference{index="16"}

---

# 38. PLIC 不负责 Hart 内部 interrupt nesting

这个区别对 Arm 工程师很重要。

你可能习惯：

```text
GIC priority
+
CPU priority masking
+
preemption
```

PLIC 并不是一个完整对应 GIC CPU interface 的东西。

PLIC 主要负责：

```text
external interrupt source
       ↓
priority
       ↓
target context
```

而：

```text
MEI
MSI
MTI
```

这种 hart-level interrupt arbitration 是 CPU architecture 自己处理。

PLIC 规范也明确指出，它不负责不同 hart context 之间的 interrupt preemption/nesting；这属于 hart/core 一侧。:chatgpt-content-reference{index="17"}

所以：

> **PLIC ≠ GIC。**

这个结论很重要。

---

# 39. 那 AIA 是什么？

现在进入现代 RISC-V。

AIA：

```text
Advanced Interrupt Architecture
```

Ratified version：

```text
AIA 1.0
```

官方当前发布页标记为 v1.0、2025-03-12 clarification revision。:chatgpt-content-reference{index="18"}

它主要引入：

```text
AIA
├── Hart interrupt architecture extensions
│
├── APLIC
│    Advanced Platform-Level Interrupt Controller
│
└── IMSIC
     Incoming Message-Signaled Interrupt Controller
```

核心目标之一就是：

> 更好地支持 MSI、PCIe、高性能系统和 virtualization。:chatgpt-content-reference{index="19"}

---

# 40. 为什么 PLIC 不够？

PLIC 的经典模型：

```text
Device
  │
 wired IRQ
  ▼
PLIC
  │
 shared external interrupt signal
  ▼
Hart
  │
 claim
  ▼
interrupt ID
```

对于现代：

```text
PCIe
MSI/MSI-X
virtual machine
device passthrough
```

不够理想。

现代高性能中断越来越倾向：

```text
Device
  │
  │ memory write
  ▼
per-CPU interrupt structure
```

这也是：

```text
Message Signaled Interrupt
```

的思想。

AIA 就是针对这种系统设计。

---

# 41. IMSIC：最值得理解的 AIA 组件

IMSIC：

```text
Incoming Message-Signaled Interrupt Controller
```

你可以先粗略把它理解成：

> **一个紧靠 hart 的 MSI receiver / interrupt file。**

架构：

```text
PCIe Device
      │
      │ MSI write
      ▼
    IMSIC
      │
      ▼
Hart interrupt file
      │
      ▼
SEIP / MEIP
```

而不是：

```text
Device
 ↓
central PLIC
 ↓
CPU
```

官方 AIA 定义 IMSIC 为与 hart 紧密耦合的 incoming MSI controller，一般每个 hart 一个，并为不同 privilege level 提供不同 interrupt file。:chatgpt-content-reference{index="20"}

---

# 42. IMSIC interrupt file

例如一个支持 S-mode 的 hart：

```text
Hart 0 IMSIC

├── M interrupt file
│
└── S interrupt file
```

如果支持 H extension：

```text
Hart 0 IMSIC

├── M interrupt file
├── S interrupt file
│
├── Guest interrupt file 0
├── Guest interrupt file 1
├── Guest interrupt file 2
...
```

于是一个 MSI 可以直接针对：

```text
Hart X
+
Privilege Y
+
Interrupt ID Z
```

甚至：

```text
Guest N
```

这是 virtualization 性能优化的关键基础。

---

# 43. APLIC 是什么？

APLIC：

```text
Advanced Platform-Level Interrupt Controller
```

主要解决：

```text
wired interrupts
```

例如：

```text
UART IRQ
GPIO IRQ
legacy peripheral IRQ
```

它可以工作为：

```text
Direct delivery
```

或者：

```text
MSI delivery
```

例如：

```text
UART wired IRQ
      │
      ▼
    APLIC
      │
      │ convert to MSI
      ▼
    IMSIC
      │
      ▼
     Hart
```

于是整个现代路径：

```text
Legacy device
   │
wired IRQ
   ▼
 APLIC
   │
 MSI
   ▼
 IMSIC
   │
   ▼
 Hart
```

而现代 PCIe device：

```text
PCIe
  │
 MSI
  └──────────────→ IMSIC
```

因此 APLIC 可以理解为：

> **wired interrupt front-end**

IMSIC 可以理解为：

> **per-hart MSI back-end**

这是一个非常实用的 mental model。

---

# 44. AIA 和 Arm GICv3 怎么对照？

不是严格 1:1，但可以辅助理解：

```text
Arm GICv3

Device
   │
   ▼
Distributor
   │
Redistributor
   │
CPU interface
   ▼
PE
```

RISC-V AIA：

```text
wired Device
   │
   ▼
APLIC
   │
   │ MSI
   ▼
IMSIC
   │
   ▼
Hart
```

而 PCIe：

```text
Device MSI
      │
      └────────→ IMSIC
```

你可能会自然想到：

```text
GICv3 ITS
```

因为它们都高度重视：

```text
MSI
virtualization
per-CPU delivery
```

这个类比是有帮助的。

但：

> **IMSIC 不是 ITS，APLIC 也不是 Distributor 的简单翻版。**

还是应该理解各自架构，而不是硬对应。

---

# 45. 为什么 IMSIC 对 virtualization 非常重要？

传统方案：

```text
Physical device
      │
      ▼
Hypervisor
      │
      │ emulate virtual interrupt controller
      ▼
Guest
```

路径比较重：

```text
device IRQ
 ↓
host
 ↓
hypervisor
 ↓
inject virtual interrupt
 ↓
guest
```

IMSIC + H extension 可以给 guest：

```text
Guest Interrupt File
```

于是配合 IOMMU，可以让 MSI 更接近：

```text
Device
   │
   │ MSI
   ▼
Guest IMSIC interrupt file
   │
   ▼
VS-mode
```

减少 hypervisor involvement。

这正是 AIA 的明确设计目标之一。:chatgpt-content-reference{index="21"}

这一点你以后学习：

```text
RISC-V virtualization
IOMMU
confidential computing
```

都会继续碰到。

---

# 46. AIA 中 Major ID / Minor ID 的概念

这可能是理解 AIA 最漂亮的一点。

Hart 首先看到：

```text
major interrupt
```

例如：

```text
SEI = 9
```

然后 external controller 再告诉你：

```text
minor interrupt ID
```

例如：

```text
UART = 32
NVMe = 45
Network = 51
```

即：

```text
scause
   │
   │ major = SEI
   ▼
IMSIC/APLIC
   │
   │ minor = 45
   ▼
NVMe IRQ
```

这与传统 PLIC：

```text
SEIP
+
claim ID
```

在思想上是一脉相承的。

---

# 47. RISC-V interrupt nesting 怎么做？

这里和 Cortex-M 差异很明显。

Cortex-M：

```text
IRQ A priority 10
IRQ B priority 5

IRQ B automatically preempts IRQ A
```

NVIC + exception architecture 自动支持。

RISC-V base architecture：

```text
enter interrupt
    │
    ▼
xIE automatically cleared
```

所以默认：

```text
same-level interrupt
```

不能立即嵌套。

如果 handler 希望允许 nesting：

```text
save context
   │
   ▼
software re-enable xIE
   │
   ▼
higher-priority interrupt
may preempt
```

也就是说：

> **RISC-V 更偏向 software-controlled interrupt nesting。**

AIA 可以增强 priority information，但 AIA 1.0 仍明确没有定义类似 Cortex-M 那样的自动基于优先级 nesting。:chatgpt-content-reference{index="22"}

---

# 48. 一个 interrupt nesting demo

假设：

```text
S external interrupt
```

进入：

```asm
s_irq_entry:

    addi sp, sp, -128

    sd ra, 0(sp)
    sd t0, 8(sp)
    sd t1, 16(sp)
    ...

    # context 已经安全保存
```

如果 OS 希望允许 S-level interrupt nesting：

```asm
    csrsi sstatus, 0x2   # SIE = 1
```

那么现在：

```text
another eligible S interrupt
```

可以再次进入。

返回前：

```asm
    csrci sstatus, 0x2

    ...
    restore context

    sret
```

当然真正 OS 实现会考虑：

```text
priority
per-CPU irq state
preemption count
locking
stack
```

不会这么简单。

---

# 49. 为什么 privilege priority 高于 interrupt priority？

假设同时：

```text
M timer interrupt

S external interrupt
```

当前正在：

```text
U-mode
```

即使：

```text
S external
```

在自己的 priority domain 中很高：

```text
M timer
```

仍属于更高 privilege target。

设计原则是：

```text
privilege arbitration
first

interrupt priority
second
```

AIA 也明确规定：

> destined to a more-privileged mode 的 interrupt 优先于 destined to lower privilege 的 interrupt。:chatgpt-content-reference{index="23"}

因为如果 lower privilege interrupt 可以阻止 higher privilege interrupt：

```text
S interrupt storm
       ↓
prevent M firmware from running
```

就会破坏 privilege hierarchy。

---

# 50. `WFI` 和 interrupt

系统 idle 时经常：

```asm
wfi
```

即：

```text
Wait For Interrupt
```

典型：

```text
scheduler
   │
no runnable task
   │
   ▼
WFI
   │
timer/device/IPI
   │
   ▼
wake CPU
```

但学习时要注意：

> `WFI` 更准确地说是一种等待机制，而不是简单的“开启中断并睡眠”。

它的具体 wake 行为和 implementation/privileged rules 有细节。

所以 OS idle code 不能简单认为：

```text
WFI == Cortex-M WFI 的所有语义完全一致
```

但宏观用途完全类似。

---

# 51. Interrupt 和 SMP 的关系

RISC-V interrupt 学到最后一定会进入：

```text
per-hart
```

这个概念。

RISC-V 中：

```text
Hart0

mip
mie
mtvec
mstatus

Hart1

mip
mie
mtvec
mstatus
```

这些都是：

> **per-hart architectural state**

不是所有 CPU 共享：

```text
one global mip
```

所以 SMP：

```text
       Device
         │
         ▼
     PLIC/AIA
       /   \
      /     \
   Hart0   Hart1
```

每个 hart 再分别拥有：

```text
pending
enable
trap state
```

IPI 的本质就是：

```text
Hart A
   │
   │ request
   ▼
Hart B interrupt pending
```

后面学习：

```text
09 SMP
```

时我们会把：

```text
IPI
secondary hart startup
scheduler
TLB shootdown
```

全部连起来。

---

# 52. RISC-V vs Arm：最值得记住的对照表

| Arm | RISC-V |
|---|---|
| IRQ exception | Interrupt trap |
| FIQ exception | 没有直接等价物 |
| DAIF.I/F | `xstatus.xIE`，但语义不同 |
| VBAR_ELx | `mtvec/stvec` |
| ESR_ELx | `mcause/scause` |
| ELR_ELx | `mepc/sepc` |
| SPSR_ELx | `mstatus/sstatus` 中 trap-state fields |
| GIC | PLIC / AIA，但非 1:1 |
| GIC Distributor | 部分可类比 PLIC/APLIC |
| GIC redistributor/CPU interface | 部分概念可帮助理解 IMSIC |
| SGI | software interrupt / IPI |
| Generic Timer interrupt | MTI/STI |
| SPI/PPI | 没有简单一一映射 |
| interrupt priority | Hart priority + PLIC/AIA priority |
| ERET | `mret/sret` |
| SMC/HVC | 与 interrupt 无关；RISC-V privileged transition 常见为 ECALL |

这里最重要的是三条：

```text
GIC ≠ PLIC

FIQ ≠ NMI

Arm priority model ≠ RISC-V priority model
```

---

# 53. 和 Cortex-M 再比较一次

你也熟悉 Armv8-M，这个对照尤其有价值。

Cortex-M：

```text
Peripheral
   │
   ▼
NVIC
   │
priority
   │
exception entry
   │
hardware stacking
   │
vector table
   │
ISR
```

RISC-V：

```text
Peripheral
   │
   ▼
PLIC/APLIC/IMSIC
   │
   ▼
xIP
   │
xIE
   │
xstatus
   │
trap entry
   │
xtvec
   │
software saves registers
   │
ISR
```

最大的 architecture philosophy 差异：

```text
Cortex-M
hardware assists RTOS / interrupt handling heavily

RISC-V application-profile style
hardware keeps primitive mechanism minimal
software controls policy
```

所以你不要用：

```text
NVIC mental model
```

直接理解 RISC-V。

它更接近：

```text
Arm A-profile + system interrupt controller
```

---

# 54. Debug interrupt 时，建议按这条链检查

假设：

> UART 已经触发 IRQ，但是 handler 就是不进。

不要随机看寄存器。

按照 architecture pipeline 一层一层检查：

```text
1. Device

UART IRQ condition 是否真的存在？
        │
        ▼

2. External controller

PLIC/APLIC/IMSIC
source pending？
source enabled？
priority/threshold？
target hart/context？
        │
        ▼

3. Hart pending

sip.SEIP / mip.MEIP
是否为 1？
        │
        ▼

4. Individual enable

sie.SEIE / mie.MEIE
是否为 1？
        │
        ▼

5. Delegation

mideleg
target privilege 是否正确？
        │
        ▼

6. Global enable

sstatus.SIE
mstatus.MIE
是否正确？
        │
        ▼

7. Trap vector

stvec / mtvec
地址正确？
        │
        ▼

8. Handler

scause/mcause
读到了什么？
        │
        ▼

9. Claim

PLIC/IMSIC interrupt ID 是什么？
        │
        ▼

10. Clear / Complete

device source 清了吗？
controller complete 了吗？
```

这一套调试顺序非常值得形成肌肉记忆。

---

# 55. 用一个“门电路”模型记住整个 interrupt architecture

如果你以后忘掉大量 CSR，只要记住：

```text
Interrupt Source
       │
       ▼
    pending
       │
       AND
       │
     enable
       │
       AND
       │
 delegation / target privilege
       │
       AND
       │
 privilege/global interrupt enable
       │
       ▼
      TRAP
```

即：

```text
IRQ =
    source_pending
    &&
    source_enabled
    &&
    target_this_privilege
    &&
    privilege_allows_interrupt
```

一旦成立：

```text
xepc     ← interrupted PC

xcause   ← interrupt identity

xstatus  ← save privilege / interrupt state

xIE      ← 0

pc       ← xtvec
```

然后：

```text
software save context
   ↓
identify source
   ↓
service source
   ↓
ack/complete
   ↓
restore context
   ↓
xret
```

**这就是 RISC-V Interrupt 的本质。**

---

# 56. 再把 PLIC/AIA 放进这张图

最终完整模型：

```text
                         RISC-V SYSTEM

 Local to Hart                            Platform Devices
───────────────────                      ──────────────────

Software / IPI                           UART
      │                                  SPI
      │                                  Ethernet
      │                                  PCIe
      │                                    │
      │                        ┌───────────┴──────────┐
      │                        │                      │
Timer                         PLIC               AIA
      │                        │               APLIC/IMSIC
      │                        │                      │
      └──────────────┬─────────┴──────────────────────┘
                     │
                     ▼
               interrupt pending
                  mip / sip
                     │
                     ▼
               interrupt enable
                  mie / sie
                     │
                     ▼
                 mideleg
                     │
                     ▼
                target mode
                  M / S
                     │
                     ▼
               status.MIE/SIE
                     │
                     ▼
                  trap
                     │
             ┌───────┼────────┐
             │       │        │
           xepc    xcause    xtvec
             │       │        │
             └───────┼────────┘
                     │
                     ▼
                IRQ handler
                     │
                     ▼
                mret / sret
```

这张图建议作为这一章最终 mental model。

---

# 57. 这一阶段你应该掌握到什么程度？

学完 Interrupt，我不要求你背所有 AIA CSR。

但至少应该能够直接回答这些问题：

1. `mip` 和 `mie` 的区别是什么？
2. `mie.MTIE` 和 `mstatus.MIE` 为什么同时存在？
3. 为什么 CPU 在 S-mode 时，M-mode interrupt 不一定需要 `mstatus.MIE=1`？
4. `mideleg` 对 interrupt routing 做了什么？
5. `sip/sie` 和 `mip/mie` 到底是什么关系？
6. `MTIP/STIP/MEIP/SEIP/MSIP/SSIP` 分别是什么？
7. `mcause=Interrupt|11` 代表什么？
8. `scause=Interrupt|9` 为什么还不能知道具体是 UART？
9. PLIC claim/complete 为什么存在？
10. `mtime/mtimecmp` 怎么产生 timer interrupt？
11. Linux 为什么传统上通过 SBI 设置 timer？
12. `Sstc` 解决什么问题？
13. IPI 为什么通常和 software interrupt 联系在一起？
14. PLIC 为什么不能简单理解为 RISC-V GIC？
15. APLIC 和 IMSIC 分别解决什么问题？
16. RISC-V 有没有 IRQ/FIQ？
17. interrupt entry 为什么不会自动保存所有 GPR？
18. `stvec Vectored` 怎么计算 handler address？
19. 默认进入 handler 后为什么 `SIE/MIE=0`？
20. 一个 UART IRQ 从 device 到 Linux handler 的完整路径是什么？

如果这 20 个问题你能够不查资料解释清楚，那么：

> **RISC-V Interrupt architecture 的主干就已经真正建立起来了。**

---

# 58. 最后再浓缩成 Staff Engineer 面试版

如果面试官问：

> **“简单介绍一下 RISC-V interrupt architecture。”**

我建议回答成下面这样：

> RISC-V 将 hart 内部 interrupt architecture 与 platform interrupt controller 分开。Hart 本身通过 `mip/mie` 或 `sip/sie` 表示 pending 和 per-interrupt enable，通过 `mstatus.MIE` / `sstatus.SIE` 提供 privilege-level global enable，并通过 `mideleg` 决定 interrupt 由 M-mode 还是 S-mode 处理。
>
> 基础架构主要定义 software、timer 和 external 三类 interrupt，对应 M/S privilege level，例如 MSIP/MTIP/MEIP 和 SSIP/STIP/SEIP。interrupt 被接受后，硬件写入 `xepc/xcause`，保存并关闭当前 privilege 的 interrupt-enable state，然后跳转到 `xtvec`；software handler 自己保存 GPR，处理 interrupt，最后通过 `mret/sret` 返回。
>
> Timer interrupt 基础设施由 `mtime/mtimecmp` 产生 MTIP，S-mode 传统上可通过 SBI 获取 timer service，Sstc 则允许 S-mode 使用 `stimecmp` 直接管理 timer。
>
> External interrupt 传统上通过 PLIC routing 和 priority arbitration，CPU 收到 MEIP/SEIP 后再通过 claim 获得具体 interrupt ID，并在处理后 complete。现代高性能平台可以使用 AIA，其中 APLIC 主要处理 wired interrupts，IMSIC 负责 per-hart MSI delivery，同时增强 virtualization support。
>
> 与 Arm 不同，RISC-V 没有 IRQ/FIQ 这样的固定 interrupt class，而是通过 privilege、interrupt identity 和 priority 来表达中断层次。

这已经是一个相当完整、同时又抓住架构本质的回答。

---

下一阶段 **05 PMP** 时，我们就可以把这里的 `M/S/U privilege` 和后面的物理内存访问权限真正连接起来：

```text
M/S/U
  │
  ├── Trap / Interrupt
  │
  └── PMP
       │
       ▼
physical memory protection
```

而如果按照知识依赖关系来看，**04 Interrupt 到这里最值得继续深入的一个支线其实是“拿 QEMU virt + OpenSBI + Linux，真实追一次 `UART → PLIC → SEIP → Linux irq_handler` 的代码路径”**。这会把今天所有 CSR、PLIC context、claim/complete 从“架构知识”变成真正的 kernel implementation knowledge。