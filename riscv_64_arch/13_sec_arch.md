下面这份我会把第 13 阶段当成一份“系统安全架构课程”来讲，而不是某个项目的说明书。主线是：**Threat Model → RoT → M-mode → PMP/ePMP → Enclave → Attestation → DMA/IOMMU → CoVE → Confidential Computing**。


# 13. RISC-V Security / TEE

## 0. 先建立最重要的结论

对于有 Arm TrustZone 经验的人，学习 RISC-V Security 时最容易犯的错误是：

```text
Arm:
EL3 + Secure World + OP-TEE
          ↓
寻找
          ↓
RISC-V:
M-mode + ??? = TrustZone
```

**这个映射不存在。**

RISC-V 标准 Privileged Architecture 定义的是：

```text
M
↓
S
↓
U
```

本质上是一套 **privilege hierarchy**。

而 Arm TrustZone 定义的是另一维：

```text
Security State
    |
    +-- Secure
    |
    +-- Non-secure
```

因此：

```text
M-mode ≠ Secure World
S-mode ≠ Normal World
```

基础 RISC-V Privileged Architecture 并没有定义一个全平台统一传播的 `Secure / Non-secure` world bit。

这决定了 RISC-V TEE 的整体设计思想：

> **不是先定义一个 Secure World，然后把 Trusted OS 放进去；而是先提供一系列隔离 primitive，由平台根据自己的 threat model 组合出 TEE。**

今天理解 RISC-V Security，建议始终抓住下面这张图：

```text
                         Security
                            |
       +--------------------+---------------------+
       |                    |                     |
      RoT              Isolation              Attestation
       |                    |                     |
 Secure Boot             PMP/ePMP             Measurement
 Measured Boot           MMU/H-ext                |
 Key hierarchy              |                     |
       |                     |                     |
       +-----------> Security Monitor <------------+
                          |
                   Trusted workload
                          |
             +------------+------------+
             |                         |
          Enclave                     TVM
        Keystone-like                 CoVE
```

RISC-V 目前的安全架构也越来越明确地使用 **domain** 作为基本抽象，而不是强制整个系统只有 Secure/Non-secure 两个 world。2026 年的 RISC-V Security Model 就以 isolated security domains 作为通用参考模型。:chatgpt-content-reference{index="0"}

---

# 1. Security / TEE 到底解决什么问题？

TEE 最重要的问题并不是：

> “怎样让程序运行在高 privilege？”

而是：

> **当系统中某些高权限软件不可信时，怎样继续保护特定代码和数据？**

例如：

```text
Application / Enclave
       |
       |
Linux Kernel
       |
Hypervisor
       |
Firmware
       |
Hardware
```

传统 OS 安全模型：

```text
App
 |
 | kernel provides isolation
 v
Linux
```

隐含前提是：

```text
Linux = trusted
```

但 TEE 的 threat model 可以变成：

```text
Linux root compromised
        ↓
Linux = attacker
```

我们仍希望：

```text
Linux
  X
  |
  | cannot read
  v
TEE private memory
```

因此 TEE 真正关心的是：

```text
Confidentiality
Integrity
Isolation
Attestation
```

其中最重要的第一步是：

> **谁拥有最终的 physical memory access control？**

---

# 2. 为什么 MMU 本身无法构建 TEE？

假设：

```text
User Enclave
0x40000000
```

Linux 可以给它建立：

```text
satp
 ↓
Page Table
 ↓
Enclave memory
```

问题是：

```text
Page Table 本身由 Linux 管理
```

如果 Linux 已经被攻破：

```text
Linux
 |
 | modify PTE
 v
map enclave physical pages
 |
 v
read secret
```

所以：

```text
MMU
```

只能解决：

> **OS 为普通应用提供 isolation**

不能解决：

> **从恶意 OS 手里保护应用。**

于是需要一层：

```text
        M-mode
           |
          PMP
           |
           X
           |
        S-mode Linux
```

这就是 RISC-V TEE 最基础的硬件 primitive。

---

# 3. PMP：RISC-V TEE 最基础的安全 primitive

你已经学过第五阶段 PMP，这里从 **security architecture** 角度重新理解一次。

PMP：

```text
Physical Memory Protection
```

由 M-mode 配置：

```text
pmpcfgX
pmpaddrX
```

决定：

```text
physical address range
        +
R/W/X permissions
```

基线 PMP 是 **per-hart** 的物理地址访问控制机制，主要限制低于 M-mode 的访问。:chatgpt-content-reference{index="1"}

例如：

```text
DRAM

0x80000000
   |
   | Linux
   |
0x88000000
   |
   | Enclave
   |
0x89000000
   |
   | Linux
   |
0xC0000000
```

M-mode Security Monitor：

```text
PMP entry 0:

0x88000000 - 0x89000000
S-mode: ---
```

