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

#include <lz4frame.h>
#include <netinet/in.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/checksum_impl.h"
#include "vesal/codec.h"
#include "vesal/types.h"
#include "zlib.h"

extern "C" {
#include "cpa_types.h"
}

namespace vesal {

const CpaBoolean kCrc32cReflectIn = CPA_TRUE;
const CpaBoolean kCrc32cReflectOut = CPA_TRUE;

// Max size for buffers to hold header and footer.
const size_t kMaxHeaderSize = 32;
const size_t kMaxFooterSize = 32;

// LZ4 frame has several fields, some are optional and some are not. So users can choose the fields
// they want to enable. In general, vesal keeps the frame simple, while keeping the compatibility
// with QAT. Here we explain the frame format detail that vesal uses:
// General layout:
// |----------header---------|-Blocks...-|--footer-|
// | Magic | FrameDescriptor | Blocks... | EndMark |
// |4 Bytes|     3 Bytes     |    ...    | 4 Bytes |
// The header is a fixed 7 Bytes and footer is a fixed 4 Bytes.
//
// Magic: 4 Bytes, Little endian format. Value : 0x184D2204
//
// The FrameDescriptor vesal uses:
// |   FLG  |   BD   |   HC   |
// | 1 Byte | 1 Byte | 1 Byte |
//
// FLG: Flags, 1 Byte, showing the most properties of the frame. In bits and actual value is:
// |Version|B.Indep|B.Checksum|C.size|C.checksum|Reserved|DictId|
// |  7-6  |   5   |     4    |   3  |     2    |    1   |   0  |
// | 0x01  |  0x1  |    0x0   |  0x0 |    0x0   |   0x0  |  0x0 |
//
// BD: Block Descriptor, 1 Byte, showing the maximum block size of *source data*.
// In bits and actual value is:
// | Reserved | BlockMaxSize | Reserved |
// |    7     |     6-4      |    3-0   |
// |   0x0    |     0x4      |    0x0   |
// BlockMaxSize has 4 valid values: 0x7: 4MB
//                                  0x6: 1MB
//                                  0x5: 256KB
//                                  0x4: 64KB (vesal uses this one)
//
// HC: HeaderChecksum, 1 Byte. The result is the second byte of xxhash32 of the combined of
// FrameDescriptor part, where the seed is 0. I.e. (xxhash32(FrameDescriptor) >> 8) & 0xff
//
// EndMark: 4 Bytes, all zero.
//
// LZ4 frame format is explained in official doc:
// https://github.com/lz4/lz4/blob/dev/doc/lz4_Frame_format.md
const uint32_t kLz4FrameHeaderMagic = 0x184D2204u;
const uint8_t kLz4FrameHeaderVersion = 0x1u;  // Aligned with QAT
const uint8_t kLz4FrameHeaderMaxBlockSize =
    static_cast<uint8_t>(LZ4F_max64KB);  // 64KB, aligned with QAT
const uint8_t kLz4FrameHeaderBlockIndep =
    static_cast<uint8_t>(LZ4F_blockIndependent);  // True, aligned with QAT
const uint8_t kLz4FrameHeaderBlockChecksum =
    static_cast<uint8_t>(LZ4F_noBlockChecksum);       // False, aligned with QAT
const uint8_t kLz4FrameHeaderContentSizeFlag = 0x0u;  // False, aligned with QAT
const uint8_t kLz4FrameHeaderContentChecksum = static_cast<uint8_t>(
    LZ4F_noContentChecksum);  // False, NOT aligned with QAT, as vesal doesn't need this at current
                              // stage. Users shall use checksum ability along with the
                              // comp/decompression.
const uint8_t kLz4FrameHeaderDictPresent = 0x0u;  // False, aligned with QAT

const uint32_t kLz4FrameFooterEndMark = 0x0u;

// See https://datatracker.ietf.org/doc/html/rfc1950 for zlib header format.
const uint8_t kZlibHeaderCmf = 0x78;
const uint8_t kZlibHeaderFlagLow = 0x01;
const uint8_t kZlibHeaderFlagFast = 0x5E;
const uint8_t kZlibHeaderFlagDefault = 0x9C;
const uint8_t kZlibHeaderFlagBest = 0xDA;

inline uint8_t GetZlibHeaderFlag(const CodecCompLevel& level) {
    switch (level) {
    case CodecCompLevel::kLevel1:
    case CodecCompLevel::kLevel2:
        return kZlibHeaderFlagLow;
    case CodecCompLevel::kLevel3:
    case CodecCompLevel::kLevel4:
    case CodecCompLevel::kLevel5:
        return kZlibHeaderFlagFast;
    case CodecCompLevel::kLevel6:
    case CodecCompLevel::kLevel7:
    case CodecCompLevel::kLevel8:
        return kZlibHeaderFlagDefault;
    default:
        break;
    }
    return kZlibHeaderFlagBest;
}

inline int ChannelLvlToZlibLvl(const CodecCompLevel& level) {
    switch (level) {
    case CodecCompLevel::kLevel1:
    case CodecCompLevel::kLevel2:
        return Z_BEST_SPEED;
    case CodecCompLevel::kLevel3:
    case CodecCompLevel::kLevel4:
    case CodecCompLevel::kLevel5:
    case CodecCompLevel::kLevel6:
    case CodecCompLevel::kLevel7:
    case CodecCompLevel::kLevel8:
        return Z_DEFAULT_COMPRESSION;
    default:
        break;
    }
    return Z_BEST_COMPRESSION;
}

#pragma pack(push, 1)
// lz4 frame header
// Note there is no content size, aligned with QAT
struct Lz4FrameHeader {
    uint32_t magic; /* LZ4 magic number */
    uint8_t flag_desc;
    uint8_t block_desc;
    uint8_t hdr_cksum; /* header checksum */
};  // 7 Bytes

/* lz4 frame footer. Note vesal doesn't use content checksum so only end_mark here. */
struct Lz4FrameFooter {
    uint32_t end_mark; /* LZ4 end mark */
};  // 4 Bytes

struct ZlibHeader {
    uint8_t cmf;
    uint8_t flag;
};
struct ZlibFooter {
    checksum32_t adler32;
};
#pragma pack(pop)

// vesal uses fixed args for lz4 frame, hence the sizes are fixed.
const size_t kLz4FrameHeaderSize = sizeof(Lz4FrameHeader);
const size_t kLz4FrameFooterSize = sizeof(Lz4FrameFooter);

const size_t kZlibHeaderSize = sizeof(ZlibHeader);
const size_t kZlibFooterSize = sizeof(ZlibFooter);

#define HEADER_SIZE_CHECK(header_size) \
    static_assert((header_size) <= kMaxHeaderSize, "header size exceeds max size")
#define FOOTER_SIZE_CHECK(footer_size) \
    static_assert((footer_size) <= kMaxFooterSize, "footer size exceeds max size")

HEADER_SIZE_CHECK(kLz4FrameHeaderSize);
FOOTER_SIZE_CHECK(kLz4FrameFooterSize);
HEADER_SIZE_CHECK(kZlibHeaderSize);
FOOTER_SIZE_CHECK(kZlibFooterSize);

// TODO(sjj): Seperate header footer gen function for different algo, as they need different args.
Status FrameHeaderGen(const CodecAlgorithm& algorithm, char* buf, size_t* size);

Status FrameFooterGen(const CodecAlgorithm& algorithm, char* buf, size_t* size);

size_t Lz4FrameHeaderGen(char* buf);

size_t Lz4FrameFooterGen(char* buf);

// Zlib footer's adler32 checksum is calculated at runtime (e.g by QAT).
inline size_t ZlibFooterGen(checksum32_t adler32, char* buf) {
    ZlibFooter footer{htonl(adler32)};
    memcpy(buf, reinterpret_cast<char*>(&footer), sizeof(footer));
    return sizeof(footer);
}

inline size_t ZlibHeaderGen(const CodecCompLevel& level, char* buf) {
    ZlibHeader header{kZlibHeaderCmf, GetZlibHeaderFlag(level)};
    memcpy(buf, reinterpret_cast<char*>(&header), sizeof(header));
    return sizeof(header);
}

void InitSoftwarePreferences(const CodecChannelOption& channel_opt,
                             LZ4F_preferences_t* preferences);

struct Lz4StaticData {
    char header[kMaxHeaderSize];
    char footer[kMaxFooterSize];
    checksum32_t header_crc;
    checksum32_t footer_crc;

    Lz4StaticData() {
        Lz4FrameHeaderGen(header);
        Lz4FrameFooterGen(footer);
        header_crc = ComputeCRC32(kCrc32cInitialValue, header, kLz4FrameHeaderSize);
        footer_crc = ComputeCRC32(kCrc32cInitialValue, footer, kLz4FrameFooterSize);
    }
};

inline const Lz4StaticData& GetLz4StaticData() {
    static Lz4StaticData data;
    return data;
}

inline const char* GetLz4StaticHeader() {
    return GetLz4StaticData().header;
}

inline const char* GetLz4StaticFooter() {
    return GetLz4StaticData().footer;
}

inline checksum32_t GetPreCalcLz4HeaderCrc() {
    return GetLz4StaticData().header_crc;
}

inline checksum32_t GetPreCalcLz4FooterCrc() {
    return GetLz4StaticData().footer_crc;
}

}  // namespace vesal
