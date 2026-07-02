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

#include "vesal/zstd_helper.h"

#include "codec/zstd_helper/zstd_helper_impl.h"
#include "vesal/status.h"

namespace vesal {

size_t QatSequenceProducer(void* zstd_helper,
                           ZSTD_Sequence* zstd_sequence,
                           size_t zstd_sequence_capacity,
                           const void* src,
                           size_t src_size,
                           const void* dict,
                           size_t dict_size,
                           int compression_level,
                           size_t window_size) {
    auto* helper = reinterpret_cast<ZSTDHelperImpl*>(zstd_helper);
    return helper->ProduceZstdSequence(zstd_sequence,
                                       zstd_sequence_capacity,
                                       src,
                                       src_size,
                                       dict,
                                       dict_size,
                                       compression_level,
                                       window_size);
}

std::pair<Status, std::unique_ptr<ZSTDHelper>> ZSTDHelper::GetZSTDHelper(
    const ZSTDHelperOpts& opts) {
    auto zstd_helper = std::make_unique<ZSTDHelperImpl>(opts);
    auto init_result = zstd_helper->Init();
    if (!init_result.ok()) {
        return {init_result, nullptr};
    }
    return {OkStatus(), std::move(zstd_helper)};
}

}  // namespace vesal
