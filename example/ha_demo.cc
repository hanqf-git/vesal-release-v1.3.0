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

#include <unistd.h>

#include <cstring>

#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    vesal::InitOptions init_option;
    init_option.codec_init_opt.init_qat = true;
    VESAL_CHECK(vesal::Init(init_option)) << "Init vesal failed";

    // Create channel with ha policy enabled.
    vesal::CodecChannelOption chan_option;
    chan_option.ha_policy = vesal::HaPolicy::kHardware;
    auto res = vesal::CodecChannel::CreateCodecChannel(chan_option);
    VESAL_CHECK(res.first.ok()) << "Create codec channel failed: " << res.first;
    std::unique_ptr<vesal::CodecChannel> channel = std::move(res.second);

    size_t src_len = 4096;
    size_t dst_len = src_len << 1;
    unsigned char src[src_len];
    unsigned char dst[dst_len];
    memset(src, 'a', src_len);
    vesal::CodecResult result;
    auto compress = channel->CompressAsync(src, src_len, dst, dst_len, &result);
    while (!IsOk(compress)) {
        if (vesal::IsPermanentError(compress)) {
            VESAL_LOG(ERROR) << "Compress failed: " << compress
                             << ", channel no longer usable even after HA.";
            exit(-1);
        }
        VESAL_LOG(INFO) << "Retry compression.";
        compress = channel->CompressAsync(src, src_len, dst, dst_len, &result);
    }
    auto poll = channel->Poll(&result, 1, -1);
    while (poll == 0) {
        poll = channel->Poll(&result, 1, -1);
    }
    if (poll < 0) {
        VESAL_LOG(ERROR) << "Poll returned -1, means channel no longer usable even after HA.";
        exit(-1);
    }
    if (vesal::IsPermanentError(result.status)) {
        VESAL_LOG(ERROR) << "Compress failed: " << result.status
                         << ", channel no longer usable even after HA.";
        exit(-1);
    }
    VESAL_LOG(INFO) << "HA demo success.";
    channel->Close();
    vesal::Uninit();
    return 0;
}
