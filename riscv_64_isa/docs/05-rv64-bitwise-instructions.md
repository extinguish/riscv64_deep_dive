# RISC-V 64 位 ISA：Bitwise Instructions

本文是 RISC-V 64 位 ISA 系列的下一部分，集中讲解 RV64I 基础整数指令集中的位逻辑操作，并提供可以直接汇编、运行、反汇编和调试的完整示例。

本篇首先聚焦所有 RV64I 基础位逻辑指令：

```text
AND   OR   XOR
ANDI  ORI  XORI
```

同时讲解由这些真实指令构成的常见伪指令和编程模式：

```text
NOT
设置位、清除位、翻转位、测试位
构造掩码、提取位域、替换位域
```

RISC-V 的 B 扩展还提供 `ANDN`、`CLZ`、`CPOP`、`BSET`、`BCLR` 等更丰富的位操作。它们不属于基础 RV64I，本文最后会说明 Zba/Zbb/Zbc/Zbs 的边界，避免把基础指令与可选扩展混在一起。

## 1. 位编号和基本概念

RV64 整数寄存器包含 64 个二进制位，通常从最低位开始编号：

```text
bit 63                                      bit 0
  │                                           │
  ▼                                           ▼
+---+---+---+-----------------------------+---+
|63 |62 |61 |            ...              | 0 |
+---+---+---+-----------------------------+---+
```

- bit 0 是最低有效位，LSB。
- bit 63 是最高有效位，MSB。
- 对有符号 64 位补码数，bit 63 也作为符号位。
- 位逻辑指令逐位独立运算，不关心寄存器当前被程序解释为有符号数、无符号数、地址还是其他数据。

例如：

```text
A = 1100 1100
B = 1010 1010
```

逐位运算结果为：

```text
A AND B = 1000 1000
A OR  B = 1110 1110
A XOR B = 0110 0110
NOT A    = 0011 0011    # 这里只展示 8 位
```

在 RV64 中，实际运算宽度是完整的 64 位。

## 2. 真值表

对单独一位 `A` 和 `B`：

| A | B | `A AND B` | `A OR B` | `A XOR B` |
|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 0 | 0 |
| 0 | 1 | 0 | 1 | 1 |
| 1 | 0 | 0 | 1 | 1 |
| 1 | 1 | 1 | 1 | 0 |

可以直观理解为：

- `AND`：两个输入位都为 1，结果才为 1。
- `OR`：至少一个输入位为 1，结果就为 1。
- `XOR`：两个输入位不同，结果为 1。
- `NOT`：把 0 变成 1，把 1 变成 0。

## 3. 指令汇总

### 3.1 寄存器形式

| 指令 | 语法 | 语义 |
|---|---|---|
| `AND` | `and rd, rs1, rs2` | `rd = rs1 & rs2` |
| `OR` | `or rd, rs1, rs2` | `rd = rs1 \| rs2` |
| `XOR` | `xor rd, rs1, rs2` | `rd = rs1 ^ rs2` |

这些都是 R-type 指令，对两个 64 位源寄存器逐位运算，将 64 位结果写入目标寄存器。

### 3.2 立即数形式

| 指令 | 语法 | 语义 |
|---|---|---|
| `ANDI` | `andi rd, rs1, imm12` | `rd = rs1 & sign_extend(imm12)` |
| `ORI` | `ori rd, rs1, imm12` | `rd = rs1 \| sign_extend(imm12)` |
| `XORI` | `xori rd, rs1, imm12` | `rd = rs1 ^ sign_extend(imm12)` |

这些都是 I-type 指令，立即数宽度为 12 位，并在参与 64 位运算之前符号扩展。

可直接书写的数值范围通常表示为：

```text
-2048 ～ 2047
```

立即数符号扩展是本组指令最容易被忽略的规则之一。

## 4. `AND`：寄存器按位与

语法：

```asm
and rd, rs1, rs2
```

示例：

```asm
li  t0, 0xcc
li  t1, 0xaa
and t2, t0, t1
```

计算过程：

```text
0xcc = 1100 1100
0xaa = 1010 1010
       ---------
AND  = 1000 1000 = 0x88
```

结果：

```text
t2 = 0x88
```

`AND` 最常见的用途是：

- 使用掩码保留选中的位。
- 清除不需要的位。
- 测试某些位是否为 1。
- 提取位域。

## 5. `ANDI`：立即数按位与

