# PR #2 selective merge plan

## 目标

把当前 `h2d-ffts-share-experiments` 分支里已经整理好的 Ascend copy 能力，选择性合到 PR #2 的源分支 `upstream-yuanrong-pipeline` 上。

PR 地址：

```text
https://github.com/mag1c-h/dev-sandbox/pull/2
```

当前本地观察到：

- PR 源分支对应本地远端分支：`origin/upstream-yuanrong-pipeline`
- 当前实验分支：`h2d-ffts-share-experiments`
- `origin/upstream-yuanrong-pipeline` 是当前实验分支的祖先，可以做增量选择性合入。
- 不能直接 merge 整个实验分支，因为里面包含实验脚本和过程文档。

## 合入范围

只合入代码和 `readme.md`。

计划带入这些文件：

```text
module/copy/ascend/copy_buffer_ascend.h
module/copy/ascend/copy_case_ascend.cc
module/copy/ascend/copy_instance_ascend.h
module/copy/ascend/forked_copy_runner_ascend.h
module/copy/ascend/h2d_ffts_pipeline/copy_case_h2d_ffts_pipeline_ascend.cc
module/copy/copy_case.h
module/copy/copy_main.cc
module/copy/copy_result.h
readme.md
```

不要带入这些文件：

```text
docs/ascend_copy_cases.md
docs/fork_multiprocess_shared_host_copy_plan.md
docs/h2d_ffts_pipeline_share_plan.md
docs/h2d_ffts_pipeline_design.md
docs/latest_commit_fork_multiprocess_copy.md
scripts/run_h2d_ffts_pipeline_share_exp1.py
scripts/run_h2d_ffts_pipeline_share_exp3.py
scripts/run_h2d_ffts_pipeline_share_experiments.py
```

## 功能边界

合入后 PR 分支应该包含这些能力：

1. 保留 PR #2 原本的 H2D FFTS pipeline 基础能力。
2. 新增 Ascend shared host buffer：
   - parent 创建 POSIX shm。
   - child 进程 `mmap(MAP_SHARED)` 同一块 shm。
   - child 内按 device 调 `aclrtHostRegisterV2`。
3. 新增 forked multi-device runner：
   - 每张卡一个 child process。
   - child 内独立 `CopyRuntime`。
   - parent 通过 pipe 收集 `CopyResult::Result`。
   - parent 合并每轮 submit/copy costs。
4. 整理 Ascend case 语义：
   - `one_share_host_to_all_device_*`: shared shm + fork。
   - `one_host_to_all_device_*`: 普通 `HostCopyBuffer` fan-out。
   - `all_host_to_all_device_*`: 每卡独立 host，fork 并发提交。
5. `readme.md` 更新 case 表，说明 shared-host、one-host、all-host 的区别。

## 推荐操作步骤

不要在当前实验分支上直接改 PR 分支。建议新建临时工作分支：

```bash
git fetch origin
git switch -c pr2-selective-merge origin/upstream-yuanrong-pipeline
```

从当前实验分支只取白名单文件：

```bash
git checkout h2d-ffts-share-experiments -- \
  module/copy/ascend/copy_buffer_ascend.h \
  module/copy/ascend/copy_case_ascend.cc \
  module/copy/ascend/copy_instance_ascend.h \
  module/copy/ascend/forked_copy_runner_ascend.h \
  module/copy/ascend/h2d_ffts_pipeline/copy_case_h2d_ffts_pipeline_ascend.cc \
  module/copy/copy_case.h \
  module/copy/copy_main.cc \
  module/copy/copy_result.h \
  readme.md
```

确认没有误带脚本和文档：

```bash
git status --short
git diff --name-only --cached
git diff --check
```

预期只出现白名单文件。如果出现 `docs/*.md` 或 `scripts/*.py`，需要还原掉。

提交建议拆成一个 commit：

```bash
git add \
  module/copy/ascend/copy_buffer_ascend.h \
  module/copy/ascend/copy_case_ascend.cc \
  module/copy/ascend/copy_instance_ascend.h \
  module/copy/ascend/forked_copy_runner_ascend.h \
  module/copy/ascend/h2d_ffts_pipeline/copy_case_h2d_ffts_pipeline_ascend.cc \
  module/copy/copy_case.h \
  module/copy/copy_main.cc \
  module/copy/copy_result.h \
  readme.md

git commit -m "copy/ascend: organize shared-host multi-device copy cases"
```

如果确认要直接更新 PR #2：

```bash
git push origin HEAD:upstream-yuanrong-pipeline
```

## README 调整要求

`readme.md` 需要做到两点：

1. case 表里明确 `one_share_host_to_all_device_*`、`one_host_to_all_device_*`、`all_host_to_all_device_*` 的语义。
2. 不把实验脚本作为 PR 主能力写进去。PR 分支不带脚本，所以 README 里只保留 copy case 使用示例和基础验证命令。

建议保留的示例：

```bash
COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t host_to_device_ffts_pipeline -s 32K -n 32 -i 4 -d 1

COPY_FFTS_VALIDATE=1 COPY_FFTS_PIPELINE_OBJECT_FRAGS=8 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_pipeline -s 2K -n 16 -i 4 -d 2
```

## PR 描述草稿

标题建议：

```text
copy/ascend: add FFTS pipeline and shared-host multi-device copy cases
```

正文建议：

```text
## Summary

- add Ascend H2D FFTS pipeline cases for fragmented device buffers
- add fork-based multi-device submission for shared-host and all-host Ascend copy cases
- add POSIX shared-memory host buffer support with per-child ACL host registration
- clarify Ascend copy case topology in the README:
  - one_share_host_to_all_device_* uses shared shm + fork
  - one_host_to_all_device_* uses one ordinary HostCopyBuffer fan-out
  - all_host_to_all_device_* uses per-device HostCopyBuffer + fork

## Notes

This PR intentionally does not include local experiment scripts or experiment-design documents. It only carries the benchmark implementation and README case documentation.

## Validation

- [ ] Build copy target on an Ascend/Linux environment
- [ ] Run a small FFTS validation case
- [ ] Run shared-host smoke cases
```

## 建议验证命令

在 Ascend/Linux 环境上：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target copy -j

COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t host_to_device_ffts_pipeline -s 32K -n 32 -i 4 -d 1

COPY_FFTS_VALIDATE=1 COPY_FFTS_PIPELINE_OBJECT_FRAGS=4 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_pipeline -s 2K -n 16 -i 4 -d 2

./build/module/copy/copy -t one_share_host_to_all_device_ce -s 2K -n 16 -i 4 -d 2
./build/module/copy/copy -t one_host_to_all_device_ce -s 2K -n 16 -i 4 -d 2
./build/module/copy/copy -t all_host_to_all_device_ce -s 2K -n 16 -i 4 -d 2
```

## 风险点

- 不能直接 merge `h2d-ffts-share-experiments`，否则会把实验脚本和过程文档带进 PR。
- `readme.md` 需要人工复核，避免残留实验脚本相关描述。
- fork case 依赖 Linux/POSIX API，本地 Windows/MinGW 环境无法完整验证，需要在 Ascend/Linux 环境跑构建和 smoke。
