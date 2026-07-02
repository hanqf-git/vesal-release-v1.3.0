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

#include "codec/qat/qat_codec_engine.h"

#include <gtest/gtest-death-test.h>
#include <gtest/gtest.h>
#include <lz4.h>
#include <lz4frame.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "codec/dc_format.h"
#include "codec/qat/qat_codec.h"
#include "common/defer.h"
#include "common/memory_pool_helper.h"
#include "common/object_pool.h"
#include "common/qat/qat_buffer.h"
#include "common/qat/qat_util.h"
#include "common/uds_listener.h"
#include "ut_util.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"

extern "C" {
#include <cpa.h>
#include <dc/cpa_dc.h>
#include <icp_sal_poll.h>
}

namespace vesal {

// TODO(...): combine util functions in UT
bool ValidateResult(unsigned char* data1, unsigned char* data2, int size) {
    return memcmp(data1, data2, size) == 0;
}

class QatCodecEngineSpecialTest : public ::testing::TestWithParam<HaPolicy> {
public:
    void SetUp() override {
        VESAL_INIT_CODEC_QAT_ONLY;
    }
    void TearDown() override {
        VESAL_UNINIT;
    }
};

class QatCodecEngineTest : public ::testing::TestWithParam<std::tuple<HaPolicy, CodecAlgorithm>> {
public:
    void SetUp() override {
        VESAL_INIT_CODEC_QAT_ONLY;
        vesal::CodecChannelOption channel_opts{};
        channel_opts.ha_policy = std::get<0>(GetParam());
        channel_opts.comp_algorithm = std::get<1>(GetParam());
        channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;
        auto qat_engine_result = qat::CreateQatCodecEngine(channel_opts);
        EXPECT_TRUE(qat_engine_result.first.ok()) << qat_engine_result.first;
        qat_engine_ = std::move(qat_engine_result.second);
        qat_chnnl_opts_ = channel_opts;
        channel_opts.engine_type = vesal::CodecEngineType::kSoftware;
        auto sw_channel_result = CodecChannel::CreateCodecChannel(channel_opts);
        EXPECT_TRUE(sw_channel_result.first.ok());
        sw_channel_ = std::move(sw_channel_result.second);
        sw_chnnl_opts_ = channel_opts;
    }

