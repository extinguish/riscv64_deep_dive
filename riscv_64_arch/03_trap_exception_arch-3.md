可以。下面给出完整答案。建议你不要把它当“参考答案背诵”，而是重点看每题后面的**设计本质**：Staff Engineer 的区别通常不在于知道 `sepc/scause/stval` 是什么，而在于能解释**为什么架构这样设计、软件和硬件的责任边界在哪里、异常场景下会怎么失效**。


# RISC-V Trap / Exception — Staff Engineer 测试题参考答案

---

# 1. Trap、Exception、Interrupt 三者是什么关系？

最核心的关系是：

```text
Exception
    = synchronous event

Interrupt
    = asynchronous event

Trap
    = CPU 因 Exception 或 Interrupt
      把控制流转移到 trap handler
```

因此：

```text
Exception ──┐
            ├──> Trap
Interrupt ──┘
```

具体来说：

```text
ECALL
    → Exception
    → synchronous

Load Page Fault
    → Exception
    → synchronous

Timer Interrupt
    → Interrupt
    → asynchronous
```

### 为什么 Exception 和 Interrupt 要区分？

因为它们与当前 instruction 的关系完全不同。

Exception：

```text
Instruction N
     |
     +--> Illegal Instruction
     +--> Page Fault
     +--> ECALL
```

发生原因与当前 instruction 直接相关。

Interrupt：

```text
Instruction stream
     |
     |         Timer expires
     |              |
     v              v
   normal        interrupt
```

并不是当前 instruction 导致的。

但是两者一旦决定“需要 CPU 停止当前执行流”，后面的动作高度一致：

```text
保存 PC
保存 cause
保存 privilege state
选择 handler
跳到 handler
最终返回
```

所以 RISC-V 统一使用：

```text
xepc
xcause
xtvec
xstatus
xRET
```

### 本质

Exception / Interrupt 描述：

> **事件来源。**

Trap 描述：

> **控制权转移机制。**

因此不要把：

```text
Trap == Exception
```

理解成同义词。

---

# 2. 为什么 Trap 不是“错误处理机制”？

因为很多 Trap 根本不是错误。

例如：

```text
ECALL
```

是程序主动请求 OS 服务。

Timer interrupt：

```text
不是程序错误
```

Demand paging：

```text
Page Fault
```

甚至可能是正常 OS 行为。

所以 Trap 更准确的定义是：

> CPU 暂停当前 execution context，把控制权交给一个更适合处理当前事件的软件 execution environment。

例如：

```text
U-mode
   |
   | ECALL
   v
S-mode kernel
```

表示：

```text
Application
    ↓
请求 OS service
```

而：

```text
U-mode
   |
   | Page Fault
   v
S-mode
```

表示：

```text
Application
    ↓
OS 需要修复当前 execution environment
```

Timer interrupt：

```text
running task
    ↓
kernel scheduler
```

### 本质

Trap 是：

```text
execution-control transfer primitive
```

而不是：

```text
error handler
```

这点非常重要，因为后面：

```text
syscall
page fault
interrupt
virtualization
instruction emulation
```

其实都是建立在 Trap mechanism 上。

---

# 3. 完整解释一次 U → S Load Page Fault

假设：

```text
当前 privilege = U

Load Page Fault

medeleg[13] = 1
```

CPU 首先执行某条：

```asm
ld a0, 0(a1)
```

地址转换失败。

---

## Step 1：产生 Exception

CPU 确认：

```text
cause = Load Page Fault
```

对应：

```text
cause code = 13
```

因为是 Exception：

```text
scause.interrupt = 0
```

---

## Step 2：Trap routing

因为当前：

```text
U-mode
```

然后检查：

```text
medeleg[13]
```

发现：

```text
= 1
```

所以：

```text
Trap target = S-mode
```

而不是 M-mode。

---

## Step 3：保存 PC

CPU：

```text
sepc ← faulting instruction PC
```

注意：

这里通常是：

```text
ld
```

这条 instruction 的地址。

不是：

```text
下一条 instruction
```

---

## Step 4：保存 cause

```text
scause = 13
```

---

## Step 5：保存附加 fault 信息

对于 Load Page Fault：

```text
stval ← faulting virtual address
```

例如：

```text
a1 = 0x40001234
```

那么：

```text
stval = 0x40001234
```

---

## Step 6：保存 previous privilege

`sstatus.SPP`：

```text
SPP ← 0
```

因为之前在 U-mode。

意义：

```text
SRET 时应该返回 U-mode
```

---

## Step 7：保存 interrupt enable state

Trap entry：

```text
SPIE ← SIE
SIE  ← 0
```

也就是：

```text
旧 SIE
 ↓
SPIE

当前 SIE
 ↓
0
```

原因是：

刚进入 handler 时避免同一 privilege 的 interrupt 立即再次进入。

---

## Step 8：切 privilege

```text
privilege ← S
```

---

## Step 9：设置 PC

如果：

```text
stvec.MODE = Direct
```

则：

```text
PC ← stvec.BASE
```

现在 CPU 开始执行 S-mode trap entry。

---

完整流程：

```text
U-mode

ld a0, 0(a1)
      |
      v
Load Page Fault
      |
      v
check medeleg[13]
      |
      v
target = S

-------------------------

sepc   ← ld PC
scause ← 13
stval  ← fault VA

SPP    ← U
SPIE   ← SIE
SIE    ← 0

privilege ← S

PC ← stvec
```

### 本质

Hardware Trap Entry 的职责非常有限：

> **保存最小架构状态 + 改变 privilege + 转移 PC。**

