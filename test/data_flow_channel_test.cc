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

#include "common/checksum_impl.h"
#include "data_flow/data_flow_channel_impl.h"
#include "data_flow/data_flow_resource_manager.h"
#include "vesal/data_flow.h"
#include "vesal/memory_pool.h"

#include <gtest/gtest.h>
#include <random>
#include <stack>
#include <thread>
#include <vector>

namespace vesal {
static void initialize_data_with_random(uint8_t* data, size_t size) {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<uint8_t> distribution(0, 255);

    for (size_t i = 0; i < size; ++i) {
        data[i] = distribution(generator);
    }
}

class ReverseOrderCompleteNothingEngine : public data_flow::DataFlowEngine {
    StatusCode PreallocateCtx(data_flow::DataFlowRequest* item) override {
        return StatusCode::kOk;
    }
    StatusCode Submit(data_flow::DataFlowRequest* item) override {
        items.push(item);
        return StatusCode::kOk;
    }
    void CheckCompletion(data_flow::DataFlowRequest* item, DataFlowResult* result) override {
        if (item == items.top()) {
            items.pop();
            item->completed = true;
            result->ctx = item->user_ctx;
        }
    }
    std::stack<data_flow::DataFlowRequest*> items;
};

class FlagControlledCompleteNothingEngine : public data_flow::DataFlowEngine {
    StatusCode PreallocateCtx(data_flow::DataFlowRequest* item) override {
        return StatusCode::kOk;
    }
    StatusCode Submit(data_flow::DataFlowRequest* item) override {
        return StatusCode::kOk;
    }
    void CheckCompletion(data_flow::DataFlowRequest* item, DataFlowResult* result) override {
        if (*static_cast<bool*>(item->user_ctx)) {
            item->completed = true;
            result->ctx = item->user_ctx;
        }
        return;
    }
};

class DataFlowChannelTest : public ::testing::Test {
public:
    void SetUp() override {
        DataFlowInitOptions data_flow_init_opt;
        data_flow::DataFlowResourceManager* rm = data_flow::DataFlowResourceManager::GetInstance();
        rm->Uninit();
        EXPECT_TRUE(rm->Init(data_flow_init_opt));
    }

