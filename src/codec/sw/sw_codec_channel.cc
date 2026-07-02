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

#include "sw_codec_channel.h"

#include <cstring>
#include <functional>

#include "codec/codec_common.h"
#include "codec/dc_format.h"
#include "common/defer.h"
#include "common/metrics_internal.h"
#include "common/timestamp.h"
#include "libdeflate.h"
#include "lz4frame.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {
namespace sw {

void ResetDecompressionCtx(LZ4F_decompressionContext_t* dctx) {
    VESAL_CHECK(!LZ4F_isError(LZ4F_freeDecompressionContext(*dctx)));
    VESAL_CHECK(!LZ4F_isError(LZ4F_createDecompressionContext(dctx, LZ4F_VERSION)));
}

inline void DetailedLog(StreamableLogger&& logger,
                        unsigned char* src,
                        unsigned int src_index,
                        unsigned int src_len,
                        unsigned int dst_index,
                        unsigned int dst_len) {
    logger << "src_index=" << src_index << ", src_len=" << src_len << ", dst_index=" << dst_index
           << ", dst_len=" << dst_len;
    if (src_len < LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH) {
        logger << ", LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH=" << LZ4F_MIN_SIZE_TO_KNOW_HEADER_LENGTH;
        return;
    }
    size_t hdr_size = LZ4F_headerSize(src, src_len);
    auto* lz4f_hdr = reinterpret_cast<Lz4FrameHeader*>(src);
    logger << ", hdr_size=" << hdr_size << ", lz4f_hdr->magic=" << lz4f_hdr->magic
           << ", lz4f_hdr->block_desc=" << static_cast<int>(lz4f_hdr->block_desc)
           << ", lz4f_hdr->flag_desc=" << static_cast<int>(lz4f_hdr->flag_desc)
           << ", lz4f_hdr->hdr_cksum=" << static_cast<int>(lz4f_hdr->hdr_cksum);
};

SwCodecChannel::SwCodecChannel(const CodecChannelOption& channel_opts, size_t result_queue_capacity)
    : channel_opts_(channel_opts), closed_(true), result_queue_capacity_(result_queue_capacity) {}

SwCodecChannel::~SwCodecChannel() {
    VESAL_CHECK(closed_);
}

StatusCode SwCodecChannel::CompressAsync(
    unsigned char* src, unsigned int src_len, unsigned char* dst, unsigned int dst_len, void* ctx) {
    if (VESAL_UNLIKELY(codec_results_.Size() >= result_queue_capacity_)) {
        VESAL_LOG(WARN) << "Request overcrowded, retry after polling. result_queue_size="
                        << codec_results_.Size()
                        << ", result_queue_capacity_=" << result_queue_capacity_;
        return StatusCode::kResourceBusy;
    }
    VESAL_LOG(DEBUG) << "submitted compress request to sw engine";
    CodecResult result = Compress(src, src_len, dst, dst_len);
    VESAL_LOG(DEBUG) << "completed compress request by sw engine, result=" << result;
    if (IsOk(result.status)) {
        codec_results_.Push(ctx,
                            StatusCode::kOk,
                            result.consumed,
                            result.produced,
                            result.in_checksum,
                            result.out_checksum);
    } else {
        codec_results_.Push(ctx, result.status);
    }
    return StatusCode::kOk;
}

StatusCode SwCodecChannel::CompressSGLAsync(const std::vector<unsigned char*>& src,
                                            const std::vector<unsigned int>& src_len,
                                            unsigned char* dst,
                                            unsigned int dst_len,
                                            void* ctx) {
    if (VESAL_UNLIKELY(codec_results_.Size() >= result_queue_capacity_)) {
        VESAL_LOG(WARN) << "Request overcrowded, retry after polling. result_queue_size="
                        << codec_results_.Size()
                        << ", result_queue_capacity_=" << result_queue_capacity_;
        return StatusCode::kResourceBusy;
    }
    VESAL_LOG(DEBUG) << "submitted CompressSGL request to sw engine";
    CodecResult result = CompressSGL(src, src_len, dst, dst_len);
    VESAL_LOG(DEBUG) << "completed CompressSGL request by sw engine, result=" << result;
    if (IsOk(result.status)) {
        codec_results_.Push(ctx,
                            StatusCode::kOk,
                            result.consumed,
                            result.produced,
                            result.in_checksum,
                            result.out_checksum);
    } else {
        codec_results_.Push(ctx, result.status);
    }
    return StatusCode::kOk;
}

CodecResult SwCodecChannel::Compress(unsigned char* src,
                                     unsigned int src_len,
                                     unsigned char* dst,
                                     unsigned int dst_len) {
    CodecResult r;
    auto start_time = TimeStamp::Now();
    auto guard = defer([&start_time, &r, this]() {
        if (VESAL_LIKELY(StatusCode::kOk == r.status)) {
            compress_throughput_->Add(r.consumed);
        }
        compress_rps_counters_[static_cast<int>(r.status)]->Add(1);
        compress_e2e_latency_->Set(TimeStamp::DurationToNs(TimeStamp::Now() - start_time));
    });

    if (src_len == 0) {
        VESAL_LOG(ERROR) << "invalid src_len: 0";
        r.status = StatusCode::kInvalidArgument;
        return r;
    }
    switch (channel_opts_.comp_algorithm) {
    case CodecAlgorithm::kZlib:
        r = ZlibCompress(src, src_len, dst, dst_len);
        break;
    case CodecAlgorithm::kDeflate: {
        auto size = libdeflate_deflate_compress(deflate_compressor_, src, src_len, dst, dst_len);
        if (size == 0U) {
            VESAL_LOG(ERROR) << "Deflate compress overflow error, src_len: " << src_len
                             << ", dst_len: " << dst_len;
            r.status = StatusCode::kOverflow;
        } else {
            r.consumed = src_len;
            r.produced = size;
            r.status = StatusCode::kOk;
        }
        break;
    }
    case CodecAlgorithm::kLz4: {
        size_t result = LZ4F_compressFrame(dst, dst_len, src, src_len, &preferences_);
        if (VESAL_UNLIKELY(LZ4F_isError(result) != 0U)) {
            const char* err_name = LZ4F_getErrorName(result);
            VESAL_LOG(ERROR) << "LZ4F_compressFrame error: " << err_name;
            // TODO(...): handle lz4 lib error properly, converting to vesal error. Here is merely a
            // temporary solution because our lz4 lib doesn't expose API to get error code.
            auto cmp_r = strcmp(err_name, "ERROR_dstMaxSize_tooSmall");
            r.status = cmp_r == 0 ? StatusCode::kOverflow : StatusCode::kChannelError;
        } else {
            r.consumed = src_len;
            r.produced = result;
            r.status = StatusCode::kOk;
        }
        break;
    }
    default:
        break;
    }
    if (IsOk(r.status) && channel_opts_.checksum_type == CodecChecksumType::kCrc32) {
        r.in_checksum = ComputeCRC32(kCrc32cInitialValue, reinterpret_cast<char*>(src), r.consumed);
        r.out_checksum =
            channel_opts_.compressed_checksum
                ? ComputeCRC32(kCrc32cInitialValue, reinterpret_cast<char*>(dst), r.produced)
                : 0;
    }
    return r;
}

CodecResult SwCodecChannel::CompressSGL(const std::vector<unsigned char*>& src,
                                        const std::vector<unsigned int>& src_len,
                                        unsigned char* dst,
                                        unsigned int dst_len) {
    // note this implementation is memory consuming and time consuming
    // but suitable for mock usage
    CodecResult r;
    StatusCode validate_result = ValidateSglArgs(src, src_len);
    if (!IsOk(validate_result)) {
        r.status = validate_result;
        return r;
    }
    unsigned int len_sum = 0;
    for (unsigned int len : src_len) {
        len_sum += len;
    }
    auto concatenated_src = std::make_unique<unsigned char[]>(len_sum);
    size_t offset = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        memcpy(&concatenated_src[offset], src[i], src_len[i]);
        offset += src_len[i];
    }
    return Compress(concatenated_src.get(), len_sum, dst, dst_len);
}

