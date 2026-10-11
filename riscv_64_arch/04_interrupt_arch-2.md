可以。上一版确实更像“把 RISC-V Interrupt 的零件全部摆在桌子上”，但还没有真正把它们装成一台机器。

这次我们换一种方法：**完全不按寄存器讲，而是围绕一个真实问题——“UART 收到一个字符以后，CPU 到底是怎么进入 S-mode ISR 的？”——一路追完整条链。**

你理解完这条链以后，会发现：

```text
PLIC
mip/sip
mie/sie
mideleg
sstatus
stvec
scause
claim/complete
```

其实分别都在回答一个非常明确的问题。

---

# 一、先建立整个 RISC-V Interrupt Architecture 的设计模型

我认为理解 RISC-V Interrupt 最重要的一句话是：

> **RISC-V 把“设备中断路由”和“CPU Trap 接收”设计成了两个相对独立的层次。**

也就是：

```text
                 RISC-V Interrupt Architecture

       Platform interrupt domain
       “哪个设备要通知哪个 hart？”
                    │
                    │
         PLIC / APLIC / IMSIC
                    │
                    ▼
       ┌──────────────────────────┐
       │         Hart             │
       │                          │
       │ “我现在是否接受它？”     │
       │ “由 M 还是 S 处理？”     │
       │ “handler 在哪里？”       │
       │                          │
       │ mip/sip                  │
       │ mie/sie                  │
       │ mideleg                  │
       │ mstatus/sstatus          │
       │ mtvec/stvec              │
       └──────────────────────────┘
```

这是整个设计的根。

如果用你熟悉的 Arm 思维说：

RISC-V 不希望 ISA 本身规定：

```text
必须有一个类似 GIC 的复杂 interrupt controller
```

ISA 只定义：

> **当“一个 external interrupt”到达 hart 时，这个 hart 应该怎么处理。**

至于：

> UART、SPI、PCIe、Ethernet 几百个 IRQ 怎么汇聚、怎么选 CPU、怎么做 priority？

交给：

```text
PLIC
APLIC
IMSIC
```

这种 **platform architecture**。

这和 RISC-V 整体哲学高度一致：Privileged Architecture 本身故意和具体 platform 保持一定解耦。官方 privileged architecture 也明确强调，它定义的是运行 OS、连接外设所需的 privileged mechanism，而不是固定某一种完整平台实现。:chatgpt-content-reference{index="0"}

---

# 二、所以 RISC-V Interrupt 实际上是在解决 6 个问题

我们先不看任何代码。

假设：

```text
UART 收到字符 'A'
```

整个系统必须依次回答：

```text
① 发生了什么事件？
      ↓
UART IRQ

② 这个设备 IRQ 发给谁？
      ↓
PLIC → Hart 0

③ 它应该交给哪个 privilege level？
      ↓
S-mode

④ CPU 当前允许接受它吗？
      ↓
enable / privilege check

⑤ CPU 接受以后跳到哪里？
      ↓
stvec

⑥ ISR 怎么知道到底是哪个设备？
      ↓
PLIC claim → IRQ 10
```

于是你之前觉得很散的那些机制，其实刚好对应：

| 问题 | RISC-V 机制 |
|---|---|
| IRQ 来了吗？ | Device / PLIC pending |
| 发给哪个 hart？ | PLIC routing/context |
| M 还是 S 处理？ | `mideleg` |
| 这种 interrupt 开了吗？ | `mie/sie` |
| 当前允许被打断吗？ | `mstatus.MIE/sstatus.SIE` |
| handler 在哪里？ | `mtvec/stvec` |
| 为什么进入 trap？ | `mcause/scause` |
| 具体哪个外设？ | PLIC claim |
| 处理结束了吗？ | device clear + PLIC complete |

这时候整个体系其实已经开始连起来了。

---

# 三、我们搭一个真实 Demo

场景：

```text
QEMU virt
RV64
Hart 0

M-mode:
    OpenSBI / minimal firmware

S-mode:
    我们自己的 mini kernel

Device:
    NS16550 UART

Interrupt controller:
    PLIC
```

QEMU `virt` 当前仍提供 NS16550 UART，并通过自动生成的 DTB 描述设备地址和 interrupt line；传统非 AIA 配置使用 PLIC。:chatgpt-content-reference{index="1"}

我们使用经典配置：

```text
UART0
PA = 0x10000000
IRQ = 10

PLIC
PA = 0x0c000000
```

MIT xv6 在 QEMU virt 上也是使用这套布局。:chatgpt-content-reference{index="2"}

整个目标只有一个：

