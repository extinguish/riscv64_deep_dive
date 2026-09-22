# RISC-V Staff Engineer 汇编程序设计面试题库

> 面向 RISC-V SoC 公司 Staff Engineer / Senior Firmware Engineer / Bootloader / OpenSBI / TEE / RTOS / Linux BSP / Hypervisor 岗位，系统覆盖 RV32/RV64 汇编、特权架构与底层系统设计。
>
> 本题库参照《AArch64 Staff Engineer 汇编程序设计面试题库》的组织方式重新设计，但不是指令名替换版。RISC-V 没有 TF-A/EL3/PSCI 的同构软件栈，相应内容以 **M-mode + OpenSBI + SBI** 为主；GIC 内容以 **PLIC/ACLINT/AIA（APLIC + IMSIC）** 为主；EL2 内容以 **H 扩展的 HS/VS/VU 与两阶段地址翻译** 为主。
>
> 全库共 **320 道正式题**，统一按 `Q001`～`Q320` 连续编号。每题可作为场景题、代码审查题、现场 Coding、系统设计题或故障分析题使用。

---

# Part I. 题库目标与设计原则

## 1. 目标

本题库不把“会背指令”视为 Staff 水平。候选人应能：

- 独立编写并审查 Reset、Trap、Context Switch、原子操作和地址翻译关键路径；
- 区分 ISA 规范、特权规范、psABI、SBI、平台规范和具体 SoC 契约；
- 判断代码在多 hart、乱序、Cache、DMA、中断、虚拟化和安全边界下是否正确；
- 定位“Spike/QEMU 正常、FPGA/真芯片失败”以及仅在特定扩展组合上失败的问题；
- 在性能、实时性、安全性、可移植性和可维护性之间建立工程规范。

### 1.1 全篇阅读约定

- 文中出现 `H/V/AIA/Zicbom/Zalasr/Zacas/Svnapot` 等命名扩展时，均表示“目标系统明确实现该扩展”这一条件成立；若题干未明确给出，候选人应先说明发现机制、扩展版本和可靠 fallback，不能把可选扩展当作基线 ISA。
- `xstatus/xepc/xcause/xtval/xtvec/xscratch` 表示对应特权级的一族 CSR；回答具体 Trap 路径时必须落到实际的 `m*`、`s*` 或 `vs*` CSR，不能把 `x` 当作真实 CSR 名称。
- SBI 是 S/HS 与更高特权软件之间的标准接口；OpenSBI 是常见的 M-mode 实现。SBI 规范定义了某扩展，不等于任意 OpenSBI tag、平台后端或虚拟 SBI provider 均已实现它；软件必须锁定版本并使用 SBI Base probe 做能力发现。
- `hart` 指架构执行上下文，`CPU` 指 OS 管理的逻辑 CPU；涉及拓扑、hart mask、hotplug 或 per-CPU 数据时二者不得未经映射直接互换。
- 本文使用 RISC-V 术语 **VS-stage** 和 **G-stage**；“第二阶段翻译”仅作解释性描述，不使用容易带入 Arm 语义的 `Stage-2` 作为规范名称。
- 题中汇编片段、扩展清单和错误码清单均是考察入口，不替代目标产品锁定版本的规范；精确编码、WARL 行为和平台完成语义应以该版本规范、平台手册及 errata 为准。

## 2. AArch64 到 RISC-V 的能力映射

| AArch64 主题 | RISC-V 对应主题 | 不能简单类比之处 |
|---|---|---|
| A64 ISA | RV32/RV64 + 模块化扩展 | 必须先定义 ISA 字符串、扩展版本和 profile，不能假定 `G`、`V` 或 `H` 均存在 |
| AAPCS64 | RISC-V ELF psABI | `gp/tp`、linker relaxation、soft/hard-float ABI 和 vector calling-convention variant 是重点 |
| EL0/EL1/EL2/EL3 | U/S/M + HS/VS/VU | H 扩展增加虚拟化状态 `V`，并非简单增加一个“EL2”数值等级 |
| VBAR + 固定向量槽 | `mtvec/stvec/vstvec` | Direct/Vectored 语义不同；异常总到 BASE，Vectored 只对中断按 cause 偏移 |
| ESR/FAR/ELR/SPSR | `xcause/xtval/xepc/xstatus` | `xtval` 的有效内容依异常类型和实现；硬件同样不自动保存 GPR |
| DMB/DSB/ISB | RVWMO `FENCE`、aq/rl 与指令流同步机制 | 不存在逐条同构映射：`FENCE.I` 只解决当前 hart 的数据写入到后续取指同步，不是通用 ISB；设备排序与完成还取决于 PMA、平台和 I/O domain |
| LDXR/STXR + LSE | LR/SC + AMO + Zacas | reservation set、允许的 SC 伪失败和 Zalrsc forward-progress 约束必须掌握 |
| TTBR/TCR/MAIR + TLBI | `satp/vsatp/hgatp` + PTE + `SFENCE.VMA/HFENCE.*` | 内存类型主要来自 PMA/PBMT；ASID/VMID 作用域及 fence 语义不同 |
| Arm cache maintenance | Zicbom/Zicboz/Zicbop + 平台接口 | 基础 ISA 不保证软件可见的 Cache 操作；必须先发现 CMO 扩展和 block size |
| GIC | PLIC/CLINT/ACLINT/AIA | PLIC claim/complete 与 APLIC/IMSIC 的中断文件模型差异很大 |
| PSCI/SMCCC/TF-A | SBI/OpenSBI | `ecall` 使用 EID/FID；HSM、IPI、RFENCE、SRST、PMU 等扩展替代不同职责 |
| TrustZone World Switch | PMP/Smepmp、OpenSBI domain、TEE/CoVE 等 | RISC-V 基线没有统一“两世界”模型，不应虚构一一对应关系 |

## 3. Staff Engineer 能力域

### 3.1 ISA、编码与扩展模型

应覆盖 RV32I/RV64I、立即数拼接、符号扩展、`AUIPC`、`JAL/JALR`、C 扩展、M/A/B/V 扩展、Zicond、Zalasr、Zacas、Zifencei、Zicsr，以及扩展发现、版本依赖和 illegal-instruction fallback。

### 3.2 psABI、ELF、重定位与 relaxation

应覆盖 `ra/sp/gp/tp`、`a0-a7`、`s0-s11`、栈对齐、soft/hard-float ABI、Vector ABI、`R_RISCV_PCREL_HI20/LO12`、CALL/CALL_PLT、GOT/TLS、code model、链接松弛和 `.option norelax`。

### 3.3 Trap、RTOS 与 Linux arch 设计

应覆盖 `xstatus/xepc/xcause/xtval/xtvec/xscratch`、委托、嵌套 Trap、用户/内核栈切换、`pt_regs`、`switch_to`、首次任务启动、signal、FPU/Vector lazy state、Linux entry/exit 与 RTOS 确定性取舍。

### 3.4 RVWMO、原子操作与锁

应覆盖 PPO、RISC-V multi-copy atomic 内存模型与全局内存顺序、`FENCE` predecessor/successor、aq/rl、LR/SC、AMO、Zalasr、Ztso、Zacas、锁公平性、无锁结构、RCU、设备 I/O 排序和 C/C++ memory model 映射。

### 3.5 MMU、PMP、Cache 与 DMA

应覆盖 Sv32/Sv39/Sv48/Sv57、`satp`、PTE A/D、ASID、`SFENCE.VMA`、PBMT、NAPOT、PMP/Smepmp、Zicbom、I/D 一致性、IOMMU、IOVA 和 non-coherent DMA ownership。

### 3.6 SMP、Timer 与中断

应覆盖 `mhartid`、hart 到 CPU 的映射、SBI HSM/IPI/RFENCE、CLINT/ACLINT、PLIC claim/complete、AIA 的 APLIC/IMSIC、Sstc、CPU hotplug、per-CPU、IPI mailbox 和 panic stop。

### 3.7 H 扩展与虚拟化

应覆盖 HS/VS/VU、`hstatus/hedeleg/hideleg/hgatp/vsatp`、G-stage、`HFENCE.*`、trap 虚拟化、timer、virtual interrupt、AIA guest interrupt file、IOMMU 直通和 nested virtualization。

### 3.8 M-mode、OpenSBI 与安全固件

应覆盖 coldboot/warmboot、scratch、domain、PMP、SBI Base/TIME/IPI/RFENCE/HSM/SRST/PMU/DBCN/SUSP/CPPC/NACL/STA/SSE/FWFT/DBTR/MPXY 等扩展、平台 hooks、FDT fixup、payload handoff、system suspend、firmware update 与 crash dump，并区分 SBI 规范能力与具体 OpenSBI/平台实现能力。

## 4. 能力分层

1. **能写：** 能完成函数、启动、Trap、切换、原子和页表关键代码。
2. **能解释：** 能说明每个 fence、CSR 和上下文状态为何存在。
3. **能审查：** 能发现 ABI、relaxation、内存序、TLB、Cache、委托和安全漏洞。
4. **能设计：** 能定义 boot-stage ABI、arch 抽象层、平台契约、验证矩阵与 review checklist。

## 5. 题库结构

| Part | 题号 | 主题 | 数量 |
|---|---:|---|---:|
| II | Q001～Q020 | 核心综合题 | 20 |
| III-A | Q021～Q060 | ISA、ABI、ELF、Inline asm、Early Boot | 40 |
| III-B | Q061～Q105 | RVWMO、原子、锁与无锁算法 | 45 |
| III-C | Q106～Q145 | MMU、PMP、Cache、DMA 与 IOMMU | 40 |
| III-D | Q146～Q185 | Trap、Context Switch、RTOS/Linux arch | 40 |
| III-E | Q186～Q230 | SMP、Timer、PLIC/ACLINT/AIA | 45 |
| III-F | Q231～Q270 | H 扩展与虚拟化 | 40 |
| III-G | Q271～Q320 | M-mode、OpenSBI、SBI、安全与电源 | 50 |

---

# Part II. 20 道核心综合题（Q001～Q020）

## Q001. 编写最小 RV64 Reset Entry

**类型：启动 / 现场 Coding**

BootROM 让多个 hart 从 `_start` 进入 M-mode，`a0=hartid`、`a1=FDT`，但 `sp/gp/tp` 未初始化，`.bss` 未清零。请实现：保存启动参数、主从 hart 分流、初始化 `gp`、按 hart 分配栈、清 `.bss`、安装 `mtvec`，主 hart 调用 `firmware_main()`，从 hart进入等待路径。

要求说明：

1. 哪些输入是架构保证，哪些只是平台/BootROM ABI；
2. 为什么初始化 `gp` 常配合 `.option push; .option norelax`；
3. `mhartid` 为什么不能直接当连续 CPU index；
4. 主 hart 清 `.bss` 时如何避免其他 hart 提前使用；
5. 发布入口前需要怎样的内存序和 I-cache 同步；
6. `mtvec.BASE/MODE` 的对齐约束；
7. 何时能安全调用遵守 psABI 的 C 函数。

**主要考察点：** Reset 契约、GPR 初始化、链接符号、并发启动、Trap 基础。

---

## Q002. `la/lla/li/AUIPC/LUI` 与地址生成

**类型：地址生成 / 重定位分析**

比较以下意图：取得 `global_data` 地址、装入其内容、构造常量 `0x123456789abcdef0`。候选人需写出使用 `la`、`lla`、`AUIPC+ADDI`、`AUIPC+LD`、`LUI+ADDI` 和多条移位/加法的方案。

核心追问：

1. `la`/`lla` 是伪指令，PIC 与非 PIC 时可能怎样展开；
2. `%pcrel_hi/%pcrel_lo` 为什么需要配对标签；
3. 低 12 位为负时，HI20 为什么要进位修正；
4. linker relaxation 能把哪些序列改短；
5. `.option norelax` 应局部用于什么场景；
6. medlow、medany 和大代码模型分别限制什么；
7. 镜像复制后 PC-relative 引用为何仍可能失效。

**主要考察点：** U/I 型立即数、PC-relative addressing、relocation、relaxation、PIC。

---

## Q003. RISC-V psABI 函数调用约定审查

**类型：ABI 代码审查**

审查一个调用子函数却未保存 `ra`，并把长期变量放在 `t0` 的手写函数；修正为规范栈帧并补充 `.cfi_*`。

核心追问：

1. `a0-a7`、`t0-t6`、`s0-s11`、`ra` 谁负责保存；
2. `gp`、`tp` 为什么是 unallocatable；
3. `sp` 在标准 ABI 下为何必须保持 16 字节对齐；
4. frame pointer 是否强制，若存在为何必须是 `s0/fp`；
5. 超过八个整型参数和大结构体怎样传递；
6. RV64 上窄于 XLEN 的整数如何扩展；
7. LP64、LP64F、LP64D 和 soft-float ABI 能否任意混链。

**主要考察点：** psABI、stack frame、unwind、ABI 混用。

---

## Q004. 编写 Early Boot `memzero`

**类型：现场 Coding**

实现 `void asm_memzero(void *start, size_t size)`，支持任意地址/长度且不越界；对齐后用 XLEN 宽 Store，尾部按字节处理。

进一步回答：

1. RV64 的 `sd` 对未对齐地址是否必然可用；
2. PMA 与 Device/MMIO 区域如何限制访问宽度和对齐；
3. 能否使用 `cbo.zero`，如何发现 Zicboz 和 block size；
4. 何时用 Vector 扩展优化，如何处理未知 VLEN；
5. 清 `.bss` 后是否需要 fence；
6. 多 hart 同时清零为什么可能破坏已写数据。

**主要考察点：** 对齐、PMA、Zicboz、Vector、启动并发。

---

## Q005. 建立 M-mode Trap Vector

**类型：Trap / 现场 Coding**

分别设计 `mtvec` Direct 和 Vectored 两种入口，覆盖异常、软件中断、定时器中断和外部中断，并跳转到公共保存现场代码。

核心追问：

1. Direct 与 Vectored 模式下异常 PC 如何计算；
2. Vectored 模式为什么只有 interrupt 按 `4*cause` 偏移；
3. C 扩展存在时 slot 仍为何按 4 字节；
4. BASE 需要满足什么对齐，哪些约束可能是实现相关；
5. 硬件是否自动切栈或保存任何 GPR；
6. 如何用 `mscratch` 与 `sp`/临时寄存器交换；
7. 未知 cause 如何安全落入 default handler。

**主要考察点：** `mtvec`、trap entry、`mscratch`、vectored dispatch。

---

## Q006. 同步 Trap 现场分析

**类型：故障分析**

系统记录：

```text
scause  = 0x000000000000000d
stval   = 0xffffffc080001003
sepc    = 0xffffffc0001022ae
sstatus = 0x0000000200040120
```

请给出从 cause、特权来源、反汇编、页表、PTE 权限、PMA 到恢复策略的完整分析流程。

核心追问：

1. `scause` 最高位和 Exception Code 如何解释；
2. `stval` 何时保存地址、指令 bits 或零；
3. `sepc` 低位与 IALIGN、C 扩展的关系；
4. 应重试、跳过 2/4 字节指令还是终止任务；
5. `sstatus.SPP/SPIE/SIE` 如何反映来源和返回状态；
6. 怎样在不再次 fault 的情况下读取故障指令。

**主要考察点：** `scause/stval/sepc/sstatus`、页故障、可恢复性、双重 Trap。

---

## Q007. 实现 S-mode Trap Entry/Exit

**类型：现场 Coding**

为 RV64 OS 编写从 U-mode 进入 S-mode 的入口：切到 kernel stack，保存 x1～x31、原用户 `sp`、`sepc/sstatus/scause/stval`，调用 `do_trap(pt_regs *)`，恢复后 `sret`。

进一步讨论：

1. 如何在没有空闲 GPR 的第一条指令安全使用 `sscratch`；
2. `x0` 是否需要保存；
3. 内核态再入 Trap 如何避免重复交换 `sp`；
4. `gp/tp` 是保存用户值还是立即切换内核值；
5. C 扩展对返回 PC 有何影响；
6. 返回 U-mode 前如何验证 `sstatus` 与 `sepc`；
7. Linux 与小型 RTOS 的 entry frame 为什么不同。

**主要考察点：** Trap frame、stack switch、嵌套、`sret`、安全返回。

---

## Q008. 实现 RTOS 上下文切换

**类型：现场 Coding**

实现 `switch_context(prev, next)`，保存/恢复 `ra`、`sp`、`s0-s11`，并设计新任务初始上下文，使其进入 `task_entry(arg)`、返回后进入 `task_exit()`。

核心追问：

1. 协作式 switch 为什么通常不保存 `a*`、`t*`；
2. 抢占发生在 Trap 中时完整现场位于哪里；
3. `gp/tp` 是否属于每任务状态取决于什么 ABI/运行时设计；
4. F/D、V、调试触发器状态采用 eager 还是 lazy save；
5. `FS/VS/XS` Dirty 状态怎样参与决策；
6. SMP 任务迁移还需同步哪些 per-hart 状态；
7. shadow call stack 或 CFI 会怎样改变切换路径。

**主要考察点：** psABI、scheduler、首次任务、扩展上下文。

---

## Q009. 使用 LR/SC 实现自旋锁

**类型：现场 Coding**

使用 `lr.w/sc.w` 实现 RV64 上的 32 位自旋锁，并写出 acquire/release 语义正确的 unlock。

核心追问：

1. `.aq`、`.rl`、`.aqrl` 分别约束什么；
2. reservation set 的大小和失效条件是否由软件精确可知；
3. `sc.w` 非零为什么不只表示“另一 hart 写过”；
4. Zalrsc constrained loop 对 forward progress 有何要求；
5. Trap、context switch、同 hart 普通 Store 会怎样影响 reservation；
6. 锁变量所在内存必须具备哪些 PMA；
7. unlock 用普通 Store + fence 还是 AMO `.rl` 更合适；
8. 高竞争下如何退避或改用 ticket/MCS lock。

**主要考察点：** A 扩展、LR/SC、aq/rl、伪失败、活锁。

---

## Q010. 使用 AMO/Zacas 改写原子操作

**类型：原子设计**

分别用 `amoswap.w.aq`、`amoadd.d` 和 Zacas 的 `amocas.*` 设计锁、计数器和 CAS 接口。

核心追问：

1. AMO 的返回值和写入值各是什么；
2. `.aqrl` 是否等同任意场景的全 `FENCE`；
3. 失败 CAS 应使用什么内存序；
4. 运行时如何发现 Zacas，工具链不认识指令时怎么办；
5. 二进制如何为 A-only 与 Zacas hart 提供 alternatives；
6. RV64 上 `amo*.w` 的返回值如何符号扩展；
7. RV64 的 `AMOCAS.Q` 如何用合法的连续寄存器对表示 compare、swap 与返回值，对齐及 atomicity PMA 有何要求；RV32 为什么没有 `AMOCAS.Q`。

**主要考察点：** AMO、Zacas、feature alternatives、原子宽度。

---

## Q011. RVWMO Producer-Consumer

**类型：内存模型推演**

Hart0 写 `data` 后发布 `ready=1`，Hart1 轮询 `ready` 后读取 `data`。变量位于 coherent normal memory。

要求回答：

1. 无同步时是否允许观察 `ready=1,data=old`；
2. Cache coherence 与 RVWMO ordering 有何区别；
3. C `volatile` 能否形成同步；
4. 如何用 `fence rw,w` / `fence r,rw` 修复；
5. 如何用 fence + 普通 Load/Store、带 aq/rl 的 AMO，或在实现 Zalasr 时用独立 load-acquire/store-release 修复；
6. 地址/数据/control dependency 可提供哪些 PPO；
7. Ztso 平台上源码是否仍应写清同步契约；
8. C11 atomics 与手写汇编怎样保持一致。

**主要考察点：** RVWMO、PPO、acquire/release、语言内存模型。

---

## Q012. MMIO 为什么不等于 `volatile`

**类型：MMIO / Barrier**

CPU 写 DMA descriptor，随后写 doorbell MMIO。请设计正确顺序并审查仅使用 `volatile` 的实现。

核心追问：

1. `FENCE` 的 `I/O/R/W` predecessor 和 successor 集合如何选择；
2. `fence w,o`、`fence rw,ow` 和 `fence iorw,iorw` 差异；
3. MMIO 地址是否一定属于 `O` domain，由什么决定；
4. posted write completion 是否由普通 `FENCE` 保证；
5. 设备 read-back 何时是平台要求；
6. PIO 与 memory-mapped I/O 的排序模型怎样区别；
7. 为什么驱动应优先使用平台提供的 `writel()`/DMA API。

**主要考察点：** I/O ordering、PMA、compiler barrier、doorbell publication。

---

## Q013. 开启 Sv39 地址翻译

**类型：现场 Coding**

当前处于 S-mode，页表已构造。写出配置 `satp`、清理旧翻译并从物理/恒等映射切换到 higher-half 的流程。

核心追问：

