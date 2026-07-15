/*
 * Copyright (c) 2026 ByteDance Inc.
 *
 * This file is part of veSAL.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <csignal>
#include <fstream>
#include <gflags/gflags.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "vesal/codec.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

namespace {

DEFINE_string(input_path,
              "",
              "Path to a timeout_recent_*.bin file or a directory containing recent dump files");
DEFINE_string(replay_mode,
              "per_file",
              "Replay mode: per_file creates one worker per recent file; merged replays all "
              "records sorted by submit_ts_ns in one worker");
DEFINE_string(replay_direction,
              "all",
              "Replay request direction: all, comp, or decomp");
DEFINE_uint32(loop_num, 1, "Number of times to replay the loaded records");
DEFINE_uint32(inflight_num, 16, "Maximum async requests in flight per replay worker");
DEFINE_uint32(max_records, 0, "Maximum records to replay per worker. 0 means all records");
DEFINE_uint32(timeout_ms, 3000, "Timeout waiting for an async request completion");
DEFINE_uint32(post_timeout_poll_ms,
              17000,
              "Extra polling time after first timeout before force cleanup. 17000ms means 20s "
              "total with timeout_ms=3000");
DEFINE_uint32(mem_pool_mb, 1024, "veSAL hugepage memory pool preallocation size in MiB");
DEFINE_bool(debug, false, "Enable verbose replay progress logging");

constexpr size_t kRecentRecordHeaderSize = 44;
constexpr size_t kMaxCrashSlots = 4096;

struct CrashRecord {
    uint64_t seq{0};
    uint64_t req_id{0};
    uint64_t submit_ts_ns{0};
    uint64_t submit_wall_ms{0};
    uint32_t src_total_len{0};
    uint32_t dst_size{0};
    uint32_t num_blocks{0};
    uint32_t payload_len{0};
    uint8_t direction{0};
    uint8_t codec_algo{0};
    uint8_t comp_level{0};
    uint8_t payload_truncated{0};
    pid_t tid{0};
    char replay_name[96]{};
    char source_file[512]{};
};

CrashRecord g_crash_records[kMaxCrashSlots];
volatile sig_atomic_t g_crash_active[kMaxCrashSlots];
volatile sig_atomic_t g_handling_crash_signal = 0;
std::atomic<uint64_t> g_submit_seq{1};
std::atomic<bool> g_timeout_warning_printed_global{false};

struct RecentRecord {
    uint64_t payload_seq_in_file{0};
    uint64_t req_id{0};
    uint64_t submit_ts_ns{0};
    uint64_t submit_wall_ms{0};
    uint8_t direction{0};
    uint8_t codec_algo{0};
    uint8_t comp_level{0};
    uint8_t payload_truncated{0};
    uint32_t src_total_len{0};
    uint32_t dst_size{0};
    uint32_t num_blocks{0};
    uint32_t payload_len{0};
    std::vector<unsigned char> payload;
    std::string source_file;
};

struct RecentFile {
    std::string path;
    std::string header;
    uint64_t trigger_req_id{0};
    uint64_t entries{0};
    uint64_t ring_depth{0};
    uint64_t total_pushes{0};
    uint64_t pid{0};
    uint64_t tid{0};
    uint64_t ts_ms{0};
    std::vector<RecentRecord> records;
};

struct ActiveRequest {
    const RecentRecord* record{nullptr};
    std::vector<unsigned char> dst;
    vesal::IOBlock src_block;
    vesal::IOBlock dst_block;
    vesal::CodecRequestArgs args;
    std::chrono::steady_clock::time_point submit_time;
    size_t crash_slot{kMaxCrashSlots};
};

struct ReplayStats {
    uint64_t submitted{0};
    uint64_t completed{0};
    uint64_t compression{0};
    uint64_t decompression{0};
    uint64_t bytes_in{0};
    uint64_t bytes_out{0};
};

struct ReplayResult {
    bool ok{true};
    int code{0};
    std::string error;
    ReplayStats stats;
    bool abandoned_unsafe_channel{false};
};

enum class ReplayDirectionFilter {
    kAll,
    kComp,
    kDecomp,
};

void AddStats(ReplayStats* total, const ReplayStats& delta) {
    total->submitted += delta.submitted;
    total->completed += delta.completed;
    total->compression += delta.compression;
    total->decompression += delta.decompression;
    total->bytes_in += delta.bytes_in;
    total->bytes_out += delta.bytes_out;
}

ReplayDirectionFilter ParseReplayDirectionFilter(const std::string& value) {
    if (value == "all") {
        return ReplayDirectionFilter::kAll;
    }
    if (value == "comp") {
        return ReplayDirectionFilter::kComp;
    }
    if (value == "decomp") {
        return ReplayDirectionFilter::kDecomp;
    }
    throw std::runtime_error(
        "Invalid --replay_direction, expected one of: all, comp, decomp");
}

bool ShouldReplayRecord(const RecentRecord& record, ReplayDirectionFilter filter) {
    switch (filter) {
        case ReplayDirectionFilter::kAll:
            return true;
        case ReplayDirectionFilter::kComp:
            return record.direction == static_cast<uint8_t>(vesal::CodecDirection::kComp);
        case ReplayDirectionFilter::kDecomp:
            return record.direction == static_cast<uint8_t>(vesal::CodecDirection::kDecomp);
    }
    return false;
}

void CopyCString(char* dst, size_t dst_size, const std::string& src) {
    if (dst_size == 0) {
        return;
    }
    size_t n = std::min(dst_size - 1, src.size());
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

void AppendChar(char* buf, size_t buf_size, size_t* pos, char c) {
    if (*pos + 1 < buf_size) {
        buf[(*pos)++] = c;
        buf[*pos] = '\0';
    }
}

void AppendStr(char* buf, size_t buf_size, size_t* pos, const char* s) {
    while (*s != '\0' && *pos + 1 < buf_size) {
        buf[(*pos)++] = *s++;
    }
    buf[*pos] = '\0';
}

void AppendUint(char* buf, size_t buf_size, size_t* pos, uint64_t value) {
    char tmp[32];
    size_t tmp_pos = sizeof(tmp);
    tmp[--tmp_pos] = '\0';
    if (value == 0) {
        tmp[--tmp_pos] = '0';
    } else {
        while (value > 0 && tmp_pos > 0) {
            tmp[--tmp_pos] = static_cast<char>('0' + value % 10);
            value /= 10;
        }
    }
    AppendStr(buf, buf_size, pos, &tmp[tmp_pos]);
}

const char* SignalName(int sig) {
    switch (sig) {
    case SIGABRT:
        return "SIGABRT";
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGILL:
        return "SIGILL";
    case SIGFPE:
        return "SIGFPE";
    default:
        return "UNKNOWN";
    }
}

void AppendCrashRecord(char* buf, size_t buf_size, size_t* pos, const CrashRecord& record) {
    AppendStr(buf, buf_size, pos, "data_replay earliest in-flight request: seq=");
    AppendUint(buf, buf_size, pos, record.seq);
    AppendStr(buf, buf_size, pos, ", req_id=");
    AppendUint(buf, buf_size, pos, record.req_id);
    AppendStr(buf, buf_size, pos, ", direction=");
    AppendUint(buf, buf_size, pos, record.direction);
    AppendStr(buf, buf_size, pos, ", codec_algo=");
    AppendUint(buf, buf_size, pos, record.codec_algo);
    AppendStr(buf, buf_size, pos, ", comp_level=");
    AppendUint(buf, buf_size, pos, record.comp_level);
    AppendStr(buf, buf_size, pos, ", payload_truncated=");
    AppendUint(buf, buf_size, pos, record.payload_truncated);
    AppendStr(buf, buf_size, pos, ", src_total_len=");
    AppendUint(buf, buf_size, pos, record.src_total_len);
    AppendStr(buf, buf_size, pos, ", dst_size=");
    AppendUint(buf, buf_size, pos, record.dst_size);
    AppendStr(buf, buf_size, pos, ", num_blocks=");
    AppendUint(buf, buf_size, pos, record.num_blocks);
    AppendStr(buf, buf_size, pos, ", payload_len=");
    AppendUint(buf, buf_size, pos, record.payload_len);
    AppendStr(buf, buf_size, pos, ", submit_ts_ns=");
    AppendUint(buf, buf_size, pos, record.submit_ts_ns);
    AppendStr(buf, buf_size, pos, ", submit_wall_ms=");
    AppendUint(buf, buf_size, pos, record.submit_wall_ms);
    AppendStr(buf, buf_size, pos, ", tid=");
    AppendUint(buf, buf_size, pos, static_cast<uint64_t>(record.tid));
    AppendStr(buf, buf_size, pos, ", replay_name=");
    AppendStr(buf, buf_size, pos, record.replay_name);
    AppendStr(buf, buf_size, pos, ", source_bin=");
    AppendStr(buf, buf_size, pos, record.source_file);
    AppendChar(buf, buf_size, pos, '\n');
}

void CrashSignalHandler(int sig) {
    if (g_handling_crash_signal) {
        for (;;) {
        }
    }
    g_handling_crash_signal = 1;

    char msg[2048] = {};
    size_t pos = 0;
    AppendStr(msg, sizeof(msg), &pos, "\ndata_replay caught fatal signal: ");
    AppendStr(msg, sizeof(msg), &pos, SignalName(sig));
    AppendChar(msg, sizeof(msg), &pos, '\n');

    int best = -1;
    uint64_t best_seq = 0;
    for (size_t i = 0; i < kMaxCrashSlots; ++i) {
        if (!g_crash_active[i]) {
            continue;
        }
        uint64_t seq = g_crash_records[i].seq;
        if (seq != 0 && (best < 0 || seq < best_seq)) {
            best = static_cast<int>(i);
            best_seq = seq;
        }
    }
    if (best >= 0) {
        AppendCrashRecord(msg, sizeof(msg), &pos, g_crash_records[best]);
    } else {
        AppendStr(msg, sizeof(msg), &pos, "data_replay has no recorded in-flight request\n");
    }

    if (pos > 0) {
        ssize_t ignored = write(STDERR_FILENO, msg, pos);
        (void)ignored;
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

void InstallCrashSignalHandlers() {
    signal(SIGABRT, CrashSignalHandler);
    signal(SIGSEGV, CrashSignalHandler);
    signal(SIGBUS, CrashSignalHandler);
    signal(SIGILL, CrashSignalHandler);
    signal(SIGFPE, CrashSignalHandler);
}

size_t RegisterCrashRecord(const RecentRecord& record, const std::string& replay_name) {
    uint64_t seq = g_submit_seq.fetch_add(1, std::memory_order_relaxed);
    size_t slot = seq % kMaxCrashSlots;
    g_crash_active[slot] = 0;
    CrashRecord& crash_record = g_crash_records[slot];
    crash_record = CrashRecord{};
    crash_record.seq = seq;
    crash_record.req_id = record.req_id;
    crash_record.submit_ts_ns = record.submit_ts_ns;
    crash_record.submit_wall_ms = record.submit_wall_ms;
    crash_record.src_total_len = record.src_total_len;
    crash_record.dst_size = record.dst_size;
    crash_record.num_blocks = record.num_blocks;
    crash_record.payload_len = record.payload_len;
    crash_record.direction = record.direction;
    crash_record.codec_algo = record.codec_algo;
    crash_record.comp_level = record.comp_level;
    crash_record.payload_truncated = record.payload_truncated;
    crash_record.tid = static_cast<pid_t>(syscall(SYS_gettid));
    CopyCString(crash_record.replay_name, sizeof(crash_record.replay_name), replay_name);
    CopyCString(crash_record.source_file, sizeof(crash_record.source_file), record.source_file);
    g_crash_active[slot] = 1;
    return slot;
}

void ClearCrashRecord(size_t slot) {
    if (slot < kMaxCrashSlots) {
        g_crash_active[slot] = 0;
    }
}

uint64_t ReadLe64(const unsigned char* data) {
    uint64_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return v;
}

uint32_t ReadLe32(const unsigned char* data) {
    uint32_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    return v;
}

bool IsRegularFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool IsDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string BaseName(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

void FindRecentFiles(const std::string& path, std::vector<std::string>* files) {
    if (IsRegularFile(path)) {
        std::string name = BaseName(path);
        if (StartsWith(name, "timeout_recent_") && EndsWith(name, ".bin")) {
            files->push_back(path);
        }
        return;
    }
    if (!IsDirectory(path)) {
        return;
    }

    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return;
    }
    while (dirent* ent = readdir(dir)) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        FindRecentFiles(path + "/" + name, files);
    }
    closedir(dir);
}

uint64_t ParseHeaderValue(const std::string& header, const std::string& key) {
    std::string token = key + "=";
    size_t pos = header.find(token);
    if (pos == std::string::npos) {
        throw std::runtime_error("Missing header key: " + key);
    }
    pos += token.size();
    size_t end = header.find(' ', pos);
    return std::stoull(header.substr(pos, end == std::string::npos ? end : end - pos));
}

RecentRecord ParseRecordHeader(const unsigned char* raw) {
    RecentRecord r;
    r.req_id = ReadLe64(raw);
    r.submit_ts_ns = ReadLe64(raw + 8);
    r.submit_wall_ms = ReadLe64(raw + 16);
    r.direction = raw[24];
    r.codec_algo = raw[25];
    r.comp_level = raw[26];
    r.payload_truncated = raw[27];
    r.src_total_len = ReadLe32(raw + 28);
    r.dst_size = ReadLe32(raw + 32);
    r.num_blocks = ReadLe32(raw + 36);
    r.payload_len = ReadLe32(raw + 40);
    return r;
}

RecentFile LoadRecentFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open " + path);
    }

    RecentFile file;
    file.path = path;
    std::getline(in, file.header);
    if (!StartsWith(file.header, "VESAL_RECENT_REQUESTS v1 ")) {
        throw std::runtime_error("Invalid recent header in " + path + ": " + file.header);
    }

    file.entries = ParseHeaderValue(file.header, "entries");
    file.ring_depth = ParseHeaderValue(file.header, "ring_depth");
    file.total_pushes = ParseHeaderValue(file.header, "total_pushes");
    file.trigger_req_id = ParseHeaderValue(file.header, "trigger_req_id");
    file.pid = ParseHeaderValue(file.header, "pid");
    file.tid = ParseHeaderValue(file.header, "tid");
    file.ts_ms = ParseHeaderValue(file.header, "ts_ms");
    file.records.reserve(file.entries);

    unsigned char raw[kRecentRecordHeaderSize];
    for (uint64_t i = 0; i < file.entries; ++i) {
        in.read(reinterpret_cast<char*>(raw), sizeof(raw));
        if (in.gcount() != static_cast<std::streamsize>(sizeof(raw))) {
            throw std::runtime_error("Truncated record header in " + path);
        }
        RecentRecord record = ParseRecordHeader(raw);
        record.source_file = path;
        record.payload.resize(record.payload_len);
        if (record.payload_len > 0) {
            in.read(reinterpret_cast<char*>(record.payload.data()), record.payload_len);
            if (in.gcount() != static_cast<std::streamsize>(record.payload_len)) {
                throw std::runtime_error("Truncated record payload in " + path);
            }
        }
        record.payload_seq_in_file = i;
        file.records.push_back(std::move(record));
    }
    return file;
}

vesal::CodecAlgorithm CodecAlgorithmFromRecord(uint8_t algo) {
    switch (algo) {
    case static_cast<uint8_t>(vesal::CodecAlgorithm::kLz4):
        return vesal::CodecAlgorithm::kLz4;
    case static_cast<uint8_t>(vesal::CodecAlgorithm::kZstd):
        return vesal::CodecAlgorithm::kZstd;
    case static_cast<uint8_t>(vesal::CodecAlgorithm::kDeflate):
        return vesal::CodecAlgorithm::kDeflate;
    case static_cast<uint8_t>(vesal::CodecAlgorithm::kZlib):
        return vesal::CodecAlgorithm::kZlib;
    default:
        return vesal::CodecAlgorithm::kNum;
    }
}

vesal::CodecCompLevel CodecCompLevelFromRecord(uint8_t level) {
    if (level >= static_cast<uint8_t>(vesal::CodecCompLevel::kLevel1) &&
        level < static_cast<uint8_t>(vesal::CodecCompLevel::kNum)) {
        return static_cast<vesal::CodecCompLevel>(level);
    }
    return vesal::CodecCompLevel::kNum;
}

int ErrorCodeForStatus(vesal::StatusCode status) {
    if (status == vesal::StatusCode::kSliceHang) {
        return 24;
    }
    if (status == vesal::StatusCode::kTimeout) {
        return 20;
    }
    if (status == vesal::StatusCode::kOverflow) {
        return 21;
    }
    if (status == vesal::StatusCode::kBadData) {
        return 22;
    }
    if (status == vesal::StatusCode::kPermanentError) {
        return 23;
    }
    return 14;
}

std::string RequestType(const RecentRecord& record) {
    if (record.direction == static_cast<uint8_t>(vesal::CodecDirection::kComp)) {
        return "compress";
    }
    if (record.direction == static_cast<uint8_t>(vesal::CodecDirection::kDecomp)) {
        return "decompress";
    }
    return "unknown";
}

std::string RecordDebugString(const RecentRecord& record) {
    std::ostringstream oss;
    oss << "payload_seq_in_file=" << record.payload_seq_in_file
        << ", req=" << record.req_id << ", type=" << RequestType(record)
        << ", direction=" << static_cast<int>(record.direction)
        << ", codec_algo=" << static_cast<int>(record.codec_algo)
        << ", comp_level=" << static_cast<int>(record.comp_level)
        << ", payload_truncated=" << static_cast<int>(record.payload_truncated)
        << ", src_total_len=" << record.src_total_len << ", dst_size=" << record.dst_size
        << ", num_blocks=" << record.num_blocks << ", payload_len=" << record.payload_len
        << ", submit_ts_ns=" << record.submit_ts_ns
        << ", submit_wall_ms=" << record.submit_wall_ms << ", source_bin=" << record.source_file;
    return oss.str();
}

std::string CurrentTimeString() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);

    tm local_tm{};
    localtime_r(&ts.tv_sec, &local_tm);

    char time_buf[32];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_tm);

    char result[64];
    std::snprintf(result, sizeof(result), "[%s.%09ld]", time_buf, ts.tv_nsec);
    return result;
}

std::unique_ptr<ActiveRequest> BuildActiveRequest(const RecentRecord& record) {
    auto active = std::make_unique<ActiveRequest>();
    active->record = &record;
    active->dst.resize(record.dst_size == 0 ? record.src_total_len * 2 : record.dst_size);

    active->src_block.size = record.payload_len;
    active->src_block.data = const_cast<unsigned char*>(record.payload.data());
    active->src_block.ctx = nullptr;
    active->src_block.deleter = nullptr;

    active->dst_block.size = active->dst.size();
    active->dst_block.data = active->dst.data();
    active->dst_block.ctx = nullptr;
    active->dst_block.deleter = nullptr;

    active->args.src.num_blocks = 1;
    active->args.src.blocks = &active->src_block;
    active->args.dst = active->dst_block;
    active->args.ctx = active.get();
    active->args.direction = static_cast<vesal::CodecDirection>(record.direction);
    active->args.session_opt.codec_algorithm = CodecAlgorithmFromRecord(record.codec_algo);
    active->args.session_opt.comp_level = CodecCompLevelFromRecord(record.comp_level);
    return active;
}

ReplayResult ReplayRecords(const std::string& name, const std::vector<const RecentRecord*>& records) {
    ReplayResult replay_result;
    if (records.empty()) {
        return replay_result;
    }

    vesal::CodecChannelOption channel_opt;
    channel_opt.mode = vesal::ChannelMode::kDedicated;
    channel_opt.engine_type = vesal::CodecEngineType::kQat;
    channel_opt.comp_algorithm = CodecAlgorithmFromRecord(records.front()->codec_algo);
    channel_opt.comp_level = CodecCompLevelFromRecord(records.front()->comp_level);
    channel_opt.checksum_type = vesal::CodecChecksumType::kCrc32;
    //channel_opt.checksum_type = vesal::CodecChecksumType::kNone;
    channel_opt.timeout_ms = FLAGS_timeout_ms;

    auto channel_pair = vesal::CodecChannel::CreateCodecChannel(channel_opt);
    if (!channel_pair.first.ok()) {
        replay_result.ok = false;
        replay_result.code = 2;
        replay_result.error = name + " CreateCodecChannel failed: " + channel_pair.first.ToString();
        return replay_result;
    }
    std::unique_ptr<vesal::CodecChannel> channel = std::move(channel_pair.second);

    std::deque<std::unique_ptr<ActiveRequest>> active_requests;
    size_t next_index = 0;
    size_t limit = records.size();
    if (FLAGS_max_records > 0) {
        limit = std::min<size_t>(limit, FLAGS_max_records);
    }
    const auto timeout = std::chrono::milliseconds(FLAGS_timeout_ms);
    auto start = std::chrono::steady_clock::now();
    bool timeout_detected = false;
    bool timeout_warning_printed = false;

    auto set_error = [&](const std::string& error, int code) {
        replay_result.ok = false;
        replay_result.code = code;
        replay_result.error = error;
    };

    auto mark_timeout_if_needed = [&](const char* phase) {
        if (active_requests.empty()) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = now - active_requests.front()->submit_time;
        if (elapsed > timeout) {
            timeout_detected = true;
            if (!timeout_warning_printed) {
                bool expected = false;
                if (g_timeout_warning_printed_global.compare_exchange_strong(
                        expected, true, std::memory_order_acq_rel)) {
                    const RecentRecord& timeout_record = *active_requests.front()->record;
                    const auto elapsed_ms =
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
                    std::cerr << CurrentTimeString() << " " << name
                              << " warning: request elapsed " << elapsed_ms
                              << " ms exceeds timeout_ms=" << FLAGS_timeout_ms
                              << ", source_bin=" << BaseName(timeout_record.source_file)
                              << ", payload_seq_in_file=" << timeout_record.payload_seq_in_file
                              << ", req_id=" << timeout_record.req_id
                              << ", continue submit/poll, phase=" << phase << std::endl;
                }
                timeout_warning_printed = true;
            }
        }
    };

    auto submit_next = [&]() -> bool {
        const RecentRecord& record = *records[next_index];
        if (record.payload_truncated != 0 || record.payload_len != record.src_total_len) {
            std::ostringstream oss;
            oss << name << " unsupported record req=" << record.req_id
                << ", payload_truncated=" << static_cast<int>(record.payload_truncated)
                << ", src_total_len=" << record.src_total_len
                << ", payload_len=" << record.payload_len;
            set_error(oss.str(), 3);
            return false;
        }
        if (CodecAlgorithmFromRecord(record.codec_algo) == vesal::CodecAlgorithm::kNum ||
            CodecCompLevelFromRecord(record.comp_level) == vesal::CodecCompLevel::kNum ||
            record.direction >= static_cast<uint8_t>(vesal::CodecDirection::kNum)) {
            std::ostringstream oss;
            oss << name << " unsupported codec fields req=" << record.req_id
                << ", direction=" << static_cast<int>(record.direction)
                << ", codec_algo=" << static_cast<int>(record.codec_algo)
                << ", comp_level=" << static_cast<int>(record.comp_level);
            set_error(oss.str(), 4);
            return false;
        }

        auto active = BuildActiveRequest(record);
        active->crash_slot = RegisterCrashRecord(record, name);
        active->submit_time = std::chrono::steady_clock::now();
        vesal::StatusCode status = channel->SubmitAsync(active->args);
        if (!vesal::IsOk(status)) {
            ClearCrashRecord(active->crash_slot);
            std::ostringstream oss;
            oss << name << " submit failed " << RecordDebugString(record)
                << ", status=" << vesal::StatusCodeToString(status);
            set_error(oss.str(), ErrorCodeForStatus(status));
            return false;
        }
        active_requests.push_back(std::move(active));
        ++next_index;
        ++replay_result.stats.submitted;
        return true;
    };

    vesal::CodecResult results[64];
    while (replay_result.ok &&
           (!active_requests.empty() || next_index < limit)) {
        while (next_index < limit && active_requests.size() < FLAGS_inflight_num) {
            submit_next();
        }

        ssize_t n = channel->Poll(results, 64, 0);

        mark_timeout_if_needed("post-poll completion check");
        if (n <= 0) {
            usleep(100);
            continue;
        }

        for (ssize_t i = 0; i < n; ++i) {
            auto* completed = static_cast<ActiveRequest*>(results[i].ctx);
            auto it = std::find_if(active_requests.begin(), active_requests.end(), [completed](const std::unique_ptr<ActiveRequest>& ptr) {
                return ptr.get() == completed;
            });
            if (it == active_requests.end()) {
                set_error(name + " got completion with unknown ctx", 14);
                break;
            }

            const RecentRecord& record = *(*it)->record;
            if (!vesal::IsOk(results[i].status)) {
                std::cerr << CurrentTimeString() << " " << name
                          << " warning: drop failed response "
                          << RecordDebugString(record)
                          << ", status=" << vesal::StatusCodeToString(results[i].status)
                          << std::endl;
                ClearCrashRecord((*it)->crash_slot);
                active_requests.erase(it);
                continue;
            }

            ClearCrashRecord((*it)->crash_slot);
            ++replay_result.stats.completed;
            replay_result.stats.bytes_in += record.src_total_len;
            replay_result.stats.bytes_out += results[i].produced;
            if (record.direction == static_cast<uint8_t>(vesal::CodecDirection::kComp)) {
                ++replay_result.stats.compression;
            } else if (record.direction == static_cast<uint8_t>(vesal::CodecDirection::kDecomp)) {
                ++replay_result.stats.decompression;
            }
            active_requests.erase(it);
        }
    }

    if (!active_requests.empty()) {
        if (replay_result.ok) {
            std::ostringstream oss;
            oss << name << " still has " << active_requests.size()
                << " in-flight requests before channel close";
            set_error(oss.str(), ErrorCodeForStatus(vesal::StatusCode::kTimeout));
        }
        replay_result.abandoned_unsafe_channel = true;
        for (auto& active_request : active_requests) {
            ClearCrashRecord(active_request->crash_slot);
            active_request.release();
        }
        active_requests.clear();
        channel.release();
    } else {
        vesal::Status close_status = channel->Close();
        if (!close_status.ok() && replay_result.ok) {
            replay_result.ok = false;
            replay_result.code = 5;
            replay_result.error = name + " channel close failed: " + close_status.ToString();
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now() - start).count();
    double gib_per_s = elapsed > 0 ? replay_result.stats.bytes_in / 1024.0 / 1024.0 / 1024.0 / elapsed : 0;
    if (FLAGS_debug) {
        std::cout << CurrentTimeString() << " Replay worker done, name=" << name
              << ", ok=" << replay_result.ok
                  << ", submitted=" << replay_result.stats.submitted
                  << ", completed=" << replay_result.stats.completed
                  << ", compression=" << replay_result.stats.compression
                  << ", decompression=" << replay_result.stats.decompression
                  << ", bytes_in=" << replay_result.stats.bytes_in
                  << ", bytes_out=" << replay_result.stats.bytes_out << ", elapsed=" << elapsed
                  << " s, throughput=" << gib_per_s << " GiB/s" << std::endl;
    }
    return replay_result;
}

int PrintAndReturn(const std::string& msg, int code) {
    std::cerr << CurrentTimeString() << " " << msg << std::endl;
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    InstallCrashSignalHandlers();
    gflags::SetUsageMessage(
        "Replay bgworker timeout_recent_*.bin request dumps through veSAL QAT.\n"
        "\n"
        "Examples:\n"
        "  data_replay --input_path=/path/to/sample_dir --replay_mode=per_file "
        "--inflight_num=64 --loop_num=10 --mem_pool_mb=1024 "
        "--vesal_codec_qat_section_name=SSL\n"
        "  data_replay --input_path=/path/to/timeout_recent.bin --replay_mode=merged "
        "--max_records=1024\n"
        "\n"
        "Replay modes:\n"
        "  per_file  One worker and one dedicated QAT channel per timeout_recent file.\n"
        "  merged    Merge all records by submit_ts_ns and replay them with one worker.\n"
        "\n"
        "The replay uses payloads embedded in timeout_recent_*.bin and checks only request "
        "completion status.\n");
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    ReplayDirectionFilter replay_direction_filter;
    try {
        replay_direction_filter = ParseReplayDirectionFilter(FLAGS_replay_direction);
    } catch (const std::exception& ex) {
        return PrintAndReturn(ex.what(), 1);
    }

    if (FLAGS_input_path.empty()) {
        return PrintAndReturn("input_path must be set", 1);
    }
    if (FLAGS_inflight_num == 0 || FLAGS_inflight_num > 256) {
        return PrintAndReturn("inflight_num must be in range [1, 256]", 1);
    }
    if (FLAGS_loop_num == 0) {
        return PrintAndReturn("loop_num must be > 0", 1);
    }
    if (FLAGS_mem_pool_mb == 0) {
        return PrintAndReturn("mem_pool_mb must be > 0", 1);
    }
    if (FLAGS_replay_mode != "per_file" && FLAGS_replay_mode != "merged") {
        return PrintAndReturn("replay_mode must be one of: per_file, merged", 1);
    }

    std::vector<std::string> recent_paths;
    FindRecentFiles(FLAGS_input_path, &recent_paths);
    std::sort(recent_paths.begin(), recent_paths.end());
    if (recent_paths.empty()) {
        return PrintAndReturn("No timeout_recent_*.bin files found under input_path", 1);
    }

    std::vector<RecentFile> recent_files;
    recent_files.reserve(recent_paths.size());
    try {
        for (const auto& path : recent_paths) {
            RecentFile file = LoadRecentFile(path);
            if (FLAGS_debug) {
                std::cout << CurrentTimeString() << " Loaded recent file, path=" << path
                          << ", entries=" << file.records.size()
                          << ", trigger_req_id=" << file.trigger_req_id << ", pid=" << file.pid
                          << ", tid=" << file.tid << std::endl;
            }
            recent_files.push_back(std::move(file));
        }
    } catch (const std::exception& ex) {
        return PrintAndReturn(ex.what(), 1);
    }

    vesal::InitOptions init_opt;
    init_opt.codec_init_opt.init_qat = true;
    init_opt.cypher_init_opt.init_qat = false;
    init_opt.data_flow_init_opt.init_dsa = false;
    init_opt.mem_pool_init_opt.init_mem_pool = true;
    init_opt.mem_pool_init_opt.prealloc_page_size = vesal::HugePageSize::k2MB;
    init_opt.mem_pool_init_opt.prealloc_size_mb = FLAGS_mem_pool_mb;

    if (!vesal::Init(init_opt)) {
        return PrintAndReturn("vesal::Init failed", 1);
    }

    auto replay_start = std::chrono::steady_clock::now();
    std::vector<ReplayResult> results;
    if (FLAGS_replay_mode == "merged") {
        std::vector<const RecentRecord*> records;
        for (const auto& file : recent_files) {
            for (const auto& record : file.records) {
                if (ShouldReplayRecord(record, replay_direction_filter)) {
                    records.push_back(&record);
                }
            }
        }
        std::sort(records.begin(), records.end(), [](const RecentRecord* lhs, const RecentRecord* rhs) {
            if (lhs->submit_ts_ns != rhs->submit_ts_ns) {
                return lhs->submit_ts_ns < rhs->submit_ts_ns;
            }
            return lhs->req_id < rhs->req_id;
        });
        ReplayResult merged_total;
        for (uint32_t loop = 0; loop < FLAGS_loop_num; ++loop) {
            std::ostringstream name;
            name << "merged#" << loop;
            ReplayResult loop_result = ReplayRecords(name.str(), records);
            AddStats(&merged_total.stats, loop_result.stats);
            merged_total.abandoned_unsafe_channel = merged_total.abandoned_unsafe_channel ||
                                                    loop_result.abandoned_unsafe_channel;
            if (!loop_result.ok) {
                merged_total.ok = false;
                merged_total.code = loop_result.code;
                merged_total.error = loop_result.error;
                break;
            }
        }
        results.push_back(std::move(merged_total));
    } else {
        std::mutex result_mu;
        std::vector<std::thread> workers;
        for (const auto& file : recent_files) {
            workers.emplace_back([&file, replay_direction_filter, &result_mu, &results]() {
                std::vector<const RecentRecord*> records;
                records.reserve(file.records.size());
                for (const auto& record : file.records) {
                    if (ShouldReplayRecord(record, replay_direction_filter)) {
                        records.push_back(&record);
                    }
                }
                ReplayResult result;
                for (uint32_t loop = 0; loop < FLAGS_loop_num; ++loop) {
                    std::ostringstream name;
                    name << BaseName(file.path) << "#" << loop;
                    ReplayResult loop_result = ReplayRecords(name.str(), records);
                    AddStats(&result.stats, loop_result.stats);
                    result.abandoned_unsafe_channel = result.abandoned_unsafe_channel ||
                                                      loop_result.abandoned_unsafe_channel;
                    if (!loop_result.ok) {
                        result.ok = false;
                        result.code = loop_result.code;
                        result.error = loop_result.error;
                        break;
                    }
                }
                std::lock_guard<std::mutex> lock(result_mu);
                results.push_back(std::move(result));
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }
    double replay_seconds = std::chrono::duration_cast<std::chrono::duration<double>>(
                                std::chrono::steady_clock::now() - replay_start)
                                .count();

    bool ok = true;
    bool abandoned_unsafe_channel = false;
    ReplayStats total;
    int code = 0;
    for (const auto& result : results) {
        ok = ok && result.ok;
        abandoned_unsafe_channel = abandoned_unsafe_channel || result.abandoned_unsafe_channel;
        if (!result.ok && code == 0) {
            code = result.code;
            std::cerr << CurrentTimeString() << " " << result.error << std::endl;
        }
        AddStats(&total, result.stats);
    }

    if (abandoned_unsafe_channel) {
        return ok ? 1 : (code == 0 ? 1 : code);
    }

    if (!vesal::Uninit()) {
        return PrintAndReturn("vesal::Uninit failed", 7);
    }

    double input_gib_per_s = replay_seconds > 0
                                 ? total.bytes_in / 1024.0 / 1024.0 / 1024.0 / replay_seconds
                                 : 0;
    double output_gib_per_s = replay_seconds > 0
                                  ? total.bytes_out / 1024.0 / 1024.0 / 1024.0 / replay_seconds
                                  : 0;

    std::cout << CurrentTimeString() << " Replay summary, ok=" << ok
              << ", files=" << recent_files.size()
              << ", mode=" << FLAGS_replay_mode
              << ", replay_direction=" << FLAGS_replay_direction
              << ", loop_num=" << FLAGS_loop_num
              << ", submitted=" << total.submitted
              << ", completed=" << total.completed << ", compression=" << total.compression
              << ", decompression=" << total.decompression << ", bytes_in=" << total.bytes_in
              << ", bytes_out=" << total.bytes_out << ", replay_time=" << replay_seconds << " s"
              << ", input_throughput=" << input_gib_per_s << " GiB/s"
              << ", output_throughput=" << output_gib_per_s << " GiB/s" << std::endl;
    return ok ? 0 : (code == 0 ? 1 : code);
}
