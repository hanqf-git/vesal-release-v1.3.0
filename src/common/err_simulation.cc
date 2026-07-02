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

#include "common/err_simulation.h"

#include <arpa/inet.h>
#include <byteswap.h>

#include <cstring>
#include <iostream>

#include "codec/codec_internal.h"
#include "codec/qat/qat_codec.h"
#include "common/qat/qat_unit.h"
#include "common/timestamp.h"

namespace vesal {

#define VESAL_QAT_ERR_SIM_SPECIFY_TYPE(flags) ((flags)&0b11)
#define VESAL_QAT_ERR_SIM_OP_TYPE(flags) (((flags)&0b100) >> 2)

std::ostream& operator<<(std::ostream& os, const QatErrSim& err_sim) {
    os << "err_type: " << static_cast<int32_t>(err_sim.err_type_)
       << ", err_code: " << static_cast<int32_t>(err_sim.err_code_)
       << ", err_cnt: " << err_sim.err_cnt_
       << ", err_hang_until_ts: " << err_sim.err_hang_until_ts_;
    return os;
}

// big endian order to host order
inline uint64_t ntohll(uint64_t x) {
    if (__BYTE_ORDER == __LITTLE_ENDIAN) {
        return bswap_64(x);
    }
    return x;
}

// host order to big endian order
inline uint64_t htonll(uint64_t x) {
    if (__BYTE_ORDER == __LITTLE_ENDIAN) {
        return bswap_64(x);
    }
    return x;
}

uint8_t PackQatErrSimUdsFlags(uint8_t selection, uint8_t op) {
    uint8_t ret = 0;
    ret |= selection & 0b11;
    ret |= (op << 2) & 0b100;
    return ret;
}

std::string PackQatErrSimUdsMsg(uint8_t flags,
                                uint8_t pf_id,
                                uint8_t vf_id,
                                uint8_t inst_id,
                                QatErrSimType type,
                                QatErrSimCode code,
                                QatErrSimCnt cnt) {
    std::string msg(VESAL_ERR_SIM_UDS_MSG_LEN, '\0');
    memcpy(&msg[0], VESAL_QAT_ERR_SIM_UDS_MAGIC, VESAL_ERR_SIM_UDS_MAGIC_LEN);
    size_t curr = VESAL_ERR_SIM_UDS_MAGIC_LEN;
    msg[curr++] = flags;
    msg[curr++] = pf_id;
    msg[curr++] = vf_id;
    msg[curr++] = inst_id;
    msg[curr++] = static_cast<char>(type);
    msg[curr++] = static_cast<char>(code);
    cnt = htonll(cnt);
    memmove(&msg[curr], &cnt, 8);
    return msg;
}

std::pair<QatErrSimCode, uint64_t> QatErrSim::GetErr(QatErrSimType type) {
    std::lock_guard<std::mutex> lg(mutex_);
    if (type != err_type_ || err_code_ == VESAL_QAT_ERR_SIM_OK) {
        return {VESAL_QAT_ERR_SIM_OK, 0};
    }
    QatErrSimCode code = err_code_;
    VESAL_DCHECK(err_cnt_ > 0);
    if (VESAL_QAT_ERR_SIM_IS_TIMEOUT(code)) {
        if (TimeStamp::Now() > err_hang_until_ts_) {
            err_cnt_ = 0;
        }
    } else {
        err_cnt_--;
    }
    if (err_cnt_ == 0) {
        Reset();
        VESAL_LOG(INFO) << "Clear error simulation, type: " << static_cast<int32_t>(err_type_)
                        << ", code: " << static_cast<int32_t>(err_code_) << " for " << *qat_unit_;
    }
    return {code, err_hang_until_ts_};
}

void QatErrSim::SetError(QatErrSimType type, QatErrSimCode code, QatErrSimCnt cnt) {
    {
        std::lock_guard<std::mutex> lg(mutex_);
        err_type_ = type;
        err_code_ = code;
        err_cnt_ = cnt;
        if (VESAL_QAT_ERR_SIM_IS_TIMEOUT(code)) {
            err_hang_until_ts_ = TimeStamp::Now() + TimeStamp::UsToDuration(cnt);
        } else {
            err_hang_until_ts_ = 0;
        }
    }
    VESAL_LOG(DEBUG) << "Set error simulation " << *this << " for " << *qat_unit_;
}

std::string GetQatUdsSocketPath(const std::string& section_name) {
    return "/dev/vesal_errsim_qat_dc_" + section_name + ".sock";
}

static std::vector<qat::QatUnit*> GetQatUnit(uint8_t specify_type,
                                             uint8_t pf_id,
                                             uint8_t vf_id,
                                             uint8_t inst_id) {
    auto qat_codec = g_qat_codec.get();
    if (!qat_codec) {
        VESAL_LOG(ERROR) << "qat codec is nullptr";
        return {};
    }
    switch (specify_type) {
    case VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL:
        return qat_codec->GetAllUnits();
    case VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_PF:
        return qat_codec->GetUnitWithDeviceId(pf_id);
    case VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_VF:
        return qat_codec->GetUnitWithFunctionId(pf_id, vf_id);
    case VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_INST:
        return {qat_codec->GetUnitWithInstId(pf_id, vf_id, inst_id)};
    }
    VESAL_CHECK(false) << "Should not reach here";
    return {};
}

static Status HandleQatErrSim(const std::string& msg, std::string* resp) {
    size_t curr = VESAL_ERR_SIM_UDS_MAGIC_LEN;
    uint8_t flags = msg[curr++];
    uint8_t pf_id = msg[curr++];
    uint8_t vf_id = msg[curr++];
    uint8_t inst_id = msg[curr++];
    auto specify_type = VESAL_QAT_ERR_SIM_SPECIFY_TYPE(flags);
    auto op = VESAL_QAT_ERR_SIM_OP_TYPE(flags);
    QatErrSimType type = static_cast<QatErrSimType>(msg[curr++]);
    QatErrSimCode code = static_cast<QatErrSimCode>(msg[curr++]);
    QatErrSimCnt cnt = 0;
    memmove(&cnt, &msg[curr], 8);
    cnt = ntohll(cnt);
    curr += 8;

    std::vector<qat::QatUnit*> units = GetQatUnit(specify_type, pf_id, vf_id, inst_id);
    if (op == VESAL_QAT_ERR_SIM_FLAGS_OP_LIST) {
        std::stringstream ss;
        for (auto unit : units) {
            ss << *unit << std::endl;
        }
        *resp = ss.str();
        return OkStatus();
    }

    if (units.empty()) {
        return InvalidArgumentError("invalid msg id");
    }
    if (VESAL_QAT_ERR_SIM_IS_TIMEOUT(code) && type != QatErrSimType::kResult) {
        return InvalidArgumentError("invalid timeout msg type");
    }

    if (cnt == 0 && code != VESAL_QAT_ERR_SIM_OK) {
        return InvalidArgumentError("invalid msg cnt");
    }
    for (auto unit : units) {
        unit->SetQatErrSim(type, code, cnt);
    }
    if (resp) {
        *resp = "OK";
    }
    return OkStatus();
}

Status ErrSimHandler(const std::string& msg, std::string* resp) {
    if (msg.size() != VESAL_ERR_SIM_UDS_MSG_LEN) {
        return InvalidArgumentError("invalid msg len=" + std::to_string(msg.size()));
    }
    Status ret;
    if (memcmp(msg.data(), VESAL_QAT_ERR_SIM_UDS_MAGIC, VESAL_ERR_SIM_UDS_MAGIC_LEN) == 0) {
        // QAT case, add more case in the future following here.
        ret = HandleQatErrSim(msg, resp);
    } else {
        ret =
            InvalidArgumentError("invalid msg magic " + msg.substr(0, VESAL_ERR_SIM_UDS_MAGIC_LEN));
    }
    return ret;
}

}  // namespace vesal
