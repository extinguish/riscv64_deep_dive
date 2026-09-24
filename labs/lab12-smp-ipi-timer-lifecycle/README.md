# Lab 12：Per-Hart、IPI Mailbox、Timer 与 CPU Lifecycle

本实验承接 Lab02 的 early boot，把题库 Q194～Q207 扩展为一个可运行的 4-hart
运行期 SMP 实验。重点是 per-hart identity、HSM 风格状态机、coalesced IPI、SBI
hart mask、`WFI` 重查循环和绝对 deadline timer，而不是再次实现 coldboot。

## 基础设施与实验边界

环境为 RV64IMAC + Zicsr + Zifencei、LP64、M-mode、QEMU `virt -smp 4`。基础设施
提供安全的启动栈、`.bss` 单次初始化、`gp/tp`、完整 M-mode interrupt frame、UART、
QEMU finisher、CLINT MSIP/MTIME/MTIMECMP 访问、内部超时、三层测试和 ELF 检查。

完整测试会真实执行以下路径：hart0 向三个 secondary 的 mailbox 合并发布两种 reason，
每个 secondary 经 MSIP 从 `WFI` 唤醒并原子取走 reason；随后 hart0 编程绝对
`mtimecmp` 接收 MTIP，最后用 panic-stop reason 让三个 secondary 退出协议参与。

启动代码为保证测试基础设施可用，会对 QEMU 的实际 hart 0～3 做有界栈选择，并由
一个内部原子 winner 清 `.bss`。这不代表产品软件可以假设 hart ID 连续；你实现的映射
和 hart-mask helper 会用稀疏、非零起始的 synthetic topology 验证。

## 需要你完成

只修改 `exercises/*.S`。

1. `lab_hartid_to_cpu(hartid, hartids, count)`
   - 在线性表中查找 physical hart ID，返回稳定 logical CPU ID；
   - 未知 ID、空表或非法指针返回 `UINT64_MAX`，不得越界访问。

2. `lab_hart_mask_chunk(hartids, count, base, width)`
   - 为 SBI `hart_mask + hart_mask_base` 形式构造一个 1～64 bit 窗口；
   - 只加入 `[base, base + width)` 中的稀疏 hart ID，避免加法 overflow；
   - 参数非法返回 `UINT64_MAX`，空 topology 返回零 mask。

3. `lab_cpu_transition(state, event)`
   - 实现 OFFLINE→START_PENDING→ONLINE 和
     ONLINE→STOP_PENDING→OFFLINE 状态机；
   - start failure 进入 FAILED，stop failure 回到 ONLINE；
   - 拒绝重复 start/stop 和所有未定义状态或事件。

4. `lab_ipi_publish(mailbox, reasons)` / `lab_ipi_consume(mailbox)`
   - producer 用带 release 语义的 atomic OR 合并 reason，并返回旧值；
   - consumer 用带 acquire 语义的 atomic exchange 一次取走并清零；
   - 校验地址对齐、空指针、零 reason 和未知 bits。

5. `lab_wait_for_bits(word, mask)`
   - 实现“acquire 检查 → `WFI` → 重查”的循环；
   - 不得把 `WFI` 当作事件计数或保证睡眠的指令；
   - 命中后返回完整 snapshot，空指针或零 mask 返回零。

6. `lab_timer_select(now, local, remote)` / `lab_timer_program(compare, deadline)`
   - 选择最早的绝对 deadline；已过期的 deadline clamp 到 `now`；
   - 两者均为 `UINT64_MAX` 时保持“无限远”；
   - RV64 compare 地址必须非空且 8-byte 对齐，写后建立 I/O ordering 并校验 readback。

## 使用方法

```sh
cd labs/lab12-smp-ipi-timer-lifecycle
./build.sh toolcheck
./build.sh smoke       # 4-hart 栈、tp、mtime 基础设施，初始 PASS
./build.sh unit        # topology/lifecycle/mailbox/timer，初始 FAIL
./build.sh test        # 真实 MSIP/WFI/MTIP/panic-stop，初始 FAIL
./build.sh inspect
```

## 自动测试覆盖

- 稀疏 hartid 映射、未知 ID 和 SBI mask 的 64-bit 分块；
- HSM 风格 start/stop pending、成功、失败和非法重复事件；
- atomic OR/exchange 的 coalescing、清取和非法 reason；
- 三个 secondary 的真实 MSIP、`WFI` 唤醒与 reason delivery；
- deadline earliest/overdue/infinite 选择和 64-bit compare 编程；
- hart0 的真实 MTIP 触发及 panic-stop 有界完成；
- per-hart stack/`tp`、ELF W^X、符号、relocation 和反汇编报告。

## 必须人工完成的证明

1. 为什么 physical hart ID 不能直接作为数组下标，以及 hotplug 后 logical ID 的稳定性；
2. HSM START_PENDING/STOP_PENDING 的 linearization point、并发错误码和 rollback；
3. mailbox publication 为什么必须先于 MSIP/MSI doorbell，接收端为何先 clear 再 exchange；
4. 新 reason 分别落在 clear 前、exchange 前后时为什么不会 lost wakeup；
5. `WFI` 可立即返回、可能 Trap，并且全局 `xIE` 不决定其全部 resume 条件的影响；
6. SBI IPI/RFENCE 的 portable completion guarantee 与回收页表所需 ack 的区别；
7. offline 前 timer、interrupt、lazy owner、TLB/RCU participation 的退出顺序；
8. `time`、`cycle`、wall clock、DT timebase-frequency 与跨 hart 同步的区别；
9. RV32 写 64-bit `mtimecmp` 为什么需要防伪中断序列；
10. Sstc、SBI TIME、CLINT/ACLINT backend 的权限、成本和 fallback 边界。

## 不在自动测试中的内容

- 不实际调用 SBI HSM/IPI/RFENCE/TIME，也不模拟 OpenSBI state machine；
- 不验证 non-coherent cluster、真实 hotplug、RCU、TLB page lifetime 或 cache maintenance；
- 不执行 RV32 split-width timer 序列，也不假装 CLINT 是架构强制设备；
- 不启用 Sstc、guest timer、suspend/resume 或 timebase 校准；
- panic-stop 仅验证通知协议，不替代 NMI、watchdog 或 M-mode fallback。
