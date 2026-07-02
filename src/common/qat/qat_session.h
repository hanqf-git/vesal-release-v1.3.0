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

extern "C" {
#include <cpa.h>
#include <cpa_cy_sym.h>
}

#include "qat_unit.h"
#include "vesal/codec.h"
#include "vesal/cypher.h"

namespace vesal {
namespace qat {

const int SHA256_DST_LEN = 32;

typedef void (*CpaCallbackFn)(void*, CpaStatus);

enum class QatSessionType : uint8_t { kCodec = 1, kCypher = 2, kNum };

struct QatSessionOption {
    CodecChannelOption codec_chann_opt;
    struct {
        CypherSessionOption session_opt;
        CypherOp op;
    } SymOption;
    QatSessionType type{QatSessionType::kCodec};

    QatSessionOption() {}
    QatSessionOption(const CodecChannelOption& channel_option) {
        codec_chann_opt = channel_option;
        type = QatSessionType::kCodec;
    }
    QatSessionOption(const CypherSessionOption& session_option, CypherOp op) {
        SymOption.session_opt = session_option;
        SymOption.op = op;
        type = QatSessionType::kCypher;
    }
};

class QatSession {
public:
    explicit QatSession(QatUnit* unit);

    ~QatSession();

    Status Init(const QatSessionOption& sess_opts, CpaCallbackFn cb);
    Status Init(const QatSessionOption& sess_opts, CpaCySymCbFunc cb);

    CpaDcSessionHandle GetSessionHandle() {
        return cpa_dc_session_handle_;
    }

    CpaCySymSessionCtx GetSessionCtx() {
        return cpa_sym_session_ctx_;
    }

    Status Close();

    const CodecAlgorithm& codec_algo() const {
        return codec_algo_;
    }

    const CodecCompLevel& comp_level() const {
        return comp_level_;
    }

private:
    void SetQatSessionData(const QatSessionOption& sess_opts, CpaDcSessionSetupData* sess_data);

    void SetQatSessionData(const QatSessionOption& sess_opts, CpaCySymSessionSetupData* sess_data);

    bool CheckCapabilities(const QatSessionOption& opt);

    Status InitCodecSession(const QatSessionOption& sess_opts, CpaCallbackFn cb);

    Status InitCypherSession(const QatSessionOption& sess_opts, CpaCySymCbFunc cb);

    QatUnit* qat_unit_;
    CpaDcSessionHandle cpa_dc_session_handle_;
    CpaCySymSessionCtx cpa_sym_session_ctx_;
    // sizeof(CpaCrcControlData) = 32, safe to use inplace instance here
    CpaCrcControlData cpa_crc_control_data_;
    std::unique_ptr<unsigned char[]> sym_cy_key_;
    CodecAlgorithm codec_algo_;
    CodecCompLevel comp_level_;
    bool closed_;
};

}  // namespace qat
}  // namespace vesal
