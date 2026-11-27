# **OP-TEE OS Internals — Chapter 06**

## **完整系统生命周期与六条端到端执行链**

Chapter 06 · End-to-End Execution Flow, Context, Ownership & Security Invariants

本章以 **OP-TEE 4.11.0** 作为源码基线，以传统 Arm SMC + Linux TEE Driver + GP User TA 作为主要分析场景。FF-A、Secure Partition、RISC-V 和 Virtualization 的差异会在关键位置说明，不把某个平台的实现误认为通用架构要求。

## **6.0 本章目标：不再孤立地理解 OP-TEE 的子系统**

前五章主要回答了 OP-TEE 的架构、源码组织、Build System、ELF 和核心设计理念。

从 Chapter 06 开始，我们要建立另一种理解方式：

**不再以源文件为中心，而是以一次真实的系统行为为中心，研究这个行为如何穿越多个子系统。**

比如，一个看起来很简单的调用：

```
TEEC_InvokeCommand(session, CMD_ENCRYPT, ...);
```

内部可能涉及：

- Linux TEE Driver 和 Shared Memory。
- Secure World Entry、Trusted Thread 分配。
- TA Session Manager 和 Client Identity。
- TA Address Space 激活与 User Mode Execution。
- Syscall Dispatch、Crypto Framework。
- Secure DMA、Hardware Accelerator。
- Interrupt、Thread Suspension、RPC。
- 参数返回与资源释放。

单独理解每个模块，仍然不足以说明整个系统为什么正确。

真正的 Core Engineer 必须能回答：

> 在一次请求执行的每一个阶段，谁拥有 CPU 执行权？谁拥有当前资源？信任边界在哪里？出现异常后，谁负责恢复或清理？

### **6.0.1 六条需要完全掌握的执行链**

| 执行链                        | 核心问题                          | 对应章节 |
| -------------------------- | ----------------------------- | ---- |
| Boot → Core Init           | 一个二进制镜像如何成为可运行的 OS？           | 6.1  |
| OpenSession → TA Load      | 一个 UUID 如何变成运行中的 User TA？     | 6.2  |
| TA → Syscall → Crypto      | 一个用户态 API 如何安全访问内核服务？         | 6.3  |
| TA → Storage → RPC         | 为什么安全存储需要离开 Secure World？     | 6.4  |
| Interrupt → Suspend/Resume | 执行中的 Trusted Thread 如何被打断和恢复？ | 6.5  |
| CloseSession → Cleanup     | 如何确保资源最终释放，并且不会被提前释放？         | 6.6  |

我们还要用一个贯穿这六条路径的系统状态模型来理解它们。

### **6.0.2 OP-TEE 的五种核心状态**

这里的分类是为了分析方便，并不是 OP-TEE 源码中的五种正式对象类型。

OP-TEE Runtime State Model

**Global System State**

Boot Initialization · Kernel Global Objects · Security Configuration · Driver Registry

**Per-CPU State**

Current Thread · Entry Stack · Interrupt State · CPU-local Data

**Per-Thread State**

Saved Registers · Kernel Stack · Suspend/Resume Context

**Per-TA Context**

Address Space · Code · Data · TA State · Resource Lists

**Per-Session State**

Client Identity · Session ID · Session-local Data · Cancellation

请特别注意：

**CPU、Thread、TA Context 和 Session 是四种完全不同的资源抽象。**

它们之间不是简单的一对一关系。

例如：

- 一个 CPU 在不同时间可以运行不同的 Trusted Thread。
- 一个 Trusted Thread 在一次嵌套调用中可以进入不同 TA Context。
- 一个 TA Context 可以被多个 Session 引用，具体取决于 TA Flags。
- 一个 Session 可以跨越多次 `InvokeCommand()`，但每次调用不一定使用同一个 Trusted Thread。
- RPC 挂起的是执行上下文，不意味着对应的 TA Context 或 Session 被销毁。

这些关系是本章后面几乎所有并发、内存生命周期和安全问题的基础。

# **6.1 第一条执行链：Boot Firmware → OP-TEE Core → Ready**

## **6.1.1 为什么 Boot 不只是“跳转到入口函数”？**

从传统 Bare-metal 开发视角看，Boot 往往被理解成：

```
Reset
  ↓
初始化 Stack
  ↓
初始化 BSS
  ↓
main()
```

但对于 OP-TEE，这远远不够。

OP-TEE 在首次向 Normal World 提供服务之前，必须满足多个条件：

1. 安全内存边界已经可靠建立。
2. Core 代码、数据和页表具有正确的映射及权限。
3. CPU Exception、Thread Entry、Interrupt 基础设施已经可用。
4. Heap、Page Allocator、Crypto 和关键 Driver 已经完成适当初始化。
5. 后续来自 Normal World 的不可信参数可以被安全解析。
6. 与 Firmware 的入口、返回及共享内存接口已经建立。

因此，**Boot 的本质不是执行初始化函数，而是逐步建立 OS 所依赖的安全不变量。**

## **6.1.2 Boot 状态机**

Boot Phases

1. **Firmware Handoff**

   入口地址、CPU 权限、内存布局和启动参数满足约定
2. **Early Architecture Setup**

   Stack、Exception、MMU、运行时地址建立
3. **Core Runtime Setup**

   Per-CPU State、Thread、Heap、Page Allocator、Pager
4. **Service Initialization**

   Crypto、Drivers、Storage 基础设施和必要 Initcalls
5. **Normal World Handoff**

   完成启动并返回调用方或启动链的下一阶段
6. **Runtime Ready**

   可根据所启用的 Transport 处理后续服务请求

这里的各阶段是逻辑分类，不表示所有平台都严格以相同函数顺序执行。例如 Initcall、Pager 初始化和 Per-CPU 初始化的时机取决于配置，某些组件还可能延迟到首次使用时初始化。

## **6.1.3 从源码观察 Core 初始化**

主要源码入口：

- [`core/arch/arm/kernel/boot.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/boot.c?utm_source=chatgpt.com)
- [`core/arch/arm/kernel/thread.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread.c?utm_source=chatgpt.com)
- `core/include/initcall.h`
- `core/mm/`

以 Arm Backend 为例，`boot.c` 中的 `init_primary()` 涉及异常屏蔽、运行时设施、内存分配、Pager 和 Per-CPU 初始化，相关初始化代码还调用不同阶段的 Initcalls。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



一个概念上的简化路径是：

```
/* Teaching pseudocode: not copied from upstream */

void secure_boot(void)
{
    establish_early_execution_environment();
    establish_secure_memory_mapping();

    init_kernel_memory();
    init_thread_infrastructure();

    run_required_initcalls();
    init_platform_services();

    publish_runtime_entry();
    return_to_firmware();
}
```

虽然这段伪代码很简单，但其背后有一个重要依赖关系：

```
Memory Isolation
      ↓
Kernel Memory Management
      ↓
Thread / Execution Infrastructure
      ↓
Crypto & Driver Services
      ↓
TA Runtime Services
      ↓
External Request Processing
```

这不是实际的严格全序表；例如一些 Early Driver 必须在 Heap 初始化前运行。我们需要判断的是每个组件真正依赖哪些前置条件。

### **6.1.4 为什么初始化顺序本身就是安全问题？**

假设你在 OP-TEE 中增加一个 Hardware Crypto Driver。

它需要通过 MMIO 访问硬件寄存器：

```
static TEE_Result crypto_hw_init(void)
{
    /* MMIO mapping and hardware initialization */
    return TEE_SUCCESS;
}
```

如果它在 MMU Mapping 完成前执行，则可能访问一个尚未建立有效映射的地址。

如果它在安全内存隔离完成前配置 DMA，则可能让设备访问不应暴露的内存。

如果它在 Clock Driver 初始化前访问寄存器，则可能触发 Bus Fault。

反过来，如果 Secure Storage 初始化依赖 Crypto Hardware，而 Crypto Driver 初始化又调用 Storage 读取配置，就会形成循环依赖。

这说明：

**Initcall Order 不是单纯的代码运行顺序，而是系统依赖图的一部分。**

### **6.1.5 Boot 的最关键安全不变量**

| 不变量        | 需要保证的事实                          |
| ---------- | -------------------------------- |
| Isolation  | Normal World 无法越权修改 Core 的安全内存   |
| Execution  | 进入 C Runtime 前栈、地址和寄存器状态有效       |
| Memory     | Allocator 只管理已经正确声明的内存范围         |
| Interrupt  | 异常入口和执行栈在允许相应异常前已经可用             |
| Dependency | Driver 不会在其依赖尚未建立时执行             |
| Trust      | 不会未经验证就采用 Normal World 提供的安全关键配置 |

### **6.1.6 第一个值得深入思考的问题**

假设一个平台可以成功打印：

```
I/TC: OP-TEE version ...
```

并且 Linux 也启动成功。

这能否证明 OP-TEE 初始化完全正确？

不能。

这只能证明相关启动路径执行到了这些位置，并且没有触发阻止继续运行的错误。

例如：

- 某个 Crypto Backend 只在第一次调用时初始化。
- 某块 Secure DMA Memory 只在特定设备启用时分配。
- 一个 Secondary CPU 可能尚未执行完整 Per-CPU Initialization。
- Secure Storage 可以在第一次打开对象时才发现问题。
- TA VM 的某条特殊映射路径可能还未执行。

因此，Boot Success 与 Runtime Correctness 并不等价。

对一位 Core Engineer 而言，真正的验收必须结合后续功能路径、并发和异常测试。

# **6.2 第二条执行链：Client → OpenSession → TA Loader → TA Entry**

这是理解 OP-TEE 运行时的第一条核心路径。

## **6.2.1 从用户角度看，OpenSession 到底做了什么？**

假设一个 Linux CA 执行：

```
TEEC_InitializeContext(NULL, &ctx);

TEEC_OpenSession(&ctx, &session, &uuid,
                 TEEC_LOGIN_PUBLIC, NULL,
                 NULL, &origin);
```

从 CA 视角看：

```
UUID → Session Handle
```

但从 OP-TEE Core 视角看，它实际上是在请求：

> 找到一个可信服务；如果必要，创建它的执行实例；建立客户端与服务之间的会话关系；验证初始化参数；运行 TA 的 Session Entry；最后返回一个能够用于后续调用的 Session ID。

这里至少包含三个独立的生命周期：

- **Trusted Thread Lifetime：** 本次 Secure World 请求从开始到完成。
- **TA Context Lifetime：** TA 实例从创建到销毁，可能跨越多个 Session。
- **Session Lifetime：** 从 OpenSession 成功到 CloseSession。

这三者并不重合。

## **6.2.2 完整调用链**

