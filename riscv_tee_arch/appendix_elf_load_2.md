# OP-TEE Deep Dive：ldelf 与 TA 的隔离机制、VM_FLAG_LDELF、dlopen() 的完整实现

这一部分非常值得深入，因为它涉及 OP-TEE 一个容易被忽略、但非常重要的设计问题：

ldelf 和 TA 都运行在 S-EL0，为什么 TA 不能随意修改 ldelf？如果它们共享同一个用户地址空间，OP-TEE 又如何维护 Loader 自身的安全性？

我们还要回答另一个问题：

TA 运行起来之后，为什么还能使用 `dlopen()`？此时 ldelf 已经完成了最初的加载任务，它又是如何继续参与 TA 的运行时动态加载的？

先给出一个重要结论：

> OP-TEE 并不是简单地依赖 S-EL0 与 S-EL1 的特权级隔离来保护 ldelf。它还依赖用户虚拟内存映射、内存对象生命周期、页级访问权限，以及 Core 对 Loader 系统调用的约束。

另外，需要特别区分三个概念：

1. Privilege Isolation：S-EL0 无法直接执行 S-EL1 特权操作。
2. Address-space Isolation：不同用户上下文之间的虚拟地址映射隔离。
3. Memory-region Isolation：同一个用户地址空间内部，不同内存区域也可以具有不同访问权限。

第三点正是我们接下来需要重点研究的地方。

## 一、先纠正一个容易产生的架构误解：同一个地址空间不等于相同权限

假设某个 TA 已经完成加载，用户地址空间大致如下：

TA User Virtual Address Space — S-EL0

示意布局，并非 OP-TEE 固定地址布局

ldelf Code

URX

Loader 代码：用户可读、可执行，不可写

ldelf Data

URW

Loader 数据：用户可读、可写，需结合具体生命周期和映射策略理解

TA Code

URX

TA 自身的可执行代码

TA Data / BSS

URW

TA 的可写数据

TA Stack / Heap

URW

TA 的运行时内存

这里有一个重要事实：

CPU 并不知道某一段内存属于 ldelf，还是属于 TA。CPU 只知道当前访问的虚拟地址对应什么页表属性。

假设：

```
ldelf code:
    VA = 0x40000000
    Permission = URX

TA data:
    VA = 0x50000000
    Permission = URW
```

当 TA 执行：

```
*(volatile uint64_t *)0x40000000 = 0x1234;
```

CPU 会根据页表权限检查写访问。

由于 ldelf Code 是只读映射，这次写操作会触发 Data Abort，而不是成功修改 Loader 代码。

但是，这里还存在一个更深的问题：

如果 ldelf Data 是 URW，那么 TA 是否可以修改 ldelf Data？

答案不能简单地说不可以。

我们必须分别研究 Code、Data、动态加载过程和实际映射权限，不能把 Loader 的所有内存笼统地称为“受保护”。

## 二、VM_FLAG_LDELF 到底是什么？它是不是一种内存保护机制？

先看当前主线源码中的 `ldelf_load_ldelf()`：

```
res = alloc_and_map_fobj(uctx, LDELF_STACK_SIZE,
                         TEE_MATTR_URW | TEE_MATTR_PRW,
                         VM_FLAG_LDELF,
                         &stack_addr);

res = alloc_and_map_fobj(uctx, ldelf_code_size,
                         TEE_MATTR_PRW,
                         VM_FLAG_LDELF,
                         &code_addr);

res = alloc_and_map_fobj(uctx, ldelf_data_size,
                         TEE_MATTR_URW | TEE_MATTR_PRW,
                         VM_FLAG_LDELF,
                         &rw_addr);
```

然后：

```
prot = TEE_MATTR_URX;

res = vm_set_prot(uctx, code_addr,
                  ROUNDUP(ldelf_code_size, SMALL_PAGE_SIZE),
                  prot);
```

这几段代码非常关键。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



### 2.1 VM_FLAG_LDELF 和 TEE_MATTR_URX 是两种完全不同的东西

| 属性                  | 本质                       | 主要作用                   |
| ------------------- | ------------------------ | ---------------------- |
| `VM_FLAG_LDELF`     | VM Region 元数据标记          | 标识该映射属于 ldelf          |
| `TEE_MATTR_URX`     | MMU Protection Attribute | 用户可读、可执行               |
| `TEE_MATTR_URW`     | MMU Protection Attribute | 用户可读、可写                |
| `VM_FLAG_PERMANENT` | VM Region 生命周期标记         | 标识不可通过普通 unmap 路径移除的映射 |

