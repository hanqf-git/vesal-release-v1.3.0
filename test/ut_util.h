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

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

#include "codec/codec_common.h"
#include "codec/qat/qat_codec.h"
#include "codec/qat/qat_codec_engine.h"
#include "common/checksum_impl.h"
#include "common/timestamp.h"
#include "vesal/codec.h"
#include "vesal/vesal.h"

namespace vesal {

inline void SetErrSimIfPossible() {
#ifdef VESAL_ENABLE_ERR_SIM
    FLAGS_vesal_enable_err_sim = true;
#endif
}

inline void UnsetErrSimIfPossible() {
#ifdef VESAL_ENABLE_ERR_SIM
    FLAGS_vesal_enable_err_sim = false;
#endif
}

#define VESAL_INIT_LOGGING_ONLY EXPECT_EQ(InitLogging({}), 0);

#define VESAL_INIT(is_init_codec_qat, is_init_cypher_qat, is_init_dsa) \
    vesal::InitOptions __vesal_opts{};                                 \
    __vesal_opts.codec_init_opt.init_qat = is_init_codec_qat;          \
    __vesal_opts.cypher_init_opt.init_qat = is_init_cypher_qat;        \
    __vesal_opts.data_flow_init_opt.init_dsa = is_init_dsa;            \
    __vesal_opts.mem_pool_init_opt.prealloc_size_mb = 1024;            \
    FLAGS_vesal_log_console_output = true;                             \
    SetErrSimIfPossible();                                             \
    auto __init_r = vesal::Init(__vesal_opts);                         \
    EXPECT_TRUE(__init_r);

#define VESAL_INIT_CODEC_QAT_ONLY VESAL_INIT(true, false, false)

#define VESAL_UNINIT                        \
    UnsetErrSimIfPossible();                \
    FLAGS_vesal_log_console_output = false; \
    vesal::Uninit();

#define RAII_VESAL_INIT(is_init_codec_qat, is_init_cypher_qat, is_init_dsa) \
    auto __init_defer = defer([]() { VESAL_UNINIT; });                      \
    VESAL_INIT(is_init_codec_qat, is_init_cypher_qat, is_init_dsa);

#define RAII_VESAL_INIT_CODEC_QAT_ONLY RAII_VESAL_INIT(true, false, false)

const char* kUtData =
    "Four score and seven years ago our fathers brought forth, upon this continent, a new nation, "
    "conceived in Liberty, and dedicated to the proposition that all men are created equal. Now we "
    "are engaged in a great civil war, testing whether that nation, or any nation so conceived and "
    "so dedicated, can long endure. We are met on a great battle-field of that war. We have come "
    "to dedicate a portion of that field, as a final resting place for those who here gave their "
    "lives that that nation might live. It is altogether fitting and proper that we should do "
    "this. But, in a larger sense, we can not dedicate -- we can not consecrate -- we can not "
    "hallow -- this ground. The brave men, living and dead, who struggled here, have consecrated "
    "it, far above our poor power to add or detract. The world will little note, nor long remember "
    "what we say here, but it can never forget what they did here. It is for us the living, "
    "rather, to be dedicated here to the unfinished work which they who fought here have thus far "
    "so nobly advanced. It is rather for us to be here dedicated to the great task remaining "
    "before us -- that from these honored dead we take increased devotion to that cause for which "
    "they gave the last full measure of devotion -- that we here highly resolve that these dead "
    "shall not have died in vain -- that this nation, under God, shall have a new birth of freedom "
    "-- and that government of the people, by the people, for the people, shall not perish from "
    "the earth.";

struct Req {
    std::vector<unsigned char*> src;
    std::vector<unsigned int> src_len;
    unsigned char* dst;
    unsigned int dst_len;
    CodecDirection direction{CodecDirection::kComp};
    void* ctx{nullptr};

    Req() = default;

    Req(Req&& rhs) {
        src = rhs.src;
        src_len = rhs.src_len;
        dst = rhs.dst;
        dst_len = rhs.dst_len;
        direction = rhs.direction;
        ctx = rhs.ctx;
        rhs.src.clear();
        rhs.src_len.clear();
        rhs.dst = nullptr;
        rhs.dst_len = 0;
        rhs.direction = CodecDirection::kComp;
        rhs.ctx = nullptr;
    }

