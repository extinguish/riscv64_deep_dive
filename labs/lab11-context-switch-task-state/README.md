# Lab 11：Context Switch、Task Bootstrap 与扩展状态

本实验把题库 Q166～Q185 扩展为一个可运行的调度边界实验，覆盖 cooperative context
switch、首次任务栈构造、task trampoline、FPU/Vector lazy-state 计划和跨 hart 迁移约束。

## 基础设施与实验边界

环境为 RV64IMAC + Zicsr + Zifencei、LP64、QEMU `virt` 单 hart M-mode 裸机。
基础设施提供 reset、UART、独立 scheduler/task stack、task probe、trampoline、退出路径、
三层测试、ELF W^X 与反汇编报告。真实切换只测试整数 cooperative boundary；不会假装
当前 QEMU 已执行 RVV/FPU 寄存器保存。

`struct lab_switch_context` 与 Lab10 的完整 `pt_regs` 有意不同：scheduler 在正常 C ABI
边界只需保存 resume PC、SP、s0～s11，并按本实验契约显式切换 tp；异步 Trap 仍必须使用
完整 frame。

## 需要你完成

只修改 `exercises/*.S`。

1. `lab_context_init(...)`
   - 校验 context、entry、exit、stack base/size 和 base+size overflow；
   - 将 stack top 向下对齐到 16 byte，并保证至少 `LAB_TASK_STACK_MIN` 可用空间；
   - 清空 context，设置 `ra=lab_task_trampoline`、SP、s0=entry、s1=argument、
     s2=exit、tp 和 READY state。

2. `lab_context_switch(outgoing, incoming)`
   - 将当前 ra/sp/s0～s11/tp 保存到 outgoing；
   - 从 incoming 恢复同一集合；
   - 用恢复后的 ra 返回，使正常任务从上次调用点继续、首次任务进入 trampoline；
   - 不复制 Trap frame，不保存 caller-saved，也不要在切换中调用 C。

3. `lab_vector_context_size(vlenb)`
   - 接受 16～65536 byte 的 2 次幂 VLENB；
   - 计算 32 个 vector register 加 5 个 64-bit metadata 字段；
   - 检查乘加与 align-up overflow，最终按 64 byte 对齐；失败返回 `UINT64_MAX`。

4. `lab_extension_switch_plan(outgoing, incoming, owner_mask)`
   - FS/VS 各使用 Off/Initial/Clean/Dirty 两位状态；
   - outgoing Dirty 产生 save action；
   - incoming Off 产生 disable action；非 Off 且硬件不属于 incoming task 时产生 restore；
   - 拒绝所有未定义 state/owner bits。

5. `lab_migration_allowed(...)`
   - required ISA 必须是目标 hart capability 的子集；
   - migrate-disabled task 必须拒绝；
   - active vector state 要求非零且完全一致的 VLENB；
   - lazy live owner 只有目标支持 owner handoff 时才允许迁移；
   - 拒绝未知 flags 和不一致的非 vector VLENB。

## 使用方法

```sh
cd labs/lab11-context-switch-task-state
./build.sh toolcheck
./build.sh smoke       # stack/trampoline 基础设施，初始 PASS
./build.sh unit        # init/vector/lazy/migration，初始 FAIL
./build.sh test        # task yield/resume/exit，初始 FAIL
./build.sh inspect
```

## 自动测试覆盖

- synthetic context 的地址、对齐、overflow、寄存器槽和状态；
- 真实 scheduler→task→scheduler→task→exit 双向切换；
- task 的 s3/s11/tp 跨 yield 保存及 task 返回值；
- VLENB 合法范围、2 次幂和精确 context size；
- FS/VS Dirty save、owner-aware restore 与 Off disable；
- ISA subset、VLEN compatibility、lazy owner handoff 和 migrate-disable；
- `.task_stack`、ELF W^X、必需符号与反汇编报告。

## 必须人工完成的证明

1. `ra` 为什么是 cooperative switch 的 resume PC，为什么无需另存 `pc`；
2. 首次 restore 如何经 trampoline 把 argument 交给 entry，并让意外 return 走 task_exit；
3. 异步 Trap frame 为什么不能缩减为 callee-saved switch frame；
4. kernel thread、用户 task、active_mm、satp/ASID、sscratch 与 tp 在切换时的差异；
5. FS/VS Dirty、lazy owner、kernel FPU/Vector 使用和跨任务数据泄漏之间的关系；
6. vector context 为什么还必须保存 vstart/vl/vtype/vcsr，并避免保存过程自破坏；
7. signal frame/ptrace 如何承载可变长度扩展状态并验证用户伪造内容；
8. trigger、PMU、shadow stack/CFI 等非 GPR 状态由谁拥有和切换；
9. 跨 hart 迁移如何联合 ISA/VLEN affinity、TLB/ASID、lazy owner、reservation、timer、
   cache/DMA ownership；
10. RTOS tick/WCET、Linux entry-exit、stack overflow 与 panic path 为什么不能只看 switch.S。

## 不在自动测试中的内容

- 不执行 FPU/RVV 指令，不构造用户 signal frame 或 debug trigger；
- 不实现抢占、中断栈、地址空间切换、TLB shootdown 或真正 SMP migration；
- 不提供 DWARF unwind/CFI 答案，但实验报告必须说明手写 switch 的 unwind 策略；
- task stack 没有 MMU guard page，真实系统仍需 guard/canary/emergency containment。
