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
#include "codec/qat/qat_ha.h"

#include "gflags/gflags.h"

DEFINE_uint32(
    vesal_qat_ha_min_counting_time_window_us,
    50,
    "If multiple errors happens within this time window, they will be seen as one error.");
DEFINE_uint32(vesal_qat_ha_sliding_time_window_sec,
              1 * 60,
              "The sliding time window size in seconds. Default is 1 minutes.");

static bool VesalQatHaTriggerErrorNumValidator(const char* name, uint32_t value) {
    return value <= vesal::qat::kVesalQatHaMaxErrorNum;
}
// Note vesal_qat_ha_trigger_error_num flag is directly used in the code so we can modify it in
// runtime. And for performance reason we don't use lock to protect it, but this should not cause
// trouble in our case.
DEFINE_uint32(
    vesal_qat_ha_trigger_error_num,
    5,
    "For accumulate error, like timeout in result, if more than this number of error happen within "
    "the vesal_qat_ha_sliding_time_window_sec window, fallback will be triggered. Max number is "
    "1024. If more than 1024, it will be set to 1024.");
DEFINE_validator(vesal_qat_ha_trigger_error_num, &VesalQatHaTriggerErrorNumValidator);

namespace vesal {
namespace qat {

bool QatHa::ShouldFallback(const StatusCode& status_code) {
    VESAL_DCHECK(IsHaEnabled()) << "Should not call this function if HA not enabled.";
    auto level = JudgeHaErrorLevel(status_code);
    if (level == QatHaErrorLevel::kNotHandle) {
        return false;
    }
    if (level == QatHaErrorLevel::kImmediately ||
        !sliding_time_window_->RecordErrorAndCheckIsSafe()) {
        VESAL_LOG(WARN) << "Trigger fallback, level=" << static_cast<int>(level)
                        << ", status_code=" << static_cast<int>(status_code)
                        << ", err_num=" << sliding_time_window_->GetErrorNum();
        sliding_time_window_->Reset();
        return true;
    }
    return false;
}

}  // namespace qat
}  // namespace vesal
