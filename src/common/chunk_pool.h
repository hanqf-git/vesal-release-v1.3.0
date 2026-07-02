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

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/memory_pool_helper.h"
#include "common/mpsc_queue.h"
#include "vesal/memory_pool.h"
#include "vesal/metrics.h"

namespace vesal {

constexpr size_t kBlockSizes[] = {1UL * 1024,
                                  2UL * 1024,
                                  4UL * 1024,
                                  8UL * 1024,
                                  16UL * 1024,
                                  32UL * 1024,
                                  64UL * 1024,
                                  128UL * 1024,
                                  256UL * 1024,
                                  512UL * 1024};
const size_t kBlockNum = sizeof(kBlockSizes) / sizeof(kBlockSizes[0]);

struct BlockHeader;
struct ChunkHeader;
struct TLSBlockCache;
class TLSCache;

/*
Block: the minimal unit of memory pool allocation, consisting of a BlockHeader and a consecutive
free memory with size of [1KB, 2KB, 4KB, 8KB, 16KB, 32KB, 64KB, 128KB, 256KB, 512KB]. example
layout:
|-BlockHeader-|----4KB----|
|-BlockHeader-|--------8KB--------|

When user ask memory pool to allocate size_t memory, it will select a block that is big enough and
best fits size_t, for example, when allocate(31KB), the result will be a 32KB block
*/
struct BlockHeader {
    ChunkHeader* chunk = nullptr;
    BlockHeader* next = nullptr;
};

/*
Chunk: 2MB continuously memory, will be divided into one ChunkHeader and multiple Blocks of same
size example layout:
|-ChunkHeader-|-Block(512KB)-|-Block(512KB)-|-Block(512KB)-|-unusable space-|

When Memory pool initialize, it will split allocated hugepages into multiple chunks and later
distribute them to TLSCache when user calls Allocate(size)
*/
struct ChunkHeader {
    ChunkHeader* prev = nullptr;
    ChunkHeader* next = nullptr;
    BlockHeader* head = nullptr;
    TLSBlockCache* cache = nullptr;
    size_t available_block_num = 0;
    size_t total_block_num = 0;
    IF_ASAN(size_t block_size = 0);
};
const size_t kChunkSize = 2UL * 1024 * 1024;
const size_t kChunkBufSize = kChunkSize - sizeof(ChunkHeader);

class ChunkPool : public MemoryPool {
    friend class TLSCache;

public:
    ChunkPool() = default;
    ~ChunkPool() = default;

    bool Init() override;
    bool Init(const MemoryPoolInitOption& opt) override;
    bool Register(const std::vector<MemoryInfo>& infos) override;
    void* Allocate(size_t size) override;
    void Deallocate(void* ptr) override;
    void Reset() override;
    uint64_t GetMemoryUsage() const override;

    // Allocate hugepages based on opt_.prealloc_size_mb and split them into chunks at
    // initialization stage.
    bool Preallocate();
    // For TLSCache to get chunks
    ChunkHeader* GetChunks(size_t n);
    // For TLSCache to recycle chunks
    void PutChunks(ChunkHeader* head);
    // TODO(Pinnong.Li): Add metrics
    std::vector<MemoryInfo> GetInternalMemoryInfoVec() const override;

    void AddTlsCache(TLSCache* c);
    void RemoveTlsCache(TLSCache* c);

    void AddOrphanTlsCache(std::unique_ptr<TLSCache> cache) {
        std::lock_guard<std::mutex> guard(mutex_);
        orphan_tls_caches_.push_back(std::move(cache));
    }

    std::unique_ptr<TLSCache> AdoptOrNewTlsCache() {
        std::unique_ptr<TLSCache> ret = nullptr;
        std::lock_guard<std::mutex> guard(mutex_);
        if (orphan_tls_caches_.empty()) {
            ret = std::make_unique<TLSCache>(this);
            tls_caches_.insert(ret.get());
        } else {
            ret = std::move(orphan_tls_caches_.back());
            orphan_tls_caches_.pop_back();
        }
        return ret;
    }

private:
    bool ValidateInitOption() {
        if (!opt_.init_mem_pool) {
            return true;
        }
        if (opt_.prealloc_page_size == HugePageSize::kUnknown) {
            VESAL_LOG(ERROR) << "Wrong prealloc_page_size type";
            return false;
        }
        size_t sum = std::accumulate(opt_.prealloc_numa_mb, opt_.prealloc_numa_mb + kMaxNodeNum, 0);
        if (sum != 0 && sum != opt_.prealloc_size_mb) {
            VESAL_LOG(ERROR) << "sum of prealloc_numa_mb must equal to prealloc_size_mb, "
                             << "sum: " << sum << ", prealloc_size_mb: " << opt_.prealloc_size_mb;
            return false;
        }
        return true;
    }

