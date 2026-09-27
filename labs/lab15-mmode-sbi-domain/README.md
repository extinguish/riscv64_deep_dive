# Lab 15：M-mode SBI、Domain/PMP 与 S-mode Handoff

本实验承接题库 Q271～Q320。你将在 RV64 M-mode 编写一个教学用的最小 SBI
provider：校验 S-mode 提供的物理地址、编码 PMP NAPOT 区域，并按 SBI 调用约定
处理真实的 S-mode `ecall`。基础设施负责 coldboot、完整 Trap frame、M/S 栈切换、
PMP 编程与 S-mode payload。它不是 OpenSBI，也不声称支持未实现的标准扩展。

## 基础设施与实验边界

- RV64IMAC + Zicsr + Zifencei、LP64、M-mode 起步、QEMU `virt` 单 hart。
- M firmware 位于 `0x80000000` 起始的 2 MiB；S payload 放在
  `[0x80200000, 0x80400000)`。PMP entry 0 用 TOR 拒绝 S-mode 访问前一段；
  entry 1 用 NAPOT 授权后一段的 R/W/X。M-mode 能访问两段。
- S payload 先调用 SBI Base，再调用实验专用 SBI EID。最后真实 Store
  firmware secret，必须产生 M-mode Store Access Fault；M handler 跳过该条
  已知 32-bit Store 并验证 secret 保持不变。
- 本实验的 `mepc += 4` 仅用于 `ecall` 和已知的 32-bit faulting Store；
  不得推广到任意压缩指令或异常。
- M trap handler 不支持嵌套 M-mode Trap、RNMI 或多 hart 并发。PMP 策略
  只验证一个 S-domain；生产固件还须处理 PMP grain、Smepmp、跨域设备、
  cold/warmboot、scratch 生命周期及可靠失败回退。

## 需要完成

只修改 `exercises/firmware.S`，实现三个函数：

1. `lab_domain_range(addr, size, base, end)`：要求 `addr` 与 `size` 均按 8 字节
   对齐，`size>0`，且整个物理范围处于 `[base,end)`；无越界、无加法回绕，
   合法返回 1，否则返回 0。
2. `lab_pmp_napot(base, size)`：`size` 是不小于 8 的 2 的幂，`base` 按
   `size` 对齐，区间无回绕；返回 NAPOT `pmpaddr` 编码，否则返回
   `UINT64_MAX`。实际 S payload 的 2 MiB 授权由该返回值编程。
3. `lab_sbi_dispatch(frame)`：读取 `a7=EID`、`a6=FID`、`a0=arg0`，只改写
   frame 的 `a0=error` 与 `a1=value`；保留其余 GPR。实现下表中的服务，
   未知 EID/FID 返回 `SBI_ERR_NOT_SUPPORTED=-2`。

| EID | FID | 服务与返回 |
|---|---:|---|
| `0x10`，SBI Base | 0 | 规范版本：`a0=0, a1=0x03000000`，表示 SBI v3.0 |
| `0x10` | 1 | 本实验的 implementation ID：`0x4c4142` (`LAB`)，不冒称 OpenSBI |
| `0x10` | 2 | implementation version：`1` |
| `0x10` | 3 | probe：Base 与实验 EID 返回 1，其余返回 0 |
| `0x10` | 4～6 | 分别返回 `mvendorid`、`marchid`、`mimpid` |
| `0x08000000`，实验 EID | 0 | 验证 `a0` 指向 S-owned 的 8-byte 物理 buffer；合法则读出值，无权或非法返回 `-5` |
| `0x08000000` | 1 | 实验结束请求，返回成功；由基础设施判断 payload 结果 |

实验约定错误时 `a1=0`，便于测试；[SBI 规范](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/binary-encoding.adoc)
对一般错误返回的 `a1` 并不保证具体值。实验 EID 的 FID 语义只在本 Lab 中有效。

## 使用与验收

```sh
cd labs/lab15-mmode-sbi-domain
./build.sh toolcheck
./build.sh smoke    # M→S 和 S ecall 基础设施，初始 PASS
./build.sh unit     # range/NAPOT/SBI，初始 FAIL
./build.sh test     # 真实 S-mode payload + PMP Access Fault，初始 FAIL
./build.sh inspect # ELF、段权限、反汇编和 relocation 报告
```

完整测试要求 SBI Base 的所有 FID 0～6、实验 EID 的参数校验和 ABI 保留均
正确；S payload 经 6 次 `ecall` 完成调用；PMP 必须阻止一次针对 firmware
secret 的 S-mode Store。`inspect` 和 ELF 检查关注分段、关键符号及 W^X。

## Staff 级人工证明

1. **Q271～Q279：** 定义前级到固件的 `a0/a1`、privilege、FDT、内存和
   relocation 契约；区分 FW_DYNAMIC/FW_JUMP/FW_PAYLOAD、coldboot/warmboot、
   per-hart scratch 与本实验单 hart handoff 的差距。
2. **Q280～Q283：** 将 domain region 编译为 TOR/NAPOT，讨论重叠优先级、
   PMP entry 耗尽、Smepmp/lock、跨域 hart mask、设备归属与 fail-closed。
3. **Q284～Q300：** 给出 SBI Base probe 后才使用 TIME/IPI/RFENCE/HSM、
   SRST/DBCN/PMU/CPPC/SUSP 的流程；为 STA/SSE/FWFT/DBTR/MPXY/NACL
   指定版本、实现与平台 gate，不能因规范列出扩展就假定固件支持。
4. **Q301～Q310：** 证明物理地址验证的 overflow/权限/TOCTOU 边界；
   讨论 M-mode nested Trap、锁顺序、delegation、hart/system suspend、
   resume trampoline、Cache/coherency handoff 与不可递归 panic。
5. **Q311～Q320：** 定义 secure boot/update 的信任链和 rollback、M-mode
   最小 TCB、errata/runtime alternative、RAS、真机观测与 QEMU/RTL/真机
   故障注入矩阵。

标准 Base FID 与调用约定以 [RISC-V SBI specification](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-base.adoc)
和 [Binary Encoding](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/binary-encoding.adoc)
为准。实验专用 EID 取自规范的 [experimental space](https://docs.riscv.org/reference/sbi/ext-experimental.html)。
