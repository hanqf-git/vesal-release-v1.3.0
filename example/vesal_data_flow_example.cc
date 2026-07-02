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

#include <unistd.h>
#include <cstring>
#include <memory>

#include "gflags/gflags.h"
#include "vesal/data_flow.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    // Init vesal
    FLAGS_vesal_log_console_output = true;
    VESAL_LOG(INFO) << "Start to init vesal";
    vesal::InitOptions init_option;
    init_option.codec_init_opt.init_qat = false;
    init_option.data_flow_init_opt.init_dsa = true;
    init_option.cypher_init_opt.init_qat = false;
    VESAL_CHECK(vesal::Init(init_option)) << "Init vesal failed";
    VESAL_LOG(INFO) << "Init vesal success";

    // Create channel
    VESAL_LOG(INFO) << "Start to create data flow channel";
    vesal::DataFlowChannelOptions dsa_opts;
    dsa_opts.engine_type = vesal::DataFlowEngineType::kDsa;
    auto dsa_result = vesal::DataFlowChannel::CreateDataFlowChannel(dsa_opts);
    VESAL_CHECK(dsa_result.first == vesal::StatusCode::kOk) << "Create dsa channel failed";
    auto dsa_channel = std::move(dsa_result.second);
    VESAL_LOG(INFO) << "Create dsa channel success";

    vesal::DataFlowChannelOptions sw_opts;
    sw_opts.engine_type = vesal::DataFlowEngineType::kSoftware;
    auto sw_result = vesal::DataFlowChannel::CreateDataFlowChannel(sw_opts);
    VESAL_CHECK(sw_result.first == vesal::StatusCode::kOk) << "Create sw channel failed";
    auto sw_channel = std::move(sw_result.second);
    VESAL_LOG(INFO) << "Create sw channel success";

    // Prepare data
    VESAL_LOG(INFO) << "Start to prepare data buffer with vesal memory pool for zero copy";
    unsigned int N = 10, data_len = 4096;
    std::vector<unsigned char*> inputs(N), outputs(N);
    for (int i = 0; i < N; i++) {
        inputs[i] = (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(data_len);
        VESAL_CHECK(inputs[i]) << "MemoryPool allocate failed";
        outputs[i] = (unsigned char*)vesal::MemoryPool::GetInstance()->Allocate(data_len);
        VESAL_CHECK(outputs[i]) << "MemoryPool allocate failed";
        memset(inputs[i], 'a' + i, data_len);
    }
    VESAL_LOG(INFO) << "Prepare data buffer success";

    VESAL_LOG(INFO) << "Start to submit move with crc request to dsa channel";
    // dsa engine move with crc example
    vesal::DataFlowResult move_with_crc_results;
    vesal::DataFlowMoveOperation move_with_crc_ops[N];
    for (int i = 0; i < N; i++) {
        move_with_crc_ops[i].src = {inputs[i]};
        move_with_crc_ops[i].src_len = {data_len};
        move_with_crc_ops[i].dst = outputs[i];
        move_with_crc_ops[i].enable_crc = true;
        move_with_crc_ops[i].seed = 0;
    }
    // submit all requests as 1 batch
    auto submit_res = dsa_channel->SubmitMove(move_with_crc_ops, N, nullptr);
    VESAL_CHECK(submit_res == vesal::StatusCode::kOk);
    VESAL_LOG(INFO) << "Succeeded to submit move with crc request to dsa channel";
    VESAL_LOG(INFO) << "Start to poll results from dsa channel";
    ssize_t poll_res = 0;
    while (poll_res == 0) {
        // try to poll 1 batch
        poll_res = dsa_channel->Poll(&move_with_crc_results, 1, -1);
        sleep(1);
    }
    VESAL_CHECK(poll_res == 1) << "failed to poll move with crc results";
    VESAL_LOG(INFO) << "Succeeded to poll results from dsa channel";

    VESAL_LOG(INFO) << "Start to submit crc request to sw channel";
    // sw engine crc example
    vesal::DataFlowResult crc_results;
    vesal::DataFlowCrcOperation crc_ops[N];
    for (int i = 0; i < N; i++) {
        crc_ops[i].src = inputs[i];
        crc_ops[i].len = data_len;
        crc_ops[i].seed = 0;
    }
    // submit all requests as 1 batch
    submit_res = sw_channel->SubmitCrc(crc_ops, N, nullptr);
    VESAL_CHECK(submit_res == vesal::StatusCode::kOk);
    VESAL_LOG(INFO) << "Succeeded to submit crc request to sw channel";
    VESAL_LOG(INFO) << "Start to poll results from sw channel";
    poll_res = 0;
    while (poll_res == 0) {
        // try to poll 1 batch
        poll_res = sw_channel->Poll(&crc_results, 1, -1);
        sleep(1);
    }
    VESAL_CHECK(poll_res == 1) << "failed to poll crc results";
    VESAL_LOG(INFO) << "Succeeded to poll results from sw channel";

    // check results
    VESAL_LOG(INFO) << "Start to check results";
    for (int i = 0; i < N; i++) {
        VESAL_CHECK(memcmp(inputs[i], outputs[i], data_len) == 0) << "memcpy check failed";
        VESAL_CHECK(crc_results.crc_output[i] == move_with_crc_results.crc_output[i])
            << "crc check failed";
        VESAL_CHECK(crc_results.crc_output[i] != 0) << "crc check failed";
    }
    VESAL_LOG(INFO) << "Succeeded to check memcpy and crc results";

    // Teardown
    VESAL_LOG(INFO) << "Start to close channel and exit";
    auto r = dsa_channel->Close();
    VESAL_CHECK(r == vesal::StatusCode::kOk) << "Close dsa channel failed, status code: " << r;
    r = sw_channel->Close();
    VESAL_CHECK(r == vesal::StatusCode::kOk) << "Close sw channel failed, status code: " << r;
    VESAL_CHECK(vesal::Uninit()) << "Uninit failed";
    return 0;
}