以下以传统 SMC Transport、一个尚未加载的 REE FS User TA 为例。

```
Linux CA
 |
 | TEEC_OpenSession()
 v
libteec
 |
 | ioctl(TEE_IOC_OPEN_SESSION)
 v
Linux TEE / OP-TEE Driver
 |
 | Build optee_msg_arg
 | Prepare Shared Memory
 v
SMC → Secure Monitor → OP-TEE
 |
 | Allocate Trusted Thread
 v
tee_entry_std()
 |
 | OPTEE_MSG_CMD_OPEN_SESSION
 v
entry_open_session()
 |
 | Parse UUID / Client Identity
 | Convert Parameters
 v
tee_ta_open_session()
 |
 | tee_ta_init_session()
 |   |
 |   +-- Existing TA Context?
 |   +-- Secure Partition?
 |   +-- Pseudo TA?
 |   +-- User TA?
 |          |
 |          v
 |    Create user_ta_ctx
 |    Create TA VM
 |    Load ldelf
 |    Obtain TA Image
 |    Verify / Map ELF
 |    Resolve Relocations
 |          |
 |          v
 |    TA_CreateEntryPoint()
 |
 | TA_OpenSessionEntryPoint()
 |
 v
Return Session ID
 |
 v
Linux CA
```

这是依据真实调用点整理的逻辑图；具体 Store 选择、加载代码及 Secure Partition 分支取决于编译配置。官方文档确认，不同 User TA 存储来源最终共用 `ldelf` 加载机制。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+3



## **6.2.3 关键源码：Entry Layer**

源码：

[`core/tee/entry_std.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/tee/entry_std.c?utm_source=chatgpt.com)

这个文件是外部标准消息和 OP-TEE 内部服务之间的重要边界。

其中：

```
TEE_Result __tee_entry_std(
    struct optee_msg_arg *arg,
    uint32_t num_params);
```

会根据 `arg->cmd` 分发请求。

例如：

```
case OPTEE_MSG_CMD_OPEN_SESSION:
    entry_open_session(arg, num_params);
    break;
```

而 `entry_open_session()` 的重要处理步骤包括：

```
get_open_session_meta()
    ↓
copy_in_params()
    ↓
tee_ta_open_session()
    ↓
copy_out_param()
    ↓
cleanup_shm_refs()
```

这条路径很有研究价值。

### **为什么需要&#x20;**`copy_in_params()`**？**

因为外部请求参数并不是可信的 Kernel Data Structure。

它们通过 Normal World 和共享内存传递进来。

例如：

```
struct optee_msg_arg {
    /* Message header ... */
    struct optee_msg_param params[];
};
```

其中的字段可能包含：

- Value Parameter
- Temporary Memory Reference
- Registered Memory Reference
- Offset 和 Size
- Shared Memory Identifier

OP-TEE 必须把外部通信格式转换成内部可以管理的对象表示。

内部 `tee_ta_param` 使用 `param_val` 和 `param_mem`，其中内存参数包含 `struct mobj *`、`size`、`offs` 等信息。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这不是简单的结构体拷贝。

它是一次 **Representation Conversion + Resource Acquisition + Trust Boundary Validation**。

例如：

```
Untrusted SHM Descriptor
       |
       v
Validate Buffer / Offset / Size
       |
       v
Resolve or Acquire MOBJ Reference
       |
       v
Internal tee_ta_param
       |
       v
TA Session Manager
```

它改变了内存资源的表示形式和管理方式，但不意味着把 Normal World 内存自动变成了可信的 Secure Memory。

尤其需要注意：

**Valid Mapping 不等于 Trusted Content。**

一个共享内存区域可以拥有合法的映射和长度，但它仍然可能被 Normal World 修改。

如果 TA 或 Kernel 需要保证输入在整个操作中保持不变，则必须采用相应的 Copy、Snapshot、Ownership Transfer 或其他协议级机制。

## **6.2.4 关键源码：**`tee_ta_open_session()`

核心入口：

```
TEE_Result tee_ta_open_session(
    TEE_ErrorOrigin *err,
    uint32_t *sess_id,
    struct tee_ta_session_head *open_sessions,
    const TEE_UUID *uuid,
    const TEE_Identity *clnt_id,
    uint32_t cancel_req_to,
    struct tee_ta_param *param);
```

在 4.11.0 中，这个函数先调用：

```
res = tee_ta_init_session(
    err, open_sessions, uuid, &s);
```

随后进行参数检查、设置 Client Identity、记录参数及取消超时信息，再通过 `ts_ctx->ops->enter_open_session()` 进入对应的可信服务。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



注意这里的：

```
ts_ctx->ops->enter_open_session(...)
```

它体现了 OP-TEE 内部很重要的设计：

**Generic Session Manager 不需要把每一种 Trusted Service 的执行细节直接写死在自身代码里。**

不同的 Trusted Service Type 通过操作表抽象执行入口。

例如 User TA 的操作表中具有：

```
.enter_open_session = user_ta_enter_open_session,
.enter_invoke_cmd   = user_ta_enter_invoke_cmd,
.enter_close_session = user_ta_enter_close_session,
```

这些是 4.11.0 中可以直接确认的实际代码。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这类似于 Linux VFS 中的 `file_operations`：

```
Generic API
    |
    v
Common Object
    |
    v
Operations Table
    |
    +-- Backend A
    +-- Backend B
    +-- Backend C
```

但两者的语义并不相同：OP-TEE 的这里还涉及执行权限、TA 生命周期和安全上下文管理。

## **6.2.5 为什么&#x20;**`tee_ta_init_session()`**&#x20;很重要？**

这个函数负责将 Session 与具体 Trusted Service Context 建立关系。

在当前版本中，它包含对现有 TA Context、Secure Partition、Pseudo TA 和 User TA 的处理分支。实际匹配顺序由代码决定，而不应简单假定“所有 UUID 一律先去 Linux 文件系统查找”。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此：

```
OpenSession(UUID)
```

并不必然等于：

```
Load new TA ELF
```

可能出现的情况包括：

| 情景                              | 结果                     |
| ------------------------------- | ---------------------- |
| 已存在允许复用的 TA Context             | 复用已有 Context           |
| 第一次请求 User TA                   | 创建 Context，并进行加载       |
| UUID 对应 PTA                     | 使用内核中的 PTA 实现          |
| UUID 对应已支持的 Secure Partition 服务 | 走对应 Session Backend    |
| TA 已存在但不允许新的 Session            | 返回相应 Busy/Access Error |
| TA Image 不存在                    | 加载失败                   |
| TA Image 验证失败                   | 不能进入 TA                |
| 资源不足                            | 清理已创建的部分资源             |

## **6.2.6 TA Context 初始化的关键代码**

在：

[`core/kernel/user_ta.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/user_ta.c?utm_source=chatgpt.com)

4.11.0 的 `tee_ta_init_user_ta_session()` 会初始化一系列对象：

```
user_ta_ctx
    |
    +-- open_sessions
    +-- cryp_states
    +-- objects
    +-- storage_enums
    +-- busy_cv
    +-- TA Context
    +-- VM Info
```

然后把 Session 指向新的 Context。

随后，在 `tee_ta_complete_user_ta_session()` 中可以看到：

```
res = ldelf_load_ldelf(&utc->uctx);

if (!res)
    res = ldelf_init_with_ldelf(
        &s->ts_sess, &utc->uctx);
```

这些调用明确说明了 **Core Context 初始化与用户态 ELF 加载阶段是分开的**。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### **这里有一个非常值得研究的设计细节**

源码中，在真正执行 `ldelf` 加载前，不会继续持有 `tee_ta_mutex` 来覆盖整个加载过程。

原因是什么？

源码注释明确指出：如果在分配 Page Table 期间持有该 Mutex，可能导致 Deadlock。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这就产生了一个有趣的问题：

> 如果加载过程不能一直持有全局锁，那么其他 Trusted Thread 如何知道这个 TA Context 已经被创建，但还不能被正常使用？

答案在于初始化状态与等待机制的组合。

Context 会被标识为 `is_initializing`，并在适当阶段加入 Context List；其他尝试使用相同 Context 的调用需要处理其初始化状态。

这是一种典型的：

**Publish → Initialize → Mark Ready / Rollback**

形式的状态管理。

它必须同时保证：

- 不重复创建不该重复的 TA Context。
- 不允许其他 Thread 使用未完成初始化的 Context。
- 初始化失败时能够移除对象并清理资源。
- 等待初始化的 Thread 可以被正确唤醒。
- 不形成 Page Table、Allocator 和 TA Manager 之间的锁依赖循环。

这类问题才是 OP-TEE OS Internals 真正有难度的地方。

## **6.2.7 OpenSession 的资源所有权**

| 对象                   | 主要归属                    | 生命周期                 |
| -------------------- | ----------------------- | -------------------- |
| Linux `TEEC_Session` | CA / libteec            | Open 到 Close         |
| Message / SHM Buffer | 通信与 Linux Driver 层      | 由分配、注册和释放协议决定        |
| Trusted Thread       | OP-TEE Thread Framework | 一次请求；可能中途挂起          |
| `tee_ta_session`     | Session Manager         | Open 到 Close         |
| `user_ta_ctx`        | TA Runtime              | 按 TA Flags、引用和异常状态决定 |
| TA VM Regions        | User TA Context         | 随 Context 创建和释放      |
| `ldelf` Runtime      | TA Loader               | 依具体加载与动态链接运行模型       |
| User TA Entry State  | TA Runtime              | 随 TA/Session 各自的生命周期 |

特别要区分两个事实：

1. `OpenSession()` 返回后，这次请求使用的 Trusted Thread 可以释放。
2. 但成功建立的 Session 和仍被使用的 TA Context 必须保留。

## **6.2.8 本条路径的 Security Invariants**

- Client 不能通过伪造 Session ID 越权调用其他 Session。
- 新建 TA Context 不得在初始化完成前作为 Ready Context 使用。
- TA Image 在进入执行前必须满足对应来源的验证和加载约束。
- Shared Memory Descriptor 必须经过参数与资源检查。
- 加载失败不得留下可被访问的半初始化 TA Context。
- Session 引用 TA Context 时，必须保证其生命周期有效。
- 并发创建和关闭 TA 不能导致 Reference Count 错误。

**理解这一条路径后，你应该能够回答：为什么一次 OpenSession 失败，可能是 Linux Driver、Shared Memory、TA Store、Crypto Verification、ELF Loader、VM Mapping、TA 初始化逻辑或者 Session Manager 中任意一层的问题。**

# **6.3 第三条执行链：TA → Syscall → Kernel Crypto Service → Return**

这条路径主要用于理解 OP-TEE 的用户态/内核态边界。

