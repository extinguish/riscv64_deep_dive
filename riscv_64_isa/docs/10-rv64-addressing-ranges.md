# RISC-V 64 位 ISA：Addressing Ranges

本文继续 RISC-V 64 位 ISA 系列，集中讲解“寻址范围”。这个词经常把几种不同限制混在一起：指令能编码多大的偏移、两条指令能构造多远的地址、64 位地址是否符合页表模式，以及地址是否真的已映射。它们需要分别判断。

本篇提供五个可运行 Demo 和一个预期链接失败的边界实验，全部使用 RV64I 与现有章节相同的 GNU 工具链、QEMU User Mode 流程。

## 1. 先区分四种“范围”

| 问题 | 限制来自哪里 | 典型例子 |
|---|---|---|
| 单条指令能表示多大偏移 | 指令编码 | `LD` 偏移 `-2048～2047` 字节 |
| 一组指令能引用多远的符号 | 指令组合、重定位、代码模型 | `AUIPC + ADDI` 附近约 ±2 GiB |
| 哪些 64 位值是有效虚拟地址 | 当前页表模式与地址规范化规则 | Sv39 要求高位复制 bit 38 |
| 某个有效地址能否访问 | 页表映射和访问权限 | 已映射、可读写、用户态可访问 |

一个 64 位寄存器可以保存 `0x123456789abcdef0`，不代表该值是当前进程可访问的地址。反过来，单条 `LD` 只有 12 位偏移，也不代表进程只能访问基址周围 4 KiB；程序可以先计算新的 64 位基址。

本篇沿用 [内存指令](01-rv64-memory-instructions.md)、[PC 相对寻址](03-rv64-pc-relative-instructions.md) 和 [跳转指令](08-rv64-jump-and-branch-instructions.md) 的概念，重点验证边界值和容易混淆的情况。

## 2. 常见指令的直接偏移范围

| 指令或形式 | 立即数或偏移 | 单位 | 目标或结果 |
|---|---:|---|---|
| `ADDI` | `-2048～2047` | 整数数值 | `rs1 + sign_extend(imm12)` |
| `LB/LH/LW/LD`、`SB/SH/SW/SD` | `-2048～2047` | 字节 | `rs1 + sign_extend(imm12)` |
| `JALR` | `-2048～2047` | 字节 | `(rs1 + sign_extend(imm12)) & ~1` |
| `BEQ/BNE/BLT/BGE/BLTU/BGEU` | `-4096～4094` | 字节，编码步长 2 | 当前分支的 `PC + 偏移` |
| `JAL` | `-1048576～1048574` | 字节，编码步长 2 | 当前 `JAL` 的 `PC + 偏移` |
| `LUI` | 20 位字段放在 bits `[31:12]` | 值，不是偏移 | 32 位结果在 RV64 上符号扩展 |
| `AUIPC` | 20 位字段放在 bits `[31:12]` | 相对当前 PC 的值 | PC 加上符号扩展的 32 位高部 |

前三类 12 位偏移范围相同，但用途不同。`LD` 的偏移是内存地址的一部分；`ADDI` 计算整数值；`JALR` 最后还清除目标 bit 0。条件分支和 `JAL` 的编码步长为 2 字节；本篇 `-march=rv64i` 只有 4 字节指令，所以可执行标签仍需 4 字节对齐。

## 3. Demo 1：加载与存储的正负偏移边界

以一个 4096 字节缓冲区中间为基址，验证 `-2048` 与 `+2047` 两个最远字节偏移都能工作。`+2048` 则不能放入单条 `LB/SB` 的立即数字段。

保存为 `offset_bounds.S`：

