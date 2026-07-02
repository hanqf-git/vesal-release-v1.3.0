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

#pragma once

#include <memory>

#include "codec/codec_internal.h"
#include "common/qat/qat_hardware_api_wrapper.h"
#include "common/qat/qat_unit.h"
#include "common/qat/qat_unit_manager.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/vesal.h"

namespace vesal {
namespace qat {

// QAT iodepth always needs to subtract this value by its design.
#define QAT_RING_FREE_SIZE 2

// Read from config file. Otherwise use 512 - 2 as default.
extern uint32_t g_max_qat_cfg_concurrency;

extern std::string g_vesal_codec_qat_section_name;

class QatCodec : public Codec {
public:
    QatCodec() : unit_manager_(std::make_unique<QatUnitManager>()) {}

    Status Start();
    ~QatCodec() override;

    std::pair<Status, std::unique_ptr<CodecChannel>> CreateCodecChannel(
        const CodecChannelOption& opts) override;

    Status Stop();

    std::vector<QatUnit*> GetAllUnits() {
        return unit_manager_->LookupUnits(QatUnitSelection());
    }

    std::vector<QatUnit*> GetUnitWithDeviceId(uint32_t device_id) {
        QatUnitSelection selection;
        selection.pf_id = device_id;
        return unit_manager_->LookupUnits(selection);
    }

    std::vector<QatUnit*> GetUnitWithFunctionId(uint32_t device_id, uint32_t function_id) {
        QatUnitSelection selection;
        selection.pf_id = device_id;
        selection.vf_id = function_id;
        return unit_manager_->LookupUnits(selection);
    }

    QatUnit* GetUnitWithInstId(uint32_t device_id, uint32_t function_id, uint32_t inst_id) {
        QatUnitSelection selection;
        selection.pf_id = device_id;
        selection.vf_id = function_id;
        selection.inst_id = inst_id;
        auto v = unit_manager_->LookupUnits(selection);
        return v.size() > 0 ? v[0] : nullptr;
    }

    QatUnitManager* GetUnitManager() {
        return unit_manager_.get();
    }

    size_t GetMaxInQatSize() const {
        return max_in_qat_size_;
    }

private:
    // total qat unit number
    std::unique_ptr<QatUnitManager> unit_manager_;

    static std::mutex qat_codec_build_mutex_;
    static bool is_running_;
    size_t max_in_qat_size_;
};

class QatCodecEngine;
// TODO(sjj): return Engine type after implemented.
std::pair<Status, std::unique_ptr<qat::QatCodecEngine>> CreateQatCodecEngine(
    const CodecChannelOption& opts);

}  // namespace qat
}  // namespace vesal
