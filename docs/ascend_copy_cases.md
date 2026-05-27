# Ascend copy case 能力整理

本文整理当前 Ascend copy benchmark 暴露的 case 和对应能力，重点说明 H2D CE、CE multi-stream、H2D FFTS pipeline 的多卡拓扑语义。

核心实现入口：

`@module/copy/ascend/copy_case_ascend.cc`

`@module/copy/ascend/h2d_ffts_pipeline/copy_case_h2d_ffts_pipeline_ascend.cc`

`@module/copy/ascend/copy_buffer_ascend.h`

`@module/copy/ascend/forked_copy_runner_ascend.h`

## 1. 拓扑命名约定

| 拓扑名字 | 含义 | 是否多进程 |
| --- | --- | --- |
| `one_share_host_to_all_device_*` | 父进程创建一块 POSIX shared memory host buffer，所有 child 进程映射同一块 shm 后分别拷到各自 device | 是，每卡一个 child |
| `one_host_to_all_device_*` | 主进程创建一块普通 `HostCopyBuffer`，同一份 host buffer fan-out 到多张卡 | 否 |
| `all_host_to_all_device_*` | 每张卡使用自己的 `HostCopyBuffer`，各自拷到对应 device | 是，每卡一个 child |

这里的 `HostCopyBuffer` 是 `aclrtMallocHost` host buffer。

`SharedHostRegion` / `SharedHostCopyBuffer` 是 POSIX shm + `mmap(MAP_SHARED)` + child 内 `aclrtHostRegisterV2`。

`@module/copy/ascend/copy_buffer_ascend.h:38`

`@module/copy/ascend/copy_buffer_ascend.h:82`

`@module/copy/ascend/copy_buffer_ascend.h:115`

## 2. H2D CE copy

| Case | 能力 | Buffer 拓扑 | 执行方式 |
| --- | --- | --- | --- |
| `host_to_device_ce` | 普通 H2D CE baseline | 每张卡自己的 host/device buffer | 主进程逐 device 跑 |
| `host_to_device_batch_ce` | ACL batch CE baseline | 每张卡自己的 host/device buffer | 主进程逐 device 跑 |
| `one_share_host_to_all_device_ce` | 一份 shared host 数据 fan-out 到多卡 | POSIX shm shared host -> device[i] | fork，每卡一个 child |
| `one_host_to_all_device_ce` | 一份普通 host 数据 fan-out 到多卡 | `HostCopyBuffer(0)` -> device[i] | 主进程逐 device 跑 |
| `all_host_to_all_device_ce` | 多份 host 数据同时到多卡 | child 内 `HostCopyBuffer(device)` -> device | fork，每卡一个 child |
| `device_to_device_ce` | 单卡 D2D CE baseline | device -> same device | 主进程逐 device 跑 |
| `one_device_to_all_device_ce` | 一张卡上的 device buffer fan-out 到多卡 | device0 -> device[i] | 主进程逐 device 跑 |
| `anonymous_to_device_ce` | anonymous mmap registered host baseline | anonymous mmap host -> device | 主进程逐 device 跑 |

对应实现：

`@module/copy/ascend/copy_case_ascend.cc:30`

`@module/copy/ascend/copy_case_ascend.cc:43`

`@module/copy/ascend/copy_case_ascend.cc:56`

`@module/copy/ascend/copy_case_ascend.cc:72`

`@module/copy/ascend/copy_case_ascend.cc:85`

`@module/copy/ascend/copy_case_ascend.cc:99`

`@module/copy/ascend/copy_case_ascend.cc:112`

`@module/copy/ascend/copy_case_ascend.cc:125`

## 3. H2D CE multi-stream copy

multi-stream CE 当前固定使用 `48` 个 stream。单卡内会把 fragments 分摊到多个 stream；多卡 fork case 则是每个 child 内部各自跑 48 stream。

| Case | 能力 | Buffer 拓扑 | 执行方式 |
| --- | --- | --- | --- |
| `host_to_device_ce_multi_stream` | 单卡/逐卡 multi-stream H2D baseline | 每张卡自己的 host/device buffer | 主进程逐 device 跑 |
| `one_share_host_to_all_device_ce_multi_stream` | shared host fan-out 到多卡，单卡内 multi-stream | POSIX shm shared host -> device[i] | fork，每卡一个 child |
| `one_host_to_all_device_ce_multi_stream` | 普通 host fan-out 到多卡，单卡内 multi-stream | `HostCopyBuffer(0)` -> device[i] | 主进程内 batch 跑 |
| `one_malloc_host_to_all_device_ce_multi_stream` | 兼容 alias，语义等同普通 one-host multi-stream | `HostCopyBuffer(0)` -> device[i] | 主进程内 batch 跑 |
| `all_host_to_all_device_ce_multi_stream` | 多份 host 数据同时到多卡，单卡内 multi-stream | child 内 `HostCopyBuffer(device)` -> device | fork，每卡一个 child |

