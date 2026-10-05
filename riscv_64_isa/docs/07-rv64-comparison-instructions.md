# RISC-V 64 位 ISA：Comparison Instructions

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准；“真实指令”表示有机器编码，但可能属于可选扩展或特权架构，并不表示仅用 `-march=rv64i` 就能汇编、执行。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `BEQ` | **Branch if Equal** | 真实指令 |
| `BEQZ` | **Branch if Equal to Zero** | 伪指令 |
| `BGE` | **Branch if Greater Than or Equal** | 真实指令 |
| `BGEU` | **Branch if Greater Than or Equal Unsigned** | 真实指令 |
| `BGEZ` | **Branch if Greater Than or Equal to Zero** | 伪指令 |
| `BGT` | **Branch if Greater Than** | 伪指令 |
| `BGTU` | **Branch if Greater Than Unsigned** | 伪指令 |
| `BLE` | **Branch if Less Than or Equal** | 伪指令 |
| `BLEU` | **Branch if Less Than or Equal Unsigned** | 伪指令 |
| `BLT` | **Branch if Less Than** | 真实指令 |
| `BLTU` | **Branch if Less Than Unsigned** | 真实指令 |
| `BLTZ` | **Branch if Less Than Zero** | 伪指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `BNEZ` | **Branch if Not Equal to Zero** | 伪指令 |
| `ECALL` | **Environment Call** | 真实指令 |
| `J` | **Jump** | 伪指令 |
| `JAL` | **Jump And Link** | 真实指令 |
| `LA` | **Load Address** | 伪指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `SEQZ` | **Set if Equal to Zero** | 伪指令 |
| `SEXT.W` | **Sign-Extend Word** | 伪指令 |
| `SGT` | **Set Greater Than** | 伪指令 |
| `SGTU` | **Set Greater Than Unsigned** | 伪指令 |
| `SGTZ` | **Set if Greater Than Zero** | 伪指令 |
| `SLLI` | **Shift Left Logical Immediate** | 真实指令 |
| `SLT` | **Set Less Than** | 真实指令 |
| `SLTI` | **Set Less Than Immediate** | 真实指令 |
| `SLTIU` | **Set Less Than Immediate Unsigned** | 真实指令 |
| `SLTU` | **Set Less Than Unsigned** | 真实指令 |
| `SLTZ` | **Set if Less Than Zero** | 伪指令 |
| `SNEZ` | **Set if Not Equal to Zero** | 伪指令 |
| `SRLI` | **Shift Right Logical Immediate** | 真实指令 |

ABI 别名是同一套 `x0～x31` 整数寄存器的另一种写法，并非额外寄存器；下面给出本文提及的整数寄存器编号与对应 ABI 别名。`PC` 若在本文出现，指 **Program Counter**（程序计数器），它不属于 `x0～x31`。

| ABI 别名 | 硬件编号 | 英文完整名称 |
|---|---|---|
| `zero` | `x0` | **Zero** |
| `t0` | `x5` | **Temporary register 0** |
| `t1` | `x6` | **Temporary register 1** |
| `t2` | `x7` | **Temporary register 2** |
| `s0` | `x8` | **Saved register 0 / Frame Pointer（作 `fp` 时）** |
| `s1` | `x9` | **Saved register 1** |
| `a0` | `x10` | **Argument register 0** |
| `a1` | `x11` | **Argument register 1** |
| `a2` | `x12` | **Argument register 2** |
| `a7` | `x17` | **Argument register 7** |
| `s2` | `x18` | **Saved register 2** |
| `s3` | `x19` | **Saved register 3** |
| `s4` | `x20` | **Saved register 4** |
| `s5` | `x21` | **Saved register 5** |
| `s6` | `x22` | **Saved register 6** |
| `s7` | `x23` | **Saved register 7** |
| `s8` | `x24` | **Saved register 8** |
| `s9` | `x25` | **Saved register 9** |
| `s10` | `x26` | **Saved register 10** |
| `s11` | `x27` | **Saved register 11** |
| `t3` | `x28` | **Temporary register 3** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

本文是 RISC-V 64 位 ISA 系列的下一部分，讲解 RV64I 的整数比较指令。示例延续前几篇的方式，可以直接汇编、运行、反汇编和调试。

