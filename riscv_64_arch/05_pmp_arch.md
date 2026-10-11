# 05 — RISC-V PMP：Physical Memory Protection

## 0. 先给出这一章最重要的结论

**PMP 的本质是：由 M-mode 配置的一组“物理地址访问控制规则”，决定某个 hart 以 S/U 等有效 privilege 访问某段 Physical Address 时，是否允许 Read / Write / Execute。**

先把它画成这样：

```text
                    CPU / Hart
                       |
              load/store/fetch
                       |
                 effective privilege
                       |
                       v
               +---------------+
               |      PMP      |
               | PA + R/W/X    |
               +---------------+
                       |
                allow / deny
                       |
                       v
             Physical Address Space
                 /            \
               RAM            MMIO
```

所以 PMP：

```text
不是 MMU
不是 page table
不负责 VA -> PA translation
不负责 per-process address space

而是：

Physical Address
      +
Privilege
      +
Access Type
      ↓
   Allow / Deny
```

官方当前 Privileged ISA 明确定义 PMP 为 **per-hart、M-mode 控制的 physical memory access protection**，PMP 与 PMA 检查并行；PMP CSR 只能由 M-mode 访问。:chatgpt-content-reference{index="0"}

如果你有 Armv8-M MPU 经验，可以先暂时记成：

```text
PMP ≈ RISC-V 世界里的 region-based physical memory firewall
```

它和 MPU 很像，但**不能直接认为 PMP == MPU**。

---

# 1. PMP 为什么存在？

我们先从最根本的问题开始。

假设一个简单 RISC-V SoC：

```text
Physical Memory

0x0000_0000   Boot ROM
0x1000_0000   UART
0x2000_0000   Security Engine
0x8000_0000   DRAM
0x8100_0000   M-mode firmware
0x8200_0000   S-mode Linux
```

系统有：

```text
M-mode firmware
        |
        v
S-mode Linux
        |
        v
U-mode process
```

如果没有 PMP，仅仅存在：

```text
M > S > U
```

这个 privilege hierarchy 本身并没有回答一个问题：

> S-mode 到底能访问哪些物理地址？

例如：

```text
S-mode Linux

load 0x81000000   // M-mode firmware
load 0x20000000   // security engine
```

我们希望：

```text
S-mode:
DRAM                yes
UART                yes
security engine     no
M-mode firmware     no
```

这就是 PMP 要解决的问题。

所以可以把：

```text
Privilege Mode
```

理解为：

> 你是谁？

而：

```text
PMP
```

回答：

> 以你的身份，你可以访问哪些 physical resources？

---

# 2. PMP 和 MMU 的职责完全不同

这是这一章最容易混淆的地方。

MMU 做的是：

```text
VA
 |
 | page table
 v
PA
```

PMP 做的是：

```text
PA
 |
 | PMP permission
 v
Allow / Deny
```

因此一个完整访问过程可能是：

```text
S-mode:

load VA 0xffff_ffc0_80001000
          |
          v
       Sv39
          |
          v
PA = 0x80001000
          |
          v
        PMP
          |
      R allowed?
       /      \
     yes       no
      |         |
     RAM     access fault
```

所以：

> **Page Table 决定“虚拟地址映射到哪里”，PMP 决定“你有没有资格访问那个物理地址”。**

两层保护是叠加关系。

---

# 3. PMP 在整个内存系统中的位置

你可以建立这样一个架构模型：

```text
               instruction
                   |
                   v
             Virtual Address
                   |
             +-----------+
             | MMU / MMU |
             | Sv39      |
             +-----------+
                   |
                   v
            Physical Address
                   |
          +--------+--------+
          |                 |
         PMP               PMA
          |                 |
    access policy      HW property
          |                 |
          +--------+--------+
                   |
                   v
             Memory System
```

这里顺带区分 PMP 和 PMA：

```text
PMA = Physical Memory Attributes
PMP = Physical Memory Protection
```

PMA 描述：

```text
这块硬件是什么性质？

RAM?
MMIO?
是否支持 atomic?
是否 cacheable?
是否 idempotent?
```

而 PMP 描述：

```text
你有没有 R/W/X 权限？
```

PMA 通常是硬件/platform 固有属性；PMP 则是软件可以由 M-mode 配置的访问策略。官方规范明确区分两者，并规定两类检查可以并行进行。:chatgpt-content-reference{index="1"}

---

# 4. PMP 的基本硬件模型

每一个 PMP rule 本质上有两个东西：

```text
pmpcfg[i]
+
pmpaddr[i]
```

