# RISC-V 64 位 ISA：PC Relative Instructions

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准；“真实指令”表示有机器编码，但可能属于可选扩展或特权架构，并不表示仅用 `-march=rv64i` 就能汇编、执行。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `ADDI` | **Add Immediate** | 真实指令 |
| `AUIPC` | **Add Upper Immediate to PC** | 真实指令 |
| `BEQ` | **Branch if Equal** | 真实指令 |
| `BEQZ` | **Branch if Equal to Zero** | 伪指令 |
| `BGE` | **Branch if Greater Than or Equal** | 真实指令 |
| `BGEU` | **Branch if Greater Than or Equal Unsigned** | 真实指令 |
| `BGT` | **Branch if Greater Than** | 伪指令 |
| `BLE` | **Branch if Less Than or Equal** | 伪指令 |
| `BLT` | **Branch if Less Than** | 真实指令 |
| `BLTU` | **Branch if Less Than Unsigned** | 真实指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `BNEZ` | **Branch if Not Equal to Zero** | 伪指令 |
| `CALL` | **Call** | 伪指令 |
| `ECALL` | **Environment Call** | 真实指令 |
| `J` | **Jump** | 伪指令 |
| `JAL` | **Jump And Link** | 真实指令 |
| `JALR` | **Jump And Link Register** | 真实指令 |
| `JR` | **Jump Register** | 伪指令 |
| `LA` | **Load Address** | 伪指令 |
| `LB` | **Load Byte** | 真实指令 |
| `LD` | **Load Doubleword** | 真实指令 |
| `LH` | **Load Halfword** | 真实指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `LUI` | **Load Upper Immediate** | 真实指令 |
| `LW` | **Load Word** | 真实指令 |
| `RET` | **Return** | 伪指令 |
| `SD` | **Store Doubleword** | 真实指令 |
| `TAIL` | **Tail Call** | 伪指令 |

ABI 别名是同一套 `x0～x31` 整数寄存器的另一种写法，并非额外寄存器；下面给出本文提及的整数寄存器编号与对应 ABI 别名。`PC` 若在本文出现，指 **Program Counter**（程序计数器），它不属于 `x0～x31`。

| ABI 别名 | 硬件编号 | 英文完整名称 |
|---|---|---|
| `zero` | `x0` | **Zero** |
| `ra` | `x1` | **Return Address** |
| `t0` | `x5` | **Temporary register 0** |
| `t1` | `x6` | **Temporary register 1** |
| `t2` | `x7` | **Temporary register 2** |
| `s0` | `x8` | **Saved register 0 / Frame Pointer（作 `fp` 时）** |
| `a0` | `x10` | **Argument register 0** |
| `a1` | `x11` | **Argument register 1** |
| `a2` | `x12` | **Argument register 2** |
| `a7` | `x17` | **Argument register 7** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

本文是 RISC-V 64 位 ISA 系列的第二部分，集中讲解 PC 相对寻址相关指令、常见伪指令、链接器重定位，以及对应的运行和调试方法。

PC 相对寻址的核心思想是：

```text
目标地址 = 当前指令的 PC + 有符号偏移量
```

代码无论被加载到什么地址，只要代码与目标之间的相对距离不变，仍然可以正常工作。因此，PC 相对寻址是位置无关代码、动态链接、共享库和 ASLR 的重要基础。

## 1. 涉及 PC 相对寻址的指令

| 指令 | 作用 | 相对范围 |
|---|---|---:|
| `AUIPC` | 以 PC 为基准构造地址 | 高 20 位立即数，约 ±2 GiB |
| `BEQ/BNE` | 相等/不等时跳转 | `-4096～4094` 字节 |
| `BLT/BGE` | 有符号比较后跳转 | `-4096～4094` 字节 |
| `BLTU/BGEU` | 无符号比较后跳转 | `-4096～4094` 字节 |
| `JAL` | PC 相对无条件跳转并保存返回地址 | `-1048576～1048574` 字节 |
| `JALR` | 寄存器间接跳转 | 本身不是 PC 相对，但常和 `AUIPC` 组合 |

常见伪指令也大量依赖这些指令：

| 伪指令 | 典型展开 |
|---|---|
| `la rd, symbol` | `AUIPC + ADDI` |
| `call function` | `AUIPC + JALR`，近距离时可能被优化为 `JAL` |
| `tail function` | `AUIPC + JALR`，不保存返回地址 |
| `j label` | `JAL x0, label` |
| `beqz rs, label` | `BEQ rs, x0, label` |
| `bnez rs, label` | `BNE rs, x0, label` |
| `ret` | `JALR x0, 0(ra)` |

