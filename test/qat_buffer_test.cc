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

#include "common/qat/qat_buffer.h"

#include <signal.h>

#include <cstdlib>
#include <numeric>

#include "codec/qat/qat_codec.h"
#include "codec/qat/qat_codec_engine.h"
#include "common/defer.h"
#include "common/memory_pool_helper.h"
#include "common/qat/qat_unit.h"
#include "common/qat/qat_util.h"
#include "gtest/gtest-death-test.h"
#include "gtest/gtest.h"
#include "vesal/io_vector.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"

extern "C" {
#include <dc/cpa_dc.h>
}

namespace vesal {
namespace qat {

class MockQatUnit : public qat::QatUnit {
public:
    MockQatUnit() : qat::QatUnit(CpaInstanceHandle()) {
        VESAL_CHECK(QatUserStart());
        VESAL_CHECK(MemoryPool::GetInstance()->Init());
        VESAL_CHECK(cpaDcGetInstances(1, &cpa_instance_handle_) == CPA_STATUS_SUCCESS);
    }
    ~MockQatUnit() {
        VESAL_CHECK(Stop().ok());
        VESAL_CHECK(QatUserStop());
        MemoryPool::GetInstance()->Reset();
    }

    void DisableSVM() {
        qat_unit_attr_.is_phys_contiguous_mem_required = true;
    }

    void EnableSVM() {
        qat_unit_attr_.is_phys_contiguous_mem_required = false;
    }
};

TEST(QatBufCacheTest, BasicApisWithSvmAndNonSvm) {
    MockQatUnit mock_qat_unit;
    mock_qat_unit.DisableSVM();
    std::unique_ptr<qat::QatBufCache> qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, 128);
    EXPECT_TRUE(qat_buf_cache);
    EXPECT_FALSE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(qat_buf_cache->Init());
    qat::QatBuf* buf = qat_buf_cache->GetOne();
    EXPECT_TRUE(buf);
    qat_buf_cache->ReturnOne(buf);
    buf = nullptr;

    mock_qat_unit.EnableSVM();
    std::unique_ptr<qat::QatBufCache> svm_qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, 128);
    EXPECT_TRUE(svm_qat_buf_cache);
    EXPECT_TRUE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(svm_qat_buf_cache->Init());
    buf = svm_qat_buf_cache->GetOne();
    EXPECT_TRUE(buf);
    svm_qat_buf_cache->ReturnOne(buf);
}

TEST(QatBufTest, BasicApisWithSvmAndNonSvm) {
    using ustring = std::basic_string<unsigned char>;
    ustring s1 = (const unsigned char*)"str1";
    ustring s2 = (const unsigned char*)"str114514";
    IOBlock blocks[2];
    blocks[0] = {(uint32_t)s1.size(), &s1[0]};
    blocks[1] = {(uint32_t)s2.size(), &s2[0]};
    IOVector srcs = {2, blocks};

    MockQatUnit mock_qat_unit;
    mock_qat_unit.DisableSVM();
    std::unique_ptr<qat::QatBufCache> qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, 128);
    EXPECT_TRUE(qat_buf_cache);
    EXPECT_FALSE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(qat_buf_cache->Init());
    qat::QatBuf* buf = qat_buf_cache->GetOne();
    EXPECT_TRUE(buf);
    EXPECT_TRUE(buf->FillDst(blocks[0]));
    buf->DecompCopyBackIfNecessary(blocks[0].size);
    buf->FreeDataIfNecessary();
    EXPECT_TRUE(buf->FillDst(blocks[1]));
    buf->DecompCopyBackIfNecessary(blocks[1].size);
    buf->FreeDataIfNecessary();
    EXPECT_TRUE(buf->FillSrc(srcs));
    buf->FreeDataIfNecessary();

    qat_buf_cache->ReturnOne(buf);

    mock_qat_unit.EnableSVM();
    std::unique_ptr<qat::QatBufCache> svm_qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, 128);
    EXPECT_TRUE(svm_qat_buf_cache);
    EXPECT_TRUE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(svm_qat_buf_cache->Init());
    buf = svm_qat_buf_cache->GetOne();
    EXPECT_TRUE(buf);

    EXPECT_TRUE(buf->FillDst(blocks[0]));
    buf->DecompCopyBackIfNecessary(blocks[0].size);
    buf->FreeDataIfNecessary();
    EXPECT_TRUE(buf->FillDst(blocks[1]));
    buf->DecompCopyBackIfNecessary(blocks[1].size);
    buf->FreeDataIfNecessary();
    EXPECT_TRUE(buf->FillSrc(srcs));
    buf->FreeDataIfNecessary();

    svm_qat_buf_cache->ReturnOne(buf);
}

