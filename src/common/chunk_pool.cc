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

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "common/metrics_internal.h"
#include "common/scheduler.h"
#ifdef VESAL_ENABLE_ASAN
#include <sanitizer/asan_interface.h>
#endif

#include "common/memory_pool_helper.h"
#include "common/timestamp.h"
#include "gflags/gflags.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/metrics.h"
#include "vesal/vesal.h"

namespace vesal {

static const size_t kMemoryPoolMaxAllocate = kBlockSizes[kBlockNum - 1];

thread_local std::unique_ptr<TLSCacheGuard> g_tls_cache;

TLSCache* GetOrNewTlsCache(ChunkPool* cp);

// Get the start address of the buffer in the Chunk
inline void* BufAddr(ChunkHeader* header) {
    return VESAL_PTR_ADD(header, sizeof(ChunkHeader));
}

void ChunkPool::Reset() {
    if (!initialized_) {
        return;
    }
    // Clean TLSCaches, risk of race condition exists, but this is only for unit tests that reuses
    // Chunkpool and we will avoid that. For actual users, TLSCaches will destruct before
    // ChunkPool::Reset(), so the following for loop won't be executed
    for (auto& c : tls_caches_) {
        c->Reset();
    }
    std::lock_guard<std::mutex> guard(mutex_);
    g_periodic_scheduler.CompleteTask(stats_task_id_);
    IF_ASAN({
        for (ChunkHeader* p = chunks_; p; p = p->next)
            __asan_unpoison_memory_region(BufAddr(p), kChunkBufSize);
    });
    for (const auto& r : regions_) {
        DeallocHugepageFn(r.virtual_addr, r.len);
    }
    regions_.clear();
    chunks_ = nullptr;
    chunk_num_ = 0;
    total_chunk_num_ = 0;
    addr_manager_->Reset();
    addr_manager_.reset();
    initialized_ = false;
    VESAL_LOG(INFO) << "reset MemoryPool";
}

bool ChunkPool::Init(const MemoryPoolInitOption& opt) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (initialized_) {
        return true;
    }
    opt_ = opt;
    return ValidateInitOption() && Preallocate();
}

bool ChunkPool::Init() {
    return ChunkPool::Init({});
}

bool ChunkPool::Register(const std::vector<MemoryInfo>& infos) {
    if (VESAL_UNLIKELY(!initialized_)) {
        VESAL_LOG(ERROR) << "MemoryPool is not initialized";
        return false;
    }
    return addr_manager_->AddExternalEntries(infos);
}

void* ChunkPool::Allocate(size_t size) {
    VESAL_DCHECK(initialized_) << "MemoryPool is not initialized";
    if (VESAL_UNLIKELY(size > kMemoryPoolMaxAllocate)) {
        VESAL_LOG(ERROR) << "size=" << size << ", exceeds maximum size=" << kMemoryPoolMaxAllocate;
        return nullptr;
    }
    if (VESAL_UNLIKELY(!size)) {
        VESAL_LOG(ERROR) << "size should be greater than 0";
        return nullptr;
    }
    void* p = GetOrNewTlsCache(this)->GetBlock(size);
    if (VESAL_UNLIKELY(!p)) {
        VESAL_LOG(ERROR) << "No enough memory, fail to allocate for size: " << size;
        metrics_fail_allocate_counter_->Add(1);
    }
    return p;
}

void ChunkPool::Deallocate(void* ptr) {
    VESAL_DCHECK(initialized_) << "MemoryPool is not initialized";
    if (VESAL_UNLIKELY(!ptr)) {
        VESAL_LOG(WARN) << "Empty ptr when Deallocate";
        return;
    }
    GetOrNewTlsCache(this)->PutBlock(ptr);
}

uint64_t ChunkPool::GetMemoryUsage() const {
    if (VESAL_UNLIKELY(!initialized_)) {
        VESAL_LOG(ERROR) << "MemoryPool is not initialized";
        return 0;
    }
    return total_chunk_num_ * kChunkSize;
}

inline AlignType HugePageSizeToAlignType(HugePageSize hp) {
    switch (hp) {
    case HugePageSize::k2MB:
        return AlignType::kPage2Mb;
    case HugePageSize::k1GB:
        return AlignType::kPage1Gb;
    case vesal::HugePageSize::k512MB:
        return AlignType::kPage512Mb;
    default:
        return AlignType::kUnknown;
    }
}

