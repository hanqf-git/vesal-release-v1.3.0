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

#include <utility>

namespace vesal {

namespace {

template <typename Func> struct deferred_action {
    explicit deferred_action(Func&& func) noexcept : func_(std::move(func)) {}
    ~deferred_action() {
        func_();
    }

private:
    Func func_;
};

}  // namespace

template <typename Func> inline deferred_action<Func> defer(Func&& func) {
    return deferred_action<Func>(std::forward<Func>(func));
}

}  // namespace vesal
