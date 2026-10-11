# RISC-V Architecture 系统学习 · 12：IOMMU

## 0. 先给结论：IOMMU 的本质是什么？

你可以先把 **IOMMU = “给 DMA Device 使用的 MMU”**。

CPU 的 MMU 解决：

```text
CPU VA
   |
   | MMU
   v
PA
   |
   v
DRAM
```

IOMMU 解决：

```text
Device DMA Address / IOVA
        |
        | IOMMU
        v
       PA
        |
        v
       DRAM
```

所以它首先解决的不是 CPU 虚拟地址问题，而是：

> **限制一个 DMA-capable device 到底能够访问哪些系统内存，并为设备执行地址转换。**

如果没有 IOMMU：

```text
Device
   |
   | DMA PA = 0x8000_0000
   v
DRAM
```

只要设备能发出这个物理地址，它理论上就可能直接读写这块内存。

CPU 的：

```text
satp
Sv39
PTE
U/S privilege
```

**都挡不住 DMA。**

因为 DMA 根本没有经过 CPU MMU。

IOMMU 插进来以后：

```text
Device
   |
   | IOVA
   v
+---------+
| IOMMU   |
|         |
| page    |
| tables  |
+---------+
   |
   | PA
   v
DRAM
```

现在：

```text
Device A → 只能访问 Memory A
Device B → 只能访问 Memory B
Guest VM → 只能 DMA 到 Guest VM memory
```

这就是理解整个 IOMMU 的核心。

RISC-V 当前 ratified IOMMU Base Architecture 是 **1.0**；官方当前文档版本为 **v1.0.1 / Revised 2026-02-22**，另外包含 QoS ID、non-leaf PTE invalidation、address-range invalidation 等 ratified 扩展。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/iommu_preface.html?utm_source=chatgpt.com)

---

# 1. 为什么已经有 MMU，还需要 IOMMU？

这是第一道必须真正理解的问题。

假设：

```text
CPU
 |
 | MMU
 v
Physical Memory

PCIe NIC
 |
 | DMA
 +------------------> Physical Memory
```

Linux 中某进程：

```text
Process A
VA 0x400000
   |
  MMU
   v
PA 0x90000000
```

CPU 对 `0x400000` 的访问会：

```text
VA → MMU → PA
```

但是网卡 DMA：

```text
NIC
  |
  | DMA PA = 0x90000000
  v
DRAM
```

根本不经过 CPU MMU。

因此：

```text
CPU MMU
    │
    └── 控制 CPU memory access

IOMMU
    │
    └── 控制 Device DMA memory access
```

这两者的角色必须严格分开。

---

# 2. IOMMU 主要解决四类问题

理解这四类场景，后面的寄存器和 page table 就都有意义了。

| 场景 | IOMMU 作用 |
|---|---|
| DMA isolation | 防止 device DMA 任意系统内存 |
| DMA address translation | IOVA → PA |
| Virtualization | Guest Device DMA → Guest memory |
| Shared Virtual Address | Device 与 CPU process 使用相同/相关 VA |

其中最重要的其实是：

```text
Security
+
Virtualization
```

例如一个 PCIe 设备发生 bug：

```c
dma_addr = 0;
dma_len  = 4GB;
```

没有 IOMMU：

```text
Device
 |
 +-------> Kernel
 |
 +-------> Other process
 |
 +-------> Hypervisor
 |
 +-------> Other VM
```

理论上都可能遭到破坏。

有 IOMMU：

```text
Device
   |
   v
IOMMU
   |
   +-- allowed 0x10000000~0x10ffffff → OK
   |
   +-- other address              → FAULT
```

RISC-V Server SoC 规范甚至要求受其约束的 DMA-capable peripherals / PCIe root ports 由 IOMMU 管理，其理由就是 DMA isolation、device passthrough 和 shared virtual addressing 等场景。[RISC-V Documentation](https://docs.riscv.org/reference/server-soc/v1.0/server_soc_requirements.html?utm_source=chatgpt.com)

---

# 3. 先建立 RISC-V IOMMU 总体架构图

这是这一章最重要的一张图。

```text
                         CPU / Hypervisor
                              |
                              |
                     configure IOMMU
                              |
                              v
                    +-------------------+
                    |   RISC-V IOMMU    |
                    |                   |
Device DMA -------->| Device ID         |
IOVA                 |       |           |
                     |       v           |
                     |      DDT          |
                     |       |           |
                     |       v           |
                     | Device Context    |
                     |       |           |
                     |       +----------+
                     |       |
                     |       | optional Process ID
                     |       v
                     |      PDT
                     |       |
                     |       v
                     | Process Context
                     |       |
                     |       v
                     | First-stage PT
                     |       |
                     |     IOVA
                     |       |
                     |       v
                     |      GPA
                     |       |
                     | Second-stage PT
                     |       |
                     |       v
                     |      SPA
                     +-------+-----------+
                             |
                             v
                            DRAM
```

以后看到 RISC-V IOMMU，只要脑中有这条链：

```text
Device ID
   ↓
DDT
   ↓
Device Context
   ↓
Process ID (optional)
   ↓
PDT
   ↓
Process Context
   ↓
First-stage translation
   ↓
Second-stage translation
```

整个架构就不会乱。

---

# 4. 这里先建立几个最重要的名词

## Device ID

IOMMU 首先必须回答：

> 这次 DMA 是哪个设备发出来的？

所以每个 DMA transaction 都需要关联一个：

```text
device_id
```

例如 PCIe 中通常可以来自：

```text
Bus
Device
Function

→ Requester ID / RID
```

RISC-V IOMMU 允许 `device_id` 最大到 24 bit；PCIe 场景中可对应 RID，并可结合 hierarchy/segment 信息。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20230605/iommu_intro.html?utm_source=chatgpt.com)

