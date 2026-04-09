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
#include "rdma_channel.h"

#include <acl/acl.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cstdlib>
#include <exception>
#include <new>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

#include "adapter_hccp.h"
#include "adapter_hccp_common.h"
#include "error_handle.h"
#include "externalinput_pub.h"
#include "hccl_ip_address.h"
#include "hccl_network_pub.h"

// Pull in only the specific hcomm APIs used here so this target does not
// depend on the full env/op-base header stack and its runtime-private headers.
HcclResult InitEnvConfig();
HcclResult HcclDeviceRefresh(s32 &deviceLogicId);
extern "C" HcclResult hrtGetDevicePhyIdByIndex(u32 deviceLogicId, u32 &devicePhyId, bool isRefresh);

namespace {

constexpr int kIbvPort = 1;

struct QPEndpoint {
    uint32_t qpn = 0;
    uint16_t lid = 0;
    int gidIndex = 0;
    uint8_t gid[16] = {};
};

void CheckHccl(HcclResult ret, const char* expr)
{
    if (ret != HCCL_SUCCESS) {
        AscendGdrbwThrowError(std::string(expr) + " failed, ret=" + std::to_string(static_cast<int>(ret)));
    }
}

#define ASCENDGDRBW_HCCL_ASSERT(expr) CheckHccl((expr), #expr)

bool IsDirectDeviceIbvRegDisabledByEnv()
{
    const char* value = std::getenv("ASCENDGDRBW_DISABLE_DIRECT_DEVICE_IBV_REG");
    if (value == nullptr) {
        return false;
    }
    return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
           std::strcmp(value, "ON") == 0;
}

bool CompareIpAndGid(const hccl::HcclIpAddress& localIp, const union ibv_gid& gid)
{
    if (localIp.GetFamily() == AF_INET6) {
        const auto binary = localIp.GetBinaryAddress();
        return std::memcmp(&gid, &binary.addr6, sizeof(gid)) == 0;
    }

    uint32_t gidV4[4] = {0, 0, htonl(0x0000FFFF), localIp.GetBinaryAddress().addr.s_addr};
    return std::memcmp(&gid, gidV4, sizeof(gid)) == 0;
}

hccl::HcclIpAddress ResolveDeviceIp(uint32_t devicePhyId, int32_t deviceLogicId)
{
    std::fprintf(stderr, "[ascendgdrbw] ResolveDeviceIp begin: phy=%u logic=%d\n", devicePhyId, deviceLogicId);
    std::vector<hccl::HcclIpAddress> deviceIps;
    ASCENDGDRBW_HCCL_ASSERT(hrtRaGetDeviceIP(devicePhyId, deviceIps));
    std::fprintf(stderr, "[ascendgdrbw] ResolveDeviceIp via hrtRaGetDeviceIP: phy=%u logic=%d candidates=%zu\n",
                 devicePhyId, deviceLogicId, deviceIps.size());
    for (size_t index = 0; index < deviceIps.size(); ++index) {
        std::fprintf(stderr, "[ascendgdrbw]   candidate[%zu]=%s family=%d invalid=%d\n", index,
                     deviceIps[index].GetReadableAddress(), deviceIps[index].GetFamily(),
                     deviceIps[index].IsInvalid() ? 1 : 0);
    }
    for (const auto& ip : deviceIps) {
        if (!ip.IsInvalid() && !ip.IsIPv6()) {
            std::fprintf(stderr, "[ascendgdrbw] ResolveDeviceIp select IPv4=%s\n",
                         ip.GetReadableAddress());
            return ip;
        }
    }
    for (const auto& ip : deviceIps) {
        if (!ip.IsInvalid()) {
            std::fprintf(stderr, "[ascendgdrbw] ResolveDeviceIp select fallback=%s\n",
                         ip.GetReadableAddress());
            return ip;
        }
    }
    AscendGdrbwThrowError("failed to resolve device NIC IP from hrtRaGetDeviceIP");
    return hccl::HcclIpAddress();
}

ibv_context* OpenNicContextByDeviceIp(const hccl::HcclIpAddress& deviceIp,
                                      std::string& resolvedIbvDeviceName, int& gidIndex)
{
    int deviceCount = 0;
    ibv_device** deviceList = ibv_get_device_list(&deviceCount);
    ASCENDGDRBW_ASSERT(deviceList != nullptr);
    ASCENDGDRBW_ASSERT(deviceCount > 0);

    ibv_context* matchedContext = nullptr;
    for (int index = 0; index < deviceCount; ++index) {
        std::fprintf(stderr, "[ascendgdrbw] Probe ibv device[%d]=%s for device_ip=%s\n", index,
                     ibv_get_device_name(deviceList[index]), deviceIp.GetReadableAddress());
        ibv_context* context = ibv_open_device(deviceList[index]);
        if (context == nullptr) {
            std::fprintf(stderr, "[ascendgdrbw]   open failed errno=%d(%s)\n", errno,
                         std::strerror(errno));
            continue;
        }

        ibv_port_attr portAttr = {};
        const int queryPortRet = ibv_query_port(context, kIbvPort, &portAttr);
        if (queryPortRet != 0) {
            (void)ibv_close_device(context);
            continue;
        }

        int matchedGidIndex = -1;
        for (int gidTableIndex = 0; gidTableIndex < portAttr.gid_tbl_len; ++gidTableIndex) {
            union ibv_gid gid = {};
            if (ibv_query_gid(context, kIbvPort, gidTableIndex, &gid) != 0) {
                continue;
            }
            if (CompareIpAndGid(deviceIp, gid)) {
                matchedGidIndex = gidTableIndex;
                break;
            }
        }

        if (matchedGidIndex >= 0) {
            matchedContext = context;
            gidIndex = matchedGidIndex;
            resolvedIbvDeviceName = ibv_get_device_name(deviceList[index]);
            std::fprintf(stderr,
                         "[ascendgdrbw]   matched ibv device=%s gid_index=%d for device_ip=%s\n",
                         resolvedIbvDeviceName.c_str(), gidIndex, deviceIp.GetReadableAddress());
            break;
        }

        (void)ibv_close_device(context);
    }

    ibv_free_device_list(deviceList);
    if (matchedContext == nullptr) {
        AscendGdrbwThrowError("failed to resolve ibverbs device by device IP " +
                              std::string(deviceIp.GetReadableAddress()));
    }
    return matchedContext;
}

ibv_context* OpenNicContextByName(const std::string& nicName)
{
    int deviceCount = 0;
    ibv_device** deviceList = ibv_get_device_list(&deviceCount);
    ASCENDGDRBW_ASSERT(deviceList != nullptr);
    ASCENDGDRBW_ASSERT(deviceCount > 0);

    ibv_context* context = nullptr;
    for (int index = 0; index < deviceCount; ++index) {
        if (nicName == ibv_get_device_name(deviceList[index])) {
            context = ibv_open_device(deviceList[index]);
            break;
        }
    }

    ibv_free_device_list(deviceList);
    return context;
}

QPEndpoint QueryEndpoint(ibv_qp* queuePair, ibv_context* context, int gidIndex)
{
    QPEndpoint endpoint;
    endpoint.qpn = queuePair->qp_num;
    endpoint.gidIndex = gidIndex;

    ibv_port_attr portAttr = {};
    ASCENDGDRBW_IBV_ASSERT(ibv_query_port(context, kIbvPort, &portAttr));
    endpoint.lid = portAttr.lid;

    union ibv_gid gid = {};
    ASCENDGDRBW_IBV_ASSERT(ibv_query_gid(context, kIbvPort, gidIndex, &gid));
    std::memcpy(endpoint.gid, &gid, sizeof(endpoint.gid));
    return endpoint;
}

void ConnectQueuePair(ibv_qp* queuePair, const QPEndpoint& remoteEndpoint, bool isRoce,
                      ibv_mtu pathMtu)
{
    {
        ibv_qp_attr attr = {};
        attr.qp_state = IBV_QPS_INIT;
        attr.pkey_index = 0;
        attr.port_num = kIbvPort;
        attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE;
        ASCENDGDRBW_IBV_ASSERT(
            ibv_modify_qp(queuePair, &attr,
                          IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                              IBV_QP_ACCESS_FLAGS));
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
            attr.ah_attr.grh.sgid_index = static_cast<uint8_t>(remoteEndpoint.gidIndex);
            attr.ah_attr.dlid = 0;
        } else {
            attr.ah_attr.is_global = 0;
            attr.ah_attr.dlid = remoteEndpoint.lid;
        }
        ASCENDGDRBW_IBV_ASSERT(
            ibv_modify_qp(queuePair, &attr,
                          IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                              IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                              IBV_QP_MIN_RNR_TIMER));
    }

