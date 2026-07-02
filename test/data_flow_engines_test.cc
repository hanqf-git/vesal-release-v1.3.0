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

#include "data_flow/data_flow_engines.h"
#include "common/checksum_impl.h"
#include "common/memory_pool_helper.h"
#include "data_flow/data_flow_request.h"
#include "data_flow/data_flow_resource_manager.h"
#include "vesal/data_flow.h"
#include "vesal/memory_pool.h"

#include <gtest/gtest.h>
#include <cstring>
#include <random>
#include <stack>

namespace vesal {

static void initialize_data_with_random(uint8_t* data, size_t size) {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<uint8_t> distribution(0, 255);

    for (size_t i = 0; i < size; ++i) {
        data[i] = distribution(generator);
    }
}

class DataFlowEnginesTest : public ::testing::Test {
public:
    void SetUp() override {
        DataFlowInitOptions data_flow_init_opt;
        data_flow::DataFlowResourceManager* rm = data_flow::DataFlowResourceManager::GetInstance();
        rm->Uninit();
        // vesal::AddressManager::t_tls_memory_info_by_vaddr_.clear();
        EXPECT_TRUE(rm->Init(data_flow_init_opt));
        dsa_engine = rm->dsa_engine_.get();
        sw_engine = rm->sw_engine_.get();
    }

    void TearDown() override {
        data_flow::DataFlowResourceManager* rm = data_flow::DataFlowResourceManager::GetInstance();
        rm->Uninit();
    }

