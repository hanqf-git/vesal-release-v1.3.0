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
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include "data_flow/dsa/dsa_ctx_factory.h"
#include "common/checksum_impl.h"
#include "common/memory_pool_helper.h"
#include "data_flow/data_flow_request.h"
#include "data_flow/data_flow_resource_manager.h"
#include "vesal/data_flow.h"
extern "C" {
#include "dsa_uio_config.h"
}
#include <gtest/gtest.h>
#include <cstring>
#include <random>

namespace vesal {

static void initialize_data_with_random(uint8_t* data, size_t size) {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<uint8_t> distribution(0, 255);

    for (size_t i = 0; i < size; ++i) {
        data[i] = distribution(generator);
    }
}

static inline void movdir64b(void* dst, const void* src) {
    asm volatile(".byte 0x66, 0x0f, 0x38, 0xf8, 0x02\t\n" : : "a"(dst), "d"(src));
}

class DsaCtxFactoryTest : public ::testing::Test {
public:
    void SetUp() override {
        // avoid data flow resource manager's dsa engine influence
        data_flow::DataFlowResourceManager* rm = data_flow::DataFlowResourceManager::GetInstance();
        rm->Uninit();
        factory.Init();
    }
    data_flow::DsaCtxFactory factory;
};

TEST_F(DsaCtxFactoryTest, SingleMemmove) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    DataFlowMoveOperation op;
    uint64_t chunk_size = 4096;
    void* src = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src);
    void* dst = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, dst);
    op.src = {src};
    op.src_len = {chunk_size};
    op.dst = {dst};
    data_flow::DataFlowRequest req;
    req.Build(&op, 1, nullptr);
    factory.PreallocateCtx(&req);
    factory.BuildCtx(&req);
    data_flow::DsaCtx* ctx = static_cast<data_flow::DsaCtx*>(req.engine_ctx);
    EXPECT_EQ(ctx->desc_num, 1);
    factory.ClearCtx(&req);
    mp->Deallocate(src);
    mp->Deallocate(dst);
}

TEST_F(DsaCtxFactoryTest, MemmoveWithNonDmaMemory) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    DataFlowMoveOperation op;
    data_flow::DataFlowRequest req;
    factory.PreallocateCtx(&req);
    uint64_t chunk_size = 4096;
    // single chunk
    {
        auto src = std::make_unique<char[]>(chunk_size);
        auto dst = std::make_unique<char[]>(chunk_size);
        op.src = {src.get()};
        op.src_len = {chunk_size};
        op.dst = {dst.get()};
        req.Build(&op, 1, nullptr);
        auto res = factory.BuildCtx(&req);
        EXPECT_EQ(res, StatusCode::kBadData);
    }
    // multi chunk, invalid dst
    {
        void* src1 = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, src1);
        void* src2 = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, src2);
        auto dst = std::make_unique<char[]>(chunk_size);
        op.src = {src1, src2};
        op.src_len = {chunk_size, chunk_size};
        op.dst = dst.get();
        req.Build(&op, 1, nullptr);
        auto res = factory.BuildCtx(&req);
        EXPECT_EQ(res, StatusCode::kBadData);
    }
    // multi chunk, invalid src
    {
        void* src1 = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, src1);
        auto src2 = std::make_unique<char[]>(chunk_size);
        void* dst = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, dst);
        op.src = {src1, src2.get()};
        op.src_len = {chunk_size, chunk_size};
        op.dst = dst;
        req.Build(&op, 1, nullptr);
        auto res = factory.BuildCtx(&req);
        EXPECT_EQ(res, StatusCode::kBadData);
    }
    factory.ClearCtx(&req);
}

TEST_F(DsaCtxFactoryTest, SingleCrc) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    DataFlowCrcOperation op;
    uint64_t chunk_size = 4096;
    void* src = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src);
    op.src = src;
    op.len = chunk_size;
    data_flow::DataFlowRequest req;
    req.Build(&op, 1, nullptr);
    factory.PreallocateCtx(&req);
    factory.BuildCtx(&req);
    data_flow::DsaCtx* ctx = static_cast<data_flow::DsaCtx*>(req.engine_ctx);
    EXPECT_EQ(ctx->desc_num, 1);
    factory.ClearCtx(&req);
    mp->Deallocate(src);
}

