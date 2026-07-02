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

#include "common/defer.h"

#include <gtest/gtest.h>

namespace vesal {

TEST(TestDefer, DeferToCallFunction) {
    int a = 0;
    {
        auto gurad = defer([&a]() { a = 10; });
        EXPECT_EQ(a, 0);
    }
    EXPECT_EQ(a, 10);
}

}  // namespace vesal
