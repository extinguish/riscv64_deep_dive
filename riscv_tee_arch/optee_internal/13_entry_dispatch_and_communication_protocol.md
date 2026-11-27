# **Chapter 13 — OP-TEE Entry Dispatch & Communication Protocol**

OP-TEE OS Internals · PART III — Kernel Execution Model & Concurrency

**源码基线：OP-TEE OS 4.11.0**

本章研究 OP-TEE OS 最重要的入口机制之一：Normal World 的请求如何跨越安全边界，变成 Secure Kernel 内部的服务调用。

我们不仅要知道 `SMC → thread → tee_entry_std()` 这条路径，更需要理解：

- 为什么 OP-TEE 要把 Transport、Execution Context 和 Service Dispatch 分开？
- 为什么 Fast Call 和 Yielding Call 必须使用不同的执行约束？
- Normal World 提供的参数究竟在什么时候可以被 Secure Kernel 使用？
- 为什么一个看起来只是传递 Buffer 的 API，实际上会涉及 MMU、MOBJ、Session、Thread 和 RPC？
- 如何保证错误返回、资源释放和中断状态始终符合内核安全约束？

这部分内容与 Chapter 14 的 Thread、Chapter 18 的 RPC、Chapter 22 的 MOBJ 和 Chapter 29 的 Session Manager 密切相关。

## **13.0 本章学习目标**

完成本章后，你应当能够独立解释下面这条调用链：

```
Normal World
  |
  +-- Client Application
  |      TEEC_OpenSession()
  |
  +-- libteec
  |      ioctl(TEE_IOC_OPEN_SESSION)
  |
  +-- Linux OP-TEE Driver
  |      Prepare optee_msg_arg
  |      Setup Shared Memory
  |      Invoke SMC
  |
  v
Secure Monitor / Firmware
  |
  |  Save NS context
  |  Restore Secure context
  v
OP-TEE Secure Entry
  |
  +-- Fast Call Path
  |      tee_entry_fast()
  |
  +-- Standard / Yielding Call Path
         |
         v
     Allocate Trusted Thread
         |
         v
     thread_std_smc_entry()
         |
         v
     Validate Message Buffer
         |
         v
     tee_entry_std()
         |
         +-- OPEN_SESSION
         +-- INVOKE_COMMAND
         +-- CLOSE_SESSION
         +-- CANCEL
         +-- Shared Memory operations
         +-- Other supported commands
         |
         v
     TA Manager / Kernel Service
         |
         v
     Return Result
```

这里展示的是 **Arm 平台采用传统 OP-TEE SMC ABI 时的主路径**。FF-A 的 Transport 和入口不同；不同架构也不能直接照搬 Arm 的汇编入口。

# **13.1 从设计理念开始：为什么 OP-TEE 需要多层 Entry？**

我们先不阅读代码，而是讨论一个操作系统设计问题。

假设你正在设计一个 TEE OS。Normal World 需要调用安全服务，最简单的方案可能是：

```
switch (smc_func_id) {
case OPEN_SESSION:
    open_session();
    break;
case INVOKE_COMMAND:
    invoke_command();
    break;
}
```

看起来非常直观。

但是，它隐藏了多个问题。

首先，`open_session()` 可能需要加载 TA，而加载 TA 又可能需要访问 Linux Filesystem。这意味着安全内核执行过程中必须能够暂停、返回 Normal World，并在之后恢复。

其次，调用方可以传递内存地址。这个地址来自不可信环境，不能未经检查就转换成 Secure Kernel 可以访问的指针。

再次，多个 CPU 可能同时进入 OP-TEE。系统必须知道哪个执行上下文属于哪个请求，以及如何隔离它们。

最后，如果将来系统从 SMC 切换到 FF-A，难道要重新实现一遍 Session Manager 和 TA Loading？

这些问题决定了需要对入口进行分层。

## **13.1.1 四个基本抽象**

Layer 1 — Transport / Conduit

请求如何跨安全边界到达 OP-TEE？

SMC · FF-A · 平台适配入口

Layer 2 — Execution Context

请求应该在哪种内核执行上下文中运行？

Entry Stack · Trusted Thread · Suspend / Resume

Layer 3 — Message & Parameter Validation

请求是什么？参数是否合法？引用了哪些内存对象？

optee_msg_arg · MOBJ · Attributes · Metadata

Layer 4 — Service Dispatch

应该调用哪个内核服务，如何管理服务对象？

tee_entry_std · TA Manager · Session · RPC

这四层并非严格一一对应四个源码文件，而是四种不同的职责。

最重要的架构原则是：

**Transport 决定请求怎样到达；Execution Context 决定请求怎样运行；Message Layer 负责安全地解释请求；Service Layer 决定请求实际做什么。**

在 4.11.0 中，这种分离能够通过 `thread_optee_smc.c`、`thread_spmc.c`、`entry_std.c` 等实际代码观察到。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



### **13.1.2 这为什么对 Porting 很重要？**

假设你要将 OP-TEE 从 Arm 移植到 RISC-V，并通过某种 Firmware/SBI/Domain Switch 路径进入 TEE。

正确的方法不是首先重写 `tee_ta_manager.c`，而是识别以下边界：

| 职责                            | 理想的移植策略                        |
| ----------------------------- | ------------------------------ |
| CPU 特权切换                      | Architecture-specific          |
| CSR/寄存器状态保存                   | Architecture-specific          |
| Entry Stack 与 Thread Stack 切换 | Architecture-specific          |
| Transport 消息接收                | Transport-specific             |
| OP-TEE Message ABI            | 尽量保持共享契约                       |
| 参数验证与 MOBJ 解析                 | 优先复用 Core 抽象                   |
| TA Session 管理                 | 优先复用 Architecture-neutral Core |
| Crypto/Storage 服务             | 尽量复用现有实现                       |

当然，这并不意味着 RISC-V 可以无条件复用所有代码。必须先证明它提供了相同的执行、内存隔离、共享内存和资源生命周期语义。

这里有一个贯穿本章的重要概念：

> **不同架构可以使用不同的入口机制，但如果要复用同一个 OP-TEE Core Service，就必须满足该 Service 所依赖的全部前置条件和安全不变量。**

# **13.2 Fast Call 与 Yielding Call：为什么必须分开？**

这是理解 OP-TEE 执行模型的第一个关键点。

## **13.2.1 两类请求在执行性质上的不同**

Arm SMCCC 使用 Function ID 描述调用，其中一个重要字段用于区分 Fast Call 与 Standard Call。

OP-TEE 在传统 SMC ABI 中延续这一机制。

| 属性                | Fast SMC                | Standard / Yielding SMC     |
| ----------------- | ----------------------- | --------------------------- |
| 主要用途              | Capability、版本、简短控制服务    | OpenSession、InvokeCommand 等 |
| 执行栈               | Entry Stack             | Trusted Thread Stack        |
| Trusted Thread 分配 | 通常不分配                   | 需要                          |
| 是否允许等待外部服务        | 不允许                     | 可以通过 RPC 等机制                |
| 是否允许执行复杂 TA 服务    | 不适合                     | 是主要路径                       |
| 中断处理              | Fast Handler 维持入口异常屏蔽约束 | 按执行阶段控制中断状态                 |
| 典型时长              | 应短小、可预期                 | 可以较长                        |
| 是否可能挂起后恢复         | 通常不允许                   | 支持                          |

注意：Fast Call 与 Yielding Call 的根本区别不是某个固定微秒数阈值，而是 **执行上下文和能否挂起的契约**。

官方架构文档明确区分了两种路径：Fast SMC 在 Entry Stack 上处理，而 Standard SMC 使用 Trusted Thread，并允许在需要 Normal World 服务时暂停后恢复。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## **13.2.2 为什么 Fast Call 不应该等待 Mutex？**

思考以下情景。

CPU0 正在处理一个 Fast Call：

```
CPU0 / Secure World

tee_entry_fast()
    |
    v
mutex_lock()
    |
    +-- Mutex 已被其他 Thread 持有
    |
    v
等待 Mutex
```

如果这个 Mutex 的等待路径依赖 RPC，那么调用需要：

