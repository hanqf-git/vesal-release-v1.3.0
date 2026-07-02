#! /bin/bash

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

help_message="$(basename "$0") [--debug] [--release] [--asan] [--gcov] [--disable_test] [--disable_perf] [--disable_zstd] [--enable_lz4ipp] [--enable_err_sim] [--enable_bench] [--disable_sw_cy] [-h]

where:
    -h|--help                         Show this help message.
    --debug                           Build vesal with debug mode.
    --release                         Build vesal with release mode.
    --asan                            Enable asan sanitizer.
    --gcov                            Enable coverage analysis.
    --disable_test                    Disable build unit test.
    --disable_perf                    Disable build perf tools.
    --disable_zstd                    Disable zstd related functionality.
    --enable_lz4ipp                   Enable lz4 ipp feature.
    --enable_err_sim                  Enable error simulation.
    --enable_metrics                  Enable metrics for perf, zstd and test will be disabled.
    --enable_bench                    Enable build benchmark.
    --disable_sw_cy                   Disable build software cypher and openssl
    --disable_kae                     Disable KAE codec support"

SOURCE_DIR=`pwd`
BUILD_DIR=${BUILD_DIR:-./build}
BUILD_TYPE=RelWithDebInfo
CXX=${CXX:-g++}
ENABLE_ASAN="OFF"
ENABLE_GCOV="OFF"
ENABLE_TEST="ON"
ENABLE_PERF="ON"
ENABLE_ZSTD="ON"
ENABLE_LZ4IPP="OFF"
ENABLE_ERR_SIM="OFF"
ENABLE_METRICS="OFF"
ENABLE_BENCH="OFF"
ENABLE_SSL="ON"
ENABLE_KAE="ON"

while [[ $# > 0 ]]; do
    key="$1"
    case $key in
        -h|--help)
            echo "$help_message"
            exit
            ;;
        debug|--debug)
            BUILD_TYPE=Debug
            echo "build vesal with Debug mode"
            ;;
        release|--release)
            BUILD_TYPE=RelWithDebInfo
            echo "build vesal with RelWithDebInfo mode"
            ;;
        --asan)
            ENABLE_ASAN="ON"
            echo "build with asan"
            ;;
        --gcov)
            ENABLE_GCOV="ON"
            echo "build with gcov"
            ;;
        --disable_test)
            ENABLE_TEST="OFF"
            echo "build unit test"
            ;;
        --disable_perf)
            ENABLE_PERF="OFF"
            echo "build perf tools"
            ;;
        --disable_zstd)
            ENABLE_ZSTD="OFF"
            echo "disable zstd related functionality"
            ;;
        --enable_lz4ipp)
            ENABLE_LZ4IPP="ON"
            echo "enable lz4 ipp feature"
            ;;
        --enable_err_sim)
            ENABLE_ERR_SIM="ON"
            echo "enable error simulation"
            ;;
        --enable_metrics)
            ENABLE_METRICS="ON"
            # Disable following options to prevent dependency conflicts
            ENABLE_ZSTD="OFF"
            ENABLE_TEST="OFF"
            echo "enable metrics for perf"
            ;;
        --enable_bench)
            ENABLE_BENCH="ON"
            echo "enable benchmarks"
            ;;
        --disable_sw_cy)
            ENABLE_SSL="OFF"
            echo "disable software cypher"
            ;;
        --disable_kae)
            ENABLE_KAE="OFF"
            echo "disable KAE codec support"
            ;;
    esac
    shift
done

set -e
set -x

git submodule update --init --recursive

cmake -S . -B build -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DBUILD_TESTING=ON \
    -DVESAL_ENABLE_ASAN=$ENABLE_ASAN \
    -DVESAL_ENABLE_GCOV=$ENABLE_GCOV \
    -DVESAL_BUILD_TESTS=$ENABLE_TEST \
    -DVESAL_BUILD_PERF=$ENABLE_PERF \
    -DVESAL_ENABLE_ZSTD=$ENABLE_ZSTD \
    -DVESAL_ENABLE_ERR_SIM=$ENABLE_ERR_SIM \
    -DVESAL_ENABLE_LZ4IPP=$ENABLE_LZ4IPP $SOURCE_DIR \
    -DVESAL_ENABLE_BYTE_METRICS=$ENABLE_METRICS \
    -DVESAL_BUILD_BENCH=$ENABLE_BENCH \
    -DVESAL_ENABLE_SSL=$ENABLE_SSL \
    -DVESAL_ENABLE_KAE=$ENABLE_KAE

cmake --build build -j $(nproc)

cd $SOURCE_DIR
rm -f perf_simple
ln -s build/perf/codec/perf_simple perf_simple
