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

#include "qat_codec.h"

#include <fstream>
#include <memory>
#include <mutex>
#include <utility>

#include "codec/qat/qat_codec_engine.h"
#include "codec/qat/qat_codec_dedicated_channel.h"
#include "codec/qat/qat_codec_shared_channel.h"
#include "codec/qat/qat_error_handling.h"
#include "common/qat/qat_hardware_api_wrapper.h"
#include "common/qat/qat_util.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"

extern "C" {
#include <qat_dummy_driver.h>
}

namespace vesal {
namespace qat {

bool QatCodec::is_running_ = false;
std::mutex QatCodec::qat_codec_build_mutex_;

namespace {

Status ValidateChannelOption(const CodecChannelOption& opts) {
    if (opts.mode == ChannelMode::kNum || opts.ha_policy == HaPolicy::kNum ||
        opts.engine_type == CodecEngineType::kNum || opts.comp_algorithm == CodecAlgorithm::kNum ||
        opts.checksum_type == CodecChecksumType::kNum || opts.comp_level == CodecCompLevel::kNum) {
        return InvalidArgumentError("Invalid channel option.");
    }
    return OkStatus();
}

}  // namespace

Status QatCodec::Start() {
    std::lock_guard<std::mutex> lg(QatCodec::qat_codec_build_mutex_);
    if (QatCodec::is_running_) {
        VESAL_LOG(ERROR)
            << "QatCodec already initialized. Only one instance can be used in the process.";
        return ResourceBusyError("QatCodec already initialized.");
    }

    if (g_driver_load_codec_ok == 0) {
        return vesal::NotSupportedError("Failed to load qat driver, make sure libqat_s.so and "
                                        "libusdm_drv_s.so exits in the system.");
    }
    // Read the config and get the minimum value of concurrency number.
    // If not, use 512, which is from default qat config.
    g_max_qat_cfg_concurrency = GetQatMaxConcurrency() - QAT_RING_FREE_SIZE;

    if (FLAGS_vesal_codec_qat_max_in_qat_num > g_max_qat_cfg_concurrency) {
        VESAL_LOG(WARN) << "FLAGS_vesal_codec_qat_max_in_qat_num="
                        << FLAGS_vesal_codec_qat_max_in_qat_num
                        << ", exceeds g_max_qat_cfg_concurrency=" << g_max_qat_cfg_concurrency
                        << ", will use " << g_max_qat_cfg_concurrency << " for max_in_qat_size_";
        max_in_qat_size_ = g_max_qat_cfg_concurrency;
    } else {
        max_in_qat_size_ = FLAGS_vesal_codec_qat_max_in_qat_num;
    }
    VESAL_LOG(INFO) << "g_max_qat_cfg_concurrency=" << g_max_qat_cfg_concurrency
                    << ", max_in_qat_size_=" << max_in_qat_size_;

    Status unit_manager_r = unit_manager_->Init();
    if (!unit_manager_r.ok()) {
        // If the env is not 100% ready, don't provide the service.
        return unit_manager_r;
    }

    QatCodec::is_running_ = true;
    return OkStatus();
}

std::pair<Status, std::unique_ptr<CodecChannel>> QatCodec::CreateCodecChannel(
    const CodecChannelOption& opts) {
    std::pair<Status, std::unique_ptr<CodecChannel>> r{};
    auto validation_r = ValidateChannelOption(opts);
    if (!validation_r.ok()) {
        return {validation_r, nullptr};
    }
    std::unique_ptr<CodecChannel> channel;
    if (opts.mode == ChannelMode::kDedicated) {
        auto dedicated_channel = std::make_unique<qat::QatCodecDedicatedChannel>(
            opts, unit_manager_.get(), max_in_qat_size_);
        r.first = dedicated_channel->Init();
        r.second = std::move(dedicated_channel);
    } else if (opts.mode == ChannelMode::kShared) {
        auto shared_channel = std::make_unique<qat::QatCodecSharedChannel>(opts);
        r.first = shared_channel->Init();
        r.second = std::move(shared_channel);
    } else {
        r.first = InvalidArgumentError("Unsupported channel mode: " +
                                       std::to_string(static_cast<int>(opts.mode)));
        return r;
    }
    if (!r.first.ok()) {
        return r;
    }
    return r;
}

QatCodec::~QatCodec() {
    VESAL_CHECK(Stop().ok());
}

Status QatCodec::Stop() {
    std::lock_guard<std::mutex> lg(QatCodec::qat_codec_build_mutex_);
    if (!QatCodec::is_running_) {
        return OkStatus();
    }
    unit_manager_->Uninit();
    QatCodec::is_running_ = false;
    return OkStatus();
}

std::pair<Status, std::unique_ptr<qat::QatCodecEngine>> CreateQatCodecEngine(
    const CodecChannelOption& opts) {
    std::pair<Status, std::unique_ptr<QatCodecEngine>> r{};
    auto c = std::make_unique<QatCodecEngine>(
        opts, g_qat_codec->GetUnitManager(), g_qat_codec->GetMaxInQatSize());
    r.first = c->Init();
    r.second = std::move(c);
    return r;
}

}  // namespace qat
}  // namespace vesal
