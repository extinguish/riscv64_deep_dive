# **Chapter 03 — OP-TEE OS 源码树、核心数据结构与架构导航**

OP-TEE OS Internals & Architecture · Staff Engineer Level

本章源码基线：**OP-TEE 4.11.0（2026-10-16）**，Git Commit：`f54190f22611dd5ad5d22546fd4209aa100f56bd`。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



## **本章的核心目标**

这一章我们不急于把 `thread.c`、`tee_ta_manager.c` 或 `vm.c` 的每一个函数逐行读完。后续章节会分别深入它们。

Chapter 03 要解决一个更基础、也更重要的问题：

**面对 OP-TEE OS 这样一个跨越 Kernel、TA Runtime、Memory、Crypto、Storage、Driver 和多个 CPU Architecture 的大型代码库，如何建立一套可靠的源码理解与定位方法？**

例如，假设未来有人给你一个任务：

> 修改 OP-TEE，使多个 User TA 可以更高效地访问一个新的 Secure Hardware Crypto Engine，同时保证隔离、并发访问以及异常恢复的正确性。

你应该能够在研究具体寄存器和算法之前，就知道需要调查：

- 哪些源码目录涉及 TA 调用、Crypto Framework、Driver 和 Memory。
- 哪些头文件定义了跨模块接口。
- 哪些数据结构维护 TA Context、Session、Memory 和 Driver State。
- 哪些模块拥有资源，哪些模块只是临时引用。
- 哪些函数是调用入口，哪些函数负责最终释放。
- 哪些代码是 Architecture-Neutral，哪些必须由 Arm/RISC-V 分别实现。
- 编译配置发生变化时，真实执行的代码路径是否也发生变化。

这才是本章需要形成的能力。

### **学习完成标准**

| 能力                   | 验收要求                                 |
| -------------------- | ------------------------------------ |
| Source Navigation    | 5 分钟内定位陌生功能的主要实现和接口                  |
| Architecture Mapping | 从源码解释模块职责和依赖                         |
| Data Modeling        | 区分 Thread、TA Context、Session、VM、MOBJ |
| Lifecycle Analysis   | 找到对象的创建、使用、共享、销毁路径                   |
| Code Investigation   | 结合 Build、Call Graph、History 判断真实代码行为 |
| Impact Analysis      | 对一处修改提出可信的影响范围假设和验证方法                |

本章按以下主线展开：

1. OP-TEE 工程源码树与设计理念。
2. Core 内部模块的组织方式。
3. 关键接口与头文件的架构作用。
4. 核心数据结构和生命周期。
5. 真实业务调用链中的数据结构关系。
6. 大型 C/Assembly 工程的源码研究方法。
7. 实战研究与 Change Impact Analysis。
8. Staff Engineer 级思考题及参考分析。

## **3.1 从源代码目录理解 OS 架构**

### **3.1.1 第一原则：源码目录不是运行时架构**

我们在 Linux Kernel、NuttX、Zephyr 等系统中都会看到大量按子系统划分的目录。

但必须区分三个层面的概念：

- **Source Organization：** 文件放在哪个目录。
- **Build Organization：** 哪些文件编译进哪个二进制。
- **Runtime Architecture：** 运行时对象如何交互，代码位于什么特权级、地址空间和安全域。

这三个层面并不总是严格一一对应。

比如：

`lib/libutee/` 位于 OP-TEE OS 仓库，但是它主要为 User TA 提供用户态 API，并不意味着它是 Secure Kernel 的一部分。

类似地，`core/pta/` 中的 PTA 是一种 Trusted Service，但它通常直接运行在 Secure Kernel 的权限与地址空间中，不能把它理解为普通的用户态应用。

再如，`core/arch/arm/` 包含架构相关实现，但某项通用功能可能同时涉及 `core/kernel/`、`core/mm/` 和架构 Backend。

**所以，看见一个目录，只能知道代码的组织位置，不能直接判断它的执行权限、资源所有权和安全边界。**

### **3.1.2 OP-TEE 4.11.0 的顶层目录**

以下目录已经按照上游 4.11.0 tag 核对。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google+4



```
optee_os/
├── core/                  # Secure Kernel
│   ├── arch/              # Architecture / Platform
│   ├── crypto/            # Crypto Framework
│   ├── drivers/           # Secure Hardware Drivers
│   ├── include/           # Core interfaces
│   ├── kernel/            # Kernel infrastructure
│   ├── lib/               # Core-only libraries
│   ├── mm/                # Memory management
│   ├── pta/               # Pseudo Trusted Applications
│   ├── tee/               # TEE internal services
│   └── tests/             # Core-side tests
│
├── ldelf/                 # User TA ELF loader
├── lib/                   # Shared and user libraries
│   ├── libdl/
│   ├── libmbedtls/
│   ├── libunw/
│   ├── libutee/
│   └── libutils/
│
├── ta/                    # TAs and TA build support
│   ├── arch/
│   ├── mk/
│   ├── avb/
│   ├── pkcs11/
│   ├── remoteproc/
│   └── ...
│
├── mk/                    # Common build framework
├── scripts/               # Build/signing utilities
├── keys/                  # Development key material
├── Makefile
├── CHANGELOG.md
└── MAINTAINERS
```

这里有三个需要特别留意的地方。

**第一，**`optee_os`**&#x20;不等于完整的 OP-TEE 软件栈。**

Linux TEE Driver、`libteec`、`tee-supplicant` 和 `xtest` 主要分别位于 Linux、`optee_client`、`optee_test` 等工程中。研究端到端问题时不能只搜索 `optee_os`。

**第二，**`core/`**&#x20;不等于所有安全执行代码。**

`ldelf` 和 User TA 也是 Secure World 软件，但它们通常采用与 Secure Kernel 不同的权限与地址空间。

**第三，**`core/tests/`**&#x20;不等于整个 xtest。**

`xtest` 测试框架属于独立的 `optee_test` 工程；某些测试依赖 Core 内部测试功能、PTA 或 User TA。因此查找测试目标时，需要跨工程定位。

### **3.1.3 用两个维度重新组织源码**

我们把上面目录重新按照运行位置整理：

| 逻辑组件                  | 主要源码                                  | 典型执行环境            |
| --------------------- | ------------------------------------- | ----------------- |
| Secure Kernel         | `core/kernel/`、`core/tee/`、`core/mm/` | Secure Privileged |
| Kernel Crypto/Driver  | `core/crypto/`、`core/drivers/`        | Secure Privileged |
| Pseudo TA             | `core/pta/`                           | Secure Privileged |
| User TA Loader        | `ldelf/`                              | Secure User Mode  |
| User TA Runtime       | `lib/libutee/`、部分 `ta/`               | Secure User Mode  |
| Platform Backend      | `core/arch/`                          | 对应硬件特权环境          |
| Linux Client Services | 其他仓库                                  | Normal World      |

这是更有意义的架构视图，因为它把源文件所在位置与执行时的安全域联系起来。

其中，`Secure Privileged` 和 `Secure User Mode` 是逻辑分类，并不表示所有 Arm、RISC-V 实现使用完全相同的硬件特权模式。

# **3.2 深入&#x20;**`core/`**：Kernel 内部是如何组织的？**

先看一张按照职责重新整理的图。

OP-TEE Core 逻辑模块关系

**External Entry / Transport**

OP-TEE Message ABI · SMC · FF-A

Kernel Infrastructure

Thread · Boot · Session · Synchronization

core/kernel/

TEE Services

Syscall · Storage · Crypto Operations

core/tee/

Memory Infrastructure

MOBJ · VM · MMU · Page Allocation

core/mm/

Hardware Services

Crypto Backend · Device Drivers

core/crypto/ · core/drivers/

**Architecture & Platform Backend**

CPU · Trap · MMU · Interrupt · SBI/SMC · Platform

core/arch/

逻辑依赖示意，并非每条运行路径都按图中自上而下的顺序调用。

## **3.2.1&#x20;**`core/kernel/`**：内核运行基础设施**

这个目录最值得关注，因为它管理 OP-TEE 执行过程中的大量关键状态。

