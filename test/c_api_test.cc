
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

#include <gtest/gtest.h>
#include <linux/mman.h>
#include <sys/mman.h>

#include "common/memory_pool_helper.h"
#include "vesal/c_api/c_api_vesal.h"
#include "vesal/vesal.h"

namespace vesal {

TEST(CApiTest, Basic) {
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    init_opts.flags.vesal_log_console_output = VESAL_TRUE;
    VESAL_BOOL ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);
    vesal_uninit();
}

TEST(CApiTest, DefaultCodecChannelOptionsHaPolicyIsNone) {
    vesal_codec_channel_options_t chnnl_opts;
    default_vesal_codec_channel_options(&chnnl_opts);
    EXPECT_EQ(chnnl_opts.ha_policy, VESAL_HA_POLICY_NONE);
}

TEST(CApiTest, DefaultCypherChannelOptionsHaPolicyIsNone) {
    vesal_cypher_channel_option_t chnnl_opts;
    default_vesal_cypher_channel_options(&chnnl_opts);
    EXPECT_EQ(chnnl_opts.ha_policy, VESAL_HA_POLICY_NONE);
}

TEST(CApiTest, CreateDestroy) {
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    VESAL_BOOL ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);
    vesal_codec_channel_options_t chnnl_opts;
    default_vesal_codec_channel_options(&chnnl_opts);
    VesalCodecChannelHandle handle;
    auto create_r = vesal_create_codec_channel(&chnnl_opts, &handle);
    EXPECT_EQ(create_r, VESAL_OK);
    vesal_destroy_codec_channel(handle);
    vesal_uninit();
}

TEST(CApiTest, FlagsInit) {
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    init_opts.flags.vesal_codec_qat_max_in_qat_num = 1024;
    VESAL_BOOL ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);
    EXPECT_EQ(FLAGS_vesal_codec_qat_max_in_qat_num, 1024);
    vesal_uninit();
}

TEST(CApiTest, DedicatedCompressDecompress) {
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    VESAL_BOOL ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);
    vesal_codec_channel_options_t chnnl_opts;
    default_vesal_codec_channel_options(&chnnl_opts);
    chnnl_opts.mode = VESAL_CHANNEL_MODE_DEDICATED;
    chnnl_opts.comp_algorithm = VESAL_CODEC_ALGORITHM_LZ4;
    VesalCodecChannelHandle handle;
    auto create_r = vesal_create_codec_channel(&chnnl_opts, &handle);
    EXPECT_EQ(create_r, VESAL_OK);
    uint32_t input_len = 4096;
    uint32_t output_len = 8192;
    std::unique_ptr<unsigned char[]> input = std::make_unique<unsigned char[]>(input_len);
    std::unique_ptr<unsigned char[]> output = std::make_unique<unsigned char[]>(output_len);
    std::unique_ptr<unsigned char[]> output2 = std::make_unique<unsigned char[]>(output_len);
    auto comp_r = vesal_codec_compress_async(
        handle, input.get(), input_len, output.get(), output_len, nullptr);
    EXPECT_EQ(comp_r, VESAL_OK);
    vesal_codec_result_t result;
    auto poll_r = 0;
    while (poll_r == 0) {
        poll_r = vesal_codec_poll(handle, &result, 1, -1);
    }
    EXPECT_EQ(poll_r, 1);
    auto new_src_len = result.produced;
    EXPECT_TRUE(result.status == VESAL_OK) << result.status;
    auto decomp_r = vesal_codec_decompress_async(
        handle, output.get(), new_src_len, output2.get(), output_len, nullptr);
    EXPECT_EQ(decomp_r, VESAL_OK);
    poll_r = 0;
    while (poll_r == 0) {
        poll_r = vesal_codec_poll(handle, &result, 1, -1);
    }
    EXPECT_EQ(poll_r, 1);
    EXPECT_TRUE(result.status == VESAL_OK) << result.status;
    EXPECT_EQ(result.consumed, new_src_len);
    EXPECT_EQ(result.produced, input_len);
    EXPECT_EQ(result.in_checksum, result.out_checksum);
    vesal_destroy_codec_channel(handle);
    vesal_uninit();
}

