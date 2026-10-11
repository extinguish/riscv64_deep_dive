下面这份我会把它作为我们前面规划中的 **03 Trap / Exception 正式教学章节**。这一章建议学透，因为后面的 Interrupt、Sv39 Page Fault、SBI、Hypervisor，本质上都会反复使用同一套 Trap machinery。

# RISC-V Architecture 系统学习 03：Trap / Exception

## 0. 本章先给出最重要的结论

如果这一章最后只记住一句话，请记住：

> **Exception / Interrupt 描述“发生了什么”；Trap 描述“CPU 因此把控制流转移给 Trap Handler”。**

RISC-V 官方术语是：

```text
Exception
    = 与当前正在执行的 instruction 相关的同步事件

Interrupt
    = 与当前 instruction 没有直接因果关系的异步事件

Trap
    = Exception 或 Interrupt 导致 CPU
      将控制流转移到 trap handler 的过程
```

也就是说：

```text
                 Trap
                  ▲
                  |
         +--------+--------+
         |                 |
    Exception           Interrupt
    synchronous        asynchronous
```

这是理解 RISC-V Trap architecture 的第一原则。官方规范明确如此定义 Exception、Interrupt 和 Trap。:chatgpt-content-reference{index="0"}

它与 Arm 的术语习惯有一个明显区别。

Arm Architecture 经常把：

```text
Exception
```

当成更大的总称，例如：

```text
SVC
Data Abort
Instruction Abort
IRQ
FIQ
SError
```

都属于 exception handling architecture。

而 RISC-V 更严格地区分：

```text
Exception != Interrupt

Exception + Interrupt
        ↓
       Trap
```

所以你看到：

```text
medeleg = Machine Exception Delegation
mideleg = Machine Interrupt Delegation
```

就很自然了。

---

# 1. 本章学习完成后，你应该具备什么能力？

这一章结束以后，你应该能够看到下面这些代码：

```asm
csrr  t0, scause
csrr  t1, sepc
csrr  t2, stval

...
csrw  sepc, t1
sret
```

立即在脑中恢复：

```text
发生 Trap
   ↓
CPU 保存：
    sepc
    scause
    stval
    sstatus.SPP/SPIE
   ↓
PC → stvec
   ↓
软件保存 GPR
   ↓
判断 scause
   ↓
处理 Exception
   ↓
恢复 GPR
   ↓
SRET
   ↓
PC ← sepc
```

并且能够进一步判断：

```text
这个 trap 为什么进 S-mode？

为什么没有进 M-mode？

sepc 应该 +4 吗？

stval 保存的是什么？

为什么 SRET 可以恢复原 privilege？

为什么 trap entry 不能直接用普通 C 函数？

为什么 kernel trap handler 必须自己保存 GPR？
```

如果这些都能够解释，这一章就真正掌握了。

---

# 2. 从 Arm 思维切入 RISC-V Trap

先建立一个非常粗略的对应关系。

| Armv8-A | RISC-V |
|---|---|
| Exception | Trap architecture 大体对应 |
| Synchronous Exception | Exception |
| IRQ/FIQ | Interrupt |
| VBAR_EL1 | `stvec` |
| ELR_EL1 | `sepc` |
| ESR_EL1 | `scause` |
| FAR_EL1 | `stval` |
| SPSR_EL1 | `sstatus` 的部分状态 |
| ERET | `SRET` |
| VBAR_EL3 | `mtvec` |
| ELR_EL3 | `mepc` |
| ESR_EL3 | `mcause` |
| FAR_EL3 | `mtval` |
| ERET | `MRET` |

因此你可以暂时建立：

```text
Arm EL1 exception handling:

VBAR_EL1
ELR_EL1
ESR_EL1
FAR_EL1
SPSR_EL1
ERET
```

对应：

```text
RISC-V S-mode trap handling:

stvec
sepc
scause
stval
sstatus
SRET
```

这个对应关系非常重要。

但是：

> **RISC-V 的 Trap Routing / Delegation 与 Arm Exception Routing 的设计方式非常不同。**

这一点后面重点讲。

---

# 3. Trap Architecture 的核心模型

先完全忽略中断控制器，只看 CPU Hart。

假设 CPU 正在 U-mode：

```text
                     Hart

               U-mode application
                       |
                       |
                    Exception
                       |
                       v
                Trap Routing
                       |
              +--------+--------+
              |                 |
              v                 v
           S-mode             M-mode
         trap handler       trap handler
```

CPU 必须解决五个问题：

```text
1. 为什么发生 Trap？
2. Trap 之前执行到哪里？
3. Trap 应该交给谁处理？
4. Trap Handler 从哪里开始执行？
5. Handler 完成后怎样回去？
```

RISC-V 分别提供：

```text
why?
 ↓
xcause

where?
 ↓
xepc

extra information?
 ↓
xtval

who?
 ↓
delegation + privilege

handler address?
 ↓
xtvec

return?
 ↓
xRET
```

所以你实际上可以把整个 Trap Architecture 压缩成：

```text
             Trap
               |
       +-------+-------+
       |       |       |
     xepc   xcause   xtval
       |
       v
     xtvec
       |
       v
   trap handler
       |
       v
      xRET
```

这里：

```text
x = m
```

就是：

```text
mepc
mcause
mtval
mtvec
mret
```

而：

```text
x = s
```

就是：

```text
sepc
scause
stval
stvec
sret
```

这就是 RISC-V Trap Architecture 最核心的骨架。

---

# 4. Exception 和 Interrupt 到底有什么本质区别？

这是 RISC-V 非常值得理解的设计。

## 4.1 Exception：同步事件

例如：

```asm
ld a0, 0(a1)
```

假设：

```text
a1 = invalid address
```

执行到这条指令时：

```text
instruction
    |
    v
address translation
    |
    X
page fault
```

于是：

```text
Load Page Fault
```

它与：

```text
ld a0, 0(a1)
```

存在直接因果关系。

所以是：

```text
synchronous exception
```

典型 Exception：

```text
Illegal Instruction

Breakpoint

ECALL

Instruction Page Fault
Load Page Fault
Store Page Fault

Instruction Access Fault
Load Access Fault
Store Access Fault

Address Misaligned
```

---

# 5. Interrupt：异步事件

例如 CPU 正在：

```asm
add a0, a1, a2
```

这时候 timer 到期。

Timer 并不是因为：

```asm
add
```

指令执行错误。

只是：