    void TearDown() override {
        auto qat_close_result = qat_engine_->Close();
        EXPECT_TRUE(qat_close_result.ok()) << qat_close_result;
        auto sw_close_result = sw_channel_->Close();
        EXPECT_TRUE(sw_close_result.ok()) << sw_close_result;
        VESAL_UNINIT;
    }

private:
    CodecChannelOption qat_chnnl_opts_;
    CodecChannelOption sw_chnnl_opts_;
    std::unique_ptr<qat::QatCodecEngine> qat_engine_;
    std::unique_ptr<CodecChannel> sw_channel_;
};

void GenerateInput(uint32_t input_len, unsigned char* input) {
    std::string str = "The quick brown fox jumps over the lazy dog";
    std::srand(std::time(nullptr));
    for (uint32_t i = 0; i < input_len - 1; ++i)
        input[i] = str[std::rand() % str.size()];
    input[input_len - 1] = '\0';
}

// Teardown handles the close of the channel.
TEST_P(QatCodecEngineTest, SucceedToClose) {
    Req req(4096);
    auto res = OnePollingRequest(qat_engine_.get(), &req);
    EXPECT_TRUE(IsOk(res.status)) << res.status;
}

TEST_P(QatCodecEngineTest, CompressDstbufTooSmall) {
    const static int length_a = 4096;
    const static int length_b = 32768;
    struct S {
        unsigned char a[length_a];
        unsigned char b[length_b];
    } output;
    uint32_t input_len = length_a * 4;
    uint32_t output_len;
    std::unique_ptr<unsigned char[]> input = std::make_unique<unsigned char[]>(input_len);
    CodecResult compress_result;
    StatusCode compress_async_result;

    GenerateInput(input_len, input.get());

    // Case1: output_len >= 1024 and big enough, buffer is not enough, expect success with memory
    // corruption
    memset(output.a, 0, sizeof(output));
    input_len = length_a * 4;
    output_len = length_a * 8;
    compress_async_result = qat_engine_->SubmitAsyncRequest(CodecDirection::kComp,
                                                            {input.get()},
                                                            {input_len},
                                                            output.a,
                                                            output_len,
                                                            /*void* ctx*/ nullptr);
    EXPECT_TRUE(IsOk(compress_async_result));
    while (true) {
        ssize_t poll_result_n = qat_engine_->Poll(&compress_result, 1, -1);
        EXPECT_NE(poll_result_n, -1);
        if (poll_result_n) {
            break;
        }
    }
    EXPECT_TRUE(IsOk(compress_result.status));
    unsigned char checksum = 0;
    for (uint32_t i = 0; i < output_len - length_a; i++)
        checksum |= output.b[i];
    EXPECT_NE(checksum, 0);  // b got corrupted

    // Case2: output_len >= 1024 and not big enough, however buffer is enough, expect failure with
    // buffer only filled with at most output_len bytes
    memset(output.a, 0, sizeof(output));
    input_len = length_a;
    output_len = length_a / 3;
    compress_async_result = qat_engine_->SubmitAsyncRequest(CodecDirection::kComp,
                                                            {input.get()},
                                                            {input_len},
                                                            output.a,
                                                            output_len,
                                                            /*void* ctx*/ nullptr);
    EXPECT_TRUE(IsOk(compress_async_result));
    while (true) {
        ssize_t poll_result_n = qat_engine_->Poll(&compress_result, 1, -1);
        EXPECT_NE(poll_result_n, -1);
        if (poll_result_n) {
            break;
        }
    }
    EXPECT_TRUE(IsOverflow(compress_result.status));
    // only filled with at most output_len bytes
    for (uint32_t i = output_len; i < length_a; i++)
        EXPECT_EQ(output.a[i], 0);

    // Case3: output_len >= 1024 and not big enough, buffer is not enough, expect failure with
    // memory corruption when svm enabled
    if (qat_engine_->GetQatUnit()->SvmEnabled()) {
        memset(output.a, 0, sizeof(output));
        input_len = length_a * 4;
        output_len = length_a * 2;
        compress_async_result = qat_engine_->SubmitAsyncRequest(CodecDirection::kComp,
                                                                {input.get()},
                                                                {input_len},
                                                                output.a,
                                                                output_len,
                                                                /*void* ctx*/ nullptr);
        EXPECT_TRUE(IsOk(compress_async_result));
        while (true) {
            ssize_t poll_result_n = qat_engine_->Poll(&compress_result, 1, -1);
            EXPECT_NE(poll_result_n, -1);
            if (poll_result_n) {
                break;
            }
        }
        EXPECT_TRUE(IsOverflow(compress_result.status));
        checksum = 0;
        for (uint32_t i = 0; i < output_len - length_a; i++)
            checksum |= output.b[i];
        EXPECT_NE(checksum, 0);  // b got corrupted
    }
}

TEST_P(QatCodecEngineTest, DecompressDstbufTooSmall) {
    const static int length_a = 2048;
    const static int length_b = 32768;
    struct S {
        unsigned char a[length_a];
        unsigned char b[length_b];
    } output;

    // Construct data for decompress
    const static uint32_t original_len = length_a * 2;
    std::unique_ptr<unsigned char[]> original_data =
        std::make_unique<unsigned char[]>(original_len);
    GenerateInput(original_len, original_data.get());
    std::unique_ptr<unsigned char[]> input = std::make_unique<unsigned char[]>(original_len * 2);
    StatusCode compress_async_result = qat_engine_->SubmitAsyncRequest(CodecDirection::kComp,
                                                                       {original_data.get()},
                                                                       {original_len},
                                                                       input.get(),
                                                                       original_len * 2,
                                                                       /*void* ctx*/ nullptr);

    EXPECT_TRUE(IsOk(compress_async_result));
    CodecResult compress_result;
    while (true) {
        ssize_t poll_result_n = qat_engine_->Poll(&compress_result, 1, -1);
        EXPECT_NE(poll_result_n, -1);
        if (poll_result_n) {
            break;
        }
    }
    EXPECT_TRUE(IsOk(compress_result.status));

    // Done compress, start decompress tests
    uint32_t input_len = compress_result.produced;
    uint32_t output_len;
    StatusCode decompress_async_result;
    CodecResult decompress_result;

    // Case1: output_len >= 1024 and big enough, buffer is not enough, expect success with memory
    // corruption
    memset(output.a, 0, sizeof(output));
    output_len = original_len;
    decompress_async_result = qat_engine_->SubmitAsyncRequest(CodecDirection::kDecomp,
                                                              {input.get()},
                                                              {input_len},
                                                              output.a,
                                                              output_len,
                                                              /*void* ctx*/ nullptr);
    EXPECT_TRUE(IsOk(decompress_async_result));
    while (true) {
        ssize_t poll_result_n = qat_engine_->Poll(&decompress_result, 1, -1);
        EXPECT_NE(poll_result_n, -1);
        if (poll_result_n) {
            break;
        }
    }
    EXPECT_TRUE(IsOk(decompress_result.status));
    unsigned char checksum = 0;
    for (uint32_t i = 0; i < output_len - length_a; i++)
        checksum |= output.b[i];
    EXPECT_NE(checksum, 0);  // b got corrupted

    // Case2: output_len >= 1024 and not big enough, however buffer is enough, expect failure with
    // buffer only filled with at most output_len bytes
    memset(output.a, 0, sizeof(output));
    output_len = original_len / 2;
    decompress_async_result = qat_engine_->SubmitAsyncRequest(CodecDirection::kDecomp,
                                                              {input.get()},
                                                              {input_len},
                                                              output.b,
                                                              output_len,
                                                              /*void* ctx*/ nullptr);
    EXPECT_TRUE(IsOk(decompress_async_result));
    while (true) {
        ssize_t poll_result_n = qat_engine_->Poll(&decompress_result, 1, -1);
        EXPECT_NE(poll_result_n, -1);
        if (poll_result_n) {
            break;
        }
    }
    EXPECT_TRUE(IsOverflow(decompress_result.status));
    // only filled with at most output_len bytes
    for (uint32_t i = output_len; i < length_b; i++)
        EXPECT_EQ(output.b[i], 0);

    // Case3: output_len >= 1024 and not big enough, buffer is not enough, expect failure with
    // memory corruption when svm enabled
    if (qat_engine_->GetQatUnit()->SvmEnabled()) {
        memset(output.a, 0, sizeof(output));
        output_len = original_len - 1;
        decompress_async_result = qat_engine_->SubmitAsyncRequest(CodecDirection::kDecomp,
                                                                  {input.get()},
                                                                  {input_len},
                                                                  output.a,
                                                                  output_len,
                                                                  /*void* ctx*/ nullptr);
        EXPECT_TRUE(IsOk(decompress_async_result));
        while (true) {
            ssize_t poll_result_n = qat_engine_->Poll(&decompress_result, 1, -1);
            EXPECT_NE(poll_result_n, -1);
            if (poll_result_n) {
                break;
            }
        }
        EXPECT_TRUE(IsOverflow(decompress_result.status));
        checksum = 0;
        for (uint32_t i = 0; i < output_len - length_a; i++)
            checksum |= output.b[i];
        EXPECT_NE(checksum, 0);  // b got corrupted
    }
}

TEST_P(QatCodecEngineTest, Timeout) {
    // Only error simulation can trigger timeout
#ifdef VESAL_ENABLE_ERR_SIM
    if (!FLAGS_vesal_enable_err_sim) {
        return;
    }
    // Inject timeout error
    auto s = PackQatErrSimUdsMsg(PackQatErrSimUdsFlags(VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL,
                                                       VESAL_QAT_ERR_SIM_FLAGS_OP_INJECT),
                                 -1,
                                 -1,
                                 -1,
                                 QatErrSimType::kResult,
                                 VESAL_QAT_ERR_SIM_TIMEOUT_ERROR,
                                 1000 * 1000 * 5);
    std::string resp;
    EXPECT_TRUE(WriteUdsAndReadResponse(
        GetQatUdsSocketPath(qat::g_vesal_codec_qat_section_name), s, &resp));
    EXPECT_EQ(resp, "OK");
#else
    return;
#endif
    auto ha_policy = std::get<0>(GetParam());
    if (ha_policy == HaPolicy::kHardware) {
        return;
    }
    std::vector<Req> reqs(32);
    for (size_t i = 0; i < 32; ++i) {
        reqs[i] = Req(4096);
        reqs[i].LoadData();
    }
    BatchSend(qat_engine_.get(), 32, &reqs[0]);
    // sleep and let the requests timeout
    usleep(qat_chnnl_opts_.timeout_ms * 1000 + 1000);
    CodecResult res[32];
    ssize_t r_n = qat_engine_->Poll(res, 32, -1);
    EXPECT_EQ(r_n, 32);
    for (ssize_t i = 0; i < r_n; ++i) {
        EXPECT_TRUE(IsTimeout(res[i].status)) << res[i].status;
    }
}

TEST_P(QatCodecEngineTest, ChecksumCorrectness) {
    Req req(4096);
    auto res = OnePollingRequest(qat_engine_.get(), &req);
    EXPECT_TRUE(IsOk(res.status)) << res.status;
    CheckCheckSum(qat_chnnl_opts_.checksum_type, &req, res);
    Req decomp_req;
    TransforDecomp(&req, res.produced, &decomp_req);
    auto decomp_res = OnePollingRequest(qat_engine_.get(), &decomp_req);
    EXPECT_TRUE(IsOk(decomp_res.status)) << decomp_res.status;
    CheckCheckSum(qat_chnnl_opts_.checksum_type, &decomp_req, decomp_res);
}

TEST_P(QatCodecEngineTest, Pressure) {
    const size_t req_num = 1024;
    std::vector<Req> reqs;
    reqs.reserve(req_num);
    for (size_t i = 0; i < req_num; ++i) {
        reqs.emplace_back(4096);
        reqs.back().LoadData();
    }
    CodecResult* ress = new CodecResult[req_num];
    auto __defer = defer([ress]() { delete[] ress; });
    VESAL_LOG(INFO) << "Channel option: " << qat_chnnl_opts_;
    ParallelCodecRequestPolling(qat_engine_.get(), req_num, &reqs[0], &ress[0]);
    for (size_t i = 0; i < req_num; ++i) {
        EXPECT_TRUE(IsOk(ress[i].status)) << ress[i].status;
        CheckCheckSum(qat_chnnl_opts_.checksum_type, &reqs[i], ress[i]);
    }
    std::vector<Req> decomp_reqs(req_num);
    for (size_t i = 0; i < req_num; ++i) {
        // source.push_back(TransforDecomp(&reqs[i], ress[i].produced));
        TransforDecomp(&reqs[i], ress[i].produced, &decomp_reqs[i]);
    }
    ParallelCodecRequestPolling(qat_engine_.get(), req_num, &decomp_reqs[0], &ress[0]);
    for (size_t i = 0; i < req_num; ++i) {
        EXPECT_TRUE(IsOk(ress[i].status)) << ress[i].status;
        CheckCheckSum(qat_chnnl_opts_.checksum_type, &decomp_reqs[i], ress[i]);
    }
}

TEST_P(QatCodecEngineSpecialTest, NoChecksumForNone) {
    vesal::CodecChannelOption channel_opts{};
    // enough timeout for polling
    channel_opts.checksum_type = vesal::CodecChecksumType::kNone;
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_result.first.ok());
    auto none_channel = std::move(channel_result.second);
    auto __defer = defer([&none_channel]() { EXPECT_TRUE(none_channel->Close().ok()); });

