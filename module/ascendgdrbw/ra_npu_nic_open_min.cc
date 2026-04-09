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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "error_handle.h"
#include "adapter_hccp.h"
#include "adapter_hccp_common.h"
#include "externalinput_pub.h"
#include "hccl_ip_address.h"
#include "hccl_network_pub.h"

HcclResult InitEnvConfig();
HcclResult HcclDeviceRefresh(s32 &deviceLogicId);
extern "C" HcclResult hrtGetDevicePhyIdByIndex(u32 deviceLogicId, u32 &devicePhyId, bool isRefresh);

namespace {

void CheckHccl(HcclResult ret, const char* expr)
{
    if (ret != HCCL_SUCCESS) {
        AscendGdrbwThrowError(std::string(expr) + " failed, ret=" +
                              std::to_string(static_cast<int>(ret)));
    }
}

#define RA_OPEN_ASSERT(expr) CheckHccl((expr), #expr)

struct Options {
    int32_t deviceId = 0;
    std::string targetIp;
    bool initSocket = false;
};

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg(argv[index]);
        if (arg == "--help" || arg == "-h") {
            std::fprintf(stderr, "Usage: %s [--device=N] [--ip=A.B.C.D] [--socket]\n", argv[0]);
            std::exit(0);
        }
        if (arg.find("--device=") == 0U) {
            options.deviceId = std::stoi(arg.substr(std::strlen("--device=")));
            continue;
        }
        if (arg.find("--ip=") == 0U) {
            options.targetIp = arg.substr(std::strlen("--ip="));
            continue;
        }
        if (arg == "--socket") {
            options.initSocket = true;
            continue;
        }
        AscendGdrbwThrowError("unknown argument: " + arg);
    }
    return options;
}

