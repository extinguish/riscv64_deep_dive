# RISC-V 64 位 ISA：CSR Instructions

本文继续讲解 RISC-V 64 位的控制与状态寄存器（Control and Status Register，CSR）指令。与前几篇一样，文末提供可以直接汇编、运行、反汇编和调试的完整示例。

本篇涉及 `Zicsr` 扩展中的六条真实指令：

```text
寄存器形式：CSRRW   CSRRS   CSRRC
立即数形式：CSRRWI  CSRRSI  CSRRCI
```

同时讲解 `CSRR/CSRW/CSRS/CSRC` 等伪指令、零寄存器的特殊语义、CSR 权限，以及在 Linux 用户态能实际访问哪些 CSR。

## 1. CSR 是什么

普通整数寄存器 `x0～x31` 保存通用数据。CSR 是另一套独立的寄存器地址空间，常用于记录或控制处理器状态，例如浮点异常标志、计数器、异常处理状态与特权级配置。

每条 CSR 指令包含一个 **12 位 CSR 编号**，因此最多可编码 4096 个 CSR 地址。示例：

| CSR | 地址 | 用途 | 用户态访问 |
|---|---:|---|---|
| `fflags` | `0x001` | 浮点累计异常标志 | F 扩展可用时可读写 |
| `frm` | `0x002` | 浮点动态舍入模式 | F 扩展可用时可读写 |
| `fcsr` | `0x003` | 浮点控制与状态 | F 扩展可用时可读写 |
| `cycle` | `0xc00` | 周期计数 | Zicntr 可用且环境允许时只读 |
| `time` | `0xc01` | 时间计数 | Zicntr 可用且环境允许时只读 |
| `instret` | `0xc02` | 已完成指令计数 | Zicntr 可用且环境允许时只读 |

CSR 不是普通内存地址，不能用 `LD/SD` 访问。指令中的 CSR 编号也不能放在整数寄存器里动态选择；需要在汇编时确定。

`Zicsr` 指令只是访问机制。能否访问某个 CSR，还取决于该 CSR 是否实现、相关扩展是否可用，以及当前特权级或运行环境是否允许。本文的 Demo 在 Linux 用户态运行，不访问 `mstatus`、`mtvec` 等机器模式 CSR；这些寄存器和陷入返回流程见[特权级陷入与返回](14-rv64-privilege-trap-and-return.md)。

## 2. 为什么要指定 `Zicsr`

现代 RISC-V 规范把 CSR 指令作为独立的 `Zicsr` 扩展描述，不能因为使用 RV64I 就默认六条 CSR 指令一定可用。仅汇编 CSR 指令时，需要在目标架构字符串中加入 `zicsr`：

```bash
 -march=rv64i_zicsr
```

不过 `rv64i_zicsr` 本身并没有保证存在一个可在 Linux 用户态读写、又适合演示全部六条指令的 CSR。本篇使用 F 扩展定义的 `fflags`，因此完整 Demo 的汇编目标为：

```bash
 -march=rv64if_zicsr -mabi=lp64
```

F 扩展在这里提供 `fflags` CSR；Demo 不执行浮点算术。`lp64` 是本示例的整数调用约定，程序直接从 `_start` 运行，不依赖 C 库。

## 3. 六条指令总览

设 `old` 是执行指令前的 CSR 值，`mask` 是 `rs1` 的原值；立即数形式的 `uimm` 是 5 位无符号数。

| 指令 | 语法 | 写入 `rd` | 写入 CSR |
|---|---|---|---|
| `CSRRW` | `csrrw rd, csr, rs1` | `old` | `rs1` 的原值 |
| `CSRRS` | `csrrs rd, csr, rs1` | `old` | `old \| mask` |
| `CSRRC` | `csrrc rd, csr, rs1` | `old` | `old & ~mask` |
| `CSRRWI` | `csrrwi rd, csr, uimm` | `old` | `uimm` |
| `CSRRSI` | `csrrsi rd, csr, uimm` | `old` | `old \| uimm` |
| `CSRRCI` | `csrrci rd, csr, uimm` | `old` | `old & ~uimm` |

