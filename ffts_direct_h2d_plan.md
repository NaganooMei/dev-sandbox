# FFTS Direct H2D Case 方案

## 背景

当前 `upstream-yuanrong-pipeline` 分支已经有一组 H2D FFTS pipeline case，路径在 `module/copy/ascend/h2d_ffts_pipeline`。这组 pipeline 的含义是：先通过普通 H2D 把 host 数据搬到 device staging buffer，再用 FFTS SDMA 做 device 侧拆分。

这次要加的是另一个更直接的路径：FFTS SDMA descriptor 直接使用 mapped host pointer 作为 source，device pointer 作为 destination。这个语义和 `ascend-ffts-sdma-probe` 里的 `h2d-sdma` 一致，不再经过 CE staging。

新增能力统一命名为 `ffts-direct-h2d`。为了保持 copy 仓库现有 case key 风格，建议 CLI 里的 `-t` case 名使用下划线，输出 Method 字段使用 `ffts-direct-h2d`。

## 新增 Case

先只做两个 case：

| Case | Source | Destination | 提交方式 | 输出口径 |
| --- | --- | --- | --- | --- |
| `all_host_to_all_device_ffts_direct_h2d` | 每个子进程各自一块 `aclrtMallocHost` host buffer | 每个 device 一块 `DeviceCopyBuffer` | fork fan-out，每个子进程负责一张卡 | 聚合一行 |
| `one_share_host_to_all_device_ffts_direct_h2d` | 父进程创建一块 POSIX shared memory host buffer，所有子进程 mmap 同一块源 | 每个 device 一块 `DeviceCopyBuffer` | fork fan-out，每个子进程负责一张卡 | 聚合一行 |

说明：

- `all_host_to_all_device_ffts_direct_h2d` 对应“8 个进程、8 块 host buffer、8 张卡”。每块 host buffer 用 `aclrtMallocHost` 分配，并注册为 mapped host。
- `one_share_host_to_all_device_ffts_direct_h2d` 对应“one host to all”，这里的 one 按仓库现有共享源语义走 POSIX shared memory，不使用普通单进程 `HostCopyBuffer` 顺序拷贝。
- 暂不加 per-device 输出版本，保持这次范围只覆盖用户点名的两个聚合 case。

## Direct H2D 地址模型

FFTS descriptor 不能直接填普通 host pointer。稳定路径按 probe 结果采用：

1. host 内存必须 4K 对齐。
2. `aclrtMallocHost` 分配的 host 内存使用 `aclrtHostRegisterV2(ptr, size, ACL_HOST_REG_MAPPED)` 注册。
3. POSIX shared memory / mmap host 内存使用 `aclrtHostRegisterV2(ptr, size, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED)` 注册。
4. 调用 `aclrtHostGetDevicePointer(ptr, &mappedPtr, 0)` 获取 device-visible mapped pointer。
5. FFTS SDMA descriptor 的 source 填 mapped pointer，destination 填 device pointer。
6. 校验时仍然读回 device destination，用原始 host pattern 比较。

为了避免 size 不是页大小的边界风险，新增 direct H2D 专用 host buffer 会把注册长度 round up 到 4K。实际参与 copy 的总量仍然是 `ctx.size * ctx.num`。

## 建议代码结构

新增目录：

`module/copy/ascend/ffts_direct_h2d`

建议新增文件：

| 文件 | 职责 |
| --- | --- |
| `copy_instance_ffts_direct_h2d_ascend.h` | 实现 direct H2D FFTS copy instance，把 mapped host source 和 device destination 组装成 FFTS SDMA contexts |
| `copy_case_ffts_direct_h2d_ascend.cc` | 注册两个新 case，并复用 fork runner |
| `mapped_host_buffer_ffts_direct_h2d_ascend.h` | 提供 direct H2D 专用 mapped host buffer 和 shared mapped host buffer |

