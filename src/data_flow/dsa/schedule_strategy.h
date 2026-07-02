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
#include "data_flow/dsa/work_queue_info.h"
#include "vesal/data_flow.h"
#include "vesal/log_setting.h"

namespace vesal {
namespace data_flow {

class ScheduleStrategy {
public:
    virtual void Init(std::vector<std::unique_ptr<WorkQueueInfo>>* wq_list) = 0;
    virtual WorkQueueInfo* GetNext(size_t schedule_hint) = 0;
    virtual ~ScheduleStrategy() = default;
};

class DeviceAwareStrategy : public ScheduleStrategy {
public:
    void Init(std::vector<std::unique_ptr<WorkQueueInfo>>* wq_list) override {
        VESAL_CHECK(!wq_list->empty()) << "no work queue available";
        uint64_t dsa_index_max = 0;
        for (const auto& wq : *wq_list) {
            if (wq->capacity > 0 && wq->dsa_index > dsa_index_max)
                dsa_index_max = wq->dsa_index;
        }
        wq_list_by_dsa_index_.resize(dsa_index_max + 1, nullptr);
        for (const auto& wq : *wq_list) {
            if (wq->capacity > 0) {
                // currently, each dsa has only one work queue
                VESAL_CHECK(wq_list_by_dsa_index_[wq->dsa_index] == nullptr)
                    << "multiple work queue on same dsa";
                wq_list_by_dsa_index_[wq->dsa_index] = wq.get();
            }
        }
    }

    WorkQueueInfo* GetNext(size_t schedule_hint) override {
        if (VESAL_UNLIKELY(schedule_hint >= wq_list_by_dsa_index_.size())) {
            VESAL_LOG(WARN) << "no dsa found given dsa index hint: " << schedule_hint;
            return nullptr;
        }
        return wq_list_by_dsa_index_[schedule_hint];
    }

private:
    std::vector<WorkQueueInfo*> wq_list_by_dsa_index_;
};

class RoundRobinStrategy : public ScheduleStrategy {
public:
    void Init(std::vector<std::unique_ptr<WorkQueueInfo>>* wq_list) override {
        VESAL_CHECK(!wq_list->empty()) << "no work queue available";
        for (const auto& wq : *wq_list) {
            if (wq->capacity > 0) {
                wq_list_.push_back(wq.get());
            }
        }
    }
    WorkQueueInfo* GetNext(size_t schedule_hint) override {
        // round robin
        uint64_t choice = tls_counter_ % wq_list_.size();
        // if multiple channel on the same thread, they share the same counter
        // counter uint64_t => safe overflow
        ++tls_counter_;
        return wq_list_[choice];
    }

private:
    std::vector<WorkQueueInfo*> wq_list_;
    thread_local static uint64_t tls_counter_;
};

class NumaAwareStrategy : public ScheduleStrategy {
public:
    void Init(std::vector<std::unique_ptr<WorkQueueInfo>>* wq_list) override {
        VESAL_CHECK(!wq_list->empty()) << "no work queue available";
        for (const auto& wq : *wq_list) {
            if (wq->capacity > 0) {
                VESAL_CHECK(wq->uio_info.numa_node < kMaxNumaNum)
                    << "wrong numa node: " << wq->uio_info.numa_node;
                wq_list_by_numa_[wq->uio_info.numa_node].push_back(wq.get());
            }
        }
    }

    WorkQueueInfo* GetNext(size_t schedule_hint) override {
        uint64_t numa = schedule_hint;
        if (VESAL_UNLIKELY(numa >= kMaxNumaNum)) {
            VESAL_LOG(WARN) << "wrong numa hint: " << numa;
            return nullptr;
        }
        if (VESAL_UNLIKELY(wq_list_by_numa_[numa].empty())) {
            VESAL_LOG(WARN) << "no work queue on numa: " << numa;
            return nullptr;
        }
        // round robin
        uint64_t choice = tls_counter_by_numa_[numa] % wq_list_by_numa_[numa].size();
        // if multiple channel on the same thread, they share the same counter
        // counter uint64_t => safe overflow
        ++tls_counter_by_numa_[numa];
        return wq_list_by_numa_[numa][choice];
    }

private:
    // there might be multiple work queues on each numa
    std::vector<WorkQueueInfo*> wq_list_by_numa_[kMaxNumaNum];
    thread_local static uint64_t tls_counter_by_numa_[kMaxNumaNum];
};

}  // namespace data_flow
}  // namespace vesal