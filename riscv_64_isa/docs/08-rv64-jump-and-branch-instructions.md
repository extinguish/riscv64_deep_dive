# RISC-V 64 位 ISA：Jump and Branch Instructions

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准；“真实指令”表示有机器编码，但可能属于可选扩展或特权架构，并不表示仅用 `-march=rv64i` 就能汇编、执行。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `ADDI` | **Add Immediate** | 真实指令 |
| `AND` | **AND** | 真实指令 |
| `AUIPC` | **Add Upper Immediate to PC** | 真实指令 |
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
| `CALL` | **Call** | 伪指令 |
| `ECALL` | **Environment Call** | 真实指令 |
| `J` | **Jump** | 伪指令 |
| `JAL` | **Jump And Link** | 真实指令 |
| `JALR` | **Jump And Link Register** | 真实指令 |
| `JR` | **Jump Register** | 伪指令 |
| `LA` | **Load Address** | 伪指令 |
| `LD` | **Load Doubleword** | 真实指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `RET` | **Return** | 伪指令 |
| `SD` | **Store Doubleword** | 真实指令 |
| `TAIL` | **Tail Call** | 伪指令 |

ABI 别名是同一套 `x0～x31` 整数寄存器的另一种写法，并非额外寄存器；下面给出本文提及的整数寄存器编号与对应 ABI 别名。`PC` 若在本文出现，指 **Program Counter**（程序计数器），它不属于 `x0～x31`。

| ABI 别名 | 硬件编号 | 英文完整名称 |
|---|---|---|
| `zero` | `x0` | **Zero** |
| `ra` | `x1` | **Return Address** |
| `sp` | `x2` | **Stack Pointer** |
| `t0` | `x5` | **Temporary register 0** |
| `t1` | `x6` | **Temporary register 1** |
| `t2` | `x7` | **Temporary register 2** |
| `a0` | `x10` | **Argument register 0** |
| `a1` | `x11` | **Argument register 1** |
| `a2` | `x12` | **Argument register 2** |
| `a7` | `x17` | **Argument register 7** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

本文继续讲解 RV64I 基础整数指令集中的控制流指令：无条件跳转、条件跳转、函数调用与返回。文末提供可以直接汇编、运行、反汇编和调试的完整示例。

本篇涉及的真实指令有：

```text
无条件跳转：JAL  JALR
条件跳转：  BEQ  BNE  BLT  BGE  BLTU  BGEU
```

前面的 [PC 相对寻址](03-rv64-pc-relative-instructions.md) 一章解释过目标地址和重定位；[比较指令](07-rv64-comparison-instructions.md) 一章解释过有符号与无符号比较。本篇重点放在这些指令如何改变执行路径，以及 `j/call/ret` 等伪指令如何用于实际程序。

## 1. 顺序执行与控制流

本篇示例只使用 `-march=rv64i`。基础 RV64I 指令长度为 4 字节，因此通常按下面的顺序执行：

```text
当前指令位于 PC
下一条指令位于 PC + 4
```

跳转指令会改变下一条要执行的地址：

```text
JAL：   目标 = 当前 JAL 的 PC + 有符号偏移
JALR：  目标 = (rs1 + 有符号立即数) & ~1
分支：  条件成立时，目标 = 当前分支的 PC + 有符号偏移
       条件不成立时，继续执行 PC + 4
```

RISC-V 的这些控制流指令没有架构可见的延迟槽。跳转一旦成立，排在跳转指令后面的源码指令不会因为“延迟槽”而必然执行。

## 2. 指令总览

| 指令 | 类型 | 目标来源 | 返回地址 |
|---|---|---|---|
| `JAL rd, label` | J-type | `PC + 偏移` | 将 `PC + 4` 写入 `rd` |
| `JALR rd, imm12(rs1)` | I-type | `(rs1 + imm12) & ~1` | 将 `PC + 4` 写入 `rd` |
| `BEQ/BNE` | B-type | 条件成立时 `PC + 偏移` | 不保存 |
| `BLT/BGE` | B-type | 有符号比较成立时 `PC + 偏移` | 不保存 |
| `BLTU/BGEU` | B-type | 无符号比较成立时 `PC + 偏移` | 不保存 |

