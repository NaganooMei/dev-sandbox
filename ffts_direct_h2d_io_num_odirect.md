# FFTS Direct H2D io_num 与 O_DIRECT 说明

## 当前 direct H2D 实现

`ffts-direct-h2d` 是 Ascend copy benchmark 中的 direct FFTS SDMA H2D 路径。它不经过 CE staging buffer，而是直接把 `aclrtHostGetDevicePointer` 返回的 device-visible mapped host pointer 写入 FFTS SDMA descriptor 的 source，把 device buffer pointer 写入 destination。

当前 direct H2D 包含三个 case：

| Case 名称 | 源 buffer | 目标 buffer | 含义 |
| --- | --- | --- | --- |
| `all_host_to_all_device_ffts_direct_h2d` | 每个子进程各自分配一块 `aclrtMallocHost` host buffer，并注册为 mapped host memory | 每张卡一块 device buffer | all host to all device |
| `one_share_host_to_all_device_ffts_direct_h2d` | 父进程创建一块 POSIX shared memory，所有子进程 mmap 同一块源 buffer，并在各自进程里注册为 mapped host memory | 每张卡一块 device buffer | one shared host to all device |
| `all_odirect_host_to_all_device_ffts_direct_h2d` | 每个子进程分配一块 UCM O_DIRECT local buffer 形态的 anonymous mmap host buffer，并注册为 mapped + pinned host memory | 每张卡一块 device buffer | local direct-IO style host buffer to all device |

校验默认关闭。需要调试数据正确性时，设置 `COPY_FFTS_VALIDATE=1`，程序会初始化确定性的 host pattern，通过 FFTS SDMA 拷贝，再把 device buffer 读回 host 做比较。

## `-n` 与 `--frags`

copy benchmark 原来已经有全局 `-n` 参数。为了兼容旧逻辑，direct H2D 现在分成两种模式：

| 模式 | 命令形式 | 含义 |
| --- | --- | --- |
| 兼容模式 | 不传 `--frags` 或 `-f` | `-n` 仍表示总 fragment 数，direct H2D 会把所有 fragment 合成一个 FFTS task 下发。这和旧行为一致。 |
| 多 task 模式 | 传 `--frags <count>` 或 `-f <count>` | `-n` 表示 IO/task 数量，`--frags` 表示每个 IO/task 内包含多少个 fragment。程序会分配 `-n * --frags` 个 fragment，并在每次迭代里下发 `-n` 个 FFTS task。 |

示例：

```bash
# 旧行为：1000 个 fragment 合并为 1 个 FFTS task。
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 -i 10 -d 8

# 新行为：1000 个 IO/task，每个 task 里 1 个 fragment。
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 --frags 1 -i 10 -d 8

# 新行为：1000 个 IO/task，每个 task 里 4 个 fragment，每张卡共 4000 个 fragment。
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 --frags 4 -i 10 -d 8
```

默认 benchmark 配置建议固定 lanes 为 8：

```bash
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_odirect_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_odirect_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 --frags 1 -i 10 -d 8
```

## 提交流程

direct H2D copy instance 在 `Prepare` 阶段会为每个 fragment 构造一个 copy spec：

```text
mapped host source pointer -> device destination pointer -> fragment size
```

兼容模式下，所有 copy spec 放进同一个 task group，只调用一次 FFTS dispatcher。多 task 模式下，copy spec 会按 `--frags` 分组，每个分组单独调用一次 dispatcher。因此 `-n 1000 --frags 1` 表示每次统计迭代、每张卡会调用 1000 次 `rtFftsPlusTaskLaunchWithFlag`。

输出结果里的 `Count` 仍然按总 fragment 数统计，不只按 task 数统计：

```text
单卡 Count = -n * --frags
聚合 Count = -n * --frags * device_count
```

## UCM 开启 O_DIRECT 后的 host buffer 形态

UCM local CacheStore buffer 有两种 host 分配路径：

| 条件 | 分配路径 | 实际内存形态 |
| --- | --- | --- |
| `io_direct=false` | `MakeHostBuffer` | `aclrtMallocHost` |
| `io_direct=true` 且 shared buffer 关闭 | `MakeHostBuffer4DirectIo` | anonymous private `mmap`，先尝试 HugeTLB 或 gigantic HugeTLB，失败后 fallback 到 transparent hugepage advice，然后 `mlock`，再 `aclrtHostRegisterV2(MAPPED | PINNED)` |

新增的 dev-sandbox case `all_odirect_host_to_all_device_ffts_direct_h2d` 对齐第二种形态。它会分配 anonymous mmap host memory，优先尝试 HugeTLB/gigantic HugeTLB，失败后使用 THP advice fallback，然后用 `ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED` 注册，并通过 `aclrtHostGetDevicePointer` 拿到 FFTS descriptor 可用的 mapped pointer。

