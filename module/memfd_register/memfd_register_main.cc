// SPDX-License-Identifier: MIT
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fmt/format.h>
#include <fstream>
#include <limits>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include "shm_numa.h"

#ifdef MEMFD_REGISTER_HAS_ASCEND
#include <acl/acl.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kWireMagic = 0x4d465244;  // MFRD
#ifndef MEMFD_REGISTER_HAS_ASCEND
constexpr std::int64_t kRuntimeUnavailable = -1000000;
#endif

enum class MessageKind : std::uint32_t {
    LocalReady = 1,
    PeerFd = 2,
    Abort = 3,
    Done = 4,
    Start = 5,
    Release = 6,
};

enum class RegistrationMode {
    Concurrent,
    Serial,
};

struct Message {
    std::uint32_t magic{kWireMagic};
    MessageKind kind{MessageKind::Abort};
    std::uint32_t rank{0};
    std::int64_t status{0};
    std::uint64_t registeredSegments{0};
    std::uint64_t registeredBytes{0};
};

struct Options {
    size_t ranks{0};
    std::uint64_t totalBytes{0};
    size_t deviceOffset{0};
    size_t timeoutSeconds{600};
    std::optional<std::vector<size_t>> numaNodes{};
    RegistrationMode registrationMode{RegistrationMode::Concurrent};
};

struct Segment {
    int fd{-1};
    void* address{MAP_FAILED};
    bool registered{false};
};

struct Channel {
    int parent{-1};
    int child{-1};
};

template <typename... Args>
void Log(bool error, fmt::format_string<Args...> format, Args&&... args)
{
    auto line = fmt::format(format, std::forward<Args>(args)...);
    line.push_back('\n');
    const int fd = error ? STDERR_FILENO : STDOUT_FILENO;
    const char* cursor = line.data();
    size_t remaining = line.size();
    while (remaining > 0) {
        const auto written = ::write(fd, cursor, remaining);
        if (written < 0 && errno == EINTR) { continue; }
        if (written <= 0) { return; }
        cursor += written;
        remaining -= static_cast<size_t>(written);
    }
}

std::uint64_t ParseUnsigned(std::string_view text, std::string_view option)
{
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) {
        throw std::invalid_argument("invalid value for " + std::string(option) + ": " +
                                    std::string(text));
    }
    return value;
}

std::uint64_t ParseSize(std::string_view text)
{
    if (text.empty()) { throw std::invalid_argument("--total-size cannot be empty"); }
    const auto firstUnit = text.find_first_not_of("0123456789");
    const auto number = text.substr(0, firstUnit);
    std::string unit =
        firstUnit == std::string_view::npos ? "" : std::string(text.substr(firstUnit));
    std::transform(unit.begin(), unit.end(), unit.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::uint64_t multiplier = 1;
    if (unit.empty() || unit == "B") {
        multiplier = 1;
    } else if (unit == "K" || unit == "KB" || unit == "KIB") {
        multiplier = 1ULL << 10;
    } else if (unit == "M" || unit == "MB" || unit == "MIB") {
        multiplier = 1ULL << 20;
    } else if (unit == "G" || unit == "GB" || unit == "GIB") {
        multiplier = 1ULL << 30;
    } else if (unit == "T" || unit == "TB" || unit == "TIB") {
        multiplier = 1ULL << 40;
    } else {
        throw std::invalid_argument("invalid size suffix: " + unit);
    }
    const auto value = ParseUnsigned(number, "--total-size");
    if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        throw std::invalid_argument("--total-size overflows uint64");
    }
    return value * multiplier;
}

void PrintHelp(const char* program)
{
    fmt::print(
        "Usage: {} --ranks N --total-size SIZE [options]\n"
        "\n"
        "Required:\n"
        "  --ranks N             Number of rank processes/NPU contexts\n"
        "  --total-size SIZE     Total Cache2 data size, e.g. 192G or 256GiB\n"
        "\n"
        "Options:\n"
        "  --device-offset N     Device ID of rank 0 (default: 0)\n"
        "  --numa-nodes LIST     Ordered NUMA IDs, e.g. 0-7 (default: auto)\n"
        "  --registration-mode M concurrent or serial (default: concurrent)\n"
        "  --timeout-seconds N   IPC/setup timeout (default: 600)\n"
        "  -h, --help            Show this help\n",
        program);
}

