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

#include <stdint.h>
#include <unistd.h>

// C APIs for vesal. Wrapper for C++ APIs.
#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t VESAL_ERROR_CODE;

#define VESAL_OK 0
#define VESAL_INVALID_ARGUMENT 1
#define VESAL_NOT_SUPPORTED 2
#define VESAL_RESOURCE_BUSY 3
#define VESAL_HARDWARE_ERROR 4
#define VESAL_CHANNEL_ERROR 5
#define VESAL_TIMEOUT 6
#define VESAL_OVERFLOW 7
#define VESAL_BAD_DATA 8
#define VESAL_SHOULD_RETRY 9
#define VESAL_DROPPED 10
#define VESAL_PERMANENT_ERROR 11
#define VESAL_UNKNOWN 12

typedef enum _VESAL_BOOL { VESAL_FALSE = (0 == 1), VESAL_TRUE = (1 == 1) } VESAL_BOOL;

#define VESAL_FLAGS_STR_MAX_LEN 255
// Currently only support codec related flags, data flow and cypher not involved.
typedef struct _vesal_flags_t {
    uint32_t vesal_codec_qat_max_in_qat_num;                         // default: 8190
    uint32_t vesal_metrics_sample_rate;                              // default: 255
    VESAL_BOOL vesal_metrics_disable_poller_metrics;                 // default: VESAL_FALSE
    char vesal_codec_qat_section_name[VESAL_FLAGS_STR_MAX_LEN + 1];  // default: ""
    VESAL_BOOL vesal_log_console_output;                             // default: VESAL_FALSE
    int32_t vesal_log_level;                                         // default: 2
    // Ha related
    uint32_t vesal_qat_ha_min_counting_time_window_us;  // default: 50
    uint32_t vesal_qat_ha_sliding_time_window_sec;      // default: 1 * 60
    uint32_t vesal_qat_ha_trigger_error_num;            // default: 5
    // Event mode related
    uint32_t vesal_codec_qat_shared_mode_poller_op_process_num;  // default: 16
    uint32_t vesal_codec_qat_shared_mode_poller_num;             // default: 2
    uint32_t vesal_codec_qat_shared_mode_poller_sleep_time_us;   // default: 0
} vesal_flags_t;

typedef struct _vesal_mem_pool_init_opt_t {
    VESAL_BOOL init_mem_pool;                  // default: true
    uint32_t prealloc_size_mb;                 // default: 4096
    uint64_t prealloc_page_size;               // default: 2*1024*1024
    uint64_t cache_recycle_threshold_size_mb;  // default: 256
    uint64_t recycle_chunk_num;                // default: 16
    uint64_t fetch_chunk_num;                  // default: 8
} vesal_mem_pool_init_opt_t;

typedef struct _vesal_init_options_t {
    VESAL_BOOL codec_init_qat;   // default: VESAL_TRUE
    VESAL_BOOL cypher_init_qat;  // default: VESAL_TRUE
    vesal_mem_pool_init_opt_t mem_pool_init_opt;
    vesal_flags_t flags;         // see above for default.
} vesal_init_options_t;
// default_vesal_init_options() will give default values to all options:
// - init_qat: VESAL_TRUE
// - flags: see above.
void default_vesal_init_options(vesal_init_options_t* opts);

// Should be called before any other vesal APIs. default_vesal_init_options() should be used to
// initialize init_opts. Also C API doesn't support gflags parsing from command line.
VESAL_BOOL vesal_init(vesal_init_options_t* init_opts);

// Should be called after all other vesal APIs.
void vesal_uninit(void);

typedef struct _vesal_memory_info_t {
    // required
    void* virtual_addr;
    // optional
    // physical_addr is an array, and size is len/page_size.
    // If phys_addr is provided with nullptr, memory pool will try to deduct internally.
    // Note: physical_addr is ignored, will deduct internally for now.
    uint64_t* physical_addr;
    // required
    // memory size, and len is a multiple of page_size
    uint64_t len;
    // required
    // page_size in bytes
    uint64_t page_size;
} vesal_memory_info_t;

/**
 * Register a list of memory info provided by the user. It is user's reponsibility to ensure the
 * memory is allocated and DMA-able during the use, as well as the info is correct. The memory
 * pool only records address info and its length.
 * For the sake of simplicity, the memory is required to be 1GB/2MB-page-aligned.
 * NOTE: The memory from mmap(addr=nullptr) naturally meets this requirement.
 *
 * @return: true - if success, false otherwise.
 *
 * @thread-safe: yes
 */

