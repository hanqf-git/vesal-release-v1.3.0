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

#include "vesal/status.h"

#include <gtest/gtest.h>

#include <sstream>

namespace vesal {

TEST(StatusTest, StreamTest) {
    Status ok(StatusCode::kOk, "should be ignored");
    std::ostringstream s;
    s << ok;
    EXPECT_EQ(s.str(), "OK");

    Status err(StatusCode::kUnknown, "unknown");
    std::ostringstream s2;
    s2 << err;
    EXPECT_EQ(s2.str(), "UNKNOWN: unknown");

    StatusCode code = StatusCode::kOk;
    std::ostringstream s3;
    s3 << code;
    EXPECT_EQ(s3.str(), "OK");
}

TEST(StatusTest, CheckTest) {
    EXPECT_TRUE(OkStatus().ok());
    EXPECT_FALSE(InvalidArgumentError("").ok());
    EXPECT_TRUE(IsInvalidArgument(InvalidArgumentError("")));
    EXPECT_TRUE(IsChannelError(ChannelError("")));
    EXPECT_TRUE(IsHardwareError(HardwareError("")));
    EXPECT_TRUE(IsResourceBusy(ResourceBusyError("")));
    EXPECT_TRUE(IsTimeout(TimeoutError("")));
    EXPECT_TRUE(IsNotSupported(NotSupportedError("")));
    EXPECT_TRUE(IsUnknown(UnknownError("")));
}

}  // namespace vesal
