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

#include "common/mpsc_queue.h"

#include <thread>

#include "gtest/gtest.h"
#include "vesal/log_setting.h"

namespace vesal {

struct Element {
    Element() = default;
    explicit Element(int v) : val(v) {}
    int val;
};

TEST(MPSCQueueTest, PushAndPop) {
    MPSCQueue<Element*> queue;
    auto* elem = new Element(1);
    queue.Push(elem);
    Element* e;
    ASSERT_TRUE(queue.Pop(&e));
    ASSERT_EQ(e->val, 1);
    delete elem;
}

TEST(MPSCQueueTest, PushAndPopMultiThread) {
    MPSCQueue<Element*> queue;
    std::vector<std::thread> threads;
    const int thread_num = 16;
    const int elem_num_each_thread = 512;
    int got = 0;
    for (int i = 0; i < thread_num; i++) {
        threads.emplace_back([&queue]() {
            for (int j = 0; j < elem_num_each_thread; j++) {
                auto* elem = new Element(j);
                queue.Push(elem);
            }
        });
    }
    while (got < thread_num * elem_num_each_thread) {
        Element* e = nullptr;
        if (queue.Pop(&e)) {
            got++;
            delete e;
            e = nullptr;
        }
    }
    EXPECT_EQ(queue.Size(), 0);
    for (auto& t : threads) {
        t.join();
    }
}

}  // namespace vesal
