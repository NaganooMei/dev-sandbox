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

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "error_handle.h"
#include "rdma_channel.h"

namespace {

struct Options {
    int32_t deviceId = 0;
    std::string nicHint = "mlx5_0";
    size_t bytes = 1024 * 1024;
    size_t warmup = 10;
    size_t iterations = 100;
};

void FillPattern(void* buffer, size_t bytes)
{
    auto* ptr = static_cast<uint8_t*>(buffer);
    for (size_t i = 0; i < bytes; ++i) {
        ptr[i] = static_cast<uint8_t>((i * 29U + 7U) & 0xFFU);
    }
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg(argv[index]);
        if (arg == "--help" || arg == "-h") {
            std::fprintf(stderr,
                         "Usage: %s [--device=N] [--nic=NAME] [--bytes=N] [--warmup=N] [--iters=N]\n",
                         argv[0]);
            std::exit(0);
        }
        if (arg.find("--device=") == 0U) {
            options.deviceId = std::stoi(arg.substr(std::strlen("--device=")));
            continue;
        }
        if (arg.find("--nic=") == 0U) {
            options.nicHint = arg.substr(std::strlen("--nic="));
            continue;
        }
        if (arg.find("--bytes=") == 0U) {
            options.bytes = static_cast<size_t>(std::stoull(arg.substr(std::strlen("--bytes="))));
            continue;
        }
        if (arg.find("--warmup=") == 0U) {
            options.warmup = static_cast<size_t>(std::stoull(arg.substr(std::strlen("--warmup="))));
            continue;
        }
        if (arg.find("--iters=") == 0U) {
            options.iterations = static_cast<size_t>(std::stoull(arg.substr(std::strlen("--iters="))));
            continue;
        }
        AscendGdrbwThrowError("unknown argument: " + arg);
    }
    ASCENDGDRBW_ASSERT(options.bytes > 0);
    ASCENDGDRBW_ASSERT(options.iterations > 0);
    return options;
}

double BytesPerSecondToGiBPerSecond(size_t bytes, double averageUsec)
{
    if (averageUsec <= 0.0) {
        return 0.0;
    }
    const double seconds = averageUsec / 1000.0 / 1000.0;
    return static_cast<double>(bytes) / seconds / 1024.0 / 1024.0 / 1024.0;
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    bool success = false;
    bool aclInitialized = false;
    bool deviceSet = false;
    void* hostBuffer = nullptr;
    void* deviceBuffer = nullptr;
    MemoryRegistration* hostRegistration = nullptr;
    MemoryRegistration* deviceRegistration = nullptr;
    std::unique_ptr<RDMAChannel> channel;

    try {
        options = ParseOptions(argc, argv);

        ASCENDGDRBW_ASCEND_ASSERT(aclInit(nullptr));
        aclInitialized = true;
        ASCENDGDRBW_ASCEND_ASSERT(aclrtSetDevice(options.deviceId));
        deviceSet = true;

        RDMAChannelConfig config;
        config.cqDepth = 1024;
        config.qpSendWr = 1024;
        config.qpRecvWr = 1;

        channel = std::make_unique<RDMAChannel>(options.deviceId, options.nicHint, config);
        std::fprintf(stderr,
                     "[device0-self-h2d] channel_summary device=%d logic=%d phy=%u requested_nic=%s resolved_ibv=%s resolved_ip=%s gid_index=%d fallback_by_name=%d\n",
                     channel->DeviceId(), channel->DeviceLogicId(), channel->DevicePhyId(),
                     channel->NicName().c_str(), channel->ResolvedIbvDeviceName().c_str(),
                     channel->ResolvedDeviceIp().c_str(), channel->GidIndex(),
                     channel->UsedNameFallback() ? 1 : 0);

        ASCENDGDRBW_ASCEND_ASSERT(aclrtMallocHost(&hostBuffer, options.bytes));
        ASCENDGDRBW_ASCEND_ASSERT(
            aclrtMalloc(&deviceBuffer, options.bytes, ACL_MEM_MALLOC_HUGE_FIRST));
        ASCENDGDRBW_ASCEND_ASSERT(aclrtMemset(deviceBuffer, options.bytes, 0, options.bytes));
        FillPattern(hostBuffer, options.bytes);

        hostRegistration = channel->RegisterHostMemory(hostBuffer, options.bytes);
        deviceRegistration = channel->RegisterDeviceMemory(deviceBuffer, options.bytes);
        ASCENDGDRBW_ASSERT(hostRegistration != nullptr);
        ASCENDGDRBW_ASSERT(deviceRegistration != nullptr);
        ASCENDGDRBW_ASSERT(hostRegistration->IsIbverbs());
        ASCENDGDRBW_ASSERT(deviceRegistration->IsRaGlobal());

        std::fprintf(stderr,
                     "[device0-self-h2d] registration_summary host_backend=%s host_lkey=%u device_backend=%s device_lkey=%u device_rkey=%u\n",
                     hostRegistration->BackendName(), hostRegistration->lkey,
                     deviceRegistration->BackendName(), deviceRegistration->lkey,
                     deviceRegistration->rkey);

        std::vector<double> samplesUsec;
        samplesUsec.reserve(options.iterations);
        for (size_t loop = 0; loop < options.warmup + options.iterations; ++loop) {
            const auto begin = std::chrono::steady_clock::now();
            const uint64_t workRequestId =
                channel->SubmitWrite(reinterpret_cast<uint64_t>(hostBuffer), hostRegistration->lkey,
                                     reinterpret_cast<uint64_t>(deviceBuffer), deviceRegistration->rkey,
                                     options.bytes);
            channel->Wait(workRequestId);
            const auto end = std::chrono::steady_clock::now();
            if (loop >= options.warmup) {
                samplesUsec.emplace_back(static_cast<double>(
                    std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()));
            }
        }

        std::vector<uint8_t> verify(options.bytes, 0);
        ASCENDGDRBW_ASCEND_ASSERT(aclrtMemcpy(verify.data(), options.bytes, deviceBuffer, options.bytes,
                                              ACL_MEMCPY_DEVICE_TO_HOST));
        ASCENDGDRBW_ASSERT(std::memcmp(verify.data(), hostBuffer, options.bytes) == 0);

        double averageUsec = 0.0;
        for (double sample : samplesUsec) {
            averageUsec += sample;
        }
        averageUsec /= static_cast<double>(samplesUsec.size());

        std::fprintf(stderr,
                     "[device0-self-h2d] validation_success bytes=%zu warmup=%zu iters=%zu avg_us=%.3f bw_gib_s=%.3f\n",
                     options.bytes, options.warmup, options.iterations, averageUsec,
                     BytesPerSecondToGiBPerSecond(options.bytes, averageUsec));
        success = true;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "device0_self_h2d_min failed: %s\n", ex.what());
    }

    if (channel != nullptr) {
        if (deviceRegistration != nullptr) {
            channel->DeregisterMemory(deviceRegistration);
            deviceRegistration = nullptr;
        }
        if (hostRegistration != nullptr) {
            channel->DeregisterMemory(hostRegistration);
            hostRegistration = nullptr;
        }
        channel.reset();
    }
    if (deviceBuffer != nullptr) {
        (void)aclrtFree(deviceBuffer);
    }
    if (hostBuffer != nullptr) {
        (void)aclrtFreeHost(hostBuffer);
    }
    if (deviceSet) {
        (void)aclrtResetDevice(options.deviceId);
    }
    if (aclInitialized) {
        (void)aclFinalize();
    }
    return success ? 0 : 1;
}
