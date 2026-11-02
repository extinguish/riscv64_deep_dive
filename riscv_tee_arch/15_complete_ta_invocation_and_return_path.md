# Chapter 15 — Complete TA Invocation & Return Path

RISC-V OP-TEE Architecture & Porting · Staff Engineer 深度课程

本章主题：从 `TEEC_InvokeCommand()` 到 Linux → OpenSBI → OP-TEE → User TA → RPC → Linux 的完整执行链

## 本章目标

前面的章节分别讨论了 OpenSBI Domain、Hardware Isolation、Trusted Thread、TA Context、Shared Memory 和 RPMI/SBI MPXY。

但如果仅仅掌握这些组件的功能，仍然无法真正理解一个 RISC-V TEE 系统如何工作。

本章要回答一个更有挑战性的问题：

> 假设 Linux 中一个用户进程调用 `TEEC_InvokeCommand()`，请求 OP-TEE 内部的 Trusted Application 执行一项加密操作。CPU 究竟执行了哪些代码？发生了哪些 Privilege Transition、Domain Switch、Thread Context Switch？如果 TA 中途需要 Linux 提供文件系统服务，如何返回 Linux，之后又如何恢复原来的 TA 执行？

我们会从 API 一直讲到源码、寄存器、状态机、内存生命周期、异常和工业调试。

完成本章后，你应该能够：

- 在 `optee_client`、Linux Kernel、OpenSBI 和 `optee_os` 中定位完整的调用路径。
- 解释每次跨安全边界的原因、参数和返回机制。
- 区分 Linux Task、OpenSBI Domain Context、OP-TEE Trusted Thread、User TA Context。
- 解释一次 RPC 如何在不丢失 TA 执行状态的前提下暂时返回 Linux。
- 设计并验证多 hart、超时、取消、异常退出、共享内存失效等情况下的处理机制。
- 对 RISC-V OP-TEE 的通信协议和 Context Switch 提出可审核的架构设计。

## 阅读前先明确：2026 年 11 月的代码成熟度

这一点非常重要，否则容易把不同设计阶段的代码拼成一条实际上并不存在的调用链。

当前至少需要区分以下三个层次：

| 实现                                              | 状态与作用                                |
| ----------------------------------------------- | ------------------------------------ |
| OP-TEE 通用层：Client API、Linux TEE Core、TA Manager | 已有成熟实现，可以直接进行源码级分析                   |
| RISC-V OP-TEE + OpenSBI Domain 的参考实现            | 已有代码，但使用过自定义的 TEE SBI 接口及特定监控固件约定    |
| 标准化 RPMI TEE + SBI MPXY Linux 通信                | 截至查阅的 2026 年 10 月资料，仍有 RFC 设计与协议集成工作 |

尤其需要注意两个不同的 Linux RFC 方案。

2026 年 9 月 10 日的方案，提出把现有 OP-TEE SMC ABI 的寄存器语义封装进 RPMI `TEE_CALL`，通过 SBI MPXY 传输，以便复用原来的调用和 RPC 机制。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

Openwall Lists



2026 年 10 月 5 日的另一套 v2 RFC，则提出独立的 RPMI Backend、固定宽度控制消息、Memory Parcel、Opaque Resume Token，并明确表示对应的 OP-TEE OS 端支持尚未完成。