> 在终端输入 `A`，我们的 S-mode mini kernel 收到 UART interrupt，并打印 `"UART IRQ: A"`。

---

# 四、先看最终完整数据流

你先不要管代码。

把这条线理解透：

```text
用户键入 'A'
       │
       ▼
+----------------+
| NS16550 UART   |
| RX FIFO = 'A'  |
+----------------+
       │
       │ UART IRQ = 10
       ▼
+----------------+
|      PLIC      |
| source 10      |
| pending = 1    |
+----------------+
       │
       │ arbitration
       │ priority
       │ enable
       │ threshold
       ▼
Hart0 S-context
       │
       ▼
   SEIP = 1
       │
       ▼
+--------------------------------+
|          Hart 0                |
|                                |
| sip.SEIP = 1                   |
| sie.SEIE = 1                   |
| interrupt delegated to S       |
| privilege allows delivery      |
+--------------------------------+
       │
       ▼
   TAKE TRAP
       │
       ├── sepc   ← interrupted PC
       ├── scause ← interrupt | 9
       ├── SPP    ← previous mode
       ├── SPIE   ← SIE
       ├── SIE    ← 0
       │
       ▼
    PC = stvec
       │
       ▼
S-mode trap handler
       │
       ▼
read scause
       │
       │ "Supervisor external interrupt"
       ▼
PLIC claim
       │
       │ returns 10
       ▼
UART handler
       │
       ├── read RX FIFO
       └── get 'A'
       │
       ▼
PLIC complete(10)
       │
       ▼
     sret
       │
       ▼
continue interrupted code
```

**RISC-V Interrupt architecture 基本就是这张图。**

下面开始逐层理解为什么要这样设计。

---

# 五、第一层：UART 为什么不能直接告诉 CPU “我是 IRQ 10”？

你可能首先想到：

```text
UART
  │
 IRQ10
  │
  └────────────→ CPU
```

但现实中一个 SoC 有：

```text
UART
SPI
I2C
Ethernet
PCIe
USB
GPU
DMA
...
```

可能数百个 interrupt source。

CPU 不应该为每一个 device 都设计一个独立的 architectural interrupt pin。

所以首先做一次汇聚：

```text
UART ──────┐
SPI ───────┤
Ethernet ──┤
DMA ───────┤
           ▼
          PLIC
           │
           ▼
   External Interrupt
           │
           ▼
          Hart
```

于是发生了第一个重要抽象：

> **CPU 不关心 UART/SPI/Ethernet，它只知道“External Interrupt”。**

这解释了为什么：

```text
scause = 9
```

只表示：

```text
Supervisor External Interrupt
```

而不是：

```text
UART interrupt
```

具体是谁，要再问 PLIC。

这是一种非常典型的**分层抽象**：

```text
Hart architecture:

    External Interrupt
          ↑
          │
────────── boundary ──────────
          │
          ↓
Platform:

 UART / SPI / Ethernet / ...
```

这就是为什么 RISC-V 可以换：

```text
PLIC
→ APLIC
→ IMSIC
→ vendor interrupt controller
```

而 CPU Trap architecture 不需要全部重做。

---

# 六、因此 PLIC 的本质不是“Trap controller”

这是理解 PLIC 最关键的一点。

PLIC 更准确的角色是：

> **大量 Platform IRQ → 少量 Hart external interrupt inputs 的 router + arbitrator。**

例如：

```text
IRQ 10 UART     priority 1 ─┐
IRQ 11 Ethernet priority 3 ─┤
IRQ 12 DMA      priority 2 ─┤
                            │
                            ▼
                           PLIC
                            │
                    choose destination
                            │
              ┌─────────────┴─────────────┐
              ▼                           ▼
          Hart0 S                     Hart1 S
             │
            SEIP                         SEIP
```

PLIC 决定的是：

```text
WHO:
哪个 source

WHERE:
送哪个 hart/context

PRIORITY:
谁先送
```

而不是：

```text
CPU trap entry 怎么保存 PC？
SIE 怎么处理？
scause 怎么写？
```

那些全部属于 hart privileged architecture。

---

# 七、第二层：PLIC 选择 Hart0 S-context

假设 UART IRQ 是：

```text
ID = 10
```

我们初始化：

```c
#define PLIC_BASE      0x0c000000UL

#define UART_IRQ       10

#define PLIC_PRIORITY(id) \
        (*(volatile uint32_t *)(PLIC_BASE + 4 * (id)))

#define PLIC_SENABLE0 \
        (*(volatile uint32_t *)(PLIC_BASE + 0x2080))

#define PLIC_STHRESHOLD \
        (*(volatile uint32_t *)(PLIC_BASE + 0x201000))

#define PLIC_SCLAIM \
        (*(volatile uint32_t *)(PLIC_BASE + 0x201004))
```

