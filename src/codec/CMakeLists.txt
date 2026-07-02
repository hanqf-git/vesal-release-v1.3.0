# Copyright (c) 2023 ByteDance Inc.
#
# This file is part of veSAL.
#
# veSAL is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# veSAL is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with veSAL. If not, see <https://www.gnu.org/licenses/>.

cmake_minimum_required(VERSION 3.13)

add_library(
  vesal_codec_obj OBJECT
  codec.cc
  codec_common.cc
  dc_format.cc
  qat/qat_codec_engine.cc
  qat/qat_codec.cc
  qat/qat_ha.cc
  qat/qat_handle.cc
  qat/qat_poller_worker.cc
  qat/qat_poller.cc
  qat/qat_poller_manager.cc
  qat/qat_codec_shared_channel.cc
  qat/qat_codec_dedicated_channel.cc
  sw/sw_codec_channel.cc
  sw/sw_codec.cc)
if(VESAL_ENABLE_KAE)
  target_sources(vesal_codec_obj PRIVATE kae/kae_codec.cc
                                         kae/kae_codec_channel.cc)
endif()
if(${VESAL_ENABLE_ZSTD})
  target_sources(vesal_codec_obj PRIVATE zstd_helper/zstd_helper.cc
                                         zstd_helper/zstd_helper_impl.cc)
endif()

target_link_libraries(
  vesal_codec_obj PRIVATE vesal_compile_options vesal_includes
                          vesal_internal_includes vesal_third_libs)

