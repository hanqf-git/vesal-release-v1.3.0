/*
 * Copyright (c) 2025 ByteDance Inc.
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

#include <unistd.h>

#include <numeric>
#include <string>

#include "codec/codec_common.h"
#include "vesal/c_api/c_api_vesal.h"
#include "vesal/cypher.h"
#include "vesal/log_setting.h"

// Now ensure vesal::CodecResult and vesal_codec_result_t have the same layout.
static_assert(sizeof(vesal::CypherResult) == sizeof(vesal_cypher_result_t),
              "CypherResult and vesal_cypher_result_t size mismatch!");

static_assert(alignof(vesal::CypherResult) == alignof(vesal_cypher_result_t),
              "CypherResult and vesal_cypher_result_t alignment mismatch!");

void default_vesal_cypher_channel_options(vesal_cypher_channel_option_t* opts) {
    opts->engine_type = VESAL_CYPHER_ENGINE_QAT;
    opts->ha_policy = VESAL_HA_POLICY_NONE;
    opts->timeout_ms = 1000;
    opts->session_option.algorithm = VESAL_CYPHER_ALGORITHM_AES_XTS;
    opts->session_option.aes_xts_key = "";
    opts->session_option.key_len = 0;
    opts->user_cb = nullptr;
}

VESAL_ERROR_CODE vesal_create_cypher_channel(vesal_cypher_channel_option_t* opts,
                                             VesalCypherChannelHandle* handle) {
    vesal::CypherChannelOption internal_opts{};
    internal_opts.engine = static_cast<vesal::EngineType>(opts->engine_type);
    internal_opts.user_cb = reinterpret_cast<vesal::CypherUserCallback>(opts->user_cb);
    internal_opts.ha_policy = static_cast<vesal::HaPolicy>(opts->ha_policy);
    internal_opts.timeout_ms = opts->timeout_ms;
    internal_opts.session_option.algorithm =
        static_cast<vesal::CypherAlgorithm>(opts->session_option.algorithm);
    if (internal_opts.session_option.algorithm == vesal::CypherAlgorithm::kAES_XTS) {
        int key_len = opts->session_option.key_len;
        // AES key can only be 16 or 32
        if (key_len != 16 && key_len != 32) {
            VESAL_LOG(ERROR) << "Invalid key len for Aes, must be 16 or 32, but got " << key_len;
            return VESAL_INVALID_ARGUMENT;
        }
        internal_opts.session_option.aes_xts_spec.aes_xts_key1 =
            std::string(opts->session_option.aes_xts_key, key_len);
        internal_opts.session_option.aes_xts_spec.aes_xts_key2 =
            std::string(opts->session_option.aes_xts_key + key_len, key_len);
    }
    auto r = vesal::CypherChannel::CreateCypherChannel(internal_opts);
    if (!r.first.ok()) {
        return static_cast<VESAL_ERROR_CODE>(r.first.code());
    }
    *handle = r.second.release();
    return VESAL_OK;
}

void* vesal_cypher_add_session(VesalCypherChannelHandle handle,
                               vesal_cypher_session_option_t* opts) {
    VESAL_DCHECK(handle != nullptr);
    VESAL_DCHECK(opts != nullptr);
    if (handle != nullptr && opts != nullptr) {
        auto* chnnl = reinterpret_cast<vesal::CypherChannel*>(handle);
        vesal::CypherSessionOption internal_opts;
        internal_opts.algorithm = static_cast<vesal::CypherAlgorithm>(opts->algorithm);
        if (internal_opts.algorithm == vesal::CypherAlgorithm::kAES_XTS) {
            int key_len = opts->key_len;
            // AES key can only be 16 or 32
            if (key_len != 16 && key_len != 32) {
                VESAL_LOG(ERROR) << "Invalid key len for Aes, must be 16 or 32, but got "
                                 << key_len;
                return nullptr;
            }
            internal_opts.aes_xts_spec.aes_xts_key1 = std::string(opts->aes_xts_key, key_len);
            internal_opts.aes_xts_spec.aes_xts_key2 =
                std::string(opts->aes_xts_key + key_len, key_len);
        }
        return chnnl->AddSession(internal_opts);
    }
    return nullptr;
}

void vesal_cypher_remove_session(VesalCypherChannelHandle handle, void* session) {
    VESAL_DCHECK(handle != nullptr);
    VESAL_DCHECK(session != nullptr);
    if (handle != nullptr && session != nullptr) {
        auto* chnnl = reinterpret_cast<vesal::CypherChannel*>(handle);
        chnnl->RemoveSession(session);
    }
}

void vesal_destroy_cypher_channel(VesalCypherChannelHandle handle) {
    if (handle != nullptr) {
        auto* chnnl = reinterpret_cast<vesal::CypherChannel*>(handle);
        auto r = chnnl->Close();
        delete chnnl;
    }
}

VESAL_ERROR_CODE vesal_cypher_submit(VesalCypherChannelHandle handle,
                                     unsigned char* src,
                                     unsigned int src_len,
                                     unsigned char* dst,
                                     unsigned int dst_len,
                                     vesal_cypher_req_args_t* req) {
    auto* channel = reinterpret_cast<vesal::CypherChannel*>(handle);
    VESAL_DCHECK(channel != nullptr);
    vesal::CypherReqArgs internal_req{};
    internal_req.op = static_cast<vesal::CypherOp>(req->op);
    internal_req.ctx = req->ctx;
    internal_req.aes_xts_tweak = req->aes_xts_tweak;
    internal_req.session = req->session;
    auto r = channel->SubmitCypherReq(src, src_len, dst, dst_len, &internal_req);
    return static_cast<VESAL_ERROR_CODE>(r);
}

VESAL_ERROR_CODE vesal_cypher_submit_sgl(VesalCypherChannelHandle handle,
                                         unsigned char* src[],
                                         unsigned int src_len[],
                                         unsigned int src_num,
                                         unsigned char* dst,
                                         unsigned int dst_len,
                                         vesal_cypher_req_args_t* req) {
    auto* channel = reinterpret_cast<vesal::CypherChannel*>(handle);
    VESAL_DCHECK(channel != nullptr);
    std::vector<unsigned char*> src_vec;
    std::vector<unsigned int> src_len_vec;
    // copy data when src_num > VESAL_MAX_SGL_NUM(32) for DPU use case
    std::unique_ptr<unsigned char[]> tmp_buffer;
    if (src_num > VESAL_MAX_SGL_NUM) {
        unsigned int total_size = std::accumulate(src_len, src_len + src_num, 0);
        tmp_buffer.reset(new unsigned char[total_size]);
        unsigned int offset = 0;
        for (size_t i = 0; i < src_num; i++) {
            memcpy(tmp_buffer.get() + offset, src[i], src_len[i]);
            offset += src_len[i];
        }
        src_vec = {tmp_buffer.get()};
        src_len_vec = {total_size};
    } else {
        src_vec = {src, src + src_num};
        src_len_vec = {src_len, src_len + src_num};
    }
    vesal::CypherReqArgs internal_req{};
    internal_req.op = static_cast<vesal::CypherOp>(req->op);
    internal_req.ctx = req->ctx;
    internal_req.aes_xts_tweak = req->aes_xts_tweak;
    internal_req.session = req->session;
    auto status = channel->SubmitCypherSGLReq(src_vec, src_len_vec, dst, dst_len, &internal_req);
    return static_cast<VESAL_ERROR_CODE>(status);
}

ssize_t vesal_cypher_poll(VesalCypherChannelHandle handle,
                          vesal_cypher_result_t* result,
                          unsigned int result_num,
                          int timeout) {
    auto* channel = reinterpret_cast<vesal::CypherChannel*>(handle);
    VESAL_DCHECK(channel != nullptr);
    auto* cxx_results = reinterpret_cast<vesal::CypherResult*>(result);
    return channel->Poll(cxx_results, result_num, timeout);
}