那么：

```text
Linux S-mode

lw t0, enclave_address
          |
          v
       PMP check
          |
          X
     Access Fault
```

即使 Linux：

```text
root
kernel exploit
arbitrary kernel code execution
```

也没有用。

因为：

```text
Linux privilege = S

PMP owner = M
```

这是一个非常关键的 security hierarchy：

```text
S-mode OS
   |
   | cannot modify
   v
PMP
   |
   | configured by
   v
M-mode Security Monitor
```

---

# 4. PMP 和 Arm TrustZone 最大的区别

这部分非常重要。

Arm TrustZone 大致是：

```text
CPU transaction
     |
     | NS attribute
     v
Interconnect
     |
     +--> DRAM controller
     |
     +--> peripheral
     |
     +--> DMA
```

安全属性是一个 **system-level security property**。

而基础 PMP 更像：

```text
Hart
 |
 | load/store/fetch
 v
PMP
 |
 v
Physical memory
```

所以：

> **PMP 本质上首先是 CPU/Hart physical-access protection，而不是天然覆盖整个 SoC 的 security state。**

这意味着：

```text
CPU
 |
 PMP
 |
 enclave RAM
```

即使 CPU 被限制了：

```text
DMA device
     |
     +------------------> enclave RAM
```

仍然可能成为攻击路径。

因此完整 TEE 必须考虑：

```text
CPU isolation
+
DMA isolation
+
interrupt isolation
+
device isolation
+
debug isolation
+
boot chain
+
attestation
```

而不是只配置几个 PMP entry。

这也是为什么 RISC-V Security 最终必然需要：

```text
PMP/ePMP
IOMMU
AIA
RoT
Security Monitor
Platform security
```

一起工作。

---

# 5. PMP 的另一个核心问题：M-mode 本身怎么办？

这里开始进入比较深的 security architecture。

基础 PMP 下，可以粗略理解：

```text
             PMP
              |
         +----+----+
         |         |
       S/U         M
      checked    normally privileged
```

因此很容易形成这样的 TCB：

```text
Hardware
   |
M-mode firmware
   |
Security Monitor
   |
Enclave
```

那么：

> **如果 M-mode firmware 被攻破怎么办？**

很多早期 PMP-based TEE 的回答其实是：

```text
M-mode Security Monitor
        =
       TCB
```

也就是：

> 我们假设 Security Monitor 不会被攻破。

例如 Keystone 明确把 M-mode Security Monitor 放进 TCB；它负责 PMP memory isolation、remote attestation 和 enclave thread management。:chatgpt-content-reference{index="2"}

这和 OP-TEE 非常类似：

```text
OP-TEE compromise
     ↓
TA security lost
```

只是两者架构完全不同。

---

# 6. Smepmp / ePMP：进一步强化 M-mode security

后来 RISC-V 增加了：

```text
Smepmp
```

即：

```text
PMP Enhancements for
memory access and execution prevention
in Machine mode
```

目前 Smepmp v1.0 已经进入 Ratified specification。:chatgpt-content-reference{index="3"}

它新增一个非常重要的 CSR：

```text
mseccfg
```

核心字段：

```text
RLB
MMWP
MML
```

可以这样记。

### MMWP

```text
Machine Mode Whitelist Policy
```

准确名称为 Machine-Mode Allowlist Policy。

普通思维：

```text
没有 PMP entry match
       ↓
M-mode allow
```

开启 MMWP 后：

```text
没有 PMP entry match
       ↓
DENY
```

也就是：

> **M-mode 从 default allow 变成 default deny。**

---

### MML

```text
Machine Mode Lockdown
```

它进一步改变 PMP 权限语义。

Smepmp 的一个核心目标就是允许出现：

```text
        Memory Region

M-mode       X
S/U-mode     R/W
```

即：

> **某些内存甚至不给 M-mode 普通软件访问。**

这对于构建更严格的安全边界非常重要。Smepmp 的规范明确指出，没有这个能力时，对没有 Locked rule 保护的区域，M-mode 拥有非常强的访问能力。:chatgpt-content-reference{index="4"}

---

### RLB

```text
Rule Locking Bypass
```

主要用于：

```text
boot / debug / initialization
```

允许暂时调整 locked rule。

正常运行以后应尽量关闭，否则会削弱 lockdown。

---

因此可以形成：

```text
baseline PMP:

M-mode
  |
  | controls
  v
S/U access


Smepmp:

        Security policy
              |
       +------+------+
       |             |
    M-mode          S/U
       |             |
       +--- PMP -----+
```

PMP 从：

> “M-mode 管别人”

逐渐变成：

> **“M-mode 建立整个系统的物理访问 policy，其中某些 policy 甚至约束 M-mode 自己。”**

这是 security 架构上的重要进步。

---