```text
CPU execution
      |
      |      timer expires
      |             |
      v             v
    normal       interrupt
      |
      +-------------+
             |
             v
            Trap
```

所以 Interrupt 是：

```text
asynchronous
```

本章只需要知道 Interrupt 也使用：

```text
xepc
xcause
xtvec
xstatus
xRET
```

同一套 Trap machinery。

至于：

```text
xip
xie
PLIC
APLIC
IMSIC
Timer
Software Interrupt
External Interrupt
interrupt priority
```

我们下一章 **04 Interrupt** 再单独深入。

---

# 6. `xcause`：到底为什么发生 Trap？

这是最重要的 CSR 之一。

以 RV64 为例：

```text
63                                            0
+------+--------------------------------------+
| INT  |          Exception Code              |
+------+--------------------------------------+
```

最高 bit：

```text
Interrupt = 0
    ↓
Exception

Interrupt = 1
    ↓
Interrupt
```

剩余 bit：

```text
cause code
```

因此：

```text
scause
```

同时可以表达：

```text
Exception

和

Interrupt
```

这也是为什么 CSR 不需要：

```text
sexceptioncause
sinterruptcause
```

两个寄存器。

官方规范就是使用 `xcause` 最高位区分 Interrupt，再用剩余字段记录 exception/interrupt cause。:chatgpt-content-reference{index="1"}

---

# 7. 常见 Exception Code

先掌握这些就足够覆盖绝大多数 kernel 场景。

| Code | Exception |
|---:|---|
| 0 | Instruction address misaligned |
| 1 | Instruction access fault |
| 2 | Illegal instruction |
| 3 | Breakpoint |
| 4 | Load address misaligned |
| 5 | Load access fault |
| 6 | Store/AMO address misaligned |
| 7 | Store/AMO access fault |
| 8 | Environment call from U-mode |
| 9 | Environment call from S-mode |
| 11 | Environment call from M-mode |
| 12 | Instruction page fault |
| 13 | Load page fault |
| 15 | Store/AMO page fault |

当前 Privileged Architecture 1.13 还定义了更新的异常类别，例如 software-check、hardware-error 等，因此不要把上表理解成完整的未来不变全集。:chatgpt-content-reference{index="2"}

但 Linux / RTOS / OpenSBI 学习前期，最重要的是：

```text
2   Illegal Instruction

3   Breakpoint

8   ECALL from U

9   ECALL from S

12  Instruction Page Fault

13  Load Page Fault

15  Store Page Fault
```

---

# 8. Page Fault 和 Access Fault 一定要区分

这是非常值得掌握的一个细节。

例如：

```text
Load Page Fault
```

通常表示：

```text
Virtual Memory Translation / PTE permission
```

出了问题。

例如：

```text
PTE.V = 0

PTE.R = 0

U/S permission violation
```

而：

```text
Load Access Fault
```

更倾向于：

```text
最终的 physical access
```

被：

```text
PMP
PMA
bus/system
```

等机制拒绝。

概念上：

```text
Virtual Address
     |
     v
  Page Table
     |
     X
  Page Fault
```

而：

```text
Virtual Address
     |
     v
Page Translation
     |
     v
Physical Address
     |
     X
PMP / PMA / system
     |
 Access Fault
```

RISC-V 专门把 page fault 与 physical-memory access fault 分成不同 cause，其中一个重要目的就是允许 S-mode OS 自己处理 page fault，而 M-mode firmware 保留对底层 physical protection fault 的控制。:chatgpt-content-reference{index="3"}

这个设计之后学习 PMP + Sv39 时会再次出现。

---

# 9. `xepc`：Trap 前 CPU 执行到哪里？

以：

```text
sepc
```

为例。

发生 Trap 时，CPU 会保存：

```text
sepc = interrupted / faulting instruction PC
```

例如：

```asm
0x1000: add a0,a1,a2
0x1004: ecall
0x1008: add a3,a4,a5
```

执行：

```text
ECALL
```

产生 exception：

```text
sepc = 0x1004
```

注意：

> **不是 0x1008。**

官方规范规定，当 Trap 进入对应 privilege 时，`xepc` 保存被中断或发生异常的 instruction virtual address。:chatgpt-content-reference{index="4"}

这直接引出一个极其重要的问题：

> Handler 返回时到底应该重新执行这条 instruction，还是跳过它？

答案：

**取决于 exception。**

---

# 10. 为什么有时 `sepc += 4`，有时绝对不能加？

这是操作系统 Trap Handler 中最重要的区别之一。

## 场景一：ECALL

```asm
ecall
```

产生：

```text
Environment Call exception
```

然后：

```text
sepc → ECALL
```

如果处理完 syscall 后直接：

```asm
sret
```

那么 CPU 又回到：

```asm
ecall
```

于是：

```text
ECALL
 ↓
Trap
 ↓
SRET
 ↓
ECALL
 ↓
Trap
 ↓
...
```

无限循环。

因此 syscall handler 通常：

```text
sepc += 4
```

因为 `ECALL` 本身是 32-bit instruction。

于是：

```text
ECALL
   |
   v
Trap
   |
   v
syscall
   |
   v
sepc += 4
   |
   v
SRET
   |
   v
next instruction
```

---

# 11. Page Fault 为什么通常不能 `sepc += 4`？

例如：

```asm
ld a0, 0(a1)
```

发生：

```text
Load Page Fault
```

OS 执行：

```text
allocate page
update PTE
SFENCE.VMA
```

然后应该：

```text
retry ld
```

所以：

```text
sepc 不变
```

执行：

```asm
sret
```

CPU 返回：

```asm
ld a0, 0(a1)
```

重新执行。

这次 page table 已经修好：

```text
ld
 ↓
success
```

所以：

```text
ECALL
    → advance PC

Page Fault
    → retry instruction
```

这个区别非常重要。

---

# 12. `xtval`：提供“额外错误信息”

`xcause` 告诉你：

```text
发生了什么类型的问题
```

例如：

```text
Load Page Fault
```

但 kernel 还需要知道：

```text
访问哪个地址发生了 page fault？
```

于是：

```text
stval
```

保存：

```text
faulting virtual address
```

例如：

```asm
ld a0, 0(a1)

a1 = 0x0000000040001234
```

发生：

```text
Load Page Fault
```

可能得到：

```text
scause = 13
sepc   = PC of LD
stval  = 0x0000000040001234
```

于是三者分别回答：

```text
scause:
    什么错误？

sepc:
    哪条 instruction？

stval:
    哪个地址/附加信息？
```

这是一个非常漂亮的分工：

