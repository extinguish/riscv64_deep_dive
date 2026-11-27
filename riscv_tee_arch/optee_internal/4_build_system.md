# **Chapter 04 — OP-TEE Build System, Configuration & Dependency Management**

OP-TEE OS Internals · PART I — Architecture & Engineering Foundations

本章源码基线：**OP-TEE 4.11.0（2026-10-16）**

## **本章的核心目标**

如果把 OP-TEE OS 看成一个运行时系统，那么 Build System 就是决定这个系统最终长成什么样子的机制。

对于普通应用程序，Build System 可能主要决定编译哪些源文件、链接哪些库。

但对于 OP-TEE，它还决定：

- Secure Kernel 支持哪些能力。
- 哪些功能由 Kernel 实现，哪些由 User TA 实现。
- 某个 SoC 平台启用哪些驱动和硬件安全机制。
- Core、`ldelf`、User TA 使用什么 ABI 和工具链。
- 哪些代码参与最终 TCB（Trusted Computing Base）。
- 哪些安全约束在编译阶段就能得到验证。
- 运行时系统与 TA Development Kit、Normal World 之间是否兼容。

因此，这一章真正要研究的是：

> **如何把一组平台需求、安全约束和软件功能选择，转换成一个内部自洽、可重复构建、能够验证的 OP-TEE 系统。**

对于有过移植经验的工程师，重点不是记住 `make` 命令，而是理解配置、依赖关系、代码选择、ABI 和最终镜像之间的因果关系。

## **本章目录**

| 节次   | 主题                             | 重点                       |
| ---- | ------------------------------ | ------------------------ |
| 4.1  | Build System 的本质与设计理念          | Why                      |
| 4.2  | 上游构建系统整体架构                     | Makefile、Core、TA、ldelf   |
| 4.3  | GNU Make 执行机制                  | Include、变量、动态规则          |
| 4.4  | Configuration System           | CFG、优先级、强制约束             |
| 4.5  | Source Selection & Compilation | sub.mk、编译规则              |
| 4.6  | Architecture / Platform        | Arm、RISC-V、Flavor        |
| 4.7  | Core、User TA、ldelf             | 多执行环境与 ABI               |
| 4.8  | Dependency Management          | Crypto、Storage、安全约束      |
| 4.9  | Link、Image 与构建产物               | 从对象文件到镜像                 |
| 4.10 | Incremental Build              | 依赖追踪与缓存正确性               |
| 4.11 | Cross-Repository Integration   | Buildroot、Linux、Firmware |
| 4.12 | 工业级配置管理与安全                     | Release、CI、可重复构建         |
| 4.13 | 综合源码实验                         | 修改与验证                    |
| 4.14 | Staff Engineer 考核              | 10 道高难度综合题               |

# **4.1 Build System 的本质：它是 OS 架构的一部分**

## **4.1.1 为什么 OP-TEE 不直接使用 Linux Kbuild？**

OP-TEE 和 Linux 都需要处理多架构、平台、编译选项和驱动依赖，但它们面对的问题并不完全相同。

Linux 通常是一个完整的通用操作系统，而 OP-TEE 的软件构建至少涉及以下几个相互关联的执行环境：

OP-TEE Build-time Configuration

**Build Inputs**

Platform · Architecture · CFG\_\* · Toolchain · Signing Keys

Secure Core

Kernel / Drivers

Privileged

ldelf

ELF Loader

User Mode

TA SDK

Libraries / Headers

User Mode

**System Integration**

Boot Firmware · Linux TEE Driver · tee-supplicant · TA Images

这些组件可能使用同一个代码库、同一部分基础库，但它们的编译条件并不相同。

例如，`libutils` 可以用于 Core，也可以用于 User TA。然而，这不意味着两个环境的 `libutils` 可以使用完全相同的构建参数。

原因很简单：

**同样一份 C 源码，被编译到不同特权域后，可能拥有不同的 ABI、运行时依赖、权限要求和安全约束。**

它们至少存在以下区别：

| 维度        | Secure Core             | User TA                 |
| --------- | ----------------------- | ----------------------- |
| 执行权限      | Privileged              | Unprivileged            |
| 虚拟内存      | Core Address Space      | TA Address Space        |
| Syscall   | 提供服务                    | 调用服务                    |
| Stack     | Kernel/Thread Stack     | User TA Stack           |
| C Runtime | Kernel-oriented         | User-mode-oriented      |
| 编译参数      | Core Architecture Flags | TA Architecture Flags   |
| Link      | Core Linker Script      | TA Linker Script        |
| 产物        | Core ELF / 镜像           | User TA ELF / Signed TA |

因此，OP-TEE 的 Build System 必须管理一个更复杂的关系：

\\[ \text{Build Output} = f(\text{Source},\text{Configuration},\text{Architecture}, \text{Toolchain},\text{Dependencies}) \\]

但需要注意，这个函数不一定天然是确定性的。构建时间、工具链版本、签名方式、外部文件和生成脚本，都可能影响产物是否能够做到 bit-for-bit reproducible。

## **4.1.2 Build System 实际负责三个不同的工作**

建议把构建过程分成三类决策。

### **第一类：Configuration Resolution**

决定最终哪些功能应该启用。

例如：

```
CFG_WITH_USER_TA ?= y
CFG_REE_FS ?= y
CFG_RPMB_FS ?= n
CFG_NUM_THREADS ?= 2
```

这是配置层。

注意：这里的值仅表示输入或默认配置。**一个变量最终是什么值，还要经过平台约束和依赖检查。**

### **第二类：Build Graph Construction**

根据最终配置，决定哪些文件被编译、使用哪些编译参数、链接哪些库。

例如，以下是说明 `sub.mk` 机制的简化示意：

```
subdirs-y += kernel
subdirs-y += mm

srcs-$(CFG_MY_DRIVER) += my_driver.c
```

当 `CFG_MY_DRIVER=y` 时，`my_driver.c` 进入编译对象集合；否则不会进入。

这不是 C 预处理阶段的选择，而是在生成构建规则时就完成了 Source Selection。

### **第三类：Artifact Construction**

把目标文件和库转换成最终产物。

例如：

```
Source Files
     |
     v
Object Files
     |
     v
Core Libraries + Objects
     |
     v
Core ELF
     |
     v
Binary / Loadable Image
```

在另外一条链上，User TA 的 ELF 还要经过 TA 专用的链接和签名流程。

因此，**Build-time Dependency 和 Runtime Dependency 是两种不同的概念。**

例如，Secure Storage 在运行时需要 Crypto Service，但是 Build System 必须提前保证所需密码算法、代码实现和相关依赖都具备。

如果只依赖运行时检查，就可能在开发板已经启动之后才发现必需的算法没有编入镜像。

## **4.1.3 一个特别重要的设计原则**

对于 OS 和 Firmware，我们应该尽量把错误提前到能够检测到的阶段：

**Configuration Time**

发现不支持的硬件能力和无效功能组合

**Compile / Link Time**

发现类型、接口、符号和代码依赖问题

**Boot / Runtime**

只处理无法提前验证的真实硬件状态和运行时错误

但是，这里存在一个重要区别：

- **编译配置可以证明某个功能被选择。**
- **编译配置不能证明硬件一定具备对应能力。**

例如，设置一个 Crypto Accelerator 的配置选项，并不能证明 SoC 一定包含这个硬件，也不能证明寄存器地址和 DMA 权限配置正确。

这些仍需要平台验证。

# **4.2 OP-TEE Build System 整体架构**

我们开始进入真实源码。

## **4.2.1 必须掌握的源码入口**

以下路径均来自 OP-TEE 4.11.0 的实际源码组织。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google+2



```
optee_os/
|
+-- Makefile
|
+-- mk/
|   +-- config.mk
|   +-- checkconf.mk
|   +-- macros.mk
|   +-- subdir.mk
|   +-- compile.mk
|   +-- lib.mk
|   +-- gcc.mk
|   +-- clang.mk
|   +-- cleanvars.mk
|
+-- core/
|   +-- core.mk
|   +-- crypto.mk
|   +-- sub.mk
|   +-- arch/
|   +-- kernel/
|   +-- mm/
|   +-- tee/
|   +-- drivers/
|
+-- ldelf/
|   +-- ldelf.mk
|
+-- ta/
|   +-- ta.mk
|   +-- mk/
|   |   +-- ta_dev_kit.mk
|   |   +-- build-user-ta.mk
|   +-- ...
|
+-- lib/
|
+-- scripts/
```

建议重点理解下面这些文件的职责，而不是逐个背诵：

| 文件                    | 主要职责                      |
| --------------------- | ------------------------- |
| `Makefile`            | 构建入口，决定包含哪些主要构建单元         |
| `core/core.mk`        | Core 构建、配置集成、库选择、源文件和链接规则 |
| `mk/config.mk`        | 全局配置默认值                   |
| `mk/checkconf.mk`     | 配置约束、依赖处理和配置文件生成          |
| `mk/subdir.mk`        | 递归展开源码目录和文件清单             |
| `mk/compile.mk`       | 生成编译命令、依赖跟踪规则             |
| `mk/lib.mk`           | 静态库构建                     |
| `core/crypto.mk`      | Crypto 相关功能及依赖关系          |
| `ldelf/ldelf.mk`      | `ldelf` 构建                |
| `ta/ta.mk`            | TA 开发库与 SDK 导出            |
| `ta/mk/ta_dev_kit.mk` | 外部 User TA 的构建入口          |

## **4.2.2 从顶层 Makefile 看系统的组织方式**

4.11.0 的顶层 `Makefile` 主要执行以下逻辑。以下是为了突出主流程而保留的源码骨架：

```
include mk/macros.mk
include mk/checkconf.mk

# Load optional configuration
ifdef CFG_OPTEE_CONFIG
include $(CFG_OPTEE_CONFIG)
endif

# Select architecture/platform
ARCH ?= arm
PLATFORM ?= vexpress

# Build OP-TEE Core
include core/core.mk

# Build user-mode components if supported
ifeq ($(CFG_WITH_USER_TA),y)
include ldelf/ldelf.mk

define build-ta-target
ta-target := $(1)
include ta/ta.mk
endef

$(foreach t,$(ta-targets), \
    $(eval $(call build-ta-target,$(t))))

# In-tree user TAs
# ...
endif
```