hccl::HcclIpAddress ResolveDeviceIp(uint32_t devicePhyId, const std::string& overrideIp)
{
    if (!overrideIp.empty()) {
        hccl::HcclIpAddress ip;
        RA_OPEN_ASSERT(ip.SetReadableAddress(overrideIp));
        ASCENDGDRBW_ASSERT(!ip.IsInvalid());
        return ip;
    }

    std::vector<hccl::HcclIpAddress> deviceIps;
    RA_OPEN_ASSERT(hrtRaGetDeviceIP(devicePhyId, deviceIps));
    std::fprintf(stderr, "[ra-npu-nic-open] hrtRaGetDeviceIP success phy=%u candidates=%zu\n",
                 devicePhyId, deviceIps.size());
    for (size_t i = 0; i < deviceIps.size(); ++i) {
        std::fprintf(stderr, "[ra-npu-nic-open]   candidate[%zu]=%s family=%d invalid=%d\n", i,
                     deviceIps[i].GetReadableAddress(), deviceIps[i].GetFamily(),
                     deviceIps[i].IsInvalid() ? 1 : 0);
    }
    for (const auto& ip : deviceIps) {
        if (!ip.IsInvalid() && !ip.IsIPv6()) {
            return ip;
        }
    }
    for (const auto& ip : deviceIps) {
        if (!ip.IsInvalid()) {
            return ip;
        }
    }
    AscendGdrbwThrowError("no valid device ip");
    return hccl::HcclIpAddress();
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    bool aclInitialized = false;
    bool deviceSet = false;
    bool netInitialized = false;
    int32_t deviceLogicId = -1;
    uint32_t devicePhyId = 0;
    RdmaHandle rdmaHandle = nullptr;
    SocketHandle socketHandle = nullptr;

    try {
        options = ParseOptions(argc, argv);

        RA_OPEN_ASSERT(InitExternalInput());
        RA_OPEN_ASSERT(InitEnvConfig());

        ASCENDGDRBW_ASCEND_ASSERT(aclInit(nullptr));
        aclInitialized = true;
        ASCENDGDRBW_ASCEND_ASSERT(aclrtSetDevice(options.deviceId));
        deviceSet = true;

        RA_OPEN_ASSERT(HcclDeviceRefresh(deviceLogicId));
        RA_OPEN_ASSERT(hrtGetDevicePhyIdByIndex(static_cast<u32>(deviceLogicId), devicePhyId, false));
        std::fprintf(stderr, "[ra-npu-nic-open] device_context device=%d logic=%d phy=%u\n",
                     options.deviceId, deviceLogicId, devicePhyId);

        RA_OPEN_ASSERT(HcclNetInit(NICDeployment::NIC_DEPLOYMENT_DEVICE,
                                   static_cast<int32_t>(devicePhyId), deviceLogicId, false, false));
        netInitialized = true;
        std::fprintf(stderr, "[ra-npu-nic-open] HcclNetInit success\n");

        const hccl::HcclIpAddress deviceIp = ResolveDeviceIp(devicePhyId, options.targetIp);
        std::fprintf(stderr, "[ra-npu-nic-open] use_device_ip=%s family=%d\n",
                     deviceIp.GetReadableAddress(), deviceIp.GetFamily());

        struct rdev rdevInfo {};
        rdevInfo.phyId = devicePhyId;
        rdevInfo.family = deviceIp.GetFamily();
        rdevInfo.localIp.addr = deviceIp.GetBinaryAddress().addr;
        rdevInfo.localIp.addr6 = deviceIp.GetBinaryAddress().addr6;

        struct RdevInitInfo initInfo = { DEFAULT_INIT_RDMA_CONFIG };
        initInfo.mode = NETWORK_OFFLINE;
        initInfo.notifyType = NOTIFY;
        initInfo.disabledLiteThread = false;
        initInfo.enabled910aLite = false;
        initInfo.enabled2mbLite = false;

        RA_OPEN_ASSERT(HrtRaRdmaInitWithAttr(initInfo, rdevInfo, rdmaHandle));
        ASCENDGDRBW_ASSERT(rdmaHandle != nullptr);
        std::fprintf(stderr, "[ra-npu-nic-open] HrtRaRdmaInitWithAttr success rdmaHandle=%p mode=%d notifyType=%u\n",
                     rdmaHandle, initInfo.mode, initInfo.notifyType);

        if (options.initSocket) {
            RA_OPEN_ASSERT(hrtRaSocketInit(NETWORK_OFFLINE, rdevInfo, socketHandle));
            ASCENDGDRBW_ASSERT(socketHandle != nullptr);
            std::fprintf(stderr, "[ra-npu-nic-open] hrtRaSocketInit success socketHandle=%p\n",
                         socketHandle);
        }

        std::fprintf(stderr, "[ra-npu-nic-open] probe_finished success\n");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "ra_npu_nic_open_min failed: %s\n", ex.what());
        if (socketHandle != nullptr) {
            (void)hrtRaSocketDeInit(socketHandle);
        }
        if (rdmaHandle != nullptr) {
            (void)HrtRaRdmaDeInit(rdmaHandle, NOTIFY);
        }
        if (netInitialized) {
            (void)HcclNetDeInit(NICDeployment::NIC_DEPLOYMENT_DEVICE,
                                static_cast<int32_t>(devicePhyId), deviceLogicId, false);
        }
        if (deviceSet) {
            (void)aclrtResetDevice(options.deviceId);
        }
        if (aclInitialized) {
            (void)aclFinalize();
        }
        return 1;
    }

    if (socketHandle != nullptr) {
        (void)hrtRaSocketDeInit(socketHandle);
    }
    if (rdmaHandle != nullptr) {
        (void)HrtRaRdmaDeInit(rdmaHandle, NOTIFY);
    }
    if (netInitialized) {
        (void)HcclNetDeInit(NICDeployment::NIC_DEPLOYMENT_DEVICE,
                            static_cast<int32_t>(devicePhyId), deviceLogicId, false);
    }
    if (deviceSet) {
        (void)aclrtResetDevice(options.deviceId);
    }
    if (aclInitialized) {
        (void)aclFinalize();
    }
    return 0;
}
