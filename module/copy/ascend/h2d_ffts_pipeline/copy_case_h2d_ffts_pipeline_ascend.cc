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
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#include "ascend/copy_buffer_ascend.h"
#include "ascend/forked_copy_runner_ascend.h"
#include "copy_case.h"
#include "copy_instance_h2d_ffts_pipeline_ascend.h"

namespace {

bool FftsValidationEnabled()
{
    const char* value = std::getenv("COPY_FFTS_VALIDATE");
    if (value == nullptr) { return false; }
    return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
           std::strcmp(value, "ON") == 0;
}

size_t ReadFftsPipelineObjectFrags()
{
    constexpr size_t kDefaultObjectFrags = 8;
    const char* value = std::getenv("COPY_FFTS_PIPELINE_OBJECT_FRAGS");
    if (value == nullptr || value[0] == '\0') { return kDefaultObjectFrags; }

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0) {
        return kDefaultObjectFrags;
    }
    if (parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return std::numeric_limits<size_t>::max();
    }
    return static_cast<size_t>(parsed);
}

std::vector<uint8_t> MakePattern(size_t fragmentIndex, size_t size)
{
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((fragmentIndex * 17 + i * 31 + (i >> 8)) & 0xFF);
    }
    return data;
}

void InitializeHostPatternedBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        const auto pattern = MakePattern(i, buffer.Size());
        std::memcpy(buffer[i], pattern.data(), pattern.size());
    }
}

void ResetBuffer(const CopyBuffer& buffer)
{
    ASCEND_ASSERT(aclrtSetDevice(buffer.Device()));
    for (size_t i = 0; i < buffer.Number(); ++i) {
        ASCEND_ASSERT(aclrtMemset(buffer[i], buffer.Size(), 0, buffer.Size()));
    }
}

std::vector<uint8_t> CopyDeviceToHost(const CopyBuffer& buffer, size_t index)
{
    std::vector<uint8_t> data(buffer.Size());
    ASCEND_ASSERT(aclrtSetDevice(buffer.Device()));
    ASCEND_ASSERT(aclrtMemcpy(data.data(), data.size(), buffer[index], buffer.Size(),
                              ACL_MEMCPY_DEVICE_TO_HOST));
    return data;
}

bool ValidatePatternedBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        auto actual = CopyDeviceToHost(buffer, i);
        auto expected = MakePattern(i, buffer.Size());
        if (actual.size() != expected.size() ||
            std::memcmp(actual.data(), expected.data(), expected.size()) != 0) {
            return false;
        }
    }
    return true;
}

void ValidateDeviceBufferIfEnabled(const CopyBuffer& buffer, bool enabled)
{
    if (enabled) { ASSERT(ValidatePatternedBuffer(buffer)); }
}

void ShowPipelineResult(const CopyCase& copyCase, const CopyResult& result,
                        size_t effectiveObjectFrags)
{
    result.Show("[[ " + copyCase.Key() + " ]] " + copyCase.Brief() +
                " [object_frags=" + std::to_string(effectiveObjectFrags) + "]");
}

void PrintValidationPassIfEnabled(const CopyCase& copyCase, bool enabled)
{
    if (enabled) { std::cout << "[validation] " << copyCase.Key() << " PASS\n"; }
}

}  // namespace

DEFINE_COPY_CASE(Host2DeviceFFTSPipelineCase, "host_to_device_ffts_pipeline",
                 "copy host buffers to fragmented device buffers with h2d and ffts pipeline",
                 ctx)
{
    CopyResult result;
    const auto objectFrags = ReadFftsPipelineObjectFrags();
    const auto effectiveObjectFrags = ctx.num == 0 ? objectFrags : std::min(objectFrags, ctx.num);
    const bool validationEnabled = FftsValidationEnabled();
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializeHostPatternedBuffer(srcBuffer);
        ResetBuffer(dstBuffer);

        H2DFFTSPipelineCopyInstance instance{ctx.iter, false, objectFrags};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateDeviceBufferIfEnabled(dstBuffer, validationEnabled);
    }
    PrintValidationPassIfEnabled(*this, validationEnabled);
    ShowPipelineResult(*this, result, effectiveObjectFrags);
}

