# RISC-V 64 位 ISA：Shift Instructions

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `ADD` | **Add** | 真实指令 |
| `ADDIW` | **Add Immediate Word** | 真实指令 |
| `ANDI` | **AND Immediate** | 真实指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `DIV` | **Divide** | 真实指令（M 扩展） |
| `ECALL` | **Environment Call** | 真实指令 |
| `LA` | **Load Address** | 伪指令 |
| `LD` | **Load Doubleword** | 真实指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `OR` | **OR** | 真实指令 |
| `ROL` | **Rotate Left** | 真实指令 |
| `ROR` | **Rotate Right** | 真实指令 |
| `RORI` | **Rotate Right Immediate** | 真实指令（Zbb 扩展） |
| `SEXT.W` | **Sign-Extend Word** | 伪指令 |
| `SLL` | **Shift Left Logical** | 真实指令 |
| `SLLI` | **Shift Left Logical Immediate** | 真实指令 |
| `SLLIW` | **Shift Left Logical Immediate Word** | 真实指令 |
| `SLLW` | **Shift Left Logical Word** | 真实指令 |
| `SRA` | **Shift Right Arithmetic** | 真实指令 |
| `SRAI` | **Shift Right Arithmetic Immediate** | 真实指令 |
| `SRAIW` | **Shift Right Arithmetic Immediate Word** | 真实指令 |
| `SRAW` | **Shift Right Arithmetic Word** | 真实指令 |
| `SRL` | **Shift Right Logical** | 真实指令 |
| `SRLI` | **Shift Right Logical Immediate** | 真实指令 |
| `SRLIW` | **Shift Right Logical Immediate Word** | 真实指令 |
| `SRLW` | **Shift Right Logical Word** | 真实指令 |
| `ZEXT.W` | **Zero-Extend Word** | 伪指令 |

ABI 别名是同一套 `x0～x31` 整数寄存器的另一种写法，并非额外寄存器；下面给出本文提及的整数寄存器编号与对应 ABI 别名。`PC` 若在本文出现，指 **Program Counter**（程序计数器），它不属于 `x0～x31`。

| ABI 别名 | 硬件编号 | 英文完整名称 |
|---|---|---|
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
| `t3` | `x28` | **Temporary register 3** |
| `t4` | `x29` | **Temporary register 4** |
| `t5` | `x30` | **Temporary register 5** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

本文是 RISC-V 64 位 ISA 系列的第三部分，集中讲解 RV64I 中的整数移位指令，并提供可以直接汇编、运行、反汇编和调试的完整示例。

移位操作将一个整数的二进制位整体向左或向右移动，常用于：

- 乘以或除以 2 的幂。
- 提取、组合和定位位域。
- 构造掩码。
- 计算数组元素的字节偏移。
- 实现符号扩展和零扩展。
- 处理寄存器中的 32 位子字。

RV64I 的移位指令可以从三个维度理解：

```text
数据宽度：64 位普通指令 / 32 位 W 指令
移位来源：寄存器 / 指令中的立即数
右移类型：逻辑右移 / 算术右移
```

## 1. RV64I 移位指令汇总

### 1.1 64 位移位

| 指令 | 移位量来源 | 操作 |
|---|---|---|
| `SLL` | 寄存器 `rs2` 的低 6 位 | 64 位逻辑左移 |
| `SRL` | 寄存器 `rs2` 的低 6 位 | 64 位逻辑右移 |
| `SRA` | 寄存器 `rs2` 的低 6 位 | 64 位算术右移 |
| `SLLI` | 6 位立即数 | 64 位逻辑左移 |
| `SRLI` | 6 位立即数 | 64 位逻辑右移 |
| `SRAI` | 6 位立即数 | 64 位算术右移 |

64 位移位量范围是：

```text
0～63
```

### 1.2 RV64 特有的 32 位 W 移位

