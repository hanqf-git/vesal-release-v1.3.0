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

#include "vesal/status.h"
#include "vesal/vesal_version.h"

#include <memory>
#include <vector>

namespace vesal {

// maximum op num, "size_t num", in SubmitMove() and SubmitCrc()
static constexpr uint64_t kMaxSubmittedBatchSize = 128;
// for each op, maximum input src length sum
static constexpr uint64_t kMaxTotalLength = 128 * 1024 * 1024;
// for DataFlowMoveOperation, maximum size of "src_len"
static constexpr uint64_t kMaxSrcSegmentNum = 128;
// Note that there's a bound on dsa internal batch size,
// which is that (batch size)+(segment num sum) <= 1022,
// where (segment num sum) is the sum of segment num of every op within one batch

struct DataFlowResult {
    StatusCode status;  // OK if success.
    std::vector<uint64_t> crc_output;
    void* ctx;
};

std::ostream& operator<<(std::ostream& os, const DataFlowResult& result);

enum class DataFlowEngineType : uint8_t { kSoftware, kDsa, kNum };
enum class DataFlowPollOrderType : uint8_t { kOrdered, kUnordered, kNum };
enum class DataFlowPriority : uint8_t { kHighPriority, kLowPriority, kNum };
enum class DataFlowScheduleHintType : uint8_t { kNumaAware, kDeviceAware, kRoundRobin, kNum };
enum class DataFlowOperationType : uint8_t { kMove, kCrc, kNum };

struct DataFlowChannelOptions {
    DataFlowPollOrderType order_type = DataFlowPollOrderType::kOrdered;
    DataFlowEngineType engine_type = DataFlowEngineType::kSoftware;
    // related to DataFlowScheduleHintType
    size_t schedule_hint = 0;
    uint64_t timeout_ms{3000};
};

struct DataFlowInitOptions {
    bool init_dsa = true;  // if not, will only support software engine
    DataFlowScheduleHintType schedule_hint_type = DataFlowScheduleHintType::kNumaAware;
};

struct DataFlowOperationBase {
    virtual ~DataFlowOperationBase() = default;
    DataFlowPriority priority = DataFlowPriority::kHighPriority;
    DataFlowOperationType type;
};

// Merge multiple chunks together from src vector into dst
struct DataFlowMoveOperation : public DataFlowOperationBase {
    DataFlowMoveOperation() {
        type = DataFlowOperationType::kMove;
    }
    std::vector<void*> src;
    std::vector<uint64_t> src_len;
    // dst's size need to be big enough to hold the result
    // otherwise undefined behavior might happen
    void* dst;
    uint64_t seed = 0;
    bool enable_crc = 0;
};
std::ostream& operator<<(std::ostream& os, const DataFlowMoveOperation& opt);

struct DataFlowCrcOperation : public DataFlowOperationBase {
    DataFlowCrcOperation() {
        type = DataFlowOperationType::kCrc;
    }
    void* src;
    uint64_t len;
    uint64_t seed = 0;
};
std::ostream& operator<<(std::ostream& os, const DataFlowCrcOperation& opt);

class DataFlowChannel {
protected:
    DataFlowChannel() = default;

public:
    virtual ~DataFlowChannel() = default;
    /**
     * @brief Create the channel based on options. Channels are thread-exclusive. Sharing one
     * channel between threads might cause undefined behaviours.
     *
     * @param opts User spcified options.
     *
     * @return OK if success, with DataFlowChannel created. Otherwise:
     * - RESOURCE_BUSY Not enough software resourses to create a new channel
     * - INVALID_ARGUMENT Wrong opts given
     * - UNKNOWN Internal system error
     */
    static std::pair<StatusCode, std::unique_ptr<DataFlowChannel>> CreateDataFlowChannel(
        const DataFlowChannelOptions& opts);

    /**
     * @brief Submit a batch of Move requests. The function returns
     * immediately if the requests sent successfully. User needs to ensure that
     * each Move's arguments are valid.
     *
     * @param ops a batch of Move requests
     * @param num batch size
     * @param user_ctx User's context data. It's guaranteed the data will not be
     * touched during the request.
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel closed
     */
    virtual StatusCode SubmitMove(DataFlowMoveOperation ops[], size_t num, void* user_ctx) = 0;

    /**
     * @brief Submit a batch of crc generation requests. The function returns
     * immediately if the requests sent successfully. User needs to ensure that
     * each crcgen's arguments are valid.
     *
     * @param ops a batch of crc generation requests
     * @param num batch size
     * @param user_ctx User's context data. It's guaranteed the data will not be
     * touched during the request.
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel closed
     */
    virtual StatusCode SubmitCrc(DataFlowCrcOperation ops[], size_t num, void* user_ctx) = 0;

    /**
     * @brief Poll the results in the current channel.
     *
     * @param results CodecResult array to take polled results with member StatusCode: Ok, if
     * success. Otherwise:
     * - RESOURCE_BUSY No results ready yet
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel closed
     * @param max_num The maximum number of results can be got in this call.
     * @param timeout How long blocking in the call
     * timeout = -1 means blocking until at least one result ready
     * timeout = 0 means returning immediately if no results ready
     * timeout > 0 means blocking until at least one result ready or timeouted
     *
     * @return number of actual results got in this call. -1 if this call failed.
     */
    virtual ssize_t Poll(DataFlowResult results[], unsigned int max_num, int timeout) = 0;

    /**
     * @brief Close the channel. It will try to poll the remaining results if there is any.
     * The api will fail with RESOURCE_BUSY if there are still requests in flight.
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Still requests in the queue, need to wait and poll them before close
     * - UNKNOWN Internal system error
     */
    virtual StatusCode Close() = 0;
};

}  // namespace vesal
