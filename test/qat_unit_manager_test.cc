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

#include "common/qat/qat_unit_manager.h"

#include <gtest/gtest.h>

#include "codec/codec_internal.h"
#include "codec/qat/qat_codec.h"
#include "common/defer.h"
#include "vesal/vesal.h"

namespace vesal {
namespace qat {

TEST(QatUnitManager, InitAndUninit) {
    vesal::InitOptions opts;
    opts.data_flow_init_opt.init_dsa = false;
    opts.codec_init_opt.init_qat = true;
    EXPECT_TRUE(vesal::Init(opts));
    auto __g = defer([&]() { vesal::Uninit(); });
    auto* mgr = dynamic_cast<QatCodec*>(g_qat_codec.get())->unit_manager_.get();
    EXPECT_NE(mgr, nullptr);
    EXPECT_GT(mgr->all_units_.size(), 0);
}

TEST(QatUnitManager, CreateAndCloseChannels) {
    vesal::InitOptions opts;
    opts.data_flow_init_opt.init_dsa = false;
    opts.codec_init_opt.init_qat = true;
    EXPECT_TRUE(vesal::Init(opts));
    auto __g = defer([&]() { vesal::Uninit(); });
    auto* mgr = dynamic_cast<QatCodec*>(g_qat_codec.get())->unit_manager_.get();
    EXPECT_NE(mgr, nullptr);
    std::vector<std::unique_ptr<CodecChannel>> channels;
    for (auto itr = mgr->pf_in_use_cnt_.begin(); itr != mgr->pf_in_use_cnt_.end(); ++itr) {
        CodecChannelOption channel_opt;
        channel_opt.engine_type = CodecEngineType::kQat;
        auto cr = CodecChannel::CreateCodecChannel(channel_opt);
        EXPECT_TRUE(cr.first.ok()) << cr.first;
        channels.push_back(std::move(cr.second));
    }
    EXPECT_EQ(channels.size(), mgr->pf_in_use_cnt_.size());
    for (auto itr = mgr->pf_in_use_cnt_.begin(); itr != mgr->pf_in_use_cnt_.end(); ++itr) {
        EXPECT_EQ(itr->second, 1);
    }
    for (auto& c : channels) {
        auto close = c->Close();
        EXPECT_TRUE(close.ok()) << close;
        c.reset();
    }
    for (auto itr = mgr->pf_in_use_cnt_.begin(); itr != mgr->pf_in_use_cnt_.end(); ++itr) {
        EXPECT_EQ(itr->second, 0);
    }
}

TEST(QatUnitManager, LookupUnits) {
    vesal::InitOptions opts;
    opts.data_flow_init_opt.init_dsa = false;
    opts.codec_init_opt.init_qat = true;
    EXPECT_TRUE(vesal::Init(opts));
    auto __g = defer([&]() { vesal::Uninit(); });
    auto* mgr = dynamic_cast<QatCodec*>(g_qat_codec.get())->unit_manager_.get();
    EXPECT_NE(mgr, nullptr);

    auto units = mgr->LookupUnits(QatUnitSelection());
    EXPECT_EQ(units.size(), mgr->all_units_.size());
    QatUnitSelection s;
    s.pf_id = units[0]->GetDeviceId();
    units = mgr->LookupUnits(s);
    for (auto& u : units) {
        EXPECT_EQ(u->GetDeviceId(), s.pf_id);
    }
    s.vf_id = units[0]->GetFunctionId();
    units = mgr->LookupUnits(s);
    for (auto& u : units) {
        EXPECT_EQ(u->GetDeviceId(), s.pf_id);
        EXPECT_EQ(u->GetFunctionId(), s.vf_id);
    }
    s.inst_id = units[0]->GetInstId();
    units = mgr->LookupUnits(s);
    for (auto& u : units) {
        EXPECT_EQ(u->GetDeviceId(), s.pf_id);
        EXPECT_EQ(u->GetFunctionId(), s.vf_id);
        EXPECT_EQ(u->GetInstId(), s.inst_id);
    }
    EXPECT_EQ(units.size(), 1);
}

TEST(QatUnitManager, GrabFromDiffDevice) {
    vesal::InitOptions opts;
    opts.data_flow_init_opt.init_dsa = false;
    opts.codec_init_opt.init_qat = true;
    EXPECT_TRUE(vesal::Init(opts));
    auto __g = defer([&]() { vesal::Uninit(); });
    auto* mgr = dynamic_cast<QatCodec*>(g_qat_codec.get())->unit_manager_.get();
    EXPECT_NE(mgr, nullptr);
    int pf_num = 4;
    std::vector<QatUnit*> units;
    for (int i = 0; i < pf_num - 1; ++i) {
        units.push_back(mgr->GrabAvailableUnit(QatUnitSelection()));
        EXPECT_NE(units.back(), nullptr);
    }
    auto u = mgr->GrabFromDiffDevice(units);
    EXPECT_NE(u, nullptr);
    for (auto& e : units) {
        EXPECT_NE(u->GetDeviceId(), e->GetDeviceId());
    }
    units.push_back({u});
    u = mgr->GrabFromDiffDevice(units);
    EXPECT_EQ(u, nullptr);
    for (auto& u : units) {
        mgr->PutBackUnit(u);
    }
}

TEST(QatUnitManager, GetDeviceInfoTest) {
    vesal::InitOptions opts;
    opts.data_flow_init_opt.init_dsa = false;
    opts.codec_init_opt.init_qat = true;
    EXPECT_TRUE(vesal::Init(opts));
    auto infos = GetQatDeviceInfos();
    // 4 qat devices on s80 machine
    EXPECT_EQ(infos.size(), 4);
    vesal::Uninit();

    opts.codec_init_opt.init_qat = false;
    opts.cypher_init_opt.init_qat = true;
    EXPECT_TRUE(vesal::Init(opts));
    infos = GetQatDeviceInfos();
    // 4 qat devices on s80 machine
    EXPECT_EQ(infos.size(), 4);
    vesal::Uninit();
}

}  // namespace qat
}  // namespace vesal