# RISC-V 64 位寄存器速查：U、S、M 模式

本文按**当前正在执行的特权模式**查寄存器：先看 U-mode，再看 S-mode（包括启用 H 扩展后的 HS/VS），最后看 M-mode。CSR 编号与名称以 RISC-V 官方 [2026-01-20 特权架构 CSR 分配表](https://docs.riscv.org/reference/isa/v20260120/priv/priv-csrs.html)为基准；通用寄存器的 ABI 名称以官方 [ELF psABI](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)为准。本文适用于 **RV64**，所以不列仅供 RV32 使用的 `*h` 高半部 CSR。

**先记住三条边界：**

1. `x0～x31` 和 `pc` 是 hart 的执行状态，U/S/M **共用同一套**；进入 trap 不会自动复制出一套 S 或 M 通用寄存器。`f0～f31`、`v0～v31` 同理，存在与否取决于扩展。
2. CSR 的 12 位编号中，`csr[9:8]` 按约定表示最低访问级别（`00` U、`01` S、`10` H/VS、`11` M），`csr[11:10]=11` 表示只读地址；**有编号不等于硬件已实现，也不等于当前软件可访问**。可选扩展、上级授权、虚拟化拦截都可能影响访问。访问不存在或无权限的 CSR 会陷入；不要通过盲读高权限 CSR 来探测能力。
3. 表中“读写”指该 CSR 的总体属性，不表示每一位都可写。WARL 字段要写后读回；WPRI 保留位应按规范保留；中断 pending 位尤其常由硬件或控制器驱动。寄存器名称也不等于内存映射设备地址：`mtime`、`mtimecmp`、PLIC/APLIC/IMSIC 的设备寄存器都不是基础 CSR。

## 第一部分：U-mode 可见的寄存器

### 1.1 整数通用寄存器与 `pc`

RV64I 有 32 个 64 位整数寄存器，`x0` 固定读为 0，写入被忽略。`pc` 保存当前指令地址，但没有通用的 `csrr pc` 或 `mv rd,pc` 指令；`auipc`、跳转和 trap 机制会使用或改变 PC。下表的 `ra/sp/a/s/t` 是 **psABI 用途**，不是硬件访问权限。[基础整数寄存器](https://docs.riscv.org/reference/isa/unpriv/rv32.html)、[调用约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)

| 硬件编号 | ABI 名称 | 典型用途 | 普通函数返回后 |
|---|---|---|---|
| `x0` | `zero` | 常数 0 | 固定为 0 |
| `x1` | `ra` | 返回地址 | 调用者自保 |
| `x2` | `sp` | 栈指针 | 被调用者恢复；标准 ABI 中保持对齐 |
| `x3` | `gp` | 全局指针或平台指定用途 | 不分配作临时寄存器 |
| `x4` | `tp` | 线程指针 | 不分配作临时寄存器 |
| `x5～x7` | `t0～t2` | 临时值 | 调用者自保 |
| `x8～x9` | `s0～s1` | 被调用者保存；`s0` 可作 `fp` | 被调用者恢复 |
| `x10～x17` | `a0～a7` | 参数；`a0/a1` 常作返回值 | 调用者自保 |
| `x18～x27` | `s2～s11` | 被调用者保存 | 被调用者恢复 |
| `x28～x31` | `t3～t6` | 临时值 | 调用者自保 |
| 非 `x` 寄存器 | `pc` | 当前控制流位置 | 由指令/trap 更新 |

普通函数调用遵守 psABI；**异步 trap 必须按实际使用保存被打断的寄存器**，不能因为 `t0` 在 ABI 中属于 caller-saved 就直接破坏它。`gp/tp` 是否已初始化也由启动环境决定。更多栈帧和 C/汇编例子见[第 12 篇](12-rv64-psabi-elf-and-calling-convention.md)。

### 1.2 可选的浮点与向量寄存器

| 扩展 | 数据寄存器 | 用户级控制 CSR | 速查要点 |
|---|---|---|---|
| F/D 等浮点扩展 | `f0～f31`，每个宽度为 FLEN；F 为 32 位，D 为 64 位 | `fflags`、`frm`、`fcsr` | `fcsr[4:0]` 为累计异常标志，`[7:5]` 为舍入模式；`fflags/frm` 是对应字段的独立 CSR 视图 |
| V 向量扩展 | `v0～v31`，每个宽度为实现规定的 VLEN | `vstart`、`vxsat`、`vxrm`、`vcsr`、`vl`、`vtype`、`vlenb` | `v0` 常用于掩码；`vl/vtype` 描述本轮向量配置；`vlenb=VLEN/8` |

浮点或向量指令能否执行还取决于上级状态寄存器中的 `FS/VS` 字段；当对应状态为 Off 时会陷入。**寄存器存在与调用约定不同**：LP64 整数 ABI 下 `fs` 寄存器不提供硬浮点 ABI 的保存保证；标准向量调用约定下 `v0～v31` 都是调用者保存，向量调用约定变体另有规则。见[官方浮点规范](https://docs.riscv.org/reference/isa/unpriv/f-st-ext.html)、[向量规范](https://docs.riscv.org/reference/isa/unpriv/v-st-ext.html)、[ELF psABI](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)。

### 1.3 U 级 CSR 速查

`R/W` 表示可读写，`R` 表示只读；下列寄存器**不是全部 RV64 平台必有**。`cycle/time/instret` 属于 Zicntr；HPM 计数器属于 Zihpm；浮点和向量 CSR 分别要求相应扩展。[官方 CSR 分配表](https://docs.riscv.org/reference/isa/v20260120/priv/priv-csrs.html)、[计数器规范](https://docs.riscv.org/reference/isa/unpriv/counters.html)

| CSR 地址 | 名称 | 属性与条件 | 主要作用 |
|---|---|---|---|
| `0x001/0x002/0x003` | `fflags/frm/fcsr` | R/W；F | 异常标志、舍入模式及其组合视图 |
| `0x008/0x009/0x00A/0x00F` | `vstart/vxsat/vxrm/vcsr` | R/W；V | 向量恢复位置、饱和标志、定点舍入及组合视图 |
| `0xC20/0xC21/0xC22` | `vl/vtype/vlenb` | R；V | 向量长度、数据类型、寄存器字节长度；通常通过 `vset*` 更新 `vl/vtype` |
| `0xC00/0xC01/0xC02` | `cycle/time/instret` | R；Zicntr 且上级允许 | 周期、时间、退休指令数；`mtime` 本身是平台设备寄存器，不是 CSR |
| `0xC03～0xC1F` | `hpmcounter3～31` | R；Zihpm 且上级允许 | 可选硬件性能计数器 |
| `0x011` | `ssp` | R/W；Zicfiss | 影子栈指针，非普通 `sp` |
| `0x015` | `seed` | 受限访问；Zkr | 熵源接口；访问形式及上级授权有额外限制 |
| `0x017` | `jvt` | R/W；Zcmt | 跳转表基址与控制 |

计数器可读性要看权限链：S-mode 读 U 级计数器受 `mcounteren` 控制；U-mode 还受 `scounteren` 控制（若实现 S）；VU-mode 再受 H 扩展的授权与虚拟化配置影响。**只读计数器不是“U 一定能读”**。`cycle` 与 `instret` 可能受上级过滤或虚拟化；`time` 的频率由平台提供，不能直接当纳秒。U 代码访问 `sstatus/mstatus`、`satp/pmpcfg` 等特权 CSR 不合法。第 [09 篇](09-rv64-csr-instructions.md)有 U 级 CSR 指令 Demo。

## 第二部分：S-mode 可见的寄存器

本节的普通 S-mode 是 V=0 时的 S；启用 H 扩展时它称为 **HS-mode**。S/HS 仍可使用第一部分的通用寄存器和获准的 U 级 CSR。下表列出基础 S 级 CSR，后面的可选表列扩展寄存器；**不能因为名称以 `s` 开头就假设平台必有该扩展**。[Supervisor ISA](https://docs.riscv.org/reference/isa/priv/supervisor.html)

### 2.1 基础 S 级 CSR

| CSR 地址 | 名称 | 主要用途 | 易错边界 |
|---|---|---|---|
| `0x100` | `sstatus` | S 级全局中断、返回状态、内存访问权限、扩展状态 | 它是 `mstatus` 中 S 可见字段的视图；不是另一套独立状态 |
| `0x104` / `0x144` | `sie` / `sip` | S 级中断使能 / pending | 可见与可写位受 `mideleg` 等机制限制；pending 不一定软件可清 |
| `0x105` | `stvec` | S trap 入口 BASE 与 MODE | MODE=0 Direct；MODE=1 时只有中断按 `BASE+4×cause` 进入 |
| `0x106` | `scounteren` | 允许 U 访问 `cycle/time/instret/HPM` | 不能替代 M 对 S/U 的 `mcounteren` 授权 |
| `0x140` | `sscratch` | S trap 入口暂存 hart/栈上下文指针 | 硬件不会自动切换栈 |
| `0x141` / `0x142` / `0x143` | `sepc` / `scause` / `stval` | trap PC、原因、附加信息 | `stval` 按 cause 解释；不能一律当故障地址 |
| `0x10A` | `senvcfg` | U 级执行环境配置 | `FIOM` 与 CBO 等字段取决于扩展和上级授权 |
| `0x180` | `satp` | 页表模式、ASID、根物理页号 | 写 `satp` 本身不是 TLB shootdown；M 可用 `mstatus.TVM` 截获 |

`sstatus` 常查字段如下。位号是 RV64 中的常用字段位置；可选状态字段只有在相应能力存在时有意义。[sstatus 规范](https://docs.riscv.org/reference/isa/priv/supervisor.html)

| 位 | 字段 | 含义 |
|---:|---|---|
| 1、5、8 | `SIE`、`SPIE`、`SPP` | S 当前中断总开关、trap 前开关、trap 前特权级；`SRET` 使用后两者恢复状态 |
| 14:13、10:9 | `FS`、`VS` | 浮点、向量上下文状态：Off / Initial / Clean / Dirty |
| 18、19 | `SUM`、`MXR` | 允许 S 数据访问 U 页；允许 load 把可执行页视为可读。`SUM` 不允许 S 从 U 页取指 |
| 33:32、63 | `UXL`、`SD` | U-mode 的 XLEN 配置；脏扩展状态摘要 |

`satp` 在 RV64 中是 `MODE[63:60] | ASID[59:44] | PPN[43:0]`。常见 `MODE=0` 为 Bare，`8/9/10` 分别为 Sv39/Sv48/Sv57；实际支持模式和 ASID 位数要读回验证。PPN 是**物理**根页表地址右移 12 位，不是 C 指针值。PTE 更新后的本 hart 同步用 `SFENCE.VMA`，其他 hart 另需通知和确认；见[第 17 篇](17-rv64-sv39-and-sfence-vma.md)。

常见中断位位置：`sip/sie` 的 `SSIP=1`、`STIP=5`、`SEIP=9`；`scause` 在 RV64 的 bit 63 为中断标志，低位为 cause。U→S 的 `ECALL` cause 为 8，S timer interrupt 为 5。`sepc` 处理同步异常时通常指向触发指令；修复页故障后应重试，处理 `ECALL` 则需按实际指令长度推进。有关 trap 的状态转换见[第 14 篇](14-rv64-privilege-trap-and-return.md)。

S-mode 请求 OpenSBI 服务时，`a7` 放 EID、`a6` 放 FID、`a0～a5` 放参数；返回的 `a0` 是错误码，成功时 `a1` 才按 FID 解释为结果。**这些仍是第一部分的同一组 `x10～x17`，属于 SBI 软件 ABI，不是新增 CSR。**失败时 `a1` 通常未指定。见[第 15 篇](15-rv64-sbi-and-opensbi.md)。

### 2.2 常见 S 级可选 CSR

| CSR 地址 | 名称 | 所需能力 / 用途 |
|---|---|---|
| `0x14D` | `stimecmp` | Sstc：S timer 比较值；还受 M 的 `menvcfg.STCE` 授权；SBI TIME 是另一条路径 |
| `0xDA0` | `scountovf` | Sscofpmf：只读计数器溢出摘要，不等于清除中断的寄存器 |
| `0x120` | `scountinhibit` | Smcdeleg/Ssccfg：委托后的 S 级计数器抑制控制 |
| `0x10C～0x10F` | `sstateen0～3` | Ssstateen：控制低特权级可见的扩展状态；受 M 层约束 |
| `0x150～0x153`、`0x155～0x157` | `siselect`、`sireg`、`sireg2～6` | Sscsrind/AIA 等：选择器及间接数据窗口；数据含义取决于选择值和扩展 |
| `0x181` | `srmcfg` | Ssqosid：资源管理标识配置 |
| `0x14E/0x14F/0x15F` | `sctrctl/sctrstatus/sctrdepth` | Smctr：控制转移记录配置与状态 |
| `0x5A8` | `scontext` | Debug/Trace 上下文标识，依赖相应调试/触发能力 |

若实现 **AIA**，`stopi` (`0xDB0`，只读) 报告当前最高优先级的 pending 且 enabled 的 S 中断；该值不受全局 `sstatus.SIE` 影响。有 IMSIC 时 `stopei` (`0x15C`) 用于 S 外部中断文件。`siselect/sireg` 还可能访问 IMSIC 的间接寄存器。PLIC/APLIC/IMSIC 控制器的设备寄存器不能与上述 hart CSR 混写；具体可见寄存器取决于 Smaia/Ssaia、是否有 IMSIC 和平台拓扑。见[官方 AIA CSR 表](https://docs.riscv.org/reference/aia/_attachments/riscv-interrupts.pdf)和[第 18 篇](18-rv64-interrupt-timer-and-aia.md)。

### 2.3 H 扩展：HS 管理 VS/VU 的寄存器

H 扩展实现后，V=0 的 S 是 HS；guest 运行在 VS/VU。**VS 不是另一个可直接拿到 HS CSR 的模式**：HS 可以在 V=0 时通过专用 `vs*` 编号管理 guest 状态，guest 的普通 `s*` CSR 访问则按 H 规则映射到 VS 状态或被拦截。以下寄存器全部以实现 H 为前提。[H 扩展规范](https://docs.riscv.org/reference/isa/priv/hypervisor)、[官方 CSR 分配表](https://docs.riscv.org/reference/isa/v20260120/priv/priv-csrs.html)

| HS 可访问 CSR 地址 | 名称 | 速查作用 |
|---|---|---|
| `0x600` | `hstatus` | 虚拟化 trap/return 状态；常查 `SPV`、`SPVP`、`GVA`、`VGEIN` |
| `0x602/0x603` | `hedeleg/hideleg` | HS→VS 异常/中断委托；与 M→HS 的 `medeleg/mideleg` 分层 |
| `0x604/0x644/0x645` | `hie/hip/hvip` | H 级中断使能、pending、注入的虚拟 pending |
| `0x607/0xE12` | `hgeie/hgeip` | guest external interrupt 使能/pending；后者只读 |
| `0x606/0x60A` | `hcounteren/henvcfg` | guest 计数器与环境功能授权 |
| `0x605` | `htimedelta` | VS/VU 的时间偏移 |
| `0x643/0x64A` | `htval/htinst` | guest trap 的附加地址/指令信息；不能假设一定非零 |
| `0x680` | `hgatp` | G-stage `MODE/VMID/PPN`；RV64 常用 Sv39x4/Sv48x4/Sv57x4 |
| `0x60C～0x60F` | `hstateen0～3` | 可选状态授权链 |
| `0x6A8` | `hcontext` | 可选 Debug/Trace 上下文标识 |
| `0x608/0x609/0x646/0x647` | `hvien/hvictl/hviprio1/2` | AIA：VS 虚拟中断使能、控制和优先级 |

| HS 访问的 VS CSR 地址 | 名称 | 对应 guest 状态 |
|---|---|---|
| `0x200/0x204/0x205` | `vsstatus/vsie/vstvec` | guest 的 S 状态、中断使能、trap 入口 |
| `0x240～0x244` | `vsscratch/vsepc/vscause/vstval/vsip` | guest 的 trap 暂存、PC、cause、tval、pending |
| `0x280` | `vsatp` | VS-stage 页表模式、ASID、根页号 |
| `0x24D` | `vstimecmp` | Sstc + H 时的 guest timer 比较值 |
| `0x250～0x253`、`0x255～0x257` | `vsiselect/vsireg/vsireg2～6` | 可选 VS 间接 CSR 窗口 |
| `0x24E` | `vsctrctl` | 可选 guest 控制转移记录配置 |
| `0x25C/0xEB0` | `vstopei/vstopi` | AIA：guest 外部中断文件接口（需 IMSIC）/最高优先级 guest 中断（只读） |

`hgatp` 是 G-stage 的翻译根，`vsatp` 是 VS-stage 的翻译根；对应同步分别考虑 `HFENCE.GVMA` 与 `HFENCE.VVMA`。guest-page fault 的 `htval` 在适用时给出 GPA 右移两位的值，也允许为 0；**它并非总是原始 load 的目标 GPA**。完整例子见[第 20 篇](20-rv64-h-extension-and-guest-stage.md)。

## 第三部分：M-mode 可见的寄存器

M-mode 是最高常规特权级，可以访问已实现的低级 CSR，但仍需遵守每个 CSR 的只读、WARL 和扩展规则。**Debug Mode 是另一个状态**：`dcsr/dpc` 等 Debug 专用 CSR 不能仅因代码运行在 M-mode 就随意访问。[Machine ISA](https://docs.riscv.org/reference/isa/priv/machine.html)、[官方 CSR 分配表](https://docs.riscv.org/reference/isa/v20260120/priv/priv-csrs.html)

### 3.1 身份、trap 与委托

| CSR 地址 | 名称 | 主要用途 | 易错边界 |
|---|---|---|---|
| `0xF11～0xF15` | `mvendorid/marchid/mimpid/mhartid/mconfigptr` | 只读的厂商、架构、实现、hart 与配置指针信息 | `mhartid` 是硬件线程标识，不保证连续编号或等于 OS CPU 号 |
| `0x300` | `mstatus` | M 中断/返回状态、MPRV、S 拦截、扩展上下文状态 | `MRET` 按 `MPP/MPIE` 恢复；不是普通 `ret` |
| `0x301` | `misa` | 基础 ISA 宽度及字母扩展能力 | 只反映其定义的能力；并非所有多字母扩展都能从此推断 |
| `0x302/0x303` | `medeleg/mideleg` | 把可委托的异常/中断路由给 S | 写后读回；M 自己发生的 trap 不会因此委托给 S |
| `0x304/0x344` | `mie/mip` | M 级中断使能/pending；含部分 S 级位 | `MTIP` 不是靠清 `mip` 就能消除 |
| `0x305` | `mtvec` | M trap 入口 BASE/MODE | 模式与对齐要求可能是 WARL；中断向量才使用 cause 偏移 |
| `0x306` | `mcounteren` | 授权低特权级读 U 级计数器 | 与 `scounteren/hcounteren` 共同形成权限链 |
| `0x340` | `mscratch` | M trap 入口暂存每 hart 指针 | 硬件不自动保存通用寄存器或换栈 |
| `0x341/0x342/0x343` | `mepc/mcause/mtval` | M trap PC、原因、附加值 | `mtval` 可因 cause/实现为 0；先看 `mcause` |
| `0x34A/0x34B` | `mtinst/mtval2` | H 扩展下的额外 trap 指令/G-stage 地址信息 | 仅在适用的异常与实现中有意义 |

`mstatus` 的常用 RV64 位：`MIE=3`、`MPIE=7`、`MPP=[12:11]`、`MPRV=17`、`SUM=18`、`MXR=19`、`TVM=20`、`TW=21`、`TSR=22`；浮点/向量状态由 `FS=[14:13]`、`VS=[10:9]` 管理。`MPP` 记录 trap 前的权限，用 `MRET` 回去；`MPRV=1` 让**显式数据访存**按 `MPP` 的有效权限执行，取指不因此降权。`TVM/TW/TSR` 分别影响低权限的虚存管理操作、`WFI`、`SRET`。若有 H，`mstatus` 还增加与虚拟化来源相关的字段；不要只保存上述基础位就认为保存了完整 trap 上下文。[mstatus 规范](https://docs.riscv.org/reference/isa/priv/machine.html)

`misa` 的 RV64 `MXL[63:62]=2` 表示 64 位基础整数宽度；低 26 位按字母位置编码部分单字母扩展。多字母扩展与平台设备不能只靠 `misa` 一次读取得知。`mstatus` 同时包含通过 `sstatus` 观察到的 S 状态字段，并可包含大小端、H 扩展等可选字段；保存/恢复状态时应按已实现的字段处理。

常见 `mie/mip` 位：`MSIP=3`、`MTIP=7`、`MEIP=11`，对应 cause 也是 3、7、11；S 级 `SSIP/STIP/SEIP` 对应 1、5、9。RV64 的 `mcause[63]` 表示中断，低位是编号。**pending、局部 enable、当前权限与全局 enable、委托**必须一起判断，不能只看一个 CSR。`mtime/mtimecmp` 通常是平台 MMIO 或固件服务，不在这张 CSR 表中。见[第 18 篇](18-rv64-interrupt-timer-and-aia.md)。

### 3.2 PMP、环境授权与安全配置

| CSR 地址 | 名称 | 主要用途 | 易错边界 |
|---|---|---|---|
| `0x3A0,0x3A2,…,0x3AE` | `pmpcfg0,pmpcfg2,…,pmpcfg14` | RV64 每个 CSR 放 8 个 PMP 条目配置字节 | `pmpcfg1/3/…/15` 只在 RV32 使用；未实现条目读回可能为 0 |
| `0x3B0～0x3EF` | `pmpaddr0～63` | PMP 地址与 TOR/NAPOT 编码 | 地址粒度、条目数因实现而异；TOR 使用前一地址作下界 |
| `0x30A` | `menvcfg` | S/U 环境功能授权，例如 Sstc、CBO、页表扩展 | 改字段后需按所属功能完成同步；不是通用“全部授权”开关 |
| `0x747` | `mseccfg` | Smepmp 等安全配置 | `MMWP/MML/RLB` 有锁定与粘滞语义；不能在运行中随意试写 |
| `0x30C～0x30F` | `mstateen0～3` | Smstateen：给低特权级开放可选状态 | 还需配合下级的 `sstateen/hstateen` |

普通 PMP 中，编号最小且与访问任意字节匹配的条目优先；`L=0` 时 M 的普通访存通常不受该条目 R/W/X 限制。**Smepmp 会改变这一默认假设**，尤其 `MMWP/MML`。`satp=Bare` 也不会绕过 PMP；CPU PMP 不自动限制 DMA。具体编码与可运行负例见[第 16 篇](16-rv64-pmp-and-tee-isolation.md)。

每条 PMP 配置字节中 `R/W/X` 为 bit 0/1/2，`A[4:3]` 选 OFF/TOR/NA4/NAPOT，`L` 为 bit 7。实际条目数和粒度由实现决定；写入地址与配置后应读回，且锁定后不能指望再改写这些条目。

### 3.3 计数器、间接 CSR 与其他可选 M 状态

| CSR 地址 | 名称 | 所需能力 / 用途 |
|---|---|---|
| `0xB00/0xB02` | `mcycle/minstret` | M 可管理的周期与退休指令计数器；不是 U 级 `cycle/instret` 的可写地址 |
| `0xB03～0xB1F` | `mhpmcounter3～31` | 可选 HPM 计数器 |
| `0x320` | `mcountinhibit` | 停止/恢复相应机器计数器的计数 |
| `0x323～0x33F` | `mhpmevent3～31` | 可选 HPM 事件选择；具体事件编码可能依赖平台 |
| `0x321/0x322` | `mcyclecfg/minstretcfg` | Smcntrpmf：按权限模式过滤 cycle/instret 统计 |
| `0x350～0x353`、`0x355～0x357` | `miselect/mireg/mireg2～6` | Smcsrind/AIA 的机器级间接 CSR 选择器与数据窗口 |
| `0x740/0x741/0x742/0x744` | `mnscratch/mnepc/mncause/mnstatus` | Smrnmi：可恢复 NMI 的独立状态，非普通 M trap CSR |
| `0x34E` | `mctrctl` | Smctr：控制转移记录配置 |
| `0x7A0～0x7A3/0x7A8` | `tselect/tdata1～3/mcontext` | 已实现调试触发器时的选择、数据与上下文 |
| `0x7B0～0x7B3` | `dcsr/dpc/dscratch0/1` | **仅 Debug Mode**，列在此作边界提醒；普通 M-mode 不能当作 M CSR 使用 |

若实现 AIA，`mtopi` (`0xFB0`，只读) 报告最高优先级的 pending 且 enabled 的 M 中断，该值不受全局 `mstatus.MIE` 影响；有 IMSIC 时 `mtopei` (`0x35C`) 是 M 外部中断文件接口。`mvien/mvip` (`0x308/0x309`) 为向 S 提供虚拟中断的控制与 pending 状态。间接窗口的 `*iselect` 值属于**另一套选择编号**，不是 CSR 的 12 位地址；不能把 `miselect=0x70` 误读成 CSR `0x070`。AIA 的详细表见[官方 AIA CSR 表](https://docs.riscv.org/reference/aia/_attachments/riscv-interrupts.pdf)。

**开发时的最短核对顺序：**先确认当前模式与 V 状态，再确认扩展/CSR 是否实现与上级授权；访问失败看 `*cause/*epc/*tval`，读写后检查 WARL 字段；涉及页表用相应 `SFENCE/HFENCE`，涉及中断另查来源设备与 pending/enable，涉及跨 hart 或设备还要单独建立内存顺序。CSR 的读写方法见[第 09 篇](09-rv64-csr-instructions.md)，系统行为分别见[第 14～20 篇](14-rv64-privilege-trap-and-return.md)。