TEST_F(DsaCtxFactoryTest, CrcWithNonDmaMemory) {
    DataFlowCrcOperation op[2];
    uint64_t chunk_size = 4096;
    data_flow::DataFlowRequest req;
    factory.PreallocateCtx(&req);
    auto src1 = std::make_unique<char[]>(chunk_size);
    auto src2 = std::make_unique<char[]>(chunk_size);
    op[0].src = src1.get();
    op[0].len = chunk_size;
    op[1].src = src2.get();
    op[1].len = chunk_size;
    // single chunk
    {
        req.Build(op, 1, nullptr);
        auto res = factory.BuildCtx(&req);
        EXPECT_EQ(res, StatusCode::kBadData);
    }
    // multi chunk
    {
        req.Build(op, 2, nullptr);
        auto res = factory.BuildCtx(&req);
        EXPECT_EQ(res, StatusCode::kBadData);
    }
    factory.ClearCtx(&req);
}

TEST_F(DsaCtxFactoryTest, SingleMemmoveWithCrc) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    DataFlowMoveOperation op;
    uint64_t chunk_size = 4096;
    void* src = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src);
    void* dst = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, dst);
    op.src = {src};
    op.src_len = {chunk_size};
    op.dst = {dst};
    op.enable_crc = true;
    data_flow::DataFlowRequest req;
    req.Build(&op, 1, nullptr);
    factory.PreallocateCtx(&req);
    factory.BuildCtx(&req);
    data_flow::DsaCtx* ctx = static_cast<data_flow::DsaCtx*>(req.engine_ctx);
    EXPECT_EQ(ctx->desc_num, 1);
    factory.ClearCtx(&req);
    mp->Deallocate(src);
    mp->Deallocate(dst);
}

TEST_F(DsaCtxFactoryTest, MultipleMemmove) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    const uint64_t op_num = 10;
    DataFlowMoveOperation op[op_num];
    std::vector<void*> mem_records;
    uint64_t chunk_size = 4096;
    for (uint64_t i = 0; i < op_num; ++i) {
        void* src = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, src);
        void* dst = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, dst);
        op[i].src = {src};
        op[i].src_len = {chunk_size};
        op[i].dst = {dst};
        mem_records.push_back(src);
        mem_records.push_back(dst);
    }
    data_flow::DataFlowRequest req;
    req.Build(op, op_num, nullptr);
    factory.PreallocateCtx(&req);
    factory.BuildCtx(&req);
    data_flow::DsaCtx* ctx = static_cast<data_flow::DsaCtx*>(req.engine_ctx);
    // 1*(batch)+op_num*(copy)
    EXPECT_EQ(ctx->desc_num, op_num + 1);
    factory.ClearCtx(&req);
    for (void* mem : mem_records)
        mp->Deallocate(mem);
}

TEST_F(DsaCtxFactoryTest, MultipleMemmoveWithCrc) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    const uint64_t op_num = 10;
    DataFlowMoveOperation op[op_num];
    std::vector<void*> mem_records;
    uint64_t chunk_size = 4096;
    for (uint64_t i = 0; i < op_num; ++i) {
        void* src = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, src);
        void* dst = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, dst);
        op[i].src = {src};
        op[i].src_len = {chunk_size};
        op[i].dst = {dst};
        op[i].enable_crc = true;
        mem_records.push_back(src);
        mem_records.push_back(dst);
    }
    data_flow::DataFlowRequest req;
    req.Build(op, op_num, nullptr);
    factory.PreallocateCtx(&req);
    factory.BuildCtx(&req);
    data_flow::DsaCtx* ctx = static_cast<data_flow::DsaCtx*>(req.engine_ctx);
    // 1*(batch)+op_num*(copy with crc)
    EXPECT_EQ(ctx->desc_num, op_num + 1);
    factory.ClearCtx(&req);
    for (void* mem : mem_records)
        mp->Deallocate(mem);
}