这里最值得关注的并不是 Makefile 语法，而是三个架构决策。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



**第一：Core 的配置必须先得到解析。**

因为后续 `ldelf` 和 TA 构建过程需要知道哪些用户态能力、架构和接口被启用。

**第二：Core、ldelf 和 TA SDK 不是相互独立的配置系统。**

它们拥有不同构建参数，但都受到 OP-TEE 系统配置的影响。

**第三：User TA 支持是可以在构建阶段关闭的。**

`CFG_WITH_USER_TA` 决定相关用户态组件是否进入顶层构建流程。

这也意味着，不能假设每个 OP-TEE 镜像都包含相同的 User TA Runtime。

## **4.2.3&#x20;**`core/core.mk`**&#x20;才是 Core 构建的关键控制中心**

我们看 4.11.0 中最重要的一段：

```
include mk/cleanvars.mk

sm := core

arch-dir := core/arch/$(ARCH)
platform-dir := $(arch-dir)/plat-$(PLATFORM)

include $(platform-dir)/conf.mk
include mk/config.mk
include core/arch/$(ARCH)/$(ARCH).mk

# ...

include core/crypto.mk
```

这里的 `sm` 表示构建系统的当前子模块标识，并不是 Secure Monitor。

真正值得关注的是 `include` 顺序。

Core Configuration Resolution

1

Platform conf.mk

平台默认配置和硬件约束

2

mk/config.mk

全局默认配置

3

Architecture .mk

工具链、ISA、编译参数

4

core/crypto.mk

Crypto 功能与依赖约束

5

Core Build Rules

生成 conf.h、库和目标文件

`core/core.mk` 还负责设置 Core 专用 CFLAGS、编译静态库、递归处理 `sub.mk`、生成编译规则以及选择最终链接规则。它会优先使用平台提供的 `link.mk`，否则使用相应架构的通用 Core Linker Rules。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



由此可以得到一个重要结论：

**OP-TEE 的 Build System 不是一个简单地遍历目录并编译源文件的工具，而是一个先完成配置解析，再展开源文件依赖和编译规则的系统。**

这个顺序会直接影响我们后续如何新增配置项、驱动和平台特性。

# **4.3 GNU Make 在 OP-TEE 中的真实执行机制**

这一节非常重要。很多工程师会修改 Makefile，但没有真正理解 GNU Make 如何解析整个工程。

## **4.3.1 OP-TEE 主要采用非递归式规则展开**

传统递归式 Make 构建可能采用：

```
all:
	$(MAKE) -C kernel
	$(MAKE) -C drivers
	$(MAKE) -C mm
```

每个目录独立运行 Make，可能导致全局依赖难以统一管理。

OP-TEE Core 的构建方式不同：

```
subdirs = $(core-platform-subdirs) core

include mk/subdir.mk
include mk/compile.mk
```

`mk/subdir.mk` 会递归解析各个目录的 `sub.mk`，但这种递归主要通过 GNU Make 的 `include`、`foreach`、`call` 和 `eval` 实现，而不是为 Core 的每个目录启动一个独立的 Make 进程。

`mk/compile.mk` 随后根据收集到的源文件和编译参数生成对象文件规则。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google+1



可以将整个过程理解为：

```
core/core.mk
     |
     +-- include mk/subdir.mk
     |       |
     |       +-- core/sub.mk
     |       |      |
     |       |      +-- core/kernel/sub.mk
     |       |      +-- core/mm/sub.mk
     |       |      +-- core/tee/sub.mk
     |       |      +-- core/drivers/sub.mk
     |       |
     |       +-- arch/platform sub.mk
     |       |
     |       +-- Collect source files
     |       +-- Collect per-file flags
     |
     +-- include mk/compile.mk
     |       |
     |       +-- Generate compilation rules
     |       +-- Track source dependencies
     |
     +-- Link OP-TEE Core
```

设计上的好处包括统一管理构建依赖、编译命令和工具链参数，同时避免大量独立目录构建造成的状态不一致。

但仍需注意：**非递归 Make 并不自动保证增量构建正确。** 所有真正会影响结果的输入，都必须被准确表达在依赖图中。

## **4.3.2&#x20;**`sub.mk`**&#x20;为什么如此简洁？**

看 4.11.0 中真实的 `core/sub.mk`：

```
subdirs-y += crypto
subdirs-y += drivers
subdirs-y += kernel
subdirs-y += mm
subdirs-y += pta
subdirs-y += tee

subdirs-$(CFG_TEE_CORE_EMBED_INTERNAL_TESTS) += tests
```

最后一行是最典型的 Feature-based Source Selection。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



假设：

```
CFG_TEE_CORE_EMBED_INTERNAL_TESTS=y
```

GNU Make 展开：

```
subdirs-$(CFG_TEE_CORE_EMBED_INTERNAL_TESTS)
```

等价于：

```
subdirs-y
```

因此，`tests` 目录会被加入源码扫描范围。

相反：

```
CFG_TEE_CORE_EMBED_INTERNAL_TESTS=n
```

对应的是 `subdirs-n`，不会被作为启用目录处理。

需要区分：

- `CFG_*`：决定一个功能是否可用。
- `subdirs-y`：哪些源码目录进入构建。
- `srcs-y`：哪些源文件进入构建。
- `cflags-*`：对应目标文件使用什么编译参数。
- `gensrcs-y`：哪些源文件需要先通过生成规则创建。

这构成了配置到编译对象的基本映射。

## **4.3.3 必须理解&#x20;**`$(eval $(call ...))`

OP-TEE 的 Makefile 中大量使用：

```
$(foreach sd,$(sub-subdirs), \
    $(eval $(call process-subdir,$(sd))))
```

为了理解这种机制，先看一个简化例子：

```
define build-one-object
$(1).o: $(1).c
	$$(CC) -c $$< -o $$@
endef

$(eval $(call build-one-object,foo))
```

这里有两层展开：

**第一层：**`call`**&#x20;展开模板。**

它把模板中的 `$(1)` 替换为 `foo`。

**第二层：**`eval`**&#x20;将展开结果作为新的 Makefile 内容解析。**

最终相当于动态创建：

```
foo.o: foo.c
	$(CC) -c $< -o $@
```

为什么模板中需要 `$$`？

因为 GNU Make 会经历多个展开阶段。

`$$` 的作用是让变量引用保留到后续阶段，而不是在第一次模板展开时就被提前求值。

这带来两个典型问题：

1. 过早展开导致使用到错误的局部变量。
2. 过晚展开导致变量已经被下一次目录遍历覆盖。

因此，OP-TEE 的 `mk/subdir.mk` 中会将源文件的局部参数捕获到与目标文件关联的变量中。

例如，源文件级编译选项最后会绑定到相应的 Object，而不应意外应用到同名的其他源文件。

**工程启示：** 如果你修改 `sub.mk` 后发现编译参数影响了不相关文件，除了检查配置条件，还应该检查 Make 变量的作用范围与展开时机。

## **4.3.4 Make 变量的四种常见赋值方式**

| 写法           | 语义         | 典型用途                       |
| ------------ | ---------- | -------------------------- |
| `A = value`  | 递归展开，使用时求值 | 延迟引用                       |
| `A := value` | 立即展开       | 捕获当前状态                     |
| `A ?= value` | 变量未定义时赋值   | 可覆盖的默认配置                   |
| `A += value` | 追加内容       | Source List、Compiler Flags |

例如：

```
A = $(B)
C := $(B)

B = first
```

此时：

```
$(A)   # first
$(C)   # empty
```

因为 `A` 延迟求值，而 `C` 在定义时立即展开。

对于 Build System，这不只是语法差别。

如果一个 Compiler Flag 在错误的时机展开，它可能拿到前一个模块的配置，而不是当前模块的配置。

# **4.4 Configuration System：CFG\_\* 如何最终生效？**

这是本章最值得深入的一个主题。

## **4.4.1 配置优先级**

OP-TEE 4.11.0 的 `mk/config.mk` 在文件开头明确列出了以下优先级：

| 优先级   | 配置来源                        | 典型示例             |
| ----- | --------------------------- | ---------------- |
| 1（最高） | Make 命令行参数                  | `make CFG_FOO=y` |
| 2     | `CFG_OPTEE_CONFIG` 指定文件     | 产品配置文件           |
| 3     | Environment                 | `CFG_FOO=y make` |
| 4     | Platform `conf.mk`          | SoC 默认配置         |
| 5     | `mk/config.mk`              | 全局默认配置           |
| 6     | Subsystem-specific Makefile | 局部默认配置           |

这个顺序描述的是普通默认值和可覆盖赋值的优先关系，而不是说高优先级选项能突破所有硬件约束。`force` 和依赖检查会进一步限定有效配置。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



举个例子。

`mk/config.mk`：

```
CFG_NUM_THREADS ?= 2
```

平台配置：

```
CFG_NUM_THREADS ?= 4
```

产品配置：

```
CFG_NUM_THREADS ?= 8
```

命令：

```
make CFG_NUM_THREADS=12
```

最终得到：

```
CFG_NUM_THREADS=12
```

如果改成：

```
make CFG_NUM_THREADS=12 CFG_OPTEE_CONFIG=product.mk
```

命令行设置依然优先。

### **为什么要存在这些优先级？**

因为 OP-TEE 需要同时满足多个层面的配置需求：

- 上游提供合理默认值。
- SoC Vendor 提供平台默认值。
- Product Team 按产品需求裁剪。
- Developer 可以针对特定实验覆盖配置。
- CI 可以构建不同 Feature Matrix。

但这些需求又不能破坏硬件平台固有的限制。

所以我们还需要 `force`。

## **4.4.2&#x20;**`?=`**&#x20;与&#x20;**`force`**&#x20;的本质区别**

考虑一个四核 SoC：

```
CFG_TEE_CORE_NB_CORE ?= 4
```

这意味着：

> 这个平台默认使用四个 Core，但允许构建者覆盖。

而：

