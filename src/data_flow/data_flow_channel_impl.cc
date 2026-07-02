/*
 * Copyright (c) 2024 ByteDance Inc.
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

#include "data_flow/data_flow_channel_impl.h"

#include <list>

#include "common/metrics_internal.h"
#include "common/object_pool.h"
#include "common/timer_manager.h"
#include "common/timestamp.h"
#include "data_flow/data_flow_engines.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {
namespace data_flow {

DataFlowChannelImpl::DataFlowChannelImpl(DataFlowEngine* engine, const DataFlowChannelOptions& opts)
    : engine_(engine), opts_(opts) {
    timer_manager_ = std::make_unique<TimerManager>(
        [](void* timer_arg) {
            DataFlowRequest* req = reinterpret_cast<DataFlowRequest*>(timer_arg);
            req->expired = true;
        },
        TimeStamp::MsToDuration(opts.timeout_ms));
}

StatusCode DataFlowChannelImpl::Init(uint32_t channel_id) {
    for (size_t i = 0; i < kIoDepthMaximum; ++i) {
        // the cached resource will stay alive until channel close
        // no need to mess with tls object pool
        DataFlowRequest* req = new DataFlowRequest;
        auto result = engine_->PreallocateCtx(req);
        if (result != StatusCode::kOk) {
            delete req;
            for (DataFlowRequest* req : cached_resource_) {
                engine_->ClearCtx(req);
                delete req;
            }
            cached_resource_.clear();
            return result;
        }
        cached_resource_.push_back(req);
    }
    channel_id_ = channel_id;
    std::vector<Tag> tags = {
        std::make_pair("engine", opts_.engine_type == DataFlowEngineType::kSoftware ? "sw" : "dsa"),
        std::make_pair("channel", std::to_string(channel_id_))};
    move_throughput_ = g_metric_registry->RegisterCounter("vesal.data_flow.move_throughput", tags);
    crc_throughput_ = g_metric_registry->RegisterCounter("vesal.data_flow.crc_throughput", tags);

    request_failure_counter_ =
        g_metric_registry->RegisterCounter("vesal.data_flow.request_failure_counter", tags);
    submit_failure_counter_ =
        g_metric_registry->RegisterCounter("vesal.data_flow.submit_failure_counter", tags);
    request_timeout_counter_ =
        g_metric_registry->RegisterCounter("vesal.data_flow.request_timeout_counter", tags);

    preprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.data_flow.preprocess_latency", tags);
    postprocess_latency_ =
        g_metric_registry->RegisterHistogram("vesal.data_flow.postprocess_latency", tags);

    wip_request_num_ =
        g_metric_registry->RegisterGauge("vesal.data_flow.channel_wip_req_num", tags);
    return StatusCode::kOk;
}

template <typename T> std::string CArrayToString(T elements[], size_t num) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < num; ++i) {
        oss << elements[i];
        if (i + 1 != num)
            oss << ", ";
    }
    oss << "]";
    return oss.str();
}

StatusCode DataFlowChannelImpl::SubmitMove(DataFlowMoveOperation ops[],
                                           size_t num,
                                           void* user_ctx) {
    VESAL_LOG(DEBUG) << "SubmitMove() begins, num=" << num << ", user_ctx=" << user_ctx
                     << ", ops[]=" << CArrayToString(ops, num);
    auto begin_time = TimeStamp::Now();
    bool do_measure = IsEnableSampling();
    DURATION_TO_RETURN(do_measure, preprocess_latency_.get(), begin_time);
    if (VESAL_UNLIKELY(num > kMaxSubmittedBatchSize)) {
        VESAL_LOG(ERROR) << "Invalid input, op num=" << num
                         << ", kMaxSubmittedBatchSize=" << kMaxSubmittedBatchSize;
        return StatusCode::kInvalidArgument;
    }
    uint64_t move_data_size = 0;
    uint64_t crc_data_size = 0;
    uint64_t segment_num = 0;
    for (size_t i = 0; i < num; ++i) {
        const auto& op = ops[i];
        size_t op_src_size = op.src.size();
        segment_num += op_src_size;
        if (VESAL_UNLIKELY(op_src_size == 0 || op_src_size != op.src_len.size() ||
                           op_src_size > kMaxSrcSegmentNum)) {
            VESAL_LOG(ERROR) << "Invalid input, i=" << i << ", src.size()=" << op_src_size
                             << ", src_len.size()=" << op.src_len.size();
            return StatusCode::kInvalidArgument;
        }
        uint64_t total_length = 0;
        for (size_t j = 0; j < op_src_size; ++j) {
            if (VESAL_UNLIKELY(op.src_len[j] == 0)) {
                VESAL_LOG(ERROR) << "Invalid input, i=" << i << ", j=" << j
                                 << ", src_len[j]=" << op.src_len[j];
                return StatusCode::kInvalidArgument;
            }
            total_length += op.src_len[j];
        }
        if (VESAL_UNLIKELY(total_length > kMaxTotalLength)) {
            VESAL_LOG(ERROR) << "Invalid input, total length=" << total_length
                             << ", kMaxTotalLength=" << kMaxTotalLength;
            return StatusCode::kInvalidArgument;
        }
        move_data_size += total_length;
        crc_data_size += op.enable_crc * total_length;
    }
    // max descriptor num case:
    // [1]*(batch desc) + [segment_num]*(copy desc) +
    // [1]*(fence desc) + [batch_num]*(crc desc)
    if (VESAL_UNLIKELY(1 + segment_num + 1 + num > kMaxInternalBatchSize)) {
        VESAL_LOG(ERROR) << "Invalid input exceeding internal batch size, segment_num="
                         << segment_num << ", batch_num=" << num
                         << ", kMaxInternalBatchSize=" << kMaxInternalBatchSize;
        return StatusCode::kInvalidArgument;
    }
    return SubmitRequest(ops, num, move_data_size, crc_data_size, user_ctx);
}

StatusCode DataFlowChannelImpl::SubmitCrc(DataFlowCrcOperation ops[], size_t num, void* user_ctx) {
    VESAL_LOG(DEBUG) << "SubmitCrc() begins, num=" << num << ", user_ctx=" << user_ctx
                     << ", ops[]=" << CArrayToString(ops, num);
    auto begin_time = TimeStamp::Now();
    bool do_measure = IsEnableSampling();
    DURATION_TO_RETURN(do_measure, preprocess_latency_.get(), begin_time);
    if (VESAL_UNLIKELY(num > kMaxSubmittedBatchSize)) {
        VESAL_LOG(ERROR) << "Invalid input, op num=" << num
                         << ", kMaxSubmittedBatchSize=" << kMaxSubmittedBatchSize;
        return StatusCode::kInvalidArgument;
    }
    // max descriptor num case:
    // [1]*(fence desc) + [batch_num]*(crc desc)
    if (VESAL_UNLIKELY(1 + num > kMaxInternalBatchSize)) {
        VESAL_LOG(ERROR) << "Invalid input exceeding internal batch size, batch_num=" << num
                         << ", kMaxInternalBatchSize=" << kMaxInternalBatchSize;
        return StatusCode::kInvalidArgument;
    }
    uint64_t crc_data_size = 0;
    for (size_t i = 0; i < num; ++i) {
        const auto& op = ops[i];
        if (VESAL_UNLIKELY(op.len > kMaxTotalLength || op.len == 0)) {
            VESAL_LOG(ERROR) << "Invalid input, i=" << i << ", len=" << op.len
                             << ", kMaxTotalLength=" << kMaxTotalLength;
            return StatusCode::kInvalidArgument;
        }
        crc_data_size += op.len;
    }
    return SubmitRequest(ops, num, /*move_data_size=*/0, crc_data_size, user_ctx);
}