StatusCode SwCodecChannel::DecompressAsync(
    unsigned char* src, unsigned int src_len, unsigned char* dst, unsigned int dst_len, void* ctx) {
    if (VESAL_UNLIKELY(codec_results_.Size() >= result_queue_capacity_)) {
        VESAL_LOG(WARN) << "Request overcrowded, retry after polling. result_queue_size="
                        << codec_results_.Size()
                        << ", result_queue_capacity_=" << result_queue_capacity_;
        return StatusCode::kResourceBusy;
    }
    VESAL_LOG(DEBUG) << "submitted decompress request to sw engine";
    CodecResult result = Decompress(src, src_len, dst, dst_len);
    VESAL_LOG(DEBUG) << "completed decompress request by sw engine, result=" << result;
    if (IsOk(result.status)) {
        codec_results_.Push(ctx,
                            StatusCode::kOk,
                            result.consumed,
                            result.produced,
                            result.in_checksum,
                            result.out_checksum);
    } else {
        codec_results_.Push(ctx, result.status);
    }
    return StatusCode::kOk;
}

StatusCode SwCodecChannel::DecompressSGLAsync(const std::vector<unsigned char*>& src,
                                              const std::vector<unsigned int>& src_len,
                                              unsigned char* dst,
                                              unsigned int dst_len,
                                              void* ctx) {
    if (VESAL_UNLIKELY(codec_results_.Size() >= result_queue_capacity_)) {
        VESAL_LOG(WARN) << "Request overcrowded, retry after polling. result_queue_size="
                        << codec_results_.Size()
                        << ", result_queue_capacity_=" << result_queue_capacity_;
        return StatusCode::kResourceBusy;
    }
    VESAL_LOG(DEBUG) << "submitted DecompressSGL request to sw engine";
    CodecResult result = DecompressSGL(src, src_len, dst, dst_len);
    VESAL_LOG(DEBUG) << "completed DecompressSGL request by sw engine, result=" << result;
    if (IsOk(result.status)) {
        codec_results_.Push(ctx,
                            StatusCode::kOk,
                            result.consumed,
                            result.produced,
                            result.in_checksum,
                            result.out_checksum);
    } else {
        codec_results_.Push(ctx, result.status);
    }
    return StatusCode::kOk;
}