TEST(CApiTest, SharedCompressDecompress) {
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    VESAL_BOOL ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);
    vesal_codec_channel_options_t chnnl_opts;
    default_vesal_codec_channel_options(&chnnl_opts);
    chnnl_opts.mode = VESAL_CHANNEL_MODE_SHARED;
    chnnl_opts.comp_algorithm = VESAL_CODEC_ALGORITHM_LZ4;

    struct Ctx {
        std::atomic<bool> done{false};
        vesal_codec_result_t result;
    };
    Ctx ctx;
    auto user_cb = [](const vesal_codec_result_t* res) {
        EXPECT_TRUE(res->status == VESAL_OK) << res->status;
        auto* ctx_p = reinterpret_cast<Ctx*>(res->ctx);
        ctx_p->done.store(true, std::memory_order_release);
        ctx_p->result = *res;
    };
    chnnl_opts.user_cb = user_cb;
    VesalCodecChannelHandle handle;
    auto create_r = vesal_create_codec_channel(&chnnl_opts, &handle);
    EXPECT_EQ(create_r, VESAL_OK);
    uint32_t input_len = 4096;
    uint32_t output_len = 8192;
    std::unique_ptr<unsigned char[]> input = std::make_unique<unsigned char[]>(input_len);
    std::unique_ptr<unsigned char[]> output = std::make_unique<unsigned char[]>(output_len);
    std::unique_ptr<unsigned char[]> output2 = std::make_unique<unsigned char[]>(output_len);
    auto comp_r =
        vesal_codec_compress_async(handle, input.get(), input_len, output.get(), output_len, &ctx);
    EXPECT_EQ(comp_r, VESAL_OK);
    while (!ctx.done.load(std::memory_order_acquire)) {
        usleep(100);
    }
    Ctx ctx2;
    auto decomp_r = vesal_codec_decompress_async(
        handle, output.get(), ctx.result.produced, output2.get(), output_len, &ctx2);
    EXPECT_EQ(decomp_r, VESAL_OK);
    while (!ctx2.done.load(std::memory_order_acquire)) {
        usleep(100);
    }
    EXPECT_TRUE(ctx2.result.status == VESAL_OK) << ctx2.result.status;
    EXPECT_EQ(ctx2.result.consumed, ctx.result.produced);
    EXPECT_EQ(ctx2.result.produced, input_len);
    EXPECT_EQ(ctx2.result.in_checksum, ctx2.result.out_checksum);
    EXPECT_EQ(ctx2.result.in_checksum, ctx.result.out_checksum);
    vesal_destroy_codec_channel(handle);
    vesal_uninit();
}

TEST(CApiTest, SharedSyncCompDecomp) {
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    VESAL_BOOL ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);
    vesal_codec_channel_options_t chnnl_opts;
    default_vesal_codec_channel_options(&chnnl_opts);
    chnnl_opts.mode = VESAL_CHANNEL_MODE_SHARED;
    chnnl_opts.comp_algorithm = VESAL_CODEC_ALGORITHM_ZLIB;
    VesalCodecChannelHandle handle;
    auto create_r = vesal_create_codec_channel(&chnnl_opts, &handle);
    EXPECT_EQ(create_r, VESAL_OK);

    uint32_t input_len = 4096;
    uint32_t output_len = 8192;
    std::unique_ptr<unsigned char[]> input = std::make_unique<unsigned char[]>(input_len);
    std::unique_ptr<unsigned char[]> output = std::make_unique<unsigned char[]>(output_len);
    std::unique_ptr<unsigned char[]> output2 = std::make_unique<unsigned char[]>(input_len);
    auto comp_r = vesal_codec_compress(handle, input.get(), input_len, output.get(), output_len);
    EXPECT_EQ(comp_r.status, VESAL_OK);
    auto decomp_r =
        vesal_codec_decompress(handle, output.get(), comp_r.produced, output2.get(), input_len);
    EXPECT_EQ(decomp_r.status, VESAL_OK);
    EXPECT_EQ(comp_r.produced, decomp_r.consumed);
    EXPECT_EQ(comp_r.consumed, decomp_r.produced);
    vesal_destroy_codec_channel(handle);
    vesal_uninit();
}

TEST(CApiTest, CyAndMemPoolRegister) {
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    init_opts.codec_init_qat = VESAL_FALSE;
    VESAL_BOOL ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);

    // allocate hugepage
    const uint64_t page_size = 1UL << 21;
    const uint64_t page_num = 64;
    void* addr =
        (void*)mmap(nullptr,
                    page_size * page_num,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB | MAP_HUGE_2MB,
                    -1,
                    0);
    EXPECT_NE(addr, MAP_FAILED);

    // register memory
    vesal_memory_info_t info = {.virtual_addr = addr,
                                .physical_addr = nullptr,
                                .len = page_size * page_num,
                                .page_size = page_size};
    EXPECT_EQ(vesal_register(&info, 1), VESAL_TRUE);

    vesal_cypher_channel_option_t channel_opt;
    default_vesal_cypher_channel_options(&channel_opt);
    channel_opt.session_option.algorithm = VESAL_CYPHER_ALGORITHM_SHA256;
    VesalCypherChannelHandle handle;
    VESAL_ERROR_CODE create_r = vesal_create_cypher_channel(&channel_opt, &handle);
    EXPECT_EQ(create_r, VESAL_OK);

    unsigned char* src = (unsigned char*)addr;
    unsigned char* dst = src + 4096;

    vesal_cypher_req_args_t args;
    args.ctx = NULL;
    args.op = VESAL_CYPHER_OP_HASH;
    args.session = NULL;
    VESAL_ERROR_CODE r = vesal_cypher_submit(handle, src, 4096, dst, 4096, &args);
    EXPECT_EQ(r, VESAL_OK);

    size_t polled_num = 0;
    vesal_cypher_result_t result[1];
    while (polled_num < 1) {
        ssize_t m = vesal_cypher_poll(handle, result + polled_num, 1, -1);
        EXPECT_NE(m, -1);
        polled_num += m;
    }

    EXPECT_EQ(result[0].status, VESAL_OK);
    vesal_destroy_cypher_channel(handle);
    vesal_uninit();
    munmap(addr, page_num * page_size);
}

