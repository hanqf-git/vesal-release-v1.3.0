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

project(qat_dummy_driver_test CXX)

set (CMAKE_CXX_STANDARD 14)

add_executable(qat_dummy_driver_test
    ${PROJECT_SOURCE_DIR}/qat_dummy_driver_test.cc
)

target_link_libraries(qat_dummy_driver_test
    PRIVATE qat_dummy_driver vesal_internal_includes vesal_compile_options
)
