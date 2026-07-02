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

#include "codec/qat/qat_poller_manager.h"

#include "common/defer.h"
#include "common/memory_pool_helper.h"
#include "gtest/gtest.h"
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

TEST(QatPollerManagerTest, Basic) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    auto* m = QatPollerManager::GetInstance();
    m->Init();
    CodecChannelOption option;
    auto* entry = m->GetWorker(option);
    EXPECT_NE(entry, nullptr);
    m->ReturnWorker(entry);
    m->Reset();
}

TEST(QatPollerManagerTest, MultiThread) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    auto* m = QatPollerManager::GetInstance();
    m->Init();
    const int num = 16;
    std::vector<CodecChannelOption> options;
    for (int i = 0; i < num; ++i) {
        CodecChannelOption opt;
        opt.timeout_ms += i;
        options.push_back(opt);
    }
    std::vector<std::thread> ts;
    for (int i = 0; i < num; ++i) {
        std::thread t([&options, &m, i]() {
            auto* entry = m->GetWorker(options[i]);
            EXPECT_NE(entry, nullptr);
            m->ReturnWorker(entry);
        });
        ts.push_back(std::move(t));
    }
    for (int i = 0; i < num; ++i) {
        ts[i].join();
    }
    m->Reset();
}

}  // namespace qat
}  // namespace vesal
