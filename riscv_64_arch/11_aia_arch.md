下面这份我会按照你已经完成 **01~10 阶段** 的前提来讲，因此默认你已经理解 M/S/U、Trap、Interrupt、Sv39、SBI、SMP 和 H extension。AIA 正好是把前面这些知识在“现代高性能中断子系统”上串起来的一章。

# RISC-V Architecture Stage 11：AIA — Advanced Interrupt Architecture

## 0. 这一章最重要的结论

先把 AIA 压缩成一句话：

> **AIA 的本质，是把 RISC-V 外部中断架构从“中央 Interrupt Controller 拉一根 interrupt wire 给 CPU”的模型，升级成“设备通过 MSI 向目标 hart / privilege / virtual hart 的本地 interrupt file 投递消息”的模型。**

核心结构是：

```text
                       AIA
                        |
             +----------+----------+
             |                     |
           APLIC                  IMSIC
             |                     |
      wired interrupt          MSI receiver
      routing/convert          per-hart
             |                     |
             +------ MSI ----------+
                                   |
                                Hart
```

所以最重要的两个组件是：

```text
APLIC
Advanced Platform-Level Interrupt Controller

IMSIC
Incoming Message-Signaled Interrupt Controller
```

你可以暂时把它们理解为：

```text
APLIC = wired interrupt 前端

IMSIC = hart 本地 MSI interrupt controller
```

但这只是第一层理解。

真正理解 AIA 后，你应该形成这样的 mental model：

```text
            interrupt source
                  |
        +---------+---------+
        |                   |
      wire                 MSI
        |                   |
        v                   |
      APLIC                 |
        |                   |
        | convert to MSI    |
        +---------+---------+
                  |
                  v
                IMSIC
                  |
                  v
           interrupt file
                  |
                  v
         MEIP / SEIP / VSEIP
                  |
                  v
             trap handler
```

AIA v1.0 已 ratified；官方当前文档是 AIA 1.0，并带有 2025-03-12 的说明性修订。AIA 本身不仅包含 APLIC/IMSIC，还包含 hart 侧 CSR 扩展、virtual interrupt、IPI 和 IOMMU/MSI virtualization 等机制。:chatgpt-content-reference{index="0"}

---

# 1. 为什么已经有 PLIC，还需要 AIA？

这是理解整个架构的第一步。

传统 RISC-V 平台常见结构：

```text
 UART ----\
 SPI ------\
 Ethernet ---> PLIC ----> Hart0
 PCIe INTx -/      \----> Hart1
                  \-----> Hart2
```

PLIC 做三件事情：

```text
interrupt aggregation
        +
priority
        +
hart routing
```

然后 PLIC 用一个 external interrupt 信号通知 hart：

```text
PLIC
 |
 +---- MEIP / SEIP ----> hart
```

问题是，这种设计本质上仍然是：

> **shared centralized interrupt controller**

在小型 SoC 中很好用。

但是到了：

```text
64 cores
128 cores
256 cores

PCIe
NVMe
100G NIC

VM
device passthrough
SR-IOV
```

问题就出现了。

---

# 2. PLIC 模型的根本限制

假设：

```text
PCIe NIC
   |
   | interrupt
   v
 PLIC
   |
   v
Hart 37
```

CPU 收到的只是：

```text
Supervisor External Interrupt
```

也就是：

```text
scause = 9
```

CPU 并不知道：

```text
哪个设备？
哪个 interrupt vector？
```

于是必须访问 PLIC：

```text
CPU
 |
 | MMIO
 v
PLIC claim register
 |
 v
IRQ ID
```

处理完成之后：

```text
CPU
 |
 | MMIO
 v
PLIC complete
```

所以一次 interrupt：

```text
device
   ↓
PLIC
   ↓
CPU trap
   ↓
MMIO read PLIC
   ↓
handler
   ↓
MMIO write PLIC
```

这对于：

```text
大量 hart
+
高频设备 interrupt
+
虚拟机
```

就不是一个很漂亮的模型了。

---

# 3. AIA 的设计思想：从 wire interrupt 转向 MSI

AIA 的关键转变是：

```text
Interrupt
```

不再一定意味着：

```text
拉高一根 wire
```

而可以意味着：

```text
向一个特殊地址写一个 value
```

也就是：

```text
MSI
Message Signaled Interrupt
```

例如：

```text
Device:

store 23 -> 0x2800_0000
```

这个写操作的含义不是：

```text
RAM[0x28000000] = 23
```

而是：

```text
向某个 hart 的 IMSIC
发送 interrupt ID 23
```

于是：

```text
Device
   |
   | Memory Write
   | addr = Hart3 IMSIC
   | data = IRQ 23
   v
IMSIC@Hart3
   |
   v
IRQ 23 pending
```

这是理解 AIA 最核心的一步。

官方规范对 MSI 的定义也非常直接：IMSIC 拥有特定物理地址，向该地址进行规定格式的写操作，就表示向对应 interrupt file 发送一个 interrupt。:chatgpt-content-reference{index="1"}

---

# 4. 为什么 MSI 对现代系统非常重要？

因为它把：

```text
interrupt routing
```

转换成：

```text
memory routing
```

例如：

```text
Hart0 IMSIC address:
0x2800_0000

Hart1 IMSIC address:
0x2800_1000

Hart2 IMSIC address:
0x2800_2000

Hart3 IMSIC address:
0x2800_3000
```

如果设备要 interrupt Hart3：

```text
write -> 0x2800_3000
```

如果改成 Hart7：

```text
write -> 0x2800_7000
```

所以 interrupt affinity 本质上变成：

```text
MSI destination address
```

而不是：

```text
interrupt controller output wire
```

这和 PCIe 的世界天然契合。

现在 RISC-V Server SoC specification 已明确要求 AIA，并要求 external interrupt 以 MSI 的形式投递给 hart；如果 SoC 还有 wired interrupt，则 APLIC 要负责将其转换成 MSI。:chatgpt-content-reference{index="2"}

---

# 5. AIA 的三个层次

完整 AIA 不应该只理解成：

```text
APLIC + IMSIC
```

更准确应该看成：

```text
                 AIA
                  |
       +----------+----------+
       |          |          |
     Hart       IMSIC      APLIC
   extensions
```

也就是：

```text
1. Hart ISA/CSR extensions
2. IMSIC
3. APLIC
```

