# RISC-V 64 位系统开发：A 扩展与 RVWMO 内存模型

## 0. 本篇指令与寄存器缩写速查

下表列出本文代码、示例或讨论中出现的指令助记符。英文名称用于拆解缩写；“伪指令”由汇编器展开，具体机器指令以最终反汇编为准。表中的 `W` = **Word**（32 位），`D` = **Doubleword**（64 位）；`U` 在 `LBU/BLTU/DIVU` 等助记符中是 **Unsigned**，在 `LUI/AUIPC` 中是 **Upper**。

| 指令或写法 | 英文全称 | 类型 |
|---|---|---|
| `ADD` | **Add** | 真实指令 |
| `ADDI` | **Add Immediate** | 真实指令 |
| `AMOADD.D` | **Atomic Memory Operation Add Doubleword** | 真实指令 |
| `AMOADD.D.AQRL` | **Atomic Memory Operation Add Doubleword, Acquire-Release** | 真实指令 |
| `AMOSWAP.W` | **Atomic Memory Operation Swap Word** | 真实指令 |
| `AMOSWAP.W.RL` | **Atomic Memory Operation Swap Word, Release** | 真实指令 |
| `BNE` | **Branch if Not Equal** | 真实指令 |
| `BNEZ` | **Branch if Not Equal to Zero** | 伪指令 |
| `ECALL` | **Environment Call** | 真实指令 |
| `FENCE` | **Fence** | 真实指令 |
| `FENCE.I` | **Instruction-Fetch Fence** | 真实指令 |
| `LA` | **Load Address** | 伪指令 |
| `LD` | **Load Doubleword** | 真实指令 |
| `LI` | **Load Immediate** | 伪指令 |
| `LR.D` | **Load Reserved Doubleword** | 真实指令 |
| `LR.D.AQ` | **Load Reserved Doubleword, Acquire** | 真实指令 |
| `LR.W` | **Load Reserved Word** | 真实指令 |
| `LW` | **Load Word** | 真实指令 |
| `SC.D` | **Store Conditional Doubleword** | 真实指令 |
| `SC.D.RL` | **Store Conditional Doubleword, Release** | 真实指令 |
| `SC.W` | **Store Conditional Word** | 真实指令 |
| `SD` | **Store Doubleword** | 真实指令 |
| `SFENCE.VMA` | **Supervisor Memory-Management Fence** | 真实指令 |
| `SW` | **Store Word** | 真实指令 |

`AMO` = **Atomic Memory Operation**；`LR/SC` = **Load Reserved / Store Conditional**；`.W/.D` 分别操作 32/64 位，`.aq/.rl/.aqrl` 分别表示 **acquire / release / acquire-release** 排序标记，例如 `LR.D.AQ`、`SC.D.RL`、`AMOSWAP.W.RL`；后缀与基础助记符合起来仍是同一类真实指令。

`AMOSWAP/AMOADD/AMOAND/AMOOR/AMOXOR/AMOMIN/AMOMAX` 中的运算名依次是 **Swap / Add / AND / OR / Exclusive OR / Minimum / Maximum**；实际指令还要带 `.W` 或 `.D` 等宽度后缀。

ABI 别名是同一套 `x0～x31` 整数寄存器的另一种写法，并非额外寄存器；下面给出本文提及的整数寄存器编号与对应 ABI 别名。`PC` 若在本文出现，指 **Program Counter**（程序计数器），它不属于 `x0～x31`。

| ABI 别名 | 硬件编号 | 英文完整名称 |
|---|---|---|
| `zero` | `x0` | **Zero** |
| `gp` | `x3` | **Global Pointer** |
| `t0` | `x5` | **Temporary register 0** |
| `t1` | `x6` | **Temporary register 1** |
| `t2` | `x7` | **Temporary register 2** |
| `a0` | `x10` | **Argument register 0** |
| `a1` | `x11` | **Argument register 1** |
| `a7` | `x17` | **Argument register 7** |
| `t3` | `x28` | **Temporary register 3** |
| `t4` | `x29` | **Temporary register 4** |
| `t5` | `x30` | **Temporary register 5** |
| `t6` | `x31` | **Temporary register 6** |