它不会：

```text
保存全部 GPR
切换 kernel stack
处理 page fault
建立 PTE
调度进程
```

这些全部属于 software。

---

# 4. CPU 会自动保存整个 context 吗？

不会。

这是非常重要的边界。

Hardware 通常负责：

```text
xepc
xcause
xtval

xstatus 中的：
    previous privilege
    previous interrupt state
```

例如 S-mode：

```text
sepc
scause
stval
SPP
SPIE
SIE
```

但不会自动保存：

```text
ra
sp
gp
tp

a0-a7

t0-t6

s0-s11
```

这些 GPR 必须由 software 保存。

因此应该分成：

```text
Hardware Trap State
        +
Software Trap Frame
```

Hardware Trap State：

```text
sepc
scause
stval
sstatus 部分状态
```

Software Trap Frame：

```text
GPR
必要的 CSR snapshot
kernel metadata
```

### 为什么这是重要的？

因为：

```text
Trap Entry
```

和：

```text
Context Switch
```

并不是同一件事。

Trap 可能处理完成后直接返回：

```text
Task A
  |
Trap
  |
Task A
```

也可能最终触发 scheduler：

```text
Task A
  |
Trap
  |
Scheduler
  |
Task B
```

所以 hardware 不应该默认：

> 每个 Trap 都等价于 task context switch。

---

# 5. 为什么 hardware 不自动 push x1~x31？

乍看自动保存寄存器更方便：

```text
Trap
 ↓
HW push x1-x31
 ↓
handler
```

但这会带来很多问题。

## 1. 性能

很多 Trap 不需要保存全部寄存器。

例如某些非常短的 firmware handler 可能只需要：

```text
t0
t1
```

如果 hardware 每次强制保存：

```text
31 个 GPR
```

Trap latency 会显著增加。

---

## 2. Stack 到底是哪一个？

这是更本质的问题。

假设：

```text
U-mode sp = user stack
```

发生：

```text
U → S
```

hardware 如果自动 push GPR：

> Push 到哪里？

如果用当前 `sp`：

```text
push 到 user stack
```

这显然不安全。

如果自动切 kernel stack：

那 hardware 又必须知道：

```text
kernel stack address
```

于是 ISA 需要定义：

```text
kernel stack register
per-hart stack policy
user/kernel switching rules
```

硬件复杂度迅速扩大。

---

## 3. Software policy 不同

Linux、RTOS、hypervisor、bare-metal firmware：

```text
保存需求完全不同
```

例如 RTOS 可能需要：

```text
所有 GPR
```

Linux syscall fast path 可能采用另一种布局。

Hypervisor 又需要：

```text
virtualization state
```

ISA 不应该强制统一 software trapframe layout。

---

## 4. ABI 和 kernel implementation

Trap frame 是：

```text
software internal ABI
```

不同 OS 可以设计不同 layout。

如果 hardware 固定 layout：

```text
architecture
```

就会绑定 OS implementation。

---

## 5. Nested Trap

如果 hardware 自动 push：

```text
nested trap
```

还要定义：

```text
push stack
overflow
atomicity
fault during push
```

问题会越来越复杂。

### 本质

RISC-V 选择的是：

```text
minimal architectural mechanism
+
maximum software flexibility
```

Hardware：

```text
保存必要的 control state
```

Software：

```text
决定保存多少 execution state
```

这是典型的 RISC-V 设计哲学。

---

# 6. xepc 是 faulting instruction 还是 next instruction？

不能简单记：

```text
xepc = 当前 instruction
```

或者：

```text
xepc = 下一条
```

更准确是：

> 保存发生 Trap 时需要恢复 execution 的 PC。

---

## ECALL

例如：

```text
0x1000 add
0x1004 ecall
0x1008 add
```

ECALL：

```text
sepc = 0x1004
```

也就是 ECALL 本身。

之后 software：

```text
sepc += 4
```

再返回。

---

## Load Page Fault

```text
0x2000 ld a0, 0(a1)
```

Page Fault：

```text
sepc = 0x2000
```

因为 OS 修复 mapping 后需要：

```text
重新执行 LD
```

---

## Timer Interrupt

Interrupt 不是某条 instruction 的 fault。

因此：

```text
sepc
```

表示：

> 被 interrupt 打断的 execution point。

从 software 角度理解：

```text
SRET 后继续原来的 execution
```

即可。

### 本质

`xepc` 的意义不是：

> “永远是哪种 PC”。

而是：

> **Trap return continuation point。**

software 可以根据 Trap 语义修改它。

---

# 7. ECALL 为什么通常需要 sepc += 4？

例如：

```asm
ecall
addi a0, a0, 1
```

Trap：

```text
sepc = ECALL PC
```

如果 kernel：

```text
handle syscall
 ↓
SRET
```

而不修改 `sepc`：

```text
PC ← sepc
```

那么再次执行：

```text
ecall
```

结果：

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
```

无限循环。

所以 syscall handler：

```text
sepc += 4
```

使：

```text
SRET
 ↓