ssize_t DataFlowChannelImpl::Poll(DataFlowResult results[], unsigned int max_num, int timeout) {
    VESAL_LOG(DEBUG) << "Poll() begins, max_num=" << max_num
                     << ", results=" << CArrayToString(results, max_num);
    size_t result_num = 0;
    bool do_measure = !FLAGS_vesal_metrics_disable_poller_metrics && IsEnableSampling();
    uint64_t postprocess_begin_time = 0;
    for (auto it = queue_.begin(); it != queue_.end() && result_num < max_num;) {
        // "timeout" case is not related to engines
        // all related logic in data flow channel layer
        if (VESAL_UNLIKELY((*it)->expired)) {
            expired_queue_.push_back(*it);
            results[result_num].status = StatusCode::kTimeout;
            results[result_num].ctx = (*it)->user_ctx;
            it = queue_.erase(it);
            ++result_num;
            request_timeout_counter_->Add(1);
            wip_request_num_->Set(queue_.size());
            continue;
        }
        if (do_measure) {
            postprocess_begin_time = TimeStamp::Now();
        }
        engine_->CheckCompletion(*it, &results[result_num]);
        if ((*it)->completed) {
            VESAL_LOG(DEBUG) << **it;
            if (VESAL_UNLIKELY(results[result_num].status != StatusCode::kOk)) {
                VESAL_LOG(ERROR) << "request failed, status=" << results[result_num].status;
                request_failure_counter_->Add(1);
            }
            timer_manager_->RemoveTimer((*it)->timer_ctx);
            cached_resource_.push_back(*it);
            move_throughput_->Add((*it)->move_data_size);
            crc_throughput_->Add((*it)->crc_data_size);
            it = queue_.erase(it);
            wip_request_num_->Set(queue_.size());
            ++result_num;
            if (do_measure) {
                postprocess_latency_->Set(vesal::TimeStamp::DurationToNs(vesal::TimeStamp::Now() -
                                                                         (postprocess_begin_time)));
            }
        } else {
            if (opts_.order_type == DataFlowPollOrderType::kOrdered)
                break;
            else
                ++it;
        }
    }
    // check finished result first, then check if timeout
    timer_manager_->HandleTimeout();
    if (VESAL_UNLIKELY(!expired_queue_.empty())) {
        for (auto it = expired_queue_.begin(); it != expired_queue_.end();) {
            DataFlowResult discarded_result;
            engine_->CheckCompletion(*it, &discarded_result);
            if ((*it)->completed) {
                cached_resource_.push_back(*it);
                it = expired_queue_.erase(it);
            } else {
                ++it;
            }
        }
    }
    VESAL_LOG(DEBUG) << "Poll() ends, result_num=" << result_num
                     << ", results=" << CArrayToString(results, result_num);
    return result_num;
}

