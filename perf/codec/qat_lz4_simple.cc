/*
 * Copyright (c) 2026 ByteDance Inc.
 *
 * This file is part of veSAL.
 */

#include <cstring>
#include <gflags/gflags.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "vesal/codec.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

namespace {

DEFINE_uint32(thread_num, 1, "Number of worker threads");
DEFINE_uint32(loop_num, 100, "Compression/decompression loops per thread");
DEFINE_uint32(input_size, 8 * 1024, "Input bytes for each compression request");
DEFINE_uint32(inflight_num, 16, "Maximum async requests in flight per thread");
DEFINE_int32(compression_level, 9, "LZ4 compression level [1, 12]");
DEFINE_double(compress_ratio,
              1.0,
              "Random data ratio in each input buffer. 0.0 means all zero, 1.0 means all random");
DEFINE_string(data_fill_mode,
              "4kb",
              "Input data fill mode: 4kb fills each 4KB chunk, block fills the whole input block");

struct AsyncWindow;

struct AsyncRequest {
    AsyncWindow* window{nullptr};
    uint32_t loop_index{0};
    uint32_t slot_index{0};
    bool is_decompress{false};
    std::chrono::steady_clock::time_point submit_time;
    vesal::CodecResult result;
};

struct AsyncWindow {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<AsyncRequest*> completed;
};

constexpr auto kAsyncRequestTimeout = std::chrono::seconds(3);

size_t MaxCompressedCapacity(size_t input_size) {
    // Keep a wide headroom for incompressible random input across QAT/LZ4 variants.
    return input_size * 2;
}

void FillInputByCompressRatio(std::vector<unsigned char>* buf, std::mt19937_64* rng) {
    const size_t kBlockSize = 4 * 1024;
    buf->assign(buf->size(), 0);
    std::uniform_int_distribution<int> dist(0, 255);

    if (FLAGS_data_fill_mode == "block") {
        size_t rand_len = static_cast<size_t>(buf->size() * FLAGS_compress_ratio);
        for (size_t i = 0; i < rand_len; ++i) {
            (*buf)[i] = static_cast<unsigned char>(dist(*rng));
        }
        return;
    }

    size_t rand_len_per_block = static_cast<size_t>(kBlockSize * FLAGS_compress_ratio);
    for (size_t block_start = 0; block_start < buf->size(); block_start += kBlockSize) {
        for (size_t i = 0; i < rand_len_per_block; ++i) {
            (*buf)[block_start + i] = static_cast<unsigned char>(dist(*rng));
        }
    }
}

int PrintAndReturn(const std::string& msg, int code) {
    std::cerr << msg << std::endl;
    return code;
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

void CodecAsyncCallback(const vesal::CodecResult& result) {
    auto* req = static_cast<AsyncRequest*>(result.ctx);
    {
        std::lock_guard<std::mutex> lock(req->window->mu);
        req->result = result;
        req->window->completed.push_back(req);
    }
    req->window->cv.notify_one();
}

AsyncRequest* WaitCompletedRequestFor(AsyncWindow* window,
                                      std::chrono::steady_clock::duration timeout) {
    std::unique_lock<std::mutex> lock(window->mu);
    if (!window->cv.wait_for(lock, timeout, [window]() { return !window->completed.empty(); })) {
        return nullptr;
    }
    AsyncRequest* req = window->completed.front();
    window->completed.pop_front();
    return req;
}

const char* RequestType(const AsyncRequest* req) {
    return req->is_decompress ? "decompress" : "compress";
}

}  // namespace

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_thread_num == 0) {
        return PrintAndReturn("thread_num must be > 0", 10);
    }
    if (FLAGS_thread_num > 8) {
        return PrintAndReturn("thread_num must be <= 8 for stable testing", 15);
    }
    if (FLAGS_loop_num == 0) {
        return PrintAndReturn("loop_num must be > 0", 11);
    }
    if (FLAGS_inflight_num == 0 || FLAGS_inflight_num > 256) {
        return PrintAndReturn("inflight_num must be in range [1, 256]", 16);
    }
    if (FLAGS_input_size == 0 || FLAGS_input_size > 512 * 1024 ||
        FLAGS_input_size % (4 * 1024) != 0) {
        return PrintAndReturn("input_size must be in range (0, 512KB] and 4KB-aligned", 12);
    }
    if (FLAGS_compression_level < 1 || FLAGS_compression_level > 12) {
        return PrintAndReturn("compression_level must be in range [1, 12]", 13);
    }
    if (FLAGS_compress_ratio < 0.0 || FLAGS_compress_ratio > 1.0) {
        return PrintAndReturn("compress_ratio must be in range [0.0, 1.0]", 17);
    }
    if (FLAGS_data_fill_mode != "4kb" && FLAGS_data_fill_mode != "block") {
        return PrintAndReturn("data_fill_mode must be one of: 4kb, block", 18);
    }

    vesal::InitOptions init_opt;
    init_opt.codec_init_opt.init_qat = true;
    init_opt.cypher_init_opt.init_qat = false;
    init_opt.data_flow_init_opt.init_dsa = false;
    init_opt.mem_pool_init_opt.init_mem_pool = true;
    // Keep hugepage demand small for this demo.
    init_opt.mem_pool_init_opt.prealloc_page_size = vesal::HugePageSize::k2MB;
    init_opt.mem_pool_init_opt.prealloc_size_mb = 256;

    if (!vesal::Init(init_opt)) {
        return PrintAndReturn("vesal::Init failed", 1);
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> compressed_input_bytes{0};
    std::atomic<uint64_t> decompressed_output_bytes{0};
    std::mutex err_mu;
    std::string first_error;
    int first_error_code = 14;  // 默认错误码
    std::vector<std::thread> workers;
    workers.reserve(FLAGS_thread_num);

    auto start = std::chrono::steady_clock::now();
    for (uint32_t tid = 0; tid < FLAGS_thread_num; ++tid) {
        workers.emplace_back([tid,
                              &stop,
                              &compressed_input_bytes,
                              &decompressed_output_bytes,
                              &err_mu,
                              &first_error,
                              &first_error_code]() {
            vesal::CodecChannelOption channel_opt;
            channel_opt.mode = vesal::ChannelMode::kShared;
            channel_opt.user_cb = CodecAsyncCallback;
            channel_opt.engine_type = vesal::CodecEngineType::kQat;
            channel_opt.comp_algorithm = vesal::CodecAlgorithm::kLz4;
            channel_opt.comp_level = static_cast<vesal::CodecCompLevel>(FLAGS_compression_level);
            channel_opt.checksum_type = vesal::CodecChecksumType::kCrc32;

            auto channel_pair = vesal::CodecChannel::CreateCodecChannel(channel_opt);
            if (!channel_pair.first.ok()) {
                std::lock_guard<std::mutex> lock(err_mu);
                if (first_error.empty()) {
                    std::ostringstream oss;
                    oss << "Thread " << tid
                        << " CreateCodecChannel failed: " << channel_pair.first.ToString();
                    first_error = oss.str();
                }
                stop.store(true, std::memory_order_relaxed);
                return;
            }

            std::unique_ptr<vesal::CodecChannel> channel = std::move(channel_pair.second);
            std::vector<std::vector<unsigned char>> src(FLAGS_inflight_num,
                                                        std::vector<unsigned char>(FLAGS_input_size));
            std::vector<std::vector<unsigned char>> compressed(
                FLAGS_inflight_num,
                std::vector<unsigned char>(MaxCompressedCapacity(FLAGS_input_size)));
            std::vector<std::vector<unsigned char>> decompressed(
                FLAGS_inflight_num,
                std::vector<unsigned char>(FLAGS_input_size));
            std::vector<AsyncRequest> comp_reqs(FLAGS_inflight_num);
            std::vector<AsyncRequest> decomp_reqs(FLAGS_inflight_num);
            std::mt19937_64 rng(static_cast<uint64_t>(std::random_device{}()) ^
                                (static_cast<uint64_t>(tid) << 32));

            auto set_error = [&](const std::string& error, int code) {
                std::lock_guard<std::mutex> lock(err_mu);
                if (first_error.empty()) {
                    first_error = error;
                    first_error_code = code;
                }
                stop.store(true, std::memory_order_relaxed);
            };

            AsyncWindow window;
            std::deque<AsyncRequest*> pending;
            uint32_t next_loop = 0;
            uint32_t completed_loops = 0;
            uint32_t in_flight = 0;

            auto remove_pending = [&](AsyncRequest* req) {
                auto it = std::find(pending.begin(), pending.end(), req);
                if (it != pending.end()) {
                    pending.erase(it);
                }
            };

            auto wait_next_completed = [&]() {
                if (pending.empty()) {
                    return WaitCompletedRequestFor(&window, kAsyncRequestTimeout);
                }

                auto now = std::chrono::steady_clock::now();
                auto deadline = pending.front()->submit_time + kAsyncRequestTimeout;
                auto wait_time = deadline > now ? deadline - now
                                                : std::chrono::steady_clock::duration::zero();
                return WaitCompletedRequestFor(&window, wait_time);
            };

            auto set_timeout_error = [&]() {
                std::ostringstream oss;
                oss << "Thread " << tid << " async request timeout warning";
                if (!pending.empty()) {
                    AsyncRequest* timed_out_req = pending.front();
                    oss << ": loop " << timed_out_req->loop_index << " "
                        << RequestType(timed_out_req) << " request has no completion in "
                        << kAsyncRequestTimeout.count() << " seconds";
                } else {
                    oss << ": no completion in " << kAsyncRequestTimeout.count() << " seconds";
                }
                oss << ", in_flight=" << in_flight;
                set_error(oss.str(), ErrorCodeForStatus(vesal::StatusCode::kTimeout));
            };

            auto submit_compress = [&](uint32_t slot_index) {
                FillInputByCompressRatio(&src[slot_index], &rng);
                comp_reqs[slot_index].window = &window;
                comp_reqs[slot_index].loop_index = next_loop;
                comp_reqs[slot_index].slot_index = slot_index;
                comp_reqs[slot_index].is_decompress = false;
                comp_reqs[slot_index].result = vesal::CodecResult{};

                vesal::StatusCode status = channel->CompressAsync(src[slot_index].data(),
                                                                   src[slot_index].size(),
                                                                   compressed[slot_index].data(),
                                                                   compressed[slot_index].size(),
                                                                   &comp_reqs[slot_index]);
                if (!vesal::IsOk(status)) {
                    std::ostringstream oss;
                    oss << "Thread " << tid << " loop " << next_loop
                        << " CompressAsync submit failed: "
                        << vesal::StatusCodeToString(status);
                    set_error(oss.str(), ErrorCodeForStatus(status));
                    return false;
                }

                comp_reqs[slot_index].submit_time = std::chrono::steady_clock::now();
                pending.push_back(&comp_reqs[slot_index]);
                ++next_loop;
                ++in_flight;
                return true;
            };

            auto submit_decompress = [&](AsyncRequest* comp_req) {
                uint32_t slot_index = comp_req->slot_index;
                vesal::CodecResult& comp_res = comp_req->result;
                decomp_reqs[slot_index].window = &window;
                decomp_reqs[slot_index].loop_index = comp_req->loop_index;
                decomp_reqs[slot_index].slot_index = slot_index;
                decomp_reqs[slot_index].is_decompress = true;
                decomp_reqs[slot_index].result = vesal::CodecResult{};

                vesal::StatusCode status = channel->DecompressAsync(compressed[slot_index].data(),
                                                                     comp_res.produced,
                                                                     decompressed[slot_index].data(),
                                                                     decompressed[slot_index].size(),
                                                                     &decomp_reqs[slot_index]);
                if (!vesal::IsOk(status)) {
                    std::ostringstream oss;
                    oss << "Thread " << tid << " loop " << comp_req->loop_index
                        << " DecompressAsync submit failed: "
                        << vesal::StatusCodeToString(status);
                    set_error(oss.str(), ErrorCodeForStatus(status));
                    return false;
                }

                decomp_reqs[slot_index].submit_time = std::chrono::steady_clock::now();
                pending.push_back(&decomp_reqs[slot_index]);
                ++in_flight;
                return true;
            };

            for (uint32_t slot_index = 0;
                 slot_index < FLAGS_inflight_num && next_loop < FLAGS_loop_num &&
                 !stop.load(std::memory_order_relaxed);
                 ++slot_index) {
                if (!submit_compress(slot_index)) {
                    break;
                }
            }

            while (completed_loops < FLAGS_loop_num && in_flight > 0 &&
                   !stop.load(std::memory_order_relaxed)) {
                AsyncRequest* req = wait_next_completed();
                if (req == nullptr) {
                    set_timeout_error();
                    break;
                }
                remove_pending(req);
                --in_flight;

                if (!vesal::IsOk(req->result.status)) {
                    std::ostringstream oss;
                    oss << "Thread " << tid << " loop " << req->loop_index << " "
                        << (req->is_decompress ? "decompress" : "compress")
                        << " failed: " << vesal::StatusCodeToString(req->result.status);
                    set_error(oss.str(), ErrorCodeForStatus(req->result.status));
                    break;
                }

                if (!req->is_decompress) {
                    compressed_input_bytes.fetch_add(src[req->slot_index].size(),
                                                     std::memory_order_relaxed);
                    if (!submit_decompress(req)) {
                        break;
                    }
                    continue;
                }

                uint32_t slot_index = req->slot_index;
                if (req->result.produced != src[slot_index].size() ||
                    std::memcmp(src[slot_index].data(),
                                decompressed[slot_index].data(),
                                src[slot_index].size()) != 0) {
                    std::ostringstream oss;
                    oss << "Thread " << tid << " loop " << req->loop_index
                        << " data verify failed";
                    set_error(oss.str(), 14);
                    break;
                }

                decompressed_output_bytes.fetch_add(req->result.produced, std::memory_order_relaxed);
                ++completed_loops;
                if (next_loop < FLAGS_loop_num && !stop.load(std::memory_order_relaxed)) {
                    if (!submit_compress(slot_index)) {
                        break;
                    }
                }
            }

            while (in_flight > 0) {
                AsyncRequest* req = wait_next_completed();
                if (req == nullptr) {
                    break;
                }
                remove_pending(req);
                --in_flight;
            }

            vesal::Status close_status = channel->Close();
            if (!close_status.ok()) {
                std::lock_guard<std::mutex> lock(err_mu);
                if (first_error.empty()) {
                    std::ostringstream oss;
                    oss << "Thread " << tid << " channel close failed: "
                        << close_status.ToString();
                    first_error = oss.str();
                }
                stop.store(true, std::memory_order_relaxed);
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }
    auto end = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

    if (!first_error.empty()) {
        vesal::Uninit();
        std::ostringstream oss;
        oss << first_error << ", total_time=" << seconds << " s";
        return PrintAndReturn(oss.str(), first_error_code);
    }

    if (!vesal::Uninit()) {
        return PrintAndReturn("vesal::Uninit failed", 7);
    }

    uint64_t total_ops = static_cast<uint64_t>(FLAGS_thread_num) * FLAGS_loop_num;
    uint64_t total_input_bytes = total_ops * FLAGS_input_size;
    double gib_per_s = seconds > 0 ? (total_input_bytes / 1024.0 / 1024.0 / 1024.0) / seconds : 0;
    double compress_gib_per_s =
        seconds > 0 ? (compressed_input_bytes.load(std::memory_order_relaxed) / 1024.0 / 1024.0 /
                       1024.0) /
                          seconds
                    : 0;
    double decompress_gib_per_s =
        seconds > 0 ? (decompressed_output_bytes.load(std::memory_order_relaxed) / 1024.0 / 1024.0 /
                       1024.0) /
                          seconds
                    : 0;

    std::cout << "QAT LZ4 multi-thread demo success"
              << ", threads=" << FLAGS_thread_num
              << ", loops_per_thread=" << FLAGS_loop_num
              << ", inflight_num=" << FLAGS_inflight_num
              << ", input_size=" << FLAGS_input_size << " bytes"
              << ", compress_ratio=" << FLAGS_compress_ratio
              << ", data_fill_mode=" << FLAGS_data_fill_mode
              << ", compression_level=" << FLAGS_compression_level
              << ", total_ops=" << total_ops
              << ", total_time=" << seconds << " s"
              << ", elapsed=" << seconds << " s"
              << ", compressed_input_bytes=" << compressed_input_bytes.load(std::memory_order_relaxed)
              << ", decompressed_output_bytes="
              << decompressed_output_bytes.load(std::memory_order_relaxed)
              << ", compress_throughput=" << compress_gib_per_s << " GiB/s"
              << ", decompress_throughput=" << decompress_gib_per_s << " GiB/s"
              << ", throughput=" << gib_per_s << " GiB/s" << std::endl;
    return 0;
}
