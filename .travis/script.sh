#!/bin/bash
#
# This is a distribution-agnostic build script. Do not use "apt-get", "dnf", or
# similar in here. Add any package installation gunk into the appropriate
# install script instead.
#
set -ex
mkdir build-cmake
(cd build-cmake && cmake -G Ninja -DLIGHT_TESTS:BOOL=ON ..)
meson . build-meson -Dlight_tests=true
ninja -C build-cmake
ninja -C build-meson
build-meson/tests/test_crypto
build-cmake/tests/test_crypto
build-cmake/tests/test_connection || true
