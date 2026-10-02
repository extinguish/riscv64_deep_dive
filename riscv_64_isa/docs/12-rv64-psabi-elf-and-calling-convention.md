# RISC-V 64 位系统开发：调用约定与 ELF psABI

前面的[跳转与函数调用](08-rv64-jump-and-branch-instructions.md)解释了 `JAL/JALR` 如何改变 PC 和 `ra`。真实固件还要回答另一组问题：参数放在哪里、谁保存寄存器、栈如何建立、汇编如何调用 C，以及链接器为什么能改变最终指令。这些规则属于 **ELF psABI**（处理器专用应用二进制接口），并不是新增的 CPU 指令。

本篇先用 Linux 用户态混合 C/汇编 Demo 检查调用约定，再把结论应用到 OpenSBI、trap handler 和 TEE monitor。相同的寄存器约定可以用于裸机 C/汇编混编，但启动、陷入和安全上下文切换还有额外责任。

## 1. 先分清 ISA、ABI、执行环境

| 层次 | 回答的问题 | 示例 |
|---|---|---|
| ISA | 一条指令对机器状态做什么 | `jal ra, target` 写入返回 PC |
| psABI | 两个函数怎样交换数据且互不破坏状态 | `a0～a7` 传参数，`s0～s11` 由被调用者恢复 |
| 执行环境 | 程序从哪里启动，谁处理陷入 | Linux `_start`、OpenSBI M-mode、裸机 reset |

`-march=...` 选择允许生成的 ISA；`-mabi=...` 选择函数边界上的约定。例如 `LP64` 的 `long`、指针为 64 位，浮点参数按基础整数约定传递；`LP64D` 仍是 64 位 `long` 和指针，但具备双精度硬浮点调用约定。**是否执行了浮点指令**与**函数参数采用哪个浮点 ABI**是不同问题。混合链接时必须让对象文件的 ABI 兼容，不能仅看 `-march` 是否相同。

## 2. 整数寄存器的职责

| 寄存器 | 用途 | 普通函数调用后是否保持原值 |
|---|---|---|
| `zero` | 恒为零 | 不适用 |
| `ra` (`x1`) | 返回地址 | 否 |
| `sp` (`x2`) | 栈指针 | 必须恢复；标准 ABI 中执行期间保持对齐 |
| `gp` (`x3`) | 全局指针或平台指定用途 | 固定寄存器，不应当作临时寄存器 |
| `tp` (`x4`) | 线程指针 | 固定寄存器，不应当作临时寄存器 |
| `t0～t6` | 临时寄存器 | 否 |
| `a0～a7` | 参数；`a0/a1` 也传返回值 | 否 |
| `s0～s11` | 被调用者保存的寄存器 | 是 |

“被调用者保存”的意思是：函数可以修改 `s` 寄存器，但返回前必须恢复到进入函数时的值。**调用者**如果还要在一次调用之后使用 `a`、`t` 或旧 `ra`，须在调用前自行保存。`s0` 可作为帧指针 `fp`，但使用帧指针不是强制要求。

一个非叶函数会再次执行 `call`。新调用会覆盖 `ra`，所以非叶函数通常在栈中保存自己的返回地址。叶函数不调用其他函数时，可以不保存 `ra`，但中断或异常入口不能因此只保存普通 ABI 规定的寄存器：异步陷入可能发生在任意指令之间。

## 3. 参数、返回值与 RV64 的符号扩展细节

基础整数调用约定使用 `a0～a7` 放前八个整数寄存器参数。用完后，后续参数在栈上传递；被调用函数刚进入时，第一个栈参数位于 `0(sp)`。返回值通常在 `a0`，需要两个寄存器时可用 `a0/a1`。结构体、超过两个 XLEN 的对象、变参和对齐较高的对象有额外规则，不应简单地套用“每个参数一个寄存器”。

一个容易被误判为 ABI 错误的 RV64 细节：窄于 XLEN 的整数标量先按类型有无符号扩展到 32 位，再**符号扩展到 64 位**。因此把 `unsigned int 0x80000000` 作为参数传入时，`a0` 的原始位模式是 `0xffffffff80000000`。数值语义仍是无符号 32 位；汇编函数若想把它当成 64 位无符号数，应显式零扩展，例如 `slli a0,a0,32; srli a0,a0,32`。下面的 Demo 会检查原始位模式。

## 4. 栈帧：对齐、保存与恢复

标准 RV64 psABI 要求 `sp` 在函数入口对齐到 **16 字节**，且标准 ABI 函数执行期间保持对齐。栈向低地址增长。假设入口 `sp=0x10000`，分配 32 字节栈帧后 `sp=0xffe0`，仍满足对齐；只减 8 字节后再调用 C 函数，则会违约。

常见的非叶函数栈帧如下：

