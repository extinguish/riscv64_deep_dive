# RISC-V 64 位系统开发：SBI 调用与 OpenSBI 交接

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `ADD` | **Add** | 真实指令 |
| `BEQZ` | **Branch if Equal to Zero** | 伪指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `BNEZ` | **Branch if Not Equal to Zero** | 伪指令 |
| `CSRR` | **Read Control and Status Register** | 伪指令 |
| `CSRS` | **Set Bits in Control and Status Register** | 伪指令 |
| `CSRSI` | **Set CSR Bits Immediate** | 伪指令 |
| `CSRW` | **Write Control and Status Register** | 伪指令 |
| `ECALL` | **Environment Call** | 真实指令 |
| `J` | **Jump** | 伪指令 |
| `LA` | **Load Address** | 伪指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `LW` | **Load Word** | 真实指令 |
| `MRET` | **Machine Return** | 真实指令 |
| `ORI` | **OR Immediate** | 真实指令 |
| `RDTIME` | **Read Time** | 伪指令 |
| `SFENCE.VMA` | **Supervisor Memory-Management Fence** | 真实指令 |
| `SLLI` | **Shift Left Logical Immediate** | 真实指令 |
| `SRET` | **Supervisor Return** | 真实指令 |
| `SW` | **Store Word** | 真实指令 |
| `WFI` | **Wait For Interrupt** | 真实指令 |

ABI 别名是同一套 `x0～x31` 整数寄存器的另一种写法，并非额外寄存器；下面给出本文提及的整数寄存器编号与对应 ABI 别名。`PC` 若在本文出现，指 **Program Counter**（程序计数器），它不属于 `x0～x31`。

| ABI 别名 | 硬件编号 | 英文完整名称 |
|---|---|---|
| `zero` | `x0` | **Zero** |
| `t0` | `x5` | **Temporary register 0** |
| `t1` | `x6` | **Temporary register 1** |
| `a0` | `x10` | **Argument register 0** |
| `a1` | `x11` | **Argument register 1** |
| `a5` | `x15` | **Argument register 5** |
| `a6` | `x16` | **Argument register 6** |
| `a7` | `x17` | **Argument register 7** |

`CSR` = **Control and Status Register**（控制与状态寄存器）。本文出现的 CSR 名称如下；这些是独立的 CSR 地址，不是 `x` 寄存器的别名。名称以 `m/s/h/vs` 开头通常分别对应 Machine / Supervisor / Hypervisor / Virtual Supervisor，实际可访问性还取决于特权级和扩展。

| CSR 名称 | 英文完整名称 |
|---|---|
| `mcounteren` | **Machine Counter Enable** |
| `medeleg` | **Machine Exception Delegation** |
| `mepc` | **Machine Exception Program Counter** |
| `mstatus` | **Machine Status** |
| `mtvec` | **Machine Trap Vector** |
| `scause` | **Supervisor Trap Cause** |
| `sie` | **Supervisor Interrupt Enable** |
| `sstatus` | **Supervisor Status** |
| `stvec` | **Supervisor Trap Vector** |
| `time` | **Time Counter** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

前一篇的 S→M `ECALL` 只演示了 trap 机制。真正的 S-mode 内核应按 **Supervisor Binary Interface（SBI）**约定与 OpenSBI 等运行时固件通信。本篇从启动交接、寄存器 ABI、扩展探测讲到计时器中断和 TEE 边界，最后在 QEMU 自带的 OpenSBI 上运行 S-mode 程序。

## 1. 分清三层协议

| 层 | 发起方 → 接收方 | `ecall` 参数的解释者 | 典型用途 |
|---|---|---|---|
| Linux 系统调用 | U → S | Linux 内核 | 文件、进程、内存映射 |
| SBI | S → SEE（通常为 M-mode 固件；虚拟机内可由更高层提供） | SBI 实现 | 定时器、IPI、远程 fence、hart 生命周期、复位 |
| 固件内部调用 | M 内部 | 固件自行定义 | 平台设备、安全域、异常分发 |