---

## IOVA

IOVA：

> **I/O Virtual Address**

例如：

```text
NIC DMA:

IOVA = 0x1000_0000
```

经过 IOMMU：

```text
IOVA 0x1000_0000
       |
       v
     IOMMU
       |
       v
PA   0x8A00_0000
```

所以：

```text
CPU:     VA   → PA
Device:  IOVA → PA
```

---

# 5. 为什么不能所有设备共用一张 page table？

假设：

```text
NIC
GPU
NVMe
USB
```

都连接到 IOMMU。

显然我们不希望：

```text
NIC  DMA mapping
GPU  DMA mapping
NVMe DMA mapping
```

完全相同。

于是 RISC-V IOMMU 引入：

```text
device_id
     |
     v
Device Directory Table
     |
     v
Device Context
```

也就是：

```text
device_id = 10
        ↓
Device Context #10
        ↓
NIC address space

device_id = 20
        ↓
Device Context #20
        ↓
GPU address space
```

这就是 **Device Context，DC**。

---

# 6. DDT：Device Directory Table

DDT 可以理解成：

> **“device_id → Device Context”的查找表。**

结构很像 page table radix tree：

```text
               ddtp
                 |
                 v
          +-------------+
device_id |     DDT     |
--------->| radix tree  |
          +-------------+
                 |
                 v
        Device Context
```

RISC-V IOMMU 支持 1/2/3-level DDT，由 `ddtp` 控制。`ddtp` 的 `iommu_mode` 定义了 `Off`、`Bare`、`1LVL`、`2LVL`、`3LVL`；其中 `Off` 禁止 inbound transaction，而 `Bare` 不做地址翻译和保护。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_registers.html)

因此 `ddtp` 很容易理解：

```text
CPU:
satp
 ↓
Page Table

IOMMU:
ddtp
 ↓
Device Directory Table
```

不过千万别说：

```text
ddtp == satp
```

它们指向的数据结构用途完全不同。

`ddtp` 是：

```text
device_id → Device Context
```

而 `satp` 是：

```text
VA → PA
```

---

# 7. Device Context 是整个 IOMMU 的核心

找到 Device Context 后，IOMMU 才知道：

```text
这个设备属于谁？

它允许使用什么地址转换？

它的 first-stage page table 在哪里？

它的 second-stage page table 在哪里？

是否允许 ATS？

是否允许 PRI？

是否有 process context？

MSI 怎么处理？
```

可以概念化为：

```c
struct device_context
{
    translation_control;

    first_stage_context;
    second_stage_context;

    process_directory_pointer;

    address_space_ids;

    ats_control;
    pri_control;

    msi_translation_control;
};
```

当然实际 spec 的 layout 更复杂。

目前 DC 有：

```text
Base format      32 Bytes
Extended format  64 Bytes
```

Extended format 主要增加 MSI translation 相关能力。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_data_structures.html?utm_source=chatgpt.com)

---

# 8. 第一种最简单场景：Host Device

先暂时不考虑虚拟机。

例如 Linux 管理一个网卡：

```text
Linux
 |
 +-- NIC driver
 |
 +-- IOMMU driver
```

Linux 给 NIC 建立：

```text
IOVA                 PA

0x10000000  -----> 0x80000000
0x10001000  -----> 0x91234000
0x10002000  -----> 0xA3000000
```

于是：

```text
NIC
 |
 | DMA 0x10001000
 v
IOMMU
 |
 | First-stage
 v
0x91234000
 |
 v
DRAM
```

这里：

```text
First-stage enabled
Second-stage = Bare
```

这也是理解 RISC-V IOMMU 最简单的起点。

---

# 9. First-stage translation：`iosatp`

这时出现一个很像 `satp` 的东西：

```text
iosatp
```

可以直接这样记：

```text
CPU MMU:
satp

I/O MMU:
iosatp
```

它描述：

```text
first-stage page-table root
+
translation mode
```

所以：

```text
IOVA
 |
 | iosatp
 v
First-stage Page Table
 |
 v
PA
```

如果没有 virtualization：

```text
IOVA → SPA
```

