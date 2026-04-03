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
#ifndef ACLBW_MEMCPY_CASE_H
#define ACLBW_MEMCPY_CASE_H

#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "memcpy_instance.h"
#include "memcpy_result.h"

class MemcpyParameterSet {
    MemcpyParameterSet() = default;
    MemcpyParameterSet(const MemcpyParameterSet&) = delete;
    MemcpyParameterSet& operator=(const MemcpyParameterSet&) = delete;
    MemcpyParameterSet(MemcpyParameterSet&&) = delete;
    MemcpyParameterSet& operator=(MemcpyParameterSet&&) = delete;

public:
    static MemcpyParameterSet& Instance()
    {
        static MemcpyParameterSet set;
        return set;
    }

    int32_t deviceNumber;
    size_t streamNumber;
    size_t bufferSize;
    size_t bufferNumber;
    size_t iterations = 1024;
    size_t warmup = 3;
};

class MemcpyCase {
    std::string key_;

public:
    MemcpyCase(std::string key) : key_(std::move(key)) {}
    virtual ~MemcpyCase() = default;
    virtual void Run() = 0;
    const std::string& Key() const noexcept { return key_; }
};

inline void FillHostBufferPattern(const MemoryBuffer& buffer)
{
    auto* bytes = static_cast<uint8_t*>(buffer.Buffer());
    const auto totalSize = buffer.Size() * buffer.Number();
    for (size_t i = 0; i < totalSize; ++i) {
        bytes[i] = static_cast<uint8_t>((i * 17U + 31U) & 0xFFU);
    }
}

inline bool ValidateHostToDeviceCopy(const MemoryBuffer& srcBuffer, const MemoryBuffer& dstBuffer,
                                     std::string& error)
{
    ACLBW_ASCEND_ASSERT(aclrtSetDevice(dstBuffer.DeviceId()));
    void* hostMirror = nullptr;
    const auto totalSize = dstBuffer.Size() * dstBuffer.Number();
    ACLBW_ASCEND_ASSERT(aclrtMallocHost(&hostMirror, totalSize));
    auto freeMirror = [&hostMirror]() {
        if (hostMirror != nullptr) {
            ACLBW_ASCEND_ASSERT(aclrtFreeHost(hostMirror));
            hostMirror = nullptr;
        }
    };

    ACLBW_ASCEND_ASSERT(
        aclrtMemcpy(hostMirror, totalSize, dstBuffer.Buffer(), totalSize, ACL_MEMCPY_DEVICE_TO_HOST));
    const auto cmp = std::memcmp(srcBuffer.Buffer(), hostMirror, totalSize);
    freeMirror();
    if (cmp != 0) {
        error = fmt::format("device payload mismatch for {} bytes", totalSize);
        return false;
    }
    return true;
}

class HostToDeviceMemcpyCase : public MemcpyCase {
public:
    HostToDeviceMemcpyCase() : MemcpyCase("host_to_device_memcpy_ce") {}
    void Run() override
    {
        auto& param = MemcpyParameterSet::Instance();
        Host2DeviceCEMemcpyInitiator initiator;
        MemcpyInstance memcpyInstance{param.iterations, param.warmup, param.streamNumber,
                                      &initiator};
        MemcpyResult result;
        for (auto deviceId = 0; deviceId < param.deviceNumber; deviceId++) {
            AscendHostMemoryBuffer srcBuffer{deviceId, param.bufferSize, param.bufferNumber};
            AscendDeviceMemoryBuffer dstBuffer{deviceId, param.bufferSize, param.bufferNumber};
            result.Record(memcpyInstance.DoMemcpy(srcBuffer, dstBuffer));
        }
        result.Show("memcpy CE CPU -> GPU(row) bandwidth");
    }
};

class AllHostToAllDeviceMemcpyCase : public MemcpyCase {
public:
    AllHostToAllDeviceMemcpyCase() : MemcpyCase("all_host_to_all_device_memcpy_ce") {}
    void Run() override
    {
        auto& param = MemcpyParameterSet::Instance();
        Host2DeviceCEMemcpyInitiator initiator;
        MemcpyInstance memcpyInstance{param.iterations, param.warmup, param.streamNumber,
                                      &initiator};
        MemcpyResult result;
        std::vector<const MemoryBuffer*> srcBuffers(param.deviceNumber);
        std::vector<const MemoryBuffer*> dstBuffers(param.deviceNumber);
        for (auto deviceId = 0; deviceId < param.deviceNumber; deviceId++) {
            srcBuffers[deviceId] =
                new AscendHostMemoryBuffer(deviceId, param.bufferSize, param.bufferNumber);
            dstBuffers[deviceId] =
                new AscendDeviceMemoryBuffer(deviceId, param.bufferSize, param.bufferNumber);
        }
        result.Record(memcpyInstance.DoMemcpy(srcBuffers, dstBuffers));
        result.Show("memcpy CE CPU(all) -> GPU(all) bandwidth");
        for (auto& buffer : srcBuffers) { delete buffer; }
        for (auto& buffer : dstBuffers) { delete buffer; }
    }
};

