# Chapter 17 — SMP & Multi-Hart TEE Execution

RISC-V OP-TEE Architecture & Porting · 深度技术讲义 · Staff Engineer Level

本章研究一个非常重要、同时也很容易在真实 SoC 上出现复杂问题的主题：

当多个 RISC-V hart 同时执行 Linux、OpenSBI 和 OP-TEE 时，系统如何保证并发执行的正确性、内存一致性以及安全 Domain 之间的隔离？

我们不仅讨论传统 SMP Kernel 的 Spinlock、Atomic、Memory Barrier，还要研究 RISC-V OP-TEE 特有的问题：

- Linux Domain 与 TEE Domain 如何在多个 hart 上同时运行？
- Trusted Thread 为什么可能在不同 hart 上恢复执行？
- OpenSBI 的 Domain Context、OP-TEE 的 Thread Context 和 TA Context 如何协作？
- 为什么使用 `SFENCE.VMA` 不一定能够解决跨 hart 的 TLB 一致性？
- 为什么一个普通的 Spinlock 死锁可能演变成跨 Domain 的系统级故障？
- 为什么 SMP 和 Context Switch 的实现错误可能导致密钥或其他敏感状态泄露？

本章将贯穿 OpenSBI、OP-TEE、RISC-V Privileged Architecture 和真实 SoC 安全设计。

## 本章学习目标与内容覆盖

| 章节    | 核心内容                                           |
| ----- | ---------------------------------------------- |
| 17.1  | Hart、Per-Hart State 与 Trusted Thread Pool      |
| 17.2  | 多 Hart 下 Linux Domain / OP-TEE Domain 并发执行     |
| 17.3  | Domain Switch 对每个 Hart 的影响                     |
| 17.4  | SMP Spinlock 与 Memory Barrier                  |
| 17.5  | Shared Memory Cache Coherency                  |
| 17.6  | RVWMO 与 TEE Synchronization                    |
| 17.7  | Page Table Modification 与 Remote TLB Shootdown |
| 17.8  | Hart Start/Stop、IPI 与 SBI HSM                  |
| 17.9  | CPU Hotplug、Suspend/Resume 对 TEE 的影响           |
| 17.10 | Race、Deadlock、Livelock 与 Worst-case Latency    |
| 17.11 | 跨 Domain 状态泄露与安全验证                             |

结尾包含完整的调试实验、源码阅读路径和 15 道 Staff Engineer 级场景题及参考答案。

### 当前实现基线

本章采用截至 2026 年 11 月 2 日可以核实的 upstream 代码和规范作为基线。

其中有三个值得明确的事实：

