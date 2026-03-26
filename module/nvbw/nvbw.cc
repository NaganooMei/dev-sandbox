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
#include "memcpy_case.h"

int main(int argc, char const* argv[])
{
    auto& param = MemcpyParameterSet::Instance();
    param.deviceNumber = 8;
    param.bufferSize = 16 * 1024;
    param.bufferNumber = 512;
    param.iterations = 128;

    std::vector<MemcpyCase*> testcases = {
        new HostToDeviceMemcpyCase(),
        new Host0ToDeviceMemcpyCase(),
        new HostToAllDeviceMemcpyCase(),
        new AllHostToAllDeviceMemcpyCase(),
        new MmapToDeviceMemcpyCase(),
        new Mmap0ToDeviceMemcpyCase(),
        new MmapToAllDeviceMemcpyCase(),
    };

    for (auto& test : testcases) {
        fmt::println("<--- {} --->", test->Key());
        test->Run();
        delete test;
    }
    return 0;
}
