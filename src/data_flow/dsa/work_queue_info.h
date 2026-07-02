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
#include <atomic>
#include "vesal/metrics.h"
extern "C" {
#include "uio_wq_info.h"
}

namespace vesal {
namespace data_flow {

// hard coded dsa properties
static constexpr int32_t kMaxNumaNum = 32;
static constexpr uint32_t kWorkQueueCapacity = 128;

struct WorkQueueInfo {
    // wq info from dsa-uio-config
    UioWqInfo uio_info;
    // dsa index
    uint64_t dsa_index;
    // wq's capacity config
    uint64_t capacity;
    // WIP requests num
    std::atomic<uint64_t> size;
    std::shared_ptr<Gauge> in_dsa_num;
};

}  // namespace data_flow
}  // namespace vesal