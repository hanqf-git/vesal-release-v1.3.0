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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vesal {

/*
To enable vesal metrics, user needs to implement below classes: Gauge, Counter, Histogram and
MetricRegistry and pass a shared_ptr<MetricRegistry> instance through vesal::Init(const
vesal::InitOptions&). Otherwise, vesal will do no op by default when collecting metrics.
*/
class Gauge {
public:
    virtual ~Gauge() = default;

    virtual void Set(double val) = 0;
};

class Counter {
public:
    virtual ~Counter() = default;

    virtual void Add(int64_t val) = 0;
};

class Histogram {
public:
    virtual ~Histogram() = default;

    virtual void Set(int64_t val) = 0;
};

using Tag = std::pair<std::string, std::string>;
using TagList = std::vector<Tag>;

// MetricRegistry has to be thread safe since registration will happen in multiple threads
// simultaneously
class MetricRegistry {
public:
    virtual ~MetricRegistry() = default;

    virtual std::shared_ptr<Gauge> RegisterGauge(const std::string& name, const TagList& tags) = 0;
    virtual std::shared_ptr<Counter> RegisterCounter(const std::string& name,
                                                     const TagList& tags) = 0;
    virtual std::shared_ptr<Histogram> RegisterHistogram(const std::string& name,
                                                         const TagList& tags) = 0;
};

// This is a no op metric registry, which will be the default behaviour of vesal
class DefaultMetricRegistry : public MetricRegistry {
public:
    std::shared_ptr<Gauge> RegisterGauge(const std::string& name, const TagList& tags) override;
    std::shared_ptr<Counter> RegisterCounter(const std::string& name, const TagList& tags) override;
    std::shared_ptr<Histogram> RegisterHistogram(const std::string& name,
                                                 const TagList& tags) override;
};

/*
Below templates are only used for Bytedance internal users who is already using
byte::embedded_metrics. Usage example:
    // 1. init users' metrics registry
    namespace bm = byte::embedded_metrics;
    auto& registry = bm::MetricsRegistry::Registry();
    registry.Initialize("vesal-app", {}, bm::MetricsOptions{}, nullptr);
    // 2. use vesal::ByteMetricRegistry template
    auto r = std::make_shared<vesal::ByteMetricRegistry<bm::MetricsRegistry, bm::Gauge,
bm::TlsCounter, bm::TlsHistogram, bm::MetricType>>(&registry);
    // 3. pass the registry to vesal, done
    vesal::InitOptions options;
    options.registry = r;
    Status r = vesal::Init(options);
*/
template <typename G> class ByteGauge : public Gauge {
public:
    ByteGauge(std::shared_ptr<G> metric) : metric_(metric) {}
    void Set(double val) override {
        metric_->Set(val);
    }

private:
    std::shared_ptr<G> metric_;
};

template <typename C> class ByteCounter : public Counter {
public:
    ByteCounter(std::shared_ptr<C> metric) : metric_(metric) {}
    void Add(int64_t val) override {
        metric_->Add(val);
    }

private:
    std::shared_ptr<C> metric_;
};

template <typename H> class ByteHistogram : public Histogram {
public:
    ByteHistogram(std::shared_ptr<H> metric) : metric_(metric) {}
    void Set(int64_t val) override {
        metric_->Set(val);
    }

private:
    std::shared_ptr<H> metric_;
};

template <typename R, typename G, typename C, typename H, typename metricType>
class ByteMetricRegistry : public MetricRegistry {
public:
    ByteMetricRegistry(R* registry) : registry_(registry){};
    std::shared_ptr<Gauge> RegisterGauge(const std::string& name, const TagList& tags) override {
        return std::make_shared<ByteGauge<G>>(
            std::dynamic_pointer_cast<G>(registry_->Register(name, tags, metricType::kGuage)));
    }
    std::shared_ptr<Counter> RegisterCounter(const std::string& name,
                                             const TagList& tags) override {
        return std::make_shared<ByteCounter<C>>(
            std::dynamic_pointer_cast<C>(registry_->Register(name, tags, metricType::kTlsCounter)));
    }
    std::shared_ptr<Histogram> RegisterHistogram(const std::string& name,
                                                 const TagList& tags) override {
        return std::make_shared<ByteHistogram<H>>(std::dynamic_pointer_cast<H>(
            registry_->Register(name, tags, metricType::kTlsHistogram)));
    }

private:
    R* registry_;
};

}  // namespace vesal