# Chapter 03 — RISC-V OP-TEE Development Environment

RISC-V OP-TEE Architecture & Porting · 系统性深度教程 · Chapter 03

本章目标：在 Windows + WSL2 环境中，建立一套能够编译、启动、测试、调试和修改 RISC-V OP-TEE 的完整开发平台，并为后续的 OpenSBI Domain、Context Switch、Trap、MMU 和 SMP 源码研究打好基础。

本章并不只是介绍如何执行 `make` 命令。真正重要的是理解整个 Firmware/OS Development Workflow，达到能够独立定位构建问题、启动问题、链接问题和跨特权级执行问题的程度。

## 3.0 本章学习目标与技术路线

完成本章后，你应该能够：

1. 理解 RISC-V OP-TEE 涉及的多个代码仓库及其依赖关系。
2. 区分 OP-TEE upstream、RISE 开发分支和正式集成环境。
3. 在 WSL2 中建立完整的 RISC-V QEMU + OpenSBI + OP-TEE + Linux 开发环境。
4. 使用 GNU GCC/Binutils 和 LLVM/Clang 构建并分析 OP-TEE。
5. 从实际运行的 QEMU 启动脚本理解 Firmware、Device Tree、Linux Kernel 和 RootFS 的装载过程。
6. 在 GDB 中调试 RISC-V M-mode、S-mode、U-mode 的执行。
7. 分析 ELF、Linker Script、Section、Symbol、Relocation 和内存布局。
8. 建立可复现的代码、工具链、配置、测试记录体系。

本章采用两条并行开发路线。

双轨开发环境

Track A — 完整系统开发

Buildroot → OpenSBI + OP-TEE + U-Boot + Linux + RootFS → QEMU → xtest

用于集成、启动、运行时调试及端到端验证。

Track B — 独立组件开发

optee_os / OpenSBI → Make + Cross Toolchain → ELF / Disassembly / Map

用于源码修改、快速编译、架构研究和单组件问题定位。

为什么需要两条路线？

假设你修改了 `core/arch/riscv/kernel/entry.S`：

- Track B 可以快速验证汇编是否正确、编译是否通过、ELF 是否符合预期。
- Track A 则可以真正启动整个系统，验证这个修改是否破坏 OP-TEE Boot、Domain Switching 或 TA Execution。

单独编译成功，不代表集成后的 Firmware 能够正确运行。

## 3.1 OP-TEE 多仓库结构与设计理念

### 3.1.1 为什么 OP-TEE 不是一个独立的软件仓库？

OP-TEE 是一个 Trusted Execution Environment，但它并不是一个能够脱离其他 Firmware 和 OS 独立提供完整功能的系统。

一个实际的 RISC-V TEE 产品需要至少三个层面的软件：

- M-mode Firmware：建立并维护系统最底层的安全隔离和运行环境。
- TEE OS：运行 Trusted Application，并提供 Trusted Services。
- REE Software：Linux Driver、用户态 Client 和 RPC 服务。

因此，仅有 `optee_os` 并不能构建完整可运行的 Linux + TEE 系统。

### 3.1.2 完整软件栈

```
Host Development Environment (WSL2 / x86_64)
|
+-- Buildroot / Build Scripts
|       |
|       +-- RISC-V Cross Toolchain
|       |
|       +-- OpenSBI
|       |      M-mode firmware
|       |      Domain / SBI / Isolation
|       |
|       +-- OP-TEE OS
|       |      TEE Core
|       |      Trusted Thread
|       |      TA Loader / Syscall
|       |
|       +-- U-Boot
|       |      Bootloader
|       |
|       +-- Linux Kernel
|       |      TEE Driver
|       |      RPMI / MPXY integration
|       |
|       +-- optee_client
|       |      libteec
|       |      tee-supplicant
|       |
|       +-- optee_test
|       |      xtest
|       |      Test TAs
|       |
|       +-- Buildroot RootFS
|
+-- QEMU System Emulator
        |
        +-- RISC-V Virtual Hardware
        +-- Emulated Harts
        +-- Interrupt Controllers
        +-- Memory / UART / Devices
```

### 3.1.3 核心仓库职责

| 仓库             | 核心职责                           | 学习重点                        |
| -------------- | ------------------------------ | --------------------------- |
| `optee_os`     | OP-TEE Core 和 TA Runtime       | TEE 内核与 RISC-V Architecture |
| `optee_client` | CA API、libteec、tee-supplicant  | REE 用户态通信与 RPC              |
| `optee_test`   | xtest、测试 CA、测试 TA              | 正确性与回归验证                    |
| `opensbi`      | M-mode Firmware 和 SBI Services | Domain、Trap、PMP、Context     |
| Linux          | 调度、TEE Driver、设备和内存            | REE/TEE Transport           |
| U-Boot         | 后续阶段镜像加载及启动                    | Boot Chain / FDT            |
| Buildroot      | 软件包编排、构建、RootFS 和镜像            | End-to-end Integration      |
| QEMU           | 虚拟 SoC / Harts / 外设            | Debugging / Validation      |

其中最容易混淆的有两个：

`optee_os` 和 `optee_client` 的区别

`optee_os` 运行在 TEE 中，而 `optee_client` 的 libteec 和 tee-supplicant 运行在 REE 用户空间。二者处于不同的安全边界。

`optee_test` 和普通单元测试的区别

`optee_test` 包含运行在 Linux 用户空间的测试程序，也包含运行在 TEE 内的测试 TA，因此它可以验证一次真实的 CA → Kernel Driver → TEE Core → TA 调用链。

但并非所有 xtest 用例都验证 OpenSBI 或 PMP 的隔离正确性。低层安全边界仍需额外设计测试。

### 3.1.4 主要官方代码仓库

