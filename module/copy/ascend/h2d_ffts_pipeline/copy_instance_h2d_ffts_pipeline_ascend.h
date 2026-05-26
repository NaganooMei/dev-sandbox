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
#ifndef COPY_INSTANCE_H2D_FFTS_PIPELINE_ASCEND_H
#define COPY_INSTANCE_H2D_FFTS_PIPELINE_ASCEND_H

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "ascend/copy_buffer_ascend.h"
#include "copy_instance.h"
#include "ascend/error_handle_ascend.h"
#include "ffts_d2d_dispatcher_ascend.h"

class H2DFFTSPipelineCopyInstance : public CopyInstance {
protected:
    struct PipelineObjectRange {
        size_t firstFragment;
        size_t fragmentCount;
        size_t bytes;
    };

    static constexpr size_t kPipelineDepth = 2;

    struct PipelineContext {
        const CopyBuffer* src = nullptr;
        const CopyBuffer* dst = nullptr;
        size_t deviceId = 0;
        size_t size = 0;
        size_t number = 0;
        size_t objectFrags = 1;
        size_t maxObjectBytes = 0;
        aclrtStream h2dStream = nullptr;
        aclrtStream fftsStream = nullptr;
        aclrtEvent endEvent = nullptr;
        std::array<aclrtEvent, kPipelineDepth> slotReady{};
        std::array<aclrtEvent, kPipelineDepth> slotFree{};
        std::array<std::unique_ptr<DeviceCopyBuffer>, kPipelineDepth> transferBuffers;
        std::vector<PipelineObjectRange> objects;
        std::vector<AscendFftsCopySpec> objectCopies;
        std::vector<FftsD2DDispatcher> objectDispatchers;
    };

    struct SubmitWorker {
        std::mutex mutex;
        std::condition_variable ready;
        std::condition_variable finished;
        std::function<void()> task;
        std::exception_ptr error;
        bool hasTask = false;
        bool done = true;
        bool stop = false;
        std::thread thread;
    };

    size_t configuredObjectFrags_ = 1;
    aclrtEvent totalStart_ = nullptr;
    aclrtEvent totalEnd_ = nullptr;
    std::vector<PipelineContext> contexts_;
    std::vector<std::unique_ptr<SubmitWorker>> submitWorkers_;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        ASSERT(!srcBuffers.empty());
        ASSERT(srcBuffers.size() == dstBuffers.size());

