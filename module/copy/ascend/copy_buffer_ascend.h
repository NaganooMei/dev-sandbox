/**
 * MIT License
 *
 * Copyright (c) 2026 Mag1c.H
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#ifndef COPY_BUFFER_ASCEND_H
#define COPY_BUFFER_ASCEND_H

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "copy_buffer.h"
#include "error_handle_ascend.h"
#include "host_register_ascend.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#ifndef MFD_HUGETLB
#define MFD_HUGETLB 0x0004U
#endif

#ifndef MFD_HUGE_SHIFT
#define MFD_HUGE_SHIFT 26
#endif

#ifndef MFD_HUGE_2MB
#define MFD_HUGE_2MB (21U << MFD_HUGE_SHIFT)
#endif

#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

inline size_t CheckedTotalBytes(size_t size, size_t number)
{
    ASSERT(number == 0 || size <= std::numeric_limits<size_t>::max() / number);
    const auto total = size * number;
    ASSERT(total > 0);
    return total;
}

inline size_t RoundUpToHugePageSize(size_t bytes)
{
    constexpr size_t kHugePageSize = 2ull * 1024ull * 1024ull;
    const auto remainder = bytes % kHugePageSize;
    if (remainder == 0) { return bytes; }
    const auto padding = kHugePageSize - remainder;
    ASSERT(bytes <= std::numeric_limits<size_t>::max() - padding);
    return bytes + padding;
}

inline size_t RoundUpToAlignment(size_t bytes, size_t alignment)
{
    ASSERT(alignment > 0);
    const auto remainder = bytes % alignment;
    if (remainder == 0) { return bytes; }
    const auto padding = alignment - remainder;
    ASSERT(bytes <= std::numeric_limits<size_t>::max() - padding);
    return bytes + padding;
}

inline bool IsPageAligned(const void* ptr)
{
    constexpr size_t kPageSize = 4096;
    return reinterpret_cast<std::uintptr_t>(ptr) % kPageSize == 0;
}

inline bool CopyAscendBufferLogEnabled()
{
    static const bool enabled = []() {
        const auto* value = std::getenv("COPY_ASCEND_BUFFER_LOG");
        return value != nullptr && std::strcmp(value, "0") != 0 &&
               std::strcmp(value, "false") != 0 && std::strcmp(value, "FALSE") != 0;
    }();
    return enabled;
}

inline long long CopyAscendElapsedUs(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

inline unsigned long long CopyAscendPtrValue(const void* ptr)
{
    return static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(ptr));
}

inline bool ParseSmapsRange(const std::string& line, std::uintptr_t& start, std::uintptr_t& end)
{
    const auto dash = line.find('-');
    if (dash == std::string::npos) { return false; }
    char* parseEnd = nullptr;
    start = static_cast<std::uintptr_t>(std::strtoull(line.c_str(), &parseEnd, 16));
    if (parseEnd == nullptr || *parseEnd != '-') { return false; }
    end = static_cast<std::uintptr_t>(std::strtoull(parseEnd + 1, &parseEnd, 16));
    return parseEnd != nullptr && end > start;
}

inline bool ShouldPrintSmapsField(const std::string& line)
{
    return line.rfind("Size:", 0) == 0 || line.rfind("KernelPageSize:", 0) == 0 ||
           line.rfind("MMUPageSize:", 0) == 0 || line.rfind("Rss:", 0) == 0 ||
           line.rfind("AnonHugePages:", 0) == 0 || line.rfind("ShmemPmdMapped:", 0) == 0 ||
           line.rfind("FilePmdMapped:", 0) == 0 || line.rfind("Locked:", 0) == 0 ||
           line.rfind("THPeligible:", 0) == 0 || line.rfind("VmFlags:", 0) == 0;
}

inline void LogSmapsForAddress(const char* label, const void* ptr)
{
    if (!CopyAscendBufferLogEnabled() || ptr == nullptr || ptr == MAP_FAILED) { return; }

    std::ifstream smaps{"/proc/self/smaps"};
    if (!smaps.is_open()) {
        std::fprintf(stderr, "[copy-buffer] %s smaps=open-failed errno=%d(%s)\n", label, errno,
                     std::strerror(errno));
        return;
    }

    const auto target = reinterpret_cast<std::uintptr_t>(ptr);
    std::string line;
    bool inTarget = false;
    while (std::getline(smaps, line)) {
        std::uintptr_t start = 0;
        std::uintptr_t end = 0;
        if (ParseSmapsRange(line, start, end)) {
            if (inTarget) { break; }
            if (start <= target && target < end) {
                inTarget = true;
                std::fprintf(stderr, "[copy-buffer] %s smaps=%s\n", label, line.c_str());
            }
            continue;
        }
        if (inTarget && ShouldPrintSmapsField(line)) {
            std::fprintf(stderr, "[copy-buffer] %s %s\n", label, line.c_str());
        }
    }
    if (!inTarget) {
        std::fprintf(stderr, "[copy-buffer] %s smaps=not-found ptr=0x%llx\n", label,
                     CopyAscendPtrValue(ptr));
    }
}

inline void* MmapODirectHugeTlb(size_t& bytes, bool useGiganticPages)
{
    constexpr size_t kHugePageSize = 2ull * 1024ull * 1024ull;
    constexpr size_t kGiganticPageSize = 1ull * 1024ull * 1024ull * 1024ull;
    constexpr int kHugePageFlag = 21 << MAP_HUGE_SHIFT;
    constexpr int kGiganticPageFlag = 30 << MAP_HUGE_SHIFT;

    const auto pageSize = useGiganticPages ? kGiganticPageSize : kHugePageSize;
    const auto alignedBytes = RoundUpToAlignment(bytes, pageSize);
    const auto pageFlag = useGiganticPages ? kGiganticPageFlag : kHugePageFlag;
    constexpr auto prot = PROT_READ | PROT_WRITE;
    const auto flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | pageFlag;
    const auto originalBytes = bytes;
    errno = 0;
    const auto start = std::chrono::steady_clock::now();
    auto* ptr = mmap(nullptr, alignedBytes, prot, flags, -1, 0);
    const auto mmapErrno = errno;
    if (ptr != MAP_FAILED) { bytes = alignedBytes; }
    if (CopyAscendBufferLogEnabled()) {
        std::fprintf(stderr,
                     "[copy-buffer] odirect-hugetlb page=%zu requested=%zu aligned=%zu ptr=0x%llx "
                     "errno=%d(%s) cost_us=%lld\n",
                     pageSize, originalBytes, alignedBytes, CopyAscendPtrValue(ptr), mmapErrno,
                     std::strerror(mmapErrno), CopyAscendElapsedUs(start));
    }
    return ptr;
}

inline void* MmapODirectWithTransparentHugePage(size_t& bytes)
{
    constexpr size_t kHugePageSize = 2ull * 1024ull * 1024ull;
    const auto alignedBytes = RoundUpToAlignment(bytes, kHugePageSize);
    constexpr auto prot = PROT_READ | PROT_WRITE;
    constexpr auto flags = MAP_PRIVATE | MAP_ANONYMOUS;
    const auto originalBytes = bytes;
    errno = 0;
    const auto start = std::chrono::steady_clock::now();
    auto* ptr = mmap(nullptr, alignedBytes, prot, flags, -1, 0);
    const auto mmapErrno = errno;
    if (ptr != MAP_FAILED) {
        errno = 0;
        const auto madviseStart = std::chrono::steady_clock::now();
        const auto madviseStatus = madvise(ptr, alignedBytes, MADV_HUGEPAGE);
        const auto madviseErrno = errno;
        if (CopyAscendBufferLogEnabled()) {
            std::fprintf(stderr,
                         "[copy-buffer] odirect-thp madvise status=%d errno=%d(%s) cost_us=%lld\n",
                         madviseStatus, madviseErrno, std::strerror(madviseErrno),
                         CopyAscendElapsedUs(madviseStart));
        }
        bytes = alignedBytes;
    }
    if (CopyAscendBufferLogEnabled()) {
        std::fprintf(stderr,
                     "[copy-buffer] odirect-thp mmap requested=%zu aligned=%zu flags=private|anon "
                     "ptr=0x%llx errno=%d(%s) cost_us=%lld\n",
                     originalBytes, alignedBytes, CopyAscendPtrValue(ptr), mmapErrno,
                     std::strerror(mmapErrno), CopyAscendElapsedUs(start));
    }
    return ptr;
}

inline void* MmapODirectHostBuffer(size_t& bytes)
{
    constexpr size_t kGiganticPageSize = 1ull * 1024ull * 1024ull * 1024ull;
    const bool useGiganticPages = bytes >= kGiganticPageSize;
    if (CopyAscendBufferLogEnabled()) {
        std::fprintf(stderr, "[copy-buffer] odirect-select requested=%zu try_gigantic=%d\n", bytes,
                     useGiganticPages ? 1 : 0);
    }
    auto* ptr = MmapODirectHugeTlb(bytes, useGiganticPages);
    if (ptr == MAP_FAILED && useGiganticPages) { ptr = MmapODirectHugeTlb(bytes, false); }
    if (ptr == MAP_FAILED) { ptr = MmapODirectWithTransparentHugePage(bytes); }
    return ptr;
}

inline int CreateHugeTlbMemfd(const std::string& name)
{
#ifdef SYS_memfd_create
    return static_cast<int>(
        syscall(SYS_memfd_create, name.c_str(), MFD_CLOEXEC | MFD_HUGETLB | MFD_HUGE_2MB));
#else
    errno = ENOSYS;
    return -1;
#endif
}

class HostCopyBuffer : public CopyBuffer {
public:
    HostCopyBuffer(size_t device, size_t size, size_t number) : CopyBuffer{device, size, number}
    {
        const auto total = size * number;
        ASCEND_ASSERT(aclrtSetDevice(device_));
        ASCEND_ASSERT(aclrtMallocHost(&addr_, total));
        std::memset(addr_, 'h', total);
    }
    ~HostCopyBuffer() override
    {
        if (addr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtFreeHost(addr_));
        }
    }
    std::string Name() const override { return "acl::host::" + std::to_string(device_); }
};

class AnonymousCopyBuffer : public CopyBuffer {
public:
    AnonymousCopyBuffer(size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        const auto total = size * number;
        ASCEND_ASSERT(aclrtSetDevice(device_));
        constexpr auto prot = PROT_READ | PROT_WRITE;
        constexpr auto flags = MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE;
        errno = 0;
        const auto mmapStart = std::chrono::steady_clock::now();
        addr_ = mmap(nullptr, total, prot, flags, -1, 0);
        const auto mmapErrno = errno;
        if (CopyAscendBufferLogEnabled()) {
            std::fprintf(stderr,
                         "[copy-buffer] anon mmap device=%zu requested=%zu flags=anon|private|"
                         "populate ptr=0x%llx errno=%d(%s) cost_us=%lld page_aligned=%d\n",
                         device_, total, CopyAscendPtrValue(addr_), mmapErrno,
                         std::strerror(mmapErrno), CopyAscendElapsedUs(mmapStart),
                         IsPageAligned(addr_) ? 1 : 0);
        }
        ASSERT(addr_ != MAP_FAILED);
        LogSmapsForAddress("anon-after-mmap", addr_);
        const auto memsetStart = std::chrono::steady_clock::now();
        std::memset(addr_, 'a', total);
        if (CopyAscendBufferLogEnabled()) {
            std::fprintf(stderr, "[copy-buffer] anon memset bytes=%zu cost_us=%lld\n", total,
                         CopyAscendElapsedUs(memsetStart));
        }
        LogSmapsForAddress("anon-after-memset", addr_);
        const auto registerStart = std::chrono::steady_clock::now();
        const auto registerStatus =
            aclrtHostRegisterV2(addr_, total, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED);
        if (CopyAscendBufferLogEnabled()) {
            std::fprintf(stderr, "[copy-buffer] anon register bytes=%zu status=%d cost_us=%lld\n",
                         total, registerStatus, CopyAscendElapsedUs(registerStart));
        }
        ASCEND_ASSERT(registerStatus);
        LogSmapsForAddress("anon-after-register", addr_);
    }
    ~AnonymousCopyBuffer() override
    {
        if (addr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtHostUnregister(addr_));
            munmap(addr_, size_ * number_);
        }
    }
    std::string Name() const override { return "acl::anon::" + std::to_string(device_); }
};

class ODirectHostCopyBuffer : public CopyBuffer {
public:
    ODirectHostCopyBuffer(size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        const auto total = CheckedTotalBytes(size, number);
        mappedBytes_ = total;
        addr_ = MmapODirectHostBuffer(mappedBytes_);
        if (CopyAscendBufferLogEnabled()) {
            std::fprintf(stderr,
                         "[copy-buffer] odirect mmap-result device=%zu total=%zu mapped=%zu ptr=0x%llx "
                         "page_aligned=%d\n",
                         device_, total, mappedBytes_, CopyAscendPtrValue(addr_),
                         IsPageAligned(addr_) ? 1 : 0);
        }
        ASSERT(addr_ != MAP_FAILED);
        ASSERT(IsPageAligned(addr_));
        LogSmapsForAddress("odirect-after-mmap", addr_);
        const auto memsetStart = std::chrono::steady_clock::now();
        std::memset(addr_, 'o', total);
        if (CopyAscendBufferLogEnabled()) {
            std::fprintf(stderr, "[copy-buffer] odirect memset bytes=%zu cost_us=%lld\n", total,
                         CopyAscendElapsedUs(memsetStart));
        }
        LogSmapsForAddress("odirect-after-memset", addr_);
        errno = 0;
        const auto mlockStart = std::chrono::steady_clock::now();
        const auto mlockStatus = mlock(addr_, mappedBytes_);
        const auto mlockErrno = errno;
        locked_ = (mlockStatus == 0);
        if (CopyAscendBufferLogEnabled()) {
            std::fprintf(stderr,
                         "[copy-buffer] odirect mlock bytes=%zu status=%d errno=%d(%s) cost_us=%lld\n",
                         mappedBytes_, mlockStatus, mlockErrno, std::strerror(mlockErrno),
                         CopyAscendElapsedUs(mlockStart));
        }
        LogSmapsForAddress("odirect-after-mlock", addr_);

        ASCEND_ASSERT(aclrtSetDevice(device_));
        const auto registerStart = std::chrono::steady_clock::now();
        const auto registerStatus =
            aclrtHostRegisterV2(addr_, mappedBytes_, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED);
        if (CopyAscendBufferLogEnabled()) {
            std::fprintf(stderr,
                         "[copy-buffer] odirect register bytes=%zu status=%d cost_us=%lld\n",
                         mappedBytes_, registerStatus, CopyAscendElapsedUs(registerStart));
        }
        ASCEND_ASSERT(registerStatus);
        registered_ = true;
        LogSmapsForAddress("odirect-after-register", addr_);
    }

    ~ODirectHostCopyBuffer() override
    {
        if (addr_ != nullptr && addr_ != MAP_FAILED) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            if (registered_) { ASCEND_ASSERT(aclrtHostUnregister(addr_)); }
            if (locked_) { munlock(addr_, mappedBytes_); }
            munmap(addr_, mappedBytes_);
            addr_ = nullptr;
        }
    }

    std::string Name() const override
    {
        return "acl::odirect_mmap::" + std::to_string(device_);
    }

private:
    size_t mappedBytes_ = 0;
    bool registered_ = false;
    bool locked_ = false;
};

class HugeSharedRegion : public CopyBuffer {
public:
    HugeSharedRegion(std::string tag, size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        const auto total = CheckedTotalBytes(size, number);
        mappedBytes_ = RoundUpToHugePageSize(total);
        const auto name = "copy_ascend_huge_" + std::to_string(getpid()) + "_" + tag + "_" +
                          std::to_string(reinterpret_cast<std::uintptr_t>(this));
        fd_ = CreateHugeTlbMemfd(name);
        ASSERT(fd_ != -1);
        ASSERT(ftruncate(fd_, mappedBytes_) == 0);
        constexpr auto prot = PROT_READ | PROT_WRITE;
        constexpr auto flags = MAP_SHARED | MAP_POPULATE;
        addr_ = mmap(nullptr, mappedBytes_, prot, flags, fd_, 0);
        ASSERT(addr_ != MAP_FAILED);
        std::memset(addr_, 'g', total);
    }

    ~HugeSharedRegion() override
    {
        if (addr_) {
            munmap(addr_, mappedBytes_);
            addr_ = nullptr;
        }
        if (fd_ != -1) {
            close(fd_);
            fd_ = -1;
        }
    }

    int Fd() const { return fd_; }
    size_t MappedBytes() const { return mappedBytes_; }
    std::string Name() const override { return "acl::huge_shm::0"; }

private:
    int fd_ = -1;
    size_t mappedBytes_ = 0;
};

class HugeSharedCopyBuffer : public CopyBuffer {
public:
    HugeSharedCopyBuffer(size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        const auto total = CheckedTotalBytes(size, number);
        mappedBytes_ = RoundUpToHugePageSize(total);
        const auto name = "copy_ascend_huge_" + std::to_string(getpid()) + "_" +
                          std::to_string(reinterpret_cast<std::uintptr_t>(this));
        fd_ = CreateHugeTlbMemfd(name);
        ownsFd_ = true;
        ASSERT(fd_ != -1);
        ASSERT(ftruncate(fd_, mappedBytes_) == 0);
        MapAndRegister();
        std::memset(addr_, 'g', total);
    }

    HugeSharedCopyBuffer(int fd, size_t mappedBytes, size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}, fd_{fd}, mappedBytes_{mappedBytes}
    {
        ASSERT(fd_ != -1);
        ASSERT(CheckedTotalBytes(size, number) <= mappedBytes_);
        MapAndRegister();
    }

    ~HugeSharedCopyBuffer() override
    {
        if (addr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtHostUnregister(addr_));
            munmap(addr_, mappedBytes_);
            addr_ = nullptr;
        }
        if (ownsFd_ && fd_ != -1) {
            close(fd_);
            fd_ = -1;
        }
    }

    void* HostAt(size_t i) const { return At(i); }

    std::string Name() const override
    {
        return "acl::huge_shm::" + std::to_string(device_);
    }

private:
    void MapAndRegister()
    {
        constexpr auto prot = PROT_READ | PROT_WRITE;
        constexpr auto flags = MAP_SHARED | MAP_POPULATE;
        addr_ = mmap(nullptr, mappedBytes_, prot, flags, fd_, 0);
        ASSERT(addr_ != MAP_FAILED);
        ASCEND_ASSERT(aclrtSetDevice(device_));
        ASCEND_ASSERT(
            aclrtHostRegisterV2(addr_, mappedBytes_, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED));
    }

    int fd_ = -1;
    size_t mappedBytes_ = 0;
    bool ownsFd_ = false;
};

class RankStripedSharedHostSet {
public:
    RankStripedSharedHostSet(std::string tag, size_t segmentCount)
    {
        ASSERT(segmentCount > 0);
        const auto prefix = "/copy_ascend_rank_striped_" + std::to_string(getpid()) + "_" +
                            std::move(tag) + "_" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(this));
        shmNames_.reserve(segmentCount);
        for (size_t segment = 0; segment < segmentCount; ++segment) {
            shmNames_.push_back(prefix + "_" + std::to_string(segment));
        }
    }

    RankStripedSharedHostSet(const RankStripedSharedHostSet&) = delete;
    RankStripedSharedHostSet& operator=(const RankStripedSharedHostSet&) = delete;

    ~RankStripedSharedHostSet()
    {
        for (const auto& shmName : shmNames_) { shm_unlink(shmName.c_str()); }
    }

    const std::vector<std::string>& ShmNames() const { return shmNames_; }
    std::string Name() const { return "acl::rank_striped_shm::all"; }

private:
    std::vector<std::string> shmNames_;
};

using RankStripedSharedHostInitializer =
    std::function<void(void* ownerBase, size_t firstBlock, size_t blockCount, size_t blockSize)>;

class RankStripedSharedHostMappings {
public:
    RankStripedSharedHostMappings(std::vector<std::string> shmNames, size_t device,
                                  size_t blockSize, size_t blockCount,
                                  const std::function<void()>& setupBarrier,
                                  bool getMappedDevicePointers,
                                  const RankStripedSharedHostInitializer& initializer = {},
                                  CopyHostRegisterMode hostRegisterMode = CopyHostRegisterMode::V2)
        : shmNames_{std::move(shmNames)},
          device_{device},
          blockSize_{blockSize},
          blockCount_{blockCount},
          getMappedDevicePointers_{getMappedDevicePointers}
    {
        ASSERT(!shmNames_.empty());
        ASSERT(device_ < shmNames_.size());
        ASSERT(blockCount_ > 0);
        ASSERT(blockCount_ % shmNames_.size() == 0);
        ASSERT(static_cast<bool>(setupBarrier));
        blocksPerSegment_ = blockCount_ / shmNames_.size();
        const auto effectiveSegmentBytes = CheckedTotalBytes(blockSize_, blocksPerSegment_);
        mappedSegmentBytes_ = RoundUpToAlignment(effectiveSegmentBytes, kPageSize);
        hostBases_.assign(shmNames_.size(), MAP_FAILED);
        mappedDeviceBases_.assign(shmNames_.size(), nullptr);
        registered_.assign(shmNames_.size(), false);

        ASCEND_ASSERT(aclrtSetDevice(device_));
        hostBases_[device_] = MapSegment(device_, true);
        std::memset(hostBases_[device_], 's', mappedSegmentBytes_);
        if (initializer) {
            initializer(hostBases_[device_], device_ * blocksPerSegment_, blocksPerSegment_,
                        blockSize_);
        }

        setupBarrier();

        for (size_t segment = 0; segment < shmNames_.size(); ++segment) {
            if (segment != device_) { hostBases_[segment] = MapSegment(segment, false); }
        }

        for (size_t segment = 0; segment < shmNames_.size(); ++segment) {
            RegisterCopyHostBuffer(
                hostBases_[segment], mappedSegmentBytes_, hostRegisterMode,
                ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED,
                getMappedDevicePointers_ ? &mappedDeviceBases_[segment] : nullptr);
            registered_[segment] = true;
            if (CopyAscendBufferLogEnabled()) {
                std::fprintf(stderr,
                             "[copy-buffer] rank-striped device=%zu segment=%zu owner=%d "
                             "blocks=%zu first_block=%zu mapped_bytes=%zu host=0x%llx "
                             "mapped=0x%llx shm=%s\n",
                             device_, segment, segment == device_ ? 1 : 0, blocksPerSegment_,
                             segment * blocksPerSegment_, mappedSegmentBytes_,
                             CopyAscendPtrValue(hostBases_[segment]),
                             CopyAscendPtrValue(mappedDeviceBases_[segment]),
                             shmNames_[segment].c_str());
            }
        }
    }

    RankStripedSharedHostMappings(const RankStripedSharedHostMappings&) = delete;
    RankStripedSharedHostMappings& operator=(const RankStripedSharedHostMappings&) = delete;

    ~RankStripedSharedHostMappings()
    {
        ASCEND_ASSERT(aclrtSetDevice(device_));
        for (size_t segment = 0; segment < shmNames_.size(); ++segment) {
            if (registered_[segment]) { ASCEND_ASSERT(aclrtHostUnregister(hostBases_[segment])); }
            if (hostBases_[segment] != MAP_FAILED) {
                munmap(hostBases_[segment], mappedSegmentBytes_);
                hostBases_[segment] = MAP_FAILED;
            }
        }
    }

    void* HostAt(size_t blockIndex) const
    {
        ASSERT(blockIndex < blockCount_);
        const auto segment = blockIndex / blocksPerSegment_;
        const auto localBlock = blockIndex % blocksPerSegment_;
        return static_cast<void*>(static_cast<char*>(hostBases_[segment]) +
                                  localBlock * blockSize_);
    }

    void* MappedDeviceAt(size_t blockIndex) const
    {
        ASSERT(getMappedDevicePointers_);
        ASSERT(blockIndex < blockCount_);
        const auto segment = blockIndex / blocksPerSegment_;
        const auto localBlock = blockIndex % blocksPerSegment_;
        return static_cast<void*>(static_cast<char*>(mappedDeviceBases_[segment]) +
                                  localBlock * blockSize_);
    }

    size_t BlocksPerSegment() const { return blocksPerSegment_; }

private:
    static constexpr size_t kPageSize = 4096;

    void* MapSegment(size_t segment, bool create) const
    {
        const auto openFlags = create ? O_CREAT | O_EXCL | O_RDWR : O_RDWR;
        const auto fd = shm_open(shmNames_[segment].c_str(), openFlags, 0600);
        ASSERT(fd != -1);
        if (create) { ASSERT(ftruncate(fd, mappedSegmentBytes_) == 0); }
        constexpr auto prot = PROT_READ | PROT_WRITE;
        constexpr auto flags = MAP_SHARED | MAP_POPULATE;
        auto* address = mmap(nullptr, mappedSegmentBytes_, prot, flags, fd, 0);
        const auto closeStatus = close(fd);
        ASSERT(closeStatus == 0);
        ASSERT(address != MAP_FAILED);
        return address;
    }

    std::vector<std::string> shmNames_;
    size_t device_ = 0;
    size_t blockSize_ = 0;
    size_t blockCount_ = 0;
    size_t blocksPerSegment_ = 0;
    size_t mappedSegmentBytes_ = 0;
    bool getMappedDevicePointers_ = false;
    std::vector<void*> hostBases_;
    std::vector<void*> mappedDeviceBases_;
    std::vector<bool> registered_;
};

class RankStripedSharedHostCopyBuffer : public CopyBuffer {
public:
    RankStripedSharedHostCopyBuffer(std::vector<std::string> shmNames, size_t device,
                                    size_t size, size_t number,
                                    const std::function<void()>& setupBarrier)
        : CopyBuffer{device, size, number},
          mappings_{std::move(shmNames), device, size, number, setupBarrier, false}
    {
        addr_ = mappings_.HostAt(0);
    }

    void* At(size_t i) const override { return mappings_.HostAt(i); }

    std::string Name() const override
    {
        return "acl::rank_striped_shm::" + std::to_string(device_);
    }

private:
    RankStripedSharedHostMappings mappings_;
};

class SharedHostRegion : public CopyBuffer {
public:
    SharedHostRegion(std::string tag, size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        shmName_ = "/copy_ascend_" + std::to_string(getpid()) + "_" + tag + "_" +
                   std::to_string(reinterpret_cast<std::uintptr_t>(this));
        const auto total = size * number;
        const auto fd = shm_open(shmName_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        ASSERT(fd != -1);
        ASSERT(ftruncate(fd, total) == 0);
        constexpr auto prot = PROT_READ | PROT_WRITE;
        constexpr auto flags = MAP_SHARED | MAP_POPULATE;
        addr_ = mmap(nullptr, total, prot, flags, fd, 0);
        const auto closeStatus = close(fd);
        ASSERT(closeStatus == 0);
        ASSERT(addr_ != MAP_FAILED);
        std::memset(addr_, 's', total);
    }

    ~SharedHostRegion() override
    {
        if (addr_) { munmap(addr_, size_ * number_); }
        if (!shmName_.empty()) { shm_unlink(shmName_.c_str()); }
    }

    const std::string& ShmName() const { return shmName_; }
    std::string Name() const override { return "acl::shm::0"; }

private:
    std::string shmName_;
};

class SharedHostCopyBuffer : public CopyBuffer {
public:
    SharedHostCopyBuffer(std::string shmName, size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}, shmName_{std::move(shmName)}
    {
        const auto total = size * number;
        const auto fd = shm_open(shmName_.c_str(), O_RDWR, 0600);
        ASSERT(fd != -1);
        constexpr auto prot = PROT_READ | PROT_WRITE;
        constexpr auto flags = MAP_SHARED | MAP_POPULATE;
        addr_ = mmap(nullptr, total, prot, flags, fd, 0);
        const auto closeStatus = close(fd);
        ASSERT(closeStatus == 0);
        ASSERT(addr_ != MAP_FAILED);
        ASCEND_ASSERT(aclrtSetDevice(device_));
        ASCEND_ASSERT(aclrtHostRegisterV2(addr_, total, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED));
    }

    ~SharedHostCopyBuffer() override
    {
        if (addr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtHostUnregister(addr_));
            munmap(addr_, size_ * number_);
        }
    }

    std::string Name() const override { return "acl::shm::0"; }

private:
    std::string shmName_;
};

class DeviceCopyBuffer : public CopyBuffer {
public:
    DeviceCopyBuffer(size_t device, size_t size, size_t number) : CopyBuffer{device, size, number}
    {
        const auto total = size * number;
        ASCEND_ASSERT(aclrtSetDevice(device_));
        ASCEND_ASSERT(aclrtMalloc(&addr_, total, ACL_MEM_MALLOC_HUGE_FIRST));
        ASCEND_ASSERT(aclrtMemset(addr_, total, 'd', total));
    }
    ~DeviceCopyBuffer() override
    {
        if (addr_) {
            ASCEND_ASSERT(aclrtSetDevice(device_));
            ASCEND_ASSERT(aclrtFree(addr_));
        }
    }
    std::string Name() const override { return "acl::device::" + std::to_string(device_); }
};

class FragmentedDeviceCopyBuffer : public CopyBuffer {
public:
    FragmentedDeviceCopyBuffer(size_t device, size_t size, size_t number)
        : CopyBuffer{device, size, number}
    {
        fragments_.resize(number_);
        ASCEND_ASSERT(aclrtSetDevice(device_));
        for (auto& fragment : fragments_) {
            ASCEND_ASSERT(aclrtMalloc(&fragment, size_, ACL_MEM_MALLOC_HUGE_FIRST));
            ASCEND_ASSERT(aclrtMemset(fragment, size_, 'd', size_));
        }
        if (!fragments_.empty()) { addr_ = fragments_[0]; }
    }

    ~FragmentedDeviceCopyBuffer() override
    {
        ASCEND_ASSERT(aclrtSetDevice(device_));
        for (auto& fragment : fragments_) {
            if (fragment != nullptr) { ASCEND_ASSERT(aclrtFree(fragment)); }
        }
    }

    void* At(size_t i) const override
    {
        ASSERT(i < fragments_.size());
        return fragments_[i];
    }

    std::string Name() const override
    {
        return "acl::device_frag::" + std::to_string(device_);
    }

private:
    std::vector<void*> fragments_;
};

#endif  // COPY_BUFFER_ASCEND_H
