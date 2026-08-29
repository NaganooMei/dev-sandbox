# dev-sandbox Ascend copy benchmark

这个分支的 README 只整理 Ascend copy 相关 case，用来观察 H2D、D2D、单 host 多卡读、POSIX shared memory、HugeTLB shared memory、multi-stream、H2D FFTS pipeline 和 H2D FFTS direct 的带宽表现。

## 构建

```bash
cmake -B build
cmake --build build -j
```

构建完成后，`copy` 可执行文件通常位于：

```bash
./build/module/copy/copy
```

查看当前构建里实际注册的 case：

```bash
./build/module/copy/copy -t unknown
```

## 通用参数

```text
-t <name>   case 名称，可重复指定多个 -t
-s <size>   单个数据块大小，例如 16K、1M、512M
-n <count>  每个 buffer 里的数据块数量
-f <count>  FFTS 每个 task 的 fragment 数；glm5.1 模式固定为 3
-S <count>  每张卡的 stream 数；CE 默认 48，FFTS Direct 默认 1
--io-mode uniform|glm5.1
             IO 布局；glm5.1 下一个 block 固定包含 128K、16K、32K
--submit-mode stream-major|round-robin
             按 stream 整批下发，或按 block 轮询 stream 下发
-i <count>  统计迭代次数
-d <count>  设备数量
```

`Size(KB)` 是单个数据块大小，`Count` 是一次统计结果覆盖的数据块数量。聚合 case 的 `Count` 可能会等于 `-n * -d`，per-device case 则通常每张卡一行，每行 `Count = -n`。

## Fork 子进程 CPU 亲和性

所有使用 fork fan-out 的 Ascend case 默认不主动绑核。子进程继承启动进程已有的 CPU affinity，因此如果需要限制 CPU 范围，建议在外部用 `taskset`、`numactl` 或调度系统统一控制。

## Case 分类

### 基础 CE

这些 case 用 Ascend copy engine 做基础拷贝，适合做单卡或逐卡基线。

| case | 传输方向 | 输出口径 | 说明 |
| --- | --- | --- | --- |
| `host_to_device_ce` | host -> device | 每张卡一行 | 逐设备 H2D CE 拷贝 |
| `host_to_device_batch_ce` | host -> device | 每张卡一行 | 单卡内使用 `aclrtMemcpyBatchAsync` 提交 H2D |
| `device_to_device_ce` | device -> device | 每张卡一行 | 单设备内 D2D CE 拷贝 |
| `one_device_to_all_device_ce` | device0 -> all devices | 每张卡一行 | 同一块 device0 buffer 依次拷贝到所有 device |
| `anonymous_to_device_ce` | anonymous host -> device | 每张卡一行 | 匿名 host 内存注册后拷贝到 device |

常用基线：

```bash
./build/module/copy/copy -t host_to_device_ce -s 1M -n 64 -i 100 -d 8
```

### 单 Host 多卡 CE

这一组专门用来看“同一份 host 数据被多卡读取”时是否发生带宽冲突。名字相近，但语义不同。

| case | 源 buffer | 提交方式 | 输出口径 | 用途 |
| --- | --- | --- | --- | --- |
| `one_host_to_all_device_ce` | 一块 `aclrtMallocHost` host0 buffer | 主进程逐卡顺序执行 | 每张卡一行 | 顺序读基线，不是多卡同时读 |
| `one_host_to_all_device_ce_batch` | 一块 `aclrtMallocHost` host0 buffer | 主进程一次 `DoCopyBatch` 覆盖所有 device | 聚合一行 | 同一块 host0 同时发往多卡的聚合带宽 |
| `one_share_host_to_all_device_ce` | 一块 POSIX shared memory host buffer | fork fan-out，每个子进程一张卡 | 聚合一行 | 多进程同时读同一块 shared host 的整体表现 |
| `one_share_host_to_all_device_ce_per_device` | 一块 POSIX shared memory host buffer | fork fan-out，每个子进程一张卡 | 每张卡一行 | 多进程同时读同一块 shared host 时观察每张卡带宽 |
| `all_host_to_all_device_ce` | 每张卡各自一块 host buffer | fork fan-out，每个子进程一张卡 | 聚合一行 | 多卡并发但源 buffer 不共享，用来区分共享源冲突和普通并发开销 |

