#include "hcomm_roce_h2d_session.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#ifdef ACLBW_ENABLE_HCOMM_EXPERIMENT
#include <arpa/inet.h>

#include "hcomm_experimental_api.h"
#endif

namespace {
constexpr const char* kIntraRoceEnableEnv = "HCCL_INTRA_ROCE_ENABLE";

#ifdef ACLBW_ENABLE_HCOMM_EXPERIMENT
std::string FormatFailure(const std::string& stage, int32_t code)
{
    return fmt::format("{} failed, ret={}", stage, code);
}

std::string CommProtocolToString(AclbwCommProtocol protocol)
{
    switch (protocol) {
        case ACLBW_COMM_PROTOCOL_HCCS:
            return "HCCS";
        case ACLBW_COMM_PROTOCOL_TCP:
            return "TCP";
        case ACLBW_COMM_PROTOCOL_ROCE:
            return "ROCE";
        case ACLBW_COMM_PROTOCOL_UB_CTP:
            return "UB_CTP";
        case ACLBW_COMM_PROTOCOL_UB_TP:
            return "UB_TP";
        case ACLBW_COMM_PROTOCOL_PCIE:
            return "PCIE";
        case ACLBW_COMM_PROTOCOL_SIO:
            return "SIO";
        default:
            return "UNKNOWN";
    }
}

std::string CommAddrToString(const AclbwCommAddr& addr)
{
    std::array<char, INET6_ADDRSTRLEN> buf{};
    if (addr.type == ACLBW_COMM_ADDR_TYPE_IP_V4) {
        const auto* ip = reinterpret_cast<const struct in_addr*>(&addr.addr);
        if (inet_ntop(AF_INET, ip, buf.data(), static_cast<socklen_t>(buf.size())) != nullptr) {
            return buf.data();
        }
    }
    if (addr.type == ACLBW_COMM_ADDR_TYPE_IP_V6) {
        const auto* ip = reinterpret_cast<const struct in6_addr*>(&addr.addr6);
        if (inet_ntop(AF_INET6, ip, buf.data(), static_cast<socklen_t>(buf.size())) != nullptr) {
            return buf.data();
        }
    }
    if (addr.type == ACLBW_COMM_ADDR_TYPE_ID) {
        return fmt::format("id:{}", addr.id);
    }
    return fmt::format("type:{}", static_cast<int32_t>(addr.type));
}

std::string EndPointToString(const AclbwEndPoint& endpoint)
{
    return fmt::format("protocol={}, addr={}", CommProtocolToString(endpoint.protocol),
                       CommAddrToString(endpoint.commAddr));
}
#endif
}  // namespace

struct HcommRoceH2dSession::Impl {
    int32_t deviceId;
    void* hostBase;
    void* deviceBase;
    size_t totalSize;
    bool ready = false;
    std::string error;
    std::string summary;

#ifdef ACLBW_ENABLE_HCOMM_EXPERIMENT
    HcclComm comm = nullptr;
    ThreadHandle thread = 0;
    AclbwEndPoint* endpointList = nullptr;
    uint32_t endpointCount = 0;
    AclbwEndPoint localEndpoint{};
    AclbwEndPoint remoteEndpoint{};
    AclbwEndPointHandle localEndpointHandle = nullptr;
    AclbwEndPointHandle remoteEndpointHandle = nullptr;
    void* hostMemHandle = nullptr;
    void* deviceMemHandle = nullptr;
    void* exportedDeviceMemDesc = nullptr;
    uint32_t exportedDeviceMemDescLen = 0;
    AclbwHcommBuf importedRemoteBuffer{};
    bool importedRemoteBufferReady = false;
    ChannelHandle channel = 0;
#endif

    Impl(int32_t inDeviceId, void* inHostBase, void* inDeviceBase, size_t inTotalSize)
        : deviceId(inDeviceId), hostBase(inHostBase), deviceBase(inDeviceBase), totalSize(inTotalSize)
    {
    }
};

