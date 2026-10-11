# RISC-V Privileged Architecture 01：M / S / U Privilege

## 0. 先给这一章最核心的结论

理解 RISC-V 的 M/S/U，建议先记住下面这个模型：

```text
                 Higher privilege
                       ↑

                M-mode
        Machine / firmware layer
           OpenSBI / low-level FW
                       |
              PMP / delegation
                       |
                S-mode
             OS kernel layer
             Linux / NuttX
                       |
              MMU / page table
                       |
                U-mode
          application / process

                       ↓
                 Lower privilege
```

它的本质并不是简单地把 CPU 分成三个“运行模式”，而是构造一个**分层的控制权模型**：

```text
M controls S
S controls U
```

典型职责可以先记成：

```text
M-mode：控制 machine
S-mode：控制 operating system
U-mode：运行 application
```

官方 Privileged Architecture 规定：**M-mode 是唯一强制存在的 privilege mode，也是最高 privilege；U 和 S 根据实现选择。** 标准化的组合主要是 `M`、`M+U`、`M+S+U`。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/priv-intro.html?utm_source=chatgpt.com)

但更重要的是第二个结论：

> **Privilege mode 只决定“当前代码允许做什么”，它本身并不等于完整的内存隔离机制。**

真正构成典型 Linux 系统安全边界的是：

```text
                   privilege
                       +
      ┌─────────────────────────────┐
      │                             │
      ▼                             ▼
     PMP                         MMU
 M controls S/U             S controls U
 physical memory           virtual memory
```

所以后面我们会得到一个非常漂亮的层次关系：

```text
M-mode
   |
   | PMP
   | 规定 S-mode 能碰哪些 physical memory
   v
S-mode
   |
   | page table / Sv39
   | 规定 U-mode 能碰哪些 virtual memory
   v
U-mode
```

这张图是理解整个 RISC-V privileged architecture 的基础。

---

# 1. 为什么 CPU 需要 privilege mode？

先不要考虑 RISC-V。

假设一个 CPU 只有一种执行模式：

```text
Application
    |
    v
CPU
```

那么 application 可以：

```text
修改 interrupt controller
修改 page table
访问任意 physical memory
关闭 interrupt
控制 timer
访问其他 process memory
修改 kernel
```

也就是说：

```text
App A
App B
Kernel
Device
Memory
```

之间完全没有可靠边界。

因此现代 CPU 引入：

> **Privilege separation**

基本思想非常简单：

```text
普通程序：
    权限少

操作系统：
    权限更多

底层 firmware：
    权限最高
```

RISC-V 将其抽象成：

```text
Privilege Level

0  U
1  S
2  Reserved
3  M
```

官方定义的编码也是：

| Encoding | Mode |
|---|---|
| `00` | U |
| `01` | S |
| `10` | Reserved |
| `11` | M |

也就是：

```text
00 U
01 S
10 reserved
11 M
```

从编码本身你甚至都可以看出 hierarchy：

```text
00 < 01 < 11
 U     S     M
```

RISC-V privilege level 的主要目的就是在软件栈不同组件之间提供 protection；当前 mode 执行不允许的操作时，会产生 exception/trap。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/priv-intro.html?utm_source=chatgpt.com)

---

# 2. M / S / U 分别是什么？

先建立最简单的认识。

## 2.1 U-mode：Application

U = **User / Application**

典型运行：

```text
shell
browser
server
application
userspace runtime
```

比如 Linux：

```text
main()
{
    printf("hello");
}
```

这段程序绝大多数时间就在：

```text
U-mode
```

运行。

它不能直接：

```text
修改 satp
修改 page table
关闭 supervisor interrupt
访问 machine CSR
修改 PMP
控制 machine-level interrupt
```

需要 OS 服务时，例如：

```c
write(fd, buf, len);
```

最终执行：

```asm
ecall
```

产生 trap：

```text
U
|
| ECALL
v
S
```

进入 kernel。

所以可以把 U-mode 理解成：

> **被 OS 管理的 execution domain。**

---

# 3. S-mode：Operating System Kernel

S = **Supervisor**

最典型的软件就是：

```text
Linux kernel
```

所以刚开始可以直接建立：

```text
S-mode ≈ Arm EL1
```

S-mode 可以完成 U-mode 做不到的事情，例如：

```text
管理 page table
修改 satp

处理中断

处理 syscall

管理 process

context switch

执行 SFENCE.VMA

访问 supervisor CSR
```

例如：

```text
U-mode
process A
   |
   | syscall
   v
S-mode
Linux kernel
   |
   | schedule()
   v
U-mode
process B
```

这和 Arm：

```text
EL0
 |
 | SVC
 v
EL1
```

