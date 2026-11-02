# Chapter 12 — RISC-V User TA Execution & Architecture Context

RISC-V OP-TEE Architecture & Porting · 深度技术讲义 · Staff Engineer / Upstream Maintainer Level

## 本章定位与学习目标

前面我们已经研究了 OpenSBI Domain、RISC-V Trap/Exception、OP-TEE Trusted Thread 和 MMU。现在进入一个更加接近 OP-TEE 内核本质的问题：

当 OP-TEE Core 已经运行在 RISC-V S-mode，并且具备自己的内存管理能力之后，它究竟怎样加载、隔离、执行和管理一个运行在 U-mode 的 Trusted Application？

这并不是简单地调用一个函数。

OP-TEE 实际上需要完成一整套与操作系统进程管理类似的工作：

- 验证 TA 的身份和完整性。
- 解析 ELF，建立可执行的虚拟地址空间。
- 初始化代码段、数据段、栈、Heap、TLS 和动态链接环境。
- 从 S-mode 安全地进入 U-mode。
- 将 TA 的系统调用重新导向 OP-TEE Core。
- 维护 TA Instance、Session、Trusted Thread 和 CPU Context。
- 在 TA 切换或异常退出时确保敏感状态不发生泄露。
- 当 TA 发生异常时，限制错误的影响范围并清理资源。

本章要回答的核心问题是：

> 如何将一个不完全可信的 User TA，安全地运行在具有硬件特权隔离的 OP-TEE 执行环境中，并在多个 TA、多个 Trusted Thread 和多个 hart 之间保证正确性与隔离性？

这里需要特别强调：TA 的签名可信，不代表 TA 的运行时行为永远可信。 即使 TA 经认证且来自合法开发者，它依然可能包含内存越界、Use-After-Free、错误指针操作或其他安全漏洞。

因此，User TA 的权限必须始终低于 OP-TEE Core。

## 12.0 全局架构：先建立完整执行模型

RISC-V User TA 的执行层次

Linux CA / REE

TEEC_OpenSession · TEEC_InvokeCommand

OpenSBI / Domain Switching

Linux Domain → TEE Domain

OP-TEE Core — S-mode

TA Manager · ELF Loader Control · MMU · Syscall Handler

User TA A

U-mode

Private VA Space

User TA B

U-mode

Private VA Space

TA 访问受虚拟内存权限和 OP-TEE Core 的系统调用策略约束

Hardware Protection

Sv39/Sv48 · U/S PTE Permissions · PMP / SoC Isolation

这里存在两层完全不同的隔离关系。

第一层：Linux Domain ↔ OP-TEE Domain

在我们讨论的 OpenSBI Domain 部署模型中，由 M-mode Firmware 和平台硬件保护机制负责建立这一层隔离。

第二层：OP-TEE Core ↔ User TA，以及 User TA A ↔ User TA B

这是 OP-TEE 内部的隔离，主要依靠 S/U Privilege、MMU 地址空间、页表权限和内核资源管理。

它们不可混为一谈。

例如，Linux 与 OP-TEE 之间的 Domain Switch，通常需要 OpenSBI 参与；但是一次普通的 TA U-mode → OP-TEE S-mode 系统调用，只需通过已配置的 RISC-V Trap Delegation 进入 OP-TEE 内核，不必每次都切换 OpenSBI Domain。

这就是本章最重要的架构前提。

### 三种 Context 必须区分

| Context                | 管理主体                                         | 主要职责                   |
| ---------------------- | -------------------------------------------- | ---------------------- |
| Domain Context         | OpenSBI / M-mode Firmware                    | Linux 与 OP-TEE 安全执行域切换 |
| Trusted Thread Context | OP-TEE Core                                  | 保存安全内核执行线程的状态          |
| User TA Context        | OP-TEE TA Manager / MMU / Architecture Layer | 保存 TA 地址空间、执行状态及资源     |

需要再补充一点：TA Context、TA Session 和 User CPU Register Context 不是同一个对象。

TA Context 描述 TA 的运行实例；Session 描述一次客户端与 TA 之间的逻辑连接；CPU 寄存器现场则属于某次具体的执行路径。

后面我们将一直利用这个区分来分析 Session、Syscall、FP/Vector 和 SMP。

本章按照此前定义的十个知识点展开，并在最后增加源码实验、故障案例和 Staff Engineer 级综合面试题。源码以 2026 年 10 月可查的 OP-TEE upstream 和 RISE RP022 项目为参考；部分机制会用教学伪代码解释，而不会将示意代码误称为 upstream 原样实现。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE+3



## 12.1 RISC-V User TA：ELF、PIE 与 Loader 设计

### 12.1.1 为什么 TA 需要自己的 ELF Loader？

我们先从 Linux 的进程执行来理解。

当 Linux 执行：

```
execve("/usr/bin/example", argv, envp);
```

内核负责创建执行环境，包括映射 ELF Segment、准备用户栈、初始化执行上下文，并在需要时启动动态链接器。

OP-TEE User TA 也需要这些工作。

但这里存在一个关键区别：

Linux ELF Loader 不能替 OP-TEE 决定可信内存的映射。

因为 Linux 是 REE 中的一部分，不处于 OP-TEE 的可信边界内。如果把 TA 的地址空间建立过程完全交给 Linux，Linux 就可能控制 TA 的代码映射或修改敏感数据。

因此，OP-TEE 使用自己的 User-mode ELF Loader：

`ldelf`

它负责解析和装载 TA ELF，在 OP-TEE Core 提供的受控接口下建立 TA 映射、加载依赖库以及处理重定位。

官方架构文档明确指出，不同来源的 User TA 最终都会通过 `ldelf` 的共同加载接口准备执行。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### 12.1.2 TA 从哪里加载？

OP-TEE 支持多种 User TA 来源：

| TA 类型             | 存储位置               | 核心特点                                |
| ----------------- | ------------------ | ----------------------------------- |
| REE FS TA         | Linux 文件系统         | 签名认证，可选加密，通常由 `tee-supplicant` 提供文件 |
| Early TA          | OP-TEE Core 镜像内部   | 在 REE 文件系统可用之前即可访问                  |
| Secure Storage TA | TA 数据库存储及受保护的持久化存储 | 有专用的安装和管理生命周期                       |

这些 TA 最终进入 User TA 执行环境时，仍要经过 OP-TEE 的加载和内存管理机制。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里有一个很重要的安全问题：

REE FS 中的 TA 文件来自不可信 Linux，为什么 OP-TEE 还敢加载它？

因为文件存放位置和可信身份认证是两回事。

REE 提供的是文件内容，而 OP-TEE 验证的是内容是否符合自己信任的签名策略。

实际设计可以概括为：

```
Linux filesystem
       |
       v
tee-supplicant
       |
       | Untrusted TA bytes
       v
OP-TEE Core
       |
       +-- Parse signed header
       +-- Verify signature / authenticity
       +-- Verify image integrity
       +-- Optional decryption
       +-- Check TA identity / policy
       |
       v
ldelf
       |
       +-- Parse ELF
       +-- Create VM mappings
       +-- Relocate
       |
       v
Executable User TA
```

签名保证的是经过可信验证流程所认可的真实性和完整性，不提供机密性。 如果需要保护 TA 文件的内容，还需要加密；如果要防止旧版合法签名镜像被回放，还需要可靠的版本状态与回滚保护机制。

例如，OP-TEE 文档指出 REE-FS TA 的版本回滚保护依赖相应的 RPMB FS 配置，而不是仅靠 TA 签名。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### 12.1.3 为什么 RISC-V TA 应使用 Position-Independent Executable？

核心原因是：

TA 不应该被强制装载到唯一固定的虚拟地址。

假设同一个 TA ELF 的链接基准地址为零，而 Loader 为其选定：

\\[ B=0x0000000040000000 \\]

那么某个 ELF Segment 中的虚拟地址偏移为：

\\[ V\_{\text{segment}}=0x2000 \\]

实际映射位置应为：

\\[ VA\_{\text{segment}}=B+V\_{\text{segment}} =0x0000000040002000 \\]

如果某个可重定位符号的偏移为 `0x3100`，则最终地址为：

\\[ VA\_{\text{symbol}}=B+0x3100 \\]

这就是 Position-Independent Code 配合 Relocation 的基本思想。

采用 PIE 还可以让 TA ASLR 成为可能。

OP-TEE 当前 RISC-V TA Linker Script 位于：

`ta/arch/riscv/ta.ld.S`

与普通 RV64 程序不同的是，TA 需要兼容 OP-TEE 自己的加载器、TA Header、动态链接与用户态 ABI。官方 ASLR 文档也明确说明 RISC-V User TA 采用位置无关代码。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



### 12.1.4 ELF 文件中最重要的两个结构

第一是 ELF Header：

```
typedef struct {
    unsigned char e_ident[16];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off  e_phoff;
    /* ... */
} Elf64_Ehdr;
```

其中：

- `e_machine` 应匹配 RISC-V。
- `e_type` 决定文件类型，例如 `ET_DYN`。
- `e_entry` 是 ELF 入口的虚拟地址表达。
- `e_phoff` 指向 Program Header Table。

第二是 Program Header：

```
typedef struct {
    Elf64_Word  p_type;
    Elf64_Word  p_flags;
    Elf64_Off   p_offset;
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;
```

在 TA 装载过程中，最重要的是 `PT_LOAD` Segment。

例如：

| Segment      | ELF 属性                    | 典型运行时权限 |
| ------------ | ------------------------- | ------- |
| `.text`      | Executable Code           | RX      |
| `.rodata`    | Read-only Data            | R       |
| `.data`      | Initialized Writable Data | RW      |
| `.bss`       | Zero Initialized Data     | RW      |
| Stack / Heap | Runtime Allocations       | RW      |

注意，这只是典型权限设计，不代表所有 ELF Section 都与某个 `PT_LOAD` 一一对应。

真正用于建立内存映射的是 Program Header 描述的 Segment，而不是简单地为每一个 Section 创建映射。

并且，生产级 Loader 还要正确处理 Segment 重叠、对齐、整数溢出、权限合并和 W^X 等安全问题。

### 12.1.5 TA 的 ELF Relocation

RISC-V 常见的 Relocation 包括：

- `R_RISCV_RELATIVE`
- `R_RISCV_64`
- `R_RISCV_JUMP_SLOT`
- TLS 相关 Relocation

具体支持范围取决于 OP-TEE 的 `ldelf` 实现、工具链及构建配置，而不是由 RISC-V ELF 规范支持某种 Relocation 就意味着 OP-TEE Loader 必然支持它。

以 `R_RISCV_RELATIVE` 为例，其典型计算公式为：

\\[ \text{relocated value}=B+A \\]

其中：

- \\(B\\) 是 Loader 选定的 Load Base。
- \\(A\\) 是 Relocation Addend。

假设 `B = 0x40000000`、`A = 0x2800`，那么最终写入的地址是 `0x40002800`。

它不需要通过符号查找确定目标地址，因此非常适合 PIE 中的相对地址修正。

这里还有一个安全问题：Relocation 会写入目标内存。

因此 Loader 必须先校验目标地址、目标大小和目标 Segment；对于只读或可执行页面，要使用受控的初始化及权限转换流程，而不能长期留下任意 RWX 映射。

源码入口：

- `ldelf/ta_elf.c`
- `ldelf/ldelf.c`
- `ta/arch/riscv/ta.ld.S`

实际 `ldelf/ta_elf.c` 中包含 Segment Loading、Relocation、映射及最终入口确定逻辑。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 12.2 OP-TEE Core 如何进入 RISC-V U-mode 执行 TA？

这是本章第一部分最重要的底层机制。

前面已经知道，TA ELF 最终被加载进受控的虚拟地址空间。但是：

Loader 将指令放进内存，并不意味着 CPU 就已经开始执行 TA。

OP-TEE 还必须显式准备 CPU 执行现场，并完成：

\\[ \text{S-mode}\rightarrow\text{U-mode} \\]

