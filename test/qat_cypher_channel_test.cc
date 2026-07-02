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

#include "cypher/qat_cypher_channel.h"

#include <gtest/gtest-param-test.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <memory>

#include "common/qat/qat_util.h"
#include "cypher/cypher.h"
#include "vesal/cypher.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

namespace vesal {

class QatCypherTestFixture
    : public ::testing::TestWithParam<bool> {  // memory type: copy or zcopy(from memory pool)
protected:
    void SetUp() override {
        vesal::InitOptions init_opt;
        init_opt.cypher_init_opt.init_qat = true;
        init_opt.codec_init_opt.init_qat = false;
        init_opt.data_flow_init_opt.init_dsa = false;
        EXPECT_TRUE(Init(init_opt));
        copy_ = GetParam();
    }
    void TearDown() override {
        EXPECT_TRUE(vesal::Uninit());
    }

    void* Allocate(size_t size) {
        void* addr;
        if (copy_) {
            addr = malloc(size);
        } else {
            addr = MemoryPool::GetInstance()->Allocate(size);
        }
        return addr;
    }

    void Deallocate(void* addr) {
        if (copy_) {
            free(addr);
        } else {
            MemoryPool::GetInstance()->Deallocate(addr);
        }
    }

    bool copy_;
};

TEST_P(QatCypherTestFixture, SGLBasic) {
    CypherChannelOption opt;
    opt.session_option.algorithm = CypherAlgorithm::kAES_XTS;
    opt.session_option.aes_xts_spec.aes_xts_key1 = std::string("0123456789\0abcde", 16);
    opt.session_option.aes_xts_spec.aes_xts_key2 = std::string("0123456789\0ijklm", 16);
    auto p = CypherChannel::CreateCypherChannel(opt);
    EXPECT_TRUE(p.first.ok());
    auto channel = std::move(p.second);
    int sgl_size = 1;
    int len = 4096;
    std::vector<unsigned char*> src_sgl;
    std::vector<unsigned int> src_sgl_len;
    for (int i = 0; i < sgl_size; i++) {
        unsigned char* src = (unsigned char*)Allocate(len);
        src_sgl_len.push_back(len);
        memset(src, 0, len);
        src_sgl.push_back(src);
    }
    unsigned char* tweak = (unsigned char*)Allocate(16);
    unsigned char* dst = (unsigned char*)Allocate(len * sgl_size);
    memset(dst, 0, len * sgl_size);
    memset(tweak, 2, 16);
    CypherReqArgs args;
    args.op = CypherOp::kEncrypt;
    args.aes_xts_tweak = tweak;
    auto r = channel->SubmitCypherSGLReq(src_sgl, src_sgl_len, dst, len * sgl_size, &args);
    EXPECT_TRUE(IsOk(r));
    CypherResult results[100];
    int n = 0;
    while (n < 1) {
        n += channel->Poll(results, 1, -1);
    }
    EXPECT_TRUE(IsOk(results[0].status));
    sgl_size = 8;
    len = 512;
    unsigned char* decrypted_data = (unsigned char*)Allocate(len * sgl_size);
    memset(decrypted_data, 0, len);
    args.op = CypherOp::kDecrypt;
    std::vector<unsigned char*> encrypted_data;
    std::vector<unsigned int> encrypted_data_len;
    for (int i = 0; i < sgl_size; i++) {
        encrypted_data.push_back(dst + i * len);
        encrypted_data_len.push_back(len);
    }
    r = channel->SubmitCypherSGLReq(
        encrypted_data, encrypted_data_len, decrypted_data, len * sgl_size, &args);
    EXPECT_TRUE(IsOk(r));
    n = 0;
    while (n < 1) {
        n += channel->Poll(results, 1, -1);
    }
    EXPECT_TRUE(IsOk(results[0].status));
    int l = len * sgl_size / src_sgl.size();
    for (size_t i = 0; i < src_sgl.size(); i++)
        EXPECT_EQ(memcmp(src_sgl[i], decrypted_data + i * l, l), 0);
    for (size_t i = 0; i < src_sgl.size(); i++) {
        Deallocate(src_sgl[i]);
    }
    Deallocate(dst);
    Deallocate(tweak);
    Deallocate(decrypted_data);
    EXPECT_TRUE(channel->Close().ok());
}

TEST_P(QatCypherTestFixture, TestOrder) {
    CypherChannelOption opt;
    opt.session_option.algorithm = CypherAlgorithm::kAES_XTS;
    opt.session_option.aes_xts_spec.aes_xts_key1 = std::string("0123456789\0abcde", 16);
    opt.session_option.aes_xts_spec.aes_xts_key2 = std::string("0123456789\01ijklm", 16);
    auto p = CypherChannel::CreateCypherChannel(opt);
    EXPECT_TRUE(p.first.ok());
    auto channel = std::move(p.second);

    const int n = 256;
    unsigned int size = 4096;
    std::vector<unsigned char*> src;
    std::vector<unsigned char*> encrypted_data;
    std::vector<unsigned char*> decrypted_data;
    std::vector<unsigned int> src_len;
    for (int i = 0; i < n; i++) {
        src.push_back((unsigned char*)Allocate(size));
        encrypted_data.push_back((unsigned char*)Allocate(size));
        decrypted_data.push_back((unsigned char*)Allocate(size));
        src_len.push_back(size);
        for (unsigned int j = 0; j < size; j++)
            src[i][j] = (i ^ j) % 10 - '0';
    }
    unsigned char* tweak = (unsigned char*)Allocate(16);
    memset(tweak, 2, 16);

    for (int i = 0; i < n; i++) {
        CypherReqArgs args;
        args.op = CypherOp::kEncrypt;
        args.aes_xts_tweak = tweak;
        auto r = channel->SubmitCypherReq(src[i], src_len[i], encrypted_data[i], size, &args);
        EXPECT_TRUE(IsOk(r));
    }

    {
        CypherResult results[n];
        int m = 0;
        while (m < n) {
            m += channel->Poll(results + m, n, -1);
        }
        for (int i = 0; i < n; i++) {
            EXPECT_TRUE(IsOk(results[i].status));
        }
    }

    for (int i = 0; i < n; i++) {
        CypherReqArgs args;
        args.op = CypherOp::kDecrypt;
        args.aes_xts_tweak = tweak;
        auto r = channel->SubmitCypherReq(encrypted_data[i], size, decrypted_data[i], size, &args);
        EXPECT_TRUE(IsOk(r));
    }

    {
        CypherResult results[n];
        int m = 0;
        while (m < n) {
            m += channel->Poll(results + m, n, -1);
        }
        for (int i = 0; i < n; i++) {
            EXPECT_TRUE(IsOk(results[i].status));
        }
        for (int i = 0; i < n; i++) {
            EXPECT_EQ(memcmp(src[i], decrypted_data[i], size), 0);
        }
    }

    for (int64_t i = 0; i < n; i++) {
        bool encrypt = (i / 64) & 1;
        CypherReqArgs args;
        args.ctx = (void*)i;
        args.op = encrypt ? CypherOp::kEncrypt : CypherOp::kDecrypt;
        args.aes_xts_tweak = tweak;
        StatusCode r;
        if (encrypt) {
            r = channel->SubmitCypherReq(decrypted_data[i], size, encrypted_data[i], size, &args);
        } else {
            r = channel->SubmitCypherReq(encrypted_data[i], size, decrypted_data[i], size, &args);
        }
        EXPECT_TRUE(IsOk(r));
    }

    {
        CypherResult results[n];
        int m = 0;
        while (m < n) {
            m += channel->Poll(results + m, n, -1);
        }
        for (int i = 0; i < n; i++) {
            EXPECT_TRUE(IsOk(results[i].status));
            EXPECT_EQ((int64_t)results[i].ctx, i);
        }
        for (int i = 0; i < n; i++) {
            EXPECT_EQ(memcmp(src[i], decrypted_data[i], size), 0);
        }
    }

    for (int i = 0; i < n; i++) {
        Deallocate(src[i]);
        Deallocate(encrypted_data[i]);
        Deallocate(decrypted_data[i]);
    }
    Deallocate(tweak);
    EXPECT_TRUE(channel->Close().ok());
}
// TODO(Pinnong.li): add ut for timeout

TEST_P(QatCypherTestFixture, SWCrossTest) {
    CypherChannelOption opt;
    opt.session_option.algorithm = CypherAlgorithm::kAES_XTS;
    opt.session_option.aes_xts_spec.aes_xts_key1 =
        std::string("01234567890123450123456789\0abcde", 32);
    opt.session_option.aes_xts_spec.aes_xts_key2 =
        std::string("01234567890123450123456789\0ijklm", 32);
    auto p = CypherChannel::CreateCypherChannel(opt);
    EXPECT_TRUE(p.first.ok());
    auto channel = std::move(p.second);

    const int n = 256;
    unsigned int size = 4096;
    std::vector<unsigned char*> src;
    std::vector<unsigned char*> encrypted_data;
    std::vector<unsigned char*> decrypted_data;
    std::vector<unsigned int> src_len;
    for (int i = 0; i < n; i++) {
        src.push_back((unsigned char*)Allocate(size));
        encrypted_data.push_back((unsigned char*)Allocate(size));
        decrypted_data.push_back((unsigned char*)Allocate(size));
        src_len.push_back(size);
        for (unsigned int j = 0; j < size; j++)
            src[i][j] = (i ^ j) % 10 - '0';
    }
    unsigned char* tweak = (unsigned char*)Allocate(16);
    memset(tweak, 2, 16);

    for (int i = 0; i < n; i++) {
        CypherReqArgs args;
        args.op = CypherOp::kEncrypt;
        args.aes_xts_tweak = tweak;
        auto r = channel->SubmitCypherReq(src[i], src_len[i], encrypted_data[i], size, &args);
        EXPECT_TRUE(IsOk(r));
    }

    {
        CypherResult results[n];
        int m = 0;
        while (m < n) {
            m += channel->Poll(results + m, n, -1);
        }
        for (int i = 0; i < n; i++) {
            EXPECT_TRUE(IsOk(results[i].status));
        }
    }

    opt.engine = EngineType::kSoftware;
    p = CypherChannel::CreateCypherChannel(opt);
    EXPECT_TRUE(p.first.ok());
    auto sw_channel = std::move(p.second);

    {
        for (int i = 0; i < n; i++) {
            CypherReqArgs args;
            args.op = CypherOp::kDecrypt;
            args.aes_xts_tweak = tweak;
            auto r = sw_channel->SubmitCypherReq(
                encrypted_data[i], size, decrypted_data[i], size, &args);
            EXPECT_TRUE(IsOk(r));
            EXPECT_EQ(memcmp(src[i], decrypted_data[i], size), 0);
        }
    }

    for (int i = 0; i < n; i++) {
        CypherReqArgs args;
        args.op = CypherOp::kEncrypt;
        args.aes_xts_tweak = tweak;
        auto r =
            sw_channel->SubmitCypherSGLReq({src[i]}, {src_len[i]}, encrypted_data[i], size, &args);
        EXPECT_TRUE(IsOk(r));
    }

    {
        for (int i = 0; i < n; i++) {
            CypherReqArgs args;
            args.op = CypherOp::kDecrypt;
            args.aes_xts_tweak = tweak;
            auto r =
                channel->SubmitCypherReq(encrypted_data[i], size, decrypted_data[i], size, &args);
            EXPECT_TRUE(IsOk(r));
        }

        CypherResult results[n];
        int m = 0;
        while (m < n) {
            m += channel->Poll(results + m, n, -1);
        }
        for (int i = 0; i < n; i++) {
            EXPECT_TRUE(IsOk(results[i].status));
        }
        for (int i = 0; i < n; i++) {
            EXPECT_EQ(memcmp(src[i], decrypted_data[i], size), 0);
        }
    }

    for (int i = 0; i < n; i++) {
        Deallocate(src[i]);
        Deallocate(encrypted_data[i]);
        Deallocate(decrypted_data[i]);
    }
    Deallocate(tweak);
    EXPECT_TRUE(channel->Close().ok());
    EXPECT_TRUE(sw_channel->Close().ok());
}

TEST_P(QatCypherTestFixture, MultiSessionTest) {
    CypherChannelOption opt;
    opt.session_option.algorithm = CypherAlgorithm::kAES_XTS;
    opt.session_option.aes_xts_spec.aes_xts_key1 =
        std::string("01234567890123450123456789\0abcde", 32);
    opt.session_option.aes_xts_spec.aes_xts_key2 =
        std::string("01234567890123450123456789\0ijklm", 32);
    auto p = CypherChannel::CreateCypherChannel(opt);
    EXPECT_TRUE(p.first.ok());
    auto channel = std::move(p.second);

    const int sess_num = 1000;
    std::vector<void*> sessions(sess_num);
    CypherSessionOption sess_opt;
    sess_opt.algorithm = CypherAlgorithm::kAES_XTS;
    for (int i = 0; i < sess_num; i++) {
        sess_opt.aes_xts_spec.aes_xts_key1 =
            std::string("01234567890123450123456789\0zzzz", 31) + std::to_string(i % 10);
        sess_opt.aes_xts_spec.aes_xts_key2 =
            std::string("01234567890123450123456789\0yyyy", 31) + std::to_string(i % 10);
        sessions[i] = channel->AddSession(sess_opt);
    }

    const int n = 1000;
    unsigned int size = 4096;
    std::vector<unsigned char*> src;
    std::vector<unsigned char*> encrypted_data;
    std::vector<unsigned char*> decrypted_data;
    std::vector<unsigned int> src_len;
    for (int i = 0; i < n; i++) {
        src.push_back((unsigned char*)Allocate(size));
        encrypted_data.push_back((unsigned char*)Allocate(size));
        decrypted_data.push_back((unsigned char*)Allocate(size));
        src_len.push_back(size);
        for (unsigned int j = 0; j < size; j++)
            src[i][j] = (i ^ j) % 10 - '0';
    }
    unsigned char* tweak = (unsigned char*)Allocate(16);
    memset(tweak, 2, 16);

    {
        CypherResult results[n];
        for (int i = 0, m = 0; i < n; i++) {
            CypherReqArgs args;
            args.session = sessions[i];
            args.op = CypherOp::kEncrypt;
            args.aes_xts_tweak = tweak;
            auto r = channel->SubmitCypherReq(src[i], src_len[i], encrypted_data[i], size, &args);
            EXPECT_TRUE(IsOk(r));
            if (i - m > 500 || i == n - 1) {
                while (m < i) {
                    m += channel->Poll(results + m, n, -1);
                }
            }
        }
        for (int i = 0; i < n; i++) {
            EXPECT_TRUE(IsOk(results[i].status));
        }
    }
    {
        CypherResult results[n];
        for (int i = 0, m = 0; i < n; i++) {
            CypherReqArgs args;
            args.session = sessions[i];
            args.op = CypherOp::kDecrypt;
            args.aes_xts_tweak = tweak;
            auto r =
                channel->SubmitCypherReq(encrypted_data[i], size, decrypted_data[i], size, &args);
            EXPECT_TRUE(IsOk(r));
            if (i - m > 500 || i == n - 1) {
                while (m < i) {
                    m += channel->Poll(results + m, n, -1);
                }
            }
        }
        for (int i = 0; i < n; i++) {
            EXPECT_TRUE(IsOk(results[i].status));
        }
        for (int i = 0; i < n; i++) {
            EXPECT_EQ(memcmp(src[i], decrypted_data[i], size), 0);
        }
    }
    for (int i = 0; i < sess_num; i++) {
        channel->RemoveSession(sessions[i]);
    }
    for (int i = 0; i < n; i++) {
        Deallocate(src[i]);
        Deallocate(encrypted_data[i]);
        Deallocate(decrypted_data[i]);
    }
    Deallocate(tweak);
    EXPECT_TRUE(channel->Close().ok());
}

TEST_P(QatCypherTestFixture, Sha256Test) {
    CypherChannelOption opt;
    opt.session_option.algorithm = CypherAlgorithm::kSHA256;
    auto p = CypherChannel::CreateCypherChannel(opt);
    EXPECT_TRUE(p.first.ok());
    auto channel = std::move(p.second);

    const int n = 256;
    unsigned int size = 4096;
    std::vector<unsigned char*> src;
    std::vector<unsigned char*> hash;
    std::vector<unsigned char*> hash_sw;
    std::vector<unsigned int> src_len;
    for (int i = 0; i < n; i++) {
        src.push_back((unsigned char*)Allocate(size));
        hash.push_back((unsigned char*)Allocate(32));
        hash_sw.push_back((unsigned char*)Allocate(32));
        src_len.push_back(size);
        for (unsigned int j = 0; j < size; j++)
            src[i][j] = (i ^ j) % 10 - '0';
    }

    {
        // Expect Invalid Argument
        CypherReqArgs args;
        args.op = CypherOp::kHash;
        auto r = channel->SubmitCypherReq(src[0], src_len[0], hash[0], 31, &args);
        EXPECT_TRUE(IsInvalidArgument(r));
    }

    for (int i = 0; i < n; i++) {
        CypherReqArgs args;
        args.op = CypherOp::kHash;
        auto r = channel->SubmitCypherReq(src[i], src_len[i], hash[i], 32, &args);
        EXPECT_TRUE(IsOk(r));
    }

    {
        CypherResult results[n];
        int m = 0;
        while (m < n) {
            m += channel->Poll(results + m, n, -1);
        }
        for (int i = 0; i < n; i++) {
            EXPECT_TRUE(IsOk(results[i].status));
        }
    }

    opt.engine = EngineType::kSoftware;
    p = CypherChannel::CreateCypherChannel(opt);
    EXPECT_TRUE(p.first.ok());
    auto sw_channel = std::move(p.second);

    {
        for (int i = 0; i < n; i++) {
            CypherReqArgs args;
            args.op = CypherOp::kHash;
            auto r = sw_channel->SubmitCypherReq(src[i], size, hash_sw[i], 32, &args);
            EXPECT_TRUE(IsOk(r));
            EXPECT_EQ(memcmp(hash[i], hash_sw[i], 32), 0);
        }
    }

    for (int i = 0; i < n; i++) {
        Deallocate(src[i]);
        Deallocate(hash[i]);
        Deallocate(hash_sw[i]);
    }
    EXPECT_TRUE(channel->Close().ok());
    EXPECT_TRUE(sw_channel->Close().ok());
}

INSTANTIATE_TEST_SUITE_P(QatCypherTestFixtureByMemoryType, QatCypherTestFixture, ::testing::Bool());

}  // namespace vesal