可以理解：

```text
PMP entry i

+--------------------------+
| pmpaddr[i]               |
|                          |
| 哪段 Physical Address？  |
+--------------------------+

+--------------------------+
| pmpcfg[i]                |
|                          |
| 如何匹配地址？           |
| R/W/X？                  |
| 是否 Lock？              |
+--------------------------+
```

当前规范最多支持 64 个 PMP entry；标准规定实现可以提供 0、16 或 64 个 entry，并从最低编号开始实现。PMP CSR 只允许 M-mode 访问。:chatgpt-content-reference{index="2"}

因此：

```text
PMP entry 0
PMP entry 1
...
PMP entry 15
...
```

注意：

> **这是 per-hart 的。**

SMP 系统：

```text
Hart 0 PMP
Hart 1 PMP
Hart 2 PMP
Hart 3 PMP
```

不是一组全局 PMP。

因此 M-mode firmware 启动 secondary hart 时，也要正确建立各 hart 的 PMP 状态。

---

# 5. `pmpcfg`：权限控制核心

每个 PMP entry 有一个 8-bit config：

```text
  7      6 5      4 3       2   1   0
+---+-----------+-------+---+---+---+
| L | reserved  |   A   | X | W | R |
+---+-----------+-------+---+---+---+
```

最核心的字段：

```text
R = Read
W = Write
X = Execute

A = Address matching mode

L = Lock
```

比如：

```text
R=1
W=1
X=0
```

表示：

```text
read      allowed
write     allowed
execute   denied
```

通常：

```text
data RAM → RW-
code      → R-X
```

和 MPU/page-table permission 的思想一样。

正常 PMP 模式下：

```text
R=0 W=1
```

属于 reserved encoding；也就是说不要把 PMP 理解成支持普通的 write-only memory。Smepmp 的 MML 模式会重新利用部分原本保留的编码，这属于后面的高级内容。:chatgpt-content-reference{index="3"}

---

# 6. `A`：PMP 最需要认真理解的字段

`A` 是：

```text
Address Matching
```

两位：

| A | Mode | 含义 |
|---|---|---|
| 00 | OFF | entry disabled |
| 01 | TOR | Top Of Range |
| 10 | NA4 | Naturally Aligned 4 bytes |
| 11 | NAPOT | Naturally Aligned Power Of Two |

官方标准就是这四种。:chatgpt-content-reference{index="4"}

这里真正需要掌握的是：

```text
TOR
NAPOT
```

NA4 很简单，实际大型 region 更常见的是 TOR/NAPOT。

---

# 7. 为什么 `pmpaddr` 看起来不像真正的地址？

例如你可能看到：

```asm
li   t0, 0x20000000
csrw pmpaddr0, t0
```

但实际物理地址可能是：

```text
0x80000000
```

因为 PMP address 基本以：

```text
4-byte unit
```

编码。

直观理解：

```text
Physical Address

63                     2 1 0
+-----------------------+---+
|      address          |00 |
+-----------------------+---+

                         ↓

pmpaddr

+-----------------------+
|      address >> 2     |
+-----------------------+
```

所以：

```text
PA = 0x80000000

pmpaddr =
0x80000000 >> 2
=
0x20000000
```

不过 **NAPOT 会进一步借用 `pmpaddr` 的低位编码 region size**，所以不能永远简单理解为 `PA >> 2`。

---

# 8. TOR：Top Of Range

TOR 是最容易理解的一种。

假设我们要定义：

```text
0x80000000
    ~
0x80010000
```

也就是：

```text
[0x80000000, 0x80010000)
```

TOR 使用：

```text
pmpaddr[i-1] = bottom
pmpaddr[i]   = top
```

因此：

```text
pmpaddr0 = 0x80000000 >> 2
         = 0x20000000

pmpaddr1 = 0x80010000 >> 2
         = 0x20004000
```

然后：

```text
pmpcfg1.A = TOR
```

于是 entry 1：

```text
pmpaddr0 << 2
        |
        v
0x80000000
        |
        | PMP1
        |
        v
0x80010000
        ^
        |
pmpaddr1 << 2
```

即：

```text
PMP1:

[0x80000000, 0x80010000)
```

一个很重要的细节：

> `pmpaddr0` 即使对应的 `pmp0cfg.A = OFF`，仍然可以作为 PMP1 TOR 的 lower bound。

官方定义就是：

```text
pmpaddr[i-1] <= address < pmpaddr[i]
```

对于 entry 0 使用 TOR：

```text
lower = 0
```

