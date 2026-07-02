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

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <vector>

#include "codec/qat/qat_error_handling.h"
#include "common/qat/qat_hardware_api_wrapper.h"
#include "common/qat/qat_unit.h"
#include "vesal/log_setting.h"
#include "vesal/vesal.h"

namespace vesal {
namespace qat {

Status QatUnitManager::Init(UnitType unit_type) {
    std::lock_guard<std::mutex> lg(mtx_);
    auto* wrapper = GetQatApiWrapper();
    auto GetNumInstances = unit_type == UnitType::kDc
                               ? &QatHardwareApiWrapper::QAT_cpaDcGetNumInstances
                               : &QatHardwareApiWrapper::QAT_cpaCyGetNumInstances;
    auto GetInstances = unit_type == UnitType::kDc ? &QatHardwareApiWrapper::QAT_cpaDcGetInstances
                                                   : &QatHardwareApiWrapper::QAT_cpaCyGetInstances;
    CpaStatus st = (wrapper->*GetNumInstances)(&unit_num_);
    if (st != CPA_STATUS_SUCCESS) {
        return CpaStatusToVesalStatus(st, "Get qat instance num failed");
    }
    if (unit_num_ == 0) {
        return HardwareError("No qat instances found for the section");
    }
    VESAL_LOG(INFO) << "Get qat instance number=" << unit_num_;
    CpaInstanceHandle handles[unit_num_];
    st = (wrapper->*GetInstances)(unit_num_, handles);
    if (st != CPA_STATUS_SUCCESS) {
        return CpaStatusToVesalStatus(st, "Get qat instance handles failed");
    }
    size_t succ_num = 0;
    for (size_t i = 0; i < unit_num_; ++i) {
        auto unit = std::make_unique<QatUnit>(handles[i], unit_type);
        Status r = unit->Start();
        if (!r.ok()) {
            VESAL_LOG(WARN) << "Failed to start qat unit " << *unit << ", error=" << r;
            unit->MarkBlacklist();
        } else {
            ++succ_num;
            pf_in_use_cnt_[unit->GetDeviceId()] = 0;
        }
        all_units_.push_back(std::move(unit));
    }
    VESAL_LOG(INFO) << "Successfully started " << succ_num << " qat units";
    return succ_num > 0 ? OkStatus() : ResourceBusyError("No qat unit available");
}

void QatUnitManager::Uninit() {
    std::lock_guard<std::mutex> lg(mtx_);
    for (auto& each_unit : all_units_) {
        Status r = each_unit->Stop();
        if (!r.ok()) {
            VESAL_LOG(WARN) << "Failed to stop qat unit " << *each_unit << "error=" << r;
        }
        each_unit->ResetContext();
    }
    pf_in_use_cnt_.clear();
    all_units_.clear();
    unit_num_ = 0;
}

QatUnit* QatUnitManager::GrabAvailableUnit(const QatUnitSelection& selection) {
    std::lock_guard<std::mutex> lg(mtx_);
    QatUnit* ret = nullptr;
    int pf_id_cnt = -1;
    // Less busy one comes first.
    for (auto& each_unit : all_units_) {
        if (each_unit->Usable() && FitSelection(selection, each_unit.get())) {
            int32_t pf_id = each_unit->GetDeviceId();
            int cnt = pf_in_use_cnt_.find(pf_id)->second;
            if ((ret == nullptr) || cnt < pf_id_cnt) {
                ret = each_unit.get();
                pf_id_cnt = cnt;
            }
        }
    }
    if (ret != nullptr) {
        ret->IncRefCnt();
        ret->SetThreadName();
        pf_in_use_cnt_[ret->GetDeviceId()]++;
    }
    return ret;
}

QatUnit* QatUnitManager::GrabFromDiffDevice(const std::vector<QatUnit*>& excluded_units) {
    std::lock_guard<std::mutex> lg(mtx_);
    QatUnit* ret = nullptr;
    int pf_id_cnt = -1;
    for (auto& each_unit : all_units_) {
        int pf_id = each_unit->GetDeviceId();
        //  1. Usable
        //  2. Not from the same device
        //  3. Less busy
        if (each_unit->Usable() &&
            std::find_if(excluded_units.begin(),
                         excluded_units.end(),
                         [pf_id](QatUnit* u) { return u->GetDeviceId() == pf_id; }) ==
                excluded_units.end() &&
            ((ret == nullptr) || pf_in_use_cnt_[pf_id] < pf_id_cnt)) {
            ret = each_unit.get();
            pf_id_cnt = pf_in_use_cnt_[pf_id];
        }
    }
    if (ret != nullptr) {
        ret->IncRefCnt();
        ret->SetThreadName();
        pf_in_use_cnt_[ret->GetDeviceId()]++;
    }
    return ret;
}

void QatUnitManager::PutBackUnit(QatUnit* unit) {
    if (unit == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lg(mtx_);
    VESAL_DCHECK(unit->IsInUse());
    unit->DecRefCnt();
    unit->ClearThreadName();
    pf_in_use_cnt_[unit->GetDeviceId()]--;
}

void QatUnitManager::PutBackToBlackList(QatUnit* unit) {
    if (unit == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lg(mtx_);
    VESAL_DCHECK(unit->IsInUse());
    VESAL_DCHECK(!unit->IsInBlackList());
    unit->DecRefCnt();
    unit->MarkBlacklist();
    unit->ClearThreadName();
    pf_in_use_cnt_[unit->GetDeviceId()]--;
}

std::vector<QatUnit*> QatUnitManager::LookupUnits(const QatUnitSelection& selection) {
    std::vector<QatUnit*> ret;
    ret.reserve(all_units_.size());
    std::lock_guard<std::mutex> lg(mtx_);
    auto itr = all_units_.begin();
    while (itr != all_units_.end()) {
        if (FitSelection(selection, itr->get())) {
            ret.push_back(itr->get());
        }
        ++itr;
    }
    return ret;
}

bool QatUnitManager::FitSelection(const QatUnitSelection& selection, QatUnit* unit) {
    bool ret = true;
    ret &= selection.numa_id < 0 || selection.numa_id == unit->GetNumaId();
    ret &= selection.pf_id < 0 || selection.pf_id == unit->GetDeviceId();
    ret &= selection.vf_id < 0 || selection.vf_id == unit->GetFunctionId();
    ret &= selection.inst_id < 0 || selection.inst_id == unit->GetInstId();
    return ret;
}

std::vector<DeviceInfo> QatUnitManager::GetQatDeviceInfos() {
    std::vector<DeviceInfo> infos;
    std::unordered_map<uint32_t, int32_t> pf_to_numa;
    for (const auto& unit : all_units_) {
        pf_to_numa[unit->GetDeviceId()] = unit->GetNumaId();
    }
    for (const auto& p : pf_to_numa) {
        infos.push_back({.device_id = (int32_t)p.first, .numa_id = p.second});
    }
    return infos;
}

}  // namespace qat
}  // namespace vesal