这里需要区分：

- `AUIPC`、条件分支、`JAL` 是直接使用 PC 的真实指令。
- `JALR` 本身以通用寄存器为基址，但常由 `AUIPC` 先生成一个 PC 相对地址。

## 2. 什么是 PC

PC，即 Program Counter，保存当前正在执行的指令地址。

假设：

```text
当前 AUIPC 指令位于 0x10000
```

执行：

```asm
auipc t0, 0
```

结果为：

```text
t0 = 0x10000
```

这里使用的是 `AUIPC` 指令自身的地址，而不是下一条指令地址。

如果执行：

```asm
auipc t0, 1
```

结果为：

```text
t0 = 0x10000 + 0x1000
   = 0x11000
```

因为 `AUIPC` 的 20 位立即数会先左移 12 位。

## 3. `AUIPC`：PC 相对地址构造

`AUIPC` 是：

```text
Add Upper Immediate to PC
```

基本语法：

```asm
auipc rd, imm20
```

执行效果：

```text
rd = PC + sign_extend(imm20 << 12)
```

例如：

```asm
auipc t0, 0
```

得到当前 `AUIPC` 指令的地址。

```asm
auipc t0, 1
```

得到：

```text
当前 PC + 0x1000
```

```asm
auipc t0, 0xfffff
```

在 RV64 中，相当于：

```text
当前 PC - 0x1000
```

因为移位后的 32 位值会符号扩展到 64 位。

### 3.1 与 `LUI` 的区别

`LUI` 和 `AUIPC` 都使用 U-type 编码，但语义不同：

```asm
lui   t0, 1
auipc t1, 1
```

结果分别是：

```text
t0 = 0x1000
t1 = PC + 0x1000
```

因此：

- `LUI` 用于构造数值。
- `AUIPC` 用于构造相对于当前代码位置的地址。

### 3.2 直接获取当前 PC

```asm
_start:
    auipc s0, 0
```

执行后：

```text
s0 = &_start
```

前提是 `AUIPC` 正好位于 `_start` 标签处。

RISC-V 没有专门的“读取 PC”指令，`AUIPC rd, 0` 就是常见实现方式。

## 4. 用 `AUIPC + ADDI` 获取符号地址

`AUIPC` 加到 PC 上的偏移量低 12 位为零，**结果地址的低 12 位不一定为零**。例如，`AUIPC` 位于 `0x10004` 时，`auipc t0, 1` 得到 `0x11004`。如果目标相对该指令的偏移还有低 12 位部分，通常需要配合 `ADDI`。

概念上：

```asm
auipc t0, address_high
addi  t0, t0, address_low
```

最终得到：

```text
t0 = 目标符号的地址
```

实际编程不应手工计算高低位，而应使用链接器重定位：

```asm
.Laddress:
    auipc t0, %pcrel_hi(target)
    addi  t0, t0, %pcrel_lo(.Laddress)
```

注意，`%pcrel_lo` 参数指向的是对应 `AUIPC` 所在的标签，而不是简单地再次填写 `target`。

链接器负责计算：

```text
delta = target - AUIPC_PC
```

然后将其分成高 20 位和有符号低 12 位。

### 4.1 为什么不能简单切割高低位

低 12 位会被解释成有符号数。例如目标偏移是：

```text
0x1800
```

不能简单写成：

```text
高位 = 0x1
低位 = 0x800
```

因为 12 位的 `0x800` 表示 `-2048`。

正确拆分应类似：

```text
高位 = 0x2
低位 = -0x800
```

链接器一般使用类似下面的计算：

```text
hi20 = (delta + 0x800) >> 12
lo12 = delta - (hi20 << 12)
```

因此应尽量让 `%pcrel_hi/%pcrel_lo` 或 `la` 伪指令处理这些细节。

### 4.2 `la` 伪指令

通常直接写：

```asm
la t0, target
```

在非 PIC/GOT 模式下，它通常展开成：

```asm
auipc t0, %pcrel_hi(target)
addi  t0, t0, %pcrel_lo(...)
```

为了明确采用非 PIC 地址生成方式，可以在源码开头使用：

