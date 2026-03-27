#include <acl/acl.h>
#include <fmt/format.h>
#include <hccl/hccl.h>
#include <hccl/hccl_res.h>
#include <hccl/hccl_types.h>
#include <hccl_rank_graph.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex gLogMutex;

[[noreturn]] void ThrowAcl(const char* expr, aclError ret, const char* file, int line)
{
    throw std::runtime_error(
        fmt::format("ACL call failed: {} ret={} at {}:{}", expr, static_cast<int>(ret), file, line));
}

[[noreturn]] void ThrowHccl(const char* expr, HcclResult ret, const char* file, int line)
{
    throw std::runtime_error(
        fmt::format("HCCL call failed: {} ret={} at {}:{}", expr, static_cast<int>(ret), file, line));
}

#define ACL_CHECK(expr)                                 \
    do {                                                \
        const aclError ret = (expr);                    \
        if (ret != ACL_SUCCESS) {                       \
            ThrowAcl(#expr, ret, __FILE__, __LINE__);   \
        }                                               \
    } while (0)

#define HCCL_CHECK(expr)                                \
    do {                                                \
        const HcclResult ret = (expr);                  \
        if (ret != HCCL_SUCCESS) {                      \
            ThrowHccl(#expr, ret, __FILE__, __LINE__);  \
        }                                               \
    } while (0)

constexpr uint32_t kRankCount = 2;
constexpr uint64_t kElementCount = 1024;
constexpr uint64_t kBytes = kElementCount * sizeof(float);
constexpr char kMemTag[] = "hccl_mem_reg_demo_buffer";

struct ThreadContext {
    HcclRootInfo* rootInfo = nullptr;
    uint32_t rank = 0;
    uint32_t device = 0;
    std::string error;
};

void Log(const std::string& message)
{
    std::lock_guard<std::mutex> lock(gLogMutex);
    std::fprintf(stderr, "%s\n", message.c_str());
}

void FillHostBuffer(std::vector<float>& hostBuffer, uint32_t rank)
{
    for (size_t index = 0; index < hostBuffer.size(); ++index) {
        hostBuffer[index] = static_cast<float>(rank * 1000 + index);
    }
}

void DumpRemoteMems(HcclComm comm, uint32_t rank, uint32_t remoteRank, HcclMemHandle memHandle)
{
    uint32_t netLayer = 0;
    uint32_t linkCount = 0;
    CommLink* links = nullptr;
    HCCL_CHECK(HcclRankGraphGetLinks(comm, netLayer, rank, remoteRank, &links, &linkCount));

    Log(fmt::format(
        "[hccl_mem_reg_demo] rank={} remoteRank={} linkCount={}", rank, remoteRank, linkCount));

    for (uint32_t linkIndex = 0; linkIndex < linkCount; ++linkIndex) {
        HcclChannelDesc desc;
        HCCL_CHECK(HcclChannelDescInit(&desc, 1));
        desc.memHandles = &memHandle;
        desc.memHandleNum = 1;
        desc.remoteRank = remoteRank;
        desc.localEndpoint.protocol = links[linkIndex].srcEndpointDesc.protocol;
        desc.localEndpoint.commAddr = links[linkIndex].srcEndpointDesc.commAddr;
        desc.localEndpoint.loc = links[linkIndex].srcEndpointDesc.loc;
        desc.remoteEndpoint.protocol = links[linkIndex].dstEndpointDesc.protocol;
        desc.remoteEndpoint.commAddr = links[linkIndex].dstEndpointDesc.commAddr;
        desc.remoteEndpoint.loc = links[linkIndex].dstEndpointDesc.loc;
        desc.channelProtocol = links[linkIndex].linkAttr.linkProtocol;
        desc.notifyNum = 3;

        ChannelHandle channel = nullptr;
        HCCL_CHECK(HcclChannelAcquire(comm, CommEngine::COMM_ENGINE_CPU, &desc, 1, &channel));

        uint32_t memNum = 0;
        CommMem* remoteMems = nullptr;
        char** memTags = nullptr;
        HCCL_CHECK(HcclChannelGetRemoteMems(comm, channel, &memNum, &remoteMems, &memTags));

        Log(fmt::format(
            "[hccl_mem_reg_demo] rank={} remoteRank={} linkIndex={} protocol={} remoteMemNum={}",
            rank, remoteRank, linkIndex, static_cast<int>(desc.channelProtocol), memNum));

        for (uint32_t memIndex = 0; memIndex < memNum; ++memIndex) {
            const char* tag = memTags[memIndex] == nullptr ? "<null>" : memTags[memIndex];
            Log(fmt::format(
                "[hccl_mem_reg_demo] rank={} remoteRank={} memIndex={} tag={} addr={} size={}",
                rank,
                remoteRank,
                memIndex,
                tag,
                fmt::ptr(remoteMems[memIndex].addr),
                remoteMems[memIndex].size));
        }
    }
}

void RunRank(ThreadContext* ctx)
{
    ACL_CHECK(aclrtSetDevice(static_cast<int32_t>(ctx->device)));

    HcclComm comm;
    HCCL_CHECK(HcclCommInitRootInfo(kRankCount, ctx->rootInfo, ctx->rank, &comm));

    aclrtStream stream = nullptr;
    ACL_CHECK(aclrtCreateStream(&stream));

    void* deviceBuffer = nullptr;
    ACL_CHECK(aclrtMalloc(&deviceBuffer, kBytes, ACL_MEM_MALLOC_HUGE_FIRST));

    auto cleanup = [&]() {
        if (deviceBuffer != nullptr) {
            aclrtFree(deviceBuffer);
            deviceBuffer = nullptr;
        }
        if (stream != nullptr) {
            aclrtDestroyStream(stream);
            stream = nullptr;
        }
        HcclCommDestroy(comm);
        aclrtResetDevice(ctx->device);
    };

    try {
        std::vector<float> hostBuffer(kElementCount, 0.0f);
        if (ctx->rank == 0) {
            FillHostBuffer(hostBuffer, ctx->rank);
            ACL_CHECK(aclrtMemcpy(deviceBuffer,
                                  kBytes,
                                  hostBuffer.data(),
                                  kBytes,
                                  ACL_MEMCPY_HOST_TO_DEVICE));
        } else {
            ACL_CHECK(aclrtMemset(deviceBuffer, kBytes, 0, kBytes));
        }

        CommMem regMem{COMM_MEM_TYPE_DEVICE, deviceBuffer, kBytes};
        HcclMemHandle memHandle = nullptr;
        HCCL_CHECK(HcclCommMemReg(comm, kMemTag, &regMem, &memHandle));

        Log(fmt::format(
            "[hccl_mem_reg_demo] rank={} localDevice={} registered addr={} bytes={} memHandle={}",
            ctx->rank,
            ctx->device,
            fmt::ptr(deviceBuffer),
            kBytes,
            fmt::ptr(memHandle)));

        const uint32_t remoteRank = ctx->rank == 0 ? 1U : 0U;
        DumpRemoteMems(comm, ctx->rank, remoteRank, memHandle);

        if (ctx->rank == 0) {
            HCCL_CHECK(HcclSend(deviceBuffer, kElementCount, HCCL_DATA_TYPE_FP32, remoteRank, comm, stream));
        } else {
            HCCL_CHECK(HcclRecv(deviceBuffer, kElementCount, HCCL_DATA_TYPE_FP32, remoteRank, comm, stream));
        }

        ACL_CHECK(aclrtSynchronizeStream(stream));

        if (ctx->rank == 1) {
            ACL_CHECK(aclrtMemcpy(hostBuffer.data(),
                                  kBytes,
                                  deviceBuffer,
                                  kBytes,
                                  ACL_MEMCPY_DEVICE_TO_HOST));
            Log(fmt::format(
                "[hccl_mem_reg_demo] rank={} recv sample: [{}, {}, {}, {}]",
                ctx->rank,
                hostBuffer[0],
                hostBuffer[1],
                hostBuffer[2],
                hostBuffer[3]));
        }
    } catch (...) {
        cleanup();
        throw;
    }

    cleanup();
}

}  // namespace

int main()
{
    try {
        ACL_CHECK(aclInit(nullptr));

        uint32_t deviceCount = 0;
        ACL_CHECK(aclrtGetDeviceCount(&deviceCount));
        if (deviceCount < kRankCount) {
            throw std::runtime_error(
                fmt::format("hccl_mem_reg_demo requires at least {} devices, found {}",
                            kRankCount,
                            deviceCount));
        }

        ACL_CHECK(aclrtSetDevice(0));
        void* rootInfoStorage = nullptr;
        ACL_CHECK(aclrtMallocHost(&rootInfoStorage, sizeof(HcclRootInfo)));
        auto* rootInfo = static_cast<HcclRootInfo*>(rootInfoStorage);
        HCCL_CHECK(HcclGetRootInfo(rootInfo));

        std::array<ThreadContext, kRankCount> contexts{};
        std::array<std::thread, kRankCount> threads;
        for (uint32_t rank = 0; rank < kRankCount; ++rank) {
            contexts[rank].rootInfo = rootInfo;
            contexts[rank].rank = rank;
            contexts[rank].device = rank;
            threads[rank] = std::thread([&ctx = contexts[rank]]() {
                try {
                    RunRank(&ctx);
                } catch (const std::exception& ex) {
                    ctx.error = ex.what();
                }
            });
        }

        for (auto& thread : threads) {
            thread.join();
        }

        ACL_CHECK(aclrtFreeHost(rootInfoStorage));
        ACL_CHECK(aclFinalize());

        for (const auto& ctx : contexts) {
            if (!ctx.error.empty()) {
                throw std::runtime_error(
                    fmt::format("rank {} failed: {}", ctx.rank, ctx.error));
            }
        }

        std::fprintf(stderr, "hccl_mem_reg_demo finished successfully\n");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "hccl_mem_reg_demo failed: %s\n", ex.what());
        return 1;
    }
}
