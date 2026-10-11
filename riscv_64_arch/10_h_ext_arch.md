# RISC-V Architecture 第 10 阶段：H Extension

先给出这一章最重要的结论：

> **RISC-V H Extension 的本质，不是增加一个像 Arm EL2 那样的新线性 privilege level，而是在原有 M/S/U privilege architecture 上增加一个 `V`（Virtualization Mode）维度，并在 S-mode 旁边构造出一套“虚拟 S-mode”。**

因此，理解 H extension 的正确入口不是：

```text
M
↓
HS
↓
VS
↓
VU
```

这张图虽然有时方便，但**容易把你的 Arm EL3→EL2→EL1→EL0 思维带偏**。

更准确的模型是：

```text
             Virtualization Mode V
                 0             1
              ┌──────┬────────────────┐
Nominal M     │  M   │   不存在        │
              ├──────┼────────────────┤
Nominal S     │  HS  │      VS         │
              ├──────┼────────────────┤
Nominal U     │  U   │      VU         │
              └──────┴────────────────┘
```

也就是说：

```text
HS = S privilege + V=0
VS = S privilege + V=1

 U = U privilege + V=0
VU = U privilege + V=1
```

**这一个模型如果真正理解了，后面 70% 的 H extension 都会顺下来。**

RISC-V 官方当前 Privileged Architecture 中，Hypervisor ISA 仍是 ratified **Version 1.0**。H extension 将原来的 S-mode 扩展成 HS-mode，并加入 VS/VU、第二阶段地址转换以及一组 hypervisor CSR/instruction。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

---

# 1. H Extension 到底要解决什么问题？

先不要看寄存器。

假设没有 H extension，我们想运行：

```text
Linux Host
   |
Hypervisor
   |
Guest Linux
   |
Guest App
```

Guest Linux 自己认为：

```text
我是 OS
我应该运行在 S-mode
```

但真正的 Host Linux / Hypervisor 也需要 S-mode。

于是问题来了：

```text
Guest Linux
    |
    | 想执行：
    |
    +--> csrw satp
    +--> sfence.vma
    +--> sret
    +--> 操作 stvec
    +--> 处理中断
```

这些都是 S-mode 行为。

如果只有：

```text
M
S
U
```

最朴素的方法只能让 Guest Linux：

```text
Guest Linux → U-mode
```

然后所有 privileged instruction 都 trap：

```text
Guest:

csrw satp,...
      |
      v
trap
      |
      v
Hypervisor
      |
      v
software emulate
```

理论上可以。

这就是经典的：

> **trap-and-emulate virtualization**

但是性能很差。

---

# 2. H Extension 的设计目标

H extension 的核心思路是：

> **让 Guest OS 大多数情况下真的按照 S-mode 的语义运行，而不是每执行一个 S-mode 操作都 VM exit。**

于是增加：

```text
VS = Virtual Supervisor mode
```

Guest Linux：

```text
Guest Linux
     ↓
VS-mode
```

它仍然认为：

```text
我运行在 S-mode
```

例如 Guest Linux 执行：

```asm
csrw satp, a0
```

它根本不知道：

```text
vsatp
```

这个寄存器存在。

硬件发现：

```text
V = 1
```

于是自动把：

```text
satp
```

解释为：

```text
vsatp
```

同理：

```text
Guest sees        Hardware actually uses

sstatus     →     vsstatus
stvec       →     vstvec
sepc        →     vsepc
scause      →     vscause
stval       →     vstval
satp        →     vsatp
sie         →     vsie
sip         →     vsip
```

这就是 H extension 非常漂亮的一个地方：

> **不是让 Guest OS 学会一套新的“virtual CSR”，而是硬件把 Guest 对 S CSR 的访问透明重定向到 VS CSR。**

所以一个普通 S-mode Linux kernel 原则上可以不修改，就运行在 VS-mode。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

---

# 3. H extension 的完整 privilege 模型

有 H extension 后：

```text
                         M-mode
                    Machine Firmware
                     OpenSBI 等
                         |
                         |
                   +-----+-----+
                   |           |
                  HS          V=1 world
             Hypervisor         |
                   |            |
                   |       +----+----+
                   |       |         |
                   |      VS        VU
                   |   Guest OS   Guest App
                   |
                   +---- U
                    Host userspace
```

例如典型 Linux/KVM：

```text
                    M
              ┌───────────┐
              │  OpenSBI  │
              └─────┬─────┘
                    │
                   HS
        ┌──────────────────────┐
        │ Host Linux + KVM     │
        └───────┬──────────────┘
                │
       ┌────────┴──────────┐
       │                   │
      U                    VS
┌────────────┐      ┌──────────────┐
│ QEMU/VMM   │      │ Guest Linux  │
└────────────┘      └──────┬───────┘
                           │
                          VU
                    ┌──────────────┐
                    │ Guest App    │
                    └──────────────┘
```

这个结构建议你牢牢记住。

---

# 4. 与 Armv8-A 最接近的对应关系

先建立一个粗略映射：

| Armv8-A | RISC-V H Extension |
|---|---|
| EL2 | HS-mode |
| Guest EL1 | VS-mode |
| Guest EL0 | VU-mode |
| Host EL0 | U-mode |
| Guest TTBR_EL1 | `vsatp` |
| VTTBR_EL2 + 部分 VTCR_EL2 作用 | `hgatp` |
| Stage-1 translation | VS-stage |
| Stage-2 translation | G-stage |
| Stage-2 IPA | Guest Physical Address |
| Host Physical Address | Supervisor Physical Address |
| HCR_EL2 | 部分对应 `hstatus` 等控制 |
| ESR_EL2 | `scause` + `htinst` 等 |
| FAR_EL2 | `stval` |
| HPFAR_EL2 | 概念上接近 `htval` |
| ELR_EL2 | `sepc` |
| TLBI Stage-1 | `HFENCE.VVMA` |
| TLBI Stage-2 | `HFENCE.GVMA` |

