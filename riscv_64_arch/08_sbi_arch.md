下面我把这一阶段当成一章完整的课程来讲。你已经掌握了 M/S/U、Trap、Interrupt、PMP、Sv39 和 Fence，所以现在学习 SBI/OpenSBI 的时机非常合适——**SBI 正是把前面这些零散机制第一次真正串成“完整系统”的地方。**

# RISC-V Architecture 08：SBI / OpenSBI

## 0. 这一章真正要掌握什么

先给结论。

**SBI（Supervisor Binary Interface）本质上是：**

> **S-mode OS 向更高特权级软件请求“机器级服务”的标准 ABI。**

而：

**OpenSBI 是 SBI 的一个开源实现，通常运行在 M-mode。**

最典型的 RISC-V Linux 软件栈：

```text
Userspace
   |
   | syscall
   v
Linux kernel                     U → S
   |
   | SBI ECALL
   v
OpenSBI                          S → M
   |
   v
Machine-level hardware
```

因此要非常清楚地区分：

```text
syscall
    U-mode → S-mode

SBI call
    S-mode → M-mode
```

如果有 Hypervisor：

```text
Guest Linux
   |
   | SBI ECALL
   v
HS-mode hypervisor
```

这时 SBI implementation 不一定是 M-mode firmware，也可以是 HS-mode hypervisor。SBI 官方定义的 higher-privilege provider 叫 **SEE（Supervisor Execution Environment）**。官方 SBI v3.0 文档明确规定 SBI 的目标是让 S-mode 或 VS-mode 软件跨不同 RISC-V implementation 保持可移植性。:chatgpt-content-reference{index="0"}

这一章学完以后，你应该能够回答下面几个核心问题：

```text
1. 为什么 S-mode Linux 不能自己完成所有事情？

2. 为什么需要 SBI，而不能 Linux 直接访问 M-mode CSR？

3. ECALL 是怎么从 Linux 进入 OpenSBI 的？

4. OpenSBI 如何从 M-mode 返回 S-mode？

5. timer / IPI / TLB shootdown 为什么会经过 SBI？

6. secondary hart 是如何通过 SBI HSM 启动的？

7. OpenSBI 和 TF-A / PSCI / SMC 到底是什么关系？

8. OpenSBI 是 bootloader 吗？
```

其中第 8 个问题的答案先给出来：

> **OpenSBI 不是传统意义上的 bootloader。**

它更准确地说是：

> **M-mode runtime firmware + SBI service provider。**

它可以参与 boot，但 SBI runtime service 才是它真正的核心价值。

---

# 1. 为什么需要 SBI？

这是这一章最重要的问题。

假设没有 SBI。

Linux 运行：

```text
S-mode
```

但是有些硬件资源或者 CSR 只能由：

```text
M-mode
```

访问。

例如在经典 RISC-V 系统中：

```text
machine timer
machine software interrupt
hart power management
machine-level reset
PMP
某些性能计数器
```

这些资源不一定允许 S-mode 直接操作。

于是 Linux 遇到了一个问题：

```text
Linux
 S-mode
   |
   | 我要启动 CPU1
   |
   X
无法直接完成 machine-level operation
```

一种非常差的设计是：

```c
if (board == sifive)
    ...
else if (board == starfive)
    ...
else if (board == xxx)
    ...
```

让 Linux 自己知道每个平台：

```text
CPU power controller address
CLINT address
reset controller
IPI controller
timer implementation
```

这会导致：

```text
Linux
  |
  +-- board A machine code
  +-- board B machine code
  +-- board C machine code
  +-- board D machine code
```

完全失去平台抽象。

所以 RISC-V 引入一层：

```text
          Linux
            |
            | SBI
            v
       +----------+
       |   SEE    |
       | OpenSBI  |
       +----------+
          / | \
         /  |  \
        v   v   v
     timer IPI power
```

Linux 只知道：

```c
sbi_set_timer(...);
sbi_send_ipi(...);
sbi_hart_start(...);
```

至于底层到底是：

```text
CLINT
ACLINT
IMSIC
vendor timer
PMU controller
power controller
```

由 firmware 解决。

这其实就是经典的：

> **mechanism abstraction boundary**

---

# 2. SBI 本质上是一层 ABI

很多人第一次学习 SBI 时把它理解成：

> OpenSBI 提供的一组 C API。

这个理解是不准确的。

SBI 本质上是：

```text
Binary Interface
```

也就是说：

```text
register layout
+
ECALL
+
EID/FID
+
return value
```

才是 SBI。

类似：

```text
Linux syscall ABI
```

规定：

```text
哪个寄存器存 syscall number
哪个寄存器放参数
哪个寄存器返回结果
```

SBI 也做相同的事情。

现代 SBI 的调用约定是：

```text
a7 = Extension ID
a6 = Function ID

a0-a5 = arguments

ecall

返回：

a0 = error
a1 = value
```

官方 SBI v0.2 之后的 binary encoding 就是如此；除 `a0/a1` 外，其余寄存器要求由 callee 保持。:chatgpt-content-reference{index="1"}

可以画成：

```text
          SBI call
────────────────────────────

a7 = EID
a6 = FID

a0 = arg0
a1 = arg1
a2 = arg2
a3 = arg3
a4 = arg4
a5 = arg5

        ECALL
          |
          v

a0 = error
a1 = return value
```

这就是 SBI 最核心的 ABI。

---

# 3. EID / FID 是什么？

SBI 采用模块化 extension 模型。

例如：

```text
TIME
IPI
RFENCE
HSM
SRST
PMU
DBCN
...
```

每个 extension 有：

```text
EID = Extension ID
```

extension 里面再包含：

```text
FID = Function ID
```

例如 HSM：

```text
EID = "HSM"
```

内部：

```text
FID 0 → hart_start
FID 1 → hart_stop
FID 2 → hart_get_status
FID 3 → hart_suspend
```

所以逻辑结构是：