```text
               Trap
                 |
       +---------+---------+
       |         |         |
      EPC      CAUSE      TVAL
       |         |         |
       v         v         v
     where      why      details
```

对于 breakpoint、misaligned、access fault、page fault 等异常，`xtval` 可以保存 faulting address；对 illegal instruction，某些实现还允许 `xtval` 保存 faulting instruction bits。具体哪些情况必须提供有效信息由相应架构/平台规则决定，某些实现也可能返回 0。:chatgpt-content-reference{index="5"}

---

# 13. `xtvec`：Trap Handler 从哪里开始执行？

例如：

```text
stvec
```

保存 S-mode Trap Vector。

逻辑：

```text
Trap
 |
 v
PC ← stvec
```

但实际上 `xtvec`：

```text
+----------------------------+------+
|            BASE            | MODE |
+----------------------------+------+
```

有两种重要模式：

```text
MODE = 0
Direct

MODE = 1
Vectored
```

---

# 14. Direct Mode

最简单：

```text
所有 Exception
所有 Interrupt
       |
       v
    BASE
```

例如：

```text
stvec.BASE = 0x80001000
```

那么：

```text
Illegal Instruction
Page Fault
ECALL
Timer Interrupt
External Interrupt
```

全部：

```text
PC = 0x80001000
```

然后软件：

```asm
csrr t0, scause
```

再 dispatch：

```text
trap_entry
    |
    +-- exception?
    |
    +-- interrupt?
         |
         +-- timer
         +-- external
         +-- software
```

---

# 15. Vectored Mode 有一个特别容易误解的地方

很多人看到：

```text
Vectored
```

会以为：

```text
每种 Exception 都有不同 vector
```

**这是错误的。**

RISC-V 的 Vectored Mode：

```text
Exception
    ↓
BASE

Interrupt
    ↓
BASE + 4 × cause
```

例如：

```text
Timer Interrupt cause = 5
```

那么：

```text
PC = BASE + 4 × 5
```

但是：

```text
Load Page Fault
Illegal Instruction
ECALL
```

仍然：

```text
PC = BASE
```

也就是说：

```text
                 xtvec Vectored

Exception ----------------------> BASE

Interrupt 1 --------------------> BASE + 4

Interrupt 5 --------------------> BASE + 20

Interrupt 9 --------------------> BASE + 36
```

这一点和 Arm Vector Table 的思维很不一样。

官方 `mtvec` 规范明确规定：Vectored 模式下，**同步 Exception 仍进入 BASE，只有异步 Interrupt 才使用 `BASE + 4 × cause`**。:chatgpt-content-reference{index="6"}

---

# 16. Trap 到底进入 M-mode 还是 S-mode？

现在进入这一章最重要的部分之一：

# Trap Routing

默认模型非常简单：

```text
             M-mode
                ▲
                |
             S-mode
                ▲
                |
             U-mode
```

默认：

> **Trap 最终进入 M-mode。**

也就是说：

```text
U exception
     |
     v
M-mode
```

或者：

```text
S exception
     |
     v
M-mode
```

但这样性能很差。

假设 Linux 在 S-mode：

```text
User Page Fault
      |
      v
M-mode firmware
      |
      v
Linux S-mode
```

完全没有必要。

于是 RISC-V 引入：

```text
medeleg

Machine Exception Delegation Register
```

---

# 17. `medeleg` 是干什么的？

假设：

```text
medeleg[13] = 1
```

13：

```text
Load Page Fault
```

表示：

> Load Page Fault 可以直接交给 S-mode。

于是：

```text
U-mode
   |
   | Load Page Fault
   v
S-mode
```

而不是：

```text
U
 |
 v
M
 |
 v
S
```

这就是：

```text
delegation
```

同理：

```text
mideleg
```

控制 Interrupt delegation。

官方 Privileged Architecture 规定：默认 Trap 由 M-mode 处理；如果 `medeleg/mideleg` 对应位设置，则在 S/U-mode 发生的相应 Trap 可以直接进入 S-mode。:chatgpt-content-reference{index="7"}

---

# 18. 一个非常重要的规则：Trap 不能“向下”降低 privilege

假设：

```text
medeleg[2] = 1
```

也就是：

```text
Illegal Instruction
```

delegated 到 S-mode。

如果：

```text
U-mode
```

执行 illegal instruction：

```text
U
 |
 | illegal instruction
 v
S
```

这是：

```text
vertical trap
```

---

如果：

```text
S-mode
```

执行 illegal instruction：

```text
S
 |
 | illegal instruction
 v
S
```

这叫：

```text
horizontal trap
```

---

但如果：

```text
M-mode
```

执行 illegal instruction：

```text
M
 |
 | illegal instruction
 v
M
```

**绝对不会：**

```text
M
 |
 v
S
```

因为：

> Trap never transitions from a more-privileged mode to a less-privileged mode.

官方规范明确规定这一点，同时明确指出 S-mode 自己发生一个已 delegated 的异常时，可以形成 S→S 的 horizontal trap。:chatgpt-content-reference{index="8"}

因此：

```text
              M
             / \
            /   \
           ↑     |
          S -----+
          ↑
          |
          U
```

Trap 可以：

```text
U → S
U → M
S → S
S → M
M → M
```

但不能：

```text
M → S
S → U
```

---

# 19. Vertical Trap 和 Horizontal Trap

这是 RISC-V 官方很有用的一组术语。

## Vertical Trap

发生 privilege change：

```text
U
|
v
S
```

或者：

```text
S
|
v
M
```

叫：

```text
vertical trap
```

---

## Horizontal Trap

privilege 不变：

```text
S
|
v
S
```

或者：

```text
M
|
v
M
```

叫：

```text
horizontal trap
```

官方 Privileged Architecture 正是这样区分 vertical trap 与 horizontal trap。:chatgpt-content-reference{index="9"}

这也是为什么：

> **Trap 不等于 privilege switch。**

这是这一章必须建立的第二个重要认知。

---

# 20. 现在来看一次完整的 Trap Entry

假设：

```text
U-mode application

执行：

ECALL
```

并且：

```text
medeleg[8] = 1
```

所以：

```text
ECALL from U
      ↓
S-mode
```

硬件开始执行 Trap Entry。

---

# 21. 第一步：保存 PC

CPU：

```text
sepc ← current PC
```

例如：

```text
PC = 0x40001000
```

那么：

```text
sepc = 0x40001000
```

---

# 22. 第二步：保存 Cause

因为：

```text
ECALL from U
```

