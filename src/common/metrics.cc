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

#include "vesal/metrics.h"

#include <cstdint>
#include <memory>

#include "gflags/gflags.h"
#include "metrics_internal.h"

DEFINE_uint32(vesal_metrics_sample_rate,
              255,
              "The ratio of how many traffic to be measured out of 255, range from 0 to 255, "
              "default value is 255, which means all the traffic will be measured");

namespace vesal {

// If users don't provide their metric registry implementation, vesal will do no op when collecting
// metrics via DefaultMetricRegistry
std::shared_ptr<MetricRegistry> g_metric_registry = std::make_shared<DefaultMetricRegistry>();

class DefaultGauge : public Gauge {
public:
    void Set(double val) override {
        // no op
    }
};

class DefaultCounter : public Counter {
public:
    void Add(int64_t val) override {
        // no op
    }
};

class DefaultHistogram : public Histogram {
public:
    void Set(int64_t val) override {
        // no op
    }
};

std::shared_ptr<Counter> g_metric_memcpy_throughput = std::make_shared<DefaultCounter>();

std::shared_ptr<Gauge> DefaultMetricRegistry::RegisterGauge(const std::string& name,
                                                            const TagList& tags) {
    return std::make_shared<DefaultGauge>();
};

std::shared_ptr<Counter> DefaultMetricRegistry::RegisterCounter(const std::string& name,
                                                                const TagList& tags) {
    return std::make_shared<DefaultCounter>();
}
std::shared_ptr<Histogram> DefaultMetricRegistry::RegisterHistogram(const std::string& name,
                                                                    const TagList& tags) {
    return std::make_shared<DefaultHistogram>();
}
};  // namespace vesal