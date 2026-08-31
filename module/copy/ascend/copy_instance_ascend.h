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
#ifndef COPY_INSTANCE_ASCEND_H
#define COPY_INSTANCE_ASCEND_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
#include "copy_buffer.h"
#include "copy_instance.h"
#include "copy_options.h"
#include "error_handle_ascend.h"

struct AscendStreamContext {
    size_t deviceId = 0;
    aclrtStream stream = nullptr;
    aclrtEvent endEvent = nullptr;
    size_t size = 0;
    std::vector<void*> src;
    std::vector<void*> dst;
};

class AscendCopyInstanceBase : public CopyInstance {
protected:
    std::vector<AscendStreamContext> contexts_;
    aclrtEvent totalStart_;
    aclrtEvent totalEnd_;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        contexts_.clear();
        const auto bufferNumber = srcBuffers.size();
        for (size_t i = 0; i < bufferNumber; i++) {
            auto& src = *srcBuffers[i];
            auto& dst = *dstBuffers[i];
            ASSERT(src.Number() == dst.Number());
            ASSERT(src.Size() == dst.Size());

            AscendStreamContext ctx;
            ctx.deviceId = AffinityDeviceId(src, dst);
            ctx.size = src.Size();
            ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
            ASCEND_ASSERT(aclrtCreateStream(&ctx.stream));
            ASCEND_ASSERT(aclrtCreateEvent(&ctx.endEvent));
            ctx.src.reserve(src.Number());
            ctx.dst.reserve(dst.Number());
            for (size_t j = 0; j < src.Number(); j++) {
                ctx.src.push_back(src[j]);
                ctx.dst.push_back(dst[j]);
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
            ASCEND_ASSERT(aclrtDestroyEvent(ctx.endEvent));
            ASCEND_ASSERT(aclrtDestroyStream(ctx.stream));
        }
        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        ASCEND_ASSERT(aclrtDestroyEvent(totalStart_));
        ASCEND_ASSERT(aclrtDestroyEvent(totalEnd_));
        contexts_.clear();
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        using namespace std::chrono;

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        ASCEND_ASSERT(aclrtRecordEvent(totalStart_, contexts_[0].stream));

        for (size_t i = 1; i < contexts_.size(); i++) {
            ASCEND_ASSERT(aclrtSetDevice(contexts_[i].deviceId));
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[i].stream, totalStart_));
        }

        auto submitStart = steady_clock::now();
        for (auto& ctx : contexts_) {
            ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
            CopyInternal(ctx);
        }
        auto submitCost = duration_cast<microseconds>(steady_clock::now() - submitStart).count();

        for (size_t i = 1; i < contexts_.size(); i++) {
            ASCEND_ASSERT(aclrtSetDevice(contexts_[i].deviceId));
            ASCEND_ASSERT(aclrtRecordEvent(contexts_[i].endEvent, contexts_[i].stream));
            ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[0].stream, contexts_[i].endEvent));
        }

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        ASCEND_ASSERT(aclrtRecordEvent(totalEnd_, contexts_[0].stream));
        SynchronizeInternal(contexts_[0]);

        float copyCostMs = 0.f;
        ASCEND_ASSERT(aclrtEventElapsedTime(&copyCostMs, totalStart_, totalEnd_));
        size_t copyCost = static_cast<size_t>(copyCostMs * 1000);

        return {copyCost, submitCost};
    }

    virtual void CopyInternal(const AscendStreamContext& ctx) = 0;
    virtual void SynchronizeInternal(const AscendStreamContext& ctx) = 0;

public:
    AscendCopyInstanceBase(size_t iterations, bool affinitySrc)
        : CopyInstance(iterations, affinitySrc)
    {
    }
};

class H2DCECopyInstance : public AscendCopyInstanceBase {
protected:
    void CopyInternal(const AscendStreamContext& ctx) override
    {
        for (size_t i = 0; i < ctx.src.size(); i++) {
            ASCEND_ASSERT(aclrtMemcpyAsync(ctx.dst[i], ctx.size, ctx.src[i], ctx.size,
                                           ACL_MEMCPY_HOST_TO_DEVICE, ctx.stream));
        }
    }

