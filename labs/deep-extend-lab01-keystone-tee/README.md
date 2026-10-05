# Deep Extend Lab 01：Keystone TEE 的隔离、上下文切换与证明

本实验研究**真实的 Keystone 软件栈**，而不是自制的 M-mode monitor。
在 RV64 QEMU `virt` 上，BootROM 启动 Keystone Security Monitor（SM），
随后启动 Linux；Linux 驱动创建 enclave，Eyrie runtime 装载 eapp，
验证方检查 enclave 报告。实验先运行上游 `hello.ke`，再完成一个
fail-closed 的远程证明判定策略，并观察 SM 的 PMP 切换。

> 该实验的 QEMU 使用 Keystone/Buildroot 构建的 `qemu-system-riscv64`。
> 本机为前 15 个 Lab 安装的 QEMU 不能自动替代它。首次构建会下载
> Buildroot 的工具链、内核、QEMU 与其他源码，耗时和磁盘占用明显高于
> 前 15 个 Lab。`build/` 全部忽略，不提交下载源码和构建产物。

## 版本与边界

- Keystone 主仓库固定为 `73aeab79b97f4cea3e418897841e5534fc978f4b`；
  子模块由该提交固定，其中 Buildroot 为 `c92cc8796ed951f0604e9272677a9cdfcd93680f`。
- 平台为 `KEYSTONE_PLATFORM=generic`、RV64、Sv39、M/S/U 特权级及
  QEMU 提供的 PMP entry；不用 Lab 14 的 H 扩展，也不把 M-mode 当作
  RISC-V 另一个“monitor mode”。
- `scripts/bootstrap.sh` 核对主仓库提交并初始化 `buildroot`、`cmocka` 子模块。
  实验包装代码只覆盖固定版本的
  `examples/attestation/host/verifier.cpp` 和同目录的策略头文件；
  `sm/`、BootROM、Linux 驱动、SDK、Eyrie 和 eapp 保持上游源码。
- **受信任方：** 设备根密钥、BootROM、SM、enclave runtime 与 eapp。
  **不受信任方：** host OS、驱动暴露的请求路径、共享缓冲区以及传递报告的 host。
  真正的远程验证方必须独立持有预期 SM/eapp 测量值、设备公钥和本次 nonce。
  本实验沿用上游单机示例，在同一客体内产生 nonce、从镜像文件计算
  预期测量值；这是 QEMU 中验证报告校验流程的测试夹具，不能抵御已控制
  客体文件及 nonce 来源的恶意 host。
- QEMU 中的 BootROM 使用公开测试密钥。本实验验证隔离配置、调用链和
  报告校验代码的行为；它不证明生产设备具备可信启动、安全密钥存储、
  物理攻击防护或侧信道防护。