instruction after ECALL
```

### 为什么是 4？

`ECALL` 是标准 32-bit instruction。

即使系统支持 C extension：

```text
ECALL 本身仍然是 32-bit
```

因此：

```text
+4
```

是正确的。

### 本质

ECALL 是：

```text
software-requested control transfer
```

它执行一次以后：

```text
不应该重新执行
```

所以 handler 必须 advance PC。

---

# 8. Page Fault 为什么不能 sepc += 4？

代码：

```asm
ld a0, 0(a1)
```

假设 page 不存在。

OS：

```text
allocate page
update PTE
```

这个 Page Fault 的目标是：

> 让原来的 LD 最终成功执行。

所以：

```text
sepc
```

必须继续指向：

```text
LD
```

然后：

```text
SRET
```

重新执行。

如果错误地：

```text
sepc += 4
```

那么 LD 被完全跳过：

```text
a0
```

没有获得应该加载的数据。

程序却继续执行：

```text
下一条 instruction
```

这会造成非常隐蔽的：

```text
architectural state corruption
```

因为 CPU execution 从软件视角已经不满足：

```text
program semantics
```

### 本质

两类 Trap 的 recovery semantics 不同：

```text
ECALL
    → consume instruction
    → advance

Page Fault
    → repair execution environment
    → retry instruction
```

这也是为什么：

> `xepc` 是机制；是否修改 `xepc` 是 policy。

---

# 9. stval 是否永远是 faulting VA？

不是。

`stval` 更准确的定义是：

> Trap-specific auxiliary value。

---

## Load Page Fault

通常：

```text
stval = faulting VA
```

---

## Store Page Fault

通常：

```text
stval = faulting VA
```

---

## Instruction Page Fault

通常：

```text
stval = faulting instruction address
```

---

## Illegal Instruction

某些实现可以提供：

```text
faulting instruction bits
```

用于：

```text
software decode / emulation
```

但不能假定所有 implementation 都一定如此。

---

## ECALL

通常：

```text
stval = 0
```

因为没有额外 fault address。

---

## Breakpoint

根据具体 architecture semantics：

```text
stval
```

可能包含相关地址信息，但不能把：

```text
所有 breakpoint
```

都简单理解成固定某一种值。

### 本质

因此应该这样理解：

```text
xcause
    = category

xepc
    = execution location

xtval
    = cause-specific details
```

而不是：

```text
xtval = address register
```

---

# 10. 为什么同时需要 scause 和 stval？

假设只有：

```text
scause = Load Page Fault
```

那么 kernel 知道：

```text
发生了 load page fault
```

但不知道：

```text
哪个 VA？
```

为了找到 faulting VA，软件可能不得不：

```text
decode LD instruction
读取 base register
计算 effective address
```

甚至对于复杂 instruction：

```text
非常麻烦
```

所以 `stval` 提供：

```text
fault-specific detail
```

---

反过来，如果只有：

```text
stval = 0x40001000
```

kernel 不知道：

```text
Instruction Page Fault?
Load Page Fault?
Store Page Fault?
Access Fault?
```

处理 policy 完全不同。

所以：

```text
scause
    = What happened?

stval
    = What object/address/details?
```

### 本质

这是：

```text
classification
+
context
```

两个不同维度。

---

# 11. Page Fault 与 Access Fault

这是非常关键的一题。

可以建立两层模型：

```text
VA
 |
 v
Virtual Memory Translation
 |
 v
PA
 |
 v
Physical Access Control/System
```

---

## 1. PTE.V = 0

Translation 无效：

```text
Page Fault
```

如果是 load：

```text
Load Page Fault
```

---

## 2. User 访问 supervisor-only page

PTE permission 检查失败：

```text
Load/Store/Instruction Page Fault
```

取决于 access 类型。

---

## 3. Translation 成功，但 PMP 拒绝

已经获得：

```text
PA
```

但是 physical protection 失败：

```text
Access Fault
```

例如：

```text
Load Access Fault
```

---

## 4. MMIO 地址不存在

如果 physical access 无法完成：

```text
Access Fault
```

而不是 page fault。

---

## 5. instruction fetch 但 PTE.X = 0

虚拟内存 execute permission 不允许：

```text
Instruction Page Fault
```

### 本质

Page Fault：

```text
virtual address translation / virtual permission
```

Access Fault：

```text
physical access / protection / platform
```

所以：

```text
Page Fault ≈ MMU domain

Access Fault ≈ physical system domain
```

虽然具体实现细节还涉及 PMA/PMP 等。

---

# 12. 为什么必须区分 Page Fault 和 Access Fault？

假设所有 memory error 都叫：

```text
Memory Fault
```

OS 就无法知道：

```text
这是可恢复的虚拟内存问题

还是

底层 physical protection/system error
```

例如：

```text
PTE missing
```

OS 可以：

```text
allocate page
update page table
retry
```

这是正常 demand paging。

但：

```text
PMP deny
```

可能代表：

```text
S-mode 根本没有权限访问这块 physical memory
```

此时 S-mode：

```text
建立 PTE
```

毫无意义。

### 从 M/S responsibility 看

M-mode firmware 可能控制：

```text
physical security boundary
PMP
```

S-mode OS 控制：

```text
virtual memory
page table
```

因此架构区分：

```text
Page Fault
Access Fault
```

实际上是在帮助建立：

```text
OS virtual-memory policy
           vs
machine physical-resource policy
```

### 本质

这不是单纯为了“错误码更多”。

而是：

> **不同 fault 属于不同 protection layer，也可能属于不同 privilege owner。**

---

# 13. 一次 Load 可能在哪些阶段失败？

完整模型：

```text
LD
 |
 v
Effective Virtual Address
 |
 +-- address alignment
 |
 v
Address Translation
 |
 +-- page table validity
 |
 +-- PTE permission
 |
 v
Physical Address
 |
 +-- PMP
 |
 +-- PMA/platform
 |
 v
Memory / MMIO
```

可能的 fault：

```text
misaligned
    → Load Address Misaligned

translation missing
    → Load Page Fault

PTE.R denied
    → Load Page Fault