[image](https://www.google.com/s2/favicons?domain=https://lkml.rescloud.iu.edu\&sz=32)

lkml.rescloud.iu.edu



因此，本章采用以下原则：

OP-TEE Core 部分以真实 upstream 源码为准；RISC-V RPMI 调用链以 2026 年 RFC 和参考实现为依据讲解架构，不把不同 RFC 的协议字段视为已经统一的标准 ABI。

这既能帮助我们理解当前系统如何工作，也能训练未来参与 upstream 架构 Review 所需的能力。

# 15.1 完整系统架构：先建立一张执行地图

## 15.1.1 六层软件架构

REE User Space — Linux U-mode

Client Application · libteec · tee-supplicant

ioctl / syscall

REE Kernel — Linux S-mode

TEE Core · OP-TEE Driver · RPMI Backend

SBI ECALL / MPXY

Trusted Firmware — OpenSBI M-mode

RPMI TEE Mediation · Domain Context · Isolation

Domain Switch

TEE Kernel — OP-TEE S-mode

Entry · Trusted Thread · TA Manager · Syscalls

SRET / ECALL

Trusted Application — OP-TEE U-mode

TA_InvokeCommandEntryPoint · TEE Internal API

注意：上图中 OpenSBI 的上下箭头表示同一 hart 上的一次执行控制转移，而不是 Linux 和 OP-TEE 分别拥有固定的两个 CPU。上下文如何保存、目标地址如何验证、隔离配置如何更新，都由具体的可信固件设计负责。

## 15.1.2 一次 TA Invoke 的时间线

假设 CA 已经调用过：

```
TEEC_InitializeContext(...);
TEEC_OpenSession(...);
```

也就是说，Context 和 Session 已经建立，现在仅执行一次 Invoke。

我们先把流程分为九个阶段：

```
T0  CA calls TEEC_InvokeCommand()
 |
T1  libteec prepares parameters
 |   -> ioctl(TEE_IOC_INVOKE)
 |
T2  Linux TEE Core / OP-TEE driver
 |   -> build optee_msg_arg
 |   -> call transport backend
 |
T3  RISC-V transport / OpenSBI
 |   -> SBI MPXY / RPMI
 |   -> enter OP-TEE Domain
 |
T4  OP-TEE S-mode
 |   -> acquire Trusted Thread
 |   -> tee_entry_std()
 |   -> entry_invoke_command()
 |
T5  TA Manager
 |   -> validate session
 |   -> enter User TA
 |
T6  User TA (U-mode)
 |   -> TA_InvokeCommandEntryPoint()
 |   -> TEE Internal API / syscall
 |
 |   [optional RPC]
 |      -> suspend Trusted Thread
 |      -> return to Linux
 |      -> RPC service / tee-supplicant
 |      -> re-enter OP-TEE
 |      -> resume execution
 |
T7  TA finishes
 |   -> OP-TEE returns to OpenSBI
 |
T8  Linux driver copies results
 |   -> ioctl returns
 |   -> libteec post-processes parameters
 v
T9  TEEC_InvokeCommand() returns
```

请特别留意：

RPC 不是一次新的普通 TA Invoke，也不是重新执行 TA 的入口函数。

RPC 是当前正在执行的 Trusted Thread 主动让出执行控制，在 REE 完成所需服务之后，再恢复这条已经挂起的执行链。

这一区别贯穿整章。

# 15.2 从 `TEEC_InvokeCommand()` 进入 Linux TEE Driver

## 15.2.1 为什么需要 libteec？

先看一个简单的 CA。

```
TEEC_Context ctx;
TEEC_Session session;
TEEC_Operation op = { 0 };
TEEC_Result res;
uint32_t origin = 0;

/* Context 和 Session 已经建立 */

op.paramTypes = TEEC_PARAM_TYPES(
    TEEC_VALUE_INOUT,
    TEEC_NONE,
    TEEC_NONE,
    TEEC_NONE
);

op.params[0].value.a = 100;
op.params[0].value.b = 200;

res = TEEC_InvokeCommand(
    &session,
    CMD_TEST_ADD,
    &op,
    &origin
);
```

这段代码看起来只是调用一个 C 函数，但它包含三套不同的 ABI：

| ABI                 | 两端                                | 主要职责                         |
| ------------------- | --------------------------------- | ---------------------------- |
| GP Client API       | CA ↔ libteec                      | 提供跨平台的 TEE 编程接口              |
| Linux TEE ioctl ABI | libteec ↔ Linux Kernel            | 传递 Session、Command、Parameter |
| OP-TEE Message ABI  | Linux OP-TEE Driver ↔ OP-TEE Core | 表达 Core 命令及其参数               |

在 RISC-V 中，还需要第四层：Linux ↔ M-mode Firmware 的 SBI/RPMI Transport。

这些 ABI 不能混为一谈。

例如 `TEEC_Operation` 不是直接交给 OpenSBI 的数据结构。它首先在 libteec 中转换为 Linux TEE ioctl 参数，之后由 OP-TEE Driver 编码成相应的 OP-TEE Message 或目标 Transport ABI。

## 15.2.2 深入 `TEEC_InvokeCommand()` 源码

真实实现位于：

[optee_client/libteec/src/tee_client_api.c](https://github.com/OP-TEE/optee_client/blob/master/libteec/src/tee_client_api.c?utm_source=chatgpt.com)

主要流程可以概括为下面这段简化伪代码：

```
TEEC_Result TEEC_InvokeCommand(
    TEEC_Session *session,
    uint32_t cmd_id,
    TEEC_Operation *operation,
    uint32_t *error_origin)
{
    struct tee_ioctl_invoke_arg *arg;
    struct tee_ioctl_buf_data buf_data;

    /*
     * 1. Validate input.
     * 2. Prepare ioctl argument buffer.
     */

    arg->session = session->imp.session_id;
    arg->func = cmd_id;
    arg->num_params = TEEC_CONFIG_PAYLOAD_REF_COUNT;

    /*
     * 3. Convert TEEC_Operation to ioctl params.
     */
    teec_pre_process_operation(...);

    /*
     * 4. Enter Linux kernel.
     */
    rc = ioctl(session->imp.ctx->imp.fd,
               TEE_IOC_INVOKE, &buf_data);

    /*
     * 5. Read result and output parameters.
     */
    res = arg->ret;
    eorig = arg->ret_origin;

    teec_post_process_operation(...);

    /*
     * 6. Release temporary resources.
     */
    teec_free_temp_refs(...);

    return res;
}
```

这是为了说明调用流程而简化的代码，完整实现还包含错误路径、临时 Shared Memory、Cancellation 等处理。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



有几个特别值得深入的设计细节。

### A. 为什么传递的是 Session ID，而不是 TA 指针？

`TEEC_Session` 是 CA 使用的 Client API 对象，包含 libteec 维护的信息。

但 Linux 和 OP-TEE 不应该直接相信用户态传递的内部指针。

因此需要一个由 TEE 子系统管理的 Session Handle。

Linux OP-TEE Driver 根据 Session ID 在当前 TEE Context 的 Session 集合中查找对应对象，OP-TEE 内部也会针对调用者的会话进行验证。

Session ID 的意义不仅是查找对象，更重要的是将资源和调用者绑定起来。

### B. `TEEC_Operation` 怎样传输？

以 `TEEC_VALUE_INOUT` 为例：

```
op.params[0].value.a = 100;
op.params[0].value.b = 200;
```

libteec 将它编码为：

```
tee_ioctl_param
    attr = VALUE_INOUT
    a    = 100
    b    = 200
```

Linux Driver 进一步将其转换为 Transport 所要求的参数格式。

对于 Memory Reference，流程更复杂：

```
CA Buffer
   |
   v
TEEC Memory Reference
   |
   v
tee_ioctl_param
   |
   v
Linux struct tee_shm
   |
   v
OP-TEE SHM Descriptor
or RPMI Memory Parcel
   |
   v
TEE mobj / Parameter Mapping
   |
   v
TA Parameter
```

要特别区分两个东西：

Command Argument Buffer 是传递控制信息的缓冲区；TA Data Buffer 是 TA 真正读取或写入的数据缓冲区。

两者可以使用相同的共享内存管理机制，但不一定是同一块物理内存。

### C. 是否一定发生 Memory Copy？

不一定。

- `TEEC_VALUE_*` 通常直接编码为参数值。
- 临时 Memref 可能需要建立 Shadow Buffer。
- 已注册的 Shared Memory 在满足驱动、MMU 和协议要求时可以避免数据复制。
- RISC-V RPMI Parcel 提供的是内存共享的描述、授权与生命周期管理，不意味着自动复制整个 Buffer。

这涉及两个不同的概念：

Zero-copy 是数据传输优化；Memory Sharing Authorization 是安全机制。

不能因为应用层认为自己使用了 Shared Memory，就默认 TEE 可以直接访问任意 REE 物理页。

# 15.3 Linux TEE Core 如何处理 `TEE_IOC_INVOKE`

## 15.3.1 从 ioctl 进入内核

RISC-V Linux 运行在 S-mode 时，用户态调用 `ioctl()` 会通过 U-mode ECALL 进入 Linux Kernel。

但这里有一个容易混淆的地方：

```
CA (U-mode)
    |
    | ECALL
    v
Linux Kernel (S-mode)
```

这是普通的 Linux System Call，不是 SBI Call。

通常：

- U-mode ECALL 产生 Environment Call from U-mode。
- 经异常委托后进入 Linux 的 S-mode Trap Handler。
- Linux System Call Dispatcher 根据系统调用号进入 `sys_ioctl` 等通用路径。
- 最终调用 `/dev/teeX` 的 ioctl Handler。

与此不同，稍后 Linux 向 OpenSBI 发起的 SBI ECALL，是从 S-mode 进入 M-mode 固件的请求。

虽然两者使用的都是 `ecall` 指令，但 Trap 来源、处理方和 ABI 完全不同。

## 15.3.2 Linux TEE Core 源码

真实代码位于：

[Linux drivers/tee/tee_core.c](https://github.com/torvalds/linux/blob/master/drivers/tee/tee_core.c?utm_source=chatgpt.com)

调用路径：

```
ioctl(fd, TEE_IOC_INVOKE, ...)
    |
    v
tee_ioctl()
    |
    v
tee_ioctl_invoke()
    |
    +-- copy_from_user()
    |
    +-- validate num_params / buffer length
    |
    +-- params_from_user()
    |
    +-- ctx->teedev->desc->ops->invoke_func()
    |
    +-- params_to_user()
    |
    +-- copy result to userspace
```

这是当前 `tee_core.c` 可以直接验证的实现路径。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



可以重点研究这行：

```
rc = ctx->teedev->desc->ops->invoke_func(
    ctx, &arg, params);
```

这一行体现了 Linux TEE Framework 的一个设计思想：

通用 TEE Core 不关心底层 TEE 是 Arm TrustZone、RISC-V Domain 还是其他安全执行环境。

它只需要执行 Driver 注册的 `invoke_func()`。

这样 Linux TEE Framework 的上层逻辑与底层安全架构解耦。

## 15.3.3 进入 OP-TEE Driver

OP-TEE Driver 的实际实现位于：

[Linux drivers/tee/optee/call.c](https://github.com/torvalds/linux/blob/master/drivers/tee/optee/call.c?utm_source=chatgpt.com)

对应的函数：

```
int optee_invoke_func(
    struct tee_context *ctx,
    struct tee_ioctl_invoke_arg *arg,
    struct tee_param *param);
```

核心逻辑：

```
optee_invoke_func()
    |
    +-- find_session()
    |
    +-- optee_get_msg_arg()
    |
    +-- msg_arg->cmd =
    |       OPTEE_MSG_CMD_INVOKE_COMMAND
    |
    +-- msg_arg->func = arg->func
    |
    +-- msg_arg->session = arg->session
    |
    +-- optee->ops->to_msg_param()
    |
    +-- optee->ops->do_call_with_arg()
    |
    +-- optee->ops->from_msg_param()
    |
    +-- return result and origin
```

这条路径可在当前 `optee_invoke_func()` 实现中直接找到。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有两个核心数据结构。

### `struct optee_msg_arg`

它属于 Linux 与 OP-TEE Core 之间的命令协议。

在传统 OP-TEE Message ABI 中，关键字段可以概括为：

```
struct optee_msg_arg {
    uint32_t cmd;
    uint32_t func;
    uint32_t session;
    uint32_t cancel_id;
    uint32_t pad;
    uint32_t ret;
    uint32_t ret_origin;
    uint32_t num_params;

    struct optee_msg_param params[];
};
```

不同 Transport 可以对共享内存的表示方式或控制消息格式作出改变。

请注意：

`cmd` 和 `func` 不是一回事。

例如：

```
msg_arg->cmd  = OPTEE_MSG_CMD_INVOKE_COMMAND;
msg_arg->func = CMD_TEST_ADD;
```

`cmd` 告诉 OP-TEE Core 需要执行哪一种系统级操作；`func` 是需要传递给 TA 的应用命令编号。

### `struct tee_shm`

它负责表达 Linux TEE Framework 中受管理的共享内存对象。

你应该追踪的不是简单的一个 PA，而是：

```
tee_shm
    |
    +-- User Mapping
    |
    +-- Kernel Mapping
    |
    +-- Physical Pages / SG
    |
    +-- Transport-Specific Handle
    |
    +-- Lifetime / Reference Count
```

最终 OP-TEE 所见的内存必须满足其自身的地址范围、权限和引用生命周期检查。

# 15.4 Linux → SBI MPXY → RPMI → OpenSBI Domain

这一节是 RISC-V 与 Arm OP-TEE 最大的区别之一。

## 15.4.1 为什么还需要 OpenSBI？

假设当前 hart 正在运行 Linux：

```
Hart 3
    Privilege = S-mode
    Domain    = Linux
    SATP      = Linux Page Table
    PMP       = Linux Domain Policy
```

Linux 希望执行 OP-TEE。

但它不能仅仅：

```
j optee_entry
```

因为问题不仅是控制流跳转。

当前 CPU 的安全执行环境还没有改变。

即使跳转到 OP-TEE 的某个入口地址，也无法自动获得：

- OP-TEE Domain 的物理内存访问权限。
- OP-TEE 的地址空间和 Trap 配置。
- 可信的 Domain Context。
- 对 Linux 内存和设备权限的重新限制。

因此，Domain Switch 必须由一个比 Linux 更可信的组件执行。

在当前方案中，该组件是 M-mode Trusted Firmware。

## 15.4.2 MPXY 与 RPMI 分别做什么？

这两个概念很容易混淆。

| 组件                    | 主要职责                                    |
| --------------------- | --------------------------------------- |
| SBI                   | S-mode 与 M-mode Firmware 的接口            |
| SBI MPXY              | 通过 SBI 代理消息传输                           |
| RPMI                  | 定义平台管理及 TEE 相关服务协议                      |
| RPMI TEE Service      | 描述 TEE Endpoint、Call、Memory Sharing 等操作 |
| OpenSBI TEE Framework | 根据请求执行必要的端点管理和安全执行控制                    |
| OP-TEE                | 处理真正的 TEE Command                       |

可以把它类比为：

MPXY 负责消息如何传输；RPMI 负责消息表达什么服务；可信固件负责实施相应操作。

但这是一种职责概括，真实系统中这些层次可以通过不同组件组合实现。

## 15.4.3 RISC-V 的 ABI 寄存器

RISC-V SBI ECALL 通常使用以下寄存器：

| 寄存器       | SBI ABI 中的含义 |
| --------- | ------------ |
| `a7`      | Extension ID |
| `a6`      | Function ID  |
| `a0`–`a5` | 输入参数         |
| `a0`      | 返回错误码        |
| `a1`      | 返回值          |

典型的调用形态：

```
/* S-mode caller */

mv a0, t0       /* argument 0 */
mv a1, t1       /* argument 1 */

li a6, FUNC_ID
li a7, EXTENSION_ID

ecall

/* a0 = SBI error */
/* a1 = SBI value */
```

这里的 `EXTENSION_ID`、`FUNC_ID` 是示意值，不代表特定 RPMI TEE Function 编号。

特别要注意：

标准 SBI 的返回约定，不等于传统 OP-TEE SMC ABI 的多个寄存器返回约定。

因此不能把 Arm 侧 OP-TEE 的 `a0`～`a3` 直接当作一次标准 SBI 调用的四个返回寄存器。

这恰恰是 2026 年 RISC-V OP-TEE Transport RFC 想要解决的兼容问题之一。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

Openwall Lists



## 15.4.4 一次 RPMI TEE_CALL 的逻辑流程

以 2026 年 9 月的兼容 SMC ABI 方案为例：

```
Linux OP-TEE Driver
    |
    | Build OP-TEE Message
    |
    v
RPMI TEE_CALL Request
    |
    | Register-like payload
    | Shared Memory reference
    v
Linux SBI MPXY
    |
    | ECALL
    v
OpenSBI M-mode
    |
    | Validate request / endpoint
    | Save REE Domain Context
    | Apply TEE isolation
    | Enter TEE Domain
    v
OP-TEE S-mode
```

请注意，这张图描述的是 RFC 的同步调用模型，不是断言 upstream 当前每个阶段都已完成集成。

另一套 2026 年 10 月 v2 RFC 采用独立控制 ABI，把共享内存表示为 Parcel ID、Nonce、Offset、Size，并使用 Opaque Token 恢复 Yielding Call。

这两种方式对本章的后续流程会产生实质影响：

| 设计            | 兼容原 SMC ABI 的方案  | 新 RPMI Backend 方案      |
| ------------- | ---------------- | ---------------------- |
| 控制消息          | SMC 风格寄存器参数      | 固定宽度控制消息               |
| Shared Memory | 复用现有 Message ABI | RPMI Parcel ID + Nonce |
| Resume        | 复用原有调用恢复语义       | 使用 Opaque Resume Token |
| Linux Driver  | 适配现有 SMC Backend | 独立 `rpmi_abi.c`        |
| 主要挑战          | 与标准 SBI/RPMI 的匹配 | 新协议及 OP-TEE OS 端实现     |

这也是为什么在做真正的 RISC-V Porting 时，一定要先固定 Linux、OpenSBI、OP-TEE 三方的协议版本。

## 15.4.5 Domain Switch 到底需要保存什么？

我们假设 Linux 和 OP-TEE 在同一个 hart 上时间复用。

切换前：

```
Hart 3: Linux Domain

  sstatus = Linux state
  sepc    = Linux return state
  stvec   = Linux trap vector
  satp    = Linux address space
  sp      = Linux kernel stack
  PMP     = Linux domain permissions
```

切换后：

```
Hart 3: OP-TEE Domain

  sstatus = TEE state
  sepc    = TEE execution state
  stvec   = OP-TEE trap vector
  satp    = OP-TEE address space
  sp      = OP-TEE stack
  PMP     = TEE domain permissions
```

但这只是状态分类，不是完整的上下文保存清单。

一个正确的 Domain Switch 实现还需要考虑：

- GPR，包括 `ra`、`sp`、`gp`、`tp`。
- S-mode CSR，包括 `sstatus`、`sie`、`sip` 等需要隔离的状态。
- FP/Vector Context 及其 Dirty/Clean 标记。
- 地址转换相关状态及同步。
- Interrupt Delegation 与各 Domain 的中断处理配置。
- Debug、Counter、Performance Monitoring 等敏感状态。
- Hardware Isolation 配置。
- Extended ISA 状态的跨 Domain 泄露风险。

并非所有 CSR 都能简单地按 Domain 保存、恢复。例如部分 CSR 是 M-mode 全局控制状态，必须由可信固件统一管理。

在具体实现中，哪些状态由 OpenSBI 保存、哪些由 OP-TEE 自己管理、哪些由硬件或 ABI 保证，需要明确约定，不能重复保存，也不能遗漏。

## 15.4.6 一次正确切换的安全不变量

从 Linux 进入 OP-TEE 至少需要保持：

\\[ \text{Linux cannot access protected TEE state} \\]

从 OP-TEE 返回 Linux 需要保持：

\\[ \text{Linux-visible CPU state contains no unintended TEE secrets} \\]

同时需要保证：

\\[ \text{Resume restores the intended logical execution} \\]

这意味着安全性不只依赖 PMP。

如果 PMP 正确，但 Vector Register 中残留了 TA 使用过的密钥相关中间状态，且切回 Linux 时暴露了该状态，TEE 依然可能遭受信息泄露。

同样，如果 FP/Vector 保存正确，但错误的 `satp`、TLB 状态或 Device Access Policy 在切换后仍然生效，也可能造成隔离失效。

Domain Switch 是一个系统级 Security Transition，而不是普通的函数调用。

# 15.5 OP-TEE 如何获得 Trusted Thread 并执行 Invoke

终于进入 OP-TEE Core。

这一阶段建议你重点研究 RISC-V 汇编入口与 OP-TEE 通用代码之间的交界。

## 15.5.1 RISC-V Entry Assembly

当前 RISC-V OP-TEE 的参考实现中，能够看到：

`core/arch/riscv/kernel/thread_optee_abi_rv.S`

其中存在以下入口：

```
thread_vector_table
    |
    +-- vector_std_abi_entry
    |
    +-- vector_fast_abi_entry
    |
    +-- vector_fiq_entry
```

标准调用路径进一步调用：

```
thread_handle_std_abi()
```

并在正常完成时通过 `thread_std_abi_entry` 相关路径返回调用方。

这部分有真实 upstream 汇编实现。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



不过，参考实现中的部分命名保留了原有 OPTEED 风格的 ABI 约定，不能据此推断新 RPMI Transport 必须采用完全相同的入口结构。

## 15.5.2 为什么要区分 Temporary Stack 和 Trusted Thread Stack？

这涉及 OP-TEE 特殊的线程模型。

假设某个 hart 收到一次 TEE Standard Call。

初始状态：

```
Hart 3
   |
   v
OP-TEE Entry
   |
   v
Per-Hart Temporary Stack
   |
   +-- Check call type
   +-- Find available Trusted Thread
   |
   v
Trusted Thread Stack
   |
   +-- Process command
   +-- Possibly block / RPC
   +-- Return results
```

Temporary Stack 用于入口阶段和执行控制切换。

Trusted Thread Stack 用于真正执行可能比较长、可能进行 RPC、可能发生阻塞的 TEE 操作。

为什么不能一直使用 Temporary Stack？

因为当 Trusted Thread 发生 RPC 时，它的执行状态必须保留，而当前 hart 可能需要返回 Linux，甚至之后用于执行其他操作。

如果所有操作共用一个 Per-Hart Temporary Stack，就无法安全地保存多个挂起的调用栈。

因此必须区分：

Per-Hart Entry State 与 Per-Invocation Trusted Thread State。

## 15.5.3 Trusted Thread 的状态机

概念上，可以简化为三个基本状态：

简化状态机；真实实现还涉及错误处理、调用参数和线程标志等状态。

重点不是状态名字，而是这些状态转换的含义：

- `FREE → ACTIVE`：分配一个 Trusted Thread，并切换到它的栈执行命令。
- `ACTIVE → SUSPENDED`：保存足够的执行状态，使之以后能从挂起点继续运行。
- `SUSPENDED → ACTIVE`：恢复可信执行状态，继续原来的逻辑调用。
- `ACTIVE → FREE`：调用真正完成，释放本次 Trusted Thread 资源。

注意，`SUSPENDED` 不意味着整个 TA Session 已经关闭。

甚至不同 Session 的生命周期，也不等同于 Trusted Thread 的生命周期。

## 15.5.4 真正进入 `tee_entry_std()`

在 OP-TEE 通用层中：

```
tee_entry_std()
    |
    v
__tee_entry_std()
    |
    +-- OPEN_SESSION
    |
    +-- CLOSE_SESSION
    |
    +-- INVOKE_COMMAND
    |
    +-- CANCEL
    |
    +-- Other protocol commands
```

真实函数位于：

`core/tee/entry_std.c`

在 `__tee_entry_std()` 中，会按照 `arg->cmd` 选择具体处理函数；对于 Invoke，调用 `entry_invoke_command()`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这就是架构相关汇编入口与通用 TEE Command Processing 的分界点之一。

# 15.6 从 OP-TEE Core 进入 User TA

## 15.6.1 `entry_invoke_command()` 如何处理？

真实源码逻辑可以概括为：

```
static void entry_invoke_command(
    struct optee_msg_arg *arg,
    uint32_t num_params)
{
    /*
     * Convert and validate parameters.
     */
    copy_in_params(...);

    /*
     * Resolve session.
     */
    s = tee_ta_get_session(
        arg->session,
        true,
        &tee_open_sessions);

    /*
     * Execute command.
     */
    res = tee_ta_invoke_command(
        &err_orig,
        s,
        NSAPP_IDENTITY,
        TEE_TIMEOUT_INFINITE,
        arg->func,
        &param);

    /*
     * Release reference and copy outputs.
     */
    tee_ta_put_session(s);
    copy_out_param(...);

    /*
     * Release shared-memory references.
     */
    cleanup_shm_refs(...);

    arg->ret = res;
    arg->ret_origin = err_orig;
}
```

这是对实际实现的删减，并非可直接替换 upstream 的完整函数。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这段代码体现了三个安全要求。

第一，必须验证外部参数。

虽然请求经过 OpenSBI 进入了 OP-TEE，但来源仍然是 REE，因此不能因为调用已经发生 Domain Switch，就相信参数是可信的。

第二，必须验证 Session。

Session 不只是一个数字，还关系到原始调用者、TA Context、权限及当前生命周期。

第三，必须保护 Shared Memory 的引用生命周期。

如果调用处理中途发生 RPC 或其他 Yield，底层内存不能在安全侧仍然使用时被错误释放或回收。

## 15.6.2 `tee_ta_invoke_command()`

真实源码位于：

`core/kernel/tee_ta_manager.c`

核心逻辑：

```
tee_ta_invoke_command()
    |
    +-- check_client()
    |
    +-- check_params()
    |
    +-- resolve TA context
    |
    +-- tee_ta_set_busy()
    |
    +-- set invocation timeout
    |
    +-- ts_ctx->ops->enter_invoke_cmd()
    |
    +-- check whether TA panicked
    |
    +-- tee_ta_clear_busy()
    |
    +-- report result and error origin
```

真实实现中可以看到：

```
res = ts_ctx->ops->enter_invoke_cmd(
    &sess->ts_sess,
    cmd);
```

这也是一个重要的架构抽象。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



OP-TEE 的 TA 可能是：

- User Mode TA。
- Pseudo TA。
- 其他通过 Trusted Service 抽象接入的实现。

因此 TA Manager 并不应该把所有操作直接写死为跳转到某个 U-mode 地址。

不同类型的 TA 可以提供不同的执行方法。

## 15.6.3 真正进入 U-mode TA

对于普通 User TA，最终要准备：

```
User TA Context
    |
    +-- Entry PC
    +-- User Stack Pointer
    +-- User Address Space
    +-- Parameter Mapping
    +-- User Register State
    +-- TLS / ABI State
```

然后 OP-TEE 从 S-mode 进入 U-mode。

在 RISC-V 上，典型特权转换涉及：

```
S-mode OP-TEE
    |
    | Configure sstatus.SPP = U
    | Set sepc = TA entry / resume PC
    | Prepare User Register State
    | Configure User Address Space
    |
    v
  sret
    |
    v
U-mode Trusted Application
```

这里必须区分两种切换。

Linux Domain → OP-TEE Domain 是不同安全 Domain 之间的切换，需要更高可信级别的固件参与。

OP-TEE S-mode → TA U-mode 是 OP-TEE 自己管理的特权级切换，通常不需要每次都调用 OpenSBI。

TA 使用的是 OP-TEE 为它建立的受保护地址空间，而不是直接运行在 Linux 用户地址空间中。

## 15.6.4 TA 执行与 TEE Syscall

最终 TA 进入：

```
TEE_Result TA_InvokeCommandEntryPoint(
    void *session_ctx,
    uint32_t cmd_id,
    uint32_t param_types,
    TEE_Param params[4])
{
    if (cmd_id != CMD_TEST_ADD)
        return TEE_ERROR_NOT_SUPPORTED;

    params[0].value.a += params[0].value.b;

    return TEE_SUCCESS;
}
```

假设操作只是整数加法，则 TA 可以直接完成计算并返回。

但如果 TA 调用了：

```
TEE_GenerateRandom(...);
```

或者：

```
TEE_OpenPersistentObject(...);
```

则可能经由 `libutee` 与 OP-TEE Syscall Layer 进入 S-mode Core。

其逻辑类似：

```
User TA (U-mode)
    |
    | TEE Internal API
    v
libutee
    |
    | ECALL
    v
OP-TEE Syscall Entry (S-mode)
    |
    +-- Validate User Pointers
    +-- Validate Session / Permissions
    +-- Call Core Service
    |
    v
Return to User TA
```

其中密码学操作可能完全在 OP-TEE Core 内完成，也可能调用硬件加密驱动；Secure Storage 则可能需要访问 REE 文件系统，从而触发 RPC。

不能把每一个 TEE Syscall 都等同于一次 RPC。

TEE Syscall 是 User TA 与 OP-TEE Core 的边界；RPC 是 OP-TEE 请求 REE 提供服务的边界。

# 15.7 RPC：如何离开 OP-TEE，然后继续原来的 TA？

这是整个 Chapter 15 最值得深入理解的部分。

假设 User TA 调用了：

```
TEE_OpenPersistentObject(...);
```

OP-TEE 使用 REE FS Secure Storage 后端，需要通过 `tee-supplicant` 完成文件访问。

## 15.7.1 为什么不能直接在 OP-TEE 中调用 Linux 文件系统？

因为 Linux 和 OP-TEE 是隔离的执行环境。

OP-TEE 不能直接：

```
open("/data/tee/xxx", O_RDONLY);
```

来使用 Linux VFS。

Linux 的内核对象、文件描述符表、VFS、Block Device Stack 都不属于 OP-TEE 的可信内核地址空间。

所以 OP-TEE 必须将文件操作请求交给 REE。

但这带来一个问题：

当前正在执行的 Trusted Thread，如何在请求 REE 服务时保存状态？

答案是通过 RPC + Trusted Thread Suspension。

## 15.7.2 RPC 的执行时间线

```
Linux CA
    |
    | Invoke
    v
OP-TEE Trusted Thread #2
    |
    | TA calls secure storage API
    v
OP-TEE Storage Service
    |
    | Needs REE FS
    v
RPC Preparation
    |
    | Save Thread Context
    | Thread #2 -> SUSPENDED
    |
    v
Return to REE
    |
    v
Linux OP-TEE Driver
    |
    | Recognizes RPC request
    v
tee-supplicant
    |
    | File operation
    v
Linux VFS / Filesystem
    |
    | RPC result
    v
Linux OP-TEE Driver
    |
    | Re-enter TEE / Resume
    v
OP-TEE Trusted Thread #2
    |
    | SUSPENDED -> ACTIVE
    | Restore saved context
    v
Continue Secure Storage Operation
    |
    v
Return to User TA
```

注意这里保留的是 原 Trusted Thread 的执行状态，而不是重新运行 `TA_InvokeCommandEntryPoint()`。

## 15.7.3 深入 RISC-V 汇编：`thread_rpc_xstatus()`

当前 RISC-V OP-TEE 参考代码中的：

`core/arch/riscv/kernel/thread_optee_abi_rv.S`

有一个值得逐行分析的实现：

```
/* Excerpts from the RISC-V RPC path */

jal thread_get_ctx_regs

/* Save thread register state */
store_xregs a0, THREAD_CTX_REG_RA, REG_RA, REG_TP
store_xregs a0, THREAD_CTX_REG_S0, REG_S0, REG_S1
store_xregs a0, THREAD_CTX_REG_S2, REG_S2, REG_S11

/* Get per-hart temporary stack */
jal thread_get_tmp_sp

/* Switch stack */
mv sp, a0

/* Suspend current trusted thread */
li a0, THREAD_FLAGS_COPY_ARGS_ON_RETURN
la a2, .thread_rpc_return
jal thread_state_suspend
```

这些关键指令来自实际 upstream RISC-V 汇编。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



我们按执行顺序分析。

### 第一阶段：保存 Context

```
jal thread_get_ctx_regs
```

取得当前 Trusted Thread 的寄存器保存区域。

接着：

```
store_xregs a0, THREAD_CTX_REG_RA, REG_RA, REG_TP
store_xregs a0, THREAD_CTX_REG_S0, REG_S0, REG_S1
store_xregs a0, THREAD_CTX_REG_S2, REG_S2, REG_S11
```

将需要跨挂起保存的寄存器写入 Thread Context。

为什么重点保存 `ra`、`sp`、`gp`、`tp` 和 `s0`～`s11`？

因为当前执行路径本身是按照 ABI 编写的 C/Assembly 调用链。Caller-saved 寄存器在相应函数调用边界上由 ABI 约束，不代表必须由这里一律保存全部 32 个 GPR。

这里保存的是恢复这个具体挂起点所需要的状态集合。

另外，FP/Vector 及扩展状态需要依照独立的 Context Management 逻辑处理，不能从这几行整数寄存器保存代码推断已经完成全部架构状态保存。

### 第二阶段：切换到 Temporary Stack

```
jal thread_get_tmp_sp
mv sp, a0
```

当前 Trusted Thread 的 Stack 必须保留下来。

接下来返回 Linux 时，不能让普通入口逻辑继续随意复用这条 Stack。

因此需要切换到 Per-Hart Temporary Stack。

这是整个机制的核心设计之一。

### 第三阶段：保存恢复位置

```
la a2, .thread_rpc_return
jal thread_state_suspend
```

`.thread_rpc_return` 是一处恢复执行标签。

它不是 TA 的初始入口。

也不是 OP-TEE 的启动地址。

它表示：

将来 RPC 完成、该 Trusted Thread 被恢复时，需要从哪条逻辑执行路径继续。

这与 RTOS 保存 Task Context 的设计非常相似。

但 OP-TEE 的 Trusted Thread Suspension 是为跨安全边界 Yield/Resume 服务的，并非单纯因为时间片用完而被调度。

### 第四阶段：返回 REE

参考汇编继续构造返回参数并调用：

```
jal thread_return_to_udomain
```

在参考配置 `CFG_RISCV_WITH_M_MODE_SM` 下，这个函数通过专用 SBI TEE Extension 触发 ECALL，并交由 M-mode Secure Monitor 处理。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



新 RPMI Transport 需要实现与其协议对应的返回机制，不能直接假定旧的 SBI TEE Extension 就是最终标准方案。

### 第五阶段：恢复执行

真实汇编中有：

```
.thread_rpc_return:
    /* Restore RPC result */
    /* Restore execution status */
    /* Restore local stack frame */
    ret
```

这段为简化的汇编示意；实际源码还包括返回参数保存、`xstatus` 恢复等处理。

最终 `ret` 返回到之前发起 RPC 的调用链。

所以，从调用该 RPC 的 C 函数视角看，它像是一个正常的函数调用终于返回了。

这就是 OP-TEE RPC 能够保存整条 C 调用栈，并在跨 Domain 执行后继续运行的根本原理。

## 15.7.4 RPC 期间 Trusted Thread 和 CPU 是什么关系？

假设：

```
Hart 3
    |
    +-- OP-TEE Trusted Thread #2
            |
            +-- RPC Suspend
```

此时：

- Trusted Thread #2 的 Context 必须保持。
- 对应的 TA Session 仍然存在。
- OP-TEE 中相关的引用计数和共享资源不能提前释放。
- Hart 3 可以切换回 Linux 执行。

在一些 Transport 模型中，恢复调用会与原始 calling hart 绑定；另一些模型可能使用能唯一识别挂起调用的 Resume Token，并由框架定义可恢复的执行位置。

是否允许跨 hart 恢复，不能根据存在 Thread ID 或 Token 就自行推断。

这取决于 OpenSBI Domain Context、OP-TEE Context、Transport ABI 对 Hart Affinity 和 Resume 的联合约定。

## 15.7.5 Linux 如何处理 RPC？

Linux OP-TEE Driver 收到 RPC 类型返回后，需要根据 RPC Command 选择适当的处理方式。

典型类别包括：

- Shared Memory 分配与释放。
- Wait / Wakeup。
- 通知及内部协调。
- 需要 `tee-supplicant` 的普通 RPC Command。

如果需要 `tee-supplicant`：

```
Kernel OP-TEE Driver
    |
    | Queue RPC request
    v
tee-supplicant
    |
    | TEE_IOC_SUPPL_RECV
    |
    | Process REE FS request
    |
    | TEE_IOC_SUPPL_SEND
    v
Kernel OP-TEE Driver
    |
    | Resume pending call
    v
OP-TEE
```

对于该路径，Linux 可以阻塞当前调用线程，等待 `tee-supplicant` 响应。

因此一个很重要的结论是：

RPC Yield 并不代表 Linux 用户态的 `TEEC_InvokeCommand()` 已经返回。

通常 CA 所在的用户线程仍然阻塞在 ioctl 调用中，直到整个 Invoke 最终完成。

## 15.7.6 一个很容易发生的 Deadlock

假设 OP-TEE 只有一个可用 Trusted Thread。

执行：

```
CA Invoke
  |
  v
Trusted Thread #0
  |
  | Secure Storage -> RPC
  v
SUSPENDED
```

现在这唯一的 Trusted Thread 已经挂起，等待 Linux 处理 RPC。

如果 Linux 的处理路径反过来要求发起一个新的 TEE Standard Call，并且该调用必须分配另一个 Trusted Thread，就可能出现：

```
Original Trusted Thread
    waits for REE RPC

REE RPC Handler
    waits for new Trusted Thread

No free Trusted Thread
```

这是一种循环等待。

因此 OP-TEE 的 RPC、Notification、Call Queue 和 System Thread 机制必须共同考虑资源占用与前进性。

OP-TEE 文档中也专门描述了多个 Trusted Thread 在等待 Mutex、通过 RPC 返回 Linux、再被唤醒的交互模型。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



这里的关键工程教训是：

线程池数量、RPC 等待方式、通知机制和调用重入策略必须作为一个整体设计。

不能简单地通过增大 `CFG_NUM_THREADS` 就认为所有 Deadlock 问题都能解决。

# 15.8 TA 完成后，如何层层返回 Linux？

现在假设加密操作已经完成，而且所有 RPC 都已经结束。

## 15.8.1 User TA 返回 OP-TEE Core

User TA 中：

```
return TEE_SUCCESS;
```

意味着 TA 的 Entry Function 返回。

通过 TA Runtime 和 Architecture Entry/Exit 机制，控制返回 OP-TEE Core。

这不是一次新的 Domain Switch，因为仍然处于 TEE Domain 中。

控制路径可以抽象为：

```
TA_InvokeCommandEntryPoint()
    |
    v
TA Runtime Return
    |
    v
OP-TEE Core (S-mode)
    |
    v
tee_ta_invoke_command()
```

OP-TEE 在这里需要：

- 更新 TA Session 的调用状态。
- 处理 TA Panic 或其他异常状态。
- 清理当前调用的临时资源。
- 更新输出参数。
- 恢复必要的内核执行上下文。

## 15.8.2 TA Manager 如何返回结果？

前面分析过：

```
res = ts_ctx->ops->enter_invoke_cmd(...);
```

之后，TA Manager 检查 TA 是否发生 Panic。

如果 TA 已经崩溃，可能向调用者报告：

```
TEE_ERROR_TARGET_DEAD
```

如果正常执行，则将 TA 返回码及 Error Origin 交还给上一层。

最终：

```
tee_ta_invoke_command()
    |
    v
entry_invoke_command()
    |
    +-- copy_out_param()
    |
    +-- cleanup_shm_refs()
    |
    +-- arg->ret = result
    |
    +-- arg->ret_origin = origin
    |
    v
tee_entry_std() returns
```

重要的是，返回 TEE Error Code 与解除底层 Transport 资源引用是两件不同的事情。

例如：

- TA 返回 `TEE_ERROR_BAD_PARAMETERS`。
- 整个 Domain Switch 和 RPC Transport 运行正常。
- OP-TEE 必须正确返回 TA 的错误码，而不是把它误判为通信错误。

## 15.8.3 Trusted Thread 正常退出

当前调用已经完成，Trusted Thread 不再需要保留原来的执行栈。

参考 RISC-V 代码中，`thread_std_abi_entry` 的正常返回路径包含：

```
/* Mask interrupts before stack transition */
csrw CSR_XIE, x0

/* Switch back to temporary stack */
jal thread_get_tmp_sp
mv sp, a0

/* Mark trusted thread free */
jal thread_state_free

/* Return to untrusted domain */
jal thread_return_to_udomain
```

这几段来自实际代码中的主要处理逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



注意它与 RPC 退出有一个根本区别。

| 正常完成                | RPC Yield               |
| ------------------- | ----------------------- |
| 不需要继续当前 Invoke      | 将来还需要继续当前 Invoke        |
| Trusted Thread 可以释放 | Trusted Thread 需要保留挂起状态 |
| 最终结果已确定             | 仅返回中间 RPC Request       |
| 返回 CA 的 ioctl 即将完成  | ioctl 通常仍未完成            |

这是调试时最容易判断错误的地方。

## 15.8.4 OpenSBI 返回 Linux Domain

在同步 Domain Switch 模型中，OP-TEE 返回后，可信固件需要恢复之前的 Linux Domain Context：

```
TEE S-mode
    |
    | Return / Yield protocol
    v
OpenSBI M-mode
    |
    | Preserve TEE context if suspended
    | Restore REE isolation configuration
    | Restore Linux S-mode context
    | Prepare result/status
    |
    v
mret
    |
    v
Linux S-mode
```

`mret` 是 M-mode 返回低特权级上下文的关键架构指令。

但是，这个简化过程隐含了两个不同层次：

一是 M-mode 自身在 Trap Entry/Exit 过程中对 `mepc`、`mstatus` 等寄存器的使用；二是 Firmware 对 Linux/TEE S-mode Context 的逻辑保存与恢复。

不能只恢复几个 GPR 就认为完成了 Domain Switch。

真实实现必须保证 Security Policy 与恢复后的 Execution Context 匹配。

## 15.8.5 返回 Linux OP-TEE Driver

Transport 返回后：

```
optee->ops->do_call_with_arg()
    |
    v
optee_invoke_func()
    |
    +-- from_msg_param()
    |
    +-- arg->ret = msg_arg->ret
    |
    +-- arg->ret_origin = msg_arg->ret_origin
    |
    +-- release argument resources
    |
    v
tee_ioctl_invoke()
    |
    +-- params_to_user()
    |
    +-- copy result to userspace
```

最终进入 libteec：

```
ioctl() returns
    |
    v
teec_post_process_operation()
    |
    +-- Update VALUE_OUTPUT
    +-- Update VALUE_INOUT
    +-- Copy output from shadow buffer if needed
    +-- Update MEMREF size
    |
    v
teec_free_temp_refs()
    |
    v
TEEC_InvokeCommand() returns
```

回到开头的例子：

```
op.params[0].value.a = 100;
op.params[0].value.b = 200;
```

如果 TA 执行的是：

```
params[0].value.a += params[0].value.b;
```

那么成功返回后：

```
op.params[0].value.a == 300;
```

一次完整的 CA → Linux → OpenSBI → OP-TEE → TA → OP-TEE → OpenSBI → Linux → CA 调用就结束了。

# 15.9 超时、取消、异常和失败恢复

前面的内容假设所有组件正常工作。

但在工业项目中，真正困难的通常不是成功调用路径，而是各种不正常的执行状态。

例如：

- CA 调用过程中收到 Linux Signal。
- TA 执行时发生 Page Fault。
- OP-TEE 正在等待 RPC，但 `tee-supplicant` 崩溃。
- Linux 回收了 OP-TEE 还在使用的 Shared Memory。
- 某个 hart 发生 Foreign Interrupt。
- 多个 CPU 同时进入 TEE，Trusted Thread Pool 耗尽。
- RPC Resume 时提供了错误的 Thread ID 或 Resume Token。
- OpenSBI 在 Domain Switch 中发现权限配置失败。

要处理这些问题，需要首先把异常分成四个层次。

## 15.9.1 四层错误模型

| 层次                | 典型错误                   | 应由谁处理                        |
| ----------------- | ---------------------- | ---------------------------- |
| Client API        | 参数无效、Session 未建立       | libteec                      |
| Linux / Transport | ioctl 失败、共享内存注册失败、协议错误 | Linux TEE Driver / Transport |
| OP-TEE Core       | Session 无效、资源不足、内部服务失败 | OP-TEE Core                  |
| TA                | 业务错误、未支持的命令、TA Panic   | TA / TA Manager              |

为什么这样区分很重要？

因为一次失败并不意味着所有层都发生了错误。

例如：

```
CA
  |
  v
Linux Driver  -- success
  |
  v
OpenSBI      -- success
  |
  v
OP-TEE Core  -- success
  |
  v
User TA      -- TEE_ERROR_ACCESS_DENIED
```

整个调用的通信机制完全正常。

失败的是业务授权检查。

如果 Linux Driver 将 TA 的返回结果一律转换成 `TEEC_ERROR_COMMUNICATION`，就丢失了真正的故障语义。

## 15.9.2 `TEEC_Result` 与 `error_origin`

GP Client API 使用两类信息描述结果。

第一类是具体错误：

```
TEEC_Result res;
```

第二类是错误来源：

```
uint32_t error_origin;
```

常见来源包括：

| Origin                    | 含义           |
| ------------------------- | ------------ |
| `TEEC_ORIGIN_API`         | Client API 层 |
| `TEEC_ORIGIN_COMMS`       | 通信层          |
| `TEEC_ORIGIN_TEE`         | TEE Core     |
| `TEEC_ORIGIN_TRUSTED_APP` | TA           |

需要注意：`error_origin` 不一定能精确定位到底是哪一行代码失败，它提供的是协议规定的错误来源分类。

例如 TA Panic 时，OP-TEE 可能返回 `TEE_ERROR_TARGET_DEAD`，并报告 TEE Origin，而不简单地归类为普通 TA 业务错误。

这在真实 `tee_ta_invoke_command()` 代码中能够看到。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 15.9.3 Cancellation：请求取消，不等于立即终止

假设一个 CA 发起了耗时的 TA 请求：

```
TEEC_InvokeCommand(
    &session, CMD_LONG_OPERATION,
    &operation, &origin);
```

另一线程希望取消：

```
TEEC_RequestCancellation(&operation);
```

libteec 的实际实现会通过 `TEE_IOC_CANCEL` 向内核提交取消请求。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



Linux OP-TEE Driver 则可以构造：

```
msg_arg->cmd = OPTEE_MSG_CMD_CANCEL;
msg_arg->session = session;
msg_arg->cancel_id = cancel_id;
```

随后走对应的调用路径。

但这里有一个重要区别：

取消请求是一个协作式语义，不能保证立即中断任何正在执行的 TA 指令。

需要区分：

1. Cancellation Request 已经提交。
2. OP-TEE 已经处理 Cancellation。
3. 目标 TA 已经观察到取消状态。
4. TA 终止当前操作并清理资源。

这四个事件不一定同时发生。

例如，一个 TA 正在执行不可中断的计算循环，可能不能及时响应取消。

因此系统设计不能假设：

```
RequestCancellation()
    ==
Immediately stop execution
```

对于生产环境，还需要定义取消不成功时的处理策略，包括 Watchdog、调用者退出、错误上报和资源回收。

## 15.9.4 TA Panic

假设 TA 发生非法内存访问：

```
volatile uint64_t *p = (void *)0xdeadbeef;
*p = 0x1234;
```

可能出现：

```
User TA (U-mode)
    |
    | Store Page Fault
    v
OP-TEE Trap Handler (S-mode)
    |
    | scause / sepc / stval
    |
    v
TA Abort Processing
    |
    | Mark TA as panicked
    | Release or invalidate TA context
    v
TA Manager
    |
    | TEE_ERROR_TARGET_DEAD
    v
Return to CA
```

具体是否终止整个 TA Instance、影响哪些 Session，取决于 TA 类型和生命周期配置。

但有一个安全要求：

一个 User TA 的异常不应该被轻易升级为整个 OP-TEE Core 的 Fatal Error。

这需要完整的 U-mode 地址空间隔离、Trap Handling 和资源清理机制。

与之相反，如果 OP-TEE Core 自身发生不可恢复的 S-mode 异常，就可能必须进入更严重的故障处理路径。

## 15.9.5 RPC 期间 `tee-supplicant` 崩溃

考虑：

```
Trusted Thread #1
    |
    | RPC: Open REE File
    v
Linux OP-TEE Driver
    |
    v
tee-supplicant
    |
    X Process crash
```

这是一种特别值得设计故障注入测试的情况。

必须回答：

- Driver 如何发现 Supplicant 不再可用？
- 当前 RPC 是否能够被唤醒并报告失败？
- 挂起的 Trusted Thread 是否一直占用资源？
- 后续是否能够重新启动 Supplicant？
- 是否可能导致其他 Trusted Thread 也被阻塞？
- 新的 Supplicant 是否可能错误地接收旧请求的响应？

不同实现的超时和重连策略可能不同，不能假设所有 RPC 都具有统一的自动超时。

尤其是当前 `entry_invoke_command()` 调用 `tee_ta_invoke_command()` 时，代码中的 `TEE_TIMEOUT_INFINITE` 表示该入口没有简单地为所有调用设置一个固定的执行截止时间。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



所以，应用层 Timeout、TA Cancellation、RPC Timeout、Domain Switch Timeout 必须分别分析。

## 15.9.6 Foreign Interrupt

假设 OP-TEE 正在执行：

```
Hart 3
    Domain = OP-TEE
    Mode   = S
```

这时一个属于 Linux 的外部中断到达。

系统必须决定：

- 中断是否能够直接送达当前 hart？
- 是否由 M-mode Firmware 处理或转发？
- OP-TEE 当前执行是否需要 Yield？
- 被中断的 Trusted Thread 是否要 Suspend？
- Linux 处理结束后如何恢复 OP-TEE？

在 Domain-based TEE 中，这涉及中断所有权和路由策略，不能仅通过 RISC-V 标准的 `sstatus.SIE` 一个开关来完整解释。

需要在设计上区分：

RPC Yield 是 TEE 主动请求 REE 服务。

Foreign Interrupt Yield 是由于执行环境必须处理属于另一个 Domain 的异步事件。

两者都可能需要保存 Trusted Thread Context，但触发原因和恢复协议不同。

## 15.9.7 多 hart 下的 Thread Exhaustion

假设：

```
16 harts
4 Trusted Threads
```

其中四个 Trusted Thread 都因 RPC 或锁等待而挂起。

第 5 个 Linux TEE 请求到达时，会怎样？

问题不能简单回答为“再创建一个 Trusted Thread”。

Trusted Thread Pool 可能是固定规模的。

Linux Driver 需要与 TEE 调用队列协作，处理暂时没有空闲 Trusted Thread 的情况，并在有进展后重试或唤醒等待者。

这里至少涉及两个独立维度：

| 资源                     | 是否 per-hart          | 作用               |
| ---------------------- | -------------------- | ---------------- |
| Hart Execution Context | 通常与 hart 相关          | 保存硬件执行状态         |
| OP-TEE Trusted Thread  | 通常来自全局 Thread Pool   | 表示可挂起的可信执行链      |
| TA Session             | 属于 TA 生命周期管理         | 表示 CA 与 TA 的逻辑会话 |
| Linux Task             | 由 Linux Scheduler 管理 | 发起和等待 TEE 操作     |
| RPMI Channel / Context | 由 Transport 规定       | 承载通信与返回状态        |

这几个对象不能混成一个统一的“线程”。

# 15.10 Staff Engineer 必须理解的五个核心架构问题

这是整章的架构总结，也可以用来判断自己是否真正理解了执行链。

## 问题一：为什么 Domain Context Switch 和 Trusted Thread Context Switch 不是一回事？

看下面的结构：

```
Hart 3
  |
  +-- Linux Domain Context
  |
  +-- OP-TEE Domain Context
          |
          +-- Trusted Thread #0
          |
          +-- Trusted Thread #1
          |
          +-- Trusted Thread #2
```

Domain Context 解决的是：

这个 hart 当前属于哪个安全执行环境？

Trusted Thread Context 解决的是：

OP-TEE 当前应该执行哪条可信调用链？

两个机制之间有联系，但不能一一对应。

一次 Domain Switch 可能恢复某个挂起的 Trusted Thread。

但一个 OP-TEE Domain 可以运行多个 Trusted Thread，也可以在没有可运行 Trusted Thread 时仅处理入口请求。

## 问题二：为什么 RPC 之后不必重新调用 TA 入口？

因为 Trusted Thread 保留了执行栈及必要寄存器状态。

假设：

```
TEE_Result process_secure_file(void)
{
    int fd;

    fd = secure_file_open();  /* RPC */

    return secure_file_read(fd);
}
```

当 `secure_file_open()` 触发 RPC 时，完整的可信执行链被保存。

恢复后继续执行原有调用，而不是重新运行：

```
TA_InvokeCommandEntryPoint()
```

如果重新执行入口，就会破坏原有局部变量、调用嵌套和操作状态。

从语言运行时的角度看，RPC 是一种 跨安全边界、保存 Continuation 的同步服务调用。

## 问题三：为什么同一个 hart 不一定意味着同一条 Linux Task 永远独占它？

同步调用在某个 hart 上发起，并不意味着整个 RPC 周期内都不会发生 Linux Scheduling。

例如 RPC 返回 Linux 后，当前任务可能因为等待 `tee-supplicant` 而睡眠。

之后 Linux Scheduler 可以运行其他任务。

当请求恢复时，Transport 必须遵守其 Hart Affinity 和 Resume Context 约定。

如果协议要求在特定 hart 恢复，就必须通过 CPU Pinning、Per-Hart Channel 或其他协调机制保证。

如果设计支持可迁移恢复，则必须证明所有与 hart 相关的状态都能够安全迁移或重新建立。

不能把普通 Linux Task 的 CPU Migration 当作 OP-TEE Resume 天然具备的能力。

## 问题四：为什么共享内存是最危险的安全边界之一？

因为 Shared Memory 同时被可信和非可信参与者使用。

假设 Linux 向 OP-TEE 提供：

```
Buffer Address = 0x90000000
Buffer Size    = 4096
```

TEE 读取头部，得到：

```
Payload Length = 128
```

然后 Linux 并发修改 Length 或 Payload。

如果 TEE 没有正确设计参数复制、验证和访问顺序，就可能出现 TOCTOU。

此外，Memory Parcel 还要避免：

- 非法页范围。
- 越界 Offset/Size。
- 共享内存在使用期间被错误 Reclaim。
- 不同请求重复使用过期 Handle。
- Cache Coherency 导致读到旧数据。
- DMA 访问绕过预期权限。
- 不一致的属性或别名 Mapping。

Shared Memory 的高性能并不天然意味着安全。

## 问题五：怎样证明一个完整的 TA Invoke 实现是正确的？

不能只运行一次 `xtest` 成功就下结论。

至少需要验证四种性质：

| 性质          | 要证明什么                                     |
| ----------- | ----------------------------------------- |
| Safety      | REE 无法访问不允许访问的 TEE 数据                     |
| Correctness | 命令参数、状态、返回结果保持正确                          |
| Liveness    | RPC、并发和资源等待不会永久阻塞系统                       |
| Recovery    | TA Panic、Driver 错误、Transport 失败后能够清理或隔离状态 |

进一步可以把这些性质写成执行不变量。

例如：

\\[ \text{RPC Resume} \Rightarrow \text{Valid Suspended Invocation} \\]

\\[ \text{Thread FREE} \Rightarrow \text{No Outstanding Execution Context} \\]

\\[ \text{Domain = REE} \Rightarrow \text{TEE Protected Memory Inaccessible} \\]

这些不变量非常适合作为 Code Review 和 Security Test 的依据。

# 15.11 Hands-on Lab：跟踪完整 TA Invoke 执行链

建议你在 RISC-V QEMU 环境中完成一个可复现的实验。

实验目标不是只看到 TA 打印日志，而是建立一条能够关联 CA、Linux Driver、OpenSBI 和 OP-TEE 的执行 Trace。

## 实验 A：建立最小化 TA

使用一个简单的 TA，提供两个 Command：

```
#define CMD_ADD_VALUE    0
#define CMD_READ_STORAGE 1
```

`CMD_ADD_VALUE` 只执行普通计算，用于验证无 RPC 调用路径。

`CMD_READ_STORAGE` 访问 REE FS 后端的 Secure Storage，用于测试可能发生的 RPC。实验前应确保该平台确实启用了相应后端，并且操作不是被其他存储实现完全处理。

这样可以得到两类 Trace。

无 RPC：

```
CA
 -> Linux TEE Driver
 -> OpenSBI
 -> OP-TEE
 -> TA
 -> OP-TEE
 -> OpenSBI
 -> Linux
 -> CA
```

含 RPC：

```
CA
 -> Linux TEE Driver
 -> OpenSBI
 -> OP-TEE
 -> TA
 -> OP-TEE RPC
 -> OpenSBI
 -> Linux
 -> tee-supplicant
 -> Linux
 -> OpenSBI
 -> OP-TEE Resume
 -> TA
 -> OP-TEE
 -> Linux
 -> CA
```

## 实验 B：源码断点

建议优先在这些位置设置断点或添加 Trace：

| 组件              | 断点 / 观察位置                     | 观察目的                |
| --------------- | ----------------------------- | ------------------- |
| libteec         | `TEEC_InvokeCommand`          | CA 参数和 Session      |
| Linux TEE Core  | `tee_ioctl_invoke`            | ioctl 参数转换          |
| Linux OP-TEE    | `optee_invoke_func`           | `optee_msg_arg` 构造  |
| Linux Transport | `do_call_with_arg` 的具体实现      | 跨安全边界调用             |
| OpenSBI         | TEE Call / Domain Switch 实现   | Domain Context      |
| OP-TEE          | `thread_handle_std_abi` 等具体入口 | Trusted Thread 分配   |
| OP-TEE Core     | `entry_invoke_command`        | Message Dispatch    |
| TA Manager      | `tee_ta_invoke_command`       | TA Session / Invoke |
| RPC             | `thread_rpc_xstatus` 等平台实现    | Suspend / Resume    |
| User TA         | `TA_InvokeCommandEntryPoint`  | 真正的 TA 执行           |

其中 RISC-V Transport 的断点应根据你所选的 OpenSBI/OP-TEE/RPMI 分支确定，不能把 RFC 中的函数当作稳定的 upstream 函数名。

## 实验 C：记录关键 CSR

在 Domain Switch 前后，以及 OP-TEE/TA U/S 切换的合适调试点记录：

```
info registers
p/x $pc
p/x $sp
p/x $satp
p/x $sstatus
p/x $sepc
p/x $stvec
p/x $sscratch
```

在 GDB 当前确实能够访问 M-mode 的调试上下文时，还可以观察：

```
p/x $mstatus
p/x $mepc
p/x $mtvec
```

需要根据 QEMU、GDB 版本和目标调试方式确认 CSR 是否可直接访问。

不要仅比较两个寄存器快照，还要回答它们发生变化的原因。

例如：

```
Before Domain Switch:
    SATP = Linux address space

After Domain Switch:
    SATP = OP-TEE address space

After User TA Entry:
    SATP = TA-related address space
    SPP  = U-mode return target
```

上述是预期的逻辑关系；真实实现可能使用共享的 Kernel Mapping、不同的 ASID 方案或其他地址空间组织方式，因此不能预设每个阶段的 `satp` 数值必然完全不同。

## 实验 D：RPC 故障注入

设计四类测试：

1. TA 触发 REE FS RPC，验证正常 Suspend/Resume。
2. 在 RPC 执行中停止 `tee-supplicant`，观察调用、等待和恢复行为。
3. 多个 CA 并发执行包含 RPC 的 TA 操作，观察 Trusted Thread Pool、Call Queue 和 CPU 使用情况。
4. 在测试固件中故意构造无效 Resume Handle，验证协议是否拒绝错误恢复。

最后一个测试只能在可控的实验环境中实施，并且应使用专门的 Fault Injection 代码，而不是直接修改生产环境的 Trusted Thread 内存。

## 实验 E：建立一份跨层 Trace

我建议采用统一 Trace Record：

```
struct tee_trace_record {
    uint64_t timestamp;
    uint64_t correlation_id;
    uint32_t hart_id;
    uint32_t domain_id;
    uint32_t thread_id;
    uint32_t event_id;
    uint64_t arg0;
    uint64_t arg1;
};
```

这是建议的实验结构，并非 upstream 定义。

其中 `correlation_id` 用于关联一次逻辑 Invoke 的各阶段。

不要直接把跨调用阶段的原始指针或密钥材料打印到日志中。

你需要设计一种能关联 Session、RPC、Resume Token 的诊断机制，同时保证敏感状态不会通过 Debug Log 泄露。

# 15.12 Staff Engineer 级别面试题与详细参考答案

以下题目重点考查架构理解、真实问题诊断及系统设计能力，而不是记忆函数名。

## Question 1 — Domain Switch 与 Trusted Thread 的关系

场景： 一个 16-hart RISC-V SoC，在 OpenSBI 中配置 Linux Domain 和 OP-TEE Domain。OP-TEE 支持 8 个 Trusted Thread。

如果 16 个 Linux Task 同时 Invoke TA，是否意味着每个 hart 都可以直接对应一个 Trusted Thread？

参考答案

不意味着。

Hart 是硬件执行资源，Trusted Thread 是 OP-TEE 管理的可挂起执行上下文。两者不是一一绑定的静态关系。

即使 16 个 hart 都能够进入 OP-TEE Domain，可同时执行的 Trusted Thread 数量仍受 Thread Pool 限制。

当所有 Thread 都处于 Active 或 Suspended 状态时，后续 Standard Call 可能需要等待或重试。

更深一层，还要验证 RPC Resume 是否要求返回相同 hart，以及 Domain Context 是否按 hart 维护。

因此，需要同时研究 OpenSBI Per-Hart Context、OP-TEE Thread Pool 和 Linux Call Queue，不能单独修改其中一个参数。

## Question 2 — 恢复 RPC 为什么必须验证 Resume Token？

场景： 新的 RPMI Backend 使用一个 Opaque Resume Token 标识挂起的 Invoke。

攻击者控制 Linux，并尝试重放之前捕获的 Token。

应该如何设计？

参考答案

Resume Token 应仅作为定位挂起状态的受控引用，不能仅凭其数值就授予恢复权限。

验证至少需要涉及：

- Token 对应的对象是否仍然有效。
- 对应的 Trusted Thread 是否确实处于可恢复状态。
- 是否属于正确的调用端点、会话和执行实例。
- 是否已经被消费、失效或取消。
- Shared Memory 的授权和引用生命周期是否仍然有效。
- 当前 Hart/Domain 是否满足协议的恢复约束。

需要防止 Token Reuse、Stale Handle 和 ABA 问题。

Token 是否必须具有不可预测性，要根据威胁模型决定；但即使是不可预测的 Token，也不能代替服务端的状态和权限验证。

一个合格的设计应明确 Token 创建、使用、失效及并发恢复的原子状态转换。

## Question 3 — 为什么在 SBI MPXY 中长时间持有 Spinlock 会出问题？

场景： Linux 使用标准 Mailbox `mbox_send_message()` 路径同步调用 TEE。TEE 请求耗时 20 ms。

开发人员发现所有 hart 上的 TEE 调用出现明显串行化。

原因是什么？

参考答案

如果 Mailbox Driver 在持有 Channel Spinlock 的过程中执行同步且耗时的 TEE Call，就可能长期持有 Spinlock，甚至在中断关闭的上下文中执行 OP-TEE。

后果包括 Lock Contention、其他 hart 进展受阻以及中断响应延迟。

正确设计应将短时间的 Transport State 管理和可能长时间运行的 TEE 调用分离，并保留 Per-Hart Channel Ownership 和共享缓冲区访问约束。

2026 年 9 月 RFC 专门提出了 `riscv_sbi_mpxy_mbox_call()` 来处理这类同步调用问题，这是非常好的真实工业设计案例。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

Openwall Lists



## Question 4 — Linux 调用 TA 时是否一定要固定 CPU？

场景： CA 在 CPU 3 发起 Invoke，RPC 期间该 Linux Task 睡眠。唤醒后 Linux Scheduler 将任务迁移到 CPU 7。

是否可以直接在 CPU 7 Resume？

参考答案

不能直接认为可以。

需要检查 Transport 是否与 calling hart 绑定。

如果挂起的执行依赖 CPU 3 上的 OpenSBI Per-Hart Domain Context，那么 CPU 7 不能仅仅依据同一个 Linux Task 或 Thread ID 恢复该状态。

可能的实现方案包括让调用路径在需要的阶段保持 CPU Affinity，或者设计显式的可迁移恢复机制。

后一种方案需要保证寄存器、Trap、内存转换、Interrupt、FP/Vector 及 Domain 状态能够在目标 hart 安全恢复。

结论必须基于具体 Transport ABI，而不是基于 Linux Scheduler 的一般行为。

## Question 5 — 为什么 OP-TEE RPC 能恢复任意深度的 C 调用栈？

场景： TA 调用了五层函数，最底层触发 Secure Storage RPC。

是否需要把五层函数的全部局部变量序列化？

参考答案

通常不需要。

因为 Trusted Thread 的 Kernel Stack 会保留，正常 C 调用栈已经包含尚未结束的函数栈帧。

只需要按照 ABI 和架构约定保存足以恢复当前执行点的寄存器、Stack Pointer、返回位置及相关状态。

在 RISC-V OP-TEE 参考实现中，`thread_rpc_xstatus()` 保存寄存器及 Thread Context，并使用 `.thread_rpc_return` 作为恢复标签。

但必须保证保存的 Stack 在挂起期间不会被覆盖或释放。

这里体现的是 Execution Continuation，而非函数参数的重新序列化。

## Question 6 — 共享内存注册成功，为什么仍然可能发生数据泄露？

场景： Linux 向 OP-TEE 注册一块 64 KB 的 Shared Memory。TA 只应读取其中 4 KB。

开发人员认为，既然 Shared Memory 已经注册，就可以安全地信任任何 Offset 和 Size。

参考答案

这是错误的。

注册成功只能说明某些共享内存管理条件得到满足，不等于其中所有参数都安全。

OP-TEE 仍然需要校验 Offset、Size、整数溢出、实际 Mapping 范围以及访问权限。

例如：

```
if (offset > total_size ||
    size > total_size - offset)
    return TEE_ERROR_BAD_PARAMETERS;
```

这里使用减法形式可避免简单的 `offset + size` 整数溢出问题。

还需要检查数据是否可能被 REE 并发修改，以及 Memory Parcel 是否仍然处于有效共享状态。

即使 TA 只能访问已授权的 64 KB，如果逻辑本应限制为 4 KB，越界访问其他 60 KB 仍然可能违反应用级权限边界。

## Question 7 — 一个 TA Panic 是否应该导致整个 OP-TEE 重启？

场景： 某个 User TA 在 U-mode 发生 Store Page Fault。

参考答案

正常情况下，不应该仅因一个 User TA 的非法访问就重启整个 OP-TEE Core。

OP-TEE S-mode Trap Handler 应根据异常来源、故障地址和上下文判断这是 User TA Fault 还是 Core Fault。

对 User TA Fault，可以终止对应 TA Context、处理关联 Session 并报告 `TEE_ERROR_TARGET_DEAD`。

对 OP-TEE Core 自身的异常，则可能需要进入更严重的 Fatal Error 处理。

这要求 User TA 和 OP-TEE Core 之间有可靠的 U/S Privilege 与 MMU 隔离。

不能仅依靠 M-mode PMP Domain 隔离来保护 OP-TEE Core 与内部 TA，因为它们通常位于同一 TEE Domain。

## Question 8 — 多个 Trusted Thread 同时 RPC，如何避免 Deadlock？

场景： OP-TEE 有四个 Trusted Thread，四个都进入 RPC 等待。Linux 处理其中一个 RPC 时，需要再次调用 OP-TEE。

参考答案

必须分析资源依赖图。

如果新的 TEE Call 需要第五个 Trusted Thread，而线程池已全部占用，就可能无法前进。

解决方案不能仅依靠增加线程数。

需要研究 OP-TEE Call Queue、System Thread Reservation、Notification 和 RPC Reentrancy 规则，并确保关键唤醒或资源释放路径不依赖已经耗尽的普通资源池。

对于某些互斥锁和等待场景，还需要保证等待中的 Thread 可以被安全唤醒。

最终应构造受限线程池的并发压力测试，验证系统满足 Liveness，而不是只验证单线程功能。

## Question 9 — 正常返回和 RPC 返回，OpenSBI 应如何区别对待？

场景： OP-TEE 返回 M-mode，Trusted Firmware 收到一个退出事件。

这个事件究竟代表 TA 完成还是仅仅发生 RPC？

参考答案

必须由明确的 Transport 协议状态或返回语义区分。

正常完成意味着调用结果已经确定，可以执行最终返回及资源清理。

RPC Yield 则表示 OP-TEE 仍然保留一条挂起的执行链，后续还需要处理 Resume。

如果 Trusted Firmware 把 RPC Yield 当作最终完成，就可能释放不该释放的 Context。

如果 Linux 把正常完成当作 RPC，则可能错误进入等待路径。

因此协议必须定义清晰的 Return Type、Handle、Context Lifecycle 和状态转换。

对新 RPMI Backend 来说，Opaque Resume Token 的有效性与生命周期是这里的核心设计问题。

## Question 10 — PMP 正确配置后，为什么还需要清理寄存器状态？

场景： RISC-V SoC 使用 PMP 保护 OP-TEE RAM。Domain Switch 过程确认 Linux 无法访问 TEE 内存。

开发人员因此认为不需要处理 FP/Vector Context。

参考答案

这是一个错误的安全假设。

PMP 保护物理内存访问，但不自动清理 CPU 寄存器中已有的信息。

如果 OP-TEE 使用过 FP/Vector Register，切回 Linux 后这些状态可能仍然存在于硬件寄存器中。

若软件没有正确隔离、保存或重置相关状态，恶意 REE 可能获得不属于自己的信息。

同样的问题还可能发生在 Debug Register、Performance Counter、扩展架构状态等资源中。

因此安全的 Domain Switch 必须建立跨 Domain CPU State Isolation，而不仅是 Memory Isolation。

## Question 11 — 如何定位偶发的 RPC Resume 失败？

场景： 16-hart 平台在压力测试中，每约十万次 TA Invoke 出现一次 `TEE_ERROR_COMMUNICATION`，而单核测试完全正常。

请给出系统级调试方案。

参考答案

首先建立能够跨层关联一次 Invoke 的 Trace，而不是只查看最终错误码。

重点记录：

- Invoke 的 Correlation ID。
- Linux 发起调用和恢复时的 CPU/Hart。
- Transport Channel 和请求状态。
- OpenSBI Domain Switch 的前后状态。
- OP-TEE Trusted Thread ID 和状态。
- RPC Request 与 Resume 的对应关系。
- Shared Memory / Parcel 的生命周期。
- 关键同步和错误返回位置。

然后按故障类型进行隔离：

- 如果仅跨 hart 出现，优先检查 Hart Affinity、Per-Hart Context 和 CPU Migration。
- 如果仅高并发出现，检查共享状态竞态、Token 复用和 Thread Pool。
- 如果仅 RPC 场景出现，检查 Suspend/Resume 及共享内存引用。
- 如果伴随随机数据错误，检查 Cache Coherency、内存屏障和 Buffer Lifetime。

实验上应分别运行 Single Hart、Multi-Hart、CPU Affinity 固定、无 RPC、有 RPC、不同 Thread Pool Size 的组合。

最后定位到明确的状态机或同步问题，编写可以稳定复现的 Regression Test，再提交修复。

## Question 12 — 你会选择兼容 SMC ABI，还是设计新的 RPMI ABI？

场景： 你是 RISC-V 服务器 SoC 的 TEE Architecture Owner，负责确定 Linux 与 OP-TEE 的长期通信 ABI。

目前两个 RFC 方案分别倾向于：

- 在 RPMI 中承载现有 OP-TEE SMC ABI。
- 构建独立 RPMI Backend 和 OP-TEE Control ABI。

你如何做架构决策？

参考答案

不应该仅凭代码修改量最小来决定。

应建立评估矩阵：

| 维度   | 评估重点                                  |
| ---- | ------------------------------------- |
| 规范兼容 | 是否符合稳定发布的 SBI/RPMI 规范                 |
| 兼容成本 | 是否能复用 Linux 和 OP-TEE 现有功能             |
| 安全模型 | Endpoint、Token、Memory Parcel 生命周期是否清晰 |
| 多核行为 | Per-Hart Channel、CPU Migration、并发语义   |
| RPC  | Yield/Resume 是否能够保持调用状态               |
| 内存共享 | 是否支持明确的授权、撤销和回收                       |
| 可维护性 | 是否与 Arm SMC/FF-A Backend 形成合理抽象       |
| 可测试性 | 是否能够自动验证异常和并发路径                       |

兼容 SMC ABI 的优点是可以较多地复用现有 Message ABI、RPC 和 Driver 实现。

独立 RPMI Backend 则可以更自然地利用 RPMI 服务发现、Memory Parcel、Transport Signal 和新的安全协议设计。

但它需要更多 Linux、OP-TEE OS、Firmware 三方协作，并增加 ABI 稳定性与迁移成本。

2026 年 10 月的 v2 RFC 已经明显转向通用 RPMI Transport + 独立 OP-TEE Backend 的结构，而对应的 OP-TEE OS 协议实现仍需完善。

[image](https://www.google.com/s2/favicons?domain=https://lkml.rescloud.iu.edu\&sz=32)

lkml.rescloud.iu.edu



作为 Staff Engineer，我会先以实际 workload 和威胁模型定义技术要求，明确 ABI 必须满足的 Safety、Liveness、Compatibility 和 Lifecycle 约束，再通过原型和跨组件 Review 推动决策。

更重要的是，一旦形成公开稳定 ABI，就不能随意破坏已有用户的兼容性。

# 15.13 本章总结：从 API 到硬件的统一执行模型

本章最重要的成果，是把以下四种不同的切换机制区分清楚。

| 机制                    | 典型转换                         | 主要责任                       |
| --------------------- | ---------------------------- | -------------------------- |
| Linux Syscall         | REE U-mode → Linux S-mode    | Linux Kernel               |
| TEE Domain Switch     | Linux S-mode ↔ OP-TEE S-mode | OpenSBI / Trusted Firmware |
| Trusted Thread Switch | Active ↔ Suspended / Free    | OP-TEE Kernel              |
| TA Privilege Switch   | OP-TEE S-mode ↔ TA U-mode    | OP-TEE Kernel              |

这四类切换共同组成一次 TA Invoke 的执行基础，但各自保存的状态、负责的权限边界及恢复条件并不相同。

对于整个调用过程，我们可以抽象出三条必须同时正确的链。

第一条：Control Flow

```
CA
 → Linux
 → OpenSBI
 → OP-TEE Core
 → TA
 → OP-TEE Core
 → OpenSBI
 → Linux
 → CA
```

第二条：Data Flow

```
TEEC_Operation
 → tee_ioctl_param
 → tee_param
 → optee_msg_arg / Transport Message
 → tee_ta_param
 → TA Parameters
```

第三条：Execution State

```
Linux Task
 ↔ Domain Context
 ↔ Trusted Thread
 ↔ TA Context
```

Control Flow 决定执行到哪里。

Data Flow 决定传递什么信息。

Execution State 决定一次操作在跨越多个执行环境之后，如何保持正确的逻辑连续性。

对于 Staff Engineer 来说，真正需要掌握的是这三条链在正常、并发、异常和安全攻击场景下如何共同满足正确性要求。

这也是为什么完整的 OP-TEE Porting 不能只验证：

```
Linux boots
OP-TEE boots
xtest passes
```

它还必须验证 Domain Isolation、跨 hart Context、一致性、Shared Memory、安全中断、RPC、故障恢复和 ABI 生命周期。

## 本章建议掌握程度自测

能够从真实源码追踪 TEEC_InvokeCommand() 到 tee_ta_invoke_command()

能够区分 Linux ioctl ABI、OP-TEE MSG ABI 与 RPMI Transport ABI

能够解释同一 hart 上 Linux/TEE Domain Switch 的安全状态管理

能够阅读 RISC-V thread_rpc_xstatus() 的保存与恢复逻辑

能够解释一次 Secure Storage RPC 的完整 Suspend/Resume 生命周期

能够区分正常完成、RPC Yield、Foreign Interrupt 与 TA Panic

能够设计多 hart 并发、错误 Resume 和 Shared Memory 生命周期的测试

能够分析现有 RISC-V RPMI RFC 方案的取舍并提出有依据的 Review 意见

掌握进度：0 / 8

重置

### 进一步阅读的核心源码

这五份资料最适合配合本章反复阅读：

- [optee_client — Client API / ioctl 参数准备](https://github.com/OP-TEE/optee_client/blob/master/libteec/src/tee_client_api.c?utm_source=chatgpt.com)
- [Linux OP-TEE Driver — Invoke / Session / Call Queue](https://github.com/torvalds/linux/blob/master/drivers/tee/optee/call.c?utm_source=chatgpt.com)
- [OP-TEE — Standard Command Dispatch](https://github.com/OP-TEE/optee_os/blob/master/core/tee/entry_std.c?utm_source=chatgpt.com)
- [OP-TEE — TA Session / Invoke Manager](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/tee_ta_manager.c?utm_source=chatgpt.com)
- [RISC-V OP-TEE — Entry / RPC / Context Assembly](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_optee_abi_rv.S?utm_source=chatgpt.com)

我建议本章最优先完成的实操是：从 `TEEC_InvokeCommand()` 开始，一直跟踪到 `tee_ta_invoke_command()`，然后针对一次 Secure Storage RPC，深入分析 `thread_rpc_xstatus()` 的汇编状态保存、返回和恢复。

这两个源码练习能够把本章的通用内核机制与 RISC-V 特有的底层执行机制真正连接起来，也能为后续 Chapter 16 的 Interrupt Routing 和 Chapter 17 的 SMP/Multi-Hart TEE Execution 建立必要的基础。
