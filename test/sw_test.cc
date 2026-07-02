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

#include <gtest/gtest-param-test.h>
#include <gtest/gtest.h>
#include <lz4.h>
#include <lz4frame.h>
#include <string.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <vector>

#include "codec/codec_internal.h"
#include "codec/dc_format.h"
#include "codec/sw/sw_codec.h"
#include "common/memory_pool_helper.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
namespace vesal {

namespace {
void GenerateInput(uint32_t input_len, unsigned char* input) {
    std::string str = "The quick brown fox jumps over the lazy dog";
    std::srand(std::time(nullptr));
    for (uint32_t i = 0; i < input_len - 1; ++i)
        input[i] = str[std::rand() % str.size()];
    input[input_len - 1] = '\0';
}
}  // namespace

class SwTest : public ::testing::TestWithParam<CodecAlgorithm> {
public:
    void SetUp() override {
        CodecInitOptions opts;
        opts.init_qat = false;
        EXPECT_TRUE(Codec::Init(opts));
        CodecChannelOption channel_opts;
        channel_opts.engine_type = CodecEngineType::kSoftware;
        channel_opts.comp_algorithm =
            ::testing::UnitTest::GetInstance()->current_test_info()->value_param()
                ? GetParam()
                : CodecAlgorithm::kLz4;
        channel_opts.checksum_type = CodecChecksumType::kCrc32;
        auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
        EXPECT_TRUE(channel_result.first.ok());
        channel_ = std::move(channel_result.second);
    }

    void TearDown() override {
        auto close_result = channel_->Close();
        EXPECT_TRUE(close_result.ok());
        EXPECT_TRUE(Codec::Uninit());
    }

