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

#include "vesal/data_flow.h"
#include "common/checksum_impl.h"
#include "data_flow/data_flow_request.h"
#include "data_flow/data_flow_resource_manager.h"

namespace vesal {

std::ostream& operator<<(std::ostream& os, const DataFlowResult& result) {
    os << "{ "
       << "status: " << static_cast<uint64_t>(result.status) << ", "
       << "crc_output(size=" << result.crc_output.size() << "): [";
    for (size_t i = 0; i < result.crc_output.size(); ++i) {
        os << result.crc_output[i];
        if (i + 1 != result.crc_output.size())
            os << ", ";
    }
    os << "], ctx: " << result.ctx << " }";
    return os;
}

std::ostream& operator<<(std::ostream& os, const DataFlowMoveOperation& opt) {
    os << "{ ";

    os << "type: " << static_cast<int>(opt.type) << ", ";

    os << "src(size=" << opt.src.size() << "): [";
    for (size_t i = 0; i < opt.src.size(); ++i) {
        os << opt.src[i];
        if (i + 1 != opt.src.size())
            os << ", ";
    }
    os << "], ";

    os << "src_crc(size=" << opt.src.size() << "): [";
    for (size_t i = 0; i < opt.src.size(); ++i) {
        os << ComputeCRC32(
            kCrc32cInitialValue, reinterpret_cast<char*>(opt.src[i]), opt.src_len[i]);
        if (i + 1 != opt.src.size())
            os << ", ";
    }
    os << "]" << std::endl;

    os << "src_len(size=" << opt.src_len.size() << "): [";
    for (size_t i = 0; i < opt.src_len.size(); ++i) {
        os << opt.src_len[i];
        if (i + 1 != opt.src_len.size())
            os << ", ";
    }
    os << "], ";

    os << "dst: " << opt.dst << ", ";
    os << "seed: " << opt.seed << ", ";
    os << "enable_crc: " << (opt.enable_crc ? "true" : "false");

    os << " }";
    return os;
}

std::ostream& operator<<(std::ostream& os, const DataFlowCrcOperation& opt) {
    os << "{ "
       << "type: " << static_cast<int>(opt.type) << ", "
       << "src: " << opt.src << ", "
       << "src_crc: "
       << ComputeCRC32(kCrc32cInitialValue, reinterpret_cast<char*>(opt.src), opt.len) << ", "
       << "len: " << opt.len << ", "
       << "seed: " << opt.seed << " }";
    return os;
}

std::pair<StatusCode, std::unique_ptr<DataFlowChannel>> DataFlowChannel::CreateDataFlowChannel(
    const DataFlowChannelOptions& opts) {
    static data_flow::DataFlowResourceManager* rm =
        data_flow::DataFlowResourceManager::GetInstance();
    return rm->CreateDataFlowChannel(opts);
}

}  // namespace vesal