语法：

```asm
andi rd, rs1, imm12
```

例如，只保留低 8 位：

```asm
li   t0, 0x1234abcd
andi t1, t0, 0xff
```

结果：

```text
t1 = 0xcd
```

再例如，检查数值是奇数还是偶数：

```asm
andi t1, t0, 1
```

结果含义：

```text
t1 = 0：t0 是偶数
t1 = 1：t0 是奇数
```

配合分支可以写成：

```asm
andi t1, t0, 1
bne  t1, x0, is_odd
```

伪指令形式是：

```asm
andi t1, t0, 1
bnez t1, is_odd
```

## 6. `OR`：寄存器按位或

语法：

```asm
or rd, rs1, rs2
```

示例：

```asm
li t0, 0xcc
li t1, 0xaa
or t2, t0, t1
```

计算过程：

```text
0xcc = 1100 1100
0xaa = 1010 1010
       ---------
OR   = 1110 1110 = 0xee
```

结果：

```text
t2 = 0xee
```

`OR` 最常见的用途是：

- 将指定位置设置为 1。
- 合并互不重叠的位域。
- 把多个状态标志组合到一个寄存器中。

## 7. `ORI`：立即数按位或

语法：

```asm
ori rd, rs1, imm12
```

例如，把 bit 0 和 bit 1 设置为 1：

```asm
li  t0, 0xcc
ori t1, t0, 0x03
```

结果：

```text
t1 = 0xcf
```

原来已经为 1 的位保持为 1；立即数掩码中为 1 的位也会被设置为 1。

## 8. `XOR`：寄存器按位异或

语法：

```asm
xor rd, rs1, rs2
```

示例：

```asm
li  t0, 0xcc
li  t1, 0xaa
xor t2, t0, t1
```

计算过程：

```text
0xcc = 1100 1100
0xaa = 1010 1010
       ---------
XOR  = 0110 0110 = 0x66
```

结果：

```text
t2 = 0x66
```

`XOR` 最常见的用途是：

- 翻转掩码所选中的位。
- 判断两个位模式哪些位置不同。
- 配合全 1 掩码实现按位取反。
- 将寄存器自身异或以产生零。

例如：

```asm
xor t0, t0, t0
```

结果一定是：

```text
t0 = 0
```

不过编写普通代码时，通常更直观地写：

```asm
li t0, 0
```

它通常展开为：

```asm
addi t0, x0, 0
```

## 9. `XORI`：立即数按位异或

语法：

```asm
xori rd, rs1, imm12
```

例如，翻转低 8 位：

```asm
li   t0, 0xcc
xori t1, t0, 0xff
```

结果：

```text
t1 = 0x33
```

因为 `0xff` 是合法的正 12 位立即数，符号扩展后仍为：

```text
0x00000000000000ff
```

所以只有低 8 位被翻转。

## 10. `NOT` 伪指令

RV64I 没有独立编码的基础 `NOT` 指令。按位取反使用：

```asm
xori rd, rs, -1
```

因为 12 位立即数 `-1` 的编码为全 1，符号扩展到 64 位后仍然是：

```text
0xffffffffffffffff
```

任何位与 1 异或都会翻转，所以：

```asm
xori t1, t0, -1
```

等价于伪指令：

```asm
not t1, t0
```

例如：

```asm
li  t0, 0xcc
not t1, t0
```

结果为：

```text
t1 = 0xffffffffffffff33
```

使用无别名反汇编：

```bash
riscv64-linux-gnu-objdump -d -M no-aliases,numeric program
```

可以看到 `not` 最终显示为真实的：

```asm
xori rd, rs, -1
```

## 11. 立即数符号扩展

`ANDI/ORI/XORI` 的 12 位立即数会符号扩展到 64 位。

### 11.1 正立即数

```asm
andi t0, t1, 0xff
```

`0xff` 的 bit 11 为 0，因此扩展为：

```text
0x00000000000000ff
```

### 11.2 负立即数

```asm
xori t0, t1, -1
```

`-1` 符号扩展为：

```text
0xffffffffffffffff
```

再例如：

```asm
andi t0, t1, -16
```

`-16` 的 64 位位模式为：

```text
0xfffffffffffffff0
```

所以这条指令可以清除 `t1` 的低 4 位：

```text
t0 = t1 & 0xfffffffffffffff0
```

### 11.3 不能直接编码任意掩码

