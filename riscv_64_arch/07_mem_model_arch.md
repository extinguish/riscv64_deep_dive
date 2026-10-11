# 07 — RISC-V Memory Model / FENCE

先给这一章最重要的结论：

> **Memory Model 解决的核心问题不是“CPU 会不会乱序执行”，而是：多个 hart / device 同时访问共享内存时，软件能够依赖什么样的“可观察顺序”。**

RISC-V 默认采用 **RVWMO（RISC-V Weak Memory Ordering）**。因此：

```text
程序顺序 Program Order
        ≠
其他 Hart 观察到的 Memory Order
```

软件如果要求额外顺序，就必须使用：

```text
FENCE
aq / rl
Atomic operation
FENCE.I
SFENCE.VMA
```

但这几个东西解决的是不同的问题。

你可以先建立这一张总图：

```text
                    Memory Ordering
                          |
        +-----------------+------------------+
        |                 |                  |
   Data Memory       Instruction        Address Translation
        |             Fetch                  |
      FENCE           FENCE.I            SFENCE.VMA
        |
   +----+----------------+
   |                     |
normal memory            atomic
R/W ordering          aq / rl / aqrl
```

RISC-V 当前默认内存模型仍然是 RVWMO；`Ztso` 是可选的、更强的 TSO 模型。官方明确把 RVWMO 定义为所有 RISC-V 实现至少必须兼容的弱内存模型。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/rvwmo.html?utm_source=chatgpt.com)

---

# 1. 为什么现代 CPU 需要 Memory Model？

先不要看 RISC-V。

假设：

```c
int data = 0;
int ready = 0;
```

Hart 0：

```text
data = 100;
ready = 1;
```

Hart 1：

```text
if (ready == 1)
    print(data);
```

程序员直觉认为：

```text
Hart 0:

data = 100
    ↓
ready = 1
```

那么 Hart 1 看到：

```text
ready == 1
```

以后应该一定看到：

```text
data == 100
```

但弱内存系统并不自动保证这个结论。

原因包括：

```text
CPU pipeline
Out-of-order execution
Store buffer
Load queue
Cache hierarchy
Interconnect
Speculation
Compiler optimization
```

例如：

```text
Hart 0

sw data
 |
 v
Store Buffer
 |
 +---------------> cache/interconnect

sw ready
 |
 +---------------> cache/interconnect
```

理论上另一个 hart 可能先观察到：

```text
ready = 1
```

再观察到：

```text
data = 100
```

所以 Memory Model 本质上是在定义：

> **哪些重排序是 architecture 允许外界观察到的。**

注意我特意说的是：

> **observable ordering**

而不是 CPU 内部真的必须“先执行谁”。

这是理解 Memory Model 的第一原则。

---

# 2. 一定要区分四个概念

这是学习 memory model 最容易混乱的地方。

## 2.1 Atomicity

例如：

```text
64-bit store
```

是否能够被观察成：

```text
upper 32-bit = old
lower 32-bit = new
```

这是 atomicity 问题。

---

## 2.2 Cache Coherence

假设：

```text
Hart0: X = 1
Hart1: X = 2
```

其他处理器对 **同一个 X** 的写入顺序是否一致。

这是 coherence。

可以粗略理解成：

> **Coherence 管单个 memory location。**

---

## 2.3 Memory Consistency

现在：

```text
X = 1
Y = 1
```

两个不同地址之间：

```text
X
Y
```

别人必须按什么顺序观察？

这是 consistency。

所以：

```text
Coherence
    ↓
同一个地址

Memory consistency
    ↓
不同地址之间的顺序
```

---

## 2.4 Cache Maintenance

例如：

```text
clean cache
invalidate cache
flush cache
```

这是缓存内容管理。

它和：

```text
FENCE
```

不是一回事。

非常重要：

> **FENCE ≠ flush cache。**

这是从 Arm 学到 RISC-V 时一定要保持的认知。

---

