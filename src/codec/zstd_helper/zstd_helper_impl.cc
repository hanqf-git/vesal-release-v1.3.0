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
#include "zstd_helper_impl.h"

#include <cstdint>

#include "codec/codec_internal.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"

#define MATCH_LENGTH_BITS 4
#define MATCH_LENGTH_MASK ((1U << MATCH_LENGTH_BITS) - 1)
#define RUN_BITS (8 - MATCH_LENGTH_BITS)
#define RUN_MASK ((1U << RUN_BITS) - 1)
// Different mini match would use different LZ4MINMATCH to decode
// lz4s sequence. note that when it is mini match is 4, the LZ4MINMATCH
// should be 3. if mini match is 3, then LZ4MINMATCH should be 2
// we hardcoded mini match = 3 (CPA_DC_MIN_3_BYTE_MATCH)
#define LZ4MINMATCH 2

namespace vesal {

namespace {

void GetLength(unsigned char** ip_ptr, size_t* length_ptr) {
    unsigned s;
    do {
        s = *(*ip_ptr);
        ++(*ip_ptr);
        (*length_ptr) += s;
    } while (s == 255);
}

}  // namespace

// reference:
// QAT-ZSTD-Plugin/src/qatseqprod.c:QZSTD_decLz4s
// https://github.com/intel/QAT-ZSTD-Plugin/blob/e5a134e12d2ea8a5b0f3b83c5b1c325fda4eb0a8/src/qatseqprod.c
size_t ZSTDHelperImpl::DecodeLz4sToZstdSequence(unsigned char* lz4s,
                                                int lz4s_size,
                                                ZSTD_Sequence* zstd_sequence,
                                                size_t zstd_sequence_capacity) {
    unsigned char* ip = lz4s;
    unsigned char* end_ip = lz4s + lz4s_size;
    unsigned int hist_literal_len = 0;
    size_t seqs_idx = 0;

    while (ip < end_ip && lz4s_size > 0) {
        size_t length = 0;
        size_t offset = 0;
        size_t literal_len = 0;
        size_t match_len = 0;
        /* get literal length */
        unsigned const token = *ip++;
        if ((length = (token >> MATCH_LENGTH_BITS)) == RUN_MASK) {
            GetLength(&ip, &length);
        }
        literal_len = length;
        ip += length;
        if (ip == end_ip) {
            // Meet the end of the LZ4 sequence
            /* update ZSTD_Sequence */
            literal_len += hist_literal_len;
            zstd_sequence[seqs_idx].litLength = literal_len;
            zstd_sequence[seqs_idx].offset = offset;
            zstd_sequence[seqs_idx].matchLength = match_len;
            break;
        }

        /* get matchPos */
        offset = read16_function_(ip);
        ip += 2;

        /* get match length */
        length = token & MATCH_LENGTH_MASK;
        if (length == MATCH_LENGTH_MASK) {
            GetLength(&ip, &length);
        }
        if (length != 0) {
            length += LZ4MINMATCH;
            match_len = static_cast<uint16_t>(length);
            literal_len += hist_literal_len;

            /* update ZSTD_Sequence */
            zstd_sequence[seqs_idx].offset = offset;
            zstd_sequence[seqs_idx].litLength = literal_len;
            zstd_sequence[seqs_idx].matchLength = match_len;

            hist_literal_len = 0;
            ++seqs_idx;

            if (VESAL_UNLIKELY(seqs_idx >= zstd_sequence_capacity)) {
                return ZSTD_SEQUENCE_PRODUCER_ERROR;
            }
        } else {
            if (literal_len > 0) {
                /* When match length is 0, the literalLen needs to be temporarily stored
                and processed together with the next data block. If also ip == endip, need
                to convert sequences to seqStore.*/
                hist_literal_len += literal_len;
            }
        }
    }
    if (VESAL_UNLIKELY(ip != end_ip)) {
        return ZSTD_SEQUENCE_PRODUCER_ERROR;
    }
    return seqs_idx + 1;
}

size_t ZSTDHelperImpl::ProduceZstdSequence(ZSTD_Sequence* zstd_sequence,
                                           size_t zstd_sequence_capacity,
                                           const void* src,
                                           size_t src_size,
                                           const void* dict,
                                           size_t dict_size,
                                           int compression_level,
                                           size_t window_size) {
    uint64_t outlen = 2 * src_size;
    if (VESAL_UNLIKELY(outlen > opts_.intermediate_data_size)) {
        VESAL_LOG(ERROR) << "Intermediate data size is too small";
        return ZSTD_SEQUENCE_PRODUCER_ERROR;
    }
    vesal::StatusCode status = channel_->CompressAsync(
        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(src)),
        src_size,
        intermediate_data_,
        outlen,
        nullptr);
    if (VESAL_UNLIKELY(status != StatusCode::kOk)) {
        VESAL_LOG(ERROR) << "Channel CompressAsync Failure, status:" << status;
        return ZSTD_SEQUENCE_PRODUCER_ERROR;
    }
    vesal::CodecResult results;
    int result_num = 0;
    while (result_num == 0) {
        result_num = channel_->Poll(&results, 1, -1);
    }
    if (VESAL_UNLIKELY(result_num != 1 || results.status != StatusCode::kOk)) {
        VESAL_LOG(ERROR) << "Channel result, result_num:" << result_num << ", results:" << results;
        return ZSTD_SEQUENCE_PRODUCER_ERROR;
    }
    size_t zstd_sequence_size = DecodeLz4sToZstdSequence(
        intermediate_data_, results.produced, zstd_sequence, zstd_sequence_capacity);
    if (VESAL_UNLIKELY(ZSTD_SEQUENCE_PRODUCER_ERROR == zstd_sequence_size)) {
        VESAL_LOG(ERROR) << "Failed to decode lz4s to zstd sequence";
    }
    return zstd_sequence_size;
}

