/*
 * Copyright (c) 2024 ByteDance Inc.
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
#include <memory>

#include "gflags/gflags.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    // Init vesal
    FLAGS_vesal_log_console_output = true;
    VESAL_LOG(INFO) << "Start to init vesal with default options";
    vesal::InitOptions init_option;
    init_option.codec_init_opt.init_qat = true;
    init_option.cypher_init_opt.init_qat = false;
    init_option.data_flow_init_opt.init_dsa = false;
    init_option.mem_pool_init_opt.init_mem_pool = true;
    VESAL_CHECK(vesal::Init(init_option)) << "Init vesal failed";
    VESAL_LOG(INFO) << "Init vesal success";

    // Create channel
    VESAL_LOG(INFO) << "Start to create codec channel with default options";
    vesal::CodecChannelOption chan_option;
    auto res = vesal::CodecChannel::CreateCodecChannel(chan_option);
    VESAL_CHECK(res.first.ok()) << "Create codec channel failed";
    std::unique_ptr<vesal::CodecChannel> channel = std::move(res.second);
    VESAL_LOG(INFO) << "Create codec channel success";

    // Prepare data
    VESAL_LOG(INFO) << "Start to prepare data buffer with vesal memory pool for zero copy";
    unsigned int N = 10, input_len = 4096, output_len = 8192;
    unsigned int actual_output_len = 118;  // Magic number(result of compressing 4096 same char)
    vesal::CodecResult results[N];
    std::vector<unsigned char*> inputs(N), outputs(N);
    for (int i = 0; i < N; i++) {
        inputs[i] = (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(input_len);
        VESAL_CHECK(inputs[i]) << "MemoryPool allocate failed";
        outputs[i] = (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(output_len);
        VESAL_CHECK(outputs[i]) << "MemoryPool allocate failed";
        memset(inputs[i], 'a' + i, input_len);
    }
    VESAL_LOG(INFO) << "Prepare data buffer success";

    // Submit compress requests
    VESAL_LOG(INFO) << "Start to submit compress requests";
    for (int i = 0; i < N; i++) {
        auto r = channel->CompressAsync(inputs[i], input_len, outputs[i], output_len, nullptr);
        VESAL_CHECK(vesal::IsOk(r)) << "Submit compress request failed, status code: " << r;
    }
    VESAL_LOG(INFO) << "Submit compress requests success";

    // Poll and check results
    VESAL_LOG(INFO) << "Start to poll and check results";
    for (size_t n; n < N;) {
        auto r = channel->Poll(results, N, -1);
        VESAL_CHECK(r != -1) << "Poll failed";
        for (size_t i = 0; i < r; i++) {
            VESAL_CHECK(vesal::IsOk(results[i].status))
                << "Compress failed, status code: " << results[i].status;
            VESAL_CHECK(results[i].consumed == input_len)
                << "Consumed size not match, expected: " << input_len
                << ", actual: " << results[i].consumed;
            VESAL_CHECK(results[i].produced == actual_output_len)
                << "Produced size not match, expected: " << actual_output_len
                << ", actual: " << results[i].produced;
            VESAL_LOG(INFO) << "Compress success, consumed size: " << results[i].consumed
                            << ", produced size: " << results[i].produced;
        }
        n += r;
    }

    // Teardown
    VESAL_LOG(INFO) << "Start to close channel and exit";
    auto r = channel->Close();
    VESAL_CHECK(r.ok()) << "Close channel failed, status code: " << r;
    VESAL_CHECK(vesal::Uninit()) << "Uninit failed";
    return 0;
}