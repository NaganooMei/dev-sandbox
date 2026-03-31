/**
 * MIT License
 *
 * Copyright (c) 2026 Mag1c.H
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
#ifndef NVBW_MEMCPY_INSTANCE_H
#define NVBW_MEMCPY_INSTANCE_H

#include <algorithm>
#include <memory>
#include <numeric>
#include <vector>
#include "memcpy_initiator.h"
#include "memcpy_result.h"

class MemcpyInstance {
    size_t iterations_;
    size_t warmup_;
    MemcpyInitiator *memcpyInitiator_;

public:
    MemcpyInstance(size_t iterations, size_t warmup, MemcpyInitiator *memcpyInitiator)
        : iterations_(iterations), warmup_(warmup), memcpyInitiator_(memcpyInitiator)
    {
    }
    MemcpyResult::Result DoMemcpy(const std::vector<const MemoryBuffer *> &srcBuffers,
                                  const std::vector<const MemoryBuffer *> &dstBuffers)
    {
        NVBW_ASSERT(!srcBuffers.empty());
        NVBW_ASSERT(srcBuffers.size() == dstBuffers.size());
        std::vector<int32_t> contextDevices(srcBuffers.size());
        for (size_t i = 0; i < srcBuffers.size(); ++i) {
            contextDevices[i] = memcpyInitiator_->ContextDevice(*srcBuffers[i], *dstBuffers[i]);
        }

        std::vector<cudaStream_t> streams(srcBuffers.size());
        cudaEvent_t totalStart, totalEnd;
        std::vector<cudaEvent_t> endEvents(srcBuffers.size());
        /* allocate the per simulaneous copy resources */
        NVBW_CUDA_ASSERT(cudaSetDevice(contextDevices[0]));
        NVBW_CUDA_ASSERT(cudaEventCreate(&totalStart, cudaEventDefault));
        NVBW_CUDA_ASSERT(cudaEventCreate(&totalEnd, cudaEventDefault));
        for (size_t i = 0; i < srcBuffers.size(); i++) {
            NVBW_CUDA_ASSERT(cudaSetDevice(contextDevices[i]));
            NVBW_CUDA_ASSERT(cudaStreamCreateWithFlags(&streams[i], cudaStreamNonBlocking));
            NVBW_CUDA_ASSERT(cudaEventCreate(&endEvents[i], cudaEventDefault));
        }
        /* warmup then iteration */
        std::vector<double> durations;
        durations.reserve(iterations_);
        for (size_t loop = 0; loop < warmup_ + iterations_; loop++) {
            /* ensure that all copies are launched at the same time */
            NVBW_CUDA_ASSERT(cudaSetDevice(contextDevices[0]));
            NVBW_CUDA_ASSERT(cudaEventRecord(totalStart, streams[0]));
            for (size_t i = 1; i < srcBuffers.size(); i++) {
                NVBW_CUDA_ASSERT(cudaSetDevice(contextDevices[i]));
                NVBW_CUDA_ASSERT(cudaStreamWaitEvent(streams[i], totalStart));
            }
            /* submit copy task */
            for (size_t i = 0; i < srcBuffers.size(); i++) {
                NVBW_CUDA_ASSERT(cudaSetDevice(contextDevices[i]));
                memcpyInitiator_->Copy(*srcBuffers[i], *dstBuffers[i], streams[i]);
                NVBW_CUDA_ASSERT(cudaEventRecord(endEvents[i], streams[i]));
                if (i != 0) {
                    NVBW_CUDA_ASSERT(cudaSetDevice(contextDevices[0]));
                    NVBW_CUDA_ASSERT(cudaStreamWaitEvent(streams[0], endEvents[i]));
                }
            }
            NVBW_CUDA_ASSERT(cudaSetDevice(contextDevices[0]));
            NVBW_CUDA_ASSERT(cudaEventRecord(totalEnd, streams[0]));
            NVBW_CUDA_ASSERT(cudaEventSynchronize(totalEnd));
            if (loop < warmup_) { continue; }
            /* get total cost */
            float cost = 0.f;
            NVBW_CUDA_ASSERT(cudaEventElapsedTime(&cost, totalStart, totalEnd));
            durations.push_back(cost);
        }
        /* release the per simulaneous copy resources */
        for (size_t i = 0; i < srcBuffers.size(); i++) {
            NVBW_CUDA_ASSERT(cudaSetDevice(contextDevices[i]));
            NVBW_CUDA_ASSERT(cudaEventDestroy(endEvents[i]));
            NVBW_CUDA_ASSERT(cudaStreamDestroy(streams[i]));
        }
        NVBW_CUDA_ASSERT(cudaSetDevice(contextDevices[0]));
        NVBW_CUDA_ASSERT(cudaEventDestroy(totalStart));
        NVBW_CUDA_ASSERT(cudaEventDestroy(totalEnd));
        /* organize the results then return */
        MemcpyResult::Result result;
        result.src = srcBuffers.front()->ReadMe();
        result.dst = dstBuffers.front()->ReadMe();
        result.elemSize = srcBuffers.front()->Size();
        result.elemCount = srcBuffers.front()->Number();
        std::sort(durations.begin(), durations.end());
        result.minMs = durations.front();
        result.maxMs = durations.back();
        result.avgMs = std::accumulate(durations.begin(), durations.end(), 0.f) / durations.size();
        result.p50Ms = durations[durations.size() / 2];
        result.p90Ms = durations[durations.size() * 9 / 10];
        result.p99Ms = durations[durations.size() * 99 / 100];
        return result;
    }
    MemcpyResult::Result DoMemcpy(const MemoryBuffer &srcBuffer, const MemoryBuffer &dstBuffer)
    {
        return DoMemcpy({&srcBuffer}, {&dstBuffer});
    }
};

#endif  // NVBW_MEMCPY_INSTANCE_H