另外还定义：

```text
VM interrupt support
IPI
IOMMU / MSI interaction
```

AIA 定义了两个 ISA extension 名字：

```text
Smaia
Ssaia
```

其中：

```text
Smaia
= machine-level AIA ISA functionality

Ssaia
= supervisor-visible AIA ISA functionality
```

注意：

> **Smaia/Ssaia 并不意味着系统一定存在 APLIC。**

因为：

```text
APLIC = non-ISA component
```

同样 IOMMU 也不属于 Smaia/Ssaia。:chatgpt-content-reference{index="3"}

---

# 6. 先看整个 AIA architecture

典型 application processor：

```text
           wired devices
        UART / GPIO / ...
               |
               v
             APLIC
               |
               | MSI
               v
 +-------------+-------------+
 |             |             |
 v             v             v
IMSIC0        IMSIC1        IMSIC2
 |             |             |
Hart0         Hart1         Hart2
```

PCIe 设备则甚至可以：

```text
PCIe device
     |
     | MSI
     +------------------+
                        |
                        v
                    IMSIC2
                        |
                      Hart2
```

所以：

```text
wired device:
device → APLIC → IMSIC

MSI-native device:
device → IMSIC
```

这就是 AIA 最典型的 topology。:chatgpt-content-reference{index="4"}

---

# 7. APLIC 到底负责什么？

APLIC 全称：

```text
Advanced Platform-Level Interrupt Controller
```

最重要的理解：

> **APLIC 主要解决 wired interrupt。**

例如：

```text
UART IRQ
GPIO IRQ
legacy PCI interrupt
SoC peripheral IRQ
```

这些仍然是：

```text
wire
```

APLIC 接收：

```text
UART IRQ wire
      |
      v
    APLIC
```

然后决定：

```text
target hart
privilege level
interrupt identity
```

如果系统使用 IMSIC：

```text
APLIC
 |
 | MSI
 v
IMSIC
```

因此：

> **APLIC 可以看成 wired interrupt → MSI adapter/router。**

这是现代 AIA 系统中它最重要的角色。

---

# 8. APLIC 有两种 delivery mode

这一点很重要。

APLIC 支持：

```text
Direct mode
```

和：

```text
MSI mode
```

### Direct mode

```text
Device
  |
  v
APLIC
  |
  | SEIP/MEIP style signal
  v
Hart
```

这很像传统 PLIC。

### MSI mode

```text
Device
  |
  v
APLIC
  |
  | MSI write
  v
IMSIC
  |
  v
Hart
```

APLIC `domaincfg.DM` 控制 delivery mode：

```text
DM = 0
direct delivery

DM = 1
MSI delivery
```

AIA 允许实现只支持其中之一，也可以同时支持两种。:chatgpt-content-reference{index="5"}

现代高性能系统最值得关注的是：

```text
APLIC MSI mode
+
IMSIC
```

---

# 9. 为什么叫 APLIC，而不是 PLIC2？

因为它不仅仅是：

```text
better PLIC
```

APLIC 增加了非常重要的：

```text
interrupt domain
```

概念。

例如：

```text
             APLIC
               |
       Machine domain
               |
        delegation
               |
      Supervisor domain
```

于是 interrupt source：

```text
UART IRQ
```

可以属于：

```text
M domain
```

或者 delegate 到：

```text
S domain
```

这跟 RISC-V privilege architecture 的：

```text
M
↓
S
```

设计哲学非常一致。

---

# 10. APLIC sourcecfg

每个 wired IRQ source 都有：

```text
sourcecfg[i]
```

例如：

```text
sourcecfg[10]
```

代表 interrupt source 10。

它控制：

```text
edge / level
polarity
delegation
```

AIA 定义的主要 source mode 包括：

```text
Inactive

Detached

Edge1
rising edge

Edge0
falling edge

Level1
active high

Level0
active low
```

因此如果 UART IRQ：

```text
active-high level interrupt
```

可以配置：

```text
sourcecfg[UART] = Level1
```

这些 source mode 是规范明确规定的。:chatgpt-content-reference{index="6"}

---

# 11. APLIC target

APLIC 还需要知道：

```text
这个 IRQ 应该送给谁？
```

于是有：

```text
target[i]
```

概念上保存：

```text
target hart
+
priority / interrupt ID
+
guest index
```

具体字段取决于：

```text
direct mode
or
MSI mode
```

如果 MSI mode：

```text
source 25
   |
   v
target[25]
   |
   +-- hart = 3
   |
   +-- guest = 0
   |
   +-- EIID = 48
```

最终 APLIC 生成：

```text
MSI:
address = IMSIC@hart3
data    = 48
```

于是：

```text
IRQ source 25
```

在 Hart3 的 IMSIC 中变成：

```text
interrupt identity 48
```

---

# 12. APLIC 的 pending / enable 状态

基本模型与其他 interrupt controller 很类似：

```text
source
  |
  v
pending
  |
AND
  |
enable
  |
  v
delivery
```

因此存在：

```text
setip[]
clrip[]

setie[]
clrie[]
```

以及方便软件按 ID 操作的：

```text
setipnum
clripnum

setienum
clrienum
```

例如：

```text
setienum = 25
```

表示：

```text
enable IRQ 25
```

而不是对 bitmap 自己做 read-modify-write。

---

# 13. 现在进入最重要的 IMSIC

IMSIC：

```text
Incoming
Message-Signaled
Interrupt
Controller
```

最准确的 mental model 是：

> **IMSIC 是每个 hart 旁边的、本地 MSI mailbox + interrupt pending/enable controller。**

不是整个 SoC 一个 IMSIC。

而是：

```text
Hart0 ← IMSIC0

Hart1 ← IMSIC1

Hart2 ← IMSIC2

Hart3 ← IMSIC3
```

官方规范将 IMSIC 描述为 closely coupled with a hart，并规定每 hart 一个 IMSIC。:chatgpt-content-reference{index="7"}

---

# 14. IMSIC 与 Arm GICv3 的第一个类比

对你来说可以先建立：

```text
RISC-V                       Arm GICv3

APLIC                 ≈      Distributor 的
                               wired interrupt routing 部分

IMSIC                 ≈      Redistributor
                              + CPU-side interrupt state
                              + 部分 MSI endpoint 思想
```

但：

> **千万不要认为 IMSIC == GIC Redistributor。**

