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

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <infiniband/verbs.h>

#include "error_handle.h"
#include "adapter_hccp_common.h"
#include "externalinput_pub.h"
#include "hccl_ip_address.h"
#include "hccl_network_pub.h"

HcclResult InitEnvConfig();
HcclResult HcclDeviceRefresh(s32 &deviceLogicId);
extern "C" HcclResult hrtGetDevicePhyIdByIndex(u32 deviceLogicId, u32 &devicePhyId, bool isRefresh);

namespace {

constexpr int kIbvPort = 1;

struct Options {
    int32_t deviceId = 0;
    std::string nicHint = "mlx5_0";
    std::string targetIp;
};

void CheckHccl(HcclResult ret, const char* expr)
{
    if (ret != HCCL_SUCCESS) {
        AscendGdrbwThrowError(std::string(expr) + " failed, ret=" +
                              std::to_string(static_cast<int>(ret)));
    }
}

#define ASCENDGDRBW_HCCL_ASSERT(expr) CheckHccl((expr), #expr)

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg(argv[index]);
        if (arg == "--help" || arg == "-h") {
            std::fprintf(stderr, "Usage: %s [--device=N] [--nic=NAME] [--ip=A.B.C.D]\n", argv[0]);
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
        if (arg.find("--ip=") == 0U) {
            options.targetIp = arg.substr(std::strlen("--ip="));
            continue;
        }
        AscendGdrbwThrowError("unknown argument: " + arg);
    }
    return options;
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

void PrintGid(const union ibv_gid& gid)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&gid);
    for (size_t i = 0; i < 16; ++i) {
        std::fprintf(stderr, "%02x", bytes[i]);
        if (i + 1 != 16) {
            std::fprintf(stderr, ":");
        }
    }
}

hccl::HcclIpAddress ResolveDeviceIp(uint32_t devicePhyId)
{
    std::vector<hccl::HcclIpAddress> deviceIps;
    ASCENDGDRBW_HCCL_ASSERT(hrtRaGetDeviceIP(devicePhyId, deviceIps));
    std::fprintf(stderr, "[device-nic-probe] hrtRaGetDeviceIP success phy=%u candidates=%zu\n",
                 devicePhyId, deviceIps.size());
    for (size_t index = 0; index < deviceIps.size(); ++index) {
        std::fprintf(stderr, "[device-nic-probe]   candidate[%zu]=%s family=%d invalid=%d\n", index,
                     deviceIps[index].GetReadableAddress(), deviceIps[index].GetFamily(),
                     deviceIps[index].IsInvalid() ? 1 : 0);
    }
    for (const auto& ip : deviceIps) {
        if (!ip.IsInvalid() && !ip.IsIPv6()) {
            std::fprintf(stderr, "[device-nic-probe] selected IPv4 device_ip=%s\n",
                         ip.GetReadableAddress());
            return ip;
        }
    }
    for (const auto& ip : deviceIps) {
        if (!ip.IsInvalid()) {
            std::fprintf(stderr, "[device-nic-probe] selected fallback device_ip=%s\n",
                         ip.GetReadableAddress());
            return ip;
        }
    }
    AscendGdrbwThrowError("hrtRaGetDeviceIP returned no valid device IP");
    return hccl::HcclIpAddress();
}

