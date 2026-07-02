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

#include "cypher/cypher.h"

#include <qat_dummy_driver.h>

#include <memory>
#include <mutex>

#include "codec/qat/qat_codec.h"
#include "common/qat/qat_unit_manager.h"
#include "common/qat/qat_util.h"
#include "cypher/qat_cypher_channel.h"
#include "cypher/sw_cypher_channel.h"
#include "vesal/cypher.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {

std::unique_ptr<Cypher> g_cypher = nullptr;

bool Cypher::Start() {
    std::lock_guard<std::mutex> lg(build_mutex_);
    if (is_running_) {
        VESAL_LOG(ERROR)
            << "Cypher already initialized. Only one instance can be used in the process.";
        return false;
    }
    if (init_qat_) {
        if (!g_driver_load_cypher_ok) {
            VESAL_LOG(ERROR)
                << "Failed to load qat cypher driver, make sure cypher is enabled in the system.";
            return false;
        }
        max_in_qat_size_ =
            qat::GetQatMaxConcurrency(qat::QatServiceType::kSym) - QAT_RING_FREE_SIZE;
        Status unit_manager_r = unit_manager_->Init(qat::UnitType::kCy);
        if (!unit_manager_r.ok()) {
            VESAL_LOG(ERROR) << "Failed to init qat unit manager: " << unit_manager_r;
            return false;
        }
    }
    is_running_ = true;
    return true;
}

bool Cypher::Stop() {
    std::lock_guard<std::mutex> lg(build_mutex_);
    if (!is_running_) {
        VESAL_LOG(ERROR) << "Cypher already stopped.";
        return true;
    }
    if (init_qat_) {
        unit_manager_->Uninit();
    }
    is_running_ = false;
    return true;
}

std::pair<Status, std::unique_ptr<CypherChannel>> CypherChannel::CreateCypherChannel(
    const CypherChannelOption& opts) {
    if (!g_cypher) {
        return std::make_pair(NotSupportedError("Qat engine is not initialized"), nullptr);
    }
    return g_cypher->CreateCypherChannel(opts);
}

std::pair<Status, std::unique_ptr<CypherChannel>> Cypher::CreateCypherChannel(
    const CypherChannelOption& opts) {
    std::pair<Status, std::unique_ptr<CypherChannel>> r;
    switch (opts.engine) {
    case EngineType::kSoftware: {
        auto channel = std::make_unique<SwCypherChannel>(opts);
        Status st = channel->Init();
        if (!st.ok()) {
            r.first = st;
            return r;
        }
        r.second = std::move(channel);
        break;
    }
    case EngineType::kQat: {
        auto channel =
            std::make_unique<qat::QatCypherChannel>(opts, unit_manager_.get(), max_in_qat_size_);
        Status st = channel->Init();
        if (!st.ok()) {
            r.first = st;
            return r;
        }
        r.second = std::move(channel);
        break;
    }
    default:
        r.first = NotSupportedError("Unsupported engine type");
        break;
    }

    return r;
}

}  // namespace vesal