在 4.11.0 中，可以看到以下源码：

| 文件                       | 核心职责                    |
| ------------------------ | ----------------------- |
| `boot.c`                 | 通用启动初始化                 |
| `initcall.c`             | 初始化函数执行框架               |
| `thread.c`               | 通用 Thread 状态、线程资源       |
| `tee_ta_manager.c`       | TA Context、Session 生命周期 |
| `ts_manager.c`           | Trusted Service 通用抽象    |
| `user_ta.c`              | User TA 运行与 Context 管理  |
| `user_mode_ctx.c`        | User Mode Context 的管理   |
| `pseudo_ta.c`            | PTA Runtime 与注册支持       |
| `ldelf_loader.c`         | 配合 `ldelf` 的加载流程        |
| `ldelf_syscalls.c`       | `ldelf` 专用内核接口          |
| `scall.c`                | 系统调用分发相关逻辑              |
| `mutex.c`、`wait_queue.c` | 同步与等待                   |
| `lockdep.c`              | 锁依赖分析                   |
| `interrupt.c`、`notif.c`  | 中断与 Notification        |
| `dt.c`、`dt_driver.c`     | Device Tree 与驱动发现       |
| `pm.c`                   | Power Management        |

这些文件均能在 4.11.0 的 `core/kernel/` 中找到。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这里我们要纠正一个容易形成的误解：

`kernel/`**&#x20;并不意味着“所有操作系统内核功能都集中在这里”。**

例如：

- Thread 的通用状态实现位于 `core/kernel/thread.c`。
- CPU 寄存器保存和具体执行切换可能在 `core/arch/arm/kernel/` 或 `core/arch/riscv/kernel/`。
- TA VM 管理位于 `core/mm/vm.c`。
- 进入 TA 时触及的用户态库位于 `lib/libutee/`。

这意味着一个完整的 Thread Execution Path 天然会跨多个目录。

从维护者视角来看，我们应该把 `core/kernel/` 当成 **OS 运行基础设施的组织中心**，而不是所有内核实现的物理边界。

## **3.2.2&#x20;**`core/tee/`**：TEE 服务与接口实现**

这个目录特别容易产生误解。

早期接触 OP-TEE 时，可能会认为 `core/tee/` 就是“TEE Core 的全部实现”。

实际上，在当前源码组织中，它更偏向 TEE 业务服务层。

例如：

| 文件                       | 主要内容                   |
| ------------------------ | ---------------------- |
| `entry_std.c`            | 标准 OP-TEE Message 请求处理 |
| `tee_svc.c`              | TEE 内核服务基础接口           |
| `tee_svc_cryp.c`         | GP Crypto API 的内核侧实现   |
| `tee_svc_storage.c`      | GP Storage API 的内核侧实现  |
| `tee_obj.c`、`tee_pobj.c` | 对象和持久化对象管理             |
| `tee_ree_fs.c`           | REE FS Backend         |
| `tee_rpmb_fs.c`          | RPMB FS Backend        |
| `tee_fs_key_manager.c`   | Secure Storage 密钥管理    |
| `fs_htree.c`             | 文件系统完整性树               |
| `tee_fs_rpc.c`           | Storage RPC 接口         |
| `tee_ta_enc_manager.c`   | 加密 TA 的管理支持            |

这些是 4.11.0 中的实际文件位置。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



注意 `tee_svc_cryp.c` 和 `core/crypto/` 的职责区别。

假设 User TA 调用：

```
TEE_DigestDoFinal(...)
```

这至少涉及三个不同抽象：

```
User TA
  |
  v
libutee
  |
  |  TEE API / syscall boundary
  v
tee_svc_cryp.c
  |
  |  Operation handle
  |  Parameter validation
  |  Key / state management
  v
core/crypto/
  |
  |  Crypto backend abstraction
  v
Mbed TLS / LibTomCrypt / Hardware Backend
```

`tee_svc_cryp.c` 负责的是 **TEE API 语义和安全资源管理**；`core/crypto/` 更关注底层密码学能力及其实现抽象。

这不是随意的目录划分，而是 OS 设计中的经典分层思想：

**Policy 和 Mechanism 应该尽可能分离。**

如果把所有 Key Handle 管理逻辑都放进 AES Driver，会让底层驱动直接承担 TA 隔离和用户态对象生命周期管理职责。

这不仅使驱动无法复用，也容易让硬件实现细节影响系统安全策略。

## **3.2.3&#x20;**`core/mm/`**：不是简单的 MMU 配置目录**

4.11.0 主要文件包括：

```
core/mm/
├── boot_mem.c
├── core_mmu.c
├── file.c
├── fobj.c
├── mobj.c
├── mobj_dyn_shm.c
├── page_alloc.c
├── pgt_cache.c
├── phys_mem.c
├── tee_mm.c
└── vm.c
```

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google

可以把它们划分成四种职责：

| 层次      | 代表文件                         | 核心问题           |
| ------- | ---------------------------- | -------------- |
| 物理与启动内存 | `boot_mem.c`、`phys_mem.c`    | 内存从哪里来？        |
| 内存对象    | `mobj.c`、`fobj.c`            | 这块内存是什么资源？     |
| 虚拟内存管理  | `vm.c`、`core_mmu.c`          | 内存如何映射和访问？     |
| 页资源管理   | `page_alloc.c`、`pgt_cache.c` | 页表和页资源如何分配与回收？ |

我们先建立一个非常重要的区别：

**Memory Object 不等于 Virtual Mapping；Virtual Mapping 也不等于 Physical Memory Ownership。**

例如，同一块 Registered Shared Memory 可能被映射到 Secure Kernel 或 User TA 的某个虚拟地址区域。

MOBJ 描述的是内存对象及其相关操作；VM Region 描述某个地址空间中的映射关系。

它们可能具有不同的生命周期。

后面学习 MOBJ、VM、Pager 时，这个区别极其重要。

## **3.2.4&#x20;**`core/arch/`**：架构抽象的边界**

4.11.0 中包含：

```
core/arch/
├── arm/
└── riscv/
```

以 RISC-V 为例：

```
core/arch/riscv/
├── include/
├── kernel/
├── mm/
├── plat-cva6/
├── plat-sifive/
├── plat-spike/
├── plat-virt/
├── tee/
└── riscv.mk
```

其中 `kernel/` 里可以找到 `entry.S`、`thread_rv.S`、`thread_arch.c`、`arch_scall_rv.S`、`sbi.c`，甚至 `sbi_mpxy.c`、`sbi_mpxy_rpmi.c`。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google+1



这是一个很好的实际例子：

通用 `core/kernel/tee_ta_manager.c` 并不应该知道 RISC-V 的 `sstatus` 如何配置，也不应该知道一次具体的 Trap Entry 需要保存哪些寄存器。

同样，RISC-V `thread_rv.S` 也不应该承担 TA Session Reference Count 的管理职责。

正确的职责边界应当类似：

| Architecture-Neutral    | Architecture-Specific        |
| ----------------------- | ---------------------------- |
| Session 生命周期            | Trap/Exception Entry         |
| Trusted Service 抽象      | CPU Register State           |
| MOBJ 和 VM 对象模型          | Page Table Encoding          |
| Thread 通用状态             | Thread Register Save/Restore |
| Syscall Service Handler | Syscall Exception Entry      |
| Driver Framework        | 平台中断、MMIO、Cache 操作           |

当然，现实中仍然存在架构相关数据结构和通用数据结构的交叉。维护时应以实际头文件和调用路径为准。

这里有一个很值得记住的设计原则：

> 好的架构分离，不是让 Architecture-Neutral Core 完全不依赖 CPU，而是让它只依赖稳定、明确、足够抽象的 CPU 能力接口。

# **3.3 从&#x20;**`core/include/`**&#x20;理解模块契约**

当你开始修改大型 C 工程时，我建议遵循一个习惯：

**不要一开始就读&#x20;**`.c`**，先阅读定义其核心数据结构和接口的&#x20;**`.h`**。**

理由很简单。

`.c` 文件告诉你一项功能如何实现；`.h` 往往更能帮助你发现开发者希望暴露什么能力，以及哪些细节被保留在模块内部。