但是有一句一定要加：

> **这是功能类比，不是寄存器 1:1 映射。**

尤其：

```text
Arm EL2
```

是真正的独立 Exception Level。

而：

```text
HS
```

本质仍然是：

```text
S privilege + Hypervisor extension
```

官方甚至直接称它：

> hypervisor-extended supervisor mode

而不是一个新的 H privilege mode。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

---

# 5. `V` bit 才是 H extension 的灵魂

理解 H extension 时，可以在脑中始终放一个：

```text
V
```

虽然你不能简单把它当普通软件可随意操作的状态寄存器位，但架构行为可以这样理解。

---

## V=0

执行环境：

```text
M
HS
U
```

这时候：

```text
satp   = Host/HS page table
stvec  = HS trap vector
sepc   = HS trap state
scause = HS trap state
```

---

## V=1

执行：

```text
VS
VU
```

硬件进入 guest execution semantics。

于是：

```text
satp   → vsatp
stvec  → vstvec
sepc   → vsepc
scause → vscause
...
```

同时：

> **Two-stage translation 自动生效。**

官方明确规定：V=1 时启用 two-stage translation；V=0 时不启用。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

所以可以粗略记成：

```text
V=0
    Host world

V=1
    Guest world
    +
    Two-stage MMU
```

这句话极其重要。

---

# 6. 为什么叫 HS，而不叫 H-mode？

这是很多人的第一个疑问。

因为 H Extension 并没有创造：

```text
H privilege
```

原来的：

```text
S-mode
```

在实现 H extension 后扩展成：

```text
HS-mode
Hypervisor-extended Supervisor Mode
```

也就是说：

```text
without H:

M
S
U


with H:

M
HS
U

+
VS/VU guest execution
```

所以：

```text
HS ≠ 新 privilege level
```

而是：

```text
S + Hypervisor capabilities
```

这也是为什么大量原来的：

```text
sstatus
sie
sip
stvec
sepc
scause
satp
```

在 HS 中继续使用。

H extension 只是另外增加：

```text
hstatus
hedeleg
hideleg
hie
hip
hvip
hgatp
...
```

---

# 7. H extension 的 CSR 应该分成三类理解

不要一个一个死背。

## 第一类：Host/HS 原有 Supervisor CSR

例如：

```text
sstatus
stvec
sepc
scause
stval

sie
sip

satp
```

这些属于：

```text
HS
```

---

# 8. 第二类：Hypervisor 控制 CSR

核心：

```text
hstatus

hedeleg
hideleg

hie
hip
hvip

hgeip
hgeie

hgatp

htval
htinst

htimedelta
hcounteren
henvcfg
```

可以按功能归类：

```text
                H CSRs
                   |
       +-----------+------------+
       |           |            |
    Control     Interrupt       MMU
       |           |            |
   hstatus       hvip          hgatp
                hip
                hie
                  |
             hgeip/hgeie

       +-----------------------+
       |
      Trap
       |
     htval
     htinst
```

---

# 9. 第三类：VS CSR

这是 Guest OS 的“虚拟 Supervisor state”。

核心：

```text
vsstatus

vsie
vsip

vstvec
vsscratch

vsepc
vscause
vstval

vsatp
```

你可以简单理解成：

```text
S CSR
↓
复制一份
↓
VS CSR
```

例如：

```text
sstatus     vsstatus

stvec       vstvec

sepc        vsepc

scause      vscause

stval       vstval

satp        vsatp
```

为什么硬件一定要复制？

因为有些状态是在：

```text
trap entry
SRET
address translation
interrupt
```

这种硬件路径中立即生效。

不可能每次依赖 Hypervisor：

```text
save host sstatus
load guest sstatus
...
```

在恰好正确的 CPU pipeline 时刻完成切换。

所以硬件直接维护两套。

---

# 10. 这一设计真正厉害的地方：Guest 不知道 VS CSR

假设 Guest Linux：

```asm
csrw stvec, a0
```

Guest 的想法：

```text
设置自己的 S-mode exception vector
```

此时：

```text
V=1
```

于是硬件执行：

```text
vstvec ← a0
```

不是：

```text
stvec ← a0
```

因此：

```text
Host:
stvec = host_trap_handler

Guest:
vstvec = guest_trap_handler
```

可以同时存在。

同理：

```text
Host satp
Guest vsatp
```

也同时存在。

这正是：

> **hardware-assisted virtualization**

的核心之一。

---

# 11. 两阶段地址转换：H Extension 最核心的硬件能力

这一部分一定要学透。

Guest application：

```text
Guest Virtual Address
        |
        |
        v
    VS-stage
     vsatp
        |
        v
Guest Physical Address
        |
        |
        v
     G-stage
      hgatp
        |
        v
Supervisor Physical Address
```

官方命名：

```text
GVA
 ↓
VS-stage
 ↓
GPA
 ↓
G-stage
 ↓
SPA
```

官方明确规定，当 V=1 时，guest virtual address 首先通过 `vsatp` 进行 VS-stage translation 得到 GPA，再通过 `hgatp` 的 G-stage translation 得到 supervisor physical address。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

---

# 12. 与 Arm 两阶段 MMU 完全可以这样建立映射

Arm：

```text
Guest VA
   |
   | Stage-1
   | TTBR_EL1
   v
IPA
   |
   | Stage-2
   | VTTBR_EL2
   v
PA
```

RISC-V：

```text
Guest VA
   |
   | VS-stage
   | vsatp
   v
GPA
   |
   | G-stage
   | hgatp
   v
SPA
```

因此：

```text
Arm IPA
≈
RISC-V GPA
```

而：

```text
Arm Stage-2
≈
RISC-V G-stage
```

