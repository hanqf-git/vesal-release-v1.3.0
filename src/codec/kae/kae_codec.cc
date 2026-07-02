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

#include "kae_codec.h"

#include <vector>

#include "kae_codec_channel.h"
#include "vesal/codec.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

namespace vesal {
namespace kae {

std::pair<Status, std::unique_ptr<CodecChannel>> KaeCodec::CreateCodecChannel(
    const CodecChannelOption& opts) {
    std::pair<Status, std::unique_ptr<CodecChannel>> r{};
    if (opts.comp_algorithm != CodecAlgorithm::kDeflate) {
        VESAL_LOG(ERROR) << "Failed to create kae codec channel because unsupported algorithm(only "
                            "deflate is supported)";
        return {InvalidArgumentError("Only deflate is supported"), nullptr};
    }
    if (opts.mode != ChannelMode::kDedicated) {
        VESAL_LOG(ERROR) << "Failed to create kae codec channel because unsupported mode(only "
                            "dedicated is supported)";
        return {NotSupportedError("Only dedicated is supported"), nullptr};
    }

    auto device_config = KAE_CONFIG_AUTO();
    if (opts.allocation_option.device_id >= 0 &&
        opts.allocation_option.device_id < (int)device_infos_.size()) {
        device_config.policy = KAE_SELECT_BY_DEV;
        device_config.param.dev = &devices_[opts.allocation_option.device_id];
    } else if (opts.allocation_option.node_affinity >= 0) {
        device_config.policy = KAE_SELECT_BY_NUMA;
        device_config.param.numa_node = opts.allocation_option.node_affinity;
    }
    auto channel = std::make_unique<KaeCodecChannel>(opts, device_config);
    r.first = channel->Init();
    if (r.first.ok()) {
        r.second = std::move(channel);
    }
    return r;
}

std::vector<DeviceInfo> KaeCodec::GetDeviceInfos() {
    return device_infos_;
}

}  // namespace kae
}  // namespace vesal