    {
        ibv_qp_attr attr = {};
        attr.qp_state = IBV_QPS_RTS;
        attr.timeout = 14;
        attr.retry_cnt = 7;
        attr.rnr_retry = 7;
        attr.sq_psn = 0;
        attr.max_rd_atomic = 1;
        ASCENDGDRBW_IBV_ASSERT(
            ibv_modify_qp(queuePair, &attr,
                          IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                              IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC));
    }
}

}  // namespace

RDMAChannel::RDMAChannel(int32_t deviceId, std::string nicName, const RDMAChannelConfig& config)
    : deviceId_(deviceId), nicName_(std::move(nicName))
{
    ASCENDGDRBW_ASCEND_ASSERT(aclrtSetDevice(deviceId_));
    ASCENDGDRBW_HCCL_ASSERT(HcclDeviceRefresh(deviceLogicId_));
    ASCENDGDRBW_HCCL_ASSERT(InitExternalInput());
    ASCENDGDRBW_HCCL_ASSERT(InitEnvConfig());
    ASCENDGDRBW_HCCL_ASSERT(hrtGetDevicePhyIdByIndex(static_cast<uint32_t>(deviceLogicId_), devicePhyId_, false));
    ASCENDGDRBW_HCCL_ASSERT(HcclNetInit(NICDeployment::NIC_DEPLOYMENT_DEVICE,
                                        static_cast<int32_t>(devicePhyId_), deviceLogicId_, false, false));
    netInitialized_ = true;
    std::fprintf(stderr,
                 "[ascendgdrbw] HcclNetInit success: device=%d logic=%d phy=%u requested_nic=%s\n",
                 deviceId_, deviceLogicId_, devicePhyId_, nicName_.c_str());

    ASCENDGDRBW_ASSERT(config.cqDepth > 0);
    ASCENDGDRBW_ASSERT(config.qpSendWr > 0);
    ASCENDGDRBW_ASSERT(config.qpRecvWr > 0);

    std::fprintf(stderr,
                 "[ascendgdrbw] About to resolve device IP: device=%d logic=%d phy=%u requested_nic=%s\n",
                 deviceId_, deviceLogicId_, devicePhyId_, nicName_.c_str());
    try {
        hccl::HcclIpAddress bindIp = ResolveDeviceIp(devicePhyId_, deviceLogicId_);
        resolvedDeviceIp_ = bindIp.GetReadableAddress();
        std::fprintf(stderr, "[ascendgdrbw] Device IP resolved: phy=%u ip=%s\n",
                     devicePhyId_, resolvedDeviceIp_.c_str());
        context_ = OpenNicContextByDeviceIp(bindIp, resolvedIbvDeviceName_, gidIndex_);
    } catch (const std::exception& ex) {
        std::fprintf(stderr,
                     "[ascendgdrbw] failed to resolve/open device NIC by device id: reason=%s requested_nic=%s phy=%u logic=%d\n",
                     ex.what(), nicName_.c_str(), devicePhyId_, deviceLogicId_);
        throw;
    }
    ASCENDGDRBW_ASSERT(context_ != nullptr);
    ASCENDGDRBW_HCCL_ASSERT(HrtRaRdmaGetHandle(devicePhyId_, rdmaHandle_));
    ASCENDGDRBW_ASSERT(rdmaHandle_ != nullptr);
    std::fprintf(stderr,
                 "[ascendgdrbw] HrtRaRdmaGetHandle success: device=%d phy=%u rdmaHandle=%p\n",
                 deviceId_, devicePhyId_, rdmaHandle_);

    protectionDomain_ = ibv_alloc_pd(context_);
    ASCENDGDRBW_ASSERT(protectionDomain_ != nullptr);

    ibv_port_attr portAttr = {};
    ASCENDGDRBW_IBV_ASSERT(ibv_query_port(context_, kIbvPort, &portAttr));
    const bool isRoce = (portAttr.lid == 0);

    ibv_device_attr deviceAttr = {};
    ASCENDGDRBW_IBV_ASSERT(ibv_query_device(context_, &deviceAttr));

    ASCENDGDRBW_ASSERT(config.cqDepth <= static_cast<int>(deviceAttr.max_cqe));
    ASCENDGDRBW_ASSERT(config.qpSendWr <= static_cast<int>(deviceAttr.max_qp_wr));
    ASCENDGDRBW_ASSERT(config.qpRecvWr <= static_cast<int>(deviceAttr.max_qp_wr));
    ASCENDGDRBW_ASSERT(config.qpSendWr + config.qpRecvWr <= static_cast<int>(deviceAttr.max_qp_wr));

    const int cqDepth = config.cqDepth;
    completionQueue_ = ibv_create_cq(context_, cqDepth, nullptr, nullptr, 0);
    ASCENDGDRBW_ASSERT(completionQueue_ != nullptr);

    ibv_qp_init_attr qpInitAttr = {};
    qpInitAttr.send_cq = completionQueue_;
    qpInitAttr.recv_cq = completionQueue_;
    qpInitAttr.cap.max_send_wr = config.qpSendWr;
    qpInitAttr.cap.max_recv_wr = config.qpRecvWr;
    qpInitAttr.cap.max_send_sge = 1;
    qpInitAttr.cap.max_recv_sge = 1;
    qpInitAttr.qp_type = IBV_QPT_RC;
    qpInitAttr.sq_sig_all = 0;

    queuePair_ = ibv_create_qp(protectionDomain_, &qpInitAttr);
    ASCENDGDRBW_ASSERT(queuePair_ != nullptr);

    const auto cqWindow = static_cast<uint32_t>(std::max(1, cqDepth - 1));
    maxOutstandingWorkRequests_ = std::max<uint32_t>(
        1u, std::min<uint32_t>(qpInitAttr.cap.max_send_wr, cqWindow));

    std::fprintf(stderr,
                 "[ascendgdrbw] RDMAChannel init: requested_nic=%s resolved_ibv=%s device=%d logic=%d phy=%u ip=%s gid_index=%d\n",
                 nicName_.c_str(), resolvedIbvDeviceName_.c_str(), deviceId_, deviceLogicId_,
                 devicePhyId_, resolvedDeviceIp_.c_str(), gidIndex_);

    const QPEndpoint endpoint = QueryEndpoint(queuePair_, context_, gidIndex_);
    ConnectQueuePair(queuePair_, endpoint, isRoce, portAttr.active_mtu);
}

