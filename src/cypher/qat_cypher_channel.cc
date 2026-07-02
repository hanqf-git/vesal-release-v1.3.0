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

#include "cypher/qat_cypher_channel.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numeric>

#include "codec/codec_common.h"
#include "codec/qat/qat_error_handling.h"
#include "common/dedicated_pool.h"
#include "common/defer.h"
#include "common/memory_pool_helper.h"
#include "common/metrics_internal.h"
#include "common/qat/qat_buffer.h"
#include "common/qat/qat_hardware_api_wrapper.h"
#include "common/qat/qat_session.h"
#include "common/qat/qat_unit.h"
#include "common/qat/qat_util.h"
#include "common/req_ring_queue.h"
#include "common/scheduler.h"
#include "common/timestamp.h"
#include "cpa.h"
#include "cypher/cypher.h"
#include "vesal/cypher.h"
#include "vesal/io_vector.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/types.h"
#include "vesal/vesal.h"

namespace vesal {
namespace qat {

QatCypherChannel::QatCypherChannel(const CypherChannelOption& opts,
                                   QatUnitManager* unit_manager,
                                   size_t max_in_qat_size)
    : channel_opts_(opts),
      unit_manager_(unit_manager),
      max_in_qat_size_(max_in_qat_size),
      unit_(nullptr),
      closed_(true),
      in_qat_num_(0),
      in_flight_resource_(std::make_unique<CyInFlightResource>(next_power_of_two(max_in_qat_size))),
      next_req_id_(0) {}

QatCypherChannel::~QatCypherChannel() {
    VESAL_CHECK(closed_);
}

void CyCallback(void* pCallbackTag,
                CpaStatus status,
                const CpaCySymOp op_type,
                void* pOpData,
                CpaBufferList* pDstBuffer,
                CpaBoolean verify_result);

Status InitSession(CypherOp op, const CypherSessionOption& sess_opt, QatSession* session) {
    QatSessionOption opt;
    opt.type = QatSessionType::kCypher;
    opt.SymOption.session_opt = sess_opt;
    opt.SymOption.op = op;
    Status sess_r = session->Init(opt, CyCallback);
    if (!sess_r.ok()) {
        return sess_r;
    }
    return OkStatus();
}

inline bool IsHash(const CypherAlgorithm& algo) {
    switch (algo) {
    case CypherAlgorithm::kAES_XTS:
        return false;
    case CypherAlgorithm::kSHA256:
    default:
        return true;
    }
}

Status CypherSessionPair::Init(const CypherSessionOption& option, QatUnit* unit) {
    opt = option;
    is_hash = IsHash(opt.algorithm);
    // No matter what algorithm, at least one session needs to be initialzed
    auto r = InitSession(CypherOp::kEncrypt, option, encrypt_session.get());
    if (!r.ok()) {
        VESAL_LOG(ERROR) << "Failed to init qat session, r = " << r;
        return r;
    }
    switch (option.algorithm) {
    case CypherAlgorithm::kAES_XTS: {
        r = InitSession(CypherOp::kDecrypt, option, decrypt_session.get());
    }
    // For hash algorithms, no need to init second session
    default:
        break;
    }
    if (!r.ok()) {
        VESAL_LOG(ERROR) << "Failed to init qat session, r = " << r;
    }
    return r;
}

Status QatCypherChannel::Init() {
    QatUnitSelection selection;
    selection.numa_id = channel_opts_.allocation_option.node_affinity;
    selection.pf_id = channel_opts_.allocation_option.device_id;
    unit_ = unit_manager_->GrabAvailableUnit(selection);
    if (unit_ == nullptr) {
        return ResourceBusyError("No available QAT unit.");
    }
    VESAL_LOG(DEBUG) << "Grabed " << *unit_;
    CypherSessionPair p(unit_);
    auto r = p.Init(channel_opts_.session_option, unit_);
    if (!r.ok()) {
        VESAL_LOG(ERROR) << "Fail to init session, r=" << r;
        // TODO(lpn): better error handling;
        if (!UninitSession(&p).ok()) {
            VESAL_LOG(ERROR) << "Fail to clean up sessions, this channel is doomed";
        } else {
            unit_manager_->PutBackUnit(unit_);
            unit_ = nullptr;
        }
        return r;
    }
    sessions_.push_back(std::move(p));
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
            auto* req_meta = static_cast<CypherReqMeta*>(timer_arg);
            InterceptInflightReq(req_meta, StatusCode::kTimeout);
            free_mem_timer_manager_->AddTimer(
                new std::pair<IOVector, IOBlock>(req_meta->src, req_meta->dst));
        },
        TimeStamp::MsToDuration(channel_opts_.timeout_ms));
    // ring queue size set to FLAGS_vesal_qat_channel_buffer_multiplier * max_in_qat_size to avoid
    // infinite buffering
    req_ring_queue_ = std::make_unique<InflightReqRingQueue<CypherResult>>(
        next_power_of_two(max_in_qat_size_ * FLAGS_vesal_qat_channel_buffer_multiplier));
    buf_cache_ = std::make_unique<QatBufCache>(unit_, max_in_qat_size_);
    VESAL_CHECK(buf_cache_->Init(false));
    {
        // Allocate dma memory for iv in case user provides non dma iv
        int n = in_flight_resource_->request_cb_context_pool_->slot_num_;
        iv_buf_ = MemoryPool::GetInstance()->Allocate(kIvSize * n);
        VESAL_CHECK(iv_buf_) << "Failed to allocate " << kIvSize * n
                             << "bytes from memory pool for IV buffer";
        for (int i = 0; i < n; i++) {
            CypherCbContext* cb = in_flight_resource_->request_cb_context_pool_->Get(i);
            cb->op.pIv = (unsigned char*)iv_buf_ + i * kIvSize;
        }
    }
    closed_ = false;

