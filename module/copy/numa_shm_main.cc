// SPDX-License-Identifier: MIT
#include <csignal>
#include <cstdio>
#include "shm_numa.h"

namespace {
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }

size_t ParseUnsigned(std::string_view text)
{
    size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        throw std::invalid_argument("invalid unsigned integer: " + std::string(text));
    }
    return value;
}
}  // namespace

int main(int argc, char** argv)
{
    void* address = MAP_FAILED;
    size_t bytes = 32ULL * 1024 * 1024 * 1024;
    size_t holdSeconds = 60;
    auto nodes = shm_numa::ParseNodes("0-7");
    const auto name = "/copy_ascend_numa_probe_" + std::to_string(getpid());
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--help") {
                std::printf(
                    "Usage: %s [--bytes 34359738368] [--nodes 0-7] "
                    "[--hold-seconds 60]\n",
                    argv[0]);
                return 0;
            }
            if (i + 1 == argc) { throw std::invalid_argument("missing option value"); }
            const std::string_view value = argv[++i];
            if (option == "--bytes") {
                bytes = ParseUnsigned(value);
            } else if (option == "--nodes") {
                nodes = shm_numa::ParseNodes(value);
            } else if (option == "--hold-seconds") {
                holdSeconds = ParseUnsigned(value);
            } else {
                throw std::invalid_argument("unknown option: " + std::string(option));
            }
        }
        // Create verifies all resident pages before returning. No device runtime is used.
        address = shm_numa::Create(name, bytes, 's', nodes);
        std::signal(SIGINT, Stop);
        std::signal(SIGTERM, Stop);
        std::printf("READY pid=%ld shm=/dev/shm%s bytes=%zu hold_seconds=%zu\n",
                    static_cast<long>(getpid()), name.c_str(), bytes, holdSeconds);
        std::fflush(stdout);
        for (size_t i = 0; i < holdSeconds && !stopped; ++i) { sleep(1); }
        munmap(address, bytes);
        shm_unlink(name.c_str());
        return 0;
    } catch (const std::exception& error) {
        if (address != MAP_FAILED) {
            munmap(address, bytes);
            shm_unlink(name.c_str());
        }
        std::fprintf(stderr, "numa_shm: %s\n", error.what());
        return 1;
    }
}