在 conceptual level 上非常接近。

官方特权规范特别强调一点：

> S-mode 被有意限制了直接操纵底层 physical hardware 的能力，这有利于让 S-mode OS 与底层 execution environment 解耦，也使 virtualization 更干净。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/supervisor.html?utm_source=chatgpt.com)

这一点非常重要。

RISC-V 并不是设计成：

```text
S-mode = everything except M-mode
```

而是希望：

```text
                 S-mode OS
                     |
          standardized interface
                     |
                    SBI
                     |
              Execution Environment
```

例如：

```text
Linux
  S-mode
    |
   SBI
    |
 OpenSBI
  M-mode
```

后面学习 OpenSBI 时，这一点会非常重要。

---

# 4. M-mode：Machine

M = **Machine**

这是 RISC-V 最重要的 privilege mode。

官方定义：

> M-mode 是最高 privilege mode，也是所有 RISC-V hardware implementation 唯一强制要求存在的 privilege mode。Reset 后 hart 首先进入 M-mode。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/machine.html?utm_source=chatgpt.com)

典型代码是：

```text
Boot firmware
OpenSBI
low-level platform firmware
machine monitor
```

它负责管理真正的 machine-level resource。

例如：

```text
PMP
trap delegation
machine interrupts
machine timer
hart startup
platform firmware
```

所以常见 RISC-V Linux 系统：

```text
Reset
  |
  v
M-mode firmware
OpenSBI
  |
  v
S-mode Linux
  |
  v
U-mode application
```

---

# 5. 和 Armv8-A 最重要的对应关系

结合你的经验，可以先这样建立 mapping：

| RISC-V | Armv8-A | 典型软件 |
|---|---|---|
| U | EL0 | application |
| S | EL1 | Linux kernel |
| M | **粗略类似 EL3** | firmware / OpenSBI |

所以：

```text
RISC-V                     Arm

M                           EL3
|                            |
S                           EL1
|                            |
U                           EL0
```

但这里有一个非常重要的警告：

> **M-mode ≠ EL3。**

只能在：

```text
highest privilege firmware
```

这一层面类比。

因为 Arm：

```text
EL3
 |
 +-- Secure state
 |
 +-- Non-secure state
```

背后还有 TrustZone：

```text
SCR_EL3
Secure World
Normal World
```

而 RISC-V：

```text
M-mode
```

**本身没有 Secure / Non-secure World 这个含义。**

所以：

```text
M-mode ≈ EL3 privilege position
```

可以。

但是：

```text
M-mode == TrustZone Secure World
```

是错误的。

这一点以后学习 RISC-V TEE 时极其重要。

---

# 6. 那么 Arm EL2 去哪里了？

你很容易产生这个疑问：

```text
Arm:

EL3
EL2
EL1
EL0
```

而：

```text
RISC-V:

M
S
U
```

怎么少了一层？

因为 virtualization 不是基础 M/S/U 模型的一部分。

它由：

```text
H Extension
```

扩展。

支持 H extension 后：

```text
M
 |
HS        ← Hypervisor
 |
VS        ← Guest kernel
 |
VU        ← Guest application
```

大致对应：

```text
RISC-V               Arm

M                     EL3

HS                    EL2

VS                    Guest EL1

VU                    Guest EL0
```

官方 H extension 用额外的 virtualization mode `V` 将 nominal S/U 扩展为 HS/VS/VU。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

我们现在这一章暂时完全忽略 H：

```text
M
S
U
```

先学透。

---

# 7. M/S/U 究竟限制了什么？

这是理解 privilege 的核心。

可以把限制分成三类：

```text
              Privilege checking
                     |
       +-------------+-------------+
       |             |             |
     CSR        instruction       memory
    access          access         access
```

但三者实现机制不完全相同。

---

# 8. 第一类：限制 CSR

例如：

```text
mstatus
mepc
mcause
medeleg
pmpcfg
```

这些属于 machine-level state。

U-mode 不能随便：

```asm
csrw mstatus, a0
```

S-mode 也不能随便：

```asm
csrw mstatus, a0
```

而 M-mode 可以。

CSR 本身具有 privilege accessibility。

而且 RISC-V 的 CSR encoding 非常漂亮：

```text
csr[9:8]
```

本身就编码最低 privilege requirement。

更高 privilege 通常可以访问低 privilege CSR：

```text
M
|
+--> M CSR
+--> S CSR
+--> U CSR

S
|
+--> S CSR
+--> U CSR

U
|
+--> U CSR
```

官方规范明确规定：与某 privilege level 关联的 CSR，也可以被所有更高 privilege level 访问；非法 privilege 的 CSR access 会引发 illegal-instruction exception。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/priv-csrs.html?utm_source=chatgpt.com)