### 12.2.1 为什么进入 U-mode 需要 `sret`？

先回顾 RISC-V 的三个关键指令：

| 指令      | 含义                | 典型作用                            |
| ------- | ----------------- | ------------------------------- |
| `ecall` | Environment Call  | 从当前执行环境请求更高权限的软件服务              |
| `sret`  | Supervisor Return | 按 Supervisor Trap Return 状态恢复执行 |
| `mret`  | Machine Return    | 按 Machine Trap Return 状态恢复执行    |

其中，`sret` 并非只能返回 S-mode。

它根据 `sstatus.SPP` 确定返回后的特权级别。

在标准 S/U 特权模型中：

- `SPP = 0`：返回 U-mode。
- `SPP = 1`：返回 S-mode。

同时，`sepc` 提供 Trap Return 后的 PC。

因此，只要正确准备：

```
sepc           = TA_ENTRY_ADDRESS
sstatus.SPP    = 0
sstatus.SPIE   = desired interrupt-enable state
```

执行：

```
sret
```

CPU 就能够跳转到指定入口并进入 U-mode。

但这只是架构层面的最简解释。真正的 OP-TEE 还必须准备完整的用户寄存器、栈、内存映射和 Trap 入口。

### 12.2.2 从实际 upstream 函数开始分析

当前 OP-TEE RISC-V 的关键源码：

`core/arch/riscv/kernel/thread_arch.c`

其中有一个直接相关的函数：

```
uint32_t thread_enter_user_mode(
    unsigned long a0,
    unsigned long a1,
    unsigned long a2,
    unsigned long a3,
    unsigned long user_sp,
    unsigned long entry_func,
    bool is_32bit,
    uint32_t *exit_status0,
    uint32_t *exit_status1);
```

其核心逻辑经过简化后是：

```
/* 教学节选：省略中断与运行时间统计 */

regs = thread_get_ctx_regs();

status = xstatus_for_xret(true, PRV_U);

set_ctx_regs(regs,
             a0, a1, a2, a3,
             user_sp,
             entry_func,
             status,
             ie,
             NULL);

rc = __thread_enter_user_mode(
    regs, exit_status0, exit_status1);
```

这是对 upstream 真实实现的节选与整理。`xstatus_for_xret(true, PRV_U)` 准备的是返回 U-mode 的状态，随后通过架构汇编函数完成真正的切换。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们需要特别理解 `regs`。

这个对象不是普通的函数参数集合，而是即将装入 CPU 的执行现场。

可以抽象为：

```
/* 教学模型，非 upstream 完整结构体声明 */
struct user_entry_context {
    unsigned long a0;
    unsigned long a1;
    unsigned long a2;
    unsigned long a3;

    unsigned long sp;
    unsigned long gp;
    unsigned long tp;

    unsigned long epc;
    unsigned long status;
    unsigned long ie;
};
```

这些字段决定 User TA 第一次执行时看到的状态。

### 12.2.3 `thread_enter_user_mode()` 的参数从哪里来？

实际调用点位于：

`core/kernel/user_ta.c`

核心逻辑：

```
res = thread_enter_user_mode(
    func,
    kaddr_to_uref(session),
    (vaddr_t)usr_params,
    cmd,
    usr_stack,
    utc->uctx.entry_func,
    utc->uctx.is_32bit,
    &utc->ta_ctx.panicked,
    &utc->ta_ctx.panic_code);
```

这里的函数是 `user_ta_enter()`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们把参数展开：

| CPU 寄存器 / 参数 | 含义                        |
| ------------ | ------------------------- |
| `a0`         | User TA Entry Function 类型 |
| `a1`         | Session 的用户态引用            |
| `a2`         | User TA 参数结构地址            |
| `a3`         | Command ID                |
| `sp`         | TA User Stack             |
| `sepc`       | User TA Entry Address     |
| `sstatus`    | 准备返回 U-mode 的状态           |

值得注意的是，OP-TEE 并不是简单调用 `TA_InvokeCommandEntryPoint()`。

首先运行的是 TA Runtime Entry。

这个 Runtime Entry 负责根据入口功能类型，将请求分发到具体的 GP TA Entry Point。

例如：

```
OP-TEE Core
    |
    | a0 = UTEE_ENTRY_FUNC_INVOKE_COMMAND
    | a1 = Session Reference
    | a2 = Parameters
    | a3 = Command ID
    v
User TA Runtime Entry
    |
    | Decode entry function
    v
TA_InvokeCommandEntryPoint()
```

这和 Linux 中从 `execve()` 启动 ELF 后先进入 C Runtime，再调用 `main()` 的设计思想相似。

### 12.2.4 真正的汇编切换：`__thread_enter_user_mode`

现在进入最有价值的代码。

源码：

`core/arch/riscv/kernel/thread_rv.S`

当前实现的核心执行顺序如下：

```
/* 关键操作的教学节选，省略部分寄存器保存 */

FUNC __thread_enter_user_mode, :

    /* 保存内核返回现场 */
    /* 保存当前 Trusted Thread 的 kernel SP */

    /* 设置 Trap Entry 所需的 core-local 指针 */
    csrw CSR_XSCRATCH, tp

    /* regs 指向待恢复的 User CPU Context */
    mv sp, a0

    /* 加载返回执行地址 */
    load_xregs sp, THREAD_CTX_REG_EPC, REG_S0
    csrw CSR_XEPC, s0

    /* 加载准备好的状态 */
    load_xregs sp, THREAD_CTX_REG_STATUS, REG_S0
    write_xstatus_keep_fs s0, t1

    /* 加载中断使能状态 */
    load_xregs sp, THREAD_CTX_REG_IE, REG_S0
    csrw CSR_XIE, s0

    /* 恢复用户态 GPR，包括 gp、tp、sp */

    XRET
END_FUNC
```

`XRET` 是 OP-TEE 的架构抽象宏，在标准 RISC-V S-mode 构建中对应 Supervisor Return 指令；源码也支持其他构建模式，因此使用了 `X*` 命名。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这里有三个特别容易被忽略的细节。

细节一：`sp` 并不始终表示相同的栈。

在准备 Context 时，`sp` 暂时用于访问内存中的 Register Frame。

在最后恢复 User SP 后，`sp` 才真正变成 TA 的 User Stack Pointer。

因此源码有一个十分重要的约束：必须在最后恢复 `sp`。

细节二：必须保存内核的返回现场。

当 TA 完成执行后，OP-TEE 还需要返回先前的 C 函数调用位置。

因此，`__thread_enter_user_mode()` 不只是单向跳转 U-mode。它还建立了随后恢复内核函数调用的执行现场。

细节三：`CSR_XSCRATCH` 不是可随意复用的临时寄存器。

它参与了后续 U-mode Trap Entry 的 per-hart 状态恢复，我们下一节会详细分析。

### 12.2.5 建立一个正确的 Stack 模型

假设 CPU 当前位于 OP-TEE S-mode：

```
OP-TEE Trusted Thread Kernel Stack
High Address
+-----------------------------+
| Older Kernel Call Frames    |
+-----------------------------+
| user_ta_enter() frame       |
+-----------------------------+
| thread_enter_user_mode()    |
+-----------------------------+
| User Return Record          |
|  - saved kernel ra          |
|  - saved kernel s0-s11      |
|  - context pointer          |
+-----------------------------+
Low Address
```

而 TA 使用的是另外一块用户栈：

```
User TA Stack
High Address
+-----------------------------+
| TA Stack Top                |
+-----------------------------+
| utee_params                 |
+-----------------------------+
| TA Runtime Frame            |
+-----------------------------+
| TA_InvokeCommandEntryPoint  |
| Local Variables             |
+-----------------------------+
Low Address
```

两块栈在逻辑上独立，不是把同一块 Stack 的 SP 调整一下就完成了 Privilege Switch。

当 TA 调用 `ecall` 时，OP-TEE 会利用 Trusted Thread 保存的内核 SP，切换回 S-mode 可使用的内核栈。

这既是执行正确性的要求，也是安全隔离的重要组成部分。

## 12.3 User TA 的 Stack、TLS、GP/TP 与 RISC-V ABI

这一节非常重要，因为你已经熟悉 AArch64 的 `SP_EL0`、`SP_EL1`、`TPIDR_EL0` 等机制。

RISC-V 的设计不同。

### 12.3.1 RISC-V 不提供独立的 S-mode 和 U-mode SP

在 AArch64 中，软件可以利用不同的 SP 寄存器或异常级别相关机制安排栈。

而 RISC-V 通用整数寄存器只有一个：

```
x2 = sp
```

发生 U-mode → S-mode Trap 时：

- CPU 保存并更新必要的 Trap CSR。
- CPU 不会自动切换到一块特定的 S-mode Stack。
- CPU 也不会自动保存所有 GPR。

因此，Trap Handler 必须自行处理 User SP、Kernel SP 和寄存器现场。

这就是为什么 `thread_rv.S` 的 Trap Entry 是高度依赖汇编的代码。

### 12.3.2 `gp` 的用途是什么？

在 RISC-V psABI 中：

```
x3 = gp = Global Pointer
```

`gp` 可以配合编译器和链接器处理 Global Data 访问，特别是与 small-data 及相关寻址优化有关的场景。

假设 OP-TEE Kernel 与 TA 各自有不同的全局数据布局：

```
OP-TEE Kernel
    gp -> Kernel Global Data

User TA
    gp -> TA Runtime / Global Data
```

如果从 U-mode Trap 返回内核后仍然保留 User `gp`，内核中的 Global Data 访问就可能错误。

因此需要恢复正确的 Kernel GP。

实际 RISC-V Trap 代码确实包含重新加载 `__global_pointer$` 的操作。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 12.3.3 `tp` 为什么更加关键？

在 RISC-V psABI 中：

```
x4 = tp = Thread Pointer
```

用户态通常利用它定位 Thread-Local Storage。

但是在 OP-TEE RISC-V Kernel 中，`tp` 还被用于访问：

```
struct thread_core_local
```

也就是 Hart-local / Core-local 状态。

这意味着：

```
Kernel tp → thread_core_local

User tp   → User TA TLS
```

它们承担不同职责。

不能因为二者都叫 Thread Pointer，就把它们当成同一套 TLS 实现。

### 12.3.4 `sscratch` 与 `tp` 交换：理解 RISC-V Trap 的关键

当前 OP-TEE RISC-V Trap Vector 的开头：

```
FUNC thread_trap_vect, :

    csrrw tp, CSR_XSCRATCH, tp

    bnez tp, 0f

    csrrw tp, CSR_XSCRATCH, tp

    j trap_from_kernel

0:
    j trap_from_user

END_FUNC
```

这是从真实 upstream 中摘取的核心指令。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



理解它需要知道一个前提：

当运行 User TA 时：

```
tp       = User Thread Pointer
sscratch = Kernel thread_core_local pointer
```

假设：

```
User tp       = 0x40008000
sscratch      = 0xFFFFFFC000010000
```

执行：

```
csrrw tp, sscratch, tp
```

之后：

```
tp            = 0xFFFFFFC000010000
sscratch      = 0x40008000
```

所以 Trap Handler 一开始就能够获得 Kernel Core-local Pointer，同时暂时把原 User TP 保存在 CSR 中。

这是一种非常简洁的汇编设计。

它利用一条 CSR 原子交换指令，完成：

1. 获得 Kernel TLS / Per-Hart State。
2. 暂存 User TP。
3. 为接下来区分 Trap Origin 提供条件。

如果 Trap 原本来自 S-mode Kernel，代码使用 `sscratch = 0` 的约定进行区分，并恢复 Kernel `tp`。

因此：

`sscratch` 在这里不只是一个 Scratch Register，而是 Trap Entry 协议的一部分。

### 12.3.5 TLS 不能只保存 `tp`

如果 TA 使用：

```
static __thread int counter;
```

仅仅保存 `tp` 不足以保证 TLS 正确。

