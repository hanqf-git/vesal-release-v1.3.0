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

add_executable(vesal_codec_example vesal_codec_example.cc)
add_executable(ha_demo ha_demo.cc)
add_executable(c_vesal_demo c_vesal_demo.c)
add_executable(c_cypher_demo c_cypher_demo.c)
add_executable(vesal_data_flow_example vesal_data_flow_example.cc)

target_link_libraries(vesal_codec_example PRIVATE vesal)
target_link_libraries(ha_demo PRIVATE vesal)
target_link_libraries(c_vesal_demo PRIVATE vesal)
target_link_libraries(c_cypher_demo PRIVATE vesal)
target_link_libraries(vesal_data_flow_example PRIVATE vesal)