于是：

```text
U:
csrw mstatus
        |
        X
        |
illegal instruction
```

这本身就是 privilege isolation。

---

# 9. 第二类：限制 privileged instruction

例如：

```text
MRET
SRET
SFENCE.VMA
WFI
...
```

不同 instruction 有不同 privilege requirement。

假设 U-mode 执行某条只允许 S-mode 的 privileged instruction：

```text
U-mode
   |
   | illegal privileged instruction
   v
Exception
   |
   v
Trap handler
```

CPU 不会“试着帮你执行”。

而是：

> **Trap。**

这也是 RISC-V privileged architecture 一个非常核心的思想：

```text
allowed
   ↓
execute normally


not allowed
   ↓
trap
   ↓
higher privileged software handles it
```

这套设计以后会自然延伸到 virtualization：

```text
Guest executes privileged operation
        |
        v
      trap
        |
        v
Hypervisor emulate
```

---

# 10. 第三类：Memory Access

这里一定要特别注意：

> **M/S/U 并不会天然自动产生完整 memory isolation。**

例如：

```text
M
S
U
```

只是 CPU privilege hierarchy。

真正决定：

```text
S 能不能访问某 physical memory
```

通常是：

```text
PMP
```

而决定：

```text
U process 能不能访问 kernel memory
```

通常是：

```text
MMU
+
Page Table
```

因此完整系统更像：

```text
                       M
                       |
              configure PMP
                       |
       +---------------+---------------+
       |                               |
 M firmware memory                S accessible
 protected                             RAM
                                       |
                                      S
                                       |
                              configure page table
                                       |
                         +-------------+-------------+
                         |                           |
                     Kernel VA                  User VA
                                                     |
                                                     U
```

PMP 会检查有效 privilege 为 S/U 的 physical accesses；它由 machine-level CSR 配置。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/machine.html?utm_source=chatgpt.com)

所以：

```text
Privilege mode
```

和：

```text
Memory protection
```

一定要分开理解。

---

# 11. M → S → U 的“管理关系”

现在我们可以得到一个非常重要的 hierarchical model：

```text
┌─────────────────────────────────┐
│              M                  │
│                                 │
│  Machine Resource Owner         │
│                                 │
│  PMP                            │
│  trap delegation                │
│  machine interrupt              │
│  SBI implementation             │
└──────────────┬──────────────────┘
               |
               v
┌─────────────────────────────────┐
│              S                  │
│                                 │
│  OS Resource Owner              │
│                                 │
│  page tables                    │
│  process management             │
│  syscall                        │
│  scheduling                     │
└──────────────┬──────────────────┘
               |
               v
┌─────────────────────────────────┐
│              U                  │
│                                 │
│  Application                    │
│                                 │
│  normal computation             │
│  syscall                        │
└─────────────────────────────────┘
```

换句话说：

```text
M defines the environment in which S runs.

S defines the environment in which U runs.
```

这个思想非常值得记住。

---

# 12. CPU 怎么从 U 进入 S？

现在开始进入 privilege transition。

最常见场景：

```text
U-mode application
        |
        | ecall
        v
      Trap
        |
        v
S-mode kernel
```

例如 Linux syscall：

```c
write(fd, buf, size);
```

最终：

```asm
ecall
```

`ECALL` 本身的语义是：

> 请求当前 execution environment 提供服务，并产生一个精确的 environment-call exception。[RISC-V Documentation](https://docs.riscv.org/reference/isa/v20260120/unpriv/rv32.html?utm_source=chatgpt.com)

于是：

```text
U
|
| ECALL
v
S
```

但是这里马上出现一个非常重要的问题：

> **CPU 怎么知道这个 exception 应该去 S，而不是 M？**

答案就是：

```text
delegation
```

这也是下一章 Trap architecture 的核心内容。

---

# 13. 默认情况下 Trap 去哪里？

这是理解 RISC-V privileged architecture 最关键的一条规则之一：

> **默认所有 trap 都进入 M-mode。**

例如：

```text
U-mode
   |
   | exception
   v
M-mode
```

或者：

```text
S-mode
   |
   | interrupt
   v
M-mode
```

官方定义默认 trap 都由 M-mode 处理；M-mode 可以通过 `medeleg` / `mideleg` 把部分 exception/interrupt delegation 给 S-mode。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/machine.html?utm_source=chatgpt.com)

因此真正模型是：

```text
               M
               ^
               |
          default trap
               |
          +----+----+
          |         |
          S         U
```

而不是：

```text
U automatically → S
S automatically → M
```

这是 RISC-V 和很多人的第一直觉不一样的地方。

