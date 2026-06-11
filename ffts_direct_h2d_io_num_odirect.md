# FFTS Direct H2D io_num and O_DIRECT notes

## Current direct H2D implementation

`ffts-direct-h2d` is the direct FFTS SDMA host-to-device path in the Ascend copy benchmark. It does not use a CE staging buffer. The source address written into the FFTS SDMA descriptor is the device-visible mapped host pointer returned by `aclrtHostGetDevicePointer`, and the destination address is the device buffer pointer.

The current cases are:

| Case | Source buffer | Destination buffer | Meaning |
| --- | --- | --- | --- |
| `all_host_to_all_device_ffts_direct_h2d` | each child process allocates its own `aclrtMallocHost` buffer, registers it as mapped host memory | one device buffer per device | all host to all device |
| `one_share_host_to_all_device_ffts_direct_h2d` | parent creates one POSIX shared memory region, each child maps and registers it | one device buffer per device | one shared host to all device |
| `all_odirect_host_to_all_device_ffts_direct_h2d` | each child process allocates one UCM O_DIRECT style anonymous mmap buffer, then registers it as mapped and pinned host memory | one device buffer per device | local direct-IO style host buffer to all device |

Validation is off by default. Set `COPY_FFTS_VALIDATE=1` to initialize a deterministic host pattern, copy through FFTS SDMA, read back the device buffer, and compare data.

## `-n` and `--frags`

The benchmark keeps the old global `-n` option, but direct H2D now has two modes:

| Mode | Command shape | Meaning |
| --- | --- | --- |
| Compatibility mode | no `--frags` or `-f` | `-n` is the total fragment count, and direct H2D submits one FFTS task containing all fragments. This matches the old behavior. |
| Split-task mode | use `--frags <count>` or `-f <count>` | `-n` is the IO/task count, and `--frags` is the number of fragments inside each IO/task. The case allocates `-n * --frags` fragments and submits `-n` FFTS tasks per iteration. |

Examples:

```bash
# Old behavior: 1000 fragments are merged into one FFTS task.
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 -i 10 -d 8

# New behavior: 1000 IO/tasks, one fragment in each task.
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 --frags 1 -i 10 -d 8

# New behavior: 1000 IO/tasks, four fragments in each task, 4000 fragments per device.
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 --frags 4 -i 10 -d 8
```

For the default benchmark matrix, use lanes 8:

```bash
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_odirect_host_to_all_device_ffts_direct_h2d -s 4M -n 1000 --frags 1 -i 10 -d 8

FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_odirect_host_to_all_device_ffts_direct_h2d -s 32K -n 1000 --frags 1 -i 10 -d 8
```

## Submit flow

During `Prepare`, the direct H2D copy instance builds one copy spec per fragment:

```text
mapped host source pointer -> device destination pointer -> fragment size
```

In compatibility mode, all copy specs are placed into one task group. In split-task mode, the copy specs are partitioned into groups of `--frags`. Each group calls the FFTS dispatcher separately, so `-n 1000 --frags 1` means 1000 `rtFftsPlusTaskLaunchWithFlag` submissions per measured iteration per device.

The result `Count` is still based on the total fragment count, not only the task count. In split-task mode:

```text
Count per device = -n * --frags
Aggregated Count = -n * --frags * device_count
```

## O_DIRECT host buffer shape in UCM

The UCM local CacheStore buffer has two host allocation paths:

| Condition | Allocation path | Shape |
| --- | --- | --- |
| `io_direct=false` | `MakeHostBuffer` | `aclrtMallocHost` |
| `io_direct=true` and shared buffer disabled | `MakeHostBuffer4DirectIo` | anonymous private `mmap`, first trying HugeTLB or gigantic HugeTLB, then falling back to transparent hugepage advice, followed by `mlock` and `aclrtHostRegisterV2(MAPPED | PINNED)` |

The new dev-sandbox case `all_odirect_host_to_all_device_ffts_direct_h2d` mirrors the second shape. It allocates anonymous mmap host memory, tries HugeTLB/gigantic HugeTLB first, falls back to THP advice, then registers the memory with `ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED` and uses `aclrtHostGetDevicePointer`.

## Shared memory plus O_DIRECT

When shared memory is enabled, UCM does not switch the shared transfer buffer to the local `MakeHostBuffer4DirectIo` allocation path. The shared buffer remains POSIX shared memory:

