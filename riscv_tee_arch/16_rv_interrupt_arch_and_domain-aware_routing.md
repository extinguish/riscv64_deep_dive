# Chapter 16 — RISC-V Interrupt Architecture & Domain-Aware Routing

《RISC-V OP-TEE Architecture & Porting》· 第 16 章

本章主题：Interrupt Routing、Privilege Delegation、Domain Switching、VIRQ Delivery 与 TEE Security

本章将从 RISC-V Interrupt Architecture 的硬件设计开始，逐步深入 OpenSBI、OP-TEE、Linux 三者之间的中断协作，最终分析多核 RISC-V SoC 中如何实现安全、可靠、可预测的 Domain-Aware Interrupt Routing。

## 本章学习目标

完成本章后，应该能够：

1. 从硬件层面解释 PLIC、APLIC、IMSIC 与 RISC-V Privilege Mode 的关系。
2. 分清 Interrupt Delegation、Interrupt Ownership、Interrupt Routing 和 Interrupt Handling 四个不同概念。
3. 理解为什么一个属于 OP-TEE 的中断，可能在 Linux 正在执行时首先进入 OpenSBI。
4. 从源码和 CSR 层面解释 HWIRQ → VIRQ → Domain → ISR 的完整执行链。
5. 设计 Secure Interrupt、Foreign Interrupt、Domain Switch、Nested Interrupt 的处理方案。
6. 分析多 hart 条件下的 Interrupt Storm、Lost Interrupt、Race Condition 及安全隔离问题。
7. 在 QEMU 和实际 SoC 上验证中断控制器、固件路由策略和 OP-TEE 的协作。

## 本章目录

| 节次    | 主题                             | 核心问题                          |
| ----- | ------------------------------ | ----------------------------- |
| 16.1  | PLIC / APLIC / IMSIC           | 谁接收、路由和投递硬件中断？                |
| 16.2  | Interrupt Delegation           | 谁决定 Trap 进入 M-mode 还是 S-mode？ |
| 16.3  | M/S Interrupt Ownership        | 谁真正拥有设备及其中断？                  |
| 16.4  | Domain-Aware Routing           | 如何将中断投递给正确的 Domain？           |
| 16.5  | HWIRQ / VIRQ Mapping           | 中断 ID 如何映射并隔离？                |
| 16.6  | Foreign Interrupt & Yield      | OP-TEE 如何应对属于 Linux 的中断？      |
| 16.7  | Interrupt-Driven Domain Switch | 中断如何触发执行环境切换？                 |
| 16.8  | Priority / Pending / EOI       | 如何避免中断丢失与重复触发？                |
| 16.9  | Storm / Lost IRQ / Recovery    | 工业现场如何定位与恢复故障？                |
| 16.10 | Secure Interrupt Isolation     | 如何保证中断不能突破 TEE 安全边界？          |

本章最后还包含 源码分析、QEMU 实验、工业设计检查清单，以及 12 道附参考答案的 Staff Engineer 级面试题。

# 16.0 从一个真实的系统问题开始

假设我们有一颗 RV64 Server SoC：

- 16 个 Application Harts。
- 每个 hart 支持 M/S/U Privilege Mode。
- M-mode 运行 OpenSBI。
- S-mode 可以运行 Linux Domain 或 OP-TEE Domain。
- SoC 使用 APLIC 或 IMSIC 提供硬件中断路由能力。
- OP-TEE 需要访问一个 Secure Crypto Engine。
- Linux 负责网络、PCIe、存储等普通设备。

现在考虑一个问题：

Hart 3 正在执行 Linux，突然 Secure Crypto Engine 完成了一次 AES-GCM 运算，产生一个中断。

这个中断应由谁处理？

一种可能的设计是：

```
                       SoC Hardware
                            |
                     Crypto Engine
                            |
                        HWIRQ #42
                            |
                            v
                     M-mode APLIC
                            |
                      Machine IRQ
                            |
                            v
                 Hart 3: OpenSBI M-mode
                            |
                    Identify HWIRQ 42
                            |
                    Resolve IRQ Owner
                            |
                    OP-TEE Domain
                            |
                 Current Domain = Linux
                            |
                  Domain Context Switch
                            |
                            v
                   OP-TEE S-mode
                            |
                    Virtual IRQ #5
                            |
                       Secure ISR
                            |
                   Clear Crypto Status
                            |
                   Complete Interrupt
                            |
                            v
                    Return to Linux
```

注意：这是 M-mode 介入、由软件实现 Domain-Aware Interrupt Routing 的一种方案，并不是 RISC-V ISA 规定所有中断都必须这样走。

如果硬件已经提供足够可信的中断分区和直接路由机制，Secure Interrupt 也可以被直接投递到指定的安全执行环境，而不必每次都通过 OpenSBI 做软件转发。

我们后面将重点研究的 RP016 Milestone 2/3，采用的正是 M-mode 首先接收 HWIRQ，然后通过 VIRQ 层将中断转发给目标 S-mode Domain 的设计。Milestone 3 已在 QEMU `virt,aia=aplic` 上完成验证，其公开状态仍标记为上游评审中。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE+1



整章最重要的思想是：

> Interrupt 并不仅仅是一个 CPU 异常事件；在具备多个安全 Domain 的系统中，它还涉及设备所有权、硬件访问权限、执行环境切换和安全状态管理。

# 16.1 PLIC、APLIC、IMSIC 在 TEE 平台中的职责

## 16.1.1 首先区分三个概念

理解 RISC-V 中断时，最容易犯的错误，就是把 Interrupt Controller、CPU Interrupt 和 Trap Handler 混成一个概念。

它们实际上是不同层次。

RISC-V Interrupt Delivery Pipeline

Interrupt Source

UART / Network / Crypto Engine / DMA

Interrupt Controller

PLIC / APLIC / IMSIC

捕获、路由、优先级与投递

Hart Interrupt Logic

mip / mie / mideleg / mstatus

决定是否产生 Trap 以及目标特权级

Software Trap Handler

OpenSBI / Linux / OP-TEE

执行 ISR、完成中断、恢复执行

前三层属于硬件架构和中断控制基础设施，最后一层才是 Firmware/OS 软件实现。

对于 TEE 系统，还必须额外引入两个逻辑层：

- Ownership：这个 Interrupt 属于哪个安全实体？
- Domain Routing：目标 Domain 当前没有运行时，如何通知它，或者切换到它？

PLIC、APLIC、IMSIC 本身并不自动提供完整的软件 Security Domain 生命周期管理。

## 16.1.2 PLIC：传统的 Platform-Level Interrupt Controller

PLIC 是传统 RISC-V SoC 中常见的平台级外部中断控制器。

基本结构：

```
UART IRQ ----+
             |
ETH IRQ -----+----> PLIC ----> Hart 0 External IRQ
             |        |
DMA IRQ -----+        +------> Hart 1 External IRQ
             |
GPIO IRQ ----+
```

PLIC 具有以下重要概念：

Interrupt Source

每个外设中断都有一个 Source ID，例如 UART 可能是 ID 10。

Interrupt Target / Context

PLIC 为目标 Hart 的特定特权级执行上下文提供 Interrupt Enable、Threshold 和 Claim/Complete 接口。

Priority

不同中断源可以配置优先级。

Claim/Complete

CPU 收到 External Interrupt 后，还需要通过 PLIC 识别实际产生中断的设备。

一个经典的 S-mode PLIC ISR 可以简化成：

```
void plic_irq_handler(void)
{
    uint32_t id;

    while ((id = plic_claim()) != 0) {
        handle_device_irq(id);
        plic_complete(id);
    }
}
```

这里的 `plic_claim()`、`plic_complete()` 是教学抽象函数，不是 OP-TEE upstream 中的精确 API。

要注意两个不同的编号：

- `scause = Supervisor External Interrupt`，表示中断的 CPU Cause 类型。
- `PLIC Claim = 10`，表示具体的外设 Interrupt Source ID。

所以不能仅凭 `scause` 就知道发生中断的是 UART 还是 DMA。

### PLIC 在 TEE 中的局限

PLIC 可以将外部中断路由给不同的 Hart/Context，但它本身不理解 OpenSBI Supervisor Domain 的语义。

例如：

```
Hart 0: Linux Domain
Hart 1: OP-TEE Domain
```

如果 Crypto Engine 的中断固定投递给 Hart 1，那么 Hart 1 在特定时刻可能正执行其他 Domain。

即使 PLIC 能选择 S-mode Target，也不意味着它能知道当前 S-mode 软件属于哪个 Security Domain。

此外，PLIC 的寄存器访问本身也必须受到系统隔离策略保护，否则不可信 Linux 可能重新配置中断路由、屏蔽安全中断或干扰其他设备。

因此，在需要动态 Domain Switching 的系统里，还要建立更高一层的软件路由与所有权模型。

## 16.1.3 APLIC：Advanced Platform-Level Interrupt Controller

APLIC 是 RISC-V Advanced Interrupt Architecture（AIA）的一部分。

它主要面向 Wired Interrupt。

可以理解为：

> APLIC 负责接收传统的物理中断线，对它们进行配置、委派和路由，然后通过直接模式或 MSI 模式进行投递。

APLIC 支持两种主要投递方式。

Direct Delivery

`text Device    |   IRQ    v  APLIC    |   IDC    |  MEIP/  SEIP    |   Hart       `

经 APLIC 的 IDC 直接向对应 hart 的外部中断输入投递

MSI Delivery

`text Device    |   IRQ    v  APLIC    | MSI Write    |  IMSIC    |   Hart       `

APLIC 将 Wired IRQ 转为 MSI，由 IMSIC 接收

两种模式最重要的区别不是中断能否产生，而是：

谁负责将一个设备中断转换成 CPU 可领取的具体中断 ID？

Direct 模式下，APLIC IDC 提供领取接口；MSI 模式下，IMSIC Interrupt File 记录 pending/enable 状态并提供 Topei 领取机制。

### APLIC Direct Delivery 的关键寄存器

| 寄存器            | 作用                    |
| -------------- | --------------------- |
| `domaincfg`    | Domain 配置，包含投递模式和使能控制 |
| `sourcecfg[i]` | Source i 的触发类型或委派配置   |
| `setienum`     | 根据 Source ID 使能中断     |
| `clrienum`     | 根据 Source ID 禁止中断     |
| `target[i]`    | 中断源的目标 Hart/IDC 与优先级等 |
| `idelivery`    | IDC 中断投递开关            |
| `ithreshold`   | IDC 优先级阈值             |
| `claimi`       | 领取当前可处理的最高优先级中断       |

注意 APLIC Domain 是 Interrupt Controller 自身的硬件分区概念，并不天然等价于 OpenSBI `struct sbi_domain`。二者可以通过固件策略建立映射，但不能直接视作同一对象。

### 一个 UART 示例

假设：

```
UART IRQ = 10
Target Hart = 2
Target Privilege = M-mode
Delivery = Direct
```

硬件过程：

```
UART RX FIFO receives data
          |
          v
APLIC source 10 becomes pending
          |
          v
APLIC routes to Hart 2 IDC
          |
          v
Hart 2 mip.MEIP = 1
          |
          v
Machine External Interrupt Trap
          |
          v
OpenSBI
```

如果 UART 原本属于 Linux Domain，而硬件中断先被路由到 OpenSBI，那么 OpenSBI 后续必须根据路由策略决定如何交付给 Linux。

RP016 Milestone 2 正是以 QEMU UART RX 和 APLIC Direct Delivery 验证这条 M-mode 硬件中断接收路径。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



## 16.1.4 IMSIC：Incoming Message Signaled Interrupt Controller

IMSIC 解决的是另一个问题：

如果设备通过一次内存写操作直接投递中断，CPU 如何保存和领取这些中断？

这就是 MSI（Message Signaled Interrupt）。

对于 MSI-capable 设备：

```
PCIe / DMA Device
        |
        | Write MSI data to target address
        v
     IMSIC
        |
        | Set interrupt pending ID
        v
     Target Hart
```

IMSIC 在 hart 侧提供不同 Privilege Level 的 Interrupt File。

典型配置包括：

```
Hart 0 IMSIC
   |
   +-- M-level Interrupt File
   |
   +-- S-level Interrupt File
   |
   +-- VS-level Guest Interrupt Files
```

其中：

- M-level Interrupt File 服务 M-mode。
- S-level Interrupt File 服务 S-mode。
- Guest Interrupt File 与 H Extension 的虚拟化机制配合。

IMSIC 的重要接口包括：

