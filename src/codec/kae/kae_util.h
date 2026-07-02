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

#pragma once

#include "vesal/status.h"
extern "C" {
#include "kaezip.h"
}

namespace vesal {
namespace kae {

inline StatusCode KaeStatusToStatusCode(int32_t kae_status) {
    switch (kae_status) {
    case KAE_ZLIB_SUCC:
        return StatusCode::kOk;
    case KAE_ZLIB_INVAL_PARA:
        return StatusCode::kInvalidArgument;
    case KAE_ZLIB_INIT_FAIL:
        return StatusCode::kChannelError;
    case KAE_ZLIB_ALLOC_FAIL:
        return StatusCode::kResourceBusy;
    case KAE_ZLIB_HW_TIMEOUT_FAIL:
        return StatusCode::kTimeout;
    case KAE_ZLIB_DST_BUF_OVERFLOW:
        return StatusCode::kOverflow;
    case KAE_ZLIB_COMP_FAIL:
    case KAE_ZLIB_SET_FAIL:
    case KAE_ZLIB_RELEASE_FAIL:
    default:
        return StatusCode::kUnknown;
    }
}

inline Status KaeStatusToStatus(int32_t kae_status, const std::string& msg) {
    return StatusCodeToStatus(KaeStatusToStatusCode(kae_status), msg);
}

}  // namespace kae
}  // namespace vesal