这里的 SPA：

> Supervisor Physical Address

可以暂时理解成最终 system physical address。

非常重要的一点是：

**RISC-V IOMMU 的 page-based address translation 重用了 RISC-V Privileged Architecture 定义的 page-table 格式。**

例如可支持：

```text
Sv39
Sv48
Sv57
...
```

所以你之前第六阶段学的 Sv39，并没有白学。

RISC-V IOMMU 还明确允许软件视需要让 CPU MMU 与 IOMMU 使用共同 page table，或者使用独立 page table。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_intro.html?utm_source=chatgpt.com)

---

# 10. 第二种场景：Virtual Machine Device Passthrough

现在问题变得有意思了。

假设：

```text
Host
 |
 +-- VM1
      |
      +-- passthrough NIC
```

VM1 认为自己的物理内存是：

```text
GPA
```

但是实际上：

```text
GPA
 |
 | Hypervisor
 v
SPA
```

CPU 上我们已经学过：

```text
Guest VA
   |
   | VS-stage
   v
GPA
   |
   | G-stage
   v
SPA
```

那么设备呢？

完全一样的思想：

```text
Device IOVA
      |
      | First-stage
      v
     GPA
      |
      | Second-stage
      v
     SPA
```

所以：

```text
           CPU                    Device

Guest VA                         IOVA
   |                              |
   | VS-stage                     | first stage
   v                              v
  GPA                            GPA
   |                              |
   | G-stage                      | second stage
   v                              v
  SPA                            SPA
```

这就是 RISC-V IOMMU 的 **two-stage translation**。官方规范明确规定：guest-controlled device 可以由 Guest OS 提供 first-stage page table 做 IOVA→GPA，再由 hypervisor 提供 second-stage page table 做 GPA→SPA。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_data_structures.html?utm_source=chatgpt.com)

---

# 11. `iohgatp`：IOMMU 版本的 `hgatp`

你之前学习 H Extension 时已经知道：

```text
hgatp
```

控制 CPU virtualization 的：

```text
GPA → SPA
```

IOMMU 中有：

```text
iohgatp
```

所以非常容易记：

```text
CPU H-extension:
hgatp

IOMMU:
iohgatp
```

于是最重要的一组关系就是：

```text
           CPU                      IOMMU

satp / vsatp                      iosatp
     |                               |
     v                               v
VA → PA / GPA                    IOVA → GPA

hgatp                            iohgatp
  |                                 |
  v                                 v
GPA → SPA                        GPA → SPA
```

这张表建议你记下来。

---

# 12. 为什么还需要 Process Context？

这是 RISC-V IOMMU 第二个比较难的概念。

你可能会问：

> Device Context 已经能够区分 GPU 和 NIC 了，为什么还要 Process Context？

考虑 GPU。

一个 GPU 可能同时服务：

```text
Process A
Process B
Process C
```

我们希望 GPU 能直接使用：

```text
Process A VA

Process B VA
```

而不是：

```text
用户 VA
   ↓
driver 转成某个独立 DMA IOVA
   ↓
GPU
```

于是设备 transaction 除了：

```text
device_id
```

还带：

```text
process_id
```

PCIe 中这通常对应：

```text
PASID
= Process Address Space ID
```

RISC-V IOMMU 定义的 `process_id` 最大可到 20 bits。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_intro.html?utm_source=chatgpt.com)

于是：

```text
GPU
 |
 | device_id = GPU
 | process_id = Process A
 |
 v
IOMMU
```

IOMMU 查找：

```text
device_id
   |
   v
DDT
   |
   v
Device Context
   |
   | process_id
   v
PDT
   |
   v
Process Context
   |
   v
Process A page table
```

---

# 13. PDT：Process Directory Table

结构现在就非常清楚了：

```text
device_id
   ↓
DDT
   ↓
Device Context
   ↓
process_id
   ↓
PDT
   ↓
Process Context
```

PDT 也是 radix tree。

RISC-V 定义了：

```text
PD8
PD17
PD20
```

分别支持不同宽度的 process ID / PDT 层级；PD20 对应最多 20-bit `process_id` 和三级 PDT。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_data_structures.html?utm_source=chatgpt.com)

Process Context 最重要的内容之一就是：

```text
first-stage page-table root
+
PSCID
+
translation attributes
```

因此一个 GPU 可以：

```text
GPU + PID 100
        ↓
Process Context A
        ↓
Process A page table

GPU + PID 200
        ↓
Process Context B
        ↓
Process B page table
```

---

# 14. 这实际上就是 SVA

这类技术一般叫：

> **Shared Virtual Addressing，SVA**

最终达到：

```text
CPU Process
     |
     | VA = 0x12345678
     v
CPU MMU

GPU
     |
     | VA = 0x12345678
     v
IOMMU
```

两边看到逻辑上一致的 address space。

对 GPU / accelerator 特别重要。

例如：

```c
float *p = malloc(...);

gpu_run(p);
```

