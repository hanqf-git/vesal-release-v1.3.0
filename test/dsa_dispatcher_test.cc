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

#include "data_flow/dsa/dsa_dispatcher.h"
#include <gtest/gtest.h>
#include "data_flow/data_flow_resource_manager.h"
#include "data_flow/dsa/work_queue_info.h"
#include "vesal/data_flow.h"
#include "vesal/vesal.h"

namespace vesal {
namespace {
static constexpr size_t kCiMachineDsaNum = 2;
}

class DsaDispatcherTest : public ::testing::Test {
public:
    void SetUp() override {
        // avoid data flow resource manager's dsa engine influence
        data_flow::DataFlowResourceManager* rm = data_flow::DataFlowResourceManager::GetInstance();
        rm->Uninit();
    }
};

TEST_F(DsaDispatcherTest, DefaultDsaQuota) {
    std::string tmp = FLAGS_vesal_data_flow_dsa_quota;
    FLAGS_vesal_data_flow_dsa_quota = "";
    data_flow::DsaDispatcher dispatcher;
    DataFlowInitOptions init_opts;
    auto res = dispatcher.Init(init_opts);
    EXPECT_EQ(res, StatusCode::kOk);
    EXPECT_EQ(dispatcher.wq_list_.size(), kCiMachineDsaNum);
    EXPECT_EQ(dispatcher.wq_list_[0]->capacity, data_flow::kWorkQueueCapacity);
    EXPECT_EQ(dispatcher.wq_list_[0]->size, 0);
    for (size_t i = 1; i < kCiMachineDsaNum; i++) {
        EXPECT_EQ(dispatcher.wq_list_[i]->capacity, 0);
        EXPECT_EQ(dispatcher.wq_list_[i]->size, 0);
    }
    FLAGS_vesal_data_flow_dsa_quota = tmp;
}

TEST_F(DsaDispatcherTest, WrongDsaQuota) {
    std::string tmp = FLAGS_vesal_data_flow_dsa_quota;
    data_flow::DsaDispatcher dispatcher;
    DataFlowInitOptions init_opts;
    // contains characters other than digits and commas
    {
        FLAGS_vesal_data_flow_dsa_quota = "123!";
        auto res = dispatcher.Init(init_opts);
        EXPECT_NE(res, StatusCode::kOk);
    }

    // wrong format case 1
    {
        FLAGS_vesal_data_flow_dsa_quota = "123,12,";
        auto res = dispatcher.Init(init_opts);
        EXPECT_NE(res, StatusCode::kOk);
    }

    // wrong format case 2
    {
        FLAGS_vesal_data_flow_dsa_quota = "123,,12";
        auto res = dispatcher.Init(init_opts);
        EXPECT_NE(res, StatusCode::kOk);
    }

    // not match dsa amount (kCiMachineDsaNum)
    {
        FLAGS_vesal_data_flow_dsa_quota = "123";
        auto res = dispatcher.Init(init_opts);
        EXPECT_NE(res, StatusCode::kOk);
    }

    FLAGS_vesal_data_flow_dsa_quota = tmp;
}

TEST_F(DsaDispatcherTest, CorrectDsaQuota) {
    std::string tmp = FLAGS_vesal_data_flow_dsa_quota;
    data_flow::DsaDispatcher dispatcher;
    DataFlowInitOptions init_opts;
    int quota = 16;
    std::string quota_string = std::to_string(quota);
    for (size_t i = 1; i < kCiMachineDsaNum; i++) {
        quota_string += "," + std::to_string(quota);
    }
    FLAGS_vesal_data_flow_dsa_quota = quota_string;
    auto res = dispatcher.Init(init_opts);
    EXPECT_EQ(res, StatusCode::kOk);
    for (size_t i = 0; i < kCiMachineDsaNum; i++) {
        EXPECT_EQ(dispatcher.wq_list_[i]->capacity, quota);
        EXPECT_EQ(dispatcher.wq_list_[i]->size, 0);
    }
    FLAGS_vesal_data_flow_dsa_quota = tmp;
}

}  // namespace vesal