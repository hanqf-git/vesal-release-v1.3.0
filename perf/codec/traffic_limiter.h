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

#include <chrono>

class TrafficLimiter {
public:
    void Start(uint32_t limit_mbs);
    void Stop();
    void WaitUntil(uint64_t traffic_size_bytes);

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> expected_finished_time_;
    bool started_ = false;
    double ns_per_bytes_;
};