表中是普通情况下的值计算；`x0` 和零立即数会改变指令是否真正读取或写入 CSR，详见第 8 节。CSR 指令对单个 CSR 的读、改、写是原子操作，不需要先执行独立的读指令再写回；这不意味着它能代替普通内存上的原子指令。

如果 CSR 的实际值窄于 64 位，读出的旧值会零扩展后写入 `rd`。例如读取 5 位 `fflags` 时，目标整数寄存器的其余高位为零。

CSR 中只有可写的位才能被改动。对保留位或只读字段写入时的具体结果由对应 CSR 的定义决定，不能把表中的位运算当作所有 CSR 的无条件保证。

## 4. `CSRRW`：交换旧值与新值

语法：

```asm
csrrw rd, csr, rs1
```

它把 CSR 的旧值写入 `rd`，并把 `rs1` 的**原值**写入 CSR：

```asm
li    t0, 1
csrrw t1, fflags, t0
```

如果执行前 `fflags = 0`，执行后：

```text
t1     = 0    # 旧值
fflags = 1    # 新值
```

即使 `rd` 与 `rs1` 是同一个寄存器，CSR 写入的仍是该寄存器在指令执行前的值；不要把它理解为先覆盖整数寄存器再取新值。

如果只想写入而不需要旧值，可把 `rd` 指定为 `x0`：

```asm
csrrw x0, fflags, t0
```

这种形式不会读取 CSR。伪指令 `csrw fflags, t0` 对应的就是它。

## 5. `CSRRS`：按掩码置位

语法：

```asm
csrrs rd, csr, rs1
```

例如，执行前 `fflags = 0b00001`，而 `t0 = 0b00100`：

```asm
csrrs t1, fflags, t0
```

执行后：

```text
t1     = 0b00001            # 读到修改前的值
fflags = 0b00101            # 原值 OR 掩码
```

若 `rs1 = x0`，`CSRRS` 只读取 CSR，不写入：

```asm
csrrs t1, fflags, x0
```

它的常见伪指令是：

```asm
csrr t1, fflags
```

注意这里检查的是寄存器**编号**是否为 `x0`。如果写 `csrrs t1, fflags, t0`，即使运行时 `t0` 的值为零，指令仍属于会尝试写 CSR 的形式。

## 6. `CSRRC`：按掩码清位

语法：

```asm
csrrc rd, csr, rs1
```

例如，执行前 `fflags = 0b00101`，`t0 = 0b00001`：

```asm
csrrc t1, fflags, t0
```

执行后：

```text
t1     = 0b00101
fflags = 0b00100            # 原值 AND 掩码的按位取反
```

与 `CSRRS` 相同，`rs1 = x0` 表示只读，不写 CSR。

## 7. 三条 5 位立即数指令

立即数形式的数值范围是 **0～31**，并零扩展到整数寄存器宽度。这里的 `uimm` 是要写入的值或位掩码，不是 CSR 地址；CSR 地址仍是指令中的独立 12 位字段。

### 7.1 `CSRRWI`：立即数写入

```asm
csrrwi t0, fflags, 16
```

把 `fflags` 的旧值放入 `t0`，再把数值 16 写入 `fflags`。即使立即数写成 0，这也是一次写入；不能把 `csrrwi ..., 0` 当成只读操作。

### 7.2 `CSRRSI`：立即数掩码置位

```asm
csrrsi t0, fflags, 4
```

把旧值放入 `t0`，并把 bit 2 置为 1。若 `uimm = 0`，只读取 CSR，不写入。

### 7.3 `CSRRCI`：立即数掩码清位

```asm
csrrci t0, fflags, 2
```

把旧值放入 `t0`，并把 bit 1 清为 0。若 `uimm = 0`，只读取 CSR，不写入。

这三条指令只能直接处理由低五位立即数表达的值或掩码。需要更大的动态掩码时，先把掩码装入整数寄存器，再使用 `CSRRS/CSRRC`。

## 8. `x0` 与零立即数的特殊语义

这是 CSR 指令最容易混淆的规则。表中的“读”与“写”指对 CSR 的访问；是否保存旧值则还取决于 `rd` 是否为 `x0`。