    bool ValidateResult(unsigned char* data1, unsigned char* data2, int size) {
        return memcmp(data1, data2, size) == 0;
    }

protected:
    std::unique_ptr<CodecChannel> channel_;
};

TEST_P(SwTest, SyncApiSample) {
    int block_size = 4096;
    std::unique_ptr<unsigned char[]> src_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> comp_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> decomp_data =
        std::make_unique<unsigned char[]>(block_size * 2);
    GenerateInput(block_size, src_data.get());

    for (int i = 1; i <= block_size; i <<= 1) {
        CodecResult compress_result =
            channel_->Compress(src_data.get(), i, comp_data.get(), block_size * 2);
        EXPECT_TRUE(IsOk(compress_result.status));
        EXPECT_EQ(compress_result.consumed, i);
        CodecResult decompress_result = channel_->Decompress(
            comp_data.get(), compress_result.produced, decomp_data.get(), block_size * 2);
        EXPECT_TRUE(IsOk(decompress_result.status));
        EXPECT_EQ(compress_result.produced, decompress_result.consumed);
        EXPECT_EQ(decompress_result.produced, i);
        EXPECT_EQ(compress_result.in_checksum, decompress_result.out_checksum);
        EXPECT_EQ(compress_result.out_checksum, decompress_result.in_checksum);
        EXPECT_TRUE(ValidateResult(src_data.get(), decomp_data.get(), i));
    }
}

TEST_P(SwTest, AsyncApiSample) {
    int block_size = 4096;
    std::unique_ptr<unsigned char[]> src_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> comp_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> decomp_data =
        std::make_unique<unsigned char[]>(block_size * 2);
    GenerateInput(block_size, src_data.get());

    CodecResult compression_result;
    {
        auto submission_result = channel_->CompressAsync(
            src_data.get(), block_size, comp_data.get(), block_size * 2, nullptr);
        EXPECT_TRUE(IsOk(submission_result));
        // guarantee to immediately poll and get result
        size_t poll_result = channel_->Poll(&compression_result, 1, -1);
        EXPECT_EQ(1, poll_result);
        EXPECT_TRUE(IsOk(compression_result.status));
        EXPECT_EQ(compression_result.consumed, block_size);
    }

    CodecResult decompression_result;
    {
        auto submission_result = channel_->DecompressAsync(comp_data.get(),
                                                           compression_result.produced,
                                                           decomp_data.get(),
                                                           block_size * 2,
                                                           nullptr);
        EXPECT_TRUE(IsOk(submission_result));
        // guarantee to immediately poll and get result
        size_t poll_result = channel_->Poll(&decompression_result, 1, -1);
        EXPECT_EQ(1, poll_result);
        EXPECT_TRUE(IsOk(decompression_result.status));
        EXPECT_EQ(decompression_result.produced, block_size);
    }

    EXPECT_EQ(compression_result.produced, decompression_result.consumed);
    EXPECT_EQ(compression_result.in_checksum, decompression_result.out_checksum);
    EXPECT_EQ(compression_result.out_checksum, decompression_result.in_checksum);
    EXPECT_TRUE(ValidateResult(src_data.get(), decomp_data.get(), block_size));
}

TEST_P(SwTest, AsyncSGLApiSample) {
    int block_size = 4096;
    std::unique_ptr<unsigned char[]> src_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> comp_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> decomp_data =
        std::make_unique<unsigned char[]>(block_size * 2);
    GenerateInput(block_size, src_data.get());

    CodecResult compression_result;
    {
        unsigned int first_half = block_size / 2;
        unsigned int second_half = block_size - block_size / 2;
        std::vector<unsigned char*> src_sgl = {src_data.get(), &src_data[first_half]};
        std::vector<unsigned int> src_len_sgl = {first_half, second_half};
        auto submission_result = channel_->CompressSGLAsync(
            src_sgl, src_len_sgl, comp_data.get(), block_size * 2, nullptr);
        EXPECT_TRUE(IsOk(submission_result));
        // guarantee to immediately poll and get result
        size_t poll_result = channel_->Poll(&compression_result, 1, -1);
        EXPECT_EQ(1, poll_result);
        EXPECT_TRUE(IsOk(compression_result.status));
        EXPECT_EQ(compression_result.consumed, block_size);
    }

    CodecResult decompression_result;
    {
        unsigned int first_half = compression_result.produced / 2;
        unsigned int second_half = compression_result.produced - compression_result.produced / 2;
        std::vector<unsigned char*> src_sgl = {comp_data.get(), &comp_data[first_half]};
        std::vector<unsigned int> src_len_sgl = {first_half, second_half};
        auto submission_result = channel_->DecompressSGLAsync(
            src_sgl, src_len_sgl, decomp_data.get(), block_size * 2, nullptr);
        EXPECT_TRUE(IsOk(submission_result));
        // guarantee to immediately poll and get result
        size_t poll_result = channel_->Poll(&decompression_result, 1, -1);
        EXPECT_EQ(1, poll_result);
        EXPECT_TRUE(IsOk(decompression_result.status));
        EXPECT_EQ(decompression_result.produced, block_size);
    }

    EXPECT_EQ(compression_result.produced, decompression_result.consumed);
    EXPECT_EQ(compression_result.in_checksum, decompression_result.out_checksum);
    EXPECT_EQ(compression_result.out_checksum, decompression_result.in_checksum);
    EXPECT_TRUE(ValidateResult(src_data.get(), decomp_data.get(), block_size));
}

TEST_P(SwTest, InvalidInputs) {
    uint block_size = 4096;
    std::unique_ptr<unsigned char[]> in_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> out_data = std::make_unique<unsigned char[]>(block_size * 2);
    GenerateInput(block_size, in_data.get());
    {
        auto result = channel_->Compress(in_data.get(), 0, out_data.get(), block_size * 2);
        EXPECT_TRUE(IsInvalidArgument(result.status));
    }
    {
        auto result = channel_->Compress(in_data.get(), block_size, out_data.get(), 0);
        EXPECT_TRUE(IsOverflow(result.status));
    }
    {
        auto result = channel_->Decompress(in_data.get(), 0, out_data.get(), block_size * 2);
        EXPECT_TRUE(IsInvalidArgument(result.status));
    }
}

TEST_P(SwTest, DecompressInvalidInput) {
    int block_size = 4096;
    std::unique_ptr<unsigned char[]> src_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> comp_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> decomp_data =
        std::make_unique<unsigned char[]>(block_size * 2);

    // decompress failure because of invalid input
    memset(src_data.get(), 0, block_size);
    CodecResult decompress_fail_result =
        channel_->Decompress(src_data.get(), block_size, decomp_data.get(), block_size * 2);
    EXPECT_FALSE(IsOk(decompress_fail_result.status));

    // run successfully after failure
    GenerateInput(block_size, src_data.get());
    CodecResult compress_result =
        channel_->Compress(src_data.get(), block_size, comp_data.get(), block_size * 2);
    EXPECT_TRUE(IsOk(compress_result.status));
    EXPECT_EQ(compress_result.consumed, block_size);
    CodecResult decompress_result = channel_->Decompress(
        comp_data.get(), compress_result.produced, decomp_data.get(), block_size * 2);
    EXPECT_TRUE(IsOk(decompress_result.status));
    EXPECT_EQ(compress_result.produced, decompress_result.consumed);
    EXPECT_EQ(decompress_result.produced, block_size);
    EXPECT_TRUE(ValidateResult(src_data.get(), decomp_data.get(), block_size));
}

TEST_P(SwTest, DecompressWrongLength) {
    // Decompress failure because of src_len or dst_len too small
    int block_size = 4096;
    int small_size = 16;
    std::unique_ptr<unsigned char[]> src_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> comp_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> decomp_data =
        std::make_unique<unsigned char[]>(block_size * 2);

    GenerateInput(block_size, src_data.get());
    CodecResult compress_result =
        channel_->Compress(src_data.get(), block_size, comp_data.get(), block_size * 2);
    EXPECT_TRUE(IsOk(compress_result.status));
    EXPECT_EQ(compress_result.consumed, block_size);
    auto func_success = [&]() {
        CodecResult decompress_result = channel_->Decompress(
            comp_data.get(), compress_result.produced, decomp_data.get(), block_size);
        EXPECT_TRUE(IsOk(decompress_result.status));
        EXPECT_EQ(compress_result.produced, decompress_result.consumed);
        EXPECT_EQ(decompress_result.produced, block_size);
        EXPECT_TRUE(ValidateResult(src_data.get(), decomp_data.get(), block_size));
    };
    for (int src_len = 0; src_len < block_size;) {
        // decompress failure because of src_len too small
        CodecResult decompress_fail_result =
            channel_->Decompress(src_data.get(), src_len, decomp_data.get(), block_size);
        EXPECT_FALSE(IsOk(decompress_fail_result.status));
        // run successfully after failure
        func_success();
        if (src_len < small_size) {
            ++src_len;
        } else {
            src_len <<= 1;
        }
    }
    for (int dst_len = 0; dst_len < block_size;) {
        // decompress failure because of dst_len too small
        CodecResult decompress_fail_result = channel_->Decompress(
            src_data.get(), compress_result.produced, decomp_data.get(), dst_len);
        EXPECT_FALSE(IsOk(decompress_fail_result.status));
        // run successfully after failure
        func_success();
        if (dst_len < small_size) {
            ++dst_len;
        } else {
            dst_len <<= 1;
        }
    }
}

TEST_F(SwTest, Lz4DecompressExtraInput) {
    // Decompression still works even if there is extra input
    CodecChannelOption channel_opts;
    channel_opts.comp_algorithm = CodecAlgorithm::kLz4;
    channel_opts.checksum_type = CodecChecksumType::kCrc32;
    channel_opts.engine_type = CodecEngineType::kSoftware;
    channel_opts.compressed_checksum = false;
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_result.first.ok());
    auto channel = std::move(channel_result.second);