```asm
    .option nopic

    .section .rodata
pass_message:
    .ascii "Offset bounds demo passed!\n"
    .equ pass_message_len, . - pass_message
fail_message:
    .ascii "Offset bounds demo failed!\n"
    .equ fail_message_len, . - fail_message

    .section .bss
    .balign 8
buffer:
    .skip 4096

    .section .text
    .globl _start
_start:
    la  t0, buffer
    li  t1, 2048
    add t2, t0, t1            # t2 = &buffer[2048]

    li t3, 0x12
    sb t3, -2048(t2)          # &buffer[0]
    li t3, 0x34
    sb t3, 2047(t2)           # &buffer[4095]

    lbu t4, -2048(t2)
    lbu t5, 2047(t2)
after_bounds:
    li  t6, 0x12
    bne t4, t6, failed
    li  t6, 0x34
    bne t5, t6, failed

    li a0, 1
    la a1, pass_message
    li a2, pass_message_len
    li a7, 64
    ecall
    li a0, 0
    li a7, 93
    ecall

failed:
    li a0, 1
    la a1, fail_message
    li a2, fail_message_len
    li a7, 64
    ecall
    li a0, 1
    li a7, 93
    ecall
```

汇编和运行：

```bash
riscv64-linux-gnu-as -march=rv64i -mabi=lp64 -g \
    -o offset_bounds.o offset_bounds.S
riscv64-linux-gnu-ld -m elf64lriscv -static -e _start \
    -o offset_bounds offset_bounds.o
qemu-riscv64-static ./offset_bounds
echo $?
```

预期输出是 `Offset bounds demo passed!`，退出状态为 `0`。注意构造中间基址时使用 `li t1, 2048` 加 `add`；`addi t2, t0, 2048` 自身就超出 12 位有符号立即数范围。

这里选用字节访问，是为了同时验证 `-2048` 和 `+2047` 两个边界。对 8 字节对齐的基址，`LD/SD` 若要求自然对齐，则最大可用的正偏移通常是 `+2040`，不是 `+2047`；立即数能编码与访问地址是否对齐是两项不同检查。

## 4. `LUI` 的 RV64 符号扩展陷阱

`LUI` 不是“把任意 20 位数字放在 64 位寄存器的高端”。它把立即数字段置于结果的 bits `[31:12]`，低 12 位填零，然后在 RV64 上把这个 **32 位结果符号扩展到 64 位**。

```asm
lui t0, 0x7ffff            # t0 = 0x000000007ffff000
lui t1, 0x80000            # t1 = 0xffffffff80000000
```

因此 `lui t1, 0x80000` 得到的是负的 64 位位模式，不是 `0x0000000080000000`。如果程序需要后一个值，可以把低 32 位零扩展：

```asm
lui  t1, 0x80000
slli t1, t1, 32
srli t1, t1, 32
```

`LI` 是伪指令，可以装入更广的 64 位常数，但汇编器可能使用多条真实指令。把一个数放入寄存器与访问该数指向的内存仍是两回事。

## 5. Demo 2：`LUI + ADDI` 的非对称边界

这组指令在 RV64 上可以直接构造两段绝对地址位模式，边界并不是简单的正负各 `2^31`。下面用数值计算验证这一点，不实际解引用这些位模式。

保存为 `lui_bounds.S`：

```asm
    .option nopic

    .section .rodata
pass_message:
    .ascii "LUI bounds demo passed!\n"
    .equ pass_message_len, . - pass_message
fail_message:
    .ascii "LUI bounds demo failed!\n"
    .equ fail_message_len, . - fail_message

    .section .text
    .globl _start
_start:
    # 正区间的最高值
    lui  t0, 0x7ffff
    addi t0, t0, 2047         # 0x000000007ffff7ff
    li   t1, 0x000000007ffff7ff
    bne  t0, t1, failed

    # 负区间的最低值；LUI 的 bit 31 会符号扩展
    lui  t0, 0x80000
    addi t0, t0, -2048        # 0xffffffff7ffff800
    li   t1, 0xffffffff7ffff800
    bne  t0, t1, failed

    # 单独 LUI 0x80000 得到负的 64 位位模式
    lui t0, 0x80000
    li  t1, 0xffffffff80000000
    bne t0, t1, failed

    # 同一低 32 位经过零扩展，变成正的 64 位值
    slli t0, t0, 32
    srli t0, t0, 32
    li   t1, 0x0000000080000000
after_lui:
    bne t0, t1, failed

    li a0, 1
    la a1, pass_message
    li a2, pass_message_len
    li a7, 64
    ecall
    li a0, 0
    li a7, 93
    ecall

failed:
    li a0, 1
    la a1, fail_message
    li a2, fail_message_len
    li a7, 64
    ecall
    li a0, 1
    li a7, 93
    ecall
```