| 指令形式 | 是否读 CSR | 是否写 CSR |
|---|---|---|
| `csrrw x0, csr, rs1` | 否 | 是，包括 `rs1 = x0` 时写零 |
| `csrrw rd, csr, rs1`，`rd ≠ x0` | 是 | 是 |
| `csrrs/csrrc rd, csr, x0` | 是 | 否 |
| `csrrs/csrrc rd, csr, 非 x0` | 是 | 是，即使该寄存器的值为零 |
| `csrrwi x0, csr, uimm` | 否 | 是，包括 `uimm = 0` 时写零 |
| `csrrwi rd, csr, uimm`，`rd ≠ x0` | 是 | 是 |
| `csrrsi/csrrci rd, csr, 0` | 是 | 否 |
| `csrrsi/csrrci rd, csr, 非零值` | 是 | 是 |

重要结论：

```asm
csrrs t0, cycle, x0          # 只读；若环境允许访问 cycle，则合法
csrrw x0, cycle, x0          # 尝试写零到只读 CSR；不合法
```

这里省略 `CSRRW` 的读取，并不绕过 CSR 的写权限检查。相反，`CSRRS/CSRRC` 使用 `x0` 作为掩码时不写 CSR，因此可以用来读取只读 CSR。

## 9. 常见 CSR 伪指令

| 伪指令 | 真实指令 | 作用 |
|---|---|---|
| `csrr rd, csr` | `csrrs rd, csr, x0` | 读取 CSR |
| `csrw csr, rs` | `csrrw x0, csr, rs` | 写入 CSR |
| `csrwi csr, uimm` | `csrrwi x0, csr, uimm` | 写入 5 位立即数 |
| `csrs csr, rs` | `csrrs x0, csr, rs` | 按掩码置位 |
| `csrc csr, rs` | `csrrc x0, csr, rs` | 按掩码清位 |
| `csrsi csr, uimm` | `csrrsi x0, csr, uimm` | 立即数掩码置位 |
| `csrci csr, uimm` | `csrrci x0, csr, uimm` | 立即数掩码清位 |

例如：

```asm
csrr t0, fflags              # 只读
csrs fflags, t1              # 置位 t1 指定的可写位
csrc fflags, t2              # 清除 t2 指定的可写位
```

把 `rd` 写成 `x0` 表示不保留旧值，但 `CSRRS/CSRRC` 仍会读取 CSR；它们不会因为丢弃读出的值就变成纯写指令。

## 10. CSR 地址、权限与 Linux 用户态

CSR 地址的高位按照规范约定编码访问属性：`csr[11:10] = 11` 表示只读地址范围；`csr[9:8]` 表示最低访问特权级。具体 CSR 仍由相应扩展和执行环境定义。

在 Linux 用户态程序中：

- `fflags/frm/fcsr` 属于 F 扩展的用户态 CSR；F 扩展及浮点状态可用时才能访问。
- `cycle/time/instret` 属于 Zicntr 的只读计数器；环境可以限制用户态读取，不能假定在所有 Linux、模拟器或硬件上都可用。
- `mstatus/mtvec/mepc` 等机器模式 CSR 不能由普通用户态程序直接读写。它们需要相应特权级的内核、固件或裸机环境。

当 Zicntr 与环境权限都允许时，可以读取计数器：

```asm
rdcycle   t0                  # csrrs t0, cycle, x0
rdtime    t1                  # csrrs t1, time, x0
rdinstret t2                  # csrrs t2, instret, x0
```

在 RV64 上，每次读取就得到完整的 64 位计数值。计数器的数值会随执行变化，不能把两个相邻读取结果当作固定常数。

访问未实现或不允许访问的 CSR 通常会触发非法指令异常；在 Linux 用户态常表现为进程收到 `SIGILL`。因此本篇可运行 Demo 选择 `fflags`，不尝试修改机器模式 CSR，也不依赖计数器是否开放。

## 11. `fflags` 的五个可写位

F 扩展的 `fflags` 是 `fcsr` 的低 5 位。位的名称如下：

| bit | 名称 | 含义 |
|---:|---|---|
| 4 | `NV` | 无效操作 |
| 3 | `DZ` | 除以零 |
| 2 | `OF` | 上溢 |
| 1 | `UF` | 下溢 |
| 0 | `NX` | 不精确 |