# 7. 最经典的 RISC-V TEE：Keystone

理解 RISC-V TEE，非常建议把 Keystone 当作第一个 reference architecture。

它可以抽象成：

```text
               Hardware / RoT
                     |
                     v
              M-mode Security
                  Monitor
                     |
              +------+------+
              |             |
              |             |
           Linux          Enclave
          S-mode           U-mode
```

注意一个非常有意思的事情：

```text
Enclave privilege = U-mode
```

它甚至比 Linux：

```text
S-mode
```

权限低。

但：

```text
Privilege level
       ≠
Security privilege
```

因为真正决定安全性的不是：

```text
U < S
```

而是：

```text
Linux
 |
 | PMP DENY
 v
Enclave physical memory
```

这就是理解 TEE 很重要的一个思想：

> **TEE 的安全不是来源于 enclave privilege 更高，而是来源于更高 privilege 的 security monitor 强制建立 isolation policy。**

---

# 8. Keystone 是怎样创建 enclave 的？

假设 Linux 想创建：

```text
Enclave A
```

物理内存：

```text
0x90000000
    |
    | Enclave
    |
0x91000000
```

初始状态：

```text
M-mode Security Monitor
        |
        | PMP
        v

Linux:

0x80000000 =============================
0x90000000 =============================
0x91000000 =============================
```

OS 一开始拥有普通 DRAM。

创建 enclave 后，Security Monitor 设置：

```text
PMP:

SM memory:
----------------

Enclave:
        ----------------

OS memory:
========================================
```

由于 PMP entry priority：

```text
OS
 |
 | access enclave
 v
PMP
 |
 X
```

Keystone 官方设计就是使用 PMP entry 建立 enclave region，并在 enclave enter/exit 时切换相关 PMP permission。:chatgpt-content-reference{index="5"}

---

# 9. Enclave enter 到底发生什么？

这是非常值得你从 kernel engineer 角度理解的一段。

Linux：

```text
S-mode
```

调用：

```text
ecall
```

进入：

```text
M-mode Security Monitor
```

概念流程：

```text
Linux S-mode
     |
     | ECALL
     v
M-mode Security Monitor
     |
     +-- save host context
     |
     +-- restore enclave context
     |
     +-- reconfigure PMP
     |
     +-- flush translation state
     |
     +-- switch execution context
     |
     v
Enclave U-mode
```

可以把核心逻辑想象成：

```c
enter_enclave(eid)
{
    save_host_context();

    deny_host_memory();
    allow_enclave_memory();
    allow_shared_memory();

    restore_enclave_context();

    enter_enclave();
}
```

反过来：

```text
Enclave
   |
  ECALL / trap
   |
   v
Security Monitor
   |
   +-- save enclave
   +-- restore host
   +-- restore PMP
   |
   v
Linux
```

Keystone Security Monitor 的正式流程包含 context save/restore、PMP permission 切换以及 TLB flush。:chatgpt-content-reference{index="6"}

如果你熟悉 OP-TEE，可以进行**概念级**类比：

```text
Arm TrustZone

Normal World
    |
    | SMC
    v
Secure Monitor
    |
    v
Secure World
```

Keystone：

```text
Linux S
   |
   | ECALL
   v
M-mode SM
   |
   v
Enclave U
```

但一定要记住：

```text
机制完全不同
```

TrustZone 是 security state transition。

Keystone 是：

```text
privilege transition
+
PMP policy transition
```

---

# 10. Linux 已经 root 以后，Keystone 能保护什么？

假设：

```text
Linux completely compromised
```

攻击者可以：

```text
modify Linux kernel
modify page table
schedule enclave arbitrarily
forge syscall
control host memory
control normal devices
```

但是只要：

```text
Security Monitor
PMP
RoT
```

没有被攻破，那么 Linux：

```text
cannot directly read enclave private memory
cannot directly modify enclave private memory
```

Keystone 特别强调，即使 S-mode OS 能修改 `satp` 和自己的 page tables，enclave private physical memory 仍由 PMP 隔离；enclave 自己受保护的 page table 也不能由 OS 随意篡改。:chatgpt-content-reference{index="7"}

但是攻击者仍然可以：

```text
DoS enclave

delay scheduling

fake I/O

manipulate shared buffer

observe some timing behavior

attack cache / microarchitecture

deny network

deny storage
```

所以：

> **TEE 一般提供 confidentiality / integrity，而不保证 availability。**

这是几乎所有 enclave/Confidential Computing 架构都存在的重要边界。

---

# 11. Shared Memory 为什么必须被当成 untrusted？

TEE 不可能完全不和外界通信。

例如：

```text
Enclave
   |
   | network request
   v
Linux driver
   |
   v
NIC
```

Linux 又是不可信的。

所以典型结构：