理想模型下 GPU 不必再看到一个完全不同的：

```text
dma_address
```

而可以关联这个 process address space。

这也是为什么现代 IOMMU 已经远远不只是：

> “DMA firewall”。

它逐渐变成了：

> **CPU + Device shared virtual-memory infrastructure。**

---

# 15. 到这里，可以把地址翻译过程画完整

这是这一阶段最值得真正掌握的一张图。

```text
DMA Request
{
    device_id,
    process_id,
    IOVA,
    Read/Write/Execute...
}
        |
        v
+----------------------+
|       IOMMU          |
+----------------------+
        |
        | device_id
        v
       DDT
        |
        v
 Device Context
        |
        +---------------------------+
        |                           |
  process_id valid?                no
        |                           |
       yes                          |
        |                           |
        v                           |
       PDT                          |
        |                           |
        v                           |
 Process Context                    |
        |                           |
        +-------------+-------------+
                      |
                      v
              First-stage context
                   iosatp
                      |
                 IOVA → GPA
                      |
                      v
                 Second-stage
                  iohgatp
                      |
                  GPA → SPA
                      |
                      v
                     DRAM
```

如果你能脱离资料把这张图画出来，**RISC-V IOMMU 主体架构已经掌握 60% 以上。**

---

# 16. PSCID 和 GSCID 是干什么的？

你看到：

```text
PSCID
GSCID
```

第一反应可以类比：

```text
ASID
VMID
```

虽然语义不能完全写成等号，但这是非常好的 mental model。

### PSCID

Process Soft Context ID：

```text
PSCID
   |
   +-- identifies first-stage address space
```

主要方便：

```text
IOMMU translation cache tagging
+
selective invalidation
```

对应思路：

```text
CPU:
ASID + VA

IOMMU:
PSCID + IOVA
```

---

### GSCID

Guest Soft Context ID：

```text
GSCID
   |
   +-- identifies guest / second-stage address space
```

可以类比：

```text
VMID
```

所以：

```text
First-stage cache:

PSCID + IOVA
       ↓
translation


Second-stage cache:

GSCID + GPA
       ↓
translation
```

RISC-V IOMMU 1.0 支持 20-bit PSCID 和 16-bit GSCID。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/iommu_intro.html?utm_source=chatgpt.com)

---

# 17. IOMMU 当然也需要自己的 TLB

如果每次 DMA：

```text
Device
  ↓
DDT walk
  ↓
PDT walk
  ↓
First-stage walk
  ↓
Second-stage walk
```

性能显然不能接受。

所以 IOMMU 会缓存。

RISC-V 规范把这些缓存统称为：

> **IOATC — IOMMU Address Translation Caches**

可能缓存：

```text
DDT entries
PDT entries
first-stage translations
second-stage translations
MSI translations
```

于是：

```text
Device
   |
   v
 IOATC hit
   |
   v
 SPA
```

而不需要每次 page walk。

---

# 18. 那修改 Page Table 以后怎么办？

这个问题与你已经学过的：

```text
CPU TLB
+
SFENCE.VMA
```

完全同源。

CPU：

```text
modify PTE
   |
   v
SFENCE.VMA
   |
   v
old TLB entry invalid
```

IOMMU：

```text
modify IOMMU structure / PTE
   |
   v
send invalidation command
   |
   v
IOATC invalidated
```

RISC-V IOMMU 主要有两类：

```text
IODIR
IOTINVAL
```

其中：

```text
IODIR.INVAL_DDT
IODIR.INVAL_PDT
```

用于 Device/Process directory cache。

而：

```text
IOTINVAL.VMA
```

针对 first-stage translation。

```text
IOTINVAL.GVMA
```

针对 second-stage translation。

规范明确规定软件修改这些内存结构后，需要使用相应 IOMMU command 与 IOATC 同步，因为 IOATC 不会自动观察普通 hart store 对这些结构的修改。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_data_structures.html?utm_source=chatgpt.com)

这和 CPU：

```text
SFENCE.VMA
HFENCE.GVMA
```

背后的设计思想完全一致。

---

# 19. Command Queue

这些命令不是让 CPU：

```text
write some command register
```

一个一个同步执行。

现代 IOMMU 使用：

```text
Command Queue
```

简称：

```text
CQ
```

模型：

```text
Software
   |
   | write commands
   v
+--------------+
| Command Queue|
+--------------+
   ^
   |
 IOMMU consumes
```

例如：

```text
IOTINVAL.VMA
IOTINVAL.GVMA
IODIR.INVAL_DDT
IODIR.INVAL_PDT
IOFENCE.C
```

所以 CQ 可以理解成：

> **software → IOMMU 的异步 command channel。**

相关寄存器：

```text
cqb    Command Queue Base
cqh    Command Queue Head
cqt    Command Queue Tail
cqcsr  Command Queue Control/Status
```

每个 command entry 是 16 bytes。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_registers.html?utm_source=chatgpt.com)

---

# 20. IOFENCE 为什么存在？

