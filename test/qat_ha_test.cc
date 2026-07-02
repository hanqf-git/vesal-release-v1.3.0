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

#include "codec/qat/qat_ha.h"

#include "codec/qat/qat_codec_engine.h"
#include "common/defer.h"
#include "common/err_simulation.h"
#include "common/memory_pool_helper.h"
#include "common/uds_listener.h"
#include "gtest/gtest.h"

namespace vesal {
namespace qat {

#define RAII_PROTECTED_WINDOW_FLAGS                           \
    auto o1 = FLAGS_vesal_qat_ha_min_counting_time_window_us; \
    auto o2 = FLAGS_vesal_qat_ha_sliding_time_window_sec;     \
    auto o3 = FLAGS_vesal_qat_ha_trigger_error_num;           \
    auto __flags_guard = defer([&]() {                        \
        FLAGS_vesal_qat_ha_min_counting_time_window_us = o1;  \
        FLAGS_vesal_qat_ha_sliding_time_window_sec = o2;      \
        FLAGS_vesal_qat_ha_trigger_error_num = o3;            \
    });

void SetWindowFlags() {
    FLAGS_vesal_qat_ha_sliding_time_window_sec = 10;
    FLAGS_vesal_qat_ha_min_counting_time_window_us = 50 * 1000;
    FLAGS_vesal_qat_ha_trigger_error_num = 5;
}

inline void InitCodecWithHa() {
    vesal::InitOptions opts;
    opts.data_flow_init_opt.init_dsa = false;
    opts.codec_init_opt.init_qat = true;
    EXPECT_TRUE(vesal::Init(opts));
    vesal::AddressManager::t_tls_memory_info_by_vaddr_.clear();
}

inline void DestructCodec() {
    vesal::Uninit();
}

// Test the JudgeHaErrorLevel function
TEST(JudgeHaErrorLevelTest, BasicTest) {
    {
        StatusCode status = StatusCode::kOk;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kNotHandle);
    }
    {
        StatusCode status = StatusCode::kShouldRetry;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kAccumulate);
    }
    {
        StatusCode status = StatusCode::kInvalidArgument;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kNotHandle);
    }
    {
        StatusCode status = StatusCode::kNotSupported;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kNotHandle);
    }

    {
        StatusCode status = StatusCode::kOverflow;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kNotHandle);
    }

    {
        StatusCode status = StatusCode::kBadData;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kNotHandle);
    }
    {
        StatusCode status = StatusCode::kResourceBusy;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kAccumulate);
    }
    {
        StatusCode status = StatusCode::kTimeout;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kAccumulate);
    }

    {
        StatusCode status = StatusCode::kHardwareError;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kImmediately);
    }

    {
        StatusCode status = StatusCode::kChannelError;
        QatHaErrorLevel error_level = JudgeHaErrorLevel(status);
        EXPECT_EQ(error_level, QatHaErrorLevel::kImmediately);
    }
}

TEST(SlidingTimeWindowTest, Basic) {
    RAII_PROTECTED_WINDOW_FLAGS
    SetWindowFlags();
    SlidingTimeWindow window;
    window.Reset();
    EXPECT_EQ(window.GetErrorNum(), 0);
    EXPECT_TRUE(window.RecordErrorAndCheckIsSafe());
    EXPECT_EQ(window.GetErrorNum(), 1);
}

TEST(SlidingTimeWindowTest, TooQuickErrors) {
    RAII_PROTECTED_WINDOW_FLAGS
    SetWindowFlags();
    SlidingTimeWindow window;
    window.Reset();
    EXPECT_EQ(window.GetErrorNum(), 0);
    auto start = TimeStamp::Now();
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(window.RecordErrorAndCheckIsSafe())
            << "i=" << i << " window.GetErrorNum()=" << window.GetErrorNum();
    }
    auto end = TimeStamp::Now();
    VESAL_LOG(INFO) << "Avg: " << TimeStamp::DurationToUs(end - start) / 10.0 << "us";
    EXPECT_EQ(window.GetErrorNum(), 1);
}

TEST(SlidingTimeWindowTest, TriggerErrorAndReset) {
    RAII_PROTECTED_WINDOW_FLAGS
    SetWindowFlags();
    SlidingTimeWindow window;
    EXPECT_EQ(window.GetErrorNum(), 0);
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(window.RecordErrorAndCheckIsSafe());
        usleep(50 * 1000);
    }
    EXPECT_EQ(window.GetErrorNum(), 4);
    EXPECT_FALSE(window.RecordErrorAndCheckIsSafe());
    EXPECT_EQ(window.GetErrorNum(), 5);
    usleep(1000);
    EXPECT_FALSE(window.RecordErrorAndCheckIsSafe());
    window.Reset();
    EXPECT_EQ(window.GetErrorNum(), 0);
}

