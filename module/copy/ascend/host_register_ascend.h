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
#ifndef HOST_REGISTER_ASCEND_H
#define HOST_REGISTER_ASCEND_H

#include <cstddef>
#include <cstdint>
#include "copy_options.h"
#include "error_handle_ascend.h"

inline void RegisterCopyHostBuffer(void* host, size_t bytes, CopyHostRegisterMode mode,
                                   uint32_t v2Flags, void** device = nullptr)
{
    if (mode == CopyHostRegisterMode::V1) {
        // V1 returns the mapped address directly, as in UCM's default registration path.
        void* mapped = nullptr;
        ASCEND_ASSERT(aclrtHostRegister(host, bytes, ACL_HOST_REGISTER_MAPPED, &mapped));
        if (device != nullptr) { *device = mapped; }
    } else {
        ASCEND_ASSERT(aclrtHostRegisterV2(host, bytes, v2Flags));
        if (device != nullptr) { ASCEND_ASSERT(aclrtHostGetDevicePointer(host, device, 0)); }
    }
}

#endif  // HOST_REGISTER_ASCEND_H
