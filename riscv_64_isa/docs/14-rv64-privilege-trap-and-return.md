# RISC-V 64 位系统开发：特权级、陷入与返回

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `ADDI` | **Add Immediate** | 真实指令 |
| `AND` | **AND** | 真实指令 |
| `ANDI` | **AND Immediate** | 真实指令 |
| `BEQ` | **Branch if Equal** | 真实指令 |
| `BEQZ` | **Branch if Equal to Zero** | 伪指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `BNEZ` | **Branch if Not Equal to Zero** | 伪指令 |
| `CSRR` | **Read Control and Status Register** | 伪指令 |
| `CSRW` | **Write Control and Status Register** | 伪指令 |
| `EBREAK` | **Environment Breakpoint** | 真实指令 |
| `ECALL` | **Environment Call** | 真实指令 |
| `J` | **Jump** | 伪指令 |
| `JALR` | **Jump And Link Register** | 真实指令 |
| `LA` | **Load Address** | 伪指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `MRET` | **Machine Return** | 真实指令 |
| `OR` | **OR** | 真实指令 |
| `RET` | **Return** | 伪指令 |
| `SFENCE.VMA` | **Supervisor Memory-Management Fence** | 真实指令 |
| `SRET` | **Supervisor Return** | 真实指令 |
| `SRLI` | **Shift Right Logical Immediate** | 真实指令 |
| `SW` | **Store Word** | 真实指令 |
| `WFI` | **Wait For Interrupt** | 真实指令 |

ABI 别名是同一套 `x0～x31` 整数寄存器的另一种写法，并非额外寄存器；下面给出本文提及的整数寄存器编号与对应 ABI 别名。`PC` 若在本文出现，指 **Program Counter**（程序计数器），它不属于 `x0～x31`。

| ABI 别名 | 硬件编号 | 英文完整名称 |
|---|---|---|
| `zero` | `x0` | **Zero** |
| `ra` | `x1` | **Return Address** |
| `sp` | `x2` | **Stack Pointer** |
| `gp` | `x3` | **Global Pointer** |
| `tp` | `x4` | **Thread Pointer** |
| `t0` | `x5` | **Temporary register 0** |
| `t1` | `x6` | **Temporary register 1** |
| `a0` | `x10` | **Argument register 0** |
| `a1` | `x11` | **Argument register 1** |
| `a6` | `x16` | **Argument register 6** |
| `a7` | `x17` | **Argument register 7** |

`CSR` = **Control and Status Register**（控制与状态寄存器）。本文出现的 CSR 名称如下；这些是独立的 CSR 地址，不是 `x` 寄存器的别名。名称以 `m/s/h/vs` 开头通常分别对应 Machine / Supervisor / Hypervisor / Virtual Supervisor，实际可访问性还取决于特权级和扩展。

| CSR 名称 | 英文完整名称 |
|---|---|
| `mcause` | **Machine Trap Cause** |
| `medeleg` | **Machine Exception Delegation** |
| `mepc` | **Machine Exception Program Counter** |
| `mideleg` | **Machine Interrupt Delegation** |
| `mie` | **Machine Interrupt Enable** |
| `mip` | **Machine Interrupt Pending** |
| `misa` | **Machine ISA Register** |
| `mscratch` | **Machine Scratch** |
| `mstatus` | **Machine Status** |
| `mtval` | **Machine Trap Value** |
| `mtvec` | **Machine Trap Vector** |
| `pmpaddr0` | **Physical Memory Protection Address register 0** |
| `pmpcfg0` | **Physical Memory Protection Configuration register 0** |
| `satp` | **Supervisor Address Translation and Protection** |
| `scause` | **Supervisor Trap Cause** |
| `sepc` | **Supervisor Exception Program Counter** |
| `sie` | **Supervisor Interrupt Enable** |
| `sip` | **Supervisor Interrupt Pending** |
| `sscratch` | **Supervisor Scratch** |
| `sstatus` | **Supervisor Status** |
| `stval` | **Supervisor Trap Value** |
| `stvec` | **Supervisor Trap Vector** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