    void SynchronizeInternal(const AscendStreamContext& ctx) override
    {
        ASCEND_ASSERT(aclrtSynchronizeStream(ctx.stream));
    }

public:
    H2DCECopyInstance(size_t iterations, bool affinitySrc)
        : AscendCopyInstanceBase(iterations, affinitySrc)
    {
    }

    std::string Name() const override { return "CE"; }
};

class H2DCEParallelSubmitCopyInstance : public H2DCECopyInstance {
protected:
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

    std::vector<std::unique_ptr<SubmitWorker>> submitWorkers_;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        AscendCopyInstanceBase::Prepare(srcBuffers, dstBuffers);
        StartSubmitWorkers(contexts_.size());
    }

    void Cleanup() override
    {
        StopSubmitWorkers();
        AscendCopyInstanceBase::Cleanup();
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        using namespace std::chrono;

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        ASCEND_ASSERT(aclrtRecordEvent(totalStart_, contexts_[0].stream));

        for (size_t i = 1; i < contexts_.size(); i++) {
            ASCEND_ASSERT(aclrtSetDevice(contexts_[i].deviceId));
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[i].stream, totalStart_));
        }

        auto submitStart = steady_clock::now();
        SubmitContexts();
        auto submitCost = duration_cast<microseconds>(steady_clock::now() - submitStart).count();

        for (size_t i = 1; i < contexts_.size(); i++) {
            ASCEND_ASSERT(aclrtSetDevice(contexts_[i].deviceId));
            ASCEND_ASSERT(aclrtRecordEvent(contexts_[i].endEvent, contexts_[i].stream));
            ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[0].stream, contexts_[i].endEvent));
        }

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        ASCEND_ASSERT(aclrtRecordEvent(totalEnd_, contexts_[0].stream));
        SynchronizeInternal(contexts_[0]);

        float copyCostMs = 0.f;
        ASCEND_ASSERT(aclrtEventElapsedTime(&copyCostMs, totalStart_, totalEnd_));
        size_t copyCost = static_cast<size_t>(copyCostMs * 1000);

        return {copyCost, submitCost};
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
            ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
            CopyInternal(contexts_[0]);
            return;
        }

        ASSERT(submitWorkers_.size() == contexts_.size());
        for (size_t index = 0; index < contexts_.size(); ++index) {
            auto* worker = submitWorkers_[index].get();
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                ASSERT(!worker->hasTask);
                worker->task = [this, index]() {
                    ASCEND_ASSERT(aclrtSetDevice(contexts_[index].deviceId));
                    CopyInternal(contexts_[index]);
                };
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

public:
    H2DCEParallelSubmitCopyInstance(size_t iterations, bool affinitySrc)
        : H2DCECopyInstance(iterations, affinitySrc)
    {
    }

    ~H2DCEParallelSubmitCopyInstance() override { StopSubmitWorkers(); }

    std::string Name() const override { return "CE-MT"; }
};

class H2DBatchCECopyInstance : public AscendCopyInstanceBase {
protected:
    size_t targetDevice_;

    void CopyInternal(const AscendStreamContext& ctx) override
    {
        aclrtMemcpyBatchAttr attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.srcLoc.type = ACL_MEM_LOCATION_TYPE_HOST;
        attr.dstLoc.type = ACL_MEM_LOCATION_TYPE_DEVICE;
        attr.dstLoc.id = targetDevice_;

        std::vector<aclrtMemcpyBatchAttr> attrArray{attr};
        std::vector<size_t> attrIdxArray(ctx.src.size(), 0);
        std::vector<size_t> sizeArray(ctx.src.size(), ctx.size);
        size_t failureIdx = 0;

        ASCEND_ASSERT(aclrtMemcpyBatchAsync(const_cast<void**>(ctx.dst.data()), sizeArray.data(),
                                            const_cast<void**>(ctx.src.data()), sizeArray.data(),
                                            ctx.src.size(), attrArray.data(), attrIdxArray.data(),
                                            attrArray.size(), &failureIdx, ctx.stream));
    }

