# Chapter 05 — PMP 与系统级 Hardware Isolation

RISC-V OP-TEE Architecture & Porting · 深度课程 · Staff Engineer / Upstream Maintainer

## 本章的核心目标

上一章 Chapter 04 已经建立了 OpenSBI Domain 的逻辑模型：

- 一个 Domain 描述一组 Hart、Memory Region 和访问权限。
- OpenSBI 管理 Domain 的创建、运行及切换。
- Linux Domain 和 OP-TEE Domain 可以运行在相同的 RISC-V S-mode。

但这里留下了一个非常关键的问题：

OpenSBI Domain 只是一个软件抽象。即使 OpenSBI 声明 Linux 不允许访问 OP-TEE 内存，究竟是什么硬件机制阻止 Linux 绕过这项限制？

本章的任务，就是把这条从软件安全策略到硬件访问控制的链条彻底打通。

从 Domain Policy 到 Hardware Enforcement

OpenSBI Domain Policy

谁可以访问什么资源？具备什么权限？

OpenSBI Protection Configuration

将逻辑权限映射为 PMP / 系统隔离配置

Hart PMP

CPU 物理地址访问控制

System Isolation

DMA / Device / Bus Master 访问控制

Enforced TEE Security Boundary

Linux 即使控制自身页表，也无法突破已正确配置的硬件隔离

本章将完整覆盖原学习大纲的九个部分：

| Section | 内容                                       | 核心问题                         |
| ------- | ---------------------------------------- | ---------------------------- |
| 5.1     | PMP Address Matching、Priority、Permission | 硬件究竟怎样判断一次访问是否合法？            |
| 5.2     | PMP 如何保护 OP-TEE                          | 为什么 Linux root 不能访问 TEE RAM？ |
| 5.3     | PMP 与 S-mode MMU                         | 页表权限和物理权限如何组合？               |
| 5.4     | PMP Entry 数量与布局                          | 有限的硬件资源怎样表达复杂权限？             |
| 5.5     | PMP 的保护边界                                | 为什么 DMA 可以绕过 CPU PMP？        |
| 5.6     | IOPMP、WorldGuard、SoC Firewall            | 如何构建系统级隔离？                   |
| 5.7     | OpenSBI Hardware Isolation Framework     | 固件如何管理多种硬件隔离机制？              |
| 5.8     | Domain Switch 的安全顺序                      | 怎样避免权限切换时出现安全窗口？             |
| 5.9     | 安全漏洞分析与验证                                | 怎样证明隔离真的可靠？                  |

本章将使用一个贯穿始终的双 Domain SoC 作为案例，并明确区分：

- RISC-V ISA 保证的行为：例如标准 PMP Address Matching。
- OpenSBI 已有实现：例如 Domain Memory Policy 和 Hart Protection。
- 特定平台的实现：例如 WorldGuard 和 DMA Firewall。
- 课程中的简化代码：用于讲解原理，不应误认为可以直接用于生产环境。

# 5.1 PMP：从 Address Matching 到访问权限判定

## 5.1.1 Why：为什么需要 PMP？

我们先构造一个问题。

假设一个 RISC-V SoC 具有两块物理内存：

```
Physical Memory
──────────────────────────────────────────

0x80000000  ┌───────────────────────────┐
            │ Linux Memory              │
            │                           │
0x88000000  ├───────────────────────────┤
            │ OP-TEE Secure Memory      │
            │ Private Keys / TA / Core  │
0x89000000  └───────────────────────────┘
```

Linux 和 OP-TEE 都运行在 S-mode。

那么 Linux 能否执行：

```
volatile uint64_t *p = (void *)0x88001000;
uint64_t secret = *p;
```

如果系统没有使用 MMU，或者 Linux 已经为目标物理内存建立了有效的虚拟映射，并且没有其他硬件访问限制，那么 Linux 的确可能读取到 OP-TEE 的数据。

我们不能依赖 Linux 页表来防止这种攻击。

原因很简单：

Linux Kernel 本身控制 Linux 的页表。

如果 Linux Kernel 被攻破，攻击者可能修改页表，将 OP-TEE 所在的物理地址映射到 Linux 虚拟地址空间。

因此，需要一个 Linux 无法修改、并且不依赖 Linux 页表的保护机制。

这就是 PMP 的一个核心用途。

PMP（Physical Memory Protection）的设计思想是：

> 由更高特权级的 M-mode 配置物理地址访问规则，硬件在 S-mode/U-mode 发起访问时强制检查这些规则。

PMP 不只是一个软件 permission check，而是 CPU 硬件内的访问控制机制。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



## 5.1.2 What：PMP 的基本结构

一个 PMP Entry 由两类 CSR 描述：

- `pmpaddrN`：存储用于描述地址区域的编码。
- `pmpcfgN` 的对应配置字段：指定 Address Matching 模式、R/W/X 权限和 Lock 属性。

单个 PMP 配置字节

7

L

6

0

5

0

4

A1

3

A0

2

X

1

W

0

R

A[1:0] 选择 OFF / TOR / NA4 / NAPOT；R、W、X 表示访问权限；L 是锁定位。Bit 6、5 保留为零。

Address Matching 有四种模式：

| A[1:0] | 模式    | 用途                                    |
| ------ | ----- | ------------------------------------- |
| `00`   | OFF   | 此 Entry 不进行地址匹配                       |
| `01`   | TOR   | Top Of Range，用上下边界表示范围                |
| `10`   | NA4   | Naturally Aligned 4-byte Region       |
| `11`   | NAPOT | Naturally Aligned Power-of-Two Region |

注意：`pmpaddrN` 并不是直接存储完整物理地址，其地址编码采用右移两位的形式；NAPOT 还会利用低位连续的 1 表示区域大小。

### 一个重要的概念：PMP 不直接认识 Domain

PMP 只判断：

```
Access = {
    physical_address,
    access_size,
    effective_privilege,
    access_type
}
```

例如：

```
Address = 0x88001000
Size    = 8 bytes
Mode    = S-mode
Type    = Load
```

PMP 本身不知道当前 CPU 运行的是 Linux 还是 OP-TEE。

OpenSBI 必须为不同 Domain 配置不同的 PMP 规则，才能让同样运行在 S-mode 的 Linux 和 OP-TEE 得到不同访问权限。

这一点是后面理解 Domain Switch 的基础。

## 5.1.3 PMP Address Matching 的三个设计机制

### A. TOR：指定区域上下边界

假设我们希望允许：

```
0x80000000 <= PA < 0x88000000
```

可以设置：

```
pmpaddr0 = 0x80000000 >> 2
pmpaddr1 = 0x88000000 >> 2

pmp1cfg.A = TOR
pmp1cfg.R = 1
pmp1cfg.W = 1
pmp1cfg.X = 1
```

注意：

`pmpaddr0` 即使对应 Entry 0 的 A 字段为 OFF，也可以作为 Entry 1 TOR 的下边界。

TOR 的匹配规则是：

\\[ 4 \times pmpaddr[i-1] \le PA < 4 \times pmpaddr[i] \\]

对于 Entry 0，默认下边界为零。

TOR 的优点是范围直观，缺点是配置多个独立区域可能占用较多 Entry。

### B. NA4：四字节范围

若目标平台的 PMP Grain 支持，可以保护一个自然对齐的四字节区域。

例如：

```
Physical Address = 0x80001000
pmpaddr0         = 0x20000400
pmp0cfg.A        = NA4
```

其用途相对有限，实际 SoC 可能不支持这么细的粒度。

### C. NAPOT：编码 2 的幂大小的区域

NAPOT 非常适合描述自然对齐的连续内存。

假设要保护：

```
0x88000000 ~ 0x88001FFF
Size = 8 KiB
```

NAPOT 的编码规律为：

\\[ pmpaddr = (base \gg 2)\\;|\\;((size \gg 3)-1) \\]

这里假设 `size >= 8`，且满足自然对齐及硬件粒度要求。

因此：

```
base >> 2      = 0x22000000
(size >> 3)-1  = 0x000003FF

pmpaddr        = 0x220003FF
```

NAPOT 利用 `pmpaddr` 低位连续 1 的个数描述 region size。

如果有 \\(N\\) 个连续最低位为 1，那么：

\\[ size=2^{N+3} \\]

例如：

| 低位模式      | 连续 1 的数量 | Region Size |
| --------- | -------- | ----------- |
| `...0000` | 0        | 8 B         |
| `...0001` | 1        | 16 B        |
| `...0011` | 2        | 32 B        |
| `...0111` | 3        | 64 B        |
| `...1111` | 4        | 128 B       |

对于 8 KiB，低位需要有 10 个连续的 1。

