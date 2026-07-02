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

#include <atomic>
#include <queue>

#include "codec/codec_common.h"
#include "codec/qat/qat_codec_engine.h"
#include "common/mpsc_queue.h"
#include "vesal/types.h"

namespace vesal {
namespace qat {

// In one loop, we can submit up to kMaxSubmitNum requests.
const size_t kMaxSubmitNum = 128;
// In one loop, we can poll up to kMaxPollingNum requests.
const size_t kMaxPollingNum = 128;

class QatCodecSharedChannel;
struct WorkerTask {
    // For dc
    struct CodecRequest {
        std::vector<unsigned char*> src;
        std::vector<unsigned int> src_len;
        unsigned char* dst;
        unsigned int dst_len;
        void* user_ctx;
        UserCallback user_cb;
        CodecDirection direction;
        StatusCode submit_r;
        int64_t start_ts;
        QatCodecSharedChannel* shared_channel;
    } req;
    // req_id is assigned right before the submit, not when the request is enqueued. This is used
    // inside the entry to track the request.
    req_id_t req_id;
};

class QatPollerWorker {
public:
    StatusCode Init(const CodecChannelOption& opts, int poller_id);

    // TODO(sjj): Add the ref count, and only uninit when the ref count is zero.
    void Uninit();

    // Called by user thread. Add one task to the queue.
    StatusCode Schedule(const std::vector<unsigned char*>& src,
                        const std::vector<unsigned int>& src_len,
                        unsigned char* dst,
                        unsigned int dst_len,
                        void* user_ctx,
                        const CodecDirection& direction,
                        UserCallback user_cb = nullptr,
                        QatCodecSharedChannel* shared_channel = nullptr,
                        int64_t start_ts = 0);

    // Called by poller thread. Take out tasks from the queue and submit them into underlying
    // channel. Then do Polling once and process the user callback. If there are some failed tasks,
    // treat them as failed result and merge them into the result.
    void LoopOnce(size_t submit_num, size_t polling_num);

    const CodecChannelOption& GetOption() const {
        return opts_;
    }

    int GetPollerId() const {
        return poller_id_;
    }

    uint32_t GetEngineDeviceId() const {
        return worker_->GetQatUnit()->GetDeviceId();
    }

private:
    void HandleQueueFinCb(size_t num);

    bool HandleUserCallback(UserCallback user_cb, const CodecResult& result);

    size_t DrainTasks(size_t task_num);

    ssize_t PollingAndMerge(size_t polling_num);

    bool TryProcessOneFailedTask();

    bool HasUnFinishedReqs() {
        // Note worker_tasks_ is not thread safe but no tasks can be added so it's safe.
        VESAL_CHECK(!running_.load(std::memory_order_acquire));
        return inflight_num_ > 0 || !submit_failed_tasks_.empty() || !worker_tasks_.Empty();
    }

    req_id_t GetNextReqId() {
        return req_id_++;
    }

    bool MatchedExpReqId(req_id_t req_id) {
        if (req_id != expected_req_id_) {
            return false;
        }
        ++expected_req_id_;
        return true;
    }

    // This queue will be accessed by both worker and the user side channel
    MPSCQueue<WorkerTask*> worker_tasks_;
    // This queue is used inside worker to store the failed tasks. No need to lock.
    std::queue<WorkerTask*> submit_failed_tasks_;

    // TODO(sjj): make the buffer size configurable.
    WorkerTask* task_buffer_[kMaxSubmitNum];
    CodecResult result_buffer_[kMaxPollingNum];

    std::unique_ptr<QatCodecEngine> worker_;
    CodecChannelOption opts_;

    req_id_t req_id_{1};
    req_id_t expected_req_id_{1};
    // How many requests are submitted in the underlying.
    size_t inflight_num_{0};
    // Which poller this worker belongs to.
    int poller_id_{-1};

    // Used to intercept the requests after the worker is closed.
    std::atomic<bool> running_{false};

    uint32_t periodic_task_id_;
    // How many requests are queuing in worker_tasks_.
    std::shared_ptr<Gauge> metric_poller_worker_queue_num_;
    // How many requests are inflight in the underlying.
    std::shared_ptr<Gauge> metric_poller_worker_inflight_num_;
    // How many requests are consumed in each poller loop.
    std::shared_ptr<Histogram> metric_poller_worker_drained_num_;
    // Time cost of each user callback.
    std::shared_ptr<Histogram> metric_poller_worker_user_cb_latency_;
};

}  // namespace qat
}  // namespace vesal
