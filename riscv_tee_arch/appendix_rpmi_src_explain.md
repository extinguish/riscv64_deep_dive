# RISC-V RPMI 深度教程：从 Linux `cpu_up()` 到 Hart Warmboot

源码专题 · Linux SMP / SBI HSM / OpenSBI / RPMI / PuC / RV64 Assembly

这次我们不再按照 RPMI 的 Service Group 逐项介绍，而是选择一条真实的软件调用链，从 Linux 启动一个 Secondary Hart 开始，逐层分析到 RPMI Server 和底层硬件，最终追踪目标 Hart 如何重新进入 OpenSBI，再进入 Linux。

讲解基于公开的 Linux、OpenSBI、RPMI v1.0 规范以及 `librpmi` 参考实现。代码以实际实现为依据；为了突出机制，部分代码片段经过删减，涉及 SoC 私有电源和复位控制的部分则明确标注为示意实现。

## Chapter 1：建立完整的系统模型

### 1.1 假设一颗真实的 RISC-V 服务器 SoC

我们假设有这样一颗 SoC：

| 组件                            | 配置                            |
| ----------------------------- | ----------------------------- |
| Application Processor         | 16 个 RV64 Harts               |
| Privilege Modes               | M/S/U                         |
| M-mode Firmware               | OpenSBI                       |
| Operating System              | Linux                         |
| Platform Controller           | 独立的 RISC-V 微控制器 PuC           |
| Inter-processor Communication | Shared SRAM + Doorbell        |
| Management Protocol           | RPMI v1.0                     |
| Interrupt Controller          | APLIC + IMSIC，或其他适配中断架构       |
| Power Topology                | 4 Clusters，每个 Cluster 4 Harts |

现在假设：

- Hart 0 正在运行 Linux。
- Hart 1～15 处于停止状态。
- Hart 0 准备启动 Hart 8。
- Hart 8 的电源和复位由 PuC 控制。
- OpenSBI 不能直接访问对应的 Power/Reset Controller。

这里必须解决三个问题：

1. Linux 如何请求 Hart 8 启动？
2. OpenSBI 如何指示 PuC 给 Hart 8 上电和释放复位？
3. Hart 8 上电之后，如何获得正确的 Linux 执行入口、Stack 和其他上下文？

第三个问题尤其重要：PuC 并不知道 Linux 内核线程的 Stack，也不知道 Linux 的 SMP 初始化逻辑。

因此，不能简单地让 PuC 将 Hart 8 直接跳转到 Linux。

### 1.2 整体调用链

一次 Hart Start 的逻辑顺序。请求与 ACK 的完整消息往返将在后面详细展开。

需要建立一个非常重要的概念：整个流程包含三个不同的入口地址。

| 地址                       | 所属组件    | 功能                                    |
| ------------------------ | ------- | ------------------------------------- |
| `secondary_start_sbi`    | Linux   | Secondary Hart 的 S-mode 汇编入口          |
| `scratch->warmboot_addr` | OpenSBI | Secondary Hart 的 M-mode Warmboot 汇编入口 |
| Hardware Reset Vector    | SoC 硬件  | CPU 释放复位后首先取指的位置                      |

在某些 SoC 实现中，PuC 会把 Hart 的启动地址写入专用 Boot Address Register；在另一些平台上，Reset Vector 固定，目标 Hart 首先执行一段 ROM/Boot Stub，之后才跳转到 OpenSBI。

因此：

\\[ \text{Hardware Reset Vector} \rightarrow \text{OpenSBI Warmboot} \rightarrow \text{Linux Secondary Entry} \\]

这是典型的逻辑启动路径，而不是所有芯片都必须采用的固定硬件设计。

## Chapter 2：Linux 如何启动 Secondary Hart？

源码：