DEFINE_COPY_CASE(HugeShm2DeviceFFTSPipelineCase, "huge_shm_to_device_ffts_pipeline",
                 "copy HugeTLB shared host memory to fragmented device buffers with h2d and ffts pipeline",
                 ctx)
{
    CopyResult result;
    const auto objectFrags = ReadFftsPipelineObjectFrags();
    const auto effectiveObjectFrags = ctx.num == 0 ? objectFrags : std::min(objectFrags, ctx.num);
    const bool validationEnabled = FftsValidationEnabled();
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HugeSharedCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        InitializeHostPatternedBuffer(srcBuffer);
        ResetBuffer(dstBuffer);

        H2DFFTSPipelineCopyInstance instance{ctx.iter, false, objectFrags};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
        ValidateDeviceBufferIfEnabled(dstBuffer, validationEnabled);
    }
    PrintValidationPassIfEnabled(*this, validationEnabled);
    ShowPipelineResult(*this, result, effectiveObjectFrags);
}

DEFINE_COPY_CASE_NO_RUNTIME(
    OneShareHost2AllDeviceFFTSPipelineCase, "one_share_host_to_all_device_ffts_pipeline",
    "copy one shared host buffer to all fragmented device buffers with h2d and ffts pipeline using fork submit",
    ctx)
{
    CopyResult result;
    const auto objectFrags = ReadFftsPipelineObjectFrags();
    const auto effectiveObjectFrags = ctx.num == 0 ? objectFrags : std::min(objectFrags, ctx.num);
    const bool validationEnabled = FftsValidationEnabled();

    SharedHostRegion srcRegion{"one_share_host_to_all_device_ffts_pipeline", 0, ctx.size,
                               ctx.num};
    InitializeHostPatternedBuffer(srcRegion);
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, srcRegion.Name(), "acl::device_frag::all", "h2d_ffts_pipeline-FORK",
        [&](size_t device, CopyIterationObserver* observer) {
            SharedHostCopyBuffer srcBuffer{srcRegion.ShmName(), device, ctx.size, ctx.num};
            FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            ResetBuffer(dstBuffer);

            H2DFFTSPipelineCopyInstance instance{ctx.iter, false, objectFrags};
            auto childResult = instance.DoCopy(&srcBuffer, &dstBuffer, observer);
            ValidateDeviceBufferIfEnabled(dstBuffer, validationEnabled);
            return childResult;
        }));
    PrintValidationPassIfEnabled(*this, validationEnabled);
    ShowPipelineResult(*this, result, effectiveObjectFrags);
}

DEFINE_COPY_CASE_NO_RUNTIME(
    OneHugeShm2AllDeviceFFTSPipelineCase, "one_huge_shm_to_all_device_ffts_pipeline",
    "copy one HugeTLB shared host buffer to all fragmented device buffers with h2d and ffts pipeline using fork submit",
    ctx)
{
    CopyResult result;
    const auto objectFrags = ReadFftsPipelineObjectFrags();
    const auto effectiveObjectFrags = ctx.num == 0 ? objectFrags : std::min(objectFrags, ctx.num);
    const bool validationEnabled = FftsValidationEnabled();

    HugeSharedRegion srcRegion{"one_huge_shm_to_all_device_ffts_pipeline", 0, ctx.size,
                               ctx.num};
    InitializeHostPatternedBuffer(srcRegion);
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, srcRegion.Name(), "acl::device_frag::all", "h2d_ffts_pipeline-FORK",
        [&](size_t device, CopyIterationObserver* observer) {
            HugeSharedCopyBuffer srcBuffer{srcRegion.Fd(), srcRegion.MappedBytes(), device,
                                           ctx.size, ctx.num};
            FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            ResetBuffer(dstBuffer);

            H2DFFTSPipelineCopyInstance instance{ctx.iter, false, objectFrags};
            auto childResult = instance.DoCopy(&srcBuffer, &dstBuffer, observer);
            ValidateDeviceBufferIfEnabled(dstBuffer, validationEnabled);
            return childResult;
        }));
    PrintValidationPassIfEnabled(*this, validationEnabled);
    ShowPipelineResult(*this, result, effectiveObjectFrags);
}

