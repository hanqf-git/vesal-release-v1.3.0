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

#include "codec/dc_format.h"

#include <gtest/gtest.h>

#include "vesal/codec.h"

namespace vesal {

TEST(DcFormat, BasicFunction) {
    CodecAlgorithm algo = CodecAlgorithm::kLz4;
    char buf[1024] = {0};
    size_t size = 0;
    auto r = FrameFooterGen(algo, buf, &size);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(size, kLz4FrameFooterSize);
    r = FrameHeaderGen(algo, buf, &size);
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(size, kLz4FrameHeaderSize);
    algo = CodecAlgorithm::kNum;
    r = FrameHeaderGen(algo, buf, &size);
    EXPECT_FALSE(r.ok());
    r = FrameFooterGen(algo, buf, &size);

    std::string s1 = "abc";
    auto crc1 = ComputeCRC32(kCrc32cInitialValue, s1.c_str(), s1.size());
    std::string s2 = "qwertyuiop";
    auto crc2 = ComputeCRC32(kCrc32cInitialValue, s2.c_str(), s2.size());
    std::string s3 = "123456";
    auto crc3 = ComputeCRC32(kCrc32cInitialValue, s3.c_str(), s3.size());
    EXPECT_EQ(ComputeCRC32(kCrc32cInitialValue, (s1 + s2 + s3).c_str(), (s1 + s2 + s3).size()),
              CRC32Combine(~CRC32Combine(~crc1, crc2, s2.size()), crc3, s3.size()));

    size_t sz = 1024;
    std::string a(1024, '\0');
    char b[1024];
    for (size_t i = 0; i < sz; ++i) {
        a[i] = (i + 'a') % 256;
        b[i] = a[i];
    }
    EXPECT_EQ(XxHash(a.c_str(), sz), XxHash(b, sz));
}

}  // namespace vesal