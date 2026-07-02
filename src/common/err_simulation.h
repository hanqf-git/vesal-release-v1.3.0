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

#include <cpa.h>
#include <dc/cpa_dc.h>

#include <cstdint>
#include <cstring>
#include <mutex>

#include "codec/qat/qat_error_handling.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {

// clang-format off
/**
Explain the meaning of err sim msg:                                                                                                                                         
+---------------------------------------------+-----------------------+                                                                  
|                                             |                       |                                                                  
|                  17 bytes                   |       7 bytes         |                                                                  
+---------------------------------------------+-----------------------+                                                                  
|           VESAL_ERR_SIM_UDS_MSG             |       RESERVED        |                                                                  
|<------------------------------------------->|<--------------------->|                                                                  

For QAT, only the first 17 bytes are used. 'QDC' is the magic. Every byte of it is clear enough, except the 'flags' bit has a little bit complex. 
|  'Q'  |  'D'  |  'C'  | flags | pf_id | vf_id |inst_id| type  |  err  |                 repeat_times/hang_time_us                     |
+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+-------+
|                             9 bytes                                   |                          8 bytes                              |
|<--------------------------------------------------------------------->|<------------------------------------------------------------->|

The 'flags' byte's meaning is:
+---------------------+----------+                                                                                         
|                     |          |                                                                                         
|       reserved      |selection |                                                                                         
|                     |          |                                                                                         
+---------------------+----------+                                                                                         
|       6 bits        |   2 bits |                                                                                         
|<------------------->|<-------->|                                                                                         
|                     |          |                                                                                         
 */
// clang-format on
#define VESAL_ERR_SIM_UDS_MAGIC_LEN 3u
#define VESAL_ERR_SIM_UDS_MSG_LEN 24u

#define VESAL_QAT_ERR_SIM_UDS_MAGIC "QDC"
#define VESAL_QAT_ERR_SIM_OK 0u
#define VESAL_QAT_ERR_SIM_TIMEOUT_ERROR 6u
#define VESAL_QAT_ERR_SIM_IS_TIMEOUT(code) ((code) == 6u)
#define VESAL_QAT_ERR_SIM_IS_FIT_CPA_STATUS(code) \
    ((code) <= (CPA_STATUS_SUCCESS) && (code) >= (CPA_STATUS_RESTARTING))
#define VESAL_QAT_ERR_SIM_IS_FIT_CPA_DC_REQ_STATUS(code) \
    ((code) <= (CPA_DC_OK) && (code) >= (CPA_DC_LZ4_DISTANCE_OUT_OF_RANGE_ERR))

#define VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_ALL 0u
#define VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_PF 1u
#define VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_VF 2u
#define VESAL_QAT_ERR_SIM_FLAGS_SPECIFY_INST 3u

#define VESAL_QAT_ERR_SIM_FLAGS_OP_INJECT 0u
#define VESAL_QAT_ERR_SIM_FLAGS_OP_LIST 1u

static_assert(sizeof(VESAL_QAT_ERR_SIM_UDS_MAGIC) - 1 == VESAL_ERR_SIM_UDS_MAGIC_LEN,
              "VESAL_QAT_ERR_SIM_UDS_MAGIC_LEN is not equal to VESAL_ERR_SIM_UDS_MAGIC_LEN");

static_assert(VESAL_QAT_ERR_SIM_OK == CPA_STATUS_SUCCESS,
              "CPA_STATUS_SUCCESS is not equal to VESAL_QAT_ERR_SIM_OK");
static_assert(VESAL_QAT_ERR_SIM_TIMEOUT_ERROR == static_cast<int>(StatusCode::kTimeout),
              "Status::kTimeout is not equal to VESAL_QAT_ERR_SIM_TIMEOUT_ERROR");
static_assert(VESAL_QAT_ERR_SIM_IS_TIMEOUT(VESAL_QAT_ERR_SIM_TIMEOUT_ERROR),
              "VESAL_QAT_ERR_SIM_TIMEOUT_ERROR is not timeout");

using QatErrSimCode = int8_t;
using QatErrSimCnt = uint64_t;

enum class QatErrSimType : uint8_t { kSubmit = 1, kPoll = 2, kResult = 3, kNum = 4 };

namespace qat {

class QatUnit;

}

inline StatusCode QatErrSimCodeToVesalStatusCode(QatErrSimCode code) {
    VESAL_DCHECK(code >= static_cast<QatErrSimCode>(StatusCode::kOk) &&
                 code <= static_cast<QatErrSimCode>(StatusCode::kUnknown))
        << "code is not a valid QatErrSimCode, code: " << code;
    return static_cast<StatusCode>(code);
}

// Qat error simulation structure. Holding the information of a simulated error. err_cnt shall
// substract 1 after every GetErr for non-timeout error. If err_cnt is 0, or the hang is over,
// Reset() is called.
class QatErrSim {
    friend std::ostream& operator<<(std::ostream& os, const QatErrSim& err_sim);

public:
    QatErrSim(qat::QatUnit* qat_unit) : qat_unit_(qat_unit) {
        Reset();
    }

    bool operator==(const QatErrSim& other) const {
        return err_type_ == other.err_type_ && err_code_ == other.err_code_;
    }

    // If code is TimeoutError, the second return value is the timestamp after which the hang is
    // over. Otherwise, the second value is 0.
    std::pair<QatErrSimCode, uint64_t> GetErr(QatErrSimType type);

    void SetError(QatErrSimType type, QatErrSimCode code, QatErrSimCnt cnt);

private:
    // Uds message part start
    QatErrSimType err_type_;
    QatErrSimCode err_code_;
    QatErrSimCnt err_cnt_;
    uint64_t err_hang_until_ts_;
    // Uds message part end

    std::mutex mutex_;
    vesal::qat::QatUnit* qat_unit_;

    // Note the following function is not thread safe.
    void Reset() {
        err_type_ = QatErrSimType::kNum;
        err_code_ = VESAL_QAT_ERR_SIM_OK;
        err_cnt_ = 0;
        err_hang_until_ts_ = 0;
    }
};

std::string GetQatUdsSocketPath(const std::string& section_name);

uint8_t PackQatErrSimUdsFlags(uint8_t selection, uint8_t op);

// 'cnt' is the repeat times for non-timeout eror, or hang time in us for timeout error.
std::string PackQatErrSimUdsMsg(uint8_t flags,
                                uint8_t pf_id,
                                uint8_t vf_id,
                                uint8_t inst_id,
                                QatErrSimType type,
                                QatErrSimCode code,
                                QatErrSimCnt cnt);

Status ErrSimHandler(const std::string& msg, std::string* resp = nullptr);

}  // namespace vesal