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
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "flat_hash_map.hpp"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/metrics.h"

#ifdef VESAL_ENABLE_ASAN
#define IF_ASAN(x) x
#else
#define IF_ASAN(x)
#endif

#define VESAL_PTR_ADD(ptr, x) ((void*)((uintptr_t)(ptr) + (x)))
#define VESAL_PTR_SUB(ptr, x) ((void*)((uintptr_t)(ptr) - (x)))

namespace vesal {

enum class AlignType : uint8_t {
    kPage4kb = 1,
    kPage2Mb = 2,
    kPage1Gb = 3,
    kPage512Mb = 4,
    kUnknown = 255
};

#define PAGE_BIT_NUM_4KB 12
#define PAGE_BIT_NUM_2MB 21
#define PAGE_BIT_NUM_1GB 30
#define PAGE_BIT_NUM_512MB 29

struct HugePageHash {
    std::size_t operator()(const void* ptr) const {
        return reinterpret_cast<std::size_t>(ptr) >> PAGE_BIT_NUM_2MB;
    }
};

#define VESAL_PAGESIZE(page_bit_num) (1UL << (page_bit_num))
#define VESAL_PAGEMASK(page_size) (~((page_size) - 1))

// not like linux kernel PAGE_ALIGN
// here we always align to the page begin
#define VESAL_PAGE_ALIGN(addr, page_size) (((uintptr_t)(addr)) & VESAL_PAGEMASK(page_size))

// Translate AlignType to page size in bytes
inline uint64_t AlignTypeToPageSize(const AlignType& type) {
    switch (type) {
    case AlignType::kPage4kb:
        return VESAL_PAGESIZE(PAGE_BIT_NUM_4KB);
    case AlignType::kPage2Mb:
        return VESAL_PAGESIZE(PAGE_BIT_NUM_2MB);
    case AlignType::kPage1Gb:
        return VESAL_PAGESIZE(PAGE_BIT_NUM_1GB);
    case vesal::AlignType::kPage512Mb:
        return VESAL_PAGESIZE(PAGE_BIT_NUM_512MB);
    default:
        VESAL_LOG(CRITICAL) << "Unknown align type";
    }
    return 0;
}

// Translate page size in bytes to AlignType
inline AlignType PageSizeToAlignType(uint64_t page_size) {
    switch (page_size) {
    case VESAL_PAGESIZE(PAGE_BIT_NUM_4KB):
        return AlignType::kPage4kb;
    case VESAL_PAGESIZE(PAGE_BIT_NUM_2MB):
        return AlignType::kPage2Mb;
    case VESAL_PAGESIZE(PAGE_BIT_NUM_1GB):
        return AlignType::kPage1Gb;
    case VESAL_PAGESIZE(PAGE_BIT_NUM_512MB):
        return AlignType::kPage512Mb;
    }
    return AlignType::kUnknown;
}

// For both memory pool allocated memory and user provided memory,
// lookup the phys address of a given virt addr.
// This is used for QAT driver to retrieve phys addr at runtime.
uint64_t LookUpAddr(void* addr);

// Get the physical addresses, even vaddr is lying on multiple hugepages. Return the number of
// physical addresses. Note if vaddr is not fully covered by hugepages, return 0.
size_t LookUpAddrAcrossBound(
    void* addr, size_t v_len, uint64_t paddrs[], uint64_t paddr_sizes[], size_t max_num);

// Get the physical address, if vaddr is dma-able and doesn't span multiple pages.
// Otherwise return 0.
inline uint64_t LookUpAddrIfNotAcrossBound(void* addr, size_t v_len) {
    constexpr size_t lookup_max_num = 2;
    thread_local uint64_t paddrs[lookup_max_num] = {};
    thread_local size_t paddr_sizes[lookup_max_num] = {};
    size_t ret = LookUpAddrAcrossBound(addr, v_len, paddrs, paddr_sizes, lookup_max_num);
    if (ret == 1)
        return paddrs[0];
    return 0;
}

// Get page frame number based on virtual address.
// WARN: Very slow due to file reading.
std::vector<uint64_t> GetPageFrameNumberOrZero(const std::vector<void*>& addrs);

// Calculate the page flags based on page frame number.
std::vector<uint64_t> GetPageFlagOrZero(const std::vector<uint64_t>& pfns);
// Calculate the phys addr based on page frame number.
uint64_t CalcPhysAddr(void* addr, uint64_t pfn);
std::vector<uint64_t> CalcPhysAddr(const std::vector<void*>& addrs,
                                   const std::vector<uint64_t>& pfns);

bool IsMemoryPinned(uint64_t page_flag);

bool IsMemoryPinned(const std::vector<uint64_t>& page_flags);

// page_size can only be 2MB or 1GB. node == -1 means not specify numa node and let OS decide.
void* AllocHugepageFn(size_t page_size, size_t page_num = 1, int node = -1);

// size can only be 2MB or 1GB
void DeallocHugepageFn(void* addr, size_t size);

// Align the address based on align, can only be one of the following
inline void* AlignPage(void* addr, AlignType type) {
    uintptr_t ret = VESAL_PAGE_ALIGN(addr, AlignTypeToPageSize(type));
    VESAL_LOG(DEBUG) << "addr=" << (uintptr_t)addr << ", ret=" << ret
                     << ", type=" << static_cast<uint8_t>(type);
    return reinterpret_cast<void*>(ret);
}

struct PhysChunk {
    void* start_addr;  // start address for this chunk
    uint64_t phys_addr;
    AlignType align_type;
    PhysChunk* next = nullptr;