因为 GICv3：

```text
Distributor
Redistributor
ITS
CPU interface
```

之间的职责划分与 AIA 不一样。

真正应该抓住的是设计趋势：

```text
centralized interrupt state
             ↓
       more per-core state
             ↓
MSI-oriented interrupt delivery
```

这一点二者是相似的。

---

# 15. IMSIC 的核心：interrupt file

这是整个 AIA 最重要的概念之一。

IMSIC 不是只有一套：

```text
pending bitmap
enable bitmap
```

而是存在多个：

```text
interrupt file
```

例如一个支持 S-mode 的 hart：

```text
IMSIC
 |
 +-- M interrupt file
 |
 +-- S interrupt file
```

支持 H extension：

```text
IMSIC
 |
 +-- M interrupt file
 |
 +-- S interrupt file
 |
 +-- Guest interrupt file 1
 |
 +-- Guest interrupt file 2
 |
 +-- Guest interrupt file 3
 |
 +-- ...
```

也就是：

> interrupt 不仅指向一个 hart，还指向 hart 上的某个 privilege / virtual hart。

官方规范明确规定：MSI 指向的是特定 hart 上的特定 privilege level；支持 H extension 时还可以指向特定 guest interrupt file。:chatgpt-content-reference{index="8"}

这正是 AIA virtualizability 非常好的根本原因。

---

# 16. interrupt file 内部有什么？

可以把一个 interrupt file 想象成：

```text
Interrupt File
+-----------------------------+
| eidelivery                  |
+-----------------------------+
| eithreshold                 |
+-----------------------------+
| EIP                         |
| external interrupt pending  |
| 0 0 1 0 0 1 ...            |
+-----------------------------+
| EIE                         |
| external interrupt enable   |
| 0 1 1 0 0 1 ...            |
+-----------------------------+
```

其中：

```text
EIP
External Interrupt Pending

EIE
External Interrupt Enable
```

所以如果：

```text
EIP[37] = 1

EIE[37] = 1
```

说明：

```text
IRQ 37
pending + enabled
```

可以参与 interrupt delivery。

---

# 17. MSI 实际到底写到哪里？

每个 interrupt file 都占用一个：

```text
4 KiB physical address page
```

其中最重要的 MSI write port 是：

```text
offset 0x000:
seteipnum_le
```

所以：

```text
IMSIC_S_HART3 = 0x28003000
```

如果设备执行：

```c
*(uint32_t *)0x28003000 = 45;
```

概念上就是：

```text
Hart3 S-mode interrupt file:

EIP[45] = 1
```

官方规范规定一个 interrupt file 占一个自然对齐的 4 KiB physical address region，写入 `seteipnum_le` 的 interrupt identity 会置对应 pending bit。:chatgpt-content-reference{index="9"}

这就是 MSI。

没有神秘机制。

本质就是：

```text
MMIO write
↓
set pending bit
```

---

# 18. 用一个完整 MSI 例子理解

假设：

```text
NVMe
```

被分配：

```text
IRQ ID = 80

target CPU = hart2
```

Linux 配置 NVMe：

```text
MSI address:
Hart2 S-mode IMSIC page

MSI data:
80
```

NVMe 完成 IO：

```text
NVMe
 |
 | memory write
 | addr = IMSIC Hart2
 | data = 80
 v
IMSIC2
```

于是：

```text
EIP[80] = 1
```

如果：

```text
EIE[80] = 1
```

IMSIC 判断：

```text
pending
+
enabled
+
priority threshold OK
```

然后：

```text
SEIP = 1
```

Hart2：

```text
scause = interrupt | 9
```

进入：

```text
stvec
```

注意：

```text
scause = 9
```

仍然只表示：

```text
Supervisor External Interrupt
```

并没有直接告诉 CPU：

```text
IRQ 80
```

于是 CPU 要再访问：

```text
stopei
```

获得真正的：

```text
external interrupt identity = 80
```

---

# 19. major identity 和 minor identity

这是 AIA 非常重要的概念。

RISC-V CPU 层：

```text
scause = 9
```

这里：

```text
9
```

是：

```text
major interrupt identity
```

表示：

```text
Supervisor External Interrupt
```

而 IMSIC 里面：

```text
80
```

是：

```text
minor identity
```

表示：

```text
具体是哪一个 external interrupt
```

所以：

```text
scause
  ↓
major identity

IMSIC stopei
  ↓
minor identity
```

可以理解：

```text
major:
这是什么类别？

minor:
具体是谁？
```

规范中明确将 machine/supervisor/VS external interrupt 定义成 major identity 11/9/10，具体设备原因由 external interrupt controller 提供 minor identity。:chatgpt-content-reference{index="10"}

---

# 20. stopei：AIA 中你一定要记住的 CSR

S-mode：

```text
stopei
```

全称可以理解为：

```text
Supervisor Top External Interrupt
```

它告诉软件：

> 当前最高优先级的 pending + enabled external interrupt 是哪个？

例如：

```text
EIP:
IRQ 20 = pending
IRQ 40 = pending

EIE:
IRQ 20 = enable
IRQ 40 = enable
```

那么：

```text
stopei
```

返回 top interrupt。

IMSIC 中较小的 identity 数值具有更高 priority，因此可能返回：

```text
20
```

规范定义 `mtopei/stopei/vstopei` 用来报告当前最高优先级 pending-and-enabled external interrupt；写该 CSR 会 claim 当前报告的 interrupt，即清除它的 pending bit。:chatgpt-content-reference{index="11"}

---

# 21. claim interrupt 非常漂亮

传统 PLIC：

```text
irq = read(PLIC_CLAIM)
```

IMSIC：

```asm
csrrw a0, stopei, x0
```

一次 CSR 指令：

```text
read top interrupt
+
claim
```

然后：

```asm
srli a0, a0, 16
```

取 interrupt identity。

概念代码：

```c
irq = claim_stopei();

handler[irq]();
```

官方规范甚至给出的推荐 handler skeleton 就是：

```text
read/write stopei
→ 得到 IRQ ID
→ handler
```

并指出 `CSRRW` 可以同时 read + claim。:chatgpt-content-reference{index="12"}

这相较于：

```text
MMIO read PLIC
```

明显更贴近 hart。

---

# 22. eidelivery

interrupt file 还有：

```text
eidelivery
```

它控制：