Keystone 架构和调用约定见[官方 Security Monitor 文档](https://docs.keystone-enclave.org/en/latest/Security-Monitor/)
与 [SM SBI 规范](https://github.com/keystone-enclave/keystone/blob/master/sm/spec/v1.0.md)。
根密钥的测试边界见[证明文档](https://docs.keystone-enclave.org/en/latest/Keystone-Applications/Attestation.html)。

## 学员任务

只修改 [`exercises/report_policy.hpp`](exercises/report_policy.hpp)。
函数 `lab_report_accept` 必须：

1. 在调用 Keystone SDK 的 `Report::verify()` **之前**检查报告数据长度：
   长度不得超过 `max_report_data`，而且必须恰好等于非空 nonce 的字节数加
   一个 NUL 结束字节；数据指针必须非空，nonce 本身也必须能放进该上限。不得让不可信的
   `data_len` 直接进入签名验证器。固定版本的 SDK 在验签时会把
   `data_len` 用作消息长度。
2. 要求 `Report::verify(expected_enclave_hash, expected_sm_hash,
   device_public_key)` 成功。该调用同时检查两级签名与两个预期测量值。
   只打印错误、只检查签名或只比对测量值均不能通过。
3. 按长度比较报告中的 nonce 原始字节和预期 nonce，并检查末尾 NUL。
   不要对不可信报告数据使用 `strcmp`。接受返回 `true`，拒绝返回 `false`。

`infra/verifier.cpp` 将策略接入上游 `attestor.ke` 示例，并提供三个可控
反例：`LAB_FAULT=hash`、`nonce`、`signature`。这些故障只改变验证方的
预期值或收到的报告副本，不修改 SM 或 enclave。默认 TODO 返回 `false`，
因此 `unit` 和 `test` 初始应失败；`smoke` 与学员代码无关，应通过。

## 使用与验收

官方构建依赖见 [Keystone 的 Ubuntu 依赖清单](https://docs.keystone-enclave.org/en/latest/Getting-Started/QEMU-Install-Dependencies.html)。
建议先准备 `gperf`、`jq`、`texinfo`、`makeself` 等工具；Buildroot 下载还
需要网络。实验运行时应留出至少 2 GiB 给 QEMU 客体和足够的构建空间。

```sh
cd labs/deep-extend-lab01-keystone-tee
./build.sh toolcheck   # 本地基础工具检查
./build.sh bootstrap   # 下载并核对固定的 Keystone + 子模块
./build.sh unit        # 快速主机侧策略测试：TODO 初始 FAIL
./build.sh build       # Buildroot 构建固件、内核、QEMU、rootfs 与示例
./build.sh smoke       # QEMU 启动、加载驱动、运行上游 hello.ke
./build.sh test        # 单元测试 + 真实 QEMU 证明正反案例
./build.sh inspect     # SM SBI/PMP 切换源码定位与 GDB 命令
```

`smoke` 和 `test` 在 `build/logs/` 留下客体输出与 QEMU 日志。`test` 必须
先看见正常报告的 `LAB01_ATTEST=PASS`，再看到错误测量值、错误 nonce、
篡改签名数据三例均以非零状态结束，最后打印
`DEEP_EXTEND_LAB01_TEST=PASS`。单元测试还覆盖过长报告和 nonce，
并验证在这些情况下没有调用密码学验证器。完整 `test` 依赖构建镜像；
单独的 `unit` 不需要网络、QEMU 或交叉编译器。

`KEYSTONE_PORT` 可覆盖默认 SSH 转发端口 `19821`，`JOBS` 可限制
Buildroot 并行度，例如 `make JOBS=2 build`。低内存主机不要直接用
`-j$(nproc)`。`clean` 只提示清理方式，不删除大型源码和构建缓存。

制作时已验证 `toolcheck`、`bootstrap`、`inspect`、脚本语法及 verifier
适配代码的源码级编译；`unit` 在 TODO 状态下按预期失败。Buildroot
构建已经下载并编译了部分工具链，但会话中断时尚无 `images/` 产物，
所以 `smoke` 与 QEMU 端到端 `test` **尚未实测**。`build/` 中的缓存可供
再次执行 `./build.sh build` 续编。

## SM 动态分析

1. `./build.sh inspect` 定位 `sm/src/enclave.c` 的
   `context_switch_to_enclave`、`context_switch_to_host`、
   `pmp_set_keystone` 和 `osm_pmp_set`。
2. 在终端 A 进入 `build/keystone`，运行 `KEYSTONE_DEBUG=y make run`；
   在终端 B 运行 `make debug-connect`。连接后执行 `pmp-dump`，并在
   `pmp_set_keystone` 下断点。随后让客体运行 `hello.ke` 或 `attestor.ke`。
3. 记录进入 enclave 前、切换途中、返回 host 后的 PMP entry 地址、权限、
   当前 hart 和 `mstatus`。解释何时禁止 host 访问 enclave 区域、何时恢复
   host 区域，以及共享内存为何仍允许通信。
4. 沿 `sm/src/sm-sbi-opensbi.c` 跟踪 create/run/resume/stop/exit/attest
   SBI 调用。说明谁发起调用、SM 如何校验身份，以及哪个边界必须清理
   寄存器/TLB 状态。结合 BootROM、SM、eapp 的测量链解释验证方的三个
   输入：设备公钥、预期 SM/eapp 测量值和本次 nonce。

将源码位置与调试证据填写到
[`exercises/architecture.md`](exercises/architecture.md)。这些分析题需要
人工检查；自动化 `test` 只负责可执行行为，不把日志匹配当作架构证明。

GDB 可观察客体内部状态，不能把 GDB 读到的内存视为普通 host 已突破
PMP。若要额外证明真实访问被拒绝，应在 guest S-mode 内执行受控访问
测试并检查 access fault，而不是用调试器直接读物理内存。

本实验的策略检查发生在上游 host 将共享缓冲区内容解析为 `Report` 之后。
这不能替代对 edge-call offset/长度的完整校验；请在架构分析中审查该路径，
不要把本实验通过解释为上游示例已达到生产级输入安全。

## 与前置 Lab 的衔接

Lab 07 的 PMP 优先级与访问故障、Lab 15 的 M-mode SBI，以及 Lab 11 的
上下文保存，是这里阅读 Keystone SM 的基础。本 Lab 增加完整的软件栈、
动态 enclave 生命周期和远程证明链。上游源码中存在研究原型的局限，
尤其不能把 QEMU 的测试密钥或示例程序当作生产安全证明。