当然，OP-TEE 并不是严格面向对象设计的工程。一些头文件仍会暴露可修改结构体，因此头文件并不能自动保证封装边界。

## **3.3.1 三种值得区分的头文件**

**第一类：公共内核接口。**

例如：

```
core/include/kernel/
  tee_ta_manager.h
  ts_manager.h
  user_ta.h
  thread.h
  mutex.h
  dt_driver.h
```

关注它们定义的 API 和核心数据对象。

**第二类：内存子系统接口。**

例如：

```
core/include/mm/
  core_mmu.h
  mobj.h
  vm.h
  tee_mmu_types.h
  pgt_cache.h
  fobj.h
```

关注内存类型、对象引用、映射和访问权限。

**第三类：跨二进制、跨安全边界接口。**

例如：

```
core/include/optee_msg.h
core/include/optee_rpc_cmd.h
```

这类头文件尤其敏感，因为它们可能涉及 OP-TEE 与 Linux Driver、`tee-supplicant` 或其他通信端点的 ABI。

以上文件均可在当前源码树中定位。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google+2



### **为什么 ABI 头文件格外重要？**

假设我们修改一个内部函数：

```
static void update_ta_state(...);
```

如果它仅在单个 `.c` 文件内部使用，影响范围相对容易控制。

但如果修改的是：

```
struct optee_msg_arg
```

就不能只考虑 OP-TEE Core 重新编译是否成功。

还必须考虑：

- Linux Driver 是否使用相同的 ABI。
- 新旧版本的 Linux 与 OP-TEE 能否混用。
- 结构体 Alignment、Size、Field Offset 是否变化。
- Arm32、Arm64、RV64 的整数和指针宽度差异。
- FF-A 与传统 SMC Transport 是否使用相同解释方式。
- 相关输入校验是否还保持正确。

这就是为什么 **接口的影响范围不仅取决于代码所在目录，还取决于接口跨越了什么边界。**

# **3.4 核心数据结构：从对象关系理解 OS**

这是本章最重要的部分。

相比记忆 100 个函数名，我更希望你能在脑中稳定地区分下面这些对象：

`thread_ctx`、`thread_core_local`、`ts_ctx`、`tee_ta_ctx`、`user_ta_ctx`、`tee_ta_session`、`user_mode_ctx`、`vm_region`、`mobj`。

它们分别代表不同层次的状态，不能混为一谈。

## **3.4.1 先看整体数据模型**

OP-TEE 核心运行对象关系

CPU / Hart

thread_core_local：当前 CPU 的执行环境、Current Thread

Trusted Thread

thread_ctx：寄存器、状态、Kernel Stack、RPC、Thread Specific Data

thread_specific_data：当前 Trusted Service / Session Stack

Session

tee_ta_session 内嵌 ts_session

Session ID · Client Identity · Cancel · Refcount

Trusted Service Context

ts_ctx / ts_ops：通用服务抽象及操作接口

tee_ta_ctx：TA 生命周期、Busy、Panic、Refcount

User TA Context

user_ta_ctx：User Mode State · Crypto Handles · Storage Objects · Outgoing Sessions

user_mode_ctx

VM Info · ASID · Page Table Cache · User Stack

vm_region / mobj

VA · Size · Attributes · Memory Object · References

按一次请求的概念路径组织，不表示对象之间都是一对一关系或直接指针引用。多个 Session 可能引用同一 TA Context，一个 Thread 也可以先后运行不同 TA。

这张图至少要反复理解三遍。

- CPU/Hart 是硬件执行资源。
- Trusted Thread 是 Kernel Execution Context。
- Session 是调用关系。
- TA Context 是可信服务实例状态。
- User Mode Context 是用户态运行环境。
- VM/MOBJ 是内存抽象。

**它们相互关联，但生命周期完全可能不同。**

例如：

一个 CPU 可以先执行 Thread 0，再执行 Thread 1。

一个 Trusted Thread 可以处理不同 TA 的请求，也可以在某个调用中嵌套调用其他 TA。

一个 Single Instance TA 的 Context 可以服务多个 Session。

一次 Session 结束，并不意味着其所属 TA Context 必须立即销毁。

一次 RPC 导致 Trusted Thread Suspend，也不意味着 TA Context 或 Session 被销毁。

如果没有清楚地区分这些对象，后续阅读 OP-TEE 的 Context Switch、Session Manager 和 RPC 代码会非常混乱。

## **3.4.2&#x20;**`thread_ctx`**：内核执行上下文**

我们先看 4.11.0 的实际定义。

源码：`core/include/kernel/thread_private.h`

该结构体的关键成员包括：

```
struct thread_ctx
  ├── regs
  ├── state
  ├── stack_va_end
  ├── flags
  ├── user_map
  ├── have_user_map
  ├── kern_sp
  ├── rpc_arg
  ├── rpc_mobj
  ├── shm_cache
  └── tsd
```

注意，这里是依据源码整理的字段图，并非完整 C 定义；部分字段受编译配置和架构控制。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



我们逐项分析。

`regs`**：CPU 执行状态。**

它用于保存 Thread 相关的寄存器上下文，实际寄存器布局由架构相关结构定义。

这属于 Thread 被挂起后可以恢复执行的基础条件之一。

`state`**：Thread 生命周期状态。**

4.11.0 定义了：

```
THREAD_STATE_FREE
THREAD_STATE_SUSPENDED
THREAD_STATE_ACTIVE
```

其中：

- FREE：该 Thread Context 可以重新分配。
- ACTIVE：该 Thread 处于活动执行状态。
- SUSPENDED：其执行上下文被保存，等待后续恢复。

这里最值得注意的是，**SUSPENDED 不等于被 Linux 风格调度器放入普通 Sleep Queue**。

OP-TEE 的调用/返回和 RPC 模型与 Linux 内核线程调度不同，必须按照具体 Suspend/Resume Path 理解。

`user_map`**：当前用户态映射相关状态。**

它并不意味着 Thread 永久拥有某个 TA 地址空间。

Thread 与 User TA Memory Context 是两个不同对象，必须通过实际映射激活与恢复过程理解它们之间的关系。

`rpc_arg`**、**`rpc_mobj`**：RPC 相关资源。**

这也是为什么一个 Thread Context 会包含与共享内存对象有关的引用。

`tsd`**：Thread Specific Data。**

这里包含当前 Trusted Service Context、Session Stack 和其他与当前执行链相关的状态。

你应该进一步思考：

> 如果 TA-A 调用 TA-B，随后 TA-B 又触发 RPC，此时应该保存的是哪一层 Context？

答案不是简单的“只保存 TA-B 的寄存器”。

还需要保证整个可恢复执行链中涉及的 Session、TA Context、地址空间以及相应引用和状态保持一致。

### **一个重要的设计问题**

为什么 `thread_ctx` 没有直接把所有 TA 数据结构都嵌入进去？

因为 Thread 是一种执行资源，而 TA Context 是服务实例资源。

两者不应强行共享生命周期。

如果将 TA 的所有对象都存入 Thread Context，那么一次 Thread Release 就可能错误地影响仍由其他 Session 引用的 TA 实例。

这会引入非常复杂的资源所有权问题。

## **3.4.3&#x20;**`thread_core_local`**：Per-CPU 与 Per-Thread 的区别**

`thread_core_local` 是与 CPU/Core 相关的运行状态，而 `thread_ctx` 是可分配的 Trusted Thread Context。

不要假设：

```
CPU 0 <-> Thread 0
CPU 1 <-> Thread 1
```

这种永久绑定关系成立。

更合理的概念关系是：

```
CPU 0 -> Current Thread X
CPU 1 -> Current Thread Y

       可能发生变化

CPU 0 -> Current Thread Z
CPU 1 -> Current Thread X
```

是否允许某个具体上下文跨 CPU 恢复、需要哪些缓存和寄存器同步，要看相应实现和安全约束，不能单凭这张概念图认定所有迁移场景都得到支持。