bool ChunkPool::Preallocate() {
    if (initialized_)
        return true;
    // Some UT only use Preallocate instead of Init, so place metrics initialization here
    InitMetrics();
    addr_manager_ = std::make_unique<AddressManager>();
    addr_manager_->Init();
    size_t page_size = GetHugePageSize(opt_.prealloc_page_size);
    AlignType align_type = HugePageSizeToAlignType(opt_.prealloc_page_size);
    // Record numa specification. We validated outside that prealloc_numa_mb sum equals to
    // prealloc_size_mb.
    std::map<int, size_t> numa_alloc_page_num;
    size_t total_numa_page_num = 0;
    for (size_t i = 0; i < kMaxNodeNum; ++i) {
        if (opt_.prealloc_numa_mb[i] > 0) {
            numa_alloc_page_num[i] =
                (opt_.prealloc_numa_mb[i] * 1024 * 1024 + page_size - 1) / page_size;
            total_numa_page_num += numa_alloc_page_num[i];
        }
    }
    // Round to roof.
    size_t total_page_num = (opt_.prealloc_size_mb * 1024 * 1024 + page_size - 1) / page_size;
    // Need to choose max because prealloc_size_mb may be smaller than
    // prealloc_numa_mb sum. Say prealloc_numa_mb is [3, 3], prealloc_size_mb is 6, then
    // total_page_num == 3, while total_numa_page_num == 2 + 2. Note total_numa_page_num can be 0 if
    // user not specify opt_.prealloc_numa_mb.
    total_page_num = std::max(total_page_num, total_numa_page_num);
    size_t left_page_num = total_page_num;
    // Start allocate
    const std::unordered_map<HugePageSize, size_t> kChunkFetchOptions = {
        {HugePageSize::k1GB, 1}, {HugePageSize::k2MB, 512}, {HugePageSize::k512MB, 1}};
    VESAL_DCHECK(kChunkFetchOptions.count(opt_.prealloc_page_size) > 0);
    while (left_page_num > 0) {
        size_t page_num = std::min(left_page_num, kChunkFetchOptions.at(opt_.prealloc_page_size));
        int numa_id = numa_alloc_page_num.empty() ? -1 : numa_alloc_page_num.begin()->first;
        if (numa_id >= 0) {
            page_num = std::min(page_num, numa_alloc_page_num[numa_id]);
        }
        std::vector<void*> addrs = AllocHugepages(page_num, opt_.prealloc_page_size, numa_id);
        if (addrs.empty()) {
            VESAL_LOG(ERROR) << "Fail to allocate memory for ChunkPool, total_page_num="
                             << total_page_num << ", page_size=" << page_size
                             << ", left_page_num=" << left_page_num << ", numa_id=" << numa_id;
            return false;
        }
        left_page_num -= page_num;
        if (numa_id >= 0) {
            VESAL_DCHECK(numa_alloc_page_num[numa_id] >= page_num);
            numa_alloc_page_num[numa_id] -= page_num;
            if (numa_alloc_page_num[numa_id] == 0) {
                numa_alloc_page_num.erase(numa_id);
            }
        }

        std::vector<uint64_t> pfns = GetPageFrameNumberOrZero(addrs);
        std::vector<uint64_t> phys_addrs = CalcPhysAddr(addrs, pfns);
        for (uint64_t each_phys : phys_addrs) {
            if (each_phys == 0) {
                DeallocHugepageFn(addrs.front(), page_size * page_num);
                for (const auto& r : regions_) {
                    DeallocHugepageFn(r.virtual_addr, r.len);
                }
                VESAL_LOG(ERROR) << "Fail to get physical address for ChunkPool, addr: "
                                 << addrs.front() << ", size: " << page_size;
                return false;
            }
        }
        for (size_t i = 0; i < page_num; i++) {
            PhysChunk pc;
            pc.phys_addr = phys_addrs[i];
            pc.start_addr = addrs[i];
            pc.align_type = align_type;
            addr_manager_->AddInternalEntry(&pc);
        }
        MemoryInfo r;
        r.virtual_addr = addrs[0];
        r.len = page_size * page_num;
        r.page_size = page_size;
        // Actually this information is not needed here, set it as nullptr
        r.physical_addr = nullptr;
        regions_.push_back(std::move(r));
    }
    VESAL_CHECK(numa_alloc_page_num.empty())
        << "numa_alloc_page_num should be empty, but got " << numa_alloc_page_num.size();
    VESAL_CHECK(left_page_num == 0) << "left_page_num should be 0, but got " << left_page_num;

    // Split regions into chunks
    for (const auto& r : regions_) {
        size_t chunk_num = r.len / kChunkSize;
        chunk_num_ += chunk_num;
        for (size_t i = 0; i < chunk_num; i++) {
            ChunkHeader* c =
                reinterpret_cast<ChunkHeader*>(VESAL_PTR_ADD(r.virtual_addr, i * kChunkSize));
            c->next = chunks_;
            if (chunks_)
                chunks_->prev = c;
            chunks_ = c;
            IF_ASAN(__asan_poison_memory_region(BufAddr(c), kChunkBufSize));
        }
    }
    total_chunk_num_ = chunk_num_;
    initialized_ = true;
    VESAL_LOG(INFO) << "Succeed to init MemoryPool";
    return true;
}