本篇首先讲解四条把比较结果写入寄存器的真实指令：

```text
SLT   SLTU
SLTI  SLTIU
```

随后讲解六条直接根据比较结果跳转的条件分支指令：

```text
BEQ  BNE  BLT  BGE  BLTU  BGEU
```

`SEQZ/SNEZ/SLTZ/SGTZ`、`SGT/SGTU` 等常见写法是伪指令。本文会说明它们如何由基础指令实现，并解释有符号、无符号、立即数和 32 位数据比较中的易错点。

## 1. 比较结果如何表示

RV64I 没有供后续指令读取的通用条件码寄存器。使用 `SLT` 一类指令时，比较结果直接写入目标整数寄存器：

```text
条件成立：rd = 1
条件不成立：rd = 0
```

例如：

```asm
li  t0, 3
li  t1, 5
slt t2, t0, t1
```

因为 `3 < 5`，执行后 `t2 = 1`。结果是完整的 64 位整数 `0x0000000000000001`，不是仅修改其中某一位。

条件分支指令则在内部比较两个寄存器并决定是否跳转，不把布尔结果写入寄存器。

## 2. 有符号与无符号解释

同一组 64 位可以解释为有符号补码整数，也可以解释为无符号整数：

| 位模式 | 有符号 64 位 | 无符号 64 位 |
|---|---:|---:|
| `0x0000000000000001` | 1 | 1 |
| `0x7fffffffffffffff` | 最大正数 | 同一正数 |
| `0x8000000000000000` | 最小负数 | `2^63` |
| `0xffffffffffffffff` | -1 | `2^64-1` |

因此，比较 `-1` 和 `1` 时：

```text
有符号：-1 < 1           成立
无符号：2^64-1 < 1       不成立
```

RISC-V 用不同指令指定比较方式，而不是依赖寄存器里记录的“数据类型”。`SLT/SLTI/BLT/BGE` 按有符号数比较；带 `U` 的 `SLTU/SLTIU/BLTU/BGEU` 按无符号数比较。`BEQ/BNE` 只检查位模式是否相同，有符号和无符号解释不会影响结果。

## 3. 四条设置结果的指令

| 指令 | 语法 | 条件成立时写入 1，否则写入 0 |
|---|---|---|
| `SLT` | `slt rd, rs1, rs2` | 有符号 `rs1 < rs2` |
| `SLTU` | `sltu rd, rs1, rs2` | 无符号 `rs1 < rs2` |
| `SLTI` | `slti rd, rs1, imm12` | 有符号 `rs1 < sign_extend(imm12)` |
| `SLTIU` | `sltiu rd, rs1, imm12` | 无符号 `rs1 < unsigned(sign_extend(imm12))` |

寄存器形式是 R-type。立即数形式是 I-type，立即数范围为 `-2048～2047`。

所有四条指令在 RV64 上比较完整的 64 位寄存器值。RV64I 没有 `SLTW/SLTUW/SLTIW/SLTIUW` 这组 W 比较指令。

## 4. `SLT`：有符号寄存器比较

语法：

```asm
slt rd, rs1, rs2
```

例如：

```asm
li  t0, -1
li  t1, 1
slt t2, t0, t1
```

`SLT` 把两个源寄存器解释为有符号 64 位补码数。因为 `-1 < 1`，所以 `t2 = 1`。

再把操作数交换：

```asm
slt t3, t1, t0
```

因为 `1 < -1` 不成立，所以 `t3 = 0`。源寄存器不会被比较指令修改。

## 5. `SLTU`：无符号寄存器比较

语法：

```asm
sltu rd, rs1, rs2
```

仍然使用上面的位模式：

```asm
li   t0, -1
li   t1, 1
sltu t2, t0, t1
sltu t3, t1, t0
```

无符号解释下，`t0 = 0xffffffffffffffff` 是最大无符号 64 位数。因此：

```text
t2 = 0    # 最大无符号数 < 1：不成立
t3 = 1    # 1 < 最大无符号数：成立
```

`SLTU` 也能检查寄存器是否非零：

```asm
sltu t2, x0, t0
```

无符号数只有零不大于零，故 `t0 != 0` 时 `t2 = 1`。对应的伪指令是 `snez t2, t0`。

