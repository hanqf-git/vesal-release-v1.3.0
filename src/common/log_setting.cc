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

#include "vesal/log_setting.h"

#include <gflags/gflags.h>

#include <chrono>
#include <cstdio>
#include <memory>

#include "common/scheduler.h"

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/common.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>
#include <unistd.h>

#include "spdlog/async.h"
#include "spdlog/async_logger.h"
#include "spdlog/details/thread_pool.h"
#include "spdlog/sinks/sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"

namespace {

bool ValidateLogMinLevel(const char* flagname, int32_t value) {
    return vesal::SetMinLogLevel(value) == 0;
}

}  // namespace

DEFINE_bool(vesal_log_console_output,
            false,
            "If true, log output will go to console stdout instead of log file. "
            "FLAGS_vesal_log_dir and FLAGS_vesal_log_file_name will be ignored.");

DEFINE_int32(vesal_log_level,
             2,
             "1 - DEBUG, 2 - INFO, 3 - WARN, 4 - ERROR, 5 - CRITICAL, 6 - OFF."
             "The log whose level < vesal_log_level will not be displayed."
             "If --vesal_log_level is set, the actual level would be vesal_log_level, "
             "otherwise it's user-specified LogSettings.min_log_level.");
// NOTE: NOT support modify log level to `DEBUG' runtime in release build mode.
DEFINE_validator(vesal_log_level, &ValidateLogMinLevel);

DEFINE_uint32(vesal_log_thread_pool_queue_size,
              8192,
              "The queue size of logging thread pool, 8192 by default");

DEFINE_uint32(vesal_log_thread_pool_size, 1, "The size of logging thread pool, 1 by default");
DEFINE_uint32(vesal_log_flush_interval_seconds,
              10,
              "The duration in seconds of auto flushing logs, 10 by default");

namespace vesal {

// cache log level to optimize spdlog::get_level() cost.
static LogLevel g_vesal_log_level;

std::shared_ptr<spdlog::logger> g_vesal_logger = spdlog::default_logger();
std::shared_ptr<spdlog::details::thread_pool> g_logger_thread_pool = nullptr;
uint32_t g_auto_flush_task_id;

inline bool IsLogLevelValid(int level) {
    return (level >= 1 && level <= 6);
}

LogSettings::LogSettings()
    : file_name(kDefaultLogName),
      log_dir(kDefaultLogDir),
      min_log_level(FLAGS_vesal_log_level),
      max_log_file_size(kMaxLogFileSizeDefault),
      max_log_file_num(kMaxLogFileNumDefault) {}

int InitLogging(const LogSettings& settings) {
    if (!IsLogLevelValid(settings.min_log_level)) {
        fprintf(stderr,
                "The value of `%s` should in range[1, 6], but got %d\n",
                "vesal::LogSettings::min_log_level",
                settings.min_log_level);
        return -1;
    }
    if (settings.max_log_file_num < kMaxLogFileNumLowerBound) {
        fprintf(stderr,
                "The value of max_log_file_num should not be less than %d, but got %d\n",
                kMaxLogFileNumLowerBound,
                settings.max_log_file_num);
        return -1;
    }
    if (settings.max_log_file_size < kMaxLogFileSizeLowerBound) {
        fprintf(stderr,
                "The value of max_log_file_size should not be less than %d, but got %d\n",
                kMaxLogFileSizeLowerBound,
                settings.max_log_file_size);
        return -1;
    }
    std::shared_ptr<spdlog::sinks::sink> sink;
    g_logger_thread_pool = std::make_shared<spdlog::details::thread_pool>(
        FLAGS_vesal_log_thread_pool_queue_size, FLAGS_vesal_log_thread_pool_size);
    if (FLAGS_vesal_log_console_output ||
        (settings.log_dir.empty() && settings.file_name.empty())) {
        sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    } else {
        // if at least one of dir and file_name is set, then log to file
        if (!settings.log_dir.empty()) {
            const char* path = settings.log_dir.c_str();
            if ((mkdir(path, 0755) == -1 || access(path, R_OK | W_OK) == -1) && errno != EEXIST) {
                fprintf(stderr, "Could not create loogging file %s: (%d)\n", path, errno);
                return -1;
            }
        }

        std::string log_dir = settings.log_dir.empty() ? "." : settings.log_dir;
        std::string path = log_dir + "/" + settings.file_name;
        sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path, settings.max_log_file_size, settings.max_log_file_num - 1);
    }

    g_vesal_logger = std::make_shared<spdlog::async_logger>(
        "vesal_logger", sink, g_logger_thread_pool, spdlog::async_overflow_policy::overrun_oldest);
    g_vesal_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%F][%t][%l][%s:%# %!] %v");
    if (SetMinLogLevel(settings.min_log_level)) {
        return -1;
    }
    try {
        spdlog::register_logger(g_vesal_logger);
    } catch (const std::exception& e) {
        fprintf(stderr, "Failed to register logger, msg: %s\n", e.what());
        return -1;
    }
    // Spdlog does not flush automatically, add our own periodic flush task
    g_auto_flush_task_id = g_periodic_scheduler.AddPeriodicTask(
        []() { g_vesal_logger->flush(); },
        std::chrono::seconds(FLAGS_vesal_log_flush_interval_seconds));
    // Force flush error logs immediately
    g_vesal_logger->flush_on(spdlog::level::err);
    return 0;
}

void ShutdownLogging(bool uninit) {
    g_periodic_scheduler.CompleteTask(g_auto_flush_task_id);
    g_vesal_logger.reset();
    spdlog::drop("vesal_logger");
    g_logger_thread_pool.reset();
    if (!uninit)
        spdlog::shutdown();
}

int SetMinLogLevel(int level) {
    if (IsLogLevelValid(level)) {
        g_vesal_logger->set_level(static_cast<spdlog::level::level_enum>(level));
        g_vesal_log_level = static_cast<LogLevel>(level);
        return 0;
    }
    fprintf(stderr,
            "Set min log level failed. The value should be in range[1, 6], but got %d\n",
            level);
    return -1;
}

StreamableLogger::StreamableLogger(const char* file,
                                   int line,
                                   const char* function,
                                   const LogLevel log_level)
    : file_(file), line_(line), function_(function), log_level_(log_level) {}

void StreamableLogger::Flush() {
    if (log_level_ < g_vesal_log_level)
        return;
    spdlog::level::level_enum level = spdlog::level::level_enum::info;
    switch (log_level_) {
    case LogLevel::kDebug:
        level = spdlog::level::level_enum::debug;
        break;
    case LogLevel::kInfo:
        level = spdlog::level::level_enum::info;
        break;
    case LogLevel::kWarn:
        level = spdlog::level::level_enum::warn;
        break;
    case LogLevel::kError:
        level = spdlog::level::level_enum::err;
        break;
    case LogLevel::kCritical:
        level = spdlog::level::level_enum::critical;
        break;
    default:
        VESAL_CHECK(0) << "this log level does not exists";
    }
    g_vesal_logger->log(spdlog::source_loc{file_, line_, function_}, level, log_stream_.str());
}

StreamableLogger::~StreamableLogger() {
    Flush();
}

CriticalStreamableLogger::CriticalStreamableLogger(const char* file, int line, const char* function)
    : StreamableLogger(file, line, function, LogLevel::kCritical) {}

CriticalStreamableLogger::~CriticalStreamableLogger() {
    Flush();
    ShutdownLogging();
    std::abort();
}

}  // namespace vesal