RDMAChannel::~RDMAChannel()
{
    if (queuePair_ != nullptr) { ibv_destroy_qp(queuePair_); }
    if (completionQueue_ != nullptr) { ibv_destroy_cq(completionQueue_); }
    if (protectionDomain_ != nullptr) { ibv_dealloc_pd(protectionDomain_); }
    if (context_ != nullptr) { ibv_close_device(context_); }
    if (netInitialized_) {
        (void)HcclNetDeInit(NICDeployment::NIC_DEPLOYMENT_DEVICE,
                            static_cast<int32_t>(devicePhyId_), deviceLogicId_, false);
        netInitialized_ = false;
    }
}

MemoryRegistration* RDMAChannel::RegisterHostMemory(void* buffer, size_t bytes)
{
    ASCENDGDRBW_ASSERT(buffer != nullptr);
    ASCENDGDRBW_ASSERT(bytes > 0);
    std::fprintf(stderr,
                 "[ascendgdrbw] RegisterHostMemory begin: backend=ibverbs nic=%s device=%d pd=%p buffer=%p "
                 "bytes=%zu flags=0x%x\n",
                 nicName_.c_str(), deviceId_, static_cast<void*>(protectionDomain_), buffer, bytes,
                 IBV_ACCESS_LOCAL_WRITE);
    ibv_mr* memoryRegion = ibv_reg_mr(protectionDomain_, buffer, bytes, IBV_ACCESS_LOCAL_WRITE);
    if (memoryRegion == nullptr) {
        std::fprintf(stderr,
                     "[ascendgdrbw] RegisterHostMemory failed: nic=%s device=%d pd=%p buffer=%p "
                     "bytes=%zu errno=%d(%s)\n",
                     nicName_.c_str(), deviceId_, static_cast<void*>(protectionDomain_), buffer,
                     bytes, errno, std::strerror(errno));
    } else {
        std::fprintf(stderr,
                     "[ascendgdrbw] RegisterHostMemory success: backend=ibverbs nic=%s device=%d mr=%p lkey=%u "
                     "rkey=%u\n",
                     nicName_.c_str(), deviceId_, static_cast<void*>(memoryRegion),
                     memoryRegion->lkey, memoryRegion->rkey);
    }
    ASCENDGDRBW_ERRNO_ASSERT(memoryRegion != nullptr);

    auto* registration = new MemoryRegistration();
    registration->backend = MemoryRegistration::Backend::Ibverbs;
    registration->ibvMemoryRegion = memoryRegion;
    registration->lkey = memoryRegion->lkey;
    registration->rkey = memoryRegion->rkey;
    registration->backendTag = "ibverbs_host";
    return registration;
}

