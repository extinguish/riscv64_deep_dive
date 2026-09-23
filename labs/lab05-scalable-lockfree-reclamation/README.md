# Lab 05：可扩展同步、无锁结构与安全回收

本实验把 Q085、Q087、Q089～Q093 中适合在当前裸机环境实现的内容组织为一个
4-hart 并发工程，并把 Q096 的 qspinlock 压缩状态设计纳入人工审查。目标不是
只写出几个 CAS loop，而是同时处理 publication、局部自旋、ABA、对象生命周期、
grace period 和 progress contract。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64；
- 运行模式：M-mode、Bare address translation；
- 平台：QEMU `virt`，完整压力测试使用 4 个 hart；
- 所有共享对象位于 coherent normal RAM；
- cache-line 教学布局按 64 byte 构造，但不把它声明为平台发现结果；
- 不假设 Zacas、128-bit CAS、操作系统调度器或动态内存分配器存在。

QEMU stress 可以发现常见的原子性、publication 和生命周期错误，但不能证明算法
在所有 RVWMO 实现上正确，也不能证明 lock-free/wait-free progress。最终验收必须
结合反汇编、happens-before 图和阻塞点分析。

## 基础设施与学员边界

### 基础设施已经提供

- 4-hart reset、BSS 初始化、per-hart stack 和启动 release/acquire；
- 与学员算法独立的 barrier、失败位图和有界 cycle timeout；
- 单 hart API contract tests；
- 4-hart MCS 临界区、SPSC、seqlock、并发 stack 和 QSBR stress；
- 预分配且测试期间不复用的 stack node pool，避免测试框架自行引入 ABA；
- UART、QEMU finisher、ELF 检查和反汇编/section/relocation 报告。

基础设施只负责组织并发和验证结果，不会替代 `exercises/` 中的同步原语。测试
barrier 使用自己的 AMO，不能作为学员实现的间接答案。

### 需要你完成

只修改 `exercises/*.S`。

1. MCS queued lock
   - `lab_mcs_lock(lock, node)`：初始化自己的 node，原子交换 tail；
   - 有 predecessor 时先发布 `pred->next`，再只在自己的 `locked` 上自旋；
   - `lab_mcs_unlock(lock, node)`：处理“tail 已更新但 successor 尚未链接”的窗口；
   - node 在持锁和 unlock 完成前不得离开生命周期。

2. SPSC ring
   - `lab_spsc_try_push(ring, value)` 与 `lab_spsc_try_pop(ring, output)`；
   - capacity 为 8，使用单调 64-bit head/tail 和 mask 做 slot 选择；
   - producer 先写 payload，再 release 发布 head；consumer acquire head 后读取；
   - tail 的回收方向同样需要正确 ordering，full/empty 必须立即返回 0。

3. Seqlock
   - 实现 writer begin/end 和 reader begin/retry；
   - writer 进入时 sequence 变奇数，退出时 release 发布为偶数；
   - reader 不得接受奇数版本，数据读取必须位于两次 sequence 采样之间；
   - 本实验只有一个 writer；多个 writer 需要额外互斥。

4. Treiber stack 与 hazard pointer
   - `lab_stack_push` 使用 LR/SC 发布 node；
   - `lab_stack_pop_protected` 在解引用 head 前发布 hazard，并重新验证 head；
   - 实现 hazard clear 和 protection scan；
   - 成功 pop 后 hazard 仍须保持，调用者使用完 node 后才 clear；
   - stress 不复用 node，因此还必须人工解释：真实 allocator 下何时才能 free/reuse。

5. QSBR grace period
   - `lab_qsbr_start` 推进 global epoch 并返回目标 epoch；
   - `lab_qsbr_report` 只允许在该 hart 离开旧读侧临界区后报告 quiescent state；
   - `lab_qsbr_complete` 只有观察到所有参与 hart 达到目标 epoch 才返回 1；
   - 说明 stalled/offline hart、任务迁移和 trap context 如何改变这个简化协议。

## 使用方法

```sh
cd labs/lab05-scalable-lockfree-reclamation

./build.sh toolcheck
./build.sh smoke       # 只验证 4-hart 基础设施，初始应 PASS
./build.sh unit        # 单 hart API contract，初始应 FAIL
./build.sh build
./build.sh test        # 4-hart stress，初始应 FAIL，但不应超时
./build.sh inspect
./build.sh clean
```

推荐顺序：SPSC → seqlock → MCS → stack/hazard → QSBR。

## 完整测试覆盖

- 每个 hart 在自己的 MCS node 上执行 96 次临界区更新；
- hart0 向 hart1 发送 128 个严格递增的 SPSC 元素；
- 单 writer 更新 seqlock pair，reader 拒绝 torn snapshot；
- 4 个 hart 并发 push/pop 32 个唯一 node，并检查重复、遗漏和 hazard 生命周期；
- 启动 grace period，验证最后一个 hart 报告 quiescent state 前不得完成。

## 必须人工完成的证明

在 `build/lab05.disasm.txt` 和实验报告中逐项标注：

1. MCS unlock 的 CAS/link race 为什么不会丢失 successor；
2. SPSC 中哪些字段是单写者，为什么这不能直接推广到 MPSC；
3. seqlock reader 为什么可能 starvation，且为什么不能保护可立即释放的指针；
4. stack LR/SC loop 是否满足 constrained-loop 约束，hazard store→head recheck 的顺序如何建立；
5. hazard pointer 只解决生命周期安全而不自动解决所有 ABA 的原因；
6. QSBR 的 quiescent state 与 grace period 如何连接到真正的 reclaim；
7. packed qspinlock 的 mixed-size access、pending→queued 和 CPU hotplug 风险。

## 实验边界

- 不实现 Linux qspinlock、调度器、preemption 或 CPU hotplug，只做设计审查；
- 不提供通用 allocator，node 由基础设施静态预分配；
- 不声称简化 QSBR 可以直接替代 Linux RCU；
- 不测试 non-coherent shared memory、PMEM crash consistency 或 MMIO atomics；
- 进展保证必须基于完整系统假设，不能仅由一段 LR/SC loop 推出。
