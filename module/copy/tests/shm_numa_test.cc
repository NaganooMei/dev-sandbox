// SPDX-License-Identifier: MIT
#include "shm_numa.h"
#include <cstdio>

namespace {
void Require(bool condition)
{
    if (!condition) { throw std::runtime_error("NUMA layout test failed"); }
}

template <typename Function>
void Reject(Function function)
{
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("invalid NUMA layout was accepted");
}
}  // namespace

int main()
{
    // Decode the effective bits consumed by Linux get_nodes(maxnode - 1).
    // This catches node 0 becoming an empty mask and truncation at word boundaries.
    constexpr size_t bitsPerWord = sizeof(unsigned long) * 8;
    for (const size_t node : {0, 1, 7, 31, 32, 63, 64, 127, 128}) {
        const auto mask = shm_numa::SingleNodeMask(node);
        const auto copiedBits = mask.maxNode - 1;
        Require(copiedBits > node);
        Require((copiedBits + bitsPerWord - 1) / bitsPerWord <= mask.words.size());
        std::vector<size_t> decoded;
        for (size_t bit = 0; bit < copiedBits; ++bit) {
            if ((mask.words[bit / bitsPerWord] >> (bit % bitsPerWord)) & 1UL) {
                decoded.push_back(bit);
            }
        }
        Require(decoded == std::vector<size_t>{node});
    }
    Reject([]() { shm_numa::SingleNodeMask(65536); });
    const auto nodes = shm_numa::ParseNodes("0-7");
    Require(nodes == std::vector<size_t>({0, 1, 2, 3, 4, 5, 6, 7}));
    Require(shm_numa::ParseNodes("7,3-4,1") == std::vector<size_t>({7, 3, 4, 1}));
    for (const auto* text : {"", "0,", ",0", "-1", "2-1", "0,0", "0-2,2", "8x", "1--2", "65536",
                             "9999999999999999999999999"}) {
        Reject([&]() { shm_numa::ParseNodes(text); });
    }
    constexpr size_t gib = 1024ULL * 1024 * 1024;
    for (const size_t pageSize : {4096, 65536}) {
        const auto ranges = shm_numa::Plan(32 * gib, pageSize, nodes);
        Require(ranges.size() == 8);
        for (size_t i = 0; i < ranges.size(); ++i) {
            Require(ranges[i].offset == i * 4 * gib && ranges[i].bytes == 4 * gib &&
                    ranges[i].node == i);
        }
    }
    Reject([&]() { shm_numa::Plan(0, 4096, nodes); });
    Reject([&]() { shm_numa::Plan(32 * gib, 0, nodes); });
    Reject([&]() { shm_numa::Plan(32 * gib - 1, 4096, nodes); });
    Reject([&]() { shm_numa::Plan(32 * gib - 4096, 4096, nodes); });
    std::vector<size_t> counts(8);
    for (size_t segment = 0; segment < 16; ++segment) {
        const auto owner = shm_numa::SegmentNodes(nodes, 16, segment);
        Require(owner.size() == 1 && owner[0] == segment % 8);
        ++counts[owner[0]];
    }
    Require(counts == std::vector<size_t>(8, 2));
    Reject([&]() { shm_numa::SegmentNodes(nodes, 4, 0); });
    Reject([&]() { shm_numa::SegmentNodes(nodes, 16, 16); });
    Reject([&]() { shm_numa::SegmentNodes(nodes, 0, 0); });
    Require(shm_numa::Plan(1, 4096, {}).empty());
    Require(shm_numa::SegmentNodes({}, 1, 0).empty());
    std::puts("shm_numa layout tests passed");
}
