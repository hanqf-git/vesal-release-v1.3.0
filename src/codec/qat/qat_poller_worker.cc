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

#include "qat_poller_worker.h"

#include "codec/codec_internal.h"
#include "codec/qat/qat_codec_engine.h"
#include "codec/qat/qat_codec_shared_channel.h"
#include "common/metrics_internal.h"
#include "common/object_pool.h"
#include "common/scheduler.h"

namespace vesal {
namespace qat {

// Used by QatPollerWorker to handle the callback of each shared channel.
inline void SharedChannelQueueFinCallback(WorkerTask* task, int64_t now) {
    auto* shared_channel = task->req.shared_channel;
    if (VESAL_UNLIKELY(shared_channel == nullptr)) {
        return;
    }
    auto lat_ns = TimeStamp::DurationToNs(now - task->req.start_ts);
    shared_channel->SendQueueLatencyMetric(task->req.direction, lat_ns);
}

// Used by QatPollerWorker to handle the callback of each shared channel.
inline void SharedChannelE2ECallback(WorkerTask* task, int64_t now) {
    auto* shared_channel = task->req.shared_channel;
    if (VESAL_UNLIKELY(shared_channel == nullptr)) {
        return;
    }
    auto lat_us = TimeStamp::DurationToUs(now - task->req.start_ts);
    shared_channel->SendE2ELatencyMetric(task->req.direction, lat_us);
}

StatusCode QatPollerWorker::Init(const CodecChannelOption& opts, int poller_id) {
    auto qat_codec_engine_r = CreateQatCodecEngine(opts);
    if (!qat_codec_engine_r.first.ok()) {
        VESAL_LOG(ERROR) << "Create QatCodecEngine failed: " << qat_codec_engine_r.first;
        return qat_codec_engine_r.first.code();
    }
    worker_ = std::move(qat_codec_engine_r.second);
    opts_ = opts;
    poller_id_ = poller_id;
    TagList common_list = {{"poller_id", std::to_string(poller_id_)}};
    metric_poller_worker_queue_num_ =
        g_metric_registry->RegisterGauge("vesal.poller_worker_queue_num", common_list);
    metric_poller_worker_inflight_num_ =
        g_metric_registry->RegisterGauge("vesal.poller_worker_inflight_num", common_list);
    metric_poller_worker_drained_num_ =
        g_metric_registry->RegisterHistogram("vesal.poller_worker_drained_num", common_list);
    metric_poller_worker_user_cb_latency_ =
        g_metric_registry->RegisterHistogram("vesal.poller_worker_user_cb_latency", common_list);
    periodic_task_id_ = g_periodic_scheduler.AddPeriodicTask(
        [&]() {
            metric_poller_worker_queue_num_->Set(worker_tasks_.Size());
            metric_poller_worker_inflight_num_->Set(inflight_num_);
        },
        std::chrono::milliseconds(1000));
    running_.store(true);
    return StatusCode::kOk;
}

void QatPollerWorker::Uninit() {
    // Join current tasks, set running to false so no new task can be added.
    running_.store(false);
    while (HasUnFinishedReqs()) {
        LoopOnce(kMaxSubmitNum, kMaxPollingNum);
    }
    g_periodic_scheduler.CompleteTask(periodic_task_id_);
    auto r = worker_->Close();
    if (!r.ok()) {
        VESAL_LOG(ERROR) << "Close codec channel failed: " << r;
    }
    worker_.reset();
}

StatusCode QatPollerWorker::Schedule(const std::vector<unsigned char*>& src,
                                     const std::vector<unsigned int>& src_len,
                                     unsigned char* dst,
                                     unsigned int dst_len,
                                     void* user_ctx,
                                     const CodecDirection& direction,
                                     UserCallback user_cb,
                                     QatCodecSharedChannel* shared_channel,
                                     int64_t start_ts) {
    // TODO(sjj): move the new to the channel and use cache instead.
    if (!running_.load(std::memory_order_relaxed)) {
        // Note: memory_order_relaxed is safe because operations on worker_tasks_ are never
        // rescheduled before here.
        return StatusCode::kResourceBusy;
    }
    auto* task = new WorkerTask;
    task->req.src = src;
    task->req.src_len = src_len;
    task->req.dst = dst;
    task->req.dst_len = dst_len;
    task->req.user_ctx = user_ctx;
    task->req.direction = direction;
    task->req.user_cb = user_cb;
    task->req.start_ts = start_ts;
    task->req.shared_channel = shared_channel;
    worker_tasks_.Push(task);
    return StatusCode::kOk;
}

void QatPollerWorker::LoopOnce(size_t submit_num, size_t polling_num) {
    size_t num = DrainTasks(submit_num);
    HandleQueueFinCb(num);
    metric_poller_worker_drained_num_->Set(num);
    // Submit the requests.
    for (size_t i = 0; i < num; ++i) {
        auto* task = task_buffer_[i];
        task->req_id = GetNextReqId();
        CodecDirection dir = task->req.direction;
        StatusCode submit_r = worker_->SubmitAsyncRequest(
            dir, task->req.src, task->req.src_len, task->req.dst, task->req.dst_len, task);
        if (submit_r != StatusCode::kOk) {
            task->req.submit_r = submit_r;
            submit_failed_tasks_.push(task);
        } else {
            ++inflight_num_;
        }
    }
    PollingAndMerge(polling_num);
}

void QatPollerWorker::HandleQueueFinCb(size_t num) {
    if (num == 0) {
        return;
    }
    auto now = TimeStamp::Now();
    for (size_t i = 0; i < num; ++i) {
        auto* task = task_buffer_[i];
        SharedChannelQueueFinCallback(task, now);
    }
}

bool QatPollerWorker::HandleUserCallback(UserCallback user_cb, const CodecResult& result) {
    if (user_cb == nullptr) {
        return false;
    }
    int64_t start_ts = TimeStamp::Now();
    user_cb(result);
    metric_poller_worker_user_cb_latency_->Set(
        TimeStamp::DurationToNs(TimeStamp::Now() - start_ts));
    return true;
}

size_t QatPollerWorker::DrainTasks(size_t task_num) {
    task_num = std::min(task_num, kMaxSubmitNum);
    size_t num = 0;
    WorkerTask* task = nullptr;
    while (num < task_num && worker_tasks_.Pop(&task)) {
        task_buffer_[num++] = task;
    }
    return num;
}

ssize_t QatPollerWorker::PollingAndMerge(size_t polling_num) {
    const ssize_t poll_result_n = worker_->Poll(result_buffer_, kMaxSubmitNum, 0);
    size_t submit_failed_handled_num = 0;
    if (poll_result_n <= 0) {
        // No requests ready, or the polling is failed. Drain out submit failed requets if any. Need
        // to return them first before returning the polling result.
        while (TryProcessOneFailedTask()) {
            ++submit_failed_handled_num;
        }
        return submit_failed_handled_num == 0 ? poll_result_n : submit_failed_handled_num;
    }
    // We have some requests ready. Merge the result buffer with the submit failed tasks. Some
    // failed requests might be left in the submit_failed_tasks_ queue, we will get them in next
    // run.
    auto now = TimeStamp::Now();
    for (auto i = 0; i < poll_result_n; ++i) {
        auto* task = static_cast<WorkerTask*>(result_buffer_[i].ctx);
        while (TryProcessOneFailedTask()) {
            ++submit_failed_handled_num;
        }
        SharedChannelE2ECallback(task, now);
        result_buffer_[i].ctx = task->req.user_ctx;
        UserCallback user_cb = task->req.user_cb;
        HandleUserCallback(user_cb, result_buffer_[i]);
        bool matched = MatchedExpReqId(task->req_id);
        // Worker ensures the order already so here just use debug check.
        VESAL_DCHECK(matched) << "Out of order req_id: " << task->req_id;
        --inflight_num_;
        delete task;
    }
    return poll_result_n + submit_failed_handled_num;
}

bool QatPollerWorker::TryProcessOneFailedTask() {
    if (submit_failed_tasks_.empty()) {
        return false;
    }
    auto* failed_task = submit_failed_tasks_.front();
    if (!MatchedExpReqId(failed_task->req_id)) {
        return false;
    }
    submit_failed_tasks_.pop();
    auto now = TimeStamp::Now();
    SharedChannelE2ECallback(failed_task, now);
    CodecResult failed_r;
    failed_r.status = failed_task->req.submit_r;
    failed_r.ctx = failed_task->req.user_ctx;
    UserCallback user_cb = failed_task->req.user_cb;
    HandleUserCallback(user_cb, failed_r);
    delete failed_task;
    return true;
}

}  // namespace qat
}  // namespace vesal
