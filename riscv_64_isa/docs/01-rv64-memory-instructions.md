# RISC-V 64 位 ISA 入门：整数内存访问指令

## 0. 本篇指令与寄存器缩写速查

指令助记符通常由操作类型和数据宽度组成：`L` = **Load**（加载），`S` = **Store**（存储），`B` = **Byte**（8 位字节），`H` = **Halfword**（16 位半字），`W` = **Word**（32 位字），`D` = **Doubleword**（64 位双字）。加载指令末尾的 `U` = **Unsigned**，表示把读出的窄数据**零扩展**到 64 位；没有 `U` 的窄加载会符号扩展。存储指令只选取源寄存器的低位，因此没有相应的 `SBU/SHU/SWU` 指令。

| 本篇访存指令 | 英文完整名称 | 本篇含义 |
|---|---|---|
| `LB` / `LBU` | **Load Byte** / **Load Byte Unsigned** | 读取 8 位，分别符号扩展 / 零扩展 |
| `LH` / `LHU` | **Load Halfword** / **Load Halfword Unsigned** | 读取 16 位，分别符号扩展 / 零扩展 |
| `LW` / `LWU` | **Load Word** / **Load Word Unsigned** | 读取 32 位，分别符号扩展 / 零扩展 |
| `LD` | **Load Doubleword** | 读取完整 64 位 |
| `SB` | **Store Byte** | 写入源寄存器最低 8 位 |
| `SH` | **Store Halfword** | 写入源寄存器最低 16 位 |
| `SW` | **Store Word** | 写入源寄存器最低 32 位 |
| `SD` | **Store Doubleword** | 写入源寄存器全部 64 位 |

下面这些名称也出现在正文或 Demo 中；它们不属于上表的整数访存指令。

| 助记符 | 英文完整名称 | 类型与用途 |
|---|---|---|
| `ADDI` | **Add Immediate** | 真实指令；寄存器值加立即数，例如调整 `sp` |
| `BNE` | **Branch if Not Equal** | 真实指令；两个寄存器不相等时跳转 |
| `JALR` | **Jump And Link Register** | 真实指令；本篇的 `RET` 伪指令可展开为它 |
| `ECALL` | **Environment Call** | 真实指令；本篇 Linux 用户态 Demo 用它发起 `exit` 系统调用 |
| `FENCE` / `FENCE.I` | **Fence** / **Instruction-Fetch Fence** | 真实指令；`FENCE.I` 属于 Zifencei 扩展；详见[系统与屏障指令](11-rv64-system-and-fence-instructions.md) |
| `ADD` / `SUB` | **Add** / **Subtract** | 真实指令；文末预告，详见[算术指令](06-rv64-arithmetic-instructions.md) |
| `ADDW` / `SUBW` / `ADDIW` | **Add Word** / **Subtract Word** / **Add Immediate Word** | 真实指令；文末预告的 RV64 32 位结果形式，详见[算术指令](06-rv64-arithmetic-instructions.md) |
| `LA` / `LI` | **Load Address** / **Load Immediate** | 汇编器伪指令；分别构造符号地址和立即数，可能展开为多条真实指令 |
| `NOP` / `RET` | **No Operation** / **Return** | 汇编器伪指令；在本篇 RV64I 示例中分别可展开为 `addi x0,x0,0` / `jalr x0,0(ra)` |

`LA` 的“Load”**不表示读取该地址里的内存内容**：`la t0,value` 得到 `value` 的地址，`ld t1,0(t0)` 才读取内存。`.option`、`.section`、`.byte` 等以点开头的名称是汇编伪操作（directive），用于控制汇编或生成数据，不是 CPU 执行的指令。[官方汇编手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)

下面的 `t/s/a` 等名称是整数寄存器 `x0～x31` 的 **ABI（Application Binary Interface，应用二进制接口）别名**，不是额外的物理寄存器。表中列出本文实际出现的全部寄存器名称；`s0` 也叫 `fp`，但本文示例只写作 `s0`。[官方寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)

