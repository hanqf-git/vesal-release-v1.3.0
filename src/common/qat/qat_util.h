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

#include <cstdint>

namespace vesal {
namespace qat {

extern int g_qat_timeout_ms;

enum class QatServiceType : uint8_t { kDc = 1, kSym, kNum };

bool QatUserStart();

bool QatUserStop();

int GetQatMaxConcurrency(QatServiceType type = QatServiceType::kDc);

}  // namespace qat
}  // namespace vesal