# 3. RVWMO 到底是什么？

RVWMO：

> **RISC-V Weak Memory Ordering**

核心思想是：

```text
尽可能少地强制 memory ordering
              ↓
给 CPU 微架构更大的优化空间
              ↓
软件在真正需要 synchronization 时显式约束
```

这和 Armv8-A 的 weakly ordered memory 思路其实非常接近。

所以你可以先粗略建立：

```text
Arm weak memory model
          ≈
RISC-V RVWMO
```

而：

```text
x86 TSO
```

属于更强的 memory ordering。

RISC-V 因此又提供：

```text
Ztso
```

用于提供 RVTSO。官方说明，Ztso 的目的之一就是帮助移植原本针对 x86/SPARC TSO 编写的软件。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/ztso-st-ext.html?utm_source=chatgpt.com)

---

# 4. Program Order 和 Global Memory Order

这是理解 RVWMO 最关键的两个概念。

假设 Hart 0：

```text
Instruction A
Instruction B
Instruction C
```

那么：

```text
A → B → C
```

叫：

> **Program Order**

但 memory system 可能观察成：

```text
B
A
C
```

或者：

```text
A
C
B
```

只要没有违反 RVWMO 的规则。

所以 RVWMO 又定义：

> **Preserved Program Order，PPO**

意思是：

```text
Program Order
     |
     | 某些关系必须保留
     v
Preserved Program Order
```

比如某些：

```text
dependency
same-address ordering
atomic ordering
fence ordering
```

不能被任意打乱。

更正式一点理解：

```text
Program Order
       |
       | RVWMO rules
       v
哪些边必须存在
       |
       v
Global Memory Order
```

---

# 5. 一个经典 litmus test

考虑：

```text
初始：

X = 0
Y = 0
```

Hart 0：

```asm
li t0, 1

sw t0, 0(X)
sw t0, 0(Y)
```

Hart 1：

```asm
lw t0, 0(Y)
lw t1, 0(X)
```

我们最关心：

```text
t0 = 1
t1 = 0
```

也就是：

```text
Hart1 已经看到了 Y=1
但还没有看到 X=1
```

在没有额外同步约束时，弱内存模型不能让软件简单依赖：

```text
X store
   ↓
Y store
```

这个跨地址顺序。

因此需要 barrier / release-acquire。

这就是 FENCE 出现的根本原因。

---

# 6. RISC-V FENCE 的设计思想

RISC-V 的 FENCE 很漂亮，因为它不是简单：

```text
fence
```

一个开关，而是：

```asm
fence predecessor, successor
```

也就是：

> **哪些“之前的操作”必须排在“哪些之后的操作”之前。**

基本集合：

```text
R = main-memory Read
W = main-memory Write

I = device Input
O = device Output
```

因此：

```asm
fence rw, rw
```

含义是：

```text
之前的 R/W
      |
      | must happen before
      v
之后的 R/W
```

官方的定义也是：FENCE 根据 predecessor set 和 successor set 对 memory/device operations 建立 ordering。[RISC-V Documentation](https://docs.riscv.org/reference/isa/v20260120/unpriv/rv32.html?utm_source=chatgpt.com)

---

# 7. 最常见的几个 FENCE

首先记这几个就够了：

| RISC-V | 含义 |
|---|---|
| `fence rw,rw` | full memory ordering |
| `fence r,rw` | acquire-like barrier |
| `fence rw,w` | release-like barrier |
| `fence w,w` | store → store |
| `fence w,r` | store → load |
| `fence iorw,iorw` | memory + device 全 ordering |

例如：

```asm
sw t0, 0(X)

fence w,w

sw t1, 0(Y)
```

强制：

```text
Store X
   |
   v
Store Y
```

不能被其他 observer 看成：

```text
Store Y
   |
   v
Store X
```

---

# 8. 和 Arm DMB 对照起来就很好理解

这部分你可以直接复用 Arm 经验。

官方 RISC-V explanatory material 给出的典型映射包括：Arm `dmb` → `fence rw,rw`、`dmb.ld` → `fence r,rw`、`dmb.st` → `fence w,w`。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/mm-eplan.html?utm_source=chatgpt.com)