CodecResult SwCodecChannel::Decompress(unsigned char* src,
                                       unsigned int src_len,
                                       unsigned char* dst,
                                       unsigned int dst_len) {
    CodecResult r;
    auto start_time = TimeStamp::Now();
    auto guard = defer([&start_time, &r, this]() {
        if (VESAL_LIKELY(StatusCode::kOk == r.status)) {
            decompress_throughput_->Add(r.produced);
        }
        decompress_rps_counters_[static_cast<int>(r.status)]->Add(1);
        decompress_e2e_latency_->Set(TimeStamp::DurationToNs(TimeStamp::Now() - start_time));
    });

    if (src_len == 0) {
        VESAL_LOG(ERROR) << "invalid src_len: 0";
        r.status = StatusCode::kInvalidArgument;
        return r;
    }
    switch (channel_opts_.comp_algorithm) {
    case CodecAlgorithm::kDeflate:
        r = DeflateDecompress(src, src_len, dst, dst_len);
        break;
    case CodecAlgorithm::kLz4:
        r = LZ4Decompress(src, src_len, dst, dst_len);
        break;
    case CodecAlgorithm::kZlib:
        r = ZlibDecompress(src, src_len, dst, dst_len);
        break;
    default:
        VESAL_LOG(CRITICAL) << "Invalid comp_algorithm: "
                            << static_cast<int>(channel_opts_.comp_algorithm);
    }
    if (IsOk(r.status) && channel_opts_.checksum_type == CodecChecksumType::kCrc32) {
        r.in_checksum =
            channel_opts_.compressed_checksum
                ? ComputeCRC32(kCrc32cInitialValue, reinterpret_cast<char*>(src), r.consumed)
                : 0;
        r.out_checksum =
            ComputeCRC32(kCrc32cInitialValue, reinterpret_cast<char*>(dst), r.produced);
    }
    return r;
}