这里说 `JAL`、`JALR` 是真实指令，指的是表中的完整操作数形式。GNU 汇编器也接受省略操作数的写法：`jal label` 是 `jal ra, label` 的伪指令形式；`jalr rs` 是 `jalr ra, 0(rs)` 的伪指令形式。判断某一行源码是否为伪指令，要同时看助记符和操作数写法，不能只看 `JAL` 或 `JALR` 这几个字母。[RISC-V 汇编手册伪指令表](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)

`rd = x0` 时，跳转仍然发生，但返回地址被丢弃。因此同一条 `JAL` 既能实现函数调用，也能实现普通无条件跳转。

## 3. `JAL`：PC 相对无条件跳转

语法：

```asm
jal rd, label
```

假设 `JAL` 自身位于 `0x1000`，目标标签位于 `0x1020`，在本篇 RV64I 环境下：

```text
rd = 0x1004
下一条执行的指令位于 0x1020
```

`JAL` 的偏移相对于 **当前 `JAL` 的 PC**，不是相对于 `PC + 4`。指令编码的跳转偏移是 2 字节的整数倍，范围为 `-1048576～1048574` 字节，约 ±1 MiB。目标标签必须处于该编码范围内。

### 3.1 保存返回地址：直接调用

按常用调用约定，`ra` 是 `x1`，保存返回地址：

```asm
li     a0, 40
jal    ra, add_three
after_call:
    # 被调用函数返回后，a0 = 43
    j done

add_three:
    addi a0, a0, 3
    ret

done:
    # 继续执行调用者的后续代码
```

`JAL` 把 `after_call` 的地址写入 `ra`。`ret` 再利用 `ra` 跳回调用处的下一条指令。`JAL` 不会自动保存参数寄存器或栈内容，函数需要遵守相应的调用约定。

### 3.2 不保存返回地址：普通跳转

```asm
jal x0, next_step
    # 此处不会在跳转后顺序执行
next_step:
```

源码中常写为伪指令：

```asm
j next_step
```

`j label` 展开为 `jal x0, label`。需要无条件跳转时，应使用这一形式，而不是把永远成立的 `beq x0, x0, label` 当作普通跳转。

## 4. `JALR`：寄存器间接跳转

语法：

```asm
jalr rd, imm12(rs1)
```

目标计算分两步：

```text
临时地址 = rs1 + sign_extend(imm12)
目标地址 = 临时地址 & 0xfffffffffffffffe
rd = PC + 4
```

立即数范围是 `-2048～2047` **字节**，不会像 `JAL` 和条件分支偏移那样隐含乘以 2。目标计算最后会清除 bit 0。

例如，函数地址已经在 `t0` 中：

```asm
jalr ra, 0(t0)
```

这会跳到 `t0` 指向的代码，并把返回地址写入 `ra`。如果不需要返回地址，可写 `jalr x0, 0(t0)`，或使用 `jr t0` 伪指令。

### 4.1 bit 0 清零的作用

假设一个 4 字节对齐的函数入口位于 `0x1020`，而 `t0 = 0x1021`：

```asm
jalr ra, 0(t0)
```

计算出的目标仍是 `0x1020`。bit 0 可用于在函数指针中携带额外信息；执行 `JALR` 时会清掉它。但这**不能**修复所有不对齐地址：在只支持基础 RV64I 的机器上，指令入口必须 4 字节对齐，目标 bit 1 若仍为 1，会产生指令地址不对齐异常。

### 4.2 与 `JAL` 的区别

`JAL` 的目标直接从指令 PC 和编码偏移得到；`JALR` 从寄存器与 12 位立即数计算目标。函数指针、返回地址以及 `AUIPC + JALR` 形式的远调用都依赖 `JALR`。

## 5. `ret`、`jr` 与 `call` 伪指令

| 源码写法 | 基础指令或典型序列 | 用途 |
|---|---|---|
| `j label` | `jal x0, label` | PC 相对跳转，不保存返回地址 |
| `jr rs` | `jalr x0, 0(rs)` | 寄存器间接跳转，不保存返回地址 |
| `ret` | `jalr x0, 0(ra)` | 从函数返回 |
| `call function` | 通常为 `auipc ra, ...` + `jalr ra, ...(ra)` | 调用可在较远位置的函数 |
| `tail function` | 通常为 `auipc 临时寄存器, ...` + `jalr x0, ...(临时寄存器)` | 尾调用，不建立新的返回地址 |

