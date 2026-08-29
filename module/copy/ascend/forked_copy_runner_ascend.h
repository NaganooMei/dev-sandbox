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
#ifndef FORKED_COPY_RUNNER_ASCEND_H
#define FORKED_COPY_RUNNER_ASCEND_H

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "copy_case.h"
#include "copy_instance.h"
#include "copy_result.h"
#include "copy_runtime.h"
#include "error_handle.h"

namespace ascend_copy {

using ForkedChildCopyFn =
    std::function<CopyResult::Result(size_t device, CopyIterationObserver* observer)>;

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000ull;
constexpr std::uint64_t kProcessBarrierReleaseLeadNs = 1000000ull;
constexpr time_t kProcessBarrierTimeoutSeconds = 60;

inline std::uint64_t MonotonicNowNs()
{
    timespec now{};
    ASSERT(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return static_cast<std::uint64_t>(now.tv_sec) * kNanosecondsPerSecond +
           static_cast<std::uint64_t>(now.tv_nsec);
}

inline size_t NanosecondsToMicroseconds(std::uint64_t nanoseconds)
{
    return static_cast<size_t>((nanoseconds + 999) / 1000);
}

struct alignas(64) ForkProcessSyncShared {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    size_t participants = 0;
    size_t iterations = 0;
    size_t arrived = 0;
    size_t generation = 0;
    bool aborted = false;
    std::uint64_t releaseTimeNs = 0;
};

struct ForkedProcessTiming {
    std::vector<size_t> startSkewCosts;
    std::vector<size_t> groupWallCosts;
};

class ForkProcessSync {
    void* mapping_ = MAP_FAILED;
    size_t mappingSize_ = 0;
    ForkProcessSyncShared* shared_ = nullptr;
    CopyProcessSyncMode mode_ = CopyProcessSyncMode::NONE;

    size_t TimestampIndex(size_t device, size_t iteration) const
    {
        ASSERT(device < shared_->participants);
        ASSERT(iteration < shared_->iterations);
        return device * shared_->iterations + iteration;
    }

    std::uint64_t* StartTimes() const
    {
        return reinterpret_cast<std::uint64_t*>(shared_ + 1);
    }

    std::uint64_t* EndTimes() const
    {
        return StartTimes() + shared_->participants * shared_->iterations;
    }

    void Lock()
    {
        const auto rc = pthread_mutex_lock(&shared_->mutex);
#ifdef __linux__
        if (rc == EOWNERDEAD) {
            shared_->aborted = true;
            ASSERT(pthread_mutex_consistent(&shared_->mutex) == 0);
            return;
        }
#endif
        if (rc != 0) { throw std::runtime_error("failed to lock fork process barrier"); }
    }

    void Unlock() noexcept { pthread_mutex_unlock(&shared_->mutex); }

    [[noreturn]] void FailBarrier(size_t device, size_t iteration, const char* reason)
    {
        shared_->aborted = true;
        pthread_cond_broadcast(&shared_->condition);
        Unlock();
        throw std::runtime_error("fork process barrier " + std::string(reason) +
                                 " on device " + std::to_string(device) + " iteration " +
                                 std::to_string(iteration));
    }

    std::uint64_t WaitForRelease(size_t device, size_t iteration)
    {
        Lock();
        if (shared_->aborted) { FailBarrier(device, iteration, "was aborted"); }

        const auto generation = shared_->generation;
        ++shared_->arrived;
        if (shared_->arrived == shared_->participants) {
            shared_->arrived = 0;
            shared_->releaseTimeNs = MonotonicNowNs() + kProcessBarrierReleaseLeadNs;
            ++shared_->generation;
            if (pthread_cond_broadcast(&shared_->condition) != 0) {
                FailBarrier(device, iteration, "broadcast failed");
            }
        } else {
            timespec deadline{};
            ASSERT(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
            deadline.tv_sec += kProcessBarrierTimeoutSeconds;
            while (generation == shared_->generation && !shared_->aborted) {
                const auto rc =
                    pthread_cond_timedwait(&shared_->condition, &shared_->mutex, &deadline);
                if (rc == ETIMEDOUT) { FailBarrier(device, iteration, "timed out"); }
                if (rc != 0) { FailBarrier(device, iteration, "wait failed"); }
            }
            if (shared_->aborted) { FailBarrier(device, iteration, "was aborted"); }
        }

        const auto releaseTimeNs = shared_->releaseTimeNs;
        Unlock();
        return releaseTimeNs;
    }

public:
    ForkProcessSync(size_t participants, size_t iterations, CopyProcessSyncMode mode)
        : mode_(mode)
    {
        ASSERT(participants > 0);
        const auto timestampCount = participants * iterations;
        mappingSize_ = sizeof(ForkProcessSyncShared) +
                       timestampCount * sizeof(std::uint64_t) * 2;
        mapping_ = mmap(nullptr, mappingSize_, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        ASSERT(mapping_ != MAP_FAILED);
        std::memset(mapping_, 0, mappingSize_);
        shared_ = new (mapping_) ForkProcessSyncShared{};
        shared_->participants = participants;
        shared_->iterations = iterations;

        pthread_mutexattr_t mutexAttr{};
        ASSERT(pthread_mutexattr_init(&mutexAttr) == 0);
        ASSERT(pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED) == 0);
#ifdef __linux__
        ASSERT(pthread_mutexattr_setrobust(&mutexAttr, PTHREAD_MUTEX_ROBUST) == 0);
#endif
        ASSERT(pthread_mutex_init(&shared_->mutex, &mutexAttr) == 0);
        ASSERT(pthread_mutexattr_destroy(&mutexAttr) == 0);

        pthread_condattr_t conditionAttr{};
        ASSERT(pthread_condattr_init(&conditionAttr) == 0);
        ASSERT(pthread_condattr_setpshared(&conditionAttr, PTHREAD_PROCESS_SHARED) == 0);
        ASSERT(pthread_condattr_setclock(&conditionAttr, CLOCK_MONOTONIC) == 0);
        ASSERT(pthread_cond_init(&shared_->condition, &conditionAttr) == 0);
        ASSERT(pthread_condattr_destroy(&conditionAttr) == 0);
    }

    ForkProcessSync(const ForkProcessSync&) = delete;
    ForkProcessSync& operator=(const ForkProcessSync&) = delete;

    ~ForkProcessSync()
    {
        if (shared_ != nullptr) {
            pthread_cond_destroy(&shared_->condition);
            pthread_mutex_destroy(&shared_->mutex);
        }
        if (mapping_ != MAP_FAILED) { munmap(mapping_, mappingSize_); }
    }

    CopyProcessSyncMode Mode() const { return mode_; }

    void BeforeIteration(size_t device, size_t iteration)
    {
        if (mode_ == CopyProcessSyncMode::BARRIER) {
            const auto releaseTimeNs = WaitForRelease(device, iteration);
            while (MonotonicNowNs() < releaseTimeNs) {}
        }
        StartTimes()[TimestampIndex(device, iteration)] = MonotonicNowNs();
    }

    void AfterIteration(size_t device, size_t iteration)
    {
        EndTimes()[TimestampIndex(device, iteration)] = MonotonicNowNs();
    }

    void Abort() noexcept
    {
        if (shared_ == nullptr) { return; }
        const auto rc = pthread_mutex_lock(&shared_->mutex);
#ifdef __linux__
        if (rc == EOWNERDEAD) { pthread_mutex_consistent(&shared_->mutex); }
#endif
        if (rc == 0 || rc == EOWNERDEAD) {
            shared_->aborted = true;
            pthread_cond_broadcast(&shared_->condition);
            pthread_mutex_unlock(&shared_->mutex);
        }
    }

    ForkedProcessTiming CollectTiming() const
    {
        ForkedProcessTiming timing;
        timing.startSkewCosts.reserve(shared_->iterations);
        timing.groupWallCosts.reserve(shared_->iterations);
        for (size_t iteration = 0; iteration < shared_->iterations; ++iteration) {
            std::uint64_t minStart = std::numeric_limits<std::uint64_t>::max();
            std::uint64_t maxStart = 0;
            std::uint64_t maxEnd = 0;
            for (size_t device = 0; device < shared_->participants; ++device) {
                const auto index = TimestampIndex(device, iteration);
                const auto start = StartTimes()[index];
                const auto end = EndTimes()[index];
                ASSERT(start > 0);
                ASSERT(end >= start);
                minStart = std::min(minStart, start);
                maxStart = std::max(maxStart, start);
                maxEnd = std::max(maxEnd, end);
            }
            timing.startSkewCosts.push_back(NanosecondsToMicroseconds(maxStart - minStart));
            timing.groupWallCosts.push_back(NanosecondsToMicroseconds(maxEnd - minStart));
        }
        return timing;
    }
};

class ForkedCopyIterationObserver : public CopyIterationObserver {
    ForkProcessSync* processSync_;
    size_t device_;

public:
    ForkedCopyIterationObserver(ForkProcessSync* processSync, size_t device)
        : processSync_(processSync), device_(device)
    {
    }

    void BeforeIteration(size_t iteration) override
    {
        processSync_->BeforeIteration(device_, iteration);
    }

    void AfterIteration(size_t iteration) override
    {
        processSync_->AfterIteration(device_, iteration);
    }
};

struct ForkedChildProcess {
    pid_t pid = -1;
    int readFd = -1;
    size_t device = 0;
};

struct ResultWireHeader {
    std::uint64_t srcLen = 0;
    std::uint64_t dstLen = 0;
    std::uint64_t methodLen = 0;
    std::uint64_t size = 0;
    std::uint64_t count = 0;
    std::uint64_t submitCount = 0;
    std::uint64_t copyCount = 0;
};

inline bool WriteExact(int fd, const void* data, size_t size)
{
    const auto* cursor = static_cast<const char*>(data);
    while (size > 0) {
        const auto written = write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR) { continue; }
            return false;
        }
        if (written == 0) { return false; }
        cursor += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

inline bool ReadExact(int fd, void* data, size_t size)
{
    auto* cursor = static_cast<char*>(data);
    while (size > 0) {
        const auto nread = read(fd, cursor, size);
        if (nread < 0) {
            if (errno == EINTR) { continue; }
            return false;
        }
        if (nread == 0) { return false; }
        cursor += nread;
        size -= static_cast<size_t>(nread);
    }
    return true;
}

inline bool WriteString(int fd, const std::string& value)
{
    return value.empty() || WriteExact(fd, value.data(), value.size());
}

inline bool ReadString(int fd, std::uint64_t size, std::string& value)
{
    value.assign(static_cast<size_t>(size), '\0');
    return value.empty() || ReadExact(fd, value.data(), value.size());
}

inline bool WriteCosts(int fd, const std::vector<size_t>& costs)
{
    for (const auto cost : costs) {
        const std::uint64_t wireCost = static_cast<std::uint64_t>(cost);
        if (!WriteExact(fd, &wireCost, sizeof(wireCost))) { return false; }
    }
    return true;
}

inline bool ReadCosts(int fd, std::uint64_t count, std::vector<size_t>& costs)
{
    costs.resize(static_cast<size_t>(count));
    for (auto& cost : costs) {
        std::uint64_t wireCost = 0;
        if (!ReadExact(fd, &wireCost, sizeof(wireCost))) { return false; }
        cost = static_cast<size_t>(wireCost);
    }
    return true;
}

inline bool WriteResult(int fd, const CopyResult::Result& result)
{
    ResultWireHeader header;
    header.srcLen = static_cast<std::uint64_t>(result.src.size());
    header.dstLen = static_cast<std::uint64_t>(result.dst.size());
    header.methodLen = static_cast<std::uint64_t>(result.method.size());
    header.size = static_cast<std::uint64_t>(result.size);
    header.count = static_cast<std::uint64_t>(result.count);
    header.submitCount = static_cast<std::uint64_t>(result.submitCosts.size());
    header.copyCount = static_cast<std::uint64_t>(result.copyCosts.size());

    return WriteExact(fd, &header, sizeof(header)) && WriteString(fd, result.src) &&
           WriteString(fd, result.dst) && WriteString(fd, result.method) &&
           WriteCosts(fd, result.submitCosts) && WriteCosts(fd, result.copyCosts);
}

inline bool ReadResult(int fd, CopyResult::Result& result)
{
    ResultWireHeader header;
    if (!ReadExact(fd, &header, sizeof(header))) { return false; }

    std::string src;
    std::string dst;
    std::string method;
    std::vector<size_t> submitCosts;
    std::vector<size_t> copyCosts;
    if (!ReadString(fd, header.srcLen, src) || !ReadString(fd, header.dstLen, dst) ||
        !ReadString(fd, header.methodLen, method) ||
        !ReadCosts(fd, header.submitCount, submitCosts) ||
        !ReadCosts(fd, header.copyCount, copyCosts)) {
        return false;
    }

    result = CopyResult::Result{std::move(src),
                                std::move(dst),
                                std::move(method),
                                static_cast<size_t>(header.size),
                                static_cast<size_t>(header.count),
                                std::move(submitCosts),
                                std::move(copyCosts)};
    return true;
}

[[noreturn]] inline void RunChildCopy(size_t device, int writeFd, ForkProcessSync* processSync,
                                       const ForkedChildCopyFn& childCopy)
{
    int status = EXIT_FAILURE;
    {
        try {
            CopyRuntime runtime;
            ForkedCopyIterationObserver observer{processSync, device};
            auto result = childCopy(device, &observer);
            status = WriteResult(writeFd, result) ? EXIT_SUCCESS : EXIT_FAILURE;
        } catch (const std::exception& e) {
            processSync->Abort();
            std::fprintf(stderr, "[fork-copy] device %zu failed: %s\n", device, e.what());
        } catch (...) {
            processSync->Abort();
            std::fprintf(stderr, "[fork-copy] device %zu failed with unknown error\n", device);
        }
    }
    close(writeFd);
    std::_Exit(status);
}

inline std::vector<size_t> MergeMaxCosts(const std::vector<CopyResult::Result>& results,
                                         bool submit)
{
    ASSERT(!results.empty());
    const auto& first = submit ? results.front().submitCosts : results.front().copyCosts;
    std::vector<size_t> merged(first.size(), 0);
    for (const auto& result : results) {
        const auto& costs = submit ? result.submitCosts : result.copyCosts;
        ASSERT(costs.size() == merged.size());
        for (size_t i = 0; i < costs.size(); ++i) {
            merged[i] = std::max(merged[i], costs[i]);
        }
    }
    return merged;
}

inline CopyResult::Result MergeForkedResults(std::vector<CopyResult::Result>&& results,
                                             std::string srcName, std::string dstName,
                                             std::string methodName)
{
    ASSERT(!results.empty());
    size_t totalCount = 0;
    for (const auto& result : results) {
        ASSERT(result.size == results.front().size);
        totalCount += result.count;
    }

    return {std::move(srcName),
            std::move(dstName),
            std::move(methodName),
            results.front().size,
            totalCount,
            MergeMaxCosts(results, true),
            MergeMaxCosts(results, false)};
}

inline std::vector<CopyResult::Result> RunForkedCopyBatchPerDevice(
    const CopyCase::Context& ctx, const ForkedChildCopyFn& childCopy,
    ForkedProcessTiming* timing = nullptr)
{
    ASSERT(ctx.nDevice > 0);
    ForkProcessSync processSync{ctx.nDevice, ctx.iter, ctx.processSyncMode};
    std::vector<ForkedChildProcess> children;
    children.reserve(ctx.nDevice);

    for (size_t device = 0; device < ctx.nDevice; ++device) {
        int pipeFds[2];
        ASSERT(pipe(pipeFds) == 0);
        const auto pid = fork();
        ASSERT(pid != -1);
        if (pid == 0) {
            close(pipeFds[0]);
            RunChildCopy(device, pipeFds[1], &processSync, childCopy);
        }

        close(pipeFds[1]);
        children.push_back({pid, pipeFds[0], device});
    }

    std::vector<CopyResult::Result> childResults;
    childResults.reserve(children.size());
    bool failed = false;
    for (auto& child : children) {
        CopyResult::Result result{"", "", "", 0, 0, {}, {}};
        if (!ReadResult(child.readFd, result)) {
            std::fprintf(stderr, "[fork-copy] failed to read result from device %zu\n",
                         child.device);
            failed = true;
        } else {
            childResults.push_back(std::move(result));
        }
        close(child.readFd);
    }

    for (const auto& child : children) {
        int status = 0;
        pid_t waited = -1;
        do {
            waited = waitpid(child.pid, &status, 0);
        } while (waited == -1 && errno == EINTR);
        ASSERT(waited == child.pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
            std::fprintf(stderr, "[fork-copy] child for device %zu exited abnormally\n",
                         child.device);
            failed = true;
        }
    }
    ASSERT(!failed);
    ASSERT(childResults.size() == ctx.nDevice);
    if (timing != nullptr) { *timing = processSync.CollectTiming(); }

    return childResults;
}

inline CopyResult::Result RunForkedCopyBatch(const CopyCase::Context& ctx, std::string srcName,
                                             std::string dstName, std::string methodName,
                                             const ForkedChildCopyFn& childCopy)
{
    ForkedProcessTiming timing;
    auto childResults = RunForkedCopyBatchPerDevice(ctx, childCopy, &timing);
    auto result = MergeForkedResults(std::move(childResults), std::move(srcName),
                                     std::move(dstName), std::move(methodName));
    result.SetProcessTiming(CopyProcessSyncModeName(ctx.processSyncMode),
                            std::move(timing.startSkewCosts),
                            std::move(timing.groupWallCosts));
    return result;
}

}  // namespace ascend_copy

#endif  // FORKED_COPY_RUNNER_ASCEND_H
