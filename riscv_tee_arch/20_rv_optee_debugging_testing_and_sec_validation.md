# Chapter 20 — RISC-V OP-TEE Debugging, Testing & Security Validation

RISC-V OP-TEE Architecture & Porting · 高级工程实践篇 · Staff Engineer Level

## 本章定位与学习目标

前面的 Chapter 01–19 已经围绕 RISC-V OP-TEE 的核心机制建立了知识体系，包括：

- OpenSBI Domain、PMP 与硬件隔离。
- RISC-V Trap / Exception、Trusted Thread 与 Context Switching。
- MMU、TA Isolation、SMP 与 Interrupt。
- SBI MPXY / RPMI、RPC、Shared Memory。
- 真实 RISC-V SoC 的 OP-TEE 移植与 Hardware Root of Trust 集成。

Chapter 20 要解决一个不同层面的问题：

当这些组件组合成一个实际运行的系统之后，如何证明它们不仅能够正常工作，而且在异常、并发、恶意输入和硬件故障下仍然正确、安全、可恢复？

这是从 Firmware Developer 走向 Staff Engineer，以及从 OP-TEE Contributor 走向核心子系统维护者的重要分界线。

例如：

假设你已经成功将 OP-TEE 移植到了一个 16-hart RISC-V Server SoC。Linux 能够启动，`xtest` 基础用例也能通过。

但压力测试中偶尔出现以下问题：

- 某个 TA 执行 30,000 次后出现随机 Page Fault。
- Hart 7 从 OP-TEE Domain 返回 Linux Domain 后偶尔死机。
- Linux 对 OP-TEE 的 Secure Memory 发起访问，在某些配置下没有触发预期的访问异常。
- 两个 hart 同时进行 RPC 时，某个 Trusted Thread 永久处于 Suspended 状态。
- OP-TEE 在 QEMU 中稳定运行，但在 Palladium 上偶尔遭遇 DMA 数据不一致。
- Secure Storage 能够检测内容篡改，却没有阻止旧版本数据被整体回放。

这些问题都不能仅靠阅读代码或者增加 `DMSG()` 来解决。

我们需要建立一套系统的可观测性、调试、测试和安全验证方法。

### 本章目录

| 章节                                         | 核心问题               |
| ------------------------------------------ | ------------------ |
| 20.1 Logging、Trace 与 Panic Dump            | 如何观察 OP-TEE 内部行为   |
| 20.2 OpenSBI Trap Dump 与 Domain Debug      | 如何定位 M-mode 和隔离域问题 |
| 20.3 GDB 跨 M/S/U 特权级调试                     | 如何追踪真实执行路径         |
| 20.4 xtest、SBI Unit Test、Architecture Test | 如何验证基本正确性          |
| 20.5 MMU/PMP/Interrupt/Context Regression  | 如何证明核心机制不发生回归      |
| 20.6 SMP Stress Test 与 Race Detection      | 如何发现并发时序缺陷         |
| 20.7 Exception/Fault Injection/Fuzzing     | 如何主动制造异常和验证鲁棒性     |
| 20.8 Security Validation                   | 如何验证安全隔离和信息流       |
| 20.9 QEMU CI、Build Matrix、Automation       | 如何将验证转化为持续能力       |
| 20.10 Upstream Patch Engineering           | 如何提交经得起 Review 的修复 |

后面还将给出一个贯穿上述知识点的综合调试案例，并在章节结尾提供 Staff Engineer 级别的面试题及参考答案。

# 20.1 OP-TEE Logging、Trace 与 Panic Dump

## 20.1.1 为什么内核调试不能只依赖 printf？

对于普通应用，最常见的调试流程是：

```
发现问题
   ↓
增加 printf
   ↓
重新运行
   ↓
观察日志
   ↓
定位问题
```

但对于 OP-TEE，调试对象不只是一个普通进程，而是运行在可信执行环境中的内核。

增加一条日志可能产生以下影响：

1. 日志输出可能需要获得 Console Lock。
2. Console Driver 可能屏蔽当前 hart 的部分中断。
3. UART 输出速度比 CPU 执行速度慢得多。
4. 日志中可能包含敏感地址、密钥材料或安全执行状态。
5. 多个 hart 同时打印会改变并发执行顺序。
6. 如果异常发生时栈或内存已经损坏，打印函数本身也可能再次触发异常。

因此，在 TEE 内核中进行调试，必须先区分三种任务：

| 任务        | 最合适的手段                                       |
| --------- | -------------------------------------------- |
| 理解正常执行流程  | Trace、函数调用追踪、GDB                             |
| 定位异常与崩溃   | CSR、寄存器 Dump、Stack Unwind                    |
| 诊断并发和时序问题 | Event Trace、Per-Hart Ring Buffer、Stress Test |

其中一个非常重要的工程原则是：

> Instrumentation should minimize perturbation.

也就是调试手段应尽量减少对原始执行时序的干扰。

## 20.1.2 OP-TEE 的日志分层

OP-TEE 在 `lib/libutils/ext/include/trace.h` 中提供了不同等级的日志宏。

| Macro    | Level | 典型用途         |
| -------- | ----- | ------------ |
| `EMSG()` | 1     | 错误、异常和失败     |
| `IMSG()` | 2     | 初始化状态、重要运行事件 |
| `DMSG()` | 3     | 调试信息、状态变化    |
| `FMSG()` | 4     | 高频函数调用和详细流程  |

其编译期行为可以简化为：

```
/* 基于 OP-TEE trace.h 的简化示意 */

#if TRACE_LEVEL < TRACE_DEBUG
#define DMSG(...) (void)0
#else
#define DMSG(...) \
    trace_printf_helper(TRACE_DEBUG, true, __VA_ARGS__)
#endif
```

需要注意：这是编译期门控，不是所有日志都先编译进去再交给运行时过滤。

