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

#pragma once

#include <memory>

#include "codec/codec_common.h"
#include "common/dedicated_pool.h"
#include "common/req_ring_queue.h"
extern "C" {
#include "kaezip.h"
}
#include "vesal/codec.h"
#include "vesal/metrics.h"

#define MAX_KAE_CONCURRENCY (1024)  // This is fixed by kae driver

namespace vesal {
namespace kae {

struct ReqContext;
void KaeCallback(struct kaezip_result* result);

class KaeCodecChannel : public CodecChannel {
public:
    KaeCodecChannel(const CodecChannelOption& channel_opts, const device_config_t& device_config);
    ~KaeCodecChannel();

    StatusCode CompressAsync(unsigned char* src,
                             unsigned int src_len,
                             unsigned char* dst,
                             unsigned int dst_len,
                             void* ctx) override;
    StatusCode CompressSGLAsync(const std::vector<unsigned char*>& src,
                                const std::vector<unsigned int>& src_len,
                                unsigned char* dst,
                                unsigned int dst_len,
                                void* ctx) override;
    CodecResult Compress(unsigned char* src,
                         unsigned int src_len,
                         unsigned char* dst,
                         unsigned int dst_len) override;
    CodecResult CompressSGL(const std::vector<unsigned char*>& src,
                            const std::vector<unsigned int>& src_len,
                            unsigned char* dst,
                            unsigned int dst_len) override;
    StatusCode DecompressAsync(unsigned char* src,
                               unsigned int src_len,
                               unsigned char* dst,
                               unsigned int dst_len,
                               void* ctx) override;

    StatusCode DecompressSGLAsync(const std::vector<unsigned char*>& src,
                                  const std::vector<unsigned int>& src_len,
                                  unsigned char* dst,
                                  unsigned int dst_len,
                                  void* ctx) override;
    CodecResult Decompress(unsigned char* src,
                           unsigned int src_len,
                           unsigned char* dst,
                           unsigned int dst_len) override;
    CodecResult DecompressSGL(const std::vector<unsigned char*>& src,
                              const std::vector<unsigned int>& src_len,
                              unsigned char* dst,
                              unsigned int dst_len) override;
    CodecResult Submit(const CodecRequestArgs& args) override {
        return CodecResult{0, 0, 0, 0, StatusCode::kNotSupported};
    }
    StatusCode SubmitAsync(const CodecRequestArgs& args) override {
        return StatusCode::kNotSupported;
    }
    ssize_t Poll(CodecResult results[], unsigned int max_num, int timeout) override;
    Status Close() override;
    Status Init();

private:
    CodecChannelOption channel_opts_;
    bool closed_;

    void* compress_sess_;
    void* decompress_sess_;

    device_config_t device_config_;
    uint64_t req_id_;
    unsigned int inflight_num_;

    InflightReqRingQueue<CodecResult> inflight_req_queue_;
    std::unique_ptr<DedicatedPool<ReqContext>> req_context_pool_;

    // metrics
    std::shared_ptr<Counter> compress_throughput_;    // rate of total data compressed by qat
    std::shared_ptr<Counter> decompress_throughput_;  // rate of total data decompressed by qat
    // latency of process before submitting to hardware
    std::shared_ptr<Histogram> compress_preprocess_latency_;
    std::shared_ptr<Histogram> decompress_preprocess_latency_;
    // latency of callback function
    std::shared_ptr<Histogram> compress_postprocess_latency_;
    std::shared_ptr<Histogram> decompress_postprocess_latency_;
    // latency from user submitting requests to getting results or timeout
    std::shared_ptr<Histogram> compress_e2e_latency_;
    std::shared_ptr<Histogram> decompress_e2e_latency_;
    std::shared_ptr<Histogram>
        compress_submit_latency_;  // time of submitting compress requests to qat driver
    std::shared_ptr<Histogram>
        decompress_submit_latency_;  // time of submitting decompress requests to qat driver
    std::vector<std::shared_ptr<Counter>>
        compress_rps_counters_;  // counter of each type of request's result
    std::vector<std::shared_ptr<Counter>>
        decompress_rps_counters_;  // counter of each type of request's result
    std::shared_ptr<Gauge> metric_in_kae_num_;
    std::shared_ptr<Gauge> metric_max_in_kae_num_;
    std::shared_ptr<Counter> poll_total_time_;  // the total time cost of poll
    std::shared_ptr<Counter>
        poll_busy_time_;  // the time cost of poll if at least one result got polled
    std::shared_ptr<Histogram> poll_interval_;  // the interval time between poll function calls
    std::shared_ptr<Histogram> user_cb_time_;   // the time cost of user callback function in each
                                                // Poll() call. Might contain multiple calls.
    uint64_t last_poll_return_time_ = 0;
    uint32_t periodic_task_id_;

    void PrepareCtx(ReqContext* req_context,
                    const std::vector<unsigned char*>& src,
                    const std::vector<unsigned int>& src_len,
                    unsigned char* dst,
                    unsigned int dst_len,
                    void* ctx);
    friend void KaeCallback(struct kaezip_result* result);
};

struct ReqContext {
    KaeCodecChannel* channel;
    uint64_t req_id;
    uint64_t req_ring_id;
    kaezip_result result;
    kaezip_buffer_list src_buffer_list;
    kaezip_buffer_list dst_buffer_list;
    void* usr_ctx;
    bool is_compress;
    uint64_t submit_time;
};

}  // namespace kae
}  // namespace vesal