HcommRoceH2dSession::HcommRoceH2dSession(int32_t deviceId, void* hostBase, void* deviceBase,
                                         size_t totalSize)
    : impl_(std::make_unique<Impl>(deviceId, hostBase, deviceBase, totalSize))
{
#ifdef ACLBW_ENABLE_HCOMM_EXPERIMENT
    if (impl_->deviceId < 0) {
        impl_->error = "invalid device id";
        return;
    }
    if (impl_->hostBase == nullptr || impl_->deviceBase == nullptr || impl_->totalSize == 0) {
        impl_->error = "invalid host/device buffer arguments";
        return;
    }

    const char* intraRoce = std::getenv(kIntraRoceEnableEnv);
    if (intraRoce == nullptr || std::strcmp(intraRoce, "1") != 0) {
        impl_->error = fmt::format("{} must be set to 1", kIntraRoceEnableEnv);
        return;
    }
    fmt::print("[aclbw][hcomm] {}=1 confirmed\n", kIntraRoceEnableEnv);

    if (aclrtSetDevice(impl_->deviceId) != ACL_SUCCESS) {
        impl_->error = "aclrtSetDevice failed";
        return;
    }

    int32_t deviceList[1] = {impl_->deviceId};
    const auto commRet = HcclCommInitAll(1U, deviceList, &impl_->comm);
    if (commRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcclCommInitAll", commRet);
        return;
    }
    fmt::print("[aclbw][hcomm] HcclCommInitAll success on device {}\n", impl_->deviceId);

    const auto threadRet =
        HcclThreadAcquire(impl_->comm, static_cast<CommEngine>(ACLBW_COMM_ENGINE_AICPU), 1U, 0U,
                          &impl_->thread);
    if (threadRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcclThreadAcquire", threadRet);
        return;
    }
    fmt::print("[aclbw][hcomm] HcclThreadAcquire success, thread={}\n", impl_->thread);

    const auto endpointRet =
        HcommEndPointGet(impl_->deviceId, &impl_->endpointList, &impl_->endpointCount);
    if (endpointRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcommEndPointGet", endpointRet);
        return;
    }
    fmt::print("[aclbw][hcomm] HcommEndPointGet success, endpoint_count={}\n",
               impl_->endpointCount);

    std::vector<AclbwEndPoint> roceEndpoints;
    roceEndpoints.reserve(impl_->endpointCount);
    for (uint32_t i = 0; i < impl_->endpointCount; ++i) {
        const auto& endpoint = impl_->endpointList[i];
        fmt::print("[aclbw][hcomm] endpoint[{}] {}\n", i, EndPointToString(endpoint));
        if (endpoint.protocol == ACLBW_COMM_PROTOCOL_ROCE) {
            roceEndpoints.push_back(endpoint);
        }
    }
    if (roceEndpoints.empty()) {
        impl_->error = "no ROCE endpoint found";
        return;
    }

    impl_->localEndpoint = roceEndpoints.front();
    impl_->remoteEndpoint =
        (roceEndpoints.size() > 1U) ? roceEndpoints[1] : roceEndpoints.front();
    if (roceEndpoints.size() == 1U) {
        fmt::print("[aclbw][hcomm] only one ROCE endpoint exposed, reusing it for local/remote\n");
    }

    const auto localEndpointRet =
        HcommEndPointCreate(&impl_->localEndpoint, &impl_->localEndpointHandle);
    if (localEndpointRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcommEndPointCreate(local)", localEndpointRet);
        return;
    }
    fmt::print("[aclbw][hcomm] local endpoint open success: {}\n",
               EndPointToString(impl_->localEndpoint));

    const auto remoteEndpointRet =
        HcommEndPointCreate(&impl_->remoteEndpoint, &impl_->remoteEndpointHandle);
    if (remoteEndpointRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcommEndPointCreate(remote)", remoteEndpointRet);
        return;
    }
    fmt::print("[aclbw][hcomm] remote endpoint open success: {}\n",
               EndPointToString(impl_->remoteEndpoint));

    AclbwHcclMem hostMem{ACLBW_HCCL_MEM_TYPE_HOST, impl_->hostBase, impl_->totalSize};
    const auto hostMemRet =
        HcommMemReg(impl_->localEndpointHandle, hostMem, &impl_->hostMemHandle);
    if (hostMemRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcommMemReg(host)", hostMemRet);
        return;
    }
    fmt::print("[aclbw][hcomm] host memory register success, size={}\n", impl_->totalSize);

    AclbwHcclMem deviceMem{ACLBW_HCCL_MEM_TYPE_DEVICE, impl_->deviceBase, impl_->totalSize};
    const auto deviceMemRet =
        HcommMemReg(impl_->remoteEndpointHandle, deviceMem, &impl_->deviceMemHandle);
    if (deviceMemRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcommMemReg(device)", deviceMemRet);
        return;
    }
    fmt::print("[aclbw][hcomm] device memory register success, size={}\n", impl_->totalSize);

    const auto exportRet = HcommMemExport(impl_->remoteEndpointHandle, impl_->deviceMemHandle,
                                          &impl_->exportedDeviceMemDesc,
                                          &impl_->exportedDeviceMemDescLen);
    if (exportRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcommMemExport(device)", exportRet);
        return;
    }
    fmt::print("[aclbw][hcomm] device memory export success, desc_len={}\n",
               impl_->exportedDeviceMemDescLen);

    const auto importRet =
        HcommMemImport(impl_->localEndpointHandle, impl_->exportedDeviceMemDesc,
                       impl_->exportedDeviceMemDescLen, &impl_->importedRemoteBuffer);
    if (importRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcommMemImport(device)", importRet);
        return;
    }
    impl_->importedRemoteBufferReady = true;
    fmt::print("[aclbw][hcomm] device memory import success, remote_addr={}\n",
               fmt::ptr(impl_->importedRemoteBuffer.addr));

    AclbwHcommChannelDesc channelDesc{};
    channelDesc.remoteEndPoint = impl_->remoteEndpoint;
    channelDesc.notifyNum = 0;
    channelDesc.roceAttr.queueNum = 1;
    channelDesc.roceAttr.retryCnt = 0;
    channelDesc.roceAttr.retryInterval = 0;
    channelDesc.roceAttr.tc = 0;
    channelDesc.roceAttr.sl = 0;
    const void* memHandles[1] = {impl_->hostMemHandle};
    const auto channelRet = HcommChannelCreate(&impl_->localEndpointHandle,
                                               ACLBW_COMM_ENGINE_AICPU,
                                               &channelDesc, 1, memHandles, 1,
                                               &impl_->channel);
    if (channelRet != HCCL_SUCCESS) {
        impl_->error = FormatFailure("HcommChannelCreate", channelRet);
        return;
    }
    fmt::print("[aclbw][hcomm] channel create success, protocol=ROCE, channel={}\n",
               impl_->channel);

    impl_->summary = fmt::format("local={}, remote={}, channel={}, imported_remote={}",
                                 EndPointToString(impl_->localEndpoint),
                                 EndPointToString(impl_->remoteEndpoint), impl_->channel,
                                 fmt::ptr(impl_->importedRemoteBuffer.addr));
    impl_->ready = true;
#else
    impl_->error = "ACLBW_ENABLE_HCOMM_EXPERIMENT is not enabled at build time";
#endif
}