还需要保证：

- TA 的 TLS Block 已经正确分配和初始化。
- ELF TLS Segment 和动态库 TLS 布局正确。
- Runtime 的 TLS Address Calculation 与 RISC-V psABI 匹配。
- 不同 TA Instance 的 TLS 数据互不混淆。
- Session 和 Invocation 是否共享执行上下文符合运行时设计。
- Trap Entry、Syscall 和 Context Restore 不会破坏 `tp`。

这里需要特别避免一个误解：TA Instance 不等价于 Linux pthread，Session 也不天然拥有独立的 TLS Block。

TLS 的生命周期必须按照实际 OP-TEE User Runtime 实现理解。

如果同一个 TA Instance 可以接收多个 Session，那么不能简单假设 `__thread` 数据就是每个 Session 各自独立的存储。

### 12.3.6 Stack Alignment 与 ABI

RISC-V RV64 常用的标准 ABI 为 LP64 系列。

在标准 ABI 约定下，栈应遵守 16-byte 对齐要求。

例如：

```
SP = 0x40010000  ✓
SP = 0x4000FFF0  ✓
SP = 0x4000FFF8  ✗
```

进入 TA Runtime 前，OP-TEE 要预留用户参数空间：

```
usr_stack = utc->uctx.stack_ptr;

usr_stack -= ROUNDUP(
    sizeof(struct utee_params),
    STACK_ALIGNMENT);
```

这正是 `user_ta_enter()` 中实际存在的行为。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



另外需要关注：

- Stack Overflow Protection。
- Guard Page。
- Stack Canary。
- 用户栈与 Kernel Stack 的映射权限。
- User Stack Pointer 是否落在有效地址空间内。

一旦这些设计存在缺陷，TA 就可能不只是 Crash，还可能触发越权内存访问。

## 12.4 TA 如何通过 Syscall 进入 S-mode OP-TEE？

这是本章最重要的执行路径之一。

### 12.4.1 `TEE_*` API 并不等于 Syscall

假设 User TA 调用：

```
TEE_GenerateRandom(buffer, 32);
```

TA 所调用的是 GlobalPlatform TEE Internal Core API。

在 OP-TEE 中，许多 Internal API 由 User Space Library `libutee` 实现。

其调用可能继续进入一个底层 `_utee_*` 接口，并触发 OP-TEE 特有的系统调用。

例如：

```
User TA
   |
   | TEE_GenerateRandom()
   v
libutee
   |
   | _utee_cryp_random_number_generate()
   v
RISC-V User Syscall Wrapper
   |
   | ecall
   v
OP-TEE S-mode Trap Entry
   |
   v
Syscall Dispatcher
   |
   v
syscall_cryp_random_number_generate()
   |
   v
OP-TEE Crypto Subsystem / RNG Driver
```

这里不是所有 TEE API 都必须进入 S-mode。

例如某些参数处理、内存操作或者纯用户态运算可以在 `libutee` 内完成。

真正需要特权服务时才需要 Syscall。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



### 12.4.2 OP-TEE RISC-V Syscall ABI 与 Linux 不一样

这是一个特别值得记忆的重点。

Linux RISC-V Syscall 通常使用：

```
a7 = Linux syscall number
a0-a5 = arguments
```

但是 OP-TEE RISC-V 内部的 User TA Syscall ABI 并不是这个约定。

当前 upstream：

`core/arch/riscv/include/kernel/arch_scall.h`

明确实现：

```
static inline void scall_get_max_args(
    struct thread_scall_regs *regs,
    size_t *scn,
    size_t *max_args)
{
    *scn = regs->t0;
    *max_args = regs->t1;
}
```

返回值则设置到：

```
regs->a0 = ret_val;
```

因此：

| 寄存器    | OP-TEE RISC-V Syscall 语义 |
| ------ | ------------------------ |
| `t0`   | Syscall Number           |
| `t1`   | 参数数量                     |
| `a0` 等 | 参数及返回值传递                 |
| `sepc` | `ecall` 指令地址             |

这与 Linux ABI 不是一回事。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



为什么值得强调？

因为 `ecall` 是 ISA 定义的异常指令，但参数寄存器和服务编号是调用方与服务方约定的 ABI。

执行同样的 `ecall`，并不意味着调用相同的系统服务。

### 12.4.3 RISC-V `ecall` 的硬件行为

假设 TA 正在 U-mode 执行：

```
ecall
```

而且 U-mode ECALL 已经通过 `medeleg` 委托到 S-mode。

那么处理器执行 Trap 时将更新：

```
scause = 8          # Environment call from U-mode
sepc   = ECALL_PC
sstatus.SPP = 0
pc     = stvec
```

此外，硬件会按规范处理 `SIE/SPIE` 等相关状态。

要注意：

`sepc` 保存的是触发 `ecall` 的指令地址，不是其后一条指令的地址。

如果直接 `sret` 而不推进 `sepc`，TA 就会重复执行相同的 `ecall`。

### 12.4.4 从 OP-TEE 汇编分析 Syscall Entry

实际源码：

`core/arch/riscv/kernel/thread_rv.S`

进入 `trap_from_user` 后，判断是否为 User ECALL：

```
exception_from_user:

    li a1, CAUSE_USER_ECALL
    bne a0, a1, abort_from_user

ecall_from_user:
    /* Switch to kernel stack */
    /* Save syscall register frame */
    /* Call thread_scall_handler */
```

其中最重要的操作是：

```
get_thread_ctx a0, a1

load_xregs a0, THREAD_CTX_KERN_SP, REG_SP

addi sp, sp, -THREAD_SCALL_REGS_SIZE
```

这里会恢复当前 Trusted Thread 的 Kernel SP，并在内核栈上建立 `thread_scall_regs`。

随后保存调用参数及必要的寄存器，调用：

```
mv a0, sp
call thread_scall_handler
```

这条调用链可以准确地描述为：

```
User TA ecall
    |
    v
thread_trap_vect
    |
    v
trap_from_user
    |
    v
exception_from_user
    |
    v
ecall_from_user
    |
    v
thread_scall_handler
```

以上入口和 Kernel SP 切换都可以在实际汇编中逐项验证。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 12.4.5 `thread_scall_handler()` 如何分发？

当前 `thread_arch.c` 中的核心代码：

```
sess = ts_get_current_session();

assert(sess && sess->handle_scall);

if (sess->handle_scall(regs)) {
    regs->epc += 4;
} else {
    setup_unwind_user_mode(regs);
}
```

这里最值得注意的不是 `if` 本身，而是：

```
sess->handle_scall
```

OP-TEE 通过当前 Session 关联的 Handler，判断接下来的 Syscall 应由哪个处理器接管。

例如：

- 正常 User TA → `scall_handle_user_ta`
- ELF Loader → `scall_handle_ldelf`

为什么 Loader 和普通 TA 需要不同的 Syscall Table？

因为 Loader 需要一些特殊的内存映射和 ELF 装载服务，但普通 User TA 不应该获得完全相同的能力。

这是对 Least Privilege 原则的具体应用。

实际代码中：

`core/kernel/scall.c`

包含：

```
static const struct syscall_entry
    tee_syscall_table[] = { ... };

static const struct syscall_entry
    ldelf_syscall_table[] = { ... };
```

两者的服务范围不同。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



### 12.4.6 如何正确处理 `sepc`？

这里有两个场景。

普通 Syscall 返回：

```
regs->epc += 4;
```

然后恢复 User Context，通过 `sret` 返回 U-mode。

为什么加 4？

因为 RISC-V `ecall` 是 32-bit 指令，即使启用了 C Extension，它也不是一条 16-bit 压缩指令。

TA Return / Panic：

如果 Syscall 表示 TA 执行结束，那么就不能简单地返回 User PC。

OP-TEE 需要退出 User Execution，恢复先前的 S-mode C 调用。

这时会通过 `setup_unwind_user_mode()` 将 Trap Return 目标调整为内核恢复路径。

这与普通 Syscall Return 完全不同。

### 12.4.7 完整的 Syscall Return 汇编过程

调用处理结束后，需要：

1. 保存/恢复当前 Kernel SP 的关联状态。
2. 恢复 `sepc`。
3. 恢复 `sstatus`。
4. 恢复中断控制状态。
5. 判断最终返回 U-mode 还是 S-mode。
6. 为 U-mode 恢复 User GP、TP。
7. 恢复其他 GPR。
8. 执行 `sret`。

当前汇编针对返回 U-mode 的情况，还会在 `CSR_XSCRATCH` 中恢复 Kernel Core-local Pointer，以便下一次 User Trap 能正确找到内核上下文。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



这形成一个完整闭环：

```
U-mode TA
   |
   | ecall
   v
S-mode Trap Handler
   |
   | save User Context
   | switch Kernel Stack
   | dispatch Syscall
   | restore Context
   v
sret
   |
   v
U-mode TA continues
```

## 12.5 TA Session / Context Lifecycle

这是最容易与 Trusted Thread 混淆的知识点。

我们先将四个概念严格区分。

| 对象                    | 是什么                   | 生命周期                       |
| --------------------- | --------------------- | -------------------------- |
| TA Binary             | TA 的可执行映像及其元数据        | 文件或镜像存储生命周期                |
| TA Instance / Context | 一个已加载、可运行的 TA 实例      | 根据 TA Flags 和引用情况决定        |
| TA Session            | 客户端与 TA 建立的逻辑会话       | OpenSession → CloseSession |
| Trusted Thread        | 执行 TA 请求的 OP-TEE 内核线程 | 请求进入、挂起、恢复、释放              |

一个 TA Instance 可以有多个 Session，一个 Session 可以被多次 Invoke；Trusted Thread 则是某次执行的载体。

### 12.5.1 TA 的标准入口函数

普通 GlobalPlatform User TA 一般实现：

```
TEE_Result TA_CreateEntryPoint(void);

void TA_DestroyEntryPoint(void);

TEE_Result TA_OpenSessionEntryPoint(
    uint32_t param_types,
    TEE_Param params[4],
    void **session_context);

void TA_CloseSessionEntryPoint(
    void *session_context);

TEE_Result TA_InvokeCommandEntryPoint(
    void *session_context,
    uint32_t command_id,
    uint32_t param_types,
    TEE_Param params[4]);
```

这几种函数服务于不同生命周期。

典型调用过程：

```
Client OpenSession
    |
    v
Find or Create TA Instance
    |
    +-- If new instance:
    |      Load ELF
    |      TA_CreateEntryPoint()
    |
    v
TA_OpenSessionEntryPoint()
    |
    v
Session Established
    |
    +-- TA_InvokeCommandEntryPoint()
    |
    +-- TA_InvokeCommandEntryPoint()
    |
    +-- ...
    |
    v
TA_CloseSessionEntryPoint()
    |
    v
Release Session
    |
    +-- Destroy TA Instance if required
           |
           v
       TA_DestroyEntryPoint()
```

这个流程说明：

- `TA_CreateEntryPoint()` 不是每次 Invoke 都执行。
- `TA_OpenSessionEntryPoint()` 与 Session 创建关联。
- `TA_InvokeCommandEntryPoint()` 可以多次调用。
- TA Instance 不一定在第一个 Session 关闭之后立即销毁。

### 12.5.2 TA Flags 如何改变生命周期？

OP-TEE 使用：

```
TA_FLAGS
```

描述重要的 Instance 和 Session 行为。

其中最重要的三个 Flag：

```
TA_FLAG_SINGLE_INSTANCE
TA_FLAG_MULTI_SESSION
TA_FLAG_INSTANCE_KEEP_ALIVE
```

例如：

```
#define TA_FLAGS \
    (TA_FLAG_SINGLE_INSTANCE | TA_FLAG_MULTI_SESSION)
```

表示允许使用单个 TA Instance 服务多个 Session。

而 `TA_FLAG_INSTANCE_KEEP_ALIVE` 进一步改变全部 Session 关闭后是否保留该 Instance 的行为。

