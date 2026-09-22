# RISC-V 汇编实验环境（RV64 裸机）

## 正式 Labs

从 Staff Engineer 题库扩展的独立实验位于 [`labs/`](labs/README.md)。当前已经建立：

- [`Lab 01：RV64 汇编运行库、psABI 与 ELF`](labs/lab01-asm-runtime-abi-elf/README.md)
- [`Lab 02：多 Hart Reset、Early Boot 与自重定位`](labs/lab02-multihart-early-boot/README.md)

根目录下原有的 `memcpy` demo 继续保留，作为最小工具链检查示例。

这是一个可在本机运行的最小 RISC-V 汇编实验环境。它使用已安装的
RISC-V 裸机交叉编译器和 QEMU 的 `virt` 开发板模型，不需要目标硬件、
Linux rootfs 或 C 标准库。

## 前置工具

需要以下两个命令在 `PATH` 中：

```sh
riscv64-unknown-elf-gcc
qemu-system-riscv64
```

检查环境：

```sh
make toolcheck
```

## 构建并运行

```sh
make run
```

该命令会：

1. 把 `start.S`、`memcpy.S` 和 `test_memcpy.c` 交叉编译为 RV64 裸机 ELF。
2. 用 QEMU 启动 ELF。
3. 由 `test_memcpy.c` 验证复制结果、返回值和零长度复制。
4. 测试通过时 QEMU 以成功状态退出，并显示 `PASS`。

## 汇编 memcpy

`memcpy.S` 中的接口是：

```c
void *asm_memcpy(void *dst, const void *src, unsigned long count);
```

它按 RISC-V ABI 接收 `a0=dst`、`a1=src`、`a2=count`，逐字节复制，并在
`a0` 返回原始 `dst`。和标准 `memcpy` 一样，源和目的区域不能重叠；有重叠
的场景应实现 `memmove`。

## 常用命令

```sh
make              # 仅构建 build/memcpy_demo.elf
make run          # 在 QEMU 中运行验证
make disasm       # 查看带源码的反汇编（按 q 退出）
make clean        # 删除可重新生成的 build/ 目录
```

## 文件说明

- `memcpy.S`：待学习和修改的 RISC-V 汇编函数。
- `test_memcpy.c`：无 libc 的测试代码。
- `start.S`：设置栈、清零 `.bss`，并把测试结果报告给 QEMU。
- `linker.ld`：QEMU `virt` 的裸机内存布局。
- `Makefile`：构建、仿真和检查入口。