TEST(QatHaTest, Basic) {
    RAII_PROTECTED_WINDOW_FLAGS
    SetWindowFlags();
    auto retry_err = StatusCode::kShouldRetry;
    auto ok = StatusCode::kOk;
    auto not_handle = StatusCode::kInvalidArgument;
    auto immediate = StatusCode::kHardwareError;
    auto accum = StatusCode::kTimeout;
    {
        HaPolicy ha_policy = HaPolicy::kHardware;
        QatHa ha(ha_policy);
        EXPECT_FALSE(ha.ShouldFallback(retry_err));
        EXPECT_FALSE(ha.ShouldFallback(ok));
        EXPECT_FALSE(ha.ShouldFallback(not_handle));
        EXPECT_TRUE(ha.ShouldFallback(immediate));
        EXPECT_FALSE(ha.ShouldFallback(accum));
        int cnt = 10;
        while (!ha.ShouldFallback(accum) && cnt-- > 0) {
            usleep(50 * 1000);
        }
        EXPECT_GT(cnt, 0);
    }
}

#ifdef VESAL_ENABLE_ERR_SIM
// We need error sim for fallback test.
void SendError(uint8_t selection,
               QatErrSimType phase,
               QatErrSimCode err,
               QatErrSimCnt cnt,
               int pf_id = -1,
               int vf_id = -1,
               int inst_id = -1) {
    auto flag = PackQatErrSimUdsFlags(selection, VESAL_QAT_ERR_SIM_FLAGS_OP_INJECT);
    std::string msg = PackQatErrSimUdsMsg(
        flag, pf_id, vf_id, inst_id, phase, static_cast<QatErrSimCode>(err), cnt);
    std::string resp;
    EXPECT_TRUE(
        WriteUdsAndReadResponse(GetQatUdsSocketPath(g_vesal_codec_qat_section_name), msg, &resp));
}

TEST(QatHaTest, SubmitImmediateErrorAndFallbackOnce) {
    FLAGS_vesal_enable_err_sim = true;
    FLAGS_vesal_log_console_output = true;
    InitCodecWithHa();
    auto __g = defer([&]() {
        FLAGS_vesal_enable_err_sim = false;
        FLAGS_vesal_log_console_output = false;
    });
    CodecChannelOption channel_opts;
    channel_opts.timeout_ms = 3600 * 1000;
    channel_opts.ha_policy = HaPolicy::kHardware;
    auto channel_r = CreateQatCodecEngine(channel_opts);
    EXPECT_TRUE(channel_r.first.ok());
    auto* qc = channel_r.second.get();
    auto* qu = qc->GetQatUnit();
    EXPECT_NE(qc, nullptr);
    SendError(VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_INST,
              QatErrSimType::kSubmit,
              static_cast<QatErrSimCode>(StatusCode::kHardwareError),
              1,
              qu->GetDeviceId(),
              qu->GetFunctionId(),
              qu->GetInstId());
    usleep(5000);
    // Send now and trigger fallback
    unsigned char* src = new unsigned char[4096];
    unsigned char* dst = new unsigned char[4096];
    auto compress =
        qc->SubmitAsyncRequest(CodecDirection::kComp, {src}, {4096}, dst, 4096, nullptr);
    EXPECT_FALSE(IsChannelError(compress));
    usleep(500);
    CodecResult res;
    auto poll = qc->Poll(&res, 1, -1);
    EXPECT_EQ(poll, 0);
    auto close = qc->Close();
    EXPECT_TRUE(close.ok()) << close;
    DestructCodec();
    delete[] src;
    delete[] dst;
}

