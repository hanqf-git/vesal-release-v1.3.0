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

#include "vesal/memory_pool.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "common/memory_pool_helper.h"
#include "vesal/vesal.h"

namespace vesal {

TEST(MemoryPoolAPITest, ReInitMemoryPoolSuccess) {
    MemoryPool* mp = MemoryPool::GetInstance();
    ASSERT_TRUE(mp->Init());
    void* addr = mp->Allocate(1024);
    ASSERT_NE(addr, nullptr);
    mp->Deallocate(addr);
    mp->Reset();
    ASSERT_TRUE(mp->Init());
    addr = mp->Allocate(1024);
    ASSERT_NE(addr, nullptr);
    mp->Deallocate(addr);
    mp->Reset();
}

TEST(MemoryPoolAPITest, AllocDeallocMultiThreadSucc) {
    MemoryPool* mp = MemoryPool::GetInstance();
    ASSERT_TRUE(mp->Init());
    size_t thread_num = 2;

    auto fn = [&mp]() {
        std::vector<void*> mm;
        for (size_t i = 1; i <= 512 * 1024; i <<= 1) {
            mm.push_back(mp->Allocate(i));
            ASSERT_TRUE(mm.back());
        }
        for (auto& m : mm) {
            mp->Deallocate(m);
        }
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < thread_num; ++i) {
        threads.emplace_back(fn);
    }
    for (size_t i = 0; i < thread_num; ++i) {
        threads[i].join();
    }
    mp->Reset();
}

TEST(MemoryPoolAPITest, RegisterUnregisterHugepageMultiThreadSucc) {
    MemoryPool* mp = MemoryPool::GetInstance();
    ASSERT_TRUE(mp->Init());
    size_t thread_num = 3;
    size_t register_num = 16;
    size_t page_size = 2 * 1024 * 1024;

    auto fn = [&]() {
        std::vector<MemoryInfo> mm;
        for (size_t i = 0; i < register_num; ++i) {
            MemoryInfo info{.virtual_addr = AllocHugepageFn(page_size),
                            .physical_addr = nullptr,
                            .len = page_size,
                            .page_size = page_size};
            ASSERT_TRUE(info.virtual_addr);
            mm.push_back(info);
        }
        ASSERT_TRUE(mp->Register(mm));
    };

    std::vector<std::thread> threads;
    for (size_t i = 0; i < thread_num; ++i) {
        threads.emplace_back(fn);
    }
    for (size_t i = 0; i < thread_num; ++i) {
        threads[i].join();
    }
    mp->Reset();
}

// allocate
TEST(MemoryPoolAPITest, MultithreadLookup) {
    MemoryPool* mp = MemoryPool::GetInstance();
    ASSERT_TRUE(mp->Init());
    size_t register_num = 16;
    std::vector<MemoryInfo> mi;
    std::vector<PhysChunk> mm;
    uint64_t lookup_step = 64;
    size_t page_size = 2 * 1024 * 1024;
    size_t thread_num = 2;

    for (size_t i = 0; i < register_num; ++i) {
        MemoryInfo info{.virtual_addr = AllocHugepageFn(page_size),
                        .physical_addr = nullptr,
                        .len = page_size,
                        .page_size = page_size};
        ASSERT_TRUE(info.virtual_addr);
        mi.push_back(info);
    }
    ASSERT_TRUE(mp->Register(mi));
    for (size_t i = 1; i <= 512 * 1024; i <<= 1) {
        void* addr = mp->Allocate(i);
        mm.push_back(PhysChunk{
            .start_addr = addr, .phys_addr = 0, .align_type = PageSizeToAlignType(page_size)});
        ASSERT_TRUE(mm.back().start_addr);
    }

    // look up for all pages' different part
    auto lookup_fn = [&]() {
        for (size_t i = 0; i < mm.size(); ++i) {
            uint64_t offset = 0;
            uint64_t start = reinterpret_cast<uint64_t>(mm[i].start_addr);
            // the address from allocate might not be real beginning of one page, align first
            uint64_t page_end =
                (uint64_t)AlignPage(mm[i].start_addr, AlignType::kPage1Gb) + page_size;
            for (; start + offset < page_end &&
                   start + offset < AlignTypeToPageSize(mm[i].align_type);
                 offset += lookup_step) {
                ASSERT_NE(LookUpAddr((void*)(start + offset)), 0UL);
            }
        }
        for (size_t i = 0; i < mi.size(); ++i) {
            uint64_t offset = 0;
            uint64_t page_start = reinterpret_cast<uint64_t>(mi[i].virtual_addr);
            uint64_t page_end = page_start + page_size;
            for (; page_start + offset < page_end; offset += lookup_step) {
                ASSERT_NE(LookUpAddr((void*)(page_start + offset)), 0UL);
            }
        }
    };
    std::vector<std::thread> threads;
    for (size_t i = 0; i < thread_num; ++i) {
        threads.emplace_back(lookup_fn);
    }
    for (size_t i = 0; i < thread_num; ++i) {
        threads[i].join();
    }

    for (auto& m : mm) {
        mp->Deallocate(m.start_addr);
    }
    mp->Reset();
}

TEST(MemoryPoolAPITest, AllocMoreThanMaxThenFail) {
    MemoryPool* mp = MemoryPool::GetInstance();
    ASSERT_TRUE(mp->Init());
    size_t max_size = 512 * 1024;
    void* addr = mp->Allocate(max_size);
    ASSERT_TRUE(addr);
    ASSERT_FALSE(mp->Allocate(max_size + 1));
    mp->Deallocate(addr);
    mp->Reset();
}

TEST(MemoryPollAPITest, ReInitOk) {
    MemoryPool* mp = MemoryPool::GetInstance();
    ASSERT_TRUE(mp->Init());
    ASSERT_TRUE(mp->Init());
    mp->Reset();
}

TEST(MemoryPollAPITest, UsageAPIs) {
    MemoryPool* mp = MemoryPool::GetInstance();
    ASSERT_TRUE(mp->Init());
    MemoryPoolInitOption opt = {};
    EXPECT_EQ(mp->GetMemoryUsage(), 1024UL * 1024 * opt.prealloc_size_mb);
    void* addr = mp->Allocate(1);
    EXPECT_GT(mp->GetMemoryUsage(), 0);
    mp->Deallocate(addr);
    EXPECT_GT(mp->GetMemoryUsage(), 0);
    mp->Reset();
}

TEST(MemoryPoolTest, ExitNormalIfTLSCacheDeconBeforeBlockPools) {
    MemoryPool* mp = MemoryPool::GetInstance();
    ASSERT_TRUE(mp->Init());
    std::thread th([]() {
        MemoryPool::GetInstance()->Allocate(1024);
        MemoryPool::GetInstance()->Allocate(2048);
    });
    th.join();
    mp->Reset();
}

// 1. In sub thread allocate
// 2. Sub thread hanging, main thread Reset MemoryPool
// 3. Sub thread finish
TEST(MemoryPoolTest, ExitNormalIfTLSCacheDeconAfterBlockPools) {
    MemoryPool* mp = MemoryPool::GetInstance();
    ASSERT_TRUE(mp->Init());
    std::condition_variable cv1, cv2;
    std::mutex mtx;
    bool already_allocated = false;
    bool alreadt_reset = false;
    std::thread th([&]() {
        MemoryPool::GetInstance()->Allocate(1024);
        MemoryPool::GetInstance()->Allocate(2048);
        std::unique_lock<std::mutex> lk(mtx);
        already_allocated = true;
        cv1.notify_one();
        cv2.wait(lk, [&] { return alreadt_reset; });
    });
    std::unique_lock<std::mutex> lk(mtx);
    cv1.wait(lk, [&] { return already_allocated; });
    mp->Reset();
    alreadt_reset = true;
    lk.unlock();
    cv2.notify_one();
    th.join();
}

}  // namespace vesal
