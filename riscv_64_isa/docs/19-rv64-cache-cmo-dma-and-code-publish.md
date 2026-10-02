# RISC-V 64 位系统开发：缓存块维护、DMA 与代码发布

CPU 与外设共享缓冲区时，“写入内存”“数据到达设备可见点”“设备完成写入”“CPU 重新读取”是不同事件。RVWMO 的排序、缓存块维护、IOMMU 地址转换和所有权协议分别解决不同问题。本篇聚焦 Zicbom/Zicboz 指令、`menvcfg` 授权与 DMA 交接；最后用 QEMU `virt` 验证一个 64 字节缓存块的 `CBO.ZERO/CLEAN/FLUSH` 执行，但不把 QEMU 的同一内存模型当作真实非一致性 DMA 设备。

## 1. 四种常被混为一谈的“同步”

| 机制 | 保证的方向 | 不能代替 |
|---|---|---|
| `FENCE` / acquire-release | 按 RVWMO/PMA 约束显式内存与 I/O 操作顺序 | 脏缓存行回写、远端取指同步 |
| Zicbom `CBO.CLEAN/INVAL/FLUSH` | 在定义的缓存一致性代理集合间维护某个缓存块 | 设备 ownership、IOMMU 失效、任意平台 cache controller |
| `FENCE.I` | 本 hart 先前数据写入与之后取指同步 | 其他 hart 的取指同步、DMA 数据缓存维护 |
| `SFENCE.VMA` | 本 hart 页表存储与之后隐式地址转换同步 | 普通数据 cache clean、指令取指同步 |

