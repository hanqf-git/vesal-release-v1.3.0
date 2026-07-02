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

#include "common/memory_pool_helper.h"

#include <linux/mman.h>  // MAP_HUGE_2MB
#include <numa.h>        // numa allocate
#include <numaif.h>
#include <sys/mman.h>
#include <unistd.h>  // getpagesize()

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>  // fread() etc.
#include <fstream>
#include <functional>
#include <iomanip>
#include <vector>

#include "common/metrics_internal.h"
#include "common/scheduler.h"
#include "memory_pool_helper.h"
#include "vesal/log_setting.h"

namespace vesal {

namespace {

// The buffer is dma(in mem pool) but not covered by one huge page, will fallback copy.
std::shared_ptr<Counter> g_metric_dma_buffer_but_not_covered_by_one_hugepage;

// These are flags used in pageflag, indicating the attributes of a page. Details:
// https://www.kernel.org/doc/html/latest/admin-guide/mm/pagemap.html?highlight=kpageflags

// This is a huge page
#define VESAL_PAGEFLAG_HUGE 17
// this page is in kernel unevictable LRU, will not be swapped out (pinned).
#define VESAL_PAGEFLAG_UNEVICTABLE 18

#define VESAL_IS_PAGE_ALIGNED(addr, page_size) \
    ((uintptr_t)(addr) == (uintptr_t)(VESAL_PAGE_ALIGN((addr), (page_size))))

const char* kPagemapPath = "/proc/self/pagemap";
const char* kPageFlags = "/proc/kpageflags";

const uint64_t kPFNMask = 0x7FFFFFFFFFFFFF;  // 55bits
const size_t kPagemapEntrySize = 8;

const static size_t kPageTypeNum = 4;

}  // namespace

template <typename T>
std::vector<uint64_t> GetPageFrameNumberOrZero(const std::vector<T>& addr_items,
                                               const std::function<void*(T)>& get_addr_func) {
    size_t len = addr_items.size();
    std::vector<uint64_t> ret(len, 0);

    FILE* pagemap_file = fopen(kPagemapPath, "rb");
    VESAL_CHECK(pagemap_file) << "Failed to open pagemap, file path=" << kPagemapPath;
    // By the kernel doc of pagemap, the size of pagemap entry is 8(kPagemapEntrySize)
    for (size_t i = 0; i < len; i++) {
        uint64_t pagemap_offset = reinterpret_cast<uint64_t>(get_addr_func(addr_items.at(i))) /
                                  getpagesize() * kPagemapEntrySize;
        VESAL_LOG(DEBUG) << "pagesize=" << getpagesize() << ", pagemap_offset: " << pagemap_offset;
        if (fseek(pagemap_file, reinterpret_cast<uint64_t>(pagemap_offset), SEEK_SET) != 0) {
            VESAL_LOG(ERROR) << "Failed to seek pagemap to proper location, errno: " << errno;
            continue;
        }
        uint64_t pfn = 0;
        int read_n = fread(&pfn, 1, kPagemapEntrySize, pagemap_file);
        if (VESAL_UNLIKELY(read_n != kPagemapEntrySize)) {
            // Let it finish the loop and handle outside
            VESAL_LOG(WARN) << "Failed to read pagemap, read_n=" << read_n << ", errno: " << errno;
        }
        pfn &= kPFNMask;
        ret.at(i) = pfn;
        VESAL_LOG(DEBUG) << "pfn: " << pfn;
    }

    fclose(pagemap_file);
    return ret;
}

std::vector<uint64_t> GetPageFrameNumberOrZero(const std::vector<PhysChunk>& chunks) {
    std::function<void*(PhysChunk)> get_addr_func = [](const PhysChunk& chunk) {
        return chunk.start_addr;
    };
    return GetPageFrameNumberOrZero(chunks, get_addr_func);
}

std::vector<uint64_t> GetPageFrameNumberOrZero(const std::vector<void*>& addrs) {
    std::function<void*(void*)> get_addr_func = [](void* addr) { return addr; };
    return GetPageFrameNumberOrZero(addrs, get_addr_func);
}

// What is a pageflag? See
// https://www.kernel.org/doc/html/latest/admin-guide/mm/pagemap.html?highlight=kpageflags
std::vector<uint64_t> GetPageFlagOrZero(const std::vector<uint64_t>& pfns) {
    std::vector<uint64_t> ret(pfns.size(), 0);

    FILE* pageflags_file = fopen(kPageFlags, "rb");
    for (size_t i = 0; i < pfns.size(); i++) {
        size_t offset = pfns.at(i) * kPagemapEntrySize;
        if (fseek(pageflags_file, offset, SEEK_SET) != 0) {
            VESAL_LOG(ERROR) << "Failed to seek pageflags to proper location, offset=" << offset;
            continue;
        }
        uint64_t flag = 0;
        int read_n = fread(&flag, 1, kPagemapEntrySize, pageflags_file);
        if (VESAL_UNLIKELY(read_n != kPagemapEntrySize)) {
            // Let it finish the loop and handle outside
            VESAL_LOG(WARN) << "Failed to read pagemap, read_n=" << read_n << ", errno: " << errno;
        }
        ret.at(i) = flag;
        VESAL_LOG(DEBUG) << "pfn=" << pfns.at(i) << ", flag: " << flag;
    }
    fclose(pageflags_file);
    return ret;
}

uint64_t CalcPhysAddr(void* addr, uint64_t pfn) {
    if (VESAL_UNLIKELY(pfn == 0)) {
        VESAL_LOG(WARN) << "Invalid PageFrameNumber for " << addr;
        return 0;
    }
    uint64_t distance_from_page_boundary = (uint64_t)addr % getpagesize();
    uint64_t pa = pfn * getpagesize() + distance_from_page_boundary;
    uint64_t ret = pa;
    VESAL_LOG(DEBUG) << "addr=" << (uintptr_t)addr << ", pfn=" << pfn
                     << ", distance_from_page_boundary=" << distance_from_page_boundary
                     << ", pa=" << pa;
    return ret;
}

std::vector<uint64_t> CalcPhysAddr(const std::vector<void*>& addrs,
                                   const std::vector<uint64_t>& pfn) {
    VESAL_CHECK(pfn.size() == addrs.size());
    std::vector<uint64_t> ret(addrs.size(), 0);
    for (size_t i = 0; i < addrs.size(); i++) {
        ret.at(i) = CalcPhysAddr(addrs[i], pfn[i]);
    }
    return ret;
}

// In some systems, MAP_HUGE_512MB may not be defined
#ifndef MAP_HUGE_512MB
#define MAP_HUGE_512MB (29 << (MAP_HUGE_SHIFT))
#endif

uint64_t GetPageFlagBySize(size_t page_size) {
    VESAL_CHECK(page_size == VESAL_PAGESIZE(PAGE_BIT_NUM_2MB) ||
                page_size == VESAL_PAGESIZE(PAGE_BIT_NUM_1GB) ||
                page_size == VESAL_PAGESIZE(PAGE_BIT_NUM_512MB))
        << "Not support huge page size: " << page_size;
    if (page_size == VESAL_PAGESIZE(PAGE_BIT_NUM_2MB)) {
        return MAP_HUGE_2MB;
    } else if (page_size == VESAL_PAGESIZE(PAGE_BIT_NUM_1GB)) {
        return MAP_HUGE_1GB;
    } else if (page_size == VESAL_PAGESIZE(PAGE_BIT_NUM_512MB)) {
        return MAP_HUGE_512MB;
    }
    return 0;
}

void* AllocHugepageFn(size_t page_size, size_t page_num, int node) {
    uint64_t page_flag = GetPageFlagBySize(page_size);
    if (node >= 0) {
        uint64_t nmask = 1UL << node;
        int r = set_mempolicy(MPOL_BIND, &nmask, sizeof(nmask) * 8);
        if (r != 0) {
            VESAL_LOG(WARN) << "set_mempolicy failed for node: " << node
                            << ", page_size: " << page_size << ", page_num: " << page_num
                            << ", errno=" << errno;
            return nullptr;
        }
        VESAL_LOG(INFO) << "set_mempolicy for node: " << node << ", page_size: " << page_size
                        << ", page_num: " << page_num;
    }
    void* addr = (void*)mmap(nullptr,
                             page_size * page_num,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB | page_flag,
                             -1,
                             0);
    if (node >= 0) {
        // Set back to default
        set_mempolicy(MPOL_DEFAULT, nullptr, 0);
        VESAL_LOG(INFO) << "Set back mempolicy to default for node: " << node;
    }
    if (addr == MAP_FAILED) {
        VESAL_LOG(WARN) << "hugepage mmap failed, page_size=" << page_size
                        << ", page_num=" << page_num << ", errno=" << errno;
        return nullptr;
    }
    VESAL_LOG(DEBUG) << "hugepage allocated, addr=" << (uintptr_t)addr
                     << ", page_size=" << page_size << ", page_num=" << page_num;
    return addr;
}

void DeallocHugepageFn(void* vaddr, size_t size) {
    VESAL_CHECK((size % VESAL_PAGESIZE(PAGE_BIT_NUM_2MB)) == 0);
    VESAL_LOG(DEBUG) << "hugepage freed, vaddr=" << (uintptr_t)vaddr << ", size=" << size;
    munmap(vaddr, size);
}

inline bool ContainFlag(uint64_t x, uint8_t flag_bit) {
    return (x >> flag_bit) & 0x1;
}

bool IsMemoryPinned(uint64_t page_flag) {
    return ContainFlag(page_flag, VESAL_PAGEFLAG_UNEVICTABLE) ||
           ContainFlag(page_flag, VESAL_PAGEFLAG_HUGE);
}

bool IsMemoryPinned(const std::vector<uint64_t>& page_flags) {
    for (size_t i = 0; i < page_flags.size(); i++) {
        if (!IsMemoryPinned(page_flags.at(i)))
            return false;
    }
    return true;
}

// TODO(sjj): consider a more accurate way. Now we cannot distinguish a 2MB-hugepage with
// 1GB-alignment.
AlignType PageAlignType(void* addr) {
    if (VESAL_IS_PAGE_ALIGNED(addr, VESAL_PAGESIZE(PAGE_BIT_NUM_1GB))) {
        return AlignType::kPage1Gb;
    } else if (VESAL_IS_PAGE_ALIGNED(addr, VESAL_PAGESIZE(PAGE_BIT_NUM_512MB))) {
        return AlignType::kPage512Mb;
    } else if (VESAL_IS_PAGE_ALIGNED(addr, VESAL_PAGESIZE(PAGE_BIT_NUM_2MB))) {
        return AlignType::kPage2Mb;
    } else if (VESAL_IS_PAGE_ALIGNED(addr, VESAL_PAGESIZE(PAGE_BIT_NUM_4KB))) {
        return AlignType::kPage4kb;
    }
    return AlignType::kUnknown;
}

thread_local ska::flat_hash_map<void*, PhysChunk, HugePageHash>
    AddressManager::t_tls_memory_info_by_vaddr_;

void AddressManager::Init() {
    metric_memory_pool_chunk_entry_num_ =
        g_metric_registry->RegisterGauge("vesal.memory_pool_chunk_entry_num", {});
    g_metric_dma_buffer_but_not_covered_by_one_hugepage =
        g_metric_registry->RegisterCounter("vesal.dma_buffer_but_not_covered_by_one_hugepage", {});
    metrics_task_id_ = g_periodic_scheduler.AddPeriodicTask(
        // TODO(Pinnong Li): specify internal and external chunk num
        [this]() { metric_memory_pool_chunk_entry_num_->Set(memory_info_by_vaddr_.size()); },
        std::chrono::milliseconds(1000));
}

void AddressManager::Reset() {
    g_periodic_scheduler.CompleteTask(metrics_task_id_);
    // Todo(Pinnong.li): clear tls every thread or not use tls at all
    t_tls_memory_info_by_vaddr_.clear();
}

void AddressManager::AddInternalEntry(PhysChunk* chunk) {
    AlignType align_type = chunk->align_type;
    VESAL_CHECK(align_type == AlignType::kPage1Gb || align_type == AlignType::kPage2Mb ||
                align_type == AlignType::kPage512Mb)
        << "align type=" << static_cast<int>(align_type) << ", chunk->start_addr=" << std::hex
        << chunk->start_addr;
    std::unique_lock<std::shared_timed_mutex> write_lck(g_rw_mutex_);
    // copy semantic
    memory_info_by_vaddr_[chunk->start_addr] = *chunk;
}

bool AddressManager::AddExternalEntry(const MemoryInfo& memory_info) {
    return AddExternalEntries({memory_info});
}

bool AddressManager::AddExternalEntries(const std::vector<MemoryInfo>& memory_infos) {
    std::vector<PhysChunk> chunks;

    uint64_t chunk_num = 0;
    for (const auto& memory_info : memory_infos) {
        if (VESAL_UNLIKELY(!ValidateExternalMemInfo(memory_info)))
            return false;
        chunk_num += memory_info.len / memory_info.page_size;
    }
    chunks.reserve(chunk_num);

    for (const auto& memory_info : memory_infos) {
        uint64_t page_num = memory_info.len / memory_info.page_size;
        for (uint64_t i = 0; i < page_num; ++i) {
            PhysChunk chunk;
            chunk.start_addr =
                static_cast<char*>(memory_info.virtual_addr) + i * memory_info.page_size;
            // We ignore user-provided physical address currently.
            chunk.phys_addr = 0;
            chunk.align_type = PageSizeToAlignType(memory_info.page_size);
            chunk.next = nullptr;
            chunks.push_back(std::move(chunk));
        }
    }

    std::vector<uint64_t> pfns = GetPageFrameNumberOrZero(chunks);
    if (!IsMemoryPinned(GetPageFlagOrZero(pfns)))
        return false;
    if (!FillPhysAddrIfMissing(pfns, &chunks))
        return false;

    std::unique_lock<std::shared_timed_mutex> write_lck(g_rw_mutex_);

    for (size_t i = 0; i < chunk_num; ++i)
        memory_info_by_vaddr_[chunks.at(i).start_addr] = std::move(chunks.at(i));

    return true;
}

inline const PhysChunk* TableLookUp(const ska::flat_hash_map<void*, PhysChunk, HugePageHash>& map,
                                    void* vaddr,
                                    const AlignType order[kPageTypeNum]) {
    for (size_t i = 0; i < kPageTypeNum; ++i) {
        auto itr = map.find(AlignPage(vaddr, order[i]));
        if (itr != map.end() && itr->second.align_type == order[i]) {
            return &(itr->second);
        }
    }
    return nullptr;
}

uint64_t AddressManager::GetPhysAddr(void* vaddr, const AlignType* order) {
    static AlignType default_order[kPageTypeNum] = {
        AlignType::kPage2Mb, AlignType::kPage512Mb, AlignType::kPage1Gb, AlignType::kPage4kb};
    if (!order) {
        order = default_order;
    }
    const PhysChunk* result = TableLookUp(t_tls_memory_info_by_vaddr_, vaddr, order);
    PhysChunk from_global;
    if (VESAL_UNLIKELY(!result)) {
        {
            std::shared_lock<std::shared_timed_mutex> read_lck(g_rw_mutex_);
            result = TableLookUp(memory_info_by_vaddr_, vaddr, order);
            if (VESAL_UNLIKELY(!result)) {
                VESAL_LOG(DEBUG) << "Not found entry, vaddr=" << (uintptr_t)vaddr;
                return 0;
            }
            // de-reference here in case the map rebalance messing up original memory where the
            // 'result' points at.
            from_global = *result;
        }
        result = &from_global;
        t_tls_memory_info_by_vaddr_[from_global.start_addr] = from_global;
    }
    uint64_t offset =
        reinterpret_cast<uint64_t>(vaddr) - reinterpret_cast<uint64_t>(result->start_addr);
    return result->phys_addr + offset;
}

size_t AddressManager::GetPhysAddrBound(void* vaddr,
                                        size_t v_len,
                                        uint64_t paddrs[],
                                        size_t paddr_sizes[],
                                        size_t max_num,
                                        const AlignType* order) {
    static AlignType default_order[kPageTypeNum] = {
        AlignType::kPage2Mb, AlignType::kPage512Mb, AlignType::kPage1Gb, AlignType::kPage4kb};
    if (!order) {
        order = default_order;
    }
    size_t ret = 0;
    uint64_t curr_v_addr = reinterpret_cast<uint64_t>(vaddr);
    size_t left = v_len;
    while (ret < max_num && left > 0) {
        const PhysChunk* result =
            TableLookUp(t_tls_memory_info_by_vaddr_, reinterpret_cast<void*>(curr_v_addr), order);
        PhysChunk from_global;
        if (VESAL_UNLIKELY(!result)) {
            {
                std::shared_lock<std::shared_timed_mutex> read_lck(g_rw_mutex_);
                result =
                    TableLookUp(memory_info_by_vaddr_, reinterpret_cast<void*>(curr_v_addr), order);
                if (VESAL_UNLIKELY(!result)) {
                    VESAL_LOG(DEBUG) << "Not found entry, curr_v_addr=" << (uintptr_t)curr_v_addr;
                    // Not found entry, return 0 means not DMA.
                    return 0;
                }
                // de-reference here in case the map rebalance messing up original memory where
                // the 'result' points at.
                from_global = *result;
            }
            result = &from_global;
            t_tls_memory_info_by_vaddr_[from_global.start_addr] = from_global;
        }
        uint64_t page_ending =
            reinterpret_cast<uint64_t>(result->start_addr) + result->GetChunkSize();
        paddrs[ret] =
            result->phys_addr + (curr_v_addr - reinterpret_cast<uint64_t>(result->start_addr));
        if (curr_v_addr + left <= page_ending) {
            // Completely covered by this page
            paddr_sizes[ret] = left;
            left = 0;
        } else {
            // exceeds the this page, move the cursor and let the next round loopup handle it.
            uint64_t covered_size = page_ending - curr_v_addr;
            left -= covered_size;
            curr_v_addr += covered_size;
            paddr_sizes[ret] = covered_size;
        }
        ret++;
    }
    if (left > 0 && left != v_len) {
        // Not all covered, means not fully DMA
        g_metric_dma_buffer_but_not_covered_by_one_hugepage->Add(1);
    }
    // If not fully covered by DMA memory, return 0.
    return left > 0 ? 0 : ret;
}

bool AddressManager::ValidateExternalMemInfo(const MemoryInfo& memory_info) {
    if (!memory_info.virtual_addr) {
        VESAL_LOG(ERROR) << "Invalid memory info: virtual_addr is nullptr";
        return false;
    }
    if (!memory_info.len) {
        VESAL_LOG(ERROR) << "Invalid memory info: len is 0";
        return false;
    }
    // Currently 4KB pinned page is not supported
    if (memory_info.page_size != VESAL_PAGESIZE(PAGE_BIT_NUM_1GB) &&
        memory_info.page_size != VESAL_PAGESIZE(PAGE_BIT_NUM_2MB) &&
        memory_info.page_size != VESAL_PAGESIZE(PAGE_BIT_NUM_512MB)) {
        VESAL_LOG(ERROR) << "Invalid memory info: page_size needs to be 1GB/2MB/512MB, page_size="
                         << memory_info.page_size;
        return false;
    }
    if (!VESAL_IS_PAGE_ALIGNED(memory_info.virtual_addr, memory_info.page_size)) {
        VESAL_LOG(ERROR) << "Invalid memory info: virtual_addr needs to be aligned with page_size, "
                            "virtual_addr=0x"
                         << std::hex << memory_info.virtual_addr << ", page_size=" << std::dec
                         << memory_info.page_size;
        return false;
    }
    if (memory_info.len % memory_info.page_size) {
        VESAL_LOG(ERROR) << "Invalid memory info: len is not a multiple of page_size, len="
                         << memory_info.len << ", page_size=" << memory_info.page_size;
        return false;
    }
    return true;
}

bool AddressManager::FillPhysAddrIfMissing(const std::vector<uint64_t>& pfns,
                                           std::vector<PhysChunk>* chunks) {
    for (size_t i = 0; i < chunks->size(); ++i) {
        auto& chunk = chunks->at(i);
        if (!chunk.phys_addr) {
            chunk.phys_addr = CalcPhysAddr(chunk.start_addr, pfns.at(i));
        }
        if (!chunk.phys_addr) {
            return false;
        }
    }
    return true;
}

uint64_t LookUpAddr(void* addr) {
    MemoryPool* mp = MemoryPool::GetInstance();
    VESAL_DCHECK(mp && mp->initialized_);
    return mp->addr_manager_->GetPhysAddr(addr);
}

size_t LookUpAddrAcrossBound(
    void* addr, size_t v_len, uint64_t paddrs[], uint64_t paddr_sizes[], size_t max_num) {
    MemoryPool* mp = MemoryPool::GetInstance();
    VESAL_DCHECK(mp && mp->initialized_);
    return mp->addr_manager_->GetPhysAddrBound(addr, v_len, paddrs, paddr_sizes, max_num);
}

}  // namespace vesal