这些 offset 和 QEMU virt Hart0 S-mode context 的经典 PLIC 布局一致。:chatgpt-content-reference{index="3"}

初始化：

```c
void plic_init(void)
{
    /* UART interrupt priority */
    PLIC_PRIORITY(UART_IRQ) = 1;

    /* Enable UART IRQ 10 for Hart0 S-context */
    PLIC_SENABLE0 |= (1U << UART_IRQ);

    /* Accept every interrupt whose priority > 0 */
    PLIC_STHRESHOLD = 0;
}
```

这里实际上在做：

```text
IRQ10 enabled?
       YES
        │
priority = 1
        │
1 > threshold(0)?
        YES
        │
        ▼
Hart0 S-context
```

于是 PLIC 对 Hart0 表达：

> **“你有一个 Supervisor External Interrupt。”**

---

# 八、这里发生一个非常关键的“信息压缩”

PLIC 内部知道：

```text
IRQ = 10
```

但 hart 暂时不知道。

Hart 只看到：

```text
SEIP = 1
```

也就是：

```text
Supervisor External Interrupt Pending
```

可以理解成：

```text
PLIC:

    “有事！”

Hart:

    “什么事？”

PLIC:

    “等你进 handler 后再来 claim。”
```

于是：

```text
Platform IRQ ID 10
        ↓
       PLIC
        ↓
 architectural identity
        ↓
       SEI
        ↓
   scause = 9
```

这就是 **major interrupt identity / source identity 分层**。

AIA 仍然延续了这种思想。:chatgpt-content-reference{index="4"}

---

# 九、第三层：为什么还有 `mideleg`？

现在 interrupt 已经到 Hart0。

还有一个问题没有解决：

> 谁拥有这个 interrupt？

系统中：

```text
M-mode
    OpenSBI

S-mode
    OS

U-mode
    Application
```

UART 显然应该由 OS driver 处理：

```text
Linux / NuttX / mini kernel
```

而不是 OpenSBI。

于是需要建立：

```text
interrupt ownership
```

这就是：

```text
mideleg
```

真正的设计意义。

---

# 十、你可以把 `mideleg` 理解成“纵向路由器”

PLIC 做的是：

```text
空间路由：

IRQ
 ↓
Hart0 / Hart1 / Hart2
```

而 `mideleg` 做的是：

```text
Privilege 路由：

Interrupt
   ↓
M-mode
or
S-mode
```

所以整个路由实际上是二维的：

```text
                 Spatial routing
                   PLIC
                     │
        ┌────────────┼─────────────┐
        ▼            ▼             ▼
      Hart0        Hart1         Hart2
        │
        │
        │ privilege routing
        │ mideleg
        │
      ┌─┴─┐
      ▼   ▼
      M   S
```

这一点非常关键。

**PLIC 和 `mideleg` 根本不是重复机制。**

一个决定：

> 哪个 CPU？

一个决定：

> CPU 上哪个 privilege domain？

---

# 十一、M-mode 做一次 ownership 配置

例如启动阶段：

```c
#define MIP_SEIP (1UL << 9)

void m_init(void)
{
    unsigned long x;

    asm volatile("csrr %0, mideleg" : "=r"(x));

    x |= MIP_SEIP;

    asm volatile("csrw mideleg, %0" : : "r"(x));
}
```

意思：

```text
Supervisor External Interrupt
           │
           │ delegated
           ▼
        S-mode
```

官方定义也是：

一旦某个 interrupt bit 在 `mideleg` 中设置，相应 interrupt 就成为 S-mode 可见的 `sip/sie` interrupt。:chatgpt-content-reference{index="5"}

于是 OpenSBI 不需要：

```text
UART interrupt
   ↓
OpenSBI
   ↓
OpenSBI解析
   ↓
再通知Linux
```

而可以：

```text
UART
 ↓
PLIC
 ↓
S-mode
 ↓
OS driver
```

---

# 十二、这背后体现了 RISC-V 一个很重要的设计原则

> **Higher privilege 配置 policy，lower privilege 执行 fast path。**

M-mode：

```text
配置：
“这个 interrupt 属于 S”
```

之后就退出 fast path：

```text
M-mode
    × 每次 IRQ 不参与
```

S-mode：

```text
直接处理 UART
```

这非常重要。

否则每个 device interrupt 都：