这种编码的好处在于一个 PMP Entry 就能描述较大的连续区域。

但需要认识到：

PMP Grain 是平台属性，并不是所有硬件都支持最小 4B 或 8B 的粒度。

若实际最小 Grain 为 4 KiB，那么某些非常小的区域虽然符合标准 PMP 编码，也不能作为有效的独立保护区域。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



## 5.1.4 PMP Entry Priority：为什么低编号优先？

PMP 并不是简单地对所有匹配的 Entry 进行权限 OR。

它采用静态优先级：

编号越低，优先级越高。

例如：

| Entry | Region                  | Permission |
| ----- | ----------------------- | ---------- |
| PMP0  | `0x88000000–0x880FFFFF` | NONE       |
| PMP1  | `0x80000000–0x8FFFFFFF` | RWX        |

假设 CPU 访问：

```
PA = 0x88001000
S-mode Read
```

PMP0 和 PMP1 都覆盖该地址，但 PMP0 的优先级更高，因此访问被拒绝。

交叠区域的优先级示例

图中的地址和权限为教学示例，并非某款 SoC 的真实配置。

而且还有一个容易被忽视的规则：

如果最高优先级的匹配 Entry 只覆盖此次内存操作的部分字节，访问仍然失败，不会自动交给优先级较低的 Entry。

例如某次 8 字节 Load 横跨 PMP0 边界，即使 PMP1 允许整个范围，也不能通过 PMP1 绕过该限制。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



这一规则对于检查跨越安全内存边界的访问非常重要。

## 5.1.5 L Bit：Lock 和 M-mode 权限

在标准 PMP 语义下：

| 条件    | 对 M-mode 的影响           | 对 S/U-mode 的影响 |
| ----- | ---------------------- | -------------- |
| `L=0` | 通常不受该 Entry 的 R/W/X 限制 | 按 R/W/X 检查     |
| `L=1` | 也按该 Entry 的 R/W/X 检查   | 按 R/W/X 检查     |

`L=1` 还会锁定该 Entry 的配置和相关地址寄存器，通常直到 Hart Reset。

但有一个扩展需要特别说明：

Smepmp（PMP Enhancements）会改变部分传统 PMP 权限编码与 L Bit 的语义。

例如 `mseccfg.MML` 启用后，可以区分 M-mode-only、S/U-mode-only 和 Shared Region 等规则；`MMWP` 可以让 M-mode 采用更严格的默认访问策略。

因此，不能把传统 PMP 的全部规则原封不动地应用到开启了 Smepmp 的平台。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



## 5.1.6 PMP 的默认权限策略

在传统 PMP 语义下：

- M-mode：没有匹配 Entry 时，通常允许访问。
- S/U-mode：只要至少实现了一个 PMP Entry，没有匹配 Entry 时访问就会被拒绝。
- 访问被拒绝后，硬件产生相应的 Instruction/Load/Store Access Fault。

这意味着有一个非常重要的 Bring-up 陷阱：

如果 SoC 实现了 PMP，但 OpenSBI 没有为 S-mode 创建必要的物理内存许可，那么 Linux 即使有有效页表，也可能完全无法访问 DRAM。

小结：PMP 是一个按物理地址匹配、按有效特权级检查、按静态优先级选择规则的 CPU 访问控制机制，而不是简单的“保护某段内存”开关。

# 5.2 为什么 PMP 能保护 OP-TEE 的物理内存？

## 5.2.1 建立一个贯穿本章的 SoC 实例

假设 SoC 的物理布局如下：

| Physical Address | Size    | 用途                |
| ---------------- | ------- | ----------------- |
| `0x80000000`     | 1 MiB   | OpenSBI Firmware  |
| `0x80100000`     | 127 MiB | Linux Low Memory  |
| `0x88000000`     | 16 MiB  | OP-TEE Secure RAM |
| `0x89000000`     | 112 MiB | Linux High Memory |

这里假设 Linux 允许使用的是两块不连续的内存，而 OP-TEE 拥有独立的安全内存。

OpenSBI 需要为 Linux Domain 和 OP-TEE Domain 定义不同权限：

| Memory Region    | Linux Domain | OP-TEE Domain |
| ---------------- | ------------ | ------------- |
| OpenSBI Firmware | NONE         | NONE          |
| Linux RAM        | RWX          | 受控共享或拒绝       |
| OP-TEE RAM       | NONE         | RWX           |
| Shared Memory    | RW           | RW            |

这里的表格描述的是目标 Domain Policy，并不直接等于最终的 PMP CSR 值。

## 5.2.2 Linux Root 为什么不能绕过 PMP？

假设 Linux Kernel 已被攻击者控制。

攻击者尝试修改页表：

```
// 示意代码：假设攻击者能控制 Linux 内核页表
map_page(
    virtual_address  = 0xffffffc040000000,
    physical_address = 0x88001000,
    permission       = READ | WRITE
);
```

此时 CPU 会进行两种不同的检查：

```
Linux Virtual Address
0xffffffc040000000
          |
          v
   Sv39 Page Table
          |
          | PTE: R=1, W=1
          v
Physical Address
0x88001000
          |
          v
   PMP Permission Check
          |
          | Current Domain: Linux
          | S-mode: No Access
          v
      Access Fault
```

Linux 可以改变自己的 Page Table，但不能直接修改 M-mode 才能访问的 PMP CSR。

因此，在 OpenSBI 和硬件配置正确的前提下，Linux 修改页表也无法获得 TEE RAM 的访问权。

这里需要注意一个更深层次的问题：

PMP 本身不区分 Linux S-mode 与 OP-TEE S-mode。

假设一个 hart 在 Linux Domain 下运行时，PMP 允许 Linux RAM，禁止 OP-TEE RAM；当这个 hart 切换到 OP-TEE Domain 时，OpenSBI 必须更新该 hart 的隔离配置，使 OP-TEE 能够访问自己的 RAM。

例如：

```
Before Domain Switch:

Hart 0: Linux S-mode
PMP: Linux RAM = RWX
     TEE RAM   = NONE

After Domain Switch:

Hart 0: OP-TEE S-mode
PMP: Linux RAM = restricted
     TEE RAM   = RWX
```

如果 OpenSBI 不重新配置 PMP，OP-TEE 自己也不能访问 Secure RAM。

因此：

> PMP 提供硬件能力，Domain 定义安全策略，而 OpenSBI 负责将两者连接起来。

这个职责分离，就是 RISC-V Domain-based TEE Architecture 的核心。

## 5.2.3 Hart-local PMP 与跨 Hart 安全性

再看一个更复杂的问题：

```
Hart 0                    Hart 1
────────────────          ────────────────
Linux Domain              OP-TEE Domain

PMP:
TEE RAM = NONE            PMP:
                          TEE RAM = RWX
```

这里是否存在安全问题？

不一定。

因为标准 PMP 配置是 Hart-local 的，每个 hart 使用自己的 PMP CSR 进行访问控制。

但前提是：

1. Hart 0 在 Linux Domain 期间始终无法访问 TEE RAM。
2. Hart 1 在 OP-TEE Domain 期间获得所需的内存权限。
3. Hart 0 无法通过 DMA 或其他 Bus Master 间接访问 TEE RAM。
4. OpenSBI 不允许恶意 S-mode 软件自行更改 Domain 归属或隔离配置。

这同时说明：

SMP 下的 Domain Isolation 不能仅靠一个全局软件变量判断当前 Domain，而必须保证每个 Hart 的实际硬件权限正确。

# 5.3 PMP 与 S-mode MMU：两级权限检查

这是本章必须真正理解的另一个核心问题。

很多工程师把 PMP 理解成「没有 MMU 时才需要的 MPU」，这个理解是不正确的。

PMP 和 MMU 并不是互相替代的关系。它们可以共同参与同一次内存访问，而且检查的是不同层面的权限。

## 5.3.1 Why：为什么有了 MMU 还需要 PMP？

MMU 主要解决两个问题：

1. Virtual Address → Physical Address 的转换。
2. 通过 PTE 对当前地址空间的访问进行限制。

PMP 则提供一个独立于 S-mode 页表的物理访问控制机制。

二者最根本的区别是：

| 比较维度               | Sv39/Sv48 MMU       | PMP               |
| ------------------ | ------------------- | ----------------- |
| 管理实体               | S-mode OS 通常管理自己的页表 | M-mode Firmware   |
| 作用地址               | 虚拟地址经翻译得到物理地址       | 物理地址              |
| 主要权限               | R/W/X/U 等 PTE 属性    | R/W/X、区域匹配及相关扩展属性 |
| 地址空间粒度             | Page / Superpage    | PMP Region        |
| Linux Kernel 能否修改？ | Linux 管理的页表可以       | 标准 PMP CSR 不可以    |
| 主要职责               | 地址空间隔离与转换           | 建立更高特权级控制的物理访问边界  |