Options ParseOptions(int argc, char** argv)
{
    Options options;
    bool ranksSet = false;
    bool totalSet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == "-h" || option == "--help") {
            PrintHelp(argv[0]);
            std::exit(EXIT_SUCCESS);
        }
        if (i + 1 == argc) {
            throw std::invalid_argument("missing value for " + std::string(option));
        }
        const std::string_view value = argv[++i];
        if (option == "--ranks") {
            const auto parsed = ParseUnsigned(value, option);
            if (parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("--ranks must fit in uint32 and be positive");
            }
            options.ranks = static_cast<size_t>(parsed);
            ranksSet = true;
        } else if (option == "--total-size") {
            options.totalBytes = ParseSize(value);
            totalSet = true;
        } else if (option == "--device-offset") {
            options.deviceOffset = static_cast<size_t>(ParseUnsigned(value, option));
        } else if (option == "--numa-nodes") {
            options.numaNodes = shm_numa::ParseNodes(value);
        } else if (option == "--registration-mode") {
            if (value == "concurrent") {
                options.registrationMode = RegistrationMode::Concurrent;
            } else if (value == "serial") {
                options.registrationMode = RegistrationMode::Serial;
            } else {
                throw std::invalid_argument("--registration-mode must be concurrent or serial");
            }
        } else if (option == "--timeout-seconds") {
            options.timeoutSeconds = static_cast<size_t>(ParseUnsigned(value, option));
        } else {
            throw std::invalid_argument("unknown option: " + std::string(option));
        }
    }
    if (!ranksSet || !totalSet) {
        throw std::invalid_argument("--ranks and --total-size are required");
    }
    if (options.totalBytes == 0 || options.totalBytes % options.ranks != 0) {
        throw std::invalid_argument("--total-size must divide evenly by --ranks");
    }
    if (options.deviceOffset > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
        options.ranks - 1 >
            static_cast<size_t>(std::numeric_limits<int32_t>::max()) - options.deviceOffset) {
        throw std::invalid_argument("device IDs exceed int32");
    }
    if (options.timeoutSeconds == 0 ||
        options.timeoutSeconds > static_cast<size_t>(std::numeric_limits<int>::max() / 1000)) {
        throw std::invalid_argument("--timeout-seconds is outside the supported range");
    }
    return options;
}

std::vector<size_t> ReadNodeList(const std::string& path)
{
    std::ifstream input(path);
    std::string line;
    if (!std::getline(input, line)) { throw std::runtime_error("cannot read " + path); }
    return shm_numa::ParseNodes(line);
}

std::vector<size_t> ReadAllowedNodes()
{
    std::ifstream input("/proc/self/status");
    const std::string prefix = "Mems_allowed_list:";
    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, prefix.size(), prefix) != 0) { continue; }
        const auto start = line.find_first_not_of(" \t", prefix.size());
        if (start != std::string::npos) { return shm_numa::ParseNodes(line.substr(start)); }
        break;
    }
    throw std::runtime_error("cannot read Mems_allowed_list from /proc/self/status");
}

std::vector<size_t> AvailableNodes()
{
    const auto online = ReadNodeList("/sys/devices/system/node/online");
    const auto memory = ReadNodeList("/sys/devices/system/node/has_memory");
    const auto allowed = ReadAllowedNodes();
    std::vector<size_t> result;
    for (const auto node : memory) {
        if (std::find(online.begin(), online.end(), node) == online.end()) { continue; }
        if (std::find(allowed.begin(), allowed.end(), node) == allowed.end()) { continue; }
        result.push_back(node);
    }
    if (result.empty()) { throw std::runtime_error("no usable NUMA memory node"); }
    std::sort(result.begin(), result.end());
    return result;
}

std::string JoinNodes(const std::vector<size_t>& nodes)
{
    std::string result;
    for (const auto node : nodes) {
        if (!result.empty()) { result.push_back(','); }
        result += std::to_string(node);
    }
    return result;
}