考虑：

```text
Software:
update PTE
   ↓
submit IOTINVAL
   ↓
继续后面的操作
```

IOTINVAL 是异步 command。

软件什么时候知道：

> invalidation 真正完成了？

于是需要：

```text
IOFENCE.C
```

你可以类比：

```text
memory ordering barrier
```

但它针对的是：

> **IOMMU command completion/order。**

概念：

```text
Command A
Command B
IOTINVAL
       |
       v
IOFENCE.C
       |
       +---- previous commands committed/completed
```

这在做：

```text
unmap DMA buffer
device teardown
VM reassignment
```

时尤其重要。

---

# 21. Fault Queue

IOMMU 也会发生类似 MMU page fault 的问题。

例如：

```text
NIC
 |
 | DMA IOVA = 0x12340000
 v
IOMMU
 |
 +-- no valid PTE
 |
 v
FAULT
```

但这里不是 CPU instruction，所以不能：

```text
trap → stvec
```

直接处理。

于是 IOMMU 写入：

```text
Fault Queue
```

简称：

```text
FQ
```

模型：

```text
Device DMA
   |
   v
IOMMU
   |
   | fault
   v
+------------+
| Fault Queue|
+------------+
      |
      | interrupt
      v
IOMMU driver
```

寄存器包括：

```text
fqb
fqh
fqt
fqcsr
```

每个 fault record 是 32 bytes。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_registers.html?utm_source=chatgpt.com)

---

# 22. 所以 CQ / FQ 很容易记

```text
CPU / Software
       |
       | command
       v
      CQ
       |
       v
     IOMMU
       |
       | fault
       v
      FQ
       |
       v
Software
```

也就是：

```text
CQ:
Software → IOMMU

FQ:
IOMMU → Software
```

这个方向感非常重要。

---

# 23. Page Request Queue 又是什么？

这是进入高级功能：

```text
PRI
```

之后才需要理解的。

PCIe PRI：

> **Page Request Interface**

假设 GPU 使用 Process A 的 VA：

```text
GPU
 |
 | VA 0x100000
 v
IOMMU
```

但这一页当前：

```text
not resident
```

传统做法：

```text
FAULT
DMA failed
```

高级 SVA 场景希望：

```text
Device
  |
  | "I need page X"
  v
IOMMU
  |
  v
Page Request Queue
  |
  v
OS memory manager
  |
  | bring page in / map page
  v
Device retry
```

这就是：

```text
PRI
```

带来的 pageable shared virtual memory 能力之一。

RISC-V IOMMU 把 Page Request 放入：

```text
PQ
= Page Request Queue
```

相关寄存器：

```text
pqb
pqh
pqt
pqcsr
```

每条 page request 为 16 bytes。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_data_structures.html?utm_source=chatgpt.com)

---

# 24. ATS 又是什么？

另外一个经常和 IOMMU 一起出现的词：

> **ATS = Address Translation Service**

正常：

```text
Device
 |
 | every DMA
 v
IOMMU translation
```

这仍然有 overhead。

某些高性能 PCIe device 自己有：

```text
ATC
= Address Translation Cache
```

也可以粗略理解成：

> **Device-side TLB**

Device 可以向 IOMMU 发：

```text
ATS Translation Request
```

例如：

```text
"IOVA 0x1000 对应什么 physical translation？"
```

IOMMU 返回 translation。

Device 缓存：

```text
IOVA → translated address
```

随后发：

```text
Translated Request
```

从而减少 IOMMU page-walk 等开销。

RISC-V IOMMU 1.0 标准支持 PCIe ATS 和 PRI；Device Context 中有相应 enable/control 位。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_intro.html?utm_source=chatgpt.com)

---

# 25. ATS / PRI / SVA 三者关系

这是很容易混的一组概念。

可以这样理解：

```text
SVA
Shared Virtual Addressing
        |
        +--------------------+
        |                    |
       ATS                  PRI
        |                    |
        v                    v
Device cache             Device asks OS
translations             to resolve pages
```

ATS 解决：

> **地址转换结果如何让 Device 自己缓存？**

PRI 解决：

> **Device 遇到 unresolved/not-present page 时，如何请求 OS 帮它处理？**

SVA 是更高层目标：

> **CPU process 和 Device 能共享 virtual address space。**

---

# 26. IOMMU 与 MSI：为什么这两个东西会发生关系？

你刚刚上一阶段学习了 AIA。

你知道 MSI 本质上是：

```text
Memory Write
```

例如：

```text
Device
 |
 | store value to interrupt file address
 v
IMSIC
```

那么虚拟化就出现问题了。

Guest 认为：

```text
Guest IMSIC address = GPA X
```

但是实际硬件 IMSIC：

```text
SPA Y
```

所以：

```text
Device MSI
     |
     | GPA X
     v
   IOMMU
     |
     | MSI translation
     v
real IMSIC interrupt file
SPA Y
```

因此 RISC-V：