```text
Device
 ↓
M
 ↓
S
```

会产生大量 privilege transition。

所以 delegation 不只是：

> 一个 CSR feature。

它实际上表达的是：

> **RISC-V privilege architecture 如何把 mechanism 和 policy 分开。**

---

# 十三、第四层：为什么又有这么多 Enable？

现在来到很多人最容易困惑的地方。

你会看到：

```text
UART interrupt enable

PLIC enable

sie.SEIE

sstatus.SIE
```

怎么这么多？

实际上它们不是重复的。

它们分别控制不同层次。

---

# 十四、第一层 Enable：Device Enable

UART 自己首先要允许 RX IRQ：

```c
#define UART_BASE 0x10000000UL

#define UART_IER 1

void uart_enable_rx_irq(void)
{
    volatile uint8_t *ier =
        (volatile uint8_t *)(UART_BASE + UART_IER);

    *ier |= 1;
}
```

意思是：

```text
UART:

RX data available
       │
       ▼
要不要产生 IRQ？
```

如果这里关闭：

```text
UART
 ×
```

PLIC 根本不知道发生了什么。

---

# 十五、第二层 Enable：PLIC Enable

PLIC：

```text
IRQ 10 UART
```

可能允许 Hart0 接收，也可能不允许。

```text
UART IRQ
   │
   ▼
PLIC source 10

enable[Hart0 S] = 1
      │
      ▼
deliver to Hart0
```

这是：

> **platform routing policy**

---

# 十六、第三层 Enable：`sie.SEIE`

即：

```text
Supervisor Interrupt Enable
```

里的：

```text
SEIE
Supervisor External Interrupt Enable
```

代码：

```c
#define SIE_SEIE (1UL << 9)

static inline void enable_sei(void)
{
    asm volatile(
        "csrs sie, %0"
        :
        : "r"(SIE_SEIE)
        : "memory"
    );
}
```

意思：

> 我这个 S-mode software 是否接受“External Interrupt”这一类 interrupt？

注意它已经不区分 UART/Ethernet：

```text
PLIC:
UART level

sie:
External Interrupt class level
```

---

# 十七、第四层：`sstatus.SIE`

这个才相当于：

> **当前 S-mode execution context 的 global mask。**

```c
#define SSTATUS_SIE (1UL << 1)

asm volatile("csrs sstatus, %0"
             :
             : "r"(SSTATUS_SIE)
             : "memory");
```

于是形成四道门：

```text
UART RX
   │
   │ Device IER
   ▼
UART IRQ
   │
   │ PLIC source enable
   ▼
SEIP
   │
   │ sie.SEIE
   ▼
eligible S interrupt
   │
   │ privilege / sstatus.SIE
   ▼
TRAP
```

---

# 十八、为什么设计这么多层？

因为它们解决的问题完全不同：

```text
Device enable
    ↓
“这个设备是否产生 IRQ？”

PLIC enable
    ↓
“这个 IRQ 发给哪个 hart？”

sie.SEIE
    ↓
“这个 hart 是否接受 external interrupt 这一类事件？”

sstatus.SIE
    ↓
“当前执行上下文现在允许被 interrupt 吗？”
```

这其实是非常干净的层次化设计。

比如 OS 进入 critical section：

```c
local_irq_disable();
```

它不应该去修改：

```text
每一个 UART
每一个 SPI
每一个 Ethernet
每一个 PLIC enable
```

只需要：

```text
SIE = 0
```

就可以临时关闭 local S-mode interrupt delivery。

这就是为什么需要 global interrupt enable。

---

# 十九、现在 UART 真正收到字符 'A'

我们终于开始 runtime。

CPU 当前假设正在：

```text
U-mode
```

运行 application：

```asm
loop:
    add a0, a0, a1
    ld  t0, 0(sp)
    ...
```

突然：

```text
UART RX FIFO receives 'A'
```

UART：

```text
RX ready
+
IER.RX = 1
```

于是：

```text
IRQ10 = asserted
```

进入 PLIC。

---

# 二十、PLIC 内部做什么？

可以概念化成：

```text
source 10
   │
   ▼
pending[10] = 1
   │
   ▼
priority[10] = 1
   │
   ▼
enable[Hart0 S][10] = 1
   │
   ▼
threshold = 0
   │
   ▼
1 > 0
   │
   ▼
Hart0 S-context has interrupt
```

然后：

```text
Hart0:

SEIP = 1
```

---

# 二十一、这里终于进入 Hart Interrupt Architecture

现在 hardware 看到：

