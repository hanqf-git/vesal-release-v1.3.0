/*
 * Copyright (c) 2024 ByteDance Inc.
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

#include <cstring>
#include "data_flow/data_flow_request.h"
#include "vesal/log_setting.h"
#include "vesal/status.h"
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include "data_flow/dsa/dsa_ctx_factory.h"
#include "data_flow/dsa/dsa_dispatcher.h"
#endif

namespace vesal {
namespace data_flow {

class DataFlowEngine {
public:
    virtual StatusCode Init(const DataFlowInitOptions& init_opts) {
        return StatusCode::kNotSupported;
    }
    virtual ~DataFlowEngine() = default;
    virtual StatusCode Submit(DataFlowRequest* item) {
        return StatusCode::kNotSupported;
    }
    virtual void CheckCompletion(DataFlowRequest* item, DataFlowResult* result) {}

    virtual StatusCode PreallocateCtx(DataFlowRequest* item) {
        return StatusCode::kNotSupported;
    }

    virtual void ClearCtx(DataFlowRequest* item) {}
};

class SwEngine : public DataFlowEngine {
public:
    StatusCode Submit(DataFlowRequest* req) override;
    void CheckCompletion(DataFlowRequest* req, DataFlowResult* result) override;
    StatusCode PreallocateCtx(DataFlowRequest* item) override;
    void ClearCtx(DataFlowRequest* item) override;
};

}  // namespace data_flow
}  // namespace vesal

namespace vesal {
namespace data_flow {
class DsaEngine : public DataFlowEngine {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
public:
    StatusCode Init(const DataFlowInitOptions& init_opts) override;

    StatusCode Submit(DataFlowRequest* item) override;

    void CheckCompletion(DataFlowRequest* item, DataFlowResult* result) override;

    StatusCode PreallocateCtx(DataFlowRequest* item) override;

    void ClearCtx(DataFlowRequest* item) override;

private:
    std::unique_ptr<DsaDispatcher> dsa_dispatcher_;
    std::unique_ptr<DsaCtxFactory> dsa_desc_factory_;
#endif
};
}  // namespace data_flow
}  // namespace vesal
