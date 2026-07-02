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

#include <string.h>

#include <ctime>
#include <memory>

#include "codec/qat/qat_codec_dedicated_channel.h"
#include "codec/zstd_helper/zstd_helper_impl.h"
#include "common/qat/qat_util.h"
#include "gtest/gtest.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"
#include "vesal/zstd_helper.h"

namespace vesal {

namespace {

class MockCodec : public Codec {
public:
    MockCodec() {
        EXPECT_TRUE(MemoryPool::GetInstance()->Init());
    }
    std::pair<Status, std::unique_ptr<CodecChannel>> CreateCodecChannel(
        const CodecChannelOption& opts) override {
        return {UnknownError("test"), nullptr};
    }

    ~MockCodec() {
        MemoryPool::GetInstance()->Reset();
    }
};

struct MockCodecChannelOpts {
    int CompressAsync_mock_type = 0;
};

class MockCodecChannel : public qat::QatCodecDedicatedChannel {
public:
    MockCodecChannel()
        : qat::QatCodecDedicatedChannel(
              CodecChannelOption(), nullptr, fLU::FLAGS_vesal_codec_qat_max_in_qat_num) {}
    StatusCode CompressAsync(unsigned char* src,
                             unsigned int src_len,
                             unsigned char* dst,
                             unsigned int dst_len,
                             void* ctx) override {
        switch (opts_.CompressAsync_mock_type) {
        // failure
        case 1:
            return StatusCode::kUnknown;
        // fake success
        case 2:
            return StatusCode::kOk;
        }
        return StatusCode::kUnknown;
    }

    ssize_t Poll(CodecResult results[], unsigned int max_num, int timeout) override {
        return -1;
    }

    Status Close() override {
        return UnknownError("test");
    }

