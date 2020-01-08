#!/bin/bash
#
# Meson build test
#

set -e

cleanup() {
	echo "Cleaning up Meson build directories" >&2
	rm -rf build-meson
}

trap cleanup EXIT
cleanup

MESON_ARGS=(
	-Dlight_tests=true
	-DWerror=true
)

set -x

# Build lightweight test builds
meson . build-meson ${MESON_ARGS[@]} --buildtype debugoptimized

# Build all targets of CMake/meson, ensuring everything can build.
ninja -C build-meson

# Run basic tests
build-meson/tests/test_crypto

set +x

exit 0