对于 RISC-V OP-TEE：

- PMP / 系统级隔离负责 Linux Domain 和 OP-TEE Domain 之间的物理资源隔离。
- OP-TEE MMU进一步保护 OP-TEE Core、User TA 以及不同 TA 之间的地址空间。

这两个层次缺一不可。

## 5.3.2 CPU 具体如何完成权限检查？

假设 Linux 执行：

```
ld a0, 0(a1)
```

其中：

```
a1 = 0xffffffc040000000
```

页表将该 VA 转换为：

```
PA = 0x88001000
```

可以把整个检查过程理解为：

```
S-mode Load
      |
      v
Virtual Address
      |
      v
Sv39 Page Table Walk
      |
      +-- PTE not valid / permission denied
      |          |
      |          v
      |      Page Fault
      |
      v
Physical Address
      |
      v
PMP Permission Check
      |
      +-- Not allowed
      |       |
      |       v
      |   Access Fault
      |
      v
PMA / Other HW Constraints
      |
      v
Memory Access
```

这是为了理解而简化的逻辑图，不表示硬件严格串行执行这些步骤。实际 CPU 可能并行执行、缓存部分检查结果，或者进行投机地址翻译。

对成功访问来说，可以把有效权限理解成：

\\[ Permission\_{effective} = Permission\_{PTE} \cap Permission\_{PMP} \\]

例如：

| PTE Permission | PMP Permission | 最终结果   |
| -------------- | -------------- | ------ |
| RWX            | RWX            | 对应访问允许 |
| RWX            | R              | 只读     |
| R              | RWX            | 只读     |
| RW             | NONE           | 禁止     |
| NONE           | RWX            | 禁止     |

这里暂时忽略 U/S、SUM、MXR、PMA 等额外检查条件。

## 5.3.3 PMP 也会检查 Page Table Walk

这是一个容易遗漏的细节。

PMP 不只检查最终访问的目标物理地址，也检查地址翻译过程中的隐式内存访问。

假设 Linux 页表的物理地址位于：

```
0x88004000
```

但这个地址恰好属于 OP-TEE RAM。

当 Linux 试图进行 Page Table Walk 时：

```
CPU
 |
 +-- Read Page Table Entry
       |
       v
     PA = 0x88004000
       |
       v
     PMP Check
       |
       v
     Denied
```

即使最终目标地址属于 Linux RAM，页表遍历也可能因无法读取 PTE 而失败。

RISC-V Privileged ISA 明确规定：页表遍历中的隐式内存访问同样受到 PMP 检查，其有效特权级按 S-mode 处理。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



所以在实际 Porting 中，要验证两个地址：

- Page Table 自身位于哪里？
- Page Table 指向的目标物理页位于哪里？

不能只检查最终的 TA RAM 或 Linux RAM。

## 5.3.4 为什么 PMP 变更之后需要 `SFENCE.VMA`？

假设 Hart 0 原本运行 Linux Domain：

```
TEE RAM = NONE
```

之后通过 OpenSBI 切换到 OP-TEE Domain：

```
TEE RAM = RWX
```

但 CPU 可能已经缓存了先前的地址翻译或者与 PMP 检查相关的结果。

因此，切换 PMP 配置后，不能仅仅认为 CSR 写入结束就完成了全部内存保护状态更新。

RISC-V Privileged ISA 要求：在启用虚拟内存的情况下，修改 PMP CSR 后使用全范围的 `SFENCE.VMA`，以同步 PMP 权限与地址翻译相关缓存；如果支持 Hypervisor Extension，还可能需要对应的 HFENCE 操作。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

RISC-V Documentation



```
# 示意：在 M-mode 修改 PMP 配置之后
# 执行本 hart 的地址翻译同步

sfence.vma x0, x0
```

这里要区分三个概念：

| 操作                        | 主要作用                 |
| ------------------------- | -------------------- |
| `FENCE`                   | 约束内存/IO 操作的可见性顺序     |
| `SFENCE.VMA`              | 同步本 Hart 的地址翻译相关状态   |
| Remote SFENCE / Shootdown | 让其他 Hart 同步其相关地址翻译状态 |

`SFENCE.VMA` 不是 Data Cache Flush，也不是向所有 Hart 自动广播 TLB Invalidation。

标准 PMP 是 Hart-local 的。如果只修改 Hart 0 的 PMP，主要需要同步 Hart 0 的保护与翻译状态；若同时修改了其他 Hart 使用的共享页表，则需要分析跨 Hart 的页表同步要求。

关于这部分，更深入的 SMP 处理将在 Chapter 17 讲解。

## 5.3.5 一个额外细节：MPRV

你之前学习过 RISC-V `mstatus.MPRV`。

这里它有一个重要作用。

当 M-mode 执行数据访问，而：

```
mstatus.MPRV = 1
mstatus.MPP  = S
```

该数据访问会按照 S-mode 的有效特权级执行相应权限检查。

因此，M-mode 并不是只要处于最高特权级，就在任何情况下都绕过 PMP。

对于需要代表 S-mode 访问内存的固件代码，MPRV 可以用于受限访问。

同时，它也是安全代码审查时必须关注的状态：M-mode Trap Handler 和 Domain Switch 代码需要正确管理 MPRV，防止隐式访问权限与开发者预期不一致。

本节结论：PMP 解决“当前 Domain 是否有权访问这个物理资源”，而 MMU 解决“当前执行上下文如何映射以及访问自己的地址空间”。

# 5.4 PMP Entry 数量有限时，如何设计内存布局？

## 5.4.1 问题：为什么不能给每一个内存对象单独设置 PMP？

假设某款 SoC 提供 16 个 PMP Entry。

但系统中可能存在：

```
OpenSBI Firmware
OP-TEE Core Text
OP-TEE Core Data
TA RAM Pool
Linux DRAM
Shared Memory
UART
SPI
DMA Controller
Crypto Engine
Secure Mailbox
Reserved Memory
...
```

如果每个对象都消耗一个 Entry，很快就会耗尽 PMP 资源。

因此在实际架构设计中，我们要考虑：

怎样用尽可能少的 PMP Entry，表达一个正确且最小权限的隔离策略？

我建议采用三类优化思想。

## 5.4.2 方法一：优先布局为 2 的幂大小且自然对齐的内存区域

例如：

```
TEE RAM Base = 0x88000000
TEE RAM Size = 16 MiB
```

该区域可以使用一个 NAPOT Entry 描述。

但假设另一块内存为：

```
Base = 0x88201000
Size = 10 MiB
```

就无法直接用单个 NAPOT Entry 精确描述。

可以使用多个 NAPOT Entry，也可以在具备条件时使用 TOR。

这也是为什么 SoC 的安全内存布局在芯片设计阶段就值得与 Firmware 团队一起规划。

安全内存区域的 Alignment 和 Size 不仅影响内存分配，还影响硬件隔离配置的复杂度。

## 5.4.3 方法二：使用高优先级例外区域 + 低优先级大范围区域

继续使用前面的 SoC：

```
0x80000000  OpenSBI 1 MiB
0x80100000  Linux RAM
0x88000000  OP-TEE RAM 16 MiB
0x89000000  Linux RAM
0x90000000  DRAM End
```

假设采用传统 PMP、NAPOT 且具备足够的地址匹配能力。

可以为 Linux Domain 设计下面三条 PMP Entry：

| PMP Entry | Region            | 权限   | 目的                 |
| --------- | ----------------- | ---- | ------------------ |
| PMP0      | OpenSBI 1 MiB     | NONE | 禁止 S/U 访问 Firmware |
| PMP1      | OP-TEE RAM 16 MiB | NONE | 禁止 Linux 访问 TEE    |
| PMP2      | 整个 256 MiB DRAM   | RWX  | 允许 Linux 使用其他 DRAM |

PMP0 和 PMP1 的编号低，因此优先级高。

PMP2 虽然覆盖整个 DRAM，但无法覆盖前面两条更高优先级的禁止规则。

这类似于：

```
Default: Allow DRAM
Exception 1: Deny Firmware
Exception 2: Deny Secure RAM
```

### 具体 CSR 编码

以上三个区域都能够通过 NAPOT 表示：

```
PMP0:
    base      = 0x80000000
    size      = 0x00100000
    pmpaddr0  = 0x2001FFFF
    cfg       = NAPOT | NONE = 0x18

PMP1:
    base      = 0x88000000
    size      = 0x01000000
    pmpaddr1  = 0x221FFFFF
    cfg       = NAPOT | NONE = 0x18

PMP2:
    base      = 0x80000000
    size      = 0x10000000
    pmpaddr2  = 0x21FFFFFF
    cfg       = NAPOT | RWX = 0x1F
```