## **6.3.1 一个普通的 Crypto API**

假设 User TA 运行：

```
TEE_Result res;

res = TEE_DigestDoFinal(
    operation,
    input, input_len,
    digest, &digest_len);
```

这是 GP Internal Core API 的一部分。

但 `TEE_DigestDoFinal()` 不应该直接让 User TA 访问内核 Crypto Object 指针，也不应该把 Hardware Crypto Driver 的 MMIO 映射给 User TA。

因为 User TA 与 OP-TEE Core 是不同的权限域。

于是系统需要一条受控路径：

```
User TA
 |
 | TEE_DigestDoFinal()
 v
libutee
 |
 | utee_* syscall wrapper
 v
Architecture Syscall Entry
 |
 | Trap / Exception
 v
OP-TEE Kernel Syscall Dispatcher
 |
 | Validate arguments & operation
 v
tee_svc_cryp.c
 |
 | Crypto Operation State
 v
Crypto Backend
 |
 | Software implementation
 | or Hardware accelerator
 v
Return to User TA
```

OP-TEE 的官方 Crypto 文档将这一实现关系划分为 GP API、`libutee`、Syscall、Kernel Crypto API 和 Crypto Library 等层。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **6.3.2 为什么需要这么多层？**

表面上看，这似乎比直接调用 Crypto Library 复杂得多。

但各层承担不同职责：

| 层次                    | 核心职责                 |
| --------------------- | -------------------- |
| `TEE_*` API           | 提供标准化服务语义            |
| `libutee`             | 用户态参数管理和 API 适配      |
| Syscall ABI           | 建立明确的用户态到内核态调用协议     |
| Syscall Dispatcher    | 根据调用编号进入正确 Handler   |
| Kernel Crypto Service | 权限、Handle、状态、资源及参数检查 |
| Crypto Framework      | 提供算法与 Backend 抽象     |
| Crypto Backend        | 实际执行算法或访问硬件          |

一个常见的理解误区是：

> User TA 运行在 Secure World，所以它操作的所有指针应该都可以被 Secure Kernel 直接信任。

这是错误的。

User TA 仍属于 OP-TEE Core 的潜在攻击面。

User TA 可以存在逻辑 Bug、内存破坏或者恶意代码。

因此，进入 Kernel 的用户态指针必须按照对应的访问语义检查，不能简单地当成内核指针解引用。

## **6.3.3 Crypto Handle 为什么不是 Kernel Pointer？**

例如：

```
TEE_OperationHandle operation;
```

TA 可以使用这个 Handle 引用一个 Crypto Operation。

但是它不应该能够通过伪造一个任意数值，就访问内核里的任意 `crypto_hash_ctx`。

为了保证安全，Kernel 需要进行对象查找、合法性检查和资源所有权验证。

概念上：

```
Untrusted / User-controlled Handle
            |
            v
Resolve in Current TA Context
            |
            v
Validate Object Type & State
            |
            v
Acquire / Use Kernel Crypto State
```

在 OP-TEE 实现中，Crypto Operation State 与 User TA Context 相关联，`user_ta_ctx` 内有 `cryp_states` 等资源链表，说明 Kernel 不把这些对象作为任意 TA 都能自由访问的全局裸对象。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



需要进一步理解一个概念：

**Handle 有两个不同层次的正确性。**

- *Memory Safety*：这个 Handle 是否指向合法的内核对象？
- *Authorization*：当前 TA 是否有权使用这个对象？

如果只检查第一项，那么跨 TA 的 Handle 访问仍然可能构成安全漏洞。

## **6.3.4 Syscall 中的当前执行身份**

Syscall 处理时，Kernel 必须能够找到当前正在执行的 TA Context。

实际代码中的重要机制包括：

```
ts_get_current_session()
```

以及当前 `ts_session` 对应的 `ts_ctx`。

例如在 Storage Syscall 中，上游明确通过当前 Session 获取对应 `user_ta_ctx`，随后在该 Context 的资源范围内查找对象。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



Crypto Syscall 采用类似的权限与资源管理思想。

这说明：

**Syscall 的授权身份来自受保护的当前执行上下文，而不是 User TA 自己递交一个声称可信的 UUID。**

如果允许 TA 在 Syscall 参数中随意指定“我是哪一个 TA”，那么攻击者便可以尝试伪装成其他 TA，访问其对象和密钥。

## **6.3.5 Syscall 并不意味着退出 Secure World**

这是非常重要的一点。

在 Arm 实现中，User TA 通过 `SVC` 进入 OP-TEE Kernel 的服务路径。

通常是：

```
Secure EL0
    |
    | SVC
    v
Secure EL1
    |
    | Kernel Crypto Service
    v
Secure EL0
```

这里发生的是 **Secure World 内部的权限切换**。

而 CA 调用 OP-TEE 则不同：

```
Normal World
    |
    | SMC / Transport
    v
Secure World
```

前者是 GP User TA → Core 的 Syscall；后者是 Normal World → TEE 的通信。

因此需要区分：

- User/Kernel Privilege Transition
- Secure/Normal World Transition
- TA Address Space Transition
- Trusted Thread Suspend/Resume

这四者可能相互关联，但不是同一个概念。

## **6.3.6 如果 Crypto Backend 使用硬件 DMA 呢？**

此时执行链可能扩展为：

```
Kernel Crypto Service
        |
        v
Secure Crypto Driver
        |
        +-- Validate / Prepare Buffer
        |
        +-- Configure DMA
        |
        +-- Start Hardware
        |
        +-- Wait for Completion
        |
        +-- Interrupt / Polling
        |
        +-- Verify Result
        |
        +-- Release DMA Resources
```

这时就需要考虑：

- User Buffer 是否必须复制到 Secure Buffer。
- DMA 是否能够绕过 CPU 的页表权限。
- Crypto Engine 是否被多个 Session 并发使用。
- Hardware Timeout 后能否安全复位。
- 如果 TA 在等待时被取消，DMA 是否仍在运行。
- Driver 是否持有会在等待过程中造成死锁的锁。
- 结果 Buffer 的有效长度如何验证。

这些问题已经超出单纯的 Crypto API 设计，而进入了 OS Resource Management 范畴。

## **6.3.7 本条路径的 Security Invariants**

- User TA 不能把任意 Kernel Pointer 当作合法 Handle 使用。
- Kernel 不得在未经适当验证的情况下访问用户态内存。
- Crypto Operation 必须属于有权限的 TA Context。
- 敏感密钥不得通过错误路径泄露到 Normal World。
- Hardware Backend 不得在操作尚未结束时释放 DMA Buffer。
- Syscall 返回后必须恢复正确的用户态执行状态。
- Crypto Failure 必须遵守 API 定义的状态和输出语义。

### **6.3.8 第三个值得思考的问题**

假设你发现一个 Crypto Hash 实现存在性能问题，决定通过以下方式优化：

> 在 `TEE_DigestDoFinal()` 处理中，直接使用 User TA 的输入 Buffer 作为 DMA Source，减少一次内存复制。

这种优化是否合理？

不能仅凭“User TA 也运行在 Secure World”来判断。

首先必须证明：

1. DMA Master 对该物理地址拥有合法且受限的访问权限。
2. 输入内存在整个 DMA 期间不可被错误释放或重新映射。
3. 设备能够正确处理物理非连续性和 Cache Coherency。
4. 数据不会因外部并发修改而破坏协议语义。
5. TA 在异常退出或取消时仍能保证安全回收。
6. 不会因为硬件地址转换或权限配置错误暴露其他 TA 的内存。

这也解释了为什么一个表面上的 Crypto Performance Patch，可能最终需要 Memory、DMA、Driver、Thread 和 TA Runtime 多个维护者一起 Review。

# **6.4 第四条执行链：TA → Secure Storage → RPC → REE Filesystem**

这条路径是 OP-TEE 系统设计中最有代表性的路径之一。

因为它揭示了一个看似矛盾的事实：

**一个安全操作系统可以利用不可信操作系统提供的服务，但这不意味着把安全决策权交给不可信系统。**

## **6.4.1 从一次 Persistent Object Write 开始**

假设 User TA 调用：

```
TEE_WriteObjectData(
    object,
    data,
    data_size);
```

如果这个对象采用 REE FS Storage Backend，实际的数据可能最终被写入 Linux 的普通文件系统。

但这里有一个严格的分工：

- OP-TEE 负责对象权限、数据加密、完整性及一致性机制。
- Normal World 负责提供普通文件读写能力。
- Linux 文件系统负责物理持久化，但不能因此被信任为安全判定主体。

上游文档明确确认，Trusted Storage 写入会经过 Kernel Storage Service 和 TEE 文件系统，安全数据经加密处理后，相关文件操作由 RPC 转交 `tee-supplicant`。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **6.4.2 完整调用链**

```
User TA
 |
 | TEE_WriteObjectData()
 v
libutee
 |
 | Storage Syscall
 v
OP-TEE Kernel
 |
 | syscall_storage_obj_write()
 |    |
 |    +-- Validate Object Handle
 |    +-- Check Access Rights
 |    +-- Check Offset / Length
 |    +-- Resolve File Operations
 |
 v
REE FS Backend
 |
 | tee_ree_fs.c
 |    |
 |    +-- Prepare Data / Metadata
 |    +-- Encrypt / Authenticate
 |    +-- Update Hash Tree
 |    +-- Prepare File Operations
 |
 v
tee_fs_rpc.c
 |
 | thread_rpc_cmd()
 v
OP-TEE Thread Framework
 |
 | Suspend Trusted Thread
 | Return RPC Request to Normal World
 v
Linux OP-TEE Driver
 |
 | Dispatch RPC
 v
tee-supplicant
 |
 | Read / Write / Sync / Rename ...
 v
Linux Filesystem
 |
 | Complete Operation
 v
tee-supplicant
 |
 v
Linux Driver
 |
 | Resume Secure Call
 v
OP-TEE Thread Framework
 |
 | Restore Suspended Thread
 v
Secure Storage Backend
 |
 | Validate Result
 | Complete / Abort Transaction
 v
Kernel Storage Service
 |
 v
User TA
```

注意：这是多次 RPC 可能组成的一次存储操作的逻辑链，实际并不是每次写入只发生一次 RPC。

此外，某些文件操作使用独立的提交或同步协议，不能把 Linux `write()` 成功直接等同于 GP Storage 操作已经提交成功。

## **6.4.3 为什么要有&#x20;**`tee_fs_rpc.c`**？**

相关源码：