ChunkHeader* ChunkPool::GetChunks(size_t n) {
    std::lock_guard<std::mutex> guard(mutex_);
    // Not enough chunks
    if (chunk_num_ < n)
        return nullptr;
    // Move the first n chunks out of chain
    chunk_num_ -= n;
    ChunkHeader* p = chunks_;
    for (--n; n && p; n--)
        p = p->next;
    ChunkHeader* r = chunks_;
    chunks_ = p->next;
    if (chunks_)
        chunks_->prev = nullptr;
    p->next = nullptr;
    IF_ASAN({
        for (ChunkHeader* p = r; p; p = p->next)
            __asan_unpoison_memory_region(BufAddr(p), kChunkBufSize);
    });
    metrics_getchunks_counter_->Add(1);
    return r;
}

void ChunkPool::PutChunks(ChunkHeader* head) {
    if (VESAL_UNLIKELY(!head))
        return;
    IF_ASAN({
        for (ChunkHeader* p = head; p; p = p->next) {
            for (BlockHeader* b = p->head; b; b = b->next)
                __asan_unpoison_memory_region(VESAL_PTR_ADD(b, sizeof(BlockHeader)), p->block_size);
            __asan_poison_memory_region(BufAddr(p), kChunkBufSize);
        }
    });
    std::lock_guard<std::mutex> guard(mutex_);
    if (VESAL_UNLIKELY(!initialized_))
        return;
    ChunkHeader* p = head;
    size_t n = 1;
    for (; p->next; n++)
        p = p->next;
    p->next = chunks_;
    if (chunks_)
        chunks_->prev = p;
    chunks_ = head;
    chunk_num_ += n;
    metrics_putchunks_counter_->Add(1);
    metrics_putchunks_total_size_->Add(n * kChunkSize);
}

std::vector<MemoryInfo> ChunkPool::GetInternalMemoryInfoVec() const {
    return regions_;
}

void ChunkPool::AddTlsCache(TLSCache* c) {
    std::lock_guard<std::mutex> guard(mutex_);
    tls_caches_.insert(c);
}

void ChunkPool::RemoveTlsCache(TLSCache* c) {
    std::lock_guard<std::mutex> guard(mutex_);
    tls_caches_.erase(c);
}

void ChunkPool::InitMetrics() {
    std::call_once(metric_registration_flag_, [this]() {
        metrics_rest_memory_size_ =
            g_metric_registry->RegisterGauge("vesal.chunk_pool_rest_memory_size", {});
        metrics_getchunks_counter_ =
            g_metric_registry->RegisterCounter("vesal.chunk_pool_fetch_counter", {});
        metrics_putchunks_counter_ =
            g_metric_registry->RegisterCounter("vesal.chunk_pool_recycle_counter", {});
        metrics_putchunks_total_size_ =
            g_metric_registry->RegisterCounter("vesal.chunk_pool_recycle_memory_size", {});
        metrics_fail_allocate_counter_ =
            g_metric_registry->RegisterCounter("vesal.chunk_pool_fail_allocate_counter", {});
    });
    stats_task_id_ = g_periodic_scheduler.AddPeriodicTask(
        [this]() { metrics_rest_memory_size_->Set(chunk_num_ * kChunkSize); },
        std::chrono::seconds(1));
}

inline TLSCache* GetOrNewTlsCache(ChunkPool* cp) {
    if (VESAL_UNLIKELY(!g_tls_cache)) {
        g_tls_cache = std::make_unique<TLSCacheGuard>(cp, cp->AdoptOrNewTlsCache());
    }
    return g_tls_cache->GetCache();
}

