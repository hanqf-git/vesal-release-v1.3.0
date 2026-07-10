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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>

#include "codec/codec_common.h"
#include "codec/dc_format.h"
#include "codec/qat/qat_codec.h"
#include "codec/qat/qat_ha.h"
#include "common/dedicated_pool.h"
#include "common/object_pool.h"
#include "common/qat/qat_buffer.h"
#include "common/qat/qat_session.h"
#include "common/qat/qat_unit.h"
#include "common/qat/qat_unit_manager.h"
#include "common/timer_manager.h"
#include "common/timestamp.h"
#include "cpa_dc.h"
#include "vesal/codec.h"
#include "vesal/io_vector.h"
#include "vesal/metrics.h"
#include "vesal/status.h"
#include "vesal/types.h"

namespace vesal {
namespace qat {

class QatHandle;
class QatCodecEngine;
struct RequestCbContext;
void Callback(void* pCallbackTag, CpaStatus status);

// qat codec request meta
struct RequestMeta {
    req_id_t req_id;
    void* user_ctx;
    TimeoutContext* timer_ctx;
    CodecDirection direction;
    int64_t begin_time;
    bool valid;
    // algorithm and compression level is request specific in multi-session scenario
    int header_size;
    int footer_size;
    CodecAlgorithm codec_algo;
    CodecCompLevel comp_level;
    IOBlock src_blocks[VESAL_MAX_SGL_NUM] = {};
    IOVector src;
    IOBlock dst;
#ifdef VESAL_ENABLE_ERR_SIM
    QatErrSimCode err_sim_code;
    uint64_t err_sim_onfire_ts;
#endif

    RequestMeta()
        : req_id(0),
          user_ctx(nullptr),
          timer_ctx(nullptr),
          direction(CodecDirection::kNum),
          begin_time(0),
          valid(false),
          header_size(0),
          footer_size(0),
          codec_algo(CodecAlgorithm::kNum),
          comp_level(CodecCompLevel::kNum),
          src{0, src_blocks},
          dst{} {
#ifdef VESAL_ENABLE_ERR_SIM
        err_sim_code = VESAL_QAT_ERR_SIM_OK;
        err_sim_onfire_ts = 0;
#endif
    }
};

std::ostream& operator<<(std::ostream& os, const RequestMeta& req_meta);

struct InFlightResource {
    // inflight request map, including the requests that have already been sent to QAT
    std::unique_ptr<DedicatedPool<RequestMeta>> request_meta_pool_;
    // pooled resources
    std::unique_ptr<DedicatedPool<RequestCbContext>> request_cb_context_pool_;
    InFlightResource(size_t slot_num) {
        request_meta_pool_ = std::make_unique<DedicatedPool<RequestMeta>>(slot_num);
        request_cb_context_pool_ = std::make_unique<DedicatedPool<RequestCbContext>>(slot_num);
    }
};

class QatCodecEngine {
    friend void Callback(void* pCallbackTag, CpaStatus status);

public:
    QatCodecEngine(const CodecChannelOption& channel_opts,
                   QatUnitManager* unit_manager,
                   size_t max_in_qat_size);

    ~QatCodecEngine();

    Status Init();

    StatusCode SubmitAsyncRequest(const CodecRequestArgs& args);

    StatusCode SubmitAsyncRequest(const CodecDirection& dir,
                                  const std::vector<unsigned char*>& src,
                                  const std::vector<unsigned int>& src_len,
                                  unsigned char* dst,
                                  unsigned int dst_len,
                                  void* ctx);

    ssize_t Poll(CodecResult results[], unsigned int max_num, int timeout);

    Status Close();

    StatusCode DumpDebugInfo();

    QatBufCache* GetQatBufCache() {
        return buf_cache_.get();
    }

    QatUnit* GetQatUnit();

private:
    checksum32_t CompressedCRC32(const RequestCbContext* cb_ctx,
                                 const CodecDirection& dir,
                                 const CodecAlgorithm& algo);