```text
Private Memory
     |
 Enclave
     |
     +--------+
              |
        Shared Buffer
              |
              v
           Linux
```

Security Monitor 配置：

```text
Private:

Enclave    RWX
Linux      ---

Shared:

Enclave    RW
Linux      RW
```

于是：

```text
shared buffer
```

**绝不能被当作 trusted data。**

Enclave 必须假设：

```text
Linux can modify it at any time
```

所以需要：

```text
bounds checking
protocol validation
cryptographic validation
TOCTOU protection
```

这个设计与 OP-TEE：

```text
REE shared memory
```

非常相似。

---

# 12. TEE 的第二根支柱：Root of Trust

只有 PMP 仍然不够。

因为攻击者可能在启动时直接替换：

```text
Security Monitor
```

例如：

```text
malicious SM:

configure PMP
then read enclave memory
```

所以必须有：

```text
Root of Trust
```

典型启动链：

```text
Immutable BootROM
       |
       | verify / measure
       v
M-mode firmware
       |
       | verify / measure
       v
Security Monitor
       |
       | measure
       v
Enclave
```

这对应你熟悉的：

```text
Secure Boot
+
Measured Boot
```

TEE 安全其实有两个维度：

```text
Isolation

“别人不能访问我”
```

和：

```text
Identity

“我到底运行的是谁？”
```

PMP 解决第一个。

Measurement + Attestation 解决第二个。

---

# 13. Remote Attestation

假设云服务器里运行：

```text
Payment Enclave
```

银行服务器不能因为客户端说：

```text
“I am a secure enclave”
```

就相信它。

因此：

```text
Bank
 |
 | nonce N
 v
Enclave
 |
 | request attestation
 v
Security Monitor / RoT
```

建立：

```text
measurement =
HASH(
    enclave code
    + initial data
    + configuration
)
```

然后：

```text
Evidence = {
    measurement,
    nonce,
    platform identity,
    security configuration
}
```

最后：

```text
Sign(
    Attestation_Private_Key,
    Evidence
)
```

返回：

```text
Evidence
Signature
Certificate Chain
```

服务器验证：

```text
Trusted Root CA
      |
      v
Platform / Attestation certificate
      |
      v
Signature
      |
      v
Measurement
      |
      v
Expected enclave
```

这和你前面学习 TPM Attestation 时建立的 mental model，本质上完全一致：

```text
RoT
  ↓
Measurement
  ↓
Evidence
  ↓
Attestation Key
  ↓
Certificate Chain
  ↓
Remote Verifier
```

Keystone 也采用这个模型：RoT 首先度量 Security Monitor，并建立用于 remote attestation 的信任链；Security Monitor 再提供 enclave measurement 和 attestation。:chatgpt-content-reference{index="8"}

---

# 14. Secure Boot 和 Attestation 的关系

这里建议你形成一个非常简单的模型：

```text
Secure Boot:

Can I execute it?
```

而：

```text
Measured Boot:

What did I execute?
```

而：

```text
Attestation:

Can I prove to someone else
what I executed?
```

所以三者：

```text
Secure Boot
Measured Boot
Attestation
```

并不冲突。

一个完整 TEE 平台通常是：

```text
Secure Boot
      ↓
trusted Security Monitor
      ↓
Measurement
      ↓
Attestation
```

---

# 15. PMP-based Enclave 的可扩展性问题

PMP 非常漂亮，但存在一个现实问题：

```text
PMP entries
```

数量有限。

假设：

```text
PMP entry 0: monitor
PMP entry 1: enclave A
PMP entry 2: enclave B
PMP entry 3: enclave C
...
```

当你想运行：

```text
1000 confidential VMs
```

这种方案显然不现实。

而且 PMP 是 region-oriented：

```text
TOR
NAPOT
```

并不是为了大规模管理：

```text
millions of 4K pages
```

设计的。

RISC-V 当前 Supervisor Domain Access Protection 工作也明确指出：PMP entry 数量有限，并且频繁动态重编程 PMP 的复杂性和开销，使单纯 PMP 更适合相对简单、静态的资源隔离。:chatgpt-content-reference{index="9"}

于是安全架构开始从：

```text
region-based PMP
```

向：

```text
page-based ownership
```

演进。

---

# 16. SmMTT / Supervisor Domain：为什么非常重要？

这一步开始接近：

```text
Arm CCA
Intel TDX
AMD SEV-SNP
```

要解决的规模问题。

一个更 scalable 的模型是：

```text
Physical Page
     |
     | belongs to
     v
Supervisor Domain ID
```

例如：

```text
Page 0x10000 → Domain 0
Page 0x10001 → Domain 2
Page 0x10002 → Domain 2
Page 0x10003 → Domain 5
```

而不是：

```text
PMP region 1
PMP region 2
PMP region 3
```