VESAL_BOOL vesal_register(const vesal_memory_info_t* infos, const int n);

void* vesal_allocate(size_t size);

void vesal_free(void* ptr);

typedef void* VesalCodecChannelHandle;

// For performance reason, expose vesal::CodecResult as vesal_codec_result_t to avoid copying.
// They have the same layout.
typedef struct _vesal_codec_result_t {
    unsigned int consumed;
    unsigned int produced;
    uint64_t in_checksum;
    uint64_t out_checksum;
    VESAL_ERROR_CODE status;
    void* ctx;
} vesal_codec_result_t;

typedef enum {
    VESAL_CODEC_ENGINE_TYPE_SOFTWARE = 1,
    VESAL_CODEC_ENGINE_TYPE_QAT,  // default
    VESAL_CODEC_ENGINE_TYPE_NUM
} VESAL_CODEC_ENGINE_TYPE;

typedef enum {
    VESAL_CODEC_CHECKSUM_TYPE_NONE = 1,  // default
    VESAL_CODEC_CHECKSUM_TYPE_CRC32,
    VESAL_CODEC_CHECKSUM_TYPE_XXHASH32,
    VESAL_CODEC_CHECKSUM_TYPE_NUM
} VESAL_CODEC_CHECKSUM_TYPE;

typedef void (*vesal_codec_user_callback_t)(const vesal_codec_result_t* res);

typedef enum {
    VESAL_CODEC_ALGORITHM_LZ4 = 1,  // default
    VESAL_CODEC_ALGORITHM_ZSTD,
    VESAL_CODEC_ALGORITHM_DEFLATE,
    VESAL_CODEC_ALGORITHM_ZLIB,
    VESAL_CODEC_ALGORITHM_NUM
} VESAL_CODEC_ALGORITHM;

typedef enum {
    VESAL_CHANNEL_MODE_DEDICATED = 1,  // default
    VESAL_CHANNEL_MODE_SHARED,
    VESAL_CHANNEL_MODE_NUM
} VESAL_CHANNEL_MODE;

typedef enum {
    VESAL_HA_POLICY_HARDWARE = 1,
    VESAL_HA_POLICY_SOFTWARE,
    VESAL_HA_POLICY_NONE,  // default
    VESAL_HA_POLICY_NUM
} VESAL_HA_POLICY;

typedef int VESAL_CODEC_COMP_LEVEL;
#define VESAL_CODEC_COMP_LEVEL_FASTEST 1
#define VESAL_CODEC_COMP_LEVEL_DEFAULT 4
#define VESAL_CODEC_COMP_LEVEL_BEST 12

typedef struct _vesal_codec_channel_options_t {
    vesal_codec_user_callback_t user_cb;      // default: NULL
    VESAL_CHANNEL_MODE mode;                  // default: VESAL_CHANNEL_MODE_DEDICATED
    VESAL_HA_POLICY ha_policy;                // default: VESAL_HA_POLICY_NONE
    VESAL_CODEC_ENGINE_TYPE engine_type;      // default: VESAL_CODEC_ENGINE_TYPE_QAT
    VESAL_CODEC_ALGORITHM comp_algorithm;     // default: VESAL_CODEC_ALGORITHM_LZ4
    VESAL_CODEC_COMP_LEVEL comp_level;        // default: VESAL_CODEC_COMP_LEVEL_DEFAULT
    VESAL_CODEC_CHECKSUM_TYPE checksum_type;  // default: VESAL_CODEC_CHECKSUM_TYPE_NONE
    VESAL_BOOL compressed_checksum;           // default: VESAL_TRUE
    int node_affinity;                        // default: -1
    int timeout_ms;                           // default: 3000
} vesal_codec_channel_options_t;
// default_vesal_codec_channel_options() will give default values to all options:
void default_vesal_codec_channel_options(vesal_codec_channel_options_t* opts);

VESAL_ERROR_CODE vesal_create_codec_channel(vesal_codec_channel_options_t* opts,
                                            VesalCodecChannelHandle* handle);

void vesal_destroy_codec_channel(VesalCodecChannelHandle handle);

vesal_codec_result_t vesal_codec_compress(VesalCodecChannelHandle handle,
                                          unsigned char* src,
                                          unsigned int src_len,
                                          unsigned char* dst,
                                          unsigned int dst_len);

vesal_codec_result_t vesal_codec_decompress(VesalCodecChannelHandle handle,
                                            unsigned char* src,
                                            unsigned int src_len,
                                            unsigned char* dst,
                                            unsigned int dst_len);