```asm
.option nopic  # 后续 la 使用非 PIC 形式；局部符号可用 PC 相对寻址，不经 GOT，也不强制绝对地址
```

## 5. PC 相对数据访问

RV64I 没有类似 x86-64：

```text
mov value(%rip), %rax
```

这样的单条 PC 相对加载指令。

访问 PC 附近的数据通常分两步：

1. `AUIPC` 生成地址高位。
2. `LD/LW/LH/LB` 使用低 12 位完成加载。

例如：

```asm
.Lload_value:
    auipc t0, %pcrel_hi(value)
    ld    t1, %pcrel_lo(.Lload_value)(t0)
```

执行后：

```text
t1 = value 中保存的 64 位数据
```

地址计算过程为：

```text
t0 = AUIPC_PC + high_part(value - AUIPC_PC)
t1 = memory[t0 + low_part(value - AUIPC_PC)]
```

也可以先使用 `la` 获得地址：

```asm
la t0, value
ld t1, 0(t0)
```

后一种写法更容易阅读，但查看真实 ISA 指令时，应使用 `objdump` 观察 `la` 的展开。

存储也采用相同思路：

```asm
.Lstore_value:
    auipc t0, %pcrel_hi(destination)
    sd    t1, %pcrel_lo(.Lstore_value)(t0)
```

## 6. 条件分支指令

RISC-V 没有单独的条件码寄存器。条件分支指令直接比较两个整数寄存器。

所有基础整数条件分支都是 PC 相对跳转：

```text
如果条件成立：
    PC = 当前分支指令的 PC + 偏移量
否则：
    PC = PC + 4
```

### 6.1 条件分支汇总

| 指令 | 条件 | 比较方式 |
|---|---|---|
| `BEQ rs1, rs2, label` | `rs1 == rs2` | 相等 |
| `BNE rs1, rs2, label` | `rs1 != rs2` | 不相等 |
| `BLT rs1, rs2, label` | `rs1 < rs2` | 有符号 |
| `BGE rs1, rs2, label` | `rs1 >= rs2` | 有符号 |
| `BLTU rs1, rs2, label` | `rs1 < rs2` | 无符号 |
| `BGEU rs1, rs2, label` | `rs1 >= rs2` | 无符号 |

分支偏移范围为：

```text
-4096 ～ 4094 字节
```

偏移最低位不编码，隐含为零，所以目标地址至少按 2 字节对齐。

### 6.2 `BEQ` Demo

```asm
li t0, 10
li t1, 10

beq t0, t1, equal
```

因为两个寄存器相等，所以跳转到 `equal`。

### 6.3 `BNE` Demo

```asm
li t0, 10
li t1, 20

bne t0, t1, not_equal
```

### 6.4 `BLT/BGE` 有符号比较

```asm
li t0, -1
li t1, 1

blt t0, t1, signed_less
```

条件成立，因为按有符号数解释：

```text
-1 < 1
```

```asm
bge t1, t0, signed_greater_equal
```

条件也成立，因为：

```text
1 >= -1
```

### 6.5 `BLTU/BGEU` 无符号比较

仍然使用：

```text
t0 = 0xffffffffffffffff
t1 = 0x0000000000000001
```

执行：

```asm
bltu t0, t1, unsigned_less
```

条件不成立，因为按无符号数解释：

```text
0xffffffffffffffff > 1
```

执行：

```asm
bgeu t0, t1, unsigned_greater_equal
```

条件成立。

这说明同一组二进制位，在有符号和无符号比较中可能产生完全不同的结果。

### 6.6 常见分支伪指令

```asm
beqz t0, label
```

展开为：

```asm
beq t0, x0, label
```

```asm
bnez t0, label
```

展开为：

```asm
bne t0, x0, label
```

```asm
ble t0, t1, label
```

通常通过交换操作数展开为：

```asm
bge t1, t0, label
```

类似地：

```asm
bgt t0, t1, label
```

展开为：

```asm
blt t1, t0, label
```

## 7. `JAL`：PC 相对跳转并保存返回地址

`JAL` 表示：

```text
Jump And Link
```

语法：

```asm
jal rd, label
```

执行效果：

```text
rd = PC + 4
PC = PC + sign_extend(offset)
```

也就是说：

1. 把下一条指令地址保存到 `rd`。
2. 跳转到 PC 相对目标地址。

跳转范围为：

```text
-1048576 ～ 1048574 字节
```