这是非常接近的概念。

---

# 13. 为什么需要第二阶段？

假设 Guest Linux 认为：

```text
Guest Physical RAM:

0x8000_0000
   ↓
0xBFFF_FFFF
```

Guest Linux 建立自己的：

```text
GVA → GPA
```

例如：

```text
0x400000
   ↓
0x90000000
```

但 Host 当然不能真的允许 Guest：

```text
直接访问 host physical 0x90000000
```

因为：

```text
VM A
VM B
Host
```

必须互相隔离。

Hypervisor 建立：

```text
Guest GPA
     ↓
Host real memory
```

例如：

```text
Guest GPA

0x90000000
     |
     | hgatp
     v
SPA

0x4A000000
```

于是：

```text
0x00400000 GVA
       |
       | vsatp
       v
0x90000000 GPA
       |
       | hgatp
       v
0x4A000000 SPA
```

这个：

```text
hgatp
```

就是 Hypervisor 控制 Guest memory isolation 的关键。

---

# 14. 谁控制哪张页表？

这是面试中特别重要的问题。

答案非常清楚：

```text
Guest OS
   |
   v
vsatp
   |
   v
VS-stage page tables
```

Guest 完全负责：

```text
GVA → GPA
```

而：

```text
Hypervisor
   |
   v
hgatp
   |
   v
G-stage page tables
```

Hypervisor 负责：

```text
GPA → SPA
```

因此权限边界：

```text
Guest:
我决定自己的 VA 如何使用

Hypervisor:
我决定 Guest 最终能够访问哪些真正的物理内存
```

这就是 virtualization memory isolation 的本质。

---

# 15. 一个特别容易忽略的地方：Guest page table 自己也要经过 G-stage

假设 Guest 做：

```text
GVA
 ↓
vsatp page walk
```

Guest 的页表本身位于：

```text
Guest Physical Address
```

例如：

```text
Guest page-table PTE
GPA = 0x88001000
```

硬件想读取这个 PTE。

但：

```text
0x88001000
```

只是 Guest Physical Address。

所以还必须：

```text
GPA 0x88001000
       |
       | G-stage
       v
SPA
```

才能真正读取 PTE。

因此真实过程比：

```text
GVA → GPA → SPA
```

更加有意思：

```text
               Guest page table
                    |
GVA                 | 每次读取 Guest PTE
 |                  | 本身也要经过
 |                  v
 |              G-stage
 |
 v
VS page walk
 |
 v
GPA
 |
 v
G-stage
 |
 v
SPA
```

这就是 nested page walk 性能代价产生的根源。

CPU 通常会通过：

```text
TLB
combined translation cache
page-walk cache
```

来降低成本。

---

# 16. `hgatp`

你可以把：

```text
satp
```

和：

```text
hgatp
```

放一起看。

`satp`：

```text
MODE
ASID
PPN
```

`hgatp`：

```text
MODE
VMID
PPN
```

所以：

```text
satp.ASID
```

用于区分 address space。

而：

```text
hgatp.VMID
```

用于区分 virtual machine。

可以形成：

```text
VMID
 |
 +-- VM A
 |
 +-- VM B
 |
 +-- VM C
```

这样 VM 切换时不必总是把所有 translation cache 全部清空。

官方 `hgatp` 包含 G-stage root page-table PPN、VMID 以及 MODE；在 RV64 中定义了 Sv39x4、Sv48x4、Sv57x4 等 G-stage 模式。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

---

# 17. 为什么是 `Sv39x4` 而不是 `Sv39`？

这个命名第一次看非常奇怪。

Guest：

```text
vsatp.MODE = Sv39
```

但：

```text
hgatp.MODE = Sv39x4
```

为什么多了：

```text
x4
```

原因是 G-stage 支持比普通 Sv39 多：

```text
2 bits
```

的 Guest Physical Address。

普通 Sv39：

```text
39-bit VA
```

而：

```text
Sv39x4
```

允许：

```text
41-bit GPA
```

这额外两位通过：

> **把最顶层 root page table 扩大 4 倍**

来实现。

普通：

```text
root PT = 4 KiB
```

Sv39x4：

```text
root PT = 16 KiB
```

而且要求：

```text
16 KiB alignment
```

官方定义 Sv39x4 将输入 GPA 扩展两位，因此只有 root page table 扩展为普通大小的四倍，即 16 KiB。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

这个设计非常 RISC-V：

> 尽量复用已有 Sv39 page walker，仅对 root level 做最小扩展。

---

# 18. 两种 Page Fault 一定不要混

这是 H extension 最重要的考点之一。

有：

```text
Page Fault
```

和：

```text
Guest-Page Fault
```

它们完全不是一个责任域。

---

## Case 1：VS-stage page fault

例如：

```text
Guest App
   |
   v
GVA = 0x400000
```

Guest Linux 的：

```text
vsatp
```

页表里面没有映射。

于是：

```text
VS-stage fault
```

本质是：

```text
Guest OS 自己的 page fault
```

应该交给：

```text
Guest Linux
```

处理。

因此：

```text
VU
 |
 | page fault
 v
VS Guest Kernel
```

完全可以**不发生 VM Exit**。

---

# 19. Case 2：G-stage Guest-Page Fault

假设：

```text
GVA
 |
 | VS-stage OK
 v
GPA = 0x90000000
```

但是 Hypervisor 的：

```text
hgatp
```

里：

```text
0x90000000
```

没有映射。

于是发生：

```text
Guest-Page Fault
```

这是：

```text
G-stage fault
```

责任属于：

```text
Hypervisor
```

所以：

```text
VS/VU
   |
   | Guest-Page Fault
   v
HS
```

发生：

> **VM Exit**

Guest-page fault 的 exception cause 包括 instruction/load/store guest-page fault；这类 fault 可由 M-mode 经 `medeleg` 交给 HS，但不能继续 delegate 给 VS。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

