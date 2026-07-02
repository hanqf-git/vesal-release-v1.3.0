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

#include <cstring>
#include <thread>

#include "codec/qat/qat_codec.h"
#include "codec/qat/qat_poller_manager.h"
#include "common/defer.h"
#include "common/memory_pool_helper.h"
#include "gtest/gtest.h"
#include "ut_util.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/vesal.h"

namespace vesal {
namespace qat {

#define RAII_QAT_INIT_AND_SHUTDONW(console_output)              \
    vesal ::InitOptions __opts;                                 \
    __opts.codec_init_opt.init_qat = true;                      \
    __opts.data_flow_init_opt.init_dsa = false;                 \
    FLAGS_vesal_log_console_output = console_output;            \
    vesal::AddressManager::t_tls_memory_info_by_vaddr_.clear(); \
    auto __r = vesal::Init(__opts);                             \
    EXPECT_TRUE(__r);                                           \
    auto __g = defer([&]() {                                    \
        vesal::Uninit();                                        \
        FLAGS_vesal_log_console_output = false;                 \
    });

TEST(QatCodecEventPollerTest, InitAndClose) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    auto* m = QatPollerManager::GetInstance();
    m->Init();
    std::vector<std::unique_ptr<CodecChannel>> channels;
    for (int i = 0; i < 1; i++) {
        CodecChannelOption opts;
        opts.mode = ChannelMode::kShared;
        auto r = CodecChannel::CreateCodecChannel(opts);
        EXPECT_TRUE(r.first.ok());
        channels.push_back(std::move(r.second));
    }
    for (auto& c : channels) {
        auto r = c->Close();
        EXPECT_TRUE(r.ok());
    }
    m->Reset();
}

TEST(QatCodecEventPollerTest, InitAndCloseMultiThread) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    auto* m = QatPollerManager::GetInstance();
    m->Init();
    auto m_guard = defer([&]() { m->Reset(); });
    const int each_thread_num = 3;
    auto fn = []() {
        std::vector<std::unique_ptr<CodecChannel>> channels;
        for (int i = 0; i < each_thread_num; i++) {
            CodecChannelOption opts;
            opts.mode = ChannelMode::kShared;
            auto r = CodecChannel::CreateCodecChannel(opts);
            EXPECT_TRUE(r.first.ok());
            channels.push_back(std::move(r.second));
        }
        for (auto& c : channels) {
            auto r = c->Close();
            EXPECT_TRUE(r.ok());
        }
    };
    const int thread_num = 1;
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_num; i++) {
        threads.push_back(std::thread(fn));
    }
    for (auto& t : threads) {
        t.join();
    }
}

struct CodecReq {
    CodecReq(unsigned int src_len, unsigned int dst_len)
        : src_len(src_len),
          dst_len(dst_len),
          src(new unsigned char[src_len]),
          dst(new unsigned char[dst_len]) {
        res.status = StatusCode::kUnknown;
    }

    void ShrinkSrcLen(unsigned int src_len) {
        VESAL_CHECK(src_len <= this->src_len);
        this->src_len = src_len;
    }

    unsigned int src_len;
    unsigned int dst_len;
    std::unique_ptr<unsigned char[]> src;
    std::unique_ptr<unsigned char[]> dst;

    CodecResult res;
};

StatusCode Send(CodecChannel* channel,
                CodecReq* req,
                void* ctx,
                CodecDirection dir = CodecDirection::kComp) {
    if (dir == CodecDirection::kComp) {
        return channel->CompressAsync(
            req->src.get(), req->src_len, req->dst.get(), req->dst_len, ctx);
    }
    return channel->DecompressAsync(
        req->src.get(), req->src_len, req->dst.get(), req->dst_len, ctx);
}

TEST(QatCodecEventPollerTest, CompressAsync) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    auto* m = QatPollerManager::GetInstance();
    m->Init();
    auto m_guard = defer([&]() { m->Reset(); });
    CodecChannelOption opts;
    opts.mode = ChannelMode::kShared;
    opts.user_cb = [](const CodecResult& res) {
        auto req = static_cast<CodecReq*>(res.ctx);
        req->res = res;
    };
    auto r = CodecChannel::CreateCodecChannel(opts);
    EXPECT_TRUE(r.first.ok());
    auto channel = std::move(r.second);

    std::vector<std::unique_ptr<CodecReq>> reqs;
    const int req_num = 64;
    for (int i = 0; i < req_num; i++) {
        reqs.push_back(std::make_unique<CodecReq>(4096, 8192));
        EXPECT_EQ(StatusCode::kOk, Send(channel.get(), reqs.back().get(), reqs.back().get()));
    }
    usleep(5000);
    for (auto& req : reqs) {
        EXPECT_EQ(req->res.status, StatusCode::kOk);
        EXPECT_GT(req->res.produced, 0);
    }
    EXPECT_TRUE(channel->Close().ok());
}

