#include <acl/acl.h>
#include <acl/acl_rt.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hixl/hixl.h"

namespace {

using Clock = std::chrono::steady_clock;
using Microseconds = std::chrono::microseconds;

constexpr std::uint64_t kDefaultTotalBytes = 128ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kDefaultMinBlockBytes = 256ULL * 1024ULL;
constexpr std::uint64_t kDefaultMaxBlockBytes = 8ULL * 1024ULL * 1024ULL;
constexpr int kDefaultRepeats = 5;
constexpr int kDefaultConnectTimeoutMs = 10'000;
constexpr int kDefaultCompletionTimeoutMs = 60'000;
constexpr int kDefaultMetadataTimeoutMs = 60'000;
constexpr int kDefaultPollIntervalUs = 50;
constexpr int kMetadataPollIntervalMs = 50;

[[noreturn]] void Fail(const std::string &message)
{
    throw std::runtime_error(message);
}

void CheckAcl(aclError code, const std::string &context)
{
    if (code != ACL_ERROR_NONE) {
        std::ostringstream stream;
        stream << context << " failed with aclError=" << static_cast<int>(code);
        Fail(stream.str());
    }
}

void CheckHixl(hixl::Status code, const std::string &context)
{
    if (code != hixl::SUCCESS) {
        std::ostringstream stream;
        stream << context << " failed with hixl::Status=" << code;
        Fail(stream.str());
    }
}

void LogInfo(const std::string &message)
{
    std::cerr << "[hixlbw] " << message << '\n';
}

void LogRequestedDeviceContext(const std::string &role, int requested_device)
{
    std::ostringstream stream;
    stream << role << " requested_device=" << requested_device;
    if (const char *visible = std::getenv("ASCEND_RT_VISIBLE_DEVICES"); visible != nullptr) {
        stream << " ASCEND_RT_VISIBLE_DEVICES=" << visible;
    }
    if (const char *device_id = std::getenv("ASCEND_DEVICE_ID"); device_id != nullptr) {
        stream << " ASCEND_DEVICE_ID=" << device_id;
    }

    LogInfo(stream.str());
}

void LogBoundDeviceContext(const std::string &role)
{
    int32_t current_device = -1;
    const aclError device_ret = aclrtGetDevice(&current_device);

    std::ostringstream stream;
    stream << role;
    if (device_ret == ACL_ERROR_NONE) {
        stream << " current_device=" << current_device;
    } else {
        stream << " current_device=<aclrtGetDevice failed:" << static_cast<int>(device_ret) << '>';
    }

    aclrtRunMode run_mode = ACL_DEVICE;
    const aclError run_mode_ret = aclrtGetRunMode(&run_mode);
    if (run_mode_ret == ACL_ERROR_NONE) {
        stream << " run_mode=" << static_cast<int>(run_mode);
    } else {
        stream << " run_mode=<aclrtGetRunMode failed:" << static_cast<int>(run_mode_ret) << '>';
    }

    LogInfo(stream.str());
}

bool FileExists(const std::string &path)
{
    std::ifstream input(path);
    return input.good();
}

std::string DoneFilePath(const std::string &metadata_file)
{
    return metadata_file + ".done";
}

std::string Trim(const std::string &value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::uint64_t ParseUnsigned(const std::string &value, const std::string &name)
{
    if (value.empty()) {
        Fail(name + " must not be empty");
    }
    size_t offset = 0;
    const auto parsed = std::stoull(value, &offset, 10);
    if (offset != value.size()) {
        Fail(name + " must be an unsigned integer, got: " + value);
    }
    return parsed;
}

std::uint64_t ParseByteCount(const std::string &value, const std::string &name)
{
    if (value.empty()) {
        Fail(name + " must not be empty");
    }
    size_t offset = 0;
    const auto number = std::stoull(value, &offset, 10);
    const auto suffix = Trim(value.substr(offset));
    if (suffix.empty() || suffix == "B" || suffix == "b") {
        return number;
    }
    if (suffix == "K" || suffix == "KB" || suffix == "KiB" || suffix == "k" || suffix == "kb" ||
        suffix == "kib") {
        return number * 1024ULL;
    }
    if (suffix == "M" || suffix == "MB" || suffix == "MiB" || suffix == "m" || suffix == "mb" ||
        suffix == "mib") {
        return number * 1024ULL * 1024ULL;
    }
    if (suffix == "G" || suffix == "GB" || suffix == "GiB" || suffix == "g" || suffix == "gb" ||
        suffix == "gib") {
        return number * 1024ULL * 1024ULL * 1024ULL;
    }
    Fail("unsupported byte suffix for " + name + ": " + value);
}

double ToMicroseconds(const Clock::duration &duration)
{
    return static_cast<double>(std::chrono::duration_cast<Microseconds>(duration).count());
}

double BytesPerMicrosecondToGiBPerSecond(std::uint64_t bytes, double total_us)
{
    if (total_us <= 0.0) {
        return 0.0;
    }
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    return (static_cast<double>(bytes) / kGiB) / (total_us / 1'000'000.0);
}

void FillPattern(std::uint8_t *buffer, std::size_t bytes)
{
    for (std::size_t i = 0; i < bytes; ++i) {
        buffer[i] = static_cast<std::uint8_t>((i * 131U + 17U) & 0xFFU);
    }
}

struct Options {
    std::string role;
    int device = 0;
    std::string local_engine;
    std::string remote_engine;
    std::string metadata_file;
    std::uint64_t total_bytes = kDefaultTotalBytes;
    std::uint64_t min_block_bytes = kDefaultMinBlockBytes;
    std::uint64_t max_block_bytes = kDefaultMaxBlockBytes;
    int repeats = kDefaultRepeats;
    int connect_timeout_ms = kDefaultConnectTimeoutMs;
    int completion_timeout_ms = kDefaultCompletionTimeoutMs;
    int metadata_timeout_ms = kDefaultMetadataTimeoutMs;
    int poll_interval_us = kDefaultPollIntervalUs;
    bool print_header = false;
};

struct Metadata {
    std::string remote_engine;
    int device_id = -1;
    std::uint64_t remote_addr = 0;
    std::uint64_t total_bytes = 0;
    bool ready = false;
};

struct DoneSignal {
    bool success = false;
    std::string message;
};

struct HostBuffer {
    void *data = nullptr;
    std::uint64_t bytes = 0;

    explicit HostBuffer(std::uint64_t size_bytes) : bytes(size_bytes)
    {
        CheckAcl(aclrtMallocHost(&data, bytes), "aclrtMallocHost");
    }

    ~HostBuffer()
    {
        if (data != nullptr) {
            (void)aclrtFreeHost(data);
        }
    }

    HostBuffer(const HostBuffer &) = delete;
    HostBuffer &operator=(const HostBuffer &) = delete;
};

struct DeviceBuffer {
    void *data = nullptr;
    std::uint64_t bytes = 0;

    explicit DeviceBuffer(std::uint64_t size_bytes) : bytes(size_bytes)
    {
        CheckAcl(aclrtMalloc(&data, bytes, ACL_MEM_MALLOC_HUGE_FIRST), "aclrtMalloc");
    }

    ~DeviceBuffer()
    {
        if (data != nullptr) {
            (void)aclrtFree(data);
        }
    }

    DeviceBuffer(const DeviceBuffer &) = delete;
    DeviceBuffer &operator=(const DeviceBuffer &) = delete;
};

struct Stream {
    aclrtStream value = nullptr;

    Stream()
    {
        CheckAcl(aclrtCreateStream(&value), "aclrtCreateStream");
    }

    ~Stream()
    {
        if (value != nullptr) {
            (void)aclrtDestroyStream(value);
        }
    }

    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
};

struct HixlEngineGuard {
    hixl::Hixl engine;
    bool connected = false;
    bool initialized = false;
    hixl::MemHandle mem_handle = nullptr;
    std::string remote_engine;

    ~HixlEngineGuard()
    {
        if (connected && !remote_engine.empty()) {
            (void)engine.Disconnect(remote_engine.c_str());
        }
        if (mem_handle != nullptr) {
            (void)engine.DeregisterMem(mem_handle);
        }
        if (initialized) {
            engine.Finalize();
        }
    }
};

struct RepeatStats {
    double submit_us = 0.0;
    double total_us = 0.0;
    std::uint64_t poll_count = 0;
};

struct ResultRow {
    std::string path;
    std::uint64_t total_bytes = 0;
    std::uint64_t block_bytes = 0;
    std::uint64_t transfer_count = 0;
    int repeats = 0;
    double submit_us_avg = 0.0;
    double total_us_avg = 0.0;
    double bandwidth_gib_s = 0.0;
    double poll_count_avg = 0.0;
};

std::vector<std::uint64_t> BuildBlockSizes(const Options &options);

void PrintUsage(const char *program)
{
    std::cerr
        << "Usage: " << program << " --role <server|client|acl> --device <id> [options]\n"
        << "  --local-engine <ip[:port]>\n"
        << "  --remote-engine <ip[:port]>\n"
        << "  --metadata-file <path>\n"
        << "  --total-bytes <bytes|size>\n"
        << "  --min-block-bytes <bytes|size>\n"
        << "  --max-block-bytes <bytes|size>\n"
        << "  --repeats <count>\n"
        << "  --connect-timeout-ms <ms>\n"
        << "  --completion-timeout-ms <ms>\n"
        << "  --metadata-timeout-ms <ms>\n"
        << "  --poll-interval-us <us>\n"
        << "  --print-header\n";
}

Options ParseOptions(int argc, char **argv)
{
    Options options;
    std::map<std::string, std::string> values;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        }
        if (arg == "--print-header") {
            options.print_header = true;
            continue;
        }
        if (arg.rfind("--", 0) != 0) {
            Fail("unexpected positional argument: " + arg);
        }
        if (i + 1 >= argc) {
            Fail("missing value for option: " + arg);
        }
        values[arg] = argv[++i];
    }

