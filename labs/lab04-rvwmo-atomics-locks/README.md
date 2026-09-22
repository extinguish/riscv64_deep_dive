# Lab 04：RVWMO、原子操作与可扩展锁

本实验把 Q009～Q012、Q061～Q086、Q097～Q105 中适合在当前环境实作的内容
组织为一个 4-hart 并发实验。目标是同时训练原子性、ordering、锁的 API 契约和
多 hart 验证方法。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64；
- 运行模式：M-mode、Bare address translation；
- 平台：QEMU `virt`，完整压力测试使用 4 个 hart；
- 所有共享对象位于 coherent normal RAM；
- 不把 MMIO、non-coherent DMA 或 Ztso 当作本实验的默认条件。

QEMU 的执行结果只能发现一部分功能和竞争错误。即使压力测试通过，也不能证明
一个 RVWMO outcome 被架构禁止；ordering 仍须通过 happens-before/PPO 推理、
反汇编和必要时的 herd/model 工具验证。

## 基础设施与学员边界

### 基础设施已经提供

- 4-hart Reset、BSS 初始化、per-hart stack 和启动 release/acquire；
- 独立的 infrastructure barrier，不依赖学员实现；
- 单 hart API unit tests；
- 4-hart 原子计数、spinlock/ticket-lock 临界区、bit operation 和 mailbox stress；
- bounded mailbox timeout，错误实现不会无限卡死；
- UART、QEMU finisher、超时诊断；
- ELF、relocation、attributes 和带源码反汇编报告。

基础设施的 AMO 只用于启动、测试 barrier 和错误汇总，不会代替需要学员实现的
原语。

### 需要你完成

只修改 `exercises/*.S`。

1. `lab_atomic_fetch_add(address, increment)`
   - 使用 `lr.d/sc.d` constrained retry loop；
   - 返回旧值，内存写入旧值加 increment；
   - 本实验要求 `.aqrl` 语义；
   - SC 失败只能重试，不能推断一定有其他 hart 写入。

2. Spinlock
   - `lab_spin_trylock(lock)`：成功返回 1，失败返回 0；
   - `lab_spin_lock(lock)`：允许 relaxed polling，但成功路径必须 acquire；
   - `lab_spin_unlock(lock)`：必须 release；
   - 只使用 32 位、自然对齐的 lock word；
   - 不得把普通 load/store read-modify-write 当作原子操作。

3. Ticket lock
   - `lab_ticket_lock(lock)`：原子获取 `next` ticket，等待 `owner`；
   - `lab_ticket_unlock(lock)`：release 发布下一个 owner；
   - 明确 32 位 wraparound 和活跃 waiter 数小于计数空间的假设。

4. Message publication
   - `lab_publish_message(mailbox, value)`：先写 data，再 release 发布 ready；
   - `lab_consume_message(mailbox, output)`：ready 未发布返回 0；
   - 观察到 ready 后执行 acquire，再读取 data、写入 output 并返回 1；
   - `volatile` 不是硬件或语言层同步的替代品。

5. `lab_atomic_test_and_set_bit(word, bit_index)`
   - 使用单条 64 位 AMO 设置 bit；
   - 返回设置前该 bit 的 0/1；
   - 本实验输入保证 `bit_index < 64`；
   - 使用 `.aqrl`，并注意 AMO 返回的是旧的完整 word。

## 使用方法

```sh
cd labs/lab04-rvwmo-atomics-locks

./build.sh toolcheck
./build.sh smoke       # 验证 4-hart 基础设施，初始应 PASS
./build.sh unit        # 单 hart API contract，初始应 FAIL
./build.sh build
./build.sh test        # 4-hart stress，初始应 FAIL，但不应超时
./build.sh inspect
./build.sh clean
```

推荐顺序：atomic fetch-add → bit operation → spinlock → ticket lock → mailbox。

## 完整测试覆盖

- 4 个 hart 各执行 256 次 LR/SC fetch-add；
- 4 个 hart 分别在 spinlock 和 ticket lock 下更新共享 counter；
- 临界区重叠探测和最终 counter 校验；
- 每个 hart 原子设置自己的 bit，并再次设置以检查旧值；
- hart0→hart1 message publication，带 bounded polling；
- 所有阶段均使用独立 infrastructure barrier 隔离。

## 必须人工完成的证明

运行测试之外，还应在 `build/lab04.disasm.txt` 中逐项标注：

1. LR/SC loop 为什么满足 constrained-loop 约束；
2. lock 成功路径的 acquire 从哪里建立，失败 polling 为什么可以更弱；
3. unlock 的 release 覆盖了哪些临界区访问；
4. ticket `next` 的 RMW 与 `owner` publication 为什么是两个不同同步点；
5. mailbox 的 data store、ready store、ready load、data load 构成什么 happens-before；
6. 哪些 fence/aq/rl 只覆盖 memory domain，不能直接套用到 MMIO doorbell。

## 实验边界

- 不测试 IRQ/preemption 与锁的组合，留给 RTOS Lab；
- 不声称 QEMU stress 可以证明 RVWMO 禁止某个结果；
- 不测试 atomicity PMA、misaligned atomics 或 MMIO AMO；
- MCS、qspinlock、RCU 和无锁回收属于后续 Lab 05。
