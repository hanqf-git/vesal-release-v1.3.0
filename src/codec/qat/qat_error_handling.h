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

#pragma once

#include <string>

#include "vesal/log_setting.h"
#include "vesal/status.h"

extern "C" {
#include "cpa.h"
#include "cpa_dc.h"
}

namespace vesal {
namespace qat {

inline StatusCode CpaDcReqStatusToVesalStatusCode(const CpaDcReqStatus& req_status) {
    switch (req_status) {
    case CPA_DC_OK:
        return StatusCode::kOk;
    // Here CPA_DC_SOFTERR, as per the QAT driver doc should we retry. But in practice we found
    // there is chance that the QAT keeps returning CPA_DC_SOFTERR error. So we treat it as resource
    // busy, HA will handle it if happens too frequently.
    case CPA_DC_SOFTERR:
    case CPA_DC_MAX_RESUBITERR:
    case CPA_DC_LZ4_MAX_BLOCK_SIZE_EXCEEDED:
        return StatusCode::kResourceBusy;
    case CPA_DC_OVERFLOW:
        return StatusCode::kOverflow;
    case CPA_DC_VERIFY_ERROR:
        return StatusCode::kChannelError;
    case CPA_DC_INVALID_BLOCK_TYPE:
    case CPA_DC_BAD_STORED_BLOCK_LEN:
    case CPA_DC_TOO_MANY_CODES:
    case CPA_DC_INCOMPLETE_CODE_LENS:
    case CPA_DC_REPEATED_LENS:
    case CPA_DC_MORE_REPEAT:
    case CPA_DC_BAD_LITLEN_CODES:
    case CPA_DC_BAD_DIST_CODES:
    case CPA_DC_INVALID_CODE:
    case CPA_DC_INVALID_DIST:
    case CPA_DC_LZ4_BLOCK_OVERFLOW_ERR:
    case CPA_DC_LZ4_TOKEN_IS_ZERO_ERR:
    case CPA_DC_LZ4_DISTANCE_OUT_OF_RANGE_ERR:
        return StatusCode::kBadData;
    case CPA_DC_FATALERR:
        return StatusCode::kHardwareError;
    case CPA_DC_INCOMPLETE_FILE_ERR:
        return StatusCode::kInvalidArgument;
    case CPA_DC_WDOG_TIMER_ERR:
        return StatusCode::kTimeout;
    default:
        break;
    }
    return StatusCode::kUnknown;
}

inline StatusCode CpaStatusToVesalStatusCode(CpaStatus cpa_status) {
    switch (cpa_status) {
    case CPA_STATUS_SUCCESS:
        return StatusCode::kOk;
    case CPA_STATUS_RESOURCE:
        return StatusCode::kResourceBusy;
    case CPA_STATUS_RESTARTING:
    case CPA_STATUS_RETRY:
        return StatusCode::kShouldRetry;
    case CPA_STATUS_INVALID_PARAM:
        return StatusCode::kInvalidArgument;
    case CPA_STATUS_UNSUPPORTED:
        return StatusCode::kNotSupported;
    case CPA_STATUS_FAIL:
        return StatusCode::kChannelError;
    case CPA_STATUS_FATAL:
        return StatusCode::kHardwareError;
    default:
        break;
    }
    // unknown system error:
    return StatusCode::kUnknown;
}

// Use CpaDcReqStatus as primary as it conveys more information, such as overflow case. While
// CPA_DC_OVERFLOW is shown in CpaDcReqStatus, CpaStatus is still CPA_STATUS_SUCCESS.
inline Status CpaStatusToVesalStatus(CpaStatus cpa_status, const std::string& msg) {
    return StatusCodeToStatus(CpaStatusToVesalStatusCode(cpa_status), msg);
}

inline StatusCode CpaStatusToVesalStatusCode(CpaStatus cpa_status, CpaDcReqStatus req_status) {
    if (VESAL_UNLIKELY(req_status != CPA_DC_OK)) {
        return CpaDcReqStatusToVesalStatusCode(req_status);
    }
    return CpaStatusToVesalStatusCode(cpa_status);
}

inline Status CpaStatusToVesalStatus(CpaStatus cpa_status,
                                     CpaDcReqStatus req_status,
                                     const std::string& msg) {
    return StatusCodeToStatus(CpaStatusToVesalStatusCode(cpa_status, req_status),
                              msg + std::string("CpaDcReqStatus=") + std::to_string(req_status));
}

}  // namespace qat
}  // namespace vesal