```
$(call force,CFG_TEE_CORE_NB_CORE,4)
```

意味着：

> 对当前平台实现而言，这项配置必须是 4，不接受冲突值。

如果用户执行：

```
make CFG_TEE_CORE_NB_CORE=8
```

`force` 会检测到冲突并终止构建，而不是悄悄将用户输入改成 4。

这是一项非常重要的安全设计。

例如，系统仅配置了四组 Per-CPU Stack，但编译配置却允许八个 CPU 使用相同资源，就可能产生越界或内存覆盖。

使用 `force` 可以把这种问题从潜在运行时故障提前为配置错误。

### **什么时候应该使用哪一种？**

采用如下判断：

- 属于软件策略或默认性能参数：通常用 `?=`。
- 属于硬件或当前实现不能违反的条件：用 `force`。
- 属于多个功能之间的逻辑依赖：使用 Dependency Helpers 或明确的配置检查。
- 属于不受支持的组合：应当拒绝构建，而不是盲目继续。

特别注意，`force`**&#x20;表达的是当前软件实现的约束，不是永恒的硬件真理。** 假如未来内核支持动态 CPU 拓扑，原本必须固定的变量也可能需要重新设计。

## **4.4.3&#x20;**`mk/checkconf.mk`**&#x20;的核心机制**

上游提供了几个重要的配置帮助函数：

```
$(call force,CFG_FOO,y)

$(call cfg-one-enabled,CFG_A CFG_B)

$(call cfg-all-enabled,CFG_A CFG_B)

$(eval $(call cfg-depends-all,CFG_FOO,CFG_A CFG_B))

$(eval $(call cfg-depends-one,CFG_FOO,CFG_A CFG_B))

$(call cfg-check-value,FOO,foo bar)
```

它们不是等价的。

例如：

```
$(eval $(call cfg-depends-all,CFG_FOO,CFG_A CFG_B))
```

其核心语义为：

```
CFG_FOO=y
    |
    +-- CFG_A=y AND CFG_B=y ?
            |
            +-- Yes: keep CFG_FOO
            |
            +-- No: warn and disable CFG_FOO
```

注意最后一点：

`cfg-depends-all`**&#x20;在依赖不满足时通常会给出 Warning，并将功能自动关闭，而不是必然让构建失败。** 这是 4.11.0 源码中的实际行为。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这会带来一个容易被忽略的风险：

```
make CFG_FOO=y
```

构建成功并不等于最终镜像一定包含 `CFG_FOO`。

因为依赖解析可能把它转换成 `n`。

对于关键安全能力，我们不能只检查 Build Exit Code，还需要验证最终配置。

## **4.4.4&#x20;**`CFG_*`**&#x20;如何进入 C 源码？**

我们研究完整路径：

```
make CFG_TEE_CORE_LOG_LEVEL=4
             |
             v
GNU Make Configuration Resolution
             |
             v
mk/checkconf.mk
             |
             v
out/.../include/generated/conf.h
             |
             v
Compiler -include conf.h
             |
             v
#ifdef / #if in C source
```

例如：

```
CFG_WITH_USER_TA=y
CFG_TEE_CORE_LOG_LEVEL=4
CFG_RPMB_FS=n
```

可能生成相应内容：

```
#define CFG_WITH_USER_TA 1
#define CFG_TEE_CORE_LOG_LEVEL 4

/* CFG_RPMB_FS is not set */
```

这是示意性截取，真实文件还包含其他配置以及 Header Guard。

`mk/checkconf.mk` 将 `y` 转为 `1`，将 `n` 和空值视为未定义，其余值生成对应宏。Core 编译时使用 `-include` 将生成的配置头文件引入源文件。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google+1



所以，以下代码：

```
#ifdef CFG_RPMB_FS
initialize_rpmb();
#endif
```

只有对应宏被定义时才参与编译。

### **一个常见陷阱**

下面两种写法不等价：

```
#ifdef CFG_RPMB_FS
```

以及：

```
#if CFG_RPMB_FS
```

在 OP-TEE 的配置语义中，`n` 通常对应未定义宏。

因此，应该遵循项目既有的条件编译风格，特别是在 `-Wundef` 等 Warning 开启时。

更重要的是，不要随意把：

```
#ifdef CFG_FEATURE
```

改成：

```
#if CFG_FEATURE == 1
```

如果 `CFG_FEATURE` 是非布尔值、字符串或者表达式，就可能引入新的编译问题。

## **4.4.5 生成配置文件是调试 Build System 的重要入口**

`core/core.mk` 中定义：

```
conf-file := $(out-dir)/include/generated/conf.h
conf-mk-file := $(out-dir)/conf.mk
conf-cmake-file := $(out-dir)/conf.cmake
```

分别服务于不同消费者。

| 文件                         | 主要用途                     |
| -------------------------- | ------------------------ |
| `include/generated/conf.h` | C/Assembly 预处理           |
| `conf.mk`                  | 记录最终 Make 配置             |
| `conf.cmake`               | 向需要 CMake 配置格式的构建消费者输出配置 |

如果你发现：

> 我明明指定 `CFG_FOO=y`，但是功能好像没有启用。

建议优先执行：

```
grep '^CFG_FOO=' out/rv64-virt/conf.mk
grep 'CFG_FOO' out/rv64-virt/include/generated/conf.h
```

然后再检查实际编译命令。

这比直接阅读业务 C 代码更有效。

# **4.5 Source Selection、Compiler Flags 与编译规则**

## **4.5.1 新增一个 Driver，需要修改多少地方？**

假设我们新增一个 RISC-V 平台 Crypto Engine 驱动：

```
core/drivers/my_crypto/
    my_crypto.c
    sub.mk
```

一个简化的 `sub.mk` 可以写成：

```
srcs-$(CFG_MY_CRYPTO) += my_crypto.c
```

但这还远远不够。

完整设计至少包括：

1. 配置变量由哪里定义。
2. 平台是否支持该硬件。
3. 驱动如何被选入 Source List。
4. 头文件和编译参数如何传播。
5. 编译后是否存在符号和链接依赖。
6. 驱动初始化发生在什么阶段。
7. Runtime Failure 如何处理。
8. 关闭 Feature 时是否还能成功链接。

**真正的 Build Integration 是对全部八个问题给出一致答案。**

## **4.5.2 Compiler Flag 的作用范围**

OP-TEE 提供多层 Flags：

```
core-platform-cflags
core-platform-cppflags

cflags-y
cppflags-y

cflags-my_crypto.c-y
cppflags-my_crypto.c-y
```

它们的适用范围不同。

例如：

```
srcs-y += my_crypto.c

cflags-my_crypto.c-y += -Wno-unused-parameter
```

只应该对 `my_crypto.c` 生效。

相反，在架构文件中设置：

```
core-platform-cflags += -march=...
```

会影响相应 Core 构建范围内的源文件。

对于 `-march`、`-mabi`、栈保护、PIE 等选项，错误的作用范围可能引起非常严重的兼容性或安全问题。

### **为什么不应该全局添加所有 Compiler Flags？**

假设某个 Driver 需要：

```
-march=rv64gc_zicbom
```

直接把它添加到所有 Core 代码的 Flags 中，可能使原本不需要 Zicbom 的代码也允许编译器生成相关指令。

这会影响：

- CPU ISA 兼容性。
- Early Boot 阶段的可执行条件。
- 未来同一个镜像在不同 CPU Revision 上的运行能力。
- 仿真环境与真实 Silicon 之间的一致性。

因此，ISA Feature Selection 必须从整体目标平台的执行契约出发，而不只是为某个驱动消除编译错误。

## **4.5.3&#x20;**`mk/compile.mk`**&#x20;如何生成编译命令？**

在 4.11.0 中，它会为每个源文件生成对象文件规则，并记录编译器、编译参数和依赖文件。

简化理解：

```
source.c
   |
   +-- Compile Command
   |
   +-- Include Dependencies (.d)
   |
   +-- Previous Command (.cmd)
   |
   v
source.o
```

实际编译规则包含：

```
-MD -MF <dependency-file>
-MP
```

并且记录之前使用过的编译命令。

因此，是否需要重新编译，不只是检查 `source.c` 的修改时间，还需要考虑头文件和命令行参数是否变化。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这是 OP-TEE 增量构建正确性的重要基础。

## **4.5.4 为什么同一份源码可能生成多个 Object？**

例如：

```
lib/libutils/
```

可以同时参与：

```
Core
ldelf
User TA
```

但是三个构建目标的 Compiler Flags、链接方式和执行环境可能不同。

所以不能把这三个对象简单合并成同一个通用 Object。

如果盲目共享，就可能出现：

- Kernel Code 意外依赖 User Runtime。
- 不同 ABI 的对象混合链接。
- 某个宏只在 Core 构建时正确。
- 用户态使用了 Privileged-only Implementation。
- 性能优化和安全编译选项不一致。

这是典型的 **Source Reuse 不等于 Binary Reuse**。

# **4.6 Architecture、Platform 与 Flavor 的真正分工**

这一节我们结合 RISC-V 4.11.0 的真实源码来分析。

## **4.6.1 三个层次**

| 层次           | 解决的问题                | 典型配置                  |
| ------------ | -------------------- | --------------------- |
| Architecture | ISA、ABI、CSR/寄存器、编译模型 | `ARCH=riscv`          |
| Platform     | SoC 或平台系列            | `PLATFORM=virt`       |
| Flavor       | 同一平台的具体变体            | `PLATFORM_FLAVOR=...` |

例如：

```
make \
    ARCH=riscv \
    PLATFORM=virt \
    CROSS_COMPILE64=riscv64-linux-gnu- \
    O=out/rv64-virt
```

这是基于 4.11.0 `plat-virt` 和工具链配置的独立构建示例；实际使用前需具备对应交叉编译工具链及其他构建依赖。

注意，Arm 和 RISC-V 的 Core 编译选项并不相同，因此不能机械套用 Arm 的配置方法。

## **4.6.2 RISC-V Architecture 层的职责**

源码入口：

`core/arch/riscv/riscv.mk`

它定义了如下逻辑：

```
ifeq ($(CFG_RV64_core),y)
arch-bits-core := 64
else
arch-bits-core := 32
endif
```