TEST(CApiTest, MemPoolInit) {
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    EXPECT_EQ(init_opts.mem_pool_init_opt.prealloc_page_size, 2 * 1024 * 1024);
    VESAL_BOOL ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);
    vesal_uninit();

    init_opts.mem_pool_init_opt.prealloc_size_mb = 128;
    ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);
    EXPECT_EQ(vesal::MemoryPool::GetInstance()->GetMemoryUsage(),
              init_opts.mem_pool_init_opt.prealloc_size_mb * 1024 * 1024)
        << vesal::MemoryPool::GetInstance()->GetMemoryUsage();
    vesal_uninit();
}

TEST(CApiTest, CypherBigSglTest) {
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    init_opts.codec_init_qat = VESAL_FALSE;
    VESAL_BOOL ret = vesal_init(&init_opts);
    EXPECT_EQ(ret, VESAL_TRUE);

    vesal_cypher_channel_option_t channel_opt;
    default_vesal_cypher_channel_options(&channel_opt);
    channel_opt.session_option.aes_xts_key =
        "01234567890123450123456789012345012345678901234501234567890abcde";
    channel_opt.session_option.key_len = 32;
    channel_opt.session_option.algorithm = VESAL_CYPHER_ALGORITHM_AES_XTS;
    VesalCypherChannelHandle handle;
    VESAL_ERROR_CODE create_r = vesal_create_cypher_channel(&channel_opt, &handle);
    EXPECT_EQ(create_r, VESAL_OK);

    unsigned char* tweak = (unsigned char*)vesal_allocate(16);
    const int n = 256;
    const int sgl_num = 4096;
    const int size = 4096;
    unsigned char* src[n];
    unsigned char* encrypted_data[n];
    unsigned char* decrypted_data[n];

    for (int i = 0; i < n; i++) {
        src[i] = (unsigned char*)vesal_allocate(size);
        encrypted_data[i] = (unsigned char*)vesal_allocate(size);
        decrypted_data[i] = (unsigned char*)vesal_allocate(size);
    }

    for (int i = 0; i < n; i++) {
        unsigned char* src_sgl[sgl_num];
        unsigned int src_len_sgl[sgl_num];
        for (int j = 0; j < sgl_num; j++) {
            src_len_sgl[j] = size / sgl_num;
            if (j == 0) {
                src_sgl[j] = src[i];
            } else {
                src_sgl[j] = src_sgl[j - 1] + src_len_sgl[j - 1];
            }
        }
        vesal_cypher_req_args_t args;
        args.aes_xts_tweak = tweak;
        args.ctx = NULL;
        args.op = VESAL_CYPHER_OP_ENCRYPT;
        args.session = NULL;  // Set to NULL explicitly
        VESAL_ERROR_CODE r = vesal_cypher_submit_sgl(
            handle, src_sgl, src_len_sgl, sgl_num, encrypted_data[i], size, &args);
        if (r != VESAL_OK) {
            printf("Send failed, ret: %d\n", r);
            exit(-1);
        }
    }

    size_t polled_num = 0;
    vesal_cypher_result_t result[n];
    while (polled_num < n) {
        ssize_t m = vesal_cypher_poll(handle, result + polled_num, n, -1);
        EXPECT_GE(m, 0);
        polled_num += m;
    }

    for (int i = 0; i < n; i++) {
        EXPECT_EQ(result[i].status, VESAL_OK);
    }

    for (int i = 0; i < n; i++) {
        vesal_cypher_req_args_t args;
        args.aes_xts_tweak = tweak;
        args.ctx = NULL;
        args.op = VESAL_CYPHER_OP_DECRYPT;
        args.session = NULL;  // Set to NULL explicitly
        VESAL_ERROR_CODE r =
            vesal_cypher_submit(handle, encrypted_data[i], size, decrypted_data[i], size, &args);
        EXPECT_EQ(r, VESAL_OK);
    }

    polled_num = 0;
    while (polled_num < n) {
        ssize_t m = vesal_cypher_poll(handle, result + polled_num, n, -1);
        EXPECT_GE(m, 0);
        polled_num += m;
    }

    for (int i = 0; i < n; i++) {
        EXPECT_EQ(memcmp(src[i], decrypted_data[i], size), 0);
    }

    vesal_free(tweak);
    for (int i = 0; i < n; i++) {
        vesal_free(src[i]);
        vesal_free(encrypted_data[i]);
        vesal_free(decrypted_data[i]);
    }
    vesal_destroy_cypher_channel(handle);
    vesal_uninit();
}

}  // namespace vesal