MemoryRegistration* RDMAChannel::RegisterDeviceMemory(void* buffer, size_t bytes)
{
    ASCENDGDRBW_ASSERT(buffer != nullptr);
    ASCENDGDRBW_ASSERT(bytes > 0);
    ASCENDGDRBW_ASCEND_ASSERT(aclrtSetDevice(deviceId_));
    constexpr int kDirectIbvAccessFlags =
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    const bool disableDirectDeviceIbvReg = IsDirectDeviceIbvRegDisabledByEnv();
    if (disableDirectDeviceIbvReg) {
        std::fprintf(stderr,
                     "[ascendgdrbw] RegisterDeviceMemory skip direct path: direct_device_ibv_reg=disabled_by_env "
                     "requested_nic=%s resolved_ibv=%s device=%d logic=%d phy=%u gid_index=%d pd=%p "
                     "buffer=%p bytes=%zu access_flags=0x%x\n",
                     nicName_.c_str(), resolvedIbvDeviceName_.c_str(), deviceId_, deviceLogicId_,
                     devicePhyId_, gidIndex_, static_cast<void*>(protectionDomain_), buffer, bytes,
                     kDirectIbvAccessFlags);
    } else {
        std::fprintf(stderr,
                     "[ascendgdrbw] RegisterDeviceMemory begin: backend=ibverbs_direct_device "
                     "requested_nic=%s resolved_ibv=%s device=%d logic=%d phy=%u gid_index=%d pd=%p "
                     "buffer=%p bytes=%zu access_flags=0x%x\n",
                     nicName_.c_str(), resolvedIbvDeviceName_.c_str(), deviceId_, deviceLogicId_,
                     devicePhyId_, gidIndex_, static_cast<void*>(protectionDomain_), buffer, bytes,
                     kDirectIbvAccessFlags);
        ibv_mr* memoryRegion =
            ibv_reg_mr(protectionDomain_, buffer, bytes, kDirectIbvAccessFlags);
        if (memoryRegion != nullptr) {
            std::fprintf(stderr,
                         "[ascendgdrbw] RegisterDeviceMemory success: backend=ibverbs_direct_device "
                         "requested_nic=%s resolved_ibv=%s device=%d logic=%d phy=%u gid_index=%d "
                         "pd=%p mr=%p lkey=%u rkey=%u\n",
                         nicName_.c_str(), resolvedIbvDeviceName_.c_str(), deviceId_, deviceLogicId_,
                         devicePhyId_, gidIndex_, static_cast<void*>(protectionDomain_),
                         static_cast<void*>(memoryRegion), memoryRegion->lkey, memoryRegion->rkey);

            auto* registration = new MemoryRegistration();
            registration->backend = MemoryRegistration::Backend::Ibverbs;
            registration->ibvMemoryRegion = memoryRegion;
            registration->lkey = memoryRegion->lkey;
            registration->rkey = memoryRegion->rkey;
            registration->backendTag = "ibverbs_direct_device";
            return registration;
        }

        std::fprintf(stderr,
                     "[ascendgdrbw] RegisterDeviceMemory failed: backend=ibverbs_direct_device "
                     "requested_nic=%s resolved_ibv=%s device=%d logic=%d phy=%u gid_index=%d pd=%p "
                     "buffer=%p bytes=%zu access_flags=0x%x errno=%d(%s)\n",
                     nicName_.c_str(), resolvedIbvDeviceName_.c_str(), deviceId_, deviceLogicId_,
                     devicePhyId_, gidIndex_, static_cast<void*>(protectionDomain_), buffer, bytes,
                     kDirectIbvAccessFlags, errno, std::strerror(errno));
    }

    std::fprintf(stderr,
                 "[ascendgdrbw] RegisterDeviceMemory begin: backend=ra_global_mr_fallback "
                 "requested_nic=%s resolved_ibv=%s device=%d logic=%d phy=%u gid_index=%d "
                 "rdmaHandle=%p buffer=%p bytes=%zu access_flags=0x%x\n",
                 nicName_.c_str(), resolvedIbvDeviceName_.c_str(), deviceId_, deviceLogicId_,
                 devicePhyId_, gidIndex_, rdmaHandle_, buffer, bytes,
                 RA_ACCESS_LOCAL_WRITE | RA_ACCESS_REMOTE_WRITE | RA_ACCESS_REMOTE_READ);

    MrInfoT info = {};
    info.addr = buffer;
    info.size = static_cast<unsigned long long>(bytes);
    info.access = RA_ACCESS_LOCAL_WRITE | RA_ACCESS_REMOTE_WRITE | RA_ACCESS_REMOTE_READ;
    MrHandle mrHandle = nullptr;
    const HcclResult ret = hrtRaRegGlobalMr(rdmaHandle_, info, mrHandle);
    if (ret != HCCL_SUCCESS || mrHandle == nullptr) {
        std::fprintf(stderr,
                     "[ascendgdrbw] RegisterDeviceMemory failed: backend=ra_global_mr_fallback "
                     "requested_nic=%s resolved_ibv=%s device=%d logic=%d phy=%u gid_index=%d "
                     "buffer=%p bytes=%zu ret=%d mrHandle=%p\n",
                     nicName_.c_str(), resolvedIbvDeviceName_.c_str(), deviceId_, deviceLogicId_,
                     devicePhyId_, gidIndex_, buffer, bytes, static_cast<int>(ret), mrHandle);
    } else {
        std::fprintf(stderr,
                     "[ascendgdrbw] RegisterDeviceMemory success: backend=ra_global_mr_fallback "
                     "requested_nic=%s resolved_ibv=%s device=%d logic=%d phy=%u gid_index=%d "
                     "mrHandle=%p lkey=%u rkey=%u\n",
                     nicName_.c_str(), resolvedIbvDeviceName_.c_str(), deviceId_, deviceLogicId_,
                     devicePhyId_, gidIndex_, mrHandle, info.lkey, info.rkey);
    }
    ASCENDGDRBW_HCCL_ASSERT(ret);
    ASCENDGDRBW_ASSERT(mrHandle != nullptr);

    auto* registration = new MemoryRegistration();
    registration->backend = MemoryRegistration::Backend::RaGlobal;
    registration->mrHandle = mrHandle;
    registration->lkey = info.lkey;
    registration->rkey = info.rkey;
    registration->backendTag = "ra_global_mr_fallback";
    return registration;
}