| 指令 | 移位量来源 | 操作 |
|---|---|---|
| `SLLW` | 寄存器 `rs2` 的低 5 位 | 低 32 位逻辑左移，再符号扩展到 64 位 |
| `SRLW` | 寄存器 `rs2` 的低 5 位 | 低 32 位逻辑右移，再符号扩展到 64 位 |
| `SRAW` | 寄存器 `rs2` 的低 5 位 | 低 32 位算术右移，再符号扩展到 64 位 |
| `SLLIW` | 5 位立即数 | 低 32 位逻辑左移，再符号扩展到 64 位 |
| `SRLIW` | 5 位立即数 | 低 32 位逻辑右移，再符号扩展到 64 位 |
| `SRAIW` | 5 位立即数 | 低 32 位算术右移，再符号扩展到 64 位 |

W 指令的移位量范围是：

```text
0～31
```

所有 W 指令最终都会把 32 位结果的 bit 31 符号扩展到 bit 63。这个规则对名字中含有“逻辑”的 `SLLW/SRLW/SLLIW/SRLIW` 也同样成立。

## 2. 移位的基本二进制模型

以 8 位数据演示基本概念。假设：

```text
value = 1011 0010
```

左移两位：

```text
1011 0010 << 2
1100 1000
```

左侧移出的位被丢弃，右侧补零。

逻辑右移两位：

```text
1011 0010 >> 2
0010 1100
```

右侧移出的位被丢弃，左侧补零。

算术右移两位：

```text
1011 0010 >>> 2
1110 1100
```

如果最高位为 1，算术右移会在左侧补 1，从而尽量保持二进制补码数的符号。

在 RV64 中，对应规则应用于完整的 64 位或 32 位操作数。

## 3. 左移：`SLL` 和 `SLLI`

### 3.1 `SLL`：寄存器指定移位量

语法：

```asm
sll rd, rs1, rs2
```

语义：

```text
rd = rs1 << rs2[5:0]
```

只使用 `rs2` 的低 6 位，所以实际移位量一定在 `0～63` 之间。

例如：

```asm
li  t0, 3
li  t1, 4
sll t2, t0, t1
```

结果：

```text
t2 = 3 << 4
   = 48
   = 0x30
```

### 3.2 `SLLI`：立即数指定移位量

语法：

```asm
slli rd, rs1, shamt
```

例如：

```asm
li   t0, 1
slli t1, t0, 8
```

结果：

```text
t1 = 0x100
```

再例如，将 bit 0 移到 bit 63：

```asm
li   t0, 1
slli t1, t0, 63
```

结果：

```text
t1 = 0x8000000000000000
```

### 3.3 左移时的溢出

左侧移出的位会被直接丢弃，不产生异常，也没有进位标志或溢出标志。

```asm
li   t0, 0x8000000000000000
slli t1, t0, 1
```

结果为：

```text
t1 = 0
```

RISC-V 没有单独的算术左移指令。对于二进制补码，逻辑左移和所谓的算术左移位级行为相同，所以只有 `SLL/SLLI`，没有基础指令 `SLA/SLAI`。

## 4. 逻辑右移：`SRL` 和 `SRLI`

逻辑右移始终在左侧补零，不考虑原始数据的符号。

### 4.1 `SRL`：寄存器指定移位量

语法：

```asm
srl rd, rs1, rs2
```

语义：

```text
rd = logical_right_shift(rs1, rs2[5:0])
```

例如：

```asm
li  t0, -1
li  t1, 60
srl t2, t0, t1
```

`-1` 的 64 位补码为：

```text
0xffffffffffffffff
```

逻辑右移 60 位后：

```text
t2 = 0x000000000000000f
```

### 4.2 `SRLI`：立即数指定移位量

语法：

```asm
srli rd, rs1, shamt
```

例如：

```asm
li   t0, 0x8000000000000000
srli t1, t0, 60
```

结果：

```text
t1 = 0x0000000000000008
```

即使原数据最高位是 1，`SRL/SRLI` 也始终在左侧补零。

## 5. 算术右移：`SRA` 和 `SRAI`

