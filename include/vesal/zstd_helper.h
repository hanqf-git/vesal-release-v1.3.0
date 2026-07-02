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

#include "vesal/codec.h"
#include "vesal/status.h"
#include "zstd.h"

namespace vesal {

struct ZSTDHelperOpts {
    CodecEngineType engine_type{CodecEngineType::kQat};
    // Buffer size for the intermediate data
    // in [ZSTD_BLOCKSIZE_MAX_MIN, 2 * ZSTD_BLOCKSIZE_MAX].
    // The libzstd will guarantee the input block size is less than
    // ZSTD_BLOCKSIZE_MAX, therefore the recommended intermediate_data_size
    // is 2 times ZSTD_BLOCKSIZE_MAX.
    // User can change it to a smaller size if it's certain that the
    // input block size is within some smaller limit.
    uint64_t intermediate_data_size = 2 * ZSTD_BLOCKSIZE_MAX;
    // The compression level of producing lz4s
    // TODO(qlma): how does it and ZSTD_c_compressionLevel affect each other?
    enum CodecCompLevel comp_level = CodecCompLevel::kLevel1;
};

// The key function to register an external sequence producer is as follows:
// ZSTD_registerSequenceProducer(
//     zstd_cctx,
//     sequence_producer_state,
//     sequence_producer
// );
// where the 'sequence_producer_state' is a void pointer pointing to producer's
// state and the 'sequence_producer' is a function pointer pointing to the
// external sequence producer's process.
//
// Here the 'QatSequenceProducer' corresponds to 'sequence_producer', and
// 'GetZSTDHelper' along with 'Close' control the lifetime of the ZSTDHelper,
// which manages producer's state

/**
 * @brief The function used to register customized zstd sequence producer.
 * The function type is defined in zstd.h.
 */
#if defined(__cplusplus)
extern "C" {
#endif
size_t QatSequenceProducer(void* zstd_helper,
                           ZSTD_Sequence* zstd_sequence,
                           size_t zstd_sequence_capacity,
                           const void* src,
                           size_t src_size,
                           const void* dict,
                           size_t dict_size,
                           int compression_level,
                           size_t window_size);
#if defined(__cplusplus)
}
#endif

class ZSTDHelper {
public:
    /**
     * @brief Get the zstd helper based on options. Cannot be shared among threads.
     * The helper is necessary for producing zstd sequence using qat hardware.
     *
     * @param opts User specified options.
     *
     * @return OK if success, with the helper created. Otherwise:
     * - RESOURCE_BUSY Not enough hardware resourses to get a new zstd helper
     * - INVALID_ARGUMENT Wrong opts given
     * - UNKNOWN Internal system error
     */
    static std::pair<Status, std::unique_ptr<ZSTDHelper>> GetZSTDHelper(const ZSTDHelperOpts& opts);

    /**
     * @brief Close the helper.
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Still requests in flight
     * - HARDWARE_ERROR QAT hardware error
     * - UNKNOWN Internal system error
     */
    virtual Status Close() = 0;

    virtual ~ZSTDHelper() = default;
};

}  // namespace vesal
