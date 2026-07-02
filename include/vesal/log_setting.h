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
#include <sstream>
#include <string>

namespace vesal {

const char kDefaultLogName[] = "vesal.log";
const char kDefaultLogDir[] = "";
const int kMaxLogFileSizeDefault = 100 * 1024 * 1024UL;  // 100MB
const int kMaxLogFileNumDefault = 10;
const int kMaxLogFileSizeLowerBound = 128;
const int kMaxLogFileNumLowerBound = 1;

enum class LogLevel : uint8_t {
    kDebug = 1,
    kInfo = 2,
    kWarn = 3,
    kError = 4,
    kCritical = 5,
};

struct LogSettings {
    LogSettings();

    // If both file_name and log_dir are unset, then log to console

    // Specify the name of logging files.
    // Default: "vesal"
    std::string file_name;

    // If specified, logfiles are written into this directory instead of the default logging
    // directory. It will create the log_dir firstly if the dir isn't existed.
    // Default: ${pwd}
    std::string log_dir;

    // Log messages at or above this level.
    //
    // 1 - DEBUG
    // 2 - INFO
    // 3 - WARN
    // 4 - ERROR
    // 5 - CRITICAL
    // 6 - OFF
    //
    // Default: INFO
    int min_log_level;

    // Log file's maximum size
    // Default: 100MB
    int max_log_file_size;

    // maximum num of log files
    // Default: 10
    // eg:
    //      vesal_log,vesal_log.1,...,vesal_log.9
    int max_log_file_num;
};

// Initialize logging, return 0 on success, or return -1.
// NOTE: log will be printed on stdout if NOT invoke this API.
int InitLogging(const LogSettings& settings);

// Call spdlog::shutdown which will flush logs and destory spdlog logging resources globally,
// however if called from vesal::Uninit(), spdlog::shutdown() will not be called and only local
// resources will be destroyed
void ShutdownLogging(bool uninit = false);

// Return 0 on success. Return -1 and keep minloglevel unchanged if level is invalid.
int SetMinLogLevel(int level);

class StreamableLogger {
public:
    explicit StreamableLogger(const char* file,
                              int line,
                              const char* function,
                              const LogLevel log_level);

    template <typename T> StreamableLogger& operator<<(const T& value) {
        log_stream_ << value;
        return *this;
    }

    ~StreamableLogger();

protected:
    void Flush();

private:
    std::ostringstream log_stream_;
    const char* file_;
    int line_;
    const char* function_;
    LogLevel log_level_;
};

class CriticalStreamableLogger : public StreamableLogger {
public:
    explicit CriticalStreamableLogger(const char* file, int line, const char* function);
    [[noreturn]] ~CriticalStreamableLogger();
};

// This class is used to explicitly ignore values in the conditional
// logging macros.  This avoids compiler warnings like "value computed
// is not used" and "statement has no effect".
class LogMessageVoidify {
public:
    LogMessageVoidify() {}
    // This has to be an operator with a precedence lower than << but
    // higher than ?:
    void operator&(StreamableLogger&) {}
};

}  // namespace vesal

// Mark a branch likely or unlikely to be true.
#if defined(__GNUC__) || defined(__clang__)
#if defined(__cplusplus)
#define VESAL_LIKELY(expr) (__builtin_expect(static_cast<bool>(expr), true))
#define VESAL_UNLIKELY(expr) (__builtin_expect(static_cast<bool>(expr), false))
#else
#define VESAL_LIKELY(expr) (__builtin_expect(!!(expr), 1))
#define VESAL_UNLIKELY(expr) (__builtin_expect(!!(expr), 0))
#endif
#else
#define VESAL_LIKELY(expr) (expr)
#define VESAL_UNLIKELY(expr) (expr)
#endif

#define VESAL_LOG(level) VESAL_LOG_##level

#define COMPACT_VESAL_LOG(level) \
    vesal::StreamableLogger(__FILE__, __LINE__, __FUNCTION__, vesal::LogLevel::k##level)
#ifdef NDEBUG
#define VESAL_LOG_DEBUG \
    while (false)       \
    COMPACT_VESAL_LOG(Debug)
#else
#define VESAL_LOG_DEBUG COMPACT_VESAL_LOG(Debug)
#endif
#define VESAL_LOG_INFO COMPACT_VESAL_LOG(Info)
#define VESAL_LOG_WARN COMPACT_VESAL_LOG(Warn)
#define VESAL_LOG_ERROR COMPACT_VESAL_LOG(Error)
#define VESAL_LOG_CRITICAL vesal::CriticalStreamableLogger(__FILE__, __LINE__, __FUNCTION__)

#define VESAL_LOG_IF(level, condition) \
    !(condition) ? (void)0 : vesal::LogMessageVoidify() & VESAL_LOG(level)

#define VESAL_CHECK(condition) \
    VESAL_LOG_IF(CRITICAL, VESAL_UNLIKELY(!(condition))) << "Check failed: " #condition " "

#ifdef NDEBUG
#define VESAL_DCHECK(condition) \
    while (false)               \
    VESAL_CHECK(condition)
#else
#define VESAL_DCHECK(condition) VESAL_CHECK(condition)
#endif