TEST(QatCodecEventPollerTest, CompDecompMultiThread) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    auto* m = QatPollerManager::GetInstance();
    m->Init();
    auto m_guard = defer([&]() { m->Reset(); });
    // Prepare decompress data.
    CodecChannelOption option;
    option.user_cb = [](const CodecResult& res) {
        auto req = static_cast<CodecReq*>(res.ctx);
        req->res = res;
    };
    option.mode = ChannelMode::kShared;
    auto r = CodecChannel::CreateCodecChannel(option);
    EXPECT_TRUE(r.first.ok());
    auto channel = std::move(r.second);
    const int src_len = 4096;
    CodecReq req(src_len, src_len * 2);
    EXPECT_EQ(StatusCode::kOk, Send(channel.get(), &req, &req));
    usleep(2000);
    EXPECT_EQ(req.res.status, StatusCode::kOk);
    EXPECT_TRUE(channel->Close().ok());
    unsigned char* compressed_data[src_len * 2];
    unsigned int compressed_len = req.res.produced;
    memcpy(compressed_data, req.dst.get(), compressed_len);
    // Now compressed_data is ready.
    auto fn = [&](bool is_comp) {
        CodecChannelOption option;
        option.mode = ChannelMode::kShared;
        option.user_cb = [](const CodecResult& res) {
            auto req = static_cast<CodecReq*>(res.ctx);
            req->res = res;
        };
        auto r = CodecChannel::CreateCodecChannel(option);
        EXPECT_TRUE(r.first.ok());
        auto channel = std::move(r.second);
        const int req_num = 128;
        std::vector<CodecReq> reqs;
        for (int i = 0; i < req_num; i++) {
            reqs.push_back(CodecReq(src_len, src_len * 2));
            if (!is_comp) {
                reqs.back().ShrinkSrcLen(compressed_len);
                memcpy(reqs.back().src.get(), compressed_data, compressed_len);
            }
        }
        for (auto& req : reqs) {
            EXPECT_EQ(StatusCode::kOk,
                      Send(channel.get(),
                           &req,
                           &req,
                           is_comp ? CodecDirection::kComp : CodecDirection::kDecomp));
        }
        usleep(20 * 1000);
        for (auto& req : reqs) {
            EXPECT_EQ(req.res.status, StatusCode::kOk);
            EXPECT_GT(req.res.produced, 0);
        }
        EXPECT_TRUE(channel->Close().ok());
    };
    const int thread_num = 4;
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_num; i++) {
        threads.push_back(std::thread(fn, i % 2 == 0));
    }
    for (auto& t : threads) {
        t.join();
    }
}

TEST(QatCodecEventPollerTest, UserCbTest) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    auto* m = QatPollerManager::GetInstance();
    m->Init();
    auto m_guard = defer([&]() { m->Reset(); });
    CodecChannelOption opts;
    opts.mode = ChannelMode::kShared;
    size_t counter = 0;
    opts.user_cb = [](const CodecResult& res) {
        size_t* counter_p = reinterpret_cast<size_t*>(res.ctx);
        (*counter_p)++;
    };
    auto r = CodecChannel::CreateCodecChannel(opts);
    EXPECT_TRUE(r.first.ok());
    auto channel = std::move(r.second);
    const int src_len = 4096;
    CodecReq req(src_len, src_len * 2);
    std::vector<CodecReq> reqs;
    for (int i = 0; i < 16; i++) {
        reqs.push_back(CodecReq(src_len, src_len * 2));
    }
    for (auto& req : reqs) {
        EXPECT_EQ(StatusCode::kOk, Send(channel.get(), &req, &counter));
    }
    usleep(2000);
    EXPECT_EQ(counter, 16);
    // Now try sync apis
    for (size_t i = 0; i < reqs.size(); ++i) {
        auto res = channel->Compress(reqs[i].src.get(), reqs[i].src_len, reqs[i].dst.get(), reqs[i].dst_len);
        EXPECT_EQ(res.status, StatusCode::kOk);
    }
    // Sync apis don't call user_cb.
    EXPECT_EQ(counter, 16);
    EXPECT_TRUE(channel->Close().ok());
}