另外，4.11.0 的 `core/kernel/thread.c` 已包含通用的 `threads` 和 `thread_core_local` 状态管理，并支持 `CFG_DYN_CONFIG` 下的动态配置路径，而不仅仅是一组固定的静态数组。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google+1



这里对你研究 RISC-V SMP 很有帮助：

**Per-Hart State、Per-Thread State 和 Per-TA State 是三个不同维度。**

一个 Global Lock 保护哪个维度的数据，必须明确。

## **3.4.4&#x20;**`ts_ctx`**&#x20;与&#x20;**`ts_ops`**：Trusted Service 通用抽象**

源码：

`core/include/kernel/ts_manager.h`

其中最重要的设计是：

```
ts_ctx
  ├── uuid
  └── ops -> ts_ops
```

`ts_ops` 定义了服务操作，例如：

- `enter_open_session`
- `enter_invoke_cmd`
- `enter_close_session`
- `release_state`
- `destroy`
- `handle_scall`

这些在 4.11.0 的实际 `ts_manager.h` 中均有定义。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



为什么需要这层抽象？

因为 OP-TEE 内部并非只存在一种 Trusted Service。

不同的服务可能有不同的执行方式：

- User TA 需要用户态加载和地址空间管理。
- PTA 通常在 Kernel Context 中直接执行。
- 某些安全分区相关服务具有不同的初始化与执行入口。

如果 TA Manager 直接到处判断：

```
if (is_user_ta)
    ...
else if (is_pta)
    ...
else if (is_secure_partition)
    ...
```

就很容易把不同的服务执行模型硬编码到统一的 Session 流程中。

`ts_ops` 的设计把一部分差异隐藏在运行时操作接口后面。

这是 C 语言中典型的 **Operations Table / Polymorphism**。

但注意，这不是说所有服务都完全共享一套生命周期和权限模型。通用抽象只覆盖可以共用的部分，类型专有的逻辑仍然要在对应实现中处理。

### **从这里可以学到一个高级架构技巧**

当一项新功能需要支持多个 Trusted Service Backend 时，优先检查现有接口是否已经提供合适的抽象。

不要因为一种新 Backend 具有特殊能力，就直接在 Generic Manager 中引入大量平台条件判断。

不过，也不要为了减少 `if` 而过度抽象。应该首先确认不同 Backend 是否确实具有相同的生命周期语义。

## **3.4.5&#x20;**`tee_ta_ctx`**&#x20;和&#x20;**`tee_ta_session`**：实例与调用关系**

源码：

`core/include/kernel/tee_ta_manager.h`

先看 `tee_ta_ctx` 的主要字段：

| 字段                | 含义                         |
| ----------------- | -------------------------- |
| `flags`           | TA Flags                   |
| `ts_ctx`          | 通用 Trusted Service Context |
| `ref_count`       | TA Context 引用计数            |
| `busy`            | Context 进入控制状态             |
| `is_initializing` | 初始化尚未完成                    |
| `is_releasing`    | Context 正在释放               |
| `panicked`        | TA Panic 状态                |
| `panic_code`      | Panic Code                 |
| `busy_cv`         | 等待 Context 可用的条件变量         |

再看 `tee_ta_session`：

| 字段                     | 含义                         |
| ---------------------- | -------------------------- |
| `ts_sess`              | 通用 Trusted Service Session |
| `id`                   | Session Handle             |
| `clnt_id`              | Client Identity            |
| `param`                | 当前调用参数                     |
| `cancel`、`cancel_mask` | Cancellation State         |
| `ref_count`            | Session 引用计数               |
| `refc_cv`              | 引用同步                       |
| `lock_cv`              | Session Lock 等待            |
| `lock_thread`          | 持有 Session Lock 的 Thread   |
| `unlink`               | Session 从管理列表移除相关状态        |

这些字段均来自 4.11.0 的实际定义。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



现在考虑一个实例：

```
Client A ---- Session 100 ----+
                              |
                              v
                         TA Context X
                              ^
                              |
Client B ---- Session 200 ----+
```

在允许多 Session 共享同一实例的配置下，两个 Session 可以引用同一个 TA Context。

于是产生两个不同的问题：

**TA Context 的引用计数回答：还有谁需要这个 TA 实例？**

**Session 的引用计数回答：还有谁正在使用这个 Session 对象？**

这是不同的生命周期约束。

如果只使用一个引用计数，很难同时正确表达这两种对象之间的关系。

### **更深入的思考**

假设 Client A 关闭 Session 100，同时 Client B 仍然正在 Session 200 中执行。

此时可以销毁 Session 100，但不能仅仅因为 A 关闭了 Session 就立即销毁整个 TA Context X。

同样，如果 Session 100 正好有一个 Invocation 正在执行，那么关闭操作可能需要协调正在引用 Session 的执行路径。

这就是为什么这里不仅存在 `ref_count`，还存在 `busy`、Condition Variable、Session Lock 等同步机制。

**对象的生命周期、对象是否可进入、对象是否正在执行，通常是三个不同的问题。**

这个认识非常重要。

## **3.4.6&#x20;**`user_ta_ctx`**：User TA 的真正运行状态容器**

源码：

`core/include/kernel/user_ta.h`

4.11.0 的结构很有代表性：

```
user_ta_ctx
  |
  +-- open_sessions
  |
  +-- cryp_states
  |
  +-- objects
  |
  +-- storage_enums
  |
  +-- uctx: user_mode_ctx
  |
  +-- ta_ctx: tee_ta_ctx
```

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google

从这个结构体，我们可以观察到一个重要的 OS 设计事实：

**User TA 不仅是一个 ELF Program，它还拥有一组由 Secure Kernel 管理的持久运行状态。**

例如：

`open_sessions` 表示这个 TA 作为客户端打开的其他服务 Session。

`cryp_states` 保存该 TA 的 Crypto Operation State。

`objects` 记录由 TA 打开的对象。

`storage_enums` 记录持久化存储枚举操作。

`uctx` 保存 User Mode Context。

`ta_ctx` 提供通用 TA 生命周期状态。

注意其中 `open_sessions` 并不是“所有客户端打开这个 TA 的 Session 集合”，而是**这个 TA 自己作为 Client 打开的 Session 集合**。

这是非常容易弄错的一个细节。

### **这个结构体为什么重要？**

假设一个 TA 在执行过程中发生 Panic。

如果只清理：

```
TA Code Mapping
TA Data Mapping
```

显然是不够的。

它可能还持有：

```
Crypto Operation Handles
Storage Object Handles
Outgoing TA Sessions
Storage Enumerators
User Mode VM Resources
```

因此，TA Context Destruction 必须考虑这些资源的完整生命周期。

这里可以提炼出一个通用 OS 设计原则：

> 一个 User Process/TA Context 的销毁，不只是回收它的内存，而是撤销它对所有 Kernel-managed Resources 的持有关系。

后续我们会在 TA Lifecycle 章节深入跟踪这些资源的实际释放路径。

## **3.4.7&#x20;**`user_mode_ctx`**：User TA 运行环境**

源码：

`core/include/kernel/user_mode_ctx_struct.h`

关键字段：

```
user_mode_ctx
  ├── vm_info
  ├── regions
  ├── pgt_cache
  ├── ts_ctx
  ├── entry_func
  ├── load_addr
  ├── dl_entry_func
  ├── ldelf_stack_ptr
  ├── is_32bit
  ├── stack_ptr
  └── bounce buffer metadata
```

另有部分受配置控制的状态，例如 FPU/VFP 与 Pointer Authentication。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这个结构体把 User TA 的执行入口、地址空间、栈、加载器状态及部分 CPU 扩展状态组织在一起。

我们需要明确：

`tee_ta_ctx` 主要表示 **Trusted Application 的管理状态**。

`user_mode_ctx` 主要表示 **User Mode Execution Environment**。

它们紧密相关，但不是同一个抽象。

比如，一个 PTA 可能有 TA Context，但不需要普通 User TA 的 ELF Loader 和用户态地址空间。

所以 OP-TEE 不应该要求所有 Trusted Service 都必须拥有完整的 `user_mode_ctx`。

## **3.4.8&#x20;**`vm_info`**、**`vm_region`**&#x20;与&#x20;**`mobj`