这点必须记住：

`VM_FLAG_LDELF` 本身不会让 CPU 拒绝 TA 对某个地址的读写。真正决定 CPU 能否读、写、执行的是页表中的权限属性。

可以从 `core/mm/vm.c` 中的 `vm_set_prot()` 看到，函数会更新 Region 的属性，重新建立页表映射，并执行 TLB invalidation。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



### 2.2 一个非常重要的源码发现：ldelf Code 和 Data 的保护并不对称

从前面的代码可以直接得到：

```
ldelf Code:
    初始：PRW
    最终：URX

ldelf Data:
    初始：URW | PRW
    最终：URW | PRW

ldelf Stack:
    URW | PRW
```

也就是说：

- ldelf 的代码最终不能被用户态直接写入。
- ldelf 的 Data 和 Stack 则是用户可写的。
- 它们与 TA 的 Data、Stack 一样，都处在同一个 `user_mode_ctx` 管理的用户地址空间中。

这里必须修正一个可能产生的错误理解：

不能认为 `VM_FLAG_LDELF` 就代表 ldelf 的所有内存都对 TA 不可访问。

至少从上述映射和权限设置来看，`VM_FLAG_LDELF` 不是这样的安全边界。

### 2.3 那么 TA 能不能修改 ldelf 的 Data？

从当前这些映射属性来看，不能仅凭 `VM_FLAG_LDELF` 就保证不能。

如果 TA 能够获得 ldelf Data 的有效虚拟地址，并且对应页面仍然是 `URW`，CPU 的普通 EL0 写访问检查不会因为这个 Region 被标记为 `VM_FLAG_LDELF` 就自动拒绝写操作。

这与 ldelf Code 完全不同：

```
/* TA 尝试修改 ldelf Code */
str x0, [x1]
```

如果 `x1` 指向 URX 页面，会触发权限异常。

而如果 `x1` 指向 ldelf 的 URW Data 页面，单从 MMU 权限看，这个写访问是允许的。

这也是我们分析安全边界时必须区分的：

- Loader Code Integrity：通过 RX 权限保护代码不被直接修改。
- Loader Data Integrity：不能简单依赖 RX 机制保护，因为 Data 本身就是 RW。
- Kernel Integrity：依靠 S-EL0/S-EL1 权限边界以及 Core 自身的内存保护。

因此，ldelf 并不是一个与 TA 在同一地址空间中仍然享有完整独立保护域的进程。

这是一个很重要的架构事实。

### 2.4 VM_FLAG_LDELF 实际上还有哪些用途？

例如，在 `ldelf_loader.c` 的异常状态收集路径中：

```
if (r->flags & VM_FLAG_LDELF)
    arg->maps[n].flags |= DUMP_MAP_LDELF;
```

Core 会把这个标志传给 ldelf 的状态分析代码，以便识别哪些映射属于 Loader。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



另外，Core 的 Attestation 实现也会排除 `VM_FLAG_LDELF` 映射，因为 Loader、Kernel 和临时映射并不属于需要作为 TA 自身代码/只读数据来度量的区域。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



所以，理解 `VM_FLAG_LDELF` 时，应该把它看成一个重要的 VM Region 分类标签，而不是硬件访问控制位。

## 三、ldelf 为什么需要保留在 TA 地址空间中？

这就进入 `dlopen()` 设计的核心。

你可能会认为：

```
第一次加载 TA：

Core → ldelf → 加载 TA → 返回 Core

之后：

Core → TA
```

既然 TA 已经加载完毕，为什么还需要 Loader？

原因是动态加载。

OP-TEE 官方文档明确说明：

ldelf 在 TA 完成初始加载后仍然保留在内存中，后续用于 `dlopen()` 等动态加载操作，也用于 TA Abort 后的 Stack Trace 和 Memory Mapping 分析。&#x20;

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



这里实际上有两种不同的执行场景：

场景 A：TA 初始加载

OP-TEE Core / S-EL1

ldelf / S-EL0

解析 Main TA ELF、加载依赖、Relocation

Main TA / S-EL0

场景 B：TA 运行期间请求 dlopen

Main TA / S-EL0

调用动态加载 API

OP-TEE Core / S-EL1

验证参数、调度 Loader

ldelf / S-EL0

加载共享库、重定位、符号解析

返回 Main TA / S-EL0

继续执行

注意第二种场景：不是 TA 自己跳转到 ldelf 的代码地址，而是先进入 Core，由 Core 决定是否以及如何重新进入 ldelf。

这个区别非常重要。

