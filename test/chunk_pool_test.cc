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

#include "common/chunk_pool.h"

#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>

#include <condition_variable>

#include "ut_util.h"
#ifdef VESAL_ENABLE_ASAN
#include <sanitizer/asan_interface.h>
#endif

#include <numaif.h>

#include <chrono>
#include <ctime>
#include <memory>
#include <thread>
#include <vector>

#include "common/memory_pool_helper.h"
#include "vesal/memory_pool.h"
#include "vesal/vesal.h"

namespace vesal {

TEST(ChunkPoolTest, ChunkPoolBasicApiTest) {
    ChunkPool cp;
    MemoryPoolInitOption opt;
    opt.prealloc_size_mb = 2500;
    // Test twice to make sure Reset work
    for (size_t _ = 0; _ < 2; _++) {
        // Allocates 2GB space and then keep getting different number of chunks then put them back,
        // expecting all operations successful
        EXPECT_TRUE(cp.Init(opt));
        auto meminfos = cp.GetInternalMemoryInfoVec();
        uint64_t meminfo_total_size = 0;
        for (const auto& info : meminfos)
            meminfo_total_size += info.len;
        EXPECT_EQ(meminfo_total_size, 1024UL * 1024 * opt.prealloc_size_mb);
        size_t total_chunk_num = 1024UL * 1024 * opt.prealloc_size_mb / kChunkSize;
        EXPECT_EQ(cp.chunk_num_, total_chunk_num);
        // Expect Get and Put to work
        for (size_t n = 1; n < total_chunk_num; n *= 10) {
            ChunkHeader* h = cp.GetChunks(n);
            EXPECT_NE(h, nullptr);
            ChunkHeader* p = h;
            for (size_t i = 0; i < n; i++) {
                if (p->prev) {
                    EXPECT_EQ(p->prev->next, p);
                }
                p = p->next;
            }
            EXPECT_EQ(p, nullptr);
            cp.PutChunks(h);
        }
        // Try get more than total chunks, expect nullptr as return value
        EXPECT_EQ(cp.GetChunks(total_chunk_num + 1), nullptr);
        // Also more than total chunks, but with 2 steps
        ChunkHeader* h = cp.GetChunks(total_chunk_num - 1);
        EXPECT_NE(h, nullptr);
        EXPECT_EQ(cp.GetChunks(2), nullptr);
        ChunkHeader* h2 = cp.GetChunks(1);
        EXPECT_NE(h2, nullptr);
        EXPECT_EQ(cp.GetChunks(1), nullptr);
        cp.PutChunks(h);
        cp.PutChunks(h2);
        // Expect to be able to handle nullptr
        cp.PutChunks(nullptr);
        cp.Reset();
    }
}

TEST(ChunkPoolTest, TLSBlockCacheBuildBlocksTest) {
    ChunkPool cp;
    MemoryPoolInitOption opt;
    opt.prealloc_size_mb = 1024;
    EXPECT_TRUE(cp.Init(opt));
    TLSBlockCache bc;
    // Build 10 chunks with every block size, expect all attributes and structure correct
    for (size_t i = 0; i < kBlockNum; i++) {
        ChunkHeader* chunks = cp.GetChunks(10);
        size_t block_num =
            (kChunkSize - sizeof(ChunkHeader)) / (sizeof(BlockHeader) + kBlockSizes[i]);
        EXPECT_NE(chunks, nullptr);
        bc.BuildBlocks(chunks, kBlockSizes[i]);
        for (ChunkHeader* p = chunks; p; p = p->next) {
            // Expect correct attributes
            EXPECT_EQ(p->total_block_num, block_num);
            EXPECT_EQ(p->available_block_num, block_num);
            EXPECT_EQ(p->cache, &bc);
            // Expect correct structure
            BlockHeader* b = p->head;
            for (size_t j = 0; j < block_num; j++) {
                EXPECT_EQ(b->chunk, p);
                b = b->next;
            }
            EXPECT_EQ(b, nullptr);
        }
        cp.PutChunks(chunks);
    }
    cp.Reset();
}

TEST(ChunkPoolTest, TLSCacheBasicApiTest) {
    ChunkPool cp;
    MemoryPoolInitOption opt;
    opt.prealloc_size_mb = 1024;
    opt.fetch_chunk_num = 1;
    EXPECT_TRUE(cp.Init(opt));
    const int kCacheNum = 8;
    std::vector<TLSCache*> caches(kCacheNum);
    std::vector<std::vector<void*>> addrs(kCacheNum);
    for (int i = 0; i < kCacheNum; i++)
        caches[i] = new TLSCache(&cp);
    // Test twice to make sure PutBlock did recycled all blocks
    for (int _ = 0; _ < 2; _++) {
        // Keep allocating blocks until running out and then put them all back, expect all operation
        // successful
        size_t total_chunk_num = 1UL * 1024 * 1024 * 1024 / kChunkSize;
        for (size_t i = 0; i < total_chunk_num; i++) {
            // cache round robin
            int cache_index = i % kCacheNum;
            TLSCache* cache = caches[cache_index];
            size_t block_size = kBlockSizes[i % 5];
            size_t block_num =
                (kChunkSize - sizeof(ChunkHeader)) / (block_size + sizeof(BlockHeader));
            for (size_t j = 0; j < block_num; j++) {
                void* addr = cache->GetBlock(block_size - 100);
                EXPECT_NE(addr, nullptr);
                addrs[cache_index].push_back(addr);
            }
        }
        // No more blocks, expect nullptr as return value
        for (int i = 0; i < kCacheNum; i++)
            EXPECT_EQ(caches[i]->GetBlock(1), nullptr);
        // Put all blocks back
        for (int i = 0; i < kCacheNum; i++) {
            for (auto addr : addrs[i])
                caches[i]->PutBlock(addr);
            addrs[i].clear();
        }
    }
    for (int i = 0; i < kCacheNum; i++)
        delete caches[i];
    cp.Reset();
}

TEST(ChunkPoolTest, RecycleTest) {
    ChunkPool cp;
    const int kChunkPoolSizeMB = 1024;
    MemoryPoolInitOption opt;
    opt.prealloc_size_mb = kChunkPoolSizeMB;
    for (size_t i = 0; i < kBlockNum; i++) {
        EXPECT_TRUE(cp.Init(opt));
        TLSCache cache(&cp), cache2(&cp);
        size_t block_size = kBlockSizes[i];
        size_t block_num = (kChunkSize - sizeof(ChunkHeader)) / (block_size + sizeof(BlockHeader));
        size_t total_block_num = block_num * kChunkPoolSizeMB / 2;
        std::vector<void*> addrs;
        for (size_t j = 0; j < total_block_num; j++) {
            void* addr = cache.GetBlock(block_size - 100);
            EXPECT_NE(addr, nullptr);
            addrs.push_back(addr);
        }
        // Expect recycle not triggered because no recyclable chunks
        EXPECT_EQ(cache2.GetBlock(block_size - 100), nullptr);
        // Expect all blocks been put back recycled, so same number of blocks can be allocated from
        // cache2
        for (size_t j = 0; j < block_num * 16; j++)
            cache.PutBlock(addrs[j]);
        for (size_t j = 0; j < block_num * 16; j++)
            EXPECT_NE(cache2.GetBlock(block_size - 100), nullptr);
        cp.Reset();
    }
}

IF_ASAN(TEST(ChunkPoolTest, UseAfterFreeTest) {
    ChunkPool cp;
    const int kChunkPoolSizeMB = 1024;
    MemoryPoolInitOption opt;
    opt.prealloc_size_mb = kChunkPoolSizeMB;
    EXPECT_TRUE(cp.Init(opt));
    auto cache = std::make_unique<TLSCache>(&cp);
    void* addr = cache->GetBlock(4096);
    EXPECT_NE(addr, nullptr);
    cache->PutBlock(addr);
    EXPECT_DEATH(VESAL_LOG(INFO) << ((char*)addr)[1], "AddressSanitizer");
    cache.reset();
    cp.Reset();
});

IF_ASAN(TEST(ChunkPoolTest, DoubleFreeTest) {
    ChunkPool cp;
    const int kChunkPoolSizeMB = 1024;
    MemoryPoolInitOption opt;
    opt.prealloc_size_mb = kChunkPoolSizeMB;
    EXPECT_TRUE(cp.Init(opt));
    auto cache = std::make_unique<TLSCache>(&cp);
    void* addr = cache->GetBlock(4096);
    EXPECT_NE(addr, nullptr);
    cache->PutBlock(addr);
    EXPECT_DEATH(cache->PutBlock(addr), "");
    cache.reset();
    cp.Reset();
});

IF_ASAN(TEST(ChunkPoolTest, IllegalAccessTest) {
    ChunkPool cp;
    const int kChunkPoolSizeMB = 1024;
    MemoryPoolInitOption opt;
    opt.prealloc_size_mb = kChunkPoolSizeMB;
    EXPECT_TRUE(cp.Init(opt));
    auto cache = std::make_unique<TLSCache>(&cp);
    void* addr = cache->GetBlock(4096);
    EXPECT_NE(addr, nullptr);
    EXPECT_DEATH(VESAL_LOG(INFO) << ((char*)addr)[5000], "AddressSanitizer");
    cache->PutBlock(addr);
    cache.reset();
    cp.Reset();
});

TEST(ChunkPoolTest, CrossThreadDeallocationTest) {
    ChunkPool cp;
    const int kChunkPoolSizeMB = 1024;
    MemoryPoolInitOption opt;
    opt.prealloc_size_mb = kChunkPoolSizeMB;
    EXPECT_TRUE(cp.Init(opt));
    std::mutex mtx;
    std::condition_variable cv;
    void* addr = nullptr;
    bool task_ready = false;
    std::thread t1{[&]() {
        auto tid = std::this_thread::get_id();
        // trigger GetOrNewTlsCache()
        void* p = cp.Allocate(1);
        cp.Deallocate(p);
        TLSCache* current_cache = nullptr;
        MPSCQueue<void*>* queue = nullptr;
        for (TLSCache* cache : cp.tls_caches_) {
            if (cache->thread_id_ == tid) {
                current_cache = cache;
                queue = cache->thread_deallocate_tasks_.get();
            }
        }
        EXPECT_TRUE(queue->Empty());
        std::unique_lock<std::mutex> lock(mtx);
        addr = cp.Allocate(512 * 1024);
        EXPECT_NE(nullptr, addr);
        cv.notify_one();
        cv.wait(lock, [&] { return task_ready; });
        EXPECT_FALSE(queue->Empty());
        current_cache->DeferredDeallocate();
        EXPECT_TRUE(queue->Empty());
    }};
    std::thread t2{[&]() {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [&] { return addr != nullptr; });
        cp.Deallocate(addr);
        task_ready = true;
        cv.notify_one();
    }};
    t1.join();
    t2.join();
    cp.Reset();
}