也可以复用现有 `h2d_ffts_pipeline/ffts_d2d_dispatcher_ascend.h` 作为 SDMA dispatcher，因为它本质上只是构造 FFTS SDMA descriptor，不强绑定 D2D。更干净的做法是后续把它改名或抽成公共 `ffts_sdma_dispatcher_ascend.h`，但第一版建议先少动 pipeline 代码，降低回归风险。

## Buffer 设计

### 独立 host buffer

用于 `all_host_to_all_device_ffts_direct_h2d`。

建议新增 `FftsMappedHostCopyBuffer`：

- 继承 `CopyBuffer`。
- 构造时：
  - `aclrtSetDevice(device)`。
  - `aclrtMallocHost(&addr_, roundedBytes)`。
  - 初始化 host pattern。
  - `aclrtHostRegisterV2(addr_, roundedBytes, ACL_HOST_REG_MAPPED)`。
  - `aclrtHostGetDevicePointer(addr_, &mappedAddr_, 0)`。
- 析构时：
  - `aclrtSetDevice(device)`。
  - `aclrtHostUnregister(addr_)`。
  - `aclrtFreeHost(addr_)`。
- `At(i)` 返回普通 host pointer，用于初始化和校验辅助。
- 新增 `MappedAt(i)` 返回 mapped host pointer，用于 FFTS descriptor source。

### 共享 host buffer

用于 `one_share_host_to_all_device_ffts_direct_h2d`。

建议新增两类：

- `FftsMappedSharedHostRegion`
  - 父进程创建 shm。
  - `ftruncate` 到 4K round up 后的大小。
  - `mmap(MAP_SHARED | MAP_POPULATE)`。
  - 父进程只负责填 pattern 和生命周期管理，不一定注册。
- `FftsMappedSharedHostCopyBuffer`
  - 子进程打开同一个 shm。
  - mmap 同一块区域。
  - `aclrtSetDevice(device)`。
  - `aclrtHostRegisterV2(addr_, mappedBytes, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED)`。
  - `aclrtHostGetDevicePointer(addr_, &mappedAddr_, 0)`。
  - 析构时 unregister + munmap。

这样保持和现有 `one_share_host_to_all_device_ce` 一样的共享源模型，同时满足 FFTS descriptor 需要 mapped pointer 的要求。

## CopyInstance 设计

建议新增 `FftsDirectH2DCopyInstance`。

职责：

1. `Prepare` 阶段创建每个 device 的 stream/event。
2. 从 source buffer 取 `MappedAt(i)`，从 destination buffer 取 `At(i)`。
3. 对每个 IO 创建一个 FFTS SDMA copy spec：
   - src = mapped host pointer。
   - dst = device pointer。
   - size = `ctx.size`。
4. 使用 FFTS dispatcher 按 `FFTS_MAX_READY_LANES` 建依赖链，默认 8 lanes。
5. `DoCopyOnce` 中记录 total start event，launch FFTS task，record total end event，同步 stream，统计 copy time 和 submit time。
6. `Name()` 返回 `ffts-direct-h2d`。

这里不要复用 `AscendCopyInstanceBase` 的 `CopyInternal` 做 CE，因为 direct H2D 的 submit 是 FFTS launch，不是 `aclrtMemcpyAsync`。

## Case 实现流程

### all_host_to_all_device_ffts_direct_h2d

流程：

1. `DEFINE_COPY_CASE_NO_RUNTIME`，因为走 fork，每个子进程内部创建 runtime。
2. 父进程调用 `RunForkedCopyBatch`。
3. 每个子进程：
   - 创建 `FftsMappedHostCopyBuffer{device, ctx.size, ctx.num}`。
   - 创建 `DeviceCopyBuffer{device, ctx.size, ctx.num}`。
   - 填充 source pattern。
   - reset destination。
   - `FftsDirectH2DCopyInstance{ctx.iter, false}` 执行 copy。
   - 如果 `COPY_FFTS_VALIDATE=1`，读回 destination 校验。
4. 父进程聚合 8 个子进程结果，输出一行。

