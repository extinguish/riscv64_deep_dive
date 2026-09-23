# Lab 07：PMP 隔离、优先级与 Access Fault

本实验把 Q123～Q127 扩展为一个可运行的 PMP policy lab。目标是掌握 TOR/NA4/NAPOT
编码、最低编号优先、跨边界访问、动态更新顺序，以及 page fault 与 PMP access fault
之间的归因边界。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64；
- 平台：QEMU `virt`，单 hart，M-mode 裸机；
- 使用 MPRV+MPP=S 发起受 PMP 约束的数据访问，指令流仍留在 M-mode；
- entry 1 由基础设施配置为低优先级 allow-all baseline；
- entry 0 是学员管理的高优先级 4 KiB 测试窗口；
- live test 不写 L bit，避免一次错误把本次 QEMU 运行不可逆地锁死；
- 不假设 Smepmp、ePMP domain 或 vendor-specific region 扩展。

## 基础设施与学员边界

### 基础设施已经提供

- M-mode reset、BSS、stack、UART 和 test finisher；
- entry 1 allow-all safety baseline，确保 TODO 不会切断代码/栈/MMIO；
- MPRV load/store probe、M-mode trap fixup 和 `mcause/mtval` 记录；
- 独立的 4 KiB 测试页；
- 纯软件 PMP matcher contract tests；
- live deny、read-only、read-write 和 disable 流程；
- CSR readback、ELF 和反汇编检查。

基础设施的 entry 1 仅用于维持实验控制面。entry 0 仍按架构的最低编号优先规则
覆盖测试页，因此不会替代学员的编码和更新逻辑。

### 需要你完成

只修改 `exercises/*.S`。

1. PMP 编码/解码
   - `lab_pmp_napot_encode(base, size)`：验证 size 为至少 8 byte 的 2 次幂，
     base 按 size 对齐，返回 `pmpaddr`；失败返回 `UINT64_MAX`；
   - `lab_pmp_napot_decode(pmpaddr, base_out, size_out)`：恢复 NAPOT 区间；
   - `lab_pmp_tor_encode(top)`：验证 4 byte 对齐并返回 `top >> 2`；
   - 明确 NA4 与 NAPOT 8-byte 在 A 字段上的区别。

2. Config byte
   - `lab_pmp_make_cfg(perms, mode, locked)` 构造 R/W/X/A/L；
   - 拒绝 W=1/R=0、非法 mode、额外 permission bits 和非布尔 locked；
   - OFF entry 不得携带有效权限。

3. Software policy matcher
   - `lab_pmp_match(addrs, packed_cfg, count, address, size, access)`；
   - 按最低编号优先解析 OFF/TOR/NA4/NAPOT；
   - 第一个与任意访问 byte 相交的 entry 必须覆盖整个 access，否则拒绝；
   - 无 entry 匹配时按 S/U-mode 默认拒绝；
   - 检查 address+size overflow 和 R/W/X 权限。

4. Live PMP window
   - `lab_pmp_program_window(base, size, permissions)` 更新 entry 0；
   - 使用 deny-before-readdress-before-enable 顺序，保留 entry 1～7 config；
   - `lab_pmp_disable_window()` 只关闭 entry 0；
   - 当前实验不允许设置 L bit，permissions 只接受 R/W/X。

## 使用方法

```sh
cd labs/lab07-pmp-isolation-access-fault

./build.sh toolcheck
./build.sh smoke       # baseline + MPRV access，初始应 PASS
./build.sh unit        # 编码与软件 matcher，初始应 FAIL
./build.sh build
./build.sh test        # live PMP window，初始应 FAIL
./build.sh inspect
./build.sh clean
```

推荐顺序：NAPOT → config byte → matcher → live program/disable。

## 自动测试覆盖

- 8-byte、4 KiB NAPOT encode/decode 及 alignment/power-of-two 错误；
- TOR top 和 config byte 合法/非法组合；
- entry 0 deny 覆盖 entry 1 allow、entry OFF fallback 和跨边界拒绝；
- entry 0 deny 下的 load/store access fault；
- read-only load 成功、store fault；
- read-write load/store 成功，以及 disable 后回退到 baseline；
- `mcause=5/7`、`mtval` 和 PMP CSR readback。

## 必须人工完成的证明

在 `build/lab07.disasm.txt` 和实验报告中说明：

1. NAPOT trailing-ones 与 region size/base 的双向推导；
2. TOR lower bound 为什么来自前一个 `pmpaddr`，即使前一 entry 为 OFF；
3. “最低编号优先”与“整个 access 必须落入同一 entry”如何组合；
4. entry 0 动态更新期间为什么要先 deny/disable，再改地址，最后发布权限；
5. MPRV probe 为什么约束 data access，却不等价于真正执行在 S-mode；
6. PMP access fault、PMA/bus access fault和 Sv39 page fault如何区分；
7. L bit 与 Smepmp MML/MMWP/RLB 为什么不能套用普通 PMP 的旧规则。

## 实验边界

- 不在 live test 中设置 L bit；错误 lock 只能通过 reset 恢复；
- 不实现 Smepmp、OpenSBI domain 或多 hart PMP rendezvous；
- 不验证 execute permission；instruction-fetch 测试需要独立受控 trampoline；
- 不把基础设施的 allow-all entry 当作生产系统安全策略；
- cacheability、DMA ownership 与 IOMMU translation 属于后续 Lab。
