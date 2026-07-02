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

#include "codec/qat/qat_poller.h"

#include <unistd.h>

#include "common/metrics_internal.h"
#include "common/scheduler.h"
#include "gflags/gflags.h"
#include "vesal/vesal.h"

DEFINE_uint32(
    vesal_codec_qat_shared_mode_poller_op_process_num,
    16,
    "How many PollerOp can be handled by one poller in one polling. We don't want to "
    "create/destruct too many workers in a row and cause starvation on IO requests. If "
    "the number is bigger than 512, 512 will be used. Support modifying this flag in runtime.");
static bool VesalQatPollerPollerOpProcessNumValidator(const char* name, uint32_t value) {
    return value <= vesal::qat::kMaxPollerOpProcessNum;
}
DEFINE_validator(vesal_codec_qat_shared_mode_poller_op_process_num,
                 &VesalQatPollerPollerOpProcessNumValidator);

namespace vesal {
namespace qat {

// A poller can hold at most kMaxWorkerNumPerPoller workers.
const size_t kMaxWorkerNumPerPoller = 128;

// TODO(sjj): Use pool to reduce memory allocation.
inline PollerTask* NewPollerTask() {
    return new PollerTask();
}

inline void FreePollerTask(PollerTask* task) {
    delete task;
}

void QatPoller::Start() {
    if (running_) {
        return;
    }
    poller_thread_ = std::thread(&QatPoller::Run, this);
    VESAL_LOG(INFO) << "QatPoller started, poller_id: " << poller_id_;
    Tag poller_id_tag = {"poller_id", std::to_string(poller_id_)};
    metric_poller_poller_worker_num_ =
        g_metric_registry->RegisterGauge("qat_poller_poller_worker_num", {poller_id_tag});
    metric_poller_channel_num_ =
        g_metric_registry->RegisterGauge("qat_poller_channel_num", {poller_id_tag});
    periodic_task_id_ = g_periodic_scheduler.AddPeriodicTask(
        [&]() {
            metric_poller_poller_worker_num_->Set(GetWorkerNum());
            metric_poller_channel_num_->Set(GetRefCnt());
        },
        std::chrono::milliseconds(1000));
    running_ = true;
}

void QatPoller::Shutdown() {
    if (!running_) {
        return;
    }
    g_periodic_scheduler.CompleteTask(periodic_task_id_);
    auto* task = NewPollerTask();
    task->op = PollerOp::kShutdown;
    poller_tasks_.Push(task);
    poller_thread_.join();
    FreePollerTask(task);
    running_ = false;
}

QatPollerWorker* QatPoller::GetOrNewWorker(const CodecChannelOption& option) {
    if (!running_) {
        return nullptr;
    }
    auto* task = NewPollerTask();
    task->op = PollerOp::kGet;
    task->option = option;
    poller_tasks_.Push(task);
    auto* r = task->get_r.get_future().get();
    if (r == nullptr) {
        VESAL_LOG(ERROR) << "Failed to Get or New worker, option=" << option
                         << ", poller_id: " << poller_id_;
    } else {
        VESAL_LOG(INFO) << "Get worker, option=" << option << ", worker=" << r
                        << ", poller_id: " << poller_id_;
    }
    FreePollerTask(task);
    return r;
}

bool QatPoller::ReturnWorker(QatPollerWorker* worker) {
    if (!running_) {
        return false;
    }
    auto* task = NewPollerTask();
    task->op = PollerOp::kReturn;
    task->worker = worker;
    CodecChannelOption option = worker->GetOption();
    poller_tasks_.Push(task);
    auto r = task->return_r.get_future().get();
    VESAL_LOG(INFO) << "Returned worker, option=" << option << ", poller_id: " << poller_id_
                    << ", worker removed: " << r;
    FreePollerTask(task);
    return r;
}

void QatPoller::Run() {
    pthread_setname_np(pthread_self(), ("qat_poller_" + std::to_string(poller_id_)).c_str());
    while (true) {
        size_t num = DrainTasks(FLAGS_vesal_codec_qat_shared_mode_poller_op_process_num);
        for (size_t i = 0; i < num; ++i) {
            auto* task = task_buffer_[i];
            switch (task->op) {
            case PollerOp::kGet:
                task->get_r.set_value(GetOrNewWorkerInternal(task->option));
                break;
            case PollerOp::kReturn:
                task->return_r.set_value(ReturnWorkerInternal(task->worker));
                break;
            case PollerOp::kShutdown:
                ShutdownInternal();
                return;
            default:
                VESAL_DCHECK(false)
                    << "Unknown poller op=" << static_cast<int>(task->op) << ", ignored.";
            }
        }
        for (auto& kv : workers_) {
            auto* worker = kv.second->worker.get();
            worker->LoopOnce(kMaxSubmitNum, kMaxPollingNum);
        }
        // usleep(0) can be UB so just skip if zero.
        if (FLAGS_vesal_codec_qat_shared_mode_poller_sleep_time_us > 0) {
            usleep(FLAGS_vesal_codec_qat_shared_mode_poller_sleep_time_us);
        }
    }
}

QatPollerWorker* QatPoller::GetOrNewWorkerInternal(const CodecChannelOption& option) {
    // No need lock here because we are in QatPoller now.
    if (GetRefCnt() >= kMaxWorkerNumPerPoller) {
        return nullptr;
    }
    auto it = workers_.find(option);
    if (it != workers_.end()) {
        it->second->ref_cnt++;
        return it->second->worker.get();
    }
    // Need a new worker.
    auto worker_info = std::make_unique<QatPollerWorkerInfo>();
    worker_info->ref_cnt = 1;
    worker_info->worker = std::make_unique<QatPollerWorker>();
    auto create_r = worker_info->worker->Init(option, poller_id_);
    if (!IsOk(create_r)) {
        VESAL_LOG(ERROR) << "Failed to create codec channel, error=" << create_r
                         << ", poller_id: " << poller_id_;
        return nullptr;
    }
    workers_[option] = std::move(worker_info);
    return workers_[option]->worker.get();
}

bool QatPoller::ReturnWorkerInternal(QatPollerWorker* worker) {
    if (worker == nullptr) {
        return false;
    }
    const CodecChannelOption& option = worker->GetOption();
    // Note this is accessed by poller per se so no need lock.
    auto it = workers_.find(option);
    VESAL_DCHECK(it != workers_.end())
        << "Try to retrun a non-existing worker. worker: " << worker
        << ", poller id: " << poller_id_ << ", entries_.size(): " << workers_.size();
    if (it == workers_.end() || it->second->worker.get() != worker) {
        VESAL_LOG(WARN) << "Tried to return a non-existing worker. worker: " << worker
                        << ", option: " << worker->GetOption() << ", poller id: " << poller_id_
                        << ", entries_.size(): " << workers_.size();
        return false;
    }
    it->second->ref_cnt--;
    if (it->second->ref_cnt == 0) {
        it->second->worker->Uninit();
        VESAL_LOG(INFO) << "Removed worker, option: " << option << ", poller id: " << poller_id_;
        workers_.erase(it);
        return true;
    }
    return false;
}

void QatPoller::ShutdownInternal() {
    for (auto& kv : workers_) {
        kv.second->worker->Uninit();
    }
    // Uninit() ensures all inflight requests are finished. So we can safely exit.
    workers_.clear();
}

size_t QatPoller::DrainTasks(size_t task_num) {
    task_num = std::min(
        task_num, static_cast<size_t>(FLAGS_vesal_codec_qat_shared_mode_poller_op_process_num));
    size_t num = 0;
    PollerTask* task = nullptr;
    while (num < task_num && poller_tasks_.Pop(&task)) {
        task_buffer_[num++] = task;
    }
    return num;
}

}  // namespace qat
}  // namespace vesal
