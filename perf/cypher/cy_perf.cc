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
#include <memory>
#include <string>
#include <vector>

#include "simple_histogram.h"
#include "util.h"
#include "vesal/cypher.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

#ifdef VESAL_ENABLE_BYTE_METRICS
#include "byte/embedded_metrics/metrics.h"
#include "byte/embedded_metrics/metrics_registry.h"
#endif

DEFINE_uint64(total_data_per_thread, 10, "how many data(GB) in total for perf");
DEFINE_uint32(inflight_num, 64, "");
DEFINE_uint64(source_size, 4 * 1024, "size of a single source data chunk");
DEFINE_uint32(channel_num, 1, "number of channels per thread");
DEFINE_string(metrics_prefix, "cy_perf", "Metrics prefix, by default: cy_perf");
DEFINE_string(engine, "qat", "engine type, can be qat or sw");
DEFINE_string(algo, "aes", "algorithm type: [aes, sha256], aes by default");
DEFINE_bool(encryption_perf, true, "whether to do encryption perf");
DEFINE_bool(decryption_perf, true, "whether to do decryption perf");

std::vector<std::unique_ptr<vesal::CypherChannel>> g_channels;
SimpleCSVWriter g_writer;

struct Context {
    std::chrono::time_point<std::chrono::steady_clock> submit_time;
};

