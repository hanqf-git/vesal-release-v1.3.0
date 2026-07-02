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

#include <benchmark/benchmark.h>

#include <cstdlib>
#include <cstring>
#include <vector>

#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/vesal.h"

void GlobalSetUp() {
    FLAGS_vesal_memory_pool_prealloc_size_mb = 4096 + 4096;
    vesal::LogSettings s;
    vesal::InitLogging(s);
    vesal::MemoryPool* mp = vesal::MemoryPool::GetInstance();
    VESAL_CHECK(mp->Init());
}

void GlobalTearDown() {
    vesal::MemoryPool::GetInstance()->Reset();
}

void NoRecycle(benchmark::State& state) {
    const uint32_t kBlockSize = state.range(0);
    const uint32_t kMaxSize = (8 << 20) / kBlockSize;
    auto* mp_ = vesal::MemoryPool::GetInstance();
    auto addrs = std::unique_ptr<void*[]>(new void*[kMaxSize]);
    while (state.KeepRunningBatch(kMaxSize << 1)) {
        for (uint32_t i = 0; i < kMaxSize; i++)
            addrs[i] = mp_->Allocate(kBlockSize);
        for (uint32_t i = 0; i < kMaxSize; i++)
            mp_->Deallocate(addrs[i]);
    }
}

void RecycleRegularly(benchmark::State& state) {
    const uint32_t kBlockSize = state.range(0);
    const uint32_t kMaxSize = (64 << 20) / kBlockSize;
    auto* mp_ = vesal::MemoryPool::GetInstance();
    auto addrs = std::unique_ptr<void*[]>(new void*[kMaxSize]);
    std::vector<void*> pinned_addrs;
    for (uint32_t i = 0; i < FLAGS_vesal_memory_pool_cache_recycle_threshold_size_mb << 20;
         i += kBlockSize)
        pinned_addrs.push_back(mp_->Allocate(kBlockSize));
    while (state.KeepRunningBatch(1UL * (kMaxSize << 1))) {
        for (int _ = 0; _ < 1; _++) {
            for (uint32_t i = 0; i < kMaxSize; i++)
                addrs[i] = mp_->Allocate(kBlockSize);
            for (uint32_t i = 0; i < kMaxSize; i++)
                mp_->Deallocate(addrs[i]);
        }
    }
    for (auto addr : pinned_addrs)
        mp_->Deallocate(addr);
}

BENCHMARK(NoRecycle)->RangeMultiplier(2)->Range(1 << 10, 512 << 10)->ThreadRange(1, 16);

BENCHMARK(RecycleRegularly)->RangeMultiplier(2)->Range(1 << 10, 512 << 10)->ThreadRange(1, 16);

int main(int argc, char** argv) {
    GlobalSetUp();

    // Initialize Google Benchmark and run benchmarks
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    ::benchmark::RunSpecifiedBenchmarks();

    GlobalTearDown();
    return 0;
}