DEFINE_COPY_CASE(OneHost2AllDeviceFFTSPipelineCase, "one_host_to_all_device_ffts_pipeline",
                 "copy one host buffer to all fragmented device buffers with h2d and ffts pipeline",
                 ctx)
{
    CopyResult result;
    const auto objectFrags = ReadFftsPipelineObjectFrags();
    const auto effectiveObjectFrags = ctx.num == 0 ? objectFrags : std::min(objectFrags, ctx.num);
    const bool validationEnabled = FftsValidationEnabled();

    HostCopyBuffer srcBuffer{0, ctx.size, ctx.num};
    InitializeHostPatternedBuffer(srcBuffer);
    std::vector<const CopyBuffer*> srcBuffers(ctx.nDevice, &srcBuffer);
    std::vector<const CopyBuffer*> dstBuffers(ctx.nDevice);
    for (size_t device = 0; device < ctx.nDevice; device++) {
        dstBuffers[device] = new FragmentedDeviceCopyBuffer{device, ctx.size, ctx.num};
        ResetBuffer(*dstBuffers[device]);
    }

    H2DFFTSPipelineCopyInstance instance{ctx.iter, false, objectFrags};
    result.Push(instance.DoCopyBatch(srcBuffers, dstBuffers));
    for (size_t device = 0; device < ctx.nDevice; device++) {
        ValidateDeviceBufferIfEnabled(*dstBuffers[device], validationEnabled);
        delete dstBuffers[device];
    }
    PrintValidationPassIfEnabled(*this, validationEnabled);
    ShowPipelineResult(*this, result, effectiveObjectFrags);
}

DEFINE_COPY_CASE_NO_RUNTIME(
    AllHost2AllDeviceFFTSPipelineCase, "all_host_to_all_device_ffts_pipeline",
    "copy all host buffers to all fragmented device buffers with h2d and ffts pipeline using fork submit",
    ctx)
{
    CopyResult result;
    const auto objectFrags = ReadFftsPipelineObjectFrags();
    const auto effectiveObjectFrags = ctx.num == 0 ? objectFrags : std::min(objectFrags, ctx.num);
    const bool validationEnabled = FftsValidationEnabled();

    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, "acl::host::all", "acl::device_frag::all", "h2d_ffts_pipeline-FORK",
        [&](size_t device, CopyIterationObserver* observer) {
            HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
            FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            InitializeHostPatternedBuffer(srcBuffer);
            ResetBuffer(dstBuffer);

            H2DFFTSPipelineCopyInstance instance{ctx.iter, false, objectFrags};
            auto childResult = instance.DoCopy(&srcBuffer, &dstBuffer, observer);
            ValidateDeviceBufferIfEnabled(dstBuffer, validationEnabled);
            return childResult;
        }));
    PrintValidationPassIfEnabled(*this, validationEnabled);
    ShowPipelineResult(*this, result, effectiveObjectFrags);
}

DEFINE_COPY_CASE_NO_RUNTIME(
    AllODirectHost2AllDeviceFFTSPipelineCase,
    "all_odirect_host_to_all_device_ffts_pipeline",
    "copy all UCM O_DIRECT style host buffers to all fragmented device buffers with h2d "
    "and ffts pipeline using fork submit",
    ctx)
{
    CopyResult result;
    const auto objectFrags = ReadFftsPipelineObjectFrags();
    const auto effectiveObjectFrags = ctx.num == 0 ? objectFrags : std::min(objectFrags, ctx.num);
    const bool validationEnabled = FftsValidationEnabled();

    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, "acl::odirect_mmap::all", "acl::device_frag::all", "h2d_ffts_pipeline-FORK",
        [&](size_t device, CopyIterationObserver* observer) {
            ODirectHostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
            FragmentedDeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            InitializeHostPatternedBuffer(srcBuffer);
            ResetBuffer(dstBuffer);

            H2DFFTSPipelineCopyInstance instance{ctx.iter, false, objectFrags};
            auto childResult = instance.DoCopy(&srcBuffer, &dstBuffer, observer);
            ValidateDeviceBufferIfEnabled(dstBuffer, validationEnabled);
            return childResult;
        }));
    PrintValidationPassIfEnabled(*this, validationEnabled);
    ShowPipelineResult(*this, result, effectiveObjectFrags);
}
