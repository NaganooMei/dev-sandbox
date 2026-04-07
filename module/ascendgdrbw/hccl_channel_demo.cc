#include <acl/acl.h>
#include <hccl/hccl.h>
#include <hccl/hccl_res.h>
#include <hccl/hcomm_primitives.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

extern "C" HcclResult __attribute__((weak))
HcclChannelDestroy(HcclComm comm, const ChannelHandle *channelList, uint32_t listNum);

constexpr uint32_t kRankCount = 2;
constexpr uint32_t kReceiverRank = 0;
constexpr uint32_t kSenderRank = 1;
constexpr uint32_t kReceiverDevice = 0;
constexpr uint32_t kSenderDevice = 1;
constexpr uint32_t kNotifyIdxAck = 0;
constexpr uint32_t kNotifyIdxData = 1;
constexpr uint32_t kNotifyNum = 2;
constexpr uint32_t kWaitTimeoutMs = 1800 * 1000;

struct DemoCase {
    const char* name;
    uint64_t bytes;
};

constexpr DemoCase kCases[] = {
    {"4B", 4},
    {"64B", 64},
    {"256B", 256},
    {"4KB", 4 * 1024},
};

struct SharedState {
    std::atomic<bool> senderDone{false};
    std::atomic<bool> receiverDone{false};
};

void CheckAcl(aclError ret, const char* expr)
{
    if (ret != ACL_SUCCESS) {
        throw std::runtime_error(std::string("ACL failed: ") + expr +
                                 " ret=" + std::to_string(static_cast<int>(ret)));
    }
}

void CheckHccl(HcclResult ret, const char* expr)
{
    if (ret != HCCL_SUCCESS) {
        throw std::runtime_error(std::string("HCCL failed: ") + expr +
                                 " ret=" + std::to_string(static_cast<int>(ret)));
    }
}

void CheckHcomm(int32_t ret, const char* expr)
{
    if (ret != 0) {
        throw std::runtime_error(std::string("HCOMM failed: ") + expr +
                                 " ret=" + std::to_string(ret));
    }
}

#define ACL_CHECK(expr) CheckAcl((expr), #expr)
#define HCCL_CHECK(expr) CheckHccl((expr), #expr)
#define HCOMM_CHECK(expr) CheckHcomm((expr), #expr)

void FillHostPattern(std::vector<uint8_t>& buffer)
{
    for (size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<uint8_t>((i * 29U + 7U) & 0xFFU);
    }
}

void PrintChannelInfo(uint32_t rank, ChannelHandle channel, void* localBuffer, uint64_t localBufferSize,
                      void* remoteBuffer, uint64_t remoteBufferSize)
{
    std::fprintf(stderr,
                 "[channel-demo][rank%u] channel=%llu local_hccl_buffer=%p(%lluB) remote_hccl_buffer=%p(%lluB)\n",
                 rank, static_cast<unsigned long long>(channel), localBuffer,
                 static_cast<unsigned long long>(localBufferSize), remoteBuffer,
                 static_cast<unsigned long long>(remoteBufferSize));
}

struct ChannelResources {
    HcclComm comm = nullptr;
    ThreadHandle thread = 0;
    ChannelHandle channel = 0;
    void* localBuffer = nullptr;
    uint64_t localBufferSize = 0;
    void* remoteBuffer = nullptr;
    uint64_t remoteBufferSize = 0;
};

ChannelResources InitializeChannel(uint32_t rank, uint32_t peerRank, uint32_t deviceId, HcclRootInfo* rootInfo)
{
    ACL_CHECK(aclrtSetDevice(static_cast<int32_t>(deviceId)));

    ChannelResources resources{};
    HCCL_CHECK(HcclCommInitRootInfo(kRankCount, rootInfo, rank, &resources.comm));
    std::fprintf(stderr, "[channel-demo][rank%u] HcclCommInitRootInfo success on device %u\n", rank, deviceId);

    constexpr CommEngine engine = COMM_ENGINE_AICPU;
    HCCL_CHECK(HcclThreadAcquire(resources.comm, engine, 1, 0, &resources.thread));
    std::fprintf(stderr, "[channel-demo][rank%u] HcclThreadAcquire success, thread=%llu, engine=AICPU\n", rank,
                 static_cast<unsigned long long>(resources.thread));

    HcclChannelDesc channelDesc{};
    HCCL_CHECK(HcclChannelDescInit(&channelDesc, 1));
    channelDesc.remoteRank = peerRank;
    channelDesc.channelProtocol = COMM_PROTOCOL_ROCE;
    channelDesc.notifyNum = kNotifyNum;
    HCCL_CHECK(HcclChannelAcquire(resources.comm, engine, &channelDesc, 1, &resources.channel));
    std::fprintf(stderr,
                 "[channel-demo][rank%u] HcclChannelAcquire success, peer=%u, protocol=ROCE, notify_num=%u, channel=%llu\n",
                 rank, peerRank, kNotifyNum, static_cast<unsigned long long>(resources.channel));

    HCCL_CHECK(HcclGetHcclBuffer(resources.comm, &resources.localBuffer, &resources.localBufferSize));
    HCCL_CHECK(HcclChannelGetHcclBuffer(resources.comm, resources.channel, &resources.remoteBuffer,
                                        &resources.remoteBufferSize));
    PrintChannelInfo(rank, resources.channel, resources.localBuffer, resources.localBufferSize,
                     resources.remoteBuffer, resources.remoteBufferSize);
    return resources;
}