    const auto require = [&values](const std::string &name) -> const std::string & {
        const auto it = values.find(name);
        if (it == values.end() || it->second.empty()) {
            Fail("missing required option: " + name);
        }
        return it->second;
    };

    options.role = require("--role");
    options.device = static_cast<int>(ParseUnsigned(require("--device"), "--device"));
    if (values.count("--local-engine") > 0) {
        options.local_engine = values["--local-engine"];
    }
    if (values.count("--remote-engine") > 0) {
        options.remote_engine = values["--remote-engine"];
    }
    if (values.count("--metadata-file") > 0) {
        options.metadata_file = values["--metadata-file"];
    }
    if (values.count("--total-bytes") > 0) {
        options.total_bytes = ParseByteCount(values["--total-bytes"], "--total-bytes");
    }
    if (values.count("--min-block-bytes") > 0) {
        options.min_block_bytes = ParseByteCount(values["--min-block-bytes"], "--min-block-bytes");
    }
    if (values.count("--max-block-bytes") > 0) {
        options.max_block_bytes = ParseByteCount(values["--max-block-bytes"], "--max-block-bytes");
    }
    if (values.count("--repeats") > 0) {
        options.repeats = static_cast<int>(ParseUnsigned(values["--repeats"], "--repeats"));
    }
    if (values.count("--connect-timeout-ms") > 0) {
        options.connect_timeout_ms =
            static_cast<int>(ParseUnsigned(values["--connect-timeout-ms"], "--connect-timeout-ms"));
    }
    if (values.count("--completion-timeout-ms") > 0) {
        options.completion_timeout_ms = static_cast<int>(
            ParseUnsigned(values["--completion-timeout-ms"], "--completion-timeout-ms"));
    }
    if (values.count("--metadata-timeout-ms") > 0) {
        options.metadata_timeout_ms =
            static_cast<int>(ParseUnsigned(values["--metadata-timeout-ms"], "--metadata-timeout-ms"));
    }
    if (values.count("--poll-interval-us") > 0) {
        options.poll_interval_us =
            static_cast<int>(ParseUnsigned(values["--poll-interval-us"], "--poll-interval-us"));
    }