下面是一个教育用途的 RV64 M-mode 配置片段：

```
# Demonstration only
# Assumptions:
# - M-mode
# - standard PMP semantics
# - PMP entries 0..2 implemented
# - no locked conflicting entries
# - memory layout as defined above

li   t0, 0x2001ffff
csrw pmpaddr0, t0

li   t0, 0x221fffff
csrw pmpaddr1, t0

li   t0, 0x21ffffff
csrw pmpaddr2, t0

# cfg0=0x18, cfg1=0x18, cfg2=0x1f
li   t0, 0x001f1818
csrw pmpcfg0, t0

sfence.vma x0, x0
```

这里使用的是未锁定的 PMP Entry，因此 M-mode 自身仍然能够访问这些区域。

生产固件不能直接把这一段当成通用初始化函数；需要额外考虑 CSR WARL 行为、PMP Grain、已有 Entry、启动时状态、异常路径和平台其他硬件保护机制。

## 5.4.4 方法三：区分静态保护和动态 Domain Protection

我建议把 PMP 规则分成两类：

静态保护规则

例如：

- OpenSBI 自身代码和数据的保护。
- 所有 S/U-mode 都不应该访问的固件专用内存。
- 平台不可修改的硬件安全边界。

动态保护规则

例如：

- 当前 Domain 能否访问 TEE RAM。
- 当前 Domain 能否访问某个 Shared Memory Region。
- 某些按 Domain 切换的执行权限。

静态规则适合在 Boot 阶段初始化，某些还可以使用锁定机制。

动态规则则必须允许 M-mode Firmware 在 Domain Switch 时重新配置。

这导致一个很重要的设计冲突：

如果你把一个需要动态切换的标准 PMP Entry 设置成 `L=1`，之后 OpenSBI 通常无法再更新这个 Entry。

所以不能为了追求 Lockdown，把所有 PMP Entry 都设置为 Locked。

启用 Smepmp 的平台又有不同的策略空间，必须针对所支持的扩展设计。

## 5.4.5 Staff-level 思考：如何处理 PMP Entry 不足？

假设硬件只有 8 个可用 PMP Entry，而安全策略需要 12 个互不连续的独立区域。

正确的处理不是简单地删除若干安全规则。

你应该按顺序检查：

1. 能否调整 Memory Layout，使区域满足 NAPOT Alignment。
2. 能否使用 TOR 以更少的 Entry 表示连续边界。
3. 能否通过高优先级 Exception + 低优先级大区域表示权限。
4. 能否将连续且权限相同的资源合并。
5. 能否将一部分访问限制交给专门的 SoC Firewall。
6. 如果硬件不能表达必需的最小安全策略，是否应该判定该平台配置不满足安全要求。

这里的核心原则是：

> 硬件资源不足是一个架构约束，而不是可以通过放松安全边界来掩盖的问题。

另外，OpenSBI Domain Memory Region 是软件抽象，不能假设任意数量的 Region 都必然能被底层 PMP 硬件精确表示。必须检查实际保护后端的编码、合并和失败处理行为。

# 5.5 为什么 PMP 无法保护 DMA？——CPU 与系统总线的边界

到这里，你应该已经能够解释 Linux Kernel 为什么不能通过修改页表直接访问 OP-TEE RAM。

但现在考虑另一个攻击场景。

## 5.5.1 攻击：通过 DMA 间接读取 TEE RAM

假设 Linux 控制一个 DMA Controller。

攻击者不能通过 CPU 直接读取：

```
TEE RAM = 0x88000000
```

于是改为配置：

```
// 简化的 DMA 寄存器配置示意
DMA_SRC = 0x88001000;
DMA_DST = 0x80200000;
DMA_LEN = 4096;
DMA_CTRL = START;
```

它希望硬件完成：

```
OP-TEE RAM
0x88001000
     |
     | DMA Read
     v
DMA Controller
     |
     | DMA Write
     v
Linux RAM
0x80200000
```

关键区别在于：

这次读取 OP-TEE RAM 的不是 CPU Hart，而是 DMA Controller。

普通的 Hart PMP 并不自动检查 DMA Controller 发起的系统总线事务。

为什么仅有 CPU PMP 不足以保护 TEE？

两个检查路径相互独立。左侧是 Hart CPU 访问；右侧是设备发起的总线事务。

如果没有对应的系统级保护，DMA 可能成为访问 Secure RAM 的旁路。

## 5.5.2 PMP、MMU 与 IOMMU 为什么不能互相替代？

这里引入 IOMMU。

IOMMU 的主要职责是控制设备发起的 DMA 地址翻译和访问。

例如：

```
Device IOVA
     |
     v
IOMMU
     |
     v
Physical Address
```

IOMMU 能够让 DMA Controller 只能访问分配给自己的缓冲区。

而 IOPMP 更侧重于对设备发起的物理地址访问实施权限控制。

| 机制           | 主要保护路径                                          |
| ------------ | ----------------------------------------------- |
| CPU MMU      | CPU Virtual Address Translation                 |
| Hart PMP     | CPU Physical Memory Access                      |
| IOMMU        | Device DMA Address Translation / Access Control |
| IOPMP        | Device-originated Physical Memory Protection    |
| SoC Firewall | 芯片内部 Bus/Peripheral/Memory Resource Access      |

IOMMU 和 IOPMP 在不同 SoC 上可以组合使用。是否必须同时具备，取决于平台的完整总线安全模型。

例如，一个 IOMMU 本身就可能被设计成足以执行某些设备隔离策略；但不能只因为 SoC 存在 IOMMU，就假设所有 DMA Master 都必然经过它。

## 5.5.3 除了 DMA，还有哪些资源 PMP 保护不了？

需要特别注意：

PMP 是对 Hart 所发起访问的限制，不是整个 SoC 的通用 Security Attribution Mechanism。

不能假设它能够单独保护：

- DMA Controller 的内存访问。
- GPU、NPU、ISP 等具备总线 Master 能力的设备。
- 其他 CPU Cluster 发起、未受当前 Hart PMP 控制的访问。
- 未经 PMP 检查的外设内部数据通路。
- 通过非可信 Firmware 或 Debug Interface 访问的安全数据。
- 非可信外设直接读取共享硬件资源。

另外，虽然 PMP 可以限制 CPU 对 MMIO 寄存器的访问，但这种限制不等于自动限制该外设自身随后发起的总线事务。

这就是为什么工业 SoC 的 TEE 设计通常需要 System-Level Hardware Isolation，而不能只依赖 CPU Privilege + PMP。

# 5.6 IOPMP、WorldGuard 与 SoC Firewall

现在我们知道，CPU PMP 不能自动约束设备发起的 DMA 访问。

因此下一步是回答：

如何给 CPU、DMA Controller、GPU、NPU 以及其他 Bus Master 建立统一的系统级安全边界？

我建议从两种不同的设计理念理解这个问题：

- IOPMP：以设备访问的物理地址权限为中心。
- WorldGuard：以整个系统的安全身份及资源访问策略为中心。

## 5.6.1 IOPMP：为设备访问建立物理内存保护

IOPMP（I/O Physical Memory Protection）的基本思想与 PMP 有相似之处，但是作用对象发生了变化。

PMP 判断的是：

```
Hart + Physical Address + Access Type
```

IOPMP 判断的是：

```
Requesting Device + Physical Address + Access Type
```

例如：

```
DMA Controller
      |
      | Requester Identity
      | PA = 0x88001000
      | READ
      v
    IOPMP
      |
      | Policy:
      | DMA0 cannot access OP-TEE RAM
      v
  Access Denied
```

RISC-V IOPMP 规范中有两个重要抽象：

- RRID（Request-Role-ID）：用于标识事务发起者所属的访问角色。
- MD（Memory Domain）：用来组织具有共同保护目标的内存访问规则。

IOPMP 可以使用 RRID 查找允许访问的 Memory Domain，再根据相应 Entry 的地址匹配和权限决定是否放行事务。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



举个例子：

| RRID | Requester            | 可访问资源                |
| ---- | -------------------- | -------------------- |
| 1    | Linux DMA0           | Linux RAM            |
| 2    | Linux GPU            | Linux RAM            |
| 3    | Secure Crypto Engine | TEE RAM / Shared RAM |
| 4    | Secure DMA           | TEE RAM              |

当 RRID=1 发起对 TEE RAM 的读取时，IOPMP 应拒绝该请求。

但是这里有一个极其重要的安全要求：

RRID 必须由可信的硬件路径绑定到实际事务发起者，不能由不可信的 Linux 软件任意伪造。