    int block_size = 4096;
    std::unique_ptr<unsigned char[]> src_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> comp_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> decomp_data =
        std::make_unique<unsigned char[]>(block_size * 2);

    GenerateInput(block_size, src_data.get());
    CodecResult compress_result =
        channel->Compress(src_data.get(), block_size, comp_data.get(), block_size * 2);
    EXPECT_TRUE(IsOk(compress_result.status));
    EXPECT_EQ(compress_result.consumed, block_size);

    // even if there is extra input, the decompression still works
    EXPECT_LT(compress_result.produced, block_size * 2);
    CodecResult decompress_result =
        channel->Decompress(comp_data.get(), block_size * 2, decomp_data.get(), block_size * 2);

    EXPECT_TRUE(IsOk(decompress_result.status));
    EXPECT_EQ(compress_result.produced, decompress_result.consumed);
    EXPECT_EQ(decompress_result.produced, block_size);
    EXPECT_TRUE(ValidateResult(src_data.get(), decomp_data.get(), block_size));
    auto close_result = channel->Close();
    EXPECT_TRUE(close_result.ok());
}

TEST_F(SwTest, NoCompressedCrcIfNotNeeded) {
    uint block_size = 4096;
    std::unique_ptr<unsigned char[]> input_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> compressed_data =
        std::make_unique<unsigned char[]>(block_size * 2);
    std::unique_ptr<unsigned char[]> decompressed_data =
        std::make_unique<unsigned char[]>(block_size * 2);
    GenerateInput(block_size, input_data.get());

    CodecChannelOption channel_opts;
    channel_opts.comp_algorithm = CodecAlgorithm::kLz4;
    channel_opts.checksum_type = CodecChecksumType::kCrc32;
    channel_opts.engine_type = CodecEngineType::kSoftware;
    channel_opts.compressed_checksum = false;
    auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_result.first.ok());
    auto channel = std::move(channel_result.second);

    auto comp_r =
        channel->Compress(input_data.get(), block_size, compressed_data.get(), block_size * 2);
    EXPECT_TRUE(IsOk(comp_r.status)) << comp_r.status;
    EXPECT_EQ(comp_r.out_checksum, 0);
    EXPECT_NE(comp_r.in_checksum, 0);
    auto decomp_r = channel->Decompress(
        compressed_data.get(), comp_r.produced, decompressed_data.get(), block_size * 2);
    EXPECT_TRUE(IsOk(decomp_r.status)) << decomp_r.status;
    EXPECT_NE(decomp_r.out_checksum, 0);
    EXPECT_EQ(decomp_r.in_checksum, 0);

    auto close_r = channel->Close();
    EXPECT_TRUE(close_r.ok()) << close_r;
}