所以：

```text
scause.Interrupt = 0
scause.Code      = 8
```

即：

```text
scause = 8
```

---

# 23. 第三步：保存 Trap Value

对于 ECALL：

```text
stval = 0
```

一般没有额外 fault address。

---

# 24. 第四步：保存 Previous Privilege

CPU 必须知道：

```text
SRET
```

以后应该回到：

```text
U
```

还是：

```text
S
```

于是 `sstatus` 有：

```text
SPP
```

Supervisor Previous Privilege。

编码：

```text
SPP = 0
    ↓
previous = U

SPP = 1
    ↓
previous = S
```

现在从 U-mode 进入：

```text
SPP ← 0
```

---

# 25. 第五步：保存 Interrupt Enable 状态

`sstatus` 还有：

```text
SIE
SPIE
```

分别：

```text
SIE
Supervisor Interrupt Enable

SPIE
Supervisor Previous Interrupt Enable
```

Trap entry：

```text
SPIE ← SIE

SIE ← 0
```

为什么？

因为进入 Trap Handler 的最初阶段：

```text
保存寄存器
建立 stack frame
保存 trap state
```

通常不能立刻被同级 interrupt 打断。

所以硬件自动：

```text
disable S-mode interrupts
```

同时把旧值保存：

```text
SPIE
```

方便 SRET 恢复。

---

# 26. 第六步：跳到 Trap Vector

最后：

```text
PC ← stvec
```

于是：

```text
U-mode:

ECALL
  |
  v
--------------------------------
Hardware Trap Entry
--------------------------------

sepc   ← PC
scause ← 8
stval  ← 0

SPP    ← U
SPIE   ← SIE
SIE    ← 0

privilege ← S

PC ← stvec
--------------------------------
        |
        v
S-mode trap_entry
```

这就是一个完整的：

```text
U → S Trap
```

---

# 27. 用一张图把它记住

```text
        U-mode

        ECALL
          |
          v

===================================
          HARDWARE
===================================

sepc   ← faulting PC

scause ← cause

stval  ← additional information

sstatus.SPP  ← previous privilege

sstatus.SPIE ← sstatus.SIE

sstatus.SIE  ← 0

privilege    ← S

PC           ← stvec

===================================
          SOFTWARE
===================================

        trap_entry
            |
            v
       save registers
            |
            v
       inspect scause
            |
            v
       handle trap
            |
            v
      restore registers
            |
            v
          SRET
```

这张图是整个 Trap Architecture 最值得记住的一张图。

---

# 28. M-mode 完全是同一套模型

S-mode：

```text
stvec
sepc
scause
stval

SIE
SPIE
SPP

SRET
```

M-mode：

```text
mtvec
mepc
mcause
mtval

MIE
MPIE
MPP

MRET
```

因此：

```text
S-mode                  M-mode

stvec                   mtvec

sepc                    mepc

scause                  mcause

stval                   mtval

SPP                     MPP

SIE                     MIE

SPIE                    MPIE

SRET                    MRET
```

官方 Machine Trap architecture 也是按照这一套 CSR 组织，并且当前官方 Privileged Architecture 发布版本为 20260120；Machine ISA 和 Supervisor ISA 都是 Version 1.13。:chatgpt-content-reference{index="10"}

---

# 29. 这里出现一个非常关键的问题：硬件保存 GPR 吗？

答案非常明确：

> **不保存。**

也就是说发生：

```text
Trap
```

之后，CPU 不会自动保存：

```text
ra
sp
gp
tp

t0-t6

a0-a7

s0-s11
```

硬件只保存 architecture-defined Trap state：

```text
xepc
xcause
xtval

xstatus relevant fields
```

然后：

```text
PC ← xtvec
```

所以：

```text
trap_entry
```

必须首先用 assembly 保存软件执行上下文。

---

# 30. 这和 Armv8-A 其实很像

Armv8-A exception entry：

```text
ELR_EL1
SPSR_EL1
ESR_EL1
FAR_EL1
```

由 hardware 保存。

但：

```text
x0-x30
```

并不会全部由硬件自动 push。

kernel exception vector 仍然需要：

```asm
stp x0, x1, [...]
stp x2, x3, [...]
...
```

RISC-V 同样：

```asm
sd ra, ...
sd sp, ...
sd gp, ...
sd tp, ...
sd t0, ...
...
```

所以这一部分你的 Arm 经验可以完全迁移。

---

# 31. 一个典型 Trap Entry 大概是什么样？

简化示意：

```asm
trap_entry:

    addi sp, sp, -TRAPFRAME_SIZE

    sd ra, OFFSET_RA(sp)
    sd gp, OFFSET_GP(sp)
    sd tp, OFFSET_TP(sp)

    sd t0, OFFSET_T0(sp)
    sd t1, OFFSET_T1(sp)
    sd t2, OFFSET_T2(sp)

    sd a0, OFFSET_A0(sp)
    sd a1, OFFSET_A1(sp)

    ...

    csrr t0, sepc
    sd   t0, OFFSET_SEPC(sp)

    csrr a0, scause
    csrr a1, stval

    call trap_handler
```

然后 C：

```c
void trap_handler(unsigned long scause,
                  unsigned long stval)
{
    ...
}
```

这里已经非常接近真实 OS。

---

# 32. 但这里还有一个问题：Trap 发生时 `sp` 是谁的？

假设：

```text
User Application
```

正在运行：

```text
sp = user stack
```

突然：

```text
U → S Trap
```

RISC-V hardware 并不会自动：

```text
sp = kernel stack
```

这意味着：

```text
trap_entry
```

不能假设当前：

```text
sp
```

已经指向 kernel stack。

这是理解真实 RISC-V kernel trap entry 的一个关键点。

---

# 33. `sscratch` 为什么存在？

这就是：

```text
sscratch
```

很重要的一个用途。

例如 kernel 可以提前设置：

```text
sscratch = kernel_stack_pointer
```

Trap entry：

```asm
csrrw sp, sscratch, sp
```

这条指令实现交换：

```text
before:

sp       = user_sp

sscratch = kernel_sp
```

执行：

```asm
csrrw sp, sscratch, sp
```

之后：

```text
sp       = kernel_sp

sscratch = user_sp
```

然后：

```text
trap_entry
```

就可以安全保存寄存器。

这是一个非常漂亮的设计。

注意：

> 真实 Linux、xv6、RTOS 的具体使用方法并不一定完全采用这个简单模板。

例如有的实现让 `sscratch` 保存：

