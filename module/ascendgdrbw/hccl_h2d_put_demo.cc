#include <acl/acl.h>
#include <hccl/hccl.h>
#include <hccl/hccl_types.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hccl_one_sided_services.h"

namespace {

// HCCL 的 one-sided 接口有 remoteRank 参数，
// 所以即使我们想验证“host -> device0”，
// 也仍然需要两个 rank：
// rank0: 持有 device0 上的目标内存
// rank1: 持有 host 源内存，并发起 BatchPut
constexpr uint32_t kReceiverRank = 0;
constexpr uint32_t kSenderRank = 1;
constexpr uint32_t kRankCount = 2;

constexpr uint64_t kElementCount = 1024;
constexpr uint64_t kBytes = kElementCount * sizeof(float);
constexpr int kExchangeTimeoutSeconds = 1800;

struct SharedState {
    std::mutex mutex;
    std::condition_variable cv;
    bool putFinished = false;
} gState;

void CheckAcl(aclError ret, const char* expr)
{
    if (ret != ACL_SUCCESS) {
        throw std::runtime_error(std::string("ACL failed: ") + expr);
    }
}

void CheckHccl(HcclResult ret, const char* expr)
{
    if (ret != HCCL_SUCCESS) {
        throw std::runtime_error(std::string("HCCL failed: ") + expr);
    }
}

#define ACL_CHECK(expr) CheckAcl((expr), #expr)
#define HCCL_CHECK(expr) CheckHccl((expr), #expr)

void FillHostBuffer(std::vector<float>& hostBuffer)
{
    for (size_t i = 0; i < hostBuffer.size(); ++i) {
        hostBuffer[i] = static_cast<float>(i) + 0.5F;
    }
}

void ReceiverThread(HcclRootInfo* rootInfo)
{
    // rank0 负责在 device0 上准备“目标内存”
    ACL_CHECK(aclrtSetDevice(0));

    HcclComm comm;
    HCCL_CHECK(HcclCommInitRootInfo(kRankCount, rootInfo, kReceiverRank, &comm));

    void* deviceBuffer = nullptr;
    ACL_CHECK(aclrtMalloc(&deviceBuffer, kBytes, ACL_MEM_MALLOC_HUGE_FIRST));
    ACL_CHECK(aclrtMemset(deviceBuffer, kBytes, 0, kBytes));

    HcclOneSidedMemDesc localDesc{};
    HcclOneSidedMemDesc remoteDesc{};

    try {
        // 把 device0 上的这块 HBM 注册给 HCCL one-sided 通信域。
        HCCL_CHECK(HcclRegisterMem(comm,
                                   kSenderRank,
                                   ASCENDGDRBW_HCCL_MEM_TYPE_DEVICE,
                                   deviceBuffer,
                                   kBytes,
                                   &localDesc));

        // 把本端描述符和对端交换。
        HcclOneSidedMemDescs localDescs{&localDesc, 1};
        HcclOneSidedMemDescs remoteDescs{&remoteDesc, 1};
        uint32_t actualNum = 0;
        HCCL_CHECK(HcclExchangeMemDesc(comm,
                                       kSenderRank,
                                       &localDescs,
                                       kExchangeTimeoutSeconds,
                                       &remoteDescs,
                                       &actualNum));

        std::fprintf(stderr,
                     "[receiver] device0 buffer=%p bytes=%llu\n",
                     deviceBuffer,
                     static_cast<unsigned long long>(kBytes));

        // 等 sender 完成 put，再把 device 数据拷回 host 看结果。
        {
            std::unique_lock<std::mutex> lock(gState.mutex);
            gState.cv.wait(lock, []() { return gState.putFinished; });
        }

        std::vector<float> hostVerify(kElementCount, 0.0F);
        ACL_CHECK(aclrtMemcpy(hostVerify.data(),
                              kBytes,
                              deviceBuffer,
                              kBytes,
                              ACL_MEMCPY_DEVICE_TO_HOST));

        std::fprintf(stderr,
                     "[receiver] device0 sample after put: [%f, %f, %f, %f]\n",
                     hostVerify[0],
                     hostVerify[1],
                     hostVerify[2],
                     hostVerify[3]);
    } catch (...) {
        HcclDeregisterMem(comm, &localDesc);
        aclrtFree(deviceBuffer);
        HcclCommDestroy(comm);
        aclrtResetDevice(0);
        throw;
    }

    HcclDeregisterMem(comm, &localDesc);
    ACL_CHECK(aclrtFree(deviceBuffer));
    HCCL_CHECK(HcclCommDestroy(comm));
    ACL_CHECK(aclrtResetDevice(0));
}

void SenderThread(HcclRootInfo* rootInfo)
{
    // rank1 负责准备 host 源内存，并发起 one-sided put。
    ACL_CHECK(aclrtSetDevice(1));

    HcclComm comm;
    HCCL_CHECK(HcclCommInitRootInfo(kRankCount, rootInfo, kSenderRank, &comm));

    aclrtStream stream = nullptr;
    ACL_CHECK(aclrtCreateStream(&stream));

    std::vector<float> hostBuffer(kElementCount, 0.0F);
    FillHostBuffer(hostBuffer);

    HcclOneSidedMemDesc localDesc{};
    HcclOneSidedMemDesc remoteDesc{};

    try {
        // 把本端 host DRAM 注册给 HCCL。
        HCCL_CHECK(HcclRegisterMem(comm,
                                   kReceiverRank,
                                   ASCENDGDRBW_HCCL_MEM_TYPE_HOST,
                                   hostBuffer.data(),
                                   kBytes,
                                   &localDesc));

        // 和对端交换内存描述符。
        HcclOneSidedMemDescs localDescs{&localDesc, 1};
        HcclOneSidedMemDescs remoteDescs{&remoteDesc, 1};
        uint32_t actualNum = 0;
        HCCL_CHECK(HcclExchangeMemDesc(comm,
                                       kReceiverRank,
                                       &localDescs,
                                       kExchangeTimeoutSeconds,
                                       &remoteDescs,
                                       &actualNum));

        // 把“对端 device memory 描述符”变成本端可访问的 remoteMem。
        HcclOneSidedMem remoteMem{};
        HCCL_CHECK(HcclEnableMemAccess(comm, &remoteDesc, &remoteMem));

        std::fprintf(stderr,
                     "[sender] host buffer=%p remote device buffer=%p bytes=%llu\n",
                     hostBuffer.data(),
                     remoteMem.addr,
                     static_cast<unsigned long long>(remoteMem.size));

        // 这里才是真正的目标路径：
        // host DRAM -> RoCE/RDMA -> remote device memory
        HcclOneSidedOpDesc opDesc{
            hostBuffer.data(),
            remoteMem.addr,
            kElementCount,
            HCCL_DATA_TYPE_FP32,
        };
        HCCL_CHECK(HcclBatchPut(comm, kReceiverRank, &opDesc, 1, stream));
        ACL_CHECK(aclrtSynchronizeStream(stream));

        HCCL_CHECK(HcclDisableMemAccess(comm, &remoteDesc));

        {
            std::lock_guard<std::mutex> lock(gState.mutex);
            gState.putFinished = true;
        }
        gState.cv.notify_all();
    } catch (...) {
        HcclDeregisterMem(comm, &localDesc);
        aclrtDestroyStream(stream);
        HcclCommDestroy(comm);
        aclrtResetDevice(1);
        throw;
    }

    HcclDeregisterMem(comm, &localDesc);
    ACL_CHECK(aclrtDestroyStream(stream));
    HCCL_CHECK(HcclCommDestroy(comm));
    ACL_CHECK(aclrtResetDevice(1));
}

}  // namespace