源码：

`core/include/mm/tee_mmu_types.h` 和 `core/include/mm/mobj.h`

先看 VM：

```
vm_info
  ├── regions
  └── asid

vm_region
  ├── mobj
  ├── offset
  ├── va
  ├── size
  ├── attr
  ├── flags
  └── link
```

这是 4.11.0 的实际逻辑关系。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



可以理解成：

`vm_info` 维护某个 User Mode Context 的虚拟内存布局及其 ASID。

`vm_region` 描述一段映射：

- 映射到哪个 VA。
- 映射多大范围。
- 使用哪个 MOBJ。
- 使用 MOBJ 的哪个 Offset。
- 具有哪些权限和 VM Flags。

而 MOBJ 则是另一层抽象：

```
mobj
  ├── ops
  ├── size
  ├── phys_granule
  └── refc
```

其 Operations 包括 `get_va`、`get_pa`、`get_mem_type`、`free`、`inc_map`、`dec_map` 等。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这是整套 OP-TEE 内存模型中最应该掌握的区别之一。

### **一个简单的情景分析**

假设存在一块内存资源：

```
Physical Memory:
0x90000000 - 0x90003FFF
```

一个 MOBJ 可以用于描述这块内存对象。

然后存在两个不同的映射：

```
Kernel VA:   0xFFFF000010000000  ──┐
                                   ├── MOBJ
TA User VA:  0x0000000012000000  ──┘
```

以上地址仅为说明 VA Mapping 概念的假设数据，不是 OP-TEE 实际布局。

两个 VA Mapping 可以具有不同的访问权限，也可能具有不同的生命周期。

这不意味着所有 MOBJ 类型都允许这种双重映射，实际是否合法取决于对象类型、映射权限和安全策略。

假设 User TA 的映射被删除：

```
TA VM Region removed
```

不一定意味着 MOBJ 本身应该立即释放，因为 Kernel 可能仍然持有有效引用。

反过来，如果 MOBJ 已经被释放，但某个 VM Region 仍然引用它，就可能产生 Use-After-Free。

所以必须分别追踪：

- **Object Reference：** MOBJ 是否仍然存活。
- **Mapping Lifetime：** 映射是否仍然有效。
- **Physical Resource Lifetime：** 底层物理页是否允许回收。
- **Security Ownership：** 哪些 CPU/设备/安全域仍然可以访问。

其中 `mobj_get()` 与 `mobj_put()` 管理对象引用；`mobj_inc_map()` 与 `mobj_dec_map()` 则属于映射相关的独立操作，并非完全相同的计数机制。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



如果以后你在 RISC-V 平台增加 SMMPT 这样的系统级保护机制，这种多层生命周期意识尤其重要。

仅仅撤销 CPU VA Mapping，并不自动意味着所有 DMA Master 都失去了访问能力。

# **3.5 用一条真实调用链把这些结构体串起来**

前面讨论了多个独立的数据结构。

现在我们来追踪一个真实场景，将它们连接到一起。

## **场景：Normal World 调用一个尚未加载的 User TA**

假设：

```
TEEC_OpenSession(...);
```

最终通过 Linux Driver 进入 OP-TEE。

本节以经典 OP-TEE Message ABI 路径为例；FF-A 和部分平台特定路径可能有不同的 Transport 层。

### **第一阶段：处理外部请求**

进入 Secure Kernel 后，标准消息入口会处理 Open Session 命令。

从：

```
core/tee/entry_std.c
```

可以找到：

```
entry_open_session(...)
```

该函数会完成部分参数解析和转换，然后调用：

```
tee_ta_open_session(...)
```

在 4.11.0 的实现中，`entry_open_session()` 还包含 `copy_in_params()`、Shared Memory 引用清理以及结果写回等处理。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这时，至少出现三个概念：

```
optee_msg_arg
    |
    v
tee_ta_param
    |
    v
tee_ta_session
```

注意：

`optee_msg_arg` 是外部通信 ABI。

`tee_ta_param` 是内部 TA 参数表示。

`tee_ta_session` 是内核管理的 Session。

它们不是同一个对象，也不应该随意共享完全相同的可信假设。

尤其是 Normal World 输入不能因为已经进入 Secure Kernel 就直接被视为可信数据。

### **第二阶段：TA Manager 创建 Session**

接下来进入：

```
core/kernel/tee_ta_manager.c
```

相关函数：

```
tee_ta_open_session(...)
```

它调用 Session 初始化流程，进行 TA 查找，并在需要时创建或初始化对应 Context。

实际实现还会检查参数、处理 Busy 状态，以及通过 `ts_ops` 调用具体服务的 Open Session 操作。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



内部的典型关系是：

```
tee_ta_session
    |
    +-- ts_session
            |
            +-- ctx -> ts_ctx
                         |
                         +-- ops
```

对于普通 User TA，该 `ts_ctx` 嵌入在 `tee_ta_ctx` 中，而 `tee_ta_ctx` 又嵌入在 `user_ta_ctx` 中。

因此可以通过 `container_of()` 一类方法从通用 Context 恢复到具体 Context。

这就是 C 语言内核代码中常见的 **Embedded Object + Polymorphism** 模式。

### **第三阶段：建立 User TA Context**

如果 TA 尚未加载，则需要建立 User TA Context。

相关实现位于：

```
core/kernel/user_ta.c
core/kernel/ldelf_loader.c
core/mm/vm.c
ldelf/
```

此处要处理：

- 创建 `user_ta_ctx`。
- 初始化 `user_mode_ctx`。
- 建立 `vm_info`。
- 分配或映射相关内存对象。
- 启动 `ldelf` 加载 TA。
- 验证与准备 User TA 执行环境。
- 设置 User TA Entry、Stack 和其他状态。

这些操作分布在不同源码模块中，并不由某个单一的巨大 `load_ta()` 函数完成。

### **第四阶段：进入 User TA**

最终通过 User TA 执行接口切换到用户态。

这一过程会涉及：

```
user_ta.c
  |
  v
user_mode_ctx
  |
  v
thread_enter_user_mode(...)
  |
  v
Architecture-specific Assembly
  |
  v
User TA Entry
```

`thread_enter_user_mode()` 是内核向 User Mode 转移的重要接口，其实现与底层寄存器、栈和特权级切换相关。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这里请特别注意：

**当前执行的 Trusted Thread 并没有因为进入 User TA 就自动变成另一个独立调度的 Linux 式线程。**

它仍然是整个 TEE Request Execution Chain 的一部分。

### **第五阶段：Session 返回并保持资源**

Open Session 执行完成后，返回 Session ID。

但是，TA Context、Session、部分内存对象和运行时资源可能继续存活，以便后续 Invoke Command。

因此：

```
Return from OpenSession
    !=
Destroy TA Context
```

同样：

```
Trusted Thread released for reuse
    !=
Session closed
```

这两组区别是后续研究 TA Lifecycle 和 Thread Lifecycle 的基础。

## **将整个过程整理成对象生命周期表**

| 对象                    | 创建/获取时机          | 通常的生命周期边界                     |
| --------------------- | ---------------- | ----------------------------- |
| Trusted Thread        | 请求进入后的分配或恢复      | 本次执行完成并 Release，或 Suspend 后恢复 |
| `tee_ta_session`      | 建立 Session       | Close、异常清理等                   |
| `tee_ta_ctx`          | 创建/复用 TA 实例      | 根据 Flags、引用和状态决定              |
| `user_mode_ctx`       | 初始化 User TA 运行环境 | 对应 User TA Context 销毁         |
| `vm_region`           | 加载 TA 或映射调用参数    | 映射类型决定，可能是短期或实例生命周期           |
| `mobj`                | 取得或创建内存对象        | 最后一个有效引用释放                    |
| Crypto/Storage Handle | TA 调用对应服务        | 显式关闭或 TA 资源清理                 |

这是逻辑归纳表，具体路径还需要结合 TA Flags、错误处理和相关配置检查。

这也是我建议每次阅读某个模块时建立的第一张表：**不要只记录 Call Graph，一定要记录 Object Lifecycle。**

