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
#ifndef FORKED_COPY_RUNNER_ASCEND_H
#define FORKED_COPY_RUNNER_ASCEND_H

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <map>
#include <sched.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "copy_case.h"
#include "copy_result.h"
#include "copy_runtime.h"
#include "error_handle.h"

namespace ascend_copy {

using ForkedChildCopyFn = std::function<CopyResult::Result(size_t device)>;

struct ForkedChildProcess {
    pid_t pid = -1;
    int readFd = -1;
    size_t device = 0;
};

struct ResultWireHeader {
    std::uint64_t srcLen = 0;
    std::uint64_t dstLen = 0;
    std::uint64_t methodLen = 0;
    std::uint64_t size = 0;
    std::uint64_t count = 0;
    std::uint64_t submitCount = 0;
    std::uint64_t copyCount = 0;
};

constexpr const char* kForkBindCpuEnv = "COPY_FORK_BIND_CPU";
constexpr const char* kForkCpuOffsetEnv = "COPY_FORK_CPU_OFFSET";
constexpr const char* kForkCpuStrideEnv = "COPY_FORK_CPU_STRIDE";
constexpr const char* kForkCpuVerboseEnv = "COPY_FORK_CPU_VERBOSE";
constexpr const char* kForkCpuBindScopeEnv = "COPY_FORK_CPU_BIND_SCOPE";

struct NpuChipInfo {
    int npuId = -1;
    int chipId = -1;
};

struct CpuBindingTarget {
    std::vector<int> cpus;
    int numaNode = -1;
    std::string pcieInfo;
    bool numaAware = false;
};

inline bool EnvFlagEnabled(const char* name, bool defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') { return defaultValue; }
    return std::strcmp(value, "0") != 0 && std::strcmp(value, "false") != 0 &&
           std::strcmp(value, "FALSE") != 0 && std::strcmp(value, "off") != 0 &&
           std::strcmp(value, "OFF") != 0;
}

inline size_t EnvUnsigned(const char* name, size_t defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') { return defaultValue; }

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') { return defaultValue; }
    return static_cast<size_t>(parsed);
}

inline std::string EnvString(const char* name, const std::string& defaultValue)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') { return defaultValue; }
    return value;
}

inline bool ParseInt(const std::string& value, int& parsed)
{
    char* end = nullptr;
    errno = 0;
    const auto number = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') { return false; }
    parsed = static_cast<int>(number);
    return true;
}

inline std::string Trim(const std::string& value)
{
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

inline std::string ToLower(std::string value)
{
    for (auto& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

inline std::string RemoveSpaces(const std::string& value)
{
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto ch : value) {
        if (std::isspace(static_cast<unsigned char>(ch)) == 0) { normalized.push_back(ch); }
    }
    return normalized;
}

inline bool StartsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

inline std::vector<int> AllowedCpuList()
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(0, sizeof(mask), &mask) != 0) { return {}; }

    std::vector<int> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &mask)) { cpus.push_back(cpu); }
    }
    return cpus;
}

inline std::string CpuListToString(const std::vector<int>& cpus)
{
    if (cpus.empty()) { return ""; }

    std::vector<int> sorted = cpus;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

    std::ostringstream os;
    auto begin = sorted.front();
    auto end = sorted.front();
    bool first = true;
    for (size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i] == end + 1) {
            end = sorted[i];
            continue;
        }
        if (!first) { os << ","; }
        os << begin;
        if (begin != end) { os << "-" << end; }
        first = false;
        begin = sorted[i];
        end = sorted[i];
    }
    if (!first) { os << ","; }
    os << begin;
    if (begin != end) { os << "-" << end; }
    return os.str();
}

inline std::vector<int> ParseCpuList(const std::string& cpulist)
{
    std::vector<int> cpus;
    std::stringstream ss(cpulist);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = Trim(item);
        if (item.empty()) { continue; }

        const auto dash = item.find('-');
        if (dash == std::string::npos) {
            int cpu = -1;
            if (ParseInt(item, cpu) && cpu >= 0) { cpus.push_back(cpu); }
            continue;
        }

        int begin = -1;
        int end = -1;
        if (!ParseInt(item.substr(0, dash), begin) || !ParseInt(item.substr(dash + 1), end)) {
            continue;
        }
        if (begin > end) { std::swap(begin, end); }
        for (int cpu = begin; cpu <= end; ++cpu) { cpus.push_back(cpu); }
    }
    std::sort(cpus.begin(), cpus.end());
    cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
    return cpus;
}

inline std::vector<int> IntersectCpuLists(const std::vector<int>& lhs,
                                          const std::vector<int>& rhs)
{
    std::vector<int> result;
    std::vector<int> sortedRhs = rhs;
    std::sort(sortedRhs.begin(), sortedRhs.end());
    for (const auto cpu : lhs) {
        if (std::binary_search(sortedRhs.begin(), sortedRhs.end(), cpu)) {
            result.push_back(cpu);
        }
    }
    return result;
}

