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

#include "qat_buffer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>

#include "common/memory_pool_helper.h"
#include "common/metrics_internal.h"
#include "common/object_pool.h"
#include "common/qat/qat_hardware_api_wrapper.h"
#include "vesal/log_setting.h"
#include "vesal/memory_pool.h"
#include "vesal/vesal.h"

extern "C" {
#include <cpa.h>  // CpaStatus, CpaBufferList
}

namespace vesal {
namespace qat {

bool QatBuf::InitMeta(QatUnit* qat_unit, uint32_t meta_size) {
    qat_unit_ = qat_unit;
    size_t cpa_buffer_list_size = VESAL_MAX_SGL_NUM * sizeof(CpaFlatBuffer) + sizeof(CpaBufferList);
    cpa_buffer_list_ = reinterpret_cast<CpaBufferList*>(malloc(cpa_buffer_list_size));
    if (VESAL_UNLIKELY(!cpa_buffer_list_)) {
        VESAL_LOG(ERROR) << "Not enough memory, try to malloc bytes:" << cpa_buffer_list_size;
        return false;
    }
    cpa_buffer_list_->pBuffers = reinterpret_cast<CpaFlatBuffer*>(cpa_buffer_list_ + 1);
    cpa_buffer_list_->numBuffers = 0;
    cpa_buffer_list_->pPrivateMetaData =
        static_cast<uint8_t*>(MemoryPool::GetInstance()->Allocate(meta_size));
    if (VESAL_UNLIKELY(!cpa_buffer_list_->pPrivateMetaData)) {
        VESAL_LOG(ERROR) << "MemoryPool Allocate failed, try to allocate bytes from memory pool:"
                         << meta_size;
        free(cpa_buffer_list_);
        return false;
    }
    return true;
}

void QatBuf::FreeMeta() {
    VESAL_CHECK(cpa_buffer_list_->numBuffers == 0)
        << "Must free data before reset meta, cpa_buffer_list_->numBuffers="
        << cpa_buffer_list_->numBuffers;
    MemoryPool::GetInstance()->Deallocate(cpa_buffer_list_->pPrivateMetaData);
    free(cpa_buffer_list_);

    qat_unit_ = nullptr;
    cpa_buffer_list_ = nullptr;
    memset(dma_able_, 0, sizeof(dma_able_));
    dst_usr_data_ = nullptr;
    dst_usr_data_len_ = 0;
    next_ = nullptr;
}

// Skip the front_offset(i.e header part) from original data.
// Note the caller shall ensure front_offset is not exceeds usr_len
inline void FindSglStartPoint(const IOVector& usr_data,
                              size_t front_offset,
                              size_t* start_index,
                              size_t* offset_in_start_index) {
    if (front_offset == 0) {
        *start_index = 0;
        *offset_in_start_index = 0;
        VESAL_LOG(DEBUG) << "front_offset == 0, fast return";
        return;
    }
    size_t front_left_size = front_offset;
    size_t curr_index = 0;
    while (true) {
        if (VESAL_LIKELY(usr_data.blocks[curr_index].size > front_left_size)) {
            break;
        }
        front_left_size -= usr_data.blocks[curr_index++].size;
    }
    *start_index = curr_index;
    *offset_in_start_index = front_left_size;
    VESAL_LOG(DEBUG) << "Found start point, start_index=" << *start_index
                     << ", offset_in_start_index=" << *offset_in_start_index;
}

// Find the dma end point for back (skip footer)
// Note the caller shall ensure back_offset is not exceeds usr_len
inline void FindSglEndPoint(const IOVector& usr_data,
                            size_t back_offset,
                            size_t* end_index,
                            size_t* end_index_len) {
    if (back_offset == 0) {
        *end_index = usr_data.num_blocks - 1;
        *end_index_len = usr_data.blocks[*end_index].size;
        VESAL_LOG(DEBUG) << "back_offset == 0, fast return";
        return;
    }
    ssize_t curr_index = usr_data.num_blocks - 1;
    size_t back_left_size = back_offset;
    while (true) {
        if (VESAL_LIKELY(usr_data.blocks[curr_index].size > back_left_size)) {
            break;
        }
        back_left_size -= usr_data.blocks[curr_index--].size;
    }
    *end_index = curr_index;
    *end_index_len = usr_data.blocks[curr_index].size - back_left_size;
    VESAL_LOG(DEBUG) << "Found end point, end_index=" << *end_index
                     << ", end_index_len=" << *end_index_len;
}

bool QatBuf::FillOneBuffer(unsigned char* usr_buf,
                           unsigned int usr_buf_len,
                           bool need_copy_content,
                           CpaFlatBuffer* cpa_buf,
                           bool* dma_able) {
    cpa_buf->dataLenInBytes = usr_buf_len;
    // Note if the LookUpAddrAcrossBound returns greater than 1, it means the usr_buf is not
    // continuous in dma memory(e.g, cross page boundary). Currently we don't handle and fallback to
    // copy mode.
    constexpr size_t lookup_max_num = 2;
    uint64_t paddrs[lookup_max_num] = {};
    size_t paddr_sizes[lookup_max_num] = {};
    // TODO(sjj): support cross page zerocopy.
    *dma_able =
        qat_unit_->SvmEnabled() ||
        LookUpAddrAcrossBound(usr_buf, usr_buf_len, paddrs, paddr_sizes, lookup_max_num) == 1;
    // Zero copy fast path
    if (*dma_able) {
        cpa_buf->pData = usr_buf;
        return true;
    }
    // Allocate DMA memory
    cpa_buf->pData = static_cast<unsigned char*>(MemoryPool::GetInstance()->Allocate(usr_buf_len));
    if (cpa_buf->pData == nullptr) {
        return false;
    }
    if (need_copy_content) {
        memcpy(cpa_buf->pData, usr_buf, cpa_buf->dataLenInBytes);
        g_metric_memcpy_throughput->Add(cpa_buf->dataLenInBytes);
    }
    return true;
}

/**
 * Usually the layout in terms of SGL and header footer looks like:
 *
 *        dma_begin_index            dma_end_index
 *              │                           │
 *              │                           │
 *              ▼                           ▼
 *┌──────┬──────┬──┬───┬──────┬──────┬──────┬───┬──┬──────┬──────┐
 *│      │      │he│DMA│      │ DMA  │      │DMA│fo│      │      │
 *│header│ ...  │ad│dat│ ...  │ data │ ...  │dat│ot│ ...  │footer│(SGL units)
 *│      │      │er│a  │      │      │      │a  │er│      │      │
 *├──────┴──────┴──┼───┴──────┴──────┴──────┴───┴──┴──────┴──────┤
 *│                │                            ▲                │
 *│                │                            │                │
 *│                │                            │                │
 *│          dma_begin_offset              dma_end_offset        │
 *│                │                            │                │
 *│◄─front_offset─►│◄────────DMA blocks────────►│◄──back_offset─►│
 *│    (header)    │                            │    (footer)    │            *
 * We handle the fill process in the following order:
 * 1. Calculate dma_begin_index, dma_begin_offset, dma_begin_offset and dma_end_offset etc.
 * 2. Handle the blocks in range usr_data[dma_begin_offset, dma_end_offset].
 *
 * Header and footer(Data before dma_begin_offset and after dma_end_offset) will be skipped. Don't
 * pass into DMA buffers.
 */
bool QatBuf::FillSrc(const IOVector& usr_data, size_t front_offset, size_t back_offset) {
    VESAL_DCHECK(usr_data.num_blocks <= VESAL_MAX_SGL_NUM);
    // Ensure the offset params are valid, no need check later
    size_t total_len = 0;
    for (uint32_t i = 0; i < usr_data.num_blocks; ++i) {
        total_len += usr_data.blocks[i].size;
    }
    if (front_offset + back_offset >= total_len) {
        VESAL_LOG(ERROR) << "Too large front_offset and back_offset, front_offset=" << front_offset
                         << ", back_offset=" << back_offset << ", but total_len=" << total_len;
        return false;
    }

    // find start point
    size_t dma_begin_index = 0;
    size_t dma_begin_offset = 0;
    FindSglStartPoint(usr_data, front_offset, &dma_begin_index, &dma_begin_offset);
    unsigned char* first_unit = usr_data.blocks[dma_begin_index].data + dma_begin_offset;
    size_t first_unit_len = usr_data.blocks[dma_begin_index].size - dma_begin_offset;

    // find end point
    size_t dma_end_index = 0;
    size_t dma_end_len = 0;
    FindSglEndPoint(usr_data, back_offset, &dma_end_index, &dma_end_len);

    bool fill_r = false;
    CpaFlatBuffer* cpa_buf = nullptr;
    unsigned char* usr_buf = nullptr;
    size_t dma_data_len = 0;
    // is_header_footer_same_block == true means there is only one dma block needed. Handle it
    // seperately.
    bool is_header_footer_same_block = dma_begin_index == dma_end_index;
    if (is_header_footer_same_block) {
        cpa_buf = &(cpa_buffer_list_->pBuffers[0]);
        dma_data_len = dma_end_len - dma_begin_offset;
        usr_buf = first_unit;
        fill_r = FillOneBuffer(
            usr_buf, dma_data_len, /*need_copy_content*/ true, cpa_buf, &dma_able_[0]);
        if (VESAL_UNLIKELY(!fill_r)) {
            VESAL_LOG(ERROR) << "Allocate DMA memory for buffer failed, len="
                             << dma_end_len - dma_begin_offset;
            return false;
        }
        cpa_buffer_list_->numBuffers = 1;
        return true;
    }

    cpa_buffer_list_->numBuffers = 0;
    for (size_t i = dma_begin_index; i <= dma_end_index; ++i) {
        size_t curr_idx = cpa_buffer_list_->numBuffers;
        cpa_buf = &(cpa_buffer_list_->pBuffers[curr_idx]);
        if (i == dma_begin_index) {
            // header block
            dma_data_len = first_unit_len;
            usr_buf = first_unit;
        } else if (i == dma_end_index) {
            // footer block
            dma_data_len = dma_end_len;
            usr_buf = usr_data.blocks[dma_end_index].data;
        } else {
            // data blocks
            dma_data_len = usr_data.blocks[i].size;
            usr_buf = usr_data.blocks[i].data;
        }
        fill_r = FillOneBuffer(usr_buf,
                               dma_data_len,
                               /*need_copy_content*/ true,
                               cpa_buf,
                               &dma_able_[curr_idx]);
        if (VESAL_UNLIKELY(!fill_r)) {
            VESAL_LOG(ERROR) << "Allocate DMA memory for buffer failed, dma_data_len="
                             << dma_data_len;
            // Clear the memory that already allocated
            FreeDataIfNecessary();
            return false;
        }
        cpa_buffer_list_->numBuffers++;
    }
    return true;
}

bool QatBuf::FillDst(const IOBlock& usr_data, size_t front_offset, size_t back_offset) {
    if (VESAL_UNLIKELY(front_offset + back_offset >= usr_data.size)) {
        VESAL_LOG(ERROR) << "front_offset=" << front_offset << ", back_offset=" << back_offset
                         << " >= usr_data.size=" << usr_data.size;
        return false;
    }
    dst_usr_data_ = usr_data.data;
    dst_usr_data_len_ = usr_data.size;

    unsigned char* dma_start = usr_data.data + front_offset;
    unsigned int dma_len = usr_data.size - front_offset - back_offset;
    cpa_buffer_list_->numBuffers = 1;
    bool fill_r = FillOneBuffer(dma_start,
                                dma_len,
                                /*need_copy_content*/ false,
                                &(cpa_buffer_list_->pBuffers[0]),
                                &dma_able_[0]);
    if (VESAL_UNLIKELY(!fill_r)) {
        VESAL_LOG(ERROR) << "FillOneBuffer failed, dma_len=" << dma_len;
    }
    return fill_r;
}

void QatBuf::FreeDataIfNecessary() {
    if (qat_unit_->SvmEnabled()) {
        cpa_buffer_list_->numBuffers = 0;
        return;
    }
    for (size_t i = 0; i < cpa_buffer_list_->numBuffers; ++i) {
        if (!dma_able_[i]) {
            MemoryPool::GetInstance()->Deallocate(cpa_buffer_list_->pBuffers[i].pData);
        }
        cpa_buffer_list_->pBuffers[i].pData = nullptr;
        cpa_buffer_list_->pBuffers[i].dataLenInBytes = 0;
    }
    cpa_buffer_list_->numBuffers = 0;
}

void QatBuf::DecompCopyBackIfNecessary(size_t produced) {
    // by design the dst should not be SGL
    VESAL_DCHECK(cpa_buffer_list_->numBuffers == 1)
        << "should only be used for dst write back while cpa_buffer_list_->numBuffers=1, "
           "cpa_buffer_list_->numBuffers="
        << cpa_buffer_list_->numBuffers;
    // No need to check length as we already checked in submission path
    if (!qat_unit_->SvmEnabled() && !dma_able_[0]) {
        memcpy(dst_usr_data_, cpa_buffer_list_->pBuffers[0].pData, produced);
        g_metric_memcpy_throughput->Add(produced);
    }
}

void QatBuf::CompCopyBackIfNecessary(size_t produced,
                                     const char* header,
                                     size_t front_offset,
                                     const char* footer,
                                     size_t back_offset) {
    // by design the dst should not be SGL
    VESAL_DCHECK(cpa_buffer_list_->numBuffers == 1)
        << "should only be used for dst write back while cpa_buffer_list_->numBuffers=1, "
           "cpa_buffer_list_->numBuffers="
        << cpa_buffer_list_->numBuffers;
    // No need to check length here because we already did in submission path
    memcpy(dst_usr_data_, header, front_offset);
    if (!qat_unit_->SvmEnabled() && !dma_able_[0]) {
        memcpy(dst_usr_data_ + front_offset, cpa_buffer_list_->pBuffers[0].pData, produced);
        g_metric_memcpy_throughput->Add(produced);
    }
    memcpy(dst_usr_data_ + front_offset + produced, footer, back_offset);
}

bool QatBufCache::Init(bool codec) {
    CpaInstanceHandle* inst_handle = qat_unit_->GetInstanceHandle();
    CpaStatus cpa_status =
        codec ? GetQatApiWrapper()->QAT_cpaDcBufferListGetMetaSize(
                    *inst_handle, VESAL_MAX_SGL_NUM, &buffer_list_private_meta_size_)
              : GetQatApiWrapper()->QAT_cpaCyBufferListGetMetaSize(
                    *inst_handle, VESAL_MAX_SGL_NUM, &buffer_list_private_meta_size_);
    VESAL_CHECK(cpa_status == CPA_STATUS_SUCCESS) << "cpa_status = " << cpa_status;
    VESAL_LOG(DEBUG) << "sizeof(QatBuf)=" << sizeof(QatBuf)
                     << ", buffer_list_private_meta_size_=" << buffer_list_private_meta_size_;
    for (size_t i = 0; i < buf_size_; ++i) {
        auto* one = vesal::get_tls_object<QatBuf>();
        if (VESAL_UNLIKELY(!one->InitMeta(qat_unit_, buffer_list_private_meta_size_))) {
            vesal::return_tls_object<QatBuf>(one);
            Clear();
            return false;
        }
        if (VESAL_UNLIKELY(!bufs_)) {
            bufs_ = one;
        } else {
            one->NextMutable() = bufs_;
            bufs_ = one;
        }
    }
    in_cache_num_ = buf_size_;
    is_inited_ = true;
    return true;
}

QatBuf* QatBufCache::GetOne() {
    QatBuf* one = bufs_;
    if (VESAL_LIKELY(one)) {
        bufs_ = one->NextMutable();
        one->NextMutable() = nullptr;
        --in_cache_num_;
        return one;
    }
    VESAL_CHECK(in_cache_num_ == 0);
    return nullptr;
}

void QatBufCache::ReturnOne(QatBuf* one) {
    if (VESAL_UNLIKELY(!one)) {
        return;
    }
    one->NextMutable() = bufs_;
    bufs_ = one;
    ++in_cache_num_;
}

void QatBufCache::Clear() {
    QatBuf* one = bufs_;
    while (one != nullptr) {
        QatBuf* next = one->NextMutable();
        one->FreeMeta();
        vesal::return_tls_object<QatBuf>(one);
        one = next;
        --in_cache_num_;
    }
    buffer_list_private_meta_size_ = 0;
    bufs_ = nullptr;
    is_inited_ = false;
    VESAL_CHECK(in_cache_num_ == 0) << "in_cache_num_: " << in_cache_num_;
}

}  // namespace qat
}  // namespace vesal