TEST_F(SwTest, SwEngineFrameCanBeDecompressedByLz4FrameApi) {
    for (uint block_size = (1 << 10); block_size <= (1 << 16); block_size <<= 1) {
        std::unique_ptr<unsigned char[]> input_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        std::unique_ptr<unsigned char[]> compressed_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        std::unique_ptr<unsigned char[]> decompressed_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        GenerateInput(block_size, input_data.get());
        for (size_t i = static_cast<size_t>(CodecCompLevel::kLevel1);
             i < static_cast<size_t>(CodecCompLevel::kNum);
             ++i) {
            CodecChannelOption channel_opts;
            channel_opts.engine_type = CodecEngineType::kSoftware;
            channel_opts.comp_algorithm = CodecAlgorithm::kLz4;
            channel_opts.comp_level = static_cast<CodecCompLevel>(i);
            auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
            EXPECT_TRUE(channel_result.first.ok());
            auto channel_lvl_i = std::move(channel_result.second);
            auto comp_r = channel_lvl_i->Compress(
                input_data.get(), block_size, compressed_data.get(), block_size * 2);
            EXPECT_TRUE(IsOk(comp_r.status)) << comp_r.status;

            LZ4F_dctx* dctx;
            LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);

            unsigned int src_index = 0;
            unsigned int dst_index = 0;
            unsigned int src_len = comp_r.produced;
            unsigned int dst_len = block_size;
            auto src = compressed_data.get();
            auto dst = decompressed_data.get();
            size_t ret = 1;
            while (src_index < src_len && ret != 0) {
                EXPECT_GT(dst_len, dst_index);
                size_t dst_size = dst_len - dst_index;
                size_t src_size = src_len - src_index;
                ret = LZ4F_decompress(
                    dctx, dst + dst_index, &dst_size, src + src_index, &src_size, NULL);
                EXPECT_FALSE(LZ4F_isError(ret));
                src_index += src_size;
                dst_index += dst_size;
            }
            EXPECT_EQ(src_index, src_len);
            EXPECT_EQ(memcmp(input_data.get(), dst, block_size), 0);

            auto close_result = channel_lvl_i->Close();
            EXPECT_TRUE(close_result.ok());
            LZ4F_freeDecompressionContext(dctx);
        }
    }
}

TEST_F(SwTest, Lz4FrameCanBeDecompressedBySwEngine) {
    for (uint block_size = (1 << 10); block_size <= (1 << 16); block_size <<= 1) {
        std::unique_ptr<unsigned char[]> input_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        std::unique_ptr<unsigned char[]> compressed_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        std::unique_ptr<unsigned char[]> decompressed_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        GenerateInput(block_size, input_data.get());
        for (size_t i = static_cast<size_t>(CodecCompLevel::kLevel1);
             i < static_cast<size_t>(CodecCompLevel::kNum);
             ++i) {
            LZ4F_preferences_t preference;
            CodecChannelOption channel_opt;
            channel_opt.comp_level = static_cast<CodecCompLevel>(i);
            InitSoftwarePreferences(channel_opt, &preference);

            size_t ret = LZ4F_compressFrame(
                compressed_data.get(), block_size * 2, input_data.get(), block_size, &preference);
            EXPECT_FALSE(LZ4F_isError(ret));

            auto decomp_r = channel_->Decompress(
                compressed_data.get(), ret, decompressed_data.get(), block_size * 2);
            EXPECT_TRUE(IsOk(decomp_r.status)) << decomp_r.status;

            EXPECT_EQ(memcmp(input_data.get(), decompressed_data.get(), block_size), 0);
        }
    }
}