const char* RegistrationModeName(RegistrationMode mode)
{
    return mode == RegistrationMode::Serial ? "serial" : "concurrent";
}

bool SendMessage(int socket, const Message& message, int fd = -1)
{
    iovec io{const_cast<Message*>(&message), sizeof(message)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr header{};
    header.msg_iov = &io;
    header.msg_iovlen = 1;
    if (fd >= 0) {
        header.msg_control = control;
        header.msg_controllen = sizeof(control);
        auto* cmsg = CMSG_FIRSTHDR(&header);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    }
    for (;;) {
        const auto sent = ::sendmsg(socket, &header, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) { continue; }
        return sent == static_cast<ssize_t>(sizeof(message));
    }
}

bool ReceiveMessage(int socket, Message& message, int& fd, int timeoutMs)
{
    fd = -1;
    pollfd ready{socket, POLLIN, 0};
    int polled = 0;
    do {
        polled = ::poll(&ready, 1, timeoutMs);
    } while (polled < 0 && errno == EINTR);
    if (polled <= 0) { return false; }

    iovec io{&message, sizeof(message)};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr header{};
    header.msg_iov = &io;
    header.msg_iovlen = 1;
    header.msg_control = control;
    header.msg_controllen = sizeof(control);
    ssize_t received = 0;
    do {
        received = ::recvmsg(socket, &header, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received != static_cast<ssize_t>(sizeof(message)) || message.magic != kWireMagic ||
        (header.msg_flags & MSG_CTRUNC) != 0) {
        return false;
    }
    for (auto* cmsg = CMSG_FIRSTHDR(&header); cmsg != nullptr; cmsg = CMSG_NXTHDR(&header, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
            std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
            break;
        }
    }
    return true;
}

std::int64_t CreateLocalSegment(size_t rank, size_t bytes, size_t numaNode, Segment& segment)
{
    const auto name = "ucm_cache2_register_probe_data_" + std::to_string(rank);
    segment.fd = ::memfd_create(name.c_str(), MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (segment.fd < 0) { return -errno; }
    if (bytes > static_cast<size_t>(std::numeric_limits<off_t>::max())) { return -EOVERFLOW; }
    if (::ftruncate(segment.fd, static_cast<off_t>(bytes)) != 0) { return -errno; }
    if (::fcntl(segment.fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK) != 0) { return -errno; }
    segment.address = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, segment.fd, 0);
    if (segment.address == MAP_FAILED) { return -errno; }
    try {
        const std::vector<shm_numa::Range> ranges{
            {0, bytes, numaNode}
        };
        shm_numa::BindBeforeTouch(segment.address, ranges, name);
    } catch (const std::exception& error) {
        Log(true, "[rank={}] NUMA bind failed: {}", rank, error.what());
        return errno == 0 ? -EIO : -errno;
    }
    const auto pageSize = shm_numa::PageSize();
    constexpr size_t kProgressBytes = 16ULL * 1024 * 1024 * 1024;
    auto* base = static_cast<volatile unsigned char*>(segment.address);
    size_t nextProgress = std::min(bytes, kProgressBytes);
    const auto touchStart = Clock::now();
    Log(false, "first_touch owner={} numa={} bytes={} status=start", rank, numaNode, bytes);
    for (size_t offset = 0; offset < bytes; offset += pageSize) {
        base[offset] = 0;
        const auto touched = std::min(bytes, offset + pageSize);
        if (touched < nextProgress) { continue; }
        const auto elapsed =
            std::chrono::duration<double, std::milli>(Clock::now() - touchStart).count();
        Log(false, "first_touch owner={} numa={} touched_bytes={} total_bytes={} cost_ms={:.3f}",
            rank, numaNode, touched, bytes, elapsed);
        if (nextProgress >= bytes || bytes - nextProgress <= kProgressBytes) {
            nextProgress = bytes;
        } else {
            nextProgress += kProgressBytes;
        }
    }
    return 0;
}

std::int64_t MapPeerSegment(int fd, size_t bytes, Segment& segment)
{
    segment.fd = fd;
    segment.address = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, segment.fd, 0);
    return segment.address == MAP_FAILED ? -errno : 0;
}

std::string RecentRuntimeError()
{
#ifdef MEMFD_REGISTER_HAS_ASCEND
    const char* message = aclGetRecentErrMsg();
    std::string result = message == nullptr ? "" : message;
    std::replace(result.begin(), result.end(), '\n', ' ');
    return result;
#else
    return "Ascend backend was not detected at build time";
#endif
}

std::string StatusDetail(std::int64_t status)
{
    if (status < 0 && -status <= std::numeric_limits<int>::max()) {
        return std::strerror(static_cast<int>(-status));
    }
    return RecentRuntimeError();
}

std::int64_t InitRuntime()
{
#ifdef MEMFD_REGISTER_HAS_ASCEND
    return aclInit(nullptr);
#else
    return kRuntimeUnavailable;
#endif
}

std::int64_t RegisterSegment(int32_t device, size_t bytes, Segment& segment)
{
#ifdef MEMFD_REGISTER_HAS_ASCEND
    auto status = aclrtSetDevice(device);
    if (status != ACL_SUCCESS) { return status; }
    status = aclrtHostRegisterV2(segment.address, bytes, ACL_HOST_REG_MAPPED | ACL_HOST_REG_PINNED);
    if (status == ACL_SUCCESS) { segment.registered = true; }
    return status;
#else
    (void)device;
    (void)bytes;
    (void)segment;
    return kRuntimeUnavailable;
#endif
}

void CleanupSegments(int32_t device, size_t bytes, std::vector<Segment>& segments,
                     bool runtimeInitialized)
{
    for (auto& segment : segments) {
#ifdef MEMFD_REGISTER_HAS_ASCEND
        if (segment.registered) {
            (void)aclrtSetDevice(device);
            const auto status = aclrtHostUnregister(segment.address);
            if (status != ACL_SUCCESS) {
                Log(true, "cleanup unregister failed: device={} status={}", device, status);
            }
        }
#else
        (void)device;
#endif
        if (segment.address != MAP_FAILED) { ::munmap(segment.address, bytes); }
        if (segment.fd >= 0) { ::close(segment.fd); }
        segment = Segment{};
    }
#ifdef MEMFD_REGISTER_HAS_ASCEND
    if (runtimeInitialized) {
        (void)aclrtResetDevice(device);
        (void)aclFinalize();
    }
#else
    (void)runtimeInitialized;
#endif
}

std::int64_t RegisterAndLog(size_t owner, int32_t device, size_t segmentRank, size_t rankBytes,
                            const std::vector<size_t>& rankNodes, std::vector<Segment>& segments,
                            std::uint64_t& registeredSegments, std::uint64_t& registeredBytes)
{
    const auto start = Clock::now();
    const auto status = RegisterSegment(device, rankBytes, segments[segmentRank]);
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start);
    if (status == 0) {
        ++registeredSegments;
        registeredBytes += rankBytes;
    }
    Log(status != 0,
        "register owner={} device={} segment={} role={} numa={} bytes={} "
        "registered_segments={} registered_bytes={} status={} cost_ms={:.3f} error={}",
        owner, device, segmentRank, segmentRank == owner ? "local" : "peer", rankNodes[segmentRank],
        rankBytes, registeredSegments, registeredBytes, status, elapsed.count(),
        status == 0 ? "" : StatusDetail(status));
    return status;
}

