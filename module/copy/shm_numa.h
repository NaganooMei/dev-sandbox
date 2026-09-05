// SPDX-License-Identifier: MIT
#ifndef COPY_SHM_NUMA_H
#define COPY_SHM_NUMA_H

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shm_numa {

// Physical node IDs, in the user's order. Ranges such as 0-7 are accepted.
inline std::vector<size_t> ParseNodes(std::string_view text)
{
    std::vector<size_t> nodes;
    auto parse = [](std::string_view token) {
        size_t value = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (token.empty() || error != std::errc() || end != token.data() + token.size() ||
            value > 65535) {
            throw std::invalid_argument("invalid NUMA node ID: " + std::string(token));
        }
        return value;
    };
    do {
        const auto comma = text.find(',');
        const auto token = text.substr(0, comma);
        const auto dash = token.find('-');
        const auto first = parse(token.substr(0, dash));
        const auto last = dash == std::string_view::npos ? first : parse(token.substr(dash + 1));
        if (last < first) { throw std::invalid_argument("descending NUMA node range"); }
        for (size_t node = first; node <= last; ++node) {
            if (std::find(nodes.begin(), nodes.end(), node) != nodes.end()) {
                throw std::invalid_argument("duplicate NUMA node: " + std::to_string(node));
            }
            nodes.push_back(node);
        }
        if (comma == std::string_view::npos) { break; }
        text.remove_prefix(comma + 1);
    } while (true);
    return nodes;
}

struct Range {
    size_t offset;
    size_t bytes;
    size_t node;
};

struct NodeMask {
    std::vector<unsigned long> words;
    unsigned long maxNode;
};

inline NodeMask SingleNodeMask(size_t node)
{
    if (node > 65535) { throw std::invalid_argument("NUMA node ID exceeds 65535"); }
    constexpr size_t bitsPerWord = sizeof(unsigned long) * 8;
    std::vector<unsigned long> words(node / bitsPerWord + 1, 0);
    words[node / bitsPerWord] = 1UL << (node % bitsPerWord);
    // Linux get_nodes() decrements maxnode before copying/masking the bitmap.
    // Pass the allocated bit capacity + 1 so even the last bit survives.
    const auto maxNode = static_cast<unsigned long>(words.size() * bitsPerWord + 1);
    return {std::move(words), maxNode};
}

inline std::vector<Range> Plan(size_t bytes, size_t pageSize, const std::vector<size_t>& nodes)
{
    if (nodes.empty()) { return {}; }
    if (pageSize == 0 || bytes == 0 || bytes % pageSize != 0 ||
        (bytes / pageSize) % nodes.size() != 0) {
        throw std::invalid_argument(
            "SHM must split into equal, nonempty, page-aligned NUMA ranges");
    }
    const auto rangeBytes = bytes / nodes.size();
    std::vector<Range> ranges;
    for (size_t i = 0; i < nodes.size(); ++i) {
        ranges.push_back({i * rangeBytes, rangeBytes, nodes[i]});
    }
    return ranges;
}

inline std::vector<size_t> SegmentNodes(const std::vector<size_t>& nodes, size_t segments,
                                        size_t segment)
{
    if (nodes.empty()) { return {}; }
    if (segments == 0 || segment >= segments || segments % nodes.size() != 0) {
        throw std::invalid_argument("rank-striped segment count must be a multiple of NUMA nodes");
    }
    return {nodes[segment % nodes.size()]};
}

}  // namespace shm_numa

#ifdef __linux__
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/mempolicy.h>
#include <map>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace shm_numa {

inline void CheckSystemCall(long result, const std::string& operation)
{
    if (result < 0) {
        const auto error = errno;
        throw std::runtime_error(operation + ": " + std::strerror(error) +
                                 " (errno=" + std::to_string(error) + ")");
    }
}

inline size_t PageSize()
{
    const auto size = sysconf(_SC_PAGESIZE);
    if (size <= 0) { throw std::runtime_error("cannot determine the system page size"); }
    return static_cast<size_t>(size);
}

inline void ValidateAllowedNodes(const std::vector<size_t>& nodes)
{
    std::ifstream status("/proc/self/status");
    std::string line;
    const std::string prefix = "Mems_allowed_list:";
    while (std::getline(status, line)) {
        if (line.compare(0, prefix.size(), prefix) != 0) { continue; }
        const auto start = line.find_first_not_of(" \t", prefix.size());
        if (start == std::string::npos) { break; }
        const auto allowed = ParseNodes(line.substr(start));
        for (const auto node : nodes) {
            if (std::find(allowed.begin(), allowed.end(), node) == allowed.end()) {
                throw std::runtime_error("NUMA node " + std::to_string(node) +
                                         " is outside Mems_allowed_list=" + line.substr(start));
            }
        }
        return;
    }
    throw std::runtime_error("cannot read Mems_allowed_list from /proc/self/status");
}