CodecResult SwCodecChannel::DecompressSGL(const std::vector<unsigned char*>& src,
                                          const std::vector<unsigned int>& src_len,
                                          unsigned char* dst,
                                          unsigned int dst_len) {
    // note this implementation is memory consuming and time consuming
    // but suitable for mock usage
    CodecResult r;
    StatusCode validate_result = ValidateSglArgs(src, src_len);
    if (!IsOk(validate_result)) {
        r.status = validate_result;
        return r;
    }
    unsigned int len_sum = 0;
    for (unsigned int len : src_len) {
        len_sum += len;
    }
    auto concatenated_src = std::make_unique<unsigned char[]>(len_sum);
    size_t offset = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        memcpy(&concatenated_src[offset], src[i], src_len[i]);
        offset += src_len[i];
    }
    return Decompress(concatenated_src.get(), len_sum, dst, dst_len);
}

ssize_t SwCodecChannel::Poll(CodecResult results[], unsigned int max_num, int timeout) {
    size_t ret_num = codec_results_.Pop(results, max_num);
    if (channel_opts_.user_cb != nullptr && ret_num > 0) {
        auto ts = TimeStamp::Now();
        for (size_t i = 0; i < ret_num; ++i) {
            channel_opts_.user_cb(results[i]);
        }
        // TODO(...): Seperate comp/decomp for user_cb, currently no easy to get op type here.
        user_cb_time_->Set(TimeStamp::DurationToNs((TimeStamp::Now() - ts)));
    }
    return ret_num;
}

Status SwCodecChannel::Close() {
    if (deflate_compressor_ != nullptr) {
        libdeflate_free_compressor(deflate_compressor_);
        libdeflate_free_decompressor(deflate_decompressor_);
        deflate_compressor_ = nullptr;
        deflate_decompressor_ = nullptr;
    } else if (channel_opts_.comp_algorithm == CodecAlgorithm::kLz4) {
        LZ4F_freeDecompressionContext(dctx_);
    }
    closed_ = true;
    return OkStatus();
}

Status SwCodecChannel::Init() {
    if (channel_opts_.checksum_type != CodecChecksumType::kNone &&
        channel_opts_.checksum_type != CodecChecksumType::kCrc32) {
        return NotSupportedError("Not supported");
    }
    if (channel_opts_.comp_algorithm == CodecAlgorithm::kLz4) {
        InitSoftwarePreferences(channel_opts_, &preferences_);
        LZ4F_errorCode_t result = LZ4F_createDecompressionContext(&dctx_, LZ4F_VERSION);
        if (LZ4F_isError(result) != 0U) {
            return ChannelError(LZ4F_getErrorName(result));
        }
    } else if (channel_opts_.comp_algorithm == CodecAlgorithm::kDeflate) {
        deflate_compressor_ =
            libdeflate_alloc_compressor(static_cast<int>(channel_opts_.comp_level));
        deflate_decompressor_ = libdeflate_alloc_decompressor();
    } else if (channel_opts_.comp_algorithm != CodecAlgorithm::kZlib) {
        return NotSupportedError("Not supported");
    }
    closed_ = false;

    Tag engine_tag = std::make_pair("engine", "sw");
    Tag compress_tag = std::make_pair("type", "compress");
    Tag decompress_tag = std::make_pair("type", "decompress");
    compress_throughput_ =
        g_metric_registry->RegisterCounter("vesal.throughput", {engine_tag, compress_tag});
    decompress_throughput_ =
        g_metric_registry->RegisterCounter("vesal.throughput", {engine_tag, decompress_tag});
    compress_e2e_latency_ =
        g_metric_registry->RegisterHistogram("vesal.e2e_latency", {engine_tag, compress_tag});
    decompress_e2e_latency_ =
        g_metric_registry->RegisterHistogram("vesal.e2e_latency", {engine_tag, decompress_tag});
    for (const auto& code : GetAllStatusCodes()) {
        compress_rps_counters_.push_back(g_metric_registry->RegisterCounter(
            "vesal.rps",
            {engine_tag, compress_tag, std::make_pair("status", StatusCodeToString(code))}));
        decompress_rps_counters_.push_back(g_metric_registry->RegisterCounter(
            "vesal.rps",
            {engine_tag, decompress_tag, std::make_pair("status", StatusCodeToString(code))}));
    }
    user_cb_time_ = g_metric_registry->RegisterHistogram("vesal.user_cb_time", {engine_tag});

    return OkStatus();
}

