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

#include <memory>

#include "defer.h"
#include "timestamp.h"
#include "vesal/metrics.h"
#include "vesal/vesal.h"

namespace vesal {

// Since metric registry will be used anywhere, declare it as global var.
// By default, g_metric_registry will init as DefaultMetricRegistry, which does no op when
// collecting metrics
extern std::shared_ptr<MetricRegistry> g_metric_registry;
extern std::shared_ptr<Counter> g_metric_memcpy_throughput;

inline uint32_t xorshift_8() {
    thread_local static uint32_t x = TimeStamp::Now();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x & 255;
}

inline bool IsEnableSampling() {
    return FLAGS_vesal_metrics_sample_rate && vesal::xorshift_8() < FLAGS_vesal_metrics_sample_rate;
}

#define DURATION_TO_RETURN(do_measure, metric, start_time)                                         \
    auto __duration_guard = defer([&]() {                                                          \
        if (do_measure) {                                                                          \
            (metric)->Set(vesal::TimeStamp::DurationToNs(vesal::TimeStamp::Now() - (start_time))); \
        }                                                                                          \
    });

template <typename T> inline void DoMeasureIfNeed(bool do_measure, T* metric, uint64_t start_time) {
    if (do_measure) {
        metric->Set(vesal::TimeStamp::DurationToNs(vesal::TimeStamp::Now() - start_time));
    }
}

}  // namespace vesal