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

#include "codec/qat/qat_codec_dedicated_channel.h"

#include "gtest/gtest.h"
#include "ut_util.h"
#include "vesal/codec.h"

namespace vesal {

class QatCodecDedicatedChannelTest
    : public ::testing::TestWithParam<std::tuple<HaPolicy, CodecAlgorithm>> {
public:
    void SetUp() override {
        VESAL_INIT_CODEC_QAT_ONLY;
        vesal::CodecChannelOption channel_opts{};
        channel_opts.mode = ChannelMode::kDedicated;
        channel_opts.ha_policy = std::get<0>(GetParam());
        channel_opts.comp_algorithm = std::get<1>(GetParam());
        channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;
        auto qat_channel_result = g_qat_codec->CreateCodecChannel(channel_opts);
        EXPECT_TRUE(qat_channel_result.first.ok()) << qat_channel_result.first;
        qat_channel_ = std::move(qat_channel_result.second);
        qat_chnnl_opts_ = channel_opts;
        channel_opts.engine_type = vesal::CodecEngineType::kSoftware;
        auto sw_channel_result = CodecChannel::CreateCodecChannel(channel_opts);
        EXPECT_TRUE(sw_channel_result.first.ok());
        sw_channel_ = std::move(sw_channel_result.second);
        sw_chnnl_opts_ = channel_opts;
    }

    void TearDown() override {
        auto qat_close_result = qat_channel_->Close();
        EXPECT_TRUE(qat_close_result.ok()) << qat_close_result;
        auto sw_close_result = sw_channel_->Close();
        EXPECT_TRUE(sw_close_result.ok()) << sw_close_result;
        VESAL_UNINIT;
    }

private:
    CodecChannelOption qat_chnnl_opts_;
    CodecChannelOption sw_chnnl_opts_;
    std::unique_ptr<CodecChannel> qat_channel_;
    std::unique_ptr<CodecChannel> sw_channel_;
};

TEST_P(QatCodecDedicatedChannelTest, CompressAndDecompressCorrectly) {
    Req comp_req(16384);
    auto comp_res = OnePollingRequest(qat_channel_.get(), &comp_req);
    EXPECT_TRUE(IsOk(comp_res.status)) << comp_res.status;
    Req decomp_req;
    TransforDecomp(&comp_req, comp_res.produced, &decomp_req);
    auto decomp_res = OnePollingRequest(qat_channel_.get(), &decomp_req);
    EXPECT_TRUE(IsOk(decomp_res.status)) << decomp_res.status;
    TwoDirResCheck(comp_res, decomp_res);
}

TEST_P(QatCodecDedicatedChannelTest, CompressAndCompressSGLProduceSameResults) {
    Req comp_req(16384);
    comp_req.LoadData();
    Req comp_req2(2, 8192);
    comp_req2.LoadData();
    auto comp_res = OnePollingRequest(qat_channel_.get(), &comp_req);
    EXPECT_TRUE(IsOk(comp_res.status)) << comp_res.status;
    auto comp_res2 = OnePollingRequest(qat_channel_.get(), &comp_req2);
    EXPECT_TRUE(IsOk(comp_res2.status)) << comp_res2.status;
    EXPECT_EQ(comp_res.produced, comp_res2.produced);
    EXPECT_EQ(comp_res.consumed, comp_res2.consumed);
    EXPECT_EQ(comp_res.in_checksum, comp_res2.in_checksum);
    EXPECT_EQ(comp_res.out_checksum, comp_res2.out_checksum);
    EXPECT_EQ(memcmp(comp_req.dst, comp_req2.dst, comp_res.produced), 0);
}

TEST_P(QatCodecDedicatedChannelTest, UserCbTest) {
    vesal::CodecChannelOption channel_opts;
    channel_opts.comp_algorithm = vesal::CodecAlgorithm::kLz4;
    channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;
    channel_opts.comp_level = vesal::CodecCompLevel::kLevel1;
    size_t counter = 0;
    channel_opts.user_cb = [](const CodecResult& res) {
        EXPECT_EQ(res.status, StatusCode::kOk);
        size_t* counter = reinterpret_cast<size_t*>(res.ctx);
        *counter += 1;
    };
    channel_opts.engine_type = CodecEngineType::kQat;
    channel_opts.mode = ChannelMode::kDedicated;
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_result.first.ok()) << channel_result.first;
    auto qat_channel = std::move(channel_result.second);
    std::vector<Req> reqs;
    const size_t req_num = 10;
    reqs.reserve(req_num);
    for (size_t i = 0; i < req_num; ++i) {
        reqs.push_back(Req(16384));
        reqs.back().LoadData();
        reqs.back().ctx = reinterpret_cast<void*>(&counter);
    }
    std::vector<CodecResult> results(req_num);
    ParallelCodecRequestPolling(qat_channel.get(), req_num, &reqs[0], &results[0]);
    EXPECT_EQ(counter, 10);
    // Now try sync apis, they don't call user_cb.
    auto r = qat_channel_->CompressSGL(reqs[0].src, reqs[0].src_len, reqs[0].dst, reqs[0].dst_len);
    EXPECT_TRUE(IsOk(r.status));
    EXPECT_EQ(r.status, StatusCode::kOk);
    EXPECT_EQ(counter, 10);
    qat_channel->Close();
}

