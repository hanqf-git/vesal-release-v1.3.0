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

#include <memory>
#include <utility>
#include <vector>

#include "vesal/io_vector.h"
#include "vesal/status.h"
#include "vesal/types.h"
#include "vesal/vesal_version.h"

namespace vesal {

struct CodecResult {
    unsigned int consumed{0};
    unsigned int produced{0};
    // Checksum for the in data.
    checksum64_t in_checksum{0};
    // Checksum for the out data.
    checksum64_t out_checksum{0};
    StatusCode status{StatusCode::kUnknown};
    void* ctx{nullptr};
};

std::ostream& operator<<(std::ostream& os, const CodecResult& res);

enum class CodecEngineType : uint8_t { kSoftware = 1, kQat, kKae, kNum };

// HaPolicy is a channel-wise option. kHardware means the channel will try to fallback to a new
// hardware if possible when it thinks it's neccessary. kSoftware means the vesal will try to
// fallback to use software based compression(e.g background thread pool) when a channel is failed.
// kNone means no HA will be triggered.
enum class HaPolicy : uint8_t { kHardware = 1, kSoftware, kNone, kNum };

struct CodecInitOptions {
    bool init_qat{true};  // if not, will only support software compress/decompress
    bool init_kae{false};
};

enum class CodecAlgorithm : uint8_t { kLz4 = 1, kZstd, kDeflate, kZlib, kNum };

// Greater level means smaller size of compressed result and slower compression speed.
// For lz4 software compression, vesal uses lz4frame, and CodecCompLevel is mapping to lz4frame
// level as: kLevel1 => -10 (the fastest), kLevel2 => -8, kLevel3 => -6, kLevel4 => -4, kLevel5 =>
// -2, kLevel6 => 0 (lz4frame's default),
// ...
// kLevel11 => 10,
// kLevel12 => 12 (lz4frame's max, i.e. LZ4HC_CLEVEL_MAX, the best compression ratio)

enum class CodecCompLevel : uint8_t {
    kLevel1 = 1,
    kLevel2,
    kLevel3,
    kLevel4,
    kLevel5,
    kLevel6,
    kLevel7,
    kLevel8,
    kLevel9,
    kLevel10,
    kLevel11,
    kLevel12,
    kNum
};

// kCrc32 follows:
// polynomial 0x1EDC6F41,
// init 0xFFFFFFFF,
// refin true,
// refout true,
// xorout 0
// This set is very similar to so-called CRC32-C algorithm except here xorout is 0, where the
// CRC32-C uses (uint64_t)(0xFFFFFFFF)<<32
enum class CodecChecksumType : uint8_t { kNone = 1, kCrc32, kNum };

enum class ChannelMode : uint8_t { kDedicated = 1, kShared, kNum };

struct ChannelAllocationOption {
    int node_affinity{-1};  // if >= 0, will try to allocate hardware resources with the specified
                            // node; random if == -1
    int32_t device_id{-1};  // if >= 0, will try to allocate hardware resources with the specified
                            // device; less busy first if == -1; Device id can be obtained with
                            // GetDeviceInfos() in vesal.h

    ChannelAllocationOption() : node_affinity(-1), device_id(-1) {}
};

/**
 * @brief Per-request session option used by Submit/SubmitAsync (CodecRequestArgs).
 *
 * This option is required for Submit/SubmitAsync for channel creation and per-request algorithm
 * selection
 *
 * Current support scope:
 * - QAT (dedicated mode): codec_algorithm supports kLz4 / kDeflate / kZlib. comp_level is used for
 *   compression only and has no effect in decompression.
 * - Software engine: Submit/SubmitAsync is not supported yet, so this option is currently ignored
 *   on the software path.
 *
 * If you do not need per-request algorithm selection, set session_opt fields to match the
 * channel's CodecChannelOption (comp_algorithm / comp_level).
 */
struct CodecSessionOption {
    enum CodecAlgorithm codec_algorithm { CodecAlgorithm::kNum };
    enum CodecCompLevel comp_level { CodecCompLevel::kNum };  // has no effect in decompression
};

using UserCallback = void (*)(const CodecResult& res);
struct CodecChannelOption {
    // Note: 'user_cb' is ONLY effective in Async APIs like CompressAsync. Will be ignored for sync
    // APIs like Compress.
    UserCallback user_cb{nullptr};
    enum ChannelMode mode { ChannelMode::kDedicated };
    enum HaPolicy ha_policy {
        HaPolicy::kNone
    };  // ha policy for this channel. Software-based is not implemented yet.
    enum CodecEngineType engine_type { CodecEngineType::kQat };
    enum CodecAlgorithm comp_algorithm { CodecAlgorithm::kLz4 };
    enum CodecCompLevel comp_level { CodecCompLevel::kLevel4 };  // has no effect in decompression
    enum CodecChecksumType checksum_type { CodecChecksumType::kNone };
    bool compressed_checksum{
        true};  // If true, checksum for the compressed data(output side for compression and input
                // side for decompression) is also calculated.