---

# 20. 这正对应 Arm Stage-1 / Stage-2 Fault

你可以这样快速复用 Arm 思维：

```text
Guest Stage-1 fault
        |
        v
Guest kernel handles


Guest Stage-2 fault
        |
        v
Hypervisor handles
```

对应 RISC-V：

```text
VS-stage page fault
        |
        v
VS Guest OS


G-stage guest-page fault
        |
        v
HS Hypervisor
```

这是几乎完全一样的设计思想。

---

# 21. Guest-page fault 时，Hypervisor 怎么知道哪里出错？

这里就引出了：

```text
stval
htval
htinst
```

假设：

```text
Guest load
GVA = 0x40001234
```

VS-stage：

```text
→ GPA = 0x90001234
```

然后 G-stage fault。

HS 得到：

```text
stval
≈ guest virtual address

htval
≈ guest physical address >> 2

htinst
≈ trap instruction information
```

所以：

```text
stval
告诉你：
哪个 Guest Virtual Address 出错

htval
告诉你：
对应哪个 Guest Physical Address 出错
```

这和你的 Arm 经验可以这样类比：

```text
FAR_EL2
HPFAR_EL2
```

虽然不是严格一一对应。

官方规定 guest-page fault 进入 HS 时，`stval` 保存相应 faulting guest virtual address，而 `htval` 可以保存 faulting GPA 右移两位后的值；`htinst` 则可以提供导致 trap 的指令信息。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

---

# 22. 为什么 `htval` 的 GPA 要右移 2 bits？

这是一个很有 RISC-V 风格的小设计。

GPA 可能比：

```text
XLEN
```

更宽。

例如：

```text
RV64
Sv57x4

GPA width > 普通可表示地址宽度
```

但地址最低：

```text
2 bits
```

可以另外恢复/组合。

因此：

```text
htval = GPA >> 2
```

可以在同样 XLEN 宽度寄存器中保存更宽的 GPA 信息。

---

# 23. Trap Delegation：H extension 另一个核心

你之前已经学过：

```text
medeleg
mideleg
```

现在只是在它下面增加第二层：

```text
hedeleg
hideleg
```

整体：

```text
          trap
            |
            v
           M
            |
       medeleg/mideleg
            |
            v
           HS
            |
       hedeleg/hideleg
            |
            v
           VS
```

官方规定，对于 VS/VU 产生的 trap：

```text
没有 M delegation
       → M

M delegation
但没有 H delegation
       → HS

M delegation
+
H delegation
       → VS
``` :chatgpt-content-reference{index="9"}


这是一条必须背后的**架构逻辑**。

---

# 24. 一个完美的例子：Guest App syscall

Guest application：

```text
VU
```

执行：

```asm
ecall
```

它是在向：

```text
Guest Linux
```

做 system call。

我们当然不希望：

```text
VU
 ↓
HS Hypervisor
 ↓
VS Guest Linux
```

每次 syscall 都 VM exit。

所以 Hypervisor 配：

```text
hedeleg
```

让：

```text
ECALL from VU
```

直接进入：

```text
VS
```

于是：

```text
Guest App
   VU
    |
    | ECALL
    v
Guest Linux
   VS
```

Host 完全不参与。

这就是 virtualization 必须做到的：

> **Guest 内部事件尽量 Guest 内部解决。**

---

# 25. 但是 Guest Linux 的 SBI ECALL 不一样

这里能把我们第 8 阶段 SBI/OpenSBI 和本章真正串起来。

Guest Linux 在 VS-mode：

```text
Guest Linux
VS
```

调用：

```text
SBI
```

就是：

```asm
ecall
```

但是这是：

```text
ECALL from VS
```

这不是 Guest 内部 syscall。

它代表：

> “Guest Supervisor 向它认为的 execution environment 请求服务。”

所以它不能再交给 VS 自己。

于是：

```text
VS Guest Linux
      |
      | ECALL / SBI
      v
HS Hypervisor
```

这是一次：

```text
VM Exit
```

然后 HS Hypervisor：

```text
模拟 SBI
```

必要时再：

```text
HS
 |
 | real SBI ECALL
 v
M / OpenSBI
```

于是形成：

```text
Guest Linux
    VS
     |
     | virtual SBI
     v
Hypervisor
    HS
     |
     | real SBI
     v
OpenSBI
     M
```

官方明确规定：HS-mode 自身仍通过 SBI 与 M-level execution environment 交互，而 HS hypervisor 应为自己的 VS guest 提供 SBI。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

这张图非常重要。

---

# 26. 为什么 ECALL from VS 不能 delegate 回 VS？

因为这在逻辑上就是：

```text
VS → higher privilege environment
```

如果：

```text
VS ECALL → VS
```

那相当于：

```text
Supervisor 调 SBI
又回 Supervisor 自己
```

没有意义。

因此相应 exception 不能通过：

```text
hedeleg
```

重新送回 VS。

---

# 27. Guest Entry / VM Exit 到底如何发生？

我们现在把最核心流程跑一遍。

假设 Host Hypervisor 当前：

```text
HS
V=0
```

准备运行 Guest Linux。

它提前准备：

```text
vsstatus
vstvec
vsepc
vsatp

hgatp
```

以及 guest GPR。

然后设置：

```text
sepc = Guest entry PC

hstatus.SPV = 1
sstatus.SPP = 1
```

从概念上表示：

```text
SRET 后：

V = 1
Privilege = S
```

也就是：

```text
VS
```

执行：

```asm
sret
```

于是：

```text
HS
 |
 | SRET
 v
VS
```

开始运行 Guest。

---

# 28. VM Exit

Guest 运行过程中发生：

```text
G-stage fault
virtual instruction
Guest SBI ECALL
某些 host interrupt
...
```