    // TODO(Pinnong Li): Adapt metrics to hash
    Tag tag_service_type = std::make_pair("service_type", "cypher");
    Tag tag_engine = std::make_pair("engine", "qat");
    Tag tag_instance = std::make_pair("instance", unit_->GetQatUnitAttr().instance_id);
    Tag tag_device = std::make_pair("device", std::to_string(unit_->GetQatUnitAttr().device_id));
    std::vector<Tag> common_tags = {tag_engine, tag_device, tag_instance, tag_service_type};
    std::vector<Tag> tags_with_encrypt_tag = {
        tag_engine, tag_device, tag_instance, std::make_pair("type", "encrypt")};
    std::vector<Tag> tags_with_decrypt_tag = {
        tag_engine, tag_device, tag_instance, std::make_pair("type", "decrypt")};
    encrypt_throughput_ =
        g_metric_registry->RegisterCounter("vesal.throughput", tags_with_encrypt_tag);
    decrypt_throughput_ =
        g_metric_registry->RegisterCounter("vesal.throughput", tags_with_decrypt_tag);
    encrypt_preprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.preprocess_latency", tags_with_encrypt_tag);
    decrypt_preprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.preprocess_latency", tags_with_decrypt_tag);
    encrypt_postprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.postprocess_latency", tags_with_encrypt_tag);
    decrypt_postprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.postprocess_latency", tags_with_decrypt_tag);
    encrypt_e2e_latency_ =
        g_metric_registry->RegisterHistogram("vesal.e2e_latency", tags_with_encrypt_tag);
    decrypt_e2e_latency_ =
        g_metric_registry->RegisterHistogram("vesal.e2e_latency", tags_with_decrypt_tag);
    encrypt_submit_latency_ =
        g_metric_registry->RegisterHistogram("vesal.submit_latency", tags_with_encrypt_tag);
    decrypt_submit_latency_ =
        g_metric_registry->RegisterHistogram("vesal.submit_latency", tags_with_decrypt_tag);
    for (const auto& code : GetAllStatusCodes()) {
        encrypt_rps_counters_.push_back(g_metric_registry->RegisterCounter(
            "vesal.rps",
            {tag_engine,
             tag_device,
             tag_instance,
             tag_service_type,
             std::make_pair("type", "encrypt"),
             std::make_pair("status", StatusCodeToString(code))}));
        decrypt_rps_counters_.push_back(g_metric_registry->RegisterCounter(
            "vesal.rps",
            {tag_engine,
             tag_device,
             tag_instance,
             tag_service_type,
             std::make_pair("type", "decrypt"),
             std::make_pair("status", StatusCodeToString(code))}));
    }
    metric_in_qat_num_ = g_metric_registry->RegisterGauge("vesal.in_qat_num", common_tags);
    metric_max_in_qat_num_ = g_metric_registry->RegisterGauge("vesal.max_in_qat_num", common_tags);
    poll_total_time_ = g_metric_registry->RegisterCounter("vesal.poll_total_time", common_tags);
    poll_busy_time_ = g_metric_registry->RegisterCounter("vesal.poll_busy_time", common_tags);
    poll_interval_ = g_metric_registry->RegisterHistogram("vesal.poll_interval", common_tags);
    user_cb_time_ = g_metric_registry->RegisterHistogram("vesal.user_cb_time", common_tags);
    periodic_task_id_ = g_periodic_scheduler.AddPeriodicTask(
        [&]() {
            metric_in_qat_num_->Set(in_qat_num_);
            metric_max_in_qat_num_->Set(max_in_qat_size_);
        },
        std::chrono::milliseconds(1000));
    return r;
}

