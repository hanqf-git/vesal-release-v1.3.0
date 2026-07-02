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

#include "codec/qat/qat_poller_worker.h"

#include <thread>

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

struct CodecReq {
    CodecReq(unsigned int src_len, unsigned int dst_len)
        : src_len(src_len),
          dst_len(dst_len),
          src(new unsigned char[src_len]),
          dst(new unsigned char[dst_len]) {
        res.status = StatusCode::kUnknown;
    }

    bool FinAndOk() {
        return res.status == StatusCode::kOk;
    }

    unsigned int src_len;
    unsigned int dst_len;
    std::unique_ptr<unsigned char[]> src;
    std::unique_ptr<unsigned char[]> dst;

    CodecResult res;
};

TEST(QatPollerWorkerTest, Basic) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    QatPollerWorker entry;
    CodecChannelOption opts;
    auto r = entry.Init(opts, -1);
    EXPECT_TRUE(IsOk(r)) << r;
    entry.Uninit();
}

TEST(QatPollerWorkerTest, Schedule) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    QatPollerWorker entry;
    CodecChannelOption opts;
    auto user_cb = [](const CodecResult& res) {
        auto* req = static_cast<CodecReq*>(res.ctx);
        req->res = res;
    };
    auto r = entry.Init(opts, -1);
    EXPECT_TRUE(IsOk(r)) << r;
    std::unique_ptr<CodecReq> req[16];
    for (int i = 0; i < 16; ++i) {
        req[i] = std::make_unique<CodecReq>(4096, 4096 * 2);
    }
    for (auto& each : req) {
        std::thread t([&]() {
            auto r = entry.Schedule({each->src.get()},
                                    {each->src_len},
                                    each->dst.get(),
                                    each->dst_len,
                                    each.get(),
                                    CodecDirection::kComp,
                                    user_cb);
            EXPECT_TRUE(IsOk(r)) << r;
        });
        t.detach();
    }
    // Wait the reqs enqueued.
    usleep(1000);
    entry.LoopOnce(16, 16);
    usleep(1000);
    entry.LoopOnce(16, 16);
    for (auto& each : req) {
        EXPECT_TRUE(each->FinAndOk()) << each->res.status;
    }
    // Now for decompress
    std::unique_ptr<CodecReq> decomp_reqs[16];
    for (int i = 0; i < 16; ++i) {
        decomp_reqs[i] = std::make_unique<CodecReq>(req[i]->res.produced, req[i]->src_len);
        memcpy(decomp_reqs[i]->src.get(), req[i]->dst.get(), req[i]->res.produced);
    }
    for (auto& each : decomp_reqs) {
        std::thread t([&]() {
            auto r = entry.Schedule({each->src.get()},
                                    {each->src_len},
                                    each->dst.get(),
                                    each->dst_len,
                                    each.get(),
                                    CodecDirection::kDecomp,
                                    user_cb);
            EXPECT_TRUE(IsOk(r)) << r;
        });
        t.detach();
    }
    // Wait the reqs enqueued.
    usleep(1000);
    entry.LoopOnce(16, 16);
    usleep(1000);
    entry.LoopOnce(16, 16);
    for (auto& each : decomp_reqs) {
        EXPECT_TRUE(each->FinAndOk()) << each->res.status;
    }
    entry.Uninit();
}

size_t g_thread_0_index;
size_t g_thread_1_index;
TEST(QatPollerWorkerTest, Ordered) {
    RAII_QAT_INIT_AND_SHUTDONW(true);
    g_thread_0_index = 1;
    g_thread_1_index = 0;
    QatPollerWorker entry;
    CodecChannelOption opts;
    auto user_cb = [](const CodecResult& res) {
        auto index = reinterpret_cast<uintptr_t>(res.ctx);
        EXPECT_EQ(res.status, StatusCode::kOk) << res.status;
        if (index & 1) {
            EXPECT_EQ(index, g_thread_0_index);
            g_thread_0_index += 2;
        } else {
            EXPECT_EQ(index, g_thread_1_index);
            g_thread_1_index += 2;
        }
    };
    auto r = entry.Init(opts, -1);
    EXPECT_TRUE(IsOk(r)) << r;
    std::unique_ptr<CodecReq> req[16];
    for (int i = 0; i < 16; ++i) {
        req[i] = std::make_unique<CodecReq>(4096, 4096 * 2);
    }
    // Prepare two threads
    std::thread t_0([&]() {
        for (uintptr_t i = 0; i < 16; ++i) {
            if (i % 2 == 0) {
                continue;
            }
            auto r = entry.Schedule({req[i]->src.get()},
                                    {req[i]->src_len},
                                    req[i]->dst.get(),
                                    req[i]->dst_len,
                                    reinterpret_cast<void*>(i),
                                    CodecDirection::kComp,
                                    user_cb);
            EXPECT_TRUE(IsOk(r)) << r;
        }
    });
    std::thread t_1([&]() {
        for (uintptr_t i = 0; i < 16; ++i) {
            if (i % 2 == 1) {
                continue;
            }
            auto r = entry.Schedule({req[i]->src.get()},
                                    {req[i]->src_len},
                                    req[i]->dst.get(),
                                    req[i]->dst_len,
                                    reinterpret_cast<void*>(i),
                                    CodecDirection::kComp,
                                    user_cb);
            EXPECT_TRUE(IsOk(r)) << r;
        }
    });
    t_0.join();
    t_1.join();
    // Wait the reqs enqueued.
    usleep(1000);
    entry.LoopOnce(16, 16);
    usleep(1000);
    entry.LoopOnce(16, 16);
    entry.Uninit();
}

}  // namespace qat
}  // namespace vesal