[Linux — arch/riscv/kernel/cpu_ops_sbi.c](https://github.com/torvalds/linux/blob/master/arch/riscv/kernel/cpu_ops_sbi.c?utm_source=chatgpt.com)

### 2.1 从 Linux SMP 子系统开始

Linux 中 CPU 的启动过程大体可以抽象为：

```
Linux SMP bring-up
    |
    v
cpu_up()
    |
    v
RISC-V CPU bring-up logic
    |
    v
cpu_ops_sbi.cpu_start()
    |
    v
sbi_cpu_start()
    |
    v
sbi_hsm_hart_start()
    |
    v
sbi_ecall()
```

这里存在一个非常容易忽略的映射：

Linux Logical CPU ID 不一定等于 RISC-V Hart ID。

例如：

```
Linux CPU ID      RISC-V Hart ID
--------------------------------
CPU 0             Hart 0
CPU 1             Hart 4
CPU 2             Hart 8
CPU 3             Hart 12
```

Linux 通过 `cpuid_to_hartid_map(cpuid)` 完成转换。

因此，Linux 要启动的 `CPU 2`，对应硬件上的 `Hart 8`。

这也是 Device Tree 或 ACPI 中的处理器拓扑描述至关重要的原因。

### 2.2 阅读 `sbi_cpu_start()`

我们关注 Linux 中的核心实现：

```
static int sbi_cpu_start(unsigned int cpuid,
                         struct task_struct *tidle)
{
    unsigned long boot_addr =
        __pa_symbol(secondary_start_sbi);

    unsigned long hartid =
        cpuid_to_hartid_map(cpuid);

    unsigned long hsm_data;
    struct sbi_hart_boot_data *bdata =
        &boot_data[cpuid];

    smp_mb();

    bdata->task_ptr = tidle;
    bdata->stack_ptr = task_pt_regs(tidle);

    smp_mb();

    hsm_data = __pa(bdata);

    return sbi_hsm_hart_start(
        hartid, boot_addr, hsm_data);
}
```

这是 Linux SBI CPU Operations 中的核心实现逻辑。

[image](https://www.google.com/s2/favicons?domain=https://linux.googlesource.com\&sz=32)

Git at Google



逐行理解。

第一步：获取 Linux Secondary Entry。

```
unsigned long boot_addr =
    __pa_symbol(secondary_start_sbi);
```

`secondary_start_sbi` 是 Linux 的汇编入口符号。

`__pa_symbol()` 将该内核符号转换为其物理地址。

为什么需要物理地址？

因为刚刚启动的 Hart 还没有进入 Linux 的正常虚拟地址空间。Linux 需要给 OpenSBI 一个能在进入 S-mode 后直接取指的物理入口。

第二步：获得 Hardware Hart ID。

```
unsigned long hartid =
    cpuid_to_hartid_map(cpuid);
```

SBI HSM 使用的是 Hart ID，而不是 Linux Logical CPU ID。

第三步：初始化 Secondary Hart 的启动数据。

```
bdata->task_ptr = tidle;
bdata->stack_ptr = task_pt_regs(tidle);
```

这里的 `tidle` 是为目标 CPU 准备的 Idle Task。

`boot_data` 则保存了目标 Hart 进入 Linux 时需要恢复的最基本上下文。

注意代码版本可能改变 Stack 指针的具体表达方式，但设计目的相同。

第四步：通过 SBI 请求启动。

```
sbi_hsm_hart_start(
    hartid,
    boot_addr,
    hsm_data
);
```

SBI HSM 的三个参数分别是：

| 参数          | 含义                     |
| ----------- | ---------------------- |
| `hartid`    | 要启动的 Hart              |
| `boot_addr` | S-mode 入口物理地址          |
| `hsm_data`  | 传递给目标 Hart 的 opaque 参数 |

按照 SBI HSM 约定，目标 Hart 启动后：

```
a0 = hartid
a1 = opaque
PC = start_addr
Privilege = S-mode
```

也就是说，Linux 不必通过某个全局变量来猜测自己是哪个 Hart，也不必依赖固定的启动参数位置。

### 2.3 最终如何执行 `ecall`？

Linux SBI HSM 的封装逻辑是：

```
ret = sbi_ecall(
    SBI_EXT_HSM,
    SBI_EXT_HSM_HART_START,
    hartid,
    saddr,
    priv,
    0, 0, 0
);
```

RISC-V SBI 的调用约定是：

| Register  | 值                |
| --------- | ---------------- |
| `a7`      | SBI Extension ID |
| `a6`      | SBI Function ID  |
| `a0`      | Hart ID          |
| `a1`      | Start Address    |
| `a2`      | Opaque           |
| `a3`～`a5` | 未使用的参数           |

HSM Extension ID 是 `0x48534D`，`HART_START` Function ID 是 `0`。

将调用简化成 RV64 Assembly，逻辑是：

```
li a7, 0x48534D       # SBI_EXT_HSM
li a6, 0              # HART_START

li a0, 8              # target hart
mv a1, s1             # linux secondary entry
mv a2, s2             # opaque boot data

ecall
```

当 `ecall` 从 S-mode 执行时，若由 M-mode 处理该异常，处理器会：

1. 把异常入口之前的 PC 保存到 `mepc`。
2. 将异常原因记录在 `mcause`。
3. 将原来的 Privilege Mode 记录在 `mstatus.MPP`。
4. 跳转到 `mtvec` 指定的 M-mode Trap Entry。

OpenSBI 接管这次同步异常，识别为 SBI 调用，之后由 HSM Extension Handler 处理。

至此，我们已经完成调用链的第一部分：Linux 如何将 CPU 启动请求交给 OpenSBI。

## Chapter 3：OpenSBI 如何处理 SBI HSM 请求？

这一章进入整个流程最重要的状态管理逻辑。

核心源码：

[lib/sbi/sbi_ecall_hsm.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_ecall_hsm.c?utm_source=chatgpt.com)[lib/sbi/sbi_hsm.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hsm.c?utm_source=chatgpt.com)

### 3.1 SBI Trap 如何分发到 HSM Handler？

Linux 的 `ecall` 进入 OpenSBI 后，流程可以简化为：

```
Linux ecall
    |
    v
OpenSBI _trap_handler
    |
    v
sbi_trap_handler()
    |
    | mcause = CAUSE_SUPERVISOR_ECALL
    v
sbi_ecall_handler()
    |
    | a7 = SBI_EXT_HSM
    v
sbi_ecall_hsm_handler()
```

在 OpenSBI 的 `sbi_trap.c` 中可以看到：

```
case CAUSE_SUPERVISOR_ECALL:
case CAUSE_MACHINE_ECALL:
    rc = sbi_ecall_handler(tcntx);
    break;
```

`CAUSE_SUPERVISOR_ECALL` 的 Exception Code 为 9。

再看 `sbi_ecall_hsm_handler()`：

```
ulong smode =
    (csr_read(CSR_MSTATUS) & MSTATUS_MPP) >>
    MSTATUS_MPP_SHIFT;

switch (funcid) {
case SBI_EXT_HSM_HART_START:
    ret = sbi_hsm_hart_start(
        scratch,
        sbi_domain_thishart_ptr(),
        regs->a0,
        regs->a1,
        smode,
        regs->a2);
    break;
}
```

这里有两个重要的设计细节。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



第一，`smode` 是从 `mstatus.MPP` 获取的。

OpenSBI 根据 Trap 来源确定调用者的 Privilege Mode，而不是直接相信调用者传入的某个参数。

第二，`sbi_domain_thishart_ptr()` 将当前 Hart 对应的 OpenSBI Domain 传递给 HSM 层。

这意味着一个 Hart 能否启动另一个 Hart，不仅取决于目标 Hart ID 是否存在，还取决于目标 Hart 是否属于调用者有权管理的 Domain。

### 3.2 OpenSBI HSM State Machine

SBI HSM 定义了以下状态：

| State           | 数值 | 含义             |
| --------------- | -- | -------------- |
| STARTED         | 0  | Hart 已经启动      |
| STOPPED         | 1  | Hart 已停止       |
| START_PENDING   | 2  | 启动请求已被接受，启动进行中 |
| STOP_PENDING    | 3  | 停止过程中          |
| SUSPENDED       | 4  | 已挂起            |
| SUSPEND_PENDING | 5  | 挂起进行中          |
| RESUME_PENDING  | 6  | 恢复过程中          |

一次正常的 Hart Start 会经历：

STOPPED

State = 1

START_PENDING

State = 2

STARTED

State = 0

请求被接受与目标 Hart 完成启动是两个不同事件。

为什么不能从 `STOPPED` 直接变成 `STARTED`？

因为 Hart Start 是异步操作。

OpenSBI 发起启动请求之后，PuC 可能还需要执行：

- Power Domain 上电
- PLL Lock 等待
- Clock Enable
- Reset Deassert
- Boot Vector 配置
- 目标 Hart 开始执行 M-mode 初始化

所以，“已经接受启动请求”和“CPU 已经进入运行状态”必须使用不同的状态表示。

### 3.3 Per-Hart HSM Context

OpenSBI 定义了：

```
struct sbi_hsm_data {
    atomic_t state;
    unsigned long suspend_type;
    unsigned long saved_mie;
    unsigned long saved_mip;
    unsigned long saved_medeleg;
    unsigned long saved_mideleg;
    u64 saved_menvcfg;
    atomic_t start_ticket;
};
```

这些数据由 OpenSBI 分配在每个 Hart 的 Scratch 扩展区域中。

因此，16 个 Hart 各自具有独立的 HSM Context，而不是共用一个全局 Hart State。

例如：

```
Hart 0 Scratch
  ├── HSM State = STARTED
  ├── next_addr
  └── next_mode

Hart 8 Scratch
  ├── HSM State = STOPPED
  ├── next_addr
  └── next_mode
```

但是这些 per-Hart 对象可以由其他 Hart 访问。

Hart 0 就会通过 `sbi_hartindex_to_scratch()` 找到 Hart 8 的 Scratch，再修改其启动上下文。

这正是多 Hart 系统中常见的一种设计：

数据归属于某个 Hart，并不代表只能由那个 Hart 访问。

### 3.4 详细阅读 `sbi_hsm_hart_start()`

下面按执行阶段重新组织上游核心代码。

阶段 A：验证 Hart 和 Domain

```
u32 hartindex = sbi_hartid_to_hartindex(hartid);

if (smode != PRV_S && smode != PRV_U)
    return SBI_EINVAL;

if (dom &&
    !sbi_domain_is_assigned_hart(dom, hartindex))
    return SBI_EINVAL;

if (dom &&
    !sbi_domain_check_addr(
        dom, saddr, smode, SBI_DOMAIN_EXECUTE))
    return SBI_EINVALID_ADDR;

rscratch = sbi_hartindex_to_scratch(hartindex);

if (!rscratch)
    return SBI_EINVAL;
```

注意这里的三个检查：

1. `smode` 检查目标运行模式。
2. `sbi_domain_is_assigned_hart()` 检查目标 Hart 是否在当前 Domain 中。
3. `sbi_domain_check_addr()` 检查 Linux 提供的启动地址在目标模式下是否具有可执行权限。

特别是第三个。

假设恶意 S-mode 软件试图让另一个 Hart 从 OpenSBI 的 M-mode 私有内存中执行代码，Domain Address Check 应该拒绝这种越权行为。

当然，最终隔离还取决于实际的 PMP、Smepmp 或平台保护机制配置，单靠软件地址检查不能构成完整的物理安全边界。

阶段 B：获取 Start Ticket

```
hdata = sbi_scratch_offset_ptr(
    rscratch, hart_data_offset);

if (!hsm_start_ticket_acquire(hdata))
    return SBI_EINVAL;
```

这里使用了 `start_ticket`，它与 Hart State 是两个独立的同步变量。

原因是什么？

假设 Hart 0 和 Hart 1 同时请求启动 Hart 8：

```
Hart 0                  Hart 1
   |                       |
   | HART_START(8)         | HART_START(8)
   |                       |
   v                       v
读取 Hart 8 状态        读取 Hart 8 状态
STOPPED                 STOPPED
```

如果只使用普通读写，那么两个调用者都可能认为自己有权启动 Hart 8，并同时修改它的 `next_addr`。

OpenSBI 使用 `atomic_cmpxchg()` 实现 Start Ticket：

```
static bool hsm_start_ticket_acquire(
    struct sbi_hsm_data *hdata)
{
    return atomic_cmpxchg(
        &hdata->start_ticket, 0, 1) == 0;
}
```

只有一个 Hart 能成功获取 Ticket。

因此不变量是：

\\[ \text{At most one initiator prepares a given Hart start} \\]

这是避免多 Hart 同时启动同一目标 Hart 的第一层保护。

阶段 C：保存启动 Context

```
hstate = atomic_read(&hdata->state);

if (hstate != SBI_HSM_STATE_STOPPED) {
    rc = hstate == SBI_HSM_STATE_STARTED ?
         SBI_EALREADY : SBI_EINVAL;
    goto err;
}

init_count = sbi_init_count(hartindex);
entry_count = sbi_entry_count(hartindex);

rscratch->next_arg1 = arg1;
rscratch->next_addr = saddr;
rscratch->next_mode = smode;
```

这里非常重要：

`rscratch` 是 Hart 8 的 Scratch，不是 Hart 0 的 Scratch。

假设 Linux 请求：

```
hartid     = 8
saddr      = 0x80208000
arg1       = 0x87001000
smode      = PRV_S
```

其中地址仅为示意。

那么保存后的数据就是：

```
Hart 8 Scratch

next_addr = 0x80208000
next_arg1 = 0x87001000
next_mode = PRV_S
```

这样即使 PuC 完全不认识 Linux，也不影响 Hart 8 将来获取正确的 S-mode 启动参数。

阶段 D：原子地发布 START_PENDING 状态

```
hstate = atomic_cmpxchg(
    &hdata->state,
    SBI_HSM_STATE_STOPPED,
    SBI_HSM_STATE_START_PENDING);
```

为什么这里需要 `atomic_cmpxchg()`，而不是：

```
hdata->state = SBI_HSM_STATE_START_PENDING;
```

不仅是为了保证状态转换本身具有原子性，还涉及内存可见性。

必须满足这样的顺序：

```
Hart 0:

写 next_arg1
写 next_addr
写 next_mode
     |
     | Ordering
     v
发布 START_PENDING
```

Hart 8 只有在观察到 `START_PENDING` 之后，才能安全使用之前保存的 Context。

上游源码明确依赖 `atomic_cmpxchg()` 的内存顺序保证完成这种发布。

这里实际上体现了常见的 Publish–Consume Synchronization 模型：

- Hart 0 发布数据和状态。
- Hart 8 观察状态，并消费已发布的数据。

如果交换顺序，Hart 8 可能看到新的状态，却读取到旧的启动地址。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 3.5 最关键的分支：RPMI Start 还是 IPI Wakeup？

紧接着的源码是：

```
if ((hsm_device_has_hart_hotplug() &&
     (entry_count == init_count)) ||
    (hsm_device_has_hart_secondary_boot() &&
     !init_count)) {

    rc = hsm_device_hart_start(
        hartid,
        scratch->warmboot_addr);

} else {
    rc = sbi_ipi_raw_send(hartindex, true);
}
```

这是理解 OpenSBI HSM 实现最关键的一段代码。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



注意两个概念：

- `init_count`：目标 Hart 成功完成 OpenSBI 初始化的次数。
- `entry_count`：目标 Hart 进入 OpenSBI Warmboot 初始化路径的次数。

当平台支持真实 Hart Hotplug 时，Hart 可能被停止并重新启动，需要再次调用底层 HSM Device。

如果目标 Hart 实际上只是停在 OpenSBI 的 `wfi()` 循环中，那么没有必要给它重新上电，使用 IPI 唤醒即可。

因此：

| 情况                                 | OpenSBI 采用的方式    |
| ---------------------------------- | ---------------- |
| 目标 Hart 尚未初始化，平台提供 Secondary Boot  | HSM Device Start |
| 已停止，且平台提供真实 Hotplug，满足相关计数条件       | HSM Device Start |
| Hart 仍在 OpenSBI Warmboot Wait Loop | IPI Wakeup       |

这里还有一个很值得关注的细节：

`scratch->warmboot_addr` 使用的是发起调用的 Hart 的 Scratch 中的地址。

在正常 OpenSBI Firmware 布局中，各 Hart 使用同一个 Warmboot 代码入口，因此可以将该入口作为目标 Hart 的启动地址。

但如果你在一个复杂的多镜像、多 Domain SoC 中移植 OpenSBI，这种假设就必须经过验证。

### 3.6 为什么不能只用 IPI？

IPI 能够让处于等待状态的 Hart 响应中断，但不能保证一个已经物理断电的 Hart 被重新上电。

例如：

```
Hart 8 Clock Gated / Power Off
      |
      | Send IPI
      v
无法取指，也无法执行中断处理
```

真正的 Power-Off CPU 需要底层 Power Controller 重新开启电源。

因此 OpenSBI 根据 HSM Device Capabilities 选择不同的启动机制。

而 RPMI HSM 正好提供了一个让 OpenSBI 请求外部管理控制器执行硬件 Hart Start 的标准化接口。

## Chapter 4：OpenSBI 如何把 HSM 转换成 RPMI Request？

核心源码：

[OpenSBI — RPMI HSM Driver](https://github.com/riscv-software-src/opensbi/blob/master/lib/utils/hsm/fdt_hsm_rpmi.c?utm_source=chatgpt.com)

### 4.1 RPMI HSM 在 OpenSBI 中是什么角色？

OpenSBI 并没有把 RPMI 硬编码到 `sbi_hsm.c` 里面。

它定义了一个 HSM Device Abstraction：

```
static struct sbi_hsm_device sbi_hsm_rpmi = {
    .name         = "rpmi-hsm",
    .hart_start   = rpmi_hsm_start,
    .hart_stop    = rpmi_hsm_stop,
    .hart_suspend = rpmi_hsm_suspend,
};
```

这是一种典型的 Hardware Abstraction Layer 设计。

`lib/sbi/sbi_hsm.c` 负责通用 HSM State Machine。

`lib/utils/hsm/fdt_hsm_rpmi.c` 负责把通用的 HSM 操作转换成 RPMI 消息。

因此未来如果平台使用另一种 Hart 启动硬件实现，不必修改 SBI HSM 状态机，只需实现对应的 HSM Device Callback。

### 4.2 阅读 `rpmi_hsm_start()`

原始核心逻辑：

```
static int rpmi_hsm_start(
    u32 hartid, ulong resume_addr)
{
    struct rpmi_hsm_hart_start_req req;
    struct rpmi_hsm_hart_start_resp resp;
    struct rpmi_hsm *rpmi =
        rpmi_hsm_get_pointer(hartid);

    if (!rpmi)
        return SBI_ENOSYS;

    req.hartid = hartid;
    req.start_addr_lo = resume_addr;
    req.start_addr_hi =
        (u64)resume_addr >> 32;

    return rpmi_normal_request_with_status(
        rpmi->chan,
        RPMI_HSM_SRV_HART_START,
        &req,
        rpmi_u32_count(req),
        rpmi_u32_count(req),
        &resp,
        rpmi_u32_count(resp),
        rpmi_u32_count(resp));
}
```

这里有三个关键点。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



第一，RPMI Request 需要区分 64-bit 地址的高低 32-bit。

```
req.start_addr_lo = resume_addr;
req.start_addr_hi = (u64)resume_addr >> 32;
```

RPMI Payload 以 32-bit Word 为基本单元，便于不同 XLEN 或不同 ISA 的管理处理器实现互操作。

第二，`resume_addr` 是 M-mode Warmboot Entry。

这里并没有把 Linux 的 `secondary_start_sbi` 直接发给 PuC。

PuC 无须理解 Linux 的上下文。

第三，RPMI HSM 驱动通过 Mailbox Channel 发送消息。

也就是说，HSM 驱动本身不直接操作共享内存 Head/Tail。

其职责只限于：

```
Hart ID
    +
M-mode Start Address
    |
    v
构造 RPMI HSM Request
    |
    v
Mailbox API
```

### 4.3 RPMI HART_START 的 Payload

根据 RPMI v1.0，HART_START 使用：

```
Service Group = 0x0005
Service ID    = 0x06
Message Type  = Normal Request
```

请求 Payload 为：

| Word | Field           | 说明                  |
| ---- | --------------- | ------------------- |
| 0    | HART_ID         | 目标 Hart ID          |
| 1    | START_ADDR_LOW  | M-mode 启动地址低 32 bit |
| 2    | START_ADDR_HIGH | M-mode 启动地址高 32 bit |

例如，假设：

```
Hart ID             = 8
OpenSBI Warmboot PC = 0x0000000080004000
```

那么 Payload 按 Little Endian 编码为：

```
08 00 00 00    HART_ID
00 40 00 80    START_ADDR_LOW
00 00 00 00    START_ADDR_HIGH
```

这个示例说明 RPMI 协议传输的是可独立解码的参数字段，不是某个 C Struct 在内存中的任意编译器布局。

### 4.4 `rpmi_normal_request_with_status()`

继续进入：

[OpenSBI — rpmi_mailbox.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/utils/mailbox/rpmi_mailbox.c?utm_source=chatgpt.com)

核心代码：

```
args.type = RPMI_MSG_NORMAL_REQUEST;
args.service_id = service_id;
args.tx_endian_words = req_endian_words;
args.rx_endian_words = resp_endian_words;

mbox_xfer_init_txrx(
    &xfer, &args,
    req, sizeof(u32) * req_words,
    RPMI_DEF_TX_TIMEOUT,
    resp, sizeof(u32) * resp_words,
    RPMI_DEF_RX_TIMEOUT);

ret = mbox_chan_xfer(chan, &xfer);

if (ret)
    return ret;

return rpmi_xlate_error(((u32 *)resp)[0]);
```

这里又引入一个重要概念：错误码存在两层。

第一层是 Transport Error：

```
Queue Full
Mailbox Timeout
Message Transmission Failure
```

第二层是 RPMI Service Error：

```
Invalid Hart ID
Invalid Start Address
Hart Already Started
Hardware Failure
```

所以：

```
ret = mbox_chan_xfer(...);
```

返回成功，只能说明 Mailbox Transaction 按预期完成，并不保证 PuC 接受了 Hart Start 操作。

接着必须检查：

```
((u32 *)resp)[0]
```

也就是 RPMI ACK Payload 的 `STATUS`。

最后通过 `rpmi_xlate_error()` 将 RPMI Error 转换为 SBI Error。

这构成了：

```
PuC RPMI Status
      |
      v
RPMI-to-SBI Error Translation
      |
      v
OpenSBI HSM Return Code
      |
      v
Linux errno
```

尤其要注意，映射不一定是双射。例如当前 OpenSBI 实现将 `RPMI_ERR_BUSY` 映射为 `SBI_EFAIL`，Linux 不能据此恢复出原始 RPMI Error 的精确类别。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## Chapter 5：RPMI Shared Memory Mailbox 的完整实现

终于进入 RPMI 最底层的通信机制。

这一部分建议重点掌握，因为它涉及几个非常底层的能力：

- Shared Memory IPC
- Ring Buffer
- Locking 与并发
- RVWMO Memory Ordering
- MMIO Doorbell
- Request/Response Matching
- Timeout 与异常恢复

核心源码：

[OpenSBI — fdt_mailbox_rpmi_shmem.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/utils/mailbox/fdt_mailbox_rpmi_shmem.c?utm_source=chatgpt.com)

### 5.1 RPMI 如何映射到 OpenSBI Mailbox Framework？

从前一章的：

```
rpmi_normal_request_with_status(...)
```

开始，实际调用链是：

```
rpmi_normal_request_with_status()
    |
    v
mbox_chan_xfer()
    |
    v
rpmi_shmem_mbox_xfer()
    |
    +-- smq_tx()
    |      |
    |      +-- __smq_tx()
    |      +-- Write Doorbell
    |
    +-- smq_rx()
           |
           +-- __smq_rx()
           +-- Check Token
           +-- Read Response
```

这里的 `mbox_chan_xfer()` 是 OpenSBI 的通用 Mailbox API。

RPMI Transport 驱动通过 `rpmi_shmem_mbox_xfer()` 提供实际发送和接收操作。

因此，它与 Linux Mailbox Framework 在设计思想上相似：通用接口负责组织传输，Controller Driver 负责具体硬件访问。

### 5.2 Mailbox 如何生成 Message Token？

看 `lib/utils/mailbox/mailbox.c`：

```
int mbox_chan_xfer(
    struct mbox_chan *chan,
    struct mbox_xfer *xfer)
{
    /* 参数检查 ... */

    if (!(xfer->flags & MBOX_XFER_SEQ))
        mbox_xfer_set_sequence(
            xfer,
            atomic_add_return(
                &chan->mbox->xfer_next_seq, 1));

    return chan->mbox->xfer(chan, xfer);
}
```

这里使用原子计数器生成传输序列号。

RPMI Message Header 中的 Token 为 16-bit，因此真正写入消息的是序列号的低 16 位。

这样可以让多个 Hart 共享同一个 RPMI Transport，而不要求全局只能有一个 Outstanding Request。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



举例：

```
Hart 0    HART_START(8)     Token = 101
Hart 1    HART_START(9)     Token = 102
Hart 2    HART_START(10)    Token = 103
```

PuC 可以按照其内部调度处理请求。

假设响应按这样的顺序返回：

```
ACK Token 103
ACK Token 101
ACK Token 102
```

客户端不能简单地认为第一个收到的 ACK 就属于 Hart 0。

它必须根据 Token 找到对应请求。

不过，Token 的 16-bit 宽度也带来一个问题：计数器会回绕。设计高并发 RPMI Firmware 时，需要考虑未完成请求、迟到响应以及 Token 复用之间的关系。

### 5.3 RPMI Header 的实际内存布局

OpenSBI 使用：

```
struct rpmi_message_header {
    le16_t servicegroup_id;
    uint8_t service_id;
    uint8_t flags;
    le16_t datalen;
    le16_t token;
} __packed;
```

因此，一个 HART_START Request 的布局如下：

HART_START Request — 20 Bytes

| Offset | 字段              | 示例值        |
| ------ | --------------- | ---------- |
| 0x00   | Service Group   | 0x0005     |
| 0x02   | Service ID      | 0x06       |
| 0x03   | Flags           | 0x00       |
| 0x04   | Data Length     | 0x000C     |
| 0x06   | Token           | 0x0065     |
| 0x08   | Hart ID         | 0x00000008 |
| 0x0C   | Start Addr Low  | 0x80004000 |
| 0x10   | Start Addr High | 0x00000000 |

示例 Token = 101。8 Bytes Header + 12 Bytes Payload。

完整字节序列是：

```
05 00 06 00 0C 00 65 00
08 00 00 00 00 40 00 80
00 00 00 00
```

可以把它视为 RPMI 协议的一个简单测试向量。

如果在 ZeBu/Palladium 仿真中抓取 Shared SRAM 的内容，就可以按照这些 Offset 进行解码。

这比仅凭串口打印 `RPMI request failed` 更容易定位故障。

### 5.4 Ring Queue 的内部数据结构

OpenSBI 中：

```
struct smq_queue_ctx {
    u32 queue_id;
    u32 num_slots;

    spinlock_t queue_lock;

    enum rpmi_queue_type queue_type;

    volatile le32_t *headptr;
    volatile le32_t *tailptr;
    volatile uint8_t *buffer;

    char name[RPMI_NAME_CHARS_MAX];
};
```

从数据结构可以看出：

- `headptr`：Consumer 管理的读指针。
- `tailptr`：Producer 管理的写指针。
- `buffer`：实际消息数据。
- `queue_lock`：保护同一侧多个 Hart 对本地 Queue Context 的并发访问。

`spinlock` 保护的是 AP 侧软件并发，并不意味着 PuC 会使用同一把锁。

AP 与 PuC 之间依靠共享队列的所有权协议和内存顺序保证协调。

### 5.5 阅读 `__smq_tx()`

核心发送逻辑可以缩减为：

```
if (__smq_queue_full(qctx))
    return SBI_ENOMEM;

tailidx = le32_to_cpu(*qctx->tailptr);

header.servicegroup_id =
    cpu_to_le16(service_group_id);

header.service_id = args->service_id;
header.flags = args->type;

header.datalen =
    cpu_to_le16((u16)xfer->tx_len);

header.token =
    cpu_to_le16((u16)xfer->seq);

/* Write header and payload */
dst = (char *)qctx->buffer +
      tailidx * slot_size;

sbi_memcpy(dst, &header, sizeof(header));

/* Copy payload ... */

smp_wmb();

*qctx->tailptr =
    cpu_to_le32((tailidx + 1) %
                qctx->num_slots);

if (mb_regs)
    writel(a2p_doorbell_value,
           &mb_regs->db_reg);
```

重点分析三个操作。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



操作一：检查队列是否已满。

```
(tail + 1) % num_slots == head
```

这里 `num_slots` 指消息区的 Slot 数量，不包含 Head/Tail 两个管理 Slot。

操作二：先写消息，再发布 Tail。

```
Write Header
Write Payload
       |
       v
smp_wmb()
       |
       v
Update Tail
```

这是 Producer–Consumer Ring Queue 最核心的顺序约束。

因为 Tail 的变化代表：

> 当前 Slot 的消息已经准备好，Consumer 可以读取。

如果先更新 Tail，再写 Payload，PuC 就可能读到未写完的数据。

操作三：通过 Doorbell 通知 PuC。

```
writel(a2p_doorbell_value,
       &mb_regs->db_reg);
```

Doorbell 只负责通知，Message Payload 始终位于共享内存。

要特别注意：`smp_wmb()` 和 `writel()` 的效果必须结合硬件的内存属性与总线可见性定义来理解。

如果 Shared SRAM 和 Doorbell 属于不同的总线或一致性域，平台需要保证：

\\[ \text{Payload Visible} \prec \text{Tail Visible} \prec \text{Doorbell Observed} \\]

单纯在代码里添加一个 `fence`，并不能自动修复错误的 Cacheability、Interconnect Ordering 或 Device Memory 配置。

### 5.6 阅读 `__smq_rx()`

收到 ACK 时，OpenSBI 并不是简单读取队列的第一个消息。

源码会遍历从 Head 到 Tail 的消息：

```
pos = headidx;

while (pos != tailidx) {
    src = (void *)qctx->buffer +
          pos * slot_size;

    if (GET_TOKEN(src) ==
        (xfer->seq & RPMI_MSG_TOKEN_MASK))
        break;

    pos = (pos + 1) % qctx->num_slots;
}
```

这里省略了无 Token 匹配模式的另一条判断分支。

如果找到了当前请求的 Token，接收端会读取 Payload，并推进 Head。

有一个有趣的实现细节：当匹配消息不在 Head 位置时，上游代码会将它与 Head Slot 的内容交换，再消费 Head 处的消息。

因此，这个接收路径支持非严格按顺序到达的响应。

从工程审查角度，还应关注：

- 是否验证了 Token 之外的 Message Type 和 Service ID？
- 是否正确验证了 Payload Length？
- 超时的旧 ACK 如何处理？
- Header 异常是否会造成 Queue 无法推进？
- PuC 是否可能写入畸形响应？

这些都是安全性和健壮性审查的重点。

### 5.7 Timeout：最容易被低估的问题

当前 OpenSBI 的 `smq_rx()` 会进行有界轮询，每次未取得匹配响应后进行延迟，然后重试。

默认的 `RPMI_DEF_RX_TIMEOUT` 为 20，驱动使用毫秒级轮询。实际耗时还会受到锁竞争和系统执行环境影响，不应该将它理解为严格的硬件计时器 Deadline。

真正难处理的是：

```
OpenSBI                PuC
   |                    |
   | HART_START         |
   |------------------->|
   |                    | 执行 Power-On
   |                    |
   |  ACK 尚未到达       |
   |                    |
   | Timeout            |
   |                    |
   |                    | 完成启动
   |                    |
   |<-------------------| Late ACK
```

这种情况下，OpenSBI 可能认为请求失败，但目标 Hart 实际上已经开始执行。

如果 OpenSBI 错误地回滚状态，而目标 Hart 正在进入 Warmboot，就可能触发更严重的状态不一致。

因此，Timeout 只是调用方在规定时间内未得到确认，并不代表操作具有事务回滚语义。

从 SoC 设计角度，我会要求 PuC Firmware 明确以下契约：

1. `HART_START` 什么时候返回 ACK？
2. ACK Success 表示请求已接受，还是硬件启动已完成？
3. 重复请求是否具有幂等性？
4. 发生超时后，客户端如何查询真实 Hart State？
5. 如果状态无法恢复，是否需要触发平台级错误处理？

RPMI 的协议层和 SoC 的硬件状态机必须共同回答这些问题。

## Chapter 6：OpenSBI 如何发现 RPMI Transport 和 HSM Service？

之前我们沿运行时路径向下追踪，现在回到系统初始化阶段。

因为只有先完成 RPMI Transport 初始化，后续 `HART_START` 才能使用。

### 6.1 Device Tree 是配置入口

上游支持的 RPMI Shared Memory Mailbox Compatible 为：

```
compatible = "riscv,rpmi-shmem-mbox";
```

实际 Device Tree 可以参考以下结构：

```
rpmi_mbox: mailbox@10001000 {
    compatible = "riscv,rpmi-shmem-mbox";

    reg = <0x10001000 0x800>,
          <0x10001800 0x800>,
          <0x10002000 0x800>,
          <0x10002800 0x800>,
          <0x10003000 0x4>;

    reg-names = "a2p-req",
                "p2a-ack",
                "p2a-req",
                "a2p-ack",
                "a2p-doorbell";

    msi-parent = <&imsic_mlevel>;

    riscv,slot-size = <64>;
    riscv,a2p-doorbell-value = <0x8000>;

    #mbox-cells = <1>;
};
```

这是基于公开 Binding 的示例地址，不代表任何具体芯片的真实地址分配。

[image](https://www.google.com/s2/favicons?domain=https://mjmwired.net\&sz=32)

mjmwired.net



在 RPMI Mailbox 中：

```
#mbox-cells = <1>;
```

后面的一个 Cell 通常用于指定 RPMI Service Group ID。

HSM 使用 `0x05`。

OpenSBI HSM Client 节点采用：

```
compatible = "riscv,rpmi-hsm";
```

并通过 `mboxes` 属性引用对应的 RPMI HSM Channel。具体节点路径及父节点地址格式要与平台 DT 和上游 Binding 保持一致。

### 6.2 Mailbox 初始化的实际动作

在 `rpmi_shmem_transport_init()` 中，OpenSBI 会：

1. 验证 Compatible。
2. 读取 `riscv,slot-size`。
3. 读取 `reg` 和 `reg-names`。
4. 计算每个 Queue 的 Slot 数量。
5. 设置 Head/Tail/Buffer 指针。
6. 配置共享内存访问权限。
7. 获取 Doorbell 地址。

其中非常值得注意的代码是：

```
sbi_domain_root_add_memrange(
    reg_addr,
    reg_size,
    reg_size,
    SBI_DOMAIN_MEMREGION_MMIO |
    SBI_DOMAIN_MEMREGION_M_READABLE |
    SBI_DOMAIN_MEMREGION_M_WRITABLE);
```

它明确向 OpenSBI Domain 管理层添加了 M-mode 可访问的内存区域。

这关系到 OpenSBI 的 PMP/系统级访问控制配置。

如果 RPMI Shared Memory 的物理地址没有正确加入允许区域，在启用强化隔离之后，后续发送 RPMI 消息就可能发生 Access Fault。

同样，Doorbell 的 MMIO 区域也需要得到适当授权。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 6.3 BASE Service Discovery

初始化 Shared Memory Transport 后，OpenSBI 并不立即假定 PuC 支持所有 RPMI Service Group。

它会通过 BASE Service 执行能力发现，包括：

```
BASE_PROBE_SERVICE_GROUP
BASE_GET_IMPLEMENTATION_VERSION
BASE_GET_IMPLEMENTATION_IDN
BASE_GET_SPEC_VERSION
BASE_GET_ATTRIBUTES
```

例如：

```
ret = smq_base_get_two_u32(
    mctl,
    RPMI_BASE_SRV_GET_SPEC_VERSION,
    NULL,
    tval);
```

为什么这一步重要？

因为不同版本 PuC Firmware 可能支持不同的 Service Group。

即使 Shared Memory 可以正常收发消息，也不代表 PuC 一定支持 HSM。

OpenSBI 需要验证协商结果，然后才能注册相应驱动。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 6.4 RPMI HSM Client 初始化

在 `rpmi_hsm_cold_init()` 中，OpenSBI 会：

```
fdt_mailbox_request_chan(
    fdt, nodeoff, 0, &chan);
```

随后查询 PuC 管理的 Hart 列表：

```
RPMI_HSM_SRV_GET_HART_LIST
```

并把对应的 Mailbox Channel 分配给各个 Hart 的 RPMI HSM Context：

```
rpmi->chan = chan;
rpmi->susp = susp;
```

最后注册：

```
sbi_hsm_set_device(&sbi_hsm_rpmi);
```

这时 OpenSBI 的通用 HSM State Machine 才与 RPMI HSM Driver 建立实际关联。

如果日志中显示：

```
Platform HSM Device : rpmi-hsm
```

可以说明 RPMI HSM Device 已经在 OpenSBI 注册。

但它并不能单独证明 PuC 的 Power/Reset Sequencing 已经完全正确。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## Chapter 7：PuC / RPMI Server 如何真正启动 CPU？

这一章跨过 AP Firmware 与 Platform Firmware 的边界。

这里需要明确：PuC 的 Power/Reset 寄存器控制是 SoC 特定实现，因此不可能存在一段适用于所有 RISC-V SoC 的固定上游代码。

不过 RPMI Server 的协议处理框架可以参考：

[librpmi — RPMI Server Reference Implementation](https://github.com/riscv-software-src/librpmi?utm_source=chatgpt.com)

### 7.1 服务端的职责

典型 RPMI Server 可以拆为：

```
RPMI Shared Memory Transport
            |
            v
RPMI Message Decoder
            |
            v
Service Group Dispatcher
            |
            v
HSM_HART_START Handler
            |
            v
Platform HSM HAL
            |
            v
Power / Clock / Reset Controller
```

这里我给出一个完整的、便于理解的 PuC 实现示例。

以下是教学用途的伪代码，并非 `librpmi` 上游函数签名。

```
int puc_handle_hart_start(
    const struct rpmi_hsm_start_req *req)
{
    uint32_t hartid = req->hartid;

    uint64_t start_addr =
        ((uint64_t)req->start_addr_hi << 32) |
        req->start_addr_lo;

    if (!platform_valid_hart(hartid))
        return RPMI_ERR_INVALID_PARAM;

    if (!platform_valid_boot_addr(start_addr))
        return RPMI_ERR_INVALID_ADDR;

    if (platform_hart_is_running(hartid))
        return RPMI_ERR_ALREADY;

    int ret = platform_power_on_hart(hartid);

    if (ret)
        return RPMI_ERR_HW_FAULT;

    ret = platform_enable_hart_clock(hartid);

    if (ret)
        goto rollback_power;

    ret = platform_set_boot_address(
        hartid, start_addr);

    if (ret)
        goto rollback_clock;

    ret = platform_release_hart_reset(hartid);

    if (ret)
        goto rollback_clock;

    return RPMI_SUCCESS;

rollback_clock:
    platform_disable_hart_clock(hartid);

rollback_power:
    platform_power_off_hart(hartid);

    return RPMI_ERR_HW_FAULT;
}
```

这段代码体现了两个设计层次。

RPMI 层只知道：

```
HART_ID
START_ADDRESS
```

底层 HAL 才知道：

```
CPU Power Domain Register
Clock Gate Register
Reset Controller
Boot Address Register
```

这正是 RPMI 将通用 Firmware 与平台专有控制逻辑解耦的价值。

### 7.2 CPU Power-On 往往不是一个寄存器操作

在一个复杂的 Server SoC 中，CPU Cluster 可能具有以下依赖：

```
System Power Domain
       |
       v
CPU Cluster Power Domain
       |
       +-- Shared L2/L3 Related Resources
       |
       +-- Cluster Clock/PLL
       |
       +-- Hart 8 Power Domain
                |
                +-- Core Reset
                +-- Local Interrupt State
                +-- Boot Address Logic
```

因此，启动 Hart 8 可能需要：

1. 确定 Cluster 是否已经上电。
2. 打开 Cluster Power Domain。
3. 等待 Power-Good。
4. 配置必要的 SRAM Retention/Isolation。
5. 使能 Cluster Clock。
6. 等待 PLL Lock。
7. 打开 Hart 8 电源域。
8. 配置 Boot Address。
9. 释放 Reset。
10. 更新 PuC 维护的 Hart State。

顺序由 SoC 硬件设计约束决定。

在一些芯片中，Shared L2 需要先于 CPU Core 上电；在另一些设计中，Clock/Reset 具有不同的依赖关系。

RPMI 不规定这些硬件控制顺序，只规定 AP 与 PuC 的通信语义。

### 7.3 PuC 的 ACK 什么时候可以返回？

考虑下面三个时间点：

```
T1: PuC 收到 HART_START
T2: PuC 完成 Reset Release
T3: Hart 8 进入 Linux Secondary Entry
```

RPMI Server 的 ACK 不能被直接解释为 T3 已发生。

SBI HSM Start 本身也允许异步完成。

因此需要分清：

- Request 被接受。
- 硬件启动动作执行成功。
- OpenSBI 完成 Warmboot。
- Linux Secondary CPU 完成 SMP Online。

这些不是同一件事。

对于工业实现，建议将这些里程碑分别记录，而不是仅打印一个 `Hart Start Success`。

### 7.4 一个真实的仿真方向

截至 2026 年 11 月 4 日，QEMU 社区正在审查一组为 RISC-V `virt` Machine 添加 RPMI 支持的补丁，最新检索到的 v13 系列于 2026 年 11 月 2 日发布。

该系列通过 `librpmi` 实现 RPMI Transport 与 Server，并提供 HSM、System Reset、System Suspend 等功能。其中 HSM 实现将 Hart 状态与 QEMU CPU 停止、恢复及 WFI 行为关联。

这非常适合作为理解 RPMI Server 的可运行参考。不过它仍是审查中的补丁系列，不能假定已经进入正式发布的 QEMU。

[image](https://www.google.com/s2/favicons?domain=https://go.mail-archive.com\&sz=32)

go.mail-archive.com+1



## Chapter 8：Hart 8 如何从 Hardware Reset 进入 OpenSBI Warmboot？

现在我们已经完成 Hart 0 发起启动到 PuC 释放 Hart 8 Reset 的部分。

接下来开始追踪目标 Hart 8 的真实汇编执行过程。

源码：

[OpenSBI — firmware/fw_base.S](https://github.com/riscv-software-src/opensbi/blob/master/firmware/fw_base.S?utm_source=chatgpt.com)

### 8.1 `_start_warm` 是什么？

在 OpenSBI Firmware 初始化阶段，会将 Warmboot Address 保存到 Scratch：

```
lla a4, _start_warm
REG_S a4, SBI_SCRATCH_WARMBOOT_ADDR_OFFSET(tp)
```

因此：

```
scratch->warmboot_addr = &_start_warm
```

这里的 `tp` 在 OpenSBI Firmware 的汇编初始化期间用来指向当前 Hart 的 Scratch Area。

随后，OpenSBI 将该地址交给 RPMI HSM Start Driver。

### 8.2 `_start_warm` 的核心汇编

上游代码中：

```
_start_warm:
    li      ra, 0
    call    _reset_regs

    csrw    CSR_MIE, zero

    lla     a4, platform

    /* 获取 Hart 数量和 Stack Size */

    csrr    s6, CSR_MHARTID

    /* 查找 Hart Index */

    /* 计算当前 Hart 的 Scratch Address */

    csrw    CSR_MSCRATCH, tp

    add     sp, tp, zero

    lla     a4, _trap_handler
    csrw    CSR_MTVEC, a4

    csrr    a0, CSR_MSCRATCH
    call    sbi_init
```

这里省略了 Hart Index 查找、H-extension Trap Handler 选择等代码。

完整源码中，这些步骤都是实际实现的一部分。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



我们逐项分析。

### 8.3 为什么必须重新初始化 `mscratch`？

`mscratch` 是每个 Hart 独立拥有的 M-mode CSR。

它通常被 OpenSBI 的 Trap Entry 代码用作寻找 per-Hart Firmware Context 的入口。

但是 CPU 刚从 Reset 状态恢复时，不能假定 `mscratch` 仍然保留着上次运行时的内容。

尤其是非保持状态的 Power-Off/Power-On 循环。

因此：

```
csrw CSR_MSCRATCH, tp
```

是 Warmboot 中一个非常关键的步骤。

它让后续 M-mode Trap Handler 能够找到对应 Hart 的 Scratch。

### 8.4 为什么还需要重新设置 Stack？

OpenSBI 为每个 Hart 准备自己的 M-mode Stack。

Hart 8 刚启动时，不能直接使用 Hart 0 的 Stack。

否则两个 Hart 就可能写入同一块 Stack Memory，造成严重的数据覆盖。

因此：

```
add sp, tp, zero
```

在当前 OpenSBI Firmware Layout 中，将 Stack Pointer 设置到相应位置。

这里需要再次区分两个 Stack：

| Stack                 | 使用阶段                                  |
| --------------------- | ------------------------------------- |
| OpenSBI M-mode Stack  | `_start_warm`、`sbi_init()`、M-mode 初始化 |
| Linux Secondary Stack | 进入 `secondary_start_sbi` 后            |

两个 Stack 不能混为一谈。

### 8.5 `sbi_init()` 如何区分 Coldboot 与 Warmboot？

在 `sbi_init.c`：

```
void __noreturn sbi_init(
    struct sbi_scratch *scratch)
{
    bool coldboot = false;
    u32 hartid = current_hartid();

    /* Coldboot Lottery 和平台检查 ... */

    if (coldboot)
        init_coldboot(scratch, hartid);
    else
        init_warmboot(scratch, hartid);
}
```

OpenSBI 会通过 Coldboot Lottery 等机制选择 Coldboot Hart。

其他 Hart 进入 Warmboot 路径。

但这里需要区分两种 Warmboot：

- SoC 首次上电后，非 Coldboot Hart 进入 OpenSBI。
- CPU Hotplug 后，目标 Hart 再次进入 OpenSBI。

二者可以复用 Warmboot Path，但硬件状态和 Firmware 初始化历史并不相同。

### 8.6 `init_warmboot()` 的真实逻辑

```
static void __noreturn init_warmboot(
    struct sbi_scratch *scratch,
    u32 hartid)
{
    int hstate;

    wait_for_coldboot(scratch);

    hstate = sbi_hsm_hart_get_state(
        sbi_domain_thishart_ptr(), hartid);

    if (hstate < 0)
        sbi_hart_hang();

    if (hstate == SBI_HSM_STATE_SUSPENDED) {
        init_warm_resume(scratch, hartid);
    } else {
        sbi_ipi_raw_clear(true);
        init_warm_startup(scratch, hartid);
    }
}
```

这段代码有两个重要信息。

第一，Warmboot 不一定意味着 Hart Start。

如果 Hart 当前处于 SUSPENDED，那么 OpenSBI 会执行 Resume Path。

如果处于其他允许的启动状态，则进入 Warm Startup Path。

第二，Warm Startup 必须等 Coldboot 初始化完成。

OpenSBI 中许多全局结构由 Coldboot Hart 初始化。

Secondary Hart 不能在这些全局数据尚未准备好时直接访问它们。

因此 `wait_for_coldboot()` 保证相应的全局初始化依赖顺序。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 8.7 `init_warm_startup()` 为什么还需要重复初始化？

看看实际执行的部分操作：

```
sbi_hsm_init(scratch, false);
sbi_hart_init(scratch, false);
sbi_trap_init(scratch, false);
sbi_timer_init(scratch, false);
sbi_platform_early_init(plat, false);
sbi_pmu_init(scratch, false);
sbi_irqchip_init(scratch, false);
sbi_ipi_init(scratch, false);
sbi_tlb_init(scratch, false);
sbi_platform_final_init(plat, false);
sbi_hart_protection_configure(
    scratch, sbi_domain_thishart_ptr());
```

并不是所有模块都重新执行完整的全局初始化。

这里大量使用 `cold_boot = false`，让各模块执行适当的 per-Hart 初始化和恢复。

为什么需要这一步？

因为 Hart 的 Power-Off/Reset 可能使以下状态丢失：

- `mstatus`
- `mie`
- `mtvec`
- 与 Hart 相关的 Interrupt Context
- Local Timer 配置
- PMU 控制状态
- PMP 等保护配置

但与之相对，部分保存在 Retained SRAM 或系统 DRAM 中的 OpenSBI 全局数据可能仍然存在。

所以 Warmboot 的设计原则是：

恢复或重新初始化可能丢失的 per-Hart 状态，同时保留已经初始化且仍然有效的全局状态。

特别是 PMP/Smepmp 的配置，OpenSBI 在交给 S-mode 之前必须建立正确的执行隔离环境。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## Chapter 9：OpenSBI 最终如何切换到 Linux S-mode？

这是我们整个调用链的最后一个关键环节。

### 9.1 `sbi_hsm_hart_start_finish()`

在 Warmboot 初始化末尾：

```
sbi_hsm_hart_start_finish(scratch, hartid);
```

其核心实现：

```
if (!__sbi_hsm_hart_change_state(
        hdata,
        SBI_HSM_STATE_START_PENDING,
        SBI_HSM_STATE_STARTED))
    sbi_hart_hang();

next_arg1 = scratch->next_arg1;
next_addr = scratch->next_addr;
next_mode = scratch->next_mode;

hsm_start_ticket_release(hdata);

sbi_hart_switch_mode(
    hartid,
    next_arg1,
    next_addr,
    next_mode,
    false);
```

可以看出：

1. Hart 8 将自己的 HSM State 从 `START_PENDING` 转为 `STARTED`。
2. 从自身 Scratch 读取之前 Hart 0 写入的 Linux 启动上下文。
3. 释放 Start Ticket。
4. 调用 Privilege Mode Switch。

这也解释了为什么 Start Ticket 不是在 Hart 0 收到 RPMI ACK 时就立即释放。

在成功启动路径中，它一直保护到目标 Hart 完成启动状态转换并读取必要的上下文。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 9.2 `sbi_hart_switch_mode()` 的底层机制

源码：

[OpenSBI — sbi_hart.c](https://github.com/riscv-software-src/opensbi/blob/master/lib/sbi/sbi_hart.c?utm_source=chatgpt.com)

其关键逻辑：

```
val = csr_read(CSR_MSTATUS);

val = INSERT_FIELD(
    val, MSTATUS_MPP, next_mode);

val = INSERT_FIELD(
    val, MSTATUS_MPIE, 0);

csr_write(CSR_MSTATUS, val);

csr_write(CSR_MEPC, next_addr);

/* 配置目标 Privilege Mode 的 Trap 和 VM 状态 */

register unsigned long a0 asm("a0") = arg0;
register unsigned long a1 asm("a1") = arg1;

__asm__ __volatile__(
    "mret" : : "r"(a0), "r"(a1));
```

当前上游实现还会对 `stvec`、`vstvec` 等 Trap Vector Address 的低模式位进行正确处理，避免 2-byte 对齐入口在包含 C 扩展的平台上被误解释为不合法的 Trap Mode。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



最核心的三个 CSR 是：

```
mstatus.MPP = S-mode
mepc        = Linux Secondary Entry
satp        = 0
```

`mret` 将处理器从 M-mode 返回到配置好的 S-mode，并从 `mepc` 继续执行。

此外，OpenSBI 还把：

```
a0 = Hart ID
a1 = Linux Boot Data Physical Address
```

传递给 Linux。

### 9.3 Linux `secondary_start_sbi`

Linux 汇编源码：

[Linux — arch/riscv/kernel/head.S](https://github.com/torvalds/linux/blob/master/arch/riscv/kernel/head.S?utm_source=chatgpt.com)

关键代码如下：

```
.global secondary_start_sbi

secondary_start_sbi:
    csrw CSR_IE, zero
    csrw CSR_IP, zero

    load_global_pointer

    li t0, SR_FS_VS
    csrc CSR_STATUS, t0

    /* Install temporary trap vector */

    /* a0 = hartid, a1 = boot data */

    li a2, SBI_HART_BOOT_TASK_PTR_OFFSET
    add a2, a2, a1
    REG_L tp, (a2)

    li a3, SBI_HART_BOOT_STACK_PTR_OFFSET
    add a3, a3, a1
    REG_L sp, (a3)

.Lsecondary_start_common:
    la a0, swapper_pg_dir
    call relocate_enable_mmu

    call .Lsetup_trap_vector

    /* Other initialization */

    call smp_callin
```

这段代码基本可以看作 Linux Secondary Hart 启动的最后一段桥梁。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 9.4 为什么 `tp` 和 `sp` 要从 Boot Data 中恢复？

在 Linux RISC-V 中，`tp` 用于与当前 Task / Thread Context 相关的访问。

而 `sp` 指向当前执行环境的 Stack。

这些东西不属于 RPMI，也不属于 PuC。

它们由 Linux 在 Hart Start 之前准备好，再通过 SBI HSM 的 `opaque` 参数传给 Secondary Hart。

因此，从硬件 Reset 一直到 Linux 执行，会出现两套完全不同的上下文：

OpenSBI M-mode Context

`mscratch` → Per-Hart SBI Scratch

`sp` → OpenSBI Stack

`mtvec` → OpenSBI Trap Handler

`mret` · M → S

Linux S-mode Context

`tp` → Linux Task Context

`sp` → Linux Kernel Stack

`stvec` → Linux Trap Handler

Linux 随后启用自己的页表，并通过 `smp_callin()` 完成 CPU 本地初始化及 SMP Online 过程。

至此，我们已经将从 Linux 发起 `HART_START` 到 Hart 8 真正加入 Linux SMP 系统的完整路径串联起来。

## Chapter 10：完整 Trace 与 Bring-up 调试方法

最后，把整个过程转换成可以在真实芯片或仿真环境中检查的观测点。

### 10.1 一次成功启动的 Trace

以下为教学示意日志，非上游软件原样输出。

```
[Linux][H0] cpu_up(cpu=8)
[Linux][H0] cpuid -> hartid = 8
[Linux][H0] secondary_start_sbi = 0x80208000

[OpenSBI][H0] SBI HSM HART_START
[OpenSBI][H0] target hart = 8
[OpenSBI][H0] domain check = OK
[OpenSBI][H0] state: STOPPED -> START_PENDING

[OpenSBI][H0] RPMI HSM_HART_START
[OpenSBI][H0] start_addr = 0x80004000

[RPMI][AP] A2P REQ token=101
[RPMI][AP] tail: 0 -> 1
[RPMI][AP] doorbell write

[PuC] RX HSM_HART_START
[PuC] target=8
[PuC] Power ON
[PuC] Clock Enable
[PuC] Reset Release
[PuC] ACK SUCCESS

[OpenSBI][H8] _start_warm
[OpenSBI][H8] mscratch initialized
[OpenSBI][H8] M-mode context ready
[OpenSBI][H8] START_PENDING -> STARTED
[OpenSBI][H8] mret -> 0x80208000

[Linux][H8] secondary_start_sbi
[Linux][H8] smp_callin
[Linux][H8] CPU Online
```

建议在调试系统中，为日志增加：

- Hart ID
- RPMI Service Group / Service ID
- Token
- Request Timestamp
- Response Timestamp
- HSM State
- PuC Hardware State

这样可以直接区分究竟是哪个软件层级出了问题。

### 10.2 故障定位矩阵

| 故障表现                        | 优先检查                                     |
| --------------------------- | ---------------------------------------- |
| OpenSBI 找不到 RPMI HSM Device | FDT、HSM Channel、BASE Discovery           |
| RPMI Request 发送失败           | A2P REQ Queue、Memory Mapping、锁           |
| Doorbell 写入后 PuC 没有响应       | Doorbell 地址、值、PuC IRQ                    |
| PuC 收到请求但启动失败               | Power/Clock/Reset Sequencing             |
| PuC ACK Success，但 Hart 无日志  | Reset Vector、M-mode Boot Address         |
| Hart 进入 `_start_warm` 后挂死   | `mscratch`、Stack、Scratch Mapping         |
| Hart 进入 `sbi_init()` 后挂死    | HSM State、Coldboot Sync、PMP              |
| `mret` 后立即 Exception        | `mepc`、Domain、Memory Protection          |
| Linux Secondary Entry 后挂死   | `a0/a1`、Stack、页表、Trap Vector             |
| CPU 能启动一次，但 Hotplug 失败      | Stop/Start State Machine、Power Retention |

### 10.3 重点检查 Shared Memory 可见性

在预硅仿真中，如果发现：

```
OpenSBI:
  tail updated successfully

PuC:
  tail still old value
```

我建议按这个顺序调查：

第一层：地址

AP 和 PuC 看到的是不是同一块物理 SRAM？

第二层：内存属性

AP 是否把该区域配置成 Cacheable？

PuC 是否把该区域当作 Device/Non-cacheable Memory？

第三层：内存顺序

Producer 是否在完成 Payload 写入后才更新 Tail？

第四层：Doorbell

PuC 是否在数据可见之前就收到中断并尝试读取？

第五层：Queue 初始化

Head/Tail 是否在正确的初始化阶段建立？是否出现多个软件实体重复清零队列？

这些问题都可以在软件日志与硬件仿真波形之间建立对应关系。

## Chapter 11：进一步理解三个 Staff Engineer 级设计问题

### 11.1 为什么 OpenSBI 需要同时维护 HSM State 和 RPMI State？

因为它们属于不同的抽象层级。

OpenSBI HSM State 表示 SBI 对上层软件暴露的 Hart Lifecycle。

PuC State 表示底层 Power/Reset Controller 对实际硬件状态的理解。

例如：

```
OpenSBI: START_PENDING
PuC:     POWERING_ON
Hardware: RESET_ASSERTED
```

这是一个完全合法的中间状态。

但如果持续出现：

```
OpenSBI: STARTED
PuC:     POWERED_OFF
```

那么就意味着系统状态存在严重的不一致。

因此，工业级设计应该明确：

- 哪个模块是物理电源状态的最终权威？
- OpenSBI 如何查询或校正状态？
- PuC Reset 后如何恢复对 Hart 的状态认识？
- AP 与 PuC 是否允许独立重启？
- 固件版本升级后如何恢复管理状态？

这已经不是普通驱动编程问题，而是跨处理器分布式状态管理问题。

### 11.2 多 Hart 同时启动时，会产生什么并发问题？

假设 Hart 0 同时请求启动 Hart 8～15。

虽然目标 Hart 不同，但它们可能共享：

```
RPMI Mailbox Controller
RPMI A2P Request Queue
RPMI P2A ACK Queue
PuC Service Dispatcher
Cluster Power Domain
Cluster Clock/PLL
```

并发控制存在至少三个层次：

第一层：Per-Hart HSM Locking。

通过 `start_ticket` 防止多个调用方同时启动同一 Hart。

第二层：RPMI Transport Queue Locking。

防止多个发送方同时修改同一个 Tail 或覆盖 Slot。

第三层：PuC Hardware Resource Locking。

例如 Hart 8 和 Hart 9 同时启动，可能会竞争同一个 Cluster PLL。

因此，Mailbox 支持并发消息，不等于硬件管理操作都能够并行执行。

还必须避免 PuC 内部的 Deadlock、优先级反转和错误的 Power Dependency Reference Counting。

### 11.3 如果 PuC 在运行时 Reset，会发生什么？

这是比正常 Hart Start 更复杂的场景。

假设 Linux 已经启动全部 16 个 Hart，随后 PuC Firmware 异常重启。

此时：

```
Application Harts:
    Linux 正常运行

OpenSBI:
    HSM States = STARTED

PuC:
    Internal Runtime State 丢失
```

PuC 重新启动时不能简单认为所有 Hart 都处于 STOPPED，也不能随意重置仍然运行中的 CPU。

一个稳健的 SoC 需要规定：

- PuC 是否允许独立 Reset？
- PuC 的 Hardware State 能否重新读取？
- RPMI Queue 的 Head/Tail 是否需要重建？
- 未完成的 Request 如何处理？
- AP 如何检测管理控制器发生了重启？
- 是否需要重新进行 BASE Service Discovery？
- 哪些操作需要故障安全恢复？

这类问题往往是服务器芯片可靠性设计与低功耗管理架构的重要组成部分。

## Chapter 12：建议动手完成的源码实验

为了真正掌握这条调用链，建议做三个由浅入深的实验。

### Experiment 1：静态追踪调用链

阅读并记录下面的函数调用关系：

```
Linux:
  sbi_cpu_start()
  sbi_hsm_hart_start()
  sbi_ecall()

OpenSBI:
  sbi_ecall_hsm_handler()
  sbi_hsm_hart_start()
  hsm_device_hart_start()
  rpmi_hsm_start()
  rpmi_normal_request_with_status()
  mbox_chan_xfer()
  rpmi_shmem_mbox_xfer()
  __smq_tx()
  __smq_rx()

Warmboot:
  _start_warm
  sbi_init()
  init_warmboot()
  init_warm_startup()
  sbi_hsm_hart_start_finish()
  sbi_hart_switch_mode()

Linux:
  secondary_start_sbi
  smp_callin()
```

在每个函数入口记录：

```
Current Hart ID
Target Hart ID
HSM State
Scratch Address
Start Address
```

这是建立源码级全链路理解的基础。

### Experiment 2：RPMI Transport Fault Injection

可以在模拟的 RPMI Server 中设置以下故障：

| Fault           | 期望验证                   |
| --------------- | ---------------------- |
| Drop ACK        | Timeout Handling       |
| Delay ACK       | Late Response          |
| Corrupt Token   | Response Matching      |
| Queue Full      | Backpressure           |
| Invalid Hart ID | Error Propagation      |
| Reset Failure   | Hardware Fault Mapping |

这个实验重点不是确认是否返回了错误码，而是确认错误发生后整个系统的状态是否仍然一致。

### Experiment 3：16-Hart 并发 Bring-up

最后进行更贴近服务器 SoC 的压力测试：

```
Hart 0 requests:
  Start Hart 1
  Start Hart 2
  ...
  Start Hart 15
```

需要分别测量：

- 从 Linux 发起 SBI 到 OpenSBI 接受请求的时间。
- RPMI Queue 等待时间。
- PuC Power-On Sequencing 时间。
- 目标 Hart Warmboot 时间。
- Linux Secondary CPU Online 时间。

建议将这些数据分开采集，避免把整个 CPU Online Latency 统称为 RPMI Latency。

## Chapter 13：Staff Engineer 级面试与架构设计题

以下题目适合用于检查是否真正掌握了这一条启动链。

1\. 并发状态一致性

Hart 0 和 Hart 1 同时请求启动 Hart 8。为什么单独使用 HSM State CAS 仍可能不足？如何设计 Start Ticket 与 Context 发布机制？

2\. RPMI Timeout 恢复

PuC 已经释放 CPU Reset，但 RPMI ACK 丢失。OpenSBI 发生超时后应如何保证最终状态一致？

3\. Hardware Reset 与 Warmboot

为什么 PuC 不应该直接将目标 Hart 启动到 Linux Secondary Entry？如何处理 M-mode 的 CSR 与保护状态？

4\. 跨处理器内存一致性

AP 使用 Write-Back Cache，而 PuC 使用 Non-cacheable SRAM Mapping。RPMI Queue 偶发读到旧数据，你如何定位并解决？

5\. Cluster Power Dependencies

四个 Hart 共享一个 Cluster PLL，其中一个 Hart 已经运行，另一个准备上电。怎样设计 PuC 的 Power Dependency Management？

6\. PuC 异常重启

Linux 已经运行 16 个 Hart，但 PuC Firmware 被 Watchdog Reset。如何恢复 RPMI 服务而不破坏正在运行的系统？

7\. Security Isolation

一个被攻陷的 S-mode 系统尝试通过 RPMI 控制不属于自身 Domain 的 Hart。在哪些层级进行权限控制？

8\. CPU Hotplug 性能优化

Linux CPU Online 需要 15 ms，你如何区分其中的 RPMI 协议开销、硬件上电时间、OpenSBI Warmboot 和 Linux SMP 初始化时间？

这些题目的共同特点是：不能只通过背诵 RPMI Specification 回答，而必须把软件状态机、RISC-V 特权架构、硬件控制与错误恢复机制结合起来考虑。

## 最终总结：把整条链路压缩成一个模型

```
Linux
  |
  | SBI HSM_HART_START
  v
OpenSBI HSM
  |
  | Validate Domain
  | Save Secondary Context
  | STOPPED -> START_PENDING
  |
  v
RPMI HSM Client
  |
  | HART_START(hartid, warmboot_addr)
  v
RPMI Shared Memory Transport
  |
  | Write Request
  | Publish Tail
  | Ring Doorbell
  v
PuC RPMI Server
  |
  | Decode Request
  | Power / Clock / Reset
  | Send ACK
  v
Target Hart
  |
  | M-mode Warmboot
  | Restore Per-Hart Context
  | START_PENDING -> STARTED
  | PMP / CSR Initialization
  |
  | mret
  v
Linux Secondary Entry
  |
  | Setup Stack / TP
  | Setup MMU / Trap
  | SMP Callin
  v
CPU Online
```

这一条链路最重要的三个边界是：

Linux → OpenSBI： 从操作系统请求转换成 SBI HSM 状态机操作。

OpenSBI → PuC： 从标准化 HSM 操作转换成 RPMI 消息和实际的物理硬件管理。

OpenSBI Warmboot → Linux： 从 M-mode Firmware 上下文重新建立 S-mode 执行环境。

掌握这三个边界以后，RPMI 就不再只是一个独立的 IPC 协议，而会成为理解 RISC-V 服务器 SoC 的 CPU Lifecycle、Power Management 与 Firmware Architecture 的一个完整切入点。

推荐规范：[RPMI Transport](https://docs.riscv.org/reference/rpmi/transport.html?utm_source=chatgpt.com) · [RPMI Service Groups](https://docs.riscv.org/reference/rpmi/service-groups.html?utm_source=chatgpt.com) · [SBI HSM Specification](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-hsm.adoc?utm_source=chatgpt.com)