## 6. `SLTI`：有符号立即数比较

语法：

```asm
slti rd, rs1, imm12
```

立即数是 12 位有符号数，先符号扩展到 64 位，再按有符号数比较：

```asm
li   t0, -5
slti t1, t0, 0              # -5 < 0：t1 = 1
slti t2, t0, -5             # -5 < -5：t2 = 0
```

这条指令特别适合判断负数。也可以写 `sltz t1, t0` 表达同一个条件；它是 `slt t1, t0, x0` 的伪指令，并非 `SLTI` 的别名编码。

如果要与无法编码的常数比较，先使用 `LI` 装入寄存器，再使用 `SLT`：

```asm
li  t1, 5000
slt t2, t0, t1
```

## 7. `SLTIU`：无符号立即数比较

语法：

```asm
sltiu rd, rs1, imm12
```

这里最重要的顺序是：**先把 12 位立即数符号扩展到 64 位，再按无符号数比较。** `U` 指定比较方式，并不表示立即数零扩展。

例如：

```asm
li    t0, 1
sltiu t1, t0, -1
```

`-1` 符号扩展后的位模式是 `0xffffffffffffffff`。按无符号数比较时，`1 < 2^64-1` 成立，因此 `t1 = 1`。若 `t0` 本身也是 `-1`，两个位模式相等，结果为零。

一个常用的特殊写法是：

```asm
sltiu t1, t0, 1
```

无符号 64 位数中，只有零小于一。因此它在 `t0 == 0` 时写入 1，其余情况写入 0。伪指令为：

```asm
seqz t1, t0
```

不要把 `sltiu rd, rs, -1` 理解成“与无符号 12 位最大值 `4095` 比较”。比较右侧实际是完整 64 位的 `2^64-1`。

## 8. 由基础比较指令构成的伪指令

| 伪指令 | 实际指令 | 条件 |
|---|---|---|
| `seqz rd, rs` | `sltiu rd, rs, 1` | `rs == 0` |
| `snez rd, rs` | `sltu rd, x0, rs` | `rs != 0` |
| `sltz rd, rs` | `slt rd, rs, x0` | `rs < 0`，有符号 |
| `sgtz rd, rs` | `slt rd, x0, rs` | `rs > 0`，有符号 |
| `sgt rd, rs1, rs2` | `slt rd, rs2, rs1` | `rs1 > rs2`，有符号 |
| `sgtu rd, rs1, rs2` | `sltu rd, rs2, rs1` | `rs1 > rs2`，无符号 |

例如，基础指令用交换操作数来表达“大于”：

```asm
slt t2, t1, t0             # 有符号 t0 > t1
```

也可以在汇编源码中写成：

```asm
sgt t2, t0, t1
```

两种写法产生相同的基础指令。`SGT` 并没有独立的 RV64I 机器码。

## 9. 六条条件分支指令

如果只需根据比较结果选择执行路径，可以直接使用条件分支：

| 指令 | 语法 | 跳转条件 |
|---|---|---|
| `BEQ` | `beq rs1, rs2, label` | 位模式相等 |
| `BNE` | `bne rs1, rs2, label` | 位模式不等 |
| `BLT` | `blt rs1, rs2, label` | 有符号 `rs1 < rs2` |
| `BGE` | `bge rs1, rs2, label` | 有符号 `rs1 >= rs2` |
| `BLTU` | `bltu rs1, rs2, label` | 无符号 `rs1 < rs2` |
| `BGEU` | `bgeu rs1, rs2, label` | 无符号 `rs1 >= rs2` |

这些是 B-type 指令。条件成立时，程序从 `label` 继续；条件不成立时，继续执行下一条指令。目标由分支指令的 PC 相对偏移表示，正常编码的条件分支可到达约 ±4 KiB 的范围。

### 9.1 相等与不等

```asm
beq a0, a1, values_equal
bne a0, a1, values_differ
```

`BEQ/BNE` 检查两个寄存器的完整 64 位位模式。没有“有符号相等”和“无符号相等”两套指令。

### 9.2 有符号大小关系

```asm
blt a0, a1, signed_less
bge a0, a1, signed_ge
```

