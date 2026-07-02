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

#include <cstdint>
#include <functional>
#include <vector>

namespace vesal {

// Timeout context for per compress or decompress request.
struct TimeoutContext {
    uint64_t onfired_ts;
    // timer_arg is from user to identify each TimeoutContext. Should not be touched druing the
    // process.
    void* timer_arg;
    TimeoutContext* next = nullptr;
    TimeoutContext* prev = nullptr;

    void Reset() {
        next = nullptr;
        prev = nullptr;
    }
};

class TimerManager {
public:
    TimerManager(std::function<void(void*)> timeout_cb, uint64_t timeout_ts)
        : timeout_cb_(std::move(timeout_cb)),
          timeout_ts_(timeout_ts),
          head_(nullptr),
          tail_(nullptr) {}

    // Add one timer in the tail
    TimeoutContext* AddTimer(void* timer_arg);

    // remove one timer
    // NOTE: `ctx' MUST in the timeout list
    void RemoveTimer(TimeoutContext* ctx);

    void HandleTimeout();

    // Clear and return all the timer_arg
    std::vector<void*> ClearAndGetUserArg();

private:
    // called for every timeout
    std::function<void(void*)> timeout_cb_;
    uint64_t timeout_ts_;
    TimeoutContext* head_;
    TimeoutContext* tail_;
};

}  // namespace vesal