| 本文名称 | 硬件编号 | 英文名称与通常用途 |
|---|---|---|
| `x0`（`zero`） | `x0` | **Zero**；恒为 0，写入被忽略 |
| `ra` | `x1` | **Return Address**；函数返回地址 |
| `sp` | `x2` | **Stack Pointer**；栈指针 |
| `gp` | `x3` | **Global Pointer**；全局指针，不能随意作临时寄存器 |
| `t0` / `t1` / `t2` | `x5` / `x6` / `x7` | **Temporary register 0/1/2**；临时寄存器 |
| `s0` / `s1` | `x8` / `x9` | **Saved register 0/1**；被调用者保存，`s0` 也可作帧指针 `fp` |
| `a0` / `a7` | `x10` / `x17` | **Argument register 0/7**；参数寄存器；本文 Linux `exit` 用 `a0` 放状态码、`a7` 放系统调用号 |
| `s2` | `x18` | **Saved register 2**；被调用者保存 |
| `t3` / `t4` / `t5` / `t6` | `x28` / `x29` / `x30` / `x31` | **Temporary register 3/4/5/6**；临时寄存器 |

注意 `t0～t6` 的硬件编号**不连续**：`t0～t2=x5～x7`，`t3～t6=x28～x31`。在 ABI 中 `s` 寄存器由被调用函数保存并恢复，`a/t` 寄存器通常由调用者按需保存；这属于软件调用约定，不改变加载或存储指令的硬件语义。[官方 RV32/RV64 整数访存定义](https://docs.riscv.org/reference/isa/unpriv/rv32.html)、[RV64 特有访存定义](https://docs.riscv.org/reference/isa/unpriv/rv64.html)

> t0, t1, t2, t3, t4, t5, t6 对应的就是原始的x5, x6, x7, x28, x29, x30, x31 这些寄存器，t代表的就是tmp了，也就是临时寄存器，可以用于保存临时中间状态的寄存器了
> a0, a1, a2, a3, a4, a5, a6, a7 对应的就是原始的 x10, x11, x12, x13, x14, x15, x16, x17这些寄存器，a代表的就是argument, 这些参数用于保存参数，如果参数超过8个，那么就使用stack来保存参数，另外结果也是用a0~a7这些寄存器来保存
> s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11 对应的就是原始的 x8, x9, x18, x19, x20, x21, x22, x23, x24, x25, x26, x27 这些寄存器，这些寄存器是callee save, 也就是被调用者来负责保存的，例如func_a调用func_b, 那么就是func_b来负责保存s0~s11这12个寄存器


本文是 RISC-V 64 位 ISA 系列的第一篇，首先建立整个学习范围，然后集中讲解 RV64I 中的整数内存访问指令，并提供可以直接汇编、运行和调试的完整示例。

完整章节目录和已覆盖范围见[系列目录](README.md)。

## 1. 学习范围和后续路线

首先要明确一个关键点：RISC-V 并不存在唯一的“64 位全部指令集”。`RV64` 只规定整数寄存器宽度为 64 位，实际处理器支持的完整能力由基础指令集和扩展组合决定，例如：

- `RV64I`：基础整数指令集
- `M`：乘法、除法
- `A`：原子操作
- `F/D`：单精度、双精度浮点
- `C`：压缩编码
- `B`：位操作
- `V`：向量
- `Zicsr`：CSR 寄存器访问
- `Zifencei`：指令流同步

建议先以 `RV64I` 为主线，按照功能学习，再按需进入常见扩展。下面是学习路线；当前系列已经写到哪里，以[系列目录](README.md)为准：

1. 整数内存访问：本篇
2. 整数算术运算
3. 逻辑、移位与比较
4. 条件分支与函数跳转
5. 地址构造和立即数
6. 乘法与除法 `M`
7. 原子操作 `A`
8. 内存屏障与指令同步
9. CSR、异常和系统调用
10. 浮点 `F/D`
11. 压缩指令 `C`
12. 位操作 `B`、向量 `V` 等进阶扩展

本篇只讲 `RV64I` 的整数加载和存储。浮点加载/存储、向量加载/存储以及原子访存属于其他扩展；`FENCE`、`FENCE.I` 和系统调用陷入的区别见[系统与屏障指令](11-rv64-system-and-fence-instructions.md)。

## 2. RISC-V 的 Load/Store 架构

RISC-V 是典型的 Load/Store 架构：

```text
内存 --load--> 寄存器 --运算--> 寄存器 --store--> 内存
```

普通算术指令不能直接操作内存。例如，对内存中的 64 位整数加一，需要先加载、再计算、最后写回：

```asm
ld   t0, 0(a0)       # 从内存加载
addi t0, t0, 1       # 在寄存器中计算
sd   t0, 0(a0)       # 写回内存
```

RV64I 基础整数访存指令采用下面两种 GNU 汇编语法：

```asm
加载指令  目标寄存器, 偏移量(基址寄存器)
存储指令  源寄存器,   偏移量(基址寄存器)
```

有效地址计算方式为：

```text
有效地址 = 基址寄存器的值 + 符号扩展后的 12 位立即数
```

因此，单条普通加载或存储指令能够直接表示的偏移范围是：

```text
-2048 ～ 2047 字节
```

偏移量的单位是字节，不是数组元素个数。假设 `s0 = 0x1000`：

```asm
lw t0, 4(s0)         # 访问地址 0x1004
ld t1, 8(s0)         # 访问地址 0x1008
```

如果目标地址距离基址超过这个范围，需要先通过其他指令计算新的基址，再进行加载或存储。

## 3. RV64I 整数加载指令汇总

| 指令 | 读取宽度 | 写入 64 位寄存器的方式 |
|---|---:|---|
| `LB` | 8 位 | 符号扩展 |
| `LBU` | 8 位 | 零扩展 |
| `LH` | 16 位 | 符号扩展 |
| `LHU` | 16 位 | 零扩展 |
| `LW` | 32 位 | 符号扩展 |
| `LWU` | 32 位 | 零扩展 |
| `LD` | 64 位 | 原样加载 |

指令名中的字母含义如下：

- `B`：byte，8 位
- `H`：half word，16 位
- `W`：word，32 位
- `D`：double word，64 位
- `U`：unsigned，加载后执行零扩展
- 没有 `U` 的窄加载会执行符号扩展

需要特别注意：这里所说的“有符号加载”和“无符号加载”，区别只在于数据写入 64 位目标寄存器时，高位如何填充。它们从内存中读取的字节数完全相同。

### 3.1 `LB`：加载有符号字节

假设 `s0` 指向的内存字节是：

```text
0x80
```

执行：

```asm
lb t0, 0(s0)
```

结果为：

```text
t0 = 0xffffffffffffff80
```

`0x80` 作为 8 位有符号数表示 `-128`，因此扩展到 64 位时，高 56 位全部填 `1`。

### 3.2 `LBU`：加载无符号字节

```asm
lbu t0, 0(s0)
```

结果为：

```text
t0 = 0x0000000000000080
```

读取的仍然是同一个字节，但扩展到 64 位时，高 56 位全部填 `0`。

### 3.3 `LH`：加载有符号半字

假设地址 `s0 + 2` 中保存着 16 位数据 `0x8001`：

```asm
lh t0, 2(s0)
```

结果为：

```text
t0 = 0xffffffffffff8001
```

### 3.4 `LHU`：加载无符号半字

```asm
lhu t0, 2(s0)
```

结果为：

```text
t0 = 0x0000000000008001
```

### 3.5 `LW`：加载有符号 32 位字

假设地址 `s0 + 4` 中保存着 `0x80000001`：

```asm
lw t0, 4(s0)
```

结果为：

```text
t0 = 0xffffffff80000001
```

这是 RV64 中非常重要的规则：`LW` 会把 bit 31 符号扩展到 bit 63。即使程序员只是想“读取四个字节”，也必须考虑这种符号扩展行为。

### 3.6 `LWU`：加载无符号 32 位字

```asm
lwu t0, 4(s0)
```

结果为：

```text
t0 = 0x0000000080000001
```

`LWU` 是 RV64 特有的指令，因为 RV32 的目标寄存器本身只有 32 位，不存在将 32 位数据零扩展到 64 位的需求。

### 3.7 `LD`：加载 64 位双字

假设地址 `s0 + 8` 中保存着 `0x1122334455667788`：

```asm
ld t0, 8(s0)
```

结果为：

```text
t0 = 0x1122334455667788
```

`LD` 不需要扩展，因为读取宽度和 RV64 整数寄存器宽度都是 64 位。`LD` 也是 RV64 指令，在 RV32I 中不存在。

## 4. RV64I 整数存储指令汇总

| 指令 | 写入宽度 | 使用源寄存器中的哪些位 |
|---|---:|---|
| `SB` | 8 位 | 最低 8 位 |
| `SH` | 16 位 | 最低 16 位 |
| `SW` | 32 位 | 最低 32 位 |
| `SD` | 64 位 | 全部 64 位 |

假设：

```text
s1 = 0x1122334455667788
```

下面的内存字节排列按当前常见的 RISC-V Linux 小端环境说明。

### 4.1 `SB`：存储低 8 位

```asm
sb s1, 0(s0)
```

写入一个字节：

```text
88
```

### 4.2 `SH`：存储低 16 位

```asm
sh s1, 2(s0)
```

写入的两个字节为：

```text
88 77
```

### 4.3 `SW`：存储低 32 位

```asm
sw s1, 4(s0)
```

写入的四个字节为：

```text
88 77 66 55
```

### 4.4 `SD`：存储完整 64 位

```asm
sd s1, 8(s0)
```

写入的八个字节为：

```text
88 77 66 55 44 33 22 11
```

存储指令没有“有符号”和“无符号”之分。它只截取源寄存器的低若干位，并将这些位写入内存。

## 5. 包含所有整数访存指令的完整 Demo

将下面内容保存为 `memory_demo.S`：

先区分两类汇编期写法：以点开头的 `.option`、`.section`、`.balign`、`.byte`、`.half`、`.word`、`.quad`、`.zero`、`.globl` 是**汇编伪操作**（directive），用于设置汇编选项、切换节、定义符号或生成数据；`la`、`li`、`nop` 是**伪指令**，由汇编器展开为真实机器指令。本例没有使用 `.macro`/`.endm` 定义的用户宏。标签和 `#` 后的注释本身也不会生成指令。[RISC-V 汇编手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)、[GNU as 伪操作手册](https://sourceware.org/binutils/docs/as/Pseudo-Ops.html)

```asm
    .option nopic         # 后续 la 按非 PIC 模式展开；本例引用局部符号时不经 GOT 取地址
    .option norelax      # 后续序列不标记为可链接器放宽；未初始化 gp 时避免 la 被改写为 gp 相对寻址

    .section .data       # 切换到可写数据节；后续数据伪操作在这里放置字节
    .balign 8           # 将当前位置补齐到 8 字节边界，使 src 满足 64 位访问的自然对齐

src:                    # 定义符号 src，值为当前位置的地址；标签本身不占空间
    .byte  0x80, 0x00  # 依次放入两个 1 字节常量，位于 src+0、src+1
    .half  0x8001       # 放入 2 字节常量，位于 src+2；不是 CPU 的 LH 指令
    .word  0x80000001   # 放入 4 字节常量，位于 src+4；RV64 的 .word 仍是 32 位
    .quad  0x1122334455667788  # 放入 8 字节常量，位于 src+8

dst:                    # src 共占 16 字节，因此 dst 也保持 8 字节对齐
    .zero 16            # 在 .data 中放入 16 个值为 0 的字节，作为本例的可写目标区

    .section .text       # 切换到代码节；后面的指令在这里编码
    .globl _start       # 将 _start 声明为全局符号；链接命令用 -e _start 指定入口

_start:                 # 程序入口标签，不是会执行的指令
    la s0, src          # la 是伪指令：把 src 的地址放入 s0，不读取 src 内存中的数据

    # -------------------------
    # 加载指令
    # -------------------------

    lb  t0, 0(s0)
    # t0 = 0xffffffffffffff80

    lbu t1, 0(s0)
    # t1 = 0x0000000000000080

    lh  t2, 2(s0)
    # t2 = 0xffffffffffff8001

    lhu t3, 2(s0)
    # t3 = 0x0000000000008001

    lw  t4, 4(s0)
    # t4 = 0xffffffff80000001

    lwu t5, 4(s0)
    # t5 = 0x0000000080000001

    ld  t6, 8(s0)
    # t6 = 0x1122334455667788

after_loads:
    nop                 # 给 GDB 断点一个独立地址；非功能必需，RV64I 下可展开为 addi x0,x0,0

    # 验证加载结果；失败时返回非零状态
    li s2, -128         # li 是装入立即数的伪指令，不是从内存加载；其他 li 同理
    bne t0, s2, failed
    li s2, 0x80
    bne t1, s2, failed
    li s2, -32767
    bne t2, s2, failed
    li s2, 0x8001
    bne t3, s2, failed
    li s2, -2147483647
    bne t4, s2, failed
    li s2, 0x80000001
    bne t5, s2, failed
    li s2, 0x1122334455667788
    bne t6, s2, failed

    # -------------------------
    # 存储指令
    # -------------------------

    la s0, dst
    li s1, 0x1122334455667788

    sb s1, 0(s0)
    # dst[0] = 0x88

    sh s1, 2(s0)
    # dst[2..3] = 88 77

    sw s1, 4(s0)
    # dst[4..7] = 88 77 66 55

    sd s1, 8(s0)
    # dst[8..15] = 88 77 66 55 44 33 22 11

after_stores:
    nop                 # 同理，断在这里可先检查内存，再执行后面的结果校验

    # 一次读取前 8 字节，检查 SB/SH/SW 的结果及未改动的 dst[1]
    ld t0, 0(s0)
    li t1, 0x5566778877880088
    bne t0, t1, failed

    # 检查 SD 写入的后 8 字节
    ld t0, 8(s0)
    bne t0, s1, failed

    # Linux exit(0)
    li a0, 0
    li a7, 93
    ecall

failed:
    li a0, 1
    li a7, 93
    ecall # 这里的ecall(Environment Call)指令是risc-v当中用于从当前优先级trap到更高优先级的指令，有点类似于Arm当中的svc/hvc/smc指令
          # 例如我们在risc-v的u-mode当中调用ecall指令，可以从u-mode trap到s-mode, 
          # 然后risc-v当中是没有eret指令的，但是有mret和sret，例如s-mode可以通过执行 sret 来从s-mode返回到 u-mode当中
```

`after_loads`、`after_stores` 后的 `nop` 只是调试用占位指令，让断点停在一个单独的指令地址。**删掉 `nop` 也能执行 `break after_loads`**：标签会直接指向后面的 `li`；`after_stores` 则会指向后面的 `ld`。程序结果不变，`nop` 也不承担内存屏障或延迟槽的作用。

### 5.1 汇编和链接

```bash
riscv64-linux-gnu-as \
    -march=rv64i \
    -mabi=lp64 \
    -g \
    -o memory_demo.o memory_demo.S

riscv64-linux-gnu-ld \
    -m elf64lriscv \
    -static \
    -e _start \
    -o memory_demo memory_demo.o
```

运行：

```bash
qemu-riscv64-static ./memory_demo
echo $?
```

加载或存储结果与预期相符时，退出状态为：

```text
0
```

任一检查失败则退出状态为 `1`。

本 Demo 直接以 `_start` 为入口，没有 C 运行库初始化 `gp`。`.option nopic` 控制后续 `la` 的汇编展开方式，**不表示必须使用绝对地址**：访问这里的局部符号时仍可生成 PC 相对寻址。`.option norelax` 阻止后续相关序列带上链接器放宽标记，避免链接器把某些 `la` 改写为依赖未初始化 `gp` 的寻址形式；否则程序可能能汇编、能运行，却读写错误地址。两个选项从出现处持续生效到再次修改或文件结束；若只想保护一小段代码，可用 `.option push` / `.option norelax` / `.option pop` 保存并恢复原有选项。[RISC-V `.option` 说明](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)

这里的 `la`、`li` 和 `nop` 是汇编器伪指令，不是新的硬件访存指令。可以查看它们展开后的真实指令：

```bash
riscv64-linux-gnu-objdump -dr memory_demo
```

如果希望完全避免伪指令显示，可以让 `objdump` 以非别名形式反汇编：

```bash
riscv64-linux-gnu-objdump -d -M no-aliases,numeric memory_demo
```

## 6. 用 GDB 观察寄存器和内存

在第一个终端启动 QEMU 的 GDB Server：

```bash
qemu-riscv64-static -g 1234 ./memory_demo
```

程序会在执行第一条指令之前暂停，等待调试器连接。

在第二个终端启动 GDB：

```bash
gdb-multiarch ./memory_demo
```

在 GDB 中执行：

```gdb
target remote :1234
break after_loads
break after_stores
continue
info registers t0 t1 t2 t3 t4 t5 t6
continue
x/16bx &dst
```

在 `after_loads` 断点处，各临时寄存器的预期值为：

```text
t0 = 0xffffffffffffff80
t1 = 0x0000000000000080
t2 = 0xffffffffffff8001
t3 = 0x0000000000008001
t4 = 0xffffffff80000001
t5 = 0x0000000080000001
t6 = 0x1122334455667788
```

在 `after_stores` 断点处，`dst` 的 16 个字节应为：

```text
88 00 88 77 88 77 66 55 88 77 66 55 44 33 22 11
```

其中 `dst[1]` 仍然是初始化时的 `0x00`，因为所有存储指令都没有访问这个字节。

## 7. 栈访问中的常见用法

函数通常使用 `SD` 保存需要保护的寄存器，返回之前再使用 `LD` 恢复：

```asm
my_function:
    addi sp, sp, -16

    sd ra, 8(sp)       # 保存返回地址
    sd s0, 0(sp)       # 保存 s0

    # 函数主体

    ld s0, 0(sp)       # 恢复 s0
    ld ra, 8(sp)       # 恢复返回地址

    addi sp, sp, 16
    ret
```

这也是 `LD` 和 `SD` 最常见的实际使用场景之一。按照 RISC-V 调用约定，函数还需要根据自己修改了哪些被调用者保存寄存器来决定具体保存哪些寄存器。

## 8. 对齐要求

建议所有访存都采用自然对齐地址：

```text
LB/LBU/SB：任意地址
LH/LHU/SH：地址是 2 的倍数
LW/LWU/SW：地址是 4 的倍数
LD/SD：地址是 8 的倍数
```

例如：

```asm
ld t0, 8(s0)          # 如果 s0 是 8 字节对齐，则地址仍然对齐
ld t0, 3(s0)          # 很可能形成非对齐的 64 位访问
```

非对齐访问是否能够完成，由具体处理器和执行环境决定。它可能由硬件完成、由软件模拟、明显变慢，或者直接触发异常。因此，可移植代码不应该依赖非对齐访问一定成功。

## 9. 初学时最容易踩的坑

### 9.1 混淆 `LW` 和 `LWU`

`LW` 会符号扩展，`LWU` 才会零扩展：

```asm
lw  t0, 0(s0)         # bit 31 为 1 时，高 32 位填 1
lwu t1, 0(s0)         # 高 32 位始终填 0
```

### 9.2 误以为存储也区分有符号和无符号

`SB`、`SH`、`SW` 和 `SD` 只选择源寄存器的低位，不关心源数据的有符号解释。

### 9.3 把偏移量当成元素下标

偏移量单位始终是字节：

```asm
ld t0, 8(s0)          # 地址增加 8 字节，而不是增加 8 个元素
```

访问 64 位数组的第 `i` 个元素时，需要使用 `base + i * 8`。但普通 load/store 的地址模式没有缩放下标，需要先使用移位或加法算出地址。

### 9.4 混淆 `la` 和 `ld`

```asm
la t0, value          # t0 = &value，得到 value 的地址
ld t1, 0(t0)          # t1 = value，读取地址中的数据
```

`la` 是汇编器伪指令；`ld` 才是真正的内存加载指令。

### 9.5 认为 `x0` 可以取消内存访问

即使加载指令的目标寄存器是 `x0`，内存访问仍然必须发生：

```asm
lw x0, 0(a0)
```

加载到的数值会被丢弃，但访问仍可能产生页错误、访问错误或设备寄存器副作用，所以不能把它理解成无条件的空操作。

### 9.6 认为“RV64”意味着每次必须访问 64 位

RV64 表示整数寄存器宽度为 64 位，并不要求每次内存访问都是 64 位。`LB/LH/LW` 等窄访问在字符串、结构体、网络协议和设备驱动中都很常见。

## 10. 本篇小结

这一组指令可以压缩成一句话：

> `LB/LH/LW` 做符号扩展，带 `U` 的版本做零扩展，`LD` 读取完整 64 位；`SB/SH/SW/SD` 分别写入源寄存器的低 8、16、32、64 位。

进一步需要记住三点：

1. RISC-V 是 Load/Store 架构，计算通常只能发生在寄存器之间。
2. 地址形式为 `offset(base)`，偏移是范围为 `-2048～2047` 的字节偏移。
3. 可移植程序应使用自然对齐地址，并明确区分符号扩展和零扩展。

下一组适合继续学习整数算术指令，包括 `ADD/SUB/ADDI`、RV64 特有的 `ADDW/SUBW/ADDIW`，以及它们的 64 位与 32 位结果规则。
