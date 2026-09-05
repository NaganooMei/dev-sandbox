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
#include <acl/acl.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>
#include "ascend/copy_buffer_ascend.h"
#include "ascend/forked_copy_runner_ascend.h"
#include "copy_case.h"
#include "copy_instance_ffts_direct_h2d_ascend.h"
#include "mapped_host_buffer_ffts_direct_h2d_ascend.h"

namespace {

bool FftsDirectValidationEnabled()
{
    const char* value = std::getenv("COPY_FFTS_VALIDATE");
    if (value == nullptr) { return false; }
    return std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 || std::strcmp(value, "on") == 0 ||
           std::strcmp(value, "ON") == 0;
}

std::vector<uint8_t> MakeFftsDirectPattern(size_t fragmentIndex, size_t size)
{
    std::vector<uint8_t> data(size);
    for (size_t i = 0; i < size; ++i) {
        data[i] = static_cast<uint8_t>((fragmentIndex * 17 + i * 31 + (i >> 8)) & 0xFF);
    }
    return data;
}

void InitializeFftsDirectHostPatternedBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        const auto pattern = MakeFftsDirectPattern(i, buffer.Size());
        std::memcpy(buffer[i], pattern.data(), pattern.size());
    }
}

void InitializeFftsDirectHostPatternedRange(void* base, size_t firstBlock, size_t blockCount,
                                            size_t blockSize)
{
    auto* bytes = static_cast<char*>(base);
    for (size_t localBlock = 0; localBlock < blockCount; ++localBlock) {
        const auto pattern = MakeFftsDirectPattern(firstBlock + localBlock, blockSize);
        std::memcpy(bytes + localBlock * blockSize, pattern.data(), pattern.size());
    }
}

void ResetFftsDirectDeviceBuffer(const CopyBuffer& buffer)
{
    ASCEND_ASSERT(aclrtSetDevice(buffer.Device()));
    for (size_t i = 0; i < buffer.Number(); ++i) {
        ASCEND_ASSERT(aclrtMemset(buffer[i], buffer.Size(), 0, buffer.Size()));
    }
}

std::vector<uint8_t> CopyFftsDirectDeviceToHost(const CopyBuffer& buffer, size_t index)
{
    std::vector<uint8_t> data(buffer.Size());
    ASCEND_ASSERT(aclrtSetDevice(buffer.Device()));
    ASCEND_ASSERT(aclrtMemcpy(data.data(), data.size(), buffer[index], buffer.Size(),
                              ACL_MEMCPY_DEVICE_TO_HOST));
    return data;
}

bool ValidateFftsDirectPatternedBuffer(const CopyBuffer& buffer)
{
    for (size_t i = 0; i < buffer.Number(); ++i) {
        auto actual = CopyFftsDirectDeviceToHost(buffer, i);
        auto expected = MakeFftsDirectPattern(i, buffer.Size());
        if (actual.size() != expected.size() ||
            std::memcmp(actual.data(), expected.data(), expected.size()) != 0) {
            return false;
        }
    }
    return true;
}

void ValidateFftsDirectDeviceBufferIfEnabled(const CopyBuffer& buffer, bool enabled)
{
    if (enabled) { ASSERT(ValidateFftsDirectPatternedBuffer(buffer)); }
}

void PrintFftsDirectValidationPassIfEnabled(const CopyCase& copyCase, bool enabled)
{
    if (enabled) { std::cout << "[validation] " << copyCase.Key() << " PASS\n"; }
}

size_t FftsDirectBufferCount(const CopyCase::Context& ctx)
{
    ASSERT(ctx.num > 0);
    if (ctx.ioMode == CopyIoMode::GLM51) { return ctx.num; }
    if (ctx.frags == 0) { return ctx.num; }
    ASSERT(ctx.num <= std::numeric_limits<size_t>::max() / ctx.frags);
    return ctx.num * ctx.frags;
}

size_t FftsDirectStreamCount(const CopyCase::Context& ctx)
{
    constexpr size_t defaultStreamCount = 1;
    return ctx.streams == 0 ? defaultStreamCount : ctx.streams;
}

