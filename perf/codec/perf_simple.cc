/*
 * Copyright (c) 2023 ByteDance Inc.
 *
 * This file is part of veSAL.
 *
 * veSAL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * veSAL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with veSAL. If not, see <https://www.gnu.org/licenses/>.
 */

#include <gflags/gflags.h>
#include <linux/mman.h>  // MAP_HUGE_2MB
#include <numa.h>
#include <sys/mman.h>
#include <unistd.h>  // getpagesize()

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "simple_histogram.h"
#include "traffic_limiter.h"
#include "util.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

#ifdef VESAL_ENABLE_BYTE_METRICS
#include "byte/embedded_metrics/metrics.h"
#include "byte/embedded_metrics/metrics_registry.h"
#endif

DEFINE_uint64(total_data, 4, "how many data(GB) in total per thread, 4 by default");
DEFINE_uint32(inflight_num, 32, "");
DEFINE_uint64(source_size, 64 * 1024, "size of a single source data chunk");
DEFINE_uint32(sgl_size, 1, "size of scatter gather list");
DEFINE_double(
    dst_buffer_mul_times,
    2.0,
    "dst_buffer_size = dst_buffer_mul_times * source_size. Since the ASB is disabled in latest"
    "QAT, we need set dst_buffer_mul_times default 2 to hold the dst_buf, otherwise it might get"
    "overflow for small src_sizes like 1KB.");
DEFINE_uint32(thread_num, 1, "");
DEFINE_uint32(channel_num, 1, "number of channels per thread");
DEFINE_string(source_path,
              "",
              "source file path. If not specified, will try to read ./perf/calgary.");
DEFINE_bool(enable_crc32, false, "whether enable crc32 feature");
DEFINE_string(memory_type,
              "user",
              "user: user's own memory; "
              "allocate: allocated from pool; "
              "register_1gb: 1gb hugepage registered in pool; "
              "register_2mb: 2mb hugepage registered in pool");
DEFINE_string(metrics_prefix, "perf_simple", "Metrics prefix, by default: perf_simple");
DEFINE_bool(compression_perf, true, "whether to do compression perf");
DEFINE_bool(decompression_perf, true, "whether to do decompression perf");
DEFINE_int32(compression_level, 1, "compression level, valid from 1 to 12");
DEFINE_uint32(throughput_limit_mbs, 0, "The limit of throughput in mb/s, 0(disable) by default");
DEFINE_string(engine_type, "qat", "engine type, can be qat or sw or kae");
DEFINE_string(algorithm_type, "lz4", "algorithm type, can be lz4 or deflate or zlib");
DEFINE_int32(numa_node, -1, "NUMA node to use, -1 for auto or no NUMA");

uint64_t g_hugepage_size;
const uint64_t hugepage_total = 1024 * 1024 * 1024;

std::unique_ptr<unsigned char[]> g_source_data;

std::atomic<double> g_duration_total_us{0};

struct TotalData {
    std::mutex mutex;
    double consumed_bytes{0};
    double produced_bytes{0};

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex);
        consumed_bytes = 0;
        produced_bytes = 0;
    }

    void AddConsumedBytes(double bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        consumed_bytes += bytes;
    }

    void AddProducedBytes(double bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        produced_bytes += bytes;
    }
};

TotalData g_total_data_compress;
TotalData g_total_data_decompress;

std::mutex total_timer_mutex;
bool g_is_timer_started{false};
size_t g_chunk_size;
size_t g_chunks_num;
uint64_t g_iteration_num;
std::chrono::time_point<std::chrono::steady_clock> g_total_start_time;
std::chrono::time_point<std::chrono::steady_clock> g_total_end_time;
vesal::MemoryPool* memory_pool;
unsigned char* huge_page_address;

SimpleCSVWriter g_csv_writer;

enum class AlignType : uint8_t { kPage4kb = 1, kPage2Mb = 2, kPage1Gb = 3, kUnknown = 255 };

#define VESAL_PAGESIZE(page_bit_num) (1UL << (page_bit_num))
#define VESAL_PAGEMASK(page_size) (~((page_size) - 1))

