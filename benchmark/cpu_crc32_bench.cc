/*
 * Copyright (c) 2024 ByteDance Inc.
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

#include <iostream>
#include <random>

#include "codec/dc_format.h"

static void initialize_data_with_random(uint8_t* data, size_t size) {
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<uint8_t> distribution(0, 255);

    for (size_t i = 0; i < size; ++i) {
        data[i] = distribution(generator);
    }
}

static void BM_CRC32(benchmark::State& state) {
    const size_t size = state.range(0);
    uint8_t* data = (uint8_t*)std::malloc(size);
    if (data == nullptr) {
        std::cerr << "Memory allocation failed" << std::endl;
        return;
    }

    initialize_data_with_random(data, size);

    for (auto _ : state) {
        uint32_t checksum = vesal::ComputeCRC32(0, reinterpret_cast<char*>(data), size);
        benchmark::DoNotOptimize(checksum);
    }

    std::free(data);

    state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
}

BENCHMARK(BM_CRC32)->RangeMultiplier(2)->Range(1 << 16, 1 << 28); // Test from 64KB to 256MB

int main(int argc, char** argv) {
    // Initialize Google Benchmark and run benchmarks
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    ::benchmark::RunSpecifiedBenchmarks();

    return 0;
}
