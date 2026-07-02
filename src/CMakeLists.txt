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

target_include_directories(vesal_internal_includes
                           INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/)

add_subdirectory(common)
add_subdirectory(codec)
add_subdirectory(data_flow)
add_subdirectory(c_api)
add_subdirectory(cypher)

set(vesal_objs)

list(APPEND vesal_objs 
  $<TARGET_OBJECTS:vesal_data_flow_obj>
  $<TARGET_OBJECTS:vesal_common_obj>
  $<TARGET_OBJECTS:vesal_codec_obj>
  $<TARGET_OBJECTS:qat_dummy_driver_obj>
  $<TARGET_OBJECTS:c_api_obj>
  $<TARGET_OBJECTS:vesal_cypher_obj>)

if(VESAL_ENABLE_KAE)
  list(APPEND vesal_objs $<TARGET_OBJECTS:kae_dummy_driver_obj>)
endif()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "i[3-9]86")
  list(APPEND vesal_objs $<TARGET_OBJECTS:lib_dsa_uio_config>)
endif()

add_library(vesal STATIC ${vesal_objs})

target_link_libraries(vesal PRIVATE vesal_compile_options
                                    vesal_third_libs
                                    qat_dummy_driver)
target_link_libraries(vesal PUBLIC vesal_includes)