CodecResult SwCodecChannel::DeflateDecompress(unsigned char* src,
                                              unsigned int src_len,
                                              unsigned char* dst,
                                              unsigned int dst_len) {
    CodecResult r;
    size_t actual_consumed = 0;
    size_t actual_produced = 0;
    auto result = libdeflate_deflate_decompress_ex(
        deflate_decompressor_, src, src_len, dst, dst_len, &actual_consumed, &actual_produced);
    r.consumed = actual_consumed;
    r.produced = actual_produced;
    switch (result) {
    case LIBDEFLATE_SUCCESS:
        r.status = StatusCode::kOk;
        return r;
    case LIBDEFLATE_INSUFFICIENT_SPACE:
        r.status = StatusCode::kOverflow;
        break;
    case LIBDEFLATE_BAD_DATA:
        r.status = StatusCode::kBadData;
        break;
    default:
        r.status = StatusCode::kUnknown;
        break;
    }
    VESAL_LOG(ERROR) << "Deflate decompress error: " << result << ", vesal error: " << r.status
                     << ", src_len: " << src_len << ", dst_len: " << dst_len
                     << ", actual_consumed: " << actual_consumed
                     << ", actual_produced: " << actual_produced;
    return r;
}

CodecResult SwCodecChannel::LZ4Decompress(unsigned char* src,
                                          unsigned int src_len,
                                          unsigned char* dst,
                                          unsigned int dst_len) {
    CodecResult r;
    unsigned int src_index = 0;
    unsigned int dst_index = 0;
    size_t ret = 1;
    while (src_index < src_len && ret != 0) {
        if (VESAL_UNLIKELY(dst_len <= dst_index)) {
            // After a decompression error, the `dctx` context is not resumable.
            // Use LZ4F_resetDecompressionContext() to return to clean state.
            // LZ4F_resetDecompressionContext(dctx_); only added after v1.8. Use an old way to reset
            // it.
            DetailedLog(VESAL_LOG(ERROR), src, src_index, src_len, dst_index, dst_len);
            ResetDecompressionCtx(&dctx_);
            VESAL_LOG(ERROR) << "invalid dst_len: not big enough";
            r.status = StatusCode::kOverflow;
            return r;
        }
        size_t dst_size = dst_len - dst_index;
        size_t src_size = src_len - src_index;
        /*
         * @return : an hint of how many `srcSize` bytes LZ4F_decompress() expects for next call.
         *  Schematically, it's the size of the current (or remaining) compressed block + header of
         * next block. Respecting the hint provides some small speed benefit, because it skips
         * intermediate buffers. This is just a hint though, it's always possible to provide any
         * srcSize.
         *
         *  When a frame is fully decoded, @return will be 0 (no more data expected).
         *  When provided with more bytes than necessary to decode a frame,
         *  LZ4F_decompress() will stop reading exactly at end of current frame, and @return 0.
         *
         *  If decompression failed, @return is an error code, which can be tested using
         * LZ4F_isError(). After a decompression error, the `dctx` context is not resumable. Use
         * LZ4F_resetDecompressionContext() to return to clean state.
         *
         *  After a frame is fully decoded, dctx can be used again to decompress another frame.
         */
        ret =
            LZ4F_decompress(dctx_, dst + dst_index, &dst_size, src + src_index, &src_size, nullptr);
        if (VESAL_UNLIKELY(LZ4F_isError(ret) != 0U)) {
            // After a decompression error, the `dctx` context is not resumable.
            // Use LZ4F_resetDecompressionContext() to return to clean state.
            // LZ4F_resetDecompressionContext(dctx_) only available after v1.8. Use an old way to
            // reset.
            DetailedLog(VESAL_LOG(ERROR), src, src_index, src_len, dst_index, dst_len);
            ResetDecompressionCtx(&dctx_);
            VESAL_LOG(ERROR) << "LZ4F_compressFrame error: " << LZ4F_getErrorName(ret);
            r.status = StatusCode::kChannelError;
            return r;
        }
        src_index += src_size;
        dst_index += dst_size;
    }
    // When a frame is fully decoded, LZ4F_decompress() will return 0 (no more data expected).
    // When provided with more bytes than necessary to decode a frame,
    // LZ4F_decompress() will stop reading exactly at end of current frame, and return 0.
    // therefore the final consumed data might be less than the input
    if (VESAL_UNLIKELY(src_index < src_len)) {
        DetailedLog(VESAL_LOG(WARN), src, src_index, src_len, dst_index, dst_len);
    }
    if (VESAL_UNLIKELY(ret != 0)) {
        VESAL_LOG(ERROR) << "Not enough input for decompression";
        DetailedLog(VESAL_LOG(ERROR), src, src_index, src_len, dst_index, dst_len);
        ResetDecompressionCtx(&dctx_);
        r.status = StatusCode::kBadData;
        return r;
    }
    r.consumed = src_index;
    r.produced = dst_index;
    r.status = StatusCode::kOk;
    return r;
}

