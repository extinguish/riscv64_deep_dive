# OP-TEE xtest 测试目标与源码分布速查表

基于最新公开的 `optee_test` 和 `optee_os` master 源码，我按测试编号整理了一份映射表。

首先记住三个目录：

| 源码目录                       | 作用                             |
| -------------------------- | ------------------------------ |
| `optee_test/host/xtest/`   | Normal World 测试用例入口            |
| `optee_test/ta/`           | 独立 User TA 测试实现                |
| `optee_os/core/pta/tests/` | OP-TEE Core 内部测试，通过 PTA 暴露测试接口 |

关键点：一个 xtest 用例可能同时测试多种组件，不一定只对应一个 TA。 因此下面区分“测试入口”和“实际测试目标”。

## 一、xtest 1000 系列：核心功能测试

这一组最适合用于理解 OP-TEE 的 TA 管理、Session、内存隔离和 Core 内部机制。

所有测试入口都在：

`optee_test/host/xtest/regression_1000.c`

| xtest | 测试内容                            | OP-TEE 侧主要目标源码                                                            | 类型      |
| ----- | ------------------------------- | ------------------------------------------------------------------------- | ------- |
| 1001  | Core Self Tests                 | `core/pta/tests/invoke.c` → `misc.c`、`dt_driver_test.c`、`transfer_list.c` | PTA     |
| 1002  | PTA 参数传递                        | `core/pta/tests/invoke.c`                                                 | PTA     |
| 1003  | Core 读写锁 / Mutex                | `core/pta/tests/invoke.c` → `mutex.c`                                     | PTA     |
| 1004  | Crypto User TA                  | `optee_test/ta/crypt/`                                                    | User TA |
| 1005  | 多 Session 管理                    | `optee_test/ta/concurrent/`                                               | User TA |
| 1006  | Basic OS Features               | `optee_test/ta/os_test/`                                                  | User TA |
| 1007  | TA Panic                        | `optee_test/ta/os_test/`                                                  | User TA |
| 1008  | Internal Client API、TA 加载验证     | `ta/os_test/`、`ta/create_fail_test/`、`core/pta/secstor_ta_mgmt.c`         | 混合      |
| 1009  | TEE Wait、Cancellation           | `optee_test/ta/os_test/`                                                  | User TA |
| 1010  | 非法内存访问及异常隔离                     | `optee_test/ta/os_test/`                                                  | User TA |
| 1011  | TA-to-TA Communication          | `optee_test/ta/rpc_test/`、`ta/crypt/`                                     | User TA |
| 1012  | Single Instance / Multi Session | `optee_test/ta/sims/`                                                     | User TA |
| 1013  | TA Concurrency                  | `optee_test/ta/concurrent/`                                               | User TA |
| 1014  | Secure Data Path                | `ta/sdp_basic/`、`core/pta/tests/invoke.c`                                 | 混合      |
| 1015  | FS Hash Tree Corner Cases       | `core/pta/tests/invoke.c` → `fs_htree.c`                                  | PTA     |
| 1016  | TA-to-TA Memref                 | `optee_test/ta/os_test/`                                                  | User TA |

路径以各仓库根目录为基准。表中的箭头表示调用关系；部分测试还会进入 OP-TEE OS 的 TA Manager、Memory Manager、Storage 等内部实现。

