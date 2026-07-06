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

#include "cypher/sw_cypher_channel.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "common/defer.h"
#ifdef VESAL_ENABLE_SSL
#include "openssl/evp.h"
#endif
#include "vesal/cypher.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"

namespace vesal {

SwCypherChannel::SwCypherChannel(const CypherChannelOption& channel_opts)
    : channel_opts_(channel_opts) {}

SwCypherChannel::~SwCypherChannel() {}

Status SwCypherChannel::Init() {
    auto& opt = channel_opts_.session_option;
    switch (opt.algorithm) {
#ifdef VESAL_ENABLE_SSL
    case CypherAlgorithm::kAES_XTS: {
        std::string key = opt.aes_xts_spec.aes_xts_key1 + opt.aes_xts_spec.aes_xts_key2;
        int len = key.size();
        aes_xts_key_.reset(new unsigned char[len]);
        memcpy(aes_xts_key_.get(), key.data(), len);
        break;
    }
    case CypherAlgorithm::kSHA256:
        break;
#endif
    default:
        return NotSupportedError("Unsupported algorithm");
    }
    return OkStatus();
}

Status SwCypherChannel::Close() {
    return OkStatus();
}

void* SwCypherChannel::AddSession(const CypherSessionOption& option) {
    return nullptr;
}

void SwCypherChannel::RemoveSession(void* session) {}

StatusCode SwCypherChannel::SubmitCypherReq(unsigned char* src,
                                            unsigned int src_len,
                                            unsigned char* dst,
                                            unsigned int dst_len,
                                            CypherReqArgs* req) {
#ifdef VESAL_ENABLE_SSL
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        VESAL_LOG(ERROR) << "Failed to create EVP_CIPHER_CTX";
        return StatusCode::kUnknown;
    }
    auto clean_up_guard = defer([ctx]() { EVP_CIPHER_CTX_free(ctx); });
    switch (channel_opts_.session_option.algorithm) {
    case CypherAlgorithm::kAES_XTS: {
        if (req->op == CypherOp::kEncrypt) {
            if (EVP_EncryptInit_ex(
                    ctx, EVP_aes_256_xts(), nullptr, aes_xts_key_.get(), req->aes_xts_tweak) != 1) {
                VESAL_LOG(ERROR) << "Failed to initialize EVP_CIPHER_CTX";
                return StatusCode::kUnknown;
            }
            int len = 0;
            if (EVP_EncryptUpdate(ctx, dst, &len, src, src_len) != 1) {
                VESAL_LOG(ERROR) << "Failed to encrypt data";
                return StatusCode::kUnknown;
            }
            if (EVP_EncryptFinal_ex(ctx, dst + len, &len) != 1) {
                VESAL_LOG(ERROR) << "Failed to finalize encryption";
                return StatusCode::kUnknown;
            }
            return StatusCode::kOk;
        } else {
            if (EVP_DecryptInit_ex(
                    ctx, EVP_aes_256_xts(), nullptr, aes_xts_key_.get(), req->aes_xts_tweak) != 1) {
                VESAL_LOG(ERROR) << "Failed to initialize EVP_CIPHER_CTX";
                return StatusCode::kUnknown;
            }
            int len = 0;
            if (EVP_DecryptUpdate(ctx, dst, &len, src, src_len) != 1) {
                VESAL_LOG(ERROR) << "Failed to Decrypt data";
                return StatusCode::kUnknown;
            }
            if (EVP_DecryptFinal_ex(ctx, dst + len, &len) != 1) {
                VESAL_LOG(ERROR) << "Failed to finalize Decryption";
                return StatusCode::kUnknown;
            }
            return StatusCode::kOk;
        }
        break;
    }
    case CypherAlgorithm::kSHA256: {
        const EVP_MD* sha256 = EVP_sha256();
        const int digest_len = EVP_MD_size(sha256);
        if (digest_len <= 0 || dst_len < static_cast<unsigned int>(digest_len)) {
            VESAL_LOG(ERROR) << "Invalid SHA256 destination buffer length";
            return StatusCode::kInvalidArgument;
        }
        if (EVP_Digest(src, src_len, dst, nullptr, sha256, nullptr) != 1) {
            VESAL_LOG(ERROR) << "Failed to calculate SHA256 digest";
            return StatusCode::kUnknown;
        }
        return StatusCode::kOk;
    }
    default:
        return StatusCode::kNotSupported;
    }
#endif
    // Avoid compiler complaint.
    return StatusCode::kNotSupported;
}

StatusCode SwCypherChannel::SubmitCypherSGLReq(const std::vector<unsigned char*>& src,
                                               const std::vector<unsigned int>& src_len,
                                               unsigned char* dst,
                                               unsigned int dst_len,
                                               CypherReqArgs* req) {
    unsigned int len_sum = 0;
    for (unsigned int len : src_len) {
        len_sum += len;
    }
    auto concatenated_src = std::make_unique<unsigned char[]>(len_sum);
    size_t offset = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        memcpy(&concatenated_src[offset], src[i], src_len[i]);
        offset += src_len[i];
    }
    return SubmitCypherReq(concatenated_src.get(), len_sum, dst, dst_len, req);
}

ssize_t SwCypherChannel::Poll(CypherResult results[], unsigned int max_num, int timeout) {
    return 0;
}

}  // namespace vesal