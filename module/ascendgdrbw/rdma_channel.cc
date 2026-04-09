#include "rdma_channel.h"

#include <acl/acl.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

#include "error_handle.h"

namespace {

constexpr int kIbvPort = 1;

void LogStep(const char* step, const char* state, const char* format = nullptr, ...)
{
    std::fprintf(stderr, "[ascendgdrbw][rdma_channel] step=%s state=%s", step, state);
    if (format != nullptr) {
        std::fprintf(stderr, " ");
        va_list args;
        va_start(args, format);
        std::vfprintf(stderr, format, args);
        va_end(args);
    }
    std::fprintf(stderr, "\n");
}

struct QPEndpoint {
    uint32_t qpn = 0;
    uint16_t lid = 0;
    uint8_t gid[16] = {};
};

QPEndpoint QueryEndpoint(ibv_qp* queuePair, ibv_context* context)
{
    LogStep("query_endpoint", "begin", "qp=%p context=%p", static_cast<void*>(queuePair),
            static_cast<void*>(context));
    QPEndpoint endpoint;
    endpoint.qpn = queuePair->qp_num;

    ibv_port_attr portAttr = {};
    const int queryPortRc = ibv_query_port(context, kIbvPort, &portAttr);
    if (queryPortRc != 0) {
        LogStep("query_endpoint", "failed", "action=query_port rc=%d errno=%d(%s)", queryPortRc,
                errno, std::strerror(errno));
    }
    ASCENDGDRBW_IBV_ASSERT(queryPortRc);
    endpoint.lid = portAttr.lid;

    union ibv_gid gid = {};
    const int queryGidRc = ibv_query_gid(context, kIbvPort, 0, &gid);
    if (queryGidRc != 0) {
        LogStep("query_endpoint", "failed", "action=query_gid rc=%d errno=%d(%s)", queryGidRc,
                errno, std::strerror(errno));
    }
    ASCENDGDRBW_IBV_ASSERT(queryGidRc);
    std::memcpy(endpoint.gid, &gid, sizeof(endpoint.gid));
    LogStep("query_endpoint", "success", "qpn=%u lid=%u", endpoint.qpn, endpoint.lid);
    return endpoint;
}

void ConnectQueuePair(ibv_qp* queuePair, const QPEndpoint& remoteEndpoint, bool isRoce,
                      ibv_mtu pathMtu)
{
    LogStep("connect_qp", "begin", "qp=%p qpn=%u remote_qpn=%u roce=%d mtu=%d",
            static_cast<void*>(queuePair), queuePair->qp_num, remoteEndpoint.qpn,
            isRoce ? 1 : 0, static_cast<int>(pathMtu));
    {
        ibv_qp_attr attr = {};
        attr.qp_state = IBV_QPS_INIT;
        attr.pkey_index = 0;
        attr.port_num = kIbvPort;
        attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
        const int modifyRc =
            ibv_modify_qp(queuePair, &attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                              IBV_QP_ACCESS_FLAGS);
        if (modifyRc != 0) {
            LogStep("connect_qp", "failed", "action=to_init rc=%d errno=%d(%s)", modifyRc, errno,
                    std::strerror(errno));
        }
        ASCENDGDRBW_IBV_ASSERT(modifyRc);
        LogStep("connect_qp", "success", "action=to_init");
    }

    {
        ibv_qp_attr attr = {};
        attr.qp_state = IBV_QPS_RTR;
        attr.path_mtu = pathMtu;
        attr.dest_qp_num = remoteEndpoint.qpn;
        attr.rq_psn = 0;
        attr.max_dest_rd_atomic = 1;
        attr.min_rnr_timer = 12;
        attr.ah_attr.port_num = kIbvPort;
        attr.ah_attr.sl = 0;
        attr.ah_attr.src_path_bits = 0;
        if (isRoce) {
            attr.ah_attr.is_global = 1;
            attr.ah_attr.grh.hop_limit = 64;
            std::memcpy(&attr.ah_attr.grh.dgid, remoteEndpoint.gid, sizeof(remoteEndpoint.gid));
            attr.ah_attr.grh.sgid_index = 0;
            attr.ah_attr.dlid = 0;
        } else {
            attr.ah_attr.is_global = 0;
            attr.ah_attr.dlid = remoteEndpoint.lid;
        }
        const int modifyRc =
            ibv_modify_qp(queuePair, &attr,
                          IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                              IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                              IBV_QP_MIN_RNR_TIMER);
        if (modifyRc != 0) {
            LogStep("connect_qp", "failed", "action=to_rtr rc=%d errno=%d(%s)", modifyRc, errno,
                    std::strerror(errno));
        }
        ASCENDGDRBW_IBV_ASSERT(modifyRc);
        LogStep("connect_qp", "success", "action=to_rtr");
    }

    {
        ibv_qp_attr attr = {};
        attr.qp_state = IBV_QPS_RTS;
        attr.timeout = 14;
        attr.retry_cnt = 7;
        attr.rnr_retry = 7;
        attr.sq_psn = 0;
        attr.max_rd_atomic = 1;
        const int modifyRc =
            ibv_modify_qp(queuePair, &attr,
                          IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                              IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
        if (modifyRc != 0) {
            LogStep("connect_qp", "failed", "action=to_rts rc=%d errno=%d(%s)", modifyRc, errno,
                    std::strerror(errno));
        }
        ASCENDGDRBW_IBV_ASSERT(modifyRc);
        LogStep("connect_qp", "success", "action=to_rts");
    }
}

ibv_context* OpenNicContext(const std::string& nicName)
{
    LogStep("open_nic", "begin", "nic=%s", nicName.c_str());
    int deviceCount = 0;
    ibv_device** deviceList = ibv_get_device_list(&deviceCount);
    if (deviceList == nullptr) {
        LogStep("open_nic", "failed", "action=get_device_list errno=%d(%s)", errno,
                std::strerror(errno));
    }
    ASCENDGDRBW_ASSERT(deviceList != nullptr);
    ASCENDGDRBW_ASSERT(deviceCount > 0);
    LogStep("open_nic", "success", "action=get_device_list count=%d", deviceCount);

    ibv_context* context = nullptr;
    for (int index = 0; index < deviceCount; ++index) {
        const char* deviceName = ibv_get_device_name(deviceList[index]);
        LogStep("open_nic", "begin", "action=probe_device index=%d name=%s", index, deviceName);
        if (nicName == deviceName) {
            context = ibv_open_device(deviceList[index]);
            if (context == nullptr) {
                LogStep("open_nic", "failed", "action=open_device name=%s errno=%d(%s)",
                        deviceName, errno, std::strerror(errno));
            } else {
                LogStep("open_nic", "success", "action=open_device name=%s context=%p",
                        deviceName, static_cast<void*>(context));
            }
            break;
        }
    }

    if (context == nullptr) {
        ibv_free_device_list(deviceList);
        LogStep("open_nic", "failed", "action=select_device nic=%s", nicName.c_str());
        AscendGdrbwThrowError("RDMA device not found: " + nicName);
    }

    ibv_free_device_list(deviceList);
    return context;
}

}  // namespace

RDMAChannel::RDMAChannel(int32_t deviceId, std::string nicName, const RDMAChannelConfig& config)
    : deviceId_(deviceId), nicName_(std::move(nicName))
{
    LogStep("channel_init", "begin", "device=%d nic=%s", deviceId_, nicName_.c_str());
    const aclError setDeviceRc = aclrtSetDevice(deviceId_);
    if (setDeviceRc != ACL_SUCCESS) {
        LogStep("channel_init", "failed", "action=set_device device=%d rc=%d msg=%s", deviceId_,
                static_cast<int>(setDeviceRc), aclGetRecentErrMsg());
    }
    ASCENDGDRBW_ASCEND_ASSERT(setDeviceRc);
    LogStep("channel_init", "success", "action=set_device device=%d", deviceId_);
    ASCENDGDRBW_ASSERT(config.cqDepth > 0);
    ASCENDGDRBW_ASSERT(config.qpSendWr > 0);
    ASCENDGDRBW_ASSERT(config.qpRecvWr > 0);
    LogStep("channel_init", "success", "action=validate_config cq_depth=%d send_wr=%d recv_wr=%d",
            config.cqDepth, config.qpSendWr, config.qpRecvWr);

    context_ = OpenNicContext(nicName_);
    ASCENDGDRBW_ASSERT(context_ != nullptr);

    LogStep("channel_init", "begin", "action=alloc_pd context=%p", static_cast<void*>(context_));
    protectionDomain_ = ibv_alloc_pd(context_);
    if (protectionDomain_ == nullptr) {
        LogStep("channel_init", "failed", "action=alloc_pd errno=%d(%s)", errno,
                std::strerror(errno));
    }
    ASCENDGDRBW_ASSERT(protectionDomain_ != nullptr);
    LogStep("channel_init", "success", "action=alloc_pd pd=%p",
            static_cast<void*>(protectionDomain_));

    ibv_port_attr portAttr = {};
    const int queryPortRc = ibv_query_port(context_, kIbvPort, &portAttr);
    if (queryPortRc != 0) {
        LogStep("channel_init", "failed", "action=query_port rc=%d errno=%d(%s)", queryPortRc,
                errno, std::strerror(errno));
    }
    ASCENDGDRBW_IBV_ASSERT(queryPortRc);
    const bool isRoce = (portAttr.lid == 0);
    LogStep("channel_init", "success", "action=query_port lid=%u mtu=%d roce=%d", portAttr.lid,
            static_cast<int>(portAttr.active_mtu), isRoce ? 1 : 0);

    ibv_device_attr deviceAttr = {};
    const int queryDeviceRc = ibv_query_device(context_, &deviceAttr);
    if (queryDeviceRc != 0) {
        LogStep("channel_init", "failed", "action=query_device rc=%d errno=%d(%s)",
                queryDeviceRc, errno, std::strerror(errno));
    }
    ASCENDGDRBW_IBV_ASSERT(queryDeviceRc);
    LogStep("channel_init", "success", "action=query_device max_cqe=%d max_qp_wr=%d",
            deviceAttr.max_cqe, deviceAttr.max_qp_wr);

    ASCENDGDRBW_ASSERT(config.cqDepth <= static_cast<int>(deviceAttr.max_cqe));
    ASCENDGDRBW_ASSERT(config.qpSendWr <= static_cast<int>(deviceAttr.max_qp_wr));
    ASCENDGDRBW_ASSERT(config.qpRecvWr <= static_cast<int>(deviceAttr.max_qp_wr));
    ASCENDGDRBW_ASSERT(config.qpSendWr + config.qpRecvWr <= static_cast<int>(deviceAttr.max_qp_wr));

    const int cqDepth = config.cqDepth;
    LogStep("channel_init", "begin", "action=create_cq depth=%d", cqDepth);
    completionQueue_ = ibv_create_cq(context_, cqDepth, nullptr, nullptr, 0);
    if (completionQueue_ == nullptr) {
        LogStep("channel_init", "failed", "action=create_cq errno=%d(%s)", errno,
                std::strerror(errno));
    }
    ASCENDGDRBW_ASSERT(completionQueue_ != nullptr);
    LogStep("channel_init", "success", "action=create_cq cq=%p", static_cast<void*>(completionQueue_));

    ibv_qp_init_attr qpInitAttr = {};
    qpInitAttr.send_cq = completionQueue_;
    qpInitAttr.recv_cq = completionQueue_;
    qpInitAttr.cap.max_send_wr = config.qpSendWr;
    qpInitAttr.cap.max_recv_wr = config.qpRecvWr;
    qpInitAttr.cap.max_send_sge = 1;
    qpInitAttr.cap.max_recv_sge = 1;
    qpInitAttr.qp_type = IBV_QPT_RC;
    qpInitAttr.sq_sig_all = 0;

    LogStep("channel_init", "begin", "action=create_qp send_wr=%d recv_wr=%d",
            qpInitAttr.cap.max_send_wr, qpInitAttr.cap.max_recv_wr);
    queuePair_ = ibv_create_qp(protectionDomain_, &qpInitAttr);
    if (queuePair_ == nullptr) {
        LogStep("channel_init", "failed", "action=create_qp errno=%d(%s)", errno,
                std::strerror(errno));
    }
    ASCENDGDRBW_ASSERT(queuePair_ != nullptr);
    LogStep("channel_init", "success", "action=create_qp qp=%p qpn=%u",
            static_cast<void*>(queuePair_), queuePair_->qp_num);

    const auto cqWindow = static_cast<uint32_t>(std::max(1, cqDepth - 1));
    maxOutstandingWorkRequests_ = std::max<uint32_t>(
        1u, std::min<uint32_t>(qpInitAttr.cap.max_send_wr, cqWindow));
    LogStep("channel_init", "success", "action=compute_outstanding_limit max_outstanding=%u",
            maxOutstandingWorkRequests_);

    const QPEndpoint endpoint = QueryEndpoint(queuePair_, context_);
    ConnectQueuePair(queuePair_, endpoint, isRoce, portAttr.active_mtu);
    LogStep("channel_init", "success", "device=%d nic=%s qpn=%u", deviceId_, nicName_.c_str(),
            queuePair_->qp_num);
}

RDMAChannel::~RDMAChannel()
{
    LogStep("channel_destroy", "begin", "device=%d nic=%s", deviceId_, nicName_.c_str());
    if (queuePair_ != nullptr) { ibv_destroy_qp(queuePair_); }
    if (completionQueue_ != nullptr) { ibv_destroy_cq(completionQueue_); }
    if (protectionDomain_ != nullptr) { ibv_dealloc_pd(protectionDomain_); }
    if (context_ != nullptr) { ibv_close_device(context_); }
    LogStep("channel_destroy", "success", "device=%d nic=%s", deviceId_, nicName_.c_str());
}

MemoryRegistration* RDMAChannel::RegisterHostMemory(void* buffer, size_t bytes)
{
    LogStep("register_host_memory", "begin", "device=%d buffer=%p bytes=%zu", deviceId_, buffer,
            bytes);
    ASCENDGDRBW_ASSERT(buffer != nullptr);
    ASCENDGDRBW_ASSERT(bytes > 0);
    ibv_mr* memoryRegion =
        ibv_reg_mr(protectionDomain_, buffer, bytes, IBV_ACCESS_LOCAL_WRITE);
    if (memoryRegion == nullptr) {
        LogStep("register_host_memory", "failed", "device=%d errno=%d(%s)", deviceId_, errno,
                std::strerror(errno));
    }
    ASCENDGDRBW_ERRNO_ASSERT(memoryRegion != nullptr);
    LogStep("register_host_memory", "success", "device=%d mr=%p lkey=%u rkey=%u", deviceId_,
            static_cast<void*>(memoryRegion), memoryRegion->lkey, memoryRegion->rkey);
    return memoryRegion;
}

MemoryRegistration* RDMAChannel::RegisterDeviceMemory(void* buffer, size_t bytes)
{
    LogStep("register_device_memory", "begin", "device=%d buffer=%p bytes=%zu", deviceId_, buffer,
            bytes);
    ASCENDGDRBW_ASSERT(buffer != nullptr);
    ASCENDGDRBW_ASSERT(bytes > 0);
    const aclError setDeviceRc = aclrtSetDevice(deviceId_);
    if (setDeviceRc != ACL_SUCCESS) {
        LogStep("register_device_memory", "failed", "action=set_device device=%d rc=%d msg=%s",
                deviceId_, static_cast<int>(setDeviceRc), aclGetRecentErrMsg());
    }
    ASCENDGDRBW_ASCEND_ASSERT(setDeviceRc);
    ibv_mr* memoryRegion = ibv_reg_mr(
        protectionDomain_, buffer, bytes, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (memoryRegion == nullptr) {
        LogStep("register_device_memory", "failed", "device=%d errno=%d(%s)", deviceId_, errno,
                std::strerror(errno));
    }
    ASCENDGDRBW_ERRNO_ASSERT(memoryRegion != nullptr);
    LogStep("register_device_memory", "success", "device=%d mr=%p lkey=%u rkey=%u", deviceId_,
            static_cast<void*>(memoryRegion), memoryRegion->lkey, memoryRegion->rkey);
    return memoryRegion;
}

void RDMAChannel::DeregisterMemory(MemoryRegistration* registration) noexcept
{
    if (registration == nullptr) {
        LogStep("deregister_memory", "success", "device=%d action=skip_null", deviceId_);
        return;
    }

    LogStep("deregister_memory", "begin", "device=%d mr=%p lkey=%u rkey=%u", deviceId_,
            static_cast<void*>(registration), registration->lkey, registration->rkey);
    const int deregRc = ibv_dereg_mr(registration);
    if (deregRc != 0) {
        LogStep("deregister_memory", "failed", "device=%d rc=%d errno=%d(%s)", deviceId_,
                deregRc, errno, std::strerror(errno));
        return;
    }
    LogStep("deregister_memory", "success", "device=%d", deviceId_);
}

uint64_t RDMAChannel::SubmitWrite(uint64_t localAddress, uint32_t localLKey, uint64_t remoteAddress,
                                  uint32_t remoteRKey, size_t bytes)
{
    ASCENDGDRBW_ASSERT(bytes > 0);
    ASCENDGDRBW_ASSERT(bytes <= std::numeric_limits<uint32_t>::max());

    while (outstandingWorkRequests_ >= maxOutstandingWorkRequests_) { PollOneCompletion(); }

    ibv_sge scatterGather = {};
    scatterGather.addr = localAddress;
    scatterGather.length = static_cast<uint32_t>(bytes);
    scatterGather.lkey = localLKey;

    ibv_send_wr workRequest = {};
    workRequest.wr_id = nextWorkRequestId_;
    workRequest.opcode = IBV_WR_RDMA_WRITE;
    workRequest.sg_list = &scatterGather;
    workRequest.num_sge = 1;
    workRequest.send_flags = IBV_SEND_SIGNALED;
    workRequest.wr.rdma.remote_addr = remoteAddress;
    workRequest.wr.rdma.rkey = remoteRKey;

    ibv_send_wr* badWorkRequest = nullptr;
    const uint64_t workRequestId = nextWorkRequestId_;
    const int postSendRc = ibv_post_send(queuePair_, &workRequest, &badWorkRequest);
    if (postSendRc != 0) {
        LogStep("submit_write", "failed",
                "wr_id=%llu bytes=%zu local_addr=0x%llx remote_addr=0x%llx rc=%d errno=%d(%s)",
                static_cast<unsigned long long>(workRequestId), bytes,
                static_cast<unsigned long long>(localAddress),
                static_cast<unsigned long long>(remoteAddress), postSendRc, errno,
                std::strerror(errno));
    }
    ASCENDGDRBW_IBV_ASSERT(postSendRc);

    ++outstandingWorkRequests_;
    return nextWorkRequestId_++;
}

void RDMAChannel::Wait(uint64_t targetWorkRequestId)
{
    while (completedWorkRequestId_ < targetWorkRequestId) { PollOneCompletion(); }
}

void RDMAChannel::PollOneCompletion()
{
    ibv_wc workCompletion = {};
    while (true) {
        const int pollResult = ibv_poll_cq(completionQueue_, 1, &workCompletion);
        if (pollResult < 0) {
            LogStep("poll_completion", "failed", "poll_result=%d errno=%d(%s)", pollResult, errno,
                    std::strerror(errno));
        }
        ASCENDGDRBW_ASSERT(pollResult >= 0);
        if (pollResult == 0) {
            std::this_thread::yield();
            continue;
        }
        break;
    }

    if (workCompletion.status != IBV_WC_SUCCESS) {
        LogStep("poll_completion", "failed", "wr_id=%llu status=%s",
                static_cast<unsigned long long>(workCompletion.wr_id),
                ibv_wc_status_str(workCompletion.status));
    }
    ASCENDGDRBW_WC_ASSERT(workCompletion.status, workCompletion.wr_id);
    completedWorkRequestId_ = std::max(completedWorkRequestId_, workCompletion.wr_id);
    if (outstandingWorkRequests_ > 0) { --outstandingWorkRequests_; }
}

ChannelManager& ChannelManager::Instance()
{
    static ChannelManager manager;
    return manager;
}

void ChannelManager::Initialize(int32_t deviceNumber, const std::vector<std::string>& nicNames,
                                const RDMAChannelConfig& config)
{
    LogStep("channel_manager_init", "begin", "device_number=%d", deviceNumber);
    std::lock_guard<std::mutex> lock(mutex_);
    ASCENDGDRBW_ASSERT(!initialized_);
    ASCENDGDRBW_ASSERT(deviceNumber > 0);
    ASCENDGDRBW_ASSERT(nicNames.size() == static_cast<size_t>(deviceNumber));

    channels_.clear();
    deviceIds_.clear();
    deviceIds_.reserve(static_cast<size_t>(deviceNumber));
    for (int32_t deviceId = 0; deviceId < deviceNumber; ++deviceId) {
        LogStep("channel_manager_init", "begin", "action=create_channel device=%d nic=%s",
                deviceId, nicNames[deviceId].c_str());
        channels_.emplace(deviceId,
                          std::make_unique<RDMAChannel>(deviceId, nicNames[deviceId], config));
        deviceIds_.push_back(deviceId);
        LogStep("channel_manager_init", "success", "action=create_channel device=%d nic=%s",
                deviceId, nicNames[deviceId].c_str());
    }
    initialized_ = true;
    LogStep("channel_manager_init", "success", "device_number=%d", deviceNumber);
}

RDMAChannel& ChannelManager::Get(int32_t deviceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    ASCENDGDRBW_ASSERT(initialized_);
    const auto it = channels_.find(deviceId);
    ASCENDGDRBW_ASSERT(it != channels_.end());
    return *it->second;
}

bool ChannelManager::IsInitialized() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
}

void ChannelManager::Shutdown() noexcept
{
    LogStep("channel_manager_shutdown", "begin", "device_count=%zu", channels_.size());
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.clear();
    deviceIds_.clear();
    initialized_ = false;
    LogStep("channel_manager_shutdown", "success");
}