inline bool ReadFirstLine(const std::string& path, std::string& line)
{
    std::ifstream file(path);
    if (!file.is_open()) { return false; }
    return static_cast<bool>(std::getline(file, line));
}

inline std::string RunCommand(const std::string& command)
{
    std::array<char, 256> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) { return output; }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    pclose(pipe);
    return output;
}

inline std::map<int, NpuChipInfo> QueryNpuChipInfo()
{
    std::map<int, NpuChipInfo> chips;
    const auto output = RunCommand("npu-smi info -m 2>/dev/null");
    std::stringstream ss(output);
    std::string line;
    while (std::getline(ss, line)) {
        std::istringstream fields(line);
        std::string npuIdText;
        std::string chipIdText;
        std::string logicIdText;
        if (!(fields >> npuIdText >> chipIdText >> logicIdText)) { continue; }

        int npuId = -1;
        int chipId = -1;
        int logicId = -1;
        if (!ParseInt(npuIdText, npuId) || !ParseInt(chipIdText, chipId) ||
            !ParseInt(logicIdText, logicId)) {
            continue;
        }
        chips[logicId] = NpuChipInfo{npuId, chipId};
    }
    return chips;
}

inline std::string QueryNpuPcieInfo(const NpuChipInfo& chip)
{
    const auto command = "npu-smi info -t board -i " + std::to_string(chip.npuId) + " -c " +
                         std::to_string(chip.chipId) + " 2>/dev/null";
    const auto output = RunCommand(command);
    std::stringstream ss(output);
    std::string line;
    constexpr const char* kPcieKey = "PCIeBusInfo:";
    while (std::getline(ss, line)) {
        const auto normalized = RemoveSpaces(line);
        if (StartsWith(normalized, kPcieKey)) {
            return ToLower(normalized.substr(std::strlen(kPcieKey)));
        }
    }
    return "";
}

inline int QueryNumaNodeFromPcie(const std::string& pcieInfo)
{
    if (pcieInfo.empty()) { return -1; }

    std::string line;
    const auto path = "/sys/bus/pci/devices/" + pcieInfo + "/numa_node";
    if (!ReadFirstLine(path, line)) { return -1; }

    int numaNode = -1;
    if (!ParseInt(Trim(line), numaNode) || numaNode < 0) { return -1; }
    return numaNode;
}

inline std::vector<int> QueryNumaCpuList(int numaNode, const std::vector<int>& allowedCpus)
{
    if (numaNode < 0) { return {}; }

    std::string cpulist;
    const auto path = "/sys/devices/system/node/node" + std::to_string(numaNode) + "/cpulist";
    if (!ReadFirstLine(path, cpulist)) { return {}; }

    return IntersectCpuLists(ParseCpuList(cpulist), allowedCpus);
}

inline std::vector<CpuBindingTarget> BuildCpuBindingPlan(size_t nDevice,
                                                         const std::vector<int>& allowedCpus)
{
    std::vector<CpuBindingTarget> plan(nDevice);
    if (!EnvFlagEnabled(kForkBindCpuEnv, true) || allowedCpus.empty()) { return plan; }

    const auto bindScope = ToLower(EnvString(kForkCpuBindScopeEnv, "numa"));
    const bool bindSingleCpu = bindScope == "cpu" || bindScope == "core" ||
                               bindScope == "single" || bindScope == "single_cpu";
    const auto offset = EnvUnsigned(kForkCpuOffsetEnv, 0);
    const auto stride = std::max<size_t>(EnvUnsigned(kForkCpuStrideEnv, 1), 1);
    auto chipInfo = QueryNpuChipInfo();
    std::map<int, size_t> numaOrdinals;

    for (size_t device = 0; device < nDevice; ++device) {
        CpuBindingTarget target;
        std::vector<int> candidateCpus = allowedCpus;

        const auto chip = chipInfo.find(static_cast<int>(device));
        if (chip != chipInfo.end()) {
            target.pcieInfo = QueryNpuPcieInfo(chip->second);
            target.numaNode = QueryNumaNodeFromPcie(target.pcieInfo);
            auto numaCpus = QueryNumaCpuList(target.numaNode, allowedCpus);
            if (!numaCpus.empty()) {
                candidateCpus = std::move(numaCpus);
                target.numaAware = true;
            }
        }

        if (bindSingleCpu) {
            const auto ordinal =
                target.numaAware ? numaOrdinals[target.numaNode]++ : static_cast<size_t>(device);
            const auto cpuIndex = (offset + ordinal * stride) % candidateCpus.size();
            target.cpus = {candidateCpus[cpuIndex]};
        } else {
            target.cpus = std::move(candidateCpus);
        }
        plan[device] = std::move(target);
    }

    return plan;
}

