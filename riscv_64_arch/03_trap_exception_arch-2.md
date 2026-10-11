可以。下面这套题我不会做成“背 CSR 名字”的题，而是按照 **Staff Engineer** 的要求来设计：重点考察你是否真的理解 **Trap routing、hardware/software boundary、nested trap、fault recovery、privilege transition，以及 OS/firmware 架构设计**。

我建议答题时尽量遵守一个原则：**不仅说“是什么”，还要解释“为什么架构这样设计、如果设计错了会发生什么”。**

# RISC-V Trap / Exception — Staff Engineer 测试题

## Part A：核心架构模型

### 1. Trap、Exception、Interrupt 三者到底是什么关系？

请解释以下三个术语的本质区别：

```text
Trap
Exception
Interrupt
```

并回答：

```text
ECALL
Load Page Fault
Timer Interrupt
```

分别属于哪一种？

进一步解释为什么 RISC-V 要明确区分：

```text
exception
interrupt
```

但最终又让两者共享：

```text
xepc
xcause
xtvec
xRET
```

---

### 2. 为什么 RISC-V 把 Trap 作为“控制流机制”，而不是“错误处理机制”？

以下事件都会发生 Trap：

```text
ECALL
Breakpoint
Page Fault
Illegal Instruction
Timer Interrupt
External Interrupt
```

其中很多并不是“错误”。

请从 CPU architecture / execution environment 的角度解释：

> Trap 的真正设计目的是什么？

---

### 3. 请完整解释一次 `U → S` Trap Entry

假设：

```text
当前：U-mode

原因：
Load Page Fault

medeleg[13] = 1
```

请从 CPU 检测 fault 开始，一直描述到：

```text
PC = stvec
```

要求至少说明：

```text
sepc
scause
stval

SPP
SPIE
SIE

privilege
PC
```

分别发生什么变化。

---

### 4. Trap Entry 时，CPU 会自动保存哪些状态？

有人说：

> “RISC-V 发生异常后 CPU 会像 context switch 一样自动保存整个 CPU context。”

这个说法哪里有问题？

请明确区分：

```text
Hardware Trap State
```

和：

```text
Software Trap Frame
```

分别包含什么。

---

### 5. 为什么 RISC-V 不让 hardware 自动保存所有 GPR？

假设架构设计成：

```text
Trap发生
 ↓
hardware自动push x1~x31
```

看起来不是更方便吗？

请从以下角度分析：

```text
performance
implementation complexity
software policy
ABI
nested trap
stack selection
```

为什么 RISC-V 更倾向于 minimal hardware trap entry。

---

# Part B：`xepc / xcause / xtval`

### 6. `xepc` 到底保存 faulting instruction 还是 next instruction？

分别分析：

```text
ECALL
Load Page Fault
Timer Interrupt
```

发生 Trap 后：

```text
sepc
```

应该如何理解？

为什么不能简单记忆：

> `sepc = next PC`

---

### 7. 为什么处理 ECALL 时通常要修改 `sepc`？

代码：

```asm
li a7, SYS_write
ecall
addi a0, a0, 1
```

假设：

```text
sepc = address(ecall)
```

如果 syscall handler 最后直接执行：

```asm
sret
```

但没有修改 `sepc`，会发生什么？

为什么通常：

```text
sepc += 4
```

---

### 8. 为什么 Page Fault 通常不能 `sepc += 4`？

假设：

```asm
ld a0, 0(a1)
```

产生：

```text
Load Page Fault
```

OS 成功建立缺失的 mapping。

请解释：

```text
为什么 sepc 必须保持原来的 LD 地址？
```

如果错误地：

```text
sepc += 4
```

从 architectural correctness 的角度会造成什么后果？

---

### 9. `stval` 是否永远是 faulting virtual address？

分析以下情况：

```text
Load Page Fault
Store Page Fault
Illegal Instruction
ECALL
Breakpoint
```

分别讨论：

```text
stval
```

可能表达什么。

Staff level 的答案不能只回答：

> “stval 就是 fault address。”

---

### 10. 为什么需要同时存在 `scause` 和 `stval`？

假设只设计：

```text
scause
```

而没有：

```text
stval
```

OS 是否仍然可以处理 Page Fault？

反过来：

如果只有：

```text
stval
```

没有：

```text
scause
```

又有什么问题？

解释二者在异常诊断中的职责边界。

---

# Part C：Page Fault / Access Fault

### 11. Page Fault 和 Access Fault 的架构含义有什么区别？

下面分别可能产生什么 fault？