    size_t GetChunkSize() const {
        return AlignTypeToPageSize(align_type);
    }
};

// Store and manage the memory address (both allocated or registered).
// Also provides the ability to lookup the physical address of a given virtual address.
class AddressManager {
public:
    void Init();
    void Reset();

    void AddInternalEntry(PhysChunk* chunk);
    bool AddExternalEntry(const MemoryInfo& memory_info);

    bool AddExternalEntries(const std::vector<MemoryInfo>& memory_infos);

    // order means order to lookup, since we don't know the memory based on
    // what type(2mb-hugepage? 1gb-hugepage? 4kb-pages?). It should be a 3-elements array and the
    // element can only be: 4KB, 2MB, 1GB
    // if nullptr is given, will use default setting [2MB, 1GB, 4KB].
    uint64_t GetPhysAddr(void* vaddr, const AlignType* order = nullptr);

    // This function is able to get the physical addresses of a given virtual address even the
    // buffer is lying across multiple DMA buffers(e.g, hugepages). It return the number of physical
    // addresses it gets and return value will not exceed num provided.
    // E.g, if the vaddr buffer is lying across two 2MB-hugepages, the function
    // returns 2, and paddrs[0] is filled with the paddr of vaddr on the first hugepage, and
    // paddrs[1] is filled with the paddr of another hugepage.
    // vaddr is DMA only when it's fully covered by one or more hugepages, otherwise it's not DMA
    // and return 0.
    size_t GetPhysAddrBound(void* vaddr,
                            size_t v_len,
                            uint64_t paddrs[],
                            size_t paddr_sizes[],
                            size_t max_num,
                            const AlignType* order = nullptr);

private:
    // Check:
    // 1. virtual_addr not nullptr
    // 2. len > 0
    // 3. page_size is 1GB/2MB
    // 4. virtual_addr is aligned with page_size, we don't accept random middle postion for
    // simplicity
    // 5. len is a multiple of page_size
    bool ValidateExternalMemInfo(const MemoryInfo& info);

    bool FillPhysAddrIfMissing(const std::vector<uint64_t>& pfns, std::vector<PhysChunk>* chunks);

    std::shared_timed_mutex g_rw_mutex_;

    ska::flat_hash_map<void*, PhysChunk, HugePageHash> memory_info_by_vaddr_;

    thread_local static ska::flat_hash_map<void*, PhysChunk, HugePageHash>
        t_tls_memory_info_by_vaddr_;

    uint32_t metrics_task_id_;

    std::shared_ptr<Gauge> metric_memory_pool_chunk_entry_num_;
};

}  // namespace vesal