        contexts_.clear();
        contexts_.reserve(srcBuffers.size());
        for (size_t bufferIndex = 0; bufferIndex < srcBuffers.size(); ++bufferIndex) {
            const auto* src = srcBuffers[bufferIndex];
            const auto* dst = dstBuffers[bufferIndex];
            ASSERT(src != nullptr);
            ASSERT(dst != nullptr);
            ASSERT(src->Number() == dst->Number());
            ASSERT(src->Size() == dst->Size());

            PipelineContext ctx;
            ctx.src = src;
            ctx.dst = dst;
            ctx.deviceId = AffinityDeviceId(*src, *dst);
            ctx.size = src->Size();
            ctx.number = src->Number();
            ASSERT(ctx.number > 0);
            ASSERT(ctx.size <= std::numeric_limits<size_t>::max() / ctx.number);

            ctx.objectFrags = std::min(configuredObjectFrags_, ctx.number);
            ASSERT(ctx.objectFrags > 0);
            ASSERT(ctx.size <= std::numeric_limits<size_t>::max() / ctx.objectFrags);
            ctx.maxObjectBytes = ctx.size * ctx.objectFrags;

            BuildObjectRanges(ctx);
            ctx.objectCopies.reserve(ctx.objectFrags);
            ctx.objectDispatchers.resize(ctx.objects.size());

            ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
            ASCEND_ASSERT(aclrtCreateStream(&ctx.h2dStream));
            ASCEND_ASSERT(aclrtCreateStream(&ctx.fftsStream));
            ASCEND_ASSERT(aclrtCreateEvent(&ctx.endEvent));

            for (size_t slot = 0; slot < kPipelineDepth; ++slot) {
                ctx.transferBuffers[slot] =
                    std::make_unique<DeviceCopyBuffer>(ctx.deviceId, ctx.size, ctx.objectFrags);
                ASCEND_ASSERT(aclrtCreateEvent(&ctx.slotReady[slot]));
                ASCEND_ASSERT(aclrtCreateEvent(&ctx.slotFree[slot]));
                ASCEND_ASSERT(aclrtRecordEvent(ctx.slotFree[slot], ctx.h2dStream));
            }
            contexts_.push_back(std::move(ctx));
        }

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        ASCEND_ASSERT(aclrtCreateEvent(&totalStart_));
        ASCEND_ASSERT(aclrtCreateEvent(&totalEnd_));
        StartSubmitWorkers(contexts_.size());
    }

    void Cleanup() override
    {
        StopSubmitWorkers();
        for (auto& ctx : contexts_) {
            ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
            for (auto& event : ctx.slotReady) {
                if (event != nullptr) {
                    ASCEND_ASSERT(aclrtDestroyEvent(event));
                    event = nullptr;
                }
            }
            for (auto& event : ctx.slotFree) {
                if (event != nullptr) {
                    ASCEND_ASSERT(aclrtDestroyEvent(event));
                    event = nullptr;
                }
            }
            if (ctx.endEvent != nullptr) {
                ASCEND_ASSERT(aclrtDestroyEvent(ctx.endEvent));
                ctx.endEvent = nullptr;
            }
            if (ctx.h2dStream != nullptr) {
                ASCEND_ASSERT(aclrtDestroyStream(ctx.h2dStream));
                ctx.h2dStream = nullptr;
            }
            if (ctx.fftsStream != nullptr) {
                ASCEND_ASSERT(aclrtDestroyStream(ctx.fftsStream));
                ctx.fftsStream = nullptr;
            }

            for (auto& buffer : ctx.transferBuffers) { buffer.reset(); }
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
        ASCEND_ASSERT(aclrtRecordEvent(totalStart_, contexts_[0].h2dStream));
        ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[0].fftsStream, totalStart_));
        for (size_t i = 1; i < contexts_.size(); ++i) {
            auto& ctx = contexts_[i];
            ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
            ASCEND_ASSERT(aclrtStreamWaitEvent(ctx.h2dStream, totalStart_));
            ASCEND_ASSERT(aclrtStreamWaitEvent(ctx.fftsStream, totalStart_));
        }

        const auto submitStart = steady_clock::now();
        SubmitContexts();
        const auto submitCost =
            static_cast<size_t>(duration_cast<microseconds>(steady_clock::now() - submitStart)
                                    .count());

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        for (auto& ctx : contexts_) {
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[0].h2dStream, ctx.endEvent));
        }
        ASCEND_ASSERT(aclrtRecordEvent(totalEnd_, contexts_[0].h2dStream));
        ASCEND_ASSERT(aclrtSynchronizeStream(contexts_[0].h2dStream));

        float copyCostMs = 0.0f;
        ASCEND_ASSERT(aclrtEventElapsedTime(&copyCostMs, totalStart_, totalEnd_));
        const auto copyCost = static_cast<size_t>(copyCostMs * 1000);
        return {copyCost, submitCost};
    }

    static void BuildObjectRanges(PipelineContext& ctx)
    {
        ctx.objects.clear();
        for (size_t first = 0; first < ctx.number; first += ctx.objectFrags) {
            const auto fragmentCount = std::min(ctx.objectFrags, ctx.number - first);
            ctx.objects.push_back({first, fragmentCount, fragmentCount * ctx.size});
        }
    }

    void StartSubmitWorkers(size_t workerCount)
    {
        StopSubmitWorkers();
        if (workerCount <= 1) { return; }

        submitWorkers_.reserve(workerCount);
        for (size_t index = 0; index < workerCount; ++index) {
            auto worker = std::make_unique<SubmitWorker>();
            auto* workerPtr = worker.get();
            worker->thread = std::thread([workerPtr]() { SubmitWorkerLoop(workerPtr); });
            submitWorkers_.emplace_back(std::move(worker));
        }
    }

    void StopSubmitWorkers() noexcept
    {
        for (auto& worker : submitWorkers_) {
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                worker->stop = true;
            }
            worker->ready.notify_one();
        }

        for (auto& worker : submitWorkers_) {
            if (worker->thread.joinable()) { worker->thread.join(); }
        }
        submitWorkers_.clear();
    }

    static void SubmitWorkerLoop(SubmitWorker* worker)
    {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(worker->mutex);
                worker->ready.wait(lock, [worker]() { return worker->hasTask || worker->stop; });
                if (worker->stop && !worker->hasTask) { return; }
                task = std::move(worker->task);
                worker->hasTask = false;
            }

            std::exception_ptr error;
            try {
                task();
            } catch (...) {
                error = std::current_exception();
            }

            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                worker->error = error;
                worker->done = true;
            }
            worker->finished.notify_one();
        }
    }

    void SubmitContexts()
    {
        if (contexts_.size() == 1) {
            SubmitContext(contexts_[0]);
            return;
        }

        ASSERT(submitWorkers_.size() == contexts_.size());
        for (size_t index = 0; index < contexts_.size(); ++index) {
            auto* worker = submitWorkers_[index].get();
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                ASSERT(!worker->hasTask);
                worker->task = [this, index]() { SubmitContext(contexts_[index]); };
                worker->error = nullptr;
                worker->done = false;
                worker->hasTask = true;
            }
            worker->ready.notify_one();
        }

        std::exception_ptr error;
        for (auto& worker : submitWorkers_) {
            auto* workerPtr = worker.get();
            std::unique_lock<std::mutex> lock(worker->mutex);
            worker->finished.wait(lock, [workerPtr]() { return workerPtr->done; });
            if (error == nullptr && worker->error != nullptr) { error = worker->error; }
        }

        if (error != nullptr) { std::rethrow_exception(error); }
    }

    void SubmitContext(PipelineContext& ctx)
    {
        ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
        for (size_t objectIndex = 0; objectIndex < ctx.objects.size(); ++objectIndex) {
            SubmitObject(ctx, objectIndex, ctx.objects[objectIndex]);
        }
        for (size_t slot = 0; slot < kPipelineDepth; ++slot) {
            ASCEND_ASSERT(aclrtStreamWaitEvent(ctx.h2dStream, ctx.slotFree[slot]));
        }
        ASCEND_ASSERT(aclrtRecordEvent(ctx.endEvent, ctx.h2dStream));
    }

    void SubmitObject(PipelineContext& ctx, size_t objectIndex, const PipelineObjectRange& object)
    {
        const size_t slot = objectIndex % kPipelineDepth;
        auto& transfer = *ctx.transferBuffers[slot];
        auto* transferBase = transfer[0];

        ASCEND_ASSERT(aclrtStreamWaitEvent(ctx.h2dStream, ctx.slotFree[slot]));
        ASCEND_ASSERT(aclrtMemcpyAsync(transferBase, ctx.maxObjectBytes,
                                       (*ctx.src)[object.firstFragment], object.bytes,
                                       ACL_MEMCPY_HOST_TO_DEVICE, ctx.h2dStream));
        ASCEND_ASSERT(aclrtRecordEvent(ctx.slotReady[slot], ctx.h2dStream));

        ASCEND_ASSERT(aclrtStreamWaitEvent(ctx.fftsStream, ctx.slotReady[slot]));
        BuildObjectCopies(ctx, slot, object);

        auto& dispatcher = ctx.objectDispatchers[objectIndex];
        const auto readyCount = dispatcher.BuildCopies(ctx.objectCopies);
        ASSERT(readyCount > 0);
        dispatcher.Launch(ctx.fftsStream, readyCount);

        ASCEND_ASSERT(aclrtRecordEvent(ctx.slotFree[slot], ctx.fftsStream));
    }

    static void BuildObjectCopies(PipelineContext& ctx, size_t slot,
                                  const PipelineObjectRange& object)
    {
        auto& transfer = *ctx.transferBuffers[slot];
        ctx.objectCopies.clear();
        ctx.objectCopies.reserve(object.fragmentCount);
        for (size_t i = 0; i < object.fragmentCount; ++i) {
            ctx.objectCopies.push_back(
                {(*ctx.dst)[object.firstFragment + i], transfer[i], ctx.size});
        }
    }

public:
    H2DFFTSPipelineCopyInstance(size_t iterations, bool affinitySrc, size_t objectFrags)
        : CopyInstance(iterations, affinitySrc),
          configuredObjectFrags_(objectFrags == 0 ? 1 : objectFrags)
    {
    }

    std::string Name() const override { return "h2d_ffts_pipeline"; }
};

#endif  // COPY_INSTANCE_H2D_FFTS_PIPELINE_ASCEND_H