U/S permission denied
    → Load Page Fault

PMP denied
    → Load Access Fault

unsupported physical region
    → Load Access Fault
```

### 一个 Staff-level 关键点

不要想当然认为：

```text
CPU 一定先完整 page walk，
然后才检查其他所有东西。
```

Architecture 对某些 fault priority 和检查 ordering 有具体规则或实现自由度。

Software 应依赖：

```text
architecturally visible exception semantics
```

而不是假设内部 microarchitecture pipeline 顺序。

---

# 14. medeleg 的本质是什么？

表面：

```text
Machine Exception Delegation Register
```

真正意义：

> **定义 M-mode 与 S-mode 对 Exception 的 ownership boundary。**

例如：

```text
M-mode
    OpenSBI

S-mode
    Linux
```

M-mode 可以说：

```text
Page Fault
ECALL from U
某些 Illegal Instruction
```

这些 exception：

```text
Linux 自己处理
```

不用每次经过 firmware。

因此：

```text
medeleg
```

并不是简单的 routing table。

它实际上表达：

```text
哪些 architectural events
属于 OS responsibility

哪些
属于 machine firmware responsibility
```

例如 Page Fault：

```text
Linux 管 page table
```

自然应该：

```text
U → S
```

而不是：

```text
U → M → S
```

否则：

```text
每个 page fault
```

都经过 OpenSBI，性能和架构分层都很差。

### 本质

`medeleg` 是：

```text
privilege responsibility partition
```

而不仅仅是：

```text
exception redirect register
```

---

# 15. medeleg[13]=1 后三个场景如何？

假设：

```text
Load Page Fault
cause = 13

medeleg[13] = 1
```

---

## U-mode Load Page Fault

因为：

```text
U < S
```

而 exception delegated：

```text
U → S
```

这是：

```text
vertical trap
```

因为 privilege 提升。

---

## S-mode Load Page Fault

Trap 已 delegated 给 S。

当前本身就在 S：

```text
S → S
```

这叫：

```text
horizontal trap
```

例如 kernel 自己产生 page fault。

---

## M-mode Load Page Fault

即使：

```text
medeleg[13] = 1
```

也不会：

```text
M → S
```

而是：

```text
M → M
```

因为：

> Trap 不会从更高 privilege 降到更低 privilege。

---

### Vertical / Horizontal 的本质

Vertical：

```text
less privileged
    ↓
more privileged
```

例如：

```text
U → S
S → M
```

Horizontal：

```text
same privilege
```

例如：

```text
S → S
M → M
```

---

# 16. medeleg[2]=1，M-mode illegal instruction 会不会进 S？

不会。

M-mode：

```text
Illegal Instruction
```

仍然：

```text
M → M
```

`medeleg` 的意思不是：

> 把 machine exception handler 替换成 S-mode。

而是：

> 对来自低 privilege 的 exception，允许 S-mode 成为 handler。

### 为什么不能 M → S？

因为这会破坏 privilege hierarchy。

假设：

```text
M-mode
```

拥有最高权限。

它因为某个 fault：

```text
自动进入 S-mode
```

那么：

```text
M-mode execution state
```

就会暴露给更低 privilege software。

更严重的是：

S-mode handler 可能没有能力访问：

```text
M-mode CSR
machine resources
```

所以无法可靠恢复。

### 本质

Privilege Trap transition 的安全原则是：

```text
same privilege
or
higher privilege
```

绝不能：

```text
lower privilege
```

这是整个 privilege architecture 的基本不变量。

---

# 17. 为什么采用 cause-based delegation？

RISC-V 的方式非常直接：

```text
medeleg
mideleg
```

按 cause 控制：

```text
哪些 Exception
哪些 Interrupt
```

允许 S-mode 处理。

这带来一个非常清晰的模型：

```text
Cause
  ↓
Owner
```

例如：

```text
Page Fault
    → S

Machine-critical event
    → M
```

Arm 的 exception routing 更紧密地与：

```text
EL
Secure state
HCR_EL2
SCR_EL3
interrupt routing
```

结合。

RISC-V 则把 policy 显式化：

```text
Machine layer
    ↓
delegation bitmap
    ↓
Supervisor layer
```

### 本质区别

Arm 更像：

```text
系统状态 + 控制寄存器
共同决定 routing
```

RISC-V 更强调：

```text
event-class ownership
```

这和 RISC-V 的整体哲学一致：

```text
机制简单
policy 显式
```

不过不能因此简单说：

```text
RISC-V 一定比 Arm 简单
```

进入 H extension 后：

```text
medeleg
mideleg
hedeleg
hideleg
```

routing 也会变复杂。

---

# 18. OpenSBI 应该 delegate 哪些 Exception？

重点不是死记 bitmask，而是看：

> 谁真正拥有处理这个事件所需的 policy 和资源？

---

## Page Fault

通常：

```text
应该给 S-mode OS
```

因为：

```text
Linux 管 virtual memory
page table
process address space
demand paging
```

M-mode firmware 并不知道：

```text
这个进程是什么
这个 VMA 是什么
是否应该 allocate page
```

所以：

```text
Page Fault → S
```

最合理。

---

## ECALL from U

通常：

```text
应该由 S-mode OS 处理
```

因为这是：

```text
userspace syscall
```

Linux 才知道：

```text
syscall ABI
process
fd
scheduler
```

---

## Illegal Instruction

这个比较有意思。

可以有两种 policy：

```text
S-mode OS 处理
```

例如：

```text
用户进程执行非法 instruction
OS 发 signal
```

或者：

某些 firmware 想保留特定 emulation。

所以：

```text
取决于 ownership
```

---

## PMP-related Access Fault

这是最值得分析的。

即使 architecture 允许某些 access fault delegation：

如果 fault 的真正原因是：

```text
PMP policy
```

而 PMP 由 M-mode 管：

```text
S-mode 无法修复
```

那么 M-mode 保留它往往更合理。

否则 Linux 收到：

```text
Load Access Fault
```

却没有权限改变 PMP。

### Staff-level 判断原则

不要按：

```text
异常名字
```

决定 ownership。

应该问：

```text
谁能诊断？
谁能修复？
谁拥有 policy？
谁拥有 resource？
```

也就是说：

```text
Trap delegation
    ≈
