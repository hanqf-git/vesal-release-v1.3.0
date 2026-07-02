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

#pragma once

#include <lz4frame.h>

#include <vector>

#include "codec/codec_common.h"
#include "libdeflate.h"
#include "vesal/codec.h"
#include "vesal/metrics.h"

namespace vesal {
namespace sw {
/*
 * Currently only support lz4 frame level synchronized compression/decompression.
 * Note the we're using frame level API to make sw-based solution and qat-base solution produce the
 * same format of data, which facilitates the smooth switch between different solutions.
 */
class SwCodecChannel : public CodecChannel {
public:
    SwCodecChannel(const CodecChannelOption& channel_opts, size_t result_queue_capacity);
    ~SwCodecChannel();

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
    // the option used for LZ4 frame compression
    // it's reused for every compression
    LZ4F_preferences_t preferences_;
    // the context used for LZ4 frame decompression
    // it's reused for every decompression
    LZ4F_decompressionContext_t dctx_;
    libdeflate_compressor* deflate_compressor_ = nullptr;
    libdeflate_decompressor* deflate_decompressor_ = nullptr;
    bool closed_;

    CodecResultList codec_results_;
    size_t result_queue_capacity_;

    std::shared_ptr<Counter> compress_throughput_;    // rate of total data compressed by sw
    std::shared_ptr<Counter> decompress_throughput_;  // rate of total data decompressed by sw

    // e2e latency
    std::shared_ptr<Histogram> compress_e2e_latency_;
    std::shared_ptr<Histogram> decompress_e2e_latency_;

    // counter of each type of request's result
    std::vector<std::shared_ptr<Counter>> compress_rps_counters_;
    std::vector<std::shared_ptr<Counter>> decompress_rps_counters_;

    std::shared_ptr<Histogram> user_cb_time_;  // the time cost of user callback function in each
                                               // Poll() call. Might contain multiple calls.

    CodecResult DeflateDecompress(unsigned char* src,
                                  unsigned int src_len,
                                  unsigned char* dst,
                                  unsigned int dst_len);
    CodecResult LZ4Decompress(unsigned char* src,
                              unsigned int src_len,
                              unsigned char* dst,
                              unsigned int dst_len);

    CodecResult ZlibCompress(unsigned char* src,
                             unsigned int src_len,
                             unsigned char* dst,
                             unsigned int dst_len) const;
    CodecResult ZlibDecompress(unsigned char* src,
                               unsigned int src_len,
                               unsigned char* dst,
                               unsigned int dst_len) const;
};

}  // namespace sw
}  // namespace vesal
