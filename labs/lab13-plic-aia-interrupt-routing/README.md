# Lab 13：PLIC、APLIC、IMSIC 与中断路由状态机

本实验把题库 Q208～Q230 扩展为一个 legacy PLIC 实机路径加 AIA 严格模型的实验。
QEMU `virt` 提供的 UART→PLIC→M-mode 外部中断用于验证真实 claim/ack/complete；
APLIC/IMSIC 不由当前机器模型提供，因此用架构字段、资源分配和 indirect window
协议进行可测试建模，不伪装成真实 AIA 硬件执行。

## 基础设施与实验边界

环境为 RV64IMAC + Zicsr + Zifencei、LP64、M-mode、QEMU `virt` 单 hart。基础设施
提供 reset、完整 interrupt frame、UART、PLIC MMIO 地址、真实 UART THRE interrupt、
三层测试、超时诊断、ELF W^X 和反汇编报告。

完整测试配置 UART source 10 的 priority、M-context enable 和 threshold，打开 MEIE/MIE，
再由 UART THRE 触发外部中断。Trap handler 调用你实现的 service routine：读取 claim、
先关闭 UART IER 使 level condition 消失、建立 I/O ordering、最后把同一 ID 写回 complete。

## 需要你完成

只修改 `exercises/*.S`。

1. `lab_plic_context_lookup(hartid, privilege, table, count)`
   - 从 firmware 构造的稀疏表查找 M/S context；
   - 不得使用 `2 * hartid + privilege` 等硬编码；
   - 未知 hart、缺失 privilege context 或非法参数返回 `UINT64_MAX`。

2. `lab_plic_select(priorities, pending, enabled, threshold, source_count)`
   - source ID 从 1 开始，priority 0 永不投递；
   - 候选必须同时 pending、enabled 且 priority 严格大于 threshold；
   - 最高数值 priority 胜出，相同 priority 选择更小 source ID；
   - 本模型支持最多 63 个 source，非法参数返回 `UINT64_MAX`，无候选返回 0。

3. `lab_plic_service(claim_complete, device_ack, ack_value, max_source)`
   - claim read 仅执行一次；ID 0 直接返回；
   - 拒绝超出范围的 ID；
   - 对有效 ID 先写 device acknowledge/deassert，执行 I/O fence 和 readback；
   - 最后向同一 context 写 claim ID 完成中断，并返回 ID。

4. `lab_plic_retarget(old_enable, new_enable, source, active_handlers)`
   - 仅允许迁移 1～63 的 source，两个 mask 必须独立且 8-byte 对齐；
   - active handler 非零时拒绝迁移；
   - 先从旧 context 清 enable，建立 ordering，再在新 context 设置该位；
   - 这是 serialized control-plane 模型，不代替真实驱动的 handler synchronization。

5. `lab_aplic_direct_target(hart, priority, hart_count, priority_max)`
   - 验证 hart index 和非零 priority；
   - 按 `Hart Index[31:18] | IPRIO[7:0]` 编码 direct-delivery target。

6. `lab_aplic_msi_target(hart, guest, eiid, hart_count, guest_files, identities)`
   - 验证 hart、guest index、非零 EIID 和实现资源上限；
   - 按 `Hart Index[31:18] | Guest Index[17:12] | EIID[10:0]` 编码；
   - bit 11 必须保持为保留零。

7. `lab_imsic_guest_allocate(bitmap, guest_files, preferred)`
   - guest file index 1～63，index 0 保留给 supervisor interrupt file；
   - 优先原子 claim `preferred`，不可用时从低到高选择空闲 file；
   - 使用 LR/SC 防止并发重复分配，耗尽或参数非法返回 `UINT64_MAX`。

8. `lab_indirect_window_access(selector, data, index, value, operation)`
   - 校验两个 32-bit register 地址和 read/write operation；
   - 保存并临时关闭本 hart MIE，保存旧 selector；
   - 完成 selector+data 窗口访问后恢复 selector 和原 MIE；
   - read 返回读取值，write 返回写入值，非法参数返回 `UINT64_MAX`。

## 使用方法

```sh
cd labs/lab13-plic-aia-interrupt-routing
./build.sh toolcheck
./build.sh smoke       # PLIC MMIO 和栈基础设施，初始 PASS
./build.sh unit        # discovery/arbitration/APLIC/IMSIC，初始 FAIL
./build.sh test        # 真实 UART→PLIC claim/complete，初始 FAIL
./build.sh inspect
```

## 自动测试覆盖

- 稀疏 hart 与缺失 S-context 的 firmware discovery；
- pending/enable/priority/threshold 组合和同优先级 tie-break；
- ID 0、非法 ID、device ack-before-complete 和同 context completion；
- affinity migration 的 quiescent 条件和 enable mask 转移；
- APLIC direct/MSI target 的字段边界与 reserved bit；
- IMSIC guest file 的 preferred/fallback/耗尽和原子占用；
- indirect selector 的保存恢复、read/write 和 critical section；
- QEMU 上真实 UART level interrupt 的一次 claim、deassert、complete；
- ELF W^X、必需符号、relocation 和反汇编报告。

## 必须人工完成的证明

1. PLIC hart 与 context 为什么不是一一对应，以及 DT `interrupts-extended` 的解析结果；
2. gateway、pending、enable、priority、threshold、delegation、local IE、global IE 的逐门诊断；
3. level source 在 complete 前未清 device condition 为什么形成 interrupt storm；
4. edge source 为什么还需要 device FIFO/status drain，PLIC pending 不能充当无界计数器；
5. nested IRQ 如何保存 threshold，并避免同 source 和不可重入 handler 重入；
6. affinity migration、CPU offline 和 active claim 的同步顺序；
7. APLIC direct delivery 与 MSI delivery 的 source mode、domain ownership 和 target 差异；
8. IMSIC pending/enable/delivery/threshold/top identity 与 PLIC claim 模型的差异；
9. `xiselect/xireg` 为什么是 same-hart 临界区，以及 Smstateen 的权限影响；
10. MSI 只表示通知，为什么不能替代 coherent/DMA/IOMMU payload ordering；
11. VS guest-file overcommit、VGEIN、vCPU migration 和 software injection fallback；
12. 对 PLIC、APLIC direct、IMSIC 分别建立“只触发一次”和“中断风暴”排查树。

## 不在自动测试中的内容

- 不在缺少 AIA 设备模型的机器上执行 APLIC/IMSIC MMIO 或 indirect CSR；
- 不实现 DT parser、S-mode delegation、nested IRQ、CPU hotplug 或 suspend/wakeup；
- 不模拟 PCIe MSI、IOMMU remapping、non-coherent DMA 或 guest interrupt injection；
- UART THRE 仅作为可控 level source，不代表所有设备的 acknowledge 方式；
- affinity mask 使用 64-bit RAM 模型，真实 PLIC enable bank 仍需按 source word 编址。

架构字段与保留位以 [RISC-V Advanced Interrupt Architecture specification](https://docs.riscv.org/reference/hardware/aia/_attachments/riscv-interrupts.pdf)
为准。
