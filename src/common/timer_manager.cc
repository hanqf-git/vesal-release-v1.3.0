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

#include "common/timer_manager.h"

#include "common/object_pool.h"
#include "common/timestamp.h"

namespace vesal {

TimeoutContext* TimerManager::AddTimer(void* timer_arg) {
    TimeoutContext* ctx = vesal::get_tls_object<TimeoutContext>();
    ctx->onfired_ts = TimeStamp::Now() + timeout_ts_;
    ctx->timer_arg = timer_arg;
    if (!head_) {  // empty list
        head_ = ctx;
        tail_ = ctx;
    } else {
        // insert the new node in the tail
        tail_->next = ctx;
        ctx->prev = tail_;
        tail_ = ctx;
    }

    return ctx;
}

void TimerManager::RemoveTimer(TimeoutContext* ctx) {
    VESAL_CHECK(ctx);
    if ((ctx == head_) && (ctx == tail_)) {
        // remove the orphan node
        head_ = nullptr;
        tail_ = nullptr;
    } else if (ctx == head_) {
        // remove head node
        head_ = head_->next;
        head_->prev = nullptr;
    } else if (ctx == tail_) {
        // remove tail node
        tail_ = tail_->prev;
        tail_->next = nullptr;
    } else {
        // remove middle node
        ctx->prev->next = ctx->next;
        ctx->next->prev = ctx->prev;
    }
    ctx->Reset();
    vesal::return_tls_object<TimeoutContext>(ctx);
}

void TimerManager::HandleTimeout() {
    uint64_t now = TimeStamp::Now();
    while (head_) {
        if (now < head_->onfired_ts) {
            break;
        }
        // remove from the list
        auto* old_head = head_;
        head_ = head_->next;

        void* timer_arg = old_head->timer_arg;
        timeout_cb_(timer_arg);

        old_head->Reset();
        vesal::return_tls_object<TimeoutContext>(old_head);
    }
    if (head_) {
        head_->prev = nullptr;
    } else {  // empty list
        tail_ = head_;
    }
}

std::vector<void*> TimerManager::ClearAndGetUserArg() {
    std::vector<void*> ret;
    while (head_) {
        ret.push_back(head_->timer_arg);
        RemoveTimer(head_);
    }
    return ret;
}

}  // namespace vesal