```text
这个 interrupt file 是否允许向 hart 发 external interrupt
```

最重要的：

```text
eidelivery = 0

disable
```

```text
eidelivery = 1

enable
```

注意：

```text
eidelivery = 0
```

不代表：

```text
MSI 不会到达
```

MSI 仍然可能：

```text
set EIP
```

只是：

```text
IMSIC 不向 CPU assert external interrupt
```

这跟：

```text
pending state
```

和：

```text
delivery
```

是两层概念。:chatgpt-content-reference{index="13"}

---

# 23. eithreshold

还有：

```text
eithreshold
```

用于 priority filtering。

简单理解：

```text
only interrupts above threshold
can signal hart
```

例如：

```text
pending:
IRQ 10
IRQ 50
IRQ 100
```

通过 threshold 可以屏蔽一部分低优先级 interrupt。

不过 IMSIC 的 priority 模型有一个值得记住的特点：

> **interrupt identity 同时天然形成优先级关系。**

较小 ID：

```text
higher priority
```

所以：

```text
ID 20
```

优先于：

```text
ID 40
```

这一点跟 PLIC：

```text
IRQ ID
```

与：

```text
priority
```

完全独立的设计不同。

---

# 24. `siselect / sireg`：为什么 IMSIC CSR 看起来怪怪的？

IMSIC 有很多状态：

```text
EIP[]
EIE[]
threshold
delivery
```

如果全部分配独立 CSR：

```text
CSR address space
```

会非常浪费。

因此 AIA 使用：

```text
indirect CSR access
```

例如：

```text
siselect
sireg
```

思想类似：

```text
siselect = register index

sireg = register data
```

例如：

```text
siselect = 0x70
```

访问：

```text
eidelivery
```

```text
siselect = 0x72
```

访问：

```text
eithreshold
```

```text
0x80 ...
```

访问：

```text
EIP[]
```

```text
0xC0 ...
```

访问：

```text
EIE[]
```

这些 register number 由 AIA 明确定义。:chatgpt-content-reference{index="14"}

这个设计模式你以后在 RISC-V 会越来越常见：

```text
selector CSR
+
data CSR
```

---

# 25. 一次完整 wired interrupt

现在把 APLIC + IMSIC 串起来。

假设：

```text
UART IRQ = source 10
```

配置：

```text
APLIC:
sourcecfg[10] = Level1

target[10]:
hart = 2
EIID = 50

enable source 10
```

UART：

```text
UART
 |
 | IRQ wire high
 v
APLIC
```

APLIC：

```text
source 10 pending
```

查：

```text
target[10]
```

得到：

```text
Hart2
ID 50
```

于是生成：

```text
MSI write

address = Hart2 S-mode IMSIC
data    = 50
```

然后：

```text
IMSIC2:

EIP[50] = 1
```

如果：

```text
EIE[50] = 1
```

则：

```text
IMSIC
 |
 v
SEIP = 1
```

CPU：

```text
scause = interrupt | 9
```

进入：

```text
stvec
```

handler：

```text
stopei
↓
50
```

然后：

```text
uart_irq_handler()
```

完整路径就是：

```text
UART
  |
  | wire
  v
APLIC
  |
  | MSI {Hart2, ID=50}
  v
IMSIC2
  |
  | SEIP
  v
Hart2
  |
  | scause=9
  v
S-mode trap
  |
  | stopei
  v
IRQ 50
  |
  v
UART driver
```

这张图必须彻底掌握。

---

# 26. PCIe MSI 则更简单

PCIe device 本身就支持：

```text
MSI
```

于是根本不需要：

```text
APLIC
```

路径变成：

```text
PCIe NVMe
    |
    | MSI
    v
  IMSIC
    |
    v
  Hart
```

这才是 AIA 真正面向 server / high-performance system 的 architecture。

---

# 27. AIA + H extension：真正精彩的地方

你上一章已经学习：

```text
HS
VS
VU
```

现在 AIA 会让 H extension 的 virtual interrupt architecture 完整起来。

假设：

```text
Hart0
```

运行：

```text
VM1 vCPU
```

IMSIC：

```text
Hart0 IMSIC

+------------------+
| M interrupt file |
+------------------+
| S interrupt file |
+------------------+
| Guest File 1     |
+------------------+
| Guest File 2     |
+------------------+
| Guest File 3     |
+------------------+
```

Hypervisor 可以：

```text
Guest File 1
```

分配给：

```text
VM1 vCPU0
```

于是：

```text
device
 |
 | MSI
 v
Guest Interrupt File 1
 |
 v
VS interrupt
 |
 v
Guest OS
```

而不是：

```text
device
 ↓
hypervisor
 ↓
trap
 ↓
inject virtual irq
 ↓
guest
```

这就是：

> **direct interrupt injection**

---

# 28. Guest interrupt file 的意义

假设 VM 中：

```text
Linux guest
```

Guest Linux 看到的 IMSIC：

```text
就像真实 S-mode IMSIC
```

它访问：

```text
siselect
sireg
stopei
```

实际在 V=1 时对应：

```text
vsiselect
vsireg
vstopei
```

底层则对应：

```text
physical IMSIC guest interrupt file
```

因此：

```text
Guest OS
```

不需要知道：

```text
自己正在使用 guest interrupt file
```

从 guest 看：

```text
这是我的 IMSIC
```

这是 AIA virtualization 非常漂亮的地方。官方规范明确说明 guest interrupt file 对 VS software 看起来与真正 supervisor interrupt file 一致。:chatgpt-content-reference{index="15"}

---

# 29. 为什么 APLIC 本身不直接 virtualize？

这里有个非常重要的 architectural decision。

AIA：

```text
IMSIC
```

提供：

```text
guest interrupt file
```

但是：

```text
APLIC
```

没有为每个 VM 都提供一个 hardware virtual APLIC。

所以：

```text
Guest APLIC
```

通常需要：

```text
hypervisor emulate
```

官方规范明确指出 AIA 没有提供 APLIC hardware virtualization assistance；设计假设高性能设备会更多使用 MSI，并直接投递到 guest interrupt file。:chatgpt-content-reference{index="16"}

这其实非常合理：

```text
modern high-performance IO
             ↓
            MSI
             ↓
          IMSIC
```

APLIC 更多是为了：

```text
legacy/wired SoC interrupt
```

---

