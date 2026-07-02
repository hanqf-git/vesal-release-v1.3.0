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

#pragma once

#include <cstddef>
#include <cstdint>

#include "vesal/types.h"

namespace vesal {

// Arguments for CRC32. Choose them only for the compatibility with ByteLib's CRC32 components.
// This set is very similar to so-called CRC32-C algorithm except here xorout is 0, where the
// CRC32-C uses (uint64_t)(0xFFFFFFFF)<<32
const uint64_t kCrc32cPolynomial = 0x1EDC6F41;
const uint64_t kCrc32cXorOut = 0;
const uint64_t kCrc32cInitialValue = 0xFFFFFFFF;
const uint64_t kCrc32cPolyReflect = 0x82f63b78UL;  // bit reflection of 0x11EDC6F41

uint32_t XxHash(const char* ptr, size_t size);

// The function uses hardware acceleration and requires SSE4 supports.
uint32_t ComputeCRC32(uint32_t init_crc, const char* buf, size_t size);

/**
 * @brief: Suppose we have data1 and data2. data1's CRC32 checksum is crc1 and data2's is crc2. This
 * function can use these 2 crc2 to compute the crc of the combination of data1 and data2. Note this
 * process is of associative and *non-commutative*.
 *
 * @param crc1: crc of the first content
 * @param crc2: crc of the second content
 * @param size2: the size of the second content
 */
uint32_t CRC32Combine(uint32_t crc1, uint32_t crc2, size_t size2);

inline checksum32_t CombineThreeCRC32(checksum32_t init_value,
                                      checksum32_t crc1,
                                      checksum32_t crc2,
                                      checksum32_t crc3,
                                      size_t size2,
                                      size_t size3) {
    return CRC32Combine(init_value ^ CRC32Combine((init_value ^ crc1), crc2, size2), crc3, size3);
}

}  // namespace vesal
