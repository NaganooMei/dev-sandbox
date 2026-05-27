# 多进程下发与共享 Host 源实验方案

## 目标

这次实验要验证两个变化是否能改善 8 卡同时读场景的稳定性和吞吐:

1. 把多卡同时读的 case 从同一进程内按 device context 下发，改成 fork 出多个子进程，每个子进程只负责一张卡的提交和同步。
2. 把 `one_host_to_all_device` 拓扑里的单一 host 源改成 POSIX shared memory，让所有子进程映射同一块 host 数据，再各自向自己的 device 读。

实验先限定在 Ascend H2D copy benchmark，不扩大到 CUDA、GDR、D2D 或 simu case。核心范围是 `module/copy/ascend` 里的 CE、多 stream CE 和 H2D FFTS pipeline 同时读场景。

## 参考代码

需要对照三个现有实现:

- `module/aio/host_buffer_ascend.cc`: AIO 里 `mmap` host buffer 的注册方式，重点是 `mmap` 后通过 `aclrtHostRegisterV2` 让 Ascend runtime 识别这块 host 内存。
- `module/aio/aio_main.cc`: AIO 测试入口的参数组织方式，可参考 `--io-type mmap|alloc` 这种把 host buffer 策略显式化的风格。
- `module/copy/simu/copy_buffer_simu.h`: 已经有 POSIX `shm_open`、`ftruncate`、`mmap MAP_SHARED`、`shm_unlink` 的最小共享内存 buffer 参考。
- `module/copy/ascend/copy_buffer_ascend.h`: 当前 Ascend `HostCopyBuffer` 使用 `aclrtMallocHost`，`AnonymousCopyBuffer` 使用 anonymous `mmap` 加 `aclrtHostRegisterV2`。
- `module/copy/ascend/copy_case_ascend.cc`: CE 和 multi-stream CE 的多卡 case。
- `module/copy/ascend/h2d_ffts_pipeline/copy_case_h2d_ffts_pipeline_ascend.cc`: H2D FFTS pipeline 的多卡 case。
- `module/copy/copy_main.cc`: 当前会在进入 case 前构造 `CopyRuntime`，也就是先 `aclInit`，这是 fork 实现需要优先调整的点。

## Case 范围

第一批改造这些 H2D 多卡同时读 case:

| Case | 当前拓扑 | 目标拓扑 |
| --- | --- | --- |
| `one_host_to_all_device_ce` | 单进程，一块 `HostCopyBuffer` 顺序跑多卡 | 父进程准备一块共享内存，fork 后每个子进程映射同一块 shm，单卡提交 |
| `all_host_to_all_device_ce` | 单进程，多块 host buffer，多卡同时读，当前已做线程下发 | fork 后每个子进程有自己的 host buffer 和 device buffer，单卡提交 |
| `one_host_to_all_device_ce_multi_stream` | 单进程，一块 anonymous mmap host，multi-stream batch | 父进程共享内存，子进程单卡 multi-stream 提交 |
| `one_malloc_host_to_all_device_ce_multi_stream` | 单进程，一块 `aclrtMallocHost` host，multi-stream batch | 作为对照可以暂时保留；若最终统一 one-host 语义，则改成 shared host 源并更名或下线旧语义 |
| `all_host_to_all_device_ce_multi_stream` | 单进程，每卡一块 `HostCopyBuffer`，multi-stream batch | fork 后每个子进程单卡 multi-stream 提交 |
| `one_host_to_all_device_ffts_pipeline` | 单进程，一块 `HostCopyBuffer`，pipeline batch | 父进程共享内存，子进程单卡 H2D+FFTS pipeline |
| `all_host_to_all_device_ffts_pipeline` | 单进程，每卡一块 `HostCopyBuffer`，pipeline batch | fork 后每个子进程单卡 H2D+FFTS pipeline |

`one_malloc_host_to_all_device_ce_multi_stream` 的名字里已经承诺了 `aclrtMallocHost`，所以它有两种处理方式:

1. 保留它作为旧语义 baseline，用于比较 shared memory one-host 是否真的更好。
2. 如果要严格执行 “one host to all device 全都读共享内存”，就把它从实验三对照里移除，避免名字和实际语义冲突。

我建议先走第一种，等 shared memory 版数据稳定后再删或改名。

## 总体架构

fork 版不应该在父进程已经 `aclInit` 之后再 fork。推荐结构是:

```text
copy main
  parse args
  filter cases
  if case is fork mode:
      parent prepares non-ACL resources
      parent fork children
      child initializes ACL runtime
      child maps/registers host memory
      child allocates device buffers/streams/events
      child runs only one device
      child writes result back to parent
      parent waits and aggregates
  else:
      initialize ACL runtime
      run current in-process case
      finalize ACL runtime
```