    std::vector<void*> AllocHugepages(size_t page_num, HugePageSize hp_sz, int node = -1) {
        void* pages_start_addr = AllocHugepageFn(GetHugePageSize(hp_sz), page_num, node);
        if (VESAL_UNLIKELY(!pages_start_addr)) {
            VESAL_LOG(ERROR) << "Fail to allocate memory for ChunkPool, page_num=" << page_num
                             << ", page_size=" << GetHugePageSize(hp_sz);
            return {};
        }
        std::vector<void*> addrs(page_num, nullptr);
        for (size_t i = 0; i < page_num; ++i) {
            addrs[i] = static_cast<char*>(pages_start_addr) + i * GetHugePageSize(hp_sz);
        }
        return addrs;
    }

    MemoryPoolInitOption opt_;
    ChunkHeader* chunks_ = nullptr;
    size_t chunk_num_ = 0;
    size_t total_chunk_num_ = 0;
    std::mutex mutex_;
    std::vector<MemoryInfo> regions_;
    std::unordered_set<TLSCache*> tls_caches_;

    uint32_t stats_task_id_;
    std::once_flag metric_registration_flag_;
    // Unallocated memory size
    std::shared_ptr<Gauge> metrics_rest_memory_size_;
    // Counter of how many times GetChunks() called
    std::shared_ptr<Counter> metrics_getchunks_counter_;
    // Counter of how many times PutChunks() called
    std::shared_ptr<Counter> metrics_putchunks_counter_;
    // Total size of memory recycled via PutChunks()
    std::shared_ptr<Counter> metrics_putchunks_total_size_;
    // Counter of how many times Allocate() failed due to insufficient memory
    std::shared_ptr<Counter> metrics_fail_allocate_counter_;

    // TODO: check orphan's deferred task occasionally
    std::list<std::unique_ptr<TLSCache>> orphan_tls_caches_;

    void InitMetrics();
};

/*
TLSBlockCache manages chunks with two linked list: chunks with all blocks within available are in
the recyclable_chunks, other chunks with at least one available block are in the available_chunks.
Chunks will move between two linked list or even out of both as blocks being allocated and recycled.
*/
struct TLSBlockCache {
    ChunkHeader* recyclable_chunks = nullptr;
    ChunkHeader* available_chunks = nullptr;
    size_t block_size = 0;
    size_t total_size = 0;
    size_t recyclable_size = 0;
    size_t in_use_size = 0;
    TLSCache* tls_cache = nullptr;

    // When new chunks obtained from ChunkPool, set the chunk header and construct the internal
    // chain of blocks for every chunk
    void BuildBlocks(ChunkHeader* chunks, size_t block_size);
};

/*
TLSCache is a set of TLSBlockCache with different block size, it will select the best fit
TLSBlockCache when allocating memory and execute recycle strategy on all TLSBlockCache
*/
class TLSCache {
public:
    TLSCache(ChunkPool* cp);
    ~TLSCache();
    void* GetBlock(size_t size);
    void PutBlock(void* ptr);
    void Reset();
    void DeferredDeallocate();
    bool Recyclable() {
        return total_chunk_num_ == recyclable_chunk_num_;
    }
    void AttachThread();
    void DetachThread();

private:
    TLSBlockCache block_caches[kBlockNum];
    size_t total_chunk_num_ = 0;
    size_t recyclable_chunk_num_ = 0;
    ChunkPool* cp_;
    std::unique_ptr<MPSCQueue<void*>> thread_deallocate_tasks_;
    std::thread::id thread_id_;

    uint32_t stats_task_id_;
    // Total memory of each block size
    std::shared_ptr<Gauge> metrics_tls_total_size_[kBlockNum];
    // Memory allocated to user of each block size
    std::shared_ptr<Gauge> metrics_tls_in_use_size_[kBlockNum];
    // Recycle all recyclable_chunks if total_chunk_num * 2 >
    // FLAGS_vesal_memory_pool_cache_recycle_threshold_size_mb and recyclable_chunk_num_ *
    // kChunkSize >= 32MB
    void Recycle();
};

class TLSCacheGuard {
public:
    TLSCacheGuard(ChunkPool* cp, std::unique_ptr<TLSCache> cache)
        : cp_(cp), cache_(std::move(cache)) {
        cache_->AttachThread();
    }
    ~TLSCacheGuard() {
        cache_->DeferredDeallocate();
        cache_->DetachThread();
        // if it's recyclable, it will be recycled in ~TLSCache()
        if (!cache_->Recyclable()) {
            // move to central
            cp_->AddOrphanTlsCache(std::move(cache_));
        }
    }
    TLSCache* GetCache() {
        return cache_.get();
    }

private:
    ChunkPool* cp_;
    std::unique_ptr<TLSCache> cache_;
};

}  // namespace vesal