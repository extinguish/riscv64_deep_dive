# RISC-V 64 位系统开发：Sv39 页表、权限与 SFENCE.VMA

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `AND` | **AND** | 真实指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `CSRR` | **Read Control and Status Register** | 伪指令 |
| `CSRW` | **Write Control and Status Register** | 伪指令 |
| `ECALL` | **Environment Call** | 真实指令 |
| `J` | **Jump** | 伪指令 |
| `LA` | **Load Address** | 伪指令 |
| `LD` | **Load Doubleword** | 真实指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `MRET` | **Machine Return** | 真实指令 |
| `OR` | **OR** | 真实指令 |
| `ORI` | **OR Immediate** | 真实指令 |
| `SD` | **Store Doubleword** | 真实指令 |
| `SFENCE.VMA` | **Supervisor Memory-Management Fence** | 真实指令 |
| `SLLI` | **Shift Left Logical Immediate** | 真实指令 |
| `SRLI` | **Shift Right Logical Immediate** | 真实指令 |
| `SW` | **Store Word** | 真实指令 |

ABI 别名是同一套 `x0～x31` 整数寄存器的另一种写法，并非额外寄存器；下面给出本文提及的整数寄存器编号与对应 ABI 别名。`PC` 若在本文出现，指 **Program Counter**（程序计数器），它不属于 `x0～x31`。

| ABI 别名 | 硬件编号 | 英文完整名称 |
|---|---|---|
| `zero` | `x0` | **Zero** |
| `t0` | `x5` | **Temporary register 0** |
| `t1` | `x6` | **Temporary register 1** |
| `t2` | `x7` | **Temporary register 2** |
| `t3` | `x28` | **Temporary register 3** |
| `t4` | `x29` | **Temporary register 4** |

`CSR` = **Control and Status Register**（控制与状态寄存器）。本文出现的 CSR 名称如下；这些是独立的 CSR 地址，不是 `x` 寄存器的别名。名称以 `m/s/h/vs` 开头通常分别对应 Machine / Supervisor / Hypervisor / Virtual Supervisor，实际可访问性还取决于特权级和扩展。

| CSR 名称 | 英文完整名称 |
|---|---|
| `mepc` | **Machine Exception Program Counter** |
| `mstatus` | **Machine Status** |
| `mtval` | **Machine Trap Value** |
| `mtvec` | **Machine Trap Vector** |
| `pmpaddr0` | **Physical Memory Protection Address register 0** |
| `pmpcfg0` | **Physical Memory Protection Configuration register 0** |
| `satp` | **Supervisor Address Translation and Protection** |
| `sstatus` | **Supervisor Status** |
| `stval` | **Supervisor Trap Value** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

Sv39 用三级页表把 S/U-mode 的虚拟地址映射到物理地址。系统开发里最危险的误解通常不是“三级索引怎么算”，而是把 `satp` 写入、PTE 存储、本 hart 的 `SFENCE.VMA`、其他 hart 的同步混成同一个动作。本篇以 **修改叶子 PTE 后切换到另一物理页**为主线，配合几个短例子解释页故障、权限、ASID 和 shootdown。

## 1. 虚拟地址、`satp` 与三级遍历