# **3.6 大型 C 工程中的四种架构组织模式**

前面我们已经看到了大量结构体。

现在需要换一个角度：**为什么开发者选择这些数据结构和实现方式？**

掌握这些 C 语言架构模式，往往比记忆具体函数名称更有价值。

## **3.6.1 Embedded Object：通过结构体嵌入实现继承式抽象**

以 `user_ta_ctx` 为例：

```
user_ta_ctx
    |
    +-- tee_ta_ctx
            |
            +-- ts_ctx
```

这是一种非常典型的内核设计模式。

其中：

- `ts_ctx`：最通用的 Trusted Service Context。
- `tee_ta_ctx`：在通用 Context 之上增加 TA 管理状态。
- `user_ta_ctx`：增加 User TA 所需要的地址空间和资源状态。

它们不是三个独立分配、通过指针随意连接的对象。

而是通过结构体嵌入来组织。

假设某个函数只知道：

```
struct ts_ctx *ctx;
```

它可以通过通用 `ts_ops` 操作这个服务。

如果能够确认它是 User TA，则可以通过 `container_of()` 恢复具体的 `user_ta_ctx`。

这种设计提供了一定程度的多态，同时没有引入 C++ Runtime 或虚函数机制。

但是它存在一个维护风险：

**对象的真实类型必须与类型转换假设一致。**

如果一个新 Backend 错误地把普通 `ts_ctx` 当成 `user_ta_ctx`，就可能发生非法内存访问。

因此，在 Code Review 中，`container_of()`、类型判定函数与对象生命周期应该一起检查。

## **3.6.2 Operations Table：通过函数指针隔离实现差异**

典型例子：

```
ts_ops
mobj_ops
```

我们可以把这种模式理解成 C 语言版本的 Interface。

例如 MOBJ：

```
/* 教学性伪代码：非上游原样定义 */

struct memory_ops {
    void *(*get_va)(void *obj, size_t offset);
    void (*release)(void *obj);
};
```

调用方只使用抽象接口，而不用知道底层内存具体来自哪里。

这对 OP-TEE 非常重要，因为内存可能来自：

- Secure RAM。
- Reserved Shared Memory。
- Registered Shared Memory。
- FF-A Memory Transaction。
- Pager 管理的文件映射内存。

但是，**抽象接口相同，不代表所有 Backend 的语义完全相同**。

例如，某些 MOBJ 的 `get_va` 操作可能有不同的映射限制。

如果调用方假定所有 MOBJ 始终存在连续可访问的 Kernel VA，就可能破坏原有抽象。

因此，阅读 Operations Table 时要特别关注：

- 哪些 Callback 是必需的。
- 哪些 Callback 是可选的。
- 不同 Backend 的行为是否一致。
- 错误返回有什么语义。
- 是否允许睡眠、阻塞或 RPC。
- 谁维护 Operations Table 和对应对象的生命周期。

## **3.6.3 Intrusive Linked List：把链表节点放进对象**

在 `tee_ta_ctx`、`tee_ta_session`、`vm_region` 等结构体中，可以看到：

```
TAILQ_ENTRY(...)
```

这与 Linux 的 `struct list_head` 属于相似的 Intrusive List 思想。

对象本身携带链表节点，而不需要为每个链表元素另外分配一个容器。

例如：

```
tee_ctxes
   |
   +-- TA Context A
   |
   +-- TA Context B
   |
   +-- TA Context C
```

这看起来是简单的链表，但在并发内核中，需要回答更多问题：

谁可以遍历这个链表？

谁可以添加或删除 Context？

遍历时是否需要锁？

从链表中摘除对象之后，它是否仍然可以通过其他有效引用被访问？

摘除对象与释放对象是否可以发生在同一个临界区？

这些问题不能仅凭 `TAILQ_REMOVE()` 的调用位置作答。

**List Membership、Object Lifetime 和 Execution Eligibility 是三个需要分别分析的属性。**

这一点在 TA Manager、Session Manager、VM Region Management 中都会反复出现。

## **3.6.4 Registration Framework：Initcall、PTA、Driver**

OP-TEE 中有不少功能采用静态注册思路。

例如：

```
Initcall Registration
PTA Registration
DT Driver Registration
```

以 Driver 为例，`core/include/kernel/dt_driver.h` 中定义了 `struct dt_driver`，包含 Name、Type、Match Table、Probe Callback 等字段，同时提供注册宏。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这种设计的目的之一是：

**让具体实现模块能够声明自己，而不必让一个中央管理函数显式调用每个模块。**

但是，静态注册框架存在一个重要的阅读陷阱。

假设你搜索：

```
my_driver_probe(...)
```

可能找不到任何普通的直接函数调用。

原因是它可能通过：

```
Macro
  |
  v
Registration Object
  |
  v
Linker Section
  |
  v
Runtime Enumeration
  |
  v
Callback
```

被间接调用。

因此，遇到这种情况时，需要同时研究：

- Registration Macro。
- Registration Object 的定义。
- Linker Section。
- 注册对象集合的遍历逻辑。
- Callback 的实际执行入口。

这与 Arm/RISC-V 启动中的 Linker Symbol、Section Placement 有相似之处，但此处关注的是软件组件如何被发现和初始化。

# **3.7 OP-TEE 源码阅读的正确方法**

这一节是可以直接应用到日常开发中的内容。

我建议以后分析一个陌生 OP-TEE 功能时，不要从“打开文件然后逐行阅读”开始。

而是采用下面的调查流程。

## **3.7.1 Step 1：先确定源码版本和实际编译配置**

源码版本是第一层边界。

例如：

```
git describe --tags --always
git rev-parse HEAD
```

然后检查 Build Configuration。

OP-TEE 的构建系统通过根目录 Makefile、各级 `sub.mk` 和 `CFG_*` 选项选择编译内容。生成的 Core 配置头文件位于构建输出树的 `include/generated/conf.h` 中。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



为什么必须先检查配置？

因为源码中存在某项功能，不等于当前 Binary 已经包含它。

例如：

```
#ifdef CFG_CORE_FFA
    ...
#else
    ...
#endif
```

如果你正在分析传统 SMC Path，但只阅读了 FF-A 分支，那么 Call Graph 即使画得非常漂亮，也可能完全不适用于你的平台。

另外，`CFG_*` 的取值还可能决定某个对象是静态分配还是动态分配。

我们刚才看到的 `thread.c` 就是实例。

所以一个完整的 Source Investigation 应该同时记录：

```
Git Commit
ARCH
PLATFORM
CFG_*
Toolchain
Build Output
```

**Source Tree + Build Configuration + Runtime Context，三者结合才是有效的代码基线。**

## **3.7.2 Step 2：从接口向内寻找，而不是从实现向外猜测**

假设要研究 Session。

建议先搜索：

```
git grep -n "struct tee_ta_session"
git grep -n "tee_ta_open_session"
git grep -n "tee_ta_close_session"
```

然后按照下面的顺序：

```
Header Definition
      |
      v
Public / Internal API
      |
      v
Implementation
      |
      v
Caller
      |
      v
Backend / Callback
```

注意要同时研究两个方向：

**Forward Dependency：** 这个函数调用了什么？

**Reverse Dependency：** 谁会调用这个函数？

对于修改影响分析，Reverse Dependency 有时比 Forward Dependency 更重要。

例如，你修改了 MOBJ 的一个接口。

单纯阅读该接口内部实现，只能帮助你理解它做了什么。

但是只有分析全部调用者，才能知道它们分别依赖什么行为。

某个调用者可能依赖其错误返回值。

另一个调用者可能依赖其允许 NULL。

还有一个调用者可能假设它不会执行 Blocking Operation。

这些隐含契约经常导致跨模块回归问题。

## **3.7.3 Step 3：追踪真正的 Data Ownership**

我建议对每个重要结构体，都主动搜索以下六种操作：

```
Allocate
Initialize
Reference / Acquire
Use
Unlink / Deactivate
Release / Destroy
```

例如：

```
git grep -n "mobj_get("
git grep -n "mobj_put("
git grep -n "mobj_inc_map("
git grep -n "mobj_dec_map("
```

