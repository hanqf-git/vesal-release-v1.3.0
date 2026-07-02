/*
 * Copyright (c) 2023 ByteDance Inc.
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
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace vesal {

struct PeriodicTask {
    std::function<void()> task;
    std::chrono::steady_clock::time_point next_run_time;
    std::chrono::milliseconds interval;
    uint32_t id;

    // Comparator for the priority queue.
    bool operator>(const PeriodicTask& other) const {
        return next_run_time > other.next_run_time;
    }
};

class Scheduler {
public:
    Scheduler() : running_(false) {}

    ~Scheduler() {
        Stop();
        Clear();
    }
    // Adds a new periodic task to the scheduler, starting from now + interval.
    uint32_t AddPeriodicTask(std::function<void()> task, std::chrono::milliseconds interval);
    // Starts the scheduler thread, can only be called once.
    void Start();
    // Stops the scheduler and joins the thread.
    void Stop();
    // Clear all resources.
    void Clear();
    // Complete a task and remove it from scheduler
    void CompleteTask(uint32_t id);

private:
    std::priority_queue<PeriodicTask, std::vector<PeriodicTask>, std::greater<PeriodicTask>> tasks_;
    std::thread scheduler_thread_;
    std::mutex tasks_mutex_;
    std::condition_variable wait_condition_;
    std::atomic<bool> running_;
    uint32_t id_cnt_ = 0;
    std::unordered_map<uint32_t, bool> task_status_;
};

extern Scheduler g_periodic_scheduler;

}  // namespace vesal