void TLSCache::AttachThread() {
    thread_id_ = std::this_thread::get_id();
    std::ostringstream ss;
    ss << thread_id_;
    Tag tag_thread_id = std::make_pair("thread_id", ss.str());
    for (size_t i = 0; i < kBlockNum; i++) {
        Tag tag_block_size =
            std::make_pair("block_size", std::to_string(kBlockSizes[i] >> 10) + "KB");
        metrics_tls_total_size_[i] = g_metric_registry->RegisterGauge(
            "vesal.chunk_pool_tls_total_size", {tag_thread_id, tag_block_size});
        metrics_tls_in_use_size_[i] = g_metric_registry->RegisterGauge(
            "vesal.chunk_pool_tls_in_use_size", {tag_thread_id, tag_block_size});
    }
    stats_task_id_ = g_periodic_scheduler.AddPeriodicTask(
        [this]() {
            for (size_t i = 0; i < kBlockNum; i++) {
                metrics_tls_total_size_[i]->Set(block_caches[i].total_size);
                metrics_tls_in_use_size_[i]->Set(block_caches[i].in_use_size);
            }
        },
        std::chrono::seconds(1));
}

void TLSCache::DetachThread() {
    g_periodic_scheduler.CompleteTask(stats_task_id_);
    // set to empty id
    thread_id_ = std::thread::id();
}

TLSCache::TLSCache(ChunkPool* cp) : cp_(cp) {
    for (size_t i = 0; i < kBlockNum; i++) {
        block_caches[i].block_size = kBlockSizes[i];
        block_caches[i].tls_cache = this;
    }
    thread_deallocate_tasks_ = std::make_unique<MPSCQueue<void*>>();
    VESAL_CHECK(thread_deallocate_tasks_) << "Init thread_deallocate_tasks_ fail";
}

void TLSCache::Reset() {
    DeferredDeallocate();
    if (total_chunk_num_ > recyclable_chunk_num_) {
        VESAL_LOG(ERROR) << "TLSCache reset before deallocating related memory";
    }
    for (size_t i = 0; i < kBlockNum; i++) {
        cp_->PutChunks(block_caches[i].recyclable_chunks);
        block_caches[i].recyclable_chunks = nullptr;
        block_caches[i].available_chunks = nullptr;
        block_caches[i].total_size = 0;
        block_caches[i].recyclable_size = 0;
        block_caches[i].in_use_size = 0;
    }
    recyclable_chunk_num_ = 0;
    total_chunk_num_ = 0;
}

TLSCache::~TLSCache() {
    // TODO(Pinnong.li): Prevent memory leak
    Reset();
    if (cp_) {
        cp_->RemoveTlsCache(this);
    }
    cp_ = nullptr;
}

void TLSBlockCache::BuildBlocks(ChunkHeader* chunks, size_t block_size) {
    size_t ele_size = block_size + sizeof(BlockHeader);
    for (; chunks; chunks = chunks->next) {
        chunks->cache = this;
        chunks->total_block_num = (kChunkSize - sizeof(ChunkHeader)) / ele_size;
        chunks->available_block_num = chunks->total_block_num;
        BlockHeader* p = (BlockHeader*)BufAddr(chunks);
        chunks->head = p;
        for (size_t i = 0; i < chunks->available_block_num - 1; i++) {
            p->chunk = chunks;
            p->next = (BlockHeader*)VESAL_PTR_ADD(p, ele_size);
            p = p->next;
        }
        p->chunk = chunks;
        p->next = nullptr;
        IF_ASAN({
            chunks->block_size = block_size;
            p = chunks->head;
            while (p) {
                __asan_poison_memory_region(VESAL_PTR_ADD(p, sizeof(BlockHeader)), block_size);
                p = p->next;
            }
        });
    }
}

inline int GetBlockIndex(size_t size) {
    // ceil(x / 1024)
    size_t x = (size + 1023) >> 10;
    if (x & (x - 1)) {
        x = 0x80000000 >> (__builtin_clz(x) - 1);
    }
    // x is now the next power of 2
    return __builtin_popcount(x - 1);
}

void TLSCache::DeferredDeallocate() {
    void* task = nullptr;
    while (!thread_deallocate_tasks_->Empty()) {
        thread_deallocate_tasks_->Pop(&task);
        PutBlock(task);
    }
}