必须注意：`MULTI_SESSION` 描述是否允许多个 Session，不意味着 TA 内部的函数天然可以无锁并行执行。是否真正支持并发、执行是否被序列化，要结合 OP-TEE 的调用管理策略和 TA 设计分析。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### 12.5.3 为什么每个 Session 需要独立状态？

假设一个 TA 为两名客户端维护身份认证状态：

```
Client A
    |
    +-- Session A
         auth_state = authenticated

Client B
    |
    +-- Session B
         auth_state = unauthenticated
```

如果使用一个 TA Global Variable：

```
static bool authenticated;
```

表示所有客户端的认证状态，就可能出现严重的 Authorization Bypass。

正确方向应该是按 Session 管理状态。

例如：

```
struct my_session {
    bool authenticated;
    uint32_t permissions;
};

TEE_Result TA_OpenSessionEntryPoint(
    uint32_t types,
    TEE_Param params[4],
    void **ctx)
{
    struct my_session *s = TEE_Malloc(
        sizeof(*s), TEE_MALLOC_FILL_ZERO);

    if (!s)
        return TEE_ERROR_OUT_OF_MEMORY;

    *ctx = s;
    return TEE_SUCCESS;
}
```

在 Invoke 时：

```
TEE_Result TA_InvokeCommandEntryPoint(
    void *ctx,
    uint32_t cmd,
    uint32_t types,
    TEE_Param params[4])
{
    struct my_session *s = ctx;

    if (cmd == CMD_PRIVILEGED_OPERATION &&
        !s->authenticated)
        return TEE_ERROR_ACCESS_DENIED;

    /* Execute command after validating its parameters */
    return TEE_SUCCESS;
}
```

这只是展示 Session State 的方法，真实产品还必须正确验证 Client Identity、授权策略、参数及身份建立流程，不能只依赖 TA 内部一个布尔变量。

### 12.5.4 Session Stack：支持 TA → TA 调用

OP-TEE 允许一个 User TA 调用另一个 TA。

例如：

```
Linux Client
    |
    v
TA A
    |
    | TEE_OpenTASession()
    v
TA B
    |
    | TEE_InvokeTACommand()
    v
TA B executes
    |
    v
Return to TA A
    |
    v
Return to Linux
```

这会引入：

- Nested Session。
- Caller Identity。
- TA Context Switch。
- Memory Mapping Switch。
- Recursive Call Depth。
- Deadlock Risk。

OP-TEE 内核通过当前 Session Stack 等机制跟踪执行链，而不是把所有请求都当成互不关联的简单函数调用。

例如 `user_ta_enter()` 中存在：

```
ts_push_current_session(session);

/* Enter user TA */

ts_pop_current_session();
```

同时还限制嵌套进入深度，避免无限递归消耗内核资源。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 12.5.5 TA A 调用 TA B 为什么需要切换地址空间？

假设：

```
TA A:
    VA 0x40000000 → PA_A

TA B:
    VA 0x40000000 → PA_B
```

两者的 VA 可以相同，但是映射的 PA 不同。

当 TA A 调用 TA B 时：

```
Current TA = A
    |
    | Enter TA B
    v
Current TA = B
    |
    | Restore TA A
    v
Current TA = A
```

内核必须确保：

- 当前生效的 User Mapping 与当前 TA 一致。
- Syscall Pointer Validation 以正确的 TA 为依据。
- Kernel 对用户缓冲区的访问不会使用错误的地址空间。
- TA B 无法访问 TA A 的私有内存。
- 返回 TA A 后，其执行现场仍然正确。

这里的 Context Switch 不等价于 OpenSBI Domain Switch。

因为 TA A 和 TA B 都运行在 OP-TEE Domain 内部。

此外，地址空间切换与实际的 TLB 管理需要服从 RISC-V 的地址转换同步规则，不能仅仅因为更新了软件 Context Pointer 就认为所有旧翻译状态已经失效。

## 12.6 User TA 的 FP/Vector Register Context 管理

这一节是 Chapter 12 中最接近 RISC-V Architecture Maintainer 工作的部分。

为什么？

因为整数寄存器的 Context Switching 相对直观，但 FP/Vector Register 涉及：

- 硬件扩展是否存在。
- 扩展状态是否启用。
- 寄存器实际由哪个执行上下文拥有。
- Trap 和 Syscall 时是否需要保存。
- Secure/Non-secure Domain 之间如何隔离。
- 多 hart 上 FP/Vector 状态如何保持正确。

最重要的是：保存 FP/Vector 状态不仅是计算正确性问题，更是安全隔离问题。

### 12.6.1 RISC-V FP Register Architecture

假设系统支持 F/D Extension，那么存在：

```
f0 ... f31
```

共 32 个 Floating-Point Registers。

同时还有：

```
fcsr
```

用于保存 Floating-Point Control and Status。

其中 `fcsr` 包含：

- `frm`：Floating-Point Rounding Mode。
- `fflags`：Floating-Point Exception Flags。

对于 RV64D，32 个 64-bit FP 寄存器的主体状态大小为：

\\[ 32\times 8=256\text{ Bytes} \\]

再加上 `fcsr` 和 Context 结构的对齐开销。

如果仅保存整数 GPR，而不保存 FP Register，那么两个不同 TA 之间切换后可能出现：

```
TA A:
    f0 = Secret_A
    f1 = Intermediate_A

Context Switch

TA B:
    reads f0
    obtains Secret_A
```

这是不可接受的。

除了显式读取寄存器，如果 TA B 在获得旧 FP State 后执行浮点计算，还可能产生不可预测的计算结果。

### 12.6.2 `sstatus.FS` 是什么？

RISC-V 利用 `sstatus.FS` 管理 Floating-Point State。

| FS Value | 状态      | 含义                   |
| -------- | ------- | -------------------- |
| `00`     | Off     | 相关 FP 指令不可执行         |
| `01`     | Initial | FP State 处于初始状态      |
| `10`     | Clean   | FP State 被标记为未发生新的修改 |
| `11`     | Dirty   | FP State 可能已经被修改     |

需要特别注意：

FS 并不是一个可信的「属于哪个 TA」的 Owner ID。

它也不是可以无限制依赖的精确 Dirty Tracking 日志。

因此不能简单认为：

```
if (FS == DIRTY)
    save_fp_registers();
```

就足以实现完整而安全的 FP Context Switching。

尤其在跨 Kernel、User 和 Domain 的切换过程中，状态位变化不能完整表达真实的寄存器所有权和安全生命周期。

### 12.6.3 Lazy Switching 的工作原理

经典 Lazy FP Switching 设计通常是：

```
TA A executes
    |
    | FP disabled initially
    v
First FP instruction
    |
    | Illegal Instruction
    v
Trap Handler
    |
    | Ensure previous owner state is saved
    | Restore or initialize TA A FP state
    | Enable FP
    v
Retry instruction
```

设计动机是性能。

如果一个 TA 从来不使用 FP，那么无需为它频繁保存和恢复 32 个 FP Registers。

但是这种策略需要区分两个独立的问题：

1. Lazy Enable / First-use Handover：第一次使用 FP 时才分配 FP Unit 的使用权。
2. Lazy Save / Restore：延迟执行真正的寄存器保存和恢复。

它们可以组合使用，但不是同一种机制。

特别是延迟保存，必须确保旧状态在其物理寄存器被覆盖前保存完毕。

### 12.6.4 为什么 RISC-V OP-TEE 选择更谨慎的 Context 管理？

RISE RP022 Milestone 7 专门讨论了这个问题。

其核心结论是：

不能仅依靠 FS/VS 状态位判断是否需要保存 FP/Vector 寄存器，因此需要对执行上下文的寄存器所有权、保存和恢复建立明确的规则。

该项目采用第一次使用时进行受控 Unit Handover，并在可能发生状态覆盖、跨 Context 切换时执行必要的完整寄存器保存和恢复。

2026 年 10 月的项目状态如下：

| 功能                              | Upstream 状态      |
| ------------------------------- | ---------------- |
| RISC-V FP Context Switching     | 已合并              |
| FP 测试 `regression_1045`         | 已合并              |
| RISC-V Vector Context Switching | 已实现和验证，仍在 Review |
| Vector 测试 `regression_1046`     | 已实现和验证，仍在 Review |

这一状态以项目公开说明为准，不应把 Vector PR 尚未合并的功能当成所有 upstream 构建已经具备的能力。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### 12.6.5 实际源码如何管理 FP Unit？

当前 RISC-V OP-TEE 中，相关入口位于：

`core/arch/riscv/kernel/thread_arch.c`

其中包括：

```
thread_kernel_enable_vfp()
thread_kernel_disable_vfp()
thread_user_enable_vfp()
thread_user_save_vfp()
thread_user_clear_vfp()
```

例如：

```
void thread_user_enable_vfp(
    struct thread_user_vfp_state *uvfp);
```

它的核心任务不是简单设置 `FS`，而是管理：

- 当前 Trusted Thread 中的扩展状态 Owner。
- 前一个 TA 或 REE 的 FP State。
- 新 TA 的初始状态。
- 需要恢复的 FP Context。
- 当前 FP Unit 的访问状态。

特别值得研究的是当前源码中的这一段逻辑：

```
/* A new user context */
vfp_enable();
vfp_clear_extension_regs();
```

这里的设计意图非常明确：

如果新 User Context 没有有效的既有 FP State，就不能把上一个使用者留下的寄存器内容直接交给它。

必须进行清理或安全初始化。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 12.6.6 First-use FP Trap 如何恢复执行？

以 User TA 首次使用 FP 为例。

假设：

```
User TA
    sstatus.FS = Off
```

随后执行：

```
fld ft0, 0(a0)
```

由于 FP Unit 未启用，会触发 Illegal Instruction Trap。

其核心状态：

```
scause = 2
sepc   = Faulting FP Instruction
```

OP-TEE 的 Abort Handler 检查：

- 该异常是否来自 U-mode。
- 是否属于 Illegal Instruction。
- FP Unit 当前是否尚未启用。
- 当前构建是否支持相关 FP Context 管理。

如果确定属于首次启用扩展的情形：

```
Save previous FP owner if needed
    |
Restore or initialize current TA FP state
    |
Enable FP
    |
Return without advancing sepc
    |
Retry original FP instruction
```

这里最值得注意的是：

不能修改 `sepc` 跳过触发异常的 FP 指令。

因为这一条指令并没有成功执行。

如果跳过，就会导致 TA 执行语义发生变化。

与前面的普通 `ecall` 返回不同，FP First-use Trap 必须重新执行原指令。

当前 RISC-V `abort.c` 中，确实具有 `is_vfp_fault()` 和 `handle_user_mode_vfp()` 相关逻辑。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 12.6.7 如果并非 FP 指令，而是真正非法的指令呢？

这里可能存在一个问题：

第一次 Illegal Instruction Trap 时，FP Unit 是 Off。

如果处理器遇到一条本来就不支持的指令，也可能首先被认为是扩展使用请求。

这该怎么办？

一种稳健设计是：

```
Illegal Instruction
       |
       v
FP Unit is Off?
       |
      Yes
       |
       v
Safely enable FP
       |
       v
Retry same instruction
       |
       v
Still illegal?
       |
      Yes
       |
       v
Treat as genuine illegal instruction
```

这意味着对于无效指令最多进行一次受控的 FP Enable Retry，而不是无限循环。

对于 FP 和 Vector 都存在的系统，还需要定义扩展启用的次序与最大重试边界。

RP022 的 Vector Context 方案采用渐进式 Unit Handover，并明确避免无界的重新触发循环。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### 12.6.8 Vector Extension 为什么更加复杂？

RISC-V Vector Extension 引入：

```
v0 ... v31
```

共 32 个 Vector Registers。

还存在：

```
vl
vtype
vstart
vcsr
vlenb
```

其中：

