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

#include "vesal/c_api/c_api_vesal.h"

#include <unistd.h>

#include <cstring>
#include <vector>

#include "vesal/codec.h"
#include "vesal/memory_pool.h"
#include "vesal/vesal.h"

// Now ensure vesal::CodecResult and vesal_codec_result_t have the same layout.
static_assert(sizeof(vesal::CodecResult) == sizeof(vesal_codec_result_t),
              "CodecResult and vesal_codec_result_t size mismatch!");

static_assert(alignof(vesal::CodecResult) == alignof(vesal_codec_result_t),
              "CodecResult and vesal_codec_result_t alignment mismatch!");

static_assert(std::is_same<std::underlying_type_t<vesal::StatusCode>, VESAL_ERROR_CODE>::value,
              "StatusCode must be VESAL_ERROR_CODE for C ABI compatibility");

static_assert(std::is_same<decltype(vesal::CodecResult::consumed),
                           decltype(vesal_codec_result_t::consumed)>::value,
              "Field 'consumed' type mismatch between C++ and C structures");

static_assert(std::is_same<decltype(vesal::CodecResult::produced),
                           decltype(vesal_codec_result_t::produced)>::value,
              "Field 'produced' type mismatch between C++ and C structures");

static_assert(std::is_same<decltype(vesal::CodecResult::in_checksum),
                           decltype(vesal_codec_result_t::in_checksum)>::value,
              "Field 'in_checksum' type mismatch between C++ and C structures");

static_assert(std::is_same<decltype(vesal::CodecResult::out_checksum),
                           decltype(vesal_codec_result_t::out_checksum)>::value,
              "Field 'out_checksum' type mismatch between C++ and C structures");

static_assert(
    std::is_same<decltype(vesal::CodecResult::ctx), decltype(vesal_codec_result_t::ctx)>::value,
    "Field 'ctx' type mismatch between C++ and C structures");