Status ZSTDHelperImpl::Init() {
    // ZSTD_BLOCKSIZE_MAX_MIN is the minimum valid max blocksize.
    // It's recommended that intermediate_data_size is at least bigger than it.
    if (opts_.intermediate_data_size < ZSTD_BLOCKSIZE_MAX_MIN) {
        return InvalidArgumentError("Intermediate data size is too small");
    }
    // no need to allocate memory for more than 2 times size of ZSTD_BLOCKSIZE_MAX
    if (opts_.intermediate_data_size > static_cast<uint64_t>(ZSTD_BLOCKSIZE_MAX * 2)) {
        return InvalidArgumentError("Intermediate data size is too big");
    }
    intermediate_data_ = reinterpret_cast<unsigned char*>(
        MemoryPool::GetInstance()->Allocate(opts_.intermediate_data_size));
    if (intermediate_data_ == nullptr) {
        return ResourceBusyError("MemoryPool Allocate failed");
    }
    vesal::CodecChannelOption channel_opts;
    channel_opts.engine_type = opts_.engine_type;
    channel_opts.comp_algorithm = vesal::CodecAlgorithm::kZstd;
    channel_opts.checksum_type = vesal::CodecChecksumType::kNone;
    channel_opts.comp_level = opts_.comp_level;
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    if (!channel_result.first.ok()) {
        MemoryPool::GetInstance()->Deallocate(intermediate_data_);
        intermediate_data_ = nullptr;
        return channel_result.first;
    }
    channel_ = std::move(channel_result.second);

    {
        uint32_t num = 1;
        auto* byte_ptr = reinterpret_cast<uint8_t*>(&num);

        if (*byte_ptr == 1) {  // Little-endian
            read16_function_ = [](const unsigned char* ptr) {
                return *reinterpret_cast<const uint16_t*>(ptr);
            };
        } else {  // Big-endian
            read16_function_ = [](const unsigned char* ptr) {
                return static_cast<uint16_t>(static_cast<uint16_t>(ptr[0]) +
                                             (static_cast<uint16_t>(ptr[1]) << 8));
            };
        }
    }

    return OkStatus();
}

Status ZSTDHelperImpl::Close() {
    if (intermediate_data_ != nullptr) {
        MemoryPool::GetInstance()->Deallocate(intermediate_data_);
        intermediate_data_ = nullptr;
    }
    if (channel_) {
        auto r = channel_->Close();
        if (!r.ok()) {
            return r;
        }
        channel_.reset();
    }
    return OkStatus();
}

ZSTDHelperImpl::ZSTDHelperImpl(const ZSTDHelperOpts& opts)
    : opts_(opts), intermediate_data_(nullptr) {}

}  // namespace vesal