---

# 14. Trap Delegation

M-mode 可以说：

> “这种 exception 我不处理，交给 S。”

通过：

```text
medeleg
```

Machine Exception Delegation Register。

中断则是：

```text
mideleg
```

Machine Interrupt Delegation Register。

例如：

```text
M-mode firmware:

User ECALL → delegate to S
Page fault → delegate to S
```

那么：

```text
U
|
| syscall
v
S
```

而不必：

```text
U
|
v
M
|
v
S
```

这既减少 latency，也让 OS 直接管理自己应该管理的事件。

---

# 15. 一个很重要的 delegation 规则

假设：

```text
medeleg:
illegal instruction → S
```

这是什么意思？

它意味着：

```text
U illegal instruction
          |
          v
          S
```

或者：

```text
S illegal instruction
          |
          v
          S
```

但是如果：

```text
M itself executes illegal instruction
```

不会：

```text
M
↓
S
```

而仍然：

```text
M
→ M trap handler
```

官方明确规定：

> Trap 不会因为 delegation 从高 privilege 降到低 privilege。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/machine.html?utm_source=chatgpt.com)

所以 delegation 的真正含义不是：

```text
所有这种 trap 都交给 S
```

而是：

```text
如果 trap 来自 S/U，
可以让它直接进入 S。
```

这一点非常重要。

---

# 16. Vertical Trap 和 Horizontal Trap

RISC-V 官方术语里有两个很有用的概念。

### Vertical trap

Privilege 升高：

```text
U
|
| trap
v
S
```

或者：

```text
S
|
| trap
v
M
```

这叫：

```text
vertical trap
```

### Horizontal trap

Privilege 不变：

```text
S
|
| trap
v
S
```

例如：

```text
S-mode page fault
    ↓
S-mode trap handler
```

这叫：

```text
horizontal trap
```

官方规范就是这样定义 vertical / horizontal trap 的。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/priv-intro.html?utm_source=chatgpt.com)

这个 terminology 后面读 specification 会经常看到。

---

# 17. Trap 发生时 CPU 做什么？

这里暂时只讲核心思想。

假设：

```text
U
|
| ECALL
v
S
```

CPU 至少需要知道：

```text
发生了什么？

从哪里发生？

trap handler 在哪里？

之前是什么 privilege？
```

所以 S-mode 有：

```text
scause
sepc
stvec
sstatus.SPP
```

对应：

```text
scause
  ↓
为什么发生 trap


sepc
  ↓
从哪条 instruction 发生


stvec
  ↓
trap handler 在哪里


SPP
  ↓
之前是什么 privilege
```

这和你的 Arm 经验可以直接对应：

| Arm | RISC-V |
|---|---|
| ESR_EL1 | `scause` |
| ELR_EL1 | `sepc` |
| VBAR_EL1 | `stvec` |
| SPSR_EL1 部分职责 | `sstatus` |

---

# 18. Trap 时 privilege 如何保存？

假设当前：

```text
U
```

发生 trap：

```text
U
↓
S
```

硬件需要记住：

```text
之前是 U
```

因此：

```text
sstatus.SPP = U
```

SPP：

```text
Supervisor Previous Privilege
```

它只有 1 bit，因为从 S trap handler 返回时，目标只需要区分：

```text
U
or
S
```

所以：

```text
SPP = 0 → U
SPP = 1 → S
```

官方规定，trap 进入 S-mode 时 SPP 保存此前的 privilege；执行 `SRET` 后根据 SPP 决定返回 U 还是 S。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/supervisor.html?utm_source=chatgpt.com)

---

# 19. M-mode 为什么需要 MPP？

M-mode 更复杂。

因为进入 M-mode 之前可能是：

```text
M
S
U
```

所以需要：

```text
mstatus.MPP
```

MPP：

```text
Machine Previous Privilege
```

需要两位。

例如：

```text
before trap:

S

↓ trap

M
```

CPU：

```text
MPP = S
```

于是：

```text
MRET
```

知道：

```text
return to S
```

这个机制可以画成：

```text
S
|
| trap
v
M

hardware:

mepc = old PC
MPP  = S
```

处理完：

```text
MRET
 |
 v
S
```

---

# 20. xRET 是 privilege return 的核心机制

RISC-V 有：

```text
MRET
SRET
```

分别：

```text
MRET
Machine Trap Return

SRET
Supervisor Trap Return
```

核心逻辑可以简单理解为：

```text
MRET:

PC ← mepc
Privilege ← MPP
```

而：

```text
SRET:

PC ← sepc
Privilege ← SPP
```

当然真实 architecture 还会恢复 interrupt-enable stack 等状态；我们到 Trap 章节再展开。

