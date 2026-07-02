
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

#include "data_flow/dsa/dsa_ctx_factory.h"
#include <cstring>
#include "common/memory_pool_helper.h"
#include "common/object_pool.h"
#include "data_flow/data_flow_request.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/status.h"

namespace vesal {
namespace data_flow {

StatusCode DsaCtxFactory::Init() {
    mp_ = MemoryPool::GetInstance();
    if (VESAL_UNLIKELY(!mp_->Init())) {
        VESAL_LOG(ERROR) << "memory pool init failed";
        return StatusCode::kUnknown;
    }
    // the memory alignment rely on sizeof(dsa_hw_desc) and sizeof(dsa_completion_record)
    // if they've been changed, the memory alignment need to be revisited
    dsa_hw_desc_size_ = GetDsaHwDescSize();
    dsa_completion_record_size_ = GetDsaCompletionRecordSize();
    VESAL_CHECK(dsa_hw_desc_size_ == kDsaHwDescSize);
    VESAL_CHECK(dsa_completion_record_size_ == kDsaCompletionRecordSize);
    return StatusCode::kOk;
}

StatusCode DsaCtxFactory::PreallocateCtx(DataFlowRequest* req) {
    auto dsa_ctx = new DsaCtx;

    dsa_ctx->desc_memory_chunk = mp_->Allocate(dsa_hw_desc_size_ * (kMaxInternalBatchSize + 1));
    dsa_ctx->completion_record_memory_chunk =
        mp_->Allocate(dsa_completion_record_size_ * (kMaxInternalBatchSize + 1));
    if (VESAL_UNLIKELY(!dsa_ctx->desc_memory_chunk || !dsa_ctx->completion_record_memory_chunk)) {
        VESAL_LOG(ERROR) << "Failed to allocate memory chunk for dsa_ctx";
        // Deallocate() can handle nullptr input
        mp_->Deallocate(dsa_ctx->desc_memory_chunk);
        mp_->Deallocate(dsa_ctx->completion_record_memory_chunk);
        delete dsa_ctx;
        return StatusCode::kResourceBusy;
    }

    uintptr_t phys_addr1 = LookUpAddr(dsa_ctx->desc_memory_chunk);
    uintptr_t phys_addr2 = LookUpAddr(dsa_ctx->completion_record_memory_chunk);
    if (VESAL_UNLIKELY(!phys_addr1 || !phys_addr2)) {
        VESAL_LOG(ERROR) << "Failed to get physical address";
        mp_->Deallocate(dsa_ctx->desc_memory_chunk);
        mp_->Deallocate(dsa_ctx->completion_record_memory_chunk);
        delete dsa_ctx;
        return StatusCode::kUnknown;
    }

    dsa_ctx->dsa_hw_descs = GetNextAlignedAddress(
        reinterpret_cast<uintptr_t>(dsa_ctx->desc_memory_chunk), dsa_hw_desc_size_);
    dsa_ctx->dsa_hw_descs_phys = GetNextAlignedAddress(phys_addr1, dsa_hw_desc_size_);

    dsa_ctx->dsa_completion_records =
        GetNextAlignedAddress(reinterpret_cast<uintptr_t>(dsa_ctx->completion_record_memory_chunk),
                              dsa_completion_record_size_);
    dsa_ctx->dsa_completion_records_phys =
        GetNextAlignedAddress(phys_addr2, dsa_completion_record_size_);

    req->engine_ctx = dsa_ctx;
    return StatusCode::kOk;
}

StatusCode DsaCtxFactory::BuildCtx(DataFlowRequest* req) {
    StatusCode result = StatusCode::kOk;
    switch (req->op->type) {
    case DataFlowOperationType::kMove:
        result = BuildMoveCtx(req);
        break;
    case DataFlowOperationType::kCrc:
        result = BuildCrcCtx(req);
        break;
    default:
        VESAL_LOG(ERROR) << "unsupported operation";
        result = StatusCode::kNotSupported;
    }
    return result;
}

void DsaCtxFactory::ClearCtx(DataFlowRequest* req) {
    auto dsa_ctx = static_cast<DsaCtx*>(req->engine_ctx);
    mp_->Deallocate(dsa_ctx->desc_memory_chunk);
    mp_->Deallocate(dsa_ctx->completion_record_memory_chunk);
    delete dsa_ctx;
}

inline bool IsNoOverlap(const void* src, const void* dst, size_t size) {
    auto src_start = reinterpret_cast<uintptr_t>(src);
    auto src_end = src_start + size;
    auto dst_start = reinterpret_cast<uintptr_t>(dst);
    auto dst_end = dst_start + size;

    return (src_end <= dst_start) || (dst_end <= src_start);
}

inline bool IsCopyWithCrcOk(DataFlowMoveOperation* op) {
    return op->src.size() == 1 && IsNoOverlap(op->src.front(), op->dst, op->src_len.front());
}

StatusCode DsaCtxFactory::BuildMoveCtx(DataFlowRequest* req) {
    thread_local uint64_t op_len_sum[kMaxSubmittedBatchSize];
    DataFlowMoveOperation* op = static_cast<DataFlowMoveOperation*>(req->op);
    auto dsa_ctx = static_cast<DsaCtx*>(req->engine_ctx);
    // note that dsa (copy op) support overlapping memory move,
    // but (copy with crc op) does not.
    // Therefore, what appears to be a simple (copy with crc),
    // might in fact require (batch)+(move)+(fence)+(crc).
    //
    // If the following conditions are met, there is only one desc
    // desc list = 1*(copy|copy with crc)
    if (req->num == 1 && op->src.size() == 1 &&
        (!op->enable_crc || IsNoOverlap(op->src.front(), op->dst, op->src_len.front()))) {
        dsa_ctx->desc_num = 1;
        ResetCtx(dsa_ctx);
        uintptr_t src_phys_addr = LookUpAddrIfNotAcrossBound(op->src.front(), op->src_len.front());
        uintptr_t dst_phys_addr = LookUpAddrIfNotAcrossBound(op->dst, op->src_len.front());
        if (VESAL_UNLIKELY(!src_phys_addr || !dst_phys_addr)) {
            VESAL_LOG(ERROR) << "Failed to get physical address, the input may not be DMA-able "
                                "memory or span pages";
            return StatusCode::kBadData;
        }
        FillCopyMaybeWithCrc(dsa_ctx->dsa_hw_descs,
                             src_phys_addr,
                             op->src_len.front(),
                             dst_phys_addr,
                             VESAL_CRC_REVISE(op->seed),
                             op->enable_crc,
                             dsa_ctx->dsa_completion_records_phys,
                             dsa_ctx->dsa_completion_records,
                             dsa_ctx->crc_results);
    }
    // desc list explanation:
    // "(desc1)" means a descriptor
    // "desc1|desc2" means desc1 or desc2
    // "[...]" means something optional
    // desc list = 1*(batch) + x*(copy|copy with crc) + [1*(fence) + y*(crc)]
    else {
        size_t copy_num = 0;
        size_t crc_num = 0;
        for (size_t i = 0; i < req->num; ++i) {
            DataFlowMoveOperation* current_op = op + i;
            // if there's only one src and no overlapping memory, we can apply (copy with crc)
            // operation directly, instead of multiple (copy) op and one (crc) op
            if (current_op->enable_crc && !IsCopyWithCrcOk(current_op)) {
                ++crc_num;
            }
            copy_num += current_op->src.size();
        }
        dsa_ctx->desc_num = 1 + copy_num + crc_num;
        // if we need to do (crc) op later, it has to wait till
        // (copy) op is completed. Therefore we need a fence here.
        if (crc_num)
            ++dsa_ctx->desc_num;
        ResetCtx(dsa_ctx);
        FillBatch(dsa_ctx->dsa_hw_descs,
                  dsa_ctx->desc_num - 1,  // the fisrt batch desc doesn't count into desc_list_size
                  dsa_ctx->dsa_hw_descs_phys +
                      dsa_hw_desc_size_,  // the batch desc list start from 2nd desc
                  dsa_ctx->dsa_completion_records_phys);
        size_t desc_offset = 1;
        for (size_t i = 0; i < req->num; ++i) {
            DataFlowMoveOperation* current_op = op + i;
            // we don't know dst len now, check it later
            uintptr_t dst_phys_addr = LookUpAddr(current_op->dst);
            if (VESAL_UNLIKELY(!dst_phys_addr)) {
                VESAL_LOG(ERROR) << "Failed to get physical address, the input may not be DMA-able "
                                    "memory or span pages";
                return StatusCode::kBadData;
            }
            op_len_sum[i] = 0;
            size_t src_size = current_op->src.size();
            for (size_t j = 0; j < src_size; ++j) {
                uintptr_t src_phys_addr =
                    LookUpAddrIfNotAcrossBound(current_op->src[j], current_op->src_len[j]);
                if (VESAL_UNLIKELY(!src_phys_addr)) {
                    VESAL_LOG(ERROR) << "Failed to get physical address, the input may not be "
                                        "DMA-able memory or span pages";
                    return StatusCode::kBadData;
                }
                FillCopyMaybeWithCrc(
                    dsa_ctx->dsa_hw_descs + desc_offset * dsa_hw_desc_size_,
                    src_phys_addr,
                    current_op->src_len[j],
                    dst_phys_addr + op_len_sum[i],
                    VESAL_CRC_REVISE(current_op->seed),
                    current_op->enable_crc && IsCopyWithCrcOk(current_op),
                    dsa_ctx->dsa_completion_records_phys +
                        desc_offset * dsa_completion_record_size_,
                    dsa_ctx->dsa_completion_records + desc_offset * dsa_completion_record_size_,
                    &dsa_ctx->crc_results[i]);
                ++desc_offset;
                op_len_sum[i] += current_op->src_len[j];
            }
            // validate dst addr when we know the length of it
            if (VESAL_UNLIKELY(!LookUpAddrIfNotAcrossBound(current_op->dst, op_len_sum[i]))) {
                VESAL_LOG(ERROR) << "Failed to get physical address, the input may not be DMA-able "
                                    "memory or span pages";
                return StatusCode::kBadData;
            }
        }
        if (crc_num) {
            FillFence(
                dsa_ctx->dsa_hw_descs + desc_offset * dsa_hw_desc_size_,
                dsa_ctx->dsa_completion_records_phys + desc_offset * dsa_completion_record_size_);
            ++desc_offset;
            for (size_t i = 0; i < req->num; ++i) {
                DataFlowMoveOperation* current_op = op + i;
                if (current_op->enable_crc && !IsCopyWithCrcOk(current_op)) {
                    FillCrc(dsa_ctx->dsa_hw_descs + desc_offset * dsa_hw_desc_size_,
                            LookUpAddr(current_op->dst),  // this addr has been validated before
                            op_len_sum[i],
                            VESAL_CRC_REVISE(current_op->seed),
                            dsa_ctx->dsa_completion_records_phys +
                                desc_offset * dsa_completion_record_size_);
                    dsa_ctx->crc_results[i] =
                        dsa_ctx->dsa_completion_records + desc_offset * dsa_completion_record_size_;
                    ++desc_offset;
                }
            }
        }
    }
    req->engine_ctx = static_cast<void*>(dsa_ctx);
    return StatusCode::kOk;
}

StatusCode DsaCtxFactory::BuildCrcCtx(DataFlowRequest* req) {
    DataFlowCrcOperation* op = static_cast<DataFlowCrcOperation*>(req->op);
    auto dsa_ctx = static_cast<DsaCtx*>(req->engine_ctx);
    // desc list = 1*(crc)
    if (req->num == 1) {
        dsa_ctx->desc_num = 1;
        ResetCtx(dsa_ctx);
        uintptr_t src_phys_addr = LookUpAddrIfNotAcrossBound(op->src, op->len);
        if (VESAL_UNLIKELY(!src_phys_addr)) {
            VESAL_LOG(ERROR) << "Failed to get physical address, the input may not be DMA-able "
                                "memory or span pages";
            return StatusCode::kBadData;
        }
        FillCrc(dsa_ctx->dsa_hw_descs,
                src_phys_addr,
                op->len,
                VESAL_CRC_REVISE(op->seed),
                dsa_ctx->dsa_completion_records_phys);
        dsa_ctx->crc_results[0] = dsa_ctx->dsa_completion_records;
    }
    // desc list = 1*(batch) + x*(crc)
    else {
        dsa_ctx->desc_num = req->num + 1;
        ResetCtx(dsa_ctx);
        FillBatch(dsa_ctx->dsa_hw_descs,
                  dsa_ctx->desc_num - 1,  // the fisrt batch desc doesn't count into desc_list_size
                  dsa_ctx->dsa_hw_descs_phys +
                      dsa_hw_desc_size_,  // the batch desc list start from 2nd desc
                  dsa_ctx->dsa_completion_records_phys);
        for (size_t i = 0; i < req->num; ++i) {
            DataFlowCrcOperation* current_op = op + i;
            uintptr_t src_phys_addr = LookUpAddrIfNotAcrossBound(current_op->src, current_op->len);
            if (VESAL_UNLIKELY(!src_phys_addr)) {
                VESAL_LOG(ERROR) << "Failed to get physical address, the input may not be DMA-able "
                                    "memory or span pages";
                return StatusCode::kBadData;
            }
            FillCrc(dsa_ctx->dsa_hw_descs + (i + 1) * dsa_hw_desc_size_,
                    src_phys_addr,
                    current_op->len,
                    VESAL_CRC_REVISE(current_op->seed),
                    dsa_ctx->dsa_completion_records_phys + (i + 1) * dsa_completion_record_size_);
            dsa_ctx->crc_results[i] =
                dsa_ctx->dsa_completion_records + (i + 1) * dsa_completion_record_size_;
        }
    }
    req->engine_ctx = static_cast<void*>(dsa_ctx);
    return StatusCode::kOk;
}

uintptr_t DsaCtxFactory::GetNextAlignedAddress(uintptr_t addr, size_t alignment) {
    // alignment should be power of 2
    VESAL_CHECK(alignment % 2 == 0);
    // Calculate the next aligned address
    uintptr_t alignedAddr = (addr + alignment - 1) & ~(alignment - 1);
    // Convert the aligned integer back to a pointer
    return alignedAddr;
}

}  // namespace data_flow
}  // namespace vesal
