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

## Run the 256 GiB HugeTLB experiment

The following writes change the host-wide HugeTLB pool, including when they are issued from a
privileged container with writable host sysfs. Use a dedicated idle node and make sure no other
workload owns HugeTLB pages. The experiment needs 131072 2 MiB pages in total: each of the eight
32 GiB rank segments needs 16384 pages on its target NUMA node.

First inspect the current global and per-NUMA pools:

```bash
grep -E 'HugePages|Hugepagesize|Hugetlb' /proc/meminfo

for node in {0..7}; do
  path=/sys/devices/system/node/node${node}/hugepages/hugepages-2048kB
  printf 'node%s total=' "$node"
  cat "$path/nr_hugepages"
  printf 'node%s free=' "$node"
  cat "$path/free_hugepages"
done
```

Save the current per-node targets, then reserve 16384 pages on each node. Writing
`nr_hugepages` sets the target count rather than adding to it.

```bash
snapshot=/tmp/memfd_register_hugepages.before
: > "$snapshot"
for node in {0..7}; do
  path=/sys/devices/system/node/node${node}/hugepages/hugepages-2048kB
  printf '%s %s\n' "$node" "$(cat "$path/nr_hugepages")" >> "$snapshot"
  echo 16384 > "$path/nr_hugepages"
done
```

Always verify the values actually obtained: runtime reservation may return fewer pages because of
memory pressure or fragmentation. Do not start the test unless every node reports at least 16384
total and free pages and the global total is at least 131072.

```bash
grep -E 'HugePages|Hugepagesize|Hugetlb' /proc/meminfo

required_per_node=16384
huge_ready=1
for node in {0..7}; do
  path=/sys/devices/system/node/node${node}/hugepages/hugepages-2048kB
  total=$(cat "$path/nr_hugepages")
  free=$(cat "$path/free_hugepages")
  if [ "$total" -ge "$required_per_node" ] && [ "$free" -ge "$required_per_node" ]; then
    printf 'node%s total=%s free=%s READY\n' "$node" "$total" "$free"
  else
    available=$total
    [ "$free" -lt "$available" ] && available=$free
    missing=$((required_per_node - available))
    [ "$missing" -lt 0 ] && missing=0
    printf 'node%s total=%s free=%s INSUFFICIENT missing=%s_pages=%s_MiB\n' \
      "$node" "$total" "$free" "$missing" "$((missing * 2))"
    huge_ready=0
  fi
done

global_total=$(awk '$1 == "HugePages_Total:" {print $2}' /proc/meminfo)
global_free=$(awk '$1 == "HugePages_Free:" {print $2}' /proc/meminfo)
if [ "$huge_ready" -eq 1 ] && [ "$global_total" -ge 131072 ] && [ "$global_free" -ge 131072 ]; then
  echo 'HUGETLB READY: safe to start the 256 GiB test'
else
  echo 'HUGETLB INSUFFICIENT: do not start the 256 GiB test'
fi
```

If a node is short, inspect its normal-memory pressure and high-order free blocks. On a dedicated
idle machine, a global compaction followed by a retry may recover fragmented 2 MiB blocks. Global
compaction can disturb running workloads, so do not run it on a busy service node.

```bash
grep -E 'MemTotal|MemFree|MemUsed|AnonPages|FilePages|HugePages' \
  /sys/devices/system/node/node7/meminfo
grep '^Node 7' /proc/buddyinfo

# Dedicated idle machine only.
echo 1 > /proc/sys/vm/compact_memory
echo 16384 > \
  /sys/devices/system/node/node7/hugepages/hugepages-2048kB/nr_hugepages
```

Build and run the full serial experiment:

```bash
cmake -S . -B build -DASCEND_ROOT=/usr/local/Ascend/ascend-toolkit/latest
cmake --build build -j --target memfd_register

./build/module/memfd_register/memfd_register \
  --ranks 8 \
  --total-size 256G \
  --numa-nodes 0-7 \
  --registration-mode serial \
  --page-mode hugetlb-2m \
  --timeout-seconds 1200
```

Every owner must print a line equivalent to the following before registration starts. The run fails
instead of silently falling back when the backing does not satisfy all HugeTLB checks.

```text
page_backing owner=0 mode=hugetlb-2m fs_magic=0x958458f6 fs_block_bytes=2097152 \
kernel_page_kb=2048 ... private_hugetlb_kb=33554432 ... vm_flags=... ht ...
hugetlb verified: owner=0 page_bytes=2097152 backed_bytes=34359738368
```

After the process exits, all 131072 pages should be free again. Restore the targets recorded before
the experiment only after confirming that no test process still maps them:

```bash
grep -E 'HugePages|Hugepagesize|Hugetlb' /proc/meminfo

while read -r node count; do
  path=/sys/devices/system/node/node${node}/hugepages/hugepages-2048kB
  echo "$count" > "$path/nr_hugepages"
done < /tmp/memfd_register_hugepages.before

grep -E 'HugePages|Hugepagesize|Hugetlb' /proc/meminfo
```

Each registration line contains the segment rank, role, bytes successfully registered so far,
return code, and elapsed time. A failure around the same `registered_bytes` for 8 and 16 ranks is
evidence for a per-device-context registration limit rather than a per-call size limit.
