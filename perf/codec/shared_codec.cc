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

#include <unistd.h>

#include <thread>
#ifdef VESAL_ENABLE_BYTE_METRICS
#include "byte/embedded_metrics/metrics.h"
#include "byte/embedded_metrics/metrics_registry.h"
#endif
#include <atomic>
#include <fstream>
#include <memory>

#include "gflags/gflags.h"
#include "vesal/status.h"
#include "vesal/vesal.h"
DEFINE_string(metrics_prefix, "metrics_trigger", "Metrics prefix, by default: metrics_trigger");
DEFINE_string(source_path,
              "",
              "source file path. If not specified, will try to read ./perf/calgary.");
DEFINE_uint64(data_size_gb, 4, "how many data(GB) in total for perf");
DEFINE_uint64(source_size, 64 * 1024, "size of a single source data chunk");
DEFINE_uint32(channel_num, 2, "how many channels to use");
std::unique_ptr<unsigned char[]> g_source_data;
uint64_t g_total_size;
uint64_t g_file_size;
void EchoAllFlags() {
    std::string strs = gflags::CommandlineFlagsIntoString();
    std::string t;
    for (size_t i = 0, begin = 0; i < strs.size(); ++i) {
        if (strs[i] == '\n') {
            t = strs.substr(begin, i - begin);
            if (!t.empty() && t.back() != '=') {
                VESAL_LOG(INFO) << t;
            }
            begin = i + 1;
            continue;
        }
    }
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
    g_total_size = static_cast<uint64_t>(FLAGS_data_size_gb) * 1024 * 1024 * 1024;
    g_file_size = file_size / FLAGS_source_size * FLAGS_source_size;
    VESAL_CHECK(g_total_size >= g_file_size);
    g_total_size = g_total_size / FLAGS_source_size * FLAGS_source_size;
    g_source_data = std::make_unique<unsigned char[]>(file_size);
    ifs.read(reinterpret_cast<char*>(g_source_data.get()), g_file_size);
    VESAL_LOG(INFO) << "file_size=" << file_size << ", total_size=" << g_total_size;
}
struct Ctx {
    unsigned char* dst;
    size_t i;
};
std::atomic<size_t> g_inflight_num[256] = {};
void Cb(const vesal::CodecResult& res) {
    VESAL_DCHECK(res.status == vesal::StatusCode::kOk) << res.status;
    auto* ctx = reinterpret_cast<Ctx*>(res.ctx);
    g_inflight_num[ctx->i].fetch_sub(1, std::memory_order_acq_rel);
    delete[] ctx->dst;
    delete ctx;
}
void Process(vesal::CodecChannel* ch, size_t idx) {
    size_t curr = 0;
    size_t dst_size = FLAGS_source_size << 1;
    while (curr < g_total_size) {
        auto* src = g_source_data.get() + (curr % g_file_size);
        auto* dst = new unsigned char[dst_size];
        auto* ctx = new Ctx;
        ctx->dst = dst;
        ctx->i = idx;
        auto r = ch->CompressAsync(src, FLAGS_source_size, dst, dst_size, ctx);
        VESAL_DCHECK(r == vesal::StatusCode::kOk);
        g_inflight_num[idx].fetch_add(1, std::memory_order_acq_rel);
        curr += FLAGS_source_size;
        while (g_inflight_num[idx].load(std::memory_order_acquire) > 512) {
            usleep(200);
        }
    }
}
int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_vesal_log_console_output = true;
    vesal::InitOptions opt;
    opt.codec_init_opt.init_qat = true;
    opt.cypher_init_opt.init_qat = false;
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
    VESAL_CHECK(vesal::Init(opt));
    EchoAllFlags();
    ReadSourceData();
    vesal::CodecChannelOption opts;
    opts.user_cb = Cb;
    opts.mode = vesal::ChannelMode::kShared;
    opts.comp_algorithm = vesal::CodecAlgorithm::kZlib;
    auto fn = [&](size_t i) {
        auto r = vesal::CodecChannel::CreateCodecChannel(opts);
        VESAL_CHECK(r.first.ok());
        auto ch = std::move(r.second);
        Process(ch.get(), i);
        ch->Close();
    };
    std::vector<std::thread> threads;
    for (size_t i = 0; i < FLAGS_channel_num; ++i) {
        threads.emplace_back(fn, i);
    }
    for (auto& t : threads) {
        t.join();
    }
    vesal::Uninit();
    return 0;
}