```text
入口 sp  -> 调用者栈帧
sp-8     -> 保存 ra
sp-16    -> 保存 s0
sp-24    -> 保存 s1
sp-32    -> 保存 s2；新的 sp
```

这里的布局是本篇 Demo 的实现选择，并非 ABI 规定所有函数都必须以相同顺序放寄存器。栈指针以下的空间也不是可随意使用的“红区”；要使用就先分配。若调用链需要可靠回溯，还需正确的 `.cfi_*` 展开信息；保存寄存器本身并不会自动产生准确 CFI。对应的进阶练习见 [Lab 01](/home/guoshichao/riscv_asm_lab/labs/lab01-asm-runtime-abi-elf/README.md)。

## 5. 完整 Demo：C ↔ 汇编的非叶调用

这个 Demo 同时检查三件事：非叶函数跨两次调用保存中间结果、**第九个参数**来自栈，以及 `unsigned int` 参数的 RV64 原始位模式。这里选用发行版常见的 `LP64D` Linux 交叉工具链；示例函数只做整数运算，重点仍是整数调用约定。

保存为 `abi_main.c`：

```c
#include <stdint.h>
#include <stdio.h>

extern long asm_fold(long x, long y);
extern long ninth(long a, long b, long c, long d,
                  long e, long f, long g, long h, long i);
extern unsigned long raw_u32_arg(unsigned int x);

/* 从汇编发起的调用，编译器仍按同一 psABI 接收 a0/a1。 */
__attribute__((noinline))
long c_step(long x, long y) {
    return x * 3 + y;
}

int main(void) {
    if (asm_fold(5, 7) != 48) return 1; /* 22 + 26 */
    if (ninth(1, 2, 3, 4, 5, 6, 7, 8, 99) != 99) return 2;
    if (raw_u32_arg(UINT32_C(0x80000000)) !=
        UINT64_C(0xffffffff80000000)) return 3;
    puts("psABI demo passed!");
    return 0;
}
```

保存为 `abi_functions.S`：

```asm
    .text
    .balign 4
    .globl asm_fold
    .type asm_fold, @function
asm_fold:
    addi sp, sp, -32        # 32 字节栈帧；入口和调用点都保持 16 字节对齐
    sd   ra, 24(sp)         # 下一次 call 会覆盖 ra
    sd   s0, 16(sp)
    sd   s1,  8(sp)
    sd   s2,  0(sp)

    mv s0, a0               # 跨调用保存 x
    mv s1, a1               # 跨调用保存 y
    call c_step             # c_step(x,y) = 22
    mv s2, a0               # 第二次调用会覆盖 a0，先保存第一次结果

    mv a0, s1
    mv a1, s0
    call c_step             # c_step(y,x) = 26
    add a0, a0, s2          # 返回 48

    ld   s2,  0(sp)
    ld   s1,  8(sp)
    ld   s0, 16(sp)
    ld   ra, 24(sp)
    addi sp, sp, 32
    ret
    .size asm_fold, .-asm_fold

    .globl ninth
    .type ninth, @function
ninth:
    ld a0, 0(sp)            # 入口 sp 指向第一个栈参数：第九个 long
    ret
    .size ninth, .-ninth

    .globl raw_u32_arg
    .type raw_u32_arg, @function
raw_u32_arg:
    ret                    # 原样返回入参 a0 的 64 位位模式
    .size raw_u32_arg, .-raw_u32_arg
```

构建、运行：

```bash
riscv64-linux-gnu-gcc -O2 -static -march=rv64gc -mabi=lp64d \
    -o abi_demo abi_main.c abi_functions.S
qemu-riscv64-static ./abi_demo
echo $?
```

预期输出 `psABI demo passed!`，退出状态为 `0`。若返回 `1/2/3`，分别对应三项检查。检查 ELF 与汇编：

```bash
riscv64-linux-gnu-readelf -h abi_demo
riscv64-linux-gnu-readelf -A abi_demo
riscv64-linux-gnu-objdump -dr abi_demo | less
```

`-march=rv64gc` 只是为了与这里的 Linux 静态库及 `LP64D` 工具链匹配；本篇不讲 C 压缩指令。最终链接器可能缩短 `call`，所以要以最终反汇编确认指令，不应把源代码中的伪指令当成固定编码。

### 5.1 从 Demo 推导责任边界

`asm_fold` 要在第二次 `call` 之后继续使用第一次结果，因此把结果放在 `s2`。它也使用了 `s0/s1`，所以必须保存并恢复这三个寄存器。即使 `c_step` 由 C 编译，调用约定也不变。`ninth` 是叶函数，既不改 `sp` 也不调用其他函数，可以直接读取入口 `sp` 所指的栈参数并返回。

试着只删除 `sd ra,24(sp)` 和相应恢复，并观察最终反汇编：第二次 `call` 会覆盖原调用者的返回地址，`ret` 不再回到 `main`。这是一项**预期故障的推理练习**，不建议把可能无限循环的版本当成正常运行命令。