    Req req(4096);
    auto res = OnePollingRequest(none_channel.get(), &req);
    EXPECT_TRUE(IsOk(res.status)) << res.status;
    EXPECT_EQ(res.in_checksum, 0);
    EXPECT_EQ(res.out_checksum, 0);
    Req decomp_req;
    TransforDecomp(&req, res.produced, &decomp_req);
    auto decomp_res = OnePollingRequest(none_channel.get(), &decomp_req);
    EXPECT_TRUE(IsOk(decomp_res.status)) << decomp_res.status;
    EXPECT_EQ(decomp_res.in_checksum, 0);
    EXPECT_EQ(decomp_res.out_checksum, 0);
}

TEST_P(QatCodecEngineSpecialTest, TrivialFailure) {
    vesal::CodecChannelOption channel_opts{};
    channel_opts.checksum_type = vesal::CodecChecksumType::kNum;
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    auto __defer = defer([&channel_result]() {
        if (channel_result.second) {
            EXPECT_TRUE(channel_result.second->Close().ok());
        }
    });
    EXPECT_FALSE(channel_result.first.ok());
    auto status = channel_result.first;
    EXPECT_TRUE(IsInvalidArgument(status)) << status;
}

// In vesal if the user assigns too big number for qat in flight number, the
// value will be set to the max value kMaxQatflightNum.
TEST(QatCodecEngineTrivial, TooBigInFlightNum) {
    auto origin_in_qat_size = fLU::FLAGS_vesal_codec_qat_max_in_qat_num;
    RAII_VESAL_INIT_CODEC_QAT_ONLY;
    FLAGS_vesal_codec_qat_max_in_qat_num = qat::g_max_qat_cfg_concurrency + 1;
    vesal::CodecChannelOption channel_opts{};
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_result.first.ok());
    EXPECT_EQ(g_qat_codec->max_in_qat_size_, qat::g_max_qat_cfg_concurrency);
    EXPECT_TRUE(channel_result.second->Close().ok());
    FLAGS_vesal_codec_qat_max_in_qat_num = origin_in_qat_size;
}

