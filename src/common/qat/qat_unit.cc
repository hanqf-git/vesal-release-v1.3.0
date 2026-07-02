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

#include "qat_unit.h"

#include <gflags/gflags.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "codec/qat/qat_error_handling.h"
#include "common/memory_pool_helper.h"
#include "common/qat/qat_hardware_api_wrapper.h"
#include "vesal/codec.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {
namespace qat {

static constexpr std::array<const char*, 4> kSupportedQatModelName = {
    "4xxx", "4xxxvf", "200xx", "200xxvf"};

QatUnit::QatUnit(CpaInstanceHandle inst_hdl, UnitType unit_type)
    : cpa_instance_handle_(inst_hdl), unit_type_(unit_type), ref_cnt_(0), blacklisted_(false) {
#ifdef VESAL_ENABLE_ERR_SIM
    qat_err_sim_ = std::make_unique<QatErrSim>(this);
#endif
}

Status QatUnit::QueryCapabilities() {
    if (unit_type_ == UnitType::kCy) {
        CpaCyCapabilitiesInfo caps;
        CpaStatus st = GetQatApiWrapper()->QAT_cpaCyQueryCapabilities(cpa_instance_handle_, &caps);
        if (CPA_STATUS_SUCCESS != st) {
            return CpaStatusToVesalStatus(st, "Fail to cpaCyQueryCapabilities");
        }
        if (caps.symSupported) {
            CpaCySymCapabilitiesInfo sym_caps;
            CpaStatus st =
                GetQatApiWrapper()->QAT_cpaCySymQueryCapabilities(cpa_instance_handle_, &sym_caps);
            if (CPA_STATUS_SUCCESS != st) {
                return CpaStatusToVesalStatus(st, "Fail to cpaCySymQueryCapabilities");
            }
            if (CPA_BITMAP_BIT_TEST(sym_caps.ciphers, CPA_CY_SYM_CIPHER_AES_XTS)) {
                qat_unit_attr_.is_aes_xts_supp = true;
            } else {
                return NotSupportedError("Not supported aes xts");
            }
        } else {
            return NotSupportedError("Not supported sym");
        }

    } else {
        CpaDcInstanceCapabilities caps;
        CpaStatus st = GetQatApiWrapper()->QAT_cpaDcQueryCapabilities(cpa_instance_handle_, &caps);
        if (CPA_STATUS_SUCCESS != st) {
            return CpaStatusToVesalStatus(st, "Fail to cpaDcQueryCapabilities");
        }
        // TODO(...): Support fine-grained check for backward compatibility. Not all QAT models
        // support there ability.
        if (caps.statelessLZ4Compression == 0U || caps.statelessLZ4Decompression == 0U ||
            caps.statelessDeflateCompression == 0U || caps.statelessDeflateDecompression == 0U ||
            caps.statelessLZ4SCompression == 0U || caps.checksumXXHash32 == 0U ||
            caps.checksumAdler32 == 0U || caps.compressAndVerify == 0U ||
            caps.compressAndVerifyAndRecover == 0U) {
            return NotSupportedError(
                "QAT Capability check failed. Vesal requires all capabilities of LZ4, LZ4S, "
                "Deflate, XXHash32, Adler32, CompressAndVerify, CompressAndVerifyAndRecover");
        }

        qat_unit_attr_.is_lz4_compression = caps.statelessLZ4Compression;
        qat_unit_attr_.is_lz4_decompression = caps.statelessLZ4Decompression;
        qat_unit_attr_.is_zstd_compression = caps.statelessLZ4SCompression;
        // zstd decomp is by software.
        qat_unit_attr_.is_zstd_decompression = false;
        qat_unit_attr_.is_deflate_compression = caps.statelessDeflateCompression;
        qat_unit_attr_.is_deflate_decompression = caps.statelessDeflateDecompression;
        qat_unit_attr_.is_crc_supp = caps.checksumXXHash32 && caps.checksumAdler32;
    }

    return OkStatus();
}

inline bool IsModelSupported(const std::string& name) {
    return std::any_of(kSupportedQatModelName.begin(),
                       kSupportedQatModelName.end(),
                       [&name](const char* s) { return name == s; });
}

int IntAtTheEnd(const std::string& s) {
    int num = 0;
    for (int i = s.length() - 1; i >= 0; i--) {
        if (s[i] >= '0' && s[i] <= '9') {
            num = num * 10 + (s[i] - '0');
        } else {
            break;
        }
    }
    return num;
}

Status QatUnit::QueryInformation() {
    CpaInstanceInfo2 info2;
    auto GetInstanceInfo2 = unit_type_ == UnitType::kDc
                                ? &QatHardwareApiWrapper::QAT_cpaDcInstanceGetInfo2
                                : &QatHardwareApiWrapper::QAT_cpaCyInstanceGetInfo2;
    CpaStatus st = (GetQatApiWrapper()->*GetInstanceInfo2)(cpa_instance_handle_, &info2);
    if (CPA_STATUS_SUCCESS != st) {
        return CpaStatusToVesalStatus(st, "Fail to CpaInstanceInfo2");
    }

    std::string part_name(reinterpret_cast<char*>(info2.partName));
    part_name = part_name.substr(0, part_name.find_first_of(' '));
    qat_unit_attr_.part_name = part_name;
    if (!IsModelSupported(part_name)) {
        return NotSupportedError("Not supported qat model: " + part_name);
    }
    std::string inst_name(reinterpret_cast<char*>(info2.instName));
    qat_unit_attr_.inst_id = IntAtTheEnd(inst_name);
    qat_unit_attr_.function_id = info2.physInstId.busAddress & 0xff;
    qat_unit_attr_.device_id = info2.physInstId.busAddress >> 8;
    // {VF id}.{InstName}, eg. "0.Dc1"
    qat_unit_attr_.instance_id = std::to_string(qat_unit_attr_.function_id) + "." + inst_name;
    qat_unit_attr_.is_phys_contiguous_mem_required =
        (info2.requiresPhysicallyContiguousMemory != 0U);
    qat_unit_attr_.numa_hint = info2.nodeAffinity;
    qat_unit_attr_.is_polled = (info2.isPolled != 0U);

    VESAL_LOG(DEBUG) << "qat_unit_attr_.is_phys_contiguous_mem_required="
                     << qat_unit_attr_.is_phys_contiguous_mem_required
                     << ", qat_unit_attr_.device_id=" << qat_unit_attr_.device_id
                     << ", qat_unit_attr_.numa_hint=" << qat_unit_attr_.numa_hint
                     << ", qat_unit_attr_.is_polled=" << qat_unit_attr_.is_polled;

    return OkStatus();
}

Status QatUnit::Start() {
    // 1. Query capabilities for instance.
    Status st = QueryCapabilities();
    if (!st.ok()) {
        return st;
    }

    // 2. Query information for instance.
    st = QueryInformation();
    if (!st.ok()) {
        return st;
    }
    // 3. Set the address translation function for the instance
    auto SetAddrTrans = unit_type_ == UnitType::kDc
                            ? &QatHardwareApiWrapper::QAT_cpaDcSetAddressTranslation
                            : &QatHardwareApiWrapper::QAT_cpaCySetAddressTranslation;
    CpaStatus cpa_st = (GetQatApiWrapper()->*SetAddrTrans)(cpa_instance_handle_, LookUpAddr);
    if (CPA_STATUS_SUCCESS != cpa_st) {
        return CpaStatusToVesalStatus(cpa_st, "Fail to cpaDcSetAddressTranslation");
    }
    // 4. Start Instance
    cpa_st = unit_type_ == UnitType::kDc
                 ? GetQatApiWrapper()->QAT_cpaDcStartInstance(cpa_instance_handle_, 0, nullptr)
                 : GetQatApiWrapper()->QAT_cpaCyStartInstance(cpa_instance_handle_);
    if (CPA_STATUS_SUCCESS != cpa_st) {
        return CpaStatusToVesalStatus(cpa_st, "Fail to cpaDcStartInstance");
    }

    VESAL_LOG(DEBUG) << "QAT device_id=" << qat_unit_attr_.device_id
                     << " started, SVM enbaled: " << SvmEnabled();

    return OkStatus();
}

Status QatUnit::Stop() {
    auto StopInstance = unit_type_ == UnitType::kDc ? &QatHardwareApiWrapper::QAT_cpaDcStopInstance
                                                    : &QatHardwareApiWrapper::QAT_cpaCyStopInstance;
    CpaStatus cpa_st = (GetQatApiWrapper()->*StopInstance)(cpa_instance_handle_);
    if (CPA_STATUS_SUCCESS != cpa_st) {
        return CpaStatusToVesalStatus(cpa_st, "Fail to cpaDcStopInstance");
    }
    return OkStatus();
}

}  // namespace qat
}  // namespace vesal