可以先记成：

```text
Arm                         RISC-V

DMB                         fence rw,rw

DMB LD                      fence r,rw

DMB ST                      fence w,w
```

因此：

```asm
// ARM
str x0, [X]
dmb ish
str x1, [Y]
```

思维上对应：

```asm
# RISC-V
sd a0, 0(X)
fence rw,rw
sd a1, 0(Y)
```

但注意：

> RISC-V FENCE 没有 Arm `ISH/OSH/SY` 那套显式 shareability domain。

这是架构设计上的重要差异。

---

# 9. Acquire / Release 才是写同步代码最核心的概念

如果你只记一个图，我建议记：

```text
              Release
                 |
                 v

operations BEFORE
        ↓
   must stay before
        ↓
 RELEASE operation
```

而 acquire：

```text
 ACQUIRE operation
        ↓
   must stay before
        ↓
operations AFTER
```

所以：

```text
             Producer

write data
write data
write data
    |
    | Release
    v
publish flag
```

Consumer：

```text
read flag
    |
    | Acquire
    v
read data
read data
read data
```

组合起来：

```text
Producer                      Consumer

data = ...
data = ...
   |
   | Release
   v
flag = 1  ------------->  read flag
                              |
                              | Acquire
                              v
                           read data
```

这就是现代 synchronization 最重要的 pattern。

---

# 10. 用 FENCE 实现 Release / Acquire

Producer：

```asm
sw a0, 0(data)

fence rw,w

li t0, 1
sw t0, 0(flag)
```

意思：

```text
之前所有 R/W
      |
      v
release fence
      |
      v
flag store
```

Consumer：

```asm
1:
    lw t0, 0(flag)
    beqz t0, 1b

    fence r,rw

    lw a0, 0(data)
```

意思：

```text
load flag
    |
 acquire fence
    |
    v
后面的 R/W
```

所以：

```text
write data
    |
release
    |
write flag
      ───────────────>
                     read flag
                        |
                     acquire
                        |
                     read data
```

这就是 publish/consume 最标准的思维模型。

---

# 11. `aq` / `rl` 是什么？

到了 A extension，你会看到：

```asm
lr.w.aq
sc.w.rl

amoadd.w.aq
amoswap.w.rl
amoadd.w.aqrl
```

这里：

```text
aq = acquire
rl = release
```

官方定义非常清楚：

```text
aq

atomic operation
      |
      +---- 后面的 memory operation
            不能被观察到跑到 aq 前面
```

而：

```text
rl

前面的 memory operation
      |
      +---- 不能被观察到跑到 rl 后面
```

两者同时：

```text
aqrl
```

则该 atomic operation 同时具有 acquire + release ordering。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/a-st-ext.html?utm_source=chatgpt.com)

---

# 12. 用 spinlock 理解 aq / rl

例如一个锁：

```text
lock = 0
```

加锁：

```asm
li t0, 1

1:
    amoswap.w.aq t1, t0, (a0)
    bnez t1, 1b
```

关键：

```text
amoswap.aq
     |
     v
critical section
```

`.aq` 保证：

> critical section 里的访问不能跑到 lock acquire 前面。

于是：

```text
      acquire
         |
         v
-------------------
critical section
-------------------
```

解锁：

```asm
amoswap.w.rl x0, x0, (a0)
```

`.rl` 保证：

```text
critical section
        |
        v
     release
```

critical section 内之前的访问不能跑到 unlock 后面。

整体：

```text
             acquire
                |
                v
       +------------------+
       | critical section |
       +------------------+
                |
                v
             release
```

这其实和 Arm：

```text
LDAXR
...
STLXR
```

背后的思想完全一样。

---

# 13. Atomicity ≠ Ordering

