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

#include "codec/qat/qat_poller.h"

#include "codec/qat/qat_codec.h"
#include "common/defer.h"
#include "common/memory_pool_helper.h"
#include "gtest/gtest.h"
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

TEST(QatPollerTest, Basic) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    QatPoller poller(0);
    poller.Start();
    CodecChannelOption option;
    auto* entry = poller.GetOrNewWorker(option);
    EXPECT_NE(entry, nullptr);
    // Check thread name
    auto all_u = g_qat_codec->GetAllUnits();
    for (auto& u : all_u) {
        if (u->GetThreadName() == "qat_poller_0") {
            EXPECT_TRUE(u->IsInUse());
        } else {
            EXPECT_TRUE(u->GetThreadName().empty())
                << u->GetThreadName() << ", size: " << u->GetThreadName().size();
        }
    }
    poller.ReturnWorker(entry);
    poller.Shutdown();
}

TEST(QatPollerTest, SameAndDiffOption) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    QatPoller poller(0);
    poller.Start();
    CodecChannelOption option;
    auto* entry = poller.GetOrNewWorker(option);
    std::vector<QatPollerWorker*> es;
    for (int i = 0; i < 8; ++i) {
        auto* e = poller.GetOrNewWorker(option);
        EXPECT_EQ(entry, e);
        es.push_back(e);
    }
    CodecChannelOption option2;
    option2.comp_algorithm = CodecAlgorithm::kDeflate;
    auto* entry2 = poller.GetOrNewWorker(option2);
    for (int i = 0; i < 8; ++i) {
        auto* e = poller.GetOrNewWorker(option2);
        EXPECT_EQ(entry2, e);
        es.push_back(e);
    }
    EXPECT_EQ(poller.GetRefCnt(), 18);
    EXPECT_EQ(poller.GetWorkerNum(), 2);
    poller.ReturnWorker(entry);
    for (int i = 0; i < 8; ++i) {
        poller.ReturnWorker(entry);
    }
    poller.ReturnWorker(entry2);
    for (int i = 0; i < 8; ++i) {
        poller.ReturnWorker(entry2);
    }
    EXPECT_EQ(poller.GetRefCnt(), 0);
    EXPECT_EQ(poller.GetWorkerNum(), 0);
    poller.Shutdown();
}

}  // namespace qat
}  // namespace vesal