Zicbom 的 `CBO.CLEAN` 把修改过的数据向公共点推进；`CBO.INVAL` 使缓存副本无效；`CBO.FLUSH` 等价于原子地 clean 再 invalidate。实现和 `xenvcfg.CBIE` 配置可让 `CBO.INVAL` 实际执行 flush。Zicboz 的 `CBO.ZERO` 对**整块**写零，不保证整个块原子完成。它不是按传入指针开始、按 C 对象长度结束的 `memset`。[CMO 规范](https://docs.riscv.org/reference/isa/unpriv/cmo.html)

`CBO` 的寄存器给出块内任意有效地址，硬件按该地址所在块执行操作；软件遍历一个任意字节区间时必须先查块大小、向下对齐起点并向上对齐终点，检查 `start+length` 和 align-up 溢出。**Zicbom 与 Zicboz 的块大小可以不同**，不能把实验里的 64 字节当成 ISA 常量。QEMU 本例设备树报告 `riscv,cbom-block-size=64`、`riscv,cboz-block-size=64`；其他平台应由固件、设备树/ACPI 或约定接口获得能力与粒度。

例如块大小为 64 字节，缓冲区半开区间 `[0x1003, 0x1049)`（70 字节）需要维护 `[0x1000, 0x1080)`，一共 **两块**。第一块的 `0x1000..0x1002` 和第二块的 `0x1049..0x107f` 都是邻居字节。对 TX 做 clean 可能只是多维护邻居的当前副本；对 RX 做 invalidate 则可能丢弃邻居的脏数据，性质完全不同。若缓冲区为空，块遍历也应为空；若 `end = start + size` 或向上对齐会溢出，必须在执行任何 CBO 前报错。

## 2. 特权级授权与 `CBO.INVAL` 的安全风险

M-mode 可执行已实现的 CBO 指令；较低权限能否执行受 `menvcfg`、`senvcfg`、虚拟化时的 `henvcfg` 等字段约束。`CBIE` 控制 invalidate，`CBCFE` 控制 clean/flush，`CBZE` 控制 zero；具体 WARL 合法值应通过执行环境发现，不能写完就假定生效。低权限非法执行可能产生 illegal-instruction 或 virtual-instruction 异常，trap 原因要结合当前虚拟化状态判断。[CMO 与环境配置](https://docs.riscv.org/reference/isa/unpriv/cmo.html)

允许不可信代码对仍含脏数据的共享块执行纯 invalidate，可能使旧内存值重新暴露。固件应清理旧域的数据、控制块所有权，并按规范选择禁止低权限 invalidate、把它转成 flush，或先 clean/flush 再授权。**同一个缓存块上的相邻对象会被一并维护**：若 DMA RX 缓冲区只占块的中间字节，设备完成后直接 invalidate 可能丢失相邻对象的 CPU 脏写入。因此 DMA 缓冲区最好独占所需缓存块；否则需要 bounce buffer 或精心设计的前后维护协议。

## 3. 一个非一致性 DMA 交接协议

以下是检查顺序的框架，具体平台是否需要 CMO、CMO 到设备的可见点、doorbell 的 I/O 排序类别，必须以设备和内存属性为准。即使设备是 coherent DMA，也仍需描述符所有权与排序。

```text
TX（CPU 生产，设备读取）
  CPU 填 payload
  clean payload 覆盖的每个缓存块
  CPU 填 descriptor（地址、长度、权限、owner=device）
  clean descriptor 覆盖的每个缓存块
  执行适合平台的 memory → MMIO release 顺序
  写 doorbell；设备才可读取 descriptor/payload

RX（设备生产，CPU 读取）
  交给设备前清理旧的 CPU 脏副本，转移 ownership；设备拥有期间 CPU 不再写
  等待设备报告完成；按平台协议建立 acquire 顺序
  确认设备不再写该缓冲区
  invalidate 设备写入范围对应的整块（或平台要求的 flush）
  再按要求排序，然后 CPU 读取 payload
```

对普通 RAM 中的写入和 MMIO doorbell，`fence w,o` 是常见的排序思路；但 CBO 管理操作按目标物理区域的属性归类为 `W` 或 `O`，具体平台还可能要求更强的 `fence iorw,iorw` 或设备专用完成检查。**排序不是“等待设备完成 DMA”**，doorbell 发出后缓冲区仍归设备所有，直到完成协议明确交还。[CMO 排序规则](https://docs.riscv.org/reference/isa/unpriv/cmo.html)

RX 的安全前提可以具体化为：缓冲区独占完整缓存块，交给设备前先处理旧脏副本，设备写入期间 CPU 不碰该块，确认设备完成后再 invalidate 并读取。若缓冲区只覆盖半块，相邻对象有 CPU 脏数据，则不应直接 invalidate 整块；改用独占块或 bounce buffer。coherent DMA 平台可能省去 CMO，但**不能省去 ownership 与 memory/MMIO ordering**。

`CBO.CLEAN` 只针对缓存状态，不负责让设备读到正确 IOVA；IOMMU/IOTLB 失效及设备 DMA 地址权限要另行处理。设备报告 completion 后也不能跳过所有权检查：中断可能合并，描述符可能复用，设备仍在写的块不可交给 CPU 读取。涉及 scatter-gather 时，要合并重叠块、拒绝地址溢出，并把中途失败时的 ownership 回滚写入协议。[Lab 08](/home/guoshichao/riscv_asm_lab/labs/lab08-cache-dma-code-publish/README.md)提供可检查顺序的 trace backend；[Lab 09](/home/guoshichao/riscv_asm_lab/labs/lab09-iommu-translation-invalidation/README.md)讨论设备侧地址转换。

TEE 或安全监控器还要考虑跨域复用：旧域的脏缓存块在物理页转交新域前是否已到达规定的共同点；新域是否可能通过同一块的邻居字节看到旧数据；设备是否仍持有该物理页的 DMA 映射。页表/PMP 收紧、IOMMU 失效、设备 quiesce 和 cache maintenance 的完成顺序必须形成一条可审计的 ownership 转移链。单独执行一次 `fence rw,rw` 无法代替其中任何一个独立步骤。

## 4. 动态代码发布与页属性

CPU 写入新指令后，本 hart 用 `FENCE.I` 同步数据写入与后续取指；其他 hart 需要各自执行相应同步，通常通过 IPI/rendezvous 或操作系统提供的代码发布接口。`CBO.CLEAN` 不直接替代 `FENCE.I`，`SFENCE.VMA` 只处理地址翻译。修改可执行页权限时还要单独考虑 `SFENCE.VMA` 与远端 shootdown。[Zifencei 规范](https://docs.riscv.org/reference/isa/unpriv/zifencei.html)

实际系统应采用 W^X：先在可写映射生成代码，完成发布与同步，再切换到可执行权限，或使用受控 RW/RX 别名。不要永久使用 RWX 页。若启用 Svpbmt，PTE 内存类型还会改变缓存/顺序行为；同一物理页通过不同属性别名访问，需要依据平台规定维护一致性，不能只看虚拟地址上的 `CBO` 指令是否成功。[Svpbmt 规范](https://docs.riscv.org/reference/isa/v20240411/priv/svpbmt.html)

## 5. Demo：验证整块写零与邻块边界

本例用 `-bios none` 直接运行在 M-mode，显式启用 QEMU CPU 的 Zicbom/Zicboz。`.data` 中一个 64 字节块预填非零值，两侧邻块各放一个哨兵；`CBO.ZERO` 之后 8 个双字必须全零，两侧哨兵必须保留。再执行 `CBO.CLEAN/FLUSH`，检查数值仍为零。这能验证指令执行与块边界，**不能证明某个真实非一致性 DMA 设备看到了回写**。

保存为 `cmo_demo.S`：

```asm
    .option norelax
    .section .text.init
    .balign 4
    .globl _start
_start:
    la t0, block
    ld t1, 0(t0)
    beqz t1, fail              # prove input was not already zero
    cbo.zero (t0)
    cbo.clean (t0)
    cbo.flush (t0)
    li t1, 8
1:  ld t2, 0(t0)
    bnez t2, fail
    addi t0, t0, 8
    addi t1, t1, -1
    bnez t1, 1b
    la t0, before
    ld t1, 0(t0)
    li t2, 0x1111
    bne t1, t2, fail
    la t0, after
    ld t1, 0(t0)
    li t2, 0x2222
    bne t1, t2, fail
pass:
    li t0, 0x100000
    li t1, 0x5555
    sw t1, 0(t0)
2:  j 2b
fail:
    li t0, 0x100000
    li t1, 0x13333
    sw t1, 0(t0)
3:  j 3b

    .section .data
    .balign 64
before: .dword 0x1111
    .balign 64
block:  .fill 8, 8, 0x5555
after:  .dword 0x2222
```

保存为 `cmo_demo.ld`：

```ld
OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
    . = 0x80000000;
    .text : { *(.text.init) *(.text .text.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
}
```

构建并运行：

```bash
riscv64-unknown-elf-gcc -march=rv64i_zicsr_zicbom_zicboz -mabi=lp64 \
    -nostdlib -nostartfiles -Wl,--no-relax -T cmo_demo.ld \
    -o cmo_demo.elf cmo_demo.S
timeout 10s qemu-system-riscv64 -machine virt \
    -cpu rv64,zicbom=true,zicboz=true -bios none \
    -nographic -smp 1 -m 128M -kernel cmo_demo.elf
echo $?
```

当前 QEMU 环境退出码 `0` 表示块与哨兵检查通过；`1` 是断言失败，`124` 是超时。把 `cbo.zero (t0)` 临时改成 `nop` 时，本例退出码变为 `1`，证明检查确实依赖写零操作。这个示例显式依赖 QEMU 当前 64 字节 CBO 块；迁移前检查设备树，例如用 `-machine virt,dumpdtb=virt-cmo.dtb` 生成 DTB，再用 `dtc` 查看 `riscv,cbom-block-size` 和 `riscv,cboz-block-size`。若粒度不同，必须按实际值重排数据并改写校验循环。