算术右移在左侧复制原数据的最高位：

- 原最高位是 0，左侧补 0。
- 原最高位是 1，左侧补 1。

### 5.1 `SRA`：寄存器指定移位量

语法：

```asm
sra rd, rs1, rs2
```

语义：

```text
rd = arithmetic_right_shift(rs1, rs2[5:0])
```

例如：

```asm
li  t0, -16
li  t1, 2
sra t2, t0, t1
```

结果：

```text
t2 = -4
   = 0xfffffffffffffffc
```

### 5.2 `SRAI`：立即数指定移位量

语法：

```asm
srai rd, rs1, shamt
```

例如：

```asm
li   t0, 0x8000000000000000
srai t1, t0, 60
```

结果：

```text
t1 = 0xfffffffffffffff8
```

按照有符号数解释就是：

```text
t1 = -8
```

### 5.3 正数的逻辑右移和算术右移

如果最高位为 0，两种右移结果相同：

```asm
li   t0, 16
srli t1, t0, 2       # 4
srai t2, t0, 2       # 4
```

两者只有在输入最高位为 1 时才会产生差异。

## 6. 寄存器移位量会被截断

64 位寄存器移位只读取 `rs2[5:0]`。因此：

```text
实际移位量 = rs2 & 0x3f
```

例如：

```asm
li  t0, 1
li  t1, 64
sll t2, t0, t1
```

因为：

```text
64 & 0x3f = 0
```

所以结果是：

```text
t2 = 1
```

再例如：

```asm
li  t0, 1
li  t1, 65
sll t2, t0, t1
```

因为：

```text
65 & 0x3f = 1
```

所以结果是：

```text
t2 = 2
```

这不会产生异常，也不会自动饱和到 63。

立即数移位不同：

```asm
slli t0, t1, 64
```

对于 RV64I 来说不是合法的立即数移位，汇编器通常会直接报错。立即数必须显式位于 `0～63`。

需要注意，虽然 RISC-V 机器指令对寄存器移位量有明确的截断规则，但 C/C++ 中按类型宽度或更大的位数移位通常属于未定义行为。不能直接用硬件行为推导 C/C++ 语言行为。

## 7. RV64 的 32 位 W 移位

W 指令只读取 `rs1` 的低 32 位，在 32 位范围内完成移位，然后把 32 位结果符号扩展为 64 位：

```text
word_result = shift(rs1[31:0])
rd = sign_extend(word_result[31:0])
```

因此，W 指令不是简单地“计算一个零扩展的 32 位结果”。最终是否在高 32 位填 0，取决于结果的 bit 31。

## 8. `SLLW` 和 `SLLIW`

### 8.1 `SLLW`

语法：

```asm
sllw rd, rs1, rs2
```

语义：

```text
word = rs1[31:0] << rs2[4:0]
rd   = sign_extend(word[31:0])
```

例如：

```asm
li   t0, 1
li   t1, 31
sllw t2, t0, t1
```

32 位结果是：

```text
0x80000000
```

符号扩展到 64 位后：

```text
t2 = 0xffffffff80000000
```

### 8.2 `SLLIW`

语法：

```asm
slliw rd, rs1, shamt
```

例如：

```asm
li    t0, 1
slliw t1, t0, 31
```

结果同样是：

```text
t1 = 0xffffffff80000000
```

注意它不是：

```text
0x0000000080000000
```

因为所有 W 指令都对最终 32 位结果进行符号扩展。

## 9. `SRLW` 和 `SRLIW`

W 逻辑右移先在 32 位范围内左侧补零，然后对最终 32 位结果做符号扩展。

### 9.1 `SRLW`

```asm
li   t0, 0x80000000
li   t1, 31
srlw t2, t0, t1
```

低 32 位逻辑右移结果为：

```text
0x00000001
```

符号扩展后仍为：

```text
t2 = 0x0000000000000001
```

### 9.2 `SRLIW`

```asm
li    t0, 0x80000000
srliw t1, t0, 31
```

结果为：