1. `satp.MODE/ASID/PPN` 如何编码；
2. 根页表为何按 4 KiB 对齐；
3. 写 `satp` 本身是否隐含页表写入排序或 TLB flush；
4. `SFENCE.VMA` 前页表 Store 需要什么顺序；
5. 当前 PC、栈、Trap vector 和页表访问路径必须怎样映射；
6. `FENCE.I` 与 `SFENCE.VMA` 各解决什么问题；
7. 不支持所选 MODE 时 `satp` 写入会怎样；
8. M-mode 或 TVM 设置会如何阻止 S-mode 操作。

**主要考察点：** Sv39、`satp`、higher-half、TLB、指令同步。

---

## Q014. 修改正在使用的 RISC-V PTE

**类型：页表更新协议**

把一页从 RWX 映射改为 RX，并在另一场景把 PPN/内存类型改掉。请分别设计本 hart与多 hart 更新协议。

核心追问：

1. 哪些 PTE 变化可原子更新，何时应先 invalid 再 make；
2. RISC-V 对 invalid PTE 的 reserved bits 有何要求；
3. `SFENCE.VMA va,asid` 两个操作数为零/非零分别代表什么；
4. global mapping 与 ASID-selective flush 怎样交互；
5. remote hart 如何通过 SBI RFENCE/IPI 完成 shootdown；
6. A/D 位由硬件更新与软件管理时竞态如何处理；
7. 修改可执行代码为何还需要 `FENCE.I`；
8. Svpbmt 类型变化为何比权限收紧更敏感。

**主要考察点：** PTE 原子性、`SFENCE.VMA`、shootdown、A/D、PBMT。

---

## Q015. Non-coherent DMA 与 Zicbom

**类型：Cache / DMA 设计**

为 CPU→Device、Device→CPU、双向共享三种 non-coherent DMA 场景制定 ownership 协议。

核心追问：

1. 基础 RISC-V 是否提供通用 cache maintenance 指令；
2. `cbo.clean`、`cbo.inval`、`cbo.flush` 的区别；
3. CMO block size 从哪里获得，能否假定 64 字节；
4. invalidate 未完整覆盖 block 时怎样破坏邻接数据；
5. CMO 完成与 doorbell/completion 的 fence 如何组合；
6. Zicbom 不存在时平台固件/驱动 API 如何 fallback；
7. coherent DMA 是否仍需要 ordering；
8. VA、PA、IOVA 与设备可见地址如何区分。

**主要考察点：** Zicbom、ownership、false sharing、DMA ordering、IOMMU。

---

## Q016. Secondary Hart 启动协议

**类型：SMP Bring-up**

比较自定义 spin-table 与 SBI HSM `hart_start`。主 hart 发布 `entry/opaque/stack`，从 hart等待软件中断或 platform event 后跳转。

核心追问：

1. `mhartid` 与 Linux logical CPU ID 如何映射；
2. mailbox 发布需要 release/acquire 还是 full fence；
3. `WFI` 能否作为可靠的“等事件”协议；
4. 中断全禁用时 `WFI` 的唤醒行为依赖什么；
5. HSM START_PENDING/STARTED/STOPPED 如何避免重复启动；
6. 从 hart进入 S-mode 前 OpenSBI 应建立哪些状态；
7. non-coherent bring-up 如何发布数据与代码；
8. hotplug 与 cold boot 复用入口有哪些风险。

**主要考察点：** HSM、mailbox、WFI、hart topology、warm boot。

---

## Q017. 从 M-mode 进入 S-mode

**类型：特权切换 / 现场 Coding**

设计 M-mode firmware 将 RV64 Linux 入口放到 S-mode 的流程：准备 `mstatus.MPP`、`mepc`、`medeleg/mideleg`、`mcounteren`、PMP、`satp`、`a0/a1` 后执行 `mret`。

核心追问：

1. `mret` 如何选择目标 privilege 和中断状态；
2. 未配置 PMP 时 S-mode 是否必然能访问全部 DRAM；
3. `medeleg/mideleg` 哪些 Trap 能或不能委托；
4. timer/IPI 是直接委托还是经 SBI 虚拟注入；
5. `mtvec/stvec` 分别由谁初始化；
6. MPRV、SUM、MXR 和 FS/VS 初值怎样设置；
7. Linux boot ABI 中 hartid 与 FDT 放在哪些寄存器；
8. 进入 S-mode 前如何清理敏感寄存器。

**主要考察点：** privilege transition、delegation、PMP、Linux handoff。

---

## Q018. 实现 SBI `ecall` 包装函数

**类型：SBI / Inline Assembly**

实现通用 SBI wrapper：`a7=EID`、`a6=FID`、`a0-a5=args`，返回 `struct sbiret { long error; long value; }`。

核心追问：

1. SBI v0.1 legacy 与 v0.2+ 调用约定如何区别；
2. `ecall` 从 S/HS-mode 后由谁处理；
3. `a0/a1` 返回值如何声明为 read-write output；
4. SBI 二进制约定要求被调用方保持除 `a0/a1` 外的全部 GPR；inline asm 应如何把 `a2-a7` 声明为输入，而不是按普通 C 调用约定将它们笼统列为 clobber；
5. 何时需要 `"memory"` clobber，它是否生成 CPU fence；
6. 如何通过 Base extension probe 扩展；
7. VS-mode guest 的 SBI 通常由 HS hypervisor 还是 M-mode OpenSBI 直接处理；
8. 错误码和负 C errno 为什么不能直接混用。

**主要考察点：** SBI ABI、register preservation、extended asm、trap routing、版本兼容。

---

## Q019. Flash 到 RAM 重定位与 `FENCE.I`

**类型：链接 / 运行时重定位**

镜像 XIP 于 Flash，`.data` 和 `.ramfunc` 复制到 SRAM，`.bss` 清零。请设计 linker symbols 与启动汇编。

核心追问：

1. LMA/VMA 如何表达；
2. `AUIPC` PC-relative 序列复制后是否保持指向同一对象；
3. linker relaxation 会不会改变可复制代码假设；
4. 复制完成后本 hart为何执行 `FENCE.I`；
5. `FENCE.I` 是否自动让其他 hart 的 I-cache 同步；
6. 如何通过 remote fence.i / IPI 同步其他 hart；
7. non-coherent Cache 还需哪些平台/CMO 操作；
8. 如何验证 ELF relocation 已全部静态解决且无运行时绝对引用。

**主要考察点：** linker script、relocation、relaxation、instruction-stream sync。

---

## Q020. QEMU 正常、真机 Load Access Fault

**类型：真机故障分析**

代码对地址 `0x10000003` 执行 `ld`，QEMU 返回数据，真机产生 load address misaligned 或 load access fault；改为对 `0x10000004` 执行 `lw` 后正常。

要求分析：

1. 两条指令访问宽度和自然对齐；
2. misaligned access 是必须硬件完成、允许 Trap 模拟，还是由 PMA 决定；
3. `mcause/scause` 如何区分 misaligned、access fault 和 page fault；
4. `mtval/stval` 与 `mepc/sepc` 如何定位；
5. MMIO 寄存器能否拆成多个 byte load；
6. QEMU machine model 与真实 PMA/总线 slave 为什么不同；
7. 开启 MMU 后 PBMT/PTE 权限会增加哪些可能性；
8. 若指令可能为 16 位压缩指令，异常模拟如何求下一 PC；
9. 怎样用反汇编、DT、地址图和总线 trace 收敛根因。

**主要考察点：** misaligned、PMA、MMIO、Trap diagnosis、模型差异。

---

## 核心 20 题使用建议

| 轮次 | 题目 | 形式 | 目标 |
|---|---:|---|---|
| 第一轮 | Q001～Q008 | 原理 + 写关键汇编 | 判断基本系统汇编和 ABI 能力 |
| 第二轮 | Q009～Q016 | Litmus + Coding + 推演 | 判断并发、MMU、Cache 和 SMP 能力 |
| 第三轮 | Q017～Q020 | 系统设计 + 故障定位 | 判断 Staff 级边界、规范意识和调试能力 |

---

# Part III. Staff Engineer 全局扩展题库

# Part III-A. ISA、ABI、ELF、Inline Assembly 与 Early Boot（Q021～Q060）

## Q021. RV64 的 `*W` 指令与地址截断

**类型：代码审查**

函数用 `addiw t0,a0,0` 复制 64 位物理地址，随后以 `t0` 做 MMIO 基址。分析 `ADDIW` 对 bit 31 的符号扩展、地址高位如何被破坏、为何低地址 QEMU 不暴露问题，并列出 `ADDW/SUBW/SLLIW/LD/LWU` 的宽度语义。

**主要考察点：** XLEN、32 位结果符号扩展、地址宽度、RV64 指令语义。

---

## Q022. `x0`、丢弃结果与伪指令

**类型：体系结构理解**

解释 `x0` 的读写语义，推导 `nop`、`mv`、`not`、`neg`、`seqz`、`j`、`ret` 的真实展开。追问：`jal x0,target` 是否仍产生取指异常，`lw x0,(a0)` 是否仍进行访存及触发 fault，为什么不能用写 `x0` 消除具有副作用的指令。

**主要考察点：** hardwired zero、伪指令、副作用、异常精确性。

---

## Q023. 有符号与无符号比较

**类型：代码审查 / Coding**

实现 `in_range(value,min,max)` 的有符号和无符号版本，比较 `BLT/BGE/BLTU/BGEU` 与 `SLT/SLTU`。要求覆盖负数、`INT64_MIN/MAX`、`UINT64_MAX`，并解释 `BGT/BLE` 等伪指令如何交换操作数。

**主要考察点：** signedness、分支伪指令、边界测试。

---

## Q024. 没有条件码时的溢出检测

**类型：现场 Coding**

分别实现 64 位有符号/无符号加减溢出检测。说明无符号加法如何以 `sltu` 检查 carry，有符号加法如何根据操作数和结果符号判断，为什么 RISC-V 不依赖全局 NZCV，以及无分支实现和编译器 builtin 的取舍。

**主要考察点：** 无 flags ISA、carry/overflow、布尔构造。

---

## Q025. 实现 128/256 位加减法

**类型：现场 Coding**

用 RV64 基础整数指令实现 `hi:lo` 的 128 位加减，再扩展为 limb loop。要求处理 carry/borrow、寄存器重叠、RV32 上 64 位版本和常量时间需求；讨论 Zbb 指令是否能简化实现。

**主要考察点：** multi-precision、`sltu` carry、寄存器分配、constant-time。

---

## Q026. 移位、旋转、位域与 B 扩展

**类型：指令选择**

实现字段 `[47:40]` 的无符号/有符号提取、字段插入、rotate、bit set/clear/extract。比较基础 `slli/srli/srai/and/or` 与 Zbb/Zbs 的 `rori/bset/bclr/bext`，并说明 shift amount 超宽时硬件实际采用哪些位。

**主要考察点：** 位操作、符号扩展、B 扩展、非法假设。

---

## Q027. Zicond 与无分支选择

**类型：优化 / 安全**

用普通分支、mask 技巧和 `czero.eqz/czero.nez` 实现 `select(cond,a,b)` 与 clamp。分析 Zicond 不存在时的 fallback、分支预测与数据依赖成本、编译器 if-conversion，以及“无分支”为什么不自动等于 constant-time。

**主要考察点：** Zicond、branchless、side channel、feature fallback。

---

## Q028. 12 位立即数与 `LUI+ADDI` 陷阱

**类型：编码推演**

手工构造 `0x000000001234f800`、`0xffffffff80000800` 等常量，解释 I/S 型 12 位立即数总是符号扩展、`LUI` 在 RV64 的结果如何符号扩展，以及汇编器为何必须对 HI20 做 rounding。

**主要考察点：** immediate encoding、HI20/LO12、sign extension。

---

## Q029. Load/Store Addressing Mode 审查

**类型：代码审查**

RISC-V 没有通用 pre/post-index addressing。请审查循环中“先更新指针还是先访问”的 off-by-one 错误，比较 `LB/LBU/LH/LHU/LW/LWU/LD`，并解释未对齐、跨页和 MMIO 宽度为何必须分别考虑。

**主要考察点：** base+offset、sign/zero extension、misalignment、loop invariant。

---

## Q030. 编写 `memcpy/memmove`

**类型：现场 Coding**

实现 overlap-safe 的 RV64 `memmove`：判断方向、对齐主体、处理头尾，不得越界读取。追问是否允许 speculative over-read、misaligned XLEN load 的平台可移植性、Vector 版本的 `vsetvli` loop，以及 Device Memory 为什么不能复用该实现。

**主要考察点：** 重叠、边界、PMA、Vector-length agnostic loop。

---

## Q031. 标准函数序言/尾声审查

**类型：ABI / Unwind**

审查一个 `sp` 减 24、保存 `ra/s0/s1` 后调用 C 的函数。要求修正 16 字节对齐、CFA 与 `.cfi_offset`，解释 leaf/non-leaf、tail call、shrink wrapping 和省略 frame pointer 对回溯的影响。

**主要考察点：** psABI、CFI、stack alignment、优化器行为。

---

## Q032. `gp` 与 `tp` 的真实边界

**类型：ABI / 平台设计**

解释 `gp` 的 small-data/relaxation 历史用途与 `tp` 的 TLS/线程指针用途。审查把 `gp` 当 per-CPU 基址或 Trap 临时寄存器的代码；若平台确需专用 `gp`，说明如何禁用不兼容 relaxation 并定义全工程契约。

**主要考察点：** unallocatable registers、TLS、global-pointer relaxation、平台 ABI。

---

## Q033. 复杂参数和返回值的 ABI 分类

**类型：ABI 推演**

给出标量、`__int128`、包含 padding 的结构体、2×XLEN 聚合体和大结构体，要求推演其寄存器/栈传递及 hidden return pointer。比较 RV32/RV64 与 hard-float ABI 下 flattening 规则，说明 C 与汇编声明不一致的后果。

**主要考察点：** aggregate ABI、pair alignment、indirect return、跨语言接口。

---

## Q034. 可变参数函数与 `va_list`

**类型：ABI 深挖**

实现汇编 `sum(int n,...)`，说明 variadic 参数为何遵循 integer calling convention、寄存器对齐导致的“空洞”、栈上参数起点和 RV32 的 2×XLEN 参数规则。分析 soft/hard-float 调用者混用的风险。

**主要考察点：** varargs、register save area、alignment、ABI compatibility。

---

## Q035. F/D/Q ABI 与 NaN-boxing

**类型：浮点 ABI**

比较 LP64、LP64F、LP64D、LP64Q 的参数/返回约定；解释 ABI_FLEN 与 ISA FLEN 不同的情况、`fcsr` 状态、窄浮点值的 NaN-boxing、variadic 浮点参数，以及 kernel/firmware 禁用浮点时编译选项必须怎样约束。

**主要考察点：** floating ABI、NaN-boxing、FS state、build contract。

---

## Q036. Vector Calling Convention Variant

**类型：Vector ABI**

比较标准 calling convention 与 vector variant：哪些 `v` 寄存器可传参/需保存，`vl/vtype/vstart/vxrm/vxsat` 的调用边界是什么。讨论 `riscv_vector_cc`、未知 VLEN、signal frame 和手写汇编调用普通 C 的风险。

**主要考察点：** RVV ABI、VLEN、vector state、variant CC。

---

## Q037. 从反汇编还原 PC-relative relocation

**类型：ELF 分析**

给出 `AUIPC+ADDI`、`AUIPC+LD`、`AUIPC+JALR`，要求结合 `readelf -r` 识别 `R_RISCV_PCREL_HI20/LO12_I/LO12_S/GOT_HI20/CALL_PLT`，说明 LO12 为何相对 HI20 指令位置而非目标符号独立计算。

**主要考察点：** relocation pairing、GOT、call sequence、工具链取证。

---

## Q038. 分支范围、long call 与 linker thunk

**类型：链接故障**

比较 B-type、`JAL`、`AUIPC+JALR` 和压缩跳转范围；分析链接布局变化后 relocation overflow、链接器 long-branch/thunk 能力与手写 `jal` 的风险。说明间接调用时 `JALR` 清 bit 0 的语义。

**主要考察点：** branch encoding、range、thunk、function-pointer tagging。

---

## Q039. Linker Relaxation 改写了什么

**类型：工具链 / 代码审查**

解释 call、address、`gp`、TLS 与 RVC relaxation；分析为何反汇编与 `.o` 不同、为何计算两标签距离的手写表可能被破坏，以及 boot 初始化 `gp`、runtime patch、代码复制何时必须用 `.option norelax`。

**主要考察点：** `R_RISCV_RELAX`、whole-link transformation、layout assumptions。

---

## Q040. 可复制代码不等于位置无关代码

**类型：重定位设计**

审查复制到 SRAM 的函数，其中 `AUIPC` 访问原 `.rodata`、literal table 存绝对地址、调用超出复制区域。要求定义 self-contained blob 的链接边界、内部相对引用、外部跳转表和复制后的 `FENCE.I` 协议。

**主要考察点：** PIC、blob boundary、relocation residue、code publication。

---

## Q041. ELF attributes 与 ISA 字符串

**类型：构建系统审查**

说明 `.riscv.attributes`、ELF flags 和 `-march/-mabi` 如何表达 RVC、float ABI 与扩展版本。分析不同对象文件扩展集合合并、链接成功但旧 hart illegal instruction，以及 profile、`misa` 和 device tree ISA 描述不一致时谁可信。

**主要考察点：** ISA contract、ELF metadata、deployment compatibility。

---

## Q042. medlow、medany、PIC 与 KASLR

**类型：Code Model**

比较 medlow/medany/PIC 的可寻址范围和典型序列。给出 kernel 位于高半地址、物理加载地址随机、模块远离核心映像的场景，要求选择 code model 并说明 PLT/GOT、module trampoline 和 relaxation 的影响。

**主要考察点：** code model、KASLR、modules、PC-relative range。

---

## Q043. TLS 模型与 `tp`

**类型：ELF/TLS**

解释 local-exec、initial-exec、global-dynamic/TLSDESC 的用途和典型 relocation；分析 linker relaxation 如何把 TLS 序列缩短，以及 RTOS 把 `tp` 用作 task pointer 时怎样与 libc TLS、signal handler 和用户态 ABI 共存。

**主要考察点：** TLS models、`tp`、dynamic linker、RTOS ABI。

---

## Q044. 手写汇编的 DWARF CFI

**类型：调试 / 可维护性**

为有动态栈帧和 frame pointer 的汇编函数补齐 `.cfi_startproc/.cfi_def_cfa/.cfi_offset/.cfi_restore`。讨论 Trap frame 与普通 call frame 的 unwind 差异、异常表/ORC 类机制以及 panic 时内存不可靠的 fallback unwinder。

**主要考察点：** DWARF、CFA、unwind reliability、crash dump。

---

## Q045. Inline asm constraint 审查

**类型：编译器代码审查**

审查一个输出与仍需使用的输入共用寄存器的 asm，要求解释 `=r`、`+r`、early-clobber `=&r`、matching constraint、immediate constraint 和固定 `a0-a7` register variable；验证 `-O0/-O2/LTO` 下的正确性。

**主要考察点：** extended asm、register allocation、early clobber、LTO。

---

## Q046. `volatile asm`、`memory` clobber 与 CPU `FENCE`

**类型：语义辨析**

说明 `asm volatile` 只约束删除/合并到何种程度，`"memory"` 是 compiler barrier 而非硬件 fence。要求为 MMIO、spinlock、CSR 操作、纯计算分别选择约束，并指出不必要 memory clobber 对优化的影响。

**主要考察点：** compiler ordering、CPU ordering、observable side effect。

---

## Q047. 安全封装 CSR 访问

**类型：Inline asm / CSR**

实现 CSR read/write/set/clear/swap helper，比较 CSR immediate 与 register forms。分析 CSR 名字依工具链、只读/WARL/WPRI 字段、权限 Trap、副作用读，以及怎样探测一个可能不存在的 CSR 而不让系统递归崩溃。

**主要考察点：** Zicsr、WARL/WPRI、illegal-instruction recovery、compiler constraints。

---

## Q048. Naked/Interrupt Function 的风险

**类型：编译器边界**

审查 `__attribute__((naked))` 函数中包含普通 C 语句的 Trap handler。解释编译器可能生成的临时寄存器/栈访问、instrumentation、stack protector、relaxation 与 epilogue；比较编译器 `interrupt` attribute 和纯 `.S` 入口的契约。

**主要考察点：** naked semantics、compiler-generated code、Trap safety。

---

## Q049. 16/32 位混合指令流

**类型：反汇编 / Trap**

说明 C 扩展下 IALIGN、`sepc` 对齐、如何用最低两位先区分 16 位指令与至少 32 位的指令，以及在实现 `ILEN>32` 时如何继续解码长度字段。讨论 breakpoint/illegal instruction/emulation 后推进 PC、跨页读取 32 位或更长指令、`stval` 提供 instruction bits 的可选性和动态 patch 原子性。

**主要考察点：** RVC、instruction length、fault-safe fetch、patching。

---

## Q050. Illegal Instruction Emulation

**类型：Trap 设计**

设计对缺失扩展指令的有限模拟路径：安全取指、判长、解码、读写 Trap frame 中的 GPR、推进 EPC。要求区分真正非法、虚拟指令 Trap、CSR 权限和懒加载 FPU/Vector 所触发的 Trap，并防止来自用户态的 DoS。

