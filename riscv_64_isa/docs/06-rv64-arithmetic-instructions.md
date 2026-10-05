# RISC-V 64 位 ISA：Arithmetic Instructions

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准；“真实指令”表示有机器编码，但可能属于可选扩展或特权架构，并不表示仅用 `-march=rv64i` 就能汇编、执行。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `ADD` | **Add** | 真实指令 |
| `ADDI` | **Add Immediate** | 真实指令 |
| `ADDIW` | **Add Immediate Word** | 真实指令 |
| `ADDW` | **Add Word** | 真实指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `DIV` | **Divide** | 真实指令（M 扩展） |
| `DIVU` | **Divide Unsigned** | 真实指令（M 扩展） |
| `DIVUW` | **Divide Unsigned Word** | 真实指令（M 扩展） |
| `DIVW` | **Divide Word** | 真实指令（M 扩展） |
| `ECALL` | **Environment Call** | 真实指令 |
| `LA` | **Load Address** | 伪指令 |
| `LD` | **Load Doubleword** | 真实指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `MUL` | **Multiply** | 真实指令（M 扩展） |
| `MULH` | **Multiply High** | 真实指令（M 扩展） |
| `MULW` | **Multiply Word** | 真实指令（M 扩展） |
| `MV` | **Move** | 伪指令 |
| `NEG` | **Negate** | 伪指令 |
| `NEGW` | **Negate Word** | 伪指令 |
| `NOP` | **No Operation** | 伪指令 |
| `REM` | **Remainder** | 真实指令（M 扩展） |
| `REMU` | **Remainder Unsigned** | 真实指令（M 扩展） |
| `REMUW` | **Remainder Unsigned Word** | 真实指令（M 扩展） |
| `REMW` | **Remainder Word** | 真实指令（M 扩展） |
| `SD` | **Store Doubleword** | 真实指令 |
| `SEXT.W` | **Sign-Extend Word** | 伪指令 |
| `SLLI` | **Shift Left Logical Immediate** | 真实指令 |
| `SLTU` | **Set Less Than Unsigned** | 真实指令 |
| `SRLI` | **Shift Right Logical Immediate** | 真实指令 |
| `SUB` | **Subtract** | 真实指令 |
| `SUBW` | **Subtract Word** | 真实指令 |

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
| `t3` | `x28` | **Temporary register 3** |
| `t4` | `x29` | **Temporary register 4** |
| `t5` | `x30` | **Temporary register 5** |
| `t6` | `x31` | **Temporary register 6** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

本文是 RISC-V 64 位 ISA 系列的下一部分，讲解 RV64I 基础整数指令集中的加法和减法，并提供可以直接汇编、运行、反汇编和调试的完整示例。

本篇聚焦 6 条真实指令：

```text
64 位结果：ADD  SUB  ADDI
32 位结果：ADDW SUBW ADDIW
```

同时讲解 `NEG`、`NEGW`、`SEXT.W` 等常见伪指令，以及立即数、溢出、进位和内存更新的规则。乘法和除法属于可选的 M 扩展，本篇最后说明它们与 RV64I 的边界。

## 1. 先理解运算宽度

RV64 的整数寄存器宽度是 64 位。普通 `ADD/SUB/ADDI` 计算完整的 64 位结果；带 `W` 后缀的指令只保留低 32 位结果，再把结果的 bit 31 符号扩展到 64 位。

```text
普通加减：结果 = 运算结果 mod 2^64

W 加减：  先取运算结果的低 32 位
         再把 bit 31 复制到结果的 bits [63:32]
```

例如，32 位结果为 `0x80000000` 时，W 指令写入寄存器的是：

```text
0xffffffff80000000
```

它不会写成 `0x0000000080000000`。这条规则与上一章讲解的 W 移位指令一致。

## 2. 指令汇总

| 指令 | 语法 | 运算规则 |
|---|---|---|
| `ADD` | `add rd, rs1, rs2` | `rd = (rs1 + rs2) mod 2^64` |
| `SUB` | `sub rd, rs1, rs2` | `rd = (rs1 - rs2) mod 2^64` |
| `ADDI` | `addi rd, rs1, imm12` | `rd = (rs1 + sign_extend(imm12)) mod 2^64` |
| `ADDW` | `addw rd, rs1, rs2` | `rd = sign_extend_64((rs1 + rs2)[31:0])` |
| `SUBW` | `subw rd, rs1, rs2` | `rd = sign_extend_64((rs1 - rs2)[31:0])` |
| `ADDIW` | `addiw rd, rs1, imm12` | `rd = sign_extend_64((rs1 + sign_extend(imm12))[31:0])` |