    void TearDown() override {
        data_flow::DataFlowResourceManager* rm = data_flow::DataFlowResourceManager::GetInstance();
        rm->Uninit();
    }
};

TEST_F(DataFlowChannelTest, NotSupported) {
    DataFlowChannelOptions opts;
    opts.engine_type = DataFlowEngineType::kNum;
    auto result = DataFlowChannel::CreateDataFlowChannel(opts);
    EXPECT_EQ(result.first, StatusCode::kNotSupported);
}

TEST_F(DataFlowChannelTest, ChannelTimeout) {
    DataFlowChannelOptions opts;
    opts.engine_type = DataFlowEngineType::kSoftware;
    opts.timeout_ms = 300;
    auto engine = std::make_unique<FlagControlledCompleteNothingEngine>();
    auto channel = std::make_unique<data_flow::DataFlowChannelImpl>(engine.get(), opts);
    EXPECT_EQ(channel->Init(0), StatusCode::kOk);

    uint64_t chunk_size = 4096;
    auto src1 = std::make_unique<unsigned char[]>(chunk_size);
    auto src2 = std::make_unique<unsigned char[]>(chunk_size);
    auto dst = std::make_unique<unsigned char[]>(chunk_size);

    StatusCode submit_res;
    ssize_t poll_res;
    DataFlowMoveOperation move_ops[1];
    move_ops[0].src = {src1.get()};
    move_ops[0].src_len = {chunk_size};
    move_ops[0].dst = dst.get();

    size_t request_num = 10;
    DataFlowResult results[request_num];

    // engine will not complete the request
    bool flag = false;

    for (size_t i = 0; i < request_num; ++i) {
        submit_res = channel->SubmitMove(move_ops, 1, &flag);
        EXPECT_EQ(submit_res, StatusCode::kOk);
    }

    size_t poll_sum = 0;
    while (poll_sum < request_num) {
        poll_res = channel->Poll(results + poll_sum, request_num - poll_sum, -1);
        EXPECT_GE(poll_res, 0);
        poll_sum += poll_res;
    }
    EXPECT_EQ(poll_sum, request_num);
    for (size_t i = 0; i < request_num; ++i)
        EXPECT_EQ(results[i].status, StatusCode::kTimeout);

    // all timeout request in the expired_queue_
    EXPECT_EQ(channel->expired_queue_.size(), request_num);

    // engine will complete the expired request
    flag = true;
    poll_res = channel->Poll(results, request_num, -1);
    EXPECT_EQ(poll_res, 0);

    // the expired_queue_ was cleared
    EXPECT_TRUE(channel->expired_queue_.empty());

    EXPECT_EQ(channel->Close(), StatusCode::kOk);
}

TEST_F(DataFlowChannelTest, SwOrderedChannel) {
    DataFlowChannelOptions opts;
    opts.engine_type = DataFlowEngineType::kSoftware;
    auto result = DataFlowChannel::CreateDataFlowChannel(opts);
    EXPECT_EQ(result.first, StatusCode::kOk);
    auto channel = std::move(result.second);
    auto sw_channel = dynamic_cast<data_flow::DataFlowChannelImpl*>(channel.get());

    uint64_t chunk_size = 4096;
    auto src1 = std::make_unique<unsigned char[]>(chunk_size);
    auto src2 = std::make_unique<unsigned char[]>(chunk_size);
    auto dst = std::make_unique<unsigned char[]>(chunk_size);
    initialize_data_with_random(src1.get(), chunk_size);
    initialize_data_with_random(src2.get(), chunk_size);

    StatusCode submit_res;
    ssize_t poll_res;

    DataFlowMoveOperation move_ops[1];
    move_ops[0].src = {src1.get()};
    move_ops[0].src_len = {chunk_size};
    move_ops[0].dst = dst.get();

    DataFlowCrcOperation crc_ops[1];
    crc_ops[0].src = src2.get();
    crc_ops[0].len = chunk_size;
    crc_ops[0].seed = 0;

    DataFlowResult results[2];
    char ctx[2];

    submit_res = sw_channel->SubmitMove(move_ops, 1, &ctx[0]);
    EXPECT_EQ(submit_res, StatusCode::kOk);

    submit_res = sw_channel->SubmitCrc(crc_ops, 1, &ctx[1]);
    EXPECT_EQ(submit_res, StatusCode::kOk);

    poll_res = sw_channel->Poll(results, 2, -1);
    EXPECT_EQ(poll_res, 2);
    EXPECT_EQ(results[0].ctx, &ctx[0]);
    EXPECT_EQ(results[1].ctx, &ctx[1]);
    EXPECT_EQ(1, results[0].crc_output.size());
    // crc=0 if disable crc
    EXPECT_EQ(0, results[0].crc_output[0]);
    EXPECT_EQ(1, results[1].crc_output.size());
    uint64_t channel_crc = results[1].crc_output.front();
    uint64_t expected_crc = ComputeCRC32(0, reinterpret_cast<const char*>(src2.get()), chunk_size);
    EXPECT_EQ(channel_crc, expected_crc);

    EXPECT_EQ(channel->Close(), StatusCode::kOk);
}

TEST_F(DataFlowChannelTest, UnorderedChannel) {
    DataFlowChannelOptions opts;
    opts.order_type = DataFlowPollOrderType::kUnordered;
    auto reverse_engine = std::make_unique<ReverseOrderCompleteNothingEngine>();
    auto channel = std::make_unique<data_flow::DataFlowChannelImpl>(reverse_engine.get(), opts);
    EXPECT_EQ(channel->Init(0), StatusCode::kOk);

    uint64_t chunk_size = 4096;
    auto src1 = std::make_unique<unsigned char[]>(chunk_size);
    auto src2 = std::make_unique<unsigned char[]>(chunk_size);
    auto dst = std::make_unique<unsigned char[]>(chunk_size);

    StatusCode submit_res;
    ssize_t poll_res;
    DataFlowMoveOperation move_ops[1];
    move_ops[0].src = {src1.get()};
    move_ops[0].src_len = {chunk_size};
    move_ops[0].dst = dst.get();
    DataFlowCrcOperation crc_ops[1];
    crc_ops[0].src = src1.get();
    crc_ops[0].len = chunk_size;
    DataFlowResult results[2];
    char ctx[2];

    submit_res = channel->SubmitMove(move_ops, 1, &ctx[0]);
    EXPECT_EQ(submit_res, StatusCode::kOk);

    submit_res = channel->SubmitCrc(crc_ops, 1, &ctx[1]);
    EXPECT_EQ(submit_res, StatusCode::kOk);

    ssize_t current_num = 0;
    ssize_t total_num = 2;
    while (current_num < total_num) {
        poll_res = channel->Poll(results, 2, -1);
        for (ssize_t i = 0; i < poll_res; ++i) {
            EXPECT_EQ(results[i].ctx, &ctx[total_num - current_num - i - 1]);
        }
        current_num += poll_res;
    }
    EXPECT_EQ(current_num, total_num);

    EXPECT_EQ(channel->Close(), StatusCode::kOk);
}

TEST_F(DataFlowChannelTest, DsaOrderedChannel) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());

    DataFlowChannelOptions opts;
    opts.engine_type = DataFlowEngineType::kDsa;
    auto result = DataFlowChannel::CreateDataFlowChannel(opts);
    EXPECT_EQ(result.first, StatusCode::kOk);
    auto dsa_channel = std::move(result.second);

    uint64_t chunk_size = 4096;
    void* src = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(src), chunk_size);
    void* dst = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, dst);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(dst), chunk_size);
    EXPECT_NE(0, std::memcmp(src, dst, chunk_size));

    StatusCode submit_res;
    ssize_t poll_res = 0;
    DataFlowMoveOperation move_ops[1];
    move_ops[0].src = {src};
    move_ops[0].src_len = {chunk_size};
    move_ops[0].dst = dst;

    submit_res = dsa_channel->SubmitMove(move_ops, 1, nullptr);
    EXPECT_EQ(submit_res, StatusCode::kOk);

    DataFlowResult results[1];
    while (poll_res == 0) {
        poll_res = dsa_channel->Poll(results, 1, -1);
    }
    EXPECT_EQ(poll_res, 1);
    EXPECT_EQ(0, std::memcmp(src, dst, chunk_size));
    mp->Deallocate(src);
    mp->Deallocate(dst);

    EXPECT_EQ(dsa_channel->Close(), StatusCode::kOk);
}

