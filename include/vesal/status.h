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

#include <cstdint>
#include <string>
#include <vector>

namespace vesal {

enum class StatusCode : uint8_t {
    // kOK is returned on success, prefer checking it with `vesl::Status::ok()` memeber function
    kOk = 0,

    // kInvalidArgument indicates the given argument is invalid, caller is recommended to correct
    // the arguments and retry
    kInvalidArgument = 1,

    // kNotSupported indicates the operation is not implemented or supported.
    kNotSupported = 2,

    // kResourceBusy indicates the resource is currently unavailable, caller can retry later
    kResourceBusy = 3,

    // kHardwareError indicates there's something wrong with the QAT hardware, caller is recommended
    // to use fallback software solutions
    kHardwareError = 4,

    // kChannelError indicates there's something wrong with the CodecChannel, caller is recommended
    // to use other channels or recreate a new one
    kChannelError = 5,

    // kTimeout indicates the operation is timed out
    kTimeout = 6,

    // kOverflow indicates the destination buffer is not enough for compress/decompress
    kOverflow = 7,

    // KBadData indicates the input data is corrupted, caller is recommended to check the input data
    kBadData = 8,

    // kShouldRetry means the operation should be retried. It's different from kResourceBusy as
    // kResourceBusy usually means the error is from the resource inefficiency(like no QAT
    // available). And this one simply means the operation should be retried, like a request is not
    // completed yet.
    kShouldRetry = 9,

    // kDropped indicates the operation/result is dropped. Typical cases are the HA triggered and
    // the inflight requests are dropped.
    kDropped = 10,

    // kPermanentError indicates the Channel is in fatal state and cannot be recovered, caller
    // should abandon the channel already.
    kPermanentError = 11,

    // kUnknown indicates an error that is no known recommended reactions, it's better to get human
    // involved and inspect the detailed message
    kUnknown = 12,

    // kSliceHang indicates QAT compression slice hang/watchdog timer error.
    kSliceHang = 13
};

inline std::vector<StatusCode> GetAllStatusCodes() {
    return {
        StatusCode::kOk,
        StatusCode::kInvalidArgument,
        StatusCode::kNotSupported,
        StatusCode::kResourceBusy,
        StatusCode::kHardwareError,
        StatusCode::kChannelError,
        StatusCode::kTimeout,
        StatusCode::kOverflow,
        StatusCode::kBadData,
        StatusCode::kShouldRetry,
        StatusCode::kDropped,
        StatusCode::kPermanentError,
        StatusCode::kUnknown,
        StatusCode::kSliceHang,
    };
}

inline bool IsStatusCode(int8_t code) {
    return code >= static_cast<int8_t>(StatusCode::kOk) &&
           code <= static_cast<int8_t>(StatusCode::kSliceHang);
}

// Call this to check if the buffer can be deallocated in zero copy mode. In some error cases like
// Timeout, user should hold the hanging buffer and let it leak because the hardware might be
// touching it. Only need to check for non-ok status.
inline bool CanDeallocZeroCopyBuffer(const StatusCode& status_code) {
    return status_code != StatusCode::kTimeout && status_code != StatusCode::kDropped &&
           status_code != StatusCode::kSliceHang;
}

// Returns the name for the status code, or "" for unknown value.
// OK
// INVALID_ARGUMENT
// NOT_SUPPORTED
// RESOURCE_BUSY
// HARDWARE_ERROR
// CHANNEL_ERROR
// TIMEOUT
// OVERFLOW
// BAD_DATA
// SHOULD_RETRY
// UNKNOWN
// SLICE_HANG
std::string StatusCodeToString(StatusCode code);

// Streams StatusCodeToString(code) to `os`.
inline std::ostream& operator<<(std::ostream& os, StatusCode code) {
    return os << StatusCodeToString(code);
}

class Status final {
public:
    // Creates an OK status without messages.
    // For constructing an OK status, prefer `vesal::OkStatus()`.
    Status() : code_(StatusCode::kOk) {}

    // Creates a status with the specified `vesal::StatusCode` and error message.
    Status(StatusCode code, const std::string& msg) : code_(code), message_(msg) {}

    // Returns `true` if `this->code()` == `vesal::StatusCode::kOk`.
    bool ok() const {
        return code_ == StatusCode::kOk;
    }

    // Returns the internal StatusCode of this status.
    StatusCode code() const {
        return code_;
    }

    // Returns the internal error message
    // prefer `operator<<` or `Status::ToString()` for debug logging.
    const char* message() const {
        return message_.c_str();
    }