```text
t1 = 1
```

一个容易忽略的例子是移位量为 0：

```asm
li    t0, 0x0000000080000000
srliw t1, t0, 0
```

32 位逻辑移位没有改变低 32 位，但最终仍要符号扩展，因此：

```text
t1 = 0xffffffff80000000
```

## 10. `SRAW` 和 `SRAIW`

W 算术右移先把 `rs1[31:0]` 视为 32 位有符号补码，再进行右移，最后符号扩展到 64 位。

### 10.1 `SRAW`

```asm
li   t0, 0x80000000
li   t1, 31
sraw t2, t0, t1
```

结果为：

```text
t2 = 0xffffffffffffffff
   = -1
```

### 10.2 `SRAIW`

```asm
li    t0, 0x80000000
sraiw t1, t0, 31
```

结果同样为：

```text
t1 = 0xffffffffffffffff
```

## 11. W 指令的移位量截断

W 寄存器移位只使用 `rs2[4:0]`：

```text
实际移位量 = rs2 & 0x1f
```

例如：

```asm
li   t0, 1
li   t1, 32
sllw t2, t0, t1
```

实际移位量为 0，所以：

```text
t2 = 1
```

再例如：

```asm
li   t0, 1
li   t1, 33
sllw t2, t0, t1
```

实际移位量为 1，所以：

```text
t2 = 2
```

`SLLIW/SRLIW/SRAIW` 的立即数必须位于 `0～31`。超出范围通常会在汇编阶段报错。

## 12. 逻辑右移与算术右移对比

假设：

```text
t0 = 0x8000000000000000
```

执行：

```asm
srli t1, t0, 60
srai t2, t0, 60
```

得到：

```text
t1 = 0x0000000000000008
t2 = 0xfffffffffffffff8
```

| 指令 | 左侧填充 | 结果解释 |
|---|---|---|
| `SRL/SRLI` | 始终补 0 | 适合无符号数、位域操作 |
| `SRA/SRAI` | 复制原符号位 | 适合有符号补码近似除法 |

选择右移指令时，不是看寄存器“声明成什么类型”，因为汇编寄存器没有类型；而是由程序员决定当前位模式应该按无符号还是有符号语义处理。

## 13. 移位与乘除法

### 13.1 左移实现乘以 2 的幂

```asm
slli t1, t0, 3
```

在不考虑被丢弃高位的情况下，相当于：

```text
t1 = t0 * 8
```

机器只保留低 64 位，不检测数学意义上的溢出。

### 13.2 无符号数右移实现除以 2 的幂

```asm
srli t1, t0, 3
```

对于无符号整数，相当于：

```text
t1 = t0 / 8
```

余数被丢弃。

### 13.3 有符号负数的舍入差异

不能把 `SRAI` 无条件当成有符号除法。

例如：

```asm
li   t0, -3
srai t1, t0, 1
```

结果为：

```text
t1 = -2
```

算术右移相当于向负无穷取整：

```text
floor(-3 / 2) = -2
```

而 RISC-V `DIV` 和 C 语言的有符号整数除法向 0 截断：

```text
-3 / 2 = -1
```

所以只有在输入非负、能够整除，或者程序明确需要向负无穷取整时，才能直接用算术右移替代有符号除法。

## 14. 常见应用 Demo

### 14.1 计算数组字节偏移

64 位整数数组每个元素占 8 字节：

```asm
# a0 = 数组首地址
# a1 = 元素下标

slli t0, a1, 3       # t0 = index * 8
add  t0, a0, t0      # t0 = &array[index]
ld   t1, 0(t0)       # 加载 array[index]
```

### 14.2 提取位域

提取一个 64 位值的 bits `[15:8]`：

```asm
li   t0, 0x123456789abcdef0
srli t1, t0, 8
andi t1, t1, 0xff
```

结果为：

```text
t1 = 0xde
```

右移先把目标字段移动到最低位，`ANDI` 再清除其他高位。

### 14.3 零扩展低 8 位

