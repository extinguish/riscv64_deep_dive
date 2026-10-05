# RISC-V 64 位系统开发：PMP、Smepmp 与 TEE 物理隔离

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准；“真实指令”表示有机器编码，但可能属于可选扩展或特权架构，并不表示仅用 `-march=rv64i` 就能汇编、执行。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `AND` | **AND** | 真实指令 |
| `BEQZ` | **Branch if Equal to Zero** | 伪指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `CSRR` | **Read Control and Status Register** | 伪指令 |
| `CSRW` | **Write Control and Status Register** | 伪指令 |
| `J` | **Jump** | 伪指令 |
| `LA` | **Load Address** | 伪指令 |
| `LD` | **Load Doubleword** | 真实指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `MRET` | **Machine Return** | 真实指令 |
| `OR` | **OR** | 真实指令 |
| `SRLI` | **Shift Right Logical Immediate** | 真实指令 |
| `SW` | **Store Word** | 真实指令 |

ABI 别名是同一套 `x0～x31` 整数寄存器的另一种写法，并非额外寄存器；下面给出本文提及的整数寄存器编号与对应 ABI 别名。`PC` 若在本文出现，指 **Program Counter**（程序计数器），它不属于 `x0～x31`。

| ABI 别名 | 硬件编号 | 英文完整名称 |
|---|---|---|
| `zero` | `x0` | **Zero** |
| `t0` | `x5` | **Temporary register 0** |
| `t1` | `x6` | **Temporary register 1** |
| `t2` | `x7` | **Temporary register 2** |
| `a0` | `x10` | **Argument register 0** |

`CSR` = **Control and Status Register**（控制与状态寄存器）。本文出现的 CSR 名称如下；这些是独立的 CSR 地址，不是 `x` 寄存器的别名。名称以 `m/s/h/vs` 开头通常分别对应 Machine / Supervisor / Hypervisor / Virtual Supervisor，实际可访问性还取决于特权级和扩展。

| CSR 名称 | 英文完整名称 |
|---|---|
| `mcause` | **Machine Trap Cause** |
| `mepc` | **Machine Exception Program Counter** |
| `mseccfg` | **Machine Security Configuration** |
| `mstatus` | **Machine Status** |
| `mtval` | **Machine Trap Value** |
| `mtvec` | **Machine Trap Vector** |
| `pmpaddr0` | **Physical Memory Protection Address register 0** |
| `pmpaddr1` | **Physical Memory Protection Address register 1** |
| `pmpcfg0` | **Physical Memory Protection Configuration register 0** |
| `pmpcfg1` | **Physical Memory Protection Configuration register 1**；仅 RV32 有此编号的配置 CSR，本文用它说明 RV64 不应访问 |
| `pmpcfg2` | **Physical Memory Protection Configuration register 2** |
| `satp` | **Supervisor Address Translation and Protection** |
| `scause` | **Supervisor Trap Cause** |
| `stval` | **Supervisor Trap Value** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

PMP（Physical Memory Protection）是 M-mode 配置的**物理地址**访问检查。它既能向 S/U-mode 授权，也能限制其访问固件或 enclave 内存。PMP 是 TEE 的一块基础设施，但“PMP 配好了”尚不等于 TEE 隔离完成：多 hart、DMA、共享内存、切换时的寄存器与缓存状态仍要分别处理。本篇在 QEMU 裸机环境中设置两条 PMP 规则，让 S-mode 对一页秘密数据的读取产生 **load access fault**。

## 1. PMP 在地址转换链中的位置