需要 HS 处理。

硬件：

```text
VS/VU
   |
   | trap
   v
HS
```

并：

```text
V ← 0
```

同时保存：

```text
sepc
scause
stval

hstatus.SPV
hstatus.SPVP
hstatus.GVA

htval
htinst
```

官方定义 trap 从 VS/VU 进入 HS 时，V 被清为 0，`hstatus.SPV` 记录之前的 V，`sstatus.SPP`/`hstatus.SPVP` 保存相应 privilege 信息，随后 `SRET` 可以恢复到 VS/VU。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

---

# 29. `SPV` 应该怎么理解？

这个名字可以直接展开：

```text
SPV
=
Supervisor Previous Virtualization mode
```

和：

```text
SPP
=
Supervisor Previous Privilege
```

是一组。

普通 S-mode trap：

```text
SPP
```

告诉你之前来自：

```text
S
还是
U
```

H extension 后还必须知道：

```text
刚才是不是 Guest？
```

于是：

```text
SPV
```

保存原来的：

```text
V
```

所以：

```text
trap from VS:

SPV = 1
SPP = 1


trap from VU:

SPV = 1
SPP = 0


trap from U:

SPV = 0
SPP = 0
```

漂亮地得到：

```text
SPV + SPP
```

足够描述：

```text
HS
U
VS
VU
```

来源。

---

# 30. `SPVP` 又是什么？

这是第一次学习最容易和：

```text
SPV
```

混淆的东西。

展开：

```text
SPVP
=
Supervisor Previous Virtual Privilege
```

它重点服务：

```text
HLV
HLVX
HSV
```

这些 hypervisor 访问 Guest memory 的指令。

它告诉硬件：

> 用 VS privilege 还是 VU privilege 来解释这次 Guest memory access。

因此：

```text
SPVP = 0
→ emulate VU access

SPVP = 1
→ emulate VS access
```

不是用来表示：

```text
现在是否 V=1
```

那个是：

```text
SPV
```

---

# 31. `SRET` 为什么既能从 HS 回 Guest，也能从 VS 回 Guest App？

这个设计也很漂亮。

---

## HS 中 SRET

```text
V=0
```

执行：

```asm
sret
```

硬件看：

```text
hstatus.SPV
+
sstatus.SPP
```

例如：

```text
SPV=1
SPP=1
```

返回：

```text
VS
```

如果：

```text
SPV=1
SPP=0
```

返回：

```text
VU
```

---

## VS 中 SRET

此时：

```text
V=1
```

Guest Linux 执行：

```asm
sret
```

这是普通 Guest OS exception return：

```text
VS
 |
 | SRET
 v
VU
```

它使用的是：

```text
vsstatus.SPP
vsepc
```

所以 Guest Linux 看到的仍然是正常 S-mode architecture。

官方分别定义了 V=0 时 HS 的 `SRET` 和 V=1 时 VS 的 `SRET` 行为。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

---

# 32. Virtual Instruction Exception

H extension 新增了一个非常重要的 exception：

```text
Virtual Instruction Exception
```

cause：

```text
22
```

它和：

```text
Illegal Instruction
```

要严格区分。

---

# 33. 为什么需要 Virtual Instruction Exception？

假设 Guest：

```text
VS
```

执行某条本来在：

```text
真实 S-mode
```

合法，但 Hypervisor 希望拦截的指令。

例如 Hypervisor 设置：

```text
hstatus.VTVM = 1
```

那么 Guest 执行：

```asm
csrw satp,...
```

或者：

```asm
sfence.vma
```

就产生：

```text
Virtual Instruction Exception
```

而不是：

```text
Illegal Instruction
```

区别在于：

```text
Illegal Instruction
=
CPU 根本不支持/当前 privilege 非法

Virtual Instruction
=
对于真实 S-mode 这本来合法，
但因为你现在是 virtual S-mode，
Hypervisor 要接管它
```

这是一个非常重要的架构语义区别。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

---

# 34. `VTVM / VTW / VTSR`

你已经认识：

```text
mstatus.TVM
mstatus.TW
mstatus.TSR
```

H extension 又增加：

```text
hstatus.VTVM
hstatus.VTW
hstatus.VTSR
```

对应：

```text
VTVM
→ virtualize satp / SFENCE.VMA 等

VTW
→ virtualize WFI

VTSR
→ virtualize SRET
```

本质：

```text
mstatus.TVM
控制真实 S/HS

hstatus.VTVM
控制 virtual S / VS
```

非常规整。

---

# 35. Two-stage TLB 管理

既然现在有：

```text
VS-stage
+
G-stage
```

当然需要两类 fence。

因此 H extension 增加：

```text
HFENCE.VVMA
HFENCE.GVMA
```

---

# 36. `HFENCE.VVMA`

可以理解：

```text
Hypervisor
对某 Guest 的
VS-stage translation
执行 fence
```

也就是 Guest：

```text
GVA → GPA
```

这一层。

它的效果类似：

> Hypervisor 替 Guest 做一次相应 `SFENCE.VMA`。

官方也直接说明，`HFENCE.VVMA` 的效果可概念性理解为临时进入 VS-mode 执行 `SFENCE.VMA`。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

---

# 37. `HFENCE.GVMA`

处理：

```text
GPA → SPA
```

即：

```text
hgatp
```

管理的 translation。

可以按照：

```text
GPA
+
VMID
```

限定影响范围。

所以：

```text
HFENCE.VVMA

VS-stage


HFENCE.GVMA

G-stage
```

是最重要的记忆方式。

---

# 38. 与 Arm TLBI 的对应

可以粗略记：

```text
Arm:

Stage-1 TLBI
≈
HFENCE.VVMA


Stage-2 TLBI
≈
HFENCE.GVMA
```

但是保持你前面学习 memory barrier 时的严谨：

