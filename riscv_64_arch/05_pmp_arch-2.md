你这个问题问到了 **PMP 真正的核心**。

前面那三个 section 本质上其实不是三个独立知识点，而是同一个问题的三个层次：

> **CPU 得到一个 Physical Address 之后，PMP 硬件到底如何判断：“这个地址属于哪个 protection region？”**

这就是 **Address Matching**。

所以可以把前面的：

> 7. `pmpaddr` 为什么不像真正地址  
> 8. TOR  
> 9. NAPOT

重新理解成：

```text
             PMP Address Matching
                     │
        ┌────────────┼────────────┐
        │            │            │
   地址怎么存？    怎么表示？    怎么表示？
        │            │            │
    pmpaddr        TOR          NAPOT
```

也就是说：

- **`pmpaddr`**：描述“地址边界/region”的编码载体
- **TOR**：一种 Address Matching 算法
- **NAPOT**：另一种 Address Matching 算法

而不是：

```text
pmpaddr → 一个地址
TOR     → 一个地址
NAPOT   → 一个地址
```

---

# 1. 先忘掉寄存器，我们先解决一个最简单的问题

假设我们有这样一个物理内存空间：

```text
Physical Address

0x0000_0000
      |
      | Boot ROM
      |
0x1000_0000
      |
      | UART
      |
0x2000_0000
      |
      | Secure Device
      |
0x8000_0000
      |
      | RAM
      |
0x9000_0000
```

现在 M-mode 想告诉硬件：

> S-mode 只能访问 `0x8000_0000 ~ 0x9000_0000` 这 256 MB RAM。

那么 CPU 执行：

```asm
ld a0, 0(t0)
```

假设最终得到：

```text
PA = 0x8000_1234
```

PMP 必须回答：

```text
0x8000_1234
     ↓
是不是在 [0x8000_0000, 0x9000_0000)？
```

如果是：

```text
match
  ↓
检查 R/W/X
```

如果不是：

```text
继续检查下一个 PMP entry
```

所以：

> **Address Matching 的本质就是一个“地址属于哪个 region？”的问题。**

这句话非常重要。

---

# 2. PMP 实际上需要完成什么计算？

可以把硬件抽象成：

```c
for (each PMP entry) {

    if (address_belongs_to_region(PA, entry)) {

        if (permission_allows(access))
            allow();

        else
            fault();

        break;
    }
}
```

所以 PMP 其实有两个完全不同的问题：

### 问题 A：Address Matching

```text
PA = 0x80001234

属于 PMP0 吗？
属于 PMP1 吗？
属于 PMP2 吗？
```

### 问题 B：Permission Checking

假设：

```text
PA 属于 PMP1
```

那么：

```text
PMP1.R = 1 ?
PMP1.W = 0 ?
PMP1.X = 1 ?
```

决定：

```text
load   → allow?
store  → fault?
fetch  → allow?
```

所以：

```text
              PMP
               |
       +-------+-------+
       |               |
 Address Matching   Permission
       |               |
 "属于哪一个 region?"  "R/W/X?"
       |               |
       +-------+-------+
               |
           Allow/Fault
```

**你前面没理解 TOR/NAPOT，很大程度就是因为没有先把这两个问题分开。**

---

# 3. 那么第一个问题：region 怎么表示？

例如我们想表示：

```text
[0x8000_0000, 0x8001_0000)
```

也就是：

```text
0x80000000
    |
    | 64 KiB
    |
0x80010000
```

PMP 必须把这个 region 的信息存起来。

最直观的方法当然是：

```text
base = 0x80000000
top  = 0x80010000
```

也就是：

```text
+----------------------+
| base                 |
+----------------------+
| top                  |
+----------------------+
```

但是 PMP 有很多 entry：

```text
PMP0
PMP1
PMP2
...
PMP63
```

如果每个 entry 都存：

```text
base
top
```

硬件需要很多寄存器。

RISC-V 因此设计了一套非常漂亮的编码方式：

> **不要求每个 PMP entry 自己同时保存 base 和 top。**

于是出现了：

```text
TOR
```

以及：

```text
NAPOT
```

---

# 4. 第一个问题：为什么 `pmpaddr` 不是完整 Physical Address？

这个其实非常简单。

PMP 标准最小 granularity 是：

```text
4 bytes
```

也就是说 PMP region boundary 至少是 4-byte aligned：

```text
0x....0000
0x....0004
0x....0008
0x....000C
...
```

所以地址最低两位：

```text
PA[1:0]
```

永远是：

