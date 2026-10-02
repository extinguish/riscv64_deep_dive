# RISC-V 64 位 ISA 入门：系列目录

这套文档以 **RV64I 基础整数指令**为起点，随后进入系统开发常用的 ABI、原子操作、特权级、SBI、PMP、虚拟内存、中断、缓存维护和 H 扩展。第 01～13 篇的可运行示例主要使用 GNU RISC-V 交叉工具链与 QEMU User Mode；第 14～20 篇使用 `qemu-system-riscv64` 验证特权级行为。其中第 15 篇使用 QEMU 提供的 OpenSBI，其余裸机示例使用 `-bios none`。两种执行环境不同：汇编器能识别某条指令，并不代表 Linux 用户态一定允许执行它。

第一次动手建议先看[环境准备与 Hello World](02-wsl2-qemu-user-riscv64.md)，再按下表学习。已有工具链的读者也可以直接从第 01 篇开始。

| 篇章 | 主题 | 主要覆盖范围 |
|---|---|---|
| [01](01-rv64-memory-instructions.md) | 整数访存 | RV64I 加载、存储与符号扩展 |
| [02](02-wsl2-qemu-user-riscv64.md) | 实验环境 | WSL2、GNU 工具链、QEMU User Mode、GDB |
| [03](03-rv64-pc-relative-instructions.md) | PC 相对寻址 | `AUIPC`、符号地址、重定位；兼述 `LUI` |
| [04](04-rv64-shift-instructions.md) | 移位 | RV64I 移位及 W 形式 |
| [05](05-rv64-bitwise-instructions.md) | 位逻辑 | RV64I 逻辑指令；不等同于 B 扩展 |
| [06](06-rv64-arithmetic-instructions.md) | 算术 | RV64I 加减法及 W 形式；不含 M 扩展乘除法 |
| [07](07-rv64-comparison-instructions.md) | 比较 | 有符号、无符号比较 |
| [08](08-rv64-jump-and-branch-instructions.md) | 控制流 | `JAL/JALR`、条件分支、调用与返回 |
| [09](09-rv64-csr-instructions.md) | CSR | `Zicsr` 指令；Demo 还依赖 F 扩展的 `fflags` |
| [10](10-rv64-addressing-ranges.md) | 寻址范围 | 立即数、跳转、PC 相对地址及虚拟地址边界 |
| [11](11-rv64-system-and-fence-instructions.md) | 系统与屏障 | RV64I 的 `FENCE/ECALL/EBREAK`，以及 `Zifencei` 的 `FENCE.I` |
| [12](12-rv64-psabi-elf-and-calling-convention.md) | 调用约定与 ELF psABI | C/汇编混编、寄存器保存、栈、`gp/tp` 与链接放宽 |
| [13](13-rv64-atomic-and-rvwmo.md) | A 扩展与 RVWMO | LR/SC、AMO、release/acquire 与并发消息发布 |
| [14](14-rv64-privilege-trap-and-return.md) | 特权级陷入与返回 | M/S/U CSR、委托、`MRET/SRET` 和裸机 QEMU Demo |
| [15](15-rv64-sbi-and-opensbi.md) | SBI 与 OpenSBI 交接 | 启动交接、SBI ABI、扩展探测、TIME 中断与安全边界 |
| [16](16-rv64-pmp-and-tee-isolation.md) | PMP 与 TEE 物理隔离 | NAPOT/TOR、优先级、MPRV、Smepmp、访问故障 Demo |
| [17](17-rv64-sv39-and-sfence-vma.md) | Sv39 与地址转换同步 | 三级页表、PTE 权限、ASID、`SFENCE.VMA` 和远端 shootdown |
| [18](18-rv64-interrupt-timer-and-aia.md) | 中断、定时器与 AIA 边界 | pending/enable/委托、MTIP/SBI TIME/Sstc、PLIC/AIA 与异步 trap Demo |
| [19](19-rv64-cache-cmo-dma-and-code-publish.md) | 缓存维护、DMA 与代码发布 | Zicbom/Zicboz、`menvcfg`、ownership、`FENCE.I` 与 CBO Demo |
| [20](20-rv64-h-extension-and-guest-stage.md) | H 扩展与 Guest Exit | HS/VS、Sv39x4、guest-page fault、`HFENCE.GVMA` 与真实 VS Demo |
| [21](21-rv64-register-reference.md) | U/S/M 寄存器速查 | 整数/浮点/向量寄存器、常用 CSR 编号、可选扩展与访问边界 |

## ISA 与运行环境的边界

- **RV64I** 是这套文档的主体。`LUI` 在第 03、10 篇讲解；`FENCE`、`ECALL`、`EBREAK` 在第 11 篇讲解。
- **`Zicsr`** 是独立扩展，第 09 篇专门讲解。其 Demo 还用到 **F** 扩展提供的 CSR，并不表示整套文档覆盖浮点运算。
- **`Zifencei`** 定义 `FENCE.I`，它不是 RV64I 的必需指令；第 11 篇说明它与 Linux 用户态指令缓存同步的关系。
- **A 扩展与 RVWMO** 从第 13 篇开始系统讲解；第 14～20 篇进入特权架构、SBI、PMP、Sv39、中断、缓存维护和 H 扩展。第 19 篇只讨论 Zicbom/Zicboz；第 20 篇要求 H 扩展，并不表示所有 RV64 平台都有它们。**M 扩展乘除法、F/D 浮点运算、C、B、V** 暂未系统展开。文中出现的 `li`、`la`、`j`、`ret` 等伪指令由汇编器展开，不是额外的硬件指令。

各篇完整 Demo 的命令写在对应章节；基础整数篇多使用 `-march=rv64i -mabi=lp64`，涉及扩展或 C 标准库时以章节里的命令为准。Linux 用户态的 `ecall` 由 Linux ABI 解释；S-mode 请求 M-mode 服务时遵循 SBI，二者不能混用。

指令定义以 [RISC-V 非特权级规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)为准；实验中由 Linux 或 QEMU 决定的行为，另按相应运行环境解释。
