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



 #include "data_flow/data_flow_request.h"
 #include "common/checksum_impl.h"
 
 namespace vesal {
 namespace data_flow {
 
 
 std::ostream& operator<<(std::ostream& os, const DataFlowRequest& req)
 {
   os << "{ user_ctx: " << req.user_ctx;

#ifndef NDEBUG
os << ",\n  src(size=" << req.src.size() << "): [";
for (size_t i = 0; i < req.src.size(); ++i) {
    os << req.src[i];
    if (i + 1 != req.src.size())
        os << ", ";
}

os << "],\n  src_len: [";
for (size_t i = 0; i < req.src_len.size(); ++i) {
    os << req.src_len[i];
    if (i + 1 != req.src_len.size())
        os << ", ";
}

os << "],\n  dst(size=" << req.dst.size() << "): [";
for (size_t i = 0; i < req.dst.size(); ++i) {
    os << req.dst[i];
    if (i + 1 != req.dst.size())
        os << ", ";
}

os << "],\n  dst_len: [";
for (size_t i = 0; i < req.dst_len.size(); ++i) {
    os << req.dst_len[i];
    if (i + 1 != req.dst_len.size())
        os << ", ";
}

os << "],\n  src_crc: [";
for (size_t i = 0; i < req.src.size(); ++i) {
    os << ComputeCRC32(
        kCrc32cInitialValue, reinterpret_cast<char*>(req.src[i]), req.src_len[i]);
    if (i + 1 != req.src.size())
        os << ", ";
}

os << "],\n  dst_crc: [";
for (size_t i = 0; i < req.dst.size(); ++i) {
    os << ComputeCRC32(
        kCrc32cInitialValue, reinterpret_cast<char*>(req.dst[i]), req.dst_len[i]);
    if (i + 1 != req.dst.size())
        os << ", ";
}
os << "]";
#endif
os << " }";
  return os;
 }
 
 }  // namespace data_flow
 }  // namespace vesal
 