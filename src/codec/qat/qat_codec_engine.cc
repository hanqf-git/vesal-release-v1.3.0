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

#include "qat_codec_engine.h"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "codec/codec_common.h"
#include "codec/dc_format.h"
#include "codec/qat/qat_error_handling.h"
#include "codec/qat/qat_handle.h"
#include "common/defer.h"
#include "common/qat/qat_session.h"
#include "common/qat/qat_util.h"
#include "vesal/io_vector.h"
#ifdef VESAL_ENABLE_ERR_SIM
#include "common/err_simulation.h"
#endif
#include "common/metrics_internal.h"
#include "common/object_pool.h"
#include "common/scheduler.h"
#include "common/timestamp.h"
#include "qat_codec.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"
#include "vesal/types.h"
#include "vesal/vesal.h"

extern "C" {
#include <cpa_dc.h>
#include <icp_sal_poll.h>
}

namespace vesal {
namespace qat {

std::ostream& operator<<(std::ostream& os, const RequestMeta& req_meta) {
    return os << "req_id=" << req_meta.req_id;
}

QatCodecEngine::QatCodecEngine(const CodecChannelOption& channel_opts,
                               QatUnitManager* unit_manager,
                               size_t max_in_qat_size)
    : unit_manager_(unit_manager),
      channel_opts_(channel_opts),
      next_req_id_(1),
      next_recv_req_id_(1),
      in_qat_num_(0),
      callback_called_times_(0),
      closed_(true),
      in_flight_resource_(std::make_unique<InFlightResource>(next_power_of_two(max_in_qat_size))),
      max_in_qat_size_(max_in_qat_size),
      is_fatal_(false) {}

QatCodecEngine::~QatCodecEngine() {
    VESAL_CHECK(closed_);
}

Status QatCodecEngine::Init() {
    ha_ = std::make_unique<QatHa>(channel_opts_.ha_policy);
    qat_handle_ = std::make_unique<QatHandle>(channel_opts_, unit_manager_);
    Status st = qat_handle_->Init();
    if (!st.ok()) {
        return st;
    }
    closed_ = false;
    free_mem_timer_manager_ = std::make_unique<TimerManager>(
        [](void* timer_arg) {
            auto* bufs = static_cast<std::pair<IOVector, IOBlock>*>(timer_arg);
            IOVector* src = &bufs->first;
            IOBlock* dst = &bufs->second;
            if (dst->deleter) {
                dst->deleter(dst->ctx);
            }
            for (uint32_t i = 0; i < src->num_blocks; ++i) {
                if (src->blocks[i].deleter) {
                    src->blocks[i].deleter(src->blocks[i].ctx);
                }
            }
            delete bufs;
        },
        TimeStamp::MsToDuration(max_in_qat_size_ * g_qat_timeout_ms));
    timer_manager_ = std::make_unique<TimerManager>(
        [this](void* timer_arg) {
            auto* req_meta = static_cast<RequestMeta*>(timer_arg);
            InterceptInflightReq(req_meta, StatusCode::kTimeout);
            free_mem_timer_manager_->AddTimer(
                new std::pair<IOVector, IOBlock>(req_meta->src, req_meta->dst));
        },
        TimeStamp::MsToDuration(channel_opts_.timeout_ms));
    codec_results_ = std::make_unique<CodecResultList>();
    buf_cache_ = std::make_unique<QatBufCache>(qat_handle_->GetUnit(), max_in_qat_size_);
    VESAL_CHECK(buf_cache_->Init());

    // Init request_cb_context_pool_
    in_flight_resource_->request_cb_context_pool_->ForEach(
        [this](RequestCbContext* ptr) { ptr->InitCrcControlData(channel_opts_.checksum_type); });

    Tag tag_service_type = std::make_pair("service_type", "codec");
    Tag tag_engine = std::make_pair("engine", "qat");
    Tag tag_instance =
        std::make_pair("instance", qat_handle_->GetUnit()->GetQatUnitAttr().instance_id);
    Tag tag_device = std::make_pair(
        "device", std::to_string(qat_handle_->GetUnit()->GetQatUnitAttr().device_id));
    std::vector<Tag> common_tags = {tag_engine, tag_device, tag_instance, tag_service_type};
    std::vector<Tag> tags_with_compress_tag = {
        tag_engine, tag_device, tag_instance, std::make_pair("type", "compress")};
    std::vector<Tag> tags_with_decompress_tag = {
        tag_engine, tag_device, tag_instance, std::make_pair("type", "decompress")};
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
             tag_instance,
             tag_service_type,
             std::make_pair("type", "compress"),
             std::make_pair("status", StatusCodeToString(code))}));
        decompress_rps_counters_.push_back(g_metric_registry->RegisterCounter(
            "vesal.rps",
            {tag_engine,
             tag_device,
             tag_instance,
             tag_service_type,
             std::make_pair("type", "decompress"),
             std::make_pair("status", StatusCodeToString(code))}));
    }
    metric_in_qat_num_ = g_metric_registry->RegisterGauge("vesal.in_qat_num", common_tags);
    metric_max_in_qat_num_ = g_metric_registry->RegisterGauge("vesal.max_in_qat_num", common_tags);
    periodic_task_id_ = g_periodic_scheduler.AddPeriodicTask(
        [&]() {
            metric_in_qat_num_->Set(in_qat_num_);
            metric_max_in_qat_num_->Set(max_in_qat_size_);
        },
        std::chrono::milliseconds(1000));

    return OkStatus();
}