也就是大约前后 1 MiB。

### 7.1 函数调用

```asm
jal ra, function
```

执行后：

```text
ra = JAL 指令地址 + 4
PC = function
```

函数可以通过：

```asm
jalr x0, 0(ra)
```

返回。

对应的常用伪指令是：

```asm
ret
```

### 7.2 无条件跳转

如果不需要保存返回地址，可以把结果写入 `x0`：

```asm
jal x0, target
```

常用伪指令是：

```asm
j target
```

因此：

```asm
j target
```

等价于：

```asm
jal x0, target
```

### 7.3 简单函数 Demo

```asm
    li a0, 40
    jal ra, add_one

    # 返回后 a0 = 41
    j done              # 跳过函数体，避免返回后再次落入 add_one

add_one:
    addi a0, a0, 1
    jalr x0, 0(ra)

done:
    # 继续执行调用者的后续代码
```

## 8. `JALR`：寄存器间接跳转

`JALR` 表示：

```text
Jump And Link Register
```

语法：

```asm
jalr rd, imm(rs1)
```

执行效果：

```text
rd = PC + 4
PC = (rs1 + sign_extend(imm12)) & ~1
```

跳转目标最低位会被清零。

`JALR` 本身不是 PC 相对寻址，因为目标来自通用寄存器。但它经常和 `AUIPC` 配合，构成远距离 PC 相对调用。

### 8.1 `AUIPC + JALR`

```asm
.Lcall:
    auipc t0, %pcrel_hi(function)
    jalr  ra, %pcrel_lo(.Lcall)(t0)
```

其整体效果是：

```text
ra = 返回地址
PC = function
```

这种组合的相对寻址能力大约为前后 2 GiB，比单条 `JAL` 的约 ±1 MiB 范围大得多。

### 8.2 `call` 伪指令

通常直接写：

```asm
call function
```

汇编器和链接器通常生成：

```asm
auipc ra, ...
jalr  ra, ...(ra)
```

如果最终链接时发现函数位于 `JAL` 的范围内，链接器还可能将其优化成：

```asm
jal ra, function
```

这种优化称为链接器松弛，即 linker relaxation。

### 8.3 返回和寄存器间接跳转

```asm
ret
```

展开为：

```asm
jalr x0, 0(ra)
```

```asm
jr t0
```

展开为：

```asm
jalr x0, 0(t0)
```

它们都把链接结果写入 `x0`，所以不会保存新的返回地址。

## 9. 完整 PC 相对寻址 Demo

下面的程序演示：

- `AUIPC rd, 0` 获取当前 PC。
- `AUIPC + LD` 进行 PC 相对数据访问。
- 条件分支 `BNE`。
- `JAL` 进行近距离函数调用。
- `AUIPC + JALR` 进行远距离形式的函数调用。
- `la` 获取字符串地址。
- `JALR` 返回函数。

保存为 `pcrel_demo.S`：

