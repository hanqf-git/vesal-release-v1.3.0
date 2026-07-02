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

#include <cstdint>
#include <memory>

#include "vesal/log_setting.h"

namespace vesal {

template <typename Result> class InflightReqRingQueue {
    struct InflightReq {
        Result result;
        int64_t id = -1;
    };

public:
    InflightReqRingQueue(size_t size) : size_(size) {
        reqs_ = std::make_unique<InflightReq[]>(size);
    }

    bool IsFull() {
        return tail_id - head_id == size_;
    }

    int GetSize() {
        return tail_id - head_id;
    }

    int64_t NewReq() {
        if (VESAL_UNLIKELY(IsFull())) {
            return -1;
        }
        return tail_id++;
    }

    void PushResult(const Result& r, int64_t id) {
        int i = id % size_;
        reqs_[i].result = r;
        reqs_[i].id = id;
    }

    int PopResults(Result* results, int size) {
        int cnt = 0;
        int i = head_id % size_;
        while (head_id < tail_id && cnt < size) {
            if (reqs_[i].id < head_id)
                break;
            results[cnt++] = reqs_[i].result;
            head_id++;
            i = (i + 1) % size_;
        }
        return cnt;
    }

private:
    std::unique_ptr<InflightReq[]> reqs_;
    int size_;
    int64_t head_id = 0;
    int64_t tail_id = 0;
};

}  // namespace vesal