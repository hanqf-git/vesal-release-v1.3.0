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

#include "traffic_limiter.h"

#include <thread>

void TrafficLimiter::Start(uint32_t limit_mbs) {
    started_ = true;
    expected_finished_time_ = std::chrono::high_resolution_clock::now();
    if (!limit_mbs)
        ns_per_bytes_ = -1;
    else
        ns_per_bytes_ = 1e9 / 1024.0 / 1024.0 / limit_mbs;
}

void TrafficLimiter::Stop() {
    started_ = false;
}

void TrafficLimiter::WaitUntil(uint64_t traffic_size_bytes) {
    if (ns_per_bytes_ < 0)
        return;
    uint64_t duraiton_ns = uint64_t(traffic_size_bytes * ns_per_bytes_);
    expected_finished_time_ += std::chrono::nanoseconds(duraiton_ns);
    std::this_thread::sleep_until(expected_finished_time_);
}