    MockCodecChannelOpts opts_;
};

void GenerateInput(uint32_t input_len, unsigned char* input) {
    std::string str = "The quick brown fox jumps over the lazy dog";
    std::srand(std::time(nullptr));
    for (uint32_t i = 0; i < input_len - 1; ++i)
        input[i] = str[std::rand() % str.size()];
    input[input_len - 1] = '\0';
}

bool ValidateResult(unsigned char* data1, unsigned char* data2, int size) {
    return !strcmp(reinterpret_cast<const char*>(data1), reinterpret_cast<const char*>(data2));
}

}  // namespace

TEST(TestZstd, Sample) {
    int block_size = 4096;
    int comp_size, decomp_size;
    std::unique_ptr<unsigned char[]> src_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> comp_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> decomp_data =
        std::make_unique<unsigned char[]>(block_size * 2);
    GenerateInput(block_size, src_data.get());

    CodecInitOptions init_opt;
    EXPECT_TRUE(MemoryPool::GetInstance()->Init());
    EXPECT_TRUE(qat::QatUserStart());
    Codec::Init(init_opt);

    // example of ZSTDHelper+libzstd usage
    {
        vesal::ZSTDHelperOpts opt{.engine_type = CodecEngineType::kQat};
        auto helper_result = vesal::ZSTDHelper::GetZSTDHelper(opt);
        EXPECT_TRUE(helper_result.first.ok());
        std::unique_ptr<vesal::ZSTDHelper> helper = std::move(helper_result.second);
        auto zstd_cctx = ZSTD_createCCtx();
        ZSTD_registerSequenceProducer(zstd_cctx, helper.get(), vesal::QatSequenceProducer);
        ZSTD_CCtx_setParameter(zstd_cctx, ZSTD_c_enableSeqProducerFallback, 1);
        comp_size = ZSTD_compress2(
            zstd_cctx, comp_data.get(), block_size * 1.5, src_data.get(), block_size);
        auto close_result = helper->Close();
        EXPECT_TRUE(close_result.ok());
        ZSTD_freeCCtx(zstd_cctx);
    }

    decomp_size = ZSTD_decompress(decomp_data.get(), block_size, comp_data.get(), comp_size);
    EXPECT_EQ(decomp_size, block_size);
    EXPECT_TRUE(ValidateResult(src_data.get(), decomp_data.get(), block_size));
    auto shutdown_result = vesal::Codec::Uninit();
    EXPECT_TRUE(shutdown_result);
}

TEST(TestZstd, InvalidOptions) {
    {
        CodecInitOptions init_opt;
        init_opt.init_qat = false;
        EXPECT_TRUE(MemoryPool::GetInstance()->Init());
        EXPECT_TRUE(Codec::Init(init_opt));
        {
            vesal::ZSTDHelperOpts opt{.engine_type = CodecEngineType::kQat,
                                      .intermediate_data_size = 0};
            auto res = vesal::ZSTDHelper::GetZSTDHelper(opt);
            EXPECT_TRUE(IsInvalidArgument(res.first));
        }
        Codec::Uninit();
        init_opt.init_qat = true;
        EXPECT_TRUE(Codec::Init(init_opt));
        // intermediate_data_size too small
        {
            vesal::ZSTDHelperOpts opt{.engine_type = CodecEngineType::kQat,
                                      .intermediate_data_size = 0};
            auto res = vesal::ZSTDHelper::GetZSTDHelper(opt);
            EXPECT_TRUE(IsInvalidArgument(res.first));
        }
        // intermediate_data_size too big
        {
            vesal::ZSTDHelperOpts opt{.engine_type = CodecEngineType::kQat,
                                      .intermediate_data_size = ZSTD_BLOCKSIZE_MAX * 3};
            auto res = vesal::ZSTDHelper::GetZSTDHelper(opt);
            EXPECT_TRUE(IsInvalidArgument(res.first));
        }
        auto shutdown_result = Codec::Uninit();
        EXPECT_TRUE(shutdown_result);
    }
}

TEST(TestZstd, FallbackToSoftwareAndGetCorrectResult) {
    std::vector<MockCodecChannelOpts> opts_vec = {{1}, {2}};
    for (auto& opts : opts_vec) {
        auto mock_channel = std::make_unique<MockCodecChannel>();
        mock_channel->opts_ = opts;
        auto helper = std::make_unique<ZSTDHelperImpl>(ZSTDHelperOpts());
        helper->channel_ = std::move(mock_channel);
        // libzstd with fake helper
        {
            int block_size = 4096;
            int comp_size, decomp_size;
            std::unique_ptr<unsigned char[]> src_data =
                std::make_unique<unsigned char[]>(block_size * 2);
            std::unique_ptr<unsigned char[]> comp_data =
                std::make_unique<unsigned char[]>(block_size * 2);
            std::unique_ptr<unsigned char[]> decomp_data =
                std::make_unique<unsigned char[]>(block_size * 2);
            GenerateInput(block_size, src_data.get());
            auto zstd_cctx = ZSTD_createCCtx();
            ZSTD_registerSequenceProducer(zstd_cctx, helper.get(), vesal::QatSequenceProducer);
            ZSTD_CCtx_setParameter(zstd_cctx, ZSTD_c_enableSeqProducerFallback, 1);
            comp_size = ZSTD_compress2(
                zstd_cctx, comp_data.get(), block_size * 1.5, src_data.get(), block_size);
            ZSTD_freeCCtx(zstd_cctx);
            decomp_size =
                ZSTD_decompress(decomp_data.get(), block_size, comp_data.get(), comp_size);
            EXPECT_EQ(decomp_size, block_size);
            EXPECT_TRUE(ValidateResult(src_data.get(), decomp_data.get(), block_size));
        }
    }
}

TEST(TestZstd, HelperFailToClose) {
    auto mock_channel = std::make_unique<MockCodecChannel>();
    auto helper = std::make_unique<ZSTDHelperImpl>(ZSTDHelperOpts());
    helper->channel_ = std::move(mock_channel);
    auto res = helper->Close();
    EXPECT_TRUE(IsUnknown(res));
}

}  // namespace vesal