可以使用两次移位：

```asm
slli t0, a0, 56
srli t0, t0, 56
```

结果只保留原始 `a0` 的低 8 位，并零扩展到 64 位。

对于低 8 位，实际编程通常直接使用：

```asm
andi t0, a0, 0xff
```

但双移位技巧可以推广到立即数掩码不容易直接编码的情况。

### 14.4 符号扩展低 8 位

```asm
slli t0, a0, 56
srai t0, t0, 56
```

先将原 bit 7 移动到 bit 63，再通过算术右移复制符号位。

### 14.5 零扩展低 32 位

```asm
slli t0, a0, 32
srli t0, t0, 32
```

结果为：

```text
t0 = zero_extend(a0[31:0])
```

如果处理器支持某些额外扩展，汇编器也可能提供更直接的 `zext.w` 形式，但上面的两条 RV64I 指令不依赖这些扩展。

### 14.6 符号扩展低 32 位

使用移位可以写成：

```asm
slli t0, a0, 32
srai t0, t0, 32
```

RV64I 中更直接的常见写法是：

```asm
addiw t0, a0, 0
```

其伪指令形式是：

```asm
sext.w t0, a0
```

## 15. RV64I 没有循环移位指令

普通移位会丢弃被移出的位：

```text
SLL：高位丢弃
SRL/SRA：低位丢弃
```

循环移位会把移出的位重新放回另一端，它与普通移位不同。

基础 RV64I 没有 `ROL/ROR`。这些指令属于位操作扩展，例如 Zbb：

```asm
rol  rd, rs1, rs2
ror  rd, rs1, rs2
rori rd, rs1, shamt
```

在只允许 `-march=rv64i` 的实验中，如果需要循环移位，必须使用普通移位和 `OR` 组合实现。

## 16. 完整 Shift Instructions Demo

下面的程序覆盖全部 12 条 RV64I 移位指令：

```text
SLL   SRL   SRA
SLLI  SRLI  SRAI
SLLW  SRLW  SRAW
SLLIW SRLIW SRAIW
```

程序还验证：

- 64 位寄存器移位只使用移位量低 6 位。
- W 寄存器移位只使用移位量低 5 位。
- 所有 W 指令都把最终结果符号扩展到 64 位。
- 位域提取结果是否正确。
- 负奇数算术右移的舍入行为。

保存为 `shift_demo.S`：