搜索时不要只关注名称相似的函数。

还需要关注：

- 是否通过 Operations Table 释放。
- 是否通过 `container_of()` 获得对象。
- 是否在 Panic Cleanup 中释放。
- 是否通过其他模块间接引用。
- 是否在错误路径中跳过某些步骤。

### **一种有用的资源分析表**

| 操作             | 对 MOBJ 的要求           |
| -------------- | -------------------- |
| Create         | 对象状态正确初始化            |
| Get Reference  | 引用计数正确增加             |
| Add Mapping    | 映射计数或相关映射状态正确建立      |
| Remove Mapping | 不得留下非法 VA/PA Mapping |
| Put Reference  | 不得提前释放仍在使用的资源        |
| Final Destroy  | 无有效引用与未清理的关联资源       |

注意这不是实际代码规定的唯一执行顺序，而是用于检查实现是否维护完整生命周期的模型。

## **3.7.4 Step 4：检查所有异常路径**

假设我们看到：

```
/* 教学性伪代码 */

obj = alloc_object();
if (!obj)
    return ERROR;

res = map_object(obj);
if (res)
    goto err;

res = register_object(obj);
if (res)
    goto err_unmap;

return SUCCESS;

err_unmap:
    unmap_object(obj);
err:
    free_object(obj);
return res;
```

初看没有明显问题。

但对于 Secure Kernel，需要进一步思考：

如果 `map_object()` 成功完成一部分映射，然后返回错误，会发生什么？

如果 `register_object()` 已经发布了对象指针，然后由于后续操作失败返回错误，其他 CPU 能不能看到这个未完全初始化的对象？

如果 `unmap_object()` 需要等待硬件操作完成，调用方此时是否持有 Spinlock？

如果 `free_object()` 没有清除敏感数据，会不会泄露密钥？

如果函数执行在 Early Boot 阶段，所使用的 Allocator 是否已经初始化？

这些才是从普通代码阅读进入 Staff Engineer 级别代码审查的关键。

## **3.7.5 Step 5：通过 Git History 理解设计决策**

针对陌生代码，我推荐使用：

```
git blame core/kernel/tee_ta_manager.c
```

然后进一步调查：

```
git log --oneline -- core/kernel/tee_ta_manager.c

git log -S'CFG_CONCURRENT_SINGLE_INSTANCE_TA' --all \
    -- core/kernel/tee_ta_manager.c
```

`git log -S` 特别有用，因为它可以追踪某个字符串的引入或删除历史。

如果发现某个字段看起来非常多余，不要急于删除。

它可能是为了解决以前出现过的 Race Condition、Deadlock 或 Compatibility Issue。

### **为什么历史信息如此重要？**

考虑一种情景：

你发现一段复杂的 Session Locking 逻辑，似乎可以简化成一个 Mutex。

AI 也认为这样更简洁。

但是，过去的某个 Patch 可能专门修复了嵌套调用期间不能始终持有这个 Mutex 的问题。

如果只根据当前函数的局部逻辑进行重构，就可能重新引入过去已修复的死锁。

**Code History 不只是了解“以前修改过什么”，更是发现隐藏架构约束的重要手段。**

# **3.8 建立自己的 Source Map**

我建议在本章完成以后，你能独立维护一份类似下面的源码地图。

## **3.8.1 第一层：关键功能与入口映射**

| 功能              | 建议首先调查的文件                                              |
| --------------- | ------------------------------------------------------ |
| Boot            | `core/kernel/boot.c`、`core/arch/*/kernel/boot.c`       |
| Thread          | `core/kernel/thread.c`、`thread_private.h`、架构 Thread 实现 |
| Trusted Service | `ts_manager.c`、`ts_manager.h`                          |
| Session         | `tee_ta_manager.c`、`tee_ta_manager.h`                  |
| User TA         | `user_ta.c`、`user_ta.h`                                |
| TA Loading      | `ldelf_loader.c`、`ldelf/`                              |
| Syscall         | `scall.c`、`tee_svc.c`、架构 Syscall Entry                 |
| Memory Object   | `mobj.c`、`mobj.h`                                      |
| User VM         | `vm.c`、`vm.h`、`tee_mmu_types.h`                        |
| Secure Storage  | `tee_svc_storage.c`、`tee_ree_fs.c`、`tee_rpmb_fs.c`     |
| Crypto          | `tee_svc_cryp.c`、`core/crypto/`                        |
| Device Tree     | `dt.c`、`dt_driver.c`、`dt_driver.h`                     |
| Driver          | `core/drivers/`、`core/include/drivers/`                |
| Interrupt       | `interrupt.c`、Architecture Backend                     |
| RPC             | `tee_fs_rpc.c`、Thread RPC 与消息接口                        |
| FF-A            | Architecture Transport、FF-A 接口及相关 MOBJ 实现              |

这张表不是最终的 Call Graph，而是遇到问题时的 **第一跳导航表**。

## **3.8.2 第二层：核心数据对象索引**

这张表更重要。

| 数据结构                   | 主要位置                     | 核心职责                       |
| ---------------------- | ------------------------ | -------------------------- |
| `thread_ctx`           | `thread_private.h`       | Trusted Thread 状态          |
| `thread_specific_data` | `thread.h`               | 当前 Thread 的服务执行链           |
| `ts_ctx`               | `ts_manager.h`           | 通用 Trusted Service         |
| `ts_session`           | `ts_manager.h`           | 通用 Trusted Service Session |
| `tee_ta_ctx`           | `tee_ta_manager.h`       | TA 实例管理状态                  |
| `tee_ta_session`       | `tee_ta_manager.h`       | TA Session 状态              |
| `user_ta_ctx`          | `user_ta.h`              | User TA 资源容器               |
| `user_mode_ctx`        | `user_mode_ctx_struct.h` | User Mode 执行状态             |
| `vm_info`              | `tee_mmu_types.h`        | 用户态虚拟地址空间                  |
| `vm_region`            | `tee_mmu_types.h`        | 单个映射区域                     |
| `mobj`                 | `mobj.h`                 | 内存对象                       |
| `mobj_ops`             | `mobj.h`                 | 内存对象行为接口                   |
| `dt_driver`            | `dt_driver.h`            | DT Driver 注册信息             |

你不必死记每个结构体字段，但是必须知道：

**它属于哪一层、负责什么资源、与哪些对象关联、在什么事件发生时结束生命周期。**

# **3.9 本章实战：独立完成三个源码调查任务**

我建议这一章不要急于修改内核代码。

我们先进行三次 Source Investigation，重点训练定位与推理。

## **Lab 1 — 追踪一个 TA Session 的生命周期**

研究对象：

```
tee_ta_open_session()
tee_ta_invoke_command()
tee_ta_close_session()
```

需要形成以下结果：

1. 绘制 Session 的调用链。
2. 解释 `tee_ta_session` 与 `tee_ta_ctx` 的关系。
3. 找到 Session ID 的生成、查找和销毁位置。
4. 找到 Session Reference Count 的所有主要修改路径。
5. 找到 Context Busy 的设置和清理位置。
6. 找到 User TA 和 PTA 的分发边界。
7. 分析一次 OpenSession Failure 后资源如何回收。
8. 分析一个 TA Panic 后哪些对象仍可能存在。

**验收标准：** 能够解释两条 CPU 执行链并发访问同一 TA Context 时，哪些共享状态需要同步。

## **Lab 2 — 追踪一次 Shared Memory 参数传递**

研究对象：

```
entry_std.c
    |
    v
tee_ta_param
    |
    v
mobj
    |
    v
vm_region
```

重点分析：

- 外部 `optee_msg_param` 如何转成内部参数。
- `mobj_get()` 为什么会在参数转换过程中使用。
- 为什么返回前需要释放 Shared Memory 引用。
- MOBJ 被映射到 TA VA 时，映射生命周期如何管理。
- 如果输入 Offset/Size 导致整数溢出，应在哪一层拒绝。
- 如果 Shared Memory 在 TA 执行期间被 Normal World 修改，安全模型应该如何考虑。
- 如果使用 FF-A，内存对象的查找方式有什么变化。