CodecResult SwCodecChannel::ZlibCompress(unsigned char* src,
                                         unsigned int src_len,
                                         unsigned char* dst,
                                         unsigned int dst_len) const {
    CodecResult r;
    int zlib_level = ChannelLvlToZlibLvl(channel_opts_.comp_level);
    uint64_t produced = dst_len;
    int zlib_compress_r = compress2(dst, &produced, src, src_len, zlib_level);
    // Zlib has no partial compress.
    r.consumed = src_len;
    r.produced = produced;
    switch (zlib_compress_r) {
    case Z_OK:
        r.status = StatusCode::kOk;
        return r;
    case Z_BUF_ERROR:
        r.status = StatusCode::kOverflow;
        break;
    default:
        r.status = StatusCode::kBadData;
        break;
    }
    VESAL_LOG(ERROR) << "Zlib compress error: " << zlib_compress_r << ", r: " << r
                     << ", src: " << reinterpret_cast<intptr_t>(src) << ", src_len: " << src_len
                     << ", dst: " << reinterpret_cast<intptr_t>(dst) << ", dst_len: " << dst_len;
    return r;
}
CodecResult SwCodecChannel::ZlibDecompress(unsigned char* src,
                                           unsigned int src_len,
                                           unsigned char* dst,
                                           unsigned int dst_len) const {
    CodecResult r;
    uint64_t consumed = src_len;
    uint64_t produced = dst_len;
    int zlib_decompress_r = uncompress2(dst, &produced, src, &consumed);
    r.consumed = consumed;
    r.produced = produced;
    switch (zlib_decompress_r) {
    case Z_OK:
        r.status = StatusCode::kOk;
        return r;
    case Z_BUF_ERROR:
        r.status = StatusCode::kOverflow;
        break;
    default:
        r.status = StatusCode::kBadData;
        break;
    }
    VESAL_LOG(ERROR) << "Zlib decompress error: " << zlib_decompress_r << ", consumed: " << consumed
                     << ", produced: " << produced << ", src: " << reinterpret_cast<intptr_t>(src)
                     << ", src_len: " << src_len << ", dst: " << reinterpret_cast<intptr_t>(dst)
                     << ", dst_len: " << dst_len;
    return r;
}

}  // namespace sw
}  // namespace vesal