responsibility delegation
```

---

# 19. Direct 与 Vectored Mode

假设：

```text
stvec.BASE = 0x80001000
MODE = Vectored
```

---

## Load Page Fault

Exception：

```text
PC = BASE
```

所以：

```text
0x80001000
```

---

## ECALL from U

也是 Exception：

```text
PC = 0x80001000
```

---

## Supervisor Timer Interrupt

Supervisor Timer Interrupt：

```text
cause = 5
```

Vectored：

```text
PC = BASE + 4 × cause
```

所以：

```text
0x80001000 + 20
= 0x80001014
```

### 为什么 Exception 不 vector？

因为 RISC-V Vectored Mode 本质上是：

```text
interrupt vectoring
```

而不是：

```text
all trap vectoring
```

因此：

```text
Exception → BASE

Interrupt → BASE + 4*cause
```

这是必须记住的。

---

# 20. 为什么主要对 Interrupt vectoring？

Interrupt 的一个核心诉求是：

```text
low latency
```

例如：

```text
timer
IPI
external interrupt
```

如果全部先：

```text
进入 common handler
读取 scause
switch
跳到具体 ISR
```

会增加 latency。

Vectored interrupt：

```text
interrupt
   ↓
直接进入特定入口
```

可以减少 dispatch cost。

---

Exception 一般不同。

例如：

```text
Page Fault
Illegal Instruction
ECALL
```

通常都需要：

```text
读取 scause
读取 stval
访问 process state
复杂 software dispatch
```

它们不像低延迟 interrupt 那么需要：

```text
直接硬件 vector
```

而且 Exception 是同步的：

```text
当前 instruction 已经停下来
```

没有典型 hardware IRQ 的实时 dispatch 压力。

### 本质

Vectored mode 优化的是：

```text
asynchronous event dispatch latency
```

而不是为了让所有 Trap 都“看起来像 vector table”。

---

# 21. 为什么 xtvec 不能通常直接指向普通 C 函数？

刚进入 trap：

```text
execution environment
```

并不满足 C ABI。

例如：

```text
sp 可能还是 user stack

GPR 尚未保存

gp 可能不是 kernel gp

tp 可能仍然代表 user/thread context

callee-saved register 还没有建立 trapframe
```

如果直接：

```text
call C
```

C compiler 会默认：

```text
stack valid
ABI valid
register convention valid
```

这些假设可能全错。

因此一般：

```text
xtvec
  ↓
assembly entry
  ↓
establish safe context
  ↓
save registers
  ↓
C handler
```

Assembly entry 的职责：

```text
switch stack
save execution context
set kernel environment
build trap frame
```

C handler：

```text
policy
dispatch
scheduler
VM
syscall
```

### 本质

Assembly 负责：

```text
建立 ABI
```

C 负责：

```text
使用 ABI
```

---

# 22. sscratch 是解决什么问题的？

典型场景：

```text
U-mode:

sp = user_sp
```

突然：

```text
U → S Trap
```

CPU：

```text
不会自动切 kernel stack
```

但 trap handler 不能安全使用：

```text
user_sp
```

因为：

```text
user 可以伪造
user stack 可能 unmapped
user stack 可能 overflow
```

所以需要某个安全 anchor。

这就是：

```text
sscratch
```

的典型用途。

例如进入 userspace 前：

```text
sscratch = kernel_sp
```

Trap：

```asm
csrrw sp, sscratch, sp
```

得到：

```text
sp       = kernel_sp
sscratch = user_sp
```

现在 handler 有：

```text
trusted kernel stack
```

### 为什么 architecture 不规定 sscratch 必须放 kernel_sp？

因为不同 OS 可能想放：

```text
per-hart structure

trapframe pointer

kernel context pointer

CPU-local data
```

所以 hardware 只提供：

```text
scratch CSR
```

不定义 policy。

### 本质

`sscratch` 是：

> **privilege-level software 在 Trap Entry 最早阶段可以依赖的一个硬件保存锚点。**

---

# 23. `csrrw sp, sscratch, sp`

执行前：

```text
sp       = user_sp
sscratch = kernel_sp
```

`CSRRW`：

```text
rd ← CSR old value
CSR ← rs1
```

因此：

```asm
csrrw sp, sscratch, sp
```

之后：

```text
sp       = kernel_sp
sscratch = user_sp
```

它非常适合做：

```text
atomic swap
```

而且只需要一条 instruction。

---

## 但是从 S-mode Trap 时还能无条件这么做吗？

不能简单无条件执行。

假设当前本来就在 kernel：

```text
sp = kernel_sp
```

而：

```text
sscratch
```

保存的可能仍然是某个 user context。

如果无条件交换：

```text
sp
```

反而被换成：

```text
user_sp
```

kernel trap handler 就坏了。

因此 kernel 必须区分：

```text
Trap from U
```

和：

```text
Trap from S
```

可能方案包括：

```text
检查 sstatus.SPP