## 四、TA 如何调用 dlopen()？从 libdl 一直追踪到 OP-TEE Core

我们现在进入真正的源码调用链。

首先明确一个 OP-TEE 与 Linux 动态链接机制的区别：

Linux 通常通过文件路径加载共享库：

```
void *handle = dlopen("libfoo.so", RTLD_NOW);
```

OP-TEE 的共享 TA Library 则采用 UUID 标识。

官方文档中有一个重要配置：

```
CFG_ULIBS_SHARED=y
```

启用后，OP-TEE 支持 TA 使用动态共享库，库文件以独立 ELF 和 `.ta` 形式构建、安装，并由 UUID 标识。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



### 4.1 OP-TEE 的动态库加载架构

Main TA — S-EL0

```
void *handle = dlopen(...);
void *sym = dlsym(handle, "foo");
```

调用 libdl API

libdl — S-EL0

把动态加载请求转换成 PTA_SYSTEM 命令

TA → Core 的受控调用

OP-TEE Core — S-EL1

core/pta/system.c

system_dlopen() / system_dlsym()

ldelf — S-EL0

ldelf/dl.c

dlopen_entry() / dlsym_entry()

Shared Library — S-EL0

加载、重定位、符号解析完成

注意：这不是 Linux 的 `ld.so` 直接在 TA 内部完成所有操作的模型。OP-TEE 把动态加载请求交给 Core，再由 Core 调度 ldelf。

### 4.2 第一步：libdl 向 PTA_SYSTEM 发起请求

先看这个文件：