这样能避免子进程继承父进程的 ACL runtime 状态。这个点比直接在 case 里加 `fork()` 更重要。

如果主入口改动太大，可以先做一个保守版本: 父进程用 fork+exec 拉起同一个 `copy` 可执行文件的单卡 worker 模式。这样 child 是干净进程，代价是启动成本更高，但实验可信度更好。等数据证明方向正确，再收敛成纯 fork 版。

## Shared Host Buffer 设计

新增 Ascend 侧共享 host buffer 抽象，建议拆成两个角色:

1. `SharedHostRegion`: 父进程负责创建和初始化 POSIX shm。
2. `SharedHostCopyBuffer`: 子进程负责打开、映射、注册并按 `CopyBuffer` 接口提供 host pointer。

生命周期:

```text
parent:
  shm_open(O_CREAT | O_EXCL | O_RDWR)
  ftruncate(total_size)
  mmap(MAP_SHARED)
  fill host pattern
  fork children
  wait children
  munmap
  shm_unlink

child:
  shm_open(O_RDWR)
  mmap(MAP_SHARED)
  aclrtSetDevice(device)
  aclrtHostRegisterV2(mapped_host_ptr, total_size, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED)
  run copy
  aclrtHostUnregister
  munmap
```

注意点:

- 每个子进程有自己的 virtual address，所以每个子进程都要对自己的 mapping 调一次 `aclrtHostRegisterV2`。
- CE H2D 路径仍然把 CPU host pointer 传给 `aclrtMemcpyAsync`，不要把 shared memory 误替换成 mapped device pointer。
- FFTS pipeline 里的 H2D staging 也是先走 host pointer 到 staging slot，再由 FFTS split 到 fragmented device buffers，因此 shared host 只替换 H2D 源，不改变 FFTS descriptor 地址语义。
- shared memory 名字要带 pid 和 case key，防止多次实验或并发实验冲突。
- `shm_unlink` 只由父进程做，子进程只负责 `munmap` 和 unregister。

## Fork Runner 设计

新增一个复用的 fork runner，不把 fork 逻辑散落在每个 case 里。

建议接口语义:

```text
ForkedCopyRunner
  input:
    context
    devices
    per-device callback
    optional shared host descriptor
  behavior:
    fork one child per device
    child runs callback(device)
    parent waits all children
    parent collects child result
```

每个 child 的工作边界:

```text
child(device):
  CopyRuntime runtime
  construct source buffer
  construct destination buffer
  construct copy instance
  run DoCopy or single-device DoCopyBatch
  optional validation
  write compact result to pipe
  exit
```

这样 all-host 和 one-host 的差异只在 source buffer 构造:

- one-host: child 用同一个 shm name 构造 `SharedHostCopyBuffer`。
- all-host: child 构造自己的 `HostCopyBuffer`，不共享 host 源。

## 结果汇总

当前 `CopyResult::Result` 存的是已经统计过的 min/max/avg/p50/p90，不保存原始每轮数组。fork 后如果只让 child 返回统计值，parent 很难严格合并 percentile。

建议第一版 child 返回每轮原始数组:

```text
src name
dst name
method name
size
count
submit costs per iteration
copy costs per iteration
exit status
```

parent 重新构造 `CopyResult::Result`:

- 对 one-host-to-all-device 和 all-host-to-all-device，聚合行的 `count` 仍然表示 `ctx.num * ctx.nDevice`。
- `submit` 可以统计所有 child 的 submit array，也可以单独增加 wall-clock submit 窗口。第一版保留现有统计方式，避免表格改动过大。
- `copy` 推荐两种都记录:
  - 表格继续输出现有风格的 per-child cost 聚合。
  - 日志额外打印 parent wall-clock batch time，用于观察真正的跨进程并发耗时。

如果只保留现有表格，可能会低估或高估 fork 带来的端到端收益；因为每个子进程的 event time 是单卡视角，parent wall-clock 才是 8 卡一起跑完的视角。

## 为什么 fork 可能更好

这组实验主要验证以下假设:

- 单进程内多 device context 轮流提交，可能被 host 线程、runtime lock、context 切换或同进程状态污染放大。
- 多线程下发能降低一部分 `Submit(us)`，但仍然共享同一进程 runtime 状态。
- fork 多进程后，每张卡的 ACL runtime、stream、event、host buffer 注册和提交路径在进程级隔离，能减少跨卡提交互相影响。
- one-host 场景改成 shared memory 后，所有子进程仍然读同一份物理 host 数据，更接近 “一份数据 fan-out 到多卡” 的拓扑，而不是每个进程复制一份普通 host buffer。

这不保证带宽一定提高。如果瓶颈在 PCIe/NUMA/host 内存带宽或 device CE 侧，fork 只能改善提交和 runtime 隔离，`Copy(us)` 可能变化不大。

