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

#include <memory>

#include "vesal/cypher.h"
#include "vesal/status.h"

namespace vesal {

class SwCypherChannel : public CypherChannel {
public:
    SwCypherChannel(const CypherChannelOption& channel_opts);
    ~SwCypherChannel() override;

    Status Init();

    Status Close() override;

    void* AddSession(const CypherSessionOption& option) override;

    void RemoveSession(void* session) override;

    StatusCode SubmitCypherReq(unsigned char* src,
                               unsigned int src_len,
                               unsigned char* dst,
                               unsigned int dst_len,
                               CypherReqArgs* req) override;

    StatusCode SubmitCypherSGLReq(const std::vector<unsigned char*>& src,
                                  const std::vector<unsigned int>& src_len,
                                  unsigned char* dst,
                                  unsigned int dst_len,
                                  CypherReqArgs* req) override;

    StatusCode SubmitAsync(const CypherReqArgs& req) override {
        return StatusCode::kNotSupported;
    }

    ssize_t Poll(CypherResult results[], unsigned int max_num, int timeout) override;

private:
    CypherChannelOption channel_opts_;
    std::unique_ptr<unsigned char[]> aes_xts_key_;
};

}  // namespace vesal