Demo 通过 CSR 指令直接写入这些位，目的是展示读、覆盖、置位和清位语义；它不执行会产生浮点异常的计算。程序在修改前保存原 `fflags`，结束前恢复原值。

`frm` 控制浮点舍入方式，随意写入保留编码可能影响后续浮点计算，所以本篇只修改 `fflags` 中定义明确的低五位。

## 12. 完整 CSR Instructions Demo

程序覆盖六条真实 `Zicsr` 指令，并检查每条指令写入目标整数寄存器的旧值和 CSR 的新值。

保存为 `csr_demo.S`：

```asm
    .option nopic

    .section .rodata
pass_message:
    .ascii "CSR instruction demo passed!\n"
    .equ pass_message_len, . - pass_message

fail_message:
    .ascii "CSR instruction demo failed!\n"
    .equ fail_message_len, . - fail_message

    .section .text
    .globl _start

_start:
    # 保存原始浮点异常标志，结束前恢复
    csrr s0, fflags
    csrrwi x0, fflags, 0       # 清零；rd=x0，不需要读取旧值

    # 1. 寄存器形式：CSRRW、CSRRS、CSRRC
    li    t0, 1
    csrrw s1, fflags, t0       # s1=0，fflags=1

    li    t1, 2
    csrrs s2, fflags, t1       # s2=1，fflags=3
    csrrc s3, fflags, t0       # s3=3，fflags=2

after_register_ops:
    bne s1, x0, failed
    li  t2, 1
    bne s2, t2, failed
    li  t2, 3
    bne s3, t2, failed
    csrr t2, fflags
    li  t3, 2
    bne t2, t3, failed

    # 2. 立即数形式：CSRRSI、CSRRCI、CSRRWI
    csrrsi s4, fflags, 4      # s4=2，fflags=6
    csrrci s5, fflags, 2      # s5=6，fflags=4
    csrrwi s6, fflags, 16     # s6=4，fflags=16

after_immediate_ops:
    li  t2, 2
    bne s4, t2, failed
    li  t2, 6
    bne s5, t2, failed
    li  t2, 4
    bne s6, t2, failed
    csrr s7, fflags           # s7=16
    li  t2, 16
    bne s7, t2, failed

    # 3. 不写 CSR 的读取形式
    csrrs  s8, fflags, x0     # rs1=x0：只读
    csrrci s9, fflags, 0      # uimm=0：只读

after_read_only_ops:
    li  t2, 16
    bne s8, t2, failed
    bne s9, t2, failed
    csrr t3, fflags
    bne t3, t2, failed

    # 恢复进入程序时的异常标志
    csrw fflags, s0

    li a0, 1
    la a1, pass_message
    li a2, pass_message_len
    li a7, 64                 # write(1, message, length)
    ecall

    li a0, 0
    li a7, 93                 # exit(0)
    ecall

failed:
    # 所有失败路径都发生在恢复之前
    csrw fflags, s0
    li a0, 1
    la a1, fail_message
    li a2, fail_message_len
    li a7, 64
    ecall

    li a0, 1
    li a7, 93                 # exit(1)
    ecall
```

## 13. 汇编、链接和运行

```bash
riscv64-linux-gnu-as \
    -march=rv64if_zicsr \
    -mabi=lp64 \
    -g \
    -o csr_demo.o \
    csr_demo.S

riscv64-linux-gnu-ld \
    -m elf64lriscv \
    -static \
    -e _start \
    -o csr_demo \
    csr_demo.o
```

运行并查看退出状态：

```bash
qemu-riscv64-static ./csr_demo
echo $?
```

在支持 F 与 Zicsr 的 QEMU 用户态环境中，预期为：

```text
CSR instruction demo passed!
0
```

若任一检查失败，程序输出 `CSR instruction demo failed!`，并以状态 1 退出。若运行环境没有启用 F 扩展或禁止用户态访问浮点 CSR，程序可能在首次 CSR 访问时因非法指令而退出。

## 14. 查看真实指令

查看源码和反汇编：

```bash
riscv64-linux-gnu-objdump -drS csr_demo
```

