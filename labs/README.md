# RISC-V Staff Assembly Labs

这里存放从题库扩展出来的正式实验。前 15 个裸机实验都是独立工程，拥有自己的：

- `build.sh` 和 `Makefile`；
- linker script、启动代码和 QEMU 运行入口；
- 基础设施自检（`smoke`）；
- 学员任务测试（`test`）；
- 实验边界、验收条件和故障排查说明。

约定：基础设施代码位于 `infra/`，需要学员完成的代码只位于
`exercises/`。除非实验文档明确要求，不应通过修改 `infra/` 或降低测试
标准来完成实验。

深度扩展实验沿用 `infra/` 与 `exercises/` 边界，但可以依赖固定版本的
上游软件栈；构建与验收步骤以各实验 README 为准。

## 当前实验

| 编号 | 目录 | 状态 |
|---|---|---|
| Lab 01 | `lab01-asm-runtime-abi-elf/` | 基础设施已建立，练习待完成 |
| Lab 02 | `lab02-multihart-early-boot/` | 基础设施已建立，练习待完成 |
| Lab 03 | `lab03-trap-syscall-exception/` | 基础设施已建立，练习待完成 |
| Lab 04 | `lab04-rvwmo-atomics-locks/` | 基础设施已建立，练习待完成 |
| Lab 05 | `lab05-scalable-lockfree-reclamation/` | 基础设施已建立，练习待完成 |
| Lab 06 | `lab06-sv39-page-tables-tlb/` | 基础设施已建立，练习待完成 |
| Lab 07 | `lab07-pmp-isolation-access-fault/` | 基础设施已建立，练习待完成 |
| Lab 08 | `lab08-cache-dma-code-publish/` | 基础设施已建立，练习待完成 |
| Lab 09 | `lab09-iommu-translation-invalidation/` | 基础设施已建立，练习待完成 |
| Lab 10 | `lab10-production-trap-entry/` | 基础设施已建立，练习待完成 |
| Lab 11 | `lab11-context-switch-task-state/` | 基础设施已建立，练习待完成 |
| Lab 12 | `lab12-smp-ipi-timer-lifecycle/` | 基础设施已建立，练习待完成 |
| Lab 13 | `lab13-plic-aia-interrupt-routing/` | 基础设施已建立，练习待完成 |
| Lab 14 | `lab14-hypervisor-guest-stage/` | 基础设施已建立，练习待完成 |
| Lab 15 | `lab15-mmode-sbi-domain/` | 基础设施已建立，练习待完成 |

## 深度扩展实验

| 编号 | 目录 | 状态 |
|---|---|---|
| Deep Extend Lab 01 | `deep-extend-lab01-keystone-tee/` | 实验代码与学员任务已建立，证明策略练习待完成；完整 QEMU 冒烟待验收 |