```text
per-hart structure
trapframe pointer
kernel context pointer
```

但设计思想一样：

> **RISC-V 不自动帮软件换 stack，所以提供 scratch CSR 让 trap entry 软件自己解决这一问题。**

---

# 34. 为什么叫 `scratch`？

因为硬件完全不解释里面的内容。

对于：

```text
sscratch
mscratch
```

CPU 只是提供一个 CSR。

你可以存：

```text
stack pointer

per-CPU pointer

trap frame pointer

hart-local data pointer
```

都可以。

所以：

```text
scratch
```

本质就是：

> **给 privilege-level trap handler 留的一个可靠临时锚点。**

---

# 35. Trap Handler 最终怎样返回？

现在来到：

```text
SRET
```

假设当前：

```text
S-mode Trap Handler
```

即将返回。

硬件执行概念上的：

```text
privilege ← SPP

SIE  ← SPIE

SPIE ← 1

SPP  ← 0

PC   ← sepc
```

最关键的是：

```text
PC ← sepc
```

所以：

```text
SRET
```

本质就是：

> **pop Trap privilege/interrupt state + jump back to EPC。**

MRET 同理：

```text
privilege ← MPP

MIE  ← MPIE

PC   ← mepc
```

官方规范明确规定 `xRET` 会恢复相应 privilege/interrupt-enable 状态并把 `pc` 设置为 `xepc`。:chatgpt-content-reference{index="11"}

---

# 36. `xstatus` 为什么看起来像一个两层小栈？

你可以这样理解：

```text
Current state:

Privilege
Interrupt Enable
```

Trap 时：

```text
current
   ↓
previous
```

例如：

```text
SIE
 ↓
SPIE
```

以及：

```text
current privilege
 ↓
SPP
```

于是形成一个非常浅的：

```text
hardware privilege stack
```

概念：

```text
Before Trap

Privilege = U
SIE       = 1

        Trap
          |
          v

During Handler

Privilege = S

SPP  = U

SPIE = 1
SIE  = 0
```

然后：

```text
SRET
```

恢复：

```text
Privilege = U

SIE = 1
```

---

# 37. 为什么只有一层 Previous State？

你可能马上会想到：

> 如果 Trap Handler 又发生 Trap 怎么办？

非常好的问题。

例如：

```text
S-mode trap handler
    |
    | Load Page Fault
    v
S-mode trap handler
```

这会再次写：

```text
sepc
scause
stval
SPP
SPIE
```

所以：

> **hardware CSR 并不是完整的 nested-trap stack。**

如果允许 nested traps：

```text
software
```

必须在合适的时候先把：

```text
sepc
scause
stval
sstatus
```

保存到：

```text
stack / trapframe
```

否则第二次 Trap 会覆盖第一次的信息。

这就是为什么 Trap Entry 最前面的代码属于非常敏感的阶段。

---

# 38. Trap Entry 的危险窗口

想象：

```text
Trap 1
  |
  v
hardware writes:

sepc
scause
stval

  |
  v

trap_entry
  |
  | 尚未保存 sepc/scause/stval
  |
  X
Trap 2
```

第二次 Trap：

```text
overwrite
    ↓
sepc
scause
stval
```

第一次 Trap 的状态可能丢失。

所以设计 robust trap handler 时：

```text
early trap entry
```

非常重要。

当前 RISC-V Privileged Architecture 甚至已经定义 `Ssdbltrp/Smdbltrp` 等 Double Trap 相关扩展，用于更明确地检测这种关键阶段发生的 unexpected trap。:chatgpt-content-reference{index="12"}

对于现在的入门阶段，你只需要先记住：

> **Trap Handler 必须尽早建立自己的软件 Trap Frame。**

---

# 39. 一个完整的 U-mode ECALL 示例

现在把所有东西串起来。

User：

```asm
li a7, SYS_write

ecall

addi a0, a0, 1
```

假设：

```text
medeleg[8] = 1
```

于是：

```text
ECALL from U
      |
      v
S-mode
```

硬件：

```text
sepc   = address(ecall)

scause = 8

stval  = 0

SPP    = U

SPIE   = SIE

SIE    = 0

PC     = stvec
```

然后：

```text
trap_entry
```

保存：

```text
ra
sp
gp
tp
a0-a7
t0-t6
s0-s11
```

接着：

```c
if (scause == 8)
    syscall();
```

完成 syscall：

```text
sepc += 4
```

然后：

```text
restore registers

SRET
```

结果：

```text
                U-mode
                  |
                ECALL
                  |
                  v
 =================================
              HARDWARE
 =================================
 sepc   ← PC
 scause ← 8
 SPP    ← U
 SIE    ← 0
 PC     ← stvec
 =================================
              SOFTWARE
 =================================
 save GPR
                  |
                  v
              syscall
                  |
                  v
             sepc += 4
                  |
                  v
 restore GPR
                  |
                  v
                SRET
 =================================
              HARDWARE
 =================================
 privilege ← U
 PC ← sepc
                  |
                  v
         instruction after ECALL
```

如果你把这个过程真正吃透，Linux syscall 的最低层架构机制也就理解了。

---

# 40. 再看一次 Page Fault

User：

```asm
ld a0, 0(a1)
```

假设：

```text
a1 = 0x40001000
```

但 page：

```text
not present
```

于是：

```text
Load Page Fault
```

hardware：

```text
sepc   = address(ld)

scause = 13

stval  = 0x40001000
```

OS：

```text
page_fault_handler
     |
     v
allocate physical page
     |
     v
update PTE
     |
     v
SFENCE.VMA if required
     |
     v
SRET
```

注意：

```text
sepc 不加 4
```

所以：

```text
ld
```

重新执行。

整个流程：

```text
                LD
                 |
                 v
           address translate
                 |
                 X
            page fault
                 |
                 v
          scause = 13
          stval  = VA
          sepc   = LD
                 |
                 v
            OS handler
                 |
          create mapping
                 |
                 v
               SRET
                 |
                 v
                LD
                 |
                 v
              success
```

这就是 demand paging 的底层 CPU architecture foundation。

---

# 41. Illegal Instruction 又是怎样？

例如：

```text
CPU 没有实现某个 extension
```

却执行相关 instruction。

例如概念上：

```asm
<unsupported instruction>
```

产生：

```text
Illegal Instruction
```

于是：

```text
scause = 2

sepc = faulting instruction
```

某些实现：

```text
stval
```

还可以直接包含：

```text
faulting instruction bits
```