    if (options.total_bytes == 0) {
        Fail("--total-bytes must be greater than zero");
    }
    if (options.min_block_bytes == 0 || options.max_block_bytes == 0) {
        Fail("block sizes must be greater than zero");
    }
    if (options.min_block_bytes > options.max_block_bytes) {
        Fail("--min-block-bytes must be <= --max-block-bytes");
    }
    if (options.repeats <= 0) {
        Fail("--repeats must be greater than zero");
    }
    if (options.completion_timeout_ms <= 0 || options.metadata_timeout_ms <= 0) {
        Fail("timeout options must be greater than zero");
    }
    if (options.role == "server" || options.role == "client") {
        if (options.local_engine.empty()) {
            Fail("--local-engine is required for HIXL roles");
        }
        if (options.metadata_file.empty()) {
            Fail("--metadata-file is required for HIXL roles");
        }
    }
    if (options.role == "client" && options.remote_engine.empty()) {
        Fail("--remote-engine is required for client role");
    }
    if (options.role != "server" && options.role != "client" && options.role != "acl") {
        Fail("unsupported role: " + options.role);
    }
    return options;
}

std::string SerializeMetadata(const Metadata &metadata)
{
    std::ostringstream stream;
    stream << "remote_engine=" << metadata.remote_engine << '\n';
    stream << "device_id=" << metadata.device_id << '\n';
    stream << "remote_addr=" << metadata.remote_addr << '\n';
    stream << "total_bytes=" << metadata.total_bytes << '\n';
    stream << "ready=" << (metadata.ready ? 1 : 0) << '\n';
    return stream.str();
}

