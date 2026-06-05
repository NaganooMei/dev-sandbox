# dev-sandbox Ascend copy benchmark

这个分支的 README 只整理 Ascend copy 相关 case，用来观察 H2D、D2D、单 host 多卡读、POSIX shared memory、HugeTLB shared memory、multi-stream 和 H2D FFTS pipeline 的带宽表现。

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
-i <count>  统计迭代次数
-d <count>  设备数量
```

`Size(KB)` 是单个数据块大小，`Count` 是一次统计结果覆盖的数据块数量。聚合 case 的 `Count` 可能会等于 `-n * -d`，per-device case 则通常每张卡一行，每行 `Count = -n`。

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

运行时观察 HugeTLB 使用量：

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

multi-stream case 在单卡内使用多个 stream 提交 H2D。当前 stream 数在代码中固定为 48。

| case | 源 buffer | 提交方式 | 输出口径 | 说明 |
| --- | --- | --- | --- | --- |
| `host_to_device_ce_multi_stream` | 每张卡各自一块 host buffer | 单卡多 stream | 每张卡一行 | 单卡 multi-stream H2D 基线 |
| `one_host_to_all_device_ce_multi_stream` | 一块 `aclrtMallocHost` host0 buffer | 主进程一次 batch 覆盖所有 device，单卡内多 stream | 聚合一行 | 同一块 host0 同时发往多卡，观察 multi-stream 聚合表现 |
| `one_share_host_to_all_device_ce_multi_stream` | 一块 POSIX shared memory host buffer | fork fan-out，单卡内多 stream | 聚合一行 | shared host 多进程 fan-out + 单卡 multi-stream |
| `all_host_to_all_device_ce_multi_stream` | 每张卡各自一块 host buffer | fork fan-out，单卡内多 stream | 聚合一行 | 非共享源的多卡并发 multi-stream |

建议和普通 CE batch 对比：

```bash
./build/module/copy/copy -t one_host_to_all_device_ce_batch -s 1M -n 64 -i 100 -d 8
./build/module/copy/copy -t one_host_to_all_device_ce_multi_stream -s 1M -n 64 -i 100 -d 8
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
