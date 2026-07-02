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

#include <memory>

#include "gtest/gtest.h"
#include "vesal/codec.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

namespace vesal {

class KaeCodecTest : public ::testing::TestWithParam<std::tuple<int, int>> {
protected:
    void SetUp() override {
        std::tie(src_len_, sgl_size_) = GetParam();
        InitOptions init_opt;
        init_opt.codec_init_opt.init_kae = true;
        init_opt.codec_init_opt.init_qat = false;
        init_opt.cypher_init_opt.init_qat = false;
        init_opt.data_flow_init_opt.init_dsa = false;
        init_opt.mem_pool_init_opt.init_mem_pool = true;
        FLAGS_vesal_log_console_output = true;
        ASSERT_TRUE(Init(init_opt));
        vesal::CodecChannelOption channel_opts;
        channel_opts.checksum_type = CodecChecksumType::kCrc32;
        channel_opts.engine_type = CodecEngineType::kKae;
        channel_opts.comp_algorithm = CodecAlgorithm::kDeflate;
        auto kae_channel_result = CodecChannel::CreateCodecChannel(channel_opts);
        ASSERT_TRUE(kae_channel_result.first.ok());
        kae_channel_ = std::move(kae_channel_result.second);

        channel_opts.engine_type = CodecEngineType::kSoftware;
        auto sw_channel_result = CodecChannel::CreateCodecChannel(channel_opts);
        EXPECT_TRUE(sw_channel_result.first.ok());
        sw_channel_ = std::move(sw_channel_result.second);
    }

