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

#include "common/err_simulation.h"

#include <gtest/gtest.h>

#include <vector>

#include "codec/codec_internal.h"
#include "codec/qat/qat_codec.h"
#include "codec/qat/qat_error_handling.h"
#include "common/defer.h"
#include "common/memory_pool_helper.h"
#include "common/timestamp.h"
#include "common/uds_listener.h"
#include "ut_util.h"

namespace vesal {

extern std::unique_ptr<UdsListener> g_uds_listener;

namespace qat {

QatUnitManager* DryInitQatCodec(int inst_num) {
    g_qat_codec = std::make_unique<QatCodec>();
    auto* qat_codec = dynamic_cast<QatCodec*>(g_qat_codec.get());
    for (int i = 1; i <= inst_num; ++i) {
        QatUnit* unit = new QatUnit(nullptr);
        unit->qat_unit_attr_.inst_id = i;
        unit->qat_unit_attr_.function_id = i;
        unit->qat_unit_attr_.device_id = i % 2;
        qat_codec->unit_manager_->all_units_.emplace_back(unit);
    }
    return qat_codec->unit_manager_.get();
}

void ResetQatCodec() {
    g_qat_codec = nullptr;
}

class QatErrSimulationTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        VESAL_INIT_CODEC_QAT_ONLY
    }

    void TearDown() override {
        VESAL_UNINIT
    }
};

TEST_F(QatErrSimulationTestFixture, PackAndParse) {
    uint8_t pf_id = 1;
    uint8_t vf_id = 2;
    uint8_t inst_id = 3;
    QatErrSimType type = QatErrSimType::kSubmit;
    QatErrSimCode code = 1;
    QatErrSimCnt cnt = 100;
    uint8_t flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_PF;

    std::string msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    auto* unit_manager = DryInitQatCodec(20);
    auto status = ErrSimHandler(msg);
    EXPECT_TRUE(status.ok()) << status;
    QatUnitSelection selection;
    selection.pf_id = pf_id;
    auto dev1 = unit_manager->LookupUnits(selection);
    selection.pf_id++;
    auto dev2 = unit_manager->LookupUnits(selection);
    for (auto unit : dev1) {
        EXPECT_EQ(unit->qat_err_sim_->GetErr(type).first, code);
    }
    for (auto unit : dev2) {
        EXPECT_EQ(unit->qat_err_sim_->GetErr(type).first, VESAL_QAT_ERR_SIM_OK);
    }
    ResetQatCodec();
}

TEST_F(QatErrSimulationTestFixture, ParseInvalidMsg) {
    std::string msg = "invalid msg";
    Status r;
    r = ErrSimHandler(msg);
    EXPECT_FALSE(r.ok()) << r;
    msg = std::string(VESAL_ERR_SIM_UDS_MSG_LEN, 'a');
    r = ErrSimHandler(msg);
    EXPECT_FALSE(r.ok()) << r;

    uint8_t pf_id = 1;
    uint8_t vf_id = 2;
    uint8_t inst_id = 3;
    QatErrSimType type = QatErrSimType::kSubmit;
    QatErrSimCode code = 1;
    QatErrSimCnt cnt = 100;
    uint8_t flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_PF;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    r = ErrSimHandler(msg);
    // QAT not enabled yet
    EXPECT_TRUE(IsInvalidArgument(r)) << r;
    code = VESAL_QAT_ERR_SIM_TIMEOUT_ERROR;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    r = ErrSimHandler(msg);
    EXPECT_TRUE(IsInvalidArgument(r)) << r;
    code = 1;

    DryInitQatCodec(20);
    pf_id = 128;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    r = ErrSimHandler(msg);
    EXPECT_FALSE(r.ok()) << r;
    pf_id = 1;
    cnt = 0;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    r = ErrSimHandler(msg);
    EXPECT_FALSE(r.ok()) << r;
    code = VESAL_QAT_ERR_SIM_TIMEOUT_ERROR;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    r = ErrSimHandler(msg);
    EXPECT_TRUE(IsInvalidArgument(r)) << r;
    ResetQatCodec();
}

