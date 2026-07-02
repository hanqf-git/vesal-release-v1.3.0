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

#include "common/checksum_impl.h"
#include "data_flow/data_flow_engines.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {
namespace data_flow {

StatusCode SwEngine::Submit(DataFlowRequest* req) {
    uint64_t* sw_ctx = static_cast<uint64_t*>(req->engine_ctx);
    switch (req->op->type) {
    case DataFlowOperationType::kMove: {
        DataFlowMoveOperation* op = static_cast<DataFlowMoveOperation*>(req->op);
        for (size_t i = 0; i < req->num; ++i, ++op) {
            size_t offset = 0;
            for (size_t j = 0; j < op->src.size(); ++j) {
                // memmove is safe for overlapping memory regions, while memcpy is not
                std::memmove(static_cast<char*>(op->dst) + offset, op->src[j], op->src_len[j]);
                offset += op->src_len[j];
            }
            if (op->enable_crc) {
                sw_ctx[i] = ComputeCRC32(op->seed, reinterpret_cast<char*>(op->dst), offset);
            } else {
                sw_ctx[i] = 0;
            }
        }
    } break;
    case DataFlowOperationType::kCrc: {
        DataFlowCrcOperation* op = static_cast<DataFlowCrcOperation*>(req->op);
        for (size_t i = 0; i < req->num; ++i, ++op) {
            sw_ctx[i] = ComputeCRC32(op->seed, reinterpret_cast<char*>(op->src), op->len);
        }
    } break;
    default:
        return StatusCode::kNotSupported;
    }
    return StatusCode::kOk;
}

void SwEngine::CheckCompletion(DataFlowRequest* req, DataFlowResult* result) {
    req->completed = true;
    auto sw_ctx = static_cast<uint64_t*>(req->engine_ctx);
    result->status = StatusCode::kOk;
    result->ctx = req->user_ctx;
    for (size_t i = 0; i < req->num; ++i) {
        result->crc_output.push_back(sw_ctx[i]);
    }
}

StatusCode SwEngine::PreallocateCtx(DataFlowRequest* item) {
    item->engine_ctx = new uint64_t[kMaxSubmittedBatchSize];
    return StatusCode::kOk;
}

void SwEngine::ClearCtx(DataFlowRequest* item) {
    delete[] static_cast<uint64_t*>(item->engine_ctx);
}

}  // namespace data_flow
}  // namespace vesal