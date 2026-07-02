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

cd $BUILD_DIR/test
ln -sf $SOURCE_DIR/scripts/valgrind_ignore.supp .
ln -sf $SOURCE_DIR/scripts/parallel_testing.py .
ln -sf $SOURCE_DIR/scripts/valgrind_block_list .
# 4 test workers (bound by QAT/DSA parallelism);
# 0 extra workers (used when tests are detected to be slow);
# 1800 sec as worker timeout;
# runs every test 3 times (gtest_repeat);
# "valgrind_block_list" skip blocked tests;
# "-r" means we search all tests under current directory recursively;
# "valgrind_ignore.supp" suppress related errors.
#
# see scripts argv parsing logic for more details.
python parallel_testing.py 4 0 1800 3 "valgrind_block_list" -r "valgrind_ignore.supp"