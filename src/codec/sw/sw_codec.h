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

#include "codec/codec_internal.h"
#include "vesal/codec.h"
#include "vesal/vesal.h"

namespace vesal {
namespace sw {

class SwCodec : public Codec {
public:
    // apply FLAGS_vesal_codec_qat_max_in_qat_num here
    // to use sw engine to mock qat engine
    SwCodec() : sw_result_queue_capacity_(FLAGS_vesal_codec_qat_max_in_qat_num) {}

    std::pair<Status, std::unique_ptr<CodecChannel>> CreateCodecChannel(
        const CodecChannelOption& opts) override;

private:
    size_t sw_result_queue_capacity_;
};

}  // namespace sw
}  // namespace vesal
