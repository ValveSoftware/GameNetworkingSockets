#!/bin/bash
#
# Meson build test
#

set -e

cleanup() {
	echo "Cleaning up Meson build directories" >&2
	rm -rf build-meson build-meson-ref
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
meson . build-meson-ref ${MESON_ARGS[@]} --buildtype debugoptimized -Duse_crypto25519=Reference

# Build all targets
ninja -v -C build-meson
ninja -v -C build-meson-ref

# Run basic tests
build-meson/tests/test_crypto
build-meson-ref/tests/test_crypto

set +x

exit 0
