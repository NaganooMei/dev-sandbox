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
#ifndef GDRBW_MEMCPY_RESULT_H
#define GDRBW_MEMCPY_RESULT_H

#include <fmt/format.h>
#include <string>
#include <vector>

class MemcpyResult {
public:
    struct Result {
        std::string src;
        std::string dst;
        size_t elemSize;
        size_t elemCount;
        double minMs;
        double maxMs;
        double avgMs;
        double p50Ms;
        double p90Ms;
        double p99Ms;
    };

    void Record(Result result) { results_.push_back(std::move(result)); }
    void Show(std::string title) const
    {
        const std::string indentation = "  ";
        fmt::println(title);
        fmt::println("{}{:<30}{:<30}{:<10}{:<12}{:<12}{:<12}{:<12}{:<12}{:<12}{:<12}{:<12}",
                     indentation, "From", "To", "Count", "Size(KB)", "Min(ms)", "Max(ms)",
                     "Avg(ms)", "P50(ms)", "P90(ms)", "P99(ms)", "BW(GB/s)");
        for (const auto& result : results_) {
            double bandwidth = (result.elemSize * result.elemCount) / (result.avgMs * 1e6);
            fmt::println(
                "{}{:<30}{:<30}{:<10}{:<12}{:<12.3f}{:<12.3f}{:<12.3f}{:<12.3f}{:<12.3f}{:<12.3f}"
                "{:<12.3f}",
                indentation, result.src, result.dst, result.elemCount, result.elemSize / 1024.0,
                result.minMs, result.maxMs, result.avgMs, result.p50Ms, result.p90Ms, result.p99Ms,
                bandwidth);
        }
    }

private:
    std::vector<Result> results_;
};

#endif  // GDRBW_MEMCPY_RESULT_H
