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
#ifndef ACLBW_MEMCPY_INSTANCE_H
#define ACLBW_MEMCPY_INSTANCE_H

#include <memory>
#include <vector>
#include "memcpy_initiator.h"
#include "memcpy_result.h"

class MemcpyInstance {
    struct MemcpyIo {
        void* src;
        void* dst;
        size_t size;
    };
    struct MemcpyStreamContext {
        int32_t deviceId;
        aclrtStream stream;
        aclrtEvent endEvent;
        std::vector<MemcpyIo> ioArray;
    };

    size_t iterations_;
    size_t warmup_;
    size_t streamNumber_;
    MemcpyInitiator* memcpyInitiator_;

    static std::vector<MemcpyStreamContext> Dispatch(const MemoryBuffer& srcBuffer,
                                                     const MemoryBuffer& dstBuffer,
                                                     size_t streamNumber)
    {
        std::vector<MemcpyStreamContext> contexts(streamNumber);
        for (size_t i = 0; i < streamNumber; i++) {
            auto& context = contexts[i];
            context.deviceId = srcBuffer.DeviceId();
            ACLBW_ASCEND_ASSERT(aclrtSetDevice(context.deviceId));
            ACLBW_ASCEND_ASSERT(aclrtCreateStreamWithConfig(
                &context.stream, 0, ACL_STREAM_FAST_LAUNCH | ACL_STREAM_FAST_SYNC));
            ACLBW_ASCEND_ASSERT(aclrtCreateEvent(&context.endEvent));
        }
        const auto ioNumber = srcBuffer.Number();
        const auto size = srcBuffer.Size();
        for (size_t i = 0; i < ioNumber; i++) {
            contexts[i % streamNumber].ioArray.push_back({srcBuffer[i], dstBuffer[i], size});
        }
        return contexts;
    }
    static std::vector<MemcpyStreamContext> Dispatch(
        const std::vector<const MemoryBuffer*>& srcBuffers,
        const std::vector<const MemoryBuffer*>& dstBuffers, size_t streamNumberPerBuffer)
    {
        const auto bufferNumber = srcBuffers.size();
        const auto streamNumber = bufferNumber * streamNumberPerBuffer;
        std::vector<MemcpyStreamContext> contexts;
        contexts.reserve(streamNumber);
        for (size_t i = 0; i < bufferNumber; i++) {
            auto bufferContexts = Dispatch(*srcBuffers[i], *dstBuffers[i], streamNumberPerBuffer);
            contexts.insert(contexts.end(), std::make_move_iterator(bufferContexts.begin()),
                            std::make_move_iterator(bufferContexts.end()));
        }
        return contexts;
    }
    static double ExecuteMemcpy(const MemcpyInitiator& initiator,
                                const std::vector<MemcpyStreamContext>& contexts)
    {
        const auto number = contexts.size();
        aclrtEvent totalStart = nullptr;
        aclrtEvent totalEnd = nullptr;
        ACLBW_ASCEND_ASSERT(aclrtSetDevice(contexts[0].deviceId));
        ACLBW_ASCEND_ASSERT(aclrtCreateEvent(&totalStart));
        ACLBW_ASCEND_ASSERT(aclrtCreateEvent(&totalEnd));
        try {
            ACLBW_ASCEND_ASSERT(aclrtRecordEvent(totalStart, contexts[0].stream));
            for (size_t i = 0; i < number; i++) {
                ACLBW_ASCEND_ASSERT(aclrtSetDevice(contexts[i].deviceId));
                if (i != 0) {
                    ACLBW_ASCEND_ASSERT(aclrtStreamWaitEvent(contexts[i].stream, totalStart));
                }
                for (const auto& io : contexts[i].ioArray) {
                    initiator.Copy(io.src, io.dst, io.size, contexts[i].stream);
                }
                initiator.FinalizeIteration(contexts[i].stream);
                if (i != 0) {
                    ACLBW_ASCEND_ASSERT(aclrtRecordEvent(contexts[i].endEvent, contexts[i].stream));
                    ACLBW_ASCEND_ASSERT(aclrtSetDevice(contexts[0].deviceId));
                    ACLBW_ASCEND_ASSERT(aclrtStreamWaitEvent(contexts[0].stream, contexts[i].endEvent));
                }
            }
            ACLBW_ASCEND_ASSERT(aclrtSetDevice(contexts[0].deviceId));
            ACLBW_ASCEND_ASSERT(aclrtRecordEvent(totalEnd, contexts[0].stream));
            ACLBW_ASCEND_ASSERT(aclrtSynchronizeEvent(totalEnd));
            float cost = 0.f;
            ACLBW_ASCEND_ASSERT(aclrtEventElapsedTime(&cost, totalStart, totalEnd));
            ACLBW_ASCEND_ASSERT(aclrtDestroyEvent(totalStart));
            ACLBW_ASCEND_ASSERT(aclrtDestroyEvent(totalEnd));
            return cost;
        } catch (...) {
            ACLBW_ASCEND_ASSERT(aclrtSetDevice(contexts[0].deviceId));
            if (totalStart != nullptr) { ACLBW_ASCEND_ASSERT(aclrtDestroyEvent(totalStart)); }
            if (totalEnd != nullptr) { ACLBW_ASCEND_ASSERT(aclrtDestroyEvent(totalEnd)); }
            throw;
        }
    }
    static void Cleanup(std::vector<MemcpyStreamContext>& contexts)
    {
        for (auto& context : contexts) {
            ACLBW_ASCEND_ASSERT(aclrtSetDevice(context.deviceId));
            ACLBW_ASCEND_ASSERT(aclrtDestroyEvent(context.endEvent));
            ACLBW_ASCEND_ASSERT(aclrtDestroyStream(context.stream));
        }
    }

public:
    MemcpyInstance(size_t iterations, size_t warmup, size_t streamNumber,
                   MemcpyInitiator* memcpyInitiator)
        : iterations_(iterations),
          warmup_(warmup),
          streamNumber_(streamNumber),
          memcpyInitiator_(memcpyInitiator)
    {
    }
    MemcpyResult::Result DoMemcpy(const std::vector<const MemoryBuffer*>& srcBuffers,
                                  const std::vector<const MemoryBuffer*>& dstBuffers)
    {
        ACLBW_ASSERT(!srcBuffers.empty());
        ACLBW_ASSERT(srcBuffers.size() == dstBuffers.size());
        auto contexts = Dispatch(srcBuffers, dstBuffers, streamNumber_);
        try {
            std::vector<double> durations;
            durations.reserve(iterations_);
            for (size_t i = 0; i < iterations_ + warmup_; i++) {
                auto cost = ExecuteMemcpy(*memcpyInitiator_, contexts);
                if (i >= warmup_) { durations.push_back(cost); }
            }
            Cleanup(contexts);
            return {srcBuffers.front()->ReadMe(), dstBuffers.front()->ReadMe(),
                    srcBuffers.front()->Size(), srcBuffers.front()->Number() * srcBuffers.size(),
                    std::move(durations)};
        } catch (...) {
            Cleanup(contexts);
            throw;
        }
    }
    MemcpyResult::Result DoMemcpy(const MemoryBuffer& srcBuffer, const MemoryBuffer& dstBuffer)
    {
        return DoMemcpy({&srcBuffer}, {&dstBuffer});
    }
};

#endif  // ACLBW_MEMCPY_INSTANCE_H