当前 RISC-V 的 Supervisor Domain Access Protection / SmMTT 工作就是朝这个方向发展：由 M-mode 管理 page-based physical-memory protection，并给 supervisor domain 分配 domain identity。其工作规范定义了 Machine-mode Memory Protection Table，并包含 `SDID` 和 root protection-table pointer 等机制。:chatgpt-content-reference{index="10"}

概念上：

```text
Hart
 |
 | SDID = 3
 v
Memory access
 |
 v
Memory Protection Table
 |
 | page belongs to Domain 3?
 |
 +-- yes → allow
 |
 +-- no  → deny
```

这就比 PMP：

```text
16 / 32 / 64 entries
```

更适合：

```text
Cloud
VM
Confidential Computing
large dynamic memory
```

---

# 17. 这时候就进入 CoVE

CoVE：

```text
Confidential VM Extension
```

目标不是简单的：

```text
small user enclave
```

而是：

```text
Trusted VM / Confidential VM
```

也就是：

```text
       Untrusted Host

Linux Host
KVM
VMM
  |
  X
  |
  v
+----------------+
|      TVM       |
|                |
| Guest Linux    |
| Applications   |
+----------------+
```

这和：

```text
AMD SEV-SNP
Intel TDX
Arm CCA Realm
```

属于同一类问题。

RISC-V AP-TEE 工作组正在定义 CoVE 的 ABI 和 programming model。不过需要特别注意时间状态：**截至 2026 年 10 月，其公开 AP-TEE repository 仍明确标注 specification 尚未 ratify，内容仍可能变化。** :chatgpt-content-reference{index="11"}

所以现阶段应该：

> 理解 CoVE architecture，而不是死背当前 ABI number。

---

# 18. CoVE 的核心人物：TSM

CoVE 引入：

```text
TEE Security Manager
        TSM
```

把它理解为：

> **Confidential Computing security policy owner**

而普通 Host：

```text
Linux + KVM
```

不再属于 TVM 的 TCB。

抽象结构：

```text
                     Hardware RoT
                         |
                         v
                  Security mechanism
                         |
                   +-----+-----+
                   |           |
                  TSM       Host/VMM
                   |         untrusted
                   |
                   v
              +----------+
              |   TVM    |
              |          |
              | Guest OS |
              +----------+
```

Host 负责：

```text
resource allocation
scheduling
normal virtualization
```

TSM 负责：

```text
security policy
private-memory ownership
TVM lifecycle security
measurement
attestation
```

这就是 Confidential Computing 一个非常核心的设计思想：

> **Resource management 可以交给 untrusted host，但 security ownership 不能交给 host。**

---

# 19. CoVE 为什么更像 Arm CCA，而不是 TrustZone？

这是非常重要的一组映射。

传统 Arm TrustZone：

```text
Normal World
     |
 Secure Monitor
     |
Secure World
```

比较适合：

```text
TEE OS
TA
payment
DRM
key storage
```

而 Arm CCA：

```text
Normal World
     |
     X
     |
 Realm
```

目标是：

```text
保护一个完整 confidential VM
```

CoVE：

```text
Host
 |
 X
 |
TVM
```

所以：

```text
RISC-V CoVE
```

在设计目标上更接近：

```text
Arm CCA
Intel TDX
AMD SEV-SNP
```

而：

```text
Keystone
```

则更容易让人联想到：

```text
SGX-style enclave
```

当然这只是帮助建立 mental model，并非机制一一对应。

---

# 20. 一个非常重要的问题：TSM 放在哪里？

RISC-V 没有强迫唯一实现。

例如当前 CoVE 工作就讨论过：

```text
M-mode TSM
```

这样的部署。

其中一种模型：

```text
             M-mode
+-----------------------------+
|                             |
| TSM + TSM Driver            |
|                             |
+-----------------------------+
              |
             PMP
              |
      +-------+--------+
      |                |
   Host domain       TVMs
```

当前 CoVE 工作规范中的 M-mode TSM deployment 就描述了使用硬件内存隔离机制，例如 PMP，把 hosting supervisor domain 与 confidential supervisor domain 隔开；TSM 再利用 G-stage translation 隔离不同 TVM。:chatgpt-content-reference{index="12"}

这体现了 RISC-V 非常典型的设计理念：

> **标准定义 security contract / interface，而不是强制全行业只有一种微架构。**

---

# 21. H Extension 在这里重新出现

你学第 10 阶段 H Extension 时已经掌握：

```text
VS
VU
HS
```

和：

```text
vsatp
hgatp
```

现在终于能看到它的 security 用途。

普通 virtualization：

```text
Guest VA
   |
 vsatp
   v
Guest PA
   |
 hgatp
   v
Host PA
```

普通 hypervisor 控制：

```text
hgatp
```

意味着：

```text
Hypervisor owns guest physical memory mapping
```

