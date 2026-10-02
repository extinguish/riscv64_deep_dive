# 在 WSL2 中使用 QEMU User Mode 运行和调试 RISC-V 64 汇编程序

本文介绍如何在 x86-64 WSL2 Ubuntu 中编写、汇编、链接、运行和调试 RISC-V 64 位 Linux 用户空间程序。这里使用的是 QEMU User Mode，不涉及 `qemu-system-riscv64`、虚拟开发板、Linux 内核启动、RootFS、OpenSBI 或其他固件。

本文给出两种运行方式：

1. 显式执行 `qemu-riscv64-static ./程序`，最稳定，也最容易调试。
2. 注册 `binfmt_misc` 后直接执行 `./程序`，使用体验接近本机 ELF。

## 1. 为什么不需要 OpenSBI

本教程运行的是 RISC-V Linux 用户空间 ELF，而不是裸机程序，也不是一个完整的 RISC-V 虚拟机。

整体路径如下：

```text
RISC-V 用户程序
    │
    │ RISC-V Linux 系统调用 ABI
    ▼
QEMU User Mode
    │
    │ 翻译指令和系统调用
    ▼
WSL2 中的 Linux 内核
```

QEMU User Mode 可以在一种 CPU 架构的 Linux 上运行另一种 CPU 架构的 Linux 用户程序，并负责系统调用转换。它不启动客户机内核，也不模拟完整的开发板。相关原理和选项参见 [QEMU User Mode 官方文档](https://www.qemu.org/docs/master/user/)。

因此，本教程中的 `ecall` 表示 Linux 用户态系统调用，而不是 SBI 调用：

```text
a7      = Linux 系统调用号
a0～a5  = 系统调用参数
a0      = 系统调用返回值
```

OpenSBI 通常出现在完整系统模拟或真实硬件的 M-mode/S-mode 启动链路中，本教程没有这条启动链路。

## 2. 所需软件

在 Ubuntu/WSL2 中安装交叉工具链、QEMU User Mode、`binfmt_misc` 管理工具和 GDB：

```bash
sudo apt update
sudo apt install -y \
    binutils-riscv64-linux-gnu \
    gcc-riscv64-linux-gnu \
    libc6-dev-riscv64-cross \
    qemu-user-static \
    binfmt-support \
    gdb-multiarch
```

各软件包的作用如下：

| 软件/命令 | 用途 |
|---|---|
| `riscv64-linux-gnu-as` | 将 RISC-V 汇编源文件汇编为目标文件 |
| `riscv64-linux-gnu-ld` | 将目标文件链接成 RISC-V Linux ELF |
| `riscv64-linux-gnu-gcc` | 预处理汇编、编译 C，以及驱动链接过程 |
| `riscv64-linux-gnu-objdump` | 反汇编、查看节和机器码 |
| `riscv64-linux-gnu-readelf` | 检查 ELF 头、程序头、符号和重定位 |
| `qemu-riscv64-static` | 在 x86-64 Linux 中运行 RISC-V 64 用户程序 |
| `update-binfmts` | 管理 `binfmt_misc` 自动解释器注册 |
| `gdb-multiarch` | 调试包括 RISC-V 在内的多种目标架构 |

检查工具是否可用：

```bash
command -v riscv64-linux-gnu-as
command -v riscv64-linux-gnu-ld
command -v qemu-riscv64-static
command -v gdb-multiarch

qemu-riscv64-static --version
riscv64-linux-gnu-as --version
```

下面是撰写本文时一次 WSL2 环境检查的示例输出（Ubuntu 22.04.5 LTS）；你的系统版本和工具路径以本机检查结果为准：

```text
/usr/bin/riscv64-linux-gnu-as
/usr/bin/riscv64-linux-gnu-ld
/usr/bin/qemu-riscv64-static
```

只要所需命令可用，就可以使用下面的流程。

## 3. 最小 RISC-V 64 Hello World

将下面内容保存为 `hello.S`：

```asm
    .option nopic

    .section .rodata
msg:
    .ascii "Hello, RISC-V64!\n"
    .equ msg_len, . - msg

    .section .text
    .globl _start

_start:
    # write(1, msg, msg_len)
    li a0, 1               # fd = stdout
    la a1, msg             # 字符串地址
    li a2, msg_len         # 字符串长度
    li a7, 64              # RISC-V Linux SYS_write
    ecall

after_write:
    # exit(0)
    li a0, 0               # 返回值
    li a7, 93              # RISC-V Linux SYS_exit
    ecall
```

`after_write` 标签不会生成机器指令，只是为了后续设置调试断点。程序没有依赖 C 库，直接从 `_start` 开始执行，并通过两个 Linux 系统调用完成输出和退出。

常用的 RISC-V Linux 系统调用号包括：

| 系统调用 | `a7` | 本例参数 |
|---|---:|---|
| `write` | 64 | `a0=fd`、`a1=buffer`、`a2=count` |
| `exit` | 93 | `a0=status` |

## 4. 汇编和静态链接

使用 GNU Assembler：

```bash
riscv64-linux-gnu-as \
    -march=rv64i \
    -mabi=lp64 \
    -g \
    -o hello.o \
    hello.S
```

参数含义：

- `-march=rv64i`：只允许 RV64I 基础整数指令。
- `-mabi=lp64`：使用 64 位 long 和指针的整数 ABI。
- `-g`：写入调试信息，便于 GDB 显示源码和行号。
- `-o hello.o`：输出可重定位目标文件。

静态链接：

```bash
riscv64-linux-gnu-ld \
    -m elf64lriscv \
    -static \
    -e _start \
    -o hello \
    hello.o
```

参数含义：

- `-m elf64lriscv`：选择 64 位小端 RISC-V ELF 链接仿真。
- `-static`：生成不依赖客户机动态加载器和共享库的程序。
- `-e _start`：明确指定入口符号。

检查生成结果：

```bash
file hello
riscv64-linux-gnu-readelf -h hello
riscv64-linux-gnu-readelf -l hello
```

`file` 应显示类似：

```text
ELF 64-bit LSB executable, UCB RISC-V, statically linked
```

`readelf -h` 中值得关注的字段包括：

```text
Class:   ELF64
Data:    2's complement, little endian
Machine: RISC-V
Entry point address: ...
```

## 5. 显式使用 QEMU 运行

这是最稳定、最容易排错的运行方法：

```bash
qemu-riscv64-static ./hello
```

预期输出：

```text
Hello, RISC-V64!
```

查看退出状态：

```bash
echo $?
```

预期为：

```text
0
```

不需要 `sudo`，也不需要注册 `binfmt_misc`。只要能够直接找到 `qemu-riscv64-static`，这种方式就可以工作。

## 6. 跟踪 Linux 系统调用

QEMU User Mode 自带系统调用跟踪能力：

```bash
qemu-riscv64-static -strace ./hello
```

会看到类似：

```text
write(1,0x...,17) = 17
exit(0)
```

也可以使用环境变量：

```bash
QEMU_STRACE=1 qemu-riscv64-static ./hello
```

这对于定位下列问题很有用：

- 系统调用号是否正确。
- 文件描述符和指针参数是否正确。
- `write` 实际写出了多少字节。
- `openat`、`read`、`mmap` 等系统调用为什么失败。
- 程序是否使用预期的退出码结束。

需要注意，QEMU 的 `-strace` 是 QEMU 自己的系统调用跟踪功能，不等同于在外层直接对客户程序使用宿主机 `strace`。

## 7. 反汇编和静态检查

查看最终 ELF 的反汇编：

```bash
riscv64-linux-gnu-objdump -d hello
```

同时显示源代码、机器码和汇编：

```bash
riscv64-linux-gnu-objdump -drS hello
```

关闭伪指令别名，并使用数字寄存器名：

```bash
riscv64-linux-gnu-objdump \
    -d \
    -M no-aliases,numeric \
    hello
```

例如，源代码中的：

```asm
li a0, 1
```

可能显示为真实 RV64I 指令：

```asm
addi x10, x0, 1
```

而 `la a1, msg` 通常会根据地址和重定位形式展开成多条地址构造指令。学习 ISA 时，建议经常使用 `objdump -M no-aliases` 区分伪指令和真实机器指令。

查看符号：

```bash
riscv64-linux-gnu-readelf -s hello
```

查看节内容：

```bash
riscv64-linux-gnu-objdump -s hello
```

## 8. 使用 GDB 单步调试

QEMU User Mode 可以充当远程 GDB Server。QEMU 的 `-g endpoint` 选项会让程序在执行第一条客户机指令之前暂停，并等待 GDB 连接。

### 8.1 启动 QEMU GDB Server

在第一个 WSL2 终端中执行：

```bash
qemu-riscv64-static -g 1234 ./hello
```

此时终端暂时没有输出是正常的，因为程序正在等待调试器连接。

### 8.2 连接 GDB

在第二个 WSL2 终端中执行：

```bash
gdb-multiarch ./hello
```

进入 GDB 后连接 QEMU：

```gdb
target remote :1234
```

检查当前架构和程序计数器：

```gdb
show architecture
info registers pc
x/8i $pc
```

GDB 通常会从 ELF 自动识别 RISC-V 64。如果自动识别失败，可以手动指定：

```gdb
set architecture riscv:rv64
```

### 8.3 设置断点并运行

在 `write` 返回之后设置断点：

```gdb
break after_write
continue
```

程序会先输出：

```text
Hello, RISC-V64!
```

然后停在 `after_write`。查看系统调用返回值和参数寄存器：

```gdb
info registers a0 a1 a2 a7
```

预期：

- `a0` 是 `write` 返回值，正常情况下为写出的字节数。
- `a1` 仍然指向字符串。
- `a2` 是请求写出的长度。
- `a7` 仍然是 `64`。

查看 `a1` 指向的字符串：

```gdb
x/s $a1
```

查看附近的机器指令：

```gdb
x/10i $pc
```

### 8.4 单步执行

单步执行一条机器指令：

```gdb
stepi
```

简写为：

```gdb
si
```

执行下一条机器指令但跨过函数调用：

```gdb
nexti
```

简写为：

```gdb
ni
```

每执行一步后，可以同时检查 PC、反汇编和寄存器：

```gdb
display/i $pc
display/x $a0
display/x $a7
si
```

取消自动显示：

```gdb
undisplay
```

### 8.5 查看全部整数寄存器

```gdb
info registers
```

查看指定寄存器：

```gdb
info registers zero ra sp gp tp t0 t1 t2 s0 a0 a1 a2 a7 pc
```

GDB 同时接受 ABI 名称和 `x0～x31` 名称。例如：

```gdb
p/x $a0
p/x $x10
```

两者访问的是同一个寄存器。

### 8.6 查看内存

常用命令：

```gdb
x/s  $a1              # 按字符串查看
x/16bx $a1            # 查看 16 个字节，十六进制显示
x/4gx $sp              # 查看 4 个 64 位值
x/10i $pc              # 反汇编 10 条指令
```

其中 `x/NFU address` 的常用含义为：

- `N`：显示多少个单元。
- `F`：格式，例如 `x` 表示十六进制、`s` 表示字符串、`i` 表示指令。
- `U`：单元宽度，例如 `b` 字节、`h` 半字、`w` 32 位字、`g` 64 位 giant word。

### 8.7 结束调试

让程序继续执行到退出：

```gdb
continue
```

退出 GDB：

```gdb
quit
```

如果程序仍然处于调试状态，GDB 会询问是否结束远程目标。

## 9. 使用 QEMU 指令日志辅助调试

查看 QEMU 支持的日志项目：

```bash
qemu-riscv64-static -d help
```

记录翻译过的客户机汇编指令：

```bash
qemu-riscv64-static \
    -d in_asm \
    -D qemu.log \
    ./hello
```

同时记录客户机指令和 CPU 状态可能产生大量日志：

```bash
qemu-riscv64-static \
    -d in_asm,cpu \
    -D qemu.log \
    ./hello
```

查看日志：

```bash
less qemu.log
```

这种日志适合调查：

- 程序在哪条指令附近发生异常。
- 伪指令最终展开成了哪些机器指令。
- 跳转实际进入了哪个基本块。
- GDB 不方便介入的早期执行问题。

日志格式和可用项目会随 QEMU 版本变化，应以本机 `qemu-riscv64-static -d help` 的输出为准。

## 10. 注册 `binfmt_misc`，直接执行 `./hello`

如果希望像执行 x86-64 程序一样直接运行：

```bash
./hello
```

需要通过 Linux `binfmt_misc` 将 RISC-V ELF 自动交给 QEMU。

安装相关软件包后执行：

```bash
sudo update-binfmts --enable qemu-riscv64
```

检查注册状态：

```bash
update-binfmts --display qemu-riscv64
```

应出现：

```text
qemu-riscv64 (enabled):
```

查看内核中的注册条目：

```bash
cat /proc/sys/fs/binfmt_misc/qemu-riscv64
```

确保程序有执行权限：

```bash
chmod +x hello
```

然后直接运行：

```bash
./hello
```

预期输出：

```text
Hello, RISC-V64!
```

撰写本文时的示例环境已经安装 `qemu-user-static` 和 `binfmt-support`，但 `qemu-riscv64` 注册状态为 disabled，因此当时需要执行上面的启用命令。你的环境应先检查注册状态；只有显示 disabled 时才需要启用。

### 10.1 `binfmt_misc` 尚未挂载

检查挂载状态：

```bash
mount | grep binfmt_misc
```

如果没有结果，可以执行：

```bash
sudo mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
sudo update-binfmts --enable qemu-riscv64
```

然后再次检查：

```bash
cat /proc/sys/fs/binfmt_misc/qemu-riscv64
```

## 11. 让 WSL2 重启后自动注册

`binfmt_misc` 的内核注册会随着 WSL2 实例停止而消失。配置文件仍然存在，但新实例启动时需要服务重新向内核注册。

### 11.1 WSL2 已启用 systemd

执行：

```bash
sudo systemctl enable --now binfmt-support
sudo systemctl restart binfmt-support
```

检查：

```bash
systemctl status binfmt-support
update-binfmts --display qemu-riscv64
```

### 11.2 在 WSL2 中启用 systemd

编辑 `/etc/wsl.conf`：

```ini
[boot]
systemd=true
```

然后在 Windows PowerShell 中执行：

```powershell
wsl.exe --shutdown
```

重新进入 WSL2，再执行：

```bash
sudo systemctl enable --now binfmt-support
sudo update-binfmts --enable qemu-riscv64
```

WSL2 对 systemd 的支持和配置方式参见 [Microsoft WSL 官方文档](https://learn.microsoft.com/en-us/windows/wsl/systemd)。

### 11.3 不启用 systemd

不使用 systemd 也不影响显式执行：

```bash
qemu-riscv64-static ./hello
```

如果仍希望直接执行，可以在每次新的 WSL2 实例启动后运行：

```bash
sudo service binfmt-support restart
```

或者：

```bash
sudo update-binfmts --enable qemu-riscv64
```

## 12. 静态程序和动态程序的区别

对于本教程这种直接使用系统调用的纯汇编程序，建议静态链接：

```bash
riscv64-linux-gnu-ld -static -o hello hello.o
qemu-riscv64-static ./hello
```

它不需要任何 RISC-V 动态加载器和共享库。

如果以后使用 C 和 glibc 生成动态程序，例如 `hello.c`：

```c
#include <stdio.h>

int main(void)
{
    puts("Hello from RISC-V C!");
    return 0;
}
```

动态编译：

```bash
riscv64-linux-gnu-gcc -O2 -g -o hello_c hello.c
```

运行时要通过 `-L` 指定 RISC-V sysroot，让 QEMU 找到客户机动态加载器和共享库：

```bash
qemu-riscv64-static \
    -L /usr/riscv64-linux-gnu \
    ./hello_c
```

QEMU 官方命令行中的 `-L path` 就是客户机 ELF 解释器前缀。也可以使用对应环境变量：

```bash
QEMU_LD_PREFIX=/usr/riscv64-linux-gnu \
    qemu-riscv64-static ./hello_c
```

直接静态编译 C 程序则不需要 `-L`：

```bash
riscv64-linux-gnu-gcc \
    -static \
    -O2 \
    -g \
    -o hello_c_static \
    hello.c

qemu-riscv64-static ./hello_c_static
```

需要特别注意：

```text
qemu-riscv64-static
```

名称里的 `static` 指 QEMU 模拟器自身是静态链接的，不代表被运行的 RISC-V 程序一定是静态链接的。这两件事彼此独立。

## 13. 为什么应使用 `riscv64-linux-gnu-*`

本教程需要生成遵循 RISC-V Linux 用户态 ABI 的 ELF，所以推荐使用：

```text
riscv64-linux-gnu-as
riscv64-linux-gnu-ld
riscv64-linux-gnu-gcc
```

不要把它与常见的裸机工具链混淆：

```text
riscv64-unknown-elf-*
```

裸机工具链本身也能生成 RISC-V 指令和 ELF，但它通常面向没有 Linux 内核的执行环境，默认运行库、链接脚本和系统调用模型可能完全不同。如果使用裸机工具链又在源码中手工遵循 Linux ABI，经过仔细链接也可能运行，但不适合作为这个用户态学习环境的默认方案。

## 14. 常见错误及排查方法

### 14.1 `Exec format error`

错误类似：

```text
bash: ./hello: cannot execute binary file: Exec format error
```

通常表示 `binfmt_misc` 没有注册或没有启用。先尝试显式运行：

```bash
qemu-riscv64-static ./hello
```

如果显式运行正常，再检查：

```bash
update-binfmts --display qemu-riscv64
ls /proc/sys/fs/binfmt_misc
cat /proc/sys/fs/binfmt_misc/qemu-riscv64
```

必要时重新启用：

```bash
sudo update-binfmts --enable qemu-riscv64
```

### 14.2 找不到动态加载器

错误类似：

```text
Could not open '/lib/ld-linux-riscv64-lp64d.so.1'
```

说明程序是动态链接的，而 QEMU 没找到 RISC-V 动态加载器。使用：

```bash
qemu-riscv64-static \
    -L /usr/riscv64-linux-gnu \
    ./hello
```

或者重新静态链接程序。

可以检查 ELF 请求的解释器：

```bash
riscv64-linux-gnu-readelf -l hello | grep interpreter
```

### 14.3 `Permission denied`

先检查执行权限：

```bash
ls -l hello
chmod +x hello
```

还要检查程序所在文件系统是否以 `noexec` 挂载。建议把源码和生成物放在 WSL2 的 Linux 文件系统中，例如：

```text
~/riscv_asm_lab/
```

而不是优先放在 `/mnt/c` 下，这也有利于获得更好的构建性能和更一致的 Linux 权限语义。

### 14.4 `Illegal instruction`

可能原因是程序使用了当前 QEMU CPU 模型或版本不支持的扩展。初学阶段先限制到基础指令集：

```bash
-march=rv64i -mabi=lp64
```

查看 QEMU 支持的 CPU 模型：

```bash
qemu-riscv64-static -cpu help
```

确认 ELF 的架构属性和反汇编：

```bash
riscv64-linux-gnu-readelf -A hello
riscv64-linux-gnu-objdump -d hello
```

对于较新的 `B`、`V` 或其他扩展，需要确认交叉工具链和 QEMU 都支持相应版本。

### 14.5 程序没有输出但退出了

使用 QEMU 系统调用跟踪：

```bash
qemu-riscv64-static -strace ./hello
```

重点检查：

- `a7` 是否为正确的系统调用号。
- `a0` 是否为正确的文件描述符。
- `a1` 是否为有效的用户空间地址。
- `a2` 是否为正确的长度。
- `write` 返回值是否为负数。

### 14.6 程序崩溃或发生段错误

先保留调试信息重新构建：

```bash
riscv64-linux-gnu-as -g -march=rv64i -mabi=lp64 -o hello.o hello.S
riscv64-linux-gnu-ld -m elf64lriscv -static -e _start -o hello hello.o
```

然后通过 GDB 定位：

```bash
qemu-riscv64-static -g 1234 ./hello
```

另一个终端：

```bash
gdb-multiarch ./hello
```

GDB 中执行：

```gdb
target remote :1234
display/i $pc
info registers
si
```

必要时结合：

```bash
qemu-riscv64-static -d in_asm,cpu -D qemu.log ./hello
```

## 15. 推荐的日常开发流程

学习阶段建议固定使用下面的循环：

```text
编辑 hello.S
    │
    ▼
riscv64-linux-gnu-as -g
    │
    ▼
riscv64-linux-gnu-ld -static
    │
    ├── objdump/readelf 静态检查
    │
    ├── qemu-riscv64-static -strace 观察系统调用
    │
    └── qemu-riscv64-static -g 1234 + gdb-multiarch 调试
```

完整命令可以归纳为：

```bash
# 构建
riscv64-linux-gnu-as \
    -g -march=rv64i -mabi=lp64 \
    -o hello.o hello.S

riscv64-linux-gnu-ld \
    -m elf64lriscv -static -e _start \
    -o hello hello.o

# 检查
file hello
riscv64-linux-gnu-readelf -h hello
riscv64-linux-gnu-objdump -drS hello

# 普通运行
qemu-riscv64-static ./hello

# 系统调用跟踪
qemu-riscv64-static -strace ./hello

# 等待 GDB 调试
qemu-riscv64-static -g 1234 ./hello
```

如果已经配置好 `binfmt_misc`，普通运行也可以写成：

```bash
./hello
```

但构建脚本、CI 和故障排查中仍建议显式写出：

```bash
qemu-riscv64-static ./hello
```

这样不依赖当前 WSL2 实例是否已经注册 `binfmt_misc`。

## 16. 本篇小结

对于 WSL2 中的简单 RISC-V 64 用户空间汇编程序，最小执行环境只有三层：

```text
RISC-V Linux ELF
    │
    ▼
qemu-riscv64-static
    │
    ▼
WSL2 Linux 内核
```

关键结论如下：

1. 不需要 OpenSBI，也不需要启动 RISC-V Linux 内核。
2. 程序必须按 RISC-V Linux 用户态 ABI 组织系统调用。
3. 纯汇编实验优先使用静态链接，避免动态库和 sysroot 干扰。
4. 显式运行 `qemu-riscv64-static ./程序` 最可靠。
5. 注册 `binfmt_misc` 后可以直接运行 `./程序`。
6. `-strace` 用于观察系统调用，`-g 1234` 配合 `gdb-multiarch` 用于指令级调试。
7. `objdump -M no-aliases` 能帮助区分伪指令和真实 ISA 指令。

这套环境非常适合作为后续 RV64I、M、A、F/D、C 等指令实验的统一运行和调试基础。
