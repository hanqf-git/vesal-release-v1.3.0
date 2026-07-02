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

#include "sw_codec.h"

#include "codec/sw/sw_codec_channel.h"
#include "vesal/status.h"

namespace vesal {
namespace sw {

std::pair<Status, std::unique_ptr<CodecChannel>> SwCodec::CreateCodecChannel(
    const CodecChannelOption& opts) {
    std::pair<Status, std::unique_ptr<CodecChannel>> r{};
    if (opts.mode != ChannelMode::kDedicated) {
        return {NotSupportedError("Shared mode is not supported"), nullptr};
    }
    auto channel = std::make_unique<SwCodecChannel>(opts, sw_result_queue_capacity_);
    r.first = channel->Init();
    if (r.first.ok()) {
        r.second = std::move(channel);
    }
    return r;
}

}  // namespace sw
}  // namespace vesal