```text
SBI
 |
 +-- BASE
 |
 +-- TIME
 |    |
 |    +-- set_timer()
 |
 +-- IPI
 |    |
 |    +-- send_ipi()
 |
 +-- RFENCE
 |
 +-- HSM
 |    |
 |    +-- hart_start()
 |    +-- hart_stop()
 |    +-- hart_get_status()
 |    +-- hart_suspend()
 |
 +-- SRST
 |
 +-- PMU
 |
 +-- DBCN
```

这非常符合 RISC-V：

> small core + extensions

的设计理念。

截至 2026 年当前官方在线 SBI 规范已指向 **SBI v3.0**；除了传统的 TIME/IPI/RFENCE/HSM 等，还包含 DBCN、SUSP、NACL、STA、SSE、FWFT、DBTR、MPXY 等 extension。:chatgpt-content-reference{index="2"}

不过我们学习时**绝对不要从这些新 extension 开始**。

第一阶段只需要掌握：

```text
BASE
TIME
IPI
RFENCE
HSM
SRST
```

即可。

---

# 4. 一次 SBI call 到底发生了什么？

这是这一章真正需要吃透的第一条代码路径。

假设 Linux 想设置 timer：

```c
sbi_set_timer(next);
```

Linux 最终准备：

```text
a7 = SBI_EXT_TIME
a6 = SBI_EXT_TIME_SET_TIMER

a0 = next_timer_value
```

然后：

```asm
ecall
```

现在 CPU 在：

```text
S-mode
```

执行：

```asm
ecall
```

会产生：

```text
Environment call from S-mode
```

也就是：

```text
mcause = Supervisor ECALL
```

由于这个 exception 没有 delegate 给 S-mode：

```text
S-mode
   |
   | ecall
   v
M-mode
```

CPU 硬件完成类似：

```text
mepc     ← ECALL PC

mcause   ← ECALL from S-mode

mstatus.MPP ← S

PC       ← mtvec
```

于是进入 OpenSBI 的 M-mode trap handler。

可以画成：

```text
Linux
S-mode
  |
  | a7 = TIME
  | a6 = SET_TIMER
  | a0 = deadline
  |
  | ECALL
  v
──────────────── privilege boundary
  |
  v
M-mode trap
  |
  +-- mcause = S-mode ECALL
  |
  v
OpenSBI trap entry
  |
  v
sbi_trap_handler()
  |
  v
sbi_ecall_handler()
  |
  v
TIME extension handler
  |
  v
timer driver
```

OpenSBI 当前代码中 `sbi_trap_handler()` 会检查 `mcause`；对于 `CAUSE_SUPERVISOR_ECALL`，进入 `sbi_ecall_handler()`。:chatgpt-content-reference{index="3"}

所以 OpenSBI 的核心路径可以记成：

```text
ECALL
 ↓
mtvec
 ↓
trap entry
 ↓
sbi_trap_handler()
 ↓
sbi_ecall_handler()
 ↓
extension handler
 ↓
platform/device operation
```

---

# 5. SBI 与普通 exception 有什么特殊之处？

这里正好可以复用你前面学习 Trap 的知识。

普通：

```text
page fault
```

通常：

```text
U
 ↓
S-mode Linux
```

因为：

```text
medeleg
```

可以 delegate 给 S-mode。

但是：

```text
S-mode ECALL
```

必须进入 SBI provider。

所以 OpenSBI 并不会把这个 exception delegation 给 S-mode。

这很好理解：

如果：

```text
S-mode ECALL
```

又回到了：

```text
S-mode
```

那 SBI 根本没法工作。

OpenSBI 的 trap delegation 代码里可以看到，它会 delegate 很多 S-mode 可处理的 trap，但 SBI 所需的 S-mode ECALL 仍然留给 M-mode firmware。:chatgpt-content-reference{index="4"}

所以你的 mental model 是：

```text
              Trap
                |
        +-------+-------+
        |               |
     delegated       not delegated
        |               |
        v               v
     S-mode           M-mode
     Linux            OpenSBI
```

例如：

```text
U-mode page fault
        ↓
      Linux

S-mode ECALL
        ↓
     OpenSBI
```

---

# 6. OpenSBI 到底是什么？

现在把 SBI 与 OpenSBI 完全分开。

```text
SBI
=
Specification
```

而：

```text
OpenSBI
=
Implementation
```

和：

```text
POSIX            glibc
规范              实现
```

或者：

```text
UEFI spec        EDK2
```

类似。

OpenSBI 本身核心可以理解成：

```text
                OpenSBI
                   |
       +-----------+-----------+
       |                       |
    libsbi.a                platform
       |                       |
       +-----------+-----------+
                   |
                   v
              SBI runtime
```

OpenSBI 官方实际上提供：

```text
libsbi.a

libplatsbi.a
```

`libsbi.a` 是 platform-independent SBI implementation；`libplatsbi.a` 把平台 hooks 和 libsbi 整合起来。OpenSBI 也允许外部 M-mode firmware 只链接 `libsbi.a`，自己提供 platform-specific hooks。:chatgpt-content-reference{index="5"}

所以：

```text
OpenSBI ≠ 单纯一个 boot binary
```

更准确：

```text
OpenSBI
=
SBI runtime library
+
platform abstraction
+
reference firmware
```

---

# 7. 用你的 TF-A 经验理解 OpenSBI

可以先建立这个近似模型：

```text
ARM
================================================

Linux EL1
   |
   | SMC
   v
TF-A BL31
EL3
   |
   v
platform firmware


RISC-V
================================================

Linux S-mode
   |
   | ECALL
   v
OpenSBI
M-mode
   |
   v
platform firmware
```

所以：

```text
OpenSBI ≈ TF-A BL31
```

这是一个**非常有用但不完全等价**的类比。

进一步：

```text
SMCCC
≈
SBI calling convention
```

以及：

```text
PSCI
≈
SBI HSM + SRST + suspend 一部分功能
```

例如 Arm：

```text
PSCI_CPU_ON
```

RISC-V：

```text
sbi_hart_start()
```

Arm：

```text
PSCI_CPU_OFF
```

RISC-V：

```text
sbi_hart_stop()
```

Arm：

```text
SYSTEM_RESET
```

RISC-V：

