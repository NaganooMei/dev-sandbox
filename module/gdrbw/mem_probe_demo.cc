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
#include <cuda_runtime.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <string>

#include <fmt/format.h>

#include "error_handle.h"
#include "rdma_channel.h"

namespace {

constexpr int32_t kDeviceId = 0;
constexpr size_t kBytes = 1 * 1024 * 1024;
constexpr const char* kNicName = "mlx5_0";

class ScopedHostMemory {
public:
    using Releaser = std::function<void(void*)>;

    ScopedHostMemory(void* ptr, size_t bytes, Releaser releaser)
        : ptr_(ptr), bytes_(bytes), releaser_(std::move(releaser))
    {
    }

    ~ScopedHostMemory()
    {
        if (ptr_ != nullptr && releaser_) { releaser_(ptr_); }
    }

    ScopedHostMemory(const ScopedHostMemory&) = delete;
    ScopedHostMemory& operator=(const ScopedHostMemory&) = delete;

    void* Data() const { return ptr_; }
    size_t Size() const { return bytes_; }

private:
    void* ptr_ = nullptr;
    size_t bytes_ = 0;
    Releaser releaser_;
};

class ScopedDeviceMemory {
public:
    ScopedDeviceMemory(int32_t deviceId, void* ptr, size_t bytes)
        : deviceId_(deviceId), ptr_(ptr), bytes_(bytes)
    {
    }

    ~ScopedDeviceMemory()
    {
        if (ptr_ != nullptr) {
            (void)cudaSetDevice(deviceId_);
            (void)cudaFree(ptr_);
        }
    }

    ScopedDeviceMemory(const ScopedDeviceMemory&) = delete;
    ScopedDeviceMemory& operator=(const ScopedDeviceMemory&) = delete;

    void* Data() const { return ptr_; }
    size_t Size() const { return bytes_; }

private:
    int32_t deviceId_ = 0;
    void* ptr_ = nullptr;
    size_t bytes_ = 0;
};

void PrintBanner(const std::string& name)
{
    fmt::println("\n=== {} ===", name);
}

void ProbeHostCase(RDMAChannel& channel, const std::string& name,
                   const std::function<ScopedHostMemory()>& factory)
{
    PrintBanner(name);
    try {
        auto memory = factory();
        std::memset(memory.Data(), 0x3C, memory.Size());
        fmt::println("buffer={}, bytes={}", fmt::ptr(memory.Data()), memory.Size());
        auto* mr = channel.RegisterHostMemory(memory.Data(), memory.Size());
        fmt::println("result=success mr={} lkey={} rkey={}", fmt::ptr(mr), mr->lkey, mr->rkey);
        ibv_dereg_mr(mr);
    } catch (const std::exception& ex) {
        fmt::println("result=failed reason={}", ex.what());
    }
}

void ProbeDeviceCase(RDMAChannel& channel, const std::string& name,
                     const std::function<ScopedDeviceMemory()>& factory)
{
    PrintBanner(name);
    try {
        auto memory = factory();
        fmt::println("buffer={}, bytes={}", fmt::ptr(memory.Data()), memory.Size());
        auto* mr = channel.RegisterGpuMemory(memory.Data(), memory.Size());
        fmt::println("result=success mr={} lkey={} rkey={}", fmt::ptr(mr), mr->lkey, mr->rkey);
        ibv_dereg_mr(mr);
    } catch (const std::exception& ex) {
        fmt::println("result=failed reason={}", ex.what());
    }
}

}  // namespace

int main(int argc, char const* argv[])
{
    (void)argc;
    (void)argv;

    auto cleanup = []() noexcept { ChannelManager::Instance().Shutdown(); };

    try {
        GDRBW_CUDA_ASSERT(cudaSetDevice(kDeviceId));

        RDMAChannelConfig config;
        config.cqDepth = 1024;
        config.qpSendWr = 1024;
        config.qpRecvWr = 1;
        ChannelManager::Instance().Initialize(1, {kNicName}, config);
        auto& channel = ChannelManager::Instance().Get(kDeviceId);

        ProbeHostCase(channel, "host_malloc", [] {
            void* ptr = std::malloc(kBytes);
            GDRBW_ASSERT(ptr != nullptr);
            return ScopedHostMemory(ptr, kBytes, [](void* p) { std::free(p); });
        });

        ProbeHostCase(channel, "host_posix_memalign_4k", [] {
            void* ptr = nullptr;
            GDRBW_ASSERT(posix_memalign(&ptr, 4096, kBytes) == 0);
            return ScopedHostMemory(ptr, kBytes, [](void* p) { std::free(p); });
        });

        ProbeHostCase(channel, "host_mmap_anon", [] {
            void* ptr =
                mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            GDRBW_ASSERT(ptr != MAP_FAILED);
            return ScopedHostMemory(ptr, kBytes, [](void* p) { munmap(p, kBytes); });
        });

        ProbeHostCase(channel, "host_cudaMallocHost", [] {
            void* ptr = nullptr;
            GDRBW_CUDA_ASSERT(cudaMallocHost(&ptr, kBytes));
            return ScopedHostMemory(ptr, kBytes, [](void* p) {
                (void)cudaFreeHost(p);
            });
        });

        ProbeDeviceCase(channel, "device_cudaMalloc", [] {
            void* ptr = nullptr;
            GDRBW_CUDA_ASSERT(cudaSetDevice(kDeviceId));
            GDRBW_CUDA_ASSERT(cudaMalloc(&ptr, kBytes));
            return ScopedDeviceMemory(kDeviceId, ptr, kBytes);
        });

        cleanup();
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "gdrbw_mem_probe_demo failed: %s\n", ex.what());
        cleanup();
        return 1;
    }
}
