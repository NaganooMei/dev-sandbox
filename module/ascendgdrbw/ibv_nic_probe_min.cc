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

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <string>

#include <infiniband/verbs.h>

namespace {

constexpr int kIbvPort = 1;

struct Options {
    std::string nicHint = "mlx5_0";
};

[[noreturn]] void ThrowError(const std::string& message)
{
    throw std::runtime_error(message);
}

std::string BuildLocation(const char* expression, const char* file, int line, const char* function)
{
    std::stringstream stream;
    stream << "expression " << expression << " failed at " << function << " : " << file << ":" << line;
    return stream.str();
}

std::string BuildErrnoMessage(const char* expression, int errorCode, const char* file, int line,
                              const char* function)
{
    std::stringstream stream;
    stream << "[" << errorCode << "] " << std::strerror(errorCode) << " in "
           << BuildLocation(expression, file, line, function);
    return stream.str();
}

#define IBV_PROBE_ASSERT(expr)                                                             \
    do {                                                                                   \
        if (!(expr)) {                                                                     \
            ThrowError(BuildLocation(#expr, __FILE__, __LINE__, __PRETTY_FUNCTION__));    \
        }                                                                                  \
    } while (0)

Options ParseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg(argv[index]);
        if (arg == "--help" || arg == "-h") {
            std::fprintf(stderr, "Usage: %s [--nic=NAME]\n", argv[0]);
            std::exit(0);
        }
        if (arg.find("--nic=") == 0U) {
            options.nicHint = arg.substr(std::strlen("--nic="));
            continue;
        }
        ThrowError("unknown argument: " + arg);
    }
    return options;
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

}  // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = ParseOptions(argc, argv);

        int deviceCount = 0;
        ibv_device** deviceList = ibv_get_device_list(&deviceCount);
        IBV_PROBE_ASSERT(deviceList != nullptr);
        std::fprintf(stderr, "[ibv-nic-probe] ibv_get_device_list success count=%d nic_hint=%s\n",
                     deviceCount, options.nicHint.c_str());

        for (int index = 0; index < deviceCount; ++index) {
            const char* deviceName = ibv_get_device_name(deviceList[index]);
            std::fprintf(stderr, "[ibv-nic-probe] device[%d] name=%s hint_match=%d\n", index,
                         deviceName, options.nicHint == deviceName ? 1 : 0);

            ibv_context* context = ibv_open_device(deviceList[index]);
            if (context == nullptr) {
                std::fprintf(stderr, "[ibv-nic-probe]   open_device failed errno=%d(%s)\n", errno,
                             std::strerror(errno));
                continue;
            }

            ibv_port_attr portAttr = {};
            const int portRet = ibv_query_port(context, kIbvPort, &portAttr);
            if (portRet != 0) {
                std::fprintf(stderr,
                             "[ibv-nic-probe]   query_port failed ret=%d errno=%d(%s)\n",
                             portRet, errno, std::strerror(errno));
                (void)ibv_close_device(context);
                continue;
            }

            std::fprintf(stderr,
                         "[ibv-nic-probe]   open_device success lid=%u mtu=%d gid_tbl_len=%d roce=%d\n",
                         portAttr.lid, static_cast<int>(portAttr.active_mtu), portAttr.gid_tbl_len,
                         portAttr.lid == 0 ? 1 : 0);

            for (int gidIndex = 0; gidIndex < portAttr.gid_tbl_len; ++gidIndex) {
                union ibv_gid gid = {};
                const int gidRet = ibv_query_gid(context, kIbvPort, gidIndex, &gid);
                if (gidRet != 0) {
                    std::fprintf(stderr,
                                 "[ibv-nic-probe]   gid[%d] query failed ret=%d errno=%d(%s)\n",
                                 gidIndex, gidRet, errno, std::strerror(errno));
                    continue;
                }

                std::fprintf(stderr, "[ibv-nic-probe]   gid[%d] value=", gidIndex);
                PrintGid(gid);
                std::fprintf(stderr, "\n");
            }

            (void)ibv_close_device(context);
        }

        ibv_free_device_list(deviceList);
        std::fprintf(stderr, "[ibv-nic-probe] probe_finished success\n");
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "ibv_nic_probe_min failed: %s\n", ex.what());
        return 1;
    }
}
