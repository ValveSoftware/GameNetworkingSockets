#!/bin/bash
#
# CMake build tests
#

set -ex

CMAKE_ARGS=(
	-G Ninja
	-DLIGHT_TESTS:BOOL=ON
	-DWERROR=ON
)

BUILD_SANITIZERS=1
[[ $(uname -s) == MINGW* ]] && BUILD_SANITIZERS=0

# Build some tests with sanitizers
if [[ $BUILD_SANITIZERS -ne 0 ]]; then
	mkdir -p build-{a,ub,t}san
	cmake -S . -B build-asan ${CMAKE_ARGS[@]} -DSANITIZE_ADDRESS:BOOL=ON
	cmake -S . -B build-ubsan ${CMAKE_ARGS[@]} -DSANITIZE_UNDEFINED:BOOL=ON
	if [[ ${CXX} == *clang* ]]; then
		cmake -S . -B build-tsan ${CMAKE_ARGS[@]} -DSANITIZE_THREAD:BOOL=ON
	fi
fi

mkdir -p build-cmake
cmake -S . -B build-cmake ${CMAKE_ARGS[@]} -DCMAKE_BUILD_TYPE=RelWithDebInfo ..

# Build normal unsanitized binaries
ninja -C build-cmake

# Build specific extended tests for code correctness validation
if [[ $BUILD_SANITIZERS -ne 0 ]]; then
	ninja -C build-asan test_connection test_crypto
	ninja -C build-ubsan test_connection test_crypto
	if [[ -d build-tsan ]]; then
		ninja -C build-tsan test_connection test_crypto
	fi
fi

# Run basic tests
build-cmake/tests/test_crypto
build-cmake/tests/test_connection

# Run sanitized builds
if [[ $BUILD_SANITIZERS -ne 0 ]]; then
	for SANITIZER in asan ubsan tsan; do
		[[ -d build-${SANITIZER} ]] || continue
		build-${SANITIZER}/tests/test_crypto
		build-${SANITIZER}/tests/test_connection
	done
fi

exit 0
