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
#ifndef NVBW_MEMCPY_INITIATOR_H
#define NVBW_MEMCPY_INITIATOR_H

#include "memory_buffer.h"

class MemcpyInitiator {
public:
    virtual ~MemcpyInitiator() = default;
    virtual int32_t ContextDevice(const MemoryBuffer& src, const MemoryBuffer& dst) const = 0;
    virtual void Copy(const MemoryBuffer& src, const MemoryBuffer& dst,
                      cudaStream_t stream) const = 0;
};

class Host2DeviceCEMemcpyInitiator : public MemcpyInitiator {
public:
    int32_t ContextDevice(const MemoryBuffer& src, const MemoryBuffer& dst) const override
    {
        (void)src;
        return dst.DeviceId();
    }

    void Copy(const MemoryBuffer& src, const MemoryBuffer& dst, cudaStream_t stream) const override
    {
        NVBW_ASSERT(src.Size() == dst.Size());
        NVBW_ASSERT(src.Number() == dst.Number());
        for (size_t i = 0; i < src.Number(); ++i) {
            NVBW_CUDA_ASSERT(
                cudaMemcpyAsync(dst[i], src[i], src.Size(), cudaMemcpyHostToDevice, stream));
        }
    }
};

class Device2HostCEMemcpyInitiator : public MemcpyInitiator {
public:
    int32_t ContextDevice(const MemoryBuffer& src, const MemoryBuffer& dst) const override
    {
        (void)dst;
        return src.DeviceId();
    }

    void Copy(const MemoryBuffer& src, const MemoryBuffer& dst, cudaStream_t stream) const override
    {
        NVBW_ASSERT(src.Size() == dst.Size());
        NVBW_ASSERT(src.Number() == dst.Number());
        for (size_t i = 0; i < src.Number(); ++i) {
            NVBW_CUDA_ASSERT(
                cudaMemcpyAsync(dst[i], src[i], src.Size(), cudaMemcpyDeviceToHost, stream));
        }
    }
};

#endif  // NVBW_MEMCPY_INITIATOR_H