TEST_F(DsaCtxFactoryTest, MultipleCrc) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    const uint64_t op_num = 10;
    DataFlowCrcOperation op[op_num];
    std::vector<void*> mem_records;
    uint64_t chunk_size = 4096;
    for (uint64_t i = 0; i < op_num; ++i) {
        void* src = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, src);
        op[i].src = src;
        op[i].len = chunk_size;
        mem_records.push_back(src);
    }
    data_flow::DataFlowRequest req;
    req.Build(op, op_num, nullptr);
    factory.PreallocateCtx(&req);
    factory.BuildCtx(&req);
    data_flow::DsaCtx* ctx = static_cast<data_flow::DsaCtx*>(req.engine_ctx);
    // 1*(batch)+op_num*(crc)
    EXPECT_EQ(ctx->desc_num, op_num + 1);
    factory.ClearCtx(&req);
    for (void* mem : mem_records)
        mp->Deallocate(mem);
}

TEST_F(DsaCtxFactoryTest, MergeChunksAndComputeCrc) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    DataFlowMoveOperation op;
    const uint64_t chunk_num = 10;
    std::vector<void*> mem_records;
    uint64_t chunk_size = 4096;
    for (uint64_t i = 0; i < chunk_num; ++i) {
        void* src = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, src);
        op.src.push_back(src);
        op.src_len.push_back(chunk_size);
        mem_records.push_back(src);
    }
    void* dst = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, dst);
    op.dst = {dst};
    op.enable_crc = true;
    data_flow::DataFlowRequest req;
    req.Build(&op, 1, nullptr);
    factory.PreallocateCtx(&req);
    factory.BuildCtx(&req);
    data_flow::DsaCtx* ctx = static_cast<data_flow::DsaCtx*>(req.engine_ctx);
    // 1*(batch)+chunk_num*(copy)+1*(fence)+1*(crc)
    EXPECT_EQ(ctx->desc_num, 3 + chunk_num);
    factory.ClearCtx(&req);
    for (void* mem : mem_records)
        mp->Deallocate(mem);
    mp->Deallocate(dst);
}

TEST_F(DsaCtxFactoryTest, SubmitCopyDescriptor) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    size_t chunk_size = 4096;
    for (size_t i = 0; i < 5; ++i) {
        void* src = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, src);
        initialize_data_with_random(reinterpret_cast<uint8_t*>(src), chunk_size);
        void* dst = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, dst);
        initialize_data_with_random(reinterpret_cast<uint8_t*>(dst), chunk_size);
        void* completion_record_chunk = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, completion_record_chunk);
        uintptr_t completion_record = factory.GetNextAlignedAddress(
            reinterpret_cast<uintptr_t>(completion_record_chunk), GetDsaCompletionRecordSize());
        memset(reinterpret_cast<void*>(completion_record), 0, GetDsaCompletionRecordSize());
        EXPECT_NE(0, std::memcmp(src, dst, chunk_size));
        void* desc = mp->Allocate(GetDsaHwDescSize());
        EXPECT_NE(nullptr, desc);
        memset(desc, 0, GetDsaHwDescSize());
        FillCopy(reinterpret_cast<uintptr_t>(desc),
                 LookUpAddr(src),
                 chunk_size,
                 LookUpAddr(dst),
                 LookUpAddr(reinterpret_cast<void*>(completion_record)));
        InitDsaConfig();
        UioWqInfo uio_info;
        EXPECT_EQ(true, GetNextWorkQueue(&uio_info));
        void* portal = uio_info.portal;
        movdir64b(portal, desc);
        while (!DsaCheckCompletion(completion_record))
            usleep(1000);
        EXPECT_TRUE(DsaCheckCompletion(completion_record));
        EXPECT_EQ(0, std::memcmp(src, dst, chunk_size));
        ReturnWorkQueue(&uio_info);
        mp->Deallocate(src);
        mp->Deallocate(dst);
        mp->Deallocate(completion_record_chunk);
        mp->Deallocate(desc);
    }
}