    Req& operator=(Req&& rhs) {
        src = rhs.src;
        src_len = rhs.src_len;
        dst = rhs.dst;
        dst_len = rhs.dst_len;
        direction = rhs.direction;
        ctx = rhs.ctx;
        rhs.src.clear();
        rhs.src_len.clear();
        rhs.dst = nullptr;
        rhs.dst_len = 0;
        rhs.direction = CodecDirection::kComp;
        rhs.ctx = nullptr;
        return *this;
    }

    Req(const Req& rhs) = delete;
    Req& operator=(const Req& rhs) = delete;

    Req(size_t src_num, size_t each_src_size, size_t dst_size, CodecDirection direction)
        : src(src_num),
          src_len(src_num, each_src_size),
          dst(new unsigned char[dst_size]),
          dst_len(dst_size),
          direction(direction),
          ctx(this) {
        for (size_t i = 0; i < src_num; ++i) {
            src[i] = new unsigned char[each_src_size];
            memset(src[i], 0, each_src_size);
        }
        memset(dst, 0, dst_size);
    }

    Req(size_t src_size, CodecDirection direction) : Req(1, src_size, src_size * 2, direction) {}

    Req(size_t src_size) : Req(1, src_size, src_size * 2, CodecDirection::kComp) {}

    Req(size_t src_num, size_t each_src_size)
        : Req(src_num, each_src_size, each_src_size * 2, CodecDirection::kComp) {}

    void LoadData() {
        size_t total = 0;
        for (size_t i = 0; i < src_len.size(); ++i) {
            for (size_t j = 0; j < src_len[i]; ++j) {
                src[i][j] = kUtData[total++ % strlen(kUtData)];
            }
        }
    }

    std::string DumpSrc() {
        std::string ret;
        for (size_t i = 0; i < src.size(); ++i) {
            ret.append(reinterpret_cast<char*>(src[i]), src_len[i]);
        }
        return ret;
    }

    std::string DumpDst() {
        return std::string(reinterpret_cast<char*>(dst), dst_len);
    }

    size_t GetTotalSrcSize() {
        return std::accumulate(src_len.begin(), src_len.end(), 0);
    }

    uint64_t ExpectTimeCostUs(uint64_t expect_time_cost_4kb_us = 50) {
        auto ret = expect_time_cost_4kb_us * (GetTotalSrcSize() / 4096 + 1);
#ifndef NDEBUG
        ret <<= 2;
#endif
#ifdef __SANITIZE_ADDRESS__
        ret <<= 2;
#endif
        return ret;
    }

    void Reset() {
        for (size_t i = 0; i < src.size(); ++i) {
            delete[] src[i];
        }
        delete[] dst;
        src.clear();
        src_len.clear();
        dst = nullptr;
        ctx = nullptr;
    }

