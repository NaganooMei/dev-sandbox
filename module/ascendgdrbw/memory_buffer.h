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
#ifndef ASCENDGDRBW_MEMORY_BUFFER_H
#define ASCENDGDRBW_MEMORY_BUFFER_H

#include <infiniband/verbs.h>

#include <cstdint>
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
        ASCENDGDRBW_ASSERT(manager.IsInitialized());
        for (const auto targetDeviceId : manager.DeviceIds()) {
            memoryRegions_.emplace(targetDeviceId,
                                   manager.Get(targetDeviceId).RegisterHostMemory(buffer_, TotalBytes()));
        }
    }

    void RegisterDeviceMemoryForOwnerChannel()
    {
        auto& manager = ChannelManager::Instance();
        ASCENDGDRBW_ASSERT(manager.IsInitialized());
        memoryRegions_.emplace(deviceId_,
                               manager.Get(deviceId_).RegisterDeviceMemory(buffer_, TotalBytes()));
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
        ASCENDGDRBW_ASSERT(index < number_);
        return static_cast<char*>(buffer_) + index * size_;
    }
    int32_t DeviceId() const { return deviceId_; }
    size_t Size() const { return size_; }
    size_t Number() const { return number_; }
    size_t TotalBytes() const { return size_ * number_; }
    uint64_t Address(size_t index) const { return reinterpret_cast<uint64_t>((*this)[index]); }
    bool HasMR() const { return HasMR(deviceId_); }
    bool HasMR(int32_t targetDeviceId) const { return FindMemoryRegion(targetDeviceId) != nullptr; }
    uint32_t LKey(int32_t targetDeviceId) const
    {
        auto* registration = FindMemoryRegion(targetDeviceId);
        ASCENDGDRBW_ASSERT(registration != nullptr);
        return registration->lkey;
    }
    uint32_t RKey() const
    {
        auto* registration = FindMemoryRegion(deviceId_);
        ASCENDGDRBW_ASSERT(registration != nullptr);
        return registration->rkey;
    }
};

class AscendHostMemoryBuffer : public MemoryBuffer {
public:
    AscendHostMemoryBuffer(int32_t deviceId, size_t size, size_t number)
        : MemoryBuffer(deviceId, size, number)
    {
        ASCENDGDRBW_ASCEND_ASSERT(aclrtSetDevice(deviceId_));
        ASCENDGDRBW_ASCEND_ASSERT(aclrtMallocHost(&buffer_, TotalBytes()));
        RegisterHostMemoryForAllChannels();
    }

    ~AscendHostMemoryBuffer() override
    {
        ReleaseMemoryRegions();
        if (buffer_ != nullptr) {
            (void)aclrtSetDevice(deviceId_);
            (void)aclrtFreeHost(buffer_);
        }
    }

    std::string ReadMe() const override { return "AscendHostMemoryBuffer"; }
};

class AscendDeviceMemoryBuffer : public MemoryBuffer {
public:
    AscendDeviceMemoryBuffer(int32_t deviceId, size_t size, size_t number)
        : MemoryBuffer(deviceId, size, number)
    {
        ASCENDGDRBW_ASCEND_ASSERT(aclrtSetDevice(deviceId_));
        ASCENDGDRBW_ASCEND_ASSERT(aclrtMalloc(&buffer_, TotalBytes(), ACL_MEM_MALLOC_HUGE_FIRST));
        RegisterDeviceMemoryForOwnerChannel();
    }

    ~AscendDeviceMemoryBuffer() override
    {
        ReleaseMemoryRegions();
        if (buffer_ != nullptr) {
            (void)aclrtSetDevice(deviceId_);
            (void)aclrtFree(buffer_);
        }
    }

    std::string ReadMe() const override { return "AscendDeviceMemoryBuffer"; }
};

#endif  // ASCENDGDRBW_MEMORY_BUFFER_H
