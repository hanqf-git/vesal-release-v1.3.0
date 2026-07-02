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

#include "qat_codec_dedicated_channel.h"

#include "common/metrics_internal.h"

namespace vesal {
namespace qat {

Status QatCodecDedicatedChannel::Init() {
    Status status = qat_codec_engine_->Init();
    if (!status.ok()) {
        return status;
    }
    Tag tag_service_type = std::make_pair("service_type", "codec");
    Tag tag_engine = std::make_pair("engine", "qat");
    Tag tag_device = std::make_pair(
        "device", std::to_string(qat_codec_engine_->GetQatUnit()->GetQatUnitAttr().device_id));
    Tag tag_instance =
        std::make_pair("instance", qat_codec_engine_->GetQatUnit()->GetQatUnitAttr().instance_id);
    std::vector<Tag> common_tags = {tag_engine, tag_device, tag_instance, tag_service_type};
    metric_user_cb_time_ = g_metric_registry->RegisterHistogram("vesal.user_cb_time", common_tags);
    return status;
}

StatusCode QatCodecDedicatedChannel::CompressAsync(
    unsigned char* src, unsigned int src_len, unsigned char* dst, unsigned int dst_len, void* ctx) {
    return CompressSGLAsync({src}, {src_len}, dst, dst_len, ctx);
}
StatusCode QatCodecDedicatedChannel::CompressSGLAsync(const std::vector<unsigned char*>& src,
                                                      const std::vector<unsigned int>& src_len,
                                                      unsigned char* dst,
                                                      unsigned int dst_len,
                                                      void* ctx) {
    if (VESAL_UNLIKELY(!Prepare(src, src_len, dst, dst_len))) {
        return StatusCode::kInvalidArgument;
    }
    return qat_codec_engine_->SubmitAsyncRequest(
        CodecDirection::kComp, src, src_len, dst, dst_len, ctx);
}

CodecResult QatCodecDedicatedChannel::Compress(unsigned char* src,
                                               unsigned int src_len,
                                               unsigned char* dst,
                                               unsigned int dst_len) {
    return CompressSGL({src}, {src_len}, dst, dst_len);
}

CodecResult QatCodecDedicatedChannel::CompressSGL(const std::vector<unsigned char*>& src,
                                                  const std::vector<unsigned int>& src_len,
                                                  unsigned char* dst,
                                                  unsigned int dst_len) {
    CodecResult res = {};
    if (VESAL_UNLIKELY(!Prepare(src, src_len, dst, dst_len))) {
        res.status = StatusCode::kInvalidArgument;
        return res;
    }
    auto submit_r = qat_codec_engine_->SubmitAsyncRequest(
        CodecDirection::kComp, src, src_len, dst, dst_len, nullptr);
    if (VESAL_UNLIKELY(!IsOk(submit_r))) {
        res.status = submit_r;
        return res;
    }
    auto n = Poll(&res, 1, -1);
    while (n == 0) {
        n = Poll(&res, 1, 0);
    }
    return res;
}
StatusCode QatCodecDedicatedChannel::DecompressAsync(
    unsigned char* src, unsigned int src_len, unsigned char* dst, unsigned int dst_len, void* ctx) {
    return DecompressSGLAsync({src}, {src_len}, dst, dst_len, ctx);
}
StatusCode QatCodecDedicatedChannel::DecompressSGLAsync(const std::vector<unsigned char*>& src,
                                                        const std::vector<unsigned int>& src_len,
                                                        unsigned char* dst,
                                                        unsigned int dst_len,
                                                        void* ctx) {
    if (VESAL_UNLIKELY(!Prepare(src, src_len, dst, dst_len))) {
        return StatusCode::kInvalidArgument;
    }
    return qat_codec_engine_->SubmitAsyncRequest(
        CodecDirection::kDecomp, src, src_len, dst, dst_len, ctx);
}
CodecResult QatCodecDedicatedChannel::Decompress(unsigned char* src,
                                                 unsigned int src_len,
                                                 unsigned char* dst,
                                                 unsigned int dst_len) {
    return DecompressSGL({src}, {src_len}, dst, dst_len);
}
CodecResult QatCodecDedicatedChannel::DecompressSGL(const std::vector<unsigned char*>& src,
                                                    const std::vector<unsigned int>& src_len,
                                                    unsigned char* dst,
                                                    unsigned int dst_len) {
    CodecResult res = {};
    if (VESAL_UNLIKELY(!Prepare(src, src_len, dst, dst_len))) {
        res.status = StatusCode::kInvalidArgument;
        return res;
    }
    auto submit_r = qat_codec_engine_->SubmitAsyncRequest(
        CodecDirection::kDecomp, src, src_len, dst, dst_len, nullptr);
    if (VESAL_UNLIKELY(!IsOk(submit_r))) {
        res.status = submit_r;
        return res;
    }
    auto n = Poll(&res, 1, -1);
    while (n == 0) {
        n = Poll(&res, 1, 0);
    }
    return res;
}

CodecResult QatCodecDedicatedChannel::Submit(const CodecRequestArgs& args) {
    CodecResult res = {};
    res.status = SubmitAsync(args);
    if (VESAL_UNLIKELY(!IsOk(res.status))) {
        return res;
    }
    auto n = Poll(&res, 1, -1);
    while (n == 0) {
        n = Poll(&res, 1, 0);
    }
    return res;
}

StatusCode QatCodecDedicatedChannel::SubmitAsync(const CodecRequestArgs& args) {
    return qat_codec_engine_->SubmitAsyncRequest(args);
}

ssize_t QatCodecDedicatedChannel::Poll(CodecResult results[], unsigned int max_num, int timeout) {
    ssize_t n = qat_codec_engine_->Poll(results, max_num, timeout);
    if (n > 0 && channel_opts_.user_cb) {
        auto begin = TimeStamp::Now();
        for (ssize_t i = 0; i < n; ++i) {
            channel_opts_.user_cb(results[i]);
        }
        metric_user_cb_time_->Set(TimeStamp::DurationToNs(TimeStamp::Now() - begin));
    }
    return n;
}

Status QatCodecDedicatedChannel::Close() {
    return qat_codec_engine_->Close();
}

}  // namespace qat
}  // namespace vesal