    ~Req() {
        Reset();
    }
};

inline std::vector<std::unique_ptr<Req>> CreateSimpleCompReqs(size_t req_num, size_t src_size) {
    std::vector<std::unique_ptr<Req>> reqs;
    for (size_t i = 0; i < req_num; ++i) {
        reqs.emplace_back(std::make_unique<Req>(src_size, CodecDirection::kComp));
        reqs.back()->LoadData();
    }
    return reqs;
}

inline void TransforDecomp(Req* comp_req, size_t compress_produced, Req* target) {
    *target = Req(1, compress_produced, comp_req->GetTotalSrcSize(), CodecDirection::kDecomp);
    memcpy(target->src[0], comp_req->dst, compress_produced);
}

// Result is not checked. Only check the send and poll OK.
inline CodecResult OnePollingRequest(CodecChannel* chnnl, Req* req, uint64_t wait_time) {
    StatusCode status = StatusCode::kUnknown;
    if (req->direction == CodecDirection::kComp) {
        status = chnnl->CompressSGLAsync(req->src, req->src_len, req->dst, req->dst_len, req->ctx);
    } else {
        status =
            chnnl->DecompressSGLAsync(req->src, req->src_len, req->dst, req->dst_len, req->ctx);
    }
    EXPECT_TRUE(IsOk(status)) << status;
    usleep(wait_time);
    CodecResult res;
    auto poll_num = chnnl->Poll(&res, 1, -1);
    EXPECT_EQ(poll_num, 1);
    return res;
}

// Result is not checked. Only check the send and poll OK.
inline CodecResult OnePollingRequest(CodecChannel* chnnl, Req* req) {
    return OnePollingRequest(chnnl, req, req->ExpectTimeCostUs());
}

inline CodecResult OnePollingRequest(qat::QatCodecEngine* eng, Req* req, uint64_t wait_time) {
    auto submit_r = eng->SubmitAsyncRequest(
        req->direction, req->src, req->src_len, req->dst, req->dst_len, req->ctx);
    EXPECT_TRUE(IsOk(submit_r)) << submit_r;
    usleep(wait_time);
    CodecResult res;
    auto poll_num = eng->Poll(&res, 1, -1);
    EXPECT_EQ(poll_num, 1);
    return res;
}

inline CodecResult OnePollingRequest(qat::QatCodecEngine* eng, Req* req) {
    return OnePollingRequest(eng, req, req->ExpectTimeCostUs());
}

inline void BatchSend(CodecChannel* chnnl, ssize_t req_num, Req* req) {
    for (ssize_t i = 0; i < req_num; ++i) {
        StatusCode status = StatusCode::kUnknown;
        if (req[i].direction == CodecDirection::kComp) {
            status = chnnl->CompressSGLAsync(
                req[i].src, req[i].src_len, req[i].dst, req[i].dst_len, req[i].ctx);
        } else {
            status = chnnl->DecompressSGLAsync(
                req[i].src, req[i].src_len, req[i].dst, req[i].dst_len, req[i].ctx);
        }
        EXPECT_TRUE(IsOk(status)) << status;
    }
}

inline void BatchSend(qat::QatCodecEngine* eng, ssize_t req_num, Req* req) {
    for (ssize_t i = 0; i < req_num; ++i) {
        auto submit_r = eng->SubmitAsyncRequest(
            req[i].direction, req[i].src, req[i].src_len, req[i].dst, req[i].dst_len, req[i].ctx);
        EXPECT_TRUE(IsOk(submit_r)) << submit_r;
    }
}

// Send request and get the result from polling. Function does not check the result. The function
// assumes the req array has at least req_num length.
inline void ParallelCodecRequestPolling(
    qat::QatCodecEngine* eng, ssize_t req_num, Req* req, CodecResult* ress, ssize_t batch = 32) {
    ssize_t sent_num = 0;
    ssize_t got_num = 0;
    while (sent_num < req_num || got_num < req_num) {
        auto batch_num = std::min(batch, req_num - sent_num);
        auto expected_cost_time_us = 0;
        for (ssize_t i = 0; i < batch_num; ++i) {
            StatusCode status = StatusCode::kUnknown;
            status = eng->SubmitAsyncRequest(req[sent_num].direction,
                                             req[sent_num].src,
                                             req[sent_num].src_len,
                                             req[sent_num].dst,
                                             req[sent_num].dst_len,
                                             req[sent_num].ctx);
            EXPECT_TRUE(IsOk(status)) << status;
            expected_cost_time_us += req[sent_num].ExpectTimeCostUs();
            ++sent_num;
        }
        usleep(expected_cost_time_us);
        auto poll_num = eng->Poll(ress + got_num, batch_num, -1);
        EXPECT_EQ(poll_num, batch_num);
        got_num += poll_num;
        if (got_num < batch_num) {
            break;
        }
    }
    EXPECT_EQ(sent_num, req_num) << "Timeout error, sent_num: " << sent_num
                                 << ", req_num: " << req_num;
}

// Send request and get the result from polling. Function does not check the result. The function
// assumes the req array has at least req_num length.
inline void ParallelCodecRequestPolling(
    CodecChannel* chnnl, ssize_t req_num, Req* req, CodecResult* ress, ssize_t batch = 32) {
    ssize_t sent_num = 0;
    ssize_t got_num = 0;
    while (sent_num < req_num || got_num < req_num) {
        auto batch_num = std::min(batch, req_num - sent_num);
        auto expected_cost_time_us = 0;
        for (ssize_t i = 0; i < batch_num; ++i) {
            StatusCode status = StatusCode::kUnknown;
            if (req[sent_num].direction == CodecDirection::kComp) {
                status = chnnl->CompressSGLAsync(req[sent_num].src,
                                                 req[sent_num].src_len,
                                                 req[sent_num].dst,
                                                 req[sent_num].dst_len,
                                                 req[sent_num].ctx);
            } else {
                status = chnnl->DecompressSGLAsync(req[sent_num].src,
                                                   req[sent_num].src_len,
                                                   req[sent_num].dst,
                                                   req[sent_num].dst_len,
                                                   req[sent_num].ctx);
            }
            EXPECT_TRUE(IsOk(status)) << status;
            expected_cost_time_us += req[sent_num].ExpectTimeCostUs();
            ++sent_num;
        }
        usleep(expected_cost_time_us);
        auto poll_num = chnnl->Poll(ress + got_num, batch_num, -1);
        EXPECT_EQ(poll_num, batch_num);
        got_num += poll_num;
        if (got_num < batch_num) {
            break;
        }
    }
    EXPECT_EQ(sent_num, req_num) << "Timeout error, sent_num: " << sent_num
                                 << ", req_num: " << req_num;
}

inline uint32_t ComputeChecksum(const CodecChecksumType& type,
                                const unsigned char* data,
                                size_t len) {
    switch (type) {
    case CodecChecksumType::kCrc32:
        return vesal::ComputeCRC32(kCrc32cInitialValue, reinterpret_cast<const char*>(data), len);
    default:
        VESAL_LOG(CRITICAL) << "Unsupported checksum type: " << static_cast<int>(type);
    }
    return 0;
}

inline void CheckCheckSum(const CodecChecksumType& type, Req* req, const CodecResult& res) {
    if (type == CodecChecksumType::kNone) {
        return;
    }
    auto src = req->DumpSrc();
    auto dst = req->DumpDst();
    if (req->direction == CodecDirection::kComp) {
        auto in =
            ComputeChecksum(type, reinterpret_cast<const unsigned char*>(src.data()), src.size());
        EXPECT_EQ(in, res.in_checksum)
            << "Not equal for compression in checksum, " << in << " vs " << res.in_checksum;
        auto out =
            ComputeChecksum(type, reinterpret_cast<const unsigned char*>(dst.data()), res.produced);
        EXPECT_EQ(out, res.out_checksum)
            << "Not equal for compression out checksum, " << out << " vs " << res.out_checksum;
    } else {
        auto out =
            ComputeChecksum(type, reinterpret_cast<const unsigned char*>(dst.data()), dst.size());
        EXPECT_EQ(out, res.out_checksum)
            << "Not equal for decompression out checksum, " << out << " vs " << res.out_checksum;
        auto in =
            ComputeChecksum(type, reinterpret_cast<const unsigned char*>(src.data()), res.consumed);
        EXPECT_EQ(in, res.in_checksum)
            << "Not equal for decompression in checksum, " << in << " vs " << res.in_checksum;
    }
}

inline void TwoDirResCheck(const CodecResult& res1, const CodecResult& res2) {
    EXPECT_EQ(res1.consumed, res2.produced);
    EXPECT_EQ(res1.produced, res2.consumed);
    EXPECT_EQ(res1.in_checksum, res2.out_checksum);
    EXPECT_EQ(res1.out_checksum, res2.in_checksum);
}

inline int get_numa_node(void* addr) {
    int node = -1;
    if (get_mempolicy(&node, NULL, 0, addr, MPOL_F_ADDR | MPOL_F_NODE) != 0) {
        return -1;
    }
    return node;
}

}  // namespace vesal
