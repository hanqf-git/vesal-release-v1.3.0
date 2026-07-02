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
#define DEMO_DST_SIZE (DEMO_SRC_SIZE << 2)

typedef struct _req_data_t {
    unsigned char src[DEMO_SRC_SIZE];
    unsigned int src_len;
    unsigned char dst[DEMO_DST_SIZE];
    unsigned int dst_len;
    void* ctx;
} req_data_t;

void SendPoll(VesalCodecChannelHandle handle, int is_compress, size_t num) {
    printf("Try send poll %zd requests\n", num);
    const size_t batch = 16;
    vesal_codec_result_t result[batch];
    while (num > 0) {
        size_t send_num = num > batch ? batch : num;
        for (size_t i = 0; i < send_num; i++) {
            VESAL_ERROR_CODE send_r;
            req_data_t* req = (req_data_t*)malloc(sizeof(req_data_t));
            req->src_len = DEMO_SRC_SIZE;
            req->dst_len = DEMO_DST_SIZE;
            req->ctx = req;
            if (is_compress) {
                send_r = vesal_codec_compress_async(
                    handle, req->src, req->src_len, req->dst, req->dst_len, req);
            } else {
                send_r = vesal_codec_decompress_async(
                    handle, req->src, req->src_len, req->dst, req->dst_len, req);
            }
            if (send_r != VESAL_OK) {
                printf("Send failed, ret: %d\n", send_r);
                exit(-1);
            }
        }
        size_t polled_num = 0;
        while (polled_num < send_num) {
            ssize_t poll_num = vesal_codec_poll(handle, result, batch, -1);
            if (poll_num < 0) {
                printf("Poll failed, ret: %zd\n", poll_num);
                exit(-1);
            }
            for (size_t i = 0; i < poll_num; i++) {
                if (result[i].status != VESAL_OK) {
                    printf("Poll result failed, ret: %d\n", result[i].status);
                    exit(-1);
                }
                polled_num++;
            }
        }
        num -= send_num;
    }
    printf("Send poll all requests success\n");
}

void DemoCb(const vesal_codec_result_t* res) {
    if (res->status != VESAL_OK) {
        printf("DemoCb failed, ret: %d\n", res->status);
        exit(-1);
    }
    req_data_t* req = (req_data_t*)res->ctx;
    free(req);
}
int main() {
    printf("Test vesal C APIs\n");
    vesal_init_options_t init_opts;
    default_vesal_init_options(&init_opts);
    VESAL_BOOL ret = vesal_init(&init_opts);
    if (!ret) {
        printf("vesal_init failed, ret: %d\n", ret);
        return -1;
    }
    printf("vesal_init success\n");
    vesal_codec_channel_options_t chnnl_opts;
    default_vesal_codec_channel_options(&chnnl_opts);
    chnnl_opts.user_cb = DemoCb;
    VesalCodecChannelHandle handle;
    VESAL_ERROR_CODE create_r = vesal_create_codec_channel(&chnnl_opts, &handle);
    if (create_r != VESAL_OK) {
        printf("vesal_create_codec_channel failed, ret: %d\n", create_r);
        return -1;
    }
    SendPoll(handle, 1, 1024);
    vesal_destroy_codec_channel(handle);
    vesal_uninit();
    printf("Test vesal C APIs success\n");
    return 0;
}
