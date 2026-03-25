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
#ifndef GDRBW_MEMCPY_INITIATOR_H
#define GDRBW_MEMCPY_INITIATOR_H

#include "memory_buffer.h"

struct PendingTransfer {
    int32_t deviceId = -1;
    uint64_t lastWorkRequestId = 0;
};

class MemcpyInitiator {
public:
    virtual ~MemcpyInitiator() = default;
    virtual PendingTransfer Submit(const MemoryBuffer& src, const MemoryBuffer& dst) const = 0;
};

class Host2DeviceRDMAMemcpyInitiator : public MemcpyInitiator {
public:
    PendingTransfer Submit(const MemoryBuffer& src, const MemoryBuffer& dst) const override
    {
        GDRBW_ASSERT(src.Size() == dst.Size());
        GDRBW_ASSERT(src.Number() == dst.Number());
        GDRBW_ASSERT(src.HasMR(dst.DeviceId()));
        GDRBW_ASSERT(dst.HasMR());

        auto& channel = ChannelManager::Instance().Get(dst.DeviceId());
        PendingTransfer pending = {.deviceId = dst.DeviceId(), .lastWorkRequestId = 0};
        for (size_t i = 0; i < src.Number(); ++i) {
            pending.lastWorkRequestId =
                channel.SubmitWrite(src.Address(i), src.LKey(dst.DeviceId()), dst.Address(i),
                                    dst.RKey(), src.Size());
        }
        return pending;
    }
};

#endif  // GDRBW_MEMCPY_INITIATOR_H