所以：

```text
PMP0 TOR
```

可以表达：

```text
[0, pmpaddr0 << 2)
```


---

# 9. NAPOT：最值得理解的 PMP 地址编码

NAPOT 全称：

> **Naturally Aligned Power-Of-Two**

也就是：

```text
region size = 2^N
并且
base 按 size 对齐
```

例如：

```text
Base = 0x80000000
Size = 64 KiB
```

合法，因为：

```text
0x80000000 % 0x10000 == 0
```

NAPOT 的编码公式可以记：

```text
pmpaddr =
    (base >> 2)
    |
    ((size >> 3) - 1)
```

以：

```text
base = 0x80000000
size = 0x10000
```

为例：

```text
base >> 2
=
0x20000000

size >> 3
=
0x2000

(size >> 3) - 1
=
0x1fff
```

所以：

```text
pmpaddr =
0x20001fff
```

为什么要这么奇怪？

因为：

```text
pmpaddr 的连续低位 1
```

被用于编码 region size。

例如：

```text
xxxxxxx0      → 8 bytes
xxxxxx01      → 16 bytes
xxxxx011      → 32 bytes
xxxx0111      → 64 bytes
...
```

可以看到：

```text
trailing ones 越多
       ↓
region 越大
```

这是一种非常典型的硬件编码技巧：

> 利用 naturally-aligned power-of-two region 本来就必然为 0 的地址低位来编码 size。

---

# 10. TOR 和 NAPOT 应该怎么理解？

可以简单建立下面这个直觉：

```text
TOR

lower ────────────── upper
        arbitrary-ish range

需要两个 address boundary
```

而：

```text
NAPOT

          power-of-two
      <---------------->
      +----------------+
base  |                |
      +----------------+

base 必须自然对齐
一个 pmpaddr 即可
```

所以：

```text
TOR
优点：range 比较灵活
缺点：通常需要相邻 address register 表示上下界

NAPOT
优点：一个 entry 自包含一个 region
缺点：size 必须 power-of-two，base 必须 aligned
```

这和 Arm MPU region 的一些 naturally-aligned region 思想，你应该会感觉很熟悉。

---

# 11. PMP 最反直觉的规则：默认权限

这是整个 PMP 最重要的规则之一。

假设 PMP 硬件存在。

对于：

```text
S-mode / U-mode
```

如果一个 memory access：

```text
没有任何 PMP entry match
```

结果是：

```text
DENY
```

而对于普通 baseline PMP 下的：

```text
M-mode
```

如果没有 entry match：

```text
ALLOW
```

也就是：

```text
             PMP no-match

         S/U             M
          |              |
          v              v
        DENY           ALLOW
```

官方规范明确规定：只要实现了 PMP，S/U 访问没有命中任何 entry 时失败；M-mode 未命中则成功。:chatgpt-content-reference{index="6"}

这个设计非常关键。

因为 PMP 从 S/U 的角度实际上非常像：

> **Allowlist / whitelist**

例如：

```text
PMP0:
DRAM
RWX

PMP1:
UART
RW
```

那么 S-mode：

```text
DRAM       allow
UART       allow

security engine
boot ROM
private SRAM
other MMIO
       ↓
no match
       ↓
deny
```

所以不是：

```text
默认所有地址可访问
然后用 PMP blacklist
```

而更接近：

```text
默认不能访问
PMP 给你开放 region
```

这是理解 PMP 的核心。

---

# 12. 但为什么 M-mode 默认能绕过 PMP？

因为 PMP 是：

```text
Machine mode
    |
    | configure
    v
PMP
    |
    v
S / U
```

也就是说基础设计里的 M-mode 是：

```text
root of control
```

所以普通：

```text
L=0
```

PMP entry：

```text
S/U → obey R/W/X
M   → ignore R/W/X
```

例如：

```text
PMP0:

0x80000000 - 0x81000000

R=0
W=0
X=0
L=0
```

那么：

```text
U-mode read → fault
S-mode read → fault

M-mode read → allowed
```

注意：

> M-mode 并不是“不进行地址 match”。

而是：

```text
entry match
    ↓
发现 effective privilege = M
    +
L = 0
    ↓
R/W/X 不约束 M
```

官方规范就是这种语义。:chatgpt-content-reference{index="7"}

---

# 13. `L` bit：PMP 的第二个关键设计

`L`：

```text
Lock
```

普通 PMP 模式下有两个作用。

第一：

```text
L = 1
    ↓
锁定 PMP entry
```

以后：

