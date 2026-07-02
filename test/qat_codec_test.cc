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

#include "codec/qat/qat_codec.h"

#include <gtest/gtest.h>
#include <qat_dummy_driver.h>

#include "codec/qat/qat_codec_engine.h"
#include "common/defer.h"
#include "common/qat/qat_hardware_api_wrapper.h"
#include "common/qat/qat_unit.h"
#include "common/qat/qat_unit_manager.h"
#include "common/qat/qat_util.h"
#include "ut_util.h"
#include "vesal/codec.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

namespace vesal {

namespace {
struct MockQatHardwareApiWrapperOpts {
    int QAT_icp_sal_userStart_mock_type = 0;
    int QAT_cpaDcGetNumInstances_mock_type = 0;
    int QAT_cpaDcGetInstances_mock_type = 0;
    int QAT_cpaDcQueryCapabilities_mock_type = 0;
};

class MockQatHardwareApiWrapper : public qat::QatHardwareApiWrapper {
public:
    CpaStatus QAT_icp_sal_userStart(const char* pProcessName) override {
        switch (opts_.QAT_icp_sal_userStart_mock_type) {
        // failure
        case 1:
            return CPA_STATUS_FAIL;
        // original api
        default:
            return qat::QatHardwareApiWrapper::QAT_icp_sal_userStart(pProcessName);
        }
    }

    virtual CpaStatus QAT_cpaDcGetNumInstances(Cpa16U* pNumInstances) override {
        switch (opts_.QAT_cpaDcGetNumInstances_mock_type) {
        // failure
        case 1:
            return CPA_STATUS_FAIL;
        // success but no instance
        case 2:
            *pNumInstances = 0;
            return CPA_STATUS_SUCCESS;
        // original api
        default:
            return qat::QatHardwareApiWrapper::QAT_cpaDcGetNumInstances(pNumInstances);
        }
    }

    virtual CpaStatus QAT_cpaDcGetInstances(Cpa16U numInstances,
                                            CpaInstanceHandle* dcInstances) override {
        switch (opts_.QAT_cpaDcGetInstances_mock_type) {
        // failure
        case 1:
            return CPA_STATUS_FAIL;
        // original api
        default:
            return qat::QatHardwareApiWrapper::QAT_cpaDcGetInstances(numInstances, dcInstances);
        }
    }

    virtual CpaStatus QAT_cpaDcQueryCapabilities(
        CpaInstanceHandle dcInstance, CpaDcInstanceCapabilities* pInstanceCapabilities) override {
        switch (opts_.QAT_cpaDcQueryCapabilities_mock_type) {
        // success every other call
        case 1:
            static bool success = false;
            success ^= 1;
            if (success)
                return qat::QatHardwareApiWrapper::QAT_cpaDcQueryCapabilities(
                    dcInstance, pInstanceCapabilities);
            else
                return CPA_STATUS_FAIL;
        // original api
        default:
            return qat::QatHardwareApiWrapper::QAT_cpaDcQueryCapabilities(dcInstance,
                                                                          pInstanceCapabilities);
        }
    }
    MockQatHardwareApiWrapperOpts opts_;
};

class MockQatUnitManager : public qat::QatUnitManager {
public:
    MockQatUnitManager() : qat::QatUnitManager() {}

    Status Init(qat::UnitType unit_type) override {
        return NotSupportedError("test");
    }
};

}  // namespace

class QatCodecTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        VESAL_INIT_CODEC_QAT_ONLY;
    }

    void TearDown() override {
        VESAL_UNINIT;
    }
};

class QatCodecMockTest : public ::testing::Test {
protected:
    void SetUp() override {
        InitLogging({});
    }
    void TearDown() override {
        ShutdownLogging(true);
    }
};

TEST_F(QatCodecTestFixture, Basic) {}