- `vl`：当前 Vector Length。
- `vtype`：Vector Element Width、LMUL 等配置。
- `vstart`：向量指令的执行起始元素索引。
- `vcsr`：Vector Control and Status。
- `vlenb`：当前硬件的 VLEN 字节数。

假设：

```
VLEN = 256 bits
```

那么单个 Vector Register 大小为：

\\[ 256/8=32\text{ Bytes} \\]

全部 Vector Registers：

\\[ 32\times32=1024\text{ Bytes} \\]

如果：

```
VLEN = 512 bits
```

则：

\\[ 32\times64=2048\text{ Bytes} \\]

这还没有计算其他 Vector CSR 和 Context Metadata。

因此：

Vector Context Switch 的保存/恢复成本会直接受到 VLEN 的影响。

### 12.6.9 Vector Context 必须保存什么？

至少需要考虑：

| 状态       | 作用               | 不保存的风险         |
| -------- | ---------------- | -------------- |
| `v0–v31` | Vector 数据        | 数据泄露、计算结果错误    |
| `vl`     | 当前 Vector Length | 指令处理元素数量改变     |
| `vtype`  | Vector 类型配置      | SEW / LMUL 不匹配 |
| `vstart` | 指令执行进度           | 中断恢复或指令重试错误    |
| `vcsr`   | Vector 控制状态      | 舍入/饱和等行为改变     |
| `VS`     | Vector Unit 状态   | 错误 Trap 或权限状态  |

注意：

`vlenb` 主要用于描述实现提供的 Vector Register 宽度，并非可任意恢复的普通用户控制状态。

如果系统在多个不同 VLEN 的 Hart 之间迁移 TA，还必须考虑目标 Hart 是否能够承载已有 Vector Context；不能仅按照某个固定数组长度盲目复制。

对于异构 Hart，这是需要单独制定调度限制或兼容性策略的架构问题。

### 12.6.10 FP/Vector Context 的真正安全边界

我建议将状态所有权划分为以下逻辑域：

```
Physical FP/Vector Register File
         |
         +-- REE Context
         |
         +-- OP-TEE Kernel Context
         |
         +-- User TA A Context
         |
         +-- User TA B Context
```

这些实体可能在同一个 Hart 上分时使用实际物理寄存器。

因此必须保证：

\\[ \text{Active Hardware State} = \text{Authorized Current Owner State} \\]

当 Owner 改变时：

\\[ \text{Old Owner State} \rightarrow \text{Secure Save or Clear} \\]

并且：

\\[ \text{New Owner State} \rightarrow \text{Validated Restore or Initialize} \\]

任何一次未经正确保存、恢复、清理的 Owner Change 都可能造成漏洞。

需要区分两种情况：

- OP-TEE TA A → TA B：主要由 OP-TEE 管理内部用户态扩展状态。
- Linux Domain ↔ OP-TEE Domain：还涉及 OpenSBI 及更底层 Domain Context 的保存与保护。

在跨 Domain 场景下，不能只检查 OP-TEE 自身的 FP/Vector 代码，因为 M-mode Firmware 也可能参与相关 Context 的管理。

## 12.7 Lazy Context Switching 与 Eager Context Switching：如何选择？

这一节我们从系统工程角度讨论它们的取舍。

### 12.7.1 Eager Context Switching

Eager 的基本思想：

在已知需要交接状态所有权的时刻，明确完成必要的 Save/Restore。

例如：

```
TA A
    |
    | Save A's FP state
    v
Secure State Switch
    |
    | Restore B's FP state
    v
TA B
```

优点：

- 执行模型清晰。
- 安全不变量较容易定义。
- 较少依赖隐含的状态位。
- 对 Review 和测试更友好。

代价是：

- 额外内存访问。
- 更多的 Context Storage。
- 使用大量 Vector 寄存器时，切换开销可能明显增加。

### 12.7.2 Lazy Context Switching

Lazy 的思想是：

如果状态暂时不会被覆盖，就推迟保存或恢复。

例如：

```
TA A uses FP
    |
    v
Kernel Syscall
    |
    | Kernel does not use FP
    |
    v
Return to TA A
```

如果执行期间寄存器始终归 TA A 所有，且不会被其他实体覆盖，就可能不必立即将全部 FP Register 写入内存。

但是需要满足前提：

- Kernel 或 Interrupt Handler 不会意外使用该扩展。
- Context Ownership 有可靠跟踪。
- Foreign Interrupt / RPC 等所有路径都有正确处理。
- 不发生未授权的跨 Domain State Exposure。
- 异常及嵌套切换不会造成失效的 Owner 判断。

因此，不能仅通过 FS/VS Dirty Bit 决定是否保存。

### 12.7.3 为什么不应该简单地二选一？

真正的实现可以同时使用：

- First-use Trap，避免对从未使用 FP 的 TA 进行无意义初始化。
- Owner Tracking，明确寄存器属于哪个 Context。
- Deferred Save，在安全的时机推迟保存。
- Mandatory Save，在即将被覆盖或跨隔离边界时强制保存。
- Clear-on-new-context，防止新的 Context 观察到旧数据。

所以看到 upstream 中某个函数叫：

```
vfp_lazy_save_state_init()
```

不意味着整个系统就是一个「只依靠 FS Dirty Bit 的纯 Lazy Switching 实现」。

需要进一步阅读函数具体如何管理 Owner、保存状态以及实现真正的硬件交接。

### 12.7.4 实际工业项目应该如何做选择？

我建议从三个维度评估。

| 维度          | Eager 优势   | Lazy 优势       |
| ----------- | ---------- | ------------- |
| Correctness | 状态管理明确     | 减少不必要的保存      |
| Security    | 较容易审计      | 需要更复杂的所有权证明   |
| Performance | 可预测，但有固定成本 | 适合扩展使用率低的工作负载 |

可以建立一个简化成本模型。

设：

- \\(C_s\\)：保存扩展寄存器的成本。
- \\(C_r\\)：恢复扩展寄存器的成本。
- \\(C_t\\)：First-use Trap 及处理成本。
- \\(p\\)：工作负载在相关路径上实际使用扩展的概率。

简化情况下：

\\[ E\_{\text{eager}}=C_s+C_r \\]

\\[ E\_{\text{lazy}}\approx p(C_t+C_s+C_r) \\]

这个式子仅用于解释设计动机。

真实实现还包含 Cache Miss、Memory Bandwidth、嵌套 Trap、Hart Migration、状态清理以及安全检查的开销。

另外，如果保存/恢复必须在某个安全边界之前发生，就不能仅为了减少平均成本而推迟。

### 12.7.5 推荐的性能验证方法

不能只运行一个空循环比较两种实现。

需要构造至少五种负载：

1. 从不使用 FP/Vector 的 TA。
2. 大量使用 FP 的 TA。
3. 大量使用 Vector 的 TA。
4. FP/Vector 使用与 Syscall、RPC 交替发生。
5. 多个 TA 在不同 Hart 或相同 Hart 上交替执行。

对每种负载分别测量：

- 平均 Context Switch 时间。
- P95/P99 延迟。
- Trap 次数。
- 寄存器 Save/Restore 次数。
- Cache Miss 和 Memory Bandwidth。
- 性能优化后的隔离测试是否仍然通过。

对于 TEE Kernel，任何声称提高 Context Switch 性能的 Patch，都必须同时提供 correctness 和 isolation 的回归测试。

## 12.8 TA Context Switch 中的状态泄露风险

到这里，我们已经分析了 User Mode、Kernel Mode、Syscall 和 FP/Vector。

现在从安全设计视角统一检查它们。

### 12.8.1 攻击模型

假设 TA A 和 TA B 都由合法开发者签名，但 TA B 可能被攻击者控制或存在漏洞。

我们的目标是：

TA B 不能访问 TA A 的私有内存、寄存器内容和私有安全资源。

隔离不能仅依赖：

```
TA B will behave correctly
```

而必须依赖 OP-TEE Core 和底层硬件强制执行。

### 12.8.2 哪些 CPU 状态可能泄露？

| 状态                 | 潜在风险           | 防护方向                              |
| ------------------ | -------------- | --------------------------------- |
| GPR                | 保留其他 TA 的敏感值   | 全面初始化/保存/恢复                       |
| `gp` / `tp`        | 错误全局数据/TLS 访问  | 正确切换 User Context                 |
| FP Registers       | 直接数据泄露         | Owner Tracking、Save/Restore、Clear |
| Vector Registers   | 大量数据泄露         | 完整 Vector Context Management      |
| `sepc` / `sstatus` | 返回错误特权级别或地址    | 校验并控制 Trap Return 状态              |
| `satp` / TLB       | 旧 TA 的地址空间仍然有效 | 严格映射和地址转换同步                       |
| Shared Memory      | 非可信数据篡改、TOCTOU | Pointer Validation、Copy 和生命周期管理   |
| Cache / Predictor  | 微架构侧信道         | 根据威胁模型采用平台缓解措施                    |

注意最后一项。

正确保存 GPR、FP 和 Vector State，不等于已经解决 Cache Side Channel、Spectre 类攻击或所有微架构信息泄露问题。

Architectural State Isolation 和 Microarchitectural Side-channel Isolation 是两个不同层面的安全目标。

### 12.8.3 Syscall 参数指针为什么不能直接相信？

假设 User TA 传入：

```
TEE_GenerateRandom((void *)0xFFFF000000000000, 32);
```

如果 OP-TEE Core 不检查这个地址，就可能访问到不属于该 TA 的内存。

因此内核必须针对当前 User TA Context：

- 判断地址是否属于允许访问的 User Mapping。
- 判断整个地址区间是否有效。
- 判断所需 Read/Write Permission。
- 避免地址长度相加发生整数溢出。
- 在访问期间保持相关 Mapping 的有效生命周期。

OP-TEE 对 User Pointer 提供了专门的 Access Check 和 Copy 接口。

例如：

```
copy_from_user(...)
copy_to_user(...)
```

在 `tee_svc.c` 中，像 `syscall_check_access_rights()` 这样的函数会结合当前 TA User Context，调用 VM Access Validation。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 12.8.4 TOCTOU 是什么？

Time-of-Check to Time-of-Use。

假设 TA 或 REE 传入一个共享内存中的参数：

```
Kernel checks parameter
        |
        | Attacker modifies shared buffer
        v
Kernel uses parameter
```

那么 Kernel 使用的数据可能已经不再等于校验时的数据。

例如：

```
/* 错误的设计示意 */
if (shared->length <= MAX_LEN)
    copy_data(dst, shared->buffer, shared->length);
```

如果 `shared->length` 在检查后被修改，可能导致长度变化带来的问题。

更安全的思路是：

```
/* 教学示意 */
size_t length = READ_ONCE(shared->length);

if (length > MAX_LEN)
    return TEE_ERROR_BAD_PARAMETERS;

/* 再验证该长度对应的地址空间和访问权限 */
```

但即使长度读取一次，也不意味着整个共享 Buffer 就具有不可变性。

如果之后仍依赖共享内存中的字段、指针或数据，必须根据威胁模型采取安全拷贝、固定快照或其他一致性措施。

Trusted Pointer Validation 和 Untrusted Shared Memory TOCTOU 是两个相关但不同的问题。

### 12.8.5 Context Save Buffer 本身也需要保护

假设：

```
struct fp_context {
    uint64_t f[32];
    uint32_t fcsr;
};
```

如果其存储位置在 REE 可修改的内存里，虽然执行了 Save/Restore，也无法保证安全。

所以除了寄存器保存逻辑正确，还必须检查：

- Context Save Buffer 是否位于可信内存。
- Context 指针是否受到内核保护。
- 生命周期结束后是否清理敏感数据。
- 不同 TA Context 是否发生内存重用和泄露。
- SMP 并发下是否可能同时修改同一 Context。

### 12.8.6 构造一个安全不变量

我建议未来做 Upstream Review 时，经常使用下面这个形式。

对于任意两个彼此隔离的 TA A、B：

\\[ A\ne B \\]

在切换到 B 后，B 可观察的架构状态应满足：

