
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

#include "data_flow/data_flow_request.h"
#include "data_flow/dsa/schedule_strategy.h"
#include "data_flow/dsa/work_queue_info.h"

namespace vesal {
namespace data_flow {

class DsaDispatcher {
public:
    StatusCode Init(const DataFlowInitOptions& init_opts);

    StatusCode Submit(DataFlowRequest* req);

    void NotifyCompletion(DataFlowRequest* req);

    ~DsaDispatcher();

private:
    void CleanUp();
    // should be at least 1 wq after init
    std::vector<std::unique_ptr<WorkQueueInfo>> wq_list_{};
    std::unique_ptr<ScheduleStrategy> strategy_ = nullptr;
    bool init_ = false;
};

}  // namespace data_flow
}  // namespace vesal