[[noreturn]] void RunChild(size_t rank, const Options& options, size_t rankBytes,
                           const std::vector<size_t>& rankNodes, int socket)
{
    const auto device = static_cast<int32_t>(options.deviceOffset + rank);
    const auto numaNode = rankNodes[rank];
    const int timeoutMs = static_cast<int>(options.timeoutSeconds * 1000);
    const bool serial = options.registrationMode == RegistrationMode::Serial;
    std::vector<Segment> segments(options.ranks);
    bool runtimeInitialized = false;
    std::int64_t status = InitRuntime();
    if (status == 0) { runtimeInitialized = true; }
    if (status == 0) { status = CreateLocalSegment(rank, rankBytes, numaNode, segments[rank]); }
    std::uint64_t registeredSegments = 0;
    std::uint64_t registeredBytes = 0;
    if (status != 0) {
        Log(true, "local setup failed: owner={} device={} numa={} status={} error={}", rank, device,
            numaNode, status, StatusDetail(status));
    } else if (!serial) {
        status = RegisterAndLog(rank, device, rank, rankBytes, rankNodes, segments,
                                registeredSegments, registeredBytes);
    }

    Message local{kWireMagic, MessageKind::LocalReady, static_cast<std::uint32_t>(rank),
                  status,     registeredSegments,      registeredBytes};
    const int localFd = status == 0 ? segments[rank].fd : -1;
    if (!SendMessage(socket, local, localFd)) { status = -EPIPE; }

    if (status == 0) {
        size_t receivedPeers = 0;
        while (receivedPeers < options.ranks - 1) {
            Message incoming;
            int peerFd = -1;
            if (!ReceiveMessage(socket, incoming, peerFd, timeoutMs)) {
                status = -ETIMEDOUT;
                break;
            }
            if (incoming.kind == MessageKind::Abort) {
                if (peerFd >= 0) { ::close(peerFd); }
                status = incoming.status == 0 ? -ECANCELED : incoming.status;
                break;
            }
            if (incoming.kind != MessageKind::PeerFd || incoming.rank >= options.ranks ||
                incoming.rank == rank || peerFd < 0) {
                if (peerFd >= 0) { ::close(peerFd); }
                status = -EPROTO;
                break;
            }
            ++receivedPeers;
            const auto peerRank = static_cast<size_t>(incoming.rank);
            auto mapStatus = MapPeerSegment(peerFd, rankBytes, segments[peerRank]);
            if (mapStatus != 0) {
                status = mapStatus;
                Log(true, "peer mmap failed: owner={} device={} segment={} status={}", rank, device,
                    peerRank, status);
                break;
            }
            if (!serial) {
                status = RegisterAndLog(rank, device, peerRank, rankBytes, rankNodes, segments,
                                        registeredSegments, registeredBytes);
                if (status != 0) { break; }
            }
        }
    } else {
        /* Earlier peer descriptors may already be queued before the parent reaches this
         * rank's failed LocalReady report. Drain them until the broker sends Abort. */
        for (;;) {
            Message incoming;
            int ignoredFd = -1;
            if (!ReceiveMessage(socket, incoming, ignoredFd, timeoutMs)) { break; }
            if (ignoredFd >= 0) { ::close(ignoredFd); }
            if (incoming.kind == MessageKind::Abort) { break; }
        }
    }

    bool serialStarted = false;
    if (serial && status == 0) {
        Message command;
        int unexpectedFd = -1;
        if (!ReceiveMessage(socket, command, unexpectedFd, timeoutMs)) {
            status = -ETIMEDOUT;
        } else if (unexpectedFd >= 0) {
            ::close(unexpectedFd);
            status = -EPROTO;
        } else if (command.kind != MessageKind::Start) {
            status = command.status == 0 ? -ECANCELED : command.status;
        } else {
            serialStarted = true;
            Log(false, "serial_start owner={} device={}", rank, device);
            status = RegisterAndLog(rank, device, rank, rankBytes, rankNodes, segments,
                                    registeredSegments, registeredBytes);
            for (size_t peerRank = 0; status == 0 && peerRank < options.ranks; ++peerRank) {
                if (peerRank == rank) { continue; }
                status = RegisterAndLog(rank, device, peerRank, rankBytes, rankNodes, segments,
                                        registeredSegments, registeredBytes);
            }
        }
    }

    Message done{kWireMagic, MessageKind::Done,  static_cast<std::uint32_t>(rank),
                 status,     registeredSegments, registeredBytes};
    (void)SendMessage(socket, done);
    if (serialStarted) {
        Message release;
        int unexpectedFd = -1;
        const bool received = ReceiveMessage(socket, release, unexpectedFd, timeoutMs);
        if (unexpectedFd >= 0) { ::close(unexpectedFd); }
        if (!received || release.kind != MessageKind::Release || unexpectedFd >= 0) {
            Log(true, "serial release failed: owner={} device={}", rank, device);
        } else {
            Log(false, "serial_release owner={} device={}", rank, device);
        }
    }
    CleanupSegments(device, rankBytes, segments, runtimeInitialized);
    ::close(socket);
    std::_Exit(status == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}

void CloseChannels(std::vector<Channel>& channels)
{
    for (auto& channel : channels) {
        if (channel.parent >= 0) { ::close(channel.parent); }
        if (channel.child >= 0) { ::close(channel.child); }
        channel = Channel{};
    }
}

int RunParent(const Options& options, size_t rankBytes, const std::vector<size_t>& rankNodes)
{
    const int timeoutMs = static_cast<int>(options.timeoutSeconds * 1000);
    std::vector<Channel> channels(options.ranks);
    std::vector<pid_t> children(options.ranks, -1);
    for (auto& channel : channels) {
        int pair[2]{};
        if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) {
            throw std::runtime_error("socketpair failed: " + std::string(std::strerror(errno)));
        }
        channel.parent = pair[0];
        channel.child = pair[1];
    }

    for (size_t rank = 0; rank < options.ranks; ++rank) {
        const auto pid = ::fork();
        if (pid < 0) {
            throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
        }
        if (pid == 0) {
            for (size_t i = 0; i < channels.size(); ++i) {
                ::close(channels[i].parent);
                if (i != rank) { ::close(channels[i].child); }
            }
            RunChild(rank, options, rankBytes, rankNodes, channels[rank].child);
        }
        children[rank] = pid;
    }
    for (auto& channel : channels) {
        ::close(channel.child);
        channel.child = -1;
    }

    bool setupSuccess = true;
    bool abortSent = false;
    std::int64_t firstFailure = 0;
    const auto sendAbort = [&] {
        if (abortSent) { return; }
        const Message abort{kWireMagic, MessageKind::Abort, 0, firstFailure, 0, 0};
        for (const auto& channel : channels) { (void)SendMessage(channel.parent, abort); }
        abortSent = true;
    };
    /* Consume and publish ranks in ascending order, matching DataStrategy::ImportPeers.
     * A child can start importing earlier ranks while later owners are still setting up. */
    for (size_t peer = 0; peer < options.ranks; ++peer) {
        Message message;
        int fd = -1;
        if (!ReceiveMessage(channels[peer].parent, message, fd, timeoutMs) ||
            message.kind != MessageKind::LocalReady || message.rank != peer) {
            setupSuccess = false;
            if (firstFailure == 0) { firstFailure = -ETIMEDOUT; }
            if (fd >= 0) { ::close(fd); }
            sendAbort();
            continue;
        }
        if (message.status != 0 || fd < 0) {
            setupSuccess = false;
            if (firstFailure == 0) {
                firstFailure = message.status == 0 ? -EPROTO : message.status;
            }
            if (fd >= 0) { ::close(fd); }
            sendAbort();
            continue;
        }
        if (setupSuccess) {
            const Message incoming{
                kWireMagic, MessageKind::PeerFd, static_cast<std::uint32_t>(peer), 0, 0, 0};
            for (size_t owner = 0; owner < options.ranks; ++owner) {
                if (owner == peer) { continue; }
                if (!SendMessage(channels[owner].parent, incoming, fd)) {
                    setupSuccess = false;
                    if (firstFailure == 0) { firstFailure = -EPIPE; }
                    sendAbort();
                    break;
                }
            }
        }
        ::close(fd);
    }

    std::vector<Message> results(options.ranks);
    std::vector<bool> resultReady(options.ranks, false);
    const auto receiveDone = [&](size_t rank) {
        int fd = -1;
        if (!ReceiveMessage(channels[rank].parent, results[rank], fd, timeoutMs) ||
            results[rank].kind != MessageKind::Done || results[rank].rank != rank) {
            results[rank] = Message{
                kWireMagic, MessageKind::Done, static_cast<std::uint32_t>(rank), -ETIMEDOUT, 0, 0};
        }
        if (fd >= 0) { ::close(fd); }
        resultReady[rank] = true;
    };

    if (options.registrationMode == RegistrationMode::Serial && setupSuccess) {
        for (size_t rank = 0; rank < options.ranks; ++rank) {
            const Message start{
                kWireMagic, MessageKind::Start, static_cast<std::uint32_t>(rank), 0, 0, 0};
            Log(false, "SERIAL dispatch rank={} device={}", rank, options.deviceOffset + rank);
            if (!SendMessage(channels[rank].parent, start)) {
                results[rank] = Message{
                    kWireMagic, MessageKind::Done, static_cast<std::uint32_t>(rank), -EPIPE, 0, 0};
                resultReady[rank] = true;
                break;
            }
            receiveDone(rank);
            if (results[rank].status != 0) { break; }
        }
        /* Successful earlier ranks keep all registrations until this point. Release them only
         * after the first failure, or after every rank completed successfully. */
        const Message release{kWireMagic, MessageKind::Release, 0, 0, 0, 0};
        for (const auto& channel : channels) { (void)SendMessage(channel.parent, release); }
    }
    for (size_t rank = 0; rank < options.ranks; ++rank) {
        if (!resultReady[rank]) { receiveDone(rank); }
    }

    CloseChannels(channels);
    for (const auto pid : children) {
        if (pid < 0) { continue; }
        int childStatus = 0;
        while (::waitpid(pid, &childStatus, 0) < 0 && errno == EINTR) {}
    }
    size_t passed = 0;
    for (const auto& result : results) {
        if (result.status == 0) { ++passed; }
        Log(result.status != 0,
            "SUMMARY rank={} device={} status={} registered_segments={} registered_bytes={}",
            result.rank, options.deviceOffset + result.rank, result.status,
            result.registeredSegments, result.registeredBytes);
    }
    Log(passed != options.ranks, "OVERALL status={} passed_ranks={} failed_ranks={}",
        passed == options.ranks ? "PASS" : "FAIL", passed, options.ranks - passed);
    return passed == options.ranks ? EXIT_SUCCESS : EXIT_FAILURE;
}

}  // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = ParseOptions(argc, argv);
#ifndef MEMFD_REGISTER_HAS_ASCEND
        Log(true,
            "memfd_register requires an Ascend build; configure with "
            "-DASCEND_ROOT=/usr/local/Ascend/ascend-toolkit/latest");
        return EXIT_FAILURE;