但是 Confidential Computing 要求：

```text
Host Hypervisor
        X
        |
        v
Confidential Guest memory
```

因此必须再引入：

```text
trusted control plane
```

即：

```text
TSM
```

由 TSM 掌握 confidential page 的安全 ownership。

于是你会发现：

> H Extension 解决 virtualization；  
> CoVE 解决 **host 本身不可信时的 virtualization security**。

这两层完全不同。

---

# 22. 为什么 IOMMU 在 TEE 中是核心安全模块？

假设：

```text
CPU
 |
 PMP / page protection
 |
 v
TVM memory
```

CPU 已经保护好了。

但是：

```text
NIC DMA
   |
   +-------------> TVM memory
```

如果 Host 控制 NIC：

```text
Host
 |
 | program DMA address
 v
NIC
 |
 v
read TVM secrets
```

整个 TEE 就失效了。

所以：

```text
CPU page isolation
```

必须配套：

```text
DMA isolation
```

也就是你第 12 阶段刚学过的：

```text
IOMMU
```

真正安全架构应该是：

```text
CPU
 |
 | CPU access control
 v
+--------------------+
| Confidential page  |
+--------------------+
 ^
 |
 | DMA access control
 |
IOMMU
 ^
 |
Device
```

这也是为什么：

> **TEE 和 IOMMU 从 architecture 角度实际上是强耦合的。**

---

# 23. Trusted I/O 为什么比 memory isolation 更难？

如果 TVM 需要 GPU：

```text
TVM
 |
GPU
```

你不能简单说：

```text
IOMMU allow GPU → TVM RAM
```

因为：

> GPU 自己可信么？

攻击者可能替换设备 firmware。

所以完整问题变成：

```text
Who is this device?
        ↓
Is device firmware trusted?
        ↓
Is PCIe path protected?
        ↓
Is DMA restricted?
        ↓
Can device enter TVM TCB?
```

于是 Confidential I/O 会引入：

```text
Device Attestation
SPDM
IOMMU
PCIe IDE
TDISP
```

当前 CoVE-IO 工作甚至明确设计了由 TSM 建立 SPDM session、配置 PCIe IDE，并控制可信设备与 TVM 的绑定；同时要求 TSM 获得相应 IOMMU 安全控制能力。:chatgpt-content-reference{index="13"}

这说明完整的 TEE security boundary 已经从：

```text
CPU + RAM
```

扩展为：

```text
CPU
RAM
IOMMU
Device
Interconnect
Interrupt
Firmware
```

---

# 24. Interrupt 也是攻击面

考虑：

```text
TVM
 |
 | interrupt
 v
Host
```

如果 Host 可以任意：

```text
inject interrupt
suppress interrupt
reroute interrupt
```

它至少可以：

```text
DoS
timing manipulation
state confusion
```

所以 Confidential VM architecture 也必须考虑：

```text
interrupt ownership
interrupt routing
interrupt injection
```

这就是为什么你前面学的：

```text
AIA
IMSIC
```

又会重新出现。

你现在应该开始看到我们第 9～13 阶段为什么是连起来的：

```text
SMP
 |
H Extension
 |
AIA
 |
IOMMU
 |
Security/TEE
```

它们共同组成一个现代 application processor 的系统架构。

---

# 25. Crypto Extension 在 TEE 中扮演什么角色？

RISC-V 还定义了：

```text
K / Zk*
```

cryptography extensions。

以及：

```text
seed CSR
```

entropy-source interface。

还有：

```text
Zkt
```

data-independent execution-latency property。

官方 scalar crypto specification 明确规定了硬件 entropy source interface，并通过 Zkt 对一组指令给出 data-independent execution latency contract，以减少直接 timing leakage。:chatgpt-content-reference{index="14"}

但这里一定要分清：

```text
Crypto extension
         ≠
TEE
```

AES/SHA 指令：

```text
protect cryptographic algorithm
```

PMP：

```text
protect memory isolation
```

Attestation：

```text
establish remote identity
```

它们是不同安全问题。

完整系统只是把它们组合起来。

---

# 26. Side Channel：TEE 无法靠 privilege mode 自动解决

假设：

```text
Linux cannot read enclave memory
```

Linux 仍可能观察：

```text
cache
branch predictor
TLB
timing
memory contention
interrupt timing
```

于是：

```text
Secret
 |
 v
microarchitecture state
 |
 v
attacker observation
```

产生：

```text
side channel
```

因此 TEE threat model 必须明确：

```text
Do we protect against cache side channel?

Do we protect against physical attack?

Do we protect against DRAM snooping?

Do we protect against speculative execution?

Do we protect against power analysis?
```

没有任何一句：

```text
“支持 TEE”
```

可以自动回答这些问题。

Security engineering 最重要的习惯其实就是：