TEST(QatBufTest, DmaableAndNonSvm) {
    MockQatUnit mock_qat_unit;
    mock_qat_unit.DisableSVM();
    std::unique_ptr<qat::QatBufCache> qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, 128);
    EXPECT_TRUE(qat_buf_cache);
    EXPECT_FALSE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(qat_buf_cache->Init());

    auto* buf = qat_buf_cache->GetOne();
    EXPECT_TRUE(buf);

    const size_t block_num = 10;
    IOBlock blocks[10];
    IOVector srcs = {block_num, blocks};
    unsigned int sz = 512 * 1024UL;
    for (size_t i = 0; i < block_num; i++) {
        unsigned char* block =
            reinterpret_cast<unsigned char*>(MemoryPool::GetInstance()->Allocate(sz));
        EXPECT_TRUE(block);
        blocks[i] = {sz, block};
    }

    EXPECT_TRUE(buf->FillDst(blocks[0]));
    buf->FreeDataIfNecessary();
    EXPECT_TRUE(buf->FillDst(blocks[0]));
    buf->FreeDataIfNecessary();
    EXPECT_TRUE(buf->FillSrc(srcs));
    buf->FreeDataIfNecessary();

    for (size_t i = 0; i < block_num; i++) {
        MemoryPool::GetInstance()->Deallocate(blocks[i].data);
    }
    qat_buf_cache->ReturnOne(buf);
}

TEST(QatBufCacheTest, NotEnoughDmaMemory) {
    MockQatUnit mock_qat_unit;
    mock_qat_unit.DisableSVM();
    std::unique_ptr<qat::QatBufCache> qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, 128);
    EXPECT_TRUE(qat_buf_cache);
    EXPECT_FALSE(mock_qat_unit.SvmEnabled());

    unsigned int sz = 0;
    CpaInstanceHandle* inst_handle = mock_qat_unit.GetInstanceHandle();
    CpaStatus cpa_status = cpaDcBufferListGetMetaSize(*inst_handle, VESAL_MAX_SGL_NUM, &sz);
    EXPECT_EQ(cpa_status, CPA_STATUS_SUCCESS);
    std::vector<unsigned char*> blocks;
    // now run out of memory
    while (true) {
        auto* block = (unsigned char*)MemoryPool::GetInstance()->Allocate(sz);
        if (!block) {
            break;
        }
        blocks.push_back(block);
    }
    EXPECT_FALSE(qat_buf_cache->Init());

    // return the memory
    for (auto* each : blocks) {
        MemoryPool::GetInstance()->Deallocate(each);
    }
    blocks.clear();
    EXPECT_TRUE(qat_buf_cache->Init());
    auto* buf = qat_buf_cache->GetOne();
    EXPECT_NE(buf, nullptr);
    // now run out of memory again
    while (true) {
        auto* block = (unsigned char*)MemoryPool::GetInstance()->Allocate(sz);
        if (!block) {
            break;
        }
        blocks.push_back(block);
    }
    using ustring = std::basic_string<unsigned char>;
    ustring s1 = (const unsigned char*)"str1";
    ustring s2 = (const unsigned char*)"str114514";
    IOBlock b[2];
    b[0] = {(uint32_t)s1.size(), &s1[0]};
    b[1] = {(uint32_t)s2.size(), &s2[0]};
    IOVector srcs = {2, b};
    // now return one, still not enough
    MemoryPool::GetInstance()->Deallocate(blocks.back());
    blocks.pop_back();
    EXPECT_FALSE(buf->FillSrc(srcs));
    buf->FreeDataIfNecessary();
    // now run out the only one block memory again
    while (true) {
        auto* block = (unsigned char*)MemoryPool::GetInstance()->Allocate(sz);
        if (!block) {
            break;
        }
        blocks.push_back(block);
    }
    EXPECT_FALSE(buf->FillDst(b[0]));
    buf->FreeDataIfNecessary();
    EXPECT_FALSE(buf->FillDst(b[1]));
    buf->FreeDataIfNecessary();

    qat_buf_cache->ReturnOne(buf);
    // return the memory
    for (auto* each : blocks) {
        MemoryPool::GetInstance()->Deallocate(each);
    }
}