否则，Linux 只需要把自己伪装为 Secure DMA，即可绕过原来的权限限制。

IOPMP 的安全性不仅来自它的地址比较逻辑，还来自 Requester Identity 的可信性及配置寄存器的保护。

## 5.6.2 WorldGuard：以安全身份控制系统资源

WorldGuard 采用另一种思路。

它引入与访问者相关的 World Identifier（WID），让系统资源根据身份和访问策略进行检查。

概念上类似：

```
Linux Domain:
    WID = 1

OP-TEE Domain:
    WID = 2

System Resources:
    Linux RAM  -> WID 1 allowed
    TEE RAM    -> WID 2 allowed
    UART       -> WID 1 allowed
    HSM        -> WID 2 allowed
```

当 Hart 在不同 Domain 之间切换时，可信固件需要配置与该 Hart 相关的 WorldGuard 状态。

而系统中的 WorldGuard Checker 根据事务身份和资源权限进行访问判定。

WorldGuard Identity-Based Protection

Linux Domain

WID = 1

TEE Domain

WID = 2

WorldGuard Checkers

Identity + Address + Access Permissions

Linux RAM

WID 1

TEE RAM

WID 2

Shared RAM

WID 1 / 2

在 RP016 Milestone 5 的 WorldGuard 方案中，OpenSBI 需要完成两类工作：

Boot-time Configuration

识别 WorldGuard Checker、读取 Device Tree 中的硬件安全策略，并配置 Checker 的 MMIO 状态。

Runtime Domain Switching

在 Domain 切换过程中，更新 Hart 对应的 WorldGuard WID/Delegation 状态。

RP016 M5 的示例实现使用 `MLWID`、`MWIDDELEG`、`SLWID` 等相关 CSR，并通过 QEMU WorldGuard 模型验证。其公开页面记录的是 2026 年 5 月提交的评审版本，因此后续阅读时需要区分该开发分支与当前 OpenSBI 主线。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### WorldGuard 的架构优势

它的重要价值是可以将安全策略表达为：

> 某个安全身份允许访问哪些系统资源。

而不仅仅是：

> 当前 Hart 是否允许访问一块物理内存。

当然，这不意味着使用 WorldGuard 就不需要 PMP。

是否仍需要 PMP、如何组合 PMP 和 WorldGuard，以及同一物理内存是否经过多个 Checker，取决于具体 SoC 的安全架构与硬件实现。

## 5.6.3 SoC Firewall：更广义的硬件安全隔离

有些 SoC 没有采用 WorldGuard，也没有完整实现通用 IOPMP。

它们可能提供厂商自己的硬件模块，例如：

```
AXI / NoC Access Controller
Memory Firewall
Peripheral Protection Controller
DMA Security Attribution
Bus Master Access Filter
```

这些模块通常可以按照以下属性进行访问控制：

```
Master ID
Security Attribute
Transaction Address
Read / Write Type
Privilege Attribute
```

需要注意，以上属性并不是所有 SoC Firewall 都完整支持。

例如某款硬件可能只支持：

```
Master ID + Physical Address + RW Permission
```

那么它未必可以直接实现按 Linux Domain / OP-TEE Domain 身份动态切换的安全策略。

因此，移植时不能只检查芯片手册里有没有一个名为 Firewall 的模块，还要检查它的实际能力。

## 5.6.4 三者应该如何选型？

| 需求                 | PMP                | IOPMP                     | WorldGuard / 适配的 SoC Firewall |
| ------------------ | ------------------ | ------------------------- | ----------------------------- |
| CPU 无法访问 TEE RAM   | 支持                 | 非主要职责                     | 视平台而定                         |
| DMA 无法访问 TEE RAM   | 不负责                | 支持                        | 视平台而定                         |
| 按设备身份授权            | 不支持                | 支持                        | 视平台而定                         |
| 按安全 Domain 身份授权    | 需要由 OpenSBI 动态配置   | 需建立 Requester/Domain 对应关系 | 可支持                           |
| 控制 Peripheral MMIO | 可限制 Hart 的 MMIO 访问 | 视覆盖范围而定                   | 可支持                           |
| M-mode 可管理         | 是                  | 取决于配置接口                   | 取决于平台                         |

这里还有一个必须认识到的原则：

安全的 SoC 需要保证所有能够访问敏感资源的实际硬件路径都受到相应的访问控制。

如果 GPU 可以通过一条未经过 IOPMP/Firewall 的总线路径访问 OP-TEE RAM，那么其他路径配置得再完善，也无法弥补这一缺口。

# 5.7 OpenSBI System-Level Hardware Isolation Framework

这是本章与实际 OpenSBI 源码结合最紧密的部分。

前面讨论了三个不同的问题：

1. OpenSBI Domain 描述资源访问策略。
2. PMP 保护 Hart 的物理内存访问。
3. IOPMP / WorldGuard / SoC Firewall 保护更多的系统资源。

那么，OpenSBI 应该如何统一管理这些硬件机制？

## 5.7.1 Why：为什么 OpenSBI 需要一个通用隔离框架？

假设有三款 SoC：

```
SoC A:
    PMP + IOPMP

SoC B:
    PMP + WorldGuard

SoC C:
    PMP + Vendor-specific Firewall
```

如果所有保护逻辑都硬编码进 Domain Switching：

```
switch_domain()
{
    save_context();

    configure_pmp();
    configure_iopmp();
    configure_worldguard();
    configure_vendor_firewall();

    restore_context();
}
```

会存在几个明显问题：

- 不同硬件平台需要反复修改通用 Domain Switch 代码。
- 通用代码依赖大量 SoC 特有的宏和分支。
- 新的隔离机制很难独立测试。
- 多个机制初始化及切换的顺序难以维护。
- 每个 Domain 的硬件策略缺少统一管理方式。

因此，更合理的设计是：

通用 Domain 框架管理生命周期，具体的硬件 Isolation Driver 负责各自的硬件操作。

这一思路与 Linux Driver Model 或 NuttX Driver Framework 的抽象思想类似。

## 5.7.2 OpenSBI 当前已有的 Hart Protection 接口

先看 OpenSBI 主线中实际存在的实现。

重点文件：

[lib/sbi/sbi_hart_protection.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart_protection.c?utm_source=chatgpt.com)

该文件提供了注册和执行 Hart Protection 机制的管理接口，包括：

```
sbi_hart_protection_configure(...)
sbi_hart_protection_unconfigure(...)
sbi_hart_protection_reconfigure(...)
```

在当前代码中，`sbi_hart_protection_reconfigure()` 的主要逻辑可以抽象为：

```
int reconfigure_protection(current_dom, next_dom)
{
    for_each_registered_hart_protection(hprot) {
        if (!should_apply(hprot))
            continue;

        hprot->unconfigure(current_dom);

        ret = hprot->configure(next_dom);
        if (ret)
            return ret;
    }

    return 0;
}
```

上面是根据实际实现整理的教学伪代码，而不是原函数的逐字复制。

其设计思路是：

```
Current Domain
      |
      v
Unconfigure Current Protection
      |
      v
Configure Next Protection
      |
      v
Next Domain
```

当前实现会区分 Memory Protection 类型与 Identity Protection 类型，并对其执行不同的选择逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这一层抽象主要面向 Hart 保护能力。

## 5.7.3 RP016 M4：进一步引入系统级 Hardware Isolation Framework

如果还希望管理独立的 IOPMP、WorldGuard Checker 或 SoC Firewall，单纯的 Hart Protection 接口不一定足够。

因此 RP016 M4 提出了通用的系统级 Hardware Isolation Framework。

其设计中使用的核心接口如下：

```
struct sbi_hwiso_ops {
    const char *name;

    int (*init)(void *fdt);

    int (*domain_init)(
        void *fdt,
        int domain_offset,
        struct sbi_domain *dom,
        void **ctx);

    void (*domain_exit)(
        const struct sbi_domain *src,
        const struct sbi_domain *dst,
        void *ctx);

    void (*domain_enter)(
        const struct sbi_domain *dst,
        const struct sbi_domain *src,
        void *ctx);

    void (*domain_cleanup)(
        struct sbi_domain *dom,
        void *ctx);
};
```

这是 RP016 M4 文档中的设计接口。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



理解这个结构时，不要只记住函数名。

要理解其背后的两个维度：

时间维度：

```
Boot
  |
  +-- Register Mechanism
  |
  +-- Global Initialization
  |
  +-- Per-Domain Policy Initialization
  |
  v
Runtime
  |
  +-- Domain Exit
  |
  +-- Domain Enter
  |
  +-- ...
```

数据维度：