例如，下面的掩码超过正 12 位立即数范围：

```text
0xffff
```

不要假设下面的写法能够表示“保留低 16 位”：

```asm
andi t0, t1, 0xffff
```

汇编器通常会因为立即数超出范围而报错。正确写法之一是先把掩码装入寄存器：

```asm
li  t2, 0xffff
and t0, t1, t2
```

也可以使用移位实现低 16 位零扩展：

```asm
slli t0, t1, 48
srli t0, t0, 48
```

## 12. 设置位、清除位、翻转位和测试位

假设：

```text
value = 原始值
mask  = 需要操作的位掩码
```

四种最常见操作是：

| 操作 | 表达式 | 基础指令 |
|---|---|---|
| 设置位 | `value \| mask` | `OR/ORI` |
| 清除位 | `value & ~mask` | `NOT + AND`，或适当的 `ANDI` |
| 翻转位 | `value ^ mask` | `XOR/XORI` |
| 测试位 | `value & mask` | `AND/ANDI`，再分支 |

### 12.1 设置指定的位

把 bit 3 设置为 1：

```asm
ori t0, t0, 0x08
```

无论 bit 3 原来是 0 还是 1，执行后都会是 1。

### 12.2 清除指定的位

清除 bit 1，需要使用除了 bit 1 为 0 之外其余位全为 1 的掩码：

```text
~(1 << 1) = 0xfffffffffffffffd = -3
```

因此可以写：

```asm
andi t0, t0, -3
```

对于无法放入 12 位立即数的通用掩码，可以写：

```asm
li   t1, 1
sll  t1, t1, a0      # t1 = 1 << bit_index
not  t1, t1           # t1 = ~(1 << bit_index)
and  t0, t0, t1
```

### 12.3 翻转指定的位

翻转 bit 0：

```asm
xori t0, t0, 1
```

bit 0 原来为 0 就变成 1，原来为 1 就变成 0。

### 12.4 测试指定的位

检查 bit 3 是否为 1：

```asm
andi t1, t0, 0x08
bne  t1, x0, bit_is_set
```

测试不会修改原始 `t0`，结果保存在临时寄存器 `t1` 中。

## 13. 动态单比特掩码

如果位编号在寄存器 `a0` 中，可以使用上一章介绍的移位指令构造单比特掩码：

```asm
li  t0, 1
sll t0, t0, a0
```

结果为：

```text
t0 = 1 << (a0 & 63)
```

假设待处理的 64 位值保存在 `a1`，位编号仍在 `a0`，掩码保存在 `t0`，就可以进行以下操作。每段代码都是独立示例；需要临时寄存器时使用 `t1`。

设置动态位：

```asm
or a1, a1, t0
```

清除动态位：

```asm
not t1, t0
and a1, a1, t1
```

翻转动态位：

```asm
xor a1, a1, t0
```

提取动态位并规范化为 0 或 1：

```asm
and t1, a1, t0
srl t1, t1, a0
```

## 14. 位域掩码

一个从 bit `lsb` 开始、宽度为 `width` 的位域，其概念掩码是：

```text
field_mask = ((1 << width) - 1) << lsb
```

例如，bits `[15:8]`：

```text
lsb   = 8
width = 8
mask  = 0x000000000000ff00
```

### 14.1 提取位域

```asm
li   t0, 0x123456789abcdef0
srli t1, t0, 8
andi t1, t1, 0xff
```

结果：

```text
t1 = 0xde
```

### 14.2 替换位域

把原数值 bits `[15:8]` 替换为 `0x5a`：

```asm
li  t0, 0x123456789abcdef0

# 清除原 bits [15:8]
li  t1, 0xff00
not t2, t1
and t0, t0, t2

# 限制新字段宽度并移动到目标位置
li   t3, 0x5a
andi t3, t3, 0xff
slli t3, t3, 8

# 合并新字段
or t0, t0, t3
```

结果：

```text
t0 = 0x123456789abc5af0
```

替换位域的通用过程是：

```text
result = (old_value & ~field_mask)
       | ((new_value << lsb) & field_mask)
```

## 15. 常见恒等式和寄存器技巧

### 15.1 保持原值

```text
x & 全 1 = x
x | 0    = x
x ^ 0    = x
```

对应示例：

```asm
andi t0, t1, -1
or   t0, t1, x0
xori t0, t1, 0
```

`or rd, rs, x0` 可以实现寄存器复制。汇编器通常提供更直观的伪指令：

