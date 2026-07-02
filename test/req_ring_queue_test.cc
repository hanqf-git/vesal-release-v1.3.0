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

#include "common/req_ring_queue.h"

#include <gtest/gtest.h>

namespace vesal {

TEST(ReqRingQueueTest, BasicTest) {
    const int N = 10;
    InflightReqRingQueue<int> q(N);
    // Expect id from 0 to N - 1
    for (int i = 0; i < N; i++) {
        EXPECT_FALSE(q.IsFull());
        int64_t id = q.NewReq();
        EXPECT_EQ(id, i);
    }
    // Expect queue is full, new req is denied
    EXPECT_TRUE(q.IsFull());
    EXPECT_EQ(q.GetSize(), N);
    int64_t id = q.NewReq();
    EXPECT_EQ(id, -1);
    // Push result reversely, expect pop results in correct order
    for (int i = N - 1; i >= 0; i--) {
        q.PushResult(i, i);
    }
    int results[N];
    int cnt = q.PopResults(results, N);
    EXPECT_EQ(cnt, N);
    for (int i = 0; i < N; i++) {
        EXPECT_EQ(results[i], i);
    }
    // Expect pop results is 0 when queue is empty
    cnt = q.PopResults(results, N);
    EXPECT_EQ(cnt, 0);
    // Expect new req is allowed after pop results
    for (int i = 0; i < N; i++) {
        EXPECT_NE(q.NewReq(), -1);
    }
    // Expect pop results is 0 when no result is pushed
    cnt = q.PopResults(results, N);
    EXPECT_EQ(cnt, 0);
}

}  // namespace vesal