下面代码中，以点开头的名称是汇编伪操作，用来设置选项、布置节/符号或生成数据；它们不是 RV64 CPU 指令。`la`、`li` 等没有点号的写法则可能是由汇编器展开的伪指令（见第 0 节）。本例没有使用 `.macro`/`.endm` 定义用户宏。[RISC-V 汇编手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)、[GNU as 伪操作手册](https://sourceware.org/binutils/docs/as/Pseudo-Ops.html)

```asm
    .option nopic  # 后续 la 使用非 PIC 形式；局部符号可用 PC 相对寻址，不经 GOT，也不强制绝对地址
    .option norelax  # 后续序列不标记为可链接器放宽；此处避免未初始化 gp 时被改写成 gp 相对寻址

    .section .rodata  # 切换到只读数据节，后续整数或字符串常量放在这里
    .balign 8  # 按字节向上对齐到 8 的倍数，必要时插入填充；使 64 位数据自然对齐

value:
    .quad 0x1122334455667788  # 在当前节放入一个 8 字节整数数据；不是运行时的 LD/SD 指令

message:
    .ascii "PC-relative demo passed!\n"  # 按原样写入字符串字节；\n 是换行，不会自动追加 NUL 结束字节
    .equ message_len, . - message  # 汇编期定义长度常量：点号是当前位置，减去字符串起始标签得到字节数；不分配存储

    .section .text  # 切换到可执行代码节，后续指令在这里编码
    .globl _start  # 将 _start 标成 ELF 全局符号；入口地址仍由链接器的 -e 或 ENTRY 指定

_start:
    # ---------------------------------
    # 1. 获取当前 AUIPC 指令的 PC
    # ---------------------------------

    auipc s0, 0

after_capture_pc:
    # 因为 AUIPC 是 _start 的第一条指令：
    # s0 = &_start

    # ---------------------------------
    # 2. AUIPC + LD：PC 相对加载
    # ---------------------------------

load_value:
    auipc t0, %pcrel_hi(value)
    ld    t1, %pcrel_lo(load_value)(t0)

after_load:
    li t2, 0x1122334455667788  # 伪指令：把常量构造到寄存器，大常量可能需要多条真实指令
    bne t1, t2, failed

    # ---------------------------------
    # 3. JAL：近距离 PC 相对调用
    # ---------------------------------

    li  a0, 40
    jal ra, add_one

after_jal:
    li  t0, 41
    bne a0, t0, failed

    # ---------------------------------
    # 4. AUIPC + JALR：远调用形式
    # ---------------------------------

    li a0, 41

far_call:
    auipc t0, %pcrel_hi(add_one)
    jalr  ra, %pcrel_lo(far_call)(t0)

after_far_call:
    li  t1, 42
    bne a0, t1, failed

    # ---------------------------------
    # 5. BNE：PC 相对循环
    # ---------------------------------

    li t2, 3

loop:
    addi t2, t2, -1
    bne  t2, x0, loop

    # ---------------------------------
    # 6. la：PC 相对获取字符串地址
    # ---------------------------------

    li a0, 1
    la a1, message  # 伪指令：构造符号地址，不读取该地址的内容；可能展开为 AUIPC+ADDI 等
    li a2, message_len
    li a7, 64
    ecall

    # exit(0)
    li a0, 0
    li a7, 93
    ecall

add_one:
    addi a0, a0, 1

    # ret 的真实形式
    jalr x0, 0(ra)

failed:
    # exit(1)
    li a0, 1
    li a7, 93
    ecall
```

这里使用：

```asm
.option norelax  # 后续序列不标记为可链接器放宽；此处避免未初始化 gp 时被改写成 gp 相对寻址
```

是为了阻止链接器将 `AUIPC + JALR` 等组合优化成更短的形式，方便观察原始 PC 相对序列。生产代码通常不需要禁用 relaxation。

## 10. 汇编、链接和运行

```bash
riscv64-linux-gnu-as \
    -march=rv64i \
    -mabi=lp64 \
    -g \
    -o pcrel_demo.o \
    pcrel_demo.S

riscv64-linux-gnu-ld \
    -m elf64lriscv \
    -static \
    -e _start \
    -o pcrel_demo \
    pcrel_demo.o
```

运行：

```bash
qemu-riscv64-static ./pcrel_demo
```

预期输出：

```text
PC-relative demo passed!
```

检查退出状态：

```bash
echo $?
```

预期：

```text
0
```

如果 PC 相对加载、函数调用或分支验证失败，程序会执行：

```text
exit(1)
```

## 11. 查看真实指令

带伪指令反汇编：

```bash
riscv64-linux-gnu-objdump -drS pcrel_demo
```

关闭伪指令别名：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    pcrel_demo
```

重点观察：

```asm
la a1, message
```

是否展开为类似：

```asm
auipc x11, ...
addi  x11, x11, ...
```

观察函数返回：

```asm
jalr x0, 0(x1)
```

默认反汇编通常会把它显示为：

```asm
ret
```

使用 `-M no-aliases` 后才能明确看到真实的 `JALR`。

查看目标文件中的重定位：

```bash
riscv64-linux-gnu-objdump -dr pcrel_demo.o
```

可能看到：

```text
R_RISCV_PCREL_HI20
R_RISCV_PCREL_LO12_I
```

这些重定位告诉链接器如何计算符号与 `AUIPC` 之间的相对距离。

## 12. 使用 GDB 验证 PC 相对寻址

终端一：

```bash
qemu-riscv64-static -g 1234 ./pcrel_demo
```

终端二：

```bash
gdb-multiarch ./pcrel_demo
```

连接：

```gdb
target remote :1234
```

### 12.1 验证 `AUIPC rd, 0`

```gdb
break after_capture_pc
continue
p/x $s0
p/x &_start
```

两个地址应当相同，因为：

```asm
_start:
    auipc s0, 0
```

`AUIPC` 正好位于 `_start`。

### 12.2 验证 PC 相对加载

```gdb
break after_load
continue
p/x $t0
p/x $t1
p/x &value
x/gx &value
```

预期：

```text
t1 = 0x1122334455667788
```

`t0` 是通过 `AUIPC` 形成的中间地址基址，不一定恰好等于 `value`，因为剩余低 12 位由 `LD` 的偏移承担。

### 12.3 验证 `JAL`

```gdb
break after_jal
continue
p/x $a0
p/x $ra
```

预期：

```text
a0 = 41
ra = after_jal
```

### 12.4 验证 `AUIPC + JALR`

```gdb
break after_far_call
continue
p/x $a0
p/x $ra
```

预期：

```text
a0 = 42
ra = after_far_call
```

查看当前指令：

```gdb
x/10i $pc
```

单步执行：

```gdb
display/i $pc
si
```

## 13. 地址范围不足时会发生什么

### 13.1 条件分支超出约 ±4 KiB

如果分支目标太远，单条 B-type 指令无法编码。

例如，原本希望在 `t0 == t1` 时跳往 `far_target`，可反转近距离分支的条件：

```asm
bne t0, t1, skip
j   far_target

skip:
```

这样只有 `t0 == t1` 时才执行远跳转；若 `t0 != t1`，则跳过它。这里的 `j` 仍需满足 `JAL` 的编码范围；目标更远时还要使用更长的跳转序列，参见[寻址范围](10-rv64-addressing-ranges.md)。汇编器或链接器是否自动扩展，取决于工具链和具体写法。

如果无法自动处理，可能报告：

```text
relocation truncated to fit
```

### 13.2 `JAL` 超出约 ±1 MiB

需要改用：

```asm
call function
```

或明确使用：

```asm
auipc ...
jalr  ...
```

### 13.3 `AUIPC + JALR` 也不可达

普通 PC 相对组合大约覆盖 ±2 GiB。如果目标仍不可达，就需要依赖更大的代码模型、GOT、PLT、函数指针或其他地址构造方式。

## 14. 初学时容易混淆的问题

### 14.1 PC 是当前指令地址

对于：

```asm
auipc t0, 0
```

`t0` 得到的是 `AUIPC` 自身的地址，不是下一条指令地址。

只有 `JAL/JALR` 写入返回地址时使用：

```text
PC + 4
```

### 14.2 `JALR` 不是直接 PC 相对指令

```asm
jalr ra, 0(t0)
```

目标来自 `t0`，所以这是寄存器间接跳转。

下面的组合整体上才是 PC 相对调用：

```asm
auipc t0, ...
jalr  ra, ...(t0)
```

### 14.3 `la` 不是一条真实硬件指令

```asm
la t0, symbol
```

通常会展开为两条或更多真实指令。实际展开还可能受到以下因素影响：

- PIC/非 PIC 模式
- 符号可见性
- GOT/PLT
- 链接器 relaxation
- 代码模型

### 14.4 分支比较不依赖状态标志

RISC-V 没有像 x86 那样先使用 `CMP` 设置标志寄存器，再执行条件跳转。

RISC-V 直接写：

```asm
blt t0, t1, target
```

比较和分支由一条指令完成。

### 14.5 有符号和无符号分支必须区分

```asm
blt  t0, t1, target
bltu t0, t1, target
```

两条指令比较的是相同的寄存器位，但数值解释不同。

## 15. 本部分小结

PC 相对寻址可以总结为四种核心模式：

```text
取得当前位置：
    AUIPC rd, 0

取得附近符号地址：
    AUIPC + ADDI
    或 la 伪指令

访问附近数据：
    AUIPC + LOAD/STORE

跳转到附近代码：
    条件分支
    JAL

远距离 PC 相对函数调用：
    AUIPC + JALR
    或 call 伪指令
```

最需要记住的范围是：

```text
条件分支：   约 ±4 KiB
JAL：        约 ±1 MiB
AUIPC 组合：约 ±2 GiB
```

学习这部分时，应同时观察源代码、目标文件重定位和最终反汇编：

```bash
riscv64-linux-gnu-objdump -dr pcrel_demo.o
riscv64-linux-gnu-objdump -d -M no-aliases,numeric pcrel_demo
```

这样可以清楚地区分“源码中的伪指令”“目标文件中的重定位”和“CPU 最终执行的真实指令”。