1. 保存当前 Trusted Thread Context。
2. 返回 Normal World。
3. 在条件满足后重新进入 Secure World。
4. 恢复原来的执行上下文。

但是 Fast Call 通常没有为这种挂起/恢复机制分配标准 Trusted Thread。

因此，若直接在 Fast Call Handler 中使用可能进入等待/RPC 的 Mutex，实际上就违反了执行模型。

这还不是单纯的性能问题。

如果 Fast Call 入口保持了特定中断屏蔽状态，而等待路径又要求其他 CPU 或中断处理器推进状态，系统可能死锁。

所以 Fast Call 应该只执行与其上下文约束兼容的短操作；不能因为一个函数名称看起来简单，就认为它可以安全放入 Fast Handler。

## **13.2.3 源码：Fast Call 的两个关键层次**

4.11.0 中可以观察以下路径：

```
Secure Entry
    |
    v
thread_handle_fast_smc()
    |
    v
tee_entry_fast()
    |
    v
__tee_entry_fast()
    |
    +-- GET_OS_REVISION
    +-- EXCHANGE_CAPABILITIES
    +-- GET_THREAD_COUNT
    +-- GET_SHM_CONFIG
    +-- ...
```

重要源码：

- `core/arch/arm/kernel/thread_optee_smc.c`
- `core/arch/arm/tee/entry_fast.c`

`thread_handle_fast_smc()` 包装 Fast Handler，并在退出时检查 Exception Mask 的约束；`__tee_entry_fast()` 根据外层 Function ID 分发不同的控制操作。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



一个很典型的例子是：

```
case OPTEE_SMC_EXCHANGE_CAPABILITIES:
    tee_entry_exchange_capabilities(args);
    break;
```

这个请求用于 Normal World 与 Secure World 协商能力，例如是否支持动态 Shared Memory、Notification 等。

它不应该启动一个 TA，也不应该尝试动态加载文件。

### **Fast Call 的隐藏挑战**

假设要增加一个 Fast Call，用于读取 Secure Hardware 当前状态。

函数看起来只是：

```
uint32_t secure_hw_get_status(void);
```

但如果内部需要打开 Clock、访问可能休眠的总线控制器，或者等待其他 CPU 完成一次设备复位，这个调用可能根本不适合 Fast Call。

**设计入口类型时，必须研究整个 Transitive Call Graph，而不只是入口函数本身。**

# **13.3 OP-TEE 有两层不同的 Command Dispatch**

这一节非常重要，因为很多工程师会把 SMC Function ID 和 OP-TEE Message Command 混在一起。

它们是两个不同层次的协议标识。

## **13.3.1 外层：SMC Function ID**

在传统 SMC 路径中，Normal World 可能使用：

```
OPTEE_SMC_CALL_WITH_ARG
```

这个 Function ID 表示：

> 我希望 OP-TEE 处理一个位于共享内存中的消息对象。

它本身并不直接代表 OpenSession 或 InvokeCommand。

Function ID 还包含 Fast/Standard、Calling Convention 和 Owner 等编码字段。4.11.0 的定义位于 `core/arch/arm/include/sm/optee_smc.h`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



对于典型的 `CALL_WITH_ARG`：

```
a0 = OPTEE_SMC_CALL_WITH_ARG
a1 = message buffer PA high 32 bits
a2 = message buffer PA low 32 bits
```

注意这里按 OP-TEE 传统 SMC ABI 的约定描述。不能将这种寄存器布局直接视为所有其他 Transport 的公共 ABI。

## **13.3.2 内层：OP-TEE Message Command**

Linux Driver 会在 Shared Memory 中准备一个 `optee_msg_arg`。

例如：

```
arg->cmd = OPTEE_MSG_CMD_OPEN_SESSION;
arg->num_params = 2 + number_of_ta_params;
```

其中 `OPEN_SESSION` 才是具体要执行的服务。

因此一次调用包含两次逻辑分发：

```
SMC Function ID
      |
      v
OPTEE_SMC_CALL_WITH_ARG
      |
      v
Locate & Validate Message Buffer
      |
      v
optee_msg_arg::cmd
      |
      +-- OPEN_SESSION
      +-- INVOKE_COMMAND
      +-- CLOSE_SESSION
      +-- CANCEL
      +-- ...
```

从架构上看，这是一种 **Transport Envelope + Service Payload** 的设计。

外层负责安全地找到消息载体，内层负责解释实际请求。

## **13.3.3 为什么不为每个 Service 定义一个 SMC Function ID？**

当然可以这样设计，但会产生明显的问题。

一方面，Session 操作、Shared Memory 操作、Cancel 操作需要传递不同数量、不同类型的数据，寄存器接口很难优雅地承载所有复杂参数。

另一方面，未来如果需要改用 FF-A Transport，就必须重新设计各个服务的物理通信接口。

把复杂参数放进版本化的消息结构，可以使调用接口更容易演进和复用。

不过，这种设计也带来了一个新的攻击面：

**Secure Kernel 必须解析来自 Normal World 的复杂消息，而消息本身完全可能是不可信的。**

这就是下一节的重点。

# **13.4 OP-TEE Message Protocol：从 ABI 到内部数据结构**

现在进入真正的源码。

OP-TEE Message Protocol 的核心定义位于：

`core/include/optee_msg.h`

这是 Linux OP-TEE Driver 与 OP-TEE Secure Kernel 之间非常重要的一份 **Binary ABI Contract**。

我们先研究两个核心结构体。

## **13.4.1&#x20;**`struct optee_msg_arg`

4.11.0 的结构体定义如下：

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

源码：`core/include/optee_msg.h`，OP-TEE 4.11.0。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

逐一分析。

| 字段           | 作用                      | 设计意义                           |
| ------------ | ----------------------- | ------------------------------ |
| `cmd`        | OP-TEE Kernel Command   | 决定 Service Dispatch            |
| `func`       | TA 内部 Command ID        | 区分 Kernel Command 与 TA Command |
| `session`    | Session ID              | 标识已打开的 TA Session              |
| `cancel_id`  | Cancellation Identifier | 与请求取消相关的 ABI 字段                |
| `pad`        | 布局填充                    | 保证后续字段布局                       |
| `ret`        | TEE 服务返回结果              | 表示操作的业务结果                      |
| `ret_origin` | 错误来源                    | 说明错误由哪一层产生                     |
| `num_params` | 参数数量                    | 决定后续数组的有效长度                    |
| `params[]`   | 参数数组                    | 保存 Value、Memory Reference 等    |

这里有三个非常重要的设计细节。

### **第一：**`cmd`**&#x20;与&#x20;**`func`**&#x20;不属于同一级别**

例如：

```
arg->cmd  = OPTEE_MSG_CMD_INVOKE_COMMAND;
arg->func = MY_TA_CMD_ENCRYPT;
```

`cmd` 决定 OP-TEE Kernel 应该执行 InvokeCommand。

`func` 决定被调用的 TA 应该执行哪个业务操作。

也就是说：

```
cmd
 |
 v
Kernel Dispatcher
 |
 v
TA Session Manager
 |
 v
TA_InvokeCommandEntryPoint()
                         |
                         v
                       func
```

如果要新增一个 TA 业务命令，通常不需要修改 Kernel Dispatcher。

如果要新增一种 Kernel-Level Command，则可能涉及 Message ABI 和 Kernel Dispatch。

这是完全不同的修改范围。

### **第二：**`session`**&#x20;不是一个可信内核指针**

Session ID 是通过 ABI 传递的标识符。

Normal World 可以修改它，所以不能简单地把它转换成 `struct tee_ta_session *`。

必须经由 Session Manager 查找并校验相关对象。

本章后面会看到 `tee_ta_get_session()`。

### **第三：**`params[]`**&#x20;是 Flexible Array Member**

这里没有固定的数组长度。

实际消息大小由：

```
OPTEE_MSG_GET_ARG_SIZE(num_params)
```

确定。

为什么这个细节重要？

假设 Normal World 提交：

```
num_params = 0xFFFFFFFF
```

如果 Secure Kernel 直接计算：

```
sizeof(struct optee_msg_arg) +
    num_params * sizeof(struct optee_msg_param)
```