官方规范规定 `MRET` 总是存在；支持 S-mode 时必须支持 `SRET`。`xRET` 同时恢复对应的 previous-privilege/interrupt state，并将 PC 设置为 `xepc`。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/machine.html?utm_source=chatgpt.com)

---

# 21. 一个特别重要的问题：CPU 如何主动从 M 降到 S？

比如刚上电：

```text
Reset
 ↓
M-mode
```

但是我们想运行：

```text
Linux in S-mode
```

怎么进入？

RISC-V 通常不会设计一个：

```text
switch_to_supervisor
```

这样的普通 instruction。

而是巧妙复用 trap return 机制。

M-mode firmware：

```text
mepc = Linux_entry
MPP  = S
```

然后：

```asm
mret
```

于是：

```text
M-mode
   |
   | MPP=S
   | mepc=Linux_entry
   |
   | MRET
   v
S-mode
Linux_entry
```

这是非常典型的 RISC-V 编程模式。

伪代码：

```asm
    # Linux kernel entry
    la      t0, linux_entry
    csrw    mepc, t0

    # mstatus.MPP = S
    csrr    t0, mstatus
    ...
    csrw    mstatus, t0

    mret
```

效果：

```text
current privilege = M

mret

current privilege = S
PC = linux_entry
```

---

# 22. S-mode 如何启动 U-mode application？

完全一样。

Linux kernel：

```text
sepc = user_pc
SPP  = U
```

然后：

```asm
sret
```

于是：

```text
S-mode
Linux
 |
 | SPP=U
 | sepc=user_pc
 |
 | SRET
 v
U-mode
application
```

所以你会发现 RISC-V 的设计非常统一：

```text
trap entry:
    lower → higher

xRET:
    higher → previous
```

并且 OS/firmware 可以利用：

```text
xPP + xepc + xRET
```

主动进入 lower privilege。

---

# 23. 这和 Arm 的异常进入/返回极其相似

Arm：

```text
EL0
 |
 | exception
 v
EL1

ELR_EL1
SPSR_EL1

...

ERET
 |
 v
EL0
```

RISC-V：

```text
U
 |
 | trap
 v
S

sepc
sstatus.SPP

...

SRET
 |
 v
U
```

对应关系：

```text
Arm                           RISC-V

ELR_EL1             <---->    sepc

SPSR_EL1            <---->    sstatus/SPP

ERET                 <---->    SRET
```

你的 Arm exception 经验在这里几乎可以直接迁移。

---

# 24. 为什么 M-mode 不直接运行 Linux？

理论上：

```text
Linux @ M-mode
```

当然可以设计。

因为 M-mode 能做几乎所有事情。

但问题是：

```text
Linux
```

如果在 M-mode：

```text
Linux bug
    |
    v
whole machine compromised
```

而且 OS 与具体 machine hardware 强耦合。

RISC-V 更希望：

```text
      Linux
       S
       |
      SBI
       |
   Machine FW
       M
```

这样：

```text
Machine-specific detail
```

放到底层：

```text
M
```

而：

```text
OS
```

工作在：

```text
S
```

因此 S-mode 官方架构本身就刻意减少对底层 physical hardware 的直接依赖。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/supervisor.html?utm_source=chatgpt.com)

---

# 25. OpenSBI 在这里是什么位置？

现在你已经可以第一次真正理解 OpenSBI：

```text
                 M-mode
        ┌───────────────────────┐
        │       OpenSBI         │
        │                       │
        │ machine timer         │
        │ IPI                   │
        │ hart management       │
        │ platform operations   │
        └──────────┬────────────┘
                   │
                  SBI
                   │
        ┌──────────▼────────────┐
        │         Linux         │
        │        S-mode         │
        └──────────┬────────────┘
                   │
                syscall
                   │
        ┌──────────▼────────────┐
        │      application      │
        │        U-mode         │
        └───────────────────────┘
```

所以 OpenSBI 不是：

```text
Bootloader
```

这么简单。

更准确地说：

> OpenSBI 是典型的 M-mode runtime firmware，为 S-mode software 提供 SBI execution environment。

以后我们专门学习 OpenSBI 时再展开。

---

# 26. 一个完整 Linux 场景

现在把整套东西串起来。

### Step 1：Reset

RISC-V hart reset 后：

```text
CPU
 |
 v
M-mode
```

进入 machine firmware。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/machine.html?utm_source=chatgpt.com)

例如：

```text
OpenSBI
```

---

### Step 2：M-mode 初始化 machine

OpenSBI：

```text
setup PMP

setup interrupt

setup delegation

prepare Linux
```

例如：

```text
medeleg
mideleg
```

