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

#pragma once

#include <deque>

#include "common/timestamp.h"
#include "vesal/codec.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

namespace vesal {
namespace qat {

// clang-format off
// We separate the vesal status into three levels:
// kNotHandle: this type of errors will not affect the HA, we return as it is. 
// kAccumulate: this type of errors will be recorded. If the error rate is higher than the threshold, HA should be trigger. 
// kImmediately: this type of errors will trigger the HA immediately.
// clang-format on
enum class QatHaErrorLevel : uint8_t { kNotHandle = 1, kAccumulate, kImmediately, kNum };

inline QatHaErrorLevel JudgeHaErrorLevel(const StatusCode& status_code) {
    switch (status_code) {
    case StatusCode::kOk:
    case StatusCode::kDropped:
    case StatusCode::kInvalidArgument:
    case StatusCode::kNotSupported:
    case StatusCode::kOverflow:
    case StatusCode::kBadData:
    case StatusCode::kPermanentError:
        return QatHaErrorLevel::kNotHandle;
    case StatusCode::kResourceBusy:
    case StatusCode::kTimeout:
    case StatusCode::kSliceHang:
    case StatusCode::kShouldRetry:
        return QatHaErrorLevel::kAccumulate;
    case StatusCode::kHardwareError:
    case StatusCode::kChannelError:
    case StatusCode::kUnknown:
        return QatHaErrorLevel::kImmediately;
    }
    VESAL_LOG(CRITICAL) << "Should not reach here.";
    return QatHaErrorLevel::kNotHandle;
}

inline uint64_t SecToUs(uint64_t sec) {
    return sec * 1000 * 1000;
}

static const int kVesalQatHaMaxErrorNum = 1024;

// Very light-weight sliding window of error statistics for Ha purpose. It's a time window which has
// the size of vesal_qat_ha_sliding_time_window_sec. If FLAGS_vesal_qat_ha_trigger_error_num errors
// happened in the window error should be triggered. Also if multiple errors happens in a short
// period(min_counting_time_window_us_), they will be seen as one error.
class SlidingTimeWindow {
public:
    SlidingTimeWindow()
        : min_counting_time_window_us_(FLAGS_vesal_qat_ha_min_counting_time_window_us),
          sliding_window_us_(SecToUs(FLAGS_vesal_qat_ha_sliding_time_window_sec)) {
        if (FLAGS_vesal_qat_ha_trigger_error_num > kVesalQatHaMaxErrorNum) {
            VESAL_LOG(WARN) << "vesal_qat_ha_trigger_error_num should be less than "
                            << kVesalQatHaMaxErrorNum << ", will set to " << kVesalQatHaMaxErrorNum;
            FLAGS_vesal_qat_ha_trigger_error_num = kVesalQatHaMaxErrorNum;
        }
    }

    ~SlidingTimeWindow() {
        Reset();
    }

    void Reset() {
        error_ts_.clear();
    }

    // Return true means no need to trigger fallback.
    bool RecordErrorAndCheckIsSafe() {
        auto now_ts = TimeStamp::Now();
        if (NeedRecord(now_ts)) {
            // Need to record a new error.
            error_ts_.push_back(now_ts);
            // Clear expired errors. Only need to clear the expired errors when adding new record.
            // Because other cases won't trigger the fallback so we don't care if the error_num_ is
            // accurate at the current state.
            ClearExpiredErrors(now_ts);
            VESAL_LOG(DEBUG)
                << "Add error, current error_num=" << GetErrorNum()
                << " after cleared expired errors, FLAGS_vesal_qat_ha_trigger_error_num="
                << FLAGS_vesal_qat_ha_trigger_error_num;
        }
        return GetErrorNum() < FLAGS_vesal_qat_ha_trigger_error_num;
    }

    uint32_t GetErrorNum() const {
        return error_ts_.size();
    }

private:
    void ClearExpiredErrors(uint64_t now) {
        while (!error_ts_.empty() &&
               TimeStamp::DurationToUs(now - error_ts_.front()) > sliding_window_us_) {
            error_ts_.pop_front();
        }
    }

    bool NeedRecord(uint64_t now) {
        return error_ts_.empty() ||
               TimeStamp::DurationToUs(now - error_ts_.back()) > min_counting_time_window_us_;
    }

    uint64_t min_counting_time_window_us_;
    size_t sliding_window_us_;
    // Record the timestamps of errors happened in the sliding window. We only
    // need to record the errors.
    std::deque<int64_t> error_ts_;
};

class QatHa {
public:
    QatHa(HaPolicy ha_policy)
        : ha_policy_(ha_policy), sliding_time_window_(std::make_unique<SlidingTimeWindow>()) {}

    // The caller should call this to check if the fallback is needed, and handle the fallback
    // itself. Because this class does not hold the channel/qat resources.
    bool ShouldFallback(const StatusCode& status_code);

    bool IsHaEnabled() const {
        // TODO(sjj): implement software fallback
        return ha_policy_ == HaPolicy::kHardware;
    }

private:
    HaPolicy ha_policy_;
    std::unique_ptr<SlidingTimeWindow> sliding_time_window_;
};

}  // namespace qat
}  // namespace vesal