| 寄存器/机制               | 作用                          |
| -------------------- | --------------------------- |
| `miselect` / `mireg` | 间接访问 M-level Interrupt File |
| `siselect` / `sireg` | 间接访问 S-level Interrupt File |
| `mtopei` / `stopei`  | 领取并确认相应层级的最高优先级外部中断         |
| `eidelivery`         | 控制 Interrupt File 的投递       |
| `eithreshold`        | 设置优先级阈值                     |
| `eip` / `eie`        | Pending / Enable Bitmaps    |

IMSIC 的一个重要特点是：对于其 Interrupt File 内的中断 ID，通常编号越小，优先级越高；ID 0 保留。具体与 APLIC Source Priority 的语义不应混用。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org+1



### 为什么 IMSIC 对 Server SoC 很重要？

因为服务器平台中可能存在大量 MSI Source：

- PCIe NVMe。
- Ethernet NIC。
- DMA Engine。
- 多队列设备。
- 虚拟机直通设备。

IMSIC 支持将 MSI 直接送到指定 hart 和特权级的 Interrupt File，减少传统外部中断控制路径上的集中式竞争。

但需要强调：

MSI 的目标地址和目标 Interrupt ID 必须受到控制。

如果不可信 PCIe 设备能够任意写入 Secure Domain 的 IMSIC Interrupt File，那么它可能伪造安全中断或造成拒绝服务。因此 TEE 平台必须结合 IOMMU、MSI Remapping、SoC Firewall 等机制约束设备中断能力。

这也是为什么设计 Secure Interrupt Routing 时不能只研究 CPU CSR，还必须研究整个 SoC 的总线和设备安全模型。

## 16.1.5 三种 Interrupt Controller 的总结

| 属性              | PLIC       | APLIC              | IMSIC                  |
| --------------- | ---------- | ------------------ | ---------------------- |
| 主要输入            | Wired IRQ  | Wired IRQ          | MSI Write              |
| AIA 组件          | 否          | 是                  | 是                      |
| Direct Delivery | 是          | 是                  | 不适用                    |
| MSI Delivery    | 不直接支持      | 支持转换               | 原生接收                   |
| Source Pending  | PLIC       | APLIC              | Interrupt File         |
| IRQ Claim       | PLIC Claim | IDC Claim          | `mtopei` / `stopei`    |
| 多级控制接口          | Context    | APLIC Domain       | M/S/VS Interrupt Files |
| 核心价值            | 传统外部中断控制   | 灵活路由和 Wired-to-MSI | 扩展性、MSI、虚拟化            |

结论：APLIC 和 IMSIC 不是互相替代的关系。 APLIC 管理 Wired IRQ；IMSIC 管理进入 Hart 的 MSI。二者可以协同工作。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



# 16.2 Interrupt Delegation 与 Domain Isolation 的相互影响

这一节需要解决一个经常被混淆的问题：

Interrupt Delegation 决定由哪个 Privilege Level 接收 Trap；Domain Routing 决定由哪个软件安全环境拥有和处理这个事件。

二者位于不同的抽象层。

## 16.2.1 RISC-V 的中断目标如何确定？

先回顾几个关键 CSR：

| CSR                 | 作用                               |
| ------------------- | -------------------------------- |
| `mip`               | 记录 M-level 可见的 Pending Interrupt |
| `mie`               | 控制各类 Interrupt Enable            |
| `mideleg`           | 控制可委派中断的 Trap 归属                 |
| `mstatus.MIE`       | M-mode 全局中断使能                    |
| `sstatus.SIE`       | S-mode 全局中断使能                    |
| `sip` / `sie`       | S-mode 可见的 Pending / Enable      |
| `mtvec` / `stvec`   | M/S-mode Trap Entry              |
| `mcause` / `scause` | Trap Cause                       |

标准中断 Cause：

| Interrupt                     | Cause Number | 目标     |
| ----------------------------- | ------------ | ------ |
| Supervisor Software Interrupt | 1            | S-mode |
| Machine Software Interrupt    | 3            | M-mode |
| Supervisor Timer Interrupt    | 5            | S-mode |
| Machine Timer Interrupt       | 7            | M-mode |
| Supervisor External Interrupt | 9            | S-mode |
| Machine External Interrupt    | 11           | M-mode |

RV64 的 `mcause` / `scause` 最高位指示 Interrupt 或 Exception，剩余 Cause 字段指示具体类型。

例如：

```
scause = 0x8000000000000009
```

表示 Supervisor External Interrupt。

而：

```
mcause = 0x800000000000000B
```

表示 Machine External Interrupt。

关键：Cause 9/11 表示 CPU 本地接收到的中断类别，并不是具体的 UART/DMA/Crypto IRQ 编号。

## 16.2.2 `mideleg` 究竟控制什么？

`mideleg` 的作用是决定可委派的 Interrupt Trap 是否交给 S-mode 处理。

如果一个中断 Cause 被委派给 S-mode，满足相应条件时就进入 `stvec`，而不是进入 `mtvec`。

但 `mideleg` 不负责：

- 选择 Linux 或 OP-TEE Domain。
- 把外部设备的物理中断线从 M-mode 重定向到 S-mode。
- 配置 APLIC / IMSIC。
- 决定某个设备的安全所有权。

例如，不能仅仅因为希望把 Machine External Interrupt 交给 OP-TEE，就尝试设置 `mideleg[11]`，然后认为 MEIP 自动变成了 SEIP。

在标准架构里，MEIP 和 SEIP 是不同的中断目标类别。如果要让设备直接产生 Supervisor External Interrupt，需要在硬件中断控制层选择适当的 Supervisor Target / Interrupt File。

OpenSBI 的基础 Trap 初始化逻辑通常会设置：

```
/* 根据 OpenSBI sbi_trap.c 简化 */
interrupts = MIP_SSIP | MIP_STIP | MIP_SEIP;

csr_write(CSR_MIDELEG, interrupts);
```

这使 S-mode Software、Timer、External Interrupt 可以按照架构语义在 S-mode 处理。

实际代码还可能考虑 PMU、异常委派和平台能力，不能将上面的片段视为全部初始化逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 16.2.3 为什么 Linux 正在执行时，M-mode 中断仍能抢占它？

这一点需要特别理解。

假设：

```
Current Privilege = S-mode
mip.MEIP = 1
mie.MEIE = 1
```

而目标是 M-mode。

那么，符合其他 Trap 条件时，中断可以进入 M-mode，不要求此时 `mstatus.MIE = 1`。

原因是 RISC-V 的全局中断使能具有特权级相关语义：

- 当前在 M-mode：接收 M-mode 中断需要满足 `mstatus.MIE`。
- 当前在 S-mode：M-mode 属于更高特权级，M-mode 中断不受当前 `mstatus.MIE` 位的全局屏蔽。
- 当前在 M-mode：S-mode 中断不会抢占 M-mode，必须保持 pending，等待返回到适当的低特权级后才可能被处理。

RISC-V Privileged Specification 明确规定了高特权级中断与当前执行特权级之间的关系。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



因此：

```
Linux S-mode
    |
    | MEIP
    v
OpenSBI M-mode
```

即使 Linux 关闭了自己的 S-mode 中断，这条路径也可能成立。

这正是 OpenSBI 能够在 Linux 执行期间响应自己负责的硬件中断的重要机制。

## 16.2.4 Delegation 与 Domain Switching 为什么是两回事？

假设同一个 Hart 上存在：

```
Domain A = Linux
Domain B = OP-TEE
```

二者都运行在 S-mode。

因此，从 CPU 的 Privilege Mode 看：

```
Linux  = S-mode
OP-TEE = S-mode
```

设置 `mideleg.SEI = 1`，只表示适当的 Supervisor External Interrupt 可以被 S-mode 接收。

它并不告诉 CPU：

```
SEIP -> Linux?
SEIP -> OP-TEE?
```

因为硬件 CSR 没有天然的 `current_opensbi_domain` 架构字段。

这意味着，如果使用软件管理多个 Supervisor Domain，单凭 `mideleg` 无法保证 Interrupt 到达正确的 Domain。

我们还需要：

```
HWIRQ
   |
   v
Domain Ownership Policy
   |
   v
Determine Target Domain
   |
   +-- Target == Current
   |       |
   |       v
   |   Notify Domain
   |
   +-- Target != Current
           |
           v
       Domain Switch
           |
           v
       Notify Domain
```

这是 Domain-Aware Interrupt Routing 的核心动机。

## 16.2.5 一个很容易忽略的安全问题

假设 Linux 正在 S-mode 执行，Crypto Engine 的安全中断直接送入 SEIP。

那么：

```
Current Domain = Linux
IRQ Owner      = OP-TEE
Interrupt      = SEIP
```

CPU 会依据当前 S-mode Trap 状态进入对应的 `stvec`，而这个 `stvec` 可能正是 Linux 配置的入口。

即使 Linux 不能读取 Crypto Engine 的受保护寄存器，它也可能接收到一个本应属于安全环境的中断事件。

因此，如果硬件不能实现按 Security Domain 的直接投递，通常需要 M-mode 固件或其他可信中断路由实体介入。

这说明：

> 中断安全隔离不仅是“防止 Linux 访问安全设备寄存器”，还包括“防止安全中断进入错误的执行上下文”。

# 16.3 M-mode / S-mode Interrupt Ownership

现在我们进一步解决：究竟谁拥有一个 Interrupt？

## 16.3.1 Interrupt Ownership 不等于当前 CPU 所在的特权级

假设 UART 的中断由 M-mode OpenSBI 首先接收，但 UART 设备属于 Linux。

那么：

| 项目                | 所属实体                        |
| ----------------- | --------------------------- |
| UART 设备所有权        | Linux                       |
| HWIRQ 初始接收        | OpenSBI                     |
| IRQ 物理配置          | OpenSBI / Platform Firmware |
| ISR 业务处理          | Linux                       |
| IRQ Controller 管理 | 根据平台策略分工                    |

这就是典型的 Physical IRQ Reception 与 Logical IRQ Ownership 分离。

类似地，Crypto Engine 可以属于 OP-TEE，但相关 HWIRQ 由 M-mode 首先接收。

因此建议将一个 Interrupt 的管理至少拆成四项：

1. Physical Source Owner。
2. Interrupt Controller Configuration Owner。
3. Target Security Domain。
4. Interrupt Service Handler。

在工业 SoC 中，它们可以属于不同的软件组件。

## 16.3.2 为什么不能让 Linux 随意配置所有 Interrupt Controller？

因为 Linux 通常属于不可信 REE。

假设 Linux 拥有全部 APLIC Controller 寄存器的写权限。

它可能：

- 禁用 Crypto Engine 的中断源。
- 将 Secure Interrupt 重定向到 Linux Hart。
- 修改 Interrupt Priority。
- 产生大量伪造或无意义的中断。
- 破坏 Secure Domain 的中断服务能力。

这不一定导致直接读取密钥，但可能破坏 TEE 的完整性或可用性。

更严重的是，如果平台在安全域切换时依赖某个中断来恢复执行，不可信 Linux 可以通过重新配置中断路由制造死锁或拒绝服务。

因此，在需要强隔离的架构中，通常应由可信固件维护安全相关的中断路由策略。

一种权限划分是：

| 资源                         | Linux  | OP-TEE  | M-mode Firmware |
| -------------------------- | ------ | ------- | --------------- |
| 普通 UART 中断                 | ISR    | 无       | 必要时路由           |
| Secure Crypto 中断           | 无      | ISR     | 路由与权限保护         |
| 安全控制器配置                    | 禁止     | 受控访问    | 平台策略管理          |
| Secure IRQ Target          | 不可任意修改 | 受限      | 可信配置            |
| Domain IRQ Ownership Table | 不可修改   | 可查询授权部分 | 可信维护            |

不过，也不能机械地认为全部中断都必须经过 M-mode。

如果硬件本身提供受保护的 S-level Interrupt Controller Partition，且配置权限被可靠隔离，则普通设备中断完全可以直接进入对应的 S-mode。

这通常具有更好的性能。

## 16.3.3 三种常见设计模式

### 模式 A：Native S-mode Interrupt

```
Network Device
      |
      v
   APLIC S
      |
      v
 Linux S-mode
```

特点：不需要 M-mode 软件中转，开销较低。

适合由硬件隔离保证路由正确的普通设备。

### 模式 B：M-mode Mediated Interrupt

```
Crypto Engine
      |
      v
   APLIC M
      |
      v
 OpenSBI
      |
  VIRQ Routing
      |
      v
 OP-TEE S-mode
```

特点：由可信固件决定目标 Domain，便于实施 Domain-Aware Routing，但增加软件中断转发与切换成本。

### 模式 C：Dedicated Secure Hart

```
Crypto Engine
      |
      v
 Secure Hart
      |
      v
 Secure Firmware / TEE
```