就可能出现整数溢出或访问越界。

因此，在真正访问 `params[]` 前，必须先验证 `num_params` 和消息所在内存范围。

这就是典型的 ABI Parsing Security。

## **13.4.2&#x20;**`struct optee_msg_param`

第二个重要结构体是：

```
struct optee_msg_param {
    uint64_t attr;

    union {
        struct optee_msg_param_tmem tmem;
        struct optee_msg_param_rmem rmem;
        struct optee_msg_param_fmem fmem;
        struct optee_msg_param_value value;
        uint8_t octets[24];
    } u;
};
```

`attr` 用于区分参数类型。

主要包括：

| 类型                      | 表示什么                        |
| ----------------------- | --------------------------- |
| VALUE_INPUT             | 输入值                         |
| VALUE_OUTPUT            | 输出值                         |
| VALUE_INOUT             | 输入输出值                       |
| TMEM_INPUT/OUTPUT/INOUT | Temporary Memory Reference  |
| RMEM_INPUT/OUTPUT/INOUT | Registered Memory Reference |
| FMEM_INPUT/OUTPUT/INOUT | FF-A Memory Reference       |
| META                    | Kernel 消费的元数据               |
| NONCONTIG               | 使用非连续内存描述                   |

注意，`FMEM` 和 `RMEM` 在 ABI 中存在属性编码复用，但引用对象的解释依赖 Transport。不能只根据 `attr` 的数值判断内存来自哪里。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## **13.4.3 Value Parameter 与 Memory Reference 的根本区别**

先看 Value：

```
struct optee_msg_param_value {
    uint64_t a;
    uint64_t b;
    uint64_t c;
};
```

一个 Value Parameter 的核心工作是传递数值。

例如：

```
a = Algorithm ID
b = Operation Flags
c = Reserved
```

Secure Kernel 需要验证这些值是否符合业务语义，但不需要为每个数值建立单独的内存映射。

Memory Reference 则完全不同。

例如：

```
struct optee_msg_param_rmem {
    uint64_t offs;
    uint64_t size;
    uint64_t shm_ref;
};
```

这三个字段表达的是：

> 从一个已经注册的 Shared Memory Object 中，引用某个 Offset 开始、Size 长度的区域。

这实际上是一个对象引用，而不仅仅是指针。

### **为什么 Registered Memory 比裸指针更适合安全系统？**

假设 Normal World 直接传递：

```
Physical Address = 0x80000000
Size             = 4096
```

OP-TEE 必须确认：

- 这个地址是否属于允许访问的内存？
- 它是否指向 Secure Kernel Memory？
- 是否跨越了非法内存范围？
- 物理页面是否连续？
- 是否存在错误的 Cache Attribute？
- 访问期间页面能否被释放或重新分配？

如果使用已经注册的 MOBJ，则可以将内存注册与实际使用分离。

```
Registration Phase
    |
    v
Validate Shared Memory
    |
    v
Create MOBJ
    |
    v
Assign Cookie
    |
    v
Use Phase
    |
    v
Lookup Cookie
    |
    v
Validate Offset + Size
    |
    v
Reference / Map / Access
```

但这并不意味着 Registered Memory 天然可信。

MOBJ 解决的是内存对象描述、引用和映射管理等问题；它不自动保证 Normal World 无法在使用期间修改 Buffer 的内容。

这个区别必须记住。

# **13.5 Standard Call 的入口与参数校验：真正的第一道防线**

我们来看 4.11.0 的实际实现。

主要源码：

- `core/arch/arm/kernel/thread_optee_smc.c`
- `core/mm/mobj.c`
- `core/tee/entry_std.c`

## **13.5.1 第一步：区分新调用与 RPC Resume**

先看 `thread_handle_std_smc()` 的关键逻辑。

以下代码为依据 4.11.0 源码整理的片段：

```
if (a0 == OPTEE_SMC_CALL_RETURN_FROM_RPC) {
    thread_resume_from_rpc(a3, a1, a2, a4, a5);
    rv = OPTEE_SMC_RETURN_ERESUME;
} else {
    thread_alloc_and_run(a0, a1, a2, a3, 0, 0);
    rv = OPTEE_SMC_RETURN_ETHREAD_LIMIT;
}
```

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

虽然只有几行，但里面隐藏着整个 OP-TEE 执行模型的关键。

**路径 A：新的 Standard Call**

调用：

```
thread_alloc_and_run(...)
```

尝试分配一个 Trusted Thread，并启动它。

**路径 B：RPC 返回**

调用：

```
thread_resume_from_rpc(...)
```

恢复此前已经挂起的 Trusted Thread，而不是重新分配一个。

为什么必须区别对待？

假设某个 TA 需要加载一个文件。

```
Trusted Thread #2
    |
    v
TA Loading
    |
    v
RPC: Load TA from REE
    |
    v
Thread #2 Suspended
```

当 Linux 完成文件读取后，OP-TEE 必须恢复 Thread #2，因为其中保存了未完成操作所需的执行状态。

如果错误地创建一个新 Thread，就会失去原来的调用栈、临时状态及关联资源。

这不是简单地“继续执行一次命令”，而是 **恢复一个被挂起的执行上下文**。

## **13.5.2 第二步：决定 Message Buffer 的解释方式**

4.11.0 中，`std_smc_entry()` 会处理几种入口：

```
switch (a0) {
case OPTEE_SMC_CALL_WITH_ARG:
    return std_entry_with_parg(...);

case OPTEE_SMC_CALL_WITH_RPC_ARG:
    return std_entry_with_parg(...);

case OPTEE_SMC_CALL_WITH_REGD_ARG:
    return std_entry_with_regd_arg(...);

default:
    return OPTEE_SMC_RETURN_EBADCMD;
}
```

简化示意，省略参数拆分和错误日志。实际代码见 `thread_optee_smc.c`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

这里出现一个很重要的 ABI 设计：

传统地址传递与 Registered SHM Cookie 传递可以共存。

| 调用方式                 | 传递的主要对象                  | 需要做什么               |
| -------------------- | ------------------------ | ------------------- |
| `CALL_WITH_ARG`      | Message Physical Address | 验证地址、区域及消息大小        |
| `CALL_WITH_RPC_ARG`  | Message PA 和随附 RPC 参数区   | 额外验证 RPC Arg Buffer |
| `CALL_WITH_REGD_ARG` | SHM Cookie + Offset      | 查找 MOBJ、建立映射并验证范围   |

Registered SHM 路径中的 `std_entry_with_regd_arg()` 大致遵循：

```
Cookie
  |
  v
mobj_reg_shm_get_by_cookie()
  |
  v
mobj_inc_map()
  |
  v
get_msg_arg()
  |
  v
call_entry_std()
  |
  v
mobj_dec_map()
  |
  v
mobj_put()
```

注意这里同时出现了 `mobj_inc_map()` 和 `mobj_put()`。

它们承担不同的资源管理职责。

- Object Reference：保证相关内存对象在使用期间不会被不正确地释放。
- Mapping Reference：保证所需映射处于可使用状态。

实际映射和引用计数的具体语义要结合 MOBJ 类型研究，不能把这两者视为同一个计数器。

## **13.5.3 第三步：验证 Message Buffer 的结构边界**

`get_msg_arg()` 的关键逻辑：

```
p = mobj_get_va(mobj, offset,
                sizeof(struct optee_msg_arg));

if (!p || !IS_ALIGNED_WITH_TYPE(
                  p, struct optee_msg_arg))
    return OPTEE_SMC_RETURN_EBADADDR;

*arg = p;

*num_params = READ_ONCE((*arg)->num_params);

if (*num_params > OPTEE_MSG_MAX_NUM_PARAMS)
    return OPTEE_SMC_RETURN_EBADADDR;

sz = OPTEE_MSG_GET_ARG_SIZE(*num_params);

if (!mobj_get_va(mobj, offset, sz))
    return OPTEE_SMC_RETURN_EBADADDR;
```

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

逐步理解：

**第一步：验证固定头部。**

必须先证明至少能安全访问一个 `struct optee_msg_arg`，才可以读取其中的 `num_params`。

**第二步：检查 Alignment。**

