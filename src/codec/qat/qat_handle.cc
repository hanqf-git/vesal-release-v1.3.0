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

#include "codec/qat/qat_handle.h"

#include <functional>

#include "codec/qat/qat_error_handling.h"
#include "common/qat/qat_session.h"
#include "vesal/codec.h"
#include "vesal/status.h"

namespace vesal {
namespace qat {

Status QatHandle::Init() {
    QatUnitSelection selection;
    selection.numa_id = channel_opts_.allocation_option.node_affinity;
    selection.pf_id = channel_opts_.allocation_option.device_id;
    unit_ = unit_manager_->GrabAvailableUnit(selection);
    if (unit_ == nullptr) {
        return ResourceBusyError("No available QAT unit.");
    }
    VESAL_LOG(INFO) << "Grabed " << *unit_;
    CodecSessionOption default_opt;
    default_opt.codec_algorithm = channel_opts_.comp_algorithm;
    default_opt.comp_level = channel_opts_.comp_level;
    void* default_session = nullptr;
    auto r = GetOrCreateSession(default_opt, &default_session);
    if (!IsOk(r)) {
        Uninit();
        return {r, "Failed to create default session."};
    }
    return OkStatus();
}

Status QatHandle::Uninit() {
    for (auto& kv : session_cache_) {
        StatusCode r = TryCloseSession(kv.second.get());
        if (!IsOk(r)) {
            // Something wrong with closing session. For now we follow the old semantic and not
            // allow to close.
            // TODO(sjj): Change the behaviour, handle the error this layer because user can do
            // nothing.
            VESAL_LOG(WARN)
                << "Fail to close session, usually due to flying requests not cleared, r=" << r;
            return {r, "Fail to close session."};
        }
    }
    session_cache_.clear();
    for (auto* sess : discarded_sessions_) {
        // For these hanging sessions we just try to close them and avoid calling the callback.
        sess->Close();
        delete sess;
    }
    discarded_sessions_.clear();
    if (unit_ != nullptr) {
        // We can safely put the unit back to the pool despite whether the session is
        // closed. Because a unit can bind to multiple sessions, there will be no conflict as long
        // as we don't use the unit in multithreads.
        unit_manager_->PutBackUnit(unit_);
        unit_ = nullptr;
    }
    for (auto* unit : discarded_units_) {
        unit_manager_->PutBackToBlackList(unit);
    }
    discarded_units_.clear();
    return OkStatus();
}

StatusCode QatHandle::Reinit() {
    // Don't close session as it might be hanging.
    for (auto& kv : session_cache_) {
        discarded_sessions_.push_back(kv.second.release());
    }
    session_cache_.clear();
    discarded_units_.push_back(unit_);
    unit_ = nullptr;
    do {
        unit_ = unit_manager_->GrabFromDiffDevice(discarded_units_);
        if (unit_ != nullptr) {
            CodecSessionOption default_opt;
            default_opt.codec_algorithm = channel_opts_.comp_algorithm;
            default_opt.comp_level = channel_opts_.comp_level;
            void* default_session = nullptr;
            auto r = GetOrCreateSession(default_opt, &default_session);
            if (IsOk(r)) {
                return StatusCode::kOk;
            }
            // TODO(sjj): Might need more delicated handling here, i.e do different things for
            // different error types. For example, if the error is due to memory inefficiency, retry
            // might not be useful. But at this point we don't know much detail of QAT driver so
            // it's hard to tell whether the error is really memory inefficiency or not. Even we
            // can, we still don't know what to do. So just try a different instance and give it a
            // shot.
            VESAL_LOG(WARN) << "Fail to init session, r=" << r << " for QatUnit: " << *unit_;
            discarded_units_.push_back(unit_);
        }
    } while (unit_ != nullptr);
    return StatusCode::kHardwareError;
}

StatusCode QatHandle::GetOrCreateSession(const CodecSessionOption& opt, void** session) {
    auto key = PackSessionKey(opt.codec_algorithm, opt.comp_level);
    auto it = session_cache_.find(key);
    if (it != session_cache_.end()) {
        *session = it->second.get();
        return StatusCode::kOk;
    }
    CodecChannelOption channel_opt = channel_opts_;
    channel_opt.comp_algorithm = opt.codec_algorithm;
    channel_opt.comp_level = opt.comp_level;
    auto sess = std::make_unique<QatSession>(unit_);
    Status sess_r = sess->Init(QatSessionOption(channel_opt), Callback);
    if (!sess_r.ok()) {
        VESAL_LOG(WARN) << "Failed to create session for option: algo="
                        << static_cast<int>(opt.codec_algorithm)
                        << ", level=" << static_cast<int>(opt.comp_level) << ", r=" << sess_r;
        return sess_r.code();
    }
    QatSession* sess_ptr = sess.get();
    session_cache_[key] = std::move(sess);
    *session = sess_ptr;
    return StatusCode::kOk;
}

StatusCode QatHandle::SubmitAsync(RequestCbContext* cb_ctx,
                                  CodecDirection dir,
                                  const CodecSessionOption& opt) {
    void* session = nullptr;
    StatusCode sess_r = GetOrCreateSession(opt, &session);
    if (!IsOk(sess_r)) {
        return sess_r;
    }
    CpaInstanceHandle* inst_hdl = unit_->GetInstanceHandle();
    QatSession* sess = static_cast<QatSession*>(session);
    CpaDcSessionHandle sess_hdl = sess->GetSessionHandle();
    CpaBufferList* src_buff_list = cb_ctx->src_qat->GetCpaBufferList();
    CpaBufferList* dst_buff_list = cb_ctx->dst_qat->GetCpaBufferList();
    CpaDcOpData* op_data = &cb_ctx->op_data;
    CpaDcRqResults* results = &cb_ctx->cpa_results;
    auto* api_wrapper = GetQatApiWrapper();
    CpaStatus cpa_status = CPA_STATUS_SUCCESS;
    auto api = dir == CodecDirection::kComp ? &QatHardwareApiWrapper::QAT_cpaDcCompressData2
                                            : &QatHardwareApiWrapper::QAT_cpaDcDecompressData2;
#ifdef VESAL_ENABLE_ERR_SIM
    StatusCode vesal_status = StatusCode::kOk;
    if (FLAGS_vesal_enable_err_sim) {
        QatErrSimCode code = unit_->GetQatErrSim(QatErrSimType::kSubmit).first;
        vesal_status = QatErrSimCodeToVesalStatusCode(code);
    }
    if (vesal_status != StatusCode::kOk) {
        // Fast return if we injected a vesal layer error.
        return vesal_status;
    }
#endif
    cpa_status = (api_wrapper->*api)(
        *inst_hdl, sess_hdl, src_buff_list, dst_buff_list, op_data, results, cb_ctx);
    return CpaStatusToVesalStatusCode(cpa_status);
}
// quoto = 0 means no quota limitation. Poll as much as possible.
StatusCode QatHandle::PollInstance(int quota) {
    CpaInstanceHandle* inst_hdl = unit_->GetInstanceHandle();
    CpaStatus cpa_status = CPA_STATUS_SUCCESS;
#ifdef VESAL_ENABLE_ERR_SIM
    StatusCode vesal_status = StatusCode::kOk;
    if (FLAGS_vesal_enable_err_sim) {
        QatErrSimCode code = unit_->GetQatErrSim(QatErrSimType::kPoll).first;
        vesal_status = QatErrSimCodeToVesalStatusCode(code);
    }
    if (vesal_status != StatusCode::kOk) {
        // Fast return if we injected a vesal layer error.
        return vesal_status;
    }
#endif
    cpa_status = GetQatApiWrapper()->QAT_icp_sal_DcPollInstance(*inst_hdl, 0);
    return CpaStatusToVesalStatusCode(cpa_status);
}

StatusCode QatHandle::DumpAllRings() {
    if (unit_ == nullptr) {
        return StatusCode::kInvalidArgument;
    }
    CpaInstanceHandle* inst_hdl = unit_->GetInstanceHandle();
    auto unit_attr = unit_->GetQatUnitAttr();
    VESAL_LOG(INFO) << "Calling dcDumpAllRings before closing QAT engine, device="
                    << unit_attr.device_id << ", instance=" << unit_attr.instance_id;
    CpaStatus cpa_status = GetQatApiWrapper()->QAT_dcDumpAllRings(*inst_hdl);
    return CpaStatusToVesalStatusCode(cpa_status);
}

StatusCode QatHandle::DumpHwRegs() {
    if (unit_ == nullptr) {
        return StatusCode::kInvalidArgument;
    }
    CpaInstanceHandle* inst_hdl = unit_->GetInstanceHandle();
    auto unit_attr = unit_->GetQatUnitAttr();
    VESAL_LOG(INFO) << "Calling dcDumpHwRegs, device=" << unit_attr.device_id
                    << ", instance=" << unit_attr.instance_id;
    CpaStatus cpa_status = GetQatApiWrapper()->QAT_dcDumpHwRegs(*inst_hdl);
    return CpaStatusToVesalStatusCode(cpa_status);
}

StatusCode QatHandle::DumpDebugInfo() {
    StatusCode rings_status = DumpAllRings();
    StatusCode regs_status = DumpHwRegs();
    if (!IsOk(rings_status)) {
        return rings_status;
    }
    return regs_status;
}

StatusCode QatHandle::TryCloseSession(QatSession* session) {
    int cnt = 10;
    StatusCode ret = session->Close().code();
    while (!IsOk(ret) && cnt-- > 0) {
        usleep(100);
        PollInstance();
        ret = session->Close().code();
    }
    return ret;
}

}  // namespace qat
}  // namespace vesal