    checksum32_t UncompressedCRC32(const RequestCbContext* cb_ctx, const CodecDirection& dir);

    // Tag the request is inflight
    void TagInFlight(uint64_t timestamp, RequestMeta* req_meta) {
        req_meta->timer_ctx = timer_manager_->AddTimer(req_meta);
        req_meta->begin_time = timestamp;
        req_meta->valid = true;
    }

    bool IsHaEnabled() const {
        return ha_->IsHaEnabled();
    }

    /**
     * @brief Handle HA for non-ok results. There are two cases:
     * 1. If ha not triggered yet or even should not affect the HA, simply return false.
     * 2. If ha triggered, results produced by and inflight requests sent to last QAT are abandoned
     * in spite of the HA succeeded or not. If ha succeeded, return true. If ha failed, return false
     * and set the engine as fatal.
     * When this function return false, caller should just raise the error because should not
     * proceed anyway. Upper layer should decide by calling IsPermanentError() on the error to check
     * if the engine is dead. Abandon the engine if so, or retry the request if not.
     *
     * @param r The result of the request. Should be a non-ok status code.
     * @return True if ha enabled and succeeded. False otherwise.
     */
    bool HandleHa(const StatusCode& error) {
        VESAL_DCHECK(!IsOk(error)) << "Only HandleHa for non-ok cases.";
        VESAL_DCHECK(IsHaEnabled()) << "Should not call HandleHa if ha not enabled.";
        if (!ha_->ShouldFallback(error)) {
            return false;
        }
        // Now trigger HA.
        int abandoned_num = DoHa();
        if (abandoned_num < 0) {
            VESAL_LOG(ERROR) << "HA failed for " << error
                             << ", engine completely failed, should not try anymore.";
            is_fatal_ = true;
            return false;
        }
        VESAL_LOG(WARN) << "HA succeeded from " << error << ", abandoned " << abandoned_num
                        << " requests from last QAT unit.";
        return true;
    }

    int DropInflightReqsAndGenDummyResults(const StatusCode& change_to);

    // Used for HA process. Handle all buffered results by the last QAT first, abandon the untrusted
    // results. Then abandon the inflight requests and generate dummy results. Return the total
    // abandoned number.
    int ClearRetiredQat(const StatusCode& inflight_change_to, const StatusCode& results_change_to);

    // Get next request id
    req_id_t GetNextReqId() {
        return next_req_id_;
    }

    void IncreaseNextReqId() {
        ++next_req_id_;
    }

    // Match compress or decompress request according to `req_id'.
    // Mark req as invalid in request_meta_pool_ and return the req if valid. Return nullptr
    // otherwise
    RequestMeta* MatchAndInvalidateFlightReq(req_id_t req_id) {
        RequestMeta* req_meta = in_flight_resource_->request_meta_pool_->Get(req_id);
        if (VESAL_UNLIKELY(!req_meta->valid)) {
            return nullptr;
        }
        VESAL_DCHECK(req_meta->req_id == req_id);
        req_meta->valid = false;
        return req_meta;
    }

    RequestMeta* MatchFlightReq(req_id_t req_id) {
        RequestMeta* req_meta = in_flight_resource_->request_meta_pool_->Get(req_id);
        if (VESAL_UNLIKELY(!req_meta->valid)) {
            return nullptr;
        }
        VESAL_DCHECK(req_meta->req_id == req_id);
        return req_meta;
    }

    RequestCbContext* GetRequestCbContext(req_id_t req_id) {
        return in_flight_resource_->request_cb_context_pool_->Get(req_id);
    }

    // Replace the cb_ctx in the pool with the new cb_ctx. Not copied so the original
    // cb_ctx will be untouched.
    RequestCbContext* ReplaceNewRequestCbContext(req_id_t req_id);