即使某个地址位于合法内存对象范围内，也不意味着它满足结构体访问所要求的 Alignment。

**第三步：读取&#x20;**`num_params`**。**

这里使用 `READ_ONCE()`，避免编译器对该字段读取进行不期望的优化或重复访问。

但是 `READ_ONCE()` 并不等同于把整份共享消息复制进可信私有内存。

**第四步：限制参数数量。**

`OPTEE_MSG_MAX_NUM_PARAMS` 提供上限，避免后续计算和映射超出协议规定的范围。

**第五步：验证完整消息长度。**

拥有合法的 Header 不代表拥有完整的 Parameter Array。

假设：

```
Message Buffer Size = 64 bytes

num_params = 4
```

只验证 32 字节 Header 显然不够。

必须验证：

```
sizeof(Header) + 4 * sizeof(Parameter)
```

代表的整个区域均可访问。

### **这里的本质是什么？**

Secure Kernel 不能因为一个指针来自 Linux Driver，就认为它可信。

即使 Linux Driver 本身通常是可信软件栈的一部分，从 TEE Threat Model 来看，Normal World 仍然可能被攻陷。

所以这些验证必须在 Secure World 内完成。

# **13.6 从&#x20;**`tee_entry_std()`**&#x20;到&#x20;**`OpenSession`**：逐层源码追踪**

现在我们已经成功定位并验证了 Message Buffer。

下一步就是进行 Service Dispatch。

## **13.6.1&#x20;**`tee_entry_std()`**&#x20;是什么？**

在 4.11.0 中：

```
TEE_Result __weak tee_entry_std(
    struct optee_msg_arg *arg,
    uint32_t num_params)
{
    return __tee_entry_std(arg, num_params);
}
```

这是一个可由平台覆盖的 Weak Symbol。

但它背后的设计意图不是允许平台随意改变通用服务语义。

源码还明确要求：如果覆盖 `tee_entry_std()`，仍应调用 `__tee_entry_std()`。

实际核心 Dispatcher 是：

```
TEE_Result __tee_entry_std(
    struct optee_msg_arg *arg,
    uint32_t num_params)
{
    TEE_Result res = TEE_SUCCESS;

    thread_set_foreign_intr(true);

    switch (arg->cmd) {
    case OPTEE_MSG_CMD_OPEN_SESSION:
        entry_open_session(arg, num_params);
        break;

    case OPTEE_MSG_CMD_CLOSE_SESSION:
        entry_close_session(arg, num_params);
        break;

    case OPTEE_MSG_CMD_INVOKE_COMMAND:
        entry_invoke_command(arg, num_params);
        break;

    case OPTEE_MSG_CMD_CANCEL:
        entry_cancel(arg, num_params);
        break;

    /* Other supported commands */

    default:
        res = TEE_ERROR_NOT_IMPLEMENTED;
    }

    return res;
}
```

按 4.11.0 源码缩减的核心逻辑。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

这里有两个值得注意的点。

第一，Dispatcher 在分发服务之前启用了 Foreign Interrupt。

这是因为 Standard Call 已经具备 Trusted Thread Context，可以在特定条件下允许 Foreign Interrupt 并执行挂起/恢复路径。

第二，Kernel Command Dispatch 本身并不意味着一定执行 User TA。

例如 REGISTER_SHM、CANCEL 或 Notification 等命令可能直接由 Kernel Service 处理。

因此：

**Standard Call 是执行模式的分类，并不等同于 User TA Invocation。**

## **13.6.2&#x20;**`entry_open_session()`**&#x20;的完整职责**

核心代码位于：

`core/tee/entry_std.c`

其主要过程如下：

```
entry_open_session()
    |
    v
get_open_session_meta()
    |
    +-- Extract Target TA UUID
    +-- Extract Client Identity
    +-- Validate Login Type
    |
    v
copy_in_params()
    |
    +-- Validate Parameter Attributes
    +-- Resolve MOBJ
    +-- Build tee_ta_param
    |
    v
tee_ta_open_session()
    |
    +-- Find / Create TA Context
    +-- Load TA if needed
    +-- Create Session
    +-- Execute TA Open Entry
    |
    v
copy_out_param()
    |
    v
cleanup_shm_refs()
    |
    v
arg->session    = sess_id
arg->ret        = res
arg->ret_origin = err_orig
```

这条执行链可在 4.11.0 的 `entry_open_session()` 中直接确认。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们选择其中三个关键环节深入分析。

### **A. 为什么 OpenSession 有两个 Meta Parameters？**

`get_open_session_meta()` 要求前两个参数具有：

```
OPTEE_MSG_ATTR_META |
OPTEE_MSG_ATTR_TYPE_VALUE_INPUT
```

第一个 Meta Parameter 主要提供目标 TA UUID。

第二个 Meta Parameter 提供 Client Identity 信息，例如 Login Class 和相关 UUID。

这些 Meta Parameters 是给 Secure Kernel 使用的，不会直接作为普通 TA Parameters 传递给 TA。

这体现了 **Control Metadata 与 Application Data 的分离**。

### **B. Client Identity 是否可信？**

这是一个非常值得 Staff Engineer 研究的问题。

假设 Normal World 传递：

```
Login Type = TEE_LOGIN_APPLICATION
Client UUID = X
```

OP-TEE 可以验证字段编码是否合法。

但是：

**验证一个身份字段符合 ABI，并不等于密码学意义上认证了该身份。**

如果 Linux Kernel 已被攻陷，那么攻击者可能可以伪造 Normal World 提供的 Client Identity 信息。

因此，对于强安全场景，不能把 Linux 提供的 Application UUID 自动当作不可伪造的应用身份凭证。

必须明确区分：

- Identity Representation：身份如何表示。
- Identity Validation：字段是否符合协议。
- Identity Authentication：身份是否通过可信机制得到认证。
- Authorization：该身份是否具有访问某项资源的权限。

这四个概念是完全不同的。

### **C. 为什么参数转换不是简单的&#x20;**`memcpy()`**？**

我们看看：

```
copy_in_params(...)
```

它负责把外部：

```
struct optee_msg_param
```

转换成内部：

```
struct tee_ta_param
```

对于普通 Value，主要完成数值读取与类型转换。

对于 Memory Reference，则可能调用：

```
set_tmem_param(...)
set_rmem_param(...)
set_fmem_param(...)
```

这些函数不仅解析结构，还需要确认引用的内存对象是否合法。

例如 Registered SHM 路径中有：

```
if (ADD_OVERFLOW(mem->offs, mem->size, &req_size) ||
    mem->mobj->size < req_size)
    return TEE_ERROR_SECURITY;
```

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1

这个条件非常有代表性。

如果只写：

```
if (offs + size > mobj->size)
    return ERROR;
```

可能发生无符号整数溢出。

例如：

```
offs = UINT64_MAX - 8
size = 64
```

相加后发生 Wraparound，结果可能变成很小的数。

检查 `offs + size` 是否小于 Buffer Size，在没有溢出检查的情况下是不充分的。

### **D. 为什么要保存 Parameter Attributes？**

源码中还有：

```
saved_attr[n] = READ_ONCE(params[n].attr);
```

此后使用 `saved_attr[]` 决定参数类型和资源清理方式。

这是一个非常重要的安全设计。

假设 Secure Kernel 进入时，某个参数为：

```
RMEM_INPUT
```

它获取了对应 MOBJ 的引用。

但在处理过程中，Normal World 修改了原始共享内存中的 `attr`：

```
RMEM_INPUT → VALUE_INPUT
```

如果退出时重新读取不可信 `attr` 来决定是否调用 `mobj_put()`，可能遗漏释放或者执行错误的清理逻辑。

保存已解释的属性有助于使资源清理遵循 Kernel 自己记录的类型，而不是重新依赖可能变化的外部字段。

但这里仍有进一步的安全问题：

**保存&#x20;**`attr`**&#x20;并不意味着整个 Message Buffer 都已经形成不可变快照。**

例如，`cmd`、`session`、Value 数据和其他共享字段仍需要按照各自的读取路径分析 TOCTOU 风险。

这是后面 Staff 面试题的重要方向。

## **13.6.3 接下来进入 TA Manager**