```text
00
```

它们没有保存价值。

于是：

```text
Physical Address:

63                         2 1 0
+---------------------------+---+
|       useful address      |00 |
+---------------------------+---+
```

PMP 直接把：

```text
PA[63:2]
```

放到：

```text
pmpaddr
```

里。

所以：

```text
PA = 0x8000_0000
```

对应：

```text
pmpaddr = PA >> 2
        = 0x2000_0000
```

反过来：

```text
PA = pmpaddr << 2
```

这就是第 7 section 想表达的第一层意思。

官方规范明确规定，`pmpaddr` 编码 physical address 的高位部分，低两位因为 PMP 的 4-byte granularity 被省略。RV64 中对应的是 physical address 的 `[55:2]`。:chatgpt-content-reference{index="0"}

---

# 5. 但是这里出现了一个更有意思的问题

如果：

```text
pmpaddr
```

只是：

```text
PA >> 2
```

那么我们只能存一个地址。

可是一个 region 通常需要：

```text
base
+
top
```

例如：

```text
[0x80000000, 0x80010000)
```

需要两个东西：

```text
base = 0x80000000
top  = 0x80010000
```

RISC-V 怎么办？

答案就是：

# TOR

---

# 6. TOR：用两个相邻的 `pmpaddr` 表示一个 region

TOR：

> **Top Of Range**

核心思想非常简单：

```text
pmpaddr[i-1] = bottom
pmpaddr[i]   = top
```

于是：

```text
PMP entry i
=
[pmpaddr[i-1], pmpaddr[i])
```

注意：

> **真正执行 TOR 的是 entry i，但是它借用了前一个 entry 的 `pmpaddr`。**

这正是 TOR 最容易让人困惑的地方。

---

# 7. 用一个完整例子理解 TOR

我们还是要表示：

```text
[0x8000_0000, 0x8001_0000)
```

64 KiB。

首先：

```text
bottom = 0x80000000
top    = 0x80010000
```

由于 `pmpaddr = PA >> 2`：

```text
bottom >> 2
=
0x20000000
```

所以：

```text
pmpaddr0 = 0x20000000
```

top：

```text
0x80010000 >> 2
=
0x20004000
```

所以：

```text
pmpaddr1 = 0x20004000
```

然后：

```text
pmpcfg1.A = TOR
```

注意是：

```text
pmpcfg1
```

不是：

```text
pmpcfg0
```

因为：

```text
PMP1:
lower = pmpaddr0
upper = pmpaddr1
```

最终：

```text
pmpaddr0
    |
    | 0x80000000
    v
    +--------------------------------+
    |                                |
    |          PMP1 region           |
    |                                |
    +--------------------------------+
                                     |
                                     | 0x80010000
                                     v
                                pmpaddr1
```

也就是：

```text
PMP1 matches:

0x80000000 <= PA < 0x80010000
```

官方规范对 TOR 的定义就是：

```text
pmpaddr[i-1] <= address < pmpaddr[i]
```

而且前一个 entry 的 `pmpcfg` 是否 OFF 并不影响它作为 lower bound 的作用。

---

# 8. 所以 TOR 的设计理念是什么？

其实就是：

> **把“一个 region 的两个 boundary”分摊到两个相邻 PMP entries 的 address registers 中。**

可以画成：

```text
                 PMP Address Registers

pmpaddr0             pmpaddr1
   |                    |
   |                    |
   v                    v
0x80000000          0x80010000
   |                    |
   +--------------------+
            |
            v
     [bottom, top)
```

所以：

```text
TOR
```

特别适合：

> **任意 bottom/top 的连续地址范围。**

比如：

```text
[0x10000000, 0x10012000)
```

也可以。

因为：

```text
bottom
```

和：

```text
top
```

不要求 region size 是 power-of-two。

---

# 9. 这就是 TOR 和 NAPOT 的第一个本质区别

比如：

```text
Region A:

0x80000000
    |
    | 0x10000
    |
0x80010000
```

这是：

```text
64 KiB
```

可以 NAPOT。

但假设：

```text
Region B:

0x80003000
    |
    | 0x7000
    |
0x8000A000
```

那么：

```text
size = 0x7000
```

不是：

```text
2^N
```

也不是自然对齐的 power-of-two region。

所以：

```text
NAPOT ❌
```

但是：

```text
TOR ✅
```

因此：

```text
TOR
    ↓
灵活的 [bottom, top)

NAPOT
    ↓
高效的 aligned power-of-two region
```

这就是两个模式存在的根本原因。