void DestroyChannel(ChannelResources& resources, uint32_t rank)
{
    if ((&HcclChannelDestroy != nullptr) && resources.comm != nullptr && resources.channel != 0) {
        (void)HcclChannelDestroy(resources.comm, &resources.channel, 1);
        std::fprintf(stderr, "[channel-demo][rank%u] HcclChannelDestroy done\n", rank);
    }
    resources.channel = 0;
    resources.thread = 0;
    resources.localBuffer = nullptr;
    resources.localBufferSize = 0;
    resources.remoteBuffer = nullptr;
    resources.remoteBufferSize = 0;
    if (resources.comm != nullptr) {
        (void)HcclCommDestroy(resources.comm);
        std::fprintf(stderr, "[channel-demo][rank%u] HcclCommDestroy done\n", rank);
    }
    resources.comm = nullptr;
}

void ReceiverThread(HcclRootInfo* rootInfo, SharedState* state, const DemoCase* demoCase, std::string* error)
{
    ChannelResources resources{};
    void* receiverDeviceBuffer = nullptr;
    try {
        resources = InitializeChannel(kReceiverRank, kSenderRank, kReceiverDevice, rootInfo);
        ACL_CHECK(aclrtMalloc(&receiverDeviceBuffer, demoCase->bytes, ACL_MEM_MALLOC_HUGE_FIRST));
        ACL_CHECK(aclrtMemset(receiverDeviceBuffer, demoCase->bytes, 0, demoCase->bytes));

        std::fprintf(stderr, "[channel-demo][rank0] wait ACK for case=%s bytes=%llu\n", demoCase->name,
                     static_cast<unsigned long long>(demoCase->bytes));
        HCOMM_CHECK(HcommChannelNotifyWaitOnThread(resources.thread, resources.channel, kNotifyIdxAck,
                                                   kWaitTimeoutMs));

        std::fprintf(stderr, "[channel-demo][rank0] HcommReadOnThread begin, dst=%p src=%p bytes=%llu\n",
                     receiverDeviceBuffer, resources.remoteBuffer,
                     static_cast<unsigned long long>(demoCase->bytes));
        HCOMM_CHECK(HcommReadOnThread(resources.thread, resources.channel, receiverDeviceBuffer,
                                      resources.remoteBuffer, demoCase->bytes));

        HCOMM_CHECK(HcommChannelNotifyRecordOnThread(resources.thread, resources.channel, kNotifyIdxData));

        std::vector<uint8_t> receivedHost(demoCase->bytes, 0);
        ACL_CHECK(aclrtMemcpy(receivedHost.data(), demoCase->bytes, receiverDeviceBuffer, demoCase->bytes,
                              ACL_MEMCPY_DEVICE_TO_HOST));

        std::vector<uint8_t> expectedHost(demoCase->bytes, 0);
        FillHostPattern(expectedHost);
        if (std::memcmp(receivedHost.data(), expectedHost.data(), demoCase->bytes) != 0) {
            throw std::runtime_error("receiver payload validation failed");
        }

        std::fprintf(stderr, "[channel-demo][rank0] validation success for case=%s bytes=%llu\n",
                     demoCase->name, static_cast<unsigned long long>(demoCase->bytes));
        state->receiverDone.store(true, std::memory_order_release);
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = ex.what();
        }
    }
    if (receiverDeviceBuffer != nullptr) {
        (void)aclrtFree(receiverDeviceBuffer);
    }
    DestroyChannel(resources, kReceiverRank);
    (void)aclrtResetDevice(static_cast<int32_t>(kReceiverDevice));
}

