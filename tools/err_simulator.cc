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
#include <gflags/gflags.h>

#include <algorithm>

#include "common/err_simulation.h"
#include "common/uds_listener.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"
#include "vesal/vesal.h"

DEFINE_string(section_name, "SSL0", "qat section name");
DEFINE_string(pf_id, "ff", "qat pf id in hex");
DEFINE_string(vf_id, "ff", "qat vf id in hex");
DEFINE_string(inst_id, "ff", "qat inst id in hex");
DEFINE_string(specify,
              "all",
              "specify qat unit to inject error. "
              "all: all QatUnits are specified. pf: only specified pf. "
              "vf: only specified vf. inst: only specified inst.");

DEFINE_string(op, "ls", "ls: list all QatUnits being used. inject: inject error.");
DEFINE_string(type, "submit", "error type, 1: submit, 2: poll, 3: result");
DEFINE_uint64(cnt,
              0,
              "error count. for non-timeout error, it's the count of error injection; "
              "for timeout error, it's the timeout in us");
DEFINE_int32(
    code,
    0,
    "See vesal::StatusCode for vesal error number. If zero, means clear the current error.");

namespace vesal {
inline static bool IsTwoDigitHex(const std::string& str) {
    return str.size() <= 2 && str.size() > 0 && std::all_of(str.begin(), str.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
           });
}

inline static int StrToHex(const std::string& str) {
    return std::stoi(str, nullptr, 16);
}

static bool ParseSpecify(uint8_t* selection, uint8_t* pf_id, uint8_t* vf_id, uint8_t* inst_id) {
    if (!IsTwoDigitHex(FLAGS_pf_id) || !IsTwoDigitHex(FLAGS_vf_id) ||
        !IsTwoDigitHex(FLAGS_inst_id)) {
        return false;
    }
    if (FLAGS_specify == "all") {
        *selection = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL;
    } else if (FLAGS_specify == "pf") {
        *selection = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_PF;
        *pf_id = StrToHex(FLAGS_pf_id);
    } else if (FLAGS_specify == "vf") {
        *selection = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_VF;
        *pf_id = StrToHex(FLAGS_pf_id);
        *vf_id = StrToHex(FLAGS_vf_id);
    } else if (FLAGS_specify == "inst") {
        *selection = VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_INST;
        *pf_id = StrToHex(FLAGS_pf_id);
        *vf_id = StrToHex(FLAGS_vf_id);
        *inst_id = StrToHex(FLAGS_inst_id);
    } else {
        return false;
    }
    return true;
}
static bool ParseOp(uint8_t* op) {
    if (FLAGS_op == "ls") {
        *op = VESAL_QAT_ERR_SIM_FLAGS_OP_LIST;
    } else if (FLAGS_op == "inject") {
        *op = VESAL_QAT_ERR_SIM_FLAGS_OP_INJECT;
    } else {
        return false;
    }
    return true;
}
static bool ParseType(QatErrSimType* type) {
    if (FLAGS_type == "submit") {
        *type = QatErrSimType::kSubmit;
    } else if (FLAGS_type == "poll") {
        *type = QatErrSimType::kPoll;
    } else if (FLAGS_type == "result") {
        *type = QatErrSimType::kResult;
    } else {
        return false;
    }
    return true;
}

static bool ParseCode(QatErrSimCode* code) {
    if (!IsStatusCode(FLAGS_code)) {
        return false;
    }
    *code = static_cast<QatErrSimCode>(FLAGS_code);
    return true;
}

static bool ParseTypeCodeCnt(QatErrSimType* type, QatErrSimCode* code, QatErrSimCnt* cnt) {
    if (!ParseType(type)) {
        return false;
    }
    if (!ParseCode(code)) {
        return false;
    }
    if (*type != QatErrSimType::kResult && VESAL_QAT_ERR_SIM_IS_TIMEOUT(*code)) {
        return false;
    }
    if (*code != VESAL_QAT_ERR_SIM_OK && FLAGS_cnt == 0) {
        return false;
    }
    *cnt = FLAGS_cnt;
    return true;
}
// TODO(sjj): add more logs
static bool ParseIntoMsg(std::string* msg) {
    uint8_t pf_id = 0xff;
    uint8_t vf_id = 0xff;
    uint8_t inst_id = 0xff;
    uint8_t selection = 0;
    uint8_t op = 0;
    if (!ParseSpecify(&selection, &pf_id, &vf_id, &inst_id)) {
        return false;
    }
    if (!ParseOp(&op)) {
        return false;
    }
    uint8_t flags = PackQatErrSimUdsFlags(selection, op);
    QatErrSimType type;
    int8_t code;
    QatErrSimCnt cnt;
    if (!ParseTypeCodeCnt(&type, &code, &cnt)) {
        return false;
    }
    *msg = PackQatErrSimUdsMsg(flags, pf_id, vf_id, inst_id, type, code, cnt);
    return true;
}

}  // namespace vesal

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    std::string msg;
    if (!vesal::ParseIntoMsg(&msg)) {
        VESAL_LOG(ERROR) << "Invalid args";
        return -1;
    }
    std::string resp;
    std::string uds_socket_path = vesal::GetQatUdsSocketPath(FLAGS_section_name);
    bool write_r = vesal::WriteUdsAndReadResponse(uds_socket_path, msg, &resp);
    if (!write_r) {
        VESAL_LOG(ERROR) << "Failed to write msg to uds, resp: " << resp;
    } else {
        VESAL_LOG(INFO) << "Success, resp: \n" << resp;
    }
    return 0;
}
