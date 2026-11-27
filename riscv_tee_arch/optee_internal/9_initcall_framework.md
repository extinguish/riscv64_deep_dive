# **Chapter 09 — OP-TEE Initcall Framework & Initialization Dependencies**

OP-TEE OS Internals · PART II: Boot & Initialization · Staff Engineer Level

本章以 [OP-TEE OS 4.11.0](https://github.com/OP-TEE/optee_os/tree/4.11.0?utm_source=chatgpt.com) 为源码基线，重点研究内核初始化框架、初始化顺序、模块依赖、失败处理与系统安全状态。

## **9.1 本章的核心思想**

本章需要回答一个比“如何使用 `service_init()`”更重要的问题：

**一个操作系统如何从一组尚未具备运行能力的独立模块，逐步建立成一个能够安全接受请求的完整系统？**

例如，OP-TEE 启动时需要初始化：

- Heap、Page Allocator、MMU、Thread。
- Crypto Framework、TRNG、Key Manager。
- Device Tree、Interrupt Controller、Clock、Reset、Device Drivers。
- TA Loader、Trusted Storage、Secure Services。
- Notification、RPC 等运行时基础设施。

这些模块并非完全独立。

假设我们准备初始化一个支持 DMA 的 Hardware Crypto Engine，其依赖关系可能是：

**Hardware Crypto Engine Ready**

MMIO Mapping

Clock / Reset

DMA Protection

Interrupt Controller

Crypto Framework

Memory Allocator

这是用于分析依赖关系的示例，而不是 OP-TEE 上游的一张固定依赖图；实际依赖由驱动实现和配置决定。

如果仅仅把 `crypto_engine_init()` 注册成一个 Initcall，并不能证明上述依赖全部满足。

因此，本章最重要的概念是：

\\[ \boxed{\text{Initialization Order} \neq \text{Dependency Satisfaction}} \\]

前者意味着某个回调按照约定先运行；后者意味着该回调真正需要的全部前置条件都已经成立。

两者不能混为一谈。

### **9.1.1 先掌握一个容易忽略的上游实现细节**

我核对了 4.11.0 的 `core/kernel/initcall.c`。

**OP-TEE 当前的通用 Initcall Dispatcher 并不会因为某个回调返回错误码，就自动终止初始化。**

其核心行为是：

```
ret = call->func();

if (ret) {
    EMSG(...);
}
```

随后循环仍然可以继续执行其他 Initcall。

这意味着：

- `TEE_SUCCESS` 不是由 Initcall 框架统一保证的系统就绪状态。
- 发生错误时，框架会记录错误，但不会自动执行 Rollback。
- 强制终止启动需要调用者或具体初始化代码另外实现相应策略。
- 即使最后打印了系统初始化完成的日志，也不能据此证明所有服务成功初始化。

这是理解 OP-TEE 初始化机制的第一个关键安全事实。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



本章将在这个事实基础上，分析 Initcall 到底提供了什么保证，以及哪些保证必须由各个子系统自行建立。

### **本章学习目标**

学完本章，你应该能够：

1. 从源码解释 Initcall 注册、链接、排序和执行的完整实现。
2. 区分 Early Boot、Preinit、Service、Driver、Final 等阶段。
3. 证明一个模块的初始化依赖是否真正满足。
4. 正确设计初始化失败后的 Fail-Stop 或 Degraded Mode 行为。
5. 理解多核、虚拟化与 Initcall 生命周期的关系。
6. 独立审查一个新增 OP-TEE Core Service 的初始化方案。

## **9.2 OP-TEE 的初始化分层**

### **9.2.1 为什么需要 Initcall？**

在一个规模较小的 Bare-metal 系统中，我们可以直接写：

```
void system_init(void)
{
    init_memory();
    init_interrupt();
    init_crypto();
    init_storage();
    init_drivers();
}
```

这种方式简单、明确，但随着系统规模扩大，会遇到三个问题。

**第一，模块之间出现强耦合。**

每增加一个服务，都需要修改全局初始化函数。平台代码必须了解所有子系统的初始化细节。

**第二，可配置功能变得难以管理。**

一个 Driver 可能只在部分 SoC 上编译。一个 Crypto Backend 可能只在启用了特定 `CFG_*` 时存在。直接调用要求不断维护条件编译分支。

**第三，初始化顺序逐渐难以维护。**

例如，Storage Key Manager 需要使用 Crypto，而 Crypto 可能依赖某个硬件 RNG，硬件 RNG 又可能依赖一个设备管理控制器。

如果这些依赖全部写在一个巨大函数中，修改任何模块都可能影响启动顺序。

因此 OP-TEE 使用一种类似 Linux Initcall 的方式：

**各模块分别声明初始化回调，编译链接时收集这些回调，启动时按照预定义的阶段顺序执行。**

这里需要特别注意：OP-TEE 的 Initcall 不等于 Linux 的所有 Initcall 语义；必须以 OP-TEE 自身源码为准。

### **9.2.2 4.11.0 的 Initcall 分类**

在 `core/include/initcall.h` 中，可以找到以下主要注册接口。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



| 分类      | 注册宏                       | Level |
| ------- | ------------------------- | ----- |
| Preinit | `preinit_early()`         | 1     |
| Preinit | `preinit()`               | 2     |
| Preinit | `preinit_late()`          | 3     |
| Early   | `early_init()`            | 1     |
| Early   | `early_init_late()`       | 2     |
| Service | `service_init_crypto()`   | 1     |
| Service | `service_init()`          | 2     |
| Service | `service_init_late()`     | 3     |
| Driver  | `driver_init()`           | 1     |
| Driver  | `driver_init_late()`      | 2     |
| Driver  | `release_init_resource()` | 3     |
| Final   | `boot_final()`            | 8     |

不要误认为：

`early_init_late()` 的 Level 2 与 `service_init()` 的 Level 2 处于同一阶段。

**Level 只在所属的 Scattered Array 内部有意义。**

准确的逻辑是：

```
Preinit Array
  level 1 → 2 → 3
       |
       v
Early Init Array
  level 1 → 2
       |
       v
Service Init Array
  level 1 → 2 → 3
       |
       v
Driver Init Array
  level 1 → 2 → 3
       |
       v
Final Array
  boot_final
```

这是非虚拟化典型执行模型的抽象。某些架构的启动函数还会在这些阶段之间执行额外的平台初始化步骤。

### **9.2.3 每个阶段应该承担什么职责？**

**Preinit：运行时正式初始化之前的准备。**

主要适用于需要在后续 Runtime 初始化之前建立状态的组件。

它不是 Reset 后的第一段初始化代码。Boot Assembly、MMU、Heap、基础 Thread State 等仍可能先于 Preinit 完成。

**Early Init：为后续服务提供基础能力。**

例如平台管理接口、基础资源、早期 Driver Provider、硬件服务前置条件。

原则：不要把一个依赖完整 TEE 服务的复杂模块放在 Early Init。

**Service Init：建立安全内核的主要服务能力。**

包括 Crypto 和 Storage Key Manager 等。这里又专门提供 `service_init_crypto()`，使密码学基础服务能够先于普通 Service 初始化。

**Driver Init：完成剩余驱动探测和硬件资源初始化。**

特别需要注意，这不意味着所有驱动都必须等到 Driver Init 才能初始化。有些 Driver 必须在 Early/Service 阶段先完成，供后续服务使用。

**Final：启动结束前的最终操作。**

例如最终资源整理、启动状态发布等。

Final 与 Driver 的区别不是“硬件与软件”，而是生命周期中的执行时机与可使用的上下文。

## **9.3 Initcall 注册机制：从 C 宏到 Linker**

本节是源码层面的核心内容。

### **9.3.1 第一步：注册一个初始化函数**

假设我们有一个安全服务：

```
#include <initcall.h>
#include <tee_api_types.h>

static TEE_Result audit_service_init(void)
{
    /* Initialize persistent service state */
    return TEE_SUCCESS;
}

service_init_late(audit_service_init);
```

这里的 `service_init_late()` 并不是在调用 `audit_service_init()`。

它是在**编译时注册一条初始化记录**。

这与运行时调用有根本区别。

### **9.3.2 第二步：理解&#x20;**`struct initcall`

源码位置：

`core/include/initcall.h`

其核心结构为：

```
struct initcall {
    TEE_Result (*func)(void);

#if TRACE_LEVEL >= TRACE_DEBUG
    int level;
    const char *func_name;
#endif
};
```

注意两个设计细节。

**第一个：Initcall 本质上是一条函数指针记录。**

它不需要复杂的动态注册机制。

**第二个：调试信息是条件编译的。**

在 `TRACE_LEVEL >= TRACE_DEBUG` 时，记录中还包含 Level 和函数名称。

这意味着：

- Debug 构建可以打印更丰富的初始化信息。
- 低日志等级的构建可以减少元数据。
- 注册表的数据布局依赖编译配置。

它也说明 `struct initcall` 不是跨模块、跨固件版本传递的 ABI，而是内核镜像内部的编译期结构。

### **9.3.3 第三步：宏如何变成特殊 Section？**

在 4.11.0 中，注册会使用：

`SCATTERED_ARRAY_DEFINE_PG_ITEM_ORDERED`

其工作原理可以概括为：

```
service_init_late(audit_service_init)
            |
            v
__define_initcall(service_init, 3, ...)
            |
            v
Create a static const struct initcall
            |
            v
Place the object into a special ELF section
            |
            v
.scattered_array_service_initcall_1_3
```

这里的特殊 Section 名称很重要。

它包含：

- Array 名称：`service_initcall`
- Item 标记：`1`
- 排序级别：`3`

实际注册宏还会利用 `__COUNTER__` 产生内部符号名称，避免同一翻译单元中的记录重名。

需要区分两个机制：

`__COUNTER__` 用于构造符号名称；**它不是全工程 Initcall 执行优先级的来源**。

执行阶段与优先级由注册 Array 和 Section 排序共同决定。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



### **9.3.4 第四步：Linker 如何构造初始化数组？**

OP-TEE 的 `scattered_array.h` 采用一个很巧妙的思路：

各个源文件可以独立贡献数组元素，Linker 最后将它们组合起来。

例如：

```
crypto.c
    → service_initcall level 1

storage.c
    → service_initcall level 3

platform.c
    → service_initcall level 2
```

在编译后的 Object File 中，它们分别处于不同的 Input Section。

Linker Script 使用类似以下指令收集这些 Section：

```
KEEP(*(SORT(.scattered_array*)));
```

这里有两个关键操作。

**SORT：** 按 Section 名称排序，使不同阶段和 Level 的记录按约定排列。

**KEEP：** 防止 Linker 的 Garbage Collection 把这些看起来没有被普通代码直接引用的记录删除。

Arm 4.11.0 的 `kern.ld.S` 中可以看到相应处理；不同 Pager、ASLR 配置下，记录会进入合适的只读或可重定位数据区域。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



最终可以得到概念上类似这样的布局：

```
Start of service_initcall array
    |
    +-- crypto_init_A()       [level 1]
    +-- crypto_init_B()       [level 1]
    |
    +-- service_init_A()      [level 2]
    +-- service_init_B()      [level 2]
    |
    +-- storage_key_init()    [level 3]
    |
End of service_initcall array
```

这里需要记住一个重要限制：

**不同 Level 存在明确的顺序约束，但不应依赖同一 Level 内、来自不同编译单元的回调之间的偶然链接顺序。**

即使你在当前构建中观察到 A 总在 B 之前，也不能把它当成长期稳定的工程契约。

### **9.3.5 第五步：Dispatcher 如何遍历？**

源码位置：

`core/kernel/initcall.c`

将实际实现简化后，可以表示成：

```
static void run_initcalls(
    const struct initcall *begin,
    const struct initcall *end)
{
    const struct initcall *p;

    for (p = begin; p < end; p++) {
        TEE_Result ret = p->func();

        if (ret != TEE_SUCCESS)
            report_init_error(ret);
    }
}
```

上面是教学化的等价伪代码，并非逐字复制的上游函数。

其实际函数为 `do_init_calls()`，内部依次执行 Initcall 记录。不同阶段通过 `call_preinitcalls()`、`call_early_initcalls()`、`call_service_initcalls()`、`call_driver_initcalls()` 和 `call_finalcalls()` 等入口触发。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这个实现非常精简。

它没有：

- 通用的初始化依赖图。
- 通用的拓扑排序算法。
- 初始化成功后的统一状态登记。
- 失败后的自动资源回滚。
- 自动重新执行失败回调的逻辑。

因此，**Initcall Framework 是一个有阶段约束的回调执行器，不是完整的 Dependency Manager。**

这也是为什么我们不能只根据两个 Initcall 的 Level 来证明一个服务安全可靠。

## **9.4 从 Boot Entry 追踪到 Initcall Execution**

理解注册表之后，我们再观察它什么时候被执行。

### **9.4.1 Arm 的典型初始化路径**

在 4.11.0 的 Arm `core/arch/arm/kernel/boot.c` 中，`init_tee_runtime()` 的主要逻辑可以概括为：

```
init_tee_runtime()
    |
    +-- call_preinitcalls()
    |
    +-- call_early_initcalls()
    |
    +-- call_service_initcalls()
    |
    +-- Initialize core-local Pointer Authentication Keys
    |
    +-- Initialize thread Pointer Authentication Keys
    |
    +-- Update stack canaries
```

这是普通非虚拟化路径的概括；虚拟化配置下的 Preinit 处理有所不同。

这里还隐藏着一个实际依赖：

**Pointer Authentication Key 和 Stack Canary 的随机化依赖 Crypto RNG 已经准备完成。**

所以，`init_tee_runtime()` 会在完成 Service Initcall 后再进行相关初始化。

这不是抽象的优先级约束，而是真正的安全依赖。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



Driver Initcall 和 Finalcall 由后续 Boot 阶段调用，而不是全部包含在 `init_tee_runtime()` 中。

因此，理解 Boot 初始化不能只看 `init_tee_runtime()` 这一个函数。

### **9.4.2 RISC-V 的真实初始化路径**

4.11.0 的：

`core/arch/riscv/kernel/boot.c`

可以找到更值得研究的一段路径：

```
boot_init_primary_runtime()
    |
    +-- thread_init_primary()
    |
    +-- boot_primary_init_core_ids()
    |
    +-- boot_primary_init_intc()
    |
    +-- hart_features_init()
    |
    +-- init_tee_runtime()
    |      |
    |      +-- call_preinitcalls()
    |      +-- call_early_initcalls()
    |      +-- call_service_initcalls()
    |      +-- thread_update_canaries()
    |
    +-- boot_mem_release_tmp_alloc()
    |
    +-- sbi_mpxy_init()
    |
    +-- sbi_mpxy_rpmi_probe_channels()
```

然后在：

```
boot_init_primary_final()
    |
    +-- call_driver_initcalls()
    |
    +-- call_finalcalls()
```

这条路径已经直接在 RISC-V 4.11.0 源码中出现。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



这里有一个非常值得思考的设计点：

**RISC-V 的&#x20;**`sbi_mpxy_init()`**&#x20;位于&#x20;**`init_tee_runtime()`**&#x20;之后、Driver Initcall 之前。**

因此，假设你要新增一个依赖 SBI MPXY 的服务：

```
static TEE_Result my_rpmi_service_init(void)
{
    /* Requires SBI MPXY already initialized */
    return TEE_SUCCESS;
}

service_init(my_rpmi_service_init);
```

它在当前路径上就存在明确的初始化时序问题。

因为：

```
service_init()
    → executes inside init_tee_runtime()

sbi_mpxy_init()
    → executes after init_tee_runtime()
```

不能因为 `service_init()` 听起来适合某个“服务”，就忽略了它真正的执行时机。

一个可能的解决方案是把依赖 MPXY 的部分安排到后续阶段；另一个方案是重新梳理平台初始化依赖并调整设计。

具体选择取决于：

- 服务是否属于所有平台都需要的通用能力。
- 是否有其他 Service Initcall 依赖它。
- MPXY 是否已经被保证可以在更早阶段使用。
- Driver Init 之后是否还需要执行其他依赖此服务的模块。

这正是维护 OS Core 时需要做的架构判断。

### **9.4.3 Early Initcall 不等于 Early Boot**

另一个常见误区是：

看到 `early_init()`，就认为此时 MMU、Heap、Thread 一定还没有初始化。

实际不是这样。

在 RISC-V 的 Primary Boot Path 中，早于 `init_tee_runtime()` 的代码已经处理了大量基础工作，例如：

- 初始堆的建立。
- 物理内存和 MMU 相关状态初始化。
- Per-CPU 基础状态。
- Stack Canary 的早期设置。
- 基本 Thread 初始化与 CPU 状态配置。

所以，Early Initcall 的“Early”，是相对于 Initcall 阶段体系而言，而不是指 Reset 后执行的最早代码。

**要判断某个 API 能否在 Early Initcall 中使用，必须检查实际 Boot Call Graph，而不是凭阶段名称猜测。**

## **9.5 初始化依赖管理：OP-TEE 真正的复杂性**

现在进入本章最重要的架构部分。

### **9.5.1 Initcall Level 只能表达部分依赖关系**

假设有三个服务：

```
A: Initialize Secure Clock
B: Initialize TRNG
C: Initialize Storage Key Manager
```

我们知道：

```
A → B → C
```

这里箭头表示：右侧依赖左侧。

最直接的做法是：

```
early_init(init_secure_clock);
service_init_crypto(init_trng);
service_init_late(init_storage_keys);
```

这看起来合理，因为执行阶段满足：

```
Early → Service Crypto → Service Late
```

但我们还没有回答更深层次的问题：

**如果 A 返回了错误，B 为什么还能被调用？**

答案是：在 OP-TEE 通用 Initcall Framework 中，A 返回错误不会阻止 B 执行。

所以，正确的依赖关系实际应该包含两部分：

\\[ A \prec B \\]

表示 A 必须先于 B 执行。

\\[ Ready(A) \Rightarrow B\text{ may proceed} \\]

表示 B 执行时必须检查其前置能力已经建立。

OP-TEE 的 Initcall Level 主要帮助建立第一种约束。

第二种约束需要由模块自身、Driver Framework 或更高层的系统设计保证。

### **9.5.2 Dependency Graph 与 Topological Ordering**

在架构设计阶段，我建议把每个模块视为一个节点。

例如：

Secure Storage 初始化依赖图

Platform Security Hardware

HUK / OTP Access

RNG / Crypto

Storage Key Derivation

Secure Storage Service Ready

逻辑依赖关系示意。RNG 是否直接用于某一阶段的密钥派生，要根据具体算法和 Backend 判断。

为了正确初始化整个系统，必须存在一个满足所有依赖的执行顺序。

如果我们把依赖图定义为：

\\[ G=(V,E) \\]

其中：

- \\(V\\)：初始化任务。
- \\(E\\)：初始化先后依赖。
- \\(A \to B\\)：B 依赖 A。

那么启动顺序必须是这张图的一个 Topological Ordering。

但是，OP-TEE 的通用 Initcall 并不在启动时执行拓扑排序，而是通过预先定义好的阶段来近似表达这些依赖。

这带来两个后果。

**第一，模块开发者必须知道初始化阶段的契约。**

不能仅根据函数名称或其所在源码目录决定注册位置。

**第二，循环依赖不能单纯通过增加 Level 解决。**

例如：

```
Crypto requires RNG
RNG requires Secure IPC
Secure IPC requires Crypto
```

这是一个真正的依赖环。

增加一个更早的 `early_init_very_early()` 也不会解决问题。

必须重新设计架构，例如：

- 使用不依赖完整 Crypto Framework 的最小 Bootstrap Primitive。
- 将 Secure IPC 分为基础传输层和认证传输层。
- 将 Crypto 分成早期 Bootstrap Crypto 与完整 Runtime Crypto。
- 明确哪些能力可以在不进行安全降级的情况下延迟启用。

这也是为什么初始化代码经常暴露底层架构设计中的循环依赖。

## **9.6 真实源码案例：Crypto、HUK、Storage 与 DT Driver**

本节用三组真实源码来理解 Initcall 的设计。

### **9.6.1 案例一：TI K3 的 TISCI 与 TRNG 初始化依赖**

在 OP-TEE 4.11.0 中：

`core/arch/arm/plat-k3/main.c`

可以看到：

```
early_init_late(init_ti_sci);
```

对应源码注释说明，该平台的 TISCI 服务必须先于使用它的 TRNG 服务准备好。

同时，平台的 SA2UL Crypto Driver 会通过 `service_init_crypto()` 注册初始化回调。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



这是非常典型的跨子系统依赖。

```
TISCI Transport
       |
       v
TISCI Service Ready
       |
       v
TRNG / Crypto Initialization
       |
       v
Crypto Service Available
```

为什么不把 `init_ti_sci()` 放到普通 `driver_init()`？

因为 Driver Init 阶段发生在 Service Init 之后。

而 TRNG 初始化需要它提前准备完成。

**这说明真实的 OP-TEE 源码中，硬件驱动与安全服务的初始化顺序并不是简单的“先全部驱动，后全部服务”。**

这是本章非常重要的设计认识。

进一步思考：

如果 TISCI 初始化失败，但是错误只被 Initcall Framework 记录，那么后续 TRNG 初始化可能会继续执行。

此时我们应该检查：

- TRNG 是否会显式检测 TISCI 尚未准备完成。
- 初始化失败是否会传播到 Crypto Framework。
- RNG API 是否可能错误地返回成功。
- Storage、Pointer Authentication、Stack Canary 是否可能依赖错误的随机源。
- 最终系统是否会暴露一项实际上并不安全的服务。

你看，分析范围已经从一个 `early_init_late()` 扩展到了整个 OS 的安全状态。

### **9.6.2 案例二：Storage Key Manager 为什么使用&#x20;**`service_init_late()`**？**

源码：

`core/tee/tee_fs_key_manager.c`

在文件末尾可以看到：

```
service_init_late(tee_fs_init_key_manager);
```

其核心行为可以概括为：

```
static TEE_Result tee_fs_init_key_manager(void)
{
    TEE_Result res;

    res = huk_subkey_derive(
        HUK_SUBKEY_SSK,
        NULL, 0,
        tee_fs_ssk.key,
        sizeof(tee_fs_ssk.key));

    if (res == TEE_SUCCESS)
        tee_fs_ssk.is_init = 1;
    else
        memzero_explicit(&tee_fs_ssk,
                         sizeof(tee_fs_ssk));

    return res;
}
```

这段代码是根据上游关键逻辑整理的教学摘录。实际代码还会输出 Secure Storage 不可用的错误信息。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



为什么要使用 `service_init_late()`？

因为 Storage Key Manager 需要使用 HUK 派生能力以及密码学相关服务。

从初始化关系看：

```
Platform HUK Source
         |
         v
Crypto / Key Derivation Capability
         |
         v
tee_fs_init_key_manager()
         |
         v
Storage Keys Ready
```

最值得研究的不是注册宏，而是这个字段：

```
tee_fs_ssk.is_init
```

当密钥派生失败时，代码清理密钥状态，并没有把 `is_init` 设置为 1。

后续需要使用 SSK 的操作会检查该状态，避免把未初始化的密钥当成有效密钥使用。

这体现了一种重要原则：

**单个服务初始化失败，不一定要求整个 Secure OS Panic，但该服务必须保持不可用，不能以错误状态向外提供能力。**

这叫做受控的功能降级，而不是安全降级。

需要注意的是，是否允许整个系统继续运行，应由产品安全策略决定。对某些依赖 Secure Storage 保存关键状态的产品来说，Storage 不可用可能本身就是必须阻止正常启动的错误。

### **9.6.3 案例三：DT Driver 的 Deferred Probe**

这是本章另一个值得重点研究的上游机制。

源码：

`core/kernel/dt_driver.c`

相关注册关系如下：

```
early_init_late(probe_dt_drivers_early);

driver_init(probe_dt_drivers);

release_init_resource(release_probe_lists);
```

OP-TEE 没有让所有设备在同一阶段简单地探测一次。

而是采用了 Early Probe、Deferred Probe、后续 Probe 和资源整理的组合。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



为什么？

考虑一个硬件设备：

```
Secure Crypto Accelerator
    |
    +-- Requires Clock Provider
    |
    +-- Requires Reset Controller
    |
    +-- Requires Interrupt Controller
    |
    +-- Requires MMIO Mapping
```

假设 Crypto Accelerator 首先被 Probe，但 Clock Provider 尚未注册。

这时不能简单认定设备损坏。

合理的返回值是：

```
TEE_ERROR_DEFER_DRIVER_INIT
```

它表达的语义是：

> 当前还不能完成初始化，因为某些依赖条件暂时没有满足。

OP-TEE 的 DT Driver Framework 可以将该节点保留在待处理队列中，稍后重新探测。

上游 `process_probe_list()` 会在仍有进展的情况下继续尝试；如果最终剩下无法满足的依赖，则会报告未解决问题。它并不是无限制地反复 Probe。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



可以抽象成：

```
Probe Device A
      |
      v
Dependency Ready?
      |
  +---+----+
  |        |
 Yes       No
  |        |
  v        v
Ready    Deferred
           |
           v
    Other Providers Initialize
           |
           v
       Retry Probe
           |
      +----+----+
      |         |
    Ready    No Progress
      |         |
      v         v
    Done    Report Failure
```

上游有两个值得特别区分的行为。

**Early Probe：** 可以暂时接受依赖尚未满足，将待 Probe 的设备保留下来。

**后续 Driver Probe：** 如果依赖最终仍无法满足，或者存在失败的设备节点，`probe_dt_drivers()` 会记录错误并调用 `panic()`。

所以它与前面介绍的通用 Initcall Dispatcher 有不同的失败策略。

这一点非常关键：

**Initcall Framework 负责调用函数；DT Driver Framework 在函数内部承担更具体的依赖管理和错误策略。**

### **9.6.4 为什么 Deferred Probe 不能无限重试？**

假设两个 Driver 存在：

```
Driver A requires Provider B
Driver B requires Provider A
```

如果不检测是否取得实际进展，系统可能永远停留在初始化循环中。

更复杂的是，某个 Driver 可能每次 Probe 都分配部分资源，然后返回 Deferred。如果没有正确回收，反复 Probe 可能导致：

- Secure Heap 泄漏。
- MMIO Mapping 泄漏。
- 多次注册同一个中断。
- 重复启用 Clock。
- Reference Count 错误。
- 硬件进入未知状态。

所以，一个支持 Deferred Probe 的 Driver，其初始化函数必须满足一个重要契约：

**返回 Deferred 时，必须保持可安全重试的状态。**

这不一定要求所有 Driver 初始化函数完全幂等，但要求已经发生的副作用得到适当处理，避免下一次 Probe 在不一致状态上继续。

## **9.7 初始化失败：Panic、Degraded Mode 还是继续启动？**

我们现在已经知道：

- 通用 Initcall 返回错误不会自动停止。
- 某些 DT Driver 初始化失败会直接 Panic。
- 某些服务（例如 Storage Key Manager）可以让自身保持不可用。

那么，应该如何为新服务选择正确策略？

### **9.7.1 不能把所有初始化错误视为同一种问题**

我建议从 Security Architecture 角度将失败分成三类。

| 类别       | 举例                            | 建议策略                   |
| -------- | ----------------------------- | ---------------------- |
| 安全基础不可建立 | 内存隔离、关键访问控制失效                 | Fail-Stop              |
| 关键服务不可用  | 产品强制要求的 RNG、HUK 或 Key Service | 根据安全策略阻止服务或启动          |
| 可选服务不可用  | 可选加速器、可选调试功能                  | Controlled Degradation |

但是，不能仅根据模块名称决定类别。

例如：

同样是 Hardware Crypto Driver 初始化失败：

- 如果具有经过验证的 Software Crypto Backend，可能允许继续运行。
- 如果 Key 只能存储在硬件 Key Slot 中，则不一定允许 Software Fallback。
- 如果密钥策略要求不可导出，Software Fallback 可能直接破坏安全边界。
- 如果该 Crypto Engine 负责 Secure Boot 验证，则失败甚至可能使整个 Boot Chain 无法继续。

因此，更准确的判断方式是：

**初始化失败后，系统能否继续维持已经声明的安全属性？**

如果不能，就应当 Fail Closed。

### **9.7.2 将初始化设计成显式状态机**

对复杂服务，我建议不要只使用一个 `bool initialized`。

可以考虑：

```
enum secure_service_state {
    SERVICE_UNINITIALIZED,
    SERVICE_INITIALIZING,
    SERVICE_READY,
    SERVICE_UNAVAILABLE,
    SERVICE_FAILED,
};
```

状态转换：

```
UNINITIALIZED
      |
      v
 INITIALIZING
      |
  +---+----------------+
  |                    |
Success              Failure
  |                    |
  v             +------+------+
 READY          |             |
                v             v
           UNAVAILABLE      FAILED
```

其中：

- `UNAVAILABLE`：在明确允许的策略下，该能力不提供服务。
- `FAILED`：初始化违反了不可恢复的约束，需要阻止相关系统能力。

真正的设计还需要额外定义：

- 谁可以触发初始化。
- 是否允许重试。
- 谁可以读取状态。
- 状态发布需要什么同步。
- 失败后如何清理已经分配的资源。
- 是否允许从 `FAILED` 恢复到 `READY`。

不能随意允许服务在失败后从任意状态回到 READY，因为这样容易绕过必要的完整性验证和资源重建过程。

### **9.7.3 Partial Initialization 为什么危险？**

考虑这样一个初始化函数：

```
static TEE_Result device_init(void)
{
    TEE_Result res;

    res = alloc_dma_buffer();
    if (res)
        return res;

    res = enable_clock();
    if (res)
        return res;

    res = register_interrupt();
    if (res)
        return res;

    return TEE_SUCCESS;
}
```

如果 `register_interrupt()` 失败，函数直接返回。

但 DMA Buffer 和 Clock 可能仍然处于已分配、已启用状态。

这就产生了 Partial Initialization。

对于普通应用，可能表现为内存泄漏。

对于 Secure Kernel，情况可能更严重：

- DMA Buffer 仍允许设备访问安全内存。
- Interrupt Controller 中遗留了错误配置。
- Clock 保持启用，导致硬件安全状态与软件记录不一致。
- 后续重新初始化可能覆盖资源。
- 另一个 Driver 可能认为该设备已经准备完毕。

因此，初始化设计应该建立资源获取与释放的严格顺序。

例如：

```
static TEE_Result device_init(void)
{
    TEE_Result res;

    res = alloc_dma_buffer();
    if (res)
        return res;

    res = enable_clock();
    if (res)
        goto free_dma;

    res = register_interrupt();
    if (res)
        goto disable_clock;

    return TEE_SUCCESS;

disable_clock:
    disable_device_clock();

free_dma:
    free_dma_buffer();
    return res;
}
```

这只是简化示例。真实 DMA Driver 还必须处理设备访问撤销、IRQ 状态、DMA Completion 与 Cache Synchronization。

**初始化函数的 Cleanup Path，与它的正常初始化路径具有同等重要的安全意义。**

## **9.8 SMP、Virtualization 与 Initcall 生命周期**

这一节是 Staff Engineer 尤其需要掌握的内容。

### **9.8.1 Global Initialization 不等于 Per-CPU Initialization**

在多核 OP-TEE 系统中，需要区分两类状态。

**Global State：**

- 全局 TA Manager。
- 全局 Crypto Service。
- Shared Memory Registry。
- Storage Key Manager。
- 某些全局硬件资源。

**Per-CPU State：**

- CPU-local Thread Context。
- CPU-specific Exception/Interrupt State。
- Per-Hart 或 Per-CPU Hardware Configuration。
- 当前 CPU 的安全执行环境。

在典型 Boot 路径中，通用 Initcall 主要由 Primary CPU 执行。

Secondary CPU 的初始化路径另行处理其 Per-CPU 状态。

例如，4.11.0 的 RISC-V `boot.c` 在 Secondary Path 中执行：

```
thread_set_exceptions()
thread_init_per_cpu()
boot_secondary_init_intc()
sbi_mpxy_init()
```

它没有简单重新执行 Primary CPU 的全部 Service Initcall。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这里必须区分：

`Global Service Ready`

与：

`This CPU is Ready to Use the Service`

这是两个不同条件。

### **9.8.2 一个真实的 SMP 风险场景**

假设我们初始化一个全局 Crypto Engine：

```
service_init_crypto(global_crypto_init);
```

成功后：

```
crypto_ready = true;
```

现在 Secondary Hart 开始执行。

它可能还没有完成：

- Local Interrupt Context。
- Local DMA/Cache Maintenance Configuration。
- Local Exception Handling。
- CPU-specific Crypto Extension Detection。

那么全局 `crypto_ready == true` 是否意味着这个 Hart 可以立即运行所有 Crypto Operation？

不一定。

如果 Crypto Backend 有 Per-Hart 依赖，可能还必须满足：

\\[ Ready(GlobalCrypto) \land Ready(LocalHart) \\]

这就是为什么跨核系统中需要区分 Global Initialization 和 Local Activation。

另外，还需要考虑 Memory Ordering。

即使 Primary CPU 设置了 `crypto_ready = true`，如果其他 Hart 可以并发读取该状态，也必须明确采用何种同步机制来发布初始化完成状态。

普通 C `bool` 不能自动保证跨 Hart 的完整初始化状态可见性。

在有明确 Boot Barrier 的系统中可以依赖经过验证的 Barrier Contract；否则可能需要合适的 Atomic Acquire/Release 或其他同步机制。

### **9.8.3 OP-TEE Virtualization 会改变初始化生命周期**

4.11.0 `initcall.h` 对 `CFG_NS_VIRTUALIZATION` 有专门说明。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



非虚拟化模式中，可以将初始化理解为一次全局 Boot 序列。

但虚拟化场景下，需要区分：

- Nexus：维护部分全局共享的 Core 状态。
- Partition：与特定虚拟化安全运行时实例相关的状态。

为此，OP-TEE 提供：

```
nex_early_init()
nex_service_init()
nex_driver_init()
boot_final()
```

等接口。

当 `CFG_NS_VIRTUALIZATION` 启用时，`nex_*` 宏将相应回调注册到用于 Nexus 启动的 Final Array 中；而在非虚拟化配置下，它们通常映射回对应的普通初始化阶段。

与此不同，Partition 的 Preinit 和 Runtime Init 会在 Partition 创建、首次 Yielding Call 等时机执行。

由此产生了一个非常重要的设计问题：

**某个对象到底属于全局 Nexus，还是属于某个 Partition？**

假设 Secure Storage Key Manager 为某个 Partition 初始化了密钥上下文。

如果我们错误地使用 Nexus Global State 保存它，就可能让不同 Partition 共享本应隔离的状态。

反过来，如果把整个全局 Hardware Crypto Driver 放在每个 Partition 的初始化路径中，又可能发生：

- 重复 Reset 同一个硬件。
- 重复注册相同 IRQ。
- 全局 DMA Buffer 被多个 Partition 重复管理。
- 一个 Partition 的销毁影响另一个 Partition。

所以在虚拟化配置下，Initcall 的选择实际决定了部分对象的生命周期与所有权。

### **9.8.4 如何设计一次性初始化和重复初始化？**

对于一个新服务，要明确至少四种事件：

| 事件                 | 需要考虑什么                |
| ------------------ | --------------------- |
| Cold Boot          | 建立全局状态                |
| Secondary CPU Boot | 建立 Per-CPU 状态         |
| Partition Creation | 建立 Partition-local 状态 |
| CPU/Device Resume  | 恢复硬件或 CPU-local 状态    |

不要用同一个初始化函数盲目处理全部事件。

有时可以复用底层 Helper，但最外层的 Lifecycle Entry 应该独立。

特别是 Suspend/Resume：

它不是一次全新的 Boot。

如果 Resume 路径重新调用只适用于 Cold Boot 的 Initcall，可能导致已有 Session、密钥、引用计数或硬件状态损坏。

## **9.9 实践：新增 Initcall、验证依赖与故障注入**

这一节给出三组可以在本地源码树中完成的实验。

### **实验一：观察不同 Initcall 阶段的实际执行顺序**

**目标：** 证明 Initcall 的阶段排序由编译链接机制决定，而不是 C 文件排列顺序。

在一个仅用于实验的 Core 源文件中增加：

```
#include <initcall.h>
#include <trace.h>
#include <tee_api_types.h>

static TEE_Result demo_early(void)
{
    IMSG("CH09: early");
    return TEE_SUCCESS;
}

static TEE_Result demo_service(void)
{
    IMSG("CH09: service");
    return TEE_SUCCESS;
}

static TEE_Result demo_late(void)
{
    IMSG("CH09: service late");
    return TEE_SUCCESS;
}

early_init(demo_early);
service_init(demo_service);
service_init_late(demo_late);
```

将该源码文件加入测试平台的 Core 构建列表中。

预期的相对执行顺序为：

```
CH09: early
CH09: service
CH09: service late
```

三条日志之间可能穿插大量其他 Initcall 日志，这很正常。

这里验证的是阶段的相对顺序，而不是整个启动过程只运行这三个函数。

**进一步实验：**

将 `demo_early()` 换成 `early_init_late()`，观察执行位置变化。

再定义两个 `service_init()` 回调，放入不同源文件。调整编译链接输入，观察是否能够证明它们之间存在稳定的顺序契约。

结论应该是：同一 Level 内的偶然排序不应作为模块依赖保证。

### **实验二：故意让一个非关键 Initcall 失败**

**目标：** 证明通用 Initcall Framework 不会自动终止启动。

```
static TEE_Result demo_failure(void)
{
    IMSG("CH09: injected failure");

    return TEE_ERROR_GENERIC;
}

service_init(demo_failure);
```

预期现象：

- `demo_failure()` 被调用。
- Dispatcher 记录失败信息。
- 其他 Initcall 仍然可以继续执行。
- 不会仅因为该错误码就由通用 Dispatcher 自动 Panic。

这组实验只能放在专用测试构建里，不能通过修改真实的 Crypto、HUK 或内存保护服务来尝试。

**思考延伸：** 如果该失败回调原本负责初始化某个安全服务，怎样证明后续没有任何代码误认为它已准备完成？

这需要检查消费者，而不是继续分析 Initcall Dispatcher。

### **实验三：使用 ELF 和 Linker Map 验证注册机制**

在 OP-TEE 源码目录执行：

```
git checkout 4.11.0

git grep -n -E \
  'early_init\(|service_init\(|driver_init\(' \
  -- core

git grep -n 'SCATTERED_ARRAY_DEFINE_PG_ITEM_ORDERED'
```

确认你能找到注册宏，以及它最终使用的 Scattered Array。

针对包含实验 Initcall 的目标文件，检查 Input Sections：

```
readelf -SW path/to/demo_init.o |
    grep scattered_array
```

最终 ELF 的输入 Section 可能已经被 Linker 合并，所以还应该查看对应的 Linker Map：

```
grep -n 'scattered_array' path/to/tee.map
```

实际输出文件路径取决于平台和构建配置。

最后，使用 GDB：

```
break do_init_calls
continue
```

进入后观察：

- `begin` 与 `end` 指向何处。
- `call->func` 分别指向哪些初始化函数。
- 失败回调返回后 Dispatcher 如何继续。
- Debug 构建中如何显示 Level 和 Function Name。

**这组实验完成后，你应该能从 C 宏一直追踪到 Linker Section，再从 Section 追踪到 Runtime Dispatcher。**

## **9.10 Change Impact Analysis：修改 Initcall 时应该审查什么？**

以一个常见需求为例：

> 某位工程师准备把一个 Secure Driver 的初始化从 `driver_init()` 改为 `early_init_late()`，以便 Crypto Framework 提前使用它。

乍看只修改一行注册宏，但实际上可能影响整个 Boot。

我建议按照以下矩阵进行审查：

| 影响维度          | 必须回答的问题                                  |
| ------------- | ---------------------------------------- |
| Call Order    | 哪些模块从原来先执行变成后执行？                         |
| Dependency    | 新执行阶段的所有前置条件是否满足？                        |
| Memory        | Heap、MMU、MMIO、Page Allocator 是否已可用？      |
| Thread        | 是否拥有能够执行阻塞操作的上下文？                        |
| Interrupt     | 中断控制器是否准备完成？                             |
| Crypto        | 是否引入 Crypto 与 Driver 的循环依赖？              |
| SMP           | 是否可能与 Secondary CPU 初始化并发？               |
| Security      | 硬件保护状态是否在 Driver 可用前建立？                  |
| Failure       | 初始化失败是否仍能安全继续？                           |
| Compatibility | Pager、Virtualization、Arm、RISC-V 配置是否仍成立？ |
| Validation    | 哪些 Boot、Driver、TA 和负面测试需要重跑？             |

这里特别推荐一种审查方法：

**对每个修改后的初始化回调，列出它所调用的所有非平凡 API，再反向追踪这些 API 的初始化前置条件。**

例如：

```
secure_driver_init()
    |
    +-- malloc()
    |       -> Heap Ready?
    |
    +-- io_pa_or_va()
    |       -> MMU Mapping Ready?
    |
    +-- clk_enable()
    |       -> Clock Provider Ready?
    |
    +-- itr_add()
    |       -> Interrupt Controller Ready?
    |
    +-- crypto_rng_read()
            -> Crypto RNG Ready?
```

如果其中一个依赖尚未建立，这次 Initcall 调整就存在风险。

但还需要向下追踪：

```
secure_driver_init()
    |
    +-- On success:
    |       publish service readiness
    |
    +-- On failure:
            revoke device access
            release allocated resources
            keep service unavailable
```

这样才能同时覆盖正常路径和异常路径。

# **9.11 Staff Engineer 级别面试与架构思考题**

以下 12 道题不会以“某个宏的 Level 是多少”之类的记忆问题为重点。

每道题都可以展开为真实的设计评审或 Debug 会话。我会提供参考思考方向，但它们不是唯一正确答案。

## **题目 1：初始化成功与系统就绪之间的逻辑缺口**

**场景**

一个研发团队发现 OP-TEE 已经打印 `Primary CPU initialized`，但第一次 CA 调用 Secure Storage 时失败。

工程师认为：

“既然整个系统已经初始化成功，那一定是 Storage Runtime 的问题。”

请设计一个方法，判断这句话是否成立。

**考察重点：**

- 如何区分 Initcall 执行完成与功能真正 Ready。
- 如何追踪初始化错误是否被记录后忽略。
- 如何证明后续服务正确检查初始化状态。
- 如何建立完整的 Boot Readiness Contract。

**参考思考方向**

首先不能将启动完成日志当作所有子系统成功初始化的证据。

应该检查 Storage Key Manager 的 Initcall 返回值、`tee_fs_ssk.is_init` 等实际状态、HUK 派生调用链、Crypto Backend 以及 Storage API 的错误传播。

进一步，需要讨论系统是否应该维护全局的 Security Readiness 状态，还是由各个服务独立管理。

优秀的答案还会考虑：如何避免将安全服务内部状态过度暴露给 Normal World，以及如何保证不同构建配置中的就绪判据一致。

## **题目 2：RISC-V SBI MPXY 初始化时序冲突**

**场景**

你在 RISC-V OP-TEE 上新增一个 RPMI-backed Security Service。

这个服务需要通过 SBI MPXY 向管理处理器发送请求。

初始实现为：

```
service_init(my_rpmi_security_service_init);
```

但是在 4.11.0 的 RISC-V Boot Path 中，`sbi_mpxy_init()` 发生在 `init_tee_runtime()` 之后。

你会怎样设计这个功能？

**考察重点：**

- 能否从源码而不是名称推断真实启动顺序。
- 能否分解 Transport、Service、Driver 的职责。
- 是否存在依赖于该服务的其他 Early Service。
- 如何在不破坏跨架构通用设计的情况下调整初始化。

**参考思考方向**

首先建立依赖图，而不是立即把 `sbi_mpxy_init()` 移到更早位置。

可以考虑将服务拆为：

```
Service Metadata Initialization
          |
          v
MPXY Transport Initialization
          |
          v
RPMI Channel Discovery
          |
          v
Security Service Activation
```

如果服务不必在 Service Initcall 阶段就绪，可以安排后续 Activation。

如果确实必须提前，则需要验证 MPXY 初始化所依赖的 Thread、Memory、SBI 能力是否已经存在，并审查移动代码可能影响的全部调用方。

还需要考虑 MPXY 初始化失败后，不能把未经过授权的管理通道当成可信安全服务使用。

## **题目 3：循环依赖导致安全启动无法完成**

**场景**

某 SoC 的安全服务关系如下：

```
Storage Key Manager
       requires Crypto

Crypto
       requires Hardware RNG

Hardware RNG
       requires Secure Mailbox

Secure Mailbox
       requires Authentication Key

Authentication Key
       is stored in Secure Storage
```

这是否可以通过增加一个新的 Initcall Level 解决？

请提出至少两种架构重构方案，并分析安全性。

**参考思考方向**

这是真正的依赖环，无法通过排列已有节点解决。

一种方案是引入 Bootstrapping Trust Anchor，例如通过安全硬件或 Root of Trust 提供最小认证能力，先建立 Mailbox，再建立完整服务。

另一种方案是将 Mailbox 分为不具备安全敏感操作能力的 Bootstrap Transport 和完成认证后的 Secure Transport。

但第二种方案不能仅通过“先跳过认证，稍后再开启”来实现。如果启动阶段已经暴露了需要认证的敏感操作，就可能形成安全漏洞。

需要定义：

- Bootstrap 阶段允许哪些命令。
- 哪些密钥可以使用。
- 哪些资源必须保持不可访问。
- 如何从 Bootstrap 状态原子地切换到 Authenticated 状态。

## **题目 4：同一 Level 的执行顺序突然改变**

**场景**

两个模块都使用：

```
service_init(init_A);
service_init(init_B);
```

多年运行正常。

一次编译器升级和 LTO 配置调整后，`init_B()` 开始先于 `init_A()` 执行，系统偶尔启动失败。

你会如何修复？

**参考思考方向**

不能把修复方案简单归结为调整 Source File 的顺序或者在函数名上增加前缀。

应该明确 A、B 是否存在真实依赖。

如果 B 必须依赖 A：

- 能否安排在不同的 Initcall 阶段？
- 是否应该把两者归为同一个模块，由单一初始化入口按正确顺序执行？
- 是否应显式检查 A 的 Ready 状态？
- 是否需要重构成 Provider/Consumer 或延迟初始化模型？

同时要设计一个回归测试，避免重新引入隐式顺序依赖。

更深入的讨论是：是否值得引入显式依赖元数据来替代部分固定 Level。

## **题目 5：Deferred Probe 形成无法解决的依赖环**

**场景**

两个 DT Driver：

```
Device A requires Clock Provider B
Device B requires Reset Provider A
```

两者都返回 `TEE_ERROR_DEFER_DRIVER_INIT`。

系统最终 Panic。

团队提出的修复方案是：

“把 Deferred Probe 的最大重试次数增加到 1000 次。”

你如何评审这个方案？

**参考思考方向**

这是依赖图中存在 Strongly Connected Component 的问题。

增加重试次数无法解决不可满足的依赖，只会浪费启动时间。

需要分析两个设备之间的依赖是否真的是硬件启动的必要条件。

可能的解决方案包括：

- 分离资源注册和硬件激活。
- 引入最小的 Early Provider。
- 调整设备初始化契约。
- 重新设计硬件 Reset/Clock Ownership。
- 在平台级别提供安全的 Bootstrap 机制。

还必须检查 Deferred Probe 是否具有完整的失败清理路径，防止重复 Probe 导致资源泄漏。

## **题目 6：多核初始化时的竞态**

**场景**

Primary Hart 初始化全局 Crypto Framework：

```
global_crypto_ready = true;
```

Secondary Hart 通过一个普通 `bool` 检测服务是否可用。

在单核 QEMU 上运行正常，在 16-Hart 仿真环境中偶尔失败。

请解释可能原因，并提出一个正确的方案。

**参考思考方向**

至少需要分成三个维度分析。

**Memory Ordering：** 普通 `bool` 不保证所有初始化写入都在跨核读取 Ready 标志前可见。

**Lifecycle：** 全局 Crypto Ready 不意味着当前 Hart 所需的 Local State 已准备完成。

**Synchronization：** 是否已有 Boot Barrier 或其他能够建立 Happens-Before 关系的同步机制？

如果没有，需要通过经过验证的原子变量、Acquire/Release 或更合适的 Boot Synchronization 机制建立状态发布协议。

仅在写入 `bool` 之后加入一个普通编译器 Barrier，不一定足以解决问题。

更深入的答案还应讨论：在初始化阶段其他 Hart 是否真的可能并发进入该服务，以及如何通过实际 Boot Call Graph 证明这一点。

## **题目 7：虚拟化配置下的资源重复初始化**

**场景**

你为 OP-TEE 增加了 Hardware Key Vault Driver。

非虚拟化环境中运行正常。

启用 `CFG_NS_VIRTUALIZATION` 后，第二个 Partition 初始化时发生设备 Reset，导致第一个 Partition 正在运行的密码学操作失败。

如何定位并重新设计？

**参考思考方向**

首先必须识别 Hardware Key Vault 是全局资源还是 Partition-local 资源。

通常应该将物理设备的初始化、全局访问仲裁和安全配置放在合适的全局生命周期中。

每个 Partition 应维护自身的权限、Session、Key Handle 等隔离状态。

需要区分：

```
Global Hardware Initialization
        ≠
Partition-local Logical Context Initialization
```

还应考虑：

- 不同 Partition 的 Key Slot 如何隔离。
- Partition 销毁时如何清理属于它的 Handle。
- 单个 Partition 是否有权触发全局设备 Reset。
- 全局硬件错误如何通知其他 Partition。
- 密钥是否允许跨 Partition 共享。

这实际上是一道涉及安全架构、虚拟化和设备资源所有权的综合题。

## **题目 8：初始化代码与内存生命周期**

**场景**

一个 Driver 在初始化期间构造了一张设备配置表，然后把指向这张表的指针保存在全局 Driver Context 中。

Boot 成功。

只有启用 Pager 和 Init Memory Reclaim 后，Runtime 才偶尔发生异常。

你如何分析？

**参考思考方向**

首先检查配置表的 Storage Duration 和最终所在的 ELF Section。

关键问题：

- 配置表位于 Init-only Data、普通 RO Data 还是 Heap？
- 它所引用的字符串、回调函数、数组是否位于 Init-only Section？
- Init Memory 释放后，全局 Driver Context 是否保留了悬空指针？
- 是否存在只在某种 Linker Layout 下才触发的问题？

这里要深入到：

`Compiler Attribute → ELF Section → Linker Script → Memory Lifetime`

而不能只在 C 语言层面看指针是否合法。

更好的实现应当明确 Runtime Persistent Data 与 Boot Temporary Data 的分界。

## **题目 9：Secure Storage 初始化失败是否应该导致系统 Panic？**

**场景**

某量产设备上，HUK 派生失败导致 Secure Storage 不可用。

产品团队希望：

“即使 Storage 不能使用，设备也必须继续启动 Linux。”

安全团队希望：

“任何 Secure Storage 失败都必须 Panic。”

你作为技术负责人，如何做决定？

**参考思考方向**

不能在缺少 Threat Model 的情况下简单认同任何一方。

必须确定 Secure Storage 保存的对象有哪些：

- 是否有用户身份凭据？
- 是否有设备认证密钥？
- 是否保存 Anti-Rollback 状态？
- 是否参与 Firmware Update Authentication？
- 是否存在安全关键状态无法持久化后的错误降级路径？

如果 Secure Storage 只是某个可选 TA 的数据源，系统可能允许其不可用。

如果某项安全关键操作依赖存储中的持久状态，那么该操作必须 Fail Closed。

更深入的设计可能是：

允许 Linux 启动，但禁止某些安全关键业务，并通过经过设计的健康状态接口提供有限的故障信息。

**关键不在于系统能不能继续执行，而在于它是否仍然满足产品的安全承诺。**

## **题目 10：初始化阶段依赖一个远程管理处理器**

**场景**

OP-TEE 的某个安全模块通过 RPMI 请求独立管理处理器开启一项硬件保护。

Boot 过程中，该请求偶尔耗时数秒，也可能因为管理处理器复位而失败。

你会如何设计初始化策略？

**参考思考方向**

首先需要分析：

- 请求是否允许阻塞。
- 当前 Initcall 阶段是否拥有完整的通信和等待条件。
- 是否可以在当前上下文进行 RPC 或等待中断。
- 请求失败后硬件默认权限是安全还是开放。
- 管理处理器在收到请求前，是否可能已有设备访问安全内存。

如果安全保护是强制性的，可能必须确保硬件从 Reset 起默认拒绝未授权访问，然后通过可信协议授予访问权限。

还要设计：

- 有界 Timeout。
- 请求幂等性。
- Reset Recovery。
- 请求状态验证。
- 不能确认成功时的 Fail-Closed 策略。

特别需要指出：

**依赖远程管理处理器完成安全配置的 Boot，不应只检查“发送命令成功”，而应建立“保护确实生效”的可验证契约。**

## **题目 11：一次 Initcall 修改如何影响跨架构兼容性？**

**场景**

你将一个通用安全服务从 `driver_init()` 移到 `service_init()`。

Arm QEMU 测试全部通过。

RISC-V FPGA 上却出现异常，只有在启用 SMP 和动态 MMU Mapping 时才能复现。

你如何组织调查？

**参考思考方向**

应该先建立两个架构的 Boot Stage Mapping。

对于每个被调用 API，需要明确：

- Arm 在 Service Init 阶段已满足哪些条件。
- RISC-V 在同阶段是否满足相同条件。
- Driver 是否依赖 RISC-V 专属的 SBI、MPXY、TLB Flush 或 Hardware Initialization。
- SMP 是否引入额外的状态可见性或中断路由要求。
- 动态 Mapping 是否依赖尚未初始化的 Page Allocator 或 Backend。

然后通过最小化配置和有针对性的 Trace 找到第一个状态差异。

真正高质量的方案不会直接在 RISC-V Backend 加一个延时循环，而是修复初始化契约不一致的问题。

## **题目 12：如果让你重新设计 OP-TEE Initcall Framework**

**场景**

现在允许你重新设计 Initcall Framework，目标是：

- 保持 Boot 简洁。
- 支持显式 Dependency。
- 支持可选模块。
- 支持严格的失败处理。
- 支持 SMP。
- 支持 Virtualization。
- 尽量不增加 Secure Kernel TCB。

你会提出什么架构？

**参考思考方向**

这是开放式的架构设计题。

一种可能方案是引入静态 Dependency Metadata：

```
struct init_node {
    const char *name;
    const char * const *dependencies;
    TEE_Result (*init)(void);
    uint32_t flags;
};
```

并支持以下逻辑：

```
Collect Init Nodes
        |
        v
Validate Dependencies
        |
        v
Detect Dependency Cycles
        |
        v
Topological Ordering
        |
        v
Execute Init Functions
        |
        v
Publish Readiness / Failure State
```

但真正的 Staff Engineer 不应到这里就结束。

还需要回答：

**复杂度：** 为什么不继续使用简单的固定 Level？

**安全性：** 运行时动态排序是否增加攻击面和不可预测性？

**可验证性：** 是否可以在 Build Time 生成拓扑序列，而不是 Boot Time 动态计算？

**生命周期：** 如何处理 Global、Per-CPU、Per-Partition、Resume 等不同 Scope？

**失败语义：** Fail-Stop、Optional、Retryable 的策略由谁定义？是否允许自动回滚？

**兼容性：** 如何与现有 `preinit_*()`、`service_init_*()`、`nex_*()` 共存？

**维护成本：** 新增的抽象是否比它试图解决的问题还要复杂？

我尤其希望你思考一个问题：

> 对一个以安全性、确定性、可审计性为核心目标的 TEE OS 而言，更通用、更自动化的初始化框架，是否真的比当前简单的静态 Initcall 机制更好？

优秀答案不一定是“应该引入 Dependency Graph”。

也可能是：保留当前简洁模型，同时增加 Build-time Dependency Validation、静态检查和更明确的初始化契约。

判断依据应该是实际维护成本和安全收益，而不是架构看起来有多先进。

## **9.12 本章验收标准与最终总结**

Chapter 09 — Completion Checklist

自检进度

0/8

能够解释 Initcall 从宏、ELF Section 到 Runtime Dispatcher 的完整过程

理解 Preinit、Early、Service、Driver、Final 的阶段关系

知道为什么 Initcall 返回失败不一定终止启动

能分析 Crypto、HUK、Storage 的初始化依赖

能解释 DT Deferred Probe 的用途及限制

能够区分 Global、Per-CPU 与 Per-Partition Initialization

能够为一个新增安全服务编写依赖与异常处理设计

能够给出跨架构、并发、故障注入的回归测试方案

本章最值得保留的五条核心结论：

**第一，Initcall 的本质是编译链接期注册、运行时按阶段遍历的回调机制。** 它不是动态服务管理器，也不是完整的依赖求解器。

**第二，初始化顺序不等于初始化依赖得到满足。** 即使 A 比 B 先执行，B 也必须确认 A 所提供的能力真正可用。

**第三，初始化失败是系统安全架构的一部分。** 不能因为通用 Dispatcher 只记录错误，就认定继续启动是安全的。

**第四，初始化阶段与对象生命周期必须匹配。** Global、Per-CPU、Per-Partition、Runtime、Resume 是不同的执行与资源管理语境。

**第五，修改 Initcall 往往意味着改变系统的依赖契约。** 即使只修改一行注册宏，也可能影响整个 OS 的 Boot、Memory、Crypto、Storage、Driver 和安全边界。

### **本章核心源码索引**

| 文件                                                                                                                          | 阅读目的                       |
| --------------------------------------------------------------------------------------------------------------------------- | -------------------------- |
| [initcall.h](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/include/initcall.h?utm_source=chatgpt.com)                 | 注册宏、阶段、虚拟化接口               |
| [scattered_array.h](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/include/scattered_array.h?utm_source=chatgpt.com)   | 编译链接期注册机制                  |
| [initcall.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/initcall.c?utm_source=chatgpt.com)                  | Dispatcher、错误处理            |
| [Arm boot.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/arm/kernel/boot.c?utm_source=chatgpt.com)             | Arm Runtime 初始化时机          |
| [RISC-V boot.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/riscv/kernel/boot.c?utm_source=chatgpt.com)        | Hart、SBI、Runtime、Driver 顺序 |
| [dt_driver.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/kernel/dt_driver.c?utm_source=chatgpt.com)                | Deferred Probe、失败与资源释放     |
| [tee_fs_key_manager.c](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/tee/tee_fs_key_manager.c?utm_source=chatgpt.com) | Service 依赖和受控降级            |

这次 Chapter 09 最重要的收获，应当是形成这样一种工程直觉：

**当你看到一个新模块的&#x20;**`service_init()`**&#x20;或&#x20;**`driver_init()`**&#x20;时，第一反应不是问它在哪个 Level，而是问：它依赖哪些已经建立的能力？失败时系统仍然安全么？它创建的状态由谁拥有？其他 CPU、Partition 或 Runtime Consumer 什么时候才能使用它？**

能够自然地从这些问题开始分析，就已经进入了 OP-TEE Core Maintainer 所需要的思考层次。
