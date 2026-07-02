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

#include "common/scheduler.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <thread>

namespace vesal {

TEST(SchedulerTest, BasicApis) {
    Scheduler scheduler;
    int cnt_100 = 0, cnt_50 = 0;
    scheduler.Start();
    scheduler.AddPeriodicTask([&cnt_100]() { cnt_100++; }, std::chrono::milliseconds(100));
    scheduler.AddPeriodicTask([&cnt_50]() { cnt_50++; }, std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(580));
    scheduler.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(cnt_100, 5);
    EXPECT_EQ(cnt_50, 11);
}

TEST(SchedulerTest, CompleteTask) {
    Scheduler scheduler;
    int cnt_100 = 0, cnt_50 = 0;
    scheduler.Start();
    auto id1 =
        scheduler.AddPeriodicTask([&cnt_100]() { cnt_100++; }, std::chrono::milliseconds(100));
    auto id2 = scheduler.AddPeriodicTask([&cnt_50]() { cnt_50++; }, std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    scheduler.CompleteTask(id1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    scheduler.CompleteTask(id2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(cnt_100, 5);
    EXPECT_EQ(cnt_50, 12);
}

}  // namespace vesal