前面的 [CSR 指令](09-rv64-csr-instructions.md)只在 Linux 用户态操作了可访问的 CSR。OpenSBI、内核和 TEE monitor 则要在 M/S/U 模式中安排陷入入口、委托、返回和权限。**CSR 指令能汇编**不等于当前特权级能访问该 CSR；本篇 Demo 必须运行在完整系统模拟的裸机环境，不能交给 `qemu-riscv64-static`。

本篇聚焦基础 M/S/U 模式，不引入 H 扩展的 HS/VS、AIA 中断控制器或 Sv39 页表。Demo 用 `-bios none` 的 QEMU `virt`：从 M-mode 启动，完成 S→M `ECALL` 与 U→S 委托 `ECALL`，并检查 EPC、cause 和返回值。它是一个**状态转换最小模型**；生产级 trap entry 还要保存现场、使用独立栈、处理嵌套与并发。

## 1. 谁可以执行什么

| 模式 | 常见软件 | 与本篇有关的能力 |
|---|---|---|
| M-mode | OpenSBI、低层固件、安全监控器 | 管理机器级 trap、PMP、向下交接到 S/U |
| S-mode | 内核、管理程序的一部分 | 管理 S 级 trap、虚拟内存，通常通过 SBI 请求 M 级服务 |
| U-mode | 应用或 enclave 用户负载 | 不能直接写 M/S 特权 CSR，通常通过 `ECALL` 请求更高层服务 |