TEST_F(QatCodecMockTest, FailToInit) {
    auto codec = std::make_unique<qat::QatCodec>();
    // hack here, replace the pool_ with a mock class pointer. restore after use.
    MockQatUnitManager* mock_manager = new MockQatUnitManager();
    auto* origin = codec->unit_manager_.release();
    codec->unit_manager_.reset(mock_manager);

    Status init_with_mock_r = codec->Start();
    EXPECT_TRUE(IsNotSupported(init_with_mock_r)) << init_with_mock_r;
    codec->unit_manager_.reset(origin);
}
namespace qat {
extern std::unique_ptr<qat::QatHardwareApiWrapper> g_qat_api_wrapper;
}
TEST_F(QatCodecTestFixture, ErrorHandling) {
    std::vector<MockQatHardwareApiWrapperOpts> opts_vec_fail = {
        {0, 1, 0, 0}, {0, 2, 0, 0}, {0, 0, 1, 0}};
    std::vector<MockQatHardwareApiWrapperOpts> opts_vec_succ = {{0, 0, 0, 1}};
    auto origin_wrapper = *qat::g_qat_api_wrapper;
    auto wrapper = new MockQatHardwareApiWrapper();
    qat::g_qat_api_wrapper.reset(wrapper);
    auto __g = defer([&]() {
        qat::g_qat_api_wrapper = std::make_unique<qat::QatHardwareApiWrapper>(origin_wrapper);
    });
    for (auto& opts : opts_vec_fail) {
        auto qat_codec = std::make_unique<qat::QatCodec>();
        wrapper->opts_ = opts;
        Status st = qat_codec->Start();
        // Currently we allow partial success.
        EXPECT_FALSE(st.ok()) << opts.QAT_icp_sal_userStart_mock_type << " "
                              << opts.QAT_cpaDcGetNumInstances_mock_type << " "
                              << opts.QAT_cpaDcGetInstances_mock_type << " "
                              << opts.QAT_cpaDcQueryCapabilities_mock_type;
    }
    for (auto& opts : opts_vec_succ) {
        auto qat_codec = std::make_unique<qat::QatCodec>();
        wrapper->opts_ = opts;
        Status st = qat_codec->Start();
        EXPECT_TRUE(st.ok()) << opts.QAT_icp_sal_userStart_mock_type << " "
                             << opts.QAT_cpaDcGetNumInstances_mock_type << " "
                             << opts.QAT_cpaDcGetInstances_mock_type << " "
                             << opts.QAT_cpaDcQueryCapabilities_mock_type;
    }
}

// This test assumes the running env has 2 numa nodes and QAT devices locates seperately on them
TEST_F(QatCodecTestFixture, NumaOpts) {
    vesal::CodecChannelOption channel_opts;
    channel_opts.comp_algorithm = vesal::CodecAlgorithm::kLz4;
    channel_opts.checksum_type = vesal::CodecChecksumType::kCrc32;

    for (int i = 0; i < 2; ++i) {
        channel_opts.allocation_option.node_affinity = i;
        // Expect right numa number
        auto channel_r = qat::CreateQatCodecEngine(channel_opts);
        EXPECT_TRUE(channel_r.first.ok()) << channel_r.first;
        auto qat_channel = std::move(channel_r.second);
        EXPECT_EQ(qat_channel->GetQatUnit()->qat_unit_attr_.numa_hint, i);
        qat_channel->Close();
    }
    for (int i = 2; i < 10; ++i) {
        // Invalid numa number
        channel_opts.allocation_option.node_affinity = i;
        auto channel_r = qat::CreateQatCodecEngine(channel_opts);
        EXPECT_TRUE(IsResourceBusy(channel_r.first));
    }
}

TEST_F(QatCodecMockTest, InitFailIfDriverLoadErr) {
    auto origin = g_driver_load_codec_ok;
    g_driver_load_codec_ok = 0;
    auto qat_codec = std::make_unique<qat::QatCodec>();
    auto r = qat_codec->Start();
    EXPECT_TRUE(IsNotSupported(r)) << r;
    g_driver_load_codec_ok = origin;
}

TEST_F(QatCodecTestFixture, GetUnits) {
    auto units = g_qat_codec->GetAllUnits();
    qat::QatHardwareApiWrapper wrapper;
    Cpa16U num_instances;
    auto r = wrapper.QAT_cpaDcGetNumInstances(&num_instances);
    EXPECT_EQ(r, CPA_STATUS_SUCCESS) << r;
    EXPECT_EQ(num_instances, units.size());
    int32_t device_id = units[0]->GetDeviceId();
    int32_t function_id = units[0]->GetFunctionId();
    int32_t inst_id = units[0]->GetInstId();
    qat::QatUnit* unit = units[0];

    units = g_qat_codec->GetUnitWithDeviceId(device_id);
    for (auto unit : units) {
        EXPECT_EQ(unit->GetDeviceId(), device_id);
    }
    units = g_qat_codec->GetUnitWithFunctionId(device_id, function_id);
    for (auto unit : units) {
        EXPECT_EQ(unit->GetDeviceId(), device_id);
        EXPECT_EQ(unit->GetFunctionId(), function_id);
    }
    auto u = g_qat_codec->GetUnitWithInstId(device_id, function_id, inst_id);
    EXPECT_EQ(u, unit);
}

}  // namespace vesal
