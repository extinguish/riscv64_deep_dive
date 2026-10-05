# RISC-V 64 位系统开发：中断、定时器与 AIA 边界

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准；“真实指令”表示有机器编码，但可能属于可选扩展或特权架构，并不表示仅用 `-march=rv64i` 就能汇编、执行。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `ADD` | **Add** | 真实指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `BNEZ` | **Branch if Not Equal to Zero** | 伪指令 |
| `CSRC` | **Clear Bits in Control and Status Register** | 伪指令 |
| `CSRR` | **Read Control and Status Register** | 伪指令 |
| `CSRS` | **Set Bits in Control and Status Register** | 伪指令 |
| `CSRSI` | **Set CSR Bits Immediate** | 伪指令 |
| `CSRW` | **Write Control and Status Register** | 伪指令 |
| `ECALL` | **Environment Call** | 真实指令 |
| `J` | **Jump** | 伪指令 |
| `LA` | **Load Address** | 伪指令 |
| `LD` | **Load Doubleword** | 真实指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `LW` | **Load Word** | 真实指令 |
| `MRET` | **Machine Return** | 真实指令 |
| `ORI` | **OR Immediate** | 真实指令 |
| `SD` | **Store Doubleword** | 真实指令 |
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
| `t2` | `x7` | **Temporary register 2** |

`CSR` = **Control and Status Register**（控制与状态寄存器）。本文出现的 CSR 名称如下；这些是独立的 CSR 地址，不是 `x` 寄存器的别名。名称以 `m/s/h/vs` 开头通常分别对应 Machine / Supervisor / Hypervisor / Virtual Supervisor，实际可访问性还取决于特权级和扩展。

| CSR 名称 | 英文完整名称 |
|---|---|
| `mcause` | **Machine Trap Cause** |
| `menvcfg` | **Machine Environment Configuration** |
| `mepc` | **Machine Exception Program Counter** |
| `mideleg` | **Machine Interrupt Delegation** |
| `mie` | **Machine Interrupt Enable** |
| `mip` | **Machine Interrupt Pending** |
| `mstatus` | **Machine Status** |
| `mtvec` | **Machine Trap Vector** |
| `scause` | **Supervisor Trap Cause** |
| `sie` | **Supervisor Interrupt Enable** |
| `sip` | **Supervisor Interrupt Pending** |
| `sstatus` | **Supervisor Status** |
| `stimecmp` | **Supervisor Timer Compare** |
| `stvec` | **Supervisor Trap Vector** |
| `time` | **Time Counter** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

系统固件处理的异步事件不能按普通 `ECALL` 的思路编写：中断可能出现在任何一条指令之后，源设备的 pending 状态也不会因为 `mret/sret` 自动清除。本篇从 `mip/mie/mstatus` 的三道条件讲到定时器、IPI、PLIC/AIA 路由，并用 QEMU `virt` 的机器级定时器验证一次真正的异步 trap。

## 1. 先确定“谁产生、谁接收、谁清除”

| 事件 | 常见 pending 位 / cause | 可能的来源与清除位置 |
|---|---|---|
| M 软件中断 | `MSIP` / 3 | hart 的 MSIP 寄存器或平台 IPI；清除源寄存器 |
| M 定时器中断 | `MTIP` / 7 | `mtime >= mtimecmp`；把 compare 推到未来或屏蔽使能 |
| M 外部中断 | `MEIP` / 11 | 外部中断控制器；按控制器协议完成/应答 |
| S 软件中断 | `SSIP` / 1 | SBI IPI、可写 pending 位或控制器；按具体来源清除 |
| S 定时器中断 | `STIP` / 5 | SBI TIME、Sstc 或平台虚拟化路径；重新编程截止值 |
| S 外部中断 | `SEIP` / 9 | PLIC context、APLIC/IMSIC 或其他平台控制器 |

