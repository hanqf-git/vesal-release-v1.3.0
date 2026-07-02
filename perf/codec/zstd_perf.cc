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

#include <gflags/gflags.h>
#include <zstd.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>

#include "simple_histogram.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/vesal.h"
#include "vesal/zstd_helper.h"

DEFINE_uint64(chunk_size, 64 * 1024, "size of the source data");
DEFINE_string(method, "sw", "sw: software method; qat: qat accelerated method");
DEFINE_string(source_file,
              "/root/qianlima/workspace/lzbench-test/silesia/corpus.tar",
              "source file path");
DEFINE_uint64(compression_loop_num, 10, "amount of loops of the source file for compression");
DEFINE_uint64(decompression_loop_num, 10, "amount of loops of the source file for decompression");
DEFINE_uint64(level, 1, "compression level");
DEFINE_bool(streaming, false, "whether to use streaming API");
DEFINE_bool(zerocopy, false, "whether to use zero copy (only effective in qat method)");

unsigned char* g_src_data;
unsigned char* g_dst_data;
unsigned char* g_decompressed_data;
uint64_t g_file_size;
std::unique_ptr<vesal::ZSTDHelper> g_zstd_helper;

void EchoAllFlags() {
    VESAL_LOG(INFO) << "chunk_size: " << FLAGS_chunk_size;
    VESAL_LOG(INFO) << "method: " << FLAGS_method;
    VESAL_LOG(INFO) << "source_file: " << FLAGS_source_file;
    VESAL_LOG(INFO) << "compression_loop_num: " << FLAGS_compression_loop_num;
    VESAL_LOG(INFO) << "decompression_loop_num: " << FLAGS_decompression_loop_num;
    VESAL_LOG(INFO) << "level: " << FLAGS_level;
    VESAL_LOG(INFO) << "streaming: " << FLAGS_streaming;
    VESAL_LOG(INFO) << "zerocopy: " << FLAGS_zerocopy;
}

void SetUp() {
    std::string path = FLAGS_source_file;
    std::ifstream ifs(path);
    if (!ifs) {
        VESAL_LOG(CRITICAL) << "failed to open file: " << path;
    }
    // Get the file size.
    ifs.seekg(0, std::ios::end);
    g_file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    g_src_data = new unsigned char[g_file_size];
    ifs.read(reinterpret_cast<char*>(g_src_data), g_file_size);

    g_dst_data = new unsigned char[g_file_size * 2];
    g_decompressed_data = new unsigned char[g_file_size * 2];

    vesal::ZSTDHelperOpts opt;
    opt.comp_level = static_cast<vesal::CodecCompLevel>(FLAGS_level);
    if (FLAGS_method == "sw") {
        opt.engine_type = vesal::CodecEngineType::kSoftware;
    } else {
        opt.engine_type = vesal::CodecEngineType::kQat;
        auto helper_result = vesal::ZSTDHelper::GetZSTDHelper(opt);
        VESAL_CHECK(helper_result.first.ok()) << helper_result.first.message();
        g_zstd_helper = std::move(helper_result.second);
    }
}