`ECALL` 指令只引发同步异常；调用号、参数、返回值属于软件 ABI。OpenSBI 是 SBI 的一个实现，**SBI 不是 OpenSBI 专有 API**。也不能把 SBI v0.1 的旧调用方式套到 v0.2 及之后的扩展模型上。[SBI 二进制编码](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/binary-encoding.adoc)

## 2. 从 M-mode 到 S-mode 的启动交接

QEMU `virt` 使用 `-bios default -kernel payload.elf` 时，固件先在 M-mode 初始化，再把控制权交给 S-mode payload。这里的 `-bios default` 使用当前 QEMU 安装提供的固件；版本和启动日志随环境变化。典型交接要完成：

1. 确定每个 hart 的启动角色，初始化每 hart scratch、trap 入口和栈。
2. 初始化平台中断、定时器与 PMP/domain 权限；保护固件自身内存。
3. 设置 `mepc` 与 `mstatus.MPP=S`，用 `mret` 进入下一阶段。
4. 传递 hart ID 和设备树地址；常见 OpenSBI/QEMU 启动约定是 `a0=hartid`、`a1=FDT`，具体入口契约仍由固件装载方式决定。

独立开发 OpenSBI 时，要先核对固件装载模型（例如 FW_DYNAMIC 的上一级引导参数、FW_PAYLOAD 的内嵌下一阶段、FW_JUMP 的固定入口），再核对每 hart 的下一阶段地址与模式。不能只看链接地址就推断 `mret` 会跳到那里。[OpenSBI 平台要求](https://github.com/riscv-software-src/opensbi/blob/master/docs/platform_requirements.md)还区分 ISA 最小集合、S-mode、定时器等平台依赖；PMP 可能是可选硬件，但安全隔离设计仍须论证保护机制。

启动交接也决定故障的第一位观察者：S-mode 页故障可能进入 S handler，S-mode `ECALL` 通常进入 M firmware，PMP 访问故障是否委托则取决于 `medeleg`。若早期固件尚未安装可信 `mtvec`，一次访问故障就可能演变成难以定位的重复陷入。

## 3. SBI v0.2+ 的寄存器 ABI

| 寄存器 | 进入 `ecall` 时 | 返回时 |
|---|---|---|
| `a7` | 扩展号 EID | 保持原值 |
| `a6` | 功能号 FID | 保持原值 |
| `a0`～`a5` | 最多 6 个参数 | `a0=error`、`a1=value`；其余保持 |

除 `a0/a1` 外，SBI 约定其他整数寄存器由**被调用的 SEE**保持。`error == 0` 才表示成功；`value` 的意义由具体 FID 定义。若 `error != 0`，除非该 FID 明确规定，否则 `a1` 的值**未指定，不能当作有效结果使用**。`error=-2` 表示不支持的 EID/FID。若在 C 中手写内联汇编，必须正确声明 `a0/a1` 为输入输出并按内存副作用加 `memory` clobber；`ecall` 本身不能代替共享内存排序协议。中断上下文、锁顺序和异步回调还要单独设计。[SBI 二进制编码](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/binary-encoding.adoc)

初始化时先调用 Base 扩展 `EID=0x10` 的 `get_spec_version(FID=0)`；再用 `probe_extension(FID=3, a0=目标 EID)` 检查需要的扩展。探测返回 `error=0` 且 `value!=0` 才表示扩展存在；不能固定与 `1` 比较。[Base 扩展](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-base.adoc)

`get_spec_version` 成功返回的 `a1` 不是普通十进制版本号：`major=(a1 >> 24) & 0x7f`，`minor=a1 & 0xffffff`，bit 31 为 0。例如 `0x00000002` 表示 v0.2，`0x01000000` 表示 v1.0。不能用 `a1 >= 0x10` 这类没有解码的比较判断功能；真正可选的服务仍以 `probe_extension` 和每次调用返回值为准。本篇 Demo 只确认版本调用成功并且版本值非零，不把某个具体版本号写死。

| 扩展 | EID | 系统开发中的作用 | 常见误用 |
|---|---:|---|---|
| BASE | `0x10` | 版本、实现信息、扩展探测 | 只读 banner，不探测运行时能力 |
| TIME | `0x54494d45` | `set_timer(FID=0)` 设置绝对 `time` 值 | 把参数当“多少微秒后”；漏处理 STIP |
| IPI | `0x735049` | 跨 hart 中断通知 | 以为 IPI 自动完成数据发布或 TLB 同步 |
| RFENCE | `0x52464e43` | 请求目标 hart 执行远程 fence | 本 hart `SFENCE.VMA` 后认为所有 hart 都已同步 |
| HSM | `0x48534d` | hart 启动、停止、挂起 | 未检查目标 hart 状态与启动地址权限 |
| SRST | `0x53525354` | 系统复位/关机 | 把返回当作成功；成功请求通常不返回 |

TIME、IPI、RFENCE、HSM、SRST 的参数应分别按[各扩展规范](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/riscv-sbi.adoc)核对。扩展存在不代表所有功能、硬件路径或目标 hart 的调用都必定成功；每次仍须处理返回错误。RFENCE 的目标 hart 掩码、ASID 和地址范围与 Sv39 页表修改直接相关。

## 4. 计时器与 trap 的完整因果链

S-mode 不能假设总能直接写 `mtimecmp`。通过 TIME `set_timer` 向固件提交**绝对**计数值；固件/平台负责让 S-mode 看见 supervisor timer interrupt。S-mode 还必须安装 `stvec`，打开 `sie.STIE` 和 `sstatus.SIE`。中断到来时 `scause` 最高位为 1、cause 编号为 5。处理完一次性定时器后，应设置下一次截止值或用 `-1` 取消，否则可能立即再次陷入。[TIME 规范](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-time.adoc)

`rdtime` 属于 `Zicntr` 计数器扩展；读到的 `time` 单位与频率由平台描述，S-mode 能否读取还取决于 `mcounteren.TM` 等平台配置。下面用 `now + 1,000,000`，只表示一百万计数周期，不能直接称作一秒。`WFI` 可以提前返回，实现还可把它当 NOP；等待循环每次都重新检查状态。本例的 trap handler 会修改 `t0/t1` 和 `a0/a1/a6/a7`，而主线在等待循环中不依赖这些旧值，所以实验能成立。**一般异步 handler 不能这样做**：它必须保存被打断的通用寄存器并建立可信栈，调用 SBI 返回后再恢复现场。

## 5. Demo：在真正的 OpenSBI 上发起 SBI 调用

本例用 `-bios default` 启动 QEMU 的 OpenSBI，S payload 位于 `0x80200000`。它先探测 TIME/SRST，再通过 TIME 产生一次 S-mode 定时器中断；handler 核对 `scause`、取消定时器，成功后用 SRST shutdown（type=0、reason=0）。失败则向 QEMU `virt` 的 finisher 写入失败码，这样退出状态能区分两条路径。本例验证 **SBI 调用链 + 中断链**，并不自行实现 M-mode firmware。

保存为 `sbi_demo.S`：

```asm
    .option norelax
    .section .text.init
    .balign 4
    .globl _start
_start:
    la t0, s_trap
    csrw stvec, t0

    li a7, 0x10
    li a6, 0
    ecall
    bnez a0, fail
    beqz a1, fail

    li a0, 0x54494d45
    li a7, 0x10
    li a6, 3
    ecall
    bnez a0, fail
    beqz a1, fail

    li a0, 0x53525354
    li a7, 0x10
    li a6, 3
    ecall
    bnez a0, fail
    beqz a1, fail

    la t0, seen
    sw zero, 0(t0)
    rdtime t0
    li t1, 1000000
    add a0, t0, t1
    li a7, 0x54494d45
    li a6, 0
    ecall
    bnez a0, fail

    li t0, 0x20
    csrs sie, t0
    csrsi sstatus, 2
wait_timer:
    la t0, seen
    lw t1, 0(t0)
    bnez t1, pass
    wfi
    j wait_timer

s_trap:
    csrr t0, scause
    li t1, 1
    slli t1, t1, 63
    ori t1, t1, 5
    bne t0, t1, fail
    li a0, -1
    li a7, 0x54494d45
    li a6, 0
    ecall
    bnez a0, fail
    la t0, seen
    li t1, 1
    sw t1, 0(t0)
    sret

pass:
    li a0, 0
    li a1, 0
    li a7, 0x53525354
    li a6, 0
    ecall
fail:
    li t0, 0x100000               # QEMU virt finisher；SBI 失败才使用
    li t1, 0x13333
    sw t1, 0(t0)
1:  j 1b

    .section .bss
    .balign 4
seen: .word 0
```

保存为 `sbi_demo.ld`：

```ld
OUTPUT_ARCH(riscv)
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);          /* R + X */
    data PT_LOAD FLAGS(6);          /* R + W */
}
SECTIONS {
    . = 0x80200000;
    .text : { *(.text.init) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    . = ALIGN(4096);
    .data : { *(.data .data.*) } :data
    .bss : { *(.bss .bss.*) *(COMMON) } :data
}
```

构建并运行：

```bash
riscv64-unknown-elf-gcc -march=rv64i_zicsr_zicntr -mabi=lp64 \
    -nostdlib -nostartfiles -Wl,--no-relax -T sbi_demo.ld \
    -o sbi_demo.elf sbi_demo.S
timeout 15s qemu-system-riscv64 -machine virt -nographic -smp 1 \
    -m 128M -bios default -kernel sbi_demo.elf
echo $?
```

在当前 QEMU/OpenSBI 环境中，固件打印 SBI 扩展与 PMP/domain 信息，随后进程以 `0` 退出。退出码 `1` 表示示例自己的检查失败；`124` 是 `timeout` 超时，应检查 payload 地址、`stvec`、`sie/sstatus` 与 TIME 探测。QEMU finisher 的地址和退出码、SRST 与宿主退出码的映射都是当前机器/固件组合的测试反馈，不是 SBI 的通用规定。

## 6. OpenSBI 与 TEE 的安全边界

OpenSBI 的 domain 用于描述各 hart 所属执行域和内存区域权限；同一物理地址能否由 S-mode 访问，最终还要由具体硬件 PMP/内存保护实现支撑。[OpenSBI domain 文档](https://github.com/riscv-software-src/opensbi/blob/master/docs/domain_support.md)

`ECALL` 进入 M-mode 后不能把 S-mode 传入的指针直接当作可信 M-mode 指针：先验证范围、权限、长度溢出、内存生命周期和复制时机。对跨域共享缓冲区，还要说明所有权转移与并发修改规则，否则检查后使用前的内容可能已变化。多 hart 固件要分别维护 trap 栈、scratch 和域状态；服务函数的可重入性、锁顺序和中断开关状态不能由 SBI 寄存器 ABI 自动保证。

TEE monitor 尤其要区分“普通 S 请求固件服务”与“安全域切换”：前者成功不意味着后者已经完成寄存器清理、PMP 重配、缓存/设备隔离。下一篇会用 PMP Demo 验证一个受保护物理页的访问失败。

本篇与[特权级陷入](14-rv64-privilege-trap-and-return.md)、[PMP 与 TEE](16-rv64-pmp-and-tee-isolation.md)、[Sv39 与 SFENCE.VMA](17-rv64-sv39-and-sfence-vma.md)应连起来读。更完整的固件与 domain 练习见 [Lab 15](/home/guoshichao/riscv_asm_lab/labs/lab15-mmode-sbi-domain/README.md)。