汇编和运行：

```bash
riscv64-linux-gnu-as -march=rv64i -mabi=lp64 -g \
    -o lui_bounds.o lui_bounds.S
riscv64-linux-gnu-ld -m elf64lriscv -static -e _start \
    -o lui_bounds lui_bounds.o
qemu-riscv64-static ./lui_bounds
echo $?
```

预期输出为 `LUI bounds demo passed!`，退出状态为 `0`。这也解释了 psABI 的 `medlow` 模型为何把 RV64 可直接寻址区域写成：

```text
低端：0x0000000000000000 ～ 0x000000007ffff7ff
高端：0xffffffff7ffff800 ～ 0xffffffffffffffff
```

中间的大多数 64 位地址不能靠这组简单的绝对地址构造形式直接引用。这里的数值边界描述**编码能力**；某地址是否真的有内存映射，是另一个问题。

## 6. `AUIPC` 与低 12 位：为什么必须进位调整

`AUIPC + ADDI` 可以形成附近符号的 PC 相对地址。设：

```text
delta = target - AUIPC_PC
```

若低 12 位对应的有符号数为负，汇编器和链接器必须把高部加一，再用一个负的低部抵消。概念上：

```text
hi = (delta + 0x800) >> 12
lo = delta - (hi << 12)       # 落在 -2048～2047
```

例如，假设 `delta = 0x2800`：

```text
直接拆成 0x2000 + 0x800 会出问题，+0x800 无法作为正 12 位立即数。
正确拆法：0x3000 + (-0x800) = 0x2800。
```

`%pcrel_lo` 引用的是对应 `AUIPC` 所在位置的标签，而不是直接再次写目标符号：

```asm
anchor:
    auipc t0, %pcrel_hi(target)
    addi  t0, t0, %pcrel_lo(anchor)
```

不要手工把符号地址的十六进制高 20 位和低 12 位直接拼接；跨过 `0x800` 边界时很容易少算或多算 `0x1000`。

## 7. Demo 3：强制出现负的 `%pcrel_lo`

把数据符号安排在页内偏移 `0x900`，使下面测试链接布局中的 PC 相对低部落在负立即数范围。源码同时通过 `la` 独立计算同一符号地址，并读取数据验证结果。

保存为 `pcrel_rounding.S`：

```asm
    .option nopic

    .section .data
    .p2align 12
    .space 0x900, 0
target:
    .dword 0x1122334455667788

    .section .rodata
pass_message:
    .ascii "PC-relative rounding demo passed!\n"
    .equ pass_message_len, . - pass_message
fail_message:
    .ascii "PC-relative rounding demo failed!\n"
    .equ fail_message_len, . - fail_message

    .section .text
    .globl _start
_start:
anchor:
    auipc t0, %pcrel_hi(target)
    addi  t0, t0, %pcrel_lo(anchor)

    la  t1, target
    bne t0, t1, failed
    ld  t2, 0(t0)
    li  t3, 0x1122334455667788
    bne t2, t3, failed
after_pcrel:
    li a0, 1
    la a1, pass_message
    li a2, pass_message_len
    li a7, 64
    ecall
    li a0, 0
    li a7, 93
    ecall

failed:
    li a0, 1
    la a1, fail_message
    li a2, fail_message_len
    li a7, 64
    ecall
    li a0, 1
    li a7, 93
    ecall
```

汇编、链接和运行：