TEST(QatBufTest, BasicApisWithCopyMode) {
    using ustring = std::basic_string<unsigned char>;
    ustring s1 = (const unsigned char*)"str1";
    ustring s2 = (const unsigned char*)"str114514";
    IOBlock blocks[2];
    blocks[0] = {(uint32_t)s1.size(), &s1[0]};
    blocks[1] = {(uint32_t)s2.size(), &s2[0]};
    IOVector srcs = {2, blocks};

    MockQatUnit mock_qat_unit;
    mock_qat_unit.DisableSVM();
    std::unique_ptr<qat::QatBufCache> qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, 128);
    EXPECT_TRUE(qat_buf_cache);
    EXPECT_FALSE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(qat_buf_cache->Init());

    auto* buf = qat_buf_cache->GetOne();

    EXPECT_TRUE(buf->FillDst(blocks[0]));
    buf->DecompCopyBackIfNecessary(blocks[0].size);
    buf->FreeDataIfNecessary();
    EXPECT_TRUE(buf->FillDst(blocks[1]));
    buf->DecompCopyBackIfNecessary(blocks[1].size);
    buf->FreeDataIfNecessary();
    EXPECT_TRUE(buf->FillSrc(srcs));
    buf->FreeDataIfNecessary();
    EXPECT_TRUE(buf->FillSrc(srcs));
    buf->FreeDataIfNecessary();

    qat_buf_cache->ReturnOne(buf);
}

TEST(QatBuf, FillSrcCorrect) {
    MockQatUnit mock_qat_unit;
    mock_qat_unit.DisableSVM();
    std::unique_ptr<qat::QatBufCache> qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, g_max_qat_cfg_concurrency);
    EXPECT_TRUE(qat_buf_cache);
    EXPECT_FALSE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(qat_buf_cache->Init());
    auto* buf = qat_buf_cache->GetOne();

    IOBlock blocks[11];
    std::vector<unsigned int> lens{1, 2, 4, 8, 16, 32, 16, 8, 4, 2, 1};
    for (size_t i = 0; i < lens.size(); ++i) {
        blocks[i] = {(uint32_t)lens[i], new unsigned char[lens[i]]};
        for (size_t j = 0; j < lens[i]; ++j) {
            blocks[i].data[j] = i + '0';
        }
    }
    IOVector srcs = {11, blocks};

    buf->FillSrc(srcs, 6, 7);
    EXPECT_EQ(buf->cpa_buffer_list_->numBuffers, 6);
    EXPECT_EQ(buf->cpa_buffer_list_->pBuffers[0].dataLenInBytes, 1);
    EXPECT_EQ(buf->cpa_buffer_list_->pBuffers[0].pData[0], '2');
    EXPECT_EQ(buf->cpa_buffer_list_->pBuffers[5].dataLenInBytes, 8);
    EXPECT_EQ(buf->cpa_buffer_list_->pBuffers[5].pData[0], '7');
    buf->FreeDataIfNecessary();

    buf->FillSrc(srcs);
    EXPECT_EQ(buf->cpa_buffer_list_->numBuffers, srcs.num_blocks);
    EXPECT_EQ(buf->cpa_buffer_list_->pBuffers[0].dataLenInBytes, 1);
    EXPECT_EQ(buf->cpa_buffer_list_->pBuffers[0].pData[0], '0');
    EXPECT_EQ(buf->cpa_buffer_list_->pBuffers[10].dataLenInBytes, 1);
    EXPECT_EQ(buf->cpa_buffer_list_->pBuffers[10].pData[0], '9' + 1);
    buf->FreeDataIfNecessary();

    for (IOBlock& b : blocks) {
        delete[] b.data;
    }
    buf->FreeDataIfNecessary();

    // then case for fast path
    blocks[0] = {(uint32_t)4096, new unsigned char[4096]};
    memset(blocks[0].data, 'x', 4096);
    // set different for data block part
    memset(&blocks[0].data[6], 'y', 4096 - 6 - 7);
    srcs = {1, blocks};
    buf->FillSrc(srcs, 6, 7);
    EXPECT_EQ(buf->cpa_buffer_list_->numBuffers, 1);
    EXPECT_EQ(buf->cpa_buffer_list_->pBuffers->dataLenInBytes, 4096 - 7 - 6);
    // expect the data part is all 'y'
    EXPECT_EQ(memcmp(buf->cpa_buffer_list_->pBuffers->pData, &blocks[0].data[6], 4096 - 6 - 7), 0);
    delete[] blocks[0].data;
    buf->FreeDataIfNecessary();
    qat_buf_cache->ReturnOne(buf);
}

