/*
 * Copyright (c) 2024 ByteDance Inc.
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

#include "codec/qat/qat_error_handling.h"

#include <gtest/gtest.h>

#include "vesal/status.h"

namespace vesal {
namespace qat {

TEST(QatErrorHandlingTest, CpaStatusToVesalStatusTest) {
    CpaStatus cpa_status = CPA_STATUS_SUCCESS;
    EXPECT_TRUE(CpaStatusToVesalStatus(cpa_status, "").ok());
    cpa_status = CPA_STATUS_RESOURCE;
    EXPECT_TRUE(IsResourceBusy(CpaStatusToVesalStatus(cpa_status, "")));
    cpa_status = CPA_STATUS_RESTARTING;
    EXPECT_TRUE(IsShouldRetry(CpaStatusToVesalStatus(cpa_status, "")));
    cpa_status = CPA_STATUS_RETRY;
    EXPECT_TRUE(IsShouldRetry(CpaStatusToVesalStatus(cpa_status, "")));
    cpa_status = CPA_STATUS_INVALID_PARAM;
    EXPECT_TRUE(IsInvalidArgument(CpaStatusToVesalStatus(cpa_status, "")));
    cpa_status = CPA_STATUS_UNSUPPORTED;
    EXPECT_TRUE(IsNotSupported(CpaStatusToVesalStatus(cpa_status, "")));
    cpa_status = CPA_STATUS_FAIL;
    EXPECT_TRUE(IsChannelError(CpaStatusToVesalStatus(cpa_status, "")));
    cpa_status = CPA_STATUS_FATAL;
    EXPECT_TRUE(IsHardwareError(CpaStatusToVesalStatus(cpa_status, "")));
    cpa_status = -100;
    EXPECT_TRUE(IsUnknown(CpaStatusToVesalStatus(cpa_status, "")));
}

TEST(QatErrorHandlingTest, CpaReqStatusToVesalStatusTest) {
    CpaStatus cpa_status = CPA_STATUS_SUCCESS;
    CpaDcReqStatus req_status = CPA_DC_OK;
    EXPECT_TRUE(CpaStatusToVesalStatus(cpa_status, req_status, "").ok());
    req_status = CPA_DC_OVERFLOW;
    EXPECT_TRUE(IsOverflow(CpaStatusToVesalStatus(cpa_status, req_status, "")));
    req_status = CPA_DC_INVALID_BLOCK_TYPE;
    EXPECT_TRUE(IsBadData(CpaStatusToVesalStatus(cpa_status, req_status, "")));
}

};  // namespace qat
};  // namespace vesal
