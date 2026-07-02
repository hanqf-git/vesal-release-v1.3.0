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

#include "kae_dummy_driver.h"

#include "libkaezip_dummy.h"

int g_driver_load_kae_ok = 0;

static __attribute__((constructor)) void driver_load_kae_init(void) {
    g_driver_load_kae_ok = Loadlibkaezip();
    if (!g_driver_load_kae_ok) {
        DLOG("Load libkaezip failed");
    }
}