建议对比：

```bash
./build/module/copy/copy -t one_host_to_all_device_ce -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t one_host_to_all_device_ce_batch -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t one_share_host_to_all_device_ce_per_device -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t all_host_to_all_device_ce -s 1M -n 64 -i 100 -d 8
```

### HugeTLB Shared Host

HugeTLB case 使用 `memfd_create` 搭配 `MFD_HUGETLB` 和 `MFD_HUGE_2MB` 创建 2 MiB HugeTLB 页，不需要挂载 hugetlbfs。宿主机需要提前预留 HugeTLB 页。

| case | 源 buffer | 提交方式 | 输出口径 | 说明 |
| --- | --- | --- | --- | --- |
| `huge_shm_to_device_ce` | 每次创建一块 HugeTLB shared host buffer | 逐设备执行 | 每张卡一行 | HugeTLB H2D CE 基线 |
| `one_huge_shm_to_all_device_ce` | 父进程创建一块 HugeTLB shared host buffer | fork fan-out，每个子进程继承 fd | 聚合一行 | 多卡同时读同一块 HugeTLB shared host 的整体表现 |
| `one_huge_shm_to_all_device_ce_per_device` | 父进程创建一块 HugeTLB shared host buffer | fork fan-out，每个子进程继承 fd | 每张卡一行 | 多卡同时读同一块 HugeTLB shared host 时观察每张卡带宽 |

预留 HugeTLB 页示例：

```bash
echo 8192 > /proc/sys/vm/nr_hugepages
```

一次性查看 HugeTLB 状态：

```bash
grep -i Huge /proc/meminfo
```

运行时持续观察 HugeTLB 使用量：

```bash
watch -n 0.2 'grep -i Huge /proc/meminfo'
```

重点看 `HugePages_Free`。例如 `-s 1M -n 64` 会申请 64 MiB，2 MiB huge page 下会消耗 32 页。这个 memfd HugeTLB 路径通常不会让 `ShmemHugePages` 增加。

建议对比：

```bash
./build/module/copy/copy -t one_huge_shm_to_all_device_ce -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t one_huge_shm_to_all_device_ce_per_device -s 1M -n 64 -i 100 -d 8
```

### Multi-Stream CE

multi-stream case 在单卡内使用多个 stream 提交 H2D。默认 stream 数为 48，可通过 `-S`、`--streams` 或 `--stream-count` 调整。

`--submit-mode stream-major` 保留原来的连续分块、逐 stream 下发行为；`--submit-mode round-robin` 按 block 在 stream 间轮询下发。glm5.1 模式下，一个 block 的 128K、16K、32K 三条 IO 始终进入同一个 stream，不会被轮询拆开。

| case | 源 buffer | 提交方式 | 输出口径 | 说明 |
| --- | --- | --- | --- | --- |
| `host_to_device_ce_multi_stream` | 每张卡各自一块 host buffer | 单卡多 stream | 每张卡一行 | 单卡 multi-stream H2D 基线 |
| `one_host_to_all_device_ce_multi_stream` | 一块 `aclrtMallocHost` host0 buffer | 主进程一次 batch 覆盖所有 device，单卡内多 stream | 聚合一行 | 同一块 host0 同时发往多卡，观察 multi-stream 聚合表现 |
| `one_share_host_to_all_device_ce_multi_stream` | 一块 POSIX shared memory host buffer | fork fan-out，单卡内多 stream | 聚合一行 | shared host 多进程 fan-out + 单卡 multi-stream |
| `all_host_to_all_device_ce_multi_stream` | 每张卡各自一块 host buffer | fork fan-out，单卡内多 stream | 聚合一行 | 非共享源的多卡并发 multi-stream |
| `all_odirect_host_to_all_device_ce_multi_stream` | 每张卡各自一块 UCM O_DIRECT 风格 anonymous mmap host buffer | fork fan-out，单卡内多 stream | 聚合一行 | 对比 `aclrtMallocHost` 版本，覆盖 O_DIRECT local host buffer 形态 |

