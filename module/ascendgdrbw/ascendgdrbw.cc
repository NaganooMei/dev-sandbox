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
#include "memcpy_case.h"

#include <acl/acl.h>

#include <cstdarg>
#include <cstdio>
#include <exception>
#include <memory>
#include <vector>

#include <fmt/format.h>

#include "error_handle.h"
#include "rdma_channel.h"
#include "submit_executor.h"

namespace {

void LogStep(const char* step, const char* state, const char* format = nullptr, ...)
{
    std::fprintf(stderr, "[ascendgdrbw] step=%s state=%s", step, state);
    if (format != nullptr) {
        std::fprintf(stderr, " ");
        va_list args;
        va_start(args, format);
        std::vfprintf(stderr, format, args);
        va_end(args);
    }
    std::fprintf(stderr, "\n");
}

}  // namespace

int main(int argc, char const* argv[])
{
    (void)argc;
    (void)argv;

    const char* currentStep = "startup";
    const char* currentCase = nullptr;
    bool aclInitialized = false;
    auto cleanup = [&aclInitialized]() noexcept {
        LogStep("cleanup", "begin");
        SubmitExecutor::Instance().Shutdown();
        ChannelManager::Instance().Shutdown();
        if (aclInitialized) {
            (void)aclFinalize();
            aclInitialized = false;
        }
        LogStep("cleanup", "success");
    };

    try {
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

        currentStep = "setup_parameters";
        LogStep(currentStep, "begin");
        auto& param = MemcpyParameterSet::Instance();
        param.deviceNumber = 8;
        param.bufferSize = 16 * 1024;
        param.bufferNumber = 512;
        param.iterations = 128;
        param.nicNames = {"mlx5_0", "mlx5_2", "mlx5_1", "mlx5_3",
                          "mlx5_4", "mlx5_6", "mlx5_5", "mlx5_7"};
        param.rdmaConfig.cqDepth = 1024;
        param.rdmaConfig.qpSendWr = 1024;
        param.rdmaConfig.qpRecvWr = 1024;
        LogStep(currentStep, "success", "device_number=%d buffer_size=%zu buffer_number=%zu iters=%zu",
                param.deviceNumber, param.bufferSize, param.bufferNumber, param.iterations);

        ASCENDGDRBW_ASSERT(param.nicNames.size() == static_cast<size_t>(param.deviceNumber));
        currentStep = "init_executor";
        LogStep(currentStep, "begin", "worker_count=%d", param.deviceNumber);
        SubmitExecutor::Instance().Initialize(static_cast<size_t>(param.deviceNumber));
        LogStep(currentStep, "success");

        currentStep = "init_channels";
        LogStep(currentStep, "begin", "device_number=%d", param.deviceNumber);
        ChannelManager::Instance().Initialize(param.deviceNumber, param.nicNames,
                                              param.rdmaConfig);
        LogStep(currentStep, "success");

        currentStep = "build_testcases";
        LogStep(currentStep, "begin");
        std::vector<std::unique_ptr<MemcpyCase>> testcases;
        testcases.emplace_back(std::make_unique<HostToDeviceMemcpyCase>());
        testcases.emplace_back(std::make_unique<HostToAllDeviceMemcpyCase>());
        testcases.emplace_back(std::make_unique<AllHostToAllDeviceMemcpyCase>());
        LogStep(currentStep, "success", "count=%zu", testcases.size());

        currentStep = "run_testcases";
        LogStep(currentStep, "begin", "count=%zu", testcases.size());
        for (const auto& test : testcases) {
            currentCase = test->Key().c_str();
            LogStep("run_testcase", "begin", "case=%s", currentCase);
            fmt::println("<--- {} --->", test->Key());
            test->Run();
            LogStep("run_testcase", "success", "case=%s", currentCase);
        }
        currentCase = nullptr;
        LogStep(currentStep, "success");

        cleanup();
        return 0;
    } catch (const std::exception& ex) {
        if (currentCase != nullptr) {
            LogStep(currentStep, "failed", "case=%s reason=%s", currentCase, ex.what());
        } else {
            LogStep(currentStep, "failed", "reason=%s", ex.what());
        }
        std::fprintf(stderr, "ascendgdrbw failed: %s\n", ex.what());
        cleanup();
        return 1;
    }
}