uint64_t AlignTypeToPageSize(const AlignType& type) {
    switch (type) {
    case AlignType::kPage4kb:
        return VESAL_PAGESIZE(PAGE_BIT_NUM_4KB);
    case AlignType::kPage2Mb:
        return VESAL_PAGESIZE(PAGE_BIT_NUM_2MB);
    case AlignType::kPage1Gb:
        return VESAL_PAGESIZE(PAGE_BIT_NUM_1GB);
    default:
        VESAL_LOG(CRITICAL) << "Unknown align type";
    }
    return 0;
}

void ReadSourceData() {
    std::string path;
    if (FLAGS_source_path.empty()) {
        VESAL_LOG(WARN)
            << "FLAGS_source_path is empty, will try to read \"./perf/calgary\" by default";
        path = "./perf/calgary";
    } else {
        path = FLAGS_source_path;
    }
    std::ifstream ifs(path);
    if (!ifs.good()) {
        VESAL_LOG(ERROR) << "Failed to open file: " << path;
        exit(-1);
    }
    // Get the file size.
    ifs.seekg(0, std::ios::end);
    size_t file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    g_chunk_size = FLAGS_sgl_size * FLAGS_source_size;
    g_chunks_num = file_size / g_chunk_size;
    g_iteration_num = std::ceil((FLAGS_total_data << 30) / (1.0 * g_chunk_size * g_chunks_num));
    g_source_data = std::make_unique<unsigned char[]>(g_chunks_num * g_chunk_size);
    ifs.read(reinterpret_cast<char*>(g_source_data.get()), g_chunk_size * g_chunks_num);
    VESAL_LOG(INFO) << "file_size=" << file_size << ", g_chunks_num=" << g_chunks_num
                    << ", g_iteration_num=" << g_iteration_num;
}

struct Context {
    size_t index;
    std::chrono::time_point<std::chrono::steady_clock> submit_time;
    bool in_use = false;
};

// Keep the order.
const std::vector<const char*> kCSVHeader = {
    "comp/decomp",
    "memory_type",
    "channel_num",
    "inflight_num",
    "source_size",
    "compression_level",
    "engine_type",
    "algorithm_type",
    "sgl_size",
    "enable_crc32",
    "AVG_latency(us)",
    "P50_latency(us)",
    "P99_latency(us)",
    "P999_latency(us)",
    "throughput(GB/s)",
    "compress_ratio",
};
// Record the arguments and results of this time run.
// Storing both key and value as string.
struct Record {
    void Init() {
        records["inflight_num"] = std::to_string(FLAGS_inflight_num);
        records["memory_type"] = FLAGS_memory_type;
        records["channel_num"] = std::to_string(FLAGS_channel_num);
        records["source_size"] = std::to_string(FLAGS_source_size);
        records["compression_level"] = std::to_string(FLAGS_compression_level);
        records["engine_type"] = FLAGS_engine_type;
        records["algorithm_type"] = FLAGS_algorithm_type;
        records["sgl_size"] = std::to_string(FLAGS_sgl_size);
        records["enable_crc32"] = std::to_string(FLAGS_enable_crc32);
        // Add stats
        records["comp/decomp"] = "N/A";
        records["AVG_latency(us)"] = "N/A";
        records["P50_latency(us)"] = "N/A";
        records["P99_latency(us)"] = "N/A";
        records["P999_latency(us)"] = "N/A";
        records["throughput(GB/s)"] = "N/A";
        records["compress_ratio"] = "N/A";
        VESAL_CHECK(records.size() == kCSVHeader.size());
    }

    std::vector<std::string> GetDataRow() const {
        std::vector<std::string> row;
        for (const auto& key : kCSVHeader) {
            row.push_back(records.at(key));
        }
        return row;
    }

    std::string ToSting() const {
        std::stringstream ss;
        for (const auto& key : kCSVHeader) {
            ss << key << ":" << records.at(key) << ", ";
        }
        return ss.str();
    }

    void AddStat(const std::string& key, const std::string& value) {
        VESAL_CHECK(records.find(key) != records.end() && records[key] == "N/A")
            << "Not allow add for " << key;
        records[key] = value;
    }

    std::unordered_map<std::string, std::string> records;
};

#define VESAL_PERF_DELETE_2D_ARRAY(two_d_arr, num) \
    do {                                           \
        for (size_t i = 0; i < num; ++i) {         \
            DeleteUcharArray(two_d_arr[i]);        \
        }                                          \
        delete[] two_d_arr;                        \
    } while (0)