寄存器的用途、权限和指令的副作用以正文为准。术语和编号可对照[官方 ISA 规范](https://docs.riscv.org/reference/isa/unpriv/unpriv-index.html)、[特权 CSR 分配表](https://docs.riscv.org/reference/isa/priv/priv-csrs.html)、[ELF psABI 寄存器约定](https://github.com/riscv-non-isa/riscv-elf-psabi-doc/blob/master/riscv-cc.adoc)与[汇编器伪指令手册](https://github.com/riscv-non-isa/riscv-asm-manual/blob/main/src/asm-manual.adoc)。

[系统与屏障指令](11-rv64-system-and-fence-instructions.md)已经说明 `FENCE` 能约束操作顺序，却不会使普通的“加载、计算、存储”自动成为不可分割的更新。本篇进入多 hart 固件和 TEE monitor 的核心：**原子性**、**可见顺序**、**缓存一致性**是三个不同问题。A 扩展提供 LR/SC 与 AMO；RVWMO 定义普通内存操作在不同 hart 之间允许怎样被观察。

本篇有两个短 Demo：一个在 Linux 用户态直接执行 A 指令，检查 AMO 旧值和 LR/SC 成功语义；另一个用 C11 原子变量运行双线程消息发布，展示 release/acquire 的契约。后者是功能验证，**不能靠 QEMU 跑一次就证明所有弱内存结果均被禁止**。多 hart 裸机压力测试与进一步证明见 [Lab 04](/home/guoshichao/riscv_asm_lab/labs/lab04-rvwmo-atomics-locks/README.md)。

## 1. 三个经常混淆的问题

| 问题 | 典型错误 | 正确工具 |
|---|---|---|
| 原子性 | 两个 hart 都 `ld` 到 0，各自 `addi` 后 `sd` 为 1，丢掉一次更新 | `AMOADD` 或正确的 LR/SC 循环 |
| 顺序 | 已写入 payload，却先让别的 hart 看到 ready | release/acquire、适当的 `FENCE` |
| 一致性 | DMA 设备与 CPU 缓存没有同一份最新数据 | 平台定义的 coherent 机制或缓存维护；普通 `FENCE` 不够 |

“单 hart 上写完马上读回正确”只能说明自己的执行路径正常，不能推出另一个 hart 或设备观察到相同顺序。RVWMO 允许不同 hart 对普通内存操作的观察存在较弱顺序；需要同步时，应从共享对象的发布／消费关系倒推需要的约束。

## 2. A 扩展有哪些操作

| 类别 | RV64 常见指令 | 作用 |
|---|---|---|
| LR/SC | `LR.W/D`、`SC.W/D` | 先读取并建立 reservation，再尝试条件存储 |
| AMO | `AMOSWAP`、`AMOADD`、`AMOAND`、`AMOOR`、`AMOXOR`、`AMOMIN/MAX` 等 `.W/.D` | 对同一地址做不可分割的读—改—写，`rd` 得到旧值 |

现行规范还把这两组能力分别命名为 `Zalrsc`（LR/SC）和 `Zaamo`（AMO）；完整 A 扩展包含二者。系统软件应按实际平台声明与运行时能力选择实现路径，不能只凭汇编器接受 `-march` 断言硬件支持。

`.W` 更新 32 位内存值；在 RV64 中，返回到整数寄存器的旧 32 位值会符号扩展到 64 位。`.D` 更新 64 位。基础 A 扩展的这些访问应使用自然对齐地址：`.W` 为 4 字节对齐，`.D` 为 8 字节对齐。目标物理区域还须支持相应的原子操作；不要直接假设 MMIO 寄存器支持 AMO。

`AMOADD.D rd, rs2, (rs1)` 的含义是：把 `rs1` 指向的旧 64 位值放入 `rd`，把旧值加 `rs2` 的结果写回同一地址。`rd=x0` 只是丢弃旧值，并不取消内存更新。

## 3. LR/SC：失败意味着什么

```asm
retry:
    lr.d.aq t1, (a0)
    add     t2, t1, a1
    sc.d.rl t3, t2, (a0)
    bnez    t3, retry
```

`LR` 读取值并建立 reservation。`SC` 成功时写入并向 `rd` 写 `0`；失败时不写入，并向 `rd` 写**非零**值。不要把失败码当作“另一个 hart 一定刚好写了该地址”的证据：reservation 可能因多种允许的原因失效；也不要假设失败码只会是 `1`。失败后必须重新执行 `LR`，不能只重复 `SC`。

这里的循环仅包含 LR/SC、计算和失败重试分支，符合教学所需的短 constrained loop 形式。规范的最终成功保证附带实现与执行环境条件，并不表示任何包含函数调用、任意访存、长时间运行逻辑的循环都保证前进。忙等还需考虑中断、抢占和功耗；在内核或固件中自旋时，要先明确是否可能在持锁期间被重入。

## 4. `aq`、`rl` 与内存域

A 指令可带 `.aq`、`.rl` 或 `.aqrl`：

| 标注 | 直观用途 | 要求 |
|---|---|---|
| 无 | relaxed 原子更新 | 保证该更新不可分割，不额外提供所需同步顺序 |
| `.aq` | acquire，通常用于取得锁或观察发布状态 | 同一地址域中后续的内存操作不得排在该原子操作之前 |
| `.rl` | release，通常用于释放锁或发布状态 | 同一地址域中先前的内存操作不得排在该原子操作之后 |
| `.aqrl` | 同时提供两边的约束 | 用于同时需要 acquire/release 的原子操作；具体性能依赖实现 |

`aq/rl` 只作用于被该原子指令访问的地址所属的内存域：普通内存与 I/O 域不能想当然地互相排序。例如先写普通 RAM 中的 DMA 描述符，再写 MMIO doorbell，需分析 `FENCE W,O` 或平台要求的更强顺序；一个针对 RAM 锁变量的 `amoswap.w.rl` 本身不等于“设备已经看到描述符”。缓存是否与 DMA 设备一致是另一层问题。

简易自旋锁常用 acquire 的原子交换取得所有权、release 的原子交换清零。失败时读取锁变量的轮询可以较弱，但**成功路径**要建立 acquire。若释放锁时只做普通 `sw`，要另行证明前面的临界区写入和解锁写入之间建立了足够的 release 顺序；不要凭“代码顺序看起来是先写数据”作判断。

## 5. Demo A：AMO 旧值与 LR/SC 重试

保存为 `atomic_demo.S`。`counter` 在 `.data` 中自然对齐。程序直接从 `_start` 运行，因此关闭可能依赖未初始化 `gp` 的 relaxation。

```asm
    .option nopic
    .option norelax

    .section .data
    .balign 8
counter:
    .dword 0

    .section .text
    .globl _start
_start:
    la t0, counter
    li t1, 5
    amoadd.d.aqrl t2, t1, (t0)  # 旧值 0；内存变成 5
    bnez t2, failed

retry:
    lr.d.aq t2, (t0)           # 读当前值 5
    addi    t3, t2, 1
    sc.d.rl t4, t3, (t0)       # 成功时写 6，t4 = 0
    bnez    t4, retry          # 失败必须重新 LR

    ld t5, 0(t0)
    li t6, 6
    bne t5, t6, failed

    li a0, 0                   # Linux exit(0)
    li a7, 93
    ecall
failed:
    li a0, 1                   # Linux exit(1)
    li a7, 93
    ecall
```

构建、运行、看真实指令：

```bash
riscv64-linux-gnu-as -march=rv64ia -mabi=lp64 \
    -o atomic_demo.o atomic_demo.S
riscv64-linux-gnu-ld -m elf64lriscv -static -e _start \
    -o atomic_demo atomic_demo.o
qemu-riscv64-static ./atomic_demo
echo $?
riscv64-linux-gnu-objdump -d -M no-aliases atomic_demo
```

退出状态应为 `0`。单 hart 下 `SC` 往往一次成功，但代码仍须正确处理失败；本 Demo 的成功不证明发生过竞争，也不证明任何弱内存结果不存在。可在 GDB 的 `retry` 附近观察 `t2/t4`，确认 `SC` 返回值与最终内存值的意义不同。

## 6. Demo B：双线程消息发布

下面用 C11 原子变量定义跨线程协议。`payload` 不是原子变量，但它在 producer 写入、consumer 读取之间有 release/acquire 同步，因此**这个程序没有因 `payload` 产生数据竞争**。编译器可选择 AMO、LR/SC 或 fence 等合法映射；不要假定一段 C 一定对应某条固定机器指令。

保存为 `mailbox_demo.c`：

```c
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

struct mailbox {
    int payload;
    atomic_int ready;
};

static struct mailbox box;
static int observed;

static void *consumer(void *unused) {
    (void)unused;
    while (atomic_load_explicit(&box.ready, memory_order_acquire) == 0) {
        /* 只用于短小演示；产品代码需考虑调度和等待策略。 */
    }
    observed = box.payload;
    return NULL;
}

int main(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, consumer, NULL) != 0) return 2;

    box.payload = 0x12345678;
    atomic_store_explicit(&box.ready, 1, memory_order_release);

    if (pthread_join(thread, NULL) != 0) return 3;
    if (observed != 0x12345678) return 1;
    puts("RVWMO publication demo passed!");
    return 0;
}
```

编译、运行：

```bash
riscv64-linux-gnu-gcc -O2 -static -pthread -march=rv64gc -mabi=lp64d \
    -o mailbox_demo mailbox_demo.c
qemu-riscv64-static ./mailbox_demo
echo $?
riscv64-linux-gnu-objdump -dr mailbox_demo | less
```

预期输出 `RVWMO publication demo passed!`，退出状态为 `0`。请在反汇编中找到 `ready` 的 release store 与 acquire load 周围的实际指令，**按编译器输出分析，不把某个版本的输出写成架构保证**。若把 `ready` 改成普通 `int`，C 程序会产生数据竞争；即使某次 QEMU 运行仍输出成功，也不能认为该修改正确。

### 6.1 为什么 release/acquire 足以发布 payload

在 C11 层，producer 对 `payload` 的写入先于 release store；consumer 的 acquire load 读到该 store 的值后，两者建立同步关系；于是 consumer 读取 `payload` 时能看到该写入。硬件和编译器都必须实现这一语言契约。这一推理有前提：consumer 的 acquire 操作确实读到了对应发布（或相关 release sequence）的值，并且没有其他线程在同步区间里乱写 `payload`。

```text
producer hart                         consumer hart
W(payload = 0x12345678)              acquire R(ready) == 1
        ↓ 程序顺序                           ↓ 程序顺序
release W(ready = 1)  ──同步──>       R(payload)
```

箭头说明的是允许推理的顺序关系，并非说所有普通内存指令都会在硬件上按源码顺序执行。若 consumer 只读到 `ready=0`，它没有取得这次发布的结果；继续等待即可。若还有其他线程改写 `payload`，就要重新分析所有写入之间的同步。

在纯汇编中没有 C11 编译器替你生成约束。一个直接的对应思路是：写 payload；对 ready 执行 release 原子写；另一 hart 通过 acquire 原子读确认 ready；随后读 payload。普通 `lw/sw` 本身没有 `aq/rl` 编码，若不用原子 RMW，需要配合适当的 `FENCE` 设计协议。性能优化应以明确的内存模型证明为前提。

## 7. TEE、OpenSBI 与 MMIO 的边界

| 场景 | 需要检查的独立条件 |
|---|---|
| 多 hart 共享的 enclave 状态表 | 更新是否原子；发布前数据是否完成；读取方是否 acquire；切换时是否清理敏感状态 |
| OpenSBI per-hart mailbox / IPI | RAM 中消息与通知顺序；远端 hart 是否建立接收同步；IPI 寄存器可能是 MMIO |
| DMA ring + doorbell | CPU 与设备是否缓存一致；描述符的可见性；普通内存写与 MMIO 写的顺序 |
| 修改可执行代码 | 数据写入与后续取指是否同步；跨 hart 时需要额外机制，参见 `FENCE.I` 章节 |

`FENCE RW,RW` 不会使两条普通 `sd` 合成为原子更新；AMO 的 `.aqrl` 也不代替页表失效同步或缓存维护。`SFENCE.VMA`、`FENCE.I`、`CBO.*` 分别针对地址转换、取指和缓存块管理；详见[Sv39](17-rv64-sv39-and-sfence-vma.md)、[系统与屏障](11-rv64-system-and-fence-instructions.md)以及[缓存维护与 DMA](19-rv64-cache-cmo-dma-and-code-publish.md)。

## 8. 验证时怎样避免误判

1. **功能测试**：检查返回值、最终共享数据、退出状态。Demo A/B 属于这一层。
2. **并发压力**：多 hart、多轮次和超时保护能暴露一部分竞态，但通过不构成内存模型证明。[Lab 04](/home/guoshichao/riscv_asm_lab/labs/lab04-rvwmo-atomics-locks/README.md)提供这个层次。
3. **模型推理**：写出每个 hart 的事件、顺序约束和同步边，必要时使用 litmus/herd 工具验证被禁止或允许的结果。不要把 QEMU 没观察到某种重排等同于规范禁止。
4. **目标平台核对**：确认访问区域的 atomicity、缓存一致性、MMIO 排序和扩展支持；这些不由 `-march=rv64ia` 的汇编成功自动保证。

规范依据：[A 扩展](https://docs.riscv.org/reference/isa/unpriv/a-st-ext.html)、[RVWMO 内存模型](https://docs.riscv.org/reference/isa/unpriv/rvwmo.html)、[RVWMO 解释材料](https://docs.riscv.org/reference/isa/unpriv/mm-eplan.html)。