`call` 和 `tail` 是汇编器伪指令；实际序列受符号距离及链接器 relaxation 影响。附近的 `call` 可能在链接后缩短为单条 `JAL`。如果要观察未缩短的 `AUIPC + JALR`，可在链接命令中加入 `--no-relax`。

例如：

```asm
call worker
```

这表示调用 `worker` 并在它返回后继续执行。对于尾调用：

```asm
tail worker
```

当前函数不再返回到 `tail` 后方，而由 `worker` 直接返回到原调用者。尾调用会使用临时寄存器形成目标地址；不能假设该临时寄存器执行后仍保留原值。

## 6. 为什么嵌套调用要保存 `ra`

每次把返回地址写入 `ra` 的调用都会覆盖旧值：

```asm
outer:
    call inner             # 覆盖 ra
    ret                    # 如果没有保存原 ra，就无法返回 outer 的调用者
```

调用其他函数且随后还要返回的函数，应在调用前保存自己的返回地址。下面使用栈上的 16 字节空间，并维持栈指针的 16 字节对齐：

```asm
outer:
    addi sp, sp, -16
    sd   ra, 8(sp)

    call inner

    ld   ra, 8(sp)
    addi sp, sp, 16
    ret
```

`inner` 返回到 `outer` 时，`ra` 仍是 `outer` 内部的返回点；只有重新加载保存的 `ra`，`outer` 才能返回原调用者。若函数还修改了调用约定要求保留的其他寄存器，也需要按约定保存和恢复。

## 7. 六条真实条件跳转指令

条件分支读取两个完整的 64 位寄存器值，条件成立时跳到目标标签；条件不成立时执行下一条指令。

| 指令 | 语法 | 跳转条件 |
|---|---|---|
| `BEQ` | `beq rs1, rs2, label` | 位模式相等 |
| `BNE` | `bne rs1, rs2, label` | 位模式不相等 |
| `BLT` | `blt rs1, rs2, label` | 有符号 `rs1 < rs2` |
| `BGE` | `bge rs1, rs2, label` | 有符号 `rs1 >= rs2` |
| `BLTU` | `bltu rs1, rs2, label` | 无符号 `rs1 < rs2` |
| `BGEU` | `bgeu rs1, rs2, label` | 无符号 `rs1 >= rs2` |

分支的编码偏移相对 **当前分支指令的 PC**，是 2 字节的整数倍，范围为 `-4096～4094` 字节。汇编源码通常写标签，由汇编器与链接器计算偏移。

### 7.1 相等和不等

```asm
beq a0, a1, equal
bne a0, a1, different
```

`BEQ/BNE` 只检查 64 位位模式，无须区分有符号和无符号。

### 7.2 有符号和无符号大小关系

```asm
li  t0, -1
li  t1, 1
blt t0, t1, signed_less     # -1 < 1：跳转
bltu t0, t1, unsigned_less  # 2^64-1 < 1：不跳转
```

`BGE` 与 `BLT` 互为相反条件；`BGEU` 与 `BLTU` 互为相反条件。符号解释一旦选错，程序就可能进入错误路径。

## 8. 条件分支的常见伪指令

| 伪指令 | 基础指令 | 条件 |
|---|---|---|
| `beqz rs, label` | `beq rs, x0, label` | 等于零 |
| `bnez rs, label` | `bne rs, x0, label` | 不等于零 |
| `bltz rs, label` | `blt rs, x0, label` | 有符号小于零 |
| `bgez rs, label` | `bge rs, x0, label` | 有符号大于等于零 |
| `bgt rs1, rs2, label` | `blt rs2, rs1, label` | 有符号大于 |
| `ble rs1, rs2, label` | `bge rs2, rs1, label` | 有符号小于等于 |
| `bgtu rs1, rs2, label` | `bltu rs2, rs1, label` | 无符号大于 |
| `bleu rs1, rs2, label` | `bgeu rs2, rs1, label` | 无符号小于等于 |