TEST(ChunkPoolTest, TLSCacheReuseTest) {
    ChunkPool cp;
    const int kChunkPoolSizeMB = 1024;
    MemoryPoolInitOption opt;
    opt.prealloc_size_mb = kChunkPoolSizeMB;
    EXPECT_TRUE(cp.Init(opt));
    void* addr = nullptr;
    TLSCache* cache1 = nullptr;
    TLSCache* cache2 = nullptr;
    std::thread t1{[&]() {
        addr = cp.Allocate(512 * 1024);
        EXPECT_NE(nullptr, addr);
        auto tid = std::this_thread::get_id();
        for (TLSCache* cache : cp.tls_caches_) {
            if (cache->thread_id_ == tid) {
                cache1 = cache;
                break;
            }
        }
    }};
    t1.join();
    std::thread t2{[&]() {
        cp.Deallocate(addr);
        auto tid = std::this_thread::get_id();
        for (TLSCache* cache : cp.tls_caches_) {
            if (cache->thread_id_ == tid) {
                cache2 = cache;
                break;
            }
        }
        EXPECT_EQ(cache1, cache2);
    }};
    t2.join();
    cp.Reset();
}

TEST(ChunkPoolTest, MultiThreadTrafficTest) {
    ChunkPool cp;
    const int kChunkPoolSizeMB = 4096;
    const int kThreadNum = 16;
    for (int _ = 0; _ < 3; _++) {
        MemoryPoolInitOption opt;
        opt.prealloc_size_mb = kChunkPoolSizeMB;
        EXPECT_TRUE(cp.Init(opt));
        std::thread threads[kThreadNum];
        for (int i = 0; i < kThreadNum; i++) {
            threads[i] = std::thread{[&]() {
                std::vector<void*> addrs;
                for (int k = 0; k < 10; k++) {
                    for (int j = 0; j < 64; j++) {
                        void* addr = cp.Allocate(kBlockSizes[j % 10]);
                        EXPECT_NE(addr, nullptr);
                        addrs.push_back(addr);
                    }
                    for (auto addr : addrs)
                        cp.Deallocate(addr);
                    addrs.clear();
                }
            }};
        }
        for (int i = 0; i < kThreadNum; i++) {
            threads[i].join();
        }
        cp.Reset();
    }
}