还会选择：

```
CROSS_COMPILE_core
COMPILER_core
```

并根据 ISA Feature 组合形成：

```
-march=$(riscv-isa)
-mabi=$(riscv-abi)
```

4.11.0 的 RISC-V 实现还包含 RV32/RV64 区分、`CFG_RISCV_MMU_MODE` 检查、M/S-mode 配置、SBI 依赖处理和 TA Target 选择。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这说明一个重要事实：

**Architecture Makefile 不只是 Compiler Flags 集合，还承担架构能力验证和执行模式约束。**

## **4.6.3 RISC-V&#x20;**`plat-virt/conf.mk`**&#x20;的真实配置**

以下是上游 4.11.0 `plat-virt` 的一部分：

```
$(call force,CFG_RV64_core,y)

$(call force,CFG_CORE_RESERVED_SHM,n)
$(call force,CFG_CORE_DYN_SHM,y)

CFG_TEE_CORE_NB_CORE ?= 1
CFG_NUM_THREADS ?= 1

CFG_RISCV_PLIC ?= y
CFG_RISCV_APLIC ?= n
CFG_RISCV_IMSIC ?= n

CFG_RISCV_SBI_MPXY ?= y
CFG_RISCV_SBI_MPXY_RPMI ?= y

$(call force,CFG_RISCV_M_MODE,n)
$(call force,CFG_RISCV_S_MODE,y)

supported-ta-targets = ta_rv64
```

这段代码非常适合拿来分析实际产品配置。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



可以看到，配置中混合了三种性质：

**硬件或平台强约束**

```
$(call force,CFG_RV64_core,y)
```

**可修改的默认配置**

```
CFG_NUM_THREADS ?= 1
```

**软件能力选择**

```
CFG_RISCV_SBI_MPXY ?= y
```

这三种配置在设计 Review 时应该分别处理。

例如，`CFG_NUM_THREADS` 不只影响静态数据结构大小，还可能影响可同时处理的 Secure Request 数量和内存占用。

而 `CFG_RISCV_SBI_MPXY` 的改变会涉及 OpenSBI 所提供的能力和运行时通信路径。

## **4.6.4 一个真实的配置依赖案例：RISC-V MPXY**

在 `riscv.mk` 中：

```
ifeq ($(CFG_RISCV_SBI_MPXY),y)
$(call force,CFG_RISCV_SBI,y)
endif

ifeq ($(CFG_RISCV_SBI_MPXY_RPMI),y)
$(call force,CFG_RISCV_SBI_MPXY,y)
endif
```

可以得到：

```
CFG_RISCV_SBI_MPXY_RPMI=y
             |
             v
CFG_RISCV_SBI_MPXY=y
             |
             v
CFG_RISCV_SBI=y
```

这是配置依赖闭包的真实例子。

但它只解决 Build-time Configuration 的一致性。

它并不能证明运行时 OpenSBI 一定支持相应的 MPXY Channel、Protocol ID 或 RPMI Service。

因此，系统至少还需要：

- 编译阶段确保相关代码和接口存在。
- 启动阶段确认所需 SBI/MPXY 服务能够使用。
- 运行阶段处理 Channel/Service Failure。
- 集成测试阶段验证真实 Request/Response。

这非常适合用来理解 **Build-time Capability 与 Runtime Capability 的边界**。

# **4.7 Core、ldelf 与 User TA：三个相关但不同的构建系统**

## **4.7.1 为什么 Core 和 ldelf 不能简单共用一个 Binary？**

`ldelf` 是 OP-TEE 中的 User TA ELF Loader。

它虽然属于受信任的软件栈，但运行模型不同于 Privileged Core。

4.11.0 中的 `ldelf/ldelf.mk` 包含：

```
sm := ldelf

cflags$(sm) := $(core-platform-cflags) \
               -fpie -fvisibility=hidden
```

并根据 Core 的架构宽度选择对应 Loader 架构：

```
ifeq ($(CFG_RV64_core),y)
CFG_RV64_$(sm) := y
endif
```

还采用与 Core 对应的工具链。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



注意这里有两个设计决定。

**第一，Loader 被编译成独立的 ELF。**

这是为了满足其独立的运行时执行模型和地址空间约束。

**第二，Loader 的架构选择与 Core 存在明确关联。**

否则就可能出现 Core 试图运行不匹配架构的 Loader 的情况。

## **4.7.2&#x20;**`ldelf`**&#x20;如何进入 Core Image？**

这是一个值得直接研究的真实例子。

`core/sub.mk` 中有以下规则：

```
gensrcs-y += ldelf

produce-ldelf = ldelf_hex.c

depends-ldelf = scripts/gen_ldelf_hex.py \
                $(out-dir)/ldelf/ldelf.elf
```

生成规则通过 `gen_ldelf_hex.py` 把 `ldelf.elf` 转换为能够被编入 Core 的 C 数据。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



完整路径可以理解为：

```
ldelf/*.c
    |
    v
ldelf/*.o
    |
    v
ldelf.elf
    |
    v
gen_ldelf_hex.py
    |
    v
ldelf_hex.c
    |
    v
Core Object
    |
    v
OP-TEE Core Image
```

因此，虽然 `ldelf` 作为独立 ELF 构建，但它最终也形成了 Core 镜像的一项构建依赖。

这正好解释了一个容易忽略的问题：

> 如果只修改 `ldelf` 的代码，是否只需要更新 `ldelf.elf`？

不一定。

因为实际运行使用的 Loader 可能是已经嵌入 Core 镜像中的版本，必须确保 Core 的生成对象和最终镜像也同步更新。

这里的依赖并非普通静态库链接，而是 **Generated Source Dependency**。

## **4.7.3 TA Development Kit 是构建契约**

`ta/ta.mk` 并不只是编译几个静态库。

它还导出一个供外部 TA 使用的开发环境。

以 RISC-V RV64 为例，产物中会有类似：

```
out/rv64-virt/export-ta_rv64/
    |
    +-- include/
    +-- host_include/
    +-- lib/
    |   +-- libutee.a
    |   +-- libutils.a
    |   +-- libdl.a
    |   +-- ...
    +-- mk/
    |   +-- ta_dev_kit.mk
    |   +-- conf.mk
    +-- src/
    +-- scripts/
    +-- keys/
```

具体库文件取决于构建配置。

`ta/ta.mk` 负责导出 TA 需要的库、头文件、Makefile、部分配置、脚本以及签名相关文件。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



这意味着，TA Dev Kit 实际上是一种版本化的构建接口。

它不仅告诉外部开发者如何编译 TA，还间接限定：

- TA 使用什么 Architecture ABI。
- 哪些用户态库可用。
- 哪些编译能力已经启用。
- 哪些 Header/API 与当前 Core 匹配。
- TA 产物如何签名。
- 哪些功能需要匹配对应的 Core 支持。

## **4.7.4 不同 TA Target 不是同一个概念**

例如：

```
ta_arm32
ta_arm64
ta_rv32
ta_rv64
```

它们对应不同的目标架构。

在 RISC-V 4.11.0 中，`riscv.mk` 通过 `supported-ta-targets` 和 `ta-targets` 组织可构建的 User TA 架构。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



例如：

```
supported-ta-targets = ta_rv64
```

意味着对应平台声明支持 RV64 User TA。

外部 TA 构建应使用：

```
make \
    TA_DEV_KIT_DIR=/path/to/export-ta_rv64 \
    CROSS_COMPILE=riscv64-linux-gnu-
```

前提是目标 TA 的 Makefile 按上游 Dev Kit 方式组织，且交叉工具链与导出配置兼容。

**不能因为 Core 是 RV64，就默认所有 RV32 TA 都受支持。**

这取决于当前架构实现、平台配置和相应的 User TA Runtime 能力。

## **4.7.5 TA 签名是 Build Pipeline 的一部分**

4.11.0 的 `ta/link.mk` 描述了：

```
TA Sources
    |
    v
Object Files
    |
    v
UUID.elf
    |
    v
UUID.stripped.elf
    |
    v
Signing / Optional Encryption
    |
    v
UUID.ta
```

其中签名流程使用：

```
scripts/sign_encrypt.py
```

这里需要严格区分：

- `.elf`：链接后的 ELF。
- `.stripped.elf`：去掉不必要符号信息后的 ELF。
- `.ta`：包含 OP-TEE TA 所需头部及认证信息的可部署产物。

4.11.0 的签名规则还传递 TA UUID、版本、签名密钥和可选的 Subkey 参数。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



### **一个很重要的安全警告**

上游 Dev Kit 可以导出默认开发签名密钥，但这个机制是为了便于开发和测试。

**不能把开发签名私钥直接纳入生产镜像供应链。**

实际产品需要明确：

- 哪个公钥被嵌入 Core。
- 哪个实体拥有 TA Signing Private Key。
- 是否通过 HSM 或离线流程签名。
- 如何管理 Subkeys 和密钥轮换。
- Core 更新是否改变现有 TA 的认证能力。
- 如何阻止降级与未授权 TA 的加载。

所以 TA Build Pipeline 不只是编译流程，它也是软件供应链的安全边界。

# **4.8 Dependency Management：配置依赖如何决定系统安全性？**

前面已经理解了 Makefile 如何选择代码。接下来研究一个更深层次的问题：

**如何保证被选择的代码能够共同组成一个有效、安全的系统？**

## **4.8.1 OP-TEE 的配置依赖不是简单的树形结构**

举例来说：

```
Secure Storage
     |
     +-- Crypto Framework
     |       |
     |       +-- AES
     |       +-- SHA256
     |       +-- HMAC
     |
     +-- RPC
     |
     +-- Key Management
```

而 Crypto Framework 又可能依赖软件 Crypto Library、硬件加速器、CPU ISA Extension 和线程上下文保存机制。

因此，OP-TEE 的功能依赖本质上是一个有向图。

某项配置可能同时被多个功能依赖。

例如：

```
                   CFG_CRYPTO_SHA256
                      ^       ^
                      |       |
                 Secure FS   Pager
```