StatusCode QatCypherChannel::TryCloseSession(qat::QatSession* session) {
    if (!session) {
        return StatusCode::kOk;
    }
    int cnt = 10;
    StatusCode ret = session->Close().code();
    while (!IsOk(ret) && cnt-- > 0) {
        usleep(100);
        GetQatApiWrapper()->QAT_icp_sal_CyPollInstance(*(unit_->GetInstanceHandle()), 0);
        ret = session->Close().code();
    }
    return ret;
}

Status QatCypherChannel::UninitSession(CypherSessionPair* sess_pair) {
    if (unit_) {
        auto r = TryCloseSession(sess_pair->GetEncryptSession());
        if (!IsOk(r)) {
            VESAL_LOG(ERROR) << "Fail to close encrypt session, r=" << r;
            return {r, "Fail to close encrypt session."};
        }
        r = TryCloseSession(sess_pair->GetDecryptSession());
        if (!IsOk(r)) {
            VESAL_LOG(ERROR) << "Fail to close decrypt session, r=" << r;
            return {r, "Fail to close decrypt session."};
        }
        sess_pair->encrypt_session.reset();
        sess_pair->decrypt_session.reset();
    }
    return OkStatus();
}

Status QatCypherChannel::Close() {
    if (closed_) {
        return OkStatus();
    }
    g_periodic_scheduler.CompleteTask(periodic_task_id_);
    for (auto& sess : sessions_) {
        auto r = UninitSession(&sess);
        if (!r.ok()) {
            VESAL_LOG(ERROR) << "Fail to close session, r=" << r;
            return r;
        }
    }
    unit_manager_->PutBackUnit(unit_);
    unit_ = nullptr;
    timer_manager_.reset();
    free_mem_timer_manager_.reset();
    req_ring_queue_.reset();
    for (auto* cb_ctx : discarded_cb_ctxs_) {
        cb_ctx->Reset();
        delete cb_ctx;
    }
    discarded_cb_ctxs_.clear();
    in_flight_resource_.reset();
    buf_cache_.reset();
    MemoryPool::GetInstance()->Deallocate(iv_buf_);
    closed_ = true;
    return OkStatus();
}

void* QatCypherChannel::AddSession(const CypherSessionOption& option) {
    CypherSessionPair p(unit_);
    auto r = p.Init(option, unit_);
    if (!r.ok()) {
        VESAL_LOG(ERROR) << "Fail to init qat session, r=" << r;
        return nullptr;
    }
    sessions_.push_back(std::move(p));
    return &sessions_.back();
}

void QatCypherChannel::RemoveSession(void* session) {
    auto* p = static_cast<CypherSessionPair*>(session);
    auto r = UninitSession(p);
    if (!r.ok()) {
        VESAL_LOG(ERROR) << "Fail to close session, r=" << r;
    }
}

bool ValidateArgs(const CypherReqArgs& req, CypherSessionPair* p) {
    bool r = true;
    switch (p->opt.algorithm) {
    case CypherAlgorithm::kAES_XTS: {
        unsigned int tot_len = 0;
        for (uint32_t i = 0; i < req.src.num_blocks; i++) {
            tot_len += req.src.blocks[i].size;
        }
        if (req.dst.size < tot_len) {
            r = false;
            VESAL_LOG(ERROR) << "Invalid args for AES_XTS, length of dst buffer is " << req.dst.size
                             << " while the total length of src buffer is " << tot_len;
        }
        break;
    }
    case CypherAlgorithm::kSHA256: {
        if (req.dst.size < SHA256_DST_LEN) {
            r = false;
            VESAL_LOG(ERROR) << "Invalid args for Sha256, dst_len is " << req.dst.size
                             << ", requires at least 32";
        }
        break;
    }
    default:
        break;
    }
    return r;
}

StatusCode QatCypherChannel::SubmitCypherReq(unsigned char* src,
                                             unsigned int src_len,
                                             unsigned char* dst,
                                             unsigned int dst_len,
                                             CypherReqArgs* req) {
    return SubmitCypherSGLReq({src}, {src_len}, dst, dst_len, req);
}