```
Global Hardware Resources
      |
      +-- IOPMP instances
      +-- WorldGuard Checkers
      +-- Platform Firewall
      |
      v
Per-Domain Security Policy
      |
      +-- Linux Domain Policy
      +-- OP-TEE Domain Policy
      +-- Other Domain Policy
```

这种设计很值得你在未来进行 Upstream Review 时重点研究。

### `init()`：硬件资源初始化

这一阶段需要识别全局硬件资源。

例如 WorldGuard 驱动可以解析：

```
WorldGuard Checker MMIO Base
Supported WIDs
Resource Protection Regions
Hardware Capabilities
```

然后构建一个全局硬件模型。

### `domain_init()`：每个 Domain 的 Policy 初始化

同一个 WorldGuard Checker 可以保护不同 Domain 的资源。

因此除了全局硬件信息，还需要对应每个 Domain 的访问策略。

例如：

```
Linux Domain:
    WID = 1

OP-TEE Domain:
    WID = 2
```

`domain_init()` 的职责是解析、校验并保存这些 Policy。

在安全架构上，应尽量将复杂的策略验证安排在此阶段，而不是把所有可能失败的操作推迟到运行时切换路径。

### `domain_exit()` 和 `domain_enter()`：生命周期回调

当 Hart 离开 Linux Domain 时：

```
domain_exit(Linux)
```

当 Hart 进入 OP-TEE Domain 时：

```
domain_enter(OP-TEE)
```

不同的硬件隔离机制通过各自的回调维护相应状态。

例如：

```
WorldGuard:
    Update WID state

Hart PMP:
    Reconfigure PMP regions

Vendor Firewall:
    Update domain-related access policy
```

注意这三个机制并不一定通过同一类接口实现，具体依赖当前 OpenSBI 版本和硬件后端。

### 为什么 Runtime Callback 返回 `void` 值值得注意？

RP016 M4 中的 `domain_exit()` 和 `domain_enter()` 返回 `void`。

这意味着这套设计更适合将运行时操作建立在已经完成验证、预期不会失败的配置上。

但实际硬件可能出现：

```
Firewall timeout
MMIO write error
Unexpected hardware state
Invalid transition state
```

因此在生产级设计中必须明确：

如果隔离硬件更新失败，是否有可靠的 Fail-Closed 路径，确保非可信 Domain 不会在权限不确定的状态下恢复执行？

这正是适合在 RFC 和 Code Review 中提出的架构问题。

## 5.7.4 Device Tree 如何描述硬件隔离 Policy？

RP016 M4 采用 Device Tree 描述 per-domain isolation policy。

例如：

```
domain@1 {
    compatible = "opensbi,domain,instance";

    /* other domain configuration */

    hw-isolation {
        wg-demo {
            compatible = "sifive,wgchecker2";

            worldguard,wid = <1>;
            worldguard,widlist = <1 3>;
        };
    };
};
```

这是该项目开发分支采用的绑定形式，并不意味着任意 OpenSBI 版本都接受这组属性。

Device Tree 在这里承担两个角色：

1. 描述硬件上有哪些 Isolation Controller。
2. 描述不同 Domain 的 Isolation Policy。

但 Device Tree 本身只是配置数据。

如果 Device Tree 可以被不可信 Linux 任意替换，再交给 OpenSBI 用于安全初始化，那么就会产生信任链问题。

因此真正的 Secure Boot 设计，还需要保护安全策略的来源、完整性，以及它与可信 Firmware 的绑定关系。

## 5.7.5 当前项目的实际实现状态

根据 RP016 的公开记录：

| 工作 | 内容                                        | 公开项目状态                          |
| -- | ----------------------------------------- | ------------------------------- |
| M4 | 通用 Hardware Isolation Framework、生命周期回调与测试 | 已实现并在 QEMU 验证，相关 Patch 处于上游评审流程 |
| M5 | 在 Framework 上加入具体的 WorldGuard 支持          | QEMU 上实现与验证，项目记录为评审中            |

这两个 Milestone 的区别非常关键。

M4 主要证明框架可以工作；M5 才把它连接到更具体的 WorldGuard 硬件行为。&#x20;

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE

+1



# 5.8 Domain Switch：如何安全地切换 PMP 和系统隔离权限？

这是本章最重要的实际工程问题之一。

我们已经知道：

```
Linux Domain -> OP-TEE Domain
```

需要修改 PMP、Domain Context 和可能存在的系统级访问身份。

但更深的问题是：

这些修改并不是在一个原子操作中完成的。那么在修改的中间阶段，是否可能存在某种权限配置，使 Linux 获得了不应该拥有的访问能力？

## 5.8.1 首先明确一个重要事实

以正常的 S-mode → M-mode → S-mode Domain Switch 为例：

当 CPU 进入 M-mode 执行 OpenSBI 时，原来的 Linux S-mode 代码已经不再在这个 Hart 上执行。

因此，即使 M-mode 在更新 PMP 时短暂出现中间配置状态，也不能简单断言 Linux 可以在同一个 Hart 上同时利用这个状态访问 TEE RAM。

但是，这并不代表可以忽略更新顺序。

原因包括：

- 异常和中断可能改变预期执行路径。
- M-mode 自身可能发生错误。
- 其他 Hart 仍然并行执行。
- DMA Controller 可能仍有正在执行的传输。
- 某些系统级 Firewall 配置可能是全局性的。
- 状态更新可能失败，或者部分成功。
- CPU 可能保存了需要同步的地址翻译状态。

因此要从整个系统，而不是单一 Hart 的角度验证安全性。

## 5.8.2 一个安全的切换状态机

我建议先建立以下抽象状态机：

Domain A → Domain B 安全切换模型

1. Enter Trusted Firmware

   从 Domain A Trap 进入可信 M-mode，保存当前执行上下文。
2. Quiesce Relevant Operations

   对需要动态转移的设备或共享资源，停止新请求并确认必要的在途事务状态。
3. Revoke / Transition Isolation State

   更新与旧 Domain 有关的 PMP、WorldGuard 或 Firewall 状态。
4. Configure Destination Domain

   设置目标 Domain 的身份、物理访问权限及必要的硬件状态。
5. Synchronize and Validate

   完成适用的 Fence、TLB 同步和保护状态校验。
6. Restore Destination Context

   恢复目标 Domain 的 CSR、Trap Context 和其他架构状态。
7. Return to S-mode

   确认隔离已经生效之后，通过 `mret` 恢复目标 Domain 执行。

这是用于建立安全不变量的逻辑顺序，不是 OpenSBI 代码逐条语句的实际执行顺序。实际实现可能在保持 M-mode 控制权的期间交叉保存 Context 和更新隔离状态。

关键的安全边界不是每次 CSR 写入，而是“重新允许非可信执行上下文运行”的那一刻。

在执行 `mret` 之前，目标 Domain 的全部必需硬件隔离状态必须已经正确建立。

## 5.8.3 与真实 OpenSBI Domain Switch 代码对照

重点文件：

[lib/sbi/sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)

当前主线中有：

```
static int switch_to_next_domain_context(
    struct hart_context *ctx,
    struct hart_context *dom_ctx);
```

实际代码中会完成以下主要操作：

```
1. 更新 Hart 的 Domain Assignment

2. Save / Restore S-mode CSRs
   - sstatus
   - sie
   - stvec
   - sscratch
   - sepc
   - scause
   - stval
   - sip
   - satp
   - ...

3. Save / Restore FP Context

4. Save / Restore Vector Context

5. Save / Restore Trap Context

6. Reconfigure Hart Protection

7. Continue / Resume Destination Context
```

其中关键调用为：

```
sbi_hart_protection_reconfigure(
    scratch,
    current_dom,
    target_dom);
```

这是当前源码中实际存在的接口。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



我们需要特别区分两个问题：

问题一：为什么 `satp` 可以在 PMP 更新之前发生变化？

因为此时控制权仍在可信 M-mode Firmware 内。

在没有不可信 S-mode 指令运行的前提下，恢复目标 `satp` 本身不等于已经把目标 Domain 放回运行状态。

但这仍要求 M-mode 的内存访问、Trap Handling 和地址翻译同步逻辑正确。

问题二：为什么 Hart Protection 更新后需要 Fence？

因为新 Domain 可能使用不同的 PMP Policy、`satp` 和地址翻译状态。

OpenSBI 当前代码也明确将保护状态重新配置与完整的 SFENCE/HFENCE 同步关联起来。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 5.8.4 设备 DMA 为什么让问题变得更复杂？

现在考虑一个危险场景：

```
Time T0:
    Linux owns DMA0
    DMA0 begins transferring data

Time T1:
    Hart enters OpenSBI

Time T2:
    OpenSBI switches to OP-TEE Domain

Time T3:
    DMA0 is still active
```

