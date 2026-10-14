# memfd_register

`memfd_register` isolates the Cache2 memfd data-plane setup used by UCM. It does not create
CacheBuffer metadata, slots, hash tables, or connector state.

For every simulated rank, the tool forks one process and performs the same relevant operations as
the A5 branch:

1. Create one local memfd with `MFD_ALLOW_SEALING | MFD_CLOEXEC`; explicit HugeTLB mode also adds
   `MFD_HUGETLB | MFD_HUGE_2MB`.
2. `ftruncate`, add `F_SEAL_SEAL | F_SEAL_SHRINK`, and map it with `MAP_SHARED`.
3. Bind the local segment to the rank's NUMA node and first-touch one byte per page.
4. Set the rank's NPU and register the local segment with
   `aclrtHostRegisterV2(..., ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED)`.
5. Exchange memfd descriptors with `SCM_RIGHTS`; every rank maps and registers all peer segments in
   ascending rank order.

The total requested size is divided evenly between ranks. Each per-rank segment is rounded up to
the selected backing page size.

## Build on the Ascend server

```bash
cmake -S . -B build -DASCEND_ROOT=/usr/local/Ascend/ascend-toolkit/latest
cmake --build build -j --target memfd_register
```

## Run

```bash
# Reproduce 16 ranks registering a 256 GiB cache in every device context.
./build/module/memfd_register/memfd_register \
  --ranks 16 --total-size 256G --numa-nodes 0-7

# Reproduce the exact 17,179,852,800-byte segment size from the reported UCM log.
./build/module/memfd_register/memfd_register \
  --ranks 16 --total-size 274877644800 --numa-nodes 0-7

# Compare with 8 ranks. The total registered by each rank remains 256 GiB.
./build/module/memfd_register/memfd_register \
  --ranks 8 --total-size 256G --numa-nodes 0-7

# Register one complete 256 GiB device context at a time. Earlier successful
# contexts stay registered while the next rank runs, exposing the stable global limit.
./build/module/memfd_register/memfd_register \
  --ranks 8 --total-size 256G --numa-nodes 0-7 \
  --registration-mode serial

# Repeat the serial experiment with explicit 2 MiB HugeTLB-backed memfds.
# This mode fails rather than falling back if the requested HugeTLB pool is unavailable.
./build/module/memfd_register/memfd_register \
  --ranks 8 --total-size 256G --numa-nodes 0-7 \
  --registration-mode serial --page-mode hugetlb-2m
```

`--numa-nodes` is optional. When omitted, the tool uses the intersection of online, memory-bearing,
and process-allowed NUMA nodes. Rank placement follows Cache2's fallback formula
`rank * node_count / rank_count`. `--device-offset` defaults to zero.

`--registration-mode concurrent` matches the normal Cache2 startup race. In `serial` mode all
segments are created, placed, touched, and exchanged first; rank 0 then registers all segments and
keeps them registered while rank 1 runs, followed by the remaining ranks. Registrations are released
only after the first failure or after every rank succeeds. This separates the stable global
registration capacity from transient contention.

The tool prints `first_touch` progress every 16 GiB before registration begins. With `--ranks 1`,
the complete `--total-size` is bound to one NUMA node, so a 256 GiB run can spend a long time in
page faults/reclaim or exhaust that node even when the machine has enough aggregate memory. Use
multiple ranks to reproduce Cache2's rank-sliced NUMA layout.

`--page-mode base` preserves the normal memfd behavior. `--page-mode hugetlb-2m` uses explicit
HugeTLB pages, not a best-effort `MADV_HUGEPAGE` hint. Before registration, every owner checks that
the fd reports `HUGETLBFS_MAGIC`, the filesystem block size is 2 MiB, `/proc/self/smaps` contains
the `ht` VM flag, and the complete segment is accounted in `Shared_Hugetlb + Private_Hugetlb`.
The run stops if any check fails. A successful mapping prints `hugetlb verified`.

Inspect the configured global and per-NUMA pools before running:

```bash
grep -E 'HugePages|Hugepagesize|Hugetlb' /proc/meminfo

for path in /sys/kernel/mm/hugepages/hugepages-*; do
  echo "== $path =="
  grep . "$path"/{nr_hugepages,free_hugepages,resv_hugepages,surplus_hugepages}
done

for path in /sys/devices/system/node/node*/hugepages/hugepages-*; do
  echo "== $path =="
  grep . "$path"/{nr_hugepages,free_hugepages,surplus_hugepages}
done
```

For 256 GiB backed by 2 MiB pages, the machine needs 131072 pages in total. With eight 32 GiB
rank segments placed on eight NUMA nodes, each target node needs 16384 free 2 MiB pages.

Each registration line contains the segment rank, role, bytes successfully registered so far,
return code, and elapsed time. A failure around the same `registered_bytes` for 8 and 16 ranks is
evidence for a per-device-context registration limit rather than a per-call size limit.
