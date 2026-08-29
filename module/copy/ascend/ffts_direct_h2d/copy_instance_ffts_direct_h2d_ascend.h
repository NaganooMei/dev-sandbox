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

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "ascend/error_handle_ascend.h"
#include "copy_instance.h"
#include "copy_options.h"
#include "ffts_d2d_dispatcher_ascend.h"
#include "mapped_host_buffer_ffts_direct_h2d_ascend.h"

class FftsDirectH2DCopyInstance : public CopyInstance {
protected:
    struct DirectTask {
        size_t contextIndex = 0;
        std::vector<AscendFftsCopySpec> specs;
    };

    struct InFlightTask {
        std::vector<AscendFftsCopySpec> specs;
        FftsD2DDispatcher dispatcher;
    };

    struct DirectContext {
        size_t deviceId = 0;
        aclrtStream stream = nullptr;
        aclrtEvent endEvent = nullptr;
        std::vector<size_t> taskIndices;
    };

    std::vector<DirectContext> contexts_;
    std::vector<std::vector<size_t>> contextGroups_;
    std::vector<DirectTask> tasks_;
    std::vector<std::vector<size_t>> taskGroups_;
    std::vector<std::unique_ptr<InFlightTask>> inFlight_;
    aclrtEvent totalStart_ = nullptr;
    aclrtEvent totalEnd_ = nullptr;
    size_t fragsPerTask_ = 0;
    size_t streamCount_ = 1;
    CopyIoMode ioMode_ = CopyIoMode::UNIFORM;
    CopySubmitMode submitMode_ = CopySubmitMode::STREAM_MAJOR;

    void Prepare(const std::vector<const CopyBuffer*>& srcBuffers,
                 const std::vector<const CopyBuffer*>& dstBuffers) override
    {
        ASSERT(!srcBuffers.empty());
        ASSERT(srcBuffers.size() == dstBuffers.size());
        ASSERT(streamCount_ > 0);

        contexts_.clear();
        contextGroups_.clear();
        tasks_.clear();
        taskGroups_.clear();
        inFlight_.clear();
        contexts_.reserve(srcBuffers.size() * streamCount_);
        contextGroups_.reserve(srcBuffers.size());
        taskGroups_.reserve(srcBuffers.size());
        for (size_t i = 0; i < srcBuffers.size(); ++i) {
            const auto* src = srcBuffers[i];
            const auto* dst = dstBuffers[i];
            ASSERT(src != nullptr);
            ASSERT(dst != nullptr);
            ASSERT(src->Number() == dst->Number());
            ASSERT(src->Size() == dst->Size());

            const auto* mappedSrc = dynamic_cast<const FftsDirectMappedHostBuffer*>(src);
            ASSERT(mappedSrc != nullptr);

            const auto deviceId = AffinityDeviceId(*src, *dst);
            const auto size = src->Size();
            const auto number = src->Number();
            ASSERT(number > 0);
            if (ioMode_ == CopyIoMode::GLM51) {
                ASSERT(size == kGlm51BlockBytes);
                ASSERT(fragsPerTask_ == kGlm51IoCount);
            }

            const auto taskFrags =
                ioMode_ == CopyIoMode::GLM51 ? size_t{1}
                                             : (fragsPerTask_ == 0 ? number : fragsPerTask_);
            ASSERT(taskFrags > 0);
            ASSERT(number % taskFrags == 0);
            const auto taskCount = number / taskFrags;
            const auto activeStreamCount = std::min(streamCount_, taskCount);

            ASCEND_ASSERT(aclrtSetDevice(deviceId));
            std::vector<size_t> group;
            group.reserve(activeStreamCount);
            for (size_t stream = 0; stream < activeStreamCount; ++stream) {
                DirectContext ctx;
                ctx.deviceId = deviceId;
                ASCEND_ASSERT(aclrtCreateStream(&ctx.stream));
                ASCEND_ASSERT(aclrtCreateEvent(&ctx.endEvent));
                group.push_back(contexts_.size());
                contexts_.push_back(std::move(ctx));
            }

            std::vector<size_t> groupTasks;
            groupTasks.reserve(taskCount);
            for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex) {
                const size_t stream = CopyTaskStreamIndex(submitMode_, taskIndex, taskCount,
                                                          activeStreamCount);

                std::vector<AscendFftsCopySpec> copies;
                if (ioMode_ == CopyIoMode::GLM51) {
                    copies.reserve(kGlm51IoCount);
                    auto* srcBase = static_cast<char*>(mappedSrc->MappedAt(taskIndex));
                    auto* dstBase = static_cast<char*>((*dst)[taskIndex]);
                    for (size_t io = 0; io < kGlm51IoCount; ++io) {
                        copies.push_back(
                            {dstBase + kGlm51IoOffsets[io], srcBase + kGlm51IoOffsets[io],
                             kGlm51IoSizes[io]});
                    }
                } else {
                    copies.reserve(taskFrags);
                    const size_t first = taskIndex * taskFrags;
                    for (size_t fragment = first; fragment < first + taskFrags; ++fragment) {
                        copies.push_back({(*dst)[fragment], mappedSrc->MappedAt(fragment), size});
                    }
                }

                DirectTask task;
                task.contextIndex = group[stream];
                task.specs = std::move(copies);
                const size_t globalTaskIndex = tasks_.size();
                tasks_.push_back(std::move(task));
                contexts_[group[stream]].taskIndices.push_back(globalTaskIndex);
                groupTasks.push_back(globalTaskIndex);
            }
            contextGroups_.push_back(std::move(group));
            taskGroups_.push_back(std::move(groupTasks));
        }
        inFlight_.reserve(tasks_.size());

