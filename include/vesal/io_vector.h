/*
 * Copyright (c) 2026 ByteDance Inc.
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

struct IOBlock {
    uint32_t size{0};
    unsigned char* data{nullptr};
    void* ctx{nullptr};  // context pointer to be passed to the deleter, can be same as data
    void (*deleter)(void* ctx){
        nullptr};  // Optional deleter function to manage the memory lifecycle of IO blocks.
                   // The memory ownership rules:
                   // - If the request completes successfully: vesal returns memory ownership to
                   //   the caller, the caller is responsible for releasing or reusing the memory.
                   // - If the request fails deterministically (no in-flight requests remain on
                   //   hardware): vesal returns memory ownership to the caller, the caller is
                   //   responsible for releasing or reusing the memory.
                   // - If the request times out: vesal takes over memory ownership. The caller
                   //   MUST NOT release or reuse this memory. If a deleter is provided, vesal will
                   //   call deleter(block.ctx) for each block after the hardware releases the
                   //   memory.
                   // Note: hardware may take seconds to release the memory, though this is
                   // extremely uncommon.
};

struct IOVector {
    uint32_t num_blocks{0};
    IOBlock* blocks;
};

}  // namespace vesal