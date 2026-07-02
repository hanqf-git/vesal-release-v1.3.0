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

#pragma once

#include <memory>
#include <mutex>
#include <utility>

#include "common/qat/qat_unit_manager.h"
#include "vesal/cypher.h"

namespace vesal {

// Most of the crypto algorithms require 16 bytes IV
static const int kIvSize = 16;

class Cypher {
public:
    Cypher(bool init_qat)
        : unit_manager_(std::make_unique<qat::QatUnitManager>()),
          init_qat_(init_qat) {}

    bool Start();
    bool Stop();

    std::pair<Status, std::unique_ptr<CypherChannel>> CreateCypherChannel(
        const CypherChannelOption& opts);

    qat::QatUnitManager* GetUnitManager() {
        return unit_manager_.get();
    }

private:
    std::unique_ptr<qat::QatUnitManager> unit_manager_;
    bool is_running_ = false;
    std::mutex build_mutex_;
    int max_in_qat_size_;
    bool init_qat_;
};

extern std::unique_ptr<Cypher> g_cypher;

inline std::ostream& operator<<(std::ostream& os, const CypherOp& op) {
    switch (op) {
    case CypherOp::kEncrypt:
        os << "kEncrypt";
        break;
    case CypherOp::kDecrypt:
        os << "kDecrypt";
        break;
    case CypherOp::kHash:
        os << "kHash";
        break;
    }
    return os;
}

}  // namespace vesal