当你试图关闭 SHA256 时，不能只考虑应用程序是否需要 Hash API，还必须考虑 Pager 和 Secure Storage 是否依赖该能力。

## **4.8.2 从真实源码分析 Crypto Dependency**

在 `core/crypto.mk` 中，有如下逻辑：

```
ifeq ($(CFG_WITH_PAGER),y)
ifneq ($(CFG_CRYPTO_SHA256),y)
$(warning Warning: Enabling CFG_CRYPTO_SHA256 \
  [required by CFG_WITH_PAGER])
CFG_CRYPTO_SHA256:=y
endif
endif
```

这段代码表示：

**如果启用了 Pager，构建系统会确保所需的 SHA256 能力被启用。**&#x20;

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



同一个文件还具有 Secure Storage 的算法依赖处理：

```
$(eval $(call cryp-enable-all-depends, \
    CFG_REE_FS, AES ECB CTR HMAC SHA256 GCM))

$(eval $(call cryp-enable-all-depends, \
    CFG_RPMB_FS, AES ECB CTR HMAC SHA256 GCM))
```

因此，假设你执行：

```
make \
    CFG_REE_FS=y \
    CFG_CRYPTO_SHA256=n
```

不能简单推断最终系统会关闭 SHA256。

构建配置处理可能重新启用相关算法，以满足 Secure Storage 的功能需求。

### **深一层的思考**

为什么有些 Dependencies 会自动启用，而有些 Dependencies 会自动关闭 Feature？

两种策略都有合理性。

**策略 A：自动启用依赖**

适用于上层功能已经被明确启用，而底层能力是必要实现细节的场景。

**策略 B：禁用上层功能**

适用于底层能力无法满足、并且上层功能可以安全关闭的场景。

但还应该存在第三种策略：

**策略 C：Fail Build**

如果某项能力是安全目标的必要条件，则不能容忍系统悄悄降低安全保证。

例如：

```
Product Requirement:
Anti-Rollback MUST be enabled
             |
             v
No secure monotonic storage available
             |
             v
Build must fail
```

如果仅仅打印 Warning 然后关闭 Anti-Rollback，最终发布的产品就可能不满足安全需求。

这是 Staff Engineer 必须考虑的关键区别：

> Build System 的配置依赖策略，应该与产品的安全需求和失败策略保持一致。

## **4.8.3 编译时依赖、链接时依赖和运行时依赖**

三者必须分开分析。

| 依赖类型          | 举例                     | 发现问题的阶段                       |
| ------------- | ---------------------- | ----------------------------- |
| Configuration | Pager 要求 SHA256        | Make 配置解析                     |
| Source        | Driver 要求特定 Header     | Compile                       |
| Symbol        | 调用 `crypto_*()` 实现     | Link                          |
| Binary        | Core 包含正确 `ldelf`      | Image Build                   |
| Runtime       | OpenSBI 提供 SBI Service | Boot / Runtime                |
| Security      | DMA 只允许访问授权缓冲区         | Runtime Security Verification |

例如，一个安全 DMA Driver 可能在编译、链接阶段完全通过，但 SoC 的 Memory Firewall 没有正确配置。

此时 Build System 不会发现硬件隔离问题。

所以，**成功构建是系统正确性的必要步骤，但远远不是充分条件。**

# **4.9 从 Object 到最终 Image：构建产物的完整生命周期**

## **4.9.1 Core Link 的总体过程**

可以简化为：

```
C / Assembly Sources
          |
          v
        *.o
          |
          +----------------+
          |                |
          v                v
   Core Libraries     Generated Objects
          |                |
          +----------------+
                   |
                   v
              Core Link
                   |
                   v
               Core ELF
                   |
                   v
         Image Post-processing
                   |
                   v
         Platform Boot Artifacts
```

实际输出文件取决于 Architecture、Platform 及相应的 Linker Rules。

例如，不同平台可能使用 ELF、Raw Binary 或专有 Loadable Image，不能假设每个 OP-TEE 平台都需要同一组镜像格式。

在 4.11.0 中，`core/core.mk` 通过以下逻辑选择链接规则：

```
include $(if $(wildcard $(platform-dir)/link.mk), \
    $(platform-dir)/link.mk, \
    core/arch/$(ARCH)/kernel/link.mk)
```

这表明平台可以提供专有 Linker Rules，否则使用架构层通用实现。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



## **4.9.2 Linker Script 属于 OS 内存架构的一部分**

为什么 Linker Script 不能被视为普通构建细节？

因为它决定了：

- Code、RO Data、RW Data 的组织。
- Boot-time Code 与 Runtime Code 的布局。
- 特殊 Section 的保留。
- 初始化和清零区间。
- 特定数据与代码的 Address Alignment。
- 可执行代码与不可执行数据的边界。
- 对某些配置而言，Unpaged/Paged Code 的划分。

假设某个函数必须在 Pager 尚未初始化时执行。

如果错误地修改 Linker Script，使它进入需要 Pager 才能访问的区域，就可能出现启动死锁或异常。

因此，源码变更不仅需要检查 C 函数的 Call Graph，有时还需要检查它所在的 ELF Section。

## **4.9.3 如何验证最终 Binary？**

建议熟练使用：

```
readelf -h core.elf
readelf -S core.elf
readelf -l core.elf

nm -n core.elf
objdump -d core.elf
```

其中：

- `readelf -h` 查看 ELF 类型和机器架构。
- `readelf -S` 查看 Section。
- `readelf -l` 查看 Program Header。
- `nm -n` 按地址观察符号。
- `objdump -d` 查看最终机器指令。

一个尤其重要的检查是：

**不要只看源文件是否被编译，还要检查对应代码是否真的存在于最终可加载镜像中。**

因为编译成功与最终链接保留不是同一件事。

例如，Function Section Garbage Collection、链接优化和其他构建规则可能将某些未被引用的实现排除在最终镜像之外。

# **4.10 Incremental Build：为什么只改一个 CFG 就可能导致整个 Core 重编译？**

这是实际工作中经常遇到的问题。

## **4.10.1 GNU Make 默认依靠时间戳，但 OP-TEE 还有命令追踪**

假设：

```
crypto.c
   |
   v
crypto.o
```

通常，如果 `crypto.c` 比 `crypto.o` 更新，Make 会重新编译。

但是，考虑下面的情况：

第一次构建：

```
make CFG_CC_OPT_LEVEL=s
```

第二次：

```
make CFG_CC_OPT_LEVEL=0
```

源文件没有改变，但 Compiler Flags 已经改变。

如果只依靠源文件时间戳，构建系统可能错误地复用旧对象。

OP-TEE 在 `mk/compile.mk` 中还会维护编译命令记录，例如：

```
.crypto.o.cmd
.crypto.o.d
```

路径和文件名随实际目标对象变化。

每次构建时，可以比较：

```
Previous Compile Command
             |
             v
Current Compile Command
             |
             v
Changed?
```

如果命令发生改变，即使源文件没有改变，也应重新编译相关目标。

4.11.0 的 `compile.mk` 确实包含这一机制。

