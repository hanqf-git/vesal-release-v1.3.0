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

#include "qat_session.h"

#include <cstdint>
#include <cstdio>
#include <memory>

#include "common/qat/qat_hardware_api_wrapper.h"
#include "vesal/codec.h"
#include "vesal/cypher.h"

extern "C" {
#include <cpa.h>
#include <cpa_cy_sym.h>
#include <cpa_types.h>
#include <dc/cpa_dc.h>
}

#include "codec/dc_format.h"
#include "codec/qat/qat_error_handling.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"

namespace vesal {
namespace qat {

QatSession::QatSession(QatUnit* unit)
    : qat_unit_(unit),
      cpa_dc_session_handle_(nullptr),
      cpa_sym_session_ctx_(nullptr),
      closed_(true) {}

QatSession::~QatSession() {
    if (cpa_dc_session_handle_ != nullptr) {
        MemoryPool::GetInstance()->Deallocate(cpa_dc_session_handle_);
        cpa_dc_session_handle_ = nullptr;
    }
    if (cpa_sym_session_ctx_ != nullptr) {
        MemoryPool::GetInstance()->Deallocate(cpa_sym_session_ctx_);
        cpa_sym_session_ctx_ = nullptr;
    }
}

Status QatSession::InitCodecSession(const QatSessionOption& sess_opts, CpaCallbackFn cb) {
    // Set qat session setup data
    CpaDcSessionSetupData sess_data;
    SetQatSessionData(sess_opts, &sess_data);

    uint32_t session_ctx_size = 0;
    uint32_t cpa_session_size = 0;
    CpaInstanceHandle* inst_handle = qat_unit_->GetInstanceHandle();
    // Get the size of the memory required to hold the session information.
    CpaStatus cpa_st =
        cpaDcGetSessionSize(*inst_handle, &sess_data, &cpa_session_size, &session_ctx_size);
    if (CPA_STATUS_SUCCESS != cpa_st) {
        return CpaStatusToVesalStatus(cpa_st, "Fail to cpaDcGetSessionSize");
    }
    cpa_dc_session_handle_ = MemoryPool::GetInstance()->Allocate(cpa_session_size);
    VESAL_CHECK(cpa_dc_session_handle_)
        << "Session allocation failed, cpa_session_size=" << cpa_session_size;
    VESAL_CHECK(session_ctx_size == 0);

    // Initialize qat session
    cpa_st = cpaDcInitSession(*inst_handle, cpa_dc_session_handle_, &sess_data, nullptr, cb);
    if (CPA_STATUS_SUCCESS != cpa_st) {
        MemoryPool::GetInstance()->Deallocate(cpa_dc_session_handle_);
        cpa_dc_session_handle_ = nullptr;
        return CpaStatusToVesalStatus(cpa_st, "Fail to cpaDcInitSession");
    }

    // Initialize crc32 control data
    const auto& chnnl_opt = sess_opts.codec_chann_opt;
    if (chnnl_opt.checksum_type == CodecChecksumType::kCrc32) {
        cpa_crc_control_data_.polynomial = kCrc32cPolynomial << 32;
        cpa_crc_control_data_.xorOut = kCrc32cXorOut;
        cpa_crc_control_data_.initialValue = kCrc32cInitialValue << 32;
        cpa_crc_control_data_.reflectIn = kCrc32cReflectIn;
        cpa_crc_control_data_.reflectOut = kCrc32cReflectOut;

        cpa_st =
            cpaDcSetCrcControlData(*inst_handle, cpa_dc_session_handle_, &cpa_crc_control_data_);

        if (CPA_STATUS_SUCCESS != cpa_st) {
            cpaDcRemoveSession(*inst_handle, cpa_dc_session_handle_);
            MemoryPool::GetInstance()->Deallocate(cpa_dc_session_handle_);
            cpa_dc_session_handle_ = nullptr;
            return CpaStatusToVesalStatus(cpa_st, "Fail to cpaDcSetCrcControlData");
        }
    }

    codec_algo_ = sess_opts.codec_chann_opt.comp_algorithm;
    comp_level_ = sess_opts.codec_chann_opt.comp_level;
    return OkStatus();
}

Status QatSession::InitCypherSession(const QatSessionOption& sess_opts, CpaCySymCbFunc cb) {
    CpaCySymSessionSetupData sess_data;
    SetQatSessionData(sess_opts, &sess_data);

    uint32_t session_ctx_size = 0;
    CpaInstanceHandle* inst_handle = qat_unit_->GetInstanceHandle();
    CpaStatus cpa_st = GetQatApiWrapper()->QAT_cpaCySymSessionCtxGetDynamicSize(
        *inst_handle, &sess_data, &session_ctx_size);
    if (CPA_STATUS_SUCCESS != cpa_st) {
        return CpaStatusToVesalStatus(cpa_st, "Fail to cpaCySymSessionCtxGetDynamicSize");
    }
    cpa_sym_session_ctx_ = MemoryPool::GetInstance()->Allocate(session_ctx_size);
    VESAL_CHECK(cpa_sym_session_ctx_)
        << "Session allocation failed, session_ctx_size=" << session_ctx_size;
    cpa_st = GetQatApiWrapper()->QAT_cpaCySymInitSession(
        *inst_handle, cb, &sess_data, cpa_sym_session_ctx_);
    if (CPA_STATUS_SUCCESS != cpa_st) {
        MemoryPool::GetInstance()->Deallocate(cpa_sym_session_ctx_);
        cpa_sym_session_ctx_ = nullptr;
        return CpaStatusToVesalStatus(cpa_st, "Fail to cpaCySymInitSession");
    }
    return OkStatus();
}

Status QatSession::Init(const QatSessionOption& sess_opts, CpaCallbackFn cb) {
    // Check channel options.
    if (!CheckCapabilities(sess_opts)) {
        return NotSupportedError("qat instance NOT support this operation");
    }

    Status r;
    r = InitCodecSession(sess_opts, cb);
    if (!r.ok()) {
        return r;
    }
    closed_ = false;
    return OkStatus();
};

Status QatSession::Init(const QatSessionOption& sess_opts, CpaCySymCbFunc cb) {
    // Check channel options.
    if (!CheckCapabilities(sess_opts)) {
        return NotSupportedError("qat instance NOT support this operation");
    }

    Status r;
    r = InitCypherSession(sess_opts, cb);
    if (!r.ok()) {
        return r;
    }
    closed_ = false;
    return OkStatus();
}

Status QatSession::Close() {
    if (closed_) {
        return OkStatus();
    }
    CpaInstanceHandle* inst_handle = qat_unit_->GetInstanceHandle();
    CpaStatus cpa_st = CPA_STATUS_SUCCESS;
    if (cpa_sym_session_ctx_) {
        cpa_st = GetQatApiWrapper()->QAT_cpaCySymRemoveSession(*inst_handle, cpa_sym_session_ctx_);
    }
    if (cpa_dc_session_handle_) {
        cpa_st = cpaDcRemoveSession(*inst_handle, cpa_dc_session_handle_);
    }
    // Removal will fail if outstanding calls still exist for the initialized session handle
    if (CPA_STATUS_SUCCESS != cpa_st) {
        return CpaStatusToVesalStatus(cpa_st, "Fail to cpaRemoveSession");
    }
    if (cpa_dc_session_handle_) {
        MemoryPool::GetInstance()->Deallocate(cpa_dc_session_handle_);
        cpa_dc_session_handle_ = nullptr;
    }
    if (cpa_sym_session_ctx_) {
        MemoryPool::GetInstance()->Deallocate(cpa_sym_session_ctx_);
        cpa_sym_session_ctx_ = nullptr;
    }
    closed_ = true;
    return OkStatus();
}

void QatSession::SetQatSessionData(const QatSessionOption& sess_opts,
                                   CpaCySymSessionSetupData* sess_data) {
    // Need reset the session data to avoid UB.
    memset(sess_data, 0, sizeof(CpaCySymSessionSetupData));
    // Currently we set all priority to normal to keep the FIFO order
    sess_data->sessionPriority = CPA_CY_PRIORITY_NORMAL;
    switch (sess_opts.SymOption.session_opt.algorithm) {
    case CypherAlgorithm::kAES_XTS: {
        sess_data->symOperation = CPA_CY_SYM_OP_CIPHER;
        // TODO(Pinnong Li): support more algorithms
        sess_data->cipherSetupData.cipherAlgorithm = CPA_CY_SYM_CIPHER_AES_XTS;
        sess_data->cipherSetupData.cipherKeyLenInBytes =
            sess_opts.SymOption.session_opt.aes_xts_spec.aes_xts_key1.length() << 1;
        // session is not bidirectional
        sess_data->cipherSetupData.cipherDirection = sess_opts.SymOption.op == CypherOp::kEncrypt
                                                         ? CPA_CY_SYM_CIPHER_DIRECTION_ENCRYPT
                                                         : CPA_CY_SYM_CIPHER_DIRECTION_DECRYPT;
        sym_cy_key_ = std::unique_ptr<unsigned char[]>(
            new unsigned char[sess_data->cipherSetupData.cipherKeyLenInBytes + 1]);
        std::string aes_xts_key = sess_opts.SymOption.session_opt.aes_xts_spec.aes_xts_key1 +
                                  sess_opts.SymOption.session_opt.aes_xts_spec.aes_xts_key2;
        memcpy(sym_cy_key_.get(), aes_xts_key.data(), aes_xts_key.size());
        sess_data->cipherSetupData.pCipherKey = sym_cy_key_.get();
        break;
    }
    case CypherAlgorithm::kSHA256: {
        sess_data->symOperation = CPA_CY_SYM_OP_HASH;
        sess_data->hashSetupData.hashAlgorithm = CPA_CY_SYM_HASH_SHA256;
        // SHA256 output length is 256 bits
        sess_data->hashSetupData.digestResultLenInBytes = SHA256_DST_LEN;
        sess_data->hashSetupData.hashMode = CPA_CY_SYM_HASH_MODE_PLAIN;
        break;
    }
    case CypherAlgorithm::kNum: {
        VESAL_LOG(ERROR) << "Failed to set qat session: kNum is invalid algorithm";
    }
    }
}

void QatSession::SetQatSessionData(const QatSessionOption& sess_opts,
                                   CpaDcSessionSetupData* sess_data) {
    // Need reset the session data to avoid UB.
    memset(sess_data, 0, sizeof(CpaDcSessionSetupData));
    const auto& chnnl_opt = sess_opts.codec_chann_opt;
    // reference: qat-programmers-guide 6.1.3
    sess_data->sessState = CPA_DC_STATELESS;
    sess_data->compLevel = static_cast<CpaDcCompLvl>(static_cast<size_t>(chnnl_opt.comp_level));

    // New QAT driver 1.1.40.0018 does not support ABS feature, hence disabled
    sess_data->autoSelectBestHuffmanTree = CPA_DC_ASB_DISABLED;
    sess_data->sessDirection = CPA_DC_DIR_COMBINED;

    switch (chnnl_opt.comp_algorithm) {
    case CodecAlgorithm::kLz4: {
        // In QAT, some part of lz4 frame attributes can be affected by session data.
        // Need to set those parts of QAT session data aligned with vesal lz4 frame rule.
        sess_data->compType = CPA_DC_LZ4;
        // Required by Qat driver.
        sess_data->checksum = CPA_DC_XXHASH32;
        sess_data->huffType = CPA_DC_HT_STATIC;
        VESAL_CHECK(kLz4FrameHeaderMaxBlockSize == 0x04) << "Currently only supports 64KB";
        sess_data->lz4BlockMaxSize = CPA_DC_LZ4_MAX_BLOCK_SIZE_64K;
        sess_data->lz4BlockIndependence = kLz4FrameHeaderBlockIndep != 0U ? CPA_TRUE : CPA_FALSE;
        sess_data->lz4BlockChecksum = kLz4FrameHeaderBlockChecksum != 0U ? CPA_TRUE : CPA_FALSE;
        sess_data->accumulateXXHash = CPA_FALSE;
        break;
    }
    case CodecAlgorithm::kZstd: {
        sess_data->compType = CPA_DC_LZ4S;
        // Required by Qat driver.
        sess_data->checksum = CPA_DC_XXHASH32;
        sess_data->huffType = CPA_DC_HT_STATIC;
        // TODO(shijunjie.1) determine use CPA_DC_MIN_3_BYTE_MATCH or CPA_DC_MIN_4_BYTE_MATCH
        // reference: QAT-ZSTD-Plugin hardcoded CPA_DC_MIN_3_BYTE_MATCH
        sess_data->minMatch = CPA_DC_MIN_3_BYTE_MATCH;
        // According to test, when lz4s decompressed result is longer than original
        // data, enabling this will let it produce the original data itself,
        // which cannot be used for post-processing
        sess_data->autoSelectBestHuffmanTree = CPA_DC_ASB_DISABLED;
        break;
    }
    case CodecAlgorithm::kDeflate: {
        sess_data->compType = CPA_DC_DEFLATE;
        sess_data->checksum = CPA_DC_CRC32;
        // Dynamic HT shows best compression ratio and very little impact on performance
        sess_data->huffType =
            sess_data->compLevel >= CPA_DC_L9 ? CPA_DC_HT_FULL_DYNAMIC : CPA_DC_HT_STATIC;
        // Though it's not related to deflate, CNV error may occur if not set to false
        sess_data->accumulateXXHash = CPA_FALSE;
        break;
    }
    case CodecAlgorithm::kZlib: {
        sess_data->compType = CPA_DC_DEFLATE;
        // Needs adler32 for zlib footer generation
        sess_data->checksum = CPA_DC_ADLER32;
        // Dynamic HT shows best compression ratio and very little impact on performance
        sess_data->huffType =
            sess_data->compLevel >= CPA_DC_L9 ? CPA_DC_HT_FULL_DYNAMIC : CPA_DC_HT_STATIC;
        // Follow kDefalte
        sess_data->accumulateXXHash = CPA_FALSE;
        break;
    }
    default:
        VESAL_LOG(CRITICAL) << "Not supported algorithm type: "
                            << static_cast<int>(chnnl_opt.comp_algorithm);
    }
}

bool QatSession::CheckCapabilities(const QatSessionOption& opt) {
    QatUnitAttr attr = qat_unit_->GetQatUnitAttr();

    if (!attr.is_polled) {
        VESAL_LOG(ERROR)
            << "Vesal only supports Polling mode of QAT. Please check the QAT driver config.";
        return false;
    }

    if (opt.type == QatSessionType::kCypher) {
        if (!attr.is_aes_xts_supp) {
            VESAL_LOG(ERROR) << "Not support sym, attr.is_aes_xts_supp=" << attr.is_aes_xts_supp;
            return false;
        }
        switch (opt.SymOption.session_opt.algorithm) {
        case CypherAlgorithm::kNum: {
            VESAL_LOG(ERROR) << "Not supported algorithm, the input type is "
                             << static_cast<int>(opt.SymOption.session_opt.algorithm);
            return false;
        }
        case CypherAlgorithm::kAES_XTS: {
            int len1 = opt.SymOption.session_opt.aes_xts_spec.aes_xts_key1.length();
            int len2 = opt.SymOption.session_opt.aes_xts_spec.aes_xts_key2.length();
            if (len1 != len2) {
                VESAL_LOG(ERROR) << "The length of AES-XTS key1 and key2 must be equal, len1 = "
                                 << len1 << ", len2 = " << len2;
                return false;
            }
            // As AES-XTS algorithm specified, length can only be 16 or 32 bytes
            if (len1 != 16 && len1 != 32) {
                VESAL_LOG(ERROR)
                    << "The length of AES-XTS key must be either 16 or 32 bytes, len = " << len1;
                return false;
            }
            break;
        }
        case CypherAlgorithm::kSHA256:
            break;
        }
        return true;
    }

    const auto& codec_chnnl_opt = opt.codec_chann_opt;
    if (codec_chnnl_opt.comp_algorithm == CodecAlgorithm::kLz4) {
        if (!attr.is_lz4_compression || !attr.is_lz4_decompression) {
            VESAL_LOG(ERROR) << "Not support compress or decompress for LZ4,"
                                " attr.is_lz4_compression="
                             << attr.is_lz4_compression
                             << ", attr.is_lz4_decompression=" << attr.is_lz4_decompression;
            return false;
        }
    } else if (codec_chnnl_opt.comp_algorithm == CodecAlgorithm::kZstd) {
        // QAT hw doesn't support zstd decompression
        if (!attr.is_zstd_compression) {
            VESAL_LOG(ERROR) << "Not support compress for ZSTD,"
                                " attr.is_zstd_compression="
                             << attr.is_zstd_compression
                             << ", attr.is_zstd_decompression=" << attr.is_zstd_decompression;
            return false;
        }
    } else if (codec_chnnl_opt.comp_algorithm == CodecAlgorithm::kDeflate) {
        if (!attr.is_deflate_compression || !attr.is_deflate_decompression) {
            VESAL_LOG(ERROR) << "Not support compress or decompress for Deflate,"
                                " attr.is_deflate_compression="
                             << attr.is_deflate_compression
                             << ", attr.is_deflate_decompression=" << attr.is_deflate_decompression;
            return false;
        }
    }

    // Requires checksum ability forcefully. Note this is different from CpaDcOpData's CpaCrcData.
    // We use CRC32 by from CpaDcOpData's integrityCrc64b. We set here forcefully to because we need
    // adler32 for deflate.
    if (!attr.is_crc_supp) {
        VESAL_LOG(ERROR) << "QAT not support checksum, attr.is_crc_supp=" << attr.is_crc_supp;
        return false;
    }
    // TODO(sjj): Check QAT Driver version because only high version QAT support compressed
    // checksum.
    if (codec_chnnl_opt.checksum_type != CodecChecksumType::kNone &&
        codec_chnnl_opt.checksum_type != CodecChecksumType::kCrc32) {
        VESAL_LOG(ERROR) << "QAT not support checksum type, checksum_type="
                         << static_cast<int>(codec_chnnl_opt.checksum_type);
        return false;
    }
    return true;
}

}  // namespace qat
}  // namespace vesal