**主要考察点：** instruction emulation、privilege、lazy state、robustness。

---

## Q051. `misa` 探测为何不够

**类型：Feature Discovery**

解释 `misa` 能表示哪些单字母扩展、为何可能只读/不可访问/信息不完整，multi-letter Z/S 扩展如何通过 DT/ACPI、SBI、hwprobe 或平台表发现。要求设计 M-mode firmware 与 Linux 各自的探测来源优先级。

**主要考察点：** modular ISA、discovery ABI、trust source、版本信息。

---

## Q052. 异构 Hart 的 ISA 交集

**类型：系统设计**

一个 SoC 的 hart 在 VLEN、Zicbom、Zacas 和 PMU 上不一致。设计 boot CPU 与 secondary 校验、scheduler affinity、alternatives patch、迁移限制和用户 ABI；说明为何只读 boot hart 的能力可能导致 illegal instruction 或错误 cache block 步长。

**主要考察点：** heterogeneous ISA、common denominator、task migration、hotplug。

---

## Q053. Vendor/Custom Extension 边界

**类型：架构治理**

评审一个占用 custom opcode、增加 CSR 和编译器 builtin 的扩展。要求定义 discovery、context save、virtualization、debug、ELF attribute、assembler/disassembler、kernel alternative、用户 ABI 与未来标准扩展冲突策略。

**主要考察点：** extension lifecycle、ABI、virtualization、ecosystem impact。

---

## Q054. Endianness 与设备数据格式

**类型：移植性**

区分指令编码、内存数据 endian、MMIO 寄存器 endian、DMA descriptor endian 和网络字节序。分析 RISC-V psABI 当前部署契约、byte swap 的 Zbb 指令、跨 endian firmware handoff，以及为何结构体强转不是可靠的 wire-format parser。

**主要考察点：** byte order、ABI、MMIO accessor、serialization。

---

## Q055. Reset 后绝不能假设什么

**类型：启动契约**

列出 privilege mode、PMP、Cache、TLB、interrupt、reservation、FPU/Vector、`satp`、`mtvec`、`mstatus`、RAM 初始化和其他 hart 状态中，哪些由 RISC-V 架构规定、哪些由 platform/BootROM 规定。要求形成显式 entry contract。

**主要考察点：** architectural reset vs platform reset、defensive boot。

---

## Q056. Linux/RTOS Payload Handoff ABI

**类型：接口设计**

为 OpenSBI→U-Boot/Linux 和 bootloader→RTOS 定义 handoff：目标 mode、PC、hartid、FDT、memory map、interrupt state、PMP、Cache、`satp`、寄存器清理和 secondary hart 状态。说明哪些内容应由 DT/ACPI 描述而非私有寄存器参数。

**主要考察点：** boot ABI、firmware layering、discoverability、security hygiene。

---

## Q057. 多 Hart Early Stack 设计

**类型：启动代码审查**

给定稀疏 `mhartid`，审查 `sp = stack_top - mhartid*STACK_SIZE`。设计合法 hart 表映射、临时栈、边界/guard、未支持 hart 停放和 warmboot 栈；讨论在内存尚未初始化或 coherent 前如何避免共享栈。

**主要考察点：** hart mapping、stack isolation、overflow、warmboot。

---

## Q058. PIE Firmware 自重定位

**类型：系统设计 / 链接**

设计可在任意 DRAM 地址启动的 RV64 firmware：获取 runtime delta、处理 RELATIVE relocation、建立 `gp`、避免重定位前绝对引用、同步复制后的指令流。说明为什么 medany 不自动解决数据表中的绝对指针。

**主要考察点：** PIE、self-relocation、dynamic relocation、early execution constraints。

---

## Q059. `.data/.bss/.sdata/.sbss/TLS` 初始化

**类型：链接与启动**

根据 map 文件设计复制/清零顺序，解释 NOLOAD、COMMON、small data 和 TLS template；多 hart 下谁初始化全局区、每 hart TLS/`tp` 何时创建，链接脚本如何以 ASSERT 验证范围不重叠和对齐。

**主要考察点：** linker sections、C runtime、TLS、multi-hart rendezvous。

---

## Q060. Early Boot 综合 Review Checklist

**类型：Staff 系统评审**

请建立 checklist，至少覆盖：入口契约、ISA/XLEN、`gp/sp/tp`、主从分流、内存初始化、Trap vector、PMP、委托、FDT、relocation/relaxation、I/D 同步、错误输出、panic、warmboot、可观测性和真机测试。要求指出每项的 owner 与验证方法。

**主要考察点：** 规范建立、跨层接口、review/test ownership。

### 本批覆盖矩阵

| 能力 | 题目 |
|---|---|
| RV64 ISA/编码/扩展 | Q021～Q030、Q049～Q054 |
| psABI/浮点/Vector ABI | Q031～Q036 |
| ELF/重定位/relaxation | Q037～Q044、Q058～Q059 |
| Inline asm/编译器边界 | Q045～Q048 |
| Early Boot/系统评审 | Q055～Q060 |

---

# Part III-B. RVWMO、原子、锁与无锁算法（Q061～Q105）

## Q061. `volatile` 为什么不是同步机制

**类型：代码审查**

两个 hart 通过 `volatile data` 与 `volatile ready` 通信。要求分别说明 compiler access、RVWMO global memory order、cache coherence 和 C data race；给出 C11 atomic 与汇编 fence 两种修复，并解释 `READ_ONCE/WRITE_ONCE` 类宏解决和未解决的问题。

**主要考察点：** language memory model、compiler reorder、hardware reorder、data race。

---

## Q062. Message Passing Litmus Test

**类型：内存模型推演**

初始 `x=y=0`；Hart0：`x=1; y=1`，Hart1：`r0=y; r1=x`。判断 `r0=1,r1=0` 是否允许；分别用 `FENCE`、A-extension 原子与 C11 release/acquire 建立 synchronizes-with，并说明只在一端放半个同步为何不足。

**主要考察点：** publication、PPO、release/acquire pairing、litmus reasoning。

---

## Q063. Store Buffering Litmus Test

**类型：内存模型推演**

Hart0：`x=1; r0=y`，Hart1：`y=1; r1=x`。分析 `r0=r1=0` 在 RVWMO 下为何可能，`fence rw,rw` 放在哪里可禁止；进一步说明仅给 Store 加 release、给 Load 加 acquire 并不会在它们未形成 reads-from/synchronizes-with 关系时自动禁止该结果，并设计可重复的 stress test。

**主要考察点：** Store→Load ordering、synchronization absence、测试方法。

---

## Q064. Load Buffering、IRIW 与观察顺序

**类型：高级 Litmus**

推演 Load Buffering 和四 hart IRIW 测试，区分全局 store order、每 hart 的 preserved program order 与读取顺序。要求候选人不凭 x86 直觉作答，并说明怎样用 herd7/模型工具验证允许结果。

**主要考察点：** RVWMO formal reasoning、multi-hart observation、tool-assisted proof。

---

## Q065. RVWMO 的 Preserved Program Order

**类型：体系结构深挖**

要求解释 overlapping address、explicit synchronization、syntactic dependency、pipeline dependency 与 acquire/release 形成的 PPO；给一段混合 load/store/AMO 代码，要求标出哪些边必须保留、哪些可重排。

**主要考察点：** PPO、global memory order、formal model、dependency graph。

---

## Q066. `FENCE pred,succ` 精确语义

**类型：指令语义**

比较 `fence r,r`、`fence w,w`、`fence rw,rw`、`fence w,rw`。说明 predecessor/successor 集合、为什么 fence 不是“等待所有流水线清空”、同 hart 与其他 observer 的关系，以及过强 fence 对性能和实时性的影响。

**主要考察点：** FENCE set、ordering vs completion、minimal ordering。

---

## Q067. Memory 与 I/O Domain 的跨域排序

**类型：MMIO 设计**

分析 `descriptor(normal memory) → doorbell(MMIO)` 与 `status(MMIO) → buffer(memory)` 两条链。要求选择包含 R/W/I/O bits 的 fence，说明地址 PMA 决定哪个 domain、设备完成语义为何还需平台 read-back，以及 relaxed accessor/full accessor API 应如何命名。

**主要考察点：** I/O domain、PMA、posted writes、driver API。

---

## Q068. aq/rl 的精确边界

**类型：原子内存序**

解释 `amo*.aq`、`amo*.rl`、`amo*.aqrl` 与 `lr.aq/sc.rl`，并与 Zalasr 的独立 load-acquire/store-release 区分。追问：aq/rl 是否只保证原子对象本身、失败 SC 的 `.rl` 有何意义、能否跨 memory/I/O domain 排序、与 C++ relaxed/acquire/release/acq_rel/seq_cst 如何映射。

**主要考察点：** acquire/release、Zalasr、address domain、C memory order mapping。

---

## Q069. 地址、数据与控制依赖

**类型：代码审查**

给出 pointer chasing、由 load 值计算 store data、条件分支后 load 三种代码，要求判断 RVWMO 中哪些形成 syntactic dependency/PPO，编译器可能怎样消除依赖，以及 speculative execution 安全与架构 ordering 为什么是两个问题。

**主要考察点：** dependency ordering、compiler transformation、speculation barrier。

---

## Q070. Release Sequence 与跨 Hart 传递

**类型：高级同步**

三 hart 通过两个 atomic flag 传递数据。要求依据 C/C++ release sequence 与 RVWMO 生成序列判断 transitivity，区分 RCsc/RCpc 风格假设，检查中间 relaxed RMW 是否维持同步链，并以 litmus 证明结论。

**主要考察点：** transitive publication、RMW chain、language/ISA correspondence。

---

## Q071. Ztso 并不等于删除所有同步

**类型：扩展语义**

说明 Ztso 加强了哪些 load/store order、哪些 I/O、instruction stream、compiler 和原子 RMW 需求仍存在。设计一个 RVWMO 通用 binary 在 Ztso hart 上的优化策略，并讨论 runtime patch 与单一 portable code path 的取舍。

**主要考察点：** Ztso、portability、compiler barriers、feature alternatives。

---

## Q072. C11 Atomics 到 RISC-V 指令的映射审查

**类型：编译器 / ABI**

对 `atomic_load/store/fetch_add/compare_exchange` 的各 memory order 查看 GCC/Clang 输出。解释为什么不同 `-march`、扩展版本和编译器版本可能选择 fence + 普通 Load/Store、LR/SC、AMO、Zalasr 或 Zacas，手写 asm 与 LTO 混用时如何验证语义而非匹配固定指令串。

**主要考察点：** compiler lowering、memory-order contract、toolchain variation。

---

## Q073. LR/SC Reservation 的工作机制

**类型：体系结构原理**

解释 reservation set、`LR` 地址、`SC` 地址/大小要求、其他 hart store、device write、eviction 和 context event 对成功的影响。强调实现可以伪失败，软件不得从一次 SC 失败反推出具体并发事件。

**主要考察点：** reservation set、PMA、SC failure semantics。

---

## Q074. Zalrsc Constrained Loop

**类型：规范深挖 / 代码审查**

给出包含函数调用、额外 load、过多指令或后向分支的 LR/SC loop，判断是否仍属于 constrained loop。说明 eventuality guarantee 的前提、重试上限/退避策略，以及为何形式上能汇编不等于有 forward progress 保证。

**主要考察点：** constrained LR/SC、livelock、forward progress。

---

## Q075. Trap/切换后的 Reservation 清理

**类型：上下文 / 原子交叉题**

讨论 `xRET` 是否必然清 reservation、OS 切换地址空间或任务时为何通常主动用一次必失败 SC 清理、debugger 单步 LR/SC 的影响，以及 reservation 泄漏是否会造成错误成功还是主要造成可观测性/规范问题。

**主要考察点：** reservation lifetime、xRET implementation freedom、context switch。

---

## Q076. Acquire/Release Spinlock 完整实现

**类型：现场 Coding**

写出 trylock、lock、unlock，处理 LR/SC 伪失败与 busy value；要求区分 relaxed polling 和 acquire 成功路径，unlock 建立 release，禁止编译器把临界区访问移出，并给出中断/抢占封装契约。

**主要考察点：** lock ordering、compiler contract、LR/SC loop、API boundary。

---

## Q077. AMO Test-and-Set Lock

**类型：性能审查**

用 `amoswap.w.aq` 实现 TAS lock，比较直接反复 AMO 与先普通 load 自旋的 test-and-test-and-set。分析缓存行独占流量、公平性、NUMA/cluster 扩展性和 unlock waiter notification。

**主要考察点：** AMO、cache-line bouncing、contention、fairness。

---

## Q078. Zacas Compare-and-Swap

**类型：扩展原子**

实现 strong/weak CAS wrapper，说明 expected 值如何输入输出、成功/失败 ordering、alignment/PMA 和 RV32/RV64 宽度。比较 Zacas 与 LR/SC 在被抢占、竞争和 forward progress 上的工程差异。

**主要考察点：** CAS semantics、Zacas、weak/strong API、fallback。

---

## Q079. Double-width CAS 与 128 位状态

**类型：高级原子设计**

在 RV64 + Zacas 上，分析如何用 `AMOCAS.Q` 原子更新 `{pointer,counter}` 128 位状态，包括合法寄存器对、规范要求的自然对齐和 atomicity PMA；在 RV32 或缺少相应能力时，不得把任意两条 64 位操作当作原子，并须给出 lock、seqlock 或 hazard-pointer fallback。

**主要考察点：** atomic width、feature gating、ABA mitigation、fallback design。

---

## Q080. Atomic Reference Count

**类型：安全并发**

设计 `inc_not_zero()` 和 `dec_and_test()`，处理从 1 到 0 的唯一销毁者、禁止从 0 复活、整数 overflow、对象发布/析构 ordering。说明 relaxed increment 何时足够，最后 decrement 为何常需更强语义。

**主要考察点：** refcount lifecycle、overflow hardening、release/acquire on destruction。

---

## Q081. Atomic Bit Operations

**类型：现场 Coding / 代码审查**

用 `amoor/amoand/amoxor` 实现 set/clear/test-and-set bit。处理 RV32/RV64 word selection、bit index、邻位并发、返回旧值符号扩展与 ordering variants；解释普通 read-modify-write 为什么丢更新。

**主要考察点：** AMO bitops、word geometry、atomic return semantics。

---

## Q082. Lock、Interrupt 与 Preemption

**类型：系统设计**

分析 task 与同 hart IRQ、不同 hart、NMI-like critical path 共享锁的死锁组合。为 RTOS/Linux 风格设计 `spin_lock_irqsave`、preempt disable、raw spinlock，强调保存并恢复原中断状态而不是无条件 enable。

**主要考察点：** execution context、deadlock、irqsave、preemption。

---

## Q083. TAS Lock 的扩展性问题

**类型：性能分析**

在 64-hart 系统上分析 TAS lock 的 invalidate storm、unfair reacquire 和 tail latency。要求用 PMU/互连计数验证，比较 backoff、ticket、MCS 和分片数据结构，而不是用更强 fence 掩盖问题。

**主要考察点：** scalability、coherence traffic、measurement、algorithm choice。

---

## Q084. Ticket Lock

**类型：现场设计**

用两个 counter 实现 ticket lock，定义原子 fetch-add、owner polling 和 release increment。讨论 wraparound、安全的队列深度假设、公平性、远端缓存流量、等待者休眠与 16/32 位字段打包竞态。

**主要考察点：** fairness、counter wrap、AMO、wait strategy。

---

## Q085. MCS/Queued Lock

**类型：高级锁算法**

设计每 waiter node 的 MCS lock：原子交换 tail、链接 predecessor、在本地 flag 自旋、处理无 successor 的 CAS。说明 node 生命周期、每 CPU vs 栈上分配、preemption 和 unlock/link race。

**主要考察点：** scalable queued lock、publication、node ownership、race proof。

---

## Q086. False Sharing 与锁布局

**类型：性能 / 结构布局**

两个独立锁或 per-hart counter 落在同一 cache block，吞吐随 hart 数下降。要求区分 cache line 与 Zicbom block、用 padding/alignment 解决、考虑异构实现的最大 line size，并讨论过度 padding 的内存成本。

**主要考察点：** false sharing、cache geometry、data-layout tradeoff。

---

## Q087. SPSC Ring Buffer

**类型：无锁现场 Coding**

设计单生产者/单消费者 ring：先写 payload 再 release 发布 head，acquire 读取 head 后消费；tail 类似。要求处理 wrap、full/empty、非 power-of-two、cache-line separation 和 non-coherent shared memory。

**主要考察点：** SPSC publication、index arithmetic、ownership、cache layout。

---

## Q088. MPSC Queue 的发布竞态

**类型：无锁代码审查**

多个 producer 先抢 slot 后写 payload；consumer 可能看到 head 前移但 payload 未完成。要求设计 per-slot sequence/ready、证明 ordering，处理 producer 被抢占、wraparound 和 false sharing。

**主要考察点：** reservation vs publication、per-slot state、progress guarantee。

---

## Q089. ABA 问题

**类型：故障分析**

用 lock-free stack 演示指针 A→B→A 导致 CAS 误判。比较 tagged pointer、宽 CAS、hazard pointer、epoch/RCU 和 lock fallback；说明地址复用、counter wrap 与 allocator contract。

**主要考察点：** ABA、memory reclamation、atomic width、allocator interaction。

---

## Q090. Seqlock

**类型：现场设计**

写者把 sequence 变奇数、更新数据、再变偶数；读者前后采样并重试。要求给出 fence/acquire/release 位置，分析写者互斥、读者 starvation、不可复制指针和 IRQ 中写者被抢占的风险。

**主要考察点：** seqlock ordering、consistency snapshot、starvation。

---

## Q091. 无锁内存回收

**类型：系统设计**

解释为什么 CAS 正确不代表节点可立即 free。为一个 read-mostly 表选择 hazard pointers、epoch、QSBR 或 RCU；定义 quiescent state、任务迁移、hart offline、Trap context 和 stalled hart 的处理。

**主要考察点：** lifetime safety、grace period、scheduler integration。

---

## Q092. Lock-free、Wait-free 与 Obstruction-free

**类型：概念与证明**

分别给出系统级/线程级进展定义，判断 LR/SC loop、ticket lock、MCS、seqlock reader、CAS stack 属于哪类。说明硬件 SC 伪失败、OS 抢占和内存分配器会如何改变完整算法的 progress claim。

**主要考察点：** progress taxonomy、whole-system assumptions、formal proof。

---

## Q093. RCU 在 RISC-V 上的内存序

**类型：Linux arch 设计**

分析 pointer publication、dereference、grace period 和 callback；说明依赖排序与编译器变换、`rcu_assign_pointer/rcu_dereference` 的 arch 实现、Ztso 与 RVWMO 差异，以及 Alpha 经验不能机械套用。

**主要考察点：** RCU、dependency ordering、Linux memory model、arch primitives。

---

## Q094. Futex 与用户/内核原子边界

**类型：Linux/RTOS 设计**

设计用户态 compare 后进入内核 sleep、内核 wake 的协议，解释 lost wakeup 防护、用户地址 fault、atomic user access、PI futex 与任务退出清理。讨论 SBI/firmware 不应参与普通 futex fast path 的分层原因。

**主要考察点：** futex race、uaccess、scheduler、layering。

---

## Q095. 用 `WFI` 优化争用锁是否可靠

**类型：低功耗并发**

审查“锁忙就 `WFI`，unlock 只写 0”的实现。说明 `WFI` 允许作为 hint、可能立即返回、需要可达中断/通知，lost wakeup 怎样发生；设计 spin-then-park 与 IPI/doorbell/等待队列协议。

**主要考察点：** WFI semantics、wake protocol、lost wakeup、power/performance。

---

## Q096. Queued Spinlock 的压缩状态

**类型：综合算法审查**

评审把 locked/pending/tail 编码进一个 word 的 qspinlock。要求分析原子字段更新、pending→queued 转换、tail 编码与 CPU hotplug、byte/word mixed-size access，以及 debug 状态如何避免扰动时序。

**主要考察点：** compact atomic state、mixed-size race、CPU topology。

---

## Q097. 原子访问的对齐与 PMA

**类型：平台约束**

判断 aligned AMO/LRSC 在普通 DRAM、MMIO、uncached SRAM、跨 cache block、跨 page 的合法性。解释 atomicity PMA、misaligned atomicity granule（若实现）与普通 misaligned load/store 支持不能相互推出。

**主要考察点：** atomicity PMA、alignment、memory type、platform discovery。

---

## Q098. Mixed-size Atomicity

**类型：内存模型深挖**

一个 hart 用 64 位 AMO 更新 word，另一个用 8/32 位普通访问其子字段。要求分析 data race、overlapping-address ordering、是否撕裂、语言层 UB 和硬件原子宽度；给出统一访问宽度或字段分离方案。

**主要考察点：** mixed-size overlap、tearing、C model、layout discipline。

---

## Q099. Misaligned Atomicity Granule

**类型：高级架构题**