inline void BindChildToCpu(size_t device, const CpuBindingTarget& target)
{
    if (!EnvFlagEnabled(kForkBindCpuEnv, true) || target.cpus.empty()) { return; }

    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (const auto cpu : target.cpus) { CPU_SET(cpu, &mask); }

    const auto cpulist = CpuListToString(target.cpus);
    if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
        std::fprintf(stderr, "[fork-copy] failed to bind device %zu child to cpus %s: %s\n",
                     device, cpulist.c_str(), std::strerror(errno));
    } else if (EnvFlagEnabled(kForkCpuVerboseEnv, false)) {
        if (target.numaAware) {
            std::fprintf(stderr,
                         "[fork-copy] device %zu child bound to NUMA node %d cpus %s"
                         " pcie %s\n",
                         device, target.numaNode, cpulist.c_str(), target.pcieInfo.c_str());
        } else {
            std::fprintf(stderr, "[fork-copy] device %zu child bound to fallback cpus %s\n",
                         device, cpulist.c_str());
        }
    }
}

inline bool WriteExact(int fd, const void* data, size_t size)
{
    const auto* cursor = static_cast<const char*>(data);
    while (size > 0) {
        const auto written = write(fd, cursor, size);
        if (written < 0) {
            if (errno == EINTR) { continue; }
            return false;
        }
        if (written == 0) { return false; }
        cursor += written;
        size -= static_cast<size_t>(written);
    }
    return true;
}

inline bool ReadExact(int fd, void* data, size_t size)
{
    auto* cursor = static_cast<char*>(data);
    while (size > 0) {
        const auto nread = read(fd, cursor, size);
        if (nread < 0) {
            if (errno == EINTR) { continue; }
            return false;
        }
        if (nread == 0) { return false; }
        cursor += nread;
        size -= static_cast<size_t>(nread);
    }
    return true;
}

inline bool WriteString(int fd, const std::string& value)
{
    return value.empty() || WriteExact(fd, value.data(), value.size());
}

inline bool ReadString(int fd, std::uint64_t size, std::string& value)
{
    value.assign(static_cast<size_t>(size), '\0');
    return value.empty() || ReadExact(fd, value.data(), value.size());
}

inline bool WriteCosts(int fd, const std::vector<size_t>& costs)
{
    for (const auto cost : costs) {
        const std::uint64_t wireCost = static_cast<std::uint64_t>(cost);
        if (!WriteExact(fd, &wireCost, sizeof(wireCost))) { return false; }
    }
    return true;
}

inline bool ReadCosts(int fd, std::uint64_t count, std::vector<size_t>& costs)
{
    costs.resize(static_cast<size_t>(count));
    for (auto& cost : costs) {
        std::uint64_t wireCost = 0;
        if (!ReadExact(fd, &wireCost, sizeof(wireCost))) { return false; }
        cost = static_cast<size_t>(wireCost);
    }
    return true;
}

inline bool WriteResult(int fd, const CopyResult::Result& result)
{
    ResultWireHeader header;
    header.srcLen = static_cast<std::uint64_t>(result.src.size());
    header.dstLen = static_cast<std::uint64_t>(result.dst.size());
    header.methodLen = static_cast<std::uint64_t>(result.method.size());
    header.size = static_cast<std::uint64_t>(result.size);
    header.count = static_cast<std::uint64_t>(result.count);
    header.submitCount = static_cast<std::uint64_t>(result.submitCosts.size());
    header.copyCount = static_cast<std::uint64_t>(result.copyCosts.size());

    return WriteExact(fd, &header, sizeof(header)) && WriteString(fd, result.src) &&
           WriteString(fd, result.dst) && WriteString(fd, result.method) &&
           WriteCosts(fd, result.submitCosts) && WriteCosts(fd, result.copyCosts);
}

inline bool ReadResult(int fd, CopyResult::Result& result)
{
    ResultWireHeader header;
    if (!ReadExact(fd, &header, sizeof(header))) { return false; }

    std::string src;
    std::string dst;
    std::string method;
    std::vector<size_t> submitCosts;
    std::vector<size_t> copyCosts;
    if (!ReadString(fd, header.srcLen, src) || !ReadString(fd, header.dstLen, dst) ||
        !ReadString(fd, header.methodLen, method) ||
        !ReadCosts(fd, header.submitCount, submitCosts) ||
        !ReadCosts(fd, header.copyCount, copyCosts)) {
        return false;
    }

    result = CopyResult::Result{std::move(src),
                                std::move(dst),
                                std::move(method),
                                static_cast<size_t>(header.size),
                                static_cast<size_t>(header.count),
                                std::move(submitCosts),
                                std::move(copyCosts)};
    return true;
}