当 `a0 = -1`、`a1 = 1` 时，`BLT` 会跳转，`BGE` 不会跳转。

### 9.3 无符号大小关系

```asm
bltu a0, a1, unsigned_less
bgeu a0, a1, unsigned_ge
```

对于相同的位模式 `a0 = 0xffffffffffffffff`、`a1 = 1`，`BLTU` 不跳转，`BGEU` 会跳转。

## 10. “大于”和“小于等于”的分支写法

RV64I 没有独立编码的 `BGT/BLE/BGTU/BLEU`。交换两个操作数即可使用已有指令：

| 目标条件 | 基础指令 | 常见伪指令 |
|---|---|---|
| 有符号 `a > b` | `blt b, a, label` | `bgt a, b, label` |
| 有符号 `a <= b` | `bge b, a, label` | `ble a, b, label` |
| 无符号 `a > b` | `bltu b, a, label` | `bgtu a, b, label` |
| 无符号 `a <= b` | `bgeu b, a, label` | `bleu a, b, label` |

例如：

```asm
li  t0, 9
li  t1, 3
bgt t0, t1, greater         # 汇编成 blt t1, t0, greater
```

与零比较也有常见伪指令：

```asm
beqz t0, is_zero            # beq t0, x0, is_zero
bnez t0, not_zero           # bne t0, x0, not_zero
bltz t0, is_negative        # blt t0, x0, is_negative
bgez t0, nonnegative        # bge t0, x0, nonnegative
```

## 11. 什么时候保存比较结果

需要一个供后续代码使用的布尔值时，使用 `SLT/SLTU/SLTI/SLTIU`：

```asm
slt a0, a1, a2              # a0 = (a1 < a2) ? 1 : 0
```

只需要跳转时，直接使用对应的分支通常更直观：

```asm
blt a1, a2, less_than
```

两种写法可以组合，但不是必须先执行 `SLT` 才能分支。条件分支自己读取并比较源寄存器：

```asm
slt  t0, a0, a1
bnez t0, less_than
```

这段代码有用的场景是 `t0` 的比较结果还要供别处使用。若只是跳转，单条 `BLT` 已能表达相同条件。

## 12. RV64 中比较 32 位数据

RV64I 的比较指令处理完整的 64 位寄存器，**没有 W 比较指令**。若程序只关心低 32 位，需要先把两个操作数转换为一致的 64 位表示。

### 12.1 有符号 32 位比较

把两个操作数的低 32 位符号扩展后，使用 `SLT` 或 `BLT`：

```asm
sext.w t0, a0
sext.w t1, a1
slt    t2, t0, t1
```

例如，低 32 位 `0xffffffff` 表示有符号 `-1`；符号扩展后为 `0xffffffffffffffff`，与 `1` 比较会得到 `-1 < 1`。

### 12.2 无符号 32 位比较

最直接的做法是把两个操作数的低 32 位都零扩展，再使用 `SLTU` 或 `BLTU`：

```asm
slli t0, a0, 32
srli t0, t0, 32
slli t1, a1, 32
srli t1, t1, 32
sltu t2, t0, t1
```

若两边**已经统一符号扩展**为 64 位，也可以直接用 `SLTU` 比较无符号 32 位数：符号扩展后的 `0x80000000～0xffffffff` 都落在无符号 64 位范围的高端，相对顺序仍然正确。混用零扩展与符号扩展的操作数则可能产生错误结果。

例如，低 32 位 `0x80000000` 与 `0xffffffff`：

```text
都零扩展：0x0000000080000000 < 0x00000000ffffffff
都符号扩展：0xffffffff80000000 < 0xffffffffffffffff
```

两种一致的表示得到相同的无符号 32 位顺序。

## 13. 完整 Comparison Instructions Demo

程序覆盖四条设置结果指令、六条真实条件分支，并验证伪指令和有符号/无符号边界。

保存为 `comparison_demo.S`：

