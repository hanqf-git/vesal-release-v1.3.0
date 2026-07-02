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

#include <string>

namespace vesal {

std::string StatusCodeToString(StatusCode code) {
    switch (code) {
    case StatusCode::kOk:
        return "OK";
    case StatusCode::kInvalidArgument:
        return "INVALID_ARGUMENT";
    case StatusCode::kNotSupported:
        return "NOT_SUPPORTED";
    case StatusCode::kResourceBusy:
        return "RESOURCE_BUSY";
    case StatusCode::kHardwareError:
        return "HARDWARE_ERROR";
    case StatusCode::kChannelError:
        return "CHANNEL_ERROR";
    case StatusCode::kTimeout:
        return "TIMEOUT";
    case StatusCode::kUnknown:
        return "UNKNOWN";
    case StatusCode::kOverflow:
        return "OVERFLOW";
    case StatusCode::kBadData:
        return "BAD_DATE";
    case StatusCode::kShouldRetry:
        return "SHOULD_RETRY";
    case StatusCode::kDropped:
        return "DROPPED";
    case StatusCode::kPermanentError:
        return "PERMANENT_ERROR";
    case StatusCode::kSliceHang:
        return "SLICE_HANG";
    default:
        return "";
    }
}

}  // namespace vesal