void SenderThread(HcclRootInfo* rootInfo, SharedState* state, const DemoCase* demoCase, std::string* error)
{
    ChannelResources resources{};
    void* senderDeviceBuffer = nullptr;
    try {
        resources = InitializeChannel(kSenderRank, kReceiverRank, kSenderDevice, rootInfo);

        std::vector<uint8_t> hostBuffer(demoCase->bytes, 0);
        FillHostPattern(hostBuffer);

        ACL_CHECK(aclrtMalloc(&senderDeviceBuffer, demoCase->bytes, ACL_MEM_MALLOC_HUGE_FIRST));
        ACL_CHECK(aclrtMemcpy(senderDeviceBuffer, demoCase->bytes, hostBuffer.data(), demoCase->bytes,
                              ACL_MEMCPY_HOST_TO_DEVICE));

        std::fprintf(stderr, "[channel-demo][rank1] HcommLocalCopyOnThread begin, dst=%p src=%p bytes=%llu\n",
                     resources.localBuffer, senderDeviceBuffer,
                     static_cast<unsigned long long>(demoCase->bytes));
        HCOMM_CHECK(HcommLocalCopyOnThread(resources.thread, resources.localBuffer, senderDeviceBuffer,
                                           demoCase->bytes));

        HCOMM_CHECK(HcommChannelNotifyRecordOnThread(resources.thread, resources.channel, kNotifyIdxAck));
        std::fprintf(stderr, "[channel-demo][rank1] ACK sent, wait DATA_SIGNAL\n");
        HCOMM_CHECK(HcommChannelNotifyWaitOnThread(resources.thread, resources.channel, kNotifyIdxData,
                                                   kWaitTimeoutMs));
        state->senderDone.store(true, std::memory_order_release);
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = ex.what();
        }
    }
    if (senderDeviceBuffer != nullptr) {
        (void)aclrtFree(senderDeviceBuffer);
    }
    DestroyChannel(resources, kSenderRank);
    (void)aclrtResetDevice(static_cast<int32_t>(kSenderDevice));
}

}  // namespace

int main()
{
    try {
        const char* intraRoce = std::getenv("HCCL_INTRA_ROCE_ENABLE");
        std::fprintf(stderr, "[channel-demo] HCCL_INTRA_ROCE_ENABLE=%s\n",
                     intraRoce == nullptr ? "(null)" : intraRoce);

        ACL_CHECK(aclInit(nullptr));

        uint32_t deviceCount = 0;
        ACL_CHECK(aclrtGetDeviceCount(&deviceCount));
        std::fprintf(stderr, "[channel-demo] detected device_count=%u\n", deviceCount);
        if (deviceCount < 2) {
            throw std::runtime_error("channel demo needs at least 2 devices");
        }

        ACL_CHECK(aclrtSetDevice(static_cast<int32_t>(kReceiverDevice)));
        void* rootInfoStorage = nullptr;
        ACL_CHECK(aclrtMallocHost(&rootInfoStorage, sizeof(HcclRootInfo)));
        auto* rootInfo = static_cast<HcclRootInfo*>(rootInfoStorage);
        HCCL_CHECK(HcclGetRootInfo(rootInfo));
        std::fprintf(stderr, "[channel-demo] HcclGetRootInfo success\n");

        for (const auto& demoCase : kCases) {
            std::fprintf(stderr, "\n[channel-demo] ===== case=%s bytes=%llu =====\n",
                         demoCase.name, static_cast<unsigned long long>(demoCase.bytes));
            SharedState state;
            std::string receiverError;
            std::string senderError;

            std::thread receiver([&]() {
                ReceiverThread(rootInfo, &state, &demoCase, &receiverError);
            });
            std::thread sender([&]() {
                SenderThread(rootInfo, &state, &demoCase, &senderError);
            });

            sender.join();
            receiver.join();

            if (!receiverError.empty() || !senderError.empty()) {
                throw std::runtime_error("receiver=" + receiverError + " | sender=" + senderError);
            }
            if (!state.senderDone.load(std::memory_order_acquire) ||
                !state.receiverDone.load(std::memory_order_acquire)) {
                throw std::runtime_error("channel demo did not complete successfully");
            }
        }

        ACL_CHECK(aclrtFreeHost(rootInfoStorage));
        ACL_CHECK(aclFinalize());
        std::fprintf(stderr, "[channel-demo] finished successfully\n");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[channel-demo] failed: %s\n", ex.what());
        return 1;
    }
}