- [`core/tee/tee_svc_storage.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/tee/tee_svc_storage.c?utm_source=chatgpt.com)
- [`core/tee/tee_ree_fs.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/tee/tee_ree_fs.c?utm_source=chatgpt.com)
- [`core/tee/tee_fs_rpc.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/tee/tee_fs_rpc.c?utm_source=chatgpt.com)
- [`core/arch/arm/kernel/thread_optee_smc.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread_optee_smc.c?utm_source=chatgpt.com)

这几个模块职责不同：

| 模块                   | 主要职责                               |
| -------------------- | ---------------------------------- |
| `tee_svc_storage.c`  | Persistent Object 的用户态 API 语义和权限管理 |
| `tee_ree_fs.c`       | REE FS Backend、文件布局与存储操作           |
| `fs_htree.c`         | Hash Tree、受保护数据块和元数据               |
| `tee_fs_rpc.c`       | 把文件系统操作转换成 RPC 请求                  |
| `thread_optee_smc.c` | 传统 SMC Transport 的 RPC 返回和恢复机制     |
| `tee-supplicant`     | 在 Normal World 执行对应服务              |

这里最值得关注的代码是 `tee_fs_rpc.c` 中的 `operation_commit()`：

```
static TEE_Result operation_commit(
    struct tee_fs_rpc_operation *op)
{
    return thread_rpc_cmd(
        op->id, op->num_params, op->params);
}
```

这是 4.11.0 中的实际实现。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



接着，在 SMC Backend 中：

```
uint32_t thread_rpc_cmd(
    uint32_t cmd,
    size_t num_params,
    struct thread_param *params);
```

它会准备 RPC Message，经过 `thread_rpc()` 将请求交给 Normal World，然后在恢复后读取 RPC 结果。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有两个独立的接口边界：

```
Storage → RPC Command
RPC Command → Secure World Exit
```

第一个边界负责业务协议，第二个边界负责执行上下文切换。

这种设计让 Storage Framework 不需要直接知道 Arm SMC 寄存器如何使用。

## **6.4.4 RPC 最重要的设计：执行被挂起，而不是被销毁**

假设 Trusted Thread 当前调用栈为：

```
TEE_WriteObjectData()
    |
    v
syscall_storage_obj_write()
    |
    v
ree_fs_write()
    |
    v
tee_fs_rpc_write()
    |
    v
thread_rpc_cmd()
```

此时 OP-TEE 需要 Normal World 执行文件写入。

它并不是销毁这条 C 调用栈。

实际发生的是：

```
Thread ACTIVE
     |
     | Save Execution Context
     v
Thread SUSPENDED
     |
     | Normal World handles RPC
     v
Thread RESUMED / ACTIVE
     |
     | Return from thread_rpc()
     v
Continue C execution
```

从调用 `thread_rpc_cmd()` 的函数视角看，这很像一个同步函数。

但底层已经发生了安全世界与普通世界之间的多次交互。

**这是 OP-TEE 的核心执行抽象：利用挂起和恢复，将跨世界远程服务包装成同步内核执行流程。**

## **6.4.5 RPC 返回结果为什么不能直接信任？**

假设 TA 请求读取某个安全存储对象。

Normal World 返回：

```
Read succeeded
Length = 4096
Data = ...
```

Kernel 能否直接把内容返回给 TA？

不能。

原因不是 Linux 文件系统“不可靠”这么简单，而是：

- RPC 返回值可以被恶意 Normal World 伪造。
- 返回长度可能与预期不一致。
- 文件内容可能已被修改。
- 文件元数据可能被替换。
- 数据可能来自历史版本。
- 文件读写操作可能只完成了一部分。

因此，Secure Storage 必须依赖其可信验证逻辑来决定结果是否可以被接受。

例如：

```
REE FS Read Result
       |
       v
Validate RPC Result / Length
       |
       v
Read Protected Metadata
       |
       v
Verify Authentication / Integrity
       |
       v
Decrypt Data
       |
       v
Return to TA
```

这里还要区分：

**Integrity Protection 不自动等于 Rollback Protection。**

即使每一份文件都拥有正确的 MAC，攻击者仍可能尝试用一份经过认证但已经过时的历史文件替换当前文件。是否能检测，需要看具体的 Counter、可信根及 Backend 设计。

## **6.4.6 4.11.0 中值得注意的变化**

如果你曾经研究过旧版 REE FS，尤其要注意存储格式和操作组织的演进。

4.11.0 的 Release Notes 明确指出，这个版本的 REE FS 内部格式存在不兼容变更，涉及把每个对象的数据组织为单个文件。GP API 兼容并不意味着磁盘上的数据格式天然兼容。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



本章不展开新旧格式细节，但这提供了一个很好的架构思考案例：

> 如果上层 `TEE_WriteObjectData()` API 没有变化，底层存储文件格式却变化了，那么测试范围应该如何扩大？

至少要考虑：

- 旧设备升级后的对象可用性。
- 新旧镜像切换。
- 掉电后的目录和对象状态。
- 正在打开的 File Handle 与缓存状态。
- 哈希树、文件布局和元数据一致性。
- Recovery 与 Rollback 保护。

因此，修改一个 File Backend 可能具有比 API 调用链更加深远的影响。

## **6.4.7 一个危险的优化方案**

假设工程师提出：

> Storage RPC 很慢，我们可以在发起 RPC 前释放所有 Storage Mutex，等待 RPC 完成后重新加锁。

这能否降低锁竞争？

可能，但不能直接这样修改。

以 `tee_ree_fs.c` 为例，上游使用 `ree_fs_mutex` 保护文件系统相关操作，其中部分路径涉及 Hash Tree、目录状态和 RPC。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



如果释放锁后，其他 Thread 修改同一对象，原先的本地状态假设可能失效。

必须检查：

1. RPC 前哪些对象处于修改中？
2. 是否已经发布新的 Metadata？
3. 其他 Thread 是否会观察到半提交状态？
4. File Handle 和 Hash Tree 的引用是否仍有效？
5. RPC 返回后能否安全继续旧事务？
6. 是否需要 Transaction ID、Generation 或 Version Check？
7. 正在执行的对象删除能否与 RPC 并发？

因此，优化锁持有时间之前，必须先证明事务状态机可以支持这种并发。

## **6.4.8 本条路径的 Security Invariants**

- RPC 返回值永远按非可信输入处理。
- Secure Storage 的权限检查不依赖 Normal World 作出安全结论。
- RPC Suspend/Resume 不得破坏原有 Kernel Call Stack。
- RPC Buffer 在整个操作期间必须有效。
- Storage Handle 和内部状态必须保持一致。
- 文件提交的原子性和数据完整性必须由可信机制维护。
- Normal World 阻塞不得演变成无法控制的锁依赖问题。

# **6.5 第五条执行链：Interrupt → Trusted Thread Suspend → Resume**

这是我认为本章最值得反复研究的路径之一。

因为在传统 RTOS 中，Interrupt 经常意味着：

```
Interrupt
  ↓
ISR
  ↓
Scheduler
  ↓
Next Thread
```

但 OP-TEE 的传统 Trusted Thread 模型不是这样的。

## **6.5.1 OP-TEE 为什么要区分 Native Interrupt 和 Foreign Interrupt？**

OP-TEE 把中断分为两类：

**Native Interrupt：** 由 OP-TEE 自身处理的中断。

**Foreign Interrupt：** 不应由 OP-TEE 自身直接完成处理，而需要转交给相应外部执行环境的中断。

例如，在传统 Arm TrustZone 场景下：

- Secure Device Interrupt 可能由 OP-TEE 处理。
- Non-Secure Timer/Network Interrupt 可能需要交给 Linux。
- 某些 EL3 Secure Interrupt 也不属于 OP-TEE 自己的处理范围。

上游文档明确区分了这两种中断，并指出 Foreign Interrupt 可能需要让正在执行的 Trusted Thread 暂停，以便 Normal World 处理中断。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **6.5.2 Native Interrupt 路径**

假设 OP-TEE 正在执行一个 TA Crypto 请求，Secure Crypto Engine 完成 DMA，产生 Native Interrupt。

逻辑过程为：

```
Trusted Thread ACTIVE
        |
        | Secure IRQ
        v
OP-TEE Exception Vector
        |
        v
Secure Interrupt Handler
        |
        +-- Read Device Status
        +-- Acknowledge Interrupt
        +-- Update Driver State
        |
        v
Exception Return
        |
        v
Continue Trusted Execution
```

这里通常不需要返回 Linux 处理中断。

但是必须根据 IRQ Handler 的上下文限制判断哪些操作可以直接执行。

例如：

- 可以更新合适的原子状态。
- 可以触发受支持的异步通知。
- 不能假定普通 Mutex 的阻塞等待在 IRQ Context 中可用。
- 不能假定任何可能睡眠或需要 RPC 的操作都可以在 IRQ Handler 里安全执行。

如果驱动需要执行复杂逻辑，可能必须使用 Deferred Processing。

## **6.5.3 Foreign Interrupt 路径**

假设一个 Trusted Thread 正在执行：

```
TA_InvokeCommand()
    |
    v
TEE_AEUpdate()
    |
    v
Crypto Kernel Service
```

就在此时，Linux 的一个非安全中断需要处理。

逻辑路径是：

```
OP-TEE Trusted Thread
        |
        | Foreign Interrupt
        v
Save Trusted Execution State
        |
        v
Mark Thread SUSPENDED
        |
        v
Switch to Entry Context
        |
        v
Secure Monitor
        |
        v
Normal World
        |
        | Handle Interrupt
        | Schedule Linux Tasks
        v
Resume Secure Call
        |
        v
Restore Trusted Thread
        |
        v
Continue Interrupted Execution
```

这里最重要的不是 Monitor 如何切换，而是：

**OP-TEE 必须在返回 Normal World 前，保存足够的执行状态，使得原有 Trusted Thread 能够继续执行。**

这种继续执行必须对 C 调用链保持正确语义。

## **6.5.4 源码中的 Thread State**

在：