1. OpenSBI 已有 Domain Context Switch 的基础实现，`sbi_domain_context.c` 按 Domain 和 Hart 组织上下文，并管理 CSR、FP/Vector 状态及隔离配置。

   [image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

   GitHub

2. RISC-V OP-TEE 当前源码具有 Per-Hart Core Local、Trusted Thread Suspend/Resume、跨 Hart 恢复支持和 Remote TLB Invalidation 路径。

   [image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

   GitHub+1

3. RISC-V OP-TEE 的 Linux 通信协议和部分中断路由增强仍在演进中。例如 2026 年 9 月的 RPMI TEE Linux 驱动仍以 RFC 形式提交，因此不能将所有设计中的能力视为已在任意平台可用。

   [image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

   Openwall Lists



本章中出现的伪代码和时序图，除明确标注真实函数名的部分外，均作为原理教学示例，而非声称与某一 commit 完全逐行一致。

# 17.1 Hart、Per-Hart State 与 Trusted Thread Pool

## 17.1.1 首先理解：16 个 Hart 不等于 16 个 OP-TEE 实例

假设一颗 RISC-V Server SoC 内部有 16 个 RV64 hart：

```
             RISC-V SoC
  +---------------------------------+
  | Hart 0       Hart 1              |
  | Hart 2       Hart 3              |
  | ...                              |
  | Hart 14      Hart 15             |
  +---------------------------------+
               |
        Shared DRAM / LLC
               |
       Platform Hardware
```

每个 hart 拥有独立的架构执行状态，例如：

- `pc`、`x1`～`x31`
- `mstatus`、`mepc`、`mcause`
- `sstatus`、`sepc`、`scause`
- `satp`、`stvec`、`sscratch`
- FP/Vector 状态（如果支持）
- 本地中断状态和地址转换缓存

但是，这不意味着每个 hart 都需要一个独立的 OP-TEE OS Image。

更合理的模型是：

```
         One OP-TEE Kernel Image
                  |
       +----------+----------+
       |                     |
   Global State         Per-Hart State
       |                     |
   Thread Pool         Hart 0 Core Local
   Memory Pools        Hart 1 Core Local
   TA Management       ...
   Global Locks        Hart 15 Core Local
```

多个 hart 可以共享同一份 OP-TEE Core Code、全局数据结构和资源管理器，同时每个 hart 维护自己独立的运行时状态。

这与 Linux SMP Kernel 或 NuttX SMP Kernel 的基本设计理念相似。

但有一个关键差别：OP-TEE 的 Trusted Thread 不等同于由一个完整 SMP Scheduler 独立调度的普通 RTOS Task。

OP-TEE 的 Trusted Thread 通常服务于来自 REE 的同步调用，并在必要时挂起，通过 RPC 返回 Normal World。

## 17.1.2 三个必须区分的概念

| 概念                     | 归属                  | 作用                             |
| ---------------------- | ------------------- | ------------------------------ |
| Per-Hart State         | CPU/Hart            | 维护当前 hart 正在执行什么、使用什么栈及中断状态    |
| Trusted Thread Context | OP-TEE Thread       | 保存一次可信执行任务的寄存器、栈、状态和恢复位置       |
| TA Context             | Trusted Application | 维护 TA 的地址空间、执行状态、Session 与安全资源 |

例如：

Hart 3 正在执行 Trusted Thread 2，而 Trusted Thread 2 正在执行 TA-A。

此时：

```
Hart 3
  |
  +-- Per-Hart State
  |      curr_thread = 2
  |
  +-- Trusted Thread 2
  |      kernel stack
  |      saved registers
  |      user mapping
  |
  +-- TA-A
         user stack
         virtual address space
         session context
```

这里不能简单地把三个 Context 合并。

因为接下来可能发生：

1. TA-A 发起一个需要 REE 参与的 RPC。
2. Trusted Thread 2 被挂起。
3. Hart 3 返回 Linux。
4. Linux 后续在 Hart 7 上发起恢复请求。
5. Trusted Thread 2 在 Hart 7 上继续运行。

因此：

Per-Hart State 跟随物理 hart，Trusted Thread State 跟随可信执行任务，TA State 跟随 TA/Session。

这是整个 Chapter 17 最重要的基础概念之一。

## 17.1.3 从真实 OP-TEE 源码理解

RISC-V OP-TEE 当前源码中，以下数据非常值得研究：

```
struct thread_core_local;
struct thread_ctx;
```

它们分别承担 per-hart 执行管理和 Trusted Thread 上下文保存的角色。

在 RISC-V 汇编入口 `thread_rv.S` 中，`tp` 被用于访问当前 hart 的 Core Local 数据。

例如，当前源码中的 `__get_core_pos()` 会从 Core Local 结构读取逻辑 hart index：

```
lw a0, THREAD_CORE_LOCAL_HART_INDEX(tp)
```

注意这不是直接读取 `mhartid`，而是通过初始化好的 Core Local 映射取得 OP-TEE 内部编号。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么要这么设计？

因为 RISC-V 的硬件 Hart ID 不保证连续。

例如，SoC 可能使用：

```
Hardware Hart ID:
0, 2, 4, 6, 8, 10, ...

OP-TEE Logical Core Index:
0, 1, 2, 3, 4, 5, ...
```

此时不能直接用硬件 Hart ID 索引 `thread_core_local[]`。

### 特别重要：`tp` 为什么需要在跨 Hart 恢复时重新设置？

当前 `thread_resume_from_rpc()` 源码中有专门处理这一问题的逻辑：

```
if (!is_user_mode(&threads[n].regs))
    threads[n].regs.tp = read_tp();
```

这段代码解决的是：

保存的 Trusted Thread Context 可能属于之前执行它的 Hart，而当前恢复执行的是另一个 Hart。

如果直接恢复旧的 Kernel `tp`，后续访问 Per-Hart Core Local 时可能访问原来 Hart 的数据结构。

结果可能是：

- 错误的 `curr_thread`。
- 错误的栈或中断状态。
- 并发数据结构损坏。
- 跨 hart 的安全状态混淆。

这不是假想场景，而是 upstream 源码明确处理的跨 hart 恢复问题。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里还要区分 Kernel `tp` 与 User TA 的 `tp`：用户态 `tp` 可能具有 TLS 语义，不能无条件用当前 Kernel Per-Hart Pointer 覆盖。

## 17.1.4 Trusted Thread Pool 与 Hart 数量的关系

假设：

```
CFG_TEE_CORE_NB_CORE = 16
CFG_NUM_THREADS     = 8
```

这表示一个设计场景：

- 平台最多配置 16 个 OP-TEE Core 位置。
- OP-TEE Trusted Thread Pool 包含 8 个执行上下文。

这并不意味着只能启动 8 个 hart。

也不意味着每个 hart 预先绑定一个 Trusted Thread。

Trusted Thread 数量决定了能够同时占用多少个可信 Thread Context，而 Core 数量描述能够参与执行的 hart 资源规模。

因此需要区分：

- 多个 hart 可以同时运行 OP-TEE Kernel 的不同路径。
- 同时存在的 Active/Suspended Trusted Thread 受到 Thread Pool 管理。
- 一次新的 Standard Call 可能因为没有空闲 Trusted Thread 而无法立即开始。
- 已经处于 Suspended 状态的 Thread 会保留相关 Context，不能直接当作 Free Thread 重用。

## 17.1.5 必须掌握的设计不变量

对于一个正确的 SMP OP-TEE，我建议你将下面这些条件作为代码审查的基础：

Invariant A： 同一 Trusted Thread 同一时刻不能在两个 hart 上处于 Active 执行状态。

Invariant B： 一个 hart 的 Per-Hart Core Local 必须准确反映该 hart 当前的执行上下文。

Invariant C： Suspended Thread 恢复时必须恢复其 Thread/TA 状态，但同时重新绑定当前 hart 专属的状态。

Invariant D： 访问全局 Thread Pool 时必须有正确的同步保护。

Invariant E： Thread Context 的生命周期不能早于其他 hart 可能仍在使用它的时刻结束。

这些不变量将贯穿后面全部章节。

# 17.2 多个 Hart 同时执行 Linux 与 OP-TEE

## 17.2.1 同一个 Hart 与不同 Hart 的并发模型

这是很多人第一次研究 RISC-V TEE SMP 时容易混淆的地方。

首先，一个 hart 在某个时刻只能执行一个当前执行上下文。即使 Linux 和 OP-TEE 都运行于 S-mode，它们也不会在同一个 hart 上真正并行执行。

但是，不同 hart 可以同时运行不同 Domain。

16-Hart SoC：某个时间点的 Domain 分布示例

Hart 0

Linux

Hart 1

Linux

Hart 2

OP-TEE

Hart 3

Linux

Hart 4

Linux

Hart 5

OP-TEE

Hart 6

Linux

Hart 7

Linux

Hart 8

Linux

Hart 9

OP-TEE

Hart 10

Linux

Hart 11

Linux

Hart 12

OP-TEE

Hart 13

Linux

Hart 14

Linux

Hart 15

Linux

REE Domain

TEE Domain

示意一个时间点的归属。Hart 是否能够在两个 Domain 间切换、哪些 hart 属于 possible_harts，以及可同时执行的数量，都取决于平台配置和固件实现。

假设 Linux 在 Hart 0、1、3、4 上运行，OP-TEE 在 Hart 2、5 上运行，那么：

- Hart 0 上 Linux 可以处理网络中断。
- Hart 1 上 Linux 可以继续运行其他应用。
- Hart 2 上 OP-TEE 可以处理 TA-A。
- Hart 5 上 OP-TEE 可以处理 TA-B。

这构成了真正的跨 Domain 并行执行。

与此相对，如果只有一个 hart 可用，Linux 和 OP-TEE 必须通过 Domain Switch 时间复用该 hart。

## 17.2.2 OpenSBI Domain 的 Hart 归属模型

OpenSBI 中存在两个重要的 Hart 集合：

```
possible_harts
assigned_harts
```

它们的意义不同。

`possible_harts` 描述允许分配给某个 Domain 的 hart 集合。

`assigned_harts` 描述当前实际分配给该 Domain 的 hart 集合。

例如：

```
Linux Domain:
    possible_harts = {0 ... 15}
    assigned_harts = {0,1,3,4,...}

OP-TEE Domain:
    possible_harts = {0 ... 15}
    assigned_harts = {2,5}
```

这里 `possible_harts` 可以重叠，但某个 hart 的当前 Domain 归属必须唯一。

OpenSBI 官方文档也明确规定：每个 hart 在任一时刻处于一个 Domain Context，SBI IPI、RFENCE、HSM 操作还受到 Domain 归属限制。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 一个值得深究的问题

如果 Hart 5 当前在 OP-TEE Domain，而 Hart 0 当前在 Linux Domain，那么 Linux 能否直接通过 SBI IPI 向 Hart 5 发送普通跨核中断？

不能简单认为一定可以。

因为 SBI 操作存在 Domain Access Control，Linux 不能默认控制当前属于其他 Domain 的 hart。

因此，一套完整的 RISC-V TEE 系统需要考虑：

- Domain 内部 IPI。
- Domain 之间的通知。
- Domain 归属切换。
- 跨 Domain 的中断投递。
- 不能被非法跨 Domain 请求绕过的访问控制。

这也是为什么 RISC-V OP-TEE 的 Domain-Aware Interrupt Routing 需要单独设计，而不能直接复用普通 Linux SMP IPI 语义。

## 17.2.3 多 Hart 并发与 Trusted Thread 并发不是一回事

考虑：

```
Hart 2 -> OP-TEE Thread 0 -> TA-A
Hart 5 -> OP-TEE Thread 1 -> TA-B
```

这表示两个 Trusted Thread 并行执行。

但是，如果 TA-A 与 TA-B 都要访问同一个全局 Secure Storage 数据结构，就必须考虑同步。

同理：

```
Hart 2 -> Thread 0 -> TA-A
Hart 5 -> Thread 1 -> TA-A
```

也不能因为是同一个 TA，就假设其调用一定串行。

是否允许并行、需要在哪一层序列化，还涉及 Single Instance TA、Multi Session、TA Session Lock 和服务内部资源同步策略。

因此：

Domain 的物理执行并发、Trusted Thread 的并发和 TA Session 的并发，是三个独立但相互影响的问题。

# 17.3 Domain Switch 对每个 Hart 的影响

这一节要解决一个核心问题：

一个 Hart 从 Linux Domain 切换到 OP-TEE Domain 时，到底发生了什么？另外 15 个 Hart 是否需要一起切换？

## 17.3.1 Domain Switch 是 Per-Hart 的

假设 Hart 3 从 Linux 切换到 OP-TEE：

```
Time        Hart 0      Hart 3       Hart 7
------------------------------------------------
T0          Linux       Linux        Linux
              |            |            |
T1          Linux       SBI ECALL    Linux
              |            |            |
T2          Linux       M-mode       Linux
              |            |            |
T3          Linux       OP-TEE       Linux
              |            |            |
T4          Linux       OP-TEE       Linux
              |            |            |
T5          Linux       Linux        Linux
```

Hart 3 的 Domain Switch 不要求其他 Hart 同时停止。

但如果这个切换改变了全局共享的硬件保护策略，或者影响其他 hart 的资源权限，就必须采取额外同步措施。

因此需要区分两种状态：

- Hart-local state：仅影响当前 Hart。
- System-wide state：可能同时影响所有 Hart、DMA Master 或总线设备。

例如 `satp` 通常是 Hart-local CSR，而 SoC Firewall 的某些配置可能是全局共享的。

不能因为 Domain Switch 在 Hart 3 上发生，就认为涉及的所有权限更新都天然是 Per-Hart 操作。

## 17.3.2 OpenSBI 的 Context 数据结构

在当前 upstream：

`lib/sbi/sbi_domain_context.c`

定义了用于保存一个 hart 在某个 Domain 中执行状态的 `struct hart_context`。

下面是根据当前实现提炼的结构示意：

```
struct hart_context {
    struct sbi_trap_context trap_ctx;

    unsigned long sstatus;
    unsigned long sie;
    unsigned long stvec;
    unsigned long sscratch;

    unsigned long sepc;
    unsigned long scause;
    unsigned long stval;
    unsigned long sip;

    unsigned long satp;
    unsigned long scounteren;
    unsigned long senvcfg;
    unsigned long srmcfg;

    struct sbi_fp_context fp_ctx;
    struct sbi_vector_context *vec_ctx;

    struct sbi_domain *dom;
    struct hart_context *prev_ctx;

    bool initialized;
};
```

这与实际源码中的字段对应，但为了聚焦概念省略了注释及其他实现上下文。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



它回答了一个重要问题：

为什么单纯保存 GPR 不足以完成 Linux → OP-TEE 的 Domain Switch？

因为两个 Domain 可能具有不同的：

- S-mode Exception Vector。
- S-mode Address Translation。
- Interrupt Enable/Mask。
- Counter/Environment Configuration。
- FP/Vector 执行状态。
- Trap 返回位置。

例如，如果只恢复 Linux 的 GPR，却忘了切换 `stvec`，那么 OP-TEE 运行期间发生 S-mode 异常时，可能跳转到错误的异常处理程序。

如果忘记恢复 `satp`，那么 TEE Domain 可能沿用 REE 的地址空间。

如果忘记处理 FP/Vector 状态，就可能造成计算结果破坏甚至敏感数据泄露。

## 17.3.3 Domain Switch 的源码级逻辑

在当前 OpenSBI 中，关键函数是：

```
switch_to_next_domain_context()
```

其主要流程可以概括为：

```
1. 确定当前 Domain 与目标 Domain
       |
2. 更新当前 Hart 的 Domain 归属
       |
3. 保存当前 S-mode CSR
       |
4. 恢复目标 Domain CSR
       |
5. 保存/恢复 FP 和 Vector 状态
       |
6. 保存/恢复 Trap Context
       |
7. 配置目标 Domain 的硬件隔离
       |
8. 执行所需的 Address Translation Fence
       |
9. 返回目标 Domain 的执行上下文
```

其中，第 7 步会调用：

```
sbi_hart_protection_reconfigure(
    scratch, current_dom, target_dom);
```

当前源码也包含对 `assigned_harts` 的更新，以及通过 `csr_swap()` 保存和恢复 S-mode CSR 的逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



需要强调：这是实际实现的逻辑摘要，不是供其他项目直接复制的安全切换模板。真正移植时必须验证所有中间步骤在异常、中断和并发情况下的安全性。

### 从安全架构角度重新理解

Domain Switch 实际上涉及两个层面：

Execution Context Switch

决定当前 Hart 下一条要执行哪个 Domain 的指令，以及使用哪个寄存器和地址空间状态。

Security Context Switch

决定当前 Hart 被允许访问哪些物理内存和设备。

两者缺一不可。

例如，假设从 OP-TEE 返回 Linux：

```
Execution Context = Linux
Security Context  = OP-TEE
```

如果这种组合在正常 Linux 指令执行时出现，Linux 就可能继承本不属于它的安全权限。

因此一个可靠的 Security Monitor 必须保证：

> 在目标 Domain 的不可信指令开始执行以前，目标 Domain 的硬件访问权限必须已经正确生效。

这是一条关键的安全不变量。

## 17.3.4 一个容易忽略的问题：FP/Vector Context

RISC-V 中，FP 和 Vector Register 是 Hart 上的物理执行资源。

例如 RVV 的寄存器文件可能保存 TA 使用过的中间秘密数据。

假设：

```
Hart 3:
  OP-TEE Domain
    TA-A 使用 Vector Register v0-v31
         |
         v
    Domain Exit
         |
         v
  Linux Domain
```

如果在 Domain Exit 时没有正确管理 Vector Context，Linux 可能观察到不应该属于它的旧状态，或者破坏 OP-TEE 恢复执行所需的状态。

早期设计曾考虑 Lazy Context Switch，但 RISE RP016 Milestone 6 最终采用了 Eager FP/Vector Context Switch，并于 2026 年合入 OpenSBI upstream。其一个关键原因是 M-mode 不能仅依赖 S-mode 可控制的 FS/VS Dirty 状态可靠判断所有寄存器使用情况。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



这里有一项非常重要的架构区分：

| 切换                    | 需要管理什么                              |
| --------------------- | ----------------------------------- |
| Linux ↔ OP-TEE Domain | OpenSBI 负责跨 Domain 的 FP/Vector 状态隔离 |
| TA-A ↔ TA-B           | OP-TEE 负责自己的 TA 用户态状态               |
| Hart 3 ↔ Hart 7 恢复执行  | 必须正确绑定目标 Hart 的本地状态                 |

三种切换具有不同的责任主体，不能认为 OpenSBI 保存了 FP/Vector 状态，就自动解决了 OP-TEE 内部不同 TA 的状态隔离。

## 17.3.5 SMP 下的特殊挑战：共享硬件保护配置

PMP 是典型的 Hart-local 保护机制。

如果 Hart 3 正在执行 OP-TEE，Hart 7 正在执行 Linux，那么它们可以有不同的 PMP 访问权限配置。

但如果底层使用全局性的 SoC Firewall，情况就可能不同。

假设某个硬件 Firewall 只提供：

```
SECURE_DRAM_ACCESS_ENABLE = 1 / 0
```

而没有按 Master ID、Domain ID 或其他执行身份隔离的能力。

那么：

```
Hart 3 -> OP-TEE 需要访问 Secure DRAM
Hart 7 -> Linux 不允许访问 Secure DRAM
```

单个全局 Enable Bit 无法同时表达这两个需求。

这不是软件加锁能够解决的问题。

即使对 Firewall Register 的写入进行序列化，也不能让两个同时运行的 Hart 拥有不同的访问权限。

这是硬件安全架构能力不足，而非普通的软件同步问题。

正确的设计可能需要按 Master/Domain 识别访问请求的硬件隔离，或者在不具备该能力时限制 Domain 并发执行。

# 17.4 SMP Spinlock 与 Memory Barrier

## 17.4.1 为什么禁用中断不等于 SMP Lock？

你之前有 NuttX SMP 和 Arm GIC 的经验，这个问题可以直接和 RTOS Kernel 对比。

单核环境中：

```
disable_interrupt();

shared_counter++;

enable_interrupt();
```

在满足调用上下文约束时，这可以防止当前 CPU 被本地中断打断。

但是在 SMP 中：

```
Hart 0                 Hart 1

disable_irq()
                       disable_irq()

counter = counter + 1
                       counter = counter + 1
```

双方禁用的只是本地中断。

Hart 0 并不能阻止 Hart 1 并发访问同一个全局变量。

因此必须引入跨 Hart 的同步原语，例如 Spinlock、Mutex 或 Atomic。

## 17.4.2 Spinlock 的基本实现原理

在 RISC-V A Extension 中，可以通过 AMO 构建 Spinlock。

下面是一个用于解释机制的 RV64 GNU Assembly 示例：

```
/*
 * a0 = lock address
 * 0 = unlocked
 * 1 = locked
 */

spin_lock:
    li t0, 1

1:
    amoswap.w.aq t1, t0, (a0)
    bnez t1, 1b

    ret

spin_unlock:
    fence rw, w
    amoswap.w.rl zero, zero, (a0)
    ret
```

这里的重点不是建议直接用这个简单锁替换 OP-TEE 内核锁，而是理解 `aq` 和 `rl`。

- `amoswap.w.aq`：对 Lock 的原子交换，并提供 Acquire Ordering。
- `amoswap.w.rl`：Release Ordering，确保临界区先前的访问不会被错误地排到解锁之后。
- `fence rw,w`：在该示例中额外加强释放前的排序，通常可由适当的 Release Atomic 语义替代，不必机械重复。

工业实现还必须考虑 CPU Relax、锁竞争公平性、Interrupt State、Memory Type 和锁调试等问题。

### 为什么 Acquire/Release 比简单的 Atomic Exchange 更重要？

假设：

```
Hart 0                   Hart 1

lock()
secret_state = READY     lock()
unlock()                  read(secret_state)
                          unlock()
```

正确的 Lock 不仅要保证互斥，还要保证：

Hart 1 成功获取锁后，能按照同步协议观察到 Hart 0 在释放锁前完成的共享内存更新。

这就是 Memory Ordering 的作用。

## 17.4.3 OP-TEE 中的 Global Thread Lock

RISC-V OP-TEE 的 `thread_arch.c` 已经有一个实际案例。

在 `__thread_alloc_and_run()` 中：

```
thread_lock_global();

for (n = 0; n < CFG_NUM_THREADS; n++) {
    if (threads[n].state == THREAD_STATE_FREE) {
        threads[n].state = THREAD_STATE_ACTIVE;
        found_thread = true;
        break;
    }
}

thread_unlock_global();
```

这段代码的设计意图非常清晰：

必须保证两个 hart 不能同时将同一个 Free Trusted Thread 标记为 Active。

`thread_resume_from_rpc()` 也在全局锁保护下检查 Suspended State 并转换为 Active State。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么检查和修改必须处于同一个临界区？

错误示例：

```
if (threads[id].state == THREAD_STATE_FREE) {
    /* Race window */
    threads[id].state = THREAD_STATE_ACTIVE;
}
```

可能发生：

```
Hart 3: read state == FREE
Hart 7: read state == FREE
Hart 3: state = ACTIVE
Hart 7: state = ACTIVE
```

两个 hart 同时拥有同一个 Thread Context。

如果二者使用同一个 Kernel Stack，后果可能是栈破坏、控制流损坏，甚至安全漏洞。

## 17.4.4 Spinlock 与 Interrupt 的关系

对于 SMP TEE Kernel，有一个需要特别强调的规则：

不能在持有 Spinlock 时，允许可能重新进入相同锁路径的本地中断或异步执行路径。

例如：

```
Hart 3:
    acquire(lock_A)
        |
        | Interrupt arrives
        v
    interrupt_handler()
        |
        v
    acquire(lock_A)
        |
        v
    Deadlock
```

原因是：

- 被中断的上下文持有 `lock_A`。
- Interrupt Handler 等待 `lock_A`。
- 原上下文只有 Interrupt Handler 退出后才能继续执行并释放锁。

因此内核中通常需要 `spin_lock_irqsave` 一类机制。

OP-TEE 的 RISC-V 实现同样明确检查：在持有 Spinlock 时，不能随意解除 Foreign Interrupt 的屏蔽。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



但还需要一个更深的判断：

禁用 S-mode Interrupt，是否足以避免所有能够抢占当前代码的事件？

不一定。

因为 M-mode 事件、NMI 类事件和平台特定异步异常的行为，不完全受 `sie` 或 `sstatus.SIE` 控制。

因此代码审查时不仅需要检查 Lock 本身，还需要检查可能在该临界区发生的更高特权级执行路径是否会重新进入相关资源。

## 17.4.5 Spinlock、Mutex、Atomic 应该如何选择？

| 同步机制          | 典型用途                     | 注意事项                         |
| ------------- | ------------------------ | ---------------------------- |
| Spinlock      | Thread State、短时间内核共享数据更新 | 不应在锁内执行可能睡眠或长时间阻塞的操作         |
| Mutex         | 可以等待的资源访问与服务序列化          | 需要对应的等待/恢复模型                 |
| Atomic        | 引用计数、状态标记、计数器            | 单个原子变量并不能自动维护多个字段的不变量        |
| Lock-free     | 高并发数据路径                  | Memory Ordering、ABA、生命周期更加复杂 |
| Per-Hart Data | 减少竞争                     | 跨 Hart 汇总及迁移仍需同步             |

尤其要注意，OP-TEE Trusted Thread Suspend/Resume 与普通 Linux Task Sleep 不完全一样。

在一个可能触发 RPC/Domain Exit 的路径中持有 Spinlock，往往是一种危险信号。

# 17.5 Shared Memory Cache Coherency

这节需要区分三个经常被混在一起的概念：

1. Cache Coherency。
2. Memory Ordering。
3. Security Isolation。

它们虽然有关联，但不是同一回事。

## 17.5.1 Cache Coherency 到底解决什么问题？

假设 OP-TEE 在 Hart 3 上写入一个共享 Buffer，Hart 7 随后读取。

```
Hart 3                         Hart 7
  |                               |
  v                               v
D-Cache 3                      D-Cache 7
  |                               |
  +-------------+-----------------+
                |
          Coherent Fabric
                |
               DRAM
```

如果 SoC 的 CPU Cache 是硬件一致性的，那么 Hart 7 对同一物理地址的读取，应按照平台定义的一致性和同步规则观察相关写入。

但注意：

Cache Coherence 负责处理缓存副本的一致性，不负责决定 Hart 7 应该在什么时刻读取 Buffer。

何时可以读取，需要软件同步机制，例如：

- Acquire/Release。
- Mutex。
- Spinlock。
- Message Protocol。
- Doorbell + Memory Barrier。

即使硬件 Cache Coherent，没有正确的同步协议仍然可能读取到尚未准备完成的数据。

## 17.5.2 Non-Coherent SoC 中的问题

某些 SoC 并不保证所有 CPU、DMA 和专用处理器共享完全一致的缓存域。

例如：

```
Linux CPU                  OP-TEE CPU
     |                          |
  D-Cache                     D-Cache
     |                          |
     +----------DRAM------------+
```

如果相关 CPU 不具备硬件 Cache Coherency，执行：

```
fence rw, rw
```

并不能自动完成 D-Cache Clean 或 Invalidate。

`FENCE` 保证的是规定范围内的内存访问排序，而不是一个通用的缓存维护指令。

因此在非一致性平台上，可能需要：

```
Producer:
    write buffer
    clean data cache
    publish ownership

Consumer:
    observe ownership
    invalidate data cache as required
    read buffer
```

具体操作还取决于缓存维护 ISA 扩展、平台 Cache Controller、内存属性和传输协议。

## 17.5.3 Linux / OP-TEE Shared Memory

考虑一次 TEE Invoke：

```
Linux
  |
  | Fill OP-TEE message and buffer
  v
Shared Memory
  |
  | TEE_CALL
  v
OpenSBI
  |
  | Domain Switch
  v
OP-TEE
  |
  | Read message and process data
  v
Shared Memory
```

这里有三个独立的正确性要求。

要求一：可访问性

共享内存必须能在其授权生命周期内，被相关 Domain 的 CPU 以合法权限访问。

要求二：一致性

在 OP-TEE 读取数据前，必须通过硬件一致性和必要的同步/缓存维护，保证写入的内容对目标 CPU 可见。

要求三：安全性

即使 Buffer 可以被 OP-TEE 读取，也不能因此认为其内容可信。

Linux 可能随时修改 Shared Memory，因此 OP-TEE 必须对参数、长度、偏移、引用和生命周期进行检查。

## 17.5.4 TOCTOU：共享内存不仅是同步问题

假设：

```
/* Unsafe example */
if (shared->len <= MAX_LEN) {
    memcpy(dst, shared->data, shared->len);
}
```

在 SMP 场景下：

```
Hart 3 / OP-TEE             Hart 7 / Linux

read len = 128
                              write len = 4096
memcpy(..., len)
```

如果第二次访问使用了被 Linux 修改后的值，就可能发生越界。

即使内存有硬件 Cache Coherence，这个 Bug 仍然存在。

安全设计应采用：

```
/* Conceptual example */
size_t len = read_untrusted_length_once(shared);

if (len > MAX_LEN)
    return ERROR;

copy_from_untrusted(validated_copy,
                    shared->data, len);

/* Process the private, validated snapshot. */
```

更完整的实现还需要检查地址范围、整数溢出、Buffer 生命周期、映射有效性，以及复制失败的清理路径。

这个例子说明：

Shared Memory 的安全性不能由 Cache Coherence 或 Memory Barrier 单独保证。

对于 OP-TEE 来说，能被 REE 访问的数据始终应该按 Untrusted Input 处理。

## 17.5.5 DMA 对 SMP TEE 的影响

如果 SoC 存在独立 HSM、Crypto Engine 或 PCIe DMA Master，那么问题进一步复杂化。

例如：

```
                    Shared DRAM
                    /    |     \
                   /     |      \
             Linux CPU  OP-TEE  DMA Engine
```

即使 CPU-CPU Cache 是一致性的，DMA Engine 也不一定处于同一个一致性域。

并且：

PMP 对 CPU S/U-mode 的约束，不自动保证普通 DMA Master 受到相同访问限制。

必须额外确认 IOMMU/IOPMP/Firewall 等机制如何约束 DMA 地址、设备权限及安全归属。

从安全角度，需要回答：

> Linux 能否配置一个 DMA Engine，直接读取 OP-TEE 的 Secure DRAM？

如果可以，那么即使 OP-TEE 的页表和 PMP 配置完全正确，系统的 TEE 隔离仍然是不完整的。

# 17.6 RISC-V Memory Model 与 TEE Synchronization

## 17.6.1 为什么 RISC-V SMP 必须理解 RVWMO？

RISC-V 默认采用 RVWMO（RISC-V Weak Memory Ordering）。

它允许处理器在不违反架构规定的情况下，对不同内存访问采用较灵活的排序。

因此：

```
data = 0x1234;
ready = 1;
```

不能在缺乏同步的情况下，简单认为另一个 hart 只要观察到 `ready == 1`，就一定已经正确观察到 `data == 0x1234`。

RISC-V ISA 明确规定了 FENCE、Acquire/Release、Atomic 及其他约束对内存顺序的作用。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



## 17.6.2 Message Passing 的典型案例

假设两个 Hart 通过共享内存通信：

```
Hart 0 / Producer         Hart 1 / Consumer

payload = 42             while (!ready)
ready = 1                    ...

                         result = payload
```

在没有适当同步的情况下，Consumer 的观察行为不能仅凭源码先后顺序推断。

正确设计可以使用 C11 Atomic：

```
#include <stdatomic.h>

static int payload;
static atomic_int ready = ATOMIC_VAR_INIT(0);

void producer(void)
{
    payload = 42;

    atomic_store_explicit(
        &ready, 1, memory_order_release);
}

int consumer(void)
{
    while (!atomic_load_explicit(
               &ready, memory_order_acquire)) {
        /* cpu_relax(); */
    }

    return payload;
}
```

这里假设 `payload` 仅通过这个发布协议访问，没有其他并发写入，并且双方处于支持该原子内存模型的共享一致性域。

`release` 保证 Producer 在发布 `ready` 之前的相关内存写入，能够与 Consumer 的 `acquire` 建立同步关系。

在汇编层面，Acquire/Release 可能由 AMO/LR-SC 的 `aq`、`rl` 位或适当的 FENCE 指令实现，具体取决于目标 ISA、编译器和操作宽度。

## 17.6.3 `volatile` 不能替代 Memory Barrier

常见错误：

```
volatile int ready;
volatile int payload;
```

`volatile` 并不是多核并发协议。

它不能独立保证：

- 对共享变量的原子性。
- CPU 间的同步关系。
- 正确的 Acquire/Release。
- Race-Free 的 C Memory Model 行为。

`volatile` 的主要用途与可观察访问有关，例如某些 MMIO 场景；对普通共享内存同步，应使用正确的 Atomics 和 Locking Primitive。

## 17.6.4 五类容易混淆的 Fence

| 指令/机制             | 主要用途                 | 不负责什么                     |
| ----------------- | -------------------- | ------------------------- |
| `FENCE rw,rw`     | 对规定的普通内存访问建立顺序       | 不直接刷新所有 Cache             |
| `FENCE iorw,iorw` | 包含设备 I/O 访问的排序       | 不决定设备的安全权限                |
| `FENCE.I`         | 同步当前 hart 的指令获取与相关存储 | 不自动完成其他 hart 的 I-Cache 同步 |
| `SFENCE.VMA`      | 页表更新与当前 hart 地址转换的同步 | 不自动使其他 hart 的 TLB 失效      |
| SBI Remote Fence  | 请求其他 hart 执行相应 Fence | 不自动解决 DMA Cache Coherency |

还需要记住，`SFENCE.VMA` 不是普通的 `FENCE`，它针对的是地址转换结构与隐式 Page Table Walk，而不只是普通数据 Load/Store 顺序。

## 17.6.5 TEE 中特别需要 Memory Ordering 的位置

在实际 OP-TEE 和 OpenSBI 中，我建议优先检查：

- Trusted Thread State 从 FREE 转成 ACTIVE 时。
- Suspended Thread 被另一个 Hart 恢复时。
- Per-Hart Boot State 初始化及发布时。
- Page Table Entry 更新后。
- Domain Security Policy 修改后。
- Shared Memory Ownership 发生变化时。
- IPI/Interrupt Doorbell 发送前。
- 释放或回收 Secure Memory 前。

这些场景都有一个共同特点：

先修改某些共享状态，然后需要另一个执行主体正确观察这些状态。

而这正是 Memory Ordering 和同步协议要解决的问题。

# 17.7 Page Table Modification 与 Remote TLB Shootdown

本章最值得深入的技术专题

如果只能从 Chapter 17 中挑选一个主题深入到能够提交 OP-TEE 核心 Patch 的程度，我会优先推荐 SMP TLB Shootdown。

因为它同时涉及：

- RISC-V ISA。
- OP-TEE MMU。
- OpenSBI RFENCE。
- SMP Synchronization。
- TA Address Space Isolation。
- Memory Lifetime。
- Security Boundary。

## 17.7.1 首先理解问题的本质

假设 Hart 3 和 Hart 7 都运行 OP-TEE。

它们共享一份 Page Table。

```
                     Page Table
                       |
         +-------------+-------------+
         |                           |
      Hart 3                       Hart 7
         |                           |
       TLB 3                       TLB 7
```

假设地址：

```
VA = 0x4000_0000
```

原来映射到：

```
PA = 0x9000_0000
Permission = RW
```

现在 Hart 3 希望撤销这个 Mapping：

```
PTE.V = 0
```

问题来了：

Hart 3 修改内存中的 PTE，不意味着 Hart 7 已经缓存的 TLB Entry 自动被删除。

因此可能出现：

```
Hart 3                           Hart 7

PTE.V = 0
   |
SFENCE.VMA
   |
Mapping removed locally         Old TLB Entry
                                      |
                                      v
                               Access old PA
```

对 Hart 7 而言，旧的地址转换信息可能仍然被使用。

如果这块物理内存已经分配给另一个 TA，那么这就不仅是一个普通的 Page Fault Bug，而是潜在的跨 TA 访问问题。

## 17.7.2 `SFENCE.VMA` 为什么不够？

RISC-V `SFENCE.VMA` 只对执行该指令的 hart 建立相应的地址转换同步效果。

因此：

```
sfence.vma
```

在 Hart 3 上执行，并不能直接代替 Hart 7 的 TLB Invalidation。

如果多个 hart 可能持有受影响的旧映射，就需要 Remote Shootdown。

对应的概念流程：

```
Hart 3
  |
  | Update PTE
  | Publish page-table changes
  v
Local SFENCE.VMA
  |
  | Remote fence request
  v
OpenSBI
  |
  +------------> Hart 7
  |                 |
  |             SFENCE.VMA
  |                 |
  |             Completion
  |                 |
  <-----------------+
  |
  v
Safe to complete
the invalidation protocol
```

RISC-V SBI RFENCE Extension 提供了：

```
sbi_remote_sfence_vma(...)
sbi_remote_sfence_vma_asid(...)
```

以允许 Supervisor Software 请求目标 Hart 执行远程地址转换同步。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



严格来说，实际安全协议还要确认 RFENCE 的完成语义、PTE 写入的可见性、目标 Hart 生命周期，以及是否有无法通过该请求覆盖的执行主体。

## 17.7.3 从真实 OP-TEE 源码分析

当前 RISC-V OP-TEE 的：

`core/arch/riscv/mm/core_mmu_arch.c`

包含了：

```
tlbi_all()
tlbi_va_allasid()
tlbi_asid()
tlbi_va_asid()
tlbi_va_range()
tlbi_va_range_asid()
```

以及：

```
tlbi_remote(...)
```

其中 `tlbi_remote()` 在 SBI 环境且 Core 数大于 1 时，通过 RFENCE 请求其他可访问 hart 执行 Remote Invalidation。

典型调用关系：

```
tlbi_va_range()
     |
     +-- mb()
     |
     +-- Local TLB Invalidation
     |
     +-- tlbi_remote()
              |
              +-- sbi_remote_sfence_vma()
                        |
                        +-- SBI ECALL
                                  |
                                  v
                                OpenSBI
     |
     +-- mb()
```

当前实现使用 `SBI_HART_MASK_BASE_ALL` 针对调用 Supervisor 所有可访问的 hart 进行 Remote Fence；如果 SBI 调用返回失败，相关代码会记录错误并 Panic。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 为什么出现错误时选择 Panic？

考虑下面的情况：

1. Hart 3 已经修改了 PTE。
2. Hart 7 的 Remote TLB Invalidation 失败。
3. 系统继续运行。
4. Hart 7 仍可能使用旧的权限或地址转换信息。

如果此时继续把相关物理页面分配给其他 TA，就可能破坏内存隔离。

因此，对于不能安全恢复的 TLB 同步失败，直接停止受影响的可信执行环境可能比继续运行更合理。

但注意，Panic 并不自动构成完整的安全恢复机制。真实平台还必须定义 Panic 后的处理，包括敏感资源失效、密钥清理、复位和 SoC 安全状态。

## 17.7.4 为什么还需要 ASID？

RISC-V `satp` 包含 ASID（Address Space Identifier）。

ASID 用于区分不同地址空间中的 Translation Entry。

假设：

```
TA-A  ASID = 3
TA-B  ASID = 4
```

两者都使用：

```
VA = 0x40000000
```

但各自映射到不同 PA。

如果 TLB Entry 正确按 ASID 区分，就可以减少不同地址空间切换时需要执行的全局刷新。

RISC-V 提供：

```
sfence.vma rs1, rs2
```

其中：

- `rs1` 可以限定 Virtual Address。
- `rs2` 可以限定 ASID。
- `x0` 有相应的全范围含义。

例如：

```
li t0, 0x40000000
li t1, 3

sfence.vma t0, t1
```

表示针对给定 VA 和 ASID 进行本地地址转换同步。

这里还存在两个高级问题：

Global Mapping

如果页表使用了 `PTE.G`，ASID-specific 的 Fence 不会处理相应的 Global Translation。修改全局映射时必须选择适当的 Invalidation Scope。

ASID Reuse

如果一个 ASID 曾属于 TA-A，后来分配给 TA-B，就必须保证旧地址转换信息不会错误地被 TA-B 继承。

## 17.7.5 从安全角度理解 TLB Shootdown

考虑以下执行时序：

```
Hart 3                           Hart 7
  |                                |
  |  TA-A exits                    |
  |                                |
  |  Remove TA-A mappings          |
  |                                |
  |  Free physical pages           |
  |                                |
  |                           Old TLB mapping
  |                                |
  |  Allocate pages to TA-B        |
  |                                |
  |                           Access old VA
  |                                |
  v                                v
              Security Risk
```

真正危险的是：

Memory Reuse 发生在所有相关旧访问路径都已失效之前。

因此需要建立一个完整的安全顺序：

```
Stop or synchronize users of old mapping
          |
Modify page tables
          |
Publish page-table updates
          |
Local and remote translation synchronization
          |
Confirm required completion
          |
Ensure no outstanding unauthorized access
          |
Release or reuse physical memory
```

其中最后的 outstanding access 问题还需要根据平台的访存和硬件一致性特性分析。

这与 Linux 的 Page Table Tear-down / TLB Gather 等机制在设计思想上有相似之处，但 TEE 的失败后果可能涉及直接的安全隔离破坏。

## 17.7.6 一个高级问题：目标 Hart 已经退出 OP-TEE Domain 怎么办？

假设 Hart 7 在 TA-A 执行期间曾经缓存过 TA-A 的 TLB Entry。

随后 Hart 7 离开 OP-TEE Domain，返回 Linux。

Hart 3 此时删除了 TA-A Mapping，但 Remote RFENCE 的目标列表只覆盖当前属于 OP-TEE Domain 的 hart。

那么：

Hart 7 将来重新进入 OP-TEE Domain 时，是否可能使用旧的 TLB Entry？

答案不能仅凭“当前不属于 Domain”来判断安全。

这里需要分析：

- Domain Exit 是否执行了必要的地址转换同步。
- 再进入时是否重新设置 `satp`。
- ASID 是否会被复用。
- 旧的 TLB Entry 是否可能在再次进入时匹配。
- OpenSBI 是否保证 Domain Switch 时完成相应的 Fence。
- RFENCE 与 Hart 归属变化之间是否存在竞态。

在当前 OpenSBI 的 Domain Context Switch 实现中，硬件保护重新配置路径也负责执行必要的 Translation Fence。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



但在 Staff-level Code Review 中，应进一步检查其具体顺序和覆盖范围，而不能只因为存在一个 `sfence.vma` 就认为所有竞态已被排除。

# 17.8 Hart Start/Stop、IPI 与 SBI HSM

## 17.8.1 谁负责启动 Secondary Hart？

在典型 RISC-V 系统中：

```
Boot Hart
    |
    v
OpenSBI Coldboot
    |
    v
Primary S-mode Payload
    |
    | SBI HSM hart_start()
    v
OpenSBI
    |
    v
Secondary Hart Entry
```

需要区分三个动作：

Hart Power/Execution State Management

由平台电源控制和相应固件机制实现。

SBI HSM

为 Supervisor Software 提供标准化的 Hart State Management 接口。

OP-TEE Secondary Initialization

在 Secondary Hart 获得执行权后，完成 OP-TEE 专属的 per-hart 初始化。

不能把这三个层次理解为同一个模块。

## 17.8.2 OP-TEE 的真实 Secondary Hart 启动代码

当前 RISC-V OP-TEE：

`core/arch/riscv/kernel/boot.c`

包含：

```
boot_start_secondary_cores()
```

其中核心逻辑是：

```
rc = sbi_hsm_hart_get_status(hartid, &status);

if (rc || status != SBI_HSM_STATE_STOPPED)
    continue;

rc = sbi_hsm_hart_start(hartid, start_addr, 0);
```

这段代码还会统计真正启动的 Secondary Hart 数量，并据此更新 Boot Synchronization 所需的 Hart 数。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这比假设所有配置中的 Hart 都一定已经启动要安全。

### 为什么要检查 HSM State？

因为一个 Hart 可能处于：

- STARTED。
- STOPPED。
- START_PENDING。
- STOP_PENDING。
- SUSPENDED / RESUME_PENDING 等相关状态。

不能向任意状态的 Hart 无条件执行 Hart Start。

而且在 Domain 环境中，HSM 操作还受到 Domain Access Control 约束。

因此对于一个 Hart：

```
Hart 7
```

需要分别问：

1. 它是否存在？
2. 它是否属于本 Domain 的 possible_harts？
3. 它当前是否分配给本 Domain？
4. 它当前的 HSM State 是什么？
5. 当前调用者是否有权执行 Hart Start？

这五个问题不能合并为一句“Hart 7 在线吗”。

## 17.8.3 Secondary Hart 必须初始化哪些状态？

一个典型 OP-TEE Secondary Hart Bring-up 需要考虑：

```
Secondary Hart Entry
       |
       v
Identify Hart ID
       |
       v
Map to OP-TEE Core Index
       |
       v
Initialize Per-Hart Stack
       |
       v
Initialize Core Local Data
       |
       v
Configure MMU / satp
       |
       v
Configure Trap Vector
       |
       v
Initialize Interrupt State
       |
       v
Join Synchronization Protocol
       |
       v
Ready to execute Trusted Work
```

一些全局资源只应由 Primary Hart 初始化一次。

而 per-hart 资源通常需要每个参与的 Hart 自己初始化。

### 一个常见的 SMP Boot Bug

假设：

```
global_state = READY;
```

在实际共享数据尚未正确初始化或发布前被 Secondary Hart 观察到。

那么 Secondary Hart 可能过早访问：

- 未建立完整的 MMU Mapping。
- 尚未初始化的 Thread Pool。
- 未配置的 Interrupt Vector。
- 尚未建立的 Heap/Memory Pool。

因此 Boot Barrier 的正确性不仅是计数器正确，还涉及状态发布和内存顺序。

## 17.8.4 IPI：多 Hart 协调机制

IPI（Inter-Processor Interrupt）用于不同 Hart 之间的协调。

典型用途包括：

- 请求其他 Hart 处理某项工作。
- SMP Reschedule。
- Cross-CPU Function Call。
- TLB Shootdown 通知。
- 某些停机或状态协调操作。

在 RISC-V 中，普通 Supervisor Software 可以通过 SBI IPI Extension 请求相应的 IPI 服务。

但在 Domain 隔离环境中必须遵守 Domain Access Control。

例如：

```
Linux Domain Hart 0
          |
          | SBI IPI
          v
OpenSBI
          |
          +--> Linux Domain Hart 1  (authorized)
          |
          +--> TEE Domain Hart 7    (not generally allowed)
```

因此如果 Linux 想通知 TEE Domain，不能默认通过普通 SBI IPI 任意控制 TEE Hart。

应使用经过授权的 TEE Transport、Domain-Aware Interrupt 或相应的固件协议。

## 17.8.5 IPI 与 Remote Fence 的区别

IPI 是一种通知机制。

Remote Fence 是具有明确地址转换或指令同步语义的远程操作。

两者常常组合使用，但语义不同。

例如：

```
Hart 0:
    request remote TLB flush

OpenSBI:
    send notification to target hart

Hart 7:
    execute requested fence

OpenSBI:
    synchronize completion
```

不能认为“IPI 已经发出”等同于“目标 Hart 已经完成所有要求的地址转换同步”。

在分析实现时，需要确认 RFENCE 请求和完成协议的具体语义，以及异常、离线和超时路径。

# 17.9 CPU Hotplug / Suspend / Resume 对 TEE 的影响

## 17.9.1 为什么 Hotplug 比冷启动更复杂？

冷启动时，系统通常具有相对清晰的初始化顺序。

Hotplug 则不同：

一个 Hart 可能正在执行 TA，或者刚好处于 Domain Switch 中，又或者持有尚未处理完的 IPI。

假设 Linux 希望 Offline Hart 7：

```
Hart 7
    |
    +-- currently running Linux?
    |
    +-- currently executing OP-TEE?
    |
    +-- suspended Trusted Thread?
    |
    +-- outstanding TLB shootdown?
    |
    +-- owns a global lock?
```

这些状态对 Hotplug 是否安全有直接影响。

## 17.9.2 正确的 Hart Offline 设计

我建议从状态机出发理解：

```
ONLINE
   |
   v
QUIESCING
   |
   +-- Reject new work
   +-- Drain current operations
   +-- Complete pending synchronization
   +-- Release owned locks
   +-- Handle or migrate suspended work
   |
   v
QUIESCED
   |
   v
HSM STOP / SUSPEND
   |
   v
OFFLINE
```

这是一种参考设计状态机，不是当前 OP-TEE 已实现的统一 Hotplug 接口。

其中最重要的是 Quiescing。

不能在一个 Hart 尚未退出关键安全执行路径时，直接把它视为 Offline。

## 17.9.3 Trusted Thread Migration

假设：

```
Thread 2:
    Suspended on Hart 7
```

此时 Hart 7 Offline。

如果恢复请求后来由 Hart 3 发起，只要相关 Thread Context 能够迁移、地址空间与寄存器状态正确恢复，就有可能在 Hart 3 继续执行。

当前 OP-TEE 已有跨 Hart RPC Resume 的相关处理。

但这不代表所有外设状态、Interrupt State、Per-Hart Hardware Feature 都可以无条件迁移。

例如：

- Thread Context 中是否保存了错误的 Per-Hart Pointer？
- TA 是否依赖只在 Hart 7 上存在的硬件能力？
- Local Interrupt Routing 是否需要重新配置？
- FP/Vector Capability 是否在目标 Hart 上等价？
- 目标 Hart 是否具有相同的 MMU/ISA 扩展能力？

最后一项在异构 RISC-V SoC 中尤其值得关注。

## 17.9.4 Suspend / Resume 还涉及硬件安全状态

假设 SoC 进入 Deep Sleep：

```
OP-TEE Domain
    |
    v
System Suspend
    |
    +-- CPU power down
    +-- SRAM retention?
    +-- PMP registers retained?
    +-- Firewall state retained?
    +-- IOMMU/IOPMP retained?
    |
    v
System Resume
```

这里必须明确哪些状态可以保留，哪些会丢失。

如果 PMP 或 Firewall 在 Resume 后恢复到了宽松的默认权限，而 Linux 在安全配置重新建立前开始执行，就可能出现安全窗口。

因此正确的 Resume 顺序应保证：

硬件隔离策略重建完成后，才能让低信任执行环境重新访问相关资源。

对于涉及独立 HSM/RSS 的 SoC，还需要定义由谁负责验证恢复状态、重新配置安全硬件，以及是否需要重新建立设备密钥句柄和安全会话。

# 17.10 Race、Deadlock、Livelock 与 Worst-case Latency

这节更接近真实工业项目的故障分析。

## 17.10.1 Race Condition

Race 不是简单的“两个 Hart 同时访问同一个变量”。

真正的问题是：

多个操作之间缺乏必要的同步，导致结果依赖执行时序，并可能违反系统不变量。

例如：

```
Hart 3                     Hart 7

Thread 2 = SUSPENDED
                           Thread 2 = SUSPENDED

Resume Thread 2
                           Resume Thread 2

Thread 2 = ACTIVE
                           Thread 2 = ACTIVE
```

如果检查和状态转换不是原子操作，就可能发生 Double Resume。

OP-TEE 目前通过 Global Thread Lock 保护相应状态转移。

## 17.10.2 Deadlock：跨 Domain 的特殊情形

经典双锁死锁：

```
Hart 3 / OP-TEE             Hart 7 / OP-TEE

lock(A)                     lock(B)

lock(B)                     lock(A)

wait forever                wait forever
```

这是 SMP Kernel 的常见问题。

但是 TEE 还可能出现更隐蔽的跨 Domain Deadlock。

例如一个错误设计：

```
OP-TEE:
    acquire secure_lock
          |
          v
       RPC to Linux
          |
          v
       Wait for response

Linux:
    process RPC
          |
          v
       Invoke OP-TEE
          |
          v
       acquire secure_lock
          |
          v
       Blocked
```

形成循环依赖：

```
OP-TEE waits for Linux
        ^
        |
        v
Linux waits for OP-TEE
```

这里的根本问题是：在持有不能跨越 RPC 等待的锁时进入了跨 Domain 阻塞路径。

因此在设计 OP-TEE 子系统时，应该明确：

- 哪些锁允许跨函数保持。
- 哪些锁不能跨 RPC。
- 哪些路径允许 Thread Suspend。
- 哪些路径需要等待 REE 响应。
- 是否存在 REE 回调或重新进入 TEE 的可能。

## 17.10.3 Livelock

Livelock 与 Deadlock 的区别是：

Deadlock 中，相关执行主体通常无法取得进展。

Livelock 中，执行主体可能一直运行并不断重试，但业务状态始终无法前进。

例如：

```
Hart 3:
    Try acquire resource
    Conflict
    Retry

Hart 7:
    Try acquire resource
    Conflict
    Retry
```

如果双方不断互相退让，并且没有公平性或退避策略，就可能出现活锁。

在多核 TEE 中，Livelock 可能导致：

- 大量 CPU 时间消耗。
- TEE Request 长时间没有响应。
- Watchdog 超时。
- Secure Service 不可用。

## 17.10.4 Worst-case Latency

Staff Engineer 不能只关心平均性能。

对于安全服务，需要区分：

- 平均 TEE Invoke Latency。
- P95 / P99 Latency。
- Worst-case Latency。
- Domain Switch Overhead。
- Lock Contention。
- Remote TLB Shootdown。
- RPMI/MPXY Transport。
- RPC 等待时间。

可以把同步 TEE 请求的延迟近似分解为：

\\[ T\_{\text{invoke}} = T\_{\text{transport}} + T\_{\text{domain-switch}} + T\_{\text{thread-wait}} + T\_{\text{TA}} + T\_{\text{RPC}} + T\_{\text{return}} \\]

各部分又受到 SMP 竞争、Cache、Interrupt 和硬件资源占用影响。

这不是严格固定的实际调用路径公式，而是性能分析模型；一次 Invoke 可能包含多次 Domain Switch 和多次 RPC。

### 一个具体性能案例

假设某个 Key Derivation 请求在低负载下平均耗时很短。

在 16-Hart 高并发情况下却出现明显的 P99 Latency 尖峰。

可能原因：

1. Trusted Thread Pool 资源不足。
2. 全局 Lock Contention。
3. Remote TLB Shootdown 范围过大。
4. 其他 Hart 长时间无法处理 Remote Request。
5. Secure Storage 资源串行化。
6. Domain Switch 或 HSM 管理开销。
7. RPMI/MPXY 或 REE RPC 的队列拥塞。

因此不能简单地认为“多加 Trusted Thread”一定解决性能问题。

如果瓶颈是一个全局 Spinlock，增加 Thread 数可能反而提高竞争程度。

## 17.10.5 工业项目中的调试策略

我建议把性能和并发事件分层记录：

| 层次       | Trace 内容                                   |
| -------- | ------------------------------------------ |
| Linux    | CA 调用开始/结束、TEE Driver 请求与等待                |
| OpenSBI  | ECALL、Domain Enter/Exit、IPI、RFENCE         |
| OP-TEE   | Thread Alloc/Suspend/Resume、Lock Wait      |
| TA       | Session、Command、Syscall、Service Time       |
| Hardware | Interrupt、Cache Miss、Memory/Bus Contention |

同时必须避免 Trace 本身引入新的安全问题：

- 不记录密钥和原始敏感 Buffer。
- 不随意打印受保护物理地址。
- Trace Buffer 不能被不可信域任意访问。
- 不在持锁的关键路径引入高延迟的阻塞式日志。
- 要考虑各 Hart Timestamp 的时钟同步和时序可比较性。

# 17.11 跨 Domain 状态泄露与并发安全验证

这是整章的最后一项，也是将普通 SMP Kernel 开发提升为 Security Architecture 的关键。

## 17.11.1 什么是 Cross-Domain State Leakage？

我们先定义：

Linux Domain 和 OP-TEE Domain 在安全设计中属于不同的信任边界。

一个 Domain 的私有状态不应该在没有授权的情况下被另一个 Domain 读取或控制。

这些状态不仅仅包括内存。

还包括：

- GPR 中的敏感值。
- FP/Vector Register。
- CSR。
- TLB 和相关地址转换状态。
- PMP / Hardware Protection State。
- Interrupt State。
- Debug/Trace/Performance Counter 配置。
- Shared Hardware Engine 的状态。
- Cache 和其他微架构资源所反映的间接信息。

## 17.11.2 Register Leakage

考虑：

```
OP-TEE:
    v0 contains sensitive data

Domain Switch

Linux:
    reads v0
```

如果 Domain Switch 没有正确处理 Vector Register，就可能泄露敏感内容。

这是一个直接的 Architectural State Leakage。

对应的测试应当：

1. 在 OP-TEE 中为相关寄存器写入可识别的测试模式。
2. 触发 Domain Switch。
3. 检查 Linux 看到的是否仅是其自身合法的寄存器状态。
4. 再切回 OP-TEE，确认受保护状态是否正确恢复。
5. 在多个 Hart、高频切换和异常路径下重复。

不应仅测试正常的显式 Domain Return，还需要覆盖 Interrupt-driven Exit、异常退出等路径。

## 17.11.3 TLB Leakage

TLB 问题与普通 Register Leakage 略有不同。

如果 TLB 中残留了旧 Domain 的转换，但新的 Domain 使用不同 `satp`、ASID 和硬件保护权限，那么残留并不必然直接形成可访问漏洞。

真正需要检查的是：

- 是否可能错误匹配旧 Translation。
- Global Mapping 是否正确处理。
- ASID 是否安全复用。
- PMP 是否正确隔离。
- 旧 Mapping 是否在必要 Fence 前被重新使用。

因此不能简单地把“TLB 存在旧 Entry”等同于“Linux 一定可以访问 Secure Memory”。

必须构造完整的可达性和权限分析。

## 17.11.4 Interrupt State Leakage

例如，OP-TEE 使用一个安全设备，其 Interrupt 属于 TEE Domain。

如果 Domain Switch 后 Linux 意外继承了不应该看到的 Interrupt Pending/Enable 状态，就可能出现：

- 非法 Interrupt Delivery。
- Secure Device 的运行信息泄露。
- Interrupt Routing 错误。
- Interrupt Storm。
- 以中断机制构建的 DoS。

具体风险取决于 APLIC/IMSIC/PLIC 的实现及硬件隔离模型。

这里尤其需要审查：

Interrupt Ownership 是否跟随 Domain，而不是仅仅跟随当前 Hart？

因为一个 Hart 可以在不同时间运行不同 Domain，但设备的安全所有权不应因此被任意改变。

## 17.11.5 PMP 与硬件权限残留

这是高风险问题之一。

假设：

```
Hart 3:
    OP-TEE Domain
       |
       | PMP allows Secure DRAM
       v
    Domain Switch
       |
       | PMP not reconfigured correctly
       v
    Linux Domain
```

如果 Linux 在错误的 PMP 权限下继续执行，就可能访问本不应该开放的物理内存。

因此必须验证：

1. Domain Switch 前后的 PMP 配置。
2. 权限变化时的同步要求。
3. 中断/异常是否可能打断保护更新。
4. 失败路径是否会暴露中间状态。
5. 多 Hart 同时切换时是否会出现共享策略冲突。

## 17.11.6 微架构侧信道不是普通 Context Save 能完全解决的

还有一个更深入的安全问题：

即使 GPR、FP/Vector、CSR 和 PMP 全部正确恢复，Cache、Branch Predictor 等微架构资源仍可能保留历史运行信息。

它们可能形成侧信道。

但是应避免夸大：

并不是只要两个 Domain 共用 Cache，就一定存在可以直接读取秘密的漏洞。

是否可利用取决于：

- 攻击者能够执行哪些操作。
- Cache/预测器架构。
- 时间测量能力。
- 是否共享物理缓存资源。
- 是否存在秘密相关的访问模式。
- Firmware/Hardware 是否具有隔离或缓解机制。

对 Staff Engineer 来说，需要建立完整的 Threat Model，而不是只根据是否执行了 Cache Flush 判断 TEE 是否安全。

## 17.11.7 设计一套 SMP Domain Security Validation Matrix

我建议将测试分成五类。

| 测试维度       | 测试场景                     | 期望结果                    |
| ---------- | ------------------------ | ----------------------- |
| Register   | FP/Vector 模式值跨 Domain 切换 | 不能观察到其他 Domain 的私有寄存器状态 |
| MMU/TLB    | 删除映射并跨 Hart 访问           | 不得使用已撤销权限访问受保护页面        |
| Domain/PMP | Linux 越权访问 TEE DRAM      | 被硬件拒绝                   |
| Interrupt  | 非法跨 Domain IPI/IRQ       | 被拒绝或按授权策略路由             |
| Lifecycle  | Hotplug、Suspend、Reset    | 不产生安全配置丢失窗口             |

测试应该覆盖不同调度顺序、多个 Hart 并发和异常路径。

通过 QEMU 测试的软件路径仍需要在真实 RTL/仿真/硅片上补充验证，因为 QEMU 未必准确建模实际 Cache、Fabric、Interrupt Controller 和 SoC Firewall 行为。

# 17.12 综合实战：在 QEMU 和真实 SoC 上验证 SMP OP-TEE

前面 11 节已经建立了完整的架构模型。这一节将这些知识组合成可执行的工程实验。

建议使用：

- RV64 QEMU virt，配置 2～16 个 hart。
- OpenSBI Domain + OP-TEE 的兼容开发基线。
- Linux TEE Driver、OP-TEE Client 和 xtest。
- GDB、反汇编及内核 Trace。
- 真实 SoC 或 RTL Emulation 进行硬件特定的安全验证。

环境可参考 [RISE RP016 Build Instructions](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/build/?utm_source=chatgpt.com)。具体 QEMU 参数和固件镜像要与选定的 OpenSBI/OP-TEE 分支匹配。

## Experiment 1 — 分析 Trusted Thread 的跨 Hart 恢复

实验目标： 证明 Trusted Thread Context 与 Per-Hart Context 并不是相同的对象。

首先定位关键代码：

```
cd optee_os

rg -n \
  "thread_resume_from_rpc|thread_state_suspend" \
  core/arch/riscv

rg -n \
  "thread_get_core_local|thread_lock_global" \
  core/arch/riscv
```

在调试器中设置断点：

```
set pagination off

break thread_state_suspend
break thread_resume_from_rpc
break thread_state_free

continue
```

通过多线程 CA 压力测试和能够触发 RPC 的 TA 操作，尝试观察跨 Hart 恢复。

记录：

| 字段           | Suspend       | Resume        |
| ------------ | ------------- | ------------- |
| Hart ID      | H1            | H2            |
| Thread ID    | T             | T             |
| Thread State | SUSPENDED     | ACTIVE        |
| Kernel SP    | Saved SP      | Restored SP   |
| `tp`         | H1 Core Local | H2 Core Local |
| TA Mapping   | Saved         | Restored      |

其中 H1 和 H2 可以相同，也可能不同。不能假设普通测试必然触发跨 Hart 迁移，需要通过调度和压力条件提高覆盖率。

成功判据： 在真实发生跨 Hart Resume 时，同一个 Thread 的安全执行上下文得到正确恢复，且 Kernel Per-Hart Pointer 对应当前 Hart。

## Experiment 2 — RISC-V Remote TLB Shootdown

实验目标： 验证删除 Mapping 后，其他 hart 不会继续使用过期的地址转换。

首先阅读：

```
rg -n \
  "tlbi_remote|tlbi_va_range|tlbi_asid" \
  core/arch/riscv/mm

rg -n \
  "sbi_remote_sfence_vma" \
  core/arch/riscv
```

测试思路：

```
Hart 3                      Hart 7

Create Mapping
                              |
                              v
                         Access Mapping
                              |
Remove Mapping                |
      |                       |
Remote TLB Shootdown ------>  |
      |                       v
      |                  Invalidate TLB
      |                       |
Reclaim Page                  |
                              |
                         Attempt Access
                              |
                              v
                        Access must fail
```

请注意，真正的测试必须在 OP-TEE 内部或受控的测试 TA 中建立合适的可访问映射；不能用 REE 用户空间直接修改 TEE 页表来代替。

建议设计两组测试：

Case A： 正常执行 Remote TLB Invalidation，验证撤销权限生效。

Case B： 在独立、可重置的测试分支中故意跳过 Remote Invalidation，用于研究旧 Entry 的可能影响。

第二组只能在隔离实验环境中进行；并且即使跳过 Remote Fence，也不能保证 QEMU 一定重现真实硬件上的 Stale TLB 行为。

## Experiment 3 — Spinlock Contention 和 Trusted Thread Pool

设计一个需要访问共享 TA 状态的测试服务。

让不同数量的 Linux Client Thread 同时调用：

```
1 Client
2 Clients
4 Clients
8 Clients
16 Clients
32 Clients
```

并记录：

- 请求吞吐量。
- 平均延迟。
- P95/P99 延迟。
- Trusted Thread 分配失败或等待情况。
- Global Lock Contention。
- CPU 利用率。

重点观察：

当 Client 并发数量超过 Trusted Thread Pool 的能力时，性能如何变化？

尝试在可控实验中分别改变 `CFG_NUM_THREADS` 和可参与 OP-TEE 的 Hart 数量，观察它们对性能的不同影响。

实验不应假定两种参数具有简单线性关系。

## Experiment 4 — Domain State Leakage Test

这个实验更贴近 Security Architecture。

在实验固件中构造：

```
OP-TEE Domain:
    GPR = Pattern A
    FP  = Pattern B
    RVV = Pattern C

         Domain Switch

Linux Domain:
    Validate its own architectural state

         Domain Switch

OP-TEE Domain:
    Validate restored Pattern A/B/C
```

为了避免测试自身的 ABI 调用和编译器优化破坏模式值，需要用受控汇编测试例程和明确的调用约定实施。

随后增加：

- 高频 Domain Switch。
- 不同 Hart 同时切换。
- Interrupt-triggered Switch。
- RPC/Resume。
- FP/Vector 混合负载。

最终形成一份 Security State Preservation Test Report。

对于准备向 OP-TEE upstream 提交核心 Patch 的工程师，这类带有明确安全不变量与可复现步骤的测试非常有价值。

# 17.13 Staff Engineer 级面试题与参考答案

以下 15 道题目全部采用工业场景形式。

这些题目不仅考察是否知道某个 API，更重要的是能否从架构、并发、安全和故障恢复多个维度给出可执行的设计。

## Problem 1 — 16-Hart SoC 上的 Domain 并发设计

场景：

一颗 RV64 SoC 有 16 个 hart。要求同时运行 Linux 和 OP-TEE，TEE 服务支持多个客户端并发请求。

架构团队提出两种设计：

- A：固定 4 个 Hart 专门执行 OP-TEE，其他 12 个 Hart 执行 Linux。
- B：16 个 Hart 都允许在 Linux 和 OP-TEE Domain 之间动态切换。

请分析各自的安全性、性能、调度复杂度和适用场景。

参考答案：

A 的优点是运行时 Domain 切换较少，容易建立较稳定的 Hart Ownership 和 Interrupt Routing。

缺点是资源利用率可能较低，而且 Linux 和 OP-TEE 的负载不一定平衡。

B 的优点是资源利用更加灵活，可以让不同 Linux Client 所在的 Hart 直接进入 OP-TEE，而不一定通过专用 CPU 执行。

但 B 对 Domain Context Switch、PMP、Interrupt、Per-Hart State、FP/Vector 保存和 SMP 同步提出更高要求。

还要考虑 Domain Switch 后 Linux 是否仍然认为该 Hart 在线、如何协调中断和调度语义，以及动态分配是否与 SBI HSM/IPI 权限兼容。

Staff-level 延伸： 应先确认硬件隔离是否支持不同 Hart 同时具有不同的安全权限。如果 SoC Firewall 只有全局开关，两种设计的并发安全性都可能受到限制。

## Problem 2 — Trusted Thread 在不同 Hart 上恢复时发生崩溃

场景：

OP-TEE Trusted Thread 先在 Hart 3 上执行，因 RPC 挂起，之后在 Hart 7 上恢复。系统只在跨 Hart Resume 时出现随机 Kernel Crash。

如何定位？

参考答案：

首先检查 Thread Context 中是否包含 Hart-local 的状态：

- `tp`。
- Per-Hart Core Local Pointer。
- `sscratch`。
- Kernel Stack 与当前 Thread 的关联。
- Trap Vector 和中断上下文。
- User Mapping 的恢复。

一个特别值得检查的点，是保存的 Kernel `tp` 是否仍然指向 Hart 3 的 Core Local。

当前 RISC-V OP-TEE 在 `thread_resume_from_rpc()` 中已有对 Kernel `tp` 重新绑定的处理。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



然后需要通过 Trace 关联：

```
Thread ID
Original Hart ID
Resume Hart ID
Saved SP
Restored SP
Kernel TP
User Mapping / ASID
```

不能简单地把跨 Hart Resume 全部禁用作为最终修复，因为这可能只是掩盖真实的状态管理问题。

## Problem 3 — 为什么仅保存 GPR 不足以完成 Domain Switch？

场景：

一位工程师实现了 M-mode Domain Switching，保存并恢复 `x1`～`x31`，随后使用 `mret` 返回目标 Domain。

部分基本程序可以运行，但复杂应用出现异常。

参考答案：

Domain Switch 不只是 GPR Context Switch。

还必须考虑：

- PC/Trap Return State。
- S-mode CSR。
- `satp` 和地址转换同步。
- `stvec`、`sscratch`。
- Interrupt Enable/Pending State。
- FP/Vector State。
- PMP/平台隔离权限。
- 可选 ISA 扩展引入的新增状态。

最重要的是安全权限必须与目标执行上下文一致。

如果已经切换到 Linux 的执行上下文，却仍允许访问 OP-TEE 私有物理内存，就可能发生严重的安全隔离破坏。

Staff-level 延伸： 需要检查哪些 CSR 是 Hart-local、哪些硬件配置是共享的，以及在异常中途发生时系统能否保证不会暴露错误权限。

## Problem 4 — PMP 是 Per-Hart 的，如何保证多 Hart Domain Isolation？

场景：

Hart 3 运行 OP-TEE，Hart 7 运行 Linux。

Hart 3 配置 PMP 允许访问 Secure DRAM，而 Hart 7 的 PMP 拒绝访问 Secure DRAM。

是否已经足够安全？

参考答案：

对于 CPU 的 S/U-mode 访存，正确配置的 PMP 可以提供相应的物理访问控制。

但完整的安全性还取决于：

- M-mode Firmware 是否可信。
- 是否存在可绕过 PMP 的 DMA Master。
- SoC Firewall 是否具有足够粒度。
- PMP Region 覆盖范围和优先级是否正确。
- Reset/Resume 时权限能否正确恢复。
- Shared MMIO、Debug 和其他资源是否受保护。

PMP 是重要的 CPU 访问控制机制，但不是整颗 SoC 的完整安全隔离方案。

## Problem 5 — 远程 TLB Shootdown 为什么仍然会失败？

场景：

Hart 3 修改 PTE 并调用 SBI RFENCE。

返回 Success 后，Hart 7 偶尔仍观察到不符合预期的映射行为。

请给出系统性的排查方案。

参考答案：

首先不要立即认定 RFENCE 实现有 Bug。

需要检查：

1. 修改的 PTE 是否是 Hart 7 实际使用的 Page Table。
2. PTE Store 是否正确发布。
3. Remote Fence 的 VA/ASID 范围是否正确。
4. 是否涉及 Global Mapping。
5. ASID 是否发生复用。
6. 目标 Hart 是否属于 RFENCE 的有效目标集合。
7. Domain Exit/Enter 与 RFENCE 是否发生竞态。
8. 是否有页面回收与重分配时序问题。
9. 是否混淆了 TLB 问题与 Data Cache Coherency 问题。

尤其需要验证 PTE 更新 → Remote Translation Synchronization → Memory Reuse 之间的顺序。

如果未经过充分同步就回收了物理页面，问题可能表现为随机访问错误，甚至跨 TA 隔离失效。

## Problem 6 — `fence rw,rw` 能否保证 DMA Cache Coherency？

场景：

TEE 使用一块 Shared Memory 与 Crypto DMA Engine 通信。

Engineer 在 Descriptor 写入后执行：

```
fence rw, rw
```

但 DMA Engine 偶尔读取旧 Descriptor。

参考答案：

`FENCE` 提供架构定义的访存排序，而不是通用 D-Cache Clean 操作。

如果 DMA Engine 不在 CPU 的硬件 Cache Coherent Domain 内，CPU 可能需要执行针对平台的 Cache Maintenance。

还要检查：

- Descriptor Memory Attribute。
- DMA Mapping API。
- Physical/IOVA 地址是否正确。
- Cache Line Alignment。
- Ownership Transfer。
- Doorbell MMIO 的排序。
- DMA Completion 后的 Invalidation。

Staff-level 延伸： 同时还应确认 DMA Engine 是否被硬件限制在授权的内存范围内。这是安全问题，不只是功能问题。

## Problem 7 — OP-TEE Global Lock 导致高并发性能下降

场景：

16-Hart 平台上，TEE Invoke 并发数从 4 增长到 16 后，吞吐量没有明显增加，P99 Latency 反而大幅上升。

如何分析？

参考答案：

首先分解执行路径，而不是直接提高 Thread Pool 数量。

需要区分：

- Thread Allocation Lock Contention。
- TA Session 级串行化。
- Secure Storage Lock。
- Shared Memory 管理。
- Domain Switch Overhead。
- RPC 资源拥塞。
- RFENCE/Cache Maintenance。
- 设备驱动串行执行。

建议记录 Lock Hold Time、Lock Wait Time、Concurrent Trusted Thread 数量和每次请求的 RPC 次数。

如果瓶颈集中在某个高竞争锁，可以考虑缩短临界区、将数据结构分片、使用 Per-Hart 缓存或重新设计同步范围。

但任何优化都不能破坏原来的全局不变量。

## Problem 8 — 在持有 Spinlock 时发生 Foreign Interrupt

场景：

Hart 3 正持有 OP-TEE Global Spinlock，此时发生需要返回 Linux 处理的 Foreign Interrupt。

该如何设计？

参考答案：

首先要分析发生中断的层级，以及它是否会立即导致 Trusted Thread Suspend 或 Domain Exit。

如果持锁上下文被挂起，而其他执行路径需要同一把锁继续处理请求，可能导致系统级死锁。

常用的设计原则是：

- 短临界区屏蔽可能导致不安全重入的中断。
- 不允许在持锁期间进行可能阻塞的 RPC。
- 保证锁的 Acquire/Release 遵守明确的 Interrupt State 约束。
- 如果存在不可屏蔽的高特权级事件，应保证其处理路径不重新获取被中断上下文持有的锁。

Staff-level 延伸： 要检查 M-mode 中断路径是否会访问与 OP-TEE 共享的数据结构，不能只分析 S-mode 的本地中断状态。

## Problem 9 — CPU Hotplug 与 TEE Request 冲突

场景：

Linux 正在 Offline Hart 7，但 Hart 7 上刚好执行了一个长时间 OP-TEE 操作。

系统应该怎么处理？

参考答案：

必须首先建立跨组件的 Quiesce Protocol。

需要确认：

- Hart 7 当前属于哪个 Domain。
- OP-TEE 是否存在 Active Thread。
- 是否允许新请求进入。
- 当前请求是否可以安全完成或迁移。
- 是否存在未完成的 IPI/RFENCE。
- 是否还持有 Global Lock。
- 何时可以安全执行 SBI HSM Stop。

不能简单地让 Linux 在看不到 Hart 7 的时候就认为整个安全侧已经完成停止。

如果硬件或固件没有提供完整的协调机制，最安全的工程选择可能是在初期明确禁用该 Hotplug 组合，并将其作为后续平台能力来实现。

## Problem 10 — 如何设计 OP-TEE SMP 的 Security Regression Test？

场景：

你负责 OP-TEE 在一颗新 RISC-V SoC 上的 Bring-up。架构团队要求证明 SMP 场景下没有明显的跨 Domain 状态泄露。

如何建立测试体系？

参考答案：

首先建立 Threat Model，然后按资源分类设计测试。

Architectural State

验证 GPR、FP/Vector、CSR 保存与恢复。

Memory Isolation

验证 PMP、MMU、TLB、ASID、Shared Memory 和 DMA Protection。

Interrupt Isolation

验证 IPI、External IRQ、Domain Ownership、Pending State。

Lifecycle

验证 Boot、Stop、Suspend、Resume、异常复位。

Concurrency

验证多 Hart 同时 Domain Switch、TA Invoke、RPC 和 Memory Reclaim。

每个测试必须有明确的失败判据，且应包括正常路径、边界条件和故障注入。

对于真实 SoC，需要把 QEMU 测试与 RTL Emulation、FPGA/Prototype、Silicon Validation 区分开来，因为模拟器不一定覆盖硬件一致性和安全 Firewall 行为。

## Problem 11 — ASID Reuse 引起跨 TA 数据错误

场景：

TA-A 使用 ASID 5。TA-A 退出后，系统立即把 ASID 5 分配给 TA-B。

TA-B 偶尔访问到与预期不一致的物理页面。

参考答案：

首先应检查 ASID 生命周期和 TLB Invalidation。

ASID 不直接代表一块固定物理地址空间。

当同一个 ASID 被用于新的地址空间时，如果旧的 TLB Entry 仍然有效，就可能造成错误 Translation。

需要检查：

- TA-A 页表拆除流程。
- ASID Free/Allocate 路径。
- Local/Remote TLB Invalidation。
- Global Mapping。
- Thread 迁移与历史 Hart 集合。
- 旧 Page Table 的回收时机。

需要保证新地址空间使用该 ASID 时，所有可能错误匹配旧 Translation 的执行路径都已得到正确处理。

## Problem 12 — 为什么 Eager FP/Vector Switch 可能比 Lazy 更适合 TEE？

场景：

一颗支持 RVV 的芯片需要在 Linux 和 OP-TEE 之间频繁切换。

性能工程师希望实现 Lazy Vector Context Switch 来减少开销。

你是否同意？

参考答案：

不应直接拒绝，也不应仅凭性能理由接受。

Lazy 方案需要可靠地知道 FP/Vector State 是否被使用和修改，并建立可验证的 Trap/Ownership 协议。

在跨 M/S Domain 的情况下，OpenSBI upstream 曾讨论过 Lazy 方案，但由于 S-mode 对 FS/VS 状态的影响以及 M-mode 对真实状态使用情况的可见性限制，最终采用 Eager 方案。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



Eager Switch 的主要优势是安全边界清晰、行为确定、验证成本较低。

缺点是可能增加切换成本，尤其在 VLEN 很大的实现上。

Staff-level 延伸： 如果要重新设计 Lazy 机制，需要证明完整的状态所有权、安全性、异常行为和性能收益，而不是只提供平均执行时间数据。

## Problem 13 — 多 Hart 同时更新硬件 Firewall

场景：

多个 Hart 在 Linux/OP-TEE Domain 之间动态切换。

SoC Firewall 通过一组全局 MMIO Register 控制对 Secure DRAM 的访问。

工程师提出使用一个 Global Spinlock 序列化 Firewall Register 写入。

这样是否足够？

参考答案：

不一定，甚至可能完全不能解决问题。

Spinlock 只能保证软件不会同时修改相关寄存器。

但如果 Firewall 的硬件策略是全局的，而 Hart 3 和 Hart 7 同时需要不同的访问权限，那么序列化配置更新不能表达两个不同的并发安全状态。

必须确认 Firewall 是否支持：

- Per-Master Permission。
- Domain ID / Security Tag。
- 按访问发起者识别的 Memory Protection。
- 多个权限配置并存。

如果硬件无法支持，就必须改变 Domain 并发模型，而不是继续增加软件锁。

这是一个典型的 Hardware/Software Co-Design 问题。

## Problem 14 — Domain Switch 平均很快，但 P99 Latency 很差

场景：

TEE Invoke 平均延迟满足要求，但高并发条件下偶尔发生严重抖动。

你如何建立性能模型？

参考答案：

需要把一次请求拆成：

```
Linux Request
    |
    +-- TEE Transport
    +-- M-mode Entry
    +-- Domain Switch
    +-- Trusted Thread Allocation
    +-- TA Execution
    +-- Optional RPC
    +-- Domain Return
    +-- Linux Completion
```

随后为每个阶段记录时间，进一步关联：

- Current Hart。
- Thread ID。
- Domain ID。
- Lock Contention。
- IPI/RFENCE。
- Cache Miss。
- Interrupt。
- RPC 次数。
- 设备队列延迟。

特别需要关注 TLB Shootdown 和 Global Lock 是否成为长尾延迟来源。

优化目标应根据 Service Level Objective 制定，而不是只提升平均吞吐量。

## Problem 15 — 设计一个可 Upstream 的 RISC-V OP-TEE SMP 修复

场景：

你发现当前某平台上的 OP-TEE 在高并发 TA Mapping Update 时存在潜在 TLB 同步问题。

你准备向 upstream 提交 Patch。

作为 Staff Engineer，你会如何组织工作？

参考答案：

第一步：定义问题。

明确触发条件、涉及 Hart、受影响的 Page Table、ASID、TLB State 及可能的安全后果。

第二步：建立最小复现。

尽量在 QEMU 或通用 RISC-V 平台上构造可重现的测试，而不是只提供芯片厂商内部的日志。

第三步：确认责任边界。

问题属于 OP-TEE MMU、OpenSBI RFENCE、Platform Memory Coherency，还是多个组件的集成问题？

第四步：设计最小修复。

不应在不理解 Root Cause 的情况下直接加入全局 `SFENCE.VMA` 或禁用 SMP。

第五步：建立测试矩阵。

覆盖：

- Single Hart。
- Multi Hart。
- ASID-specific Mapping。
- Global Mapping。
- Page Reclaim。
- RPC/Resume。
- 受影响的不同 MMU 配置。

第六步：提交具有清晰 Commit Message、测试证据和性能影响说明的 PR。

最终考核点：

真正的 Maintainer 级工程师，不仅应该知道如何修复，还应该能够说明为什么修复不会破坏其他路径，以及如何防止未来 Regression。

# 17.14 本章源码阅读清单

建议在结束本章之前，至少完整阅读以下几个文件。

| 项目          | 源码文件                                                                                                                                  | 阅读重点                                                |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| OpenSBI     | [sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com) | Per-Domain/Per-Hart Context、CSR/FP/Vector、PMP       |
| OpenSBI     | [domain_support.md](https://github.com/riscv-software-src/opensbi/blob/master/docs/domain_support.md?utm_source=chatgpt.com)          | possible_harts、assigned_harts、Domain Access Control |
| OP-TEE      | [thread_arch.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_arch.c?utm_source=chatgpt.com)           | Thread Alloc/Suspend/Resume、Per-Hart State          |
| OP-TEE      | [thread_rv.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_rv.S?utm_source=chatgpt.com)               | Trap、Register Save/Restore、Stack、`tp`               |
| OP-TEE      | [core_mmu_arch.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/mm/core_mmu_arch.c?utm_source=chatgpt.com)           | ASID、MMU、Remote TLB Shootdown                       |
| OP-TEE      | [boot.c](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/boot.c?utm_source=chatgpt.com)                         | Secondary Hart、HSM、Boot Synchronization             |
| RISC-V Spec | [RVWMO Specification](https://docs.riscv.org/reference/isa/unpriv/rvwmo.html?utm_source=chatgpt.com)                                  | Atomic、FENCE、Memory Ordering                        |
| SBI Spec    | [SBI RFENCE Extension](https://docs.riscv.org/reference/sbi/v3.0/ext-rfence.html?utm_source=chatgpt.com)                              | Remote SFENCE、ASID、目标 Hart                          |

建议在阅读过程中，特别关注下面六个函数或代码路径：

```
OpenSBI:
  switch_to_next_domain_context()
  sbi_hart_protection_reconfigure()

OP-TEE:
  thread_resume_from_rpc()
  thread_state_suspend()
  boot_start_secondary_cores()
  tlbi_remote()
```

它们分别连接了本章最重要的几项内容：Domain Context、Hardware Isolation、Thread Migration、Hart Lifecycle 与 Remote TLB Synchronization。

# 17.15 本章总结：建立完整的 SMP TEE 思维模型

完成这一章之后，希望你不再把 SMP TEE 理解为：

“给 OP-TEE 增加一个 Spinlock，再让多个 Hart 同时运行。”

更准确的模型是下面这张表：

| 层次                | 谁负责                                   | 核心任务                                 |
| ----------------- | ------------------------------------- | ------------------------------------ |
| Hardware          | Hart / Cache / PMP / IOPMP / Firewall | CPU 执行、硬件一致性、物理访问权限                  |
| M-mode Firmware   | OpenSBI                               | Domain Context、HSM、IPI、RFENCE、隔离配置   |
| S-mode TEE Kernel | OP-TEE                                | Trusted Thread、MMU、TA Isolation、内核同步 |
| U-mode TA         | Trusted Application                   | 安全服务逻辑和 TA 私有状态                      |
| REE               | Linux / Driver                        | 请求调度、Transport、RPC、客户端生命周期           |

这几个层次共同组成可信的 SMP 执行系统。

最终需要牢固掌握以下六个结论：

1. Per-Hart 不等于 Per-Thread。 Hart-local State、Trusted Thread Context 和 TA Context 必须具有清晰的生命周期与所有权。
2. Domain Switch 不等于普通 Task Switch。 它同时涉及执行上下文与硬件安全权限的切换。
3. Cache Coherence 不等于 Memory Ordering。 正确的共享数据通信需要明确的同步协议。
4. Local `SFENCE.VMA` 不等于 Remote TLB Shootdown。 跨 Hart 的 Page Table 修改需要可靠的 Translation Synchronization。
5. 软件锁不能弥补硬件隔离能力不足。 特别是全局 Firewall 和 DMA 权限设计。
6. SMP Bug 可能演变成安全漏洞。 Thread Context、TLB、PMP、FP/Vector 和 Interrupt 的错误状态都需要按照 Security Threat Model 分析。

## 最值得深入研究的下一项课题

结合整套 OP-TEE 学习路线，我建议把 RISC-V OP-TEE 的 Remote TLB Shootdown、ASID Lifecycle 与跨 Hart Trusted Thread Resume 作为 Chapter 17 之后的第一个源码级深入实验。

这三个问题恰好把你已经掌握的 RISC-V Privileged Architecture、OpenSBI Domain、OP-TEE Trusted Thread 和 SMP Kernel 连接起来。

如果能够独立审查这几条路径、提出可靠的回归测试，并针对实际问题完成 upstream 修复，就已经开始从“理解 SMP TEE 的工程师”向“能够维护 SMP TEE 核心机制的工程师”迈进。