建议和普通 CE batch 对比：

```bash
./build/module/copy/copy -t one_host_to_all_device_ce_batch -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t one_host_to_all_device_ce_multi_stream -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t all_host_to_all_device_ce_multi_stream -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t all_odirect_host_to_all_device_ce_multi_stream -s 1M -n 64 -i 100 -d 8
```

### H2D FFTS Pipeline

这一组只有在构建环境检测到 Ascend FFTS 头文件和 `libruntime.so` 时才会编译。它把 host 数据先 H2D 写入 device staging buffer，再用 FFTS split 到 fragmented device buffer。

| case | 源 buffer | 目标 buffer | 输出口径 | 说明 |
| --- | --- | --- | --- | --- |
| `host_to_device_ffts_pipeline` | 每张卡各自一块 host buffer | fragmented device | 每张卡一行 | H2D FFTS pipeline 基线 |
| `huge_shm_to_device_ffts_pipeline` | 每次创建一块 HugeTLB shared host buffer | fragmented device | 每张卡一行 | HugeTLB 源的 H2D FFTS pipeline |
| `one_host_to_all_device_ffts_pipeline` | 一块 host0 buffer | fragmented device | 聚合一行 | 同一块 host0 发往所有 device |
| `one_share_host_to_all_device_ffts_pipeline` | 一块 POSIX shared memory host buffer | fragmented device | 聚合一行 | POSIX shared host fork fan-out |
| `one_huge_shm_to_all_device_ffts_pipeline` | 一块 HugeTLB shared host buffer | fragmented device | 聚合一行 | HugeTLB shared host fork fan-out |
| `all_host_to_all_device_ffts_pipeline` | 每张卡各自一块 host buffer | fragmented device | 聚合一行 | 非共享源的多卡 fan-out |
| `all_odirect_host_to_all_device_ffts_pipeline` | 每张卡各自一块 UCM O_DIRECT 风格 anonymous mmap host buffer | fragmented device | 聚合一行 | O_DIRECT 源的 H2D staging + FFTS pipeline |

相关环境变量：

```text
COPY_FFTS_VALIDATE=1
COPY_FFTS_PIPELINE_OBJECT_FRAGS=8
FFTS_MAX_READY_LANES=8
```

`COPY_FFTS_VALIDATE` 控制正确性校验。`COPY_FFTS_PIPELINE_OBJECT_FRAGS` 控制一个 logical object 包含多少个 fragment。`FFTS_MAX_READY_LANES` 控制 FFTS dispatcher 初始 ready 的 SDMA context 数量。

正确性 smoke：

```bash
COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t host_to_device_ffts_pipeline -s 32K -n 32 -i 4 -d 1
```

HugeTLB FFTS smoke：

```bash
COPY_FFTS_VALIDATE=1 COPY_FFTS_PIPELINE_OBJECT_FRAGS=8 \
./build/module/copy/copy -t one_huge_shm_to_all_device_ffts_pipeline -s 32K -n 1024 -i 16 -d 8
```

O_DIRECT FFTS pipeline smoke：

```bash
COPY_FFTS_VALIDATE=1 COPY_FFTS_PIPELINE_OBJECT_FRAGS=8 \
./build/module/copy/copy -t all_odirect_host_to_all_device_ffts_pipeline -s 32K -n 1024 -i 16 -d 8
```

### H2D FFTS Direct

当前 direct H2D 最新用法如下：

