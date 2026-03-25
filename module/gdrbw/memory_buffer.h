/**
 * MIT License
 *
 * Copyright (c) 2026 relat-ivity
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
#ifndef GDRBW_MEMORY_BUFFER_H
#define GDRBW_MEMORY_BUFFER_H

#include <cuda_runtime.h>
#include <infiniband/verbs.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "error_handle.h"
#include "rdma_channel.h"

class MemoryBuffer {
protected:
    void* buffer_;
    int32_t deviceId_;
    size_t size_;
    size_t number_;
    std::unordered_map<int32_t, ibv_mr*> memoryRegions_;

    void RegisterHostMemoryForAllChannels()
    {
        auto& manager = ChannelManager::Instance();
        GDRBW_ASSERT(manager.IsInitialized());
        for (const auto targetDeviceId : manager.DeviceIds()) {
            memoryRegions_.emplace(targetDeviceId,
                                   manager.Get(targetDeviceId).RegisterHostMemory(buffer_, TotalBytes()));
        }
    }

    void RegisterDeviceMemoryForOwnerChannel()
    {
        auto& manager = ChannelManager::Instance();
        GDRBW_ASSERT(manager.IsInitialized());
        memoryRegions_.emplace(deviceId_, manager.Get(deviceId_).RegisterGpuMemory(buffer_, TotalBytes()));
    }

    void ReleaseMemoryRegions() noexcept
    {
        for (auto& entry : memoryRegions_) {
            if (entry.second != nullptr) { ibv_dereg_mr(entry.second); }
        }
        memoryRegions_.clear();
    }

    ibv_mr* FindMemoryRegion(int32_t targetDeviceId) const
    {
        const auto it = memoryRegions_.find(targetDeviceId);
        return (it == memoryRegions_.end()) ? nullptr : it->second;
    }

public:
    MemoryBuffer(int32_t deviceId, size_t size, size_t number)
        : buffer_(nullptr), deviceId_(deviceId), size_(size), number_(number)
    {
    }

    virtual ~MemoryBuffer() = default;
    MemoryBuffer(const MemoryBuffer&) = delete;
    MemoryBuffer& operator=(const MemoryBuffer&) = delete;
    MemoryBuffer(MemoryBuffer&&) = delete;
    MemoryBuffer& operator=(MemoryBuffer&&) = delete;

    virtual std::string ReadMe() const = 0;
    void* Buffer() const { return buffer_; }
    void* operator[](size_t index) const
    {
        GDRBW_ASSERT(index < number_);
        return static_cast<char*>(buffer_) + index * size_;
    }
    int32_t DeviceId() const { return deviceId_; }
    size_t Size() const { return size_; }
    size_t Number() const { return number_; }
    size_t TotalBytes() const { return size_ * number_; }
    uint64_t Address() const { return reinterpret_cast<uint64_t>(buffer_); }
    uint64_t Address(size_t index) const { return reinterpret_cast<uint64_t>((*this)[index]); }
    bool HasMR() const { return HasMR(deviceId_); }
    bool HasMR(int32_t targetDeviceId) const { return FindMemoryRegion(targetDeviceId) != nullptr; }
    uint32_t LKey() const { return LKey(deviceId_); }
    uint32_t LKey(int32_t targetDeviceId) const
    {
        auto* memoryRegion = FindMemoryRegion(targetDeviceId);
        GDRBW_ASSERT(memoryRegion != nullptr);
        return memoryRegion->lkey;
    }
    uint32_t RKey() const { return RKey(deviceId_); }
    uint32_t RKey(int32_t targetDeviceId) const
    {
        auto* memoryRegion = FindMemoryRegion(targetDeviceId);
        GDRBW_ASSERT(memoryRegion != nullptr);
        return memoryRegion->rkey;
    }
};

class MmapSharedBuffer : public MemoryBuffer {
public:
    MmapSharedBuffer(int32_t deviceId, size_t size, size_t number)
        : MemoryBuffer(deviceId, size, number)
    {
        const auto totalSize = TotalBytes();
        const auto fd = shm_open("gdrbw_shared_buffer", O_CREAT | O_RDWR, 0666);
        GDRBW_ERRNO_ASSERT(fd != -1);
        GDRBW_ERRNO_ASSERT(ftruncate(fd, static_cast<off_t>(totalSize)) != -1);
        buffer_ = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
        GDRBW_ERRNO_ASSERT(buffer_ != MAP_FAILED);
        close(fd);
        shm_unlink("gdrbw_shared_buffer");
    }

    ~MmapSharedBuffer() override
    {
        if (buffer_ != nullptr) { munmap(buffer_, TotalBytes()); }
    }

    std::string ReadMe() const override { return "MmapSharedBuffer"; }
};

class PosixMemalignBuffer : public MemoryBuffer {
    size_t alignment_;

public:
    PosixMemalignBuffer(int32_t deviceId, size_t size, size_t number, size_t alignment)
        : MemoryBuffer(deviceId, size, number), alignment_(alignment)
    {
        GDRBW_ASSERT(posix_memalign(&buffer_, alignment_, TotalBytes()) == 0);
    }

    ~PosixMemalignBuffer() override
    {
        if (buffer_ != nullptr) { free(buffer_); }
    }

    std::string ReadMe() const override
    {
        return "PosixMemalignBuffer" + std::to_string(alignment_);
    }
};

class MmapSharedRegisteredBuffer : public MemoryBuffer {
public:
    MmapSharedRegisteredBuffer(int32_t deviceId, size_t size, size_t number)
        : MemoryBuffer(deviceId, size, number)
    {
        GDRBW_CUDA_ASSERT(cudaSetDevice(deviceId_));
        const auto totalSize = TotalBytes();
        const auto fd = shm_open("gdrbw_shared_buffer", O_CREAT | O_RDWR, 0666);
        GDRBW_ERRNO_ASSERT(fd != -1);
        GDRBW_ERRNO_ASSERT(ftruncate(fd, static_cast<off_t>(totalSize)) != -1);
        buffer_ = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
        GDRBW_ERRNO_ASSERT(buffer_ != MAP_FAILED);
        close(fd);
        shm_unlink("gdrbw_shared_buffer");
        GDRBW_CUDA_ASSERT(cudaHostRegister(buffer_, totalSize, cudaHostRegisterDefault));
    }

    MmapSharedRegisteredBuffer(const char* shmName, int32_t deviceId, size_t size, size_t number)
        : MemoryBuffer(deviceId, size, number)
    {
        GDRBW_CUDA_ASSERT(cudaSetDevice(deviceId_));
        const auto totalSize = TotalBytes();
        const auto fd = shm_open(shmName, O_CREAT | O_RDWR, 0666);
        GDRBW_ERRNO_ASSERT(fd != -1);
        GDRBW_ERRNO_ASSERT(ftruncate(fd, static_cast<off_t>(totalSize)) != -1);
        buffer_ = mmap(nullptr, totalSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
        GDRBW_ERRNO_ASSERT(buffer_ != MAP_FAILED);
        close(fd);
        GDRBW_CUDA_ASSERT(cudaHostRegister(buffer_, totalSize, cudaHostRegisterDefault));
    }

    ~MmapSharedRegisteredBuffer() override
    {
        if (buffer_ != nullptr) {
            (void)cudaSetDevice(deviceId_);
            (void)cudaHostUnregister(buffer_);
            munmap(buffer_, TotalBytes());
        }
    }

    std::string ReadMe() const override { return "MmapSharedRegisteredBuffer"; }
};

class PosixMemalignRegisteredBuffer : public MemoryBuffer {
    size_t alignment_;

public:
    PosixMemalignRegisteredBuffer(int32_t deviceId, size_t size, size_t number, size_t alignment)
        : MemoryBuffer(deviceId, size, number), alignment_(alignment)
    {
        GDRBW_CUDA_ASSERT(cudaSetDevice(deviceId_));
        GDRBW_ASSERT(posix_memalign(&buffer_, alignment_, TotalBytes()) == 0);
        GDRBW_CUDA_ASSERT(cudaHostRegister(buffer_, TotalBytes(), cudaHostRegisterDefault));
    }

    ~PosixMemalignRegisteredBuffer() override
    {
        if (buffer_ != nullptr) {
            (void)cudaSetDevice(deviceId_);
            (void)cudaHostUnregister(buffer_);
            free(buffer_);
        }
    }

    std::string ReadMe() const override
    {
        return "PosixMemalignRegisteredBuffer" + std::to_string(alignment_);
    }
};

class CudaHostMemoryBuffer : public MemoryBuffer {
public:
    CudaHostMemoryBuffer(int32_t deviceId, size_t size, size_t number)
        : MemoryBuffer(deviceId, size, number)
    {
        GDRBW_CUDA_ASSERT(cudaSetDevice(deviceId_));
        GDRBW_CUDA_ASSERT(cudaMallocHost(&buffer_, TotalBytes()));
        RegisterHostMemoryForAllChannels();
    }

    ~CudaHostMemoryBuffer() override
    {
        ReleaseMemoryRegions();
        if (buffer_ != nullptr) {
            (void)cudaSetDevice(deviceId_);
            (void)cudaFreeHost(buffer_);
        }
    }

    std::string ReadMe() const override { return "CudaHostMemoryBuffer"; }
};

class CudaDeviceMemoryBuffer : public MemoryBuffer {
public:
    CudaDeviceMemoryBuffer(int32_t deviceId, size_t size, size_t number)
        : MemoryBuffer(deviceId, size, number)
    {
        GDRBW_CUDA_ASSERT(cudaSetDevice(deviceId_));
        GDRBW_CUDA_ASSERT(cudaMalloc(&buffer_, TotalBytes()));
        RegisterDeviceMemoryForOwnerChannel();
    }

    ~CudaDeviceMemoryBuffer() override
    {
        ReleaseMemoryRegions();
        if (buffer_ != nullptr) {
            (void)cudaSetDevice(deviceId_);
            (void)cudaFree(buffer_);
        }
    }

    std::string ReadMe() const override { return "CudaDeviceMemoryBuffer"; }
};

#endif  // GDRBW_MEMORY_BUFFER_H