真正的 Session 创建发生于：

```
tee_ta_open_session(
    &err_orig,
    &sess_id,
    &tee_open_sessions,
    &uuid,
    &clnt_id,
    TEE_TIMEOUT_INFINITE,
    &param
);
```

这里有一个非常重要的参数：

```
&tee_open_sessions
```

它代表从 Normal World 发起的 Session 所归属的 Session List。

Session Manager 还需要处理 User TA、PTA、内部嵌套调用以及 Session Lifetime。

这些问题会在 Chapter 29 深入研究。

本章只需要掌握：

```
Entry Dispatcher
    |
    v
TA Manager
    |
    +-- Identity / UUID
    +-- Session List
    +-- TA Context
    +-- Lifetime
    +-- Error Origin
```

并且知道：**Entry Dispatcher 不应该重新实现 TA 生命周期管理。**

# **13.7 Trusted Thread、Entry Stack 与异常状态**

现在回到一个更底层的问题。

Normal World 发起 SMC 后，CPU 究竟在哪个 Stack 上执行？

为什么 Fast Call 和 Standard Call 的 Stack 不一样？

## **13.7.1 Entry Stack 和 Trusted Thread Stack 的职责**

可以先用一个逻辑模型理解：

Entry Stack

通常按 CPU 配置的临时入口栈

Fast Dispatch

Register Handling

Thread Selection

Return Preparation

Trusted Thread Stack

由一个 Trusted Thread 拥有

Kernel Service

TA Manager

RPC State

Suspend / Resume

逻辑用途示意。实际异常栈、Pager Stack、SP_EL0/SP_EL1 等细节依赖架构和编译配置。

这里最关键的区别在于生命周期。

Entry Stack 通常只需要支持短暂的入口处理。

Trusted Thread Stack 则需要支持完整 Kernel Call Chain。

而当 Thread 因 RPC 挂起时，Thread Stack 上保存的中间执行状态不能直接丢弃。

## **13.7.2 源码：一个 Trusted Thread 如何被分配？**

在 Arm 4.11.0 的 `thread.c` 中，`alloc_thread()` 的关键操作是：

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

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub

这里有几个关键性质：

- 通过 Global Lock 保护 Thread Allocation。
- 为新请求找到一个 Free Thread。
- 标记为 Active。
- 如果没有 Free Thread，则当前入口无法分配新 Thread。

`thread_handle_std_smc()` 在这种情况下可以返回：

```
OPTEE_SMC_RETURN_ETHREAD_LIMIT
```

这意味着请求尚未得到一个可以处理它的 Trusted Thread。

它不是 TA 返回的 `TEE_ERROR_BUSY`，也不是普通的业务错误。

## **13.7.3 初始化执行上下文**

在 `__thread_alloc_and_run()` 中：

```
thread = alloc_thread(flags);

if (!thread)
    return;

init_regs(thread, ...);

thread_resume(&thread->regs);
```

`init_regs()` 会初始化 Thread 需要的 PC、SP、寄存器参数和异常状态。

对于 Arm64，源码明确将新 Thread 的 SP 设置为：

```
thread->regs.sp = thread->stack_va_end;
```

然后通过架构相关的 `thread_resume()` 恢复/建立相应寄存器状态，进入指定的执行入口。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



从系统设计角度，这里发生了一次重要转换：

```
CPU-local Entry Context
          |
          v
Allocated Trusted Thread Context
          |
          v
Kernel Service Execution
```

**CPU 与 Trusted Thread 并不是同一个概念。**

CPU 是实际执行资源；Trusted Thread 是 OP-TEE 管理的逻辑执行上下文。

一个 Thread 在 RPC 挂起后，不应该被简单地理解成仍然永久占用某个 CPU。

但它会继续占有相应的 Trusted Thread 资源，直到恢复并完成或被正确清理。

## **13.7.4 为什么 Exception Mask 很重要？**

4.11.0 在 Standard Entry 中调用：

```
thread_set_foreign_intr(true);
```

这允许按照 OP-TEE 的 Foreign Interrupt 模型处理中断。

但不能把这理解成“所有中断都直接在当前 Thread Stack 上执行”。

Native Interrupt、Foreign Interrupt、异常入口、Thread Suspend 等各有独立机制。

在后续章节会进一步研究：

- 哪些 IRQ 在 Secure World 处理。
- 哪些 Foreign Interrupt 需要交还 Normal World。
- 什么时候必须 Mask Interrupt。
- 持有 Spinlock 时为什么不应该开放特定中断。
- RPC 返回前后如何恢复正确的 Exception State。

本章先建立一条重要不变量：

> **任何可能允许当前执行上下文被抢占、挂起或切换的操作，都必须与当前 Stack、锁状态、寄存器保存状态和中断状态保持一致。**

# **13.8 SMC 与 FF-A：相同的服务核心，不同的通信边界**

到这里，我们基本上讲完了传统 SMC ABI。

但 OP-TEE 4.11.0 不能只看 SMC。

FF-A 是整个架构演进中不可忽略的一部分。

## **13.8.1 两种不同的入口模型**

Traditional SMC

Linux OP-TEE Driver

SMC ABI

thread_optee_smc.c

FF-A Transport

FF-A Client Endpoint

FF-A / SPMC

thread_spmc.c

Common GP Message Dispatch

tee_entry_std() → TA Manager / Kernel Services

这是逻辑架构图，并不意味着所有 FF-A Direct Message 都会进入 `tee_entry_std()`。

FF-A 同时可以承载 Secure Partition 消息，这些消息可能通过其他 Endpoint Dispatch 路径处理。

## **13.8.2 FF-A 如何处理 Shared Memory？**

传统 SMC ABI 可以通过 Physical Address 或 Registered SHM Cookie 定位消息。

FF-A 引入了 Memory Handle 和明确的内存共享事务模型。

因此，相同的业务参数不能无条件套用同一种 Memory Reference 解释方式。

例如，4.11.0 的：

```
set_fmem_param(...)
```

通过：

```
mobj_ffa_get_by_cookie(...)
```

查找对应的 FF-A Memory Object。

它随后检查 Offset、Size 是否落在已经验证的 MOBJ 范围内。

这一点说明，OP-TEE 的内部核心服务可以共享一部分逻辑，但 Transport 的内存授权语义必须单独处理。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## **13.8.3 4.11.0 的真实代码关系**

`thread_spmc.c` 中的 GP 消息处理路径也能够调用：

```
tee_entry_std(arg, num_params);
```

在进入该函数前，FF-A 路径负责取得对应 MOBJ、建立映射、验证消息大小以及准备 RPC 相关状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这正是前面提到的 Architecture Separation：

```
Different Transport
        |
        v
Transport-specific Validation
        |
        v
Common Internal Message Representation
        |
        v
Common Service Dispatcher
```

### **一个重要的设计风险**

假设未来开发者将 SMC Path 中的：

```
set_rmem_param(...)
```

改成能够接受更多种类型的物理内存。

但没有检查 FF-A Path 的授权与引用语义。

那么即使 SMC 功能测试全部通过，也不能证明 FF-A Path 同样安全。

**通用消息 ABI 并不代表所有 Transport 的内存安全前提天然相同。**

这也是跨架构 Porting 和 Core Code Review 必须重视的问题。

# **13.9 Return Path：为什么有两套错误返回机制？**

这是最容易被忽略，但又非常重要的部分。

OP-TEE 的调用结果至少需要区分两个层次。

## **13.9.1 Transport-Level Return**

例如：

```
OPTEE_SMC_RETURN_OK
OPTEE_SMC_RETURN_EBADADDR
OPTEE_SMC_RETURN_ETHREAD_LIMIT
OPTEE_SMC_RETURN_EBADCMD
```

这些结果说明的是外层 SMC 请求是否能够被处理、是否成功完成传输，以及是否需要重试或处理特殊状态。

还有一种特殊返回：

```
OPTEE_SMC_RETURN_IS_RPC(...)
```

表示 Secure World 请求 Normal World 执行 RPC 服务。

此时整个 TEE 业务操作并不一定已经完成。

## **13.9.2 Service-Level Return**

当外层执行正常完成时，业务结果通常写在：