Metadata ParseMetadata(const std::string &content)
{
    Metadata metadata;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        const auto key = Trim(line.substr(0, pos));
        const auto value = Trim(line.substr(pos + 1));
        if (key == "remote_engine") {
            metadata.remote_engine = value;
        } else if (key == "device_id") {
            metadata.device_id = static_cast<int>(ParseUnsigned(value, "device_id"));
        } else if (key == "remote_addr") {
            metadata.remote_addr = ParseUnsigned(value, "remote_addr");
        } else if (key == "total_bytes") {
            metadata.total_bytes = ParseUnsigned(value, "total_bytes");
        } else if (key == "ready") {
            metadata.ready = ParseUnsigned(value, "ready") != 0;
        }
    }
    return metadata;
}

void WriteFileAtomic(const std::string &path, const std::string &content)
{
    const std::string tmp_path = path + ".tmp";
    {
        std::ofstream output(tmp_path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            Fail("failed to open file for writing: " + tmp_path);
        }
        output << content;
        output.flush();
        if (!output.good()) {
            Fail("failed to write file: " + tmp_path);
        }
    }
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        Fail("failed to rename " + tmp_path + " to " + path);
    }
}

std::string ReadWholeFile(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        Fail("failed to open file for reading: " + path);
    }
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

Metadata WaitForMetadata(const std::string &path, int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (FileExists(path)) {
            const auto metadata = ParseMetadata(ReadWholeFile(path));
            if (metadata.ready && metadata.remote_addr != 0 && !metadata.remote_engine.empty()) {
                return metadata;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kMetadataPollIntervalMs));
    }
    Fail("timed out waiting for metadata file: " + path);
}

DoneSignal ParseDoneSignal(const std::string &content)
{
    DoneSignal signal;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        const auto pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }
        const auto key = Trim(line.substr(0, pos));
        const auto value = Trim(line.substr(pos + 1));
        if (key == "success") {
            signal.success = ParseUnsigned(value, "success") != 0;
        } else if (key == "message") {
            signal.message = value;
        }
    }
    return signal;
}

void WriteDoneSignal(const std::string &metadata_file, bool success, const std::string &message)
{
    std::ostringstream stream;
    stream << "success=" << (success ? 1 : 0) << '\n';
    stream << "message=" << message << '\n';
    WriteFileAtomic(DoneFilePath(metadata_file), stream.str());
}

DoneSignal WaitForDoneSignal(const std::string &metadata_file, int timeout_ms)
{
    const auto path = DoneFilePath(metadata_file);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline) {
        if (FileExists(path)) {
            return ParseDoneSignal(ReadWholeFile(path));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kMetadataPollIntervalMs));
    }
    Fail("timed out waiting for done signal: " + path);
}