解释实现若声明 misaligned atomicity granule，哪些普通 load/store/AMO 可能得到更强原子性、LR/SC 是否包含在内、跨 granule/PMA 边界会怎样。要求软件只在可发现且契约明确时依赖该能力。

**主要考察点：** MAG PMA、misalignment、portable fallback。

---

## Q100. 对 MMIO 使用 AMO 的错误

**类型：代码审查**

驱动用 `amo.or` 设置设备寄存器 bit。分析设备区域是否支持 AMO 由何种 PMA/总线能力决定，read side effect、write-one-to-clear、访问宽度和 endian 风险；给出设备专用 set/clear register 或锁保护 shadow state 方案。

**主要考察点：** MMIO semantics、AMO-capable PMA、device side effects。

---

## Q101. DMA Descriptor 发布顺序

**类型：并发 + I/O 综合题**

CPU 填 buffer、descriptor、ownership bit，再敲 doorbell；设备写 completion 后 CPU 读数据。要求画出 memory/cache maintenance/I/O fence 全序，分别处理 coherent 与 non-coherent DMA，指出 acquire/release 不能代替必要 CMO。

**主要考察点：** DMA ownership、RVWMO、CMO、I/O ordering。

---

## Q102. IPI Mailbox 发布顺序

**类型：SMP 同步**

Hart0 写 per-hart mailbox 后通过 SBI IPI、ACLINT MSWI 或 IMSIC MSI 通知 Hart1。要求发送前 release、接收后 acquire/适当 fence，处理 coalesced IPI、clear 顺序和 mailbox 多生产者竞争。

**主要考察点：** IPI as notification not memory barrier、mailbox protocol、coalescing。

---

## Q103. 用 Litmus 与 Formal Model 验证

**类型：验证方法**

要求把一个偶发并发 bug 缩成 litmus，定义初值、hart 程序、允许/禁止 outcome；使用模型工具与 QEMU/Spike/真机 stress 各验证什么，为什么“跑十亿次没出现”不能证明禁止。

**主要考察点：** formal model、minimal reproducer、simulation vs proof。

---

## Q104. 原子与 Fence 性能评估

**类型：性能工程**

设计基准比较 LR/SC、AMO、CAS、ticket/MCS、不同 fence 强度。要求控制 hart placement、cache warmth、竞争度、频率、NUMA/cluster、编译器和统计分布；利用 PMU/互连指标而非只报平均 ns。

**主要考察点：** benchmarking、tail latency、microarchitecture、measurement bias。

---

## Q105. 并发原语 Review Checklist

**类型：Staff 规范设计**

建立覆盖语言内存模型、RVWMO、compiler barrier、address domain、PMA、alignment、IRQ/preempt、fairness、progress、overflow、reclamation、异构 hart、验证和文档契约的 checklist；要求每个原语注明最弱充分 ordering。

**主要考察点：** organization-wide concurrency standard、proof obligation、maintainability。

### 本批覆盖矩阵

| 能力 | 题目 |
|---|---|
| RVWMO/FENCE/依赖 | Q061～Q072 |
| LR/SC、AMO、Zacas | Q073～Q081 |
| 锁、IRQ 与扩展性 | Q082～Q086、Q094～Q100 |
| 无锁算法/回收/RCU | Q087～Q093 |
| I/O/IPI/验证/规范 | Q101～Q105 |

---

# Part III-C. MMU、PMP、Cache、DMA 与 IOMMU（Q106～Q145）

## Q106. `satp` MODE 与地址空间选择

**类型：体系结构基础**

比较 Bare、Sv32、Sv39、Sv48、Sv57 的 XLEN、级数、VPN/PPN 宽度和地址能力。要求通过 WARL 行为安全探测 MODE，解释 unsupported MODE、ASIDLEN 探测和为何不能根据编译目标假设硬件支持 Sv48。

**主要考察点：** `satp`、translation mode、WARL、capability discovery。

---

## Q107. 手工完成一次 Sv39 Page Walk

**类型：白板推演**

给定 VA、`satp.PPN` 和三级页表内存内容，计算 VPN[2:0]、页表项地址、leaf level、最终 PA；每一步检查 V/R/W/X/U/A/D、PMA/PMP 和 access type，指出 page fault 与 access fault 的分界。

**主要考察点：** Sv39 algorithm、PTE validation、fault classification。

---

## Q108. PTE 字段与权限组合

**类型：PTE 设计**

解释 V/R/W/X/U/G/A/D、RSW 和 PPN；推演 fetch/load/store 在 `SUM/MXR` 各组合下的权限。追问为何 `R=0,W=1` 非法、execute-only 页面如何被读取、global bit 在 non-leaf/leaf 上的传播语义。

**主要考察点：** PTE permissions、SUM/MXR、global mapping。

---

## Q109. Megapage/Gigapage 对齐

**类型：代码审查**

页表构造器在 Sv39 level-1 建 2 MiB leaf，却保留 PPN 低位。分析为何形成 misaligned superpage fault；写出按 level 验证 VA/PA/size 的通用算法，并讨论拆分大页、合并小页时的并发和 TLB 协议。

**主要考察点：** superpage、PPN alignment、mapping transformation。

---

## Q110. Canonical Virtual Address

**类型：地址分析**

解释 Sv39/Sv48/Sv57 高位必须如何与最高有效 VA bit 一致。给出若干高半地址判断其 canonical 性，说明 non-canonical 地址产生何种 fault、pointer tagging 是否可直接使用，以及内核地址布局为何要留扩展余量。

**主要考察点：** sign-extension rule、higher-half layout、pointer assumptions。

---

## Q111. Invalid/Reserved PTE 与前向兼容

**类型：代码审查**

审查用 invalid PTE 保存软件 metadata 的实现。当前特权规范允许在 `V=0` 时由软件使用其余 PTE 位，但重新置 `V=1` 前必须构造出在目标模式与已启用扩展下合法的 PTE。要求区分有效 PTE 中的 RSW、标准扩展占用位和 reserved bits，说明 speculative page walk、未来扩展及 swap-entry ABI 对编码、并发更新和前向兼容的约束。

**主要考察点：** reserved encoding、software PTE state、forward compatibility。

---

## Q112. Accessed/Dirty 位管理

**类型：页故障 / 并发**

比较硬件原子更新 A/D 与 Svade 异常管理方案。要求处理两个 hart 同时访问、清 A/D 采样 working set、PTE 被换出、TLB 中旧权限和 IOMMU/DMA dirty logging；说明 A/D 更新对 page-table memory PMA 的要求。

**主要考察点：** A/D schemes、atomic PTE update、aging、dirty tracking。

---

## Q113. `SUM/MXR` 与内核访问用户内存

**类型：安全代码审查**

分析内核临时设置 `sstatus.SUM` 后 copy_from_user，期间被中断/抢占的风险；`MXR` 如何允许从 execute-only 页读取。设计 per-hart 保存恢复、fault fixup 与最小开启窗口，并比较软件 page-table walk 方案。

**主要考察点：** privilege override、uaccess、preemption、least privilege。

---

## Q114. `SFENCE.VMA` 操作数语义

**类型：TLB 深挖**

比较 `sfence.vma x0,x0`、`sfence.vma va,x0`、`sfence.vma x0,asid`、`sfence.vma va,asid`。要求解释 VA 参数只使用哪些位、global mapping、invalid ASID、页表 Store 排序和为什么该指令只作用当前 hart。

**主要考察点：** address/ASID-selective fence、global entries、local scope。

---

## Q115. ASID 的作用与有效位宽

**类型：地址空间设计**

通过写全 1 再读回探测 ASIDLEN，设计 ASID allocator。分析 ASID 只对当前 hart是否具有全局含义、进程在 hart 间迁移、同一数值不同根页表和 global mapping 的正确使用。

**主要考察点：** ASID discovery、per-hart TLB、migration、tagged translations。

---

## Q116. ASID Rollover

**类型：系统设计**

当有限 ASID 耗尽时，设计 generation/bitmap 分配与 rollover shootdown；处理 sleeping CPU、offline/online、并发 context switch 和 lazy TLB。要求证明旧 generation 的 translation 不会被新地址空间误用。

**主要考察点：** generation protocol、global coordination、lazy flush。

---

## Q117. Context Switch 中更新 `satp`

**类型：OS arch 设计**

比较每次写 `satp` + full flush、ASID tagged switch、相同 mm fast path。要求排序加载 next page table 与恢复用户态、处理 speculatively cached translation、KPTI 双页表，以及 `satp` 写本身不等于 fence 的事实。

**主要考察点：** address-space switch、ASID optimization、KPTI、ordering。

---

## Q118. 多 Hart TLB Shootdown

**类型：SMP 协议**

设计发起者修改 PTE、选择目标 hart并等待安全完成后再回收物理页的完整协议：自建 IPI 路径应由远端执行 `SFENCE.VMA` 后显式 ack；采用 SBI RFENCE 时，应先依据锁定的 SBI 版本说明调用返回究竟保证“请求/IPI 已发送”还是“远端 fence 已完成”，并为页面复用补足所需完成协议。处理目标正切换 mm、离线、卡死、批量范围和 timeout；区分“权限已撤销”与“页可复用”的完成点。

**主要考察点：** remote invalidation、acknowledgement、page lifetime、failure handling。

---

## Q119. Kernel Global Mapping 与 KPTI

**类型：安全 / 性能权衡**

讨论 kernel 映射使用 G 位和所有进程共享 ASID-independent TLB 的收益；当 KPTI、module unload、live patch 或 direct-map permission change 时怎样失效。说明 global bit 用错会造成跨进程别名和安全问题。

**主要考察点：** global PTE、KPTI、shootdown scope、security isolation。

---

## Q120. Svnapot 连续映射

**类型：现代扩展**

以 Svnapot 1.0 为基线，解释其目前标准化的 level-0 64 KiB 连续映射如何编码，为什么其余 NAPOT 编码仍为 reserved、16 个 4 KiB PTE 必须保持一致，以及更新映射时为何要失效整个 64 KiB 范围内的所有 4 KiB 地址。比较 NAPOT mapping 与 superpage 的 page-table footprint/TLB 效果，并设计未实现 Svnapot 时的小页 fallback；未来新增粒度必须以锁定的新版本规范及其发现机制为准。

**主要考察点：** Svnapot 1.0、64 KiB NAPOT PTE、reserved encoding、TLB invalidation、fallback。

---

## Q121. Svpbmt 内存类型覆盖

**类型：内存属性**

解释 PMA 与 page-based memory type 的职责边界，设计 Normal/NC/IO 映射。分析同一 PA 以不同 PBMT alias、把 MMIO 映射为 cacheable、修改 PBMT 时的 TLB/Cache/DMA 协议，以及 guest stage 组合的影响。

**主要考察点：** PMA/PBMT、memory type alias、I/O safety、virtualization。

---

## Q122. Svinval 分离式失效序列

**类型：高级 TLB**

比较传统 `SFENCE.VMA` 与 Svinval 的 `SFENCE.W.INVAL`、`SINVAL.VMA`、`SFENCE.INVAL.IR` 设计意图。要求在支持检测、批量 invalidation、跨 hart协议和异常路径中保证 begin/end 配对，未支持时可靠 fallback。

**主要考察点：** Svinval、pipelined invalidation、feature abstraction。

---

## Q123. PMP 匹配与优先级

**类型：安全基础**

给定多条 PMP entry，推演 S/U-mode fetch/load/store 是否允许。说明最低编号优先、访问跨越区域边界、无 entry 匹配的默认行为、M-mode 是否受约束，以及 PMP granularity 如何探测。

**主要考察点：** PMP priority、default permissions、granularity、access coverage。

---

## Q124. TOR/NA4/NAPOT 编码

**类型：现场推演**

手工编码若干 ROM、DRAM、MMIO 区域，解释 TOR 下界来自前一 `pmpaddr`、NAPOT trailing ones、NA4 的可用性与 grain。要求优化有限 entry 数并避免空洞意外开放或 priority shadowing。

**主要考察点：** PMP address modes、entry packing、security review。

---

## Q125. Locked PMP 与 Smepmp

**类型：M-mode 安全设计**

解释 `L` bit 对普通 PMP 和 M-mode 的影响，再讨论 Smepmp 的 MML/MMWP/RLB 如何改变规则。设计 secure boot 后锁定策略、debug/RMA 例外和不可恢复配置错误处理，避免用旧 PMP 心智模型审查 Smepmp 系统。

**主要考察点：** PMP lock、Smepmp、M-mode isolation、lifecycle security。

---

## Q126. 动态更新 PMP 的安全窗口

**类型：安全并发**

OpenSBI domain 或 TEE 需要改变一组 PMP entry。要求设计禁止中断/并发 hart、先 deny 后 allow、CSR 写顺序、回滚与验证读回；分析 TOR 相邻 entry 更新产生临时扩大权限的风险。更新后若 PMP 变化影响页表所在物理内存或地址翻译的目标内存，M-mode 应执行 `SFENCE.VMA x0,x0`；实现 H 扩展的系统还要判断是否需要 `HFENCE.GVMA x0,x0`，并另行满足平台规定的流水线、Cache 或设备完成要求，不能用含糊的“加一个 fence”代替。

**主要考察点：** atomic policy update、PMP transient state、multi-hart coordination。

---

## Q127. Access Fault 与 Page Fault 归属

**类型：故障诊断**

对 fetch/load/store，区分地址翻译失败、PTE 权限、PMP 拒绝、PMA/总线错误和 page-table walk 本身失败。要求结合 `scause/stval/sepc`、page-table dump、M-mode log 与硬件错误寄存器定位，避免把所有 fault 都当缺页。

**主要考察点：** fault taxonomy、PMP/PMA/MMU layering、diagnostics。

---

## Q128. Cache 几何与 Block Size 发现

**类型：平台设计**

RISC-V 基础 ISA 不提供统一的 cache level/line CSR。要求说明通过 platform description、DT/ACPI、SBI/firmware 或 vendor CSR 获取几何，区分 coherency line、Zicbom/Zicboz block、DMA alignment，并处理异构 hart 值不一致。

**主要考察点：** discovery、architectural vs platform property、heterogeneity。

---

## Q129. Zicbom 指令语义

**类型：Cache Management**

比较 `cbo.clean`、`cbo.inval`、`cbo.flush` 的最终内存/Cache 状态与权限检查。追问 dirty block invalidate 的架构保证、虚拟地址翻译 fault、`menvcfg/senvcfg` 控制、guest trap 和 fence 配套。

**主要考察点：** Zicbom、privilege enable、translation、completion ordering。

---

## Q130. 编写安全的 CMO Range Helper

**类型：现场 Coding**

实现 clean/inval/flush range：起始向下、结束向上、`size==0`、地址溢出、block size 非常量、首尾部分 block policy。要求 API 显式区分“调用者保证独占整 block”与 bounce-buffer fallback。

**主要考察点：** range arithmetic、partial block、API contract、overflow。

---

## Q131. `FENCE.I` 到底保证什么

**类型：指令流同步**

解释本 hart 先前 data stores 与后续 instruction fetch 的同步，为什么普通 `FENCE` 不够、`FENCE.I` 不是通用 D-cache clean、并且只保证执行该指令的 hart。讨论 Zifencei 在用户 ABI 中通常为何经系统调用暴露。

**主要考察点：** Zifencei、local hart scope、I/D coherence、ABI mediation。

---

## Q132. JIT/Self-Modifying Code 发布

**类型：多 Hart 系统设计**

JIT 线程写代码，目标线程可能位于另一 hart。设计 W^X permission transition、D-cache/平台 CMO、remote `FENCE.I`、TLB shootdown 和 publication flag；防止执行半写指令、跨 block patch 与攻击者利用 RWX 窗口。

**主要考察点：** code publication、W^X、TLB/I-cache sync、security。

---

## Q133. Remote Instruction Fence

**类型：SBI / SMP**

比较 SBI RFENCE 的 remote fence.i、内核 IPI 后本地 `FENCE.I`、stop-machine patch 三种协议。要求区分 SBI 规范所报告的发送结果与调用方额外需要的完成 ack，并处理 hart mask/base、目标离线、批量 patch、firmware 版本差异和未来实现可能无需物理 I-cache flush。

**主要考察点：** remote instruction synchronization、SBI RFENCE、hotplug。

---

## Q134. Cacheable Alias 与 Synonym

**类型：高级 Cache**

同一 PA 被两个 VA 或不同 PBMT 映射。分析 VIPT/PIPT 实现差异、D/I synonym、用户 alias、DMA alias 和固件/OS handoff；要求建立“不创建冲突内存类型 alias”的映射规范与 debug 方法。

**主要考察点：** aliases、cache implementation freedom、memory-type consistency。

---

## Q135. CPU 写、DMA 读

**类型：驱动场景**

为 non-coherent TX 路径排列：CPU 写 payload、clean payload、写 descriptor、clean descriptor、release ownership、I/O fence、doorbell。说明各步目标点/完成条件、设备运行期间 CPU 禁止修改的 ownership 契约。

**主要考察点：** clean-before-device、descriptor ordering、doorbell、ownership。

---

## Q136. DMA 写、CPU 读

**类型：驱动场景**

设计 RX 路径：交付前处理 CPU dirty block，设备完成后确认 completion，再 invalidate/flush 并 acquire 后读取。比较预 invalidate、后 invalidate 和 bounce buffer，说明错误 invalidate 如何丢掉相邻 CPU 数据。

**主要考察点：** device-to-CPU ownership、dirty data、completion ordering。

---

## Q137. Partial Cache Block 与邻接破坏

**类型：故障分析**

DMA buffer 与普通变量共用 block，invalidate 后变量偶发回退。要求重建 dirty/clean/设备写时序，解释 allocator 的 cache-block alignment contract，给出 padding、专用 pool、bounce buffer 或 clean+invalidate 策略及其限制。

**主要考察点：** false sharing with DMA、partial invalidation、allocator design。

---

## Q138. Scatter-Gather DMA

**类型：综合 Coding/设计**

多个 segment 可重叠 block、方向不同、长度为零。设计去重/合并 CMO range、descriptor chain 发布、错误回滚和 completion ownership；评估逐段维护与批量维护的正确性和成本。

**主要考察点：** SG list、range coalescing、directionality、failure handling。

---

## Q139. Coherent DMA 仍需要什么

**类型：概念辨析**

即使设备参与 coherency，CPU store、descriptor ownership、doorbell 和 completion 仍需 ordering。区分 coherence、consistency、completion、IOMMU translation 和 device internal cache；说明 coherent 属性如何由平台/DT/ACPI 确认。

**主要考察点：** coherence vs ordering、device contract、discoverability。

---

## Q140. 异构 Cache Block Size

**类型：跨核可移植性**

不同 hart 的 CMO block size 或 cache topology 不同，任务又可迁移。要求选择系统级安全粒度、禁止不兼容 hart online 或 per-hart operation，分析 range 对齐扩大和 partial block ownership的交互。

**主要考察点：** heterogeneity、system-wide invariant、hotplug validation。

---

## Q141. RISC-V IOMMU 地址翻译路径

**类型：IOMMU 基础**

从 device ID/process ID、device context、process context 到 IO virtual address translation，说明 IOVA、GPA、SPA 的层次与 fault reporting。比较 CPU `satp` 页表和 IOMMU 数据结构可复用与不可假设之处。

**主要考察点：** device context、IOVA、multi-stage DMA translation、fault attribution。

---

## Q142. IOMMU 页表更新与失效

**类型：系统设计**

设计 map/unmap：发布 page table、发 IOMMU invalidation command、等待 completion、再复用物理页。处理 command queue ordering、device ATS/device TLB、并发 DMA 和 timeout；说明 CPU `SFENCE.VMA` 不能替代 IOMMU invalidation。

**主要考察点：** IO page-table ordering、IOTLB、ATS、page lifetime。

---

## Q143. 设备直通与 MSI 隔离

**类型：虚拟化 / 安全**

把 PCIe function 直通 guest，设计 DMA translation、MSI 到 IMSIC guest interrupt file 的重定向、reset/quiesce 和 teardown。分析恶意设备伪造 MSI/地址、共享 IOMMU group、PRI/ATS stale entry 和 guest 销毁竞态。

**主要考察点：** assignment isolation、IOMMU、AIA MSI、lifecycle safety。

---

## Q144. 只在真机出现的页表/Cache 故障

**类型：故障分析**

系统 QEMU 稳定，真机随机 instruction page fault 或 DMA corruption。要求按 `cause/tval/epc → PTE → satp/ASID → SFENCE → PMA/PMP → CMO → interconnect/IOMMU` 建立证据链，设计 trace buffer 与最小化实验。

**主要考察点：** cross-layer diagnosis、emulator gaps、observability。

---

## Q145. MMU/PMP/Cache/DMA Review Checklist

**类型：Staff 规范设计**

建立覆盖 mode/VA canonical、PTE、A/D、ASID、TLB shootdown、PMP priority/lock、PBMT alias、CMO discovery/range、DMA ownership、IOMMU invalidation、hotplug/异构和故障注入的评审表。

**主要考察点：** architecture governance、proof points、test matrix。

### 本批覆盖矩阵

