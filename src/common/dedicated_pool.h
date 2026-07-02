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

#include <cstddef>
#include <functional>
#include <memory>

#include "vesal/log_setting.h"

namespace vesal {

inline bool is_power_of_two(size_t n) {
    return (n != 0) && ((n & (n - 1)) == 0);
}

inline size_t next_power_of_two(size_t n) {
    if (is_power_of_two(n))
        return n;
    return 0x8000000000000000 >> (__builtin_clzl(n) - 1);
}

template <typename T> struct DedicatedPool {
    const size_t slot_num_;
    const size_t slot_mask_;
    std::unique_ptr<std::unique_ptr<T>[]> pool_;
    DedicatedPool(size_t slot_num) : slot_num_(slot_num), slot_mask_(slot_num_ - 1) {
        VESAL_CHECK(slot_num > 0 && !(slot_num & (slot_num - 1)))
            << "pool slot_num must greater than 0 and, greater or equal to kMaxQatflightNum and, "
               "is of power of 2, slot_num_="
            << slot_num;
        pool_ = std::make_unique<std::unique_ptr<T>[]>(slot_num_);
        for (size_t i = 0; i < slot_num; ++i) {
            pool_[i] = std::make_unique<T>();
        }
    }
    void ForEach(std::function<void(T*)> func) {
        for (size_t i = 0; i < slot_num_; ++i) {
            func(pool_[i].get());
        }
    }
    T* Get(int64_t key) const {
        return pool_[key & slot_mask_].get();
    }
    T* Replace(int64_t key, T* new_obj) {
        VESAL_DCHECK(new_obj);
        auto* old_obj = pool_[key & slot_mask_].release();
        pool_[key & slot_mask_].reset(new_obj);
        return old_obj;
    }
    void Reset() {
        for (size_t i = 0; i < slot_num_; ++i) {
            pool_[i] = std::make_unique<T>();
        }
    }
};

}  // namespace vesal