int EstimateServerWaitTimeoutMs(const Options &options)
{
    const auto block_sizes = BuildBlockSizes(options);
    const auto request_count =
        std::max<std::size_t>(1U, block_sizes.size()) * static_cast<std::size_t>(std::max(1, options.repeats));
    const std::uint64_t base_timeout = static_cast<std::uint64_t>(std::max(1, options.completion_timeout_ms));
    const std::uint64_t connect_timeout = static_cast<std::uint64_t>(std::max(1, options.connect_timeout_ms));
    const std::uint64_t metadata_timeout = static_cast<std::uint64_t>(std::max(1, options.metadata_timeout_ms));
    const std::uint64_t slack_timeout = 5'000ULL;
    const std::uint64_t total_timeout =
        request_count * base_timeout + connect_timeout + metadata_timeout + slack_timeout;
    const std::uint64_t capped_timeout =
        std::min<std::uint64_t>(total_timeout, static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
    return static_cast<int>(capped_timeout);
}

void PrintHeader()
{
    std::cout << "path,total_bytes,block_bytes,transfer_count,repeats,submit_us_avg,total_us_avg,"
                 "bandwidth_gib_s,poll_count_avg\n";
}

void PrintResultRow(const ResultRow &row)
{
    std::cout << row.path << ',' << row.total_bytes << ',' << row.block_bytes << ',' << row.transfer_count
              << ',' << row.repeats << ',' << std::fixed << std::setprecision(3) << row.submit_us_avg << ','
              << row.total_us_avg << ',' << row.bandwidth_gib_s << ',' << row.poll_count_avg << '\n';
}

std::vector<std::uint64_t> BuildBlockSizes(const Options &options)
{
    std::vector<std::uint64_t> block_sizes;
    for (std::uint64_t block = options.min_block_bytes; block <= options.max_block_bytes; block *= 2ULL) {
        block_sizes.push_back(block);
        if (block > (std::numeric_limits<std::uint64_t>::max() / 2ULL)) {
            break;
        }
    }
    if (block_sizes.empty()) {
        block_sizes.push_back(options.min_block_bytes);
    }
    return block_sizes;
}

std::vector<hixl::TransferOpDesc> BuildTransferDescs(std::uint8_t *local_base,
                                                     std::uint64_t remote_base,
                                                     std::uint64_t total_bytes,
                                                     std::uint64_t block_bytes)
{
    std::vector<hixl::TransferOpDesc> descs;
    for (std::uint64_t offset = 0; offset < total_bytes; offset += block_bytes) {
        const auto len = std::min(block_bytes, total_bytes - offset);
        descs.push_back(hixl::TransferOpDesc{reinterpret_cast<std::uintptr_t>(local_base + offset),
                                             static_cast<std::uintptr_t>(remote_base + offset),
                                             static_cast<std::size_t>(len)});
    }
    return descs;
}

ResultRow Summarize(const std::string &path,
                    std::uint64_t total_bytes,
                    std::uint64_t block_bytes,
                    std::uint64_t transfer_count,
                    const std::vector<RepeatStats> &stats)
{
    const auto repeats = static_cast<int>(stats.size());
    const auto submit_sum =
        std::accumulate(stats.begin(), stats.end(), 0.0,
                        [](double acc, const RepeatStats &value) { return acc + value.submit_us; });
    const auto total_sum =
        std::accumulate(stats.begin(), stats.end(), 0.0,
                        [](double acc, const RepeatStats &value) { return acc + value.total_us; });
    const auto poll_sum = std::accumulate(
        stats.begin(), stats.end(), 0ULL,
        [](std::uint64_t acc, const RepeatStats &value) { return acc + value.poll_count; });

    ResultRow row;
    row.path = path;
    row.total_bytes = total_bytes;
    row.block_bytes = block_bytes;
    row.transfer_count = transfer_count;
    row.repeats = repeats;
    row.submit_us_avg = submit_sum / static_cast<double>(repeats);
    row.total_us_avg = total_sum / static_cast<double>(repeats);
    row.bandwidth_gib_s = BytesPerMicrosecondToGiBPerSecond(total_bytes, row.total_us_avg);
    row.poll_count_avg = static_cast<double>(poll_sum) / static_cast<double>(repeats);
    return row;
}

void VerifyBytes(const std::vector<std::uint8_t> &actual, const std::vector<std::uint8_t> &expected,
                 const std::string &label)
{
    if (actual.size() != expected.size()) {
        Fail(label + " verification size mismatch");
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != expected[i]) {
            std::ostringstream stream;
            stream << label << " verification failed at byte " << i << ", actual="
                   << static_cast<unsigned int>(actual[i]) << ", expected="
                   << static_cast<unsigned int>(expected[i]);
            Fail(stream.str());
        }
    }
}