```text
sip.SEIP = 1
sie.SEIE = 1
```

问题：

> CPU 要不要现在立刻跳到 S-mode？

这里就出现 RISC-V Interrupt 最值得深入理解的一条规则。

---

# 二十二、如果当前正在 U-mode，`SIE` 居然不重要

假设：

```text
Current privilege = U
```

interrupt target：

```text
S
```

因为：

```text
S > U
```

所以只要：

```text
sip.SEIP = 1
sie.SEIE = 1
```

这个 S-mode interrupt 就可以进入。

**即使：**

```text
sstatus.SIE = 0
```

也不会阻止 S-mode interrupt 从 U-mode 抢占。

官方规则明确规定：

S-mode interrupt 可以触发，如果：

```text
current mode < S
```

或者：

```text
current mode == S && SIE == 1
```

并且对应的 `sip/sie` bit 都为 1。:chatgpt-content-reference{index="6"}

---

# 二十三、这一点非常重要

很多介绍会简单写：

```text
sip.SEIP
&&
sie.SEIE
&&
sstatus.SIE
```

这其实是不完整的。

正确逻辑更接近：

```c
if (sip.SEIP &&
    sie.SEIE &&
    (
        current_priv < S ||
        (current_priv == S && sstatus.SIE)
    ))
{
    take_interrupt();
}
```

这揭示了一个非常漂亮的设计：

> **低 privilege software 不能通过关闭高 privilege 的 global interrupt enable 来阻止高 privilege interrupt。**

例如：

```text
U-mode
```

当然不应该有办法：

```text
“我把 S-mode interrupt 全屏蔽掉”
```

否则 U-mode app 可以 DoS kernel。

---

# 二十四、和 M-mode 完全一样

假设：

```text
Current = S
Target interrupt = M
```

即使：

```text
mstatus.MIE = 0
```

一个 enabled、non-delegated M interrupt 仍可进入 M-mode。

只有：

```text
Current = M
```

时：

```text
mstatus.MIE
```

才决定 M-mode 是否允许被同级 interrupt 打断。官方 M-mode interrupt 条件就是这种设计。:chatgpt-content-reference{index="7"}

所以 `xIE` 真正的含义应该理解成：

> **“当前正在 x-mode 时，是否允许同 privilege 的 interrupt。”**

而不是简单理解成：

> “整个系统的 interrupt master switch。”

这个区别非常重要。

---

# 二十五、Hardware 决定 Take Interrupt

现在满足：

```text
SEIP = 1
SEIE = 1
Target = S
Current = U
```

CPU 开始 Trap Entry。

这里 RISC-V 做的事情非常克制。

它不会像 Cortex-M 那样：

```text
push x registers
push PC
push status
...
```

只保存最少 architectural state。

---

# 二十六、Trap Entry 到底做了什么？

概念上：

```text
sepc ← 当前被中断的 PC

scause ←
    Interrupt bit = 1
    Cause = 9

sstatus.SPP ← previous privilege
             = U

sstatus.SPIE ← sstatus.SIE

sstatus.SIE ← 0

PC ← stvec
```

于是：

```text
           before

PC = user instruction
priv = U


            │ interrupt
            ▼


           after

PC = stvec
priv = S

sepc = old user PC
scause = interrupt | 9
SPP = U
SIE = 0
```

---

# 二十七、为什么 RISC-V 不自动保存所有寄存器？

这是一个重要设计哲学。

RISC-V 倾向：

> **hardware 做最小 mechanism，software 决定 context policy。**

如果 hardware 自动保存：

```text
x1~x31
```

那么即使某个 extremely small handler 只需要：

```text
t0
t1
```

也必须付出完整保存开销。

而不同 OS：

```text
Linux
RTOS
hypervisor
bare-metal
```

需要的 context 完全不同。

所以 RISC-V：

```text
hardware:
    save minimum trap metadata

software:
    decide register save strategy
```

这和 Cortex-M 是明显不同的哲学。

---

# 二十八、Trap Vector

初始化：

```c
extern void s_trap_vector(void);

void trap_init(void)
{
    uintptr_t addr = (uintptr_t)s_trap_vector;

    asm volatile("csrw stvec, %0"
                 :
                 : "r"(addr));
}
```

假设：

```text
stvec.MODE = Direct
```

那么：

```text
all S-mode traps
      ↓
s_trap_vector
```

---

# 二十九、汇编入口

一个简化版：