维护特殊 sscratch convention

不同 stvec entry

进入 kernel 后把 sscratch 设为 0
```

例如某些 OS 使用：

```text
sscratch == 0
```

表示：

```text
当前已经在 kernel
```

具体 implementation 可以不同。

### 本质

`sscratch` 是 mechanism。

如何表示：

```text
user/kernel entry state
```

是 software protocol。

---

# 24. Trap Frame 应该保存什么？

没有唯一正确答案。

必须根据：

```text
是否可能 schedule
是否支持 signal
是否支持 nested trap
是否支持 debugging
```

决定。

---

## GPR

如果 Trap 可能：

```text
context switch
```

通常需要保存：

```text
完整 architectural GPR state
```

这样以后才能恢复 process。

---

## sepc

通常必须保存。

因为：

```text
process resume PC
```

依赖它。

---

## sstatus

通常也很重要。

因为里面包含：

```text
previous privilege
interrupt state
以及其他 execution state
```

尤其 context switch / signal return 时需要。

---

## scause

是否必须长期保存：

```text
不一定
```

如果只是：

```text
进入 handler
读取 cause
处理完
```

可能不用放进长期 task context。

但对于：

```text
debugging
crash dump
nested traps
signal
```

保存非常有价值。

---

## stval

同理。

处理 page fault 时：

```text
马上读取即可
```

但：

```text
fault reporting
nested trap
debug
```

情况下保存可能很重要。

### Staff-level 本质

要区分：

```text
Architectural resume state
```

与：

```text
Diagnostic trap state
```

前者：

```text
GPR
PC
status
```

通常必须恢复。

后者：

```text
cause
tval
```

是否进入 persistent context 是 OS policy。

---

# 25. Nested Trap 为什么危险？

第一次 Trap：

```text
Trap #1
 ↓
sepc   = PC1
scause = Cause1
stval  = Val1
```

handler 刚进入：

```text
还没把 CSR 保存到 stack
```

第二次 Trap：

```text
Trap #2
```

hardware 会重新写：

```text
sepc   = PC2
scause = Cause2
stval  = Val2
```

那么：

```text
PC1
Cause1
Val1
```

可能完全丢失。

这是最直接的问题。

---

更麻烦的是：

```text
SPP
SPIE
```

previous-state fields 也会被新的 Trap 更新。

于是第一次 Trap 的：

```text
return context
```

可能被破坏。

### 怎么降低风险？

Trap entry 的最前面通常：

```text
interrupt disabled
```

并且尽快：

```text
建立 safe stack
保存 critical CSR
保存 GPR
建立 trapframe
```

然后才考虑：

```text
reenable interrupts
```

### 本质

Hardware CSR：

```text
不是一个 arbitrary-depth trap stack
```

而只是：

```text
当前 privilege 的一组 trap register
```

所以 nested trap management 是：

```text
software responsibility
```

---

# 26. 为什么 previous-state 不是完整 exception stack？

例如：

```text
SIE
SPIE
SPP
```

只有：

```text
current
previous
```

一层。

第一次 Trap：

```text
SIE → SPIE
SPP ← old privilege
```

第二次 Trap：

```text
SPIE
SPP
```

又会被覆盖。

没有：

```text
SPPIE
SPP2
SPP3
```

这种任意深度状态。

因此：

```text
Trap
 ↓
Trap
 ↓
Trap
```

如果 software 不保存：

```text
sstatus
sepc
```

信息会丢失。

### 为什么 architecture 不设计多层硬件 stack？

因为最大 nesting depth：

```text
无法合理固定
```

而且不同 OS policy 不同。

如果 hardware 实现：

```text
8-level exception stack
```

那第 9 层怎么办？

最终仍然需要软件。

所以 RISC-V 的选择是：

```text
one architectural level
+
software stack
```

### 本质

Hardware 提供：

```text
primitive state transition
```

Software 提供：

```text
arbitrary nesting abstraction
```

---

# 27. Trap Entry critical window 是什么？

Critical window 大致是：

```text
hardware 已经进入 handler
        ↓
software 尚未把关键 trap state
安全复制到自己的 memory stack
```

例如：

```text
PC ← stvec

但还没有保存：

sepc
sstatus
GPR
```

此时如果发生：

```text
新的 Trap
```

可能覆盖：

```text
第一次 Trap state
```

---

## 什么时候可以重新打开 interrupt？

没有一个 ISA 固定答案。

通常至少要满足：

```text
safe kernel stack 已建立

当前 execution state 已保存

critical trap CSRs 已保存

nested trap software protocol 已建立
```

才能考虑：

```text
SIE = 1
```

但是否允许 nested interrupts：

```text
完全属于 OS design
```

某些 RTOS：

```text
尽早打开高优先级 interrupt
```

以降低 interrupt latency。

某些 kernel：

```text
较晚才打开
```

简化 consistency。

### Staff-level 本质

Architecture 只提供：

```text
SIE
SPIE
Trap CSR
```

并不规定：

```text
OS 第几条 instruction 必须 enable interrupt
```

这就是：

```text
mechanism vs policy
```

最典型的例子。

---

# 28. 设计一个最小 S-mode Trap Handler

整体结构：

```text
stvec
 ↓
assembly entry
 ↓
safe stack
 ↓
save GPR
 ↓
save sepc/sstatus
 ↓
read scause/stval
 ↓
C dispatcher
 ↓
restore
 ↓