    ChannelAllocationOption allocation_option;
    bool sw_backup{false};      // if true, fallback to the software path when the result failed
    uint64_t timeout_ms{3000};  // timeout for per compress or decompress request

    friend std::ostream& operator<<(std::ostream& os, const CodecChannelOption& opt);

    bool operator<(const CodecChannelOption& rhs) const;
};

enum class CodecDirection : uint8_t { kComp = 0, kDecomp, kNum };

struct CodecRequestArgs {
    IOVector src;
    IOBlock dst;
    void* ctx;
    CodecSessionOption session_opt;
    enum CodecDirection direction { CodecDirection::kComp };
};

class CodecChannel {
public:
    CodecChannel() = default;
    virtual ~CodecChannel() = default;

    /**
     * @brief Create the channel based on options.
     * TODO(sjj): support dedicated mode's user_cb. Currently has no effect.
     * If opts.mode=ChannelMode::kDedicated, Channels are thread-exclusive. Sharing one channel
     * between threads might cause undefined behaviours, but it's OK to have one thread holding
     * multiple CodecChannel. Typical usage is one thread holding one channel, submit one or
     * multiple requests to the channel by CompressAsync/DecompressAsync series APIs, do something
     * else then call Poll() to get the result, this also triggers the callback. We gurantee the
     * callback 'user_cb' set in the option will be triggered in the same order as the requests are
     * submitted.
     *
     * If opts.mode=ChannelMode::kShared, Channels are thread-safe. Sharing channels between threads
     * is safe, and channel's CompressAsync/DecompressAsync series APIs are thread-safe, too.
     * Typical usage is have one or several channels per thread, and submit one or multiple requests
     * to the channel by CompressAsync/DecompressAsync series APIs, do something else. After the
     * requests are submitted, callback  'user_cb' set in the option will be triggered in the same
     * order as the requests are submitted. Note in this mode, user shall not call Poll().
     *
     * @param opts User specified options.
     *
     * @return OK if success, with CodecChannel created. Otherwise:
     * - RESOURCE_BUSY Not enough hardware resourses to create a new channel
     * - INVALID_ARGUMENT Wrong opts given
     * - NOT_SUPPORTED Engine is not initialized or not available
     * - UNKNOWN Internal system error
     */
    static std::pair<Status, std::unique_ptr<CodecChannel>> CreateCodecChannel(
        const CodecChannelOption& opts);

    /**
     * @brief Compress a source buffer asynchronously. The function returns
     * immediately if the compress request sent successfully. User needs to ensure that
     * dst_len is long enough to hold the result.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use Submit or
     * SubmitAsync with CodecRequestArgs (IOVector/IOBlock) instead.
     * @note PERFORMANCE: This API has about 1% performance degradation compared to the new
     * IOVector-based APIs, as it requires converting std::vector to IOVector internally.
     *
     * @param src The source data buffer
     * @param src_len The length of src
     * @param dst The user-allocated buffer to store the compressed data. User
     * must ensure the length is enough long to store the compressed data
     * @param dst_len The length of dst
     * @param ctx User's context data. It's guaranteed the data will not be
     * touched during the request.
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */
    virtual StatusCode CompressAsync(unsigned char* src,
                                     unsigned int src_len,
                                     unsigned char* dst,
                                     unsigned int dst_len,
                                     void* ctx) = 0;