```text
SBI SRST
```

但是不能说：

```text
SBI == PSCI
```

因为 SBI 范围比 PSCI 大得多：

```text
timer
IPI
remote fence
PMU
debug console
...
```

所以更准确的关系：

```text
                 SBI
                  |
    +-------------+-------------+
    |             |             |
   HSM           TIME          IPI
    |
 roughly PSCI-like
```

而不是：

```text
SBI = PSCI
```

---

# 8. OpenSBI 启动时到底干了什么？

现在来看 boot。

典型：

```text
RESET
  |
  v
BootROM / FSBL
  |
  v
OpenSBI
M-mode
  |
  v
U-Boot / Linux
S-mode
```

或者 QEMU 上：

```text
QEMU
 |
 |- OpenSBI
 |
 `- Linux
```

OpenSBI 启动以后需要完成大量 machine-level 初始化：

```text
per-hart scratch
stack
trap vector
PMP
trap delegation
timer
IPI
interrupt controller
console
HSM
domain
...
```

然后才把 CPU 切换到：

```text
S-mode
```

进入下一阶段。

核心操作本质上类似：

```c
mepc = linux_entry;

mstatus.MPP = S;

a0 = hartid;
a1 = dtb_address;

mret;
```

也就是说：

```text
OpenSBI
M-mode
   |
   | configure M-mode environment
   |
   | mepc = Linux entry
   | MPP  = S
   |
   | mret
   v
Linux
S-mode
```

这个机制与你非常熟悉的：

```text
TF-A EL3
   |
   | configure SPSR
   | configure ELR
   |
   | ERET
   v
Linux EL1
```

高度类似。

---

# 9. OpenSBI 为什么退出到 S-mode 之后还不能消失？

这是理解 OpenSBI 的关键。

OpenSBI 启动 Linux：

```text
M → S
```

但它不是像普通 bootloader 一样：

```text
jump Linux
然后结束
```

OpenSBI 的代码仍然留在：

```text
M-mode firmware region
```

之后 Linux 每次：

```asm
ecall
```

都会重新进入 OpenSBI。

所以整个运行期间：

```text
Linux
  |
  | ECALL
  v
OpenSBI
  |
  | MRET
  v
Linux
```

可能反复发生几百万次。

因此：

> **OpenSBI 是 runtime firmware，而不仅仅是 boot firmware。**

---

# 10. 第一条核心 SBI 服务：TIME

现在进入真正的 SBI service。

传统系统里面 timer 通常存在一个问题：

Linux 在：

```text
S-mode
```

但某些平台的 timer compare mechanism 由：

```text
M-mode
```

控制。

于是 Linux 不能直接：

```text
write machine timer compare
```

所以：

```text
Linux
 |
 | sbi_set_timer()
 v
OpenSBI
 |
 v
timer hardware
```

调用：

```c
sbi_set_timer(stime_value);
```

`stime_value` 是 absolute time。官方 TIME extension 定义该接口，并要求设定未来 timer event 时清除当前 pending timer interrupt。:chatgpt-content-reference{index="6"}

完整路径：

```text
Linux scheduler/timer
       |
       v
clockevent driver
       |
       v
sbi_set_timer()
       |
       | ECALL
       v
OpenSBI TIME
       |
       v
platform timer
```

等 timer 到达：

```text
timer hardware
      |
      v
M-mode timer interrupt
      |
      v
OpenSBI
      |
      v
inject/delegate S timer
      |
      v
Linux timer interrupt
```

这背后其实就是：

> M-mode firmware 对 machine-level timer 的 virtualization。

---

# 11. 但是为什么现在有 Sstc 后可能不需要 TIME？

这是现代 RISC-V 很重要的发展。

如果硬件提供：

```text
Sstc
```

也就是：

```text
stimecmp
```

那么 S-mode 可以直接：

```text
write stimecmp
```

此时：

```text
Linux
  |
  | CSR write
  v
stimecmp
```

而不需要：

```text
Linux
 ↓
SBI
 ↓
OpenSBI
 ↓
timer
```

所以 SBI 里面存在一个非常重要的设计趋势：

> **能安全直接暴露给 S-mode 的硬件能力，尽量让 S-mode 直接使用；SBI 只承担必须由 higher privilege 完成的服务。**

当前 RISC-V Boot and Runtime Services 规范也体现了这一点：如果 Sstc 存在，TIME extension 不再是必须条件。:chatgpt-content-reference{index="7"}

这点非常值得从架构设计角度理解。

---

# 12. 第二条核心服务：IPI

在 SMP 中：

```text
CPU0
```

经常需要通知：

```text
CPU1
```

例如：

```text
reschedule
TLB shootdown
call_function
stop CPU
```

于是：

```text
CPU0 Linux
    |
    | SBI send_ipi
    v
OpenSBI
    |
    v
IPI controller
    |
    v
CPU1
```

现代 SBI IPI：

```c
sbi_send_ipi(
    hart_mask,
    hart_mask_base
);
```

目标 hart 最终观察到：

```text
Supervisor Software Interrupt
```

官方 SBI IPI extension 就是用 hart mask 指定目标 hart，并让接收端看到 supervisor software interrupt。:chatgpt-content-reference{index="8"}

所以：

```text
Linux logical CPU
      ↓
hartid
      ↓
SBI IPI
      ↓
M-mode IPI mechanism
      ↓
target hart
```

---

# 13. 为什么 IPI 不应该简单理解成“OpenSBI 发中断”？

这里需要再深入一步。

旧平台可能是：

```text
CLINT
 |
 `-- MSIP
```

所以只有 M-mode 能比较方便地产生 machine software interrupt。

OpenSBI 就负责：

```text
machine software interrupt
       ↓
supervisor software interrupt
```

但现代：

```text
AIA / IMSIC
```

可能允许 S-mode 拥有更直接的 interrupt mechanism。

因此和 TIME 一样：

```text
旧系统：
Linux → OpenSBI → hardware

新系统：
Linux → hardware
```

这反映的是 SBI 的本质：

> SBI 不是为了“多一层 firmware”。

而是：