TEST_F(SwTest, SwEngineAndLz4FrameApiProduceSameResults) {
    for (uint block_size = (1 << 10); block_size <= (1 << 16); block_size <<= 1) {
        std::unique_ptr<unsigned char[]> input_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        std::unique_ptr<unsigned char[]> compressed_data_1 =
            std::make_unique<unsigned char[]>(block_size * 2);
        std::unique_ptr<unsigned char[]> compressed_data_2 =
            std::make_unique<unsigned char[]>(block_size * 2);
        GenerateInput(block_size, input_data.get());
        for (size_t i = static_cast<size_t>(CodecCompLevel::kLevel1);
             i < static_cast<size_t>(CodecCompLevel::kNum);
             ++i) {
            CodecChannelOption channel_opts;
            channel_opts.engine_type = CodecEngineType::kSoftware;
            channel_opts.comp_algorithm = CodecAlgorithm::kLz4;
            channel_opts.comp_level = static_cast<CodecCompLevel>(i);
            auto channel_result = CodecChannel::CreateCodecChannel(channel_opts);
            EXPECT_TRUE(channel_result.first.ok());
            auto channel_lvl_i = std::move(channel_result.second);
            auto comp_r = channel_lvl_i->Compress(
                input_data.get(), block_size, compressed_data_1.get(), block_size * 2);
            EXPECT_TRUE(IsOk(comp_r.status)) << comp_r.status;
            auto close_result = channel_lvl_i->Close();
            EXPECT_TRUE(close_result.ok());

            LZ4F_preferences_t preference;
            InitSoftwarePreferences(channel_opts, &preference);
            size_t ret = LZ4F_compressFrame(
                compressed_data_2.get(), block_size * 2, input_data.get(), block_size, &preference);
            EXPECT_FALSE(LZ4F_isError(ret));

            EXPECT_EQ(ret, comp_r.produced);
            EXPECT_EQ(memcmp(compressed_data_1.get(), compressed_data_2.get(), ret), 0);
        }
    }
}

TEST_P(SwTest, SyncAndAsyncApiProduceSameResults) {
    std::vector<CodecResult> sync_api_results;
    std::vector<CodecResult> sync_sgl_api_results;
    std::vector<std::unique_ptr<unsigned char[]>> input_data_list;
    std::vector<std::unique_ptr<unsigned char[]>> sync_compressed_data_list;
    std::vector<std::unique_ptr<unsigned char[]>> sync_sgl_compressed_data_list;
    uint min_size = (1 << 10);
    uint max_size = (1 << 16);
    for (uint block_size = min_size; block_size <= max_size; block_size <<= 1) {
        std::unique_ptr<unsigned char[]> input_data = std::make_unique<unsigned char[]>(block_size);
        GenerateInput(block_size, input_data.get());

        std::unique_ptr<unsigned char[]> compressed_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        auto sync_api_result =
            channel_->Compress(input_data.get(), block_size, compressed_data.get(), block_size * 2);
        EXPECT_TRUE(IsOk(sync_api_result.status)) << sync_api_result.status;
        sync_api_results.push_back(std::move(sync_api_result));
        sync_compressed_data_list.push_back(std::move(compressed_data));

        std::unique_ptr<unsigned char[]> sgl_compressed_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        std::vector<unsigned char*> src_sgl = {input_data.get(), &input_data[block_size / 2]};
        std::vector<unsigned int> src_len_sgl = {block_size / 2, block_size - block_size / 2};
        auto sync_sgl_api_result =
            channel_->CompressSGL(src_sgl, src_len_sgl, sgl_compressed_data.get(), block_size * 2);
        EXPECT_TRUE(IsOk(sync_sgl_api_result.status)) << sync_sgl_api_result.status;
        sync_sgl_api_results.push_back(std::move(sync_sgl_api_result));
        sync_sgl_compressed_data_list.push_back(std::move(sgl_compressed_data));

        input_data_list.push_back(std::move(input_data));
    }
    size_t i = 0;
    std::vector<std::unique_ptr<unsigned char[]>> async_compressed_data_list;
    for (uint block_size = min_size; block_size <= max_size; block_size <<= 1) {
        std::unique_ptr<unsigned char[]> compressed_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        auto submission_result = channel_->CompressAsync(
            input_data_list[i].get(), block_size, compressed_data.get(), block_size * 2, nullptr);
        EXPECT_TRUE(IsOk(submission_result)) << submission_result;
        async_compressed_data_list.push_back(std::move(compressed_data));
        ++i;
    }
    {
        std::unique_ptr<CodecResult[]> codec_result_list = std::make_unique<CodecResult[]>(i);
        size_t poll_result = channel_->Poll(codec_result_list.get(), i, -1);
        EXPECT_EQ(poll_result, i);
        while (i--) {
            EXPECT_EQ(sync_api_results[i].consumed, codec_result_list[i].consumed);
            EXPECT_EQ(sync_api_results[i].produced, codec_result_list[i].produced);
            EXPECT_EQ(sync_api_results[i].in_checksum, codec_result_list[i].in_checksum);
            EXPECT_EQ(sync_api_results[i].out_checksum, codec_result_list[i].out_checksum);
            EXPECT_EQ(sync_api_results[i].status, codec_result_list[i].status);
            EXPECT_EQ(memcmp(async_compressed_data_list[i].get(),
                             sync_compressed_data_list[i].get(),
                             sync_api_results[i].produced),
                      0);
        }
    }
    i = 0;
    std::vector<std::unique_ptr<unsigned char[]>> async_sgl_compressed_data_list;
    for (uint block_size = min_size; block_size <= max_size; block_size <<= 1) {
        unsigned char* input_data = input_data_list[i].get();
        std::unique_ptr<unsigned char[]> sgl_compressed_data =
            std::make_unique<unsigned char[]>(block_size * 2);
        std::vector<unsigned char*> src_sgl = {input_data, &input_data[block_size / 2]};
        std::vector<unsigned int> src_len_sgl = {block_size / 2, block_size - block_size / 2};
        auto submission_result = channel_->CompressSGLAsync(
            src_sgl, src_len_sgl, sgl_compressed_data.get(), block_size * 2, nullptr);
        EXPECT_TRUE(IsOk(submission_result)) << submission_result;
        async_sgl_compressed_data_list.push_back(std::move(sgl_compressed_data));
        ++i;
    }
    {
        std::unique_ptr<CodecResult[]> codec_result_list = std::make_unique<CodecResult[]>(i);
        size_t poll_result = channel_->Poll(codec_result_list.get(), i, -1);
        EXPECT_EQ(poll_result, i);
        while (i--) {
            EXPECT_EQ(sync_sgl_api_results[i].consumed, codec_result_list[i].consumed);
            EXPECT_EQ(sync_sgl_api_results[i].produced, codec_result_list[i].produced);
            EXPECT_EQ(sync_sgl_api_results[i].in_checksum, codec_result_list[i].in_checksum);
            EXPECT_EQ(sync_sgl_api_results[i].out_checksum, codec_result_list[i].out_checksum);
            EXPECT_EQ(sync_sgl_api_results[i].status, codec_result_list[i].status);
            EXPECT_EQ(memcmp(async_sgl_compressed_data_list[i].get(),
                             sync_sgl_compressed_data_list[i].get(),
                             sync_sgl_api_results[i].produced),
                      0);
        }
    }
}