TEST_F(DataFlowChannelTest, DataFlowResourceManagerNeedInit) {
    // singleton rm might be inited before
    data_flow::DataFlowResourceManager* rm = data_flow::DataFlowResourceManager::GetInstance();
    rm->Uninit();

    DataFlowChannelOptions opts;
    opts.engine_type = DataFlowEngineType::kSoftware;
    auto result = DataFlowChannel::CreateDataFlowChannel(opts);
    EXPECT_EQ(result.first, StatusCode::kChannelError);

    opts.engine_type = DataFlowEngineType::kDsa;
    result = DataFlowChannel::CreateDataFlowChannel(opts);
    EXPECT_EQ(result.first, StatusCode::kChannelError);
}

TEST_F(DataFlowChannelTest, InvalidInput) {
    DataFlowChannelOptions opts;
    opts.engine_type = DataFlowEngineType::kSoftware;
    auto result = DataFlowChannel::CreateDataFlowChannel(opts);
    EXPECT_EQ(result.first, StatusCode::kOk);
    auto channel = std::move(result.second);
    auto sw_channel = dynamic_cast<data_flow::DataFlowChannelImpl*>(channel.get());

    uint64_t chunk_size = 4096;
    auto src1 = std::make_unique<unsigned char[]>(kMaxTotalLength * 2);
    auto src2 = std::make_unique<unsigned char[]>(kMaxTotalLength * 2);
    auto dst = std::make_unique<unsigned char[]>(chunk_size);

    StatusCode submit_res;
    DataFlowMoveOperation move_ops[2 * kMaxSubmittedBatchSize];

    // note here every op has the same src&dst, which is invalid
    // it's to avoid debug log core
    for (size_t i = 0; i < 2 * kMaxSubmittedBatchSize; ++i) {
        move_ops[i].src = {src1.get()};
        move_ops[i].src_len = {chunk_size};
        move_ops[i].dst = dst.get();
    }
    DataFlowCrcOperation crc_ops[2 * kMaxSubmittedBatchSize];
    for (size_t i = 0; i < 2 * kMaxSubmittedBatchSize; ++i) {
        crc_ops[i].src = src1.get();
        crc_ops[i].len = chunk_size;
    }

    size_t invalid_op_num = kMaxSubmittedBatchSize + 1;
    submit_res = sw_channel->SubmitMove(move_ops, invalid_op_num, nullptr);
    EXPECT_EQ(submit_res, StatusCode::kInvalidArgument);

    move_ops[0].src = {src1.get()};
    move_ops[0].src_len = {};
    move_ops[0].dst = dst.get();
    EXPECT_NE(move_ops[0].src.size(), move_ops[0].src_len.size());
    submit_res = sw_channel->SubmitMove(move_ops, 1, nullptr);
    EXPECT_EQ(submit_res, StatusCode::kInvalidArgument);

    uint64_t invalid_length = kMaxTotalLength + 1;
    move_ops[0].src = {src1.get()};
    move_ops[0].src_len = {invalid_length};
    move_ops[0].dst = dst.get();
    submit_res = sw_channel->SubmitMove(move_ops, 1, nullptr);
    EXPECT_EQ(submit_res, StatusCode::kInvalidArgument);

    move_ops[0].src = {src1.get(), src2.get()};
    move_ops[0].src_len = {kMaxTotalLength, kMaxTotalLength};
    move_ops[0].dst = dst.get();
    EXPECT_GT(move_ops[0].src_len[0] + move_ops[0].src_len[1], kMaxTotalLength);
    submit_res = sw_channel->SubmitMove(move_ops, 1, nullptr);
    EXPECT_EQ(submit_res, StatusCode::kInvalidArgument);

    submit_res = sw_channel->SubmitCrc(crc_ops, invalid_op_num, nullptr);
    EXPECT_EQ(submit_res, StatusCode::kInvalidArgument);

    crc_ops[0].src = src1.get();
    crc_ops[0].len = invalid_length;
    submit_res = sw_channel->SubmitCrc(crc_ops, 1, nullptr);
    EXPECT_EQ(submit_res, StatusCode::kInvalidArgument);

    EXPECT_EQ(channel->Close(), StatusCode::kOk);
}

