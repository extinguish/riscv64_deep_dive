# Chapter 07 — OpenSBI Domain Context 的多核一致性与资源管理

RISC-V OP-TEE Architecture & Porting · Staff Engineer 深度课程

## 本章定位与学习目标

前面的 Chapter 04～06 已经建立了三个基础：

- Chapter 04 — OpenSBI Domain Architecture：Domain 如何描述资源、权限与 Hart 归属。
- Chapter 05 — PMP & Hardware Isolation：如何通过 PMP 和 SoC 级硬件机制实现隔离。
- Chapter 06 — Domain Context Switching：单个 Hart 如何在 Linux Domain 和 OP-TEE Domain 之间切换。

现在进入一个更复杂、也更贴近真实 SoC 的问题：

当系统具有多个 Hart，而且多个 Domain 可以在不同 Hart 上并发执行时，如何保证安全隔离、上下文一致性和资源管理的正确性？

这正是 Chapter 07 的核心。

本章会从 OpenSBI 的现有代码出发，覆盖原定大纲的所有八节，并且增加完整的工业案例、关键代码解析、验证实验和 Staff Engineer 级设计题。

## 0. 建立贯穿全章的系统模型

我们先假设一颗 RV64 四核 SoC：

- Hart 0～3：四个 Application Harts。
- Domain L：Linux REE。
- Domain T：OP-TEE。
- OpenSBI：运行在 M-mode，负责 Domain 切换及相关固件服务。
- PMP：每个 Hart 都有自己的物理内存访问控制状态。
- 系统级保护单元：根据实际 SoC，可能存在 IOPMP、WorldGuard 或专有安全防火墙。

四核 RISC-V TEE 并发执行模型

Hart 0

Linux

S-mode

OpenSBI

M-mode

PMP 0

Hart 1

OP-TEE

S-mode

OpenSBI

M-mode

PMP 1

Hart 2

Linux

S-mode

OpenSBI

M-mode

PMP 2

Hart 3

OP-TEE

S-mode

OpenSBI

M-mode

PMP 3

共享物理内存与 SoC 互连

Linux RAM

Normal Memory

OP-TEE RAM

Protected Memory

这是用于教学的并发时刻快照。不同 Hart 的 M-mode 入口属于同一个固件信任域，但各 Hart 的 PMP/CSR 状态分别维护。图中的安全性以硬件隔离正确配置为前提。

假设当前状态：

| Hart   | 当前 Domain | S-mode 软件 | 物理访问策略       |
| ------ | --------- | --------- | ------------ |
| Hart 0 | L         | Linux     | Linux Policy |
| Hart 1 | T         | OP-TEE    | TEE Policy   |
| Hart 2 | L         | Linux     | Linux Policy |
| Hart 3 | T         | OP-TEE    | TEE Policy   |

这个模型立即引出三个重要事实。

事实一：Domain 是系统级的资源与权限抽象，但 Domain Switch 是在具体 Hart 上发生的。

Hart 0 从 Linux 切换到 OP-TEE，不等于 Hart 2 也必须切换。

事实二：PMP 是 Hart-local 的保护机制。

Hart 0 更新自己的 PMP，不会自动改写 Hart 1 的 PMP 配置。因此，多 Hart 隔离的实现不能只考虑某一个 Hart。

事实三：同一个 Domain 可以在多个 Hart 上并发执行。

Domain T 运行在 Hart 1 和 Hart 3 时，二者拥有各自的寄存器上下文，却可能共享 OP-TEE 的全局内核数据结构、页表和物理内存。

这意味着我们需要同时解决：

- 每个 Hart 当前运行的是哪个 Domain？
- Domain 的共享状态是否并发安全？
- 多个 Hart 的权限配置是否一致？
- 资源权限变更时是否需要通知其他 Hart？
- 某个 Hart 进入异常状态，其他 Hart 是否还能安全运行？

我建议用一个公式理解本章的目标：

\\[ \boxed{ \text{Secure SMP Domain} = \text{Per-Hart Isolation} + \text{Global State Consistency} + \text{Cross-Hart Synchronization} } \\]

但必须强调：这只是软件设计目标的概括，不是硬件提供的自动保证。

### 0.1 本章代码基线

以下讨论以我核对的 OpenSBI upstream `master` 源码，以及 RISE RP016 的公开设计资料为依据。

主要文件：

| 文件                              | 本章重点                           |
| ------------------------------- | ------------------------------ |
| `lib/sbi/sbi_domain.c`          | Domain 注册、Hart Assignment、资源检查 |
| `include/sbi/sbi_domain.h`      | Domain 和 Hart Mask 数据结构        |
| `lib/sbi/sbi_domain_context.c`  | Per-Domain/Per-Hart Context、切换 |
| `lib/sbi/sbi_hart_protection.c` | Hart-local Hardware Protection |
| `lib/sbi/sbi_ipi.c`             | Cross-Hart IPI                 |
| `lib/sbi/sbi_hsm.c`             | Hart State Management          |
| `lib/sbi/sbi_tlb.c`             | 跨核 TLB/Fence 请求                |