```asm
mv rd, rs
```

通常展开为：

```asm
addi rd, rs, 0
```

### 15.2 产生零

```text
x & 0 = 0
x ^ x = 0
```

对应示例：

```asm
and t0, t1, x0
xor t0, t1, t1
```

### 15.3 德摩根定律

```text
~(A & B) = (~A) | (~B)
~(A | B) = (~A) & (~B)
```

RV64I 没有单条 `NAND` 或 `NOR` 指令，需要组合实现。例如 NAND：

```asm
and t0, a0, a1
not t0, t0
```

NOR：

```asm
or  t0, a0, a1
not t0, t0
```

## 16. RV64I 没有 ANDW、ORW 和 XORW

RV64I 提供：

```text
ADDW SUBW SLLW SRLW SRAW
```

但没有：

```text
ANDW ORW XORW
ANDIW ORIW XORIW
```

原因是逐位逻辑运算对高低位完全独立。程序可以先执行普通 64 位逻辑运算，再根据需要对低 32 位进行零扩展或符号扩展。

只保留并零扩展低 32 位：

```asm
slli t0, t0, 32
srli t0, t0, 32
```

符号扩展低 32 位：

```asm
addiw t0, t0, 0
```

其伪指令形式为：

```asm
sext.w t0, t0
```

## 17. 寄存器位操作与内存位操作

`AND/OR/XOR` 只操作寄存器，不能直接修改内存。

要修改内存中的标志，需要执行加载、计算和存储：

```asm
ld   t0, 0(a0)
ori  t0, t0, 0x08
sd   t0, 0(a0)
```

这个序列不是原子的。多个线程或处理器同时修改同一内存位置时，可能发生更新丢失：

```text
CPU 0 load
CPU 1 load
CPU 0 modify/store
CPU 1 modify/store    # 可能覆盖 CPU 0 的修改
```

并发场景需要使用 A 扩展的原子指令、锁或其他同步机制，不能因为这里只有三条简单指令就把它当成原子位操作。

## 18. 完整 Bitwise Instructions Demo

下面的程序覆盖全部 6 条 RV64I 基础位逻辑指令：

```text
AND OR XOR ANDI ORI XORI
```

同时验证：

- `NOT` 伪指令。
- 设置、清除、翻转和测试标志位。
- 动态位掩码。
- bits `[15:8]` 位域替换。
- 立即数符号扩展的实际用途。

保存为 `bitwise_demo.S`：

```asm
    .option nopic

    .section .rodata

pass_message:
    .ascii "Bitwise instruction demo passed!\n"
    .equ pass_message_len, . - pass_message

fail_message:
    .ascii "Bitwise instruction demo failed!\n"
    .equ fail_message_len, . - fail_message

    .section .text
    .globl _start

_start:
    # =================================================
    # 1. 寄存器形式：AND、OR、XOR
    # =================================================

    li t0, 0xcc
    li t1, 0xaa

    and s0, t0, t1
    # s0 = 0x88

    or  s1, t0, t1
    # s1 = 0xee

    xor s2, t0, t1
    # s2 = 0x66

after_register_ops:
    li  t2, 0x88
    bne s0, t2, failed

    li  t2, 0xee
    bne s1, t2, failed

    li  t2, 0x66
    bne s2, t2, failed

    # =================================================
    # 2. 立即数形式：ANDI、ORI、XORI
    # =================================================

    andi s3, t0, 0x0f
    # s3 = 0x0c

    ori  s4, t0, 0x03
    # s4 = 0xcf

    xori s5, t0, 0xff
    # s5 = 0x33

    # NOT 是 XORI rd, rs, -1 的伪指令
    not  s6, t0
    # s6 = 0xffffffffffffff33

after_immediate_ops:
    li  t2, 0x0c
    bne s3, t2, failed

    li  t2, 0xcf
    bne s4, t2, failed

    li  t2, 0x33
    bne s5, t2, failed

    li  t2, 0xffffffffffffff33
    bne s6, t2, failed

    # =================================================
    # 3. 设置、清除、翻转和测试位
    # =================================================

    li t0, 0x02
    # 初始：bit 1 = 1

    ori t0, t0, 0x08
    # 设置 bit 3：t0 = 0x0a

    andi t0, t0, -3
    # -3 = 0xfffffffffffffffd
    # 清除 bit 1：t0 = 0x08

    xori t0, t0, 0x01
    # 翻转 bit 0：t0 = 0x09

    andi t1, t0, 0x08
    # 测试 bit 3：t1 = 0x08

after_flag_ops:
    li  t2, 0x09
    bne t0, t2, failed

    li  t2, 0x08
    bne t1, t2, failed

    # =================================================
    # 4. 动态单比特掩码
    # =================================================

    li  t0, 1
    li  t1, 12
    sll t0, t0, t1
    # t0 = 1 << 12 = 0x1000

    li t2, 0
    or t2, t2, t0
    # 设置动态 bit 12

    and t3, t2, t0
    srl t3, t3, t1
    # 提取并规范化：t3 = 1

    li  t4, 0x1000
    bne t2, t4, failed

    li  t4, 1
    bne t3, t4, failed

    # =================================================
    # 5. 替换 bits [15:8]
    # =================================================

    li t0, 0x123456789abcdef0

    li  t1, 0xff00
    not t2, t1
    and t0, t0, t2
    # 清除原 bits [15:8]

    li   t3, 0x5a
    andi t3, t3, 0xff
    slli t3, t3, 8
    or   t0, t0, t3
    # 写入新字段 0x5a

after_bitfield:
    li  t4, 0x123456789abc5af0
    bne t0, t4, failed

    # =================================================
    # 6. 使用负 ANDI 立即数对齐到 16 字节
    # =================================================

    li   t0, 0x123f
    andi t0, t0, -16
    # t0 = 0x1230

    li  t1, 0x1230
    bne t0, t1, failed

after_all_tests:
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

## 19. 汇编、链接和运行

```bash
riscv64-linux-gnu-as \
    -march=rv64i \
    -mabi=lp64 \
    -g \
    -o bitwise_demo.o \
    bitwise_demo.S

