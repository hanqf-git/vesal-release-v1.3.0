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

#include "common/qat/qat_util.h"

#include <qat_dummy_driver.h>
#include <unistd.h>

#include <fstream>

#include "codec/qat/qat_error_handling.h"
#include "common/qat/qat_hardware_api_wrapper.h"
#include "vesal/vesal.h"

namespace vesal {
namespace qat {
// From intel's IPS reply:
// For a specific instance, set timeout to wait for a response. It is safe to free src/dest buffer
// after timeout. This timeout value could be “queue depth x 21 seconds (as 21 seconds are biggest
// timeout value used by QAT firmware)”, or “the number of inflight requests x 21 seconds”. The
// first one is safer, but it’s a long time (1.5 hours timeout based on queue depth used by
// Bytedance)
int g_qat_timeout_ms = 21 * 1000;

static const size_t kMaxQatSectionNameId = 1024;
// Only support self adaption of SSL for now.
// TODO(sjj): Grab the section name from config files.
static const char kQatSectionPrefix[] = "SSL";
static const int kQatDriverConfigNum = 64;
static const uint32_t kMinQatConcurrency = 64;
static const uint32_t kMaxQatConcurrency = 65536;
// 512 is from default qat config
static const uint32_t kDefaultQatConcurrency = 512;
static const char kQatDcConcurrecyKey[] = "DcNumConcurrentRequests";
static const char kQatSymConcurrecyKey[] = "CyNumConcurrentSymRequests";

uint32_t g_max_qat_cfg_concurrency = kDefaultQatConcurrency;

std::string g_vesal_codec_qat_section_name;
bool g_qat_started = false;

std::vector<std::string> GetQatSectionNames() {
    if (!FLAGS_vesal_codec_qat_section_name.empty()) {
        VESAL_LOG(INFO) << "Will try to use user specified qat section name: "
                        << FLAGS_vesal_codec_qat_section_name;
        return {FLAGS_vesal_codec_qat_section_name};
    }
    VESAL_LOG(INFO) << "Will try to use default qat section names";
    std::vector<std::string> ret;
    ret.reserve(kMaxQatSectionNameId + 2);
    for (size_t i = 0; i <= kMaxQatSectionNameId; ++i) {
        ret.emplace_back(kQatSectionPrefix + std::to_string(i));
    }
    // Specially, for original "SSL"
    ret.emplace_back(kQatSectionPrefix);
    return ret;
}

bool QatUserStart() {
    if (g_qat_started) {
        return true;
    }
    if (!g_driver_load_codec_ok && !g_driver_load_cypher_ok) {
        VESAL_LOG(ERROR) << "Neither codec nor cypher is found in libqat_s.so, "
                         << "please make sure qat is enabled in the system.";
        return false;
    }
    auto* qat_api_wrapper = GetQatApiWrapper();
    auto candidates = GetQatSectionNames();

    const int max_retries = 3;
    int retry_interval_ms = 200;

    for (int i = 0; i < max_retries; ++i) {
        for (auto& each : candidates) {
            // Qat driver guarantee this function is thread-safe.
            auto r = qat_api_wrapper->QAT_icp_sal_userStart(each.c_str());
            if (r == CPA_STATUS_SUCCESS) {
                if (i > 0) {
                    VESAL_LOG(INFO) << "QAT user start succeeded after " << i << " retries.";
                }
                VESAL_LOG(INFO) << "Using qat section name: " << each << " to start up";
                g_vesal_codec_qat_section_name = each;
                g_qat_started = true;
                return true;
            }
            if (r != CPA_STATUS_FAIL) {
                VESAL_LOG(WARN) << "Unexpected error on qat section name: " << each
                                << " to start up, error code: " << r;
            }
        }

        if (i < max_retries - 1) {
            VESAL_LOG(WARN) << "Failed to start qat, retrying in " << retry_interval_ms << "ms... ("
                            << i + 1 << "/" << max_retries << ")";
            usleep(retry_interval_ms * 1000);
            retry_interval_ms *= 2;  // Exponential backoff
        }
    }

    VESAL_LOG(ERROR) << "Failed to start qat after tried all section names and retries. "
                     << "This is usually because the QAT hardware resources (/dev/uio*) "
                     << "are currently occupied by another process or a zombie process.";
    return false;
}

bool QatUserStop() {
    if (g_qat_started) {
        CpaStatus cpa_status = qat::GetQatApiWrapper()->QAT_icpSalUserStop();
        if (CPA_STATUS_SUCCESS != cpa_status) {
            VESAL_LOG(ERROR) << "Fail to QAT_icpSalUserStop, cpa_status=" << cpa_status;
            return false;
        }
        g_qat_started = false;
    }
    return true;
}

// Read the concurrency from the file.
// Accept the value only if the section name matches, and the value of kQatDcConcurrecyKey is
// configured, then return true and modify 'concurrecy'. Otherwise return false and leave concurrecy
// untouched.
bool ReadConcurrencyFromFile(const std::string& path,
                             const std::string& section_name,
                             const char* currency_key,
                             uint32_t* concurrecy) {
    std::fstream file(path);
    if (!file) {
        return false;
    }
    std::string line;
    bool section_matched = false;
    bool configured = false;
    std::string section_line = "[" + section_name + "]";
    while (std::getline(file, line)) {
        if (line == section_line) {
            section_matched = true;
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.find(currency_key) != std::string::npos) {
            auto pos = line.find('=');
            if (pos == std::string::npos) {
                continue;
            }
            pos++;
            while (!(line[pos] >= '0' && line[pos] <= '9') && pos < line.size()) {
                ++pos;
            }
            auto end = line.size() - 1;
            while (!(line[end] >= '0' && line[end] <= '9') && end >= pos) {
                --end;
            }
            if (end < pos) {
                continue;
            }
            *concurrecy = std::stoi(line.substr(pos, end - pos + 1));
            VESAL_DCHECK(*concurrecy >= kMinQatConcurrency && *concurrecy <= kMaxQatConcurrency)
                << "concurrecy=" << *concurrecy;
            configured = true;
        }
    }
    return section_matched && configured;
}

// There is no easy way to get the max concurrency number for each instance at runtime, qat driver
// does not expose it for us. Given the configurations are the same in most cases, simply take
// the minimum value. If the concurrency is not found in the file, use the default value of 512.

int GetQatMaxConcurrency(QatServiceType type) {
    uint32_t ret = kMaxQatConcurrency;  // max for init.
    bool config_exists = false;
    const char* currency_key = nullptr;
    switch (type) {
    case QatServiceType::kDc:
        currency_key = kQatDcConcurrecyKey;
        break;
    case QatServiceType::kSym:
        currency_key = kQatSymConcurrecyKey;
        break;
    default:
        VESAL_LOG(CRITICAL) << "Unsupported qat service type: " << static_cast<int>(type);
    }
    for (int i = 0; i < kQatDriverConfigNum; ++i) {
        std::string file_name = "/etc/4xxxvf_dev" + std::to_string(i) + ".conf";
        uint32_t concurrecy = kDefaultQatConcurrency;
        bool found = ReadConcurrencyFromFile(
            file_name, g_vesal_codec_qat_section_name, currency_key, &concurrecy);
        config_exists |= found;
        if (found && concurrecy < ret) {
            ret = concurrecy;
        }
    }
    if (!config_exists) {
        VESAL_LOG(WARN) << "No qat config file found for " << g_vesal_codec_qat_section_name.c_str()
                        << ", use kDefaultQatConcurrency=" << kDefaultQatConcurrency;
        ret = kDefaultQatConcurrency;
    }
    if (ret < kMinQatConcurrency) {
        VESAL_LOG(WARN) << "Parsed qat config concurrency is less than " << kMinQatConcurrency
                        << ", use " << kMinQatConcurrency << " instead.";
        ret = kMinQatConcurrency;
    }
    return ret;
}

}  // namespace qat
}  // namespace vesal