真实实现见 [OP-TEE trace.h](https://github.com/OP-TEE/optee_os/blob/master/lib/libutils/ext/include/trace.h?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



例如：

```
#include <trace.h>

void inspect_state(unsigned int id, unsigned int state)
{
    DMSG("thread=%u state=%u", id, state);
}
```

如果对应编译单元的 `TRACE_LEVEL` 低于 `TRACE_DEBUG`，这条日志将不会形成实际的日志调用。

### 核心构建选项

在 OP-TEE 的 `mk/config.mk` 中可以找到以下选项：

```
CFG_TEE_CORE_DEBUG
CFG_TEE_CORE_LOG_LEVEL
CFG_TEE_TA_LOG_LEVEL
CFG_TEE_CORE_TA_TRACE
CFG_DEBUG_INFO
CFG_CC_OPT_LEVEL
```

其中：

- `CFG_TEE_CORE_DEBUG=y`：打开部分 Debug Feature，包括额外的 Assertion、Lock Check 等。
- `CFG_TEE_CORE_LOG_LEVEL=4`：允许较详细的 Core Trace。
- `CFG_TEE_TA_LOG_LEVEL=3`：允许 TA 输出 Debug 等级日志。
- `CFG_DEBUG_INFO=y`：保留用于符号分析的 Debug Information。
- `CFG_CC_OPT_LEVEL=0`：降低编译优化以改善调试体验。

这些选项是不同维度，不能简单认为 `DEBUG=1` 等价于启用了全部日志与调试功能。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



例如在源码目录执行一次用于开发环境的 Debug Build：

```
make -j"$(nproc)" \
    ARCH=riscv \
    PLATFORM=virt \
    CROSS_COMPILE64=riscv64-linux-gnu- \
    CFG_TEE_CORE_DEBUG=y \
    CFG_TEE_CORE_LOG_LEVEL=4 \
    CFG_TEE_TA_LOG_LEVEL=3 \
    CFG_DEBUG_INFO=y \
    CFG_CC_OPT_LEVEL=0 \
    O=out/debug
```

这是 OP-TEE Core 的构建示例，不等于完整 Linux/OpenSBI/QEMU 系统构建；完整镜像仍需要平台 Buildroot 或其他集成脚本。

特别注意：

用于性能、时序或安全回归验证时，应使用接近产品配置的构建，而不是长期依赖 `-O0` 和最高日志等级的镜像。

## 20.1.3 理解 OP-TEE Core Panic

OP-TEE Core Panic 和 TA Panic 是两个不同层面的事件。

OP-TEE Core Panic

内核自身发生不可恢复的异常，可能涉及核心执行状态、内存管理、Thread 或安全服务完整性。

User TA Panic

发生在独立 TA Execution Context 中，通常应该终止相应 TA，而不是立即使整个 TEE Core 崩溃。

例如，一个 User TA 访问非法地址：

```
/* 仅用于隔离实验的测试 TA */
static void trigger_fault(void)
{
    volatile unsigned int *p =
        (volatile unsigned int *)0x1000;

    *p = 0x12345678;
}
```

如果该地址在当前 TA 地址空间中没有合法的可写映射，就应该触发相应的异常。具体是 Page Fault 还是 Access Fault，取决于地址翻译和底层物理访问保护状态。

在 RISC-V OP-TEE 中，关键源码入口之一是：

`core/arch/riscv/kernel/abort.c`

该文件实际包含：

- `set_abort_info()`：解析异常信息。
- `abort_is_user_exception()`：判断异常来源。
- `get_fault_type()`：进行异常分类。
- `handle_user_mode_panic()`：处理 User Mode 的不可恢复异常。
- `abort_handler()`：异常处理入口。

其中 User Mode Panic 处理会设置相应的返回状态，并把恢复地址调整到 `thread_unwind_user_mode`，使执行路径退出失效的 User Context。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里隐藏了一个重要设计思想：

TA 失效应该尽可能限制在 TA 的生命周期内，而不是破坏整个 TEE Kernel。

当然，如果 TA 故障暴露的是内核内存损坏或安全不变量失效，就不能简单将其当作普通 TA Crash。

## 20.1.4 RISC-V Exception Dump 应该包含哪些信息？

对于 RISC-V 平台，建议每次严重异常至少采集以下信息。

| 信息                    | 作用              |
| --------------------- | --------------- |
| Hart ID               | 确定异常发生在哪个 hart  |
| Privilege / Domain    | 确定执行时所处的安全环境    |
| `sepc` / `mepc`       | 定位异常指令          |
| `scause` / `mcause`   | 确定 Trap 类型      |
| `stval` / `mtval`     | 辅助分析故障地址或指令信息   |
| `sstatus` / `mstatus` | 确认特权状态及中断相关状态   |
| `satp`                | 确定地址转换上下文       |
| `sp` / `ra`           | 进行 Stack Unwind |
| Thread ID / TA UUID   | 关联具体执行上下文       |
| Firmware Build ID     | 确保日志与调试符号匹配     |

例如，假设在 OP-TEE 中得到如下人工构造的异常快照：

```
hart_id = 3
domain  = optee

sepc    = 0xffffffc0001056a0
scause  = 0x000000000000000f
stval   = 0x0000000088001000
sstatus = 0x0000000200000120
satp    = 0x8000000000123456
```

对于 RV64，`scause=15` 表示 Store/AMO Page Fault。

但这里不能立刻得出结论说「OP-TEE 缺少一条页表映射」。

还需要继续判断：

1. `sepc` 对应的是 Core 还是 TA 代码？
2. 该 Trap 之前运行的是 S-mode 还是 U-mode？
3. `stval` 指向哪种内存区域？
4. `satp` 对应哪套页表？
5. 页表项是不存在，还是存在但缺少 Write Permission？
6. 是否使用过 ASID，是否存在旧的 TLB Translation？
7. 是否涉及其他 hart 刚刚修改过 PTE？

对于 Page Fault，还要结合 `A/D` 位管理策略、SUM/MXR、用户态与内核态权限，以及当前实现的实际页表编码进行分析。

只有将异常现场、页表、执行上下文和最近的内存管理事件结合起来，才能形成可靠的根因判断。

## 20.1.5 Stack Unwind 与 Symbolization

如果 OP-TEE 输出类似：

```
Call stack:
  0xffffffc0001056a0
  0xffffffc0001028f0
  0xffffffc000101a60
```

这些地址本身没有太大的可读价值。

必须将它们转换为：

```
function_name()
    source_file.c:line
```

最简单的静态符号分析工具是：

```
riscv64-linux-gnu-addr2line \
    -e out/debug/core/tee.elf \
    -f -C \
    0xffffffc0001056a0
```

这里的 `tee.elf` 必须包含对应版本的符号和调试信息。

如果启用了 ASLR，不能直接假设运行时 VA 与 ELF 符号地址一致。需要根据启动时的实际加载地址、重定位偏移和 OP-TEE 的映射关系进行规范化。

OP-TEE 自带的 `scripts/symbolize.py` 可以辅助解析 Panic Dump：

```
cat optee_panic.log | \
    python3 optee_os/scripts/symbolize.py \
    -d optee_os/out/debug/core
```

这里的目录需要根据实际构建产物布局调整。`symbolize.py` 会根据提供的目录查找合适的 ELF 和符号信息；实际运行参数以对应版本的 `--help` 为准。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### 本节工程实践

建议你为 RISC-V OP-TEE 建立一份统一的 Crash Report 模板，至少包含：

- 完整异常寄存器。
- 发生异常的 Hart/Domain/Thread。
- 符号化 Call Stack。
- 相关 Memory Mapping 信息。
- 最后一次 Domain Switch。
- 对应的 Git Commit、Build Configuration 和 Toolchain。

这样一次偶现问题就不会只剩下一段无法复现的 Console Output。

# 20.2 OpenSBI Trap Dump 与 Domain Context Debug

## 20.2.1 为什么 OP-TEE 调试必须同时覆盖 OpenSBI？

在使用 OpenSBI Domain 构建 RISC-V TEE 时，OP-TEE 并不独立管理所有底层硬件安全状态。

例如：

```
Linux Domain
      |
      | SBI ECALL / TEE request
      v
OpenSBI M-mode
      |
      | Domain Context Switch
      | PMP / Isolation Reconfiguration
      v
OP-TEE Domain
      |
      | Trusted Thread / TA
      v
OP-TEE Execution
```

如果出现以下问题：

- Linux 无法进入 OP-TEE。
- Domain Switch 返回后 `satp` 错误。
- PMP 没有恢复为预期配置。
- 某个 Hart 在切换时进入非法状态。
- OP-TEE 尚未开始执行就触发异常。

仅仅打开 OP-TEE 的日志可能得不到任何信息，因为问题可能完全发生在 M-mode。

因此，调试必须分成三个独立观察面：

| 观察面     | 负责定位                                  |
| ------- | ------------------------------------- |
| Linux   | TEE Driver、请求参数、调度、RPC                |
| OpenSBI | SBI ECALL、Domain、PMP、Hart、M-mode Trap |
| OP-TEE  | Trusted Thread、TA、MMU、TEE Service     |

## 20.2.2 OpenSBI M-mode Trap Dump

OpenSBI 的实际源码：

`lib/sbi/sbi_trap.c`

包含 `sbi_trap_error()` 和 `sbi_trap_error_one()` 等异常输出逻辑。

其中 `sbi_trap_error_one()` 会输出 Hart ID、Trap Depth、`mcause`、`mtval` 和部分 GPR 状态；`sbi_trap_error()` 可以沿着嵌套 Trap Context 链输出异常现场，最后执行错误终止路径。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



为什么要关注 Trap Depth？

因为 M-mode 的问题可能不是第一次 Trap 造成的。

例如：

```
S-mode ECALL
    |
    v
M-mode Trap Handler
    |
    | Access invalid memory
    v
Nested M-mode Trap
    |
    | Fault while dumping state
    v
Further nested failure
```

只看最后一次 Trap，有可能把异常处理函数中的第二次故障误当成原始问题。

在 OpenSBI 中定位异常时，必须区分：

- 最初进入 M-mode 的 Trap 原因。
- 当前发生失败的 Trap 原因。
- 是否出现嵌套 Trap。
- 是否仍处于原来的 Domain。
- 是否已经修改了中断状态或硬件隔离状态。

## 20.2.3 结合真实源码分析 Domain Switch

OpenSBI 的关键文件：

`lib/sbi/sbi_domain_context.c`

当前实现包含 `struct hart_context`，负责保存同一 hart 在各 Domain 中的 Supervisor Context。

下面列出真实结构的主要字段（省略部分注释）：

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

这一结构说明：

Domain Context 保存的不仅是 PC 和 GPR，还有整个 Supervisor 执行环境的重要架构状态。

当前 `switch_to_next_domain_context()` 中实际存在如下逻辑：

```
ctx->sstatus = csr_swap(CSR_SSTATUS, dom_ctx->sstatus);
ctx->sie     = csr_swap(CSR_SIE, dom_ctx->sie);
ctx->stvec   = csr_swap(CSR_STVEC, dom_ctx->stvec);
ctx->satp    = csr_swap(CSR_SATP, dom_ctx->satp);
```

这里的 `csr_swap()` 可以理解为：把目标 Domain 保存的 CSR 值写入当前 hart，同时将旧值保存回原 Domain Context。

该函数还保存和恢复 FP/Vector Context、Trap Context，并重新配置 PMP 等硬件保护状态。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 调试 Domain Switch 时，最重要的不是多打印几个 CSR

而是验证一组 Domain Switch Invariants（切换不变量）。

假设当前 hart 从 Domain A 切换到 Domain B：

| 不变量                  | 必须保证                      |
| -------------------- | ------------------------- |
| Context Preservation | A 的完整架构状态能够正确恢复           |
| Context Isolation    | B 不应读取 A 的私有状态            |
| Memory Protection    | B 不能访问未授权的 A 私有内存         |
| Trap Routing         | B 的 Trap Vector 和中断配置符合预期 |
| Address Translation  | B 的 `satp` 与 TLB 状态匹配     |
| Hart Ownership       | Domain 与 Hart 归属状态一致      |
| Failure Safety       | 切换失败不能遗留扩大权限的半完成状态        |

### 一个非常重要的深入问题

观察源码可以发现，Domain 切换包含多项操作：

```
Update Domain Assignment
          ↓
Save/Restore Supervisor CSR
          ↓
Save/Restore FP / Vector State
          ↓
Save/Restore Trap Context
          ↓
Reconfigure Hardware Protection
          ↓
Execute Target Domain
```

这是便于理解的逻辑顺序，并不意味着所有步骤都是一条不可中断的硬件原子操作。

真正应该研究的是：

在某一步执行失败、触发 NMI、Double Trap 或其他异常时，如何防止系统处于权限与执行状态不一致的中间状态？

如果目标 Domain 的某个 Context 没有被正确初始化，或者目标 `satp` 与硬件隔离配置不匹配，可能出现：

- M-mode 继续执行，但 S-mode 进入错误地址空间。
- PMP 已经变化，但 Supervisor Context 仍然属于原 Domain。
- 一部分状态已经被覆盖，导致原 Domain 无法可靠恢复。

这些风险要结合具体硬件和实现分析，不能仅从 C 函数的正常返回路径证明安全。

## 20.2.4 如何设计 Domain Switch Trace？

我建议在实验分支中增加轻量的结构化事件，而不是无条件使用大量 UART 打印。

例如：

```
/* 教学示意：不是 OpenSBI upstream API */

struct domain_switch_event {
    uint64_t timestamp;
    uint32_t hartid;
    uint32_t from_domain;
    uint32_t to_domain;

    uintptr_t old_satp;
    uintptr_t new_satp;

    uint32_t phase;
    int32_t result;
};
```

建议设置以下 Phase：

```
SWITCH_BEGIN
CONTEXT_SAVE_DONE
CONTEXT_RESTORE_DONE
ISOLATION_UPDATED
SWITCH_COMMIT
SWITCH_FAILURE
```

这里的 `SWITCH_COMMIT` 是建议设计的调试事件，不代表 upstream 已经实现事务化 Domain Switching。

为什么建议使用 Ring Buffer？

因为一旦每次 Domain Switch 都调用 UART：

- 中断时间可能发生变化。
- Domain Switch Latency 被显著放大。
- 多 hart 竞争 Console Lock。
- 一些 Race Condition 可能因时序变化而无法复现。

采用 Per-Hart Ring Buffer，则可以将事件写入预留的诊断内存，之后再由受信任的 Debug 路径读取。

但这里还有一项安全约束：诊断内存本身必须受到保护，不能因为新增调试功能而让 Linux 读取 OP-TEE 的私有地址或数据。生产构建中还应禁止或严格控制此类敏感追踪。

### 本节实验

在 OpenSBI Domain Context Switch 路径中设置 GDB 断点，执行一次：

```
Linux → OpenSBI → OP-TEE → OpenSBI → Linux
```

分别记录两次切换前后的：

`mepc`、`mstatus`、`satp`、`stvec`、`sscratch`、Domain ID 和 PMP 配置。

然后验证：

Linux 恢复后的架构状态，是否与进入 OP-TEE 之前的预期状态完全一致？

不应该简单要求所有 CSR 位都逐位相等，因为某些状态可能有合法的生命周期变化。必须先定义哪些状态需要精确恢复，哪些状态允许按照 SBI/ISA 语义发生变化。

这一实验是后面 SMP Stress Test 与 Context Leakage Test 的基础。

# 20.3 GDB 跨 M/S/U Privilege 调试

这一节我们要解决的是：如何从指令级别追踪一次完整的 Linux → OpenSBI → OP-TEE → User TA 执行过程。

## 20.3.1 首先理解调试器处于哪一层

RISC-V 的 M-mode、S-mode、U-mode 是 CPU 执行特权级，不是三个独立 CPU。

因此，QEMU System Emulation 配合 GDB Remote Stub，可以从整个模拟 CPU 的角度观察程序执行。

但需要特别注意：

GDB 的 `info registers` 显示的是目标当前能够通过调试接口提供的寄存器状态；并不意味着所有 CSR 在所有硬件 Debug Implementation 上都可以无条件读取。

在真实芯片上，还要考虑 Debug Module、Authentication、生命周期状态以及 Security Debug Policy。

## 20.3.2 从 QEMU 启动阶段连接 GDB

此前使用的 RP016 Buildroot 实验环境提供了以下构建流程：

```
git clone \
    -b rp016_m1 \
    https://gitlab.com/riseproject/riscv-optee/buildroot.git

cd buildroot

make qemu_riscv64_virt_optee_defconfig
make -j"$(nproc)"
```

该分支的构建脚本会管理 OpenSBI、OP-TEE、Linux、U-Boot 和 QEMU 的集成。运行脚本为：

```
./output/images/start-qemu.sh
```

对应 Buildroot 文档还说明，OpenSBI/U-Boot 日志和 OP-TEE 日志可能位于不同 Console，因此不能假设所有日志都能从同一个终端看到。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



对于 GDB 调试，我们需要在 QEMU 启动命令中配置：

```
-S -gdb tcp:127.0.0.1:1234
```

其中：

- `-S`：CPU 在启动时保持暂停。
- `-gdb`：开启 GDB Remote Stub。
- `127.0.0.1`：仅在本机监听，避免调试端口暴露到网络。

具体应在集成脚本中找到 `qemu-system-riscv64` 的启动命令并加入上述参数，或使用脚本支持的调试选项；不建议直接假定某个未验证的脚本参数一定存在。

然后启动 GDB：

```
gdb-multiarch
```

```
set architecture riscv:rv64
set pagination off
target remote :1234

info threads
info registers
```

如果 QEMU 的 GDB Stub 把多个 hart 暴露为不同的 Thread，就可以使用：

```
info threads
thread 3
info registers
```

实际 Thread 编号取决于 QEMU 及 GDB 的呈现方式，不能直接认为 GDB Thread 3 就是 Hart 3。

## 20.3.3 如何调试多个 Firmware Image？

这里涉及一个 Embedded Debugging 中常见的问题。

整个 RISC-V 启动链中可能包含：

```
Boot ROM
   ↓
OpenSBI ELF
   ↓
OP-TEE Core ELF
   ↓
U-Boot / Linux
   ↓
User TA ELF
```

这些组件拥有各自的 ELF 文件和符号。

因此不能期望 GDB 只加载一个 ELF 就能够符号化整条执行链。

推荐先加载 OpenSBI 的调试符号：

```
file /path/to/opensbi/build/platform/generic/firmware/fw_jump.elf
```

如果需要额外加载 OP-TEE 的符号，可使用：

```
add-symbol-file /path/to/optee_os/out/debug/core/tee.elf
```

这里默认 ELF 所包含的链接地址与实际执行地址匹配；如果存在重定位或 ASLR，则必须使用正确的 Section Load Address。

例如可以先用：

```
riscv64-linux-gnu-readelf -SW \
    /path/to/tee.elf
```

确认 `.text`、`.rodata`、`.data` 的地址和大小，再根据启动后的实际映射进行符号加载。

尤其不要混淆 Link Address、Physical Load Address 和 Runtime Virtual Address。

## 20.3.4 在哪里设置断点？

我建议根据前面学过的执行路径，先关注以下真实源码位置：

| 组件      | 位置                     | 调试目标           |
| ------- | ---------------------- | -------------- |
| OpenSBI | `sbi_trap.c`           | M-mode Trap    |
| OpenSBI | `sbi_ecall.c`          | SBI ECALL 分发   |
| OpenSBI | `sbi_domain_context.c` | Domain 切换      |
| OP-TEE  | `entry.S`              | Core 启动        |
| OP-TEE  | `thread_rv.S`          | Trap/Thread 汇编 |
| OP-TEE  | `abort.c`              | 异常分类           |
| OP-TEE  | `core_mmu_arch.c`      | 地址转换和 MMU      |
| Linux   | TEE Driver             | REE 请求与返回      |

例如，调试 OP-TEE Exception：

```
break abort_handler
continue
```

触发异常后：

```
info registers
x/12i $pc
bt
```

还可以检查重要的 RISC-V CSR：

```
p/x $sepc
p/x $scause
p/x $stval
p/x $sstatus
p/x $satp
```

这些 CSR 是否可直接通过相应寄存器名访问，要以实际 GDB Target Description 和 QEMU 版本为准。若不可用，可通过保存异常现场的数据结构或 QEMU Monitor 等方式辅助观察。

### 一个关键区分：GDB 当前 PC 与 Fault PC

发生 Trap 后，GDB 显示的 `$pc` 可能已经进入异常入口或 `abort_handler()`。

而真正引发异常的指令地址往往保存在 `sepc` 或软件保存的 Exception Frame 中。

因此：

```
GDB $pc
    ≠
Faulting Instruction PC
```

尤其在嵌套异常场景下，直接对 `$pc` 做 `addr2line`，很可能只得到异常处理器的地址。

## 20.3.5 如何从真实汇编理解异常入口？

在当前 RISC-V OP-TEE 的 `thread_rv.S` 中，可以找到保存 Trap Context 的汇编代码。

其中一段核心逻辑是：

```
/* 摘录：保存部分异常 CSR */

csrr t0, CSR_XSTATUS
store_xregs sp, THREAD_ABT_REG_STATUS, REG_T0

csrr t0, CSR_XEPC
store_xregs sp, THREAD_ABT_REG_EPC, REG_T0

csrr t0, CSR_XTVAL
store_xregs sp, THREAD_ABT_REG_TVAL, REG_T0

csrr a0, CSR_XCAUSE
store_xregs sp, THREAD_ABT_REG_CAUSE, REG_A0

mv a1, sp
call abort_handler
```

这里 `CSR_XSTATUS`、`CSR_XEPC` 等是 OP-TEE 汇编使用的架构抽象宏，在对应构建模式下映射到实际 CSR。

从代码可以看出：

1. Trap Handler 先保存必要的寄存器。
2. 将异常 CSR 保存到 Stack 上的 Exception Frame。
3. 将 Exception Cause 放入 `a0`。
4. 将 Exception Frame 地址放入 `a1`。
5. 调用 C 函数 `abort_handler()`。
6. C 函数处理完成后，再恢复对应的寄存器并执行相应的 `XRET`。

这正好符合 RISC-V Calling Convention：`a0`、`a1` 负责前两个函数参数。

源码在 [thread_rv.S](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_rv.S?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 需要深入思考的问题

当 Trap 从 User TA 发起时，为什么不能直接使用 TA 的 User Stack 来处理所有内核异常？

因为 TA 的 Stack 是低可信执行上下文的一部分。其地址、权限、内容甚至可用性，都不应该被内核无条件信任。

OP-TEE 需要切换到受保护的内核执行上下文，并正确保存 User Context。

这也是 Stack Switching 不仅仅是程序执行细节，而是一项安全边界机制的原因。

# 20.4 xtest、SBI Unit Test 与 RISC-V Architecture Test

## 20.4.1 为什么需要三类不同的测试？

在一个完整的 RISC-V OP-TEE 平台中：

```
Hardware / ISA
       ↓
OpenSBI Firmware
       ↓
OP-TEE Kernel
       ↓
Linux TEE Driver
       ↓
CA / TA Applications
```

如果一项 `xtest` 测试失败，可能的原因包括：

- CPU 不符合预期的 RISC-V ISA 行为。
- OpenSBI 的 SBI Service 实现错误。
- PMP/Domain 配置存在问题。
- OP-TEE 的 Trusted Thread 或 MMU 出错。
- Linux TEE Driver 的参数传递发生问题。
- TA 自身存在 Bug。

因此需要按层拆分测试责任。

| Test Framework      | 测试对象             | 主要目标                            |
| ------------------- | ---------------- | ------------------------------- |
| RISC-V ACT          | CPU Architecture | ISA 及特权架构一致性                    |
| SBIUnit             | OpenSBI 内部模块     | Firmware 功能逻辑正确性                |
| Bare-metal SBI Test | SBI 接口           | SBI ECALL 行为与返回状态               |
| OP-TEE xtest        | OP-TEE/TEE 服务    | TA、Session、Crypto、Storage、RPC 等 |
| 系统级 Stress Test     | 全系统              | SMP、长时间运行、异常恢复                  |

三种核心工具并非互相替代，而是分别覆盖不同抽象层次。

## 20.4.2 OP-TEE xtest

`xtest` 来自 `optee_test` 仓库。

其主程序实现可见：

`host/xtest/xtest_main.c`

在 Linux 目标环境中，可以运行：

```
xtest
```

也可以选择测试等级：

```
xtest -l 0
xtest -l 15
```

当前主程序定义的 `-l` 范围是 0～15，0 为默认等级，15 包含可选等级的测试。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



执行指定回归用例：

```
xtest -t regression 4001 4003
```

排除指定用例：

```
xtest -t regression -x 1027 -x 1028
```

这里的具体用例编号和测试能力取决于当前 `optee_test` 的版本、平台配置及启用的 TA。对于缺少某项硬件能力的 RISC-V 目标平台，不应把所有被跳过的用例都简单视为通过。

### xtest 到底测到了什么？

举例来说，某个测试执行：

```
xtest
  |
  +-- TEEC_InitializeContext()
  |
  +-- TEEC_OpenSession()
  |
  +-- TEEC_InvokeCommand()
  |
  +-- TEEC_CloseSession()
```

在运行时，它会跨越：

```
Linux User Space
       ↓
Linux TEE Driver
       ↓
OpenSBI / Domain Transport
       ↓
OP-TEE Trusted Thread
       ↓
User TA
```

因此一个成功的测试能够间接验证整条路径中的某些功能。

但其局限性是：

xtest 通过，只说明该测试所执行的路径和检查条件满足预期，不代表所有 PMP、MMU、SMP 或安全状态都是正确的。

例如一个 PMP 配置错误可能并不影响任何正常 TA 调用，却允许恶意 Linux 访问 OP-TEE 物理内存。

普通功能测试就很难发现这一问题。

## 20.4.3 SBIUnit：测试 OpenSBI 的内部函数

OpenSBI upstream 包含 SBIUnit 测试框架。

其使用方式与典型 C Unit Test Framework 类似，例如：

```
/* 测试框架用法示意 */

static void check_value(struct sbiunit_test_case *test)
{
    SBIUNIT_EXPECT_EQ(test, 2 + 2, 4);
}
```

具体 Test Case 注册、Suite 声明和 Makefile 集成应参照当前版本的 [OpenSBI SBIUnit Guide](https://github.com/riscv-software-src/opensbi/blob/master/docs/writing_tests.md?utm_source=chatgpt.com)。

当前 SBIUnit 还支持针对静态函数的测试，以及对部分依赖结构进行 Mock。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



建议优先为以下逻辑增加 Unit Test：

- Domain Region 权限匹配。
- Hart 与 Domain 映射。
- Memory Region Overlap 处理。
- SBI 参数合法性验证。
- MPXY/RPMI 请求长度、边界和错误码处理。
- Domain Context 初始化与异常状态转换。

举例：

```
/* 教学伪代码：非 upstream 现成测试 API */

TEST(domain_permission_check)
{
    domain_A = create_domain();

    add_region(domain_A, 0x80000000,
               0x100000, READ | WRITE);

    EXPECT_TRUE(can_access(domain_A,
                           0x80001000, READ));

    EXPECT_FALSE(can_access(domain_A,
                            0x90000000, WRITE));
}
```

这个测试能够验证 Domain Policy 的软件判断逻辑。

但它还无法证明硬件 PMP 实际按照该 Policy 生效。

后者需要在模拟器或真实硬件上运行访问测试。

## 20.4.4 RISC-V Architecture Test

RISC-V 的 Architecture Certification Tests（ACT）旨在验证硬件实现是否符合相应 ISA 规范。

截至当前，ACT4 已成为新的测试框架，旧的 RISCOF 流程不应该被当作当前默认方案。ACT4 支持更多 Privileged Architecture、Exception、Interrupt 和 PMP 测试。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



对于 RISC-V TEE 平台，我建议重点关注：

1. PMP 权限匹配与访问异常。
2. Privilege Transition。
3. `mret` / `sret` 的状态行为。
4. CSR Access Permission。
5. Address Translation。
6. Exception Delegation。
7. Interrupt Enable、Pending 与 Delegation。
8. Memory Ordering 与 Atomic。
9. FP / Vector Context 相关 ISA 行为。

要注意：ACT 主要检验硬件规范符合性，不能代替 SoC Security Validation；其官方说明也明确指出需要额外进行设计验证。

### 如何设计分层测试策略？

对于一个“Linux 能读取 OP-TEE 私有内存”的问题：

- ACT：证明 CPU 的 PMP/访问异常机制在指定配置下符合规范。
- SBIUnit：证明 OpenSBI Domain 权限规则按预期计算。
- Platform Test：证明 OpenSBI 将正确的规则配置到了硬件。
- Security Test：证明 Linux 实际无法访问 OP-TEE 私有物理内存。

真正的安全结论必须建立在最后两个层次的测试结果上。

# 20.5 MMU、PMP、Interrupt 和 Context Switch 回归测试

这是 Chapter 20 的核心内容之一。

它关注的不仅是如何发现 Bug，更是如何建立一套不变量，让未来的任何代码修改都无法悄悄破坏核心安全机制。

## 20.5.1 什么是 Regression Test？

Regression Test 的目标是确认：

以前正确的行为，在引入新修改后仍然正确。

对于 OP-TEE，我建议从传统的功能预期扩展到 Security Invariants。

例如：

- 旧功能：TA 可以正常执行。
- 安全不变量：TA A 不能读取 TA B 的私有内存。
- 旧功能：Linux 可以请求 OP-TEE 服务。
- 安全不变量：Linux 无法通过共享内存参数绕过 TEE 的内存边界。
- 旧功能：Domain Switch 成功返回。
- 安全不变量：切换后的权限不会残留、扩大或泄露。

## 20.5.2 MMU Regression Test

建议覆盖：

| Test               | 预期结果                   |
| ------------------ | ---------------------- |
| TA 读取自身合法内存        | 成功                     |
| TA 写入只读页面          | Permission Fault       |
| TA 执行不可执行页面        | Instruction Page Fault |
| TA 访问未映射 VA        | Translation Fault      |
| TA A 访问 TA B 私有 VA | 不能获得 B 的数据             |
| S-mode Core 访问非法地址 | 进入正确异常路径               |
| PTE 更新后重新访问        | 使用符合规范的新映射             |
| 跨 Hart PTE 更新      | 无过期映射导致的越权访问           |

这里需要强调：TA A、TA B 可以拥有相同数值的虚拟地址，但映射到不同物理地址。

所以“TA A 能否访问 TA B 内存”的验证，必须基于物理内存身份和地址空间，而不能只看数值 VA。

### TLB Shootdown 实验

假设 Hart 0 和 Hart 1 运行使用同一地址空间的执行上下文。

首先：

```
VA X → PA A, RW
```

然后 Hart 0 修改：

```
VA X → Unmapped
```

Hart 1 如果继续使用旧 TLB Entry，就可能仍然访问 PA A。

推荐设计如下同步实验：

```
Hart 0                     Hart 1
  |                          |
  | Create mapping X→A       |
  |------------------------->|
  |                          | Access X
  |                          | Populate TLB
  |                          |
  | Remove mapping X         |
  | Local SFENCE.VMA         |
  | Remote shootdown ------->|
  |                          | SFENCE.VMA
  |<------ Acknowledge ------|
  |                          |
  | Signal test complete     | Access X again
  |                          | Expect Fault
```

这是具有必要同步协议的测试示意。

单纯在 Hart 0 上执行 `SFENCE.VMA`，不能保证 Hart 1 的旧 Translation 已经失效。

同时，PTE 修改的内存可见性与 TLB Invalidation 是两个有关联但不同的问题。测试必须保证页表更新、远程同步和测试线程之间存在正确的 Memory Ordering。

## 20.5.3 PMP Regression Test

PMP 是 Domain 隔离的重要机制。

建议建立一个 Access Matrix：

| 发起者            | 目标内存               | 期望        |
| -------------- | ------------------ | --------- |
| Linux Domain   | Linux RAM          | 按配置允许     |
| Linux Domain   | OP-TEE Private RAM | 拒绝        |
| OP-TEE Domain  | OP-TEE RAM         | 按配置允许     |
| OP-TEE Domain  | Shared Memory      | 受限允许      |
| OP-TEE Domain  | Linux Private RAM  | 仅允许显式授权区域 |
| 未授权 DMA Master | OP-TEE RAM         | 拒绝        |

注意最后一行不是 PMP 自身能够保证的能力，需要 IOPMP 或其他平台级 DMA/Bus Firewall。

对于 PMP Test，建议至少包含：

- Read / Write / Execute 权限组合。
- PMP Entry Priority。
- TOR/NAPOT 边界。
- Region Alignment。
- 多 Region Overlap。
- 最大/最小地址。
- Domain Switch 后的权限恢复。
- 非法配置和异常边界。

### 一个容易忽略的问题

在 M-mode 执行的访问测试不能直接证明 S-mode 访问权限。

普通 PMP 对 M-mode 的约束与 S/U-mode 不同，取决于 Entry Lock、Smepmp 等特性和配置。

所以 PMP 安全实验必须明确记录：

```
Access Privilege
Effective Privilege
PMP Configuration
Access Address
Access Type
Observed Trap
```

否则很容易把错误的测试结论当作硬件隔离证明。

## 20.5.4 Interrupt Regression Test

建议覆盖以下组合：

| 场景                     | 验证目标                         |
| ---------------------- | ---------------------------- |
| Interrupt Disabled     | 中断保持 Pending，不应被错误处理         |
| Interrupt Enabled      | 正确进入预期 Handler               |
| Domain Switch 期间到达     | 不丢失、不错误路由                    |
| 多个 Pending Interrupt   | 符合优先级与仲裁规则                   |
| Interrupt + RPC        | Trusted Thread 状态正确          |
| Interrupt + Page Fault | 异常处理状态不被破坏                   |
| 多 Hart MSI             | 正确的 Hart / Interrupt File 接收 |
| Interrupt Storm        | 系统可恢复、无永久饥饿                  |

特别是 Domain Switch 期间到达的 Interrupt，必须区分：

- Interrupt 首先由 M-mode 还是 S-mode 接收。
- 当前 `mideleg`、`mie`、`sie`、`mstatus`、`sstatus` 的状态。
- 当前 APLIC/IMSIC 的路由配置。
- Interrupt 属于哪个 Domain。
- Interrupt Controller 是否已经完成 Claim/Complete 或 EOI。
- Domain Switch 过程中是否暂时屏蔽或重新路由该 Interrupt。

不能只看“Interrupt Handler 有没有运行”，而要看是否由正确的安全实体执行了正确的处理。

## 20.5.5 Context Switch Regression Test

这是 RISC-V TEE 最值得投入的测试方向之一。

我们把需要验证的状态分为：

| Context        | 重要状态                                             |
| -------------- | ------------------------------------------------ |
| Integer        | GPR、SP、RA、PC                                     |
| Supervisor CSR | `sstatus`、`stvec`、`sepc`、`sscratch` 等            |
| Address Space  | `satp`、ASID、相关 TLB 状态                            |
| FP             | F Register、`fcsr`、FS State                       |
| Vector         | V Register、`vl`、`vtype`、`vstart`、`vcsr`、VS State |
| Security       | PMP/IOPMP Policy、Domain Ownership                |
| Interrupt      | Enable、Pending、Routing State                     |

### Context Pattern Test

一个很有效的方法是给不同 Domain 的架构状态设置不同模式。

例如：

```
Domain A:
    s2 = 0xAAAAAAAAAAAAAAAA
    s3 = 0x1111111111111111

Domain B:
    s2 = 0xBBBBBBBBBBBBBBBB
    s3 = 0x2222222222222222
```

反复执行：

```
A → B → A → B → A ...
```

每次恢复后检查：

```
A.s2 == 0xAAAAAAAAAAAAAAAA
A.s3 == 0x1111111111111111
```

对于 FP/Vector 也使用相同思路，填入独特的数据模式并验证。

但要注意：测试程序不能只是写两个 C 局部变量然后比较，因为编译器可能优化掉操作。必要时应使用专门的汇编 Test Harness，确保目标寄存器确实被写入、保存、恢复并检查。

对于 Supervisor CSR，则需要在具备相应权限的执行环境中进行测试。

### 一个真实而有价值的上游问题

OpenSBI 在 2026 年的实际 Issue 中已经出现了 Domain Context Switch 相关的 CSR 保存和 Vector 状态保存讨论，例如 `vl` / `vtype` Preservation。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这说明 Context Switch Regression Test 并不是抽象的学习题目，而是完全可以转化为真实 upstream 工作的方向。

对于你希望长期深入的 RISC-V OP-TEE，我建议将 Domain Context Switch + FP/Vector State + SMP 作为一个持续维护的测试专题。

# 20.6 SMP Stress Test 与 Race Detection

对于单核系统，一个错误往往可以通过固定执行路径复现。

但对于 16-hart RISC-V SoC，很多问题本质上是 Interleaving-dependent Bug，也就是只有当多个 hart 的执行顺序恰好满足某种条件时，错误才会出现。

## 20.6.1 Race Condition、Deadlock 与 Livelock

先区分三个概念：

| 问题             | 本质            | 典型表现         |
| -------------- | ------------- | ------------ |
| Race Condition | 结果依赖并发执行顺序    | 数据损坏、状态错误    |
| Deadlock       | 多个执行实体互相等待    | 永久阻塞         |
| Livelock       | 执行实体不断运行但没有进展 | CPU 忙碌、请求不完成 |

另外还需要关注 Starvation：系统整体仍然工作，但某些 Trusted Thread 长时间无法获得服务。

### 示例：Trusted Thread 分配中的竞争

假设两个 hart 同时请求一个空闲 Thread：

```
Hart 0                      Hart 1
  |                           |
  | Read thread.state = FREE  |
  |                           | Read thread.state = FREE
  |                           |
  | Write state = BUSY        |
  |                           | Write state = BUSY
  |                           |
  | Both use same thread      |
```

如果分配过程没有正确同步，就可能导致两个执行路径使用同一份 Thread Context。

真实的 OP-TEE Thread 分配有自己的并发保护；这里描述的是为了说明 Race Condition 而刻意简化的错误实现。

问题在于：

即使这样的错误存在，普通 `xtest` 也可能连续通过很多次，因为两个 hart 未必刚好同时进入关键区域。

## 20.6.2 如何设计高价值 Stress Test？

不要只运行：

```
while true; do xtest; done
```

这种方法可以发现一部分问题，但不容易建立可复现的故障条件。

更系统的方式是定义 Workload、Concurrency、Duration、Fault Condition 和 Progress Metric。

例如：

| Workload           | 并发规模       | 重点                     |
| ------------------ | ---------- | ---------------------- |
| CA → TA Invoke     | 1/2/4/8/16 | Thread 并发              |
| Open/Close Session | 2/8/16     | Session 生命周期           |
| TA Create/Destroy  | 4/8        | 资源分配和清理                |
| RPC Round-trip     | 2/8/16     | Thread Suspend/Resume  |
| Secure Storage     | 4/8        | 锁和数据一致性                |
| Domain Switch      | 1/8/16     | Context 保存恢复           |
| Mixed Interrupt    | 4/8/16     | Interrupt 与 Context 交错 |

并发压力不是越高越好。先在两个 hart 上稳定复现，再增加到 4、8、16 个 hart，往往更容易定位根因。

## 20.6.3 使用 Linux CA 产生并发压力

一个简单的测试框架可以采用多个 Linux Worker Thread，每个 Worker 重复调用：

```
TEEC_InitializeContext();
TEEC_OpenSession();

for (...) {
    TEEC_InvokeCommand();
}

TEEC_CloseSession();
TEEC_FinalizeContext();
```

真正实现时，可以让每个 Worker 使用独立的 Context 和 Session，减少客户端对象共享导致的额外问题。

然后记录：

```
worker_id
linux_tid
invoke_count
success_count
failure_count
last_error
latency
```

我建议进一步添加 `request_id`，并在 Linux Driver、OpenSBI、OP-TEE 各层能够安全地关联同一请求时记录它。

这样当系统出现一次超时，你就能知道：

```
Request #10293

Linux:
    TEE request submitted

OpenSBI:
    Domain switched to OP-TEE

OP-TEE:
    Thread allocated
    RPC requested

Linux:
    RPC returned

OpenSBI:
    No subsequent successful return observed
```

这些事件只能证明观察到的执行阶段，不能直接证明一定是哪个组件造成的故障，但已经可以显著收窄排查范围。

## 20.6.4 Lock Dependency Analysis

OP-TEE upstream 提供了可选的 Lock Dependency Checking。

在 `mk/config.mk` 中可以找到：

```
CFG_LOCKDEP ?= n
CFG_LOCKDEP_RECORD_STACK ?= y
```

其中 `CFG_LOCKDEP` 用于检测锁获取顺序违反预期的情况。启用后会引入明显的运行开销。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



例如：

```
Thread A:
    acquire(lock1)
    acquire(lock2)

Thread B:
    acquire(lock2)
    acquire(lock1)
```

形成了锁顺序循环：

```
lock1 → lock2 → lock1
```

如果这两条路径能够并发，就存在 Deadlock 风险。

但是需要注意：

Lockdep 主要用于分析被其跟踪的锁之间的依赖关系，并不能保证发现所有并发错误。

它通常不能仅凭锁顺序检查发现：

- 无锁数据结构的 Memory Ordering Bug。
- 缺少 Release/Acquire 的状态发布问题。
- 设备 DMA 引起的数据不一致。
- M-mode 与 S-mode 之间没有统一建模的锁依赖。
- 无法纳入同一 Lockdep 系统的 Cross-Domain Synchronization。

## 20.6.5 RISC-V Memory Model 为什么很重要？

对于你已经学习过的 RISC-V RVWMO Memory Model，可以直接构造如下典型问题：

```
/* 教学伪代码：错误的跨 hart 同步 */

shared_data = 0x1234;
ready = 1;
```

另一个 hart：

```
while (ready == 0)
    ;

assert(shared_data == 0x1234);
```

如果 `shared_data` 和 `ready` 是普通共享变量，而且缺少正确的同步，这在 C/C++ 内存模型下可能存在 Data Race，不能将其视为正确的并发程序。

即使把它们改成 `volatile`，也不意味着已经建立跨 hart 的同步关系。

一种正确的 C11 表达方式是：

```
#include <stdatomic.h>

static int shared_data;
static atomic_int ready;

void producer(void)
{
    shared_data = 0x1234;

    atomic_store_explicit(&ready, 1,
                          memory_order_release);
}

void consumer(void)
{
    while (!atomic_load_explicit(
               &ready, memory_order_acquire))
        ;

    /* Release/Acquire 建立同步关系 */
    assert(shared_data == 0x1234);
}
```

这里的关键不是“变量用了 Atomic”，而是：

- Producer 使用 Release 发布数据。
- Consumer 使用 Acquire 读取发布状态。
- 读到了对应的发布值之后，才读取受保护的数据。

在 RISC-V 上，编译器会根据目标 ISA 和 ABI 将相应操作映射到满足内存模型要求的指令序列。

但这只解决符合相关一致性条件的 CPU 内存访问问题，不自动解决 DMA 非一致性。

跨 hart 同步和 CPU-DMA Cache Coherency 是两个不同层次的问题。

### 本节实验

建议使用一个专用测试 TA，令 8 个 Linux Worker 同时调用同一个服务，重复执行：

```
Open Session
    ↓
Invoke
    ↓
RPC
    ↓
Resume
    ↓
Close Session
```

设计测试时要求记录：

- 每个 Worker 的成功次数。
- 最大请求延迟。
- 所有活跃请求的状态。
- 当前 Trusted Thread 使用量。
- 是否发生 Panic、Deadlock 或 Timeout。

然后分别比较正常 Build、Debug Build 和启用 Lockdep 的 Build。

如果只有高日志等级的 Build 才能正常通过，反而应该优先怀疑时序相关的问题。

# 20.7 Exception Injection、Fault Injection 与 Fuzzing

我们前面讨论的 Regression Test 主要验证预期功能。

但在真实工业系统中，还必须测试 异常条件下的行为。

## 20.7.1 Fault Injection 的设计思想

假设 OP-TEE 在执行一次 RPC：

```
OP-TEE
   |
   | RPC Request
   v
Linux
   |
   | Process Request
   v
OP-TEE Resume
```

如果 Linux 在处理 RPC 时崩溃、超时或者返回非法数据，OP-TEE 应该如何处理？

这正是 Fault Injection 要验证的问题。

一个成熟的异常测试不只是制造错误，而应包含四个组成部分：

1. Fault Trigger：在哪个位置注入错误。
2. Fault Model：模拟什么类型的失败。
3. Expected Behavior：系统应该如何响应。
4. Recovery Criteria：怎样判断故障处理已经完成。

## 20.7.2 Fault Injection 分类

| 类别               | 示例                             | 预期          |
| ---------------- | ------------------------------ | ----------- |
| CPU Fault        | Illegal Instruction、Page Fault | 正确分类与隔离     |
| Memory Fault     | 非法 VA、权限错误                     | 不发生越权访问     |
| Allocation Fault | 内存分配失败                         | 返回错误、释放资源   |
| IPC Fault        | 非法长度、错误版本                      | 拒绝或安全失败     |
| RPC Fault        | Timeout、服务终止                   | 有界失败，不永久死锁  |
| Interrupt Fault  | Delayed / Missing IRQ          | 状态不损坏       |
| Storage Fault    | Short Write、Corrupted Metadata | 检测损坏并安全处理   |
| Hardware Fault   | Crypto Engine Timeout          | 不泄露密钥或返回伪成功 |

其中最值得注意的是 Allocation Failure。

很多代码只验证了：

```
ptr = malloc(size);

if (!ptr)
    return ERROR;
```

但是如果函数之前已经申请了其他资源，失败路径是否正确释放这些资源？

例如：

```
Allocate A
    ↓
Acquire Lock
    ↓
Allocate B
    ↓
B Allocation Failure
```

需要检查：

- A 是否被释放。
- Lock 是否被释放。
- Thread State 是否恢复。
- Session Refcount 是否正确。
- 是否残留 Shared Memory Mapping。

这些才是异常路径测试的价值。

## 20.7.3 Fuzzing OP-TEE 的哪些接口？

我建议按信任边界分类，而不是随意对内部函数发送随机数据。

Untrusted Input

Linux CA、Shared Memory、RPC Response、RPMI Request、TA Input

Trust Boundary / Parser

Length Check、Memory Validation、State Check、Authorization

Trusted Execution

OpenSBI、OP-TEE Kernel、Secure Services

特别建议关注以下接口：

A. RPMI / MPXY Input Validation

- Message Length。
- Service Group / Function ID。
- Channel Ownership。
- Memory Offset。
- Shared Memory Bounds。
- 不支持的协议版本。
- 不合法的状态转换。

B. OP-TEE Message ABI

- 非法参数类型。
- 参数数量不一致。
- 错误的内存地址。
- Integer Overflow。
- Shared Memory 生命周期变化。
- 多个参数引用重叠区域。

C. TA Loader

- 错误 ELF Header。
- Section/Segment Offset Overflow。
- 无效的 Relocation。
- 不合法的内存权限。
- 伪造或损坏的 TA Metadata。

### Fuzzing 的重要限制

对于 Kernel 和 Firmware，完全依赖随机输入效率并不高。

更推荐：

- Grammar-aware Fuzzing。
- State-aware Fuzzing。
- Coverage-guided Fuzzing。
- 边界值与长度字段组合测试。

另外，应优先在隔离的 QEMU、Host-side Harness 或专用测试环境中运行。生产固件不能遗留可被普通 REE 利用的任意 Fault Injection 或 Debug 接口。

## 20.7.4 Fault Injection 的安全原则

在实际 SoC 平台上，Fault Injection 可能改变 PMP、Interrupt Controller、DMA 或 Secure Storage 状态。

因此实验必须满足：

- 使用独立测试固件。
- 不使用生产密钥。
- 不连接真实用户数据。
- 具有可重复的恢复手段。
- 在测试完成后恢复已知良好的硬件状态。
- 明确区分测试导致的 Panic 和真实系统故障。

对于模拟器中的不可信内存访问和异常输入，可以较自由地构造边界测试；对于硬件故障、Secure Boot 和非易失性安全状态，则需要更严格的实验环境隔离。

# 20.8 Context Leakage、Invalid Pointer 与 TOCTOU Security Test

这一节从功能正确性转向安全正确性。

一个很重要的区别是：

系统没有 Crash，不代表系统是安全的。

例如：

Linux 通过一个不应该被允许的物理地址成功读到了 OP-TEE 的一部分内存。

整个系统可能没有任何异常，但这已经是严重的安全边界失效。

## 20.8.1 Context Leakage：什么是跨 Domain 状态泄露？

假设 Domain A 和 Domain B 在同一个 Hart 上交替执行。

Domain A 的寄存器中包含一段敏感数据。

如果 Domain Switch 没有完整地保存、恢复或清理相关执行状态，Domain B 就可能观察到不属于自己的数据。

潜在泄露面包括：

- Integer Register。
- FP Register。
- Vector Register。
- CSR。
- Scratch Register。
- Temporary Stack。
- Shared Debug Buffer。
- Hardware Performance Counter。
- Cache / Microarchitectural State。

这里必须区分两种性质不同的泄露：

| 类型                         | 示例                        | 处理方式            |
| -------------------------- | ------------------------- | --------------- |
| Architectural Leakage      | Vector Register 恢复错误      | 正确保存、恢复和清理状态    |
| Microarchitectural Leakage | Cache Timing Side Channel | 额外的硬件、调度和软件缓解机制 |

正确的 Register Save/Restore 并不能自动证明系统已经防御了所有 Side Channel。

## 20.8.2 Context Isolation Test

可以设计两个测试 Domain：

```
Domain A
    |
    | Write secret pattern to Vector Registers
    |
    | Domain Switch
    v
Domain B
    |
    | Write its own Vector Pattern
    |
    | Domain Switch
    v
Domain A
    |
    | Verify original pattern
```

但这个测试仍然不够。

因为它主要验证 Domain A 的状态是否被正确恢复。

还应该验证：

Domain B 是否有机会观察到 Domain A 的状态？

具体测试要基于实际 Context 初始化、FP/Vector Enable 状态、Domain 生命周期和架构规定的可见性进行设计。

尤其对于首次进入一个 Domain 的场景，必须确认未初始化的寄存器状态不会成为信息泄露途径。

## 20.8.3 Invalid Pointer 与 Shared Memory Validation

假设 Linux 为 OP-TEE 提供一个 Shared Memory Buffer：

```
PA: 0x90000000
Size: 0x1000
```

然后传入：

```
offset = 0xFF0
length = 0x100
```

很显然：

```
offset + length > buffer_size
```

如果 OP-TEE 没有正确检查，访问就可能超出允许的内存区域。

正确的边界检查不应该依赖可能溢出的加法：

```
/* 不够安全的写法 */
if (offset + length > buffer_size)
    return ERROR;
```

应该避免整数溢出，例如：

```
/* 适用于无符号 size_t 的安全边界检查 */

static bool valid_range(size_t offset,
                        size_t length,
                        size_t buffer_size)
{
    if (offset > buffer_size)
        return false;

    if (length > buffer_size - offset)
        return false;

    return true;
}
```

这个检查避免了 `offset + length` 产生整数溢出。

但仍然需要额外验证：

- Buffer 是否实际属于调用者。
- 是否属于允许共享的内存区域。
- 是否已经注销或撤销授权。
- 访问权限是否与参数要求一致。
- 地址加法、映射、物理连续性等其他条件是否成立。

也就是说，Bounds Check 只是 Memory Validation 的一个组成部分。

## 20.8.4 TOCTOU：为什么检查通过仍然可能不安全？

TOCTOU = Time Of Check To Time Of Use。

考虑以下执行流程：

```
OP-TEE                         Linux
   |                              |
   | Read shared header           |
   | Check length = 64            |
   |                              |
   |                         Modify length
   |                         from 64 to 4096
   |                              |
   | Read shared header again     |
   | Use length = 4096            |
```

这是一个典型的 Double Fetch 问题。

OP-TEE 虽然检查了参数，但使用时又从 REE 可写内存中读取了一次。

攻击者可以在两个时间点之间修改数据。

### 正确的设计思想

对于较小且结构化的输入，一种常见方式是：

```
/* 教学示意，非 OP-TEE 真实接口 */

struct request local_req;

copy_from_untrusted(&local_req,
                    untrusted_req,
                    sizeof(local_req));

if (!validate_request(&local_req))
    return ERROR;

/* 后续使用已验证的本地副本 */
process_request(&local_req);
```

关键是将需要共同验证的字段复制到受保护的本地快照中，然后基于这份快照完成检查和使用。

但要注意：

- 一次结构体拷贝不自动保证任意并发更新下的协议一致性。
- 如果结构体中包含指向其他不可信 Buffer 的指针，需要分别复制和验证。
- 如果使用过程中还需要访问外部可变数据，必须重新定义其所有权和同步协议。
- 单纯禁止编译器优化或者增加 `volatile` 不能解决 TOCTOU。

对于用户态 TA 访问不可信共享内存，也需要遵循相同的安全原则。

## 20.8.5 设计 Security Test Matrix

对于一个 RISC-V OP-TEE Port，我建议至少建立如下矩阵：

| 安全边界                           | 测试                            |
| ------------------------------ | ----------------------------- |
| Linux → OP-TEE Memory          | 非授权物理访问                       |
| TA A → TA B                    | 跨 TA 内存访问                     |
| TA → OP-TEE Core               | 越权执行与数据访问                     |
| Domain A → Domain B            | Context Leakage               |
| Linux → Shared Memory          | TOCTOU / Double Fetch         |
| DMA Master → TEE RAM           | DMA Firewall                  |
| Linux → HSM Service            | 非授权请求及密钥操作                    |
| Debug Interface → Secure State | Debug Authentication / Policy |

最后一个测试尤其容易在开发阶段被忽略。

在真实芯片中，JTAG/Debug Module、Trace Port 和 Crash Dump 是否能够访问 Secure Memory，需要有明确的安全生命周期策略。

开发阶段允许的调试能力，不应该无条件保留到生产状态。

# 20.9 QEMU CI、Build Matrix 与 Test Automation

前面设计了大量测试，但如果它们只能由工程师手动运行，长期价值有限。

对于 upstream 开发，必须把测试变成可重复执行的自动化流程。

## 20.9.1 为什么只做 Build CI 不够？

假设某个 PR 修改了：

```
core/arch/riscv/kernel/thread_rv.S
```

CI 显示：

```
RISC-V Build PASSED
```

它只能说明代码能够通过指定配置的编译和链接。

它不能证明：

- Context 保存正确。
- RPC 能够恢复。
- 多 Hart 不会 Deadlock。
- FP/Vector State 不泄露。
- TLB Invalidation 正确。

事实上，OP-TEE 当前 upstream CI 已包含针对 RISC-V `virt` 平台的不同编译配置，如 PLIC、APLIC、IMSIC 等；工作流中也有 QEMU Runtime Test，但不能把 ARM QEMU 测试的存在直接等同于所有 RISC-V 组合均已得到运行时测试覆盖。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此，我建议将 CI 设计成三个等级。

Level 1 — Compile & Static Checks

每次 PR：编译、静态检查、配置合法性、代码风格

Level 2 — QEMU Functional & Security Regression

完整启动、xtest、Domain、MMU、SMP、Fault Tests

Level 3 — Hardware / Emulator Validation

ZeBu/Palladium/FPGA/Silicon：真实中断、DMA、缓存、设备、安全策略

## 20.9.2 RISC-V OP-TEE Build Matrix

推荐的起始矩阵：

| 维度           | 配置                 |
| ------------ | ------------------ |
| Architecture | RV64               |
| Platform     | QEMU virt、实际 SoC   |
| Toolchain    | GCC、LLVM（支持时）      |
| Interrupt    | PLIC、APLIC、IMSIC   |
| CPU Count    | 1、2、4、8、16         |
| MMU          | Sv39、其他实际支持的分页模式   |
| FP           | Enabled / Disabled |
| Vector       | Enabled / Disabled |
| Debug        | Debug / Release    |
| Isolation    | 目标 SoC 支持的安全机制     |

需要注意：这是测试覆盖的候选维度，不是要求所有维度做完全笛卡尔积。

例如，如果某个平台不支持 IMSIC，就不应该强行在该平台执行 IMSIC 测试；如果编译选项或硬件组合互斥，也必须显式声明。

可采用：

- 每次 PR：少量高价值组合。
- 每日运行：较完整的 QEMU 组合。
- 每周或重要合并前：长时间 SMP Stress。
- 发布前：实际硬件安全验证。

## 20.9.3 如何构建可复现 CI？

一个合格的 CI Test Run 应记录：

```
optee_os_commit
opensbi_commit
linux_commit
optee_test_commit

toolchain_version
qemu_version

platform_configuration
device_tree_hash
firmware_image_hash

test_name
test_result
test_duration

console_log
panic_dump
```

为什么要记录所有组件的 Commit？

因为 RISC-V OP-TEE 是跨多个仓库协作的系统。

同一个 OP-TEE Commit，在不同版本的 OpenSBI、Linux TEE Driver、RPMI ABI 或 Device Tree 下可能表现完全不同。

只记录 `optee_os` 的 Commit，往往无法重建故障。

## 20.9.4 自动化测试的具体示例

RP016 实验环境可以通过：

```
./output/images/start-qemu.sh
```

启动系统，然后在 Linux 中执行：

```
xtest -l 0
```

可以使用 Python 或 Shell Harness 自动启动 QEMU、等待 Console Ready、执行测试并采集结果。

以下是自动化 Harness 的逻辑伪代码：

```
# 教学示意：需要接入具体 QEMU Console 控制器def run_test(config):    qemu = start_qemu(config)    try:        wait_for_linux_prompt(qemu, timeout=120)        result = qemu.run_command(            "xtest -l 0",            timeout=600        )        assert result.exit_code == 0        assert result.summary_failed == 0        save_logs(config, result)    finally:        qemu.stop()
```

这里有一个很重要的细节：

不能仅仅依赖 Console 是否包含 `PASS` 字符串来判断整套测试成功。

更可靠的判断包括：

- 测试进程退出码。
- 失败用例数量。
- 测试是否完整结束。
- 是否有 Unexpected Skip。
- 是否出现 Panic 或 Timeout。
- 是否产生内核 Oops、异常退出等附加错误。

## 20.9.5 QEMU 与真实硬件之间的验证鸿沟

QEMU 很适合用于：

- 指令执行逻辑。
- ABI。
- 大量软件异常路径。
- Trusted Thread Lifecycle。
- MMU 和部分 PMP 行为。
- 可控的多 Hart 测试。

但是，它不一定能够精确反映：

- SoC Cache Coherency Fabric。
- DMA / IOMMU / IOPMP 的具体实现。
- 实际 Interrupt Latency。
- 电源管理时序。
- NoC / AXI Ordering。
- 真实 Security Firewall。
- 硬件 Errata。

因此在 ZeBu/Palladium 或实际 Silicon 环境中，应特别增加：

DMA、Interrupt、Memory Coherency、Power/Reset、Hardware Security Policy 的验证。

此外，Emulator 比真实芯片运行慢，某些微秒级 Race 在 Emulator 上可能根本无法以相同方式复现。

Staff Engineer 需要为不同执行环境制定不同的验证目标，而不是简单把一套 QEMU Test 原样复制到所有平台。

# 20.10 从 Bugfix 到 Upstream Patch：完整工程实践

这一节直接服务于你长期希望成为 OP-TEE 社区维护者的目标。

## 20.10.1 什么是高质量的 Upstream Bugfix？

一个 Patch 不应该只包含：

```
Fix RISC-V crash.
```

然后改动几行代码。

一个能够让 Maintainer 信任的 Bugfix，需要回答：

1. 问题是什么？
2. 在什么配置下发生？
3. 最小复现条件是什么？
4. 根因是什么？
5. 为什么这个修改可以解决根因？
6. 有没有引入新的安全问题？
7. 如何确保未来不会回归？

尤其对于 OP-TEE 内核，建议做到 Reproducer + Root Cause + Fix + Regression Test 四者闭环。

## 20.10.2 一个典型的 RISC-V Context Switch Bugfix

假设某次多 Domain 切换后，发现某个 Vector Control CSR 没有被正确保存或恢复。

正常预期：

```
Domain A: Vector Configuration A
Domain B: Vector Configuration B

A → B → A

Expected:
    Restored Configuration A
```

实际观察：

```
A → B → A

Observed:
    Configuration B or invalid state
```

正确的处理不是立即在某个恢复点随意增加一次 CSR Write。

需要先分析：

- 该 CSR 是否属于需要由当前切换层管理的状态。
- 是否属于架构定义的可写或只读字段。
- 是否应该与 Vector Register 一起保存。
- CPU 是否实际支持相关 Extension。
- 访问 CSR 时需要满足哪些 Enable State。
- 是否存在 Lazy Context Switch。
- 是否需要在首次切入 Domain 时初始化。

然后才修改对应 Context Save/Restore 路径。

这一案例的技术性质与 OpenSBI upstream 中已经公开讨论过的 Vector Domain Context 问题相近。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 20.10.3 设计 Regression Test

理想的测试结构：

```
TEST_BEGIN

    Initialize Domain A Context
    Initialize Domain B Context

    Write Pattern A
    Switch to B

    Write Pattern B
    Switch to A

    Verify Pattern A

    Repeat 1000 times

TEST_END
```

进一步扩展：

```
A → B → A
A → B → C → A
A → B → A → B
```

如果涉及 SMP，还需要测试不同 Hart 同时发生 Domain Switch 的情况。

此外，建议分别验证：

- 启用了 Vector Extension。
- 没有启用 Vector Extension。
- Vector Context 首次使用。
- Vector Context 已经初始化。
- 异常和中断打断切换。
- FP 与 Vector State 同时启用。

## 20.10.4 如何写清晰的 Commit Message？

例如：

```
riscv: preserve vector control state on domain switch

A domain switch must preserve the architectural
vector context of the outgoing supervisor domain
and restore the context of the incoming domain.

Without preserving the required vector control
state, a resumed domain may execute with stale
configuration inherited from another domain.

Save and restore the missing state as part of
the domain context transition.

Add a regression test that alternates between
domains with different vector configurations
and verifies the restored state.

Signed-off-by: Developer Name <developer@example.com>
```

这是教学示例，不代表已经存在或已经验证的具体补丁。

关键是 Commit Message 必须解释问题机制，而不仅是描述改动了哪些文件。

## 20.10.5 一个 Bugfix 的完整流程

1. Reproduce

   在原始代码上稳定复现问题。
2. Minimize

   减少并发、输入和配置，建立最小化复现条件。
3. Analyze

   获取异常现场、验证假设、明确根因。
4. Write Regression Test

   首先让测试在旧实现上可靠失败。
5. Implement Fix

   以尽可能小的改动修复根因。
6. Validate

   运行功能、异常、安全和受影响架构的相关测试。
7. Review & Upstream

   提交 PR，解释设计依据、已验证范围及剩余限制。

### 为什么要先写一个会失败的 Regression Test？

因为它能证明：

这个测试确实能够检测到你准备修复的 Bug。

如果测试在旧版本和新版本上都通过，那么它可能根本没有覆盖到导致问题的条件。

对于安全修复，还需要考虑公开 Reproducer 是否会暴露尚未修复的可利用漏洞。涉及未公开安全问题时，应遵循项目的安全披露流程。

# 20.11 综合实战：16-Hart RISC-V OP-TEE 偶发 Context Corruption

我们用一个完整的工业场景把前面十节连接起来。

这是一个构造的教学案例，用于演示调查方法，并非声称某款实际 SoC 存在该故障。

## 20.11.1 问题背景

系统结构：

```
16-Hart RISC-V Server SoC
        |
        +-- OpenSBI M-mode
        |      |
        |      +-- Domain Isolation
        |      +-- PMP / Hardware Firewall
        |
        +-- Linux Domain
        |
        +-- OP-TEE Domain
                 |
                 +-- Trusted Thread
                 +-- User TA
                 +-- RPC
```

基本功能测试全部通过。

但在启用 Vector Extension 后，多核压力测试偶尔发生：

```
TA operation result mismatch
```

如果关闭 Vector Extension，则错误不再出现。

## 20.11.2 初步调查

第一步：确认问题是否真正与 Vector Context 有关。

构造对照实验：

| 配置                      | 结果     |
| ----------------------- | ------ |
| 1 Hart，Vector Disabled  | 通过     |
| 1 Hart，Vector Enabled   | 通过     |
| 8 Harts，Vector Disabled | 通过     |
| 8 Harts，Vector Enabled  | 偶发错误   |
| 16 Harts，Vector Enabled | 错误概率提高 |

这说明问题与 Vector 和 SMP 存在相关性，但还不能证明根因就在 Vector Save/Restore。

第二步：确认错误发生在哪一层。

需要比较：

- TA 的 Vector Context。
- OP-TEE Trusted Thread Context。
- OpenSBI Domain Context。
- 各个 hart 的 Vector CSR。
- Domain Switch 前后的实际状态。

第三步：增加最小化测试。

不执行 Crypto、Storage、RPC 等复杂功能，而是仅执行：

```
Domain A
   → Set Vector Pattern A

Domain B
   → Set Vector Pattern B

Domain A
   → Verify Pattern A
```

如果该最小测试也会失败，问题范围将明显缩小。

## 20.11.3 精确定位

在 GDB 中设置断点：

```
switch_to_next_domain_context()
```

然后检查：

```
Before Switch:
    Current Domain
    Target Domain
    Current Hart
    Vector Registers
    Vector CSRs
```

再观察切换后的恢复结果。

假设最终发现某个必须保存的 Vector Control CSR 没有被保存。

这就形成了可以验证的 Root Cause Hypothesis：

> Domain Context Save/Restore 缺失部分架构状态，导致切换后的执行上下文使用了不正确的 Vector Configuration。

下一步必须通过测试证明这一假设，而不是因为它看起来合理就直接宣布找到根因。

## 20.11.4 修复后的验证

至少执行以下测试：

- 单 Hart Context Switch。
- 16-Hart Context Switch。
- Vector Enabled / Disabled。
- FP + Vector Combined。
- Interrupt During Domain Switch。
- 高并发 TA Invoke。
- 长时间运行。
- 故障注入和错误路径检查。

最后将原始失败用例加入自动化测试。

如果最终修复涉及 OpenSBI Domain Context，那么代码变更可能应该提交到 OpenSBI，而不是 OP-TEE。

能够判断 Bug 属于哪个维护边界，也是跨系统 Staff Engineer 必须具备的能力。

# 20.12 Staff Engineer 级别面试题与参考答案

下面的题目不以概念记忆为主，而是要求分析真实系统中的 Security Boundary、Runtime State、Concurrency 和 Root Cause。

## Question 1 — OP-TEE 在 16-Hart 平台上偶发 Page Fault，如何定位？

场景：

OP-TEE 在 QEMU 单 Hart 模式正常运行。启用 16 Hart 后，某 TA 偶尔发生 Store Page Fault。

错误通常发生在另一个 Hart 修改页表之后。

参考答案：

首先需要确定 Faulting VA、Faulting PC、`satp`、ASID、PTE 以及发生异常的 Hart。

然后分析三个方面：

1. 页表更新是否正确，新的 PTE 是否已经对其他 Hart 可见。
2. 是否对可能缓存旧 Translation 的所有相关 Hart 执行了必要的 TLB Invalidation。
3. PTE 所在的内存是否使用了正确的同步协议。

重点考虑 Remote TLB Shootdown。

如果只有修改 PTE 的 Hart 执行了 `SFENCE.VMA`，其他 Hart 仍有可能持有旧的地址转换状态。

验证时可以固定两个 hart，构造一个建立映射、远程访问、撤销映射、同步、再次访问的最小实验。

进一步还要检查 ASID Reuse、页表页释放顺序，以及是否发生了访问与映射生命周期之间的竞争。

优秀的答案应区分：

PTE Memory Visibility、TLB Invalidation 和 Page Table Lifetime 三个问题。

## Question 2 — 如果 Domain Switch 的中间步骤失败，怎样保证安全？

场景：

OpenSBI 正在从 Linux Domain 切换到 OP-TEE Domain。

此时部分 CSR 已经恢复，但 PMP 尚未完成重新配置。

参考答案：

首先分析当前实现允许在哪些位置失败或被异步事件打断，以及哪些操作实际上不会返回。

不能直接假设 C 函数是一项原子操作。

设计上应明确：

- 何时停止不可信 Domain 的执行。
- 哪些状态可以安全地保存。
- 何时改变物理访问权限。
- 何时允许目标 Domain 开始执行。
- 失败后能否恢复原始状态。

在安全上，最危险的是：

某个 Domain 获得了不属于自己的访问权限，同时拥有执行机会。

因此需要建立严格的权限与执行状态约束。

对于无法安全恢复的中间状态，安全停止可能比尝试继续运行更合理。

验证手段包括异常注入、状态快照、不变量检查，以及在支持的环境中验证 NMI/Double Trap 场景。

## Question 3 — 为什么 PMP Unit Test 通过仍不能证明 TEE 安全？

参考答案：

PMP Unit Test 可能只证明软件计算的访问权限符合预期。

但实际安全性还取决于：

- PMP CSR 是否正确写入。
- PMP Region Priority 和 Address Encoding 是否正确。
- CPU 实际访问权限是否符合预期。
- M-mode、S-mode 和 Effective Privilege 是否正确。
- Domain Switch 是否及时更新 PMP。
- 其他 Hart 是否保持一致的安全配置。
- DMA Master 是否受到独立保护。

所以至少需要软件 Policy Test、硬件访问测试、跨 Domain 测试和 DMA/Peripheral Isolation Test。

Software Policy Correctness 不等于 Hardware Enforcement Correctness。

## Question 4 — 为什么打开 DMSG 之后 Race Condition 消失？

参考答案：

这是典型的 Heisenbug。

UART 打印可能引入 Console Lock、Interrupt Masking 和额外执行延迟，从而改变线程执行顺序。

因此错误消失并不证明修复成功。

应采用低开销事件追踪、固定输入、控制并发规模，并尽量避免在关键窗口打印。

同时分别比较 Debug Build 和 Release Build。

如果需要寻找 Race Window，可以在受控测试环境中人为改变某些同步点的时序，但不能把加入随机延迟当作最终修复方案。

最终必须通过 Memory Ordering、Locking 或 Lifecycle Management 层面的正确性证明解决问题。

## Question 5 — OP-TEE Core Panic 与 TA Panic 有什么安全区别？

参考答案：

TA Panic 发生于较低权限的 Trusted Application Context。

正常情况下，应能够终止相关 TA 并清理其 Session、内存和资源。

而 Core Panic 表示内核安全执行环境发生严重错误。

如果是核心状态或安全不变量受到破坏，就不能保证继续执行仍然安全。

因此二者的错误处理策略不能完全相同。

还必须避免恶意 TA 通过普通非法输入轻易触发整个 TEE Core 的不可恢复崩溃，否则可能形成 Denial-of-Service 风险。

对于受影响的服务，返回 `TEE_ERROR_TARGET_DEAD` 等错误只是接口层面的结果；真正重要的是内部状态是否得到正确清理。

## Question 6 — 如何验证 FP/Vector Context 不泄露？

参考答案：

需要同时验证两个性质：

1. State Preservation：Domain A 恢复后能够获取自己的寄存器状态。
2. State Non-Disclosure：Domain B 不能读取 Domain A 的私有架构状态。

测试应覆盖：

- 不同 Domain 使用不同数据模式。
- 第一次切入 Domain。
- FP/Vector Disabled 与 Enabled。
- Lazy 和 Eager Context Switch。
- Interrupt/Exception 打断。
- 多 Hart 执行。
- FP/Vector 混合使用。

仅仅检查 GPR 和 Vector Data Registers 不够，还必须覆盖适用的控制及状态 CSR。

另外，Architectural Context Isolation 不能自动证明 Cache 等 Microarchitectural Side Channel 已经被消除。

## Question 7 — 两个 hart 同时 RPC，OP-TEE 出现 Deadlock，如何定位？

参考答案：

先采集：

- Linux Worker 状态。
- OP-TEE Trusted Thread 状态。
- RPC Request ID。
- 当前 Lock Ownership。
- `tee-supplicant` 请求处理情况。
- OpenSBI Domain Context 状态。

然后判断 Deadlock 发生在哪一层。

可能性包括：

- 两个 Trusted Thread 持有锁并等待对方。
- 一个 Thread Suspend 时保留了不应该跨 RPC 持有的锁。
- Linux RPC Worker 没有返回。
- Domain Return 路径遗漏状态更新。
- Shared Resource 的 Reference Count 错误。

对于真正的死锁，应构建 Wait-for Graph：

```
Thread A → Resource X
Resource X → Thread B
Thread B → Resource Y
Resource Y → Thread A
```

如果存在环状等待，就有必要调查 Lock Ordering。

但如果系统一直在执行某段重试逻辑，则更可能是 Livelock。

## Question 8 — 如何测试 OP-TEE 的 Shared Memory 是否存在 TOCTOU？

参考答案：

构造可由 Linux 并发修改的 Shared Memory。

让 OP-TEE 先读取参数并验证，再在使用之前由另一个 Linux Worker 修改关键字段。

观察是否能够使 OP-TEE 使用未经验证的数据。

需要关注：

- Length。
- Offset。
- Buffer Address。
- Object Handle。
- Nested Pointer。
- 生命周期标志。

正确修复通常需要建立可信的数据快照、明确所有权以及避免在验证后重新读取可变的不可信字段。

测试还应覆盖整数溢出和 Buffer 生命周期撤销等情况。

对于不稳定的 Race，可考虑在测试专用 Build 中使用明确的同步门控来控制 Check 与 Use 的执行顺序。

## Question 9 — 真实芯片上出现 DMA 数据不一致，为什么 QEMU 没发现？

参考答案：

原因可能是 QEMU 的内存模型和设备模型没有精确复现实际 SoC 的 Cache Coherency 行为。

需要调查：

- DMA 是否是 Cache Coherent。
- CPU Cache Clean/Invalidate 是否正确。
- DMA Buffer 的内存属性。
- CPU 与设备之间的 Ownership Transfer。
- Memory Barrier 与设备操作顺序。
- IOMMU/IOPMP 配置。
- DMA 是否可能访问 Secure Memory。

尤其要区分：

CPU 间的 Cache Coherency 不等于所有 DMA Master 自动具有相同的一致性保证。

不能通过增加一个普通 `FENCE` 就假定解决了所有 Cache Maintenance 问题。

## Question 10 — 如何设计可以发现 Context Leakage 的自动化测试？

参考答案：

将测试划分为：

- Integer Context。
- FP Context。
- Vector Context。
- CSR Context。
- Memory Context。
- Hardware Security Context。

为每类状态设计独立 Pattern。

在多个 Domain 之间执行大量切换，验证预期保存恢复行为及不可见性。

还应考虑：

- Domain 首次初始化。
- Context 重复使用。
- Nested Trap。
- Hart Stop/Start。
- Interrupt During Switch。
- CPU Extension Capability 不同的配置。

测试结果必须保留具体失败的 Hart、Domain、CSR/Register、预期值和实际值。

如果发现泄露，还需要判断属于 Architectural State Leakage，还是 Microarchitectural Side Channel。

## Question 11 — 为什么一个通过 100 万次的 Stress Test 仍不能证明系统不存在 Race？

参考答案：

Stress Test 属于经验性验证，能够提高发现缺陷的概率，但并不穷举所有可能的 Interleaving。

某个 Race 可能只在极窄的时间窗口发生。

同时，测试程序、编译选项、CPU Frequency、Cache Behavior 和中断时序都可能影响出现概率。

更有说服力的验证需要组合：

- Code Review。
- Memory Ordering Analysis。
- Lock Dependency Analysis。
- Directed Concurrency Test。
- Fault Injection。
- Long-running Stress Test。
- 必要时的形式化方法或模型检查。

此外，如果测试没有设置 Progress Assertion，某些 Worker 长期饥饿也可能被误认为系统正常。

因此应同时记录 Safety Property 和 Liveness Property。

## Question 12 — 你如何评审一个修改 OpenSBI Domain Context Switch 的 PR？

参考答案：

我会从六个方面进行 Review。

第一，Architecture Correctness。

检查所有必须保存和恢复的架构状态，确认其行为符合目标 ISA 与扩展规范。

第二，Security Correctness。

确认不存在跨 Domain State Leakage、权限残留和不安全的中间切换状态。

第三，Concurrency Correctness。

检查 Per-Hart Data、Global Domain Mapping、锁和 Memory Ordering。

第四，Compatibility。

确认修改不会破坏不支持相关 Extension 的 CPU，也不会错误依赖某一种平台特性。

第五，Failure Handling。

分析内存分配失败、Context 尚未初始化、非法目标 Domain 以及 Trap 等异常路径。

第六，Validation。

要求提交者提供最小复现、Regression Test 和受影响配置的验证结果。

对于这种 Patch，单纯的 Build Pass 不足以作为合并依据。

# 20.13 本章综合实验：建立自己的 RISC-V OP-TEE Validation Lab

最后，我建议将 Chapter 20 的所有知识沉淀为一个持续使用的本地验证环境。

下面是推荐的目录组织，不是 OP-TEE 官方仓库结构：

```
riscv-tee-validation/
│
├── configs/
│   ├── qemu-virt/
│   └── soc-platform/
│
├── scripts/
│   ├── build.sh
│   ├── boot.sh
│   ├── run-tests.sh
│   └── collect-logs.sh
│
├── tests/
│   ├── domain/
│   ├── pmp/
│   ├── mmu/
│   ├── context/
│   ├── interrupt/
│   ├── rpc/
│   └── security/
│
├── debug/
│   ├── gdb/
│   └── symbolize/
│
├── results/
│   ├── build-info/
│   ├── logs/
│   └── reports/
│
└── README.md
```

建议按以下四个 Milestone 实施：

| Milestone                  | 交付结果                                             |
| -------------------------- | ------------------------------------------------ |
| M1 — Debug Environment     | QEMU + OpenSBI + OP-TEE + GDB + Symbolization    |
| M2 — Functional Validation | xtest、SBIUnit、MMU/PMP/Context Tests              |
| M3 — Security & Stress     | SMP、RPC、Fault Injection、Context Leakage          |
| M4 — Automation            | CI、Build Matrix、Crash Report、Regression Tracking |

其中有三个实验优先级最高：

实验 A：OP-TEE Exception-to-Source Debug

人为触发一次 TA Page Fault，从 GDB 中获取 `scause`、`sepc`、`stval`，完成 Stack Unwind 和源码定位，并验证 TA 的异常退出行为。

实验 B：OpenSBI Domain Isolation Verification

在两个 Domain 之间切换，检查 `satp`、PMP、Trap Context 及权限矩阵；通过非授权访问验证实际隔离效果。

实验 C：Multi-Hart Context Stress

通过多 Worker 调用、Domain Switch、FP/Vector Pattern 和 Fault Injection，验证多 Hart 下的状态保存、隔离与系统进展。

# 20.14 本章总结：Staff Engineer 应建立怎样的验证思维？

整章最重要的不是某个 GDB 命令、某条 CSR 或某个 `xtest` 用例。

而是一套能够反复应用于真实工程的验证方法：

从实际故障到可维护的系统

1\. Observe

获取跨 Linux / OpenSBI / OP-TEE 的可靠状态

2\. Reproduce

将偶发问题转化为可控实验

3\. Isolate

缩小问题涉及的硬件与软件边界

4\. Understand

从 ISA、数据结构和执行路径证明根因

5\. Fix

修复根本原因，而不是掩盖现象

6\. Validate

验证正确性、安全性与并发行为

7\. Prevent

将测试加入持续集成，防止回归

对于 RISC-V OP-TEE，我尤其希望你牢固建立三个认识。

第一，功能正确性不等于安全正确性。

Linux 能够正常调用 TA，并不代表 Linux 无法读取 TEE Private Memory；Context Switch 返回成功，也不代表所有 FP/Vector State 都得到了正确隔离。

第二，测试通过不等于证明不存在问题。

一个成熟的系统需要结合 Architecture Analysis、Security Invariants、Directed Test、Stress Test 和 Fault Injection，而不是仅依赖测试数量。

第三，高价值的 upstream 贡献往往来自可验证的工程问题。

当你发现一个 RISC-V Context Switch、TLB Shootdown 或 Domain Isolation Bug，能够提交完整的 Root Cause Analysis、Minimal Reproducer、Fix 和 Regression Test，这比单纯增加平台初始化代码更能体现核心架构能力。

从长期技术发展来看，我最建议你将 Chapter 20 中的 Domain Context Validation、MMU/TLB Regression 和 Multi-Hart Security Testing 作为持续深入的三个重点。

它们既紧密连接 OpenSBI 与 OP-TEE，又能形成真正有长期价值的 upstream 测试与架构贡献。

Chapter 20 到这里完成。 本章从日志和异常现场出发，逐步覆盖 GDB、测试框架、硬件隔离、SMP、故障注入、安全验证、CI 和上游补丁工程，形成了一套完整的 RISC-V OP-TEE 工程验证方法。