riscv64-linux-gnu-ld \
    -m elf64lriscv \
    -static \
    -e _start \
    -o bitwise_demo \
    bitwise_demo.o
```

运行：

```bash
qemu-riscv64-static ./bitwise_demo
```

预期输出：

```text
Bitwise instruction demo passed!
```

检查退出状态：

```bash
echo $?
```

预期：

```text
0
```

如果任意验证失败，程序会输出：

```text
Bitwise instruction demo failed!
```

并通过 `exit(1)` 退出。

## 20. 查看真实指令

查看源码、机器码和反汇编：

```bash
riscv64-linux-gnu-objdump -drS bitwise_demo
```

关闭伪指令别名：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    bitwise_demo
```

只查看基础位逻辑指令：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    bitwise_demo | \
rg '\b(and|andi|or|ori|xor|xori)\b'
```

重点观察源码中的：

```asm
not s6, t0
```

在无别名反汇编中会显示成类似：

```asm
xori x22, x5, -1
```

这说明 `NOT` 只是汇编器提供的便利写法，CPU 实际执行的是 `XORI`。

## 21. 使用 GDB 验证结果

终端一：

```bash
qemu-riscv64-static -g 1234 ./bitwise_demo
```

终端二：

```bash
gdb-multiarch ./bitwise_demo
```

连接：

```gdb
target remote :1234
```

### 21.1 查看寄存器形式结果

```gdb
break after_register_ops
continue
info registers s0 s1 s2
```

预期：

```text
s0 = 0x88
s1 = 0xee
s2 = 0x66
```

### 21.2 查看立即数形式和 `NOT`

```gdb
break after_immediate_ops
continue
info registers s3 s4 s5 s6
```

预期：

```text
s3 = 0x000000000000000c
s4 = 0x00000000000000cf
s5 = 0x0000000000000033
s6 = 0xffffffffffffff33
```

### 21.3 查看标志位操作

```gdb
break after_flag_ops
continue
info registers t0 t1
```

预期：

```text
t0 = 0x09
t1 = 0x08
```

### 21.4 查看位域替换

```gdb
break after_bitfield
continue
p/x $t0
```

预期：

```text
$t0 = 0x123456789abc5af0
```

### 21.5 单步观察

```gdb
display/i $pc
display/x $t0
display/x $t1
si
```

## 22. B 扩展与基础位逻辑指令的边界

RISC-V 的位操作扩展通常统称为 B 扩展。现代规范把它拆分成若干子扩展。

| 扩展 | 主要用途 | 代表指令 |
|---|---|---|
| Zba | 地址生成 | `SH1ADD/SH2ADD/SH3ADD` |
| Zbb | 基础位操作 | `ANDN/ORN/XNOR`、`CLZ/CTZ/CPOP`、`ROL/ROR`、`MIN/MAX`、`REV8` |
| Zbc | 无进位乘法 | `CLMUL/CLMULH/CLMULR` |
| Zbs | 单比特操作 | `BSET/BCLR/BINV/BEXT` 及立即数形式 |

例如，基础 RV64I 清除动态位通常需要：

```asm
li  t0, 1
sll t0, t0, a0
not t0, t0
and a1, a1, t0
```

Zbs 可以提供更直接的单比特清除指令：

```asm
bclr a1, a1, a0
```

基础 RV64I 计算人口计数需要软件算法；Zbb 可以使用：

```asm
cpop t0, a0
```

基础 RV64I 按位与非需要两条指令：

```asm
not t0, a1
and t0, a0, t0
```

Zbb 可以使用：

```asm
andn t0, a0, a1
```

这些扩展指令不能在 `-march=rv64i` 下直接使用。必须根据所需扩展选择 `-march`，并确保汇编器、QEMU 或真实处理器都支持对应扩展。例如：

```bash
riscv64-linux-gnu-as \
    -march=rv64i_zbb_zbs \
    -mabi=lp64 \
    -o advanced_bits.o \
    advanced_bits.S