    void SynchronizeInternal(const AscendStreamContext& ctx) override
    {
        ASCEND_ASSERT(aclrtSynchronizeStream(ctx.stream));
    }

public:
    H2DBatchCECopyInstance(size_t iterations, bool affinitySrc, size_t targetDevice)
        : AscendCopyInstanceBase(iterations, affinitySrc), targetDevice_(targetDevice)
    {
    }

    std::string Name() const override { return "BatchCE"; }
};

class D2DCECopyInstance : public AscendCopyInstanceBase {
protected:
    void CopyInternal(const AscendStreamContext& ctx) override
    {
        for (size_t i = 0; i < ctx.src.size(); i++) {
            ASCEND_ASSERT(aclrtMemcpyAsync(ctx.dst[i], ctx.size, ctx.src[i], ctx.size,
                                           ACL_MEMCPY_DEVICE_TO_DEVICE, ctx.stream));
        }
    }

    void SynchronizeInternal(const AscendStreamContext& ctx) override
    {
        ASCEND_ASSERT(aclrtSynchronizeStream(ctx.stream));
    }

public:
    D2DCECopyInstance(size_t iterations, bool affinitySrc)
        : AscendCopyInstanceBase(iterations, affinitySrc)
    {
    }

    std::string Name() const override { return "CE"; }
};

class H2DCEMultiStreamCopyInstance : public CopyInstance {
protected:
    struct CopySpec {
        void* dst = nullptr;
        void* src = nullptr;
        size_t size = 0;
    };

    struct CopyTask {
        size_t contextIndex = 0;
        std::vector<CopySpec> copies;
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

    std::vector<AscendStreamContext> contexts_;
    std::vector<std::vector<size_t>> contextGroups_;
    std::vector<std::vector<size_t>> contextTasks_;
    std::vector<CopyTask> tasks_;
    std::vector<std::vector<size_t>> taskGroups_;
    std::vector<std::unique_ptr<SubmitWorker>> submitWorkers_;
    aclrtEvent totalStart_ = nullptr;
    aclrtEvent totalEnd_ = nullptr;
    size_t streamCount_;
    CopyIoMode ioMode_;
    CopySubmitMode submitMode_;
    bool streamStartGate_;
    CopyStreamSyncMode streamSyncMode_;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        contexts_.clear();
        contextGroups_.clear();
        contextTasks_.clear();
        tasks_.clear();
        taskGroups_.clear();
        ASSERT(!srcBuffers.empty());
        ASSERT(srcBuffers.size() == dstBuffers.size());
        ASSERT(streamCount_ > 0);
        const auto bufferNumber = srcBuffers.size();
        contexts_.reserve(bufferNumber * streamCount_);
        contextTasks_.reserve(bufferNumber * streamCount_);
        contextGroups_.reserve(bufferNumber);
        taskGroups_.reserve(bufferNumber);