CypherCbContext* QatCypherChannel::NewCypherCallbackCtx(uint32_t req_id,
                                                        const CypherReqArgs& req,
                                                        QatBuf* src_buf,
                                                        QatBuf* dst_buf) {
    CypherCbContext* cb = in_flight_resource_->request_cb_context_pool_->Get(req_id);
    CypherSessionPair* p =
        req.session == nullptr ? &sessions_.front() : (CypherSessionPair*)req.session;
    const CypherSessionOption& opt = p->opt;
    qat::QatSession* encrypt_session = p->GetEncryptSession();
    qat::QatSession* decrypt_session = p->GetDecryptSession();

    cb->req_id = req_id;
    cb->channel = this;
    Cpa32U* len_ptr = nullptr;
    switch (opt.algorithm) {
    case CypherAlgorithm::kAES_XTS: {
        len_ptr = &cb->op.messageLenToCipherInBytes;
        cb->op.sessionCtx = req.op == CypherOp::kEncrypt ? encrypt_session->GetSessionCtx()
                                                         : decrypt_session->GetSessionCtx();
        cb->op.ivLenInBytes = 16;  // Tweak length is fixed to 16 bytes by algorithm
        memcpy(cb->op.pIv, req.aes_xts_tweak, cb->op.ivLenInBytes);
        break;
    }
    case CypherAlgorithm::kSHA256: {
        len_ptr = &cb->op.messageLenToHashInBytes;
        cb->op.sessionCtx = encrypt_session->GetSessionCtx();
        cb->op.pDigestResult = dst_buf->GetCpaBufferList()->pBuffers->pData;
    }
    default:
        break;
    }
    *len_ptr = 0;
    for (uint32_t i = 0; i < req.src.num_blocks; i++) {
        *len_ptr += req.src.blocks[i].size;
    }
    cb->src_buf = src_buf;
    cb->dst_buf = dst_buf;
    return cb;
}

StatusCode QatCypherChannel::SubmitCypherSGLReq(const std::vector<unsigned char*>& src,
                                                const std::vector<unsigned int>& src_len,
                                                unsigned char* dst,
                                                unsigned int dst_len,
                                                CypherReqArgs* req) {
    IOBlock blocks[VESAL_MAX_SGL_NUM];
    for (uint32_t i = 0; i < src.size(); i++) {
        blocks[i].data = src[i];
        blocks[i].size = src_len[i];
        blocks[i].deleter = nullptr;
    }
    req->src.blocks = blocks;
    req->src.num_blocks = src.size();
    req->dst.data = dst;
    req->dst.size = dst_len;
    return SubmitAsync(*req);
}