SRET
```

---

## Assembly Entry

概念：

```asm
trap_entry:

    /* establish kernel stack */

    addi sp, sp, -TRAPFRAME_SIZE

    sd ra, ...
    sd gp, ...
    sd tp, ...

    sd t0, ...
    sd t1, ...

    sd a0, ...
    ...

    csrr t0, sepc
    sd   t0, TF_SEPC(sp)

    csrr t0, sstatus
    sd   t0, TF_SSTATUS(sp)

    csrr a0, scause
    csrr a1, stval

    mv   a2, sp

    call trap_dispatch
```

---

## Dispatcher

概念：

```c
switch (scause)
{
case 8:
    syscall();
    tf->sepc += 4;
    break;

case 13:
    handle_load_page_fault(stval);
    break;

case 2:
    handle_illegal_instruction();
    break;
}
```

---

## ECALL

处理完成：

```text
sepc += 4
```

因为不能重新执行 ECALL。

---

## Load Page Fault

如果修复成功：

```text
sepc 保持不变
```

让：

```text
LD retry
```

如果无法修复：

```text
terminate process
signal
```

---

## Illegal Instruction

有两种常见情况。

不能处理：

```text
kill/signal
```

可以 software emulate：

```text
decode instruction
emulate effect
advance sepc by instruction length
```

注意这里：

```text
不能永远简单 +4
```

因为如果未来考虑 compressed instruction：

```text
instruction length
```

需要正确 decode。

不过标准 illegal instruction emulation 的具体处理取决于实现。

---

# 29. ECALL 不断重复，问题在哪里？

日志：

```text
scause = 8
sepc = 0x40001000

return

scause = 8
sepc = 0x40001000
```

最明显结论：

```text
ECALL instruction 被反复重新执行
```

最可能原因：

```text
syscall handler 没有 advance sepc
```

即：

```text
sepc 仍然指向 ECALL
```

SRET：

```text
PC ← sepc
```

所以再次 ECALL。

---

## 如何验证？

在 handler 中打印：

```text
sepc before
sepc after
```

如果：

```text
before = 0x40001000
after  = 0x40001000
```

问题基本确认。

也可以反汇编：

```text
0x40001000
```

确认确实是：

```text
ecall
```

---

## 如何修复？

对于：

```text
ECALL from U
```

处理成功后：

```text
sepc += 4
```

或者：

```text
trapframe->sepc += 4
```

取决于软件如何管理 EPC。

### Staff-level 补充

还应该检查：

```text
handler 修改的是 CSR sepc
```

还是：

```text
软件 trapframe 里的 saved sepc
```

如果最后 restore：

```text
csrw sepc, trapframe->sepc
```

那么只修改 CSR：

```text
可能又被 restore 覆盖
```

这是实际 OS debug 中很常见的问题。

---

# 30. 综合题：U-mode Load Page Fault 到重新执行成功

系统：

```text
M-mode
    OpenSBI-like firmware

S-mode
    Linux-like OS

U-mode
    application
```

应用执行：

```asm
ld a0, 0(a1)
```

某个 page 尚未建立。

---

## 第一层：Architecture 做什么？

### 1. Detect fault

CPU page walker / translation logic 检测：

```text
PTE invalid
```

产生：

```text
Load Page Fault
```

---

### 2. Determine Trap target

M-mode 在 boot 时配置：

```text
medeleg[13] = 1
```

所以来自 U-mode 的：

```text
Load Page Fault
```

直接：

```text
U → S
```

---

### 3. Save architectural state

CPU：

```text
sepc
    ← faulting LD PC

scause
    ← Load Page Fault

stval
    ← faulting VA
```

然后：

```text
SPP  ← U
SPIE ← SIE
SIE  ← 0
```

接着：

```text
privilege ← S
PC ← stvec
```

到这里：

```text
architecture 的 hardware job
```

基本完成。

---

# 第二层：Firmware policy 做了什么？

OpenSBI-like firmware 在系统初始化阶段决定：

```text
哪些 exception delegate
```

例如：

```text
Page Fault → S
```

这是 firmware policy。

原因：

```text
Linux owns virtual memory
```

如果 firmware 不这么做：

```text
Page Fault
 ↓
M-mode firmware
```

那么 M-mode 还必须：

```text
转发给 Linux
```

效率和架构分层都更差。

Firmware 还可能配置：

```text
PMP
physical memory layout
machine timer
interrupt delegation
```

这些属于：

```text
machine execution environment
```

---

# 第三层：S-mode Assembly Trap Entry

CPU 到：

```text
stvec
```

第一段通常是 assembly。

做：

```text
establish kernel stack

save GPR

save sepc

save sstatus

read scause

read stval

build trapframe
```

现在：

```text
User execution context
```

已经安全保存在 kernel memory。

---

# 第四层：OS Trap Dispatcher

Kernel：

```text
scause = Load Page Fault
```

然后：

```text
fault VA = stval
```

查：

```text
current process
VMA
page table
```

决定：

```text
这个 fault 是合法 demand paging
```

还是：

```text
非法地址访问
```

---

# 第五层：Page Table Handling

如果合法：

```text
allocate physical page
```

然后：

```text
initialize page
```

接着更新：

```text
PTE
```

例如：

```text
V = 1
R = 1
U = 1
```

具体取决于 mapping。

---

# 第六层：TLB synchronization

如果旧 translation 可能存在：

```text
SFENCE.VMA
```

确保：

```text
旧 translation state
```

不会继续使用。

是否需要、采用哪种粒度：

```text
取决于具体 page-table update 情况
```

但从概念上：

```text
page table update
   ↓