[`core/arch/arm/kernel/thread.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread.c?utm_source=chatgpt.com)

能够看到：

```
thread_state_suspend(...)
```

以及：

```
thread_resume_from_rpc(...)
```

它们反映了 Thread Suspend/Resume 的核心状态转换。

例如，`thread_state_suspend()` 会记录寄存器状态，把 Thread 标记为 Suspended，并保存必要的用户态映射信息。`thread_resume_from_rpc()` 会在验证状态后恢复对应 Thread，必要时恢复其用户映射，再继续执行。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



可以把重要状态简化为：

Trusted Thread State Machine

简化状态机：完整的源码还需要考虑 Per-CPU 当前线程、线程标志、寄存器状态、用户映射和 Transport 的特殊处理。

### **为什么不存在 SUSPENDED → FREE 的普通直接路径？**

因为一个处于 Suspended 状态的 Trusted Thread 仍然可能保存着未完成的 C 调用栈、内核对象引用、用户态映射和资源锁状态。

直接把它改成 FREE，就相当于把一个尚未结束的执行上下文当成可复用资源。

这很可能造成：

- Stack Use-After-Free。
- 内核对象泄漏。
- Session 或 TA Context 引用丢失。
- 锁永远无法释放。
- 下一个请求继承不属于自己的敏感状态。

因此，即使 Normal World 已经不想继续等待，也不能简单通过覆盖 Thread State 来“取消”它。

## **6.5.5 Resume 时必须恢复什么？**

不仅是 PC 和 SP。

从设计角度，需要考虑：

| 状态                          | 为什么重要                           |
| --------------------------- | ------------------------------- |
| General Registers           | 恢复指令的执行语义                       |
| PC / SP / Status            | 回到正确的执行点和权限状态                   |
| Thread Stack                | 恢复 C 调用链                        |
| Current Thread ID           | 建立 Per-CPU 到 Trusted Thread 的关系 |
| User Address Space          | 避免访问错误 TA 的虚拟内存                 |
| Interrupt State             | 避免错误地屏蔽或开启中断                    |
| Floating-Point State        | 防止计算错误和跨上下文状态泄露                 |
| Session Context             | 保证仍然代表原始 TA 调用链执行               |
| Architecture-specific State | 满足平台的扩展状态和安全要求                  |

其中，4.11.0 的 `thread_resume_from_rpc()` 有一个非常值得关注的行为：

```
if (threads[n].have_user_map)
    core_mmu_set_user_map(&threads[n].user_map);
```

这说明 Resume 不只是恢复寄存器，还可能需要重新建立对应的 User Mapping。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



如果跳过这一步，Trusted Thread 可能使用错误的地址空间继续执行。

在 SMP 场景下，这可能引发非常复杂的跨核问题。

## **6.5.6 OP-TEE 如何与 Linux Scheduler 协作？**

传统 OP-TEE 并不需要创建一个完整的 Secure World General-Purpose Scheduler。

当 Trusted Thread 因 RPC 或 Foreign Interrupt 返回 Normal World 后，Linux Scheduler 可以调度其他任务。

当原来的调用者再次进入 OP-TEE，系统尝试恢复对应的 Trusted Thread。

所以可以这样理解：

```
Linux Scheduling
        |
        v
When to Resume TEE Request
        |
        v
OP-TEE Thread Context
```

不过要避免一个常见误解：

**Linux 调度器控制的是 Normal World 请求何时获得继续执行的机会，并不因此获得对 Secure Thread Context 的可信修改权限。**

具体 Thread ID、寄存器状态和恢复检查仍由 OP-TEE 自己管理。

这是 Scheduling Policy 与 Trusted Execution State Ownership 之间的边界。

## **6.5.7 为什么 Thread Exhaustion 是系统问题？**

假设：

```
CFG_NUM_THREADS = 4
```

现在四个 Trusted Thread 都因 RPC 等待 Normal World 服务而 Suspended。

第五个请求进入 OP-TEE 时，可能找不到可分配的空闲 Trusted Thread。

这个问题很有意思：

**CPU 可能没有运行任何 Trusted Thread，但 Thread Pool 已经耗尽。**

因为线程占用的不是单纯的 CPU 时间，还包括：

- Kernel Stack。
- Saved Execution Context。
- RPC 状态。
- 尚未完成的资源引用。

所以 CPU Idle 与 Secure Thread Available 是两种不同的资源状态。

这也是为什么在 OP-TEE 中，设计一个会长时间阻塞的安全服务必须特别小心。

## **6.5.8 本条路径的 Security Invariants**

- Suspended Thread 的 Context 必须保持有效。
- Resume 只能恢复合法且处于预期状态的 Thread。
- 不得把非可信 RPC 返回寄存器当成安全执行状态。
- User Mapping 必须对应正确的 TA Context。
- Secure/Non-Secure 执行切换不能造成敏感寄存器泄露。
- Interrupt Context 不得执行不允许的阻塞操作。
- Thread Pool 的资源状态必须保持一致。

# **6.6 第六条执行链：CloseSession → TA Context Teardown → Resource Cleanup**

如果说前五条路径主要研究“系统如何运行”，那么这条路径主要研究：

**当一次可信服务结束时，如何确保它拥有的所有资源都被正确处理，而且没有任何仍在执行的请求使用已经释放的资源。**

对于一个 OS，资源回收逻辑往往比资源创建逻辑更难。

原因是创建过程通常比较线性，而销毁过程可能与并发调用、异常退出和不完整初始化同时发生。

## **6.6.1 正常的 CloseSession 流程**

假设 Linux CA 执行：

```
TEEC_CloseSession(&session);
```

逻辑路径：

```
Linux CA
 |
 v
libteec / Linux TEE Driver
 |
 | OPTEE_MSG_CMD_CLOSE_SESSION
 v
OP-TEE Entry
 |
 | entry_close_session()
 v
tee_ta_close_session()
 |
 | Locate Session by ID
 | Validate Calling Client
 v
close_session()
 |
 | Ensure Safe Access to TA Context
 |
 +-- Run TA_CloseSessionEntryPoint()
 |
 +-- Unlink and Destroy Session
 |
 +-- Decrement TA Context Refcount
 |
 +-- Decide whether Context stays alive
 |
 +-- Destroy Context if required
 |
 v
Return to Linux
```

4.11.0 的 `entry_close_session()` 调用 `tee_ta_close_session()`，后者通过 Session ID 查找对象、检查 Client Identity，再进入内部的 `close_session()`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



## **6.6.2 Session 与 TA Context 为什么必须分别销毁？**

看下面这个例子：

```
TA Context A
    |
    +-- Session 1  (CA-1)
    |
    +-- Session 2  (CA-2)
    |
    +-- Session 3  (CA-3)
```

如果 TA 配置允许一个实例同时服务多个 Session，那么关闭 Session 1 时，不能直接销毁 TA Context A。

因为 Session 2 和 Session 3 仍然使用它。

所以至少需要维护两类不同的生命周期：

**Session Lifetime**

```
CREATE → OPEN → INVOKE... → CLOSE → DESTROY
```

**TA Context Lifetime**

```
ALLOCATE
    ↓
INITIALIZE
    ↓
READY
    ↓
SERVING SESSIONS
    ↓
RELEASE / RETAIN
    ↓
DESTROY
```

注意，TA Context 是否可以被保留，受到 `TA_FLAG_SINGLE_INSTANCE`、`TA_FLAG_INSTANCE_KEEP_ALIVE`、引用计数和 Panic State 等因素影响。

例如，在 4.11.0 中，`close_session()` 会减少 Context Reference Count，并结合 Keep-Alive 和 Panicked State 决定是否销毁 Context。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



因此：

**CloseSession 不等价于 Destroy TA Context。**

## **6.6.3&#x20;**`tee_ta_close_session()`**&#x20;的关键机制**

源码：

[`core/kernel/tee_ta_manager.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/tee_ta_manager.c?utm_source=chatgpt.com)

其内部会调用：

```
close_session(sess, open_sessions);
```

但不是只执行：

```
free(sess);
```

真正复杂的逻辑发生在销毁之前。

例如：

- Session 必须属于正确的 Open Session List。
- 调用者必须有关闭该 Session 的权限。
- 不能有不安全的并发使用仍引用它。
- 需要防止正在执行的 TA Entry 与 Close 发生冲突。
- TA Context 引用数必须正确减少。
- 最后一个 Session 关闭后，可能需要销毁 TA Context。

### **为什么需要 Session Unlink？**

假设两个 Trusted Thread 同时操作同一个 Session：

```
Thread A                        Thread B
   |                               |
   | InvokeCommand(Session X)      |
   |                               |
   |                        CloseSession(X)
   |                               |
   | Still Using Session X         |
   |                               |
```

如果 Thread B 直接释放 Session X，Thread A 就可能访问被释放的对象。

所以正确设计必须先阻止新的不安全访问，再处理已有引用，最后才允许对象释放。

4.11.0 的 `tee_ta_unlink_session()` 会设置 `unlink`，通知等待方，并等待 Session Reference Count 到达指定状态，然后从 Session List 中移除对象。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



概念上可以表示为：

```
Session OPEN
     |
     | Close requested
     v
Session UNLINKING
     |
     | Reject / Wake blocked users
     | Wait for outstanding references
     v
Session DETACHED
     |
     | No remaining unsafe references
     v
Session DESTROYED
```

这个过程的本质是：

**先撤销可发现性和新使用资格，再等待已有使用结束，最后回收内存。**

这是许多内核级对象销毁机制的共同原则。

## **6.6.4 为什么关闭 Session 可能触发死锁？**

考虑下面的场景：

```
TA-A owns Session S
      |
      v
TA-A invokes TA-B
      |
      v
TA-B calls back into TA-A
      |
      v
Nested Session / Busy State
```

此时如果所有 CloseSession 路径都强制等待 Busy TA 变成 Idle，就可能产生循环等待。

上游的 `close_session()` 已经包含专门的逻辑：如果由于 Single-Instance Lock 和 TA Busy 状态无法安全进入 TA 的 Close Entry，就可能跳过该 Entry，只清理 Session 状态，以避免死锁。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这很值得思考。

从 Application API 角度看，我们可能希望每个 OpenSession 都对应一次 Close Entry。

但是从 OS Correctness 角度看，不能为了强行执行回调，把整个系统锁死。

因此需要明确：

- 正常关闭时，应执行规定的 TA Cleanup Callback。
- TA 已崩溃或存在特定死锁风险时，回调可能不能安全执行。
- Kernel 必须有能力在不依赖不可信或不再可执行的 TA 代码时，清理自己管理的资源。

这也是为什么一个安全内核不能把所有资源回收都交给 User TA 自己完成。

## **6.6.5 User TA Context 销毁时，会释放什么？**

再看：

[`core/kernel/user_ta.c`](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/user_ta.c?utm_source=chatgpt.com)

其中：

```
static void release_utc_state(
    struct user_ta_ctx *utc);
```

包含以下重要清理动作：

```
Close nested sessions owned by TA
           |
           v
Finalize TA VM
           |
           v
Free Crypto Operation States
           |
           v
Close Crypto / Storage Objects
           |
           v
Close Storage Enumerators
```

实际代码中有：

```
vm_info_final(&utc->uctx);
tee_svc_cryp_free_states(utc);
tee_obj_close_all(utc);
tee_svc_storage_close_all_enum(utc);
```

这些调用可以直接在 4.11.0 源码中确认。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



由此可以看出，**一个 TA Context 不只是 CPU 地址空间。**

