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

#include "kae_codec_channel.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <numeric>

#include "codec/codec_common.h"
#include "common/checksum_impl.h"
#include "common/dedicated_pool.h"
#include "common/memory_pool_helper.h"
#include "common/metrics_internal.h"
#include "common/scheduler.h"
#include "common/timestamp.h"
#include "kae_util.h"
#include "kaezip_dev.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

extern "C" {
#include "kaezip.h"
}

namespace vesal {
namespace kae {

void* VAddr2PAddr(void* usr, void* vaddr, size_t sz) {
    return (void*)(uintptr_t)LookUpAddr(vaddr);
}

checksum32_t SwCalcCRC32(kaezip_buffer_list* buffer_list, size_t len) {
    if (buffer_list->buf_num == 1) {
        return ComputeCRC32(kCrc32cInitialValue, (const char*)buffer_list->buf[0].data, len);
    }
    auto tmp_buffer = std::make_unique<char[]>(len);
    size_t offset = 0;
    for (unsigned int i = 0; i < buffer_list->buf_num; ++i) {
        memcpy(tmp_buffer.get() + offset, buffer_list->buf[i].data, buffer_list->buf[i].buf_len);
        offset += buffer_list->buf[i].buf_len;
    }
    return ComputeCRC32(kCrc32cInitialValue, tmp_buffer.get(), len);
}

void KaeCallback(struct kaezip_result* result) {
    bool do_measure = IsEnableSampling();
    auto start_time = do_measure ? TimeStamp::Now() : 0;
    auto* req_context = (ReqContext*)result->user_data;
    CodecResult codec_result = {
        .consumed = (unsigned int)result->src_size,
        .produced = (unsigned int)result->dst_len,
        .in_checksum = 0,
        .out_checksum = 0,
        .status = KaeStatusToStatusCode(result->status),
        .ctx = req_context->usr_ctx,
    };
    if (req_context->channel->channel_opts_.checksum_type == CodecChecksumType::kCrc32 &&
        IsOk(codec_result.status)) {
        codec_result.in_checksum =
            SwCalcCRC32(&req_context->src_buffer_list, codec_result.consumed);
        codec_result.out_checksum =
            SwCalcCRC32(&req_context->dst_buffer_list, codec_result.produced);
    }
    req_context->channel->inflight_req_queue_.PushResult(codec_result, req_context->req_ring_id);
    req_context->channel->inflight_num_--;
    auto* channel = req_context->channel;
    auto& throughput =
        req_context->is_compress ? channel->compress_throughput_ : channel->decompress_throughput_;
    auto& counters = req_context->is_compress ? channel->compress_rps_counters_
                                              : channel->decompress_rps_counters_;
    auto& e2e_latency = req_context->is_compress ? channel->compress_e2e_latency_
                                                 : channel->decompress_e2e_latency_;
    if (IsOk(codec_result.status)) {
        throughput->Add((uint64_t)codec_result.produced);
    }
    counters[static_cast<int>(codec_result.status)]->Add(1);
    e2e_latency->Set(TimeStamp::DurationToNs(TimeStamp::Now() - req_context->submit_time));
    auto post_latency = req_context->is_compress ? channel->compress_postprocess_latency_
                                                 : channel->decompress_postprocess_latency_;
    post_latency->Set(TimeStamp::DurationToNs(TimeStamp::Now() - start_time));
}

KaeCodecChannel::KaeCodecChannel(const CodecChannelOption& channel_opts,
                                 const device_config_t& device_config)
    : channel_opts_(channel_opts),
      closed_(true),
      device_config_(device_config),
      req_id_(0),
      inflight_num_(0),
      inflight_req_queue_(next_power_of_two(MAX_KAE_CONCURRENCY)) {}

KaeCodecChannel::~KaeCodecChannel() {
    VESAL_CHECK(closed_);
}

Status KaeCodecChannel::Init() {
    compress_sess_ = KAEZIP_create_async_compress_session(VAddr2PAddr, &device_config_);
    if (compress_sess_ == 0) {
        return ChannelError("KAEZIP_create_async_compress_session failed");
    }
    decompress_sess_ = KAEZIP_create_async_decompress_session(VAddr2PAddr, &device_config_);
    if (decompress_sess_ == 0) {
        KAEZIP_destroy_async_compress_session(compress_sess_);
        return ChannelError("KAEZIP_create_async_decompress_session failed");
    }
    req_context_pool_ = std::make_unique<DedicatedPool<ReqContext>>((size_t)MAX_KAE_CONCURRENCY);
    req_context_pool_->ForEach([this](ReqContext* ctx) {
        ctx->src_buffer_list.buf =
            (struct kaezip_buffer*)malloc(VESAL_MAX_SGL_NUM * sizeof(kaezip_buffer));
        ctx->dst_buffer_list.buf = (struct kaezip_buffer*)malloc(1 * sizeof(kaezip_buffer));
        ctx->channel = this;
    });
    Tag tag_service_type = std::make_pair("service_type", "codec");
    Tag tag_engine = std::make_pair("engine", "kae");
    std::string device_tag_value;
    if (device_config_.policy == KAE_SELECT_AUTO) {
        device_tag_value = "auto";
    } else if (device_config_.policy == KAE_SELECT_BY_DEV) {
        device_tag_value = std::to_string(device_config_.param.dev->dev_id);
    } else if (device_config_.policy == KAE_SELECT_BY_NUMA) {
        device_tag_value = std::to_string(device_config_.param.numa_node);
    }
    Tag tag_device = std::make_pair("device", device_tag_value);
    std::vector<Tag> common_tags = {tag_engine, tag_device, tag_service_type};
    std::vector<Tag> tags_with_compress_tag = {
        tag_engine, tag_device, std::make_pair("type", "compress")};
    std::vector<Tag> tags_with_decompress_tag = {
        tag_engine, tag_device, std::make_pair("type", "decompress")};
    compress_throughput_ =
        g_metric_registry->RegisterCounter("vesal.throughput", tags_with_compress_tag);
    decompress_throughput_ =
        g_metric_registry->RegisterCounter("vesal.throughput", tags_with_decompress_tag);
    compress_preprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.preprocess_latency", tags_with_compress_tag);
    decompress_preprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.preprocess_latency", tags_with_decompress_tag);
    compress_postprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.postprocess_latency", tags_with_compress_tag);
    decompress_postprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.postprocess_latency", tags_with_decompress_tag);
    compress_e2e_latency_ =
        g_metric_registry->RegisterHistogram("vesal.e2e_latency", tags_with_compress_tag);
    decompress_e2e_latency_ =
        g_metric_registry->RegisterHistogram("vesal.e2e_latency", tags_with_decompress_tag);
    poll_total_time_ = g_metric_registry->RegisterCounter("vesal.poll_total_time", common_tags);
    poll_busy_time_ = g_metric_registry->RegisterCounter("vesal.poll_busy_time", common_tags);
    poll_interval_ = g_metric_registry->RegisterHistogram("vesal.poll_interval", common_tags);
    compress_submit_latency_ =
        g_metric_registry->RegisterHistogram("vesal.submit_latency", tags_with_compress_tag);
    decompress_submit_latency_ =
        g_metric_registry->RegisterHistogram("vesal.submit_latency", tags_with_decompress_tag);
    for (const auto& code : GetAllStatusCodes()) {
        compress_rps_counters_.push_back(g_metric_registry->RegisterCounter(
            "vesal.rps",
            {tag_engine,
             tag_device,
             tag_service_type,
             std::make_pair("type", "compress"),
             std::make_pair("status", StatusCodeToString(code))}));
        decompress_rps_counters_.push_back(g_metric_registry->RegisterCounter(
            "vesal.rps",
            {tag_engine,
             tag_device,
             tag_service_type,
             std::make_pair("type", "decompress"),
             std::make_pair("status", StatusCodeToString(code))}));
    }
    metric_in_kae_num_ = g_metric_registry->RegisterGauge("vesal.in_kae_num", common_tags);
    metric_max_in_kae_num_ = g_metric_registry->RegisterGauge("vesal.max_in_kae_num", common_tags);
    periodic_task_id_ = g_periodic_scheduler.AddPeriodicTask(
        [&]() {
            metric_in_kae_num_->Set(inflight_num_);
            metric_max_in_kae_num_->Set(MAX_KAE_CONCURRENCY);
        },
        std::chrono::milliseconds(1000));
    closed_ = false;
    return OkStatus();
}

Status KaeCodecChannel::Close() {
    if (closed_) {
        return OkStatus();
    }
    g_periodic_scheduler.CompleteTask(periodic_task_id_);
    KAEZIP_destroy_async_compress_session(compress_sess_);
    KAEZIP_destroy_async_decompress_session(decompress_sess_);
    req_context_pool_->ForEach([](ReqContext* ctx) {
        free(ctx->src_buffer_list.buf);
        free(ctx->dst_buffer_list.buf);
    });
    closed_ = true;
    return OkStatus();
}

StatusCode KaeCodecChannel::CompressAsync(
    unsigned char* src, unsigned int src_len, unsigned char* dst, unsigned int dst_len, void* ctx) {
    return CompressSGLAsync({src}, {src_len}, dst, dst_len, ctx);
}

StatusCode KaeCodecChannel::DecompressAsync(
    unsigned char* src, unsigned int src_len, unsigned char* dst, unsigned int dst_len, void* ctx) {
    return DecompressSGLAsync({src}, {src_len}, dst, dst_len, ctx);
}

StatusCode ArgsCheck(const std::vector<unsigned char*>& src,
                     const std::vector<unsigned int>& src_len,
                     unsigned char* dst,
                     unsigned int dst_len) {
    if (VESAL_UNLIKELY(src.empty() || src_len.empty() || dst == nullptr || dst_len == 0)) {
        return StatusCode::kInvalidArgument;
    }
    if (VESAL_UNLIKELY(src.size() != src_len.size())) {
        return StatusCode::kInvalidArgument;
    }
    // Kae has 255 SGL and 8MB size in total limit
    if (VESAL_UNLIKELY(src.size() > 255) ||
        std::accumulate(src_len.begin(), src_len.end(), 0) > (8 << 20)) {
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

void KaeCodecChannel::PrepareCtx(ReqContext* req_context,
                                 const std::vector<unsigned char*>& src,
                                 const std::vector<unsigned int>& src_len,
                                 unsigned char* dst,
                                 unsigned int dst_len,
                                 void* ctx) {
    req_context->usr_ctx = ctx;
    struct kaezip_buffer_list* buffer_list = &req_context->src_buffer_list;
    req_context->req_id = req_id_;
    for (size_t i = 0; i < src.size(); i++) {
        buffer_list->buf[i].data = src[i];
        buffer_list->buf[i].buf_len = src_len[i];
    }
    buffer_list->buf_num = src.size();
    struct kaezip_buffer_list* dst_buffer_list = &req_context->dst_buffer_list;
    dst_buffer_list->buf[0].data = dst;
    dst_buffer_list->buf[0].buf_len = dst_len;
    dst_buffer_list->buf_num = 1;
    req_context->result.user_data = (void*)req_context;
}

StatusCode KaeCodecChannel::CompressSGLAsync(const std::vector<unsigned char*>& src,
                                             const std::vector<unsigned int>& src_len,
                                             unsigned char* dst,
                                             unsigned int dst_len,
                                             void* ctx) {
    auto begin_time = TimeStamp::Now();
    auto ret = ArgsCheck(src, src_len, dst, dst_len);
    if (ret != StatusCode::kOk) {
        return ret;
    }
    if (inflight_num_ == MAX_KAE_CONCURRENCY) {
        return StatusCode::kResourceBusy;
    }
    bool do_measure = IsEnableSampling();
    DURATION_TO_RETURN(do_measure, compress_preprocess_latency_, begin_time);
    auto* req_context = req_context_pool_->Get(req_id_);
    PrepareCtx(req_context, src, src_len, dst, dst_len, ctx);
    req_context->is_compress = true;
    req_context->submit_time = begin_time;
    struct kaezip_buffer_list* buffer_list = &req_context->src_buffer_list;
    struct kaezip_buffer_list* dst_buffer_list = &req_context->dst_buffer_list;
    auto submit_begin_time = do_measure ? TimeStamp::Now() : 0;
    int r = KAEZIP_compress_async_in_session(
        compress_sess_, buffer_list, dst_buffer_list, KaeCallback, &req_context->result);
    DoMeasureIfNeed(do_measure, compress_submit_latency_.get(), submit_begin_time);
    if (r != 0) {
        VESAL_LOG(ERROR) << "KAEZIP_compress_async_in_session failed, r = " << r;
        return KaeStatusToStatusCode(r);
    }
    req_id_++;
    inflight_num_++;
    req_context->req_ring_id = inflight_req_queue_.NewReq();
    return StatusCode::kOk;
}

StatusCode KaeCodecChannel::DecompressSGLAsync(const std::vector<unsigned char*>& src,
                                               const std::vector<unsigned int>& src_len,
                                               unsigned char* dst,
                                               unsigned int dst_len,
                                               void* ctx) {
    auto begin_time = TimeStamp::Now();
    auto ret = ArgsCheck(src, src_len, dst, dst_len);
    if (ret != StatusCode::kOk) {
        return ret;
    }
    if (inflight_num_ == MAX_KAE_CONCURRENCY) {
        return StatusCode::kResourceBusy;
    }
    bool do_measure = IsEnableSampling();
    DURATION_TO_RETURN(do_measure, decompress_preprocess_latency_, begin_time);
    auto* req_context = req_context_pool_->Get(req_id_);
    PrepareCtx(req_context, src, src_len, dst, dst_len, ctx);
    req_context->is_compress = false;
    req_context->submit_time = begin_time;
    struct kaezip_buffer_list* buffer_list = &req_context->src_buffer_list;
    struct kaezip_buffer_list* dst_buffer_list = &req_context->dst_buffer_list;
    auto submit_begin_time = do_measure ? TimeStamp::Now() : 0;
    int r = KAEZIP_decompress_async_in_session(
        decompress_sess_, buffer_list, dst_buffer_list, KaeCallback, &req_context->result);
    DoMeasureIfNeed(do_measure, decompress_submit_latency_.get(), submit_begin_time);
    if (r != 0) {
        VESAL_LOG(ERROR) << "KAEZIP_decompress_async_in_session failed, r = " << r;
        return KaeStatusToStatusCode(r);
    }
    req_id_++;
    inflight_num_++;
    req_context->req_ring_id = inflight_req_queue_.NewReq();
    return StatusCode::kOk;
}

ssize_t KaeCodecChannel::Poll(CodecResult results[], unsigned int max_num, int timeout) {
    // TODO(Pinnong.Li): Add timeout handling
    uint64_t start_time = 0;
    if (!FLAGS_vesal_metrics_disable_poller_metrics) {
        start_time = TimeStamp::Now();
        // Skip first poll
        if (VESAL_LIKELY(last_poll_return_time_ != 0)) {
            poll_interval_->Set(TimeStamp::DurationToNs(start_time - last_poll_return_time_));
        }
    }
    auto latency_guard = defer([&start_time, &max_num, this]() {
        if (!FLAGS_vesal_metrics_disable_poller_metrics) {
            auto duration = TimeStamp::DurationToNs(TimeStamp::Now() - start_time);
            if (max_num != 0U) {
                poll_busy_time_->Add(duration);
            }
            poll_total_time_->Add(duration);
            last_poll_return_time_ = TimeStamp::Now();
        }
    });
    KAEZIP_async_polling_in_session(compress_sess_, inflight_num_);
    KAEZIP_async_polling_in_session(decompress_sess_, inflight_num_);
    max_num = inflight_req_queue_.PopResults(results, max_num);
    auto user_cb_guard = defer([this, &max_num, &results] {
        if (channel_opts_.user_cb && max_num) {
            auto ts = TimeStamp::Now();
            for (size_t i = 0; i < max_num; i++) {
                channel_opts_.user_cb(results[i]);
            }
            user_cb_time_->Set(TimeStamp::DurationToNs((TimeStamp::Now() - ts)));
        }
    });
    return max_num;
}

CodecResult KaeCodecChannel::Compress(unsigned char* src,
                                      unsigned int src_len,
                                      unsigned char* dst,
                                      unsigned int dst_len) {
    return CompressSGL({src}, {src_len}, dst, dst_len);
}

CodecResult KaeCodecChannel::Decompress(unsigned char* src,
                                        unsigned int src_len,
                                        unsigned char* dst,
                                        unsigned int dst_len) {
    return DecompressSGL({src}, {src_len}, dst, dst_len);
}

CodecResult KaeCodecChannel::CompressSGL(const std::vector<unsigned char*>& src,
                                         const std::vector<unsigned int>& src_len,
                                         unsigned char* dst,
                                         unsigned int dst_len) {
    CodecResult res = {};
    auto submit_r = CompressSGLAsync(src, src_len, dst, dst_len, nullptr);
    if (VESAL_UNLIKELY(submit_r != StatusCode::kOk)) {
        res.status = submit_r;
        return res;
    }
    while (Poll(&res, 1, -1) == 0)
        ;
    return res;
}

CodecResult KaeCodecChannel::DecompressSGL(const std::vector<unsigned char*>& src,
                                           const std::vector<unsigned int>& src_len,
                                           unsigned char* dst,
                                           unsigned int dst_len) {
    CodecResult res = {};
    auto submit_r = DecompressSGLAsync(src, src_len, dst, dst_len, nullptr);
    if (VESAL_UNLIKELY(submit_r != StatusCode::kOk)) {
        res.status = submit_r;
        return res;
    }
    while (Poll(&res, 1, -1) == 0)
        ;
    return res;
}

}  // namespace kae
}  // namespace vesal