std::string FftsDirectMethodName(const CopyCase::Context& ctx)
{
    return "ffts-direct-h2d-" + std::to_string(FftsDirectStreamCount(ctx)) + "s" +
           CopySubmitModeSuffix(ctx.submitMode) + CopyStreamStartGateSuffix(ctx.streamStartGate) +
           CopyStreamSyncModeSuffix(ctx.streamSyncMode) +
           (ctx.ioMode == CopyIoMode::GLM51 ? "-GLM51" : "") +
           CopyHostRegisterModeSuffix(ctx.hostRegisterMode);
}

}  // namespace

DEFINE_COPY_CASE_NO_RUNTIME(
    AllHost2AllDeviceFftsDirectH2DCase, "all_host_to_all_device_ffts_direct_h2d",
    "copy all aclrtMallocHost mapped host buffers to all device buffers with ffts direct h2d",
    ctx)
{
    CopyResult result;
    const bool validationEnabled = FftsDirectValidationEnabled();
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, "acl::host_mapped::all", "acl::device::all", FftsDirectMethodName(ctx),
        [&](size_t device, CopyIterationObserver* observer) {
            const auto bufferCount = FftsDirectBufferCount(ctx);
            const auto bufferSize = CopyIoBufferSize(ctx.ioMode, ctx.size);
            FftsMappedHostCopyBuffer srcBuffer{device, bufferSize, bufferCount,
                                               ctx.hostRegisterMode};
            DeviceCopyBuffer dstBuffer{device, bufferSize, bufferCount};
            InitializeFftsDirectHostPatternedBuffer(srcBuffer);
            ResetFftsDirectDeviceBuffer(dstBuffer);

            FftsDirectH2DCopyInstance instance{ctx.iter, false, ctx.frags,
                                               FftsDirectStreamCount(ctx), ctx.ioMode,
                                               ctx.submitMode, ctx.streamStartGate,
                                               ctx.streamSyncMode};
            auto childResult = instance.DoCopy(&srcBuffer, &dstBuffer, observer);
            ValidateFftsDirectDeviceBufferIfEnabled(dstBuffer, validationEnabled);
            return childResult;
        }));
    PrintFftsDirectValidationPassIfEnabled(*this, validationEnabled);
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(
    AllODirectHost2AllDeviceFftsDirectH2DCase,
    "all_odirect_host_to_all_device_ffts_direct_h2d",
    "copy all UCM O_DIRECT style mmap mapped host buffers to all device buffers with "
    "ffts direct h2d",
    ctx)
{
    CopyResult result;
    const bool validationEnabled = FftsDirectValidationEnabled();
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, "acl::odirect_mmap::all", "acl::device::all", FftsDirectMethodName(ctx),
        [&](size_t device, CopyIterationObserver* observer) {
            const auto bufferCount = FftsDirectBufferCount(ctx);
            const auto bufferSize = CopyIoBufferSize(ctx.ioMode, ctx.size);
            FftsODirectMappedHostCopyBuffer srcBuffer{device, bufferSize, bufferCount,
                                                      ctx.hostRegisterMode};
            DeviceCopyBuffer dstBuffer{device, bufferSize, bufferCount};
            InitializeFftsDirectHostPatternedBuffer(srcBuffer);
            ResetFftsDirectDeviceBuffer(dstBuffer);

            FftsDirectH2DCopyInstance instance{ctx.iter, false, ctx.frags,
                                               FftsDirectStreamCount(ctx), ctx.ioMode,
                                               ctx.submitMode, ctx.streamStartGate,
                                               ctx.streamSyncMode};
            auto childResult = instance.DoCopy(&srcBuffer, &dstBuffer, observer);
            ValidateFftsDirectDeviceBufferIfEnabled(dstBuffer, validationEnabled);
            return childResult;
        }));
    PrintFftsDirectValidationPassIfEnabled(*this, validationEnabled);
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(
    OneShareHost2AllDeviceFftsDirectH2DCase, "one_share_host_to_all_device_ffts_direct_h2d",
    "copy one shared mapped host buffer to all device buffers with ffts direct h2d using fork submit",
    ctx)
{
    CopyResult result;
    const bool validationEnabled = FftsDirectValidationEnabled();
    const auto bufferCount = FftsDirectBufferCount(ctx);
    const auto bufferSize = CopyIoBufferSize(ctx.ioMode, ctx.size);
    FftsMappedSharedHostRegion srcRegion{"one_share_host_to_all_device_ffts_direct_h2d", 0,
                                         bufferSize, bufferCount, ctx.shmNumaNodes};
    InitializeFftsDirectHostPatternedBuffer(srcRegion);
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, ctx.ioMode == CopyIoMode::GLM51 ? "acl::shm::glm5.1" : srcRegion.Name(),
        "acl::device::all", FftsDirectMethodName(ctx),
        [&](size_t device, CopyIterationObserver* observer) {
            FftsMappedSharedHostCopyBuffer srcBuffer{
                srcRegion.ShmName(), srcRegion.MappedBytes(), device, bufferSize,
                bufferCount,         ctx.hostRegisterMode};
            DeviceCopyBuffer dstBuffer{device, bufferSize, bufferCount};
            ResetFftsDirectDeviceBuffer(dstBuffer);

            FftsDirectH2DCopyInstance instance{ctx.iter, false, ctx.frags,
                                               FftsDirectStreamCount(ctx), ctx.ioMode,
                                               ctx.submitMode, ctx.streamStartGate,
                                               ctx.streamSyncMode};
            auto childResult = instance.DoCopy(&srcBuffer, &dstBuffer, observer);
            ValidateFftsDirectDeviceBufferIfEnabled(dstBuffer, validationEnabled);
            return childResult;
        }));
    PrintFftsDirectValidationPassIfEnabled(*this, validationEnabled);
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(
    RankStripedHost2AllDeviceFftsDirectH2DCase,
    "rank_striped_host_to_all_device_ffts_direct_h2d",
    "copy per-rank shared mapped host segments to all devices with rotated ffts direct h2d",
    ctx)
{
    ASSERT(ctx.nDevice > 0);
    ASSERT(ctx.num > 0);
    const bool validationEnabled = FftsDirectValidationEnabled();
    const auto bufferCount = FftsDirectBufferCount(ctx);
    const auto bufferSize = CopyIoBufferSize(ctx.ioMode, ctx.size);
    const auto taskFrags =
        ctx.ioMode == CopyIoMode::GLM51 ? size_t{1} : (ctx.frags == 0 ? bufferCount : ctx.frags);
    ASSERT(taskFrags > 0);
    ASSERT(bufferCount % taskFrags == 0);
    const auto taskCount = bufferCount / taskFrags;
    ASSERT(bufferCount % ctx.nDevice == 0);
    ASSERT(taskCount % ctx.nDevice == 0);
    const auto tasksPerSegment = taskCount / ctx.nDevice;
    RankStripedSharedHostSet srcSet{"rank_striped_host_to_all_device_ffts_direct_h2d",
                                    ctx.nDevice};

    CopyResult result;
    result.Push(ascend_copy::RunForkedCopyBatchWithSync(
        ctx,
        ctx.ioMode == CopyIoMode::GLM51 ? "acl::rank_striped_shm_mapped::glm5.1"
                                        : "acl::rank_striped_shm_mapped::all",
        "acl::device::all", FftsDirectMethodName(ctx) + "-RANK-STRIPED",
        [&](size_t device, CopyIterationObserver* observer,
            ascend_copy::ForkProcessSync* processSync) {
            FftsRankStripedMappedSharedHostCopyBuffer srcBuffer{
                srcSet.ShmNames(),
                device,
                bufferSize,
                bufferCount,
                [processSync, device]() { processSync->SetupBarrier(device, 0); },
                InitializeFftsDirectHostPatternedRange,
                ctx.hostRegisterMode,
                ctx.shmNumaNodes};
            DeviceCopyBuffer dstBuffer{device, bufferSize, bufferCount};
            ResetFftsDirectDeviceBuffer(dstBuffer);

            const auto taskOrderOffset = device * tasksPerSegment;
            FftsDirectH2DCopyInstance instance{
                ctx.iter, false, ctx.frags, FftsDirectStreamCount(ctx), ctx.ioMode,
                ctx.submitMode, ctx.streamStartGate, ctx.streamSyncMode, taskOrderOffset};
            auto childResult = instance.DoCopy(&srcBuffer, &dstBuffer, observer);
            ValidateFftsDirectDeviceBufferIfEnabled(dstBuffer, validationEnabled);
            return childResult;
        }));
    PrintFftsDirectValidationPassIfEnabled(*this, validationEnabled);
    result.Show("[[ " + Key() + " ]] " + Brief());
}