`ADD/SUB/ADDW/SUBW` 是 R-type，两个输入都来自寄存器。`ADDI/ADDIW` 是 I-type，第二个输入是 12 位有符号立即数。

立即数范围是：

```text
-2048 ～ 2047
```

`SUBI` 和 `SUBIW` 不是 RV64I 的真实指令。减去小的常数通常使用负立即数的 `ADDI` 或 `ADDIW`。

## 3. `ADD`：64 位寄存器加法

语法：

```asm
add rd, rs1, rs2
```

例如：

```asm
li  t0, 17
li  t1, 25
add t2, t0, t1
```

结果：

```text
t2 = 42 = 0x000000000000002a
```

`ADD` 只保存低 64 位。若数学结果超出 64 位，超过 bit 63 的部分被丢弃：

```asm
li  t0, -1                   # 位模式为 0xffffffffffffffff
li  t1, 1
add t2, t0, t1
```

结果为零。把 `t0` 解释为无符号数时，这是最大无符号数加一；解释为有符号数时，`-1 + 1 = 0`。指令执行的位级运算相同，解释方式由程序决定。

## 4. `SUB`：64 位寄存器减法

语法：

```asm
sub rd, rs1, rs2
```

注意操作数顺序是 `rs1 - rs2`：

```asm
li  t0, 17
li  t1, 25
sub t2, t0, t1
```

结果：

```text
17 - 25 = -8
t2 = 0xfffffffffffffff8
```

与加法一样，减法结果按 64 位截断。例如：

```asm
sub t0, x0, t1             # t0 = -t1，按 64 位取模
```

汇编器提供更直观的伪指令：

```asm
neg t0, t1
```

`NEG` 的真实指令是 `SUB rd, x0, rs`。

## 5. `ADDI`：64 位立即数加法

语法：

```asm
addi rd, rs1, imm12
```

立即数先按 12 位有符号数解释，再符号扩展到 64 位，最后与 `rs1` 相加：

```asm
li   t0, 17
addi t1, t0, 5              # t1 = 22
addi t2, t0, -5             # t2 = 12
```

负立即数也能完成减法：

```asm
addi t0, t0, -1             # t0 = t0 - 1
```

但是 `-2048` 的相反数 `+2048` 超过 `ADDI` 范围。不能把所有常数减法机械地改写为一条 `ADDI`。对于 `t0 - 2048`，可以先把常数装入寄存器：

```asm
li  t1, 2048
sub t0, t0, t1
```

若立即数本身超出范围，例如 `addi t0, t1, 4096`，汇编器通常报错。可以改用 `li` 加 `add`，或按实际常数选择合适的指令组合。

## 6. `ADDW`：32 位寄存器加法

语法：

```asm
addw rd, rs1, rs2
```

它只使用两个源操作数的低 32 位，32 位加法后进行符号扩展：

```asm
li   t0, 0x000000017fffffff
li   t1, 1
addw t2, t0, t1
```

计算过程：

```text
t0[31:0] = 0x7fffffff
t1[31:0] = 0x00000001
低 32 位和 = 0x80000000
t2 = 0xffffffff80000000
```

`t0` 的高 32 位 `0x00000001` 不参与计算。最终结果的高 32 位来自低 32 位结果的 bit 31。

作为对照，同样的输入使用 `ADD`：

```asm
add t3, t0, t1
```

得到：

```text
t3 = 0x0000000180000000
```

## 7. `SUBW`：32 位寄存器减法

语法：

```asm
subw rd, rs1, rs2
```

例如：

```asm
li   t0, 0x0000000100000000
li   t1, 1
subw t2, t0, t1
```

只看低 32 位：

```text
0x00000000 - 0x00000001 = 0xffffffff（低 32 位）
t2 = 0xffffffffffffffff = -1
```

若使用普通 `SUB`，结果则是 `0x00000000ffffffff`。这两个 64 位寄存器值的低 32 位相同，高 32 位不同。

`NEGW rd, rs` 是 `SUBW rd, x0, rs` 的伪指令。它先按 32 位求负，再把 32 位结果符号扩展到 64 位。

## 8. `ADDIW`：32 位立即数加法

语法：

```asm
addiw rd, rs1, imm12
```

`ADDIW` 与 `ADDI` 使用相同范围的 12 位有符号立即数，但最终只保留低 32 位结果并符号扩展。

```asm
li    t0, 0x000000007fffffff
addiw t1, t0, 1
```

结果：

```text
t1 = 0xffffffff80000000
```

立即数为零时，它仍然会处理符号扩展：