int main()
{
    try {
        ACL_CHECK(aclInit(nullptr));

        uint32_t deviceCount = 0;
        ACL_CHECK(aclrtGetDeviceCount(&deviceCount));
        if (deviceCount < 2) {
            throw std::runtime_error("This demo needs 2 devices: device0 as target, device1 as sender rank");
        }

        // 两个 rank 共享同一份 rootInfo。
        ACL_CHECK(aclrtSetDevice(0));
        void* rootInfoStorage = nullptr;
        ACL_CHECK(aclrtMallocHost(&rootInfoStorage, sizeof(HcclRootInfo)));
        auto* rootInfo = static_cast<HcclRootInfo*>(rootInfoStorage);
        HCCL_CHECK(HcclGetRootInfo(rootInfo));

        std::string receiverError;
        std::string senderError;

        std::thread receiver([&]() {
            try {
                ReceiverThread(rootInfo);
            } catch (const std::exception& ex) {
                receiverError = ex.what();
            }
        });

        std::thread sender([&]() {
            try {
                SenderThread(rootInfo);
            } catch (const std::exception& ex) {
                senderError = ex.what();
            }
        });

        sender.join();
        receiver.join();

        ACL_CHECK(aclrtFreeHost(rootInfoStorage));
        ACL_CHECK(aclFinalize());

        if (!receiverError.empty()) {
            throw std::runtime_error(std::string("receiver failed: ") + receiverError);
        }
        if (!senderError.empty()) {
            throw std::runtime_error(std::string("sender failed: ") + senderError);
        }

        std::fprintf(stderr, "hccl_h2d_put_demo finished successfully\n");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "hccl_h2d_put_demo failed: %s\n", ex.what());
        return 1;
    }
}
