# RISC-V SMMPT 架构深度教程

PART III · System-Level Resource Isolation

本部分完成前三章：

- Chapter 10 — 跨 Domain 内存生命周期与安全共享
- Chapter 11 — IOMMU、I/O MPT 与 DMA 安全隔离
- Chapter 12 — Domain Interrupt Isolation 与 AIA

本部分会从单纯的 CPU 物理内存保护，进一步扩展到内存所有权、设备 DMA、跨域通信以及中断资源隔离。

我们继续使用同一个 RISC-V SoC 场景：16 个 RV64 Hart，M-mode 运行 Root Domain Security Manager（RDSM），两个 S-mode Domain 分别运行 Linux 和 OP-TEE，并假设硬件支持所讨论的 Smsd、I/O MPT 和 Smsdia 机制。

规范依据： 主要参考 RISC-V Supervisor Domain Access Protection 公开规范及已批准的 AIA 1.0。需要注意，截至本次讲解，SDAP 相关扩展仍处于规范推进阶段，不能把本章设计示例直接视为已经合入 OP-TEE/OpenSBI 主线的实现。

[image](https://www.google.com/s2/favicons?domain=https://riscv.atlassian.net\&sz=32)

RISC-V Tech Hub+1



# Chapter 10 — 跨 Domain 内存生命周期与安全共享

## 10.1 本章目标：从 Memory Protection 走向 Memory Ownership

在 Part II 中，我们已经学习了 MPT 如何控制对物理地址的 R/W/X 访问。

例如：

| 物理区域                | Linux Domain | OP-TEE Domain |
| ------------------- | ------------ | ------------- |
| Linux DDR           | RWX          | 禁止            |
| OP-TEE Secure DDR   | 禁止           | RWX           |
| Shared Memory       | RW           | RW            |
| RDSM Private Memory | 禁止           | 禁止            |

示例权限策略。实际权限应按代码段、数据段、共享页、设备区域分别最小化配置，而非整个区域统一 RWX。

这已经能够建立基本隔离。

但有一个更加困难的问题：

假设一块原本属于 Linux 的物理内存，需要临时交给 OP-TEE 使用。如何证明 Linux 已经彻底失去了访问它的能力？

考虑一块物理页面：

```
Physical Address: 0x9000_0000
Size:             4 KiB

Initial Owner: Linux
New Owner:     OP-TEE
```

如果仅仅修改 MPT：

```
Linux MPT: RW -> No Access
OP-TEE MPT: No Access -> RW
```

看起来已经完成了转移。

但是，我们必须继续考虑：

- Linux 是否仍有正在执行的 Load/Store？
- 另一个 Hart 是否缓存了旧 MPT 权限？
- Linux 的 DMA Engine 是否仍在写这块内存？
- Linux 原有的数据是否需要清零？
- OP-TEE 是否会通过旧映射泄漏此前保存的数据？
- 如果更新过程中发生错误，内存归谁管理？

因此要建立一个重要认识：

MPT 是硬件权限执行机制，Memory Ownership 是软件定义并维护的安全状态。

二者不能混为一谈。

SMMPT 规范允许 RDSM 通过撤销一个 Domain 的访问权限、授予另一个 Domain 权限来实现动态内存重新分配，并明确指出缓存同步及敏感数据清理是转换过程需要考虑的安全事项。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+1



## 10.2 为 Memory Ownership 建立正式模型

首先区分三个概念。

| 概念       | 含义                  | 管理者           |
| -------- | ------------------- | ------------- |
| Owner    | 谁拥有资源分配和生命周期控制权     | RDSM 的策略及元数据  |
| Accessor | 哪些 Domain 当前能够访问该资源 | MPT 与 I/O MPT |
| Borrower | 哪个 Domain 临时获得授权    | 安全共享协议        |

例如：

```
Page P
  Owner:       OP-TEE
  Accessors:   OP-TEE, Linux
  Permission:  OP-TEE RW, Linux R
  State:       SHARED
```

此时 Linux 可以读取页面，但并不意味着 Linux 有权把页面转移给第三个 Domain。

这个差异至关重要。

Access Permission 不等于 Ownership，也不等于 Delegation Authority。

如果 RDSM 仅维护 MPT，而没有独立记录所有权，那么下面两个状态可能在权限层面完全相同：

```
State A:
  Owner: Linux
  Shared With: OP-TEE

State B:
  Owner: OP-TEE
  Shared With: Linux
```

两者的 MPT 都可能是双方 RW，但它们的回收权、撤销权和安全策略完全不同。

因此我建议 RDSM 维护一个独立的 Physical Memory Ownership Database。

### 10.2.1 推荐的元数据结构

下面是课程参考设计，不是 RISC-V 规范定义的 API 或数据结构。

```
enum page_state {
    PAGE_FREE,
    PAGE_PRIVATE,
    PAGE_REVOKING,
    PAGE_QUARANTINED,
    PAGE_SHARED,
    PAGE_TRANSFERRING,
};

struct page_owner {
    uint64_t pa;
    uint64_t size;

    uint32_t owner_sdid;
    uint32_t state;

    uint64_t accessor_bitmap;
    uint64_t generation;

    uint32_t pin_count;
    uint32_t dma_refcount;
};
```

字段设计思路：

- `owner_sdid`：真正的资源所有者。
- `accessor_bitmap`：允许访问的 Domain 集合；每个 Domain 的 R/W/X 仍需单独表示。
- `state`：资源生命周期状态。
- `generation`：防止旧句柄在页面重新分配后仍然有效。
- `pin_count`：防止存在未结束使用者时回收页面。
- `dma_refcount`：跟踪尚未完成的 DMA 使用。

这里的 `generation` 特别重要。

假设 Linux 曾拥有 Page P：

```
Handle = (Page P, Generation 12)
```

随后页面转移给 OP-TEE，又重新分配给 Linux：

```
Handle = (Page P, Generation 13)
```

如果只根据物理地址授权，Linux 可能拿旧 Handle 再次操作新资源。

加入 Generation 后，RDSM 就能区分这两次所有权关系，从而降低 ABA 和 Stale Handle 风险。

当然，Generation 只是逻辑有效性验证；真正阻断恶意访问仍依赖硬件 MPT。

## 10.3 设计一套 Memory Lifecycle State Machine

建议至少支持以下状态。

建议采用中间隔离态，而非直接从旧 Owner 切换到新 Owner。

为什么需要 `QUARANTINED`？

因为权限更新不是一个简单的赋值，而是一个可能跨越多个 Hart、多个 I/O MPT Checker 和多个设备的分布式同步过程。

在此期间，如果直接把页面授权给新 Domain，就可能产生旧 Domain 和新 Domain 同时可访问的窗口。

可以定义一个安全不变量：

\\[ NewOwnerAccess(P) \Rightarrow OldOwnerQuiesced(P) \\]

即：新 Owner 获得访问权限之前，旧 Owner 的使用必须已经结束。

更严格地说，还必须确保：

\\[ NewOwnerAccess(P) \Rightarrow \begin{cases} OldCPUAccessRevoked(P)\\\ OldDMAAccessRevoked(P)\\\ OldTransactionsDrained(P)\\\ MemorySanitized(P) \end{cases} \\]

对于没有完成这些条件的页面，RDSM 必须保持隔离态，而不能把它当成可分配内存。

## 10.4 实际场景：Linux 将 4 KiB 页面转移给 OP-TEE

假设：

```
Linux SDID = 1
OP-TEE SDID = 2

Target Page:
  PA = 0x9000_0000
  Size = 4 KiB
```

### Step 1：Linux 发起 Transfer 请求

假设平台定义了一个安全服务：

```
struct mem_transfer_req {
    uint64_t pa;
    uint64_t size;
    uint32_t target_sdid;
    uint64_t generation;
};
```

Linux 通过平台定义的调用接口将请求发送给 RDSM。

注意，RISC-V SMMPT 规范并没有规定一个通用的 `SBI_MEM_TRANSFER` 扩展。这里是需要在系统集成时设计的软件 ABI。

RDSM 首先验证：

1. 调用者确实是当前 Owner。
2. 地址已经正确对齐。
3. 页面不包含 RDSM 或其他 Domain 私有资源。
4. 页面没有被不允许的设备占用。
5. 目标 Domain 有资格接收该内存。
6. 请求的 Generation 有效。

### Step 2：冻结旧 Owner 的新使用

RDSM 将状态更新为：

```
PRIVATE -> REVOKING
```

随后阻止新的分配、映射、共享和 DMA 提交。

但这里要注意：仅仅要求 Linux 删除页表是不可信的。

如果 Linux 已经被攻陷，它完全可以拒绝删除映射，或者重新建立映射。

因此，RDSM 必须能够不依赖 Linux 自愿配合，直接撤销硬件访问权限。

这正是 MPT 的价值。

### Step 3：撤销 Linux 的 MPT 权限

```
Before:
  MPT_Linux[P] = RW

After:
  MPT_Linux[P] = No Access
```

之后要执行相应的 MPT 同步操作。

不能仅通过：

```
sfence.vma
```

就认为撤销完成，因为 `SFENCE.VMA` 处理的是 S-stage 地址转换相关同步，而不是 MPT 权限缓存的通用失效。

应使用相应的 `MFENCE.PA` / `MINVAL.PA` 机制，并根据实际 MPT 修改场景完成多 Hart 同步。

此外，CPU 权限撤销不能替代 DMA 权限撤销。若页面曾授权设备访问，必须同步更新 I/O MPT 保护及相关 IOMMU 状态。

### Step 4：排空旧请求并清理页面

RDSM 必须确认：

- 所有相关 Hart 的旧权限状态已失效。
- 设备不再有新的 DMA 请求进入。
- 已接收的旧 DMA 请求完成或被安全中止。
- 必要的缓存写回/失效和一致性处理已完成。
- 页面中需要保密的内容已清除。

这里的清理不是简单地调用 `memset()` 就一定足够。

例如，存在 Write-back Cache、Non-coherent DMA 或外部 Memory Encryption Engine 时，必须综合考虑缓存中可能残留的旧数据以及加密上下文变化。

如果新 Owner 有不同的内存加密密钥，还要遵循平台的密钥切换及数据处理协议。

### Step 5：授予 OP-TEE 权限

完成上述处理后：

```
MPT_OPTEE[P] = RW
```

再同步相应 MPT 状态，最终提交：

```
owner_sdid = 2
state      = PRIVATE
generation = 13
```

只有达到所有安全条件后，Transfer 才能返回成功。

### 10.4.1 参考实现伪代码

```
int rdsm_transfer_page(struct page_owner *p,
                       uint32_t new_owner)
{
    lock(p);

    if (!validate_transfer(p, new_owner)) {
        unlock(p);
        return -EINVAL;
    }

    p->state = PAGE_REVOKING;

    /*
     * Platform operations:
     * Disable new uses, quiesce devices and
     * revoke the old domain's permissions.
     */
    if (quiesce_old_owner(p))
        goto quarantine;

    if (revoke_cpu_mpt(p))
        goto quarantine;

    if (revoke_io_mpt(p))
        goto quarantine;

    if (sync_all_access_paths(p))
        goto quarantine;

    p->state = PAGE_QUARANTINED;

    if (sanitize_memory(p))
        goto quarantine;

    if (grant_new_owner_mpt(p, new_owner))
        goto quarantine;

    if (sync_new_permissions(p))
        goto quarantine;

    p->owner_sdid = new_owner;
    p->generation++;
    p->state = PAGE_PRIVATE;

    unlock(p);
    return 0;

quarantine:
    /*
     * Do not silently return the page to
     * either domain while state is uncertain.
     */
    p->state = PAGE_QUARANTINED;
    unlock(p);
    return -EIO;
}
```

该代码只展示状态协调逻辑。实际实现还需要定义锁的范围、跨 Hart 同步、MMIO 命令完成等待、错误恢复与授权失败时的处理。尤其不能假设每一种失败都可以安全地回滚到原 Owner。

## 10.5 Shared Memory：共享和所有权转移有本质区别

现在讨论另一个经常被混淆的机制：Shared Memory。

假设 Linux 需要通过 OP-TEE Client API 调用一个 TA：

```
TEEC_InvokeCommand(session,
                   CMD_PROCESS_DATA,
                   &operation,
                   &origin);
```

`operation` 中传递了一块 Linux Buffer。

Linux 和 OP-TEE 都需要访问这块物理内存，但 Linux 并没有将它永久赠予 OP-TEE。

这是一种借用关系：

```
Owner: Linux

Linux  -> RW
OP-TEE -> R
```

如果 TA 需要把结果写回 Buffer：

```
Linux  -> RW
OP-TEE -> RW
```

实际授权应尽可能限定共享页面及必要的操作权限。

我们将共享内存分为三类：

| 类型                | 说明                | 示例                    |
| ----------------- | ----------------- | --------------------- |
| Private           | 单个 Domain 独占      | TA Heap、Private Key   |
| Global Shared     | 所有允许的 Domain 均可访问 | 公共通信区域                |
| Restricted Shared | 只有特定 Domain 子集可访问 | Linux ↔ OP-TEE Buffer |

这种区分与 SDAP 规范对 Domain Exclusive、Global Shared 和 Restricted Shared Memory 的描述一致。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 10.5.1 Shared Memory 调用路径

```
Linux Application
   |
   | TEEC_InvokeCommand()
   v
libteec / Linux OP-TEE Driver
   |
   | Prepare Shared Memory
   v
RDSM / Platform Security Service
   |
   | Validate memory and sharing policy
   | Grant appropriate MPT permissions
   v
OP-TEE Domain
   |
   | Map shared PA into TEE VA
   | Validate operation parameters
   v
Trusted Application
   |
   | Read / Write Shared Buffer
   v
Return to Linux
   |
   | Revoke temporary access if required
   v
Complete
```

这是一条基于 SMMPT 的平台集成示意路径，不是声称现有 upstream Linux OP-TEE Driver 已经执行这些 RDSM 调用。

几个实现细节必须理解。

第一，Linux VA 与 OP-TEE VA 不需要相同。

例如：

```
Linux VA:
  0xffff_ffe0_1000_0000
          |
          | Linux satp
          v
Physical:
  0x9000_0000
          ^
          | OP-TEE satp
          |
OP-TEE VA:
  0xffff_ffc0_2000_0000
```

MPT 检查的是物理地址与 Domain 权限，而不是两端的虚拟地址是否相同。

第二，Shared Memory 不意味着可以信任其中的数据。

即便 RDSM 允许 OP-TEE 读取 Linux Buffer，Linux 仍可能在 OP-TEE 验证完长度、指针或命令之后修改 Buffer。

因此 TA 必须将 Shared Buffer 看作 Untrusted Input。

涉及敏感操作时，应考虑将参数复制到 Secure Private Memory，完成长度、边界和格式检查后再处理。

第三，不能把 Secure Private Memory 当成普通 Shared Memory。

例如私钥原本存放在 OP-TEE 私有页面中，不能为了给 Linux 提供计算结果而直接对整个页面开启 Linux Read 权限。

应使用独立共享 Buffer，只复制经过允许的数据。

## 10.6 Cache Coherency：共享内存不等于数据天然一致

在 ARM 或 RISC-V 多核 SoC 中，CPU Cache Coherency 和 Domain Access Protection 是两个不同的问题。

例如：

```
Linux Hart 0:
  Buffer[0] = 0x1234

OP-TEE Hart 1:
  value = Buffer[0]
```

如果系统提供正确的硬件 CPU 缓存一致性，两端使用兼容的 Cacheability 属性和相应同步协议，则不需要在每次共享内存访问后手动 Clean/Invalidate Cache。

但如果：

- 两个 Hart 不在同一个一致性域；
- 某一端使用 Non-cacheable Mapping；
- 设备 DMA 不具备 I/O Coherency；
- 或平台存在特殊的 Memory Encryption / Cache Attribute 约束；

那么软件可能需要显式进行 Cache Maintenance。

需要强调：

Cache Flush 不能替代 MPT Invalidation；MPT Invalidation 也不能替代数据缓存一致性处理。

它们解决的是不同层面的问题。

| 操作                        | 主要目的               |
| ------------------------- | ------------------ |
| `SFENCE.VMA`              | 同步 S-stage 地址转换状态  |
| `MFENCE.PA` / `MINVAL.PA` | 同步 MPT 权限相关状态      |
| Cache Clean / Invalidate  | 管理数据缓存可见性          |
| I/O MPT `MPTINVAL`        | 同步设备侧 MPT 权限缓存     |
| I/O MPT `IOFENCE`         | 完成相关 I/O 请求排序与配置同步 |

最后两项将在 Chapter 11 具体分析。

## 10.7 多 Hart 并发：为什么 Ownership Transfer 很容易出现 Race Condition？

假设 Linux Domain 有 Hart 0、Hart 1 和 Hart 2。

```
Hart 0:
  Request transfer P to OP-TEE

Hart 1:
  Continually read P

Hart 2:
  Submit DMA involving P
```

RDSM 在 Hart 0 上撤销 MPT 权限，并不意味着 Hart 1 立即停止使用旧权限。

而 Hart 2 提交的设备请求可能已经进入 I/O Bridge。

如果没有同步机制，可能发生：

```
T0: Hart 0 revokes MPT of P
T1: Hart 1 uses stale permission
T2: OP-TEE is granted P
T3: Old DMA writes P
```

这会破坏 OP-TEE 的完整性。

因此真实设计必须解决三个层面的同步：

1. CPU 同步： 所有可能使用旧 MPT 上下文的 Hart 必须完成权限更新同步。
2. 设备同步： 阻止新增 DMA，处理旧 DMA，并同步 I/O MPT 和 IOMMU。
3. 生命周期同步： 只有双方都不再处于旧授权状态时，才能提交所有权变化。

同时还应考虑一个设计问题：当某个 Hart 长时间未响应同步请求时怎么办？

不能简单地假设 IPI 一定成功，也不能无限等待而不提供故障处理策略。

可行的设计通常需要有受信任的 Hart 管理、停止执行或故障隔离机制；具体依赖平台实现。

## 10.8 Chapter 10 的安全设计总结

我建议记住以下五条原则：

1. Ownership 不等于 Permission。 MPT 只表达访问能力，所有权语义需要软件维护。
2. Revoke 必须先于 Grant 完成。 严禁在旧访问路径仍有效时开放新访问。
3. CPU、DMA、缓存和中断都可能影响内存生命周期。 只修改 CPU MPT 不足以保证安全。
4. Shared Memory 仍然是不可信输入。 被授权共享不意味着数据具有完整性保证。
5. 失败时安全优先于可用性。 不确定状态下的页面应隔离，而不是立即重新分配。

### Chapter 10 — Staff Engineer 测试题

建议先独立回答，再展开查看对应的答题要点。

1\. 为什么只有 MPT 权限表还不足以实现安全的 Memory Ownership？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

2\. 设计 Linux → OP-TEE 页面转移协议，如何保证切换期间不存在双重访问窗口？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

3\. 一个 Linux Hart 在 MPT 撤销后仍能访问 Secure Page，你会从哪些方向定位？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

4\. 为什么在内存 Transfer 流程中先更新 Owner Metadata，最后撤销 MPT 权限是不安全的？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

5\. OP-TEE 接收 Linux Shared Memory 后，为什么不能直接相信 Header 和 Length？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

6\. 假设页面的 Owner 从 A → B → A，如何避免旧 Handle 被重新利用？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

7\. 一个页面还有未完成 DMA，你会允许它进入 FREE 状态吗？为什么？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

8\. 如何处理多 Hart MPT 同步过程中某个 Hart 无响应的问题？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

9\. Memory Encryption 能否代替 MPT？MPT 能否代替 Memory Encryption？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

10\. 如果要在 RDSM 实现 Memory Manager，你会设计哪些关键不变量和测试？

Staff · 架构设计 / 故障分析

查看答题要点&#x20;

# Chapter 11 — IOMMU、I/O MPT 与 DMA 安全隔离

## 11.1 本章目标：CPU 安全不代表 SoC 安全

先考虑一个非常典型的场景：

Linux 和 OP-TEE 已经通过 SMMPT 实现了完全隔离。

```
Linux Domain:
  SDID = 1

OP-TEE Domain:
  SDID = 2
```

CPU 访问权限已经正确配置：

```
Linux CPU -> Secure DDR: Denied
OP-TEE CPU -> Secure DDR: Allowed
```

现在 Linux 控制一个 PCIe NVMe Controller。

该设备支持 DMA，并且 Linux 可以设置 DMA Descriptor 中的目标地址。

于是产生一个问题：

如果 Linux 把 DMA Destination Address 设置为 OP-TEE 的 Secure DDR，SMMPT 能阻止这个 DMA 吗？

仅依赖 Hart 上的 SMMPT 检查，答案是不能。

因为 DMA 并不是 CPU 执行普通 Load/Store 指令产生的访问。

它是一个独立 Bus Master 发起的内存事务。

逻辑示意：CPU 与设备是两个独立的内存请求源，需要分别执行适当的安全检查。

这正是引入 I/O MPT Checker 的动机。

它的基本目标是：

不论某个物理地址是由 CPU 访问，还是 DMA 设备访问，都必须满足该请求所属 Supervisor Domain 的访问策略。

## 11.2 为什么 IOMMU 不能直接替代 I/O MPT？

IOMMU 的核心功能是对设备发起的地址进行转换和权限检查。

例如：

```
Device DMA:
  IOVA = 0x4000_0000
          |
          v
        IOMMU
          |
          v
  PA   = 0x9000_0000
```

IOMMU 可以根据设备上下文、页表和配置策略决定地址转换是否有效。

但如果 Linux 控制相关 IOMMU 映射，它可能尝试把某个 IOVA 映射到 OP-TEE 的 Secure DDR。

例如：

```
Linux-managed DMA Mapping:

IOVA 0x4000_0000
    ->
PA   0x9000_0000 (Secure DDR)
```

如果没有更高信任层的物理地址保护，那么被攻陷的 Linux 就可能利用设备访问其不应访问的内存。

当然，也可以把 IOMMU 的全部安全敏感配置都交给 M-mode 管理。这样同样能够建立设备隔离，但会改变 IOMMU 的管理模型，并可能增加 Firmware 参与度。

I/O MPT 提供的另一种分工是：

- Linux 管理自己被授权设备的 IOMMU 地址转换。
- RDSM 管理跨 Domain 的最终物理地址访问权限。
- 即使 Linux 错误地建立某个 DMA 映射，设备访问仍不能越过 Domain 的物理内存边界。

这与 CPU 一侧的设计十分相似：

```
CPU:
  Linux manages satp
  RDSM manages MPT

Device:
  Linux may manage IOMMU mappings
  RDSM manages I/O MPT policy
```

所以可以将两者的职责归纳为：

| 组件      | 核心问题                            |
| ------- | ------------------------------- |
| IOMMU   | 设备请求地址应该转换到哪里？                  |
| I/O MPT | 转换后访问的物理地址，是否允许该设备所属 Domain 访问？ |

二者不是互斥关系。

事实上，SDAP 规范明确允许 I/O MPT Checker 与 IOMMU 协同工作，也允许在不使用 IOMMU 的平台上，单独通过 I/O MPT 对设备物理地址访问实施控制。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 11.3 I/O MPT Checker 的整体硬件架构

下面沿着规范定义的功能模块解释一次 DMA 请求。

```
PCIe Device
     |
     | DMA Request
     | Device ID / TEE attributes
     v
+-----------------------------+
|          I/O Bridge         |
|                             |
|    +-------------------+    |
|    | SDCL              |    |
|    | Domain Classifier |    |
|    +-------------------+    |
|       |                     |
|       | SDID / IOMMU ID     |
|       v                     |
|    +-------------------+    |
|    | IOMMU             |    |
|    | IOVA -> SPA       |    |
|    +-------------------+    |
|       |                     |
|       | SPA                 |
|       v                     |
|    +-------------------+    |
|    | I/O MPT Checker   |    |
|    | MPT R/W/X Check   |    |
|    +-------------------+    |
+-----------------------------+
     |
     | Allowed transaction
     v
  Memory / MMIO
```

这里是逻辑数据流，并不意味着所有 SoC 的硬件模块在物理上必须按该顺序独立部署。

规范将 I/O MPT Checker 的核心功能分为 SDCL 与 MPT Check，并定义 SDR/SDC、DTR/DTC、MCR/MCC 等与 I/O Bridge 交互的逻辑接口。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 11.3.1 SDCL：Supervisor Domain Classifier

它负责判断一条 DMA Request 应当归属哪个 Supervisor Domain。

输入可能包含：

- Device ID
- PCIe Requester ID
- PCIe IDE Stream Identifier
- TEE-associated Transaction 属性
- 其他平台支持的可信事务源标识

输出包括：

```
SDID
IOMMU_ID
```

例如：

| Device               | SDID        | IOMMU                |
| -------------------- | ----------- | -------------------- |
| NVMe 0               | 1（Linux）    | IOMMU 0              |
| Secure Crypto Engine | 2（OP-TEE）   | IOMMU 1              |
| Root Security Device | Root Policy | Root-controlled path |

第三行需要使用平台对应的 Root Device 隔离机制，不能简单假定所有 Root Device 都适合配置成普通 Supervisor Domain。

这里必须注意：`SDID` 不能由不可信设备任意声明并被系统无条件信任。

SDCL 使用的设备身份及安全属性，必须来自能够被硬件或可信管理组件可靠识别的事务元数据。

否则一个恶意设备只需伪装成 Secure Device，就能突破隔离。

### 11.3.2 IOMMU：地址转换

SDCL 确定设备上下文后，I/O Bridge 请求对应 IOMMU 进行地址转换。

```
IOVA
  |
  | IOMMU page table walk
  v
Supervisor Physical Address (SPA)
```

对于支持多级虚拟化的设备，转换过程还可能涉及 Guest Address Translation。

这里要特别注意，IOMMU 自身读取 Device Context、Page Table 等内存数据结构也属于内存访问。

IOMMU 自身也可能是 Bus Master，也应受到正确的安全域归属和相应物理访问保护。

规范要求为 IOMMU 自身发起的内存访问提供有效分类规则，不能因为它是地址转换单元就默认不受检查。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 11.3.3 I/O MPT：物理权限检查

得到最终 SPA 后，I/O MPT Checker 查询对应 SDID 的 MPT。

例如：

```
Device ID = 0x0100
        |
        v
SDCL
        |
        v
SDID = 1
        |
        v
MPT_Linux[0x90000000]
        |
        v
Permission = No Access
        |
        v
DMA Rejected
```

请注意：DMA 不会自动使用当前某个 Hart 的 `mmpt` CSR。

设备可能在 Linux Domain 没有运行于任何 Hart 时继续 DMA，因此 I/O MPT Checker 必须能够独立选择并访问相应 Domain 的 MPT。

这也是它必须维护 `SDID → MPT Configuration` 关联的原因。

## 11.4 从规范寄存器深入 I/O MPT 配置

I/O MPT Checker 提供 Memory-mapped Register Interface。

其基础寄存器布局为：

| Offset | Register       | Size   |
| ------ | -------------- | ------ |
| 0x00   | `capabilities` | 32-bit |
| 0x04   | `status`       | 32-bit |
| 0x08   | `control`      | 32-bit |
| 0x0C   | `command`      | 32-bit |
| 0x10   | `data1`        | 64-bit |
| 0x18   | `data2`        | 64-bit |

这是规范定义的实际偏移，而不是示例中随意创造的寄存器。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 11.4.1 `control.MODE`

I/O MPT Checker 的模式尤其重要。

| MODE | 名称   | 行为              |
| ---- | ---- | --------------- |
| 0    | Off  | 不允许新的入站内存事务     |
| 1    | Bare | 无 MPT 保护，但有特定限制 |
| 2    | On   | 启用完整 I/O MPT 保护 |

这里有一个值得注意的特殊规定：

在当前规范的 `Bare` 模式下，非 TEE-associated 的入站事务可按规定继续处理，而 TEE-associated 事务必须被拒绝。

因此不能简单理解为 `Bare = 所有 DMA 无条件通过`。

这是为了避免特定 TEE 请求在 I/O MPT 保护尚未建立的情况下被错误放行。

对于完整隔离系统，正常运行时应使用 `On` 模式，并在启用前完成 SDCL Rule 和 SDID Configuration 初始化。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 11.4.2 `command.OP` 与核心命令

规范定义了以下操作：

| OP | Operation         | 作用              |
| -- | ----------------- | --------------- |
| 1  | `IOFENCE`         | I/O 请求排序与配置同步   |
| 2  | `SET_SDCL_ENTRY`  | 配置设备分类规则        |
| 3  | `GET_SDCL_ENTRY`  | 查询分类规则          |
| 4  | `SET_SDCFG_ENTRY` | 配置 SDID 对应的 MPT |
| 5  | `GET_SDCFG_ENTRY` | 读取 SDID 配置      |
| 6  | `MPTINVAL`        | 设备侧 MPT 权限缓存失效  |

其命令接口使用 `status.BUSY` 和 `status.CODE` 报告完成状态。写入命令后不应立即假定操作已经结束。

例如参考伪代码：

```
int iompt_execute(struct iompt *iompt,
                  uint32_t cmd)
{
    int ret;

    ret = wait_until_not_busy(iompt);
    if (ret)
        return ret;

    /*
     * Required command operands must already
     * be programmed in data1/data2.
     */
    writel(cmd, iompt->base + 0x0c);

    ret = wait_until_not_busy(iompt);
    if (ret)
        return ret;

    uint32_t status =
        readl(iompt->base + 0x04);

    return decode_status(status);
}
```

这是说明硬件编程流程的示意代码，不是可直接编译的驱动。实际需要串行化共享的寄存器接口、遵循 MMIO Ordering，并根据平台规范设置超时。

尤其是 `data1` 和 `data2` 属于 64-bit 寄存器，规范并不保证任意 64-bit MMIO 写操作必然作为一个不可拆分的事务执行。

### 11.4.3 配置 SDCL Rule

例如，假设：

```
PCIe Device ID = 0x0100
SDID           = 1
IOMMU ID       = 0
```

RDSM 可以通过 `SET_SDCL_ENTRY` 建立分类规则。

其 `data1` 结构中包含：

```
SRC_IDT
SRC_IDM
TEE_FLT
SRC_ID
IOMMU_ID
SDID
```

这里 `SRC_IDT` 用于选择匹配的标识类型，`SRC_IDM` 用于选择 TOR、Unary、NAPOT 等匹配形式。

课程案例使用精确 Device ID 匹配。

一个很值得重视的规范细节是：

当多个 SDCL Rule 同时匹配同一事务时，实现允许选择其中任意一个匹配规则。

因此 RDSM 不应依赖某条规则天然拥有更高优先级来解决安全冲突。

安全配置应尽量做到规则不重叠，或者确保所有可能匹配的规则在安全策略上等价。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 11.4.4 配置 `SDID → MPT`

使用 `SET_SDCFG_ENTRY`：

```
SDID = 1

MPT_MODE = Smmpt43
PPN      = Linux_MPT_Root_PPN
```

对于 SDID 2：

```
SDID = 2

MPT_MODE = Smmpt43
PPN      = OPTEE_MPT_Root_PPN
```

这样即使两个 Domain 的 CPU 不在执行，I/O MPT Checker 仍可以根据事务所属 Domain 访问对应 MPT。

这就是设备 DMA 与 CPU 调度解耦的关键。

## 11.5 一次真正的 DMA 越权访问如何失败？

假设 Secure DDR：

```
PA:   0x9000_0000
Size: 64 KiB
Owner: OP-TEE
```

Linux 控制一个 DMA Engine，并尝试：

```
dma_descriptor.dst = 0x90000000;
dma_descriptor.len = 4096;
dma_descriptor.cmd = DMA_WRITE;
```

以下是概念性 Descriptor 格式，不代表某个具体控制器。

### 执行路径

1. Linux 把 DMA 请求提交给 Device。
2. Device 发起内存写事务，I/O Bridge 获得该请求的可信源标识。
3. SDCL 匹配规则，判定 `SDID = 1`。
4. IOMMU 完成 IOVA → SPA 转换；如果已使用物理地址或平台无 IOMMU，则按相应路径处理。
5. I/O MPT Checker 查询 Linux Domain 的 MPT。
6. 发现 Secure DDR 不允许 Linux Domain 写入。
7. I/O Bridge 中止事务，通过相应 I/O 错误路径报告。

与 CPU 访问冲突不同，这类 DMA 错误不应被直接理解为 Linux Hart 产生了一次 `scause` Trap。

它是设备/互连产生的事务错误。

例如：

- AXI Write 可以返回 `BRESP = SLVERR`。
- AXI Read 可以通过 `RRESP` 报告错误。
- PCIe 事务可能以 Unsupported Request、Completer Abort 或适当的错误机制报告。
- IOMMU 自身发起的访问失败，也需要对应 IOMMU 错误处理。

具体如何上报还取决于硬件实现。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



因此，在硬件调试时看到 DMA Timeout，不能只去分析 `scause`，而应检查整个 I/O 错误链路。

## 11.6 `MPTINVAL` 与 `IOFENCE`：两个最容易混淆的机制

这一节是 Chapter 11 最重要的内容之一。

先提出一个问题。

RDSM 修改了：

```
MPT_Linux[P] = RW
```

为：

```
MPT_Linux[P] = No Access
```

为什么 DMA 仍然可能依据旧权限执行？

原因是 I/O MPT Checker 可能存在权限缓存，而设备也可能有在途内存事务。

因此至少存在两类问题：

1. Permission Cache Staleness：旧 MPT 权限仍然被缓存。
2. In-flight Transaction：已授权或进入执行路径的旧 DMA 事务仍然没有完成。

### 11.6.1 `MPTINVAL`

`MPTINVAL` 用于同步 I/O MPT Checker 中的 MPT 权限状态。

它支持按照：

- 全局
- 特定 SDID
- 特定地址范围
- 特定 SDID + 地址范围

进行失效。

例如，我们只需要同步：

```
SDID = 1
PA   = 0x9000_0000
Size = 4 KiB
```

可以使用：

```
SDIDV = 1
SDID  = 1
PPNV  = 1
PPN   = 0x90000
S     = 0
```

其中：

```
PPN = PA >> 12
```

`S = 0` 表示按 4 KiB 范围指定失效。规范也支持通过 `S = 1` 编码 NAPOT 范围。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



但有一个关键点：

`MPTINVAL` 解决的是权限信息的同步，而不是一般意义上的“停止设备继续 DMA”。

### 11.6.2 `IOFENCE`

`IOFENCE` 确保先前已经由 I/O MPT Checker 处理的读写请求达到规定的全局排序点，同时可用于确保之前的 Checker 配置更新已被观察到。

它并不会自动命令设备停止生成新的 DMA 请求。

因此：

```
MPTINVAL != IOFENCE
IOFENCE  != Device Quiesce
```

这三种机制需要配合。

规范进一步要求：如果 SDCL/SDCFG 配置变更涉及 MPT Mode 或 Root PPN，应在 `IOFENCE` 后执行相应的 `MPTINVAL`，以同步旧 MPT 缓存状态。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 11.6.3 推荐的 DMA Revoke Protocol

以下是架构级参考流程，并不是规范要求所有平台必须照搬的固定调用顺序。

```
1. Stop new DMA submissions
          |
          v
2. Quiesce / Stop Device
          |
          v
3. Synchronize IOMMU and Device ATC
          |
          v
4. Revoke relevant MPT permissions
          |
          v
5. MPTINVAL on I/O MPT Checker(s)
          |
          v
6. Synchronize CPU MPT state
          |
          v
7. IOFENCE / Drain outstanding I/O
          |
          v
8. Verify device and memory quiescence
          |
          v
9. Memory Sanitization
          |
          v
10. Grant new Owner
```

具体实现中，一些操作可能需要重复或调整次序，例如先撤销映射、阻止新访问，再等待在途事务排空。安全性依赖于平台如何保证旧权限下已接受的访问不能在新 Owner 获得页面后生效。

这里的核心是证明如下条件：

\\[ Grant\_{new}(P) \Rightarrow OutstandingDMA\_{old}(P)=0 \\]

不能把“已经写了 MPT”当作“所有旧 DMA 已经停止”。

## 11.7 PCIe ATS：为什么还需要考虑设备自己的 Translation Cache？

在高性能 PCIe 平台中，设备可能支持 ATS（Address Translation Services）。

这意味着设备可以通过 IOMMU 查询地址转换结果，并把结果缓存到设备自己的 Address Translation Cache（ATC）。

于是完整结构可能是：

```
Device ATC
    |
    v
IOMMU Translation / Validation
    |
    v
I/O MPT Enforcement
    |
    v
Physical Memory
```

这里存在两个独立缓存：

```
Device ATC:
  IOVA -> Translated Address

I/O MPT Cache:
  SDID + SPA -> Permission
```

即使 I/O MPT 权限已经撤销，设备仍可能存在旧地址转换缓存。

但是，这并不意味着 ATS 天然能够绕过 I/O MPT。

规范明确区分不同 Translated Request 和 Translation Response 路径，并要求按相应地址阶段执行 MPT 权限检查。

例如，IOMMU 向设备返回 SPA 的 ATS Translation Response，要求检查返回范围的 MPT 访问权限；如果返回的是 GPA，则采用对应的后续地址转换检查路径。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



工程上必须检查：

- ATS Translation Request 的检查路径。
- 设备使用已缓存翻译的 Translated Request 路径。
- Device ATC 的失效与完成协议。
- IOMMU Translation Cache 的失效。
- I/O MPT 权限缓存失效。
- 所有可能绕过 IOMMU 的设备访问路径。

安全保障的关键不是设备有没有缓存，而是最终访问路径是否始终受可信策略约束。

## 11.8 多 Domain 共享设备的难题

我们考虑一个更复杂的场景：

```
PCIe Accelerator
       |
       +-- Linux Workload
       |
       +-- OP-TEE Workload
```

设备可以同时服务两个 Domain。

那么，SDCL 如何判断每个 DMA 请求属于哪个 Domain？

不能简单地按整个 Device ID 分类，因为两个 Workload 可能来自同一个物理设备。

可能需要进一步使用：

- 独立 PCIe Function。
- 平台认可的设备上下文标识。
- 不同的 IDE Stream。
- TEE-associated Metadata。
- 可信硬件执行的设备上下文隔离。

对于支持 TEE-associated Metadata 的平台，可以为同一个设备来源配置不同的分类规则。

但这是硬件能力和安全协议共同决定的，不是简单增加一个软件字段就能保证可信。

同时必须确保以下资源不会跨 Domain 泄漏：

- DMA Queue。
- Completion Queue。
- Device Registers。
- Interrupt State。
- Command Descriptor。
- Device Firmware State。

因为即使 Memory Access 被 I/O MPT 正确限制，共享设备内部仍可能存在数据泄漏、状态污染或 DoS。

这也是为什么多租户 Secure Device Assignment 往往需要结合 PCIe IDE、TDISP 和其他设备可信机制讨论。

## 11.9 面向实际 SoC Bring-up 的调试方法

假设 Linux 的一个 NVMe DMA 请求发生失败。

我建议按照下面的顺序定位：

| 检查点          | 关注内容                                  |
| ------------ | ------------------------------------- |
| Device       | Descriptor、IOVA、Direction、Length      |
| I/O Bridge   | Request Source、Transaction Attributes |
| SDCL         | 匹配的 Rule、SDID、IOMMU ID                |
| IOMMU        | Device Context、Translation Fault      |
| I/O MPT      | MPT Mode、PPN、权限与 Cache                |
| Interconnect | AXI/PCIe Error Response               |
| Firmware     | RDSM 配置日志、权限更新历史                      |

在 Palladium/ZeBu 这类预硅环境中，如果能观察到 I/O Bridge 到 Checker 的逻辑信号，最有价值的信号通常包括：

```
request_valid
source_id
selected_sdid
iova
translated_spa
access_type
mpt_check_result
abort_reason
response_valid
```

这些是建议的调试观测项，并不是规范规定的 RTL 信号名。

最终应该能够回答三个问题：

1. 设备的身份是否被正确识别？
2. 地址转换是否正确？
3. 所属 Domain 的最终物理访问权限是否正确执行？

### Chapter 11 — Staff Engineer 测试题

1\. 如果 Linux 完全控制某个 IOMMU 的页表，如何保证它不能 DMA 访问 OP-TEE 私有内存？

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

2\. 为什么 I/O MPT Checker 不能直接使用当前 Hart 的 mmpt CSR？

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

3\. 如果多个 SDCL Rule 同时匹配某 Device ID，是否可以默认使用优先级更高的规则？

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

4\. 设计一个支持 Linux 和 OP-TEE 同时使用同一 PCIe Accelerator 的隔离架构。

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

5\. \`MPTINVAL\` 与 \`IOFENCE\` 在语义上分别保障什么？

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

6\. DMA 已经发出并获得旧 MPT 权限，但 RDSM 随后撤销该页权限，如何保证页面不会被旧 DMA 污染？

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

7\. 为什么 IOMMU 的内部 Page Table Walk 也需要考虑 I/O MPT？

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

8\. 在 PCIe ATS 场景中，撤销 DMA Buffer 时需要同步哪些缓存和状态？

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

9\. Linux 遇到 DMA Timeout，但 CPU 没有 Access Fault，你如何定位是否发生了 I/O MPT Violation？

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

10\. 为 I/O MPT 设计安全 Boot Initialization，你会采用哪些默认状态和启用顺序？

Staff · SoC 安全 / DMA / IOMMU

查看答题要点&#x20;

# Chapter 12 — Domain Interrupt Isolation 与 AIA

## 12.1 本章目标：为什么 Memory Isolation 还不够？

到目前为止，我们已经建立了两套保护：

```
CPU Memory Access:
  SMMPT

Device Memory Access:
  I/O MPT
```

现在假设：

```
Domain A:
  Linux
  SDID = 1

Domain B:
  OP-TEE
  SDID = 2
```

我们将一个 Secure Crypto Engine 分配给 OP-TEE。

这个设备拥有：

```
MMIO Registers
DMA Engine
Interrupt Source
```

其中：

- MMIO 权限由 MPT 及系统访问控制机制保护。
- DMA 权限由 I/O MPT 等机制保护。
- 但设备完成计算后产生的 Interrupt 应该送到哪里？

如果 Crypto Engine 的中断被送给 Linux，就可能导致：

1. Linux 观察 Secure Operation 的完成时间。
2. Linux 干扰 Secure Device 的运行。
3. Secure Interrupt 被错误处理或丢失。
4. OP-TEE 无法正常获取设备完成通知。
5. Linux 通过频繁触发中断实施 DoS。

因此，Memory Isolation、Device Isolation 和 Interrupt Isolation 必须协同工作。

这一章将重点介绍 RISC-V `Smsdia`（Supervisor Domain Interrupt Assignment）如何在 AIA 基础上建立 Interrupt Domain 隔离。

## 12.2 先理解普通 RISC-V AIA 的工作过程

为了理解 Smsdia，先回顾 AIA 的核心组件。

| 组件    | 作用                         | Arm 类比                           |
| ----- | -------------------------- | -------------------------------- |
| PLIC  | 传统外部中断控制器                  | 部分类似传统 GIC Distributor           |
| APLIC | 高级平台中断控制与路由                | 设备中断源配置及路由                       |
| IMSIC | 接收并保存 MSI 的 Hart-local 控制器 | 部分类似 GIC Redistributor 的本地中断处理角色 |
| AIA   | 整体中断体系架构                   | GIC 架构整体                         |

以上是帮助理解的功能类比，并非完全等价。

AIA 支持两种主要的外部中断投递方式。

### 12.2.1 APLIC Direct Delivery

```
Device IRQ
    |
    v
APLIC
    |
    | Direct Interrupt Signal
    v
Target Hart
    |
    v
S-mode Trap Handler
```

这种方式无需 IMSIC。

APLIC 承担中断源管理、目标选择与直接投递职责。

### 12.2.2 APLIC + IMSIC MSI Delivery

```
Device IRQ
    |
    v
APLIC
    |
    | Generate MSI
    v
IMSIC of Target Hart
    |
    | Pending + Enable
    v
Hart External Interrupt
    |
    v
Interrupt Handler
```

MSI 本质上是向专用地址写入特定数据。

例如：

```
MSI Address = 0x2800_3000
MSI Data    = Interrupt ID 0x35
```

IMSIC 接收 MSI 后，将对应 Interrupt Identity 标记为 Pending，并在满足使能及优先级条件时向 Hart 发出外部中断信号。

这一机制已经由 AIA 1.0 定义。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org+1



## 12.3 问题出现了：同一个 Hart 会运行不同的 Supervisor Domain

假设 Hart 0 先运行 Linux，之后通过 RDSM 切换到 OP-TEE。

```
Time T0:
  Hart 0 -> Linux Domain

Time T1:
  Hart 0 -> OP-TEE Domain

Time T2:
  Hart 0 -> Linux Domain
```

如果 Hart 0 只有一个传统 Supervisor Interrupt File，那么 Linux 和 OP-TEE 就必须共用该文件，或者由 M-mode 负责模拟和切换中断状态。

这会产生两个问题。

问题一：中断状态的安全隔离。

Linux 不应该看到 OP-TEE 的 Pending Interrupt、Enable State 或属于其他安全域的中断配置。

问题二：中断的独立保存。

当 Linux 正在运行时，OP-TEE 的中断可能到达。

该中断不能被 Linux 错误处理，也不应该因为 OP-TEE 当前没有运行就丢失。

因此，Smsdia 引入了一个重要概念：

Supervisor Interrupt Domain。

它不是新的 CPU Privilege Mode，而是为不同 Supervisor Domain 提供独立中断状态及选择机制的架构抽象。

规范允许 IMSIC 中的一个 Supervisor Interrupt Domain 包含一个 S-level Interrupt File，以及可选的一个或多个 Guest Interrupt Files。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 12.4 Supervisor Domain 与 Supervisor Interrupt Domain 的区别

这是理解本章最重要的一点。

前面 SMMPT 中已经使用：

```
SDID = Supervisor Domain Identifier
```

Smsdia 又引入：

```
SIDN = Supervisor Interrupt Domain Number
```

二者不是一回事。

| 项目       | SDID    | SIDN                         |
| -------- | ------- | ---------------------------- |
| 核心用途     | 物理内存域标识 | 中断域选择                        |
| 主要关联     | MPT     | IMSIC/APLIC Interrupt Domain |
| 重要寄存器    | `mmpt`  | `msdcfg.SIDN`                |
| 管理方      | RDSM    | RDSM                         |
| 是否必须数值相同 | 否       | 否                            |

例如：

```
Linux:
  SDID = 1
  SIDN = 0

OP-TEE:
  SDID = 2
  SIDN = 3
```

当 Hart 0 运行 OP-TEE 时，RDSM 需要使：

```
Memory Domain:
  mmpt.SDID = 2
  mmpt.PPN  = OPTEE_MPT_ROOT

Interrupt Domain:
  msdcfg.SIDN = 3
```

此时 Hart 0 的内存访问受 OP-TEE MPT 约束，而它所使用的 Supervisor-level 外部中断状态来自选中的 Interrupt Domain 3。

需要注意，并不是每个 Supervisor Domain 都必须有一个独立的硬件 SIDN。规范允许更灵活的分配或软件模拟。

## 12.5 IMSIC 如何支持多个 Supervisor Interrupt Domain？

我们先看传统 IMSIC：

```
Hart 0 IMSIC
  |
  +-- M-level Interrupt File
  |
  +-- S-level Interrupt File
  |
  +-- Guest Interrupt File 1
  |
  +-- Guest Interrupt File 2
```

加入 Smsdia 后，可以理解为：

```
Hart 0 IMSIC
  |
  +-- M-level Interrupt File
  |
  +-- SIDN 0
  |    +-- S-level Interrupt File
  |    +-- Guest Interrupt Files (optional)
  |
  +-- SIDN 1
  |    +-- S-level Interrupt File
  |    +-- Guest Interrupt Files (optional)
  |
  +-- SIDN 2
       +-- S-level Interrupt File
       +-- Guest Interrupt Files (optional)
```

因此，一个 Hart 即使在不同时间执行不同 Domain，也不需要把所有 Domain 的外部中断 Pending State 混在同一个文件中。

### 12.5.1 IMSIC 中断文件的地址计算

根据 Smsdia 规范，在单 Hart Group 场景中，一个 Supervisor Interrupt File 的物理地址可以表示为：

\\[ Address = B+n\times 2^I+h\times 2^D \\]

其中：

- `B`：Supervisor Interrupt File 区域基地址。
- `n`：SIDN。
- `h`：Hart Number。
- `I`：Interrupt Domain 区域偏移的位移。
- `D`：Hart Index 区域偏移的位移。

举一个具体例子。

假设：

```
B    = 0x2800_0000
I    = 16
D    = 12
SIDN = 2
Hart = 3
```

那么：

```
SIDN Offset:
  2 << 16 = 0x20000

Hart Offset:
  3 << 12 = 0x3000

Final Address:
  0x2800_0000
+ 0x0002_0000
+ 0x0000_3000
= 0x2802_3000
```

这就是该示例配置下，Hart 3 的 SIDN 2 对应的 S-level Interrupt File 地址。

这里假设单一 Hart Group、相关位宽和地址对齐约束已经满足。实际 SoC 还必须按照 AIA 和 Smsdia 的完整地址布局检查。

这个设计的意义非常大：中断文件地址本身包含 Interrupt Domain 的隔离维度。

但注意，拥有 MSI 地址并不意味着任何设备都有权向它写入。设备事务还需要受到 SoC 对 MSI 目标地址的访问控制与路由保护。

## 12.6 APLIC 如何将不同设备的中断路由到不同 Domain？

以两个设备为例：

```
UART:
  Owner = Linux

Secure Crypto:
  Owner = OP-TEE
```

我们希望：

```
UART IRQ
   ->
Linux Interrupt Domain

Crypto IRQ
   ->
OP-TEE Interrupt Domain
```

在支持 Smsdia 的 APLIC 中，可以配置与 Supervisor Domain 对应的 Interrupt Domain。

在 MSI Delivery 模式下，APLIC 最终产生对应目标 Interrupt File 的 MSI 地址。

Smsdia 为 APLIC 的 `smsiaddrcfgh` 扩展了：

```
DXS - Domain Index Shift
DXW - Domain Index Width
```

这些参数支持将 Interrupt Domain Index 编码到 MSI 目标地址中。

以概念方式表示：

```
MSI Address
    =
Base Address
  + Hart Offset
  + Interrupt Domain Offset
  + Guest File Offset
```

这实现了：

```
Device IRQ Source
      |
      v
APLIC Domain
      |
      v
IMSIC SIDN
      |
      v
Selected Supervisor Domain
```

这里还要区分两件事：

- APLIC 的路由配置决定中断发往哪个中断文件或目标。
- \*\*`msdcfg.SIDN`\*\*决定某个 Hart 当前使用哪个 Supervisor Interrupt Domain 的中断状态。

改变 `mideleg` 并不能替代这两部分配置。

## 12.7 一次 Secure Crypto 中断的完整执行路径

我们构建一个真实的情景：

```
Hart 0:
  Running OP-TEE

Crypto Engine:
  Assigned to OP-TEE

OP-TEE SDID = 2
OP-TEE SIDN = 3
```

Crypto Engine 执行一个 AES 运算：

```
1. OP-TEE writes Crypto MMIO
2. Crypto Engine starts AES
3. Crypto Engine completes
4. Crypto Engine raises IRQ
```

后续流程为：

```
Crypto Engine
      |
      | IRQ Assert
      v
APLIC
      |
      | Target SIDN = 3
      | Target Hart = 0
      v
IMSIC: Hart 0 / SIDN 3
      |
      | Set Pending Interrupt ID
      v
mip.SEIP
      |
      | Interrupt Delegated to S-mode
      v
OP-TEE Trap Entry
      |
      v
OP-TEE IRQ Handler
      |
      | Acknowledge / Claim
      | Handle Crypto Completion
      v
Resume OP-TEE
```

这个流程成立的前提是，RDSM 已经正确建立 Interrupt Domain 关联、访问控制和所需的中断委托配置。

### 12.7.1 IMSIC Interrupt Claim：汇编层面

在使用 IMSIC 的 S-mode 中断处理程序中，可以使用 `stopei` 获取并 Claim 外部中断。

例如：

```
csrrw t0, stopei, zero
srli  t0, t0, 16
```

逐条分析：

第一条：

```
csrrw t0, stopei, zero
```

原子地读取 `stopei` 并执行其规定的读写副作用，用来获取并 Claim 当前最高优先级的外部中断。

第二条：

```
srli t0, t0, 16
```

右移 16 位，提取 Interrupt Identity。

随后 Handler 就可以依据该 ID 分发至对应的设备中断处理函数。

这里的 `stopei` 操作针对 `msdcfg.SIDN` 当前选中的 Supervisor Interrupt File。

因此，RDSM 选择错误的 SIDN，可能直接导致 Handler 看不到期望的中断，或者操作了错误的中断状态。

AIA 对 `stopei` 的 Claim 语义已有正式定义。

[image](https://www.google.com/s2/favicons?domain=https://docs.riscv.org\&sz=32)

docs.riscv.org



## 12.8 OP-TEE 没有运行时，Secure Interrupt 应该怎么办？

现在让场景变得更复杂。

```
Hart 0:
  Running Linux

OP-TEE:
  Not currently scheduled

Crypto Engine:
  Finishes its operation
```

此时 Crypto Engine 的中断仍然应该属于 OP-TEE。

但 Hart 0 目前选中的 Interrupt Domain 是 Linux。

那么，谁负责唤醒或调度 OP-TEE？

答案是 Smsdia 提供的 Pending Summary 和 Machine-level Notification 机制。

### 12.8.1 `msideip`：Machine Supervisor Interrupt Domain External Interrupt Pending

`msideip` 是 M-mode 可读取的 Pending Summary 寄存器。

它的每个 Bit 对应一个 Supervisor Interrupt Domain Number。

例如：

```
msideip = 0b0000_1000
                  ^
                  |
              SIDN = 3
              Pending
```

这表示 SIDN 3 对应的 Interrupt Domain 存在符合规范条件的 Pending External Interrupt。

注意，Bit 3 表示 SIDN 3，而不是 SDID 3。

### 12.8.2 `msideie`：Pending Summary Enable

`msideie` 用于指定哪些 Supervisor Interrupt Domain 的 Pending 状态可以产生 Machine Supervisor Domain External Interrupt。

例如：

```
msideie = 0b0000_1000
```

表示启用 SIDN 3 的相关通知。

### 12.8.3 `MSDEI`：Machine Supervisor Domain External Interrupt

Smsdia 引入了新的 Machine Supervisor Domain External Interrupt（MSDEI）。

规范将其分配到中断 Bit 14：

```
mip.MSDEIP
mie.MSDEIE
```

其中：

\\[ MSDEIP = \bigvee_i(msideip[i]\land msideie[i]) \\]

当满足中断使能、优先级及委托等条件时，这个事件可以使 RDSM 知道某个目前未运行的 Domain 有 Pending Interrupt。

然后 RDSM 可以决定是否调度该 Domain。

这个机制解决的不是设备 IRQ 本身的内容，而是：

如何通知调度者：另一个 Supervisor Domain 有事件需要处理。

规范也允许在受信任的场景下将 MSDEI 委托给特定的 S-mode 调度组件，但不能无条件把所有 Domain 的 Pending 信息暴露给普通不可信 Linux。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



### 12.8.4 完整时序

```
T0:
  Hart 0 = Linux
  Active SIDN = 0

T1:
  Crypto Engine raises IRQ
  IMSIC SIDN 3 becomes pending

T2:
  msideip[3] = 1

T3:
  msideie[3] = 1
  MSDEI becomes pending

T4:
  RDSM receives notification

T5:
  RDSM determines OP-TEE is runnable

T6:
  RDSM schedules OP-TEE on an eligible Hart

T7:
  Active SIDN = 3

T8:
  OP-TEE receives its pending external IRQ
```

需要说明，T5 到 T8 是 RDSM 的一种合理调度策略，不是规范规定只要产生 MSDEI 就必须立即切换到对应 Domain。

调度时机仍由 RDSM 根据优先级、资源占用和安全策略决定。

## 12.9 Domain Switching 时，Interrupt Context 如何切换？

这一节与 OP-TEE Porting 高度相关。

考虑同一个 Hart：

```
Linux Domain
     |
     | Domain Switch
     v
OP-TEE Domain
```

在 Memory Protection 方面，我们需要切换：

```
mmpt.SDID
mmpt.PPN
mmpt.MODE
```

在 Interrupt Protection 方面，可能需要切换：

```
msdcfg.SIDN
mideleg
Interrupt Enable State
Timer State
Guest Interrupt State
```

另外，完整 Supervisor Context 还包括：

```
sepc / sstatus / stvec / sscratch
satp
GPRs
Relevant S/H CSRs
```

但这些并不全都属于 Smsdia 新增的寄存器。有些是传统 Supervisor Context Switch 本来就需要维护的状态。

### 12.9.1 为什么不能只切换 SIDN？

因为即使选中了 OP-TEE 对应的 IMSIC 文件，也还需要确保：

- Interrupt Delegation Policy 正确。
- 不会把 Root-only Interrupt 送给不可信 Domain。
- `stvec` 指向正确的 Trap Handler。
- S-mode Interrupt Enable State 已恢复。
- Guest Interrupt File 选择正确。
- Timer Compare 和相关 CSR 不会继承另一个 Domain 的私有状态。

例如，`mideleg` 只决定特定类别的中断是否可以交给 S-mode，并不决定中断属于 Linux 还是 OP-TEE。

当 RDSM 对不同 Domain 使用不同中断策略时，`mideleg` 等状态就需要纳入上下文管理。

### 12.9.2 推荐的 Domain Switch 流程

假设从 Linux 切换到 OP-TEE。

```
1. Trap from Linux to RDSM
          |
          v
2. Disable/mask relevant interrupts
          |
          v
3. Save Linux supervisor context
          |
          v
4. Save relevant interrupt state
          |
          v
5. Configure OP-TEE MPT context
          |
          v
6. Configure msdcfg.SIDN
          |
          v
7. Restore OP-TEE trap / timer /
   delegation / interrupt context
          |
          v
8. Restore OP-TEE execution context
          |
          v
9. Resume OP-TEE via mret
```

在具体实现中，步骤 5–8 的精确顺序以及权限同步屏障必须根据 CSR 语义、Trap Masking 和平台安全条件来决定。

最核心的约束是：

在返回低特权级执行之前，Memory Domain、Interrupt Domain 和 CPU Context 必须已经构成同一个一致的安全上下文。

如果其中任一部分仍属于旧 Domain，就可能出现信息泄漏或执行错误。

### 12.9.3 RDSM 参考伪代码

```
void rdsm_switch_domain(
    struct domain_ctx *next)
{
    struct domain_ctx *prev =
        current_domain();

    /*
     * Execute in trusted M-mode context.
     * Interrupt and scheduling synchronization
     * are platform-specific.
     */
    mask_switch_sensitive_interrupts();

    save_supervisor_context(prev);
    save_interrupt_context(prev);

    /*
     * Validate and install memory domain.
     */
    install_mpt_context(next);

    /*
     * Select interrupt domain.
     */
    install_sidn(next->sidn);

    /*
     * Restore all domain-specific state.
     */
    restore_interrupt_context(next);
    restore_supervisor_context(next);

    set_current_domain(next);

    /*
     * Complete architecture-required
     * synchronization and return via mret.
     */
    resume_supervisor(next);
}
```

这同样是参考架构代码，不是现有 OpenSBI `sbi_domain` API。

这里真正难的不是 `mret` 本身，而是跨 Domain 切换时如何保持所有安全状态一致。

## 12.10 `mideleg`、`hgeip`、`mgeien` 的职责

下面几个机制容易混淆，需要分别建立概念。

### 12.10.1 `mideleg`

决定某些 Interrupt Cause 是否可以委托给 S-mode。

例如：

```
SEI (Supervisor External Interrupt)
STI (Supervisor Timer Interrupt)
SSI (Supervisor Software Interrupt)
```

但它并不决定一个设备属于哪个 Supervisor Domain。

Domain 的中断隔离还需要 APLIC/IMSIC 与 SIDN 的配合。

### 12.10.2 `hgeip` 与 `hgeie`

如果 Supervisor Domain 内还有 Hypervisor，例如：

```
Supervisor Domain A
    |
    v
Hypervisor
    |
    +-- Guest VM 0
    |
    +-- Guest VM 1
```

那么还需要处理 Guest External Interrupt。

AIA 与 H-extension 提供 `hgeip`、`hgeie` 等状态。

Smsdia 在多个 Supervisor Interrupt Domain 场景下，要求相应 Guest Interrupt State 与选中的 SIDN 关联。

这使不同的 Supervisor Domain 可以拥有独立的 Guest External Interrupt 状态。

### 12.10.3 `Smgeien` / `Ssgeien`

相关可选扩展进一步提供对 Guest Interrupt File 访问能力的控制。

例如 `mgeien` 可以限制某个 Domain 可以操作哪些 Guest Interrupt File。

可以把它理解为：

```
SIDN:
  Select interrupt domain

mgeien:
  Restrict accessible guest files
```

因此，即便一个 Hypervisor 位于某个 Supervisor Domain 内，也不应该自动拥有其他 Domain 的 Guest Interrupt File 访问权限。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 12.11 多 Hart 系统中的 Interrupt Routing

现在回到 16-Hart SoC。

假设：

```
Hart 0-7:
  Linux preferred

Hart 8-15:
  OP-TEE eligible
```

这里的 Affinity 只是调度偏好，不意味着每个 Hart 永久属于某个 Domain。

当 Secure Crypto Engine 完成任务时，RDSM 需要判断：

1. OP-TEE 是否当前已经在运行？
2. 是否存在能够处理该 IRQ 的 Hart？
3. 应该将 IRQ 投递到哪个 Hart 的 IMSIC？
4. 如果目标 Hart 未运行 OP-TEE，是否需要触发调度？
5. 是否需要迁移 Interrupt Affinity？
6. 是否存在旧 Pending Interrupt 没有处理？

这一场景涉及三个不同的维度：

```
Hart Selection
     +
Supervisor Domain Selection
     +
Interrupt File Selection
```

正确的架构不能把这三件事简单合并。

例如：

```
Hart 8, SIDN 3
```

和：

```
Hart 9, SIDN 3
```

虽然属于同一个 Interrupt Domain Number，但对应不同 Hart 的 Interrupt File。

如果把 MSI 投递到 Hart 8，而最终把 OP-TEE 调度到 Hart 9，那么必须考虑 Pending State 和中断路由之间的关系。

规范不会自动替 RDSM 完成完整的负载均衡或迁移策略。

这仍然属于系统软件的职责。

## 12.12 中断与内存保护如何协同？

我们之前已经讨论 I/O MPT。

这里把两者串起来。

假设 Crypto Engine 通过 MSI 发出中断：

```
Crypto Engine
     |
     | MSI Write
     v
IMSIC Address
```

本质上，这也是一次 Device-originated Memory Write。

因此其目标地址和发起者需要满足适当的设备访问控制要求。

若系统采用 I/O MPT 路径保护 MSI 写入，必须确保：

- Crypto Engine 有权访问 OP-TEE 的相应 IMSIC Interrupt File。
- Linux 设备无法任意写入 OP-TEE 的 Interrupt File。
- APLIC Configuration Registers 不允许被不可信 Linux 改写。
- IOMMU 中关于 MSI Translation 的设置不能绕过中断域隔离。

请注意：设备 MSI 路径在某些实现中可以有专门的中断重映射或互连处理方式，不应未经 SoC 验证就假定所有 MSI 都与普通 DMA 完全相同。

这一章的核心设计原则是：

中断的归属必须与设备的安全归属相匹配。

否则即使 DMA Memory Isolation 正确，Interrupt Routing 仍然可能成为攻击路径。

## 12.13 从实际 Linux 与 OpenSBI 源码学习 AIA

为了将规范与代码结合，建议研究以下几个现有源码位置：

| 组件          | 源码位置                                         | 主要内容                                |
| ----------- | -------------------------------------------- | ----------------------------------- |
| Linux IMSIC | `drivers/irqchip/irq-riscv-imsic-state.c`    | IMSIC Local State 和 Interrupt ID 管理 |
| Linux IMSIC | `drivers/irqchip/irq-riscv-imsic-platform.c` | MSI 分配、目标与 Affinity                 |
| Linux APLIC | `drivers/irqchip/irq-riscv-aplic-msi.c`      | APLIC MSI 投递                        |
| Linux APLIC | `drivers/irqchip/irq-riscv-aplic-direct.c`   | APLIC Direct Delivery               |
| OpenSBI     | `lib/sbi/sbi_trap.c`                         | M-mode Trap 和 Interrupt Dispatch    |

这些是可以直接查阅的实际源码位置，但其基础 AIA 支持不等价于已经完成 Smsdia 多 Domain 扩展。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub+2



其中，Linux IMSIC Driver 的 MSI Message 生成逻辑尤其值得研究。

例如，可以看到类似这样的逻辑：

```
msg->address_hi = upper_32_bits(msi_addr);
msg->address_lo = lower_32_bits(msi_addr);
msg->data = vec->local_id;
```

这段源码能够帮助理解：

MSI 并不是一次普通的软件函数调用，而是设备使用 Address + Data 表示一个中断投递请求。

Linux 中 `irq_set_affinity` 的相关实现还会涉及重新分配 MSI Vector、更新设备的 MSI Address/Data、处理迁移中的中断状态。

在基于 Smsdia 的多 Domain 系统中，还需要叠加 SIDN 维度以及更严格的安全授权控制。

## 12.14 如何设计一个可验证的 Interrupt Isolation 实验？

建议构建以下测试场景。

```
Hart 0:
  Linux Domain
  SIDN 0

Hart 1:
  OP-TEE Domain
  SIDN 1

APLIC:
  IRQ 32 -> Linux
  IRQ 33 -> OP-TEE
```

### 实验 A：正常 IRQ 路由

触发 IRQ 32：

```
Expected:
  Linux receives IRQ
  OP-TEE receives nothing
```

触发 IRQ 33：

```
Expected:
  OP-TEE receives IRQ
  Linux receives nothing
```

### 实验 B：Inactive Domain

将 Hart 1 切换出 OP-TEE。

再触发 IRQ 33：

```
Expected:
  OP-TEE Interrupt File retains pending state
  RDSM can observe pending summary
  Linux must not process OP-TEE IRQ
```

随后重新调度 OP-TEE，验证中断能够被正确 Claim 和处理。

### 实验 C：非法 MSI 注入

尝试让不可信设备向 Secure Interrupt File 发出未经授权的 MSI。

预期：

```
Access rejected by appropriate
trusted access-control mechanism
```

或通过平台定义的安全处理路径报告故障。

这里的关键是验证保护机制，而不是假设所有拒绝都以同一种错误码报告。

### 实验 D：Domain Switching 压力测试

让 Linux 与 OP-TEE 在同一个 Hart 上频繁切换，同时随机触发两个 Domain 的中断。

需要验证：

- Interrupt 不被错误 Domain Claim。
- 不发生未经解释的 Lost Interrupt。
- Pending State 不跨 Domain 泄漏。
- 不出现不合理的 Duplicate Handling。
- 中断状态在 Hart 切换后仍保持一致。
- Root-only Interrupt 不会错误委托给不可信 Domain。

### 实验 E：中断状态调试

在受信任的 M-mode 调试环境下采集：

```
Hart ID
Current SDID
Current SIDN

msdcfg
mmpt
mideleg
mie / mip

msideip
msideie

mepc
mcause
```

在 S-mode 侧，还需要适当采集：

```
sie / sip
stvec
sepc
scause
stopei / IMSIC state
```

需要注意，读取某些中断相关寄存器可能具有副作用，尤其不能把 Claim 操作当作普通无副作用的日志采样。

在调试过程中，应先确认具体 CSR 的读取语义。

## 12.15 Chapter 12 总结

本章最重要的五个知识点：

1. AIA 解决外部中断的管理、路由和投递，Smsdia 在此基础上增加 Supervisor Domain 维度。
2. SDID 与 SIDN 不是同一个概念。 前者用于物理内存保护，后者用于中断域选择。
3. 一个 Domain 没有运行，也可能有 Pending Interrupt。 `msideip`、`msideie` 和 MSDEI 使 RDSM 能观察此类事件。
4. `mideleg` 不是 Device Ownership 机制。 它只控制中断委托，不能替代 APLIC/IMSIC Domain Assignment。
5. Domain Switch 必须维护完整安全上下文。 不仅要切换 MPT，还必须保证相关 Trap、Interrupt、Timer、Guest State 等与目标 Domain 一致。

### Chapter 12 — Staff Engineer 测试题

1\. 为什么传统 AIA 已经支持 IMSIC Guest Interrupt File，仍然需要 Smsdia？

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

2\. SDID 和 SIDN 有什么区别？能否直接用 SDID 作为 SIDN？

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

3\. 一个 Secure Device IRQ 到达时 OP-TEE 没有运行，RDSM 如何知道它需要被调度？

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

4\. 为什么仅仅修改 mideleg 无法实现两个 S-mode Domain 之间的中断隔离？

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

5\. 在 Hart 0 从 Linux 切换到 OP-TEE 时，你需要管理哪些 Interrupt/Trap 上下文？

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

6\. 一个 MSI 被错误地发送到 Linux Interrupt File，但设备属于 OP-TEE。你会从哪些硬件模块定位？

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

7\. 同一 SIDN 对应 Hart 0 与 Hart 1 的 Interrupt File 是否是同一个物理中断文件？

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

8\. 如果某 SoC 只有两个硬件 Supervisor Interrupt Domain，却要运行八个 Supervisor Domain，应该如何设计？

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

9\. Linux 可以自由修改一个安全设备的 MSI Target Address，会有什么风险？如何防护？

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

10\. 设计一个可证明 Linux 和 OP-TEE 中断状态隔离的验证体系。

Staff · Interrupt / AIA / Domain Isolation

查看答题要点&#x20;

# Part III 阶段总结：建立完整的 SoC Domain 隔离模型

通过这三章，我们实际上已经将 SMMPT 从一个 CPU Memory Protection 机制，扩展为更完整的 SoC 安全隔离架构。

可以用下面这张概念图把它们串起来：

三章可以总结为三个安全不变量：

| Chapter        | 核心安全不变量                             |
| -------------- | ----------------------------------- |
| 10 — Memory    | 新 Owner 获权时，旧访问者及旧事务必须已经安全退出        |
| 11 — DMA       | 所有设备内存事务必须满足其可信归属 Domain 的物理权限策略    |
| 12 — Interrupt | 中断必须归属于正确的 Domain，不能泄漏、错误处理或绕过受信任调度 |

理解了这三条不变量，后续看到真实的 RTL、OpenSBI 代码、DMA Fault、Domain Switch 异常，就可以从安全边界出发定位问题，而不是仅仅查找某个寄存器是否配置正确。

## 推荐的进一步实践

为了强化这部分的理解，我建议把下面三个小项目作为阶段性成果：

1. Memory Ownership Manager： 实现一个简化的 RDSM 页面状态机，支持 Private、Share、Revoke、Transfer，并注入并发和失败场景。
2. I/O MPT Policy Simulator： 输入 Device ID、SDID、IOVA、MPT 权限，输出 DMA Allow/Deny 决策，并模拟权限失效与在途事务。
3. Supervisor Interrupt Domain Scheduler： 模拟多个 Domain、多个 SIDN、多个 Hart 的 Pending Interrupt 和 Domain Switching。

这三个项目都可以先采用软件模型验证安全不变量，不必一开始就依赖昂贵的仿真平台或尚不完整的硬件扩展实现。

至此，Part III 的 Chapter 10、11、12 已完成。按照原定大纲，下一章是 Chapter 13 — Debug、Trace、QoS 与 Confidential Computing。它将从安全边界的角度，进一步讨论调试权限、共享硬件资源及侧信道相关问题。
