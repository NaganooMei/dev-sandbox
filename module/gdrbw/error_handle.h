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
#ifndef GDRBW_ERROR_HANDLE_H
#define GDRBW_ERROR_HANDLE_H

#include <cuda_runtime.h>
#include <infiniband/verbs.h>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

[[noreturn]] inline void GdrbwThrowError(const std::string& message)
{
    throw std::runtime_error(message);
}

inline std::string GdrbwLocation(const char* expression, const char* file, int line,
                                 const char* function)
{
    std::stringstream stream;
    stream << "expression " << expression << " failed at " << function << " : " << file << ":"
           << line;
    return stream.str();
}

inline std::string GdrbwErrnoMessage(const char* expression, int errorCode, const char* file,
                                     int line, const char* function)
{
    std::stringstream stream;
    stream << "[" << errorCode << "] " << std::strerror(errorCode) << " in "
           << GdrbwLocation(expression, file, line, function);
    return stream.str();
}

inline std::string GdrbwCudaMessage(cudaError_t errorCode, const char* expression, const char* file,
                                    int line, const char* function)
{
    std::stringstream stream;
    stream << "[" << cudaGetErrorName(errorCode) << "] " << cudaGetErrorString(errorCode)
           << " in " << GdrbwLocation(expression, file, line, function);
    return stream.str();
}

inline std::string GdrbwWorkCompletionMessage(ibv_wc_status status, uint64_t workRequestId,
                                              const char* file, int line, const char* function)
{
    std::stringstream stream;
    stream << "[WC " << workRequestId << "] " << ibv_wc_status_str(status) << " at " << function
           << " : " << file << ":" << line;
    return stream.str();
}

#define GDRBW_ASSERT(expr)                                                          \
    do {                                                                            \
        if (!(expr)) {                                                              \
            GdrbwThrowError(GdrbwLocation(#expr, __FILE__, __LINE__, __PRETTY_FUNCTION__)); \
        }                                                                           \
    } while (0)

#define GDRBW_ERRNO_ASSERT(expr)                                                          \
    do {                                                                                  \
        if (!(expr)) {                                                                    \
            GdrbwThrowError(                                                              \
                GdrbwErrnoMessage(#expr, errno, __FILE__, __LINE__, __PRETTY_FUNCTION__)); \
        }                                                                                 \
    } while (0)

#define GDRBW_IBV_ASSERT(expr)                                                            \
    do {                                                                                  \
        const int __ibvRc = (expr);                                                       \
        if (__ibvRc != 0) {                                                               \
            GdrbwThrowError(                                                              \
                GdrbwErrnoMessage(#expr, __ibvRc, __FILE__, __LINE__, __PRETTY_FUNCTION__)); \
        }                                                                                 \
    } while (0)

#define GDRBW_CUDA_ASSERT(expr)                                                           \
    do {                                                                                  \
        const auto __cudaErr = (expr);                                                    \
        if (__cudaErr != cudaSuccess) {                                                   \
            GdrbwThrowError(                                                              \
                GdrbwCudaMessage(__cudaErr, #expr, __FILE__, __LINE__, __PRETTY_FUNCTION__)); \
        }                                                                                 \
    } while (0)

#define GDRBW_WC_ASSERT(status, wrId)                                                           \
    do {                                                                                        \
        if ((status) != IBV_WC_SUCCESS) {                                                       \
            GdrbwThrowError(                                                                    \
                GdrbwWorkCompletionMessage((status), (wrId), __FILE__, __LINE__, __PRETTY_FUNCTION__)); \
        }                                                                                       \
    } while (0)

#endif  // GDRBW_ERROR_HANDLE_H
