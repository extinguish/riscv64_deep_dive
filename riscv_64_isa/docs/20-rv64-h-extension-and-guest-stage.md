# RISC-V 64 位系统开发：H 扩展、两阶段地址转换与 Guest Exit

前面的 [Sv39](17-rv64-sv39-and-sfence-vma.md)只有一个地址转换阶段。启用 H 扩展后，guest 的虚拟地址先由 VS-stage 变为 guest physical address（GPA），再由 G-stage 变为 host/supervisor physical address（SPA）。调试 guest fault 时若只看 `stval` 或只执行 `SFENCE.VMA`，很容易把两个阶段混淆。本篇用 QEMU `virt -cpu rv64,h=true` 运行一个真正的 VS-mode guest：先触发 G-stage load guest-page fault，HS 安装映射并 `HFENCE.GVMA`，guest 重试成功后以 VS `ECALL` 退出。

## 1. HS、VS、VU 与两阶段转换

| 状态 | 常见执行者 | 关键 CSR / 地址含义 |
|---|---|---|
| M-mode | 固件、监控器 | `mstatus/mepc/medeleg` 决定向 HS 交接和异常委托 |
| HS-mode（V=0 的 S） | host 内核或 hypervisor | `satp` 管理 host 映射；`hgatp` 管理 G-stage；`hstatus` 管理虚拟化返回状态 |
| VS-mode（V=1 的 S） | guest 内核 | 看见虚拟化的 S 级 CSR，例如 `vsstatus/vsatp/vstvec` |
| VU-mode（V=1 的 U） | guest 用户程序 | 受 guest 内核和 G-stage 双重约束 |