- [OP-TEE OS](https://github.com/OP-TEE/optee_os?utm_source=chatgpt.com)
- [OP-TEE Client](https://github.com/OP-TEE/optee_client?utm_source=chatgpt.com)
- [OP-TEE Test](https://github.com/OP-TEE/optee_test?utm_source=chatgpt.com)
- [OpenSBI](https://github.com/riscv-software-src/opensbi?utm_source=chatgpt.com)
- [OP-TEE Build](https://github.com/OP-TEE/build?utm_source=chatgpt.com)

注意：官方 `OP-TEE/build` 和 RISE 使用的 Buildroot 集成仓库不是同一个工程。 前者是 OP-TEE 的通用构建基础设施，后者为当前 RISC-V OP-TEE 研究提供完整集成和特殊 Firmware/Kernel 分支。

## 3.2 Upstream、RISE 分支与版本管理

这是进行 RISC-V OP-TEE 开发时必须首先建立的一个工程意识。

### 3.2.1 为什么不能简单地使用所有仓库的 master？

传统软件开发中，我们通常习惯：

```
git clone repository
git checkout master
```

但在底层 Firmware/OS 集成中，多个子系统之间可能存在尚未稳定的接口。

例如：

```
Linux TEE Driver
     |
     | RPMI TEE protocol version A
     v
OpenSBI
     |
     | SBI MPXY implementation
     v
OP-TEE
     |
     | TEE service ABI
     v
Trusted Application
```

如果 Linux 使用一套协议实现，而 OpenSBI/OP-TEE 使用另一套开发中的实现，就可能出现：

- Linux TEE Driver Probe 失败。
- OpenSBI 不识别 SBI Extension 或 Function ID。
- Shared Memory 参数格式不匹配。
- TEE Call 进入后无法正常返回。
- xtest 出现失败或系统挂起。

这些不一定是 OP-TEE Core 本身的问题，而可能是仓库版本不兼容。

### 3.2.2 当前建议使用的两类代码基线

| 基线                        | 用途                | 建议                 |
| ------------------------- | ----------------- | ------------------ |
| OP-TEE / OpenSBI Upstream | 长期源码研究、可合并的 Patch | 跟踪 upstream，使用单独分支 |
| RISE RP016 Buildroot      | 完整系统搭建、端到端调试      | 首先选择文档明确对应的集成分支    |

RISE RP016 使用 Buildroot 统一管理 OpenSBI、OP-TEE、U-Boot、Linux、QEMU 和交叉工具链。官方提供 `rp016_m1` 的完整构建及 xtest 操作流程。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



RISE RP022 则是在 upstream RV64 基础上继续增强 Compiler Support、ASLR、CFI、FP/Vector Context 和内存权限。相关功能处于不同完成或评审阶段，不应默认所有 milestone 都已合入同一个稳定版本。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### 3.2.3 推荐工作模式

建议建立两套独立目录：

```
~/work/tee/
|
+-- integrated/
|   |
|   +-- buildroot-rp016/
|       |
|       +-- output/
|       +-- configs/
|
+-- upstream/
    |
    +-- optee_os/
    +-- opensbi/
    +-- optee_client/
    +-- optee_test/
```

`integrated` 用于运行一套已验证兼容的完整系统；`upstream` 用于跟踪源码、研究社区 Patch 和创建个人开发分支。

后续如需将 upstream Patch 应用到 Buildroot 集成环境，再进行有控制的移植或 package override。

这样可以避免一个常见问题：

> 为了研究某个新的 RISC-V Patch，直接替换掉整套可运行环境中的 OP-TEE Core，结果破坏版本兼容性，又失去原本可运行的调试基线。

# 3.3 WSL2 + QEMU + Buildroot 开发环境搭建

## 3.3.1 为什么推荐 WSL2？

你的主要开发环境是 Windows，而 OP-TEE、OpenSBI 和 Buildroot 都以 Linux 构建环境为主要目标。

WSL2 的优势在于：

- 可以使用 Linux 原生构建工具、Shell 和文件系统。
- 不需要实体 RISC-V 开发板。
- QEMU 可以在 x86-64 主机上通过 TCG 模拟 RISC-V 指令和平台硬件。
- 可以直接使用 GDB、Binutils、Device Tree Compiler。
- Windows 上可以使用 VS Code、Git 和其他源码浏览工具。

但需要正确理解 WSL2 与 QEMU 的关系：

```
Windows Host
    |
    +-- WSL2 Linux VM
            |
            +-- Buildroot
            |
            +-- RISC-V Cross Compiler
            |
            +-- qemu-system-riscv64
                    |
                    +-- Emulated RV64 Harts
                    +-- Emulated Memory
                    +-- Emulated Interrupt Controllers
                    +-- OpenSBI / OP-TEE / Linux
```

这里存在两层不同的虚拟化/模拟：

WSL2 运行的是 x86-64 Linux 环境；QEMU TCG 则将 RISC-V Guest 指令模拟在 x86-64 Host 上运行。

因此，WSL2 不是直接运行 RISC-V Linux，运行 RISC-V 的是 QEMU。

## 3.3.2 主机资源建议

以下是方便调试与编译的建议配置，并非官方最低要求。

| 资源               | 推荐配置                     | 说明                       |
| ---------------- | ------------------------ | ------------------------ |
| Windows          | Windows 11 + WSL2        | Windows 10 支持 WSL2 的版本也可 |
| WSL Distribution | Ubuntu 22.04 / 24.04 LTS | 使用 Linux 原生构建环境          |
| Host RAM         | 16 GB 以上                 | 编译 LLVM/Buildroot 时更多更好  |
| WSL 可用 RAM       | 8–12 GB 以上               | 避免并行构建 OOM               |
| CPU              | 8 个逻辑核心左右                | 视内存调整并行度                 |
| 可用磁盘             | 80–120 GB                | 保留多套构建输出和调试文件            |
| 文件系统             | WSL2 ext4                | 不推荐在 `/mnt/c` 下进行完整构建    |

最后一项尤其重要。

应当使用：

```
cd ~
mkdir -p ~/work/tee
```

而不是：

```
cd /mnt/c/Users/...
```

因为 Buildroot 会产生大量小文件、Symbolic Link 和临时编译文件。在 Windows 挂载目录中构建，可能遇到性能及文件系统语义问题。

### WSL2 资源限制

若需要配置 WSL2 资源，可以在 Windows 用户目录的 `.wslconfig` 中使用类似设置：

```
[wsl2]
memory=12GB
processors=8
swap=8GB
```

修改后，在 Windows PowerShell 中执行：

```
wsl --shutdown
```

重新进入 WSL2 后即可使用新配置。

注意：不要让 WSL2 分配的内存过大，导致 Windows Host 本身内存不足。

## 3.3.3 安装基础开发工具

以下命令在 Ubuntu 22.04/24.04 作为建议安装清单：

```
sudo apt update

sudo apt install -y \
    build-essential \
    git \
    curl wget \
    ca-certificates \
    python3 \
    python3-dev \
    python3-pip \
    python3-venv \
    python3-cryptography \
    python3-pyelftools \
    bc bison flex \
    libssl-dev \
    libelf-dev \
    libncurses-dev \
    zlib1g-dev \
    libglib2.0-dev \
    cpio rsync unzip \
    file xz-utils \
    pkg-config \
    cmake ninja-build \
    device-tree-compiler \
    gdb-multiarch \
    gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu \
    qemu-system-misc \
    inetutils-telnet \
    ccache
```

这里需要区分三类工具。

| 工具                         | 执行位置        | 作用                 |
| -------------------------- | ----------- | ------------------ |
| `gcc` / `make` / `python3` | x86-64 Host | 构建和管理软件            |
| `riscv64-linux-gnu-gcc`    | x86-64 Host | 生成 RV64 机器码        |
| `qemu-system-riscv64`      | x86-64 Host | 模拟 RV64 系统         |
| `gdb-multiarch`            | x86-64 Host | 调试 RV64 Guest      |
| `dtc`                      | x86-64 Host | 编译和反编译 Device Tree |

为什么需要 `gdb-multiarch`？

因为你在 x86-64 上运行 GDB，但要调试的目标架构是 RISC-V。普通 x86-64 GDB 未必包含对 RISC-V Target 的完整支持。

验证基本环境：

```
uname -m

riscv64-linux-gnu-gcc --version

riscv64-linux-gnu-objdump --version

gdb-multiarch --version

qemu-system-riscv64 --version

dtc --version
```

其中：

```
uname -m
    x86_64
```

这是预期结果。

你现在是在 Host 上执行交叉编译，并不是直接进入 RISC-V Guest。

还需要注意，安装发行版 QEMU 并不意味着它可以运行 RISE 环境所需的全部扩展。对于实际的完整系统，应优先使用集成工程生成的 QEMU，而不是擅自替换为系统安装的 QEMU。

# 3.4 建立完整 RISC-V OP-TEE 系统

## 3.4.1 为什么选择 Buildroot？

Buildroot 的核心作用可以概括为：

将多个相互依赖的源码仓库、工具链、构建配置及最终镜像，组织成一个可重复执行的 Build Pipeline。

例如：

```
                  Buildroot
                      |
       +--------------+--------------+
       |              |              |
   Firmware        Kernel         User Space
       |              |              |
    OpenSBI         Linux         libteec
    OP-TEE                         xtest
    U-Boot                         RootFS
       |              |              |
       +--------------+--------------+
                      |
                Image Packaging
                      |
                 QEMU Startup
```

如果不使用 Buildroot，你需要手动解决所有组件的依赖关系、构建顺序、交叉编译参数，以及生成镜像的打包问题。

对于研究 OpenSBI 和 OP-TEE 内部机制来说，这些工作并不是我们的主要目标。

所以第一次完整集成，先采用官方已提供的 Buildroot 工程，不要从零手动拼装 Linux、OpenSBI 和 OP-TEE。

## 3.4.2 获取 RP016 Buildroot

本章选择 `rp016_m1` 作为第一次完整运行的基线，因为它有明确的公开构建、启动及 xtest 操作流程。它是教学基线，不代表 RP016 的最新功能全集。

```
mkdir -p ~/work/tee/integrated

cd ~/work/tee/integrated

git clone \
  https://gitlab.com/riseproject/riscv-optee/buildroot.git \
  -b rp016_m1 \
  buildroot-rp016

cd buildroot-rp016
```

确认 Git 状态：

```
git branch --show-current
git log -1 --oneline
git rev-parse HEAD
```

这里特别提醒：`rp016_m1` 表示 Milestone 1 对应的集成基线，并不是一个可以随意和最新 Linux/OpenSBI master 混用的版本组合。

后续学习 Domain Interrupt、Hardware Isolation 和 FP/Vector 时，我们会根据对应 milestone 使用单独的集成目录。

## 3.4.3 配置目标平台

```
make qemu_riscv64_virt_optee_defconfig
```

这个命令的本质不是进行编译，而是加载一套事先配置好的 Buildroot 参数。

可以理解为：

```
configs/qemu_riscv64_virt_optee_defconfig
                     |
                     v
             Buildroot Kconfig
                     |
                     v
                  .config
                     |
       +-------------+-------------+
       |             |             |
   Packages      Toolchain      Architecture
```

执行后：

```
grep -E \
  'BR2_riscv|BR2_RISCV|BR2_TARGET|BR2_PACKAGE_OPTEE' \
  .config | head -80
```

观察实际启用的平台、固件和软件包配置。

`make menuconfig` 可以进一步修改选项，但初次构建不要修改默认配置。

原因是我们首先需要确认官方集成基线是否能够完整构建，不能把环境自身的问题与个人配置修改引起的问题混在一起。

## 3.4.4 构建完整系统

按照 RP016 的公开构建说明：

```
make -j"$(nproc)"
```

如果你的 WSL2 内存有限，建议先降低并行度：

```
make -j4
```

RISE 文档还针对 Host CMake 版本较新的环境给出了兼容参数。例如：

```
make -j4 CMAKE_POLICY_VERSION_MINIMUM=3.5
```

是否需要此参数取决于实际 CMake 版本和构建失败原因，不建议无条件加入。

官方 RP016 构建文档确认 Buildroot 会获取并编译相应的交叉工具链及各系统组件。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



### 需要注意 Buildroot 的并行构建机制

Buildroot 的 `BR2_JLEVEL` 主要控制单个软件包内部的编译并行度，而顶层 `make -jN` 的并行编排与 `BR2_PER_PACKAGE_DIRECTORIES` 等配置有关。

Buildroot 手册明确提醒：顶层并行构建存在额外的依赖与兼容性要求。

[image](https://www.google.com/s2/favicons?domain=https://buildroot.org\&sz=32)

Buildroot



对于首次构建：

- 按当前集成分支文档使用 `make -jN`。
- 遇到不稳定的并行失败时，降低并行度。
- 若怀疑 Buildroot 并行依赖问题，可使用单作业顶层构建重新验证。

## 3.4.5 Buildroot 输出目录内部结构

完成构建后，重点检查：

```
ls -lh output/

ls -lh output/images/

ls output/build/ | head -50

ls output/host/bin/ | grep riscv
```

典型 Buildroot Output Layout：

```
output/
|
+-- build/
|    |
|    +-- opensbi-<version>/
|    +-- optee-os-<version>/
|    +-- linux-<version>/
|    +-- uboot-<version>/
|    +-- ...
|
+-- host/
|    |
|    +-- bin/
|    +-- lib/
|    +-- ...
|
+-- staging/
|
+-- target/
|
+-- images/
     |
     +-- Firmware Images
     +-- Kernel Image
     +-- Root Filesystem
     +-- QEMU Startup Scripts
```

上述目录是 Buildroot 的通用布局，具体包名与文件名以当前分支实际输出为准。

必须理解这几个目录的差异。

| 目录               | 意义                            | 是否直接运行       |
| ---------------- | ----------------------------- | ------------ |
| `output/build`   | 软件源码与编译目录                     | 通常不是最终镜像     |
| `output/host`    | Host 工具、交叉工具链及开发 Sysroot      | 用于构建和调试      |
| `output/staging` | Target 开发依赖和头文件               | 不作为最终 RootFS |
| `output/target`  | RootFS 的未打包目录树                | 通常不是最终可启动镜像  |
| `output/images`  | 生成的 Firmware、Kernel、RootFS 镜像 | 用于启动或部署      |

非常重要：`output/target` 不是可以直接部署的完整根文件系统镜像。

它可能不具备最终文件系统所需的真实权限、设备节点或镜像结构。部署时应使用 `output/images` 中经构建系统生成的产物。

# 3.5 启动 OpenSBI + OP-TEE + Linux

## 3.5.1 启动 QEMU

在 Buildroot 根目录下执行：

```
./output/images/start-qemu.sh
```

RP016 文档使用这个脚本启动 QEMU，并指出该配置的一个重要特点：

Linux/REE 的串口和 OP-TEE 的调试串口可能分别显示在不同终端中。

在公开的 `rp016_m1` 运行说明中，启动脚本会等待客户端连接 TCP 端口 `64320`。

打开第二个 WSL2 Terminal：

```
telnet localhost 64320
```

连接成功后，可以观察 OpenSBI、U-Boot 和 Linux 启动信息；原来的终端可观察 OP-TEE 日志。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



这里的 `64320` 是该示例配置中的端口，不是 OP-TEE 或 QEMU 的固定标准端口。

后续切换集成分支时，以对应 `start-qemu.sh` 中的串口配置为准。

## 3.5.2 正确理解三个启动日志阶段

第一次运行时，我建议你不要急于执行 xtest，而是尝试把日志与实际的 Firmware 执行过程对应起来。

### Stage A — OpenSBI

典型信息可能包含：

```
OpenSBI v...
Platform Name ...
Firmware Base ...
Firmware Size ...
Runtime SBI Version ...
```

这说明 M-mode Firmware 已经执行。

但请注意：

打印了 OpenSBI 版本，不等于 Domain Isolation 已经完成配置。

真正要验证隔离是否有效，还需要检查 Domain 相关日志、配置及后续硬件权限测试。

### Stage B — OP-TEE

你需要观察：

- OP-TEE Core 是否进入启动流程。
- MMU 是否初始化。
- Trusted Thread 相关初始化是否完成。
- TA Runtime 是否准备就绪。
- 是否发生 Panic、Page Fault 或其他异常。

不同构建版本输出的具体信息会不同，因此不要将某一条固定日志作为唯一判断条件。

### Stage C — U-Boot / Linux

可能看到：

```
U-Boot ...
Starting kernel ...

Linux version ...
...
```

这些说明 Linux 启动链已经执行。

但是，Linux 正常启动也不能证明 TEE 功能正常。

最终还需要通过：

```
Linux CA
   |
   v
TEE Driver
   |
   v
OpenSBI / RPMI
   |
   v
OP-TEE
   |
   v
Trusted Application
```

对整条调用链进行测试。

## 3.5.3 Linux 中验证 TEE

在 Guest Linux 中：

```
uname -a
```

查看 TEE 设备：

```
ls -l /dev/tee*
```

如果 Guest 包含相关工具，可以检查：

```
dmesg | grep -iE 'optee|tee|rpmi|mpxy'
```

以及：

```
ps | grep tee-supplicant
```

这里需要做几项区分：

- 存在 `/dev/tee0`，说明 Linux TEE Framework 暴露了一个 TEE 设备，但不能单凭它证明所有 TA 功能正常。
- `tee-supplicant` 运行正常，不等于 Secure Storage 一定可用。
- `xtest` 能运行，不等于所有安全隔离机制都已经过验证。

## 3.5.4 运行 xtest

在 Linux Guest 中执行：

```
xtest
```

对于 `rp016_m1`，RISE 已提供了此测试流程。

[image](https://www.google.com/s2/favicons?domain=https://riscv-optee-docs-c9fea0.gitlab.io\&sz=32)

RISC-V OP-TEE



测试覆盖范围取决于构建时包含的测试 TA、软件功能和平台配置。

建议将测试结果划分为：

| 测试类别              | 验证目标                     |
| ----------------- | ------------------------ |
| TEE Communication | CA → TEE Core 的调用能力      |
| TA Lifecycle      | TA Open / Invoke / Close |
| Memory            | 参数、Shared Memory 与访问边界   |
| Cryptography      | TEE Crypto Service       |
| Storage           | Secure Storage 相关能力      |
| Regression        | 修改内核后是否引入功能退化            |

成功运行 `xtest` 只能说明已执行的测试通过，不能自动推广为整个 TEE 安全架构已得到验证。

尤其是 PMP、DMA、Domain Context 和多 hart 权限状态，需要在后续章节设计专门的安全测试。

# 3.6 GNU / LLVM Toolchain 与 OP-TEE Build System

接下来进入比环境搭建更重要的部分：理解 OP-TEE 到底是怎样被编译出来的。

## 3.6.1 Host、Build、Target 的区别

对于目前这套环境：

```
Build Machine:   x86_64 Linux (WSL2)
Host Tools:      x86_64 ELF
Target Machine:  riscv64
Target Output:   RISC-V ELF / Firmware / TA
```

假设执行：

```
riscv64-linux-gnu-gcc hello.c -o hello
```

生成的不是 x86-64 程序，而是 RISC-V 程序。

通过：

```
file hello
readelf -h hello
```

可以验证目标架构。

虽然用户态 Linux 程序、OpenSBI 和 OP-TEE 都可能是 RISC-V ELF，但它们的运行环境不相同。

| 组件          | 运行特权级              | 运行时依赖              |
| ----------- | ------------------ | ------------------ |
| Linux CA    | U-mode             | Linux、libc、动态链接器   |
| OpenSBI     | M-mode             | Firmware Runtime   |
| OP-TEE Core | S-mode             | TEE Kernel Runtime |
| User TA     | U-mode（TEE Domain） | OP-TEE TA Runtime  |

不能认为只要 ELF Architecture 都是 `RISC-V`，它们就能够相互替换。

## 3.6.2 GNU GCC 和 Binutils

GNU Toolchain 的几个重要工具：

```
gcc         C Compiler Driver
as          Assembler
ld          Linker
objdump     Disassembler / Object Inspector
readelf     ELF Structure Inspector
nm          Symbol Inspector
objcopy     Binary Conversion
addr2line   Address to Source Mapping
```

你应该区分：

```
C Source
   |
   | gcc -c
   v
Object File (.o)
   |
   | ld / gcc linking
   v
ELF
   |
   | objcopy (when required)
   v
Binary Image
```

这里的 ELF 和 Binary Image 不是同一种文件格式。

ELF 包含机器码及用于解释程序布局的元数据。Raw Binary 则通常不包含 ELF Header、Program Header 和 Symbol Table。

因此，分析 OP-TEE Core 的地址布局时，首选 ELF，而不是直接打开 `.bin`。

## 3.6.3 独立构建 upstream RISC-V OP-TEE

这一部分属于 Track B，不会替代前面的完整 Buildroot 环境。

首先：

```
mkdir -p ~/work/tee/upstream
cd ~/work/tee/upstream

git clone https://github.com/OP-TEE/optee_os.git

cd optee_os
```

以下命令是面向 upstream `virt` 平台的独立构建示例：

```
make -j"$(nproc)" \
    ARCH=riscv \
    PLATFORM=virt \
    CFG_RV64_core=y \
    CFG_USER_TA_TARGETS=ta_rv64 \
    CROSS_COMPILE64=riscv64-linux-gnu- \
    CROSS_COMPILE_ta_rv64=riscv64-linux-gnu- \
    DEBUG=1 \
    CFG_TEE_CORE_LOG_LEVEL=4 \
    O=out/riscv
```

此处的几个关键参数：

| 参数                            | 含义                         |
| ----------------------------- | -------------------------- |
| `ARCH=riscv`                  | 选择 RISC-V 架构               |
| `PLATFORM=virt`               | 选择 OP-TEE 的 RISC-V virt 平台 |
| `CFG_RV64_core=y`             | 生成 RV64 Core               |
| `CFG_USER_TA_TARGETS=ta_rv64` | 构建 RV64 用户态 TA Runtime     |
| `CROSS_COMPILE64`             | RV64 编译工具前缀                |
| `DEBUG=1`                     | 开启便于源码调试的构建设置              |
| `CFG_TEE_CORE_LOG_LEVEL=4`    | 设置 Core 日志级别               |
| `O=out/riscv`                 | 指定构建输出目录                   |

`DEBUG=1` 在当前 OP-TEE 配置中会强制 `CFG_CC_OPT_LEVEL=0` 并开启 Debug Information。upstream CI 也使用 `ARCH=riscv`、`PLATFORM=virt` 和 RV64 GCC 工具链。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+1



如果上述命令遇到平台特定配置错误，应先对比当前源码版本的 `mk/config.mk`、`core/arch/riscv/plat-virt/conf.mk` 及 upstream CI，而不是直接关掉安全检查。

成功构建后，你可以研究输出目录中的：

```
out/riscv/
    |
    +-- core/
    |     +-- tee.elf
    |     +-- ...
    |
    +-- export-ta_rv64/
    |
    +-- ...
```

文件布局可能随版本变化。定位时使用：

```
find out/riscv -name 'tee.elf' -o -name '*.map'
```

特别注意：

Track B 独立编译出来的 `tee.elf` 不会自动进入 Track A 的 QEMU 启动镜像。

需要在集成构建中显式替换或重新构建相应组件之后，修改才会影响运行环境。

## 3.6.4 深入理解 OP-TEE Build Configuration

OP-TEE 的构建过程不只是简单地把所有 `.c` 文件编译链接到一起。

它需要根据 Architecture、Platform 和 Configuration 条件选择对应源码。

例如：

```
ARCH=riscv
    |
    v
Architecture Configuration
    |
    v
PLATFORM=virt
    |
    v
Platform-Specific Configuration
    |
    v
CFG_XXX Feature Configuration
    |
    v
Object Selection
    |
    v
Compile + Link
```

为了深入掌握这套机制，我建议先从下面几个位置研究：

```
optee_os/
|
+-- Makefile
|
+-- mk/
|    +-- config.mk
|
+-- core/
     +-- arch/
          +-- riscv/
               +-- riscv.mk
               +-- kernel/
               |    +-- link.mk
               +-- plat-virt/
                    +-- conf.mk
```

需要回答：

- 某个 `CFG_XXX` 宏由谁定义？
- 默认值在哪里？
- 平台能否强制覆盖？
- 对应的 `.c`、`.S` 如何被选择编译？
- 条件编译与 Makefile 对象选择之间是什么关系？

建议开启详细编译日志：

```
make \
    ARCH=riscv \
    PLATFORM=virt \
    CROSS_COMPILE64=riscv64-linux-gnu- \
    V=1 \
    O=out/riscv
```

然后分析最终的 Compiler Flags 和 Linker Flags。

例如：

```
-march=...
-mabi=...
-mcmodel=...
-fpie
-pie
-ffreestanding
-nostdlib
```

这些选项可能因构建组件和配置而不同。

重点不是记住每一个选项，而是理解它们对 RISC-V Firmware 的约束。

以 `-march` 和 `-mabi` 为例：

`-march` 描述编译器可以使用哪些 ISA 扩展，`-mabi` 描述函数调用与寄存器使用的 ABI。

如果你的 QEMU CPU 不支持编译器生成的某条指令，可能直接在早期启动阶段发生 Illegal Instruction Trap。

## 3.6.5 LLVM/Clang 构建

LLVM 已经成为 RISC-V OP-TEE 的重要开发方向。

2026 年 RP022 Milestone 2 的开发工作包含 LLVM/Clang + lld 支持，以及配套的 RISC-V OP-TEE Buildroot 配置。公开技术记录使用 LLVM 22.1.8，并提到 `qemu_riscv64_virt_optee_llvm_defconfig`。

[image](https://www.google.com/s2/favicons?domain=https://raymo200915.github.io\&sz=32)

Raymond Mao



与 GNU 相比：

| 功能                | GNU Toolchain | LLVM Toolchain            |
| ----------------- | ------------- | ------------------------- |
| C Compiler        | GCC           | Clang                     |
| Assembler         | GNU as        | LLVM Integrated Assembler |
| Linker            | GNU ld        | lld                       |
| Object Inspection | objdump       | llvm-objdump              |
| ELF Information   | readelf       | llvm-readelf              |
| Debugging         | GDB           | LLDB / GDB                |

不要简单地认为：

```
CC=clang make
```

就一定能将整套 OP-TEE 改成 LLVM 编译。

完整构建可能依赖特定 Makefile 变量、链接器、Compiler Runtime 和 TA Dev Kit 配置。

对于独立 OP-TEE 源码，可以研究：

```
make ARCH=riscv PLATFORM=virt COMPILER=clang ...
```

但在实际执行前，应根据所用源码版本确认 LLVM 版本、交叉编译目标、编译器链接方式，以及当前构建支持的配置。

我的建议是：

先用当前已验证的 GNU Baseline 成功运行完整系统，再建立 LLVM Baseline。

这样如果 LLVM 版本遇到 Boot Hang 或 Stack Canary Failure，你可以比较编译器生成代码和链接布局，而不必同时怀疑整个集成环境。

# 3.7 从 Buildroot 进入日常 Kernel 开发工作流

完成第一次完整编译后，不能每修改一行 OP-TEE 代码，就重新搭建整个构建环境。

我们需要建立更高效的 Incremental Development Workflow。

## 3.7.1 如何找到 Buildroot 实际使用的源码？

在 Buildroot 根目录下：

```
find output/build \
    -maxdepth 1 \
    -type d \
    | grep -E 'optee|opensbi|linux|uboot'
```

通常可以找到相关软件包的构建目录。

但是需要注意，Buildroot Package Build Directory 与普通 Git Working Tree 不一定相同。

有些 Buildroot Package 会下载 Tarball；有些会下载指定 Commit 后解包；也有 Package 使用本地源码 Override。

因此不要默认 `output/build/optee-os-*` 里面一定存在 `.git`。

检查：

```
find output/build \
    -maxdepth 2 \
    -name .git
```

如果没有 `.git`，并不表示源码不完整，只说明该构建目录不是标准 Git Working Tree。

## 3.7.2 修改源码后的增量编译

Buildroot 提供 Package Rebuild 机制。

例如，在相应 OP-TEE Package 确实存在时，可以使用：

```
make optee-os-rebuild
```

之后重新执行：

```
make
```

这里：

- `optee-os-rebuild` 重新触发 OP-TEE Package 的构建和安装阶段。
- 顶层 `make` 用于完成必要的后续镜像生成。

Buildroot 还提供：

```
make optee-os-reconfigure
make optee-os-dirclean
```

三者区别如下：

| 命令              | 用途                         |
| --------------- | -------------------------- |
| `*-rebuild`     | 从编译阶段重新构建                  |
| `*-reconfigure` | 从配置阶段重新执行                  |
| `*-dirclean`    | 删除对应 Package 的构建目录，重新提取并构建 |

这是 Buildroot 手册定义的通用机制。需要注意 Package 名称可能由所使用的 RISE 分支定制，应通过当前配置及 `make help` 进行确认。

[image](https://www.google.com/s2/favicons?domain=https://buildroot.org\&sz=32)

Buildroot



### 为什么不能无条件只 Rebuild OP-TEE？

假设你修改了 OP-TEE 的某个 TA Syscall ABI。

不仅 OP-TEE Core 需要重新编译，TA Runtime 或测试 TA 也可能需要相应更新。

同样，如果你修改了：

```
OpenSBI Domain ABI
       |
       v
OP-TEE Driver / Integration
       |
       v
Linux TEE Driver
```

那么可能需要重新构建多个相关组件。

Buildroot 的 Incremental Build 不会自动发现所有跨 Package 的语义依赖变化。

判断需要重编译哪些组件，是 Firmware/OS Engineer 必须掌握的工程能力。

## 3.7.3 推荐采用本地源码 Override

长期进行 Upstream 开发时，我不建议直接在 `output/build` 中大量修改源码。

更合理的方式是让 Buildroot 使用你自己维护的 Git Working Tree：

```
~/work/tee/
|
+-- upstream/
|    +-- optee_os/
|    +-- opensbi/
|
+-- integrated/
     +-- buildroot-rp016/
```

标准 Buildroot 支持 `BR2_PACKAGE_OVERRIDE_FILE` 和 `*_OVERRIDE_SRCDIR` 机制，让指定 Package 使用本地源码目录。

例如，对于使用标准 `OPTEE_OS` Package 名称的配置，Override 文件可能包含：

```
OPTEE_OS_OVERRIDE_SRCDIR = /home/<user>/work/tee/upstream/optee_os
```

这只是标准 Buildroot 机制的示例。RISE 分支是否使用对应 Package 变量、是否附加补丁及额外构建过程，需要先检查当前 `.mk` 文件。

使用 Override 后，可以形成：

```
Edit Git Working Tree
        |
        v
Buildroot Package Rebuild
        |
        v
Generate Firmware Image
        |
        v
Run QEMU
        |
        v
Debug / xtest
```

这套工作流更适合长期修改 `thread_rv.S`、`entry.S`、MMU 和 SBI Driver。

# 3.8 GDB 跨 M-mode / S-mode / U-mode 调试

本章技术重点

这是整个 Chapter 03 最值得深入的部分。

因为你后续研究 OpenSBI Domain Switch 和 OP-TEE Trusted Thread 时，几乎一定需要从 C 函数深入到底层 Assembly 和 CSR。

## 3.8.1 首先理解 QEMU GDB 的工作原理

QEMU System Emulator 内置 GDB Stub，可以通过 Remote Serial Protocol（RSP）向 Host GDB 暴露模拟 CPU 的执行状态。

```
WSL2 Host
|
+-- gdb-multiarch
|       |
|       | Remote GDB Protocol
|       | TCP 127.0.0.1:1234
|       v
+-- QEMU GDB Stub
        |
        +-- Hart 0
        |    +-- GPR
        |    +-- CSR
        |    +-- PC
        |
        +-- Hart 1
        |
        +-- Hart 2
        |
        +-- Hart 3
```

这与 Linux `gdbserver` 不同。

`gdbserver` 主要用于调试操作系统管理的用户态进程，而 QEMU System GDB Stub 可以在 Guest OS 尚未启动时调试整个模拟系统。

因此你可以在最早期 Firmware 代码处设置断点。

## 3.8.2 开启 QEMU GDB Stub

QEMU 官方支持：

```
-s
-S
```

含义如下：

| 参数     | 作用                        |
| ------ | ------------------------- |
| `-s`   | 启动默认 TCP 1234 的 GDB Stub  |
| `-S`   | 启动时暂停 Guest CPU，等待调试器继续执行 |
| `-gdb` | 显式指定 GDB Stub 的连接方式       |

例如，通用 QEMU 命令可以使用：

```
qemu-system-riscv64 \
    <existing boot arguments> \
    -S \
    -gdb tcp:127.0.0.1:1234
```

这里的 `<existing boot arguments>` 表示你当前已验证的完整启动参数，不是需要原样输入的 Shell 参数。

对于 RP016，建议先阅读：

```
sed -n '1,240p' \
    output/images/start-qemu.sh
```

找到真正启动 `qemu-system-riscv64` 的命令，再在那条命令中加入 `-S` 和 `-gdb`。

不要假定：

```
./start-qemu.sh -S -s
```

会生效，因为 Shell Script 未必把参数转发给 QEMU。

QEMU 官方 GDB 文档：[QEMU GDB Usage](https://www.qemu.org/docs/master/system/gdb.html?utm_source=chatgpt.com)。

[image](https://www.google.com/s2/favicons?domain=https://www.qemu.org\&sz=32)

QEMU documentation



## 3.8.3 GDB 连接并观察 CPU 状态

打开另一个终端：

```
gdb-multiarch
```

在 GDB 中：

```
set pagination off

set architecture riscv:rv64

target remote localhost:1234

info registers

x/16i $pc
```

这些命令分别用于：

- 关闭分页输出。
- 选择 RV64 指令架构。
- 连接 QEMU GDB Stub。
- 查看当前 CPU 寄存器。
- 反汇编 PC 附近的 16 条指令。

继续执行：

```
si
```

即 Single Instruction Step。

或者：

```
continue
```

让 CPU 继续执行。

注意：不同 QEMU/GDB 版本对 RISC-V CSR 的暴露程度可能不同。某些 CSR 可以在 `info all-registers` 中看到，但不能假设所有 CSR 都始终能够被 GDB 读取。

## 3.8.4 M-mode 调试：OpenSBI

首先定位 OpenSBI 的 ELF：

```
find output/build \
    -name 'fw_dynamic.elf' \
    -o -name 'fw_jump.elf' \
    -o -name 'fw_payload.elf'
```

找到实际启动使用的 OpenSBI ELF。

这里要特别注意：不能因为找到一个 ELF，就认定 QEMU 当前使用它。

你必须查看启动脚本，确认它对应的是哪个 Firmware Image。

在 GDB 中，假设选择的是 `fw_dynamic.elf`：

```
symbol-file /absolute/path/to/fw_dynamic.elf
```

如果需要从最初入口调试：

```
info address _start

break _start

continue
```

需要根据当前 OpenSBI ELF 的实际符号、装载地址和启动状态调整断点。

成功停在 OpenSBI 后，可检查：

```
info registers
x/20i $pc
bt
```

在 RISC-V M-mode 中，特别值得观察：

```
mstatus
mepc
mcause
mtval
mtvec
mscratch
medeleg
mideleg
```

### 一个重要细节：如何判断当前 Privilege Mode？

不能简单地通过 `mstatus.MPP` 判断当前正在 M-mode 还是 S-mode。

`MPP` 主要保存 Trap 进入 M-mode 之前的特权级。

类似地，`sstatus.SPP` 表示对应 S-mode Trap 的先前特权级。

因此：

当前执行特权级与 Trap 返回状态寄存器中保存的先前特权级不是同一概念。

具体调试时，应该结合 QEMU 暴露的执行状态、当前 PC 对应代码、Trap 入口与返回路径进行判断。

如果 GDB/QEMU 版本暴露当前 Privilege 状态的伪寄存器，也可以直接利用，但不能把它视为 ISA 规定的通用 CSR。

## 3.8.5 S-mode 调试：OP-TEE

定位 OP-TEE ELF：

```
find output/build \
    -name 'tee.elf'
```

或者在独立构建目录中：

```
find out/riscv \
    -name 'tee.elf'
```

使用：

```
symbol-file /absolute/path/to/tee.elf
```

然后查看可用函数：

```
info functions boot
info functions thread
```

也可以查看启动汇编中的 Symbol：

```
riscv64-linux-gnu-nm -n tee.elf | head -40
```

根据实际 Symbol 设置断点：

```
break <verified_symbol_name>
```

需要注意：在同一个 GDB 会话中，`symbol-file` 会替换主符号文件。

如果希望保留 OpenSBI 符号并加入 OP-TEE 的符号信息，可以使用 `add-symbol-file`，但必须考虑映像装载及运行时地址。

对于固定链接地址的映像，若 GDB 能根据 ELF 获取正确地址，可以直接加载。

对于 PIE/ASLR 映像，则需要根据实际运行时重定位位置进行符号加载。

这部分是 OP-TEE 调试中特别容易踩到的坑。

## 3.8.6 U-mode 调试：Trusted Application

这部分比调试 OpenSBI 和 OP-TEE Core 更复杂。

原因在于：

1. User TA 可能动态加载。
2. TA 可能使用 PIE。
3. TA 在 TEE Domain 的 U-mode 中运行。
4. TA 的 VA Mapping 与普通 Linux CA 不同。
5. 需要先知道 TA 的实际运行时 Load Address。

因此，GDB 正确显示 OP-TEE Core 符号，并不表示它也能自动识别 TA 函数符号。

例如，假设：

```
TA ELF Link-time Address = 0x00000000
TA Runtime Load Address  = 0x40000000
```

这里只是演示，不代表 OP-TEE 固定采用该地址。

需要根据实际加载地址将符号重定位。

GDB 可以用 `add-symbol-file` 的 Offset 或指定 Section Address 的方式实现。

要验证地址映射，必须配合：

- TA Loader 的日志。
- TA ELF Program Headers。
- TA Runtime Mapping。
- GDB 当前 PC。

另外，TA 的签名加载文件不一定是可以直接作为 GDB 符号文件使用的原始 ELF。你可能需要使用构建目录中保留的未剥离 TA ELF。

## 3.8.7 跨 Privilege 调试的核心问题

这里有一个非常重要的概念区别：

```
M-mode -> S-mode
    Privilege Transition

Linux Domain -> OP-TEE Domain
    Security Domain Transition

OP-TEE Core -> User TA
    S-mode -> U-mode

OP-TEE Trusted Thread A -> Thread B
    Trusted Thread Context Switch
```

这四个事件并不等价。

例如：

从 Linux S-mode 进入 OP-TEE S-mode，不意味着简单执行一次 `sret` 就能完成安全 Domain Switch。

还涉及 M-mode 管理的上下文、隔离策略和实际执行权限。

同样，从 OP-TEE S-mode 进入 User TA U-mode，也不表示发生了 Linux 与 OP-TEE 的 Domain Switch。

后续 Chapter 06、10、12 会分别深入这些机制。

# 3.9 ELF、Linker Script 与 Firmware Memory Layout

这一部分对 Firmware Engineer 特别重要。

因为早期启动错误、异常入口错误、MMU 初始化失败，很多时候并不是 C 算法本身有问题，而是 ELF Layout、链接地址和运行时地址之间出现了不匹配。

## 3.9.1 ELF 的三种重要信息

一个 ELF 文件包含多类信息：

```
ELF File
|
+-- ELF Header
|      Architecture
|      Entry Point
|      ELF Type
|
+-- Program Headers
|      LOAD Segments
|      Virtual Address
|      Physical Address
|      Permissions
|
+-- Section Headers
       .text
       .rodata
       .data
       .bss
       .rela.*
       .symtab
       .debug_info
```

分别使用以下命令观察：

```
riscv64-linux-gnu-readelf -h tee.elf
```

```
riscv64-linux-gnu-readelf -lW tee.elf
```

```
riscv64-linux-gnu-readelf -SW tee.elf
```

其中：

- `-h`：ELF Header。
- `-l`：Program Headers。
- `-S`：Section Headers。

为什么要同时看 Program Header 和 Section Header？

因为 Section 主要描述链接和文件内部的组织，而 Segment 描述 Loader 应如何把程序装入内存。

对于 Boot Firmware，后者尤其重要。

## 3.9.2 VMA 与 LMA

这两个概念值得深入理解。

- VMA — Virtual Memory Address：代码运行时使用的虚拟地址。
- LMA — Load Memory Address：初始化映像被放置或装载的地址。

在简单的物理地址直接执行环境中，二者可能相同。

但在启用 MMU、进行重定位或使用特殊 Firmware Layout 时，二者可能不同。

例如，假设某个内核映像：

```
Physical Load Address
    0x80200000
          |
          | Enable MMU
          v
Virtual Execution Address
    0xffffffc000000000
```

这里的地址仅用于解释原理。

Boot Code 的一项重要工作可能是：

1. 使用 MMU 关闭状态下可访问的地址执行初始化。
2. 创建页表。
3. 配置 `satp`。
4. 执行必要的 `SFENCE.VMA`。
5. 将执行流程转换到预期虚拟地址。

如果这些地址关系处理错误，典型结果可能是：

- Instruction Page Fault。
- Load/Store Page Fault。
- PC 跳转到错误位置。
- Stack Pointer 指向无效 VA。
- GDB 中显示异常的 Call Stack。

### 一个 RISC-V OP-TEE 特别重要的补充：PIE/ASLR

当前 OP-TEE 文档中，RISC-V Core ASLR 使用 `-fpie` 和 `-pie`，并包含运行时地址重定位。不能将 `tee.elf` 的链接时地址简单等同于所有启动中的实际执行 VA。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation



因此，调试 OP-TEE Core 时要区分：

```
ELF Link-time VMA
        |
        | Relocation / ASLR
        v
Runtime Virtual Address
        |
        | Page Table Translation
        v
Runtime Physical Address
```

三个地址层次可能不同。

你已经学习过 Sv39/Sv48，因此这里建议将 ELF 地址与实际 PTE Walk 对照研究。

## 3.9.3 Linker Script 如何决定地址布局？

Linker Script 可以决定：

- `.text`、`.rodata`、`.data` 的布局。
- Section Alignment。
- Entry Point。
- Stack/Heap 或预留区域。
- 特定 Boot Section 的位置。
- 符号定义及链接布局约束。

对 OP-TEE RISC-V，可以从：

```
core/arch/riscv/kernel/link.mk
```

及其引用的链接脚本开始研究。

建议不要只阅读最终 `tee.elf`，还要观察实际链接命令：

```
make V=1 ...
```

特别关注：

```
-T <linker-script>
```

和编译器/链接器输出的 Map File。

## 3.9.4 Symbol 与反汇编

查看 Symbol：

```
riscv64-linux-gnu-nm -n tee.elf | less
```

查看完整反汇编：

```
riscv64-linux-gnu-objdump \
    -d \
    tee.elf | less
```

显示源码与反汇编混合视图：

```
riscv64-linux-gnu-objdump \
    -dS \
    tee.elf | less
```

查看 Debug Information：

```
riscv64-linux-gnu-readelf \
    --debug-dump=info \
    tee.elf | less
```

查看 Relocations：

```
riscv64-linux-gnu-readelf \
    -rW \
    tee.elf
```

这里有一个很重要的训练目标：

> 能够从 `entry.S` 中某条汇编指令，找到其 Object File、最终 ELF Address，并在 QEMU GDB 中确认这条指令确实被执行。

这能够将源码、Linker 和运行时行为真正串联起来。

# 3.10 可复现构建与版本管理

对于希望长期向 OP-TEE Upstream 贡献代码的工程师来说，构建环境的可复现性不是附加工作，而是基本要求。

## 3.10.1 为什么只记录 Branch Name 不够？

假设你记录：

```
OP-TEE branch: master
OpenSBI branch: master
Linux branch: master
```

三个月后重新构建，同样的 Branch Name 可能已经指向不同 Commit。

最终结果可能完全不同。

正确的 Baseline 应该记录每个仓库的 Commit ID，以及工具链和构建参数。

例如：

```
Buildroot Commit:  <40-byte SHA>
OpenSBI Commit:    <40-byte SHA>
OP-TEE Commit:     <40-byte SHA>
Linux Commit:      <40-byte SHA>
U-Boot Commit:     <40-byte SHA>
QEMU Commit:       <40-byte SHA>
```

对于 Buildroot 从 Tarball 获取的包，应该记录对应的固定版本、源码哈希和补丁集，而不是硬套 Git Commit。

## 3.10.2 建议的 Build Manifest

一个实际工程至少应该记录：

```
project:
    riscv-optee-lab

platform:
    qemu-riscv64-virt

integration:
    rp016_m1
    buildroot_commit

components:
    opensbi_version
    optee_os_version
    linux_version
    uboot_version
    qemu_version

toolchain:
    gcc_version
    binutils_version
    gdb_version

configuration:
    buildroot_defconfig
    optee_config
    linux_config
    opensbi_config

tests:
    boot_result
    xtest_result
    known_failures
```

这是一种建议的数据记录结构，不是 Buildroot 官方 Manifest Schema。

## 3.10.3 记录实际构建版本

在 Buildroot 根目录：

```
mkdir -p lab-results
```

记录 Buildroot Commit：

```
git rev-parse HEAD \
    > lab-results/buildroot-commit.txt
```

保存 Buildroot 配置：

```
cp .config \
    lab-results/buildroot.config
```

保存 Host 编译器版本：

```
gcc --version \
    > lab-results/host-gcc.txt
```

保存实际交叉编译器版本：

```
find output/host/bin \
    -maxdepth 1 \
    -name '*gcc' \
    -type f
```

这里需要确认到底使用的是哪一个 GCC，不要只记录系统安装的 `riscv64-linux-gnu-gcc`。

因为 Buildroot 可能自动下载或生成独立的交叉工具链。

最后，对实际启动镜像记录 SHA256：

```
sha256sum output/images/*bin \
    > lab-results/image-sha256.txt
```

如果某个模式在当前镜像目录不存在，Shell 可能提示文件不匹配。也可以先用 `find output/images -maxdepth 1 -type f` 确认需要记录的文件。

## 3.10.4 推荐建立 Regression Baseline

每次修改内核代码之前，应保存：

```
Baseline:
    Boot: PASS
    Linux: PASS
    TEE Driver: PASS
    xtest: PASS / known failures
```

修改之后：

```
New Build:
    Boot: PASS
    Linux: PASS
    TEE Driver: PASS
    xtest: PASS / new failures
```

通过对比可以明确判断是否引入 Regression。

尤其是安全相关改动，不仅要证明新的功能正确，还需要证明原本有效的隔离与保护措施没有退化。

# 3.11 常见问题及定位方法

下面列出最容易在 RISC-V OP-TEE 开发环境中遇到的几个问题。

| 故障现象               | 优先检查方向                                  |
| ------------------ | --------------------------------------- |
| Buildroot 获取源码失败   | GitLab/GitHub 访问、代理、下载缓存、固定 URL         |
| OP-TEE 编译失败        | Toolchain、ISA/ABI、Feature Configuration |
| QEMU 无输出           | 串口配置、等待连接、`-S` 暂停状态                     |
| 只有 OpenSBI 日志      | Next Stage Entry、Domain Boot、镜像装载       |
| OP-TEE Boot Hang   | Early Stack、MMU、Relocation、Trap         |
| Linux 找不到 TEE      | Driver、RPMI/MPXY、Device Tree、版本兼容       |
| `xtest` 失败         | TA、RPC、Shared Memory、TEE Core、配置        |
| GDB Breakpoint 不命中 | ELF 不匹配、ASLR、地址和符号加载                    |
| 第二个 Hart 启动挂起      | HSM、Per-Hart Init、同步与状态保存               |

### 案例 A：在 OpenSBI 阶段可以打印日志，但无法进入 OP-TEE

建议沿下面路径检查：

```
OpenSBI Startup OK
    |
    v
Domain Initialization OK?
    |
    v
OP-TEE Load Address Correct?
    |
    v
OP-TEE Entry Address Correct?
    |
    v
Hart Assigned Correct Domain?
    |
    v
Isolation Policy Allows Execution?
    |
    v
OP-TEE Entry.S
```

要特别区分：

是根本没有跳转到 OP-TEE Entry，还是已经进入 OP-TEE 但在早期初始化中发生 Trap。

前者优先检查 OpenSBI Domain 和启动参数；后者优先分析 PC、`scause`、`sepc`、`stval`、`satp` 以及早期 Page Table。

### 案例 B：GDB 显示 OP-TEE 正确函数名，但无法与源码执行对应

优先怀疑：

- GDB 装载的是旧版本 `tee.elf`。
- QEMU 实际启动使用的是另一个构建产物。
- 运行时 ASLR 改变了实际虚拟地址。
- Debug Information 对应的源码路径不同。
- 当前 PC 位于尚未建立正常 VA Mapping 的 Early Boot 阶段。

不能仅凭 GDB 能显示符号，就认定符号解析一定正确。

### 案例 C：单 Hart 正常，多 Hart 启动失败

可以采用对比实验：

```
CPU = 1
    Boot PASS

CPU = 2
    Boot FAIL
```

此时建议优先研究：

- Per-Hart Stack。
- `tp`、`sscratch` 及 Per-Hart Data。
- Secondary Hart Entry。
- Global Initialization Synchronization。
- Hart-local MMU/PMP State。
- Domain Context。

但是在这个实验中，首先要确认 QEMU 启动脚本是否真的支持并正确传递了 SMP 数量配置。

# 3.12 本章综合实操与验收

我建议把本章拆成四个可交付实验。

Lab 03.1 — Complete System Boot

建立 `rp016_m1` Buildroot 环境，启动 QEMU，并运行 `xtest`。

交付：Boot Log、Build Config、xtest Result。

Lab 03.2 — M-mode Firmware Debugging

在 OpenSBI Entry 处设置断点，通过 GDB 观察寄存器，单步进入 M-mode Firmware 初始化代码。

交付：入口寄存器快照、关键汇编执行记录。

Lab 03.3 — OP-TEE Early Boot Debugging

在 OP-TEE 的 Early Boot 路径中设置断点，分析其执行地址、SP、MMU 状态及虚拟地址映射。

交付：OP-TEE Boot Address Map 与 GDB Trace。

Lab 03.4 — ELF & Linker Analysis

对 `fw_dynamic.elf` 或当前实际使用的 Firmware ELF，以及 `tee.elf` 分析 Entry、LOAD Segment、Section、Symbol 和 Relocation。

交付：ELF Layout Report、地址转换说明与源码映射。

## Staff Engineer 级检查题

这些问题不要求只回答定义，而需要结合执行过程、源码或实验论证。

1. 为什么 Buildroot 中 OP-TEE Package 编译成功，却不能证明集成镜像能够正常启动？
2. `tee.elf` 的 `e_entry`、运行时 PC 和物理内存地址之间可能存在什么关系？
3. 为什么 OpenSBI 和 OP-TEE 都可以使用 RV64 GCC，但不能简单地共用同一套 Runtime/ABI 假设？
4. 如果 QEMU 在 M-mode 运行正常，进入 S-mode 立刻发生 Instruction Access Fault，你如何组织排查顺序？
5. 为什么一个 Trusted Application 的 GDB Symbol 不能直接按照静态 ELF 地址解析？
6. Buildroot 中修改了 `optee_os` 后，为什么可能还需要重编译 `optee_test`？
7. 调试跨 Domain Switch 时，如何确认当前执行的代码属于 Linux Domain 还是 OP-TEE Domain，而不是仅凭 S-mode 判断？
8. 为什么将所有 upstream 仓库同步到最新 master，反而可能使完整系统无法工作？

如果能够通过实际代码和 GDB Trace 回答这些问题，说明你已经具备继续深入后续架构章节的基础。

# 3.13 本章总结与下一章的衔接

这一章最重要的收获应该是建立以下完整的工程模型：

```
Source Repositories
        |
        v
Version / Patch / Config Management
        |
        v
Cross Compilation
        |
        v
ELF Linking
        |
        v
Firmware + Kernel + RootFS Integration
        |
        v
QEMU Virtual Platform
        |
        v
Boot and Runtime Verification
        |
        v
GDB + ELF + CSR + Source Analysis
        |
        v
Bugfix / Regression Test / Upstream Patch
```

到这里，我们已经为后续 RISC-V OP-TEE 内核学习准备好了完整的方法和工具。

下一章 Chapter 04 — OpenSBI Domain Architecture，将正式进入这套课程最核心的安全架构部分。

下一章我建议围绕一条实际的源码执行链展开：

```
OpenSBI Coldboot
    |
    v
Device Tree Parsing
    |
    v
Domain Initialization
    |
    v
sbi_domain Registration
    |
    v
Hart Assignment
    |
    v
Domain Memory Region Configuration
    |
    v
PMP / HW Isolation Setup
    |
    v
Boot Linux / OP-TEE Domains
```

重点深入 `sbi_domain` 的数据结构、Domain 初始化与生命周期、Per-Hart Context，以及内存权限配置过程。

在 Chapter 04 中，我们不只研究 OpenSBI 如何创建 Domain，还要回答一个贯穿后续课程的关键问题：

OpenSBI Domain 到底是一个软件抽象，还是一个真正能够阻止恶意 Linux Kernel 访问 OP-TEE 内存的安全边界？如果它是软件抽象，那么究竟由哪些硬件机制把这个抽象转化成实际的访问控制？

这将是理解整个 RISC-V OP-TEE Architecture 的关键起点。