```asm
.align 4
.globl s_trap_vector

s_trap_vector:

    addi sp, sp, -128

    sd ra,   0(sp)
    sd t0,   8(sp)
    sd t1,  16(sp)
    sd t2,  24(sp)
    sd a0,  32(sp)
    sd a1,  40(sp)

    call s_trap_handler

    ld ra,   0(sp)
    ld t0,   8(sp)
    ld t1,  16(sp)
    ld t2,  24(sp)
    ld a0,  32(sp)
    ld a1,  40(sp)

    addi sp, sp, 128

    sret
```

真实 OS 会保存更加完整的 context。

但这个 demo 已经体现：

```text
hardware trap entry
       ↓
software context save
       ↓
C handler
       ↓
software restore
       ↓
sret
```

---

# 三十、C handler 第一件事情：读 `scause`

```c
void s_trap_handler(void)
{
    unsigned long scause;

    asm volatile("csrr %0, scause"
                 : "=r"(scause));

    ...
}
```

RV64：

```text
bit63 = Interrupt

bits62:0 = cause
```

于是：

```c
#define SCAUSE_INTERRUPT (1UL << 63)

if (scause & SCAUSE_INTERRUPT) {
    unsigned long cause =
        scause & ~SCAUSE_INTERRUPT;

    if (cause == 9) {
        handle_external_irq();
    }
}
```

这里得到：

```text
cause = 9
```

说明：

```text
Supervisor External Interrupt
```

---

# 三十一、现在出现整个架构最关键的第二次查询

CPU：

```text
我知道：

External Interrupt

但到底是谁？
```

于是问：

```text
PLIC
```

代码：

```c
uint32_t plic_claim(void)
{
    return PLIC_SCLAIM;
}
```

得到：

```text
10
```

于是：

```c
void handle_external_irq(void)
{
    uint32_t irq = plic_claim();

    if (irq == UART_IRQ) {
        uart_irq_handler();
    }

    if (irq != 0) {
        plic_complete(irq);
    }
}
```

---

# 三十二、到这里可以看到非常漂亮的两级 dispatch

第一级：

```text
scause
   │
   ▼
What kind of interrupt?

timer?
software?
external?
```

第二级：

```text
PLIC claim
   │
   ▼
Which external device?

UART?
SPI?
Ethernet?
```

也就是：

```text
                       Interrupt
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
           Timer        Software      External
                                        │
                                      PLIC
                                        │
                          ┌─────────────┼────────────┐
                          ▼             ▼            ▼
                        UART           SPI         Ethernet
```

这个分层特别重要。

---

# 三十三、为什么不让 `scause` 直接保存 IRQ10？

因为这样 ISA 就会和 platform device topology 耦合。

如果：

```text
scause = UART10
```

那么：

```text
有多少设备？
IRQ number 多宽？
PCIe MSI 怎么编码？
SoC vendor IRQ 怎么定义？
虚拟机 IRQ 怎么办？
```

全部会污染 CPU architecture。

RISC-V 的选择是：

```text
ISA only knows:

software
timer
external
...
```

platform controller knows：

```text
UART = 10
Ethernet = 11
PCIe = ...
```

于是：

```text
CPU architecture
```

和：

```text
SoC interrupt topology
```

解耦。

这非常符合 RISC-V 的模块化设计哲学。

---

# 三十四、UART ISR 真正清 interrupt source

例如 NS16550：

```c
#define UART_RHR 0

char uart_getc(void)
{
    volatile uint8_t *rhr =
        (volatile uint8_t *)(UART_BASE + UART_RHR);

    return *rhr;
}
```

于是：

```c
void uart_irq_handler(void)
{
    char c = uart_getc();

    printk("UART IRQ: %c\n", c);
}
```

读取 receive holding register 后：

```text
RX condition 被处理
```

UART 可以 deassert IRQ。

---

# 三十五、然后为什么还需要 `PLIC complete`？

```c
void plic_complete(uint32_t irq)
{
    PLIC_SCLAIM = irq;
}
```

Claim/Complete 是 PLIC 自己的 interrupt transaction state。

逻辑可以理解：

```text
Device:

    “我的中断条件已经处理了。”


PLIC:

    “CPU 对 source 10 的这次 interrupt transaction
     也处理结束了。”
```

这两个不是同一件事情。

---

# 三十六、这也是驱动开发中很重要的一层区别

有三个状态：

```text
Device source state

        ↓

PLIC source state

        ↓

Hart external interrupt state
```

例如 UART 是 level interrupt。

如果你：

```text
PLIC complete
```

但没有读取 UART FIFO：

```text
UART IRQ line
     still asserted
```

那么：

```text
PLIC
  ↓
再次 pending
  ↓
再次 interrupt
```

于是形成：

