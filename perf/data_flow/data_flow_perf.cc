/*
 * Copyright (c) 2025 ByteDance Inc.
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
#include <sys/mman.h>

#include <chrono>
#include <random>

#include "simple_histogram.h"
#include "util.h"
#include "vesal/data_flow.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

#ifdef VESAL_ENABLE_BYTE_METRICS
#include "byte/embedded_metrics/metrics.h"
#include "byte/embedded_metrics/metrics_registry.h"
#endif

DEFINE_uint64(perf_total_data, 50, "how many data(GB) in total for perf");
DEFINE_uint32(io_depth, 8, "");
DEFINE_uint64(chunk_size, 4 * 1024, "chunk size");
DEFINE_uint64(batch_size, 1, "batch size");
DEFINE_uint32(op, 1, "0: memcpy with crc; 1: memcpy; 2: crc");
DEFINE_string(metrics_prefix, "data_flow_perf", "Metrics prefix, by default: data_flow_perf");
DEFINE_uint32(numa, 0, "which numa");

enum class PerfOp: uint8_t {
    kMoveWithCrc = 0,
    kMove = 1,
    kCrc = 2,
    kNum = 3,
};

uint64_t g_hugepage_size;
uint64_t g_hugepage_total;
unsigned char* g_hugepage_address;
vesal::MemoryPool* memory_pool;

unsigned char* GetDMAMemoryChunk(uint64_t len) {
    static uint64_t offset = 0;
    VESAL_CHECK(len <= g_hugepage_size)
        << "memory requested is bigger than a hugepage size" << len << " " << g_hugepage_size;
    // avoid using memory blocks that span hugepages
    uint64_t diff = g_hugepage_size - offset % g_hugepage_size;
    if (diff < len)
        offset += diff;
    offset += len;
    VESAL_CHECK(offset <= g_hugepage_total)
        << "memory requested is bigger than hugepages total size";
    return (g_hugepage_address + offset - len);
}

static void initialize_data_with_random(uint8_t* data, size_t size) {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<uint8_t> distribution(0, 255);
    for (size_t i = 0; i < size; ++i) {
        data[i] = distribution(generator);
    }
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_vesal_log_console_output = true;

    vesal::InitOptions opt;
    opt.codec_init_opt.init_qat = false;
    opt.data_flow_init_opt.init_dsa = true;
    opt.data_flow_init_opt.schedule_hint_type = vesal::DataFlowScheduleHintType::kNumaAware;
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
    EchoAllFlags();

    // always 1GB input data
    g_hugepage_size = VESAL_PAGESIZE(PAGE_BIT_NUM_1GB);
    int64_t src_total = g_hugepage_size;
    g_hugepage_total = 2 * g_hugepage_size;
    // allocate 1GB input + 1GB output = 2GB hugepage
    vesal::MemoryInfo mem_info = {
        .virtual_addr = AllocHugepageFn(g_hugepage_size, g_hugepage_total / g_hugepage_size),
        .physical_addr = nullptr,
        .len = g_hugepage_total,
        .page_size = g_hugepage_size};

    memory_pool = vesal::MemoryPool::GetInstance();
    VESAL_CHECK(memory_pool->Init()) << "memory pool init failure";
    VESAL_CHECK(memory_pool->Register({mem_info})) << "memory pool register failure";
    g_hugepage_address = reinterpret_cast<unsigned char*>(mem_info.virtual_addr);
    initialize_data_with_random(g_hugepage_address, src_total);

    VESAL_CHECK(src_total % FLAGS_chunk_size == 0) << "src_total\%FLAGS_chunk_size!=0";
    uint64_t chunk_num = src_total / FLAGS_chunk_size;

    std::chrono::time_point<std::chrono::steady_clock> start_time;
    std::chrono::time_point<std::chrono::steady_clock> end_time;
    std::vector<unsigned char*> src(chunk_num);
    std::vector<unsigned char*> dst(chunk_num);
    std::vector<std::chrono::time_point<std::chrono::steady_clock>> time_point(FLAGS_io_depth);
    for (uint64_t i = 0; i < chunk_num; ++i) {
        src[i] = GetDMAMemoryChunk(FLAGS_chunk_size);
    }
    for (uint64_t i = 0; i < chunk_num; ++i) {
        dst[i] = GetDMAMemoryChunk(FLAGS_chunk_size);
    }

    vesal::DataFlowChannelOptions opts;
    opts.engine_type = vesal::DataFlowEngineType::kDsa;
    // currently only perf on 1 channel 1 numa
    opts.schedule_hint = FLAGS_numa;
    auto result = vesal::DataFlowChannel::CreateDataFlowChannel(opts);
    VESAL_CHECK(result.first == vesal::StatusCode::kOk);
    auto channel = std::move(result.second);
    size_t wip_num = 0;
    uint64_t processed_data = 0;
    size_t submit_index = 0;
    size_t chunk_index = 0;
    size_t poll_index = 0;
    vesal::DataFlowResult results[FLAGS_io_depth];
    vesal::DataFlowMoveOperation move_ops[vesal::kMaxSubmittedBatchSize];
    vesal::DataFlowCrcOperation crc_ops[vesal::kMaxSubmittedBatchSize];
    VESAL_CHECK(FLAGS_batch_size <= vesal::kMaxSubmittedBatchSize);
    VESAL_CHECK(FLAGS_op<static_cast<uint32_t>(PerfOp::kNum));
    PerfOp perf_op = static_cast<PerfOp>(FLAGS_op);
    VESAL_LOG(INFO) << "preparation done";
    start_time = std::chrono::steady_clock::now();
    SimpleHistogram histogram(0, 1000000, 1000000);
    VESAL_CHECK(FLAGS_io_depth * FLAGS_batch_size <= chunk_num);
    while (processed_data < FLAGS_perf_total_data * 1024 * 1024 * 1024) {
        while (wip_num < FLAGS_io_depth) {
            time_point[submit_index] = std::chrono::steady_clock::now();
            if (perf_op == PerfOp::kMoveWithCrc || perf_op == PerfOp::kMove) {
                for (size_t i = 0; i < FLAGS_batch_size; ++i) {
                    move_ops[i].src = {src[chunk_index]};
                    move_ops[i].src_len = {FLAGS_chunk_size};
                    move_ops[i].dst = dst[chunk_index];
                    move_ops[i].enable_crc = (perf_op == PerfOp::kMoveWithCrc);
                    ++chunk_index;
                    chunk_index %= chunk_num;
                }
                auto submit_res = channel->SubmitMove(move_ops, FLAGS_batch_size, nullptr);
                VESAL_CHECK(submit_res == vesal::StatusCode::kOk);
            } else {
                for (size_t i = 0; i < FLAGS_batch_size; ++i) {
                    crc_ops[i].src = src[chunk_index];
                    crc_ops[i].len = FLAGS_chunk_size;
                    crc_ops[i].seed = 0;
                    ++chunk_index;
                    chunk_index %= chunk_num;
                }
                auto submit_res = channel->SubmitCrc(crc_ops, FLAGS_batch_size, nullptr);
                VESAL_CHECK(submit_res == vesal::StatusCode::kOk);
            }
            ++wip_num;
            ++submit_index;
            submit_index %= FLAGS_io_depth;
        }
        int64_t poll_num = channel->Poll(results, wip_num, -1);
        VESAL_CHECK(poll_num >= 0);
        auto time_now = std::chrono::steady_clock::now();
        for (int i = 0; i < poll_num; ++i) {
            auto diff_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 time_now - time_point[poll_index])
                                 .count();
            histogram.Add(diff_time);
            VESAL_CHECK(results[i].status == vesal::StatusCode::kOk);
            ++poll_index;
            poll_index %= FLAGS_io_depth;
        }
        wip_num -= poll_num;
        processed_data += poll_num * FLAGS_chunk_size * FLAGS_batch_size;
    }
    end_time = std::chrono::steady_clock::now();
    auto total_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    VESAL_LOG(INFO) << "timecost: " << total_us
                    << "[us], consume total size: " << processed_data / 1024 / 1024 / 1024
                    << "[GB], consume speed: "
                    << processed_data * 1000.0 * 1000 / 1024 / 1024 / 1024 / total_us << "[GB/s]";
    VESAL_LOG(INFO) << "latency(ns), avg: " << histogram.GetAvg() << ", max: " << histogram.GetMax()
                    << ", p50: " << histogram.GetPercentage(50)
                    << ", p99: " << histogram.GetPercentage(99)
                    << ", p999: " << histogram.GetPercentage(99.9);

    DeallocHugepageFn(g_hugepage_address, g_hugepage_total);
    auto status = vesal::Uninit();
    VESAL_CHECK(status);

    if (memory_pool) {
        memory_pool->Reset();
        memory_pool = nullptr;
    }
}
