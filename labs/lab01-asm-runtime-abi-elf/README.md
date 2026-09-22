# Lab 01：RV64 汇编运行库、psABI 与 ELF

本实验把题库中的 Q002～Q004、Q019、Q021～Q045、Q058～Q059 聚合为一个
可运行工程。目标不是一次实现题库里的所有变体，而是建立一条可以反复进行
“编写汇编 → 运行测试 → 检查 ELF → 阅读反汇编”的学习主线。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64，整数调用约定；
- code model：medany；
- 运行环境：QEMU `virt`，M-mode，`-bios none`；
- 地址空间：Bare，无 libc、无动态链接器；
- 启动地址：`0x80000000`。

实验不会把可选的 V、B、Zicond、Zacas 等扩展当作基线。完成基础任务后，可以
自行增加按 ISA capability 选择的优化版本。

## 基础设施与学员边界

### 基础设施已经提供

- Reset entry、`gp/sp` 初始化、`.bss` 清零和 fatal trap；
- RV64 linker script、独立栈和可复制代码区边界；
- UART 输出及 QEMU `sifive_test` 退出；
- 无 libc 的测试 runner 和边界 canary；
- 检查 `s0-s11` 是否被破坏的 ABI 调用包装器；
- PIC blob 的复制、`fence.i` 和二次执行；
- ELF class/machine/entry/undefined-symbol 静态检查；
- sections、relocations、attributes、CFI 和带源码反汇编报告。

ELF 报告优先使用交叉工具链的 `readelf/nm/objdump`；精简交叉工具链缺少
`readelf` 或 `nm` 时，构建系统会回退到能识别 RISC-V ELF 的宿主工具。

基础设施仅建立执行环境和观察手段，不实现下面的练习算法。

### 需要你完成

只修改 `exercises/*.S`：

1. `asm_memzero(start, size)`
   - 任意地址和长度；
   - `size == 0` 不得访问内存；
   - 不能改写范围外 canary；
   - 建议先字节实现，再增加对齐后的 XLEN store。

2. `asm_memmove(dst, src, size)`
   - 返回原始 `dst`；
   - 支持不重叠、前向重叠和后向重叠；
   - 不允许通过越界读取换取性能；
   - 仅针对普通可缓存 RAM，不用于 MMIO。

3. `asm_add128(out, lhs, rhs)`
   - 三个对象都采用 little-endian limb 布局：`[0]=low, [1]=high`；
   - 使用 `sltu` 等基础整数指令传递 carry；
   - 最终 carry 超出 128 位时丢弃。

4. `asm_call_checksum(values, count)`
   - 必须对每个元素调用 C 函数 `lab_checksum_step(acc, value)`；
   - 初始 accumulator 为 `LAB_CHECKSUM_INIT`；
   - 正确建立 16 字节对齐的非叶函数栈帧；
   - 保存 `ra` 以及实际使用的 callee-saved 寄存器；
   - 为栈帧补齐正确的 `.cfi_*` 描述。

5. `asm_global_address()` 和 `asm_global_load()`
   - 前者返回 `lab_global_anchor` 的地址；
   - 后者返回该对象的 64 位值；
   - 使用 PC-relative 地址生成，结合 `.o` 和最终 ELF 观察 relocation 与
     linker relaxation，不能把链接地址硬编码进指令。

6. `asm_pic_blob(value)`
   - 返回 `value * 3 + 7`；
   - 必须完全包含在 `.lab_blob`；
   - 复制到另一地址后仍产生相同结果；
   - 不得访问 blob 外部数据、调用外部函数或依赖原始 PC。

`exercises/` 中初始实现是可链接的 TODO stub。因此：

- `smoke` 初始必须通过；
- `test` 初始应当失败；
- 完成所有任务后 `test` 和 `inspect` 都应通过。

## 使用方法

```sh
cd labs/lab01-asm-runtime-abi-elf

./build.sh toolcheck   # 检查依赖
./build.sh smoke       # 只验证基础设施，初始应 PASS
./build.sh build       # 构建完整实验 ELF
./build.sh test        # 运行学员任务，初始应 FAIL
./build.sh inspect     # 生成并检查 ELF/反汇编报告
./build.sh clean
```

也可以直接使用对应的 `make` target。

若显示 `Error 124`，表示宿主 `timeout` 杀掉了没有正常退出的 QEMU，而不是
测试断言的普通失败。先执行 `./build.sh clean && ./build.sh smoke`：若 smoke
也超时，应检查启动/QEMU 环境；若只有 test 超时，应优先检查练习代码中的
死循环以及 `sp/ra` 破坏。可用 `QEMU_TIMEOUT=30s make test` 临时延长时限，
但不应以延长时限掩盖客体挂死。

## 验收标准

运行测试必须全部通过，并且不得修改测试来掩盖失败。此外应人工回答：

1. 为什么 `gp` 初始化需要局部关闭 relaxation？
2. 为什么 `sp` 在调用 C 函数前必须保持 16 字节对齐？
3. `asm_global_address` 在 `.o` 和最终 ELF 中的指令为什么可能不同？
4. `asm_pic_blob` 能复制执行，为什么不能推出任意 PC-relative 函数都可复制？
5. `.cfi_*` 描述的是哪一时刻的 CFA，错误 CFI 为什么不一定影响正常执行？
6. 当前实现为什么不能直接用于 Device/MMIO memory？

## 调试提示

- Trap 时基础设施会打印 `mcause/mepc/mtval`，优先在反汇编中定位 `mepc`；
- 测试失败但 QEMU 不退出时，检查是否破坏了 `ra`、`sp` 或栈帧；
- 只在 `-O0` 正确的 inline asm 不算正确，本实验按 `-O2` 构建；
- `build/lab01.relocations.txt` 同时包含练习 `.o` 和最终 ELF 的 relocation；
- `build/lab01.frames.txt` 用来核对非叶汇编函数的 unwind 信息。