void RDMAChannel::DeregisterMemory(MemoryRegistration* registration) noexcept
{
    if (registration == nullptr) {
        return;
    }

    if (registration->backend == MemoryRegistration::Backend::Ibverbs) {
        if (registration->ibvMemoryRegion != nullptr) {
            (void)ibv_dereg_mr(registration->ibvMemoryRegion);
        }
    } else {
        if (registration->mrHandle != nullptr && rdmaHandle_ != nullptr) {
            (void)hrtRaDeRegGlobalMr(rdmaHandle_, registration->mrHandle);
        }
    }

    delete registration;
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
    ASCENDGDRBW_IBV_ASSERT(ibv_post_send(queuePair_, &workRequest, &badWorkRequest));

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
        ASCENDGDRBW_ASSERT(pollResult >= 0);
        if (pollResult == 0) {
            std::this_thread::yield();
            continue;
        }
        break;
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
    std::lock_guard<std::mutex> lock(mutex_);
    ASCENDGDRBW_ASSERT(!initialized_);
    ASCENDGDRBW_ASSERT(deviceNumber > 0);
    ASCENDGDRBW_ASSERT(nicNames.size() == static_cast<size_t>(deviceNumber));

    channels_.clear();
    deviceIds_.clear();
    deviceIds_.reserve(static_cast<size_t>(deviceNumber));
    for (int32_t deviceId = 0; deviceId < deviceNumber; ++deviceId) {
        channels_.emplace(deviceId,
                          std::make_unique<RDMAChannel>(deviceId, nicNames[deviceId], config));
        deviceIds_.push_back(deviceId);
    }
    initialized_ = true;
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
    std::lock_guard<std::mutex> lock(mutex_);
    channels_.clear();
    deviceIds_.clear();
    initialized_ = false;
}
