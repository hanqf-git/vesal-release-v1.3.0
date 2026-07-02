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

#include <cstddef>
#include <memory>
#include "common/timer_manager.h"
#include "vesal/data_flow.h"
#include "vesal/log_setting.h"
namespace vesal {
namespace data_flow {

// turn user requests into an internal batch of dsa descriptors
// the upper bound size of the internal batch
static const uint64_t kMaxInternalBatchSize = 1024;

struct DataFlowRequest {
    // the beginning of a batch of operations
    DataFlowOperationBase* op;
    // the amount of the operations in the batch
    size_t num;
    size_t schedule_hint;
    bool completed;
    bool expired;
    TimeoutContext* timer_ctx;
    void* engine_ctx;
    void* user_ctx;
    // metrics usage
    uint64_t move_data_size;
    uint64_t crc_data_size;

#ifndef NDEBUG
    std::vector<void*> src;
    std::vector<uint64_t> src_len;
    std::vector<void*> dst;
    std::vector<uint64_t> dst_len;
#endif

    void Build(DataFlowOperationBase* op,
               size_t num,
               void* user_ctx,
               size_t schedule_hint = 0,
               uint64_t move_data_size = 0,
               uint64_t crc_data_size = 0) {
        this->op = op;
        this->num = num;
        this->schedule_hint = schedule_hint;
        this->completed = false;
        this->expired = false;
        this->timer_ctx = nullptr;
        this->user_ctx = user_ctx;
        this->move_data_size = move_data_size;
        this->crc_data_size = crc_data_size;
#ifndef NDEBUG
        src.clear();
        src_len.clear();
        dst.clear();
        dst_len.clear();
        DataFlowMoveOperation* moveop;
        DataFlowCrcOperation* crcop;
        switch (op->type) {
        case DataFlowOperationType::kMove:
            moveop = dynamic_cast<DataFlowMoveOperation*>(op);
            for (size_t i = 0; i < num; ++i) {
                const auto& cur_op = moveop[i];
                size_t op_src_size = cur_op.src.size();
                uint64_t len_sum = 0;
                for (size_t j = 0; j < op_src_size; ++j) {
                    src.push_back(cur_op.src[j]);
                    src_len.push_back(cur_op.src_len[j]);
                    len_sum += cur_op.src_len[j];
                }
                dst.push_back(cur_op.dst);
                dst_len.push_back(len_sum);
            }
            break;
        case DataFlowOperationType::kCrc:
            crcop = dynamic_cast<DataFlowCrcOperation*>(op);
            for (size_t i = 0; i < num; ++i) {
                const auto& cur_op = crcop[i];
                src.push_back(cur_op.src);
                src_len.push_back(cur_op.len);
            }
            break;
        default:
            VESAL_LOG(CRITICAL) << "Wrong DataFlowOperationType";
        }
#endif
    }
};

std::ostream& operator<<(std::ostream& os, const DataFlowRequest& result);

}  // namespace data_flow
}  // namespace vesal
