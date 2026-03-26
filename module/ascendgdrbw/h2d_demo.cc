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
#include <acl/acl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>

#include <fmt/format.h>

#include "error_handle.h"
#include "rdma_channel.h"

namespace {

class AscendHostBuffer {
public:
    explicit AscendHostBuffer(int32_t deviceId, size_t bytes) : deviceId_(deviceId), bytes_(bytes)
    {
        ASCENDGDRBW_ASSERT(bytes_ > 0);
        ASCENDGDRBW_ASCEND_ASSERT(aclrtSetDevice(deviceId_));
        ASCENDGDRBW_ASCEND_ASSERT(aclrtMallocHost(&buffer_, bytes_));
        std::memset(buffer_, 0x5A, bytes_);
    }

    ~AscendHostBuffer()
    {
        if (buffer_ != nullptr) {
            (void)aclrtSetDevice(deviceId_);
            (void)aclrtFreeHost(buffer_);
        }
    }

    AscendHostBuffer(const AscendHostBuffer&) = delete;
    AscendHostBuffer& operator=(const AscendHostBuffer&) = delete;

    void* Data() const { return buffer_; }
    size_t Size() const { return bytes_; }

private:
    int32_t deviceId_ = 0;
    void* buffer_ = nullptr;
    size_t bytes_ = 0;
};

class AscendDeviceBuffer {
public:
    AscendDeviceBuffer(int32_t deviceId, size_t bytes) : deviceId_(deviceId), bytes_(bytes)
    {
        ASCENDGDRBW_ASCEND_ASSERT(aclrtSetDevice(deviceId_));
        ASCENDGDRBW_ASCEND_ASSERT(aclrtMalloc(&buffer_, bytes_, ACL_MEM_MALLOC_HUGE_FIRST));
    }

    ~AscendDeviceBuffer()
    {
        if (buffer_ != nullptr) {
            (void)aclrtSetDevice(deviceId_);
            (void)aclrtFree(buffer_);
        }
    }

    AscendDeviceBuffer(const AscendDeviceBuffer&) = delete;
    AscendDeviceBuffer& operator=(const AscendDeviceBuffer&) = delete;

    void* Data() const { return buffer_; }
    size_t Size() const { return bytes_; }
    int32_t DeviceId() const { return deviceId_; }

private:
    int32_t deviceId_ = 0;
    void* buffer_ = nullptr;
    size_t bytes_ = 0;
};

}  // namespace

int main(int argc, char const* argv[])
{
    (void)argc;
    (void)argv;

    constexpr int32_t kDeviceId = 0;
    constexpr size_t kBytes = 1 * 1024 * 1024;
    constexpr const char* kNicName = "mlx5_0";

    bool aclInitialized = false;
    auto cleanup = [&aclInitialized]() noexcept {
        ChannelManager::Instance().Shutdown();
        if (aclInitialized) {
            (void)aclFinalize();
            aclInitialized = false;
        }
    };

    try {
        ASCENDGDRBW_ASCEND_ASSERT(aclInit(nullptr));
        aclInitialized = true;

        RDMAChannelConfig config;
        config.cqDepth = 1024;
        config.qpSendWr = 1024;
        config.qpRecvWr = 1;
        ChannelManager::Instance().Initialize(1, {kNicName}, config);

        AscendHostBuffer hostBuffer{kDeviceId, kBytes};
        AscendDeviceBuffer deviceBuffer{kDeviceId, kBytes};

        auto& channel = ChannelManager::Instance().Get(kDeviceId);
        ibv_mr* hostMr = channel.RegisterHostMemory(hostBuffer.Data(), hostBuffer.Size());
        ASCENDGDRBW_ERRNO_ASSERT(hostMr != nullptr);
        ibv_mr* deviceMr = channel.RegisterDeviceMemory(deviceBuffer.Data(), deviceBuffer.Size());
        ASCENDGDRBW_ERRNO_ASSERT(deviceMr != nullptr);

        const auto wrId =
            channel.SubmitWrite(reinterpret_cast<uint64_t>(hostBuffer.Data()), hostMr->lkey,
                                reinterpret_cast<uint64_t>(deviceBuffer.Data()), deviceMr->rkey,
                                kBytes);
        channel.Wait(wrId);

        fmt::println("ascendgdrbw_h2d_demo success: device={}, nic={}, bytes={}", kDeviceId,
                     kNicName, kBytes);

        ibv_dereg_mr(hostMr);
        ibv_dereg_mr(deviceMr);
        cleanup();
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "ascendgdrbw_h2d_demo failed: %s\n", ex.what());
        cleanup();
        return 1;
    }
}