```asm
    .option nopic

    .section .rodata

pass_message:
    .ascii "Shift instruction demo passed!\n"
    .equ pass_message_len, . - pass_message

fail_message:
    .ascii "Shift instruction demo failed!\n"
    .equ fail_message_len, . - fail_message

    .section .text
    .globl _start

_start:
    # =================================================
    # 1. 64 位立即数移位：SLLI、SRLI、SRAI
    # =================================================

    li   t0, 1
    slli s0, t0, 63
    # s0 = 0x8000000000000000

    srli s1, s0, 60
    # s1 = 0x0000000000000008

    srai s2, s0, 60
    # s2 = 0xfffffffffffffff8 (-8)

    # =================================================
    # 2. 64 位寄存器移位：SLL、SRL、SRA
    # =================================================

    li  t1, 4
    sll s3, t0, t1
    # s3 = 16

    li  t2, -1
    li  t3, 60
    srl s4, t2, t3
    # s4 = 0x000000000000000f

    sra s5, t2, t3
    # s5 = 0xffffffffffffffff (-1)

    # 只使用 65 的低 6 位：65 & 63 = 1
    li  t4, 65
    sll s6, t0, t4
    # s6 = 2

after_shift64:
    # 验证 64 位结果
    li  t5, 0x8000000000000000
    bne s0, t5, failed

    li  t5, 8
    bne s1, t5, failed

    li  t5, -8
    bne s2, t5, failed

    li  t5, 16
    bne s3, t5, failed

    li  t5, 15
    bne s4, t5, failed

    li  t5, -1
    bne s5, t5, failed

    li  t5, 2
    bne s6, t5, failed

    # =================================================
    # 3. 32 位立即数移位：SLLIW、SRLIW、SRAIW
    # =================================================

    li    t0, 1
    slliw s0, t0, 31
    # 32 位结果 0x80000000 符号扩展：
    # s0 = 0xffffffff80000000

    srliw s1, s0, 31
    # s1 = 1

    sraiw s2, s0, 31
    # s2 = 0xffffffffffffffff (-1)

    # =================================================
    # 4. 32 位寄存器移位：SLLW、SRLW、SRAW
    # =================================================

    li   t1, 31
    sllw s3, t0, t1
    # s3 = 0xffffffff80000000

    srlw s4, s3, t1
    # s4 = 1

    sraw s5, s3, t1
    # s5 = 0xffffffffffffffff (-1)

    # 只使用 33 的低 5 位：33 & 31 = 1
    li   t2, 33
    sllw s6, t0, t2
    # s6 = 2

after_shift32:
    # 验证 32 位 W 指令结果
    li  t5, 0xffffffff80000000
    bne s0, t5, failed

    li  t5, 1
    bne s1, t5, failed

    li  t5, -1
    bne s2, t5, failed

    li  t5, 0xffffffff80000000
    bne s3, t5, failed

    li  t5, 1
    bne s4, t5, failed

    li  t5, -1
    bne s5, t5, failed

    li  t5, 2
    bne s6, t5, failed

    # =================================================
    # 5. 位域提取：取 bits [15:8]
    # =================================================

    li   t0, 0x123456789abcdef0
    srli t1, t0, 8
    andi t1, t1, 0xff

    li  t2, 0xde
    bne t1, t2, failed

    # =================================================
    # 6. 负数算术右移的舍入行为
    # =================================================

    li   t0, -3
    srai t1, t0, 1

    li  t2, -2
    bne t1, t2, failed

after_examples:
    # 所有验证成功，输出提示
    li a0, 1
    la a1, pass_message
    li a2, pass_message_len
    li a7, 64
    ecall

    # exit(0)
    li a0, 0
    li a7, 93
    ecall

failed:
    li a0, 1
    la a1, fail_message
    li a2, fail_message_len
    li a7, 64
    ecall

    # exit(1)
    li a0, 1
    li a7, 93
    ecall
```

## 17. 汇编、链接和运行

```bash
riscv64-linux-gnu-as \
    -march=rv64i \
    -mabi=lp64 \
    -g \
    -o shift_demo.o \
    shift_demo.S

riscv64-linux-gnu-ld \
    -m elf64lriscv \
    -static \
    -e _start \
    -o shift_demo \
    shift_demo.o
```

运行：

```bash
qemu-riscv64-static ./shift_demo
```

预期输出：

```text
Shift instruction demo passed!
```

检查退出状态：

```bash
echo $?
```

预期：

```text
0
```

如果任意一项验证失败，程序会输出：

```text
Shift instruction demo failed!
```

并通过 `exit(1)` 退出。

## 18. 查看真实指令

查看源码、机器码和反汇编：

```bash
riscv64-linux-gnu-objdump -drS shift_demo
```

关闭伪指令别名：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    shift_demo
```

重点观察：

```text
slli/srli/srai
sll/srl/sra
slliw/srliw/sraiw
sllw/srlw/sraw
```

还可以观察 `li` 如何被展开成一条或多条真实指令。

只查看移位指令：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    shift_demo | \
rg '\b(sll|slli|srl|srli|sra|srai|sllw|slliw|srlw|srliw|sraw|sraiw)\b'
```

## 19. 使用 GDB 验证移位结果

终端一：

```bash
qemu-riscv64-static -g 1234 ./shift_demo
```

终端二：

```bash
gdb-multiarch ./shift_demo
```

连接：

```gdb
target remote :1234
```

### 19.1 查看 64 位移位结果

```gdb
break after_shift64
continue
info registers s0 s1 s2 s3 s4 s5 s6
```

预期：