它还拥有多种内核管理的资源：

- Virtual Memory。
- Crypto Operation Context。
- Object Handles。
- Persistent Object 的已打开引用。
- Storage Enumerator。
- 嵌套打开的 Trusted Service Session。
- 其他 TA Runtime State。

这意味着，如果你给 OP-TEE 增加一种新的 Per-TA 内核资源，必须同时考虑：

1. 资源在哪里创建？
2. 归属哪个 TA Context？
3. 正常情况下如何显式关闭？
4. TA Panic 时如何回收？
5. CloseSession 与 Destroy Context 时是否都需要处理？
6. 资源释放是否会等待硬件或触发 RPC？
7. 是否可能与其他 TA 共享？
8. 释放顺序是否会形成死锁或访问已销毁资源？

## **6.6.6 一个常见误区：关闭 Object Handle 等于删除安全存储数据**

不是。

例如，一个 TA 通过：

```
TEE_CreatePersistentObject(...)
```

创建持久化对象，然后关闭 Session。

TA Context 被销毁后，内核可以关闭该 TA 打开的 Object Handle，但不能因此自动删除对应的持久化对象。

应该区分：

| 对象                        | 结束时的行为              |
| ------------------------- | ------------------- |
| Session Handle            | Close 后失效           |
| TA Context                | 根据引用和 Flags 决定销毁    |
| TA VM                     | Context 销毁时释放       |
| Crypto Operation          | 释放内核运行时状态           |
| Open Storage Handle       | 关闭运行时引用             |
| Persistent Storage Object | 按存储 API 语义持续存在      |
| Trusted Thread            | 请求完成时归还 Thread Pool |

这说明：

**Resource Cleanup 不等于 Data Deletion。**

对象的“内存生命周期”和“业务数据生命周期”必须分别设计。

## **6.6.7 Panic 与正常 Close 有什么区别？**

假设：

```
User TA
   |
   | Memory Access Fault
   v
TA Panic
```

Kernel 不应继续把已经崩溃的 TA 当成正常运行中的服务。

但是也不能直接丢弃所有管理状态。

它需要：

- 标记 TA 发生 Panic。
- 终止当前不安全的用户态执行。
- 向调用方传播适当的失败结果，例如 `TEE_ERROR_TARGET_DEAD`。
- 根据当前的 TA Flags 和生命周期规则处理 Context。
- 清理内核仍能安全管理的资源。
- 保证其他 TA 和 Core 本身不因该 TA Panic 而被破坏。

4.11.0 的 `tee_ta_open_session()` 和 `tee_ta_invoke_command()` 都有处理 Panicked TA Context 并返回 Target Dead 错误的逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



真正需要分析的是：

**如果 TA 在持有资源的情况下崩溃，Kernel 是否仍然可以完成可靠回收？**

例如：

- TA 持有一个 Storage Object Handle。
- TA 有正在执行的 Hardware Crypto Operation。
- TA 打开了另一 TA 的 Session。
- TA 在某次 RPC 返回后触发 Page Fault。
- TA 在内核资源操作中发生异常。

这些场景都可能暴露 Cleanup Path 的设计缺陷。

## **6.6.8 本条路径的 Security Invariants**

- 一个 Session 不能在仍被合法使用时释放。
- Session ID 不得绕过 Client Identity 和 Session Ownership 检查。
- TA Context 只能在满足其生命周期规则时销毁。
- Runtime Crypto/Storage Handle 必须被正确关闭。
- TA Panic 不能造成跨 TA 的资源泄露或权限破坏。
- Persistent Storage 的数据生命周期不能被 Session Cleanup 意外改变。
- Cleanup 必须具有正确的锁顺序，避免死锁。
- Partial Initialization Failure 也必须能够正确清理。

# **6.7 将六条执行链合并成一个完整的 OS 模型**

到这里，我们已经分别分析了六条路径。

现在需要进一步回答一个问题：

**它们究竟如何构成一个完整的操作系统运行机制？**

## **6.7.1 一次真实业务请求的生命周期**

假设系统启动后，Linux CA 请求一个 User TA 完成安全存储写入。

这个场景几乎可以把六条路径串联起来：

```
                    SYSTEM BOOT
                         |
                         v
                OP-TEE Core Ready
                         |
                         v
               Linux CA OpenSession
                         |
                         v
                 Trusted Thread
                         |
                         v
                  TA Manager
                         |
                         v
              TA Loader + TA VM
                         |
                         v
               TA OpenSession
                         |
                         v
               TA InvokeCommand
                         |
                         v
                Storage Syscall
                         |
                         v
                 Secure Storage
                         |
                         v
                  Storage RPC
                         |
             +-----------+-----------+
             |                       |
             v                       v
      Thread Suspend          Linux Scheduler
             |                       |
             +-----------+-----------+
                         |
                         v
                 Thread Resume
                         |
                         v
                Storage Commit
                         |
                         v
               Return to User TA
                         |
                         v
              Return to Linux CA
                         |
                         v
                CloseSession
                         |
                         v
              Session / TA Cleanup
```

这就是 OP-TEE 一套完整的运行闭环。

## **6.7.2 六条路径对应的资源生命周期**

| 资源                  | 创建阶段               | 使用阶段               | 释放阶段                    |
| ------------------- | ------------------ | ------------------ | ----------------------- |
| Kernel Global State | Boot               | 整个运行周期             | 通常随系统重启或关机终止            |
| Per-CPU State       | CPU 初始化            | 当前 CPU 运行          | 按 CPU 电源和平台生命周期         |
| Trusted Thread      | 请求进入               | TA、Syscall、RPC     | 请求完成                    |
| TA Context          | 首次加载或按实例策略         | Session、TA Runtime | 满足销毁条件时                 |
| TA Session          | OpenSession        | InvokeCommand      | CloseSession 或错误回收      |
| TA VM               | Context 初始化        | User TA Execution  | Context 销毁              |
| Crypto State        | TA Crypto 操作       | Hash/Cipher 等      | 显式 Free 或 Context 清理    |
| RPC Buffer          | RPC 准备             | 外部服务调用             | RPC/缓存管理规则决定            |
| Storage Handle      | Open/Create Object | Read/Write         | Close 或 Context Cleanup |
| Persistent Object   | Create             | 跨 Session/Boot 使用  | 显式删除或存储策略决定             |

不要将这些资源简单串成同一个析构链。

真正的系统里，它们可能同时存在、互相引用，而且具有不同的 Ownership 和 Lifetime。

## **6.7.3 修改影响范围的统一分析框架**

以后只要修改 OP-TEE Core 中的任何关键代码，我建议都进行下面的七项检查：

| 分析维度             | 需要问的问题                                      |
| ---------------- | ------------------------------------------- |
| Execution        | 当前运行于哪个权限级、CPU、Thread、TA Context？           |
| Ownership        | 对象由谁分配、持有、共享和释放？                            |
| Synchronization  | 哪些锁保护状态？哪些路径可能等待或 RPC？                      |
| Security         | 当前输入和调用身份是否可信？谁执行权限验证？                      |
| State Transition | 修改前后状态机是否仍然完整？                              |
| Failure          | 分配失败、Timeout、Panic、Cancellation 后如何恢复？      |
| Compatibility    | 是否影响其他 Transport、Architecture Backend 或旧数据？ |

这些问题不是独立的。

例如，一个 Reference Count 错误可能最终变成 Use-After-Free；一个 Lock Ordering 错误可能使 RPC 永久阻塞；一个失败的 Address Space Restore 可能造成跨 TA 内存访问。

所以必须把 **Correctness、Concurrency 和 Security** 放在同一个系统模型中分析。

# **6.8 本章实战：建立六条可以重复执行的 Trace**

我建议这一章结束前，完成六个源码追踪实验。

实验无需同时完成；可以先利用 QEMU 和上游测试环境建立基线，再迁移到你的 RISC-V 环境。

| 实验                | 关键入口                      | 最重要的观察对象                            |
| ----------------- | ------------------------- | ----------------------------------- |
| Boot Trace        | `init_primary()`、Initcall | 初始化顺序与 Kernel Global State          |
| OpenSession Trace | `entry_open_session()`    | Session、TA Context、Loader           |
| Syscall Trace     | `scall_handle_user_ta()`  | User/Kernel Boundary、当前 TA          |
| Storage RPC Trace | `thread_rpc_cmd()`        | Thread Suspend、RPC Buffer           |
| Interrupt Trace   | `thread_state_suspend()`  | Registers、Thread State、User Mapping |
| Cleanup Trace     | `close_session()`         | Refcount、Busy、Unlink、Destroy        |

建议以调试器的硬件或软件断点配合 Trace Log，不要在所有 Kernel Hot Path 中无差别增加大量日志，因为日志本身可能改变并发时序。

## **实验 A：观察 Session 和 TA Context 是否一对一**

准备一个支持 Single Instance 和 Multi Session 的实验 TA。

步骤：

1. CA-1 OpenSession。
2. CA-2 OpenSession。
3. 观察 Session ID 和 TA Context。
4. CA-1 CloseSession。
5. 验证 CA-2 是否仍然可正常 Invoke。
6. CA-2 CloseSession。
7. 检查 TA Context 是否销毁，或者因为配置被保留。

验收标准：

可以明确说明 Session 与 Context 何时共享，Refcount 如何变化，何时允许 Destroy。

## **实验 B：RPC 前后 Trusted Thread 状态**

设置断点：

```
break thread_rpc_cmd
break thread_state_suspend
break thread_resume_from_rpc
```

具体符号可用性取决于构建配置、架构和编译优化。

触发一次 REE FS Storage 操作。

至少记录：

```
Current CPU
Thread ID
Thread State
Current Session
TA Context
Active User Mapping
RPC Command
RPC Buffer Reference
```

验收标准：

可以证明一次 RPC 并没有销毁 Thread Context，而且恢复后重新进入了正确的执行路径。

## **实验 C：关闭 Session 时的资源回收**

准备一个 TA，在一个 Session 中分配多种 Crypto State、Storage Handle 和临时资源。

之后故意不主动释放部分 Handle，而是关闭 Session 并让 TA Context 最终销毁。

观察：

- `close_session()`
- `destroy_session()`
- `destroy_context()`
- `release_utc_state()`
- `tee_svc_cryp_free_states()`
- `tee_obj_close_all()`

验收标准：

能准确说出哪些资源是 Session-owned，哪些是 Context-owned，哪些是 Persistent，并证明 Context 销毁之后没有遗留不应存在的运行时引用。

# **6.9 Staff Engineer 级综合考核**

12 道开放式架构题 · 重点考察推理、设计判断、故障分析和安全不变量