class CodecCompressReq {
public:
    CodecCompressReq(SimpleHistogram* histogram) : histogram_(histogram) {
        PreInit();
    }
    ~CodecCompressReq() {
        delete[] compressed_data_;
        delete[] decompressed_data_;
        delete[] compressed_data_lengths_;
        delete[] decompressed_data_lengths_;
        delete[] original_data_checksum_;
        delete[] produced_data_checksum_;
    }

    void InitBuffers() {
        AllocateAndInitBuffers();
    }

    void Warmup() {
        total_num_ = g_chunks_num;
        CreateChannel();
        RunCompress();
        RunDecompress();
        Validate();
        Close();
        total_num_ = g_chunks_num * g_iteration_num;
    }

    void ReleaseBuffers() {
        for (const std::vector<unsigned char*>& each_group : src_data_) {
            for (unsigned char* each_buf : each_group) {
                DeleteUcharArray(each_buf);
            }
        }
        for (size_t i = 0; i < g_chunks_num; ++i) {
            DeleteUcharArray(compressed_data_[i]);
            compressed_data_[i] = nullptr;
        }
        for (size_t i = 0; i < g_chunks_num; ++i) {
            DeleteUcharArray(decompressed_data_[i]);
            decompressed_data_[i] = nullptr;
        }
    }

    void SetNuma(int32_t numa) {
        this->numa = numa;
    }

    const int32_t GetNuma() const {
        return numa;
    }

    void CreateChannel() {
        // channel must be created within the same thread as Run().
        // since channel has thread_local mem pool if mem pool enabled.
        vesal::CodecChannelOption channel_opts;
        if (numa >= 0) {
            channel_opts.allocation_option.node_affinity = numa;
        }
        if (FLAGS_engine_type == "qat") {
            channel_opts.engine_type = vesal::CodecEngineType::kQat;
        } else if (FLAGS_engine_type == "sw") {
            channel_opts.engine_type = vesal::CodecEngineType::kSoftware;
        } else if (FLAGS_engine_type == "kae") {
            channel_opts.engine_type = vesal::CodecEngineType::kKae;
        } else {
            VESAL_LOG(CRITICAL) << "Unsupported engine type: " << FLAGS_engine_type;
        }
        if (FLAGS_algorithm_type == "lz4") {
            channel_opts.comp_algorithm = vesal::CodecAlgorithm::kLz4;
        } else if (FLAGS_algorithm_type == "deflate") {
            channel_opts.comp_algorithm = vesal::CodecAlgorithm::kDeflate;
        } else if (FLAGS_algorithm_type == "zlib") {
            channel_opts.comp_algorithm = vesal::CodecAlgorithm::kZlib;
        } else {
            VESAL_LOG(CRITICAL) << "Unsupported algorithm type: " << FLAGS_algorithm_type;
        }
        if (FLAGS_enable_crc32)
            channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;
        VESAL_CHECK(FLAGS_compression_level >= 1 && FLAGS_compression_level <= 12)
            << "Invalid compression level: " << FLAGS_compression_level << ". Valid range: [1, 12]";
        channel_opts.comp_level = static_cast<vesal::CodecCompLevel>(FLAGS_compression_level);
        for (uint32_t i = 0; i < FLAGS_channel_num; i++) {
            auto channel_r = vesal::CodecChannel::CreateCodecChannel(channel_opts);
            VESAL_CHECK(channel_r.first.ok()) << channel_r.first;
            channels_.push_back(std::move(channel_r.second));
        }
    }

