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

#include <cstdio>
#include <exception>
#include <memory>
#include <vector>

#include <fmt/format.h>

#include "rdma_channel.h"
#include "submit_executor.h"

int main(int argc, char const* argv[])
{
    (void)argc;
    (void)argv;

    try {
        auto& param = MemcpyParameterSet::Instance();
        param.deviceNumber = 8;
        param.bufferSize = 16 * 1024;
        param.bufferNumber = 512;
        param.iterations = 128;
        // GPUs分别对应的网卡
        param.nicNames = {"mlx5_0", "mlx5_2", "mlx5_1", "mlx5_3", "mlx5_4", "mlx5_6", "mlx5_5", "mlx5_7"};
        param.rdmaConfig.cqDepth = 1024;
        param.rdmaConfig.qpSendWr = 1024;
        param.rdmaConfig.qpRecvWr = 1024;

        GDRBW_ASSERT(param.nicNames.size() == static_cast<size_t>(param.deviceNumber));
        SubmitExecutor::Instance().Initialize(static_cast<size_t>(param.deviceNumber));
        ChannelManager::Instance().Initialize(param.deviceNumber, param.nicNames,
                                              param.rdmaConfig);

        std::vector<std::unique_ptr<MemcpyCase>> testcases;
        testcases.emplace_back(std::make_unique<HostToDeviceMemcpyCase>());
        testcases.emplace_back(std::make_unique<HostToAllDeviceMemcpyCase>());
        testcases.emplace_back(std::make_unique<AllHostToAllDeviceMemcpyCase>());

        for (const auto& test : testcases) {
            fmt::println("<--- {} --->", test->Key());
            test->Run();
        }

        SubmitExecutor::Instance().Shutdown();
        ChannelManager::Instance().Shutdown();
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "gdrbw failed: %s\n", ex.what());
        SubmitExecutor::Instance().Shutdown();
        ChannelManager::Instance().Shutdown();
        return 1;
    }
}