# 30. IOMMU 为什么会突然出现在 AIA 中？

这是理解：

```text
device passthrough
```

的关键。

假设 Guest：

```text
VM1
```

控制 PCIe NIC。

Guest 认为自己的 IMSIC 地址：

```text
Guest Physical Address

0x28000000
```

于是 guest driver 配置 NIC：

```text
MSI addr = 0x28000000
```

NIC 发 interrupt：

```text
write 0x28000000
```

问题来了。

这个地址是：

```text
Guest PA
```

而设备需要访问：

```text
Host PA
```

所以必须：

```text
Device
   |
   | MSI GPA
   v
IOMMU
   |
   | translate
   v
Host Physical IMSIC Guest File
```

路径：

```text
PCIe Device
      |
      | MSI
      v
    IOMMU
      |
      v
Guest Interrupt File
      |
      v
   Guest OS
```

这就是：

> **IOMMU interrupt remapping**

AIA 明确把 IOMMU 参与 MSI virtualization 纳入体系；guest 直接控制设备时，IOMMU 可以把 guest 配置的 MSI destination 转换到真正的 physical guest interrupt file。:chatgpt-content-reference{index="17"}

---

# 31. 这和 Arm GICv3/ITS 怎么对应？

你可以建立下面这个概念映射，但只能用于帮助理解：

| RISC-V | Arm GICv3/4 | 主要思想 |
|---|---|---|
| APLIC | Distributor 的部分职责 | wired interrupt routing |
| IMSIC | Redistributor/CPU interface 的部分思想 | per-hart interrupt state |
| MSI write | ITS/LPI 体系中的 MSI | message-based interrupt |
| interrupt file | per-PE interrupt state 的一种实现 | local interrupt state |
| guest interrupt file | GIC virtualization/direct injection 的相似目标 | VM interrupt acceleration |
| APLIC target | GIC routing | interrupt affinity |
| `stopei` | ICC_IARx_EL1 在“取得当前 IRQ”这个动作上的类比 | claim interrupt |

最值得比较的是：

```text
GICv2:
centralized

        ↓

GICv3:
redistributor
MSI/LPI
ITS
virtualization
```

与：

```text
PLIC:
centralized

        ↓

AIA:
APLIC
IMSIC
MSI
guest interrupt file
```

它们背后的 architectural trend 很相似：

> **减少 central controller 成为 scalability bottleneck，把更多 interrupt state 下沉到 CPU/hart 附近，并拥抱 MSI。**

---

# 32. 但 IMSIC 和 ITS 不是同一个东西

这是一个容易出现的错误。

Arm：

```text
PCIe MSI
   |
   v
ITS
   |
   | DeviceID/EventID translation
   v
Redistributor
   |
   v
PE
```

RISC-V AIA 更接近：

```text
PCIe MSI
   |
   v
IMSIC
   |
   v
Hart
```

IMSIC 自己就是：

```text
MSI receiving endpoint
```

地址：

```text
决定 target hart / interrupt file
```

数据：

```text
决定 interrupt ID
```

复杂的 DMA/MSI translation 需求则更多交给：

```text
IOMMU
```

因此两套 architecture 的职责边界并不相同。

---

# 33. AIA 没有替代 `mip/mie/sip/sie`

这一点也非常重要。

AIA 并没有把原来的 RISC-V interrupt architecture 推倒重来。

你之前学过：

```text
mie
mip

sie
sip
```

这些依然存在。

例如 IMSIC S-mode interrupt file 有可投递 interrupt：

```text
IMSIC
 |
 v
SEIP
```

也就是最终体现在：

```text
sip.SEIP
```

然后是否真正 trap：

还要满足：

```text
sip.SEIP = 1

AND

sie.SEIE = 1

AND

sstatus.SIE / privilege condition
```

所以：

```text
IMSIC
```

是 external interrupt controller。

而：

```text
sip/sie
```

仍然属于：

```text
hart architectural interrupt control
```

这两个层次不要混。

---

# 34. interrupt enable 实际存在好几层

这一点非常适合 Staff-level 面试。

以 S-mode external interrupt 为例：

```text
device IRQ
   |
   v
APLIC source enable
   |
   v
IMSIC EIP
   |
   AND
   |
IMSIC EIE
   |
   AND
   |
eidelivery
   |
   v
sip.SEIP
   |
   AND
   |
sie.SEIE
   |
   AND
   |
sstatus.SIE
   |
   v
trap
```

也就是说：

```text
APLIC enable
```

与：

```text
IMSIC EIE
```

与：

```text
sie.SEIE
```

完全不是一回事。

分别控制：

```text
source
↓
external interrupt identity
↓
major interrupt
↓
global S-mode interrupt
```

这是非常值得形成 mental model 的地方。

---

# 35. AIA 还扩展了 local interrupt

传统 RISC-V 常见 interrupt：

```text
software
timer
external
```

AIA 又为：

```text
local interrupt
```

预留了更多 major interrupt identities。

例如：

```text
RAS events
```

可以直接产生：

```text
hart-local interrupt
```

而不需要绕：

```text
APLIC
```

或：

```text
IMSIC
```

AIA 当前标准化了例如 low/high-priority RAS event interrupt，并保留了一批 standard local interrupt identities。:chatgpt-content-reference{index="18"}

路径可以是：

```text
Hart internal error
       |
       v
   local interrupt
       |
       v
      mip
```

而不是：

```text
Hart error
↓
external controller
↓
Hart
```

---

# 36. AIA 还改善了 interrupt priority

以前一个问题是：

```text
PLIC
```

可以给 external interrupt 排 priority。

但：

```text
timer interrupt
software interrupt
external interrupt
```

之间的 priority 比较能力非常有限。

AIA 引入：

```text
iprio[]
```

概念，使 major interrupt 也可以具有 configurable priority。

于是概念上可以：

```text
timer
software
external
RAS
local
```

放进更统一的 priority decision。

不过这是 AIA 中偏进阶的部分。

第一次学习只需要记住：

> AIA 不只是重新设计 external interrupt controller，也扩展了 hart 本地 interrupt priority architecture。

---

# 37. AIA 中 IPI 怎么办？

传统 RISC-V：

```text
IPI
```

常见：

```text
CLINT/ACLINT MSWI
```

或者：

```text
SBI send_ipi()
```

AIA 系统也允许利用：

```text
IMSIC
```