    // Try to pop out the buffered results from codec_results_. Return the popped number.
    size_t PopBufferedResults(CodecResult results[], unsigned int max_num) {
        VESAL_LOG(DEBUG) << "codec_results_->Size()=" << codec_results_->Size();
#ifdef VESAL_ENABLE_ERR_SIM
        if (FLAGS_vesal_enable_err_sim) {
            max_num = codec_results_->PopIfReady(results, max_num);
        } else {
            max_num = codec_results_->Pop(results, max_num);
        }
#else
        max_num = codec_results_->Pop(results, max_num);
#endif
        return max_num;
    }

    // Push success codec result to queue
    void PushSuccessCodecResult(RequestMeta* req_meta,
                                size_t consumed,
                                size_t produced,
                                const checksum64_t& in,
                                const checksum64_t& out) {
#ifdef VESAL_ENABLE_ERR_SIM
        CodecResultList::ErrSimContext err_sim_ctx(
            req_meta->err_sim_code, req_meta->err_sim_onfire_ts, req_meta->timer_ctx->onfired_ts);
        codec_results_->Push(req_meta->user_ctx,
                             StatusCode::kOk,
                             consumed,
                             produced,
                             in,
                             out,
                             std::move(err_sim_ctx));
#else
        codec_results_->Push(req_meta->user_ctx, StatusCode::kOk, consumed, produced, in, out);
#endif
        auto& counters = req_meta->direction == CodecDirection::kComp ? compress_rps_counters_
                                                                      : decompress_rps_counters_;
        counters[static_cast<int>(StatusCode::kOk)]->Add(1);
        auto& m = req_meta->direction == CodecDirection::kComp ? compress_e2e_latency_
                                                               : decompress_e2e_latency_;
        m->Set(TimeStamp::DurationToNs(TimeStamp::Now() - req_meta->begin_time));
    }

    // Push failed codec result to queue
    void PushFailedCodecResult(RequestMeta* req_meta, const StatusCode& status) {
#ifdef VESAL_ENABLE_ERR_SIM
        CodecResultList::ErrSimContext err_sim_ctx(
            req_meta->err_sim_code, req_meta->err_sim_onfire_ts, req_meta->timer_ctx->onfired_ts);
        codec_results_->Push(req_meta->user_ctx, status, std::move(err_sim_ctx));
#else
        codec_results_->Push(req_meta->user_ctx, status);
#endif
        auto& counters = req_meta->direction == CodecDirection::kComp ? compress_rps_counters_
                                                                      : decompress_rps_counters_;
        counters[static_cast<int>(status)]->Add(1);
        auto& m = req_meta->direction == CodecDirection::kComp ? compress_e2e_latency_
                                                               : decompress_e2e_latency_;
        m->Set(TimeStamp::DurationToNs(TimeStamp::Now() - req_meta->begin_time));
    }

    bool QatRingFull() const {
        return in_qat_num_ >= max_in_qat_size_;
    }

    bool QatRingEmpty() const {
        return !in_qat_num_;
    }

    /**
     * @brief: Allocate and initialise common request data based on input. Caller needs to release
     * the resources despite the function success or not.
     *
     * @param in: dir - the direction, compress, decompress or combined
     * @param in: src - user's source data
     * @param in: src_len - user's source data length
     * @param in: dst - user provided buffer for the result data
     * @param in: dst_len - user's dst data buffer length
     * @param in: ctx - user's context data
     * @param out: req_meta - on success, initialised based on provided data.
     * @param out: cb_ctx - on success, initialised based on provided data.
     *
     * @return: On success, return OkStatus. req_meta, cb_ctx are initialised with
     * proper state, ready to be used by QAT driver compress/decompress API. Otherwise return the
     * status with reason, caller needs to call `CleanUpReqData` to clean up the resources.
     */
    StatusCode PrepareCommonReqData(const CodecRequestArgs& args,
                                    RequestMeta** req_meta,
                                    RequestCbContext** cb_ctx);

    void CleanUpReqData(RequestMeta* req_meta, RequestCbContext* cb_ctx);

    // For cases like timeout, intercept the inflight request and generate a dummy result. Also
    // needs to store the callback context because hardware might be touching the DMA memory.
    void InterceptInflightReq(RequestMeta* req_meta, const StatusCode& dummy_code);