#define VESAL_CHECK_OFFSET(field)                                                               \
    static_assert(offsetof(vesal::CodecResult, field) == offsetof(vesal_codec_result_t, field), \
                  "Field offset mismatch: " #field)

VESAL_CHECK_OFFSET(consumed);
VESAL_CHECK_OFFSET(produced);
VESAL_CHECK_OFFSET(in_checksum);
VESAL_CHECK_OFFSET(out_checksum);
VESAL_CHECK_OFFSET(status);
VESAL_CHECK_OFFSET(ctx);
#undef VESAL_CHECK_OFFSET

static_assert(static_cast<uint8_t>(vesal::StatusCode::kOk) == VESAL_OK,
              "StatusCode::kOk must be VESAL_OK for C ABI compatibility");
static_assert(
    static_cast<uint8_t>(vesal::StatusCode::kInvalidArgument) == VESAL_INVALID_ARGUMENT,
    "StatusCode::kInvalidArgument must be VESAL_INVALID_ARGUMENT for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kNotSupported) == VESAL_NOT_SUPPORTED,
              "StatusCode::kNotSupported must be VESAL_NOT_SUPPORTED for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kResourceBusy) == VESAL_RESOURCE_BUSY,
              "StatusCode::kResourceBusy must be VESAL_RESOURCE_BUSY for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kHardwareError) == VESAL_HARDWARE_ERROR,
              "StatusCode::kHardwareError must be VESAL_HARDWARE_ERROR for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kChannelError) == VESAL_CHANNEL_ERROR,
              "StatusCode::kChannelError must be VESAL_CHANNEL_ERROR for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kTimeout) == VESAL_TIMEOUT,
              "StatusCode::kTimeout must be VESAL_TIMEOUT for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kOverflow) == VESAL_OVERFLOW,
              "StatusCode::kOverflow must be VESAL_OVERFLOW for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kBadData) == VESAL_BAD_DATA,
              "StatusCode::kBadData must be VESAL_BAD_DATA for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kShouldRetry) == VESAL_SHOULD_RETRY,
              "StatusCode::kShouldRetry must be VESAL_SHOULD_RETRY for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kDropped) == VESAL_DROPPED,
              "StatusCode::kDropped must be VESAL_DROPPED for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kPermanentError) == VESAL_PERMANENT_ERROR,
              "StatusCode::kPermanentError must be VESAL_PERMANENT_ERROR for C ABI compatibility");
static_assert(static_cast<uint8_t>(vesal::StatusCode::kUnknown) == VESAL_UNKNOWN,
              "StatusCode::kUnknown must be VESAL_UNKNOWN for C ABI compatibility");

void default_vesal_flags(vesal_flags_t* flags) {
    flags->vesal_codec_qat_max_in_qat_num = FLAGS_vesal_codec_qat_max_in_qat_num;
    flags->vesal_metrics_sample_rate = FLAGS_vesal_metrics_sample_rate;
    flags->vesal_metrics_disable_poller_metrics =
        static_cast<VESAL_BOOL>(FLAGS_vesal_metrics_disable_poller_metrics);
    size_t section_name_sz = std::min(sizeof(flags->vesal_codec_qat_section_name),
                                      FLAGS_vesal_codec_qat_section_name.size());
    strncpy(flags->vesal_codec_qat_section_name,
            FLAGS_vesal_codec_qat_section_name.c_str(),
            section_name_sz);
    flags->vesal_codec_qat_section_name[section_name_sz] = '\0';
    flags->vesal_log_console_output = static_cast<VESAL_BOOL>(FLAGS_vesal_log_console_output);
    flags->vesal_log_level = FLAGS_vesal_log_level;
    // Ha related
    flags->vesal_qat_ha_min_counting_time_window_us =
        FLAGS_vesal_qat_ha_min_counting_time_window_us;
    flags->vesal_qat_ha_sliding_time_window_sec = FLAGS_vesal_qat_ha_sliding_time_window_sec;
    flags->vesal_qat_ha_trigger_error_num = FLAGS_vesal_qat_ha_trigger_error_num;
    // Event mode related
    flags->vesal_codec_qat_shared_mode_poller_op_process_num =
        FLAGS_vesal_codec_qat_shared_mode_poller_op_process_num;
    flags->vesal_codec_qat_shared_mode_poller_num = FLAGS_vesal_codec_qat_shared_mode_poller_num;
    flags->vesal_codec_qat_shared_mode_poller_num = FLAGS_vesal_codec_qat_shared_mode_poller_num;
    flags->vesal_codec_qat_shared_mode_poller_sleep_time_us =
        FLAGS_vesal_codec_qat_shared_mode_poller_sleep_time_us;
}

void default_vesal_memory_pool_init_options(vesal_mem_pool_init_opt_t* mmp_opt) {
    mmp_opt->init_mem_pool = VESAL_TRUE;
    mmp_opt->prealloc_page_size = 2 * 1024 * 1024;
    mmp_opt->prealloc_size_mb = 4096;
    mmp_opt->cache_recycle_threshold_size_mb = 256;
    mmp_opt->recycle_chunk_num = 16;
    mmp_opt->fetch_chunk_num = 8;
}

void default_vesal_init_options(vesal_init_options_t* opts) {
    opts->codec_init_qat = VESAL_TRUE;
    opts->cypher_init_qat = VESAL_TRUE;
    default_vesal_flags(&opts->flags);
    default_vesal_memory_pool_init_options(&opts->mem_pool_init_opt);
}

void translate_flags(vesal_flags_t* flags, vesal::InitOptions* opts) {
    FLAGS_vesal_codec_qat_max_in_qat_num = flags->vesal_codec_qat_max_in_qat_num;
    FLAGS_vesal_metrics_sample_rate = flags->vesal_metrics_sample_rate;
    FLAGS_vesal_metrics_disable_poller_metrics =
        static_cast<bool>(flags->vesal_metrics_disable_poller_metrics);
    FLAGS_vesal_codec_qat_section_name = flags->vesal_codec_qat_section_name;
    FLAGS_vesal_log_console_output = static_cast<bool>(flags->vesal_log_console_output);
    FLAGS_vesal_log_level = flags->vesal_log_level;
    // Ha related
    FLAGS_vesal_qat_ha_min_counting_time_window_us =
        flags->vesal_qat_ha_min_counting_time_window_us;
    FLAGS_vesal_qat_ha_sliding_time_window_sec = flags->vesal_qat_ha_sliding_time_window_sec;
    FLAGS_vesal_qat_ha_trigger_error_num = flags->vesal_qat_ha_trigger_error_num;
    // Event mode related
    FLAGS_vesal_codec_qat_shared_mode_poller_op_process_num =
        flags->vesal_codec_qat_shared_mode_poller_op_process_num;
    FLAGS_vesal_codec_qat_shared_mode_poller_num = flags->vesal_codec_qat_shared_mode_poller_num;
    FLAGS_vesal_codec_qat_shared_mode_poller_sleep_time_us =
        flags->vesal_codec_qat_shared_mode_poller_sleep_time_us;
}

void translate_memory_pool_init_options(vesal_mem_pool_init_opt_t* mmp_opt_c,
                                        vesal::MemoryPoolInitOption* mpo) {
    mpo->init_mem_pool = static_cast<bool>(mmp_opt_c->init_mem_pool);
    mpo->prealloc_page_size = vesal::ToHugePageSize(mmp_opt_c->prealloc_page_size);
    mpo->prealloc_size_mb = mmp_opt_c->prealloc_size_mb;
    mpo->cache_recycle_threshold_size_mb = mmp_opt_c->cache_recycle_threshold_size_mb;
    mpo->recycle_chunk_num = mmp_opt_c->recycle_chunk_num;
    mpo->fetch_chunk_num = mmp_opt_c->fetch_chunk_num;
}

VESAL_BOOL vesal_init(vesal_init_options_t* init_opts) {
    vesal::InitOptions internal_opts{};
    translate_flags(&init_opts->flags, &internal_opts);
    translate_memory_pool_init_options(&init_opts->mem_pool_init_opt,
                                       &internal_opts.mem_pool_init_opt);
    internal_opts.codec_init_opt.init_qat = static_cast<bool>(init_opts->codec_init_qat);
    internal_opts.cypher_init_opt.init_qat = static_cast<bool>(init_opts->cypher_init_qat);
    // TODO(sjj): Only support qat for now.
    internal_opts.data_flow_init_opt.init_dsa = false;
    return vesal::Init(internal_opts) ? VESAL_TRUE : VESAL_FALSE;
}

void vesal_uninit() {
    vesal::Uninit();
}

VESAL_BOOL vesal_register(const vesal_memory_info_t* infos, const int n) {
    std::vector<vesal::MemoryInfo> internal_infos(n);
    for (int i = 0; i < n; i++) {
        internal_infos[i] = {.virtual_addr = infos[i].virtual_addr,
                             .physical_addr = infos[i].physical_addr,
                             .len = infos[i].len,
                             .page_size = infos[i].page_size};
    }
    return static_cast<VESAL_BOOL>(vesal::MemoryPool::GetInstance()->Register(internal_infos));
}

void* vesal_allocate(size_t size) {
    return vesal::MemoryPool::GetInstance()->Allocate(size);
}

void vesal_free(void* ptr) {
    vesal::MemoryPool::GetInstance()->Deallocate(ptr);
}

void default_vesal_codec_channel_options(vesal_codec_channel_options_t* opts) {
    opts->user_cb = nullptr;
    opts->mode = VESAL_CHANNEL_MODE_DEDICATED;
    opts->ha_policy = VESAL_HA_POLICY_NONE;
    opts->engine_type = VESAL_CODEC_ENGINE_TYPE_QAT;
    opts->comp_algorithm = VESAL_CODEC_ALGORITHM_LZ4;
    opts->comp_level = VESAL_CODEC_COMP_LEVEL_DEFAULT;
    opts->checksum_type = VESAL_CODEC_CHECKSUM_TYPE_NONE;
    opts->compressed_checksum = VESAL_TRUE;
    opts->node_affinity = -1;
    opts->timeout_ms = 3000;
}

VESAL_ERROR_CODE vesal_create_codec_channel(vesal_codec_channel_options_t* opts,
                                            VesalCodecChannelHandle* handle) {
    vesal::CodecChannelOption internal_opts{};
    internal_opts.user_cb = reinterpret_cast<vesal::UserCallback>(opts->user_cb);
    internal_opts.mode = static_cast<vesal::ChannelMode>(opts->mode);
    internal_opts.ha_policy = static_cast<vesal::HaPolicy>(opts->ha_policy);
    internal_opts.engine_type = static_cast<vesal::CodecEngineType>(opts->engine_type);
    internal_opts.comp_algorithm = static_cast<vesal::CodecAlgorithm>(opts->comp_algorithm);
    internal_opts.comp_level = static_cast<vesal::CodecCompLevel>(opts->comp_level);
    internal_opts.checksum_type = static_cast<vesal::CodecChecksumType>(opts->checksum_type);
    internal_opts.compressed_checksum = static_cast<bool>(opts->compressed_checksum);
    internal_opts.allocation_option.node_affinity = opts->node_affinity;
    internal_opts.timeout_ms = opts->timeout_ms;
    auto r = vesal::CodecChannel::CreateCodecChannel(internal_opts);
    if (!r.first.ok()) {
        return static_cast<VESAL_ERROR_CODE>(r.first.code());
    }
    *handle = r.second.release();
    return VESAL_OK;
}

void vesal_destroy_codec_channel(VesalCodecChannelHandle handle) {
    if (handle != nullptr) {
        auto* chnnl = reinterpret_cast<vesal::CodecChannel*>(handle);
        auto r = chnnl->Close();
        delete chnnl;
    }
}

static vesal_codec_result_t convert_codec_result(const vesal::CodecResult& from) {
    return {.consumed = from.consumed,
            .produced = from.produced,
            .in_checksum = from.in_checksum,
            .out_checksum = from.out_checksum,
            .status = static_cast<VESAL_ERROR_CODE>(from.status),
            .ctx = nullptr};
}

vesal_codec_result_t vesal_codec_compress(VesalCodecChannelHandle handle,
                                          unsigned char* src,
                                          unsigned int src_len,
                                          unsigned char* dst,
                                          unsigned int dst_len) {
    auto* chnnl = reinterpret_cast<vesal::CodecChannel*>(handle);
    VESAL_DCHECK(chnnl != nullptr);
    return convert_codec_result(chnnl->Compress(src, src_len, dst, dst_len));
}

vesal_codec_result_t vesal_codec_decompress(VesalCodecChannelHandle handle,
                                            unsigned char* src,
                                            unsigned int src_len,
                                            unsigned char* dst,
                                            unsigned int dst_len) {
    auto* chnnl = reinterpret_cast<vesal::CodecChannel*>(handle);
    VESAL_DCHECK(chnnl != nullptr);
    return convert_codec_result(chnnl->Decompress(src, src_len, dst, dst_len));
}

VESAL_ERROR_CODE vesal_codec_compress_async(VesalCodecChannelHandle handle,
                                            unsigned char* src,
                                            unsigned int src_len,
                                            unsigned char* dst,
                                            unsigned int dst_len,
                                            void* ctx) {
    auto* chnnl = reinterpret_cast<vesal::CodecChannel*>(handle);
    VESAL_DCHECK(chnnl != nullptr);
    auto r = chnnl->CompressAsync(src, src_len, dst, dst_len, ctx);
    return static_cast<VESAL_ERROR_CODE>(r);
}

VESAL_ERROR_CODE vesal_codec_compress_sgl_async(VesalCodecChannelHandle handle,
                                                unsigned char* src[],
                                                unsigned int src_len[],
                                                unsigned int src_num,
                                                unsigned char* dst,
                                                unsigned int dst_len,
                                                void* ctx) {
    auto* chnnl = reinterpret_cast<vesal::CodecChannel*>(handle);
    VESAL_DCHECK(chnnl != nullptr);
    std::vector<unsigned char*> src_vec(src, src + src_num);
    std::vector<unsigned int> src_len_vec(src_len, src_len + src_num);
    auto status = chnnl->CompressSGLAsync(src_vec, src_len_vec, dst, dst_len, ctx);
    return static_cast<VESAL_ERROR_CODE>(status);
}

VESAL_ERROR_CODE vesal_codec_decompress_async(VesalCodecChannelHandle handle,
                                              unsigned char* src,
                                              unsigned int src_len,
                                              unsigned char* dst,
                                              unsigned int dst_len,
                                              void* ctx) {
    auto* chnnl = reinterpret_cast<vesal::CodecChannel*>(handle);
    VESAL_DCHECK(chnnl != nullptr);
    auto r = chnnl->DecompressAsync(src, src_len, dst, dst_len, ctx);
    return static_cast<VESAL_ERROR_CODE>(r);
}

VESAL_ERROR_CODE vesal_codec_decompress_sgl_async(VesalCodecChannelHandle handle,
                                                  unsigned char* src[],
                                                  unsigned int src_len[],
                                                  unsigned int src_num,
                                                  unsigned char* dst,
                                                  unsigned int dst_len,
                                                  void* ctx) {
    auto* chnnl = reinterpret_cast<vesal::CodecChannel*>(handle);
    VESAL_DCHECK(chnnl != nullptr);
    std::vector<unsigned char*> src_vec(src, src + src_num);
    std::vector<unsigned int> src_len_vec(src_len, src_len + src_num);
    auto status = chnnl->DecompressSGLAsync(src_vec, src_len_vec, dst, dst_len, ctx);
    return static_cast<VESAL_ERROR_CODE>(status);
}

ssize_t vesal_codec_poll(VesalCodecChannelHandle handle,
                         vesal_codec_result_t result[],
                         unsigned int result_num,
                         int timeout) {
    auto* chnnl = reinterpret_cast<vesal::CodecChannel*>(handle);
    VESAL_DCHECK(chnnl != nullptr);
    // For performance reason we treat two types has the same layout to avoid memcpy.
    auto* cxx_results = reinterpret_cast<vesal::CodecResult*>(result);
    return chnnl->Poll(cxx_results, result_num, timeout);
}