// todo(jj): somehow EXPECT_DEATH cannot catch coredump here, the process dies directly...
// Skip this test for now
TEST(QatCodecEngineTrivial, DISABLED_DeathIfOutOfOrder) {
    MemoryPool::GetInstance()->Init();
    qat::QatUserStart();
    vesal::AddressManager::t_tls_memory_info_by_vaddr_.clear();
    CodecInitOptions init_opts;
    init_opts.init_qat = true;
    EXPECT_TRUE(Codec::Init(init_opts));
    vesal::CodecChannelOption channel_opts;
    channel_opts.engine_type = CodecEngineType::kQat;
    channel_opts.comp_algorithm = vesal::CodecAlgorithm::kLz4;
    channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;
    channel_opts.comp_level = vesal::CodecCompLevel::kLevel4;
    // enough timeout for polling
    channel_opts.timeout_ms = 1000;
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_result.first.ok());
    auto channel = std::move(channel_result.second);
    qat::QatCodecEngine* qat_channel = dynamic_cast<qat::QatCodecEngine*>(channel.get());
    using uc = unsigned char;
    using ui = unsigned int;

    ui src_size = 4096;
    ui dst_size = src_size << 1;
    uc* src = new uc[src_size];
    uc* dst = new uc[dst_size];
    auto status = channel->CompressAsync(src, src_size, dst, dst_size, nullptr);
    EXPECT_TRUE(IsOk(status)) << status;
    usleep(500);
    CodecResult res;
    channel->Poll(&res, 1, -1);
    EXPECT_TRUE(IsOk(res.status)) << res.status;
    status = channel->CompressAsync(src, src_size, dst, dst_size, nullptr);
    EXPECT_TRUE(IsOk(status)) << status;
    usleep(500);

    qat_channel->next_recv_req_id_++;
    EXPECT_DEATH(channel->Poll(&res, 1, -1), ".*");

    EXPECT_TRUE(channel->Close().ok());
    EXPECT_TRUE(Codec::Uninit());
    delete[] src;
    delete[] dst;
    qat::QatUserStop();
}