TEST(SwTestMisc, SwResultQueueCapacity) {
    CodecInitOptions opts;
    opts.init_qat = false;
    EXPECT_TRUE(Codec::Init(opts));
    EXPECT_EQ(g_sw_codec->sw_result_queue_capacity_, FLAGS_vesal_codec_qat_max_in_qat_num);
}

TEST_P(SwTest, CompressAndCompressSGLProduceSameResults) {
    uint32_t list_len = 5;
    uint32_t pow2 = 8;
    uint32_t block_size = 1UL << (pow2 + list_len);
    auto input_data = std::make_unique<unsigned char[]>(block_size * 2);
    auto compressed_data_1 = std::make_unique<unsigned char[]>(block_size * 2);
    auto compressed_data_2 = std::make_unique<unsigned char[]>(block_size * 2);

    std::vector<unsigned char*> src_vec;
    std::vector<unsigned int> src_len_vec;
    uint32_t src_len_sum = 0;
    for (uint32_t i = 0; i < list_len; ++i) {
        uint32_t src_len = 1 << (pow2 + i);
        src_vec.push_back(input_data.get() + src_len_sum);
        src_len_vec.push_back(src_len);
        src_len_sum += src_len;
    }
    EXPECT_LT(src_len_sum, block_size);

    size_t iterations = 10;

    for (size_t i = 0; i < iterations; ++i) {
        GenerateInput(src_len_sum, input_data.get());

        CodecResult compress_result = channel_->Compress(
            input_data.get(), src_len_sum, compressed_data_1.get(), block_size * 2);
        EXPECT_TRUE(IsOk(compress_result.status));
        EXPECT_EQ(compress_result.consumed, src_len_sum);

        CodecResult compress_sgl_result =
            channel_->CompressSGL(src_vec, src_len_vec, compressed_data_2.get(), block_size * 2);
        EXPECT_TRUE(IsOk(compress_sgl_result.status));
        EXPECT_EQ(compress_sgl_result.consumed, src_len_sum);

        EXPECT_EQ(compress_result.produced, compress_sgl_result.produced);
        EXPECT_EQ(compress_result.in_checksum, compress_sgl_result.in_checksum);
        EXPECT_EQ(compress_result.out_checksum, compress_sgl_result.out_checksum);
        EXPECT_TRUE(ValidateResult(
            compressed_data_1.get(), compressed_data_2.get(), compress_result.produced));
    }
}

