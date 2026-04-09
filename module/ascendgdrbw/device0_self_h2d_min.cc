#include <acl/acl.h>

#include <chrono>
#include <cerrno>
#include <cstdarg>
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

constexpr size_t kNoLoop = static_cast<size_t>(-1);

struct Options {
    int32_t deviceId = 0;
    std::string nicHint = "mlx5_0";
    size_t bytes = 1024 * 1024;
    size_t warmup = 10;
    size_t iterations = 100;
};

void LogStep(const char* step, const char* state, const char* format = nullptr, ...)
{
    std::fprintf(stderr, "[device0-self-h2d] step=%s state=%s", step, state);
    if (format != nullptr) {
        std::fprintf(stderr, " ");
        va_list args;
        va_start(args, format);
        std::vfprintf(stderr, format, args);
        va_end(args);
    }
    std::fprintf(stderr, "\n");
}

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
    const char* currentStep = "startup";
    size_t currentLoop = kNoLoop;
    bool success = false;
    bool aclInitialized = false;
    bool deviceSet = false;
    void* hostBuffer = nullptr;
    void* deviceBuffer = nullptr;
    MemoryRegistration* hostRegistration = nullptr;
    MemoryRegistration* deviceRegistration = nullptr;
    std::unique_ptr<RDMAChannel> channel;

    try {
        currentStep = "parse_options";
        LogStep(currentStep, "begin", "argc=%d", argc);
        options = ParseOptions(argc, argv);
        LogStep(currentStep, "success", "device=%d nic=%s bytes=%zu warmup=%zu iters=%zu",
                options.deviceId, options.nicHint.c_str(), options.bytes, options.warmup,
                options.iterations);

        currentStep = "acl_init";
        LogStep(currentStep, "begin");
        const aclError aclInitRc = aclInit(nullptr);
        if (aclInitRc != ACL_SUCCESS) {
            LogStep(currentStep, "failed", "rc=%d msg=%s", static_cast<int>(aclInitRc),
                    aclGetRecentErrMsg());
        }
        ASCENDGDRBW_ASCEND_ASSERT(aclInitRc);
        aclInitialized = true;
        LogStep(currentStep, "success");

        currentStep = "set_device";
        LogStep(currentStep, "begin", "device=%d", options.deviceId);
        const aclError setDeviceRc = aclrtSetDevice(options.deviceId);
        if (setDeviceRc != ACL_SUCCESS) {
            LogStep(currentStep, "failed", "device=%d rc=%d msg=%s", options.deviceId,
                    static_cast<int>(setDeviceRc), aclGetRecentErrMsg());
        }
        ASCENDGDRBW_ASCEND_ASSERT(setDeviceRc);
        deviceSet = true;
        LogStep(currentStep, "success", "device=%d", options.deviceId);

        RDMAChannelConfig config;
        config.cqDepth = 1024;
        config.qpSendWr = 1024;
        config.qpRecvWr = 1;

        currentStep = "create_channel";
        LogStep(currentStep, "begin", "device=%d nic=%s", options.deviceId,
                options.nicHint.c_str());
        channel = std::make_unique<RDMAChannel>(options.deviceId, options.nicHint, config);
        LogStep(currentStep, "success", "device=%d nic=%s", channel->DeviceId(),
                channel->NicName().c_str());

        currentStep = "alloc_host_buffer";
        LogStep(currentStep, "begin", "bytes=%zu", options.bytes);
        hostBuffer = std::malloc(options.bytes);
        if (hostBuffer == nullptr) {
            LogStep(currentStep, "failed", "errno=%d(%s)", errno, std::strerror(errno));
        }
        ASCENDGDRBW_ERRNO_ASSERT(hostBuffer != nullptr);
        LogStep(currentStep, "success", "buffer=%p bytes=%zu", hostBuffer, options.bytes);

        currentStep = "alloc_device_buffer";
        LogStep(currentStep, "begin", "bytes=%zu", options.bytes);
        const aclError mallocDeviceRc =
            aclrtMalloc(&deviceBuffer, options.bytes, ACL_MEM_MALLOC_HUGE_FIRST);
        if (mallocDeviceRc != ACL_SUCCESS) {
            LogStep(currentStep, "failed", "rc=%d msg=%s", static_cast<int>(mallocDeviceRc),
                    aclGetRecentErrMsg());
        }
        ASCENDGDRBW_ASCEND_ASSERT(mallocDeviceRc);
        LogStep(currentStep, "success", "buffer=%p bytes=%zu", deviceBuffer, options.bytes);

        currentStep = "clear_device_buffer";
        LogStep(currentStep, "begin", "buffer=%p bytes=%zu", deviceBuffer, options.bytes);
        const aclError memsetRc = aclrtMemset(deviceBuffer, options.bytes, 0, options.bytes);
        if (memsetRc != ACL_SUCCESS) {
            LogStep(currentStep, "failed", "rc=%d msg=%s", static_cast<int>(memsetRc),
                    aclGetRecentErrMsg());
        }
        ASCENDGDRBW_ASCEND_ASSERT(memsetRc);
        LogStep(currentStep, "success");

        currentStep = "fill_host_pattern";
        LogStep(currentStep, "begin", "buffer=%p bytes=%zu", hostBuffer, options.bytes);
        FillPattern(hostBuffer, options.bytes);
        LogStep(currentStep, "success");

        currentStep = "register_host_memory";
        LogStep(currentStep, "begin", "buffer=%p bytes=%zu", hostBuffer, options.bytes);
        hostRegistration = channel->RegisterHostMemory(hostBuffer, options.bytes);
        LogStep(currentStep, "success", "mr=%p lkey=%u rkey=%u",
                static_cast<void*>(hostRegistration), hostRegistration->lkey,
                hostRegistration->rkey);

        currentStep = "register_device_memory";
        LogStep(currentStep, "begin", "buffer=%p bytes=%zu", deviceBuffer, options.bytes);
        deviceRegistration = channel->RegisterDeviceMemory(deviceBuffer, options.bytes);
        LogStep(currentStep, "success", "mr=%p lkey=%u rkey=%u",
                static_cast<void*>(deviceRegistration), deviceRegistration->lkey,
                deviceRegistration->rkey);
        ASCENDGDRBW_ASSERT(hostRegistration != nullptr);
        ASCENDGDRBW_ASSERT(deviceRegistration != nullptr);

        LogStep("registration_summary", "success", "host_lkey=%u device_lkey=%u device_rkey=%u",
                hostRegistration->lkey, deviceRegistration->lkey, deviceRegistration->rkey);

        std::vector<double> samplesUsec;
        samplesUsec.reserve(options.iterations);
        currentStep = "run_transfers";
        LogStep(currentStep, "begin", "warmup=%zu iters=%zu bytes=%zu", options.warmup,
                options.iterations, options.bytes);
        for (size_t loop = 0; loop < options.warmup + options.iterations; ++loop) {
            currentLoop = loop;
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
        currentLoop = kNoLoop;
        LogStep(currentStep, "success");

        std::vector<uint8_t> verify(options.bytes, 0);
        currentStep = "copy_back_verify_buffer";
        LogStep(currentStep, "begin", "bytes=%zu", options.bytes);
        const aclError memcpyRc = aclrtMemcpy(verify.data(), options.bytes, deviceBuffer, options.bytes,
                                              ACL_MEMCPY_DEVICE_TO_HOST);
        if (memcpyRc != ACL_SUCCESS) {
            LogStep(currentStep, "failed", "rc=%d msg=%s", static_cast<int>(memcpyRc),
                    aclGetRecentErrMsg());
        }
        ASCENDGDRBW_ASCEND_ASSERT(memcpyRc);
        LogStep(currentStep, "success");

        currentStep = "compare_verify_buffer";
        LogStep(currentStep, "begin", "bytes=%zu", options.bytes);
        ASCENDGDRBW_ASSERT(std::memcmp(verify.data(), hostBuffer, options.bytes) == 0);
        LogStep(currentStep, "success");

        currentStep = "compute_statistics";
        double averageUsec = 0.0;
        for (double sample : samplesUsec) {
            averageUsec += sample;
        }
        averageUsec /= static_cast<double>(samplesUsec.size());
        LogStep(currentStep, "success", "sample_count=%zu avg_us=%.3f", samplesUsec.size(),
                averageUsec);

        LogStep("validation_summary", "success",
                "bytes=%zu warmup=%zu iters=%zu avg_us=%.3f bw_gib_s=%.3f", options.bytes,
                options.warmup, options.iterations, averageUsec,
                BytesPerSecondToGiBPerSecond(options.bytes, averageUsec));
        success = true;
    } catch (const std::exception& ex) {
        if (currentLoop != kNoLoop) {
            LogStep(currentStep, "failed", "loop=%zu reason=%s", currentLoop, ex.what());
        } else {
            LogStep(currentStep, "failed", "reason=%s", ex.what());
        }
        std::fprintf(stderr, "device0_self_h2d_min failed: %s\n", ex.what());
    }

    if (channel != nullptr) {
        if (deviceRegistration != nullptr) {
            LogStep("cleanup_deregister_device", "begin", "mr=%p", static_cast<void*>(deviceRegistration));
            channel->DeregisterMemory(deviceRegistration);
            deviceRegistration = nullptr;
            LogStep("cleanup_deregister_device", "success");
        }
        if (hostRegistration != nullptr) {
            LogStep("cleanup_deregister_host", "begin", "mr=%p", static_cast<void*>(hostRegistration));
            channel->DeregisterMemory(hostRegistration);
            hostRegistration = nullptr;
            LogStep("cleanup_deregister_host", "success");
        }
        LogStep("cleanup_channel", "begin");
        channel.reset();
        LogStep("cleanup_channel", "success");
    }
    if (deviceBuffer != nullptr) {
        LogStep("cleanup_device_buffer", "begin", "buffer=%p", deviceBuffer);
        (void)aclrtFree(deviceBuffer);
        LogStep("cleanup_device_buffer", "success");
    }
    if (hostBuffer != nullptr) {
        LogStep("cleanup_host_buffer", "begin", "buffer=%p", hostBuffer);
        std::free(hostBuffer);
        LogStep("cleanup_host_buffer", "success");
    }
    if (deviceSet) {
        LogStep("cleanup_reset_device", "begin", "device=%d", options.deviceId);
        (void)aclrtResetDevice(options.deviceId);
        LogStep("cleanup_reset_device", "success", "device=%d", options.deviceId);
    }
    if (aclInitialized) {
        LogStep("cleanup_acl_finalize", "begin");
        (void)aclFinalize();
        LogStep("cleanup_acl_finalize", "success");
    }
    return success ? 0 : 1;
}
