# Sandbox 显式分配 SHM 到 NUMA

这一版先控制物理页分布，再比较 H2D。`setDevice` 不承担 NUMA 分配职责。
不开 `--shm-numa-nodes` 时沿用原来的分配路径。

显式分配路径使用新建的 POSIX SHM：`shm_open(O_EXCL)` → `ftruncate` →
`mmap(MAP_SHARED)`（不带 `MAP_POPULATE`）→ 按区域执行单节点 `mbind(MPOL_BIND)` →
完整 `memset` → 使用 `move_pages` 查询每个物理页的位置。
绑定和查询失败，或实际节点不符，都会报错；验证在 Host Register 和拷贝计时之前完成。

- one-share：同一个 SHM 按节点数划分连续区域。32 GiB、8 节点时每区 4 GiB。
- rank-striped：segment `s` 绑定到列表中的第 `s % 节点数` 个节点。
  16 个等大 segment、总共 32 GiB 时，每段 2 GiB，每个节点两段。
- 参数是物理 NUMA ID，支持 `0-7` 或 `0,1,2,3,4,5,6,7`；区域必须按系统页大小对齐并等分，
  rank segment 数必须是节点数的倍数。不改变已有的 block 提交顺序。

## 先验证 32 GiB Host 内存

在 Linux 的 dev-sandbox 仓库根目录执行。这个程序不依赖 ACL、NPU 或 libnuma：

```bash
mkdir -p build
g++ -std=c++17 -O2 -Wall -Wextra -Werror -I module/copy \
  module/copy/numa_shm_main.cc -o build/numa_shm -lrt

numactl --hardware
grep Mems_allowed_list /proc/self/status
df -h /dev/shm

./build/numa_shm --bytes 34359738368 --nodes 0-7 --hold-seconds 120
```

首次验证也可以先用 `--bytes 8388608 --nodes 0-7 --hold-seconds 5`，每节点只分配
1 MiB，确认绑定和逐页查询通过后再运行 32 GiB。

要求所选 8 个节点都允许分配、各有足够空间容纳 4 GiB，并且 `/dev/shm` 和容器内存额度能容纳
32 GiB。程序输出每区的实际页数和 `mismatches=0`，最后输出 `READY pid=...`。
持有时间从验证完成后开始计算，正常退出或 READY 后按 Ctrl+C 会释放并删除本次 SHM。

在另一个终端替换 PID 后查看：

```bash
pid=12345  # 替换为 READY 输出的 pid
grep 'file=/dev/shm/copy_ascend_numa_probe_' /proc/$pid/numa_maps
```

`mbind` 可能把一个映射拆成多个 VMA，因此要看同一 PID、同一文件的所有行。
4 KiB 系统页下，预期每个节点累计 `1048576` 页，即 4 GiB；总共 `8388608` 页。
判断物理位置看 `N0=...` 等字段，不能用 `active=0` 判断没有物理页。
如果 `mbind` 或 `move_pages` 返回 EPERM，需检查运行环境是否允许这些系统调用。

## 接入 H2D 比较

正常构建 sandbox 后，可在以下四个 case 增加 `--shm-numa-nodes 0-7`：

- `one_share_host_to_all_device_ce_multi_stream`
- `rank_striped_host_to_all_device_ce_multi_stream`
- `one_share_host_to_all_device_ffts_direct_h2d`
- `rank_striped_host_to_all_device_ffts_direct_h2d`

例如 FFTS Direct、Register V1、32 GiB 总 Host SHM：

```bash
cmake -B build
cmake --build build -j

./build/module/copy/copy \
  -t one_share_host_to_all_device_ffts_direct_h2d \
  --io-mode uniform -s 1M -n 32768 -f 1 -S 4 -d 16 -i 20 \
  --host-register v1 --shm-numa-nodes 0-7
```

这会在每个设备上分配约 32 GiB 目标 Device Buffer；如果只验证 Host 分布，使用上面的
`numa_shm` 即可。把 case 名换为 rank-striped 就能验证每段绑定；删除 NUMA 参数可获得原分配路径的对照。
比较时保持数据量、设备数、stream 数和其他参数一致，分别记录 NUMA 分布与 GroupWall/WallBW。
CE 的 Host Register 仍沿用已有实现，`--host-register` 只作用于 FFTS Direct。

GLM5.1 模式有固定 block 大小，不能直接照搬上述 block 数来表示 32 GiB；本例用 uniform
固定总容量。等分约束作用于实际映射大小，不能等分时会报错。

## 验证边界

本地已运行节点解析、32 GiB/8 节点布局、4 KiB/64 KiB 页对齐、16 段分配以及非法参数测试：

```bash
g++ -std=c++17 -Wall -Wextra -Werror -I module/copy \
  module/copy/tests/shm_numa_test.cc -o build/shm_numa_test
./build/shm_numa_test
```

开发机没有可用 Linux/Ascend 运行环境，尚未验证 Linux 编译、32 GiB 实际落点和 H2D 性能。
运行时逐页检查证明的是初始化完成时的分布；后续注册和运行阶段仍可抓取 `numa_maps` 复核。