```text
pmpcfg[i]
pmpaddr[i]
```

写入都被忽略。

直到：

```text
hart reset
```

第二个作用更加重要：

```text
L = 1
    ↓
PMP permissions
也作用于 M-mode
```

于是：

```text
             L=0          L=1

S/U          enforce      enforce

M            bypass       enforce
```

例如：

```text
PMP0

region = ROM
R-X
L=1
```

那么：

```text
U     R yes  W no  X yes
S     R yes  W no  X yes
M     R yes  W no  X yes
```

连 M-mode 都不能写。

而且：

```text
M-mode
```

也不能简单重新修改 PMP0，因为：

```text
L = 1
```

已经锁住。

只有 reset 可以恢复普通 baseline PMP 的 locked entry。:chatgpt-content-reference{index="8"}

---

# 14. 一个非常容易踩坑的 TOR + Lock 细节

假设：

```text
PMP1 = TOR
```

它的 region：

```text
pmpaddr0
    ↓
[--------- PMP1 ---------)
                         ↑
                     pmpaddr1
```

如果：

```text
PMP1.L = 1
```

那么不能只锁：

```text
pmpaddr1
```

因为 lower bound：

```text
pmpaddr0
```

如果还能修改，就等于可以修改 PMP1 region。

因此规范规定：

```text
PMP1 = TOR
PMP1.L = 1
```

之后：

```text
pmpaddr1 locked
pmpaddr0 also cannot be written
```

这是非常合理的硬件设计。

---

# 15. PMP entry 是有优先级的

PMP 不只是：

```text
找到任何一个 match
```

而是：

> **最低编号的 matching entry 优先级最高。**

也就是：

```text
PMP0 > PMP1 > PMP2 > PMP3 ...
```

官方称为 static priority。:chatgpt-content-reference{index="9"}

例如：

```text
PMP0
0x80001000 - 0x80002000
---
DENY

PMP1
0x80000000 - 0x81000000
---
RWX
```

虽然 PMP1：

```text
allow 整块 16 MiB
```

但是：

```text
0x80001000 - 0x80002000
```

同时 match：

```text
PMP0
PMP1
```

结果：

```text
PMP0 wins
```

于是可以实现：

```text
+--------------------------------+
|            PMP1 RWX            |
|                                |
|        +---------------+       |
|        | PMP0 DENY     |       |
|        +---------------+       |
|                                |
+--------------------------------+
```

这种：

> **大 region allow + 小 region carve-out deny**

是 PMP priority 一个非常重要的使用方法。

---

# 16. 一个 access 必须“完整落在”同一个 winning PMP region 中

这是另外一个容易忽略的 corner case。

假设：

```text
PMP0:

0x100C - 0x100F
```

CPU 执行：

```text
8-byte load
address = 0x1008
```

访问范围：

```text
0x1008 ... 0x100F
```

其中：

```text
0x100C ... 0x100F
```

碰到了 PMP0。

于是 PMP0 成为：

```text
first matching entry
```

但是 PMP0 又没有覆盖：

```text
整个 8-byte access
```

结果：

```text
FAULT
```

不是说：

```text
前 4 byte 用默认规则
后 4 byte 用 PMP0
```

而是 winning PMP entry 必须 cover 整个 memory operation。官方规范特别强调了这一点。:chatgpt-content-reference{index="10"}

这对于：

```text
unaligned access
wide memory operation
```

尤其重要。

---

# 17. PMP violation 产生什么异常？

这是你后面调试时非常关键的知识点。

如果是：

```text
instruction fetch
```

违反 PMP：

```text
Instruction Access Fault
```

如果：

```text
load
```

违反 PMP：

```text
Load Access Fault
```

如果：

```text
store / AMO
```

违反 PMP：

```text
Store/AMO Access Fault
```

不是：

```text
Page Fault
```

这是一个重要区别。

例如：

```text
VA
 |
 | Sv39 translation successful
 v
PA
 |
 | PMP denied
 v
Load Access Fault
```

而：

```text
VA
 |
 | invalid PTE
 v
Load Page Fault
```

所以：

```text
Page Fault
    → address translation / page permission

Access Fault
    → PMP / PMA / physical access failure 等
```

PMP 权限失败属于 access-fault。:chatgpt-content-reference{index="11"}

---

# 18. PMP + Sv39：真正重要的组合关系

假设 Linux 在 S-mode：

```text
Linux VA
   |
   v
Sv39
   |
   v
Page Table
   |
   v
PA
   |
   v
PMP
```

你可能认为：