S/U-mode 执行访存时，若启用 Sv39，先经页表把虚拟地址转换成物理地址，再按物理地址进行 PMP 等检查。页表 PTE 的 R/W/X 决定**虚拟映射许可**；PMP 决定**物理区域许可**。两者都要允许访问。页表无效或权限不满足通常是 page fault；PMP 拒绝通常是 access fault。使用 `satp=Bare` 也不绕过 PMP。页表遍历本身涉及物理内存访问，也要考虑 PMP 权限。[机器级 PMP 规范](https://docs.riscv.org/reference/isa/priv/machine.html)

不要从 `mtval/stval` 单独判断是哪一层拒绝，先看 `mcause/scause`。对于宽访问、跨边界或分解的访问，规范允许实现把一个访存拆为多个内存操作；发生部分写入与失败的组合时，不能只凭单一故障地址推断前面的字节都未生效。

## 2. 一个条目的编码与匹配

RV64 的 `pmpcfg0` 包含 8 个配置字节；每字节的低位是 `R/W/X`，`A[4:3]` 选择地址匹配方式，`L[7]` 锁定。高位保留。RV64 仅实现偶数编号的 `pmpcfg` CSR（如 `pmpcfg0`、`pmpcfg2`），不要按 RV32 的 CSR 排列写 `pmpcfg1`。PMP 条目数、地址位宽和粒度由实现决定，写完应读回检查。

| A 值 | 模式 | 覆盖范围 | 容易出错处 |
|---:|---|---|---|
| `0` | OFF | 不匹配 | `pmpaddr` 值仍可能影响下一条 TOR 的下界 |
| `1` | TOR | 前一 `pmpaddr` 到本条 `pmpaddr`，上界不含 | 第 0 条的下界固定为 0；边界按物理地址编码 |
| `2` | NA4 | 4 字节 | 粒度较粗的实现可能不支持 |
| `3` | NAPOT | 对齐的 8 字节及以上 2 的幂区域 | `pmpaddr` 低位 1 的个数表示区域大小 |

本篇 4 KiB `secret` 页的 NAPOT 编码是 `(secret >> 2) | 0x1ff`。一般 NAPOT 大小为 `2^(N+3)` 字节时，地址字段低 `N` 位设为 1，基址必须按区域大小对齐；不要把 NAPOT 编码中的低位 1 当作物理地址本身。需要先确认实现的 PMP 粒度是否支持该大小。

TOR 则用**相邻两个地址寄存器**给出半开区间：例如要描述 `[0x80000000, 0x80001000)`，可写 `pmpaddr0=0x80000000>>2`、`pmpaddr1=0x80001000>>2`，让 entry 0 的 `A=OFF`、entry 1 的 `A=TOR`，再给 entry 1 配权限。即使 entry 0 是 OFF，它的 `pmpaddr0` 仍是 entry 1 的下界；把 entry 0 地址清零会意外扩大 entry 1 的覆盖范围。边界会受实现的 PMP 粒度约束，必须读回确认。[TOR 编码规则](https://docs.riscv.org/reference/isa/priv/machine.html)

## 3. 优先级、权限与锁

**编号最小、与访问任意字节重叠的条目先决定结果。**它必须覆盖本次内存操作的所有字节，否则访问失败；硬件不会因为后面还有“允许所有”的规则而改用下一条。若它完整覆盖，则再看权限。S/U-mode 没有匹配条目时，在实现了 PMP 的 hart 上访问被拒绝；M-mode 在普通 PMP 中没有匹配条目时默认允许。若匹配条目的 `L=0`，M-mode 访问不受该条目 R/W/X 限制；`L=1` 后，R/W/X 也约束 M-mode。[PMP 匹配与锁定规则](https://docs.riscv.org/reference/isa/priv/machine.html)

本例按此顺序放两条规则：

```text
entry 0: secret 4 KiB, NAPOT, R=W=X=0, L=0      → S/U 拒绝，M 仍可访问
entry 1: 全物理地址, NAPOT, R=W=X=1, L=0       → 其余地址向 S/U 放行
```

对应低 16 位 `pmpcfg0 = 0x1f18`。若把 entry 1 放在前面，它先匹配秘密页，entry 0 永远没有机会拒绝。实际系统通常还要保护 M 固件、设备寄存器与域间共享页，而不能照搬“其余地址全放行”。本例的配置只用于观察优先级。

`L` 一旦置位，相关配置与地址寄存器的可写性按规范受限，通常要复位才解除。TOR 的上界条目锁定还会影响前一 `pmpaddr` 的可写性。初始化时先确定区域和优先级，再写地址和权限，读回验证后才锁定；不要把锁定位当成调试开关。`W=1,R=0` 是保留的权限组合，不能假设其效果。[PMP 规范](https://docs.riscv.org/reference/isa/priv/machine.html)

## 4. MPRV、页表遍历与 TEE 切换

`mstatus.MPRV=1` 时，M-mode 的**显式数据访存**按 `mstatus.MPP` 指定的权限执行；取指仍按 M-mode。固件用 MPRV 访问 S/U 缓冲区时，可能因此被 PMP 拦下。这是检查低权限可访问性的一种手段，但不能用“当前 PC 在 M-mode”推断所有数据访问都绕过 PMP；也不能忘记返回普通固件路径前恢复 MPRV。页表遍历和显式访存的权限细节应按硬件规范分别核对。

TEE 切换至少要建立以下不变量：

1. 进入 enclave 前，所有相关 hart 的 PMP 配置与域归属一致；退出时撤销或重设访问许可。
2. 不可信 S-mode 不可写 enclave 的私有页、页表或监控器自身代码与状态。
3. 共享缓冲区的所有权、清零、长度验证和并发访问有明确协议；页表别名不能突破 PMP 物理权限。
4. DMA 设备和其他总线主设备另需平台级隔离（例如 IOPMP/IOMMU）；**CPU PMP 本身不自动阻止 DMA**。
5. trap entry 保存寄存器并清除跨域敏感状态；必要时还要处理缓存侧信道、预测器和其他微架构状态。PMP 只定义访问许可，不能独自证明侧信道安全。

OpenSBI 的 domain 是软件管理框架，具体域隔离受平台保护能力约束；参见[OpenSBI domain 文档](https://github.com/riscv-software-src/opensbi/blob/master/docs/domain_support.md)。设计时要分别列出“固件可见”“普通 S 可见”“enclave 可见”“设备可见”的物理区域，而不是只列一个虚拟地址范围。

## 5. Smepmp：改变 M-mode 的默认信任假设

Smepmp 是可选扩展，增加 `mseccfg` 中的 MML、MMWP、RLB 等控制。MMWP 可让**未匹配 PMP 的 M-mode 访问也默认拒绝**；MML 修改锁定条目与 M/S/U 的权限解释；RLB 与锁定规则的绕过有关。相关字段具有锁定/粘滞语义，不能在运行中的安全系统上随意写位“探测支持”再试图清除。[Smepmp 规范](https://docs.riscv.org/reference/isa/priv/smepmp.html)

本篇 Demo **不启用 Smepmp**，其 `L=0` 时 M 仍能读取 secret 的行为只适用于普通 PMP 语义。移植到 Smepmp 平台前，先用设备树、平台文档和只读能力信息确认扩展，再按完整初始化时序设计规则；不能把本例的 `0x1f18` 原封不动移植。

## 6. Demo：S-mode 读取秘密页被 PMP 拒绝

`-bios none` 让程序直接从 M-mode 启动，`satp=Bare` 排除了页表干扰。M 在配置 PMP **前后**都读取 secret，验证未锁定条目不会限制 M 的普通访存，然后 `mret` 到 S。S 的 `ld` 必须陷入 M，handler 核对 `mcause=5`（load access fault）和 `mepc=try_load`；`mtval` 若非零，再核对它等于 secret。随后推进 EPC 并返回 S。若读取竟然成功，测试失败。这个设计同时验证了“页表未开启仍有 PMP”和“较高优先级拒绝覆盖后面的全放行”。

保存为 `pmp_demo.S`：

下面代码中，以点开头的名称是汇编伪操作，用来设置选项、布置节/符号或生成数据；它们不是 RV64 CPU 指令。`la`、`li` 等没有点号的写法则可能是由汇编器展开的伪指令（见第 0 节）。本例没有使用 `.macro`/`.endm` 定义用户宏。 后文的 `.ld` 文件使用 GNU ld 链接脚本命令，属于另一个处理阶段。[RISC-V 汇编手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)、[GNU as 伪操作手册](https://sourceware.org/binutils/docs/as/Pseudo-Ops.html)

```asm
    .option norelax  # 后续序列不标记为可链接器放宽；此处避免未初始化 gp 时被改写成 gp 相对寻址
    .section .text.init  # 切换到启动代码节；链接脚本将其放在入口代码所在区域
    .balign 4  # 按字节向上对齐到 4 的倍数，必要时插入填充；常用于 4 字节指令/陷入入口对齐
    .globl _start  # 将 _start 标成 ELF 全局符号；入口地址仍由链接器的 -e 或 ENTRY 指定
_start:
    la t0, m_trap  # 伪指令：构造符号地址，不读取该地址的内容；可能展开为 AUIPC+ADDI 等
    csrw mtvec, t0

    la t0, secret
    ld t1, 0(t0)                # M-mode reads before PMP configuration
    li t2, 0xdeadbeef  # 伪指令：把常量构造到寄存器，大常量可能需要多条真实指令
    bne t1, t2, fail

    srli t1, t0, 2
    li t2, 0x1ff
    or t1, t1, t2             # 4 KiB NAPOT region
    csrw pmpaddr0, t1
    li t1, -1
    csrw pmpaddr1, t1         # catch-all NAPOT region
    li t1, 0x1f18             # entry 0: deny; entry 1: R/W/X
    csrw pmpcfg0, t1
    csrr t2, pmpcfg0
    li t1, 0xffff
    and t2, t2, t1
    li t1, 0x1f18
    bne t2, t1, fail
    la t0, secret
    ld t1, 0(t0)                # L=0: M-mode still reads after configuration
    li t2, 0xdeadbeef
    bne t1, t2, fail

    la t0, s_entry
    csrw mepc, t0
    csrr t0, mstatus
    li t1, ~(3 << 11)
    and t0, t0, t1
    li t1, 1 << 11
    or t0, t0, t1             # MPP=S
    csrw mstatus, t0
    mret

    .section .text  # 切换到可执行代码节，后续指令在这里编码
    .balign 4  # 按字节向上对齐到 4 的倍数，必要时插入填充；常用于 4 字节指令/陷入入口对齐
s_entry:
    la t0, secret
try_load:
    ld a0, 0(t0)              # must trap with load access fault
    j fail
after_load:
    li t0, 0x5a
    bne a0, t0, fail
    j pass

m_trap:
    csrr t0, mcause
    li t1, 5                   # load access fault, not page fault
    bne t0, t1, fail
    csrr t0, mtval
    beqz t0, 1f                # mtval may be zero on a compliant implementation
    la t1, secret
    bne t0, t1, fail
1:
    csrr t0, mepc
    la t1, try_load
    bne t0, t1, fail
    la t0, after_load
    csrw mepc, t0
    li a0, 0x5a
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

    .section .secret,"aw",@progbits  # 定义可分配且可写的 PROGBITS 节；数据存于文件中，由链接脚本单独放置
    .balign 4096  # 按字节向上对齐到 4096 的倍数，必要时插入填充；满足 4 KiB 页或页表边界
secret:
    .dword 0xdeadbeef  # 在当前节放入一个 8 字节整数数据；不是运行时的 LD/SD 指令
    .zero 4088  # 在当前节写入 4088 个零字节；这里的填充值属于数据
```

保存为 `pmp_demo.ld`：

```ld
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x80000000;
    .text : { *(.text.init) *(.text .text.*) }
    . = ALIGN(4096);
    .secret : { *(.secret) }
}
```

构建、运行：

```bash
riscv64-unknown-elf-gcc -march=rv64i_zicsr -mabi=lp64 \
    -nostdlib -nostartfiles -Wl,--no-relax -T pmp_demo.ld \
    -o pmp_demo.elf pmp_demo.S
timeout 10s qemu-system-riscv64 -machine virt -bios none \
    -nographic -smp 1 -m 128M -kernel pmp_demo.elf
echo $?
```

QEMU `virt` 的 finisher 收到 `0x5555` 时进程退出码为 `0`；收到 `0x13333` 时为失败。`124` 是 `timeout` 产生的超时，通常要查 trap 循环或异常 EPC。finisher 地址 `0x100000` 是 QEMU 设备约定，不属于 RISC-V ISA。此例在当前 QEMU 环境退出码为 `0`。把代码中配置值和对应读回期望都从 `0x1f18` 改成 `0x1f1f`（让高优先级条目也放行）后，S-mode 读取成功，示例按预期退出 `1`；这证实失败判据确实依赖 PMP 拒绝行为。

`mtval` 对访问故障的报告有实现允许的差异：QEMU 当前报告 secret 地址，规范允许实现报告 0；因此代码只在非零时断言它等于目标地址。真实 trap handler 不能这样直接破坏 `t0/t1`，必须保存完整现场并使用独立栈。[Lab 07](/home/guoshichao/riscv_asm_lab/labs/lab07-pmp-isolation-access-fault/README.md)还覆盖 TOR、边界和 MPRV；前一篇[SBI/OpenSBI](15-rv64-sbi-and-opensbi.md)提供固件调用背景。