```bash
riscv64-linux-gnu-as -march=rv64i -mabi=lp64 -g \
    -o pcrel_rounding.o pcrel_rounding.S
riscv64-linux-gnu-ld -m elf64lriscv -static --no-relax -e _start \
    -o pcrel_rounding pcrel_rounding.o
qemu-riscv64-static ./pcrel_rounding
echo $?
```

预期输出为 `PC-relative rounding demo passed!`，退出状态为 `0`。查看重定位和最终指令：

```bash
riscv64-linux-gnu-objdump -dr pcrel_rounding.o
riscv64-linux-gnu-objdump -d -M no-aliases,numeric pcrel_rounding
riscv64-linux-gnu-nm pcrel_rounding | rg 'anchor|target'
```

重点观察链接后的 `ADDI` 使用负立即数，同时 `AUIPC` 的高部已被调整。数值会随 ELF 布局变化，应该用当前反汇编结果计算 `target - anchor`，不应死记某一次的机器地址。`--no-relax` 使原始两指令序列便于观察。

## 8. 条件分支的范围与汇编器自动展开

条件分支的直接偏移范围是 `-4096～4094` 字节。由于基础 RV64I 代码要求 4 字节对齐，本篇向前跳转时最大的对齐偏移为 **4092 字节**。

超出这一范围时，GNU 汇编器可能把源码中的一条远条件分支展开成两条真实指令：先用相反条件跳过下一条，再用 `JAL x0` 去更远的目标。这不改变单条 B-type 指令的编码范围。

## 9. Demo 4：4092 字节直达，4096 字节自动展开

保存为 `branch_range.S`：

```asm
    .option nopic

    .section .rodata
pass_message:
    .ascii "Branch range demo passed!\n"
    .equ pass_message_len, . - pass_message

    .section .text
    .globl _start
_start:
    beq x0, x0, near_target
    .space 4088, 0            # 目标相对原 BEQ 为 4+4088=4092

near_target:
    beq x0, x0, far_target
    .space 4092, 0            # 若 BEQ 仍为 4 字节，目标将相距 4096

far_target:
    li a0, 1
    la a1, pass_message
    li a2, pass_message_len
    li a7, 64
    ecall
    li a0, 0
    li a7, 93
    ecall
```

`.space` 填充的零字节不是可执行的有效 RV64I 指令；程序依靠跳转越过填充区。汇编和运行：

```bash
riscv64-linux-gnu-as -march=rv64i -mabi=lp64 -g \
    -o branch_range.o branch_range.S
riscv64-linux-gnu-ld -m elf64lriscv -static --no-relax -e _start \
    -o branch_range branch_range.o
qemu-riscv64-static ./branch_range
echo $?
```

预期输出为 `Branch range demo passed!`，退出状态为 `0`。查看真正的指令：

```bash
riscv64-linux-gnu-objdump -d -M no-aliases,numeric branch_range \
    | rg '(<(_start|near_target|far_target)>|\b(beq|bne|jal)\b)'
```

在当前 GNU 工具链中，第一处仍是单条 `BEQ`；第二处变为 `BNE x0, x0, skip` 加 `JAL x0, far_target`。此时 `BNE` 的条件恒为假，所以继续执行 `JAL`。这里的汇编器自动展开是工具行为，不应推断所有汇编器都会以相同方式处理远分支。

## 10. `JAL` 边界：超范围可能在链接时才报错

单条 `JAL` 的最大正向偏移是 `1048574` 字节。本篇 RV64I 目标要求 4 字节对齐，因此最大的对齐正向偏移是 **1048572 字节**；目标再向前 4 字节就变成 `1048576`，超出单条 `JAL` 的编码范围。

汇编器有时会先生成目标文件与重定位，链接器确定最终地址时才发现越界。因此“汇编通过”不等于“跳转距离合法”。

## 11. Demo 5：预期失败的 `JAL` 链接实验

这个 Demo 的预期结果是 **汇编成功、链接失败**。它不生成可运行程序，用来认识真实的超范围诊断。