这是一个特别重要的 Staff 级面试点。

例如：

```asm
amoadd.w t0, t1, (a0)
```

它保证的是：

```text
read
modify
write
```

作为一个 atomic operation。

但如果：

```text
aq = 0
rl = 0
```

它**并不自动给周围普通 memory operations 提供 acquire/release ordering**。

所以：

```text
Atomic
```

解决：

> 这个 RMW 会不会被其他 hart 插进来？

而：

```text
Acquire / Release / Fence
```

解决：

> 前后 memory operation 的可观察顺序是什么？

完全不同。

---

# 14. `fence` 和 `.aq/.rl` 的本质区别

可以这样理解：

```text
FENCE

operation A
operation B
operation C

    fence

operation D
operation E
```

它是在两组操作之间建立 ordering。

而：

```text
AMO.aq
```

是：

```text
       AMO
        |
        +---- acquire semantics
```

ordering 直接附着在 atomic operation 上。

因此 `.aq/.rl` 往往比：

```asm
fence rw,rw
```

更精确。

这和 Arm：

```text
DMB
```

vs：

```text
LDAR
STLR
```

的设计理念非常接近。

---

# 15. 一个 RISC-V 新变化：Zalasr

这点值得你知道，因为现在的 RISC-V 已经和早期资料有一点不同。

早期经典 RISC-V：

```text
普通 lw/sw
      ↓
没有 .aq/.rl

LR/SC/AMO
      ↓
支持 aq/rl
```

因此普通 acquire load 常常需要：

```asm
lw
fence r,rw
```

release store：

```asm
fence rw,w
sw
```

但现在 ratified 的 **Zalasr 1.0** 已经增加了独立的 load-acquire / store-release，例如：

```asm
lw.aq
ld.aq

sw.rl
sd.rl
```

就是为了更直接支持 C++/Java 等语言的 acquire/release primitive。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/zalasr.html?utm_source=chatgpt.com)

所以未来你看到：

```asm
ld.aq a0, (a1)
```

不要奇怪。

可以理解为非常接近 Arm：

```asm
LDAR
```

而：

```asm
sd.rl a0, (a1)
```

接近：

```asm
STLR
```

但实际写底层软件时仍然必须确认目标 ISA profile / CPU 是否实现 Zalasr，不能默认所有 RISC-V CPU 都有。

---

# 16. RISC-V 为什么还区分 R/W 与 I/O？

这是 RISC-V `FENCE` 比较有特色的地方。

例如驱动：

```text
CPU
 |
 | write DMA descriptor
 v
DDR memory

CPU
 |
 | write doorbell register
 v
Device
```

假设：

```c
desc->addr = buffer;
desc->len  = 4096;

DEVICE_DOORBELL = 1;
```

真正需要的关系是：

```text
memory writes
      |
      v
MMIO write
```

因此可以：

```asm
# descriptor writes

fence w,o

# MMIO doorbell write
```

也就是：

```text
W
|
| fence w,o
v
O
```

其中：

```text
W = normal memory write
O = device output
```