```text
1. PTE.V = 0

2. 用户访问 supervisor-only page

3. Translation 成功，
   但最终 physical address 被 PMP 拒绝

4. MMIO 地址不存在

5. Instruction fetch PTE.X = 0
```

不要只回答 cause 名称，还要解释：

```text
translation layer
```

与：

```text
physical access/protection layer
```

之间的区别。

---

### 12. 为什么 RISC-V 要把 Page Fault 和 Access Fault 分开？

假设架构只提供一种：

```text
Memory Fault
```

会有什么问题？

请从：

```text
M-mode firmware
S-mode OS
virtual memory
PMP
fault delegation
```

的责任划分角度回答。

---

### 13. 一个 Load 到底可能在哪些阶段失败？

考虑：

```asm
ld a0, 0(a1)
```

请画出或描述完整路径：

```text
VA
 ↓
address translation
 ↓
PTE permission
 ↓
PA
 ↓
PMP/PMA
 ↓
memory system
```

并说明每一个阶段可能对应什么异常。

---

# Part D：Delegation

### 14. `medeleg` 的本质是什么？

不要只回答：

> Machine Exception Delegation Register。

请解释：

```text
medeleg
```

实际上是在划分哪两层软件之间的责任？

例如：

```text
OpenSBI
Linux
```

为什么 Linux Page Fault 通常需要 delegate 给 S-mode？

---

### 15. 为什么 `medeleg[13] = 1` 不意味着所有 Load Page Fault 都进 S-mode？

考虑三个场景：

```text
U-mode Load Page Fault

S-mode Load Page Fault

M-mode Load Page Fault
```

假设：

```text
medeleg[13] = 1
```

分别进入哪里？

解释：

```text
vertical trap
horizontal trap
```

以及：

> Trap cannot reduce privilege

的含义。

---

### 16. 以下说法是否正确？

```text
medeleg[2] = 1
```

因此：

```text
M-mode 执行 illegal instruction
        ↓
S-mode handler
```

请判断，并解释其架构原因。

进一步回答：

> 如果允许 M-mode trap down 到 S-mode，会造成什么样的 privilege model 问题？

---

### 17. 为什么 RISC-V 使用“cause-based delegation”？

Arm 系统更多通过：

```text
SCR_EL3
HCR_EL2
exception routing rules
```

进行 routing。

RISC-V 则明显暴露：

```text
medeleg
mideleg
```

请比较两种设计思想，而不是只比较寄存器名称。

---

### 18. OpenSBI 应该 delegate 哪些异常给 Linux？

不用给精确 bitmask。

从设计原则讨论：

```text
Page Fault
ECALL from U
Illegal Instruction
PMP-related Access Fault
```

哪些更应该由 S-mode OS 处理？

哪些可能应该由 M-mode firmware 保留？

你的判断依据是什么？

---

# Part E：`xtvec` 与 Trap Entry

### 19. Direct 和 Vectored 模式的真正区别是什么？

假设：

```text
stvec.BASE = 0x80001000
MODE = Vectored
```

请分别给出：

```text
Load Page Fault

ECALL from U

Supervisor Timer Interrupt
```

进入 handler 后的 PC。

重点回答：

> 为什么 Vectored Mode 并不意味着每一种 Exception 都拥有独立 vector？

---

### 20. 为什么 Vectored Mode 只对 Interrupt vectoring？

从架构设计角度解释：

```text
Exception
```

和：

```text
Interrupt
```

在 dispatch latency 上的需求为什么不同。

你认为这种设计有什么优势？

---

### 21. 为什么 `xtvec` 指向的代码通常不能直接是普通 C 函数？

刚进入 trap handler 时：

```text
GPR 尚未保存
sp 可能是 user stack
tp/gp 状态可能属于 user
ABI 环境未必成立
```

请解释为什么一般需要：

```text
assembly entry
        ↓
C handler
```

两阶段设计。

---

# Part F：`sscratch` / Stack / Trap Frame

### 22. `sscratch` 是为了解决什么问题？

假设：

```text
U-mode

sp = user_sp
```

突然发生：

```text
U → S Trap
```

RISC-V hardware 不自动切换 stack。

回答：

1. kernel trap handler 面临什么问题？
2. `sscratch` 可以如何解决？
3. 为什么 architecture 不规定 `sscratch` 必须存 kernel SP？

---

### 23. 分析下面这条指令

```asm
csrrw sp, sscratch, sp
```

进入 Trap 前：

```text
sp       = user_sp
sscratch = kernel_sp
```