```text
只需要最终 PA 做 PMP check
```

其实不止如此。

Page Table Walker 自己也需要访问物理内存：

```text
satp
 |
 v
L2 PTE
 |
 v
L1 PTE
 |
 v
L0 PTE
```

这些：

```text
PTE memory reads
```

也要经过 PMP。

而且 page-table walk 的 effective privilege：

```text
S-mode
```

因此：

```text
S-mode
   |
   | VA
   v
MMU
   |
   +-- read page table PA
   |         |
   |        PMP check
   |
   v
Final PA
   |
  PMP check
   |
 memory
```

官方规范明确规定 PMP 应用于这些隐式 page-table accesses，并把其有效 privilege 视为 S。:chatgpt-content-reference{index="12"}

这会产生一个很重要的效果：

即使 Linux：

```text
PTE mapping 看起来正确
```

但如果 page table 本身所在的物理 RAM 没有 PMP permission：

```text
Page Walker
   |
   v
PMP DENY
   |
   v
translation 无法完成
```

所以：

> **PMP 是比 S-mode page table 更底层的一层 physical authority。**

这一点非常重要。

---

# 19. 修改 PMP 后为什么还可能需要 `SFENCE.VMA`？

你之前学 MMU 时知道：

```text
修改 page table
    ↓
SFENCE.VMA
```

但当前规范还有一个容易忽略的要求：

如果 hart 支持 page-based virtual memory，在修改 PMP CSR 后，M-mode 软件也需要通过：

```asm
sfence.vma x0, x0
```

把 PMP 改动与地址翻译相关缓存状态同步。

原因是实现可能缓存：

```text
translation
+
相关 PMP information
```

所以：

```text
write PMP CSR
     |
     v
sfence.vma
```

对于带分页虚拟内存的实现是重要的软件步骤，即使当前 VM 没有启用也要遵循规范要求。:chatgpt-content-reference{index="13"}

---

# 20. `MPRV` 与 PMP：一个很漂亮的组合

你前面学过：

```text
mstatus.MPRV
mstatus.MPP
```

例如：

```text
current mode = M
MPRV = 1
MPP  = S
```

那么 M-mode 执行普通 data access 时，可以让访问：

```text
effective privilege = S
```

于是：

```text
M-mode firmware

load user/supervisor buffer
        |
      MPRV=1
      MPP=S
        |
        v
effective privilege S
        |
        v
      PMP check
```

而不是：

```text
M-mode unrestricted access
```

这允许 M-mode firmware 用较低 privilege 的 memory permission 去访问某些缓冲区。

规范明确规定，当 M-mode 设置 `MPRV=1` 且 `MPP=S/U` 时，这些 data accesses 也接受 PMP 检查。:chatgpt-content-reference{index="14"}

注意这里主要是：

```text
load/store data access
```

MPRV 不会把 instruction fetch 变成 S/U fetch。

---

# 21. 一个完整的 PMP 配置例子

现在假设：

```text
0x80000000 - 0x80010000
```

是一段 64 KiB RAM。

我们想允许：

```text
S/U:
Read
Write
Execute
```

使用 NAPOT。

前面算过：

```text
base = 0x80000000
size = 0x10000

pmpaddr =
(base >> 2)
|
((size >> 3) - 1)

=
0x20000000
|
0x1fff

=
0x20001fff
```

配置：

```text
A = NAPOT = 11b

X=1
W=1
R=1
L=0
```

config byte：

```text
L   A   XWR
0   11  111
```

数值：

```text
A = 0x18
RWX = 0x07

cfg = 0x1f
```

于是简单的 RISC-V 汇编可以写成：

```asm
li      t0, 0x20001fff
csrw    pmpaddr0, t0

li      t0, 0x1f
csrw    pmpcfg0, t0

sfence.vma
```

逻辑效果：

```text
PMP0

0x80000000
       |
       | 64 KiB
       | RWX for S/U
       v
0x80010000
```

由于：

```text
L = 0
```

M-mode 自己仍然不会被这一规则限制。

---

# 22. PMP 与 Armv8-M MPU 的关系

现在可以做你最熟悉的映射。

```text
Armv8-M MPU                    RISC-V PMP

region                         PMP entry

RBAR/RLAR                      pmpaddr

AP permission                  R/W

XN                             X

region priority                PMP entry priority

privileged/unprivileged        M/S/U effective privilege

MPU fault                      access fault

region-based                   region-based
```

所以从设计思想上：

```text
            MPU / PMP
                |
       +--------+--------+
       |                 |
   region match       permission
       |                 |
     address           R/W/X
```