    data_flow::DsaEngine* dsa_engine;
    data_flow::SwEngine* sw_engine;
};

TEST_F(DataFlowEnginesTest, SwEngineCrcTest) {
    DataFlowCrcOperation op;
    uint64_t chunk_size = 4096;
    auto src = std::make_unique<unsigned char[]>(chunk_size);
    initialize_data_with_random(src.get(), chunk_size);
    op.src = src.get();
    op.len = chunk_size;
    op.seed = 0;
    data_flow::DataFlowRequest req;
    auto res = sw_engine->PreallocateCtx(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    req.Build(&op, 1, nullptr);
    DataFlowResult data_flow_result;

    res = sw_engine->Submit(&req);
    EXPECT_EQ(res, StatusCode::kOk);

    sw_engine->CheckCompletion(&req, &data_flow_result);
    EXPECT_EQ(data_flow_result.status, StatusCode::kOk);

    uint64_t engine_crc = data_flow_result.crc_output.front();
    uint64_t expected_crc =
        vesal::ComputeCRC32(0, reinterpret_cast<const char*>(src.get()), op.len);
    EXPECT_EQ(engine_crc, expected_crc);

    sw_engine->ClearCtx(&req);
}

TEST_F(DataFlowEnginesTest, SwEngineMemmoveTest) {
    DataFlowMoveOperation op;
    uint64_t chunk_size = 4096;
    auto src = std::make_unique<unsigned char[]>(chunk_size);
    auto dst = std::make_unique<unsigned char[]>(chunk_size);
    initialize_data_with_random(src.get(), chunk_size);
    op.src = {src.get()};
    op.src_len = {chunk_size};
    op.dst = dst.get();
    data_flow::DataFlowRequest req;
    auto res = sw_engine->PreallocateCtx(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    req.Build(&op, 1, nullptr);
    DataFlowResult data_flow_result;

    res = sw_engine->Submit(&req);
    EXPECT_EQ(res, StatusCode::kOk);

    sw_engine->CheckCompletion(&req, &data_flow_result);
    EXPECT_EQ(data_flow_result.status, StatusCode::kOk);

    EXPECT_EQ(0, std::memcmp(src.get(), dst.get(), chunk_size));
    sw_engine->ClearCtx(&req);
}

TEST_F(DataFlowEnginesTest, SwEngineMemmoveWithCrcTest) {
    DataFlowMoveOperation op;
    uint64_t chunk_size = 4096;
    auto src = std::make_unique<unsigned char[]>(chunk_size);
    auto dst = std::make_unique<unsigned char[]>(chunk_size);
    initialize_data_with_random(src.get(), chunk_size);
    op.src = {src.get()};
    op.src_len = {chunk_size};
    op.dst = dst.get();
    op.seed = 0;
    op.enable_crc = 1;
    data_flow::DataFlowRequest req;
    auto res = sw_engine->PreallocateCtx(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    req.Build(&op, 1, nullptr);
    DataFlowResult data_flow_result;

    res = sw_engine->Submit(&req);
    EXPECT_EQ(res, StatusCode::kOk);

    sw_engine->CheckCompletion(&req, &data_flow_result);
    EXPECT_EQ(data_flow_result.status, StatusCode::kOk);

    EXPECT_EQ(0, std::memcmp(src.get(), dst.get(), chunk_size));

    uint64_t engine_crc = data_flow_result.crc_output.front();
    uint64_t expected_crc =
        vesal::ComputeCRC32(0, reinterpret_cast<const char*>(src.get()), chunk_size);
    EXPECT_EQ(engine_crc, expected_crc);
    sw_engine->ClearCtx(&req);
}

TEST_F(DataFlowEnginesTest, DsaEngineMemmoveTest) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());

    uint64_t chunk_size = 4096;
    void* src = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(src), chunk_size);
    void* dst = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, dst);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(dst), chunk_size);
    EXPECT_NE(0, std::memcmp(src, dst, chunk_size));

    DataFlowMoveOperation op;
    op.src = {src};
    op.src_len = {chunk_size};
    op.dst = dst;

    data_flow::DataFlowRequest req;
    auto res = dsa_engine->PreallocateCtx(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    req.Build(&op, 1, nullptr);
    DataFlowResult data_flow_result;
    data_flow_result.status = StatusCode::kUnknown;

    res = dsa_engine->Submit(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    while (data_flow_result.status != StatusCode::kOk) {
        dsa_engine->CheckCompletion(&req, &data_flow_result);
        usleep(1000);
    }
    EXPECT_EQ(data_flow_result.status, StatusCode::kOk);
    EXPECT_EQ(0, std::memcmp(src, dst, chunk_size));

    mp->Deallocate(src);
    mp->Deallocate(dst);
    dsa_engine->ClearCtx(&req);
}

TEST_F(DataFlowEnginesTest, DsaEngineCrcTest) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());

    uint64_t chunk_size = 4096;
    void* src = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(src), chunk_size);

    DataFlowCrcOperation op;
    op.src = src;
    op.len = chunk_size;
    op.seed = 0;

    data_flow::DataFlowRequest req;
    auto res = dsa_engine->PreallocateCtx(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    req.Build(&op, 1, nullptr);
    DataFlowResult data_flow_result;
    data_flow_result.status = StatusCode::kUnknown;

    res = dsa_engine->Submit(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    while (data_flow_result.status != StatusCode::kOk) {
        dsa_engine->CheckCompletion(&req, &data_flow_result);
        usleep(1000);
    }
    EXPECT_EQ(data_flow_result.status, StatusCode::kOk);
    EXPECT_EQ(1, data_flow_result.crc_output.size());
    EXPECT_EQ(ComputeCRC32(op.seed, reinterpret_cast<char*>(src), chunk_size),
              data_flow_result.crc_output.front());

    mp->Deallocate(src);
    dsa_engine->ClearCtx(&req);
}

TEST_F(DataFlowEnginesTest, DsaEngineMemmoveWithCrcTest) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());

    uint64_t chunk_size = 4096;
    void* src = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(src), chunk_size);
    void* dst = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, dst);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(dst), chunk_size);
    EXPECT_NE(0, std::memcmp(src, dst, chunk_size));

    DataFlowMoveOperation op;
    op.src = {src};
    op.src_len = {chunk_size};
    op.dst = {dst};
    op.seed = 0;
    op.enable_crc = true;

    data_flow::DataFlowRequest req;
    auto res = dsa_engine->PreallocateCtx(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    req.Build(&op, 1, nullptr);
    DataFlowResult data_flow_result;
    data_flow_result.status = StatusCode::kUnknown;

    res = dsa_engine->Submit(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    while (data_flow_result.status != StatusCode::kOk) {
        dsa_engine->CheckCompletion(&req, &data_flow_result);
        usleep(1000);
    }
    EXPECT_EQ(data_flow_result.status, StatusCode::kOk);
    EXPECT_EQ(0, std::memcmp(src, dst, chunk_size));
    EXPECT_EQ(1, data_flow_result.crc_output.size());
    EXPECT_EQ(ComputeCRC32(op.seed, reinterpret_cast<char*>(src), chunk_size),
              data_flow_result.crc_output.front());

    mp->Deallocate(src);
    mp->Deallocate(dst);
    dsa_engine->ClearCtx(&req);
}

TEST_F(DataFlowEnginesTest, DsaEngineMemmoveThenCrcTest) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());

    uint64_t chunk_size = 4096;
    void* src1 = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src1);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(src1), chunk_size);
    void* src2 = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src2);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(src2), chunk_size);
    void* dst = mp->Allocate(chunk_size * 2);
    EXPECT_NE(nullptr, dst);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(dst), chunk_size * 2);
    EXPECT_NE(0, std::memcmp(src1, dst, chunk_size));
    EXPECT_NE(0, std::memcmp(src2, (((char*)dst) + chunk_size), chunk_size));

    DataFlowMoveOperation op;
    op.src = {src1, src2};
    op.src_len = {chunk_size, chunk_size};
    op.dst = dst;
    op.seed = 0;
    op.enable_crc = true;

    data_flow::DataFlowRequest req;
    auto res = dsa_engine->PreallocateCtx(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    req.Build(&op, 1, nullptr);
    DataFlowResult data_flow_result;
    data_flow_result.status = StatusCode::kUnknown;

    res = dsa_engine->Submit(&req);
    EXPECT_EQ(res, StatusCode::kOk);
    while (data_flow_result.status != StatusCode::kOk) {
        dsa_engine->CheckCompletion(&req, &data_flow_result);
        usleep(1000);
    }
    EXPECT_EQ(data_flow_result.status, StatusCode::kOk);
    EXPECT_EQ(0, std::memcmp(src1, dst, chunk_size));
    EXPECT_EQ(0, std::memcmp(src2, (((char*)dst) + chunk_size), chunk_size));
    EXPECT_EQ(1, data_flow_result.crc_output.size());
    EXPECT_EQ(ComputeCRC32(op.seed, reinterpret_cast<char*>(dst), chunk_size * 2),
              data_flow_result.crc_output.front());

    mp->Deallocate(src1);
    mp->Deallocate(src2);
    mp->Deallocate(dst);
    dsa_engine->ClearCtx(&req);
}