void Run(vesal::CypherOp op,
         std::vector<unsigned char*>& src,
         std::vector<unsigned char*>& dst,
         unsigned char* iv,
         uint64_t total,
         bool output = true) {
    const int channel_num = g_channels.size();
    const int source_size = FLAGS_source_size;
    const int inflight_num = FLAGS_inflight_num;
    uint64_t sent_num = 0;  // how many requests sent now
    uint64_t res_num = 0;   // how many response received now
    auto engine = FLAGS_engine == "qat" ? vesal::EngineType::kQat : vesal::EngineType::kSoftware;

    vesal::CypherResult results[inflight_num];
    vesal::CypherReqArgs args[channel_num][inflight_num];
    Context ctxs[channel_num][inflight_num];
    uint32_t batch_num[channel_num];
    uint32_t batch_index[channel_num];
    double processed_data = 0;
    SimpleHistogram histogram(0, 1000000, 1000000);

    for (int i = 0; i < channel_num; i++) {
        batch_num[i] = inflight_num;
        batch_index[i] = 0;
        for (int j = 0; j < inflight_num; j++) {
            args[i][j].op = op;
            args[i][j].aes_xts_tweak = iv;
            args[i][j].ctx = &ctxs[i][j];
        }
    }

    auto start_time = std::chrono::steady_clock::now();
    while (sent_num < total || res_num < sent_num) {
        for (int channel_i = 0; channel_i < channel_num; channel_i++) {
            auto submit_t = std::chrono::steady_clock::now();
            auto* channel = g_channels[channel_i].get();
            uint32_t& idx = batch_index[channel_i];
            while (batch_num[channel_i] > 0 && sent_num < total) {
                int data_idx = channel_i * inflight_num + idx;
                auto& arg = args[channel_i][idx];
                ((Context*)arg.ctx)->submit_time = submit_t;
                auto r = channel->SubmitCypherReq(
                    src[data_idx], source_size, dst[data_idx], source_size, &args[channel_i][idx]);
                VESAL_CHECK(vesal::IsOk(r));
                if (engine == vesal::EngineType::kSoftware) {
                    histogram.Add(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - submit_t)
                                      .count());
                    res_num++;
                }
                --batch_num[channel_i];
                idx = (idx + 1) % inflight_num;
                ++sent_num;
            }
        }
        if (engine == vesal::EngineType::kQat) {
            for (int channel_i = 0; channel_i < channel_num; channel_i++) {
                auto* channel = g_channels[channel_i].get();
                auto time_now = std::chrono::steady_clock::now();
                int m = channel->Poll(results, inflight_num, -1);
                VESAL_DCHECK(m != -1);
                batch_num[channel_i] += m;
                res_num += m;
                for (int j = 0; j < m; j++) {
                    VESAL_CHECK(vesal::IsOk(results[j].status));
                    Context* ctx = (Context*)results[j].ctx;
                    histogram.Add(std::chrono::duration_cast<std::chrono::microseconds>(
                                      time_now - ctx->submit_time)
                                      .count());
                }
            }
        } else {
            for (int channel_i = 0; channel_i < channel_num; channel_i++) {
                batch_num[channel_i] = inflight_num;
            }
        }
    }
    processed_data = total * source_size;
    auto end_time = std::chrono::steady_clock::now();
    auto total_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();
    if (output) {
        VESAL_LOG(INFO) << "Finished task: "
                        << (op == vesal::CypherOp::kEncrypt ? "Encryption" : "Decryption");
        double throughput = processed_data * 1000.0 * 1000 / 1024 / 1024 / 1024 / total_us;
        VESAL_LOG(INFO) << "timecost: " << total_us / 1000000.0
                        << "[s], consume total size: " << processed_data / 1024 / 1024 / 1024
                        << "[GiB], consume speed: "
                        << processed_data * 1000.0 * 1000 / 1024 / 1024 / 1024 / total_us
                        << "[GiB/s]";
        EchoHistogram(histogram);

        // dump results to csv
        const char* op_str = "";
        switch (op) {
        case vesal::CypherOp::kEncrypt:
            op_str = "encrypt";
            break;
        case vesal::CypherOp::kDecrypt:
            op_str = "decrypt";
            break;
        case vesal::CypherOp::kHash:
            op_str = "Hash";
            break;
        }
        g_writer.WriteRow(op_str,
                          FLAGS_engine,
                          FLAGS_channel_num,
                          FLAGS_source_size,
                          FLAGS_inflight_num,
                          throughput,
                          histogram.GetAvg(),
                          histogram.GetPercentage(50),
                          histogram.GetPercentage(99),
                          histogram.GetPercentage(99.9),
                          histogram.GetMax());
    }
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_vesal_log_console_output = true;
    EchoAllFlags();

    vesal::InitOptions opt;
    opt.data_flow_init_opt.init_dsa = false;
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
    auto* memory_pool = vesal::MemoryPool::GetInstance();
    VESAL_CHECK(memory_pool->Init()) << "memory pool init failure";

    // prepare data and channels
    auto engine = FLAGS_engine == "qat" ? vesal::EngineType::kQat : vesal::EngineType::kSoftware;
    auto algo =
        FLAGS_algo == "aes" ? vesal::CypherAlgorithm::kAES_XTS : vesal::CypherAlgorithm::kSHA256;
    const int N = FLAGS_inflight_num * FLAGS_channel_num;
    const int source_size = FLAGS_source_size;
    const int key_len = 32;
    const int iv_len = 16;
    unsigned char* iv = (unsigned char*)memory_pool->Allocate(iv_len);
    std::vector<unsigned char*> encrypted_data(N);
    std::vector<unsigned char*> decrypted_data(N);
    std::string key1 = std::string(key_len, '0');
    std::string key2 = std::string(key_len, '1');
    std::unique_ptr<vesal::CypherResult[]> results(new vesal::CypherResult[N]);

    for (int i = 0; i < N; i++) {
        encrypted_data[i] = (unsigned char*)memory_pool->Allocate(source_size);
        decrypted_data[i] = (unsigned char*)memory_pool->Allocate(source_size);
        VESAL_DCHECK(encrypted_data[i] != nullptr);
        VESAL_DCHECK(decrypted_data[i] != nullptr);
    }

    for (auto i = FLAGS_channel_num; i; i--) {
        vesal::CypherChannelOption opt;
        opt.session_option.aes_xts_spec.aes_xts_key1 = key1;
        opt.session_option.aes_xts_spec.aes_xts_key2 = key2;
        opt.session_option.algorithm = algo;
        opt.engine = engine;
        auto result = vesal::CypherChannel::CreateCypherChannel(opt);
        VESAL_CHECK(result.first.ok());
        g_channels.push_back(std::move(result.second));
    }

    // warm up and prepare decrypted data
    { Run(vesal::CypherOp::kEncrypt, encrypted_data, decrypted_data, iv, N, false); }

    // Run perf
    g_writer.OpenFile("cy_perf_results.csv");
    g_writer.WriteRow("cypher_op",
                      "engine",
                      "channel_num",
                      "source_size",
                      "inflight_num",
                      "Throughput[GiB/s]",
                      "Avg latency[us]",
                      "P50 latency[us]",
                      "P99 latency[us]",
                      "P999 latency[us]",
                      "Max latency[us]");
    uint64_t total = (FLAGS_total_data_per_thread << 30) / source_size;
    switch (algo) {
    case vesal::CypherAlgorithm::kAES_XTS: {
        if (FLAGS_encryption_perf) {
            Run(vesal::CypherOp::kEncrypt, encrypted_data, decrypted_data, iv, total);
        }

        if (FLAGS_decryption_perf) {
            Run(vesal::CypherOp::kDecrypt, decrypted_data, encrypted_data, iv, total);
        }
        break;
    }
    case vesal::CypherAlgorithm::kSHA256: {
        Run(vesal::CypherOp::kHash, encrypted_data, decrypted_data, iv, total);
        break;
    }
    default:
        VESAL_LOG(ERROR) << "Unsupported algorithm";
    }

    g_writer.CloseFile();

    // clean up
    for (auto& channel : g_channels) {
        channel->Close();
    }
    memory_pool->Deallocate(iv);
    for (int i = 0; i < N; i++) {
        memory_pool->Deallocate(encrypted_data[i]);
        memory_pool->Deallocate(decrypted_data[i]);
    }
    vesal::Uninit();
}