# Chapter 14 — Shared Memory & RPC Architecture

RISC-V OP-TEE Architecture & Porting · 深度技术讲义 · Staff Engineer / Upstream Maintainer Level

本章主题：Linux 与 OP-TEE 如何安全、高效地交换内存数据，以及 OP-TEE 为什么能够在执行过程中主动返回 Linux 请求服务，再恢复原来的 Trusted Thread。

这一章是理解 OP-TEE 内部工作机制的关键章节，也是 RISC-V OP-TEE 从“能够启动”发展到“能够完整执行 TA、访问 REE 文件系统、动态加载 TA”的重要基础。

我们将按照上一版课程大纲，完整覆盖：

1. Shared Memory 的必要性与设计原理
2. OP-TEE Message ABI
3. Static Shared Memory 与 Dynamic Shared Memory
4. RPMI Memory Parcel 与共享内存生命周期
5. Linux `tee_shm` 与 OP-TEE `mobj`
6. RPC 的设计原因与执行模型
7. `tee-supplicant` 的职责
8. RPC 返回后的 Trusted Thread 恢复
9. Cache Coherency、DMA 与 Shared Memory
10. TOCTOU、Double Fetch 与非可信参数验证

最后增加源码阅读路径、可操作实验、典型工业故障分析以及 Staff Engineer 级面试题和参考答案。

## 本章学习目标

完成本章后，你应该能够独立解释并验证：

- 为什么 Shared Memory 是 OP-TEE 的基本通信机制，而不是一个可选的性能优化。
- 一个 Linux User VA 如何被转换成 OP-TEE 可识别、可映射的内存对象。
- `struct tee_shm`、`struct optee_msg_param` 和 `struct mobj` 分别代表什么。
- 为什么 Shared Memory 的物理地址有效，不代表其访问安全。
- 一次 RPC 为什么能够跨越 Linux、M-mode、OP-TEE，再恢复到原来的 C 函数调用位置。
- 为什么“Linux root 无法读取 OP-TEE Private RAM”不等于“Linux 不能攻击 OP-TEE”。
- RISC-V RPMI Memory Parcel 的生命周期如何与传统 OP-TEE Dynamic SHM 对接。
- 如何排查多 hart、非一致性缓存、共享内存回收及并发访问造成的系统问题。

## 0. 先建立整章的统一架构模型

### 0.1 OP-TEE 的通信不是一次简单的函数调用

先看一个典型的 CA 请求：

```
TEEC_InvokeCommand(&session, cmd_id, &operation, &origin);
```

在应用层看来，它与普通的同步函数调用差别不大。

但真正执行时，可能跨越多个不同的执行环境：

```
                   Linux / REE
   ┌────────────────────────────────────┐
   │ Client Application                 │
   │                                    │
   │ TEEC_InvokeCommand()               │
   │        │                           │
   │        v                           │
   │ libteec / Linux TEE Subsystem      │
   │        │                           │
   │        v                           │
   │ Linux OP-TEE Driver                │
   │   tee_shm + optee_msg_arg          │
   └────────┬───────────────────────────┘
            │
            │ Shared Memory + Call
            │
   ┌────────v───────────────────────────┐
   │ M-mode Firmware / Domain Manager   │
   │ OpenSBI / RPMI TEE Framework       │
   └────────┬───────────────────────────┘
            │
            │ Enter TEE Domain
            v
   ┌────────────────────────────────────┐
   │ OP-TEE Core (S-mode)               │
   │                                    │
   │ Resolve Message Parameters         │
   │       │                            │
   │       v                            │
   │ Allocate/Resume Trusted Thread     │
   │       │                            │
   │       v                            │
   │ User TA (U-mode)                   │
   │       │                            │
   │       v                            │
   │ OP-TEE Core Services               │
   └────────┬───────────────────────────┘
            │
            │ Optional RPC
            v
   ┌────────────────────────────────────┐
   │ Linux Kernel / tee-supplicant      │
   │ File I/O / TA Loading / etc.       │
   └────────────────────────────────────┘
```

这里有两种本质不同的数据：

Control Plane（控制平面）：请求哪个 TA、执行哪个命令、参数类型、Session ID、请求状态、RPC 返回状态。

Data Plane（数据平面）：TA 实际处理的数据，例如大块输入、加密结果、文件内容、TA 镜像等。

OP-TEE Message ABI 负责描述通信语义，而 Shared Memory 承载主要数据及部分控制结构。

RPMI、SMC、FF-A 等则属于通信传输或调用机制，不应与 OP-TEE 的消息语义混为一谈。

### 0.2 必须区分四种不同的“内存对象”

| 内存或抽象                 | 管理者                 | 主要用途             |
| --------------------- | ------------------- | ---------------- |
| REE User Buffer       | Linux / 用户进程        | CA 的输入输出         |
| Linux `tee_shm`       | Linux TEE Subsystem | 管理跨 TEE 共享的内存    |
| OP-TEE `mobj`         | OP-TEE Core         | 描述并访问底层内存对象      |
| Secure Private Memory | OP-TEE / 安全固件       | Core、TA 的私有代码和数据 |

核心原则：Shared Memory 是通信介质，不是可信内存。

即使已经通过 OpenSBI 和 PMP 建立 Linux/TEE Domain 隔离，Linux 仍然可能修改允许双方访问的 Shared Memory。

所以 OP-TEE 必须假设：

> Shared Memory 的内容可以被恶意 REE 任意修改，甚至可以在 OP-TEE 读取同一块内存的两次操作之间发生变化。

这个假设贯穿本章的参数解析、内存对象注册、RPC 和 TOCTOU 分析。

### 0.3 版本与实现边界

本章特别区分三个层次：

| 实现路径                         | 状态与定位                   |
| ---------------------------- | ----------------------- |
| Arm SMC + OP-TEE Message ABI | 成熟的主要参考实现，适合深入源码        |
| OP-TEE FF-A Backend          | 已有的另一套共享内存与调用模型         |
| RISC-V RPMI TEE / MPXY       | 2026 年仍在演进的 RISC-V 通信架构 |

尤其需要说明：2026 年 10 月 5 日的 Linux RPMI OP-TEE Backend RFC v2 已采用 Memory Parcel ID + Nonce、显式消息缓冲区范围和 Opaque Resume Token 的设计，但 RFC 明确指出与之匹配的 OP-TEE OS 实现尚未完成。

因此，本章不会把这个 RFC 方案描述为已经正式合入并具备完整端到端验证的通用实现。