    void RunCompress() {
        vesal::CodecResult results[inflight_num_];
        uint64_t sent_num = 0;  // how many requests sent now
        uint64_t res_num = 0;   // how many response received now
        uint32_t batch_num[FLAGS_channel_num];
        uint32_t batch_index[FLAGS_channel_num];
        for (uint32_t i = 0; i < FLAGS_channel_num; i++) {
            batch_num[i] = inflight_num_;
            batch_index[i] = 0;
        }
        TrafficLimiter tl;
        tl.Start(FLAGS_throughput_limit_mbs);

        while (sent_num < total_num_ || res_num < sent_num) {
            for (size_t channel_i = 0; channel_i < FLAGS_channel_num; ++channel_i) {
                tl.WaitUntil(std::min(uint64_t(batch_num[channel_i]), total_num_ - sent_num) *
                             FLAGS_source_size * FLAGS_sgl_size);
                auto submit_time = std::chrono::steady_clock::now();
                while (batch_num[channel_i] > 0 && sent_num < total_num_) {
                    int index = sent_num % g_chunks_num;
                    Context* ctx = &contexts_[channel_i][batch_index[channel_i]];
                    VESAL_DCHECK(!ctx->in_use)
                        << "sent_num: " << sent_num << ", ctx->index: " << index;
                    ctx->index = index;
                    ctx->submit_time = submit_time;
                    ctx->in_use = true;
                    vesal::StatusCode comp_async_r =
                        channels_[channel_i]->CompressSGLAsync(src_data_[ctx->index],
                                                               source_data_len_,
                                                               compressed_data_[ctx->index],
                                                               dst_data_len_,
                                                               ctx);
                    VESAL_CHECK(IsOk(comp_async_r));
                    --batch_num[channel_i];
                    batch_index[channel_i] = (batch_index[channel_i] + 1) % inflight_num_;
                    ++sent_num;
                }
            }
            for (size_t channel_i = 0; channel_i < FLAGS_channel_num; ++channel_i) {
                auto& channel = channels_[channel_i];
                auto poll_r = channel->Poll(results, FLAGS_inflight_num, -1);
                VESAL_DCHECK(poll_r != -1);

                batch_num[channel_i] += poll_r;
                res_num += poll_r;
                auto complete_time = std::chrono::steady_clock::now();
                for (uint32_t i = 0; i < poll_r; i++) {
                    VESAL_CHECK(results[i].status == vesal::StatusCode::kOk) << results[i].status;
                    Context* ctx = (Context*)results[i].ctx;
                    uint32_t idx = ctx->index;
                    auto submit_time = ctx->submit_time;
                    ctx->in_use = false;
                    histogram_->Add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        complete_time - submit_time)
                                        .count() /
                                    1000.0);

                    // the results should be consistent every batch
                    VESAL_CHECK(original_data_checksum_[idx] == 0 ||
                                original_data_checksum_[idx] == results[i].in_checksum)
                        << "original_data_checksum_[idx]: " << original_data_checksum_[idx]
                        << ", results[i].in_checksum: " << results[i].in_checksum;
                    compressed_data_lengths_[idx] = results[i].produced;
                    original_data_checksum_[idx] = results[i].in_checksum;
                }
            }
        }
        VESAL_CHECK(res_num == sent_num);
        VESAL_CHECK(total_num_ == sent_num);
    }

    void RunDecompress() {
        vesal::CodecResult results[inflight_num_];
        uint64_t sent_num = 0;  // how many requests sent now
        uint64_t res_num = 0;   // how many response received now
        uint32_t batch_num[FLAGS_channel_num];
        uint32_t batch_index[FLAGS_channel_num];
        for (uint32_t i = 0; i < FLAGS_channel_num; i++) {
            batch_num[i] = inflight_num_;
            batch_index[i] = 0;
        }
        TrafficLimiter tl;
        tl.Start(FLAGS_throughput_limit_mbs);

        while (sent_num < total_num_ || res_num < sent_num) {
            for (size_t channel_i = 0; channel_i < FLAGS_channel_num; ++channel_i) {
                tl.WaitUntil(std::min((uint64_t)batch_num[channel_i], total_num_ - sent_num) *
                             FLAGS_source_size * FLAGS_sgl_size);
                auto submit_time = std::chrono::steady_clock::now();
                while (batch_num[channel_i] > 0 && sent_num < total_num_) {
                    int index = sent_num % g_chunks_num;
                    Context* ctx = &contexts_[channel_i][batch_index[channel_i]];
                    VESAL_DCHECK(!ctx->in_use)
                        << "sent_num: " << sent_num << ", ctx->index: " << index;
                    ctx->index = index;
                    ctx->submit_time = submit_time;
                    ctx->in_use = true;
                    vesal::StatusCode decomp_async_r =
                        channels_[channel_i]->DecompressAsync(compressed_data_[ctx->index],
                                                              compressed_data_lengths_[ctx->index],
                                                              decompressed_data_[ctx->index],
                                                              g_chunk_size,
                                                              ctx);
                    VESAL_CHECK(IsOk(decomp_async_r));
                    --batch_num[channel_i];
                    batch_index[channel_i] = (batch_index[channel_i] + 1) % inflight_num_;
                    ++sent_num;
                }
            }

            for (size_t channel_i = 0; channel_i < FLAGS_channel_num; ++channel_i) {
                auto& channel = channels_[channel_i];
                auto poll_r = channel->Poll(results, FLAGS_inflight_num, -1);
                VESAL_DCHECK(poll_r != -1);

                batch_num[channel_i] += poll_r;
                res_num += poll_r;
                auto complete_time = std::chrono::steady_clock::now();
                for (uint32_t i = 0; i < poll_r; i++) {
                    VESAL_CHECK(results[i].status == vesal::StatusCode::kOk) << results[i].status;
                    Context* ctx = (Context*)results[i].ctx;
                    uint32_t idx = ctx->index;
                    auto submit_time = ctx->submit_time;
                    ctx->in_use = false;
                    histogram_->Add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        complete_time - submit_time)
                                        .count() /
                                    1000.0);

                    // the results should be consistent every batch
                    VESAL_CHECK(decompressed_data_lengths_[idx] == 0 ||
                                decompressed_data_lengths_[idx] == results[i].produced);
                    VESAL_CHECK(produced_data_checksum_[idx] == 0 ||
                                produced_data_checksum_[idx] == results[i].out_checksum);
                    decompressed_data_lengths_[idx] = results[i].produced;
                    produced_data_checksum_[idx] = results[i].out_checksum;
                }
            }
        }
        VESAL_CHECK(res_num == sent_num);
        VESAL_CHECK(total_num_ == sent_num);
    }

    double GetCompressedResultsBytes() {
        double ret = 0;
        for (size_t i = 0; i < g_chunks_num; ++i) {
            ret += compressed_data_lengths_[i];
            VESAL_CHECK(compressed_data_lengths_[i] > 0);
        }
        return ret;
    }

    void Validate() {
        for (uint32_t i = 0; i < g_chunks_num; i++) {
            VESAL_CHECK(original_data_checksum_[i] == produced_data_checksum_[i]);
            for (uint32_t j = 0; j < sgl_size_; j++)
                for (uint32_t k = 0; k < FLAGS_source_size; k++)
                    VESAL_CHECK(decompressed_data_[i][j * FLAGS_source_size + k] ==
                                src_data_[i][j][k]);
        }
    }

    void Close() {
        for (auto& channel : channels_) {
            vesal::Status close_r = channel->Close();
            VESAL_CHECK(close_r.ok());
        }
        channels_.clear();
    }