这样 trap handler 就可以：

```text
decode instruction
    |
    v
software emulate
```

然后：

```text
advance sepc
```

继续执行。

这也是：

```text
Invisible Trap
```

的一种典型实现。

---

# 42. RISC-V 官方对 Trap 还有四种软件视角分类

这是一个很有意思、也很有架构味道的分类。

官方把 execution environment 视角下的 Trap 分为：

```text
Contained Trap

Requested Trap

Invisible Trap

Fatal Trap
```

例如：

```text
ECALL

≈ Requested Trap
```

应用明确请求 OS：

```text
帮我做某件事情
```

---

Page Fault：

```text
application
    |
    | load
    v
page absent
    |
    v
OS silently fixes page
    |
    v
resume
```

应用甚至不知道发生过：

```text
Invisible Trap
```

---

Protection violation 导致程序被 kill：

```text
Fatal Trap
```

---

这不是最核心的寄存器知识，但有助于理解：

> Trap architecture 的真正目标不是“处理错误”，而是建立 **Execution Environment ↔ Software** 之间的控制转移机制。:chatgpt-content-reference{index="13"}

这是一个非常重要的架构思想。

---

# 43. Trap 并不意味着发生“错误”

这是最常见的误解之一。

例如：

```text
ECALL
```

不是错误。

Timer Interrupt：

```text
不是错误
```

Demand Page Fault：

```text
从 application 角度也未必是错误
```

所以：

```text
Trap
```

更准确的意思是：

> **CPU 暂停正常 instruction flow，把控制权交给更适合处理该事件的软件环境。**

这也是为什么：

```text
system call
page fault
interrupt
instruction emulation
debug breakpoint
```

可以统一在同一套 Trap Architecture 下面。

---

# 44. 为什么 RISC-V 要用统一 Trap Architecture？

因为无论发生：

```text
System Call

Page Fault

Illegal Instruction

Timer Interrupt

External Interrupt
```

CPU 真正需要做的事情高度相似：

```text
保存 PC

保存 cause

保存 additional info

保存 previous privilege

禁止当前层 interrupt

选择 handler

jump

...

return
```

所以架构可以统一：

```text
xepc

xcause

xtval

xstatus

xtvec

xRET
```

这就是 RISC-V Trap architecture 的核心设计思想。

---

# 45. 和 Armv8-A 做一次完整对应

现在你可以把两边放在一起。

## Armv8-A

```text
User EL0
   |
   | SVC
   v
EL1 exception entry
   |
   +--> ELR_EL1
   |
   +--> ESR_EL1
   |
   +--> SPSR_EL1
   |
   +--> FAR_EL1 when relevant
   |
   v
VBAR_EL1
   |
   v
exception handler
   |
   v
ERET
```

## RISC-V

```text
User U-mode
   |
   | ECALL
   v
S-mode trap entry
   |
   +--> sepc
   |
   +--> scause
   |
   +--> sstatus
   |
   +--> stval when relevant
   |
   v
stvec
   |
   v
trap handler
   |
   v
SRET
```

所以：

```text
ELR_EL1
      ≈
sepc
```

```text
ESR_EL1
      ≈
scause
```

```text
FAR_EL1
      ≈
stval
```

```text
VBAR_EL1
      ≈
stvec
```

```text
SPSR_EL1
      ≈
sstatus 的 previous-state 相关部分
```

```text
ERET
      ≈
SRET
```

这是你可以最大程度复用 Armv8-A 经验的部分。

---

# 46. 但 Arm ESR 与 RISC-V scause 并不是完全一样

Arm：

```text
ESR_EL1
```

里面通常有：

```text
EC
Exception Class

ISS
Instruction Specific Syndrome
```

包含的信息相对丰富。

例如：

```text
Data Abort
```

ESR 可以提供：

```text
fault class
access type
syndrome
```

再配合：

```text
FAR_EL1
```

分析问题。

而 RISC-V 更倾向：

```text
scause
    ↓
粗粒度 cause

stval
    ↓
相关地址/额外值
```

复杂信息尽可能：

```text
由 software decode
```

这也是 RISC-V 整体设计风格的一种体现：

> **hardware mechanism 尽量简洁，把 policy 和复杂 decode 留给 software。**

---

# 47. RISC-V 为什么 Exception 和 Interrupt 使用同一个 `xcause`？

现在这个问题应该非常自然了。

因为：

```text
Exception
Interrupt
```

虽然来源不同：

```text
sync
async
```

但进入 Trap 以后：

```text
save state

jump handler

return
```

完全使用相同机制。

因此：

```text
xcause
```

最高 bit：

```text
Interrupt?
```

其余：

```text
cause
```

是一种非常紧凑的 encoding。

RV64 下甚至可以：

```asm
bltz a0, interrupt_handler
```

如果：

```text
a0 = scause
```

因为 Interrupt bit 就是最高 bit。

所以：

```text
scause < 0
```

从 signed interpretation 来看可以直接判断：

```text
Interrupt
```

官方规范也特别指出了这一技巧。:chatgpt-content-reference{index="14"}

---

# 48. 一个简单 Trap Dispatcher

于是 OS 中可以出现类似：

```c
void trap_handler(void)
{
    unsigned long cause = read_csr(scause);
    unsigned long epc   = read_csr(sepc);
    unsigned long tval  = read_csr(stval);

    if (cause >> 63)
    {
        handle_interrupt(cause);
    }
    else
    {
        handle_exception(cause, epc, tval);
    }
}
```

Exception：

```c
switch (cause)
{
case 8:
    handle_syscall();
    break;

case 12:
    handle_instruction_page_fault();
    break;

case 13:
    handle_load_page_fault();
    break;

case 15:
    handle_store_page_fault();
    break;

case 2:
    handle_illegal_instruction();
    break;
}
```

现在应该已经非常容易读懂。

---

# 49. Trap Routing 再用一张总图整理

假设系统支持：

```text
M
S
U
```

那么：

```text
                          M-mode
                        trap handler
                             ▲
                             |
             not delegated   |
                             |
                +------------+
                |
                |
             S-mode
           trap handler
              ▲   |
              |   |
    delegated |   | horizontal trap
              |   |
              |   v
             S-mode
                ▲
                |
       delegated|
                |
              U-mode
```

于是你需要先问：

```text
1. 当前在哪个 privilege？

2. 发生什么 cause？

3. medeleg/mideleg 对应 bit 是多少？

4. 最终 target privilege 是谁？
```

然后才能决定使用：