> **填补 Supervisor 与平台机器能力之间的 privilege gap。**

如果 gap 消失：

```text
SBI service
```

就可能不再是必要路径。

---

# 14. 第三条核心服务：RFENCE

RFENCE：

```text
Remote Fence
```

这个 extension 对理解 SMP 特别重要。

假设：

```text
CPU0
```

修改了一个 process 的 page table。

但是：

```text
CPU1
```

TLB 中可能还保存旧 translation。

所以 Linux 必须：

```text
TLB shootdown
```

Arm 上你熟悉：

```text
TLBI ...
DSB
IPI / broadcast
```

RISC-V 本地可以：

```asm
sfence.vma
```

但是：

```text
CPU0 sfence.vma
```

只影响：

```text
CPU0
```

它不能神奇地清掉：

```text
CPU1 TLB
```

于是：

```text
CPU0
 |
 | remote_sfence_vma
 v
OpenSBI
 |
 +---- IPI ----> CPU1
 |
              sfence.vma
```

这就是：

```text
RFENCE
```

的核心作用。

典型接口：

```text
remote_fence_i
remote_sfence_vma
remote_sfence_vma_asid
```

H extension 后还包括：

```text
remote_hfence_gvma
remote_hfence_vvma
...
```

OpenSBI 和 Linux 当前代码中仍可以清楚看到这些 RFENCE 路径。:chatgpt-content-reference{index="9"}

所以可以建立一个非常清晰的关系：

```text
SFENCE.VMA
=
local hart translation fence

SBI RFENCE
=
remote hart translation fence orchestration
```

---

# 15. RFENCE 与 IPI 的关系

你可能马上想到：

> Linux 自己发 IPI，让 remote CPU 执行 sfence.vma 不就可以了吗？

没错。

实际上逻辑确实就是：

```text
CPU0
 |
 | IPI
 v
CPU1
 |
 | sfence.vma
 v
done
```

RFENCE 提供的是：

> **一个标准化的 higher-privilege remote fence primitive。**

OpenSBI 可以根据平台实现最有效的方式。

所以：

```text
RFENCE
```

不是一条特殊 CPU instruction。

而是：

```text
system-level operation
```

---

# 16. 第四条，也是最重要的一条：HSM

HSM：

```text
Hart State Management
```

如果你熟悉 PSCI：

> **HSM 是最容易通过 PSCI 来理解的 SBI extension。**

主要接口：

```c
sbi_hart_start()
sbi_hart_stop()
sbi_hart_get_status()
sbi_hart_suspend()
```

官方 HSM 定义了一套 hart state machine，包括：

```text
STARTED
STOPPED
START_PENDING
STOP_PENDING
SUSPENDED
SUSPEND_PENDING
RESUME_PENDING
``` :chatgpt-content-reference{index="10"}


其中最重要的是：

```c
sbi_hart_start(
    hartid,
    start_addr,
    opaque
);
```

---

# 17. Linux 如何通过 HSM 启动 secondary CPU？

假设：

```text
hart0 = boot hart
hart1 = secondary hart
```

Linux 在 hart0：

```text
CPU0 Linux
    |
    | sbi_hart_start(
    |     hart1,
    |     secondary_start_addr,
    |     opaque
    | )
    v
OpenSBI
```

OpenSBI 设置：

```text
hart1 next_addr
hart1 next_mode
hart1 next_arg
```

然后请求平台：

```text
power-on hart1
```

或者：

```text
IPI wakeup hart1
```

OpenSBI 当前的 HSM 实现确实会记录：

```text
next_arg1
next_addr
next_mode
```

然后根据平台是否支持真正的 hart hotplug / secondary boot，选择硬件 start 或 IPI/warmboot 路径。:chatgpt-content-reference{index="11"}

最终 hart1：

```text
M-mode
   |
   | OpenSBI warm boot
   v
configure machine state
   |
   v
switch S-mode
   |
   v
secondary_start_addr
```

---

# 18. `hart_start()` 为什么 start_addr 是 physical address？

这是非常好的架构问题。

官方规定 secondary hart 开始 S-mode execution 时：

```text
satp = 0
sstatus.SIE = 0

a0 = hartid
a1 = opaque
``` :chatgpt-content-reference{index="12"}


所以：

```text
MMU OFF
```

因此：

```text
start_addr
```

必须是：

```text
physical address
```

流程：

```text
hart1
 |
 | MMU off
 v
physical secondary entry
 |
 | establish kernel state
 |
 | enable satp
 v
normal Linux VA
```

这和很多 OS secondary CPU bootstrap 都是同样思想：

> **先进入一个确定的 physical boot environment，再建立完整 virtual memory context。**

---

# 19. HSM 与 PSCI_CPU_ON 的真正对应关系

可以做一个相对准确的对照：

| Arm | RISC-V |
|---|---|
| PSCI CPU_ON | SBI HSM hart_start |
| PSCI CPU_OFF | SBI HSM hart_stop |
| CPU_SUSPEND | SBI HSM hart_suspend |
| AFFINITY_INFO | HSM get_status 有部分相似性 |
| SYSTEM_RESET | SBI SRST |
| EL3 firmware | M-mode firmware |
| SMC | ECALL |

但是最重要的一点：

```text
PSCI
```

定义的主要是：

```text
power / CPU management
```

而：

```text
SBI
```

定义的是：

```text
Supervisor Execution Environment interface
```

所以 SBI 更宽。

---

# 20. SRST：system reset

这部分比较简单。

Linux 想：

```text
shutdown
reboot
```

但 reset controller 通常属于 platform-level hardware。

所以：

```text
Linux
 |
 | SBI SRST
 v
OpenSBI
 |
 v
reset driver
 |
 v
hardware
```

Linux 当前 RISC-V SBI 代码会探测 SRST extension，并把它注册成 reboot/poweroff 后端。:chatgpt-content-reference{index="13"}

所以：

```text
reboot()
```

最终可能走：

```text
Linux
 ↓
SBI ECALL
 ↓
OpenSBI
 ↓
platform reset controller
```

---

# 21. BASE extension 为什么非常重要？