TEST_P(QatCodecEngineSpecialTest, VesalFrameWithoutHeaderFooterCanBeDecompressByLz4BlockApi) {
    vesal::CodecChannelOption channel_opts;
    channel_opts.comp_algorithm = vesal::CodecAlgorithm::kLz4;
    channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;
    channel_opts.comp_level = vesal::CodecCompLevel::kLevel1;
    channel_opts.engine_type = CodecEngineType::kQat;
    auto channel_result = qat::CreateQatCodecEngine(channel_opts);
    EXPECT_TRUE(channel_result.first.ok());
    auto crc32_channel = std::move(channel_result.second);

    size_t src_size = 4096;
    Req req(src_size);
    req.LoadData();
    auto comp_res = OnePollingRequest(crc32_channel.get(), &req);
    EXPECT_TRUE(IsOk(comp_res.status)) << comp_res.status;
    Req decomp_req;
    TransforDecomp(&req, comp_res.produced, &decomp_req);
    auto hd_size = kLz4FrameHeaderSize;
    auto footer_size = kLz4FrameFooterSize;
    size_t block_size = 0;
    memcpy(&block_size, decomp_req.src[0] + hd_size, 4);
    EXPECT_EQ(block_size, decomp_req.src_len[0] - hd_size - footer_size - 4);
    auto* new_src = reinterpret_cast<char*>(decomp_req.src[0] + hd_size + 4);
    auto* new_dst = reinterpret_cast<char*>(decomp_req.dst);
    // 4 is the size of block_size part
    EXPECT_FALSE(
        LZ4F_isError(LZ4_decompress_safe(new_src, new_dst, block_size, decomp_req.dst_len)));
    EXPECT_EQ(memcmp(new_dst, req.src[0], src_size), 0);

    EXPECT_TRUE(crc32_channel->Close().ok());
}

