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

#include "codec/qat/qat_poller_worker.h"
#include "vesal/codec.h"
#include "vesal/status.h"

namespace vesal {
namespace qat {

// All Compress/Decompress APIs are thread-safe.
class QatCodecSharedChannel : public CodecChannel {
public:
    QatCodecSharedChannel(const CodecChannelOption& channel_opts)
        : channel_opts_(channel_opts), worker_(nullptr) {}

    Status Init();

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
    };

    StatusCode SubmitAsync(const CodecRequestArgs& args) override {
        return StatusCode::kNotSupported;
    };

    // Poll has no effect on shared channel.
    ssize_t Poll(CodecResult results[], unsigned int max_num, int timeout) override {
        VESAL_CHECK(false) << "Should not be called on shared channel";
        return -1;
    }

    Status Close() override;

    // Called by others, like QatPollerWorker, to send the metrics.
    void SendQueueLatencyMetric(CodecDirection direction, int64_t latency_ns) {
        if (direction == CodecDirection::kComp) {
            shared_compress_queue_latency_->Set(latency_ns);
        } else {
            shared_decompress_queue_latency_->Set(latency_ns);
        }
    }

    // Called by others, like QatPollerWorker, to send the metrics.
    void SendE2ELatencyMetric(CodecDirection direction, int64_t latency_us) {
        if (direction == CodecDirection::kComp) {
            shared_compress_e2e_latency_->Set(latency_us);
        } else {
            shared_decompress_e2e_latency_->Set(latency_us);
        }
    }

private:
    static bool Prepare(const std::vector<unsigned char*>& src,
                        const std::vector<unsigned int>& src_len,
                        unsigned char* dst,
                        unsigned int dst_len,
                        void* ctx,
                        CodecDirection direction);

    StatusCode SubmitInternal(const std::vector<unsigned char*>& src,
                              const std::vector<unsigned int>& src_len,
                              unsigned char* dst,
                              unsigned int dst_len,
                              void* ctx,
                              CodecDirection direction,
                              UserCallback user_cb);

    CodecChannelOption channel_opts_;
    QatPollerWorker* worker_;

    // How long a compress/decompress request will be queued in the poller's task queue, untill it
    // start getting processed by the poller.
    std::shared_ptr<Histogram> shared_compress_queue_latency_;
    std::shared_ptr<Histogram> shared_decompress_queue_latency_;
    // The whole end-to-end time cost of the compress/decompress request. Including queuing time in
    // the poller task queue.
    std::shared_ptr<Histogram> shared_compress_e2e_latency_;
    std::shared_ptr<Histogram> shared_decompress_e2e_latency_;
};

}  // namespace qat
}  // namespace vesal