```
arg->ret
arg->ret_origin
```

例如：

```
arg->ret = TEE_ERROR_BAD_PARAMETERS;
arg->ret_origin = TEE_ORIGIN_TEE;
```

或者：

```
arg->ret = TEE_SUCCESS;
arg->ret_origin = TEE_ORIGIN_TRUSTED_APP;
```

这里的 `ret_origin` 应根据实际执行来源设置，不应假定成功调用都必须具有某个固定 Origin。

### **一个具体例子**

假设：

```
OPTEE_SMC_RETURN_OK
```

但：

```
arg->ret = TEE_ERROR_ITEM_NOT_FOUND
```

这并不矛盾。

它意味着外层调用已经正常返回，但内部服务没有找到所请求的对象。

再例如：

```
OPTEE_SMC_RETURN_ETHREAD_LIMIT
```

此时 Standard Call 可能连 Trusted Thread 都未能分配成功，自然不能把它当成 TA 已执行并返回了某个业务错误。

## **13.9.3 异常返回矩阵**

| 失败位置                | 典型表现                     | 应该如何理解              |
| ------------------- | ------------------------ | ------------------- |
| SMC Function ID 不支持 | Transport Error          | 外层入口无法识别            |
| Message PA 非法       | `EBADADDR`               | 还未完成合法消息构建          |
| Trusted Thread 不足   | `ETHREAD_LIMIT`          | 需要适当的重试/等待策略        |
| Kernel Command 不支持  | `EBADCMD` 等外层错误          | Command Dispatch 失败 |
| TA UUID 不存在         | `arg->ret` 为相关 TEE Error | 服务内部结果              |
| TA Panic            | 相关 TA Error 和 Origin     | TA 执行失败             |
| RPC 请求              | RPC Return Encoding      | 当前服务尚未最终完成          |

需要特别注意：在 `entry_std.c` 中，部分 Command Handler 会自行设置 `arg->ret`，而 `__tee_entry_std()` 的返回值又用于外层判断。

例如 `call_entry_std()` 会根据 `tee_entry_std()` 的返回值决定最终的 SMC Return Code。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+2



所以修改错误处理时必须确认：

**当前错误应该属于 Transport Layer，还是应该属于 Service Layer？**

如果将本来属于 Service-Level 的异常错误直接变成 Transport Failure，Linux Driver 可能无法按照预期处理业务结果。

# **13.10 Entry Security：把 Normal World 当成恶意输入源**

现在进入本章最有价值的安全分析。

我们不再把 `entry_std.c` 看成一个普通 Dispatcher，而是把它当成 **OP-TEE Secure Kernel 的公共攻击面**。

## **13.10.1 建立 Threat Model**

假设攻击者拥有以下能力：

- 控制一个 Normal World 进程。
- 或者已经取得 Linux Kernel 权限。
- 可以提交格式错误的 OP-TEE Message。
- 可以伪造 Shared Memory 内容。
- 可以并发地修改 Normal World Memory。
- 可以反复发起合法和非法请求。
- 可以故意不完成 RPC。
- 可以在各种中间状态下触发 Timeout、Cancellation 或异常。

但不应默认攻击者已经可以直接访问 Secure Kernel 内存。

我们的目标是：

**即使 Normal World 完全不可信，也不能通过 Entry 接口破坏 Secure Kernel 的隔离性、资源安全性和内部状态一致性。**

## **13.10.2 最容易出现的五类问题**

### **问题一：TOCTOU**

```
Secure Kernel
     |
     v
Read shared field: size = 4096
     |
     | Normal World changes size
     v
Use shared field: size = UINT64_MAX
```

如果验证和使用依赖不同时间读取的共享字段，就可能发生 TOCTOU。

防御思路：

- 尽可能将安全决策所需字段保存到可信局部变量。
- 后续操作使用经过验证的值。
- 不要在验证后重新读取不可信字段以决定资源大小、权限或释放行为。
- 对无法冻结的共享数据，设计明确的访问协议和安全边界。

但要特别注意：使用 `READ_ONCE()` 只是其中一个手段，不能把它等同于完整的 TOCTOU 防护。

### **问题二：Integer Overflow**

例如：

```
offset + size
```

或者：

```
sizeof(header) + count * sizeof(param)
```

防御时必须考虑：

- Addition Overflow。
- Multiplication Overflow。
- 32-bit 与 64-bit 的转换。
- `size_t`、`paddr_t` 和 `uint64_t` 的宽度差异。
- Alignment 和 Page Boundary。

这类问题在 Arm32、Arm64 与 RV64 之间尤其需要注意。

### **问题三：Resource Exhaustion**

攻击者持续发起：

```
OpenSession
OpenSession
OpenSession
...
```

或者让多个请求停留在 RPC 等待状态。

即使没有内存越界，也可能耗尽：

- Trusted Thread。
- TA Session。
- MOBJ。
- Kernel Heap。
- RPC Buffer。

因此必须讨论：

- 资源限制。
- 失败后的释放。
- Backpressure。
- 正常请求与恶意请求的公平性。
- 拒绝服务与保密性、完整性之间的安全目标区别。

### **问题四：Session Confusion**

假设攻击者从另一个客户端获取一个 Session ID，然后尝试：

```
INVOKE_COMMAND(session = stolen_id)
```

Kernel 不能仅因为这个 ID 存在，就允许调用。

必须检查该 Session 是否处于正确的 Session List、是否可以被当前 Client 使用，以及相关调用状态是否合法。

但是 Session ID 校验所能提供的身份保证，还取决于 Transport 和 Normal World 身份模型。

它不是独立于系统的密码学认证机制。

### **问题五：错误清理路径**

假设：

```
copy_in_params()
    |
    +-- Param 0: MOBJ Reference Acquired
    |
    +-- Param 1: MOBJ Reference Acquired
    |
    +-- Param 2: Invalid
    |
    v
Return Error
```

如果直接返回而不清理 Param 0、Param 1 获得的资源，就可能泄漏 MOBJ Reference。

反复触发后可能造成 Secure Kernel 资源耗尽。

如果错误地释放从未获得的 Reference，则可能造成更严重的内存安全问题。

因此每一个 `goto cleanup` 都应该与资源获取状态相对应。

这个思维方式对审查 OP-TEE Core Patch 非常重要。

# **13.11 综合源码实验：追踪一次完整的 OpenSession**

下面给出一套适合在 QEMU + GDB 中完成的实验。

目标不是仅仅看到 `TEEC_OpenSession()` 返回成功，而是证明整条调用链中的核心状态变化。

## **13.11.1 实验目标**

重点观察：

1. SMC Function ID。
2. `optee_msg_arg`。
3. Trusted Thread ID。
4. TA UUID 与 Client Identity。
5. Parameter Conversion。
6. TA Session ID。
7. `ret` 与 `ret_origin`。

建议使用支持调试的 OP-TEE 构建，保留 Core ELF 符号，并确保 GDB 已连接到能够观察 Secure World 的模拟器或调试接口。

## **13.11.2 GDB 关键断点**

```
set pagination off
set print pretty on

break thread_handle_std_smc
break std_smc_entry
break get_msg_arg
break call_entry_std
break __tee_entry_std
break entry_open_session
break tee_ta_open_session
```

注意：

- 部分函数是 `static`，必须依赖正确的调试符号。
- 优化编译可能造成函数内联、参数不可见。
- 部分入口函数属于 C 与 Assembly 的协作路径。
- Linux 与 OP-TEE 应分别加载对应的符号信息。

在 `entry_open_session()` 中可以重点观察：

```
print *arg
print arg->cmd
print arg->num_params
print arg->params[0]
print arg->params[1]
print arg->ret
print arg->ret_origin
```

然后分别在 `tee_ta_open_session()` 前后观察：

```
Session ID
TA UUID
Client Identity
Result
Error Origin
```

## **13.11.3 必须分析的三个实验结果**

**实验 A：TA 已加载。**

观察 OpenSession 是否复用已有 TA Context，以及 Session 是否重新创建。

**实验 B：TA 尚未加载。**

观察是否进入 Loader/RPC 路径，尤其是因 REE FS TA 加载而挂起的 Trusted Thread。

