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
#ifndef COPY_OPTIONS_H
#define COPY_OPTIONS_H

#include <array>
#include <cstddef>

enum class CopyIoMode { UNIFORM, GLM51 };

enum class CopySubmitMode { STREAM_MAJOR, ROUND_ROBIN };

constexpr std::array<size_t, 3> kGlm51IoSizes{128ull * 1024ull, 16ull * 1024ull,
                                              32ull * 1024ull};
constexpr std::array<size_t, 3> kGlm51IoOffsets{0, kGlm51IoSizes[0],
                                                kGlm51IoSizes[0] + kGlm51IoSizes[1]};
constexpr size_t kGlm51IoCount = kGlm51IoSizes.size();
constexpr size_t kGlm51BlockBytes =
    kGlm51IoSizes[0] + kGlm51IoSizes[1] + kGlm51IoSizes[2];

inline const char* CopyIoModeName(CopyIoMode mode)
{
    return mode == CopyIoMode::GLM51 ? "glm5.1" : "uniform";
}

inline const char* CopySubmitModeName(CopySubmitMode mode)
{
    return mode == CopySubmitMode::ROUND_ROBIN ? "round-robin" : "stream-major";
}

inline const char* CopySubmitModeSuffix(CopySubmitMode mode)
{
    return mode == CopySubmitMode::ROUND_ROBIN ? "-RR" : "-SM";
}

inline size_t CopyIoBufferSize(CopyIoMode mode, size_t uniformSize)
{
    return mode == CopyIoMode::GLM51 ? kGlm51BlockBytes : uniformSize;
}

inline size_t CopyTaskStreamIndex(CopySubmitMode mode, size_t taskIndex, size_t taskCount,
                                  size_t activeStreamCount)
{
    if (mode == CopySubmitMode::ROUND_ROBIN) { return taskIndex % activeStreamCount; }

    const size_t base = taskCount / activeStreamCount;
    const size_t remainder = taskCount % activeStreamCount;
    const size_t largerGroupSize = base + 1;
    const size_t largerTaskCount = remainder * largerGroupSize;
    if (taskIndex < largerTaskCount) { return taskIndex / largerGroupSize; }
    return remainder + (taskIndex - largerTaskCount) / base;
}

#endif  // COPY_OPTIONS_H