private:
    std::vector<std::vector<unsigned char*>> src_data_;
    unsigned char** compressed_data_;
    unsigned char** decompressed_data_;
    uint32_t* compressed_data_lengths_;
    uint32_t* decompressed_data_lengths_;
    // the compress api's result contains the original data's checksum
    uint32_t* original_data_checksum_;
    // the decompress api's result contains the produced data's checksum
    uint32_t* produced_data_checksum_;
    std::vector<std::unique_ptr<vesal::CodecChannel>> channels_;
    std::vector<uint32_t> source_data_len_;  // lens for every source SGL
    uint64_t dst_data_len_;
    uint32_t inflight_num_;
    uint64_t total_num_;  // how many requests are gonna send at beginning
    uint32_t sgl_size_;
    SimpleHistogram* histogram_;
    std::vector<std::vector<Context>> contexts_;
    int32_t numa = -1;

    unsigned char* NewUcharArray(uint64_t len) {
        if (FLAGS_memory_type == "user") {
            return new unsigned char[len];
        } else if (FLAGS_memory_type == "allocate") {
            return reinterpret_cast<unsigned char*>(memory_pool->Allocate(len));
        } else if (FLAGS_memory_type == "register_1gb" || FLAGS_memory_type == "register_2mb") {
            static std::mutex offset_mutex;
            static uint64_t offset = 0;
            std::lock_guard<std::mutex> lock(offset_mutex);

            VESAL_CHECK(len <= g_hugepage_size)
                << "memory requested is bigger than a hugepage size, " << len
                << ", g_hugepage_size=" << g_hugepage_size;
            // avoid using memory blocks that span hugepages
            uint64_t diff = g_hugepage_size - offset % g_hugepage_size;
            if (diff < len)
                offset += diff;
            offset += len;
            VESAL_CHECK(offset <= hugepage_total)
                << "memory requested is bigger than hugepages total size";
            return (huge_page_address + offset - len);
        } else
            VESAL_CHECK(0) << "wrong memory_type";
        return nullptr;
    }

    void DeleteUcharArray(unsigned char* arr) {
        if (FLAGS_memory_type == "user") {
            delete[] arr;
        } else if (FLAGS_memory_type == "allocate") {
            memory_pool->Deallocate(arr);
        } else if (FLAGS_memory_type == "register_1gb" || FLAGS_memory_type == "register_2mb") {
        } else
            VESAL_CHECK(0) << "wrong memory_type";
    }

    // PreInit: Pre-initialization function
    // - Initialize basic parameters (sgl_size, dst_data_len, etc.)
    // - Initialize contexts and source data lengths
    // - Initialize data buffer containers (only allocate container structure, not actual memory)
    // - Actual memory will be allocated in InitBuffers(), which is called within task threads to
    // ensure NUMA affinity
    void PreInit() {
        // Initialize basic parameters
        sgl_size_ = FLAGS_sgl_size;
        dst_data_len_ = FLAGS_dst_buffer_mul_times * g_chunk_size;
        inflight_num_ = FLAGS_inflight_num;
        total_num_ = g_chunks_num * g_iteration_num;

        // Initialize contexts and source data lengths
        source_data_len_ = std::vector<uint32_t>(sgl_size_, FLAGS_source_size);
        for (size_t i = 0; i < FLAGS_channel_num; ++i)
            contexts_.push_back(std::vector<Context>(inflight_num_));

        // Initialize data buffer containers (only allocate container structure, not actual memory)
        // Actual memory will be allocated in InitBuffers(), which is called within task threads to
        // ensure NUMA affinity
        src_data_ = std::vector<std::vector<unsigned char*>>(g_chunks_num);
        for (size_t i = 0; i < g_chunks_num; i++) {
            src_data_[i] = std::vector<unsigned char*>(sgl_size_, nullptr);
        }

        compressed_data_ = new unsigned char*[g_chunks_num];
        memset(compressed_data_, 0, g_chunks_num * sizeof(unsigned char*));
        decompressed_data_ = new unsigned char*[g_chunks_num];
        memset(decompressed_data_, 0, g_chunks_num * sizeof(unsigned char*));
        compressed_data_lengths_ = new uint32_t[g_chunks_num];
        memset(compressed_data_lengths_, 0, g_chunks_num * sizeof(uint32_t));
        original_data_checksum_ = new uint32_t[g_chunks_num];
        memset(original_data_checksum_, 0, g_chunks_num * sizeof(uint32_t));
        decompressed_data_lengths_ = new uint32_t[g_chunks_num];
        memset(decompressed_data_lengths_, 0, g_chunks_num * sizeof(uint32_t));
        produced_data_checksum_ = new uint32_t[g_chunks_num];
        memset(produced_data_checksum_, 0, g_chunks_num * sizeof(uint32_t));
    }

    void AllocateAndInitBuffers() {
        for (size_t i = 0; i < g_chunks_num; i++) {
            for (size_t j = 0; j < sgl_size_; j++) {
                unsigned char* buf = NewUcharArray(FLAGS_source_size);
                memcpy(buf,
                       g_source_data.get() + i * g_chunk_size + j * FLAGS_source_size,
                       FLAGS_source_size);
                src_data_[i][j] = buf;
            }
        }

        for (size_t i = 0; i < g_chunks_num; ++i) {
            compressed_data_[i] = NewUcharArray(dst_data_len_);
            memset(compressed_data_[i], '\0', dst_data_len_);
        }

        for (size_t i = 0; i < g_chunks_num; ++i) {
            decompressed_data_[i] = NewUcharArray(g_chunk_size);
            memset(decompressed_data_[i], '\0', g_chunk_size);
        }
    }
};