**实验 C：非法参数。**

修改输入参数的类型或范围，观察拒绝发生在：

- Message Buffer Validation；
- Parameter Conversion；
- TA Session Manager；
- 还是 User TA 自身。

这四个失败位置有不同的安全意义。

# **13.12 代码修改影响分析**

假设我们需要扩展 `entry_std.c`，新增一个 Kernel Command。

真正的工作绝不是简单增加：

```
case OPTEE_MSG_CMD_MY_SERVICE:
    entry_my_service(arg, num_params);
    break;
```

需要评估：

| 影响范围                 | 必须回答的问题                                            |
| -------------------- | -------------------------------------------------- |
| Message ABI          | 是否增加 Command ID、Parameter Type 或字段？                |
| Linux Driver         | 是否需要配套支持？旧 Driver 如何表现？                            |
| FF-A                 | 新 Command 是否允许通过 FF-A Transport 进入？                |
| Parameter Validation | 参数类型、长度、Overflow 是否得到验证？                           |
| MOBJ                 | 是否引用 Shared Memory？如何保证生命周期？                       |
| Thread               | 会不会 Block、RPC、Sleep？                               |
| Lock                 | 是否可能与现有 Session、Driver 锁形成循环依赖？                    |
| Session / Identity   | 是否需要验证调用方身份和权限？                                    |
| Error Handling       | 返回 Transport Error 还是 TEE Error？                   |
| Cleanup              | 中途失败时如何释放所有资源？                                     |
| Compatibility        | 旧版 Kernel/Driver 是否仍然能协作？                          |
| Testing              | 包含哪些 Negative、Concurrency 和 Fault Injection Tests？ |

这是我们这套 OP-TEE OS Internals 课程最希望建立的工程能力。

# **13.13 Chapter 13 核心知识总结**

本章应该真正掌握的不是某几个函数名，而是五个相互关联的设计原则。

**第一，入口和服务必须解耦。**

SMC/FF-A 处理请求怎么进来，Kernel Dispatcher 处理请求做什么。

**第二，执行上下文决定可执行操作的边界。**

Fast Handler 不能任意使用可能睡眠、等待或 RPC 的服务。

**第三，Message ABI 是跨安全边界的协议。**

每个字段都应当按照不可信输入来分析。Parameter Parsing 本身就是 Security-Critical Code。

**第四，Trusted Thread 和 Shared Memory 都需要严格的生命周期管理。**

调用成功、失败、RPC、Cancellation 等路径不能破坏资源所有权。

**第五，错误返回必须保留分层语义。**

Transport Failure 和 Service Failure 是不同的问题，Linux Driver 的恢复行为也可能不同。

# **13.14 Staff Engineer Interview & Architecture Assessment**

以下 10 道题的目标，不是考察是否记住 `thread_handle_std_smc()` 或 `tee_entry_std()` 的函数名称。

它们考察的是：**当面对未知条件、不完整的信息和复杂的系统约束时，能否从底层机制出发建立正确的推理模型，并设计出可验证的解决方案。**

每道题都附有参考分析方向，但并不认为只有唯一答案。部分题目故意保留多个合法方案，要求说明取舍。

## **Question 1 — 设计一个全新的 RISC-V TEE Entry Transport**

Architecture Design · 难度 5/5

假设你正在设计 RISC-V TEE 系统。

系统包含：

- OpenSBI。
- Secure Domain Switch。
- OP-TEE Secure Kernel。
- Linux Client Driver。
- Hardware Memory Isolation。

你需要提供与传统 OP-TEE SMC ABI 语义相当的：

```
OpenSession
InvokeCommand
CloseSession
RPC
Cancel
```

但新的 RISC-V Transport 并不使用 Arm SMC。

**问题：**

你会如何设计 Entry Transport、消息格式、Thread Context 和 RPC Resume 机制，使上游 `entry_std.c`、TA Manager 和大部分 Kernel Service 无需修改？

进一步考虑：

- 如何验证发起请求的 Domain？
- 如何证明 Shared Memory 符合安全约束？
- RPC Resume Token 如何防止错误或恶意恢复？
- 多 Hart 同时执行时如何维护 Thread Ownership？
- 如果原始调用 Hart 不再可用，是否允许在另一个 Hart Resume？

**考核重点：** Architecture Separation、Resume Context Ownership、Memory Authorization、SMP、跨架构兼容性。

优秀答案不仅要给出调用图，还要解释哪些语义必须由 Firmware/Hardware 保证，哪些由 OP-TEE 自己维护。

## **Question 2 — 一个 Fast Call 在量产系统中偶发卡死**

Concurrency Debug · 难度 5/5

某工程师新增了一个 Fast Call，用于读取 Secure Hardware 状态。

在 QEMU 上一切正常，但在真实 SoC 上，高并发压力测试时偶尔永久卡死。

代码逻辑：

```
uint32_t my_fast_handler(void)
{
    return secure_hw_read_status();
}
```

其中 `secure_hw_read_status()` 会根据设备状态等待一次 Interrupt Completion。

**问题：**

你会如何定位这个问题？

要求考虑：

- Fast Call Context。
- Interrupt Mask。
- Spinlock。
- Clock/Power State。
- Device Completion。
- 多核竞态。
- QEMU 与真实硬件的差异。

**参考分析方向：** 从 Execution Contract 入手，而不是首先怀疑硬件。需要证明完成事件能否在当前中断状态下发生；如果等待操作具有阻塞语义，应评估改为 Standard/Yielding Call，或者重新设计为安全的短操作/异步状态机。

## **Question 3 — 共享内存 TOCTOU 是否可能绕过所有边界检查？**

Security Analysis · 难度 5/5

某个 Kernel Entry Handler 使用：

```
size = READ_ONCE(arg->params[0].u.rmem.size);

if (size <= MAX_SIZE)
    do_operation(arg->params[0].u.rmem.size);
```

攻击者能够在另一个 CPU 上修改 `arg->params[0].u.rmem.size`。

**问题：**

- `READ_ONCE()` 为什么不足以保证安全？
- 如果修改成局部变量 `do_operation(size)`，就一定安全吗？
- 如果 Buffer 是 Registered SHM，是否意味着 Buffer Content 也得到保护？
- 如何审查整个调用链中所有再次读取共享字段的位置？
- 如果硬件无法阻止 Normal World 并发修改共享内存，应该设计什么协议？

**参考分析方向：** 区分字段快照、对象生命周期稳定性、内存内容稳定性、数据权限和业务授权。避免把一次越界检查当作全部安全证明。

## **Question 4 — Trusted Thread Exhaustion：安全还是可用性问题？**

System Reliability · 难度 5/5

一个系统设置：

```
CFG_NUM_THREADS = 4
```

四个 Linux Task 几乎同时发起 OP-TEE 请求，四个 Trusted Thread 都因为等待 Normal World RPC 而被挂起。

此时，第五个 Linux Task 需要调用一个对系统启动恢复非常重要的 TA。

但是系统已经没有空闲 Trusted Thread。

**问题：**

- 为什么 CPU 可能空闲，但 OP-TEE Thread 仍然耗尽？
- `ETHREAD_LIMIT` 应由谁处理？
- 增加 `CFG_NUM_THREADS` 能否从根本上解决？
- 如何设计 Backpressure、RPC Completion 和 Retry？
- 能否为高优先级服务预留执行能力？这会带来什么复杂性？
- 如果 Normal World 恶意不完成 RPC，OP-TEE 能否保证关键服务一定可用？

**参考分析方向：** 区分 CPU Scheduling、Thread Resource Accounting 和 Service Progress Guarantee。应该明确可信和不可信组件之间无法无条件保证的 Liveness。

## **Question 5 — 修改&#x20;**`optee_msg_arg`**&#x20;导致旧版 Linux 无法启动**

ABI Evolution · 难度 5/5

你决定在 `struct optee_msg_arg` 的 `session` 字段之后增加：

```
uint64_t security_domain_id;
```

新 OP-TEE OS 与新 Linux Driver 测试通过。

但是使用旧版 Linux Driver 时，Kernel 无法正确解析请求。

**问题：**