void* TLSCache::GetBlock(size_t size) {
    // Allocate() already did validation check, the size must be > 0 and
    // <= kBlockSize[kBlockNum - 1]
    int index = GetBlockIndex(size);
    TLSBlockCache* block_cache = &block_caches[index];

    if (!block_cache->available_chunks && !block_cache->recyclable_chunks) {
        DeferredDeallocate();
    }

    // If available_chunks is empty, get one from recyclable_chunks
    if (!block_cache->available_chunks) {
        // Recyclable_chunks empty too, get one from central chunk pool
        if (!block_cache->recyclable_chunks) {
            ChunkHeader* chunks = cp_->GetChunks(cp_->opt_.fetch_chunk_num);
            // Empty central chunk pool, GG!
            if (!chunks) {
                return nullptr;
            }
            total_chunk_num_ += cp_->opt_.fetch_chunk_num;
            recyclable_chunk_num_ += cp_->opt_.fetch_chunk_num;
            block_cache->total_size += cp_->opt_.fetch_chunk_num * kChunkSize;
            block_cache->recyclable_size += cp_->opt_.fetch_chunk_num * kChunkSize;
            block_cache->BuildBlocks(chunks, kBlockSizes[index]);
            block_cache->recyclable_chunks = chunks;
        }

        ChunkHeader* chunk = block_cache->recyclable_chunks;
        block_cache->available_chunks = chunk;
        block_cache->recyclable_chunks = chunk->next;
        if (block_cache->recyclable_chunks) {
            block_cache->recyclable_chunks->prev = nullptr;
        }
        chunk->next = nullptr;
        block_cache->recyclable_size -= kChunkSize;
        recyclable_chunk_num_--;
    }
    // Get a block from available_chunks
    ChunkHeader* chunk = block_cache->available_chunks;
    BlockHeader* block = chunk->head;
    chunk->head = block->next;
    chunk->available_block_num--;
    // Remove the chunk from the list if it becomes empty
    if (!chunk->available_block_num) {
        block_cache->available_chunks = chunk->next;
        if (block_cache->available_chunks)
            block_cache->available_chunks->prev = nullptr;
    }
    block_cache->in_use_size += kBlockSizes[index];
    IF_ASAN({
        __asan_unpoison_memory_region(VESAL_PTR_ADD(block, sizeof(BlockHeader)), chunk->block_size);
        __asan_poison_memory_region(block, sizeof(BlockHeader));
    });

    return VESAL_PTR_ADD(block, sizeof(BlockHeader));
}

void TLSCache::PutBlock(void* ptr) {
    IF_ASAN(VESAL_CHECK(!__asan_address_is_poisoned(ptr)) << "double free detected");
    BlockHeader* block = (BlockHeader*)VESAL_PTR_SUB(ptr, sizeof(BlockHeader));
    IF_ASAN(__asan_unpoison_memory_region(block, sizeof(BlockHeader)));
    ChunkHeader* chunk = block->chunk;
    TLSCache* owner = chunk->cache->tls_cache;
    if (owner->thread_id_ != thread_id_) {
        VESAL_LOG(DEBUG) << ptr << " is allocated from thread: " << owner->thread_id_
                         << ", current thread is " << thread_id_
                         << ", cross thread deallocation is deferred.";
        IF_ASAN(__asan_poison_memory_region(block, sizeof(BlockHeader)));
        owner->thread_deallocate_tasks_->Push(ptr);
        return;
    }
    TLSBlockCache* cache = chunk->cache;
    block->next = chunk->head;
    chunk->head = block;
    chunk->available_block_num++;
    cache->in_use_size -= cache->block_size;
    IF_ASAN(__asan_poison_memory_region(ptr, chunk->block_size));

    if (chunk->available_block_num == 1) {
        // Add to available_chunks
        chunk->prev = nullptr;
        chunk->next = cache->available_chunks;
        if (cache->available_chunks)
            cache->available_chunks->prev = chunk;
        cache->available_chunks = chunk;
    }

    if (chunk->available_block_num == chunk->total_block_num) {
        // Move from available_chunks to recyclable_chunks
        // 1. Remove from available_chunks
        ChunkHeader* prev = chunk->prev;
        ChunkHeader* next = chunk->next;
        if (!prev)
            cache->available_chunks = next;
        else
            prev->next = next;
        if (next)
            next->prev = prev;
        // 2. Add to recyclable_chunks
        chunk->prev = nullptr;
        chunk->next = cache->recyclable_chunks;
        if (cache->recyclable_chunks)
            cache->recyclable_chunks->prev = chunk;
        cache->recyclable_chunks = chunk;
        recyclable_chunk_num_++;
        cache->recyclable_size += kChunkSize;
    }
    Recycle();
}

void TLSCache::Recycle() {
    if (total_chunk_num_ * kChunkSize > (cp_->opt_.cache_recycle_threshold_size_mb << 20) &&
        recyclable_chunk_num_ >= cp_->opt_.recycle_chunk_num) {
        for (size_t i = 0; i < kBlockNum; i++) {
            cp_->PutChunks(block_caches[i].recyclable_chunks);
            block_caches[i].recyclable_chunks = nullptr;
            block_caches[i].total_size -= block_caches[i].recyclable_size;
            block_caches[i].recyclable_size = 0;
        }
        total_chunk_num_ -= recyclable_chunk_num_;
        recyclable_chunk_num_ = 0;
    }
}

}  // namespace vesal