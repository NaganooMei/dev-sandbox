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
#ifndef GDRBW_MEMCPY_INSTANCE_H
#define GDRBW_MEMCPY_INSTANCE_H

#include <algorithm>
#include <chrono>
#include <numeric>
#include <unordered_map>
#include <vector>

#include "memcpy_initiator.h"
#include "memcpy_result.h"
#include "submit_executor.h"

class MemcpyInstance {
    using Clock = std::chrono::steady_clock;

    size_t iterations_;
    size_t warmup_;
    MemcpyInitiator* memcpyInitiator_;

public:
    MemcpyInstance(size_t iterations, size_t warmup, MemcpyInitiator* memcpyInitiator)
        : iterations_(iterations), warmup_(warmup), memcpyInitiator_(memcpyInitiator)
    {
    }

    MemcpyResult::Result DoMemcpy(const std::vector<const MemoryBuffer*>& srcBuffers,
                                  const std::vector<const MemoryBuffer*>& dstBuffers)
    {
        GDRBW_ASSERT(!srcBuffers.empty());
        GDRBW_ASSERT(srcBuffers.size() == dstBuffers.size());

        std::vector<double> durations;
        durations.reserve(iterations_);
        for (size_t loop = 0; loop < warmup_ + iterations_; ++loop) {
            const auto start = Clock::now();
            std::unordered_map<int32_t, uint64_t> waitTargets;
            if (srcBuffers.size() <= 1) {
                for (size_t i = 0; i < srcBuffers.size(); ++i) {
                    const auto pending = memcpyInitiator_->Submit(*srcBuffers[i], *dstBuffers[i]);
                    if (pending.lastWorkRequestId == 0) { continue; }
                    auto& target = waitTargets[pending.deviceId];
                    target = std::max(target, pending.lastWorkRequestId);
                }
            } else {
                std::vector<PendingTransfer> pendings(srcBuffers.size());
                SubmitExecutor::Instance().Run(srcBuffers.size(), [&](size_t index) {
                    pendings[index] =
                        memcpyInitiator_->Submit(*srcBuffers[index], *dstBuffers[index]);
                });
                for (const auto& pending : pendings) {
                    if (pending.lastWorkRequestId == 0) { continue; }
                    auto& target = waitTargets[pending.deviceId];
                    target = std::max(target, pending.lastWorkRequestId);
                }
            }
            for (const auto& waitTarget : waitTargets) {
                ChannelManager::Instance().Get(waitTarget.first).Wait(waitTarget.second);
            }
            const auto end = Clock::now();
            if (loop < warmup_) { continue; }
            durations.push_back(
                std::chrono::duration<double, std::milli>(end - start).count());
        }

        MemcpyResult::Result result;
        result.src = srcBuffers.front()->ReadMe();
        result.dst = dstBuffers.front()->ReadMe();
        result.elemSize = srcBuffers.front()->Size();
        result.elemCount = srcBuffers.front()->Number();
        std::sort(durations.begin(), durations.end());
        result.minMs = durations.front();
        result.maxMs = durations.back();
        result.avgMs = std::accumulate(durations.begin(), durations.end(), 0.0) / durations.size();
        result.p50Ms = durations[durations.size() / 2];
        result.p90Ms = durations[durations.size() * 9 / 10];
        result.p99Ms = durations[durations.size() * 99 / 100];
        return result;
    }

    MemcpyResult::Result DoMemcpy(const MemoryBuffer& srcBuffer, const MemoryBuffer& dstBuffer)
    {
        return DoMemcpy({&srcBuffer}, {&dstBuffer});
    }
};

#endif  // GDRBW_MEMCPY_INSTANCE_H
