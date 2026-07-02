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

#include "scheduler.h"

#include <atomic>
#include <mutex>

#include "vesal/log_setting.h"

namespace vesal {

Scheduler g_periodic_scheduler;

uint32_t Scheduler::AddPeriodicTask(std::function<void()> task,
                                    std::chrono::milliseconds interval) {
    tasks_mutex_.lock();
    auto next_run_time = std::chrono::steady_clock::now() + interval;
    bool notify = tasks_.empty() || next_run_time < tasks_.top().next_run_time;
    uint32_t task_id = ++id_cnt_;
    tasks_.push({task, next_run_time, interval, task_id});
    task_status_[task_id] = true;
    tasks_mutex_.unlock();
    if (notify) {
        wait_condition_.notify_one();
    }
    return task_id;
}

void Scheduler::Start() {
    bool expect = false;
    if (running_.compare_exchange_strong(expect, true)) {
        scheduler_thread_ = std::thread([this]() {
            while (running_) {
                std::unique_lock<std::mutex> lock(tasks_mutex_);
                if (tasks_.empty()) {
                    wait_condition_.wait(lock);
                    continue;
                }
                auto next_task = tasks_.top();
                auto now = std::chrono::steady_clock::now();
                if (now >= next_task.next_run_time) {
                    auto task = next_task.task;
                    auto interval = next_task.interval;
                    auto id = next_task.id;
                    tasks_.pop();  // Remove the task before running it, to prevent overlap.
                    if (task_status_[id]) {
                        lock.unlock();
                        task();  // Run the task without holding the lock.
                        lock.lock();

                        // Reschedule the periodical task
                        tasks_.push({task, now + interval, interval, id});
                    } else {
                        // Task completed, remove it
                        task_status_.erase(id);
                    }
                } else {
                    wait_condition_.wait_until(lock, next_task.next_run_time);
                }
            }
        });
    }
}

void Scheduler::Stop() {
    if (running_) {
        running_ = false;
        wait_condition_.notify_all();
        if (scheduler_thread_.joinable()) {
            scheduler_thread_.join();
        }
    }
}

void Scheduler::Clear() {
    VESAL_CHECK(!running_) << "Scheduler is running, cannot reset.";
    while (tasks_.size()) {
        tasks_.pop();
    }
    task_status_.clear();
    id_cnt_ = 0;
}

void Scheduler::CompleteTask(uint32_t id) {
    std::unique_lock<std::mutex> guard(tasks_mutex_);
    if (task_status_.find(id) != task_status_.end())
        task_status_[id] = false;
}

}  // namespace vesal