VESAL_ERROR_CODE vesal_codec_compress_async(VesalCodecChannelHandle handle,
                                            unsigned char* src,
                                            unsigned int src_len,
                                            unsigned char* dst,
                                            unsigned int dst_len,
                                            void* ctx);

VESAL_ERROR_CODE vesal_codec_compress_sgl_async(VesalCodecChannelHandle handle,
                                                unsigned char* src[],
                                                unsigned int src_len[],
                                                unsigned int src_num,
                                                unsigned char* dst,
                                                unsigned int dst_len,
                                                void* ctx);

VESAL_ERROR_CODE vesal_codec_decompress_async(VesalCodecChannelHandle handle,
                                              unsigned char* src,
                                              unsigned int src_len,
                                              unsigned char* dst,
                                              unsigned int dst_len,
                                              void* ctx);

VESAL_ERROR_CODE vesal_codec_decompress_sgl_async(VesalCodecChannelHandle handle,
                                                  unsigned char* src[],
                                                  unsigned int src_len[],
                                                  unsigned int src_num,
                                                  unsigned char* dst,
                                                  unsigned int dst_len,
                                                  void* ctx);

ssize_t vesal_codec_poll(VesalCodecChannelHandle handle,
                         vesal_codec_result_t result[],
                         unsigned int result_num,
                         int timeout);

// Cypher api below
typedef enum {
    VESAL_CYPHER_ALGORITHM_AES_XTS = 1,
    VESAL_CYPHER_ALGORITHM_SHA256,
    VESAL_CYPHER_ALGORITHM_NUM
} VESAL_CYPHER_ALGORITHM;

typedef enum {
    VESAL_CYPHER_ENGINE_SOFTWARE = 1,
    VESAL_CYPHER_ENGINE_QAT,
    VESAL_CYPHER_ENGINE_NUM
} VESAL_CYPHER_ENGINE_TYPE;

typedef enum {
    VESAL_CYPHER_OP_ENCRYPT = 1,
    VESAL_CYPHER_OP_DECRYPT,
    VESAL_CYPHER_OP_HASH
} VESAL_CYPHER_OP;

typedef struct _vesal_cypher_session_option_t {
    VESAL_CYPHER_ALGORITHM algorithm;
    const char* aes_xts_key;  // two aes keys concatenated together
    unsigned int key_len;     // key length in bytes, can only be 16 or 32 or 0(when algo is sha256)
} vesal_cypher_session_option_t;

typedef struct _vesal_cypher_channel_option_t {
    VESAL_CYPHER_ENGINE_TYPE engine_type;
    VESAL_HA_POLICY ha_policy;
    int timeout_ms;
    void* user_cb;
    vesal_cypher_session_option_t session_option;
} vesal_cypher_channel_option_t;

typedef struct _vesal_cypher_result_t {
    VESAL_ERROR_CODE status;
    void* ctx;
} vesal_cypher_result_t;

typedef struct _vesal_cypher_req_args_t {
    VESAL_CYPHER_OP op;
    void* ctx;
    void* session;
    unsigned char* aes_xts_tweak;
} vesal_cypher_req_args_t;

typedef void* VesalCypherChannelHandle;

void default_vesal_cypher_channel_options(vesal_cypher_channel_option_t* opts);

VESAL_ERROR_CODE vesal_create_cypher_channel(vesal_cypher_channel_option_t* opts,
                                             VesalCypherChannelHandle* handle);
void vesal_destroy_cypher_channel(VesalCypherChannelHandle handle);
void* vesal_cypher_add_session(VesalCypherChannelHandle handle,
                               vesal_cypher_session_option_t* opts);
void vesal_cypher_remove_session(VesalCypherChannelHandle handle, void* session);
VESAL_ERROR_CODE vesal_cypher_submit(VesalCypherChannelHandle handle,
                                     unsigned char* src,
                                     unsigned int src_len,
                                     unsigned char* dst,
                                     unsigned int dst_len,
                                     vesal_cypher_req_args_t* req);

VESAL_ERROR_CODE vesal_cypher_submit_sgl(VesalCypherChannelHandle handle,
                                         unsigned char* src[],
                                         unsigned int src_len[],
                                         unsigned int src_num,
                                         unsigned char* dst,
                                         unsigned int dst_len,
                                         vesal_cypher_req_args_t* req);
ssize_t vesal_cypher_poll(VesalCypherChannelHandle handle,
                          vesal_cypher_result_t result[],
                          unsigned int result_num,
                          int timeout);

#ifdef __cplusplus
}
#endif