        ASSERT(!contexts_.empty());
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
        contextGroups_.clear();
        tasks_.clear();
        taskGroups_.clear();
        inFlight_.clear();
    }

    std::pair<size_t, size_t> DoCopyOnce() override
    {
        using namespace std::chrono;

        ASSERT(inFlight_.empty());
        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        ASCEND_ASSERT(aclrtRecordEvent(totalStart_, contexts_[0].stream));
        for (size_t i = 1; i < contexts_.size(); ++i) {
            ASCEND_ASSERT(aclrtSetDevice(contexts_[i].deviceId));
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[i].stream, totalStart_));
        }

        const auto submitStart = steady_clock::now();
        SubmitTasks();
        const auto submitCost =
            static_cast<size_t>(duration_cast<microseconds>(steady_clock::now() - submitStart)
                                    .count());

        ASCEND_ASSERT(aclrtSetDevice(contexts_[0].deviceId));
        for (size_t i = 1; i < contexts_.size(); ++i) {
            ASCEND_ASSERT(aclrtStreamWaitEvent(contexts_[0].stream, contexts_[i].endEvent));
        }
        ASCEND_ASSERT(aclrtRecordEvent(totalEnd_, contexts_[0].stream));
        ASCEND_ASSERT(aclrtSynchronizeStream(contexts_[0].stream));
        inFlight_.clear();

        float copyCostMs = 0.0f;
        ASCEND_ASSERT(aclrtEventElapsedTime(&copyCostMs, totalStart_, totalEnd_));
        const auto copyCost = static_cast<size_t>(copyCostMs * 1000);
        return {copyCost, submitCost};
    }

    void SubmitTask(size_t taskIndex)
    {
        const auto& task = tasks_[taskIndex];
        auto& ctx = contexts_[task.contextIndex];
        auto inFlight = std::make_unique<InFlightTask>();
        inFlight->specs = task.specs;
        const auto readyCount = inFlight->dispatcher.BuildCopies(inFlight->specs);
        ASSERT(readyCount > 0);
        inFlight->dispatcher.Launch(ctx.stream, readyCount);
        inFlight_.push_back(std::move(inFlight));
    }

    void SubmitTasks()
    {
        for (size_t groupIndex = 0; groupIndex < contextGroups_.size(); ++groupIndex) {
            const auto& group = contextGroups_[groupIndex];
            ASSERT(!group.empty());
            auto& firstContext = contexts_[group.front()];
            ASCEND_ASSERT(aclrtSetDevice(firstContext.deviceId));

            if (submitMode_ == CopySubmitMode::ROUND_ROBIN) {
                for (const auto taskIndex : taskGroups_[groupIndex]) { SubmitTask(taskIndex); }
            } else {
                for (const auto contextIndex : group) {
                    for (const auto taskIndex : contexts_[contextIndex].taskIndices) {
                        SubmitTask(taskIndex);
                    }
                }
            }

            for (const auto contextIndex : group) {
                auto& ctx = contexts_[contextIndex];
                ASCEND_ASSERT(aclrtRecordEvent(ctx.endEvent, ctx.stream));
            }
        }
    }

public:
    FftsDirectH2DCopyInstance(
        size_t iterations, bool affinitySrc, size_t fragsPerTask = 0, size_t streamCount = 1,
        CopyIoMode ioMode = CopyIoMode::UNIFORM,
        CopySubmitMode submitMode = CopySubmitMode::STREAM_MAJOR)
        : CopyInstance(iterations, affinitySrc),
          fragsPerTask_(fragsPerTask),
          streamCount_(streamCount),
          ioMode_(ioMode),
          submitMode_(submitMode)
    {
    }

    std::string Name() const override
    {
        return "ffts-direct-h2d-" + std::to_string(streamCount_) + "s" +
               CopySubmitModeSuffix(submitMode_) +
               (ioMode_ == CopyIoMode::GLM51 ? "-GLM51" : "");
    }
};

#endif  // COPY_INSTANCE_FFTS_DIRECT_H2D_ASCEND_H