> `HFENCE` 的架构语义首先是 memory-management ordering / translation synchronization，而不是承诺“内部物理上执行某个特定 TLB flush 动作”。

硬件可以使用：

```text
TLB
combined TLB
walk cache
```

等不同实现。

---

# 39. Guest 执行自己的 `SFENCE.VMA` 怎么办？

这个也很关键。

当：

```text
V=1
```

Guest Linux：

```asm
sfence.vma
```

默认作用于：

```text
VS-stage
```

也就是：

```text
vsatp
```

对应的：

```text
GVA → GPA
```

不会让 Guest 任意修改 Hypervisor：

```text
hgatp
```

管理的：

```text
GPA → SPA
```

这就是 privilege separation。

---

# 40. Interrupt Virtualization

H extension 的第三个大核心就是：

> **让 Hypervisor 可以给 Guest 注入中断。**

核心寄存器：

```text
hvip
hip
hie
```

其中：

```text
hvip
=
Hypervisor Virtual Interrupt Pending
```

Hypervisor 可以设置：

```text
VSSIP
VSTIP
VSEIP
```

分别表示：

```text
Virtual Supervisor Software Interrupt

Virtual Supervisor Timer Interrupt

Virtual Supervisor External Interrupt
```

官方规定 `hvip` 可以由 HS 写入这些 virtual interrupt pending 位；`hip`/`hie` 则记录相应 pending/enable 状态。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

---

# 41. Guest 看到什么？

Guest 不应该看到：

```text
VSSIP
VSTIP
VSEIP
```

否则 Guest 会知道：

```text
“我是虚拟机”
```

Guest Linux 只看到正常的：

```text
SSIP
STIP
SEIP
```

也就是：

```text
vsip
vsie
```

内部完成映射：

```text
Hypervisor world:

VSSIP
VSTIP
VSEIP


        ↓ virtualize


Guest sees:

SSIP
STIP
SEIP
```

因此普通 Linux interrupt subsystem 基本不需要知道自己运行在 VS。

---

# 42. `hideleg` 的作用

假设 Hypervisor 注入：

```text
Virtual Timer Interrupt
```

如果：

```text
hideleg
```

允许交给 VS：

```text
hvip.VSTIP
     |
     v
VS Guest Kernel
```

Guest 直接进入自己的：

```text
vstvec
```

而不是先：

```text
VS
 ↓
HS
 ↓
VS
```

这能减少 VM exits。

---

# 43. Guest External Interrupt

H extension 还有：

```text
hgeip
hgeie
```

即：

```text
Hypervisor Guest External Interrupt Pending
Hypervisor Guest External Interrupt Enable
```

以及：

```text
hstatus.VGEIN
```

用于选择某个 guest external interrupt source。

这为硬件直接把外部中断导向 Guest 提供了架构基础。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

后续学：

```text
AIA
IMSIC
```

时你会看到这一套进一步扩展。

因此可以把：

```text
H extension
```

看作 virtualization framework，而：

```text
AIA
```

进一步解决高性能 scalable interrupt virtualization。

---

# 44. Timer Virtualization

还有一个非常漂亮的设计：

```text
htimedelta
```

Guest 读取时间时可以得到概念上的：

```text
Guest Time
=
Physical Time
+
htimedelta
```

所以：

```text
VM A
VM B
```

可以拥有不同 virtual time base。

这样无需：

```text
每次 guest read time
→ VM exit
→ software emulate
```

当实现 `time` CSR 时，H extension 要求实现对应的 `htimedelta`。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

如果结合：

```text
Sstc
vstimecmp
```

还可以进一步降低 virtual timer interrupt 的 trap/emulation 开销。

---

# 45. Hypervisor 如何读取 Guest memory？

这是实际写 Hypervisor 时必然遇到的问题。

例如 Guest 触发：

```text
SBI call
```

其中：

```text
a0 = pointer
```

这个 pointer 是：

```text
Guest Virtual Address
```

Hypervisor 在：

```text
HS
```

怎么读取它？

普通：

```asm
ld
```

不行。

因为普通 HS load 使用：

```text
Host translation
```

不是 Guest translation。

所以 H extension 提供：

```text
HLV.*
HLVX.*
HSV.*
```

---

# 46. HLV / HSV

例如：

```asm
HLV.D
```

意思可以粗略理解：

> Hypervisor Load Virtual

按照 Guest 的：

```text
VS/VU privilege
+
VS-stage
+
G-stage
```

去读取 Guest memory。

例如：

```text
HS Hypervisor
     |
     | HLV
     v
Guest Virtual Address
     |
     | vsatp
     v
Guest Physical
     |
     | hgatp
     v
Real memory
```

所以 Hypervisor 不需要真正：

```text
切回 VS
```

就能访问 Guest address space。

官方规定 HLV/HLVX/HSV 的显式内存访问按照 VS 或 VU 权限执行，并应用两阶段地址转换。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

---

# 47. HLVX 又是干什么？

`HLVX` 很特别。

它不是按照：

```text
Read permission
```

检查 Guest page。

而是按照：

```text
Execute permission
```

检查。

主要用途：

> Hypervisor 需要读取 Guest 正在执行的 instruction。

例如 emulate trapped instruction。

如果 Guest memory：

```text
X=1
R=0
```

普通 HLV 读不到。

但：

```text
HLVX
```

可以按 execute permission 读取。

官方就是为这种 instruction emulation 场景设计 HLVX。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

---

# 48. PMP 在哪里？

H extension 并没有绕过：

```text
PMP
```

完整链条实际上是：

```text
Guest Virtual Address
        |
        | VS-stage
        v
Guest Physical Address
        |
        | G-stage
        v
Supervisor Physical Address
        |
        | PMP
        v
Physical Memory
```

所以：

```text
hgatp
```

控制 Guest memory isolation。

但是最终：

