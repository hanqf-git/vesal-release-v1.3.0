/*
 * Copyright (c) 2025 ByteDance Inc.
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

#include <gflags/gflags.h>
#include <linux/mman.h>  // MAP_HUGE_2MB
#include <sys/mman.h>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "gflags/gflags_declare.h"
#include "simple_histogram.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

#define PAGE_BIT_NUM_4KB 12
#define PAGE_BIT_NUM_2MB 21
#define PAGE_BIT_NUM_1GB 30
#define VESAL_PAGESIZE(page_bit_num) (1UL << (page_bit_num))

void* AllocHugepageFn(size_t page_size, size_t page_num) {
    VESAL_CHECK(page_size == VESAL_PAGESIZE(PAGE_BIT_NUM_2MB) ||
                page_size == VESAL_PAGESIZE(PAGE_BIT_NUM_1GB));
    uint64_t page_flag =
        page_size == VESAL_PAGESIZE(PAGE_BIT_NUM_2MB) ? MAP_HUGE_2MB : MAP_HUGE_1GB;
    void* addr = (void*)mmap(nullptr,
                             page_size * page_num,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB | page_flag,
                             -1,
                             0);
    if (addr == MAP_FAILED) {
        VESAL_LOG(WARN) << "hugepage mmap failed, page_size=" << page_size
                        << ", page_num=" << page_num << ", errno=" << errno;
        return nullptr;
    }
    VESAL_LOG(DEBUG) << "hugepage allocated, addr=" << (uintptr_t)addr
                     << ", page_size=" << page_size << ", page_num=" << page_num;
    return addr;
}

void DeallocHugepageFn(void* vaddr, size_t size) {
    VESAL_CHECK((size % VESAL_PAGESIZE(PAGE_BIT_NUM_2MB)) == 0);
    VESAL_LOG(DEBUG) << "hugepage freed, vaddr=" << (uintptr_t)vaddr << ", size=" << size;
    munmap(vaddr, size);
}

void EchoAllFlags() {
    std::string strs = gflags::CommandlineFlagsIntoString();
    std::string t;
    for (size_t i = 0, begin = 0; i < strs.size(); ++i) {
        if (strs[i] == '\n') {
            t = strs.substr(begin, i - begin);
            if (!t.empty() && t.back() != '=') {
                VESAL_LOG(INFO) << t;
            }
            begin = i + 1;
            continue;
        }
    }
}
inline void EchoHistogram(const SimpleHistogram& his) {
    VESAL_LOG(INFO) << "AVG:" << his.GetAvg() << "[us],MAX:" << his.GetMax()
                    << "[us],MIN:" << his.GetMin() << "[us],P50:" << his.GetPercentage(50)
                    << "[us],P90:" << his.GetPercentage(90) << "[us],P99:" << his.GetPercentage(99)
                    << "[us],P999:" << his.GetPercentage(99.9) << "[us]";
}

/**
 * A simple CSV writer that supports writing rows with mixed-type data
 */
class SimpleCSVWriter {
public:
    /**
     * Opens a CSV file for writing (overwrites existing file)
     * @param filename Path to output file
     * @return true if file opened successfully
     */
    bool OpenFile(const std::string& filename) {
        file_.open(filename, std::ios::out);
        return file_.is_open();
    }

    /**
     * Writes a single row of comma-separated values to the file
     * @tparam Args Variadic template for mixed data types
     * @param args Values to write (strings, numbers, etc.)
     */
    // Variadic template with a constraint
    template <typename T, typename... Args>
    // This `std::enable_if` prevents this function from being chosen if T is a vector
    typename std::enable_if<!std::is_same<typename std::decay<T>::type, std::vector<std::string>>::value, void>::type
    WriteRow(T&& first, Args&&... args) {
        if (!file_.is_open()) {
            return;
        }
        std::vector<std::string> row;
        _WriteRowHelper(row, std::forward<T>(first), std::forward<Args>(args)...);

        for (size_t i = 0; i < row.size(); ++i) {
            file_ << row[i];
            if (i < row.size() - 1) {
                file_ << ",";
            }
        }
        file_ << "\n";
    }

    void WriteRow(const std::vector<std::string>& row) {
        if (!file_.is_open()){return;}
        // Write values with comma separators
        for (size_t i = 0; i < row.size(); ++i) {
            file_ << row[i];
            if (i < row.size() - 1)
                file_ << ",";
        }
        file_ << "\n";  // End of line
    }

    void WriteRow(const std::vector<const char*>& row) {
        if (!file_.is_open()){return;}
        // Write values with comma separators
        for (size_t i = 0; i < row.size(); ++i) {
            file_ << row[i];
            if (i < row.size() - 1)
                file_ << ",";
        }
        file_ << "\n";  // End of line
    }

    /**
     * Closes the output file explicitly
     */
    void CloseFile() {
        if (file_.is_open())
            file_.close();
    }

    // Auto-close file when writer is destroyed
    ~SimpleCSVWriter() {
        CloseFile();
    }

private:
    std::ofstream file_;  // Output file stream

    // Recursively unpack variadic arguments
    template <typename T, typename... Args>
    void _WriteRowHelper(std::vector<std::string>& row, T&& first, Args&&... args) {
        row.push_back(_ToString(std::forward<T>(first)));
        _WriteRowHelper(row, std::forward<Args>(args)...);
    }

    // Base case for recursion termination
    void _WriteRowHelper(std::vector<std::string>&) {}

    // Convert supported types to string.
    // No rvalue reference here to avoid that std::string as arguments being resolved into this
    // function.
    template <typename T> std::string _ToString(T value) {
        return std::to_string(value);  // Handles numeric types
    }

    // Specialization for std::string
    std::string _ToString(const std::string& value) {
        return value;
    }

    // Specialization for C-style strings
    std::string _ToString(const char* value) {
        return std::string(value);
    }
};