---

# 10. 那为什么还要 NAPOT？

因为现实系统里有大量这种 region：

```text
4 KiB
8 KiB
16 KiB
32 KiB
64 KiB
1 MiB
2 MiB
16 MiB
...
```

这些全部都是：

```text
2^N
```

并且通常天然对齐：

```text
64 KiB region
64 KiB aligned
```

例如：

```text
0x80000000
    |
    | 64 KiB
    |
0x80010000
```

如果我们知道：

```text
base = 0x80000000
size = 64 KiB
```

其实：

```text
top = base + size
```

完全可以推导出来。

所以：

> **既然 size 是 power-of-two，而且 base 又自然对齐，就没必要同时存 bottom 和 top。**

只需要：

```text
base
+
size
```

甚至可以进一步：

> **把 size 编码进 base 的低位。**

这就是 NAPOT 的核心设计思想。

---

# 11. NAPOT 的关键：地址低位本来就是 0

还是：

```text
base = 0x80000000
size = 4 KiB
```

4 KiB：

```text
4096 = 2^12
```

因为 base 必须 4 KiB aligned：

```text
0x80000000
```

二进制低 12 位：

```text
0000 0000 0000
```

也就是说：

```text
base:

xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
                         ^^^^^^^^^^^^
                         12 bits = 0
```

这些低位原本没有什么信息。

RISC-V 就想：

> “既然这些低位本来就是 0，我能不能拿其中一部分来编码 region size？”

答案：

> **可以。**

于是就有了 NAPOT。

---

# 12. 用 4 KiB NAPOT 完整推导一次

现在我们要创建：

```text
Region:

0x80000000
    |
    | 4 KiB
    |
0x80001000
```

---

## Step 1：base

```text
base = 0x80000000
```

先：

```text
base >> 2
=
0x20000000
```

所以：

```text
pmpaddr
```

原本可以是：

```text
0x20000000
```

---

## Step 2：size

```text
size = 4096
     = 2^12
```

NAPOT 利用低位编码 size。

对于：

```text
2^N bytes
```

需要：

```text
N - 3
```

个 trailing `1`。

这里：

```text
N = 12
```

所以：

```text
12 - 3 = 9
```

个 `1`。

于是：

```text
pmpaddr
=
0x20000000
|
0x000001FF
```

得到：

```text
pmpaddr0 = 0x200001FF
```

然后：

```text
pmpcfg0.A = NAPOT
```

---

# 13. `0x200001FF` 到底是什么意思？

这个数最重要。

不要再把它理解成：

```text
PA = 0x800007FC
```

不是。

它是一个：

> **Address + Size Encoding**

我们把它拆开：

```text
0x200001FF

0x20000000     0x1FF
     |            |
     |            +---- size information
     |
     +----------------- base information
```

二进制低位：

```text
... 0 111111111
    ^ ^^^^^^^^^
    |     9 ones
    |
    bit 9
```

看到：

```text
9 个 trailing 1
```

硬件就知道：

```text
region size = 2^(9+3)
            = 2^12
            = 4096
            = 4 KiB
```

然后：

```text
base
=
clear 掉这 9 个 size bits
+
<< 2
```

得到：

```text
0x20000000 << 2
=
0x80000000
```

于是硬件知道：

```text
base = 0x80000000
size = 0x1000
```

最终：

```text
[0x80000000, 0x80001000)
```

这就是 NAPOT。

官方规范的 NAPOT 表正是利用 `pmpaddr` 低位连续的 `1` 来编码 region size：例如末尾 `0` 表示 8-byte NAPOT，末尾 `01` 表示 16-byte，末尾 `011` 表示 32-byte，依此类推。:chatgpt-content-reference{index="2"}

---

# 14. 你现在应该能看懂这张表了

这是官方规范中 NAPOT encoding 的核心思想：

| `pmpaddr` 低位 | Region size |
|---|---:|
| `...0` | 8 B |
| `...01` | 16 B |
| `...011` | 32 B |
| `...0111` | 64 B |
| `...01111` | 128 B |
| ... | ... |
| `...111111111` | 4 KiB |

注意这里最关键的规律：

```text
trailing ones 数量
        ↓
       size
```

公式：

```text
number_of_trailing_ones = k

region_size = 2^(k + 3)
```

例如：

```text
...0
```

有：

```text
k = 0
```

所以：

```text
2^(0+3)
= 8 B
```

而：

```text
...01
```

有：

```text
k = 1
```

所以：

```text
2^(1+3)
= 16 B
```