TEST_F(DsaCtxFactoryTest, SubmitCrcDescriptor) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    size_t chunk_size = 4096;
    void* src = mp->Allocate(chunk_size);
    EXPECT_NE(nullptr, src);
    initialize_data_with_random(reinterpret_cast<uint8_t*>(src), chunk_size);
    std::vector<uint32_t> seeds = {0U, ~0U, 123, 456};
    for (uint32_t seed : seeds) {
        void* completion_record_chunk = mp->Allocate(chunk_size);
        EXPECT_NE(nullptr, completion_record_chunk);
        uintptr_t completion_record = factory.GetNextAlignedAddress(
            reinterpret_cast<uintptr_t>(completion_record_chunk), GetDsaCompletionRecordSize());
        memset(reinterpret_cast<void*>(completion_record), 0, GetDsaCompletionRecordSize());
        void* desc = mp->Allocate(GetDsaHwDescSize());
        EXPECT_NE(nullptr, desc);
        memset(desc, 0, GetDsaHwDescSize());
        FillCrc(reinterpret_cast<uintptr_t>(desc),
                LookUpAddr(src),
                chunk_size,
                VESAL_CRC_REVISE(seed),
                LookUpAddr(reinterpret_cast<void*>(completion_record)));
        InitDsaConfig();
        UioWqInfo uio_info;
        EXPECT_EQ(true, GetNextWorkQueue(&uio_info));
        void* portal = uio_info.portal;
        movdir64b(portal, desc);
        while (!DsaCheckCompletion(completion_record))
            usleep(1000);
        EXPECT_TRUE(DsaCheckCompletion(completion_record));
        uint32_t hw_res = VESAL_CRC_REVISE(GetCrcInDsaCompletionRecord(completion_record));
        uint32_t sw_res = ComputeCRC32(seed, static_cast<const char*>(src), chunk_size);
        EXPECT_EQ(hw_res, sw_res);
        ReturnWorkQueue(&uio_info);
        mp->Deallocate(completion_record_chunk);
        mp->Deallocate(desc);
    }
    mp->Deallocate(src);
}

TEST_F(DsaCtxFactoryTest, SubmitCopyWithCrcDescriptor) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    size_t chunk_size = 4096;
    std::vector<uint32_t> seeds = {0U, ~0U, 123, 456};
    for (uint32_t seed : seeds) {
        for (size_t i = 0; i < 5; ++i) {
            void* src = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, src);
            initialize_data_with_random(reinterpret_cast<uint8_t*>(src), chunk_size);
            void* dst = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, dst);
            initialize_data_with_random(reinterpret_cast<uint8_t*>(dst), chunk_size);
            void* completion_record_chunk = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, completion_record_chunk);
            uintptr_t completion_record = factory.GetNextAlignedAddress(
                reinterpret_cast<uintptr_t>(completion_record_chunk), GetDsaCompletionRecordSize());
            memset(reinterpret_cast<void*>(completion_record), 0, GetDsaCompletionRecordSize());
            EXPECT_NE(0, std::memcmp(src, dst, chunk_size));
            void* desc = mp->Allocate(GetDsaHwDescSize());
            EXPECT_NE(nullptr, desc);
            memset(desc, 0, GetDsaHwDescSize());
            FillCopyWithCrc(reinterpret_cast<uintptr_t>(desc),
                            LookUpAddr(src),
                            chunk_size,
                            LookUpAddr(dst),
                            VESAL_CRC_REVISE(seed),
                            LookUpAddr(reinterpret_cast<void*>(completion_record)));
            InitDsaConfig();
            UioWqInfo uio_info;
            EXPECT_EQ(true, GetNextWorkQueue(&uio_info));
            void* portal = uio_info.portal;
            movdir64b(portal, desc);
            while (!DsaCheckCompletion(completion_record))
                usleep(1000);
            EXPECT_TRUE(DsaCheckCompletion(completion_record));
            EXPECT_EQ(0, std::memcmp(src, dst, chunk_size));
            uint32_t hw_res = VESAL_CRC_REVISE(GetCrcInDsaCompletionRecord(completion_record));
            uint32_t sw_res = ComputeCRC32(seed, static_cast<const char*>(src), chunk_size);
            EXPECT_EQ(hw_res, sw_res);
            ReturnWorkQueue(&uio_info);
            mp->Deallocate(src);
            mp->Deallocate(dst);
            mp->Deallocate(completion_record_chunk);
            mp->Deallocate(desc);
        }
    }
}

