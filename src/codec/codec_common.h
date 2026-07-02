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

#ifdef VESAL_ENABLE_ERR_SIM
#include "common/err_simulation.h"
#endif
#include "common/object_pool.h"
#include "common/timestamp.h"
#include "vesal/codec.h"

namespace vesal {

enum class CodecDataDirection : uint8_t { kInput, kOutput, kNum };

inline std::ostream& operator<<(std::ostream& os, CodecDirection direction) {
    switch (direction) {
    case CodecDirection::kComp:
        os << "kComp";
        break;
    case CodecDirection::kDecomp:
        os << "kDecomp";
        break;
    case CodecDirection::kNum:
        os << "kNum";
        break;
    }
    return os;
}

// for SGL APIs, the maximum number of scattered buffers
#define VESAL_MAX_SGL_NUM 32

StatusCode ValidateSglArgs(const std::vector<unsigned char*>& src,
                           const std::vector<unsigned int>& src_len);

// codec result list
// used to restore compress and decompress results.
struct CodecResultList {
#ifdef VESAL_ENABLE_ERR_SIM
    // If now we have not reached err_sim_onfire_ts, we hold the requests. Otherwise we return as
    // normal. If err_sim_onfire_ts is later than onfire_ts, we use the onfire_ts as the "real"
    // timeout. That is, the maximum timeout is onfire_ts and the minimum process time is
    // min(onfire_ts, err_sim_onfire_ts).
    struct ErrSimContext {
        QatErrSimCode err_sim_code;
        uint64_t err_sim_onfire_ts;
        uint64_t onfire_ts;

        ErrSimContext() : err_sim_code(VESAL_QAT_ERR_SIM_OK), err_sim_onfire_ts(0), onfire_ts(0) {}
        ErrSimContext(QatErrSimCode err_sim_code, uint64_t err_sim_onfire_ts, uint64_t onfire_ts)
            : err_sim_code(err_sim_code),
              err_sim_onfire_ts(err_sim_onfire_ts),
              onfire_ts(onfire_ts) {}
    };
#endif
    struct CodecResultNode {
        CodecResult result;
#ifdef VESAL_ENABLE_ERR_SIM
        ErrSimContext err_sim_ctx;
#endif
        CodecResultNode* next = nullptr;
    };

    // Pop from head node
    CodecResultNode* head = nullptr;
    // Push to tail node
    CodecResultNode* tail = nullptr;
    // Node number in list
    size_t num = 0;

    ~CodecResultList() {
        while (num--) {
            auto* tmp = head;
            head = head->next;
            vesal::return_tls_object<CodecResultNode>(tmp);
        }
    }

    void PushInternal(CodecResultNode* node) {
        if (0 == num) {  // empty list
            head = node;
            tail = node;
        } else {
            tail->next = node;
            tail = node;
        }
        num++;
    }

#ifdef VESAL_ENABLE_ERR_SIM
    void Push(void* user_ctx, const StatusCode& status, ErrSimContext&& err_sim_ctx) {
        CodecResultNode* node = vesal::get_tls_object<CodecResultNode>();
        node->result.status = status;
        node->result.ctx = user_ctx;
        node->result.consumed = 0;
        node->result.produced = 0;
        node->result.in_checksum = 0;
        node->result.out_checksum = 0;
        node->err_sim_ctx = err_sim_ctx;
        PushInternal(node);
    }

    void Push(void* user_ctx,
              const StatusCode& status,
              size_t consumed,
              size_t produced,
              const checksum64_t& in,
              const checksum64_t& out,
              ErrSimContext&& err_sim_ctx) {
        CodecResultNode* node = vesal::get_tls_object<CodecResultNode>();
        node->result.status = status;
        node->result.ctx = user_ctx;
        node->result.consumed = consumed;
        node->result.produced = produced;
        node->result.in_checksum = in;
        node->result.out_checksum = out;
        node->err_sim_ctx = std::move(err_sim_ctx);
        PushInternal(node);
    }

