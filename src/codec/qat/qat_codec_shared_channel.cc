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

#include "codec/qat/qat_codec_shared_channel.h"

#include "codec/codec_common.h"
#include "codec/qat/qat_poller_manager.h"
#include "common/metrics_internal.h"

namespace vesal {
namespace qat {

void SyncApiCallback(const CodecResult& res) {
    auto* codec_result = static_cast<CodecResult*>(res.ctx);
    auto* prom = static_cast<std::promise<void>*>(codec_result->ctx);
    *codec_result = res;
    codec_result->ctx = nullptr;
    prom->set_value();
}

Status QatCodecSharedChannel::Init() {
    auto* qat_poller_manager = QatPollerManager::GetInstance();
    if (qat_poller_manager == nullptr) {
        return {StatusCode::kUnknown, "QatPollerManager is not initialized"};
    }
    worker_ = qat_poller_manager->GetWorker(channel_opts_);
    if (worker_ == nullptr) {
        return {StatusCode::kResourceBusy, "Failed to get QatPollerWorker"};
    }

    Tag tag_engine = std::make_pair("engine", "qat");
    Tag tag_comp = std::make_pair("type", "compress");
    Tag tag_decomp = std::make_pair("type", "decompress");
    Tag tag_poller_id = std::make_pair("poller_id", std::to_string(worker_->GetPollerId()));
    Tag tag_shared_channel_ptr =
        std::make_pair("shared_channel_ptr", std::to_string(reinterpret_cast<uintptr_t>(this)));
    Tag tag_device_id = std::make_pair("device_id", std::to_string(worker_->GetEngineDeviceId()));

    TagList comp_list = {
        tag_engine, tag_comp, tag_poller_id, tag_shared_channel_ptr, tag_device_id};
    TagList decomp_list = {
        tag_engine, tag_decomp, tag_poller_id, tag_shared_channel_ptr, tag_device_id};

    shared_compress_queue_latency_ =
        g_metric_registry->RegisterHistogram("vesal.shared_queue_latency", comp_list);
    shared_decompress_queue_latency_ =
        g_metric_registry->RegisterHistogram("vesal.shared_queue_latency", decomp_list);
    shared_compress_e2e_latency_ =
        g_metric_registry->RegisterHistogram("vesal.shared_e2e_latency", comp_list);
    shared_decompress_e2e_latency_ =
        g_metric_registry->RegisterHistogram("vesal.shared_e2e_latency", decomp_list);

    return OkStatus();
}

StatusCode QatCodecSharedChannel::CompressAsync(
    unsigned char* src, unsigned int src_len, unsigned char* dst, unsigned int dst_len, void* ctx) {
    return CompressSGLAsync({src}, {src_len}, dst, dst_len, ctx);
}

StatusCode QatCodecSharedChannel::CompressSGLAsync(const std::vector<unsigned char*>& src,
                                                   const std::vector<unsigned int>& src_len,
                                                   unsigned char* dst,
                                                   unsigned int dst_len,
                                                   void* ctx) {
    VESAL_DCHECK(worker_ != nullptr);
    if (VESAL_UNLIKELY(!Prepare(src, src_len, dst, dst_len, ctx, CodecDirection::kComp))) {
        return StatusCode::kInvalidArgument;
    }
    return SubmitInternal(
        src, src_len, dst, dst_len, ctx, CodecDirection::kComp, channel_opts_.user_cb);
}

CodecResult QatCodecSharedChannel::Compress(unsigned char* src,
                                            unsigned int src_len,
                                            unsigned char* dst,
                                            unsigned int dst_len) {
    return CompressSGL({src}, {src_len}, dst, dst_len);
}

CodecResult QatCodecSharedChannel::CompressSGL(const std::vector<unsigned char*>& src,
                                               const std::vector<unsigned int>& src_len,
                                               unsigned char* dst,
                                               unsigned int dst_len) {
    VESAL_DCHECK(worker_ != nullptr);
    CodecResult result;
    std::promise<void> prom;
    result.ctx = &prom;
    std::future<void> fut = prom.get_future();
    if (VESAL_UNLIKELY(!Prepare(src, src_len, dst, dst_len, &result, CodecDirection::kComp))) {
        result.status = StatusCode::kInvalidArgument;
        return result;
    }
    StatusCode status =
        SubmitInternal(src, src_len, dst, dst_len, &result, CodecDirection::kComp, SyncApiCallback);
    if (status != StatusCode::kOk) {
        result.status = status;
        return result;
    }
    fut.get();
    return result;
}

StatusCode QatCodecSharedChannel::DecompressAsync(
    unsigned char* src, unsigned int src_len, unsigned char* dst, unsigned int dst_len, void* ctx) {
    return DecompressSGLAsync({src}, {src_len}, dst, dst_len, ctx);
}

StatusCode QatCodecSharedChannel::DecompressSGLAsync(const std::vector<unsigned char*>& src,
                                                     const std::vector<unsigned int>& src_len,
                                                     unsigned char* dst,
                                                     unsigned int dst_len,
                                                     void* ctx) {
    VESAL_DCHECK(worker_ != nullptr);
    if (VESAL_UNLIKELY(!Prepare(src, src_len, dst, dst_len, ctx, CodecDirection::kDecomp))) {
        return StatusCode::kInvalidArgument;
    }
    return SubmitInternal(
        src, src_len, dst, dst_len, ctx, CodecDirection::kDecomp, channel_opts_.user_cb);
}

CodecResult QatCodecSharedChannel::Decompress(unsigned char* src,
                                              unsigned int src_len,
                                              unsigned char* dst,
                                              unsigned int dst_len) {
    return DecompressSGL({src}, {src_len}, dst, dst_len);
}

CodecResult QatCodecSharedChannel::DecompressSGL(const std::vector<unsigned char*>& src,
                                                 const std::vector<unsigned int>& src_len,
                                                 unsigned char* dst,
                                                 unsigned int dst_len) {
    VESAL_DCHECK(worker_ != nullptr);
    CodecResult result;
    std::promise<void> prom;
    result.ctx = &prom;
    std::future<void> fut = prom.get_future();
    if (VESAL_UNLIKELY(!Prepare(src, src_len, dst, dst_len, &result, CodecDirection::kDecomp))) {
        result.status = StatusCode::kInvalidArgument;
        return result;
    }
    StatusCode status = SubmitInternal(
        src, src_len, dst, dst_len, &result, CodecDirection::kDecomp, SyncApiCallback);
    if (status != StatusCode::kOk) {
        result.status = status;
        return result;
    }
    fut.get();
    return result;
}

Status QatCodecSharedChannel::Close() {
    if (worker_ != nullptr) {
        auto* qat_poller_manager = QatPollerManager::GetInstance();
        if (qat_poller_manager != nullptr) {
            qat_poller_manager->ReturnWorker(worker_);
        }
        worker_ = nullptr;
    }
    return OkStatus();
}

bool QatCodecSharedChannel::Prepare(const std::vector<unsigned char*>& src,
                                    const std::vector<unsigned int>& src_len,
                                    unsigned char* dst,
                                    unsigned int dst_len,
                                    void* ctx,
                                    CodecDirection direction) {
    return IsOk(ValidateSglArgs(src, src_len));
}

StatusCode QatCodecSharedChannel::SubmitInternal(const std::vector<unsigned char*>& src,
                                                 const std::vector<unsigned int>& src_len,
                                                 unsigned char* dst,
                                                 unsigned int dst_len,
                                                 void* ctx,
                                                 CodecDirection direction,
                                                 UserCallback user_cb) {
    auto start_ts = TimeStamp::Now();
    return worker_->Schedule(src, src_len, dst, dst_len, ctx, direction, user_cb, this, start_ts);
}

}  // namespace qat
}  // namespace vesal