TEST(ChunkPoolTest, NumaPrealloc) {
    FLAGS_vesal_log_console_output = true;
    ChunkPool cp;
    MemoryPoolInitOption opt;

    opt.init_mem_pool = false;
    EXPECT_TRUE(cp.Init(opt));
    cp.Reset();

    opt.init_mem_pool = true;
    opt.prealloc_size_mb = 1024;
    opt.prealloc_numa_mb[0] = 512;
    opt.prealloc_numa_mb[1] = 1024;
    // Not equal
    EXPECT_FALSE(cp.Init(opt));

    opt.prealloc_numa_mb[1] = 512;
    EXPECT_TRUE(cp.Init(opt));
    size_t numa_mm[2] = {};
    for (auto& r : cp.regions_) {
        int page_num_in_region = r.len / r.page_size;
        EXPECT_EQ(r.len % r.page_size, 0);
        for (int i = 0; i < page_num_in_region; ++i) {
            int numa =
                get_numa_node(reinterpret_cast<void*>((uintptr_t)r.virtual_addr + i * r.page_size));
            EXPECT_GE(numa, 0);
            numa_mm[numa]++;
        }
    }
    EXPECT_EQ(numa_mm[0], numa_mm[1]);
    EXPECT_EQ(numa_mm[0], 256);
    cp.Reset();
}

}  // namespace vesal
