#!/bin/bash

# Copyright (c) 2025 ByteDance Inc.
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

set -e
set -x

SOURCE_DIR=$(pwd)
BUILD_DIR=$SOURCE_DIR/build

############## run all unitests ###############
SERIAL_TESTS_REGEX="data_flow_engines_test|dsa_ctx_factory_test"

parallel_num=4
parallel_num=$(grep -oP 'ssl_num:\s*\K\d+' /etc/qat_conf_version 2>/dev/null || echo 4)

cd $BUILD_DIR/test
CTEST_OUTPUT_ON_FAILURE=1 ctest -j${parallel_num} -E $SERIAL_TESTS_REGEX
# dsa tests don't support parallel for now.
CTEST_OUTPUT_ON_FAILURE=1 ctest -j1 -R $SERIAL_TESTS_REGEX