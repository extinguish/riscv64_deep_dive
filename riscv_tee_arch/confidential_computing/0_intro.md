可以。为了真正理解机密计算，我建议不要从 SEV-SNP、RME、RMM 这些名词开始，而是先抓住一个核心问题：

> **传统计算机的权限模型默认认为：更高特权的软件一定可以看到更低特权的软件。  
> 机密计算要打破的，正是这个假设。**

换句话说，机密计算真正要解决的问题是：

**“如果 Host OS / Hypervisor / 云管理员本身都不可信，我还能不能安全地把程序和数据放到这台机器上运行？”**

Arm 对 Confidential Computing 的定义也基本抓住这一点：保护 **data in use**，让代码和数据即使面对具有更高软件权限的实体，也不能被观察或修改。[文档服务](https://documentation-service.arm.com/static/6812468479c4c17ad8037525?utm_source=chatgpt.com)

---

# 一、传统安全体系还缺哪一块？

我们先看三个经典场景。

数据静态存储时：

```text
Disk
 │
 │ AES / dm-crypt / BitLocker
 ▼
Encrypted Data

解决：Data at Rest
```

数据通过网络传输时：

```text
Client
   │
   │ TLS
   ▼
Server

解决：Data in Transit
```

但真正计算时：

```text
          plaintext
             ↓
         CPU / DRAM
             ↑
             │
        application
```

数据最终必须解密。

于是产生一个非常重要的问题：

```text
          Your VM
        ┌───────────┐
        │ plaintext │
        │ password  │
        │ AES key   │
        │ AI model  │
        └───────────┘
              ↑
              │
        Hypervisor
              ↑
              │
          Host Kernel
```

在传统虚拟化架构里，Hypervisor 控制二阶段页表，因此理论上可以映射 VM 的物理内存；Host root、Hypervisor 漏洞或者恶意云管理员，都可能成为威胁。

TLS 和磁盘加密对此无能为力。

因为此时数据已经进入：

> **Data in Use**

这就是 Confidential Computing 最主要的目标。

---

# 二、机密计算真正改变的是“信任模型”

传统云计算的信任链通常类似：

```text
Application
    │ trusts
    ▼
Guest OS
    │ trusts
    ▼
Hypervisor
    │ trusts
    ▼
Host OS / Cloud Provider
    │ trusts
    ▼
Hardware
```

因此你购买一个普通云 VM，本质上是在相信：

```text
AWS / Azure / GCP / CSP
         +
Hypervisor
         +
Host administrator
```

不会读取你的 VM。

而 Confidential Computing 希望把模型变成：

```text
          Application
              │
              ▼
          Guest OS
              │
              ▼
     Confidential VM
              │
              │ hardware boundary
══════════════╪══════════════════
              │
          Hypervisor
          Host Kernel
          Cloud Admin
```

这里非常反直觉：

> **Hypervisor 仍然管理 VM，但不再拥有读取 VM 内容的权力。**

Arm 对 CCA 的表述非常准确：

> 将 **management of resources** 和 **access to resources** 分开。[Arm Learn](https://learn.arm.com/learning-paths/cross-platform/cca_rme/cca/?utm_source=chatgpt.com)

这是理解整个 Confidential Computing 最重要的一句话。

---

# 三、为什么这在传统虚拟化中做不到？

以你熟悉的二阶段地址转换来看。

普通 VM：

```text
Guest Virtual Address
        │
        │ Stage-1
        ▼
Guest Physical Address
        │
        │ Stage-2 / NPT / EPT
        ▼
System Physical Address
        │
        ▼
DRAM
```

其中：

```text
Stage-1
Guest OS控制

Stage-2
Hypervisor控制
```

所以 Hypervisor 可以做：

```text
GPA 0x1000
    │
    ▼
SPA 0x80001000
```

同时 Hypervisor 自己又可以：

```text
Host VA
   │
   ▼
SPA 0x80001000
```

那么：

```text
Guest Secret
    ↓
DRAM page
    ↑
Hypervisor
```

Hypervisor 能看到数据。

所以只靠 MMU / Stage-2 virtualization 根本解决不了问题。

---

# 四、机密计算最核心的硬件思想

这里可以建立一个非常重要的统一模型。

传统系统只有：

```text
Address Translation
+
Page Permission
```

机密计算增加了一层：

```text
                    ┌───────────────┐
Guest VA ──────────►│ Stage-1       │
                    └──────┬────────┘
                           │ GPA
                           ▼
                    ┌───────────────┐
                    │ Stage-2       │  ← Hypervisor
                    └──────┬────────┘
                           │ SPA
                           ▼
                ┌─────────────────────┐
                │ Hardware ownership  │
                │ / security check    │
                └──────────┬──────────┘
                           │
                           ▼
                         DRAM
```

第三层才是关键。

而非常巧的是：

```text
AMD SEV-SNP          Arm CCA
────────────         ─────────────

RMP                  GPT
Reverse Map Table    Granule Protection Table

       ↓                    ↓

CPU强制检查           GPC硬件检查
```

也就是说：

> **Hypervisor 可以控制地址映射，但最终“这个物理页面到底属于谁”，不再由 Hypervisor 决定。**

这是 SEV-SNP 和 Arm CCA 在设计哲学上的一个重要共同点。

---

# 五、所以机密计算其实可以理解成“大型 TEE”

这和你之前熟悉的 OP-TEE 很容易建立联系。

经典 TrustZone：

```text
         CPU
          │
 ┌────────┴────────┐
 │                 │
Normal World    Secure World
Linux           OP-TEE
EL1/EL0         S-EL1
 │                 │
 Apps              TA
```

TA 可以保护：

```text
TA secret
```

不让 Linux 获取。

但问题是 TA 一般比较小：

```text
Payment TA
Key Management TA
DRM TA
Biometric TA
```

并不适合直接运行：

```text
Linux
MySQL
Redis
PyTorch
LLM
普通大型应用
```

Confidential Computing 的思路相当于：

```text
Tiny TEE
   ↓
VM-sized TEE
```

于是：

```text
Confidential VM
┌───────────────────────────┐
│ Linux                     │
│ libc                      │
│ OpenSSL                   │
│ Database                  │
│ Python                    │
│ AI model                  │
└───────────────────────────┘

整个 VM = TEE
```

AMD 明确把 SEV 定义为以 confidential VM 边界构成 TEE 的方案。[AMD](https://www.amd.com/en/developer/sev.html?utm_source=chatgpt.com)

这也是为什么很多 Confidential Computing 岗位特别喜欢 TEE 背景：两者本质上讨论的是同一类问题：

```text
Trust Boundary
Isolation
Memory Ownership
Attestation
Root of Trust
Secure Provisioning
```

只是粒度不同。

---

# 六、但仅仅“隔离内存”还不够

假设服务器告诉你：

```text
放心，我已经给你创建了一个 Confidential VM。
```

你怎么知道它是真的？

也许运行的是：

```text
普通 VM
```

或者：

```text
被修改过的 kernel
```

甚至：

```text
攻击者自己模拟的 TEE
```

所以 Confidential Computing 必须有第二个支柱：

# Remote Attestation

它解决的是：

> **远程的数据拥有者如何确认：自己的 workload 确实运行在真正的可信硬件环境里，而且初始化状态符合预期？**

典型流程：

```text
Customer
   │
   │ nonce
   ▼
Confidential VM
   │
   │ request attestation
   ▼
Hardware Root of Trust
   │
   │ sign
   ▼
Attestation Report
   │
   ▼
Customer
```

Report 中通常涉及：

```text
measurement
platform identity
firmware / TCB version
security configuration
nonce
```

客户验证之后：

```text
if attestation == trusted:
        send_database_key()
else:
        reject()
```

于是出现 Confidential Computing 一个极其重要的设计模式：

```text
        Create VM
            │
            ▼
        Measure VM
            │
            ▼
        Attestation
            │
            ▼
Verifier checks report
            │
       trusted?
       /      \
     NO        YES
     │          │
   reject    release secret
```

所以 Attestation 并不是辅助功能。

它实际上回答：

> **“我为什么敢把真正的 secret 发给这台远程机器？”**

---

# 七、把整体架构浓缩成四件事情

你以后看到 Intel TDX、AMD SNP、Arm CCA、RISC-V CoVE，可以统一往这四个问题上套：

| 问题 | Confidential Computing 需要的机制 |
|---|---|
| Host 能不能读取 VM 内存？ | Memory isolation / encryption |
| Host 能不能偷偷修改 VM？ | Integrity / ownership protection |
| 我怎么知道环境是真的？ | Attestation |
| 我什么时候把密钥交给 VM？ | Secret provisioning |

再加一个近年来越来越重要的问题：

```text
GPU / NIC / SSD
怎么办？
```

也就是：

> Trusted I/O。

AMD 当前的 SEV 路线已经扩展到基于 TDISP 的 Trusted I/O，Arm CCA 也在推进 Realm 的安全设备分配。[AMD](https://www.amd.com/en/developer/sev.html?utm_source=chatgpt.com)

---

# 八、现在看 AMD：SEV → SEV-ES → SEV-SNP

AMD 的发展路线特别适合用来理解“为什么机密计算不是简单的内存加密”。

## 第一代：SEV

基本思想：

```text
VM A → Key A
VM B → Key B
VM C → Key C
Host → Host Key
```

每个 VM 有独立的 memory encryption key，这些 key 由 AMD Secure Processor 管理。CPU 内存控制器里的硬件 AES engine 在 DRAM read/write 时透明完成加解密。[AMD](https://www.amd.com/en/developer/sev.html?utm_source=chatgpt.com)

可以把数据路径理解成：

```text
CPU
 │
 │ plaintext
 ▼
Memory Controller
 │
 │ AES(K_VM)
 ▼
DRAM

DRAM里面：
ciphertext
```

而且：

```text
VM A:
ASID = A
    ↓
Key A

VM B:
ASID = B
    ↓
Key B
```

AMD 官方资料描述了 ASID 与每个 guest 的独立 encryption key 的绑定。[AMD](https://www.amd.com/content/dam/amd/en/documents/resources/bulletin/technical-guidance-for-mitigating-effects-of-ciphertext-visibility-under-amd-sev.pdf?utm_source=chatgpt.com)

---

# 九、SEV 的第一个问题：CPU Register

假设 VM 正在执行：

```c
AES_decrypt(key, data);
```

此时：

```text
RAX = secret
RBX = key pointer
XMM0 = crypto state
```

然后发生：

```text
VMEXIT
```

传统虚拟化：

```text
Guest register
      ↓
Hypervisor
```

于是内存虽然加密：

```text
DRAM ✔
```

但 CPU context 泄漏了：

```text
Registers ✘
```

所以出现：

# SEV-ES

ES = Encrypted State。

VM 停止运行时，Guest register state 也得到保护，从而避免 Hypervisor 直接检查 Guest CPU context。[AMD](https://www.amd.com/en/developer/sev.html?utm_source=chatgpt.com)

于是：

```text
SEV
保护：
Memory

SEV-ES
保护：
Memory
+
CPU State
```

---

# 十、但是还有更严重的问题：恶意 Hypervisor 控制 NPT

例如 Guest：

```text
GPA 0x1000
→ Secret A

GPA 0x2000
→ Secret B
```

Hypervisor 控制 Nested Page Table。

它可能恶意改：

```text
GPA 0x1000
        ↓
某个旧页面
```

或者进行：

```text
remapping
aliasing
replay
```

仅仅 AES 加密无法回答：

> “这一块 ciphertext 本来应该属于哪个 Guest Physical Address？”

于是 SEV-SNP 引入极其关键的数据结构：

# RMP — Reverse Map Table

---

# 十一、RMP 是理解 SEV-SNP 的核心

你可以把传统 Nested Page Table 理解为：

```text
GPA → SPA
```

而 RMP 反过来记录：

```text
SPA → owner + expected GPA + attributes
```

于是：

```text
           Hypervisor NPT
GPA ─────────────────────► SPA
                            │
                            ▼
                     ┌────────────┐
                     │    RMP     │
                     │ owner      │
                     │ GPA        │
                     │ validated  │
                     │ permission │
                     └────────────┘
```

CPU 不再简单相信 NPT。

它还会问：

```text
这个 SPA：

是不是属于这个 VM？
是不是应该映射到这个 GPA？
这个页面是否合法？
```

RMP 是由硬件/安全架构保护的，不能由恶意 Hypervisor 随意伪造这些 ownership 关系。

AMD 将 SNP 的保护目标明确描述为阻止诸如 replay、memory remapping 等恶意 Hypervisor 攻击。[AMD](https://www.amd.com/en/developer/sev.html?utm_source=chatgpt.com)

这就是一个本质变化：

```text
以前：

Hypervisor:
"这个page属于VM A"

CPU:
"OK"


SNP：

Hypervisor:
"这个page属于VM A"

CPU:
"等等，我查一下RMP。"
```

---

# 十二、PVALIDATE 为什么存在？

这也是 SNP 非常漂亮的设计。

增加一个新页面大致有两个参与方。

Host：

```text
RMPUPDATE
```

把 page 分配给 Guest。

但此时页面进入类似：

```text
Guest-Invalid
```

Guest 自己还必须：

```text
PVALIDATE
```

表示：

> “我确认接受这个 GPA 对应的页面。”

之后：

```text
Guest-Valid
```

AMD 的 SNP 文档明确描述了这个两阶段流程：

```text
Hypervisor:
RMPUPDATE

Guest:
PVALIDATE
```

并通过 RMP 中的 GPA / Validated 信息约束恶意 remapping。[AMD](https://www.amd.com/content/dam/amd/en/documents/epyc-business-docs/white-papers/SEV-SNP-strengthening-vm-isolation-with-integrity-protection-and-more.pdf?utm_source=chatgpt.com)

这个设计其实体现了一条非常典型的安全原则：

> **资源提供者不能单方面决定安全资源的最终绑定关系。**

Host 提供资源。

Guest 接受资源。

Hardware 做仲裁。

---

# 十三、所以 AMD SNP 的 memory access 可以这样理解

假设 Guest：

```c
x = *ptr;
```

概念上：

```text
Guest Virtual Address
        │
        │ guest page table
        ▼
Guest Physical Address
        │
        │ NPT
        │ Hypervisor controls
        ▼
System Physical Address
        │
        ▼
      RMP check
        │
        ├─ owner == current VM ?
        ├─ expected GPA correct ?
        ├─ validated ?
        └─ permission OK ?
        │
        ▼
Memory Controller
        │
        │ VM-specific key
        ▼
AES decrypt
        │
        ▼
       CPU
```

这是我认为理解 AMD Confidential Computing 最重要的一张图。

因为你会发现：

> **SNP 并没有取消 Hypervisor。**

Hypervisor 仍然负责：

```text
CPU scheduling
memory allocation
NPT
interrupt delivery
I/O virtualization
VM lifecycle
```

只是它从：

```text
resource manager
+
resource owner
```

变成：

```text
resource manager
≠
security owner
```

---

# 十四、再看 Armv9：它的思想与 SNP 很像，但架构表达方式不同

传统 TrustZone：

```text
             Arm CPU
       ┌────────┴────────┐
       │                 │
Non-secure           Secure
Normal World        Secure World
```

CCA/RME 在此基础上引入：

```text
               Root
                │
       ┌────────┼─────────┐
       │        │         │
    Secure    Realm    Normal
```

即四个 Security States：

```text
Root
Secure
Realm
Non-secure
```

其中 Realm 专门用于 Confidential Computing。Arm 官方 CCA 架构就是这样描述 Normal、Secure、Realm 以及底层 Root 状态的。[Arm](https://www.arm.com/architecture/security-features/arm-confidential-compute-architecture?utm_source=chatgpt.com)

---

# 十五、它和传统 TrustZone 最关键的区别

传统 TrustZone：

```text
Secure World
    >
Normal World
```

通常可以理解为平台构建者信任 Secure World。

但云客户可能会说：

```text
我连云厂商的 Secure World firmware
也不希望完全信任。
```

CCA 引入 Realm：

```text
Secure World    Realm World
      \           /
       \         /
       mutually isolated
```

Realm 不是简单放进 Secure World。

而是独立的 Security State。

因此：

```text
Normal World
不能直接访问Realm

Secure World
也不能直接访问Realm
```

真正处于更根部的是：

```text
Root World
```

这样 TCB 可以进一步重新划分。

---

# 十六、Arm CCA 软件栈

结合你熟悉的 TF-A，可以画成：

```text
                  Realm VM
          ┌────────────────────┐
R-EL0     │ Application        │
          ├────────────────────┤
R-EL1     │ Realm Linux        │
          └────────────────────┘
                    │
                  RSI
                    │
          ┌────────────────────┐
R-EL2     │ TF-RMM             │
          │ Realm Management   │
          │ Monitor            │
          └────────────────────┘
                    ▲
                    │ RMI
                    │
NS-EL2    ┌────────────────────┐
          │ Linux KVM          │
          │ Host Hypervisor    │
          └────────────────────┘


EL3       ┌────────────────────┐
          │ TF-A Monitor       │
          │ Root World         │
          └────────────────────┘
```

RMM 运行在 Realm EL2，Realm guest 运行在 R-EL1/R-EL0；Host 通过 **RMI — Realm Management Interface** 请求 RMM 创建、执行、管理 Realm，而 Realm 自身通过 **RSI — Realm Services Interface** 请求 attestation 等服务。[文档服务](https://documentation-service.arm.com/static/64a28a02df6cd61d528c3b81?token=\&utm_source=chatgpt.com)

这跟 OP-TEE 的 SMC 调用模型有一点神似，但职责不同。

---

# 十七、Arm CCA 最值得你深入理解的是 GPT

这几乎就是 Arm 世界里对应 SNP RMP 的关键机制之一：

# Granule Protection Table

GPT 描述：

```text
Physical Granule
      ↓
属于哪个 Physical Address Space
```

例如：

```text
PA 0x80000000
      ↓
Realm

PA 0x80001000
      ↓
Non-secure

PA 0x80002000
      ↓
Secure
```

然后硬件有：

# GPC — Granule Protection Check

每次访问 physical resource 时：

```text
Requester Security State
         │
         ▼
    access PA
         │
         ▼
       GPT
         │
         ▼
      GPC check
         │
      allowed?
      /     \
    yes      no
     │        │
   access    GPF
```

Arm RME 规范明确规定，GPT 记录 resource 与 Physical Address Space 的归属，而 GPC 对访问者的 PAS 与目标 granule 的 PAS 进行匹配；不允许就产生 Granule Protection Fault。[文档服务](https://documentation-service.arm.com/static/6555fdefee7b9d0d88eb65ee?token=\&utm_source=chatgpt.com)

---

# 十八、这里你会看到与 AMD SNP 极其有意思的对应关系

| AMD SEV-SNP | Arm CCA/RME |
|---|---|
| Confidential VM | Realm |
| AMD Secure Processor | Root-of-trust firmware / platform RoT |
| RMP | GPT |
| RMP check | GPC |
| VM ownership | PAS / granule ownership |
| NPT | Realm Stage-2 Translation |
| Hypervisor | NS-EL2 KVM |
| security monitor | RMM + Root Monitor |
| Attestation report | Realm attestation token/report |

注意它们不是一一等价的实现，但**解决的问题非常接近**。

---

# 十九、为什么 Arm 还需要 RMM？

因为如果：

```text
KVM controls Realm Stage-2 directly
```

那 KVM 仍可能通过：

```text
modify S2 translation
```

攻击 Realm。

所以 Arm 把 Realm 安全相关的二阶段映射管理放进 RMM。

Host 可以提出：

```text
给Realm增加一页内存
创建vCPU
运行Realm
```

但真正涉及：

```text
Realm Translation Table
security validation
Realm context
```

由 RMM 执行。

Arm 的 RMM specification 明确指出 Realm Stage-2 descriptor 由 RMM 写入；Host 通过 RMI 驱动这些状态转换，而不是直接控制安全关键映射。[文档服务](https://documentation-service.arm.com/static/642a900db90f6b20f33b5e7c?utm_source=chatgpt.com)

于是又出现：

```text
Host
负责 policy：

“我要给Realm 2GB RAM。”

RMM / Root
负责 enforcement：

“这些page到底是否真的安全地属于Realm。”
```

仍然是那句话：

> management ≠ access.

---

# 二十、Arm Realm 创建大概是什么过程？

把大量细节压缩之后，大概可以理解成：

```text
KVM
 │
 │ allocate pages
 ▼
delegate pages
 │
 ▼
Root / GPT
 │
 │ NS → Realm ownership
 ▼
RMM
 │
 │ create Realm Descriptor
 │ create RTT
 │ create REC(vCPU)
 │ populate Realm memory
 ▼
measurement
 │
 ▼
activate Realm
 │
 ▼
Realm Linux
```

Arm 当前的参考 CCA stack 中，KVM 会通过 RMI 请求 TF-RMM 创建 Realm、配置 Stage-2、创建 REC，并在初始化阶段 seal measurement。[文档服务](https://documentation-service.arm.com/static/6863a14046dc1e49e34b7b70%3Ftoken%3D?utm_source=chatgpt.com)

其中：

```text
REC
≈ Realm Execution Context
```

可以暂时理解成 Realm vCPU 的受保护执行上下文。

后面我们深入 CCA 时再专门拆它。

---

# 二十一、Attestation 把整个体系闭环

现在来看最重要的一次业务流程。

假设你有：

```text
Bank Database
```

里面包含几百万客户信息。

你想在云上处理，但不相信云管理员。

第一阶段：

```text
Cloud
   │
   ▼
Create Confidential VM
```

第二阶段：

```text
Hardware
measure:
    firmware
    kernel
    initial VM image
    security config
```

第三阶段：

```text
VM
 │
 │ attestation report
 ▼
Bank Attestation Server
```

银行验证：

```text
Hardware genuine?
Firmware acceptable?
TCB version acceptable?
Measurement == expected?
Debug disabled?
Nonce correct?
```

通过后：

```text
Bank KMS
   │
   │ Database Key
   ▼
Confidential VM
```

于是 secret 从来没有交给：

```text
Host OS
Hypervisor
Cloud administrator
```

而只交给：

```text
经过验证的 Confidential VM
```

这其实正好与你之前讨论 TPM Attestation 时建立的模型完全衔接：

```text
measurement
+
hardware identity
+
attestation
+
remote verifier
+
secret release
```

机密计算只是把这个机制真正用于：

> **远程建立一个可信执行环境，然后安全地向其中 provision secret。**

---

# 二十二、所以 Secure Boot、Measured Boot、Attestation、Confidential Computing 是一条链

可以这样记：

```text
Secure Boot
    │
    │ 防止错误代码启动
    ▼
Trusted Initial State


Measured Boot
    │
    │ 记录启动了什么
    ▼
Measurement


Attestation
    │
    │ 向远端证明measurement
    ▼
Remote Trust


Confidential Computing
    │
    │ hardware isolation
    │ + attestation
    ▼
Protected Execution
```

所以：

> Secure Boot 解决“能不能启动”。

> Attestation 解决“远端敢不敢相信你”。

> Confidential Computing 解决“运行起来以后，上层管理员还能不能偷看你”。

---

# 二十三、它不能解决什么？

这个边界也非常重要。

如果 Realm/SEV-SNP VM 自己运行：

```c
strcpy(buf, attacker_input);
```

然后 stack overflow：

```text
Guest compromised
```

Confidential Computing 不会 magically 修复它。

所以它主要保护的是：

```text
Outside → Inside
```

例如：

```text
malicious Hypervisor
malicious Host OS
other VM
some physical memory attacks
DMA attacks
```

而不是自动保护：

```text
buggy guest application
guest root attacker
bad application design
weak crypto
logic vulnerability
```

它通常也不提供 Availability 保证。

恶意 Hypervisor 仍然可以：

```text
不调度你
断电
杀掉VM
不给你内存
不给你网络
```

即：

```text
Confidentiality   ✔
Integrity         ✔ / architecture-dependent
Availability      通常 ✘
```

另外 side channel 仍是复杂问题。例如 AMD 公开文档也明确讨论过 guest ciphertext visibility 以及相关 side-channel 风险，所以不能简单把 SNP 理解成“物理层和所有微架构侧信道全部解决”。[AMD](https://www.amd.com/content/dam/amd/en/documents/resources/bulletin/technical-guidance-for-mitigating-effects-of-ciphertext-visibility-under-amd-sev.pdf?utm_source=chatgpt.com)

---

# 二十四、现在你应该先形成这样一个最终认知模型

把 Confidential Computing 压缩成一张图：

```text
                Workload Owner
                      │
              verify attestation
                      │
              release secret
                      │
                      ▼
╔═════════════════════════════════╗
║        Confidential VM         ║
║                                ║
║   Application                  ║
║   Guest Linux                  ║
║   secrets / models / keys      ║
║                                ║
╚═════════════════════════════════╝
        ▲                   ▲
        │                   │
   CPU state             memory
  protection           protection
        │                   │
════════╪═══════════════════╪════════
        │  Hardware Trust Boundary
════════╪═══════════════════╪════════
        │                   │
      Host OS           Hypervisor
        │                   │
        └─────────┬─────────┘
                  │
             Cloud Admin

             NOT TRUSTED
```

对应 AMD：

```text
SEV memory encryption
      +
SEV-ES register protection
      +
SNP RMP integrity/isolation
      +
AMD-SP attestation
```

对应 Armv9：

```text
Realm Security State
      +
GPT / GPC
      +
RMM
      +
Root Monitor
      +
Realm Attestation
```

---

# 二十五、结合你的背景，我建议后面按这条路线深入

你已经有 TrustZone / OP-TEE / TF-A / Secure Boot 的背景，所以不需要按普通入门教程学习。最值得深入的是这条链：

```text
① Threat Model
        ↓
② Confidential VM vs traditional TEE
        ↓
③ Memory ownership
        ↓
④ AMD:
   SEV → ES → SNP
   RMP / PVALIDATE / NPT
        ↓
⑤ Arm:
   TrustZone → RME
   Root / Realm / Secure / NS
   GPT / GPC
        ↓
⑥ RMM
   RMI / RSI / REC / RTT
        ↓
⑦ Attestation
   Measurement
   certificate chain
   key provisioning
        ↓
⑧ I/O问题
   virtio/shared memory
   DMA/IOMMU
   Trusted I/O
        ↓
⑨ Threat analysis
   malicious hypervisor
   side-channel
   interrupt injection
   replay/remapping
   migration
```

其中我认为**下一步最值得直接深入的是“为什么 Memory Encryption 本身完全不够，以及 AMD SEV-SNP 的 RMP 到底解决了什么”**。

因为一旦真正理解：

```text
NPT
+
RMP
+
PVALIDATE
```

这三个东西之间的关系，你就已经抓住了现代 Confidential VM 架构非常大的一部分本质；随后再看 Arm：

```text
Stage-2
+
GPT/GPC
+
RMM
```

会非常容易。[AMD](https://www.amd.com/content/dam/amd/en/documents/epyc-business-docs/white-papers/SEV-SNP-strengthening-vm-isolation-with-integrity-protection-and-more.pdf?utm_source=chatgpt.com)

而且对于你已有的 TEE 背景，可以把整个领域浓缩成一句话：

> **传统 TEE 是“让一个小的 trusted world 抵抗大 OS”；Confidential Computing 则是在虚拟化场景下，让整个 workload/VM 能够抵抗它自己的 Host 和 Hypervisor。**

这也是从 **TEE → Confidential Computing → AMD SEV-SNP / Arm CCA** 最自然的一条认知路线。
