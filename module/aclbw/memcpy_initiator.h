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
#ifndef ACLBW_MEMCPY_INITIATOR_H
#define ACLBW_MEMCPY_INITIATOR_H

#include "hcomm_roce_h2d_session.h"
#include "memory_buffer.h"

class MemcpyInitiator {
public:
    virtual ~MemcpyInitiator() = default;
    virtual void Copy(void* src, void* dst, size_t size, aclrtStream stream) const = 0;
    virtual void FinalizeIteration(aclrtStream stream) const { (void)stream; }
    virtual void Copy(const MemoryBuffer& src, const MemoryBuffer& dst, aclrtStream stream) const
    {
        for (size_t i = 0; i < src.Number(); ++i) { Copy(src[i], dst[i], src.Size(), stream); }
    }
};

class Host2DeviceCEMemcpyInitiator : public MemcpyInitiator {
public:
    void Copy(void* src, void* dst, size_t size, aclrtStream stream) const override
    {
        ACLBW_ASCEND_ASSERT(
            aclrtMemcpyAsync(dst, size, src, size, ACL_MEMCPY_HOST_TO_DEVICE, stream));
    }
};

class Device2HostCEMemcpyInitiator : public MemcpyInitiator {
public:
    void Copy(void* src, void* dst, size_t size, aclrtStream stream) const override
    {
        ACLBW_ASCEND_ASSERT(
            aclrtMemcpyAsync(dst, size, src, size, ACL_MEMCPY_DEVICE_TO_HOST, stream));
    }
};

class Host2DeviceHcommRoceMemcpyInitiator : public MemcpyInitiator {
public:
    explicit Host2DeviceHcommRoceMemcpyInitiator(const HcommRoceH2dSession& session)
        : session_(session)
    {
    }

    void Copy(void* src, void* dst, size_t size, aclrtStream stream) const override
    {
        (void)stream;
        session_.Write(src, dst, size);
    }

    void FinalizeIteration(aclrtStream stream) const override
    {
        (void)stream;
        session_.Fence();
    }

private:
    const HcommRoceH2dSession& session_;
};

#endif  // ACLBW_MEMCPY_INITIATOR_H