如果 DMA0 的权限与 Domain 切换之间没有清晰的生命周期管理，可能出现两种问题：

- DMA 继续访问不再属于原 Domain 的内存。
- Firmware 修改 Firewall 后，正在进行的 DMA 因新规则失败，导致设备状态不一致。

这里的解决办法不能简单归结为执行一个 `FENCE`。

RISC-V `FENCE` 不能自动保证外部 DMA Engine 已经完成所有传输。

生产级设计需要结合：

- DMA Engine 的停止、取消和完成确认机制。
- Buffer Ownership Transfer。
- Cache Maintenance 和 Memory Ordering。
- Firewall Policy 更新时的硬件行为。
- IOMMU/IOPMP 配置更新的同步机制。
- 超时与错误恢复。

而且不必每次普通 Domain Switch 都停止所有 DMA。

只有涉及资源所有权转移、访问权限变化或者相应设备隔离策略要求时，才需要执行对应的 Quiesce / Transfer 流程。

## 5.8.5 SMP 下最危险的问题：全局状态和 Hart-local 状态混淆

假设：

```
Hart 0: Linux Domain
Hart 1: OP-TEE Domain
```

如果使用 Hart-local PMP：

```
Hart 0 PMP: Linux Policy
Hart 1 PMP: TEE Policy
```

两套配置可以同时存在。

但是，如果某款硬件的 Firewall 使用一组全局权限寄存器：

```
GLOBAL_FIREWALL_CONFIG
```

而你在 Hart 1 进入 OP-TEE 时，把整个系统的访问策略都改成：

```
TEE Policy
```

那么 Hart 0 的 Linux 执行可能立即受到影响。

更严重的是，错误配置可能导致另一个 Hart 意外获得安全资源访问权。

因此，必须在设计阶段明确：

| 保护状态                             | Scope                 |
| -------------------------------- | --------------------- |
| Standard PMP CSR                 | Hart-local            |
| `satp`                           | Hart-local            |
| Domain Memory Policy             | Software Domain State |
| Per-Hart WorldGuard WID          | 取决于平台实现               |
| Global WorldGuard Checker        | System-level          |
| DMA Firewall                     | Device/System-level   |
| IOMMU Domain / Translation State | 与设备、硬件实例有关            |

一个多核 TEE 系统必须能够表达多个 Domain 同时运行，而不能依赖某个会被不同 Hart 反复覆盖的全局安全开关。

# 5.9 Hardware Isolation 的安全风险与验证方法

前面讨论了机制、架构以及实现顺序。

最后，需要站在 Security Reviewer 的角度，回答一个问题：

怎样证明一套 RISC-V OP-TEE 的隔离设计真的安全？

我建议从九类风险入手。

## 5.9.1 风险一：PMP Region 覆盖不完整

例如：

```
TEE RAM:
0x88000000 - 0x88FFFFFF

实际 PMP Deny:
0x88000000 - 0x887FFFFF
```

结果是：

```
0x88800000 - 0x88FFFFFF
```

仍然可能被 Linux 访问。

这属于典型的 Security Boundary Gap。

应该使用自动化测试验证每个 region 的：

```
Base
Size
End
Alignment
Permission
Overlap
```

同时覆盖每个 region 边界前后的地址。

## 5.9.2 风险二：PMP Priority 错误

假设：

| Entry | Region      | Permission |
| ----- | ----------- | ---------- |
| PMP0  | Entire DRAM | RWX        |
| PMP1  | TEE RAM     | NONE       |

如果两个区域发生重叠，PMP0 优先级更高，PMP1 就不能覆盖 PMP0 的允许规则。

因此这套配置不能有效拒绝 Linux 访问 TEE RAM。

正确设计需要根据 PMP 的低编号优先规则重新排列 Entry。

不能只检查 PMP Entry 是否存在，还必须检查所有交叠 Entry 的优先级。

## 5.9.3 风险三：PMP 状态与 TLB 不一致

例如：

```
Old Domain:
    TEE RAM = RWX

New Domain:
    TEE RAM = NONE
```

若在修改 PMP 后没有按架构要求同步相关的地址翻译缓存，可能导致实际访问行为与软件认为的新策略不一致。

需要验证：

- PMP CSR 写入结果。
- `SFENCE.VMA` 的执行路径。
- 相关的 Hypervisor Fence。
- 多 Hart 页表修改对应的 Shootdown。
- Context Switch 和异常退出路径。

## 5.9.4 风险四：设备绕过 PMP

这就是前面讨论的 DMA Bypass。

验证时不能只检查：

```
Linux CPU -> TEE RAM
```

还需要检查：

```
Linux-controlled DMA -> TEE RAM
Linux-controlled Device -> Secure Peripheral
Other Bus Master -> TEE RAM
```

尤其要验证硬件的默认策略是否为 Deny，以及所有实际 DMA 路径是否都经过相应的访问控制单元。

## 5.9.5 风险五：Domain Identity 被伪造

如果 WorldGuard 或其他 Firewall 使用可由低可信软件控制的 Identity，那么基于 Identity 的权限模型将不可靠。

需要检查：

- 谁可以修改 WID？
- 谁可以配置 WID Delegation？
- 谁可以修改 Checker Policy？
- Hart 切换 Domain 后，旧的 WID 是否正确失效？
- DMA Requester Identity 是否绑定到实际设备？
- Linux 是否可以通过 MMIO 接口修改安全权限？

这些问题往往比单纯检查一段 PMP 配置代码更有价值。

## 5.9.6 风险六：Boot-time Protection Window

假设：

```
Reset
  |
  v
Boot ROM
  |
  v
OpenSBI
  |
  +-- Initializes Linux Domain
  |
  +-- Configures PMP
  |
  v
Linux
```

需要确认：

在 M-mode 将控制权交给非可信 S-mode 之前，必需的隔离是否已经全部建立？

如果保护寄存器初始化顺序不正确，可能产生安全窗口。

还要审查 Secondary Hart 启动流程，确认每个 Hart 都在进入非可信执行环境前获得正确的保护配置。

## 5.9.7 风险七：Domain Switch Failure

假设：

```
Switch Linux -> OP-TEE

Step 1: Save Linux Context      SUCCESS
Step 2: Update Domain Identity  SUCCESS
Step 3: Configure PMP           SUCCESS
Step 4: Configure Firewall      FAILED
Step 5: Restore OP-TEE Context  ?
```

此时绝对不能默认：

```
Firewall failed -> continue boot
```

而应该明确设计：

- 是否可以恢复到一个已知安全的隔离状态？
- 是否必须停止当前 Hart？
- 是否必须 Panic？
- 是否需要 Reset 某个安全子系统？
- 系统是否可以保证 Fail-Closed？

在安全敏感的 Firmware 中，错误处理路径有时比正常路径更值得 Review。

这里还有一个实际源码审查点：

当前 OpenSBI 中 `sbi_hart_protection_reconfigure()` 返回 `int`，而所查版本的 `switch_to_next_domain_context()` 调用该函数时，并未直接使用返回值。

这不能仅凭一处调用就认定存在漏洞：还需要研究后端的错误可能性、实际控制流、调用约束和版本差异。但它是一个很适合深入开展 Error Propagation / Fail-Closed 审查的具体切入点。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



## 5.9.8 风险八：不可信固件或 Device Tree 篡改安全策略

假设 OpenSBI 通过 Device Tree 读取：

```
worldguard,wid = <2>;
```

攻击者将安全配置修改为允许 Linux 使用相同身份。

如果初始化过程中缺乏可信校验，就可能破坏 Domain Isolation。

因此：

- 策略应当来自经过信任链保护的配置。
- 安全配置不应该被不可信 Linux 任意覆盖。
- 配置中的非法地址、重复 Region、权限冲突需要被明确拒绝。
- Firmware 不能因无法解析关键保护属性而悄悄退化成 Allow All。

## 5.9.9 风险九：被忽略的微架构状态和调试接口

到目前为止，我们讨论的是 CPU 的架构可见访问权限。

但工业级 TEE 设计还需要考虑：

- Cache Timing Side Channel。
- Branch Predictor / Microarchitectural State。
- Debug/JTAG 权限。
- Performance Counter 暴露的信息。
- 中断和异常状态泄露。
- FP/Vector Context 清理。
- Reset/Resume 时是否保留敏感状态。

PMP 的架构访问限制，并不自动构成对所有 Side Channel 的防护。

因此一份安全设计文档应该明确写出它保障的安全属性，以及不在当前保护范围内的威胁。

# 六、实战实验：在 WSL2 + QEMU 中验证 Hardware Isolation

我建议设计三个层次的实验。

## 实验 A — PMP Address Matching 单元实验