std::vector<ResultRow> RunHixlClient(const Options &options)
{
    const auto metadata = WaitForMetadata(options.metadata_file, options.metadata_timeout_ms);
    if (metadata.total_bytes != options.total_bytes) {
        Fail("metadata total_bytes does not match client configuration");
    }

    LogRequestedDeviceContext("client", options.device);
    CheckAcl(aclrtSetDevice(options.device), "aclrtSetDevice(client)");
    LogBoundDeviceContext("client");

    HixlEngineGuard guard;
    std::map<hixl::AscendString, hixl::AscendString> init_options;
    init_options[hixl::OPTION_BUFFER_POOL] = "0:0";
    CheckHixl(guard.engine.Initialize(options.local_engine.c_str(), init_options), "Hixl::Initialize(client)");
    guard.initialized = true;

    HostBuffer host_buffer(options.total_bytes);
    FillPattern(static_cast<std::uint8_t *>(host_buffer.data), host_buffer.bytes);

    hixl::MemDesc mem_desc{};
    mem_desc.addr = reinterpret_cast<std::uintptr_t>(host_buffer.data);
    mem_desc.len = static_cast<std::size_t>(host_buffer.bytes);
    CheckHixl(guard.engine.RegisterMem(mem_desc, hixl::MEM_HOST, guard.mem_handle), "Hixl::RegisterMem(host)");

    const auto remote_engine = metadata.remote_engine.empty() ? options.remote_engine : metadata.remote_engine;
    guard.remote_engine = remote_engine;
    CheckHixl(guard.engine.Connect(remote_engine.c_str(), options.connect_timeout_ms), "Hixl::Connect");
    guard.connected = true;

    std::vector<ResultRow> rows;
    const auto block_sizes = BuildBlockSizes(options);
    for (const auto block_bytes : block_sizes) {
        const auto descs =
            BuildTransferDescs(static_cast<std::uint8_t *>(host_buffer.data), metadata.remote_addr,
                               options.total_bytes, block_bytes);
        std::vector<RepeatStats> stats;
        stats.reserve(static_cast<std::size_t>(options.repeats));
        for (int repeat = 0; repeat < options.repeats; ++repeat) {
            hixl::TransferReq req = nullptr;
            const auto submit_start = Clock::now();
            CheckHixl(guard.engine.TransferAsync(remote_engine.c_str(), hixl::WRITE, descs, {}, req),
                      "Hixl::TransferAsync");
            const auto submit_end = Clock::now();

            hixl::TransferStatus status = hixl::TransferStatus::WAITING;
            std::uint64_t polls = 0;
            const auto deadline = Clock::now() + std::chrono::milliseconds(options.completion_timeout_ms);
            while (status == hixl::TransferStatus::WAITING) {
                if (Clock::now() >= deadline) {
                    Fail("timed out waiting for HIXL async request completion");
                }
                ++polls;
                CheckHixl(guard.engine.GetTransferStatus(req, status), "Hixl::GetTransferStatus");
                if (status == hixl::TransferStatus::WAITING) {
                    std::this_thread::sleep_for(std::chrono::microseconds(options.poll_interval_us));
                }
            }
            if (status != hixl::TransferStatus::COMPLETED) {
                Fail("HIXL async request did not complete successfully");
            }
            const auto done_time = Clock::now();
            stats.push_back(RepeatStats{ToMicroseconds(submit_end - submit_start),
                                        ToMicroseconds(done_time - submit_start), polls});
        }
        rows.push_back(Summarize("hixl_async_h2d", options.total_bytes, block_bytes, descs.size(), stats));
    }

    WriteDoneSignal(options.metadata_file, true, "completed");
    return rows;
}

int RunHixlServer(const Options &options)
{
    LogRequestedDeviceContext("server", options.device);
    CheckAcl(aclrtSetDevice(options.device), "aclrtSetDevice(server)");
    LogBoundDeviceContext("server");

    HixlEngineGuard guard;
    std::map<hixl::AscendString, hixl::AscendString> init_options;
    init_options[hixl::OPTION_BUFFER_POOL] = "0:0";
    CheckHixl(guard.engine.Initialize(options.local_engine.c_str(), init_options), "Hixl::Initialize(server)");
    guard.initialized = true;

    DeviceBuffer device_buffer(options.total_bytes);
    hixl::MemDesc mem_desc{};
    mem_desc.addr = reinterpret_cast<std::uintptr_t>(device_buffer.data);
    mem_desc.len = static_cast<std::size_t>(device_buffer.bytes);
    CheckHixl(guard.engine.RegisterMem(mem_desc, hixl::MEM_DEVICE, guard.mem_handle),
              "Hixl::RegisterMem(device)");

    Metadata metadata;
    metadata.remote_engine = options.local_engine;
    metadata.device_id = options.device;
    metadata.remote_addr = reinterpret_cast<std::uintptr_t>(device_buffer.data);
    metadata.total_bytes = options.total_bytes;
    metadata.ready = true;
    WriteFileAtomic(options.metadata_file, SerializeMetadata(metadata));
    LogInfo("server metadata written to " + options.metadata_file);

    const auto server_wait_timeout_ms = EstimateServerWaitTimeoutMs(options);
    LogInfo("server waiting for client completion, timeout_ms=" + std::to_string(server_wait_timeout_ms));
    const auto done = WaitForDoneSignal(options.metadata_file, server_wait_timeout_ms);
    if (!done.success) {
        Fail("client reported failure: " + done.message);
    }

    std::vector<std::uint8_t> actual(static_cast<std::size_t>(options.total_bytes));
    std::vector<std::uint8_t> expected(static_cast<std::size_t>(options.total_bytes));
    FillPattern(expected.data(), expected.size());
    CheckAcl(aclrtMemcpy(actual.data(), actual.size(), device_buffer.data, options.total_bytes,
                         ACL_MEMCPY_DEVICE_TO_HOST),
             "aclrtMemcpy(server verify)");
    VerifyBytes(actual, expected, "HIXL server");
    LogInfo("server verification succeeded");
    return 0;
}