而：

```text
...011
```

有：

```text
k = 2
```

所以：

```text
2^(2+3)
= 32 B
```

非常漂亮。

---

# 15. 那么 NAPOT 为什么叫这个名字？

NAPOT：

> **Naturally Aligned Power-Of-Two**

三个关键词：

### Power-Of-Two

```text
size = 2^N
```

例如：

```text
4K
8K
16K
32K
64K
...
```

### Naturally Aligned

如果：

```text
size = 4K
```

那么：

```text
base % 4K == 0
```

例如：

```text
0x80000000  ✅

0x80001000  ✅

0x80002000  ✅

0x80000800  ❌
```

所以：

```text
base + size
```

可以通过非常简单的硬件逻辑推导。

---

# 16. 现在我们终于可以理解 `pmpaddr` 为什么“看起来不像地址”

实际上：

## TOR 模式

`pmpaddr`：

```text
基本就是：

Physical Address >> 2
```

例如：

```text
PA = 0x80010000

pmpaddr = 0x80010000 >> 2
        = 0x20004000
```

所以 TOR 下比较直观。

---

## NAPOT 模式

`pmpaddr`：

```text
不是简单的 PA >> 2
```

而是：

```text
base >> 2
+
size encoding
```

例如：

```text
base = 0x80000000
size = 4K

base >> 2 = 0x20000000

size encoding = 0x1ff

pmpaddr = 0x200001ff
```

所以：

> **`pmpaddr` 应该理解成“PMP address-matching encoding”，而不是普通意义上的 physical address register。**

这其实才是第 7 section 最想表达的东西。

---

# 17. 把 TOR 和 NAPOT 放在一起，你就非常容易理解了

假设我们需要三个 region：

```text
Physical Address

0x80000000
    |
    | 64K
    |
0x80010000

0x80010000
    |
    | 37K
    |
0x80019400

0x80020000
    |
    | 64K
    |
0x80030000
```

其中：

```text
Region A:
[0x80000000, 0x80010000)
size = 64K
```

是 power-of-two aligned：

```text
NAPOT ✅
```

Region B：

```text
[0x80010000, 0x80019400)
size = 37K
```

不是 power-of-two：

```text
NAPOT ❌
TOR ✅
```

Region C：

```text
[0x80020000, 0x80030000)
size = 64K
```

也是：

```text
NAPOT ✅
```

所以 PMP 给了你两种表达能力：

```text
                 PMP Region
                     |
             +-------+-------+
             |               |
            TOR            NAPOT
             |               |
       arbitrary range   aligned 2^N
             |               |
        bottom + top      base + size
```

这才是 **Address Matching architecture** 的设计核心。

---

# 18. 为什么 RISC-V 不直接设计一个 `base + size` 的 PMP？

这是一个非常好的架构设计问题。

当然可以设计：

```text
pmpbase0
pmpsize0

pmpbase1
pmpsize1
```

但是硬件需要：

```text
base comparator
size decoder
range calculator
```

而 RISC-V 选择利用：

```text
TOR:
两个 boundary

NAPOT:
地址低位编码 size
```

使得 PMP entry 的硬件状态非常紧凑。

尤其 NAPOT：

```text
一个 pmpaddr
+
一个 pmpcfg.A
```

就可以表达：

```text
base
+
size
```

硬件可以通过：

```text
trailing-one decode
```

确定 region size，再进行地址匹配。

对于一个需要支持很多 PMP entries 的 CPU，这种 encoding 非常有价值。

---

# 19. 从硬件设计角度再看一次 NAPOT

假设：

```text
pmpaddr = ...011111111
```

硬件可以做：

```text
                pmpaddr
                   |
                   v
        +----------------------+
        | trailing-one detect  |
        +----------------------+
                   |
                   v
             region size
                   |
                   v
        +----------------------+
PA ---> |   address comparator |
        +----------------------+
                   |
                 match
```

这比给每个 entry 存：

```text
base
limit
size
```

要紧凑得多。

所以 NAPOT 并不是一个“为了让软件难受而设计的奇怪公式”。

恰恰相反：

> **它是一个典型的硬件编码优化：利用 naturally-aligned power-of-two region 本来就存在的地址低位冗余，把 size 信息塞进去。**

这是理解 NAPOT 最重要的设计思想。

---

# 20. 再看 TOR，你也会发现它非常合理

TOR 的硬件就更简单：

```text
pmpaddr[i-1] = lower
pmpaddr[i]   = upper
```

然后：

```text
lower <= PA < upper
```