```asm
li    t0, 0x00000000ffffffff
addiw t1, t0, 0
```

结果：

```text
t1 = 0xffffffffffffffff
```

因此 `ADDIW rd, rs, 0` 可以用来符号扩展低 32 位。其常见伪指令为：

```asm
sext.w t1, t0
```

相比之下，`ADDI rd, rs, 0` 保留完整 64 位，通常作为 `MV rd, rs` 的展开形式。

## 9. 为什么 W 结果总要符号扩展

W 指令的行为可以分成两步：

```text
1. 只计算结果的低 32 位。
2. 将低 32 位结果的 bit 31 复制到 bits [63:32]。
```

| 低 32 位结果 | bit 31 | 写入 RV64 寄存器的结果 |
|---|---:|---|
| `0x00000001` | 0 | `0x0000000000000001` |
| `0x7fffffff` | 0 | `0x000000007fffffff` |
| `0x80000000` | 1 | `0xffffffff80000000` |
| `0xffffffff` | 1 | `0xffffffffffffffff` |

这里的“符号扩展”是位级规则。即使程序把低 32 位解释为无符号整数，W 指令仍然这样写入 64 位寄存器。读取无符号 32 位数值时，应按低 32 位解释；若需要零扩展后的 64 位数值，可以使用上一章的移位组合：

```asm
slli t0, t0, 32
srli t0, t0, 32
```

## 10. 补码、回绕和溢出

加减法的硬件结果按位宽取模。指令本身不区分有符号与无符号操作数，也不因为数学结果超出范围而自动陷入异常。

### 10.1 64 位有符号上界

```asm
li   t0, 0x7fffffffffffffff
addi t1, t0, 1
```

结果：

```text
t1 = 0x8000000000000000
```

若按有符号 64 位解释，结果从最大正数回绕为最小负数。

### 10.2 64 位无符号上界

```asm
li   t0, -1
addi t1, t0, 1
```

结果是零。若按无符号 64 位解释，这是从 `2^64-1` 回绕到零。

### 10.3 检查无符号加法进位

对于两个无符号 64 位寄存器值，完成加法后可通过结果是否小于一个输入来判断是否有进位：

```asm
add  t0, a0, a1
sltu t1, t0, a0              # t1 = 1 表示加法产生进位
```

`SLTU` 是整数比较指令。该检查假设 `a0` 和 `a1` 是本次加法的原始输入，且在比较之前未被覆盖。RISC-V 没有专门的进位标志寄存器供后续读取。

有符号溢出与无符号进位不是同一条件。例如 `0x7fffffffffffffff + 1` 有有符号溢出，但没有无符号进位。需要检测有符号溢出时，必须另外比较输入和结果的符号。

## 11. 常见伪指令与编码

| 写法 | 对应的真实指令 | 作用 |
|---|---|---|
| `neg rd, rs` | `sub rd, x0, rs` | 64 位求负 |
| `negw rd, rs` | `subw rd, x0, rs` | 32 位求负，再符号扩展 |
| `sext.w rd, rs` | `addiw rd, rs, 0` | 符号扩展低 32 位 |
| `mv rd, rs` | `addi rd, rs, 0` | 复制完整 64 位寄存器值 |
| `nop` | `addi x0, x0, 0` | 不改变可见的寄存器状态 |

`LI` 是装载常数的伪指令，可能展开为一条或多条真实指令。使用无别名反汇编可以区分源码写法和实际编码。

## 12. 加减法与地址计算

寄存器地址也可以使用 `ADD/ADDI` 计算：

```asm
addi t0, a0, 8              # t0 = a0 + 8
ld   t1, 0(t0)
```

或在地址偏移能放进 12 位有符号立即数时，直接使用加载指令的偏移：

```asm
ld t1, 8(a0)
```

地址需要完整的 64 位结果。`ADDW/ADDIW` 会丢弃计算结果的高 32 位，并按 bit 31 重建高位，一般不能拿来替代 64 位指针计算。

与上一章的位操作类似，算术指令只处理寄存器。要给内存中的 64 位数加一，需要加载、计算、写回：

```asm
ld   t0, 0(a0)
addi t0, t0, 1
sd   t0, 0(a0)
```

这个三条指令的序列不是原子操作。多个线程同时修改共享内存时，需要 A 扩展的原子指令或其他同步机制。

## 13. 完整 Arithmetic Instructions Demo

下面的程序覆盖全部 6 条 RV64I 基础加减法指令，验证 64 位回绕、W 指令忽略高 32 位、W 结果符号扩展，以及常见伪指令。