非常类似。

但是有几个非常关键的区别。

### 区别一：PMP 是 physical address protection

PMP：

```text
Physical Memory Protection
```

它的语义非常明确地围绕 PA。

MPU 虽然同样没有 page translation，本质也属于 region-based protection，但在 Cortex-M 编程模型里你通常直接面对处理器 memory map；RISC-V PMP 更明确地作为 **M-mode 对更低 privilege 的 physical memory authority** 出现。

### 区别二：PMP 的控制权天然属于 M-mode

```text
M-mode
   |
 configure
   v
PMP
   |
 protect
   v
S/U
```

这个 hierarchy 是 PMP 架构设计的重要部分。

### 区别三：默认 deny 行为很重要

当 PMP 存在时：

```text
S/U unmatched
    ↓
DENY
```

这让 PMP 很适合做：

```text
allowlist
```

### 区别四：M-mode 本身通常 bypass

普通 PMP：

```text
L=0
```

情况下：

```text
M-mode ignores R/W/X
```

这在 MPU 思维里容易被忽略。

---

# 23. PMP 和 TrustZone 完全不是一回事

这个必须单独强调。

不能：

```text
PMP = TrustZone
```

TrustZone：

```text
Secure State
Non-secure State
```

属于：

```text
system-wide security state separation
```

例如 Arm：

```text
CPU state
bus transaction security attribute
memory controller
peripherals
GIC
TZASC/TZPC
```

整个 SoC 都可以参与 Secure/Non-secure partition。

而基本 PMP：

```text
hart
 |
 physical access
 |
 PMP
```

只是 CPU/hart physical-memory access control。

所以：

```text
PMP
```

更接近：

```text
CPU-side memory firewall
```

而不是：

```text
完整 TEE architecture
```

这也是为什么 RISC-V TEE 往往会：

```text
M-mode monitor
+
PMP / ePMP(Smepmp)
+
其他 SoC isolation
```

组合起来实现，而不是简单说：

```text
有 PMP = 有 TrustZone
```

---

# 24. PMP 能不能挡 DMA？

这里也非常重要。

假设：

```text
CPU
 |
 PMP
 |
 DRAM
```

同时：

```text
DMA engine
    |
    +----------> DRAM
```

普通 hart PMP 保护的是：

> **hart 发起的访问。**

DMA master 不会因为 CPU 的 PMP 设置，就天然获得同样的 per-hart PMP policy。

所以安全 SoC 还需要：

```text
IOMMU
IOPMP
SoC firewall
```

之类机制控制设备 DMA。

RISC-V 的 IOMMU 规范本身也把 hart-side PMP 与 I/O-side protection 分开建模；服务器平台甚至要求 DMA-capable peripheral 受 IOMMU 管控。:chatgpt-content-reference{index="15"}

这一点和 Arm 世界：

```text
CPU MMU
+
SMMU
```

的关系非常类似。

---

# 25. PMP 在实际系统中最典型的三个用途

## 场景一：M-mode firmware 隔离 S-mode OS

例如：

```text
Physical Memory

+----------------------+ 0x90000000
| Linux RAM            |  ← S RWX
+----------------------+ 0x82000000
| OpenSBI / M firmware |  ← S DENY
+----------------------+ 0x81000000
| Secure data          |  ← S DENY
+----------------------+ 0x80000000
```

M-mode：

```text
configure PMP
       ↓
只给 S-mode 开放 Linux RAM + 必要 MMIO
```

于是即使 S-mode kernel 被攻破：

```text
attacker has S-mode control
```

也不能因为自己是 kernel 就天然读取：

```text
M-mode firmware memory
```

---

## 场景二：只有 M/U 的 RTOS/embedded system

RISC-V 并不强制实现 S-mode。

可以存在：

```text
M-mode kernel
   |
   +---- U-mode Task A
   |
   +---- U-mode Task B
```

这时候可以在 context switch：

```text
Task A -> Task B
```

时更新 PMP：

```text
Task A:

PMP0 → code A
PMP1 → data A
PMP2 → stack A
```

切换为：

```text
Task B:

PMP0 → code B
PMP1 → data B
PMP2 → stack B
```

于是形成：

```text
Task A cannot access Task B
```

这和你熟悉的：

```text
Armv8-M MPU protected RTOS
```

非常接近。

---

## 场景三：TEE / Security Monitor

例如：

```text
M-mode Security Monitor
        |
        +------ Secure region
        |
        +------ Normal-world S-mode
```