```text
interrupt storm
```

这和你以前调 NuttX driver 的经验应该非常一致。

---

# 三十七、最后 `sret`

ISR 结束：

```asm
sret
```

hardware：

```text
privilege ← SPP

SIE ← SPIE

PC ← sepc
```

因为：

```text
SPP = U
```

最终：

```text
S-mode
  ↓ sret
U-mode
```

继续刚才被打断的：

```asm
ld t0, 0(sp)
```

或者下一条 architectural instruction。

---

# 三十八、现在重新看完整代码，逻辑就非常清楚了

初始化：

```c
void irq_init(void)
{
    /*
     * 1. Trap destination
     */
    write_csr(stvec, s_trap_vector);


    /*
     * 2. Device generates IRQ
     */
    uart_enable_rx_irq();


    /*
     * 3. Platform routing
     */
    PLIC_PRIORITY(UART_IRQ) = 1;

    PLIC_SENABLE0 |=
        1U << UART_IRQ;

    PLIC_STHRESHOLD = 0;


    /*
     * 4. Hart accepts external interrupt class
     */
    set_csr(sie, SIE_SEIE);


    /*
     * 5. Allow same-level S interrupts
     */
    set_csr(sstatus, SSTATUS_SIE);
}
```

你现在应该能看到每一行属于不同 architecture layer：

```text
UART enable
    │
    └── device layer


PLIC priority/enable
    │
    └── platform interrupt layer


sie.SEIE
    │
    └── hart interrupt-class enable


sstatus.SIE
    │
    └── privilege execution state


stvec
    │
    └── trap dispatch
```

所以它们其实一点都不乱。

---

# 三十九、把整个 RISC-V Interrupt Architecture 抽象成三个平面

这是我认为 Staff Engineer 层面非常值得形成的 mental model。

## 第一：Source Plane

回答：

> **发生了什么？**

```text
UART
timer
IPI
PCIe MSI
GPIO
...
```

---

## 第二：Routing / Ownership Plane

回答：

> **谁来处理？**

有两个维度：

```text
             physical destination
                     │
                    PLIC
                     │
             Hart0 / Hart1
                     │
                     │
             privilege ownership
                     │
                  mideleg
                     │
                 M or S
```

---

## 第三：Execution Plane

回答：

> **什么时候进入 handler，怎么进入？**

```text
pending
+
enable
+
current privilege
+
xstatus.xIE
      │
      ▼
Trap Entry
      │
      ├── xepc
      ├── xcause
      ├── xstatus
      └── xtvec
```

于是完整 architecture：

```text
                    SOURCE PLANE

              UART / TIMER / IPI
                       │
                       ▼


               ROUTING PLANE

 Device IRQ
     │
    PLIC
     │
 Which Hart?
     │
    Hart0
     │
  mideleg
     │
 Which privilege?
     │
   S-mode
     │
     ▼


              EXECUTION PLANE

   pending + enable
          │
 privilege check
          │
      trap entry
          │
      stvec
          │
       ISR
```

这比记一堆 CSR 要重要得多。

---

# 四十、为什么 Timer 和 Software Interrupt 不经过 PLIC？

现在你应该能够自然推导出来。

Timer 通常已经是：

```text
per-hart resource
```

IPI 也是：

```text
hart → hart
```

它们不需要回答：

> 这是 UART 还是 Ethernet？

也不需要一个大型 platform IRQ multiplexer。

于是：

```text
Timer
   │
   └────────────→ STIP / MTIP


Software/IPI
   │
   └────────────→ SSIP / MSIP
```

而 external devices：

```text
UART
SPI
Ethernet
...
  │
  ▼
PLIC/APLIC/IMSIC
  │
  ▼
SEIP/MEIP
```

于是 Software / Timer / External 三分法突然就不再是“几个 feature”。

它其实来自：

> **interrupt source topology 不同。**

---

# 四十一、这就是三类 interrupt 的设计本质

```text
                       Hart

              ┌─────────┼─────────┐
              │         │         │
             IPI       Timer    External
              │         │         │
              │         │         │
         Hart-local   Hart-local  Platform-wide
                                  many devices
                                      │
                                  need router
                                      │
                                     PLIC
```

这比背：

```text
SSI/STI/SEI
MSI/MTI/MEI
```

有意义得多。

---

# 四十二、那 AIA 为什么又出现？

现在也可以从同一个模型自然推导。

PLIC 的问题在于它是：

```text
central interrupt router
+
claim/complete
```

现代系统越来越多：

```text
PCIe MSI
MSI-X
hundreds of queues
VM
device passthrough
```