BASE 不做具体硬件操作。

它主要解决：

```text
“这个 SBI implementation 到底支持什么？”
```

例如：

```text
get_spec_version
get_impl_id
get_impl_version
probe_extension
get_mvendorid
get_marchid
get_mimpid
```

其中最关键：

```c
sbi_probe_extension(EID);
```

Linux boot 时会：

```text
probe TIME
probe IPI
probe RFENCE
probe HSM
probe SRST
...
```

官方 BASE extension 的设计目标本来就是一个极小的 discovery mechanism。:chatgpt-content-reference{index="14"}

Linux 当前代码也是：

```text
get SBI version

probe TIME
probe IPI
probe RFENCE
probe SRST
probe DBCN
probe FWFT
...
``` :chatgpt-content-reference{index="15"}


这其实和：

```text
CPUID
feature detection
```

有相似思想：

> 不假设 feature 一定存在，而是 runtime probe。

---

# 22. OpenSBI 内部架构应该怎么理解？

不要一上来读所有代码。

建议形成下面这张图：

```text
                Linux
                  |
                ECALL
                  |
                  v
        +--------------------+
        | trap entry         |
        +--------------------+
                  |
                  v
        +--------------------+
        | sbi_trap_handler   |
        +--------------------+
                  |
                  v
        +--------------------+
        | sbi_ecall_handler  |
        +--------------------+
                  |
       +----------+----------+
       |          |          |
      TIME       HSM        IPI
       |          |          |
       v          v          v
    timer       hsm        ipi
    subsystem   subsystem   subsystem
       \          |          /
        \         |         /
         +--------+--------+
                  |
                  v
          platform / drivers
                  |
                  v
               hardware
```

所以代码阅读建议：

```text
第一层：

sbi_trap.c
sbi_ecall.c
```

然后：

```text
第二层：

sbi_timer.c
sbi_ipi.c
sbi_hsm.c
sbi_system.c
```

然后才：

```text
第三层：

drivers/
platform/
lib/utils/
```

不要反过来。

---

# 23. `sbi_scratch` 为什么非常重要？

理解 OpenSBI 代码时你很快会遇到：

```c
struct sbi_scratch
```

它本质上是：

> **OpenSBI 的 per-hart runtime context。**

可以先粗略理解：

```text
hart0
 |
 +-- scratch0

hart1
 |
 +-- scratch1

hart2
 |
 +-- scratch2
```

然后：

```text
mscratch
```

指向当前 hart 对应的：

```text
sbi_scratch
```

这和 OS 中：

```text
per-cpu data
```

非常类似。

OpenSBI 官方也要求外部 firmware 调用 OpenSBI library 时，`mscratch` 必须指向当前 hart 的有效 `sbi_scratch`，并且每个 hart 要有独立 stack。:chatgpt-content-reference{index="16"}

所以：

```text
mscratch
   |
   v
per-hart OpenSBI context
```

这是阅读代码必须掌握的一个 anchor。

---

# 24. 为什么 OpenSBI 需要 PMP？

因为：

```text
OpenSBI
```

自己运行：

```text
M-mode
```

Linux：

```text
S-mode
```

如果 firmware memory 没保护：

```text
Linux
 |
 | accidental / malicious access
 v
OpenSBI memory
```

可能破坏 firmware。

于是 OpenSBI 可以利用：

```text
PMP
```

划分：

```text
OpenSBI firmware region

S-mode memory region

MMIO
...
```

官方当前 OpenSBI platform requirements 也明确指出：PMP 并非绝对要求，但如果硬件没有 PMP，OpenSBI 就无法保护 M-mode firmware 和 secure memory region。:chatgpt-content-reference{index="17"}

这也正好把：

```text
第 05 阶段 PMP
```

和 SBI 串起来了。

---

# 25. OpenSBI 与 Device Tree 的关系

SBI 不负责 hardware discovery。

这是一个经常混淆的问题。

SBI：

```text
提供 service interface
```

DT/ACPI：

```text
描述 hardware
```

官方 SBI 明确规定 hardware discovery 不属于 SBI specification；S-mode 软件仍然通过 DT 或 ACPI 获取平台描述。:chatgpt-content-reference{index="18"}

所以：

```text
SBI
≠
Device Tree
```

例如 Linux 仍然通过 DT 知道：

```text
memory
UART
PLIC
PCIe
virtio
CPU topology
```

但需要 machine privilege 服务时：

```text
Linux → SBI
```

---

# 26. OpenSBI 的 FW_DYNAMIC / FW_JUMP / FW_PAYLOAD

学习 OpenSBI 时很容易被这三个名字绕晕。

其实非常简单。

## FW_JUMP

```text
OpenSBI
   |
   | jump fixed address
   v
next stage
```

也就是：

```text
next stage 已经在 RAM
```

OpenSBI 只需要知道入口。

例如：

```text
OpenSBI
   ↓
Linux @ 0x80200000
```

FW_JUMP 需要配置 next-stage address 或 offset。:chatgpt-content-reference{index="19"}

---

## FW_PAYLOAD

```text
OpenSBI image
    |
    +-- OpenSBI
    |
    `-- Linux/U-Boot payload
```

也就是直接：

```text
embed
```

下一阶段。

官方支持：

```text
FW_PAYLOAD_PATH=<linux Image>
```

把 Linux 或 U-Boot 直接嵌进去。:chatgpt-content-reference{index="20"}

---

## FW_DYNAMIC

最灵活：

```text
previous boot stage
       |
       +-- OpenSBI
       |
       +-- next stage
       |
       `-- dynamic_info
```

previous stage 告诉 OpenSBI：

```text
next stage 在哪里
next mode 是什么
```

地址通过：

```text
a2
```

传递 `fw_dynamic_info`。:chatgpt-content-reference{index="21"}

所以可以一句话记忆：

```text
FW_JUMP
= next address 编译时确定

FW_PAYLOAD
= next image 直接嵌入 OpenSBI

FW_DYNAMIC
= previous stage 运行时告诉 OpenSBI
```

---

# 27. QEMU 上最常见的启动关系

