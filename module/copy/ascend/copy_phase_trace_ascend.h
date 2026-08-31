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
#ifndef COPY_PHASE_TRACE_ASCEND_H
#define COPY_PHASE_TRACE_ASCEND_H

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string>
#include <vector>
#include "copy_result.h"

struct AscendCopyPhaseSample {
    size_t setDeviceUs = 0;
    size_t recordStartUs = 0;
    size_t armStartUs = 0;
    size_t submitUs = 0;
    size_t fanInUs = 0;
    size_t synchronizeUs = 0;
    size_t releaseUs = 0;
    size_t elapsedQueryUs = 0;
    size_t totalHostUs = 0;
};

class AscendCopyPhaseRecorder {
    using Clock = std::chrono::steady_clock;

    bool enabled_ = false;
    Clock::time_point start_{};
    Clock::time_point last_{};

public:
    explicit AscendCopyPhaseRecorder(bool enabled) : enabled_(enabled)
    {
        if (enabled_) { start_ = last_ = Clock::now(); }
    }

    void Mark(size_t& durationUs)
    {
        if (!enabled_) { return; }
        const auto now = Clock::now();
        durationUs = static_cast<size_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - last_).count());
        last_ = now;
    }

    void Finish(size_t& durationUs) const
    {
        if (!enabled_) { return; }
        durationUs = static_cast<size_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start_)
                .count());
    }
};

class AscendCopyPhaseTrace {
    using Member = size_t AscendCopyPhaseSample::*;

    bool enabled_ = false;
    std::vector<AscendCopyPhaseSample> samples_;

    static bool IsEnabledByEnvironment()
    {
        const char* value = std::getenv("COPY_ASCEND_PHASE_TRACE");
        if (value == nullptr) { return false; }
        return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
               std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
               std::strcmp(value, "ON") == 0;
    }

    CopyResult::Statistics Statistics(size_t begin, Member member) const
    {
        std::vector<size_t> values;
        values.reserve(samples_.size() - begin);
        for (size_t i = begin; i < samples_.size(); ++i) {
            values.push_back(samples_[i].*member);
        }
        CopyResult::Statistics statistics;
        statistics.Parse(values);
        return statistics;
    }

public:
    void Reset(size_t measuredIterations)
    {
        enabled_ = IsEnabledByEnvironment();
        samples_.clear();
        if (enabled_) { samples_.reserve(measuredIterations + 3); }
    }

    bool Enabled() const { return enabled_; }

    void Add(const AscendCopyPhaseSample& sample)
    {
        if (enabled_) { samples_.push_back(sample); }
    }

    void PrintSummary(const std::string& method, size_t device, size_t streams,
                      const char* streamSync, size_t measuredIterations) const
    {
        if (!enabled_ || samples_.empty()) { return; }
        const size_t measured = std::min(measuredIterations, samples_.size());
        const size_t begin = samples_.size() - measured;

        const auto setDevice = Statistics(begin, &AscendCopyPhaseSample::setDeviceUs);
        const auto recordStart = Statistics(begin, &AscendCopyPhaseSample::recordStartUs);
        const auto armStart = Statistics(begin, &AscendCopyPhaseSample::armStartUs);
        const auto submit = Statistics(begin, &AscendCopyPhaseSample::submitUs);
        const auto fanIn = Statistics(begin, &AscendCopyPhaseSample::fanInUs);
        const auto synchronize = Statistics(begin, &AscendCopyPhaseSample::synchronizeUs);
        const auto release = Statistics(begin, &AscendCopyPhaseSample::releaseUs);
        const auto elapsedQuery = Statistics(begin, &AscendCopyPhaseSample::elapsedQueryUs);
        const auto totalHost = Statistics(begin, &AscendCopyPhaseSample::totalHostUs);

        const auto line = fmt::format(
            "ASCEND_PHASE method={} device={} streams={} sync={} samples={} "
            "set_device_avg_us={} set_device_p90_us={} "
            "record_start_avg_us={} record_start_p90_us={} "
            "arm_start_avg_us={} arm_start_p90_us={} "
            "submit_avg_us={} submit_p90_us={} fan_in_avg_us={} fan_in_p90_us={} "
            "synchronize_avg_us={} synchronize_p90_us={} "
            "release_avg_us={} release_p90_us={} "
            "elapsed_query_avg_us={} elapsed_query_p90_us={} "
            "total_host_avg_us={} total_host_p90_us={}",
            method, device, streams, streamSync, measured, setDevice.avg, setDevice.p90,
            recordStart.avg, recordStart.p90, armStart.avg, armStart.p90, submit.avg,
            submit.p90, fanIn.avg, fanIn.p90, synchronize.avg, synchronize.p90,
            release.avg, release.p90, elapsedQuery.avg, elapsedQuery.p90, totalHost.avg,
            totalHost.p90);
        std::fprintf(stderr, "%s\n", line.c_str());
        std::fflush(stderr);
    }
};

#endif  // COPY_PHASE_TRACE_ASCEND_H