保存为 `jal_too_far.S`：

```asm
    .option nopic
    .section .text
    .globl _start
_start:
    jal x0, far_target
    .space 1048572, 0         # 目标距 JAL：4+1048572=1048576

far_target:
    li a0, 0
    li a7, 93
    ecall
```

执行：

```bash
riscv64-linux-gnu-as -march=rv64i -mabi=lp64 \
    -o jal_too_far.o jal_too_far.S
riscv64-linux-gnu-ld -m elf64lriscv -static --no-relax -e _start \
    -o jal_too_far jal_too_far.o
```

当前 GNU 链接器会报告类似：

```text
relocation truncated to fit: R_RISCV_JAL against `far_target'
```

解决办法不是“换一个更大的 JAL 立即数”，而是改用合适的远调用或地址构造方式，例如 `call far_target`、`AUIPC + JALR`，或通过寄存器中的完整函数地址执行 `JALR`。具体选择还取决于代码模型和目标距离。

## 12. `AUIPC + ADDI/JALR` 与代码模型

`AUIPC` 的高部加上 12 位有符号低部，能形成当前 PC 附近约 ±2 GiB 的地址。由于低部是有符号数，可编码的数值差并不严格关于零对称；RV64I 规范给出的组合范围是：

```text
[-2^31 - 2^11, 2^31 - 2^11 - 1]
```

这说明“±2 GiB”是便于记忆的近似说法。计算是否可达时应以实际重定位与链接结果为准。

常见代码模型的想法不同：

| 模型 | 地址形成方式 | 主要限制 |
|---|---|---|
| `medlow` | `LUI + ADDI/LOAD/STORE` | 绝对地址处于可编码的低端或高端区域 |
| `medany` | `AUIPC + ADDI/LOAD/STORE` | 符号在当前 PC 附近可达 |
| `large` | 多条指令及字面量数据等 | 支持更广的 64 位地址布局，指令序列更长 |

这些是工具链的代码生成约定，不改变单条 `ADDI/LD/BEQ/JAL` 的编码位数。`la` 也是伪指令，它的展开与 PIC、符号可见性、GOT 和链接器 relaxation 有关。阅读最终反汇编比仅凭源码猜测范围可靠。

## 13. 64 位寄存器不等于 64 位虚拟地址空间

RV64 寄存器能保存任意 64 位位模式，但采用页式虚拟内存时，虚拟地址还必须符合当前地址翻译模式的规范化规则。

| 模式 | 有效虚拟地址位数 | 低端规范区间 | 高端规范区间 | 高位规则 |
|---|---:|---|---|---|
| Sv39 | 39 | `0x0000000000000000～0x0000003fffffffff` | `0xffffffc000000000～0xffffffffffffffff` | bits `[63:39]` 复制 bit 38 |
| Sv48 | 48 | `0x0000000000000000～0x00007fffffffffff` | `0xffff800000000000～0xffffffffffffffff` | bits `[63:48]` 复制 bit 47 |
| Sv57 | 57 | `0x0000000000000000～0x00ffffffffffffff` | `0xff00000000000000～0xffffffffffffffff` | bits `[63:57]` 复制 bit 56 |

这些区间只表示**地址位模式符合相应模式的要求**，不表示整个区间已映射，更不表示 Linux 用户态可以访问高端区间。具体使用哪种模式由系统决定，不能只看处理器是 RV64 就假设一定是 Sv39。

即使地址规范化正确，仍可能因缺少映射、没有读写权限、用户态访问了内核区域，或访问不对齐而失败。物理地址宽度和可用 RAM 大小又是另外的限制，不能由 RV64 的“64 位”直接推出。

## 14. Demo 6：只检查 Sv39 地址位模式

本 Demo 不访问下面的候选地址，只在寄存器中检查它们是否满足 Sv39 的符号扩展规则。算法把 bit 38 移到 bit 63，再算术右移回来：

```asm
slli t1, t0, 25
srai t1, t1, 25
```

如果结果与原值相同，该 64 位位模式就是 Sv39 规范化地址。

保存为 `sv39_pattern.S`：

```asm
    .option nopic

    .section .rodata