TEST(QatHaTest, SubmitAccumulateError) {
    FLAGS_vesal_enable_err_sim = true;
    FLAGS_vesal_log_console_output = true;
    InitCodecWithHa();
    auto __g = defer([&]() {
        FLAGS_vesal_enable_err_sim = false;
        FLAGS_vesal_log_console_output = false;
    });
    CodecChannelOption channel_opts;
    channel_opts.timeout_ms = 3600 * 1000;
    channel_opts.ha_policy = HaPolicy::kHardware;
    auto channel_r = CreateQatCodecEngine(channel_opts);
    EXPECT_TRUE(channel_r.first.ok());
    auto* qc = channel_r.second.get();
    auto* qu = qc->GetQatUnit();
    EXPECT_NE(qc, nullptr);
    SendError(VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_INST,
              QatErrSimType::kSubmit,
              static_cast<QatErrSimCode>(StatusCode::kResourceBusy),
              FLAGS_vesal_qat_ha_trigger_error_num,
              qu->GetDeviceId(),
              qu->GetFunctionId(),
              qu->GetInstId());
    usleep(5000);
    size_t src_sz = 4096;
    size_t dst_sz = src_sz * 2;
    unsigned char* src = new unsigned char[src_sz];
    unsigned char* dst = new unsigned char[dst_sz];
    for (size_t i = 0; i < FLAGS_vesal_qat_ha_trigger_error_num - 1; ++i) {
        auto r = qc->SubmitAsyncRequest(CodecDirection::kComp,
                                        {src},
                                        {static_cast<unsigned int>(src_sz)},
                                        dst,
                                        dst_sz,
                                        nullptr);
        EXPECT_EQ(r, StatusCode::kResourceBusy);
        usleep(FLAGS_vesal_qat_ha_min_counting_time_window_us);
    }
    auto r = qc->SubmitAsyncRequest(
        CodecDirection::kComp, {src}, {static_cast<unsigned int>(src_sz)}, dst, dst_sz, nullptr);
    EXPECT_FALSE(IsPermanentError(r));  // Now HA triggered

    r = qc->SubmitAsyncRequest(
        CodecDirection::kComp, {src}, {static_cast<unsigned int>(src_sz)}, dst, dst_sz, nullptr);
    EXPECT_EQ(r, StatusCode::kOk);
    usleep(1000);
    CodecResult res;
    auto poll = qc->Poll(&res, 1, -1);
    EXPECT_EQ(poll, 1);
    EXPECT_EQ(res.status, StatusCode::kOk);
    auto close = qc->Close();
    EXPECT_TRUE(close.ok()) << close;
    DestructCodec();
    delete[] src;
    delete[] dst;
}

TEST(QatHaTest, NoAvailableDevCanBeChosen) {
    FLAGS_vesal_enable_err_sim = true;
    FLAGS_vesal_log_console_output = true;
    InitCodecWithHa();
    auto __g = defer([&]() {
        FLAGS_vesal_enable_err_sim = false;
        FLAGS_vesal_log_console_output = false;
    });
    CodecChannelOption channel_opts;
    channel_opts.timeout_ms = 3600 * 1000;
    channel_opts.ha_policy = HaPolicy::kHardware;
    auto channel_r = CreateQatCodecEngine(channel_opts);
    EXPECT_TRUE(channel_r.first.ok());
    auto* qc = channel_r.second.get();
    EXPECT_NE(qc, nullptr);
    SendError(VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL,
              QatErrSimType::kPoll,
              static_cast<QatErrSimCode>(StatusCode::kHardwareError),
              1,
              -1,
              -1,
              -1);
    usleep(1000);
    CodecResult res;
    auto poll_r = 0;
    int i = 4;
    while (i--) {
        poll_r = qc->Poll(&res, 1, -1);
        EXPECT_EQ(poll_r, 0);
    }
    // Now channel is fatal
    unsigned char* src = new unsigned char[4096];
    unsigned char* dst = new unsigned char[4096];
    auto r = qc->SubmitAsyncRequest(CodecDirection::kComp, {src}, {4096}, dst, 4096, nullptr);
    EXPECT_TRUE(IsPermanentError(r)) << r;
    delete[] src;
    delete[] dst;
    auto close = qc->Close();
    EXPECT_TRUE(close.ok()) << close;
    DestructCodec();
}

struct Req {
    unsigned char src[4096];
    unsigned char dst[8192];
};

