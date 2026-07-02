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

#include <memory>

#include "vesal/codec.h"

namespace vesal {

namespace qat {
class QatCodec;
}
namespace sw {
class SwCodec;
}
namespace kae {
class KaeCodec;
}

class Codec {
public:
    // Init g_qat_codec and g_sw_codec
    static bool Init(const CodecInitOptions& init_opts);

    // Reset g_qat_codec and g_sw_codec
    static bool Uninit();

    Codec() = default;
    virtual ~Codec() = default;

    /**
     * @brief Create the channel based on options. Channels are thread-exclusive. Sharing one
     * channel between threads might cause undefined behaviours. Note: It's OK to have one thread
     * holding multiple CodecChannel.
     *
     * @param opts User spcified options.
     *
     * @return OK if success, with CodecChannel created. Otherwise:
     * - RESOURCE_BUSY Not enough hardware resourses to create a new channel
     * - INVALID_ARGUMENT Wrong opts given
     * - UNKNOWN Internal system error
     */
    virtual std::pair<Status, std::unique_ptr<CodecChannel>> CreateCodecChannel(
        const CodecChannelOption& opts) = 0;
};

extern std::unique_ptr<qat::QatCodec> g_qat_codec;
extern std::unique_ptr<sw::SwCodec> g_sw_codec;
extern std::unique_ptr<kae::KaeCodec> g_kae_codec;

}  // namespace vesal