```text
shm_open/ftruncate -> mmap(MAP_SHARED) -> page-aligned data area -> aclrtHostRegisterV2(MAPPED | PINNED)
```

For PCStore, the `ioDirect` flag is kept in the reader and affects file IO by opening files with `O_DIRECT`. The shared host buffer itself is still POSIX shared memory registered as mapped and pinned host memory. Therefore the existing `one_share_host_to_all_device_ffts_direct_h2d` case is the dev-sandbox equivalent for shared memory plus O_DIRECT.

## Runtime FFTS launch usage

The current dispatcher includes either `runtime/rt_ffts_plus.h` or `rt_external_ffts.h`, then builds FFTS Plus task descriptors:

| Field | Current value or meaning |
| --- | --- |
| `rtFftsPlusSqe_t::fftsType` | `RT_FFTS_PLUS_TYPE` |
| `rtFftsPlusSqe_t::totalContextNum` | number of SDMA contexts in the current task |
| `rtFftsPlusSqe_t::readyContextNum` | number of contexts that can run immediately; controlled by `FFTS_MAX_READY_LANES`, default 8 |
| `rtFftsPlusSqe_t::preloadContextNum` | min of ready count and 128 |
| `rtFftsPlusSqe_t::timeout` | 0 |
| `rtFftsPlusSqe_t::subType` | `0x5A`, used here as a communication task |
| `rtFftsPlusTaskInfo_t::descBuf` | host address of the context descriptor array |
| `rtFftsPlusTaskInfo_t::descBufLen` | byte length of the descriptor array |
| `rtFftsPlusTaskInfo_t::descAddrType` | `RT_FFTS_PLUS_CTX_DESC_ADDR_TYPE_HOST` |
| `rtFftsPlusTaskInfo_t::argsHandleInfoNum` | 0 |
| `rtFftsPlusTaskInfo_t::argsHandleInfoPtr` | null |

Each descriptor is an `rtFftsPlusSdmaCtx_t` viewed through the common 128-byte context type. The current context fields set `contextType = RT_CTX_TYPE_SDMA`, fill source and destination address high/low words, and fill the data length fields.

In this checkout, the runtime headers are not present, so the full upstream enum list for every supported FFTS task/context type cannot be confirmed locally. The code that is visible in dev-sandbox and UCM only uses `RT_FFTS_PLUS_TYPE` plus `RT_CTX_TYPE_SDMA`. Future extensions should add new dispatchers around the runtime header's other `RT_CTX_TYPE_*` context layouts, update `subType` if the runtime requires another task class, and keep the current SDMA path unchanged.

## Modified behavior summary

- Added `--frags` and `-f` to the copy CLI.
- Kept direct H2D default behavior compatible by using one FFTS task when `--frags` is omitted.
- Split direct H2D into multiple FFTS task launches when `--frags` is specified.
- Kept validation disabled by default and retained `COPY_FFTS_VALIDATE=1`.
- Added `all_odirect_host_to_all_device_ffts_direct_h2d` for UCM local O_DIRECT style host memory.
- Documented that shared memory plus O_DIRECT still maps to POSIX shared memory registered as mapped and pinned host memory.

## Test plan

Local static checks:

```bash
git diff --check
```

Build check when CMake is available:

```bash
cmake -B build
cmake --build build -j
```

Runtime smoke tests on an Ascend host:

```bash
# Default validation off and compatibility mode.
FFTS_MAX_READY_LANES=8 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 8 -i 1 -d 1

# Manual validation on.
FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 8 -i 1 -d 1

# Split-task mode: 8 tasks, one fragment each.
FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t all_host_to_all_device_ffts_direct_h2d -s 32K -n 8 --frags 1 -i 1 -d 1

# O_DIRECT style local host buffer.
FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t all_odirect_host_to_all_device_ffts_direct_h2d -s 32K -n 8 --frags 1 -i 1 -d 1

# Shared memory shape used by shared memory plus O_DIRECT.
FFTS_MAX_READY_LANES=8 COPY_FFTS_VALIDATE=1 \
./build/module/copy/copy -t one_share_host_to_all_device_ffts_direct_h2d -s 32K -n 8 --frags 1 -i 1 -d 1
```
