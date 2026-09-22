# Lab 02：多 Hart Reset、Early Boot 与自重定位

本实验把 Q001、Q016、Q019、Q055～Q060、Q186～Q193、Q272～Q279 中适合
实作的部分聚合成一个 RV64 多 hart early-boot 工程。重点是建立显式入口契约，
而不是假设 `hart0`、共享栈、已初始化的 `gp/tp` 或天然有序的启动过程。

## 环境基线

- ISA：RV64IMAC + Zicsr + Zifencei；
- ABI：LP64；
- 运行模式：M-mode，Bare address translation；
- 平台：QEMU `virt`，`-bios none`；
- hart 数：完整测试固定为 4；
- QEMU Reset shim 输入：`a0=mhartid`、`a1=FDT address`；
- hart ID：当前运行平台为 `0,1,2,3`，但映射 helper 必须支持稀疏 ID。

QEMU 的 ELF loader 会装载并清理部分镜像状态，因此实验使用显式的
`.lab_data_load/.lab_data/.lab_bss` 区域模拟 firmware 的 data copy 和 BSS
clear，避免把 loader 行为误当成自己的启动代码已经正确。

## 基础设施与学员边界

### 基础设施已经提供

- 最小 Reset shim：读取 `mhartid`、为每个 QEMU hart 分配 emergency stack、
  关闭中断并安装 fatal vector；
- 4-hart QEMU 启动、自检和超时诊断；
- early UART/finisher，不依赖 `gp`、普通 C runtime 或已清零 BSS；
- permanent per-hart stacks、per-hart storage 和 linker symbols；
- FDT、栈区间、`gp/tp`、owner、section init 和 publication 的验收代码；
- 独立 helper unit tests，不要求先完成多 hart entry；
- 自包含 relocation image 的复制、`fence.i` 和二次执行；
- ELF program headers、sections、symbols、relocations、attributes 和反汇编报告。

Reset shim 只提供进入练习前不可避免的 emergency stack。它不会初始化 `gp/tp`、
不会选择 coldboot owner、不会初始化实验 data/BSS、不会发布 boot-ready，也不会
切换到 permanent stack。

### 需要你完成

只修改 `exercises/*.S`。

1. `lab_hartid_to_index(hartid, table, count)`
   - 在线性表中查找 hart ID，返回 logical index；
   - 找不到时返回 `-1`；
   - 支持 `count==0` 和稀疏、非零起始 hart ID。

2. `lab_init_regions(data_dst, data_src, data_size, bss_start, bss_size)`
   - 精确复制 data 区域并清零 BSS 区域；
   - 支持任意字节长度和 `size==0`；
   - 不得访问范围外 canary；
   - 这里只处理普通 RAM，不处理 MMIO 或 non-coherent cache。

3. `lab_relocated_stage(value)`
   - 函数和内部常量都必须位于 `.reloc_image`；
   - 返回 `(value ^ internal_constant) + 0x1234`；
   - 必须使用位置无关的内部引用；
   - 整个 section 复制到另一地址后仍应运行。

4. `lab_boot_entry(hartid, fdt)`
   - 保存入口参数并用 `lab_hartid_to_index` 建立 logical index；
   - 初始化 `gp`，并在该序列局部禁止 linker relaxation；
   - 按 logical index 切换 permanent stack；
   - 设置 `tp = __per_hart_start + index * LAB_PER_HART_SIZE`；
   - 使用 LR/SC 或等价原子协议从 `UINT64_MAX` 状态选出唯一 coldboot owner；
   - owner 用 `lab_init_regions` 初始化实验段并写入 publication token；
   - owner release 发布 `lab_boot_ready=1`，其他 hart acquire 等待；
   - 所有 hart 最终以
     `a0=hartid, a1=fdt, a2=index, a3=is_owner` tail-call
     `lab_boot_hart_main()`；该函数不会返回；
   - 未知 hart ID 必须进入有界错误/park 路径，不能越界选择栈。

相关地址、大小和 token 定义在 `include/lab_layout.h` 与 linker symbols 中。

## 分阶段使用

```sh
cd labs/lab02-multihart-early-boot

./build.sh toolcheck
./build.sh smoke       # 基础设施；初始应 PASS
./build.sh unit        # 三个 helper；初始应 FAIL
./build.sh build       # 构建完整 multi-hart image
./build.sh test        # 完整 boot；初始显示 TODO 并 FAIL
./build.sh inspect     # 生成 ELF/反汇编报告
./build.sh clean
```

建议按以下顺序完成：

1. 先实现 hart mapping，运行 `unit`；
2. 实现 data/BSS 初始化，继续运行 `unit`；
3. 实现 relocation image，使 `unit` 全部通过；
4. 最后实现 `lab_boot_entry`，运行完整 `test`。

## 验收重点

完整测试会检查：

- 4 个 hart 是否都到达 runtime；
- hartid/logical-index 映射和栈是否匹配；
- 每个 hart 的 stack 是否隔离且保持 16 字节对齐；
- `gp` 是否等于 `__global_pointer$`；
- `tp` 是否指向正确的 per-hart slot；
- FDT 指针是否保留且头部 magic 正确；
- 是否恰好有一个 coldboot owner；
- data copy、BSS clear、publication token 和 boot-ready 是否都完成；
- helper 和复制后的 relocation image 是否通过。

运行测试通过并不自动证明 fence 是最弱且充分的；最后还应在反汇编中确认
release/acquire 边、`gp` 初始化的 relocation 以及各 hart 的 stack 计算。

## 思考题

1. 为什么 Reset shim 可以按 `mhartid` 选择 emergency stack，而产品代码通常不能？
2. 为什么 winner 清全局区之前，其他 hart 不能调用普通 C？
3. 为什么原子选主不能替代 winner 对初始化结果的 release publication？
4. 为什么 `WFI` 只能放在“检查条件 → WFI → 重查条件”的循环中？
5. 为什么 `medany` 不能保证含绝对指针的数据表可随代码复制？
6. 真正 Flash→RAM 镜像还需要哪些 LMA/VMA、cache 和 remote `FENCE.I` 协议？

