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

#include <stdio.h>
#include <stdlib.h>

#include "vesal/c_api/c_api_vesal.h"

#define DEMO_SRC_SIZE (16 * 1024)
#define DEMO_DST_SIZE DEMO_SRC_SIZE

int main() {
    printf("Test vesal C APIs\n");
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    init_opts.codec_init_qat = VESAL_FALSE;
    VESAL_BOOL ret = vesal_init(&init_opts);
    if (!ret) {
        printf("vesal_init failed, ret: %d\n", ret);
        return -1;
    }
    printf("vesal_init success\n");

    vesal_cypher_channel_option_t channel_opt;
    default_vesal_cypher_channel_options(&channel_opt);
    channel_opt.session_option.aes_xts_key =
        "01234567890123450123456789012345012345678901234501234567890abcde";
    channel_opt.session_option.key_len = 32;
    channel_opt.session_option.algorithm = VESAL_CYPHER_ALGORITHM_AES_XTS;
    VesalCypherChannelHandle handle;
    VESAL_ERROR_CODE create_r = vesal_create_cypher_channel(&channel_opt, &handle);
    if (create_r != VESAL_OK) {
        printf("vesal_create_cypher_channel failed, ret: %d\n", create_r);
        return -1;
    }
    printf("vesal_create_cypher_channel success\n");

    // Basic encryption and decryption demo
    printf("Start basic encryption and decryption demo\n");
    unsigned char* tweak = vesal_allocate(16);
    const int n = 256;
    unsigned char* src[n];
    unsigned char* encrypted_data[n];
    unsigned char* decrypted_data[n];
    for (int i = 0; i < n; i++) {
        src[i] = vesal_allocate(DEMO_SRC_SIZE);
        encrypted_data[i] = vesal_allocate(DEMO_DST_SIZE);
        decrypted_data[i] = vesal_allocate(DEMO_DST_SIZE);
    }
    for (int i = 0; i < n; i++) {
        vesal_cypher_req_args_t args;
        args.aes_xts_tweak = tweak;
        args.ctx = NULL;
        args.op = VESAL_CYPHER_OP_ENCRYPT;
        args.session = NULL;  // Set to NULL explicitly
        VESAL_ERROR_CODE r = vesal_cypher_submit(
            handle, src[i], DEMO_SRC_SIZE, encrypted_data[i], DEMO_DST_SIZE, &args);
        if (r != VESAL_OK) {
            printf("Send failed, ret: %d\n", r);
            exit(-1);
        }
    }
    printf("Send encryption requests success\n");

    size_t polled_num = 0;
    vesal_cypher_result_t result[n];
    while (polled_num < n) {
        ssize_t m = vesal_cypher_poll(handle, result + polled_num, n, -1);
        if (m < 0) {
            printf("Poll failed");
            exit(-1);
        }
        polled_num += m;
    }
    for (int i = 0; i < n; i++) {
        if (result[i].status != VESAL_OK) {
            printf("Encrypt failed, ret: %d\n", result[i].status);
            exit(-1);
        }
    }
    printf("Poll encryption requests success\n");

    for (int i = 0; i < n; i++) {
        vesal_cypher_req_args_t args;
        args.aes_xts_tweak = tweak;
        args.ctx = NULL;
        args.op = VESAL_CYPHER_OP_DECRYPT;
        args.session = NULL;  // Set to NULL explicitly
        VESAL_ERROR_CODE r = vesal_cypher_submit(
            handle, encrypted_data[i], DEMO_DST_SIZE, decrypted_data[i], DEMO_DST_SIZE, &args);
        if (r != VESAL_OK) {
            printf("Send failed, ret: %d\n", r);
            exit(-1);
        }
    }
    printf("Send decryption requests success\n");

    polled_num = 0;
    while (polled_num < n) {
        ssize_t m = vesal_cypher_poll(handle, result + polled_num, n, -1);
        if (m < 0) {
            printf("Poll failed");
            exit(-1);
        }
        polled_num += m;
    }
    printf("Poll decryption requests success\n");
    for (int i = 0; i < n; i++) {
        if (result[i].status != VESAL_OK) {
            printf("Decrypt failed, ret: %d\n", result[i].status);
            exit(-1);
        }
        for (int j = 0; j < DEMO_SRC_SIZE; j++) {
            if (src[i][j] != decrypted_data[i][j]) {
                printf("Decrypt failed, src[%d][%d] != decrypted_data[%d][%d]\n", i, j, i, j);
                exit(-1);
            }
        }
    }
    printf("Validate decryption results success, end basic demo\n\n");

    // Multi session and inplace decryption demo
    printf("Start multi session and inplace decryption demo\n");
    // Create a new session to add different keys
    vesal_cypher_session_option_t session_option;
    session_option.algorithm = VESAL_CYPHER_ALGORITHM_AES_XTS;
    session_option.aes_xts_key = "012345678901234501234567890)(*&^01234567890123450123456789009876";
    session_option.key_len = 32;
    void* session = vesal_cypher_add_session(handle, &session_option);
    if (!session) {
        printf("Failed to create new session");
        exit(-1);
    }
    printf("Create new session success");

    for (int i = 0; i < n; i++) {
        vesal_cypher_req_args_t args;
        args.aes_xts_tweak = tweak;
        args.ctx = NULL;
        args.op = VESAL_CYPHER_OP_ENCRYPT;
        args.session = session;  // Set to newly created session
        VESAL_ERROR_CODE r = vesal_cypher_submit(
            handle, src[i], DEMO_SRC_SIZE, encrypted_data[i], DEMO_DST_SIZE, &args);
        if (r != VESAL_OK) {
            printf("Send failed, ret: %d\n", r);
            exit(-1);
        }
    }
    printf("Send encryption requests success\n");

    polled_num = 0;
    while (polled_num < n) {
        ssize_t m = vesal_cypher_poll(handle, result + polled_num, n, -1);
        if (m < 0) {
            printf("Poll failed");
            exit(-1);
        }
        polled_num += m;
    }
    for (int i = 0; i < n; i++) {
        if (result[i].status != VESAL_OK) {
            printf("Encrypt failed, ret: %d\n", result[i].status);
            exit(-1);
        }
    }
    printf("Poll encryption requests success\n");

    for (int i = 0; i < n; i++) {
        vesal_cypher_req_args_t args;
        args.aes_xts_tweak = tweak;
        args.ctx = NULL;
        args.op = VESAL_CYPHER_OP_DECRYPT;
        args.session = session;  // Set to newly created session
        // Perform inplace decryption
        VESAL_ERROR_CODE r = vesal_cypher_submit(
            handle, encrypted_data[i], DEMO_DST_SIZE, encrypted_data[i], DEMO_DST_SIZE, &args);
        if (r != VESAL_OK) {
            printf("Send failed, ret: %d\n", r);
            exit(-1);
        }
    }
    printf("Send decryption requests success\n");

    polled_num = 0;
    while (polled_num < n) {
        ssize_t m = vesal_cypher_poll(handle, result + polled_num, n, -1);
        if (m < 0) {
            printf("Poll failed");
            exit(-1);
        }
        polled_num += m;
    }
    printf("Poll decryption requests success\n");
    for (int i = 0; i < n; i++) {
        if (result[i].status != VESAL_OK) {
            printf("Decrypt failed, ret: %d\n", result[i].status);
            exit(-1);
        }
        for (int j = 0; j < DEMO_SRC_SIZE; j++) {
            if (src[i][j] != encrypted_data[i][j]) {
                printf("Decrypt failed, src[%d][%d] != encrypted_data[%d][%d]\n", i, j, i, j);
                exit(-1);
            }
        }
    }
    printf(
        "Validate decryption results success, end multi session and inplace decryption demo\n\n");

    vesal_cypher_remove_session(handle, session);
    vesal_free(tweak);
    for (int i = 0; i < n; i++) {
        vesal_free(src[i]);
        vesal_free(encrypted_data[i]);
        vesal_free(decrypted_data[i]);
    }
    vesal_destroy_cypher_channel(handle);
    vesal_uninit();
    printf("Test vesal cypher C APIs success\n");
}