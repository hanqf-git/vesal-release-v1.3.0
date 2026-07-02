
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
extern "C" {
#include "dsa_uio_config.h"
}
#include <cstring>
#include "data_flow/data_flow_request.h"
#include "data_flow/dsa/dsa_dispatcher.h"
#include "vesal/data_flow.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"

namespace vesal {
namespace data_flow {

// CRC computation has many variants, including inversion and reflection of CRC seed and CRC result.
// While DSA provides several variant options, none of them align with the company's business use
// case. The operation involves modifying the CRC seed and CRC result, then integrating it with DSA
// to achieve the desired outcome.
#define VESAL_CRC_REVISE(x) (~(x))

// According to data-streaming-accelerator-user-guide-003,
// the size of dsa_hw_desc is 64 bytes, and
// the size of dsa_completion_record is 32 bytes.
// The related memory alignment requirements and designs are based on them.
static const uint64_t kDsaHwDescSize = 64;
static const uint64_t kDsaCompletionRecordSize = 32;

struct WorkQueueInfo;

struct DsaCtx {
    // get a memory chunk from memory pool to store
    // dsa_hw_descs
    // ** virtual address **
    void* desc_memory_chunk;
    // the beginning address of dsa_hw_descs
    // each dsa_hw_desc takes 64 bytes
    // dsa_hw_desc's address must be 64-byte aligned dma memory.
    // all descriptors are in the consecutive memory in desc_memory_chunk.
    // ** virtual address **
    uintptr_t dsa_hw_descs;
    // the physical address of dsa_hw_descs;
    // ** physical address **
    uintptr_t dsa_hw_descs_phys;

    // get a memory chunk from memory pool to store
    // completion_records
    // ** virtual address **
    void* completion_record_memory_chunk;
    // the beginning address of dsa_completion_records
    // each desc has a completion record(32bytes).
    // completion record's address must be 32-byte aligned dma memory.
    // all completion records are in the consecutive memory
    // in completion_record_memory_chunk.
    // ** virtual address **
    uintptr_t dsa_completion_records;
    // the physical address of dsa_completion_records;
    // ** physical address **
    uintptr_t dsa_completion_records_phys;

    size_t desc_num;
    // completion records that contain crc results
    // crc_results[0..req_num-1] = nullptr (crc not required) / a valid addr (crc required)
    // ** virtual address **
    uintptr_t crc_results[kMaxSubmittedBatchSize];
    WorkQueueInfo* work_queue;
};

// DSA supports 5 kinds of desc:
// batch: contains pointer pointing to list of descs
// memcpy: copy from src to dst
// crc: calculate crc for src
// memcpy with crc: copy from src to dst and calculate crc for src
// no-op: fence
//
// use these desc to complete DataFlowRequest
class DsaCtxFactory {
public:
    StatusCode Init();

    StatusCode BuildCtx(DataFlowRequest* req);

    void ClearCtx(DataFlowRequest* req);

    StatusCode PreallocateCtx(DataFlowRequest* req);

private:
    StatusCode BuildMoveCtx(DataFlowRequest* req);
    StatusCode BuildCrcCtx(DataFlowRequest* req);
    uintptr_t GetNextAlignedAddress(uintptr_t addr, size_t alignment);
    void ResetCtx(DsaCtx* dsa_ctx) {
        // according to
        // "intel-data-streaming-accelerator-spec"
        // "5.5 Descriptor Reserved Field Checking"
        // each descriptor has different reserved field
        // here we just set everything to 0
        memset(reinterpret_cast<void*>(dsa_ctx->dsa_hw_descs),
               0,
               dsa_hw_desc_size_ * dsa_ctx->desc_num);
        // completion_record->status will affect polling results
        // we only care about the first completion_record
        memset(reinterpret_cast<void*>(dsa_ctx->dsa_completion_records),
               0,
               dsa_completion_record_size_);
    }

    void FillCopyMaybeWithCrc(uintptr_t desc,
                              uintptr_t src,
                              uint64_t len,
                              uintptr_t dst,
                              uint32_t seed,
                              bool enable_crc,
                              uintptr_t completion_record_addr,
                              uintptr_t completion_record,
                              uintptr_t* crc_result) {
        if (enable_crc) {
            FillCopyWithCrc(desc, src, len, dst, seed, completion_record_addr);
            *crc_result = completion_record;
        } else {
            FillCopy(desc, src, len, dst, completion_record_addr);
            *crc_result = 0;
        }
    }
    MemoryPool* mp_;
    size_t dsa_hw_desc_size_;
    size_t dsa_completion_record_size_;
};

}  // namespace data_flow
}  // namespace vesal