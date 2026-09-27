# Lab 14：H 扩展、VS Guest 与 G-stage 翻译

本实验承接题库 Q231～Q270，在 QEMU `virt` 上运行一个真正的 VS-mode guest。
M-mode 只负责进入 HS；HS 建立 Sv39x4 G-stage，guest 访问未映射 GPA 产生
guest-page fault。HS 在 Trap 中安装只读映射、执行 `HFENCE.GVMA`，guest
重试 Load 成功，随后 Store 因权限被拒，再经一个 MMIO hole 的 G-stage
fault 和 VS `ecall` 退出。

## 基础设施与边界

- RV64IMACH + Zicsr + Zifencei、LP64、QEMU `virt -cpu rv64,h=true`、单 hart。
- `infra/` 提供 M→HS handoff、完整 VS GPR trap frame、host/guest stack swap、
  UART、finisher、guest 程序及三层测试。只修改 `exercises/gstage.S`。
- VS-stage 使用 Bare，重点观察 G-stage；G-stage 以 16 KiB x4 root 和一个
  4 KiB L1 表构成。基础设施给 guest 代码和栈建立 1 GiB identity leaf。
  GPA `0x40000000` 初始未映射，等待学员安装一个 2 MiB leaf，映射到独立的
  host backing region。GPA `0x10000000` 保持为 MMIO hole。
- 供 guest 执行的 1 GiB identity leaf 同时覆盖 host 固件内存，因此本实验
  **不提供 VM 与 host 的安全隔离**；权限负例只证明 alias leaf 的 G-stage
  写保护。安全隔离需要独立 guest image、精确的 G-stage 映射与资源归属。
- MMIO 实例仅模拟一个固定的 64-bit Store；这不是通用设备指令解码器。
- Trap 入口处理本实验的 VS→HS 退出；不支持 HS handler 内再发生同步 Trap
  或嵌套中断。生产环境需要独立的 emergency stack 和嵌套状态机。

## 需要完成

1. `lab_gstage_index(gpa)`：验证 41-bit GPA，提取 Sv39x4 root index `[40:30]`；
   越界返回 `UINT64_MAX`。
2. `lab_gstage_leaf(pa, flags)`：检查 PA 小于 2^56 且 2 MiB 对齐、PTE 合法
   flag（包括 W→R、U=1，leaf 必须有 R 或 X），生成 G-stage L1 leaf PTE；
   非法输入返回 0。
3. `lab_guest_fault(scause, hstatus, htval, stval)`：只在 `SPV=1` 且 fault
   GPA 与预期一致时识别 alias Load（返回 1）、MMIO Store（返回 2）和
   read-only alias Store（返回 3）；
   其他 Trap 返回 0。`htval` 保存的是 GPA 右移两位的值。
4. `lab_install_mapping(slot, pte, gpa)`：验证 slot 对齐、2 MiB GPA 对齐和
   合法的 G-stage L1 leaf PTE，返回旧 PTE；先发布新 PTE，
   再完成页表 Store ordering 和本 hart `HFENCE.GVMA`。该指令的 `rs1` 必须是
   **GPA 右移 2 位**，`rs2=x0` 表示全部 VMID。

## 使用

```sh
cd labs/lab14-hypervisor-guest-stage
./build.sh toolcheck
./build.sh smoke    # M→HS 和页表对齐；初始 PASS
./build.sh unit     # 学员函数边界；初始 FAIL
./build.sh test     # 真正进入 VS 并处理 Guest Exit；初始 FAIL
./build.sh inspect
```

## 验收与人工证明

完整测试要求 alias 缺页恰好 fault 一次，安装映射并重试成功；向只读
alias Store 恰好 fault 一次且 backing 未改变；MMIO Store 恰好 fault 一次、
保留正确的 guest 寄存器值；VS `ecall` 由 HS 正确接收。`inspect`
检查 RV64 H 属性、关键符号、W^X 与反汇编。

人工说明：HS/VS/VU 的关系；guest-page fault 与 VS-stage page fault 的归属；
Sv39x4 root 为什么是 16 KiB；VS page walk 的 GPA 访问也经过 G-stage；
`HFENCE.VVMA` 与 `HFENCE.GVMA` 的区别及远端 hart 失效；VMID 回绕、vCPU
迁移、AIA guest file、IOMMU/ATS 和物理页回收的同步顺序。该单 hart实验
没有验证远端 fence、真实 AIA、设备直通、nested virtualization 或完整 MMIO 模拟。

建议以题库 Q231～Q270 为索引，另提交以下人工证明：

1. **Q231～Q241：** 给出 H 扩展发现和无 H fallback；画出 M/HS/VS/VU、
   `hstatus.SPV/SPVP` 与 `hedeleg/hideleg` 的 trap 路由；说明为什么 HS host
   fault 不能误判成 guest exit，以及完整 vCPU frame 还需保存哪些 VS/H CSR。
2. **Q242～Q250：** 手推一次 GVA→GPA→SPA，包括 VS page-table walk 的
   G-stage 访问；说明 x4 root 几何、G-stage `U` 位、`htval` 的 GPA 编码、
   `HFENCE.VVMA/GVMA` 的 operand 差异与 guest-memory access fault fixup。
3. **Q251～Q255：** 为 guest CSR、`WFI`、virtual timer 和 `hvip` injection
   定义 trap/emulate 或直通策略；说明 AIA guest file 不可用时的回退。
4. **Q256～Q261：** 写出 vCPU 跨 hart 迁移、VMID 回绕、G-stage PTE 收紧和
   页回收的完成协议；讨论 dirty logging、huge page 与 demand paging。
5. **Q262～Q270：** 给出设备直通的 quiesce→IOTLB/ATS invalidation→reset
   顺序；区分 guest SBI provider 与 M-mode SBI；列出 FPU/Vector、PMU、
   stateen、nested virtualization 和 confidential VM 的能力边界与威胁模型。

`smoke` 只检查基础设施；`unit` 能定位 TODO 函数；`test` 还会静态检查
`lab_install_mapping` 含有 `HFENCE.GVMA`。完成练习后再用 `test` 验证真实路径。