本质就是：

```text
               PA
                |
        +-------+-------+
        |               |
     >= lower        < upper
        |               |
        +-------+-------+
                |
              match
```

所以 TOR 的优势就是：

> **表达任意连续 range 非常简单。**

代价：

> **一个 region 通常需要两个相邻的 address registers。**

---

# 21. 为什么 TOR 要“借用前一个 entry”？

这个地方其实也有很强的设计意味。

假设每一个 entry 都需要：

```text
base
top
```

64 个 entry 就需要：

```text
128 个 address registers
```

而 RISC-V 让：

```text
PMP0 → pmpaddr0
PMP1 → pmpaddr1
PMP2 → pmpaddr2
...
```

相邻 entry 共享 boundary：

```text
PMP0:
[0, pmpaddr0)

PMP1:
[pmpaddr0, pmpaddr1)

PMP2:
[pmpaddr1, pmpaddr2)

PMP3:
[pmpaddr2, pmpaddr3)
```

看到了吗？

它实际上形成了一串 boundary：

```text
0
 |
pmpaddr0
 |
pmpaddr1
 |
pmpaddr2
 |
pmpaddr3
 |
...
```

然后 region 是：

```text
[boundary0, boundary1)
[boundary1, boundary2)
[boundary2, boundary3)
...
```

这就是 TOR 非常漂亮的设计。

---

# 22. 一个更真实的 PMP 配置

假设我们想建立：

```text
Physical Address

0x80000000
    |
    | Linux RAM
    | RWX
    |
0x88000000
    |
    | Secure firmware
    | 不允许 S-mode
    |
0x88100000
    |
    | MMIO
    | RW
    |
0x88200000
```

可以用 TOR：

```text
pmpaddr0 = 0x80000000 >> 2
pmpaddr1 = 0x88000000 >> 2
pmpaddr2 = 0x88100000 >> 2
pmpaddr3 = 0x88200000 >> 2
```

然后：

```text
PMP0 = TOR?
PMP1 = TOR
PMP2 = TOR
PMP3 = TOR
```

逻辑上形成：

```text
PMP1:
[0x80000000, 0x88000000)

PMP2:
[0x88000000, 0x88100000)

PMP3:
[0x88100000, 0x88200000)
```

分别配置：

```text
PMP1: RWX
PMP2: ---
PMP3: RW-
```

这时 Address Matching 首先解决：

```text
PA 0x80001234
    ↓
PMP1
```

然后 Permission：

```text
PMP1.RWX
    ↓
load/store/fetch allowed
```

而：

```text
PA 0x88000100
    ↓
PMP2
    ↓
R/W/X = 0
    ↓
Access Fault
```

---

# 23. 现在再加入 PMP priority，就完整了

例如：

```text
PMP0:
[0x80000000, 0x90000000)
RWX

PMP1:
[0x88000000, 0x88100000)
---
```

那么：

```text
0x80000000 ───────────────────────────── 0x90000000
                  RWX
             +----------------+
             |     DENY       |
             +----------------+
             0x88000000   0x88100000
```

访问：

```text
PA = 0x88001000
```

同时：

```text
PMP0 match
PMP1 match
```

但是：

```text
PMP0 priority > PMP1
```

所以不能这样理解成：

```text
PMP0 allow
PMP1 deny
=> deny
```

实际上是：

```text
找到最低编号的 matching entry
             ↓
           PMP0
             ↓
      PMP1 不再参与
```

所以这个例子实际上：

```text
allow
```

如果你想做：

```text
大范围 allow
+
小范围 deny
```

就必须非常认真地设计 entry priority。

这也是 Address Matching 不只是“地址比较”而是一个完整规则匹配机制的原因。

---

# 24. 这里有一个非常重要的认知升级

你可以把 PMP 看成一个非常小的：

> **Hardware firewall rule table**

例如：

```text
PMP0:
if PA in [0x80000000, 0x90000000)
    allow RWX

PMP1:
if PA in [0x88000000, 0x88100000)
    deny
```

然后：

```text
              CPU PA
                |
                v
        +----------------+
        | PMP0 match ?   |
        +----------------+
                |
               yes
                |
                v
          PMP0 permission
```

但是 RISC-V 规定：

> **lowest-numbered matching entry wins**

所以它更准确地像：

```text
rule 0
rule 1
rule 2
...
```

而不是：

```text
all rules OR/AND together
```

这对以后看 Linux / OpenSBI / Keystone / TEE 的 PMP 配置代码非常重要。

---

