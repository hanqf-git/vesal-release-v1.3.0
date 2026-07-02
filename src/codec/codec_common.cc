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

#include "codec/codec_common.h"

namespace vesal {

static const unsigned int kMaxSrcLen = 1 << 31;

StatusCode ValidateSglArgs(const std::vector<unsigned char*>& src,
                           const std::vector<unsigned int>& src_len) {
    uint64_t total_len = 0;
    for (auto len : src_len) {
        if (len == 0) {
            VESAL_LOG(ERROR) << "invalid src_len: 0";
            return StatusCode::kInvalidArgument;
        }
        total_len += len;
    }
    if (total_len > kMaxSrcLen || total_len == 0) {
        VESAL_LOG(ERROR) << "total src_len exceeds limit, total_len=" << total_len
                         << ", limit=" << kMaxSrcLen;
        return StatusCode::kInvalidArgument;
    }
    if (src.size() != src_len.size()) {
        VESAL_LOG(ERROR) << "vector src and vector src_len should have same size, vector src size="
                         << src.size() << ", vector src_len size=" << src_len.size();
        return StatusCode::kInvalidArgument;
    }
    if (src.size() > VESAL_MAX_SGL_NUM) {
        VESAL_LOG(ERROR) << "sgl size exceeds limit, src.size()=" << src.size()
                         << ", exceeds limit " << VESAL_MAX_SGL_NUM;
        return StatusCode::kInvalidArgument;
    }
    return StatusCode::kOk;
}

}  // namespace vesal