保存为 `arithmetic_demo.S`：

下面代码中，以点开头的名称是汇编伪操作，用来设置选项、布置节/符号或生成数据；它们不是 RV64 CPU 指令。`la`、`li` 等没有点号的写法则可能是由汇编器展开的伪指令（见第 0 节）。本例没有使用 `.macro`/`.endm` 定义用户宏。[RISC-V 汇编手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)、[GNU as 伪操作手册](https://sourceware.org/binutils/docs/as/Pseudo-Ops.html)

```asm
    .option nopic  # 后续 la 使用非 PIC 形式；局部符号可用 PC 相对寻址，不经 GOT，也不强制绝对地址

    .section .rodata  # 切换到只读数据节，后续整数或字符串常量放在这里
pass_message:
    .ascii "Arithmetic instruction demo passed!\n"  # 按原样写入字符串字节；\n 是换行，不会自动追加 NUL 结束字节
    .equ pass_message_len, . - pass_message  # 汇编期定义长度常量：点号是当前位置，减去字符串起始标签得到字节数；不分配存储

fail_message:
    .ascii "Arithmetic instruction demo failed!\n"  # 按原样写入字符串字节；\n 是换行，不会自动追加 NUL 结束字节
    .equ fail_message_len, . - fail_message  # 汇编期定义长度常量：点号是当前位置，减去字符串起始标签得到字节数；不分配存储

    .section .text  # 切换到可执行代码节，后续指令在这里编码
    .globl _start  # 将 _start 标成 ELF 全局符号；入口地址仍由链接器的 -e 或 ENTRY 指定

_start:
    # 1. 64 位 ADD、SUB、ADDI
    li   t0, 17  # 伪指令：把常量构造到寄存器，大常量可能需要多条真实指令
    li   t1, 25
    add  s0, t0, t1           # 42
    sub  s1, t0, t1           # -8
    addi s2, t0, -5           # 12

after_64bit_ops:
    li  t2, 42
    bne s0, t2, failed
    li  t2, -8
    bne s1, t2, failed
    li  t2, 12
    bne s2, t2, failed

    # 2. 64 位回绕
    li   t0, 0x7fffffffffffffff
    addi s3, t0, 1            # 0x8000000000000000
    li   t1, -1
    sub  s4, x0, t1           # 0 - (-1) = 1

after_wraparound:
    li  t2, 0x8000000000000000
    bne s3, t2, failed
    li  t2, 1
    bne s4, t2, failed

    # 3. W 指令只保留低 32 位，并符号扩展
    li   t0, 0x000000017fffffff
    li   t1, 1
    addw s5, t0, t1           # 0xffffffff80000000
    add  s6, t0, t1           # 0x0000000180000000

    li   t0, 0x0000000100000000
    subw s7, t0, t1           # 0xffffffffffffffff
    sub  s8, t0, t1           # 0x00000000ffffffff

    li    t0, 0x00000000ffffffff
    addiw s9, t0, 0           # 0xffffffffffffffff
    li    t0, 0x000000007fffffff
    addiw s10, t0, 1          # 0xffffffff80000000

after_w_ops:
    li  t2, 0xffffffff80000000
    bne s5, t2, failed
    li  t2, 0x0000000180000000
    bne s6, t2, failed
    li  t2, -1
    bne s7, t2, failed
    li  t2, 0x00000000ffffffff
    bne s8, t2, failed
    li  t2, -1
    bne s9, t2, failed
    li  t2, 0xffffffff80000000
    bne s10, t2, failed

    # 4. 伪指令与负立即数
    li   t0, 5
    neg  t1, t0               # SUB t1, x0, t0：-5
    negw t2, t0               # SUBW t2, x0, t0：-5
    addi t3, t0, -1           # 4
    li   t4, 0x00000000ffffffff
    sext.w t5, t4             # ADDIW t5, t4, 0：-1

after_pseudo_ops:
    li  t6, -5
    bne t1, t6, failed
    bne t2, t6, failed
    li  t6, 4
    bne t3, t6, failed
    li  t6, -1
    bne t5, t6, failed

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
    -o arithmetic_demo.o \
    arithmetic_demo.S

riscv64-linux-gnu-ld \
    -m elf64lriscv \
    -static \
    -e _start \
    -o arithmetic_demo \
    arithmetic_demo.o
```

运行：

```bash
qemu-riscv64-static ./arithmetic_demo
echo $?
```

预期输出与退出状态：

```text
Arithmetic instruction demo passed!
0
```

如果任意检查失败，程序会输出 `Arithmetic instruction demo failed!`，并通过 `exit(1)` 退出。

## 15. 查看真实指令

查看机器码和反汇编：

```bash
riscv64-linux-gnu-objdump -drS arithmetic_demo
```

关闭伪指令别名：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    arithmetic_demo
```

重点观察源码中的 `neg`、`negw` 和 `sext.w`。无别名反汇编中，它们分别显示为 `sub`、`subw` 和 `addiw`。`li`、`la` 也可能展开成多条真实指令。

## 16. 使用 GDB 验证结果

终端一：

```bash
qemu-riscv64-static -g 1234 ./arithmetic_demo
```

终端二：

```bash
gdb-multiarch ./arithmetic_demo
```

连接并设置断点：

```gdb
target remote :1234
break after_64bit_ops
break after_wraparound
break after_w_ops
break after_pseudo_ops
continue
```

第一个断点检查普通加减法：

```gdb
info registers s0 s1 s2
```

预期为 `42`、`-8` 对应的位模式 `0xfffffffffffffff8`、`12`。继续到第二个断点：

```gdb
continue
p/x $s3
```

预期为 `0x8000000000000000`。继续到 W 指令断点：

```gdb
continue
p/x $s5
p/x $s6
p/x $s7
p/x $s8
p/x $s9
p/x $s10
```

其中 `s5` 为 `0xffffffff80000000`，`s6` 为 `0x0000000180000000`，最能看出 `ADDW` 与 `ADD` 的区别。再继续到伪指令断点：

```gdb
continue
info registers t1 t2 t3 t5
```

预期依次为 `-5`、`-5`、`4` 和 `-1`。可以用 `display/i $pc` 和 `si` 单步观察实际执行的指令。

## 17. M 扩展与基础算术指令的边界

RV64I 基础整数加减法就是本篇的 6 条真实指令。常见的乘法和除法指令，例如 `MUL/MULH/DIV/DIVU/REM/REMU`，以及 RV64 对应的 `MULW/DIVW/DIVUW/REMW/REMUW`，属于 M 扩展。

这些指令不能在 `-march=rv64i` 下直接使用。若程序需要 M 扩展，可将汇编目标改为 `-march=rv64im`，并确保运行环境支持它。`MUL` 之类的指令不是 `ADD` 的伪指令，也不能靠一条基础 RV64I 指令完成。

## 18. 常见错误

### 18.1 把 `ADDW` 当成普通 64 位加法

`ADDW` 忽略输入高 32 位，结果再符号扩展。要计算完整的 64 位数或地址，使用 `ADD`。

### 18.2 以为 W 指令零扩展

低 32 位结果的 bit 31 为 1 时，高 32 位全部为 1。如果需要零扩展，需要额外执行零扩展步骤。

### 18.3 以为运算溢出会触发异常

基础加减法按 64 位或 32 位结果回绕。是否需要检测有符号溢出或无符号进位，由程序决定。

### 18.4 混淆 `ADDI` 与 `ADDIW` 的零立即数

```asm
addi  t0, t1, 0             # 保留完整 64 位
addiw t0, t1, 0             # 低 32 位符号扩展
```

两个结果只在 `t1` 已符合 32 位符号扩展形式时才一定相同。

### 18.5 使用超出范围的立即数

`ADDI/ADDIW` 的立即数只有 `-2048～2047`。大常数先用 `LI` 装入寄存器，再使用 `ADD/SUB/ADDW/SUBW`。

### 18.6 把加载、加法、存储当成原子自增

`LD + ADDI + SD` 有可能与其他线程交错。共享计数器的原子自增需要 A 扩展的原子指令或同步机制。

## 19. 本部分小结

```text
完整 64 位运算：ADD、SUB、ADDI
低 32 位运算后符号扩展：ADDW、SUBW、ADDIW
```

最需要记住的规则是：

1. 基础加减法按结果位宽取模，不自动报告溢出。
2. `ADDI/ADDIW` 使用符号扩展的 12 位立即数。
3. W 指令只保留低 32 位结果，并把 bit 31 复制到高 32 位。
4. `ADDIW rd, rs, 0` 可以符号扩展低 32 位；`ADDI rd, rs, 0` 保留完整 64 位。
5. `NEG/NEGW/SEXT.W` 是伪指令，乘除法属于可选的 M 扩展。

规范依据：[RV32I 基础整数指令](https://docs.riscv.org/reference/isa/unpriv/rv32.html)、[RV64I 基础整数指令](https://docs.riscv.org/reference/isa/unpriv/rv64.html)、[M 扩展](https://docs.riscv.org/reference/isa/unpriv/m-st-ext.html)。