下面代码中，以点开头的名称是汇编伪操作，用来设置选项、布置节/符号或生成数据；它们不是 RV64 CPU 指令。`la`、`li` 等没有点号的写法则可能是由汇编器展开的伪指令（见第 0 节）。本例没有使用 `.macro`/`.endm` 定义用户宏。[RISC-V 汇编手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)、[GNU as 伪操作手册](https://sourceware.org/binutils/docs/as/Pseudo-Ops.html)

```asm
    .option nopic  # 后续 la 使用非 PIC 形式；局部符号可用 PC 相对寻址，不经 GOT，也不强制绝对地址

    .section .rodata  # 切换到只读数据节，后续整数或字符串常量放在这里
pass_message:
    .ascii "Comparison instruction demo passed!\n"  # 按原样写入字符串字节；\n 是换行，不会自动追加 NUL 结束字节
    .equ pass_message_len, . - pass_message  # 汇编期定义长度常量：点号是当前位置，减去字符串起始标签得到字节数；不分配存储

fail_message:
    .ascii "Comparison instruction demo failed!\n"  # 按原样写入字符串字节；\n 是换行，不会自动追加 NUL 结束字节
    .equ fail_message_len, . - fail_message  # 汇编期定义长度常量：点号是当前位置，减去字符串起始标签得到字节数；不分配存储

    .section .text  # 切换到可执行代码节，后续指令在这里编码
    .globl _start  # 将 _start 标成 ELF 全局符号；入口地址仍由链接器的 -e 或 ENTRY 指定

_start:
    # 1. 同一位模式的有符号/无符号比较
    li    t0, -1  # 伪指令：把常量构造到寄存器，大常量可能需要多条真实指令
    li    t1, 1
    slt   s0, t0, t1          # -1 < 1：1
    sltu  s1, t0, t1          # 2^64-1 < 1：0
    slti  s2, t0, 0           # -1 < 0：1
    sltiu s3, t1, -1          # 1 < 2^64-1：1
    sltiu s4, t0, -1          # 2^64-1 < 2^64-1：0

after_set_ops:
    li  t2, 1
    bne s0, t2, failed
    bne s2, t2, failed
    bne s3, t2, failed
    bne s1, x0, failed
    bne s4, x0, failed

    # 2. 伪指令：零判断、符号判断和大于
    seqz s5, x0               # 1
    snez s6, t0               # 1
    sltz s7, t0               # 1
    sgtz s8, t1               # 1
    sgt  s9, t1, t0           # 有符号 1 > -1：1
    sgtu s10, t0, t1          # 无符号 2^64-1 > 1：1

after_pseudo_ops:
    li  t2, 1
    bne s5, t2, failed
    bne s6, t2, failed
    bne s7, t2, failed
    bne s8, t2, failed
    bne s9, t2, failed
    bne s10, t2, failed

    # 3. 六条真实条件分支的成立路径
    beq t0, t0, equal_ok
    j failed
equal_ok:
    bne t0, t1, not_equal_ok
    j failed
not_equal_ok:
    blt t0, t1, signed_less_ok
    j failed
signed_less_ok:
    bge t1, t0, signed_ge_ok
    j failed
signed_ge_ok:
    bltu t1, t0, unsigned_less_ok
    j failed
unsigned_less_ok:
    bgeu t0, t1, unsigned_ge_ok
    j failed
unsigned_ge_ok:
    # 同样验证两个不成立的条件不会跳转
    bge t0, t1, failed
    bltu t0, t1, failed

after_branch_ops:
    # 4. 先零扩展两个低 32 位，再按无符号数比较
    li   t0, 0x80000000
    li   t1, 0xffffffff
    slli t0, t0, 32
    srli t0, t0, 32
    slli t1, t1, 32
    srli t1, t1, 32
    sltu s11, t0, t1          # 0x80000000 < 0xffffffff：1

after_word_compare:
    li  t2, 1
    bne s11, t2, failed

    li a0, 1
    la a1, pass_message  # 伪指令：构造符号地址，不读取该地址的内容；可能展开为 AUIPC+ADDI 等
    li a2, pass_message_len
    li a7, 64                 # write(1, message, length)
    ecall

    li a0, 0
    li a7, 93                 # exit(0)
    ecall

failed:
    li a0, 1
    la a1, fail_message
    li a2, fail_message_len
    li a7, 64
    ecall

    li a0, 1
    li a7, 93                 # exit(1)
    ecall
```

## 14. 汇编、链接和运行

```bash
riscv64-linux-gnu-as \
    -march=rv64i \
    -mabi=lp64 \
    -g \
    -o comparison_demo.o \
    comparison_demo.S

riscv64-linux-gnu-ld \
    -m elf64lriscv \
    -static \
    -e _start \
    -o comparison_demo \
    comparison_demo.o
```

运行并查看退出状态：

```bash
qemu-riscv64-static ./comparison_demo
echo $?
```

预期：

```text
Comparison instruction demo passed!
0
```

任一检查失败时，程序输出 `Comparison instruction demo failed!`，然后以状态 1 退出。

## 15. 查看真实指令

查看源码、机器码和反汇编：

```bash
riscv64-linux-gnu-objdump -drS comparison_demo
```

关闭伪指令别名：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    comparison_demo
```

重点寻找 `seqz` 对应的 `sltiu`、`snez` 对应的 `sltu`，以及 `sgt/sgtu` 对应的操作数交换后的 `slt/sltu`。程序中的 `j` 也只是无条件跳转伪指令，通常显示为 `jal x0, label`。

## 16. 使用 GDB 验证结果

终端一：

```bash
qemu-riscv64-static -g 1234 ./comparison_demo
```

终端二：

```bash
gdb-multiarch ./comparison_demo
```

连接并设置断点：

```gdb
target remote :1234
break after_set_ops
break after_pseudo_ops
break after_branch_ops
break after_word_compare
continue
```

第一个断点：

```gdb
info registers s0 s1 s2 s3 s4
```

预期依次为 `1、0、1、1、0`。继续到第二个断点：

```gdb
continue
info registers s5 s6 s7 s8 s9 s10
```

六个寄存器都应为 `1`。继续到分支结束后，可检查程序是否到达 `after_branch_ops`；再继续到 32 位比较结束：

```gdb
continue
continue
info registers s11
```

`s11` 应为 `1`。可使用 `display/i $pc` 和 `si` 单步观察分支是否跳转。

## 17. 常见错误

### 17.1 把 `SLTIU` 的立即数当成零扩展

`SLTIU` 的立即数仍先符号扩展。`-1` 在 RV64 上参与无符号比较时是 `0xffffffffffffffff`。

### 17.2 忘记 `BLT` 与 `BLTU` 的区别

对于位模式 `0xffffffffffffffff` 与 `1`，有符号 `BLT` 和无符号 `BLTU` 的结果相反。选择指令时要明确数据的解释方式。

### 17.3 以为比较会设置条件码

`SLT` 把 0 或 1 写入目标寄存器；条件分支自己比较源寄存器。后续分支不会自动读取上一次 `SLT` 的结果。

### 17.4 直接比较未统一扩展的 32 位数

RV64I 的比较宽度是 64 位。比较低 32 位时，先让两个输入统一零扩展或统一符号扩展，再选择所需的有符号或无符号指令。

### 17.5 假设有 W 比较指令

RV64I 没有 `SLTW/SLTUW`。`SLT/SLTU` 始终比较完整的 64 位寄存器。

### 17.6 把分支伪指令当作独立编码

`BGT/BLE/BGTU/BLEU` 通过交换源操作数映射到 `BLT/BGE/BLTU/BGEU`，不需要额外的比较指令编码。

## 18. 本部分小结

```text
把比较结果写入寄存器：SLT SLTU SLTI SLTIU
比较并直接跳转：    BEQ BNE BLT BGE BLTU BGEU
```

最需要记住的规则是：

1. 设置结果的比较指令只写入完整整数 `0` 或 `1`。
2. `SLT/SLTI/BLT/BGE` 使用有符号顺序；带 `U` 的对应指令使用无符号顺序。
3. `SLTIU` 的立即数先符号扩展，再按无符号数参与比较。
4. `BEQ/BNE` 只检查位模式是否相同。
5. RV64I 没有 W 比较指令；比较 32 位数据前，要统一其 64 位表示。
6. `SEQZ/SNEZ/SGT/BGT` 等方便写法属于伪指令。

规范依据：[RV32I 基础整数与条件分支指令](https://docs.riscv.org/reference/isa/v20240411/unpriv/rv32.html)、[RV64I 的 32 位数表示约定](https://docs.riscv.org/reference/isa/unpriv/rv64.html)、[RISC-V 汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。