这里可以直接对照 `entry_std.c` 中的 `copy_in_params()` 和 `cleanup_shm_refs()`。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



**验收标准：** 能区分 Shared Memory 的数据完整性风险、对象生命周期风险以及 CPU/设备访问权限风险。

## **Lab 3 — 为一个陌生模块建立 Source Map**

选择：

```
core/kernel/dt_driver.c
```

不要先读任何二手教程。

只利用源码和官方文档完成：

- 找到公开接口头文件。
- 找到关键结构体。
- 找到 Driver Registration Macro。
- 找到注册数据如何被组织。
- 找到 Runtime Driver Probe 的调用入口。
- 找到 Probe Failure 的处理。
- 找到 Provider/Consumer 依赖处理。
- 找出哪些实现可能是 Platform-Specific。
- 设计一个新增驱动的最小修改方案。
- 给出需要验证的配置、依赖和初始化顺序。

**验收标准：** 可以向一位没有接触过该框架的高级工程师解释其架构，并指出可能出现的初始化时序问题。

# **3.10 Staff Engineer Level — 深度面试与架构推理**

这部分不考察函数名称记忆。

每一道题都围绕 Chapter 03 的主题——**源码组织、核心数据结构、接口契约与跨模块影响分析**——展开，但可以涉及并发、虚拟化、安全、硬件和架构演进等真实工程问题。

我给出 10 道题，每题都附带评审时应该体现的核心推理方向。

建议先独立思考，再展开参考分析。

01

问题 1 — 如果你要重构 TA Manager，是否应该将 Session 和 TA Context 合并？

假设当前系统中每个 TA 只允许一个客户端打开 Session。为了减少内存使用，某工程师建议把 tee_ta_session 与 tee_ta_ctx 合并成一个结构体，并删除部分 Reference Count。请评估此设计是否合理。如果未来要支持多客户端并发、嵌套 TA 调用以及 Secure Partition，会遇到什么问题？

02

问题 2 — 怎样判断一个结构体可以安全地移动到 Architecture-Neutral Core？

RISC-V Backend 中出现一套 Thread Context 结构，Arm Backend 也有类似字段。有人建议抽取公共结构体，统一两种架构。你如何决定哪些成员可以上移？怎样处理寄存器布局、Stack Pointer、FPU State、ABI 和 Assembly 中依赖的字段 Offset？

03

问题 3 — 如何证明一个 MOBJ 的释放路径不会产生 Use-After-Free？

你准备修改 Shared Memory 注册逻辑。MOBJ 同时可能被 TA VM、RPC 和某个设备驱动引用。某测试显示 OpenSession/CloseSession 执行一万次也没有内存泄漏。这样的测试能够证明实现安全吗？你还需要什么证据？

04

问题 4 — 如果所有已知 Caller 都正常，能否认定函数修改安全？

一个 Kernel Helper 过去不会阻塞。某 Patch 为了降低 CPU 使用率，将内部 Spinlock 改成 Mutex，并使函数可能 Sleep。所有 Unit Tests 通过，函数签名也没有变化。为什么这仍可能是架构级回归？请设计完整调查方法。

05

问题 5 — 如何设计一个可同时服务 User TA 和 Secure Partition 的 Crypto Service？

现有 Crypto Service 由 User TA 通过 Syscall 调用。现在 FF-A Secure Partition 也需要同样的硬件加速能力。你应该将 Crypto Driver 复制一份给 Secure Partition，还是直接复用 tee_svc_cryp.c？应怎样分层？

06

问题 6 — 从源码结构如何判断一项修改是否会影响安全启动？

某工程师只修改 core/include/mm/tee_mmu_types.h 中一个 Memory Attribute 定义，并宣称没有修改 Boot Code，因此不会影响 Boot。作为 Reviewer，你会怎样评估这个结论？

07

问题 7 — 如何识别一个隐藏在 Callback 中的死锁？

TA-A 调用 TA-B；TA-B 通过 PTA 调用 Storage；Storage 触发 RPC。某工程师在 TA Manager 入口增加了一个全局 Mutex，直到最外层 Invoke 完成才释放。为什么这看起来合理的串行化方案可能失败？如何利用 Source Map 找到问题？

08

问题 8 — 一个 ABI 没有变化的 Patch 为什么可能导致旧数据无法读取？

OP-TEE 的 Secure Storage 内部格式升级，所有 GP API、libutee Syscall 和 Storage Service 函数签名都不变。升级后旧版本创建的 Persistent Object 却不能读取。请从源码组织和兼容性角度解释，并设计升级方案。

09

问题 9 — AI 自动生成的 Source Map 可以信任吗？

AI Agent 扫描 OP-TEE 仓库，自动生成了调用图和依赖关系图，并声称某个函数只有三个 Caller。但其中一个调用路径使用 Operations Table，另一个来自 Initcall Macro，还有一个来自 Architecture Assembly。你会如何设计一套可信的 Source Analysis Pipeline？

10

问题 10 — 如果你成为 OP-TEE Core Maintainer，如何决定接受一个跨模块重构 Patch？

某 Patch 删除 400 行重复代码，将 Thread、TA Context 和 VM 的部分公共逻辑统一为一个新 Framework。性能没有回退，代码量明显减少，但涉及 20 多个文件。你应该如何评估代码简化是否值得承担跨模块风险？

### **如何判断这些题目回答得够不够深入？**

我建议评估时不要只看候选人是否说出了正确的术语。

可以用四个方面衡量。

| 维度           | 需要体现的能力              |
| ------------ | -------------------- |
| Architecture | 能否识别正确的模块和抽象边界       |
| Causality    | 能否解释问题发生的机制，而不只是描述现象 |
| Trade-offs   | 能否说明不同技术方案的代价和适用条件   |
| Verification | 能否设计实验或找到源码证据验证结论    |

优秀的 Staff Engineer 答案往往不是唯一方案。

真正重要的是：能否清楚给出自己的假设、设计依据、风险边界，以及可以证伪自己结论的方法。

# **3.11 Chapter 03 结束语与能力总结**

这一章，我们从 OP-TEE 的源码目录出发，逐步深入到了整个 OS 最重要的一组数据结构。

但我希望你真正带走的不是：

```
thread_ctx 位于哪个 .h
tee_ta_ctx 有哪些字段
mobj 有哪些 Callback
```

而是下面三个结论。

**第一，理解 OS 的核心是理解其抽象与资源，而不是背诵源码。**

`thread_ctx` 代表执行上下文。

`tee_ta_session` 代表服务调用关系。

`tee_ta_ctx` 代表 TA 实例状态。

`user_mode_ctx` 代表用户态运行环境。

`vm_region` 代表映射。

`mobj` 代表内存对象。

这些抽象共同构成了 OP-TEE 的运行基础。

**第二，真正可靠的源码分析必须同时研究控制流和资源流。**

Call Graph 告诉你程序如何执行。

Object Lifecycle 告诉你资源如何创建、共享和销毁。

Lock Dependency 告诉你哪些并发执行顺序可能导致问题。

Security Boundary 告诉你哪些输入、权限和资源访问必须接受验证。

少了任何一个维度，都可能遗漏重要的修改风险。

**第三，优秀的 Core Engineer 不只能够回答“代码是怎样实现的”，还要回答“为什么这样实现，以及怎样证明修改不会破坏原有行为”。**

我建议在进入下一章之前，至少把 `ts_ctx → tee_ta_ctx → user_ta_ctx` 的嵌入关系、`thread_ctx → Session Stack` 的执行关系，以及 `vm_region → mobj` 的映射关系彻底理解清楚。

它们将是后续 Chapter 04–30 中大量源码分析的共同基础。

**下一章 Chapter 04 — Build System、Configuration 与 Dependency Management**，我们将进一步研究一个非常现实的问题：

> 代码仓库中存在的实现，究竟如何经过 Makefile、`sub.mk`、`CFG_*`、Linker Script 和平台配置，变成真正运行在某个 SoC 上的 OP-TEE Binary？

这会把本章建立的静态 Source Map 与真实 Binary 的生成和运行联系起来。