\\[ \operatorname{Obs}\_B \subseteq \operatorname{AuthorizedState}\_B \\]

也就是说：

TA B 不能通过未授权的架构接口观察到 TA A 的私有状态。

这一不变量可以展开为：

```
TA B starts/resumes
    |
    +-- Correct User VA Mapping
    +-- Correct GPR State
    +-- Correct GP / TP / SP
    +-- Correct FP / Vector State
    +-- Correct Privilege / CSR State
    +-- Correct Session Identity
    +-- Correct Resource Handles
```

任何一项不成立，都可能造成错误甚至隔离突破。

这也是为什么 OP-TEE Context Switch 代码虽然不一定很长，却属于安全内核中最需要严格 Review 的代码之一。

## 12.9 TA Crash、异常终止与资源清理

一个成熟的 TEE Kernel 不能只保证 TA 正常执行，还需要能够处理恶意 TA 和存在 Bug 的 TA。

### 12.9.1 RISC-V TA 可能发生哪些异常？

典型包括：

| Exception                    | `scause` | 场景                |
| ---------------------------- | -------- | ----------------- |
| Illegal Instruction          | 2        | 不支持的指令、扩展 Off     |
| Breakpoint                   | 3        | `ebreak`          |
| Load Access Fault            | 5        | Load 的物理访问异常      |
| Store/AMO Access Fault       | 7        | Store/AMO 的物理访问异常 |
| Environment Call from U-mode | 8        | TA Syscall        |
| Instruction Page Fault       | 12       | 指令地址转换或权限异常       |
| Load Page Fault              | 13       | 数据读取页表异常          |
| Store/AMO Page Fault         | 15       | 数据写入页表异常          |

这里必须区分：

- 可恢复的扩展首次使用 Trap：可能需要启用 FP/Vector 并重试。
- 可处理的内存异常：是否能恢复取决于映射、Pager 和错误类型。
- 不可恢复的 TA 异常：通常需要终止当前 TA 执行。
- OP-TEE Core 自身异常：可能影响整个 TEE 的可信状态，需要更严格的处理。

### 12.9.2 `abort_from_user` 的调用链

在 `thread_rv.S` 中，非 ECALL 的 User Exception 进入：

```
trap_from_user
    |
    v
exception_from_user
    |
    | Not CAUSE_USER_ECALL
    v
abort_from_user
    |
    | Save User Register State
    | Switch Abort Stack
    v
abort_handler
```

Abort Handler 随后根据异常来源及类型决定：

- 恢复执行。
- 重新启用 FP/Vector。
- 处理可恢复的 Page Fault。
- 终止 User TA。
- 报告严重的 Kernel 异常。

相关实现位于：

`core/arch/riscv/kernel/abort.c`。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 12.9.3 为什么 TA Crash 不应该导致整个 OP-TEE Panic？

因为 User TA 是一个低权限执行实体。

例如：

```
volatile uint64_t *p = (uint64_t *)0;
*p = 0x1234;
```

如果零地址未映射，会触发 Store Page Fault。

理想行为应该是：

```
TA illegal memory access
    |
    v
User-mode Page Fault
    |
    v
OP-TEE Abort Handler
    |
    v
Mark TA as Panicked / Dead
    |
    v
Unwind User Execution
    |
    v
Return TEE_ERROR_TARGET_DEAD
```

而不是将整个安全系统直接 Reset。

但这不是绝对规则。如果异常发生在内核关键路径，或者内核已经无法确认安全状态，那么系统可能需要 Panic 或进入平台定义的故障处理流程。

### 12.9.4 如何从异常现场回到 S-mode C 代码？

这是一个非常重要的汇编细节。

当前 `abort.c` 中的：

```
handle_user_mode_panic(...)
```

并不是简单将 `sepc` 加 4 后返回 U-mode。

它会准备一个新的异常返回上下文。

核心动作包括：

```
ai->regs->a0 = TEE_ERROR_TARGET_DEAD;

ai->regs->epc =
    (vaddr_t)thread_unwind_user_mode;

ai->regs->sp = thread_get_saved_thread_sp();

ai->regs->status =
    xstatus_for_xret(true, PRV_S);
```

这是一段基于真实 upstream 的核心代码节选。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



其含义是：

- 不再返回触发异常的 User TA PC。
- 将返回目标改为 OP-TEE Kernel 的 Unwind Routine。
- 恢复预先保存的 Kernel Stack。
- 准备返回 S-mode。
- 将 TA 终止状态传递给上层。

于是执行：

```
User TA Fault
    |
    v
S-mode Abort Handler
    |
    | Modify Return Context
    v
sret
    |
    v
thread_unwind_user_mode
    |
    | Restore Kernel Call Frame
    v
thread_enter_user_mode returns
    |
    v
user_ta_enter()
```

这个技巧体现了一种常见的底层操作系统设计思想：

异常返回机制不一定非要返回原来的异常位置，它也可以作为受控执行流恢复机制的一部分。

当然，新的 Return Context 必须由内核构造并满足权限约束，不能直接采用不可信 User Data 决定特权返回地址。

### 12.9.5 TA Panic 状态如何向调用者传播？

在：

`core/kernel/user_ta.c`

`user_ta_enter()` 返回后会检查：

```
if (utc->ta_ctx.panicked) {
    abort_print_current_ts();
    res = TEE_ERROR_TARGET_DEAD;
}
```

因此，OP-TEE 可以将 User TA 的异常转换为面向上层的错误语义。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



但要注意：

TA Panic 不等于 Linux Kernel Panic，也不等于 OpenSBI Fatal Trap。

这三个错误的故障域不同。

### 12.9.6 正确的资源清理包括什么？

一个 User TA 的异常退出，可能留下多个类型的资源：

- User VM Mapping。
- TA Heap 和 Stack。
- Open Sessions。
- Crypto Operation State。
- Storage Object Handles。
- FP/Vector Context。
- 当前执行中的 Memory Reference。
- RPC 相关的临时状态。
- Trace、Debug、Ftrace 状态。

例如：

```
struct user_ta_ctx {
    struct tee_ta_session_head open_sessions;
    struct tee_cryp_state_head cryp_states;
    struct tee_obj_head objects;
    struct tee_storage_enum_head storage_enums;
    struct user_mode_ctx uctx;
    struct tee_ta_ctx ta_ctx;
};
```

这个结构来自 OP-TEE Core 的实际代码。它明确表明 User TA Context 不只有寄存器和页表，而是还持有 Session、Crypto State 和 Storage Object 等内核资源。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



需要特别注意：异常退出后并不是在一个汇编函数里一次性释放全部资源。

实际清理需要遵守 Session、Context 和资源引用的生命周期，由各层协同处理。

### 12.9.7 Staff 级别必须考虑：Cleanup 本身是否可能失败？

例如：

- TA A 调用 TA B，TA B 在 Crypto Operation 中崩溃。
- TA B 仍持有存储对象的锁。
- TA A 等待 TA B 返回。
- 同时 Hart 1 又尝试通过另一个 Session 访问 TA B。

这时需要判断：

- 是否存在锁顺序问题。
- 是否所有错误路径都会解除资源占用。
- 是否可能形成悬挂 Session。
- 是否需要标记 Context 为不可恢复。
- 是否会影响其他已经存在的 Session。
- 是否有敏感数据没有清理。

如果只能解释正常路径，却无法解释异常与并发清理路径，说明对 Kernel Lifecycle 的理解仍不够完整。

## 12.10 从源码、实验到 Upstream：完整实践路线

这一节把前面的知识落到可以操作的工程任务上。

### 12.10.1 建议的源码阅读顺序

不要随机打开大量目录。我建议按照执行路径阅读以下文件：

Step 1 — TA Loading

验证、映射和入口准备

`core/kernel/ldelf_loader.c` → `ldelf/ta_elf.c` → `core/kernel/user_ta.c`

Step 2 — User Mode Entry

CPU Context 与 S/U Transition

`core/arch/riscv/kernel/thread_arch.c` → `thread_rv.S`

Step 3 — Syscall

Trap、Syscall Number、Dispatcher

`thread_rv.S` → `thread_arch.c` → `core/kernel/scall.c` → `core/tee/tee_svc.c`

Step 4 — Session & TA Context

Instance、Session 和 Kernel Resource

`core/kernel/tee_ta_manager.c` → `user_ta.c` → `core/mm/vm.c`

Step 5 — FP/Vector & Exception

扩展寄存器状态与异常恢复

`thread_arch.c` → `thread_rv.S` → `abort.c` → `core/arch/riscv/kernel/vfp*`

### 12.10.2 实验 A：ELF 与 TA Loader

目标：理解 TA 的 ELF 格式、PIE 和 Relocation。

在已经安装 RV64 GNU Toolchain 的 WSL2 环境中，对一个构建好的 RISC-V TA ELF 执行：

```
riscv64-linux-gnu-readelf -h ta.elf

riscv64-linux-gnu-readelf -l ta.elf

riscv64-linux-gnu-readelf -r ta.elf

riscv64-linux-gnu-objdump -d ta.elf
```

`ta.elf` 指 TA 构建过程中、签名打包前的 ELF 产物；它不一定与最终安装的 UUID 命名 `.ta` 文件具有相同的封装格式。

重点检查：

- ELF Class 是否为 ELF64。
- Machine 是否为 RISC-V。
- Entry Address。
- `PT_LOAD` Segment。
- Segment Permissions。
- Dynamic Relocations。
- `.text`、`.rodata`、`.data`、`.bss` 的排列。
- 是否存在不合理的 RWX Mapping。

然后跟踪 `ldelf/ta_elf.c`，观察 ELF 的 `p_vaddr`、Load Base 和最终 VM Mapping 之间的关系。

### 12.10.3 实验 B：U-mode → S-mode Syscall

目标：从汇编理解完整 Syscall。

推荐断点：

```
b thread_enter_user_mode
b __thread_enter_user_mode
b thread_trap_vect
b thread_scall_handler
b scall_handle_user_ta
```

根据当前加载地址、符号可见性和优化配置，可能需要在对应的汇编指令地址下断点。

在 Syscall Entry 观察：

```
info registers

p/x $scause
p/x $sepc
p/x $sstatus
p/x $sscratch

x/8i $sepc
```

要验证：

1. Trap 前后 `tp` 和 `sscratch` 如何变化。
2. Kernel SP 如何恢复。
3. `scause` 是否为 U-mode ECALL。
4. `sepc` 是否指向 `ecall`。
5. Syscall Number 如何从 `t0` 提取。
6. Syscall 正常返回时为何将 `sepc` 增加 4。
7. `sret` 返回后执行的是否是 `ecall` 后的下一条指令。

### 12.10.4 实验 C：构造一个非法 TA 内存访问

在专门用于实验的 User TA 中添加一个命令：

```
/* 仅用于隔离验证，不用于生产 */
#define CMD_TRIGGER_FAULT 0x100

TEE_Result TA_InvokeCommandEntryPoint(
    void *session_ctx,
    uint32_t command_id,
    uint32_t param_types,
    TEE_Param params[4])
{
    if (command_id == CMD_TRIGGER_FAULT) {
        volatile uint64_t *p = (uint64_t *)0;

        *p = 0x12345678;

        return TEE_SUCCESS;
    }

    return TEE_ERROR_NOT_SUPPORTED;
}
```

在未映射零地址的实验环境中，这段代码应触发 User-mode Store Fault。

用 GDB 验证：

```
scause = 15
stval  = 0
SPP    = 0
```

具体前提是 CPU 产生的是 Store Page Fault；如果底层硬件、映射或 Fault 类型不同，`scause` 可能不同，不应该把 15 当成所有平台的固定结果。

需要观察是否进入：

```
abort_from_user
    |
    v
abort_handler
    |
    v
handle_user_mode_panic
    |
    v
thread_unwind_user_mode
```

最终还应该验证：

