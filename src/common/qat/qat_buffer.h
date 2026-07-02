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

#pragma once

#include <cstdint>
#include <vector>

#include "codec/codec_common.h"
#include "common/qat/qat_unit.h"
#include "vesal/io_vector.h"

extern "C" {
#include <cpa.h>
#include <dc/cpa_dc.h>
}

namespace vesal {
namespace qat {

class QatBuf {
public:
    bool InitMeta(QatUnit* qat_unit, uint32_t meta_size);
    // If SVM is enabled or usr_data is DMA-able, set CpaBufferList's pData to the usr_data address
    // (i.e zero copy). Otherwise, allocate DMA memory from MemoryPool and set pData. If zero copy
    // not enabled, data is always copied from user buffer to DMA buffer. On success, return true,
    // others return false. The most possible reason for failure is out of MemoryPool memory.
    // Skip offset size from data front, as QAT does not handle that part.
    bool FillSrc(const IOVector& usr_data, size_t front_offset = 0, size_t back_offset = 0);
    bool FillDst(const IOBlock& dst, size_t front_offset = 0, size_t back_offset = 0);
    // Copy back the data from DMA buffer to user data if in copy mode. For decomp direction, no
    // need to consider format.
    void DecompCopyBackIfNecessary(size_t produced);
    // Copy back the data from DMA buffer to user data if in copy mode. In comp dir, need to
    // assemble header, compressed data and footer into one user buffer.
    void CompCopyBackIfNecessary(size_t produced,
                                 const char* header,
                                 size_t front_offset,
                                 const char* footer,
                                 size_t back_offset);
    // user must call this to free any possible data allocated by FillData before using it
    // again or destruction.
    // Reentrant: yes
    void FreeDataIfNecessary();
    // user must call this to free any possible meta memory for CpaBufferList before destruction.
    void FreeMeta();

    CpaBufferList* GetCpaBufferList() {
        return cpa_buffer_list_;
    }

    QatBuf*& NextMutable() {
        return next_;
    }

    unsigned int GetDstDataLen() {
        return dst_usr_data_len_;
    }

private:
    // Move user data usr_buf into QAT's CpaFlatBuffer cpa_buf. It will first update dma_able based
    // on the address. Then if dma_able == true, which means usr_buf is dma-able, assign the pointer
    // directly (i.e zero-copy). Otherwise allocate DMA buffer from memory pool and use that memory
    // to pass content to QAT driver. If need_copy_content == true, then it also needs to
    // copy data content from usr_buf into internal-allocated DMA buffer. The reason is, we should
    // copy data content when dealing with src, but no need copy for dst.
    // Return true if success, false if failure.
    bool FillOneBuffer(unsigned char* usr_buf,
                       unsigned int usr_buf_len,
                       bool need_copy_content,
                       CpaFlatBuffer* cpa_buf,
                       bool* dma_able);

    QatUnit* qat_unit_ = nullptr;
    // Todo: Combine cpa_buffer_list_ and dma_able_
    CpaBufferList* cpa_buffer_list_ = nullptr;
    bool dma_able_[VESAL_MAX_SGL_NUM] = {
        false};  // describes if each CpaFlatBuffer in cpa_buffer_list_ is dma-able.

    // Only dst needs to store the usr data because we need to copy the data from DMA buffer back to
    // the user buffer. This buffer is only used when QatBuf being dst buffer.
    unsigned char* dst_usr_data_ = nullptr;
    unsigned int dst_usr_data_len_ = 0;

    QatBuf* next_ = nullptr;
};

class QatBufCache {
public:
    QatBufCache(QatUnit* qat_unit, size_t max_in_qat_size)
        : qat_unit_(qat_unit),
          bufs_(nullptr),
          buffer_list_private_meta_size_(0),
          is_inited_(false),
          in_cache_num_(0),
          // Multiplied by 2 because each request involves 2 qat buffers: both src and dst buffers.
          // Add the divided-by-2 part because we allow the cache to be extended by this number. In
          // case of HA happend, there might be hanging requests holding the QatBufs and never
          // return. So we need some extra.
          buf_size_((max_in_qat_size << 1) + (max_in_qat_size >> 1)) {}
    ~QatBufCache() {
        Clear();
    }

    bool Init(bool codec = true);
    // Return value can be nullptr due to no more QatBuf in the cache
    QatBuf* GetOne();
    // if buf == nullptr, simply ignore
    void ReturnOne(QatBuf* one);
    void Clear();

private:
    QatUnit* qat_unit_;
    QatBuf* bufs_;
    uint32_t buffer_list_private_meta_size_;  // all meta size should be the same in this session
    bool is_inited_;                          // if the buf cache is allocated.
    uint32_t in_cache_num_;                   // total node in cache
    size_t buf_size_;  // QatBuf number for QatBufCache. The number is bound to the number of
                       // concurrency for one QAT instance.
};

}  // namespace qat
}  // namespace vesal