发送 IPI。

本质仍然很简单。

Hart0：

```text
store IPI_ID
```

到：

```text
Hart1 IMSIC MSI address
```

于是：

```text
Hart0
 |
 | MSI
 v
IMSIC1
 |
 v
Hart1
```

所以从 architecture 的角度：

```text
IPI
```

也可以变成：

```text
hart → hart MSI
```

这再次体现了 AIA 的核心设计：

> **把 interrupt communication 统一到 message delivery。**

---

# 38. 一个特别重要的区别：AIA 不提供标准 broadcast interrupt

AIA external interrupt routing：

```text
one interrupt
        ↓
one hart
```

没有标准 hardware：

```text
broadcast
```

也没有要求：

```text
1-of-N automatic load balancing
```

如果需要向多个 hart 发送 IPI：

```text
for each hart:
    send interrupt
```

官方规范明确选择了这种模型，并解释了不标准化 broadcast / hardware 1-of-N interrupt routing 的原因。:chatgpt-content-reference{index="19"}

这是和某些 GIC routing capability 不同的地方。

---

# 39. PLIC → AIA 的设计演化，可以这样理解

第一代：

```text
               PLIC
                 |
      +----------+----------+
      |          |          |
    Hart0      Hart1      Hart2
```

controller 中央化。

AIA：

```text
              APLIC
                |
             MSI fabric
          /      |      \
         v       v       v
      IMSIC0   IMSIC1   IMSIC2
         |       |       |
       Hart0   Hart1   Hart2
```

再加 PCIe：

```text
              APLIC
                |
                +-------+
                        |
PCIe device ------------+---- MSI fabric
                        |
          +-------------+-------------+
          v             v             v
       IMSIC0         IMSIC1         IMSIC2
```

整个系统开始从：

```text
interrupt controller network
```

变成：

```text
memory/message network
```

这是 AIA 架构思想的本质。

---

# 40. 再看 virtualization，就会更加漂亮

传统模型：

```text
Device
  |
  v
Host IRQ controller
  |
  v
Hypervisor
  |
  | virtual interrupt injection
  v
Guest
```

AIA：

```text
Device
  |
  | MSI
  v
IOMMU
  |
  v
IMSIC guest interrupt file
  |
  v
Guest
```

理想情况下：

```text
Hypervisor
```

从 interrupt fast path 中退出。

这对：

```text
VFIO
PCI passthrough
SR-IOV
networking
storage
```

意义非常大。

---

# 41. AIA 与 H Extension 的完整关系

你现在可以把上一章 H Extension 和这一章连起来：

```text
                      Physical Hart
                           |
                    +------+------+
                    |    IMSIC    |
                    +------+------+
                           |
            +--------------+--------------+
            |              |              |
          M file          S file       Guest file
            |              |              |
            v              v              v
            M             HS             VS
                                         |
                                         v
                                    Guest Kernel
```

Guest：

```text
VS
```

访问：

```text
siselect
sireg
stopei
```

实际上：

```text
V=1
```

后对应：

```text
vsiselect
vsireg
vstopei
```

然后 hardware 通过：

```text
hstatus.VGEIN
```

等状态选择当前 guest interrupt file。

所以：

```text
H extension
```

提供：

```text
virtual privilege architecture
```

而：

```text
AIA
```

补上：

```text
virtual interrupt architecture
```

两者是天然配套的。:chatgpt-content-reference{index="20"}

---

# 42. 你现在应该重新理解 `hgeip`

上一章学习 H extension 时：

```text
hgeip
```

可能比较抽象。

现在就非常清楚了。

IMSIC guest interrupt files：

```text
Guest File 1
Guest File 2
Guest File 3
...
```

如果：

```text
Guest File 2
```

存在 interrupt：

```text
pending + enabled
```

那么对应：

```text
hgeip
```

里的 guest external interrupt pending state 会体现出来。

所以：

```text
IMSIC guest interrupt file
       ↓
     hgeip
       ↓
hypervisor interrupt architecture
```

它们不是两个独立体系。

---

# 43. AIA register architecture 最值得掌握哪些？

不要试图背完整 specification。

第一轮只掌握：

| Component | Register | 含义 |
|---|---|---|
| APLIC | `domaincfg` | domain global config |
| APLIC | `sourcecfg[i]` | source type/delegation |
| APLIC | `target[i]` | target hart/IRQ |
| APLIC | `setip/clrip` | pending |
| APLIC | `setie/clrie` | enable |
| IMSIC | `eidelivery` | file delivery enable |
| IMSIC | `eithreshold` | threshold |
| IMSIC | `eip[]` | external pending |
| IMSIC | `eie[]` | external enable |
| Hart | `siselect/sireg` | indirect access |
| Hart | `stopei` | top external IRQ |
| VS | `vsiselect/vsireg` | guest access |
| VS | `vstopei` | guest top IRQ |

只要这些掌握了：

> 80% AIA interrupt datapath 已经能读懂。

---

# 44. 一个 AIA S-mode 初始化伪代码

例如：

```c
/* APLIC */

sourcecfg[UART_IRQ] = LEVEL_HIGH;

target[UART_IRQ] =
        TARGET_HART(2) |
        TARGET_EIID(50);

setienum = UART_IRQ;


/* IMSIC on hart2 */

siselect = EIDELIVERY;
sireg = 1;

siselect = EIE_REGISTER_FOR(50);
sireg |= BIT_FOR(50);


/* hart */

sie |= SIE_SEIE;
sstatus |= SSTATUS_SIE;
```

运行后：

```text
UART
↓
APLIC
↓
MSI ID 50
↓
Hart2 IMSIC
↓
SEIP
↓
S-mode
```

---

# 45. handler 伪代码

```c
void s_external_irq_handler(void)
{
    uint64_t x;

    x = csr_swap(stopei, 0);

    unsigned irq = x >> 16;

    irq_table[irq]();
}
```

核心动作就是：

```text
stopei
↓
claim highest external IRQ
↓
dispatch
```

这比传统 PLIC：

```text
MMIO claim
```

更加 hart-local。

---

# 46. 为什么 IMSIC 的性能通常更好？

并不是简单因为：

```text
CSR 比 MMIO 快
```

真正 architecture-level 优势是：

```text
per-hart interrupt state
```

降低了：

```text
shared central controller contention
```

再加上：

```text
MSI
```

