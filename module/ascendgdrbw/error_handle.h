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
#ifndef ASCENDGDRBW_ERROR_HANDLE_H
#define ASCENDGDRBW_ERROR_HANDLE_H

#include <acl/acl.h>
#include <infiniband/verbs.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

[[noreturn]] inline void AscendGdrbwThrowError(const std::string& message)
{
    throw std::runtime_error(message);
}

inline std::string AscendGdrbwLocation(const char* expression, const char* file, int line,
                                       const char* function)
{
    std::stringstream stream;
    stream << "expression " << expression << " failed at " << function << " : " << file << ":"
           << line;
    return stream.str();
}

inline std::string AscendGdrbwErrnoMessage(const char* expression, int errorCode, const char* file,
                                           int line, const char* function)
{
    std::stringstream stream;
    stream << "[" << errorCode << "] " << std::strerror(errorCode) << " in "
           << AscendGdrbwLocation(expression, file, line, function);
    return stream.str();
}

inline std::string AscendGdrbwAclMessage(aclError errorCode, const char* expression,
                                         const char* file, int line, const char* function)
{
    std::stringstream stream;
    stream << "[" << errorCode << "] " << aclGetRecentErrMsg() << " in "
           << AscendGdrbwLocation(expression, file, line, function);
    return stream.str();
}

inline std::string AscendGdrbwWorkCompletionMessage(ibv_wc_status status, uint64_t workRequestId,
                                                    const char* file, int line,
                                                    const char* function)
{
    std::stringstream stream;
    stream << "[WC " << workRequestId << "] " << ibv_wc_status_str(status) << " at " << function
           << " : " << file << ":" << line;
    return stream.str();
}

#define ASCENDGDRBW_ASSERT(expr)                                                            \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            AscendGdrbwThrowError(                                                           \
                AscendGdrbwLocation(#expr, __FILE__, __LINE__, __PRETTY_FUNCTION__));       \
        }                                                                                    \
    } while (0)

#define ASCENDGDRBW_ERRNO_ASSERT(expr)                                                       \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            AscendGdrbwThrowError(AscendGdrbwErrnoMessage(#expr, errno, __FILE__, __LINE__, \
                                                          __PRETTY_FUNCTION__));             \
        }                                                                                    \
    } while (0)

#define ASCENDGDRBW_IBV_ASSERT(expr)                                                         \
    do {                                                                                     \
        const int __ibvRc = (expr);                                                          \
        if (__ibvRc != 0) {                                                                  \
            AscendGdrbwThrowError(AscendGdrbwErrnoMessage(#expr, __ibvRc, __FILE__,         \
                                                          __LINE__, __PRETTY_FUNCTION__));   \
        }                                                                                    \
    } while (0)

#define ASCENDGDRBW_ASCEND_ASSERT(expr)                                                      \
    do {                                                                                     \
        const auto __aclErr = (expr);                                                        \
        if (__aclErr != ACL_SUCCESS) {                                                       \
            AscendGdrbwThrowError(AscendGdrbwAclMessage(__aclErr, #expr, __FILE__,          \
                                                        __LINE__, __PRETTY_FUNCTION__));     \
        }                                                                                    \
    } while (0)

#define ASCENDGDRBW_WC_ASSERT(status, wrId)                                                  \
    do {                                                                                     \
        if ((status) != IBV_WC_SUCCESS) {                                                    \
            AscendGdrbwThrowError(AscendGdrbwWorkCompletionMessage(                          \
                (status), (wrId), __FILE__, __LINE__, __PRETTY_FUNCTION__));                \
        }                                                                                    \
    } while (0)

#endif  // ASCENDGDRBW_ERROR_HANDLE_H