TEST_P(QatCodecEngineSpecialTest, VesalQatLz4FrameCanBeDecompressByLz4FrameApi) {
    vesal::CodecChannelOption channel_opts;
    // enough timeout for polling
    channel_opts.timeout_ms = 1000;
    channel_opts.comp_algorithm = vesal::CodecAlgorithm::kLz4;
    channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;
    channel_opts.comp_level = vesal::CodecCompLevel::kLevel1;
    channel_opts.engine_type = CodecEngineType::kQat;
    auto channel_result = qat::CreateQatCodecEngine(channel_opts);
    EXPECT_TRUE(channel_result.first.ok());
    auto crc32_channel = std::move(channel_result.second);

    Req req(16384);
    req.LoadData();
    auto res = OnePollingRequest(crc32_channel.get(), &req);
    EXPECT_TRUE(IsOk(res.status)) << res.status;
    LZ4F_dctx* dctx;
    LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    auto g1 = defer([&]() { LZ4F_freeDecompressionContext(dctx); });
    auto hd_size = kLz4FrameHeaderSize;
    auto* dst = req.dst;
    auto produced = res.produced;

    EXPECT_EQ(LZ4F_headerSize(dst, produced), hd_size);

    auto src_size = req.GetTotalSrcSize();
    char* decompressed = new char[src_size];
    auto ori_dec = decompressed;

    auto comp_start = dst;
    auto comp_end = comp_start + produced;
    auto r = 1;
    while (comp_start < comp_end && r != 0) {
        size_t frame_dst_size = src_size;
        size_t frame_src_size = comp_end - comp_start;
        r = LZ4F_decompress(
            dctx, decompressed, &frame_dst_size, comp_start, &frame_src_size, nullptr);
        decompressed += frame_dst_size;
        comp_start += frame_src_size;
    }
    EXPECT_LE(comp_start, comp_end);
    EXPECT_EQ(memcmp(ori_dec, req.src[0], src_size), 0);
    EXPECT_TRUE(crc32_channel->Close().ok());
    delete[] ori_dec;
}