## 6. `gp/tp` 与手写启动代码

在标准 ABI 中，普通函数不把 `gp/tp` 当临时寄存器。`tp` 关联线程本地存储；`gp` 可能参与链接器对附近全局对象地址的放宽。手写 `_start` 没有运行库替你建立状态时，应先明确谁初始化 `gp`、栈和 `.bss`。如果初始化 `gp` 的 `la` 自身又被放宽成依赖 `gp` 的指令，就形成循环依赖；常见做法是在该小段初始化代码周围使用 `.option norelax`。

一种使用链接脚本符号的初始化形态如下。它应出现在任何可能依赖 `gp` 的全局对象访问之前；`__global_pointer$` 的位置由链接脚本和目标 psABI 确定：

```asm
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop
```

`.option push/pop` 将关闭 relaxation 的作用域限制在这段初始化代码。若固件随后整体搬移，仍须确认此时加载到 `gp` 的值对应的是**当前实际执行位置**，不能只凭链接时符号值推断。

第 [01 篇访存 Demo](01-rv64-memory-instructions.md)展示另一种教学处理：完全不用 `gp`，对该 Demo 关闭 relaxation 并逐项核对实际读写。生产固件通常应按自己的链接脚本、代码模型和启动协议初始化 `gp`，并用 `readelf/objdump` 检查链接结果，而不是全局禁用所有优化。`gp` 的特殊用途与放宽规则由 psABI 和平台 ABI 共同决定。

## 7. ELF、重定位与代码模型

ELF 头的 `e_flags` 记录浮点 ABI 等信息，`.riscv.attributes` 记录架构属性。源代码中的 `la`、`call` 可能先生成重定位，再由链接器根据最终地址选择指令序列。`medlow`、`medany` 和 PIC/GOT 是不同的地址构造约定；不能把某个链接地址下的反汇编硬编码成通用的可重定位代码。具体数值边界见[寻址范围](10-rv64-addressing-ranges.md)。

| 形式 | 典型地址构造 | 固件开发时要问的问题 |
|---|---|---|
| `medlow` | `LUI` 加低部立即数 | 最终绝对地址是否落在该模型可编码的 RV64 区间？ |
| `medany` | `AUIPC` 加低部立即数 | 引用目标与当前 PC 是否在组合指令可达范围内？ |
| PIC/GOT | 通过 GOT 间接取得可重定位符号地址 | 谁在何时完成重定位，GOT 当前是否已可用？ |

这些是典型工具链约定，不能仅凭 `-march` 推断；同一源码在 `.o` 与最终 ELF 中可能有不同指令。对照 `objdump -dr abi_functions.o` 与 `objdump -dr abi_demo`、检查 `readelf -r`，能把源码伪指令、重定位和执行指令分开。

在 OpenSBI/TEE 开发中尤其要核对：固件是否可重定位、重定位发生在何时、物理地址与虚拟地址是否混用、`gp` 是在当前地址还是重定位后的地址建立，以及 trap 入口调用 C 之前是否已有合规的栈。发现“单步看寄存器都对，但调用 C 后损坏”的问题时，先检查 `sp/ra/gp` 与保存寄存器，再看最终 ELF。

## 8. ABI 与 trap 上下文不是同一个集合

普通函数只需遵守上面的调用者／被调用者保存规则。**异步中断**会打断任意代码，原代码还没有机会按调用约定保存 `a/t` 寄存器；若 handler 要使用这些寄存器，入口必须保存被中断上下文需要恢复的值。硬件陷入会记录部分控制状态，但不会替软件自动保存全部通用寄存器，也不会自动建立 C 函数所需的栈帧。第 [14 篇特权级陷入与返回](14-rv64-privilege-trap-and-return.md)会专门处理这个区别。

## 9. 常见故障检查表

| 现象 | 优先检查 |
|---|---|
| 调用 C 后随机跳转 | 非叶函数是否保存 `ra`；栈上的返回地址是否被覆盖 |
| 只在优化级别变化后出错 | 是否错误依赖 `a/t` 寄存器跨调用不变；内联汇编 clobber 是否完整 |
| 访问全局对象时偶发错误 | `gp` 初始化、链接器 relaxation、PIC/GOT 与实际加载地址 |
| 进入 C 函数后崩溃 | 调用点 `sp` 是否 16 字节对齐；栈是否可写 |
| 汇编与 C 对结构体参数理解不同 | 重新核对 psABI 的聚合类型、对齐、按引用传递规则 |
| 固件对象无法一起链接 | 对比 `readelf -h/-A` 中 ABI 标志与目标 ISA 属性 |

规范依据：[RISC-V 调用约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)、[RISC-V ELF psABI：代码模型、重定位与 relaxation](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-elf.adoc)。
