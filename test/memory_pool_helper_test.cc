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

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/defer.h"
#include "common/qat/qat_util.h"
#include "ut_util.h"
#include "vesal/memory_pool.h"
#include "vesal/vesal.h"

extern "C" {
// clang-format off
// this need to be before icp_sal_poll.h for CpaStatus declaration
#include <cpa.h>
#include <icp_sal_poll.h>
// clang-format on
#include <cpa_types.h>
#include <dc/cpa_dc.h>
}

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

namespace vesal {

namespace {
std::vector<uint64_t> CalcPhysAddrSlow(const std::vector<void*>& addrs) {
    std::vector<uint64_t> pfns = GetPageFrameNumberOrZero(addrs);
    return CalcPhysAddr(addrs, pfns);
}

uint64_t CalcPhysAddrSlow(void* addr) {
    return CalcPhysAddrSlow(std::vector<void*>({addr})).at(0);
}

bool IsMemoryPinnedSlow(const std::vector<void*>& addrs) {
    return IsMemoryPinned(GetPageFlagOrZero(GetPageFrameNumberOrZero(addrs)));
}
}  // namespace
// TODO(sjj): only enable these tests if the hugepage is enabled. Will check by MemoryPool::Iinit(),
// currently not done yet.

class AddressManagerTest : public ::testing::Test {
public:
    void SetUp() override {
        vesal::AddressManager::t_tls_memory_info_by_vaddr_.clear();
        VESAL_INIT_CODEC_QAT_ONLY;
    }
    void TearDown() override {
        VESAL_UNINIT;
    }
};

class MemoryPoolHelperTest : public ::testing::Test {
public:
    void SetUp() override {
        vesal::AddressManager::t_tls_memory_info_by_vaddr_.clear();
    }
};

TEST_F(MemoryPoolHelperTest, IsMemoryPinnedCheck) {
    // ASAN intercepts mlock, we can't pin memory in this case.
    // Skip test if asan enabled.
    const size_t size = 2 * 1024 * 1024;
    const size_t num = 32;
#ifndef __SANITIZE_ADDRESS__
    std::vector<void*> addr(num, nullptr);
    for (size_t i = 0; i < num; ++i) {
        addr[i] = malloc(size);
    }
    EXPECT_FALSE(IsMemoryPinnedSlow(addr));
    for (size_t i = 0; i < num; ++i) {
        int mlock_r = mlock(addr[i], size);
        ASSERT_EQ(mlock_r, 0);
    }
    EXPECT_TRUE(IsMemoryPinnedSlow(addr));
    for (size_t i = 0; i < num; ++i) {
        munlock(addr[i], size);
    }
    for (size_t i = 0; i < num; ++i) {
        free(addr[i]);
    }
#endif
    std::vector<void*> huge_addr(num, nullptr);
    for (size_t i = 0; i < num; ++i) {
        huge_addr[i] = AllocHugepageFn(size);
        ASSERT_NE(huge_addr[i], nullptr);
    }
    EXPECT_TRUE(IsMemoryPinnedSlow(huge_addr));

    for (size_t i = 0; i < num; ++i) {
        DeallocHugepageFn(huge_addr[i], size);
    }
}

TEST_F(MemoryPoolHelperTest, AllocHugepageFnTooManyReturnNullptr) {
    // run out of hugepages
    std::vector<void*> addrs;
    while (true) {
        void* addr = AllocHugepageFn(1 * 1024 * 1024 * 1024);
        if (addr == nullptr) {
            break;
        }
        addrs.push_back(addr);
    }
    ASSERT_GT(addrs.size(), 0);
    for (auto addr : addrs) {
        DeallocHugepageFn(addr, 1 * 1024 * 1024 * 1024);
    }
}

TEST_F(MemoryPoolHelperTest, CalcAddrForInvalidAddrReturn0) {
    std::vector<void*> addrs = {reinterpret_cast<void*>(0), reinterpret_cast<void*>(~0ULL)};
    EXPECT_FALSE(IsMemoryPinnedSlow(addrs));

    std::vector<uint64_t> phys_addrs = CalcPhysAddrSlow(addrs);
    for (auto phys_addr : phys_addrs) {
        EXPECT_EQ(phys_addr, 0) << phys_addr;
    }
}

TEST_F(MemoryPoolHelperTest, LookUpAddrForInternalAndExternal) {
    MemoryPool* mp = MemoryPool::GetInstance();
    EXPECT_TRUE(mp->Init());
    size_t page_size = 2 * 1024 * 1024;
    size_t page_num = 2;
    size_t pages_len = page_size * page_num;
    size_t internal_allocate_size = 512 * 1024;
    void* addr = mp->Allocate(internal_allocate_size);
    ASSERT_NE(addr, nullptr);
    MemoryInfo mem_info{.virtual_addr = AllocHugepageFn(page_size, page_num),
                        .physical_addr = nullptr,
                        .len = pages_len,
                        .page_size = page_size};
    EXPECT_TRUE(mp->Register({mem_info}));
    size_t step = 1024;
    // internal
    for (size_t offset = 0; offset < internal_allocate_size; offset += step)
        ASSERT_NE(LookUpAddr(static_cast<char*>(addr) + offset), 0);
    // external
    for (size_t offset = 0; offset < pages_len; offset += step)
        ASSERT_NE(LookUpAddr(static_cast<char*>(mem_info.virtual_addr) + offset), 0);
    mp->Deallocate(addr);
    DeallocHugepageFn(mem_info.virtual_addr, mem_info.len);
    mp->Reset();
}

extern AlignType PageAlignType(void* addr);

TEST_F(MemoryPoolHelperTest, PageAlign) {
    uintptr_t initial = (1UL << 31) - 1;
    void* addr_4kb = AlignPage((void*)initial, AlignType::kPage4kb);
    void* addr_2mb = AlignPage((void*)initial, AlignType::kPage2Mb);
    void* addr_1gb = AlignPage((void*)initial, AlignType::kPage1Gb);
    ASSERT_EQ(PageAlignType(addr_4kb), AlignType::kPage4kb);
    ASSERT_EQ(PageAlignType(addr_2mb), AlignType::kPage2Mb);
    ASSERT_EQ(PageAlignType(addr_1gb), AlignType::kPage1Gb);
    ASSERT_EQ(PageAlignType((void*)((uintptr_t)addr_1gb + 1)), AlignType::kUnknown);
}

// add, lookup, remove
TEST_F(AddressManagerTest, InternalEntryApis) {
    std::unique_ptr<AddressManager> addr_mngr = std::make_unique<AddressManager>();

    size_t size_1gb = 1UL * 1024 * 1024 * 1024;
    PhysChunk chunk_1gb{.start_addr = AllocHugepageFn(size_1gb)};
    chunk_1gb.align_type = AlignType::kPage1Gb;
    ASSERT_NE(chunk_1gb.start_addr, nullptr);
    chunk_1gb.phys_addr = CalcPhysAddrSlow(chunk_1gb.start_addr);
    ASSERT_NE(chunk_1gb.phys_addr, 0UL);

    addr_mngr->AddInternalEntry(&chunk_1gb);
    ASSERT_EQ(addr_mngr->GetPhysAddr(chunk_1gb.start_addr), chunk_1gb.phys_addr);

    size_t size_2mb = 2UL * 1024 * 1024;
    size_t page_num_2mb = 5;
    void* chunks_2mb_addr = AllocHugepageFn(size_2mb, page_num_2mb);
    std::vector<PhysChunk> chunks_2mb(page_num_2mb);
    for (size_t i = 0; i < page_num_2mb; ++i) {
        chunks_2mb[i].start_addr = static_cast<char*>(chunks_2mb_addr) + i * size_2mb;
        chunks_2mb[i].align_type = AlignType::kPage2Mb;
        ASSERT_NE(chunks_2mb[i].start_addr, nullptr);
        chunks_2mb[i].phys_addr = CalcPhysAddrSlow(chunks_2mb[i].start_addr);
        ASSERT_NE(chunks_2mb[i].phys_addr, 0UL);
        addr_mngr->AddInternalEntry(&chunks_2mb[i]);
        ASSERT_EQ(addr_mngr->GetPhysAddr(chunks_2mb[i].start_addr), chunks_2mb[i].phys_addr);
    }

    DeallocHugepageFn(chunk_1gb.start_addr, size_1gb);
    DeallocHugepageFn(chunks_2mb_addr, size_2mb * page_num_2mb);
}

// add, lookup, remove
TEST_F(AddressManagerTest, ExternalEntryApis) {
    std::unique_ptr<AddressManager> addr_mngr = std::make_unique<AddressManager>();

    size_t sizes[1] = {2UL * 1024 * 1024};
    std::vector<MemoryInfo> memory_info_vec;
    for (size_t i = 0; i < 1; i++) {
        void* hugepage_start = AllocHugepageFn(sizes[i]);
        MemoryInfo info{.virtual_addr = hugepage_start,
                        .physical_addr = nullptr,
                        .len = sizes[i],
                        .page_size = sizes[i]};
        ASSERT_NE(info.virtual_addr, nullptr);

        ASSERT_TRUE(addr_mngr->AddExternalEntry(info));
        ASSERT_EQ(addr_mngr->GetPhysAddr(info.virtual_addr), CalcPhysAddrSlow(hugepage_start));

        memory_info_vec.push_back(std::move(info));
    }
    for (const auto& info : memory_info_vec) {
        DeallocHugepageFn(info.virtual_addr, info.len);
    }
}

TEST_F(AddressManagerTest, UnAlignedExternalEntryRegisterFail) {
    std::unique_ptr<AddressManager> addr_mngr = std::make_unique<AddressManager>();

    size_t sizes[1] = {2UL * 1024 * 1024};
    size_t offset = 16;
    for (size_t i = 0; i < 1; i++) {
        void* hugepage_start = AllocHugepageFn(sizes[i]);
        MemoryInfo info{.virtual_addr = hugepage_start,
                        .physical_addr = nullptr,
                        .len = sizes[i],
                        .page_size = sizes[i]};
        ASSERT_NE(info.virtual_addr, nullptr);
        info.virtual_addr = reinterpret_cast<void*>((uintptr_t)info.virtual_addr + offset);

        ASSERT_FALSE(addr_mngr->AddExternalEntry(info));

        DeallocHugepageFn(hugepage_start, sizes[i]);
    }
}

TEST_F(AddressManagerTest, InvalidExternalEntryRegisterFail) {
    std::unique_ptr<AddressManager> addr_mngr = std::make_unique<AddressManager>();
    size_t size_2mb = 2UL * 1024 * 1024;
    size_t size_1gb = 512 * size_2mb;
    void* hugepage_start = AllocHugepageFn(size_2mb);
    // virtual_addr is null
    {
        MemoryInfo info{.virtual_addr = nullptr,
                        .physical_addr = nullptr,
                        .len = size_2mb,
                        .page_size = size_2mb};
        ASSERT_FALSE(addr_mngr->AddExternalEntry(info));
    }
    // page_size is not 1gb/2mb
    {
        MemoryInfo info{.virtual_addr = hugepage_start,
                        .physical_addr = nullptr,
                        .len = size_2mb,
                        .page_size = size_2mb + 1};
        ASSERT_FALSE(addr_mngr->AddExternalEntry(info));
    }
    // len is not a multiple of page_size
    {
        MemoryInfo info{.virtual_addr = hugepage_start,
                        .physical_addr = nullptr,
                        .len = size_2mb,
                        .page_size = size_1gb};
        ASSERT_FALSE(addr_mngr->AddExternalEntry(info));
    }
    DeallocHugepageFn(hugepage_start, size_2mb);
}

TEST_F(AddressManagerTest, LookupExternalExceedsLength) {
    AddressManager addr_mngr;
    size_t huge_size = 2 * 1024 * 1024;
    MemoryInfo mem_info{.virtual_addr = AllocHugepageFn(huge_size),
                        .physical_addr = nullptr,
                        .len = huge_size,
                        .page_size = huge_size};
    addr_mngr.AddExternalEntry(mem_info);

    ASSERT_NE(addr_mngr.GetPhysAddr(mem_info.virtual_addr), 0);
    // calc the addr exceeds boundary
    ASSERT_EQ(addr_mngr.GetPhysAddr(reinterpret_cast<void*>(
                  reinterpret_cast<uint64_t>(mem_info.virtual_addr) + huge_size)),
              0);
    DeallocHugepageFn(mem_info.virtual_addr, huge_size);
}

TEST_F(AddressManagerTest, ExternalAndInternalMultiThread) {
    std::unique_ptr<AddressManager> addr_mngr = std::make_unique<AddressManager>();

    size_t size_2mb = 2UL * 1024 * 1024;
    size_t size_1gb = 1UL * 1024 * 1024 * 1024;

    std::mutex vec_mutex;
    std::vector<std::pair<void*, size_t>> hugepage_info_vec;

    auto internal = [&addr_mngr, &vec_mutex, &hugepage_info_vec, size_1gb]() {
        PhysChunk chunk{.start_addr = AllocHugepageFn(size_1gb)};
        chunk.align_type = AlignType::kPage1Gb;
        ASSERT_NE(chunk.start_addr, nullptr);
        chunk.phys_addr = CalcPhysAddrSlow(chunk.start_addr);
        ASSERT_NE(chunk.phys_addr, 0UL);

        addr_mngr->AddInternalEntry(&chunk);
        ASSERT_EQ(addr_mngr->GetPhysAddr(chunk.start_addr), chunk.phys_addr);

        std::lock_guard<std::mutex> lock(vec_mutex);
        hugepage_info_vec.push_back({chunk.start_addr, size_1gb});
    };
    auto external = [&addr_mngr, &vec_mutex, &hugepage_info_vec, size_2mb]() {
        size_t sizes[1] = {size_2mb};
        for (size_t i = 0; i < 1; i++) {
            void* hugepage_start = AllocHugepageFn(sizes[i]);
            MemoryInfo info{.virtual_addr = hugepage_start,
                            .physical_addr = nullptr,
                            .len = sizes[i],
                            .page_size = sizes[i]};
            ASSERT_NE(info.virtual_addr, nullptr);

            ASSERT_TRUE(addr_mngr->AddExternalEntry(info));
            ASSERT_EQ(addr_mngr->GetPhysAddr(info.virtual_addr), CalcPhysAddrSlow(hugepage_start));

            std::lock_guard<std::mutex> lock(vec_mutex);
            hugepage_info_vec.push_back({hugepage_start, sizes[i]});
        }
    };

    auto proccess = [&internal, &external]() {
        internal();
        external();
    };

    std::vector<std::thread> threads;
    size_t thread_num = 3;
    while (thread_num--) {
        threads.emplace_back(proccess);
    }
    for (auto& thread : threads) {
        thread.join();
    }

    for (const auto& pair : hugepage_info_vec)
        DeallocHugepageFn(pair.first, pair.second);
}

void AssertOkCallback(void* ctx, CpaStatus status) {
    EXPECT_EQ(status, CPA_STATUS_SUCCESS);
}

TEST_F(MemoryPoolHelperTest, InternalMetadataSizeLimit) {
    EXPECT_LE(sizeof(PhysChunk), 32);
}

TEST_F(MemoryPoolHelperTest, ExternalMeminfoCrossMultiPages) {
    // This must have two hugepages
    RAII_VESAL_INIT_CODEC_QAT_ONLY;
    const size_t mb_2 = 1 << 21;
    const size_t num = 8;
    auto* pages = AllocHugepageFn(mb_2, num);
    MemoryInfo info{
        .virtual_addr = pages, .physical_addr = nullptr, .len = mb_2 * num, .page_size = mb_2};
    EXPECT_TRUE(MemoryPool::GetInstance()->Register({info}));
    EXPECT_EQ(LookUpAddrIfNotAcrossBound(pages, mb_2 * num), 0);
    EXPECT_NE(LookUpAddrIfNotAcrossBound(pages, mb_2), 0);
    EXPECT_EQ(LookUpAddrIfNotAcrossBound(pages, mb_2), LookUpAddr(pages));
    const size_t max_num = num << 1;
    uint64_t paddrs[max_num];
    size_t paddr_sizes[max_num];
    void* vaddr = reinterpret_cast<void*>(reinterpret_cast<uint64_t>(pages) + 4096);
    auto ret = LookUpAddrAcrossBound(vaddr, mb_2 * num, paddrs, paddr_sizes, max_num);
    // Not fully covered.
    EXPECT_EQ(ret, 0);
    ret = LookUpAddrAcrossBound(vaddr, mb_2 * num - 4096, paddrs, paddr_sizes, max_num);
    // Now fully covered.
    EXPECT_EQ(ret, num);
    EXPECT_EQ(paddrs[0], LookUpAddr(vaddr));
    EXPECT_EQ(paddr_sizes[0], info.page_size - 4096);
    for (size_t i = 1; i < num; i++) {
        EXPECT_EQ(paddr_sizes[i], info.page_size);
        EXPECT_EQ(
            paddrs[i],
            LookUpAddr(reinterpret_cast<void*>(reinterpret_cast<uint64_t>(pages) + i * mb_2)));
    }
    DeallocHugepageFn(pages, mb_2 * num);
}

TEST_F(MemoryPoolHelperTest, Hugepage512MbTest) {
    RAII_VESAL_INIT(false, false, false);
    // Test if 512MB hugepage can be allocated and deallocated correctly
    const size_t mb_512 = 512ul * 1024 * 1024;
    void* page = AllocHugepageFn(mb_512, 1);
    if (page == nullptr) {
        GTEST_SKIP() << "Skip test since no 512MB hugepage";
    }
    // Test if 512MB hugepage can be registered and looked up correctly
    MemoryInfo info = {.virtual_addr = page,
                       .physical_addr = nullptr,
                       .len = mb_512,
                       .page_size = mb_512};
    EXPECT_TRUE(MemoryPool::GetInstance()->Register({info}));
    void* p = page;
    uint64_t phys_addr = CalcPhysAddrSlow(page);
    EXPECT_NE(phys_addr, 0);
    while (p < reinterpret_cast<void*>(reinterpret_cast<intptr_t>(page) + mb_512)) {
        EXPECT_EQ(LookUpAddr(p),
                  phys_addr + (reinterpret_cast<intptr_t>(p) - reinterpret_cast<intptr_t>(page)))
            << std::hex << reinterpret_cast<intptr_t>(p) << " vs " << phys_addr;
        p = reinterpret_cast<void*>(reinterpret_cast<intptr_t>(p) + 2 * 1024 * 1024);
    }
    DeallocHugepageFn(page, mb_512);
}

TEST_F(MemoryPoolHelperTest, PreallocWith512Mb) {
    FLAGS_vesal_log_console_output = true;
    RAII_VESAL_INIT(false, false, false);
    const size_t mb_512 = 512ul * 1024 * 1024;
    void* page = AllocHugepageFn(mb_512, 1);
    if (page == nullptr) {
        GTEST_SKIP() << "Skip test since no 512MB hugepage";
    }
    DeallocHugepageFn(page, mb_512);
    InitOptions opts;
    opts.codec_init_opt.init_qat = false;
    opts.cypher_init_opt.init_qat = false;
    opts.data_flow_init_opt.init_dsa = false;
    opts.mem_pool_init_opt.prealloc_page_size = HugePageSize::k512MB;
    EXPECT_TRUE(Init(opts));
    Uninit();
    FLAGS_vesal_log_console_output = false;
}

}  // namespace vesal