- 当前 TA 请求返回 `TEE_ERROR_TARGET_DEAD`。
- OP-TEE Core 不会因为普通 User TA Fault 直接崩溃。
- 其他正常 TA 是否仍然能执行。
- 发生异常的 TA Instance 和相关 Session 是否按预期处理。

### 12.10.5 实验 D：FP/Vector Context Isolation

建议优先运行：

```
xtest 1045
```

对应 FP Context Switching 测试。

如果构建环境包含仍在 Review 的 Vector 相关修改和测试，再运行：

```
xtest 1046
```

这里不能假设任意 upstream 版本都已经包含 `1046`。

RP022 提供的测试覆盖 Syscall、RPC、不同 TA Instance 及并发 Context 等情形。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



可以进一步自己设计以下测试：

```
TA A:
    Initialize FP registers with Pattern A
    Call OP-TEE Syscall
    Verify FP registers unchanged

TA B:
    Initialize FP registers with Pattern B
    Call OP-TEE Syscall
    Verify FP registers unchanged

TA A:
    Resume
    Verify Pattern A
```

并添加：

```
TA A uses FP
    |
    v
RPC to Linux
    |
    v
Linux uses FP
    |
    v
Return to OP-TEE
    |
    v
TA A verifies its FP State
```

这个实验可以验证跨 Domain 以及 OP-TEE 内部的状态交接，但若希望严格证明跨 REE/TEE 无信息泄露，还需要进一步设计针对寄存器可观察性与状态初始化的测试。

### 12.10.6 上游贡献的切入点

完成这一章之后，我认为可以重点寻找以下类型的改进：

| 课题                              | 贡献形式                 | 难度 |
| ------------------------------- | -------------------- | -- |
| RISC-V Syscall ABI 测试           | `xtest` / 架构测试       | 中  |
| TA Illegal Instruction Recovery | Bugfix / Regression  | 中高 |
| FP/Vector State Leakage 检测      | 安全回归测试               | 高  |
| TA Exception + RPC 并发           | Stress Test / Bugfix | 高  |
| TA ASLR / W^X 边界                | MMU Hardening        | 高  |
| Context Switch 性能优化             | RFC / Kernel Patch   | 极高 |

对于目标是成长为 OP-TEE Maintainer 的工程师，我尤其推荐：

FP/Vector Context Management + RISC-V Exception Recovery + Regression Testing

因为这个方向同时涉及 RISC-V ISA、汇编、OP-TEE Core、SMP 和实际安全保证，非常适合形成具有持续性的架构专长。

# 12.11 综合案例：从 Linux Invoke 到 TA 执行结束

前面已经分别研究了 ELF、U-mode、Syscall、Session 和 Context。

最后将所有机制放在一个完整工业执行场景中。

假设有一块 RV64 SoC：

- 8 个 Hart。
- OpenSBI 运行在 M-mode。
- Linux 和 OP-TEE 运行在隔离的 Supervisor Domains。
- OP-TEE Core 使用 S-mode。
- User TA 使用 U-mode。
- Linux 通过适配好的 TEE Transport 调用 OP-TEE。

系统需要执行一个签名服务：

```
TEEC_InvokeCommand(
    &session,
    CMD_SIGN_DATA,
    &operation,
    &origin);
```

完整执行链：

```
[1] Linux User CA
      |
      | TEEC_InvokeCommand()
      v
[2] Linux OP-TEE Driver
      |
      | Transport request
      v
[3] OpenSBI / M-mode
      |
      | Switch to OP-TEE Domain
      v
[4] OP-TEE S-mode Core
      |
      | Find TA Session / Context
      |
      +-- TA not loaded?
      |       |
      |       v
      |   Verify & Load TA
      |       |
      |       v
      |   ldelf / ELF Relocation
      |
      v
[5] user_ta_enter()
      |
      | Prepare User Stack
      | Prepare Parameters
      | Prepare User Register Context
      v
[6] thread_enter_user_mode()
      |
      | Set sepc / sstatus
      | Restore User Registers
      | sret
      v
[7] RISC-V U-mode User TA
      |
      | TA_InvokeCommandEntryPoint()
      |
      | TEE crypto API
      v
[8] libutee
      |
      | ecall
      v
[9] OP-TEE S-mode Trap
      |
      | Save User Registers
      | Switch Kernel Stack
      | Dispatch Syscall
      v
[10] OP-TEE Crypto Service
      |
      | Hardware or Software Crypto
      v
[11] Return to User TA
      |
      | sret
      v
[12] TA Execution Continues
      |
      | Return result
      | User return syscall
      v
[13] thread_unwind_user_mode()
      |
      | Restore Kernel Context
      v
[14] OP-TEE Core
      |
      | Update output parameters
      | Restore Domain return path
      v
[15] OpenSBI / Linux
      |
      v
TEEC_InvokeCommand() returns
```

这个流程包含三类不同的切换：

| 切换                    | 主要机制                                | 目的               |
| --------------------- | ----------------------------------- | ---------------- |
| Linux ↔ OP-TEE        | OpenSBI Domain / Platform Transport | 隔离执行域转换          |
| OP-TEE Core ↔ User TA | `sret`、`ecall`、Trap Handler         | S/U Privilege 切换 |
| User TA A ↔ User TA B | Session/VM Context + User Execution | TA 资源和执行上下文切换    |

你应该能够将这张流程图的每一项映射到实际代码，再解释对应的 CSR、寄存器和内存权限变化。

# Chapter 12 — Staff Engineer 面试与架构评审题

下面设计了 14 道题，覆盖真实工业问题中的异常处理、隔离、安全、性能、并发与 Upstream Review。

每道题都附带参考答案和关键考察点。

## Q1. 一个签名合法的 RISC-V User TA 能否直接被 OP-TEE Core 当成普通 ELF 加载并执行？为什么？

考察：TA Trust Chain、ELF Loader、Security Boundary

参考答案：

不能仅凭签名合法就直接执行。

必须区分几个阶段：

1. Signature Verification：确认镜像来源和内容完整性。
2. TA Identity：确认 UUID 和签名授权策略一致。
3. ELF Validation：验证格式、Segment、Relocation 和地址范围。
4. Memory Mapping：建立符合权限要求的映射。
5. Runtime Initialization：准备 Stack、Heap、TLS 和入口。
6. Privilege Transition：在受控状态下进入 U-mode。

更重要的是，ELF Parser 自身也会处理来自不可信存储的复杂输入，因此它也是攻击面。

OP-TEE 的 `ldelf` 虽然位于安全域中运行，但采用用户态执行模式。它通过受限的 Loader Syscall 请求 Core 建立映射，避免将全部 ELF 解析逻辑直接放在最高权限的内核执行路径中。

Staff-level 进一步要讨论：签名密钥泄露、合法旧版 TA 回滚、恶意 ELF Relocation、Segment 重叠以及整数溢出。

## Q2. RISC-V OP-TEE 中，为什么 User TA 执行 `ecall` 不需要每次都进入 OpenSBI？

考察：Privilege Delegation、Domain Boundary、SBI

参考答案：

因为 TA 和 OP-TEE Core 处于同一个受隔离的 OP-TEE Domain 中。

User TA 在 U-mode，OP-TEE Core 在 S-mode。

如果 M-mode 已经将 U-mode ECALL Exception 委托给 S-mode，TA 的 `ecall` 可以直接进入 S-mode Trap Handler。

```
User TA U-mode
    |
    | ecall
    v
OP-TEE S-mode
```

而 Linux Domain → OP-TEE Domain 的转换涉及两个相互隔离的 Supervisor Domains，需要更高权限的 Domain Manager 参与。

```
Linux S-mode
    |
    v
OpenSBI M-mode
    |
    v
OP-TEE S-mode
```

二者的安全边界不同。

进一步的问题是：如果系统错误配置了 `medeleg`，使 U-mode ECALL 进入 M-mode，会发生什么？

这可能导致异常进入错误的服务处理路径，并破坏预期的 OP-TEE Syscall ABI，需要结合实际 Delegation 和 Domain 配置分析。

## Q3. 为什么 OP-TEE 在 Trap Entry 中交换 `tp` 和 `sscratch`？是否可以直接使用当前 `sp`？

考察：RISC-V Assembly、Per-Hart State、Trap Entry

参考答案：

不能直接相信 User SP。

因为 Trap 来自 U-mode 时，`sp` 属于 User TA，可能是损坏的或者攻击者构造的值。

OP-TEE 需要一个能够可靠定位 Kernel State 的入口机制。

当前实现使用：

```
csrrw tp, sscratch, tp
```

通过交换：

```
Kernel Core-local Pointer
User Thread Pointer
```

使 Trap Handler 获得 Kernel Core-local 数据结构。

随后通过 Core-local 数据和 Trusted Thread Context 找到 Kernel Stack。

这是一种安全的入口协议设计，但前提是 S-mode 已正确设置 `sscratch`，而且 User Mode 无权写入这个 CSR。

Staff-level 延伸：如果同一 hart 在尚未完成 GP/TP 切换时再次 Trap，应如何保证 Nested Trap 安全？

## Q4. 一个 User TA 执行 `ecall` 后，为什么普通 Syscall 需要 `sepc += 4`，而 FP First-use Trap 不应该增加 `sepc`？

考察：Trap Semantics、Instruction Retry、Exception Return

参考答案：

因为这两种 Trap 具有不同的执行语义。

对于普通 Syscall：

```
ecall
```

内核已经处理了请求，因此需要返回下一条指令。

RISC-V `ecall` 长度为 4 Bytes，所以：

```
sepc += 4;
```

对于 FP First-use Trap：

```
fld ft0, 0(a0)
```

由于 FS=Off，该指令没有成功执行。

内核启用 FP Unit 后，需要重新执行原指令，所以 `sepc` 保持不变。

进一步地，如果是可恢复 Page Fault，也可能需要重试原指令；如果是不可恢复的 User Fault，则应终止 TA，而不是简单跳过出错指令。

因此：

Trap Handler 不能采用一条统一的「sepc += 4」规则处理所有异常。

## Q5. 如果一个 User TA 调用另一个 User TA，需要保存哪些状态？是否一定要执行 OpenSBI Domain Switch？

考察：Nested TA Call、Memory Context、Session Lifecycle

参考答案：

通常不需要 OpenSBI Domain Switch，因为二者仍位于同一个 OP-TEE Domain。

但需要维护：

- Caller TA 的执行现场。
- 当前 Session Stack。
- TA A 和 TA B 的 User VM Context。
- User Stack 和 Kernel Stack 状态。
- FP/Vector Context Ownership。
- 调用参数和权限。
- TA B 返回后的恢复路径。

TA A 与 TA B 可以使用相同 VA，但映射到不同 PA。

因此不能只保存 GPR，就认为完成了 TA Context Switch。

另外，TA B 可以通过正常 API 返回错误；如果 TA B Panic，则返回路径还需要处理 `TEE_ERROR_TARGET_DEAD`。

## Q6. 某个 Secure TA 在单客户端测试中运行正常，但增加到 8 个 Linux Client 后出现随机数据泄露。如何定位？

考察：Session Isolation、Concurrency、Security Debugging

参考答案：

首先建立最小复现条件，再检查：

1. TA 是否为 Single Instance。
2. 是否允许 Multi-session。
3. TA 是否将 Session 私有状态错误地保存在 Global Variable。
4. 是否存在 Shared Memory Buffer Reuse。
5. 是否存在错误的 Context Ownership。
6. 是否有 FP/Vector State 保存问题。
7. 是否存在不正确的锁、引用计数或释放后访问。

例如：

```
static uint8_t signing_key_cache[64];
```

如果该缓存包含客户端专属数据，却被不同 Session 共享，可能产生 Cross-session Leakage。

不能仅仅通过增加一个大锁就认为问题解决了。

需要确定状态本来应当是 per-Session、per-TA Instance，还是允许全局共享。

## Q7. 为什么不能单纯通过 `sstatus.FS == Dirty` 决定是否保存 FP Context？