这些设备本身已经可以发：

```text
memory write based interrupt
```

再：

```text
MSI
 ↓
central PLIC
 ↓
claim
```

并不理想。

所以 AIA 不是“又发明一套 interrupt”。

它只是改了：

```text
Routing Plane
```

Hart Trap Plane 基本思想仍然保持。

官方 AIA 的明确目标之一就是在已有 privileged interrupt architecture 上增加原生 MSI 支持，而不是完全替换原有模型。:chatgpt-content-reference{index="8"}

---

# 四十三、从 PLIC 到 AIA，其实只是这一段发生改变

PLIC：

```text
Device
   │
wired IRQ
   ▼
PLIC
   │
   ▼
Hart external interrupt
```

AIA：

```text
wired device
    │
    ▼
  APLIC
    │
   MSI
    ▼
  IMSIC
    │
    ▼
Hart external interrupt
```

或者 PCIe：

```text
PCIe Device
     │
     │ MSI
     ▼
   IMSIC
     │
     ▼
Hart external interrupt
```

后面：

```text
pending
enable
privilege
trap
stvec
scause
```

这个 conceptual pipeline 没有消失。

所以 AIA 就不会再显得像完全独立的一章。

---

# 四十四、和 Arm GIC 对比一下，你会理解得更深

Arm 倾向把很多东西组织进：

```text
GIC
├── Distributor
├── Redistributor
├── CPU interface
├── priority
├── SGI/PPI/SPI
└── virtualization support
```

而 RISC-V 的分层更明显：

```text
                 RISC-V

Platform:
    PLIC/APLIC/IMSIC

           ↓

Hart architecture:
    xip/xie
    privilege
    delegation
    xtvec
    xcause
```

所以不要问：

> “RISC-V 哪个东西 == GIC？”

更准确的问题是：

> **“Arm GIC 中某项职责，在 RISC-V 被放在哪一层？”**

比如：

```text
device routing
    → PLIC/APLIC

MSI reception
    → IMSIC

privilege interrupt acceptance
    → Hart CSR

trap vector
    → xtvec

trap reason
    → xcause
```

这样对应就准确多了。

---

# 四十五、最后给你一个我认为最重要的 RISC-V Interrupt 心智模型

以后看到任何 RISC-V Interrupt 问题，都从这 7 步思考：

```text
① Source
   谁产生 interrupt？
          │
          ▼

② Route
   发给哪个 hart？
          │
          ▼

③ Ownership
   M / S / VS 谁拥有它？
          │
          ▼

④ Pending
   interrupt 是否已经到达？
          │
          ▼

⑤ Enable / Privilege
   当前是否允许接受？
          │
          ▼

⑥ Trap
   保存什么状态？
   跳到哪里？
          │
          ▼

⑦ Service / Acknowledge
   是谁产生的？
   source 怎么清？
```

映射成 RISC-V：

```text
① Source
      UART / timer / IPI

② Route
      PLIC / APLIC / IMSIC

③ Ownership
      mideleg / hideleg

④ Pending
      mip / sip

⑤ Enable
      mie / sie
      mstatus / sstatus

⑥ Trap
      xepc
      xcause
      xstatus
      xtvec

⑦ Service
      claim
      device ISR
      complete
      xret
```

**这就是整个 RISC-V Interrupt Architecture。**

---

## 最值得你真正理解的设计理念

如果把这一章进一步压缩，我认为 RISC-V Interrupt 背后有四个核心思想：

```text
1. ISA 与 platform 解耦

Hart 只理解：
    software / timer / external

而不知道：
    UART / Ethernet / PCIe


2. Routing 与 Trap 解耦

PLIC：
    谁来处理？

Hart：
    什么时候进入 handler？


3. Privilege ownership 是独立维度

PLIC：
    Hart0 / Hart1

mideleg：
    M / S


4. Hardware mechanism minimal，
   software policy explicit

hardware：
    pending
    enable
    trap state

software：
    routing policy
    nesting policy
    context save
    device dispatch
```

尤其第 4 点，实际上贯穿整个 RISC-V：

```text
Trap
Interrupt
PMP
Page Table
SBI
Hypervisor
```

都能看到这种 **“提供最小硬件 mechanism，把大量 policy 留给 software”** 的倾向。

如果你把今天这个 **`UART → PLIC → SEIP → scause → claim → UART ISR → complete → sret`** 的链真正吃透，那么以后再学 Timer、IPI、AIA、IMSIC，本质上都只是在替换其中某一段，而不再是学习一堆互不关联的新 feature。