TEST_F(QatErrSimulationTestFixture, ClearErrAuto) {
    uint8_t pf_id = 1;
    uint8_t vf_id = 2;
    uint8_t inst_id = 3;
    QatErrSimType type = QatErrSimType::kSubmit;
    QatErrSimCode code = 1;
    QatErrSimCnt cnt = 100;
    uint8_t flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_PF;
    std::string msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    auto* pool = DryInitQatCodec(1);

    auto status = ErrSimHandler(msg);
    EXPECT_TRUE(status.ok()) << status;
    QatUnitSelection selc;
    selc.pf_id = pf_id;
    auto dev1 = pool->LookupUnits(selc);
    EXPECT_EQ(dev1.size(), 1);
    for (size_t i = 0; i < cnt; ++i) {
        EXPECT_EQ(dev1[0]->qat_err_sim_->GetErr(type).first, code);
    }
    EXPECT_EQ(dev1[0]->qat_err_sim_->GetErr(type).first, VESAL_QAT_ERR_SIM_OK);

    ResetQatCodec();
}

TEST_F(QatErrSimulationTestFixture, SubmitAndPollErrorThenClear) {
    auto* qat_codec = dynamic_cast<QatCodec*>(g_qat_codec.get());
    auto* unit_manager = qat_codec->unit_manager_.get();
    EXPECT_NE(unit_manager, nullptr);
    auto devs = unit_manager->LookupUnits(QatUnitSelection());
    EXPECT_GT(devs.size(), 0);
    uint8_t pf_id = 0xff;
    uint8_t vf_id = 0xff;
    uint8_t inst_id = 0xff;
    QatErrSimType type = QatErrSimType::kSubmit;
    QatErrSimCode code = static_cast<QatErrSimCode>(StatusCode::kInvalidArgument);
    QatErrSimCnt cnt = 100;
    uint8_t flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL;
    std::string msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    auto uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    EXPECT_TRUE(WriteUdsAndReadResponse(uds_path, msg, nullptr));
    // Wait for handler
    usleep(1000);
    CodecChannelOption channel_opts;
    channel_opts.ha_policy = HaPolicy::kNone;
    channel_opts.timeout_ms = 10 * 1000;
    auto channel_r = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_r.first.ok()) << channel_r.first;
    auto* channel = channel_r.second.get();
    unsigned char src[4096];
    unsigned char dst[8192];
    auto compress_r = channel->CompressAsync(src, 4096, dst, 8192, nullptr);
    EXPECT_EQ(QatErrSimCodeToVesalStatusCode(code), compress_r);

    type = QatErrSimType::kPoll;
    code = static_cast<QatErrSimCode>(StatusCode::kShouldRetry);
    cnt = 100;
    flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    EXPECT_TRUE(WriteUdsAndReadResponse(uds_path, msg, nullptr));
    // Wait for handler
    usleep(1000);
    CodecResult results[1];
    ssize_t poll_r = channel->Poll(results, 1, -1);
    EXPECT_EQ(0, (int)poll_r);

    code = VESAL_QAT_ERR_SIM_OK;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    EXPECT_TRUE(WriteUdsAndReadResponse(uds_path, msg, nullptr));
    // Wait for handler
    usleep(1000);
    poll_r = channel->Poll(results, 1, -1);
    EXPECT_EQ(0, (int)poll_r);  // already cleared

    auto close_r = channel->Close();
    EXPECT_TRUE(close_r.ok()) << close_r;
}

