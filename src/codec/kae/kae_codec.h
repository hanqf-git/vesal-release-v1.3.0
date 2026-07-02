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

#include <vector>

#include "codec/codec_internal.h"
#include "vesal/codec.h"
#include "vesal/vesal.h"
extern "C" {
#include "kaezip.h"
}

namespace vesal {
namespace kae {

class KaeCodec : public Codec {
public:
    KaeCodec() {
        unsigned int num_devices = 0;
        devices_ = KAEZIP_get_devices(&num_devices);
        for (unsigned int i = 0; i < num_devices; ++i) {
            device_infos_.push_back({(int32_t)i, devices_[i].numa_id});
        }
    }

    std::pair<Status, std::unique_ptr<CodecChannel>> CreateCodecChannel(
        const CodecChannelOption& opts) override;

    std::vector<DeviceInfo> GetDeviceInfos();

private:
    const struct zip_dev* devices_ = nullptr;
    std::vector<DeviceInfo> device_infos_;
};

}  // namespace kae
}  // namespace vesal