        for (size_t i = 0; i < bufferNumber; i++) {
            auto& src = *srcBuffers[i];
            auto& dst = *dstBuffers[i];
            ASSERT(src.Number() == dst.Number());
            ASSERT(src.Size() == dst.Size());
            ASSERT(src.Number() > 0);
            if (ioMode_ == CopyIoMode::GLM51) { ASSERT(src.Size() == kGlm51BlockBytes); }

            const size_t taskCount = src.Number();
            const size_t activeStreamCount = std::min(streamCount_, taskCount);
            const size_t deviceId = AffinityDeviceId(src, dst);
            ASCEND_ASSERT(aclrtSetDevice(deviceId));

            std::vector<size_t> group;
            group.reserve(activeStreamCount);
            for (size_t stream = 0; stream < activeStreamCount; ++stream) {
                AscendStreamContext ctx;
                ctx.deviceId = deviceId;
                ctx.size = src.Size();
                ASCEND_ASSERT(aclrtCreateStream(&ctx.stream));
                if (streamSyncMode_ == CopyStreamSyncMode::EVENT) {
                    ASCEND_ASSERT(aclrtCreateEvent(&ctx.endEvent));
                }
                group.push_back(contexts_.size());
                contexts_.push_back(std::move(ctx));
                contextTasks_.emplace_back();
            }

            std::vector<size_t> groupTasks;
            groupTasks.reserve(taskCount);
            for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
                const size_t stream = CopyTaskStreamIndex(submitMode_, taskIndex, taskCount,
                                                          activeStreamCount);

                CopyTask task;
                task.contextIndex = group[stream];
                if (ioMode_ == CopyIoMode::GLM51) {
                    task.copies.reserve(kGlm51IoCount);
                    auto* srcBase = static_cast<char*>(src[taskIndex]);
                    auto* dstBase = static_cast<char*>(dst[taskIndex]);
                    for (size_t io = 0; io < kGlm51IoCount; ++io) {
                        task.copies.push_back(
                            {dstBase + kGlm51IoOffsets[io], srcBase + kGlm51IoOffsets[io],
                             kGlm51IoSizes[io]});
                    }
                } else {
                    task.copies.push_back({dst[taskIndex], src[taskIndex], src.Size()});
                }

                const size_t globalTaskIndex = tasks_.size();
                tasks_.push_back(std::move(task));
                contextTasks_[group[stream]].push_back(globalTaskIndex);
                groupTasks.push_back(globalTaskIndex);
            }
            contextGroups_.push_back(std::move(group));
            taskGroups_.push_back(std::move(groupTasks));
        }

