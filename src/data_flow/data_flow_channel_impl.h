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

#include <list>
#include "common/timer_manager.h"
#include "data_flow/data_flow_engines.h"
#include "vesal/metrics.h"
#include "vesal/status.h"

namespace vesal {
namespace data_flow {

// maximum amount of WIP requests in channel
static constexpr size_t kIoDepthMaximum = 128;

class DataFlowChannelImpl : public DataFlowChannel {
public:
    DataFlowChannelImpl(DataFlowEngine* engine, const DataFlowChannelOptions& opts);

    StatusCode Init(uint32_t channel_id);

    StatusCode SubmitMove(DataFlowMoveOperation ops[], size_t num, void* user_ctx) override;

    StatusCode SubmitCrc(DataFlowCrcOperation ops[], size_t num, void* user_ctx) override;

    ssize_t Poll(DataFlowResult results[], unsigned int max_num, int timeout) override;

    StatusCode Close() override;

private:
    StatusCode SubmitRequest(DataFlowOperationBase* ops,
                             size_t num,
                             uint64_t move_data_size,
                             uint64_t crc_data_size,
                             void* user_ctx);

    void PollDiscardedResult();

    DataFlowEngine* engine_;
    // TODO: replace std list
    std::list<DataFlowRequest*> queue_;
    std::list<DataFlowRequest*> expired_queue_;
    std::list<DataFlowRequest*> cached_resource_;
    DataFlowChannelOptions opts_;
    std::unique_ptr<TimerManager> timer_manager_;
    uint32_t channel_id_;

    std::shared_ptr<Counter> move_throughput_;
    std::shared_ptr<Counter> crc_throughput_;

    std::shared_ptr<Counter> request_failure_counter_;
    std::shared_ptr<Counter> submit_failure_counter_;
    std::shared_ptr<Counter> request_timeout_counter_;

    std::shared_ptr<Histogram> preprocess_latency_;
    std::shared_ptr<Histogram> postprocess_latency_;

    std::shared_ptr<Gauge> wip_request_num_;
};

}  // namespace data_flow
}  // namespace vesal
