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

#include <valgrind/memcheck.h>
#include <valgrind/valgrind.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "codec/codec_internal.h"
#include "common/qat/qat_hardware_api_wrapper.h"
#ifdef VESAL_ENABLE_ERR_SIM
#include "common/err_simulation.h"
#endif
#include "gflags/gflags_declare.h"
#include "vesal/codec.h"
#include "vesal/status.h"

extern "C" {
#include <dc/cpa_dc.h>
}

namespace vesal {
namespace qat {

enum class UnitType : uint8_t {
    kDc = 0,
    kCy = 1,
    kNum = 2,
};

struct QatUnitAttr {
    /* Device bus address (B.D.F)
     * Bit 15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00
     *    |--------Device---------|----------FUNCT--------|
     */
    std::string part_name{
        "4xxxvf"};  // QAT model name, currently only supports 4xxx and 200xx, and their vfs.
    std::string instance_id{
        ""};               // For metrics compatiblity, this is {VF id}.{InstName}, eg. "0.Dc1".
    uint32_t device_id;    // Device part as device id(higher 8 bit of bus address)
    uint32_t function_id;  // FUNCT part as function id(lower 8 bit of bus address)
    uint32_t inst_id;      // CpaInstanceInfo2.instID. E.g, for Dc0, inst_id is 0.
    int32_t numa_hint;
    bool is_polled;
    // Calc crc for input source data in compress direction,
    // and calc crc for output data in decompress direction.
    bool is_crc_supp;
    bool is_phys_contiguous_mem_required;
    bool is_lz4_compression;
    bool is_lz4_decompression;
    bool is_zstd_compression;
    bool is_zstd_decompression;
    bool is_deflate_compression;
    bool is_deflate_decompression;
    bool is_aes_xts_supp{false};
};

class QatUnit {
public:
    QatUnit(CpaInstanceHandle inst_hdl, UnitType unit_type = UnitType::kDc);
    virtual ~QatUnit() = default;  // mocked by UT

    Status Start();

    CpaInstanceHandle* GetInstanceHandle() {
        return &cpa_instance_handle_;
    }

    QatUnitAttr GetQatUnitAttr() const {
        return qat_unit_attr_;
    }

    int GetNumaId() const {
        return qat_unit_attr_.numa_hint;
    }

    int GetDeviceId() const {
        return qat_unit_attr_.device_id;
    }

    int GetFunctionId() const {
        return qat_unit_attr_.function_id;
    }

    int GetInstId() const {
        return qat_unit_attr_.inst_id;
    }

    bool SvmEnabled() const {
        return !qat_unit_attr_.is_phys_contiguous_mem_required;
    }

    void SetThreadName() {
        in_use_thread_name_.resize(16);
        pthread_getname_np(pthread_self(), &in_use_thread_name_[0], in_use_thread_name_.size());
        in_use_thread_name_.resize(in_use_thread_name_.find_first_of('\0'));
    }
    void ClearThreadName() {
        in_use_thread_name_.clear();
    }

    std::string GetThreadName() const {
        return in_use_thread_name_;
    }

    bool IsInUse() const {
        return ref_cnt_ > 0;
    }

    void IncRefCnt() {
        ref_cnt_++;
    }

    void DecRefCnt() {
        ref_cnt_--;
    }
    void ResetRefCnt() {
        ref_cnt_ = 0;
    }

    void MarkBlacklist() {
        blacklisted_ = true;
    }
    void UnMarkBlacklist() {
        blacklisted_ = false;
    }
    bool IsInBlackList() const {
        return blacklisted_;
    }

    bool Usable() {
        return !IsInBlackList() && !IsInUse();
    }

    void ResetContext() {
        UnMarkBlacklist();
        ResetRefCnt();
        ClearThreadName();
    }

    Status Stop();

#ifdef VESAL_ENABLE_ERR_SIM
    void SetQatErrSim(QatErrSimType type, QatErrSimCode code, QatErrSimCnt cnt) {
        qat_err_sim_->SetError(type, code, cnt);
    }

    std::pair<QatErrSimCode, uint64_t> GetQatErrSim(QatErrSimType type) {
        return qat_err_sim_->GetErr(type);
    }
#endif
    friend std::ostream& operator<<(std::ostream& os, const QatUnit& unit);

private:
    Status QueryCapabilities();
    Status QueryInformation();
#ifdef VESAL_ENABLE_ERR_SIM
    std::unique_ptr<QatErrSim> qat_err_sim_;
#endif
    CpaInstanceHandle cpa_instance_handle_;
    QatUnitAttr qat_unit_attr_;
    UnitType unit_type_;

    //  If the unit is in use, the thread name will be set to this.
    std::string in_use_thread_name_{""};
    //  Hold by how many channels. For now it's only 1 or 0 because only one instance can only be
    //  held by one channel.
    int ref_cnt_;
    bool blacklisted_;
};

inline std::ostream& operator<<(std::ostream& os, const QatUnit& unit) {
    VALGRIND_MAKE_MEM_DEFINED(&unit, sizeof(QatUnit));
    os << std::hex << "QatUnit, instance_id: " << unit.qat_unit_attr_.instance_id
       << ", device_id: " << unit.qat_unit_attr_.device_id
       << ", function_id: " << unit.qat_unit_attr_.function_id
       << ", inst_id: " << unit.qat_unit_attr_.inst_id
       << ", numa_hint: " << unit.qat_unit_attr_.numa_hint
       << ", is_polled: " << unit.qat_unit_attr_.is_polled
       << ", is_crc_supp: " << unit.qat_unit_attr_.is_crc_supp
       << ", svm_disabled: " << unit.qat_unit_attr_.is_phys_contiguous_mem_required << std::dec
       << ", in_use_cnt: " << unit.ref_cnt_ << ", blacklisted: " << unit.blacklisted_
       << ", in_use_thread_name_: " << unit.in_use_thread_name_;
    return os;
}

}  // namespace qat
}  // namespace vesal