        ASSERT(!contexts_.empty());
        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        if (streamSyncMode_ == CopyStreamSyncMode::EVENT || streamStartGate_) {
            ASCEND_ASSERT(aclrtCreateEvent(&totalStart_));
        }
        if (streamSyncMode_ == CopyStreamSyncMode::EVENT) {
            ASCEND_ASSERT(aclrtCreateEvent(&totalEnd_));
        }
        StartSubmitWorkers(contextGroups_.size());
    }

    void Cleanup() override
    {
        StopSubmitWorkers();
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
        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        if (totalStart_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyEvent(totalStart_));
            totalStart_ = nullptr;
        }
        if (totalEnd_ != nullptr) {
            ASCEND_ASSERT(aclrtDestroyEvent(totalEnd_));
            totalEnd_ = nullptr;
        }
        contexts_.clear();
        contextGroups_.clear();
        contextTasks_.clear();
        tasks_.clear();
        taskGroups_.clear();
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        using namespace std::chrono;

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        if (streamSyncMode_ == CopyStreamSyncMode::EVENT || streamStartGate_) {
            ASCEND_ASSERT(aclrtRecordEvent(totalStart_, contexts_[0].stream));
        }
        if (streamStartGate_) { ArmStartDependencies(); }

        auto submitStart = steady_clock::now();
        SubmitGroups();
        auto submitCost = duration_cast<microseconds>(steady_clock::now() - submitStart).count();

        if (streamSyncMode_ == CopyStreamSyncMode::STREAM) {
            SynchronizeAllStreams();
            return {0, submitCost};
        }

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        for (size_t i = 1; i < contexts_.size(); i++) {
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[0].stream, contexts_[i].endEvent));
        }
        ASCEND_ASSERT(aclrtRecordEvent(totalEnd_, contexts_[0].stream));
        ASCEND_ASSERT(aclrtSynchronizeStream(contexts_[0].stream));

        float copyCostMs = 0.f;
        ASCEND_ASSERT(aclrtEventElapsedTime(&copyCostMs, totalStart_, totalEnd_));
        size_t copyCost = static_cast<size_t>(copyCostMs * 1000);

        return {copyCost, submitCost};
    }

    bool HasDeviceCopyTiming() const override
    {
        return streamSyncMode_ == CopyStreamSyncMode::EVENT;
    }

    void SynchronizeAllStreams()
    {
        for (auto& ctx : contexts_) {
            ASCEND_ASSERT(aclrtSetDevice(ctx.deviceId));
            ASCEND_ASSERT(aclrtSynchronizeStream(ctx.stream));
        }
    }

    void ArmStartDependencies()
    {
        for (const auto& group : contextGroups_) {
            if (group.empty()) { continue; }
            ASCEND_ASSERT(aclrtSetDevice(contexts_[group[0]].deviceId));
            for (const auto contextIndex : group) {
                if (contextIndex != 0) {
                    auto& ctx = contexts_[contextIndex];
                    ASCEND_ASSERT(aclrtStreamWaitEvent(ctx.stream, totalStart_));
                }
            }
        }
    }

    void StartSubmitWorkers(size_t workerCount)
    {
        StopSubmitWorkers();
        if (workerCount <= 1) { return; }

        submitWorkers_.reserve(workerCount);
        for (size_t index = 0; index < workerCount; index++) {
            auto worker = std::make_unique<SubmitWorker>();
            auto* workerPtr = worker.get();
            worker->thread = std::thread([workerPtr]() { SubmitWorkerLoop(workerPtr); });
            submitWorkers_.push_back(std::move(worker));
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

    void SubmitGroups()
    {
        if (contextGroups_.size() == 1) {
            SubmitGroup(0);
            return;
        }

        ASSERT(submitWorkers_.size() == contextGroups_.size());
        for (size_t index = 0; index < contextGroups_.size(); index++) {
            auto* worker = submitWorkers_[index].get();
            {
                std::lock_guard<std::mutex> lock(worker->mutex);
                ASSERT(!worker->hasTask);
                worker->task = [this, index]() { SubmitGroup(index); };
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

    void SubmitTask(const CopyTask& task)
    {
        auto& ctx = contexts_[task.contextIndex];
        for (const auto& copy : task.copies) {
            ASCEND_ASSERT(aclrtMemcpyAsync(copy.dst, copy.size, copy.src, copy.size,
                                           ACL_MEMCPY_HOST_TO_DEVICE, ctx.stream));
        }
    }

    void SubmitGroup(size_t groupIndex)
    {
        const auto& group = contextGroups_[groupIndex];
        if (group.empty()) { return; }

        ASCEND_ASSERT(aclrtSetDevice(contexts_[group[0]].deviceId));
        if (submitMode_ == CopySubmitMode::ROUND_ROBIN) {
            for (const auto taskIndex : taskGroups_[groupIndex]) { SubmitTask(tasks_[taskIndex]); }
        } else {
            for (const auto contextIndex : group) {
                for (const auto taskIndex : contextTasks_[contextIndex]) {
                    SubmitTask(tasks_[taskIndex]);
                }
            }
        }

        if (streamSyncMode_ == CopyStreamSyncMode::EVENT) {
            for (const auto contextIndex : group) {
                auto& ctx = contexts_[contextIndex];
                ASCEND_ASSERT(aclrtRecordEvent(ctx.endEvent, ctx.stream));
            }
        }
    }

public:
    H2DCEMultiStreamCopyInstance(
        size_t iterations, bool affinitySrc, size_t streamCount,
        CopyIoMode ioMode = CopyIoMode::UNIFORM,
        CopySubmitMode submitMode = CopySubmitMode::STREAM_MAJOR,
        bool streamStartGate = true,
        CopyStreamSyncMode streamSyncMode = CopyStreamSyncMode::EVENT)
        : CopyInstance(iterations, affinitySrc),
          streamCount_(streamCount),
          ioMode_(ioMode),
          submitMode_(submitMode),
          streamStartGate_(streamStartGate),
          streamSyncMode_(streamSyncMode)
    {
    }

    std::string Name() const override
    {
        return "CE-MS" + std::to_string(streamCount_) + CopySubmitModeSuffix(submitMode_) +
               CopyStreamStartGateSuffix(streamStartGate_) +
               CopyStreamSyncModeSuffix(streamSyncMode_) +
               (ioMode_ == CopyIoMode::GLM51 ? "-GLM51" : "");
    }
};

#endif  // COPY_INSTANCE_ASCEND_H