TEST_P(QatCodecDedicatedChannelTest, HwSyncApi) {
    Req req(16384);
    req.LoadData();
    auto r = qat_channel_->CompressSGL(req.src, req.src_len, req.dst, req.dst_len);
    EXPECT_TRUE(IsOk(r.status));
    Req req_decomp;
    TransforDecomp(&req, r.produced, &req_decomp);
    r = qat_channel_->DecompressSGL(
        req_decomp.src, req_decomp.src_len, req_decomp.dst, req_decomp.dst_len);
    EXPECT_TRUE(IsOk(r.status));
    EXPECT_EQ(memcmp(req.src[0], req_decomp.dst, 16384), 0);
}

TEST_P(QatCodecDedicatedChannelTest, MultiSessionTest) {
    vesal::CodecChannelOption channel_opts;
    channel_opts.comp_algorithm = vesal::CodecAlgorithm::kLz4;
    channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;
    channel_opts.comp_level = vesal::CodecCompLevel::kLevel1;
    channel_opts.engine_type = CodecEngineType::kQat;
    channel_opts.mode = ChannelMode::kDedicated;
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_result.first.ok()) << channel_result.first;
    auto qat_channel = std::move(channel_result.second);

    auto submit_comp =
        [&](CodecChannel* ch, Req* req, CodecAlgorithm algo, CodecCompLevel level) -> CodecResult {
        CodecRequestArgs args{};
        IOBlock src_block[VESAL_MAX_SGL_NUM];
        args.src.blocks = src_block;
        args.src.num_blocks = req->src.size();
        for (size_t i = 0; i < req->src.size(); ++i) {
            src_block[i].data = req->src[i];
            src_block[i].size = req->src_len[i];
            src_block[i].deleter = nullptr;
        }
        args.dst.data = req->dst;
        args.dst.size = req->dst_len;
        args.ctx = nullptr;
        args.direction = CodecDirection::kComp;
        args.session_opt.codec_algorithm = algo;
        args.session_opt.comp_level = level;
        return ch->Submit(args);
    };

    const int SIZE = 4096;
    Req comp_req(SIZE);
    auto r =
        submit_comp(qat_channel.get(), &comp_req, CodecAlgorithm::kLz4, CodecCompLevel::kLevel1);
    EXPECT_TRUE(IsOk(r.status));
    auto checksum = r.out_checksum;
    r = submit_comp(
        qat_channel.get(), &comp_req, CodecAlgorithm::kDeflate, CodecCompLevel::kLevel1);
    EXPECT_TRUE(IsOk(r.status));
    auto checksum2 = r.out_checksum;
    EXPECT_NE(checksum2, checksum);

    r = submit_comp(
        qat_channel.get(), &comp_req, static_cast<CodecAlgorithm>(0), CodecCompLevel::kLevel1);
    EXPECT_TRUE(IsInvalidArgument(r.status));

    channel_opts.comp_algorithm = vesal::CodecAlgorithm::kZlib;
    channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;
    channel_opts.comp_level = vesal::CodecCompLevel::kLevel1;
    channel_opts.engine_type = CodecEngineType::kQat;
    channel_opts.mode = ChannelMode::kDedicated;
    auto channel_result2 = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_result2.first.ok()) << channel_result2.first;
    auto qat_channel2 = std::move(channel_result2.second);

    r = submit_comp(qat_channel.get(), &comp_req, CodecAlgorithm::kZlib, CodecCompLevel::kLevel1);
    EXPECT_TRUE(IsOk(r.status));

    auto r2 =
        submit_comp(qat_channel2.get(), &comp_req, CodecAlgorithm::kZlib, CodecCompLevel::kLevel1);
    EXPECT_TRUE(IsOk(r2.status));
    EXPECT_EQ(r.out_checksum, r2.out_checksum);

    r = submit_comp(qat_channel.get(), &comp_req, CodecAlgorithm::kLz4, CodecCompLevel::kLevel1);
    EXPECT_TRUE(IsOk(r.status));

    r2 = submit_comp(qat_channel2.get(), &comp_req, CodecAlgorithm::kLz4, CodecCompLevel::kLevel1);
    EXPECT_TRUE(IsOk(r2.status));
    EXPECT_EQ(r.out_checksum, r2.out_checksum);

    qat_channel2->Close();
    qat_channel->Close();
}

INSTANTIATE_TEST_SUITE_P(QatCodecDedicatedChannelTestByAlgorithmAndHaPolicy,
                         QatCodecDedicatedChannelTest,
                         ::testing::Combine(::testing::Values(vesal::HaPolicy::kHardware,
                                                              vesal::HaPolicy::kNone),
                                            ::testing::Values(CodecAlgorithm::kLz4,
                                                              CodecAlgorithm::kDeflate,
                                                              CodecAlgorithm::kZlib)));

}  // namespace vesal