void DataFlowChannelImpl::PollDiscardedResult() {
    DataFlowResult discarded_result;
    for (auto it = queue_.begin(); it != queue_.end();) {
        engine_->CheckCompletion(*it, &discarded_result);
        if ((*it)->completed) {
            timer_manager_->RemoveTimer((*it)->timer_ctx);
            cached_resource_.push_back(*it);
            it = queue_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = expired_queue_.begin(); it != expired_queue_.end();) {
        engine_->CheckCompletion(*it, &discarded_result);
        if ((*it)->completed) {
            cached_resource_.push_back(*it);
            it = expired_queue_.erase(it);
        } else {
            ++it;
        }
    }
}

StatusCode DataFlowChannelImpl::SubmitRequest(DataFlowOperationBase* ops,
                                              size_t num,
                                              uint64_t move_data_size,
                                              uint64_t crc_data_size,
                                              void* user_ctx) {
    if (VESAL_UNLIKELY(cached_resource_.empty())) {
        VESAL_LOG(WARN) << "channel busy";
        return StatusCode::kResourceBusy;
    }
    DataFlowRequest* req = cached_resource_.front();
    cached_resource_.pop_front();
    req->Build(ops, num, user_ctx, opts_.schedule_hint, move_data_size, crc_data_size);
    StatusCode result = engine_->Submit(req);
    if (VESAL_LIKELY(result == StatusCode::kOk)) {
        req->timer_ctx = timer_manager_->AddTimer(req);
        queue_.push_back(req);
        wip_request_num_->Set(queue_.size());
    } else {
        submit_failure_counter_->Add(1);
        cached_resource_.push_back(req);
    }
    // set op to nullptr after submission
    // to avoid any attempt to access op later, because user might free it
    req->op = nullptr;
    return result;
}

StatusCode DataFlowChannelImpl::Close() {
    PollDiscardedResult();
    if (!queue_.empty() || !expired_queue_.empty()) {
        VESAL_LOG(WARN) << "Failed to close channel due to WIP requests";
        return StatusCode::kResourceBusy;
    }
    for (DataFlowRequest* req : cached_resource_) {
        engine_->ClearCtx(req);
        delete req;
    }
    return StatusCode::kOk;
}

}  // namespace data_flow
}  // namespace vesal