[[noreturn]] inline void RunChildCopy(size_t device, int writeFd,
                                      const ForkedChildCopyFn& childCopy)
{
    int status = EXIT_FAILURE;
    {
        try {
            CopyRuntime runtime;
            auto result = childCopy(device);
            status = WriteResult(writeFd, result) ? EXIT_SUCCESS : EXIT_FAILURE;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[fork-copy] device %zu failed: %s\n", device, e.what());
        } catch (...) {
            std::fprintf(stderr, "[fork-copy] device %zu failed with unknown error\n", device);
        }
    }
    close(writeFd);
    std::_Exit(status);
}

inline std::vector<size_t> MergeMaxCosts(const std::vector<CopyResult::Result>& results,
                                         bool submit)
{
    ASSERT(!results.empty());
    const auto& first = submit ? results.front().submitCosts : results.front().copyCosts;
    std::vector<size_t> merged(first.size(), 0);
    for (const auto& result : results) {
        const auto& costs = submit ? result.submitCosts : result.copyCosts;
        ASSERT(costs.size() == merged.size());
        for (size_t i = 0; i < costs.size(); ++i) {
            merged[i] = std::max(merged[i], costs[i]);
        }
    }
    return merged;
}

inline CopyResult::Result MergeForkedResults(std::vector<CopyResult::Result>&& results,
                                             std::string srcName, std::string dstName,
                                             std::string methodName)
{
    ASSERT(!results.empty());
    size_t totalCount = 0;
    for (const auto& result : results) {
        ASSERT(result.size == results.front().size);
        totalCount += result.count;
    }

    return {std::move(srcName),
            std::move(dstName),
            std::move(methodName),
            results.front().size,
            totalCount,
            MergeMaxCosts(results, true),
            MergeMaxCosts(results, false)};
}

inline std::vector<CopyResult::Result> RunForkedCopyBatchPerDevice(
    const CopyCase::Context& ctx, const ForkedChildCopyFn& childCopy)
{
    ASSERT(ctx.nDevice > 0);
    const auto allowedCpus = AllowedCpuList();
    const auto bindingPlan = BuildCpuBindingPlan(ctx.nDevice, allowedCpus);
    if (EnvFlagEnabled(kForkBindCpuEnv, true) && allowedCpus.size() < ctx.nDevice) {
        std::fprintf(stderr,
                     "[fork-copy] warning: only %zu CPUs are allowed for %zu children; "
                     "CPU binding will wrap\n",
                     allowedCpus.size(), ctx.nDevice);
    }

    std::vector<ForkedChildProcess> children;
    children.reserve(ctx.nDevice);

    for (size_t device = 0; device < ctx.nDevice; ++device) {
        int pipeFds[2];
        ASSERT(pipe(pipeFds) == 0);
        const auto pid = fork();
        ASSERT(pid != -1);
        if (pid == 0) {
            close(pipeFds[0]);
            BindChildToCpu(device, bindingPlan[device]);
            RunChildCopy(device, pipeFds[1], childCopy);
        }

        close(pipeFds[1]);
        children.push_back({pid, pipeFds[0], device});
    }

    std::vector<CopyResult::Result> childResults;
    childResults.reserve(children.size());
    bool failed = false;
    for (auto& child : children) {
        CopyResult::Result result{"", "", "", 0, 0, {}, {}};
        if (!ReadResult(child.readFd, result)) {
            std::fprintf(stderr, "[fork-copy] failed to read result from device %zu\n",
                         child.device);
            failed = true;
        } else {
            childResults.push_back(std::move(result));
        }
        close(child.readFd);
    }

    for (const auto& child : children) {
        int status = 0;
        pid_t waited = -1;
        do {
            waited = waitpid(child.pid, &status, 0);
        } while (waited == -1 && errno == EINTR);
        ASSERT(waited == child.pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS) {
            std::fprintf(stderr, "[fork-copy] child for device %zu exited abnormally\n",
                         child.device);
            failed = true;
        }
    }
    ASSERT(!failed);
    ASSERT(childResults.size() == ctx.nDevice);

    return childResults;
}

inline CopyResult::Result RunForkedCopyBatch(const CopyCase::Context& ctx, std::string srcName,
                                             std::string dstName, std::string methodName,
                                             const ForkedChildCopyFn& childCopy)
{
    auto childResults = RunForkedCopyBatchPerDevice(ctx, childCopy);
    return MergeForkedResults(std::move(childResults), std::move(srcName), std::move(dstName),
                              std::move(methodName));
}

}  // namespace ascend_copy

#endif  // FORKED_COPY_RUNNER_ASCEND_H
