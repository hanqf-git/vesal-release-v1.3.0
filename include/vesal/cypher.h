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

#include <memory>
#include <utility>

#include "vesal/codec.h"
#include "vesal/io_vector.h"
#include "vesal/status.h"
#include "vesal/vesal_version.h"

namespace vesal {

struct CypherInitOptions {
    bool init_qat{true};  // if not, will only support software cypher
};

enum class EngineType : uint8_t { kSoftware = 1, kQat, kNum };
enum class CypherAlgorithm : uint8_t { kAES_XTS = 1, kSHA256, kNum };

struct CypherResult {
    StatusCode status;  // OK if success
    void* ctx;
    // Checksum out_checksum{0}; // Not ready now
};

struct CypherSessionOption {
    enum CypherAlgorithm algorithm { CypherAlgorithm::kAES_XTS };

    struct {
        // AES_XTS algorithm specific
        std::string aes_xts_key1;  // Length of key1/key2 must be either 16 bytes or 32 bytes, both
                                   // keys must have the same size.
        std::string aes_xts_key2;
    } aes_xts_spec;
};

using CypherUserCallback = void (*)(const CypherResult& res);
struct CypherChannelOption {
    EngineType engine{EngineType::kQat};
    ChannelAllocationOption allocation_option;
    CypherUserCallback user_cb{nullptr};
    HaPolicy ha_policy{
        HaPolicy::kNone};  // ha policy for this channel. Software-based is not implemented yet.
    uint64_t timeout_ms{1000};  // timeout for per encrypt or decrypt request
    CypherSessionOption session_option;
};

enum class CypherOp : uint8_t { kEncrypt = 1, kDecrypt, kHash };

struct CypherReqArgs {
    IOVector src;
    IOBlock dst;
    CypherOp op;
    union {
        unsigned char* aes_xts_tweak;  // 128 bit tweak value for AES_XTS
    };
    void* ctx{nullptr};  // User's context data. It's guaranteed the data will not be touched
    void* session{
        nullptr};  // Session pointer to specify which qat crypto/hash session this request belongs
                   // to, using default session(created along with the channel) if set nullptr
};

class CypherChannel {
public:
    CypherChannel() = default;
    virtual ~CypherChannel() = default;
    /**
     * @brief Create the channel based on options. Channels are thread-exclusive. Sharing one
     * channel between threads might cause undefined behaviours. Note: It's OK to have one thread
     * holding multiple CypherChannels.
     *
     * @param opts User specified options.
     *
     * @return OK if success, with CypherChannel created. Otherwise:
     * - RESOURCE_BUSY Not enough hardware resourses to create a new channel
     * - INVALID_ARGUMENT Wrong opts given
     * - NOT_SUPPORTED Engine is not initialized or not available
     * - UNKNOWN Internal system error
     */
    static std::pair<Status, std::unique_ptr<CypherChannel>> CreateCypherChannel(
        const CypherChannelOption& opts);

    /**
     * @brief Only QAT engine viable!!! Create a new qat crypto/hash session utilizing existing
     * channel's hardware resources.
     *
     * @param opts User specified options.
     *
     * @return session pointer if success, otherwise nullptr
     */
    virtual void* AddSession(const CypherSessionOption& option) = 0;

    /**
     * @brief Only QAT engine viable!!! Delete a qat crypto/hash session to release the memory.
     *
     * @param session Session pointer
     */
    virtual void RemoveSession(void* session) = 0;

    /**
     * @brief Submit encrypt/decrypt/hash request asynchronously. The function returns
     * immediately if the request sent successfully. It is required that
     * dst_len is long enough to hold the result.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use
     * SubmitAsync(const CypherReqArgs&) with IOVector/IOBlock instead.
     * @note PERFORMANCE: This API has about 1% performance degradation compared to the new
     * IOVector-based APIs, as it requires converting std::vector to IOVector internally.
     *
     * @param src The source data buffer
     * @param src_len The length of src. Maximum size 512KB
     * @param dst The The user-allocated buffer to store the output data. User
     * must ensure the length is as same as source data.
     * @param dst_len The length of dst
     * @param req request data. It's guaranteed the data will not be
     * touched
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel closed
     */

    virtual StatusCode SubmitCypherReq(unsigned char* src,
                                       unsigned int src_len,
                                       unsigned char* dst,
                                       unsigned int dst_len,
                                       CypherReqArgs* req) = 0;

    /**
     * @brief Submit encrypt/decrypt/hash request asynchronously. The function returns
     * immediately if the request sent successfully. It is required that
     * dst_len is long enough to hold the result.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use
     * SubmitAsync(const CypherReqArgs&) with IOVector/IOBlock instead.
     * @note PERFORMANCE: This API has about 1% performance degradation compared to the new
     * IOVector-based APIs, as it requires converting std::vector to IOVector internally.
     *
     * @param src The source data buffer list
     * @param src_len The length of src. Maximum size 512KB
     * @param dst The The user-allocated buffer to store the output data. User
     * must ensure the length is as same as source data.
     * @param dst_len The length of dst
     * @param req request data. It's guaranteed the data will not be
     * touched
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel closed
     */

    virtual StatusCode SubmitCypherSGLReq(const std::vector<unsigned char*>& src,
                                          const std::vector<unsigned int>& src_len,
                                          unsigned char* dst,
                                          unsigned int dst_len,
                                          CypherReqArgs* req) = 0;

    /**
     * @brief Submit encrypt/decrypt/hash request asynchronously. The function returns
     * immediately if the request sent successfully.
     * @param req request args. It's guaranteed the data will not be
     * touched
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel closed
     */
    virtual StatusCode SubmitAsync(const CypherReqArgs& req) = 0;

    /**
     * @brief Poll the results in the current channel.
     *
     * @note QatCypherChannel gurantees that the order of callback executed matches the order of
     * submission.
     *
     * @param results CypherResult array to take polled results with member StatusCode: Ok, if
     * success. Otherwise:
     * - RESOURCE_BUSY No results ready yet
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel closed
     * @param max_num The maximum number of results can be got in this call. If more than max_num
     * results are available, leave the extra ones in the queue.
     * @param timeout How long blocking in the call
     * timeout = -1 means blocking until at least one result ready
     * timeout = 0 means returning immediately if no results ready
     * timeout > 0 means blocking until at least one result ready or timeouted
     *
     * @return number of actual results got in this call. -1 if this call failed.
     */

    virtual ssize_t Poll(CypherResult results[], unsigned int max_num, int timeout) = 0;

    /**
     * @brief Close the channel. It will try to poll the remaining results if there is any.
     * The api will fail with RESOURCE_BUSY if there are still requests in flight after
     * timeout.
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Still requests in the queue, need to wait and poll them before close
     * - HARDWARE_ERROR QAT hardware error, recommend to fallback to software
     * - UNKNOWN Internal system error
     */
    virtual Status Close() = 0;
};

}  // namespace vesal