考察：RISC-V FP Architecture、Security Context Switching

参考答案：

FS 是扩展状态管理字段，但不表示当前物理寄存器属于哪个 TA。

例如：

```
TA A
    |
    | FP state active
    v
S-mode Handler
    |
    | Status manipulation
    v
TA B
```

如果只通过 FS 判断是否发生过修改，却没有记录物理寄存器当前的 Owner，就可能错误地恢复或跳过保存。

正确的设计要维护扩展状态所有权，并在所有权交接前完成必要的状态保护。

尤其需要覆盖：

- TA ↔ TA。
- TA ↔ Kernel。
- REE ↔ TEE。
- Syscall。
- RPC。
- Foreign Interrupt。
- SMP 执行路径。

此外，不应该把 Lazy Enable 和 Lazy Save 混为一谈。

这是当前 RISC-V OP-TEE FP/Vector Context 设计的重要背景。

## Q8. RISC-V Vector Extension 中，为什么 `v0–v31` 之外还需要保存 `vl`、`vtype` 和 `vstart`？

考察：RISC-V V Extension、Precise Exception、Execution Semantics

参考答案：

仅保存 Vector 数据不足以恢复 Vector 指令的执行语义。

例如：

```
vtype = e32, m1
vl    = 8
```

表示当前向量操作使用特定的元素宽度、寄存器分组与元素数量。

如果恢复时：

```
vtype = e64, m2
vl    = 4
```

即使 v0–v31 内容完全相同，下一条 Vector Instruction 的行为也不同。

`vstart` 更加关键，因为它用于描述向量指令的元素执行起点。

如果在异常恢复场景中不正确恢复 `vstart`，可能导致某些元素被重复处理或者跳过。

Staff-level 延伸：当 Hart 0 和 Hart 1 的 VLEN 不同时，是否可以自由迁移同一个 Vector TA？

不能默认可以。必须保证执行能力和保存格式兼容，必要时限制迁移或禁止异构 Vector 配置下的这种执行路径。

## Q9. 如果 User TA A 和 TA B 的 User Virtual Address 都是 `0x40000000`，如何保证它们不会访问到相同物理内存？

考察：Sv39、ASID、TLB、SMP

参考答案：

因为 VA 本身不唯一标识 Physical Memory。

不同的 User Address Space 可以映射：

```
TA A:
    VA 0x40000000 → PA 0x90000000

TA B:
    VA 0x40000000 → PA 0xA0000000
```

安全性建立在正确的 User Mapping、页表权限和地址转换同步之上。

切换 TA 时，需要确保当前执行环境使用正确的页表或用户地址空间配置。

如果 TLB 仍保留旧的翻译结果，还需要依据 ASID 使用方式和 RISC-V Address Translation Synchronization 规则处理。

此外，在多 hart 场景中，本地 `SFENCE.VMA` 不自动保证其他 Hart 的旧翻译全部失效。

因此真正的隔离验证必须包括 Remote TLB Synchronization。

## Q10. 一个 TA 的 Shared Memory 参数已经通过地址检查，是否就可以在整个 Syscall 中反复读取？

考察：TOCTOU、Shared Memory、User Pointer Validation

参考答案：

不一定。

首先需要区分地址有效性与数据一致性。

地址检查可以证明某个范围在检查时具备特定的访问权限，但不等于 Shared Memory 内容不可变化。

如果某块 Shared Memory 可以由 REE 修改，那么：

```
Validate → Linux Modifies → Use
```

就可能发生 TOCTOU。

对于安全敏感的 Metadata，推荐先形成经过验证的稳定快照，再在可信内存中处理。

但即使进行了 `copy_from_user()`，也需要结合映射生命周期、并发和资源状态进行检查。

在真实工业项目中，长度、指针、偏移、权限标记及 Buffer Owner 都应纳入威胁模型。

## Q11. 一个 TA 发生 Store Page Fault，为什么应该返回 `TEE_ERROR_TARGET_DEAD`，而不是直接重启 OP-TEE？

考察：Fault Containment、Kernel Recovery、Security Boundary

参考答案：

User TA 属于低特权执行实体。

OP-TEE Core 应尽可能限制它的错误影响范围。

当不可恢复的 TA Fault 发生时，可以通过：

```
Save Exception Context
    |
Mark TA Panicked
    |
Construct S-mode Unwind Context
    |
Return to Kernel
    |
Release Resources
    |
Report TEE_ERROR_TARGET_DEAD
```

处理异常。

而当 OP-TEE Core 自身出现严重异常时，则可能无法保证 TCB 完整性，需要采取更严格的措施。

进一步要考虑：TA Panic 时是否存在尚未释放的 Lock、Crypto Key Handle、Shared Memory 和 Nested Session。

## Q12. 在一块 16-Hart RISC-V SoC 中，TA A 正在 Hart 0 运行，TA B 正在 Hart 1 运行。你准备增加动态 User Page Mapping，必须关注哪些并发问题？

考察：SMP、MMU、TLB Shootdown、Synchronization

参考答案：

至少需要考虑：

第一，Page Table Modification 的原子性。

不能让其他 Hart 观察到不符合设计的中间状态。

第二，TLB Synchronization。

修改 PTE 并不自动代表所有 Hart 的旧翻译均已失效。

第三，Address Space Identity。

需要确认 ASID 或等价机制的管理是否正确。

第四，Mapping Lifecycle。

如果某个 Mapping 被 Unmap，其他 Hart 是否仍在使用对应物理内存？

第五，锁与 Memory Barrier。

确保 Page Table 更新、Shootdown 请求、完成确认和 Memory Reuse 的顺序满足架构要求。

第六，安全性。

旧 Mapping 的残留不应该允许 TA 访问已经重新分配给其他 TA 的 Physical Page。

这是一道典型的 Kernel Maintainer 级别问题，因为错误可能只在高并发或少量特定时间窗口内发生。

## Q13. 你在 RISC-V OP-TEE 中发现 FP Context Switch 成本很高，准备使用 Lazy Switching 优化。如何设计 RFC 才可能获得 upstream 接受？

考察：Architecture Design、Performance、Security Review、Upstream

参考答案：

不能只提交：

> Lazy FP Switching improves performance by 30%.

应该首先定义系统执行模型与安全前提。

RFC 至少需要包含：

1. 当前 Context Management 的执行路径。
2. 测试平台、CPU 扩展和编译配置。
3. 当前性能瓶颈的定量数据。
4. 新方案的 Owner Tracking State Machine。
5. TA/Kernel/REE 之间所有 FP Unit 交接点。
6. First-use Trap、Nested Trap、RPC、Foreign Interrupt 的处理方式。
7. 安全不变量。
8. 回归测试和跨架构影响。
9. 性能对比及最坏延迟。
10. 回滚策略与不支持配置的处理。

最困难的部分其实不是写 FP Save/Restore 汇编。

而是证明 不存在某个没有覆盖的执行路径，能够使一个 Context 观察到另一个 Context 的寄存器内容。

这也是为什么此类优化的评审难度通常较高。

## Q14. 有人提出将所有 RISC-V TA 的 FP/Vector Context 统一放到 OpenSBI 管理，因为 OpenSBI 是 M-mode，权限更高。你如何评价？

考察：TCB Design、Layering、Domain Isolation、Responsibility Boundaries

参考答案：

我不认为这是一个默认合理的架构选择。

首先要区分两种上下文：

Domain Context

Linux Domain 与 OP-TEE Domain 之间的切换确实可能需要 M-mode 参与保存和恢复状态。

User TA Context

TA A 和 TA B 都位于 OP-TEE Domain 内部。它们的执行状态由 OP-TEE Core 管理通常更加合理。

如果将每个 User TA 的上下文管理下沉到 OpenSBI，会带来：

- M-mode 需要了解 OP-TEE 的 TA Lifecycle。
- OpenSBI TCB 增大。
- 增加与 OP-TEE ABI 的耦合。
- 引入更多跨特权层调用。
- 使 TA 级状态管理与 Domain 级状态管理混合。
- 可能增加 Context Switch Latency。

合理的分层应是：

```
OpenSBI:
    Domain Isolation
    Domain-level CPU State

OP-TEE:
    Trusted Thread
    User TA Execution
    TA-level FP/Vector Context
```

当然，如果硬件安全架构要求某些状态只能由 M-mode 访问或保存，就需要定义明确的跨层接口。

但不应仅因为 M-mode 权限更高，就把所有状态管理放到 OpenSBI 中。

更高的特权并不自动意味着更合理的软件职责划分。

# 本章知识掌握检查

你可以用下面的清单评估自己是否真正掌握 Chapter 12。

Chapter 12 验收清单

0 / 10 项

ELF / PIE / Loader

能够解释 TA 验证、ldelf、PT_LOAD、Relocation、ASLR 和 W^X

U-mode Entry

能够从 thread_enter_user_mode() 分析到最终 sret

Stack / TLS / GP / TP

能够解释 Kernel/User SP 切换及 sscratch 与 tp 的交换

Syscall / Trap

知道 OP-TEE 的 t0/t1 ABI，理解 ecall、sepc 和返回路径

Session / Context

能区分 TA Instance、Session、Trusted Thread 和 CPU Context

FP / Vector

理解 FS/VS、扩展状态所有权、首次使用及寄存器保存恢复

Eager / Lazy

能够解释性能与安全取舍，并设计对应基准测试

State Isolation

能够分析 GPR、FP、Vector、TLB、共享内存的数据泄露风险

Exception Recovery

能够解释 abort_from_user 到 thread_unwind_user_mode

Engineering

能够构造 xtest、GDB 实验和可提交 upstream 的回归测试

重置进度

## 本章最终结论

我认为 Chapter 12 最重要的并不是记住 `ldelf`、`thread_enter_user_mode`、`thread_scall_handler` 等几个函数。

而是建立以下五个认知：

第一，User TA 是一个真正的低权限执行实体。 它有自己的 ELF、虚拟内存、Runtime、Session、执行现场和系统调用机制。

第二，OP-TEE S-mode Kernel 对 User TA 承担类似操作系统内核的职责。 包括加载、内存管理、权限控制、系统调用、异常恢复和生命周期管理。

第三，TA Context Switch 不只是 GPR Save/Restore。 它还可能涉及地址空间、GP/TP、FP/Vector、Session、资源和多 Hart 的一致性。

第四，RISC-V 的 FP/Vector Context Management 属于安全关键代码。 其状态所有权和保存恢复规则必须能够抵御不同 TA 或 Domain 之间的数据泄露。

第五，理解正常执行路径只是起点。 真正的 Staff Engineer 和 Maintainer 还必须能够分析异常路径、嵌套调用、资源清理、并发竞态和隔离不变量。

完成本章后，我建议你把 `core/arch/riscv/kernel/thread_rv.S`、`thread_arch.c`、`abort.c` 和 `core/kernel/user_ta.c` 作为重点源码，实际走通一次：

`User TA → ecall → S-mode OP-TEE → Syscall Handler → sret → User TA → Return/Panic → S-mode Unwind`

如果能独立解释这条路径中每一次 `sp`、`gp`、`tp`、`sscratch`、`sepc`、`sstatus` 的变化，并证明 FP/Vector 和 TA 内存状态不会跨上下文泄露，那么你对 RISC-V User TA Execution 的理解就已经达到可以参与深入 Code Review 的水平。

核心参考源码： [RISC-V Thread Assembly](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_rv.S?utm_source=chatgpt.com)、[RISC-V Thread Architecture](https://github.com/OP-TEE/optee_os/blob/master/core/arch/riscv/kernel/thread_arch.c?utm_source=chatgpt.com)、[User TA Execution](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/user_ta.c?utm_source=chatgpt.com)、[Syscall Dispatcher](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/scall.c?utm_source=chatgpt.com)、[RISE RP022 Milestone 7 — FP/Vector Context Management](https://riscv-optee-docs-c9fea0.gitlab.io/rp022/milestone7/?utm_source=chatgpt.com)。