本身天然适合：

```text
PCIe
multicore
virtualization
```

所以 scalability 更好。

可以理解为：

```text
PLIC:

many CPUs
   |
shared controller


AIA:

many CPUs
   |
local interrupt state
```

---

# 47. 不要产生一个误解：APLIC 不是必须经过 IMSIC

AIA 允许：

```text
APLIC direct mode
```

所以：

```text
wired IRQ
↓
APLIC
↓
hart
```

完全合法。

官方还明确指出，不带 IMSIC 的系统也可以使用 APLIC；这种情况下 APLIC 就承担传统 centralized interrupt controller 的角色。:chatgpt-content-reference{index="21"}

因此：

```text
AIA
```

不等于：

```text
一定存在 IMSIC
```

但是：

```text
high-performance / server AIA
```

通常重点就是：

```text
APLIC + IMSIC
```

---

# 48. 三种典型机器

你可以把实际 SoC 分成三类。

```text
A. legacy/small

Device
 ↓
PLIC
 ↓
Hart
```

```text
B. AIA but wired-oriented

Device
 ↓
APLIC
 ↓
Hart
```

```text
C. full AIA

wired device
 ↓
APLIC
 ↓
MSI
 ↓
IMSIC
 ↓
Hart

PCIe MSI device
 ──────────→ IMSIC
```

第三种是最值得你重点学习的。

---

# 49. QEMU 是学习 AIA 最好的环境之一

当前 QEMU `virt` machine 已直接支持：

```text
aia=aplic
```

以及：

```text
aia=aplic-imsic
```

例如：

```bash
qemu-system-riscv64 \
    -M virt,aia=aplic-imsic \
    ...
```

如果要实验 guest interrupt file：

```bash
qemu-system-riscv64 \
    -M virt,aia=aplic-imsic,aia-guests=5 \
    ...
```

QEMU 文档明确支持这两种 AIA 模式，并支持配置每 hart VS-level IMSIC guest pages。:chatgpt-content-reference{index="22"}

---

# 50. 我建议做三个实验

第一个实验只研究：

```text
APLIC direct mode
```

观察：

```text
UART
↓
APLIC
↓
SEIP
```

第二个：

```text
APLIC + IMSIC
```

观察：

```text
UART
↓
APLIC
↓
MSI
↓
IMSIC
↓
SEIP
↓
stopei
```

第三个：

```text
H extension
+
IMSIC guest interrupt file
```

理解：

```text
physical IMSIC guest file
↓
VS external interrupt
↓
Guest Linux
```

做到第三步时，你基本已经真正理解 AIA。

---

# 51. Linux 源码建议怎么读？

现在主线 Linux 中已经有独立的：

```text
drivers/irqchip/
```

下面的 RISC-V AIA driver。

比较值得看：

```text
irq-riscv-aplic-main.c

irq-riscv-imsic-platform.c
```

Linux 的 APLIC driver 会根据 firmware description 判断：

```text
direct mode
```

还是：

```text
MSI mode
```

例如 Device Tree 中存在 `msi-parent` 时会走 APLIC MSI mode；IMSIC driver 则会为 Linux IRQ 构造 MSI message，其中 MSI address 是目标 CPU 的 IMSIC page，MSI data 是 local interrupt identity。:chatgpt-content-reference{index="23"}

你看到 Linux 里面类似：

```c
msg->address = imsic_address;
msg->data = local_id;
```

时应该马上想到：

```text
MSI address
=
target hart / interrupt file

MSI data
=
interrupt identity
```

这就是整个 AIA 的核心。

---

# 52. AIA 与 CPU affinity

例如 Linux：

```text
irq_set_affinity(irq, CPU7)
```

在 MSI 世界中，本质上可以表现成：

```text
old MSI:

addr = CPU2 IMSIC


new MSI:

addr = CPU7 IMSIC
```

所以：

```text
IRQ affinity
```

从 hardware architecture 看越来越接近：

```text
修改 MSI destination
```

而不是：

```text
改变某个 interrupt controller output wire
```

这正是现代 interrupt architecture 的共同趋势。

---

# 53. 为什么每个 interrupt file 是一个 page？

这不仅是实现方便。

更重要的是：

```text
memory protection
```

因为：

```text
M interrupt file page

S interrupt file page

Guest interrupt file page
```

拥有不同 physical address。

于是系统可以通过：

```text
PMP
page table
IOMMU
```

控制：

```text
谁能向谁发送 interrupt
```

例如：

```text
device
```

只能写：

```text
Guest1 interrupt file
```

而不能写：

```text
M interrupt file
```

所以 MSI delivery 本身也可以纳入：

```text
memory access permission architecture
```

这是把 interrupt 转成 memory transaction 后一个非常漂亮的安全性质。

---

# 54. 从安全角度理解 IMSIC

过去：

```text
interrupt wire
```

属于：

```text
out-of-band signal
```

很难直接套用：

```text
MMU/IOMMU access control
```

MSI：

```text
interrupt = memory write
```

于是可以：

```text
IOMMU:
Device A
can write IMSIC page X

cannot write IMSIC page Y
```

因此：

```text
interrupt isolation
```

可以部分转化成：

```text
memory access isolation
```

这就是为什么：

```text
AIA
+
IOMMU
+
H Extension
```

结合起来非常重要。

---

# 55. AIA 和你的 TrustZone / TEE 思维怎么联系？

虽然 AIA 本身不是 TEE architecture，但你可以用熟悉的安全问题理解：

```text
Who is allowed to inject an interrupt
into whom?
```

如果：

```text
Device A
```

能够随便写：

```text
M-mode IMSIC page
```

那当然会产生严重问题。

所以真正的平台必须控制：

```text
MSI address reachability
```

通过：

```text
PMP
PMA
IOMMU
bus firewall
```

等 mechanism。

也就是说：

> **AIA 把一部分 interrupt security 问题转换成了 address-space isolation 问题。**

这一点值得你从 security architecture 角度重点理解。

---

# 56. 与之前 04 Interrupt 阶段如何衔接？

之前你学的：

```text
mip/mie
sip/sie

MEI
SEI

PLIC
```

属于：

```text
基础 interrupt architecture
```

这一章则是在此基础上增加：

```text
              base interrupt architecture

                 mip/mie
                 sip/sie
                    |
                    |
              +-----+------+
              |            |
          old PLIC       AIA
                           |
                    +------+------+
                    |             |
                  APLIC          IMSIC
```