void ProbeIbvDevices(const hccl::HcclIpAddress& targetIp, const std::string& nicHint)
{
    int deviceCount = 0;
    ibv_device** deviceList = ibv_get_device_list(&deviceCount);
    ASCENDGDRBW_ASSERT(deviceList != nullptr);

    std::fprintf(stderr,
                 "[device-nic-probe] ibv_get_device_list success count=%d target_ip=%s nic_hint=%s\n",
                 deviceCount, targetIp.GetReadableAddress(), nicHint.c_str());

    for (int index = 0; index < deviceCount; ++index) {
        const char* deviceName = ibv_get_device_name(deviceList[index]);
        std::fprintf(stderr, "[device-nic-probe] ibv_device[%d] name=%s\n", index, deviceName);

        ibv_context* context = ibv_open_device(deviceList[index]);
        if (context == nullptr) {
            std::fprintf(stderr, "[device-nic-probe]   open_device failed errno=%d(%s)\n", errno,
                         std::strerror(errno));
            continue;
        }

        ibv_port_attr portAttr = {};
        const int portRet = ibv_query_port(context, kIbvPort, &portAttr);
        if (portRet != 0) {
            std::fprintf(stderr,
                         "[device-nic-probe]   query_port failed ret=%d errno=%d(%s)\n",
                         portRet, errno, std::strerror(errno));
            (void)ibv_close_device(context);
            continue;
        }

        std::fprintf(stderr,
                     "[device-nic-probe]   open_device success lid=%u mtu=%d gid_tbl_len=%d roce=%d hint_match=%d\n",
                     portAttr.lid, static_cast<int>(portAttr.active_mtu), portAttr.gid_tbl_len,
                     portAttr.lid == 0 ? 1 : 0, nicHint == deviceName ? 1 : 0);

        bool matched = false;
        for (int gidIndex = 0; gidIndex < portAttr.gid_tbl_len; ++gidIndex) {
            union ibv_gid gid = {};
            const int gidRet = ibv_query_gid(context, kIbvPort, gidIndex, &gid);
            if (gidRet != 0) {
                std::fprintf(stderr,
                             "[device-nic-probe]   gid[%d] query failed ret=%d errno=%d(%s)\n",
                             gidIndex, gidRet, errno, std::strerror(errno));
                continue;
            }

            const bool ipMatched = CompareIpAndGid(targetIp, gid);
            std::fprintf(stderr, "[device-nic-probe]   gid[%d] match=%d value=", gidIndex,
                         ipMatched ? 1 : 0);
            PrintGid(gid);
            std::fprintf(stderr, "\n");

            if (ipMatched) {
                matched = true;
            }
        }

        std::fprintf(stderr, "[device-nic-probe]   summary device=%s matched=%d\n", deviceName,
                     matched ? 1 : 0);
        (void)ibv_close_device(context);
    }

    ibv_free_device_list(deviceList);
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

    try {
        options = ParseOptions(argc, argv);
        const bool hasTargetIp = !options.targetIp.empty();
        hccl::HcclIpAddress manualTargetIp;
        if (hasTargetIp) {
            ASCENDGDRBW_HCCL_ASSERT(manualTargetIp.SetReadableAddress(options.targetIp));
            ASCENDGDRBW_ASSERT(!manualTargetIp.IsInvalid());
            std::fprintf(stderr, "[device-nic-probe] manual_target_ip=%s family=%d\n",
                         manualTargetIp.GetReadableAddress(), manualTargetIp.GetFamily());
        }

        ASCENDGDRBW_ASCEND_ASSERT(aclInit(nullptr));
        aclInitialized = true;
        ASCENDGDRBW_ASCEND_ASSERT(aclrtSetDevice(options.deviceId));
        deviceSet = true;

        ASCENDGDRBW_HCCL_ASSERT(HcclDeviceRefresh(deviceLogicId));
        ASCENDGDRBW_HCCL_ASSERT(InitExternalInput());
        ASCENDGDRBW_HCCL_ASSERT(InitEnvConfig());
        ASCENDGDRBW_HCCL_ASSERT(
            hrtGetDevicePhyIdByIndex(static_cast<uint32_t>(deviceLogicId), devicePhyId, false));

        std::fprintf(stderr, "[device-nic-probe] device_context device=%d logic=%d phy=%u nic_hint=%s\n",
                     options.deviceId, deviceLogicId, devicePhyId, options.nicHint.c_str());

        ASCENDGDRBW_HCCL_ASSERT(HcclNetInit(NICDeployment::NIC_DEPLOYMENT_DEVICE,
                                            static_cast<int32_t>(devicePhyId), deviceLogicId, false,
                                            false));
        netInitialized = true;
        std::fprintf(stderr, "[device-nic-probe] HcclNetInit success\n");

        const hccl::HcclIpAddress deviceIp = hasTargetIp ? manualTargetIp : ResolveDeviceIp(devicePhyId);
        ProbeIbvDevices(deviceIp, options.nicHint);

        std::fprintf(stderr, "[device-nic-probe] probe_finished success\n");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "device_nic_probe_min failed: %s\n", ex.what());
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
