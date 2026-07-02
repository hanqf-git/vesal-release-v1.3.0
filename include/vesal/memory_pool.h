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

#include <numaif.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <vector>

namespace vesal {

enum class HugePageSize : uint8_t {
    k2MB = 0,
    k1GB = 1,
    k512MB = 2, // for ARM.
    kUnknown = 255
    // No 4KB yet.
};

inline size_t GetHugePageSize(HugePageSize size) {
    switch (size) {
    case HugePageSize::k2MB:
        return 2 * 1024 * 1024;
    case HugePageSize::k1GB:
        return 1024 * 1024 * 1024;
    case HugePageSize::k512MB:
        return 512 * 1024 * 1024;
    default:
        return 0;
    }
}

inline HugePageSize ToHugePageSize(size_t sz) {
    if (sz == 2 * 1024 * 1024) {
        return HugePageSize::k2MB;
    } else if (sz == 1024 * 1024 * 1024) {
        return HugePageSize::k1GB;
    } else if (sz == 512 * 1024 * 1024) {
        return HugePageSize::k512MB;
    }
    return HugePageSize::kUnknown;
}

static constexpr size_t kMaxNodeNum = 32;
struct MemoryPoolInitOption {
    bool init_mem_pool = true;  // Note HWs(e.g, QAT/DSA) require mem pool. Only set to false if not
                                // using HW accelaration.
    HugePageSize prealloc_page_size =
        HugePageSize::k2MB;          // Mem pool only preallocates one type of pages as memory pool.
    size_t prealloc_size_mb = 4096;  // Hugepage mem size should vesal preallocate.
    size_t cache_recycle_threshold_size_mb =
        256;  // The size limit of vesal memory pool TLS cache in MB, GC will be triggered after
              // exceeding this limit.
    size_t recycle_chunk_num =
        16;  // The num of recyclable chunks of one thread that triggers GC, 16 by default.
    size_t fetch_chunk_num = 8;  // How many chunks per fetch by TLS cache from central chunk pool.
    size_t prealloc_numa_mb[kMaxNodeNum] =
        {};  // How preallocated memory distributed in each numa nodes. E.g, [2048, 0, 2048,...]
             // means node0 node 2 has 2048MB each. Remain all elements zero if not specify and let
             // vesal decide numa memory policy. Otherwise the sum of the array elements MUST equal
             // to prealloc_size_mb.
};

struct MemoryInfo {
    // required
    void* virtual_addr = nullptr;
    // optional
    // physical_addr is an array, and size is len/page_size.
    // If phys_addr is provided with nullptr, memory pool will try to deduct internally.
    // Note: physical_addr is ignored, will deduct internally for now.
    uint64_t* physical_addr = nullptr;
    // required
    // memory size, and len is a multiple of page_size
    uint64_t len = 0;
    // required
    // page_size in bytes
    uint64_t page_size = 0;

    friend std::ostream& operator<<(std::ostream& os, const MemoryInfo& info);
};

inline std::ostream& operator<<(std::ostream& os, const MemoryInfo& info) {
    os << "len=" << info.len << ", page_size=" << info.page_size
       << ", virtual_addr=" << info.virtual_addr << ", physical_addr=" << info.physical_addr;
    return os;
}

struct PhysChunk;
class AddressManager;

class MemoryPool {
public:
    static MemoryPool* GetInstance();

    /**
     * Must be called before use memory pool. This is a process-level API.
     *
     * @param: opt - Memory behaviour related options.
     *
     * @return: true - if successfully initialized, or already initialized
     *          false - if not initialised and fail to initialize. The major reason is hugepage
     * inefficiency.
     *
     * @thread-safe: yes
     */
    virtual bool Init(const MemoryPoolInitOption& opt) = 0;

    // Same as Init(const MemoryPoolInitOption& opt) but use default MemoryPoolInitOption.
    virtual bool Init() = 0;

    /**
     * Register a list of memory info provided by the user. It is user's reponsibility to ensure the
     * memory is allocated and DMA-able during the use, as well as the info is correct. The memory
     * pool only records address info and its length.
     * For the sake of simplicity, the memory is required to be 1GB/2MB-page-aligned.
     * NOTE: The memory from mmap(addr=nullptr) naturally meets this requirement.
     *
     * @param: infos - a list of user provided memory info. It's possible that
     * info.physical_addr = nullptr,
     * in which case phys_addr will be deducted internally.
     *
     * @return: true - if success, false otherwise.
     *
     * @thread-safe: yes
     */
    virtual bool Register(const std::vector<MemoryInfo>& infos) = 0;

    /**
     * Allocate `size` bytes DMA-able memory.
     *
     * @param: size - the size of memory. size must be >=0 and <= 1MB.
     *
     * @return: the pointer to the memory if success, nullptr if failed.
     *
     * @thread-safe: yes
     */
    virtual void* Allocate(size_t size = 0) = 0;

    /**
     * Deallocate a block of memory allocated from this MemoryPool.
     *
     * @param: ptr - the pointer to the memory to be released.
     *
     * @thread-safe: yes
     */
    virtual void Deallocate(void* ptr) = 0;

    /**
     * Release all resource managed by the memory pool, reset the MemoryPool to be un-initialized.
     * Do not use any memory pool after this call. It is user's responsibility to ensure there is no
     * memory in use when calling this function. This is a process-level API. User must ensure all
     * threads exit before calling this.
     *
     * @thread-safe: yes
     */
    virtual void Reset() = 0;

    /**
     * Get the memory size in byte that holding by the memory pool.
     */
    virtual uint64_t GetMemoryUsage() const = 0;

    /**
     * Get the memory info that allocated by memory pool at CURRENT STATE.
     * Used for exposing the underlying physical page info(typically hugepages).
     */
    virtual std::vector<MemoryInfo> GetInternalMemoryInfoVec() const = 0;

protected:
    MemoryPool() = default;
    virtual ~MemoryPool() = default;
    static bool initialized_;
    static std::unique_ptr<AddressManager> addr_manager_;

    friend uint64_t LookUpAddr(void* vaddr);
    friend size_t LookUpAddrAcrossBound(
        void* vaddr, size_t v_len, uint64_t paddrs[], uint64_t paddr_sizes[], size_t max_num);
};

}  // namespace vesal