    void Push(void* user_ctx, const StatusCode& status) {
        ErrSimContext dummy;
        Push(user_ctx, status, std::move(dummy));
    }

    void Push(void* user_ctx,
              const StatusCode& status,
              size_t consumed,
              size_t produced,
              const checksum64_t& in,
              const checksum64_t& out) {
        ErrSimContext dummy;
        Push(user_ctx, status, consumed, produced, in, out, std::move(dummy));
    }

    // If returns false, the request is not ready to be popped.
    bool ProcessErrSim(uint64_t now, CodecResultNode* node) {
        QatErrSimCode code = node->err_sim_ctx.err_sim_code;
        if (code == VESAL_QAT_ERR_SIM_OK) {
            return true;
        }
        if (!VESAL_QAT_ERR_SIM_IS_TIMEOUT(code)) {
            // Not timeout, just modify the result status.
            node->result.status = QatErrSimCodeToVesalStatusCode(code);
            return true;
        }
        // Timeout error
        uint64_t min_onfire_ts = node->err_sim_ctx.onfire_ts < node->err_sim_ctx.err_sim_onfire_ts
                                     ? node->err_sim_ctx.onfire_ts
                                     : node->err_sim_ctx.err_sim_onfire_ts;
        if (now < min_onfire_ts) {
            // Not ready yet.
            return false;
        }
        if (now > node->err_sim_ctx.onfire_ts) {
            // Too late, even we we already got the result, still needs to fake it's timeout'd.
            node->result.status = StatusCode::kTimeout;
        }
        return true;
    }

    // Pop as we can, stop when we found the request should not be handed out to the caller.
    size_t PopIfReady(CodecResult results[], unsigned int max_num) {
        if (max_num > num) {
            max_num = num;
        }
        size_t ret = 0;
        auto now = TimeStamp::Now();
        for (size_t i = 0; i < max_num; ++i) {
            if (!ProcessErrSim(now, head)) {
                break;
            }
            results[i] = std::move(head->result);
            auto* tmp = head;
            head = head->next;
            vesal::return_tls_object<CodecResultNode>(tmp);
            ++ret;
        }
        num -= ret;
        if (0 == num) {
            head = nullptr;
            tail = nullptr;
        }
        return ret;
    }
#else
    void Push(void* user_ctx, const StatusCode& status) {
        CodecResultNode* node = vesal::get_tls_object<CodecResultNode>();
        node->result.status = status;
        node->result.ctx = user_ctx;
        node->result.consumed = 0;
        node->result.produced = 0;
        node->result.in_checksum = 0;
        node->result.out_checksum = 0;

        PushInternal(node);
    }

    void Push(void* user_ctx,
              const StatusCode& status,
              size_t consumed,
              size_t produced,
              const checksum64_t& in,
              const checksum64_t& out) {
        CodecResultNode* node = vesal::get_tls_object<CodecResultNode>();
        node->result.status = status;
        node->result.ctx = user_ctx;
        node->result.consumed = consumed;
        node->result.produced = produced;
        node->result.in_checksum = in;
        node->result.out_checksum = out;

        PushInternal(node);
    }
#endif
    size_t Pop(CodecResult results[], unsigned int max_num) {
        size_t ret = max_num > num ? num : max_num;
        for (size_t i = 0; i < ret; ++i) {
            results[i] = std::move(head->result);
            auto* tmp = head;
            head = head->next;
            vesal::return_tls_object<CodecResultNode>(tmp);
        }
        num -= ret;
        if (0 == num) {
            head = nullptr;
            tail = nullptr;
        }

        return ret;
    }

    size_t Size() const {
        return num;
    }

    // Used by HA to modify all the results in the list. Return the modified number.
    size_t ModifyAll(const StatusCode& change_to) {
        size_t ret = num;
        auto* node = head;
        for (size_t i = 0; i < num; ++i) {
            node->result.status = change_to;
            node = node->next;
        }
        return ret;
    }
};

}  // namespace vesal