PMP 可以让 S-mode：

```text
Normal world
```

访问：

```text
normal RAM
```

但不能访问：

```text
monitor RAM
secure payload RAM
secure device MMIO
```

这就是很多 RISC-V security architecture 采用 PMP 的原因。

但要记住：

> **只有 CPU PMP 并不足以构成完整 TEE。**

DMA、debug、其他 hart、SoC bus master、peripheral access 等都需要一起考虑。

---

# 26. Baseline PMP 的一个安全缺陷：M-mode 太强

我们现在终于可以理解为什么后来会有：

```text
Smepmp
```

也就是：

> PMP Enhancements for memory access and execution prevention in Machine mode

普通 PMP：

```text
L=0:
M-mode unrestricted

L=1:
M-mode subject to rule
但 rule 被永久 lock 到 reset
```

这带来一个问题。

例如：

```text
M-mode OS / monitor
```

本身如果被攻破：

```text
attacker controls M-mode
```

普通未锁 PMP 基本挡不住它：

```text
modify PMP
或者直接 bypass PMP
```

因此 Smepmp 引入：

```text
mseccfg
```

中的：

```text
RLB
MMWP
MML
```

官方 Smepmp 1.0 已 ratified。:chatgpt-content-reference{index="16"}

---

# 27. Smepmp 先只需要掌握三个概念

这一章不用把 Smepmp truth table 全背下来。

先记住三个字段。

### `MMWP`

Machine-Mode Allowlist Policy。

普通 PMP：

```text
M-mode unmatched
      ↓
    allow
```

`MMWP=1` 后：

```text
M-mode unmatched
      ↓
     deny
```

于是 M-mode 也转向：

```text
allowlist policy
```

---

### `MML`

Machine Mode Lockdown。

它会改变：

```text
L bit
```

以及一些 R/W/X encoding 的语义，让 PMP 能表达：

```text
M-mode-only region
S/U-only region
Shared region
```

因此：

```text
M-mode
```

不再天然拥有：

```text
访问所有 S/U memory
```

的绝对能力。

这是一个非常重要的 security hardening。

---

### `RLB`

Rule Locking Bypass。

主要用来在初始化/debug 期间暂时允许修改 locked rule。

但规范明确强调：

```text
RLB 长期保持开启会削弱安全性
```

所以一般应该：

```text
boot
 |
 configure rules
 |
 disable RLB
 |
 lockdown
``` :chatgpt-content-reference{index="17"}


---

# 28. PMP 和 MPU 最大的架构思想差异之一

到了这里，你可以形成一个更准确的理解。

Armv8-M MPU 常见模型：

```text
RTOS
 |
 context switch
 |
 reconfigure MPU
 |
 unprivileged task
```

强调：

```text
task isolation
```

而 RISC-V PMP 的初始设计视角更偏向：

```text
M-mode
 |
 establish physical authority boundary
 |
 +--------------+
 |              |
 S-mode        U-mode
```

因此它天然非常适合：

```text
M firmware
vs
OS
```

或者：

```text
M kernel
vs
U tasks
```

这种层级隔离。

换句话说：

> **MPU 经常让你想到“Task protection”；PMP 更应该首先让你想到“Machine-mode-controlled physical authority boundary”。**

这是理解其设计哲学比较关键的一步。

---

# 29. 一次访问到底怎么经过 PMP？

现在把整个流程完整走一次。

假设：

```text
current = U-mode

load x5, 0(a0)
```

其中：

```text
a0 = VA 0x40001000
```

流程：

```text
U-mode load
      |
      v
effective privilege = U
      |
      v
Sv39 translation
      |
      +--> Page-table memory access
      |          |
      |         PMP(S)
      |
      v
PA = 0x80201000
      |
      v
PMP matching
      |
      +--> PMP0?
      |
      +--> PMP1?
      |
      +--> ...
      |
      v
lowest-number matching entry
      |
      +--> does it cover whole access?
      |
      +--> R permission?
      |
      v
    Allow
      |
      v
     PMA
      |
      v
   memory
```

如果失败：

```text
Load Access Fault
```

这个流程建议真正记住。

---

# 30. PMP 硬件可以抽象成这样一段伪代码

为了理解本质，可以暂时把 PMP 想象成：

```c
bool pmp_check(addr, size, access_type, privilege)
{
    for (i = 0; i < PMP_ENTRIES; i++)
    {
        if (entry[i].matches_any_byte(addr, size))
        {
            if (!entry[i].covers_all_bytes(addr, size))
                return false;

            if (privilege == M && entry[i].L == 0)
                return true;

            return permission_allowed(entry[i],
                                      access_type);
        }
    }

    if (privilege == M)
        return true;

    return false;
}
```