void TearDown() {
    if (g_zstd_helper) {
        auto close_result = g_zstd_helper->Close();
        VESAL_CHECK(close_result.ok()) << close_result.message();
        g_zstd_helper.reset();
    }
}

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    FLAGS_vesal_log_console_output = true;

    if (FLAGS_compression_loop_num < 1) {
        FLAGS_compression_loop_num = 1;
        VESAL_LOG(WARN) << "compression_loop_num is set to 1 because it should be bigger than 0";
    }
    if (FLAGS_decompression_loop_num < 1) {
        FLAGS_decompression_loop_num = 1;
        VESAL_LOG(WARN) << "decompression_loop_num is set to 1 because it should be bigger than 0";
    }

    vesal::InitOptions init_opt;
    init_opt.codec_init_opt.init_qat = true;
    init_opt.cypher_init_opt.init_qat = false;
    init_opt.data_flow_init_opt.init_dsa = false;
    vesal::Init(init_opt);
    EchoAllFlags();
    SetUp();
    vesal::MemoryPool* memory_pool = nullptr;
    if (FLAGS_method == "qat") {
        memory_pool = vesal::MemoryPool::GetInstance();
        VESAL_CHECK(memory_pool->Init()) << "memory pool init failure";
    }

    auto zstd_cctx = ZSTD_createCCtx();
    // Software compression level 3~5 is approximately corresponding to QAT compression level 1~9
    // Carefully select the compression level
    if (FLAGS_method == "sw") {
        ZSTD_CCtx_setParameter(zstd_cctx, ZSTD_c_compressionLevel, FLAGS_level);
    } else if (FLAGS_method == "qat") {
        ZSTD_registerSequenceProducer(zstd_cctx, g_zstd_helper.get(), vesal::QatSequenceProducer);
        // make sure it won't fallback to software to conceal failures
        ZSTD_CCtx_setParameter(zstd_cctx, ZSTD_c_enableSeqProducerFallback, 0);
    } else {
        VESAL_LOG(CRITICAL) << "Unknown method";
    }
    if (FLAGS_streaming) {
        ZSTD_CCtx_setParameter(zstd_cctx, ZSTD_c_maxBlockSize, FLAGS_chunk_size);
    }

    uint64_t timecost_ns = 0;
    uint64_t src_index = 0, dst_index = 0, input_size, output_size;
    const unsigned char* src_ptr;
    unsigned char* dst_ptr;
    std::vector<uint64_t> comp_size_vec;
    std::vector<unsigned char*> src_ptr_vec;
    {
        for (src_index = 0; src_index < g_file_size; src_index += FLAGS_chunk_size) {
            input_size = g_file_size - src_index < FLAGS_chunk_size ? g_file_size - src_index
                                                                    : FLAGS_chunk_size;
            if (FLAGS_zerocopy && FLAGS_method == "qat") {
                unsigned char* buf =
                    reinterpret_cast<unsigned char*>(memory_pool->Allocate(input_size));
                memcpy(buf, g_src_data + src_index, input_size);
                src_ptr_vec.push_back(buf);
            } else {
                src_ptr_vec.push_back(g_src_data + src_index);
            }
        }
    }
    SimpleHistogram histogram(0, 10000, 10000);

    for (uint64_t i = 0; i < FLAGS_compression_loop_num; ++i) {
        src_index = 0;
        dst_index = 0;
        for (uint64_t j = 0; src_index < g_file_size; ++j) {
            src_ptr = src_ptr_vec[j];
            dst_ptr = g_dst_data + dst_index;
            input_size = g_file_size - src_index < FLAGS_chunk_size ? g_file_size - src_index
                                                                    : FLAGS_chunk_size;
            auto start_time = std::chrono::steady_clock::now();
            if (FLAGS_streaming) {
                bool lastChunk = g_file_size - src_index <= FLAGS_chunk_size;
                ZSTD_EndDirective const mode = lastChunk ? ZSTD_e_end : ZSTD_e_continue;
                ZSTD_inBuffer input = {src_ptr, input_size, 0};
                ZSTD_outBuffer output = {dst_ptr, g_file_size * 2 - dst_index, 0};
                bool finished;
                do {
                    size_t const remaining = ZSTD_compressStream2(zstd_cctx, &output, &input, mode);
                    /* If we're on the last chunk we're finished when zstd returns 0,
                     * which means its consumed all the input AND finished the frame.
                     * Otherwise, we're finished when we've consumed all the input.
                     */
                    finished = lastChunk ? (remaining == 0) : (input.pos == input.size);
                } while (!finished);
                output_size = output.pos;
            } else {
                output_size = ZSTD_compress2(
                    zstd_cctx, dst_ptr, g_file_size * 2 - dst_index, src_ptr, input_size);
            }
            auto end_time = std::chrono::steady_clock::now();
            uint64_t diff_time =
                std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
            timecost_ns += diff_time;
            histogram.Add(diff_time / 1000.0);
            src_index += input_size;
            dst_index += output_size;
            if (i == 0)
                comp_size_vec.push_back(output_size);
        }
    }
    uint64_t compressed_data_size = dst_index;
    ZSTD_decompress(g_decompressed_data, g_file_size, g_dst_data, compressed_data_size);
    if (!memcmp(g_decompressed_data, g_src_data, g_file_size)) {
        VESAL_LOG(INFO) << "Compression validation succeeded.";
    } else {
        VESAL_LOG(ERROR) << "Compression validation failed.";
    }

    double sec = (double)timecost_ns / 1000 / 1000 / 1000;
    VESAL_LOG(INFO) << "compr(bytes):" << g_file_size << " -> " << compressed_data_size;
    VESAL_LOG(INFO) << "compr throughput:(MB/s)"
                    << (double)FLAGS_compression_loop_num * g_file_size / 1024 / 1024 / sec
                    << ", sec(s):" << sec;
    VESAL_LOG(INFO) << "compr ratio:" << compressed_data_size * 100.0 / g_file_size;
    VESAL_LOG(INFO) << "compr latency(us), avg: " << histogram.GetAvg()
                    << ", max: " << histogram.GetMax() << ", p50: " << histogram.GetPercentage(50)
                    << ", p99: " << histogram.GetPercentage(99)
                    << ", p999: " << histogram.GetPercentage(99.9);

    histogram.Reset();
    memset(g_decompressed_data, 0, g_file_size);
    timecost_ns = 0;
    auto zstd_dctx = ZSTD_createDCtx();
    for (uint64_t i = 0; i < FLAGS_decompression_loop_num; ++i) {
        src_index = 0;
        dst_index = 0;
        for (const auto& comp_size : comp_size_vec) {
            src_ptr = g_dst_data + src_index;
            dst_ptr = g_decompressed_data + dst_index;
            input_size = comp_size;
            auto start_time = std::chrono::steady_clock::now();
            if (FLAGS_streaming) {
                ZSTD_inBuffer input = {src_ptr, input_size, 0};
                ZSTD_outBuffer output = {dst_ptr, g_file_size * 2 - dst_index, 0};
                /* Given a valid frame, zstd won't consume the last byte of the frame
                 * until it has flushed all of the decompressed data of the frame.
                 * Therefore, instead of checking if the return code is 0, we can
                 * decompress just check if input.pos < input.size.
                 */
                while (input.pos < input.size) {
                    ZSTD_decompressStream(zstd_dctx, &output, &input);
                }
                output_size = output.pos;
            } else {
                output_size = ZSTD_decompressDCtx(
                    zstd_dctx, dst_ptr, g_file_size * 2 - dst_index, src_ptr, input_size);
            }
            auto end_time = std::chrono::steady_clock::now();
            uint64_t diff_time =
                std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
            timecost_ns += diff_time;
            histogram.Add(diff_time / 1000.0);
            src_index += input_size;
            dst_index += output_size;
        }
    }
    if (!memcmp(g_decompressed_data, g_src_data, g_file_size)) {
        VESAL_LOG(INFO) << "Decompression validation succeeded.";
    } else {
        VESAL_LOG(ERROR) << "Decompression validation failed.";
    }

    sec = (double)timecost_ns / 1000 / 1000 / 1000;
    VESAL_LOG(INFO) << "decompr throughput:(MB/s)"
                    << (double)FLAGS_decompression_loop_num * g_file_size / 1024 / 1024 / sec
                    << ", sec(s):" << sec;
    VESAL_LOG(INFO) << "decompr latency(us), avg: " << histogram.GetAvg()
                    << ", max: " << histogram.GetMax() << ", p50: " << histogram.GetPercentage(50)
                    << ", p99: " << histogram.GetPercentage(99)
                    << ", p999: " << histogram.GetPercentage(99.9);

    TearDown();
    ZSTD_freeCCtx(zstd_cctx);
    ZSTD_freeDCtx(zstd_dctx);

    auto shutdown_result = vesal::Uninit();
    VESAL_CHECK(shutdown_result);
}