> **永远先问 threat model。**

---

# 27. Debug 为什么也是 TEE 安全的一部分？

假设：

```text
TEE memory protected
```

但：

```text
JTAG
 |
 halt CPU
 |
 read registers
 |
 read physical memory
```

那么前面所有设计都没有意义。

所以 production device 通常需要：

```text
debug authentication
debug disable
lifecycle state
secure debug
```

这也是为什么 RISC-V 当前 Security Model 会把 external/debug control 纳入 TEE/security requirements，而不是把它简单视为开发工具。:chatgpt-content-reference{index="15"}

---

# 28. 完整 RISC-V TEE 应该怎样理解？

现在我们可以把所有内容放到一张图里：

```text
                  ┌──────────────────┐
                  │ Remote Verifier  │
                  └────────▲─────────┘
                           │
                       Attestation
                           │
             ┌─────────────┴────────────┐
             │       Hardware RoT       │
             │ keys / identity / boot   │
             └─────────────┬────────────┘
                           │
                      Secure Boot
                           │
                           v
                ┌──────────────────┐
                │ Security Manager │
                │ SM / TSM         │
                └───────┬──────────┘
                        │
        ┌───────────────┼───────────────┐
        │               │               │
      PMP/ePMP      page/domain      IOMMU
        │           protection          │
        │               │               │
        v               v               v
      CPU             Memory          Device
        \               |               /
         \              |              /
          +-------------+-------------+
                        |
                  Trusted Domain
                        |
                +-------+-------+
                |               |
             Enclave           TVM
```

安全性不是来自某一个 module。

而是：

```text
RoT
+
boot chain
+
privilege architecture
+
physical memory ownership
+
virtual memory
+
IOMMU
+
interrupt architecture
+
device security
+
measurement
+
attestation
+
microarchitecture defense
```

共同组成。

---

# 29. 和 Arm 做最终架构映射

这张表建议你以后一直留着。

| Arm | RISC-V | 是否等价 |
|---|---|---|
| EL3 | M-mode | 只能粗略类比 privilege |
| EL1 | S-mode | 很接近 |
| EL0 | U-mode | 很接近 |
| TrustZone Secure State | 无基础 ISA 一一对应物 | **不能映射** |
| Secure Monitor | M-mode Security Monitor | 概念相似，机制不同 |
| OP-TEE | Keystone/厂商 TEE 等 | 功能层面类比 |
| TZASC / system security controls | PMP + platform controls | 非一一对应 |
| Stage-2 translation | `hgatp` | 很接近 |
| GIC virtualization | AIA/IMSIC virtualization | 功能类比 |
| SMMU | RISC-V IOMMU | 很接近 |
| Arm CCA Realm | CoVE TVM | 设计目标更接近 |
| RMM | CoVE TSM | **概念层面很接近** |
| GPT / granule ownership | Supervisor-domain/page ownership mechanisms | 思路相近 |

所以你可以建立一个非常好的迁移路线：

```text
TrustZone experience
        |
        +--> understand TCB/security world
        |
        +--> PMP/ePMP
        |
        +--> Security Monitor
        |
        +--> Keystone

CCA / virtualization thinking
        |
        +--> H extension
        |
        +--> Supervisor Domain
        |
        +--> TSM
        |
        +--> CoVE
```

---

# 30. 一个 Staff Engineer 应该怎样回答：

## “RISC-V 没有 TrustZone，那 TEE 怎么实现？”

一个比较成熟的回答可以是：

> RISC-V 基础架构没有定义类似 Arm TrustZone Secure/Non-secure state 的统一 TEE 模型。它更倾向于提供可组合的 security primitives。
>
> 在较简单的 enclave 架构中，可以使用 M-mode Security Monitor 配置 PMP/ePMP，将一段 physical memory 从不可信 S-mode OS 中隔离出来，Keystone 就是典型案例。Security Monitor 同时负责 enclave lifecycle、context switching、measurement 和 attestation。
>
> 但 PMP 是 region-based、entry 数有限，而且仅靠 CPU physical-access protection 无法解决大规模 confidential VM、DMA 和设备问题。
>
> 所以 application-processor security 正进一步采用 page/domain based physical-memory ownership、IOMMU、AIA 和 CoVE 等机制，把不可信 Host/VMM 排除出 confidential workload 的 TCB。这个方向从设计目标上更接近 Arm CCA，而不是传统 TrustZone。

如果能回答到这个程度，已经明显不是：

```text
“我知道 PMP”
```

而是理解：

```text
RISC-V security architecture
```

---

# 31. 再回答一个关键问题

## “Linux 已经完全被攻破，RISC-V TEE 还保护什么？”

假设采用正确设计：

```text
Hardware RoT           trusted
Security Monitor/TSM   trusted
PMP/domain isolation   trusted
IOMMU security         trusted
Linux                  compromised
```