TEST(QatCodecEngineFrameTest, SoftwareFrameCanBeDecompressedByVesal) {
    RAII_VESAL_INIT_CODEC_QAT_ONLY;
    Req comp_req(16 * 1024);

    LZ4F_preferences_t preference;
    CodecChannelOption channel_opt;
    InitSoftwarePreferences(channel_opt, &preference);

    size_t ret = LZ4F_compressFrame(
        comp_req.dst, comp_req.dst_len, comp_req.src[0], comp_req.src_len[0], &preference);
    EXPECT_FALSE(LZ4F_isError(ret));
    size_t header_sz = LZ4F_headerSize(comp_req.dst, ret);
    EXPECT_EQ(header_sz, kLz4FrameHeaderSize);

    CodecChannelOption channel_opts;
    channel_opts.checksum_type = CodecChecksumType::kCrc32;
    channel_opts.comp_algorithm = CodecAlgorithm::kLz4;
    channel_opts.engine_type = CodecEngineType::kQat;
    channel_opts.timeout_ms = 100 * 1000;
    auto create_r = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(create_r.first.ok()) << create_r.first;
    auto channel = std::move(create_r.second);
    Req decomp_req;
    TransforDecomp(&comp_req, ret, &decomp_req);
    auto decomp_res = OnePollingRequest(channel.get(), &decomp_req);
    EXPECT_TRUE(IsOk(decomp_res.status)) << decomp_res.status;
    EXPECT_TRUE(ValidateResult(comp_req.src[0], decomp_req.dst, comp_req.GetTotalSrcSize()));
    auto close_r = channel->Close();
    EXPECT_TRUE(close_r.ok()) << close_r;
}

TEST(QatCodecEngineFrameTest, SoftwareAndVesalCanProcudecSameHeaderFooter) {
    RAII_VESAL_INIT_CODEC_QAT_ONLY;
    Req comp_req(16 * 1024);

    LZ4F_preferences_t preference;
    CodecChannelOption channel_opt;
    InitSoftwarePreferences(channel_opt, &preference);

    size_t ret = LZ4F_compressFrame(
        comp_req.dst, comp_req.dst_len, comp_req.src[0], comp_req.src_len[0], &preference);
    EXPECT_FALSE(LZ4F_isError(ret));
    size_t header_sz = LZ4F_headerSize(comp_req.dst, ret);
    EXPECT_EQ(header_sz, kLz4FrameHeaderSize);
    char vesal_header[32];
    auto vesal_hdr_sz = Lz4FrameHeaderGen(vesal_header);
    EXPECT_EQ(vesal_hdr_sz, kLz4FrameHeaderSize);
    EXPECT_EQ(memcmp(comp_req.dst, vesal_header, vesal_hdr_sz), 0);
    auto produced_footer = comp_req.dst + ret - vesal_hdr_sz;
    char vesal_footer[32];
    auto vesal_footer_sz = Lz4FrameFooterGen(vesal_footer);
    EXPECT_EQ(vesal_footer_sz, kLz4FrameFooterSize);
    EXPECT_EQ(memcmp(produced_footer, vesal_footer, vesal_footer_sz), 0);
}

TEST_P(QatCodecEngineSpecialTest, NoCompressedCrcIfNotNeeded) {
    vesal::CodecChannelOption channel_opts{};
    channel_opts.compressed_checksum = false;
    channel_opts.checksum_type = CodecChecksumType::kCrc32;
    channel_opts.comp_algorithm = CodecAlgorithm::kLz4;
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_result.first.ok()) << channel_result.first;
    auto channel = std::move(channel_result.second);
    auto __defer = defer([&]() {
        auto close_r = channel->Close();
        EXPECT_TRUE(close_r.ok());
    });
    Req req(4096);
    req.LoadData();
    auto res = OnePollingRequest(channel.get(), &req);
    EXPECT_EQ(res.status, StatusCode::kOk);
    EXPECT_NE(res.in_checksum, 0);
    EXPECT_EQ(res.out_checksum, 0);
}