例如：

```asm
li  t0, 5
li  t1, 3
bgt t0, t1, greater         # 实际是 blt t1, t0, greater
```

RV64I 没有独立编码的 `BGT/BLE/BGTU/BLEU`。它们通过交换操作数使用已有的分支指令。

## 9. 用分支实现 `if` 和循环

### 9.1 `if / else`

假设 `a0`、`a1` 为有符号 64 位数：

```asm
blt a0, a1, less_case

    # a0 >= a1 时执行
    li a2, 0
    j  after_if

less_case:
    # a0 < a1 时执行
    li a2, 1

after_if:
```

不成立的分支会顺序执行 `li a2, 0`；成立的分支会直接到 `less_case`。后面的 `j` 防止第一条路径继续落入第二条路径。

### 9.2 倒计数循环

```asm
li t0, 3
loop:
    addi t0, t0, -1
    bne  t0, x0, loop
```

`t0` 依次变为 `2、1、0`。前两次 `BNE` 跳回 `loop`，最后一次条件不成立，程序继续执行循环后的指令。

## 10. 偏移范围、对齐与远距离跳转

| 形式 | 基础编码偏移 | 用途 |
|---|---:|---|
| 条件分支 | `-4096～4094` 字节 | 近距离有条件跳转 |
| `JAL` | `-1048576～1048574` 字节 | 近距离无条件跳转或调用 |
| `JALR` | 12 位有符号字节偏移，加寄存器 | 寄存器间接跳转 |

条件分支和 `JAL` 的编码偏移 bit 0 恒为零，但基础 RV64I 指令入口仍要求 4 字节对齐。如果真正跳到只有 2 字节对齐的目标，会触发指令地址不对齐异常；未成立的条件分支不会因为其目标不对齐而触发该异常。

目标太远时，单条条件分支或 `JAL` 无法直接编码。汇编器或链接器可能把某些远分支改写为多指令序列，具体取决于工具链、目标位置与重定位；不能把这种工具行为当作单条基础指令的范围。显式的远条件跳转可把条件取反，然后跳过一段更远的无条件跳转：

```asm
bne a0, a1, nearby_continue
    j far_target
nearby_continue:
```

这个例子中的 `j` 仍受 `JAL` 范围限制。目标再远时，可以使用 `AUIPC + JALR` 或由工具链生成合适的跳转序列。高低位重定位见 [PC 相对寻址](03-rv64-pc-relative-instructions.md)，准确边界和可运行的越界实验见[寻址范围](10-rv64-addressing-ranges.md)。

## 11. 完整 Jump and Branch Instructions Demo

程序验证：

- `JAL` 直接调用及 `JAL x0` 无条件跳转。
- `JALR` 间接调用及目标 bit 0 清零。
- `call`、`ret`、嵌套调用时保存 `ra`。
- 六条真实条件分支，以及一个向后跳转的循环。

保存为 `jump_branch_demo.S`：