TEST_F(QatErrSimulationTestFixture, ControlHangTime) {
    auto* qat_codec = dynamic_cast<QatCodec*>(g_qat_codec.get());
    auto* pool = qat_codec->unit_manager_.get();
    EXPECT_NE(pool, nullptr);
    auto devs = pool->LookupUnits(QatUnitSelection());
    EXPECT_GT(devs.size(), 0);
    uint8_t pf_id = 0xff;
    uint8_t vf_id = 0xff;
    uint8_t inst_id = 0xff;
    QatErrSimType type = QatErrSimType::kResult;
    QatErrSimCode code = VESAL_QAT_ERR_SIM_TIMEOUT_ERROR;
    QatErrSimCnt timeout_us = 500 * 1000;  // 500ms
    uint8_t flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL;
    std::string msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, timeout_us);
    auto uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    WriteUdsAndReadResponse(uds_path, msg, nullptr);
    auto start = TimeStamp::Now();
    // Wait for handler
    usleep(1000);
    CodecChannelOption channel_opts;
    channel_opts.ha_policy = HaPolicy::kNone;
    channel_opts.timeout_ms = 10 * 1000;  // 10s
    auto channel_r = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_r.first.ok()) << channel_r.first;
    auto* channel = channel_r.second.get();
    unsigned char src[4096];
    unsigned char dst[8192];
    auto compress_r = channel->CompressAsync(src, 4096, dst, 4096, nullptr);
    usleep(1000);
    CodecResult results[1];
    while (true) {
        auto poll_r = channel->Poll(results, 1, -1);
        if (poll_r > 0) {
            break;
        }
    }
    auto now = TimeStamp::Now();
    EXPECT_EQ(results[0].status, StatusCode::kOk);
    EXPECT_EQ(compress_r, StatusCode::kOk);
    EXPECT_EQ(TimeStamp::DurationToMs(now - start), timeout_us / 1000);
    // The hang is over now
    compress_r = channel->CompressAsync(src, 4096, dst, 4096, nullptr);
    usleep(1000);
    // Should be able to get result instantly
    auto poll_r = channel->Poll(results, 1, -1);
    EXPECT_EQ(poll_r, 1);
    EXPECT_EQ(results[0].status, StatusCode::kOk);
    auto close_r = channel->Close();
    EXPECT_TRUE(close_r.ok()) << close_r;
}

// Even we set err_sim_timeout to be bigger than the real timeout, it should return once it's reallt
// timeout'd.
TEST_F(QatErrSimulationTestFixture, Timeout) {
    auto* qat_codec = dynamic_cast<QatCodec*>(g_qat_codec.get());
    auto* pool = qat_codec->unit_manager_.get();
    EXPECT_NE(pool, nullptr);
    auto devs = pool->LookupUnits(QatUnitSelection());
    EXPECT_GT(devs.size(), 0);

    uint8_t pf_id = 0xff;
    uint8_t vf_id = 0xff;
    uint8_t inst_id = 0xff;
    QatErrSimType type = QatErrSimType::kResult;
    QatErrSimCode code = VESAL_QAT_ERR_SIM_TIMEOUT_ERROR;
    QatErrSimCnt timeout_us = 500 * 1000;  // 500ms
    uint8_t flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL;
    std::string msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, timeout_us);
    auto uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    WriteUdsAndReadResponse(uds_path, msg, nullptr);
    // Wait for handler
    usleep(1000);

    CodecChannelOption channel_opts;
    channel_opts.ha_policy = HaPolicy::kNone;
    channel_opts.timeout_ms = 10;  // 10ms
    auto channel_r = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_r.first.ok()) << channel_r.first;
    auto* channel = channel_r.second.get();
    auto start = TimeStamp::Now();
    unsigned char src[4096];
    unsigned char dst[8192];
    auto compress_r = channel->CompressAsync(src, 4096, dst, 4096, nullptr);
    EXPECT_EQ(compress_r, StatusCode::kOk);
    usleep(1000);
    CodecResult results[1];
    while (true) {
        auto poll_r = channel->Poll(results, 1, -1);
        if (poll_r > 0) {
            break;
        }
    }
    auto now = TimeStamp::Now();
    EXPECT_EQ(results[0].status, StatusCode::kTimeout);
    EXPECT_EQ(TimeStamp::DurationToMs(now - start), channel_opts.timeout_ms);
    auto close_r = channel->Close();
    EXPECT_TRUE(close_r.ok()) << close_r;
}

