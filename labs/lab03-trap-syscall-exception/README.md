# Lab 03：M/S/U Trap、系统调用与异常恢复

本实验把 Q005～Q007、Q020、Q047～Q050、Q146～Q165、Q183～Q185 中适合
在当前 QEMU `virt` 环境落地的部分聚合成一个真实的 U→S Trap 工程。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64；
- 启动于 M-mode，基础设施建立 PMP 并委托指定异常到 S-mode；
- S-mode 使用 Bare 地址空间，本 Lab 暂不引入 Sv39 page fault；
- 单 hart，Direct `stvec`；
- U-mode `gp/tp` 被设置为测试 sentinel，Trap entry 必须先保存用户值，再为
  S-mode C dispatcher 恢复 kernel `gp`；
- 只支持 U→S Trap。S-mode 内再次 fault 会升级到 M-mode fatal path。

Sv39 fault recovery、IRQ stack、嵌套中断、抢占和多 hart trap routing 将分别在
后续 MMU、RTOS/SMP Lab 中展开。

## 基础设施与学员边界

### 基础设施已经提供

- M-mode Reset、BSS 清零、PMP、`medeleg` 和 M-mode fatal vector；
- M→S 和 S→U privilege transition；
- 独立 M stack、S trap stack 和 U stack；
- U-mode workload：两个 syscall、32 位 `ebreak`、16 位 `c.ebreak`、一个
  custom illegal instruction，以及最终结果上报；
- C trap dispatcher、trap frame layout 的编译期校验和异常计数；
- UART、QEMU finisher、超时诊断及 ELF/CFI/反汇编报告；
- 不经过真实 Trap 的 helper unit tests。

基础设施负责触发和验收异常，不提供保存/恢复现场、指令判长、syscall 实现或
illegal-instruction emulation。

### 需要你完成

只修改 `exercises/*.S`。

1. `lab_instruction_length(instruction_address)`
   - 最低两位不等于 `0b11` 时返回 2；
   - 最低两位为 `0b11` 且 bits `[4:2] != 0b111` 时返回 4；
   - 本实验不模拟 48 位以上指令，对更长编码返回 0；
   - 只能读取足够判长的第一个 halfword。

2. `lab_handle_syscall(trap_frame)`
   - syscall number 位于保存的 `a7`；
   - `LAB_SYSCALL_ADD`：把保存的 `a0+a1` 写回保存的 `a0`；
   - `LAB_SYSCALL_XOR`：把保存的 `a0^a1` 写回保存的 `a0`；
   - 已处理返回 0，未知 syscall 返回 `-1`；
   - 该 helper 不推进 `sepc`，由 dispatcher 统一处理。

3. `lab_emulate_instruction(trap_frame)`
   - faulting instruction 是 custom-0 opcode `0x0b`，`rd` 位于 `[11:7]`；
   - 对合法 custom 指令，将 `LAB_EMULATED_VALUE` 写入保存的 `rd`；
   - `rd=x0` 时丢弃结果；
   - 成功时把保存的 `sepc` 推进 4 并返回 0；
   - 非目标编码返回 `-1` 且不得改变 frame；
   - 取指要考虑 16 位对齐，不能假设 fault PC 总是 4 字节对齐。

4. `lab_s_trap_entry`
   - 第一条指令用 `sscratch` 安全交换用户 `sp` 和 S-mode trap stack；
   - 按 `lab_trap_frame.h` 保存 x1～x31、原始用户 `sp` 和四个 Trap CSR；
   - 在调用 C 前恢复 kernel `gp`，保持 `sp` 16 字节对齐；
   - 调用 `lab_s_trap_dispatch(frame)`；
   - 使用 dispatcher 可能修改后的 `sepc/sstatus/GPR` 恢复现场；
   - 重新令 `sscratch` 指向 kernel stack top，并通过 `sret` 返回 U-mode；
   - 不能假设硬件自动保存 GPR 或切换普通栈。

## 分阶段使用

```sh
cd labs/lab03-trap-syscall-exception

./build.sh toolcheck
./build.sh smoke       # 验证 M→S→U 与 UART/finisher，初始应 PASS
./build.sh unit        # 三个 helper，初始应 FAIL
./build.sh build
./build.sh test        # 真实 U→S Trap，初始显示 TODO 并 FAIL
./build.sh inspect
./build.sh clean
```

推荐先让 `unit` 全部通过，再实现 Trap Entry。这样真实 Trap 挂死时，可以把问题
限定在现场布局、栈切换或返回路径，而不是同时怀疑异常策略。

## 完整测试路径

U-mode workload 顺序为：

1. `ecall` 执行加法 syscall；
2. `ecall` 执行异或 syscall；
3. 执行一个强制 32 位编码的 `ebreak`；
4. 执行一个 16 位 `c.ebreak`；
5. 执行 custom-0 illegal instruction，将结果写入用户 `a5`；
6. 发出 finish syscall，由基础设施检查结果并退出 QEMU。

完整验收会检查 trap 次数、cause、U-mode 来源、用户栈、全部 callee-saved
sentinel、用户 `gp/tp`、16/32 位 EPC 推进、syscall 返回值和模拟指令写回。

## 思考题

1. 为什么 `sscratch` 的交换必须发生在覆盖任何用户 GPR 之前？
2. 为什么异步中断不能只按普通函数 ABI 保存 caller-saved registers？
3. 为什么 `ECALL` 可以固定推进 4，而 breakpoint 必须读取指令判长？
4. 为什么 faulting instruction 的读取本身也可能产生第二次 Trap？
5. `stval` 在 illegal instruction、breakpoint 和 access fault 中分别能否依赖？
6. 如果 Trap 来自 S-mode，当前 `sscratch` 协议会发生什么，如何扩展为可重入入口？