[image](https://www.google.com/s2/favicons?domain=https://chromium.googlesource.com\&sz=32)

Git at Google



## **4.10.2 为什么配置变化可能触发大量重编译？**

因为 Core 的编译命令会强制包含：

```
include/generated/conf.h
```

修改一个 `CFG_*` 可能改变 `conf.h`。

而大量源文件都依赖这个生成头文件。

因此：

```
CFG Change
    |
    v
conf.h Change
    |
    v
Large Object Rebuild
    |
    v
Core Relink
```

这可能是预期行为，而不一定是构建系统低效。

需要注意，OP-TEE 使用类似 `mv-if-changed` 的机制，尽可能避免生成内容未变化时反复更新文件时间戳。

但其配置头文件是集中式的。如果其内容真正变化，通常会产生较广泛的重编译。

### **能不能把配置拆成多个小 Header？**

理论上可以。

例如：

```
conf_crypto.h
conf_storage.h
conf_mm.h
```

这样可以减少某项配置改变后影响的编译范围。

但代价也很明显：

- 必须保证子配置之间的一致性。
- 需要重新设计 Generated Header 的依赖图。
- 必须处理跨模块配置。
- 可能造成隐藏的配置依赖。
- 增加维护成本。

因此，这不是简单的构建加速技巧，而是一项 Build Architecture 的设计权衡。

## **4.10.3 一个工业级问题：Incremental Build 和 Clean Build 结果不同**

假设你遇到：

```
Clean Build: PASS

Incremental Build: PASS

但两者生成的镜像不同，
并且运行行为也不同。
```

不要立即判断是编译器问题。

需要依次检查：

1. Build Input 是否真的相同。
2. 交叉编译器及其版本是否相同。
3. `conf.mk`、`conf.h` 是否相同。
4. Object Compiler Command 是否相同。
5. Generated Source 是否正确重新生成。
6. `.d` 与 `.cmd` 是否存在遗漏。
7. Linker Script 是否正确刷新。
8. TA Signing 或 Image Packaging 是否使用相同输入。
9. 是否存在共享 Output Directory 带来的污染。
10. 是否存在没有声明的外部文件依赖。

**最危险的情况不是构建失败，而是构建成功却复用了错误的产物。**

# **4.11 Cross-Repository Integration：OP-TEE 不可能只依靠&#x20;**`optee_os`**&#x20;完成产品构建**

对于真实 SoC 项目，这一节尤其重要。

## **4.11.1 必须区分三个构建层次**

**System Orchestration**

Manifest / Product Build / CI

**Repository Build**

OpenSBI · UEFI · Linux · optee_os · optee_client · optee_test · Buildroot

**Image Assembly & Validation**

Firmware Packaging · Rootfs · TA Deployment · Boot Tests

`optee_os` 的 Makefile 主要负责该 Repository 自身的构建。

而最终产品还需要处理不同工程之间的依赖和兼容性。

例如：

```
optee_os
    |
    +-- OP-TEE Core Image
    |
    +-- TA Dev Kit
              |
              v
          User TA
              |
              v
         TA Signing
              |
              v
            .ta
              |
              v
          Linux Rootfs
```

如果构建 User TA 时使用的是旧版本 Dev Kit，即使 Core 构建成功，也不能保证 TA 与当前 Core 的所有功能和接口兼容。

## **4.11.2 Buildroot 应该负责什么？**

Buildroot 在这里承担的是系统集成层工作，例如：

- 编译 Linux Rootfs。
- 构建并安装 Normal World 用户态组件。
- 安装 User TA。
- 管理相关 Package 依赖。
- 整合目标文件系统和运行时配置。

但是，它不能代替 `optee_os` 自身的 Architecture Configuration。

例如，Buildroot 不能仅通过安装某个 TA，就保证 Secure Kernel 已经启用对应的 PTA 或 Syscall。

这需要对应的 OP-TEE Core 构建配置。

因此，实际工程中必须维护一份跨工程能力矩阵：

| 功能               | OP-TEE Core     | User TA     | Normal World     | Firmware                |
| ---------------- | --------------- | ----------- | ---------------- | ----------------------- |
| Basic TEE Client | Entry / Session | 对应 TA       | Driver / Client  | World Switch            |
| REE FS Storage   | Storage Backend | Storage API | `tee-supplicant` | 平台密钥能力                  |
| RPMB Storage     | RPMB Backend    | Storage API | RPMB Routing     | RPMB/密钥配置               |
| MPXY/RPMI        | 对应 SBI Client   | 视服务而定       | 视架构而定            | OpenSBI / RPMI Provider |
| Secure DMA       | Driver / Memory | 服务接口        | 视需要而定            | SoC Protection Setup    |

## **4.11.3 为什么应该使用固定版本的集成配置？**

建议记录每个工程的准确 Revision：

```
product-release/
    |
    +-- optee_os:      <commit>
    +-- optee_client:  <commit>
    +-- optee_test:    <commit>
    +-- linux:         <commit>
    +-- opensbi:       <commit>
    +-- edk2:          <commit>
    +-- buildroot:     <commit>
    +-- product-config:<revision>
```

除此之外，还需要记录：

- Toolchain Version。
- `CFG_OPTEE_CONFIG`。
- Effective Configuration。
- Signing Key Identifier。
- Build Scripts Revision。
- Final Image Hash。

否则，一段时间之后很难回答：

> 某个版本为什么可以正常启动，而另一个版本不行？

更严重的是，当你准备修复一个安全漏洞时，可能无法准确判断哪些产品实际包含受影响的代码。

# **4.12 如何设计工业级 OP-TEE Configuration Management？**

这一节开始从实现细节上升到系统设计。

## **4.12.1 Configuration 本质上是产品安全策略的一部分**

建议把配置分成四类：

| 配置类型                 | 示例                    | 处理原则                    |
| -------------------- | --------------------- | ----------------------- |
| Hardware Constraint  | CPU 架构、执行模式           | 强制验证                    |
| Security Requirement | Anti-Rollback、密钥隔离    | 必须满足，失败拒绝               |
| Feature Selection    | Crypto、Storage、Driver | 可配置，但验证依赖               |
| Development Option   | Log、Debug、Profiling   | 与 Production Profile 隔离 |

最值得警惕的是把 Security Requirement 当成可选 Feature。

例如：

```
CFG_MY_SECURE_FEATURE ?= y
```

这种写法可能允许：

```
make CFG_MY_SECURE_FEATURE=n
```

直接构建一个不符合安全要求的产品。

如果该 Feature 属于产品安全认证要求，应在独立的 Product Policy Validation 中检测，不能仅依赖默认值。

## **4.12.2 建立开发、验证和生产配置**

建议至少区分：

```
development
integration
production
```

例如：

| 配置                         | Development | Integration | Production |
| -------------------------- | ----------- | ----------- | ---------- |
| Debug Log                  | 高           | 中           | 低或关闭       |
| Lock Checks                | 开启          | 视测试目的       | 按策略        |
| Debug Symbols              | 保留          | 保留          | 可保留独立符号文件  |
| Test TAs                   | 可以启用        | 按测试集启用      | 通常禁用       |
| Test Signing Keys          | 可以使用        | 专用验证密钥      | 严禁使用       |
| Required Security Controls | 开启          | 开启          | 强制开启       |

但需要特别强调：

**Production 不等于简单地把所有 Debug Option 都设置为&#x20;**`n`**。**

例如：

```
CFG_CORE_STACK_PROTECTOR_STRONG=y
```

属于安全加固能力，而不是普通调试功能。

反过来，某些栈回溯或调试选项可能增加攻击面，不能只因为方便定位问题就无条件开启。

因此，需要逐个判断配置的安全语义。

## **4.12.3 CI 不应该只测试一个 Default Configuration**

假设一个平台支持：

```
CFG_REE_FS = y/n
CFG_RPMB_FS = y/n
CFG_WITH_PAGER = y/n
CFG_WITH_USER_TA = y/n
```

即使只有四个布尔变量，也存在理论上的 16 种组合。

随着变量增长，组合数量会指数级增加。

因此，不可能对所有组合都运行完整的 SoC 仿真。

建议采用分层测试：

**Build Matrix**

验证重要配置组合是否能够编译、链接。

**Runtime Smoke Test**

验证内核可以启动，最基本的 TA 调用可以执行。

**Feature Regression**

针对被启用的子系统运行专门测试。

**Security Negative Test**

验证不满足安全条件的配置会被拒绝，或者能以明确的方式安全失败。

**Cross-Architecture Build Test**

验证通用 Core 修改不会导致 Arm/RISC-V 等其他 Backend 编译失败。

在仿真资源昂贵的环境下，可以先使用低成本 Build/Static Tests 筛除大部分错误，再把有限的仿真资源用于实际硬件行为与集成验证。

# **4.13 综合实战：把本章知识连接到真实源码**

这一节建议实际动手完成，不必修改硬件平台，也不要求立即具备完整 SoC 仿真环境。

## **实验一：建立当前 OP-TEE 的 Configuration Fingerprint**

在 `optee_os` 源码目录：

```
git checkout 4.11.0

make -j$(nproc) \
    ARCH=riscv \
    PLATFORM=virt \
    CROSS_COMPILE64=riscv64-linux-gnu- \
    O=out/rv64-virt \
    V=1
```

这是示例配置，前提是安装了匹配的工具链和构建依赖。

构建完成后检查：

```
cat out/rv64-virt/conf.mk
```

以及：

```
cat out/rv64-virt/include/generated/conf.h
```

然后执行：

```
grep -E \
'^(CFG_RV64_core|CFG_NUM_THREADS|CFG_RISCV_SBI|CFG_RISCV_SBI_MPXY|CFG_WITH_USER_TA)=' \
out/rv64-virt/conf.mk
```

观察最终配置，而不是只观察命令行输入。

**验收目标：** 能解释一个配置从哪个文件进入、在哪个阶段被约束、最终如何传播到 C Source。

## **实验二：分析配置依赖的真实行为**

尝试：

```
make \
    ARCH=riscv \
    PLATFORM=virt \
    O=out/test-config \
    CFG_RV64_core=n
```

由于该平台通过 `force` 要求 RV64，这类冲突应该在配置阶段被拒绝。

然后再选择一个具有软件依赖的 Feature，尝试关闭它依赖的底层选项。

观察：

- Build 是否失败。
- 是否出现 Warning。
- 相关 CFG 最终被强制启用还是禁用。
- 生成的配置是否符合你的原始意图。

**验收目标：** 能判断配置系统究竟是在执行 Defaulting、Constraint Checking，还是 Dependency Resolution。

## **实验三：研究编译命令与对象文件的依赖**

选择一个 Core 源文件：

```
core/kernel/panic.c
```

查找对应生成的：

```
panic.o
.panic.o.cmd
.panic.o.d
```

然后：

1. 查看当前编译命令。
2. 修改一个与此源文件有关的编译选项。
3. 重新执行 Build。
4. 比较新的 `.cmd`。
5. 观察对象是否重新编译。
6. 与 Clean Build 比较结果。

**验收目标：** 能够解释增量构建在什么条件下必须重新执行某个 Recipe。

## **实验四：追踪&#x20;**`ldelf`**&#x20;到 Core 的构建依赖**

研究：

```
ldelf/ldelf.mk
ldelf/link.mk
core/sub.mk
scripts/gen_ldelf_hex.py
```

目标是建立：

```
ldelf source
    |
    v
ldelf.elf
    |
    v
ldelf_hex.c
    |
    v
core object
    |
    v
core image
```

然后回答：

- 为什么 `ldelf` 是独立 ELF？
- 为什么还需要把它嵌入 Core？
- 哪个文件描述 Generated Source 依赖？
- 如果 `ldelf.elf` 改变，哪些目标必须重新生成？
- 为什么只复制新的 `ldelf.elf` 不能保证目标系统使用了新代码？

**验收目标：** 能证明自己真正理解了 OP-TEE 的跨构建单元依赖。

## **实验五：设计一个完整的 Custom Feature**

假设需要添加：

```
CFG_MY_SECURE_COUNTER=y
```

功能要求：

- 只支持 RV64。
- 依赖一个安全 MMIO Counter。
- 提供只读 PTA 接口。
- 不支持 User TA 直接访问设备 MMIO。
- 支持 SMP 并发。
- Production 要求该功能不可被关闭。

请自己设计：

```
Configuration Definition
        |
        v
Architecture Constraint
        |
        v
Platform Constraint
        |
        v
Source Selection
        |
        v
Driver / PTA Build
        |
        v
Initcall
        |
        v
Runtime Access
        |
        v
xtest
```

并给出必要的配置检查与测试方案。

这是本章最重要的综合实验。

# **4.14 Staff Engineer 面试与架构考核**

接下来进入本章的核心考核部分。

这次不以记忆 Makefile 语法为主，而是考察：

- 是否能够识别复杂系统中的真实依赖。
- 是否能够判断架构设计中的隐患。
- 是否能够定义并验证安全不变量。
- 是否能够在多个候选方案之间做出有依据的选择。
- 是否能够把代码修改扩展为完整的工程和安全评估。

建议先独立思考，再查看每道题的分析要点。

## **Question 01 — 一个配置不应该同时代表三种含义**

Architecture Design

某团队引入：

```
CFG_CRYPTO_HW_ACCEL=y
```

它同时表示：

1. 芯片具有 Crypto Engine。
2. OP-TEE 应编译 Hardware Crypto Driver。
3. 运行时所有 Crypto Operation 都应使用硬件加速。

上线后发现，某个芯片 Revision 的 Engine 存在硬件 Erratum，需要部分禁用。

**问题：你是否认为这个配置设计合理？应该如何重新设计？**

参考分析要点

不合理，因为一个布尔变量混合了三种不同层次的语义：

- Hardware Capability：硬件客观能力。
- Build Inclusion：是否编译相应软件实现。
- Runtime Policy：实际如何选择 Backend。

建议分别建立硬件支持声明、驱动构建选项和运行时能力/策略。

对于硬件 Erratum，应结合芯片 Revision、Capability Detection 和 Hardware Driver 的实际能力进行处理。

同时需要考虑：

- 是否支持安全的 Software Fallback。
- Fallback 是否违反性能或安全策略。
- 正在运行的 Operation 如何迁移或失败。
- 不可导出密钥是否允许 Fallback。
- 如何验证修改后的 Crypto API 行为与 Key Management 语义不变。

**核心考点：** 区分 Capability、Implementation 和 Policy，避免用一个 CFG 承载多个互相独立的架构概念。

## **Question 02 — 为什么“Build 成功”可能是坏消息？**

Security Reasoning

某产品安全要求：

```
Production:
Rollback Protection MUST be enabled.
```

工程师设置：

```
CFG_TA_VERSION_DB_RPMB=y
CFG_RPMB_FS=n
```

结果 Build System 打印 Warning，自动将前者设置为 `n`，但最终构建成功。

系统启动、TA 调用和基本功能测试全部通过。

**问题：你会认为这次 Release 合格吗？如何从工程架构上避免此类问题？**

参考分析要点

不能批准 Release。

软件功能能够运行，与是否满足产品安全要求属于两个不同维度。

应引入独立的 Security Policy Validation：

```
Product Security Requirement
            |
            v
   Effective CFG Validation
            |
            v
      Build / CI Gate
            |
            v
    Runtime Verification
```

这里必须验证的是 **Effective Configuration**，而不是用户最初传给 Make 的输入值。

另外还要检查 RPMB 实际可用性、密钥配置、版本数据库的持久化及真实回滚测试。

对不可违反的安全要求，应采用 Fail-closed 策略。

**进一步追问：** 如果产品必须支持无 RPMB 的平台，你会如何重新定义安全等级和产品能力，而不是让所有平台假装拥有相同的 Rollback Security？

## **Question 03 — 增量构建到底有没有资格进入安全发布流程？**

Build Correctness

团队发现：

```
Clean Build:
Image Hash = A

Incremental Build:
Image Hash = B
```

两个镜像都能启动，xtest 也全部通过。

测试负责人认为既然功能正常，可以直接发布增量构建的镜像。

**问题：作为 Staff Engineer，你是否接受？如何设计调查流程？**

参考分析要点

不能仅凭 xtest 通过就接受。

首先需要定义两个构建是否拥有完全相同的输入，包括：

- Source Revision。
- Toolchain 与 Build Tools。
- Effective CFG。
- Generated Sources。
- Signing Inputs。
- Linker / Image Packaging。
- 构建路径和时间等可能进入产物的环境信息。

然后按照以下顺序比较：

```
Input Manifest
      |
      v
Generated Configuration
      |
      v
Object Files
      |
      v
ELF Sections / Symbols
      |
      v
Final Image
```

需要区分三类差异：

1. Debug Metadata 等不影响执行的差异。
2. 预期存在的签名随机性或构建元数据差异。
3. 真正影响执行代码、数据或内存布局的差异。

第三类差异在找到根本原因前不能放行。

**核心考点：** 理解 Reproducible Build 和 Functional Correctness 不能互相替代。

## **Question 04 — 如何为 128 种配置组合设计有价值的测试？**

Engineering Trade-offs

某 SoC 支持七个可选 Feature：

```
Pager
REE FS
RPMB FS
Secure DMA
HW Crypto
SMP
Debug/Release
```

理论上存在 128 种布尔组合。

团队只有一套昂贵的 Palladium 仿真资源，无法运行全部组合。

**问题：如何设计测试矩阵，同时避免安全关键组合被遗漏？**

参考分析要点

不应该平均分配资源，也不能随机抽取少数配置。

推荐分成四层：

**第一层：Constraint Validation。** 枚举重要配置组合，快速验证非法配置被拒绝。可在不启动仿真的条件下完成。

**第二层：Build Matrix。** 对所有支持的核心 Feature 组合进行编译/链接验证，尽量在普通 Build Server 上完成。

**第三层：Runtime Pairwise/Interaction Testing。** 对相互影响较大的 Feature 组合进行实际运行测试，例如 SMP + Secure DMA、HW Crypto + Suspend、Pager + TA Loading。

**第四层：Mandatory Security Profiles。** 对 Production 安全配置执行完整的仿真回归。

重点不是覆盖最多组合，而是覆盖风险最高的交互关系。

还应结合变更范围动态决定本次 Patch 需要扩大的测试集合。

**进一步追问：** 如果一个 Bug 只在 SMP + Pager + OOM 三个条件同时出现，Pairwise Testing 是否足够？应该如何结合故障注入和覆盖率来弥补？

## **Question 05 — 一项 Compiler Flag 为什么可能破坏 Security Model？**

Compiler / ABI / Security

为了提升 RISC-V 性能，工程师在 Architecture Makefile 全局添加：

```
-march=rv64gc_zbb_zicbom
```

本地仿真完全正常，但另一批芯片在启动早期出现 Illegal Instruction。

**问题：你会如何分析这次修改？应该如何设计 ISA Feature 的配置边界？**

参考分析要点

首先检查：

- 目标 CPU/Hart 是否全部支持这些扩展。
- 是否存在不同 CPU Revision。
- Firmware 是否在进入 OP-TEE 前完成必要的环境配置。
- 编译器是否在原本不需要扩展的普通代码中自动生成相应指令。
- Early Boot Code 是否在扩展可用之前执行。
- 编译对象和实际执行对象是否使用同一 ISA Contract。

这里不能简单地因为 Driver 使用 Zicbom，就把整个 Core 的目标 ISA 扩大。

可以考虑：

- 为某些可选实现设置特定编译范围。
- 明确 Minimum Supported ISA。
- 将可选加速代码与通用实现分离。
- 为运行时 Capability Detection 设计安全入口。
- 对不支持相关扩展的目标使用明确的构建配置或替代实现。

需要注意，Zicbom 指令在某些执行模式下还受到特权环境配置影响，不能只检查 ISA 字符串。

**核心考点：** Compiler Flag 不是纯性能开关，而是目标机器执行能力契约。

## **Question 06 — 怎样保证 Core 与 User TA 的 ABI 一致？**

Cross-component ABI

某团队升级了 OP-TEE Core，但外部合作伙伴仍然使用旧版 TA Development Kit。

大部分 TA 都可以运行，少部分包含特定 API 的 TA 在执行中出现异常。

**问题：应该如何判断兼容性问题的来源？从产品工程角度如何避免？**

参考分析要点

首先区分：

- User TA Binary Format。
- Architecture ABI。
- `libutee` 和 Syscall ABI。
- TEE API Contract。
- TA Signing/Version Policy。
- Optional Feature Availability。

不能直接认为“使用旧 Dev Kit 的 TA 一定不兼容”，因为 OP-TEE 可能提供向后兼容能力。

正确方法是明确每项接口的兼容性保证，并设计覆盖对应 API、TA 类型和架构的 Compatibility Tests。

从交付层面，应当：

- 为 SDK 提供版本标识。
- 固定其构建输入。
- 维护 Core ↔ TA SDK Compatibility Matrix。
- 对影响 ABI 的 Patch 强制进行回归验证。
- 为不可兼容的变化提供迁移策略。

**进一步追问：** 如果只是一个 `CFG_*` 改变，为什么也可能影响 User TA 的行为，即使没有任何 Syscall Number 变化？

## **Question 07 — 如何设计一个可以安全关闭的 Kernel Feature？**

Modularity / TCB

团队想把一个大型 Hardware Security Service 加入 OP-TEE。

它依赖 Crypto、Storage、Secure DMA 和多个硬件驱动。

部分低端芯片不包含这些硬件。

**问题：如何设计 Build-time Feature Separation，确保禁用该服务后能够真正减少 TCB，而不仅仅是不执行对应初始化函数？**

参考分析要点

单纯运行时跳过 Initcall 并不足以减少静态代码的 TCB。

应考虑：

- Feature 的代码是否仍被编入 Core。
- 是否存在仍然可达的 PTA / Syscall 入口。
- Driver、Memory Mapping、Interrupt Handler 是否仍然注册。
- 依赖库是否因为其他功能仍需保留。
- 对外暴露的 ABI 是否仍然允许调用。
- 是否存在隐含初始化依赖。

对于完整禁用：

```
CFG_FEATURE=n
     |
     +-- No selected source objects
     +-- No exported service entry
     +-- No unnecessary MMIO mapping
     +-- No feature-specific Initcall
     +-- No feature-only runtime resources
```

还要检查该服务的公共实现是否被其他子系统共享。不能为了减少 TCB 而删除仍被其他模块依赖的功能。

**核心考点：** 区分 Runtime Disable 与 Build-time TCB Reduction。

## **Question 08 — 为什么选择将&#x20;**`ldelf`**&#x20;嵌入 Core？**

System Architecture

假设你正在重新设计 OP-TEE。

有人建议不要再把 `ldelf` 放进 Core Image，而是在需要加载 TA 时，由 Normal World 按需提供 `ldelf`。

这样可以减少 Core Image 大小，Loader 也可以单独升级。

**问题：你认为这个方案是否可行？需要哪些前提才能保持安全性？**

参考分析要点

这不是绝对不可行的方案，但会引入新的安全和生命周期问题。

需要考虑：

**真实性：** Loader Binary 必须被可信地认证，不能直接执行 Normal World 随意提供的 ELF。

**可用性：** 如果早期阶段尚无 Normal World Service，是否仍然需要加载 User TA？

**版本兼容：** Loader 与 Core Syscall ABI 是否兼容？

**安全升级：** 允许单独升级 Loader 时，是否允许 Downgrade？

**启动循环依赖：** 如果验证 Loader 所需的服务依赖 Loader 自身，会不会形成循环？

**TCB 边界：** Loader 即使运行在用户态，仍然负责处理复杂的 ELF 文件，其安全性不能完全依靠签名解决。

**性能与恢复：** 每次加载是否需要验证？异常退出后如何恢复？

这道题没有单一的正确设计，重点是能否完整识别方案改变后的 Trust Boundary。

**核心考点：** 从 Build Optimization 推导 Runtime Security Architecture 的变化。

## **Question 09 — 如何设计面向多个 SoC Revision 的平台配置？**

Product Architecture

一个芯片系列有三个 Revision：

| Revision | Crypto Engine | SMMPT | Interrupt     |
| -------- | ------------- | ----- | ------------- |
| A        | v1            | 不支持   | PLIC          |
| B        | v2            | 支持    | APLIC         |
| C        | v2            | 支持    | APLIC + IMSIC |

项目希望维护一份 OP-TEE Core。

**问题：你应该使用多个 Platform Flavor、多个 Binary，还是统一 Binary + Runtime Detection？为什么？**

参考分析要点

需要首先明确产品对 Binary Compatibility、Boot Chain、维护成本和运行时发现机制的要求。

**方案 A：多个独立镜像。**

优点是配置简单、每个镜像只包含需要的驱动和功能，有利于减少不必要的代码。

缺点是产物数量增加、验证矩阵扩大。

**方案 B：统一镜像 + Runtime Detection。**

优点是便于统一交付，但需要设计可靠的硬件 Revision Discovery、驱动选择、Interrupt Routing 和安全能力检查。

不能只根据非可信 Device Tree 决定安全关键的硬件能力。

**方案 C：混合方案。**

共享同一个 Architecture-Neutral Core，通过少量 Platform Flavor 和运行时探测管理硬件差异。

对于安全隔离能力差异很大的 Revision，采用不同镜像可能更容易形成明确的 Security Contract。

**核心考点：** 根据 TCB、可维护性、安全保证和测试成本选择策略，而不是把单一镜像视为天然更高级。

## **Question 10 — 一个 Build System Patch 为什么可能需要 Security Review？**

Maintainer Judgment

某工程师提交了一个 Patch，只修改：

```
mk/config.mk
core/crypto.mk
```

没有修改任何 C Source。

Patch 的主要目的为：

```
Reduce code size by disabling unused crypto algorithms.
```

提交者认为它只属于构建优化，不需要安全审查。

**问题：作为 OP-TEE Maintainer，你会要求哪些检查？**

参考分析要点

这类修改可能直接影响安全行为。

应至少考虑：

- TA Signing Verification 是否依赖被移除的算法。
- Secure Storage 是否仍然满足原有的机密性和完整性要求。
- Pager 是否具有必要的完整性验证能力。
- Crypto API 是否出现不符合预期的功能回退。
- 硬件 Backend 是否仍然满足算法依赖。
- 旧 TA 是否因算法裁剪而无法运行。
- 是否改变现有 Security Properties。
- 是否在特定平台自动关闭原本启用的功能。
- 构建结果是否在不同 Platform Flavor 上一致。
- 是否添加了能证明配置依赖正确的测试。

我会把这个 Patch 同时视为 Build Configuration Change 和 Security Capability Change。

**核心考点：** 安全影响由行为变化决定，而不是由修改文件的扩展名决定。

## **4.15 附加挑战：两个开放式架构思考题**

下面两道题比普通 Staff 面试题更接近实际 Architecture Review。

### **Challenge A — 为 OP-TEE 设计一个 Configuration Contract 系统**

当前很多 Feature Dependency 是通过 GNU Make 表达的。

假设你有机会对 OP-TEE Build System 进行长期演进，但必须保留现有 GNU Make 兼容性。

你会如何设计一个系统，让每个 Feature 都能够声明：

```
Feature Name
Required Hardware Capabilities
Required Software Dependencies
Conflicting Features
Security Requirements
Affected Components
Expected Runtime Capabilities
Required Tests
```

并要求最终构建自动生成：

```
Build Manifest
Security Capability Report
Test Recommendation Matrix
SBOM / Provenance Metadata
```

你需要思考：

- 是否引入一个声明式配置语言。
- GNU Make 是否仍然作为最终 Rule Generator。
- 如何避免两个配置系统成为不同的事实来源。
- 如何防止非法硬件配置通过检查。
- 如何保持与旧版 Platform `conf.mk` 的兼容。
- 如何控制维护成本。

我更关注的是你如何维护 **Single Source of Truth**，而不是最终采用 YAML、Kconfig、CMake 或其他某种特定技术。

### **Challenge B — AI 自动生成大量 Kernel Patch 后，如何保障构建系统的可信性？**

假设未来 AI Agent 负责 OP-TEE 大部分源码修改。

AI 每次提交 Patch 都可能自动新增：

```
CFG_*
sub.mk
Makefile
Generated Headers
Linker Rules
```

人类 Engineer 不再逐行阅读所有编译脚本。

如何设计一套可信的自动检查机制，使其能够验证：

- 安全能力没有被悄悄关闭。
- 所有 Required Dependencies 得到满足。
- 没有引入未知的编译输入。
- 没有意外修改 TA Signing Key。
- 没有把不安全的代码编入生产镜像。
- 不同 Architecture Backend 仍然正确。
- 生成产物具备可追溯性。
- AI 提供的测试确实覆盖了受影响模块。

这里最值得思考的是：

**如何从相信代码审查者，转向依靠系统化的约束和可验证证据来保证修改正确？**

这比能否记住某个 `make` 函数的语法更有长期价值。

# **4.16 本章总结：应该形成怎样的 Build System 心智模型？**

完成本章后，我希望你不再把 OP-TEE Build System 理解为：

```
Makefile → Compiler → Binary
```

而是建立下面这个模型：

**Product Requirements & Security Policies**

**Configuration Resolution**

CFG · Platform · Dependencies · Constraints

**Build Graph**

Source Selection · Compiler Flags · Generated Files · Libraries

**Artifacts & ABI**

Core · ldelf · TA Dev Kit · Signed TAs

**Firmware & OS Integration**

Boot · Linux · Storage · Driver · Hardware Capabilities

**Validation**

Build Matrix · Runtime · Security · Reproducibility

在这个模型中，Build System 是把产品需求转化为可执行系统的一个重要环节。

但同时也应该认识到：

**Build System 只能保证它能够检查到的约束。真正的安全性仍然需要运行时实现、硬件配置、测试和系统级安全论证共同支撑。**

### **本章必须记住的五条工程原则**

1. **Configuration is Architecture.** 配置选项会改变实际系统架构，而不只是改变编译开关。
2. **Source Reuse ≠ Binary Reuse.** 共享源码不意味着 Core、Loader 和 TA 可以共享相同 ABI 和构建产物。
3. **Build Success ≠ Security Correctness.** 构建成功不意味着所有安全需求得到满足。
4. **Dependencies Must Be Explicit.** 配置、源码、生成文件、工具链和运行时服务都需要明确依赖。
5. **Every Build Must Be Traceable.** 对安全关键软件，每个交付镜像都应该能够追溯到完整的构建输入。

### **推荐深入阅读的 4.11.0 上游源码**

这次建议按以下顺序阅读，而不是随机翻找 Makefile：

- [Makefile — Top-level Build](https://github.com/OP-TEE/optee_os/blob/4.11.0/Makefile?utm_source=chatgpt.com)
- [core/core.mk — Core Build](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/core.mk?utm_source=chatgpt.com)
- [mk/config.mk — Default Configuration](https://github.com/OP-TEE/optee_os/blob/4.11.0/mk/config.mk?utm_source=chatgpt.com)
- [mk/checkconf.mk — Constraint & Configuration](https://github.com/OP-TEE/optee_os/blob/4.11.0/mk/checkconf.mk?utm_source=chatgpt.com)
- [mk/subdir.mk — Source Discovery](https://github.com/OP-TEE/optee_os/blob/4.11.0/mk/subdir.mk?utm_source=chatgpt.com)
- [mk/compile.mk — Compile Rules](https://github.com/OP-TEE/optee_os/blob/4.11.0/mk/compile.mk?utm_source=chatgpt.com)
- [core/crypto.mk — Cryptographic Dependencies](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/crypto.mk?utm_source=chatgpt.com)
- [riscv.mk — RISC-V Architecture Configuration](https://github.com/OP-TEE/optee_os/blob/4.11.0/core/arch/riscv/riscv.mk?utm_source=chatgpt.com)
- [ta/ta.mk — TA SDK Export](https://github.com/OP-TEE/optee_os/blob/4.11.0/ta/ta.mk?utm_source=chatgpt.com)
- [ldelf/ldelf.mk — ELF Loader Build](https://github.com/OP-TEE/optee_os/blob/4.11.0/ldelf/ldelf.mk?utm_source=chatgpt.com)

## **Chapter 04 的最终验收标准**

如果能够独立解决以下场景，就可以认为本章达到了接近 Staff Engineer 的掌握要求：

> 某个 OP-TEE Patch 引入一个新的 `CFG_*`，修改了 Architecture Makefile、Driver `sub.mk` 和 TA SDK Export。你需要在不依赖原作者解释的情况下，识别其全部编译时依赖、最终配置、ABI 变化、潜在安全风险和跨架构影响，并设计一组足以验证修改正确性的测试。

**对于本章，我尤其建议把第 02、03、05、08、10 题作为重点。** 它们分别考察安全配置、构建正确性、编译器与 ISA 契约、Loader 架构设计以及 Maintainer 判断力。

下一章 Chapter 05 会进一步深入 ELF、Linker Script、Section Layout、Relocation 和 OP-TEE Image。届时我们会把本章的 Build Graph 继续向下推进，一直到真实的机器指令、内存布局和启动镜像。