TEST(QatCodecEventPollerTest, SyncApiProduceCorrectResults) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    auto* m = QatPollerManager::GetInstance();
    m->Init();
    auto m_guard = defer([&]() { m->Reset(); });

    // Get shared channel
    CodecChannelOption shared_channel_opts;
    shared_channel_opts.mode = ChannelMode::kShared;
    auto r = CodecChannel::CreateCodecChannel(shared_channel_opts);
    EXPECT_TRUE(r.first.ok());
    auto shared_channel = std::move(r.second);

    // Get dedicated channel
    CodecChannelOption dedicated_channel_opts{};
    dedicated_channel_opts.mode = ChannelMode::kDedicated;
    r = CodecChannel::CreateCodecChannel(dedicated_channel_opts);
    EXPECT_TRUE(r.first.ok());
    auto dedicated_channel = std::move(r.second);

    // Get dedicated channel compress result
    Req dedicated_comp_req(16384);
    auto dedicated_comp_res = OnePollingRequest(dedicated_channel.get(), &dedicated_comp_req);
    EXPECT_TRUE(IsOk(dedicated_comp_res.status)) << dedicated_comp_res.status;
    // Get shared channel compress result
    Req shared_comp_req(16384);
    auto shared_comp_res = shared_channel->CompressSGL(
        shared_comp_req.src, shared_comp_req.src_len, shared_comp_req.dst, shared_comp_req.dst_len);
    EXPECT_TRUE(IsOk(shared_comp_res.status));
    // Compare results
    EXPECT_EQ(dedicated_comp_res.produced, shared_comp_res.produced);
    EXPECT_EQ(0, memcmp(dedicated_comp_req.dst, shared_comp_req.dst, shared_comp_res.produced));

    // Get dedicated channel decompress result
    Req dedicated_decomp_req;
    TransforDecomp(&dedicated_comp_req, dedicated_comp_res.produced, &dedicated_decomp_req);
    auto dedicated_decomp_res = OnePollingRequest(dedicated_channel.get(), &dedicated_decomp_req);
    EXPECT_TRUE(IsOk(dedicated_decomp_res.status)) << dedicated_decomp_res.status;
    // Get shared channel decompress result
    Req shared_decomp_req;
    TransforDecomp(&shared_comp_req, shared_comp_res.produced, &shared_decomp_req);
    auto shared_decomp_res = shared_channel->DecompressSGL(shared_decomp_req.src,
                                                           shared_decomp_req.src_len,
                                                           shared_decomp_req.dst,
                                                           shared_decomp_req.dst_len);
    EXPECT_TRUE(IsOk(shared_decomp_res.status));
    // Compare results
    EXPECT_EQ(dedicated_decomp_res.produced, shared_decomp_res.produced);
    EXPECT_EQ(0,
              memcmp(dedicated_decomp_req.dst, shared_decomp_req.dst, shared_decomp_res.produced));

    TwoDirResCheck(dedicated_comp_res, dedicated_decomp_res);
    TwoDirResCheck(shared_comp_res, shared_decomp_res);

    EXPECT_TRUE(shared_channel->Close().ok());
    EXPECT_TRUE(dedicated_channel->Close().ok());
}

TEST(QatCodecEventPollerTest, SyncCompDecompMultiThread) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    auto* m = QatPollerManager::GetInstance();
    m->Init();
    auto m_guard = defer([&]() { m->Reset(); });

    auto fn = [&]() {
        // Get shared channel
        CodecChannelOption shared_channel_opts;
        shared_channel_opts.mode = ChannelMode::kShared;
        auto r = CodecChannel::CreateCodecChannel(shared_channel_opts);
        EXPECT_TRUE(r.first.ok());
        auto shared_channel = std::move(r.second);

        // Get shared channel compress result
        Req shared_comp_req(16384);
        auto shared_comp_res = shared_channel->CompressSGL(shared_comp_req.src,
                                                           shared_comp_req.src_len,
                                                           shared_comp_req.dst,
                                                           shared_comp_req.dst_len);
        EXPECT_TRUE(IsOk(shared_comp_res.status));

        // Get shared channel decompress result
        Req shared_decomp_req;
        TransforDecomp(&shared_comp_req, shared_comp_res.produced, &shared_decomp_req);
        auto shared_decomp_res = shared_channel->DecompressSGL(shared_decomp_req.src,
                                                               shared_decomp_req.src_len,
                                                               shared_decomp_req.dst,
                                                               shared_decomp_req.dst_len);
        EXPECT_TRUE(IsOk(shared_decomp_res.status));

        TwoDirResCheck(shared_comp_res, shared_decomp_res);
    };

    const int thread_num = 8;
    std::vector<std::thread> threads;
    for (int i = 0; i < thread_num; i++) {
        threads.push_back(std::thread(fn));
    }
    for (auto& t : threads) {
        t.join();
    }
}

}  // namespace qat
}  // namespace vesal