TEST_F(QatErrSimulationTestFixture, NonTimeout) {
    auto* qat_codec = dynamic_cast<QatCodec*>(g_qat_codec.get());
    auto* pool = qat_codec->unit_manager_.get();
    EXPECT_NE(pool, nullptr);
    auto devs = pool->LookupUnits(QatUnitSelection());
    EXPECT_GT(devs.size(), 0);
    uint8_t pf_id = 0xff;
    uint8_t vf_id = 0xff;
    uint8_t inst_id = 0xff;
    QatErrSimType type = QatErrSimType::kResult;
    QatErrSimCode code = 2;
    QatErrSimCnt cnt = 100;
    uint8_t flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL;
    std::string msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    auto uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    WriteUdsAndReadResponse(uds_path, msg, nullptr);
    // Wait for handler
    usleep(1000);
    CodecChannelOption channel_opts;
    channel_opts.ha_policy = HaPolicy::kNone;
    channel_opts.timeout_ms = 10 * 1000;
    auto channel_r = CodecChannel::CreateCodecChannel(channel_opts);
    EXPECT_TRUE(channel_r.first.ok()) << channel_r.first;
    auto* channel = channel_r.second.get();
    unsigned char src[4096];
    unsigned char dst[8192];
    auto compress_r = channel->CompressAsync(src, 4096, dst, 4096, nullptr);
    EXPECT_EQ(compress_r, StatusCode::kOk);
    usleep(1000);
    CodecResult results[1];
    while (true) {
        auto poll_r = channel->Poll(results, 1, -1);
        if (poll_r > 0) {
            break;
        }
    }
    EXPECT_EQ(results[0].status, QatErrSimCodeToVesalStatusCode(code));
    auto close_r = channel->Close();
    EXPECT_TRUE(close_r.ok()) << close_r;
}

TEST_F(QatErrSimulationTestFixture, GetQatUnitTest) {
    auto* qat_codec = dynamic_cast<QatCodec*>(g_qat_codec.get());
    auto units = qat_codec->GetAllUnits();
    std::unordered_set<uint8_t> pf_ids;
    std::unordered_set<uint8_t> vf_ids;
    std::unordered_set<uint8_t> inst_ids;
    for (auto& unit : units) {
        pf_ids.insert(unit->GetDeviceId());
        vf_ids.insert(unit->GetFunctionId());
        inst_ids.insert(unit->GetInstId());
    }
    // All
    uint8_t pf_id = 0xff;
    uint8_t vf_id = 0xff;
    uint8_t inst_id = 0xff;
    QatErrSimType type = QatErrSimType::kResult;
    QatErrSimCode code = 2;
    QatErrSimCnt cnt = 100;
    uint8_t flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL;
    std::string msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    auto uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    WriteUdsAndReadResponse(uds_path, msg, nullptr);
    // Wait for handler
    usleep(1000);
    for (auto& unit : units) {
        EXPECT_EQ(unit->GetQatErrSim(type).first, code);
    }
    // PF
    pf_id = *pf_ids.begin();
    code = 4;
    flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_PF;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    WriteUdsAndReadResponse(uds_path, msg, nullptr);
    // Wait for handler
    usleep(1000);
    for (auto& unit : units) {
        if (unit->GetDeviceId() == pf_id) {
            EXPECT_EQ(unit->GetQatErrSim(type).first, code);
        } else {
            EXPECT_NE(unit->GetQatErrSim(type).first, code);
        }
    }
    // VF
    vf_id = *vf_ids.begin();
    code = 3;
    flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_VF;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    WriteUdsAndReadResponse(uds_path, msg, nullptr);
    // Wait for handler
    usleep(1000);
    for (auto& unit : units) {
        if (unit->GetDeviceId() == pf_id && unit->GetFunctionId() == vf_id) {
            EXPECT_EQ(unit->GetQatErrSim(type).first, code);
        } else {
            EXPECT_NE(unit->GetQatErrSim(type).first, code);
        }
    }
    // Inst
    inst_id = *inst_ids.begin();
    code = 7;
    flags = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_INST;
    msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    uds_path = GetQatUdsSocketPath(g_vesal_codec_qat_section_name);
    WriteUdsAndReadResponse(uds_path, msg, nullptr);
    // Wait for handler
    usleep(1000);
    for (auto& unit : units) {
        if (unit->GetDeviceId() == pf_id && unit->GetFunctionId() == vf_id &&
            unit->GetInstId() == inst_id) {
            EXPECT_EQ(unit->GetQatErrSim(type).first, code);
        } else {
            EXPECT_NE(unit->GetQatErrSim(type).first, code);
        }
    }
}

}  // namespace qat
}  // namespace vesal