```text
M-mode PMP
```

仍然可以限制：

```text
HS
VS
VU
```

能访问哪些物理资源。

官方明确规定 PMP 作用于最终 supervisor physical address，并且无论 virtualization mode 如何都有效。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor?utm_source=chatgpt.com)

这对于你以后研究：

```text
RISC-V TEE
confidential computing
```

非常重要。

---

# 49. 一次完整 Guest App 内存访问

现在我们把整章串起来。

Guest application：

```text
VU
```

执行：

```asm
ld a0, 0x100(x5)
```

假设：

```text
x5 + 0x100
=
GVA 0x40001000
```

---

## Step 1：VS-stage

硬件：

```text
vsatp
```

找到 Guest Linux 的 page table：

```text
GVA
0x40001000

 ↓ VS-stage

GPA
0x90001000
```

---

## Step 2：G-stage

然后：

```text
hgatp
```

Hypervisor 的 page table：

```text
GPA
0x90001000

 ↓ G-stage

SPA
0x48001000
```

---

## Step 3：PMP

最后：

```text
PMP
```

确认该 physical access 是否允许。

---

## Step 4：真正访问

```text
RAM 0x48001000
```

所以完整：

```text
VU Load

GVA 0x40001000
       |
       | vsatp
       | VS-stage
       v
GPA 0x90001000
       |
       | hgatp
       | G-stage
       v
SPA 0x48001000
       |
       | PMP
       v
RAM
```

这张图就是 H extension MMU 部分的核心。

---

# 50. 再跑一次完整 Guest syscall

Guest App：

```text
VU
```

执行：

```asm
ecall
```

如果：

```text
medeleg
+
hedeleg
```

配置正确：

```text
VU
 |
 | ECALL from VU
 v
VS
```

Guest Linux：

```text
vscause
vsepc
vstval
```

处理 syscall。

Hypervisor：

```text
完全不知道这次 syscall 发生过
```

这才是高性能 virtualization。

---

# 51. 再跑一次 Guest SBI

Guest Linux：

```text
VS
```

执行：

```asm
ecall
```

这次：

```text
ECALL from VS
```

于是：

```text
VS
 |
 | ECALL
 v
HS
```

VM Exit。

HS：

```text
scause = ECALL from VS
```

然后 Hypervisor：

```text
decode SBI extension/function
```

例如：

```text
timer
IPI
HSM
RFENCE
...
```

进行虚拟化处理。

必要时：

```text
HS
 |
 | SBI ECALL
 v
M / OpenSBI
```

完成以后：

```text
HS
 |
 | SRET
 v
VS
```

Guest 继续执行。

---

# 52. 再跑一次 G-stage fault

Guest：

```text
VU
```

访问：

```text
GVA
```

VS-stage：

```text
GVA → GPA
```

成功。

但：

```text
hgatp
```

无 GPA 映射。

发生：

```text
Load Guest-Page Fault
```

于是：

```text
VU
 |
 | G-stage fault
 v
HS
```

此时：

```text
scause
=
Load Guest-Page Fault

stval
=
Guest Virtual Address

htval
≈
Guest Physical Address >> 2
```

Hypervisor 可以判断：

```text
是不是 lazy memory allocation？

是不是 MMIO？

是不是非法 Guest access？

是不是 memory ballooning？

是不是需要建立 G-stage mapping？
```

处理后：

```text
update G-stage page table

HFENCE.GVMA

SRET
```

Guest 重试。

这几乎就是你理解 Arm Stage-2 fault handler 的方式。

---

# 53. Type-1 和 Type-2 Hypervisor

H extension 两者都支持。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/hypervisor.html?utm_source=chatgpt.com)

## Type-1

```text
M
OpenSBI
  |
  v
HS
Hypervisor
  |
  +----------+
  |          |
 VS         VS
Guest A    Guest B
```

Hypervisor 本身直接控制 VM。

---

## Type-2

典型：

```text
M
OpenSBI
 |
 v
HS
Linux Host + KVM
 |
 +-------------------+
 |                   |
 U                   VS
QEMU              Guest Linux
                     |
                     v
                     VU
                  Guest App
```

这里：

```text
Linux Host kernel + KVM
```

运行 HS。

而：

```text
QEMU
```

只是 Host 的：

```text
U-mode process
```

这和 x86/Linux KVM、Arm KVM 的总体软件结构是一样的。

---

# 54. 为什么 VU 存在？

这个问题其实很简单。

如果只有：

```text
VS
```

Guest OS 可以跑。

但 Guest OS 自己还要运行：

```text
userspace
```

因此必须给 Guest 一个：

```text
Virtual User
```

所以：

```text
Guest Linux
    VS
     |
     v
Guest Process
    VU
```

本质就是：

```text
Guest EL1
Guest EL0
```

对应：

```text
VS
VU
```

---

# 55. 那为什么没有 VM-mode？

你会发现：

```text
VM
```

不存在。

也就是：

```text
V=1 + M
```

没有定义。

原因是 virtualization 的目标就是：

> **虚拟化 Supervisor execution environment。**

Guest OS 通常只应该看到：

```text
S/U
```

Machine mode：

```text
M
```

属于真正的 machine firmware / trusted execution environment。

不属于普通 VM 的 architecture。

因此：

```text
Guest M-mode
```

不是 H extension 默认提供的一层硬件 privilege。

如果你想做：

```text
nested virtualization
```

或者虚拟整个平台的 Machine firmware，则还需要 Hypervisor trap/emulation 等额外机制，而不是简单存在一个：

```text
VM
```

状态。

---

# 56. H Extension 与 Arm EL2 最大的思想差异

现在可以把问题提升到架构设计层面。

Arm 采用：

```text
EL3
 |
EL2
 |
EL1
 |
EL0
```

然后通过：

```text
EL2 controls guest EL1/EL0
```