下面代码中，以点开头的名称是汇编伪操作，用来设置选项、布置节/符号或生成数据；它们不是 RV64 CPU 指令。`la`、`li` 等没有点号的写法则可能是由汇编器展开的伪指令（见第 0 节）。本例没有使用 `.macro`/`.endm` 定义用户宏。[RISC-V 汇编手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)、[GNU as 伪操作手册](https://sourceware.org/binutils/docs/as/Pseudo-Ops.html)

```asm
    .option nopic  # 后续 la 使用非 PIC 形式；局部符号可用 PC 相对寻址，不经 GOT，也不强制绝对地址

    .section .rodata  # 切换到只读数据节，后续整数或字符串常量放在这里
pass_message:
    .ascii "Jump and branch instruction demo passed!\n"  # 按原样写入字符串字节；\n 是换行，不会自动追加 NUL 结束字节
    .equ pass_message_len, . - pass_message  # 汇编期定义长度常量：点号是当前位置，减去字符串起始标签得到字节数；不分配存储

fail_message:
    .ascii "Jump and branch instruction demo failed!\n"  # 按原样写入字符串字节；\n 是换行，不会自动追加 NUL 结束字节
    .equ fail_message_len, . - fail_message  # 汇编期定义长度常量：点号是当前位置，减去字符串起始标签得到字节数；不分配存储

    .section .text  # 切换到可执行代码节，后续指令在这里编码
    .globl _start  # 将 _start 标成 ELF 全局符号；入口地址仍由链接器的 -e 或 ENTRY 指定

_start:
    # 1. JAL 直接调用；ra 指向 JAL 后的一条指令
    li  a0, 40  # 伪指令：把常量构造到寄存器，大常量可能需要多条真实指令
    jal ra, add_three
after_direct_call:
    li  t1, 43
    bne a0, t1, failed

    # 2. JAL x0：跳过不能执行的失败路径
    jal x0, after_plain_jump
    j failed
after_plain_jump:

    # 3. JALR 间接调用；故意把目标 bit 0 置为 1
    la   t0, indirect_target  # 伪指令：构造符号地址，不读取该地址的内容；可能展开为 AUIPC+ADDI 等
    addi t0, t0, 1
    jalr ra, 0(t0)
after_indirect_call:
    li  t1, 44
    bne a0, t1, failed

    # 4. 嵌套调用：外层函数需要保存 ra
    call nested_plus_two  # 伪指令：保存返回地址并跳转，最终可能使用 JAL 或 AUIPC+JALR
after_nested_call:
    li  t1, 46
    bne a0, t1, failed

    # 5. 六条条件分支的成立路径
    li t0, -1
    li t1, 1
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
    # 检查不成立的条件应顺序执行
    bge  t0, t1, failed
    bltu t0, t1, failed

    # 6. 向后分支形成循环
    li t2, 3
countdown:
    addi t2, t2, -1
    bne  t2, x0, countdown
after_loop:
    bne t2, x0, failed

    li a0, 1
    la a1, pass_message
    li a2, pass_message_len
    li a7, 64                  # write(1, message, length)
    ecall

    li a0, 0
    li a7, 93                  # exit(0)
    ecall

failed:
    li a0, 1
    la a1, fail_message
    li a2, fail_message_len
    li a7, 64
    ecall

    li a0, 1
    li a7, 93                  # exit(1)
    ecall

    .p2align 2  # 按 2^2=4 字节对齐；参数是指数，与 .balign 的字节数不同
add_three:
    addi a0, a0, 3
    ret  # 伪指令：按 ra 中的返回地址跳转，通常展开为 JALR x0,0(ra)

    .p2align 2  # 按 2^2=4 字节对齐；参数是指数，与 .balign 的字节数不同
indirect_target:
    addi a0, a0, 1
    ret

    .p2align 2  # 按 2^2=4 字节对齐；参数是指数，与 .balign 的字节数不同
nested_plus_two:
    addi sp, sp, -16
    sd   ra, 8(sp)
    call helper_plus_one
    call helper_plus_one
    ld   ra, 8(sp)
    addi sp, sp, 16
    ret

helper_plus_one:
    addi a0, a0, 1
    ret
```

## 12. 汇编、链接和运行

```bash
riscv64-linux-gnu-as \
    -march=rv64i \
    -mabi=lp64 \
    -g \
    -o jump_branch_demo.o \
    jump_branch_demo.S

riscv64-linux-gnu-ld \
    -m elf64lriscv \
    -static \
    --no-relax \
    -e _start \
    -o jump_branch_demo \
    jump_branch_demo.o
```

这里使用 `--no-relax`，方便在最终反汇编中观察 `call` 的 `AUIPC + JALR` 序列。程序功能本身不依赖禁用 relaxation。

运行并查看退出状态：

```bash
qemu-riscv64-static ./jump_branch_demo
echo $?
```

预期：

```text
Jump and branch instruction demo passed!
0
```

任一检查失败时，程序输出 `Jump and branch instruction demo failed!`，并以状态 1 退出。

## 13. 反汇编观察真实指令

查看源码、重定位与反汇编：

```bash
riscv64-linux-gnu-objdump -drS jump_branch_demo.o
riscv64-linux-gnu-objdump -drS jump_branch_demo
```

关闭伪指令别名：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    jump_branch_demo
```

重点寻找：

1. `jal ra, add_three` 对应 `jal x1, ...`，把返回地址写入 `x1`。
2. `jal x0, after_plain_jump` 仍跳转，但不保存返回地址。
3. `ret` 对应 `jalr x0, 0(x1)`。
4. `call` 在本篇 `--no-relax` 构建中对应 `auipc x1, ...` 加 `jalr x1, ...(x1)`。
5. `BEQ/BNE/BLT/BGE/BLTU/BGEU` 都是单条真实条件分支。

`objdump` 输出采用数字寄存器名时，`x1` 是 `ra`，`x2` 是 `sp`，`x5` 是 `t0`。

## 14. 使用 GDB 观察控制流

终端一：

```bash
qemu-riscv64-static -g 1234 ./jump_branch_demo
```

终端二：

```bash
gdb-multiarch ./jump_branch_demo
```

连接并设置断点：

```gdb
target remote :1234
break add_three
break after_direct_call
break indirect_target
break after_indirect_call
break nested_plus_two
break after_nested_call
break after_loop
continue
```

在 `add_three` 断点，查看 `ra` 与调用点之后的地址：

```gdb
p/x $ra
p/x &after_direct_call
```

二者应相等。继续到 `after_direct_call`，`a0` 应为 `43`。在 `indirect_target` 断点，虽然 `t0` 的 bit 0 被设置为 1，`pc` 仍落在对齐的函数入口：

```gdb
p/x $t0
p/x $pc
```

二者相差 1。继续到 `after_indirect_call`，`a0` 应为 `44`；继续进入 `nested_plus_two`，可以单步观察栈上保存和恢复 `ra`；到达 `after_nested_call` 时，`a0` 应为 `46`。最后到 `after_loop`，`t2` 应为零。

```gdb
display/i $pc
si
info registers a0 t2 ra sp
```

## 15. 常见错误

### 15.1 把 `JAL` 的目标基准误认为 `PC + 4`

`JAL` 和条件分支的目标偏移都以自身指令的 PC 为基准；`PC + 4` 是本篇 RV64I 环境下写入链接寄存器的返回地址，或条件不成立时顺序执行的地址。

### 15.2 把 `JALR` 的立即数按 2 字节缩放

`JALR` 的 12 位立即数以字节为单位。只有目标计算完成后才清除 bit 0。

### 15.3 跳转时误覆盖 `ra`

普通无条件跳转使用 `jal x0, label`。若写成 `jal ra, label`，旧的返回地址会被覆盖。

### 15.4 嵌套调用时没有保存 `ra`

调用中的 `JAL/JALR` 会覆盖 `ra`。还需要返回自己调用者的函数必须先保存它，并在返回前恢复。

### 15.5 把 `BLT` 和 `BLTU` 当成相同条件

负数按无符号数解释会处于 64 位数值范围的高端。涉及负数位模式时，两条指令很可能走不同路径。

### 15.6 假设分支后的指令总会执行

RV64I 没有架构可见的分支延迟槽。成立的分支不会顺序执行其后的指令。

### 15.7 混淆单条指令范围与工具链改写

条件分支只有约 ±4 KiB 的直接范围，`JAL` 约 ±1 MiB。远距离跳转需要其他指令组合；即使汇编器或链接器替你扩展，最终机器码也不是一条超范围的基础分支。

## 16. 本部分小结

```text
无条件：JAL、JALR
有条件：BEQ、BNE、BLT、BGE、BLTU、BGEU
```

核心规则是：

1. `JAL` 与条件分支使用当前指令的 PC 作为相对偏移基准。
2. `JALR` 从寄存器与有符号 12 位字节偏移生成目标，并清除 bit 0。
3. `JAL/JALR` 把返回地址写入 `rd`；写入 `x0` 时，返回地址被丢弃。
4. `BEQ/BNE` 比较位模式；`BLT/BGE` 使用有符号顺序；`BLTU/BGEU` 使用无符号顺序。
5. `j/ret/call/tail` 是汇编器提供的常见伪指令。
6. 嵌套调用要保存并恢复返回地址；控制流指令没有架构可见的延迟槽。

规范依据：[RISC-V 基础整数控制流指令](https://docs.riscv.org/reference/isa/v20240411/unpriv/rv32.html)、[RISC-V 汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。
