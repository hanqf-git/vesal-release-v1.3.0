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

#include <string.h>
#include <zstd.h>
#include <zstd_errors.h>

#include <memory>

#include "codec/codec_internal.h"
#include "vesal/codec.h"
#include "vesal/status.h"
#include "vesal/zstd_helper.h"

namespace vesal {

class ZSTDHelperImpl : public ZSTDHelper {
public:
    ZSTDHelperImpl(const ZSTDHelperOpts& opts);

    ~ZSTDHelperImpl() = default;

    Status Init();

    /**
     * zstd_sequence: the output zstd sequence
     * zstd_sequence_capacity: the capacity of zstd sequence, guaranteed to be big enough
     * src: input block
     * src_size: the size of input block, guaranteed to be smaller than limitation
     * dict: not supported
     * dict_size: not supported
     * compression_level: ignored, because it's set in the beginning
     * window_size: ignored, QAT-ZSTD-Plugin doesn't use it either
     *
     * Return the size of the output zstd sequence on success;
     * Return ZSTD_SEQUENCE_PRODUCER_ERROR on failure
     * If ZSTD_c_enableSeqProducerFallback is enabled, it will automatically fall back
     * to software
     */
    size_t ProduceZstdSequence(ZSTD_Sequence* zstd_sequence,
                               size_t zstd_sequence_capacity,
                               const void* src,
                               size_t src_size,
                               const void* dict,
                               size_t dict_size,
                               int compression_level,
                               size_t window_size);

    Status Close() override;

private:
    // get zstd sequence and return its size
    size_t DecodeLz4sToZstdSequence(unsigned char* lz4s,
                                    int lz4s_size,
                                    ZSTD_Sequence* zstd_sequence,
                                    size_t zstd_sequence_capacity);

    ZSTDHelperOpts opts_;

    std::unique_ptr<vesal::CodecChannel> channel_;

    // Store lz4s result
    unsigned char* intermediate_data_;

    // different way to read 16 bits for big/little endian
    unsigned short (*read16_function_)(const unsigned char*);
};

}  // namespace vesal