std::vector<ResultRow> RunAclBaseline(const Options &options)
{
    LogRequestedDeviceContext("acl", options.device);
    CheckAcl(aclrtSetDevice(options.device), "aclrtSetDevice(acl)");
    LogBoundDeviceContext("acl");

    HostBuffer host_buffer(options.total_bytes);
    DeviceBuffer device_buffer(options.total_bytes);
    FillPattern(static_cast<std::uint8_t *>(host_buffer.data), host_buffer.bytes);

    std::vector<ResultRow> rows;
    const auto block_sizes = BuildBlockSizes(options);
    for (const auto block_bytes : block_sizes) {
        const std::uint64_t transfer_count = (options.total_bytes + block_bytes - 1ULL) / block_bytes;
        std::vector<RepeatStats> stats;
        stats.reserve(static_cast<std::size_t>(options.repeats));
        for (int repeat = 0; repeat < options.repeats; ++repeat) {
            Stream stream;
            const auto submit_start = Clock::now();
            for (std::uint64_t offset = 0; offset < options.total_bytes; offset += block_bytes) {
                const auto len = std::min(block_bytes, options.total_bytes - offset);
                auto *dst = static_cast<std::uint8_t *>(device_buffer.data) + offset;
                auto *src = static_cast<std::uint8_t *>(host_buffer.data) + offset;
                CheckAcl(aclrtMemcpyAsync(dst, len, src, len, ACL_MEMCPY_HOST_TO_DEVICE, stream.value),
                         "aclrtMemcpyAsync");
            }
            const auto submit_end = Clock::now();
            CheckAcl(aclrtSynchronizeStream(stream.value), "aclrtSynchronizeStream");
            const auto done_time = Clock::now();
            stats.push_back(
                RepeatStats{ToMicroseconds(submit_end - submit_start), ToMicroseconds(done_time - submit_start), 0});
        }

        std::vector<std::uint8_t> actual(static_cast<std::size_t>(options.total_bytes));
        std::vector<std::uint8_t> expected(static_cast<std::size_t>(options.total_bytes));
        FillPattern(expected.data(), expected.size());
        CheckAcl(aclrtMemcpy(actual.data(), actual.size(), device_buffer.data, options.total_bytes,
                             ACL_MEMCPY_DEVICE_TO_HOST),
                 "aclrtMemcpy(acl verify)");
        VerifyBytes(actual, expected, "ACL baseline");

        rows.push_back(Summarize("aclrtMemcpyAsync_h2d", options.total_bytes, block_bytes, transfer_count, stats));
    }
    return rows;
}

}  // namespace

int main(int argc, char **argv)
{
    Options options;
    bool client_done_written = false;
    try {
        options = ParseOptions(argc, argv);
        if (options.role == "server") {
            return RunHixlServer(options);
        }

        std::vector<ResultRow> rows;
        if (options.role == "client") {
            rows = RunHixlClient(options);
            client_done_written = true;
        } else {
            rows = RunAclBaseline(options);
        }

        if (options.print_header) {
            PrintHeader();
        }
        for (const auto &row : rows) {
            PrintResultRow(row);
        }
        return 0;
    } catch (const std::exception &error) {
        if (options.role == "client" && !options.metadata_file.empty() && !client_done_written) {
            try {
                WriteDoneSignal(options.metadata_file, false, error.what());
            } catch (...) {
            }
        }
        std::cerr << "[hixlbw][error] " << error.what() << '\n';
        return 1;
    }
}