把 Linux 应该处理的 trap delegation 给 S。

---

### Step 3：M → S

OpenSBI：

```text
mepc = Linux entry
MPP  = S
```

执行：

```text
MRET
```

得到：

```text
M
|
| MRET
v
S

Linux
```

---

### Step 4：Linux 创建 process

Linux：

```text
setup page table
setup process context

sepc = user entry
SPP  = U
```

然后：

```text
SRET
```

得到：

```text
S
|
| SRET
v
U

application
```

---

# 27. Application 发起 syscall

现在 application：

```c
write(fd, buf, len);
```

最终：

```asm
ecall
```

产生：

```text
Environment Call from U-mode
```

由于 M firmware 已经 delegation：

```text
U
|
| ECALL
v
S
```

硬件：

```text
sepc   ← syscall instruction PC
scause ← Environment Call from U-mode
SPP    ← U
PC     ← stvec
```

Linux syscall handler 开始执行。

---

# 28. syscall return

Linux 处理完成：

```text
write(...)
```

之后：

```asm
sret
```

CPU：

```text
PC        ← sepc
Privilege ← U
```

回到 application。

完整路径：

```text
U application
      |
      | ECALL
      v
S Linux
      |
      | syscall handler
      |
      | SRET
      v
U application
```

这和 Arm：

```text
EL0
 |
 | SVC
 v
EL1
 |
 | ERET
 v
EL0
```

本质完全一致。

---

# 29. Linux 如果需要 machine-level 服务怎么办？

现在出现另一个场景。

假设 Linux 需要某个 SBI service：

```text
S-mode Linux
```

执行：

```asm
ecall
```

如果这个 S-mode ECALL 没有 delegation 给 S：

```text
S
|
| ECALL
v
M
```

进入：

```text
OpenSBI
```

OpenSBI 处理完成：

```text
MRET
```

返回：

```text
S
```

于是整个 privilege stack 是：

```text
                OpenSBI
                 M
                 ▲
                 │ SBI ECALL
                 │
                Linux
                 S
                 ▲
                 │ syscall ECALL
                 │
             Application
                 U
```

这是整个 RISC-V Linux software stack 最漂亮的一张图。

---

# 30. 两层 ECALL 不要混淆

你以后看 Linux/OpenSBI 代码特别容易混淆：

```text
U executes ECALL
```

通常表示：

```text
system call

U → S
```

而：

```text
S executes ECALL
```

通常表示：

```text
SBI call

S → M
```

所以：

```text
ECALL
```

本身不是：

```text
system call instruction
```

更加准确是：

> **Environment Call。**

究竟进入哪一级，取决于：

```text
current privilege
+
trap delegation configuration
```

这点和 Arm `SVC/HVC/SMC` 分成不同指令的设计差异很明显。

---

# 31. Arm 与 RISC-V 在这里一个很有意思的设计差异

Arm 有：

```text
SVC → service call
HVC → hypervisor call
SMC → secure monitor call
```

不同 privilege boundary 常常有专门 instruction。

RISC-V 更倾向于：

```text
ECALL
```

统一产生 environment-call exception。

然后根据：

```text
current privilege
+
delegation
+
execution environment
```

决定由谁处理。

例如：

```text
ECALL from U
→ Environment call from U-mode

ECALL from S
→ Environment call from S-mode

ECALL from M
→ Environment call from M-mode
```

这种设计非常体现 RISC-V：

> **机制简单，把 policy 更多留给 privileged software。**

---

# 32. 为什么需要三层，而不是只有 S/U？

这个问题非常值得深入。

假设只有：

```text
Kernel
Application
```

即：

```text
S
U
```

那么：

```text
Kernel
```

必须直接负责：

```text
SoC startup
timer implementation
IPI
power management
platform-specific hardware
security monitor
```

这样 OS 和 machine 紧密耦合。

加入：

```text
M
```

以后变成：

```text
                  M
        machine execution environment
                  |
            standard interface
                  |
                  S
                  |
                  U
```

于是：

```text
M:
machine-specific

S:
OS-specific

U:
application-specific
```

这是非常漂亮的 architecture separation。

---

# 33. 反过来：为什么嵌入式 MCU 可以只有 M？

因为一些 MCU：

```text
single firmware
no process
no MMU
no userspace
```

例如：

```text
bare-metal firmware
```

那么：

```text
M only
```

就够了。

官方定义支持：

```text
M
```

作为最小 implementation。

典型：

```text
small embedded processor
```

不需要复杂 privilege isolation。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/priv-intro.html?utm_source=chatgpt.com)

---

# 34. M + U 有什么意义？

还可以有：

```text
M
U
```

没有 S。

例如一个小型 embedded runtime：