### one_share_host_to_all_device_ffts_direct_h2d

流程：

1. `DEFINE_COPY_CASE_NO_RUNTIME`。
2. 父进程创建 `FftsMappedSharedHostRegion`，只创建一块 shared source。
3. 父进程填充 shared source pattern。
4. 父进程调用 `RunForkedCopyBatch`。
5. 每个子进程：
   - 用 shm name 创建 `FftsMappedSharedHostCopyBuffer`。
   - 在本 device 上 register mapped，并 get mapped pointer。
   - 创建 `DeviceCopyBuffer{device, ctx.size, ctx.num}`。
   - reset destination。
   - 执行 `FftsDirectH2DCopyInstance`。
   - 可选校验。
6. 父进程聚合结果，输出一行。

## CMake 接入

在 `module/copy/CMakeLists.txt` 的 Ascend + `HAVE_ASCEND_FFTS_RUNTIME` 分支里追加新源文件：

- `ascend/ffts_direct_h2d/copy_case_ffts_direct_h2d_ascend.cc`

include directory 追加：

- `ascend/ffts_direct_h2d`

如果复用现有 dispatcher，还需要继续包含：

- `ascend/h2d_ffts_pipeline`

链接仍然复用现有 FFTS runtime 检测和 `Ascend::Runtime` / `ASCEND_RUNTIME_LIBRARY`。

## README 更新

建议在 Ascend README 中新增一节 `H2D FFTS Direct`，和现有 `H2D FFTS Pipeline` 分开，避免用户把 direct H2D 和 staging pipeline 混淆。

推荐命令：

```bash
FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 -i 10 -d 8

FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 -i 10 -d 8

FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 -i 10 -d 8

FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 -i 10 -d 8
```

说明：

- `-s` 是单个 IO 大小。
- `-n` 是每张卡的 IO 个数。
- `-d 8` 表示 8 张卡。
- `FFTS_MAX_READY_LANES=8` 对齐当前 probe 稳定配置。

## 校验策略

默认不强制校验，避免影响性能数据。设置 `COPY_FFTS_VALIDATE=1` 时启用校验。

校验流程：

1. source host buffer 写 pattern。
2. destination device buffer reset。
3. direct FFTS H2D copy。
4. 用 `aclrtMemcpy(..., ACL_MEMCPY_DEVICE_TO_HOST)` 读回每个 destination object。
5. 与 source pattern 对比。

校验 helpers 可以复用或轻量复制现有 pipeline case 中的 pattern / validation 逻辑。

## 风险和注意点

- Direct H2D 是否可跑通依赖 runtime 是否接受 mapped host pointer 作为 FFTS SDMA source。这个已经由独立 probe 验证过，但进入 fork benchmark 后仍要在 A3 机器上做实测确认。
- `aclrtMallocHost` buffer 仍建议注册成 `ACL_HOST_REG_MAPPED` 并使用 `aclrtHostGetDevicePointer` 返回值，不直接把原始 host pointer 填进 descriptor。
- POSIX shared memory 子进程必须各自 register mapped，因为每个进程拿到的是自己的 mmap 虚拟地址。
- 注册长度建议 4K round up，避免非页大小注册带来的边界问题。
- 新 case 不应该改动现有 CE 和 FFTS pipeline case 的行为。
- 如果后面要加 per-device 输出版本，可以直接复用 `RunForkedCopyBatchPerDevice`，命名建议追加 `_per_device`。

## 推荐实施顺序

1. 新增 direct H2D mapped host buffer 和 shared mapped host buffer。
2. 新增 `FftsDirectH2DCopyInstance`，先复用现有 FFTS SDMA dispatcher。
3. 新增两个 case：all host to all device、one shared host to all device。
4. 更新 CMake。
5. 更新 README。
6. 本地做静态检查和格式检查。
7. 在 Ascend 机器上跑 32K 小 case smoke。
8. 跑 4M * 1000 和 32K * 1000 正式 case。