    /**
     * @brief SGL(Scatter-Gather-List) version API to compress multiple source
     * data buffers in one call. Scattered buffers are treated contiguous
     * logically, for both src and dst. The API has the same behaviours like
     * CompressAsync.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use Submit or
     * SubmitAsync with CodecRequestArgs (IOVector/IOBlock) instead.
     * @note PERFORMANCE: This API has about 1% performance degradation compared to the new
     * IOVector-based APIs, as it requires converting std::vector to IOVector internally.
     *
     * @param src The source data buffers. Maximum summary 512KB
     * @param src_len The lengths of each src
     * @param dst The user-allocated buffer to store the compressed data.
     * @param dst_len The lengths of dst. User must ensure dst is enough to store the compressed
     * concatenate data
     * @param ctx User's context data. It's guaranteed the data will not be
     * touched during the request.
     * @note the size of `src` and `src_len` must be equal, and the cannot exceed 32.
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */
    virtual StatusCode CompressSGLAsync(const std::vector<unsigned char*>& src,
                                        const std::vector<unsigned int>& src_len,
                                        unsigned char* dst,
                                        unsigned int dst_len,
                                        void* ctx) = 0;

    /**
     * @brief Synchronous API to compress. Blocking until the result returned.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use Submit with
     * CodecRequestArgs (IOVector/IOBlock) instead.
     *
     * @param src The source data buffer. Maximum 512KB
     * @param src_len The length of each src
     * @param dst The The user-allocated buffer to store the compressed data.
     * @param dst_len The length of dst. User must ensure dst is enough to store the compressed
     * concatenate data
     *
     * @return CodecResult with StatusCode member: OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */
    virtual CodecResult Compress(unsigned char* src,
                                 unsigned int src_len,
                                 unsigned char* dst,
                                 unsigned int dst_len) = 0;

    /**
     * @brief SGL(Scatter-Gather-List) synchronous API to compress. Blocking until
     * the result returned.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use Submit with
     * CodecRequestArgs (IOVector/IOBlock) instead.
     *
     * @param src The source data buffers. Maximum 512KB
     * @param src_len The lengths of each src
     * @param dst The The user-allocated buffer to store the compressed data.
     * @param dst_len The length of dst. User must ensure dst is long enough to store the compressed
     * concatenate data
     * @note the size of `src` and `src_len` must be equal, and the cannot exceed 32.
     *
     * @return CodecResult with StatusCode member: OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */
    virtual CodecResult CompressSGL(const std::vector<unsigned char*>& src,
                                    const std::vector<unsigned int>& src_len,
                                    unsigned char* dst,
                                    unsigned int dst_len) = 0;

    /**
     * @brief Decompress a source buffer asynchronously. The function returns
     * immediately if the compress request sent successfully. It is required that
     * dst_len is long enough to hold the result.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use Submit or
     * SubmitAsync with CodecRequestArgs (IOVector/IOBlock) instead.
     * @note PERFORMANCE: This API has about 1% performance degradation compared to the new
     * IOVector-based APIs, as it requires converting std::vector to IOVector internally.
     *
     * @param src The source data buffer
     * @param src_len The length of src. Maximum size 512KB
     * @param dst The The user-allocated buffer to store the decompressed data. User
     * must ensure the length is long enough to store the decompressed data
     * @param dst_len The length of dst
     * @param ctx User's context data. It's guaranteed the data will not be
     * touched
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */
    virtual StatusCode DecompressAsync(unsigned char* src,
                                       unsigned int src_len,
                                       unsigned char* dst,
                                       unsigned int dst_len,
                                       void* ctx) = 0;