    // Returns "OK" if ok(), otherwise "StatusCodeToString(code_): message_"
    std::string ToString() const {
        std::string s = StatusCodeToString(code_);
        return ok() ? s : s + ": " + message_;
    }

private:
    StatusCode code_;
    std::string message_;
};

// Returns an OK status.
inline Status OkStatus() {
    return Status();
}

// Streams x.ToString() to `os`
inline std::ostream& operator<<(std::ostream& os, const Status& x) {
    return os << x.ToString();
}

// Returns true if the `status` indicates a specified error
inline bool IsInvalidArgument(const Status& status) {
    return status.code() == StatusCode::kInvalidArgument;
}
inline bool IsNotSupported(const Status& status) {
    return status.code() == StatusCode::kNotSupported;
}
inline bool IsResourceBusy(const Status& status) {
    return status.code() == StatusCode::kResourceBusy;
}
inline bool IsHardwareError(const Status& status) {
    return status.code() == StatusCode::kHardwareError;
}
inline bool IsChannelError(const Status& status) {
    return status.code() == StatusCode::kChannelError;
}
inline bool IsTimeout(const Status& status) {
    return status.code() == StatusCode::kTimeout;
}
inline bool IsUnknown(const Status& status) {
    return status.code() == StatusCode::kUnknown;
}
inline bool IsOverflow(const Status& status) {
    return status.code() == StatusCode::kOverflow;
}
inline bool IsBadData(const Status& status) {
    return status.code() == StatusCode::kBadData;
}
inline bool IsShouldRetry(const Status& status) {
    return status.code() == StatusCode::kShouldRetry;
}
inline bool IsDropped(const Status& status) {
    return status.code() == StatusCode::kDropped;
}
inline bool IsPermanentError(const Status& status) {
    return status.code() == StatusCode::kPermanentError;
}
inline bool IsSliceHang(const Status& status) {
    return status.code() == StatusCode::kSliceHang;
}

inline bool IsOk(const StatusCode& status_code) {
    return status_code == StatusCode::kOk;
}
inline bool IsInvalidArgument(const StatusCode& status_code) {
    return status_code == StatusCode::kInvalidArgument;
}
inline bool IsNotSupported(const StatusCode& status_code) {
    return status_code == StatusCode::kNotSupported;
}
inline bool IsResourceBusy(const StatusCode& status_code) {
    return status_code == StatusCode::kResourceBusy;
}
inline bool IsHardwareError(const StatusCode& status_code) {
    return status_code == StatusCode::kHardwareError;
}
inline bool IsChannelError(const StatusCode& status_code) {
    return status_code == StatusCode::kChannelError;
}
inline bool IsTimeout(const StatusCode& status_code) {
    return status_code == StatusCode::kTimeout;
}
inline bool IsUnknown(const StatusCode& status_code) {
    return status_code == StatusCode::kUnknown;
}
inline bool IsOverflow(const StatusCode& status_code) {
    return status_code == StatusCode::kOverflow;
}
inline bool IsBadData(const StatusCode& status_code) {
    return status_code == StatusCode::kBadData;
}
inline bool IsShouldRetry(const StatusCode& status_code) {
    return status_code == StatusCode::kShouldRetry;
}
inline bool IsDropped(const StatusCode& status_code) {
    return status_code == StatusCode::kDropped;
}
inline bool IsPermanentError(const StatusCode& status_code) {
    return status_code == StatusCode::kPermanentError;
}
inline bool IsSliceHang(const StatusCode& status_code) {
    return status_code == StatusCode::kSliceHang;
}

// Returns error status associated with the function name using the error message passed in `msg`.
// Prefer using these functions to construct error status
inline Status InvalidArgumentError(const std::string& msg) {
    return Status(StatusCode::kInvalidArgument, msg);
}
inline Status NotSupportedError(const std::string& msg) {
    return Status(StatusCode::kNotSupported, msg);
}
inline Status ResourceBusyError(const std::string& msg) {
    return Status(StatusCode::kResourceBusy, msg);
}
inline Status HardwareError(const std::string& msg) {
    return Status(StatusCode::kHardwareError, msg);
}
inline Status ChannelError(const std::string& msg) {
    return Status(StatusCode::kChannelError, msg);
}
inline Status TimeoutError(const std::string& msg) {
    return Status(StatusCode::kTimeout, msg);
}
inline Status UnknownError(const std::string& msg) {
    return Status(StatusCode::kUnknown, msg);
}
inline Status OverflowError(const std::string& msg) {
    return Status(StatusCode::kOverflow, msg);
}
inline Status BadDataError(const std::string& msg) {
    return Status(StatusCode::kBadData, msg);
}
inline Status ShouldRetryError(const std::string& msg) {
    return Status(StatusCode::kShouldRetry, msg);
}
inline Status DroppedError(const std::string& msg) {
    return Status(StatusCode::kDropped, msg);
}
inline Status PermanentError(const std::string& msg) {
    return Status(StatusCode::kPermanentError, msg);
}
inline Status SliceHangError(const std::string& msg) {
    return Status(StatusCode::kSliceHang, msg);
}

inline Status StatusCodeToStatus(StatusCode code, const std::string& msg) {
    switch (code) {
    case StatusCode::kOk:
        return OkStatus();
    case StatusCode::kInvalidArgument:
        return InvalidArgumentError(msg);
    case StatusCode::kNotSupported:
        return NotSupportedError(msg);
    case StatusCode::kResourceBusy:
        return ResourceBusyError(msg);
    case StatusCode::kHardwareError:
        return HardwareError(msg);
    case StatusCode::kChannelError:
        return ChannelError(msg);
    case StatusCode::kTimeout:
        return TimeoutError(msg);
    case StatusCode::kOverflow:
        return OverflowError(msg);
    case StatusCode::kBadData:
        return BadDataError(msg);
    case vesal::StatusCode::kShouldRetry:
        return ShouldRetryError(msg);
    case StatusCode::kDropped:
        return DroppedError(msg);
    case StatusCode::kPermanentError:
        return PermanentError(msg);
    case StatusCode::kUnknown:
        return UnknownError(msg);
    case StatusCode::kSliceHang:
        return SliceHangError(msg);
    default:
        break;
    }
    return UnknownError(msg);
}

}  // namespace vesal
