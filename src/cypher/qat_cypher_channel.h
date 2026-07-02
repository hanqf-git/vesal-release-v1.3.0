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

#include <list>
#include <memory>
#include <vector>

#include "common/dedicated_pool.h"
#include "common/qat/qat_buffer.h"
#include "common/qat/qat_session.h"
#include "common/qat/qat_unit.h"
#include "common/qat/qat_unit_manager.h"
#include "common/req_ring_queue.h"
#include "common/timer_manager.h"
#include "cpa.h"
#include "cpa_cy_sym.h"
#include "cpa_types.h"
#include "vesal/cypher.h"
#include "vesal/io_vector.h"
#include "vesal/log_setting.h"
#include "vesal/metrics.h"
#include "vesal/status.h"
#include "vesal/types.h"

namespace vesal {
namespace qat {

struct CypherReqMeta;
struct CypherCbContext;

struct CyInFlightResource {
    // inflight request map, including the requests that have already been sent to QAT
    std::unique_ptr<DedicatedPool<CypherReqMeta>> request_meta_pool_;
    // pooled resources
    std::unique_ptr<DedicatedPool<CypherCbContext>> request_cb_context_pool_;
    CyInFlightResource(size_t slot_num) {
        request_meta_pool_ = std::make_unique<DedicatedPool<CypherReqMeta>>(slot_num);
        request_cb_context_pool_ = std::make_unique<DedicatedPool<CypherCbContext>>(slot_num);
    }
};

struct CypherSessionPair {
    std::unique_ptr<QatSession> encrypt_session;
    std::unique_ptr<QatSession> decrypt_session;
    CypherSessionOption opt;
    bool is_hash;

    CypherSessionPair(QatUnit* unit) {
        encrypt_session = std::make_unique<QatSession>(unit);
        decrypt_session = std::make_unique<QatSession>(unit);
    }

    QatSession* GetEncryptSession() {
        return encrypt_session.get();
    }

    QatSession* GetDecryptSession() {
        return decrypt_session.get();
    }

    Status Init(const CypherSessionOption& option, QatUnit* unit);
};

class QatCypherChannel : public CypherChannel {
    friend void CyCallback(void* pCallbackTag,
                           CpaStatus status,
                           const CpaCySymOp op_type,
                           void* pOpData,
                           CpaBufferList* pDstBuffer,
                           CpaBoolean verify_result);

public:
    QatCypherChannel(const CypherChannelOption& channel_opts,
                     qat::QatUnitManager* unit_manager,
                     size_t max_in_qat_size);
    ~QatCypherChannel() override;

    Status Init();

    Status Close() override;

    void* AddSession(const CypherSessionOption& option) override;

    void RemoveSession(void* session) override;

    StatusCode SubmitCypherReq(unsigned char* src,
                               unsigned int src_len,
                               unsigned char* dst,
                               unsigned int dst_len,
                               CypherReqArgs* req) override;

    StatusCode SubmitCypherSGLReq(const std::vector<unsigned char*>& src,
                                  const std::vector<unsigned int>& src_len,
                                  unsigned char* dst,
                                  unsigned int dst_len,
                                  CypherReqArgs* req) override;

    StatusCode SubmitAsync(const CypherReqArgs& req) override;

    ssize_t Poll(CypherResult results[], unsigned int max_num, int timeout) override;

    void PushResult(CpaStatus status, CypherReqMeta* req_meta);

    QatBufCache* GetQatBufCache() {
        return buf_cache_.get();
    }

    void CleanUpReqData(CypherReqMeta* req_meta, CypherCbContext* cb_ctx);

private:
    CypherChannelOption channel_opts_;
    qat::QatUnitManager* unit_manager_;
    size_t max_in_qat_size_;
    qat::QatUnit* unit_;
    std::list<CypherSessionPair> sessions_;

    bool closed_;
    size_t in_qat_num_;

    std::unique_ptr<CyInFlightResource> in_flight_resource_;
    // Cache for QatBuf
    std::unique_ptr<QatBufCache> buf_cache_;
    uint32_t next_req_id_;
    // ring queue for buffering and ordering results
    std::unique_ptr<InflightReqRingQueue<CypherResult>> req_ring_queue_;
    std::unique_ptr<TimerManager> timer_manager_;
    std::unique_ptr<TimerManager> free_mem_timer_manager_;
    std::vector<CypherCbContext*> discarded_cb_ctxs_;
    // memory buffer for iv that needs to be copied, cost 16 bytes * size of inflight requests pool
    void* iv_buf_;

