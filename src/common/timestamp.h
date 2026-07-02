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

// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The implementation is tweaked from absl::base.

#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <mutex>
#include <string>

#include "vesal/log_setting.h"

namespace vesal {
namespace {

#if (!defined(__x86_64__) && !defined(__aarch64__))
#error "Unknown Architecture"
#endif

#ifdef __x86_64__
#define ABSL_INTERNAL_UNSCALED_CYCLECLOCK_FREQUENCY_IS_CPU_FREQUENCY
#endif

// On x86-64 and aarch64 ABSL_USE_UNSCALED_CYCLECLOCK is always true.
#define ABSL_USE_UNSCALED_CYCLECLOCK 1

class UnscaledCycleClock {
private:
    UnscaledCycleClock() = delete;

    // Return the value of a cycle counter that counts at a rate that is
    // approximately constant.
    static int64_t Now();

    // Return the how much UnscaledCycleClock::Now() increases per second.
    // This is not necessarily the core CPU clock frequency.
    // It may be the nominal value report by the kernel, rather than a measured
    // value.
    static double Frequency();

    // Allowed users
    friend class CycleClock;
    friend class UnscaledCycleClockWrapperForInitializeFrequency;
};

#ifdef ABSL_INTERNAL_UNSCALED_CYCLECLOCK_FREQUENCY_IS_CPU_FREQUENCY
// Reads a monotonic time source and returns a value in
// nanoseconds. The returned value uses an arbitrary epoch, not the
// Unix epoch.
static int64_t ReadMonotonicClockNanos() {
    struct timespec t;
#ifdef CLOCK_MONOTONIC_RAW
    int rc = clock_gettime(CLOCK_MONOTONIC_RAW, &t);
#else
    int rc = clock_gettime(CLOCK_MONOTONIC, &t);
#endif
    VESAL_CHECK(rc == 0) << "clock_gettime() failed: (" << std::to_string(errno) << ")";
    return int64_t{t.tv_sec} * 1000000000 + t.tv_nsec;
}

class UnscaledCycleClockWrapperForInitializeFrequency {
public:
    static int64_t Now() {
        return UnscaledCycleClock::Now();
    }
};

struct TimeTscPair {
    int64_t time;  // From ReadMonotonicClockNanos().
    int64_t tsc;   // From UnscaledCycleClock::Now().
};

// Returns a pair of values (monotonic kernel time, TSC ticks) that
// approximately correspond to each other.  This is accomplished by
// doing several reads and picking the reading with the lowest
// latency.  This approach is used to minimize the probability that
// our thread was preempted between clock reads.
static TimeTscPair GetTimeTscPair() {
    int64_t best_latency = std::numeric_limits<int64_t>::max();
    TimeTscPair best;
    for (int i = 0; i < 10; ++i) {
        int64_t t0 = ReadMonotonicClockNanos();
        int64_t tsc = UnscaledCycleClockWrapperForInitializeFrequency::Now();
        int64_t t1 = ReadMonotonicClockNanos();
        int64_t latency = t1 - t0;
        if (latency < best_latency) {
            best_latency = latency;
            best.time = t0;
            best.tsc = tsc;
        }
    }
    return best;
}

// Measures and returns the TSC frequency by taking a pair of
// measurements approximately `sleep_nanoseconds` apart.
static double MeasureTscFrequencyWithSleep(int sleep_nanoseconds) {
    auto t0 = GetTimeTscPair();
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = sleep_nanoseconds;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
    auto t1 = GetTimeTscPair();
    double elapsed_ticks = t1.tsc - t0.tsc;
    double elapsed_time = (t1.time - t0.time) * 1e-9;
    return elapsed_ticks / elapsed_time;
}

// Measures and returns the TSC frequency by calling
// MeasureTscFrequencyWithSleep(), doubling the sleep interval until the
// frequency measurement stabilizes.
static double MeasureTscFrequency() {
    double last_measurement = -1.0;
    int sleep_nanoseconds = 1000000;  // 1 millisecond.
    for (int i = 0; i < 8; ++i) {
        double measurement = MeasureTscFrequencyWithSleep(sleep_nanoseconds);
        if (measurement * 0.99 < last_measurement && last_measurement < measurement * 1.01) {
            // Use the current measurement if it is within 1% of the
            // previous measurement.
            return measurement;
        }
        last_measurement = measurement;
        sleep_nanoseconds *= 2;
    }
    return last_measurement;
}

static double GetNominalCPUFrequency() {
    // On these platforms, the TSC frequency is the nominal CPU
    // frequency.  But without having the kernel export it directly
    // though /sys/devices/system/cpu/cpu0/tsc_freq_khz, there is no
    // other way to reliably get the TSC frequency, so we have to
    // measure it ourselves.  Some CPUs abuse cpuinfo_max_freq by
    // exporting "fake" frequencies for implementing new features. For
    // example, Intel's turbo mode is enabled by exposing a p-state
    // value with a higher frequency than that of the real TSC
    // rate. Because of this, we prefer to measure the TSC rate
    // ourselves on i386 and x86-64.
    return MeasureTscFrequency();
}

// A default frequency of 0.0 might be dangerous if it is used in division.
// !NOTE For simplicity, here use std::once_flag + std::call_once instead of absl::once_flag and
// absl::base_internal::LowLevelCallOnce. Because we don't call NominalCPUFrequency before main().
std::once_flag init_nominal_cpu_frequency_once;
double nominal_cpu_frequency = 1.0;

// Original NominalCPUFrequency() from absl may be called before main() and before malloc is
// properly initialized, therefore this must not allocate memory.
// But in our case we don't do this.
double NominalCPUFrequency() {
    std::call_once(init_nominal_cpu_frequency_once,
                   []() { nominal_cpu_frequency = GetNominalCPUFrequency(); });
    return nominal_cpu_frequency;
}

#endif  // ABSL_INTERNAL_UNSCALED_CYCLECLOCK_FREQUENCY_IS_CPU_FREQUENCY

#ifdef __x86_64__
inline int64_t UnscaledCycleClock::Now() {
    uint64_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return static_cast<int64_t>((high << 32) | low);
}

inline double UnscaledCycleClock::Frequency() {
    return NominalCPUFrequency();
}
#elif __aarch64__
// System timer of ARMv8 runs at a different frequency than the CPU's.
// The frequency is fixed, typically in the range 1-50MHz.  It can be
// read at CNTFRQ special register.  We assume the OS has set up
// the virtual timer properly.
int64_t UnscaledCycleClock::Now() {
    int64_t virtual_timer_value;
    asm volatile("mrs %0, cntvct_el0" : "=r"(virtual_timer_value));
    return virtual_timer_value;
}

double UnscaledCycleClock::Frequency() {
    uint64_t aarch64_timer_frequency;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(aarch64_timer_frequency));
    return aarch64_timer_frequency;
}
#endif

class CycleClock {
public:
    // CycleClock::Now()
    //
    // Returns the value of a cycle counter that counts at a rate that is
    // approximately constant.
    static int64_t Now() {
        return UnscaledCycleClock::Now() >> kShift;
    }