void BindToNuma(int numa) {
    if (numa == -1) {
        return;
    }
    bitmask* cpus = numa_allocate_cpumask();
    if (cpus == nullptr) {
        VESAL_CHECK(0) << "numa_allocate_cpumask failed";
    }
    if (numa_node_to_cpus(numa, cpus) != 0) {
        VESAL_CHECK(0) << "numa_node_to_cpus failed";
    }
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);

    for (size_t i = 0; i < cpus->size; i++) {
        if (numa_bitmask_isbitset(cpus, i)) {
            CPU_SET(i, &cpu_set);
        }
    }

    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set) != 0) {
        VESAL_CHECK(0) << "pthread_setaffinity_np failed";
    }
    numa_free_cpumask(cpus);
}

void CompressTask(CodecCompressReq* req) {
    BindToNuma(req->GetNuma());
    int cpu = sched_getcpu();
    VESAL_LOG(INFO) << "CompressTask numa=" << req->GetNuma()
                    << ", curr_numa=" << numa_node_of_cpu(cpu);

    req->InitBuffers();
    req->Warmup();
    req->CreateChannel();

    {
        std::lock_guard<std::mutex> lg(total_timer_mutex);
        if (!g_is_timer_started) {
            g_total_start_time = std::chrono::steady_clock::now();
            g_total_data_compress.Reset();
            g_is_timer_started = true;
        }
    }

    req->RunCompress();
    double consumed_total_bytes = g_chunk_size * g_chunks_num * g_iteration_num;
    g_total_data_compress.AddConsumedBytes(consumed_total_bytes);
    double produced_total_bytes = req->GetCompressedResultsBytes() * g_iteration_num;
    g_total_data_compress.AddProducedBytes(produced_total_bytes);

    {
        std::lock_guard<std::mutex> lg(total_timer_mutex);
        auto now = std::chrono::steady_clock::now();
        if (now > g_total_end_time) {
            g_total_end_time = now;
        }
    }

    req->Validate();
    req->Close();
    req->ReleaseBuffers();
}