## shared memory + O_DIRECT 的实际形态

UCM 开启 shared memory 后，不会把 shared transfer buffer 切到 local `MakeHostBuffer4DirectIo` 路径。shared buffer 仍然是 POSIX shared memory：

```text
shm_open/ftruncate -> mmap(MAP_SHARED) -> page-aligned data area -> aclrtHostRegisterV2(MAPPED | PINNED)
```

PCStore 里 `ioDirect` 会保存在 reader 上，影响的是文件读写时是否用 `O_DIRECT` 打开文件；shared host buffer 本身仍是 POSIX shared memory，并注册为 mapped + pinned host memory。因此 dev-sandbox 当前已有的 `one_share_host_to_all_device_ffts_direct_h2d` 就对应 shared memory + O_DIRECT 的 host buffer 形态。

## runtime FFTS launch 接口用法

当前 dispatcher 会包含 `runtime/rt_ffts_plus.h` 或 `rt_external_ffts.h`，然后构造 FFTS Plus task descriptor：

| 字段 | 当前取值或含义 |
| --- | --- |
| `rtFftsPlusSqe_t::fftsType` | `RT_FFTS_PLUS_TYPE` |
| `rtFftsPlusSqe_t::totalContextNum` | 当前 task 里的 SDMA context 数量 |
| `rtFftsPlusSqe_t::readyContextNum` | 初始 ready 的 context 数量，由 `FFTS_MAX_READY_LANES` 控制，默认 8 |
| `rtFftsPlusSqe_t::preloadContextNum` | `readyContextNum` 和 128 的较小值 |
| `rtFftsPlusSqe_t::timeout` | 0 |
| `rtFftsPlusSqe_t::subType` | `0x5A`，当前作为 communication task 使用 |
| `rtFftsPlusTaskInfo_t::descBuf` | context descriptor array 的 host 地址 |
| `rtFftsPlusTaskInfo_t::descBufLen` | descriptor array 的字节长度 |
| `rtFftsPlusTaskInfo_t::descAddrType` | `RT_FFTS_PLUS_CTX_DESC_ADDR_TYPE_HOST` |
| `rtFftsPlusTaskInfo_t::argsHandleInfoNum` | 0 |
| `rtFftsPlusTaskInfo_t::argsHandleInfoPtr` | null |

每个 descriptor 是一个 `rtFftsPlusSdmaCtx_t`，通过公共的 128 字节 context 类型承载。当前 context 设置 `contextType = RT_CTX_TYPE_SDMA`，填充 source/destination 地址高低位，以及数据长度字段。

本 checkout 没有 runtime 头文件副本，因此本机无法枚举上游 runtime 支持的全部 FFTS task/context type。当前 dev-sandbox 和 UCM 可见代码只使用 `RT_FFTS_PLUS_TYPE` + `RT_CTX_TYPE_SDMA`。后续如果要扩展其他类型，应基于目标机 runtime 头文件里的其他 `RT_CTX_TYPE_*` layout 新增 dispatcher；如果 runtime 要求不同 task class，再调整 `subType`，同时保持当前 SDMA 路径不变。

## 修改点总结

- copy CLI 新增 `--frags` 和 `-f`。
- direct H2D 默认不传 `--frags` 时仍保持旧行为：所有 fragment 只下发一个 FFTS task。
- 传 `--frags` 后，direct H2D 会按 task 拆分，多次调用 `rtFftsPlusTaskLaunchWithFlag`。
- 校验默认关闭，继续保留 `COPY_FFTS_VALIDATE=1` 手动开启。
- 新增 `all_odirect_host_to_all_device_ffts_direct_h2d`，用于覆盖 UCM local O_DIRECT 风格 host memory。
- 明确 shared memory + O_DIRECT 仍对应 POSIX shared memory + mapped/pinned register，dev-sandbox 由 `one_share_host_to_all_device_ffts_direct_h2d` 覆盖。

## 测试方式

本地静态检查：

```bash
git diff --check
```

有 CMake 的环境可做编译检查：

```bash
cmake -B build
cmake --build build -j
```

Ascend 机器上的 runtime smoke：

```bash
# 默认关闭校验，兼容模式。
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 8 -i 1 -d 1

# 手动开启校验。
FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 8 -i 1 -d 1

# 多 task 模式：8 个 task，每个 task 1 个 fragment。
FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 8 --frags 1 -i 1 -d 1

# O_DIRECT local host buffer 形态。
FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t all_odirect_host_to_all_device_ffts_direct_h2d -s 32K -n 8 --frags 1 -i 1 -d 1

# shared memory + O_DIRECT 对应的 shared host buffer 形态。
FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 32K -n 8 --frags 1 -i 1 -d 1
```