    /**
     * @brief SGL(Scatter-Gather-List) version API to decompress multiple source
     * data buffers in one call. Scattered buffers are treated contiguous
     * logically, for both src and dst. The API has the same behaviours like
     * DecompressAsync.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use Submit or
     * SubmitAsync with CodecRequestArgs (IOVector/IOBlock) instead.
     * @note PERFORMANCE: This API has about 1% performance degradation compared to the new
     * IOVector-based APIs, as it requires converting std::vector to IOVector internally.
     *
     * @param src The source data buffers. Maximum summary 512KB
     * @param src_len The lengths of each src
     * @param dst The The user-allocated buffer to store the decompressed data.
     * @param dst_len The length of dst. User must ensure dst is enough to store the compressed
     * concatenate data
     * @param ctx User's context data. It's guaranteed the data will not be
     * touched
     * @note the size of `src` and `src_len` must be equal, and the cannot exceed 32.
     *
     * @return OK if success. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */

    virtual StatusCode DecompressSGLAsync(const std::vector<unsigned char*>& src,
                                          const std::vector<unsigned int>& src_len,
                                          unsigned char* dst,
                                          unsigned int dst_len,
                                          void* ctx) = 0;

    /**
     * @brief Synchronous API to decompress. Blocking until the result returned.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use Submit with
     * CodecRequestArgs (IOVector/IOBlock) instead.
     *
     * @param src The source data buffer. Maximum 512KB
     * @param src_len The length of each src
     * @param dst The The user-allocated buffer to store the decompressed data.
     * @param dst_len The length of dst. User must ensure dst is enough to store the decompressed
     * concatenate data
     *
     * @return CodecResult with StatusCode member: OK if success, Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */
    virtual CodecResult Decompress(unsigned char* src,
                                   unsigned int src_len,
                                   unsigned char* dst,
                                   unsigned int dst_len) = 0;

    /**
     * @brief SGL(Scatter-Gather-List) synchronous API to decompress. Blocking until
     * the result returned.
     * @note DEPRECATION: This legacy API will be deprecated in the next release. Use Submit with
     * CodecRequestArgs (IOVector/IOBlock) instead.
     *
     * @param src The source data buffers. Maximum 512KB
     * @param src_len The lengths of each src
     * @param dst The The user-allocated buffer to store the compressed data.
     * @param dst_len The length of dst. User must ensure dst is enough to store the decompressed
     * concatenate data
     * @note the size of `src` and `src_len` must be equal, and the cannot exceed 32.
     *
     * @return OK if success, with result CodecResult. Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */
    virtual CodecResult DecompressSGL(const std::vector<unsigned char*>& src,
                                      const std::vector<unsigned int>& src_len,
                                      unsigned char* dst,
                                      unsigned int dst_len) = 0;

    /**
     * @brief Synchronous API to compress/decompress. Blocking until the result returned.
     *
     * @param args The request arguments.
     *
     * @return CodecResult with StatusCode member: OK if success, Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */
    virtual CodecResult Submit(const CodecRequestArgs& args) = 0;
    /**
     * @brief Asynchronous API to compress/decompress. The function returns
     * immediately if the compress request sent successfully.
     *
     * @param args The request arguments.
     *
     * @return StatusCode with member: OK if success, Otherwise:
     * - RESOURCE_BUSY Too many requests, retry later
     * - INVALID_ARGUMENT Invalid argument such as dst_len < src_len
     * - UNKNOWN Internal system error
     * - CHANNEL_ERROR Channel error, recomend to create a new channel.
     * - PERMANENT_ERROR Channel completely failed, this channel should not be used anymore. If the
     * HA is enabled, it means the HA failed, consider the hardware is failed. Otherwise consider it
     * as CHANNEL_ERROR.
     */
    virtual StatusCode SubmitAsync(const CodecRequestArgs& args) = 0;
    /**
     * @brief Poll the results in the current channel.
     *
     * @note It gurantees that the order of callback executed matches the order of
     * submission.
     *
     * @param results CodecResult array to take polled results with member StatusCode: Ok, if
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
     * @return number of actual results got in this call. -1 if this call failed. -1 usually means
     * the channel is in serious error, consider create a new channel.
     */

    virtual ssize_t Poll(CodecResult results[], unsigned int max_num, int timeout) = 0;

    virtual StatusCode DumpDebugInfo() {
        return StatusCode::kNotSupported;
    }

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