- 从 C Structure Layout 角度解释原因。
- 为什么不能因为两边都是同一种 Architecture，就认为 ABI 兼容？
- 应该优先新增 Command、Extension Message，还是修改原始 Header？
- 如何设计 Capability Negotiation？
- 如何支持 Rolling Upgrade？
- 新旧版本不匹配时，如何保证安全失败而非错误解释字段？

**参考分析方向：** Binary Layout、Version Negotiation、Forward/Backward Compatibility、Unknown Field Handling、Fail-Closed Semantics。

## **Question 6 — 一个 Session ID 被另一个 Linux Process 使用**

Security Boundary · 难度 5/5

Process A 打开一个包含敏感业务的 TA Session。

Process B 获得了 Session ID，并试图构造自己的 OP-TEE Message：

```
cmd     = INVOKE_COMMAND
session = A's Session ID
func    = READ_SECRET
```

**问题：**

- Session ID 应该被视为 Pointer、Handle、Capability 还是 Identifier？
- Linux Driver 与 OP-TEE 分别负责什么隔离？
- 如果 Linux Kernel 被攻陷，是否还能够保证 Process A 和 Process B 的区分？
- 若要求强隔离，是否需要独立的硬件 Identity、VM ID 或其他可信身份依据？
- Session Ownership 与 TA 内部 Authorization 应如何配合？
- 如何防止旧 Session ID 被重用后引发 ABA 类问题？

**参考分析方向：** Identity Authenticity、Session Namespace、Capability Security、Threat Model、资源生命周期。

## **Question 7 — RPC Resume Token 伪造攻击**

Context Integrity · 难度 5/5

某个 Trusted Thread 正在执行：

```
TA → Storage → RPC → Linux
```

RPC 期间，Normal World 保存了 Resume 所需的返回寄存器和 Token。

攻击者尝试：

- 重复发送某个旧 Resume Token。
- 使用 Thread A 的 Token 恢复 Thread B。
- 在原来的 RPC 尚未完成时提前 Resume。
- 改变 RPC Response 内容。

**问题：**

一个安全的 Resume 机制应该满足哪些不变量？

进一步考虑：

- Thread State。
- Token Validity。
- Resume Generation。
- CPU/Hart Migration。
- Shared Memory Lifetime。
- RPC Result Validation。
- Cancellation 和 CloseSession Race。

**参考分析方向：** 必须将 Context Resume 和 RPC Response Validation 分开。正确恢复了 Thread，并不代表 RPC 返回数据可信。

优秀答案应建立明确的状态机，指出每个状态允许哪些 Transition。

## **Question 8 — FF-A 和 SMC 复用 Kernel Dispatcher 是否一定安全？**

Cross-Transport Review · 难度 5/5

开发者认为：

> `tee_entry_std()` 已经支持 OpenSession，因此只要 FF-A Transport 能构造 `optee_msg_arg`，就可以直接复用整个服务实现。

**问题：**

你会要求其证明哪些前置条件？

重点分析：

- Memory Share/Lend 的授权语义。
- Endpoint Identity。
- MOBJ Lifetime。
- Buffer Mapping。
- Message Ownership。
- FF-A 与 SMC 的 Error Reporting。
- RPC 和 Notification。
- 失效的内存引用如何撤销。
- 未来引入 Secure Partition 时的权限边界。

**参考分析方向：** ABI Compatibility 并不等于 Security Equivalence。需要形成一份 Transport Contract，并通过两种 Transport 的实际测试验证。

## **Question 9 — 如何在不查看代码的情况下推断一个入口漏洞的可能影响？**

Impact Analysis · 难度 5/5

安全团队报告：

> OP-TEE 的一个 Memory Reference 解析函数可能存在 Integer Overflow。

你只知道漏洞位于 `entry_std.c` 的参数处理路径，其他信息暂时未知。

**问题：**

在尚未看到具体 Patch 前，你会如何建立影响范围假设？

要求覆盖：

- `OPEN_SESSION`。
- `INVOKE_COMMAND`。
- Shared Memory Registration。
- MOBJ。
- TA VM。
- RPC。
- PTA。
- FF-A。
- Arm32 / Arm64 / RISC-V。
- 不同 `CFG_*` 组合。

随后，当你拿到实际代码时，你会按照什么顺序验证这些假设？

**参考分析方向：** 应区分 Reachability、Exploitability、Security Impact 和 Regression Impact。不能因为一个函数在某个模块中，就推断全部子系统一定可利用。

## **Question 10 — 从零设计一个可供社区接受的 Kernel Entry Extension**

Principal/Staff Architecture · 难度 5/5

假设某个 SoC 平台希望新增一项安全服务：

```
SECURE_RESOURCE_QUERY
```

它要向 Normal World 返回系统中部分安全资源的状态。

但存在以下要求：

- 只允许特定调用方访问。
- 部分查询需要访问硬件。
- 部分查询可能需要等待。
- 部分数据涉及安全资源布局，不能泄露。
- 需要支持多个 CPU 并发。
- 需要考虑 SMC 和 FF-A。
- 未来希望将代码 Upstream。

请从 Architecture Review 的角度提交完整设计。

至少回答：

1. 应使用 Fast Call、Standard Call、PTA 还是已有 TA 服务？
2. 是否需要扩展现有 Message ABI？
3. 如何进行身份认证和授权？
4. 是否需要创建新的 Internal Kernel Abstraction？
5. 如何访问 MMIO？
6. 如何处理锁、时钟、电源、等待和超时？
7. 是否需要新的 Driver Interface？
8. 如何在不支持硬件能力的平台上运行？
9. 如何支持不同 Architecture Backend？
10. 如何定义 Security Invariants？
11. 如何设计错误返回和异常清理？
12. 应编写哪些测试证明它可以安全 Upstream？

**考核重点：** 是否能够做出合理的架构取舍，控制 TCB 扩张，并证明设计与 OP-TEE 现有执行模型和安全边界一致。

## **最终考核建议**

这十道题建议不依赖 AI，首先独立进行架构推演。重点不是说出某个函数名称，而是提供：

- 一张准确的架构或时序图。
- 一组明确的前置条件与安全不变量。
- 两个以上可行方案及其权衡。
- 至少一个失败场景和恢复路径。
- 一套可以复现并验证设计假设的实验。

对于 Staff Engineer，真正有区分度的不是“知道正确答案”，而是能够解释 **为什么某个方案在一组条件下成立，但在另一组条件下可能不安全**。

# **Chapter 13 — 结束语**

本章最应该保留下来的认知是：

**OP-TEE Entry 不是一个简单的函数分发器，而是整个 Trusted OS 的安全入口与执行上下文转换中心。**

从一个 Normal World 请求进入 Secure Kernel 开始，系统就必须同时保证：

- 请求符合 Transport Contract。
- 执行上下文合法。
- 不可信参数经过适当验证。
- Memory Object 具有正确的生命周期。
- Session 与调用方身份关系正确。
- 锁、中断、Stack 和 RPC 状态相互一致。
- 错误路径不会破坏内部状态。
- 返回 Normal World 时不泄露可信执行状态。

理解这些之后，再研究 Chapter 14 的 Trusted Thread，你将不只是知道 OP-TEE 如何分配线程，而是可以进一步回答：

**为什么 OP-TEE 需要这种特殊的 Thread 模型？它如何支持 RPC、Interrupt、SMP 和 Secure/Normal World 之间的协作？**

这也是我们接下来深入 OS Internals 最重要的基础。

**源码参考：**

- [thread_optee_smc.c — SMC Entry & RPC](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread_optee_smc.c?utm_source=chatgpt.com)
- [entry_std.c — Standard Command Dispatch](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/tee/entry_std.c?utm_source=chatgpt.com)
- [entry_fast.c — Fast Call Dispatch](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/tee/entry_fast.c?utm_source=chatgpt.com)
- [optee_msg.h — Message ABI](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/include/optee_msg.h?utm_source=chatgpt.com)
- [thread_spmc.c — FF-A / SPMC](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/thread_spmc.c?utm_source=chatgpt.com)
- [OP-TEE Core Architecture — 官方文档](https://optee.readthedocs.io/en/4.11.0/architecture/core.html?utm_source=chatgpt.com)