    // metrics
    std::shared_ptr<Counter> encrypt_throughput_;  // rate of total data encrypted by qat
    std::shared_ptr<Counter> decrypt_throughput_;  // rate of total data decrypted by qat
    // latency of process before submitting to hardware
    std::shared_ptr<Histogram> encrypt_preprocess_latency_;
    std::shared_ptr<Histogram> decrypt_preprocess_latency_;
    // latency of callback function
    std::shared_ptr<Histogram> encrypt_postprocess_latency_;
    std::shared_ptr<Histogram> decrypt_postprocess_latency_;
    // latency from user submitting requests to getting results or timeout
    std::shared_ptr<Histogram> encrypt_e2e_latency_;
    std::shared_ptr<Histogram> decrypt_e2e_latency_;
    std::shared_ptr<Histogram>
        encrypt_submit_latency_;  // time of submitting encrypt requests to qat driver
    std::shared_ptr<Histogram>
        decrypt_submit_latency_;  // time of submitting decrypt requests to qat driver
    std::vector<std::shared_ptr<Counter>>
        encrypt_rps_counters_;  // counter of each type of request's result
    std::vector<std::shared_ptr<Counter>>
        decrypt_rps_counters_;  // counter of each type of request's result
    std::shared_ptr<Gauge> metric_in_qat_num_;
    std::shared_ptr<Gauge> metric_max_in_qat_num_;
    std::shared_ptr<Counter> poll_total_time_;  // the total time cost of poll
    std::shared_ptr<Counter>
        poll_busy_time_;  // the time cost of poll if at least one result got polled
    std::shared_ptr<Histogram> poll_interval_;  // the interval time between poll function calls
    std::shared_ptr<Histogram> user_cb_time_;   // the time cost of user callback function in each
                                                // Poll() call. Might contain multiple calls.
    uint64_t last_poll_return_time_ = 0;
    uint32_t periodic_task_id_;

    bool IsChannelFull() {
        return in_qat_num_ == max_in_qat_size_;
    }
    Status UninitSession(CypherSessionPair* sess_pair);
    CypherCbContext* NewCypherCallbackCtx(uint32_t req_id,
                                          const CypherReqArgs& req,
                                          QatBuf* src_buf,
                                          QatBuf* dst_buf);
    StatusCode TryCloseSession(qat::QatSession* session);
    CypherReqMeta* MatchFlightReq(req_id_t req_id);
    CypherReqMeta* MatchAndInvalidateFlightReq(req_id_t req_id);
    // For cases like timeout, intercept the inflight request and generate a dummy result. Also
    // needs to store the callback context because hardware might be touching the DMA memory.
    void InterceptInflightReq(CypherReqMeta* req_meta, const StatusCode& dummy_code);
    CypherCbContext* ReplaceNewRequestCbContext(req_id_t req_id);
};

struct CypherReqMeta {
    req_id_t req_id = 0;
    int64_t ring_queue_id = -1;
    void* user_ctx;
    TimeoutContext* timer_ctx;
    int64_t begin_time;
    bool valid;
    CypherOp op;
    uint32_t consumed_len;  // data consumed in bytes
    IOBlock src_blocks[VESAL_MAX_SGL_NUM] = {};
    IOVector src{0, src_blocks};
    IOBlock dst;
};

struct CypherCbContext {
    QatCypherChannel* channel;
    req_id_t req_id;
    CpaCySymOpData op;
    CpaBoolean verify_result;
    QatBuf* src_buf;
    QatBuf* dst_buf;

    CypherCbContext() {
        channel = nullptr;
        req_id = 0;
        op.cryptoStartSrcOffsetInBytes = 0;
        op.hashStartSrcOffsetInBytes = 0;
        op.messageLenToCipherInBytes = 0;
        op.packetType = CPA_CY_SYM_PACKET_TYPE_FULL;
        verify_result = CPA_TRUE;
        src_buf = nullptr;
        dst_buf = nullptr;
    }

    void Reset() {
        if (VESAL_LIKELY(src_buf)) {
            src_buf->FreeDataIfNecessary();
            channel->GetQatBufCache()->ReturnOne(src_buf);
        }
        if (VESAL_LIKELY(dst_buf)) {
            dst_buf->FreeDataIfNecessary();
            channel->GetQatBufCache()->ReturnOne(dst_buf);
        }
        src_buf = nullptr;
        dst_buf = nullptr;
        channel = nullptr;
        req_id = 0;
    }

    ~CypherCbContext() {
        Reset();
    }
};

}  // namespace qat
}  // namespace vesal