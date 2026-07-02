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

#include "vesal/vesal.h"

#include "gtest/gtest.h"

namespace vesal {

TEST(VesalTest, Init) {
    InitOptions options;
    EXPECT_TRUE(Init(options));
    Uninit();
}

TEST(VesalTest, ReentryInit) {
    InitOptions opt;
    opt.mem_pool_init_opt.prealloc_page_size = HugePageSize::kUnknown;
    for (int i = 0; i < 10; ++i) {
        EXPECT_FALSE(Init(opt));
    }
    opt = {};
    opt.mem_pool_init_opt.prealloc_size_mb = 1024;
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(Init(opt));
    }
    Uninit();
}

TEST(VesalTest, Uninit) {
    // Expect Uninit to return true without init() called before
    EXPECT_TRUE(Uninit());
    // Expect Uninit to return true after failed init()
    InitOptions opt;
    opt.mem_pool_init_opt.prealloc_page_size = HugePageSize::kUnknown;
    EXPECT_FALSE(Init(opt));
    EXPECT_TRUE(Uninit());
    // Expect Uninit to return true after successful init() and be reentriable
    opt = {};
    opt.mem_pool_init_opt.prealloc_size_mb = 1024;
    EXPECT_TRUE(Init(opt));
    EXPECT_TRUE(Uninit());
    EXPECT_TRUE(Uninit());
}

TEST(VesalTest, MultiFailAndReinit) {
    InitOptions opt;
    opt.codec_init_opt.init_qat = false;
    opt.cypher_init_opt.init_qat = false;
    for (int i = 0; i < 64; ++i) {
        Init(opt);
        Uninit();
    }
}

}  // namespace vesal