忽略 Smepmp 等扩展以后，**这个伪代码已经抓住 baseline PMP 80%~90% 的设计本质**。

尤其是三条：

```text
lowest entry wins

matched entry 必须 cover 整个 access

no match:
    M   → allow
    S/U → deny
```

---

# 31. 调试 PMP 时，你应该按照什么顺序排查？

以后真正遇到：

```text
Load Access Fault
Store Access Fault
Instruction Access Fault
```

我建议脑中按这个顺序：

```text
1. 当前 effective privilege 是什么？

2. fault PA 是多少？
   VA -> PA translation 是否成功？

3. 哪一个 PMP entry 第一个 match？

4. entry region 是否覆盖整个 access？

5. R/W/X 是否正确？

6. L 是否设置？

7. 是否是 TOR？
   lower bound 是否来自 pmpaddr[i-1]？

8. 是否有更低编号 PMP 抢先 match？

9. 是否刚更新 PMP 而没有做必要的 SFENCE.VMA？

10. 是不是其实 PMA / bus/device fault，而不是 PMP？
```

这比盯着：

```text
pmpcfg0 = 0x......
```

猜测问题有效得多。

---

# 32. PMP 最值得记住的几个“不直觉点”

到 Staff engineer 层面，我认为下面这些比“会算 NAPOT”更重要：

```text
① PMP 是 physical address protection，不做 translation。

② PMP CSR 由 M-mode 控制，而且是 per-hart。

③ S/U no-match 默认 DENY；
   M no-match 默认 ALLOW（baseline PMP）。

④ unlocked PMP 的 R/W/X 默认不限制 M-mode。

⑤ L=1 不仅 lock entry，
   还让 baseline PMP permissions 作用于 M-mode。

⑥ entry 编号越小，priority 越高。

⑦ first matching entry 必须覆盖整个 memory operation。

⑧ page-table walker 的 physical accesses 也受 PMP 约束。

⑨ PMP fault 是 Access Fault，而不是 Page Fault。

⑩ CPU PMP 并不能自动保护 DMA。
```

如果这十条真正理解了，PMP 的架构就基本掌握了。

---

# 33. 把它和你已经掌握的 Arm 知识连起来

最后建议你在脑中保留这张图：

```text
                     RISC-V

                   M-mode
                      |
              configure PMP
                      |
                      v
        +-----------------------------+
        |     Physical Authority      |
        +-----------------------------+
            |                   |
            v                   v
          S-mode              U-mode
            |
           Sv39
            |
            v
           PA
            |
           PMP
            |
            v
          Memory
```

对应你熟悉的知识：

```text
Armv8-M MPU
        ↓
理解 PMP 的 region + permission

Armv8-A EL3/EL1
        ↓
理解 M-mode 对 S-mode 的控制层级

Arm MMU
        ↓
理解 Sv39 + PMP 是两层不同 protection

Arm SMMU
        ↓
理解为什么 CPU PMP 不等于 DMA protection

TrustZone
        ↓
帮助理解 PMP 可以作为 TEE building block
但 PMP 本身绝不是 TrustZone
```

## 本章最终应该形成的一句话

如果面试让我用很短的话解释 PMP，我会这样回答：

> **RISC-V PMP 是由 M-mode 配置的 per-hart physical-memory access-control mechanism。它通过一组按优先级匹配的 physical-address regions，对 S/U 等有效 privilege 的 instruction fetch、load、store实施 R/W/X 权限控制；PMP 不负责地址翻译，而是与 MMU 组合，在 VA→PA 后对物理访问形成更底层的 authority boundary。Baseline PMP 中 S/U 未匹配默认拒绝，而 M-mode 默认可以绕过未锁定规则；通过 L bit 和 Smepmp 又可以进一步约束 M-mode。**

如果后面继续按我们的课程顺序学习，我建议 **05 PMP 下一步不要马上进入 06 Sv39**；先单独把一个很关键、也最容易真正“卡住”的细节讲透：

```text
PMP address matching
    ↓
TOR / NA4 / NAPOT
    ↓
为什么 pmpaddr 要 >> 2
    ↓
NAPOT 的 bit encoding 到底是怎么推导出来的
```

尤其是 **NAPOT encoding**，如果只是背公式，很容易过几天又忘；从硬件为什么选择这种编码方式推导一遍之后，基本就不会再忘了。