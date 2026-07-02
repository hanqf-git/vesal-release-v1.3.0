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

#include <gtest/gtest.h>
#include <cstddef>
#include <memory>
#include <vector>

#include "data_flow/dsa/schedule_strategy.h"
#include "data_flow/dsa/work_queue_info.h"

namespace vesal {

class DsaScheduleStrategyTest : public ::testing::Test {
public:
    void SetUp() override {
        EXPECT_EQ(0, wq_num % 2);
        for (int i = 0; i < wq_num; ++i) {
            auto info = std::make_unique<data_flow::WorkQueueInfo>();
            info->dsa_index = i;
            // numa0: dsa0,dsa1; numa1: dsa2,dsa3
            info->uio_info.numa_node = i / (wq_num / 2);
            info->capacity = 128;
            info->size = 0;
            wq_list.push_back(std::move(info));
        }
    }
    int wq_num = 4;
    std::vector<std::unique_ptr<data_flow::WorkQueueInfo>> wq_list;
};

TEST_F(DsaScheduleStrategyTest, RoundRobinStrategyTest) {
    size_t schedule_hint = 0;
    data_flow::RoundRobinStrategy strategy;
    strategy.Init(&wq_list);
    std::vector<data_flow::WorkQueueInfo*> expected;
    int test_num = wq_num * 2;
    int j = data_flow::RoundRobinStrategy::tls_counter_;
    for (int i = 0; i < test_num; ++i) {
        expected.push_back(wq_list[j % wq_num].get());
        ++j;
    }
    for (int i = 0; i < test_num; ++i) {
        EXPECT_EQ(expected[i], strategy.GetNext(schedule_hint));
    }
}

TEST_F(DsaScheduleStrategyTest, DeviceAwareStrategy) {
    size_t schedule_hint = 1;
    data_flow::DeviceAwareStrategy strategy;
    strategy.Init(&wq_list);
    std::vector<data_flow::WorkQueueInfo*> expected;
    int test_num = wq_num * 2;
    for (int i = 0; i < test_num; ++i) {
        expected.push_back(wq_list[schedule_hint].get());
    }
    for (int i = 0; i < test_num; ++i) {
        EXPECT_EQ(expected[i], strategy.GetNext(schedule_hint));
    }
}

TEST_F(DsaScheduleStrategyTest, NumaAwareStrategy) {
    size_t schedule_hint = 0;
    data_flow::NumaAwareStrategy strategy;
    strategy.Init(&wq_list);
    std::vector<data_flow::WorkQueueInfo*> expected;
    int test_num = wq_num * 2;
    int j = data_flow::NumaAwareStrategy::tls_counter_by_numa_[0];
    for (int i = 0; i < test_num; ++i) {
        expected.push_back(wq_list[j % (wq_num / 2)].get());
        ++j;
    }
    for (int i = 0; i < test_num; ++i) {
        EXPECT_EQ(expected[i], strategy.GetNext(schedule_hint));
    }
}

}  // namespace vesal