TEST_F(DataFlowEnginesTest, DsaEngineOverlappingMemmoveThenCrcTest) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());

    uint64_t chunk_size = 4096;
    for (uint64_t chunk_num = 1; chunk_num <= 4; ++chunk_num) {
        std::vector<void*> srcs(chunk_num);
        for (uint64_t i = 0; i < chunk_num; ++i) {
            srcs[i] = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, srcs[i]);
            initialize_data_with_random(reinterpret_cast<uint8_t*>(srcs[i]), chunk_size);
        }
        void* dst = mp->Allocate(chunk_size * chunk_num);
        EXPECT_NE(nullptr, dst);
        initialize_data_with_random(reinterpret_cast<uint8_t*>(dst), chunk_size * chunk_num);
        DataFlowMoveOperation op;
        // overlapping copy and normal copy in rotation
        for (uint64_t i = 0; i < chunk_num; ++i) {
            if (i % 2 == 0) {
                memcpy((((char*)dst) + i * chunk_size), srcs[i], chunk_size);
                EXPECT_EQ(0, std::memcmp(srcs[i], (((char*)dst) + i * chunk_size), chunk_size));
                op.src.push_back(((char*)dst) + i * chunk_size);
            } else {
                EXPECT_NE(0, std::memcmp(srcs[i], (((char*)dst) + i * chunk_size), chunk_size));
                op.src.push_back(srcs[i]);
            }
            op.src_len.push_back(chunk_size);
        }
        op.dst = dst;
        op.seed = 0;
        op.enable_crc = true;

        data_flow::DataFlowRequest req;
        auto res = dsa_engine->PreallocateCtx(&req);
        EXPECT_EQ(res, StatusCode::kOk);
        req.Build(&op, 1, nullptr);
        DataFlowResult data_flow_result;
        data_flow_result.status = StatusCode::kUnknown;

        res = dsa_engine->Submit(&req);
        EXPECT_EQ(res, StatusCode::kOk);
        while (data_flow_result.status != StatusCode::kOk) {
            dsa_engine->CheckCompletion(&req, &data_flow_result);
            usleep(1000);
        }
        EXPECT_EQ(data_flow_result.status, StatusCode::kOk);
        for (uint64_t i = 0; i < chunk_num; ++i) {
            EXPECT_EQ(0, std::memcmp(srcs[i], ((char*)dst) + i * chunk_size, chunk_size));
            mp->Deallocate(srcs[i]);
        }
        EXPECT_EQ(1, data_flow_result.crc_output.size());
        EXPECT_EQ(ComputeCRC32(op.seed, reinterpret_cast<char*>(dst), chunk_size * chunk_num),
                  data_flow_result.crc_output.front());
        mp->Deallocate(dst);
        dsa_engine->ClearCtx(&req);
    }
}

}  // namespace vesal