Sv39 使用 39 位有效虚拟地址：`VA[38]` 必须向 `VA[63:39]` 符号扩展。地址拆为 `VPN[2]`、`VPN[1]`、`VPN[0]` 各 9 位，加 12 位页内偏移。非规范地址产生页故障，不能只截取低 39 位继续访问。每级页表有 512 个 64 位 PTE，占 4 KiB 且须按 4 KiB 对齐。[Supervisor ISA / Sv39](https://docs.riscv.org/reference/isa/priv/supervisor.html)

```text
VA[63:39] = VA[38] 的符号扩展
VA[38:30] VPN[2] → 根页表
VA[29:21] VPN[1] → 第二级页表
VA[20:12] VPN[0] → 第三级页表
VA[11:0]  页内偏移
```

手算边界：`0x0000003fffffffff` 是低半区最后一个规范地址；`0xffffffc000000000` 是高半区第一个规范地址。`0x0000004000000000` 虽然低 39 位能拆出索引，bit 38 已为 1 而高位仍为 0，**不是规范地址**；硬件不会“自动截掉高位再访问”。访问它应按指令类型得到对应页故障。

RV64 `satp.MODE=8` 选择 Sv39，`ASID` 在 `satp[59:44]`，根页表的**物理**页号在 `satp[43:0]`。`satp` 写入不会自动完成地址转换缓存同步；当前实现支持的 ASID 位数也应写入后读回确认。本例仅使用 ASID 0，并在切换 `satp` 后执行 `sfence.vma x0,x0`。

## 2. PTE 的含义：叶子、权限与巨页

| 位 | 含义 | 常见陷阱 |
|---|---|---|
| `V` | 条目有效 | `V=0` 时其余位不能让映射继续有效 |
| `R/W/X` | 读/写/执行 | `W=1,R=0` 的组合无效；`R=1` 或 `X=1` 才是叶子 |
| `U` | U-mode 页 | S-mode 数据访问 U 页受 `SUM` 约束；S 不能从 U 页取指 |
| `G` | 全局映射 | 带 G 的条目不能只按一个 ASID 做失效 |
| `A/D` | 已访问/已写 | 清零后的行为依赖硬件 A/D 更新方案或 Svade，不能盲猜 |
| `RSW` | 软件保留 | 硬件忽略，软件协议自行定义 |

**非叶子 PTE 的 `V=1,R=W=X=0`，PPN 指向下一级页表。**其 `U/A/D` 位应清零；`G` 可以置位，表示整棵下级子树都是全局映射；`RSW` 由软件自用。非叶子的 R/W/X 不会“继承”给下级叶子。本篇 Demo 的非叶子只写 PPN 和 V，因此 `G=0`。在第 0 级找到叶子是 4 KiB 页，第 1 级是 2 MiB 页，第 2 级是 1 GiB 页。巨页的物理地址也必须按相应大小对齐，PTE 低位 PPN 不满足对齐会造成页故障。本例根表的两个 1 GiB 叶子用于 identity map 低 MMIO 和 DRAM；`VA=0x40000000` 使用三级映射。[非叶子与全局映射规则](https://docs.riscv.org/reference/isa/priv/supervisor.html)

若地址转换成功，访问仍可能被 PMP 拒绝，此时通常是 **access fault**；PTE 无效/权限不足通常是 **page fault**。故障 handler 要先按 cause 分类，再结合 `stval/mtval` 和当前页表查原因。补页成功后应重试原指令，不能像处理 `ecall` 那样盲目推进 EPC。与[PMP 篇](16-rv64-pmp-and-tee-isolation.md)的裸机实验对照看更清楚。

## 3. `SFENCE.VMA` 到底同步了什么

`SFENCE.VMA rs1,rs2` 同步**执行它的 hart**上先前的页表存储与之后的隐式地址转换访问；实现可用 TLB 等缓存翻译。它本身不是跨 hart 广播，也不是普通数据共享的万能内存屏障。修改 PTE 后，如果不做必要的同步，规范允许旧翻译或新翻译被使用；“我在 QEMU 上立即读到新页”不能证明省略 fence 正确。[Supervisor ISA / SFENCE.VMA](https://docs.riscv.org/reference/isa/priv/supervisor.html)

| 变化 | 本 hart 常见做法 | 原因 |
|---|---|---|
| 修改单个 4 KiB 叶子 PTE | `sfence.vma va, x0` | 针对 VA、所有 ASID，包括全局映射 |
| 修改非叶子 PTE 或调整巨页层级 | `sfence.vma x0, x0` | 下级覆盖多个 VA，旧中间级翻译可能存在 |
| 只修改某 ASID 的非全局叶子 | `sfence.vma va, asid_reg` | 可定向 ASID；不会失效全局映射 |
| 切换并复用 ASID 或一直用 ASID 0 | 按规范同步所需映射；保守做法 `sfence.vma x0,x0` | `satp` 写入自身不刷新旧翻译 |
| 多 hart 共享映射被修改 | 发布 PTE 更新、通知目标 hart、等待它们执行相应 fence | 本 hart fence 不替远端执行 |

`rs1` 是**寄存器中的虚拟地址值**，不是页号、PTE 地址或立即数；`rs2` 是 ASID 值所在寄存器，`x0` 表示所有 ASID。若 PTE 自身或其上级带 `G`，要用 `rs2=x0` 才包含全局映射。对于修改非叶子、把小页合并成巨页等复杂情况，还需要按规范先撤销旧路径并做足够范围的同步，避免多个有效翻译同时匹配。

远端流程通常是：写 PTE → 用相应内存排序/通知机制保证目标 hart 能看到页表写入 → 目标 hart 执行 `SFENCE.VMA` → 发送确认 → 才回收旧物理页。若已有 SBI RFENCE，可请求固件执行远程同步；但也要核对 SBI 参数、目标 hart 集合与错误返回。[SBI RFENCE 规范](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-rfence.adoc) 仅发送 IPI 而不执行 fence，或者未等目标 hart 确认就复用旧页，都可能让远端继续访问旧映射。

## 4. 权限组合的短 Demo 与排错

**例 A：`SUM`。** 假设叶子 PTE 的 `U=1,R=1`，S-mode 用 `ld` 读取：`sstatus.SUM=0` 时会因权限产生 load page fault；把 SUM 置 1 后，S-mode 的数据访问可被允许。SUM **不会**让 S-mode 从 U 页取指，M-mode 服务例程也不能因此放松 PMP 边界。切换 SUM 影响权限检查，不需要为这一位单独执行 `SFENCE.VMA`。

**例 B：`MXR`。** 假设叶子 `X=1,R=0`：普通 `ld` 不具备读取权限；设置 `sstatus.MXR=1` 后，若其他条件允许，可把可执行页当可读页。这个位的变化也不要求 `SFENCE.VMA`。安全域不能只靠“代码页不可读”隐藏机密，还要考虑 MXR、页表控制权和 PMP。

把两个位合并看更容易排错：一个 `U=1,R=0,X=1,A=1` 的页，S-mode 即使设置 `MXR=1`，在 `SUM=0` 时仍不能用 `ld` 读；同时设置 `SUM=1` 才可能通过页表权限检查，但仍必须通过 PMP。S-mode 对该 U 页的**取指**则无论 SUM/MXR 如何都不允许。这些例子中的 load/fetch 权限失败分别产生 load/instruction page fault，而不是 PMP 的 access fault。

**例 C：A/D。** 本篇页表预置 A/D 为 1，使演示不依赖硬件自动更新方式。真实系统若要用 A/D 实现 page aging，应查平台支持的方案：硬件可原子更新，也可能在 A/D 为 0 时产生 page fault（Svade）。把 A/D 为 0 的 fault 一概判为坏指针会误伤合法访问。

**例 D：撤销映射。** 先把旧叶子 PTE 置为无效并同步所有可能使用它的 hart，确认后才把旧物理页交给其他安全域。只更新发起 hart 的页表内存而不做远端 shootdown，会留下隔离窗口。若新旧映射在同一物理页上有不同 VA 别名，PMP 是按物理地址限制，页表层还要逐一检查别名权限。

## 5. Demo：一条虚拟地址从 page A 改映射到 page B

本例 `-bios none` 从 M-mode 启动。M 将 PMP 全放行给 S（只为隔离页表问题），建立如下映射，启用 `satp.MODE=Sv39` 后 `mret` 到 S：

```text
VA 0x00000000..0x3fffffff → 同 PA，1 GiB 叶子（含 QEMU finisher）
VA 0x40000000            → page_a，4 KiB 叶子
VA 0x80000000..0xbfffffff → 同 PA，1 GiB 叶子（代码、页表、数据）
```

S 先从 `VA=0x40000000` 读到 `0x1111`，把对应 L0 叶子 PTE 改成 page B，然后执行 `sfence.vma t0,x0`，再读到 `0x2222`。QEMU 的 0 退出状态说明两个断言均成立。**不要把删除 fence 后偶尔仍读到新值当作反证**：缺少同步时旧或新翻译都可能出现。

保存为 `sv39_demo.S`：

```asm
    .option norelax
    .equ PTE_RWX_AD, 0xcf         # V,R,W,X,A,D
    .equ PTE_RW_AD, 0xc7          # V,R,W,A,D
    .section .text.init
    .balign 4
    .globl _start
_start:
    la t0, m_trap
    csrw mtvec, t0
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x0f                # TOR full-range access for S
    csrw pmpcfg0, t0

    la t0, root
    li t1, 0xcf                # 1 GiB low MMIO identity map
    sd t1, 0(t0)               # root[0]: PA 0..1 GiB
    li t1, 0x200000cf         # root[2]: PA 0x80000000..0xbfffffff
    sd t1, 16(t0)

    la t1, l1
    srli t1, t1, 12
    slli t1, t1, 10
    ori t1, t1, 1              # non-leaf V=1
    sd t1, 8(t0)               # root[1]: VA 0x40000000
    la t0, l1
    la t1, l0
    srli t1, t1, 12
    slli t1, t1, 10
    ori t1, t1, 1
    sd t1, 0(t0)               # l1[0]
    la t0, l0
    la t1, page_a
    srli t1, t1, 12
    slli t1, t1, 10
    ori t1, t1, PTE_RW_AD
    sd t1, 0(t0)               # l0[0]: VA 0x40000000 -> page_a

    la t0, root
    srli t0, t0, 12
    li t1, 8 << 60
    or t0, t0, t1
    csrw satp, t0
    sfence.vma x0, x0
    la t0, s_entry
    csrw mepc, t0
    csrr t0, mstatus
    li t1, ~(3 << 11)
    and t0, t0, t1
    li t1, 1 << 11
    or t0, t0, t1
    csrw mstatus, t0
    mret

    .section .text
    .balign 4
s_entry:
    li t0, 0x40000000
    ld t1, 0(t0)
    li t2, 0x1111
    bne t1, t2, fail
    la t3, l0
    la t4, page_b
    srli t4, t4, 12
    slli t4, t4, 10
    ori t4, t4, PTE_RW_AD
    sd t4, 0(t3)              # replace leaf PTE
    sfence.vma t0, x0         # local VA, all ASIDs (ASID=0 here)
    ld t1, 0(t0)
    li t2, 0x2222
    bne t1, t2, fail
    j pass

m_trap:
    j fail
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

    .section .data
    .balign 4096
page_a:
    .dword 0x1111
    .zero 4088
    .balign 4096
page_b:
    .dword 0x2222
    .zero 4088

    .section .bss
    .balign 4096
root:
    .skip 4096
    .balign 4096
l1:
    .skip 4096
    .balign 4096
l0:
    .skip 4096
```

保存为 `sv39_demo.ld`：

```ld
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x80000000;
    .text : { *(.text.init) *(.text .text.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    . = ALIGN(4096);
    .bss : { *(.bss .bss.*) *(COMMON) }
}
```

构建、运行：

```bash
riscv64-unknown-elf-gcc -march=rv64i_zicsr -mabi=lp64 \
    -nostdlib -nostartfiles -Wl,--no-relax -T sv39_demo.ld \
    -o sv39_demo.elf sv39_demo.S
timeout 10s qemu-system-riscv64 -machine virt -bios none \
    -nographic -smp 1 -m 128M -kernel sv39_demo.elf
echo $?
```

`0` 为两个断言成功；`1` 是 QEMU finisher 收到失败码；`124` 是超时。该 finisher 地址是 QEMU `virt` 设备约定。根表的 MMIO 1 GiB 映射只为演示方便，实际内核应按设备属性与最小权限建立映射。Demo 未实现完整 trap frame，也未测试多 hart shootdown；更完整的页表实验见 [Lab 06](/home/guoshichao/riscv_asm_lab/labs/lab06-sv39-page-tables-tlb/README.md)。两阶段地址转换与 G-stage fence 接着看[H 扩展篇](20-rv64-h-extension-and-guest-stage.md)。