```

本篇主 Demo 刻意只使用 RV64I，保证不依赖可选 B 扩展。Zba/Zbb/Zbc/Zbs 适合在掌握基础逻辑、移位和掩码之后单独展开学习。

## 23. 常见错误和易混淆点

### 23.1 把位逻辑与逻辑真假混为一谈

`AND/OR/XOR` 对 64 个二进制位分别运算，不会自动把非零数规范化为布尔值 1。

例如：

```text
2 & 4 = 0
```

虽然 2 和 4 都是非零值，但它们没有共同的置位。

### 23.2 忘记立即数会符号扩展

```asm
xori t0, t1, -1
```

翻转的是全部 64 位，而不是只翻转低 12 位。

### 23.3 试图用 `ANDI` 编码任意大掩码

`ANDI` 只有符号扩展的 12 位立即数。超出范围的掩码需要通过 `LI + AND` 或其他指令组合实现。

### 23.4 清除位时忘记取反掩码

```asm
and a1, a1, t0        # a1 是原值，t0 是掩码
```

保留的是掩码中为 1 的位，并不是清除这些位。

清除掩码所选位应使用：

```text
value & ~mask
```

### 23.5 修改位域前没有清除原字段

仅仅执行：

```asm
or old_value, old_value, new_field
```

只能设置位，不能把原字段中的 1 清成 0。替换位域必须先清除旧字段，再合并新字段。

### 23.6 把寄存器更新误认为内存原子操作

`LD + OR + SD` 不是原子操作。并发修改共享内存时需要 A 扩展或锁。

### 23.7 把 `NOT` 当成独立 RV64I 编码

`NOT` 是：

```asm
xori rd, rs, -1
```

使用无别名反汇编才能看到 CPU 实际执行的指令。

## 24. 本部分小结

RV64I 基础位逻辑指令只有 6 条真实指令：

```text
寄存器形式：AND OR XOR
立即数形式：ANDI ORI XORI
```

核心规则是：

1. 所有基础位逻辑运算都对完整 64 位逐位执行。
2. `AND` 用于保留、清除和测试位。
3. `OR` 用于设置位和合并位域。
4. `XOR` 用于翻转位和寻找差异。
5. `NOT` 是 `XORI rd, rs, -1` 的伪指令。
6. `ANDI/ORI/XORI` 使用符号扩展的 12 位立即数。
7. RV64I 没有 `ANDW/ORW/XORW`。
8. 修改内存中的位通常需要 load-modify-store，这个序列不是原子的。
9. Zba/Zbb/Zbc/Zbs 属于可选 B 扩展，不应和基础 RV64I 混为一谈。

最常用的四种位操作模式是：

```text
设置：value | mask
清除：value & ~mask
翻转：value ^ mask
测试：value & mask
```

学习时建议同时查看无别名反汇编：

```bash
riscv64-linux-gnu-objdump -d -M no-aliases,numeric bitwise_demo
```

这样可以清楚地区分 RV64I 真实指令和 `NOT` 等汇编器伪指令。