源码参考：[OpenSBI Upstream](https://github.com/riscv-software-src/opensbi?utm_source=chatgpt.com)。实际 API 和内部数据结构可能随提交变化；后面涉及拟议方案时会明确标注。

# 7.1 Per-Hart Domain Context 与 Global Domain State

## 7.1.1 为什么必须区分 Per-Hart 和 Global？

这是设计 SMP Firmware 的第一原则。

我们先从 RISC-V 的执行硬件出发。

每个 Hart 都具有独立的体系结构执行状态，例如：

- GPR（通用寄存器）
- `mstatus`、`mepc` 等 M-mode CSR
- `sstatus`、`sepc`、`stvec` 等 S-mode CSR
- `satp` 与当前地址转换上下文
- FP/Vector 寄存器状态（在支持相应扩展时）
- PMP 配置寄存器

所以，当 Hart 0 和 Hart 1 同时进入 OP-TEE 时，它们不可能使用同一份正在运行的 CPU Register Context。

例如：

```
                    OP-TEE Domain
                         |
             +-----------+-----------+
             |                       |
         Hart 0                  Hart 1
             |                       |
      OP-TEE Context 0         OP-TEE Context 1
             |                       |
      sstatus / satp            sstatus / satp
      GPR / FP / V              GPR / FP / V
      Trap Context              Trap Context
             |                       |
             +-----------+-----------+
                         |
                  Shared Kernel
                         |
                  OP-TEE Heap
                  TA Metadata
                  Shared Locks
```

虽然 OP-TEE 作为一个 Domain 运行，但实际执行上下文是按照 Hart 分开的。

因此，不能只有：

```
struct domain {
    struct cpu_context context;
};
```

这种结构只能表示一个 Domain 对应一份 Context，无法支持两个 Hart 并发执行同一个 Domain。

更合理的概念模型是：

```
struct domain {
    struct cpu_context *hart_context[MAX_HARTS];
};
```

它表达了一个重要关系：

\\[ Context = f(Domain,\ Hart) \\]

也就是说：

Domain Context 至少具有两个索引维度：Domain ID 和 Hart ID。

## 7.1.2 OpenSBI 实际的数据结构

在当前源码 `lib/sbi/sbi_domain_context.c` 中，可以找到 `struct hart_context`。

这里选择其关键字段展示：

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

这是按现有源码摘录并简化的结构，当前实现还会针对处理器特性有条件地处理部分 CSR。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



其中最关键的字段分为四组。

| 类别               | 字段                             | 为什么重要                               |
| ---------------- | ------------------------------ | ----------------------------------- |
| Trap Context     | `trap_ctx`                     | 保存 GPR、`mepc`、`mstatus` 等 Trap 相关状态 |
| Supervisor State | `sstatus`、`stvec`、`satp` 等     | 保存当前 Domain 的 S-mode 执行环境           |
| Extended State   | `fp_ctx`、`vec_ctx`             | 防止 FP/Vector 状态相互覆盖或泄露              |
| Domain Metadata  | `dom`、`prev_ctx`、`initialized` | 建立 Domain Context 的归属和切换关系          |

注意，`trap_ctx` 与 `sstatus` 等单独保存的 CSR 不是一回事。前者与 OpenSBI M-mode Trap 框架直接关联，后者代表需要进行 Domain 隔离的 S-mode 状态。

## 7.1.3 Context 实际存放在哪里？

OpenSBI 当前并不是直接将固定数组嵌入 `struct sbi_domain`，而是利用其 Per-Domain State 机制管理 Context Pointer Table。

概念上类似：

```
Domain L
    |
    +-- Context Table
          |
          +-- [Hart 0] -> ctx_L0
          +-- [Hart 1] -> ctx_L1
          +-- [Hart 2] -> ctx_L2
          +-- [Hart 3] -> ctx_L3

Domain T
    |
    +-- Context Table
          |
          +-- [Hart 0] -> ctx_T0
          +-- [Hart 1] -> ctx_T1
          +-- [Hart 2] -> ctx_T2
          +-- [Hart 3] -> ctx_T3
```

这里的表项是否实际分配，取决于 Domain 的 `possible_harts` 等条件。

源码中的两个重要函数是：

```
hart_context_get(dom, hartindex);
hart_context_set(dom, hartindex, ctx);
```

它们通过 `sbi_domain_state_ptr()` 获取 Domain 对应的 Context Table。当前实现中，`hart_context_init()` 为允许的 Domain/Hart 组合分配上下文，同时根据 ISA Extension 为 Vector Context 分配存储空间。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



所以可以建立一个更加精确的抽象：

\\[ C[d][h] \\]

其中：

- \\(d\\)：Domain
- \\(h\\)：Hart
- \\(C[d][h]\\)：Domain \\(d\\) 在 Hart \\(h\\) 上的保存上下文

再定义：

\\[ D\_{\text{current}}(h) \\]

表示 Hart \\(h\\) 当前所属的 Domain。

当 Hart 0 从 Linux 切换到 OP-TEE 时：

```
Before:

D_current(0) = Linux

CPU State = C[Linux][0] 运行状态

After:

D_current(0) = OP-TEE

C[Linux][0] = 保存后的 Linux 状态

CPU State = 恢复后的 C[OP-TEE][0]
```

Hart 1、2、3 的执行状态不需要因为这次切换而自动改变。

## 7.1.4 Global Domain State 包含什么？

再来看 `struct sbi_domain` 的核心字段。

```
struct sbi_domain {
    /* ... */

    u32 index;

    spinlock_t assigned_harts_lock;
    struct sbi_hartmask assigned_harts;

    char name[64];
    u32 init_order;

    const struct sbi_hartmask *possible_harts;

    struct sbi_domain_memregion
        regions[SBI_DOMAIN_MEMREGION_MAX + 1];

    u32 boot_hartid;

    unsigned long next_arg1;
    unsigned long next_addr;
    unsigned long next_mode;

    /* ... */
};
```

以上是当前源码的关键字段摘录。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



其中：

- `possible_harts` 描述某个 Hart 是否有资格运行该 Domain。
- `assigned_harts` 描述当前分配给该 Domain 的 Hart 集合。
- `regions` 是 Domain 的内存权限策略描述。
- `assigned_harts_lock` 保护对应的共享 Hart Mask。

请注意一个非常重要的区别：

`regions` 是软件描述的访问策略，不等于所有 Hart 的硬件 PMP 已经具有相同且正确的配置。

真正的安全性必须由最终硬件配置来保证。

## 7.1.5 Global 不等于所有字段都可以随时动态修改

这一点是工业开发中很容易忽略的。

全局数据结构并不意味着每个字段都可以由多个 Hart 任意修改。

例如：

- Domain 配置往往在启动阶段构建并完成初始化。
- `assigned_harts` 是会随 Domain Switch 改变的运行时状态。
- Domain Memory Policy 如果要在运行期修改，需要额外定义生命周期、同步和回滚协议。

因此，针对数据结构要进一步区分：

| 类型                         | 示例                | 推荐的并发处理方式        |
| -------------------------- | ----------------- | ---------------- |
| Immutable Configuration    | 初始化后的静态 Region 定义 | 初始化完成后不再修改       |
| Mutable Shared State       | `assigned_harts`  | 锁和一致的访问协议        |
| Hart-local Execution State | CSR、当前寄存器状态       | 由所属 Hart 的切换路径管理 |
| Cross-Hart Resource State  | 共享设备、IRQ 所属关系     | 跨 Hart 同步和明确的所有权 |
| Security Policy Generation | 动态权限版本号           | 需要设计发布、确认与撤销协议   |

特别是最后一行，属于我们后面将讨论的增强设计，并不是说 upstream 已经存在统一的 `policy_generation` 实现。

### 7.1 小结

你应该形成这样的认知：

> `sbi_domain` 定义系统级权限和资源归属，`hart_context` 保存特定 Domain 在特定 Hart 上的执行状态，而硬件权限寄存器维护的是当前 Hart 实际生效的保护状态。

三者必须保持一致，但它们并不是同一个东西。

# 7.2 Domain Context 切换与 Hart Assignment 的同步

## 7.2.1 `possible_harts` 和 `assigned_harts` 的区别

首先要分清两个特别容易混淆的概念。

`possible_harts` 表示一个 Hart 被允许进入该 Domain；`assigned_harts` 表示一个 Hart 当前被分配给该 Domain。

假设系统启动时：

```
Domain Linux:
    possible_harts = {0, 1, 2, 3}
    assigned_harts = {0, 1, 2, 3}

Domain OP-TEE:
    possible_harts = {0, 1, 2, 3}
    assigned_harts = {}
```

这里假设初始四个 Hart 都分配给 Linux。

当 Hart 1 进入 OP-TEE 后，逻辑上应该变成：

```
Domain Linux:
    possible_harts = {0, 1, 2, 3}
    assigned_harts = {0, 2, 3}

Domain OP-TEE:
    possible_harts = {0, 1, 2, 3}
    assigned_harts = {1}
```

这两个集合的作用是不同的。

| 集合               | 表示什么               | 典型使用场景                   |
| ---------------- | ------------------ | ------------------------ |
| `possible_harts` | Hart 能否运行该 Domain  | 验证 Domain Enter 合法性      |
| `assigned_harts` | Hart 当前属于哪个 Domain | SBI IPI、RFENCE、HSM 的目标过滤 |
| Context Table    | Domain/Hart 的保存状态  | Domain Suspend/Resume    |

OpenSBI 官方文档明确要求一个 Hart 在某个时刻运行于一个 Domain Context；SBI IPI、RFENCE 和 HSM 的访问也受当前 Domain 的 Hart Assignment 约束。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此 `assigned_harts` 并非单纯用于输出调试信息，它会影响系统服务的访问权限。

## 7.2.2 阅读实际的 Domain Switch 代码

我们看当前 OpenSBI 中的函数：

```
static int switch_to_next_domain_context(
    struct hart_context *ctx,
    struct hart_context *dom_ctx);
```

与 Hart Assignment 相关的关键代码如下：

```
current_dom = ctx->dom;
target_dom = dom_ctx->dom;

spin_lock(&current_dom->assigned_harts_lock);

sbi_hartmask_clear_hartindex(
    hartindex,
    &current_dom->assigned_harts);

spin_unlock(&current_dom->assigned_harts_lock);

sbi_update_hartindex_to_domain(
    hartindex, target_dom);

spin_lock(&target_dom->assigned_harts_lock);

sbi_hartmask_set_hartindex(
    hartindex,
    &target_dom->assigned_harts);

spin_unlock(&target_dom->assigned_harts_lock);
```

这是当前 upstream 对应代码的节选。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



注意操作顺序：

```
1. Lock Linux Domain
2. Remove Hart 1 from Linux assigned_harts
3. Unlock Linux Domain

4. Update Hart 1's current Domain pointer

5. Lock OP-TEE Domain
6. Add Hart 1 into OP-TEE assigned_harts
7. Unlock OP-TEE Domain
```

为什么需要锁？

因为 Hart 0 可能正在读取 Linux 的 `assigned_harts`，准备执行 SBI IPI；与此同时，Hart 1 正在从 Linux 切换到 OP-TEE。

如果读写 Hart Mask 时没有同步，就可能观察到损坏的共享状态或不符合预期的目标集合。

## 7.2.3 这里存在一个非常值得研究的并发问题

从代码可以发现，切换并不是在一把覆盖所有 Domain 的全局锁中完成的。

特别是：

```
T0: Hart 1 从 Linux assigned_harts 删除

T1: Hart 0 读取 Linux assigned_harts

T2: Hart 1 更新 current_domain 指针

T3: Hart 1 加入 OP-TEE assigned_harts
```

在 T1 时刻，Hart 0 会看到 Hart 1 已经不属于 Linux。

这是一个转换中的中间状态。

而在 T2 和 T3 之间，如果其他 Hart 分别读取 Current Domain Pointer 和 OP-TEE 的 `assigned_harts`，也可能暂时观察到不同的状态。

这需要区分：

- Memory Safety：每个数据结构的访问是否受正确保护。
- State Consistency：多个相互关联的数据结构是否具有一个统一的逻辑生效点。
- Security Consistency：即便发生短暂的状态差异，是否仍然无法违反安全权限。

有锁不等于整个跨对象状态变更具有原子性。

这正是工业级并发系统设计中经常出现的问题。

不过，也不能直接根据几行源码断言它必然存在安全漏洞，因为还必须分析所有读取路径、切换所处的 M-mode 上下文，以及硬件保护何时真正更新。

例如，如果状态更新期间当前 Hart 不会返回低特权级执行，并且所有可并发访问路径都被正确限制，那么某些中间状态是可以接受的。

反之，如果某个服务错误地把软件 Hart Mask 当成已经生效的硬件保护状态，就可能产生安全风险。

## 7.2.4 为什么 `sbi_domain_context_enter()` 还要检查 `possible_harts`？

当前源码中有这样的检查：

```
if (!dom || dom == sbi_domain_thishart_ptr())
    return SBI_EINVAL;

if (!sbi_hartmask_test_hartindex(
        hartindex, dom->possible_harts))
    return SBI_EINVAL;
```

这段代码的含义是：

1. 不能进入无效 Domain。
2. 不能通过这个入口重复进入当前 Domain。
3. 当前 Hart 必须具有进入目标 Domain 的资格。

这体现了一种基本的 Authorization Policy。

但是，需要进一步强调：

`possible_harts` 不是让低特权级软件自由选择安全 Domain 的授权凭据。

对于 Linux → OP-TEE 这样的安全调用，还必须由受信任的入口协议判断请求是否合法。

否则，假如被攻破的 Linux 可以任意指定目标 Domain 和入口，那么仅检查 Hart 是否出现在 `possible_harts` 中，并不足以保护安全系统。

## 7.2.5 更严格的设计应当区分 Control Plane 和 Data Plane

我建议从系统架构角度这样理解。

Control Plane：

决定哪个 Hart 可以进入哪个 Domain，Domain 拥有哪些资源，以及切换请求是否被授权。

Execution Plane：

具体执行寄存器保存、Hart Assignment 更新和硬件隔离状态切换。

在可信固件中，两者必须协作：

```
Request Domain Switch
        |
        v
Validate Caller / Target
        |
        v
Check Possible Hart / Policy
        |
        v
Execute Trusted Transition
        |
        +-- Save Context
        +-- Update Assignment
        +-- Configure Protection
        +-- Restore Context
        |
        v
Resume Target Domain
```

这里后半部分是执行过程的概念划分，不能直接当作固定的 OpenSBI 源码调用顺序；实际代码会先后处理 CSR、Trap Context 和 Protection。

### 7.2 小结

理解 Hart Assignment 的关键不是记住两张 Hart Mask，而是能够回答：

> 在 Hart 正在切换 Domain 的过程中，其他 Hart 是否有可能观察到中间状态？如果能，这些状态会不会影响 IPI、HSM、权限检查或资源生命周期？

这是本章需要持续关注的一条主线。

# 7.3 同一 Domain 同时运行多个 Hart

## 7.3.1 Domain 不等于 Thread，也不等于 CPU

再看一个很重要的场景。

假设 Hart 1 和 Hart 3 同时运行 OP-TEE：

```
                    OP-TEE Domain
                          |
            +-------------+-------------+
            |                           |
         Hart 1                       Hart 3
            |                           |
     OP-TEE Thread A             OP-TEE Thread B
            |                           |
     Context[T][1]               Context[T][3]
            |                           |
            +-------------+-------------+
                          |
                  Shared OP-TEE Core
                          |
               TA Metadata / MMU / Heap
```

这里会同时存在三种不同层次的上下文：

第一层：OpenSBI Domain Context

负责同一个 Hart 在 Linux 与 OP-TEE 之间切换。

第二层：OP-TEE Trusted Thread Context

负责 OP-TEE Core 内部的受信任执行线程状态。

第三层：User TA Context

负责 TA 的用户态执行环境，包括 TA 的地址空间、寄存器和调用状态。

它们的关系可以概括为：

```
Hart
 |
 +-- Domain Context (OpenSBI)
       |
       +-- OP-TEE Kernel
             |
             +-- Trusted Thread
                   |
                   +-- User TA Context
```

这个结构不意味着三个层次都需要独立的抢占式调度器。

它只是表明：

Domain Context Switching 与 OP-TEE 内部 Trusted Thread 管理解决的是不同的问题。

## 7.3.2 为什么两个 Hart 可以同时执行 OP-TEE？

因为 Domain 是资源和权限边界，而不是强制单线程运行的执行实体。

只要：

1. 两个 Hart 都允许运行 OP-TEE Domain。
2. 各自具有正确的 Domain Context。
3. 各自生效的隔离配置满足 OP-TEE 访问需求。
4. OP-TEE Core 的并发数据结构和 Trusted Thread 机制正确。

就可以实现多 Hart 同时运行 OP-TEE。

这里不能简单地认为一个 Domain 在执行时需要独占整个 SoC。

它更接近：

> 多个处理器在受同一安全策略约束的地址空间和资源集合中并发执行。

但必须注意，OpenSBI Domain 和 Linux Process Address Space 并不等价；前者还包含硬件 Hart 归属、固件服务权限和系统资源约束。

## 7.3.3 同一个 Domain 内部也可能出现 Race Condition

举一个例子。

Hart 1 和 Hart 3 同时进入 OP-TEE，都试图更新一个共享计数器：

```
static unsigned int active_sessions;

void session_enter(void)
{
    active_sessions++;
}
```

执行序列可能是：

```
Initial:
    active_sessions = 5

Hart 1: load active_sessions -> 5
Hart 3: load active_sessions -> 5

Hart 1: store 6
Hart 3: store 6

Expected: 7
Actual:   6
```

这属于典型的 Lost Update。

但有一个需要明确的边界：

OpenSBI 的 `assigned_harts_lock` 不能保护 OP-TEE Kernel 中的 `active_sessions`。

因为两者属于不同的软件执行层：

- OpenSBI 管理 Domain 和底层固件状态。
- OP-TEE 内核自己负责 Thread、Session、Heap 等数据结构的同步。

不能因为 OpenSBI 提供了 Domain Isolation，就认为 OP-TEE 不再需要 SMP Lock。

## 7.3.4 一个更容易被忽略的问题：Context 和 Hart 的绑定

假设 OP-TEE Thread A 在 Hart 1 上执行了一段时间，然后因为 RPC 返回 Linux。

之后 Linux 调度器可能让相关调用在不同 CPU 上继续执行。

那么：

- OpenSBI 的 Hart 1 Domain Context 能否直接移到 Hart 3？
- OP-TEE 的 Trusted Thread 是否可以在 Hart 3 恢复？
- RPC 返回时如何找到原来的 Thread？
- 是否要求 Linux Driver、Transport 或 TEE 入口遵循某种 CPU Affinity 约束？

这几个问题不能混为一谈。

OpenSBI 的 `hart_context` 是 per-Hart/per-Domain 状态，并不是可以任意迁移的普通 Thread Context。

另一方面，OP-TEE 的 Trusted Thread 恢复能力由其自身的线程模型和传输协议决定。

工业实现需要明确 CPU affinity、会话关联、恢复 Token、Per-Hart Channel 及错误处理规则，而不是假定 Domain Context 会自动跨核迁移。

### 7.3 小结

同一个 Domain 在多个 Hart 上执行时，至少需要同时保证：

\\[ \begin{aligned} &\text{Hardware Isolation}(h) \\\ &\text{Domain Context Correctness}(h) \\\ &\text{Shared Kernel Data Synchronization} \\\ &\text{Protocol/Thread Resume Correctness} \end{aligned} \\]

这些责任跨越 OpenSBI、OP-TEE 和 Linux Driver，需要明确分层。

# 7.4 PMP 配置的 Hart-local 性质与一致性问题

这一节是本章最重要的安全基础。

## 7.4.1 PMP 配置为什么是 Hart-local？

PMP 相关寄存器属于每个 Hart 的 CSR 状态。

例如：

```
csrw pmpaddr0, a0
csrw pmpcfg0,  a1
```

这些操作更新当前 Hart 的 PMP 寄存器。

因此：

```
Hart 0 PMP       Hart 1 PMP
    |                |
    |                |
Policy L         Policy T
    |                |
    v                v
Linux Access     OP-TEE Access
```

当 Hart 0 从 Linux 切换到 OP-TEE 时：

```
Hart 0:

    PMP_L
      |
      | Domain Switch
      v
    PMP_T
```

不会意味着 Hart 1 自动也执行相同的更新。

换句话说：

\\[ PMP(h) = Policy(D\_{\text{current}}(h)) \\]

这是一条我们希望满足的安全不变量。

严格来说，应该在 Hart 即将返回 S/U-mode 时满足；在受控的 M-mode 切换中间阶段，可以存在尚未完成的硬件配置状态，但必须保证这些中间状态不会被不可信软件利用。

## 7.4.2 为什么多 Hart PMP 一致性并不等于所有 PMP 寄存器相同？

假设：

```
Hart 0 -> Linux
Hart 1 -> OP-TEE
```

此时：

```
Hart 0 PMP:
    Linux RAM = RWX
    TEE RAM   = No Access

Hart 1 PMP:
    Linux RAM = Based on TEE policy
    TEE RAM   = RWX
```

这完全可以是一种正确状态。

因此，我们追求的不是：

\\[ PMP(0) = PMP(1) \\]

而是：

\\[ \forall h,\quad PMP(h)\models Policy(D\_{\text{current}}(h)) \\]

这里的 \\(\models\\) 表示当前硬件配置满足对应 Domain 的访问规则。

更重要的是：

一个 Domain 可能既能访问自己的私有内存，也能访问经过授权的 Shared Memory。因此，不能把所有其他 Domain 的地址都一律定义为 No Access，而应该遵循实际资源政策。

## 7.4.3 当前 OpenSBI 如何更新保护配置？

回到 `switch_to_next_domain_context()`。

当前源码在保存、交换相关 Supervisor CSR、FP/Vector 和 Trap Context 后，调用：

```
sbi_hart_protection_reconfigure(
    scratch,
    current_dom,
    target_dom);
```

相关代码还说明，这一步需要完成适当的地址转换同步。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



其保护框架中，会对相关机制执行旧 Domain 的 Unconfigure，再执行新 Domain 的 Configure。当前代码已经使用统一的 Hart Protection Framework，而不是仅仅在 Domain Switch 中直接写固定的 PMP Entry。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



概念上可以理解为：

```
Current: Linux Domain
        |
        v
Prepare Context Transition
        |
        v
Save / Restore Execution State
        |
        v
Remove Old Domain Protection State
        |
        v
Configure New Domain Protection State
        |
        v
Required Fences
        |
        v
Resume OP-TEE Domain
```

需要特别注意：

保护配置切换不意味着必须先临时开放所有物理内存。

例如，对传统 PMP 和带有 Smepmp 扩展的系统，必须分别考虑 M-mode 固件自身的代码和数据在切换期间是否仍然可访问。

已有真实的 OpenSBI Patch 专门处理过这个问题：在 Smepmp 开启时，如果错误地清除 OpenSBI Firmware 的 PMP Entry，可能导致 M-mode 固件失去访问自己代码或数据的权限。

[image](https://www.google.com/s2/favicons?domain=https://lists.infradead.org\&sz=32)

lists.infradead.org



这个例子非常值得记住。

它揭示了一个普遍原则：

> 安全切换的中间状态本身也必须满足足够的执行安全性，不能只保证切换完成后的目标状态正确。

## 7.4.4 工业案例：两个 Hart 同时执行 Domain Switch

假设：

```
Hart 0: Linux -> OP-TEE
Hart 1: OP-TEE -> Linux
```

那么可能出现：

| 时刻 | Hart 0          | Hart 1          |
| -- | --------------- | --------------- |
| T0 | Linux Policy    | TEE Policy      |
| T1 | M-mode Switch   | TEE Policy      |
| T2 | Reconfigure PMP | M-mode Switch   |
| T3 | TEE Policy      | Reconfigure PMP |
| T4 | TEE Policy      | Linux Policy    |

为什么不需要让 Hart 0 和 Hart 1 总是等待对方完成 PMP 更新？

因为在这个例子中，两个 Hart 的 PMP 是独立的，而 Domain Policy 假设没有发生全局变化。

它们更新的是不同 Hart 的硬件寄存器。

但是，如果此时还要修改全局共享资源的 Ownership，那么仅靠 Hart-local 更新就不一定足够。

这正是下一节的重点。

# 7.5 Domain Memory Policy 动态变化时的同步

这一节比普通 SMP Lock 更难，因为它涉及安全权限的撤销。

## 7.5.1 首先区分两种完全不同的操作

假设 Linux Domain 的物理内存策略是：

```
Linux Domain:
    0x80000000 - 0x87FFFFFF   RWX

OP-TEE Domain:
    0x90000000 - 0x90FFFFFF   RWX
```

现在考虑两个操作。

操作 A：Hart 0 从 Linux 切换到 OP-TEE。

两个 Domain 的权限策略没有变化，只有 Hart 0 当前生效的策略发生变化。

操作 B：撤销 Linux 对某个物理 Region 的访问权限。

Linux Domain 的权限策略本身发生了变化。所有仍在运行 Linux 的 Hart 都必须满足新的策略。

这两个操作具有完全不同的同步要求。

| 操作                     | 变化对象             | 影响范围                | 是否需要跨 Hart 同步          |
| ---------------------- | ---------------- | ------------------- | ---------------------- |
| 普通 Domain Switch       | 当前 Hart 的执行和硬件状态 | 当前 Hart             | 通常不需要全局 Stop-the-world |
| Domain Policy Change   | Domain 全局权限策略    | 所有相关 Hart、设备        | 通常需要                   |
| TA Page Mapping Change | OP-TEE 页表        | 共享该地址空间的 Hart       | 按 MMU/TLB 同步规则         |
| DMA Ownership Change   | Device/Bus 权限    | 对应 DMA Master 和 CPU | 需要设备级同步                |

这里还有一个重要的 upstream 边界。

当前 OpenSBI 的标准 Domain 模型主要是在初始化阶段建立配置，并不是一个可以由 Linux 随意修改 Domain 内存权限的动态策略服务。

在 `sbi_domain_register()` 和 `sbi_domain_add_memrange()` 等路径中，已经可以看到 `domain_finalized` 对后续配置操作的限制。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此，以下讨论的动态 Policy Revocation 是一项系统设计练习，不代表现有 OpenSBI 已经提供完全相同的运行时 API。

## 7.5.2 为什么动态撤权很危险？

假设有两个 Hart：

```
Hart 0 -> Linux
Hart 1 -> Linux
```

两者都可以访问：

```
PA: 0x88000000 - 0x880FFFFF
```

现在受信任固件希望将这块内存重新分配给 OP-TEE。

错误实现：

```
T0: Firmware 更新 Global Domain Policy

T1: Hart 0 PMP 更新完成

T2: Firmware 宣布内存已经归 OP-TEE

T3: OP-TEE 开始写入秘密数据

T4: Hart 1 仍然使用旧的 PMP Policy

T5: Hart 1 读取 0x88000000
```

结果是：Hart 1 在新 Policy 生效前仍然能够访问本应已经撤销权限的内存。

这里的问题不是普通的数据竞争，而是安全权限撤销没有全局完成。

因此，一条正确的安全不变量是：

\\[ \forall h\in H\_{\text{affected}}, \quad Access(h,R)\subseteq NewPolicy(R) \\]

在新所有者使用区域 \\(R\\) 前，所有相关 Hart 的有效权限必须已经满足新策略。

同时，所有相关 DMA Master 也必须满足相应约束。

## 7.5.3 设计一个安全的动态撤权协议

我建议采用一个分阶段的事务模型。

安全权限撤销：设计级状态机

1. PREPARE

   受信任管理者确认调用方权限、资源归属及目标策略，进入不可再新增旧权限使用者的状态。
2. QUIESCE

   阻止新的访问请求，等待相关正在执行的 CPU/设备操作退出临界区，并停止可能访问目标区域的 DMA。
3. REVOKE

   更新所有受影响 Hart 的硬件权限，执行必要的地址转换和设备同步，并获得完成确认。
4. SANITIZE

   处理原所有者遗留的数据、缓存敏感状态、设备队列及可能残留的访问路径。
5. GRANT & PUBLISH

   向新 Domain 建立权限，确认硬件隔离已经生效，然后才宣布资源转移成功。

其中最重要的设计原则是：

旧权限必须先被可靠撤销，才能把同一资源交给新的、不互信的所有者。

这一过程不一定总要暂停整颗 SoC。更好的工程实现可以通过资源专属的 Quiescence 和同步协议，最小化受影响的 Hart 范围。

但在没有明确硬件和并发协议支持时，宁可采用保守的 Stop-the-world 方案，也不要假设普通的内存锁可以保证硬件访问权限的原子变更。

## 7.5.4 使用 Generation Counter 管理策略版本

这里介绍一个非常有用的设计思想。

假设我们增加一个受信任的 Domain Policy Generation：

```
/* 设计示意：不是 OpenSBI 当前 API */

struct domain_policy {
    uint64_t generation;
    struct memory_region regions[MAX_REGIONS];
};

struct hart_policy_state {
    uint64_t applied_generation;
};
```

例如：

```
Global Policy Generation = 8

Hart 0 applied = 8
Hart 1 applied = 7
Hart 2 applied = 8
Hart 3 applied = 7
```

这意味着 Hart 1 和 Hart 3 还没有完成新策略。

不能因为 Global Policy 已经更新成 Version 8，就宣布权限撤销成功。

判断条件应该类似：

\\[ \forall h\in H\_{\text{affected}}, \quad AppliedGeneration(h)\ge TargetGeneration \\]

但是，这仍然需要更严格的协议。

因为 `applied_generation` 只是软件状态，必须保证：

1. 只有可信代码能更新它。
2. 只有硬件权限真正生效后才能更新它。
3. 更新和读取具有正确的内存顺序。
4. Hart 离线、超时、重启时有明确处理。
5. 设备权限变更具有等价的完成条件。
6. 新进入该 Domain 的 Hart 不能绕过版本校验。

因此，Generation Counter 不是安全机制本身，而是帮助管理安全状态同步的一种方法。

## 7.5.5 为什么单纯执行 `fence` 或 `SFENCE.VMA` 不够？

这里必须区别三个概念。

| 机制                       | 主要职责                                   |
| ------------------------ | -------------------------------------- |
| `fence`                  | 约束相关内存及 I/O 操作的顺序和可见性                  |
| `SFENCE.VMA`             | 按 RISC-V 规则同步本 Hart 的 S-stage 地址转换相关状态 |
| PMP/Firewall Reconfigure | 真正改变对应访问路径的硬件权限                        |

如果两个 Hart 共享一套页表，Hart 0 修改了 PTE，通常需要针对适用地址空间进行 Remote TLB Synchronization。

但如果改变的是 Hart 1 的 PMP：

Hart 0 在自己的 CPU 上执行 `SFENCE.VMA`，并不能替 Hart 1 写入新的 PMP CSR。

必须确保 Hart 1 自身的有效保护配置已经更新。

同样，发送 IPI 也不自动等于操作完成。需要确认目标 Hart 已执行相应处理，并满足所要求的内存顺序和硬件生效条件。

尤其在 RISC-V SBI 中，RFENCE 和 IPI 是不同的服务接口；它们的目的和完成语义不能混淆。

## 7.5.6 工业场景：动态分配 Secure Video Buffer

考虑一个汽车座舱或者车载摄像头 SoC。

某块 DDR Buffer 起初由 Linux Video Driver 使用，之后需要转交给 OP-TEE 进行敏感图像处理。

正确的系统设计不能只是：

```
secure_region.owner = DOMAIN_TEE;
```

还需要处理：

- Linux CPU 的旧映射和访问。
- Video DMA Engine 的 In-flight Transactions。
- IOMMU/IOPMP 或 SoC Firewall。
- Cacheable Buffer 的一致性及数据残留。
- OP-TEE 对缓冲区的访问权限。
- 异常情况下的所有权恢复。

本节最重要的结论：内存所有权转移是跨 CPU、MMU、DMA 和系统级安全控制器的事务，而不是一个指针或者 Hart Mask 的变化。

# 7.6 跨 Hart 切换、重新分配和热插拔

## 7.6.1 先区分三种不同的 Hart 操作

在实际项目中，这三个概念经常被混在一起。

| 操作                    | 本质                                        |
| --------------------- | ----------------------------------------- |
| Domain Context Switch | 同一个 Hart 从 Domain A 切换到 Domain B          |
| Hart Reassignment     | 修改该 Hart 当前的 Domain 归属                    |
| CPU Hotplug / HSM     | 改变 Hart 的 Started、Stopped、Suspended 等运行状态 |

例如：

```
Hart 2:

Linux -> OP-TEE -> Linux
```

这是正常 Domain Switching，并不意味着 Hart 2 被关闭或重新启动。

而：

```
Hart 2:

STARTED -> STOPPED -> START_PENDING -> STARTED
```

属于 HSM 生命周期。

两者虽然可能相互影响，但不是同一个状态机。

## 7.6.2 OpenSBI HSM 的基本状态模型

典型的 HSM 状态包括：

```
STARTED
STOPPED
START_PENDING
STOP_PENDING
SUSPENDED
SUSPEND_PENDING
RESUME_PENDING
```

可以把基本启动停止过程理解为：

```
          hart_start()
STOPPED ----------------> START_PENDING
                               |
                               v
                            STARTED
                               |
                               | hart_stop()
                               v
                          STOP_PENDING
                               |
                               v
                            STOPPED
```

Suspend 和 Resume 是另一组有条件的状态转换，不能简单等同于 Hart Stop/Start。

这些状态由 OpenSBI HSM 机制维护。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 7.6.3 Hotplug 与 Domain Isolation 为什么会相互影响？

假设 Hart 2 当前运行 OP-TEE，并且 Linux 正在尝试执行 CPU Offline。

Linux 并不应该直接获得对 OP-TEE Domain 中 Hart 的管理权限。

OpenSBI HSM 调用会校验 Domain Assignment。例如 `sbi_hsm_hart_start()` 就包含目标 Hart 是否属于调用 Domain 的检查。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此，正常 Linux HSM 服务不能简单越过 Domain 边界操作任意 Hart。

而更复杂的场景是：

```
T0: Hart 2 在 Linux Domain

T1: Hart 2 进入 OP-TEE

T2: Hart 0 准备将 Hart 2 Offline

T3: Hart 2 正在 Domain Transition

T4: HSM 接收到 Hart Stop Request
```

必须明确：

- HSM 请求在哪个 Domain 权限下发起？
- Hart Assignment 在哪个时间点检查？
- 正在切换的 Hart 能否同时进入 Stop 流程？
- 停止后是否有尚未保存的 OP-TEE Context？
- 再次 Start 时需要恢复哪个 Domain 的执行状态？
- `sbi_scratch`、PMP 和 Interrupt State 是否完成重建？

如果设计不当，可能出现 Hart 无法恢复、旧权限残留或等待永不结束。

## 7.6.4 Hotplug 后真正需要恢复哪些状态？

这是一个典型的 Staff Engineer 设计题。

假设 Hart 2 经过物理电源关闭后重新启动。

至少应验证以下几类状态：

第一类：M-mode Firmware State

- OpenSBI per-hart scratch
- Trap Entry
- Hart ID 与 Domain 映射
- HSM Lifecycle
- 设备相关 per-hart 初始化

第二类：Hardware Isolation State

- PMP/ePMP 状态
- WorldGuard Hart ID 或其他隔离标识
- 相关系统级 Firewall
- MMU 和地址转换缓存状态

第三类：Interrupt State

- Local Interrupt Enable
- Interrupt Pending
- AIA/IMSIC Hart-local 配置
- IPI Delivery
- 必要的中断重新路由

第四类：OP-TEE SMP State

- Per-Hart Stack
- OP-TEE CPU-local Data
- Trusted Thread 使用状态
- 仍然关联该 Hart 的 RPC/Transport 状态
- 相关 TLB、Cache 和调度/锁状态

这并不意味着所有寄存器都要原样恢复。例如，当 Hart 进入一条全新的启动链时，某些寄存器应该重新初始化而不是复用旧值。

核心在于：

必须明确哪些状态具有生命周期连续性，哪些状态在 Power Cycle 后必须视为无效。

## 7.6.5 一个危险错误：将旧的 Hart Context 当成已恢复的硬件状态

假设：

```
ctx->initialized = true;
```

表示该 Hart 的某个 Domain Context 曾经被保存过。

这并不等于在一次深度电源关闭后，硬件 PMP、FP、Vector、MMU 和中断状态仍然存在。

软件保存的 Context 可以是恢复的输入，但不能代替实际硬件初始化。

当前 OpenSBI 的 `sbi_domain_context.c` 会维护 `initialized` 状态，并在目标 Context 尚未初始化时通过 Boot Hart 启动或 Hart Stop 等路径处理。这是理解生命周期的一个具体源码入口。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 7.6 小结

对于多核 TEE，需要至少同时维护：

\\[ HartState(h) \\]

\\[ CurrentDomain(h) \\]

\\[ Context(d,h) \\]

\\[ HardwareProtection(h) \\]

四者之间的关系。

不能认为 Hart `STARTED` 就一定已经具有目标 Domain 的有效安全状态。

# 7.7 Shared Resource、Lock、Race Condition 和 Deadlock

这是最容易把 RTOS SMP 经验迁移到 OP-TEE/OpenSBI 开发的部分，但它也有一些安全固件特有的约束。

## 7.7.1 首先建立资源分类

真实 SoC 中的共享资源并不只有内存。

| 资源                   | 共享范围            | 主要风险                     |
| -------------------- | --------------- | ------------------------ |
| `assigned_harts`     | OpenSBI 多 Hart  | Race、读到过渡状态              |
| Domain Context Table | M-mode Firmware | Context Lifetime、状态损坏    |
| Shared Memory        | Linux / OP-TEE  | TOCTOU、Cache Coherency   |
| Hardware Firewall    | 多 Hart / DMA    | Policy Race              |
| Interrupt Controller | 多 Hart / Domain | 错误路由、丢中断                 |
| IPI Infrastructure   | 多 Hart          | Completion Deadlock、目标错误 |
| HSM State            | 多 Hart          | Start/Stop Race          |
| Global Device        | Linux / OP-TEE  | Ownership 冲突             |

其中需要特别注意：

软件锁通常只能保证遵循该锁协议的软件访问之间的同步，并不能自动阻止 DMA 或其他 Bus Master 访问资源。

所以 Shared Resource 的管理至少要区分：

1. Software Mutual Exclusion
2. Hardware Ownership Enforcement
3. Cross-Hart Completion
4. Resource Lifetime Management

## 7.7.2 为什么 Spinlock 不能解决所有问题？

假设 OpenSBI 中有一段逻辑：

```
spin_lock(&domain->assigned_harts_lock);

update_hart_assignment();

spin_unlock(&domain->assigned_harts_lock);
```

这能保护 Hart Mask 的修改。

但如果其他组件这样读取：

```
if (current_domain(hart) == DOMAIN_TEE)
    use_tee_resource();
```

它没有通过相同的同步协议读取相关状态，因此不能因为前者有 Spinlock，就自动认为两者构成一个原子状态转换。

这体现了一个重要原则：

> 一致性是数据读写双方共同遵守的协议，不是单个写入函数独自具备的属性。

## 7.7.3 Deadlock 场景 A：跨 Domain Lock Order Inversion

假设工程师新增了同时操作两个 Domain Hart Mask 的功能：

```
Hart 0:
    lock(Linux)
    lock(TEE)

Hart 1:
    lock(TEE)
    lock(Linux)
```

那么可能出现：

```
Hart 0 holds Linux Lock
Hart 1 holds TEE Lock

Hart 0 waits TEE Lock
Hart 1 waits Linux Lock
```

形成 ABBA Deadlock。

解决方案之一是：

按照稳定的 Domain ID 建立全局 Lock Ordering：

```
/* 设计示意，不是 OpenSBI API */

struct sbi_domain *first;
struct sbi_domain *second;

first = src->index < dst->index ? src : dst;
second = src->index < dst->index ? dst : src;

spin_lock(&first->assigned_harts_lock);
spin_lock(&second->assigned_harts_lock);

/* Update related state */

spin_unlock(&second->assigned_harts_lock);
spin_unlock(&first->assigned_harts_lock);
```

但是，这只是锁顺序示意，并不意味着应该直接把这个代码替换进现有 Domain Switch。

原因是：持有跨 Domain 锁的期间，还必须考虑 M-mode Interrupt、嵌套调用和 Lock Ordering 约束。

当前源码对 Context Switch 增加了 `MUST_NOT_HOLD()` 约束，明确要求不能在持有相关 Assignment Lock 的情况下调用该函数。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这种约束值得在代码 Review 时特别关注。

## 7.7.4 Deadlock 场景 B：持锁等待远端 Hart

假设 Hart 0 希望修改 Global Policy：

```
Hart 0:
    lock(global_policy)
    send_ipi(Hart 1)
    wait_for_ack(Hart 1)
```

而 Hart 1 的 IPI Handler：

```
Hart 1:
    receive_ipi()
    lock(global_policy)
    apply_new_policy()
    send_ack()
```

这将导致：

```
Hart 0 owns global_policy lock
    |
    +-- waiting for Hart 1 ACK

Hart 1
    |
    +-- waiting for global_policy lock
```

这种情况是典型的 Lock + Cross-Hart Completion Deadlock。

解决思路是设计两阶段协议：

```
Phase 1:
    Lock
    Prepare immutable request / snapshot
    Publish request
    Unlock

Phase 2:
    Send IPI
    Wait for ACK
    Verify generation/completion
```

如果操作要求更新期间不能出现新的参与者，还需要额外的状态门控与生命周期管理。

不能仅通过释放锁就认为协议完整。

## 7.7.5 Deadlock 场景 C：Domain-Aware IPI 导致永远等待

这一点是 OpenSBI Domain 特有的难题。

OpenSBI 的 `sbi_ipi_send_many()` 会根据当前 Domain 和 HSM 状态构造目标集合，并校验显式指定的 Hart 是否属于该 Domain。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



假设：

```
Hart 0 -> Linux Domain
Hart 1 -> OP-TEE Domain
```

Linux 在 Hart 0 发起一个只应该向 Linux Domain 发送的 IPI 请求。

如果上层代码错误地把 Hart 1 视为可达目标，同时还等待其确认，那么该请求可能在 OpenSBI 的 Domain 权限检查阶段被拒绝。

如果调用者忽略错误并继续等待完成标记，就可能无限等待。

正确的设计必须做到：

- 目标 Hart 集合与当前 Domain 权限一致。
- 检查 SBI 返回值。
- 区分 Invalid Target、Offline Target 和 Timeout。
- 不能把某个 Domain 不可达的 Hart 当作必然会响应的执行对象。
- 跨 Domain 协作需要受信任的桥接服务，而不是绕过 SBI Domain 检查。

这是一个很适合进行故障注入测试的场景。

## 7.7.6 RISC-V Memory Model 对锁和跨 Hart 通知的要求

这里结合你之前学习过的 LR/SC、AMO 和 RVWMO。

假设 Hart 0 发布任务：

```
shared->request = REQUEST_REVOKE;
shared->generation = 10;
shared->ready = true;
```

Hart 1 等待：

```
while (!shared->ready)
    ;

process(shared->request);
```

在 SMP 中，这种普通变量访问不能建立可靠的跨 Hart 同步协议。

如果使用 C 原子操作，可设计为：

```
/* 设计示例 */

struct request {
    int operation;
    unsigned long generation;
    atomic_bool ready;
};

void publish(struct request *r)
{
    r->operation = REQUEST_REVOKE;
    r->generation = 10;

    atomic_store_explicit(
        &r->ready, true, memory_order_release);
}

void consume(struct request *r)
{
    while (!atomic_load_explicit(
            &r->ready, memory_order_acquire))
        ;

    process(r->operation, r->generation);
}
```

这里 Release/Acquire 建立的是符合 C 内存模型的发布和获取关系。

它还不能代替具体的硬件 PMP 更新、TLB 同步或 DMA 同步。

如果实际项目使用 OpenSBI 自己的 Atomic、Barrier 和 Lock Primitive，应该使用该环境已定义的接口，并确认其 RVWMO 内存顺序保证，而不是直接复制用户态 C11 示例。

## 7.7.7 OpenSBI 和 OP-TEE 的 Lock 应该如何协作？

答案通常是：不要试图通过跨两个内核的普通 Spinlock 直接实现协作。

尤其在可能发生 RPC 或 Domain Switch 时，如果某一层持锁然后调用另一层服务，就必须考虑：

- 这次调用会不会发生 Context Switch？
- 调用是否可能阻塞？
- 原执行环境何时恢复？
- 回调是否重新进入同一条代码路径？
- 是否可能有其他 Hart 依赖这把锁？

合理做法通常是通过明确的通信协议和资源所有权状态机管理跨 Domain 资源，而非使用一个未明确可见性与信任边界的 Shared Spinlock。

### 7.7 小结

SMP Firmware 的并发正确性由三种机制共同保证：

\\[ \text{Concurrency Correctness} = \text{Atomicity} + \text{Ordering} + \text{Liveness} \\]

- Atomicity：相关更新不能被错误交错。
- Ordering：其他 Hart 能够按规定顺序观察状态。
- Liveness：任何参与方都不应该永久等待无法完成的操作。

对于 TEE，还必须加上第四项：

\\[ \text{Security Isolation} \\]

即使没有 Deadlock 和 Data Race，系统也可能因为错误的硬件权限或跨 Domain 资源授权而不安全。

# 7.8 安全性验证：状态泄露、权限残留与错误回滚

最后一节是本章从源码分析走向工业实践的关键。

## 7.8.1 为什么普通功能测试不够？

假设在 QEMU 中运行：

```
Linux invokes TA
OP-TEE executes
TA returns success
```

这只能说明执行路径的基本功能正常。

无法证明：

- Linux 不能访问 OP-TEE 物理内存。
- Domain Switch 不会留下旧的 PMP 权限。
- FP/Vector 寄存器不会泄露安全数据。
- 多 Hart 并发执行不会破坏 Domain Assignment。
- DMA 无法绕过 CPU 的访问保护。
- Domain Switch 失败时系统不会进入权限扩大状态。

因此，TEE 测试需要从 Threat Model 出发。

## 7.8.2 定义安全不变量

建议在开发真实 SoC 时，将以下不变量写入安全设计文档。

| ID     | 安全不变量                               | 需要验证的内容                   |
| ------ | ----------------------------------- | ------------------------- |
| INV-01 | 每个运行中的 Hart 有明确的当前 Domain           | Current Domain State      |
| INV-02 | S/U-mode 的有效硬件权限符合当前 Domain Policy  | PMP / System Firewall     |
| INV-03 | Domain Context 不会被其他不可信 Domain 直接修改 | Context Memory Protection |
| INV-04 | 进入目标 Domain 前完成敏感状态切换               | GPR / CSR / FP / Vector   |
| INV-05 | 跨 Domain IPI/HSM 操作必须遵守授权范围         | IPI / RFENCE / HSM        |
| INV-06 | 资源撤权完成前不得交给新所有者                     | Global Revocation         |
| INV-07 | Domain Switch 失败不得意外扩大访问权限          | Failure Handling          |
| INV-08 | Hart Restart 后必须重建安全状态              | Hotplug / Resume          |
| INV-09 | 非 CPU Master 必须遵守适用的隔离策略            | DMA / Device Isolation    |
| INV-10 | 持有共享锁时不会形成无法完成的跨 Hart 等待            | Deadlock / Liveness       |

其中 INV-02 是整个 Domain Isolation 的核心：

\\[ \forall h,\ \forall a,\ \forall op: \\]

\\[ Allowed\_{\text{HW}}(h,a,op) \Rightarrow Allowed\_{\text{Policy}}(D(h),a,op) \\]

也就是说，硬件实际允许的访问不能超出对应 Domain Policy 所允许的范围。

它是一条必要条件，不意味着硬件必须允许 Policy 声明的全部访问；平台还可能存在更严格的安全限制。

## 7.8.3 状态泄露测试

假设 OP-TEE 使用了 RISC-V Vector Extension。

在 OP-TEE 中通过测试代码将 Vector Register 写成敏感测试模式：

```
v0 = 0xA5A5...
v1 = 0x5A5A...
```

然后切换到 Linux Domain。

测试目标是：

Linux 不应该观察到 OP-TEE 的 Vector Register 内容。

但要注意两个细节：

1. Linux 不能仅凭读取到零就断言切换绝对安全，还需要检查所有适用的 Vector 状态。
2. 必须明确 CPU 对 FS/VS 状态、Vector Register 启用条件以及异常处理的要求。

当前 OpenSBI 的 Domain Switch 路径针对支持 F/D/V Extension 的 Hart，已经包含相应的 FP/Vector Save/Restore 调用。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这意味着可以直接围绕现有代码设计回归测试。

## 7.8.4 PMP 权限残留测试

假设：

```
OP-TEE RAM:
    PA = 0x90000000
```

测试顺序：

```
1. Hart 0 运行 Linux
2. 尝试读取 0x90000000
3. 预期 Access Fault

4. Hart 0 进入 OP-TEE
5. OP-TEE 访问自身 RAM
6. 预期成功

7. Hart 0 返回 Linux
8. 再次访问 0x90000000
9. 预期仍然 Access Fault
```

这就是最基础的 Domain Switch Permission Test。

但不要忽略两个条件：

- 地址访问必须经过真实的 PMP/硬件保护检查。
- 在 S-mode 中，需要控制页表映射，避免 Page Fault 掩盖真正要测试的 PMP Access Fault。

因此，更精确的测试要求是在正确建立映射的条件下验证物理权限，并记录 Trap Cause。

## 7.8.5 Fault Injection：切换过程中失败怎么办？

假设 Domain Switch 的某个保护操作失败：

```
Save Source Context
        |
        v
Update Domain Metadata
        |
        v
Unconfigure Source Protection
        |
        v
Configure Target Protection
        |
        X  Hardware Error
```

这时必须回答：

- 是否还可以返回源 Domain？
- 旧 PMP 配置是否仍然完整？
- 新 Domain 的部分权限是否已经生效？
- Software Hart Assignment 是否已更新？
- 当前 CSR 是否已经切换为目标 Domain？
- 是否需要停止该 Hart 或进入受信任的 Fatal Error Path？

这是安全架构中的 Fail-Secure / Fail-Closed 问题。

这里需要特别提醒一个值得在源码 Review 中追踪的细节：当前 `sbi_hart_protection_reconfigure()` 的接口返回 `int`，但我核对的 `switch_to_next_domain_context()` 调用处没有直接传播其返回值。单凭这一点不能断定存在可利用漏洞，因为还要检查具体 Protection Backend、错误是否可发生以及是否具有内部 Fatal Handling；但它确实是一个很好的异常路径审查切入点。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



如果在真实 SoC 中，硬件 Protection Configure 可能失败，那么这条路径必须有明确的失效安全设计，不能默认失败之后仍然可以安全进入目标 Domain。

## 7.8.6 安全测试矩阵

建议你在 QEMU/RTL Emulation/真实 SoC 上建立下面的测试矩阵。

| 测试场景                              | 预期结果           |
| --------------------------------- | -------------- |
| Linux 访问 OP-TEE Private RAM       | 被硬件拒绝          |
| Linux 发起非法 Domain Switch          | 请求被拒绝          |
| Linux 对其他 Domain 的 Hart 发 SBI IPI | 不被授予越权目标访问     |
| 多 Hart 同时切换 Domain                | 上下文和权限正确       |
| Domain Switch + FP/Vector 操作      | 状态不跨 Domain 泄露 |
| Domain Switch + Hart Stop         | 无非法恢复或资源残留     |
| Domain Switch + Interrupt Storm   | 无错误 Domain 执行  |
| 权限撤销时另一 Hart 持续访问                 | 撤销完成后不再允许旧访问   |
| DMA 在所有权转换期间访问                    | 被正确排空或拦截       |
| Hardware Protection 配置失败          | 不进入不安全的执行状态    |

这里还需要区分验证层级：

QEMU 能帮助验证软件状态机和其已模拟的硬件机制，但不能自动证明真实 SoC 的安全性。

例如没有建模的 DMA Firewall、Cache 一致性细节、硬件 Protection Failure 和 Power Gating 行为，必须借助 RTL Simulation、Palladium/Emulation 或真实硅片验证。

这也是为什么工业级 RISC-V OP-TEE Porting 往往不能只以 QEMU xtest 通过作为交付标准。

# 八、贯穿全章的实战实验

为了把 Chapter 07 从理论变成真正的源码能力，我建议设计三个递进实验。

## Lab 07-1 — Per-Domain / Per-Hart Context Trace

目标：证明 Context 是二维的，并跟踪每个 Hart 的 Domain 切换。

以现有 `sbi_domain_context.c` 为基础，在个人调试分支中加入 Trace。

示意代码：

```
/* Instrumentation example */

static void trace_domain_switch(
    struct hart_context *src,
    struct hart_context *dst)
{
    sbi_printf(
        "[DOMAIN] hart=%u %s -> %s\n",
        current_hartid(),
        src->dom->name,
        dst->dom->name);

    sbi_printf(
        "[DOMAIN] src=%p dst=%p\n",
        src, dst);
}
```

注意不要直接在生产版本中大量输出敏感寄存器、Context Pointer 或安全内存信息。调试信息也会影响时序，并可能引入锁竞争。

建议对两个 Hart 进行多次 Domain Switch，观察：

```
hart=0 Linux -> OP-TEE
hart=1 Linux -> OP-TEE
hart=0 OP-TEE -> Linux
hart=1 OP-TEE -> Linux
```

验收点：

- 相同 Domain、不同 Hart 的 Context Pointer 应当不同。
- 相同 Hart、不同 Domain 的 Context Pointer 应当不同。
- 频繁切换后，`prev_ctx`、`initialized` 等状态仍然合理。
- 一个 Hart 的切换不会意外破坏另一个 Hart 的保存上下文。

## Lab 07-2 — Domain-Aware IPI 与 HSM

目标：验证 Domain 的 Hart Assignment 实际影响 SBI 服务。

实验场景：

```
Hart 0 -> Linux
Hart 1 -> OP-TEE
Hart 2 -> Linux
```

分别尝试：

```
Hart 0 -> SBI IPI -> Hart 2
Hart 0 -> SBI IPI -> Hart 1
```

观察两个请求的返回结果。

然后修改测试用 Domain 配置，让目标 Hart 属于不同 Domain，确认 IPI 和 HSM 的授权检查符合预期。

实验过程重点记录：

- Caller Domain
- Target Hart ID
- Target Hart Assignment
- SBI Error Code
- HSM Target State

这里应采用测试固件或具有受控入口的环境，而不是尝试让未经修改的 Linux 任意切换自身 Domain。

## Lab 07-3 — 多核安全隔离与故障注入

目标：验证硬件隔离在 SMP 环境下不是只对单个 Hart 生效。

实验步骤：

1. 使用至少两个可用 Hart。
2. 配置 Linux RAM 与 OP-TEE RAM 为不同的受保护区域。
3. 让 Hart 0 运行 Linux，Hart 1 运行 OP-TEE。
4. 分别验证各自允许与禁止的物理内存访问。
5. 交换两者的 Domain 运行状态。
6. 重复权限测试。
7. 高频执行 Domain Switch。
8. 添加 IPI、Timer Interrupt 和异常处理压力。
9. 在支持的测试硬件模型上注入 Protection Configuration Failure。
10. 检查状态恢复、错误日志和权限不变量。

可以为每个 Hart 维护测试日志：

```
hart_id
source_domain
target_domain
switch_sequence
context_id
policy_generation
protection_status
error_code
```

其中 `switch_sequence`、`policy_generation` 是建议增加的测试字段，不代表 upstream 现成的 Context 字段。

最终实验交付： 一份能够复现的多 Hart Domain Isolation 测试报告，包含配置、测试步骤、失败注入方法、预期结果、实际结果及代码版本。

参考环境：[RP016 Milestone 4 的 Hardware Isolation Unit Tests](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/milestone4/?utm_source=chatgpt.com)。公开示例中已经包含 Domain Lifecycle 和 Isolation Callback 的测试方案，但上述动态撤权与失败注入实验仍需要自行扩展。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



# 九、Staff Engineer 级面试与架构设计题

下面给出 12 道题，均以实际工业项目中的多核 RISC-V TEE 为背景。

这些题目不只是考察概念，而是要求能够识别 Trust Boundary、设计同步协议、分析源码竞态、推导故障路径，并提出可以验证的工程方案。

## 题目 1 — Hart Assignment 的原子性与一致性

难度：Advanced · OpenSBI Concurrency / Code Review

场景：

你在 Review 一段 OpenSBI Domain Switch 代码，发现代码先从源 Domain 的 `assigned_harts` 删除当前 Hart，再更新 Current Domain Pointer，最后把 Hart 加入目标 Domain。

每一步都有适当的锁，但整个过程不受一把全局锁保护。

问题：

这种实现是否一定存在 Race Condition 或安全漏洞？如果不是，你应当如何证明它的正确性？如果有风险，应如何重构？

参考答案与分析：

首先不能直接判断一定存在可利用漏洞。

需要分清两个层次。

第一，源和目标 Hart Mask 各自的读写是否遵守锁协议。如果是，那么对这些独立数据结构的操作可以是并发安全的。

第二，`assigned_harts`、Current Domain Pointer 和实际硬件 Protection 之间是否存在要求原子观察的跨对象不变量。

重点检查：

- 哪些 Reader 会读取这几个状态？
- Reader 会不会同时读取多个状态？
- 在切换中间阶段是否仍停留在 M-mode？
- 中间状态能否被其他 Hart 通过 SBI 服务利用？
- 是否可能因 Hart Assignment 变化影响 IPI/HSM Target Filtering？
- Hart 是否有机会以旧权限进入新 Domain？
- Protection 配置失败时，软件状态是否能恢复？

改进思路不一定是增加全局大锁，也可以是定义更明确的状态机、限制中间状态的可见性，或者引入 Transition State。

高水平答案的关键：锁保护的是数据访问；安全正确性取决于所有可观察路径是否维持必要的不变量。

## 题目 2 — PMP 是 Hart-local，为什么仍然需要 SMP 安全设计？

难度：Advanced · Hardware Security

场景：

某工程师认为 PMP 是 Hart-local 的，所以 Domain Switch 只需要更新当前 Hart 的 PMP，不需要考虑其他 Hart。

这句话对不对？

参考答案：

对于静态 Domain Policy 下的普通 Context Switch，当前 Hart 更新自己的 PMP 通常是合理的。

但这一结论有前提。

如果存在全局权限变更、资源所有权迁移、共享 MMU 页表修改、DMA Ownership 变化，就可能需要跨 Hart 或跨设备同步。

例如 Hart 0 撤销某物理内存的 Linux 访问权限，而 Hart 1 仍在 Linux Domain 中运行且保留旧 PMP 配置。

如果已经将该内存交给 OP-TEE，Hart 1 仍可能访问新所有者的数据。

因此需要区分：

- Hart-local Context Transition
- Domain-global Policy Update
- System-level Resource Ownership Transfer

追问： 为什么 `SFENCE.VMA` 不能直接解决 PMP Policy 同步？

因为 `SFENCE.VMA` 不会替远端 Hart 编程 PMP CSR；地址转换同步和物理访问权限更新属于不同的问题。

## 题目 3 — 多核共享内存权限撤销协议

难度：Expert · Secure Memory Reassignment

场景：

某 SoC 需要把一块 64 MiB DDR 从 Linux 转移到 OP-TEE 用于安全视频处理。

Linux 正在 Hart 0/1 上运行，OP-TEE 正在 Hart 2/3 上运行。Video DMA 也可能仍在访问该区域。

设计完整的 Memory Ownership Transfer Protocol。

参考答案：

至少应包括：

1. 验证发起者具有资源转移权限。
2. 将 Buffer 标记为 Transitioning，阻止新提交。
3. 停止或隔离相关 DMA 请求。
4. 等待 In-flight DMA 和 CPU 操作完成。
5. 撤销旧 Domain 的 CPU 物理访问权限。
6. 撤销旧 DMA Master 的访问权限。
7. 完成必要的地址转换同步与旧映射失效。
8. 在新的信任边界建立前处理敏感数据残留。
9. 配置新 Domain 的硬件访问权限。
10. 确认保护状态生效后才允许 OP-TEE 使用。

同时必须设计失败分支：

如果第 6 步失败，不能继续执行第 9 步。

如果部分 CPU 已经撤权而另一些未完成，需要维持不可交付状态，并进入受信任的恢复或隔离流程。

核心考点： DMA、CPU、Memory Translation、Hardware Firewall 之间的同步顺序和失败语义。

## 题目 4 — Domain Switch 过程中 Hardware Protection 失败

难度：Expert · Security Critical Error Handling

场景：

Hart 1 正在从 Linux 切换到 OP-TEE。

已经保存 Linux Context、交换部分 Supervisor CSR，并调用 Hardware Protection Reconfigure。

但 WorldGuard 或其他系统级保护机制返回错误。

你会如何处理？

参考答案：

首先明确是否仍然停留在可信的 M-mode，以及当前硬件保护处于什么状态。

之后应采取 Fail-Secure 方案：

- 禁止以未知权限返回 Linux 或 OP-TEE。
- 检查是否能够恢复已知安全的源 Domain Policy。
- 如果能够可靠恢复，则同步恢复相关 CSR、Trap Context 和 Assignment。
- 如果不能证明恢复正确，应当停止该 Hart 或进入平台定义的安全故障处理流程。
- 不能简单忽略错误并继续执行 `mret`。
- 记录可安全输出的故障信息，避免泄露 Context 中的秘密数据。

还应该设计可注入的硬件故障测试，验证每个中间步骤失败后的状态。

追问： 为什么把错误码往上一层返回还不够？

因为调用者能处理返回码，不代表当前硬件保护状态已经安全。必须先保证控制流仍在可信执行环境，并且不会返回不可信执行模式。

## 题目 5 — Linux 和 OP-TEE 并发使用同一组 Hart

难度：Advanced · TEE Transport / Scheduler

场景：

四核系统中：

```
Hart 0 Linux
Hart 1 Linux
Hart 2 OP-TEE
Hart 3 OP-TEE
```

现在 Linux 希望同时发起多个 TA Invoke。

你是否需要为每次 Invoke 创建一个新的 OpenSBI Domain？

参考答案：

不需要。

Domain 是系统级隔离与资源抽象，不应该简单地与每次 TA Invoke 一一对应。

OP-TEE 内部能够使用 Trusted Thread 管理多个请求。每个 Hart 的 Domain Context 负责 Linux 和 OP-TEE 的执行环境切换。

需要检查的则是：

- OP-TEE Thread Pool 容量。
- 入口 Transport 的 Per-Hart 状态。
- TA Session 的并发模型。
- RPC 和返回请求的匹配。
- 是否允许跨 Hart Resume。
- OP-TEE Core 的共享锁和 SMP Correctness。

追问： OP-TEE Trusted Thread 是否必然绑定某个物理 Hart？

不能简单回答是或否。必须结合对应 OP-TEE 架构端口的上下文保存机制、Transport Protocol 和可能的 CPU Affinity 约束分析。

## 题目 6 — Domain-Aware IPI 导致 Deadlock

难度：Expert · Debugging / Deadlock

场景：

系统在压力测试中偶发卡死。

你发现：

```
Hart 0:
    send IPI to Hart 1
    wait completion

Hart 1:
    now running OP-TEE Domain
```

如何分析？

参考答案：

第一步不是直接认为 IPI Hardware 丢失。

先确认 IPI Request 是由哪个 Domain 发出的，以及 Hart 1 当前 Assignment。

应依次检查：

- SBI IPI 调用是否返回错误。
- 调用者是否错误忽略返回码。
- Hart 1 是否仍在目标 Domain 的 Assigned Mask 中。
- HSM State 是否允许接收该 IPI。
- IPI Event 的 Update/Process/Sync Callback。
- Interrupt Masking、Pending 和实际 Delivery。
- 等待 Completion 的代码是否持有远端 Handler 需要的锁。

如果 IPI 被合法地拒绝，正确处理是终止或调整此次请求，而不是无限等待。

如果 IPI 已被接收但未完成，则继续排查 Interrupt Routing、Lock 和远端状态。

核心考点： Domain Authorization、Interrupt Delivery 和 Completion Liveness 是三个不同层面。

## 题目 7 — CPU Hotplug 与旧 Domain Context

难度：Expert · HSM / Power Management

场景：

某 RISC-V SoC 支持 CPU Power Gating。

Hart 2 原本在 OP-TEE 中执行，随后进入 Stop/Power-Off 流程。重新启动后，软件发现其 `hart_context.initialized == true`，于是跳过某些初始化。

之后出现随机 Access Fault。

请给出可能原因和完整调试方案。

参考答案：

最重要的是区分保存的逻辑 Context 与实际硬件状态。

`initialized` 只能反映特定软件上下文的初始化状态，不能证明 Hart 的硬件状态经历 Power Cycle 后仍有效。

主要检查：

1. Hart Restart 是否重新执行 OpenSBI Warmboot。
2. `sbi_scratch` 和 Per-Hart 指针是否正确。
3. PMP/ePMP/WorldGuard Hart-local 状态是否重建。
4. `satp`、`stvec`、`sscratch` 等 CSR 是否正确。
5. MMU/TLB 和 Cache 的复位及恢复行为是否符合平台规范。
6. 是否错误恢复了旧的 Interrupt Pending 或 Local Interrupt State。
7. Context 是否引用已经释放或重建的资源。
8. OP-TEE 是否正确处理 Per-Hart Stack 和 Trusted Thread 状态。

调试时，建议先比较正常启动和失败启动的每个关键硬件状态，再检查软件 Context。

追问： 如果 Hart 2 处于一个不能安全中断的 OP-TEE 临界区，是否可以立即将它 Power Off？

不可以简单这样处理。必须由系统定义受控的 Quiescence、拒绝或延迟 Stop 的策略，避免破坏 OP-TEE 的共享状态与安全不变量。

## 题目 8 — PMP 配置正确，但 Linux 仍然能够访问安全数据

难度：Expert · SoC-Level Security

场景：

安全团队已经验证 Linux Hart 的 PMP 配置正确，无法通过 CPU Load/Store 访问 OP-TEE RAM。

但发现 Linux 控制的 DMA Engine 仍然能够读取 OP-TEE RAM。

为什么？如何修复？

参考答案：

因为 PMP 约束的是相关 RISC-V Hart 的访问，不自动覆盖所有非 CPU Bus Master。

DMA Engine 作为总线 Master，可能通过独立的访问路径直接访问 DDR。

因此，完整保护通常还需要：

- IOMMU/IOPMP。
- WorldGuard 或厂商专有 System Firewall。
- Device Ownership 与 Bus Master Identity。
- 由可信固件配置且无法被不可信软件修改的访问策略。
- DMA Buffer 的生命周期和 In-flight Transaction 管理。

修复时，首先核实系统安全架构是否确实具有可用的 DMA 隔离硬件。

如果硬件没有提供必要能力，单靠 OP-TEE 软件无法保证 Linux 控制的 DMA 不访问该物理内存。

核心考点： TEE 安全性最终取决于整条物理访问路径，而不是单个 CPU 的 PMP 配置。

## 题目 9 — Context Switch 中 FP/Vector 状态泄露

难度：Expert · Architecture / Confidentiality

场景：

某厂商把 OP-TEE 移植到带 RVV 的 RV64 SoC。

基础 TA 测试能够通过，但安全审计发现 OP-TEE 执行后的 Vector Register 内容可能被 Linux 读取。

你如何设计修复？

参考答案：

首先分析 Domain Switch 是否完整覆盖 Vector State，包括 Vector Register Group 及 `vl`、`vtype`、`vstart`、`vcsr` 等适用状态。

然后检查：

- `sstatus.VS` 状态。
- 保存和恢复路径是否对所有支持的 Vector 配置有效。
- Vector Context 内存分配大小与 Hart VLEN 是否匹配。
- 新 Context 是否已经安全初始化。
- FP/Vector 的 Lazy Save/Restore 是否有状态归属错误。
- Trap/Interrupt 期间是否可能发生嵌套的 Vector 使用。
- Hart 与 Domain Context 的绑定关系是否正确。

修复策略可以采用 Eager Context Switch 作为安全优先的基线，然后再评估 Lazy Switching 的复杂性和性能收益。

必须使用包含敏感测试模式的寄存器回归测试，覆盖首次进入、重复切换、跨 Hart 并发和异常退出。

加分点： 能够解释 CPU Context Switching 的功能正确性与跨安全域机密性保护之间的区别。

## 题目 10 — 设计一个支持动态 Domain 权限的 OpenSBI 扩展

难度：Principal-level · Architecture / RFC

场景：

产品要求 OP-TEE 能够在运行时申请一块额外的 Secure RAM，并允许在使用完成后安全归还给 Linux。

当前静态 Domain Policy 不满足要求。

请提出一份 OpenSBI RFC 的核心设计。

参考答案：

我会从 API、状态机、安全授权及验证四个方面设计。

API 层：

定义受信任的资源重新分配请求，明确：

- Caller Identity
- Source/Target Domain
- Physical Region
- Requested Permissions
- Operation ID
- Completion Status

状态机层：

```
OWNED_BY_LINUX
       |
       v
PREPARING
       |
       v
REVOKING_OLD_ACCESS
       |
       v
QUARANTINED
       |
       v
GRANTING_NEW_ACCESS
       |
       v
OWNED_BY_TEE
```

安全层：

必须确认旧权限完全撤销，处理未完成 DMA、旧映射、数据残留、超时与硬件配置失败。

如果发生不可安全恢复的错误，应进入 Quarantined 或 Fault State，而不是默认重新开放访问。

验证层：

提供：

- 多 Hart 权限撤销测试。
- Hardware Protection Fault Injection。
- DMA 并发测试。
- Interrupt Storm 测试。
- Stop/Resume 期间资源转移测试。
- 超时和重复请求测试。

加分点： 说明该 RFC 会影响 OpenSBI、OP-TEE、Linux 及平台硬件多个维护领域，因此需要早期跨项目设计讨论，而不是仅在 OpenSBI 增加一个 SBI Function ID。

## 题目 11 — 判断一次 Context Switch 是否需要 Global Lock

难度：Advanced · Lock Design / Performance

场景：

四核 OP-TEE 系统中，多次 Domain Switch 出现性能下降。

某工程师建议把所有 Domain Switch 都串行化，由一把 Global Domain Lock 保护。

你是否同意？

参考答案：

通常不赞成直接引入覆盖整条 Domain Switch 路径的全局大锁。

理由是：

- 每个 Hart 的 CSR Context 是独立的。
- Hart-local PMP 更新通常不需要跨核串行化。
- 一把 Global Lock 会增加延迟和锁竞争。
- 如果 Lock Scope 覆盖复杂的 Hardware Protection Callback，可能引入新的 Deadlock。
- 单个 Hart 的异常可能阻塞其他所有 Hart 的安全执行。

更合理的策略是根据状态归属分层同步：

| 状态类型                  | 同步策略                   |
| --------------------- | ---------------------- |
| Hart-local CSR        | 当前 Hart 独立操作           |
| Per-Domain Hart Mask  | 对应 Domain Lock         |
| 静态 Domain Policy      | 初始化后只读                 |
| 动态 Resource Ownership | 明确的跨 Hart 协议           |
| 共享设备状态                | 对应设备和 Bus Master 的同步机制 |

性能评估还需要测量 Context Switch 的实际延迟分布，而不能只看平均值：

\\[ T\_{\text{switch}} = T\_{\text{context}} + T\_{\text{protection}} + T\_{\text{fence}} + T\_{\text{contention}} \\]

这个公式属于分析模型，各项可能互相影响，不保证能够直接线性分解测量。

加分点： 能够提出 P50/P95/P99 Latency、Worst-case Contention、Cache Miss 和 IPI 开销等基准测试指标。

## 题目 12 — 将 OpenSBI + OP-TEE 移植到八核 RISC-V 汽车 SoC

难度：Principal-level · System Architecture / Industrial Delivery

场景：

一颗八核 RISC-V SoC：

- 8 个 Application Harts。
- OpenSBI 运行在 M-mode。
- Linux 和 OP-TEE 使用 Domain Isolation。
- 集成 WorldGuard 或类似系统级 Firewall。
- 存在独立 HSM Core。
- 支持 CPU Hotplug、DVFS、Deep Sleep。
- 有多个 DMA Engine 和安全加密外设。

要求你负责 OP-TEE 的系统级安全架构设计与移植。

如何推进项目？哪些条件决定平台能够安全交付？

参考答案：

我会分为五个工作阶段。

阶段 A：Security Architecture

首先明确 Threat Model：

- Linux Kernel 被完全攻破。
- 攻击者可以控制 Linux 所拥有的 DMA Engine。
- 攻击者能够发起任意合法/非法 SBI 请求。
- 攻击者可以尝试对 OP-TEE 进行异常和中断压力攻击。
- 不假设攻击者可以修改可信 Boot ROM、OpenSBI 和 HSM Firmware。

这决定哪些组件属于 TCB，以及哪些硬件权限必须由受信任的 Firmware 配置。

阶段 B：Resource Isolation

建立资源所有权表：

| 资源                   | 预期 Owner            | 保护手段                        |
| -------------------- | ------------------- | --------------------------- |
| OpenSBI Firmware RAM | M-mode Firmware     | PMP/ePMP                    |
| OP-TEE Private RAM   | TEE Domain          | PMP + System Firewall       |
| Linux RAM            | Linux Domain        | Domain Policy               |
| Shared Memory        | 按协议授权               | 受控映射和访问                     |
| HSM Mailbox          | HSM/Trusted Service | 安全 IPC                      |
| Crypto Engine        | 按平台授权               | Device Ownership / Firewall |

阶段 C：SMP Execution

建立并验证：

- 多 Hart Domain Context。
- Per-Hart Protection Configuration。
- Hart Assignment 与 HSM 权限。
- OP-TEE Trusted Thread 并发。
- Remote TLB Synchronization。
- IPI/Interrupt Routing。
- CPU Suspend/Resume。
- Cross-Domain Context Confidentiality。

阶段 D：Fault Handling

设计异常流程：

- Hardware Protection Failure。
- Domain Context Corruption。
- IPI Timeout。
- Hart Stop/Restart Failure。
- DMA Timeout。
- HSM Unavailable。
- Power Transition 中断。
- Secure Storage 或安全服务不可恢复错误。

每一种错误都应明确能否恢复、恢复的信任条件，以及何时必须停止相关安全服务。

阶段 E：Validation & Certification Evidence

除了普通功能测试，还需要：

- ISA / ABI / Firmware Compliance。
- Security Isolation Test。
- DMA/Peripheral Negative Test。
- 多核并发测试。
- Power Transition Test。
- Fault Injection。
- 性能与实时性测试。
- 安全配置 Review。
- 可追踪的测试报告和代码版本。

这道题的真正考点：

能否从 OpenSBI 某个函数、OP-TEE 某个驱动的局部视角，提升到覆盖 CPU、Firmware、Hardware Isolation、DMA、Power、HSM 和软件生命周期的系统安全架构视角。

这是 Staff/Principal Engineer 与普通 Platform Porting Engineer 之间最重要的能力区别之一。

# 十、Chapter 07 知识检查与学习成果

学完本章后，我建议重点检查以下知识是否真正掌握。

章节掌握检查

0 / 8

7.1

能区分 Per-Hart Context、Global Domain State 和硬件保护状态

7.2

能解释 Hart Assignment 的更新顺序和跨对象一致性问题

7.3

能解释同一 Domain 多 Hart 并发与 OP-TEE Trusted Thread 的关系

7.4

能解释为什么 PMP 是 Hart-local，以及保护配置的安全切换要求

7.5

能设计跨 Hart 的 Memory Permission Revocation Protocol

7.6

能分析 Domain Switch、HSM 和 CPU Hotplug 的状态关系

7.7

能定位 Lock Order Inversion、IPI Completion Deadlock 和同步问题

7.8

能建立 TEE SMP 安全不变量、Fault Injection 与验证矩阵

## 我建议你重点记住的五个结论

1\. Domain Context 是 Per-Domain × Per-Hart，而不是一个 Domain 只有一份 Context。

这是支持多核并发执行同一 Domain 的基础。

2\. Hart Assignment 的软件一致性与真实硬件隔离状态是两个不同的问题。

Hart Mask 正确不代表 PMP 一定正确；反过来，PMP 正确也不代表 IPI、HSM 和软件状态机没有问题。

3\. Hart-local Context Switch 通常不需要全局同步，但 Global Resource Revocation 必须有完整的跨 Hart、跨设备协议。

这正是 Chapter 07 最核心的架构区别。

4\. 安全隔离不仅需要保证正常路径，还需要保证错误路径。

尤其要重视 Hardware Protection 配置失败、Hart Restart、DMA 未完成访问、Context Initialization 和 Interrupt Race。

5\. SMP TEE 的最终正确性依赖明确的安全不变量，而不是依赖某个锁、某条 Fence 或某个 CSR。

一个值得长期使用的评审问题是：

> 在任意一个 Hart、任意一个 Domain Switch 中间点，以及任意一个并发执行的其他 Hart 上，系统能否保证不可信执行环境无法获得超出其授权范围的资源访问能力？

如果能够通过严格的硬件分析、源码分析与测试证明这个问题，你就已经进入了架构级 TEE 研究的范畴。

## 下一步的源码研究建议

完成 Chapter 07 后，我建议你不要马上跳到下一章，而是先围绕以下真实源码做一次完整的 Code Review：

- [sbi_domain_context.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain_context.c?utm_source=chatgpt.com)
- [sbi_domain.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_domain.c?utm_source=chatgpt.com)
- [sbi_hart_protection.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart_protection.c?utm_source=chatgpt.com)
- [sbi_ipi.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_ipi.c?utm_source=chatgpt.com)
- [sbi_hsm.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hsm.c?utm_source=chatgpt.com)

重点不是把函数逐个解释一遍，而是尝试对每条关键执行路径回答：状态归谁所有？由谁修改？需要哪些锁？何时硬件权限生效？发生异常怎样恢复？其他 Hart 能观察到什么？

这是将 Chapter 07 的知识转化为 OpenSBI / RISC-V OP-TEE upstream 能力的最有效方式之一。