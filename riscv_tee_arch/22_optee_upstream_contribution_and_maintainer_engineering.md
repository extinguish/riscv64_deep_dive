# Chapter 22 — OP-TEE Upstream Contribution & Maintainer Engineering

RISC-V OP-TEE Architecture & Porting · 最终章 · Staff Engineer / Maintainer 深度教程

这一章我们要完成一次重要的角色转变：

从能够独立开发和移植 OP-TEE 的工程师，成长为能够设计上游可接受的改动、审查其他开发者的代码，并长期承担某一子系统维护责任的工程师。

此前的 Chapter 01–21 主要解决 How does OP-TEE work?，而本章重点解决：

How do we evolve OP-TEE safely, collaboratively, and sustainably?

这不仅是 Git、GitHub 或 Coding Style 的问题。对于 OP-TEE 这样的安全敏感型系统软件，优秀的 upstream engineering 需要同时具备：

- Architecture Ownership：理解内核及其安全边界，知道什么改动应该放在哪里。
- Engineering Discipline：将复杂改动拆成可审查、可验证、可维护的 Patch Series。
- Security Reasoning：证明修改没有破坏机密性、完整性、隔离性及系统兼容性。
- Community Collaboration：有效开展设计讨论、Code Review、RFC 与长期维护。

本章将完整覆盖原大纲的 22.1–22.10，并以 RISC-V OP-TEE、OpenSBI Domain、PMP/MMU、SMP 和 TEE 通信作为主要案例。

## 本章目录

| Section | 内容                                           |
| ------- | -------------------------------------------- |
| 22.1    | OP-TEE 与 OpenSBI 的代码治理及维护边界                  |
| 22.2    | 如何选择适合 upstream 的 Issue                      |
| 22.3    | Coding Style、Commit Message、DCO 与 Patch 拆分   |
| 22.4    | 如何设计测试以证明 Patch 的正确性                         |
| 22.5    | Bugfix、Feature、Refactoring 的不同 Review 标准     |
| 22.6    | ABI 与 Security Compatibility 风险识别            |
| 22.7    | 如何阅读并参与 RFC                                  |
| 22.8    | Cross-Architecture Review：Arm 与 RISC-V 的代码复用 |
| 22.9    | 持续维护一个子系统的职责                                 |
| 22.10   | 从 Contributor 到 Reviewer / Maintainer        |
| 实战      | 一个 RISC-V MMU/TLB Patch 从发现问题到合并的完整过程        |
| 考核      | Staff Engineer 级工业场景面试题及参考答案                 |

# 22.1 OP-TEE 与 OpenSBI 的代码治理及维护边界

## 22.1.1 为什么需要先理解 Governance？

假设你在进行 RISC-V OP-TEE 移植时发现：

> Linux Domain 切换到 OP-TEE Domain 之后，某个 hart 的 `satp` 没有正确恢复，导致 OP-TEE 在访问 User TA 内存时产生 Page Fault。

你可能想到三个修改方案：

1. 在 OP-TEE 的 RISC-V `entry.S` 中增加 `satp` 恢复。
2. 在 OpenSBI 的 Domain Context Switch 实现中增加 CSR 保存/恢复。
3. 在每次进入 OP-TEE 前强制重新初始化页表。

但问题是：这三个修改位置并不等价。

如果根本原因是 OpenSBI 没有正确维护 Domain Context，那么在 OP-TEE 中增加 workaround 可能掩盖问题，而且其他 Domain 也可能受到影响。

反过来，如果 OpenSBI 已正确恢复架构状态，而 OP-TEE 在恢复自己的 User TA Context 时配置了错误的 `satp`，修改 OpenSBI 就违反了职责边界。

因此，参与 upstream 之前首先要回答：

哪个组件拥有这个状态？哪个组件负责维护这个不变量？哪个仓库应该承载修复？

这就是 Architecture Ownership。

## 22.1.2 RISC-V OP-TEE 的跨仓库职责划分

完整的 RISC-V TEE 系统通常涉及多个独立上游项目。