这一章的面试题不考“你是否记住了某个函数叫什么名字”。

假设你已经能够利用 AI 搜索源码、生成调用图、分析寄存器，并快速查询所有规范。

在这种前提下，下面的问题要求你像 OP-TEE Core Maintainer 一样，面对不完整的系统信息，建立正确模型，进行设计判断，并拿出可验证的技术方案。

建议每题先独立思考，再看参考分析。

## **Question 01 — 如何证明一次 OpenSession 不会产生半初始化 TA？**

Architecture & Concurrency

假设某个 Single Instance TA 的首次加载需要 200ms。

在它被加载到一半时，另外 7 个 CPU 同时收到针对这个 UUID 的 `OpenSession()` 请求。

更复杂的是：

- 第一条加载路径可能成功，也可能在 Relocation 阶段失败。
- 第二条请求可能正在等待 TA Context 初始化。
- 第三条请求可能在等待期间被取消。
- Normal World 可以故意重复发起请求，制造高并发压力。

**问题：**

请设计一个能够保证正确性的 TA Context Initialization State Machine。

你必须说明：Context 什么时候可以发布到全局列表？等待者如何被通知？初始化失败由谁清理？如何防止 Duplicate Context、Use-After-Free 与 Deadlock？

**参考分析：**

关键是区分 Context 的 *存在* 与 *可用*。

可以把 Context 分为 `INITIALIZING`、`READY`、`FAILED/RELEASING` 等状态。它进入全局列表，并不意味着其他调用者可以使用它。等待者必须在同步机制保护下检查状态；如果等待过程释放了 Mutex，醒来后必须重新查找 Context，而不是继续使用先前保存的裸指针。

初始化成功需要在发布 READY 状态之前保证必要内存映射、Loader 和运行时数据均有效；失败则必须让已有等待者能够重新判断状态，同时完成完整回滚。

更深的考点是为什么不能持有全局 TA Mutex 覆盖全部 Loader 工作。因为 Loader、VM Allocation、Pager 等路径可能依赖其他锁或等待机制。如果不分析 Lock Dependency Graph，仅仅使用一个大锁解决重复创建问题，反而可能导致全局死锁。

优秀回答还应该给出 Linearization Point、Lock Ordering 和 Fault Injection Tests。

## **Question 02 — Thread Pool 耗尽，但 CPU 利用率只有 5%**

Resource Management

某设备配置：

```
CFG_NUM_THREADS = 8
CPU Cores       = 16
```

运行过程中：

- 8 个 Trusted Thread 都在等待 RPC。
- CPU 利用率很低。
- 第 9 个 CA 请求无法得到及时服务。
- Linux 仍有大量空闲 CPU。

有人建议：

> 既然 CPU 空闲，那就像 Linux 一样动态扩展 Trusted Thread 数量，或者给每个 CPU 固定分配一个 Trusted Thread。

请评估这两个方案。

**参考分析：**

首先要指出资源瓶颈并不是 CPU，而是 Trusted Thread Context。

一个 Suspended Thread 仍然占据 Kernel Stack、保存的寄存器状态及相关资源引用。动态扩大 Thread Pool 必须重新审查 Stack Memory、Pager、Context Table、Per-CPU Mapping、Security Hardening 和最坏情况内存预算。

固定每 CPU 一个 Trusted Thread 也不一定合理，因为请求并发数与 CPU 数量不是同一概念，而且 RPC Suspend 与恢复不等价于固定 CPU 亲和性。

真正需要先测量的是 RPC 持续时间、并发峰值、Thread 占用分布以及服务依赖关系。优化方案可能包括减少不必要的 RPC、缩短服务等待时间、改进服务并发设计，或在完成资源预算和压力测试后调整 Thread Pool。

更重要的是明确：恶意 Normal World 拒绝完成 RPC 属于 Availability Threat，扩大 Thread Pool 并不能根本消除这种威胁。

## **Question 03 — 只增加一行 memcpy，就能修复 Shared Memory TOCTOU 吗？**

Security Boundary

假设某个 OP-TEE Syscall 从 Normal World Shared Memory 读取一个请求结构：

```
struct request {
    uint32_t command;
    uint32_t length;
    uint64_t buffer;
};
```

Kernel 先验证 `length`，随后根据 `buffer` 访问数据。

安全团队发现，Normal World 可以在验证和实际使用之间修改这个结构。

工程师提出：

```
memcpy(&local_req, shared_req, sizeof(local_req));
```

然后所有检查都针对 `local_req` 执行。

请分析这个修复是否充分。

**参考分析：**

Snapshot 是必要的设计方向，但仅复制结构体还不一定够。

首先，复制本身必须保证不会越界或错误访问不可信内存；其次，`buffer` 仍然指向可能被修改的数据，必须区分外层 Descriptor 的稳定性与它引用的数据内容的稳定性。

如果业务需要请求数据在整个操作期间保持不变，还必须复制有效负载或采用可靠的 Ownership/Access Control 协议。否则攻击者仍然可以在后续处理期间修改数据。

优秀回答还应覆盖整数溢出、嵌套指针、Buffer Lifetime、Concurrent Unregistration、DMA 访问，以及不同 Memory Sharing Transport 的语义差异。

核心原则是：

**验证一次结构体，不代表验证了它指向的整个对象图。**

## **Question 04 — 为什么一个 Storage RPC 优化 Patch 会造成系统级死锁？**

Lock Dependency Analysis

某工程师为了减少锁操作，把 Storage、Crypto 和 RPC 的部分公共路径统一放在一个 Mutex 下。

单线程测试全部通过。

但 16 核高并发测试时偶尔死锁。

调用关系为：

```
Thread A
    Storage Lock
       ↓
    Crypto Service
       ↓
    RPC Wait

Thread B
    Another Kernel Service
       ↓
    Wait Storage Lock
```

同时 Normal World RPC 服务可能触发其他 TEE 请求。

请解释如何系统地分析这个死锁。

**参考分析：**

不能只看 Thread A 和 B 当前持有的锁。必须建立跨越 Secure World、RPC 和 Normal World Service 的 Wait-For Graph。

一个 RPC 等待可能在 Secure World 内保留部分锁状态；Normal World 在执行 RPC 时可能又依赖新的 Secure World 请求。如果新请求需要 Thread A 持有的锁，就可能形成跨世界循环依赖。

应调查所有可能 Sleep、RPC 和 Re-entry 的函数，标记 Lock Acquisition Order、Thread Context 和调用层级。

修复不一定是简单地释放 Mutex，因为提前释放锁可能使 Storage Transaction 的内部状态暴露给并发调用。可能需要重新设计事务生命周期、分离状态锁与操作锁，或通过明确的 Version/Generation 验证恢复后的状态。

必须同时证明 Deadlock Freedom 和 Data Consistency，不能只证明“不会再卡住”。

## **Question 05 — Foreign Interrupt 恢复到了错误的 TA 地址空间**

Context Restoration & Isolation

一个 8 核 OP-TEE 平台偶尔出现：

```
TA-A executes normally
    ↓
Foreign Interrupt
    ↓
Trusted Thread Suspended
    ↓
Resume on another CPU
    ↓
Unexpected memory access fault
```

在恢复后，发现当前页表似乎对应 TA-B。

请提出完整的问题定位方案。

**参考分析：**

首先把问题划分为三个层次：

**Execution State：** Trusted Thread ID、Registers、PC、SP、Current CPU 是否正确。

**Address Space State：** `have_user_map`、保存的 User Mapping、ASID、Translation Table 和 TLB State 是否正确。

**Ownership State：** 当前 Session 对应的 TA Context 与将要恢复的 User Mapping 是否一致。

然后在 Suspend 和 Resume 的边界记录可信状态，确认 User Mapping 的保存、失效、重新激活及 TLB 同步顺序。

还要关注 CPU Migration、Per-CPU Current Thread State、Lazy Floating-Point State，以及新的架构 Backend 是否遗漏了原有 Arm Backend 的安全不变量。

不能仅凭一次 Page Fault 就断定是页表错误，也可能是 Thread Context 错误、TA VM 被提前销毁或异常恢复过程污染了状态。

优秀回答应设计一个可复现的多核中断压力实验，并明确如何区分根因与故障的后续症状。

## **Question 06 — CloseSession 与 InvokeCommand 并发，怎么证明没有 UAF？**

Lifetime & Synchronization

同一个 Session 上发生：

```
CPU-0                         CPU-1
InvokeCommand(S)              CloseSession(S)
       |                             |
       |                             |
    Running                     Destroy?
```

请说明你会如何设计 Session Lifetime 和同步机制，以证明不会出现 Use-After-Free。

**参考分析：**

首先需要区分 Session Lookup、Reference Acquisition、Exclusive Use、Unlink 和 Free。

仅在 Close 时检查 Reference Count 不够，还必须确保新的 Lookup 不会在对象进入释放状态后继续成功获取引用。

一个可证明的方案应该建立明确的 Session 状态转换，限制新引用的获取，并等待已存在的引用结束。

这里的关键问题不是“有无 Mutex”，而是：

- 获取引用与对象状态检查是否原子化。
- 对象进入 Unlinking 后是否还能被发现。
- 最后一个引用如何触发回收。
- 持有引用的路径是否可能永久等待其他资源。
- 是否存在 Nested Session 或回调导致的自死锁。

如果 Close Entry 本身可能导致死锁，还必须有安全的异常回收策略。

优秀回答需要说明每个状态转换的 Linearization Point，并设计重复 Close、并发 Invoke、Panic 和 Session ID Reuse 等测试。

## **Question 07 — TA Panic 时还存在未完成的 Secure DMA**

Hardware & Kernel Resource Ownership

一个 User TA 使用 Hardware AES Engine。

执行中：

1. TA 发起 AES-GCM。
2. Secure DMA 已经启动。
3. TA 因其他错误触发 Panic。
4. Kernel 开始销毁 TA Context。
5. Crypto Driver 仍然可能收到 DMA Completion Interrupt。

此时如何安全清理？

**参考分析：**

不能简单销毁 TA VM 或释放 DMA Buffer。

必须先定义 DMA Operation 的 Ownership，并明确设备何时失去访问缓冲区的能力。

通常需要设计 Operation State Machine，例如 `PREPARED → RUNNING → COMPLETING/CANCELLING → QUIESCED → RELEASED`。

取消路径要考虑硬件是否支持 Abort、Abort 是否真正停止总线访问、设备 Reset 后是否仍存在 Posted Transaction，以及 IOMMU/SMMPT 的设备侧 Translation Cache 是否需要同步。

只有当设备不再可能访问缓冲区，才可以撤销 Mapping 并最终释放底层内存。