TEST_P(SwTest, CompressSGLInvalidArgument) {
    uint32_t block_size = 1024;
    auto input_data = std::make_unique<unsigned char[]>(block_size);
    auto compressed_data = std::make_unique<unsigned char[]>(block_size * 2);
    std::vector<unsigned char*> src_vec;
    std::vector<unsigned int> src_len_vec;
    src_vec.push_back(input_data.get());
    src_len_vec.push_back(block_size);
    CodecResult compress_sgl_async_result;

    // src_vec.size()!=src_len_vec.size()
    {
        src_len_vec.push_back(0);
        CodecResult compress_sgl_result =
            channel_->CompressSGL(src_vec, src_len_vec, compressed_data.get(), block_size * 2);
        EXPECT_TRUE(IsInvalidArgument(compress_sgl_result.status));

        StatusCode submission_result = channel_->CompressSGLAsync(
            src_vec, src_len_vec, compressed_data.get(), block_size * 2, nullptr);
        EXPECT_TRUE(IsOk(submission_result));
        size_t poll_result = channel_->Poll(&compress_sgl_async_result, 1, -1);
        EXPECT_EQ(1, poll_result);
        EXPECT_TRUE(IsInvalidArgument(compress_sgl_async_result.status));

        src_len_vec.pop_back();
    }

    // dst space not big enough
    {
        CodecResult compress_sgl_result =
            channel_->CompressSGL(src_vec, src_len_vec, compressed_data.get(), 1);
        EXPECT_TRUE(IsOverflow(compress_sgl_result.status));

        StatusCode submission_result =
            channel_->CompressSGLAsync(src_vec, src_len_vec, compressed_data.get(), 1, nullptr);
        EXPECT_TRUE(IsOk(submission_result));
        size_t poll_result = channel_->Poll(&compress_sgl_async_result, 1, -1);
        EXPECT_EQ(1, poll_result);
        EXPECT_TRUE(IsOverflow(compress_sgl_async_result.status));
    }
}

TEST_P(SwTest, DecompressSGLInvalidArgument) {
    uint32_t block_size = 1024;
    auto input_data = std::make_unique<unsigned char[]>(block_size);
    auto compressed_data = std::make_unique<unsigned char[]>(block_size * 2);
    auto decompressed_data = std::make_unique<unsigned char[]>(block_size * 2);
    GenerateInput(block_size, input_data.get());
    // get the compress result
    auto compress_result =
        channel_->Compress(input_data.get(), block_size, compressed_data.get(), block_size * 2);
    EXPECT_TRUE(IsOk(compress_result.status));
    EXPECT_EQ(compress_result.consumed, block_size);

    unsigned int compressed_data_len = compress_result.produced;
    unsigned int first_half = compressed_data_len / 2;
    unsigned int second_half = compressed_data_len - compressed_data_len / 2;
    std::vector<unsigned char*> src_vec = {&compressed_data[0], &compressed_data[first_half]};
    std::vector<unsigned int> src_len_vec = {first_half, second_half};
    CodecResult decompress_sgl_async_result;

    // src_vec.size()!=src_len_vec.size()
    {
        src_len_vec.push_back(0);
        CodecResult decompress_sgl_result =
            channel_->DecompressSGL(src_vec, src_len_vec, decompressed_data.get(), block_size * 2);
        EXPECT_TRUE(IsInvalidArgument(decompress_sgl_result.status));

        StatusCode submission_result = channel_->DecompressSGLAsync(
            src_vec, src_len_vec, decompressed_data.get(), block_size * 2, nullptr);
        EXPECT_TRUE(IsOk(submission_result));
        size_t poll_result = channel_->Poll(&decompress_sgl_async_result, 1, -1);
        EXPECT_EQ(1, poll_result);
        EXPECT_TRUE(IsInvalidArgument(decompress_sgl_async_result.status));
        src_len_vec.pop_back();
    }
}

