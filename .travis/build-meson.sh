#!/bin/bash
#
# Meson build test
#

set -e

cleanup() {
	echo "Cleaning up Meson build directories" >&2
	rm -rf build-meson{,-ref,-sodium{,25519}}
}

trap cleanup EXIT
cleanup

MESON_ARGS=(
	-Dlight_tests=true
	-DWerror=true
)

BUILD_LIBSODIUM=1

# libsodium's AES implementation only works on x86_64
[[ $(uname -m) != x86_64 ]] && BUILD_LIBSODIUM=0

set -x

# Build lightweight test builds
meson . build-meson ${MESON_ARGS[@]} --buildtype debugoptimized
meson . build-meson-ref ${MESON_ARGS[@]} --buildtype debugoptimized -Duse_crypto25519=Reference
[[ $BUILD_LIBSODIUM -ne 0 ]] && meson . build-meson-sodium ${MESON_ARGS[@]} --buildtype debugoptimized -Duse_crypto=libsodium -Duse_crypto25519=libsodium
meson . build-meson-sodium25519 ${MESON_ARGS[@]} --buildtype debugoptimized -Duse_crypto25519=libsodium

# Build all targets
ninja -v -C build-meson
[[ $BUILD_LIBSODIUM -ne 0 ]] && ninja -v -C build-meson-sodium
ninja -v -C build-meson-sodium25519
ninja -v -C build-meson-ref

# Run basic tests
build-meson/tests/test_crypto
[[ $BUILD_LIBSODIUM -ne 0 ]] && build-meson-sodium/tests/test_crypto
build-meson-sodium25519/tests/test_crypto
build-meson-ref/tests/test_crypto

set +x

exit 0