[image](https://www.google.com/s2/favicons?domain=https://lists.infradead.org\&sz=32)

lists.infradead.org+1



# 14.1 为什么 REE / TEE 通信不能只依赖寄存器？

## 14.1.1 寄存器适合传递控制信息，不适合承载复杂数据

你已经熟悉 RISC-V ABI，可以先考虑一次普通的 SBI 调用：

```
li a7, SBI_EXT_ID
li a6, SBI_FUNC_ID

mv a0, input0
mv a1, input1

ecall
```

这很适合传递少量整数、地址或句柄。

但假设 CA 想让 TA 计算一个 4 MiB 数据块的 SHA-256：

```
CA
 |
 | 4 MiB input buffer
 v
TA
 |
 | SHA-256
 v
32-byte digest
```

显然不能把 4 MiB 的输入依次塞入 `a0`～`a7`。

更合理的办法是通过寄存器传递一份较小的请求描述，而真实的数据保存在内存中。

```
Registers:
    command / message reference

Shared Memory:
    Request Descriptor
       |
       +-- TA UUID / Session / Command
       |
       +-- Parameter Attributes
       |
       +-- Data Buffer Reference
       |
       +-- Buffer Length
```

这实际上体现了操作系统中非常经典的设计：

通过少量寄存器传递控制权，通过内存交换复杂数据。

Linux 系统调用、VirtIO、网卡描述符环、Hypervisor Shared Memory 都采用过类似思想。

## 14.1.2 为什么不直接把指针传给 OP-TEE？

假设 Linux 用户程序中有：

```
uint8_t buffer[4096];

TEEC_Operation op = { 0 };
op.params[0].tmpref.buffer = buffer;
op.params[0].tmpref.size = sizeof(buffer);
```

`buffer` 是 Linux 进程的虚拟地址。

它受到该进程页表、`satp`、ASID 等上下文约束。

OP-TEE 不一定使用相同的页表和地址空间。

因此：

```
Linux Process VA: 0x0000004000800000
              |
              | Linux page table
              v
Physical Page:    0x0000000082000000
              |
              | OP-TEE mapping
              v
OP-TEE VA:        0x00000000XXXXXXXX
```

即使双方最终访问同一组 Physical Pages，它们也不需要使用相同的 VA。

而且 Linux 用户进程中的 buffer 可能：

- 跨多个物理页；
- 在物理地址上不连续；
- 存在非零 Page Offset；
- 是可写的用户内存；
- 在调用期间需要 Pin / Reference；
- 在共享期结束后重新映射或释放。

因此，跨 TEE 的通信不能以一个普通 Linux VA 作为可信引用。

这里需要 Linux TEE Driver 和 OP-TEE 内存管理共同建立一个跨域内存对象。

## 14.1.3 Shared Memory 并不自动意味着 Zero-Copy

需要区分：

Registration：将现有内存注册为跨域可引用的对象。

Mapping：为接收方建立访问该物理内存的地址映射。

Copying：将数据复制到另一块内存。

Dynamic Shared Memory 能避免某些中间复制，但不保证从 CA 到 TA 的整个调用链都是 Zero-Copy。

例如：

```
CA Original Buffer
      |
      | optional copy
      v
Linux TEE Shared Buffer
      |
      | shared physical pages
      v
OP-TEE Mapping
      |
      | optional secure copy
      v
TA Private Buffer
```

最后一步可能是必要的：如果 TA 需要不可被 REE 并发修改的数据快照，那么拷贝到 Private Memory 可能正是正确的安全设计。

结论：Shared Memory 主要解决跨隔离域的数据交换与内存可达性问题；Zero-Copy 是在安全约束允许时可获得的性能收益。

# 14.2 OP-TEE Message ABI

## 14.2.1 为什么需要独立的 Message ABI？

我们需要区分两层：

```
Transport ABI:
    如何进入 TEE？
    怎样传递 Message 的位置？
    如何 Yield / Resume？

Message ABI:
    请求执行什么操作？
    参数类型是什么？
    数据在哪里？
    执行结果和错误码是什么？
```

经典 Arm SMC 路径使用 OP-TEE SMC Interface 进入安全世界，再通过 OP-TEE Message Protocol 描述具体请求。

在另一种传输机制中，我们可以尝试复用 Message 语义，但必须明确新传输如何表达内存引用、返回状态和 RPC。

这也是 RISC-V RPMI Backend 架构演进的主要问题之一。

## 14.2.2 `struct optee_msg_arg`

先看 OP-TEE 的核心消息结构。

下面是去掉注释的实际结构布局：

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

源码：`optee_os/core/include/optee_msg.h`。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



每个字段的意义如下：

| 字段           | 作用                                 |
| ------------ | ---------------------------------- |
| `cmd`        | OP-TEE 消息命令，例如 Invoke、Open Session |
| `func`       | TA 内部命令编号                          |
| `session`    | TA Session ID                      |
| `cancel_id`  | 取消请求的标识                            |
| `ret`        | 请求执行结果                             |
| `ret_origin` | 错误来源                               |
| `num_params` | 参数数量                               |
| `params[]`   | 变长参数数组                             |

需要注意，`cmd` 和 `func` 并不是同一个概念。

例如：

```
cmd  = OPTEE_MSG_CMD_INVOKE_COMMAND
func = TA_CMD_SHA256
```

含义是让 OP-TEE 对已经打开的 TA Session 执行一个特定命令。

## 14.2.3 `struct optee_msg_param`

实际消息参数结构大致如下：

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

这里的 `attr` 决定 Union 的解释方式。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



先理解三种最重要的参数表示。

Value Parameter

直接通过几个整数传递数据：

```
struct optee_msg_param_value {
    uint64_t a;
    uint64_t b;
    uint64_t c;
};
```

适合命令选项、长度、状态等小型参数。

TMEM — Temporary Memory Reference

用地址、大小和共享内存引用来表示一个临时缓冲区。

典型于传统连续内存传输路径。

RMEM — Registered Memory Reference

用已经注册的共享内存对象句柄，以及 Offset、Size 来指定数据范围。

其核心含义是：

```
Registered SHM Object
    |
    +-- Cookie / shm_ref
    |
    +-- Offset
    |
    +-- Size
```

而不是让 OP-TEE 每次都重新信任一个任意物理地址。

## 14.2.4 TMEM 与 RMEM 的关键区别

| 比较项        | TMEM         | RMEM                   |
| ---------- | ------------ | ---------------------- |
| 主要引用方式     | 地址、大小、共享内存引用 | 已注册对象句柄、偏移、大小          |
| 是否要求提前注册   | 取决于传输路径及使用方式 | 经典 SMC Dynamic SHM 中需要 |
| 是否可描述非连续内存 | 取决于协议标志与描述方式 | 通过注册对象间接引用             |
| 安全校验关注点    | 地址合法性、类型、大小  | 句柄有效性、对象范围、生命周期        |
| 典型用途       | 传统临时共享参数     | Dynamic Registered SHM |

并非所有 TMEM 都代表一块无需额外描述的连续内存；例如 `OPTEE_MSG_ATTR_NONCONTIG` 可以用于描述注册时的非连续页列表。

另一方面，已经注册的 RMEM 并不意味着内存内容不可改变，只意味着 OP-TEE 已建立了一份关于该内存对象的受控描述。

## 14.2.5 消息协议为什么必须验证参数数量？

`params[]` 是 Flexible Array Member。

如果 Linux 传入：

```
arg->num_params = 0xffffffff;
```

OP-TEE 绝不能直接相信这个值，然后访问对应数量的参数。

因为攻击者可能试图造成：

- 整数溢出；
- 越界读取；
- 非可信内存访问；
- Secure Kernel 内存破坏；
- 非法类型组合导致 Union 错误解释。

因此，Message Parser 必须把消息缓冲区的实际大小、`num_params`、参数类型和可访问范围一同纳入验证。

这里还有一个经常被忽略的细节：

不能只在第一次检查时验证长度，随后又重新读取可被 REE 改变的长度。

否则就可能产生 Double Fetch 漏洞。

## 14.2.6 用一个具体消息例子理解 ABI

假设一个 TA 执行 SHA-256：

```
Command:
    cmd = INVOKE_COMMAND
    func = CMD_SHA256
    session = 0x1234

Parameters:
    param[0]:
        type = RMEM_INPUT
        shm_ref = 0x100
        offs = 0x200
        size = 4096

    param[1]:
        type = RMEM_OUTPUT
        shm_ref = 0x101
        offs = 0
        size = 32
```

这是示意性的参数实例，不表示固定句柄值。

OP-TEE 处理时必须依次确认：

1. 当前 Session 是否存在且允许请求。
2. `param[0]` 的类型是否合法。
3. `shm_ref=0x100` 对应的共享内存对象是否存在。
4. `offs=0x200` 到 `offs+size` 是否位于对象范围内。
5. 输入缓冲区是否允许访问。
6. 输出对象是否足够容纳结果。
7. 相关对象在整个执行过程中是否仍然有效。

最后一项就是 Shared Memory Lifetime Management 的核心。

它将在 14.4 和 14.5 中展开。

# 14.3 Static Shared Memory 与 Dynamic Shared Memory

## 14.3.1 两种模型的根本区别

首先要区分两个概念：

Static SHM：在平台初始化阶段预留一块双方已知、通常物理连续的共享内存区域。

Dynamic SHM：Linux 根据实际请求分配或注册普通内存页，再通过协议使 OP-TEE 识别这些页。

这两种方式主要解决的是 共享内存的发现、组织和管理，而不是“哪一种更安全”。

Static SHM

预先定义物理区域，供共享缓冲区使用

Dynamic SHM

普通 DRAM 中的离散页，经注册后成为共享对象

OP-TEE 官方文档明确支持连续 Reserved Shared Memory 和非连续 Dynamic Shared Memory，平台至少需要提供其中一种。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 14.3.2 Static SHM 的初始化过程

经典 OP-TEE 配置中，可以通过：

```
CFG_CORE_RESERVED_SHM=y
CFG_SHMEM_START=<physical_base>
CFG_SHMEM_SIZE=<size>
```

定义预留共享内存。

OP-TEE Core 将其注册为 `MEM_AREA_NSEC_SHM`。

在传统 Arm SMC 接口下，Linux 可以使用 `OPTEE_SMC_GET_SHM_CONFIG` 查询预留区域的物理起始地址、大小和缓存属性。

例如：

```
Illustrative physical memory layout

0x80000000  +---------------------------+
            | Linux Normal RAM          |
            |                           |
0x88000000  +---------------------------+
            | Reserved Shared Memory    |
            | Size: 4 MiB               |
0x88400000  +---------------------------+
            | Linux Normal RAM          |
            |                           |
0x90000000  +---------------------------+
            | OP-TEE Private RAM        |
            +---------------------------+
```

这些地址仅用于说明，并非某个具体平台的实际布局。

Linux 可以在 Reserved SHM 中分配一个 Buffer：

```
Shared Pool:
    Base = 0x88000000
    Size = 4 MiB

Buffer:
    PA   = 0x88002000
    Size = 4096
```

因为整个物理区域已经被 OP-TEE 识别，所以请求通常不需要为每个 Buffer 再单独提交离散 Page List。

但是 OP-TEE 仍需要检查：

\\[ B \subseteq [SHM\_{start}, SHM\_{end}) \\]

准确地说，要验证请求范围完全落在有效 SHM 区域内，并防止地址加法溢出。

### Static SHM 的局限

最明显的缺点是预留内存规模固定。

如果仅有 4 MiB Reserved SHM，而系统同时运行多个大量传输数据的 TA，就可能产生内存压力。

此外，用户程序原本的数据一般位于 Linux 普通用户页中，可能需要复制到共享池。

所以 Static SHM 常见的问题是：

- 预留过少导致共享内存分配失败；
- 预留过多浪费可用 RAM；
- 中间数据复制影响吞吐量；
- 固定区域需要在固件、Linux 和 OP-TEE 之间保持一致。

但它的优点也很明显：物理区域确定，通常更容易完成早期 Bring-up 和调试。

## 14.3.3 Dynamic SHM 的核心思想

Dynamic SHM 允许 Linux 注册位于正常内存中的一组物理页。

配置入口：

```
CFG_CORE_DYN_SHM=y
```

需要特别注意：

Dynamic SHM 不等于 OP-TEE 可以接受任意物理地址。

OP-TEE 仍然需要获知哪些物理区域属于允许共享的 Non-secure Memory。

这些信息可由平台配置、Device Tree 的 Memory Node、`register_ddr()` 等机制提供。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



例如，Linux 用户 Buffer 的物理布局可能是：

```
Linux Virtual Address
0x40001000
    |
    +-- VA Page 0 -> PA 0x81005000
    +-- VA Page 1 -> PA 0x83022000
    +-- VA Page 2 -> PA 0x8110A000
    +-- VA Page 3 -> PA 0x85001000
```

虚拟地址连续，但物理地址完全不连续。

Linux 将这些页面整理为 Page List，交给 OP-TEE 解析。

OP-TEE 在内部建立 Memory Object：

```
MOBJ
 |
 +-- size
 +-- page_offset
 +-- cookie
 +-- pages[]
       |
       +-- 0x81005000
       +-- 0x83022000
       +-- 0x8110A000
       +-- 0x85001000
```

之后，OP-TEE 可以通过 MOBJ 抽象找到某个逻辑 Offset 对应的物理页，并在需要时建立 VA 映射。

## 14.3.4 为什么需要 Page List，而不是一个 PA？

因为 `PA + size` 只能准确描述物理连续范围。

如果物理页不连续，那么：

```
PA = 0x81005000
Size = 16 KiB
```

会错误地表示一个从 `0x81005000` 开始的连续 16 KiB 区域。

实际上需要表示四个独立的页。

对于经典 SMC 非连续共享内存格式，OP-TEE Message ABI 使用 4 KiB Granule，并定义非连续 Page List 的表示方式。

Linux Driver 在注册时构造 Page List，然后通过 `OPTEE_MSG_CMD_REGISTER_SHM` 发送给 OP-TEE。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



### 一个完整的 Dynamic SHM 生命周期

```
Linux Allocate / Register User Buffer
            |
            v
Pin / Track Memory Pages
            |
            v
Build Page List
            |
            v
OPTEE_MSG_CMD_REGISTER_SHM
            |
            v
OP-TEE validates page addresses
            |
            v
Create MOBJ and associate cookie
            |
            v
Invoke TA using RMEM reference
            |
            v
TA uses the registered buffer
            |
            v
OPTEE_MSG_CMD_UNREGISTER_SHM
            |
            v
Release MOBJ references
            |
            v
Linux releases backing memory
```

这是经典 SMC Dynamic SHM 的逻辑顺序。

具体 Linux 内存 Pinning、释放顺序和注册回调，要以所使用的 Kernel 版本为准。

## 14.3.5 Static SHM 与 Dynamic SHM 的工程选择

| 维度           | Static SHM | Dynamic SHM      |
| ------------ | ---------- | ---------------- |
| Buffer 来源    | 预留共享区域     | Linux 普通内存       |
| 物理连续性        | 经典模式要求连续   | 可以非连续            |
| 地址描述         | PA + Size  | Page List / 对象引用 |
| 运行时管理        | 较简单        | 更复杂              |
| 内存使用效率       | 依赖预留池大小    | 通常更灵活            |
| Zero-copy 潜力 | 较受限        | 通常更好             |
| 安全性          | 依赖参数与内存校验  | 同样依赖严格验证         |
| 生命周期复杂度      | 相对低        | 相对高              |

我的工程建议是：

对于预硅 Bring-up，可以优先确保一个简单、可控的共享内存通道正常工作；进入完整 Linux / TA / High-throughput 场景后，再将 Dynamic SHM 与 Zero-copy 作为主要优化目标。

但如果目标传输 ABI 从一开始就要求 Memory Parcel，则不能直接套用旧式 SMC 的 Static SHM 实现。

# 14.4 RPMI Memory Parcel 与共享内存生命周期

这一节是 RISC-V OP-TEE 特有的架构重点。

## 14.4.1 先区分三个经常混淆的概念

在 RISC-V RPMI 方案中，你可能同时看到：

1. RPMI Transport Shared Memory
2. RPMI Memory Parcel
3. OP-TEE Message Buffer / TA Data Buffer

它们不是一回事。

Transport Buffer

用来承载 RPMI 请求与响应消息，由传输机制管理

Memory Parcel

描述一组可以在不同安全 Endpoint / Domain 间共享或借出的内存

OP-TEE Command / RPC / TA Payload

真正要传递给 TEE 的命令结构和业务数据，可放在 Parcel 对应的内存中

底层 RPMI Transport Buffer 是通信设施本身使用的内存，而 Memory Parcel 是对业务共享内存建立的独立描述和权限管理对象。

更重要的是，这两个概念可以有不同的缓存属性和生命周期。

在已批准的 RPMI v1.0 Shared Memory Transport 中，规范要求该传输共享区至少具备双方一致可见的、非缓存的 I/O 属性。但不能因此推断所有通过 Memory Parcel 共享的数据页都必须是 Uncached。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org+1



## 14.4.2 什么是 Memory Parcel？

可以把 Memory Parcel 理解成：

> 对一组内存页的跨隔离域共享关系进行登记，并通过受管理的 ID 引用这些内存。

它不是简单的物理地址。

从逻辑上看：

```
Memory Parcel
    |
    +-- Parcel ID
    +-- Nonce / Generation information
    +-- Owner / Sender
    +-- Receiver / Endpoint
    +-- Physical Memory Regions
    +-- Access Permissions
    +-- Lifecycle State
```

这里列出的是需要在设计中考虑的逻辑信息，不是声称 RPMI 规范中存在一个包含这些同名字段的 C 结构。

假设 Linux 希望 OP-TEE 访问三页内存：

```
Linux owned pages:
    PA0 = 0x81000000
    PA1 = 0x81200000
    PA2 = 0x81400000
```

在支持 Memory Parcel 的框架中，Linux 不一定需要将这三个 PA 直接放入 OP-TEE 每一次调用的参数中。

它可以先建立一个 Parcel：

```
Parcel ID = 0x25
Nonce     = 0xAABBCCDD

Pages:
    [PA0, PA1, PA2]
```

然后业务调用使用 Parcel 引用和 Offset：

```
Parcel ID = 0x25
Offset    = 1024
Size      = 4096
```

接收方根据受管理的 Parcel 描述解析出正确的物理页，再建立访问映射。

注意：Parcel ID 本身不应被视为不可伪造的安全凭证。 调用者身份、Nonce、生命周期以及框架授权都需要参与校验。

## 14.4.3 Share、Lend、Reclaim 的区别

这是需要重点理解的架构概念。

Share

内存所有者允许另一个 Endpoint 访问指定内存。通常双方可以在共享协议规定的范围内继续访问。

Lend

内存所有者在一定期间将访问能力借给另一个 Endpoint；根据协议语义，借出方原有的访问权限可能需要受到限制。

Reclaim

在接收方完成使用并解除相应共享/借用关系后，内存所有者回收这项授权，重新获得完整的生命周期控制。

不能仅从名字判断这些操作已经自动产生了硬件隔离效果。

例如，借出方是否真正失去访问能力，取决于 M-mode Firmware、PMP、WorldGuard 或其他 SoC Hardware Isolation 是否落实对应权限。

2026 年 10 月的 RPMI TEE Transport RFC 已包含 Memory Parcel 的 lend、share、reclaim 抽象。

[image](https://www.google.com/s2/favicons?domain=https://lists.infradead.org\&sz=32)

lists.infradead.org



## 14.4.4 一个完整的 Parcel 生命周期

Memory Parcel State Machine（概念模型）

用于讲解的安全生命周期状态机；不是 RPMI Specification 中的逐字状态定义。

它可以对应如下调用流程：

```
Linux
  |
  | Allocate backing pages
  |
  | MEM_PARCEL_CREATE / SHARE
  v
M-mode RPMI Framework
  |
  | Register parcel
  | Verify memory ownership/policy
  |
  v
TEE Endpoint
  |
  | Resolve parcel reference
  | Map pages into permitted address space
  | Process OP-TEE request
  |
  | Release references
  v
M-mode Framework
  |
  | Revoke access / update lifecycle state
  |
  v
Linux
  |
  | MEM_PARCEL_RECLAIM
  |
  v
Release or reuse pages
```

实际接口和步骤应对应所使用的 RPMI 版本、传输实现与 OP-TEE Backend。

## 14.4.5 为什么 Reclaim 是一个安全问题？

假设：

```
Hart 0:
    OP-TEE 正在读取 Parcel 0x25

Hart 1:
    Linux 发起 Reclaim Parcel 0x25
```

如果 Framework 在 OP-TEE 尚未释放 Mapping 和 References 时就允许回收：

```
TEE still accesses PA0
         |
         v
Linux reallocates PA0
         |
         v
PA0 now contains another user's data
```

就可能发生跨安全边界的 Use-After-Free、数据混淆甚至敏感信息泄露。

因此必须设计严格的生命周期规则：

\\[ ReclaimAllowed \Rightarrow NoActiveTEEReferences \\]

在支持动态映射与多 hart 执行的系统中，还应保证必要的 TLB Translation Invalidation 已经完成，不能只修改一个软件标志就认为权限收回了。

## 14.4.6 Nonce 为什么重要？

假设某个 Parcel ID 曾经是：

```
ID = 17
Pages = A, B, C
```

这个 Parcel 被释放后，Framework 又复用了 ID 17：

```
ID = 17
Pages = X, Y, Z
```

如果一个旧的异步请求仍然携带 ID 17，则可能错误引用新的 Parcel。

这种问题称为 ABA 类句柄重用问题。

如果协议把 Parcel ID 与 Nonce 结合：

```
Old: ID 17, Nonce 100
New: ID 17, Nonce 101
```

就更容易区分两个生命周期。

但是 Nonce 不能替代授权检查，也不能自动防止恶意 Endpoint 访问它已经能够读取的共享内存。

## 14.4.7 2026 年 RISC-V OP-TEE 方案的两条演进路线

这一点非常值得你作为 upstream Contributor 研究。

方案 A：保留经典 OP-TEE SMC Message ABI

2026 年 9 月 10 日的 RFC 提议，将经典 OP-TEE SMC Register Arguments 通过 RPMI `TEE_CALL` 携带。

它的优势是尽量复用现有 Message ABI、RPC 和 Dynamic SHM 逻辑。

方案 B：采用独立的 RPMI Backend + Parcel Reference ABI

2026 年 10 月 5 日的 RFC v2 提出：

- 使用独立 RPMI Transport；
- 用 Parcel ID + Nonce 代替部分原有的内存引用机制；
- 使用显式 Command/RPC Buffer Range；
- 使用 Opaque Token 恢复 Yielded Call；
- 尽量复用 OP-TEE Driver 的通用 Session、Call Queue、RPC 基础设施。

两者反映的是不同的软件架构取舍，而不仅是两种调用指令的区别。

[image](https://www.google.com/s2/favicons?domain=https://lists.openwall.net\&sz=32)

Openwall Lists+1



| 设计问题 | 复用经典 SMC ABI      | 新 RPMI Backend        |
| ---- | ----------------- | --------------------- |
| 兼容性  | 有利于复用现有实现         | 需要明确新 Backend ABI     |
| 内存引用 | 沿用既有 TMEM/RMEM 语义 | 可原生表达 Parcel ID/Nonce |
| RPC  | 复用传统恢复语义          | 可使用显式 Resume Token    |
| 代码维护 | 传输层兼容逻辑可能增加       | Backend 分离更清晰         |
| 测试风险 | 兼容性问题             | 新协议互通和生命周期问题          |

这两条路线不能在实现中随意拼接。 对于真实 SoC 项目，必须先锁定 Linux、OP-TEE OS、OpenSBI 和 RPMI 的 ABI 版本，再讨论具体的 Memory Parcel 流程。

# 14.5 Linux `tee_shm` 与 OP-TEE `mobj` 的关系

## 14.5.1 为什么需要两个内存对象？

这是本章最重要的源码理解点之一。

Linux 已经有完整的内存管理系统，而 OP-TEE 也有自己的 Memory Management。

它们不能直接共享内核内部的内存管理对象，因为双方的地址空间、信任边界、生命周期管理都不同。

因此：

Linux 使用 `struct tee_shm` 管理内存资源，OP-TEE 使用 `struct mobj` 对可访问内存进行抽象。

这两个对象可以指向同一组物理页，但不是同一个对象。

```
Linux Kernel                              OP-TEE Core

struct tee_shm                             struct mobj
    |                                          |
    +-- backing pages                         +-- mobj ops
    +-- size                                  +-- size
    +-- Linux references                      +-- OP-TEE references
    +-- user/kernel mappings                  +-- physical translation
    +-- registration state                    +-- secure mapping policy
    |                                          |
    +------------- Physical Pages -------------+
                    PA0 PA1 PA2
```

不要把两侧对象理解成严格的一对一关系。某些实现中存在对象缓存、多个映射、多个引用或传输层中间对象。

## 14.5.2 Linux `struct tee_shm`

在 Linux TEE Subsystem 中，`tee_shm` 是管理 Shared Memory 的通用抽象。

核心职责包括：

- 分配或注册共享 Buffer；
- 保存 Size、Flags 和 Backing Memory 信息；
- 跟踪用户空间、内核空间和驱动引用；
- 为驱动提供物理页和虚拟地址访问方法；
- 调用 TEE Driver 的注册、注销流程；
- 控制最终释放时机。

可以从以下 API 开始研究：

```
tee_shm_alloc_kernel_buf(...)
tee_shm_register_user_buf(...)
tee_shm_get_va(...)
tee_shm_get_pa(...)
tee_shm_free(...)
```

源码：[Linux drivers/tee/tee_shm.c](https://github.com/torvalds/linux/blob/master/drivers/tee/tee_shm.c?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



要注意，注册用户 Buffer 和分配 Kernel Buffer 是两个不同的操作。

例如，`tee_shm_register_user_buf()` 管理已有的 User Memory，而 `tee_shm_alloc_kernel_buf()` 创建供 Kernel TEE Client 使用的内存。

### 用户态如何与 `tee_shm` 关联？

CA 通过 `libteec` 发起共享内存操作：

```
TEEC_SharedMemory shm = { 0 };

shm.size = 4096;
shm.flags = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;

TEEC_Result res =
    TEEC_AllocateSharedMemory(&ctx, &shm);
```

实际实现还会经过 Linux TEE IOCTL、Memory Mapping 或 Buffer Registration 等操作。

Linux Driver 建立 `tee_shm` 之后，可以在底层传输要求下，将它注册给 OP-TEE 或其他受管理的 TEE Endpoint。

## 14.5.3 OP-TEE `struct mobj`

`mobj` 的设计更值得从操作系统架构角度理解。

它不是简单保存一个 PA。

从概念上，一个 Memory Object 应提供：

```
MOBJ
 |
 +-- Identity / Type
 +-- Size
 +-- Reference Count
 +-- Mapping Operations
 +-- Physical Address Translation
 +-- Security Attributes
```

不同类型的 MOBJ 可以具有不同的后端实现：

- Physically Contiguous Memory
- Registered Dynamic Shared Memory
- Paged Memory
- Secure Copy Shared Memory
- 特定安全路径使用的 Memory Object

之所以需要抽象，是因为 OP-TEE 的上层逻辑不应该关心所有 Shared Memory 都是连续 PA、离散页，还是通过 FF-A 等框架引用。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 14.5.4 重点研究 `mobj_reg_shm_alloc()`

当前 `optee_os/core/mm/mobj_dyn_shm.c` 中存在：

```
struct mobj *mobj_reg_shm_alloc(
    paddr_t *pages,
    size_t num_pages,
    paddr_t page_offset,
    uint64_t cookie);
```

这里最重要的四个参数是：

| 参数            | 含义              |
| ------------- | --------------- |
| `pages`       | 已解析的物理页地址数组     |
| `num_pages`   | 物理页数量           |
| `page_offset` | Buffer 在第一页中的偏移 |
| `cookie`      | 注册共享对象的引用标识     |

这个函数不是简单执行一次 `calloc()`。

从当前源码可以看到，除了分配 `mobj_reg_shm`，还需要：

1. 验证 `num_pages` 和 `page_offset`；
2. 为 Page Array 分配足够空间；
3. 初始化 MOBJ Size、Operations 和 Reference Count；
4. 拷贝输入的物理页数组；
5. 检查页对齐要求；
6. 检查每个页面是否属于允许的 Non-secure Memory；
7. 检查与受保护内存区域的冲突；
8. 在适当的同步保护下加入 Registered SHM List。

其中源码的两项关键检查是：

```
if (mobj_reg_shm->pages[i] & SMALL_PAGE_MASK)
    goto err;

if (!core_pbuf_is(CORE_MEM_NON_SEC,
                  mobj_reg_shm->pages[i],
                  SMALL_PAGE_SIZE))
    goto err;
```

这两段代码很重要，因为它们保证不能把任意 Secure RAM 地址伪装成 Dynamic SHM。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 为什么先验证页属性再加入注册列表？

假设恶意 Linux 提供：

```
Page 0: Normal RAM
Page 1: Normal RAM
Page 2: OP-TEE Private RAM
Page 3: Normal RAM
```

如果 OP-TEE 只验证 Page List 的首尾，或者只验证起始地址，就可能将 Secure RAM 错误注册为 Non-secure Shared Memory。

正确做法必须对整个物理页集合应用安全策略。

注意，这只是物理区域校验的一部分。真实安全性还需要避免物理地址别名、页表映射冲突、动态保护区域冲突等问题。

## 14.5.5 为什么不能只验证 `shm_ref`？

即使 Cookie 对应合法 MOBJ，传入的 Offset/Size 仍然可能越界。

例如：

```
MOBJ Size = 4096

Request:
    Offset = 3500
    Size   = 1024
```

请求结束位置是：

\\[ 3500 + 1024 = 4524 \\]

超过对象边界，因此必须拒绝。

更危险的是：

```
Offset = SIZE_MAX - 8
Size   = 32
```

如果直接使用：

```
if (offset + size <= mobj->size)
    /* accept */
```

加法可能溢出，导致错误判断。

一个更容易审查的通用写法是：

```
static bool range_valid(size_t offset,
                        size_t len,
                        size_t total)
{
    return offset <= total &&
           len <= total - offset;
}
```

这是用于解释边界校验的独立示例，不是原样摘录 OP-TEE。

当前 OP-TEE 的 `set_rmem_param()` 实现会通过 `mobj_reg_shm_get_by_cookie()` 查找对象，使用 `READ_ONCE()` 获取相关非可信字段，并利用 `ADD_OVERFLOW()` 检查请求范围。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这也提示我们另一个非常重要的工程原则：

将非可信输入转换为可信的内部描述，是 Shared Memory 处理路径中的核心安全操作。

## 14.5.6 MOBJ Reference Count 与内存生命周期

假设一个 MOBJ 被两个正在执行的 Trusted Thread 使用：

```
MOBJ X
 |
 +-- Thread A reference
 |
 +-- Thread B reference
 |
 +-- Registration / owner reference
```

如果 Linux 此时请求注销共享内存，OP-TEE 不能仅仅因为收到注销命令就强制释放全部内部状态。

需要保证引用关系、注销状态和实际物理内存回收遵守正确顺序。

从状态机角度来看：

```
REGISTERED
    |
    | Acquire reference
    v
IN_USE
    |
    | Unregister request
    v
RELEASING
    |
    | Wait / reject new references
    |
    | Existing references reach zero
    v
UNREGISTERED
```

具体实现可能通过锁、Reference Count、Condition Variable 或其他机制实现，并不要求严格使用这些状态名。

关键在于，拒绝新的引用和等待旧引用完成这两个步骤必须具有正确的同步关系。

## 14.5.7 注册代码中的一个非常值得研究的细节

在 Linux 的 `optee_shm_register()` 实现中，注册请求本身需要一个 `optee_msg_arg`。

但此时正在注册的 SHM 尚未生效。

因此，不能直接依赖“需要先注册才能使用”的普通 Dynamic SHM 来完成自己的注册请求，否则就形成循环依赖：

```
Register SHM
     |
     v
Need registered SHM for message
     |
     v
Register SHM
     |
     ...
```

当前 Linux SMC Backend 会为这类注册请求使用专门的临时 Message Buffer，然后构造 `OPTEE_MSG_CMD_REGISTER_SHM` 及非连续页列表。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这种 Bootstrapping 问题是 Firmware、Hypervisor 和 IPC 协议中非常常见的设计难点。

# 14.6 RPC：为什么 OP-TEE 需要暂时返回 REE？

## 14.6.1 RPC 的真正含义

RPC = Remote Procedure Call。

在 OP-TEE 的语境中，它通常是：

OP-TEE Core 在执行过程中，向 Linux Kernel 或 `tee-supplicant` 请求某项服务，并在服务完成之后继续原来的执行。

需要特别注意，RPC 的方向与 CA Invoke 相反：

```
Normal Call:
    Linux --> OP-TEE

RPC:
    OP-TEE --> Linux
```

## 14.6.2 为什么 OP-TEE 需要 RPC？

考虑一个 TA 首次启动时的情况。

TA 的二进制文件可能存放在 Linux 文件系统中：

```
/lib/optee_armtz/<UUID>.ta
```

这是一个常见的 Arm 平台 TA 文件布局示例，具体目录和架构依赖部署配置。

当 Linux 请求打开该 TA Session 时：

```
Linux invokes OpenSession
             |
             v
OP-TEE tries to locate TA
             |
             v
TA binary is not available locally
             |
             v
Needs REE filesystem service
```

OP-TEE 本身通常不需要实现完整的 Linux VFS、Block Layer、文件系统驱动以及磁盘设备栈。

于是它通过 RPC，请求 Linux 提供 TA Image。

这体现了一种重要设计：

TEE 只承担需要可信执行的部分，复杂但无需可信的服务可以委托给 REE。

但这不意味着 REE 提供的数据可信。

例如 Linux 提供的 TA Image 仍必须经过 OP-TEE 所要求的完整性和签名校验，不能因为它来自 `tee-supplicant` 就直接执行。

## 14.6.3 RPC 与普通函数调用有什么不同？

假设 OP-TEE 内部执行：

```
TEE_Result load_ta(...)
{
    ...
    res = request_ta_from_ree(...);
    ...
    return res;
}
```

从 C 语言语义来看，`request_ta_from_ree()` 看起来像普通函数。

但实际运行可能发生：

```
OP-TEE Secure Thread
    |
    | request_ta_from_ree()
    |
    v
Save Thread Context
    |
    v
Leave OP-TEE Domain
    |
    v
Linux processes service
    |
    v
Re-enter OP-TEE Domain
    |
    v
Restore Thread Context
    |
    v
request_ta_from_ree() returns
```

这个过程中，CPU 曾经离开 OP-TEE，但 OP-TEE 的 Kernel Call Stack 不能丢失。

因此 RPC 不只是一个 IPC Protocol 问题，还是一个 Continuation / Context Switching 问题。

## 14.6.4 Trusted Thread 的状态变化

以经典 OP-TEE Thread 执行模型为例：

```
FREE
  |
  | Normal Call
  v
ACTIVE
  |
  | RPC Exit
  v
SUSPENDED
  |
  | RPC Return
  v
ACTIVE
  |
  | Command Finished
  v
FREE
```

这里的关键区别是：

RPC Suspend 并不是 Free Trusted Thread。

Suspend 后，Thread 的执行上下文仍然需要被保存，以便继续此前的调用链。

如果一个 Trusted Thread 的调用栈是：

```
tee_entry_std()
   |
   v
tee_ta_open_session()
   |
   v
load_user_ta()
   |
   v
load_ta_from_ree()
   |
   v
thread_rpc()
```

当 `thread_rpc()` 返回之后，调用链应继续沿着：

```
load_ta_from_ree()
   |
   v
load_user_ta()
   |
   v
tee_ta_open_session()
```

运行，而不是从 `tee_entry_std()` 重新开始。

OP-TEE 官方文档对经典 RPC Exit 的解释正是保存线程状态，使恢复后能像一次普通函数返回一样继续执行。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 14.6.5 RPC 不一定需要 `tee-supplicant`

这个问题很容易被误解。

实际上 RPC 可以由两类服务提供者处理。

Kernel-handled RPC

Linux OP-TEE Driver 直接完成服务。

例如时间查询、Notification、部分 RPMB Routing 等

Supplicant-handled RPC

Linux Driver 将请求转发到 User Space。

例如 TA Loading、REE 文件系统访问及配置相关服务

当前 Linux 的 `drivers/tee/optee/rpc.c` 中，`optee_rpc_cmd()` 会根据 RPC Command 类型决定由内核处理，还是转发到 Supplicant。

例如部分 RPMB 请求是否经过 Supplicant，取决于 Kernel RPMB Routing 的支持与配置。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



所以不能简单地说：

“OP-TEE 每次 RPC 都会调用 `tee-supplicant`。”

这并不准确。

## 14.6.6 RPC 执行期间 Linux 能做什么？

在 RPC 服务期间，Linux 可能执行文件 I/O、线程调度、内存分配和其他正常操作。

但是要特别注意：

- 原来的 OP-TEE Thread Context 仍然存在。
- Linux 不能假设它已经完成。
- RPC 期间其他 hart 可能仍在 OP-TEE 执行其他请求。
- Linux 也可能产生新的 TEE 请求。
- 如果共享资源存在锁依赖，可能产生跨 Domain Deadlock。

例如：

```
Thread A:
    OP-TEE holds Lock X
        |
        v
    RPC to Linux

Linux RPC handler:
    Requires another TA operation
        |
        v
    New TA operation needs Lock X
```

如果资源获取顺序没有经过设计，就可能出现死锁。

因此，在 OP-TEE 中，RPC 不仅是通信问题，还涉及锁、调度、重入和资源依赖关系。

# 14.7 `tee-supplicant` 在整体架构中的作用

## 14.7.1 `tee-supplicant` 不是另一个 Trusted Application

它是运行在 REE User Space 中的普通进程。

通常通过 Linux TEE Device 接收来自 OP-TEE 的 RPC 请求：

```
OP-TEE Core
    |
    | RPC Request
    v
Linux OP-TEE Driver
    |
    | TEE_IOC_SUPPL_RECV / SEND
    v
tee-supplicant
    |
    +-- TA Loading
    +-- REE File System
    +-- Storage Services
    +-- Other platform-enabled services
```

即使这个进程以 root 权限运行，它仍然属于 Untrusted REE。

它不是 OP-TEE TCB 中的可信安全计算部分。

## 14.7.2 为什么不直接在 Linux Kernel 中处理所有 RPC？

原因不是 Linux Kernel 无法处理这些服务，而是职责划分。

例如读取 TA 文件：

```
Open file
Read contents
Manage paths
Interact with userspace filesystem
```

这些操作天然适合 User Space Daemon。

如果将所有文件管理逻辑都塞进 OP-TEE Driver，就会显著增加 Kernel Driver 的复杂度。

`tee-supplicant` 可以利用普通 Linux 文件系统服务，完成这些非可信工作。

## 14.7.3 Supplicant 的实际请求流程

OP-TEE 发起 RPC：

```
OP-TEE
    |
    v
Linux OP-TEE RPC Handler
    |
    v
Supplicant Request Queue
    |
    v
tee-supplicant worker
    |
    | TEE_IOC_SUPPL_RECV
    v
Decode RPC Function
    |
    v
Perform requested operation
    |
    | TEE_IOC_SUPPL_SEND
    v
Linux RPC Handler
    |
    v
OP-TEE Resume
```

当前 `tee_supplicant.c` 实际使用 `TEE_IOC_SUPPL_RECV` 和 `TEE_IOC_SUPPL_SEND`，并含有 Worker Thread 处理逻辑。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 14.7.4 一个完整的 TA Loading 示例

假设 Linux 要加载 UUID 为 `X` 的 User TA：

```
CA
 |
 | OpenSession(UUID=X)
 v
OP-TEE
 |
 | TA X not loaded
 |
 | RPC: LOAD_TA(X)
 v
Linux Driver
 |
 v
tee-supplicant
 |
 | Locate X.ta
 | Read TA binary
 | Copy into shared payload buffer
 v
Linux Driver
 |
 | RPC response
 v
OP-TEE
 |
 | Validate TA image
 | Authenticate signature
 | Load ELF
 | Establish user mappings
 v
TA X execution
```

请特别注意信任边界：

```
tee-supplicant says "Here is TA X"
```

并不等于：

```
OP-TEE can trust these bytes
```

OP-TEE 必须依照 TA 加载策略验证镜像。

另外，完整性和签名验证并不能天然保证 TA 版本新鲜性。如果平台有防回滚要求，还需要版本策略及可信计数或其他可信状态支持。

## 14.7.5 为什么 REE FS 可以存储 Secure Storage 数据？

这个问题可以用一句话解释：

存储介质不可信，不代表存储数据一定无法受到密码学保护。

例如：

```
TA Data
   |
   v
OP-TEE encryption / integrity protection
   |
   v
Encrypted Storage Object
   |
   | RPC
   v
tee-supplicant
   |
   v
Linux Filesystem
```

这里安全性依赖 OP-TEE 的加密与认证设计以及密钥安全，不依赖 Linux File System 的可信性。

但 REE 仍然可能：

- 删除文件；
- 拒绝提供数据；
- 延迟响应；
- 返回旧版本数据；
- 造成存储资源耗尽。

因此：

Confidentiality、Integrity、Rollback Protection、Availability 是四种不同的安全属性。

其中 Rollback Protection 是否成立，取决于是否存在可信的新鲜性状态和具体存储后端。

## 14.7.6 Supplicant 崩溃会怎样？

假设一个 TA 正在进行 Secure Storage 操作，刚刚发起了 RPC。

此时 `tee-supplicant` 崩溃。

可能出现：

```
OP-TEE Thread:
    SUSPENDED waiting for RPC

Linux:
    tee-supplicant exits

RPC:
    No successful response
```

表现可能是错误、等待、请求取消或经驱动执行失败恢复，具体取决于实现和触发条件。

不能简单认为 OP-TEE 会自动重启整个 Secure World。

系统必须考虑：

- Supplicant 服务恢复；
- Pending RPC 的清理；
- 请求超时与取消；
- 共享内存的释放；
- 失败后的 Session 是否仍有效；
- 服务中断期间是否需要阻止新的请求。

尤其在量产服务器 SoC 中，如果某个 Secure Service 依赖 REE Supplicant，那么 Supplicant 的可用性直接影响业务功能，即使它不是可信的安全实体。

# 14.8 RPC 返回后如何恢复 Trusted Thread？

这一节需要深入到执行上下文，而不是停留在 RPC API 层面。

## 14.8.1 回到最核心的问题：调用栈如何保留下来？

假设 OP-TEE 执行：

```
static int secure_operation(void)
{
    int a = 10;
    int result;

    result = do_rpc();

    return a + result;
}
```

当 `do_rpc()` 触发 Domain Exit 时，不能丢失：

- 当前函数的返回地址；
- 当前 SP；
- ABI 定义的需要保存的寄存器；
- Kernel Stack 内容；
- 当前 Trusted Thread Identity；
- 必要的特权 CSR；
- 当前 Address Space；
- 与 Session、TA、RPC 对应的状态。

否则，当 Linux RPC 完成后，就无法从 `do_rpc()` 返回的位置继续执行。

## 14.8.2 区分三个 Context

这里必须延续 Chapter 06 和 Chapter 10 的知识。

| Context                       | 属于哪个层次              | 保存目的                           |
| ----------------------------- | ------------------- | ------------------------------ |
| OpenSBI Domain Context        | M-mode Framework    | 在 Linux/TEE Domain 之间切换执行环境    |
| OP-TEE Trusted Thread Context | OP-TEE Kernel       | 暂停并恢复某个 Secure Thread          |
| User TA Context               | OP-TEE TA Execution | 在 TA U-mode 和 OP-TEE Core 之间切换 |

三者可能在一次 RPC 中同时产生作用，但它们绝不能被混成一个“Context Switch”。

例如：

```
RPC Exit:

1. OP-TEE saves Trusted Thread state
2. OP-TEE transfers control to M-mode framework
3. M-mode switches TEE -> REE Domain
4. Linux handles RPC

RPC Resume:

5. Linux issues resume operation
6. M-mode switches REE -> TEE Domain
7. OP-TEE validates resume identity
8. OP-TEE restores Trusted Thread
9. Original RPC call returns
```

这是 RISC-V Domain 架构下的概念执行路径。

## 14.8.3 经典 OP-TEE SMC 实现的 Resume

在经典 Arm SMC 模型中，RPC 请求可以通过专门的返回码通知 Linux。

例如：

```
OPTEE_SMC_RETURN_RPC_CMD
OPTEE_SMC_RETURN_RPC_ALLOC
OPTEE_SMC_RETURN_RPC_FREE
```

Linux Driver 处理 RPC 后，再通过 `OPTEE_SMC_CALL_RETURN_FROM_RPC` 重新进入 OP-TEE。

它不是重新执行原始的 `INVOKE_COMMAND`。

当前 Linux `smc_abi.c` 的 `optee_smc_do_call_with_arg()` 正体现了这一控制流：

```
/* Simplified conceptual flow */

for (;;) {
    result = enter_optee();

    if (result == THREAD_LIMIT) {
        wait_for_available_thread();
        continue;
    }

    if (is_rpc_return(result)) {
        handle_rpc();
        prepare_resume_call();
        continue;
    }

    break;
}
```

这段代码是根据实际控制流整理的教学伪代码，并非可以直接替换 upstream 的函数实现。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



## 14.8.4 RISC-V 下的 Resume 为什么更加复杂？

因为 RISC-V OP-TEE Domain Switch 可以同时涉及：

1. Linux S-mode → M-mode
2. M-mode → OP-TEE S-mode
3. OP-TEE Trusted Thread Resume
4. 可能的 TA U-mode 状态恢复

因此，一个 Resume 请求需要保证：

- 要恢复的 Thread 确实处于 Suspended 状态；
- Resume 请求属于正确的 Client / Domain；
- 不能恢复已释放的 Thread；
- 不能重放旧的 Resume 请求；
- 必须恢复正确的地址空间和栈；
- 不能错误地继承其他 Domain 的 Privileged State。

如果 Resume 仅通过一个容易猜测的 Thread ID 实现，而没有适当的身份及生命周期检查，就存在跨客户端恢复或错误重放的风险。

## 14.8.5 为什么 Opaque Resume Token 是值得研究的设计？

2026 年的 RPMI Backend RFC v2 引入了 Opaque Resume Token。

从系统设计角度，一个不暴露内部线程布局的 Token 有几个潜在好处：

- 避免把内部 Thread Index 直接绑定到外部 ABI；
- 可以与调用生命周期关联；
- 可以携带或关联 Generation 信息；
- 可以在恢复时检查调用者身份；
- 有利于未来修改内部 Thread 实现。

但必须强调：

Opaque 不等于 Secure。

如果 Token 只是单纯的序号，没有严格的生命周期和调用方校验，依然可能存在错误重放。

安全性来自身份绑定、状态机和有效性校验，而不来自 Token 名字。

## 14.8.6 一个特别值得研究的并发场景

假设两个 hart 同时执行：

```
Hart 0:
    OP-TEE Thread A
    -> RPC A

Hart 1:
    OP-TEE Thread B
    -> RPC B
```

Linux 可能先处理完 RPC B，再处理完 RPC A。

因此：

```
Issue order:
    A -> B

Completion order:
    B -> A
```

不能假设全局 RPC 完成顺序与发起顺序一致。

安全设计必须通过 per-request Context、Request ID 或 Resume Token 找到正确的 Suspended Thread。

并且需要考虑一个细节：

OP-TEE 的 Thread Identity、Linux Task Identity、Hart ID 是三种不同标识。

不能把它们简单等同。

在某些 RISC-V 同 hart 同步 Domain Switch 实现中，调用期间的 Hart Affinity 可能是明确约束；但这种约束来自特定 Transport/Domain Switch 设计，而不是 OP-TEE 共享内存和 RPC 的通用规则。

# 14.9 Cache Coherency、DMA 与 Shared Memory

在真实 RISC-V SoC 上，这一节的重要性不亚于 RPC 和 Memory Parcel。

因为 Shared Memory 的物理地址正确、页表映射正确，也不能保证双方一定看到相同的数据。

## 14.9.1 先区分三个不同的问题

| 问题                  | 对应机制                              | 解决什么          |
| ------------------- | --------------------------------- | ------------- |
| Address Translation | MMU / `satp` / Page Table         | VA 怎样找到 PA    |
| Memory Ordering     | RVWMO / `FENCE` / Acquire-Release | 内存访问的可观察顺序    |
| Cache Coherency     | Coherent Interconnect / CMO       | 不同访问者怎样看到正确数据 |

这是三个独立但相互关联的层次。

一个常见错误是：

> “我在 Domain Switch 时执行了 `SFENCE.VMA`，所以共享内存中的 Cache 数据肯定同步了。”

这是错误的。

`SFENCE.VMA` 用于地址转换相关的同步，不是通用 Data Cache Clean/Invalidate 指令。

同样，普通的 `FENCE` 主要负责内存访问顺序，并不能代替缺失的硬件 Cache Maintenance。

RISC-V 的 Zicbom 扩展定义了 `CBO.CLEAN`、`CBO.INVAL`、`CBO.FLUSH` 等操作，但需要硬件、权限配置及平台软件共同支持。

[image](https://www.google.com/s2/favicons?domain=https://riscv.github.io\&sz=32)

riscv.github.io



## 14.9.2 Cache Coherent 的情况

假设 Linux 和 OP-TEE 在相互一致的 Cache Coherency Domain 中运行。

```
Linux Hart 0                  OP-TEE Hart 1
     |                              |
     v                              v
  L1 DCache                     L1 DCache
     |                              |
     +----------+-------------------+
                |
          Coherent Fabric
                |
             DRAM
```

如果双方使用兼容的内存属性，而且硬件一致性协议覆盖这些访问，那么 Linux 写入的数据可以通过一致性机制被 OP-TEE 正确观察。

但是仍需要适当的同步协议。

例如：

```
/* Illustrative producer */
payload->data = 0x12345678;
atomic_store_explicit(&payload->ready, 1,
                      memory_order_release);
```

接收方：

```
while (!atomic_load_explicit(&payload->ready,
                             memory_order_acquire))
    ;

value = payload->data;
```

这种 Release/Acquire 同步在双方共享一致性内存、原子操作及 ABI 满足要求时，可以建立数据发布与读取的顺序关系。

但它并不能阻止恶意 REE 在接收后再次修改 `data`。

Memory Ordering 保证的是一定条件下的观察顺序，不保证对手不会修改内容。

## 14.9.3 Non-coherent 的情况

假设 Application Hart 与独立 HSM Core 不在同一个 Cache Coherency Domain。

```
Application CPU                   HSM Core
       |                              |
    DCache A                       DCache B
       |                              |
       +------------DRAM--------------+
```

Linux 写入：

```
shared->command = CMD_DECRYPT;
shared->length = 4096;
```

但如果数据仍然停留在 Application CPU 的 Dirty Cache Line 中，而 HSM 读取的是 DRAM，就可能看到旧值。

反过来，HSM 写入结果后，如果 Application CPU 保留了旧的 Cache Line，也可能读取到过期数据。

一个典型的双向通信协议需要：

```
Producer:
    Write payload
    Perform required cache clean / sync
    Publish descriptor
    Ring doorbell

Consumer:
    Observe notification
    Perform required cache invalidate / sync
    Read payload
    Process request
```

具体操作与硬件一致性模型有关，不能机械地在所有平台上增加同一组 Cache Flush。

## 14.9.4 Linux DMA API 在这里有什么作用？

如果数据经过 DMA Engine 或其他非一致性 Bus Master，Linux Driver 应优先通过 Linux DMA API 管理其 Buffer，而不是自己到处编写平台专用 Cache Flush。

例如：

```
dma_sync_single_for_device(dev, dma_addr, len,
                           DMA_TO_DEVICE);
```

表示在设备读取前执行所需同步。

而：

```
dma_sync_single_for_cpu(dev, dma_addr, len,
                        DMA_FROM_DEVICE);
```

用于设备写入后、CPU 读取前的同步。

这些 API 要求 Buffer 已经通过相应的 DMA Mapping 操作建立有效 DMA 地址，并遵守映射与同步生命周期，不能对任意物理地址直接调用。

[image](https://www.google.com/s2/favicons?domain=https://docs.kernel.org\&sz=32)

The Linux Kernel documentation



更重要的是：

Linux DMA API 解决的是 Linux Driver 与 Device 之间的 DMA 一致性，不会自动替你解决全部 Linux Domain ↔ OP-TEE Domain 的跨域缓存同步。

跨域数据是否一致，仍然需要平台和 Transport ABI 明确规定。

## 14.9.5 Shared Memory Cache Attribute 必须一致

假设同一组物理页在 Linux 中被映射为 Normal Cacheable Memory，而在 OP-TEE 中被映射为 Device Memory。

这可能产生错误的访问属性组合和难以调试的行为。

因此设计者需要明确：

- Linux 映射属性；
- OP-TEE 映射属性；
- M-mode 对传输内存的访问属性；
- SoC PMA/PBMT 支持情况；
- 硬件是否支持跨域一致性；
- 是否允许别名映射；
- 是否有特定的 Cache Maintenance 需求。

如果同一物理页有多个不同属性的映射，必须满足体系结构对内存别名的约束，不能假设行为总是正确。

## 14.9.6 RISC-V 内存屏障应该怎样理解？

几个关键指令的职责可以归纳为：

| 指令           | 主要目的                  |
| ------------ | --------------------- |
| `FENCE`      | 内存/I/O 操作顺序           |
| `SFENCE.VMA` | 地址翻译同步                |
| `FENCE.I`    | 指令获取与此前内存修改的相关同步      |
| `CBO.CLEAN`  | 清理相关数据 Cache Block    |
| `CBO.INVAL`  | 使相关 Cache Block 无效    |
| `CBO.FLUSH`  | 清理并使相关 Cache Block 无效 |

例如：

```
sd      a0, 0(t0)      # Write payload
fence   w, o          # Order memory write before MMIO output
sd      t1, 0(t2)      # Write doorbell MMIO
```

这里 `fence w,o` 是一种典型的 Memory-to-MMIO Ordering 示例，但它不保证所有非一致性设备都能自动看到仍在 Cache 中的数据。

RISC-V Memory Model 明确区分内存和 I/O 顺序，必要时还需要 CMO 或平台提供的 Cache Maintenance。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



## 14.9.7 DMA 为什么可能破坏 TEE 的安全边界？

这是比 Cache 更严重的问题。

假设已经通过 PMP 限制：

```
Linux S-mode:
    Cannot access OP-TEE Private RAM

OP-TEE S-mode:
    Can access its Private RAM
```

但 SoC 中存在一个普通 DMA Controller：

```
Linux configures DMA:
    Source      = OP-TEE Private RAM
    Destination = Linux Normal RAM
```

如果 DMA Controller 不受相应的系统级访问控制保护，那么即使 Linux CPU 不能直接读取 OP-TEE RAM，仍然可能通过 DMA 获取数据。

所以：

CPU PMP Protection 不自动等于系统级 Memory Protection。

在真实 SoC 中，应分析：

- DMA Master ID；
- IOMMU/IOPMP 或 SoC Firewall 配置；
- 外设是否可以绕过部分访问检查；
- Domain Switch 时外设权限是否同步变化；
- DMA 是否可以在 REE/TEE 切换期间继续执行；
- Shared Memory 的来源是否包含敏感物理区域。

如果你以后要将 OP-TEE 与独立 HSM Core 集成，这会是非常重要的架构评审点。

## 14.9.8 一个真实工业场景：QEMU 正常，Palladium 异常

假设：

```
QEMU:
    xtest 100% pass

Palladium:
    运行一段时间后出现：
    - TA 偶发加载失败
    - RPC 返回错误参数
    - 输出结果偶尔使用旧数据
```

如果你是 Staff Engineer，我希望你首先区分：

地址错误、缓存可见性错误、同步错误、生命周期错误、协议错误。

例如可以设计以下验证方法：

| 检查                                | 排查方向         |
| --------------------------------- | ------------ |
| 记录 Linux 和 OP-TEE 侧的 Buffer PA    | 是否引用了同一组物理页  |
| 比较 Page List / Parcel Description | 是否存在地址翻译错误   |
| 对比缓存一致性配置                         | 是否存在非一致性访问   |
| 在合法位置增加 Cache Sync                | 判断问题是否与可见性相关 |
| 增加 Message Sequence Number        | 检测旧消息和乱序消息   |
| 记录 SHM Register/Unregister        | 检查提前释放       |
| 增加 SMP Stress                     | 检查多 hart 竞态  |

需要注意：添加 Barrier 或 Cache Flush 后问题消失，只能提供诊断线索。最终仍需要证明根因及修复符合架构要求。

# 14.10 TOCTOU、Double Fetch 与非可信参数验证

这是本章最重要的安全章节。

即使 OP-TEE 的 MMU、PMP、Domain Isolation 完全正确，如果它错误地信任共享内存，也可能被 REE 攻击。

## 14.10.1 TOCTOU 的本质

TOCTOU = Time Of Check To Time Of Use。

含义是：

代码检查某个条件时成立，但在真正使用该数据时，条件已经发生变化。

例如：

```
struct request *req = shared_buffer;

if (req->length <= MAX_INPUT_SIZE) {
    memcpy(private_buf, req->data, req->length);
}
```

这段代码存在明显问题。

第一次读取：

```
req->length = 64
```

通过检查。

但在 `memcpy()` 读取 `req->length` 之前，恶意 Linux 修改：

```
req->length = 0x100000
```

此时 `memcpy()` 使用的长度与验证时不同，可能造成 Secure Memory 越界写入。

## 14.10.2 Double Fetch 为什么危险？

因为 CPU 两次读取同一个内存地址，不代表得到同一个值。

尤其当该地址属于 REE/TEE Shared Memory 时，REE 可能在两次读取之间修改内容。

```
OP-TEE                       Malicious REE

Read length = 64
      |
Check length <= 4096
      |
                              Write length = 0x100000
      |
Read length again
      |
memcpy(..., length)
```

即使 OP-TEE 的代码执行期间 Linux 在另一个 hart 上运行，也可能发生这种攻击。

这与前面所讨论的 SMP Domain Isolation 密切相关。

## 14.10.3 为什么 `READ_ONCE()` 并不能彻底解决 TOCTOU？

`READ_ONCE()` 可以帮助避免编译器对特定内存访问进行不期望的合并或重复等优化，并使一次显式读取具有更明确的语义。

但：

```
if (READ_ONCE(req->length) <= 4096) {
    memcpy(dst, src, READ_ONCE(req->length));
}
```

仍然不安全。

因为这里有两次读取。

正确的基本思想是：

```
size_t len = READ_ONCE(req->length);

if (len > sizeof(private_buf))
    return TEE_ERROR_BAD_PARAMETERS;

memcpy(private_buf, src, len);
```

然而，这也只解决了 长度字段二次读取 的问题。

它不保证 `src` 指向的共享数据在整个 `memcpy()` 期间保持不变。

因此还要区分两类风险：

| 风险          | 典型防护                  |
| ----------- | --------------------- |
| Metadata 改变 | 单次获取、私有快照、边界校验        |
| Payload 改变  | 安全复制、认证、协议级同步或禁止共享期修改 |

如果必须保证数据内容完全稳定，通常应该先复制到 Private Buffer，再对私有副本执行解析和安全决策。

## 14.10.4 一个更完整的安全参数解析例子

假设非可信请求包含：

```
struct input_desc {
    uint32_t offset;
    uint32_t length;
    uint32_t flags;
};
```

不应该反复从共享内存读取字段。

更合理的解析模型：

```
struct input_desc local;

/* Take a private metadata snapshot */
memcpy(&local, shared_desc, sizeof(local));

/* Validate against a trusted object size */
if (local.offset > mobj_size)
    return TEE_ERROR_BAD_PARAMETERS;

if (local.length > mobj_size - local.offset)
    return TEE_ERROR_BAD_PARAMETERS;

if (local.flags & ~ALLOWED_FLAGS)
    return TEE_ERROR_BAD_PARAMETERS;

/*
 * Access only the validated range.
 * Copy payload into private memory when a stable
 * security-relevant snapshot is required.
 */
```

这仍然是教学级示例。生产代码还需要先确保 Shared Memory Mapping 本身有效、考虑可访问性和内存故障，并处理并发与异常恢复。

其关键思想是：

所有安全相关的决策，都应该基于已经捕获并验证的可信本地状态，而不是重复依赖可变化的共享字段。

## 14.10.5 为什么不能完全信任 Memory Parcel？

假设 RPMI Framework 告诉 OP-TEE：

```
Parcel 0x25
    Pages are valid
    Owner is Linux
```

这能够帮助 OP-TEE 验证共享对象的来源和可访问物理范围。

但它不一定保证：

```
Page contents cannot change
```

因为 Share 的语义本身可能允许 Linux 继续访问。

所以要区分：

Memory Ownership / Mapping Integrity

与：

Memory Content Integrity

是两个不同的问题。

Memory Parcel 可以帮助建立内存所有权和访问授权关系，而内容完整性可能仍然需要锁定访问权限、复制、加密认证或协议约束。

## 14.10.6 RPC 返回数据为什么也需要验证？

假设 OP-TEE 请求：

```
RPC_LOAD_TA(UUID)
```

Linux 返回：

```
Buffer PA
Buffer Size
TA Image Data
```

攻击者可以尝试：

- 返回过大的 Size；
- 返回非法 Pointer；
- 返回与请求 UUID 不对应的 TA；
- 在返回后修改 TA Buffer；
- 返回旧版本 TA；
- 提供格式错误的 ELF；
- 通过高频请求造成资源耗尽。

所以 OP-TEE 必须在可信边界内校验 RPC 的返回信息。

对于 TA Image，还需要遵守 TA Authentication、ELF Parsing、Address Range 与 Memory Mapping 的完整安全规则。

不能把“RPC 调用成功”理解为“RPC 数据可信”。

## 14.10.7 Shared Memory 与 TA Private Buffer 的关系

有一个值得特别强调的设计原则：

允许 TA 使用共享 Buffer，不等于所有 TA 都应该直接在 Shared Buffer 上执行安全敏感的解析。

例如一个 TA 接收经过认证的固件升级包：

```
REE Shared Memory
      |
      v
Read Firmware Header
      |
      v
Verify Signature
      |
      v
Install Firmware
```

如果验证与安装之间仍然直接读取可修改的 Shared Memory，就可能发生：

```
Verify Image A
       |
       | REE changes buffer
       v
Install Image B
```

更安全的设计可以是：

```
REE Shared Memory
      |
      v
Copy to controlled buffer
      |
      v
Validate exact immutable snapshot
      |
      v
Verify signature / hash
      |
      v
Perform operation on verified snapshot
```

如果固件非常大，无法全部复制，也可以使用分块认证、受控制的访问权限或流式协议，但必须确保真正消费的数据与已认证内容具有正确绑定。

## 14.10.8 对 OP-TEE Shared Memory 的威胁建模

建议采用以下威胁清单：

| 攻击类型                   | 可能后果                    | 防护重点                      |
| ---------------------- | ----------------------- | ------------------------- |
| Invalid PA             | Secure RAM 访问或异常        | 物理地址属性验证                  |
| Integer Overflow       | 越界访问                    | Checked Arithmetic        |
| Double Fetch           | 验证与使用不一致                | 私有快照                      |
| Early Unregister       | Use-after-Free          | Refcount / Lifecycle      |
| Replayed Parcel ID     | 错误对象引用                  | Nonce / Generation        |
| Concurrent Mutation    | 安全决策错误                  | Private Buffer / Protocol |
| Malicious RPC Response | TA Loader 或 Storage 受攻击 | 严格解析和认证                   |
| Cache Staleness        | 错误数据或故障                 | Coherency / Cache Sync    |
| DMA Bypass             | Secure Memory 泄露        | IOMMU/IOPMP/Firewall      |
| Resource Exhaustion    | TEE 服务不可用               | 配额、超时、资源回收                |

对于 Staff Engineer 来说，理解这些漏洞的根本原因，比记住几个具体的 CVE 更重要。

# 14.11 把整章知识串联起来：一次真实的 Shared Memory + RPC 调用

现在将前面十节整合为一条执行链。

假设 CA 要打开一个尚未加载的 TA，然后通过共享内存传入一个数据块。

TA OpenSession + RPC + Invoke 组合流程

通用概念执行图。Memory Registration、TEE_CALL、Yield/Resume 的精确 ABI 取决于所选 SMC-Compatible 或 RPMI Backend；不能视为某个未合并 RFC 的实测时序。

这条链中存在四个关键的安全检查点。

Check Point 1：共享内存注册

确认 Shared Memory 只引用被允许共享的物理页面，且对象的身份、大小和生命周期正确。

Check Point 2：OP-TEE 处理请求

验证 Message ABI、参数数量、类型、Offset、Size 及 Session 权限。

Check Point 3：RPC 返回

检查 Supplicant 返回的数据及内存对象引用，不信任任何来自 REE 的数据。

Check Point 4：Trusted Thread Resume

确认恢复请求匹配正确的 Suspended Thread、Domain 和调用生命周期。

只要其中一个环节存在漏洞，即使其他三个环节都正确，整体安全性也可能被破坏。

# 14.12 源码阅读与实操实验

建议使用已经能够启动 OP-TEE 和 xtest 的 QEMU 环境完成实验。

下面给出四个由浅入深的实验，最后一个可以直接演变成 Upstream Test Contribution。

## 实验 A：追踪 Dynamic Shared Memory 注册

目标：弄清楚 Linux `tee_shm` 如何变成 OP-TEE `mobj`。

首先在 Linux 源码中定位：

```
cd linux

git grep -n "tee_shm_register_user_buf"
git grep -n "optee_shm_register"
git grep -n "OPTEE_MSG_CMD_REGISTER_SHM"
git grep -n "optee_fill_pages_list"
```

然后在 `optee_os` 中：

```
cd ../optee_os

git grep -n "mobj_reg_shm_alloc"
git grep -n "mobj_reg_shm_get_by_cookie"
git grep -n "set_rmem_param"
git grep -n "OPTEE_MSG_CMD_REGISTER_SHM"
```

需要绘制的数据关系是：

```
Linux User VA
    |
    v
Linux Backing Pages
    |
    v
struct tee_shm
    |
    v
Page List / Registration Message
    |
    v
OP-TEE Registration Handler
    |
    v
struct mobj_reg_shm
    |
    v
TA Memory Reference
```

建议使用 GDB 或临时 Trace 记录：

| 字段              | Linux 侧                | OP-TEE 侧              |
| --------------- | ---------------------- | --------------------- |
| Buffer Size     | `tee_shm` Size         | `mobj->size`          |
| Physical Pages  | Backing Page Array     | `pages[]`             |
| Shared Identity | Registration Reference | Cookie / MOBJ         |
| Buffer Offset   | Page Offset            | `page_offset`         |
| Lifecycle       | Allocated/Registered   | Registered/Referenced |

验收标准：能够解释为什么 Linux 和 OP-TEE 的虚拟地址不同，但最终访问同一组物理页。

## 实验 B：追踪一次真实 RPC

目标：理解 RPC Suspend → Linux Service → Resume。

在 OP-TEE 中重点跟踪：

```
core/kernel/
core/tee/tee_ree_fs.c
core/tee/tee_fs_rpc.c
```

在 Linux 中：

```
drivers/tee/optee/smc_abi.c
drivers/tee/optee/rpc.c
```

在 Supplicant 中：

```
optee_client/tee-supplicant/src/tee_supplicant.c
```

建议使用以下事件记录格式：

```
EVENT 01:
    Linux OP-TEE call
    Thread identity:
    Command:
    Shared object:

EVENT 02:
    OP-TEE RPC Exit
    Trusted Thread ID:
    Thread state:
    RPC Command:

EVENT 03:
    Linux RPC Handling
    Request ID:
    Service:
    Result:

EVENT 04:
    OP-TEE RPC Resume
    Trusted Thread ID:
    Resume state:

EVENT 05:
    Original command completes
```

注意：不同 ABI 的 Thread ID、Resume Token 与调用方身份表示不同，日志中应该记录实际存在的字段。

验收标准：证明 RPC 恢复的是原来的 Trusted Thread 执行状态，而不是重新执行整个 TA Command。

## 实验 C：测试恶意 Shared Memory 参数

目标：验证参数解析和内存对象校验的安全性。

可以构建一个专门的测试驱动或受控测试程序，向 OP-TEE 提交：

| 测试                             | 输入                 | 期望       |
| ------------------------------ | ------------------ | -------- |
| Invalid Handle                 | 不存在的 SHM Reference | 拒绝       |
| Out-of-Range                   | Offset 超出 MOBJ     | 拒绝       |
| Overflow                       | Offset + Size 溢出   | 拒绝       |
| Invalid Physical Page          | 超出合法 Non-secure 区域 | 拒绝       |
| Double Unregister              | 同一对象重复注销           | 安全处理     |
| Active Reference               | 仍被 Thread 使用时注销    | 正确协调生命周期 |
| Concurrent Register/Unregister | 多线程快速执行            | 无内存破坏或死锁 |

不建议直接在生产设备上运行这些故障注入。

尤其在测试内存访问错误时，要区分：

- 返回 `TEE_ERROR_BAD_PARAMETERS`；
- 返回 `TEE_ERROR_SECURITY`；
- 安全地终止当前请求；
- 意外 OP-TEE Panic。

不同测试的正确错误码可能不同，但不应造成不可控的安全内存破坏。

## 实验 D：构建 Shared Memory + RPC Stress Test

这是我最推荐你尝试转化成 upstream patch 的实验。

设计三个并发负载：

```
Worker A:
    Register SHM
    Invoke TA
    Unregister SHM

Worker B:
    OpenSession
    Trigger RPC
    CloseSession

Worker C:
    Register / Unregister
    Change buffer sizes
    Exercise boundary values
```

然后使用多个 Linux CPU 和多个 CA Worker 同时执行。

测量：

- Shared Memory 注册耗时；
- RPC Round-trip Latency；
- TA Invoke Latency；
- Trusted Thread 使用率；
- TEE Call Queue 等待；
- MOBJ 引用数量；
- 对象释放后的残留映射；
- 失败请求的资源清理正确性。

对于 RISC-V 平台，还应该增加：

- 多 hart Domain Switch；
- RPC 延迟和重入；
- 非一致性 Cache 配置验证；
- Memory Parcel Reclaim 并发测试。

这个测试项目很有价值，因为它能够同时检验 Chapter 06 的 Domain Switch、Chapter 10 的 Trusted Thread、Chapter 11 的 MMU 和本章的 Shared Memory/RPC。

# 14.13 Staff Engineer 级面试与架构设计题

下面给出 12 道题，侧重真实系统故障、安全边界和设计取舍。每道附参考答案与考察重点。

## Q1. 为什么 Linux 已经知道一块用户 Buffer 的物理地址，OP-TEE 仍然需要 Dynamic SHM Registration？

场景

Linux Driver 能通过页表获得某个 User VA 对应的物理页列表。有人建议直接将这些物理地址传递给 OP-TEE，以减少注册开销。

参考答案

物理地址只回答了“内存在哪里”，没有回答“这块内存是否允许共享、当前属于谁、生命周期何时结束”。

OP-TEE 不能信任 REE 提供的任意 PA。注册过程至少需要建立合法物理页集合、对象身份、Size、Offset、References 和相关安全属性。

如果允许每次 Invoke 直接提交任意 PA，攻击者可能把 Secure RAM、设备 MMIO 或受保护内存地址伪装成普通数据 Buffer。

此外，Linux 可能释放、重新分配或解除用户页映射。注册过程还承担了内存引用与生命周期管理职责。

考察重点： Physical Address Validity、Memory Ownership、Authorization 和 Lifetime 是不同的问题。

## Q2. Dynamic SHM 为什么可能比 Static SHM 性能更高？是否总应该启用 Zero-copy？

场景

一个加密 TA 每秒处理数千次 1 MiB Buffer。性能分析发现大部分时间花在数据复制上。

参考答案

Dynamic SHM 可以直接注册已有的 Linux 用户页，避免将数据额外复制到 Reserved SHM Pool。

但注册本身有成本，包括 User Page Pinning、Page List 构造、内存对象创建以及可能的 Cache Synchronization。

对于小 Buffer、短时请求，注册开销可能抵消 Zero-copy 收益。

还必须考虑安全问题：如果 TA 在共享内存上直接解析安全敏感数据，REE 可并发修改输入。

因此应按照 Buffer Size、复用次数、认证需求和安全边界选择：

- 小型请求：临时 Buffer 或 Copy；
- 大型非敏感流式处理：考虑 Zero-copy；
- 安全敏感且需要稳定快照的数据：复制或建立受保护的内存访问语义。

真正的优化目标不是消除一切复制，而是降低满足安全要求后的总成本。

## Q3. Linux 在 OP-TEE 使用某个 MOBJ 时请求 Unregister，应如何保证安全？

场景

两个 Trusted Thread 同时引用共享内存 X，Linux 尝试注销 X。

参考答案

必须把对象分成“是否允许新增引用”和“是否仍有已有引用”两个维度。

合理的设计是将对象转入 Releasing 状态，阻止新引用，并等待已有引用完成。

对于非同步等待的方案，也可以返回 Busy，并由调用方负责重试。

必须保证引用获取与 Releasing 状态变化是同步的，不能出现：

```
Thread A sees REGISTERED
Thread B frees MOBJ
Thread A increments stale refcount
```

对于 Memory Parcel，还需要将 OP-TEE Release 与 Linux Reclaim 排序，并处理映射撤销及 TLB 同步。

考察重点： Reference Count 并不能单独解决生命周期竞态，还需要明确状态转换和同步不变量。

## Q4. 你在 Palladium 上遇到 OP-TEE 偶发读取旧数据，应该如何定位？

场景

同一套代码在 QEMU 中完全正常，在硬件仿真环境中高负载下偶发 SHA-256 错误。

参考答案

首先不要直接在 OP-TEE 中加入大量 `FENCE`。

应当区分五个方向：

1. SHM 引用的 PA 是否正确；
2. Linux/OP-TEE 是否使用兼容的内存属性；
3. CPU/CPU 或 CPU/DMA 是否处于一致性域；
4. Memory Ordering 是否正确；
5. Buffer 是否在使用期间被重新分配或修改。

然后记录端到端 Buffer ID、物理页列表、Sequence Number、Size 和关键数据校验值。

如果怀疑 Cache，可以在明确的 Ownership Transfer 位置进行平台要求的同步实验。

如果加入 Cache Sync 后问题消失，仍需进一步证明当前平台的硬件一致性行为及所需同步操作。

考察重点： 能够把 Cache Coherency、Ordering、Address Mapping 和 Data Race 分开诊断。

## Q5. 如果 Linux 已经被 Root 攻破，OP-TEE RPC 是否仍然安全？

场景

攻击者拥有 Linux root 权限，可以修改 `tee-supplicant`、文件系统和 Shared Memory，但无法直接访问 OP-TEE Private RAM。

参考答案

OP-TEE 的硬件隔离仍然可能保障 Private RAM 的机密性。

但是 RPC 服务提供者已经不可信。

攻击者可以返回错误文件、旧文件、损坏的 TA Image、非法长度或者拒绝服务。

因此 TEE 必须：

- 对 RPC 返回的参数执行严格校验；
- 对 TA Image 执行认证；
- 对 Secure Storage 数据进行完整性验证；
- 对需要新鲜性的对象使用可信状态；
- 限制资源耗尽和异常输入影响。

同时需要承认一个安全边界：若 OP-TEE 依赖 REE 提供文件和 I/O，那么恶意 REE 通常可以破坏可用性。

考察重点： Confidentiality、Integrity、Freshness 和 Availability 的区别。

## Q6. 为什么 RPMI Memory Parcel ID 不能直接充当安全授权凭证？

场景

一个开发者建议：只要 Linux 传来的 Parcel ID 存在，OP-TEE 就允许访问对应的所有物理页。

参考答案

Parcel ID 主要是对象标识，不应自动成为访问授权。

必须确认 Parcel 所属 Endpoint、接收方权限、生命周期和内存范围。

还要防止 ID 复用造成 ABA 问题。

Nonce 或 Generation 可以减少旧引用错误匹配新对象的风险，但不能代替调用方身份及内存访问策略检查。

对于多个 TA 共享同一个 OP-TEE Domain 的情况，TA 级权限控制仍需要由 OP-TEE Kernel 执行。

考察重点： Identification 不等于 Authentication 或 Authorization。

## Q7. RPC Resume 时，如果 Linux 返回错误的 Thread ID 或 Resume Token，会发生什么？

场景

一个旧 RPC 的 Resume Token 被重放，而该 Trusted Thread 的 Slot 已被另一个请求复用。

参考答案

正确实现必须拒绝不匹配当前 Suspended Call 的 Resume 请求。

检查至少应覆盖：

- Token 或 Thread Identity；
- 调用者所属安全上下文；
- Thread 是否处于 Suspended 状态；
- Token 是否对应当前生命周期；
- 相关资源是否仍然有效。

如果只根据 Thread Slot Index 恢复，可能将错误寄存器、Stack 或 Session 状态恢复给另一个请求。

良好的设计应在释放或完成请求后使旧 Token 失效。

考察重点： Resume 是安全状态转换，而不仅是跳转到一个保存的 PC。

## Q8. 为什么 OP-TEE 不能在整个 RPC 期间简单保持某个 Global Lock？

场景

OP-TEE 持有一个全局文件系统锁，然后通过 RPC 请求 Linux 文件服务。Linux 处理请求时又触发另一个 TA 调用。

参考答案

此时可能出现跨执行环境的锁依赖环。

例如：

```
OP-TEE Thread A
    Holds Lock X
        |
        v
    Waits for Linux RPC

Linux RPC Handler
    |
    v
    Invokes TA B
        |
        v
    TA B waits for Lock X
```

如果 Linux RPC Handler 必须等待 TA B 完成，则形成死锁。

应分析实际锁的语义和作用域，尽可能避免跨 Yield 持有不必要的全局锁。

对于确实需要保留的资源状态，可以采用更细粒度锁、明确的请求状态机或异步处理机制。

但不能简单地要求所有 RPC 前释放所有锁，因为部分资源必须在整个请求期间保持有效。

考察重点： 跨 Domain Lock Dependency、Reentrancy 和资源生命周期。

## Q9. 在 RISC-V 平台中，`SFENCE.VMA` 能否替代 DMA Cache Maintenance？

参考答案

不能。

`SFENCE.VMA` 主要用于地址转换同步。

DMA Cache Maintenance 处理的是 CPU Cache 与外部 Bus Master 之间的数据可见性。

如果一个 DMA Controller 不具备硬件缓存一致性，仅执行 `SFENCE.VMA` 不会自动把 Dirty DCache Line 写回 DRAM。

相应的软件必须遵守硬件一致性模型，使用适当的 DMA API、Cache Block Operations 或平台缓存维护机制。

同样，`FENCE` 主要约束内存访问顺序，不能被简单视为 Data Cache Flush。

考察重点： Memory Translation、Memory Ordering、Cache Coherency 三者的区别。

## Q10. 设计一个支持 16 Hart 的 RISC-V OP-TEE Shared Memory 子系统，需要重点解决什么？

场景

16 个 hart 可以同时从 Linux Domain 进入 OP-TEE Domain。TEE 需要支持大量并发 TA Invoke 与 RPC。

参考答案

首先区分 Global State 和 Per-Hart State。

Global State 可能包括 Registered SHM Objects、共享对象哈希表、Global Memory Policy、RPC 资源等。

Per-Hart State 包括当前执行 Context、Domain Switch Context、Trap State 以及部分临时栈等。

重点问题包括：

- MOBJ 注册和注销的并发同步；
- Reference Count 与 Reclaim 竞态；
- Trusted Thread Pool 的并发使用；
- RPC 请求与返回的对应关系；
- SMP TLB Shootdown；
- IPI 和 Interrupt Routing；
- Shared Memory 的 Cache Coherency；
- Global Lock 的争用；
- Domain Switch 的 Hart-local 安全状态。

性能上，应避免每次数据访问都获取 Global Lock，而是通过对象级引用、局部映射和明确生命周期减少争用。

安全上，应验证任意两个 hart 交错执行时都不会出现非法内存访问或错误 Resume。

考察重点： 不只是会使用 Spinlock，还必须能够设计可伸缩的共享状态与安全不变量。

## Q11. TA 要验证并安装一个 100 MiB Firmware Image，你会怎样设计共享内存接口？

场景

设备内存有限，无法将完整 100 MiB 固件复制进 TEE Private RAM，但必须防止恶意 Linux 在验证后替换镜像。

参考答案

直接对 Mutable SHM 验证一次签名，然后再次从 SHM 安装固件是不安全的。

可以考虑一种带完整性绑定的流式设计：

1. 在可信侧验证固件 Manifest 和可信签名。
2. Manifest 绑定整个 Firmware Image 的 Hash 或 Merkle Root。
3. 分块读取 REE 提供的数据。
4. 对每块数据进行认证或校验。
5. 确保真正提交到安装目标的数据就是已验证的数据。
6. 记录安装进度，处理掉电和重试。
7. 在需要时结合可信 Rollback Counter 实现版本防回滚。

如果平台支持合适的 Memory Lending 和硬件写权限撤销，也可以研究将共享 Buffer 在验证/使用期间变为不可由 REE 修改。

但这种方案必须确保硬件实际强制执行权限，而不能只依赖软件状态。

考察重点： 在性能、内存预算和数据完整性之间进行架构设计，而非只依赖 `memcpy()`。

## Q12. 如何评审一个新的 RISC-V RPMI OP-TEE Backend？

场景

一个团队向 upstream 提交新的 RISC-V RPMI OP-TEE Backend，支持 Memory Parcel、Yield/Resume、RPC 和 Dynamic SHM。

你作为 Reviewer 应该关注哪些问题？

参考答案

我会把 Review 分成六个层次。

| 层次          | 关键 Review 问题                      |
| ----------- | --------------------------------- |
| ABI         | 是否版本化、可协商？参数是否有固定编码和字节序？          |
| Transport   | TEE_CALL、RPC、Resume 是否具有明确的状态机？   |
| Memory      | Parcel ID、Nonce、Offset、Size 如何验证？ |
| Lifecycle   | Share、Release、Reclaim、失败回滚是否一致？   |
| Security    | 是否绑定 Endpoint 身份？是否防重放、越界、TOCTOU？ |
| Engineering | 是否有 xtest、压力测试、故障注入和兼容性测试？        |

尤其要检查失败路径。

例如：

```
Parcel Created
    |
    v
TEE_CALL submitted
    |
    v
Domain Switch fails
```

此时谁负责释放 Parcel？

再例如：

```
TEE references Parcel
    |
    v
Linux process exits
    |
    v
SHM object cleanup begins
```

是否会提前释放物理页？

最后，还需要检查 Backend 是否将传输层与 OP-TEE Core 的协议语义清楚分离，避免在不同层重复维护对象状态。

考察重点： 能否作为 Cross-subsystem Reviewer，从 ABI、内存管理、并发、隔离和测试多个角度审查一个真实的系统架构。

# 14.14 本章核心知识总结

本章最重要的不是记住 `tee_shm` 或 `mobj` 的定义，而是建立以下完整逻辑：

第一层：跨域通信

Linux 通过 OP-TEE Message ABI 描述请求，Shared Memory 承载数据。

第二层：内存对象

Linux `tee_shm` 管理 REE 侧资源，OP-TEE `mobj` 管理 TEE 侧的内存描述和引用。

第三层：生命周期

注册、映射、访问、注销、释放必须遵守严格的顺序和引用关系。

第四层：RPC / Context

RPC 让 OP-TEE 暂停当前执行、请求 REE 服务，再恢复原来的 Trusted Thread。

第五层：硬件正确性

MMU、PMP、Cache Coherency、Memory Ordering、DMA Protection 必须协调工作。

第六层：安全边界

REE Shared Memory 始终属于潜在的非可信输入，必须防范越界、Double Fetch、TOCTOU 和生命周期攻击。

## 本章特别推荐精读的源码

| 源码                                                                                                                                | 最重要的研究内容                                    |
| --------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------- |
| [optee_msg.h](https://github.com/OP-TEE/optee_os/blob/master/core/include/optee_msg.h?utm_source=chatgpt.com)                     | Message ABI、TMEM、RMEM、FMEM                  |
| [mobj_dyn_shm.c](https://github.com/OP-TEE/optee_os/blob/master/core/mm/mobj_dyn_shm.c?utm_source=chatgpt.com)                    | Dynamic SHM、MOBJ、Cookie、Refcount            |
| [entry_std.c](https://github.com/OP-TEE/optee_os/blob/master/core/tee/entry_std.c?utm_source=chatgpt.com)                         | 参数解析、Session、Invoke                         |
| [tee_shm.c](https://github.com/torvalds/linux/blob/master/drivers/tee/tee_shm.c?utm_source=chatgpt.com)                           | Linux Shared Memory 管理                      |
| [smc_abi.c](https://github.com/torvalds/linux/blob/master/drivers/tee/optee/smc_abi.c?utm_source=chatgpt.com)                     | SHM 注册和经典 RPC 调用循环                          |
| [rpc.c](https://github.com/torvalds/linux/blob/master/drivers/tee/optee/rpc.c?utm_source=chatgpt.com)                             | Kernel RPC Handler 与 Supplicant 分流          |
| [tee_supplicant.c](https://github.com/OP-TEE/optee_client/blob/master/tee-supplicant/src/tee_supplicant.c?utm_source=chatgpt.com) | REE User Space RPC 服务                       |
| [RISC-V RPMI Backend RFC v2](https://lists.infradead.org/pipermail/linux-riscv/2026-October/100709.html?utm_source=chatgpt.com)   | Memory Parcel、Resume Token、新 RISC-V Backend |

最后，我希望你把本章浓缩成一个能够在架构设计评审中清楚阐述的结论：

> OP-TEE Shared Memory Architecture 的核心不是共享物理地址，而是在两个信任等级不同的执行环境之间，建立可验证的内存引用、明确的访问权限、受控的生命周期，以及可靠的同步执行模型。
>
> RPC 则在这个基础上提供安全执行的暂停与恢复能力，使 OP-TEE 能够利用 REE 的复杂服务，而不必将 REE 纳入可信计算基。

如果能够进一步在 RISC-V 平台上，将 `tee_shm`、RPMI Memory Parcel、OP-TEE `mobj`、Trusted Thread Resume 与 OpenSBI Domain Switch 的整个过程都追踪到代码与寄存器层面，那么你对这一章的掌握，就已经不仅是 OP-TEE API 使用者层级，而是具备了参与 OP-TEE Core、Linux TEE Driver 和 RISC-V RPMI Backend 跨子系统开发与 Review 的必要基础。
