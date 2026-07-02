
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

#include <cctype>
#include <memory>
#include <sstream>
#include <vector>
extern "C" {
#include "dsa_uio_config.h"
}
#include "common/metrics_internal.h"
#include "data_flow/data_flow_request.h"
#include "data_flow/dsa/dsa_ctx_factory.h"
#include "data_flow/dsa/dsa_dispatcher.h"
#include "data_flow/dsa/schedule_strategy.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

DEFINE_string(vesal_data_flow_dsa_quota,
              "",
              "DSA quota description. Will automatically grab quota from DSA0 if empty."
              "Example: 16,32"
              "Meaning: require 16 capacity from DSA0, 32 from DSA1.");

namespace vesal {
namespace data_flow {

static inline void movdir64b(void* dst, const void* src) {
    asm volatile(".byte 0x66, 0x0f, 0x38, 0xf8, 0x02\t\n" : : "a"(dst), "d"(src));
}

std::vector<uint64_t> Parse(const std::string& s) {
    std::vector<uint64_t> v;
    for (std::stringstream ss(s); ss.good();) {
        std::string token;
        std::getline(ss, token, ',');
        if (!token.empty())
            v.push_back(std::stoull(token));
    }
    return v;
}
bool IsValidNumberList(const std::string& s) {
    if (s.empty())
        return false;
    if (!isdigit(s.front()) || !isdigit(s.back()))
        return false;

    bool last_was_comma = false;
    for (char c : s) {
        if (isdigit(c)) {
            last_was_comma = false;
        } else if (c == ',') {
            if (last_was_comma)
                return false;  // reject "1,,2"
            last_was_comma = true;
        } else {
            return false;  // invalid character
        }
    }
    return true;
}

StatusCode DsaDispatcher::Init(const DataFlowInitOptions& init_opts) {
    if (!init_) {
        init_ = true;
        InitDsaConfig();
        wq_list_.clear();
        // Get all available DSA work queues
        for (uint32_t i = 0;; i++) {
            auto info = std::make_unique<WorkQueueInfo>();
            info->dsa_index = i;
            info->size = 0;
            info->uio_info.bdf = nullptr;
            info->uio_info.numa_node = -1;
            info->uio_info.portal = nullptr;
            if (!GetNextWorkQueue(&(info->uio_info))) {
                VESAL_LOG(INFO) << "Get work queue amount: " << i;
                break;
            } else {
                VESAL_LOG(INFO) << "Get work queue index: " << i << ", BDF: " << info->uio_info.bdf
                                << ", numa: " << info->uio_info.numa_node;
                VESAL_CHECK(info->uio_info.portal != nullptr) << "Failed to get wq portal";
                info->in_dsa_num = g_metric_registry->RegisterGauge(
                    "vesal.data_flow.in_dsa_num", {{"dsa", std::to_string(info->dsa_index)}});
                wq_list_.push_back(std::move(info));
            }
        }
        if (wq_list_.empty()) {
            CleanUp();
            VESAL_LOG(ERROR) << "Failed to get any wq";
            return StatusCode::kHardwareError;
        }
        if (FLAGS_vesal_data_flow_dsa_quota.empty()) {
            // By default, grab all quota from DSA0 only
            wq_list_[0]->capacity = kWorkQueueCapacity;
            for (uint32_t i = 1; i < wq_list_.size(); i++) {
                wq_list_[i]->capacity = 0;
            }
        } else {
            // grab quota based on FLAGS_vesal_data_flow_dsa_quota
            if (!IsValidNumberList(FLAGS_vesal_data_flow_dsa_quota)) {
                CleanUp();
                VESAL_LOG(ERROR) << "Invalid dsa quota, not a valid number list: "
                                 << FLAGS_vesal_data_flow_dsa_quota;
                return StatusCode::kInvalidArgument;
            }
            std::vector<uint64_t> quota = Parse(FLAGS_vesal_data_flow_dsa_quota);
            if (quota.size() != wq_list_.size()) {
                CleanUp();
                VESAL_LOG(ERROR) << "Invalid dsa quota, not match dsa amount: "
                                 << FLAGS_vesal_data_flow_dsa_quota;
                return StatusCode::kInvalidArgument;
            }
            for (uint32_t i = 0; i < wq_list_.size(); i++) {
                wq_list_[i]->capacity = quota[i];
            }
        }

        switch (init_opts.schedule_hint_type) {
        case DataFlowScheduleHintType::kNumaAware:
            strategy_ = std::make_unique<NumaAwareStrategy>();
            break;
        case DataFlowScheduleHintType::kDeviceAware:
            strategy_ = std::make_unique<DeviceAwareStrategy>();
            break;
        default:
            strategy_ = std::make_unique<RoundRobinStrategy>();
            break;
        }
        strategy_->Init(&wq_list_);
    }
    return StatusCode::kOk;
}

StatusCode DsaDispatcher::Submit(DataFlowRequest* req) {
    WorkQueueInfo* chosen_wq = strategy_->GetNext(req->schedule_hint);
    if (VESAL_UNLIKELY(!chosen_wq)) {
        VESAL_LOG(ERROR) << "Failed to choose a WQ based on schedule hint";
        return StatusCode::kInvalidArgument;
    }
    uint64_t result;
    if ((result = chosen_wq->size.fetch_add(1, std::memory_order_relaxed)) < chosen_wq->capacity) {
        DsaCtx* dsa_ctx = static_cast<DsaCtx*>(req->engine_ctx);
        dsa_ctx->work_queue = chosen_wq;
        movdir64b(chosen_wq->uio_info.portal, reinterpret_cast<void*>(dsa_ctx->dsa_hw_descs));
        chosen_wq->in_dsa_num->Set(result + 1);
        return StatusCode::kOk;
    } else {
        chosen_wq->size.fetch_sub(1, std::memory_order_relaxed);
        VESAL_LOG(WARN) << "DSA resource busy";
        return StatusCode::kResourceBusy;
    }
}

void DsaDispatcher::NotifyCompletion(DataFlowRequest* req) {
    DsaCtx* dsa_ctx = static_cast<DsaCtx*>(req->engine_ctx);
    uint64_t result = dsa_ctx->work_queue->size.fetch_sub(1, std::memory_order_relaxed);
    dsa_ctx->work_queue->in_dsa_num->Set(result - 1);
}

void DsaDispatcher::CleanUp() {
    for (auto& wq : wq_list_) {
        ReturnWorkQueue(&(wq->uio_info));
    }
    wq_list_.clear();
    strategy_.reset();
    init_ = false;
}

DsaDispatcher::~DsaDispatcher() {
    CleanUp();
}

}  // namespace data_flow
}  // namespace vesal