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

#include "vesal/codec.h"

#include <gtest/gtest.h>

#include "codec/codec_internal.h"
#include "common/qat/qat_util.h"
#include "vesal/memory_pool.h"

namespace vesal {

TEST(CodecChannelOptionTest, DefaultHaPolicyIsNone) {
    CodecChannelOption opts;
    EXPECT_EQ(opts.ha_policy, HaPolicy::kNone);
}

TEST(CodecBuildTest, Basic) {
    MemoryPool::GetInstance()->Init();
    qat::QatUserStart();
    CodecInitOptions opts;
    opts.init_qat = true;
    auto init_r = Codec::Init(opts);
    EXPECT_TRUE(init_r);
    EXPECT_NE(g_qat_codec, nullptr);
    EXPECT_NE(g_sw_codec, nullptr);
    EXPECT_TRUE(Codec::Uninit());
    qat::QatUserStop();
}

}  // namespace vesal