TEST(QatBufCache, TrivialCases) {
    // ignore when return nullptr
    MockQatUnit mock_qat_unit;
    QatBufCache* buf_cache = new QatBufCache(&mock_qat_unit, 128);
    EXPECT_TRUE(buf_cache->Init());
    buf_cache->ReturnOne(nullptr);
    // allocation must match
    buf_cache->in_cache_num_++;
    EXPECT_DEATH(buf_cache->Clear(), ".*");
    buf_cache->in_cache_num_--;
    delete buf_cache;
}

TEST(QatBuf, TrivialCases) {
    MockQatUnit mock_qat_unit;
    QatBuf* buf = new QatBuf();
    EXPECT_TRUE(buf->InitMeta(&mock_qat_unit, 2048));
    buf->cpa_buffer_list_->numBuffers = 1;
    EXPECT_DEATH(buf->FreeMeta(), ".*");
    buf->cpa_buffer_list_->numBuffers = 0;
    buf->FreeMeta();
    delete buf;
}

TEST(QatBuf, FillSrcIncorrect) {
    MockQatUnit mock_qat_unit;
    mock_qat_unit.DisableSVM();
    std::unique_ptr<qat::QatBufCache> qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, g_max_qat_cfg_concurrency);
    EXPECT_TRUE(qat_buf_cache);
    EXPECT_FALSE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(qat_buf_cache->Init());
    auto* buf = qat_buf_cache->GetOne();
    auto g = defer([&]() {
        buf->FreeDataIfNecessary();
        qat_buf_cache->ReturnOne(buf);
    });

    std::vector<unsigned int> lens{1, 2, 4, 8, 16, 32, 16, 8, 4, 2, 1};
    IOBlock blocks[11];
    auto total_len = std::accumulate(lens.begin(), lens.end(), 0);
    for (size_t i = 0; i < lens.size(); ++i) {
        blocks[i] = {(uint32_t)lens[i], new unsigned char[lens[i]]};
        for (size_t j = 0; j < lens[i]; ++j) {
            blocks[i].data[j] = i + '0';
        }
    }
    IOVector srcs = {11, blocks};
    auto g1 = defer([&]() {
        for (auto each : blocks) {
            delete[] each.data;
        }
    });
    // Header and footer too big
    EXPECT_FALSE(buf->FillSrc(srcs, total_len / 2 + 1, total_len / 2 + 1));
    // Then test fast path FillOneBuffer failed. First drain all dma memory
    std::vector<void*> tmp;
    void* p = nullptr;
    while (true) {
        p = MemoryPool::GetInstance()->Allocate(32);
        if (!p) {
            break;
        }
        tmp.push_back(p);
    }
    // Failed due to insufficient memory
    EXPECT_FALSE(buf->FillSrc(srcs, 31, 31));
    for (auto p : tmp) {
        MemoryPool::GetInstance()->Deallocate(p);
    }
}

TEST(QatBuf, FillDstInvalidOffset) {
    MockQatUnit mock_qat_unit;
    mock_qat_unit.DisableSVM();
    std::unique_ptr<qat::QatBufCache> qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, g_max_qat_cfg_concurrency);
    EXPECT_TRUE(qat_buf_cache);
    EXPECT_FALSE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(qat_buf_cache->Init());
    auto* buf = qat_buf_cache->GetOne();
    auto g = defer([&]() {
        buf->FreeDataIfNecessary();
        qat_buf_cache->ReturnOne(buf);
    });

    auto data = new unsigned char[4096];
    unsigned int len = 4096;
    IOBlock dst = {(uint32_t)len, data};
    // Header and footer too big will fail, note we treat empty dst as failure
    EXPECT_FALSE(buf->FillDst(dst, 2048, 2048));
    EXPECT_TRUE(buf->FillDst(dst, 2048, 2048 - 1));
    delete[] data;
}

TEST(QatBuf, ExtendQatBufCache) {
    MockQatUnit mock_qat_unit;
    mock_qat_unit.DisableSVM();
    std::unique_ptr<qat::QatBufCache> qat_buf_cache =
        std::make_unique<qat::QatBufCache>(&mock_qat_unit, g_max_qat_cfg_concurrency);
    EXPECT_TRUE(qat_buf_cache);
    EXPECT_FALSE(mock_qat_unit.SvmEnabled());
    EXPECT_TRUE(qat_buf_cache->Init());
    EXPECT_EQ(g_max_qat_cfg_concurrency * 2 + g_max_qat_cfg_concurrency / 2,
              qat_buf_cache->buf_size_);
}

}  // namespace qat
}  // namespace vesal