#endif
        const auto available = AvailableNodes();
        const auto nodes = options.numaNodes.value_or(available);
        for (const auto node : nodes) {
            if (std::find(available.begin(), available.end(), node) == available.end()) {
                throw std::invalid_argument("NUMA node " + std::to_string(node) +
                                            " is not online, memory-bearing, and allowed");
            }
        }
        const auto pageSize = shm_numa::PageSize();
        const auto requestedRankBytes = options.totalBytes / options.ranks;
        if (requestedRankBytes > std::numeric_limits<size_t>::max() - (pageSize - 1)) {
            throw std::invalid_argument("per-rank size exceeds size_t");
        }
        const auto rankBytes =
            static_cast<size_t>((requestedRankBytes + pageSize - 1) / pageSize * pageSize);
        if (rankBytes > std::numeric_limits<std::uint64_t>::max() / options.ranks) {
            throw std::invalid_argument("mapped total size overflows uint64");
        }
        std::vector<size_t> rankNodes(options.ranks);
        for (size_t rank = 0; rank < options.ranks; ++rank) {
            rankNodes[rank] = nodes[rank * nodes.size() / options.ranks];
        }
        Log(false,
            "PLAN ranks={} total_bytes={} requested_rank_bytes={} rank_stride={} "
            "mapped_total_bytes={} device_offset={} numa_nodes={} registration_mode={} "
            "timeout_seconds={}",
            options.ranks, options.totalBytes, requestedRankBytes, rankBytes,
            static_cast<std::uint64_t>(rankBytes) * options.ranks, options.deviceOffset,
            JoinNodes(nodes), RegistrationModeName(options.registrationMode),
            options.timeoutSeconds);
        for (size_t rank = 0; rank < options.ranks; ++rank) {
            Log(false, "PLAN rank={} device={} numa={}", rank, options.deviceOffset + rank,
                rankNodes[rank]);
        }
        ::signal(SIGPIPE, SIG_IGN);
        return RunParent(options, rankBytes, rankNodes);
    } catch (const std::exception& error) {
        Log(true, "memfd_register: {}", error.what());
        PrintHelp(argv[0]);
        return EXIT_FAILURE;
    }
}