同时，IRQ Handler 不得访问已经释放的 Operation Context。需要保证 Completion 与 Cancellation 的竞争能够收敛到一次最终释放。

优秀回答还要考虑发生设备故障时，应该返回错误、隔离设备、Reset Driver，还是将整个安全服务标记为不可用。

这是典型的跨 TA Runtime、Crypto、Memory、Driver、Interrupt 和 SoC Protection 的系统级题目。

## **Question 08 — 更新 OP-TEE 后 Secure Storage 无法打开旧文件**

Compatibility & Recovery

系统从旧版 OP-TEE 升级到新版。

所有 Crypto 算法测试通过，TA 也能正常加载，但 Persistent Object 读取失败。

系统团队认为：

> API 没有变化，而且 AES-GCM 也没有变化，所以这一定是文件系统 Bug。

你如何处理这次升级事故？

**参考分析：**

首先区分 API Compatibility、Binary Compatibility 和 On-Disk Format Compatibility。

即使密码算法不变，文件布局、Metadata、Version、Key Derivation、Object Namespace 和 Hash Tree 格式发生变化，也可能使旧数据不可读取。

应先确认版本之间是否存在已知格式变更，而不是直接修改密码学代码。

然后使用受控测试设备与备份镜像定位：

- 文件是否被找到。
- Object Metadata 是否有效。
- 使用了哪个 Storage Backend。
- Key Derivation 是否变化。
- Hash Tree 或 Authentication 是否失败。
- 是否涉及设备密钥或 Secure Boot 配置变化。

更重要的是设计升级策略：数据迁移由谁执行、迁移如何认证、掉电如何恢复、是否允许降级、是否需要版本化的 On-Disk Schema。

不能为了恢复可用性而简单跳过完整性检查。

## **Question 09 — 一个 OpenSession 失败，但系统资源持续增长**

Failure Path & Leak Analysis

压力测试反复执行：

```
OpenSession(UUID)
    ↓
TA Loader fails at relocation
    ↓
Return error
```

运行数小时后，Secure Memory 耗尽。

请设计调试方案。

**参考分析：**

首先不能仅通过 Heap Allocated Bytes 判断泄漏来自哪里。OP-TEE 可能同时使用 Core Heap、Page Allocator、TA VM、MOBJ、Pager 和 RPC Buffer Cache。

应以一次 OpenSession 的资源获取链为基础，建立：

```
Acquire Resource
       ↓
Publish / Link Resource
       ↓
Failure
       ↓
Rollback Resource
```

需要检查每个分配阶段的错误分支，尤其是 TA Context 已加入全局列表、VM 初始化完成一部分、`ldelf` 尚未完成加载的状态。

应记录每轮调用前后的 Allocation、Refcount、VM Region、Session Count、Context Count 和缓存状态，区分真正泄漏与设计上可复用的缓存。

优秀回答不仅能够定位遗漏的 Free，还应该验证错误路径不会引入 Double Free、Dangling Pointer 或被其他等待者访问的 Context。

## **Question 10 — 将 OP-TEE Transport 从 SMC 切换到 FF-A，哪些代码应当保持不变？**

Architecture Abstraction

某平台最初使用传统 SMC Transport。

现在计划迁移到 FF-A，并希望已有的 User TA、Crypto Service 和 Secure Storage 功能保持兼容。

请设计迁移架构并评估修改范围。

**参考分析：**

应该优先保持 TA Session、User TA Runtime、Syscall、Crypto、Storage 的架构无关语义稳定。

需要重点改变的是：

- External Transport。
- Endpoint Identification。
- Message Dispatch。
- Shared Memory Lifecycle。
- RPC/Notification Transport。
- Memory Share/Retrieve/Relinquish。
- 启动及 Secure Partition 管理相关接口。

但不能假定只修改 Entry Assembly 就够了。

FF-A 的内存共享与传统 Registered Shared Memory 可能具有不同的 Ownership、Lifetime 和 Reclamation 语义，因此 MOBJ、消息参数转换、RPC 和错误清理都需要检查。

优秀回答会提供明确的 Transport Abstraction Boundary，并设计同一组 GP API 回归用例分别运行于 SMC 和 FF-A。

目标不是让所有 Transport 使用完全相同的底层代码，而是让它们满足相同的上层服务契约。

## **Question 11 — Normal World 故意永远不恢复某个 Suspended Trusted Thread**

Trust Model & Availability

一个恶意 Linux Kernel 在收到 RPC 后，故意不再恢复对应的 Secure Thread。

长期运行后，Trusted Thread Pool 被逐渐占满。

安全团队提出：

> Secure World 应该有一个 Watchdog，超时后强制销毁未恢复的 Trusted Thread。

请评估。

**参考分析：**

首先区分 Confidentiality/Integrity 与 Availability。

Normal World 不恢复请求，本身已经证明它能够影响某些 TEE 服务的可用性。但这不意味着它可以访问或修改 Suspended Thread 的可信执行状态。

超时后强制销毁 Thread 看似合理，实际上可能破坏：

- Kernel Stack 生命周期。
- 已持有的锁。
- Session/TA Context 引用。
- Storage Transaction。
- Hardware Operation。
- RPC Buffer。
- 尚未完成的 Cleanup。

更可靠的设计应考虑受控 Cancellation Points、可中止 Operation、服务级 Deadline、异常恢复协议或资源配额。

如果强制终止无法保证安全，则宁可让某个服务失败或限制其可用性，也不能制造内存破坏和权限隔离漏洞。

优秀回答需要明确系统愿意接受怎样的 Availability Guarantee，以及哪些威胁在其 Trust Model 范围内无法彻底防止。

## **Question 12 — 你要评审一个修改了 120 行代码的 OP-TEE Patch**

Maintainer Review

一个 Patch 修改了：

```
core/kernel/tee_ta_manager.c
core/kernel/user_ta.c
core/mm/vm.c
```

Patch 作者声称：

> 修复了 TA 初始化失败时的内存泄漏，已经通过 xtest 1001，并且能够正常启动 Linux。

你是 Maintainer。

请说明在接受 Patch 前，至少需要完成哪些分析与验证。

**参考分析：**

第一步不是立即查看测试结果，而是要求作者明确修复前的 Failure Scenario，以及 Patch 改变了哪个资源的 Ownership 或 Lifetime。

接下来建立完整的差异分析：

**State：** Context Initialization、Session Creation、TA VM Creation 和 Failure Cleanup 的状态转换是否改变。

**Concurrency：** 是否影响正在等待 TA 初始化的其他 Thread？是否新增锁顺序风险？

**Memory：** 页表、VM Region、ASID 和共享映射是否可能重复释放或遗漏释放？

**Security：** 是否可能把半初始化 Context 暴露给外部请求？是否影响跨 TA 隔离？

**Compatibility：** PTA、Single Instance、Multi Instance、Keep Alive、Arm/RISC-V Backend 是否受影响？

**Testing：** 是否覆盖 OOM、Loader Failure、并发 Open、Panic、Close 和重复请求？

仅通过一个 xtest 和 Linux Boot，无法证明这些属性。

优秀的 Maintainer Review 应该能够提出最小、可验证的反例，或者给出清晰的 Safety Argument，而不是仅仅说“需要增加测试”。

# **6.10 综合考核评分标准**

这 12 道题可以作为完整的 Staff Engineer 技术考核。

Staff Engineer Assessment Rubric

问题建模

20%

能否将问题分解成不同执行上下文、对象和状态机

架构判断

20%

能否解释设计取舍，而不是套用通用 OS 的解决方案

安全与并发

25%

能否识别 TOCTOU、UAF、Deadlock、Privilege Boundary

异常处理

15%

能否覆盖 Panic、OOM、Timeout、Partial Initialization

验证能力

20%

能否设计能证伪自己假设的实验和测试

我尤其看重两种能力。

**第一种：能否提出反例。** 当别人提出一个看上去合理的优化方案时，你是否能够找到一个并发、异常或安全边界条件，使它的前提不再成立。

**第二种：能否设计证明方法。** 即使自己提出了一个方案，也必须能够说明它在什么条件下会失败，以及通过怎样的实验检测这种失败。

这两种能力比单纯记忆 API、结构体和函数调用关系更接近真正的架构能力。

# **6.11 Chapter 06 最终总结**

本章我们学习了六条核心执行链：

| 执行链                        | 最需要掌握的本质                               |
| -------------------------- | -------------------------------------- |
| Boot → Core Ready          | 逐步建立系统运行与安全不变量                         |
| OpenSession → TA Entry     | 建立 Client、Session、TA Context 之间的生命周期关系 |
| TA → Syscall → Crypto      | 安全地穿越 User/Kernel Privilege Boundary   |
| Storage → RPC → REE        | 在保持安全决策权的前提下利用不可信外部服务                  |
| Interrupt → Suspend/Resume | 在跨执行环境切换时维护可信执行状态                      |
| CloseSession → Cleanup     | 保证资源不会提前释放、重复释放或者永久泄漏                  |

其中最重要的不是六条调用链本身，而是贯穿所有路径的三条原则：

**1. Execution Context 不等于 Resource Ownership。**

CPU、Trusted Thread、Session、TA Context 和 Memory Object 各自有不同的状态与生命周期，不能因为一次执行结束就推断所有相关资源都应释放。

**2. 每一次跨越信任边界，都必须重新确认输入与权限。**

Normal World、User TA、OP-TEE Kernel 甚至不同 Secure Partition 之间，不能因为代码都运行在某种安全环境里就自动建立信任。

**3. 系统正确性必须对异常和并发路径同样成立。**

一次成功的运行不代表系统正确。Kernel 的安全性取决于所有允许的状态转换是否维持其不变量。

## **本章之后应该达到的能力**

面对一个陌生的 OP-TEE Core Function，你应该逐渐形成这样的分析习惯：

```
What does this function do?
          |
          v
Who calls it?
          |
          v
In which execution context?
          |
          v
What objects does it own or reference?
          |
          v
Which invariants must remain true?
          |
          v
What happens on failure or concurrency?
          |
          v
What else could break if I change it?
          |
          v
How can I prove the change is correct?
```

这也是我希望整个 OP-TEE OS Internals 系列最终建立起来的工作方式。

**Chapter 06 的核心成果不是掌握六条固定的调用链，而是学会把 OP-TEE 看作一个由状态机、资源所有权、执行上下文和安全边界共同组成的完整系统。**

后续 Chapter 07–12 将进一步进入 Boot、Early Runtime、Initcall、Memory Initialization 和 SMP Initialization 的源码细节。届时我们不仅要知道每个初始化函数做了什么，还要解释每一步为什么必须发生在那个阶段，以及它为后续运行时建立了哪些前置条件。
