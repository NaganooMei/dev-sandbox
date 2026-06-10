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
#ifndef COPY_INSTANCE_FFTS_DIRECT_H2D_ASCEND_H
#define COPY_INSTANCE_FFTS_DIRECT_H2D_ASCEND_H

#include <chrono>
#include <string>
#include <utility>
#include <vector>
#include "ascend/error_handle_ascend.h"
#include "copy_instance.h"
#include "ffts_d2d_dispatcher_ascend.h"
#include "mapped_host_buffer_ffts_direct_h2d_ascend.h"

class FftsDirectH2DCopyInstance : public CopyInstance {
protected:
    struct DirectContext {
        size_t deviceId = 0;
        aclrtStream stream = nullptr;
        aclrtEvent endEvent = nullptr;
        std::vector<AscendFftsCopySpec> copies;
        FftsD2DDispatcher dispatcher;
    };

    std::vector<DirectContext> contexts_;
    aclrtEvent totalStart_ = nullptr;
    aclrtEvent totalEnd_ = nullptr;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        ASSERT(!srcBuffers.empty());
        ASSERT(srcBuffers.size() == dstBuffers.size());

        contexts_.clear();
        contexts_.reserve(srcBuffers.size());
        for (size_t i = 0; i < srcBuffers.size(); ++i) {
            const auto* src = srcBuffers[i];
            const auto* dst = dstBuffers[i];
            ASSERT(src != nullptr);
            ASSERT(dst != nullptr);
            ASSERT(src->Number() == dst->Number());
            ASSERT(src->Size() == dst->Size());

            const auto* mappedSrc = dynamic_cast<const FftsDirectMappedHostBuffer*>(src);
            ASSERT(mappedSrc != nullptr);

            DirectContext ctx;
            ctx.deviceId = AffinityDeviceId(*src, *dst);
            const auto size = src->Size();
            const auto number = src->Number();
            ASSERT(number > 0);

            ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
            ASCEND_ASSERT(aclrtCreateStream(&ctx.stream));
            ASCEND_ASSERT(aclrtCreateEvent(&ctx.endEvent));

            ctx.copies.reserve(number);
            for (size_t fragment = 0; fragment < number; ++fragment) {
                ctx.copies.push_back({(*dst)[fragment], mappedSrc->MappedAt(fragment), size});
            }
            contexts_.push_back(std::move(ctx));
        }

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        ASCEND_ASSERT(aclrtCreateEvent(&totalStart_));
        ASCEND_ASSERT(aclrtCreateEvent(&totalEnd_));
    }

    void Cleanup() override
    {
        for (auto& ctx : contexts_) {
            ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
            if (ctx.endEvent != nullptr) {
                ASCEND_ASSERT(aclrtDestroyEvent(ctx.endEvent));
                ctx.endEvent = nullptr;
            }
            if (ctx.stream != nullptr) {
                ASCEND_ASSERT(aclrtDestroyStream(ctx.stream));
                ctx.stream = nullptr;
            }
        }
        if (!contexts_.empty()) { ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId)); }
        if (totalStart_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyEvent(totalStart_));
            totalStart_ = nullptr;
        }
        if (totalEnd_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyEvent(totalEnd_));
            totalEnd_ = nullptr;
        }
        contexts_.clear();
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        using namespace std::chrono;

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        ASCEND_ASSERT(aclrtRecordEvent(totalStart_, contexts_[0].stream));
        for (size_t i = 1; i < contexts_.size(); ++i) {
            ASCEND_ASSERT(aclrtSetDevice(contexts_[i].deviceId));
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[i].stream, totalStart_));
        }

        const auto submitStart = steady_clock::now();
        for (auto& ctx : contexts_) { SubmitContext(ctx); }
        const auto submitCost =
            static_cast<size_t>(duration_cast<microseconds>(steady_clock::now() - submitStart)
                                    .count());

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        for (size_t i = 1; i < contexts_.size(); ++i) {
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[0].stream, contexts_[i].endEvent));
        }
        ASCEND_ASSERT(aclrtRecordEvent(totalEnd_, contexts_[0].stream));
        ASCEND_ASSERT(aclrtSynchronizeStream(contexts_[0].stream));

        float copyCostMs = 0.0f;
        ASCEND_ASSERT(aclrtEventElapsedTime(&copyCostMs, totalStart_, totalEnd_));
        const auto copyCost = static_cast<size_t>(copyCostMs * 1000);
        return {copyCost, submitCost};
    }

    static void SubmitContext(DirectContext& ctx)
    {
        ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
        const auto readyCount = ctx.dispatcher.BuildCopies(ctx.copies);
        ASSERT(readyCount > 0);
        ctx.dispatcher.Launch(ctx.stream, readyCount);
        ASCEND_ASSERT(aclrtRecordEvent(ctx.endEvent, ctx.stream));
    }

public:
    FftsDirectH2DCopyInstance(size_t iterations, bool affinitySrc)
        : CopyInstance(iterations, affinitySrc)
    {
    }

    std::string Name() const override { return "ffts-direct-h2d"; }
};

#endif  // COPY_INSTANCE_FFTS_DIRECT_H2D_ASCEND_H