TEST_P(SwTest, DecompressAndDecompressSGLProduceSameResults) {
    uint32_t block_size = 4096;
    auto input_data = std::make_unique<unsigned char[]>(block_size * 2);
    auto compressed_data = std::make_unique<unsigned char[]>(block_size * 2);
    auto decompressed_data_1 = std::make_unique<unsigned char[]>(block_size * 2);
    auto decompressed_data_2 = std::make_unique<unsigned char[]>(block_size * 2);
    size_t iterations = 10;

    for (size_t i = 0; i < iterations; ++i) {
        GenerateInput(block_size, input_data.get());

        CodecResult compress_result =
            channel_->Compress(input_data.get(), block_size, compressed_data.get(), block_size * 2);
        EXPECT_TRUE(IsOk(compress_result.status));
        EXPECT_EQ(compress_result.consumed, block_size);

        uint32_t compressed_data_len = compress_result.produced;
        CodecResult decompress_result = channel_->Decompress(
            compressed_data.get(), compressed_data_len, decompressed_data_1.get(), block_size * 2);
        EXPECT_TRUE(IsOk(decompress_result.status));
        EXPECT_EQ(decompress_result.consumed, compressed_data_len);

        EXPECT_GT(compressed_data_len, block_size / 2);
        std::vector<unsigned int> sgl_src_len = {block_size / 2,
                                                 compressed_data_len - block_size / 2};
        std::vector<unsigned char*> sgl_src = {compressed_data.get(),
                                               &compressed_data[block_size / 2]};
        CodecResult decompress_sgl_result = channel_->DecompressSGL(
            sgl_src, sgl_src_len, decompressed_data_2.get(), block_size * 2);
        EXPECT_TRUE(IsOk(decompress_sgl_result.status));
        EXPECT_EQ(decompress_sgl_result.consumed, compressed_data_len);

        EXPECT_EQ(decompress_result.produced, decompress_sgl_result.produced);
        EXPECT_EQ(decompress_result.in_checksum, decompress_sgl_result.in_checksum);
        EXPECT_EQ(decompress_result.out_checksum, decompress_sgl_result.out_checksum);
        EXPECT_TRUE(ValidateResult(
            decompressed_data_1.get(), decompressed_data_2.get(), decompress_result.produced));
    }
}

TEST(SwSimpleTest, UserCbTest) {
    CodecInitOptions opts;
    opts.init_qat = false;
    EXPECT_TRUE(Codec::Init(opts));
    CodecChannelOption ch_opt;
    std::atomic<size_t> counter{0};
    ch_opt.user_cb = [](const vesal::CodecResult& result) {
        std::atomic<size_t>* counter_p = reinterpret_cast<std::atomic<size_t>*>(result.ctx);
        if (result.status == vesal::StatusCode::kOk) {
            counter_p->fetch_add(1);
        }
    };
    ch_opt.engine_type = CodecEngineType::kSoftware;
    auto ch_r = CodecChannel::CreateCodecChannel(ch_opt);
    EXPECT_TRUE(ch_r.first.ok());
    auto ch = std::move(ch_r.second);
    const size_t req_num = 16;
    std::vector<std::unique_ptr<unsigned char[]>> srcs;
    std::vector<unsigned int> src_lens(req_num, 4096);
    std::vector<std::unique_ptr<unsigned char[]>> dsts;
    std::vector<unsigned int> dst_lens(req_num, 8192);
    for (size_t i = 0; i < req_num; ++i) {
        srcs.push_back(std::make_unique<unsigned char[]>(4096));
        GenerateInput(4096, srcs.back().get());
        dsts.push_back(std::make_unique<unsigned char[]>(8192));
    }
    for (size_t i = 0; i < req_num; ++i) {
        ch->CompressAsync(srcs[i].get(), src_lens[i], dsts[i].get(), dst_lens[i], &counter);
    }
    CodecResult results[req_num];
    auto n = ch->Poll(results, req_num, -1);
    EXPECT_EQ(n, req_num);
    EXPECT_EQ(counter.load(), req_num);
    // Now try sync apis
    for (size_t i = 0; i < req_num; ++i) {
        auto res = ch->Compress(srcs[i].get(), src_lens[i], dsts[i].get(), dst_lens[i]);
        EXPECT_EQ(res.status, StatusCode::kOk);
    }
    // Only async APIs called the callback.
    EXPECT_EQ(counter.load(), req_num);
    ch->Close();
    Codec::Uninit();
}

INSTANTIATE_TEST_SUITE_P(SwTestByAlgo,
                         SwTest,
                         ::testing::Values(CodecAlgorithm::kLz4,
                                           CodecAlgorithm::kDeflate,
                                           CodecAlgorithm::kZlib));

}  // namespace vesal