```text
            M
        RTOS/kernel
            |
          PMP
            |
       +----+----+
       |         |
       U         U
     Task A    Task B
```

这样的系统不需要：

```text
full Unix-style supervisor
```

但是希望：

```text
application isolation
```

因此使用：

```text
M + U
```

官方也把这种两级配置视为适合 secure embedded systems 的标准组合之一。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/priv-intro.html?utm_source=chatgpt.com)

这与你熟悉的 Cortex-M：

```text
Privileged
Unprivileged
+
MPU
```

有一定设计思想上的相似性。

---

# 35. 这里可以对应你的 Armv8-M 经验

可以粗略地：

```text
Armv8-M                       RISC-V embedded

Privileged                    M

Unprivileged                  U

MPU                           PMP
```

但仍然不是完全相同。

例如 PMP 的 fundamental model 更接近：

```text
M configures physical memory protection
for S/U
```

后面我们会单独学习。

---

# 36. 一个必须纠正的理解：高 privilege 并不等于自动拥有所有 memory

通常我们会说：

```text
M > S > U
```

但 memory access 要区分：

```text
architectural privilege
```

和：

```text
effective memory privilege
```

例如 M-mode 有一个很有意思的：

```text
mstatus.MPRV
```

Machine Previous/Modify Privilege 相关机制。

设置 MPRV 后，M-mode 的 data load/store 可以按照 MPP 指定的低 privilege 来进行 translation/protection。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/machine.html?utm_source=chatgpt.com)

例如 kernel/firmware 想安全地读取低 privilege buffer：

```text
M code
 |
 | temporarily use S/U memory privilege
 v
load/store
```

这个设计以后讨论：

```text
copy_from_user
memory protection
```

时会很有意思。

目前知道有这个概念即可。

---

# 37. Privilege 和 Interrupt Priority 不是一回事

另一个容易混淆的问题：

```text
M
S
U
```

不是：

```text
interrupt priority
```

它描述：

```text
software execution privilege
```

例如：

```text
Machine Timer Interrupt
Supervisor Timer Interrupt
```

这里：

```text
Machine
Supervisor
```

更多是在描述：

```text
interrupt privilege target
```

而不是简单：

```text
Machine interrupt priority > Supervisor interrupt priority
```

interrupt routing / enable / pending / delegation 的具体规则，我们会在后面 Interrupt 章节专门展开。

---

# 38. 也不要把 M/S/U 理解成 ARM CPU mode 的简单翻版

尤其 Armv7-A：

```text
User
System
Supervisor
IRQ
FIQ
Abort
Undefined
Monitor
```

这里既混合：

```text
privilege
```

又混合：

```text
exception mode
```

RISC-V 的设计更干净：

```text
Privilege mode:

M
S
U
```

而 exception / interrupt：

```text
Trap
```

是另一套机制。

所以不会：

```text
interrupt occurs
→ enter IRQ mode
```

这种思维。

而是：

```text
interrupt
   |
   v
trap
   |
   v
M or S trap handler
```

这点更接近 AArch64：

```text
Exception Level
+
Exception handling
```

而不是 Armv7-A 的 banked exception modes。

---

# 39. 一个特别值得形成的 mental model

不要把：

```text
M
S
U
```

理解成三个“软件”。

应该理解成：

```text
CPU currently executing with privilege X
```

比如同一个 Linux system：

```text
time t0:

CPU privilege = U
PC = application


time t1:

CPU privilege = S
PC = syscall handler


time t2:

CPU privilege = M
PC = OpenSBI


time t3:

CPU privilege = S
PC = kernel


time t4:

CPU privilege = U
PC = application
```

同一个 hart 不断改变：

```text
current privilege mode
```

而软件：

```text
OpenSBI
Linux
Application
```

只是通常被设计为运行在特定 mode。

---

# 40. Hart 和 privilege 的关系

每一个 hart 都有自己的 execution state。

例如 4-core：

```text
Hart 0        Hart 1        Hart 2        Hart 3

   S             U             M             U
 Linux        process       OpenSBI       process
```

理论上某个时间点不同 hart 完全可以处于不同 privilege。

所以：

```text
privilege mode
```

是：

> **per-hart execution state**

而不是：

```text
整个 SoC 当前处于 M-mode
```

这种 global state。

这一点对于 SMP 很重要。

---

# 41. 再回来看 M/S/U 的核心设计哲学

到这里，可以把这一章压缩成三句话。

### 第一层：U

```text
I don't trust applications.
```

所以：

```text
U
```

只给 application 必需权限。

---

### 第二层：S

```text
Applications need a privileged manager.
```

所以：

```text
S
```

运行 OS：

