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
#include <vector>
#include "copy_buffer_ascend.h"
#include "copy_case.h"
#include "copy_instance_ascend.h"
#include "forked_copy_runner_ascend.h"

namespace {

size_t CEMultiStreamCount(const CopyCase::Context& ctx)
{
    constexpr size_t defaultStreamCount = 48;
    return ctx.streams == 0 ? defaultStreamCount : ctx.streams;
}

size_t CEMultiStreamBufferSize(const CopyCase::Context& ctx)
{
    return CopyIoBufferSize(ctx.ioMode, ctx.size);
}

std::string CEMultiStreamMethodName(const CopyCase::Context& ctx, bool forkSubmit)
{
    return "CE-MS" + std::to_string(CEMultiStreamCount(ctx)) +
           (forkSubmit ? "-FORK" : "") + CopySubmitModeSuffix(ctx.submitMode) +
           (ctx.ioMode == CopyIoMode::GLM51 ? "-GLM51" : "");
}

}  // namespace

DEFINE_COPY_CASE(Host2DeviceCECase, "host_to_device_ce",
                 "memcpy from host to device with ce one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(Host2DeviceBatchCECase, "host_to_device_batch_ce",
                 "memcpy from host to device with batch ce one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DBatchCECopyInstance instance{ctx.iter, false, device};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(OneShareHost2AllDeviceCECase, "one_share_host_to_all_device_ce",
                            "memcpy from one shared host to all device with ce using fork submit",
                            ctx)
{
    CopyResult result;
    SharedHostRegion srcRegion{"one_share_host_to_all_device_ce", 0, ctx.size, ctx.num};
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, srcRegion.Name(), "acl::device::all", "CE-FORK",
        [&](size_t device, CopyIterationObserver* observer) {
            SharedHostCopyBuffer srcBuffer{srcRegion.ShmName(), device, ctx.size, ctx.num};
            DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            H2DCECopyInstance instance{ctx.iter, false};
            return instance.DoCopy(&srcBuffer, &dstBuffer, observer);
        }));
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(
    OneShareHost2AllDeviceCEPerDeviceCase, "one_share_host_to_all_device_ce_per_device",
    "memcpy from one shared host to all device with ce using fork submit and per-device results",
    ctx)
{
    CopyResult result;
    SharedHostRegion srcRegion{"one_share_host_to_all_device_ce_per_device", 0, ctx.size,
                               ctx.num};
    auto childResults = ascend_copy::RunForkedCopyBatchPerDevice(
        ctx, [&](size_t device, CopyIterationObserver* observer) {
            SharedHostCopyBuffer srcBuffer{srcRegion.ShmName(), device, ctx.size, ctx.num};
            DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            H2DCECopyInstance instance{ctx.iter, false};
            return instance.DoCopy(&srcBuffer, &dstBuffer, observer);
        });
    for (auto& childResult : childResults) {
        childResult.src = srcRegion.Name();
        result.Push(std::move(childResult));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(OneHost2AllDeviceCECase, "one_host_to_all_device_ce",
                 "memcpy from one host to all device with ce", ctx)
{
    CopyResult result;
    HostCopyBuffer srcBuffer{0, ctx.size, ctx.num};
    for (size_t device = 0; device < ctx.nDevice; device++) {
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(OneHost2AllDeviceCEBatchCase, "one_host_to_all_device_ce_batch",
                 "memcpy from one host to all device with ce in one batch", ctx)
{
    CopyResult result;
    HostCopyBuffer srcBuffer{0, ctx.size, ctx.num};
    std::vector<const CopyBuffer*> srcBuffers(ctx.nDevice, &srcBuffer);
    std::vector<const CopyBuffer*> dstBuffers(ctx.nDevice);
    for (size_t device = 0; device < ctx.nDevice; device++) {
        dstBuffers[device] = new DeviceCopyBuffer{device, ctx.size, ctx.num};
    }
    H2DCECopyInstance instance{ctx.iter, false};
    result.Push(instance.DoCopyBatch(srcBuffers, dstBuffers));
    for (size_t device = 0; device < ctx.nDevice; device++) { delete dstBuffers[device]; }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(HugeShm2DeviceCECase, "huge_shm_to_device_ce",
                 "memcpy from HugeTLB shared host memory to device with ce one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HugeSharedCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(OneHugeShm2AllDeviceCECase, "one_huge_shm_to_all_device_ce",
                            "memcpy from one HugeTLB shared host memory to all device with ce using fork submit",
                            ctx)
{
    CopyResult result;
    HugeSharedRegion srcRegion{"one_huge_shm_to_all_device_ce", 0, ctx.size, ctx.num};
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, srcRegion.Name(), "acl::device::all", "CE-FORK",
        [&](size_t device, CopyIterationObserver* observer) {
            HugeSharedCopyBuffer srcBuffer{srcRegion.Fd(), srcRegion.MappedBytes(), device,
                                           ctx.size, ctx.num};
            DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            H2DCECopyInstance instance{ctx.iter, false};
            return instance.DoCopy(&srcBuffer, &dstBuffer, observer);
        }));
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(
    OneHugeShm2AllDeviceCEPerDeviceCase, "one_huge_shm_to_all_device_ce_per_device",
    "memcpy from one HugeTLB shared host memory to all device with ce using fork submit and per-device results",
    ctx)
{
    CopyResult result;
    HugeSharedRegion srcRegion{"one_huge_shm_to_all_device_ce_per_device", 0, ctx.size,
                               ctx.num};
    auto childResults = ascend_copy::RunForkedCopyBatchPerDevice(
        ctx, [&](size_t device, CopyIterationObserver* observer) {
            HugeSharedCopyBuffer srcBuffer{srcRegion.Fd(), srcRegion.MappedBytes(), device,
                                           ctx.size, ctx.num};
            DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            H2DCECopyInstance instance{ctx.iter, false};
            return instance.DoCopy(&srcBuffer, &dstBuffer, observer);
        });
    for (auto& childResult : childResults) {
        childResult.src = srcRegion.Name();
        result.Push(std::move(childResult));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(AllHost2AllDeviceCECase, "all_host_to_all_device_ce",
                            "memcpy from all host to all device with ce using fork submit", ctx)
{
    CopyResult result;
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, "acl::host::all", "acl::device::all", "CE-FORK",
        [&](size_t device, CopyIterationObserver* observer) {
            HostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
            DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
            H2DCECopyInstance instance{ctx.iter, false};
            return instance.DoCopy(&srcBuffer, &dstBuffer, observer);
        }));
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(Device2DeviceCECase, "device_to_device_ce",
                 "memcpy from device to device with ce one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        DeviceCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        D2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(OneDevice2AllDeviceCECase, "one_device_to_all_device_ce",
                 "memcpy from one device to all device with ce", ctx)
{
    CopyResult result;
    DeviceCopyBuffer srcBuffer{0, ctx.size, ctx.num};
    for (size_t device = 0; device < ctx.nDevice; device++) {
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        D2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(Anonymous2DeviceCECase, "anonymous_to_device_ce",
                 "memcpy from anonymous to device one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        AnonymousCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(ODirectHost2DeviceCECase, "odirect_to_device_ce",
                 "memcpy from UCM O_DIRECT style host to device one by one", ctx)
{
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        ODirectHostCopyBuffer srcBuffer{device, ctx.size, ctx.num};
        DeviceCopyBuffer dstBuffer{device, ctx.size, ctx.num};
        H2DCECopyInstance instance{ctx.iter, false};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(Anonymous2DeviceCEMultiStreamCase, "anonymous_to_device_ce_multi_stream",
                 "memcpy from anonymous to device with ce using multi stream one by one", ctx)
{
    const auto streamCount = CEMultiStreamCount(ctx);
    const auto bufferSize = CEMultiStreamBufferSize(ctx);
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        AnonymousCopyBuffer srcBuffer{device, bufferSize, ctx.num};
        DeviceCopyBuffer dstBuffer{device, bufferSize, ctx.num};
        H2DCEMultiStreamCopyInstance instance{ctx.iter, false, streamCount, ctx.ioMode,
                                              ctx.submitMode};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(Host2DeviceCEMultiStreamCase, "host_to_device_ce_multi_stream",
                 "memcpy from host to device with ce using multi stream one by one", ctx)
{
    const auto streamCount = CEMultiStreamCount(ctx);
    const auto bufferSize = CEMultiStreamBufferSize(ctx);
    CopyResult result;
    for (size_t device = 0; device < ctx.nDevice; device++) {
        HostCopyBuffer srcBuffer{device, bufferSize, ctx.num};
        DeviceCopyBuffer dstBuffer{device, bufferSize, ctx.num};
        H2DCEMultiStreamCopyInstance instance{ctx.iter, false, streamCount, ctx.ioMode,
                                              ctx.submitMode};
        result.Push(instance.DoCopy(&srcBuffer, &dstBuffer));
    }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(
    OneShareHost2AllDeviceCEMultiStreamCase, "one_share_host_to_all_device_ce_multi_stream",
    "memcpy from one shared host to all device with ce using multi stream and fork submit", ctx)
{
    const auto streamCount = CEMultiStreamCount(ctx);
    const auto bufferSize = CEMultiStreamBufferSize(ctx);
    CopyResult result;
    SharedHostRegion srcRegion{"one_share_host_to_all_device_ce_multi_stream", 0, bufferSize,
                               ctx.num};
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, ctx.ioMode == CopyIoMode::GLM51 ? "acl::shm::glm5.1" : srcRegion.Name(),
        "acl::device::all", CEMultiStreamMethodName(ctx, true),
        [&](size_t device, CopyIterationObserver* observer) {
            SharedHostCopyBuffer srcBuffer{srcRegion.ShmName(), device, bufferSize, ctx.num};
            DeviceCopyBuffer dstBuffer{device, bufferSize, ctx.num};
            H2DCEMultiStreamCopyInstance instance{ctx.iter, false, streamCount, ctx.ioMode,
                                                  ctx.submitMode};
            return instance.DoCopy(&srcBuffer, &dstBuffer, observer);
        }));
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE(OneHost2AllDeviceCEMultiStreamCase, "one_host_to_all_device_ce_multi_stream",
                 "memcpy from one host to all device with ce using multi stream", ctx)
{
    const auto streamCount = CEMultiStreamCount(ctx);
    const auto bufferSize = CEMultiStreamBufferSize(ctx);
    CopyResult result;
    HostCopyBuffer srcBuffer{0, bufferSize, ctx.num};
    std::vector<const CopyBuffer*> srcBuffers(ctx.nDevice, &srcBuffer);
    std::vector<const CopyBuffer*> dstBuffers(ctx.nDevice);
    for (size_t device = 0; device < ctx.nDevice; device++) {
        dstBuffers[device] = new DeviceCopyBuffer{device, bufferSize, ctx.num};
    }
    H2DCEMultiStreamCopyInstance instance{ctx.iter, false, streamCount, ctx.ioMode,
                                          ctx.submitMode};
    result.Push(instance.DoCopyBatch(srcBuffers, dstBuffers));
    for (size_t device = 0; device < ctx.nDevice; device++) { delete dstBuffers[device]; }
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(
    AllHost2AllDeviceCEMultiStreamCase, "all_host_to_all_device_ce_multi_stream",
    "memcpy from all host to all device with ce using multi stream and fork submit", ctx)
{
    const auto streamCount = CEMultiStreamCount(ctx);
    const auto bufferSize = CEMultiStreamBufferSize(ctx);
    CopyResult result;
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, "acl::host::all", "acl::device::all", CEMultiStreamMethodName(ctx, true),
        [&](size_t device, CopyIterationObserver* observer) {
            HostCopyBuffer srcBuffer{device, bufferSize, ctx.num};
            DeviceCopyBuffer dstBuffer{device, bufferSize, ctx.num};
            H2DCEMultiStreamCopyInstance instance{ctx.iter, false, streamCount, ctx.ioMode,
                                                  ctx.submitMode};
            return instance.DoCopy(&srcBuffer, &dstBuffer, observer);
        }));
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(
    AllAnonymousHost2AllDeviceCEMultiStreamCase,
    "all_anonymous_host_to_all_device_ce_multi_stream",
    "memcpy from all anonymous host to all device with ce using multi stream and fork submit",
    ctx)
{
    const auto streamCount = CEMultiStreamCount(ctx);
    const auto bufferSize = CEMultiStreamBufferSize(ctx);
    CopyResult result;
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, "acl::anon::all", "acl::device::all", CEMultiStreamMethodName(ctx, true),
        [&](size_t device, CopyIterationObserver* observer) {
            AnonymousCopyBuffer srcBuffer{device, bufferSize, ctx.num};
            DeviceCopyBuffer dstBuffer{device, bufferSize, ctx.num};
            H2DCEMultiStreamCopyInstance instance{ctx.iter, false, streamCount, ctx.ioMode,
                                                  ctx.submitMode};
            return instance.DoCopy(&srcBuffer, &dstBuffer, observer);
        }));
    result.Show("[[ " + Key() + " ]] " + Brief());
}

DEFINE_COPY_CASE_NO_RUNTIME(
    AllODirectHost2AllDeviceCEMultiStreamCase,
    "all_odirect_host_to_all_device_ce_multi_stream",
    "memcpy from all UCM O_DIRECT style host to all device with ce using multi stream and fork "
    "submit",
    ctx)
{
    const auto streamCount = CEMultiStreamCount(ctx);
    const auto bufferSize = CEMultiStreamBufferSize(ctx);
    CopyResult result;
    result.Push(ascend_copy::RunForkedCopyBatch(
        ctx, "acl::odirect_mmap::all", "acl::device::all", CEMultiStreamMethodName(ctx, true),
        [&](size_t device, CopyIterationObserver* observer) {
            ODirectHostCopyBuffer srcBuffer{device, bufferSize, ctx.num};
            DeviceCopyBuffer dstBuffer{device, bufferSize, ctx.num};
            H2DCEMultiStreamCopyInstance instance{ctx.iter, false, streamCount, ctx.ioMode,
                                                  ctx.submitMode};
            return instance.DoCopy(&srcBuffer, &dstBuffer, observer);
        }));
    result.Show("[[ " + Key() + " ]] " + Brief());
}