class MmapToDeviceMemcpyCase : public MemcpyCase {
public:
    MmapToDeviceMemcpyCase() : MemcpyCase("mmap_to_device_memcpy_ce") {}
    void Run() override
    {
        auto& param = MemcpyParameterSet::Instance();
        Host2DeviceCEMemcpyInitiator initiator;
        MemcpyInstance memcpyInstance{param.iterations, param.warmup, param.streamNumber,
                                      &initiator};
        MemcpyResult result;
        for (auto deviceId = 0; deviceId < param.deviceNumber; deviceId++) {
            MmapSharedRegisteredBuffer srcBuffer{deviceId, param.bufferSize, param.bufferNumber};
            AscendDeviceMemoryBuffer dstBuffer{deviceId, param.bufferSize, param.bufferNumber};
            result.Record(memcpyInstance.DoMemcpy(srcBuffer, dstBuffer));
        }
        result.Show("memcpy CE CPU(mmap) -> GPU(row) bandwidth");
    }
};

class MmapToAllDeviceMemcpyCase : public MemcpyCase {
public:
    MmapToAllDeviceMemcpyCase() : MemcpyCase("mmap_to_all_device_memcpy_ce") {}
    void Run() override
    {
        auto& param = MemcpyParameterSet::Instance();
        Host2DeviceCEMemcpyInitiator initiator;
        MemcpyInstance memcpyInstance{param.iterations, param.warmup, param.streamNumber,
                                      &initiator};
        MemcpyResult result;
        const char* shmName = "aclbw_shared_buffer";
        std::vector<const MemoryBuffer*> srcBuffers(param.deviceNumber);
        std::vector<const MemoryBuffer*> dstBuffers(param.deviceNumber);
        for (auto deviceId = 0; deviceId < param.deviceNumber; deviceId++) {
            srcBuffers[deviceId] = new MmapSharedRegisteredBuffer(
                shmName, deviceId, param.bufferSize, param.bufferNumber);
            dstBuffers[deviceId] =
                new AscendDeviceMemoryBuffer(deviceId, param.bufferSize, param.bufferNumber);
        }
        result.Record(memcpyInstance.DoMemcpy(srcBuffers, dstBuffers));
        result.Show("memcpy CE CPU(mmap) -> GPU(all) bandwidth");
        for (auto& buffer : srcBuffers) { delete buffer; }
        for (auto& buffer : dstBuffers) { delete buffer; }
        shm_unlink(shmName);
    }
};

class HostToDeviceHcommRoceSingleWriteCase : public MemcpyCase {
public:
    HostToDeviceHcommRoceSingleWriteCase()
        : MemcpyCase("host_to_device_hcomm_roce_single_write")
    {
    }

    void Run() override
    {
        auto& param = MemcpyParameterSet::Instance();
        if (param.deviceNumber != 1) {
            fmt::print("[aclbw][hcomm] {} unsupported: deviceNumber must be 1, got {}\n", Key(),
                       param.deviceNumber);
            return;
        }
        if (param.streamNumber != 1) {
            fmt::print("[aclbw][hcomm] {} unsupported: streamNumber must be 1, got {}\n", Key(),
                       param.streamNumber);
            return;
        }

        MemcpyResult result;
        for (auto deviceId = 0; deviceId < param.deviceNumber; deviceId++) {
            AscendHostMemoryBuffer srcBuffer{deviceId, param.bufferSize, param.bufferNumber};
            AscendDeviceMemoryBuffer dstBuffer{deviceId, param.bufferSize, param.bufferNumber};
            FillHostBufferPattern(srcBuffer);

            HcommRoceH2dSession session{
                deviceId, srcBuffer.Buffer(), dstBuffer.Buffer(),
                srcBuffer.Size() * srcBuffer.Number()};
            if (!session.IsReady()) {
                fmt::print("[aclbw][hcomm] {} unsupported: {}\n", Key(), session.ErrorMessage());
                return;
            }
            session.PrintSummary();

            Host2DeviceHcommRoceMemcpyInitiator initiator{session};
            MemcpyInstance memcpyInstance{param.iterations, param.warmup, param.streamNumber,
                                          &initiator};
            try {
                result.Record(memcpyInstance.DoMemcpy(srcBuffer, dstBuffer));
            } catch (const std::exception& ex) {
                fmt::print("[aclbw][hcomm] {} bring-up failed: {}\n", Key(), ex.what());
                return;
            }

            std::string validateError;
            if (!ValidateHostToDeviceCopy(srcBuffer, dstBuffer, validateError)) {
                fmt::print("[aclbw][hcomm] {} validation failed: {}\n", Key(), validateError);
                return;
            }
        }
        result.Show("memcpy HCOMM ROCE CPU -> GPU(row) bandwidth");
    }
};

#endif  // ACLBW_MEMCPY_CASE_H
