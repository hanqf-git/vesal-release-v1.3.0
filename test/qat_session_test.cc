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

#include "common/qat/qat_session.h"

#include <gtest/gtest.h>

#include "common/qat/qat_unit.h"
#include "cpa_dc.h"
#include "vesal/codec.h"
#include "vesal/cypher.h"
#include "vesal/status.h"

namespace vesal {
namespace qat {

class MockQatUnit : public QatUnit {
public:
    MockQatUnit() : QatUnit(CpaInstanceHandle()) {}

    QatUnitAttr* GetMutableAttr() {
        return &qat_unit_attr_;
    }
};

TEST(QatSessionTest, CheckCapabilitiesTest) {
    MockQatUnit unit;
    QatSession session(&unit);
    QatUnitAttr* attr = unit.GetMutableAttr();
    CodecChannelOption chan_opt;
    QatSessionOption opt(chan_opt);

    // lz4 not supported
    attr->is_lz4_compression = false;
    attr->is_lz4_decompression = false;
    attr->is_crc_supp = true;
    attr->is_polled = true;
    opt.codec_chann_opt.comp_algorithm = CodecAlgorithm::kLz4;
    ASSERT_FALSE(session.CheckCapabilities(opt));

    // zstd not supported
    attr->is_zstd_compression = false;
    attr->is_zstd_decompression = false;
    attr->is_crc_supp = true;
    opt.codec_chann_opt.comp_algorithm = CodecAlgorithm::kZstd;
    ASSERT_FALSE(session.CheckCapabilities(opt));

    // is_polled mismatch
    attr->is_zstd_compression = true;
    attr->is_zstd_decompression = true;
    attr->is_polled = false;
    attr->is_crc_supp = true;
    ASSERT_FALSE(session.CheckCapabilities(opt));

    // valid option
    attr->is_lz4_compression = true;
    attr->is_lz4_decompression = true;
    attr->is_crc_supp = true;
    attr->is_polled = true;
    attr->is_crc_supp = true;
    opt.codec_chann_opt.checksum_type = CodecChecksumType::kCrc32;
    opt.codec_chann_opt.comp_algorithm = CodecAlgorithm::kLz4;
    ASSERT_TRUE(session.CheckCapabilities(opt));

    // not support crc, fail directly
    attr->is_crc_supp = false;
    ASSERT_FALSE(session.CheckCapabilities(opt));
    opt.codec_chann_opt.checksum_type = CodecChecksumType::kNone;
    ASSERT_FALSE(session.CheckCapabilities(opt));

    // sym cypher not support
    attr->is_aes_xts_supp = false;
    opt.type = QatSessionType::kCypher;
    ASSERT_FALSE(session.CheckCapabilities(opt));
}

TEST(QatSessionTest, SetQatSessionDataTest) {
    MockQatUnit unit;
    QatSession session(&unit);
    QatSessionOption session_opt(CodecChannelOption{});
    CpaDcSessionSetupData sess_data;

    // Expect members of sess_data are correctly set

    session_opt.codec_chann_opt.comp_algorithm = CodecAlgorithm::kZstd;
    session.SetQatSessionData(session_opt, &sess_data);
    EXPECT_EQ(sess_data.compType, CPA_DC_LZ4S);
    EXPECT_EQ(sess_data.sessDirection, CPA_DC_DIR_COMBINED);

    session_opt.codec_chann_opt.checksum_type = CodecChecksumType::kCrc32;
    session.SetQatSessionData(session_opt, &sess_data);
    // internally we always use CPA_DC_XXHASH32
    EXPECT_EQ(sess_data.checksum, CPA_DC_XXHASH32);
    EXPECT_EQ(sess_data.sessDirection, CPA_DC_DIR_COMBINED);

    // correctly set sym session data
    CpaCySymSessionSetupData sym_sess_data;
    session_opt.type = QatSessionType::kCypher;
    session_opt.SymOption.op = CypherOp::kEncrypt;
    session_opt.SymOption.session_opt.algorithm = CypherAlgorithm::kAES_XTS;
    session_opt.SymOption.session_opt.aes_xts_spec.aes_xts_key1 = std::string(16, 'A');
    session_opt.SymOption.session_opt.aes_xts_spec.aes_xts_key2 = std::string(16, 'A');
    session.SetQatSessionData(session_opt, &sym_sess_data);
    EXPECT_EQ(sym_sess_data.symOperation, CPA_CY_SYM_OP_CIPHER);
    EXPECT_EQ(sym_sess_data.cipherSetupData.cipherAlgorithm, CPA_CY_SYM_CIPHER_AES_XTS);
    EXPECT_EQ(sym_sess_data.cipherSetupData.cipherKeyLenInBytes, 32);
    EXPECT_EQ(sym_sess_data.cipherSetupData.cipherDirection, CPA_CY_SYM_CIPHER_DIRECTION_ENCRYPT);

    // set up unrelated mock data
    QatUnitAttr* attr = unit.GetMutableAttr();
    attr->is_crc_supp = true;
    attr->is_polled = true;
    // Expect invalid arguments error when given wrong key length
    session_opt.SymOption.session_opt.aes_xts_spec.aes_xts_key1 = "";
    EXPECT_FALSE(session.CheckCapabilities(session_opt));
}

TEST(QatSessionTest, HTTypeTest) {
    MockQatUnit unit;
    QatSession session(&unit);
    QatSessionOption session_opt(CodecChannelOption{});
    CpaDcSessionSetupData sess_data;

    // Expect members of sess_data are correctly set
    session_opt.codec_chann_opt.comp_algorithm = CodecAlgorithm::kDeflate;
    session_opt.codec_chann_opt.comp_level = CodecCompLevel::kLevel8;
    session.SetQatSessionData(session_opt, &sess_data);
    EXPECT_EQ(sess_data.huffType, CPA_DC_HT_STATIC);
    session_opt.codec_chann_opt.comp_level = CodecCompLevel::kLevel9;
    session.SetQatSessionData(session_opt, &sess_data);
    EXPECT_EQ(sess_data.huffType, CPA_DC_HT_FULL_DYNAMIC);

    session_opt.codec_chann_opt.comp_algorithm = CodecAlgorithm::kZlib;
    session_opt.codec_chann_opt.comp_level = CodecCompLevel::kLevel8;
    session.SetQatSessionData(session_opt, &sess_data);
    EXPECT_EQ(sess_data.huffType, CPA_DC_HT_STATIC);
    session_opt.codec_chann_opt.comp_level = CodecCompLevel::kLevel9;
    session.SetQatSessionData(session_opt, &sess_data);
    EXPECT_EQ(sess_data.huffType, CPA_DC_HT_FULL_DYNAMIC);
}

};  // namespace qat
};  // namespace vesal
