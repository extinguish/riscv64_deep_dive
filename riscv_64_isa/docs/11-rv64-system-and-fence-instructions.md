# RISC-V 64 位 ISA：系统与屏障指令

本篇补齐 RV64I 基础整数指令中的 `FENCE`、`ECALL`、`EBREAK`，并说明扩展 `Zifencei` 的 `FENCE.I`。示例沿用前几篇的 GNU 汇编器、链接器和 QEMU User Mode 环境。章节目录见[系列目录](README.md)。

| 指令 | 所属 ISA | 主要作用 | 本篇的 Linux 用户态演示 |
|---|---|---|---|
| `FENCE` | RV64I | 对其他观察者约束内存和设备 I/O 操作的顺序 | 汇编、运行；解释单线程 Demo 的局限 |
| `ECALL` | RV64I | 请求执行环境提供服务 | 使用 Linux 系统调用写出消息、退出 |
| `EBREAK` | RV64I | 请求进入断点调试环境 | 说明预期陷入，不作为正常退出的 Demo |
| `FENCE.I` | `Zifencei` | 同步当前 hart 上先前写入与后续取指 | 展示编码；不把它当成 Linux 用户态的通用缓存刷新接口 |

## 1. `FENCE`：约束可观察顺序

语法：

```asm
fence predecessor, successor
```

`predecessor` 指屏障之前的操作集合，`successor` 指屏障之后的操作集合。执行环境中的其他 hart、设备等观察者看到的顺序，要满足前一集合先于后一集合。集合字母含义：

| 字母 | 操作类别 |
|---|---|
| `R` | 内存读取 |
| `W` | 内存写入 |
| `I` | 设备输入 |
| `O` | 设备输出 |

例如：

```asm
fence rw, rw         # 屏障前后的普通内存读写建立顺序约束
fence w, w           # 屏障前后的内存写入建立顺序约束
fence iorw, iorw     # 同时覆盖设备 I/O 与内存读写
```

两个操作集合并非越大越好：更强的约束可能限制硬件重排。具体应根据设备协议或并发算法选择。`FENCE` 只建立指定类别之间的顺序，不让普通加载或存储自动变成原子操作，也不能代替锁或原子扩展中的指令。

单线程程序先写入一个变量，紧接着读回，即使删掉 `FENCE` 也通常读到刚写入的值。因此下面的 Demo **只能验证指令能正确汇编、运行和退出**；它不能证明多 hart 内存排序正确。

### 1.1 可运行 Demo：`FENCE` 与 `ECALL`

将下面内容保存为 `system_fence_demo.S`：

```asm
    .option nopic
    .option norelax       # 手写 _start 未初始化 gp，避免 la 依赖 gp

    .section .rodata
pass_message:
    .ascii "FENCE and ECALL demo passed!\n"
    .equ pass_message_len, . - pass_message
fail_message:
    .ascii "FENCE and ECALL demo failed!\n"
    .equ fail_message_len, . - fail_message

    .section .bss
    .balign 4
value:
    .zero 4

    .section .text
    .globl _start
_start:
    la t0, value
    li t1, 42
    sw t1, 0(t0)
    fence rw, rw
    lw t2, 0(t0)
    bne t1, t2, failed

    # Linux write(1, pass_message, pass_message_len)
    li a0, 1
    la a1, pass_message
    li a2, pass_message_len
    li a7, 64
    ecall

    # Linux exit(0)
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

汇编、链接、运行：

```bash
riscv64-linux-gnu-as -march=rv64i -mabi=lp64 -g \
    -o system_fence_demo.o system_fence_demo.S
riscv64-linux-gnu-ld -m elf64lriscv -static -e _start \
    -o system_fence_demo system_fence_demo.o
qemu-riscv64-static ./system_fence_demo
echo $?
```

预期先输出：

```text
FENCE and ECALL demo passed!
```

随后 `echo $?` 显示 `0`；检查失败时程序输出失败消息并以 `1` 退出。用 `riscv64-linux-gnu-objdump -d -M no-aliases system_fence_demo` 可以查看 `fence` 和 `ecall` 的实际编码。

## 2. `ECALL`：陷入执行环境

`ecall` 没有显式操作数。执行它会触发“环境调用”异常；处理它的执行环境决定如何解释调用。**指令本身不规定系统调用号，也不保证所有环境都有 `write` 或 `exit`。**

本系列的 Linux 用户态 Demo 遵循 Linux RISC-V 系统调用约定：

```text
a7      = 系统调用号
a0～a5  = 参数寄存器
a0      = 返回值（若调用返回）
```

上面的 Demo 把 `64` 放在 `a7` 中调用 `write`，把 `93` 放在 `a7` 中调用 `exit`。在本仓库的裸机实验中，`ecall` 会进入该实验配置的陷入处理流程，不能直接套用这些 Linux 系统调用号。

## 3. `EBREAK`：断点异常

`ebreak` 也没有显式操作数。它用于请求进入调试环境，执行时会产生断点异常：

```asm
    ebreak
```

调试器可利用它实现软件断点。若在普通 Linux 用户态程序中直接执行而没有调试器处理，通常会收到 `SIGTRAP`，程序不会像 `exit(0)` 那样正常结束。是否以及如何恢复执行，由调试器和执行环境决定。不要把 `ebreak` 当成等待、退出或系统调用指令。

## 4. `FENCE.I`：指令流同步属于 `Zifencei`

当程序修改将要执行的代码时，之前的数据写入与之后的取指需要协调。`FENCE.I` 的作用范围是**当前 hart** 上的指令取用；它不能单独保证别的 hart 随后看到新指令。

```asm
    fence.i
```

它属于独立的 `Zifencei` 扩展，不能因为目标是 RV64I 就假设可用。可以用下面的最小文件检查汇编器是否识别该扩展；这一步只汇编，不在 Linux 用户态执行：

```asm
    .text
    .globl fence_i_example
fence_i_example:
    fence.i
    ret
```

保存为 `fence_i_example.S` 后：

```bash
riscv64-linux-gnu-as -march=rv64i_zifencei -mabi=lp64 \
    -o fence_i_example.o fence_i_example.S
riscv64-linux-gnu-objdump -d -M no-aliases fence_i_example.o
```

Linux 标准用户态 ABI 不把直接执行 `FENCE.I` 作为通用的指令缓存同步接口；需要发布动态生成或修改的代码时，应使用运行环境提供的指令缓存同步接口。不要把 `FENCE rw,rw` 当成 `FENCE.I`，两者解决的问题不同。

## 5. 容易混淆的边界

| 写法 | 解决的问题 | 不能据此推断 |
|---|---|---|
| `fence rw,rw` | 对其他观察者约束普通内存读写顺序 | 后续取指一定读取刚写入的代码 |
| `fence.i` | 本 hart 的数据写入与后续取指同步 | 所有 hart 和 Linux 用户态调度都已同步 |
| `ecall` | 交给执行环境处理请求 | 某个调用号在裸机和 Linux 中含义相同 |
| `ebreak` | 触发断点异常 | 像 `exit` 一样正常返回状态码 |

参考规范：[RV32I 基础指令（其中大部分说明同样适用于 RV64I）](https://docs.riscv.org/reference/isa/v20260120/unpriv/rv32.html)、[Zifencei 扩展](https://docs.riscv.org/reference/isa/unpriv/zifencei.html)。