所以 AIA：

> **不是替代 Privileged Architecture interrupt model，而是在外部 interrupt delivery 和 local priority/virtualization 上进行扩展。**

---

# 57. 与第 10 章 H extension 如何衔接？

第 10 章：

```text
HS
VS
VU

hvip
hideleg
hgeip
```

如果只学习 H extension，很容易感觉：

```text
virtual external interrupt 到底来自哪里？
```

这一章之后：

```text
                physical device
                      |
                     MSI
                      |
                      v
              IMSIC guest file
                      |
                      v
                   hgeip
                      |
                      v
                 VS interrupt
                      |
                      v
                  Guest OS
```

于是 H extension 中很多原本抽象的 CSR 就突然变成 concrete hardware datapath。

---

# 58. Staff Engineer 层面最值得真正理解的五个问题

第一个：

```text
为什么 PLIC 不适合现代 server？
```

不是回答：

```text
PLIC 太旧。
```

而是：

```text
centralized controller
+
wire-based routing
+
poor virtualization model
+
not naturally aligned with PCIe MSI
```

第二个：

```text
为什么拆成 APLIC + IMSIC？
```

答案：

```text
APLIC:
处理 legacy wired source

IMSIC:
处理 per-hart MSI delivery

把 source routing 和 CPU-local
interrupt state 解耦。
```

第三个：

```text
为什么 IMSIC 对 virtualization 很重要？
```

答案：

```text
guest interrupt file
```

允许：

```text
device interrupt
```

直接进入：

```text
guest virtual hart
```

减少：

```text
hypervisor trap/injection
```

第四个：

```text
为什么还需要 IOMMU？
```

因为 device 看到：

```text
guest configured MSI address
```

而真正 IMSIC 是：

```text
host physical address
```

需要：

```text
MSI address translation / isolation
```

第五个：

```text
AIA 的本质是什么？
```

我认为最好的答案是：

> **AIA 把 RISC-V 的 external interrupt architecture 从 centralized wire-routing 模型转向 distributed per-hart message-signaled architecture，并同时为 virtualization 提供硬件可扩展的 interrupt file。**

---

# 59. 一个最终必须记住的三层模型

以后看到 AIA，只画：

```text
       Layer 1
   Interrupt Source
         |
         v

       Layer 2
 Routing / Translation
         |
         v

       Layer 3
 Hart-local Delivery
```

映射：

```text
wired device
     |
     v
   APLIC
     |
     v
   IMSIC
     |
     v
    Hart
```

或者：

```text
PCIe device
     |
     +--------------+
                    |
                    v
                  IMSIC
                    |
                    v
                   Hart
```

virtualization：

```text
PCIe device
     |
     v
   IOMMU
     |
     v
IMSIC Guest File
     |
     v
Guest vCPU
```

这三幅图，就是整个 AIA architecture 的骨架。

---

# 60. 与 Arm GICv3 做最后一次统一

你已经熟悉 Arm，所以可以这样记忆设计哲学：

```text
              Arm                         RISC-V

legacy       GICv2                        PLIC

modern       GICv3/4                      AIA
                |                           |
         Distributor                    APLIC
         Redistributor                  IMSIC
         ITS                            IOMMU/MSI infra
         virtualization                guest interrupt file
```

不是组件一一对应，而是：

```text
architectural evolution
```

高度相似：

```text
centralized interrupt controller

            ↓

per-core interrupt state

            ↓

MSI

            ↓

hardware-assisted virtualization
```

掌握这一层之后，你就不需要靠死记：

```text
APLIC
IMSIC
```

它们存在的原因会非常自然。

---

# 61. 这一阶段真正的验收标准

不要用：

```text
我能不能背出 AIA register
```

判断自己学会没有。

应该问自己能否不查资料画出下面三条路径。

第一条：

```text
UART
↓
APLIC
↓
IMSIC
↓
SEIP
↓
scause=9
↓
stopei
↓
UART handler
```

第二条：

```text
NVMe
↓
MSI
↓
IMSIC
↓
SEIP
↓
stopei
↓
NVMe handler
```

第三条：

```text
Passthrough NIC
↓
MSI
↓
IOMMU
↓
IMSIC guest interrupt file
↓
VS external interrupt
↓
Guest Linux
```

如果这三条链路能解释清楚：

```text
谁产生 interrupt
谁决定 target
谁保存 pending
谁保存 enable
谁决定 privilege
谁决定 guest
CPU 从哪里获得 IRQ ID
```

那么你已经掌握了 AIA 的核心。

---

# 62. 最后压缩成一张图

```text
                    RISC-V AIA
                        |
        +---------------+---------------+
        |                               |
   Wired Interrupt                     MSI
        |                               |
        v                               |
      APLIC                             |
        |                               |
        | Routing                       |
        | Hart + EIID                   |
        |                               |
        +------------- MSI -------------+
                        |
                        v
                      IMSIC
                        |
          +-------------+-------------+
          |             |             |
        M-file        S-file       Guest-file
          |             |             |
          v             v             v
         MEIP          SEIP          hgeip
          |             |             |
          v             v             v
          M            HS/S           VS
                                      |
                                      v
                                  Guest OS
```

再给它配一句最值得记住的话：

> **APLIC 解决“wired interrupt 从哪里来、送到哪里”；IMSIC 解决“某个 hart / privilege / guest 收到了哪个 MSI interrupt”；AIA 的真正目标则是让 RISC-V 的 interrupt architecture 能够从小型 SoC 一直扩展到多核 server 和虚拟化环境。**

当前 RISC-V Server SoC 规范已经要求支持 AIA，并要求 server-class external interrupts 以 MSI 方式投递，这也说明 AIA 已经不是一个边缘扩展，而是现代 RISC-V application/server architecture 的核心组成部分。:chatgpt-content-reference{index="24"}

下一步如果继续按照我们这个系列深入，我建议对 **11 AIA** 再单独做一轮“**代码级学习**”：直接拿 **QEMU `virt,aia=aplic-imsic` + Linux `irq-riscv-aplic-*` / `irq-riscv-imsic-*`**，跟踪一次 UART wired IRQ 和一次 MSI 从产生到 `stopei` claim 的真实路径。这样会把这篇里的 architecture 模型彻底落到代码上。
