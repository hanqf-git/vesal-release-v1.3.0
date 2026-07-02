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
#include <vector>

#include "vesal/log_setting.h"

// Assuming that interval_num is big enough and interval_size is small enough
// so that we can use the avg of the interval to represent all values falling into that interval
// not thread-safe
class SimpleHistogram {
public:
    explicit SimpleHistogram(double lower_bound = 0,
                             double upper_bound = 10000,
                             uint interval_num = 10000)
        : lower_bound_(lower_bound), upper_bound_(upper_bound), interval_num_(interval_num) {
        Reset();
    }

    void Reset() {
        VESAL_CHECK(upper_bound_ > lower_bound_);
        interval_size_ = (upper_bound_ - lower_bound_) / interval_num_;
        interval_cnt_.clear();
        interval_cnt_.resize(interval_num_);
        interval_avg_.clear();
        interval_avg_.resize(interval_num_);
        cnt_ = 0;
        avg_ = 0;
        maximum_ = std::numeric_limits<double>::min();
        minimum_ = std::numeric_limits<double>::max();
    }

    void Add(double value) {
        if (value <= lower_bound_)
            Add(value, 0);
        else if (value >= upper_bound_)
            Add(value, interval_cnt_.size() - 1);
        else
            Add(value, (value - lower_bound_) / interval_size_);
    }

    void Combine(const SimpleHistogram& rhs) {
        VESAL_CHECK(lower_bound_ == rhs.lower_bound_);
        VESAL_CHECK(upper_bound_ == rhs.upper_bound_);
        VESAL_CHECK(interval_num_ == rhs.interval_num_);
        for (uint i = 0; i < interval_num_; ++i) {
            interval_avg_[i] =
                interval_avg_[i] / (interval_cnt_[i] + rhs.interval_cnt_[i]) * interval_cnt_[i] +
                rhs.interval_avg_[i] / (interval_cnt_[i] + rhs.interval_cnt_[i]) *
                    rhs.interval_cnt_[i];
            interval_cnt_[i] += rhs.interval_cnt_[i];
        }
        avg_ = avg_ / (cnt_ + rhs.cnt_) * cnt_ + rhs.avg_ / (cnt_ + rhs.cnt_) * rhs.cnt_;
        cnt_ += rhs.cnt_;
        maximum_ = maximum_ > rhs.maximum_ ? maximum_ : rhs.maximum_;
        minimum_ = minimum_ < rhs.minimum_ ? minimum_ : rhs.minimum_;
    }

    // The query happens only after all the performance test is done. No efficiency requirement.
    double GetPercentage(double percentage) const {
        VESAL_CHECK(percentage > 0 && percentage < 100);
        uint target = cnt_ / 100.0 * percentage;
        int64_t prefix_sum = 0;
        for (uint i = 0; i < interval_num_; ++i) {
            if (prefix_sum <= target && prefix_sum + interval_cnt_[i] >= target) {
                return interval_avg_[i];
            }
            prefix_sum += interval_cnt_[i];
        }
        VESAL_CHECK(0 && "shouldn't get here");
        return 0;
    }

    double GetAvg() const {
        return avg_;
    }

    double GetMax() const {
        return maximum_;
    }

    double GetMin() const {
        return minimum_;
    }

private:
    void Add(double value, uint index) {
        VESAL_CHECK(index >= 0 && index < interval_cnt_.size());
        ++interval_cnt_[index];
        interval_avg_[index] += (value - interval_avg_[index]) / interval_cnt_[index];
        ++cnt_;
        avg_ += (value - avg_) / cnt_;
        if (value > maximum_)
            maximum_ = value;
        if (value < minimum_)
            minimum_ = value;
    }
    double lower_bound_;
    double upper_bound_;
    uint interval_num_;
    double interval_size_;
    std::vector<int64_t> interval_cnt_;  // cnt of the interval's values
    std::vector<double> interval_avg_;   // avg of the interval's values
    int64_t cnt_;                        // cnt of all values
    double avg_;
    double maximum_;
    double minimum_;
};
