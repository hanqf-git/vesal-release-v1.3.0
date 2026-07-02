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

#include "vesal/codec.h"

#include <iostream>
#include <memory>
#include <utility>

#include "codec/codec_internal.h"
#ifdef VESAL_ENABLE_KAE
#include "codec/kae/kae_codec.h"
#endif
#include "codec/qat/qat_codec.h"
#include "codec/sw/sw_codec.h"
#include "vesal/status.h"

namespace vesal {

std::unique_ptr<qat::QatCodec> g_qat_codec = nullptr;
std::unique_ptr<sw::SwCodec> g_sw_codec = nullptr;
#ifdef VESAL_ENABLE_KAE
std::unique_ptr<kae::KaeCodec> g_kae_codec = nullptr;
#endif

bool Codec::Init(const CodecInitOptions& init_opts) {
    g_sw_codec = std::make_unique<sw::SwCodec>();
    if (init_opts.init_qat) {
        auto qat_codec = std::make_unique<qat::QatCodec>();
        Status qat_r = qat_codec->Start();
        if (!qat_r.ok()) {
            VESAL_LOG(WARN) << "Failed to initialize QAT Codec engine: " << qat_r.message();
            return false;
        }
        g_qat_codec = std::move(qat_codec);
    }
    if (init_opts.init_kae) {
#ifdef VESAL_ENABLE_KAE
        g_kae_codec = std::make_unique<kae::KaeCodec>();
#else
        VESAL_LOG(WARN) << "KAE codec support is not compiled in";
#endif
    }
    return true;
}

bool Codec::Uninit() {
    if (g_qat_codec) {
        auto r = g_qat_codec->Stop();
        if (!r.ok()) {
            return false;
        }
        g_qat_codec.reset();
    }
    if (g_sw_codec) {
        g_sw_codec.reset();
    }
#ifdef VESAL_ENABLE_KAE
    if (g_kae_codec) {
        g_kae_codec.reset();
    }
#endif
    return true;
}

std::pair<Status, std::unique_ptr<CodecChannel>> CodecChannel::CreateCodecChannel(
    const CodecChannelOption& opts) {
    switch (opts.engine_type) {
    case CodecEngineType::kQat:
        if (!g_qat_codec) {
            return std::make_pair(NotSupportedError("Qat engine is not initialized"), nullptr);
        }
        return g_qat_codec->CreateCodecChannel(opts);
    case CodecEngineType::kSoftware:
        if (!g_sw_codec) {
            return std::make_pair(NotSupportedError("Software engine is not initialized"), nullptr);
        }
        return g_sw_codec->CreateCodecChannel(opts);
    case CodecEngineType::kKae:
#ifdef VESAL_ENABLE_KAE
        if (!g_kae_codec) {
            return std::make_pair(NotSupportedError("Kae engine is not initialized"), nullptr);
        }
        return g_kae_codec->CreateCodecChannel(opts);
#else
        return std::make_pair(NotSupportedError("Kae engine is not compiled in"), nullptr);
#endif
    default:
        break;
    }
    return std::make_pair(NotSupportedError("Wrong engine type"), nullptr);
}

std::ostream& operator<<(std::ostream& os, const CodecChannelOption& opt) {
    return os << "comp_algorithm=" << static_cast<int>(opt.comp_algorithm)
              << ", comp_level=" << static_cast<int>(opt.comp_level)
              << ", checksum_type=" << static_cast<int>(opt.checksum_type)
              << ", compressed_checksum=" << opt.compressed_checksum
              << ", allocation_option.node_affinity=" << opt.allocation_option.node_affinity
              << ", timeout_ms=" << opt.timeout_ms;
}

bool CodecChannelOption::operator<(const CodecChannelOption& rhs) const {
    return std::tie(user_cb,
                    ha_policy,
                    comp_algorithm,
                    comp_level,
                    checksum_type,
                    compressed_checksum,
                    allocation_option.node_affinity,
                    sw_backup,
                    timeout_ms) < std::tie(rhs.user_cb,
                                           rhs.ha_policy,
                                           rhs.comp_algorithm,
                                           rhs.comp_level,
                                           rhs.checksum_type,
                                           rhs.compressed_checksum,
                                           rhs.allocation_option.node_affinity,
                                           rhs.sw_backup,
                                           rhs.timeout_ms);
}

std::ostream& operator<<(std::ostream& os, const CodecResult& res) {
    bool need_reset_hex = (os.flags() ^ std::ios_base::hex) != 0;
    os << "consumed: " << res.consumed << ", produced: " << res.produced
       << ", in_checksum: " << res.in_checksum << ", out_checksum: " << res.out_checksum
       << ", status: " << res.status << std::hex
       << ", ctx: " << reinterpret_cast<uintptr_t>(res.ctx);
    // reset modified hex flag
    if (need_reset_hex) {
        os.flags(os.flags() ^ std::ios_base::hex);
    }
    return os;
}

}  // namespace vesal