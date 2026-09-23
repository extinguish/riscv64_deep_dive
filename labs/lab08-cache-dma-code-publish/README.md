# Lab 08：Cache Maintenance、DMA Ownership 与代码发布

本实验把题库 Q128～Q140 扩展为一个可运行的 cache/DMA/code-publication
协议实验。目标不是把某一颗 SoC 的 cache controller 地址写死，而是建立可迁移的
“发现粒度—规范化范围—执行维护—交接 ownership—发布代码”思维模型。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64；
- 平台：QEMU `virt`，单 hart，M-mode 裸机；
- cache block 默认测试粒度为 64 byte，但所有练习接口都显式传入粒度；
- CMO 使用确定性的 trace backend，不假定 QEMU 实现 Zicbom；
- 动态代码路径会真实执行写入 `.jit_buffer` 的指令，并要求本地 `fence.i`；
- DMA device 用 descriptor/channel 状态模型代替具体厂商设备。

cache line/CMO block 大小不是 RV64I 常量。真实系统应从平台、固件、设备树或
扩展定义获得；本实验的显式参数正是为了避免把 64 byte 偷渡成架构保证。

## 基础设施与学员边界

### 基础设施已经提供

- M-mode reset、BSS、stack、UART 和 QEMU test finisher；
- 独立的 `smoke`、`unit`、`test` 镜像和有界超时；
- cache-maintenance trace backend，可检查 operation、block address 和顺序；
- 64-byte 对齐的 DMA descriptor、channel 与测试 buffer；
- writable、non-executable ELF data segment 中的 `.jit_buffer`；
- generated-code trampoline，只有发布函数成功后才跳入 buffer；
- ELF、program-header、属性、重定位和反汇编报告。

trace backend 只验证维护命令的范围和协议顺序，不声称模拟真实 cache 数据状态。
这使测试在没有 Zicbom 的 QEMU 上仍然确定，同时把平台 CMO 指令替换点集中在
`lab_cmo_emit()`。

### 需要你完成

只修改 `exercises/*.S`。

1. CMO 范围规范化
   - `lab_normalize_cmo_range(address, size, block_size, plan)`；
   - 验证 block size 至少 4 byte 且为 2 次幂；
   - 将半开区间向下/向上扩展到完整 block；
   - 填写 start、end、blocks、head/tail partial；
   - `size == 0` 是合法空区间；拒绝地址相加和向上对齐 overflow。

2. CMO policy 与遍历
   - `lab_cmo_apply(address, size, block_size, operation, exclusive)`；
   - operation 只接受 CLEAN、INVALIDATE、FLUSH；
   - 对每个规范化后的 block 调用一次 `lab_cmo_emit()`；
   - 非 exclusive buffer 的 partial-block INVALIDATE 必须拒绝，避免丢失相邻对象的
     dirty bytes；
   - 正确传播 trace backend 容量错误。

3. DMA ownership 协议
   - `lab_dma_prepare_tx(channel, descriptor, buffer, size, block_size)`：
     clean payload，填写 device-owned descriptor，clean descriptor，执行 release
     ordering，最后写 doorbell；
   - `lab_dma_complete_rx(channel, descriptor, buffer, size, block_size)`：
     观察 completion，执行 acquire ordering，验证 CPU ownership/status/地址/长度，
     以 exclusive contract invalidate payload，最后清 completion；
   - 空指针、零长度、错误 ownership/status 必须失败，失败路径不得虚假完成交接。

4. 本地代码发布
   - `lab_publish_code(destination, source, word_count)`；
   - 拒绝空指针、非 4-byte 对齐、零长度和超过 `LAB_JIT_MAX_WORDS`；
   - 逐个复制 32-bit instruction word；
   - data writes 完成后执行合适的 ordering 和 `fence.i`；
   - `fence.i` 只保证当前 hart 的 instruction fetch 可见性。

## 使用方法

```sh
cd labs/lab08-cache-dma-code-publish

./build.sh toolcheck
./build.sh smoke       # 完全独立于 exercises，初始应 PASS
./build.sh unit        # range/CMO/DMA，初始应 FAIL 并正常退出
./build.sh build
./build.sh test        # 加入真实 generated-code 执行，初始应 FAIL
./build.sh inspect
./build.sh clean
```

推荐实现顺序：range normalize → CMO walk → DMA TX/RX → code publication。

## 自动测试覆盖

- aligned、unaligned、empty、非法粒度、加法 overflow 和 align-up overflow；
- CLEAN 的 block 遍历与 partial INVALIDATE 的 ownership 保护；
- TX payload/descriptor clean 顺序、descriptor 内容和 doorbell；
- RX completion/ownership/acquire/invalidate/acknowledge 流程；
- 两次覆盖同一 JIT buffer，分别执行并返回 42 与 99；
- ELF64/RISC-V、W^X program headers、`.jit_buffer` 和必需符号。

## 必须人工完成的证明

在 `build/lab08.disasm.txt` 和实验报告中说明：

1. 为什么范围用半开区间表达，以及 `address + size` 与 align-up 的两个 overflow；
2. clean、invalidate、flush 的数据语义分别是什么，partial invalidate 为什么危险；
3. Zicbom 的 CBO 指令、平台 cache controller 和 coherent DMA 各自改变哪一层实现；
4. 为什么 cache maintenance 不能替代 RVWMO ordering fence，反之也不成立；
5. TX 的 payload → descriptor → release → doorbell 顺序和 RX 的 completion → acquire →
   invalidate → consume 顺序；
6. 为什么 descriptor 最好独占 cache block，以及 false sharing 会怎样破坏 ownership；
7. D-cache clean 与 `fence.i` 分别解决什么问题；
8. remote hart 执行新代码时为什么仅在发布 hart 上 `fence.i` 不够，应如何 rendezvous；
9. 为什么 W^X 系统通常通过权限切换或 RW/RX alias 发布代码，而非永久 RWX；
10. VIPT/PIPT、同一 PA 的多个 VA 和不同 PBMT alias 为什么需要一致的 mapping policy；
11. RX buffer 交给设备前应怎样处置 CPU dirty bytes，pre/post invalidate 与 bounce
    buffer 各自有什么约束；
12. scatter-gather segment 存在重叠 block、零长度和不同方向时，怎样合并范围、发布
    descriptor chain，并在中途失败时回滚 ownership；
13. cache coherence、DMA address translation/IOMMU 与设备 ownership 为什么是三个不同
    问题；
14. 怎样从 DT/ACPI、firmware 或 vendor interface 发现 CMO/DMA 粒度；不同 hart 的
    block size 不一致时，系统级安全粒度、调度 affinity 与 CPU hotplug validation
    应怎样配合。

## 实验边界

- trace backend 不测真实 cache hit/miss、脏数据回写或 Zicbom trap 行为；
- 不发现实际 cache hierarchy、line size、aliasing/VIPT 约束；
- 不连接真实 DMA engine，不处理 scatter-gather、IOMMU、bounce buffer 或 interrupt；
- 单 hart 只验证 local `fence.i`；remote instruction fence 留作人工设计；
- `.jit_buffer` 位于 ELF 的 writable/non-executable segment；裸机 QEMU 没有页表执行权限
  检查，生产系统仍须落实 W^X；
- coherent DMA 可能省略 data-cache maintenance，但不能省略 ownership 和 ordering。
