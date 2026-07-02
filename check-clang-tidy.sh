#! /bin/bash

# Requires clang-tidy-14, and assume compile_commands.json is generated under build directory.
FIX=""
if [[ "$1" == "--fix" ]]; then
    FIX="-fix"
fi
SOURCE_DIR=$(pwd)
BUILD_DIR_NAME="build"
# $1 is the build dir name
if [[ -e "$1" ]]; then
    BUILD_DIR_NAME=$1
fi
BUILD_DIR=$SOURCE_DIR/$BUILD_DIR_NAME

# TODO(sjj): check all files
CXX_FILES=$(find ${SOURCE_DIR}/src/codec -name "*.cc" -or -name "*.h" -or -name "*.cpp")
INCLUDE_FILES=$(find ${SOURCE_DIR}/include -name "*.h" -or -name "*.hpp")
ALL_FILES="$CXX_FILES $INCLUDE_FILES"

COMPILE_COMMANDS=$(realpath $BUILD_DIR/compile_commands.json)
if [ ! -f "$COMPILE_COMMANDS" ]; then
    echo "compile_commands.json not found, skip clang-tidy check"
    exit 1
fi

CLANG_TIDY_BINARY=$(which clang-tidy)
if [ -z "$CLANG_TIDY_BINARY" ]; then
    echo "clang-tidy not found, skip clang-tidy check"
    exit 1
fi

# Recommend to use v14, but newer than 14 should be ok in most cases.
CLANG_TIDY_VERSION=$("$CLANG_TIDY_BINARY" --version)
MAJOR_VERSION=$(echo "$CLANG_TIDY_VERSION" | grep -oP 'version \K[0-9]+')
if [[ $MAJOR_VERSION -lt 14 ]]; then
    echo "clang-tidy version is older than 14, skip clang-tidy check"
    exit 1
fi

# Don't do auto fix, better to lookup by human.
# TODO(sjj): add -header-filter="^${SOURCE_DIR}/(include|src)/" later.
python3 $SOURCE_DIR/tools/run-clang-tidy.py $FIX -clang-tidy-binary $CLANG_TIDY_BINARY -p $BUILD_DIR $ALL_FILES