```text
AIA
+
IOMMU
```

在 virtualization 场景需要配合。

官方 AIA 规范明确规定，IOMMU 可以通过 MSI page table 把 guest 所认为的 MSI GPA 转换到真实 IMSIC guest interrupt file，从而允许设备 MSI 直接送入 VM，而不需要每次由 hypervisor 软件介入。[RISC-V Documentation](https://docs.riscv.org/reference/aia/IOMMU.html?utm_source=chatgpt.com)

---

# 27. 现在把 H Extension + AIA + IOMMU 串起来

这其实是你前面阶段的最终汇合。

CPU：

```text
Guest Process
     |
     v
    VU
     |
     v
    VS
     |
   vsatp
     |
     v
    GPA
     |
   hgatp
     |
     v
    SPA
```

Device：

```text
Guest Device
     |
    IOVA
     |
   iosatp
     |
     v
    GPA
     |
  iohgatp
     |
     v
    SPA
```

Interrupt：

```text
Guest Device
     |
     | MSI GPA
     v
   IOMMU
     |
 MSI translation
     |
     v
 IMSIC Guest Interrupt File
     |
     v
     VS
```

于是：

```text
          H Extension
               |
          CPU isolation
               |
        +------+------+
        |             |
       AIA          IOMMU
        |             |
  Interrupt virt    DMA virt
```

这就是现代 RISC-V virtualization system 的完整图景。

---

# 28. 与 Arm SMMU 怎么对应？

你有 Armv8-A 背景，所以这一张表会非常有用。

| Arm SMMU | RISC-V IOMMU | 本质 |
|---|---|---|
| Stream ID | Device ID | 哪个 DMA master |
| Substream ID / PASID | Process ID | device 内哪个 process |
| Stream Table | DDT/DC | Device → translation context |
| CD / Context Descriptor | Process Context | process translation context |
| Stage-1 | First-stage | IOVA → GPA/PA |
| Stage-2 | Second-stage | GPA → PA |
| ASID | PSCID | first-stage address-space tagging |
| VMID | GSCID | guest/second-stage tagging |
| TLB invalidation commands | IOTINVAL | translation-cache invalidation |
| CMDQ | CQ | Software → IOMMU commands |
| EVTQ | FQ | IOMMU fault/event → software |
| PRI queue | PQ | Page request |

如果你熟悉 SMMUv3，实际上会发现：

> **RISC-V IOMMU 在总体系统设计思想上和 Arm SMMUv3 非常接近。**

最主要的区别在于：

```text
RISC-V
```

尽可能复用了自己的：

```text
Sv39/Sv48/Sv57
G-stage
AIA
```

体系。

---

# 29. 一个非常值得掌握的实际场景：NIC DMA

假设：

```text
Linux
NIC device_id = 0x20
```

Linux 分配：

```text
PA buffer = 0x81234000
```

然后建立：

```text
IOVA 0x10000000
       ↓
PA   0x81234000
```

IOMMU 数据结构：

```text
ddtp
 |
 v
DDT
 |
 | device_id = 0x20
 v
Device Context
 |
 | iosatp
 v
Sv39-like page table

0x10000000
      |
      v
0x81234000
```

Driver 最终把：

```text
DMA address = 0x10000000
```

写给 NIC。

NIC：

```text
NIC
 |
 | DMA write 0x10000000
 v
IOMMU
 |
 | page walk
 v
0x81234000
 |
 v
DRAM
```

---

# 30. 如果 NIC 发错地址会怎样？

比如：

```text
DMA 0x20000000
```

但没有 mapping：

```text
NIC
 |
 | IOVA 0x20000000
 v
IOMMU
 |
 | PTE invalid
 v
FAULT
 |
 v
Fault Queue
 |
 v
IOMMU interrupt
 |
 v
Linux IOMMU driver
```

于是：

```text
Device bug
```

不会直接变成：

```text
kernel memory corruption
```

这就是 IOMMU 最核心的安全价值。

---

# 31. Device passthrough 再走一次

VM：

```text
Guest Linux
```

拥有 NIC。

Guest 建立：

```text
IOVA 0x1000
      |
 first-stage
      v
GPA 0x2000
```

Hypervisor 建立：

```text
GPA 0x2000
     |
 second-stage
     v
SPA 0x82002000
```

所以最终：

```text
NIC
 |
 | DMA 0x1000
 v
IOMMU
 |
 | iosatp
 v
GPA 0x2000
 |
 | iohgatp
 v
SPA 0x82002000
```

Guest 能控制：

```text
IOVA → GPA
```

但是不能控制：

```text
GPA → SPA
```

这正是：

> **hypervisor 安全边界。**

所以即使 Guest 是 malicious：

```text
Guest:
map IOVA → "Other VM memory"
```

它仍然被：

```text
Second-stage
```

挡住。

这和你学 H extension 的 CPU second-stage protection 是同一个思想。

---

# 32. IOMMU 不等于 cache coherency

这个地方在面试里很容易混。

IOMMU：

```text
解决地址翻译 + 权限隔离
```

Cache coherence：

```text
解决 CPU cache 和 Device 对同一 memory 数据是否一致
```

例如：

```text
CPU cache:
buffer = old

Device DMA:
buffer = new
```

IOMMU 即使完全正常：

```text
IOVA → PA
```

也不代表：

```text
CPU cache 自动看到 new
```

是否 coherent 取决于：

```text
SoC interconnect
device coherency capability
cache maintenance
DMA API
```

所以必须记住：

> **IOMMU ≠ Cache Coherency Unit。**

---

# 33. IOMMU 也不等于 PMP

同样容易混：

```text
PMP
```

主要属于 RISC-V privileged architecture 中的 physical-memory protection mechanism。

而：

```text
IOMMU
```

是：

```text
Device DMA isolation
+
address translation
```

可以概念上看成：

```text
CPU/hart access
    ↓
PMP / MMU


Device DMA access
    ↓
IOMMU
```

两者是在不同 initiator 上建立安全边界。

---

# 34. RISC-V IOMMU 的寄存器不要全背

你只需要第一阶段记住这一小组：

```text
capabilities

fctl

ddtp
```

以及：

```text
cqb / cqh / cqt
fqb / fqh / fqt
pqb / pqh / pqt
```

可以整理成：

```text
                 IOMMU registers
                       |
        +--------------+--------------+
        |              |              |
      ddtp            CQ             FQ
        |         cqb/cqh/cqt    fqb/fqh/fqt
        |
       DDT

                       |
                      PQ
                 pqb/pqh/pqt
```

RISC-V IOMMU MMIO register block 位于一个自然对齐的 4-KiB physical address region 中。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/v20260222/iommu_registers.html)