```text
mtvec / mepc / mcause / mtval
```

还是：

```text
stvec / sepc / scause / stval
```

---

# 50. 一个特别重要的例子：Linux 为什么能自己处理 Page Fault？

因为 OpenSBI/M-mode firmware 可以：

```text
medeleg[12] = 1
medeleg[13] = 1
medeleg[15] = 1
```

分别 delegate：

```text
Instruction Page Fault

Load Page Fault

Store Page Fault
```

于是：

```text
Linux user process
      |
      | page fault
      v
Linux S-mode
```

直接处理。

而不是：

```text
Linux user
    |
    v
OpenSBI M-mode
    |
    v
Linux S-mode
```

所以：

> `medeleg/mideleg` 本质上也是在定义 **M-mode firmware 与 S-mode OS 的责任边界**。

从 system architecture 的角度理解，比把它当成几个 CSR bit 更重要。

---

# 51. 这和 Arm 最大的设计差异之一

Arm 通常通过：

```text
exception routing rules

SCR_EL3
HCR_EL2

PSTATE
DAIF

VBAR_ELx
```

决定异常最终进入：

```text
EL1
EL2
EL3
```

RISC-V 则非常显式：

```text
medeleg
mideleg
```

告诉 hardware：

```text
这个 Exception → S

那个 Exception → M
```

所以 RISC-V 的思维是：

```text
Cause-based delegation
```

这个概念一定要建立。

---

# 52. Trap Entry 到底改变了什么、不改变什么？

这个问题对于 Staff-level 理解非常重要。

发生一个普通 Trap：

硬件主要处理：

```text
PC

Privilege

Trap CSRs

Interrupt Enable state
```

但通常**不会自动替你完成**：

```text
保存全部 GPR

切 kernel stack

创建 software trap frame

切换 satp

切 kernel page table

调度 task

处理 signal

处理 syscall
```

这些都是：

```text
software
```

的责任。

因此不要把：

```text
CPU Trap Entry
```

和：

```text
OS Exception Handling
```

混为一谈。

正确分层：

```text
        Hardware

Exception detected
      ↓
Trap routing
      ↓
write xepc/xcause/xtval
      ↓
update xstatus
      ↓
PC ← xtvec

----------------------------

        Software

assembly trap entry
      ↓
save GPR
      ↓
establish kernel context
      ↓
C handler
      ↓
OS policy
      ↓
restore
      ↓
xRET
```

这一条边界非常重要。

---

# 53. 为什么 Trap Handler 的第一段通常一定是 Assembly？

因为刚进入：

```text
xtvec
```

的时候：

```text
stack 可能不是 kernel stack

GPR 尚未保存

C ABI 尚未满足

tp/gp 等环境可能尚未准备好
```

直接：

```c
trap_handler();
```

是不安全的。

所以通常：

```text
xtvec
   |
   v
assembly entry
   |
   +-- switch stack/context
   |
   +-- save registers
   |
   +-- build trap frame
   |
   v
C handler
```

这与你熟悉的：

```text
Arm exception vector assembly
```

完全相同。

---

# 54. Trap Frame 是 architecture 定义的吗？

不是。

Hardware architecture 只定义：

```text
xepc
xcause
xtval
xstatus
...
```

但：

```text
struct trapframe
```

属于 OS software design。

例如可以设计：

```c
struct trapframe
{
    uint64_t ra;
    uint64_t sp;
    uint64_t gp;
    uint64_t tp;

    uint64_t t0;
    uint64_t t1;
    uint64_t t2;

    uint64_t s0;
    uint64_t s1;

    uint64_t a0;
    uint64_t a1;
    ...
    uint64_t a7;

    uint64_t sepc;
    uint64_t sstatus;
};
```

Linux、NuttX、xv6、RTOS：

```text
layout
```

都可以不同。

所以：

```text
Trap State
```

分两层：

```text
Hardware Trap State
        +
Software Trap Frame
```

---

# 55. Exception Priority

还有一个容易遗漏的问题。

假设一条 instruction：

```text
理论上同时违反多个条件
```

例如可能：

```text
misaligned

page translation failure

access permission failure
```

CPU 最终只能在：

```text
xcause
```

报告一个 cause。

因此 architecture 需要定义：

```text
exception priority
```

Privileged Architecture 对同步异常定义了 priority ordering；具体实现和某些 memory-access 场景还存在架构允许的差异，因此写 kernel 时不应该简单假定“所有检查总是按照你直觉中的固定软件顺序发生”。:chatgpt-content-reference{index="15"}

这一点现在不需要背表。

等我们学习：

```text
Sv39
PMP
```

时再结合具体 page/access fault 展开。

---

# 56. Exception 通常是 Precise 的是什么意思？

简单理解：

```text
Instruction N-1
    已经完成

Instruction N
    发生 Exception

Instruction N+1
    尚未产生 architectural effect
```

于是：

```text
xepc = Instruction N
```

OS 可以准确知道：

```text
哪条 instruction 出错
```

这就是：

```text
Precise Exception
```

这是：

```text
Page Fault
Illegal Instruction
ECALL
```

能够可靠恢复的重要基础。

RISC-V 的规范更严格地把“Trap 是否 precise”放在 execution environment 语义下描述；对于普通 OS 软件，我们通常可以把主要同步 exception 按这种 precise trap mental model 理解，但不要把它推广成“所有可能的错误事件都绝对 precise”。:chatgpt-content-reference{index="16"}

---

# 57. NMI 为什么是一个特殊问题？

普通 Trap：

```text
xepc
xcause
```

只有一份。

假设已经：

```text
M-mode trap handler
```

又突然收到不可屏蔽的 NMI。

传统基础模型可能导致：

```text
mepc
mcause
```

被覆盖。

因此 current RISC-V Privileged Architecture 还定义了：

```text
Smrnmi
```

Resumable NMI extension。

它增加：

```text
mnepc
mncause
mnstatus
mnscratch

MNRET
```

专门保存 RNMI state。:chatgpt-content-reference{index="17"}

这现在属于高级内容。

但它再次证明：

> **RISC-V Trap CSR 本身不是任意深度的硬件 exception stack。**

---

# 58. 现在回头理解 `medeleg` 和 `mideleg` 的名字

这一章之后应该很好理解：

```text
medeleg

Machine
Exception
Delegation
```

意思：

```text
M-mode

允许某些同步 Exception

交给 S-mode
```

而：

```text
mideleg

Machine
Interrupt
Delegation
```

就是：

```text
M-mode

允许某些 Interrupt

交给 S-mode
```