RISC-V 的 physical memory attributes 会把地址区域分类成 main memory 或 I/O，从而影响 FENCE 中这些 ordering domain 的意义。[RISC-V Documentation](https://docs.riscv.org/reference/isa/priv/machine.html?utm_source=chatgpt.com)

---

# 17. `fence w,o` 解决 DMA ordering，但不解决 cache coherence

例如 non-coherent DMA：

```text
CPU cache
   |
 descriptor
   |
   X
   |
 memory ---- DMA
```

CPU 只是：

```asm
fence w,o
```

并不能保证 dirty cache line 被 clean 到 DDR。

因此实际可能还需要：

```text
write descriptor

      ↓

cache clean / cbo.clean
      ↓

fence w,o
      ↓

MMIO doorbell
```

所以一定牢记：

```text
FENCE
    =
ordering

Cache clean/invalidate
    =
cache visibility/coherency management
```

这是完全不同的两个维度。

---

# 18. FENCE 也不等价于 Arm DSB

这个地方也很容易产生误解。

Arm：

```text
DMB
```

主要强调：

```text
memory ordering
```

而：

```text
DSB
```

更强，涉及：

```text
completion
```

RISC-V：

```text
FENCE
```

本质首先是 **ordering primitive**。

不要简单写：

```text
DSB == FENCE
```

尤其在 MMIO/device side effect 场景。

RISC-V 规范甚至明确提醒：FENCE 可以约束 memory operation 被 device 观察到的顺序，但不能自动约束设备通过其他外部信号产生的所有事件。[RISC-V Documentation](https://docs.riscv.org/reference/isa/v20260120/unpriv/rv32.html?utm_source=chatgpt.com)

例如：

```text
MMIO write
     |
     v
device starts operation
     |
     v
interrupt
```

不能简单认为：

```asm
fence
```

就等于：

> “设备动作已经完成”。

如果硬件要求 posted-write completion，通常还要依赖 platform/device protocol，例如 read-back。

---

# 19. `FENCE.I`：完全不同的问题

现在看第二个 Fence：

```asm
fence.i
```

它不是：

```text
memory ordering fence
```

主要解决：

> **Data store 修改了 instruction memory 后，instruction fetch 什么时候能看到新 instruction？**

例如 JIT：

```text
CPU store

0x80000000:
    写入新的 instruction
```

随后：

```text
jalr 0x80000000
```

RISC-V 不保证 instruction fetch 马上看到这些刚写入的数据。

因此：

```asm
sd ...
sd ...
sd ...

fence.i

jalr ...
```

`FENCE.I` 保证当前 hart 后续 instruction fetch 能看到之前已经对该 hart 可见的数据写入。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/zifencei.html?utm_source=chatgpt.com)

可以画成：

```text
D-cache / data stores
        |
        | FENCE.I
        v
I-fetch
        |
        v
new instruction
```

---

# 20. `FENCE.I` 不是 SMP 全局 icache flush

这是重点。

假设 Hart 0 修改 code：

```text
Hart0

store new code
fence.i
```

这只保证：

```text
Hart0
```

自己后续 instruction fetch 正确。

并不自动让：

```text
Hart1
Hart2
Hart3
```

全部同步。

规范明确指出，多 hart 场景需要 writer 做数据 ordering，然后让其他 hart 自己执行 `FENCE.I`。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/zifencei.html?utm_source=chatgpt.com)

典型流程：

```text
Hart0
 |
 | write new code
 |
 | data fence
 |
 | IPI
 +---------------------> Hart1
                           |
                           | FENCE.I
                           v
                       execute code
```

这和你熟悉的 SMP I-cache synchronization 思路非常接近。

---

# 21. `SFENCE.VMA` 又是第三种完全不同的 Fence

在上一章 Sv39 我们已经见过：

```asm
sfence.vma
```

假设：

```text
OS 修改 page table PTE
```

然后希望 CPU 后续 translation 使用新的 PTE。

这时候：

```asm
sd new_pte, 0(pte)

sfence.vma
```

它解决的是：

```text
explicit page-table store
           |
           v
address translation hardware
```

的同步。

