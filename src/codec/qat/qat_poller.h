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

#include <cstddef>
#include <future>
#include <map>
#include <memory>
#include <thread>

#include "codec/qat/qat_poller_worker.h"
#include "common/mpsc_queue.h"
#include "vesal/codec.h"

namespace vesal {
namespace qat {

const size_t kMaxPollerOpProcessNum = 512;

enum class PollerOp : uint8_t { kGet, kReturn, kShutdown, kNum };

// For control plane.
struct PollerTask {
    PollerOp op;
    CodecChannelOption option;
    QatPollerWorker* worker;
    std::promise<QatPollerWorker*> get_r;
    std::promise<bool> return_r;

    PollerTask() {
        Reset();
    }

    void Reset() {
        op = PollerOp::kNum;
        option = CodecChannelOption();
        worker = nullptr;
        get_r = std::promise<QatPollerWorker*>();
        return_r = std::promise<bool>();
    }
};

struct QatPollerWorkerInfo {
    std::unique_ptr<QatPollerWorker> worker;
    size_t ref_cnt{0};
};

// An RTC poller. Contains several QatPollerWorkers. The typical usage is to call Start() and this
// will create a thread. The thread will be running in loop and checking the queue constantly for
// tasks.
// This class provides *SYNC* APIs to get/return an worker.
class QatPoller {
public:
    QatPoller(int poller_id) : poller_id_(poller_id) {
        task_buffer_ = std::make_unique<PollerTask*[]>(kMaxPollerOpProcessNum);
    }

    void Start();

    void Shutdown();

    // Called by user thread. Ask for an worker. If the worker is not found, create one.
    // Workers are distincted by CodecChannelOption. If two GetOrNewWorker() are called with the
    // same option, they will return the same worker. Return nullptr if failed.
    QatPollerWorker* GetOrNewWorker(const CodecChannelOption& option);

    // Called by user thread. Remove(or substract ref_cnt) an worker.
    // Return true means the entry is removed, otherwise only decrease the ref_cnt.
    bool ReturnWorker(QatPollerWorker* worker);

    size_t GetPollerId() const {
        return poller_id_;
    }

private:
    void Run();

    // Return nullptr if failed.
    QatPollerWorker* GetOrNewWorkerInternal(const CodecChannelOption& option);

    // Return true means the entry is removed, otherwise only decrease the ref_cnt.
    // Internally, it will match the worker with option, and decrease the ref_cnt. If the ref_cnt
    // is zero, the worker will be removed.
    bool ReturnWorkerInternal(QatPollerWorker* worker);

    void ShutdownInternal();

    size_t DrainTasks(size_t task_num);

    // Note this function is not thread-safe.
    size_t GetRefCnt() {
        size_t sum = 0;
        for (auto& it : workers_) {
            sum += it.second->ref_cnt;
        }
        return sum;
    }

    size_t GetWorkerNum() const {
        return workers_.size();
    }

    std::map<CodecChannelOption, std::unique_ptr<QatPollerWorkerInfo>> workers_;

    std::thread poller_thread_;
    MPSCQueue<PollerTask*> poller_tasks_;
    std::unique_ptr<PollerTask*[]> task_buffer_;

    int poller_id_{-1};
    bool running_{false};

    uint32_t periodic_task_id_;
    // How many workers this poller has.
    std::shared_ptr<Gauge> metric_poller_poller_worker_num_;
    // How many channels this poller is responsible for.
    std::shared_ptr<Gauge> metric_poller_channel_num_;
};

}  // namespace qat
}  // namespace vesal
