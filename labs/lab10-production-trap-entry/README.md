# Lab 10：Production Trap Entry、`pt_regs` 与安全返回

本实验把题库 Q146～Q165 扩展为一个可运行的 S-mode trap-entry lab。它不再只验证
“能进入 handler”，而是要求建立从第一条指令、`sscratch` 栈切换、完整 GPR frame、
CSR snapshot、C ABI、受控 dispatch 到 `sret` 的可审查闭环。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64；
- 平台：QEMU `virt`，单 hart，M-mode 裸机启动；
- M-mode 基础设施建立 allow-all PMP，将 breakpoint 和 U-mode ecall 委托给 S-mode；
- 完整测试从 S-mode 进入 U-mode，真实触发 `ecall`、32-bit `ebreak` 并返回；
- Direct `stvec` 是学员实现；基础设施另提供一个 16-slot Vectored 几何样例供检查；
- trap stack、user stack、M-mode emergency stack 与普通启动 stack 相互独立。

## 基础设施与学员边界

### 基础设施已经提供

- M-mode reset、BSS、UART、PMP、delegation 和 QEMU test finisher；
- S-mode/U-mode 启动、U-mode register sentinel probe 和 C dispatcher；
- 与汇编 offset 一致的固定 320-byte `struct lab_pt_regs`；
- 独立 trap/user/emergency stack 及 M-mode fail-stop vector；
- Direct trap TODO 的快速失败入口，未完成时不会静默超时；
- 32-bit vectored-table 样例、ELF W^X、符号、重定位和反汇编检查；
- `smoke`、`unit`、`test` 三层验证和有界 QEMU timeout。

### 需要你完成

只修改 `exercises/*.S`。

1. `sstatus` 状态机
   - `lab_status_on_trap(status, previous_privilege)`：把 SIE 压入 SPIE，清 SIE，
     按 U/S 来源设置 SPP；非法来源返回 `UINT64_MAX`；
   - `lab_status_on_sret(status)`：SPIE 恢复到 SIE，SPIE 置 1，SPP 清 0；
   - `lab_sanitize_user_sstatus(status)`：固定返回用户前的 SPP=0、SIE=0、SPIE=1，
     并清 SUM/MXR，不破坏受控的 FS/VS 等无关字段。

2. 有界 cause decode 与 EPC 推进
   - `lab_decode_scause(scause, exception_count, interrupt_count, decoded)`；
   - 使用最高位区分 interrupt/exception，剥离 cause code 后再做无符号边界检查；
   - 不得以攻击者可控的大 cause 直接索引 dispatch table；
   - `lab_advance_epc(epc, first_halfword)` 识别 16/32-bit 指令，拒绝未支持的
     >32-bit 编码、奇地址和加法 overflow；失败返回 `UINT64_MAX`；
   - ECALL 本身没有压缩编码，正常 syscall return 应前进 4 byte。

3. 完整 S-mode trap entry/exit
   - 实现 `lab_s_trap_vector`，第一条指令用 `sscratch` bootstrap stack；
   - U→S 使用专用 trap stack，S→S/re-entry 保留 interrupted S stack；
   - 在覆盖任何 GPR 前保存 x1～x31，并把原始 x2 写入 `LAB_PT_SP`；
   - 保存 `sstatus/sepc/scause/stval/orig_a0/flags`，保持 16-byte C ABI 对齐；
   - 保存用户 gp/tp 后，在调用 C 前建立内核 `__global_pointer$` 和受控 tp；
   - 调用 `lab_s_trap_dispatch(regs)`，再按 frame 恢复 CSR 和全部 GPR；
   - 返回 U-mode 前把 trap-stack top 放回 `sscratch`，返回 S-mode 时保持
     `sscratch=0`；frame-base 和 `sp` 必须最后恢复；
   - TODO stub 应保留，直到你一次性准备好完整 entry/exit。

## 使用方法

```sh
cd labs/lab10-production-trap-entry

./build.sh toolcheck
./build.sh smoke       # linker stacks/vector geometry，初始应 PASS
./build.sh unit        # status/cause/EPC helper，初始应 FAIL
./build.sh build
./build.sh test        # 真实 U→S→U trap 往返，初始快速 FAIL
./build.sh inspect
./build.sh clean
```

推荐实现顺序：status helpers → cause/EPC decode → save frame → C call → restore/sret。

## 自动测试覆盖

- SIE/SPIE/SPP trap-entry 与 `sret` 状态转移；
- user-return 的 SPP/SIE/SPIE/SUM/MXR 清理及 FS/VS 保留；
- interrupt bit、exception/interrupt table bounds 和恶意大 cause；
- RVC 16-bit、标准 32-bit、长指令前缀、misaligned EPC 与 overflow；
- U→S 时 `sscratch` trap-stack 切换、原始 user SP 和 frame alignment；
- U-mode ecall frame 中的 ra/gp/tp/t/s/a register sentinels 与 `orig_a0`；
- dispatcher 修改 a0 后完整恢复并返回 U-mode；
- 32-bit EBREAK 长度判定、第二次 trap 和最终 exit ecall；
- M-mode emergency fail-stop path、ELF sections、W^X 和必需符号。

## 必须人工完成的证明

在 `build/lab10.disasm.txt` 和实验报告中说明：

1. U→S、S→S、S→M 与 VS→HS 时硬件实际保存哪些 CSR，为什么 GPR/普通栈不自动保存；
2. `csrrw sp, sscratch, sp` 如何在没有空闲 GPR 时保住 user SP，kernel re-entry
   又如何以 `sscratch=0` 区分；
3. 为什么 t0 必须在首次借用前保存，frame-base/sp 为什么必须最后恢复；
4. Direct dispatch 的 cause bounds 与 Vectored `BASE+4*cause` slot 几何，C 扩展为什么
   不能让 slot 偷偷变成 2 byte；
5. 异步 IRQ 不能简单套用 psABI caller-saved 规则，何时才可能使用最小 frame；
6. 当前 source mask/ack、priority threshold、per-hart nesting 和 handler reentrancy 如何
   决定何时可以重新打开 SIE；
7. syscall restart、page-fault fixup、misaligned emulation 与 breakpoint 分别怎样决定 EPC；
8. FS/VS=Off 的 illegal-instruction trap、扩展不存在与 lazy allocation 应如何区分；
9. stack overflow、double trap、Smdbltrp/Smrnmi（若实现）和 emergency stack 怎样形成
   fail-stop 路径，为什么 fatal handler 不能递归打印；
10. `pt_regs` 如何同时服务 ptrace、signal、crash dump、unwind，并保持 RV32/扩展状态可演进。

## 实验边界

- 自动测试只执行 Direct mode；Vectored 表、AIA 大 cause 与跳转范围通过反汇编人工证明；
- 不打开异步中断，不实现 PLIC/IMSIC ack、nested IRQ 或 scheduler switch；
- 不模拟 page fault、misaligned load/store、FPU/Vector lazy trap 的完整副作用；
- 不实现 signal/ptrace/kprobe/RNMI/Smdbltrp，只要求形成设计与检查清单；
- 单 hart 的固定 trap stack 不等价于 SMP per-hart IRQ stack；
- Q166 起的 task context、首次启动、FPU/Vector 与迁移属于后续 Lab。
