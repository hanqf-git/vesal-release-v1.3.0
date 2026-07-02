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

#include "codec/dc_format.h"

#include <cstdint>
#include <cstring>

#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {

constexpr int kChannelLvlToLz4fLvl[] = {0, -10, -8, -6, -4, -2, 0, 2, 4, 6, 8, 10, 12};

size_t Lz4FrameHeaderGen(char* buf) {
    Lz4FrameHeader header;
    header.magic = kLz4FrameHeaderMagic;
    header.flag_desc = static_cast<unsigned char>(
        ((kLz4FrameHeaderVersion & 0x03) << 6) + ((kLz4FrameHeaderBlockIndep & 0x01) << 5) +
        ((kLz4FrameHeaderBlockChecksum & 0x01) << 4) +
        ((kLz4FrameHeaderContentSizeFlag & 0x01) << 3) +
        ((kLz4FrameHeaderContentChecksum & 0x01) << 2) + (kLz4FrameHeaderDictPresent & 0x01));
    // block descriptor
    header.block_desc = static_cast<unsigned char>((kLz4FrameHeaderMaxBlockSize & 0x07) << 4);
    // header checksum
    char* hc_start = reinterpret_cast<char*>(&header) + sizeof(kLz4FrameHeaderMagic);
    size_t hc_size = sizeof(header.flag_desc) + sizeof(header.block_desc);
    header.hdr_cksum = static_cast<uint8_t>(XxHash(hc_start, hc_size) >> 8) & 0xFF;
    memcpy(buf, reinterpret_cast<char*>(&header), sizeof(header));
    return sizeof(header);
}

size_t Lz4FrameFooterGen(char* buf) {
    // Note we don't use content checksum
    VESAL_CHECK(kLz4FrameHeaderContentChecksum == 0);
    Lz4FrameFooter footer;
    footer.end_mark = kLz4FrameFooterEndMark;
    memcpy(buf, reinterpret_cast<char*>(&footer), sizeof(footer));
    return sizeof(footer);
}

Status FrameHeaderGen(const CodecAlgorithm& algorithm, char* buf, size_t* size) {
    size_t generated = 0;
    switch (algorithm) {
    case CodecAlgorithm::kLz4:
        generated = Lz4FrameHeaderGen(buf);
        VESAL_CHECK(generated == kLz4FrameHeaderSize);
        break;
    // currently zstd uses no frame header/footer
    case CodecAlgorithm::kZstd:
    case vesal::CodecAlgorithm::kDeflate:
        generated = 0;
        memset(buf, 0, kMaxHeaderSize);
        break;
    case vesal::CodecAlgorithm::kZlib:
        return NotSupportedError("Should call ZlibHeaderGen directly,");
    default:
        return NotSupportedError("Unknown algorithm: " +
                                 std::to_string(static_cast<int>(algorithm)));
    }
    *size = generated;
    return OkStatus();
}

Status FrameFooterGen(const CodecAlgorithm& algorithm, char* buf, size_t* size) {
    size_t generated = 0;
    switch (algorithm) {
    case CodecAlgorithm::kLz4:
        generated = Lz4FrameFooterGen(buf);
        VESAL_CHECK(generated == kLz4FrameFooterSize);
        break;
    // currently zstd and deflate uses no frame header/footer
    case CodecAlgorithm::kZstd:
    case vesal::CodecAlgorithm::kDeflate:
        generated = 0;
        memset(buf, 0, kMaxFooterSize);
        break;
    case vesal::CodecAlgorithm::kZlib:
        return NotSupportedError("Should call ZlibFooterGen directly,");
    default:
        return NotSupportedError("Only support Lz4 and zstd now");
    }
    *size = generated;
    return OkStatus();
}

inline int ChannelLevelToLZ4FLevel(CodecCompLevel channel_lvl) {
    int channel_lvl_int = static_cast<int>(channel_lvl);
    return kChannelLvlToLz4fLvl[channel_lvl_int];
}

void InitSoftwarePreferences(const CodecChannelOption& channel_opt,
                             LZ4F_preferences_t* preferences) {
    memset(preferences, 0, sizeof(LZ4F_preferences_t));
    LZ4F_frameInfo_t& info = preferences->frameInfo;
    info.blockChecksumFlag = LZ4F_noBlockChecksum;
    info.contentChecksumFlag = LZ4F_noContentChecksum;
    info.blockMode = LZ4F_blockIndependent;
    info.blockSizeID = LZ4F_max64KB;
    info.frameType = LZ4F_skippableFrame;
    info.contentSize = kLz4FrameHeaderContentSizeFlag;
    info.dictID = kLz4FrameHeaderDictPresent;
    preferences->compressionLevel = ChannelLevelToLZ4FLevel(channel_opt.comp_level);
}

}  // namespace vesal