ssize_t QatCodecEngine::Poll(CodecResult results[], unsigned int max_num, int timeout) {
    uint64_t start_time = 0;
    if (!FLAGS_vesal_metrics_disable_poller_metrics) {
        start_time = TimeStamp::Now();
        // Skip first poll
        if (VESAL_LIKELY(last_poll_return_time_)) {
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

    // Channel is in fatal state, we should try to drain out the buffered results, Otherwise the
    // user's requests are hanging. But should not touch hardware anymore.
    if (VESAL_UNLIKELY(is_fatal_)) {
        max_num = PopBufferedResults(results, max_num);
        if (max_num > 0) {
            return max_num;
        }
        VESAL_LOG(ERROR) << "Channel completely failed and no buffered results.";
        return -1;
    }
    // reset callback times
    callback_called_times_ = 0;
    // Poll first here, in case the user calls Poll() too slow and we mis-judge the requests as
    // timeout. Try Poll() first, and if there the inflight requests are still hanging and timeout,
    // then timeout.
    StatusCode poll_r = qat_handle_->PollInstance();
    // handle timeout in head.
    timer_manager_->HandleTimeout();
    free_mem_timer_manager_->HandleTimeout();
    // update the in qat req number
    in_qat_num_ -= callback_called_times_;

    // Pop codec results from `codec_results_' queue to `results'.
    max_num = PopBufferedResults(results, max_num);

    // Fast path if HA not enabled.
    if (!IsHaEnabled()) {
        if (VESAL_UNLIKELY(max_num == 0 && !IsShouldRetry(poll_r) && !IsOk(poll_r))) {
            // Serious issues happened, set fatal and return -1.
            VESAL_LOG(ERROR) << "Poll status: " << poll_r << ", engine dead.";
            is_fatal_ = true;
            return -1;
        }
        return max_num;
    }

    // Path with HA enabled. Things are complicated so we split into very detailed cases here:
    // 1. Poll failed and HA triggered OK: Drop all buffered results and inflight requests.
    // Return the popped results.
    // 2. Poll failed and HA triggered failed: Drop all buffered results and inflight requests.
    // Return the popped result number. In this case the engine is in fatal state, user will get -1
    // eventually after they Poll() and drains out the buffered results.
    // 3. Poll failed and HA not triggered: Actually this is like Poll OK. Just continue to see the
    // popped results.
    // 4. Poll OK and no results: Return 0.
    // 5. Poll OK and results all good: Return the popped results.
    // 6. Poll OK and results have failed and HA not triggered: Return the popped results.
    // 7. Poll OK and results have failed and HA triggered OK: Drop all buffered results and
    // inflight requests. Return the popped results.
    // 8. Poll OK and results have failed and HA triggered failed: Drop all buffered results and
    // inflight requests. Return the popped results.
    // The key is, popped results should not be modified. If the HA happened, the requests related
    // to the last QAT should be all dropped, including inflight requests and buffered results. By
    // so, even the HA triggered at Poll stage, the buffered are all dropped so they would not
    // triggered an immediate HA again. And the cases can be combined into very easy cases.
    if (VESAL_UNLIKELY(!IsOk(poll_r) && !IsShouldRetry(poll_r))) {
        // Poll failed. HandleHa will drop all inflight requests and buffered results and try HA.
        // Succeeded or not, return the popped results anyway.
        HandleHa(poll_r);
        return max_num;
    }
    // Poll OK.
    for (size_t i = 0; i < max_num; ++i) {
        if (VESAL_UNLIKELY(!IsOk(results[i].status))) {
            if (HandleHa(results[i].status) || is_fatal_) {
                // HA triggered, just break and return the results.
                break;
            }
        }
    }
    return max_num;
}

Status QatCodecEngine::Close() {
    if (closed_) {
        return OkStatus();
    }
    g_periodic_scheduler.CompleteTask(periodic_task_id_);

#ifdef VESAL_ENABLE_QAT_DUMP
    static std::atomic<bool> dump_all_rings_called{false};
    bool expected = false;
    if (dump_all_rings_called.compare_exchange_strong(expected, true)) {
        StatusCode dump_status = qat_handle_->DumpAllRings();
        if (!IsOk(dump_status)) {
            VESAL_LOG(WARN) << "dcDumpAllRings failed, status=" << dump_status;
        }
    }
#endif

    auto r = qat_handle_->Uninit();
    if (!r.ok()) {
        VESAL_LOG(ERROR) << "Fail to uninit QAT handle, r=" << r << ", in_qat_num_=" << in_qat_num_;
        return r;
    }
    // ready to be closed
    timer_manager_.reset();
    free_mem_timer_manager_.reset();
    codec_results_.reset();
    for (auto* cb_ctx : discarded_cb_ctxs_) {
        cb_ctx->Reset();
        delete cb_ctx;
    }
    discarded_cb_ctxs_.clear();
    // need to release here as we need to access Unit state inside InFlightResource
    in_flight_resource_.reset();
    buf_cache_.reset();
    closed_ = true;

    return OkStatus();
}

StatusCode QatCodecEngine::DumpDebugInfo() {
    return qat_handle_->DumpDebugInfo();
}

StatusCode QatCodecEngine::SubmitAsyncRequest(const CodecDirection& dir,
                                              const std::vector<unsigned char*>& src,
                                              const std::vector<unsigned int>& src_len,
                                              unsigned char* dst,
                                              unsigned int dst_len,
                                              void* ctx) {
    CodecRequestArgs args;
    IOBlock src_block[VESAL_MAX_SGL_NUM];
    args.src.blocks = src_block;
    args.src.num_blocks = src.size();
    for (size_t i = 0; i < src.size(); ++i) {
        src_block[i].data = src[i];
        src_block[i].size = src_len[i];
        src_block[i].deleter = nullptr;
    }
    args.direction = dir;
    args.ctx = ctx;
    args.dst.data = dst;
    args.dst.size = dst_len;
    args.session_opt.codec_algorithm = channel_opts_.comp_algorithm;
    args.session_opt.comp_level = channel_opts_.comp_level;
    return SubmitAsyncRequest(args);
}

StatusCode QatCodecEngine::SubmitAsyncRequest(const CodecRequestArgs& args) {
    auto begin_time = TimeStamp::Now();
    auto dir = args.direction;
    bool do_measure = IsEnableSampling();
    DURATION_TO_RETURN(do_measure,
                       dir == CodecDirection::kComp ? compress_preprocess_latency_.get()
                                                    : decompress_preprocess_latency_.get(),
                       begin_time);

    auto algo = args.session_opt.codec_algorithm;
    auto comp_level = args.session_opt.comp_level;
    auto algo_u8 = static_cast<uint8_t>(algo);
    auto level_u8 = static_cast<uint8_t>(comp_level);
    if (VESAL_UNLIKELY(algo_u8 == 0 || algo_u8 >= static_cast<uint8_t>(CodecAlgorithm::kNum) ||
                       level_u8 == 0 || level_u8 >= static_cast<uint8_t>(CodecCompLevel::kNum))) {
        return StatusCode::kInvalidArgument;
    }
    if (VESAL_UNLIKELY(algo != CodecAlgorithm::kLz4 && algo != CodecAlgorithm::kDeflate &&
                       algo != CodecAlgorithm::kZlib)) {
        return StatusCode::kInvalidArgument;
    }
    StatusCode r = StatusCode::kOk;
    RequestMeta* req_meta = nullptr;
    RequestCbContext* cb_ctx = nullptr;
    // capture `r`, will do the cleanup if r is not ok upon return
    auto clean_up_guard = defer([dir, &r, &req_meta, &cb_ctx, this]() {
        if (VESAL_UNLIKELY(!IsOk(r))) {
            Counter* counter = dir == CodecDirection::kComp
                                   ? compress_rps_counters_[static_cast<int>(r)].get()
                                   : decompress_rps_counters_[static_cast<int>(r)].get();
            counter->Add(1);
            CleanUpReqData(req_meta, cb_ctx);
        }
    });

    if (VESAL_UNLIKELY(is_fatal_)) {
        VESAL_LOG(ERROR) << "Channel completely failed";
        return StatusCode::kPermanentError;
    }

    if (VESAL_UNLIKELY(QatRingFull())) {
        VESAL_LOG(WARN) << "Request overcrowded, retry after polling. in_qat_num_=" << in_qat_num_
                        << ", max_in_qat_size_=" << max_in_qat_size_ << ", Direction=" << dir
                        << ", req_id=" << GetNextReqId();
        r = StatusCode::kResourceBusy;
        return r;
    }
    r = PrepareCommonReqData(args, &req_meta, &cb_ctx);
    if (VESAL_UNLIKELY(!IsOk(r))) {
        // Currently the preparation error is only caused from software issue, mainly due to the DMA
        // memory OOM. We don't trigger HA in such case as it's not a hardware issue.
        return r;
    }
#ifdef VESAL_ENABLE_ERR_SIM
    req_meta->err_sim_code = VESAL_QAT_ERR_SIM_OK;
    req_meta->err_sim_onfire_ts = 0;
    if (FLAGS_vesal_enable_err_sim) {
        auto err = qat_handle_->GetUnit()->GetQatErrSim(QatErrSimType::kResult);
        req_meta->err_sim_code = err.first;
        if (VESAL_QAT_ERR_SIM_IS_TIMEOUT(err.first)) {
            req_meta->err_sim_onfire_ts = err.second;
        }
    }
#endif

    // submit async qat request
    uint64_t submit_begin_time = do_measure ? TimeStamp::Now() : 0;
    VESAL_LOG(DEBUG) << "SubmitAsyncRequest to qat, req_id=" << cb_ctx->req_id
                     << ", Direction=" << dir << ", req_id=" << GetNextReqId();
    r = qat_handle_->SubmitAsync(cb_ctx, dir, args.session_opt);
    DoMeasureIfNeed(do_measure,
                    dir == CodecDirection::kComp ? compress_submit_latency_.get()
                                                 : decompress_submit_latency_.get(),
                    submit_begin_time);
    if (VESAL_UNLIKELY(!IsOk(r))) {
        VESAL_LOG(WARN) << "SubmitAsyncRequest request failed=" << r << ", Direction=" << dir
                        << ", req_id=" << GetNextReqId();
        if (IsHaEnabled()) {
            HandleHa(r);
        } else if (IsChannelError(r) || IsHardwareError(r)) {
            is_fatal_ = true;
        }
        return r;
    }
    ++in_qat_num_;
    IncreaseNextReqId();
    TagInFlight(begin_time, req_meta);
    return r;
}

StatusCode QatCodecEngine::PrepareCommonReqData(const CodecRequestArgs& args,
                                                RequestMeta** req_meta,
                                                RequestCbContext** cb_ctx) {
    // init request meta & register timer
    req_id_t req_id = GetNextReqId();
    *req_meta = in_flight_resource_->request_meta_pool_->Get(req_id);
    (*req_meta)->req_id = req_id;
    // Store the req_meta to timer
    (*req_meta)->user_ctx = args.ctx;
    (*req_meta)->direction = args.direction;
    // copy blocks address in case user released the args.src
    for (uint32_t i = 0; i < args.src.num_blocks; ++i) {
        (*req_meta)->src.blocks[i] = args.src.blocks[i];
    }
    (*req_meta)->src.num_blocks = args.src.num_blocks;
    (*req_meta)->dst = args.dst;

    // init buffers
    QatBuf* src_buf = buf_cache_->GetOne();
    QatBuf* dst_buf = buf_cache_->GetOne();
    if (VESAL_UNLIKELY(!src_buf || !dst_buf)) {
        VESAL_LOG(ERROR) << "Failed to allocate QatBuf, req_meta: " << **req_meta;
        buf_cache_->ReturnOne(src_buf);
        buf_cache_->ReturnOne(dst_buf);
        return StatusCode::kResourceBusy;
    }

    bool fill_src_r = false;
    bool fill_dst_r = false;
    int header_size = 0;
    int footer_size = 0;
    CodecAlgorithm algo = args.session_opt.codec_algorithm;
    CodecCompLevel comp_level = args.session_opt.comp_level;
    if (algo == CodecAlgorithm::kLz4) {
        header_size = kLz4FrameHeaderSize;
        footer_size = kLz4FrameFooterSize;
    } else if (algo == CodecAlgorithm::kZlib) {
        header_size = kZlibHeaderSize;
        footer_size = kZlibFooterSize;
    }
    (*req_meta)->header_size = header_size;
    (*req_meta)->footer_size = footer_size;
    (*req_meta)->codec_algo = algo;
    (*req_meta)->comp_level = comp_level;
    if (args.direction == CodecDirection::kComp) {
        // src, no offset; dst, offset for footer
        fill_src_r = src_buf->FillSrc(args.src);
        fill_dst_r = dst_buf->FillDst(args.dst, header_size, footer_size);
    } else {
        // src, offset for header; dst, no offset
        fill_src_r = src_buf->FillSrc(args.src, header_size, footer_size);
        fill_dst_r = dst_buf->FillDst(args.dst);
    }
    if (VESAL_UNLIKELY(!fill_src_r || !fill_dst_r)) {
        VESAL_LOG(ERROR) << "Failed to fill QatBuf, req_meta: " << **req_meta;
        if (fill_src_r) {
            src_buf->FreeDataIfNecessary();
        }
        if (fill_dst_r) {
            dst_buf->FreeDataIfNecessary();
        }
        buf_cache_->ReturnOne(src_buf);
        buf_cache_->ReturnOne(dst_buf);
        return StatusCode::kResourceBusy;
    }

    // init qat callback context
    // remember to reset used fields before reusing cb_ctx
    *cb_ctx = in_flight_resource_->request_cb_context_pool_->Get(req_id);
    (*cb_ctx)->engine = this;
    (*cb_ctx)->req_id = req_id;
    (*cb_ctx)->src_qat = src_buf;
    (*cb_ctx)->dst_qat = dst_buf;
    if (channel_opts_.checksum_type != CodecChecksumType::kNone) {
        VESAL_DCHECK(channel_opts_.checksum_type == CodecChecksumType::kCrc32)
            << "Only support CRC32 as checksum.";
        // Used for checksum calculation.
        (*cb_ctx)->src_num = args.src.num_blocks;
        for (size_t i = 0; i < args.src.num_blocks; ++i) {
            (*cb_ctx)->src_data[i] = args.src.blocks[i].data;
            (*cb_ctx)->src_len[i] = args.src.blocks[i].size;
        }
        (*cb_ctx)->dst_data = args.dst.data;
        (*cb_ctx)->dst_len = args.dst.size;
    }

    return StatusCode::kOk;
}

void QatCodecEngine::CleanUpReqData(RequestMeta* req_meta, RequestCbContext* cb_ctx) {
    VESAL_LOG(DEBUG) << "CleanUpReqData called";
    if (VESAL_LIKELY(req_meta && req_meta->valid)) {
        // req_meta == nullptr usually means the request already timeout'd
        req_meta->valid = false;
        timer_manager_->RemoveTimer(req_meta->timer_ctx);
    }
    if (VESAL_LIKELY(cb_ctx)) {
        // cb_ctx == nullptr only happens when running out of DMA-memory/malloc-memory, and
        // QatBuffers init fails. Should not happen in usual.
        cb_ctx->Reset();
    }
}

void QatCodecEngine::InterceptInflightReq(RequestMeta* req_meta, const StatusCode& dummy_code) {
    auto* matched_req_meta = MatchAndInvalidateFlightReq(req_meta->req_id);
    VESAL_DCHECK(matched_req_meta && matched_req_meta == req_meta);
    // We have the req_metas. Create dummy results based on them.
    PushFailedCodecResult(req_meta, dummy_code);
    // Now for the inflight cb_ctx, store them in a vector and don't touch them anymore
    // because QAT might be accessing the memory.
    // TODO(sjj): find a way to cleanup the hanging inflight memory.
    auto* old_cb_ctx = ReplaceNewRequestCbContext(req_meta->req_id);
    VESAL_DCHECK(old_cb_ctx);
    discarded_cb_ctxs_.push_back(old_cb_ctx);
}

int QatCodecEngine::DoHa() {
    int abandoned_num = ClearRetiredQat(StatusCode::kDropped, StatusCode::kDropped);
    StatusCode ha_result = qat_handle_->Reinit();
    if (!IsOk(ha_result)) {
        VESAL_LOG(ERROR) << "HA failed due to " << ha_result;
        return -1;
    }
    return abandoned_num;
}

QatUnit* QatCodecEngine::GetQatUnit() {
    return qat_handle_->GetUnit();
}

checksum32_t QatCodecEngine::CompressedCRC32(const RequestCbContext* cb_ctx,
                                             const CodecDirection& dir,
                                             const CodecAlgorithm& algo) {
    if (!channel_opts_.compressed_checksum) {
        return 0;
    }
    // Compressed data is output data for compression, and input data for decompression.
    CodecDataDirection data_dir =
        dir == CodecDirection::kComp ? CodecDataDirection::kOutput : CodecDataDirection::kInput;
    if (!cb_ctx->IsCRC32CalcOffloaded()) {
        return cb_ctx->SwCalcCRC32(data_dir);
    }
    switch (algo) {
    case CodecAlgorithm::kLz4:
        return CombineThreeCRC32(kCrc32cInitialValue,
                                 GetPreCalcLz4HeaderCrc(),
                                 cb_ctx->GetOffloadedCRC32(data_dir),
                                 GetPreCalcLz4FooterCrc(),
                                 cb_ctx->GetQatProcessedDataSize(data_dir),
                                 kLz4FrameFooterSize);
    case CodecAlgorithm::kDeflate:
        return cb_ctx->GetOffloadedCRC32(data_dir);
    case CodecAlgorithm::kZlib: {
        // Zlib has a 4-byte dynamic footer and 2-byte dynamic header. Use thread_local here because
        // we ensure the QatCodecEngine is of TLS. Avoiding initialization every time.
        thread_local char zlib_header[kZlibHeaderSize] = {};
        thread_local char zlib_footer[kZlibFooterSize] = {};
        if (dir == CodecDirection::kComp) {
            cb_ctx->CopyDstData(zlib_header, kZlibHeaderSize, 0);
            // Note currently dst_data guranteens to be of one sgl. For compression we can directly
            // find the footer at the last 4 bytes of dst_data.
            cb_ctx->CopyDstData(zlib_footer,
                                kZlibFooterSize,
                                kZlibHeaderSize + cb_ctx->GetQatProcessedDataSize(data_dir));
        } else {
            cb_ctx->CopySrcData(zlib_header, kZlibHeaderSize, 0);
            // For decompression, get last 4 bytes of src_data.
            cb_ctx->CopySrcData(zlib_footer,
                                kZlibFooterSize,
                                kZlibHeaderSize + cb_ctx->GetQatProcessedDataSize(data_dir));
        }
        checksum32_t zlib_header_crc32 =
            ComputeCRC32(kCrc32cInitialValue, zlib_header, kZlibHeaderSize);
        checksum32_t zlib_footer_crc32 =
            ComputeCRC32(kCrc32cInitialValue, zlib_footer, kZlibFooterSize);
        return CombineThreeCRC32(kCrc32cInitialValue,
                                 zlib_header_crc32,
                                 cb_ctx->GetOffloadedCRC32(data_dir),
                                 zlib_footer_crc32,
                                 cb_ctx->GetQatProcessedDataSize(data_dir),
                                 kZlibFooterSize);
    }
    default:
        VESAL_LOG(CRITICAL) << "Not supported algorithm " << static_cast<int>(algo);
        return 0;
    }
}

checksum32_t QatCodecEngine::UncompressedCRC32(const RequestCbContext* cb_ctx,
                                               const CodecDirection& dir) {
    // Decompressed data is input data for decompression, and output data for compression.
    CodecDataDirection data_dir =
        dir == CodecDirection::kComp ? CodecDataDirection::kInput : CodecDataDirection::kOutput;
    return cb_ctx->IsCRC32CalcOffloaded() ? cb_ctx->GetOffloadedCRC32(data_dir)
                                          : cb_ctx->SwCalcCRC32(data_dir);
}

int QatCodecEngine::DropInflightReqsAndGenDummyResults(const StatusCode& change_to) {
    auto metas = timer_manager_->ClearAndGetUserArg();
    int size = metas.size();
    for (void* meta : metas) {
        auto* req_meta = reinterpret_cast<RequestMeta*>(meta);
        InterceptInflightReq(req_meta, StatusCode::kDropped);
    }
    return size;
}

int QatCodecEngine::ClearRetiredQat(const StatusCode& inflight_change_to,
                                    const StatusCode& results_change_to) {
    int abandoned_num = codec_results_->ModifyAll(results_change_to) +
                        DropInflightReqsAndGenDummyResults(inflight_change_to);
    next_recv_req_id_ = next_req_id_;
    callback_called_times_ = 0;
    in_qat_num_ = 0;
    return abandoned_num;
}

RequestCbContext* QatCodecEngine::ReplaceNewRequestCbContext(req_id_t req_id) {
    // TODO(sjj): Should not new here, but in_flight_resource_ pool are based on ordinary memory
    // so we have to follow. Change to use object pool in future.
    auto* u = new RequestCbContext();
    auto* old = in_flight_resource_->request_cb_context_pool_->Replace(req_id, u);
    // Need to restore the engine info, but not other stuff, to avoid double free.
    // TODO(sjj): Get a special shallow copy function for this.
    u->engine = old->engine;
    // Crc control data should be re-initialized.
    u->InitCrcControlData(channel_opts_.checksum_type);
    return old;
}

void Callback(void* pCallbackTag, CpaStatus status) {
    bool do_measure = IsEnableSampling();
    uint64_t start_time = do_measure ? TimeStamp::Now() : 0;
    auto* cb_ctx = reinterpret_cast<RequestCbContext*>(pCallbackTag);
    VESAL_LOG(DEBUG) << "Callback for req_id=" << cb_ctx->req_id;
    // check the request is timedout or not.
    QatCodecEngine* engine = cb_ctx->engine;
    VESAL_DCHECK(engine);
    // one more callback called
    ++engine->callback_called_times_;
    RequestMeta* req_meta = engine->MatchFlightReq(cb_ctx->req_id);

    // In callback we always need to do the cleanup.
    auto clean_up_guard = defer([&start_time, &engine, &req_meta, &cb_ctx, &do_measure]() {
        engine->CleanUpReqData(req_meta, cb_ctx);
        if (req_meta != nullptr) {
            DoMeasureIfNeed(do_measure,
                            req_meta->direction == CodecDirection::kComp
                                ? engine->compress_postprocess_latency_.get()
                                : engine->decompress_postprocess_latency_.get(),
                            start_time);
        }
    });

    // QAT guarantees out of order case will never happen, if it happens there must be serious
    // issues.
    req_id_t req_id_should_be = engine->next_recv_req_id_;
    if (VESAL_UNLIKELY(cb_ctx->req_id != req_id_should_be)) {
        VESAL_LOG(CRITICAL) << "Callback out of order happens! req_id_should_be="
                            << req_id_should_be << ", cb_ctx->req_id=" << cb_ctx->req_id;
    }
    engine->next_recv_req_id_++;

    if (VESAL_UNLIKELY(!req_meta)) {
        // this request maybe already timeout, ignore the result.
        VESAL_LOG(DEBUG) << "Fail to find context for req_id: " << cb_ctx->req_id;
        return;
    }

    // check qat offload success or not.
    if (VESAL_UNLIKELY(CPA_STATUS_SUCCESS != status || cb_ctx->cpa_results.status != CPA_DC_OK)) {
        VESAL_LOG(WARN) << "Callback error, req_id=" << cb_ctx->req_id << ", status=" << status
                        << ", reqStatus=" << cb_ctx->cpa_results.status;
        engine->PushFailedCodecResult(
            req_meta, CpaStatusToVesalStatusCode(status, cb_ctx->cpa_results.status));
        return;
    }

    VESAL_LOG(DEBUG) << "cpa_results, status: " << static_cast<int>(cb_ctx->cpa_results.status)
                     << ", consumed: " << cb_ctx->cpa_results.consumed
                     << ", produced: " << cb_ctx->cpa_results.produced
                     << ", checksum: " << cb_ctx->cpa_results.checksum;
    cb_ctx->vesal_consumed = cb_ctx->GetQatProcessedDataSize(CodecDataDirection::kInput);
    cb_ctx->vesal_produced = cb_ctx->GetQatProcessedDataSize(CodecDataDirection::kOutput);
    int header_size = req_meta->header_size;
    int footer_size = req_meta->footer_size;
    if (req_meta->direction == CodecDirection::kComp) {
        char hdr_content[kZlibHeaderSize];
        char ftr_content[kZlibFooterSize];
        char* hdr_content_ptr = hdr_content;
        char* ftr_content_ptr = ftr_content;
        if (req_meta->codec_algo == CodecAlgorithm::kLz4) {
            hdr_content_ptr = const_cast<char*>(GetLz4StaticHeader());
            ftr_content_ptr = const_cast<char*>(GetLz4StaticFooter());
        } else if (req_meta->codec_algo == CodecAlgorithm::kZlib) {
            // Zlib hedaer/footer must be filled at run-time.
            // TODO(sjj): Might need to find a way to fill the header/footer content
            // directly into QatBuf.
            ZlibHeaderGen(req_meta->comp_level, hdr_content);
            ZlibFooterGen(cb_ctx->cpa_results.checksum, ftr_content);
        }
        cb_ctx->dst_qat->CompCopyBackIfNecessary(cb_ctx->cpa_results.produced,
                                                 hdr_content_ptr,
                                                 header_size,
                                                 ftr_content_ptr,
                                                 footer_size);
        cb_ctx->vesal_produced += header_size + footer_size;
        engine->compress_throughput_->Add(cb_ctx->vesal_consumed);
    } else {
        cb_ctx->dst_qat->DecompCopyBackIfNecessary(cb_ctx->cpa_results.produced);
        cb_ctx->vesal_consumed += header_size + footer_size;
        engine->decompress_throughput_->Add(cb_ctx->vesal_produced);
    }

    checksum64_t in = 0;
    checksum64_t out = 0;
    if (engine->channel_opts_.checksum_type == CodecChecksumType::kCrc32) {
        if (req_meta->direction == CodecDirection::kComp) {
            in = engine->UncompressedCRC32(cb_ctx, CodecDirection::kComp);
            out = engine->CompressedCRC32(cb_ctx, CodecDirection::kComp, req_meta->codec_algo);
        } else {
            in = engine->CompressedCRC32(cb_ctx, CodecDirection::kDecomp, req_meta->codec_algo);
            out = engine->UncompressedCRC32(cb_ctx, CodecDirection::kDecomp);
        }
    }
    engine->PushSuccessCodecResult(
        req_meta, cb_ctx->vesal_consumed, cb_ctx->vesal_produced, in, out);
}

}  // namespace qat
}  // namespace vesal
