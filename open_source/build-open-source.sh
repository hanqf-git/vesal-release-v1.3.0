#! /bin/bash

help_message="$(basename "$0") [--debug] [--release] [--asan] [--gcov] [--disable_test] [--disable_perf] [--disable_zstd] [--enable_err_sim] [--enable_bench] [-h]

where:
    -h|--help                         Show this help message.
    --debug                           Build vesal with debug mode.
    --release                         Build vesal with release mode.
    --asan                            Enable asan sanitizer.
    --gcov                            Enable coverage analysis.
    --disable_test                    Disable build unit test.
    --disable_perf                    Disable build perf tools.
    --disable_zstd                    Disable zstd related functionality.
    --enable_err_sim                  Enable error simulation.
    --enable_bench                    Enable build benchmark."

SOURCE_DIR=`pwd`
BUILD_DIR=${BUILD_DIR:-./build}
BUILD_TYPE=RelWithDebInfo
CXX=${CXX:-g++}
ENABLE_ASAN="OFF"
ENABLE_GCOV="OFF"
ENABLE_TEST="ON"
ENABLE_PERF="ON"
ENABLE_ZSTD="ON"
ENABLE_ERR_SIM="OFF"
ENABLE_BENCH="OFF"

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
        --enable_err_sim)
            ENABLE_ERR_SIM="ON"
            echo "enable error simulation"
            ;;
        --enable_bench)
            ENABLE_BENCH="ON"
            echo "enable benchmarks"
            ;;
    esac
    shift
done

set -e
set -x

python3 get-deps.py

cmake -S . -B build -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
    -DBUILD_TESTING=ON \
    -DVESAL_ENABLE_ASAN=$ENABLE_ASAN \
    -DVESAL_ENABLE_GCOV=$ENABLE_GCOV \
    -DVESAL_BUILD_TESTS=$ENABLE_TEST \
    -DVESAL_BUILD_PERF=$ENABLE_PERF \
    -DVESAL_ENABLE_ZSTD=$ENABLE_ZSTD \
    -DVESAL_ENABLE_ERR_SIM=$ENABLE_ERR_SIM \
    -DVESAL_BUILD_BENCH=$ENABLE_BENCH

cmake --build build -j $(nproc)

cd $SOURCE_DIR
rm -f perf_simple
ln -s build/perf/codec/perf_simple perf_simple