---

# 35. Software 初始化过程应该怎样理解？

不要首先背 spec 的几十个步骤。

先记逻辑流程：

```text
1. discover IOMMU
        |
        v
2. read capabilities
        |
        v
3. allocate CQ / FQ / PQ
        |
        v
4. build DDT
        |
        v
5. build Device Context
        |
        v
6. build page tables
        |
        v
7. configure ddtp
        |
        v
8. enable queues
        |
        v
9. enable translation
```

官方 software guideline 同样要求软件首先读取 `capabilities` 并检查版本/feature，然后进行后续配置。[RISC-V Documentation](https://docs.riscv.org/reference/iommu/iommu_sw_guidelines.html?utm_source=chatgpt.com)

概念代码可以想象成：

```c
caps = iommu_read(CAPABILITIES);

cq = alloc_command_queue();
fq = alloc_fault_queue();

setup_cq(cq);
setup_fq(fq);

ddt = alloc_ddt();

dc = &ddt[device_id];

dc->iosatp = dma_page_table;
dc->iohgatp = BARE;
dc->valid = 1;

iommu_set_ddtp(ddt);

enable_command_queue();
enable_fault_queue();
enable_iommu();
```

这里不要求你记 bit layout。

Staff-level 更重要的是：

> **知道每一步为什么存在。**

---

# 36. 修改 DMA mapping 的完整时序

比如 Linux 做：

```text
unmap IOVA 0x10000000
```

不能简单：

```c
pte->valid = 0;
free(page);
```

因为 IOMMU 可能还有旧 IOATC：

```text
IOATC:

0x10000000 → old PA
```

正确思想应该是：

```text
1. 修改 PTE

        ↓

2. 保证修改对 IOMMU 可见

        ↓

3. IOTINVAL.VMA

        ↓

4. IOFENCE.C

        ↓

5. 确认旧 translation 不再使用

        ↓

6. safely reuse/free memory
```

这和 CPU：

```text
modify PTE
↓
TLB invalidation
↓
barrier
↓
reuse page
```

是完全同一种系统设计问题。

---

# 37. 为什么这件事情是安全问题，而不仅仅是正确性问题？

假设：

```text
VM-A 曾经拥有 Page X
```

后来：

```text
Page X
```

被重新分配给：

```text
VM-B
```

但是 IOMMU 里仍保留：

```text
VM-A Device:

IOVA → Page X
```

那么：

```text
VM-A device
     |
     | stale translation
     v
VM-B memory
```

于是直接发生：

```text
cross-VM memory access
```

所以：

> **IOMMU invalidation correctness 本身就是 virtualization security boundary 的一部分。**

这一点非常重要。

---

# 38. IOMMU 和 TEE / Confidential Computing 的关系

以你的安全背景，建议再往前想一步。

假设：

```text
Normal World OS 被攻破
```

攻击者控制 DMA-capable device。

如果：

```text
Secure memory
```

仍能被该 device DMA：

```text
CPU isolation 再强也没意义。
```

因此完整系统隔离一定不是只有：

```text
CPU privilege
```

还必须考虑：

```text
CPU memory access
DMA access
interrupt
cache
I/O ownership
```

也就是：

```text
CPU isolation
      +
IOMMU
      +
Interrupt isolation
      +
Memory-controller / platform security
```

所以以后学习：

```text
Confidential VM
Arm CCA
RISC-V CoVE / confidential computing
```

时，IOMMU 都会成为重要组成部分。

---

# 39. 一个很有用的系统安全思维

你可以把一个系统的 memory initiator 分成：

```text
                 DRAM
                  ^
                  |
        +---------+---------+
        |                   |
       CPU                Devices
        |                   |
       MMU                IOMMU
        |                   |
 U/S/VS/HS isolation    DMA isolation
```

系统安全不能只问：

> CPU 能不能访问？

还要问：

> **哪些 bus master 能访问？**

这是从 CPU architecture engineer 往：

```text
System Architecture
SoC Security
Virtualization
Confidential Computing
```

走时非常关键的一次认知升级。

---

# 40. 和 Arm SMMUv3 对照以后，RISC-V IOMMU其实并不难

如果把今天所有内容压缩成一张映射表：

```text
CPU                           Device

VA                            IOVA
 |                             |
satp / vsatp                 iosatp
 |                             |
 v                             v
GPA                           GPA
 |                             |
hgatp                        iohgatp
 |                             |
 v                             v
SPA                           SPA


ASID                         PSCID

VMID                         GSCID

TLB                          IOATC

SFENCE.VMA                  IOTINVAL.VMA

HFENCE.GVMA                 IOTINVAL.GVMA
```

Device identification 再增加：

```text
device_id
   ↓
DDT
   ↓
Device Context
```

Process identification 再增加：

```text
process_id
   ↓
PDT
   ↓
Process Context
```

基本就完成了整个架构。

---

# 41. 你现阶段应该达到什么掌握程度？

对于你这样的 Staff-level system/RTOS/security 背景，我认为现阶段不需要背：

```text
DC bit 17 是什么
fault cause 263 是什么
某个 register bit offset
```

真正应该做到的是：

```text
看到一个 DMA transaction：

(device_id, process_id, IOVA)

你能够自己推演：

device_id
   ↓
DDT
   ↓
DC
   ↓
process_id
   ↓
PDT
   ↓
PC
   ↓
iosatp
   ↓
IOVA → GPA
   ↓
iohgatp
   ↓
GPA → SPA
```

同时知道：

```text
mapping changed
      ↓
IOTINVAL
      ↓
IOATC flush

translation fault
      ↓
FQ

software command
      ↓
CQ

device page fault request
      ↓
PQ

device-local translation cache
      ↓
ATS

shared process VA
      ↓
SVA
```

做到这一点，**RISC-V IOMMU 的架构主体你就真正掌握了。**

---

# 42. 最后给你一张完整知识地图

```text
                         RISC-V IOMMU
                               |
         +---------------------+----------------------+
         |                     |                      |
    Identification         Translation             Control
         |                     |                      |
    device_id               First Stage              CQ
         |                     |                      |
        DDT                  iosatp                 IOTINVAL
         |                     |                    IODIR
        DC                 IOVA → GPA              IOFENCE
         |
    process_id
         |                 Second Stage
        PDT                    |
         |                  iohgatp
        PC                     |
                            GPA → SPA
                               |
                               v
                              DRAM


         +---------------------+----------------------+
         |                     |                      |
       Fault                Advanced              Interrupt
         |                     |                      |
        FQ                    ATS                 MSI remap
                               |                      |
                              PRI                    AIA
                               |
                              SVA
```

---

## 43. 最推荐你的下一步：不要继续背规范，而是做一次“完整 DMA 翻译”

这一阶段下一步最值得深入的是：

```text
device_id = 0x20
process_id = 0x123
IOVA = 0x40001234
```

然后我们**像之前手工走 Sv39 page table 一样**，真正从：

```text
ddtp
 ↓
DDT walk
 ↓
Device Context
 ↓
PDT walk
 ↓
Process Context
 ↓
iosatp
 ↓
Sv39 first-stage page walk
 ↓
GPA
 ↓
iohgatp
 ↓
Sv39x4 second-stage page walk
 ↓
SPA
```

完整手工走一遍。

这是从“理解 IOMMU 概念”跨到“真正掌握 RISC-V IOMMU architecture”的关键一步。

而且现在 QEMU `virt` 已经可以直接挂 `riscv-iommu-pci` 或 `riscv-iommu-sys`；QEMU 官方文档也说明 Linux 的基础 RISC-V IOMMU 支持已在 Linux 6.13 合入，因此这套知识完全可以在没有开发板的情况下做真实实验。[QEMU](https://www.qemu.org/docs/master/specs/riscv-iommu.html?utm_source=chatgpt.com)

如果继续按照我们前面课程的学习方式，**下一节我建议就专门做这个“RISC-V IOMMU 一次 DMA 地址翻译全过程”**，把 `DDT → DC → PDT → PC → iosatp → iohgatp` 用具体地址和具体表项彻底走通。