| 能力 | 题目 |
|---|---|
| Sv39+ 页表与权限 | Q106～Q113 |
| TLB/ASID/扩展 | Q114～Q122 |
| PMP/Smepmp | Q123～Q127 |
| Cache/CMO/FENCE.I | Q128～Q140 |
| IOMMU/综合评审 | Q141～Q145 |

---

# Part III-D. Trap、Context Switch、RTOS/Linux arch（Q146～Q185）

## Q146. Trap 时硬件究竟保存什么

**类型：体系结构基础深挖**

分别说明 U→S、S→S、S→M、VS→HS Trap 时实际写入的目标特权级 CSR：例如 U→S 写 `sepc/scause/stval/sstatus`，S→M 写 `mepc/mcause/mtval/mstatus`；VS→HS 还要结合 `hstatus/htval/htinst` 解释来源信息。说明 privilege、interrupt-enable stack 与 PC 的自动更新，并强调硬件不保存 GPR、不自动切换普通栈。

**主要考察点：** architectural trap state、no automatic stack frame、privilege transition。

---

## Q147. `xstatus` 状态机

**类型：CSR 推演**

推演 Trap entry 和 `xRET` 对 `xIE/xPIE/xPP` 的变化；解释 MPRV、SUM、MXR、TVM、TW、TSR、FS、VS、XS、SD。给出错误 status 值，要求判断返回 privilege、中断窗口和扩展上下文泄漏。

**主要考察点：** status stacking、return semantics、privilege control、extension state。

---

## Q148. Direct Trap Vector 设计

**类型：现场 Coding**

设计统一 `stvec` Direct 入口，通过 `scause` 最高位区分 interrupt/exception，再做有界 dispatch。要求保存用于 decode 的临时寄存器、处理未知 cause、避免可控 index 越界、使入口在 MMU/栈异常下仍可诊断。

**主要考察点：** Direct mode、safe dispatch、early-register problem、robustness。

---

## Q149. Vectored Trap Table 设计

**类型：现场 Coding / 架构题**

构造 `BASE+4*cause` 的中断 slots，每槽只能可靠容纳一条 32 位跳转或短 stub；异常统一到 BASE。分析大 cause、C 扩展、跳转范围、表对齐、AIA 扩展 cause 和汇编期 slot 大小断言。

**主要考察点：** Vectored mode、slot geometry、future interrupt IDs、link range。

---

## Q150. `xscratch` 与第一条 Trap 指令

**类型：入口设计**

在没有空闲 GPR 时，用 `csrrw` 交换 `sp` 或 `tp` 与 `sscratch/mscratch`。要求区分 user entry、kernel re-entry、M-mode per-hart scratch，证明原值不会丢失；分析 scratch 为零、嵌套 Trap 和栈指针已损坏的路径。

**主要考察点：** scratch CSR、register bootstrap、nested trap、stack recovery。

---

## Q151. 设计统一 `struct pt_regs`

**类型：系统设计**

为 RV64 Linux-like OS 设计 GPR、status、epc、badaddr、cause、original a0、syscall metadata 的固定帧。要求汇编 offset 与 C 结构单一来源、16 字节栈对齐、ptrace/signal/crash dump 兼容，并讨论 RV32 与扩展状态是否分帧。

**主要考察点：** exception ABI、layout generation、tooling consumers。

---

## Q152. 保存 x1～x31 的正确顺序

**类型：现场 Coding**

写完整 save/restore macros，不覆盖尚未保存的 `t0-t6/a0-a7`，正确记录原 `sp/gp/tp`。说明 x0 不需保存但槽位可能为调试保留，恢复 frame-base 寄存器必须最后处理，`sret` 前不能再调用普通 C。

**主要考察点：** register frame、macro auditability、restore hazard。

---

## Q153. 异常前原始 SP

**类型：代码审查**

入口先 `addi sp,sp,-PT_SIZE` 再把 `sp` 写入 frame，声称保存 original SP。要求在 U→S、S→S、IRQ stack switch、嵌套四种路径重建正确值，区分 interrupted stack 与 exception stack。

**主要考察点：** stack provenance、frame arithmetic、nesting。

---

## Q154. 汇编入口调用 C 的 ABI 边界

**类型：接口设计**

定义 `do_trap(regs,cause)` 的参数、栈对齐、可破坏寄存器和返回动作。讨论编译器是否允许 FPU/Vector、instrumentation、stack protector、sanitizer、shadow call stack，以及最低级 Trap 为什么应使用独立编译 flags。

**主要考察点：** psABI at trap boundary、compiler features、build isolation。

---

## Q155. 最小中断帧还是完整中断帧

**类型：RTOS/Linux 权衡**

比较仅保存 caller-saved 与保存全部 GPR。解释异步中断发生在任意指令处，ABI call boundary 不能直接套用；给出 leaf fast IRQ、延迟完整保存、可调度 IRQ、debug/profiling 对 frame 的不同要求。

**主要考察点：** asynchronous context、lazy save、preemption、observability。

---

## Q156. 嵌套 Interrupt 的开启时机

**类型：代码审查**

入口保存少数寄存器后立即设置 `sstatus.SIE`。分析当前 source 是否已 ack/mask、priority controller 状态、栈与 per-CPU nesting、handler reentrancy、调度限制；设计保存原 IE 状态与分级阈值的安全序列。

**主要考察点：** nested interrupt、controller state、reentrancy、state restore。

---

## Q157. Per-Hart IRQ Stack

**类型：系统设计**

设计从 task kernel stack 切换 per-hart IRQ stack，嵌套时不重复切换，保留 interrupted SP；加入 guard page/canary、overflow emergency stack，并解决 IRQ return 前发生 scheduler switch 时 frame ownership。

**主要考察点：** stack switch、per-hart data、nesting、overflow containment。

---

## Q158. Delegation 与 Trap 路由

**类型：特权架构推演**

给定 `medeleg/mideleg/hedeleg/hideleg`，推演 U/S/VS/VU 的 ecall、page fault、timer/external interrupt 最终进入 M/HS/VS。说明 delegated Trap 在当前已处于更高 mode 时的行为，以及 delegation 不等于权限授予。

**主要考察点：** trap routing、privilege hierarchy、H extension delegation。

---

## Q159. U-mode System Call Entry

**类型：现场设计**

设计 `ecall` ABI：syscall number、参数、返回值/errno、保存 original args、推进 `sepc`、可重启 syscall。明确 `ECALL` 没有压缩编码，正常返回路径通常将 EPC 推进 4 字节，而 restart 路径可以保留或恢复原始 EPC；再讨论 signal/ptrace/seccomp-like hook 和用户指针验证。

**主要考察点：** syscall ABI、EPC advance、restart、security validation。

---

## Q160. Page Fault 可恢复路径

**类型：故障分析**

区分用户缺页、copy_from_user fault fixup、kernel bug、stack growth、COW、A/D software fault。要求在可能睡眠的 fault handler 前建立完整 frame，处理中断状态、mmap lock、重复 fault 和 OOM；内核原子上下文不得随意缺页。

**主要考察点：** fault classification、exception tables、sleepability、recovery policy。

---

## Q161. Misaligned Access Emulation

**类型：Instruction Emulation**

从 faulting instruction 解码 load/store 宽度、signedness、rd/rs、立即数；安全分字节访问并正确更新 EPC。处理 MMIO 不可拆分、跨页二次 fault、部分 Store、C 指令和用户访问，说明为何内核通常优先避免而非依赖模拟。

**主要考察点：** decoder、precise side effects、MMIO/PMA、nested fault。

---

## Q162. FPU/Vector 首次使用 Trap

**类型：Lazy Context**

Illegal instruction 可能是扩展不存在，也可能是 FS/VS=Off。设计区分路径，分配上下文、初始化 architectural state、设置 Initial/Clean/Dirty 并重试；防止用户用连续 Trap 制造分配 DoS。

**主要考察点：** FS/VS state machine、lazy allocation、illegal instruction triage。

---

## Q163. Breakpoint、Single-Step 与 `sepc`

**类型：Debug / Trap**

处理 `EBREAK`、软件断点替换、trigger module 和 debug-mode entry。要求正确判定原指令 2/4 字节、恢复被替换指令、同步 I-cache、多 hart 停止，以及 breakpoint trap 与进入 Debug Mode 的区别。

**主要考察点：** breakpoint lifecycle、RVC、debug architecture、code patching。

---

## Q164. NMI/RNMI 与普通 Trap 的边界

**类型：可靠性**

在实现 Smrnmi 或平台 NMI 的系统中，说明普通 `mtvec/mstatus` 假设为何可能不成立，RNMI 专用状态/返回如何隔离。设计 watchdog/RAS NMI 的最小保存、不可睡眠、不可依赖普通栈与有限日志路径。

**主要考察点：** RNMI、fatal path、independent context、implementation discovery。

---

## Q165. Double Trap 与关键窗口

**类型：可靠性 / 安全**

Trap entry 尚未保存 `xepc/xcause` 又发生第二个 Trap。讨论 Smdbltrp 等机制（若实现）、status 的 double-trap 控制、early critical window、emergency stack 和 firmware handoff；设计 fail-stop 而非递归打印。

**主要考察点：** double-trap containment、critical entry window、fatal escalation。

---

## Q166. 普通任务 Context Switch

**类型：现场 Coding**

实现只保存 psABI callee-saved 的 cooperative/scheduler switch，解释 `ra` 怎样充当 resume PC、`sp` 与 `s0-s11` 的 layout、为何 Trap 完整 frame 不应重复拷贝，并补齐 unwind/debug metadata。

**主要考察点：** switch frame、callee-saved contract、scheduler boundary。

---

## Q167. 首次启动 RTOS Task

**类型：上下文构造**

构造未运行任务的栈，使首次 restore 后进入 trampoline，将参数传给 entry，返回走 task_exit。要求 16 字节对齐、清理 frame、设置 interrupt state，并处理 entry 异常返回、TLS/`tp` 和 stack canary。

**主要考察点：** synthetic context、trampoline、task lifecycle、ABI。

---

## Q168. 首次返回用户进程

**类型：OS 架构设计**

构造用户 `pt_regs`，设置 `sepc/sp/a0` 和 `sstatus.SPP=U/SPIE`，安装 `satp/ASID`，再复用统一 `sret` 路径。验证用户 PC/SP canonical、PTE U 权限、SUM 清零和残留寄存器不泄漏。

**主要考察点：** user bootstrap、privilege return、address-space/security validation。

---

## Q169. Kernel Thread 与用户线程切换差异

**类型：设计辨析**

比较 kernel thread 没有用户 `satp/pt_regs` 或借用 active_mm 的场景；设计 fork trampoline、返回函数与退出路径。说明 kernel thread 切换到用户 task 时 address-space、`sscratch`、`tp` 和 lazy extension state 的更新。

**主要考察点：** active_mm、thread bootstrap、entry-state transition。

---

## Q170. FPU Context 完整状态

**类型：上下文管理**

列出 `f0-f31`、`fcsr` 与 FLEN/ABI 关系；根据 FS Dirty 决定保存，恢复时避免内核污染用户 state。讨论异构 FLEN、signal frame、ptrace、kernel-mode FPU 临界区和 hard-float firmware 的编译契约。

**主要考察点：** floating context、FS、FLEN、kernel FPU ownership。

---

## Q171. Lazy FPU Switching 的安全陷阱

**类型：安全故障分析**

分析仅切换 owner 指针却未清 FS/寄存器导致跨任务信息泄漏；Trap 中使用浮点导致递归；任务迁移到另一 hart 后 owner 不一致。比较 eager、lazy restore、lazy allocation 的收益与 Spectre-like residual state 风险。

**主要考察点：** lazy ownership、data leakage、migration、nested trap。

---

## Q172. Vector Context 大小与保存

**类型：RVV / OS 设计**

根据 `vlenb` 计算 32 个向量寄存器存储，保存 `vstart/vl/vtype/vcsr`。要求防整数溢出、alignment、heterogeneous VLEN、task migration 和动态分配失败；分析 eager/lazy 策略的 latency 与内存成本。

**主要考察点：** variable VLEN、context sizing、VS state、scheduler affinity。

---

## Q173. `vstart/vl/vtype` 的精确恢复

**类型：代码审查**

审查只保存 v0-v31 而遗漏 vector CSRs 的实现。说明可重启 vector instruction、`vstart` 非零、vill、tail/mask policy 对语义的影响；设计保存代码时如何使用 vector 指令而不先破坏待保存状态。

**主要考察点：** vector CSR state、restartability、self-clobber avoidance。

---

## Q174. 构造 User Signal Frame

**类型：Linux arch 设计**

把 GPR、PC/status、FPU/Vector 可变长度 state 保存到用户栈，进入 handler，`rt_sigreturn` 验证并恢复。要求处理用户栈 fault、altstack、对齐、扩展记录链、VLEN兼容和伪造 status/PC 的安全检查。

**主要考察点：** signal ABI、extensible context、user-controlled restore、security。

---

## Q175. `sret` 前的安全检查

**类型：代码审查**

列出返回用户态必须验证/固定的 `SPP/SPIE/SIE/SUM/MXR/FS/VS`、`sepc`、用户 `sp`、`satp`、`sscratch`、`tp` 和 GPR 清理。讨论 speculation、pending work、signal/reschedule 与单一 exit path 的价值。

**主要考察点：** safe return-to-user、state sanitization、exit loop。

---

## Q176. `tp` 与 Per-CPU/Current Task

**类型：Linux/RTOS ABI**

比较内核用 `tp` 保存 current/per-CPU 与用户 TLS `tp`。设计 user entry 时 `csrrw tp,sscratch,tp` 类交换、kernel re-entry 判定和 context switch 更新；说明中断在交换中间到来如何保证原子性。

**主要考察点：** thread pointer multiplexing、sscratch protocol、per-hart state。

---

## Q177. Debug Trigger Context

**类型：调试 / 上下文**

多进程共享 trigger module 时，设计 `tselect/tdata*` 的保存、权限与 lazy programming。区分 debug-mode CSRs `dcsr/dpc` 与普通 S-mode 可管理状态，处理触发器数量 WARL、任务迁移和安全域隔离。

**主要考察点：** trigger virtualization、debug privilege、context ownership。

---

## Q178. Shadow Stack/CFI 对汇编入口的影响

**类型：安全工程**

假设工具链/平台实现软件 shadow call stack 或 control-flow integrity，分析 Trap entry、手写 switch、tail call 和 signal trampoline 需要的 annotation/保存。设计不可被普通栈破坏绕过、又能支持 unwind 的边界。

**主要考察点：** control-flow protection、assembly annotations、nonstandard context state。

---

## Q179. 任务跨 Hart 迁移

**类型：SMP Context**

列出除 GPR 外迁移相关的 ASID/TLB、FPU/Vector owner、VLEN/ISA affinity、PMU、debug trigger、timer、per-CPU pointer、reservation 和 cache/DMA ownership。要求定义 migrate-disable 与 capability mask。

**主要考察点：** heterogeneous SMP、lazy state、scheduler/arch contract。

---

## Q180. RTOS Tick、抢占与确定性

**类型：RTOS 设计**

从 timer interrupt 进入、更新 tick、决定调度、保存完整 frame、切任务并返回。比较 tickful/tickless、PLIC/ACLINT/Sstc 来源，量化最坏 entry latency、嵌套、FPU/Vector lazy trap 对 WCET 的破坏。

**主要考察点：** preemptive RTOS、timer path、WCET、lazy-state tradeoff。

---

## Q181. Linux Entry/Exit 状态机

**类型：综合架构题**

设计 user Trap、kernel Trap、syscall、IRQ 共用与分叉的入口/退出图。要求覆盖 `sscratch/tp`、`pt_regs`、irq stack、preempt count、reschedule/signal、uaccess、RCU context tracking 和 `sret` 安全点。

**主要考察点：** Linux arch entry、state machine、cross-subsystem invariants。

---

## Q182. Kprobe/Livepatch 与压缩指令

**类型：运行时修改**

在 16/32 位混合指令流插断点或跳板，处理指令边界、PC-relative relocation、跳转范围、并发执行、`FENCE.I` 和 rollback。说明哪些指令不能简单搬到 trampoline，如何 stop-machine 或 text-poke。

**主要考察点：** dynamic instrumentation、RVC decoding、relocation、atomic patch。

---

## Q183. Stack Overflow 时如何处理 Trap

**类型：可靠性**

如果当前 task stack 已越界，普通入口再减 `sp` 会覆盖 guard 或 fault。设计 guard page、提前 bounds check、per-hart emergency stack、double-trap escalation 和最小 dump；确保检查本身不用损坏栈。

**主要考察点：** overflow containment、emergency entry、recursive-fault avoidance。

---

## Q184. 不可递归 Panic Entry

**类型：系统设计**

设计保存最小 hart state、原子选主 panic hart、IPI 停其他 hart、禁中断、切 emergency stack、输出有界日志和触发 reset。处理 console lock 已占用、其他 hart 不响应、IOMMU/DMA 仍运行和敏感数据清理。

**主要考察点：** fail-stop design、SMP panic、lock avoidance、crash integrity。

---

## Q185. Trap/Context Switch Review Checklist

**类型：Staff 规范设计**

建立覆盖首条指令、scratch、完整 frame、stack alignment/overflow、嵌套、委托、C ABI、EPC 判长、FPU/Vector/debug、signal、用户返回、迁移、fatal path、unwind 与 fault injection 的清单。

**主要考察点：** arch entry standard、security/reliability audit、test coverage。

### 本批覆盖矩阵

| 能力 | 题目 |
|---|---|
| Trap 模型与入口 | Q146～Q158 |
| 系统调用/异常模拟/可靠性 | Q159～Q165 |
| 调度与首次上下文 | Q166～Q169 |
| FPU/Vector/Signal/Debug | Q170～Q178 |
| SMP 迁移、RTOS/Linux、综合评审 | Q179～Q185 |

---

# Part III-E. SMP、Timer、PLIC/ACLINT/AIA（Q186～Q230）

## Q186. `mhartid` 与硬件拓扑

**类型：拓扑发现**

解释 hart ID 是平台唯一标识但不保证从 0 连续、也不必编码 cluster/core。要求从 DT/ACPI 建立 hartid↔logical CPU 映射，处理超 XLEN/稀疏 ID、disabled hart、boot hart 不为 0 和热插拔稳定编号。

**主要考察点：** hart identity、firmware description、logical topology。

---

## Q187. Logical CPU ID 映射设计

**类型：系统设计**

比较排序表、hash、固件传入 index 和直接数组；评估 early boot 无 allocator、汇编快速路径、IPI mask 表达、NUMA/cluster affinity 与 crash dump 可读性。要求所有入口拒绝未知 hartid 而非越界访问。

**主要考察点：** mapping data structure、early constraints、bounds safety。

---

## Q188. 判断 Primary Hart 的错误方法

**类型：代码审查**

审查 `csrr a0,mhartid; bnez a0,secondary`。给出 boot hart 非零、hart0 disabled、多 socket 多 firmware instance 场景；设计由 BootROM/OpenSBI contract、atomic lottery 或 FDT 指定的冷启动 owner，并处理并发清 `.bss`。

**主要考察点：** boot-hart contract、coldboot lottery、multi-instance firmware。

---

## Q189. Spin-table Bring-up

**类型：现场设计**

设计 entry/opaque/state mailbox：主 hart release 发布后发通知，从 hart acquire 读取、原子 claim 并跳转。处理 stale event、虚假 `WFI` 返回、重复启动、non-coherent Cache、代码同步和 timeout/diagnostic。

**主要考察点：** publication protocol、WFI semantics、state machine、coherency。

---

## Q190. SBI HSM `hart_start/stop/status`

**类型：SBI / SMP**

解释 HSM 各状态和调用参数，设计 S-mode CPU bring-up。比较 HSM 与 spin-table 的 owner/权限、M-mode准备、启动地址校验；说明 `hart_stop` 成功后不返回以及 warm start 如何区别 platform cold reset。

**主要考察点：** SBI HSM、hart lifecycle、firmware/OS responsibility。

---

## Q191. HSM 启动竞态

**类型：故障分析**

两个 CPU 同时请求启动同一 hart，或 stop 与 start 交错。要求推演 START_PENDING/STOP_PENDING 等中间状态、错误码、payload 尚未发布、启动超时和 rollback；OpenSBI 内部必须用何种原子/锁保护状态。

**主要考察点：** lifecycle concurrency、linearization point、error recovery。

---

## Q192. Warmboot Entry 与临时栈

**类型：启动设计**

从 hart 上电、retention resume、HSM start 都可能进入 firmware warmboot。设计 per-hart scratch/stack、识别原因、恢复平台 state、加入 domain 后再进入 next stage；防止复用仍被上次 Trap 占用的栈。

**主要考察点：** warmboot context、per-hart scratch、resume classification。

---

## Q193. 进入 Coherency Domain

**类型：SoC 平台题**

基础 ISA 不定义互连 snoop enable。要求明确平台 hook 在何时开启 coherency、开启前 mailbox 如何通信、Cache 中 dirty firmware state 怎样 handoff、哪些 fence 不能替代 interconnect configuration，并设计多 cluster 验证。

**主要考察点：** platform coherency、pre-coherent communication、handoff。

