# Keystone 架构分析记录

在 `./build.sh smoke`、`./build.sh test` 和 `./build.sh inspect` 后填写。
每一项注明 Keystone 固定版本源码的文件、函数和至少一条 QEMU/GDB 证据。

## 1. 信任边界与威胁模型

- 哪些组件属于 TCB？host OS、Linux 驱动、Eyrie runtime、eapp 分别在哪个边界内？
- PMP 能阻止哪类 host 访问？外设 DMA、缓存侧信道和调试器访问需要哪些额外条件？
- QEMU 测试密钥如何影响远程证明的安全含义？

## 2. Enclave 生命周期

沿 `examples/attestation/host/host.cpp` → SDK → Linux 驱动 →
`sm/src/sm-sbi-opensbi.c` → `sm/src/enclave.c` 记录：

| 阶段 | 发起方与 SBI FID | SM 状态变化 | PMP/寄存器变化 | 证据 |
|---|---|---|---|---|
| create | | | | |
| run | | | | |
| stop/edge call | | | | |
| resume | | | | |
| exit/destroy | | | | |

## 3. PMP 与上下文切换

- 进入 enclave 前、运行中、返回 host 后各记录一次 `pmp-dump`，写出
  SM 区域、enclave 区域、OS 区域与 untrusted shared buffer 的访问权限。
- 说明 `context_switch_to_enclave` 和 `context_switch_to_host` 中改变 PMP、
  保存/恢复 `mepc`、`mstatus`、GPR、切换 trap vector、刷新 TLB 的顺序。
- 如果发生异步中断或 edge call，哪些状态必须继续保存在 SM 中？

## 4. 证明链

- 画出 BootROM 对 SM 的测量、SM 对 enclave 的测量、报告签名和验证方
  校验的链条，标出各公钥及预期测量值来自哪里。
- 说明当前上游单机示例为何不能代表独立远程验证方：预期测量值在何处
  计算，若 host 能同时替换 eapp 文件和预期值会发生什么？
- 解释为何“签名有效”仍不足以接受报告；为何正确的测量值仍需 nonce。
- 说明 `Report::getDataSize()` 来自何处，以及为何必须先限长再验签。
- 检查上游 `examples/attestation/host/host.cpp` 对 edge-call offset 和长度的
  验证。策略函数运行之前是否仍可能发生越界读取？给出源码依据和修复方向。

## 5. 与 Lab 07、11、15 对照

- Keystone 在 Lab 07 的静态 PMP 授权之上增加了什么动态策略？
- SM SBI 与 Lab 15 的教学用 SBI provider 有何相同点和额外约束？
- Keystone 的 host/enclave 上下文切换比 Lab 11 的普通任务切换多了哪些
  安全状态？