你实际做实验时通常：

```text
qemu-system-riscv64
        |
        v
OpenSBI
        |
        v
Linux
```

一个非常典型的结构：

```text
0x80000000
     |
     +-- OpenSBI

0x80200000
     |
     +-- Linux Image
```

所以日志里经常看到：

```text
Firmware Base ...
Domain0 Next Address ...
Domain0 Next Mode : S-mode
```

你真正应该关注的是：

```text
Firmware Base
Firmware Size

Boot HART ID

Next Address
Next Mode

PMP
MIDELEG
MEDELEG
```

因为这些字段直接描述：

```text
M-mode → S-mode
```

的系统状态。

---

# 28. Linux SBI 代码应该怎么看？

你下一步真正值得看的 Linux 文件是：

```text
arch/riscv/kernel/sbi.c
```

不用全部读。

先抓几个入口：

```c
sbi_init()
```

然后：

```c
sbi_probe_extension()
```

再看：

```c
sbi_set_timer()
sbi_send_ipi()
sbi_remote_sfence_vma()
```

Linux 当前 `sbi_init()` 会读取 SBI specification version，然后探测 TIME、IPI、RFENCE、SRST、DBCN 等 extension。:chatgpt-content-reference{index="22"}

所以 Linux 视角实际上非常清楚：

```text
Linux boot
    |
    v
sbi_init
    |
    +-- probe SBI
    |
    +-- select timer implementation
    |
    +-- select IPI implementation
    |
    +-- select RFENCE implementation
    |
    `-- register reset/debug etc.
```

---

# 29. SBI 和 syscall 的完整对照

这张表值得牢牢记住。

| Linux syscall | SBI |
|---|---|
| U-mode → S-mode | S-mode → M-mode |
| application request | OS request |
| kernel provides service | firmware provides service |
| syscall number | EID/FID |
| `ecall` | `ecall` |
| trap into kernel | trap into SBI implementation |

所以：

```asm
ecall
```

本身没有说：

```text
“这是 syscall”
```

还是：

```text
“这是 SBI”
```

它只是：

> **environment call**

真正含义由：

```text
当前 privilege level
```

决定。

例如：

```text
U-mode ECALL
      ↓
scause = Environment call from U
      ↓
Linux syscall

S-mode ECALL
      ↓
mcause = Environment call from S
      ↓
OpenSBI
```

这就是 RISC-V `ECALL` 最本质的设计。

---

# 30. 这也解释了 ECALL 和 ERET/MRET/SRET 的区别

你之前问过：

> ECALL 是否类似 Arm ERET？

答案现在应该彻底清楚：

```text
ECALL
=
进入 higher privilege

MRET / SRET
=
从 trap 返回
```

Arm 对应：

```text
SVC / SMC / HVC
≈
ECALL
```

而：

```text
ERET
≈
MRET / SRET
```

所以：

```text
Linux
  |
  | ECALL
  v
OpenSBI
  |
  | MRET
  v
Linux
```

就类似：

```text
Linux EL1
  |
  | SMC
  v
EL3 firmware
  |
  | ERET
  v
Linux EL1
```

---

# 31. H Extension 出现以后，SBI 发生什么变化？

这部分先建立概念，不需要这一阶段深入。

没有 H extension：

```text
Linux S
   |
   | SBI
   v
OpenSBI M
```

有 virtualization：

```text
Guest Linux VS
      |
      | SBI
      v
Hypervisor HS
```

此时 hypervisor 可以：

```text
emulate SBI
```

然后必要时：

```text
HS
 |
 | SBI
 v
M-mode OpenSBI
```

形成：

```text
Guest Linux
   VS
    |
    | virtual SBI
    v
Hypervisor
   HS
    |
    | physical SBI
    v
OpenSBI
    M
```

这其实与你熟悉的：

```text
Guest OS
 ↓
virtual PSCI
 ↓
KVM
 ↓
host firmware
```

理念一致。

官方 SBI 定义本身就允许 SEE 是 M-mode runtime firmware，也允许是 HS-mode hypervisor。:chatgpt-content-reference{index="23"}

---

# 32. OpenSBI 与 Secure Monitor 是否等价？

**不等价。**

这是安全方向一定要避免的误区。

Arm：

```text
EL3
+
Secure state
+
Non-secure state
+
TrustZone
```

而普通 RISC-V：

```text
M-mode
```

只是：

```text
highest privilege mode
```

并不自动意味着：

```text
Secure World
```

所以：

```text
OpenSBI
≈
EL3 firmware
```

只是在：

```text
privileged runtime firmware
```

这个层面相似。

不能推出：

```text
OpenSBI = TrustZone Secure Monitor
```

TEE 仍然需要额外的：

```text
PMP/ePMP
domain isolation
Keystone-like architecture
vendor security extension
confidential computing mechanism
...
```

---

# 33. OpenSBI Domain 是什么？

这是你以后研究 RISC-V security 时值得重点关注的概念。

OpenSBI 可以把不同 hart 和 memory region 分进：

```text
domain
```

例如：

```text
Domain A
 |
 +-- hart0
 +-- hart1
 +-- DRAM region A

Domain B
 |
 +-- hart2
 +-- memory region B
```

借助 PMP 等机制控制：

```text
哪个 domain
可以访问
哪些 memory region
```

不过不要把它直接等同：

```text
TrustZone world
```

它更接近：

> OpenSBI firmware 内部的资源隔离与运行域抽象。

这一块建议放到后面的：

```text
13 Security / TEE
```

再展开。

---

# 34. 一个完整例子：Linux 设置 timer

现在把所有东西串一次。

```text
CPU running Linux
S-mode
```

Linux scheduler 计算：

```text
next event = T
```

调用：

```c
sbi_set_timer(T);
```

wrapper：

```text
a0 = T
a6 = SET_TIMER
a7 = TIME
```

执行：

```asm
ecall
```

CPU：

```text
mepc   = ecall address
mcause = supervisor ecall
MPP    = S
PC     = mtvec
```

OpenSBI：

```text
trap entry
   ↓
save context
   ↓