pass_message:
    .ascii "Sv39 address pattern demo passed!\n"
    .equ pass_message_len, . - pass_message
fail_message:
    .ascii "Sv39 address pattern demo failed!\n"
    .equ fail_message_len, . - fail_message

    .section .text
    .globl _start
_start:
    # 低端最大规范化地址
    li   t0, 0x0000003fffffffff
    slli t1, t0, 25
    srai t1, t1, 25
    bne  t0, t1, failed

    # 再加一：bit 38 为 1，但高位仍为 0，故不规范
    li   t0, 0x0000004000000000
    slli t1, t0, 25
    srai t1, t1, 25
    beq  t0, t1, failed

    # 高端最小规范化地址
    li   t0, 0xffffffc000000000
    slli t1, t0, 25
    srai t1, t1, 25
    bne  t0, t1, failed

    # 高端区间之前的值：高位未正确复制 bit 38
    li   t0, 0xffffffbfffffffff
    slli t1, t0, 25
    srai t1, t1, 25
    beq  t0, t1, failed

after_patterns:
    li a0, 1
    la a1, pass_message
    li a2, pass_message_len
    li a7, 64
    ecall
    li a0, 0
    li a7, 93
    ecall

failed:
    li a0, 1
    la a1, fail_message
    li a2, fail_message_len
    li a7, 64
    ecall
    li a0, 1
    li a7, 93
    ecall
```

汇编和运行：

```bash
riscv64-linux-gnu-as -march=rv64i -mabi=lp64 -g \
    -o sv39_pattern.o sv39_pattern.S
riscv64-linux-gnu-ld -m elf64lriscv -static -e _start \
    -o sv39_pattern sv39_pattern.o