建立虚拟化。

RISC-V 更接近：

```text
Privilege axis:

M
S
U

+

Virtualization axis:

V=0 / V=1
```

形成：

```text
(M,0)

(S,0) = HS
(U,0) = U

(S,1) = VS
(U,1) = VU
```

从抽象设计上看：

> **Arm 更偏 hierarchy extension；RISC-V H extension 更偏 orthogonal virtualization state。**

这个区别就是为什么：

> 用 EL2/EL1/EL0 机械理解 HS/VS/VU 总觉得“不完全对得上”。

你之前的感觉其实是正确的。

---

# 57. 一张最终架构图

现在建议把 H extension 记成这一张图：

```text
                         M-mode
                   ┌───────────────┐
                   │ OpenSBI / FW  │
                   └───────┬───────┘
                           │ SBI
                           v
                      HS-mode
             ┌────────────────────────┐
             │ Hypervisor / Host OS   │
             │                        │
             │ hstatus                │
             │ hedeleg / hideleg      │
             │ hvip / hip / hie       │
             │ hgatp                  │
             │ htval / htinst         │
             └──────────┬─────────────┘
                        │
                   SRET │   Trap
                        │
                  V = 1 │
                        v
                      VS-mode
             ┌────────────────────────┐
             │ Guest OS               │
             │                        │
             │ sstatus → vsstatus     │
             │ stvec   → vstvec       │
             │ sepc    → vsepc        │
             │ scause  → vscause      │
             │ satp    → vsatp        │
             └──────────┬─────────────┘
                        │
                       SRET
                        │
                        v
                      VU-mode
             ┌────────────────────────┐
             │ Guest Application      │
             └────────────────────────┘
```

Memory：

```text
             Guest Virtual Address
                      |
                      |
                   VS-stage
                    vsatp
                      |
                      v
             Guest Physical Address
                      |
                      |
                   G-stage
                    hgatp
                      |
                      v
          Supervisor Physical Address
                      |
                     PMP
                      |
                      v
                    DRAM
```

Trap：

```text
                   Trap
                    |
                    v
                    M
                    |
              medeleg/mideleg
                    |
                    v
                   HS
                    |
              hedeleg/hideleg
                    |
                    v
                   VS
```

这三张图基本就是整个 H extension 的骨架。

---

# 58. 你真正需要记住的核心寄存器

如果现在让我压缩到最小集合，我建议先只记：

```text
hstatus
    Hypervisor control/state

hedeleg
hideleg
    VS trap delegation

hgatp
    GPA → SPA

hvip
hip
hie
    virtual interrupt

htval
htinst
    VM-exit additional information
```

以及 VS bank：

```text
vsstatus
vstvec
vsepc
vscause
vstval
vsatp
vsie
vsip
```

先把这些搞透。

像：

```text
henvcfg
hcounteren
htimedelta
hgeip
hgeie
```

属于第二优先级。

---

# 59. 从 Staff Engineer 角度，我认为这一章至少要回答清楚这 8 个问题

1. **HS 为什么不是新的 privilege level？**
   
   因为 H extension 是在 S privilege 上增加 hypervisor capability，并用 V 状态区分 Host/Guest execution。

2. **VS 和 S 有什么关系？**
   
   VS 对 Guest 呈现 S-mode semantics，Guest 对很多 S CSR 的访问由硬件透明重定向到 VS CSR。

3. **为什么需要 `vsatp` 和 `hgatp` 两个寄存器？**
   
   `vsatp` 管 GVA→GPA，由 Guest 控制；`hgatp` 管 GPA→SPA，由 Hypervisor 控制。

4. **普通 Page Fault 和 Guest-Page Fault 有什么区别？**
   
   前者通常是 VS-stage，Guest OS 自己处理；后者是 G-stage，需要 HS Hypervisor 处理。

5. **Guest syscall 为什么不 VM exit？**
   
   VU 的 ECALL 可以通过 `hedeleg` 直接送到 VS。

6. **Guest SBI 为什么必须 VM exit？**
   
   VS 的 ECALL 表示向 Guest execution environment 请求服务，因此需要进入 HS。

7. **`HFENCE.VVMA` 和 `HFENCE.GVMA` 分别干什么？**
   
   前者管理 VS-stage translation，后者管理 G-stage translation。

8. **HS 如何读取 Guest Virtual Address？**
   
   使用 HLV/HLVX/HSV，让显式访问按照 VS/VU semantics 并经过 two-stage translation。

如果这 8 个问题你都能从原理上解释，而不是背答案，**H extension 的 architecture 主体已经掌握了。**

---

# 60. 最后把它浓缩成一句话

如果面试让我只用 30 秒描述 RISC-V H extension，我会这样回答：

> **RISC-V H extension 在原有 M/S/U privilege architecture 上增加 Virtualization Mode，使 S-mode 扩展成 HS，并提供 VS/VU 运行 Guest OS 和 Guest userspace。Guest 的 S-mode CSR 由硬件透明映射到 VS CSR；`vsatp` 实现 GVA→GPA 的 VS-stage translation，`hgatp` 实现 GPA→SPA 的 G-stage translation；`hedeleg/hideleg` 控制 Guest trap 是否留在 VS，`hvip` 等寄存器用于虚拟中断，而 guest-page fault、virtual instruction、Guest SBI 等事件会触发 VS/VU→HS 的 VM exit。**

这基本抓住了 H extension 的架构本质。

下一步如果继续按照我们这个系列的顺序，**第 11 阶段 AIA 会非常适合直接承接这一章的 virtual interrupt 部分**：重点就是把传统 `PLIC + H extension` 和现代 `APLIC + IMSIC + Guest Interrupt File` 串起来，你会真正看到 **RISC-V 如何解决“虚拟机外部中断为什么不应该频繁经过 Hypervisor”** 这个问题。
