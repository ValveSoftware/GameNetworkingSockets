#!/bin/bash
#
# This is a distribution-agnostic build script. Do not use "apt-get", "dnf", or
# similar in here. Add any package installation gunk into the appropriate
# install script instead.
#
set -ex

# Build some tests with sanitizers
mkdir build-asan
(cd build-asan && cmake -G Ninja -DSANITIZE_ADDRESS:BOOL=ON -DLIGHT_TESTS:BOOL=ON ..)

mkdir build-ubsan
(cd build-ubsan && cmake -G Ninja -DSANITIZE_UNDEFINED:BOOL=ON -DLIGHT_TESTS:BOOL=ON ..)

if [[ ${CXX} == *clang* ]]; then
	mkdir build-tsan
	(cd build-tsan && cmake -G Ninja -DSANITIZE_THREAD:BOOL=ON -DLIGHT_TESTS:BOOL=ON ..)
fi

# Build lightweight test builds
mkdir build-cmake
(cd build-cmake && cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DLIGHT_TESTS:BOOL=ON ..)
meson . build-meson -Dlight_tests=true -Db_lto=true --optimization 3

# Build all targets of CMake/meson, ensuring everything can build.
ninja -C build-meson
ninja -C build-cmake

# Build specific tests for validation
ninja -C build-asan test_connection test_crypto
ninja -C build-ubsan test_connection test_crypto
if [ -d build-tsan ]; then
	ninja -C build-tsan test_connection test_crypto
fi

# Run basic tests
build-meson/tests/test_crypto
build-cmake/tests/test_crypto
build-cmake/tests/test_connection

# Run sanitized builds
for SANITIZER in asan ubsan tsan; do
	[ -d build-${SANITIZER} ] || continue
	build-${SANITIZER}/tests/test_crypto
	build-${SANITIZER}/tests/test_connection
done