// For fresh POSIX SHM only. tmpfs stores mbind policy on the shared object.
// MAP_POPULATE must be absent: no page may be faulted before all ranges are bound.
inline void BindBeforeTouch(void* base, const std::vector<Range>& ranges, const std::string& name)
{
    const auto pageSize = PageSize();
    if (reinterpret_cast<size_t>(base) % pageSize != 0) {
        throw std::invalid_argument("SHM address is not page-aligned");
    }
    for (const auto& range : ranges) {
        ValidateAllowedNodes({range.node});
        const auto mask = SingleNodeMask(range.node);
        auto* address = static_cast<char*>(base) + range.offset;
        CheckSystemCall(syscall(SYS_mbind, address, range.bytes, MPOL_BIND | MPOL_F_STATIC_NODES,
                                mask.words.data(), mask.maxNode, 0UL),
                        "mbind node=" + std::to_string(range.node) +
                            " maxnode=" + std::to_string(mask.maxNode) + " shm=" + name);
        std::fprintf(stderr, "[shm-numa] bind shm=%s offset=%zu bytes=%zu node=%zu\n", name.c_str(),
                     range.offset, range.bytes, range.node);
    }
}

// Query every base page, without migrating any page (nodes=nullptr, flags=0).
// Failure or an unexpected node is an error, never a successful fallback.
inline void Verify(void* base, const std::vector<Range>& ranges, const std::string& name)
{
    const auto pageSize = PageSize();
    constexpr size_t batchSize = 4096;
    std::vector<void*> addresses(batchSize);
    std::vector<int> status(batchSize);
    for (const auto& range : ranges) {
        std::map<int, size_t> counts;
        size_t mismatches = 0;
        const auto pageCount = range.bytes / pageSize;
        for (size_t first = 0; first < pageCount;) {
            const auto count = std::min(batchSize, pageCount - first);
            for (size_t i = 0; i < count; ++i) {
                addresses[i] = static_cast<char*>(base) + range.offset + (first + i) * pageSize;
            }
            std::fill(status.begin(), status.end(), -EIO);
            CheckSystemCall(
                syscall(SYS_move_pages, 0, count, addresses.data(), nullptr, status.data(), 0),
                "move_pages query shm=" + name);
            for (size_t i = 0; i < count; ++i) {
                if (status[i] < 0) {
                    throw std::runtime_error(
                        "move_pages page query failed: " + std::string(std::strerror(-status[i])) +
                        " shm=" + name);
                }
                ++counts[status[i]];
                if (static_cast<size_t>(status[i]) != range.node) { ++mismatches; }
            }
            first += count;
        }
        for (const auto& [node, pages] : counts) {
            std::fprintf(stderr,
                         "[shm-numa] verify shm=%s offset=%zu expected_node=%zu actual_node=%d "
                         "pages=%zu bytes=%zu mismatches=%zu\n",
                         name.c_str(), range.offset, range.node, node, pages, pages * pageSize,
                         mismatches);
        }
        if (mismatches != 0) {
            throw std::runtime_error("SHM NUMA placement verification failed: " + name);
        }
    }
    std::fprintf(stderr, "[shm-numa] verified shm=%s ranges=%zu\n", name.c_str(), ranges.size());
}

// Used only by the creating process; peers must open/map the existing object normally.
inline void* MapAndInitialize(int fd, size_t bytes, int fill, const std::vector<size_t>& nodes,
                              const std::string& name)
{
    const auto ranges = Plan(bytes, PageSize(), nodes);
    if (ranges.empty()) { throw std::invalid_argument("explicit SHM NUMA nodes are required"); }
    ValidateAllowedNodes(nodes);
    auto* address = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (address == MAP_FAILED) { CheckSystemCall(-1, "mmap shm=" + name); }
    try {
        BindBeforeTouch(address, ranges, name);
        std::memset(address, fill, bytes);
        Verify(address, ranges, name);
    } catch (...) {
        munmap(address, bytes);
        throw;
    }
    return address;
}

inline void* Create(const std::string& name, size_t bytes, int fill,
                    const std::vector<size_t>& nodes)
{
    Plan(bytes, PageSize(), nodes);
    ValidateAllowedNodes(nodes);
    if (bytes > static_cast<size_t>(std::numeric_limits<off_t>::max())) {
        throw std::invalid_argument("SHM size exceeds off_t");
    }
    const auto fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    CheckSystemCall(fd, "shm_open " + name);
    try {
        CheckSystemCall(ftruncate(fd, static_cast<off_t>(bytes)), "ftruncate " + name);
        auto* address = MapAndInitialize(fd, bytes, fill, nodes, name);
        close(fd);
        return address;
    } catch (...) {
        close(fd);
        shm_unlink(name.c_str());
        throw;
    }
}

}  // namespace shm_numa
#endif  // __linux__
#endif  // COPY_SHM_NUMA_H