TEST_F(DsaCtxFactoryTest, SubmitBatchDescriptor) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    size_t chunk_size = 4096;
    std::vector<uint32_t> seeds = {0U, ~0U, 123, 456};
    for (uint32_t seed : seeds) {
        for (size_t i = 0; i < 5; ++i) {
            void* src1 = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, src1);
            initialize_data_with_random(reinterpret_cast<uint8_t*>(src1), chunk_size);
            void* src2 = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, src2);
            initialize_data_with_random(reinterpret_cast<uint8_t*>(src2), chunk_size);

            void* completion_record_chunk = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, completion_record_chunk);
            uintptr_t batch_completion_record = factory.GetNextAlignedAddress(
                reinterpret_cast<uintptr_t>(completion_record_chunk), GetDsaCompletionRecordSize());
            uintptr_t completion_record1 = batch_completion_record + GetDsaCompletionRecordSize();
            uintptr_t completion_record2 = completion_record1 + GetDsaCompletionRecordSize();
            memset(
                reinterpret_cast<void*>(batch_completion_record), 0, GetDsaCompletionRecordSize());

            void* desc_chunk = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, desc_chunk);
            memset(desc_chunk, 0, chunk_size);
            uintptr_t batch_desc = factory.GetNextAlignedAddress(
                reinterpret_cast<uintptr_t>(desc_chunk), GetDsaHwDescSize());
            uintptr_t desc1 = batch_desc + GetDsaHwDescSize();
            uintptr_t desc2 = desc1 + GetDsaHwDescSize();

            FillCrc(desc1,
                    LookUpAddr(src1),
                    chunk_size,
                    VESAL_CRC_REVISE(seed),
                    LookUpAddr(reinterpret_cast<void*>(completion_record1)));
            FillCrc(desc2,
                    LookUpAddr(src2),
                    chunk_size,
                    VESAL_CRC_REVISE(seed),
                    LookUpAddr(reinterpret_cast<void*>(completion_record2)));
            FillBatch(batch_desc,
                      2,
                      LookUpAddr(reinterpret_cast<void*>(desc1)),
                      LookUpAddr(reinterpret_cast<void*>(batch_completion_record)));

            InitDsaConfig();
            UioWqInfo uio_info;
            EXPECT_EQ(true, GetNextWorkQueue(&uio_info));
            void* portal = uio_info.portal;
            movdir64b(portal, reinterpret_cast<void*>(batch_desc));
            while (!DsaCheckCompletion(batch_completion_record))
                usleep(1000);
            EXPECT_TRUE(DsaCheckCompletion(batch_completion_record));
            uint32_t hw_res1 = VESAL_CRC_REVISE(GetCrcInDsaCompletionRecord(completion_record1));
            uint32_t sw_res1 = ComputeCRC32(seed, static_cast<const char*>(src1), chunk_size);
            EXPECT_EQ(hw_res1, sw_res1);
            uint32_t hw_res2 = VESAL_CRC_REVISE(GetCrcInDsaCompletionRecord(completion_record2));
            uint32_t sw_res2 = ComputeCRC32(seed, static_cast<const char*>(src2), chunk_size);
            EXPECT_EQ(hw_res2, sw_res2);
            ReturnWorkQueue(&uio_info);
            mp->Deallocate(src1);
            mp->Deallocate(src2);
            mp->Deallocate(desc_chunk);
            mp->Deallocate(completion_record_chunk);
        }
    }
}