---

## Q194. Per-Hart Data 的汇编访问

**类型：现场 Coding**

比较 `tp` 直接指向、hartid 查表、offset table 和 scratch 指针。要求入口第一时间可取 stack/current、支持稀疏 hartid、用户 `tp` 切换、hotplug 初始化和不同 firmware domain 的地址隔离。

**主要考察点：** per-hart base、fast-path ABI、topology mapping。

---

## Q195. IPI Mailbox 与 Reason Bits

**类型：SMP 设计**

多个发送者原子 OR reason bits，再触发一次 IPI；接收者 exchange/loop 清取。分析 IPI coalescing、reason 在清 IPI 前后到达、lost wakeup、优先级与 starvation，并定义 reschedule/TLB/call-function/panic 的处理顺序。

**主要考察点：** coalesced notification、atomic mailbox、clear race。

---

## Q196. SBI IPI Hart Mask

**类型：SBI 接口**

解释 `hart_mask` + `hart_mask_base` 表达方式、all-harts 特例、无效/不可用 hart 和 XLEN 差异。设计把 cpumask 分块为多次 SBI call，处理 partial failure，并说明调用前 mailbox publication ordering。

**主要考察点：** SBI hart mask ABI、sparse IDs、batching、error handling。

---

## Q197. SBI RFENCE 与 Shootdown

**类型：远程同步**

比较 remote fence.i、remote sfence.vma、ASID variant 及 H 扩展 fence 服务。SBI v3.0 的返回码表将 `SBI_SUCCESS` 描述为“IPI 已成功发送到全部目标 hart”，因此要求区分这一可移植返回保证、具体 provider 可能提供的更强内部完成语义，以及回收页表/物理页所需的额外 ack。再处理地址/size overflow、批量合并和 HSM stopped hart；解释 RFENCE 不是页生命周期协议的全部。

**主要考察点：** SBI RFENCE、completion semantics、range validation。

---

## Q198. Panic Stop IPI

**类型：可靠性**

设计不依赖普通 scheduler/动态分配的 stop-other-harts：原子选主、发布 panic generation、发送 IPI、远端最小保存并停机、主 hart bounded wait。处理 hart 卡在 M-mode/关中断、console ownership 和 watchdog fallback。

**主要考察点：** crash coordination、IPI limitations、bounded failure handling。

---

## Q199. CPU Offline/Online 的架构状态

**类型：生命周期设计**

offline 前迁移任务/中断/timer，清 per-hart lazy owners，离开 scheduler，调用 HSM stop；online 恢复 trap vector、satp、ASID generation、interrupt controller、timer、PMU。要求与 TLB shootdown/RCU 等待者握手。

**主要考察点：** hotplug ordering、per-hart state、global protocol participation。

---

## Q200. `WFI` 的精确语义

**类型：体系结构深挖**

解释 `WFI` 是 hint、实现可以合法地立即返回，且 `TW/VTW` 可使其 Trap；规范要求当任一特权级“本地使能”的中断变为 pending 时恢复执行，而不以全局 `xIE` 为前提，但恢复后是否立即进入 Trap 仍取决于 privilege、delegation 和全局使能条件。要求设计“检查条件→WFI→重查”的循环，允许 spurious/early return，不把 `WFI` 当作事件计数或无丢失通知原语，并区分 idle、spin wait、suspend 和 hart stop。

**主要考察点：** WFI hint semantics、lost wakeup avoidance、virtualization trap。

---

## Q201. `time/cycle/instret` 可见性

**类型：计时器 / Counter**

解释 Zicntr counter、`mcounteren/scounteren/hcounteren` 权限，RV32 读取 64 位 counter 的 high-low-high 稳定算法。区分 cycle 与 wall time、频率来源、虚拟化 offset 和 counter overflow。

**主要考察点：** counters、delegation、RV32 atomic read、timebase contract。

---

## Q202. CLINT 与 ACLINT

**类型：平台中断模型**

比较传统 CLINT 的 MSIP/MTIME/MTIMECMP 与 ACLINT 的 MTIMER/MSWI/SSWI 分块。说明它们是平台设备而非基础 ISA、hart 索引映射、MMIO 宽度/endian、委托/SBI 关系和 DT 描述。

**主要考察点：** timer/IPI devices、platform discovery、privilege routing。

---

## Q203. RV32 写 64 位 `mtimecmp`

**类型：现场 Coding**

设计避免中途产生伪 timer interrupt 的写序列，例如先把 low 写为最大值，再写 high，最后写 low；要求结合设备规范确认寄存器顺序、处理 posted MMIO 和已经 pending 的 timer source。

**主要考察点：** split-width MMIO、atomic update pattern、timer race。

---

## Q204. SBI TIME `set_timer`

**类型：SBI / OS**

用绝对 `stime_value` 编程下一事件：未来值必须使待处理的 supervisor timer interrupt 被清除，过去值可能使其立即 pending，`UINT64_MAX` 表示实际上无限远的事件。处理 RV32 参数拆分、M-mode timer multiplex 和 spurious interrupt，并说明标准 TIME 调用的返回约定与底层 timer backend 的错误处理边界。

**主要考察点：** SBI TIME、absolute deadline、timer virtualization。

---

## Q205. Sstc `stimecmp/vstimecmp`

**类型：现代 Timer 扩展**

比较 S/HS 直接写 `stimecmp` 与 SBI TIME trap 的成本；分析 `menvcfg.STCE`、counter permissions、guest `vstimecmp`、migration 和不支持 Sstc 的 fallback。要求避免仅以 CSR 可汇编判断运行时存在。

**主要考察点：** Sstc、timer delegation、guest timer、feature fallback。

---

## Q206. Tickless Idle 与 Deadline Race

**类型：RTOS/Linux 设计**

读取当前 time、选择最早 deadline、写 compare、进入 idle；分析 deadline 在编程过程中已过、远端 enqueue 更早 timer、IPI 与 timer 同时到达。设计 recheck、per-CPU timer queue 和 broadcast timer fallback。

**主要考察点：** tickless race、absolute time、IPI/timer coordination。

---

## Q207. 多 Hart Timebase 与同步

**类型：平台验证**

time counter 是否跨 hart 同步、是否在 suspend 停止、频率是否固定由平台规范决定。设计 boot 校准、migration-safe clocksource、clockevent、wrap 和 resume correction；不可把 `cycle` 当全局纳秒时钟。

**主要考察点：** timebase properties、clocksource validation、power management。

---

## Q208. PLIC Gateway/Source/Context 模型

**类型：中断控制器基础**

描述设备 source 经 gateway 形成 pending，按 enable/priority/threshold 选择目标 context，再 claim/complete。区分 hart 与 context、M/S context 可选性，以及 PLIC 规范未统一规定 topology discovery 的平台责任。

**主要考察点：** PLIC architecture、context mapping、platform description。

---

## Q209. 发现 PLIC Context

**类型：设备树 / BSP**

根据 DT interrupt-extended 等信息映射 hart privilege context，拒绝硬编码 `2*hartid+1`。处理缺失 S context、稀疏 hart、多个 PLIC、NUMA 和 guest emulation；建立 logical CPU 到 context MMIO 的表。

**主要考察点：** PLIC discovery、DT parsing、topology portability。

---

## Q210. PLIC 初始化责任划分

**类型：Firmware/OS 边界**

定义 M-mode OpenSBI 与 S-mode OS 谁初始化 priority、enable、threshold、source routing。避免 firmware 清 pending/claim 掉属于 OS 的中断；设计 cold boot、kexec、warm reset 和 domain partition 的 ownership handoff。

**主要考察点：** interrupt-controller ownership、boot handoff、multi-domain isolation。

---

## Q211. PLIC Priority 与 Threshold

**类型：语义推演**

给定多个 pending source、priority、enable 和 threshold，判断 claim ID；说明 priority 0、相同优先级 tie、threshold 比较和实现的 priority bit 数 WARL。设计 critical section 时不要误把 threshold 当 CPU 全局 IE。

**主要考察点：** priority arbitration、WARL、threshold masking。

---

## Q212. Claim/Complete 正确顺序

**类型：现场 Coding**

写 PLIC external interrupt loop：claim、检查 ID、处理 device、complete。说明 claim read 的原子副作用、ID 0、complete 写值、嵌套/迁移和错误 context；何时循环 claim 以耗尽 pending。

**主要考察点：** PLIC state machine、MMIO side effects、handler loop。

---

## Q213. Level-triggered Interrupt 完成

**类型：故障分析**

比较先清设备 condition 再 complete 与相反顺序。推演 source 仍 asserted、gateway ready、重新 pending 和 interrupt storm；加入 posted device write read-back、I/O ordering，并说明 PLIC complete 不会替设备清中断。

**主要考察点：** level IRQ、device ack、gateway handshake、MMIO ordering。

---

## Q214. Edge-triggered Interrupt 丢事件

**类型：故障分析**

PLIC pending 通常不能计数无限多个边沿。分析 handler 期间多次 edge、设备内部 status FIFO 与 gateway 行为；设计 drain-until-empty、mask/unmask 或 MSI queue，并用硬件计数器验证是否丢失。

**主要考察点：** edge coalescing、device-level queuing、loss diagnosis。

---

## Q215. Priority Mask 与嵌套 IRQ

**类型：系统设计**

用 PLIC threshold 配合 `sstatus.SIE` 实现只允许更高优先级嵌套。要求保存/恢复原 threshold、先完成最小 frame、处理相同 source 重入、per-context MMIO ordering 和 handler 可重入性。

**主要考察点：** nested priority、threshold stack、entry ordering。

---

## Q216. PLIC 中断 Affinity

**类型：SMP 设计**

PLIC 以每 context enable 位控制 affinity。设计迁移 source：先在旧 CPU 禁用/同步 active handler，再新 CPU 启用，处理 pending window、level source 和 CPU offline。比较 enable 多 CPU 的负载均衡与抢占 claim 语义。

**主要考察点：** affinity migration、pending race、context enable model。

---

## Q217. S-mode External Interrupt 路由

**类型：特权边界**

说明 `mideleg`、`mie/mip/sie/sip` alias、PLIC S context 如何共同使 SEI 到达 S-mode。给出“PLIC pending 但 S-mode 不进中断”的逐门排查：device、gateway、enable、priority、threshold、delegation、CSR IE、global SIE。

**主要考察点：** end-to-end interrupt path、delegation、diagnostic checklist。

---

## Q218. CPU Hotplug 与 PLIC Context

**类型：生命周期**

offline 前迁移/禁用 source、完成 active claims、设置 threshold；online 重建 enable/priority state。处理中途 arriving level IRQ、per-context register 保存成本和 firmware/domain 仍占用的 source。

**主要考察点：** PLIC lifecycle、active interrupt、ownership coordination。

---

## Q219. Suspend/Wakeup Interrupt

**类型：电源管理**

选择能唤醒 power domain 的 source，配置 PLIC/APLIC/IMSIC 与平台 wake controller；保存上下文前防止 pending 丢失，resume 后区分 wake reason、重新初始化失电状态。说明普通 pending 不代表硬件具备唤醒布线。

**主要考察点：** wake routing、power-domain state、resume ordering。

---

## Q220. AIA 为什么不只是“新版 PLIC”

**类型：架构比较**

说明 AIA 增加 local interrupt priority、APLIC、IMSIC、MSI、guest interrupt file 与虚拟化能力。比较 wired direct delivery、APLIC→IMSIC MSI、PCIe MSI 路径，指出哪些组件/模式是可选的。

**主要考察点：** AIA component model、MSI、virtualization scalability。

---

## Q221. APLIC Direct Delivery

**类型：设备控制器设计**

配置 domain、source mode、target hart/priority、enable 与 IDC delivery/threshold；处理 inactive/detached source、level/edge mode和 claim/complete。比较其语义与 PLIC，避免复用错误寄存器假设。

**主要考察点：** APLIC direct mode、domain、IDC、source configuration。

---

## Q222. APLIC MSI Delivery

**类型：MSI 路由**

把 wired source 转换成发往 IMSIC 的 MSI，配置 target hart/guest/EIID 与 MSI address。分析 target 更改、pending source、源端重触发、MSI 写 ordering 和 domain delegation；说明 EIID 0 不可作为有效中断身份。

**主要考察点：** APLIC MSI mode、EIID、target encoding、ordering。

---

## Q223. IMSIC Interrupt File

**类型：AIA 现场推演**

解释每 hart 的 M/S/VS interrupt files、pending/enable、delivery、threshold、top interrupt 与 claim/complete 接口。要求通过 indirect CSR 安全访问位图，处理实现的 identity 数量、保留 ID 和优先级宽度。

**主要考察点：** IMSIC state、interrupt files、indirect CSR、WARL discovery。

---

## Q224. AIA Local Interrupt Priority

**类型：优先级模型**

推演多个 local 与 external interrupt 同时 pending 时的 major priority、`xip/xie`、`xtopi`/external top 状态。说明数值编码、实现 priority bits、同目标 privilege 才比较等规则，并设计 nested interrupt threshold。

**主要考察点：** AIA priority integration、top interrupt、local/external arbitration。

---

## Q225. `xiselect/xireg` 并发访问

**类型：代码审查**

间接 CSR 先写 selector 再读写 data；如果同 hart Trap handler 也使用同一窗口，selector 会被破坏。设计关中断/锁/保存恢复协议，区分 M/S/VS 窗口和 Smstateen 权限控制。

**主要考察点：** indirect CSR atomicity、same-hart reentrancy、state-enable security。

---

## Q226. IMSIC IPI

**类型：SMP / AIA**

通过写目标 IMSIC MSI page 发送 IPI。要求计算目标地址、data=identity、发布 mailbox 后 fence、处理 IPI identity enable/priority、coalescing和 CPU offline；比较 ACLINT SSWI 与 SBI IPI 的可移植性。

**主要考察点：** MSI-based IPI、addressing、notification ordering、fallback。

---

## Q227. MSI 与内存可见性

**类型：内存序综合**

设备或 hart 写共享数据后发 MSI，不代表数据天然完成可见。要求依据 coherent/non-coherent、IOMMU、PCIe ordering、RISC-V fence 设计 producer/consumer 协议，并区分 IMSIC pending 可见与 payload DMA 完成。

**主要考察点：** MSI as signal、DMA ordering、IOMMU/coherency interaction。

---

## Q228. VS Guest Interrupt File 分配

**类型：AIA 虚拟化预备题**

为每物理 hart有限 guest interrupt files 分配 `VGEIN`，处理 vCPU schedule-in/out、pending state、迁移和 overcommit。比较直接 guest-file delivery 与 HS software injection，定义没有空闲 file 时的 fallback。

**主要考察点：** IMSIC guest files、VGEIN、vCPU migration、resource virtualization。

---

## Q229. 中断只处理一次/中断风暴综合诊断

**类型：真机故障分析**

建立从 device status、trigger type、controller pending/enable/priority、claim/complete、CPU CSR、delegation 到 handler ordering 的排查树。对 PLIC、APLIC direct、IMSIC 分别指出“只一次”和“风暴”的典型不同根因。

**主要考察点：** controller-specific diagnosis、state-machine reconstruction。

---

## Q230. SMP/Timer/Interrupt Review Checklist

**类型：Staff 规范设计**

覆盖拓扑、HSM、coherency、per-hart、IPI/RFENCE、WFI、timebase、RV32 timer、PLIC discovery/state、AIA mode/IMSIC、hotplug/suspend、ordering、错误注入和性能计数；要求定义 legacy PLIC 与 AIA 两套测试矩阵。

**主要考察点：** subsystem architecture review、lifecycle、portability、verification。

### 本批覆盖矩阵

| 能力 | 题目 |
|---|---|
| Hart 拓扑/启动/热插拔 | Q186～Q200 |
| Timer/Counter | Q201～Q207 |
| PLIC | Q208～Q219 |
| AIA/APLIC/IMSIC | Q220～Q229 |
| 综合评审 | Q230 |

---

# Part III-F. H 扩展与虚拟化（Q231～Q270）

## Q231. 探测并启用 H 扩展

**类型：Feature Discovery**

说明 M-mode `misa.H`、DT/ACPI ISA 描述、ELF target 与实际 hart能力的关系；H 可能被 M-mode禁用或 hart 不一致。设计 HS hypervisor 的启动检查、secondary validation 和无 H 时拒绝/软件虚拟化 fallback。

**主要考察点：** H extension discovery、M-mode control、heterogeneous hart。

---

## Q232. HS/VS/VU 与 V 状态

**类型：特权模型**

画出 M、HS、U(V=0)、VS、VU(V=1) 的关系。解释 HS 不是新 privilege number、两阶段翻译何时生效、普通 S OS 为何可不改运行在 HS 或 VS，以及 guest trap 可能到 VS/HS/M 的路径。

**主要考察点：** virtualization mode、privilege layering、two-stage activation。

---

## Q233. HS-mode Trap Vector

**类型：现场 Coding**

设计 `stvec` 入口同时处理 host S/U 与 guest VS/VU 退出。最早读取哪些 status 位判断 `SPV/SPVP`，怎样在破坏 guest GPR 前保存，如何选择 host emergency stack，并避免把 host page fault误送 guest emulator。

**主要考察点：** unified HS entry、trap provenance、safe stack selection。

---

## Q234. Hypervisor Exception Frame

**类型：上下文设计**

在普通 GPR/`sepc/sstatus/scause/stval` 外，选择保存 `hstatus/htval/htinst/hgatp`、VS CSRs 与 virtual interrupt state。区分每次 exit 必存、vCPU 常驻、lazy state和 debug dump，保持汇编/C offsets 可验证。

**主要考察点：** HS trap metadata、vCPU frame、lazy context partitioning。

---

## Q235. 区分 Host Fault 与 Guest Exit

**类型：故障分析**

给定相同 `scause` page fault，要求结合 `hstatus.SPV`、guest-page-fault cause、`htval/htinst`、PC 所属范围判断 host bug、guest stage-1 fault、G-stage fault或 MMIO trap；错误归属为何会破坏隔离。

**主要考察点：** fault attribution、guest vs host、security containment。

---

## Q236. 第一次进入 VS-mode Guest

**类型：现场设计**

准备 guest GPR、`vsstatus/vsepc/vstvec/vsscratch/vsatp`、`hstatus`、`hgatp`、timer/interrupt 后通过 HS `sret` 进入 VS。要求说明目标 VS/VU 选择、虚拟中断窗口、G-stage fence 和进入前 host 敏感寄存器清理。

**主要考察点：** guest bootstrap、SRET virtualization semantics、state sanitization。

---

## Q237. 完整 Guest Exit 汇编路径

**类型：现场 Coding**

保存 guest GPR 与最小 HS/VS trap state，切 host stack/address context，调用 exit dispatcher；返回 guest 时反向恢复。处理 exit 中再发生 host Trap、vCPU 被调度走和不能提前覆盖 frame base。

**主要考察点：** guest/host assembly boundary、nested host fault、scheduler integration。

---

## Q238. vCPU Context 状态集合

**类型：系统设计**

列出 GPR、VS CSRs、H controls、G-stage/VMID、timer、AIA、FPU/Vector、PMU、debug/stateen 和 SBI emulation state。按 always-save、dirty/lazy、per-VM、per-vCPU、per-pCPU 分类并说明迁移同步。

**主要考察点：** context ownership model、save policy、migration correctness。

---

## Q239. `hstatus` 关键字段审查

**类型：CSR 深挖**

解释 SPV/SPVP、GVA、HU、VTW、VTSR、VTVM、VSXL 及实现扩展相关字段对 Trap/guest 行为的影响。给出错误配置，判断 guest WFI/SRET/SFENCE、HLV/HSV 和 fault address 标记会怎样。

**主要考察点：** `hstatus`、virtualization traps、guest provenance。

---

## Q240. `hedeleg/hideleg` 设计

**类型：Trap Routing**

为 guest OS 决定哪些异常/中断直接进 VS、哪些强制 exit HS。讨论 guest page fault为何不能简单委托、virtual supervisor timer/external interrupt、illegal/virtual instruction、调试和安全策略；验证只写可委托 WARL bits。

**主要考察点：** guest delegation、exit frequency、security policy。

---

## Q241. Virtual Instruction Exception

**类型：异常解码**

区分 illegal instruction 与 virtual instruction exception；列举受 VTVM/VTSR/VTW、stateen 或虚拟化规则影响的操作。设计取指/`htinst` 辅助解码、模拟或注入 guest exception，避免把真正 guest bug 无限重试。

**主要考察点：** virtual-instruction trap、emulation、policy controls。

---

## Q242. `HFENCE.VVMA/GVMA` 语义

**类型：TLB 虚拟化**

分别说明 VS-stage 与 G-stage translation cache 的失效范围、ASID/VMID 操作数、local-hart scope 和前置页表 Store ordering。特别说明 `HFENCE.VVMA` 的 `rs1` 按 guest virtual address 解释，而 `HFENCE.GVMA` 为覆盖扩展 GPA 宽度而把 `rs1` 编码为 **guest physical address 右移 2 位**；设计 vCPU 迁移及 remote HFENCE 协议。

