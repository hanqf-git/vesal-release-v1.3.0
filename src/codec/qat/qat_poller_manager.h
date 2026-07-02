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

#include <unordered_map>
#include "codec/qat/qat_poller.h"
#include "vesal/vesal.h"

namespace vesal {
namespace qat {

const size_t kMaxPollerSleepTimeUs = 30 * 1000 * 1000;  // 30s

class QatPollerManager {
public:
    static QatPollerManager* GetInstance() {
        // Only initialized once when the first time this function is called.
        static auto instance = std::make_unique<QatPollerManager>();
        return instance.get();
    }

    QatPollerManager() {
        Init();
    }

    ~QatPollerManager() {
        Reset();
    }

    // Need the Reset() function because we need to ensure the QatPollerManager is completely
    // shutdown before deresgitering to the QAT Driver.
    void Reset();

    // Thread-safe
    QatPollerWorker* GetWorker(const CodecChannelOption& option);
    // Thread-safe
    void ReturnWorker(QatPollerWorker* worker);

private:
    // Note not thread-safe
    size_t GetNextPollerId() {
        return next_poller_id_++ % pollers_.size();
    }

    // Used internal and convenient for testing because it's hard to destuct an singleton.
    void Init();

    std::vector<std::unique_ptr<QatPoller>> pollers_;
    // unordered_map only used to return the worker to the right poller.
    std::unordered_map<QatPollerWorker*, QatPoller*> poller_by_worker_map_;
    // This is not an IO path, mutex should be enough.
    std::mutex mtx_;
    size_t next_poller_id_{0};
};

}  // namespace qat
}  // namespace vesal