特权级表示执行权限，**不是**汇编目标的 `-march=rv64im...` 中的 `M` 扩展。后者表示整数乘除法。本篇最小 Demo 使用 `-march=rv64i_zicsr`，仍能在 M-mode 执行；OpenSBI 产品平台另有自己的最小 ISA 要求，应按[平台要求](https://github.com/riscv-software-src/opensbi/blob/master/docs/platform_requirements.md)核对。目标平台是否有 S-mode、PMP、某个可选 CSR，也都要单独确认。

## 2. 两组必须成对理解的 trap CSR

| 作用 | M 级 | S 级 | 易错点 |
|---|---|---|---|
| 总体状态 | `mstatus` | `sstatus`（相关字段视图） | 先前特权级、先前中断使能、当前中断使能不是同一位 |
| 入口地址 | `mtvec` | `stvec` | Direct/Vectored 的行为不同；入口对齐要求与模式有关 |
| 被打断的 PC | `mepc` | `sepc` | 同步异常一般指向触发指令；是否推进由 handler 决定 |
| 原因 | `mcause` | `scause` | RV64 最高位区分中断与同步异常，其余位是 cause 编号 |
| 补充信息 | `mtval` | `stval` | 含义随 cause 与实现变化，不能一律当故障地址 |
| 软件 scratch | `mscratch` | `sscratch` | 硬件不替你换栈；常用它保存 trap 入口所需指针 |
| 中断使能与待处理 | `mie/mip` | `sie/sip` | pending 与 enable 都满足且全局开关允许才会被接收 |

`mstatus` 中 `MPP/MPIE/MIE` 是 M 级返回状态栈；`sstatus` 中 `SPP/SPIE/SIE` 对应 S 级。发生 trap 时，硬件记录来源权限和中断使能、清除当前级中断使能，并写 EPC/cause；执行 `MRET/SRET` 时按这些字段恢复权限和中断状态并从 EPC 继续。**硬件不会保存所有通用寄存器，也不会替软件推进 EPC。**

`ECALL` 按来源特权级区分 cause：U-mode 为 `8`，S-mode 为 `9`，M-mode 为 `11`；`EBREAK` 为 `3`，非法指令为 `2`。本篇 Demo 只用 32 位 `ECALL`，处理后把 EPC 加 `4`。不能把这条规则机械套到 `C.EBREAK`、非法压缩指令或需要重试的页故障。若页故障通过补页解决，通常要从原指令重试，不能盲目推进 EPC。

## 3. 委托改变入口，不改变权限层级

`medeleg` 决定哪些**来自低于 M-mode** 的同步异常交给 S-mode；`mideleg` 对中断起类似作用。写入某位后应读回确认：可写位集合可能因实现和平台而异。即使某个 cause 被委托，M-mode 自己触发同类异常也仍由 M-mode 处理；trap 不会从更高权限自动落到更低权限。

一个容易在 OpenSBI 开发中弄错的路径：

```text
S-mode ecall -> M-mode SBI handler -> mret -> S-mode
U-mode ecall -> 通常由 S-mode 处理（如已把 U ecall 委托给 S）
```

同样一条 `ecall`，**SBI 扩展号/功能号**与**Linux 系统调用号**由不同的软件接口解释。Linux 用户态章节使用 `a7=93` 请求 `exit`；SBI v0.2 及之后使用 `a7` 放扩展号、`a6` 放功能号，返回 `a0=error`、`a1=value`。这是执行环境约定，不是 `ECALL` 指令自身编码出的语义。[SBI 调用约定](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/binary-encoding.adoc)

## 4. `mtvec/stvec` 与入口的第一批指令

Direct 模式把所有 trap 导向 BASE。Vectored 模式下，同步异常仍到 BASE，中断按 cause 偏移进入向量入口；不能以为所有异常都会进入 `BASE+4×cause`。BASE 必须满足所选模式及实现的对齐约束。最初做系统 bring-up 时，Direct 模式更便于使用单入口记录 `cause/EPC/tval`。

trap entry 在切换到可信栈之前，不能随意使用被打断上下文的 `sp`，也不能假定 `gp/tp` 已是固件自己的值。常见做法是在当前模式的 scratch CSR 中保存每 hart 的上下文或栈指针，用一次 CSR 交换尽早建立安全入口，再保存 GPR。若要调用 C dispatcher，必须先恢复该 C 代码所需的 `gp`，让 `sp` 满足 ABI 对齐，并明确 C 可覆盖哪些寄存器。可在 [Lab 03](/home/guoshichao/riscv_asm_lab/labs/lab03-trap-syscall-exception/README.md)练习完整现场保存；本篇 Demo 有意不承担这部分实现。

## 5. `MRET/SRET` 是状态转换，不是普通 `ret`

普通 `ret` 通常是 `jalr x0,0(ra)`：它只按普通寄存器给出的地址跳转，不恢复特权级或中断状态。`mret` 使用 `mepc` 和 `mstatus` 中保存的状态；`sret` 使用 `sepc` 和 `sstatus`。在 M→S 首次交接前，应准备 `mepc` 和 `mstatus.MPP=S`；在 S→U 交接前，应准备 `sepc` 和 `sstatus.SPP=U`。返回地址、栈、页表和 PMP 权限仍要由软件分别安排。

M-mode 还能通过 `mstatus.TSR` 截获 S-mode 的 `SRET`，通过 `TW` 限制低权限的 `WFI`，通过 `TVM` 截获 S-mode 的 `satp` 访问与 `SFENCE.VMA`。因此某条特权指令在汇编阶段合法、在另一块板子上却产生非法指令异常时，要同时查当前模式、扩展能力和上一级设置的拦截位。Demo 假定这些拦截位处于 QEMU `-bios none` 的复位默认状态；产品固件不应依赖这个假设。[机器级状态控制](https://docs.riscv.org/reference/isa/priv/machine.html)

## 6. 完整 Demo：M→S、S→M、S→U、U→S

这段代码在 QEMU `virt` 的一个 hart 上运行，使用 `satp=Bare`，并把 PMP 临时配置成**允许低特权级访问所有物理内存**，只为隔离 trap 状态转换的问题。**这不是 TEE 安全配置**：真实固件必须收紧 PMP，保护固件和 enclave 区域。

执行流程：

```text
M 启动 -> mret 到 S
S 发 ecall -> M 检查 mcause/mstatus/mepc，设置 a0，mret 回 S
S 准备 sepc/SPP -> sret 到 U
U 发 ecall -> S 检查 scause/sstatus/sepc，设置 a0，sret 回 U
U 再发 ecall -> S -> M 的结束服务 -> QEMU finisher
```

保存为 `trap_demo.S`：

```asm
    .option norelax
    .section .text.init
    .balign 4
    .globl _start
_start:
    la t0, m_trap
    csrw mtvec, t0                 # Direct 模式，地址 bit[1:0] 为零
    la t0, s_trap
    csrw stvec, t0

    li t0, 1 << 8
    csrw medeleg, t0               # 委托 U-mode ECALL（cause 8）到 S
    csrr t1, medeleg
    andi t1, t1, 0x100
    beqz t1, failed                # 实现若不支持此委托，明确失败

    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x0f                    # TOR + R/W/X：演示用全地址放行
    csrw pmpcfg0, t0

    la t0, s_entry
    csrw mepc, t0
    csrr t0, mstatus
    li t1, ~(3 << 11)
    and t0, t0, t1                 # 清 MPP
    li t1, 1 << 11
    or t0, t0, t1                  # MPP = S
    csrw mstatus, t0
    mret

    .section .text
    .balign 4
s_entry:
    li a7, 0
s_to_m:
    ecall                         # S-mode ECALL：应进入 M
    li t0, 0x5a
    bne a0, t0, failed            # M handler 写入的返回值

    la t0, u_entry
    csrw sepc, t0
    csrr t0, sstatus
    li t1, ~(1 << 8)
    and t0, t0, t1                 # SPP = U
    csrw sstatus, t0
    sret

u_entry:
    li a7, 0
u_to_s:
    ecall                         # U-mode ECALL：委托给 S
    li t0, 0x22
    bne a0, t0, failed
    li a7, 1
    ecall                         # 请求 S handler 结束实验
    j failed

    .balign 4
m_trap:
    csrr t0, mcause
    li t1, 9                       # S-mode ECALL
    bne t0, t1, failed
    csrr t0, mstatus
    srli t0, t0, 11
    andi t0, t0, 3                 # MPP 应为 S
    li t1, 1
    bne t0, t1, failed
    beqz a7, m_first_call
    li t0, 1
    beq a7, t0, passed
    j failed
m_first_call:
    csrr t0, mepc
    la t1, s_to_m
    bne t0, t1, failed             # EPC 指向 ECALL 本身
    addi t0, t0, 4
    csrw mepc, t0                  # 不推进就会再次执行同一 ECALL
    li a0, 0x5a
    mret

    .balign 4
s_trap:
    csrr t0, scause
    li t1, 8                       # U-mode ECALL
    bne t0, t1, failed
    csrr t0, sstatus
    andi t0, t0, 0x100             # SPP 应为 U
    bnez t0, failed
    beqz a7, s_first_call
    li t0, 1
    beq a7, t0, s_finish
    j failed
s_first_call:
    csrr t0, sepc
    la t1, u_to_s
    bne t0, t1, failed
    addi t0, t0, 4
    csrw sepc, t0
    li a0, 0x22
    sret
s_finish:
    li a7, 1
    ecall                         # 从 S-mode 进入 M-mode，结束实验
    j failed

passed:
    li t0, 0x100000               # QEMU virt 的 sifive_test finisher
    li t1, 0x5555                 # pass
    sw t1, 0(t0)
1:  j 1b
failed:
    li t0, 0x100000
    li t1, 0x13333                # fail
    sw t1, 0(t0)
2:  j 2b
```

保存为 `trap.ld`：

```ld
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x80000000;
    .text : { *(.text.init) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
}
```

构建并运行：

```bash
riscv64-unknown-elf-gcc -march=rv64i_zicsr -mabi=lp64 \
    -nostdlib -nostartfiles -Wl,--no-relax -T trap.ld \
    -o trap_demo.elf trap_demo.S
timeout 10s qemu-system-riscv64 -machine virt -bios none \
    -nographic -smp 1 -m 128M -kernel trap_demo.elf
echo $?
```

预期 QEMU 通过 finisher 结束，退出状态为 `0`；状态 `1` 表示检查失败，`124` 表示超时，通常意味着返回 PC 或权限状态错误。这个退出机制是 **QEMU `virt` 平台设备约定**，不是 RISC-V ISA 指令。若运行平台不是 QEMU `virt`，必须替换退出设备地址与方式。

### 6.1 Demo 能证明什么、不能证明什么

它实际检查了 S-mode `ECALL` 的 M 级 cause、来源权限、`mepc`，以及 U-mode `ECALL` 委托后的 S 级 cause、来源权限、`sepc`；还检查两次返回后 `a0` 的值。但 handler 直接使用 `t0/t1`，没有保存被打断上下文，也没有建立可信 trap 栈，因此**只适合作为受控状态机示例**。真实中断可能发生在任何时刻；一旦调用 C、处理嵌套中断或切换安全域，必须使用完整 trap frame 和独立栈。

本 Demo 不启用页表；它不能验证 Sv39 page fault、`satp` 或 `SFENCE.VMA`。PMP 的全放行配置也不能验证 enclave 隔离。后续分别见 [SBI/OpenSBI](15-rv64-sbi-and-opensbi.md)、[PMP/TEE](16-rv64-pmp-and-tee-isolation.md)与[Sv39](17-rv64-sv39-and-sfence-vma.md)；对应完整练习见 [Lab 03](/home/guoshichao/riscv_asm_lab/labs/lab03-trap-syscall-exception/README.md)、[Lab 06](/home/guoshichao/riscv_asm_lab/labs/lab06-sv39-page-tables-tlb/README.md) 与 [Lab 07](/home/guoshichao/riscv_asm_lab/labs/lab07-pmp-isolation-access-fault/README.md)。

## 7. CSR 字段不是普通可任意写的变量

系统 CSR 中常见字段类别包括 WARL（可写，但读回一定是某个合法值）、WLRL、WPRI（保留供未来使用）。不能写入一个想要的位模式，就断言硬件一定原样保存。应按规范使用**读—改—写—读回**，只修改目标字段，并给不支持的合法值准备退路。`misa`、`mtvec.MODE`、PMP 粒度、委托位等都可能受实现与平台限制。

另外，访问控制不只由 CSR 地址高位决定：某 CSR 可能根本未实现；某些扩展定义的功能可能被环境寄存器禁止；虚拟化模式还可能把访问变成 virtual-instruction 异常。构建时的 `-march` 只帮助汇编器检查目标 ISA，并不替运行时代码探测硬件能力。

## 8. 系统开发常见故障定位顺序

| 现象 | 第一批检查项 |
|---|---|
| `ecall` 后反复陷入 | EPC 是否仍指向同一 `ecall`；应推进多少字节 |
| trap 到了错误级别 | `medeleg/mideleg` 读回值、来源模式、cause |
| `mret/sret` 后非法指令或访问故障 | `MPP/SPP`、目标 EPC、PMP、页表和目标指令对齐 |
| 调用 C 后返回错误 | trap 栈、`gp`、16 字节对齐、GPR 保存与恢复 |
| 只在中断时损坏变量 | handler 是否按异步入口保存了被打断的 caller-saved 寄存器 |
| `mtval/stval` 与预想不同 | 先看 cause；对应异常和实现是否规定该字段有效 |

规范依据：[机器级 ISA](https://docs.riscv.org/reference/isa/priv/machine.html)、[监督级 ISA](https://docs.riscv.org/reference/isa/priv/supervisor.html)。
