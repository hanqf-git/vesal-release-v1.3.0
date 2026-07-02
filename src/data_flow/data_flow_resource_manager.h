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

#include <atomic>

#include "data_flow/data_flow_channel_impl.h"
#include "data_flow/data_flow_engines.h"
#include "vesal/data_flow.h"
#include "vesal/log_setting.h"

namespace vesal {

namespace data_flow {

class DataFlowResourceManager {
public:
    DataFlowResourceManager() = default;
    ~DataFlowResourceManager() = default;

    // reentrant, not thread-safe
    bool Init(const DataFlowInitOptions& init_opts) {
        if (init_) {
            VESAL_LOG(WARN) << "Data flow resource manger has been initialized previously";
            return true;
        }
        init_ = true;
        sw_engine_ = std::make_unique<SwEngine>();
        if (init_opts.init_dsa) {
            dsa_engine_ = std::make_unique<DsaEngine>();
            StatusCode init_result = dsa_engine_->Init(init_opts);
            if (init_result != StatusCode::kOk) {
                VESAL_LOG(ERROR) << "dsa engine initialization failed, status code: "
                                 << init_result;
                return false;
            }
        }
        return true;
    }

    static DataFlowResourceManager* GetInstance() {
        static auto p = std::make_unique<DataFlowResourceManager>();
        return p.get();
    }

    std::pair<StatusCode, std::unique_ptr<DataFlowChannel>> CreateDataFlowChannel(
        const DataFlowChannelOptions& opts) {
        std::unique_ptr<DataFlowChannelImpl> channel;
        switch (opts.engine_type) {
        case DataFlowEngineType::kSoftware:
            if (!sw_engine_) {
                VESAL_LOG(ERROR) << "sw engine not found";
                return {StatusCode::kChannelError, nullptr};
            }
            channel = std::make_unique<DataFlowChannelImpl>(sw_engine_.get(), opts);
            break;
        case DataFlowEngineType::kDsa:
            if (!dsa_engine_) {
                VESAL_LOG(ERROR) << "dsa engine not found";
                return {StatusCode::kChannelError, nullptr};
            }
            channel = std::make_unique<DataFlowChannelImpl>(dsa_engine_.get(), opts);
            break;
        default:
            return {StatusCode::kNotSupported, nullptr};
        }
        auto result = channel->Init(channel_id_counter_.fetch_add(1, std::memory_order_relaxed));
        if (result == StatusCode::kOk)
            return {result, std::move(channel)};
        return {result, nullptr};
    }

    void Uninit() {
        if (!init_) {
            VESAL_LOG(WARN) << "Data flow resource manger is not initialized";
            return;
        }
        // for the components below,
        // make sure to run their uninit() or dtor()
        sw_engine_.reset();
        dsa_engine_.reset();
        init_ = false;
    }

private:
    bool init_ = false;
    std::unique_ptr<SwEngine> sw_engine_;
    std::unique_ptr<DsaEngine> dsa_engine_;
    std::atomic<uint64_t> channel_id_counter_{0};
};

}  // namespace data_flow
}  // namespace vesal