有 H 扩展时，原来的 S 变成 HS；VS/VU 不是额外的物理权限等级。guest 的访问必须同时通过 VS-stage 和 G-stage，再通过 PMP/PMA 等物理检查。**G-stage 不能替代设备 DMA/IOMMU 隔离，也不自动把 host 代码从 guest 地址空间排除。**[H 扩展规范](https://docs.riscv.org/reference/isa/priv/hypervisor)

本篇令 `vsatp=Bare`，因此 guest 的 GVA 数值直接作为 GPA 进入 G-stage，便于隔离第二阶段问题。真实 guest 通常启用 VS-stage Sv39：GVA→GPA→SPA。读取 guest 页表 PTE 本身也是 guest memory access，会经过 G-stage；因此 guest-page fault 的 GPA 可能指向**页表项所在地址**，而不是原始 load 的目标。此时还应结合 `htinst` 判断故障来自隐式页表遍历。

## 2. `hgatp` 与 Sv39x4 的几何

RV64 G-stage 的 `hgatp.MODE=8` 选择 Sv39x4；其 GPA 宽度比 Sv39 多 2 位，根表多 2 位索引，需 **16 KiB 对齐、2048 个 PTE**。根索引取 GPA `[40:30]`。后续页表级与普通 Sv39 类似，仍可用 1 GiB、2 MiB、4 KiB 叶子，并要求对应物理地址对齐。本例：

```text
GPA 0x80000000..0xbfffffff → SPA 同地址，1 GiB 叶子，供 guest 取指
GPA 0x40000000             → 起初无映射；fault 后增加 L1[0] 2 MiB 叶子
                                指向 SPA 0x80200000 的 backing 数据
```

G-stage PTE 的 `U` 位与通常 S-mode 页表的直觉不同：G-stage 有效叶子需要按规范设置 U，不能把“guest 在 VS-mode”误写成 U=0。页表还须允许 guest 代码取指、数据读取，并受 host PMP 权限约束。Demo 的 root[2] 直接给 guest 映射了 host 代码所在 1 GiB DRAM，**只是教学映射，不提供 VM 与 host 安全隔离**；生产系统必须只映射 guest 拥有的页。[G-stage 地址转换规则](https://docs.riscv.org/reference/isa/priv/hypervisor)

`hgatp` 中的 VMID 标识 G-stage 地址空间；复用 VMID、切换 MODE 或收紧映射时，都要执行所需 G-stage 同步。和 `satp` 一样，写 `hgatp` 本身不等于全局失效。若 guest 页表也改动，则还要考虑 VS-stage 的 ASID 与 `HFENCE.VVMA`。

## 3. Guest fault 的四组诊断数据

| 字段 | 本篇 load guest-page fault 的值 | 意义 |
|---|---:|---|
| `scause` | `21` | load guest-page fault；普通 load page fault 是 `13` |
| `stval` | `0x40000000` | 触发访问的 guest virtual address |
| `htval` | QEMU 报告 `0x10000000` | 此次 GPA `0x40000000 >> 2`；规范允许某些实现报告 0 |
| `hstatus.SPV` | `1` | trap 前在虚拟化模式，用于区分 host 自身故障 |

`htval` 不应一律左移 2 就当作原始请求的 GPA：它可能为 0；VS-stage 页表遍历故障时，它还可能指向 guest PTE 的 GPA。`htinst` 可提供转换后的指令或特殊伪指令信息。若 trap 被 M 处理，则对应信息在 `mtval2/mtinst`。Guest-page fault 能由 `medeleg` 委托给 HS，却不能再委托给 VS；guest 自身普通 page fault 的路由还受 VS/HS 委托状态影响。[Guest-page fault 规范](https://docs.riscv.org/reference/isa/priv/hypervisor)

VS `ECALL` 的 cause 是 **10**，与 U `ECALL` 的 8、S `ECALL` 的 9 不同。HS 在处理 guest SBI 时扮演 guest 的 SBI provider；不能把 guest 的 EID/FID、指针和 GPA 原样转发给 M-mode OpenSBI。需要按 VM 能力表验证服务、转换地址、检查所有权与长度，并处理 guest 可见返回值。trap entry 还必须保存 guest 的 GPR、VS CSR、可能的浮点/向量状态和虚拟中断状态；本例仅用受控路径验证地址转换。

一个最小 vCPU 上下文至少还要明确这些状态的归属：

| 状态 | 作用 | 切换时的风险 |
|---|---|---|
| `vsstatus/vsepc/vscause/vstval/vstvec/vsatp` | guest S 级 trap 与页表状态 | 把前一个 VM 的 trap 入口或 ASID 留给下一个 VM |
| `hstatus/hedeleg/hideleg` | HS↔VS 返回与委托 | 把 host fault 当成 guest exit；错误地把 guest fault 投递给 VS |
| `hvip/hip/hie`、AIA guest file | 虚拟中断 pending/enable | 旧 VM 的待处理中断泄露到新 VM |
| `htimedelta/hcounteren/henvcfg` | guest 时间、计数器与扩展授权 | 时间回退、计数器越权或 CBO/其他环境功能误授权 |
| `hgatp` VMID 与 G-stage 根 | GPA→SPA 映射 | VMID 复用未同步，旧翻译命中其他 VM 的页 |

保存/恢复哪些寄存器取决于实际启用的扩展和调度策略；懒保存不能把前一个 VM 的敏感状态暴露给后一个 VM。这里的表是设计检查项，Demo 没有实现完整 vCPU 调度器。

## 4. 三种地址转换 fence 的对象

| 指令 | 主要同步对象 | 操作数容易写错处 |
|---|---|---|
| `SFENCE.VMA`（HS V=0） | HS 的 `satp` 映射 | `rs1` 为 host VA |
| `HFENCE.VVMA` | 当前 VM 的 VS-stage `vsatp` 映射 | `rs1` 为 GVA；`rs2` 为 guest ASID |
| `HFENCE.GVMA` | `hgatp` 的 G-stage 映射 | `rs1` 为 **GPA>>2**；`rs2` 为 VMID |

`HFENCE.GVMA x0,x0` 覆盖所有 GPA/VMID；传入地址时不要把 GPA 原值当作 operand。本例 fault 后使用 `0x40000000 >> 2 = 0x10000000`。这些 fence 只同步执行它的 hart；远端 vCPU 可能在另一个 hart 上继续使用旧翻译，页回收前必须完成跨 hart shootdown 与确认。虚拟化的 IPI、AIA guest interrupt file、设备直通/IOMMU、ATS 和 DMA quiesce 又各有独立协议。[HFENCE 规范](https://docs.riscv.org/reference/isa/priv/hypervisor)

Demo 在写页表后还用了 `fence rw,rw` 作为保守的显式内存排序；**让后续 G-stage 隐式页表读取使用新 PTE 的关键指令仍是 `HFENCE.GVMA`**。`FENCE` 不能代替它，单 hart 的 `HFENCE.GVMA` 也不能代替通知并同步运行在其他 hart 上的 vCPU。

## 5. Demo：缺失 G-stage 映射，补页后重试

代码从 M-mode 开始，检查 `misa.H`，全放行 PMP，并把 VS `ECALL` 与 load guest-page fault 委托给 HS。HS 建立 Sv39x4 root，设置 `hstatus.SPV=1` 与 `sstatus.SPP=1`，用 `sret` 进入 VS。第一次 guest load 因 root[1] 的 L1[0] 无效而 trap；HS 核对 cause、`stval`，若 `htval` 非零再核对它是 GPA 右移两位的值。然后安装指向 `0x80200000` 的 2 MiB 只读叶子，执行 `HFENCE.GVMA`，从同一条 load 重试。最后 VS `ECALL` 交给 HS，HS 同时核对读取值为 `0x1234` 且补页 fault 恰好发生一次。

保存为 `h_demo.S`：

```asm
    .option norelax
    .section .text.init
    .balign 4
    .globl _start
_start:
    la t0, m_trap
    csrw mtvec, t0
    csrr t0, misa
    andi t0, t0, 128             # H extension
    beqz t0, fail
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x1f
    csrw pmpcfg0, t0             # demo: allow S/VS all physical memory
    li t0, (1 << 10) | (1 << 21)
    csrw medeleg, t0            # VS ecall and load guest-page fault to HS
    csrr t1, medeleg
    and t1, t1, t0
    bne t1, t0, fail
    la t0, hs_entry
    csrw mepc, t0
    csrr t0, mstatus
    li t1, ~(3 << 11)
    and t0, t0, t1
    li t1, 1 << 11
    or t0, t0, t1               # MPP=S (HS)
    csrw mstatus, t0
    mret

    .section .text
    .balign 4
hs_entry:
    la t0, hs_trap
    csrw stvec, t0
    csrw vsatp, zero            # VS-stage Bare
    la t0, fault_count
    sw zero, 0(t0)
    la t0, root
    li t1, 0x200000df          # root[2]: GPA 0x80000000 -> same PA, 1 GiB
    sd t1, 16(t0)
    la t1, l1
    srli t1, t1, 12
    slli t1, t1, 10
    ori t1, t1, 1
    sd t1, 8(t0)               # root[1] -> L1; L1[0] initially invalid
    fence rw, rw
    srli t0, t0, 12
    li t1, 8 << 60
    or t0, t0, t1
    csrw hgatp, t0             # Sv39x4, VMID 0
    hfence.gvma x0, x0
    la t0, guest_entry
    csrw sepc, t0
    li t0, 0x100
    csrs sstatus, t0           # SPP=VS
    li t0, 0x80
    csrs hstatus, t0           # SPV=1: sret enters virtual mode
    sret

guest_entry:
    li t0, 0x40000000
guest_load:
    ld a0, 0(t0)               # first time: load guest-page fault
    ecall                       # VS ecall to HS; a0 should be magic
1:  j 1b

hs_trap:
    csrr t0, hstatus
    andi t0, t0, 0x80
    beqz t0, fail              # must have come from VS
    csrr t0, scause
    li t1, 21
    beq t0, t1, map_fault
    li t1, 10
    bne t0, t1, fail
    la t0, fault_count
    lw t1, 0(t0)
    li t2, 1
    bne t1, t2, fail           # the load must have faulted exactly once
    li t0, 0x1234
    bne a0, t0, fail
    j pass
map_fault:
    la t0, fault_count
    lw t1, 0(t0)
    bnez t1, fail             # never accept a second fault
    li t1, 1
    sw t1, 0(t0)
    csrr t0, sepc
    la t1, guest_load
    bne t0, t1, fail
    csrr t0, stval
    li t1, 0x40000000
    bne t0, t1, fail
    csrr t0, htval
    beqz t0, 4f                 # zero is permitted by the ISA
    li t1, 0x10000000          # GPA >> 2
    bne t0, t1, fail
4:
    la t0, l1
    ld t1, 0(t0)
    bnez t1, fail             # exactly one fault
    li t1, 0x80200000
    srli t1, t1, 12
    slli t1, t1, 10
    ori t1, t1, 0xd3           # V,R,U,A,D; no W/X
    sd t1, 0(t0)
    fence rw, rw
    li t0, 0x10000000          # operand = GPA >> 2
    hfence.gvma t0, x0
    li t0, 0x40000000          # restore guest's base register
    sret                       # retry the same ld

m_trap:
    j fail
pass:
    li t0, 0x100000
    li t1, 0x5555
    sw t1, 0(t0)
2:  j 2b
fail:
    li t0, 0x100000
    li t1, 0x13333
    sw t1, 0(t0)
3:  j 3b

    .section .bss
    .balign 16384
root: .skip 16384
    .balign 4096
l1:   .skip 4096
fault_count: .word 0

    .section .guest_data,"aw",@progbits
    .balign 2097152
backing: .dword 0x1234
```

保存为 `h_demo.ld`：

```ld
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x80000000;
    .text : { *(.text.init) *(.text .text.*) }
    . = ALIGN(16384);
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = 0x80200000;
    .guest_data : { *(.guest_data) }
}
```

构建、运行：

```bash
riscv64-unknown-elf-gcc -march=rv64ih_zicsr -mabi=lp64 \
    -nostdlib -nostartfiles -Wl,--no-relax -T h_demo.ld \
    -o h_demo.elf h_demo.S
timeout 10s qemu-system-riscv64 -machine virt -cpu rv64,h=true \
    -bios none -nographic -smp 1 -m 128M -kernel h_demo.elf
echo $?
```

当前 QEMU 环境退出码 `0`，`1` 表示断言失败，`124` 是超时。QEMU 此次提供非零 `htval=0x10000000`；代码同时接受规范允许的 `htval=0`，但非零且不匹配预期 GPA 时失败。通过测试证明 fault→补页→重试这条路径成立，**不证明 QEMU 必须按地址精确失效**：实现允许把定向 `HFENCE.GVMA` 执行为更宽范围的 fence，因此仍要静态检查代码确实传入 `GPA>>2`。若 M/HS 的页表、PMP 或委托配置不同，也应据此修改测试。完整的 guest trap frame、权限负例和 VM 生命周期练习见 [Lab 14](/home/guoshichao/riscv_asm_lab/labs/lab14-hypervisor-guest-stage/README.md)。
