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

#include <cstring>
#include "data_flow/data_flow_engines.h"
#include "data_flow/data_flow_request.h"
#include "dsa_uio_config.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {
namespace data_flow {

StatusCode DsaEngine::Init(const DataFlowInitOptions& init_opts) {
    dsa_dispatcher_ = std::make_unique<DsaDispatcher>();
    StatusCode init_result = dsa_dispatcher_->Init(init_opts);
    if (init_result != StatusCode::kOk) {
        VESAL_LOG(ERROR) << "dsa dispatcher initialization failed, status code: " << init_result;
        return init_result;
    }
    dsa_desc_factory_ = std::make_unique<DsaCtxFactory>();
    init_result = dsa_desc_factory_->Init();
    if (init_result != StatusCode::kOk) {
        VESAL_LOG(ERROR) << "dsa desc factory initialization failed, status code: " << init_result;
        return init_result;
    }
    return StatusCode::kOk;
}

StatusCode DsaEngine::Submit(DataFlowRequest* item) {
    StatusCode dsa_desc_factory_result = dsa_desc_factory_->BuildCtx(item);
    if (VESAL_UNLIKELY(dsa_desc_factory_result != StatusCode::kOk))
        return dsa_desc_factory_result;
    StatusCode dsa_dispatcher_result = dsa_dispatcher_->Submit(item);
    if (VESAL_UNLIKELY(dsa_dispatcher_result != StatusCode::kOk))
        return dsa_dispatcher_result;
    return StatusCode::kOk;
}

void DsaEngine::CheckCompletion(DataFlowRequest* item, DataFlowResult* result) {
    auto dsa_ctx = static_cast<DsaCtx*>(item->engine_ctx);
    int ret = DsaCheckCompletion(dsa_ctx->dsa_completion_records);
    if (ret != DSA_TASK_WIP) {
        item->completed = true;
        result->status = (ret == DSA_TASK_SUCCESS ? StatusCode::kOk : StatusCode::kHardwareError);
        result->ctx = item->user_ctx;
        result->crc_output.resize(item->num);
        for (size_t i = 0; i < item->num; ++i) {
            // revise crc result to be compatible with sw result
            result->crc_output[i] =
                (dsa_ctx->crc_results[i])
                    ? VESAL_CRC_REVISE(GetCrcInDsaCompletionRecord(dsa_ctx->crc_results[i]))
                    : 0;
        }
        dsa_dispatcher_->NotifyCompletion(item);
    }
}

StatusCode DsaEngine::PreallocateCtx(DataFlowRequest* item) {
    return dsa_desc_factory_->PreallocateCtx(item);
}

void DsaEngine::ClearCtx(DataFlowRequest* item) {
    dsa_desc_factory_->ClearCtx(item);
}

}  // namespace data_flow
}  // namespace vesal