StatusCode QatCypherChannel::SubmitAsync(const CypherReqArgs& req) {
    auto begin_time = TimeStamp::Now();
    if (IsChannelFull() || req_ring_queue_->IsFull()) {
        VESAL_LOG(ERROR) << "Fail to submit cypher req, qat full: " << IsChannelFull()
                         << ", in_qat_num: " << in_qat_num_
                         << ", req ring queue full: " << req_ring_queue_->IsFull()
                         << ", req_ring_queue size: " << req_ring_queue_->GetSize();
        return StatusCode::kResourceBusy;
    }
    bool do_measure = IsEnableSampling();
    CypherSessionPair* p =
        req.session == nullptr ? &sessions_.front() : (CypherSessionPair*)req.session;
    if (!ValidateArgs(req, p)) {
        return StatusCode::kInvalidArgument;
    }
    bool is_hash = p->is_hash;
    // Always encrypt when doing hash
    bool is_dir_encrypt = req.op == CypherOp::kEncrypt || is_hash;
    // Prepare buf
    DURATION_TO_RETURN(
        do_measure,
        is_dir_encrypt ? encrypt_preprocess_latency_.get() : decrypt_preprocess_latency_.get(),
        begin_time);
    QatBuf* src_buf = buf_cache_->GetOne();
    QatBuf* dst_buf = buf_cache_->GetOne();
    if (VESAL_UNLIKELY(!src_buf || !dst_buf)) {
        VESAL_LOG(ERROR) << "Failed to allocate QatBuf, current req_id: " << next_req_id_
                         << ", op: " << req.op;
        buf_cache_->ReturnOne(src_buf);
        buf_cache_->ReturnOne(dst_buf);
        return StatusCode::kResourceBusy;
    }
    bool fill_src_r = src_buf->FillSrc(req.src);
    bool fill_dst_r = dst_buf->FillDst(req.dst);
    if (VESAL_UNLIKELY(!fill_src_r || !fill_dst_r)) {
        VESAL_LOG(ERROR) << "Failed to fill QatBuf, current req_id: " << next_req_id_
                         << ", op: " << req.op;
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
    // set dst_buf_list to be src_buf if it's hash algorithm, otherwise dst_buf
    CpaBufferList* dst_buf_list =
        is_hash ? src_buf->GetCpaBufferList() : dst_buf->GetCpaBufferList();
    // Prepare args for submitting to driver
    CypherCbContext* cb = NewCypherCallbackCtx(next_req_id_, req, src_buf, dst_buf);
    CpaInstanceHandle* inst_handle = unit_->GetInstanceHandle();
    auto r = StatusCode::kOk;
    auto clean_up_guard = defer([this, &r, &cb, &begin_time, &req, is_hash]() {
        if (VESAL_UNLIKELY(!IsOk(r))) {
            VESAL_LOG(ERROR) << "Failed to submit cypher req, r=" << r << ", req_id=" << cb->req_id
                             << ", op=" << req.op;
            CleanUpReqData(nullptr, cb);
            Counter* counter = req.op == CypherOp::kEncrypt
                                   ? encrypt_rps_counters_[static_cast<int>(r)].get()
                                   : decrypt_rps_counters_[static_cast<int>(r)].get();
            counter->Add(1);
        } else {
            CypherReqMeta* req_meta = in_flight_resource_->request_meta_pool_->Get(next_req_id_);
            req_meta->req_id = next_req_id_;
            req_meta->ring_queue_id = req_ring_queue_->NewReq();
            req_meta->user_ctx = req.ctx;
            req_meta->timer_ctx = timer_manager_->AddTimer(req_meta);
            req_meta->valid = true;
            req_meta->begin_time = begin_time;
            req_meta->op = req.op;
            req_meta->consumed_len =
                is_hash ? cb->op.messageLenToHashInBytes : cb->op.messageLenToCipherInBytes;
            req_meta->src.num_blocks = req.src.num_blocks;
            // copy blocks address in case user released the req.src
            for (uint32_t i = 0; i < req.src.num_blocks; ++i) {
                req_meta->src.blocks[i] = req.src.blocks[i];
            }
            req_meta->dst = req.dst;
            next_req_id_++;
            in_qat_num_++;
        }
    });
    auto submit_begin_time = do_measure ? TimeStamp::Now() : 0;
    r = CpaStatusToVesalStatusCode(GetQatApiWrapper()->QAT_cpaCySymPerformOp(
        *inst_handle, cb, &cb->op, src_buf->GetCpaBufferList(), dst_buf_list, &cb->verify_result));
    DoMeasureIfNeed(do_measure,
                    is_dir_encrypt ? encrypt_submit_latency_.get() : decrypt_submit_latency_.get(),
                    submit_begin_time);
    return r;
}

ssize_t QatCypherChannel::Poll(CypherResult results[], unsigned int max_num, int timeout) {
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
    auto user_cb_guard = defer([this, &max_num, &results] {
        if (channel_opts_.user_cb && max_num) {
            auto ts = TimeStamp::Now();
            for (size_t i = 0; i < max_num; i++) {
                channel_opts_.user_cb(results[i]);
            }
            user_cb_time_->Set(TimeStamp::DurationToNs((TimeStamp::Now() - ts)));
        }
    });
    auto r = GetQatApiWrapper()->QAT_icp_sal_CyPollInstance(*(unit_->GetInstanceHandle()), 0);
    if (r != CPA_STATUS_SUCCESS && r != CPA_STATUS_RETRY) {
        VESAL_LOG(ERROR) << "Fail to QAT_icp_sal_CyPollInstance, r=" << r
                         << ", instance_id=" << unit_->GetInstId();
        return -1;
    }
    timer_manager_->HandleTimeout();
    free_mem_timer_manager_->HandleTimeout();
    max_num = req_ring_queue_->PopResults(results, max_num);
    return max_num;
}

void QatCypherChannel::PushResult(CpaStatus status, CypherReqMeta* req_meta) {
    CypherResult r;
    r.status = CpaStatusToVesalStatusCode(status);
    r.ctx = req_meta->user_ctx;
    req_ring_queue_->PushResult(r, req_meta->ring_queue_id);

    bool is_encrypt = req_meta->op == CypherOp::kEncrypt;
    auto& throughput = is_encrypt ? encrypt_throughput_ : decrypt_throughput_;
    auto& counters = is_encrypt ? encrypt_rps_counters_ : decrypt_rps_counters_;
    auto& e2e = is_encrypt ? encrypt_e2e_latency_ : decrypt_e2e_latency_;
    if (IsOk(r.status)) {
        throughput->Add(req_meta->consumed_len);
    }
    counters[static_cast<int>(r.status)]->Add(1);
    e2e->Set(TimeStamp::DurationToNs(TimeStamp::Now() - req_meta->begin_time));
}

CypherReqMeta* QatCypherChannel::MatchFlightReq(req_id_t req_id) {
    CypherReqMeta* req_meta = in_flight_resource_->request_meta_pool_->Get(req_id);
    if (VESAL_UNLIKELY(!req_meta->valid)) {
        return nullptr;
    }
    VESAL_DCHECK(req_meta->req_id == req_id);
    return req_meta;
}

CypherReqMeta* QatCypherChannel::MatchAndInvalidateFlightReq(req_id_t req_id) {
    CypherReqMeta* req_meta = in_flight_resource_->request_meta_pool_->Get(req_id);
    if (VESAL_UNLIKELY(!req_meta->valid)) {
        return nullptr;
    }
    VESAL_DCHECK(req_meta->req_id == req_id);
    req_meta->valid = false;
    return req_meta;
}

void CyCallback(void* pCallbackTag,
                CpaStatus status,
                const CpaCySymOp op_type,
                void* pOpData,
                CpaBufferList* pDstBuffer,
                CpaBoolean verify_result) {
    bool do_measure = IsEnableSampling();
    auto start_time = do_measure ? TimeStamp::Now() : 0;
    auto* cb_ctx = reinterpret_cast<CypherCbContext*>(pCallbackTag);
    VESAL_LOG(DEBUG) << "Callback for req_id=" << cb_ctx->req_id;
    QatCypherChannel* channel = cb_ctx->channel;
    VESAL_CHECK(channel);
    channel->in_qat_num_--;
    CypherReqMeta* req_meta = channel->MatchFlightReq(cb_ctx->req_id);
    auto clean_up_guard = defer([&channel, req_meta, &cb_ctx, &start_time, &do_measure]() {
        channel->CleanUpReqData(req_meta, cb_ctx);
        if (VESAL_LIKELY(req_meta)) {
            DoMeasureIfNeed(do_measure,
                            req_meta->op == CypherOp::kEncrypt
                                ? channel->encrypt_postprocess_latency_.get()
                                : channel->decrypt_postprocess_latency_.get(),
                            start_time);
        }
    });
    VESAL_LOG(DEBUG) << "Callback for req_id=" << cb_ctx->req_id << ", status = " << status;
    // Only push result if the request is still valid, otherwise drop it.
    if (VESAL_LIKELY(req_meta)) {
        cb_ctx->dst_buf->DecompCopyBackIfNecessary(cb_ctx->dst_buf->GetDstDataLen());
        channel->PushResult(status, req_meta);
    }
}

void QatCypherChannel::CleanUpReqData(CypherReqMeta* req_meta, CypherCbContext* cb_ctx) {
    if (req_meta && req_meta->valid) {
        req_meta->valid = false;
        timer_manager_->RemoveTimer(req_meta->timer_ctx);
    }
    if (cb_ctx) {
        cb_ctx->Reset();
    }
}

CypherCbContext* QatCypherChannel::ReplaceNewRequestCbContext(req_id_t req_id) {
    // TODO(Pinnong.li): use better way to allocate memory instead of new and delete
    auto* u = new CypherCbContext;
    auto* old = in_flight_resource_->request_cb_context_pool_->Replace(req_id, u);
    u->channel = old->channel;
    return old;
}

void QatCypherChannel::InterceptInflightReq(CypherReqMeta* req_meta, const StatusCode& dummy_code) {
    auto* matched_req_meta = MatchAndInvalidateFlightReq(req_meta->req_id);
    VESAL_DCHECK(matched_req_meta && matched_req_meta == req_meta);
    CypherResult r;
    r.status = dummy_code;
    r.ctx = req_meta->user_ctx;
    req_ring_queue_->PushResult(r, req_meta->ring_queue_id);
    auto* old_cb_ctx = ReplaceNewRequestCbContext(req_meta->req_id);
    VESAL_DCHECK(old_cb_ctx);
    discarded_cb_ctxs_.push_back(old_cb_ctx);
}

}  // namespace qat
}  // namespace vesal