TEST_F(DataFlowChannelTest, DsaMultiThread) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    size_t thread_num = 5;
    size_t io_depth = 5;
    EXPECT_LE(thread_num * io_depth, data_flow::kIoDepthMaximum);
    auto func = [&]() {
        // create channel
        DataFlowChannelOptions opts;
        opts.engine_type = DataFlowEngineType::kDsa;
        auto result = DataFlowChannel::CreateDataFlowChannel(opts);
        EXPECT_EQ(result.first, StatusCode::kOk);
        auto dsa_channel = std::move(result.second);

        size_t iteration = 100;
        while (iteration--) {
            StatusCode submit_res;
            ssize_t poll_res = 0;
            size_t poll_num = 0;
            std::vector<DataFlowMoveOperation*> move_ops(io_depth);
            size_t chunk_num = kMaxSrcSegmentNum;
            uint64_t chunk_size = 4096;
            for (size_t depth = 0; depth < io_depth; ++depth) {
                move_ops[depth] = new DataFlowMoveOperation[1];
                auto& move_op = move_ops[depth];
                for (size_t i = 0; i < chunk_num; ++i) {
                    void* src = mp->Allocate(chunk_size);
                    EXPECT_NE(nullptr, src);
                    initialize_data_with_random(reinterpret_cast<uint8_t*>(src), chunk_size);
                    move_op[0].src.push_back(src);
                    move_op[0].src_len.push_back(chunk_size);
                }
                void* dst = mp->Allocate(chunk_num * chunk_size);
                EXPECT_NE(nullptr, dst);
                move_op[0].dst = dst;
                move_op[0].enable_crc = true;
                submit_res = dsa_channel->SubmitMove(move_op, 1, nullptr);
                EXPECT_EQ(submit_res, StatusCode::kOk);
            }
            DataFlowResult results[1];
            while (poll_num < io_depth) {
                poll_res = dsa_channel->Poll(results, 1, -1);
                EXPECT_GE(poll_res, 0);
                poll_num += poll_res;
            }
            EXPECT_EQ(poll_num, io_depth);
            for (size_t depth = 0; depth < io_depth; ++depth) {
                for (size_t i = 0; i < chunk_num; ++i) {
                    ASSERT_EQ(0,
                              std::memcmp(move_ops[depth][0].src[i],
                                          (char*)move_ops[depth][0].dst + i * chunk_size,
                                          chunk_size));
                    mp->Deallocate(move_ops[depth][0].src[i]);
                }
                mp->Deallocate(move_ops[depth][0].dst);
                delete[] move_ops[depth];
            }
        }
        EXPECT_EQ(dsa_channel->Close(), StatusCode::kOk);
    };
    std::vector<std::thread> threads;
    for (size_t i = 0; i < thread_num; ++i) {
        threads.emplace_back(func);
    }
    for (auto& t : threads) {
        t.join();
    }
}

}  // namespace vesal