关闭伪指令别名：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    csr_demo
```

重点观察：

- `csrr s0, fflags` 展开为 `csrrs s0, fflags, x0`。
- `csrw fflags, s0` 展开为 `csrrw x0, fflags, s0`。
- `csrrwi x0, fflags, 0` 虽然 `rd=x0` 且立即数为零，仍是一条写入零的指令。
- `csrrci s9, fflags, 0` 只是读取，不写回 CSR。

在数字寄存器名的反汇编输出中，`fflags` 也可能以 CSR 编号 `0x001` 显示；关键是观察真实操作码以及 `rd`、`rs1` 或立即数字段。

## 15. 使用 GDB 验证旧值与新值

终端一：

```bash
qemu-riscv64-static -g 1234 ./csr_demo
```

终端二：

```bash
gdb-multiarch ./csr_demo
```

连接并设置断点：

```gdb
target remote :1234
break after_register_ops
break after_immediate_ops
break after_read_only_ops
continue
```

在第一个断点，前三条寄存器形式已执行：

```gdb
info registers s1 s2 s3
```

预期依次为 `0、1、3`。继续到立即数形式之后：

```gdb
continue
info registers s4 s5 s6
```

预期依次为 `2、6、4`。继续到只读形式之后：

```gdb
continue
info registers s8 s9
```

二者都应为 `16`。可用 `display/i $pc` 与 `si` 单步观察 CSR 指令；GDB 是否直接显示 `fflags` 取决于目标描述和版本，因此这里主要通过保存的旧值与程序的验证分支观察行为。

## 16. 常见错误

### 16.1 以为 CSR 指令是基础 `RV64I` 的一部分

本篇六条指令属于 `Zicsr`。汇编时应指定相应目标架构；Demo 使用 `rv64if_zicsr`，因为它还需要 F 扩展提供 `fflags`。

### 16.2 把 `rs1=x0` 当作所有指令的“不写”开关

只有 `CSRRS/CSRRC` 在 `rs1=x0` 时不写 CSR。`CSRRW` 在 `rs1=x0` 时会尝试把零写入 CSR。

### 16.3 把寄存器里的零当作 `x0`

`CSRRS/CSRRC` 使用非 `x0` 寄存器时，即使该寄存器的值为零，也属于会写 CSR 的形式，可能触发只读 CSR 的写权限错误或写副作用。

### 16.4 把零立即数当作所有指令的“只读”开关

`CSRRSI/CSRRCI` 的 `uimm=0` 不写 CSR；`CSRRWI` 的 `uimm=0` 会写入零。

### 16.5 在用户态访问特权级 CSR

`mstatus/mtvec` 等机器模式 CSR 无法由本文的 Linux 用户态程序直接访问。需要在相应特权级的环境中研究它们。

### 16.6 假设计数器在每个运行环境都可读

`cycle/time/instret` 是只读 CSR，其访问还依赖 Zicntr 与环境权限。不要把可选计数器读取当成验证全部 CSR 指令的通用 Demo。

## 17. 本部分小结

```text
替换：CSRRW   CSRRWI
置位：CSRRS   CSRRSI
清位：CSRRC   CSRRCI
```

最需要记住的规则是：

1. CSR 指令属于 `Zicsr`，CSR 编号是指令里的 12 位字段。
2. 需要读取且 `rd ≠ x0` 时，写入 `rd` 的是 CSR 旧值，而不是修改后的值。
3. `CSRRS/CSRRC` 在 `rs1=x0` 时只读；立即数版本在 `uimm=0` 时只读。
4. `CSRRW/CSRRWI` 即使写入零也仍是写操作；`rd=x0` 时才省略读取。
5. CSR 可访问性由扩展、特权级与运行环境共同决定。
6. `CSRR/CSRW/CSRS/CSRC` 是常用伪指令，反汇编可观察对应的真实指令。

规范依据：[Zicsr 扩展](https://docs.riscv.org/reference/isa/unpriv/zicsr.html)、[CSR 地址和权限约定](https://docs.riscv.org/reference/isa/_attachments/riscv-privileged.pdf)、[浮点 CSR 地址表](https://docs.riscv.org/reference/isa/unpriv/rv-32-64g.html)、[Zicntr 计数器](https://docs.riscv.org/reference/isa/v20260120/unpriv/counters.html)。