`mcause/scause` 的最高位表示中断，低位才是 cause 编号。**同一个编号不等于同一个设备寄存器**：例如 cause 5 是 S timer interrupt，cause 7 是 M timer interrupt。外部中断 cause 9/11 只是进入对应级别的入口；设备 ID、优先级和完成协议还要查外部控制器。规范见[机器级中断](https://docs.riscv.org/reference/isa/priv/machine.html)和[监督级中断](https://docs.riscv.org/reference/isa/priv/supervisor.html)。

## 2. Pending、局部 enable、全局 enable

对当前在 M-mode 执行的 MTI，触发需要 `mip.MTIP=1`、`mie.MTIE=1`、`mstatus.MIE=1`。当 hart 当前级别低于 M 时，目标为 M 的中断不受当前 `mstatus.MIE` 的同级全局门控方式限制。S 级中断有相应的 `sip/sie/sstatus.SIE` 条件。`mideleg` 决定中断是否交给 S：被委托的 pending/enable 位才通过 `sip/sie` 的视图对 S 有效；S 软件不能仅凭写 `sie` 把未委托的 M 级来源变成 S 中断。

`mip` 的各 pending 位并非都可写。**MTIP 不能靠 `csrc mip` 清除**；它由比较器条件决定。写 `mtimecmp` 后 pending 解除也可能有传播延迟，handler 若立刻返回可能再次陷入；一次性定时器在清源后还可关掉 `MTIE`。同理，PLIC 的 claim/complete、IMSIC 的取中断 ID 与设备自己的清源寄存器，各自都是独立步骤。把 `mepc` 加 4 是处理 `ecall` 的动作，异步中断应从保存的 PC 正常继续。

trap entry 必须保存被打断的寄存器，选定每 hart 可信栈，并区分同步异常与异步中断。`mtvec/stvec` Direct 模式的所有事件先进入 BASE；Vectored 模式只有中断按 `BASE+4×cause` 分派，同步异常仍到 BASE。表内并列出现不同类型 trap 时，不能从向量槽号猜设备 ID。

## 3. 定时器的三条常见路径

| 路径 | 谁写截止值 | 适用场景与限制 |
|---|---|---|
| M-mode `mtimecmp` | M 固件 | 平台有相应 MMIO 比较器；地址来自平台描述，不是 ISA 常量 |
| SBI TIME | S 发 SBI `set_timer`，由固件/SEE 处理 | S-mode 可移植入口；参数是绝对 `time` 值，见[第 15 篇](15-rv64-sbi-and-opensbi.md) |
| Sstc `stimecmp` | 获授权的 S 直接写 CSR | 可选扩展；M 通过 `menvcfg.STCE` 等授权，仍须探测硬件与权限 |

`time` 是计数器，不是以微秒为单位的架构时钟。时间频率来自设备树、固件或平台接口。比较值应按模数/溢出策略计算；RV32 写 64 位比较器时还要防止两半更新期间出现伪中断，RV64 的自然对齐 64 位写不需照搬 RV32 的序列。Sstc 与 SBI TIME 可以在同一系统里并存，内核应根据能力选择路径，而不应靠一次非法 CSR 访问来猜测。[Sstc 规范](https://docs.riscv.org/reference/isa/extensions/sstc/_attachments/Sstc.pdf)

`WFI` 是等待提示，可以无中断返回，某些实现允许把它当 NOP。它的唤醒条件与是否真正**进入 trap**不同：局部使能的 pending 中断可令 `WFI` 返回，即使该特权级的全局中断位暂时关闭；随后是否进入 handler 仍按中断投递条件判断。因此 idle 循环应“检查条件 → WFI → 再检查条件”，不能把每次从 `WFI` 返回都计作一次已处理的中断，也要分析检查与睡眠间的唤醒竞态。IPI 接收端同样应先处理共享队列，再按来源协议确认通知；**IPI 表示有事可做，并不替代共享数据的 release/acquire 顺序**。[WFI 与中断条件](https://docs.riscv.org/reference/isa/priv/machine.html)

## 4. PLIC 与 AIA 的路由区别

传统 QEMU `virt` 默认设备树通常提供 PLIC：外部源经过 priority、enable、threshold 路由到某 hart 的 M/S context；软件 claim 得到设备 ID，处理设备并 complete。PLIC 的 MMIO 地址和 context 编号来自设备树或平台描述，不能从 `mcause` 直接推导。

AIA 引入 APLIC 与 IMSIC：APLIC 可对有线中断做直送或转换为 MSI，IMSIC 为每 hart/特权级维护中断文件并接收 MSI。AIA 还影响外部中断优先级和虚拟化投递。**AIA 是平台/扩展组合，不能因为 CPU 支持某些 AIA CSR 就假定机器拥有 APLIC/IMSIC 设备。**本篇 Demo 只使用 QEMU 默认 `virt` 的机器级定时器；没有假装模拟 APLIC/IMSIC。设备与 CSR 的精确接口参见[AIA 规范](https://docs.riscv.org/reference/aia/v1.0/colophon.html)，更大的路由实验见 [Lab 13](/home/guoshichao/riscv_asm_lab/labs/lab13-plic-aia-interrupt-routing/README.md)。

多 hart bring-up 时还有一个常见边界：写目标 hart 的 MSIP 或使用 SBI IPI 前，要先发布共享任务、按内存模型排序，再发通知。目标 hart 清通知之后必须重新检查队列，处理通知合并和重复唤醒；在线/离线 hart 状态要与投递集合一致。[Lab 12](/home/guoshichao/riscv_asm_lab/labs/lab12-smp-ipi-timer-lifecycle/README.md)覆盖这组生命周期问题。

中断路由要按每层分别核对。例如设备中断在 PLIC 中已 pending，不代表目标 hart 已使能该 source；target context 的 threshold 可能挡住它；即便 PLIC 发出 SEIP，`mideleg`、`sie.SEIE` 和 `sstatus.SIE` 仍决定 S-mode 是否立即进入 handler。反过来，`scause=9` 只能说明收到了 S 外部中断，不能据此判断是哪一个设备。AIA 下也要区分 APLIC 的源配置和 IMSIC 中断文件的 pending/enable，不可把两套寄存器混成一个确认步骤。

### 4.1 常见症状的定位顺序

| 症状 | 优先检查 |
|---|---|
| 定时器反复陷入 | compare 是否仍小于当前 `mtime`；是否在源头解除 pending；返回前是否需要暂时关 MTIE |
| `WFI` 一直醒来但没有工作 | 是否把 `WFI` 当成必定只因目标中断返回；共享队列条件与通知清除顺序 |
| S-mode 定时器始终不进入 | TIME/Sstc 路径是否真的可用，`mideleg/sie.STIE/sstatus.SIE` 与 `stvec` 是否正确 |
| 外部中断到了错误 hart 或级别 | 控制器 target/context、`mideleg`、hart ID 映射、设备树中断描述 |
| handler 返回后数据损坏 | 异步入口是否保存全部 GPR、栈是否 per-hart、是否发生嵌套或 C 调用 |
| PLIC claim 后再次触发 | 设备自身原因是否清除、PLIC complete 顺序、level-triggered 信号是否仍有效 |

## 5. Demo：QEMU 机器级定时器产生一次异步 trap

本例使用 `-bios none` 从 M-mode 启动，QEMU `virt` hart 0 的 `mtimecmp=0x02004000`、`mtime=0x0200bff8`。这两个地址属于 QEMU 当前平台布局，**不是 RISC-V ISA 固定地址**。程序先把 compare 设为最大值，再设置 `now+100000`，打开 MTIE/MIE 并等待。handler 核对 `mcause` 最高位和 cause 7，清除定时器条件、关闭 MTIE，然后 `mret`。主线看到 `seen=1` 后向 QEMU finisher 写通过码。

保存为 `irq_demo.S`：

下面代码中，以点开头的名称是汇编伪操作，用来设置选项、布置节/符号或生成数据；它们不是 RV64 CPU 指令。`la`、`li` 等没有点号的写法则可能是由汇编器展开的伪指令（见第 0 节）。本例没有使用 `.macro`/`.endm` 定义用户宏。 后文的 `.ld` 文件使用 GNU ld 链接脚本命令，属于另一个处理阶段。[RISC-V 汇编手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)、[GNU as 伪操作手册](https://sourceware.org/binutils/docs/as/Pseudo-Ops.html)

```asm
    .option norelax  # 后续序列不标记为可链接器放宽；此处避免未初始化 gp 时被改写成 gp 相对寻址
    .section .text.init  # 切换到启动代码节；链接脚本将其放在入口代码所在区域
    .balign 4  # 按字节向上对齐到 4 的倍数，必要时插入填充；常用于 4 字节指令/陷入入口对齐
    .globl _start  # 将 _start 标成 ELF 全局符号；入口地址仍由链接器的 -e 或 ENTRY 指定
_start:
    la t0, m_trap  # 伪指令：构造符号地址，不读取该地址的内容；可能展开为 AUIPC+ADDI 等
    csrw mtvec, t0
    li t0, 0x02004000          # QEMU virt hart0 mtimecmp
    li t1, -1
    sd t1, 0(t0)               # clear an old MTIP condition first
    la t0, seen
    sw zero, 0(t0)
    li t0, 0x0200bff8          # mtime
    ld t1, 0(t0)
    li t2, 100000
    add t1, t1, t2
    li t0, 0x02004000
    sd t1, 0(t0)
    li t0, 0x80                # mie.MTIE
    csrs mie, t0
    csrsi mstatus, 8           # mstatus.MIE
wait:
    la t0, seen
    lw t1, 0(t0)
    bnez t1, pass
    wfi
    j wait

    .section .text  # 切换到可执行代码节，后续指令在这里编码
    .balign 4  # 按字节向上对齐到 4 的倍数，必要时插入填充；常用于 4 字节指令/陷入入口对齐
m_trap:
    csrr t0, mcause
    li t1, 1
    slli t1, t1, 63
    ori t1, t1, 7              # interrupt + machine timer cause
    bne t0, t1, fail
    li t0, 0x02004000
    li t1, -1
    sd t1, 0(t0)               # acknowledge at the source
    li t0, 0x80
    csrc mie, t0               # mask MTIE before returning
    la t0, seen
    li t1, 1
    sw t1, 0(t0)
    mret
pass:
    li t0, 0x100000
    li t1, 0x5555
    sw t1, 0(t0)
1:  j 1b
fail:
    li t0, 0x100000
    li t1, 0x13333
    sw t1, 0(t0)
2:  j 2b

    .section .bss  # 切换到零初始化节；运行时占内存，目标文件通常不保存同等数量的零字节
    .balign 4  # 按字节对齐到 4 的倍数，让后面的 32 位共享变量自然对齐
seen: .word 0  # 为零初始化变量预留 4 字节；RV64 的 .word 仍是 32 位数据
```

保存为 `irq_demo.ld`：

```ld
OUTPUT_ARCH(riscv)
ENTRY(_start)
PHDRS {
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS {
    . = 0x80000000;
    .text : { *(.text.init) *(.text .text.*) } :text
    . = ALIGN(4096);
    .bss : { *(.bss .bss.*) *(COMMON) } :data
}
```

构建与运行：

```bash
riscv64-unknown-elf-gcc -march=rv64i_zicsr -mabi=lp64 \
    -nostdlib -nostartfiles -Wl,--no-relax -T irq_demo.ld \
    -o irq_demo.elf irq_demo.S
timeout 10s qemu-system-riscv64 -machine virt -bios none \
    -nographic -smp 1 -m 128M -kernel irq_demo.elf
echo $?
```

当前 QEMU 环境退出码为 `0`；`1` 是示例断言失败，`124` 是超时。QEMU finisher 地址 `0x100000` 也是平台约定。本例的 trap handler 直接使用 `t0/t1`，因为主线在受控循环中每次重新计算地址；真实异步 handler 必须保存完整 GPR、处理中断嵌套与 per-hart 栈，不能复制这段简化入口到生产固件。