**主要考察点：** two-stage TLB、selective invalidation、remote coordination。

---

## Q243. `vsatp` 与 Guest Stage-1

**类型：地址翻译**

guest 写 `satp` 实际影响 `vsatp` 的条件是什么；当 VTVM trap 时 HS 如何验证/虚拟化。讨论 guest ASID、VS-stage `SFENCE.VMA`/`HFENCE.VVMA`、shadow paging fallback 和 nested guest 的处理。

**主要考察点：** VS translation、guest CSR virtualization、ASID namespace。

---

## Q244. `hgatp`、VMID 与 G-stage

**类型：地址空间设计**

解释 MODE/VMID/PPN，探测 VMIDLEN，比较 Bare/Sv39x4/Sv48x4/Sv57x4。说明写 `hgatp` 不是完整的旧 translation flush，per-VM root、VMID allocator和 schedule-in sequence 如何组织。

**主要考察点：** `hgatp`、G-stage modes、VMID、context switch。

---

## Q245. 手工完成 VS + G 两阶段翻译

**类型：白板推演**

给定 GVA、`vsatp` 页表和 `hgatp` 页表，先得到 GPA，再得到 SPA；指出 VS page-table walk 本身的 GPA 访问也受 G-stage约束。逐层判断 fault 应报告给 VS 还是 HS。

**主要考察点：** nested page walk、GVA/GPA/SPA、fault attribution。

---

## Q246. x4 Root Page Table

**类型：体系结构细节**

解释 G-stage x4 mode 为什么扩展 guest physical address top bits、root table 对齐/大小与普通 Sv39 不同。审查只分配 4 KiB root 的 hypervisor，推演越界 page walk 和安全后果。

**主要考察点：** Sv*x4 root geometry、alignment、allocator contract。

---

## Q247. G-stage PTE 设计

**类型：页表设计**

为 guest RAM、只读 firmware、MMIO hole、共享页构造 G-stage PTE，处理 U bit等 H 特有解释、A/D、PBMT/NAPOT（若支持）、huge page 和 execute permission。说明 host mapping 与 guest mapping权限不能混为一层。

**主要考察点：** G-stage permissions、memory types、defense in depth。

---

## Q248. Guest-Page Fault 解码

**类型：故障分析**

对 instruction/load/store guest-page fault，结合 `stval/htval/htinst/hstatus.GVA` 还原 GVA/GPA 和 faulting access。讨论 `htval` 编码/有效性、pseudo instruction 信息、取指跨页和无法重建时的保守处理。

**主要考察点：** guest fault CSRs、address reconstruction、robust decode。

---

## Q249. G-stage MMIO Trap 模拟

**类型：设备模拟**

通过在 G-stage 中将 GPA MMIO 留空触发 guest-page fault，解码 Load/Store/AMO、宽度、signedness、寄存器和指令长度；调用 device model 后更新 GPR/EPC。处理 misaligned、compressed、跨页、LR/SC 和 side effect 失败语义。

**主要考察点：** MMIO emulation、instruction decoder、precise virtualization。

---

## Q250. `HLV/HLVX/HSV` 的使用边界

**类型：Hypervisor 指令**

说明 HS 访问 guest virtual memory的 translation/permission语义，`HLVX` 与 execute permission的关系，HU 控制和 Trap 行为。设计 copy_to/from_guest 的 fault fixup、SUM-like policy与防止 host confused-deputy。

**主要考察点：** guest-memory access、fault recovery、privilege validation。

---

## Q251. Guest CSR Trap 与模拟

**类型：虚拟设备 / CSR**

选择直接虚拟化、Trap-and-emulate 或禁止的 CSR：counter、timer、AIA、PMU、envcfg、stateen、custom CSR。要求 WARL 行为与 guest 可见 ISA 一致，迁移时不暴露目标不支持的 bits。

**主要考察点：** CSR virtualization、virtual ISA contract、migration compatibility。

---

## Q252. Guest `WFI` Trap

**类型：调度设计**

通过 VTW 等控制 guest WFI，决定 block vCPU、yield 或直接执行。处理 pending virtual interrupt 在检查与 sleep 间到达、spurious wake、oversubscription 和 malicious WFI loop；定义 waitqueue 原子协议。

**主要考察点：** WFI virtualization、lost wakeup、vCPU scheduling。

---

## Q253. Virtual Timer

**类型：Timer 虚拟化**

使用 `htimedelta`、VS time/counter权限、`vstimecmp` 或软件 timer 模拟 guest time。处理 vCPU suspend/migrate、time dilation、deadline 已过、nested virtualization和稳定时钟 ABI。

**主要考察点：** virtual time、Sstc、migration、timer injection。

---

## Q254. 注入 Virtual Interrupt

**类型：现场设计**

比较设置 `hvip`、AIA guest interrupt file 与让 VS controller model pending。要求处理优先级、`vsie/vsstatus.SIE`、already pending、level deassert、vCPU不在当前 hart 和 wakeup；防止重复/丢失注入。

**主要考察点：** virtual interrupt state、injection/wakeup、level semantics。

---

## Q255. AIA Guest External Interrupt

**类型：高级虚拟化**

配置 `hgeip/hgeie`、`hstatus.VGEIN` 与 IMSIC guest file，使 device MSI 直达 scheduled vCPU。设计 schedule-out 保存/切换、file清理、guest number复用和没有 guest file 时的 software injection fallback。

**主要考察点：** AIA virtualization、guest interrupt file、resource reuse。

---

## Q256. vCPU 跨物理 Hart 迁移

**类型：上下文 / SMP**

列出 `HFENCE`、VMID、timer、AIA guest file、FPU/Vector owner、PMU、debug、pending interrupt 和 per-hart residency bitmap 的迁移顺序。要求旧 hart ack 完全退出后才能在新 hart运行同一 vCPU。

**主要考察点：** migration serialization、per-hart hardware state、TLB safety。

---

## Q257. VMID 分配与回绕

**类型：资源管理**

探测 VMID 位宽，设计 generation allocator与 rollover HFENCE.GVMA。处理 VM 在多个 hart运行、离线 hart stale G-stage entry、nested VMID和 live migration；证明旧 VM 的 translation 不会污染新 VM。

**主要考察点：** VMID lifecycle、global flush、generation proof。

---

## Q258. G-stage Mapping 更新

**类型：页表并发**

改变 GPA→SPA 或权限时，设计 invalid→fence→new mapping→fence 序列、remote HFENCE、vCPU停顿与物理页回收。区分权限放宽/收紧、A/D 更新和 PBMT变化，防止 DMA/IOMMU 仍访问旧页。

**主要考察点：** G-stage break-before-reuse、remote invalidation、DMA coordination。

---

## Q259. Dirty Logging

**类型：Live Migration**

比较清 D 位、write-protect fault、硬件 dirty tracking 与 IOMMU dirty log。要求处理并发 vCPU Store、TLB cached writable、huge page拆分、DMA写和迭代迁移收敛；定义原子取清 bitmap。

**主要考察点：** dirty tracking、TLB synchronization、DMA visibility、migration。

---

## Q260. Huge Page 与两阶段组合

**类型：性能 / MMU**

VS 和 G-stage 页大小组合决定有效 TLB coverage。要求推演 guest huge page落在碎片化 host mapping、拆分/合并、dirty logging 和 NUMA placement；选择性能与内存碎片/迁移粒度的平衡。

**主要考察点：** two-stage page size、TLB reach、memory management tradeoff。

---

## Q261. G-stage Demand Paging

**类型：系统设计**

guest 首次访问触发分配 host page、加载 backing store、安装 PTE并重试。处理同 GPA 多 vCPU fault 合并、I/O error、zero-page/COW、OOM、锁顺序和恶意 fault storm。

**主要考察点：** concurrent fault handling、retry semantics、resource isolation。

---

## Q262. Device Passthrough 生命周期

**类型：安全虚拟化**

按 bind→map DMA→route interrupt→run→quiesce→unmap→reset→reassign 设计流程。要求设备 reset 真正清内部 DMA/MSI/ATS state，guest teardown 时先阻断新请求，再等待 in-flight 完成。

**主要考察点：** device ownership、quiesce/reset、teardown ordering。

---

## Q263. IOMMU 与 G-stage 一致性

**类型：地址翻译综合**

比较 CPU G-stage 与 IOMMU translation 可共享/独立的页表及失效命令。更新 guest memory 时协调 HFENCE、IOTLB、device ATS 和 page lifetime；解释只做其中一种 invalidation 的安全漏洞。

**主要考察点：** CPU/device translation coherence、invalidation completion。

---

## Q264. Guest FPU/Vector Context

**类型：扩展虚拟化**

区分 host task、vCPU、guest process 三层 FS/VS ownership。设计 trap/lazy restore、VLEN暴露、guest migration目标兼容和 host kernel vector使用；处理嵌套 exit 时不能覆盖 guest state。

**主要考察点：** nested extension ownership、virtual ISA、migration compatibility。

---

## Q265. `mstateen/hstateen` 权限门

**类型：安全 CSR**

解释 state-enable 层次如何限制 S/HS/VS 对某类扩展状态访问，以及 illegal 与 virtual-instruction exception 的区别。为 AIA、CSR indirect、context state设计最小暴露策略和 context switch。

**主要考察点：** Smstateen/Ssstateen、extension isolation、trap classification。

---

## Q266. PMU 虚拟化

**类型：性能监控**

虚拟化 cycle/instret/HPM counter、event selector、overflow interrupt与 SBI PMU。比较 passthrough、time-multiplex、software accumulate；防止 guest 观察 host、侧信道或 monopolize counters，并支持 vCPU迁移。

**主要考察点：** PMU context、SBI PMU、side channel、multiplexing。

---

## Q267. 为 Guest 实现 SBI

**类型：虚拟固件接口**

HS hypervisor 为 VS guest提供 Base/TIME/IPI/RFENCE/HSM/SRST/PMU 的虚拟实现。说明哪些可本地完成、哪些映射 vCPU、哪些绝不能盲目转发到 M-mode OpenSBI；验证 guest hart mask 与地址参数。

**主要考察点：** virtual SBI、namespace translation、input validation、layering。

---

## Q268. Nested Virtualization

**类型：高级系统设计**

L0 运行 L1 hypervisor，L1 再运行 L2 guest。讨论虚拟 H CSRs、嵌套 two-stage composition、shadow `hgatp`、HFENCE 拦截、VMID、timer/AIA和 performance。要求明确硬件未提供完整 nested assist 时的软件边界。

**主要考察点：** nested H virtualization、translation composition、CSR emulation。

---

## Q269. Confidential VM 与规范成熟度

**类型：安全架构判断**

设计题要求候选人先区分已 ratified H/PMP/IOMMU/AIA 与 evolving confidential-computing/TEE proposal或 vendor extension。定义 threat model、内存加密/完整性、attestation、debug/DMA/firmware TCB，不得把 Arm Realm/TrustZone 名称直接移植。

**主要考察点：** standards maturity、threat model、TCB、honest portability。

---

## Q270. H 扩展 Hypervisor Review Checklist

**类型：Staff 综合评审**

覆盖 HS entry、状态归属、`hstatus`、delegation、VS/G-stage page walk、x4 root、HFENCE/VMID、fault decode/MMIO、timer/interrupt/AIA、migration、IOMMU、SBI、扩展 context、nested与安全清理。

**主要考察点：** hypervisor architecture review、isolation proof、lifecycle test matrix。

### 本批覆盖矩阵

| 能力 | 题目 |
|---|---|
| H 模式/Entry/Context | Q231～Q241 |
| 两阶段翻译/HFENCE | Q242～Q250 |
| CSR/Timer/Interrupt/AIA | Q251～Q255 |
| VMID/Migration/内存管理 | Q256～Q261 |
| 设备/扩展/SBI/Nested/安全 | Q262～Q270 |

---

# Part III-G. M-mode、OpenSBI、SBI、安全与电源（Q271～Q320）

> 本 Part 同时考察 SBI 规范接口和 OpenSBI 实现。题目列出某个 SBI 扩展不表示所有 OpenSBI tag、平台后端或虚拟 SBI provider 都支持它；作答时应先说明目标 SBI 版本、Base probe 结果和 implementation/platform gate，再讨论实现细节。

## Q271. OpenSBI 在软件栈中的职责

**类型：架构边界**

说明 OpenSBI 是运行于 M-mode 的 SBI 参考实现/库，不是完整功能 bootloader、通用 hypervisor 或天然 TEE；同时说明其 firmware images 可以装载、携带或移交 payload。划分 BootROM、OpenSBI、U-Boot/EDK2、S/HS OS 的职责，说明 HS 为 VS guest 提供 SBI 时与 OpenSBI 的层级差异。

**主要考察点：** firmware layering、SBI provider、TCB scope、职责边界。

---

## Q272. OpenSBI Entry Contract

**类型：Early Boot**

前级通常传 `a0=hartid`、`a1=8-byte aligned FDT address`，但其余状态依 firmware type/platform contract。要求定义 privilege/XLEN/ISA、PMP、interrupt、memory、other-hart、relocation 和 next-stage metadata 的输入验证。

**主要考察点：** OpenSBI boot ABI、platform assumptions、defensive validation。

---

## Q273. `mscratch` 与 `struct sbi_scratch`

**类型：OpenSBI 核心设计**

解释每 hart scratch 如何承载 firmware/next-stage 信息、临时栈与扩展 offset，为什么 OpenSBI 运行时要求 `mscratch` 指向有效 scratch。设计 scratch 分配、hartid 映射、warmboot 复用和 M-mode Trap 第一条指令。

**主要考察点：** per-hart firmware state、`mscratch`、extensible scratch layout。

---

## Q274. Coldboot Lottery 与 Warmboot Rendezvous

**类型：多 Hart 启动**

多个 hart 同时进入 OpenSBI 时，设计原子 coldboot winner、其余 warmboot wait。winner 初始化全局子系统后 release，各 hart完成 per-hart init；处理 boot hart失败、non-coherent cache、WFI spurious wake 和启动超时。

**主要考察点：** boot lottery、global/per-hart initialization、publication。

---

## Q275. OpenSBI 初始化顺序

**类型：系统设计**

为 scratch、heap、console、domain/PMP、interrupt/IPI/timer、HSM、PMU、ecall extensions、platform finalization 和 next-stage handoff 排序。要求指出哪些组件用于错误输出但又依赖前置组件，设计可回滚/可诊断的 partial-init path。

**主要考察点：** dependency graph、early diagnostics、failure containment。

---

## Q276. Platform Operations 与通用核心边界

**类型：OpenSBI Porting**

为新 SoC 划分通用 libsbi、platform driver/hook、DT match/quirk。涵盖 early/final init、hart start/stop/suspend、timer/IPI/reset、console、PMU、PMP/domain 与 vendor extension；禁止把板级地址散落到通用代码。

**主要考察点：** platform abstraction、portability、maintainability、hook contract。

---

## Q277. Generic Platform 与 FDT Fixup

**类型：设备树 / 移植**

解释 Generic platform 如何靠 FDT 适配 emulator/FPGA/board，并通过 match/override 处理 quirk。设计 FDT 地址/大小/重叠验证、reserved-memory、CPU status、ISA/SBI node fixup 和传给 next stage 前的 cache/ownership。

**主要考察点：** data-driven platform、FDT security、firmware fixup。

---

## Q278. `FW_DYNAMIC/FW_JUMP/FW_PAYLOAD`

**类型：OpenSBI Firmware 选择**

比较三种 firmware 如何获得 next address/mode/arg、是否嵌入 payload、适合的前级 loader。审查错误选择导致 entry 不匹配、FDT 丢失或镜像重叠；说明 firmware 类型不同但 libsbi 初始化主干相同。

**主要考察点：** OpenSBI build modes、boot-chain integration、image layout。

---

## Q279. Next-stage Handoff

**类型：现场设计**

OpenSBI 进入 S/HS next stage 前，设置 `mstatus.MPP/mepc`、delegation、PMP、counter access、`satp`、PMP-visible address，并传 hartid/FDT。要求清理 M-mode敏感寄存器、处理 next_mode非法和 entry不可执行。

**主要考察点：** M→S/HS transition、payload ABI、security sanitization。

---

## Q280. OpenSBI Domain 模型

**类型：安全分区**

解释 ROOT 与非 ROOT domain 的 assigned/possible harts、boot hart、next-stage 和 memory regions。设计多个 S-mode partition 的启动，确保每个 domain只看到自己的设备描述，并限制跨 domain SBI IPI/RFENCE。

**主要考察点：** OpenSBI domains、static partitioning、resource namespace。

---

## Q281. Domain Memory Region 设计

**类型：权限模型**

用 base/order/flags 表达 firmware、RAM、MMIO 和共享区域；处理 region 重叠、优先级、自然对齐、全地址空间 default region 与有限 PMP entry。要求用 adversarial addresses 验证 read/write/execute/M-mode 权限。

**主要考察点：** region algebra、PMP realization、overlap/security proof。

---

## Q282. 从 Domain 生成 PMP 配置

**类型：现场推演**

把 region 列表编译为 TOR/NAPOT PMP entries，考虑 grain、entry数量、priority、L/Smepmp、firmware self-protection。若无法精确表达，必须 fail closed；设计各 hart domain switch/boot 时的编程序列。

**主要考察点：** policy-to-hardware compilation、PMP limits、fail-closed behavior。

---

## Q283. Domain Hart 启动与跨域服务

**类型：生命周期 / 安全**

启动属于不同 domain 的 hart，验证 caller 是否有权、目标 entry/opaque 位于允许区域；IPI/RFENCE hart mask不得跨域。处理 hart reassignment、共享 timer/controller 和一个 domain 崩溃时的隔离策略。

**主要考察点：** domain authorization、hart ownership、SBI isolation。

---

## Q284. SBI Ecall Dispatch Framework

**类型：OpenSBI 核心**

从 M-mode 的 supervisor ecall Trap 读取 EID/FID/args，查 extension、调用 handler、写 `a0/a1`、将 `mepc` 推进 4 字节并返回。要求区分 S/HS caller 与非法 Trap 来源，遵守“除 `a0/a1` 外全部 GPR 由 SBI 被调用方保持”的二进制约定，并处理 handler 睡眠/嵌套限制和 unknown function 错误。

**主要考察点：** SBI dispatcher、Trap frame ABI、register preservation、extension registration、error path。

---

## Q285. SBI Base 与 Legacy Compatibility

**类型：版本兼容**

使用 Base 查询 spec version、implementation ID/version、probe extension 和 machine IDs。比较 legacy v0.1 的单值返回/extension encoding，设计 OS feature gating；禁止用 OpenSBI implementation version代替 SBI spec capability probe。

**主要考察点：** SBI versioning、capability discovery、legacy ABI。

---

## Q286. SBI TIME 的 M-mode 实现

**类型：OpenSBI 驱动**

实现 set_timer：编程平台 timer、清/注入 S timer pending、处理 deadline过去和 64 位值。比较有 Sstc 时允许 S-mode直写与 trap emulation；多 domain/guest 的 timer owner如何隔离。

**主要考察点：** timer backend、interrupt injection、Sstc fast path。

---

## Q287. SBI IPI 的 M-mode 实现

**类型：OpenSBI SMP**

验证 hart mask与 domain，给目标 per-hart event入队、排序后触发 platform IPI；远端 M-mode handler清 source、交换 event bits并执行回调。处理 coalescing、self-IPI、offline hart和发送失败。

**主要考察点：** IPI backend、event queue、domain validation、ordering。

---

## Q288. SBI RFENCE 的 M-mode 实现

**类型：远程调用框架**

将 remote fence 请求发送给目标 hart。SBI v3.0 对 `SBI_SUCCESS` 的规范描述是 IPI 已成功发送到全部目标 hart；实现即使内部提供更强保证，也不能要求可移植 caller 默认依赖未写入规范的保证。若调用方要据此回收页表或物理页，须提供与该需求匹配的完成/ack 协议；provider 还必须保证 request data 在远端消费前一直有效。设计多个 sender 合并、hart stop race、range/ASID validation 和 timeout，并避免在持有会被远端路径获取的锁时等待。

**主要考察点：** RFENCE return contract、remote execution、deadlock avoidance、completion、batching。

---

## Q289. SBI HSM 后端

**类型：Hart Lifecycle**

把标准 HSM 状态机映射到 platform power controller/mailbox。验证 start address/domain，原子转换 START_PENDING，warmboot完成后才 STARTED；stop/suspend时保存必要 scratch，并处理不可启动 hart与 platform error。

**主要考察点：** HSM state machine、platform power control、linearization。

---

## Q290. SBI System Reset（SRST）

**类型：电源 / 安全**

处理 shutdown、cold/warm reboot 与 reset reason；验证 type/reason、协调其他 hart、quiesce console/DMA、同步持久日志后调用 platform reset。成功调用不返回；失败返回不得留下半停机系统。

**主要考察点：** SRST、system quiesce、non-returning success、failure recovery。

---

## Q291. SBI System Suspend（SUSP）

**类型：系统低功耗**