特点：安全业务由专用 hart 执行，不必在同一 Application Hart 上频繁切换 Linux 和 OP-TEE。

但需要解决与 Application CPU 的 IPC、共享内存、设备安全和中断管理。

Staff Engineer 的设计能力，体现在能基于具体 SoC 的安全能力、性能目标和成本选择模式，而不是机械地认为其中一种永远最好。

# 16.4 外部中断到达时如何确定目标 Domain？

这一节是本章的核心内容。

我们先定义一个具体的 SoC 场景。

```
              Application Harts
              Hart 0 / 1 / 2 / 3
                       |
               OpenSBI M-mode
                       |
              Domain Manager
                       |
              +--------+--------+
              |                 |
          Linux Domain      OP-TEE Domain
              |                 |
         UART / NIC         Secure Crypto
              |                 |
              +--------+--------+
                       |
                APLIC HWIRQ
```

假设：

```
UART        HWIRQ 10 -> Linux Domain
Crypto      HWIRQ 42 -> OP-TEE Domain
Security    HWIRQ 57 -> M-mode Firmware
```

## 16.4.1 Domain Routing 需要维护哪些信息？

最小的路由表可以抽象成：

```
/* 教学伪代码：不是 OpenSBI 原始定义 */

struct irq_route {
    uint32_t chip_uid;
    uint32_t hwirq;
    uint32_t target_domain_id;
    uint32_t target_hart_policy;
    uint32_t security_class;
};
```

其中：

- `chip_uid`：中断控制器实例。
- `hwirq`：该控制器内的中断源编号。
- `target_domain_id`：安全域所有者。
- `target_hart_policy`：可在哪个 hart 上交付。
- `security_class`：该中断是否属于安全关键资源。

为什么要同时使用 `chip_uid` 和 `hwirq`？

因为大型 SoC 可能存在多个 Interrupt Controller，两个控制器都可能使用 HWIRQ 42。

所以：

```
(APLIC_A, IRQ 42) != (APLIC_B, IRQ 42)
```

仅凭 `42` 无法全局唯一定位中断源。

## 16.4.2 从 HWIRQ 到 Domain 的处理流程

以 Crypto Engine IRQ 42 为例：

```
1. Crypto Engine asserts IRQ 42

2. APLIC delivers MEIP

3. Hart enters M-mode

4. OpenSBI claims HWIRQ 42

5. Lookup route:
      chip = APLIC_0
      hwirq = 42
      owner = OP-TEE

6. Check current domain:
      current = Linux
      target  = OP-TEE

7. Queue interrupt for OP-TEE

8. Suspend Linux domain context

9. Switch to OP-TEE domain context

10. Notify OP-TEE via virtual interrupt

11. OP-TEE ISR processes Crypto completion

12. Complete HWIRQ

13. Restore Linux domain context
```

这个流程在架构上有两个关键判断：

第一：IRQ 属于谁？

这应当由可信的 Ownership Policy 决定，而不是由正在运行的 Linux 自行声明。

第二：目标 Domain 是否能够立即执行？

如果目标 Domain 当前没有可用的 hart，必须决定是：

- 在某个 hart 上切换 Domain。
- 将 VIRQ 暂存在 Pending Queue 中。
- 通过跨 hart 通知唤醒目标执行环境。
- 先保持 HWIRQ Masked，直到允许服务。

这涉及调度与服务延迟，而不仅是硬件 IRQ Routing。

## 16.4.3 RP016 Milestone 3 的实际设计

RP016 M3 提供了一个很有价值的参考实现。

其核心机制包括：

- HWIRQ → Domain Routing Rules。
- HWIRQ → VIRQ Mapping。
- Per-(Domain, Hart) Pending Queue。
- Domain Context Switching。
- SEIP Notification。
- VIRQ POP / COMPLETE SBI ECALL。

其文档中的 Device Tree 路由配置形式为：

```
/* RP016 M3 文档中的示例结构，省略上下文 */

rpmi_sysirq_intc: interrupt-controller {
    compatible = "opensbi,mpxy-sysirq";
    interrupt-controller;
    #interrupt-cells = <1>;

    interrupts-extended = <&aplic 10 IRQ_TYPE_LEVEL_HIGH>;

    opensbi,mpxy-channel-id = <4>;
    opensbi,domain = <&domain1>;
};
```

示意意义：

```
APLIC HWIRQ 10
      |
      v
MPXY Channel 4
      |
      v
Domain 1
```

这是一种由 Device Tree 提供静态 Routing Policy 的方法。

RP016 M3 的实现还规定：如果没有匹配的路由规则，向 Root Domain 路由，作为兼容性回退行为。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



这里必须做一个安全工程判断：

兼容性回退不等于最严格的安全回退。

对于 Secure IRQ，我更建议采用：

```
Unknown Secure IRQ
       |
       v
  Mask / Quarantine
       |
       v
 Security Audit
```

而不是无条件交给 Root Domain。

因为 Root Domain 未必是可信安全域。在实际产品中，应该根据设备的安全属性确定 Fail-Closed 行为。

## 16.4.4 Domain Routing 的安全不变量

一个可靠的设计必须保证：

\\[ \operatorname{Owner}(IRQ)=D \Rightarrow \operatorname{Delivery}(IRQ)\subseteq D \\]

这里的含义是：一个中断应该只被投递给被授权处理它的安全 Domain。

但是需要区分两个事实：

- 可信 M-mode Firmware 可以接收物理中断并转发。
- 最终处理设备业务的 S-mode Domain 必须受到 Ownership Policy 的约束。

因此更准确地说：

> 除了可信的路由与管理实体，中断事件不得被交付给未经授权的软件执行环境。

这个原则也要延伸到 IRQ 相关 MMIO、MSI 地址、Pending Queue 和 Completion API。

# 16.5 HWIRQ / VIRQ Mapping 与 Interrupt Routing

在多 Domain 架构中，VIRQ 是非常重要的抽象。

## 16.5.1 为什么不直接向 OP-TEE 暴露物理 HWIRQ？

假设 OP-TEE 需要处理一个 Crypto Engine 中断。

如果直接向 OP-TEE 暴露：

```
APLIC 0, HWIRQ 42
```

OP-TEE 可能需要知道：

- 具体 APLIC 设备实例。
- HWIRQ 编号。
- 中断触发类型。
- APLIC Register Layout。
- Interrupt Target。
- IRQ Enable/Disable 方法。

这会把 OP-TEE 的核心逻辑与底层硬件耦合起来。

如果使用 VIRQ：

```
Crypto Completion -> VIRQ 5
```

OP-TEE 只需要知道：

```
VIRQ 5 represents Crypto Completion
```

物理路由、Controller ID 和硬件屏蔽策略仍可由可信固件管理。

这与 Hypervisor 为 Guest OS 提供 Virtual Interrupt 的设计理念相似，但不意味着这里一定使用了 RISC-V H Extension。

RP016 的 VIRQ 是一种软件 Paravirtual Interrupt Abstraction。

## 16.5.2 三个不同层级的中断编号

务必区分：

| 编号类型                | 示例              | 代表什么                       |
| ------------------- | --------------- | -------------------------- |
| CPU Interrupt Cause | `mcause = 11`   | Machine External Interrupt |
| HWIRQ               | APLIC Source 42 | 物理中断控制器中的 Source           |
| VIRQ                | VIRQ 5          | 特定逻辑通道内的软件中断身份             |

因此：

```
MEIP
  |
  v
Claim APLIC HWIRQ 42
  |
  v
Map to OP-TEE VIRQ 5
```

不是简单的 11 → 42 → 5 数字转换，而是三个不同抽象层上的解析过程。

## 16.5.3 VIRQ Mapping 的数据结构

RP016 M3 的公开设计中，Mapping 以 MPXY Channel 为单位管理，并支持双向查找。

可抽象为：

```
struct irq_endpoint {
    uint32_t chip_uid;
    uint32_t hwirq;
};

struct virq_mapping {
    uint32_t virq;
    struct irq_endpoint endpoint;
};
```

正向查找：

```
(chip_uid, HWIRQ) -> VIRQ
```

反向查找：

```
VIRQ -> (chip_uid, HWIRQ)
```

正向查找用于投递，反向查找则可以用于完成中断时定位物理资源。

在 RP016 的实际设计中，Mapping 采用 Bitmap 分配 VIRQ，并使用动态结构进行正向和反向查询。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### 为什么 Mapping 应相对稳定？

假设：

```
VIRQ 5 -> Crypto IRQ 42
```

如果运行期间未经协调地更改成：

```
VIRQ 5 -> UART IRQ 10
```

那么 OP-TEE 可能执行错误的 ISR，或者对错误的设备发出 Completion。

因此，动态 Mapping 更新必须具备同步、权限检查和生命周期保证。

对安全设备，优先考虑启动时固定分配和可信初始化。

## 16.5.4 Per-(Domain, Hart) Pending Queue

这是 M3 设计中的核心数据结构。

假设：

```
OP-TEE Domain
    |
    +-- Hart 0 Queue: [VIRQ 5, VIRQ 7]
    |
    +-- Hart 1 Queue: [VIRQ 3]
    |
    +-- Hart 2 Queue: []
```

为什么不使用一个全局队列？

因为：

1. 中断可能被定向到指定 hart。
2. 每个 hart 的当前 Domain Context 不同。
3. Per-Hart Queue 能减少部分竞争。
4. 能追踪某一 Domain 在某一 Hart 上尚未处理的中断。
5. 便于与 Hart-local SEIP Pending 状态关联。

RP016 提供的状态结构可概括为：

```
/* 根据 RP016 M3 公开结构摘取关键成员 */

struct sbi_domain_virq_state {
    spinlock_t lock;

    u32 head;
    u32 tail;

    struct {
        u32 virq;
        u32 channel_id;
        struct sbi_irqchip_device *chip;
    } q[VIRQ_QSIZE];

    bool return_to_prev;
};
```

其中：