官方定义非常明确：`SFENCE.VMA` 用于把 page-table update 与后续隐式 address-translation references 同步，同时处理本 hart 的 translation caches；它**只直接约束本 hart 的 memory-management implicit references**。[RISC-V Documentation](https://docs.riscv.org/reference/isa/_attachments/riscv-privileged.pdf?utm_source=chatgpt.com)

所以：

```text
FENCE

data memory ordering
```

```text
FENCE.I

data write → instruction fetch
```

```text
SFENCE.VMA

page-table write → address translation
```

三者一定不能混。

---

# 22. 这一张表建议直接记下来

| 指令 | 主要解决什么 |
|---|---|
| `FENCE` | normal memory / MMIO ordering |
| `FENCE.I` | instruction-fetch synchronization |
| `SFENCE.VMA` | page-table / address-translation synchronization |

对应你的 Arm 思维：

```text
RISC-V                      Arm 思维

FENCE                       DMB 类似角色

FENCE.I                     instruction synchronization
                            + I-cache coherence 思维

SFENCE.VMA                  TLBI + translation synchronization
                            一类问题
```

但这里都只是 **mental mapping**，不是严格 1:1。

---

# 23. SMP 下修改页表也需要 remote shootdown

这一点和 Arm/Linux 非常像。

假设：

```text
Hart0 修改 PTE
```

Hart1 已经缓存：

```text
VA → old PA
```

Hart0：

```asm
sfence.vma
```

只影响：

```text
Hart0
```

并不会自动刷新 Hart1。

官方给出的典型 SMP 思路本身就是：

```text
Hart0
 |
 | modify PTE
 |
 | data fence
 |
 | send IPI
 v
Hart1
 |
 | interrupt handler
 |
 | SFENCE.VMA
 v
new translation
```

这实际上就是 RISC-V 版的：

> **TLB shootdown**。[RISC-V Documentation](https://docs.riscv.org/reference/isa/_attachments/riscv-privileged.pdf?utm_source=chatgpt.com)

你有 Arm SMP/Linux 背景，这部分基本可以直接迁移。

---

# 24. `FENCE.TSO` 是什么？

RISC-V 还定义：

```asm
fence.tso
```

它不是普通：

```asm
fence rw,rw
```

的完全同义词。

它大概保持：

```text
Load → Load
Load → Store
Store → Store
```

但允许：

```text
Store → Load
```

这种 TSO 典型行为保持 relaxed。

官方描述是：`FENCE.TSO` 不强制普通 predecessor store 与后续普通 load 之间的 ordering，而 `fence rw,rw` 会提供更强的 ordering。[RISC-V Documentation](https://docs.riscv.org/reference/isa/v20260120/unpriv/rv32.html?utm_source=chatgpt.com)

所以：

```text
fence rw,rw
```

比：

```text
fence.tso
```

更强。

---

# 25. Ztso 又和 FENCE.TSO 不完全是一个东西

这一点名字很容易误导。

```text
FENCE.TSO
```

是：

> 一条 fence instruction。

而：

```text
Ztso
```

是：

> 一个 ISA extension，意味着整个程序可以依赖更强的 RVTSO memory model。

Ztso 会使普通 loads/stores 获得比 RVWMO 更强的默认 ordering；规范描述为 loads 具有 acquire-RCpc 类行为、stores 具有 release-RCpc 类行为，而 AMO 具有 acquire+release 的更强 ordering。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/ztso-st-ext.html?utm_source=chatgpt.com)

所以：

```text
RVWMO
  +
fence.tso instruction
```

和：

```text
整个 hart 声明支持 Ztso
```

不是同一件事。

---

# 26. Dependency 在 RVWMO 中为什么重要？

弱内存架构还有一个非常重要的概念：

```text
dependency
```

例如：

```asm
ld a0, 0(a1)

ld a2, 0(a0)
```

第二次访问地址依赖第一次 load 的结果：

```text
load pointer
      |
      | address dependency
      v
load *pointer
```

这种关系可能成为 RVWMO 的 preserved program order 一部分。

类似还有：

```text
data dependency
address dependency
control-related ordering rules
```

这是 RVWMO formal model 很重要的一部分。

但在日常 kernel/app 同步代码里，我建议你：

> **先不要试图依靠复杂 dependency 手写 lock-free synchronization。**

因为还有一个非常现实的问题：

```text
Compiler
```

可能把你以为存在的 dependency 优化掉。

所以 C/C++ 层应该优先：

```text
C11/C++ atomics
Linux barrier primitives
```

然后由 compiler 映射成：

```text
fence
aq/rl
AMO
```

---

# 27. CPU barrier 和 compiler barrier 是两回事

假设：

```c
a = 1;

asm("fence rw,rw");

b = 1;
```

CPU 确实可能受到 FENCE 的限制。

但是 compiler：

```text
C source
   |
   | optimizer
   v
assembly
```

也可能在生成 assembly 之前重排。

所以内核代码里的 barrier 通常同时需要控制：

```text
Compiler
+
CPU
```

例如 GNU inline asm：

```c
asm volatile("fence rw,rw" ::: "memory");
```

这里：

```text
fence rw,rw
```

约束硬件；

而：

```text
"memory"
```

clobber 告诉 compiler：

> 不要把周围的 memory operations 随意跨过这里优化。

因此：

```text
Compiler barrier
        ≠
Hardware memory barrier
```

这一点和 Arm 完全一样。

---

# 28. `volatile` 更不是 memory barrier

这是面试里经常出现的坑。

```c
volatile int flag;
```

`volatile` 主要告诉 compiler：

> 这次 access 本身不要被随便删掉或合并。

但：

```text
volatile
```

并不等价于：

```text
Acquire
Release
FENCE
atomicity
```

因此不能通过：

```c
volatile int lock;
```

来实现正确的 SMP lock。

正确方向应该是：

```text
atomic
+
memory ordering semantics
```

---

# 29. Arm 和 RISC-V 的整体对应关系

现在可以整理成一张非常适合你的表：

| Armv8-A | RISC-V |
|---|---|
| weak memory model | RVWMO |
| DMB | `fence rw,rw` 类似 |
| DMB LD | `fence r,rw` |
| DMB ST | `fence w,w` |
| LDAR | `*.aq` / Zalasr `ld.aq` 思维 |
| STLR | `*.rl` / Zalasr `sd.rl` 思维 |
| LDAXR | `lr.*.aq` |
| STLXR | `sc.*.rl` |
| atomic RMW | AMO |
| ISB / I-cache sync 相关 | `FENCE.I`，但非简单 1:1 |
| TLBI + translation sync | `SFENCE.VMA` 类问题 |

RISC-V 官方 explanatory material 也专门给出了 Arm→RVWMO 的映射示例，因此用 Arm memory-ordering 模型来学习 RISC-V 是非常合适的切入方式。[RISC-V Documentation](https://docs.riscv.org/reference/isa/unpriv/mm-eplan.html?utm_source=chatgpt.com)

---

# 30. 从架构设计角度理解 RISC-V 为什么这么设计

这里才是这一章真正值得理解的设计思想。

RISC-V 没有选择：

```text
所有 memory operation
天然强顺序
```

而是：

```text
默认：

        RVWMO
          |
       weak ordering
          |
       高性能
          |
       简单实现也可以
          |
          v
需要同步时显式说明
```

然后提供三个层次：

```text
Level 1

FENCE
    ↓
大范围 ordering
```

```text
Level 2

Acquire / Release
    ↓
单向、更加精确的 ordering
```

```text
Level 3

Atomics
    ↓
atomicity + ordering
```

所以这是：

```text
性能
  ↑
  |
fine-grained synchronization
  |
general barrier
  |
  ↓
简单性
```

之间的权衡。

---

# 31. 从 CPU 微架构理解这一切

你也可以把 RVWMO 想象成 CPU 有：

```text
                    CPU core
                       |
        +--------------+-------------+
        |                            |
     Load Queue                  Store Buffer
        |                            |
        +-------------+--------------+
                      |
                    Cache
                      |
                 Interconnect
                      |
                 Other Harts
```

如果每次：

```text
load
store
```

都必须：

```text
严格按 program order
```

那么：

```text
OOO
store buffer
speculative execution
multiple outstanding requests
```

很多优化都会受限。

Weak memory model 实际上是在说：

> 微架构可以大胆优化，只要最终 observable behavior 仍然属于 architecture 允许的集合。

而 Fence：

```text
Fence
  ↓
在这里告诉 CPU：
这个 ordering 是软件语义的一部分
不能优化掉
```

这就是 memory barrier 最本质的意义。

---

# 32. 实际工程中什么时候用哪一个？

你可以用这一张决策图。

```text
需要 ordering？
    |
    +-- 普通共享 memory？
    |        |
    |        +-- publish/consume
    |        |       ↓
    |        | acquire/release
    |        |
    |        +-- 强制全顺序
    |                ↓
    |           fence rw,rw
    |
    +-- lock / atomic counter？
    |        ↓
    |      AMO/LRSC
    |      aq / rl
    |
    +-- normal memory → MMIO？
    |        ↓
    |     fence w,o
    |
    +-- 修改 executable code？
    |        ↓
    |      fence.i
    |
    +-- 修改 page table？
             ↓
         sfence.vma
```

这张图基本覆盖绝大部分 kernel/driver 场景。

---

# 33. 五个最应该掌握的实际场景

学完这一章，我认为你应该能独立解释下面五种情况：

```text
① SMP message passing

data
 ↓
release
 ↓
flag
 ↓
acquire
 ↓
consume data
```

```text
② Spinlock

amoswap.aq
     ↓
critical section
     ↓
amoswap.rl
```

```text
③ DMA

write descriptor
     ↓
cache maintenance（如果需要）
     ↓
fence w,o
     ↓
MMIO doorbell
```

```text
④ JIT / self-modifying code

write instruction
     ↓
fence.i
     ↓
execute instruction
```

```text
⑤ Page-table update

write PTE
     ↓
sfence.vma
     ↓
new translation
```

如果这五个场景能够解释清楚，那么 **Memory Model/FENCE 的工程部分实际上已经掌握得比较扎实了。**

---

# 34. Staff Engineer 层面再往下深入什么？

下一层才是 formal memory model：

```text
RVWMO
 |
 +-- program order
 |
 +-- global memory order
 |
 +-- preserved program order
 |      |
 |      +-- dependencies
 |      +-- fences
 |      +-- aq/rl annotations
 |      +-- overlapping-address rules
 |
 +-- load-value axiom
 |
 +-- atomicity axiom
```

这时候可以开始研究经典 litmus tests：

```text
MP    Message Passing
SB    Store Buffering
LB    Load Buffering
IRIW
WRC
RWC
```

然后真正回答：

```text
这个 outcome：

r0 = 0
r1 = 0

在 RVWMO 下到底允许还是禁止？
```

这才是 memory model 的“深水区”。

但我不建议现在第一遍就钻进去，否则很容易从架构学习变成研究形式化 memory model。

---

# 35. 最后，把这一章压缩成一张脑图

```text
                   RISC-V Memory Model
                           |
                        RVWMO
                           |
            weak observable ordering
                           |
        +------------------+------------------+
        |                  |                  |
      FENCE              Atomics          Special Fence
        |                  |                  |
   pred → succ          aq / rl          +----+---------+
        |                  |              |              |
     R/W/I/O           LR / SC / AMO   FENCE.I      SFENCE.VMA
        |                  |              |              |
memory/device          synchronization   I-fetch       MMU/TLB
 ordering
```

再压缩成四句话：

> **① RISC-V 默认是 RVWMO，普通 load/store 之间不能假设强 ordering。**

> **② `FENCE` 控制普通 memory/MMIO 的可观察顺序，而不是 flush cache。**

> **③ `aq/rl` 是更精确的 acquire/release ordering，常与 LR/SC/AMO 配合；现在 Zalasr 还提供 standalone acquire-load/release-store。**

> **④ `FENCE.I` 管 instruction fetch，`SFENCE.VMA` 管 address translation，它们都不是普通 data memory barrier。**

对你这种已经熟悉 Armv8-A barrier、SMP 和 MMU 的背景来说，这四条抓牢以后，RISC-V Memory Model 的主框架其实已经建立起来了。