TEST(QatHaTest, FlushInflightReq) {
    FLAGS_vesal_enable_err_sim = true;
    FLAGS_vesal_log_console_output = true;
    InitCodecWithHa();
    auto __g = defer([&]() {
        FLAGS_vesal_enable_err_sim = false;
        FLAGS_vesal_log_console_output = false;
    });
    CodecChannelOption channel_opts;
    channel_opts.timeout_ms = 3600 * 1000;
    channel_opts.ha_policy = HaPolicy::kHardware;
    auto channel_r = CreateQatCodecEngine(channel_opts);
    EXPECT_TRUE(channel_r.first.ok());
    auto* qc = channel_r.second.get();
    auto* qu = qc->GetQatUnit();
    EXPECT_NE(qc, nullptr);

    std::vector<Req> reqs;
    for (int i = 0; i < 4; ++i) {
        reqs.push_back(Req());
        auto r = qc->SubmitAsyncRequest(
            CodecDirection::kComp, {reqs[i].src}, {4096}, reqs[i].src, 8192, nullptr);
        EXPECT_EQ(r, StatusCode::kOk);
    }

    SendError(VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_INST,
              QatErrSimType::kSubmit,
              static_cast<QatErrSimCode>(StatusCode::kHardwareError),
              1,
              qu->GetDeviceId(),
              qu->GetFunctionId(),
              qu->GetInstId());
    usleep(1000);
    Req req;
    auto r =
        qc->SubmitAsyncRequest(CodecDirection::kComp, {req.src}, {4096}, req.dst, 8192, nullptr);
    EXPECT_FALSE(IsPermanentError(r));
    CodecResult res[4];
    auto n = qc->Poll(res, 4, -1);
    EXPECT_EQ(n, 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(res[i].status, StatusCode::kDropped);
    }
    // Then the channel can be used normally.
    for (int i = 0; i < 4; ++i) {
        auto r = qc->SubmitAsyncRequest(
            CodecDirection::kComp, {reqs[i].src}, {4096}, reqs[i].dst, 8192, nullptr);
        EXPECT_EQ(r, StatusCode::kOk);
    }
    usleep(1000);
    n = qc->Poll(res, 4, -1);
    EXPECT_EQ(n, 4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(res[i].status, StatusCode::kOk);
    }
    auto close = qc->Close();
    EXPECT_TRUE(close.ok()) << close;
    DestructCodec();
}

TEST(QatHaTest, RunTimeTest) {
    FLAGS_vesal_enable_err_sim = true;
    FLAGS_vesal_log_console_output = true;
    InitCodecWithHa();
    auto __g = defer([&]() {
        FLAGS_vesal_enable_err_sim = false;
        FLAGS_vesal_log_console_output = false;
    });
    CodecChannelOption channel_opts;
    channel_opts.timeout_ms = 3600 * 1000;
    channel_opts.ha_policy = HaPolicy::kHardware;
    auto channel_r = CreateQatCodecEngine(channel_opts);
    EXPECT_TRUE(channel_r.first.ok());
    auto* qc = channel_r.second.get();
    auto* qu = qc->GetQatUnit();
    EXPECT_NE(qc, nullptr);

    std::atomic<bool> fin{false};
    std::thread t([&]() {
        for (int i = 0; i < 3; ++i) {
            usleep(1000 * 1000);
            QatErrSimType type = QatErrSimType::kSubmit;
            if (i == 1) {
                type = QatErrSimType::kPoll;
            } else {
                type = QatErrSimType::kResult;
            }
            SendError(VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_INST,
                      type,
                      static_cast<QatErrSimCode>(StatusCode::kHardwareError),
                      FLAGS_vesal_qat_ha_trigger_error_num,
                      qu->GetDeviceId(),
                      qu->GetFunctionId(),
                      qu->GetInstId());
        }
        fin = true;
    });
    while (!fin) {
        std::vector<Req> reqs(4);
        int i = 0;
        while (i < 4) {
            reqs.push_back(Req());
            auto r = qc->SubmitAsyncRequest(
                CodecDirection::kComp, {reqs[i].src}, {4096}, reqs[i].dst, 8192, nullptr);
            EXPECT_FALSE(IsPermanentError(r));
            if (IsOk(r)) {
                ++i;
            }
        }
        int n = 0;
        CodecResult res[4];
        while (n < 4) {
            auto poll = qc->Poll(res, 4, -1);
            EXPECT_GE(poll, 0);
            n += poll;
        }
    }
    SendError(VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_INST,
              QatErrSimType::kResult,
              static_cast<QatErrSimCode>(StatusCode::kResourceBusy),
              FLAGS_vesal_qat_ha_trigger_error_num - 1,
              qu->GetDeviceId(),
              qu->GetFunctionId(),
              qu->GetInstId());
    const size_t total = 16;
    Req r[16];
    for (size_t i = 0; i < total; ++i) {
        auto sr = qc->SubmitAsyncRequest(
            CodecDirection::kComp, {r[i].src}, {4096}, r[i].dst, 4096, nullptr);
        EXPECT_FALSE(IsPermanentError(sr));
    }
    usleep(500 * 1000);
    CodecResult res[total];
    auto pr = qc->Poll(res, total, -1);
    EXPECT_EQ(pr, total);
    for (size_t i = 0; i < total; ++i) {
        EXPECT_TRUE(IsShouldRetry(res[i].status) || IsOk(res[i].status));
    }
    qc->Close();
    auto close = qc->Close();
    EXPECT_TRUE(close.ok()) << close;
    DestructCodec();
    t.join();
}
#endif  // VESAL_ENABLE_ERR_SIM

}  // namespace qat
}  // namespace vesal
