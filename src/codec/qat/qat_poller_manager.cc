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

#include "codec/qat/qat_poller_manager.h"

#include "gflags/gflags.h"

DEFINE_uint32(vesal_codec_qat_shared_mode_poller_num,
              2,
              "Number of QAT pollers. 2 should be enough to drive 4 QATs.");

static bool VesalQatPollerSleepTimeValidator(const char* name, uint32_t value) {
    return value <= vesal::qat::kMaxPollerSleepTimeUs;
}
DEFINE_uint32(vesal_codec_qat_shared_mode_poller_sleep_time_us,
              0,
              "Sleep interval between each loop for QatPoller. Bigger value means less CPU usage "
              "and higher latency. Should not bigger than 30 seconds. Can adjust during runtime.");
DEFINE_validator(vesal_codec_qat_shared_mode_poller_sleep_time_us,
                 &VesalQatPollerSleepTimeValidator);

namespace vesal {
namespace qat {

// Here assumes the Stop() not blocked because this level has no way to handle the failure. The
// real workers inside the pollers ensure that the close(cleaning QAT) is not blocked.
void QatPollerManager::Reset() {
    for (auto& p : pollers_) {
        p->Shutdown();
    }
    pollers_.clear();
}

QatPollerWorker* QatPollerManager::GetWorker(const CodecChannelOption& option) {
    std::lock_guard<std::mutex> lk(mtx_);
    // TODO(sjj): Can share strategy with DataFlow.
    auto id = GetNextPollerId();
    auto* p = pollers_[id]->GetOrNewWorker(option);
    if (p == nullptr) {
        return nullptr;
    }
    poller_by_worker_map_[p] = pollers_[id].get();
    return p;
}

void QatPollerManager::ReturnWorker(QatPollerWorker* worker) {
    if (worker == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = poller_by_worker_map_.find(worker);
    if (it == poller_by_worker_map_.end()) {
        VESAL_LOG(WARN) << "Tried to return a non-existing worker. worker: " << worker
                        << ", option: " << worker->GetOption();
        return;
    }
    QatPoller* p = it->second;
    if (p->ReturnWorker(worker)) {
        poller_by_worker_map_.erase(it);
    }
}

void QatPollerManager::Init() {
    for (size_t i = 0; i < FLAGS_vesal_codec_qat_shared_mode_poller_num; ++i) {
        auto p = std::make_unique<QatPoller>(i);
        // Start() should not fail because it only starts the threads, no task is sent.
        p->Start();
        pollers_.push_back(std::move(p));
    }
}

}  // namespace qat
}  // namespace vesal