目标不是先启动 OP-TEE，而是验证自己是否真正掌握 PMP 编码和 Priority。

可以写一个独立程序来模拟 PMP Region 的基本地址匹配。

下面使用 Python 演示标准 NAPOT 的编码：

```
def encode_napot(base, size):    assert size >= 8    assert (size & (size - 1)) == 0    assert base % size == 0    return (base >> 2) | ((size >> 3) - 1)regions = [    ("Firmware", 0x80000000, 0x00100000),    ("TEE RAM",  0x88000000, 0x01000000),    ("DRAM",     0x80000000, 0x10000000),]for name, base, size in regions:    addr = encode_napot(base, size)    print(f"{name:10s}: pmpaddr=0x{addr:08x}")
```

预期输出：

```
Firmware  : pmpaddr=0x2001ffff
TEE RAM   : pmpaddr=0x221fffff
DRAM      : pmpaddr=0x21ffffff
```

然后进一步扩展程序，实现：

```
pmp_match(address, size)
pmp_check(address, size, privilege, access_type)
```

重点测试：

- NAPOT Alignment。
- TOR 上下边界。
- 跨越 Region 边界的访问。
- Overlapping Entry Priority。
- No Matching Entry。
- M-mode 的 Locked / Unlocked Rule。

这是非常适合编写成持续回归测试的小项目。

## 实验 B — 运行 RP016 Hardware Isolation Framework 测试

RP016 M4 提供了一个可复现的 Buildroot/QEMU 实验环境。

在 WSL2 中可以使用如下命令：

```
git clone \
  https://gitlab.com/riseproject/riscv-optee/buildroot.git \
  -b rp016_m4

cd buildroot

make qemu_riscv64_virt_optee_defconfig

make -j"$(nproc)"
```

完成构建后：

```
./output/images/start-qemu-dto.sh
```

这是该 Milestone 文档提供的构建和运行方式。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



正常情况下，可以看到类似：

```
[HWISO] init sifive,wgchecker2

[HWISO] domain exit
[WG] wg_demo_domain_exit

[HWISO] domain enter
[WG] wg_demo_domain_enter

[PASSED] hwiso_domain_switch_test
```

本实验应完成四项观察：

1. 追踪 Hardware Isolation Mechanism 的注册过程。
2. 观察 Device Tree 如何转换成 Per-Domain Context。
3. 设置断点，跟踪 `domain_exit()` 和 `domain_enter()`。
4. 检查测试实际覆盖的是 Hook/Policy Lifecycle，还是硬件访问真正被拒绝。

最后一点尤其重要：某个测试证明回调执行成功，并不自动证明真实总线访问已经受到保护。

## 实验 C — WorldGuard 真实访问隔离验证

这个实验可以基于 RP016 M5 和对应的 QEMU WorldGuard 模型继续开展。

测试目标：

```
Domain A: WID = 1
Domain B: WID = 2
```

构造：

| Access                | 预期    |
| --------------------- | ----- |
| Domain A → RAM A      | Allow |
| Domain A → RAM B      | Deny  |
| Domain B → RAM B      | Allow |
| Domain B → RAM A      | Deny  |
| Domain A → Shared RAM | Allow |
| Domain B → Shared RAM | Allow |

然后结合异常处理记录：

```
Access Address
Access Type
Current Domain
Current WID
Fault Cause
Faulting PC
```

特别需要在测试报告中区分：

- Page Fault：例如虚拟地址映射无效。
- PMP / Physical Access Fault：物理权限不允许。
- WorldGuard / Firewall Fault：由系统级 Checker 拒绝的事务。

这些故障的上报机制可能不同，实际表现需要对照硬件或 QEMU 模型，而不能简单假设它们都具有相同的 `scause`。

对于 CPU PMP Load Access Fault，异常原因编码为 5；但 Trap 最终由 S-mode 还是 M-mode 处理，还取决于相应的 Exception Delegation 配置。

实验的最终验收标准

不只是证明 Linux 正常运行、OP-TEE 正常运行，而是：

当 Linux 故意访问不属于自己的 Secure RAM 时，能够捕获并解释具体硬件拒绝路径，同时证明 TEE 的合法访问不受影响。

# 七、Staff Engineer 级别测试题

建议你在学习结束后，不看前面的内容，尝试独立回答下面的问题。

10 道核心考核题 · 点击查看参考答案

展开全部

Q1. 为什么 Linux 和 OP-TEE 都处于 S-mode，PMP 仍然可以隔离二者？

Q2. 如果 Linux 修改自己的 PTE，将 TEE RAM 映射为 RWX，为什么仍无法访问？

Q3. PMP0 允许整个 DRAM RWX，PMP1 禁止 TEE RAM，能否保护 TEE？

Q4. 为什么在 PMP 变更后执行 SFENCE.VMA？它能清空 Data Cache 吗？

Q5. 为什么不建议对所有 PMP Entry 都设置 L=1？

Q6. CPU PMP 已经拒绝 Linux 访问 TEE RAM，为什么 DMA 仍可能读取？

Q7. IOPMP 的 Requester ID 为什么不能由 Linux 随意指定？

Q8. Domain Switch 期间 PMP 权限短暂改变，是否意味着 Linux 一定能利用这一窗口？

Q9. Hardware Isolation Framework 为什么需要 domain_init、domain_exit、domain_enter 三类回调？

Q10. 如果 SoC 只有少量 PMP Entry，无法表达所有安全边界，应该如何处理？

# 八、回到本质：本章真正需要建立的安全架构认知

学完 Chapter 05，你应该能够形成下面这个完整模型：

```
                Domain Security Policy
                         |
                         v
                      OpenSBI
                         |
             +-----------+-----------+
             |                       |
             v                       v
       Hart Protection      System-Level Protection
             |                       |
       PMP / Smepmp         IOPMP / WorldGuard
             |               / SoC Firewall
             |                       |
             v                       v
        CPU Access              Device / Bus Access
             |                       |
             +-----------+-----------+
                         |
                         v
               Physical Resources
                         |
             +-----------+-----------+
             |                       |
          Linux RAM               TEE RAM
```

我希望你重点记住三项设计原则。

第一，Domain Policy 不等于 Hardware Isolation。

OpenSBI 中的 Domain Memory Region 只是软件安全策略的表达，真正的安全保证来自后续硬件保护配置的正确性和完整性。

第二，PMP 不等于 SoC Security Boundary。

PMP 可以有效限制 Hart 的物理地址访问，但整个 SoC 还存在 DMA、GPU、外设和其他 Bus Master。安全设计必须覆盖实际的硬件访问路径。

第三，安全隔离不是一次性初始化，而是一个生命周期问题。

Boot、Domain Switch、SMP、Interrupt、DMA Ownership Transfer、Suspend/Resume、Reset 和 Error Recovery 都可能改变系统的安全状态。

因此，真正的架构级问题是：

\\[ \boxed{ \text{在任意执行状态和状态转换期间，} \\\ \text{不可信 Domain 都不能访问未经授权的资源。} } \\]

这是我们下一章 Chapter 06 — OpenSBI Domain Context Switching 要进一步落实到实际汇编、CSR 保存恢复和 Hart Context 的核心安全不变量。

## 本章参考源码与规范

| 资料                                                                                                                                                | 核心研究内容                                                |
| ------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------- |
| [RISC-V Privileged ISA — Machine-Level ISA](https://docs.riscv.org/reference/isa/v20260120/priv/machine.html?utm_source=chatgpt.com)              | PMP Address Matching、Priority、Permission、PMP 与 Paging |
| [OpenSBI — sbi_hart_protection.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart_protection.c?utm_source=chatgpt.com) | Hart Protection 的注册、配置及重新配置                           |
| [OpenSBI — sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)   | Domain Context Switch 与 Protection Reconfiguration    |
| [RP016 Milestone 4](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/milestone4/?utm_source=chatgpt.com)                                           | 通用 Hardware Isolation Framework                       |
| [RP016 Milestone 5](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/milestone5/?utm_source=chatgpt.com)                                           | WorldGuard Boot-time 和 Runtime Isolation              |
| [RISC-V IOPMP Specification](https://github.com/riscv-non-isa/riscv-iopmp?utm_source=chatgpt.com)                                                 | 设备 Requester、Memory Domain 与物理访问保护                    |

Chapter 05 的完成标准： 能够独立分析一个 RISC-V SoC 的物理内存布局，设计 Linux/OP-TEE 的 PMP 权限方案，识别 DMA 等硬件保护缺口，跟踪 OpenSBI Protection Reconfiguration，并设计可以验证安全隔离的测试。这才算真正从 PMP 寄存器知识进入了 TEE System Security Architecture。