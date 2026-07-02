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

#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "common/scheduler.h"
#include "vesal/vesal.h"

namespace vesal {
TEST(LogsettingTest, DefaultConfigTest) {
    LogSettings setting;
    EXPECT_EQ(0, InitLogging(setting));
    auto logger = spdlog::get("vesal_logger");
    EXPECT_NE(logger, nullptr);
    EXPECT_EQ(2, static_cast<int>(logger->level()));

    VESAL_LOG(INFO) << "Hello, vesal";
    ShutdownLogging();
}

TEST(LogSettingTest, Basic) {
    LogSettings setting;
    setting.file_name = "vesal_ut";
    setting.min_log_level = 0;
    EXPECT_NE(0, InitLogging(setting));
    setting.min_log_level = 7;
    EXPECT_NE(0, InitLogging(setting));
    setting.min_log_level = 2;
    setting.log_dir = "/this/is/a/very/long/directory/path/that/exceeds/the/maximum/allowed/length/"
                      "for/some/systems/and/will/cause/an/error";
    EXPECT_NE(0, InitLogging(setting));
    setting.log_dir = "";
    EXPECT_EQ(0, InitLogging(setting));
    {
        auto logger = spdlog::get("vesal_logger");
        EXPECT_NE(logger, nullptr);
        EXPECT_EQ(2, static_cast<int>(logger->level()));
        VESAL_LOG(INFO) << "Hello, vesal info log";

        // modify min log level in runtime
        EXPECT_EQ(-1, SetMinLogLevel(7));
        EXPECT_EQ(2, static_cast<int>(logger->level()));
        EXPECT_EQ(0, SetMinLogLevel(4));
        EXPECT_EQ(4, static_cast<int>(logger->level()));
        VESAL_LOG(INFO) << "Should not print this info log";
        VESAL_LOG(WARN) << "Should not print this warn log";
        VESAL_LOG(ERROR) << "Hello, vesal error log";
    }

    ShutdownLogging();
}

TEST(LogSettingTest, StreamableLoggerTest) {
    LogSettings setting;
    setting.file_name = "vesal_ut";
    setting.log_dir = "";
    setting.min_log_level = 2;
    EXPECT_EQ(0, InitLogging(setting));
    StreamableLogger logger("log_setting_test.cc", 0, "StreamableLoggerTest", LogLevel::kInfo);
    int i = 0;
    char c = '0';
    logger << i << c << "StreamableLoggerTest";
    EXPECT_EQ(0, std::strcmp("00StreamableLoggerTest", logger.log_stream_.str().c_str()));
}

TEST(LogSettingTest, CriticalLogTest) {
    // Must set this flag for multithread death test
    testing::FLAGS_gtest_death_test_style = "threadsafe";
    LogSettings setting;
    setting.file_name = "vesal_ut";
    setting.min_log_level = 5;
    // Clean up for previous test
    ShutdownLogging();
    std::remove(setting.file_name.c_str());

    EXPECT_EQ(0, InitLogging(setting));
    EXPECT_DEATH(VESAL_LOG(CRITICAL) << "12345", ".*");

    // Expect critical logs flushed before exit
    std::ifstream log_file(setting.file_name);
    EXPECT_TRUE(log_file.is_open());
    std::string critical_log;
    std::getline(log_file, critical_log);
    log_file.close();
    EXPECT_EQ(0, critical_log.substr(critical_log.size() - 5, 5).compare("12345"));

    ShutdownLogging();
}

TEST(LogSettingTest, InvalidMaxLogFileNum) {
    LogSettings setting;
    setting.max_log_file_num = 0;
    EXPECT_NE(0, InitLogging(setting));
}

TEST(LogSettingTest, InvalidMaxLogFileSize) {
    for (int i = 0; i < 128; i += 30) {
        LogSettings setting;
        setting.max_log_file_size = i;
        EXPECT_NE(0, InitLogging(setting));
    }
}

TEST(LogSettingTest, RotatingLogTest) {
    std::string sample_log = "Hello, vesal sample log";
    int sample_log_size = 0;

    // Get the sample log's size
    {
        LogSettings setting;
        setting.file_name = "vesal_ut";
        std::remove(setting.file_name.c_str());
        EXPECT_EQ(0, InitLogging(setting));
        VESAL_LOG(INFO) << sample_log;
        ShutdownLogging();

        // Open file in binary mode and move the cursor to the end
        std::ifstream log_file(setting.file_name.c_str(), std::ios::binary | std::ios::ate);
        EXPECT_TRUE(log_file.is_open());
        // Get the position of the cursor which is the size of the file
        sample_log_size = log_file.tellg();
        log_file.close();

        // log success
        EXPECT_GT(sample_log_size, 0);
        // one line log size should not be greater than file size limit
        EXPECT_LE(sample_log_size, setting.max_log_file_size);
    }

    for (int i = (1 << 9); i <= (1 << 12); i <<= 1)
        for (int j = 1; j <= 10; ++j) {
            LogSettings setting;
            setting.file_name = "vesal_ut";
            setting.log_dir = "./ut_log_dir";
            setting.max_log_file_size = i;
            setting.max_log_file_num = j;

            // the sample log size should not be greater than file size limit
            EXPECT_LE(sample_log_size, setting.max_log_file_size);

            EXPECT_EQ(0, InitLogging(setting));

            // enough iteration to produce content exceeding one log file's limit
            int iteration = setting.max_log_file_size / sample_log_size + 1;
            // produce enough content exceeding total logs limit to trigger log rotation
            for (int i = 1; i < (setting.max_log_file_num + 5) * iteration; ++i)
                VESAL_LOG(INFO) << sample_log;

            ShutdownLogging();

            // count file numbers and validate log rotation
            std::string file_cnt_result_file_path = setting.log_dir + "/file_cnt_result";
            std::string file_cnt_script =
                "ls " + setting.log_dir + " | wc -l > " + file_cnt_result_file_path;
            EXPECT_EQ(0, system(file_cnt_script.c_str()));

            std::ifstream file_cnt_result_file(file_cnt_result_file_path);
            EXPECT_TRUE(file_cnt_result_file.is_open());
            int file_cnt;
            file_cnt_result_file >> file_cnt;
            file_cnt_result_file.close();
            // the file cnt includes "file_cnt_result"
            EXPECT_EQ(file_cnt, setting.max_log_file_num + 1);

            // clean up
            // std::filesystem::remove_all() in C++17
            std::string remove_dir_script = "rm -r " + setting.log_dir;
            EXPECT_EQ(0, system(remove_dir_script.c_str()));
        }
}

TEST(LogSettingTest, AutoFlushTest) {
    std::string sample_log = "Hello, vesal sample log";

    LogSettings setting;
    setting.file_name = "vesal_ut";
    std::remove(setting.file_name.c_str());

    auto old_val = FLAGS_vesal_log_flush_interval_seconds = 1;
    EXPECT_EQ(0, InitLogging(setting));
    VESAL_LOG(INFO) << sample_log;
    g_periodic_scheduler.Start();

    sleep(2);

    std::ifstream log_file(setting.file_name);
    EXPECT_TRUE(log_file.is_open());
    std::string file_content;
    std::getline(log_file, file_content);
    log_file.close();
    EXPECT_NE(file_content.find(sample_log), file_content.npos);

    ShutdownLogging();
    g_periodic_scheduler.Stop();
    FLAGS_vesal_log_flush_interval_seconds = old_val;
}

}  // namespace vesal