sbi_trap_handler
   ↓
sbi_ecall_handler
   ↓
TIME handler
   ↓
timer driver
```

然后：

```text
a0 = SBI_SUCCESS
a1 = ...
mepc += 4
```

最后：

```asm
mret
```

于是：

```text
OpenSBI M
     |
     | MRET
     v
Linux S
```

Linux 从：

```text
ECALL 下一条 instruction
```

继续执行。

这就是一个完整 SBI transaction。

---

# 35. 一个完整例子：SMP CPU_ON

现在再串一次更有价值的：

```text
Linux boot CPU0
```

发现 DT：

```text
CPU0
CPU1
CPU2
CPU3
```

Linux 要启动 CPU1：

```c
sbi_hart_start(
    hartid1,
    secondary_start,
    opaque
);
```

流程：

```text
CPU0 Linux
S-mode
      |
      | ECALL HSM
      v
CPU0 OpenSBI
M-mode
      |
      | set hart1:
      | next_addr
      | next_arg
      | state = START_PENDING
      |
      | hardware / IPI
      v

CPU1 wakes
M-mode
      |
      | OpenSBI warmboot
      |
      | PMP / machine state
      |
      | state = STARTED
      |
      | next_mode = S
      v
secondary_start
S-mode
      |
      v
Linux secondary CPU init
```

这条路径几乎就是你之前学习：

```text
PSCI CPU_ON
```

时那条路径的 RISC-V 版本。

---

# 36. Cold boot 与 warm boot

OpenSBI SMP 代码里经常出现：

```text
coldboot
warmboot
```

不要理解成：

```text
系统冷启动 / 系统重启
```

这里主要是 hart 视角。

## cold boot hart

通常第一个负责初始化整个 firmware environment 的 hart：

```text
hart0
 |
 +-- global initialization
 +-- platform init
 +-- device init
 +-- domain init
 +-- ...
```

## warm boot hart

其它 hart：

```text
hart1
hart2
hart3
```

通常不会重新做所有 global initialization。

而是：

```text
per-hart init
 ↓
wait HSM
 ↓
enter next stage
```

所以：

```text
cold boot
≈ bootstrap processor initialization

warm boot
≈ secondary hart firmware entry
```

---

# 37. 为什么 OpenSBI 需要 trap emulation？

OpenSBI 不只是：

```text
SBI API server
```

它还有另一个非常重要的 firmware 功能：

```text
instruction / CSR emulation
```

例如硬件缺某些行为时：

```text
illegal instruction
misaligned access
CSR access
```

OpenSBI 有时可以在 M-mode trap 中：

```text
emulate
```

再返回 S-mode。

当前 OpenSBI `sbi_trap_handler()` 除 ECALL 外，也处理 illegal instruction、misaligned load/store 等情况。:chatgpt-content-reference{index="24"}

所以 OpenSBI 是：

```text
SBI provider
+
machine-mode runtime
+
部分 trap emulation
+
platform abstraction
```

而不仅是：

```text
ECALL dispatcher
```

---

# 38. SBI v0.1 与现代 SBI 为什么看起来不一样？

你阅读旧代码时经常会碰到：

```text
SBI v0.1
```

老 SBI：

```text
a7
```

直接表示 function/extension。

没有现代清晰的：

```text
EID
+
FID
```

分层。

现代 SBI 从 v0.2 开始：

```text
a7 = EID
a6 = FID
```

旧 legacy extensions 现在已经 deprecated；官方文档也明确推荐 TIME、IPI、RFENCE、SRST、DBCN 等现代 extension 取代 legacy 接口。:chatgpt-content-reference{index="25"}

所以学习源码时看到：

```text
SBI_EXT_0_1_xxx
```

你的反应应该是：

> compatibility path，不是现代 SBI 的主设计。

---

# 39. 当前 SBI 与 OpenSBI 版本不要混淆

截至 2026 年 10 月，官方在线 SBI 文档当前指向：

```text
SBI specification v3.0
``` :chatgpt-content-reference{index="26"}


而 OpenSBI 是独立项目，其当前 release 页面最新为：

```text
OpenSBI v1.9
```

发布于 2026 年 7 月。:chatgpt-content-reference{index="27"}

因此：

```text
SBI v3.0
```

和：

```text
OpenSBI v1.9
```

完全不是同一个版本体系。

类似：

```text
UEFI specification version

≠

EDK2 release version
```

---

# 40. 你应该如何读 OpenSBI 源码？

按照下面这个顺序，效率最高。

第一轮只看：

```text
lib/sbi/sbi_trap.c
```

目标：

```text
trap 如何进入 SBI dispatcher
```

然后：

```text
sbi_ecall.c
```

目标：

```text
EID/FID 如何 dispatch
```

然后：

```text
sbi_hsm.c
```

目标：

```text
secondary hart 如何启动
```

然后：

```text
sbi_timer.c
sbi_ipi.c
```

目标：

```text
timer / IPI virtualization
```

然后：

```text
firmware/
```

目标：

```text
OpenSBI 如何从 reset 走到 next stage
```

最后才看：

```text
platform/
lib/utils/
```

目标：

```text
具体 hardware driver
```

千万不要一上来：

```text
grep platform
```

然后陷入几千行 board-specific code。

---

# 41. 我建议你亲手做的第一个实验

环境：

```text
QEMU virt
+
OpenSBI
+
Linux
+
GDB
```

官方 OpenSBI QEMU 文档本身就支持用 `-S` 和 GDB 在第一条指令暂停调试。:chatgpt-content-reference{index="28"}

实验目标不是：

```text
把 Linux boot 成功
```

而是打断点：

```text
sbi_trap_handler
```

然后 Linux 起来后观察：

```text
ECALL
```

进入 M-mode。

重点看：

```text
mcause
mepc
mstatus

a0
a1
a6
a7
```

第一次实验只需要验证：

```text
Linux SBI call
     ↓
OpenSBI trap
```

就非常有价值。

---

# 42. 第二个实验：trace timer SBI

打断点：

```text
sbi_set_timer
```

或者 OpenSBI TIME handler。

观察：

```text
Linux
 ↓
