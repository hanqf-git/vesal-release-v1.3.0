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
#include <cstddef>

#include "vesal/log_setting.h"

namespace vesal {

constexpr size_t kCacheLineSize = 64;

// The queue has following properties:
//   Producers are wait-free (one atomic exchange per enqueue)
//   Consumer is
//     - lock-free
//     - mostly wait-free, except when consumer reaches the end of the queue
//       and producer enqueued a new node, but did not update the next pointer
//       on the old node, yet
template <typename T> class MPSCQueue {
private:
    template <typename E> struct Node {
    public:
        explicit Node(E* elem = nullptr) {
            if (elem) {
                element = *elem;
            }
            next.store(nullptr, std::memory_order_relaxed);
        }
        E element;
        std::atomic<Node<E>*> next;
    };

public:
    MPSCQueue() {
        tail_ = new Node<T>();
        head_.store(tail_);
        size_ = 0;
    }

    ~MPSCQueue() {
        T dummy;
        while (Pop(&dummy)) {
        }

        delete tail_;
    }

    // Multi producer safe.
    size_t Push(const T& elem) {
        T* element = const_cast<T*>(&elem);
        // A `nullptr` is used to denote an empty queue when doing a
        // `dequeue()` so producers can't use it as an element.
        VESAL_DCHECK(element != nullptr);
        auto newNode = new Node<T>(element);
        auto oldhead = head_.exchange(newNode, std::memory_order_acq_rel);

        // At this point if this thread context switches out we may block
        // the consumer from doing a dequeue (see below). Eventually we'll
        // unblock the consumer once we run again and execute the next
        // line of code.
        oldhead->next.store(newNode, std::memory_order_release);

        return size_.fetch_add(1, std::memory_order_relaxed);
    }

    // Single consumer only.
    bool Pop(T* elem) {
        auto currentTail = tail_;

        // Check and see if there is an actual element linked from `tail`
        // since we use `tail` as a "stub" rather than the actual element.
        auto nextTail = currentTail->next.load(std::memory_order_acquire);

        // There are three possible cases here:
        //
        // (1) The queue is empty.
        // (2) The queue appears empty but a producer is still enqueuing
        //     so let's wait for it and then dequeue.
        // (3) We have something to dequeue.
        //
        // Start by checking if the queue is or appears empty.
        if (nextTail == nullptr) {
            // Now check if the queue is actually empty or just appears
            // empty. If it's actually empty then return `nullptr` to denote
            // emptiness.
            if (head_.load(std::memory_order_relaxed) == tail_) {
                return false;
            }

            // Another thread already inserted a new node, but did not
            // connect it to the tail, yet, so we spin-wait. At this point
            // we are not wait-free anymore.
            do {
                nextTail = currentTail->next.load(std::memory_order_acquire);
            } while (nextTail == nullptr);
        }

        VESAL_DCHECK(nextTail != nullptr);

        // Set next pointer of current tail to null to disconnect it
        // from the queue.
        currentTail->next.store(nullptr, std::memory_order_release);

        *elem = std::move(nextTail->element);

        tail_ = nextTail;
        delete currentTail;

        size_.fetch_sub(1, std::memory_order_relaxed);
        return true;
    }

    size_t Size() const {
        return size_.load(std::memory_order_relaxed);
    }

    // Single consumer only.
    bool Empty() const {
        return tail_->next.load(std::memory_order_relaxed) == nullptr &&
               head_.load(std::memory_order_relaxed) == tail_;
    }

private:
    alignas(kCacheLineSize) std::atomic<Node<T>*> head_;
    alignas(kCacheLineSize) Node<T>* tail_;
    alignas(kCacheLineSize) std::atomic<size_t> size_;
};

}  // namespace vesal