    void TearDown() override {
        auto kae_close_result = kae_channel_->Close();
        EXPECT_TRUE(kae_close_result.ok());
        auto sw_close_result = sw_channel_->Close();
        EXPECT_TRUE(sw_close_result.ok());
        FLAGS_vesal_log_console_output = false;
        vesal::Uninit();
    }

private:
    std::unique_ptr<CodecChannel> kae_channel_;
    std::unique_ptr<CodecChannel> sw_channel_;
    int src_len_;
    int sgl_size_;
};

TEST(KaeTest, TestInvalidArgs) {
    // Expect channel creation failed with invalid algorithm
    vesal::CodecChannelOption channel_opts;
    channel_opts.engine_type = CodecEngineType::kKae;
    for (int algo = static_cast<int>(CodecAlgorithm::kLz4);
         algo < static_cast<int>(CodecAlgorithm::kNum);
         algo++) {
        if (algo == static_cast<int>(CodecAlgorithm::kDeflate)) {
            continue;
        }
        channel_opts.comp_algorithm = static_cast<CodecAlgorithm>(algo);
        auto kae_channel_result = CodecChannel::CreateCodecChannel(channel_opts);
        EXPECT_FALSE(IsInvalidArgument(kae_channel_result.first));
    }
}

TEST_P(KaeCodecTest, SyncApiCorrectnessTest) {
    const int len = src_len_;
    unsigned char* input = (unsigned char*)MemoryPool::GetInstance()->Allocate(len);
    unsigned char* compressed_output = (unsigned char*)MemoryPool::GetInstance()->Allocate(len);
    unsigned char* decompressed_output = (unsigned char*)MemoryPool::GetInstance()->Allocate(len);

    // Initialize input data
    for (int i = 0; i < len; i++) {
        input[i] = static_cast<unsigned char>(i % 256);
    }

    // Kae compression and sw decompression matches
    auto kae_compress_status = kae_channel_->Compress(input, len, compressed_output, len);
    EXPECT_TRUE(IsOk(kae_compress_status.status));

    auto sw_decompress_status =
        sw_channel_->Decompress(compressed_output, len, decompressed_output, len);
    EXPECT_TRUE(IsOk(sw_decompress_status.status));
    EXPECT_EQ(memcmp(decompressed_output, input, len), 0);
    EXPECT_EQ(kae_compress_status.in_checksum, sw_decompress_status.out_checksum);
    EXPECT_EQ(kae_compress_status.out_checksum, sw_decompress_status.in_checksum);

    // Sw compression and kae decompression matches
    auto sw_compress_status = sw_channel_->Compress(input, len, compressed_output, len);
    EXPECT_TRUE(IsOk(sw_compress_status.status));

    auto kae_decompress_status =
        kae_channel_->Decompress(compressed_output, len, decompressed_output, len);
    EXPECT_TRUE(IsOk(kae_decompress_status.status));
    EXPECT_EQ(memcmp(decompressed_output, input, len), 0);
    EXPECT_EQ(kae_compress_status.in_checksum, sw_decompress_status.out_checksum);
    EXPECT_EQ(kae_compress_status.out_checksum, sw_decompress_status.in_checksum);

    MemoryPool::GetInstance()->Deallocate(input);
    MemoryPool::GetInstance()->Deallocate(compressed_output);
    MemoryPool::GetInstance()->Deallocate(decompressed_output);
}

TEST_P(KaeCodecTest, AsyncApiCorrectnessTest) {
    const int len = src_len_;
    const int sgl_size = sgl_size_;
    const int io_depth = 64;

    unsigned char* src_raw[io_depth];
    unsigned char* compressed_raw[io_depth];
    unsigned char* decompressed_raw[io_depth];
    for (int i = 0; i < io_depth; i++) {
        src_raw[i] = (unsigned char*)MemoryPool::GetInstance()->Allocate(len * sgl_size);
        compressed_raw[i] = (unsigned char*)MemoryPool::GetInstance()->Allocate(len * sgl_size);
        decompressed_raw[i] = (unsigned char*)MemoryPool::GetInstance()->Allocate(len * sgl_size);
        for (int j = 0; j < len * sgl_size; j++) {
            src_raw[i][j] = static_cast<unsigned char>(j % 256);
        }
    }

    std::vector<unsigned char*> src[io_depth];
    std::vector<unsigned int> src_len[io_depth];
    std::vector<unsigned char*> compressed[io_depth];
    std::vector<unsigned int> compressed_len[io_depth];
    for (int i = 0; i < io_depth; i++) {
        for (int j = 0; j < sgl_size; j++) {
            src[i].push_back(src_raw[i] + j * len);
            src_len[i].push_back(len);
        }
    }

    // Compression
    for (int i = 0; i < io_depth; i++) {
        auto kae_compress_status = kae_channel_->CompressSGLAsync(
            src[i], src_len[i], compressed_raw[i], len * sgl_size, nullptr);
        EXPECT_TRUE(IsOk(kae_compress_status));
    }
    int polled = 0;
    CodecResult results[io_depth];
    while (polled < io_depth) {
        int n = kae_channel_->Poll(results, io_depth, 0);
        for (int j = 0; j < n; j++) {
            EXPECT_TRUE(IsOk(results[j].status));
            int m = results[j].produced;
            // Split compressed results into SGL
            compressed[polled].resize(0);
            compressed_len[polled].resize(0);
            for (unsigned char* addr = compressed_raw[polled]; m > 0; addr += len) {
                compressed[polled].push_back(addr);
                compressed_len[polled].push_back(m > len ? len : m);
                m -= len;
            }
            polled++;
        }
    }

    // Decompression
    for (int i = 0; i < io_depth; i++) {
        auto kae_decompress_status = kae_channel_->DecompressSGLAsync(
            compressed[i], compressed_len[i], decompressed_raw[i], len * sgl_size, nullptr);
        EXPECT_TRUE(IsOk(kae_decompress_status));
    }
    polled = 0;
    while (polled < io_depth) {
        int n = kae_channel_->Poll(results, io_depth, 0);
        for (int j = 0; j < n; j++) {
            EXPECT_TRUE(IsOk(results[j].status));
        }
        polled += n;
    }

    for (int i = 0; i < io_depth; i++) {
        EXPECT_EQ(memcmp(decompressed_raw[i], src_raw[i], len * sgl_size), 0);
    }

    for (int i = 0; i < io_depth; i++) {
        for (int j = 0; j < sgl_size; j++) {
            MemoryPool::GetInstance()->Deallocate(src_raw[i]);
            MemoryPool::GetInstance()->Deallocate(compressed_raw[i]);
            MemoryPool::GetInstance()->Deallocate(decompressed_raw[i]);
        }
    }
}

TEST_P(KaeCodecTest, CrcTest) {
    const int len = src_len_;
    const int sgl_size = sgl_size_;
    const int io_depth = 1;

    unsigned char* src_raw[io_depth];
    unsigned char* compressed_raw[io_depth];
    unsigned char* decompressed_raw[io_depth];
    for (int i = 0; i < io_depth; i++) {
        src_raw[i] = (unsigned char*)MemoryPool::GetInstance()->Allocate(len * sgl_size);
        compressed_raw[i] = (unsigned char*)MemoryPool::GetInstance()->Allocate(len * sgl_size);
        decompressed_raw[i] = (unsigned char*)MemoryPool::GetInstance()->Allocate(len * sgl_size);
        for (int j = 0; j < len * sgl_size; j++) {
            src_raw[i][j] = static_cast<unsigned char>(j % 256);
        }
    }

    std::vector<unsigned char*> src[io_depth];
    std::vector<unsigned int> src_len[io_depth];
    std::vector<unsigned char*> compressed[io_depth];
    std::vector<unsigned int> compressed_len[io_depth];
    for (int i = 0; i < io_depth; i++) {
        for (int j = 0; j < sgl_size; j++) {
            src[i].push_back(src_raw[i] + j * len);
            src_len[i].push_back(len);
        }
    }

    // Kae compression and sw decompression matches
    auto kae_compress_status =
        kae_channel_->CompressSGL(src[0], src_len[0], compressed_raw[0], len * sgl_size);
    EXPECT_TRUE(IsOk(kae_compress_status.status));
    compressed_len[0].push_back(kae_compress_status.produced);

    auto sw_decompress_status = sw_channel_->Decompress(
        compressed_raw[0], compressed_len[0][0], decompressed_raw[0], len * sgl_size);
    EXPECT_TRUE(IsOk(sw_decompress_status.status));
    EXPECT_EQ(memcmp(decompressed_raw[0], src_raw[0], len * sgl_size), 0);
    EXPECT_EQ(kae_compress_status.in_checksum, sw_decompress_status.out_checksum);
    EXPECT_EQ(kae_compress_status.out_checksum, sw_decompress_status.in_checksum);

    for (int i = 0; i < io_depth; i++) {
        for (int j = 0; j < sgl_size; j++) {
            MemoryPool::GetInstance()->Deallocate(src_raw[i]);
            MemoryPool::GetInstance()->Deallocate(compressed_raw[i]);
            MemoryPool::GetInstance()->Deallocate(decompressed_raw[i]);
        }
    }
}

INSTANTIATE_TEST_CASE_P(KaeCodecTestBySglSizeAndIoDepth,
                        KaeCodecTest,
                        ::testing::Combine(::testing::Values(4096, 32768),
                                           ::testing::Values(1, 16)));

}  // namespace vesal