    // Return abandoned request number if ha success, or return -1.
    int DoHa();

    QatUnitManager* unit_manager_;
    CodecChannelOption channel_opts_;
    std::unique_ptr<QatHandle> qat_handle_;
    // the req_id should be allocated to new request. It's guranteed that
    // req_id = 1, 2, ..., next_req_id_-1
    // matches every previously successfully submitted request
    req_id_t next_req_id_;
    // the req_id should be received from QAT. QatCodecEngine guarantees that the order of callback
    // executed matches the order of submission.
    req_id_t next_recv_req_id_;
    // the number of the requests that already sent to QAT
    uint64_t in_qat_num_;
    // how many callback executed in one poll
    size_t callback_called_times_;
    bool closed_;

    std::unique_ptr<InFlightResource> in_flight_resource_;

    // timeout manager
    std::unique_ptr<TimerManager> timer_manager_;
    // timeout manager for freeing memory
    std::unique_ptr<TimerManager> free_mem_timer_manager_;

    // used to restore compress and decompress results from qat.
    std::unique_ptr<CodecResultList> codec_results_;
    // Cache for QatBuf
    std::unique_ptr<QatBufCache> buf_cache_;

    size_t max_in_qat_size_;

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
    std::shared_ptr<Counter> poll_total_time_;  // the total time cost of poll
    std::shared_ptr<Counter>
        poll_busy_time_;  // the time cost of poll if at least one result got polled
    std::shared_ptr<Histogram> poll_interval_;  // the interval time between poll function calls
    uint64_t last_poll_return_time_ = 0;
    std::shared_ptr<Histogram>
        compress_submit_latency_;  // time of submitting compress requests to qat driver
    std::shared_ptr<Histogram>
        decompress_submit_latency_;  // time of submitting decompress requests to qat driver
    std::vector<std::shared_ptr<Counter>>
        compress_rps_counters_;  // counter of each type of request's result
    std::vector<std::shared_ptr<Counter>>
        decompress_rps_counters_;  // counter of each type of request's result
    std::shared_ptr<Gauge> metric_in_qat_num_;
    std::shared_ptr<Gauge> metric_max_in_qat_num_;
    uint32_t periodic_task_id_;
    std::unique_ptr<QatHa> ha_;
    bool is_fatal_;  // Channel is completely not usable. Reject all requests.
    std::vector<RequestCbContext*> discarded_cb_ctxs_;
};

// qat callback context
struct RequestCbContext {
    QatCodecEngine* engine = nullptr;
    req_id_t req_id = 0;
    CpaDcRqResults cpa_results;
    // sizeof(CpaDcOpData)=64
    CpaDcOpData op_data;
    // src_qat and dst_qat needs to be nullptr after reset
    QatBuf* src_qat = nullptr;
    QatBuf* dst_qat = nullptr;
    // Reference to the original src data. Can be used for crc calculation.
    unsigned char* src_data[VESAL_MAX_SGL_NUM] = {};
    unsigned int src_len[VESAL_MAX_SGL_NUM] = {};
    size_t src_num = 0;
    const unsigned char* dst_data = nullptr;
    unsigned int dst_len = 0;

    // The actual size of src data processed.
    size_t vesal_consumed = 0;
    // The actual size of dst data modified.
    size_t vesal_produced = 0;

    RequestCbContext() {
        // Init op data
        op_data.compressAndVerify = CPA_TRUE;
        // New QAT driver 1.1.40.0018 does not support CnVR, hence CPA_FALSE
        op_data.compressAndVerifyAndRecover = CPA_FALSE;
        op_data.flushFlag = CPA_DC_FLUSH_FINAL;
        op_data.inputSkipData.skipMode = CPA_DC_SKIP_DISABLED;
        op_data.outputSkipData.skipMode = CPA_DC_SKIP_DISABLED;
        op_data.integrityCrcCheck = CPA_FALSE;
        op_data.verifyHwIntegrityCrcs = CPA_FALSE;
        op_data.pCrcData = nullptr;
    }

