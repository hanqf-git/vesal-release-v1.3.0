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

#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "common/qat/qat_unit.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

namespace vesal {
namespace qat {

class QatUnit;

struct QatUnitSelection {
    int numa_id;
    int pf_id;
    int vf_id;
    int inst_id;

    QatUnitSelection() : numa_id(-1), pf_id(-1), vf_id(-1), inst_id(-1) {}
};

// Hold all the QatUnit and manage their life cycles, responsible for start and stop the QatUnit
// using native QAT APIs. Also provide the ability to select the QatUnit by multiple policies. For
// simplicity, all units are held by simple list because the units are normally no more than 100,
// loop is enough for us.
class QatUnitManager {
public:
    QatUnitManager() : unit_num_(0) {}
    virtual ~QatUnitManager() = default;

    // Get and start all QatUnits. The Unit failed to start shall be kept in black list.
    virtual Status Init(UnitType unit_type = UnitType::kDc);
    // Stop all the QatUnit and release them. Will do the best to stop all but some might fail. In
    // failure case, the reason shall be logged but users can do nothing, just ignore and exit.
    void Uninit();

    // QatUnitSelection is used to grab a QatUnit from the idle pool. From top to bottom, the pool
    // is organised in a structure is like root->numa->pf->vf->inst. The lookup process starts from
    // root. At every level, the id -1 no filter at this level. E.g, numa_id =
    // 0, pf_id = 117, vf_id = -1, inst_id = 0 means to select a QatUnit from numa 0, pf=117,
    // despite the vf_id, but the inst_id = 0. Also, for all units meets the selection, try to
    // return the less busy one.
    QatUnit* GrabAvailableUnit(const QatUnitSelection& selection);
    //  Grab the unit but not from the same devices in the excluded_units.
    QatUnit* GrabFromDiffDevice(const std::vector<QatUnit*>& excluded_units);

    void PutBackUnit(QatUnit* unit);

    void PutBackToBlackList(QatUnit* unit);

    // Select all the QatUnit that fits the selection. Calling this function has no effect on the
    // state of the units, like busy or idle, so the caller no needs to return the units.
    // The API is supposed to be used internally.
    std::vector<QatUnit*> LookupUnits(const QatUnitSelection& selection);

    std::vector<DeviceInfo> GetQatDeviceInfos();

private:
    static bool FitSelection(const QatUnitSelection& selection, QatUnit* unit);

    std::mutex mtx_;

    uint16_t unit_num_;

    std::list<std::unique_ptr<QatUnit>> all_units_;  // Unit life cycle managed by this
    //  TODO(sjj): add metrics for pf_in_use_cnt_
    std::unordered_map<int32_t, int32_t>
        pf_in_use_cnt_;  // Key: pf_id, Value: how many channels are connecting to this PF. Here we
                         // assume that the more channels are connected to this PF, the busier it
                         // is.
};

}  // namespace qat
}  // namespace vesal