TEST_F(DsaCtxFactoryTest, SubmitBatchWithFenceDescriptor) {
    auto mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    size_t chunk_size = 4096;
    std::vector<uint32_t> seeds = {0U, ~0U, 123, 456};
    for (uint32_t seed : seeds) {
        for (size_t i = 0; i < 5; ++i) {
            void* src = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, src);
            initialize_data_with_random(reinterpret_cast<uint8_t*>(src), chunk_size);
            void* dst = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, dst);
            initialize_data_with_random(reinterpret_cast<uint8_t*>(dst), chunk_size);
            EXPECT_NE(0, std::memcmp(src, dst, chunk_size));

            void* completion_record_chunk = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, completion_record_chunk);
            uintptr_t batch_completion_record = factory.GetNextAlignedAddress(
                reinterpret_cast<uintptr_t>(completion_record_chunk), GetDsaCompletionRecordSize());
            uintptr_t copy_completion_record =
                batch_completion_record + GetDsaCompletionRecordSize();
            uintptr_t fence_completion_record =
                copy_completion_record + GetDsaCompletionRecordSize();
            uintptr_t crc_completion_record =
                fence_completion_record + GetDsaCompletionRecordSize();
            memset(
                reinterpret_cast<void*>(batch_completion_record), 0, GetDsaCompletionRecordSize());

            void* desc_chunk = mp->Allocate(chunk_size);
            EXPECT_NE(nullptr, desc_chunk);
            memset(desc_chunk, 0, chunk_size);
            uintptr_t batch_desc = factory.GetNextAlignedAddress(
                reinterpret_cast<uintptr_t>(desc_chunk), GetDsaHwDescSize());
            uintptr_t copy_desc = batch_desc + GetDsaHwDescSize();
            uintptr_t fence_desc = copy_desc + GetDsaHwDescSize();
            uintptr_t crc_desc = fence_desc + GetDsaHwDescSize();

            FillBatch(batch_desc,
                      3,
                      LookUpAddr(reinterpret_cast<void*>(copy_desc)),
                      LookUpAddr(reinterpret_cast<void*>(batch_completion_record)));
            FillCopy(copy_desc,
                     LookUpAddr(src),
                     chunk_size,
                     LookUpAddr(dst),
                     LookUpAddr(reinterpret_cast<void*>(copy_completion_record)));
            FillFence(fence_desc, LookUpAddr(reinterpret_cast<void*>(fence_completion_record)));
            FillCrc(crc_desc,
                    LookUpAddr(dst),
                    chunk_size,
                    VESAL_CRC_REVISE(seed),
                    LookUpAddr(reinterpret_cast<void*>(crc_completion_record)));
            InitDsaConfig();
            UioWqInfo uio_info;
            EXPECT_EQ(true, GetNextWorkQueue(&uio_info));
            void* portal = uio_info.portal;
            movdir64b(portal, reinterpret_cast<void*>(batch_desc));
            while (!DsaCheckCompletion(batch_completion_record))
                usleep(1000);
            EXPECT_TRUE(DsaCheckCompletion(batch_completion_record));
            EXPECT_EQ(0, std::memcmp(src, dst, chunk_size));
            uint32_t hw_res = VESAL_CRC_REVISE(GetCrcInDsaCompletionRecord(crc_completion_record));
            uint32_t sw_res = ComputeCRC32(seed, static_cast<const char*>(dst), chunk_size);
            EXPECT_EQ(hw_res, sw_res);
            ReturnWorkQueue(&uio_info);
            mp->Deallocate(src);
            mp->Deallocate(dst);
            mp->Deallocate(desc_chunk);
            mp->Deallocate(completion_record_chunk);
        }
    }
}

}  // namespace vesal
#endif