源码：[regression_1000.c](https://github.com/OP-TEE/optee_test/blob/master/host/xtest/regression_1000.c?utm_source=chatgpt.com)

### 特别看一下 xtest 1001

这是你提到的最典型的例子。

```
xtest 1001
  |
  |  optee_test/host/xtest/regression_1000.c
  |
  +-- TEEC_OpenSession(PTA_INVOKE_TESTS_UUID)
  |
  +-- TEEC_InvokeCommand()
        |
        +-- optee_os/core/pta/tests/invoke.c
              |
              +-- PTA_INVOKE_TESTS_CMD_SELF_TESTS
              |     +-- misc.c
              |
              +-- PTA_INVOKE_TESTS_CMD_DT_DRIVER_TESTS
              |     +-- misc.c
              |     +-- dt_driver_test.c (配置启用时)
              |
              +-- PTA_INVOKE_TESTS_CMD_TRANSFER_LIST_TESTS
                    +-- transfer_list.c (配置启用时)
```

所以 xtest 1001 是通过 `invoke_tests.pta` 执行 Core 内部测试的，并不是独立 User TA。

源码中的关键分派逻辑：

```
case PTA_INVOKE_TESTS_CMD_SELF_TESTS:
    return core_self_tests(nParamTypes, pParams);

case PTA_INVOKE_TESTS_CMD_DT_DRIVER_TESTS:
    return core_dt_driver_tests(nParamTypes, pParams);

case PTA_INVOKE_TESTS_CMD_TRANSFER_LIST_TESTS:
    return core_transfer_list_tests(nParamTypes, pParams);
```

其中 `core_self_tests()` 的实现位于 `core/pta/tests/misc.c`，不是 `invoke.c`。这正是区分 PTA Dispatch 和实际测试逻辑的意义。

源码：[invoke.c](https://github.com/OP-TEE/optee_os/blob/master/core/pta/tests/invoke.c?utm_source=chatgpt.com)

## 二、其他主要测试系列

| 测试编号      | 测试方向                      | 主要目标代码                                     | 类型                |
| --------- | ------------------------- | ------------------------------------------ | ----------------- |
| 2001–2004 | TCP/UDP Socket API        | `optee_test/ta/socket/`                    | User TA           |
| 4001      | Hash / Digest             | `optee_test/ta/crypt/`                     | User TA           |
| 4002      | MAC                       | `optee_test/ta/crypt/`                     | User TA           |
| 4003      | Cipher                    | `optee_test/ta/crypt/`                     | User TA           |
| 4004      | Random Number             | `optee_test/ta/crypt/`                     | User TA           |
| 4005      | Authenticated Encryption  | `optee_test/ta/crypt/`                     | User TA           |
| 4006、4007 | RSA、ECC、DSA 等             | `optee_test/ta/crypt/`                     | User TA           |
| 5006      | GlobalPlatform Client API | `optee_client/libteec/`、Linux TEE Driver 等 | 主要是 Client API 测试 |
| 6001–6017 | Secure Storage            | `optee_test/ta/storage/`                   | User TA           |
| 8001      | HKDF、PBKDF2 等 KDF         | `optee_test/ta/crypt/`                     | User TA           |
| 8101–8103 | mbedTLS、证书处理              | `optee_test/ta/crypt/`                     | User TA           |

其中 5006 主要直接验证 Client API，并不以某个特定测试 TA 为唯一目标；某些测试子项甚至不会进入 Secure World。

上述编号是常用用例的归类，不是所有已注册测试的完整列表。

相应的测试入口可在以下源码中找到：

[host/xtest 测试源码目录](https://github.com/OP-TEE/optee_test/tree/master/host/xtest?utm_source=chatgpt.com)

## 三、测试 TA 与 OP-TEE OS 真正被测试的代码

还有一点值得特别强调：User TA 通常只是测试驱动端，不代表被测功能的最终实现都在 User TA 内。

例如 xtest 6001 验证持久化对象创建：

```
xtest 6001
  |
  +-- optee_test/ta/storage/
        |
        +-- TEE_CreatePersistentObject()
              |
              +-- OP-TEE Internal Core API
                    |
                    +-- optee_os/core/tee/
                          |
                          +-- Storage 实现
                          +-- REE FS / RPMB FS
```

因此可以再按真正的 OP-TEE OS 子系统建立以下索引：

| 功能领域                | 重点关注的 OP-TEE OS 目录                                            |
| ------------------- | ------------------------------------------------------------- |
| PTA 调度              | `core/kernel/pseudo_ta.c`                                     |
| User TA 加载          | `core/kernel/user_ta.c`、`core/kernel/ldelf_loader.c`、`ldelf/` |
| Session 管理          | `core/kernel/tee_ta_manager.c`                                |
| Crypto              | `core/crypto/`、`lib/libutee/`                                 |
| Secure Storage      | `core/tee/`、`core/tee/tee_svc_storage.c`                      |
| 内存管理                | `core/mm/`                                                    |
| 内部 Mutex、Self Tests | `core/pta/tests/`                                             |

这样可以把一次测试区分为三个层次：测试用例、测试 TA，以及最终被测试的 Core 子系统。

## 四、对于 OP-TEE 移植，我建议优先关注的测试

根据你目前做 RISC-V OP-TEE 移植的方向，优先级可以这样安排：

| 优先级 | xtest          | 主要验证目标                                |
| --- | -------------- | ------------------------------------- |
| P0  | 1002           | PTA Session、Memref、Command Dispatch   |
| P0  | 1006           | User TA ELF 加载及基本执行                   |
| P0  | 1007、1010      | TA Panic、异常处理及隔离                      |
| P0  | 1005、1012、1013 | 多 Session、并发与 TA Context 管理           |
| P1  | 1008、1011      | TA 加载验证、TA-to-TA 通信                   |
| P1  | 4001–4005      | Crypto Internal API                   |
| P1  | 6001–6017      | Secure Storage                        |
| P2  | 1001、1003、1015 | Core 内部 Self Tests、Mutex、FS Hash Tree |

注意，1001、1002、1003、1015 等测试依赖对应的测试 PTA；需要检查 `CFG_TEE_CORE_EMBED_INTERNAL_TESTS` 及平台配置。部分测试缺少对应组件时会跳过，Skipped 不等于 Passed。

建议你以后定位任何一个 xtest 用例时，都按固定顺序分析：

`regression_xxxx.c` → `TA UUID` → `PTA/User TA` → `Command ID` → `具体测试函数` → `OP-TEE OS 底层实现`

按照这条路径，就能很快定位真正的测试目标，而不会把 xtest、测试 TA 和 OP-TEE Core 的功能实现混为一谈。