    // CycleClock::Frequency()
    //
    // Returns the amount by which `CycleClock::Now()` increases per second. Note
    // that this value may not necessarily match the core CPU clock frequency.
    static double Frequency() {
        return kFrequencyScale * UnscaledCycleClock::Frequency();
    }

private:
#ifdef NDEBUG
#ifdef ABSL_INTERNAL_UNSCALED_CYCLECLOCK_FREQUENCY_IS_CPU_FREQUENCY
    // Not debug mode and the UnscaledCycleClock frequency is the CPU
    // frequency.  Scale the CycleClock to prevent overflow if someone
    // tries to represent the time as cycles since the Unix epoch.
    static constexpr int32_t kShift = 1;
#else
    // Not debug mode and the UnscaledCycleClock isn't operating at the
    // raw CPU frequency. There is no need to do any scaling, so don't
    // needlessly sacrifice precision.
    static constexpr int32_t kShift = 0;
#endif
#else   // NDEBUG
    // In debug mode use a different shift to discourage depending on a
    // particular shift value.
    static constexpr int32_t kShift = 2;
#endif  // NDEBUG
    static constexpr double kFrequencyScale = 1.0 / (1 << kShift);

    CycleClock() = delete;  // no instances
    CycleClock(const CycleClock&) = delete;
    CycleClock& operator=(const CycleClock&) = delete;
};

}  // namespace

// TimeStamp Class is only used for measuring time duration
// NOT associated with real-world time
class TimeStamp {
public:
    // Get current timestamp
    static uint64_t Now() {
        return CycleClock::Now();
    }
    static uint64_t DurationToMs(uint64_t duration) {
        return static_cast<uint64_t>(duration / (FreqGHZ() * 1000000));
    }
    static uint64_t DurationToUs(uint64_t duration) {
        return static_cast<uint64_t>(duration / (FreqGHZ() * 1000));
    }
    static uint64_t DurationToNs(uint64_t duration) {
        return static_cast<uint64_t>(duration / FreqGHZ());
    }
    static uint64_t MsToDuration(uint64_t ms) {
        return static_cast<uint64_t>(ms * FreqGHZ() * 1000000);
    }
    static uint64_t UsToDuration(uint64_t us) {
        return static_cast<uint64_t>(us * FreqGHZ() * 1000);
    }
    static uint64_t NsToDuration(uint64_t us) {
        return static_cast<uint64_t>(us * FreqGHZ());
    }

    // if not on x86, absl::cycleclock will use similar api to tsc(such as arm's CNTVCT_EL0, which
    // is reliable to use) and it will fall back to using chrono when the platform does not have any
    // api like that. Unfortunately, absl::cycleclock does not check whether it is reliable to use
    // rdtsc on x86 platform. So what we want to do is adding the check of x86 platform's
    // characteristic related to rdtsc, which is missed in absl.
    static bool TscNotReliable() {
#if (defined(__x86_64__) && ABSL_USE_UNSCALED_CYCLECLOCK)
        std::function<void(int[4], int)> cpu_id = [](int cpu_info[4], int info_type) {
            __asm__ volatile(
                "cpuid \n\t"
                : "=a"(cpu_info[0]), "=b"(cpu_info[1]), "=c"(cpu_info[2]), "=d"(cpu_info[3])
                : "a"(info_type));
        };
        bool tsc_reliable = false;
        int cpu_info[4] = {-1};
        // EAX=80000000h: Get Highest Extended Function Implemented
        cpu_id(cpu_info, 0x80000000);
        int max_parameter = cpu_info[0];

        if (max_parameter >= static_cast<int>(0x80000007)) {
            // EAX=80000007h: EDX bit 8 indicates support for invariant TSC
            cpu_id(cpu_info, 0x80000007);
            tsc_reliable = (cpu_info[3] & (1U << 8)) != 0;
        }
        return !tsc_reliable;
#else
        return false;
#endif
    }

private:
    static inline double FreqGHZ() {
        static double freq_ghz = CycleClock::Frequency() / 1000000000;
        return freq_ghz;
    }
};

}  // namespace vesal