执行后分别是什么？

为什么：

```text
CSRRW
```

特别适合做这种操作？

进一步回答一个 Staff 问题：

> 如果当前 Trap 本来就是从 S-mode kernel 中发生，而不是 U→S，是否还能无条件执行同样的交换？

---

### 24. Trap Frame 应该包含哪些内容？

你正在设计一个 RTOS/Linux-like kernel。

Trap Frame 是否应该至少保存：

```text
GPR
sepc
sstatus
scause
stval
```

所有这些？

哪些是：

```text
必须长期保存
```

哪些可能只是：

```text
handler 临时读取
```

答案应考虑：

```text
context switch
signal
debugging
nested trap
```

---

# Part G：Nested Trap / Double Trap

### 25. 为什么 nested trap 是危险的？

初始状态：

```text
Trap #1
 ↓
sepc/scause/stval written
 ↓
进入 trap_entry
```

但在软件把这些 CSR 保存到 stack 之前：

```text
Trap #2
```

发生。

请描述：

```text
第一次 Trap 的哪些状态可能丢失？
```

以及 OS/firmware 通常如何设计 Trap Entry 来降低这个风险。

---

### 26. 为什么 `xstatus` 的 previous state 不能被理解成完整 exception stack？

例如：

```text
SIE
SPIE
SPP
```

看起来好像硬件帮我们保存了上一次状态。

那么为什么不能支持：

```text
Trap
  ↓
Trap
  ↓
Trap
  ↓
SRET
  ↓
SRET
  ↓
SRET
```

而完全不需要 software stack？

---

### 27. 什么是 Trap Entry 的“critical window”？

假设 handler：

```text
1. Trap occurs
2. PC = stvec
3. 保存一部分 GPR
4. 保存 sepc/scause
5. 打开 interrupt
```

请指出：

```text
什么时候可以安全重新打开 interrupt？
```

这是不是一个固定架构答案？

为什么？

这个题重点考：

> architecture mechanism 和 OS policy 的边界。

---

# Part H：综合系统设计题

### 28. 请设计一个最小 S-mode Trap Handler

目标支持：

```text
U-mode syscall

Load Page Fault

Illegal Instruction
```

请描述整体软件流程：

```text
stvec
 ↓
assembly entry
 ↓
trapframe
 ↓
C dispatcher
 ↓
per-cause handling
 ↓
restore
 ↓
sret
```

要求明确说明三个异常分别如何处理 `sepc`。

---

### 29. Debug 场景：系统不断重复触发 ECALL

日志：

```text
scause = 8
sepc   = 0x40001000
stval  = 0

return from trap

scause = 8
sepc   = 0x40001000
stval  = 0

return from trap
...
```

系统没有 crash，但永远离不开 trap handler。

请判断最可能的问题在哪里。

进一步给出：

```text
如何验证
如何修复
```

---

### 30. Staff Engineer 综合题

有如下系统：

```text
M-mode
    OpenSBI-like firmware

S-mode
    OS

U-mode
    applications
```

发生：

```text
U-mode application
    |
    | Load
    v
Page Fault
```

请完整描述从 fault 产生到应用重新执行成功之间：

```text
hardware
M-mode configuration
S-mode trap handling
page table
TLB
software trap frame
SRET
```

分别承担什么职责。

最后回答：

> **在整个过程中，哪些部分属于 RISC-V architecture 定义，哪些属于 firmware policy，哪些属于 OS policy？**

这最后一个问题，是我认为最能区分 **Senior Engineer 和 Staff Engineer** 的题。

---

## 我建议的通过标准

这 30 题不需要“全答出才算会”。如果按照 Staff Engineer 的标准，我会这样看：

**20/30 左右能够准确回答**，说明 Trap Architecture 的主体已经很扎实；如果第 **14～18、22～27、30** 这些 delegation、stack、nested trap、hardware/software boundary 类问题也能讲清楚，基本说明已经不是在背 RISC-V CSR，而是在理解 **RISC-V privileged architecture 的设计**。

尤其建议你优先挑战这 8 道：

```text
3   完整 Trap Entry
12  Page Fault vs Access Fault 的设计原因
15  Delegation + horizontal/vertical trap
18  M/S responsibility
23  sscratch + kernel/user trap
25  Nested trap
27  Trap critical window
30  Architecture/Firmware/OS 三层边界
```

如果这 8 题能在**不查资料的情况下完整讲清楚**，我会认为 `03 Trap/Exception` 这一章已经达到相当好的 Staff-level 掌握程度。