- `COPY_FFTS_VALIDATE` 默认不设置，即默认不做数据校验；需要调试正确性时再设置 `COPY_FFTS_VALIDATE=1`。
- 不传 `--frags`、`-frags` 或 `-f` 时，`-n` 仍表示总 fragment 数，所有 fragment 合并为一个 FFTS task 下发，兼容旧行为。
- 传 `--frags <count>`、`-frags <count>` 或 `-f <count>` 时，`-n` 表示 IO/task 数量，`frags` 表示每个 IO/task 内的 fragment 数量。
- `-S`、`--streams` 或 `--stream-count` 控制每张卡的 FFTS stream 数，默认值为 1。实际 stream 数不会超过 task/block 数。
- `--submit-mode stream-major` 先下发一个 stream 的全部 task，再处理下一个 stream；`--submit-mode round-robin` 按 task/block 轮询 stream 下发。
- `--io-mode glm5.1 -f 3` 启用服务 IO 布局：`-n` 表示 block 数，每个 block 是一个 FFTS task，内部包含 128K、16K、32K 三个 SDMA context，总计 176K。
- `all_odirect_host_to_all_device_ffts_direct_h2d` 用于覆盖 UCM local O_DIRECT 风格 host buffer，也就是 anonymous mmap + HugeTLB/THP fallback + mapped/pinned register。
- `one_share_host_to_all_device_ffts_direct_h2d` 对应 shared memory + O_DIRECT 的 host buffer 形态，因为 UCM shared buffer 在 O_DIRECT 下仍是 POSIX shared memory + mapped/pinned register。

详细说明见 `ffts_direct_h2d_io_num_odirect.md`。

这一组只有在构建环境检测到 Ascend FFTS 头文件和 `libruntime.so` 时才会编译。它不经过 CE staging，FFTS SDMA descriptor 直接使用 `aclrtHostGetDevicePointer` 返回的 mapped host pointer 作为 source，device pointer 作为 destination。

| case | 源 buffer | 目标 buffer | 输出口径 | 说明 |
| --- | --- | --- | --- | --- |
| `all_host_to_all_device_ffts_direct_h2d` | 每张卡各自一块 `aclrtMallocHost` host buffer，并注册 mapped | device buffer | 聚合一行 | 8 进程、8 host buffer、8 device 的 direct H2D SDMA |
| `one_share_host_to_all_device_ffts_direct_h2d` | 一块 POSIX shared memory host buffer，每个子进程注册 mapped | device buffer | 聚合一行 | 多进程同时从同一块 shared host 做 direct H2D SDMA |
| `all_odirect_host_to_all_device_ffts_direct_h2d` | 每张卡各自一块 UCM O_DIRECT 风格 anonymous mmap host buffer，并注册 mapped + pinned | device buffer | 聚合一行 | 覆盖 local direct-IO style host buffer 的 direct H2D SDMA |

推荐命令：

```bash
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 4M -n 100 -frags 128 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 100 -frags 128 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 4M -n 100 -frags 128 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 32K -n 100 -frags 128 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_odirect_host_to_all_device_ffts_direct_h2d -s 4M -n 100 -frags 128 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_odirect_host_to_all_device_ffts_direct_h2d -s 32K -n 100 -frags 128 -i 10 -d 8
```

GLM5.1 的 shared-host CE/FFTS 对比命令：

```bash
./build/module/copy/copy \
  -t one_share_host_to_all_device_ce_multi_stream \
  --io-mode glm5.1 -f 3 -n 1024 -S 16 \
  --submit-mode round-robin -i 10 -d 16

COPY_FFTS_VALIDATE=1 FFTS_MAX_READY_LANES=3 \
./build/module/copy/copy \
  -t one_share_host_to_all_device_ffts_direct_h2d \
  --io-mode glm5.1 -f 3 -n 1024 -S 16 \
  --submit-mode round-robin -i 10 -d 16
```

这两个命令每张卡每次迭代都传输 `1024 * 176K`；聚合输出的 `Size(KB)` 为 176，`Count` 为 `1024 * 16`。

## 单源多卡冲突排查顺序

建议用下面顺序排查，每条命令单独起一个新进程跑，避免前一个 case 的 runtime 状态影响后一个 case：

```bash
./build/module/copy/copy -t one_host_to_all_device_ce -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t one_host_to_all_device_ce_batch -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t one_share_host_to_all_device_ce_per_device -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t one_huge_shm_to_all_device_ce_per_device -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t all_host_to_all_device_ce -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t one_host_to_all_device_ce_multi_stream -s 1M -n 64 -i 100 -d 8
```

读结果时先分清输出口径：`one_host_to_all_device_ce` 是顺序逐卡基线；`one_host_to_all_device_ce_batch` 是同进程聚合；`*_per_device` 是 fork 并发但每卡一行；`all_host_to_all_device_*` 是每张卡独立源 buffer。