    void InitCrcControlData(const CodecChecksumType& checksum_type) {
        if (checksum_type == CodecChecksumType::kCrc32) {
            op_data.pCrcData = get_tls_object<CpaCrcData>();
            op_data.integrityCrcCheck = CPA_TRUE;
            op_data.integrityCrcSize = CPA_DC_INTEGRITY_CRC32;
        } else {
            op_data.pCrcData = nullptr;
            op_data.integrityCrcCheck = CPA_FALSE;
        }
    }

    bool IsCRC32CalcOffloaded() const {
        return op_data.integrityCrcCheck == CPA_TRUE;
    }

    // Copy len bytes of data into buf, from offset of src_data.
    // Return the number of bytes copied.
    size_t CopySrcData(void* buf, size_t len, size_t offset) const {
        size_t i = 0;
        size_t left = offset;
        // Ensure that offset + len <= total src len.
        VESAL_DCHECK(offset + len <=
                     std::accumulate(src_len, src_len + src_num, static_cast<size_t>(0)));
        while (left >= src_len[i]) {
            left -= src_len[i++];
        }
        size_t copied = 0;
        while (copied < len) {
            size_t copy_len = std::min(len - copied, src_len[i] - left);
            std::memcpy(buf, src_data[i] + left, copy_len);
            copied += copy_len;
            left = 0;
            ++i;
        }
        return copied;
    }

    // Copy len bytes of data from buf to dst_data, from offset of dst_data.
    // Return the number of bytes copied.
    size_t CopyDstData(void* buf, size_t len, size_t offset) const {
        VESAL_DCHECK(offset + len <= dst_len);
        std::memcpy(buf, dst_data + offset, len);
        return len;
    }

    checksum32_t GetOffloadedCRC32(const CodecDataDirection& data_dir) const {
        VESAL_DCHECK(IsCRC32CalcOffloaded());
        return data_dir == CodecDataDirection::kInput ? op_data.pCrcData->integrityCrc64b.iCrc
                                                      : op_data.pCrcData->integrityCrc64b.oCrc;
    }

    size_t GetQatProcessedDataSize(const CodecDataDirection& data_dir) const {
        return data_dir == CodecDataDirection::kInput ? cpa_results.consumed : cpa_results.produced;
    }

    // Note we can only calculate the checksum of data that is actually part of the
    // comp/decompression.
    checksum32_t SwCalcCRC32(const CodecDataDirection& data_dir) const {
        if (data_dir == CodecDataDirection::kInput) {
            if (src_num == 1) {
                return ComputeCRC32(kCrc32cInitialValue,
                                    reinterpret_cast<const char*>(src_data[0]),
                                    vesal_consumed);
            }
            auto guard = std::make_unique<unsigned char[]>(vesal_consumed);
            CopySrcData(guard.get(), vesal_consumed, 0);
            return ComputeCRC32(
                kCrc32cInitialValue, reinterpret_cast<const char*>(guard.get()), vesal_consumed);
        }
        return ComputeCRC32(
            kCrc32cInitialValue, reinterpret_cast<const char*>(dst_data), vesal_produced);
    }

    void Reset() {
        if (VESAL_LIKELY(src_qat)) {
            src_qat->FreeDataIfNecessary();
            engine->GetQatBufCache()->ReturnOne(src_qat);
        }
        if (VESAL_LIKELY(dst_qat)) {
            dst_qat->FreeDataIfNecessary();
            engine->GetQatBufCache()->ReturnOne(dst_qat);
        }
        src_qat = nullptr;
        dst_qat = nullptr;
        src_num = 0;
        dst_data = nullptr;
        dst_len = 0;
        engine = nullptr;
        req_id = 0;
        vesal_consumed = 0;
        vesal_produced = 0;
    }

    ~RequestCbContext() {
        Reset();
        if (VESAL_LIKELY(op_data.pCrcData)) {
            return_tls_object(op_data.pCrcData);
            op_data.pCrcData = nullptr;
        }
    }
};

}  // namespace qat
}  // namespace vesal
