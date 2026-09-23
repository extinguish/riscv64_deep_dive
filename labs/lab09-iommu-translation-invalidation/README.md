# Lab 09：IOMMU 翻译、IOTLB 失效与设备直通生命周期

本实验把题库 Q141～Q145 扩展为一个可运行的软件 IOMMU protocol lab。目标是把
device/process context、IOVA→GPA→SPA、IOTLB invalidation、物理页生命周期、DMA/MSI
隔离和跨层故障归因连接成一套完整模型。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64；
- 平台：QEMU `virt`，单 hart，M-mode 裸机；
- 使用教学用两阶段 software translation model，不要求 QEMU 提供真实 IOMMU；
- 页大小固定为 4 KiB，每张测试页表覆盖 64 页的连续窗口；
- command queue、completion 和 timeout 由确定性 backend 模拟；
- device assignment 同时检查 device ID、IOMMU group、VMID、DMA aperture 与 MSI aperture。

本实验的 PTE、context 和 queue 结构是为了训练协议而定义的 lab ABI，并不是 RISC-V
IOMMU Architecture 的寄存器级兼容实现。真实驱动必须按目标 IOMMU 版本、capability、
内存数据结构与 command/fault queue 格式实现，不能直接复用这里的位布局。

## 基础设施与学员边界

### 基础设施已经提供

- M-mode reset、BSS、stack、UART 和 QEMU test finisher；
- 独立的 `smoke`、`unit`、`test` 镜像和有界超时；
- page-aligned `.io_tables` 区域和 C/assembly 共享布局断言；
- deterministic invalidation command queue、ticket、completion 和 timeout 注入；
- context/PTE/translation fault、map/unmap、assignment lifecycle 测试；
- ELF W^X、必需符号、重定位、section、segment 和反汇编报告。

### 需要你完成

只修改 `exercises/*.S`。

1. IO PTE 编码与解码
   - `lab_iopte_make(physical_page, permissions)`；
   - `lab_iopte_decode(pte, physical_page_out, permissions_out)`；
   - 只接受 page-aligned、落在 `LAB_IO_PA_MASK` 内的物理页；
   - 权限只接受 R 或 R|W，拒绝 W-only、空权限和保留位；
   - 编码失败返回 `UINT64_MAX`，解码失败返回 0。

2. Device/process context 与两阶段翻译
   - `lab_iommu_translate(contexts, count, device_id, process_id, iova,
     access, result)`；
   - 按 device ID + process ID 精确选择 context；
   - 根据 flags 执行 IOVA→GPA 和 GPA→SPA，可支持单阶段；
   - 每一级独立检查窗口、valid PTE 和 device read/write permission；
   - 成功填写 GPA/SPA；失败填写 fault address、cause、stage 和 context index。

3. Map/unmap 与 IOTLB completion
   - `lab_iommu_map(domain, iova, physical_page, permissions)`；
   - `lab_iommu_unmap(domain, iova, reclaimable_page)`；
   - 仅允许 READY domain 和窗口内、page-aligned 的 IOVA；
   - PTE store/clear 后执行 release ordering，再调用
     `lab_iommu_submit_invalidation()`；
   - 必须用有界 `lab_iommu_wait_completion()` 等待 ticket；
   - timeout 或 queue failure 将 domain 标记为 QUARANTINED；
   - unmap 只有在 completion 后才能把旧物理页写入 `reclaimable_page`，失败时必须保持 0。

4. 设备直通 policy 与生命周期
   - `lab_assignment_transition(assignment, event)` 实现：
     `DETACHED → BOUND → RUNNING → QUIESCED → RESET → DETACHED`；
   - QUIESCED 可重新 START，但 RUNNING 不允许直接 DETACH/RESET；
   - BIND 时验证 DMA/MSI aperture，DETACH 时清除全部 assignment 数据；
   - `lab_assignment_authorize(...)` 只允许 RUNNING 状态下 identity 完全匹配且不
     overflow、不越过对应 aperture 的 DMA/MSI 请求；MSI 必须是对齐的 4-byte write。

## 使用方法

```sh
cd labs/lab09-iommu-translation-invalidation

./build.sh toolcheck
./build.sh smoke       # queue/completion backend，初始应 PASS
./build.sh unit        # PTE/translate/invalidate/assignment，初始应 FAIL
./build.sh build
./build.sh test        # map→translate→unmap→fault，初始应 FAIL
./build.sh inspect
./build.sh clean
```

推荐实现顺序：PTE → translate → map/unmap → assignment policy。

## 自动测试覆盖

- PTE address、permission、valid/reserved bits 和输出指针检查；
- device/process context lookup、单/双阶段翻译和 offset 保留；
- stage-1/stage-2 address、invalid-PTE、permission fault attribution；
- map/unmap 的 PTE、invalidation ticket、completion 和 reclaim gate；
- timeout 后 domain quarantine，禁止提前归还物理页；
- assignment 合法状态迁移、错误 teardown 顺序和 detach 清理；
- device/group/VMID spoof、DMA range overflow、MSI aperture/alignment；
- ELF64/RISC-V、W^X program headers、`.io_tables` 和必需符号。

## 必须人工完成的证明

在 `build/lab09.disasm.txt` 和实验报告中说明：

1. device ID、process/PASID、device context 与 process context 如何决定 address space；
2. IOVA→GPA→SPA 的 fault 分别应归因给谁，CPU `satp` 页表为何不能被默认当成
   IOMMU 页表；
3. page-table write → release → invalidation command → completion → page reuse 的
   happens-before 链；
4. CPU `SFENCE.VMA`、`HFENCE.GVMA`、IOMMU IOTLB invalidation 和 device ATS invalidation
   分别影响哪些 translation cache；
5. timeout 后为什么必须 quarantine domain/page，而不能把未知完成状态当成功或简单回滚；
6. concurrent DMA、PRI/page request、ATS 与 unmap/page reuse 如何形成 use-after-free；
7. PCIe function reset、bus mastering quiesce、IOMMU group 和 shared device state 为什么
   都是安全直通的组成部分；
8. MSI address/data remapping 与 IMSIC guest interrupt file 隔离如何阻止伪造中断；
9. QEMU 稳定而真机失败时，怎样依次收集 cause/tval/epc、PTE、PMP/PMA、CMO、
   interconnect、IOMMU command/fault queue 和 device log；
10. 为 MMU/PMP/cache/DMA/IOMMU review 定义 proof point、negative test、timeout 和
    hotplug/heterogeneity matrix。

## 实验边界

- 不实现真实 RISC-V IOMMU register map、DDT/PDT、command queue 或 fault queue encoding；
- 不连接 PCIe endpoint，不发真实 DMA、ATS、PRI 或 MSI；
- software walker 不模拟硬件 IOTLB，trace 只证明命令和 completion protocol；
- 不实现 concurrent queue producer、interrupt-driven completion 或 domain recovery；
- assignment policy 是最小安全状态机，不替代 VFIO/IOMMUFD、AIA 或平台 reset 框架；
- Lab06 的 CPU TLB、Lab07 的 PMP 和 Lab08 的 cache/DMA ownership 仍需在真实系统中
  与本实验协议联合成立。