| 项目            | 典型职责                                        | 代码仓库                                                                            |
| ------------- | ------------------------------------------- | ------------------------------------------------------------------------------- |
| OpenSBI       | M-mode Firmware、SBI、Domain、Hart、硬件隔离        | [OpenSBI](https://github.com/riscv-software-src/opensbi?utm_source=chatgpt.com) |
| OP-TEE OS     | Trusted Thread、TA、Core MMU、Secure Services  | [optee_os](https://github.com/OP-TEE/optee_os?utm_source=chatgpt.com)           |
| Linux         | TEE Driver、Shared Memory、SBI/RPMI Transport | [Linux](https://github.com/torvalds/linux?utm_source=chatgpt.com)               |
| OP-TEE Client | `libteec`、`tee-supplicant`                  | [optee_client](https://github.com/OP-TEE/optee_client?utm_source=chatgpt.com)   |
| OP-TEE Test   | xtest、Regression Tests                      | [optee_test](https://github.com/OP-TEE/optee_test?utm_source=chatgpt.com)       |
| OP-TEE Docs   | 开发文档、架构说明、平台文档                              | [optee_docs](https://github.com/OP-TEE/optee_docs?utm_source=chatgpt.com)       |

这里必须形成一个基本原则：

> Ownership follows architectural responsibility, not the repository in which the bug was first observed.

例如，在 OP-TEE 中观察到 Page Fault，不代表一定应该在 OP-TEE 中修复。

### 一个典型的架构边界案例

```
Linux CA
    |
    | TEEC_InvokeCommand()
    v
Linux TEE Driver
    |
    | SBI MPXY / RPMI
    v
OpenSBI M-mode
    |
    | Domain Context Switch
    | Restore CSR / Protection
    v
OP-TEE S-mode
    |
    | Trusted Thread
    | TA MMU Context
    v
User TA (U-mode)
```

当错误发生时，可以按照下面的层级检查：

| 故障位置                     | 首先应检查                                    |
| ------------------------ | ---------------------------------------- |
| Linux 无法发起 TEE 请求        | Linux TEE/RPMI Driver                    |
| SBI 调用返回错误               | Linux SBI 层、OpenSBI MPXY/RPMI            |
| Domain 切换后 CSR 异常        | OpenSBI Domain Context                   |
| OP-TEE Core 页表配置错误       | OP-TEE RISC-V MMU                        |
| TA 访问自身内存失败              | OP-TEE TA Loader / User MMU              |
| RPC 后无法恢复 Trusted Thread | OP-TEE Thread 与 Transport 双方             |
| DMA 可以绕过安全内存隔离           | SoC Hardware Isolation / Firmware Policy |

这张表并不意味着问题只能属于某一层；它是一套定位起点。

复杂问题有时确实需要多个仓库协同修改。

## 22.1.3 OP-TEE 的 Maintainer 组织结构

在 [OP-TEE MAINTAINERS](https://github.com/OP-TEE/optee_os/blob/master/MAINTAINERS?utm_source=chatgpt.com) 中，项目按照平台和子系统划分维护范围，并使用类似 Linux 的维护字段：

- `M:` — Maintainer
- `R:` — Reviewer
- `S:` — Maintenance Status
- `F:` — 负责的文件或目录范围

OP-TEE 还设有 `THE CORE REVIEWERS` 与 `THE REST` 两类全局范围的维护/审查条目。

当前 RISC-V 条目将 `core/arch/riscv/`、`lib/libutee/arch/riscv/`、`ta/arch/riscv/` 等目录列入维护范围。Core Team 则负责合并、发布及安全事件响应等项目级工作。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



这里需要特别区分三种角色：

Contributor：提出问题、编写代码、提交补丁。

Reviewer：评估代码是否正确、安全、可维护，并能够指出具体问题。

Maintainer：持续承担某一范围的技术方向、回归修复、代码审查及维护责任；最终合并权限还取决于项目治理。

一个人的 Code Review 即使非常有价值，也不代表他已经具有 GitHub Write Access。

对于长期希望成为 OP-TEE Committer 的工程师，这一点非常重要。

## 22.1.4 OP-TEE 与 OpenSBI 的提交方式不同

这是一个容易踩坑的地方。

OP-TEE OS

以 GitHub Fork + Pull Request 为主要贡献方式。

代码审查、修改建议及最终合并主要通过 GitHub 进行。

OpenSBI

当前官方贡献文档要求向 `opensbi@lists.infradead.org` 提交邮件 Patch Series，由 Maintainer 接收并整合。

因此不要假设将一个 OpenSBI GitHub PR 提交出去就完成了正式投稿流程。

Linux Kernel

按内核邮件列表流程提交，通过 `scripts/get_maintainer.pl` 找到相应维护者和列表，并遵守 Linux Patch Review 约定。

以上来自各项目当前官方贡献文档。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+2



如果一个新特性需要同时修改 OpenSBI、OP-TEE 和 Linux，应该创建相互关联、遵守各项目流程的独立 Patch Series。

不要将它们混成一个无法单独审查的提交。

### Cross-Repository Feature 的正确组织

例如为新的 RISC-V TEE Transport 增加支持，可以将工作组织为：

```
Architecture RFC
    |
    +-- OpenSBI Series
    |     1. Transport capability
    |     2. Channel handling
    |     3. Domain integration
    |     4. Tests
    |
    +-- OP-TEE Series
    |     1. Transport abstraction
    |     2. RPMI backend
    |     3. RPC integration
    |     4. Tests
    |
    +-- Linux Series
          1. Binding / capability discovery
          2. Driver
          3. Shared memory management
          4. Tests
```

这里有两个设计要求：

首先，每个项目的 Patch Series 必须能够独立审查，明确描述自己的外部依赖。

其次，跨仓库接口需要明确版本与兼容策略，而不是假设三个仓库总是同时更新。

这就是 Staff Engineer 与普通移植工程师之间的一项重要差别：不仅要让当前组合工作，还要考虑不同版本组件之间的系统兼容性。

# 22.2 如何选择适合 Upstream 的 Issue？

## 22.2.1 并不是所有问题都值得提交 upstream

实际项目中，你可能遇到这些问题：

- 某款 SoC 的 UART Base Address 不正确。
- OP-TEE RISC-V Context Switch 遗漏某个 CSR。
- 某个 TA 的 Page Table Update 缺少 TLB Synchronization。
- 某一平台私有的 PMP Region Layout 有问题。
- OP-TEE Core 存在跨平台的整数溢出风险。
- 测试工具没有覆盖 RISC-V User TA 的 FP Context。

这些问题在工程价值上都可能重要，但从 upstream 角度看，其优先级与处理方式有很大差异。

我建议采用五个判断标准：

1. Generality：是否能够改善通用代码，或者影响多个平台？
2. Correctness/Security：是否修复真实的正确性或安全问题？
3. Reproducibility：是否能够构造最小复现案例？
4. Reviewability：是否能够提供足够明确的代码变更和测试？
5. Maintainability：是否不会引入无法长期维护的特例？

对于安全漏洞，还必须增加第六个判断：是否应通过私密安全报告流程处理，而不是直接公开提交。

## 22.2.2 建立一个 Issue Selection Matrix

下面是我建议用于实际工作的优先级模型。

Issue 选择示例

综合优先级

| 候选问题                      | 综合分 | 工作量 |
| ------------------------- | --- | --- |
| RISC-V 缺失 TLB 同步          | 21  | 3/5 |
| Trusted Thread FP 状态泄露    | 19  | 4/5 |
| 补充 RISC-V Regression Test | 19  | 2/5 |
| 大规模 MMU Refactoring       | 15  | 5/5 |
| 新增私有 SoC UART Driver      | 9   | 2/5 |

假设性候选项，不代表已经确认的 upstream Bug。评分公式为 2×影响 + 2×通用性 + 可复现性 − 工作量；仅供个人任务排序。

这里比较值得注意的是：新增 Regression Test 往往是非常优质的 upstream 切入点。

原因是它同时满足几个条件：

- Reviewer 很容易理解测试目的。
- 不需要先完成大型架构重构。
- 可以长期防止某类问题重新出现。
- 能够让 Maintainer 看见你对正确性和测试体系的理解。

对于你这种已经有底层经验的工程师，我不建议把大量时间花在纯粹的格式修复上。

你更适合寻找能够体现 Kernel、MMU、Context Switch、SMP 或 Security 理解的问题，并通过 Regression Test 降低审查风险。

## 22.2.3 如何从代码和社区历史发现 Issue？

我建议建立如下日常工作流程：

```
Read a subsystem
    |
    v
Identify an invariant
    |
    v
Check whether it is enforced
    |
    +-- Yes --> Inspect edge cases / concurrency
    |
    +-- No  --> Construct minimal reproduction
                       |
                       v
                Check existing issues/PRs
                       |
                       v
                 Confirm ownership
                       |
                       v
                 Test / Fix / RFC
```

假设正在分析 RISC-V MMU。

你首先定义一个安全不变量：

> 当某个 TA 的虚拟页从 Valid 变成 Invalid 后，相关 hart 不应继续通过过期的地址转换访问被撤销的物理内存。

接着研究：

1. 哪些函数修改 PTE？
2. 修改后是否执行 `SFENCE.VMA`？
3. 仅执行本地 `SFENCE.VMA` 是否足够？
4. 其他 hart 能否运行同一个地址空间？
5. ASID 是否被正确分配、复用和失效？
6. 是否已经有相应的回归测试？

你会发现，真正有价值的 Issue 往往不是通过浏览 Issues 页面找到的，而是通过 验证架构不变量 发现的。

这也是我建议你长期采用的源码研究方式。

# 22.3 Coding Style、Commit Message、DCO 与 Patch 拆分

这部分虽然属于开发流程，但对于能否高效率进入 upstream 非常关键。

## 22.3.1 OP-TEE Coding Style 的特殊要求

OP-TEE 大体采用 Linux Kernel Coding Style，但有少量项目特有规则。

例如：

- 允许使用 GlobalPlatform 定义的 CamelCase 类型。
- 第三方引入的密码学库等通常不进行大规模格式改写。
- 特别强调变量初始化，减少未初始化状态导致的安全风险。
- 对无符号整数常量的宽度有明确约定。

这些要求在当前官方 Coding Standards 中均有说明。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



例如：

```
/* 不推荐：状态可能未初始化 */
struct tee_request req;

/* 推荐：明确初始化 */
struct tee_request req = { };
```

这里 `tee_request` 是用于教学的假设结构。

为什么 OP-TEE 对初始化要求这么严格？

假设：

```
struct tee_request {
    uint64_t secure_pa;
    uint32_t flags;
    uint32_t reserved;
};
```

如果 `reserved` 未初始化，却被复制到 REE 可读取的 Shared Memory 中，可能发生安全信息泄露。

如果未初始化的是权限字段，则可能造成错误的授权判断。

因此 Coding Style 在安全内核中不仅影响可读性，还直接与 Security Correctness 有关。

## 22.3.2 如何运行 Checkpatch？

当前 OP-TEE 支持使用 Linux `checkpatch.pl` 配合本地检查脚本：

```
export CHECKPATCH=/path/to/linux/scripts/checkpatch.pl

cd optee_os

# 检查一个已提交的 commit
./scripts/checkpatch.sh HEAD

# 检查相对 upstream/master 的改动
./scripts/checkpatch.sh --diff upstream/master HEAD

# 检查暂存区
make checkpatch-staging

# 检查工作区改动
make checkpatch-working
```

这与官方文档提供的调用方式一致。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



但需要明确：

`checkpatch` 只是一项静态规范检查，不能证明内存安全、竞态正确性或 ABI 兼容性。

一个 Patch 即使完全通过 `checkpatch`，依然可能引入严重的 Domain Isolation 漏洞。

## 22.3.3 正确的 Commit Message

我建议每个 Commit 都尽可能回答三个问题：

What was wrong?

Why was it wrong?

How does this change fix it?

例如，下面是一条用于教学的 RISC-V TLB 修复 Commit：

示例 Commit Message（假设性问题）

```
core: riscv: synchronize address translation after PTE update

A user mapping may be invalidated while an older translation
remains cached in the local hart TLB.

This can allow subsequent accesses to use an obsolete mapping
instead of observing the updated page table permissions.

Synchronize address translation after updating the affected
mapping. Keep the invalidation within the current address
space and document the required synchronization ordering.

Add a regression test covering mapping revocation.

Signed-off-by: Example Developer <developer@example.com>
```

这里故意没有加入 `Fixes:`，因为没有确认对应的引入问题 Commit。

如果是修复既有回归，应该找到真正引入问题的 Commit，并按照项目要求填写 `Fixes:`，而不是猜测一个 Commit ID。

官方文档要求 Commit Subject 尽量精确，通常使用子系统前缀，Subject 不超过 80 字符，正文建议按 72 字符左右换行。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



### 为什么 Commit Message 不能只写修复结果？

例如：

```
riscv: fix page fault
```

这样的 Commit Message 不合格，因为未来 Maintainer 无法理解：

- Page Fault 在什么条件下出现？
- 哪个不变量被违反？
- 为什么选择这个修复点？
- 有没有其他架构受到影响？
- 改动是否依赖特殊配置？

一个好的 Commit Message 应当能够让几年后的维护者在不阅读完整 PR 讨论的情况下，理解当时的设计原因。

## 22.3.4 DCO 与 Signed-off-by

DCO（Developer Certificate of Origin）解决的是代码贡献的来源与授权声明问题。

在 OP-TEE 中，贡献者必须满足 DCO 要求，使用真实身份完成 Signed-off-by。最简单的方式是：

```
git config user.name "Your Real Name"
git config user.email "you@example.com"

git commit -s
```

其中的姓名和邮箱需要替换为你有权使用的真实信息。

`git commit -s` 会自动添加类似：

```
Signed-off-by: Your Real Name <you@example.com>
```

注意：

- `Signed-off-by` 不是代码审查通过的证明。
- `Signed-off-by` 不等于 GPG 签名。
- 它不代表公司自动授权公开内部源代码。
- 对包含雇主知识产权的改动，必须先确认贡献权限及公开披露边界。

对实际 SoC 项目尤其需要谨慎：公司内部的 RTL、地址映射、密钥方案、专有驱动或 NDA 信息，不能因为想贡献 upstream 就直接公开。

DCO 具体规则以 [OP-TEE Contribute](https://optee.readthedocs.io/en/latest/general/contribute.html?utm_source=chatgpt.com) 为准。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



## 22.3.5 Patch 拆分：One Logical Change per Commit

假设要在 RISC-V OP-TEE 中新增 FP/Vector Context Support。

一个不理想的 Commit 可能同时包含：

```
Add RISC-V vector support

  - Modify entry.S
  - Modify thread context
  - Modify user TA context
  - Add config option
  - Modify ABI
  - Add 700 lines of test code
  - Reformat unrelated headers
```

即使最终可以运行，也很难 Review。

更好的 Patch Series：

```
[PATCH 0/5] RISC-V: Add vector context management

[PATCH 1/5]
core: riscv: define vector state layout

[PATCH 2/5]
core: riscv: add vector save and restore helpers

[PATCH 3/5]
core: riscv: integrate vector state with TA context

[PATCH 4/5]
core: riscv: enable vector context configuration

[PATCH 5/5]
tests: riscv: verify vector register isolation
```

以上都是示例标题，不代表真实 PR。

为什么这样更好？

因为 Reviewer 可以分别验证：

- Context Layout 是否正确。
- Save/Restore 汇编是否符合架构。
- 生命周期集成是否完整。
- Feature Detection 和配置逻辑是否安全。
- 测试是否覆盖跨 TA 泄露。

每个 Commit 都应该构成一个清晰的逻辑单位，尽可能保持可编译，并避免引入临时性的危险状态。

同时，不要机械地将每一个函数都拆成单独 Commit。如果两部分变更必须同时存在才能保证正确性，就应作为一个原子改动提交。

# 22.4 如何设计测试以证明 Patch 的正确性？

这是我认为整个 Chapter 22 中最值得认真学习的部分之一。

在普通应用软件中，测试通过往往意味着主要功能能够正常工作。

但对于 OP-TEE 这样的 Trusted Kernel：

功能测试通过，不代表安全性质得到证明。

例如，给 RISC-V Context Switch 增加 Vector Register Save/Restore 后，你可能发现两个 TA 都能正常执行 Vector 指令。

这只能证明正常功能基本可用，不能证明：

- TA A 的寄存器内容不会泄露给 TA B。
- Linux Domain 无法读取 OP-TEE 的 Vector State。
- RPC Suspend/Resume 后状态不会损坏。
- Trap 或 Interrupt 发生时不会丢失状态。
- 多 hart 并发切换时不存在 Race Condition。

因此必须建立分层测试体系。

## 22.4.1 五层验证模型

L5 — Security Invariant Validation

机密性、完整性、权限隔离、跨 Domain 状态泄露和异常恢复

L4 — Cross-Component Integration

Linux → OpenSBI → OP-TEE → TA → RPC 的完整调用链

L3 — Architecture / SMP Validation

CSR、Context、TLB、IPI、Interrupt、Cache、并发

L2 — Functional Regression

xtest、TA 生命周期、参数、错误路径、边界条件

L1 — Static & Build Validation

Coding Style、Compiler、链接、不同配置矩阵

这五层没有严格的执行先后要求。实际项目中通常先运行低成本测试，再进行更昂贵的系统与安全验证。

### L1：编译及静态检查

以 RISC-V 为例：

```
make -j"$(nproc)" \
    ARCH=riscv \
    PLATFORM=virt \
    CROSS_COMPILE64=riscv64-linux-gnu- \
    O=out-riscv
```

如果修改的是架构无关代码，还应该编译 Arm 平台：

```
make -j"$(nproc)" \
    PLATFORM=vexpress-qemu_armv8a \
    CFG_ARM64_core=y \
    CROSS_COMPILE64=aarch64-linux-gnu- \
    O=out-arm64
```

上述命令展示的是 Core 构建，具体环境还需要满足对应依赖。

当前 OP-TEE CI 对 RISC-V 已包含 `virt`、`sifive`、`cva6`，以及 PLIC、APLIC、IMSIC 等配置组合。它同时具有 Arm QEMU 的运行测试任务。不能把 RISC-V 编译矩阵通过误认为已经执行了完整的 RISC-V xtest。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### L2：Functional Regression

OP-TEE 的主要测试项目是 `optee_test`，由 Linux/REE 侧的 `xtest` 驱动测试逻辑，通过 TEE Client API 运行。

例如在目标系统内：

```
# 默认测试
xtest

# 指定单个测试
xtest 1001

# 运行 regression 测试
xtest -t regression

# 运行指定测试组
xtest _1
```

这些是官方支持的典型用法。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



当你修改 TA Lifecycle 或 RPC 时，最好不仅运行一次全量 xtest，还要增加专门覆盖修改条件的测试案例。

### L3：Architecture Validation

以 RISC-V `satp` 切换为例，测试可能需要检查：

- `satp.MODE` 是否符合平台能力。
- Root Page Table PPN 是否正确。
- ASID 是否属于预期的地址空间。
- 发生页表变化后是否执行必要的地址转换同步。
- 是否存在其他正在使用相同地址空间的 hart。
- ASID 重用时是否正确处理旧地址转换。

这类测试不能只检查返回值是否为 `TEE_SUCCESS`。

必要时，应当记录进入和退出关键函数前后的 CSR，并设计多 hart 负载。

### L4：Integration Validation

你可以建立如下端到端测试：

```
Linux CA
   |
   | OpenSession
   v
OP-TEE TA A
   |
   | Allocate secure memory
   | Perform cryptographic operation
   |
   | Trigger RPC
   v
Linux tee-supplicant
   |
   | Return RPC result
   v
OP-TEE TA A
   |
   | Verify previous context
   v
Linux CA
```

这一测试不仅检查最终结果，还验证 RPC 之后的线程和 TA 状态是否正确恢复。

### L5：Security Invariant Validation

假设设计一个验证 TA 内存隔离的实验。

TA A 将一个随机秘密放入其私有内存，TA B 尝试访问 A 的地址范围。

正确的测试目标并不是让 B 猜中这个秘密，而是验证：

TA B 无论通过直接地址访问、错误共享内存描述符还是非法参数组合，都不能获得不属于它的内存访问权限。

实验还应考虑 Core Panic、异常恢复和资源释放。

对于 RISC-V，还需要补充一个系统级测试：

如果 Linux 或非可信 DMA Master 试图访问 OP-TEE 物理内存，实际硬件保护是否阻止该访问？

这类隔离验证不一定能在普通 QEMU virt 配置中完整完成，需要根据模拟的硬件能力与目标 SoC 的实际保护机制决定验证范围。

## 22.4.2 测试必须包含 Negative Path

很多 Patch 只测试 Happy Path。

例如新增一项 TEE RPC Feature，作者测试：

```
Valid request → Success
```

但是可靠的测试应该包括：

| 情况                  | 预期行为            |
| ------------------- | --------------- |
| 正常请求                | 成功完成            |
| 非法参数                | 明确拒绝，不产生越界访问    |
| 超长 Buffer           | 验证长度，不发生整数溢出    |
| 非法 Physical Address | 不映射、不访问未授权内存    |
| 错误 Session ID       | 不访问其他会话的资源      |
| RPC 提前失败            | 恢复或终止相关状态       |
| 重复 Close            | 不出现 Double Free |
| 多线程并发               | 无竞态或跨会话污染       |
| 系统资源耗尽              | 可控地返回错误         |
| 请求被取消               | 不留下错误的锁和资源状态    |

这些场景并不全部适用于每种 RPC，而应根据接口的状态机和威胁模型选择。

## 22.4.3 什么才算一个高质量 Regression Test？

一个好的 Regression Test 应该满足：

1. 在修复之前，可以稳定触发或识别原始错误。
2. 在修复之后，可以稳定通过。
3. 不依赖不必要的全局状态和特定执行时序。
4. 如果依赖某项硬件特性，能够进行 Capability Detection。
5. 对失败结果有明确诊断能力。

例如，对于 SMP TLB Shootdown 问题，你可能需要构造：

```
Hart 0:
    Update mapping
    Trigger required synchronization

Hart 1:
    Continue accessing affected address space
    Verify expected translation/permission behavior
```

这里有一个需要注意的点：

单靠重复运行 10000 次并不能证明不存在 SMP Race。

如果 Test 没有构造出必要的并发交错，即使运行 100 万次也可能无法发现问题。

更好的策略是使用 Barrier、状态同步、受控的执行窗口和必要的 Fault Injection，让问题所依赖的时序更容易出现。

同时区分两个目标：

- Functional Regression：验证 bug 不再出现。
- Security Argument：论证为什么在架构约束下不存在未授权访问路径。

## 22.4.4 测试完成后，应该交付什么证据？

我建议每个非平凡的 OP-TEE Patch 都附带一份简短的测试记录。

Patch Validation Record — 示例

| Field          | Value                      |
| -------------- | -------------------------- |
| Platform       | QEMU RV64 virt             |
| Base Commit    | 实际测试时记录的 SHA               |
| Toolchain      | GCC 版本及目标 Triple           |
| Test           | xtest + 自定义 MMU Regression |
| Before         | 失败，命中预期错误条件                |
| After          | 通过                         |
| SMP            | 1/2/4/8 hart 配置测试          |
| Arm Regression | Arm64 QEMU 通过或标明未执行        |
| Limitations    | QEMU 不完全模拟目标硬件隔离           |

这是建议的测试报告格式，而不是已经执行过的测试结果。

如果不能运行某项测试，应明确写出原因，不要用 `Tested` 描述未执行的项目。

# 22.5 Bugfix、Feature 与 Refactoring 的不同 Review 标准

不是所有 Patch 都采用相同的 Review 方法。

一个只有三行代码的 Bugfix，可能比一个几百行的 Feature 更需要安全分析。

## 22.5.1 三种 Patch 的本质区别

| 类型          | 主要目标      | 关键 Review 问题            |
| ----------- | --------- | ----------------------- |
| Bugfix      | 恢复原本应有的行为 | Root Cause 是否准确？是否完整修复？ |
| Feature     | 引入新的可观察能力 | 接口是否合理？兼容性如何？谁维护？       |
| Refactoring | 改善内部结构    | 是否证明行为等价？为什么值得改？        |

### Bugfix Review

应该重点验证 Root Cause、影响范围、错误处理路径、回归测试及是否需要 backport。

例如修复 TLB 相关问题时，不能因为插入一条 `SFENCE.VMA` 就认为完成。

还应验证：

- 需要的是本地还是跨 hart 同步？
- 所有相关页表修改路径都覆盖了吗？
- 目标地址空间可能在其他 hart 上运行吗？
- 这条 Fence 是否真的具有预期的架构语义？
- 是否引入不必要的 Performance Regression？

目前社区有一个真实案例：

[PR #8104 — RISC-V PTE 生效后的地址转换同步](https://github.com/OP-TEE/optee_os/pull/8104?utm_source=chatgpt.com)。

这个 PR 可以用来练习如何从 RISC-V 页表规范、实际代码和性能影响三个维度审查补丁。它的具体适用条件必须以 PR 中的代码和说明为准，不能简单推广为所有 PTE 更新都应增加完全相同的 Fence。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### Feature Review

Feature Review 的重点更偏向架构设计。

假设开发者提交了一个新功能：

> Support RISC-V Vector Context Switching in OP-TEE。

Reviewer 需要分析：

- ISA Extension Discovery 如何完成？
- 不支持 Vector 的硬件如何执行？
- 不同 `VLEN` 的实现如何适配？
- Vector State 如何分配、保存和清理？
- 切换时 `sstatus.VS` 如何变化？
- TA、Thread 和 Domain Context 分别负责哪部分状态？
- 如何防止跨 TA / Domain 数据残留？
- 是否引入新的 ABI 或配置限制？
- 有没有足够的 Regression Test？

注意，RISC-V Vector Register State 并不只是整数寄存器数组。不同实现可能具有不同的 VLEN，因此不能写死为某个固定的 128-bit 或 256-bit 大小。

Feature Review 的核心，是确保引入的是可长期维护的抽象，而不是只适配当前某一个 SoC。

### Refactoring Review

Refactoring 往往需要最强的论证，因为它没有直接引入新功能，却可能影响大量代码。

例如：

```
Before:
    Arm-specific Thread MMU Handling
    RISC-V-specific Thread MMU Handling

After:
    Common Thread MMU Framework
        |
        +-- Arm callbacks
        +-- RISC-V callbacks
```

这个设计表面上提高了复用性，但也可能存在问题：

- 共同抽象是否真的表达了两个架构的相同语义？
- 是否为了统一接口引入过多 Callback？
- 是否把架构独有的同步语义隐藏在 Common Code 中？
- 是否增加了难以测试的状态组合？

因此 Refactoring 的判断标准应该是：

可维护性收益必须足够明确，而且应该有证据证明原有行为没有发生意外变化。

## 22.5.2 OP-TEE 特有的 Review Feedback Workflow

这一点需要特别强调，因为 OP-TEE 的正式贡献指南与很多项目的习惯不同。

当前官方指导要求：

Review 期间，先追加 Fixup Commit，不要立即 Squash 并 Force Push。待审查意见处理完毕并获得相应认可后，再集中整理 Commit。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



为什么这样设计？

因为 Reviewer 希望容易地看到你针对每一轮意见做了什么修改，而不是每次都重新阅读整个 Patch。

例如初始有：

```
A: Add RISC-V context structure
B: Add register save/restore support
```

Review 后追加：

```
C: fixup! Add RISC-V context structure
D: fixup! Add register save/restore support
```

可用：

```
git commit --fixup=<commit-A>
git commit --fixup=<commit-B>
```

这些命令是示意，应该在对应代码修改暂存后执行。

待 Review 完成，再进行：

```
git rebase -i --autosquash upstream/master
```

此时如果 upstream 已新增提交，可能先需要同步基线并解决冲突；不能盲目假定本地基线没有变化。

检查最终 Commit 历史和测试结果之后：

```
git push --force-with-lease origin my-feature-branch
```

这里使用 `--force-with-lease` 是为了尽可能避免覆盖远端已经发生的非预期更新。

最终 Commit 应当保持清晰、独立，并正确保留由 Reviewer 实际授予的 `Reviewed-by:`、`Acked-by:` 等标签。

不要自行添加别人没有明确给予的 Review 标签。

# 22.6 如何识别 ABI 与 Security Compatibility 风险？

这一节是 Staff Engineer 和 Maintainer 必须重点掌握的内容。

因为在系统软件中，接口一旦进入 upstream，就可能被大量外部组件依赖。

尤其对于 OP-TEE，它不是一个独立运行的软件。

它与 Linux、OpenSBI、Boot Firmware、User TA、Client Library 和 Hardware Root of Trust 之间存在多重兼容关系。

## 22.6.1 API 与 ABI 不是一回事

API（Application Programming Interface）描述源代码层面的接口。

ABI（Application Binary Interface）描述二进制组件之间的约定，例如：

- 调用约定。
- 寄存器和参数布局。
- 数据结构在内存中的 Layout。
- 整数大小、对齐和字节序。
- Function ID / Message ID。
- 返回值语义。
- 二进制兼容性约束。

举个例子：

```
struct tee_message_v1 {
    uint32_t command;
    uint32_t flags;
    uint64_t address;
};
```

假设你为了扩展一个 Feature，将它改成：

```
struct tee_message_v2 {
    uint32_t command;
    uint64_t address;
    uint32_t flags;
};
```

即使成员的名称和含义没有变化，结构体的字段偏移和对齐也可能变化。

对于已经编译好的旧版 Linux Driver 来说，它可能按照旧 Layout 解释数据，导致参数读取错误。

因此：

C 结构体可以重新编译，不代表跨组件 ABI 可以任意修改。

上述结构体仅用于讲解布局问题，并非 OP-TEE 的真实 ABI 定义。

## 22.6.2 OP-TEE 的关键兼容性边界

| Boundary                   | 需要重点关注                    |
| -------------------------- | ------------------------- |
| CA ↔ libteec               | GlobalPlatform Client API |
| libteec ↔ Linux TEE Driver | ioctl UAPI                |
| Linux ↔ OP-TEE             | Message ABI / Transport   |
| Linux ↔ OpenSBI            | SBI Extension / MPXY      |
| OpenSBI ↔ OP-TEE           | Domain 切换及平台协定            |
| OP-TEE Core ↔ TA           | TA ABI / Syscall          |
| OP-TEE ↔ Secure Storage    | 持久化格式和密钥派生                |
| OP-TEE ↔ HSM               | Secure IPC、密钥句柄和版本协商      |

Linux TEE UAPI 中，例如 `TEE_IOC_VERSION`、`TEE_IOC_OPEN_SESSION`、`TEE_IOC_INVOKE`、`TEE_IOC_CANCEL` 等接口，直接关系到用户态 Client 和 Kernel Driver 的兼容性。

[image](https://www.google.com/s2/favicons?domain=https://kernel.org\&sz=32)

The Linux Kernel documentation



## 22.6.3 设计新 ABI 时应遵守的原则

假设正在设计新的 RISC-V TEE Transport。

建议明确以下机制：

Version Negotiation

如果未来需要扩展消息格式，双方如何发现协议版本？

Capability Discovery

如果 OpenSBI 支持某个可选 Feature，但 OP-TEE 不支持，应如何处理？

Length and Bounds Checking

所有来自 REE 的长度、偏移、地址和数组数量都应作为非可信输入验证。

Backward Compatibility

新固件能否继续支持旧版 Linux？

Failure Semantics

当 RPC 中断、请求取消或 Domain Switch 失败时，返回值和资源状态应如何定义？

Concurrency Semantics

多个 hart、多个 Session 是否可以同时发起请求？

Security Binding

一次请求应如何绑定到正确的 Session、TA Context 或安全身份？

这里还有一个非常现实的问题：Capability Detection 本身并不自动等于授权。

REE 提供某个 Feature ID，不能作为它有权访问某项 Secure Service 的证明。

授权必须基于可信的服务端状态和安全策略。

## 22.6.4 用 Compatibility Matrix 验证跨版本行为

例如新 Transport 引入 Protocol v2：

| Linux Driver | OpenSBI | OP-TEE | 期望结果       |
| ------------ | ------- | ------ | ---------- |
| v1           | v1      | v1     | 原有功能正常     |
| v1           | v2      | v2     | 兼容旧协议或明确拒绝 |
| v2           | v1      | v2     | 发现不支持，安全失败 |
| v2           | v2      | v1     | 能力协商或回退    |
| v2           | v2      | v2     | 新功能正常      |

这里的 v1/v2 是教学中的假设协议版本，不代表具体的 RPMI 标准版本。

一个好的兼容设计不一定要求支持所有新旧版本组合，但必须定义行为，避免：

- 静默解析错误。
- 协议状态不一致。
- 权限提升。
- 未检查的数据结构布局变化。
- 安全降级而不通知调用方。

## 22.6.5 Security Compatibility：比 ABI 更隐蔽的问题

这里举一个非常典型的情况。

假设你优化 OP-TEE 的 Shared Memory Mapping，允许直接使用 REE 提供的 Buffer，减少一次内存拷贝。

性能提升很明显。

但是该 Buffer 属于非可信世界，REE 可能在 OP-TEE 读取之前、之后或期间修改它。

假设代码如下：

```
/* 非安全示例：非可信 length 被读取两次 */
if (shared->length <= MAX_DATA_SIZE)
    memcpy(dst, shared->data, shared->length);
```

如果 `shared->length` 位于 REE 可写的共享内存中，那么两次读取之间，它可能发生变化。

结果就可能是：

第一次验证一个合法长度，第二次却使用了不合法长度。

这属于典型的 Double Fetch / TOCTOU 风险。

较稳妥的方式是先将非可信元数据复制到安全侧临时变量，对其验证，再在适当的生命周期和访问规则下使用该快照。

例如：

```
size_t len = READ_ONCE(shared->length);

if (len > MAX_DATA_SIZE)
    return TEE_ERROR_BAD_PARAMETERS;

memcpy(dst, shared->data, len);
```

但这段代码只是在演示对长度使用同一个快照的原则，并不是可直接合入 OP-TEE 的完整安全实现。

它尚未解决 `shared->data` 本身的并发修改、共享内存有效性、地址范围验证、Buffer 生命周期和访问顺序等问题。

在安全敏感的操作中，通常还需要将必须保持一致的输入数据复制到可信内存，验证后只使用可信副本。

这也说明：

Performance Optimization 往往必须伴随 Security Boundary Review。

## 22.6.6 怎样判断某个问题是 Security Vulnerability？

OP-TEE 当前的 Security Scope 文档强调隔离域之间的 Confidentiality、Integrity 和 Availability，尤其关注横向隔离失效及 Confused Deputy 问题。并非所有崩溃、单域自身资源耗尽或理论缺陷都自动构成可报告的安全漏洞。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



我建议在判断时分析：

- 攻击者属于哪个隔离域？
- 攻击者拥有哪些能力？
- 被影响的资产属于哪个域？
- 是否跨越授权边界？
- 是否能够在真实产品配置中触发？
- 受影响的是通用 OP-TEE、特定 SoC，还是最终产品？

对于尚未公开的疑似漏洞，不应直接把利用细节提交为公开 GitHub Issue。

当前官方流程推荐通过 [OP-TEE Security Advisories](https://github.com/OP-TEE/optee_os/security?utm_source=chatgpt.com) 私密报告，也支持 TrustedFirmware.org 安全事件邮件流程。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



这不仅是技术礼仪，更关系到用户设备的安全及协调修复的能力。

# 22.7 如何阅读并参与 RFC？

RFC（Request for Comments）不仅是一个征求意见的文档。

在 OP-TEE、OpenSBI 等系统软件社区中，RFC 是在大规模代码变更之前，就设计方向、接口边界、兼容性和长期维护成本达成共识的机制。

越是跨架构、跨仓库、影响 TCB 或 ABI 的 Feature，越应该在正式实现之前参与 RFC。

## 22.7.1 什么问题需要先写 RFC？

一般来说，以下情况非常值得先进行设计讨论：

- 新增 RISC-V TEE Transport。
- 引入新的 Domain Isolation 模型。
- 改变 Trusted Thread 的执行或调度机制。
- 增加 FP/Vector Context Management。
- 改变 Core/User TA Memory Management 架构。
- 引入新的 Secure Storage Format。
- 需要修改多个项目的公开 ABI。
- 引入新的安全机制，例如 CFI、Shadow Stack。

而一个明显的空指针检查错误，通常没有必要为其写大型 RFC。

RFC 的价值不是增加流程，而是降低后期推翻设计的风险。

## 22.7.2 高质量 RFC 应包含什么？

我建议采用如下设计模板。

Architecture RFC Template

1. Problem Statement：目前存在什么问题？有什么实际证据？
2. Goals / Non-goals：本次解决什么问题？哪些明确不解决？
3. Current Architecture：现有调用链、数据结构和限制。
4. Proposed Design：新组件、状态机、接口和执行路径。
5. Security Model：攻击者能力、TCB 变化、信任边界。
6. ABI / Compatibility：版本、协商、旧实现行为。
7. Alternatives：考虑过哪些方案，为什么不选择？
8. Validation：怎样证明设计正确？如何进行回归测试？
9. Migration：如何分阶段合入？怎样防止短暂破坏兼容？
10. Open Questions：哪些技术决策仍需要社区讨论？

其中最容易被忽视的三个部分是：

Non-goals

如果不明确哪些问题不解决，Review 过程中就可能无限扩大 Scope。

Alternatives

如果只提供一个方案，Reviewer 难以判断你是否认真考虑过更简单的实现。

Migration

如果新设计需要同时更新 Linux、OpenSBI、OP-TEE，必须考虑某些组件暂时未升级的情况。

## 22.7.3 一个真实的 RISC-V OP-TEE RFC 案例

2026 年 10 月 8 日，OP-TEE 社区建立了：

[RFC #8096 — RISC-V Zicfilp Landing Pad Support for OP-TEE](https://github.com/OP-TEE/optee_os/issues/8096?utm_source=chatgpt.com)

这是一个非常适合学习的 RFC。

它讨论使用 RISC-V `Zicfilp` 实现 Forward-edge Control-Flow Integrity（CFI），并将 Arm BTI 作为参考设计。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 问题背景

假设程序存在一次间接跳转：

```
jalr ra, 0(t0)
```

普通情况下，CPU 根据 `t0` 指向的地址决定跳转目的地。

如果攻击者能够破坏某个间接跳转目标，程序就可能跳向非预期代码位置。

Zicfilp 通过 Landing Pad 机制约束合法间接跳转目标。

但是将它集成到 OP-TEE，远远不只是给编译器增加一个选项。

还需要处理：

- Core 是否启用 CFI。
- User TA 是否启用 CFI。
- Toolchain 是否支持。
- 手工汇编入口如何适配。
- 异常处理如何识别 CFI Fault。
- `ldelf`、TA Loader 如何处理相关属性。
- 在不同执行上下文之间如何管理硬件启用状态。
- OpenSBI 是否需要参与状态管理。

实际 RFC 就明确区分 Core/TA 配置，讨论 GNU/LLVM 支持、Runtime State、异常处理以及 OpenSBI 的相关依赖。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 我们应该怎样 Review 这个 RFC？

首先不要从「代码怎样写」开始，而是先分析安全不变量。

例如：

> 当执行上下文要求启用 CFI 时，任何允许执行间接跳转的路径都必须维持与该上下文一致的硬件 CFI 状态。

然后提出关键问题：

Q1：在 User TA 启用 Zicfilp、Core 未启用的情况下，Trap Entry 会发生什么？

需要分析 U-mode → S-mode 的状态变化、Landing Pad 状态以及 Trap Handler 使用的汇编入口。

Q2：如果 TA A 启用 CFI，TA B 未启用，Context Switch 是否可能让 B 继承 A 的设置？

这是 Security State Leakage。

Q3：发生 RPC，再由 Linux 返回 OP-TEE 后，CFI 状态如何恢复？

这关系到 OP-TEE Trusted Thread 和 OpenSBI Domain Context。

Q4：如果硬件不支持 Zicfilp，配置应该导致编译失败、启动失败，还是功能自动降级？

必须有清晰的 Capability Policy。

Q5：如果旧版 `ldelf` 加载了带有新 GNU Property 的 TA，预期行为是什么？

这属于 Loader ABI Compatibility。

这样的意见比简单回复「Looks good」具有更高的技术价值。

## 22.7.4 RFC Review 中如何提出高质量意见？

高质量 Review Comment 应当具备：

Observation → Risk → Reasoning → Suggestion

例如：

> The proposed context-switch path updates the CFI state, but the RFC does not yet describe how that state is restored after RPC resumption. Could we document the ownership of the relevant CSR fields and add a test covering a CFI-enabled TA across an RPC round trip?

这条示例评论没有简单否定设计，而是：

- 指出一个尚不明确的执行路径。
- 说明为什么它重要。
- 提出一个可以验证的改进建议。

我建议在参与 RFC 时，优先提出这种具体、可操作的问题。

# 22.8 Cross-Architecture Review：Arm 与 RISC-V 的代码复用

这是我认为你最有机会形成个人优势的技术方向。

你的目标不只是理解 Arm 与 RISC-V 两套代码，而是能够判断：

哪些机制应该放入 OP-TEE Common Core，哪些机制必须留在 Architecture-Specific Layer。

## 22.8.1 两个架构中相同的高层问题

Arm 和 RISC-V OP-TEE 都需要：

- Trusted Thread。
- Trap / Exception Handling。
- User TA Isolation。
- Virtual Memory。
- Syscall。
- Secure Services。
- RPC。
- 多核同步。

但实现基础不同。

| 关注点         | Arm TrustZone                 | RISC-V Domain 方案                |
| ----------- | ----------------------------- | ------------------------------- |
| 可信执行环境      | Secure World                  | 隔离的 Security Domain             |
| 特权机制        | EL3、S-EL1、S-EL0               | M/S/U                           |
| 安全域切换       | Secure Monitor / FF-A 等架构     | M-mode Firmware / Domain Switch |
| User TA     | S-EL0                         | U-mode                          |
| 页表          | Arm Translation Regime        | Sv39/Sv48 等                     |
| TLB 同步      | TLBI / DSB / ISB 等            | SFENCE.VMA 等                    |
| 中断架构        | GIC                           | PLIC / AIA                      |
| 安全物理隔离      | TrustZone + System Protection | PMP + Platform Isolation        |
| Firmware 接口 | SMC / SMCCC 等                 | SBI / MPXY / RPMI 等             |

RISC-V 还可能采用其他安全架构。因此这里比较的是本课程的 OpenSBI Domain 路线，而非全部 RISC-V TEE 实现。

## 22.8.2 为什么不能把 Arm 代码直接复制到 RISC-V？

例如 Context Switch。

一个简单的 Common Interface 可以是：

```
/* 教学用接口，并非真实 OP-TEE API */

void arch_save_context(struct arch_context *ctx);
void arch_restore_context(struct arch_context *ctx);
```

表面上两个架构都能实现。

但是它们需要保存的内容可能不同：

```
Arm:
    x0-x30
    SP / PC
    PSTATE
    FP/SIMD registers
    System registers
    Architecture security context

RISC-V:
    x1-x31
    sepc / sstatus
    sscratch / satp
    FP registers + fcsr
    Vector registers + vtype/vl/vstart...
    Domain-related architectural state
```

实际保存集合要根据调用约定、Trap 类型、Lazy State 管理和所有权来确定；并非每次切换都保存上述全部寄存器。

更重要的是：

OP-TEE Trusted Thread Context 和 OpenSBI Domain Context 不应该被混为同一个抽象。

因为两者的所有者不同。

OP-TEE Core 负责 Trusted Thread 和 TA 执行上下文，OpenSBI 则负责它所管理的 Domain 和 M-mode 相关切换状态。

如果把两个层次硬塞进一个 `arch_context`，可能造成职责错乱。

## 22.8.3 什么是好的 Common Architecture Abstraction？

我建议采用以下判断标准。

### 抽象行为，而不是抽象指令

例如：

```
/* 教学用伪接口 */

void arch_tlb_invalidate_all(void);
```

这是一个可能合理的抽象方向，因为它表达了上层期望的某种行为。

但是：

```
void arch_execute_sfence_vma(void);
```

不应该出现在通用 Arm/RISC-V Core 接口中，因为它把 RISC-V 的指令语义泄露到了 Common Layer。

然而即使是 `arch_tlb_invalidate_all()`，也不能仅凭名称认为它拥有完整的跨 hart 同步语义。

实际 API 必须进一步规定：

- 作用于当前 hart 还是所有相关 hart？
- 作用于哪个地址空间和 ASID？
- 调用前要求哪些 Page Table Store Ordering？
- 是否保证远程地址转换已经失效？
- 是否要求调用方持有某个锁？
- 返回时是否已经完成所有同步？

语义定义比函数名称更重要。

### 复用 Policy，保留 Architecture Mechanism

例如 TA Memory Isolation：

```
Common OP-TEE Core
    |
    | Policy:
    | TA A must not access TA B
    | Shared memory must be validated
    |
    v
Architecture MMU Interface
    |
    +-- Arm Page Table / TLBI
    |
    +-- RISC-V Sv39 / SFENCE.VMA
```

通用层负责：

- 哪些 Memory Region 可访问。
- 哪些映射需要创建和撤销。
- 哪个 TA Context 应当拥有权限。
- 什么情况下应拒绝请求。

架构层负责：

- 如何编码 PTE。
- 如何设置硬件页表。
- 如何执行必要的地址转换同步。
- 如何处理架构特有的 Fault。

这通常比将具体硬件行为写进 Common Core 更容易维护。

## 22.8.4 一个典型的错误抽象

假设开发者为了减少重复代码，新增：

```
void tee_secure_world_switch(void);
```

然后让 Arm 和 RISC-V 都实现这个函数。

问题是：

在 Arm TrustZone 中，Secure World Switch 可能由 Monitor/EL3 Firmware 组织。

而在本课程的 RISC-V Domain 方案中，Domain Switch 由 M-mode Firmware 负责，OP-TEE 并不直接控制全部切换过程。

因此，这个接口暗示 OP-TEE 自己拥有 World Switch 的全部职责，可能在架构上是错误的。

更合适的设计通常要从上层真实需求出发，例如：

- 提交跨域服务调用。
- Yield 当前 Trusted Thread。
- 恢复被挂起的调用。
- 通知或响应 RPC。

再由 Transport 和 Architecture Layer 完成具体机制。

## 22.8.5 如何做 Cross-Architecture Code Review？

我推荐按如下顺序：

```
Identify common requirement
        |
        v
Write architecture-neutral invariant
        |
        v
Read Arm implementation
        |
        v
Read RISC-V implementation
        |
        v
Compare semantics and state ownership
        |
        v
Identify common policy
        |
        v
Keep hardware mechanisms separate
        |
        v
Run cross-architecture regression
```

这里最容易犯的错误是：

看到 Arm 与 RISC-V 两个实现都存在相同函数名称，就认定它们具有完全相同的执行语义。

你应该把每个接口看作一项契约，而不是一个函数名。

对于想成为 OP-TEE Core Maintainer 的工程师，具备这项能力非常重要。

# 22.9 持续维护一个子系统意味着什么？

当一个工程师成为某个子系统的 Maintainer，工作内容会发生明显变化。

普通 Contributor 更关注：

> 我提交的 Patch 是否能够合入？

Maintainer 更关注：

> 这个子系统在未来几年能否保持正确、安全、可测试、可演进？

## 22.9.1 Maintainer 的五项持续责任

### 1. Architecture Consistency

维护子系统设计原则，避免加入越来越多的平台特例。

例如 RISC-V MMU，如果新平台要求增加 `SFENCE.VMA`，Maintainer 必须判断：

- 这是通用架构要求？
- 是某个 ISA Extension 的特定语义？
- 是旧实现遗漏的正确性问题？
- 还是某个 SoC 的硬件 Erratum？

不同原因会导致完全不同的实现位置。

### 2. Regression Ownership

当子系统被其他 Patch 影响时，维护者需要参与分析和修复。

例如一个通用 `core_mmu.c` Refactoring 导致 RV64 TA 启动失败，即使修改来自 Arm 开发者，RISC-V 维护者也应参与判断。

维护责任不是只 Review 自己写的代码。

### 3. Compatibility Management

新特性不能无意破坏旧平台。

对于 OP-TEE，Maintainer 需要关心：

- 旧 TA 是否还能运行。
- 旧 Linux Driver 是否兼容。
- 禁用 Feature 的构建路径是否正常。
- 32/64-bit、不同 Compiler 是否兼容。
- 已发布版本是否需要 Backport。

### 4. Security Stewardship

如果有人私密报告某个子系统的漏洞，维护者可能需要在受控流程内协助：

- 评估受影响版本与配置。
- 复现和确认 Root Cause。
- 设计修复。
- 评估类似代码是否存在同类漏洞。
- 为受影响维护分支准备修复。
- 配合安全团队协调披露。

这部分必须遵守项目的安全事件流程，不能自行公开未披露的细节。

### 5. Reviewer & Contributor Development

优秀的 Maintainer 不应该成为所有技术决策的单点瓶颈。

他还需要帮助其他贡献者理解：

- 为什么原设计如此实现。
- 什么情况下可以修改。
- 什么测试可以证明安全。
- 哪些改动需要先进行 RFC。

持续培养新的 Reviewer，是子系统长期健康的重要条件。

## 22.9.2 维护一个子系统应保留哪些文档？

我建议至少维护以下工程资产：

| 资产                       | 内容                   |
| ------------------------ | -------------------- |
| Architecture Overview    | 设计目标、组件边界、调用链        |
| Security Invariants      | 必须持续成立的安全性质          |
| Supported Configurations | 已验证的硬件、Toolchain、配置  |
| Regression Suite         | 功能、并发、异常和安全测试        |
| Known Limitations        | 尚未支持的能力和限制           |
| Design Decisions         | 关键设计选择及其原因           |
| Change History           | 重大 ABI/行为变化          |
| Debug Guide              | 日志、寄存器、常见 Fault、复现方法 |

例如，你如果长期维护 RISC-V Context Switch，那么最有价值的文档可能不是 `context_switch.md` 里介绍一个函数如何执行，而是：

《RISC-V OP-TEE Context Ownership and Switching Invariants》

应明确哪些状态由 OpenSBI 管理、哪些由 OP-TEE Thread 管理、哪些由 User TA Context 管理、何时需要清理敏感寄存器，以及哪些路径可能产生嵌套状态切换。

这类文档对未来新的维护者非常有价值。

# 22.10 从 Contributor 到 Reviewer / Maintainer

最后，我们把前面的技术能力映射到一个可执行的社区成长路线。

首先强调：

成为 Maintainer 并不等于获得某种考试证书。社区信任通常来自长期、持续且可验证的技术贡献。

## 22.10.1 四个能力阶段

1. Contributor — 能独立提交高质量 Patch

   能够定位 Root Cause、提出最小修复、补充 Regression Test、写出清晰的 Commit Message，并独立处理 Review Feedback。
2. Active Reviewer — 能判断别人代码的正确性

   能够解释安全不变量、识别执行路径遗漏、发现潜在 ABI 兼容问题，并提出可操作的 Review Comments。
3. Subsystem Expert — 能主导设计演进

   熟悉一个子系统的历史、约束和跨架构实现，能够推动 RFC、测试基础设施及复杂架构改动。
4. Maintainer Candidate — 能承担长期责任

   持续参与该领域的 Review、回归修复、发布兼容性、安全响应和新贡献者支持，获得社区认可。

它不是一个自动晋升的四级制度，而是我建议采用的个人能力模型。

## 22.10.2 我建议优先建立的维护领域

在 RISC-V OP-TEE 中，最适合你的长期方向，我认为有三个。

第一选择：RISC-V Thread / Context Management

理由是它结合了汇编、RISC-V Privileged Architecture、Trusted Thread、RPC、FP/Vector 和安全状态清理。

第二选择：RISC-V MMU / TLB / TA Isolation

理由是它具备跨架构共通性，同时对安全正确性要求非常高。

第三选择：OpenSBI Domain & OP-TEE Integration

理由是它覆盖 OpenSBI、硬件隔离、Domain Switching、SMP 与跨项目架构设计。

如果未来希望成为 OP-TEE Core Maintainer，我会把第二个方向作为尤其值得长期深耕的领域，因为通用内存管理知识更容易向其他架构和子系统延伸。

## 22.10.3 一条现实的 12 个月贡献路线

| 时间          | 核心目标                      | 可衡量成果                   |
| ----------- | ------------------------- | ----------------------- |
| Month 1–2   | 构建完整环境、理解一个子系统            | 独立的代码分析与测试基线            |
| Month 3–4   | 提交正确性修复和 Regression Test  | 争取 2–4 个高质量 PR          |
| Month 5–6   | 开始系统 Review 他人的 RISC-V PR | 形成实质性 Review 记录         |
| Month 7–8   | 参与一个 RFC 或较大的 Feature     | 架构讨论与可分阶段的 Patch Series |
| Month 9–10  | 承担某一范围的持续维护               | 回归问题处理、测试补充、文档改进        |
| Month 11–12 | 与相关 Maintainer 建立稳定协作     | 明确长期负责的代码和技术领域          |

这是个人发展规划，不是 OP-TEE 官方的 Maintainer 选拔标准，也不保证一年内取得合并权限。

另外，我不建议把 PR 数量当成唯一 KPI。

一个复杂、安全关键且长期维护的改动，可能比十个简单的格式修复具有更高价值。

## 22.10.4 怎样判断自己已经具备 Maintainer 级别的能力？

我建议使用以下自评标准：

Maintainer Readiness Self-Assessment

每项选择 1–5 分。1 = 基础理解，3 = 能独立承担工作，5 = 能指导他人并主导设计。

Architecture

—/5

能够解释自己负责子系统的安全不变量、状态所有权和架构边界

1

2

3

4

5

Code Review

—/5

能够发现真实 correctness、concurrency 和 security 风险

1

2

3

4

5

Compatibility

—/5

能够识别 ABI、版本升级和跨架构行为变化

1

2

3

4

5

Testing

—/5

能够构造可复现测试，并解释测试覆盖的局限

1

2

3

4

5

Collaboration

—/5

能够参与 RFC、处理不同设计意见并推动共识

1

2

3

4

5

Maintenance

—/5

愿意持续处理他人的 Bug、Review 与回归

1

2

3

4

5

能力自评分

# 0

&#x20;/ 30

重置评分

这是个人学习工具，不代表社区的正式评价或授权标准。

即使你对某个子系统已经非常熟悉，如果没有长期 Review 记录、没有处理别人代码的经验，也不能仅凭技术理解就认为已经具备全部 Maintainer 能力。

# 22.11 完整实战：从真实 RISC-V MMU Bug 到 Upstream Merge

前面的内容如果只停留在理论，仍然不足以掌握 Upstream Engineering。

因此最后我们分析一个真实案例，串起：

Root Cause → Architecture Review → Patch Design → Regression Testing → Code Review → Merge → Maintenance。

## 22.11.1 案例背景：RISC-V Page Table Synchronization

我们使用此前提到的真实 PR：

[OP-TEE PR #8104 — Synchronize translations after making PTEs valid](https://github.com/OP-TEE/optee_os/pull/8104?utm_source=chatgpt.com)

该 PR 已于 2026 年 11 月 4 日合入 OP-TEE master。它非常适合作为一项工程案例，因为讨论涉及真实硬件故障、RISC-V 架构约束、性能取舍和 Reviewer 反馈。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### Step 1：理解问题

在 RISC-V 中，将某个 PTE 从 Invalid 修改成 Valid 后，并不能简单地认为处理器立刻就一定能观察到新的地址转换。

尤其需要考虑处理器是否可以缓存 Invalid PTE，以及是否实现了 `Svvptc` 等相关扩展。

抽象过程如下：

```
Initial state:
    PTE.V = 0

Hart performs address translation:
    May observe/cache invalid PTE

Software updates:
    PTE.V = 1

Hart performs address translation again:
    May still observe old invalid translation

Potential result:
    Spurious Page Fault
```

为什么这不是简单的普通 Memory Barrier 问题？

因为 `FENCE` 或一般内存屏障与 `SFENCE.VMA` 解决的并不完全是同一种同步需求。

前者主要涉及内存访问顺序，后者涉及地址转换相关状态及页表更新的同步。

该 PR 的核心问题就是原有 `core_mmu_table_write_barrier()` 仅执行 `mb()`，对于缺少相关硬件保证的实现不足以处理这种情况。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### Step 2：观察真实硬件证据

该 PR 提供了一个特别有说服力的现象：

在部分真实 RISC-V 硬件上，OP-TEE 初始化时会因新建映射后的地址转换问题触发 Page Fault。

而 QEMU 并不能复现同样问题，因为它的相关地址转换行为没有模拟导致该故障的 Invalid PTE 缓存条件。

这说明：

QEMU Test Passing 不等于 Hardware Architecture Correctness。

这是一个非常重要的工程认识。

如果开发者仅依赖 QEMU，可能错误地认为：

```
Boot success
    +
xtest success
    =
MMU implementation is correct
```

实际上不是。

正确性还需要从 RISC-V Architecture Specification 推导，并在具有相关行为的真实硬件上验证。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### Step 3：Review 修复方案

最初可能想到：

```
/* 教学示例 */
write_pte(...);
flush_tlb();
```

但这里有若干问题需要分析。

首先，Flush 的 Scope 是什么？

是当前 hart 的所有地址转换、某个地址空间，还是仅特定虚拟地址？

其次，是否所有实现都需要？

如果某些 CPU 实现了能改变相关同步需求的扩展，那么无条件执行完整 Flush 可能存在性能浪费。

第三，跨 hart 怎么办？

本地 `SFENCE.VMA` 并不自动替其他 hart 完成所有同步。

第四，Compiler Ordering 怎么保证？

如果内联汇编没有正确表达内存副作用或编译器约束，软件预期的顺序也可能受到影响。

### Step 4：理解实际 Review 改进

在真实 PR 中，Review 不只讨论是否增加 `SFENCE.VMA`。

还涉及：

- 是否需要使用 `Svvptc` 配置决定同步行为。
- 是否存在重复的 TLB Flush。
- `flush_tlb()` 内联汇编是否需要 `"memory"` clobber。
- 是否需要额外的内存屏障。
- 在不同硬件上的行为差异。

作者提供了不同 RISC-V 平台的测试结果，Reviewer 也提出了避免重复 Flush 等修改建议。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



这正是一个优秀 Review 的特点：

它同时考虑 Correctness、ISA Semantics、Performance 和 Maintainability。

### Step 5：从这个案例抽象工程原则

我认为可以提炼出五个经验：

1. 硬件架构约束比 QEMU 的当前行为更基础。
2. 修复必须从 Root Cause 出发，而不是仅处理 Fault 的表面现象。
3. 架构优化必须明确依赖哪些 ISA Extension。
4. Context 或 Page Table 操作必须区分本地和远程同步。
5. 一个高质量 PR 应提供测试环境、复现条件、修改原因及已知局限。

更进一步，你可以把这次修复作为一个研究起点：

检查 OP-TEE 其他 MMU 路径是否存在同类同步假设。

例如：

- Page Mapping 的创建与撤销。
- User TA 地址空间切换。
- ASID 回收。
- SMP 中跨 hart 的地址空间活动。
- Page Table Reuse。

但在实际提交之前，应先验证是否存在独立问题，避免为了制造 PR 而提交没有必要的修改。

## 22.11.2 一个可复用的 GitHub PR 工作流程

下面给出 OP-TEE 开发者可以实际使用的流程。

### 第一步：建立 Upstream 和 Fork

先在 GitHub 上 Fork `OP-TEE/optee_os`。

然后：

```
git clone git@github.com:<your-account>/optee_os.git
cd optee_os

git remote add upstream \
    https://github.com/OP-TEE/optee_os.git

git fetch upstream

git switch -c \
    riscv-mmu-fix upstream/master
```

其中 `<your-account>` 需要替换为实际 GitHub 用户名。

### 第二步：完成修改并检查 Diff

```
git diff --check

git status

git diff --stat
```

对于与 MMU 相关的改动，建议同时检查调用关系与可能影响的配置。

### 第三步：构建和测试

```
make -j"$(nproc)" \
    ARCH=riscv \
    PLATFORM=virt \
    CROSS_COMPILE64=riscv64-linux-gnu- \
    O=out-riscv
```

然后在已经集成完整固件、Linux 和 OP-TEE Test 的环境中运行对应 xtest，并补充特定问题的测试。

应记录实际使用的 OP-TEE、OpenSBI、Linux、QEMU 和 Toolchain 版本。

对于集成环境，可以参考 [RP016 Buildroot & QEMU Instructions](https://riscv-optee-docs-c9fea0.gitlab.io/rp016/build/?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### 第四步：提交 Signed-off-by Commit

```
git add <changed-files>

git commit -s
```

Commit 内容应说明 Root Cause 和测试结果。

### 第五步：检查 Patch History

```
git log --oneline upstream/master..HEAD

git diff upstream/master...HEAD --stat

git diff upstream/master...HEAD --check
```

运行 OP-TEE 本地 Checkpatch 脚本，并确认不会意外包含内部路径、调试数据或无关修改。

### 第六步：Push 和创建 PR

```
git push -u origin riscv-mmu-fix
```

随后在 GitHub 中创建针对 `OP-TEE/optee_os:master` 的 PR。

PR Description 可以采用以下结构：

```
## Problem

Describe the observed failure and the affected platforms.

## Root cause

Explain the violated architectural invariant.

## Proposed fix

Describe the minimal change and why it is sufficient.

## Compatibility

List affected architectures, configurations and interfaces.

## Testing

- Base commit:
- Toolchain:
- Platform:
- Before:
- After:
- Regression tests:
- Known limitations:

## Dependencies

List related changes in OpenSBI, Linux or optee_test.

## References

Architecture specification sections and related issues.
```

### 第七步：处理 Review

按照 OP-TEE 官方流程，在 Review 阶段追加 Fixup Commit，完成意见处理之后再统一 Squash 和整理 History。

最后重新测试合并候选版本。

不要因为 Reviewer 已经认可早期版本，就默认最后一次 Rebase/Squash 后不需要再测试。

如果你的改动位于 OpenSBI，应改用其官方要求的邮件 Patch 流程。例如使用 `git format-patch` 生成邮件 Patch Series，并配置 `git send-email`；不要把 OP-TEE 的 GitHub PR 流程机械套用到 OpenSBI。

# 22.12 Staff Engineer 级别综合面试题与参考答案

下面的题目以真实工业项目中的架构评审、移植问题、性能优化、社区维护和安全决策为背景。

每道题不仅考查你会不会写代码，还考查你能否识别系统边界、提出合理假设、设计验证方案，并对长期维护作出判断。

## Question 1 — RISC-V TLB Synchronization 的架构正确性

考查：MMU / ISA Semantics / Root Cause / Testing

场景

某 RISC-V SoC 在 QEMU 中可以正常启动 OP-TEE，但在真实 8-hart SoC 上，偶尔会在初始化 Trusted Thread Stack 时出现 Page Fault。

工程师发现，增加一次 `SFENCE.VMA` 后问题消失。

现在你作为 Maintainer，需要决定是否接受这个 Patch。

问题

你会要求开发者提供哪些证据？如何判断它是正确修复，而不是碰巧改变了执行时序？

参考分析

首先确认 Page Fault 地址、`scause`、`sepc`、`stval` 和对应 PTE 的状态。追踪 PTE 更新路径，确认是否涉及 Invalid → Valid 转换以及旧地址转换状态的观察问题。

然后分析 RISC-V 架构要求，确认在当前硬件支持的 Extension 下是否必须执行相应的地址转换同步。

进一步检查同步范围：本地 `SFENCE.VMA` 是否足够，其他 hart 是否也可能使用受影响的地址空间。

最后要求至少提供修复前后的真实硬件验证、对应的编译配置、Regression Test，以及性能影响评估。

最关键的判断是：能用 ISA 语义解释修复为什么有效，而不是仅仅证明增加一个 Fence 后不再发生 Fault。

## Question 2 — RISC-V Vector Context 的跨 TA 信息泄露

考查：Context Ownership / Confidentiality / Assembly / ABI

场景

某开发者为 RISC-V OP-TEE 增加 Vector Context Switching。

单个 TA 测试全部通过，但当 TA A 和 TA B 交替执行时，发现 TA B 有时能观察到 TA A 留下的 Vector Register 数据。

问题

你如何定位？需要检查哪些层次的 Context？怎样设计可靠修复？

参考分析

首先区分三种不同的状态所有者：

- OpenSBI Domain Context：跨 Supervisor Domain 的状态。
- OP-TEE Trusted Thread：安全内核执行上下文。
- OP-TEE User TA Context：不同 TA 的状态。

接着检查 TA 首次执行、正常切换、RPC Suspend/Resume、Trap Entry/Return 等路径。

RISC-V 的 `sstatus.VS` 与 Vector Register State 生命周期必须正确协同。状态保存既要保证功能正确，也要确保敏感状态不会跨越未经授权的执行上下文。

修复方案需要考虑不同 VLEN、运行时支持检测、内存分配、状态初始化、清理时机和错误恢复。

测试应让不同 TA 使用可区分的寄存器模式，并强制发生切换和 RPC，而不是只执行单个 Vector 算法。

优秀答案还应指出：不能简单把所有状态都归给 OpenSBI，因为 TA-to-TA 切换未必经过 OpenSBI Domain Switch。

## Question 3 — Linux、OpenSBI 和 OP-TEE 的跨版本兼容

考查：ABI / Cross-Repository Integration / Rollout

场景

你设计了一个新的 RISC-V TEE Transport。

它需要：

- 修改 Linux Driver。
- 增加 OpenSBI SBI MPXY/RPMI 支持。
- 修改 OP-TEE 的请求处理路径。

某厂商要求允许用户只升级 Linux Kernel，不同时升级 OpenSBI 或 OP-TEE。

问题

你怎样设计接口，避免系统无法启动或 TEE 完全不可用？

参考分析

应首先定义最小兼容版本和支持的升级组合。

通过 Capability Discovery 判断固件是否支持新 Transport，而不能因为 Linux Driver 编译时启用了该 Feature 就直接调用。

在协议设计中，应明确 Request Version、返回值、Unsupported Feature 行为，以及是否支持旧 Transport 回退。

如果不能安全回退，应该明确报告不兼容，而不是静默改用一个权限较弱或行为不明确的通道。

三个项目的 Patch Series 应分别提交，各自保持可构建，并通过兼容矩阵验证。

此题的核心不是完成新版 Linux + 新版 OpenSBI + 新版 OP-TEE 的 Happy Path，而是设计可控的升级和失败机制。

## Question 4 — PMP 已保护 TEE RAM，为什么 DMA 仍能读取？

考查：Threat Modeling / System Security / SoC Architecture

场景

某 RISC-V SoC 使用 PMP 保护 OP-TEE Physical Memory。

Linux 上运行恶意程序后，攻击者利用具有 DMA 能力的设备，将 OP-TEE RAM 内容复制到了 REE Buffer。

平台团队认为是 OP-TEE 的漏洞，因为 OP-TEE 没有正确配置 PMP。

问题

你会如何分析责任边界？如何设计系统级修复？

参考分析

首先确认 PMP 的保护对象。标准 PMP 主要约束相关 hart 的内存访问，并不自动保护所有外部 DMA Master。

因此必须检查 SoC 是否具备 IOPMP、IOMMU、WorldGuard 或其他 Firewall，以及设备访问权限是否正确初始化。

还要检查设备生命周期：Boot、Reset、Power Management 和 Domain Switch 过程中是否存在短暂的权限空窗。

修复可能属于 Platform Firmware、Hardware Isolation Driver 或 SoC Security Policy，而不一定是 OP-TEE Core 的 MMU Bug。

如果问题可以由 REE 控制的设备持续触发，那么其影响显然跨越了 TEE 隔离边界。

Staff Engineer 应推动跨硬件、Firmware 和 OS 团队进行 Root Cause 分析，而不能只在 OP-TEE 中增加无效的地址检查。

## Question 5 — 一个 2000 行的 RISC-V SMP Feature 应如何 Review？

考查：Patch Architecture / Code Review / Community Collaboration

场景

开发者提交一个 2000 行的 PR，声称：

> Add full RISC-V SMP support to OP-TEE.

其中包含 Thread Management、IPI、TLB Shootdown、Lock、Boot Sequence 和 Device Tree 修改。

问题

你作为 Reviewer，是否直接开始逐行审查？如何组织整个 Review？

参考分析

不应该立即进入逐行 Review，而应先确认设计 Scope、依赖和责任边界。

可以要求开发者将改动组织为以下逻辑单元：

1. Per-Hart 数据结构和初始化。
2. Secondary Hart Boot。
3. IPI / Synchronization。
4. TLB Shootdown。
5. Trusted Thread SMP Integration。
6. Device Tree 与 Platform Support。
7. Regression Tests。

对每个单元分别确定锁规则、数据所有权和测试方法。

尤其需要分析 Lock Ordering、Remote Hart Completion、Page Table 生命周期、Interrupt Enabled/Disabled 状态和 Error Handling。

最后确认各阶段是否能够独立编译、验证和合入。

能够将复杂 Feature 拆成可审查架构，是 Maintainer 的核心能力之一。

## Question 6 — Common MMU Refactoring 是否值得合并？

考查：Architecture Abstraction / Maintainability / Cross-Architecture

场景

某工程师发现 Arm 和 RISC-V 都存在 `core_mmu_*` 接口，于是提出将两个架构的大部分 MMU 代码统一到 Common Layer。

修改后减少约 1000 行重复代码，但增加了 25 个 Architecture Callback。

问题

你是否支持这个 Refactoring？

参考分析

不能只根据删除的代码行数作出决定。

首先检查两个架构是否拥有真正相同的 Policy 和 Lifetime，而不仅仅是函数名称相似。

其次评估 Callback 是否导致语义碎片化。例如，一个 API 叫 `flush_tlb()`，却在不同架构上拥有不同的局部与远程同步保证，就可能构成错误抽象。

需要检查新设计是否把架构特殊行为暴露到 Common Code 中，是否降低测试可控性，是否增加调用路径复杂度。

如果没有清晰的架构收益和充分的跨架构回归证据，可以要求缩小 Scope，先提取真正共通的 Policy。

正确的目标是减少概念复杂度，而不是单纯减少代码行数。

## Question 7 — 发现严重漏洞，但问题来自公司内部 SoC

考查：Security Governance / Responsible Disclosure / Professional Judgment

场景

你在公司新 SoC 的 OP-TEE 移植中发现一个可由 REE 触发的安全问题。

问题一部分来自 OP-TEE Common Core，另一部分来自公司私有的 Hardware Firewall 配置。

公司希望暂时不要公开，而你认为应该尽快提交 upstream。

问题

你应该怎么做？

参考分析

首先区分可公开的通用缺陷与可能涉及 NDA 的平台细节，评估受影响的软件版本和产品范围。

不应未经授权公开公司私有的地址映射、硬件配置、漏洞细节或验证日志。

但也不应把涉及通用 OP-TEE 的安全问题无限期隐藏。

应启动公司内部安全事件流程，与法律/安全负责人确认披露边界，同时通过 OP-TEE 官方私密漏洞报告渠道与项目安全团队协调。

需要准备最小技术说明、根本原因、影响范围和修复思路，并讨论协调披露及回归测试策略。

这里考查的是能否同时承担技术正确性、用户安全、组织合规及社区责任。

## Question 8 — RPC 路径偶发 Deadlock

考查：Concurrency / Locking / RPC / Debugging

场景

在 16-hart RISC-V SoC 上运行并发 xtest 时，系统偶尔卡死。

调试发现：

- Hart 0 正在执行 OP-TEE Trusted Thread。
- Hart 1 等待某个全局锁。
- Hart 0 发起 RPC，需要 Linux 侧完成操作后恢复。
- 系统最终无法继续。

问题

你会从哪些方面分析？怎样证明修复正确？

参考分析

首先建立 Wait-for Graph，确定每个 hart、Trusted Thread 和 Linux Task 正在等待什么资源。

检查发起 RPC 时是否持有不应该跨越 RPC/Yield 边界的锁。

然后分析 OP-TEE Thread Suspend/Resume 与资源所有权是否一致，Linux 返回路径是否依赖某个被当前安全侧线程持有的锁。

如果存在嵌套调用或跨 Domain 重入，还需要检查锁的作用范围和递归条件。

修复通常应该消除循环等待或调整资源生命周期，而不是通过延长 Timeout 掩盖死锁。

测试应控制并发交错，记录锁的获取/释放顺序，并覆盖取消、异常和 RPC Failure 情况。

## Question 9 — 一个功能修复可能影响整个 TEE 安全模型

考查：Security Invariant / Shared Memory / API Review

场景

某开发者发现 OP-TEE Crypto RPC 每次都会复制输入 Buffer，存在较大性能开销。

于是提出 Zero-copy 优化，让 OP-TEE 直接从 REE Shared Memory 中读取输入数据。

性能提升 25%。

问题

你如何决定是否接受这个 Feature？

参考分析

首先分析 Buffer 所有权与可信性。

REE 控制的共享内存通常应视为非可信输入，即使 OP-TEE 已经映射，也不代表内容在调用期间不变。

因此要检查 Length、Offset、Address 和实际数据内容是否可能在检查之后发生变化。

如果 Crypto Operation 对输入完整性有要求，还必须分析 Double Fetch、TOCTOU 和 Buffer Lifecycle。

需要明确哪些数据允许 Zero-copy，哪些必须先复制到可信内存；不能为了减少一次拷贝而改变原来的安全承诺。

最后通过可复现 Benchmark 测量收益，并建立负向测试和威胁模型。

性能提升不能成为跨越安全边界的理由。

## Question 10 — 你已经有很多 PR，为什么仍未成为 Maintainer？

考查：Community Leadership / Long-Term Ownership

场景

某工程师在一年内向 OP-TEE 提交了 40 个 PR，其中 35 个已经合并。

他认为自己理应成为 RISC-V Maintainer。

但目前社区仍没有授予他该职责。

问题

你怎样评价他的贡献？还应该具备哪些能力？

参考分析

PR 数量只能反映部分贡献，不能充分衡量长期维护能力。

需要进一步检查：

- PR 是否涉及具有架构价值的改动。
- 是否能独立完成复杂 Root Cause Analysis。
- 是否长期参与其他人的 Review。
- 是否能够识别安全和兼容性风险。
- 是否持续维护自己提交的功能。
- 是否能够参与 RFC 并推动跨团队共识。
- 是否愿意处理 Bug、Regression 和 Release Compatibility。

Maintainer 的职责不仅是开发新功能，更是维护代码库的长期健康。

因此真正值得追求的是让社区自然地信任你对某个领域的技术判断，而不仅是累计提交数量。

## 本章总结

Chapter 22 希望建立的核心认识是：

Upstream Engineering 是系统架构能力、工程正确性、安全责任和社区协作能力的综合体现。

对于你的 RISC-V OP-TEE 方向，我建议将长期实践收敛在一条路径上：

深入一个 Core Subsystem

例如 RISC-V MMU / Context Switch

发现并验证 Architecture Invariant

Root Cause + ISA Reasoning + Tests

持续提交高质量 Upstream Patch

Bugfix + Regression + Documentation

参与他人的 Review 和 Architecture RFC

Cross-Architecture + Security + Compatibility

建立长期维护责任和社区信任

Subsystem Ownership → Maintainer Candidate

这也是整套 22 章课程的最终落点：不仅掌握 RISC-V 上的 OP-TEE 实现，还能为该架构的安全性、正确性和长期发展持续贡献高质量工程成果。