[lib/libutee/include/pta_system.h](https://github.com/OP-TEE/optee_os/blob/master/lib/libutee/include/pta_system.h?utm_source=chatgpt.com)

其中定义了：

```
#define PTA_SYSTEM_DLOPEN 10
#define PTA_SYSTEM_DLSYM  11
```

特别值得注意的是：

```
/*
 * Load a shared library
 *
 * [in] memref[0]: the UUID of the shared library
 * [in] value[1].a: @flags
 *
 * Used by: (libdl) dlopen(...)
 */
#define PTA_SYSTEM_DLOPEN 10
```

而 `DLSYM`：

```
/*
 * Resolve a symbol in a previously loaded shared library
 *
 * [in] memref[0]: UUID
 * [in] memref[1]: symbol name
 * [out] value[2]: address of the symbol
 */
#define PTA_SYSTEM_DLSYM 11
```

这说明 libdl 与 Core 之间有一个明确的命令接口，而不是直接操作 ldelf 的内部数据结构。

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub



### 4.3 第二步：Core 接收 PTA_SYSTEM_DLOPEN

打开：

[core/pta/system.c](https://github.com/OP-TEE/optee_os/blob/master/core/pta/system.c?utm_source=chatgpt.com)

重点函数：

```
static TEE_Result system_dlopen(
    struct user_mode_ctx *uctx,
    uint32_t param_types,
    TEE_Param params[TEE_NUM_PARAMS])
```

看几个关键操作。

第一，检查参数类型。

```
uint32_t exp_pt = TEE_PARAM_TYPES(
    TEE_PARAM_TYPE_MEMREF_INPUT,
    TEE_PARAM_TYPE_VALUE_INPUT,
    TEE_PARAM_TYPE_NONE,
    TEE_PARAM_TYPE_NONE);

if (exp_pt != param_types)
    return TEE_ERROR_BAD_PARAMETERS;
```

第二，把 UUID 从 User Space 复制到 Kernel Buffer。

```
TEE_UUID uuid = { };

res = copy_from_user(
    &uuid,
    params[0].memref.buffer,
    sizeof(uuid));

if (res)
    return res;
```

为什么要复制？

因为 Core 不能直接相信 User Space 提供的指针。

例如：

```
params[0].memref.buffer = 0x40000000;
```

这个地址可能：

- 根本没有映射。
- 跨越非法内存区域。
- 指向不可访问的区域。
- 在复制过程中触发访问异常。

`copy_from_user()` 是一个重要的安全边界。

第三，真正调用 ldelf。

```
flags = params[1].value.a;

s = ts_pop_current_session();

res = ldelf_dlopen(uctx, &uuid, flags);

ts_push_current_session(s);

return res;
```

这几行是整个设计最值得研究的地方。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



原本正在处理的是 TA 发起的系统调用，现在 Core 需要重新进入 ldelf 执行动态加载。

因此，它暂时保存当前 Session，调用 Loader，然后恢复 Session。

这里不要将 `ts_pop_current_session()` 理解为创建一个新的 TA，也不要认为它会复制一份 Loader。它是 Core 内部的 Session 管理操作。

### 4.4 第三步：ldelf_dlopen() 如何重新进入 S-EL0？

打开：

[core/kernel/ldelf_loader.c](https://github.com/OP-TEE/optee_os/blob/master/core/kernel/ldelf_loader.c?utm_source=chatgpt.com)

找到：

```
TEE_Result ldelf_dlopen(
    struct user_mode_ctx *uctx,
    TEE_UUID *uuid,
    uint32_t flags)
```

我们逐步看。

首先在 Kernel 中创建参数：

```
arg = bb_alloc(sizeof(*arg));

memset(arg, 0, sizeof(*arg));

arg->cmd = LDELF_DL_ENTRY_DLOPEN;
arg->dlopen.uuid = *uuid;
arg->dlopen.flags = flags;
```

这里的 `bb_alloc()` 分配的是 Core 使用的 Bounce Buffer。

然后把参数复制到 ldelf 用户态 Stack：

```
usr_stack -= ROUNDUP(
    sizeof(*arg),
    STACK_ALIGNMENT);

usr_arg = (void *)usr_stack;

res = copy_to_user(
    usr_arg,
    arg,
    sizeof(*arg));
```

这里特别重要：

Core 并没有把参数直接传递给 ldelf 的内部 C 函数，而是通过用户态 Stack 建立一个明确的参数传递 ABI。

接下来：

```
sess = ts_get_current_session();
sess->handle_scall = scall_handle_ldelf;

res = thread_enter_user_mode(
    usr_stack,
    0, 0, 0,
    usr_stack,
    uctx->dl_entry_func,
    is_32bit,
    &panicked,
    &panic_code);
```

注意：

```
uctx->dl_entry_func
```

这个地址非常关键。

它不是 TA 的入口，而是初次加载时由 ldelf 返回给 Core 的专门动态加载入口。

在初始加载过程中：

```
arg->dl_entry = (vaddr_t)(void *)dl_entry;
```

对应：

`ldelf/main.c` 中的：

```
static void __noreturn dl_entry(
    struct dl_entry_arg *arg)
{
    switch (arg->cmd) {
    case LDELF_DL_ENTRY_DLOPEN:
        arg->ret = dlopen_entry(arg);
        break;

    case LDELF_DL_ENTRY_DLSYM:
        arg->ret = dlsym_entry(arg);
        break;

    default:
        arg->ret = TEE_ERROR_NOT_SUPPORTED;
    }

    sys_return_cleanup();
}
```

因此，Core 再次进入 ldelf 时，执行的不是 `_ldelf_start`，也不是最初的 `ldelf()`，而是 `dl_entry()`。

这是理解整个动态加载机制的关键。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub+1



### 4.5 把两次 ldelf 执行的入口区分开

| 阶段   | 入口               | 功能             |
| ---- | ---------------- | -------------- |
| 初次加载 | `_ldelf_start`   | Bootstrap、自重定位 |
| 初次加载 | `ldelf()`        | 加载 Main TA 和依赖 |
| 动态加载 | `dl_entry()`     | 动态加载命令分发       |
| 动态加载 | `dlopen_entry()` | 加载新的共享库        |
| 动态加载 | `dlsym_entry()`  | 查询符号地址         |

你可以把它理解成一个运行在用户态的 Loader Service。

它不需要永久运行在后台，也不需要独立线程。Core 在需要时重新进入它的指定入口，完成工作后 Loader 再通过系统调用返回 Core。

## 五、深入 dlopen_entry()：共享库如何真正加载？

源码：

[ldelf/dl.c](https://github.com/OP-TEE/optee_os/blob/master/ldelf/dl.c?utm_source=chatgpt.com)

这里需要先理解一个概念：

Linux 的 `dlopen()` 通常是：

```
Filename
   |
   v
Filesystem Lookup
   |
   v
ELF File
   |
   v
Dynamic Loader
```

OP-TEE 则是：

```
Library UUID
   |
   v
OP-TEE TA Storage
   |
   v
Secure World 获取 Library ELF
   |
   v
ldelf 解析与加载
```

为什么这样设计？

因为共享库也是一个独立的、受 TA 签名机制保护的 ELF 映像，而不是可以任意从普通文件系统加载的 `.so`。

### 5.1 Library 的存储和加载

动态加载时，ldelf 仍然使用此前介绍过的接口：

```
sys_open_ta_bin()
sys_map_ta_bin()
sys_copy_from_ta_bin()
sys_close_ta_bin()
sys_map_zi()
sys_set_prot()
```

这些函数在：

[ldelf/sys.c](https://github.com/OP-TEE/optee_os/blob/master/ldelf/sys.c?utm_source=chatgpt.com)

中实现。

它们最终通过 `_ldelf_*` 汇编入口进入 Core 的 Loader System Call Handler。

这意味着：

ldelf 虽然可以解析 ELF、决定需要哪些映射，但真正涉及安全内存分配、文件访问和页表修改时，仍然要由 Core 执行受控操作。

### 5.2 动态库的 Relocation

假设：

```
Main TA:
    VA = 0x40000000

Shared Library A:
    VA = 0x50000000
```

Main TA 需要调用 Library A 的：

```
int secure_hash(const void *buf, size_t len);
```

Loader 需要完成：

1. 找到 Library A 的 ELF。
2. 检查 ELF 格式。
3. 分配并映射 PT_LOAD。
4. 初始化 BSS。
5. 解析动态符号。
6. 处理 Relocation。
7. 修正 GOT 中的符号地址。
8. 更新动态加载器的对象状态。

例如：

```
GOT Entry:
    0x40008000

secure_hash:
    0x50002000
```

需要得到：

```
[0x40008000] = 0x50002000
```

这样 Main TA 就能够通过对应的 GOT Entry 找到共享库函数。

### 5.3 为什么还需要 dlsym()？

假设 TA 不想依赖编译阶段已经确定的函数引用，而希望在运行时查询：

```
void *func = dlsym(handle, "secure_hash");
```

这时会通过：

```
libdl
  |
  v
PTA_SYSTEM_DLSYM
  |
  v
system_dlsym()
  |
  v
ldelf_dlsym()
  |
  v
dlsym_entry()
  |
  v
Symbol Resolution
```

Core 最终将符号地址返回给 TA。

这与 Linux `dlsym()` 的基本概念类似，但 OP-TEE 使用自己的 UUID、系统调用和 Loader 实现。

## 六、一个关键安全问题：如果 TA 和 ldelf 共享地址空间，TA 能否攻击 Loader？

现在回到最初的问题。

我们已经知道：

```
TA Code       = URX
ldelf Code    = URX

TA Data       = URW
ldelf Data    = URW

TA Stack      = URW
ldelf Stack   = URW
```

那么，假设 TA 存在一个任意地址写漏洞：

```
*(uint64_t *)target_addr = 0xdeadbeef;
```

如果 `target_addr` 恰好是 ldelf 的代码地址，MMU 会阻止写操作。

但如果它指向 ldelf 的可写数据区域，单从页表权限来说，写操作可以成功。

这意味着我们需要从三个层次理解安全性。

### 6.1 第一层：Loader Code Integrity

这是最直接的保护。

```
ldelf Code
    |
    v
URX
    |
    +-- User Read: Allowed
    +-- User Write: Denied
    +-- User Execute: Allowed
```

即使 TA 拿到了 ldelf 代码的地址，也不能直接修改代码。

### 6.2 第二层：Loader Data Integrity

这里没有独立的硬件隔离。

ldelf Data 是 RW，TA 和 ldelf 在同一个用户地址空间中。

所以不能把 ldelf Data 当作与 TA 完全隔离的安全内存。

这背后是一个很重要的信任模型：

OP-TEE 主要保护的是 Secure Kernel 不被 User TA 直接破坏，而不是在同一个 TA 实例内部，让 Loader 和 TA 成为两个完全互不信任的进程。

TA 本身是经过认证的 Trusted Application。它的代码属于被信任的应用代码。

Loader 则是为了处理 ELF 等复杂输入而被降权运行。

这两种信任关系并不相同：

| 对象                | 信任定位              |
| ----------------- | ----------------- |
| OP-TEE Core       | 高权限可信计算基          |
| ldelf             | 低权限 Loader，处理复杂输入 |
| Main TA           | 经认证的应用代码          |
| Shared TA Library | 经认证的共享库代码         |
| REE 提供的存储内容       | 不应仅凭来源信任，必须经过验证   |

因此，不能认为 OP-TEE 提供了完整的 Loader-versus-TA 隔离。

### 6.3 第三层：Loader 与 Core 之间的安全边界

这一层才是最重要的。

即使 ldelf 被攻破，攻击者也只获得 S-EL0 的权限。

要进一步破坏 Core，仍然需要突破：

- 系统调用参数验证。
- 用户地址访问检查。
- 内存映射管理。
- 内存对象管理。
- Secure/Non-secure 内存隔离。
- S-EL0/S-EL1 权限边界。

比如前面看到的：

```
res = copy_from_user(
    &uuid,
    params[0].memref.buffer,
    sizeof(uuid));
```

以及：

```
if (arg_bbuf->flags & ~TA_FLAGS_MASK)
    return TEE_ERROR_BAD_FORMAT;
```

都是这种安全边界设计的具体体现。

特别是 `ldelf_loader.c` 中这段注释：

```
/*
 * This is already checked by the elf loader,
 * but since it runs in user mode we're not
 * trusting it entirely.
 */
```

这是非常值得记住的一段代码。

它直接告诉我们：

Core 不会因为 ldelf 是 OP-TEE 自己的 Loader，就完全信任它返回的数据。

这也是为什么 User Mode ELF Loader 设计非常有价值：它把复杂 ELF Parser 的漏洞影响范围限制在低权限执行环境，而不是直接把解析器放进 Kernel TCB。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 七、VM_FLAG_LDELF 与 TA Memory Management 的更深层关系

再进一步看 OP-TEE 的 VM Region 管理。

源码：

[core/mm/vm.c](https://github.com/OP-TEE/optee_os/blob/master/core/mm/vm.c?utm_source=chatgpt.com)

核心数据结构可以概念化为：

```
struct vm_region {
    vaddr_t va;
    size_t size;

    uint32_t flags;
    uint32_t attr;

    struct mobj *mobj;
    size_t offset;
};
```

具体字段定义以当前版本的 `core/include/mm/vm.h` 为准。

理解这几个字段：

| 字段       | 意义                  |
| -------- | ------------------- |
| `va`     | 用户虚拟地址              |
| `size`   | 映射大小                |
| `flags`  | VM Region 分类和生命周期属性 |
| `attr`   | 内存访问权限及属性           |
| `mobj`   | 映射背后的内存对象           |
| `offset` | 对应内存对象的偏移           |

你可以把它理解为：

```
VM Region
    |
    +-- Address Range
    |
    +-- Permission
    |
    +-- Memory Object
    |
    +-- Region Classification
```

这比简单地说：

```
VA → PA
```

更接近 OP-TEE 的实际内存管理模型。

### 7.1 为什么 Core 需要 VM Region 元数据？

假设 TA 申请一个动态库映射：

```
VA: 0x50000000
Size: 0x10000
Permission: URX
Flags: ...
Mobj: ...
```

Core 不只是建立页表，还需要记录：

- 这块内存属于哪个用户上下文。
- 它对应哪个内存对象。
- 是否允许移除。
- 是否属于临时参数映射。
- 是否属于 Loader。
- 是否属于共享映射。

后续如果要修改权限、释放内存或打印映射信息，都需要依赖这些元数据。

### 7.2 vm_set_prot() 为什么要做 TLB Invalidation？

回忆前面的代码：

```
r->attr &= ~TEE_MATTR_PROT_MASK;
r->attr |= prot;

set_um_region(uctx, r);

tlbi_va_range_asid(
    r->va,
    r->size,
    SMALL_PAGE_SIZE,
    uctx->vm_info.asid);
```

为什么要执行 `TLBI`？

因为 AArch64 MMU 不会在每次内存访问时都重新遍历完整页表。

TLB 会缓存 VA → PA 的转换结果以及相关权限属性。

如果 Core 修改了页表，但是没有正确处理 TLB，那么 CPU 可能继续使用旧的转换信息。

因此，权限修改必须配合必要的 TLB invalidation 和内存屏障。

此外，当原映射可写、现在需要改变访问属性时，OP-TEE 还处理 Data Cache Clean 和 Instruction Cache Invalidate。

这对我们理解 W^X 尤其重要：

```
PRW
 |
 | Core 写入代码
 v
URX
 |
 | 更新页表
 | TLBI
 | Cache maintenance
 v
S-EL0 执行代码
```

这不是简单修改一个 C 变量，而是实际改变 CPU 的内存访问行为。

[image](https://www.google.com/s2/favicons?domain=https://raw.githubusercontent.com\&sz=32)

GitHub



## 八、深入一个真实的 AArch64 场景：TA 调用 dlopen() 时 CPU 究竟经历了什么？

现在把整个过程放到 CPU 执行层面。

假设 TA 正在执行：

```
void *handle = dlopen(library_name, RTLD_NOW);
```

实际调用路径可以分成五个阶段。

## AArch64 执行时序

1. TA / S-EL0
   ```
   bl dlopen
   ```
   libdl 执行动态加载 API，并发起对 PTA_SYSTEM 的调用。
2. OP-TEE Core / S-EL1
   ```
   svc #0
   ```
   进入系统调用异常处理路径，Core 验证参数并执行 `system_dlopen()`。
3. Core 调度 ldelf
   ```
   ldelf_dlopen(uctx, &uuid, flags);
   ```
   将请求参数复制到 ldelf 用户态 Stack。
4. 重新进入 ldelf / S-EL0
   ```
   thread_enter_user_mode(
       ...,
       uctx->dl_entry_func,
       ...);
   ```
   通过 `ERET` 恢复 ldelf 的用户态执行状态，进入 `dl_entry()`。
5. 动态库加载完成
   ```
   dlopen_entry(arg);
   ```
   ldelf 返回 Core，再由 Core 返回原 TA。

这里有一个特别值得注意的细节：

TA → Core → ldelf → Core → TA。

这不是一个简单的函数调用栈。

它跨越了 CPU 的异常级别，涉及：

- SVC 异常进入。
- EL1 系统调用处理。
- 用户上下文保存。
- ldelf 入口参数准备。
- ELR_EL1 / SPSR_EL1 / SP_EL0 设置。
- ERET 返回 S-EL0。
- Loader 通过系统调用返回 Core。
- Core 恢复原 TA 的执行。

从系统软件角度，这才是完整的动态加载过程。

## 九、OP-TEE 的 dlopen() 与 Linux dlopen() 有哪些重要区别？

这一部分对你理解动态链接器设计非常有帮助。

| 维度     | Linux/glibc          | OP-TEE               |
| ------ | -------------------- | -------------------- |
| Loader | ld.so                | ldelf                |
| 库标识    | 文件路径                 | UUID                 |
| 库文件获取  | Linux 文件系统           | OP-TEE TA Storage    |
| 内存映射   | Linux mmap 等机制       | Core VM / System PTA |
| 动态重定位  | glibc Dynamic Linker | ldelf                |
| 系统服务   | Linux Syscalls       | OP-TEE Syscalls      |
| 执行环境   | Normal World         | Secure World         |
| 用户权限   | EL0                  | S-EL0                |

还有一个特别值得注意的限制。

OP-TEE 的共享库并不是完整复刻 Linux ELF Dynamic Linking ABI。

`pta_system.h` 对 `dlopen()` 的 Flags 有明确约束：

```
RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE
```

其中：

- `RTLD_NOW`：加载时立即解析需要的符号。
- `RTLD_GLOBAL`：使相关符号能够参与全局符号解析。
- `RTLD_NODELETE`：不采用普通 Linux 意义上的真正卸载语义。

所以，不要直接把 Linux glibc 的 Lazy Binding、Library Unload、Namespace 等行为套用到 OP-TEE。

[image](https://www.google.com/s2/favicons?domain=https://optee.readthedocs.io\&sz=32)

OP-TEE documentation documentation+1



## 十、建议重点阅读的源码：建立完整的调用链

这一部分我建议你直接在 OP-TEE 仓库里按以下顺序阅读。

| 顺序 | 文件                                 | 重点函数/定义                                 |
| -- | ---------------------------------- | --------------------------------------- |
| 1  | `lib/libutee/include/pta_system.h` | `PTA_SYSTEM_DLOPEN`、`PTA_SYSTEM_DLSYM`  |
| 2  | `lib/libdl/`                       | `dlopen()`、`dlsym()`、`dlclose()`        |
| 3  | `core/pta/system.c`                | `system_dlopen()`、`system_dlsym()`      |
| 4  | `core/kernel/ldelf_loader.c`       | `ldelf_dlopen()`、`ldelf_dlsym()`        |
| 5  | `ldelf/main.c`                     | `dl_entry()`                            |
| 6  | `ldelf/dl.c`                       | `dlopen_entry()`、`dlsym_entry()`        |
| 7  | `ldelf/ta_elf.c`                   | ELF Segment Loading                     |
| 8  | `ldelf/ta_elf_rel.c`               | Dynamic Relocation                      |
| 9  | `core/mm/vm.c`                     | `vm_map()`、`vm_set_prot()`、`vm_unmap()` |

建议特别关注 `ldelf_loader.c` 中的：

```
uctx->dl_entry_func = arg_bbuf->dl_entry;
```

以及：

```
res = thread_enter_user_mode(
    usr_stack,
    0, 0, 0,
    usr_stack,
    uctx->dl_entry_func,
    is_32bit,
    &panicked,
    &panic_code);
```

这两处把初次加载和后续动态加载连接起来。

## 十一、Staff Engineer 级别的架构思考题

这里给你五个问题。这些问题不要求背诵 OP-TEE 源码，而是考察你能否从安全边界、内存管理和动态链接器设计角度进行推导。

问题 1：如果 TA 可以修改 ldelf Data，为什么 OP-TEE 仍然可以接受这种设计？

进一步考虑：

- TA 与 ldelf 是否属于同一个信任域？
- Loader Code 和 Loader Data 是否需要相同的保护等级？
- 如果 TA 破坏了 ldelf 的动态链接状态，最坏会影响什么？
- 为什么这不应该导致 Core 被破坏？

问题 2：假设攻击者利用 TA 的内存漏洞，修改 ldelf 的 GOT Entry，使下一次 `dlopen()` 调用跳转到攻击者构造的代码，是否能够突破 S-EL0/S-EL1 隔离？

重点分析：

- GOT 所在页面权限。
- 函数指针的完整性。
- BTI 能够防御什么，不能防御什么。
- S-EL0 执行任意代码与 S-EL1 权限提升的区别。
- Core 是否应该把 Loader 的返回值视为可信数据。

问题 3：如果我们希望真正隔离 Loader 和 TA，能否给 ldelf 建立一个独立的 ASID 和页表？

思考：

- Loader 与 TA 是否需要共享 ELF 映射？
- 动态加载后的库如何映射到 TA？
- 是否需要跨地址空间共享物理页？
- 是否需要 IPC/RPC 式的 Loader Service？
- 性能和 TCB 复杂度如何变化？

问题 4：如果动态库的 ELF 被恶意构造，利用 Relocation 解析中的整数溢出漏洞，可能产生什么后果？

从以下角度分析：

- `r_offset + load_bias`。
- `r_addend + load_bias`。
- Relocation Target 是否位于合法 Segment。
- GOT 是否可写。
- 是否可能覆盖其他 ELF Segment。
- Core 是否需要对 Loader 的映射请求进行二次验证？

问题 5：如果将这套 OP-TEE ldelf 设计迁移到你正在研究的 RISC-V Confidential Computing 架构中，你会如何重新设计 Loader？

特别考虑：

- M/S/U Privilege。
- Sv39/Sv48 Page Table。
- ASID。
- PMP 与页表的职责区别。
- S-Mode Kernel 和 U-Mode Loader 的边界。
- `ecall` / `sret`。
- Shared Library 的物理内存共享与虚拟地址映射。
- SMMPT 对不同 Domain 内存访问的约束。

## 十二、本章最终需要建立的架构认知

我们把今天的内容压缩成三个重要结论：

第一：ldelf 和 TA 同处一个用户地址空间，不代表它们的所有内存拥有相同权限。

ldelf Code 是 URX，TA 无法通过普通用户态写操作修改它。但 ldelf Data/Stack 是 RW，不能把 `VM_FLAG_LDELF` 当成完整的硬件隔离机制。

第二：OP-TEE 的安全边界主要在 User TA 与 Secure Kernel 之间，而不是 ldelf 与 TA 之间。

Loader 降权运行的主要目的是限制 ELF Parser 漏洞的影响范围。Core 仍然负责所有关键的特权资源管理，并对 Loader 返回的数据进行必要的重新验证。

第三：ldelf 不只是一个一次性的 ELF Loader，而是一个被 Core 保留并按需重新调度的用户态动态加载服务。

它通过 `dl_entry_func` 暴露专门的运行时入口，由 Core 通过 `thread_enter_user_mode()` 再次进入 S-EL0 执行。

这三个结论连接起来，你就能真正理解 OP-TEE 为什么采用这种架构，而不只是知道 `ldelf_load_ldelf()` 和 `dlopen_entry()` 分别做什么。

### 推荐继续深入的下一个主题

我建议下一步研究一个与当前内容直接相关、而且对你的 RISC-V SMMPT + OP-TEE 工作尤其有价值的主题：

OP-TEE VM Region、Mobj、Page Table 与 SMMPT/Domain Isolation 的关系。

重点从一个实际问题出发：

当 ldelf 调用 `sys_map_zi()` 或 `sys_map_ta_bin()` 时，Core 如何从一个用户态虚拟地址请求，最终建立安全物理内存映射？如果 TA、Shared Library 和不同 Security Domain 之间需要共享或隔离物理页面，应该在哪一层实施约束？

这会把 ELF Loader、AArch64 MMU、OP-TEE Memory Management 和 RISC-V Confidential Computing 真正串成一套完整的系统架构。