TEST_P(QatCodecEngineTest, SwResultCanBeDecompressedByQat) {
    Req req(16384);
    req.LoadData();
    auto compress_result = OnePollingRequest(sw_channel_.get(), &req);
    EXPECT_TRUE(IsOk(compress_result.status)) << compress_result.status;
    Req decomp_req;
    TransforDecomp(&req, compress_result.produced, &decomp_req);
    auto decompress_result = OnePollingRequest(qat_engine_.get(), &decomp_req);
    EXPECT_TRUE(IsOk(decompress_result.status)) << decompress_result.status;
    TwoDirResCheck(compress_result, decompress_result);
    EXPECT_TRUE(ValidateResult(req.src[0], decomp_req.dst, req.GetTotalSrcSize()));
}

TEST_P(QatCodecEngineTest, QatResultCanBeDecompressedBySw) {
    Req req(16384);
    req.LoadData();
    auto compress_result = OnePollingRequest(qat_engine_.get(), &req);
    EXPECT_TRUE(IsOk(compress_result.status)) << compress_result.status;
    Req decomp_req;
    TransforDecomp(&req, compress_result.produced, &decomp_req);
    auto decompress_result = OnePollingRequest(sw_channel_.get(), &decomp_req);
    EXPECT_TRUE(IsOk(decompress_result.status)) << decompress_result.status;
    TwoDirResCheck(compress_result, decompress_result);
    EXPECT_TRUE(ValidateResult(req.src[0], decomp_req.dst, req.GetTotalSrcSize()));
}

TEST(QatCodecEngineMemTest, FallbackCopyIfCrossHugepage) {
    RAII_VESAL_INIT_CODEC_QAT_ONLY;
    const size_t page_size = 2 * 1024 * 1024;
    const size_t page_num = 4;
    auto* pages = AllocHugepageFn(page_size, page_num);
    auto g = defer([&]() { DeallocHugepageFn(pages, page_size * page_num); });
    MemoryInfo info{
        .virtual_addr = pages,
        .physical_addr = nullptr,
        .len = page_size * page_num,
        .page_size = page_size,
    };
    EXPECT_TRUE(MemoryPool::GetInstance()->Register({info}));
    unsigned char* src = reinterpret_cast<unsigned char*>(pages) + page_size - 4096;
    unsigned char* dst = reinterpret_cast<unsigned char*>(pages) + page_size * 2 - 1;
    CodecChannelOption opt{};
    opt.engine_type = CodecEngineType::kQat;
    auto r = qat::CreateQatCodecEngine(opt);
    EXPECT_TRUE(r.first.ok());
    auto ch = std::move(r.second);
    auto comp_r = ch->SubmitAsyncRequest(CodecDirection::kComp, {src}, {8192}, dst, 8192, nullptr);
    EXPECT_EQ(comp_r, StatusCode::kOk);
    opt.engine_type = CodecEngineType::kSoftware;
    auto sw_ch = vesal::CodecChannel::CreateCodecChannel(opt).second;
    usleep(50);
    CodecResult res;
    auto n = ch->Poll(&res, 1, -1);
    EXPECT_EQ(n, 1);
    EXPECT_TRUE(res.status == StatusCode::kOk) << res.status;
    auto* sw_dst = new unsigned char[8192];
    auto sw_r = sw_ch->Decompress(dst, res.produced, sw_dst, 8192);
    EXPECT_EQ(sw_r.status, StatusCode::kOk);
    EXPECT_EQ(memcmp(src, sw_dst, 8192), 0);
    ch->Close();
    sw_ch->Close();
    delete[] sw_dst;
}

INSTANTIATE_TEST_SUITE_P(QatCodecEngineTestByHaPolicy,
                         QatCodecEngineSpecialTest,
                         ::testing::Values(vesal::HaPolicy::kHardware, vesal::HaPolicy::kNone));

INSTANTIATE_TEST_SUITE_P(QatCodecEngineTestByAlgorithmAndHaPolicy,
                         QatCodecEngineTest,
                         ::testing::Combine(::testing::Values(vesal::HaPolicy::kHardware,
                                                              vesal::HaPolicy::kNone),
                                            ::testing::Values(CodecAlgorithm::kLz4,
                                                              CodecAlgorithm::kDeflate,
                                                              CodecAlgorithm::kZlib)));

}  // namespace vesal