所以 RISC-V 没有混成：

```text
mtrapdeleg
```

是因为 architecture 从源头就明确区分：

```text
Exception
Interrupt
```

只是最后都通过：

```text
Trap
```

完成 control transfer。

---

# 59. 这一章最容易出现的 8 个误解

### 误解 1

```text
Trap == Exception
```

错误。

正确：

```text
Exception ─┐
           ├→ Trap
Interrupt ─┘
```

---

### 误解 2

```text
Trap 一定提升 privilege
```

错误。

可能：

```text
S → S
M → M
```

即：

```text
horizontal trap
```

---

### 误解 3

```text
发生 Trap 后 hardware 自动保存所有寄存器
```

错误。

Hardware 主要保存：

```text
xepc
xcause
xtval
xstatus state
```

GPR：

```text
software 保存
```

---

### 误解 4

```text
Trap 进入 S-mode 后 hardware 自动切 kernel stack
```

错误。

需要 software 利用：

```text
sscratch
```

或其他方式建立 kernel context。

---

### 误解 5

```text
Vectored mode 下所有 Exception 都有独立 vector
```

错误。

```text
Exception → BASE

Interrupt → BASE + 4*cause
```

---

### 误解 6

```text
sepc 永远返回下一条 instruction
```

错误。

它通常记录：

```text
被 interrupt 的 instruction

或

发生 exception 的 instruction
```

handler 决定：

```text
retry

还是

advance
```

---

### 误解 7

```text
medeleg[2]=1 后 M-mode Illegal Instruction 会进入 S-mode
```

错误。

```text
M → S
```

这种向下 Trap 不允许。

---

### 误解 8

```text
Page Fault 就是 Access Fault
```

错误。

粗略：

```text
Page Fault
    → virtual-memory translation / permission

Access Fault
    → physical-access / protection/system side
```

---

# 60. 如果把整章压缩成一张图

建议最终把下面这张图记在脑子里：

```text
                  Running Instruction
                         |
           +-------------+-------------+
           |                           |
       Exception                    Interrupt
      synchronous                 asynchronous
           |                           |
           +-------------+-------------+
                         |
                         v
                       TRAP
                         |
                         v
                 Trap Delegation
                 medeleg/mideleg
                         |
               +---------+---------+
               |                   |
               v                   v
            M-mode              S-mode
               |                   |
        +------+------+     +------+------+
        |      |      |     |      |      |
      mepc  mcause  mtval  sepc  scause  stval
        |                   |
        v                   v
      mtvec                stvec
        |                   |
        v                   v
     Assembly             Assembly
     trap_entry           trap_entry
        |                   |
        +---------+---------+
                  |
                  v
          Save software context
                  |
                  v
             Trap Handler
                  |
       +----------+-----------+
       |          |           |
     syscall   page fault   interrupt
       |          |           |
       +----------+-----------+
                  |
                  v
          Restore context
                  |
              xRET
                  |
                  v
          Resume execution
```

---

# 61. 从 Staff Engineer 角度，真正应该理解到什么程度？

如果是普通 RISC-V developer：

```text
会使用：

scause
sepc
stval
stvec
sret
```

基本够用。

如果要达到：

```text
Kernel / BSP / Firmware
Senior
```

应该理解：

```text
Trap entry sequence

software context save

sscratch usage

delegation

vertical/horizontal traps

page fault retry semantics

interrupt state stacking
```

如果目标是：

```text
Staff / System Architect
```

则需要继续理解：

```text
Trap ownership between M/S

delegation policy

nested trap

double trap

virtualization trap routing

M → HS → VS

guest page fault

mtval2 / mtinst

NMI

SBI interaction

security implications
```

后面的 H Extension 我们还会再把这一模型扩展：

```text
M
|
HS
|
VS
|
VU
```

并增加：

```text
hedeleg
hideleg
```

官方 H Extension 的 Trap Routing 正是：

```text
VS/VU Trap
   ↓
M

如果 medeleg/mideleg
   ↓
HS

如果进一步 hedeleg/hideleg
   ↓
VS
```

因此今天学习的 M/S/U Trap 模型其实就是未来理解虚拟化的基础。:chatgpt-content-reference{index="18"}

---

# 62. 最后，把整个 Trap Architecture 压缩成六句话

第一：

```text
Exception = synchronous
Interrupt = asynchronous
Trap = control transfer
```

第二：

```text
xepc
    = where

xcause
    = why

xtval
    = additional information
```

第三：

```text
xtvec
    = where handler starts
```

第四：

```text
medeleg/mideleg
    = who handles the Trap
```

第五：

```text
Hardware saves minimal architectural state;
software saves GPR / stack / trapframe.
```

第六：

```text
xRET
    = restore privilege/interrupt state
      +
      PC ← xepc
```

如果这六句话真正理解了，RISC-V Trap Architecture 的骨架就已经完全建立起来了。

---

# 63. 下一章之前，你脑中最好已经形成这个执行过程

看到：

```asm
ecall
```

不要只想到：

```text
system call
```

而应该自动展开：

```text
ECALL
 |
 v
Exception
 |
 v
cause = Environment Call
 |
 v
check delegation
 |
 v
target privilege
 |
 v
write xepc
 |
 v
write xcause
 |
 v
write xtval
 |
 v
update xstatus
 |
 v
disable target-level interrupt
 |
 v
PC ← xtvec
 |
 v
assembly trap entry
 |
 v
save GPR
 |
 v
software handler
 |
 v
modify xepc if necessary
 |
 v
restore GPR
 |
 v
xRET
 |
 v
resume
```

一旦这条链已经可以在脑中自动运行，**03 Trap / Exception 就算真正掌握了。**

接下来进入 **04 Interrupt** 时，我们只是在这套已经建立好的 Trap machinery 前面增加：

```text
Interrupt Source
      ↓
pending
      ↓
enable
      ↓
priority
      ↓
delegation
      ↓
Trap
```

也就是说，下一章我们不需要重新学习“如何进入 handler”，只需要回答一个新的核心问题：

> **一个异步 Interrupt 到底如何从 device/timer/software source，一路变成 `xip/xie → xcause → xtvec`，最后让 CPU 真正发生 Trap？**

我建议你这一章重点反复消化 **第 16～38 节**：`delegation → hardware trap entry → xstatus → software context save → xRET`。这部分是真正的架构核心；后面的 Interrupt、Sv39 Page Fault、OpenSBI 和 H Extension 都是在它上面继续搭建。