translation synchronization
```

必须正确。

---

# 第七层：不要修改 sepc

这是关键。

因为：

```text
faulting LD
```

必须 retry。

所以：

```text
sepc 保持 faulting LD
```

---

# 第八层：恢复 execution state

Kernel：

```text
restore GPR

restore appropriate status

SRET
```

Hardware SRET：

```text
Privilege ← U

SIE ← SPIE

PC ← sepc
```

于是再次执行：

```asm
ld a0, 0(a1)
```

---

# 第九层：第二次 LD

这次：

```text
page table valid
```

所以：

```text
VA
 ↓
PTE
 ↓
PA
 ↓
memory
```

成功：

```text
a0 ← data
```

然后执行：

```text
next instruction
```

应用甚至可能：

```text
完全不知道发生过 page fault
```

---

# 整个过程画成一张图

```text
User Application
      |
      | LD
      v
Virtual Translation
      |
      X
PTE not present
      |
      v
Load Page Fault
      |
      | medeleg[13]
      v
S-mode Trap
      |
      +--> sepc = LD PC
      +--> scause = 13
      +--> stval = VA
      |
      v
stvec
      |
      v
Assembly Entry
      |
      +--> switch/establish kernel stack
      +--> save GPR
      +--> build trapframe
      |
      v
Kernel Page Fault Handler
      |
      +--> inspect VMA
      +--> allocate physical page
      +--> update PTE
      +--> SFENCE.VMA if needed
      |
      v
sepc unchanged
      |
      v
restore context
      |
      v
SRET
      |
      v
U-mode
      |
      v
Retry LD
      |
      v
Success
```

---

# 最重要的一问：哪些属于 Architecture，哪些属于 Firmware，哪些属于 OS？

这是整个章节最重要的分层。

## Architecture 定义

RISC-V architecture 定义：

```text
什么叫 Page Fault

什么时候产生对应 exception

sepc/scause/stval 的 architectural semantics

Trap routing mechanism

medeleg mechanism

stvec

SPP/SPIE/SIE

SRET

SFENCE.VMA semantics

PTE format
```

也就是说：

> Architecture 提供 mechanism。

---

## Firmware policy

M-mode firmware 决定：

```text
哪些 exception delegate

哪些 resources 给 S-mode

PMP 如何配置

machine environment 如何初始化
```

例如：

```text
medeleg[13] = 1
```

是：

```text
firmware policy
```

而：

```text
medeleg 这个 CSR 怎么工作
```

是：

```text
architecture
```

这两者一定要分清。

---

## OS policy

Linux 决定：

```text
这个 VA 是否合法

是否 demand allocate

分配哪个 physical page

PTE 设置什么权限

fault 是否转成 signal

何时重新 enable interrupt

trapframe layout

kernel stack layout

scheduler 是否运行
```

这些都不是 ISA 决定的。

---

# Staff Engineer 最重要的分层

整个系统可以压缩成：

```text
Architecture
    |
    | defines mechanism
    v

Firmware
    |
    | establishes machine policy
    v

OS
    |
    | implements process/system policy
    v

Application
```

例如：

```text
Architecture:
    提供 medeleg

Firmware:
    设置 medeleg[13]

Architecture:
    Page Fault 写 sepc/scause/stval

OS:
    决定 allocate page

Architecture:
    提供 SFENCE.VMA

OS:
    决定什么时候执行它

Architecture:
    提供 SRET

OS:
    决定何时以及返回哪个 task
```

这就是整个 RISC-V privileged architecture 最重要的思维方式之一：

> **ISA 定义 mechanism，privileged firmware/OS 定义 policy。**

---

# 最后：30 道题真正需要形成的 10 个核心认知

如果把前面全部答案进一步压缩，我认为真正需要留下的是：

```text
1.
Exception / Interrupt 是 event type，
Trap 是 control transfer。

2.
Trap Entry != context switch。

3.
Hardware 只保存 minimal architectural state，
完整 execution context 由 software 保存。

4.
xepc 是 continuation/recovery mechanism，
是否 advance 取决于 exception semantics。

5.
Page Fault 是 virtual-memory domain，
Access Fault 更接近 physical-access domain。

6.
medeleg/mideleg 本质上是
M/S responsibility partition。

7.
Trap 永远不会自动从 higher privilege
降到 lower privilege。

8.
sscratch 解决 early trap entry
缺少 trusted software context 的问题。

9.
xstatus previous state 不是 arbitrary-depth
hardware exception stack。

10.
Architecture 定义 mechanism，
Firmware/OS 定义 policy。
```

如果你能够把这 10 点不依赖背诵地解释出来，并且面对一个新的 Trap 场景能够自己推导：

```text
source privilege
    ↓
cause
    ↓
delegation
    ↓
target privilege
    ↓
which xepc/xcause/xtval
    ↓
which xtvec
    ↓
software trapframe
    ↓
repair / emulate / service
    ↓
modify xepc?
    ↓
xRET
```

那么我会认为你对 **RISC-V Trap / Exception 已经真正进入 Staff Engineer 级别的理解框架**，而不是停留在“知道 CSR 名字”的层面。

接下来如果继续巩固这一章，最有价值的是做一组 **“现场推导题”**：我给你 8～10 个复杂 Trap 场景，例如 `S-mode page fault`、`M-mode illegal instruction`、`U-mode ECALL + delegation`、`trap entry 中再次 fault`，你只根据 CSR 配置现场推导最终进入哪个 privilege、哪些 CSR 被改写、能否安全恢复。这个训练会比继续背知识点更接近 Staff 面试。