那么仍然可以保护：

```text
TEE private memory confidentiality

TEE private memory integrity

TEE initial identity

cryptographic keys

remote attestation

certain trusted execution
```

但是通常保护不了：

```text
availability

network availability

host-controlled scheduling

所有 microarchitectural side channel

所有 physical attack
```

除非平台的 threat model 明确加入相应机制。

所以真正准确的答案永远不是：

> “TEE 可以防 root。”

而应该是：

> **TEE 可以把 compromised OS 排除出某个 security domain 的 TCB，但具体还能保证哪些 property，取决于硬件隔离、I/O、side-channel 和 attestation threat model。**

---

# 32. 从 Keystone 到 CoVE，其实是一条很清晰的演进路线

你可以把整个 RISC-V TEE 学习过程浓缩成：

```text
              PMP
               |
               v
     CPU physical isolation
               |
               v
           Keystone
               |
               v
       enclave isolation
               |
     PMP scalability problem
               |
               v
 page/domain-based isolation
               |
               v
          H Extension
               |
               v
             CoVE
               |
               v
      Confidential VM
               |
        +------+------+
        |             |
      IOMMU          AIA
        |             |
        +------+------+
               |
           Trusted IO
```

这基本就是从：

```text
Embedded / simple enclave
```

一路走到：

```text
Datacenter Confidential Computing
```

的完整技术路线。

---

# 33. 第 13 阶段你真正应该掌握到什么程度？

完成这一阶段以后，我建议你的认知标准不是背下多少 CSR，而是能够脱离资料回答下面这些架构问题：

```text
1. 为什么 M-mode 不能简单等价于 Arm EL3 Secure World？

2. 为什么 MMU 无法从恶意 Linux kernel 手里保护 enclave？

3. PMP 为什么能够做到？

4. PMP 为什么又不足以构造完整 SoC TEE？

5. M-mode Security Monitor 为什么属于 TCB？

6. Smepmp 的 MML/MMWP 到底解决什么 security problem？

7. Keystone 如何利用 PMP 在 Linux 和 enclave 之间切换权限？

8. Linux root 后还能攻击 enclave 的哪些面？

9. Remote Attestation 与 Secure Boot 有什么区别？

10. PMP 为什么无法很好扩展到几百上千 confidential VM？

11. 为什么需要 page/domain-based ownership？

12. CoVE 的 TSM 为什么必须独立于普通 Host/VMM 的 security authority？

13. 为什么 H Extension 本身并不等于 Confidential Computing？

14. 为什么 IOMMU 是 TEE architecture 的组成部分？

15. 为什么 device attestation 也是 Confidential I/O 的必要条件？

16. Keystone 和 CoVE 各自解决什么问题？

17. CoVE 为什么更应该和 Arm CCA 比，而不是和 TrustZone 比？

18. TEE 为什么通常无法保证 availability？

19. PMP/ePMP、MMU、IOMMU 分别保护哪一级 address translation/access？

20. 整个系统的 Root of Trust 到底在哪里？
```

如果这 20 个问题你能从 architecture 原理自行推出来，而不是背答案：

> **RISC-V Security / TEE 这一阶段就已经真正学通了。**

---

# 34. 最终知识地图

最后把第 1～13 阶段真正串起来：

```text
                     RISC-V
                        |
                M / S / U privilege
                        |
                       CSR
                        |
                 Trap / Exception
                        |
          +-------------+-------------+
          |                           |
         PMP                       Sv39
          |                           |
        ePMP                      S-mode OS
          |                           |
          +-------------+-------------+
                        |
                       SBI
                        |
                    OpenSBI
                        |
                       SMP
                        |
                  H Extension
                        |
                HS / VS / VU
                        |
              G-stage translation
                        |
          +-------------+-------------+
          |                           |
         AIA                        IOMMU
          |                           |
       interrupt                    DMA
       isolation                  isolation
          |                           |
          +-------------+-------------+
                        |
                Security Domain
                        |
           +------------+------------+
           |                         |
       Keystone                    CoVE
       Enclave                       TVM
           |                         |
           +------------+------------+
                        |
                   Attestation
                        |
                  Root of Trust
```

如果说前面十二阶段是在学习：

```text
CPU 如何运行系统
```

那么第十三阶段真正回答的是：

> **当 CPU 上的软件彼此不再信任时，这个系统应该怎样重新划分信任边界。**

这才是 RISC-V Security / TEE 的本质。


这一阶段建议你尤其记住一句话：**Privilege hierarchy 决定“谁能配置安全机制”，而 security domain 决定“谁能访问谁”；两者不是一回事。** 这句话一旦建立起来，M-mode、PMP、Keystone、CoVE、甚至 Arm CCA 之间的关系都会清晰很多。