HcommRoceH2dSession::~HcommRoceH2dSession()
{
#ifdef ACLBW_ENABLE_HCOMM_EXPERIMENT
    if (!impl_) {
        return;
    }
    if (impl_->channel != 0) {
        (void)HcommChannelDestroy(&impl_->channel, 1);
    }
    if (impl_->importedRemoteBufferReady) {
        (void)HcommMemClose(impl_->localEndpointHandle, &impl_->importedRemoteBuffer);
    }
    if (impl_->hostMemHandle != nullptr) {
        (void)HcommMemUnReg(impl_->localEndpointHandle, impl_->hostMemHandle);
    }
    if (impl_->deviceMemHandle != nullptr) {
        (void)HcommMemUnReg(impl_->remoteEndpointHandle, impl_->deviceMemHandle);
    }
    if (impl_->localEndpointHandle != nullptr) {
        (void)HcommEndPointDestroy(impl_->localEndpointHandle);
    }
    if (impl_->remoteEndpointHandle != nullptr) {
        (void)HcommEndPointDestroy(impl_->remoteEndpointHandle);
    }
    if (impl_->exportedDeviceMemDesc != nullptr) {
        std::free(impl_->exportedDeviceMemDesc);
    }
    if (impl_->comm != nullptr) {
        (void)HcclCommDestroy(impl_->comm);
    }
#endif
}

bool HcommRoceH2dSession::IsReady() const noexcept
{
    return impl_ != nullptr && impl_->ready;
}

const std::string& HcommRoceH2dSession::ErrorMessage() const noexcept
{
    return impl_->error;
}

void HcommRoceH2dSession::PrintSummary() const
{
    if (impl_ == nullptr) {
        return;
    }
    if (!impl_->summary.empty()) {
        fmt::print("[aclbw][hcomm] {}\n", impl_->summary);
    }
}

void HcommRoceH2dSession::Write(void* src, void* dst, size_t size) const
{
#ifdef ACLBW_ENABLE_HCOMM_EXPERIMENT
    if (!IsReady()) {
        throw std::runtime_error(ErrorMessage());
    }
    if (src == nullptr || dst == nullptr || size == 0) {
        throw std::runtime_error("invalid HCOMM write arguments");
    }

    const auto* dstBase = static_cast<const uint8_t*>(impl_->deviceBase);
    const auto* curDst = static_cast<const uint8_t*>(dst);
    if (curDst < dstBase || static_cast<size_t>(curDst - dstBase) + size > impl_->totalSize) {
        throw std::runtime_error("destination pointer is out of registered device range");
    }

    auto offset = static_cast<size_t>(curDst - dstBase);
    auto* remoteDst = static_cast<uint8_t*>(impl_->importedRemoteBuffer.addr) + offset;
    const auto ret = HcommWriteOnThread(impl_->thread, impl_->channel, remoteDst, src, size);
    if (ret != HCCL_SUCCESS) {
        throw std::runtime_error(FormatFailure("HcommWriteOnThread", ret));
    }
#else
    (void)src;
    (void)dst;
    (void)size;
    throw std::runtime_error(ErrorMessage());
#endif
}

void HcommRoceH2dSession::Fence() const
{
#ifdef ACLBW_ENABLE_HCOMM_EXPERIMENT
    if (!IsReady()) {
        throw std::runtime_error(ErrorMessage());
    }
    const auto ret = HcommChannelFenceOnThread(impl_->thread, impl_->channel);
    if (ret != HCCL_SUCCESS) {
        throw std::runtime_error(FormatFailure("HcommChannelFenceOnThread", ret));
    }
#else
    throw std::runtime_error(ErrorMessage());
#endif
}