# 25. 你可以用一个公式把整个 Address Matching 统一起来

最终你只需要记：

## TOR

```text
Region(i):

[pmpaddr[i-1] << 2,
 pmpaddr[i]   << 2)
```

也就是：

```text
bottom = pmpaddr[i-1] << 2
top    = pmpaddr[i]   << 2
```

---

## NAPOT

如果：

```text
pmpaddr = X...X 0111...1111
                  ^
                  |
             k 个 1
```

那么：

```text
size = 2^(k+3)
```

而：

```text
base ≈ clear the k size-encoding bits
       then << 2
```

例如：

```text
pmpaddr = 0x200001FF

trailing ones = 9

size = 2^(9+3)
     = 4096
     = 4 KiB

base = 0x20000000 << 2
     = 0x80000000
```

最终：

```text
Region =
[0x80000000, 0x80001000)
```

---

# 26. 最后把三个 section 真正串起来

现在重新看你问的三个 section：

### Section 7：为什么 `pmpaddr` 看起来不像地址？

答案：

> 因为 `pmpaddr` 不是普通的 PA register，而是 **PMP address-matching encoding**。

TOR 下：

```text
pmpaddr ≈ boundary >> 2
```

NAPOT 下：

```text
pmpaddr = base >> 2 + size encoding
```

---

### Section 8：TOR 是干什么的？

答案：

> 用**两个相邻的 PMP address boundary**表示一个任意的 `[bottom, top)` region。

```text
pmpaddr[i-1]
      |
      v
      +---------------------+
      |      region         |
      +---------------------+
                            |
                            v
                        pmpaddr[i]
```

适合：

```text
arbitrary range
```

---

### Section 9：NAPOT 是干什么的？

答案：

> 如果 region 是 **naturally aligned + power-of-two size**，就利用地址低位原本的 0，把 region size 编码进去，从而用一个 `pmpaddr` 表达整个 region。

```text
base
 +
size
 ↓
pmpaddr encoding
```

适合：

```text
4K
8K
16K
32K
64K
...
```

---

# 27. 最终形成这张 PMP mental model

我建议你以后看到 PMP，脑子里直接出现下面这张图：

```text
                       CPU Memory Access
                              |
                              v
                         Physical Address
                              |
                              v
                 +-------------------------+
                 |    PMP Address Match    |
                 |                         |
                 |  PMP0  → match?         |
                 |  PMP1  → match?         |
                 |  PMP2  → match?         |
                 |  ...                    |
                 +-------------------------+
                              |
                    lowest-number match
                              |
                 +------------+-------------+
                 |                          |
               TOR                        NAPOT
                 |                          |
       [bottom, top)             [base, base + 2^N)
                 |                          |
                 +------------+-------------+
                              |
                              v
                         R / W / X
                              |
                    +---------+---------+
                    |                   |
                  Allow               Fault
```

其中：

```text
TOR
=
我明确告诉你 bottom 和 top

NAPOT
=
我告诉你一个 naturally-aligned base
+
利用低位编码 size
```

---

## 最后用一句 Staff-level 的话总结

**PMP Address Matching 的设计目标不是“存一个地址”，而是用极少的硬件状态高效描述 physical-address regions。**

因此 RISC-V 提供了两个互补的 encoding：

```text
TOR
→ 用相邻 PMP address registers 表示 arbitrary [base, top) range
→ 灵活

NAPOT
→ 利用 naturally-aligned power-of-two region 的地址低位冗余编码 size
→ 紧凑、高效
```

而 `pmpaddr` 本质上是：

> **供 PMP comparator 使用的 address/range encoding，而不是一个普通意义上的“物理地址寄存器”。**

这也是为什么你之前觉得 `pmpaddr0 = 0x200001ff` 很奇怪——**如果把它当“地址”当然奇怪；如果把它当“Address Matching encoding”，整个设计马上就合理了。**

官方当前 Machine-Level ISA 的 PMP 章节可以直接作为这一部分的规范依据：[RISC-V Privileged Architecture — Physical Memory Protection](https://docs.riscv.org/reference/isa/priv/machine.html?utm_source=chatgpt.com)。:chatgpt-content-reference{index="4"}

如果把这个理解透了，下一步最值得做的不是继续背 PMP 寄存器，而是**手工模拟一个 PMP comparator**：给定 4 个 PMP entries、一个 PA、一个 load/store/fetch，逐项算出“哪个 entry match、为什么、最终 allow 还是 fault”。这样 TOR、NAPOT、priority、R/W/X 会一次性全部串起来。