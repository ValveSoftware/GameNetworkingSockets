#!/bin/bash
#
# Meson build test
#

set -ex

MESON_ARGS=(
	-Dlight_tests=true
	-DWerror=true
)


# Build lightweight test builds
meson . build-meson ${MESON_ARGS[@]} --buildtype debugoptimized

# Build all targets of CMake/meson, ensuring everything can build.
ninja -C build-meson

# Run basic tests
build-meson/tests/test_crypto

exit 0