qemu-riscv64-static ./sv39_pattern
echo $?
```

预期输出为 `Sv39 address pattern demo passed!`，退出状态为 `0`。这只是位模式计算，不能用运行成功推断当前 QEMU User Mode 实际采用 Sv39 页表或这些地址存在映射。

## 15. QEMU User Mode 与真实页表的边界

本系列使用 `qemu-riscv64-static` 运行 Linux 用户空间 ELF。QEMU User Mode 翻译用户态指令和系统调用，没有启动 RISC-V 客户机内核，也没有为该程序启动一套可由我们切换 Sv39/Sv48/Sv57 的客体页表。

因此：

- Demo 1 可以验证访存指令偏移与实际读写。
- Demo 2、3 可以验证常数和 PC 相对地址构造。
- Demo 4、5 可以验证汇编器、链接器对跳转范围的处理。
- Demo 6 只能验证 Sv39 的**数学位模式规则**，不能验证客户机 MMU 的页故障。

若要研究页表配置、规范化地址触发的异常和物理地址翻译，需要在真实 RV64 系统或 QEMU System Mode 的客体操作系统/裸机环境下实验。Linux 用户态访问失败也可能只是映射或权限问题，不能仅凭一次 `SIGSEGV` 判定原因为地址不规范。

## 16. 使用 GDB 和反汇编定位“范围”问题

对可运行 Demo，可以先查看无别名反汇编：

```bash
riscv64-linux-gnu-objdump -d -M no-aliases,numeric offset_bounds
riscv64-linux-gnu-objdump -d -M no-aliases,numeric lui_bounds
riscv64-linux-gnu-objdump -d -M no-aliases,numeric pcrel_rounding
riscv64-linux-gnu-objdump -d -M no-aliases,numeric branch_range
```

使用 GDB 时，先在一个终端启动，例如：

```bash
qemu-riscv64-static -g 1234 ./pcrel_rounding
```

另一个终端：

```bash
gdb-multiarch ./pcrel_rounding
```

连接并在 PC 相对地址形成后检查：

```gdb
target remote :1234
break after_pcrel
continue
p/x $t0
p/x $t1
p/x &target
x/gx $t0
```

`t0`、`t1` 与 `&target` 应相同，`x/gx $t0` 应读到 `0x1122334455667788`。调试 Demo 1 时可在 `after_bounds` 查看 `t2`、`t4`、`t5`；调试 Demo 2 时可在 `after_lui` 查看 `t0`。GDB 中看到的实际程序地址随 ELF 布局和运行环境而变化。

## 17. 常见错误

### 17.1 把立即数范围当成整个进程的地址空间

`LD` 只能编码 `-2048～2047` 字节偏移，但基址寄存器本身可由其他指令计算，不限制进程只能使用 4 KiB 内存。

### 17.2 忘记 RV64 `LUI` 会符号扩展

`lui rd, 0x80000` 得到 `0xffffffff80000000`。要 `0x0000000080000000` 需另行零扩展或使用合适的 `LI` 展开。

### 17.3 把 `0x800` 当成正的 12 位立即数

12 位有符号立即数最大为 `0x7ff`。PC 相对高低拆分遇到低部 `0x800～0xfff` 时，必须进位并使用负低部。

### 17.4 只检查汇编成功，不检查链接结果

跳转目标可能通过重定位到链接时才确定。Demo 5 的 `JAL` 正是汇编成功、链接失败。

### 17.5 看到远分支源码仍以为它是一条指令

Demo 4 中第二个 `BEQ` 源码被当前汇编器展开。应查看最终反汇编，而不是仅凭源码行数判断机器指令数。

### 17.6 把规范化地址当成已映射地址

Sv39/Sv48/Sv57 区间只描述位模式规则。页表、权限和操作系统布局仍决定访问是否成功。

### 17.7 用 QEMU User Mode 验证客体页表故障

QEMU User Mode 适合本文的指令与工具链 Demo；研究客体 MMU 的 Sv39 页故障应切换到客体系统或真实硬件环境。

### 17.8 手写 `_start` 时忽略 `gp` 相对寻址放宽

`la` 的最终形式还受链接器 relaxation 影响：链接器可能把访问附近符号的 PC 相对序列改成依赖 `gp` 的短指令。手写 `_start` 的程序如果没有初始化 `gp`，这种优化可能使地址错误；只检查程序是否退出 `0` 也未必能发现。第 [01 篇的访存 Demo](01-rv64-memory-instructions.md) 使用 `.option norelax` 并逐项检查结果，展示了规避和检测方法。这里的问题来自最终机器码采用了哪种地址构造方式，而不是 `LD/SD` 的 12 位偏移发生变化。

## 18. 本部分小结

判断一个地址或跳转目标是否可用，建议按以下顺序提问：

1. **编码**：当前单条指令能表示这个立即数或偏移吗？
2. **构造**：所用指令组合与代码模型能形成目标地址吗？
3. **规范化**：当前虚拟内存模式允许这个 64 位地址位模式吗？
4. **映射与权限**：目标页实际存在，且当前特权级可访问吗？

最值得记住的边界是：

```text
I/S/JALR 12 位字节偏移：-2048～2047
B-type 条件分支：       -4096～4094 字节
JAL：                    -1048576～1048574 字节
AUIPC + 12 位低部：      约 ±2 GiB，低部需有符号进位调整
RV64 虚拟地址：          取决于当前 Sv39/Sv48/Sv57 等模式和页表
```

规范依据：[RV32I 基础指令](https://docs.riscv.org/reference/isa/v20260120/unpriv/rv32.html)、[RV64I 地址构造](https://docs.riscv.org/reference/isa/unpriv/rv64.html)、[Supervisor 虚拟内存模式](https://docs.riscv.org/reference/isa/priv/supervisor.html)、[RISC-V ELF psABI 代码模型](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-elf.adoc)、[QEMU User Mode 文档](https://www.qemu.org/docs/master/user/main.html)。
