# Lab 06：Sv39 页表、权限、ASID 与 TLB

本实验把 Q106～Q119 中可在 QEMU `virt` 裸机环境真实执行的部分组织为一个
Sv39 实验。重点不是只算出几个 VPN，而是把 PTE 合法性、page walk、live mapping、
page fault、`satp`/ASID 和 TLB invalidation 串成一条可运行路径。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64；
- 启动于 M-mode，测试主体进入 S-mode 并启用 Sv39；
- 平台：QEMU `virt`，单 hart；
- page size 为 4 KiB，每级 512 个 64-bit PTE；
- 基础设施为 RAM、UART 和 test finisher 建立恒等映射；
- `0x40000000` 起的隔离 VA 分支专供学员 map/unmap；
- 不假设 Sv48、Svnapot、Svpbmt、Svinval 或硬件 A/D 更新策略以外的扩展。

## 基础设施与学员边界

### 基础设施已经提供

- M-mode reset、BSS/stack、开放式 PMP baseline 和 M→S 转换；
- 基础设施拥有的 Sv39 root，以及代码、栈、页表和 MMIO 恒等映射；
- 独立的 smoke leaf，用于证明分页、S-mode 和 alias load 路径可运行；
- S-mode page-fault vector、预期 fault fixup 和 `scause/stval` 记录；
- 两个物理测试页、隔离的三级页表分支及 software-walk scratch tables；
- M-mode 单元测试和 S-mode live mapping/permission 测试；
- ELF、W^X segment、page-table section、relocation 和反汇编检查。

基础设施不会调用学员函数来建立启动所需的 identity mapping。即使 TODO 尚未完成，
`smoke` 仍应通过，`unit`/`test` 应明确失败而不是在打开 `satp` 后失去控制台。

### 需要你完成

只修改 `exercises/*.S`。

1. Sv39 地址与 `satp`
   - `lab_sv39_is_canonical(va)`：检查 bit 63:39 是否为 bit 38 的符号扩展；
   - `lab_sv39_vpn(va, level)`：提取 VPN[0..2]，非法 level 返回 `-1`；
   - `lab_make_satp(root, asid)`：检查 4 KiB 对齐和 16-bit ASID，构造 Sv39 value；
   - `lab_switch_address_space(root, asid)`：写 `satp` 后执行完整本地 invalidation。

2. PTE 构造
   - `lab_make_table_pte(pa)`：构造只含 V 和 PPN 的 non-leaf PTE；
   - `lab_make_leaf_pte(pa, flags)`：拒绝未对齐 PA、W=1/R=0、无 R/X 的 non-leaf；
   - flags 参数不含 V，只允许 R/W/X/U/G/A/D。

3. Software page walk
   - `lab_sv39_translate(root, va, access, output_pa)` 执行三级 walk；
   - 检查 canonical、V/R/W/X/U/A/D、SUM/MXR 和访问类型；
   - 识别任意 level leaf，拒绝 misaligned megapage/gigapage；
   - 区分 canonical、invalid、permission 和 superpage alignment 错误。

4. Live 4 KiB mapping
   - `lab_install_4k(root, va, pa, flags)` 只沿已存在的 non-leaf 路径安装 leaf；
   - `lab_unmap_4k(root, va)` 清除 leaf；
   - PTE store 必须先对 page-table walker 可见，再对目标 VA 执行 `SFENCE.VMA`；
   - 不得把 PTE invalidation 当作物理页已经可回收的充分条件。

## 使用方法

```sh
cd labs/lab06-sv39-page-tables-tlb

./build.sh toolcheck
./build.sh smoke       # infrastructure-owned Sv39 alias，初始应 PASS
./build.sh unit        # M-mode helper contract，初始应 FAIL
./build.sh build
./build.sh test        # S-mode live mapping/fault flow，初始应 FAIL
./build.sh inspect
./build.sh clean
```

推荐顺序：canonical/VPN → PTE → software walk → map/unmap → satp switch。

## 自动测试覆盖

- 正负 canonical VA、三级 VPN 和 `satp` field placement；
- table/leaf PTE 合法与非法权限组合；
- 4 KiB leaf、2 MiB leaf、permission failure 和 misaligned superpage；
- live alias 首次映射、改映射后的本地 TLB invalidation；
- read-only store page fault、unmapped load page fault及 `scause/stval`；
- 相同 root 切换到新 ASID 后的 `satp` 读回。

## 必须人工完成的证明

在 `build/lab06.disasm.txt` 和实验报告中说明：

1. 每一级 PTE 地址、leaf level 和 PA 拼接过程；
2. `R=0,W=1`、A/D、U/SUM/MXR 分别在哪一步导致 fault；
3. 2 MiB/1 GiB leaf 为什么要求低位 PPN 为零；
4. PTE store 与 `SFENCE.VMA` 的顺序，以及该指令为何只作用当前 hart；
5. `sfence.vma va,asid`、`va,x0`、`x0,asid`、`x0,x0` 的选择差异；
6. ASID rollover 与远端 shootdown ack 为什么超出本地 `satp` switch；
7. page fault、PMP/PMA access fault 和 page-table walk access fault 的归属差异。

## 实验边界

- 不实现动态 page-table allocator，intermediate tables 由基础设施预建；
- 不自动验证远端 TLB shootdown；该协议需要 IPI/SBI 与多 hart ack；
- 不覆盖 PMP policy 设计、Cache/DMA/IOMMU，它们属于后续 Lab；
- 不把 QEMU 的 TLB 行为当作所有实现都必须采用的 microarchitecture；
- 不测试 Svnapot、Svpbmt、Svinval 或 hypervisor two-stage translation。