ECALL
 ↓
OpenSBI
 ↓
timer
```

特别关注：

```text
a7 = TIME EID
a6 = SET_TIMER FID
a0 = deadline
```

你会第一次非常直观地看到：

> SBI 不是抽象概念，而就是一次真实 privilege transition。

---

# 43. 第三个实验：4-hart SMP

QEMU：

```text
-smp 4
```

观察 Linux 启动：

```text
CPU0
CPU1
CPU2
CPU3
```

然后在：

```text
sbi_hsm_hart_start()
```

打断点。

你会看到：

```text
CPU0
```

逐个请求：

```text
hart1
hart2
hart3
```

启动。

这一个实验就能把：

```text
SMP
HSM
IPI
warmboot
M-mode
S-mode
```

全部串起来。

---

# 44. 第四个实验：TLB shootdown

这是 Staff engineer 非常值得做的实验。

在 Linux：

```text
CPU0 modify PTE
```

触发：

```text
flush_tlb_*
```

然后观察：

```text
RFENCE / IPI
```

最终 remote CPU：

```text
SFENCE.VMA
```

路径：

```text
page table modification
       ↓
Linux TLB shootdown
       ↓
SBI RFENCE
       ↓
OpenSBI
       ↓
remote hart
       ↓
SFENCE.VMA
```

这会把你前面的：

```text
Sv39
Fence
SMP
SBI
```

四章彻底串起来。

---

# 45. 最终需要形成的一张架构图

学完 SBI/OpenSBI 后，你脑子里应该是下面这张图：

```text
                       Userspace
                           |
                        syscall
                           |
                           v
                   +---------------+
                   | Linux S-mode  |
                   +---------------+
                     |    |     |
                  timer  IPI   HSM
                     \    |    /
                      \   |   /
                       SBI ECALL
                           |
                           v
                +---------------------+
                |     OpenSBI         |
                |      M-mode         |
                +---------------------+
                  |      |       |
                 PMP    timer    IPI
                  |      |       |
                  +------+-------+
                         |
                         v
                      Hardware
```

而启动流程：

```text
RESET
  |
  v
BootROM / FSBL
  |
  v
OpenSBI M-mode
  |
  +-- machine init
  +-- PMP
  +-- trap
  +-- delegation
  +-- timer
  +-- IPI
  +-- HSM
  |
  | MRET
  v
Linux S-mode
```

运行过程中：

```text
Linux
  |
  | ECALL
  v
OpenSBI
  |
  | MRET
  v
Linux
```

---

# 46. 与 Arm 的最终统一模型

你可以把两个架构统一成：

```text
ARM
================================================

Userspace EL0
    |
    | SVC
    v
Linux EL1
    |
    | SMC
    v
TF-A EL3
    |
    +-- PSCI
    +-- platform service
    |
    v
Hardware


RISC-V
================================================

Userspace U
    |
    | ECALL
    v
Linux S
    |
    | SBI ECALL
    v
OpenSBI M
    |
    +-- HSM
    +-- TIME
    +-- IPI
    +-- RFENCE
    +-- SRST
    |
    v
Hardware
```

最核心的区别则是：

```text
Arm:
EL3 firmware 体系和 TrustZone architecture 高度结合

RISC-V:
M-mode 是 generic highest privilege
SBI 是 generic Supervisor interface
```

所以：

```text
OpenSBI ≈ TF-A BL31
```

在 system firmware 角度非常好用；

但：

```text
M-mode ≠ Secure World
```

这一点必须始终记住。

---

# 47. 最后把整章压缩成 8 句话

如果面试时让我用非常短的方式解释 SBI/OpenSBI，我会这样回答：

1. **SBI 是 RISC-V Supervisor software 与更高 privilege execution environment 之间的标准 binary interface。**

2. **Linux 通常运行在 S-mode，而 OpenSBI 通常运行在 M-mode。**

3. **Linux 通过 ECALL 请求 OpenSBI 执行 S-mode 无法直接完成的 machine/platform operation。**

4. **现代 SBI 使用 `a7=EID`、`a6=FID`、`a0-a5=args`，返回 `a0=error`、`a1=value`。**

5. **核心 SBI extension 包括 TIME、IPI、RFENCE、HSM、SRST 等。**

6. **HSM 对 secondary hart 启停的作用与 Arm PSCI CPU_ON/OFF 很相似，但 SBI 整体范围远大于 PSCI。**

7. **OpenSBI 不只是 bootloader，而是常驻 M-mode 的 runtime firmware；Linux 运行期间仍会不断通过 SBI 进入它。**

8. **从 Arm 角度可以把 `Linux S → ECALL → OpenSBI M → MRET` 粗略对应到 `Linux EL1 → SMC → TF-A EL3 → ERET`。**

---

# 48. 这一阶段真正的掌握标准

不要用：

```text
“我知道 SBI 有哪些 API”
```

判断自己是否掌握。

真正达到系统软件工程师级别，应该能够独立解释：

```text
Linux sbi_set_timer()
        |
        v
ECALL
        |
        v
M-mode trap
        |
        v
OpenSBI sbi_trap_handler()
        |
        v
sbi_ecall_handler()
        |
        v
timer implementation
        |
        v
MRET
```

以及：

```text
Linux CPU0
   |
   | SBI HSM hart_start
   v
OpenSBI
   |
   | wake CPU1
   v
CPU1 M-mode warmboot
   |
   | machine initialization
   | MRET
   v
Linux secondary entry
S-mode
```

**只要这两条路径你能从 CPU privilege、CSR、trap、ABI、firmware、SMP 六个层面全部讲清楚，SBI/OpenSBI 这一阶段基本就已经真正掌握了。**

我建议下一步不要立刻进入第 09 阶段 SMP，而是先针对这一章做一次**代码级深入**：从 Linux 的 `sbi_ecall()` 开始，一直跟到 OpenSBI 的 `sbi_trap_handler() → sbi_ecall_handler() → extension handler → mret`。这条链路一旦看透，后面的 RISC-V SMP 会明显容易很多。
