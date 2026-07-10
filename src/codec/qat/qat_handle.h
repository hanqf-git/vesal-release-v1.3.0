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

#include "codec/qat/qat_codec_engine.h"
#include "common/qat/qat_session.h"
#include "common/qat/qat_unit.h"
#include "flat_hash_map.hpp"
#include "vesal/codec.h"

namespace vesal {
namespace qat {

// QatHandle wraps up the qat abilities and its data structures. It's used to provide the qat
// functions, like submit and poll the result. It also shadows the 'CpaStatus' and 'CpaDcReqStatus'
// and exposes 'vesal::StatusCode'. So the users at uplayer side don't need to care about the qat
// error handling and its real APIs.
class QatHandle {
public:
    QatHandle(const CodecChannelOption& channel_opts, QatUnitManager* unit_manager)
        : unit_(nullptr), unit_manager_(unit_manager), channel_opts_(channel_opts) {}

    // TODO(sjj): Hide QatUnitPool into QatCode. Get a Unit from the pool and init a session
    // with it.
    Status Init();
    // Unit might be failed due to session hanging. In this case the unit will not be returned
    // to the pool. Just ignore it. Otherwise the session shall be closed, any inflight requests
    // will be flushed, and the unit will be returned to the pool.
    Status Uninit();
    // Try to grab a new instance and init it. Until we tried all physical devices, then we return
    // kHardwareError. Otherwise return kOk.
    StatusCode Reinit();

    // Submit the request to qat.
    StatusCode SubmitAsync(RequestCbContext* cb_ctx,
                           CodecDirection dir,
                           const CodecSessionOption& opt);

    // quoto = 0 means no quota limitation. Poll as much as possible.
    StatusCode PollInstance(int quota = 0);

    StatusCode DumpAllRings();
    StatusCode DumpHwRegs();
    StatusCode DumpDebugInfo();

    QatUnit* GetUnit() {
        return unit_;
    }

private:
    static uint16_t PackSessionKey(CodecAlgorithm algo, CodecCompLevel level) {
        return static_cast<uint16_t>(static_cast<uint8_t>(algo)) |
               (static_cast<uint16_t>(static_cast<uint8_t>(level)) << 8);
    }

    StatusCode GetOrCreateSession(const CodecSessionOption& opt, void** session);

    // Close session for max 1ms. Return the close result.
    StatusCode TryCloseSession(QatSession* session);

    QatUnit* unit_;  // life cycle managed by QatPool
    QatUnitManager* unit_manager_;
    CodecChannelOption channel_opts_;
    // discarded_sessions_ and discarded_units_ are used to store the retired sessions and units
    // upon HA. Won't close/touch them until closing the whole QatHandle, to avoid any possible
    // issues. On closing, close session, then close units and put them into blacklist.
    std::vector<QatSession*> discarded_sessions_;
    std::vector<QatUnit*> discarded_units_;
    ska::flat_hash_map<uint16_t, std::unique_ptr<QatSession>> session_cache_;
};

}  // namespace qat
}  // namespace vesal