```text
s0 = 0x8000000000000000
s1 = 0x0000000000000008
s2 = 0xfffffffffffffff8
s3 = 0x0000000000000010
s4 = 0x000000000000000f
s5 = 0xffffffffffffffff
s6 = 0x0000000000000002
```

### 19.2 查看 32 位 W 移位结果

```gdb
break after_shift32
continue
info registers s0 s1 s2 s3 s4 s5 s6
```

预期：

```text
s0 = 0xffffffff80000000
s1 = 0x0000000000000001
s2 = 0xffffffffffffffff
s3 = 0xffffffff80000000
s4 = 0x0000000000000001
s5 = 0xffffffffffffffff
s6 = 0x0000000000000002
```

### 19.3 单步观察寄存器

```gdb
display/i $pc
display/x $s0
display/x $s1
display/x $s2
si
```

也可以直接检查一条表达式：

```gdb
p/x $s0
p/d $s2
```

其中：

- `/x` 按十六进制显示。
- `/d` 按有符号十进制显示。
- `/u` 按无符号十进制显示。

## 20. 常见错误和易混淆点

### 20.1 把 `SRL` 和 `SRA` 混用

处理无符号值或原始位域时通常使用：

```asm
srl
srli
```

希望复制负数符号位时使用：

```asm
sra
srai
```

寄存器本身没有类型，选择完全由程序需要的语义决定。

### 20.2 误以为寄存器移位 64 位会得到零

```asm
li  t1, 64
sll t2, t0, t1
```

实际移位量是：

```text
64 & 63 = 0
```

所以结果等于原值，而不是零。

### 20.3 误以为 W 逻辑移位一定零扩展

`SRLW/SRLIW` 的移位过程确实是逻辑右移，但最终 32 位结果仍然统一符号扩展到 64 位。

例如：

```asm
srliw t0, t1, 0
```

如果结果 bit 31 为 1，高 32 位会全部变成 1。

### 20.4 把算术右移当成通用有符号除法

对于负奇数：

```text
-3 >> 1 = -2
-3 / 2  = -1
```

两者舍入方向不同。

### 20.5 忘记左移会丢弃高位

移位不会报告溢出：

```asm
li   t0, 0x8000000000000000
slli t1, t0, 1
```

结果直接变为零。

### 20.6 把普通移位当成循环移位

普通移位丢弃移出的位；循环移位会把移出的位送回另一端。RV64I 没有循环移位指令，需要软件组合或位操作扩展。

### 20.7 忽略立即数范围

```text
SLLI/SRLI/SRAI：   0～63
SLLIW/SRLIW/SRAIW：0～31
```

立即数超出范围时，汇编器一般会报错；它不会像寄存器移位那样在运行时自动截取低位。

## 21. 本部分小结

RV64I 移位指令可以归纳为：

```text
64 位寄存器移位：
    SLL  SRL  SRA

64 位立即数移位：
    SLLI SRLI SRAI

32 位寄存器移位：
    SLLW SRLW SRAW

32 位立即数移位：
    SLLIW SRLIW SRAIW
```

最需要记住的规则是：

1. 左移统一在右侧补零，高位被丢弃，没有单独的算术左移。
2. 逻辑右移在左侧补零，算术右移复制符号位。
3. 64 位寄存器移位只使用移位量低 6 位。
4. 32 位 W 寄存器移位只使用移位量低 5 位。
5. 所有 W 指令都会把最终 32 位结果符号扩展到 64 位。
6. 算术右移负奇数和有符号除法的舍入方向不同。
7. RV64I 普通移位不是循环移位，`ROL/ROR` 属于额外的位操作扩展。

学习这组指令时，建议同时查看无别名反汇编并在 GDB 中用十六进制观察结果：

```bash
riscv64-linux-gnu-objdump -d -M no-aliases,numeric shift_demo
```

```gdb
display/i $pc
info registers
```

这样可以直观看到逻辑右移、算术右移以及 W 指令符号扩展之间的差异。