## 风险和验证点

1. ACL fork 安全性: 不要在父进程 `aclInit` 后直接 fork。第一版必须保证 child 有自己的 ACL init/finalize。
2. shared memory 注册: `MAP_SHARED` mapping 能否稳定通过 `aclrtHostRegisterV2` 需要先用小 case 验证。
3. one-host 数据一致性: 父进程填充 pattern 后，children 只读，不要在 child 里重新 memset shared buffer。
4. 结果口径: child event time 和 parent wall-clock time 都要保留，否则多进程并发收益不好解释。
5. 输出顺序: child 不直接打印 benchmark 表格，避免多进程 stdout 交错；统一由 parent 汇总打印。
6. 清理: child 异常退出时，parent 仍要 wait 全部子进程并 unlink shm。
7. 验证开销: correctness validation 和 performance sampling 分开跑，避免 validation 影响性能数据。

## 实验步骤

### 阶段一: shared memory smoke

先只做一个最小 case，验证 Ascend child 进程能读共享 host 源:

```bash
COPY_FFTS_VALIDATE=1 ./build/module/copy/copy -t one_host_to_all_device_ffts_pipeline -s 2K -n 16 -i 4 -d 2
./build/module/copy/copy -t one_host_to_all_device_ce -s 2K -n 16 -i 4 -d 2
```

观察:

- case 能否正常退出。
- shared memory 是否被 unlink。
- validation 是否通过。
- parent 输出是否只有一张汇总表，不出现 child stdout 交错。

### 阶段二: fork submit 对比

用 8 卡固定参数对比旧路径和 fork 路径。若实现时保留 `_fork` 后缀 case，就同一 binary 对比；若直接替换原 case，就用旧 commit 的 binary 做 baseline。

```bash
./build/module/copy/copy -t one_host_to_all_device_ce -s 32K -n 1024 -i 128 -d 8
./build/module/copy/copy -t all_host_to_all_device_ce -s 32K -n 1024 -i 128 -d 8
./build/module/copy/copy -t one_host_to_all_device_ce_multi_stream -s 32K -n 1024 -i 128 -d 8
./build/module/copy/copy -t all_host_to_all_device_ce_multi_stream -s 32K -n 1024 -i 128 -d 8
./build/module/copy/copy -t one_host_to_all_device_ffts_pipeline -s 32K -n 1024 -i 128 -d 8
./build/module/copy/copy -t all_host_to_all_device_ffts_pipeline -s 32K -n 1024 -i 128 -d 8
```

每个命令单独跑一个进程，不把多个 `-t` 放在同一次命令里。case 之间至少等 `0.3s`，避免后一个 case 吃到前一个 case 的 CPU/runtime 余波。

### 阶段三: 多 size 扫描

沿用实验三的 size 维度:

```text
2K 8K 32K 64K
```

固定:

```text
io count = 1024
iterations = 128
device count = 8
```

重点看:

- `Submit(us)` 是否明显下降。
- `Copy(us)` 是否跟着下降。
- parent wall-clock batch time 是否下降。
- one-host shared memory 和 all-host 独立 buffer 的差距是否缩小或扩大。

## 推荐落地顺序

1. 加 `SharedHostRegion` 和 `SharedHostCopyBuffer`，先不改 case，只做可编译的 buffer 能力。
2. 加 fork runner 和 pipe result collector，先让 child 返回原始 timing 数组。
3. 先改 `one_host_to_all_device_ce`，验证 shared memory + fork 的最小路径。
4. 再改 `all_host_to_all_device_ce`，验证 all-host fork 但不共享源的路径。
5. 扩展到 multi-stream CE。
6. 最后扩展到 H2D FFTS pipeline，因为它还涉及 fragmented device buffer、object frags 和 validation。
7. 更新实验脚本，把 fork 版 case 和原有实验三矩阵接起来，默认 case 间 sleep `0.3s`。

## 预期结论表达

如果数据符合预期，可以这样解释:

- fork 多进程主要改善的是多卡同时提交时的进程级隔离和 runtime 状态隔离。
- one-host shared memory 保留了一份 host 数据 fan-out 到多卡的语义，比每个进程复制一份 host buffer 更贴近真实 one-host 拓扑。
- 如果 `Submit(us)` 降而 `Copy(us)` 不降，说明提交端确实被改善，但瓶颈可能在 CE、PCIe、NUMA 或 host memory bandwidth。
- 如果 parent wall-clock 下降而单卡 event time 不明显下降，说明收益来自跨卡并发组织方式，而不是单卡 copy 更快。
- 如果 shared memory 版比 `aclrtMallocHost` 版更慢，需要继续拆分是 `shm + registerV2` 的注册方式问题，还是一块物理 host 源被 8 卡同时读带来的内存带宽问题。