对应实现：

`@module/copy/ascend/copy_case_ascend.cc:138`

`@module/copy/ascend/copy_case_ascend.cc:152`

`@module/copy/ascend/copy_case_ascend.cc:170`

`@module/copy/ascend/copy_case_ascend.cc:187`

`@module/copy/ascend/copy_case_ascend.cc:206`

## 4. H2D FFTS pipeline

H2D FFTS pipeline 的目标是把多个 host fragments 聚合成 logical object，先 H2D 到 device staging slot，再用 FFTS split 到 fragmented device buffer。它依赖 FFTS runtime 头文件和 runtime library，缺失时不会注册这些 case。

| Case | 能力 | Buffer 拓扑 | 执行方式 |
| --- | --- | --- | --- |
| `host_to_device_ffts_pipeline` | 单卡/逐卡 H2D + FFTS split baseline | 每张卡自己的 host/fragmented device buffer | 主进程逐 device 跑 |
| `one_share_host_to_all_device_ffts_pipeline` | shared host fan-out 到多卡，每卡 H2D + FFTS split | POSIX shm shared host -> fragmented device[i] | fork，每卡一个 child |
| `one_host_to_all_device_ffts_pipeline` | 普通 host fan-out 到多卡，每卡 H2D + FFTS split | `HostCopyBuffer(0)` -> fragmented device[i] | 主进程内 batch 跑 |
| `all_host_to_all_device_ffts_pipeline` | 多份 host 数据同时到多卡，每卡 H2D + FFTS split | child 内 `HostCopyBuffer(device)` -> fragmented device | fork，每卡一个 child |

对应实现：

`@module/copy/ascend/h2d_ffts_pipeline/copy_case_h2d_ffts_pipeline_ascend.cc:133`

`@module/copy/ascend/h2d_ffts_pipeline/copy_case_h2d_ffts_pipeline_ascend.cc:155`

`@module/copy/ascend/h2d_ffts_pipeline/copy_case_h2d_ffts_pipeline_ascend.cc:184`

`@module/copy/ascend/h2d_ffts_pipeline/copy_case_h2d_ffts_pipeline_ascend.cc:212`

## 5. fork case 的共同机制

fork case 都通过 `RunForkedCopyBatch` 统一执行：

```text
parent:
  for device in [0, ctx.nDevice):
    pipe()
    fork()

child:
  CopyRuntime runtime
  construct per-device buffers
  run copy instance
  write CopyResult::Result to pipe
  _Exit(status)

parent:
  read child results
  waitpid all children
  merge timing arrays
  print one aggregated result row
```

`one_share_host_to_all_device_*` 的父进程会先创建 `SharedHostRegion`，child 再用 `SharedHostCopyBuffer` 打开同一个 shm 并注册 host memory。

`all_host_to_all_device_*` 不共享 host 源，每个 child 自己分配 `HostCopyBuffer`。

`@module/copy/ascend/forked_copy_runner_ascend.h:226`

`@module/copy/ascend/forked_copy_runner_ascend.h:237`

`@module/copy/ascend/forked_copy_runner_ascend.h:253`

`@module/copy/ascend/forked_copy_runner_ascend.h:267`

## 6. 结果统计口径

每个 child 返回每轮 `submitCosts` 和 `copyCosts`。父进程合并时，对同一轮取所有 child 的最大值：

```text
merged_submit[i] = max(child.submit[i])
merged_copy[i]   = max(child.copy[i])
```

聚合后的 `count` 是所有 child 的 `count` 之和。最终 `BW(GB/s)` 使用总数据量除以合并后的 `copy.avg`。

这个口径表示多卡并发时“最慢 child 完成本轮 copy 的设备侧窗口”，不包含 fork 创建、pipe 传输和 waitpid 等 parent wall-clock 开销。

`@module/copy/ascend/forked_copy_runner_ascend.h:190`

`@module/copy/ascend/forked_copy_runner_ascend.h:206`

`@module/copy/copy_result.h:92`