设计 suspend-to-RAM：验证其他 hart已 STOPPED、resume address可执行且允许、保存 firmware/platform state、下电；唤醒后从指定 S-mode地址和 opaque恢复。说明成功 suspend/resume 不从原 ecall正常返回。

**主要考察点：** SUSP entry criteria、resume ABI、domain-scoped system view。

---

## Q292. SBI CPPC

**类型：性能/电源接口**

解释 CPPC extension 如何抽象 platform performance registers，设计 probe/read/write与 64 位值、fast/slow channel、per-hart/global policy。区分 desired performance请求与频率保证，处理异构 hart和 suspend恢复。

**主要考察点：** CPPC abstraction、performance policy、heterogeneity。

---

## Q293. SBI PMU

**类型：性能监控**

实现 counter discovery、event匹配、configure/start/stop/read和 firmware counter。处理硬件 counter 数量不同、overflow/width、mhpmevent权限、多个 S-mode client、HSM suspend与 guest虚拟化。

**主要考察点：** PMU virtualization、event mapping、counter lifecycle。

---

## Q294. SBI Debug Console（DBCN）

**类型：调试接口**

比较 DBCN 与 legacy putchar/getchar；验证物理 buffer 地址跨 XLEN参数、domain access、长度/overflow和 partial write。设计 panic-safe best-effort路径，避免 console lock、DMA/coherency和恶意大请求拖死 M-mode。

**主要考察点：** DBCN、physical-buffer validation、panic console、DoS resistance。

---

## Q295. SBI Steal-time Accounting（STA）

**类型：虚拟化计时**

解释 supervisor共享内存中的 steal-time 信息用途，设计 per-hart shared-memory设置、物理地址/domain验证、原子更新与 migration。区分 bare-metal OpenSBI实现和 hypervisor向 VS guest提供的实现价值。

**主要考察点：** STA、shared memory ABI、accounting consistency。

---

## Q296. SBI Supervisor Software Events（SSE）

**类型：异步事件**

按 SBI SSE 状态机设计 event discovery、`UNUSED→REGISTERED→ENABLED→RUNNING` 生命周期、handler 注册、enable/disable、delivery 和 completion；区分 local/global event 与优先级，说明 SSE handler 会抢占普通 Trap/interrupt 处理，completion 后如何恢复被打断状态。共享物理缓冲区仅在具体 attribute 定义需要时使用，不能把它笼统描述成 SSE 的事件传输机制；还要处理嵌套策略、CPU hotplug 和恶意 S-mode 不完成事件的 backpressure。

**主要考察点：** asynchronous SBI services、event lifecycle、backpressure。

---

## Q297. SBI Firmware Features（FWFT）

**类型：现代 SBI 扩展**

以 SBI v3.0 中锁定的 FWFT feature ID 和语义为准：先用 SBI Base probe 确认 FWFT extension，再通过 FWFT `get/set` 判断并管理 misaligned exception delegation、landing pad、shadow stack、double trap、PTE A/D hardware updating、pointer masking 等能力；不得因规范已列出某 feature 就假定目标 OpenSBI tag 与平台后端已实现。区分 local/global scope、lock 语义、hart reset 和 non-retentive suspend 恢复。

**主要考察点：** FWFT、feature policy、scope/lifecycle、probe-before-use。

---

## Q298. SBI Debug Triggers（DBTR）

**类型：调试虚拟化**

为 S/HS/VS 共享有限 hardware triggers：探测数量、设置 shared memory、install/update/uninstall/enable/disable。验证 `tdata*`、禁止 M/dmode滥用、处理 trigger chain、任务迁移和异构 hart数量。

**主要考察点：** DBTR、trigger multiplexing、shared-memory validation、privilege safety。

---

## Q299. SBI Message Proxy（MPXY）

**类型：固件消息通道**

解释 channel/attribute/capability、per-hart shared memory、send/response/notification。设计对 RPMI 或平台管理固件的代理，验证长度、对齐、timeout和协议错误；防止混用同一功能的 dedicated SBI extension 与 MPXY接口。

**主要考察点：** MPXY、shared-memory transport、protocol abstraction、timeout security。

---

## Q300. SBI Nested Acceleration（NACL）

**类型：嵌套虚拟化接口**

解释 L0 SBI provider与 L1 hypervisor用 shared memory批处理 H CSR/HFENCE 的目标。设计 feature probe、shared-memory布局/同步、dirty state和 explicit sync；不支持时退回逐次 trap-and-emulate。

**主要考察点：** NACL、nested acceleration、batching、fallback correctness。

---

## Q301. SBI 参数与物理地址验证

**类型：安全代码审查**

建立统一 helper 检查 EID/FID、reserved flags、hart/domain、物理地址高低半、alignment、`base+size` overflow、PMP/domain 权限和共享内存生命周期。按具体扩展与锁定的 SBI 版本区分 `INVALID_PARAM`、`INVALID_ADDRESS`、`BAD_RANGE`、`DENIED`、`INVALID_STATE`、`NO_SHMEM`、`TIMEOUT`、`NOT_SUPPORTED` 等错误；不能用一套臆造的通用 errno 映射覆盖所有 FID。

**主要考察点：** untrusted S-mode input、integer overflow、error taxonomy、TOCTOU。

---

## Q302. Ecall Handler 的可重入性

**类型：并发 / Trap**

SBI handler 执行时 M-mode interrupt/RNMI 到达，或 handler向其他 hart同步等待。设计 per-hart scratch frame、M-mode stack nesting、锁层级、中断开关和不可重入 console；禁止持 global lock 等待远端回调。

**主要考察点：** M-mode reentrancy、lock ordering、nested trap、deadlock。

---

## Q303. Delegation 与 M-mode Trap Policy

**类型：OpenSBI 架构**

决定哪些 exception/interrupt委托 S/HS，哪些由 OpenSBI保留/模拟。考虑 misaligned、illegal CSR、timer/IPI、PMP fault、M-mode ecall、double trap和 platform RAS；用 WARL read-back验证委托结果。

**主要考察点：** delegation policy、firmware service boundary、fault ownership。

---

## Q304. HSM Hart Suspend

**类型：Per-Hart Power**

区分 retentive 与 non-retentive suspend：retentive 模式不使用 `resume_addr`，唤醒后保留各 privilege mode 的 register/CSR 状态，并从原 `hart_suspend` 调用返回；`opaque` 只在 non-retentive resume 时有规定用途。non-retentive 模式须验证 `resume_addr/opaque` 与 domain 权限，唤醒后按 warm-boot 类路径重建状态，并以 `satp=0`、`sstatus.SIE=0`、`a0=hartid`、`a1=opaque` 跳到 `resume_addr`，其余寄存器不可假定。还要设计 HSM 状态转换、platform suspend 调用、scratch/PMP/interrupt/timer/local state 恢复，以及错误或立即唤醒路径。

**主要考察点：** HSM suspend、retention contract、resume ordering。

---

## Q305. System Suspend 多 Hart 协调

**类型：系统电源设计**

S-mode先 offline其他 hart并停止设备，最后 hart调用 SUSP。OpenSBI再次验证状态，保存 M-mode和 platform context、Cache/coherency、wake source；resume时先恢复可访问内存和 Trap能力再执行 C。

**主要考察点：** system power state、last-hart protocol、early resume constraints。

---

## Q306. Warm Reset 与 Cold Reset

**类型：生命周期**

定义哪些 CPU/SoC state在 warm reset 保留：DRAM、Cache、PMP、interrupt controller、IOMMU、hart scratch、reset reason。启动代码必须主动建立确定状态，避免沿用攻击者/旧内核留下的权限、pending IRQ或 dirty cache。

**主要考察点：** reset contract、residual state、security reset hygiene。

---

## Q307. Resume Trampoline

**类型：现场设计**

在 MMU off、普通栈/DRAM可能尚不可用的入口，用位置无关汇编识别 hart、建立临时栈、恢复 interconnect/clock/memory、`mtvec/mscratch/PMP`，再进入 C。要求无未恢复设备依赖且支持失败回退 reset。

**主要考察点：** minimal resume environment、PIC trampoline、recovery path。

---

## Q308. Cache/Coherency 的电源 Handoff

**类型：SoC 平台设计**

下电前处理 dirty cache、共享数据发布、离开 coherency；上电后按平台顺序恢复。区分 architectural fence、Zicbom和 vendor cache controller，定义 firmware/OS ownership，避免双方都 flush或都假定对方已处理。

**主要考察点：** power-domain coherency、cache ownership、platform operations。

---

## Q309. OpenSBI Panic 与 Console

**类型：可靠性**

设计 M-mode fatal path：保存 `mepc/mcause/mtval/mstatus` 和 GPR、切 emergency stack、选主 hart、停其他 hart、DBCN/platform console有界输出、敏感信息策略和 watchdog/reset。不得递归格式化或等待坏锁。

**主要考察点：** firmware crash path、minimal dependencies、secure diagnostics。

---

## Q310. M-mode RAS 与错误归属

**类型：RAS / 故障分析**

处理 ECC、bus error、platform error interrupt/RNMI：判断错误属于 firmware、S-domain、guest或设备；采集可用记录、隔离 page/device/hart、通知 S-mode或 reset。区分 precise access fault 与异步不可恢复错误。

**主要考察点：** RAS attribution、firmware-first policy、containment。

---

## Q311. OpenSBI/Firmware 安全更新

**类型：系统设计**

设计 A/B image、签名、anti-rollback、断电恢复、版本兼容和 recovery mode。更新 M-mode firmware前 quiesce所有 hart/device，下一次 boot验证；SBI ABI 必须保持或通过 capability probe演进。

**主要考察点：** secure update、rollback resilience、ABI compatibility、availability。

---

## Q312. Secure Boot 到 OpenSBI 的信任链

**类型：安全架构**

从 immutable root验证 OpenSBI，再验证 next stage/FDT/config。讨论测量、密钥/rollback counter、debug lifecycle、FDT篡改、payload地址重叠和 TOCTOU；OpenSBI handoff前以 PMP保护自身和密钥材料。

**主要考察点：** chain of trust、measured/verified boot、configuration integrity。

---

## Q313. OpenSBI 自保护与最小 M-mode TCB

**类型：安全评审**

用 PMP/Smepmp/domain阻止 S-mode读写执行 firmware；移除不必要 SBI/vendor接口，验证所有物理指针，限制 console/debug/PMU侧信道。分析 M-mode bug为何可绕过所有 S/HS隔离，并制定 fuzz/audit边界。

**主要考察点：** TCB minimization、PMP hardening、attack surface、least privilege。

---

## Q314. Speculation 与特权边界

**类型：侧信道 / 微架构**

在 SBI entry/return、PMP切换、domain切换和 secret-dependent handler 中评估 Spectre-class泄漏。区分架构 fence与实现提供的 speculation mitigation，使用标准扩展/平台文档/errata而非虚构通用指令；清理可见寄存器和共享预测状态。

**主要考察点：** speculative leakage、implementation-specific mitigation、boundary sanitization。

---

## Q315. RISC-V TEE 不能照搬 TrustZone

**类型：架构判断**

要求设计者明确基线 RISC-V没有统一 Secure/Non-secure world。比较 PMP/Smepmp静态分区、OpenSBI domain、M-mode monitor、独立安全 hart/设备和 evolving TEE/confidential-computing方案；从 threat model选择而非声称“OpenSBI=TF-A”。

**主要考察点：** honest architecture mapping、TEE threat model、hardware roots of isolation。

---

## Q316. Errata 与 Runtime Alternatives

**类型：跨核可移植性**

根据 `mvendorid/marchid/mimpid`、DT errata和 feature probe选择 workaround。设计 early/static/runtime patch，执行 remote `FENCE.I`，验证 secondary/hotplug hart兼容；禁止仅凭 vendor ID 或字符串误匹配。

**主要考察点：** errata discovery、alternative patching、heterogeneous validation。

---

## Q317. OpenSBI 性能分析

**类型：性能工程**

测量 ecall entry/exit、timer、IPI/RFENCE、HSM 和 PMU handler 延迟。区分 trap成本、锁争用、MMIO、Cache、hart数量和 console干扰；提供 p50/p99/WCET，并判断某服务应下放 S-mode还是保留 M-mode。

**主要考察点：** firmware profiling、latency distribution、architectural partitioning。

---

## Q318. OpenSBI 真机调试

**类型：Debug 方法**

系统启动前无 OS debugger时，组合 early UART、memory trace buffer、JTAG/debug module、GDB、反汇编/map、CSR/PMP dump和 GPIO trace。处理调试器改变时序、hart停住导致系统死锁、secure production关闭 debug。

**主要考察点：** pre-OS observability、non-intrusive debug、security lifecycle。

---

## Q319. 验证矩阵与故障注入

**类型：测试架构**

建立 Spike/QEMU/RTL/FPGA/真机矩阵，覆盖 RV32/RV64、扩展组合、hart数、PLIC/AIA、Sv39/Sv48、PMP grain、cold/warmboot、SBI conformance。注入 SC失败、IPI丢/合并、timer race、bad FDT、PMP耗尽、RFENCE timeout、suspend唤醒和错误 SBI 地址。

**主要考察点：** layered verification、negative testing、emulator coverage gaps。

---

## Q320. OpenSBI/SBI Staff Review Checklist

**类型：终局综合评审**

候选人应提交覆盖以下方面的 checklist：

- entry、coldboot/warmboot、scratch、relocation和 per-hart stack；
- platform hooks、FDT、firmware types与 next-stage ABI；
- domain/PMP/Smepmp、物理参数验证、跨域 hart mask；
- Base/TIME/IPI/RFENCE/HSM/SRST/PMU/DBCN/SUSP/CPPC；
- NACL/STA/SSE/FWFT/DBTR/MPXY 的 probe 与生命周期，以及其中实际采用共享内存接口的扩展所需的地址、ownership 和同步验证；
- Trap委托、嵌套 M-mode、power/reset、RAS/panic；
- secure boot/update、debug lifecycle、errata、性能与验证矩阵。

要求每一项写明：规范来源、架构/平台假设、owner、失败模式、可观测性和验收测试。

**主要考察点：** Staff ownership、firmware architecture、security/reliability closure。

### 本批覆盖矩阵

| 能力 | 题目 |
|---|---|
| OpenSBI 启动/平台/Firmware | Q271～Q279 |
| Domain/PMP | Q280～Q283 |
| SBI 核心扩展 | Q284～Q294 |
| SBI v3.0 高级扩展 | Q295～Q300 |
| 安全输入/可重入/电源 | Q301～Q310 |
| Secure/Errata/性能/验证 | Q311～Q320 |

---

# Part IV. 题库使用建议与覆盖判断

## 1. 如何抽题

一次 Staff 面试不应追求题量，而应抽 12～18 题纵向深挖：

| 面试方向 | 建议组合 |
|---|---|
| Firmware/OpenSBI | 核心题 4 道 + Q055～Q060 2 道 + Q271～Q320 8 道 |
| RTOS | 核心题 4 道 + Q061～Q105 3 道 + Q146～Q185 7 道 |
| Linux BSP | 核心题 3 道 + MMU/DMA 4 道 + SMP/中断 5 道 + arch entry 3 道 |
| Hypervisor | 核心题 3 道 + Q106～Q145 3 道 + Q231～Q270 9 道 |
| 安全固件/TEE | PMP/domain 4 道 + OpenSBI 6 道 + 虚拟化/IOMMU 4 道 +综合审查 1 道 |

建议至少包含：一题现场 Coding、一题错误代码审查、一题并发 litmus、一题真机故障和一题系统设计。

## 2. 建议能力权重

| 能力 | 权重 |
|---|---:|
| ISA/psABI/ELF/工具链 | 15% |
| Trap/RTOS/Linux arch | 20% |
| RVWMO/原子/SMP | 20% |
| MMU/PMP/Cache/DMA | 20% |
| 中断/虚拟化/OpenSBI | 20% |
| Staff 设计与工程判断 | 5%（同时贯穿所有题） |

## 3. Staff 与 Senior 的分界

Senior 通常能写出工作代码；Staff 还应主动完成以下动作：

- 声明 `-march/-mabi`、privilege、PMA/PMP、coherency和 boot ABI 假设；
- 给出“最弱但充分”的 ordering，并能用 litmus 或 happens-before 证明；
- 识别规范不保证之处，转而查平台文档/DT/ACPI/errata；
- 设计错误路径、跨 hart 生命周期、回滚、观测和故障注入；
- 把一次修复沉淀成统一 arch helper、review checklist 和 conformance test。

## 4. 仍可按岗位追加的专项

320 题已闭合 Staff 级主干。若岗位高度专项，可另增：

- RVV/RVV crypto 与 vectorized libc；
- RVA23/RVB23 profile、server SoC 与 RAS；
- RISC-V Debug 1.0、trace 与外部 debug module；
- IOMMU queue/fault record 的逐字段实作；
- 特定厂商 Cache/NoC/安全岛/电源控制器；
- UEFI/ACPI、kexec、crash kernel 与 firmware update。

---

# Part V. 面试答题方法

面对任何底层题，建议按以下顺序作答：

1. **先定义执行环境：** RV32/RV64、扩展版本、当前 mode、分页模式、ABI、内存属性和 hart 数。
2. **区分四类依据：** 架构保证、标准扩展保证、平台/BootROM 契约、具体实现/errata。
3. **列状态集合：** GPR、CSR、栈、地址空间、interrupt、Cache/TLB、per-hart 与设备 ownership。
4. **画 happens-before：** 谁发布数据、谁通知、谁 acquire、何时允许复用资源。
5. **先写正确版本：** 再讨论 fast path、lazy save、selective fence、huge page 等优化。
6. **说明失败模式：** illegal instruction、fault、timeout、hotplug race、部分完成和不可恢复路径。
7. **给验证方法：** `objdump/readelf`、litmus、QEMU/Spike、RTL、真机 trace、PMU 与 fault injection。

高质量回答不应只说“加一个 fence”或“flush cache”，而应说清：

> 哪个 hart/agent，在什么 address domain，对哪些先前访问和哪些后续访问建立顺序；相关 Cache/TLB/IOMMU 状态何时完成；谁确认完成后才可复用对象或物理页。

---

# Appendix A. AArch64 原题到 RISC-V 主题的迁移说明

1. 原题中的 A64 指令语义全部改为 RV32/RV64 基础指令和模块化扩展，不保留 NZCV、`ADRP`、pair load/store 等 Arm 特有前提。
2. AAPCS64 改为 RISC-V ELF psABI，并新增 `gp/tp`、float ABI、Vector ABI、code model 与 linker relaxation。
3. Arm barrier/shareability 改为 RVWMO、`FENCE` 的 predecessor/successor 与 memory/I/O domain；没有伪造 `ISH/OSH` 对应物。
4. Arm exclusive/LSE 改为 LR/SC、AMO、Zalrsc 与 Zacas。
5. Arm EL1 页表/TLBI 改为 Sv39+、`satp`、`SFENCE.VMA`、Svinval、Svpbmt/Svnapot；安全物理隔离增加 PMP/Smepmp。
6. GIC 改为 PLIC/CLINT/ACLINT 与 AIA 的 APLIC/IMSIC，两代控制器分别成题。
7. EL2 改为 H 扩展 HS/VS/VU、`vsatp+hgatp`、`HFENCE.*` 和 virtual SBI。
8. TF-A/EL3/SMCCC/PSCI 改为 M-mode/OpenSBI/SBI/HSM/SRST/SUSP/Domain；明确 OpenSBI 不是 TrustZone 或 TEE 的同义词。

---

# Appendix B. 规范基线与延伸阅读

本文按 2026 年可获得的已批准规范与 OpenSBI 主线文档组织。正式用于某个产品时，应锁定该产品采用的具体规范版本、ISA 字符串、profile、OpenSBI tag 与平台 errata。

- [RISC-V Unprivileged ISA](https://docs.riscv.org/reference/isa/unpriv/)
- [Zalasr Load-Acquire/Store-Release Extension](https://docs.riscv.org/reference/isa/unpriv/zalasr.html)
- [Zacas Atomic Compare-and-Swap Extension](https://docs.riscv.org/reference/isa/unpriv/zacas.html)
- [RISC-V Privileged Architecture](https://docs.riscv.org/reference/isa/priv/)
- [RISC-V Hypervisor Extension](https://docs.riscv.org/reference/isa/priv/hypervisor.html)
- [RISC-V ELF psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/)
- [RISC-V SBI Specification v3.0](https://docs.riscv.org/reference/sbi/v3.0/index.html)
- [OpenSBI 项目与文档](https://github.com/riscv-software-src/opensbi)
- [OpenSBI Firmware Types](https://github.com/riscv-software-src/opensbi/blob/master/docs/firmware/fw.md)
- [OpenSBI Domain Support](https://github.com/riscv-software-src/opensbi/blob/master/docs/domain_support.md)
- [RISC-V Advanced Interrupt Architecture](https://docs.riscv.org/reference/aia/)
- [RISC-V IOMMU Architecture](https://docs.riscv.org/reference/iommu/)

---

# Appendix C. 编号完整性

- 起始题号：`Q001`
- 结束题号：`Q320`
- 正式题目总数：**320**
- 所有 Part 之间连续编号，无重复、无缺号。
