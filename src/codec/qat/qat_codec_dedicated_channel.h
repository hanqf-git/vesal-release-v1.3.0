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

#include "codec/qat/qat_codec_engine.h"
#include "common/qat/qat_unit_manager.h"
#include "vesal/codec.h"

namespace vesal {
namespace qat {

// Currently we use a dedicated channel to wrap the QatCodecEngine.
// TODO(sjj): Later will simplify the QatCodecEngine.
class QatCodecDedicatedChannel : public CodecChannel {
public:
    QatCodecDedicatedChannel(const CodecChannelOption& channel_opts,
                             QatUnitManager* unit_manager,
                             size_t max_in_qat_size)
        : qat_codec_engine_(
              std::make_unique<QatCodecEngine>(channel_opts, unit_manager, max_in_qat_size)),
          channel_opts_(channel_opts){};

    ~QatCodecDedicatedChannel() {
        qat_codec_engine_.reset();
    }

    Status Init();

    StatusCode CompressAsync(unsigned char* src,
                             unsigned int src_len,
                             unsigned char* dst,
                             unsigned int dst_len,
                             void* ctx) override;

    StatusCode CompressSGLAsync(const std::vector<unsigned char*>& src,
                                const std::vector<unsigned int>& src_len,
                                unsigned char* dst,
                                unsigned int dst_len,
                                void* ctx) override;

    CodecResult Compress(unsigned char* src,
                         unsigned int src_len,
                         unsigned char* dst,
                         unsigned int dst_len) override;

    CodecResult CompressSGL(const std::vector<unsigned char*>& src,
                            const std::vector<unsigned int>& src_len,
                            unsigned char* dst,
                            unsigned int dst_len) override;

    StatusCode DecompressAsync(unsigned char* src,
                               unsigned int src_len,
                               unsigned char* dst,
                               unsigned int dst_len,
                               void* ctx) override;

    StatusCode DecompressSGLAsync(const std::vector<unsigned char*>& src,
                                  const std::vector<unsigned int>& src_len,
                                  unsigned char* dst,
                                  unsigned int dst_len,
                                  void* ctx) override;

    CodecResult Decompress(unsigned char* src,
                           unsigned int src_len,
                           unsigned char* dst,
                           unsigned int dst_len) override;

    CodecResult DecompressSGL(const std::vector<unsigned char*>& src,
                              const std::vector<unsigned int>& src_len,
                              unsigned char* dst,
                              unsigned int dst_len) override;

    StatusCode SubmitAsync(const CodecRequestArgs& args) override;

    CodecResult Submit(const CodecRequestArgs& args) override;

    ssize_t Poll(CodecResult results[], unsigned int max_num, int timeout) override;

    StatusCode DumpDebugInfo() override;

    Status Close() override;

private:
    bool Prepare(const std::vector<unsigned char*>& src,
                 const std::vector<unsigned int>& src_len,
                 unsigned char* dst,
                 unsigned int dst_len) {
        return IsOk(ValidateSglArgs(src, src_len));
    }

    std::unique_ptr<QatCodecEngine> qat_codec_engine_;
    CodecChannelOption channel_opts_;

    std::shared_ptr<Histogram> metric_user_cb_time_;  // the time cost of user callback function in each
                                               // Poll() call. Might contain multiple calls.
};

}  // namespace qat
}  // namespace vesal