```text
process
memory
syscall
scheduler
```

---

### 第三层：M

```text
The OS itself should not own the whole machine.
```

所以：

```text
M
```

控制 machine-level environment：

```text
hardware
PMP
delegation
firmware
SBI
```

最终：

```text
M controls machine

S controls OS environment

U consumes OS services
```

---

# 42. 用一个 Arm 工程师最容易理解的图总结

你可以先把 RISC-V Linux 记成：

```text
                  RISC-V
                     │

              ┌──────────────┐
              │    M-mode    │
              │              │
              │   OpenSBI    │
              │              │
              │  ≈ 部分 EL3  │
              └──────┬───────┘
                     │
                    SBI
                     │
                     ▼
              ┌──────────────┐
              │    S-mode    │
              │              │
              │ Linux Kernel │
              │              │
              │    ≈ EL1     │
              └──────┬───────┘
                     │
                  syscall
                     │
                     ▼
              ┌──────────────┐
              │    U-mode    │
              │              │
              │ Application  │
              │              │
              │    ≈ EL0     │
              └──────────────┘
```

而安全边界：

```text
                 OpenSBI / M
                      |
                     PMP
                      |
                      v
                  Linux / S
                      |
                    Sv39
                  page table
                      |
                      v
               Application / U
```

这两张图掌握以后，M/S/U 的框架基本已经建立起来了。

---

# 43. 最后做一个完整状态机

这是我建议你真正记住的图：

```text
                         RESET
                           |
                           v
                     +-----------+
                     |  M-mode   |
                     | OpenSBI   |
                     +-----------+
                          |
                          | MPP=S
                          | mepc=Linux
                          | MRET
                          v
                     +-----------+
                     |  S-mode   |
                     |  Linux    |
                     +-----------+
                          |
                          | SPP=U
                          | sepc=app
                          | SRET
                          v
                     +-----------+
                     |  U-mode   |
                     |   App     |
                     +-----------+
                          |
                          | ECALL
                          |
                          v
                     +-----------+
                     |  S-mode   |
                     | syscall   |
                     +-----------+
                          |
                          | SBI ECALL
                          v
                     +-----------+
                     |  M-mode   |
                     | OpenSBI   |
                     +-----------+
                          |
                          | MRET
                          v
                     +-----------+
                     |  S-mode   |
                     +-----------+
                          |
                          | SRET
                          v
                     +-----------+
                     |  U-mode   |
                     +-----------+
```

如果你能完全解释这张图：

> **RISC-V M/S/U privilege 的主体结构你就已经真正掌握了。**

---

# 44. 这一章你需要达到什么程度？

我建议先不要背几十个 CSR。

这一阶段应该确保自己能够独立回答下面这些问题：

1. 为什么 RISC-V 需要 M/S/U，而不是只有 kernel/user？
2. 为什么 M-mode 是 mandatory，而 S/U 可以 optional？
3. 为什么 `M ≈ EL3` 只能作为粗略类比？
4. M/S/U 是否天然实现了 memory isolation？
5. PMP 和 MMU 在这个 privilege hierarchy 中分别位于哪里？
6. Reset 后为什么先进入 M-mode？
7. M-mode 如何进入 S-mode？
8. S-mode 如何进入 U-mode？
9. U-mode syscall 为什么可以直接进入 S，而不是 M？
10. `medeleg/mideleg` 的 architectural purpose 是什么？
11. 为什么 delegation 不能让 M-mode trap “下降”到 S？
12. `MRET/SRET` 为什么既可以用于 trap return，又可以用于初次进入低 privilege？
13. `U ECALL` 和 `S ECALL` 为什么可能进入不同 software layer？
14. OpenSBI、Linux、application 分别通常运行在哪一级？

如果这 14 个问题你都可以自然回答，**我们就不需要继续停留在 privilege 的概念层面了。**

下一章 `02 CSR Architecture` 最适合接着这一章学习，因为现在已经产生了一个很自然的问题：

```text
M/S/U 已经知道了

        ↓

CPU 到底通过什么 architectural state
保存和控制这些 privilege？

        ↓

CSR
```

下一阶段我们可以重点把：

```text
mstatus / sstatus
mepc    / sepc
mcause  / scause
mtval   / stval
mtvec   / stvec

mie / mip
sie / sip

medeleg / mideleg
misa
satp
```

按照**“为什么存在 → 谁能访问 → 保存什么 → 在真实 trap 中什么时候被修改”**的方式串成一个完整 CSR architecture，而不是逐个寄存器死记。这会和你熟悉的 `SCTLR_ELx / ESR_ELx / ELR_ELx / SPSR_ELx / VBAR_ELx` 很容易对应起来。