void DecompressTask(CodecCompressReq* req) {
    BindToNuma(req->GetNuma());

    req->InitBuffers();
    req->Warmup();
    req->CreateChannel();

    {
        std::lock_guard<std::mutex> lg(total_timer_mutex);
        if (!g_is_timer_started) {
            g_total_start_time = std::chrono::steady_clock::now();
            g_total_data_decompress.Reset();
            g_is_timer_started = true;
        }
    }

    req->RunDecompress();
    double consumed_total_bytes = req->GetCompressedResultsBytes() * g_iteration_num;
    g_total_data_decompress.AddConsumedBytes(consumed_total_bytes);
    double produced_total_bytes = g_chunk_size * g_chunks_num * g_iteration_num;
    g_total_data_decompress.AddProducedBytes(produced_total_bytes);

    {
        std::lock_guard<std::mutex> lg(total_timer_mutex);
        auto now = std::chrono::steady_clock::now();
        if (now > g_total_end_time) {
            g_total_end_time = now;
        }
    }

    req->Validate();
    req->Close();
    req->ReleaseBuffers();
}

void EchoResult(const TotalData& total_data, const SimpleHistogram& his, bool is_compress) {
    auto record = std::make_unique<Record>();
    record->Init();
    record->AddStat("AVG_latency(us)", std::to_string(his.GetAvg()));
    record->AddStat("P50_latency(us)", std::to_string(his.GetPercentage(50)));
    record->AddStat("P99_latency(us)", std::to_string(his.GetPercentage(99)));
    record->AddStat("P999_latency(us)", std::to_string(his.GetPercentage(99.9)));
    record->AddStat("comp/decomp", is_compress ? "comp" : "decomp");
    double throughput_gb = 0;
    if (is_compress) {
        throughput_gb =
            total_data.consumed_bytes / 1024 / 1024 / 1024 / g_duration_total_us * 1000 * 1000;
        record->AddStat("compress_ratio",
                        std::to_string(total_data.produced_bytes / total_data.consumed_bytes));
    } else {
        throughput_gb =
            total_data.produced_bytes / 1024 / 1024 / 1024 / g_duration_total_us * 1000 * 1000;
    }
    record->AddStat("throughput(GB/s)", std::to_string(throughput_gb));

    g_csv_writer.WriteRow(record->GetDataRow());

    VESAL_LOG(INFO) << record->ToSting();
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_vesal_log_console_output = true;
    vesal::InitOptions opt;
    opt.mem_pool_init_opt.init_mem_pool = FLAGS_engine_type == "sw" ? false : true;
    opt.codec_init_opt.init_qat = false;
    opt.cypher_init_opt.init_qat = false;
    opt.data_flow_init_opt.init_dsa = false;
    if (FLAGS_engine_type == "qat") {
        opt.codec_init_opt.init_qat = true;
    } else if (FLAGS_engine_type == "kae") {
        opt.codec_init_opt.init_kae = true;
    }
    if (FLAGS_numa_node != -1) {
        opt.mem_pool_init_opt.prealloc_numa_mb[FLAGS_numa_node] =
            opt.mem_pool_init_opt.prealloc_size_mb;
    }
#ifdef VESAL_ENABLE_BYTE_METRICS
    namespace bm = byte::embedded_metrics;
    auto& registry = bm::MetricsRegistry::Registry();
    registry.Initialize(FLAGS_metrics_prefix, {}, bm::MetricsOptions{}, nullptr);
    auto r = std::make_shared<vesal::ByteMetricRegistry<bm::MetricsRegistry,
                                                        bm::Guage,
                                                        bm::TlsCounter,
                                                        bm::TlsHistogram,
                                                        bm::MetricType>>(&registry);
    opt.registry = r;
#endif
    vesal::Init(opt);
    memory_pool = vesal::MemoryPool::GetInstance();
    EchoAllFlags();
    if (FLAGS_memory_type == "register_1gb" || FLAGS_memory_type == "register_2mb") {
        g_hugepage_size = AlignTypeToPageSize(
            FLAGS_memory_type == "register_1gb" ? AlignType::kPage1Gb : AlignType::kPage2Mb);
        VESAL_CHECK(hugepage_total % g_hugepage_size == 0)
            << "hugepage_total should be divisible by g_hugepage_size";
        vesal::MemoryInfo mem_info = {
            .virtual_addr = AllocHugepageFn(g_hugepage_size, hugepage_total / g_hugepage_size),
            .physical_addr = nullptr,
            .len = hugepage_total,
            .page_size = g_hugepage_size};
        VESAL_CHECK(memory_pool->Register({mem_info})) << "memory pool register failure";
        huge_page_address = reinterpret_cast<unsigned char*>(mem_info.virtual_addr);
    }

    g_csv_writer.OpenFile("perf_simple_results.csv");
    g_csv_writer.WriteRow(kCSVHeader);

    {
        std::thread ths[FLAGS_thread_num];
        SimpleHistogram histogram[FLAGS_thread_num];
        std::unique_ptr<CodecCompressReq> reqs[FLAGS_thread_num];

        std::srand(std::time(nullptr));
        ReadSourceData();
        for (size_t i = 0; i < FLAGS_thread_num; ++i) {
            reqs[i] = std::make_unique<CodecCompressReq>(histogram + i);
            if (FLAGS_numa_node != -1) {
                reqs[i]->SetNuma(FLAGS_numa_node);
            }
        }

        if (FLAGS_compression_perf) {
            g_is_timer_started = false;
            g_total_end_time = std::chrono::steady_clock::time_point();
            for (size_t i = 0; i < FLAGS_thread_num; ++i) {
                ths[i] = std::thread(CompressTask, reqs[i].get());
            }
            for (size_t i = 0; i < FLAGS_thread_num; ++i) {
                ths[i].join();
            }
            g_duration_total_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                      g_total_end_time - g_total_start_time)
                                      .count();
            for (size_t i = 1; i < FLAGS_thread_num; ++i) {
                histogram[0].Combine(histogram[i]);
                histogram[i].Reset();
            }
            // EchoHistogram(histogram[0]);
            EchoResult(g_total_data_compress, histogram[0], true);
            histogram[0].Reset();
        }

        if (FLAGS_decompression_perf) {
            g_is_timer_started = false;
            g_total_end_time = std::chrono::steady_clock::time_point();
            for (size_t i = 0; i < FLAGS_thread_num; ++i) {
                ths[i] = std::thread(DecompressTask, reqs[i].get());
            }
            for (size_t i = 0; i < FLAGS_thread_num; ++i) {
                ths[i].join();
            }
            g_duration_total_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                      g_total_end_time - g_total_start_time)
                                      .count();

            for (size_t i = 1; i < FLAGS_thread_num; ++i) {
                histogram[0].Combine(histogram[i]);
            }
            EchoResult(g_total_data_decompress, histogram[0], false);
        }

        auto status = vesal::Uninit();
        VESAL_CHECK(status);
    }
    g_csv_writer.CloseFile();
    if (FLAGS_memory_type == "register_1gb" || FLAGS_memory_type == "register_2mb") {
        DeallocHugepageFn(huge_page_address, hugepage_total);
    }
}
