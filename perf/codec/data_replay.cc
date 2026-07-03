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
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <gflags/gflags.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
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
DEFINE_uint32(inflight_num, 16, "Maximum async requests in flight per replay worker");
DEFINE_uint32(max_records, 0, "Maximum records to replay per worker. 0 means all records");
DEFINE_uint32(timeout_ms, 3000, "Timeout waiting for an async request completion");

constexpr size_t kRecentRecordHeaderSize = 44;

struct RecentRecord {
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
};

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

    auto set_error = [&](const std::string& error, int code) {
        replay_result.ok = false;
        replay_result.code = code;
        replay_result.error = error;
    };

    auto submit_next = [&]() -> bool {
        const RecentRecord& record = *records[next_index];
        if (record.payload_truncated != 0 || record.payload_len != record.src_total_len ||
            record.num_blocks != 1) {
            std::ostringstream oss;
            oss << name << " unsupported record req=" << record.req_id
                << ", payload_truncated=" << static_cast<int>(record.payload_truncated)
                << ", src_total_len=" << record.src_total_len
                << ", payload_len=" << record.payload_len << ", num_blocks=" << record.num_blocks;
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
        vesal::StatusCode status = channel->SubmitAsync(active->args);
        if (!vesal::IsOk(status)) {
            std::ostringstream oss;
            oss << name << " submit failed req=" << record.req_id << " type=" << RequestType(record)
                << ", status=" << vesal::StatusCodeToString(status);
            set_error(oss.str(), ErrorCodeForStatus(status));
            return false;
        }
        active->submit_time = std::chrono::steady_clock::now();
        active_requests.push_back(std::move(active));
        ++next_index;
        ++replay_result.stats.submitted;
        return true;
    };

    vesal::CodecResult results[64];
    while (replay_result.ok && (next_index < limit || !active_requests.empty())) {
        while (next_index < limit && active_requests.size() < FLAGS_inflight_num) {
            if (!submit_next()) {
                break;
            }
        }
        if (!replay_result.ok) {
            break;
        }

        ssize_t n = channel->Poll(results, 64, 0);
        if (n < 0) {
            set_error(name + " poll failed", 14);
            break;
        }
        if (n == 0) {
            if (!active_requests.empty() &&
                std::chrono::steady_clock::now() - active_requests.front()->submit_time > timeout) {
                std::ostringstream oss;
                oss << name << " request timeout req=" << active_requests.front()->record->req_id
                    << " type=" << RequestType(*active_requests.front()->record)
                    << ", in_flight=" << active_requests.size();
                set_error(oss.str(), ErrorCodeForStatus(vesal::StatusCode::kTimeout));
                break;
            }
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
                std::ostringstream oss;
                oss << name << " replay failed req=" << record.req_id << " type="
                    << RequestType(record) << ", status=" << vesal::StatusCodeToString(results[i].status);
                set_error(oss.str(), ErrorCodeForStatus(results[i].status));
                break;
            }

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

    vesal::Status close_status = channel->Close();
    if (!close_status.ok() && replay_result.ok) {
        replay_result.ok = false;
        replay_result.code = 5;
        replay_result.error = name + " channel close failed: " + close_status.ToString();
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(
        std::chrono::steady_clock::now() - start).count();
    double gib_per_s = elapsed > 0 ? replay_result.stats.bytes_in / 1024.0 / 1024.0 / 1024.0 / elapsed : 0;
    std::cout << "Replay worker done, name=" << name << ", ok=" << replay_result.ok
              << ", submitted=" << replay_result.stats.submitted
              << ", completed=" << replay_result.stats.completed
              << ", compression=" << replay_result.stats.compression
              << ", decompression=" << replay_result.stats.decompression
              << ", bytes_in=" << replay_result.stats.bytes_in
              << ", bytes_out=" << replay_result.stats.bytes_out << ", elapsed=" << elapsed
              << " s, throughput=" << gib_per_s << " GiB/s" << std::endl;
    return replay_result;
}

int PrintAndReturn(const std::string& msg, int code) {
    std::cerr << msg << std::endl;
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_input_path.empty()) {
        return PrintAndReturn("input_path must be set", 1);
    }
    if (FLAGS_inflight_num == 0 || FLAGS_inflight_num > 256) {
        return PrintAndReturn("inflight_num must be in range [1, 256]", 1);
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
            std::cout << "Loaded recent file, path=" << path << ", entries=" << file.records.size()
                      << ", trigger_req_id=" << file.trigger_req_id << ", pid=" << file.pid
                      << ", tid=" << file.tid << std::endl;
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
    init_opt.mem_pool_init_opt.prealloc_size_mb = 256;

    if (!vesal::Init(init_opt)) {
        return PrintAndReturn("vesal::Init failed", 1);
    }

    std::vector<ReplayResult> results;
    if (FLAGS_replay_mode == "merged") {
        std::vector<const RecentRecord*> records;
        for (const auto& file : recent_files) {
            for (const auto& record : file.records) {
                records.push_back(&record);
            }
        }
        std::sort(records.begin(), records.end(), [](const RecentRecord* lhs, const RecentRecord* rhs) {
            if (lhs->submit_ts_ns != rhs->submit_ts_ns) {
                return lhs->submit_ts_ns < rhs->submit_ts_ns;
            }
            return lhs->req_id < rhs->req_id;
        });
        results.push_back(ReplayRecords("merged", records));
    } else {
        std::mutex result_mu;
        std::vector<std::thread> workers;
        for (const auto& file : recent_files) {
            workers.emplace_back([&file, &result_mu, &results]() {
                std::vector<const RecentRecord*> records;
                records.reserve(file.records.size());
                for (const auto& record : file.records) {
                    records.push_back(&record);
                }
                ReplayResult result = ReplayRecords(BaseName(file.path), records);
                std::lock_guard<std::mutex> lock(result_mu);
                results.push_back(std::move(result));
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
    }

    bool ok = true;
    ReplayStats total;
    int code = 0;
    for (const auto& result : results) {
        ok = ok && result.ok;
        if (!result.ok && code == 0) {
            code = result.code;
            std::cerr << result.error << std::endl;
        }
        total.submitted += result.stats.submitted;
        total.completed += result.stats.completed;
        total.compression += result.stats.compression;
        total.decompression += result.stats.decompression;
        total.bytes_in += result.stats.bytes_in;
        total.bytes_out += result.stats.bytes_out;
    }

    if (!vesal::Uninit()) {
        return PrintAndReturn("vesal::Uninit failed", 7);
    }

    std::cout << "Replay summary, ok=" << ok << ", files=" << recent_files.size()
              << ", mode=" << FLAGS_replay_mode << ", submitted=" << total.submitted
              << ", completed=" << total.completed << ", compression=" << total.compression
              << ", decompression=" << total.decompression << ", bytes_in=" << total.bytes_in
              << ", bytes_out=" << total.bytes_out << std::endl;
    return ok ? 0 : (code == 0 ? 1 : code);
}