- `lock` 保护队列并发访问。
- `head/tail` 表示 Ring Buffer 状态。
- `channel_id` 区分 MPXY Channel。
- `chip` 提供完成物理中断时使用的控制器上下文。
- `return_to_prev` 表示处理中断后可能需要恢复之前的 Domain。

  [image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

  RISC-V OP-TEE



但这个设计还有值得进一步研究的工程问题：

如果多个 HWIRQ 同时到来，队列满了怎么办？

如果同一个 VIRQ 在处理完成之前又触发了怎么办？

如果 Hart 正在执行 Domain Switch，另一个 Hart 修改了该 Domain 的中断状态怎么办？

这些问题不能简单依靠 Spinlock 解决，需要定义完整的 VIRQ Lifecycle 和状态机。

我们下一节将从中断抢占、Yield 和 Context Switching 继续分析。

# 16.6 Foreign Interrupt 与 OP-TEE Yield

这一节我们将建立 Arm OP-TEE 和 RISC-V OP-TEE 之间的对应关系。

对于有 Arm TrustZone 开发经验的工程师，最重要的是理解：

Native Interrupt / Foreign Interrupt 是 OP-TEE 从软件所有权角度定义的分类，而不是 RISC-V ISA 的两种原生 Interrupt 类型。

## 16.6.1 Native Interrupt 和 Foreign Interrupt

OP-TEE 官方架构文档区分两种中断：

| 类型                | 含义                | 处理方式             |
| ----------------- | ----------------- | ---------------- |
| Native Interrupt  | OP-TEE 自己拥有和处理的中断 | OP-TEE 执行对应的 ISR |
| Foreign Interrupt | 不归 OP-TEE 处理的中断   | 转发给有权处理它的软件环境    |

在经典 Arm OP-TEE 架构中，Secure Interrupt 往往是 Native Interrupt，而 Normal World Interrupt 往往是 Foreign Interrupt。

例如：

```
OP-TEE is executing TA
        |
        +-- Secure Timer IRQ
        |       |
        |       v
        |   Native Interrupt
        |       |
        |       v
        |   OP-TEE handles
        |
        +-- Linux NIC IRQ
                |
                v
           Foreign Interrupt
                |
                v
           Yield to REE
```

两者的区别不仅在于 ISR 放在哪里。

更重要的是 OP-TEE 是否需要保存当前 Trusted Thread，然后让其他 Domain 运行。

OP-TEE 官方文档中，Foreign Interrupt 可以使 Trusted Thread Suspend，并通过 Monitor 将执行权交还给 Normal World；后续再恢复该线程。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 16.6.2 为什么 Foreign Interrupt 会涉及 Trusted Thread？

假设一次 TA Invoke 正在执行：

```
Linux CA
   |
   | InvokeCommand()
   v
OpenSBI
   |
   v
OP-TEE
   |
   v
Trusted Thread 3
   |
   v
TA AES-GCM Operation
```

此时 NIC 收到一个网络数据包。

由于 NIC 归 Linux 管理，OP-TEE 不应该直接执行 Linux NIC Driver 的 ISR。

所以可能出现：

```
TA executing
    |
    | Foreign IRQ
    v
Suspend Trusted Thread 3
    |
Save OP-TEE execution context
    |
Yield to Linux Domain
    |
Linux handles NIC IRQ
    |
Linux resumes TEE request
    |
Restore Trusted Thread 3
    |
Continue AES-GCM
```

关键问题是：

Trusted Thread 已经进入 TA 的函数内部，为什么能够中断后再继续执行？

因为 OP-TEE 保存的是可恢复的执行上下文，而不只是保存一个 TA Session ID。

例如可能需要保存：

```
GPRs
SP
PC / sepc
sstatus
User TA execution state
Relevant FP / Vector context
Kernel thread state
```

其中哪些状态由 S-mode Trap Entry 保存、哪些由 Trusted Thread 保存、哪些由 OpenSBI Domain Context 保存，需要根据具体架构路径区分，避免重复保存或遗漏。

特别注意三个概念：

| Context                       | 作用                                   |
| ----------------------------- | ------------------------------------ |
| OpenSBI Domain Context        | 在 Linux 和 OP-TEE 等 S-mode Domain 间切换 |
| OP-TEE Trusted Thread Context | 保存 OP-TEE 内部可暂停的执行流程                 |
| User TA Context               | 保存 TA 的用户态执行和地址空间状态                  |

三者不能混为一谈。

## 16.6.3 Arm 与 RISC-V 的 Foreign Interrupt 路径对照

Arm TrustZone：

```
Foreign IRQ
     |
     v
OP-TEE Interrupt Entry
     |
Suspend Trusted Thread
     |
Return through Secure Monitor
     |
Linux IRQ Handler
     |
Resume SMC
     |
Restore Trusted Thread
```

RISC-V Domain-based TEE 可以采用：

```
Foreign HWIRQ
     |
     v
OpenSBI M-mode
     |
Identify IRQ Owner = Linux
     |
Current Domain = OP-TEE
     |
Save / Suspend secure execution
     |
Switch to Linux Domain
     |
Deliver Linux VIRQ
     |
Linux ISR
     |
Resume / Return to OP-TEE
```

但这不是说 OpenSBI 的一次 `sbi_domain_context` 切换就自动实现了完整的 OP-TEE Foreign Interrupt 语义。

还需要正确处理：

- OP-TEE 当前是否正在执行 Trusted Thread。
- 当前线程是否可以 Yield。
- OP-TEE 是否处于 Core Critical Section。
- 是否存在尚未完成的 TA Syscall。
- Resume 时如何找到正确的 Trusted Thread。
- 如何确认 Resume Request 属于先前被挂起的调用。

这也是为什么 Foreign Interrupt 不能只靠一个 `switch_domain()` 完成。

## 16.6.4 Yield 的本质是什么？

Yield 可以理解为：

> 当前可信执行环境暂时放弃 hart 的执行权，同时保留将来恢复执行所需的受保护状态。

但 Yield 不等价于传统 `schedule()`。

一个 RTOS 的 `schedule()` 通常在同一个 OS 内选择下一个 Task。

而 OP-TEE Yield 可能意味着：

```
OP-TEE S-mode
     |
     v
OpenSBI M-mode
     |
     v
Linux S-mode
```

它跨越了 Security Domain。

所以其安全约束更加严格。

例如：

OP-TEE Yield 之前，是否必须先释放 Spinlock？

如果锁由 OP-TEE Core 中的其他执行流程持有，Yield 后又需要该锁才能恢复，就可能产生 Deadlock。

如果 OP-TEE 处于 MMU Page Table 更新的中间状态，是否允许 Yield？

如果中间状态会暴露不一致的权限配置或破坏恢复路径，就不能直接切换。

如果 Yield 后 Linux 不再调用 Resume，怎么办？

Trusted Thread 资源可能长期被占用，因此需要定义取消、超时、资源上限和清理策略。安全体系不能依赖不可信 Linux 一定会及时恢复请求。

## 16.6.5 RP016 与 RP022 的重要演进

这里需要特别区分项目阶段。

RP016 Milestone 3 使用私有 VIRQ SBI Extension 提供 `POP` 和 `COMPLETE`，但 OP-TEE 专用 Yield ECALL 并不在 M3 交付范围内。

RP022 Milestone 1 则计划：

1. 通过 SBI MPXY 访问标准化的 RPMI `SYSTEM_IRQ` 服务。
2. 使用 `SYSIRQ_CLAIM` 取得待处理的虚拟中断。
3. 提供适合 OP-TEE 的 Yield ECALL。
4. 在完成 VIRQ 处理后，根据需要返回先前的 Domain。

截至本章核对资料时，RP022 Milestone 1 的公开状态仍为 In Progress。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



因此，你研究源码时应明确：

- RP016 M3：可验证的 VIRQ Courier 原型。
- RP022 M1：标准化 SYSIRQ + Yield 的演进目标。
- Arm OP-TEE Foreign Interrupt：已经成熟的执行语义，可用于对照，但不能直接复制其 SMC/Monitor 实现。

# 16.7 Interrupt-Driven Domain Context Switching

这是整章最接近 OpenSBI Kernel Architecture 的部分。

我们现在从汇编与 CSR 的角度深入一次中断驱动的 Domain Switch。

## 16.7.1 一次完整的 Domain Switch

假设：

```
Hart 2
Current Domain = Linux
Target Domain  = OP-TEE

APLIC HWIRQ 42 belongs to OP-TEE
```

初始状态：

```
Privilege = S-mode
PC        = Linux kernel instruction
satp      = Linux page table
stvec     = Linux trap entry
sscratch  = Linux hart-local state
```

发生 Machine External Interrupt 后：

```
Hardware:
    mepc          <- interrupted PC
    mstatus.MPP   <- S-mode
    mstatus.MPIE  <- previous MIE
    mstatus.MIE   <- 0
    PC            <- mtvec
```

这些是进入 M-mode Trap 时的典型架构状态变化。硬件不会替我们自动保存全部通用寄存器，也不会直接保存一个完整的 OpenSBI Domain Context。

后续 M-mode Trap Assembly 需要保存必要的 GPR 等状态，然后进入 C Trap Handler。

执行流程可以分为四个阶段。

### 阶段 A：Trap Entry

```
Linux S-mode
    |
    | Machine External IRQ
    v
mtvec
    |
    v
OpenSBI Trap Assembly
    |
    | Save GPRs / establish trap context
    v
sbi_trap_handler()
```

当前 upstream OpenSBI 中，`sbi_trap_handler()` 位于：

`lib/sbi/sbi_trap.c`

它会识别 Interrupt Cause，并通过 AIA 或非 AIA 路径进入中断处理逻辑。现有代码包括 `sbi_trap_aia_irq()`、`sbi_trap_nonaia_irq()` 和 `sbi_irqchip_process()` 等入口。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 阶段 B：IRQ Resolution

RP016 M2/M3 参考架构：

```
sbi_trap_handler()
    |
    v
External IRQ Handling
    |
    v
IRQCHIP
    |
    v
APLIC Claim
    |
    v
VIRQ Courier
    |
    +-- Resolve IRQ Owner
    |
    +-- Determine Target Domain
    |
    +-- Enqueue VIRQ
```

这里需要注意，OpenSBI 主线的 IRQCHIP 抽象不等于 RP016 开发分支中的全部 VIRQ Courier 功能。

阅读时应使用匹配的源码版本。

### 阶段 C：Domain Context Switch

假设目标为 OP-TEE。

需要保存 Linux Domain 相关的执行状态，并恢复 OP-TEE Domain 的状态。

可以抽象成：

```
/* 教学伪代码 */

save_domain_context(linux_ctx);

apply_hardware_isolation(optee_domain);

restore_domain_context(optee_ctx);

return_to_supervisor();
```

真实实现远比这四行复杂。

在 upstream OpenSBI 的：

`lib/sbi/sbi_domain_context.c`

中，可以看到 `struct hart_context` 保存一组 Supervisor CSR，包括：

```
sstatus
sie
stvec
sscratch
sepc
scause
stval
sip
satp
```

以及 Trap Context 和其他架构状态。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 为什么需要保存 `stvec`？

因为 Linux 和 OP-TEE 是两个不同的 S-mode 软件实体。

```
Linux stvec  -> Linux trap handler
OP-TEE stvec -> OP-TEE trap handler
```

如果切换到 OP-TEE 后仍保留 Linux 的 `stvec`，那么 OP-TEE 运行期间发生 Supervisor Exception 时，可能进入 Linux 的 Trap Handler。

不仅会导致系统崩溃，还可能破坏 Security Domain 的隔离。

### 为什么需要保存 `satp`？

因为 Linux 和 OP-TEE 通常具有不同的地址空间。

```
Linux:
    satp -> Linux Page Tables

OP-TEE:
    satp -> OP-TEE Page Tables
```

如果将 OP-TEE 的 PC 恢复了，却仍使用 Linux 的 `satp`，那么同一个虚拟地址可能翻译到完全不同的物理地址。

因此 Domain Context Switch 不仅要保存 `sepc`、`sp`，还必须处理地址空间和权限边界。

### 为什么需要处理 `sie` 和 `sip`？

因为不同 Domain 的 Interrupt Enable / Pending 状态可能不同。

例如：

```
Linux Domain:
    SEIE = 1
    STIE = 1

OP-TEE Domain:
    SEIE = 1
    STIE = platform-defined
```

如果不处理这些状态，可能出现：

- OP-TEE 意外接收 Linux 的中断。
- Linux 恢复后丢失本应处理的中断。
- 被屏蔽的中断在错误 Domain 中重新使能。
- Domain Switch 后发生意外 Trap。

尤其要注意，部分 Pending Bit 由硬件或外部中断控制器驱动，并不能假设软件对 `sip` 的整体写入就能完全保存和恢复物理中断状态。

必须结合具体 CSR 的可写属性、AIA/IMSIC 状态以及 VIRQ 队列进行设计。

### 阶段 D：Resume OP-TEE

```
M-mode Context Switch
        |
        v
Restore OP-TEE sstatus / sie / satp / stvec
        |
        v
Restore GPRs / return context
        |
        v
mret
        |
        v
OP-TEE S-mode
        |
        v
Pending SEIP
        |
        v
OP-TEE Trap Handler
```

这里有一个容易忽略的细节：

OpenSBI 可以先切换到 OP-TEE 的 Supervisor Context，再通过 SEIP 通知 OP-TEE 取走 VIRQ。

不必把 HWIRQ 编号强行编码到 `scause` 中。

在这种设计下，`scause` 依旧报告 Supervisor External Interrupt，具体 VIRQ 则通过协议或队列 API 获取。

## 16.7.2 Per-Hart Domain Context 为什么重要？

假设：

```
Hart 0: Linux
Hart 1: OP-TEE
Hart 2: Linux
Hart 3: OP-TEE
```

每个 Hart 都有独立的：

- 当前 PC 和寄存器状态。
- M-mode Trap Stack。
- `satp`。
- `stvec`。
- `mstatus` / `sstatus`。
- Interrupt Enable/Pending 状态。

所以不能仅为 OP-TEE Domain 建立一个全局的 `saved_context`。

正确的上下文模型通常具有：

```
Domain
    |
    +-- Hart 0 Context
    +-- Hart 1 Context
    +-- Hart 2 Context
    +-- Hart 3 Context
```

其设计本质是：

\\[ Context = f(Domain, Hart) \\]

这并不意味着同一个 Domain 的所有状态都必须是 per-hart。Domain 的资源描述、权限策略和共享服务可以是全局的，但可恢复的 CPU 执行上下文必须遵循相应的 hart/线程归属规则。

## 16.7.3 切换顺序决定安全性

假设我们从 Linux 切到 OP-TEE。

错误设计可能是：

```
1. Restore OP-TEE PC / registers
2. Begin OP-TEE execution
3. Configure PMP / WorldGuard
```

如果第 2 步已经进入安全执行环境，但第 3 步的隔离配置尚未完成，就形成了危险窗口。

反过来，若在仍需要访问 Linux Context 保存区域时过早收紧权限，也可能导致 M-mode 固件或恢复路径访问失败。

因此要设计一个一致的切换序列：

```
Save outgoing execution state
        |
        v
Establish protected transition state
        |
        v
Apply target isolation policy
        |
        v
Apply address-space / translation synchronization
        |
        v
Restore target execution state
        |
        v
Validate target entry conditions
        |
        v
Return to target Domain
```

这不是一个固定适用于所有 SoC 的机器指令顺序。具体顺序取决于 PMP、WorldGuard、Cache、MMU 和 Firmware Data 所在的权限域。

但必须满足一个不变量：

> 任何不可信 S-mode 指令开始执行之前，硬件权限配置必须与该 Domain 的授权状态一致。

对于 Security Domain Switching，正确的状态变更顺序本身就是安全架构的一部分。

## 16.7.4 从 M-mode 中断到 Domain Switch 的时序图

```
Device        APLIC          OpenSBI         OP-TEE
   |             |               |               |
   |--- IRQ ---->|               |               |
   |             |--- MEIP ----->|               |
   |             |               |               |
   |             |      Claim HWIRQ 42           |
   |             |               |               |
   |             |      Lookup Owner = TEE       |
   |             |               |               |
   |             |      Mask Source              |
   |             |               |               |
   |             |      Enqueue VIRQ 5           |
   |             |               |               |
   |             |      Save Linux Context       |
   |             |               |               |
   |             |      Restore TEE Context      |
   |             |               |               |
   |             |               |--- SEIP ------>|
   |             |               |               |
   |             |               |    POP VIRQ   |
   |             |               |<--- ECALL ----|
   |             |               |--- VIRQ 5 --->|
   |             |               |               |
   |             |               |    TEE ISR    |
   |<----------- Clear Device Status -----------|
   |             |               |               |
   |             |               |<-- COMPLETE --|
   |             |      Unmask / EOI             |
   |             |               |               |
   |             |      Restore Linux Context    |
```

这个时序与 RP016 M3 的 UART RX 验证流程一致，体现了 `assert → claim → map → route → enqueue → notify → handle → complete` 的完整生命周期。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



其中对 OP-TEE 的适配、Resume ABI 及生产级安全处理还需要根据 RP022 后续实现进一步验证。

# 16.8 Interrupt Priority、Masking、Pending 与 Completion

这一节主要研究中断的生命周期正确性。

一个中断能够进入 CPU，并不代表它被正确地完成。

尤其对于 Level-triggered Interrupt，错误的处理顺序会引发 Interrupt Storm。

## 16.8.1 Interrupt Lifecycle 的四种状态

我们可以从软件处理角度定义：

```
DISABLED
    |
    | Enable source
    v
ARMED
    |
    | Device asserts interrupt
    v
PENDING
    |
    | Claim / Deliver
    v
IN_SERVICE
    |
    | Device handled + Completion
    v
ARMED
```

这只是一个简化逻辑状态机。

实际硬件可能同时存在 Pending、Enabled、Active、Claimed 等不同状态，不同控制器的语义并不完全相同。

例如，一个物理中断被 Masked 后，其硬件 Pending 状态可能仍然保留。

因此：

Masked 不等于 Pending 已被清除。

## 16.8.2 Level-triggered 与 Edge-triggered 的根本区别

### Level-triggered

设备通过持续保持电平来表示需要服务。

例如 UART RX FIFO 中存在未读数据。

```
RX FIFO not empty
        |
        v
IRQ line = HIGH
        |
        v
Interrupt asserted
```

如果 ISR 没有读取 FIFO 中的数据：

```
IRQ line remains HIGH
```

即使软件执行 Completion，之后中断仍可能再次触发。

因此处理 Level-triggered Interrupt 时，通常要：

1. Mask 必要的中断投递。
2. 识别设备和中断状态。
3. 清除或处理设备端的触发条件。
4. 完成中断控制器需要的领取/完成流程。
5. 根据控制器要求重新使能投递。

具体的 EOI、Unmask 顺序必须遵循控制器规范，不能机械地套用到所有硬件上。

### Edge-triggered

设备通过电平变化表示一个事件。

例如：

```
LOW -> HIGH
```

可能触发一次中断。

重要风险是：

如果中断边沿发生时 Interrupt Controller 尚未做好接收准备，是否会记录 Pending？

这取决于具体硬件的 Edge Capture 和 Pending 语义。

所以不能简单地认为对所有 Edge IRQ 执行 Mask → Unmask 都不会丢失事件。

对于需要精确计数的硬件事件，应该优先使用设备 FIFO、完成队列、计数寄存器等持久状态，而不是假设一个 Pending Bit 能记录无限次事件。

## 16.8.3 PLIC、APLIC、IMSIC 的 Completion 语义不同

### PLIC

PLIC 存在明确的 Claim/Complete 协议。

典型流程：

```
Claim ID
   |
Handle device
   |
Write ID to Completion register
```

Completion 告诉 PLIC 对应 Gateway：该中断已被软件处理，可以重新接受该 Source 的后续中断请求。

### APLIC Direct Mode

APLIC Direct Mode 的 `claimi` 可以用来领取中断。

在这种模式中，不应简单假设它具有和 PLIC 完全相同的 Write-Complete 接口。

通常需要根据 APLIC 的 Pending、Enable、Claim 和 Device Level Deassertion 行为完成整个中断生命周期。

对于 RP016 M2 的 QEMU APLIC Direct Mode 测试，公开文档指出：读取 `IDC.CLAIMI` 用于领取，设备端清除触发条件即可完成相应的物理处理流程，不要求对 `CLAIMI` 再进行写入完成操作。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



还值得注意，RP016 M2 文档的 `mask()/unmask()` 一段存在明显的文字反置：

- `SETIENUM` 的硬件语义是使能指定 Source。
- `CLRIENUM` 的硬件语义是禁止指定 Source。

实际移植时应以 APLIC Specification 和实现代码为准，不能仅复制文档中被反向标注的函数语义。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### IMSIC

IMSIC 通过 Interrupt File 维护 Pending 和 Enable Bitmaps。

`mtopei` / `stopei` 提供识别最高优先级待处理外部中断的机制。

读取/写入 Topei 的操作具备对应的 Claim/Acknowledge 语义，但设备产生 MSI 的方式与 PLIC Wired IRQ 并不相同。特别是 MSI Pending Bit 不能被视为可靠的事件计数器。

## 16.8.4 Priority 分为多个层次

不要把 Interrupt Priority 当成单一数字。

至少存在四个不同层次：

| 层次                  | 示例                         | 决定什么               |
| ------------------- | -------------------------- | ------------------ |
| Privilege Priority  | M-mode 高于 S-mode           | 何种 Trap 可以抢占当前执行环境 |
| CPU Local Priority  | `mtopi` / AIA              | 当前 hart 选择哪一类中断    |
| Controller Priority | APLIC / PLIC / IMSIC       | 同一类外部中断先交付哪个 ID    |
| Software Priority   | VIRQ Queue / Domain Policy | 软件先处理哪个逻辑事件        |

RP016 M3 原型采用 Per-(Domain, Hart) FIFO Queue，且将任意 Per-Hart IRQ Priority 管理列为非目标。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



这意味着：

即使某个 HWIRQ 在 APLIC 中具有较高优先级，也不能保证其进入软件 VIRQ Queue 后仍然获得相同的服务顺序。

因此，设计具备实时性要求的 Secure Interrupt 时，必须同时评估：

- Hardware Routing Latency。
- M-mode Trap Latency。
- Domain Switch Latency。
- Queue Waiting Time。
- OP-TEE ISR Execution Time。

否则只能说某个中断在硬件控制器中优先级较高，却无法证明它的端到端响应时间满足要求。

## 16.8.5 中断处理中的关键竞态

考虑一个 Level-triggered Crypto IRQ。

```
Time 0: Crypto IRQ asserted
Time 1: OpenSBI claims IRQ
Time 2: OpenSBI masks source
Time 3: VIRQ enqueued
Time 4: OP-TEE handles completion
Time 5: Crypto status cleared
Time 6: OpenSBI unmasks source
```

如果 Time 5 和 Time 6 的顺序被反转：

```
Unmask source
    |
    v
Crypto IRQ still asserted
    |
    v
New interrupt delivered immediately
```

这可能造成重复中断。

但另一个问题同样重要：

如果 Time 5 之后、Time 6 之前 Crypto Engine 又完成了一次新的请求，第二次完成事件是否会丢失？

答案取决于设备的完成队列、状态寄存器、Pending Latched Behavior 和中断控制器语义。

因此，正确设计一般要求：

ISR 不仅清除 IRQ，还要以设备定义的协议消费已经完成的工作，并确保新的工作不会被静默丢弃。

对于高并发设备，Completion Queue 比单纯依靠一个 IRQ Pending Bit 更可靠。

# 16.9 Interrupt Storm、Lost Interrupt 与错误恢复

前面讨论的都是正确运行路径。但对于真正的工业项目，特别是服务器 SoC，异常路径往往比正常路径更重要。

这里我们将重点研究四类问题：

- Interrupt Storm：中断不停触发。
- Lost Interrupt：设备已经产生事件，但软件没有收到或处理。
- Stuck Interrupt：中断长期处于 Pending / In-Service 状态。
- Incorrect Routing：中断被投递到错误 Hart 或错误 Domain。

## 16.9.1 Interrupt Storm：为什么 CPU 一直陷入中断？

假设 Crypto Engine 产生 Level-triggered Interrupt。

OP-TEE 的 ISR 执行：

```
/* 错误示例：没有确认设备端状态 */

void crypto_irq_handler(void)
{
    crypto_read_result();

    /* Missing device interrupt status clear */
}
```

Crypto Engine 仍然保持 IRQ Line Asserted。

于是：

```
Crypto IRQ asserted
       |
       v
OpenSBI handles
       |
       v
OP-TEE handles
       |
       v
Return
       |
       v
IRQ still asserted
       |
       v
OpenSBI handles again
       |
      ...
```

最终可能出现：

```
CPU utilization = 100%
Interrupt rate  = extremely high
Linux progress  = near zero
```

如果每次中断都需要：

```
Linux -> OpenSBI -> OP-TEE -> OpenSBI -> Linux
```

那么 Storm 的成本还会叠加 Domain Context Switch。

### 如何判断是设备问题还是中断控制器问题？

不要一开始就反复修改 ISR。

应该按下面的顺序定位：

| 检查项               | 要回答的问题                      |
| ----------------- | --------------------------- |
| Device IRQ Status | 设备是否仍认为有待处理事件？              |
| APLIC Pending     | 中断控制器是否仍记录该 Source Pending？ |
| APLIC Enable      | 是否已经正确 Mask / Unmask？       |
| CPU `mip`         | CPU 是否仍观察到 MEIP？            |
| VIRQ Queue        | 是否重复入队相同逻辑事件？               |
| Completion        | 物理和虚拟中断是否都完成？               |
| Domain Context    | 是否在错误的 Domain 重复响应？         |

要特别注意：不同层次的 Pending 不是同一件事。

```
Device Status
     !=
APLIC Pending
     !=
CPU mip.MEIP
     !=
VIRQ Queue Pending
```

这些状态之间具有因果关系，但不要求始终完全相同。

## 16.9.2 Lost Interrupt：为什么设备已经完成操作，TA 却一直等待？

假设：

```
TA submits Crypto request
        |
        v
Crypto Engine completes
        |
        v
IRQ occurs
        |
        v
OpenSBI masks HWIRQ
        |
        v
VIRQ Queue full
        |
        v
VIRQ enqueue fails
```

如果此时没有正确处理失败：

```
HWIRQ already masked
VIRQ not queued
TA never notified
```

那么 TA 可能永远阻塞。

这是一种典型的 Interrupt State Transition 不具备原子性 的设计缺陷。

一个可靠的队列入队操作必须考虑：

```
Mask HWIRQ
    |
    v
Enqueue VIRQ
    |
    +-- Success:
    |      Keep masked until service
    |
    +-- Failure:
           Preserve event / retry / quarantine
```

不能在入队失败后直接丢弃事件。

### 如何设计队列溢出的恢复策略？

取决于设备语义：

可合并的 Level Interrupt

可以保留一个 Pending 标志，稍后重新扫描设备状态。

不可合并的 Edge Event

如果没有硬件事件计数或 FIFO，简单保留一个 Pending Bit 可能丢失事件次数，需要确认硬件是否提供其他可靠状态。

DMA Completion

通常应依赖 Completion Ring 或 Descriptor Ownership，而不应该完全依靠 IRQ 次数。

安全关键事件

例如 Tamper Detection 或 Security Violation，可能必须记录到可信日志，并执行 Fail-Safe/Fatal Handling，而不是普通重试。

## 16.9.3 Lost Wakeup：检查与休眠之间的竞态

这是 SMP/RTOS 工程师非常熟悉，但在 TEE 中同样容易出现的问题。

考虑：

```
Hart 0                      Hart 1
------                      ------
Check VIRQ queue = empty
                            Enqueue VIRQ
                            Send notification
Prepare to sleep
Enter WFI
```

如果 Hart 0 的检查与进入 WFI 之间没有正确的同步协议，就可能出现 Missed Wakeup。

这里需要区分：

- 软件 Condition Variable / Wait Queue 的 Lost Wakeup。
- RISC-V WFI 的架构行为。
- VIRQ Pending State 与 Notification State 的同步。

`WFI` 允许实现因多种原因恢复执行，甚至可以表现得类似 NOP。因此，不能把 WFI 当成一个会自动保证队列状态一致性的同步原语。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



可靠的设计通常要求：

1. 原子化检查 Pending State。
2. 使用正确的 Memory Ordering。
3. 进入休眠前确保 Notification 不会被丢失。
4. 唤醒之后重新检查队列，而不是假设一次唤醒一定对应一个可处理事件。

## 16.9.4 SMP 中的 Queue Race

假设：

```
Hart 0: receives HWIRQ
Hart 1: completes VIRQ
```

而两个 Hart 都操作同一个 Domain 的 Pending Queue。

需要考虑：

- Queue Head/Tail 是否由同一把锁保护。
- Mapping 是否可能在处理期间释放。
- Completion 是否允许在不同 Hart 上执行。
- 一个 IRQ 是否可能被两个 Hart 同时 Claim。
- Domain 正在退出时是否还允许 VIRQ Enqueue。
- `return_to_prev` 是否属于全局状态还是 Per-Hart 状态。

特别是最后一点：

如果使用一个全局 `return_to_prev`，那么两个 Hart 同时处理不同 Domain 的中断时，很可能互相覆盖恢复状态。

这就是为什么 Per-(Domain, Hart) State 在设计上如此重要。

但 Per-Hart Queue 并不能自动解决所有并发问题。

例如，同一个设备的中断被错误地路由到两个 Hart，仍然可能造成设备驱动并发访问、Double Completion 或共享状态破坏。

## 16.9.5 生产系统中的错误恢复策略

我建议将异常处理分为三个等级。

| 等级                | 典型问题                         | 建议策略                  |
| ----------------- | ---------------------------- | --------------------- |
| Recoverable       | 暂时性 Queue 满、重复通知             | 保留 Pending、重试、限流      |
| Degraded          | 某设备反复 IRQ Storm              | Mask Source、记录故障、隔离设备 |
| Security Critical | 未授权路由、跨域 Completion、不可恢复隔离错误 | Fail-Closed、审计、按策略复位  |

对安全关键资源而言，一个重要原则是：

宁可让设备停止服务，也不能为了恢复服务而临时取消安全隔离。

比如某个 Secure Crypto Engine 的 IRQ 无法完成，不应该简单地把设备 MMIO 开放给 Linux，让 Linux 代替 OP-TEE 清中断。

这样可能把一个可用性故障升级为安全漏洞。

# 16.10 Secure Interrupt 与普通 Interrupt 的隔离要求

这是本章最后一个理论专题，也是从功能实现转向系统安全架构的关键一步。

## 16.10.1 先定义 Threat Model

假设：

- Linux Kernel 已经被攻击者完全控制。
- OP-TEE 和 OpenSBI 属于可信 TCB。
- Crypto Engine、Key Storage 等资源属于 Secure Domain。
- 中断控制器寄存器可以被不同的总线 Master 访问。
- 部分外设支持 DMA 或 MSI。

我们希望保证：

即使 Linux 被攻破，也不能利用中断控制基础设施访问 TEE 秘密、伪造安全状态或取得不应有的资源访问权限。

同时要承认：完全防止恶意 REE 制造 DoS 通常并不现实。安全架构应明确区分机密性、完整性以及可用性的保障程度。

## 16.10.2 Secure Interrupt 的三个隔离层次

第一层：Device Isolation

Secure Device MMIO、DMA、内部寄存器及 Key Storage 不允许 REE 任意访问。

第二层：Interrupt Controller Isolation

Secure IRQ 的路由、使能、优先级及 MSI 目标不能由不可信 REE 任意修改。

第三层：Execution Context Isolation

Secure IRQ 只能在被授权的安全执行上下文中得到处理，不能意外进入 Linux 的 S-mode Trap Handler。

仅有其中任何一层都不充分。

例如：

```
Linux cannot access Crypto MMIO
```

并不能推出：

```
Linux cannot disable Crypto IRQ
```

同样：

```
Crypto IRQ routed to OP-TEE
```

也不能推出：

```
Linux cannot DMA into OP-TEE RAM
```

必须整体设计。

## 16.10.3 PMP 为什么不能解决全部中断安全问题？

PMP 主要约束 Hart 发起的内存访问。

但外部 DMA Master 并不天然受到 Hart PMP CSR 的保护。

例如：

```
PCIe Device
     |
     | DMA Write
     v
OP-TEE Protected RAM
```

如果没有 IOMMU、IOPMP、WorldGuard 或等效的系统级访问控制，仅仅配置 Hart PMP 不能保证这条 DMA 路径被阻断。

同样，对于 MSI：

```
Untrusted PCIe Device
        |
        | MSI Write
        v
Secure IMSIC Target Address
```

需要约束设备能够写入哪些 MSI 地址和中断身份。

因此，真正的 SoC Security Architecture 必须覆盖：

- CPU Initiated Memory Access。
- DMA Initiated Memory Access。
- Interrupt Controller MMIO。
- MSI Address/Data。
- Interconnect Security Attributes。
- Security Domain Execution State。

RP016 Milestone 5 使用 WorldGuard 实现系统级硬件隔离，并支持在 Domain Switch 时更新 WorldGuard 相关状态。它很适合作为理解 PMP 局限及系统级隔离的参考案例。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



## 16.10.4 如何防止不可信 Domain 伪造 VIRQ Completion？

假设 Linux 可以调用某个 SBI VIRQ Complete 接口。

如果实现仅仅检查：

```
complete_virq(5);
```

而不检查调用者身份，那么可能出现：

```
Linux issues COMPLETE for OP-TEE VIRQ 5
           |
           v
OpenSBI unmasks Crypto HWIRQ
           |
           v
Secure interrupt state corrupted
```

因此 VIRQ Completion 必须绑定安全上下文。

例如需要验证：

```
Caller Domain
Caller Hart
MPXY Channel
VIRQ Ownership
Current In-Service State
Completion Token / Lifecycle
```

可以设计一个抽象判定：

\\[ \operatorname{CompleteAllowed}(D,H,V) \\]

仅当以下条件全部成立：

\\[ Owner(V)=D \\]

\\[ Assigned(V,H)=true \\]

\\[ State(V)=IN\\\_SERVICE \\]

才允许完成。

对于可能迁移 Hart 的设计，不能机械地要求始终由原 Hart 完成，而应该使用明确授权的 Completion Ownership 与 Migration Protocol。

重要的是：这些验证必须由可信的 M-mode 固件实施，不能依靠 Linux 自己遵守规则。

## 16.10.5 Domain Switch 与 Interrupt Routing 的组合攻击面

再考虑一个危险场景：

```
Hart 0 currently runs Linux

Secure IRQ arrives
       |
       v
OpenSBI starts Domain Switch
       |
       v
Isolation policy being updated
       |
       v
Another interrupt arrives
```

如果切换过程允许不受控的中断重入，就可能在中间状态再次访问 Domain Context 或中断控制器。

需要保证：

- 关键权限更新阶段不可被普通中断不安全地重入。
- Nested Trap 不会覆盖原先尚未保存完毕的 Trap Context。
- Domain Context 的保存和恢复满足可重入约束。
- 异常退出不会把系统留在错误的硬件安全配置中。
- 任何错误路径都不能返回到不符合当前权限配置的 S-mode。

这也解释了为什么 Domain Switching 和 Interrupt Handling 必须共同进行安全审查。

只分别审查 `sbi_domain_context.c` 和 `irqchip` 驱动是不够的。

## 16.10.6 Secure Interrupt Security Review Checklist

| 检查项                      | 必须确认                         |
| ------------------------ | ---------------------------- |
| IRQ Ownership            | 每个安全关键 Source 都有唯一且可信的 Owner |
| Controller Configuration | 非可信软件不能修改安全路由                |
| MSI Isolation            | 设备不能写入未授权的 IMSIC File        |
| DMA Protection           | DMA 不可突破安全内存边界               |
| Domain Switch            | 硬件隔离更新与 CPU Context 恢复顺序正确   |
| VIRQ Mapping             | 无跨 Domain 的非法映射              |
| VIRQ Queue               | 无双重领取、越界或未授权操作               |
| Completion               | 调用者及中断生命周期得到验证               |
| Interrupt Storm          | 不会导致无限制资源耗尽                  |
| Fault Handling           | 错误恢复不会主动放宽安全策略               |

对 Staff Engineer 来说，这张表不应该只用于最终测试，而应该在架构设计 Review 阶段就使用。

# 16.11 真实源码分析路径

经过前面的理论学习，现在应该进入源码。

我推荐按中断产生、OpenSBI Trap、Domain Switch、OP-TEE Trap 的顺序来阅读。

## 16.11.1 OpenSBI 主线源码

| 文件                             | 重点分析内容                         |
| ------------------------------ | ------------------------------ |
| `lib/sbi/sbi_trap.c`           | M-mode Trap 分发、AIA/非 AIA 路径    |
| `lib/sbi/sbi_irqchip.c`        | IRQCHIP 抽象及外部中断处理              |
| `lib/sbi/sbi_domain.c`         | Domain 权限和资源管理                 |
| `lib/sbi/sbi_domain_context.c` | Per-Hart Domain Context Switch |
| `lib/sbi/sbi_hart.c`           | Hart 初始化与相关 CSR 配置             |
| `lib/sbi/sbi_ipi.c`            | IPI 相关处理                       |

阅读时最值得关注的是：

```
/* sbi_trap.c 的简化控制逻辑 */

if (interrupt) {
    if (hart_supports_aia)
        handle_aia_interrupt();
    else
        handle_legacy_interrupt();
}
```

主线源码中对应的具体入口是：

```
sbi_trap_handler()
sbi_trap_aia_irq()
sbi_trap_nonaia_irq()
sbi_irqchip_process()
```

其中 AIA 路径可以通过 `mtopi` 等机制识别当前优先处理的中断类别，再调用对应处理函数。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



要注意：

主线 OpenSBI 的这些函数不是完整的 RP016 M3 VIRQ 实现。 VIRQ Mapping、Courier、Domain-Aware Routing 还要结合对应的项目 Patch Series 分析。

## 16.11.2 RISC-V OP-TEE 源码

推荐从：

```
core/arch/riscv/kernel/
    entry.S
    thread_rv.S
    thread_arch.c
    abort.c
    sbi.c
```

以及对应的 OP-TEE Thread / Interrupt 通用层开始。

重点关注：

- Supervisor Trap Entry。
- Trap Context 保存。
- Trusted Thread 状态。
- 用户 TA 异常处理。
- SBI ECALL。
- 需要 Yield 时的执行路径。

对于仍在演进的 RISC-V Interrupt/Foreign Interrupt 支持，不应该直接假定它已经和 Arm 实现完全等价。

## 16.11.3 三组最值得对照的实现

OpenSBI Trap

[sbi_trap.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_trap.c?utm_source=chatgpt.com)

观察 MEIP、`mtopi`、IRQCHIP 和 CSR Delegation。

OpenSBI Domain Context

[sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)

观察 `hart_context`、`satp`、`stvec`、FP/Vector 和 Domain Switch。

RISE Interrupt Prototype

[RP016 Milestone 3](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/milestone3/?utm_source=chatgpt.com)

观察 HWIRQ/VIRQ Mapping、Per-Domain Queue 和 SEIP Notification。

# 16.12 QEMU Hands-on Lab

理论部分已经完整覆盖了 16.1～16.10，接下来用三个实验把它们串联起来。

## Lab 1 — APLIC M-mode IRQ Handling

目标：验证 UART → APLIC → MEIP → OpenSBI 的完整路径。

可以使用 RP016 的 Buildroot 环境。官方提供了基于 `qemu_riscv64_virt_optee_defconfig` 的构建方法。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



如果采用已配置好的 QEMU 固件和根文件系统，可以选择 APLIC Direct 模式：

```
qemu-system-riscv64 \
    -M virt,aia=aplic \
    -m 2G \
    -smp 4 \
    -nographic \
    -S -s \
    ...
```

`...` 代表需要由实际构建环境提供的 BIOS、Kernel、DTB 和 RootFS 参数，不能直接把这段示意命令当成完整启动脚本。

建议优先使用 RP016 Buildroot 生成的 `start-qemu.sh`，并在其中检查 APLIC 相关参数。

连接 GDB 后：

```
set architecture riscv:rv64

break sbi_trap_handler
break sbi_irqchip_process

continue
```

进入 Trap 后：

```
info registers
x/12i $pc
bt
```

具体 CSR 是否可通过 `info registers` 显示，取决于 GDB/QEMU 的调试接口；可以使用：

```
info all-registers
```

检查当前提供的架构寄存器。

需要记录：

```
mcause
mepc
mstatus
mip
mie
mideleg
```

如果连接到实际 M-mode Trap Handler，分析：

```
mcause = Machine External Interrupt
```

随后跟踪 APLIC Claim，观察具体的 HWIRQ ID。

实验成功标准：

- UART 输入能够触发 MEIP。
- OpenSBI 能领取正确 HWIRQ。
- 设备端状态被正确清除。
- 返回之后不发生 Interrupt Storm。

## Lab 2 — Domain-Aware VIRQ Routing

目标：让两个 S-mode Domain 使用同一个 Hart，通过 Interrupt 完成 Domain Context Switching。

推荐参考 RP016 M3 的 QEMU Bare-metal Test，而不是一开始就修改 Linux/OP-TEE。

其测试平台使用 4 个 Hart：

```
Hart 0 / 1 : Linux
Hart 2     : Domain 1 / Domain 2
Hart 3     : Available
```

在 Hart 2 上：

```
Domain 1 currently executing
         |
         v
UART HWIRQ 10 arrives
         |
         v
OpenSBI resolves owner = Domain 2
         |
         v
Domain 1 -> Domain 2
         |
         v
VIRQ delivered
         |
         v
Domain 2 ISR
         |
         v
Return to Domain 1
```

建议分别记录：

| 时间点 | 当前 Domain | Privilege | CSR               |
| --- | --------- | --------- | ----------------- |
| T0  | Domain 1  | S         | `satp`, `stvec`   |
| T1  | Domain 1  | M Trap    | `mepc`, `mstatus` |
| T2  | OpenSBI   | M         | Domain Context    |
| T3  | Domain 2  | S         | `satp`, `stvec`   |
| T4  | Domain 2  | S Trap    | `scause = SEIP`   |
| T5  | Domain 2  | S         | VIRQ Processing   |
| T6  | Domain 1  | S         | Restored Context  |

重点检查：

1. Domain 1 的 `satp` 是否在恢复后正确。
2. Domain 2 的 `stvec` 是否被使用。
3. VIRQ Queue 是否属于 Domain 2 / Hart 2。
4. Completion 是否触发正确的 Unmask。
5. Domain 1 恢复后的寄存器状态是否保持正确。

## Lab 3 — 故障注入与安全验证

目标：验证错误路径不会破坏隔离。

建议设计以下实验：

| Fault Injection          | 期望行为                      |
| ------------------------ | ------------------------- |
| ISR 不清除 UART Status      | 观察 Level IRQ 重复触发并实施 Mask |
| VIRQ Queue Full          | 不丢失安全事件，不发生队列越界           |
| 错误 Domain 调用 COMPLETE    | 返回权限错误，不修改硬件中断状态          |
| 重复 COMPLETE              | 不能对同一事件错误地完成两次            |
| Target Domain 正在切换       | VIRQ 不进入错误的 Context       |
| Linux 尝试修改 Secure IRQ 路由 | 硬件或可信固件拒绝                 |
| FP/Vector Context 被中断    | 不发生跨 Domain 状态泄露          |

其中部分故障注入需要自行修改实验分支，并不是 RP016 默认已经提供的测试。

### 性能测量建议

对完整的 Interrupt-Driven Domain Switch，至少分解出：

\\[ T\_{\mathrm{total}} = T\_{\mathrm{hw}}+ T\_{\mathrm{Mtrap}}+ T\_{\mathrm{route}}+ T\_{\mathrm{switch}}+ T\_{\mathrm{notify}}+ T\_{\mathrm{ISR}}+ T\_{\mathrm{return}} \\]

分别对应：

- \\(T\_{\mathrm{hw}}\\)：设备到 CPU 中断可见的时间。
- \\(T\_{\mathrm{Mtrap}}\\)：进入 OpenSBI Trap Handler 的时间。
- \\(T\_{\mathrm{route}}\\)：HWIRQ Claim / Mapping / Domain Lookup。
- \\(T\_{\mathrm{switch}}\\)：Domain Context Switch。
- \\(T\_{\mathrm{notify}}\\)：SEIP / VIRQ Delivery。
- \\(T\_{\mathrm{ISR}}\\)：目标 Domain 的中断服务时间。
- \\(T\_{\mathrm{return}}\\)：Completion 和恢复原 Domain。

工业项目不仅要测平均延迟，还应关注 P99、P99.9、Worst-case Observed Latency，并明确这些测量不能自动构成严格的实时上界证明。

# 16.13 Staff Engineer 级面试题及参考答案

以下 12 道题结合 RISC-V Server SoC、OpenSBI、OP-TEE、APLIC/IMSIC 和真实中断故障场景。

每道题不仅要求解释机制，还要求给出具体的设计思路、验证方法和安全性分析。

## Question 1 — 为什么 `mideleg` 不能实现 Linux/OP-TEE 中断隔离？

场景：

同一 Hart 可以在 Linux Domain 与 OP-TEE Domain 之间切换，两者都运行在 S-mode。

一位工程师建议：

> 我们把 External Interrupt Delegation 配置好，让 SEIP 交给 S-mode 处理，就不需要 OpenSBI 参与 Interrupt Routing 了。

请分析这个设计是否正确。

参考答案：

这个结论不成立。

`mideleg` 解决的是 Privilege-level Trap Delegation，而不是 Security Domain Ownership。

当中断被交付给 S-mode 时，CPU 使用当前 S-mode 的 Trap Context，包括当前的 `stvec` 等状态。它并不知道这个中断属于 Linux 还是 OP-TEE。

因此，如果 Linux 正在执行，Secure IRQ 被直接投递为 SEIP，可能进入 Linux 的 Trap Handler。

正确方案取决于硬件能力：

- 如果硬件支持可靠的安全分区和独立中断投递，可以直接将 Secure IRQ 交给被授权的执行环境。
- 如果一个 hart 上通过软件复用多个 S-mode Domain，而且硬件不能动态识别安全 Domain，那么需要可信固件维护 IRQ Ownership，并在必要时进行 Domain Switch。

需要同时保证设备 MMIO、Interrupt Controller 配置和实际执行上下文的安全隔离。

这道题考察的是从 CPU Privilege Model 推导系统级 TEE Isolation 的能力。

## Question 2 — 为什么在 Linux 关闭中断后，OpenSBI 仍能响应 MEIP？

场景：

Linux 执行：

```
csrci sstatus, 0x2
```

关闭 S-mode Global Interrupt Enable。

随后一个 APLIC M-mode External Interrupt 到来。

请解释 CPU 是否能够进入 OpenSBI。

参考答案：

可以，前提是满足 M-mode Interrupt 的 Pending、Enable 和 Delegation 条件。

当前 CPU 在 S-mode，而 M-mode 的优先级更高。

RISC-V Privileged Architecture 规定：当 CPU 正运行在较低 Privilege Level 时，更高 Privilege Level 的中断不受该高特权级 Global Interrupt Enable Bit 的普通屏蔽条件限制。

因此，Linux 关闭 `sstatus.SIE` 不会阻止满足条件的 Machine External Interrupt 进入 M-mode。

需要检查：

```
mip.MEIP
mie.MEIE
mideleg
Current Privilege
```

而不能只检查 `sstatus.SIE`。

还必须注意：如果当前正在 M-mode 执行，则 M-mode 的 Global Interrupt Enable 规则不同。

这道题的关键是理解 Global Interrupt Enable 的效果依赖当前 Privilege Mode。

## Question 3 — Crypto IRQ 进入错误的 Domain，如何定位？

场景：

一颗 RISC-V SoC 中：

```
Crypto HWIRQ 42 -> OP-TEE
UART HWIRQ 10   -> Linux
```

系统运行中偶发 Crypto IRQ 被 Linux 处理，导致 OP-TEE TA 超时。

请给出完整的定位步骤。

参考答案：

首先确定异常发生在哪个层次。

第一层，检查设备硬件：

```
Crypto IRQ Status
Crypto IRQ Enable
APLIC sourcecfg[42]
APLIC target[42]
```

确认 HWIRQ 42 是否被错误配置到 Linux 的 S-mode 中断入口。

第二层，检查 OpenSBI：

```
Current Domain
HWIRQ Claim
Route Lookup
Target Domain
Target Hart
VIRQ Mapping
```

确认 IRQ 42 对应的 Controller UID 和 Domain Routing Policy。

第三层，检查 Context Switching：

```
saved Linux stvec
restored OP-TEE stvec
satp
sie
sip
mstatus.MPP
```

第四层，检查并发：

是否存在 Domain Switch 期间路由配置被修改、Pending Queue 竞争、或旧 SEIP 状态遗留。

最后设计 Stress Test：

多 hart 并发触发 Crypto 和 UART，记录完整事件时间线，确保每次 IRQ 都有确定的 Owner 和最终 Handler。

最重要的是不要只看 Linux `/proc/interrupts`，因为该接口无法单独证明 M-mode Domain Routing 是否正确。

## Question 4 — 如何设计一个不会丢中断的 VIRQ Queue？

场景：

OpenSBI 使用每个 Domain/Hart 一个 Ring Buffer。

当队列容量为 32 时，Crypto Engine 在短时间内完成了大量请求，产生 Queue Overflow。

请设计正确的处理机制。

参考答案：

首先需要区分设备事件能否合并。

如果 Crypto Engine 通过 Completion Ring 记录请求完成，那么 IRQ 只是通知，不需要将每一次完成都表示为独立的 VIRQ Entry。

可以使用一个 Pending Notification，之后由 OP-TEE 批量消费 Completion Ring。

对于不能合并的事件，需要保证事件数据在设备或软件中可靠保留。

状态机应至少包括：

```
IDLE
PENDING
IN_SERVICE
REARM_PENDING
```

Queue Overflow 时不能直接丢弃不可恢复事件。

可以选择：

- 保持 HWIRQ Masked。
- 将设备标记为需要重新 Poll。
- 保留可恢复的 Pending State。
- 在安全上下文中实施有界重试。
- 对持续异常触发进行 Quarantine。

并发上需要保证队列状态和通知状态的一致性。

验证时应进行高频中断、Queue Full、重复触发、跨 Hart 竞争和设备完成状态检查。

一个合格答案还必须解释：为什么 Ring Buffer Lock 只能保护数据结构并发访问，却不能自动保证事件语义不丢失。

## Question 5 — Domain Switch 中为什么必须保存 `stvec`、`satp` 和 `sie`？

场景：

某团队实现 RISC-V Domain Switch，只保存 GPR 和 `sepc`，因为他们认为 Linux 和 OP-TEE 都是 S-mode。

请指出风险。

参考答案：

保存 GPR 和 `sepc` 只保留了执行位置与通用寄存器状态。

不同 Domain 还拥有不同的 Supervisor System State。

`stvec` 决定后续 S-mode Trap 的入口。如果错误地保留 Linux 的 `stvec`，OP-TEE 发生异常时可能跳入 Linux Trap Handler。

`satp` 决定地址转换上下文。如果保留 Linux 页表，OP-TEE 的 VA 可能翻译错误，甚至破坏隔离。

`sie` 决定当前 Domain 的中断使能状态。如果不保存与恢复，可能让 OP-TEE 意外接收 Linux 的 Interrupt。

此外，还必须考虑：

```
sscratch
sstatus
sepc / scause / stval
FP/Vector registers
AIA-related context
Translation synchronization
Hardware isolation state
```

实际需要保存的范围应由实现架构与安全模型决定。

对于 Security Domain Switch，一个重要验收标准是：恢复后的执行状态不得依赖前一个 Domain 留下的任何未经授权的架构状态。

## Question 6 — 如何处理 OP-TEE 运行期间的 Linux Network IRQ？

场景：

Hart 0 正在执行 OP-TEE TA，Linux NIC 发生 RX Interrupt。

当前采用 M-mode Mediated IRQ Routing。

请设计处理流程，并说明什么情况下不允许立即切换。

参考答案：

OpenSBI 首先接收并识别 NIC HWIRQ，确定其 Owner 为 Linux。

然后判断当前 hart 是否正运行 OP-TEE。

如果需要切换，必须确保 OP-TEE 的执行能够安全暂停。

如果 OP-TEE 正在一个可中断的 Trusted Thread 中，可以保存必要状态，执行 Yield，并恢复 Linux Domain。

Linux 处理 NIC IRQ 后，再通过约定的 Resume 路径恢复被挂起的 TEE 执行。

但不能在任何指令位置无条件切换。

例如：

- OP-TEE 正处于不可重入的 Critical Section。
- 正在修改敏感 Page Table。
- 关键 Context 尚未完成保存。
- 某些硬件状态正在进行非原子安全切换。

这些情况下需要延迟、屏蔽或者采用专门的安全退出路径。

还要设计 Linux 不 Resume 时的资源和线程管理策略。

本题考察 Foreign Interrupt、Trusted Thread Suspension、Domain Switch 和 REE 不可信模型的综合理解。

## Question 7 — APLIC Direct 和 IMSIC MSI 哪种更适合服务器？

场景：

新 SoC 拥有 64 个 Application Harts，大量 PCIe NVMe、NIC 和 DMA Queue。

安全团队正在选择 Interrupt Architecture。

请给出架构建议。

参考答案：

对于大量 MSI-capable 设备和多队列服务器负载，APLIC + IMSIC 通常具有很强的吸引力。

IMSIC 可以为目标 Hart 提供本地 Interrupt File，使 MSI 分发不必全部依赖一个传统的中央 Claim/Complete Controller。

APLIC 仍然可以处理传统 Wired IRQ，并根据硬件能力将这些中断转换为 MSI。

但是不能忽视：

1. MSI Remapping 和 IOMMU。
2. Secure IMSIC File 的访问限制。
3. Guest/Virtualization Interrupt File 的管理。
4. Hart Affinity。
5. Domain Context Switching。
6. Interrupt Storm 与优先级策略。

如果 SoC 规模较小，只有少量 Wired IRQ，APLIC Direct Mode 可能更简单。

架构选择应该由实际设备规模、安全边界、性能指标和硬件成本决定。

## Question 8 — 为什么不能在 VIRQ Complete 时只检查 VIRQ ID？

场景：

Linux 和 OP-TEE 都有自己的 VIRQ 5。

Linux 通过 SBI 发起：

```
VIRQ_COMPLETE(5)
```

请设计防止跨 Domain Completion 的机制。

参考答案：

VIRQ ID 本身通常只在某个 Mapping Namespace 中具有意义。

不同 Domain 或 MPXY Channel 可能都存在 VIRQ 5。

因此必须根据可信的调用上下文确定：

```
Current Domain
Current Hart
MPXY Channel
VIRQ ID
Interrupt Lifecycle State
```

然后验证：

```
VIRQ belongs to caller
VIRQ is currently in service
Caller is authorized to complete
```

如果采用 Completion Token，还必须考虑其生命周期与重放保护。

不能让调用者在 ECALL 参数中随意传入一个 `domain_id`，然后直接信任它。

正确做法是由 M-mode Firmware 根据当前受保护的执行状态确定调用者 Domain。

这属于典型的 Confused Deputy / Cross-Domain Resource Ownership 问题。

## Question 9 — 如何证明 Domain Switch 不会泄露 FP/Vector 状态？

场景：

Linux 使用 RVV 运行高性能计算任务。

随后一个 Secure Interrupt 触发 Domain Switch，OP-TEE 开始执行加密任务。

请证明 OP-TEE 不会读到 Linux 的 Vector Register 内容。

参考答案：

首先要建立完整的状态清单：

```
v0-v31
vl
vtype
vstart
vcsr / vxrm / vxsat
sstatus.VS
```

还包括 FP Registers 和相关状态。

如果不同 Domain 能使用 FP/Vector 状态，那么 Domain Switch 必须保证：

- Outgoing Domain 状态得到正确保存。
- Incoming Domain 状态正确恢复或被安全初始化。
- 不存在部分寄存器沿用前一个 Domain 的情况。
- 异常退出或嵌套 Trap 不破坏状态隔离。

仅保存 `sstatus.VS` 并不等于保存 Vector Register File。

值得注意的是，RP016 Milestone 6 的设计讨论最终选择 Eager FP/Vector Context Switching，而不是依赖 M-mode 对 FS/VS 的 Lazy Tracking；其补丁已经进入 upstream。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



验证时应使用不同 Domain 写入不同的 FP/Vector Pattern，进行高频切换、异步中断和异常注入，并检查恢复结果与越权可见性。

这道题可以进一步扩展为安全审查：是否存在不同 VLEN 配置、Vector State 大小和异常路径的处理问题？

## Question 10 — 为什么正确的 PMP 配置仍可能发生 Secure IRQ 攻击？

场景：

安全团队已经确认 Linux 无法通过 CPU Load/Store 访问 OP-TEE 内存。

但某个不可信 PCIe 设备仍可影响安全执行环境。

请解释可能的攻击面。

参考答案：

PMP 主要约束 Hart 发起的内存访问，并不能自动约束外部 DMA Master。

可能存在：

- DMA 写入 OP-TEE 物理内存。
- PCIe 设备向未授权 IMSIC File 发送 MSI。
- Linux 通过可访问的 APLIC MMIO 重新配置中断。
- 不可信设备持续产生 Interrupt Storm。
- Shared Interrupt Controller 被多个安全域错误共享。

因此还需要：

```
IOMMU / IOPMP
MSI Remapping
SoC Firewall
WorldGuard
Secure Controller Register Ownership
```

是否具备这些能力必须由 SoC 设计确认。

一个完整 TEE Threat Model 必须涵盖 CPU、DMA、MSI、MMIO 和 Interconnect，而不是只检查 PMP 寄存器。

## Question 11 — 如何评估 Interrupt-Driven Domain Switch 的实时性能？

场景：

某安全控制中断要求从设备 Assert 到 OP-TEE ISR 开始执行的时间不超过 50 微秒。

请提出验证方法。

参考答案：

首先定义精确的延迟起止点，避免混淆：

```
Device IRQ Assert
    ->
First OP-TEE ISR instruction
```

将延迟分解成：

\\[ T\_{\mathrm{response}}= T\_{\mathrm{hardware}}+ T\_{\mathrm{trap}}+ T\_{\mathrm{routing}}+ T\_{\mathrm{switch}}+ T\_{\mathrm{delivery}} \\]

然后通过硬件计时、Trace Buffer 或性能计数器进行测量。

测试场景至少包含：

- 单 Hart 无负载。
- 多 Hart 高负载。
- Linux 高中断压力。
- OP-TEE Trusted Thread 繁忙。
- TLB / Cache Miss。
- VIRQ Queue 接近满载。
- 嵌套中断和其他更高优先级事件。

应记录最大观测值和尾部延迟。

但是 P99.9 小于 50 微秒并不能证明 Worst-case Latency 一定小于 50 微秒。

如果是硬实时要求，还需要分析调度延迟、不可抢占区间、优先级、硬件上界，以及所有可能阻塞路径。

如果软件 Domain Switch 无法满足指标，可能需要将安全中断直接路由给专用 Hart 或使用更明确的硬件隔离与投递机制。

## Question 12 — 设计一个生产级 Secure Interrupt Framework

场景：

你负责新一代 RV64 Server SoC 的 Secure Interrupt Architecture。

硬件资源：

```
32 Application Harts
1 Dedicated HSM Core
APLIC + IMSIC
IOMMU
WorldGuard / SoC Firewall
OpenSBI
Linux
OP-TEE
```

要求：

- Linux 无法篡改安全中断配置。
- OP-TEE 能处理中断驱动的 Crypto Completion。
- Linux NIC/NVMe 中断性能不能明显退化。
- 多 Hart 可以并发调用 OP-TEE。
- 系统需要支持 Suspend/Resume。
- 必须可以进行安全审计。

请给出架构设计。

参考答案：

建议将中断分成三个安全类别。

A. Normal World IRQ

例如 NIC、NVMe。

如果硬件隔离条件允许，直接使用 APLIC/IMSIC 投递给 Linux，减少 OpenSBI 中转。

B. Secure Domain IRQ

例如 Secure Crypto、Secure Peripheral。

可以选择受硬件保护的直接路由，或者由 M-mode Interrupt Manager 进行受控转发。

对于软件转发路径，必须具备：

```
HWIRQ Ownership
Domain Routing
VIRQ Mapping
Per-Domain Pending State
Secure Completion
```

C. Platform Security IRQ

例如 Tamper、Security Controller Fatal Event。

可以由 M-mode Firmware 或 Dedicated HSM Core 处理，按照平台安全策略决定是否通知 OP-TEE。

系统层面还应完成以下设计：

第一：可信资源分配

由 Root of Trust 保护启动配置，确保中断路由、设备授权和内存隔离策略不能被不可信 Linux 任意改变。

第二：Domain Context Isolation

OpenSBI 负责可靠保存和恢复 Supervisor Context，并保证 WorldGuard/PMP 权限变化与执行环境切换的一致性。

第三：Secure Interrupt Lifecycle

定义：

```
Receive
Claim
Route
Queue
Deliver
Handle
Complete
Return
```

每一步都必须有明确的 Ownership 和错误恢复策略。

第四：SMP Safety

实现 Per-Hart Context、跨 Hart 同步、Pending Queue 并发保护、设备 IRQ Affinity 与 CPU Hotplug 策略。

第五：Suspend/Resume

Suspend 之前必须处理 In-Service IRQ、Wakeup IRQ Ownership、Pending Queue 和可恢复 Context。

Resume 时应先恢复可信路由与硬件隔离配置，再允许不可信 Domain 执行。

第六：Security Testing

测试：

- Linux 修改安全中断路由。
- 未授权 MSI 注入。
- Cross-Domain VIRQ Complete。
- SMP Context Switch Race。
- Pending Queue Overflow。
- Interrupt Storm。
- Suspend/Resume 中断竞态。
- Secure Crypto DMA Isolation。

最后还必须明确：

哪些安全属性由 RISC-V ISA 保证，哪些由 AIA/IOPMP/WorldGuard 保证，哪些只是 OpenSBI/OP-TEE 的软件策略。

只有把这些责任划分清楚，架构才具备可审计性和可移植性。

# 16.14 本章核心知识总结

学完 Chapter 16，我建议重点检查自己是否真正理解以下六个结论。

第一：Interrupt Controller 和 Security Domain Manager 是两个不同的系统角色。

APLIC/IMSIC 负责中断投递，OpenSBI Domain Manager 负责软件执行环境与安全资源管理。二者必须通过可信策略衔接。

第二：`mideleg` 解决不了两个 S-mode Domain 的中断所有权问题。

Delegation 针对 Privilege Level，Domain Routing 针对安全执行上下文。

第三：Native / Foreign Interrupt 是 OP-TEE 的逻辑分类。

Native 由 OP-TEE 自身处理；Foreign 需要安全地交给其他 Owner，可能涉及 Trusted Thread Suspend、Yield 和 Resume。

第四：Interrupt-Driven Domain Switch 是完整的安全上下文切换。

除了 GPR、PC，还必须处理 S-mode CSR、MMU、FP/Vector、Hardware Isolation 和 Interrupt State。

第五：中断正确性不仅是进入 ISR。

真正的完成条件是：

```
Physical IRQ handled
AND
Logical VIRQ completed
AND
Device state consistent
AND
Domain context restored
AND
No security boundary violated
```

第六：Secure Interrupt Isolation 是系统级问题。

需要同时考虑 Hart PMP、SoC Firewall、IOMMU、IMSIC、Device MMIO、VIRQ Ownership 和 Domain Context。

## 建议你在进入 Chapter 17 前完成的三个任务

1. 源码任务： 能够从 `sbi_trap_handler()` 追踪 MEIP 到 IRQCHIP，并解释 `sbi_domain_context.c` 在切换中保存和恢复什么状态。
2. 实验任务： 在 RP016 M3 参考环境中复现 UART HWIRQ → VIRQ → Domain Switch → S-mode ISR → Return 的执行链。
3. 设计任务： 独立绘制一个 16-Hart RISC-V SoC 中 Linux、OP-TEE、OpenSBI、APLIC 和 IMSIC 的 Interrupt Ownership / Routing Architecture，并完成安全审查。

最后推荐按如下顺序重新阅读本章源码：[OpenSBI Trap](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_trap.c?utm_source=chatgpt.com) → [RP016 VIRQ Courier](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/milestone3/?utm_source=chatgpt.com) → [Domain Context Switching](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com) → [RP022 Yield / SYSIRQ](https://riscv-optee-docs-c9fea0.gitlab.io/rp022/milestone1/?utm_source=chatgpt.com)。

Chapter 16 最重要的收获应当是：你不仅知道中断如何进入 CPU，还能够设计和审查“中断如何在多个彼此隔离的安全执行环境之间被正确、安全地交付”。 这正是 RISC-V OP-TEE Architecture & Porting 从单纯 Bring-up 走向 Staff Engineer 级系统设计的关键能力。
