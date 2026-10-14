# memfd_register

`memfd_register` isolates the Cache2 memfd data-plane setup used by UCM. It does not create
CacheBuffer metadata, slots, hash tables, or connector state.

For every simulated rank, the tool forks one process and performs the same relevant operations as
the A5 branch:

1. Create one local memfd with `MFD_ALLOW_SEALING | MFD_CLOEXEC`.
2. `ftruncate`, add `F_SEAL_SEAL | F_SEAL_SHRINK`, and map it with `MAP_SHARED`.
3. Bind the local segment to the rank's NUMA node and first-touch one byte per page.
4. Set the rank's NPU and register the local segment with
   `aclrtHostRegisterV2(..., ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED)`.
5. Exchange memfd descriptors with `SCM_RIGHTS`; every rank maps and registers all peer segments in
   ascending rank order.

The total requested size is divided evenly between ranks. Each per-rank segment is rounded up to
the system page size, just like Cache2's `rankStride`.

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
```

`--numa-nodes` is optional. When omitted, the tool uses the intersection of online, memory-bearing,
and process-allowed NUMA nodes. Rank placement follows Cache2's fallback formula
`rank * node_count / rank_count`. `--device-offset` defaults to zero.

Each registration line contains the segment rank, role, bytes successfully registered so far,
return code, and elapsed time. A failure around the same `registered_bytes` for 8 and 16 ranks is
evidence for a per-device-context registration limit rather than a per-call size limit.
