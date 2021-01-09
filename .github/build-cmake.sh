#!/bin/bash
#
# CMake build tests
#

set -e

CMAKE_BUILD_DIRS=()

cmake_configure() {
	BUILD_DIR="$1"
	shift
	CMAKE_BUILD_DIRS+=("$BUILD_DIR")
	rm -rf "$BUILD_DIR"
	mkdir -p "$BUILD_DIR"
	(cd "$BUILD_DIR"; cmake "$@" ..)
}

cmake_build() {
	BUILD_DIR="$1"
	shift
	cmake --build "$BUILD_DIR" -- -v "$@"
}

cleanup() {
	echo "Cleaning up CMake build directories" >&2
	rm -rf "${CMAKE_BUILD_DIRS[@]}"
}

trap cleanup EXIT

CMAKE_ARGS=(
	-G Ninja
	-DCMAKE_CXX_COMPILER_LAUNCHER=ccache
	-DCMAKE_C_COMPILER_LAUNCHER=ccache
	-DLIGHT_TESTS:BOOL=ON
	-DWERROR=ON
)

uname_S="$(uname -s)"
uname_M="$(uname -m)"

# Default build matrix options. Any exceptions should be spelled out after this
# block.
BUILD_SANITIZERS=${BUILD_SANITIZERS:-0}
BUILD_LIBSODIUM=${BUILD_LIBSODIUM:-1}
BUILD_WEBRTC=${BUILD_WEBRTC:-1}

# Sanitizers aren't supported on MinGW
[[ "${uname_S}" == MINGW* ]] && BUILD_SANITIZERS=0

# Noticed that Clang's tsan and asan don't behave well on non-x86_64 Travis
# builders, so let's just disable them on there.
[[ "${uname_M}" != x86_64 ]] && [[ ${CXX} == *clang* ]] && BUILD_SANITIZERS=0

# Sanitizers don't link properly with clang on Fedora Rawhide
[[ "${IMAGE}" == "fedora" ]] && [[ "${IMAGE_TAG}" == "rawhide" ]] && [[ ${CXX} == *clang* ]] && BUILD_SANITIZERS=0

# Sanitizers don't link properly with clang on Ubuntu Rolling
[[ "${IMAGE}" == "ubuntu" ]] && [[ "${IMAGE_TAG}" == "rolling" ]] && [[ ${CXX} == *clang* ]] && BUILD_SANITIZERS=0

# Something's wrong with the GCC -fsanitize=address build on the s390x Travis
# builder, and it fails to link properly.
[[ ${uname_M} == s390x ]] && BUILD_SANITIZERS=0

# clang with sanitizers results in link errors on i686 Ubuntu at any rate
[[ ${CXX} == *clang* ]] && [[ "$(${CXX} -dumpmachine)" == i686-* ]] && BUILD_SANITIZERS=0

# Big-endian platforms can't build with WebRTC out-of-the-box.
[[ ${uname_M} == s390x ]] && BUILD_WEBRTC=0
[[ ${uname_M} == powerpc ]] && BUILD_WEBRTC=0
[[ ${uname_M} == ppc64* ]] && BUILD_WEBRTC=0

# Foreign architecture docker containers don't support sanitizers.
[[ ${uname_M} != x86_64 ]] && grep -q -e AuthenticAMD -e GenuineIntel /proc/cpuinfo && BUILD_SANITIZERS=0

# libsodium's AES implementation only works on x86_64
[[ ${uname_M} != x86_64 ]] && BUILD_LIBSODIUM=0

cat << EOF

=======================================================
Platform

uname -s         = ${uname_S}
uname -m         = ${uname_M}

=======================================================
Compiler

CC               = ${CC}
CXX              = ${CXX}

=======================================================
Platform permits the following optional build configs:

BUILD_SANITIZERS = ${BUILD_SANITIZERS}
BUILD_LIBSODIUM  = ${BUILD_LIBSODIUM}
BUILD_WEBRTC     = ${BUILD_WEBRTC}

=======================================================
EOF

set -x

# Use shallow clones of submodules for space/time efficiency.
git submodule update --init --depth=1

# Build some tests with sanitizers
if [[ $BUILD_SANITIZERS -ne 0 ]]; then
	cmake_configure build-asan "${CMAKE_ARGS[@]}" -DSANITIZE_ADDRESS:BOOL=ON
	cmake_configure build-ubsan "${CMAKE_ARGS[@]}" -DSANITIZE_UNDEFINED:BOOL=ON
	if [[ ${CXX} == *clang* ]]; then
		cmake_configure build-tsan "${CMAKE_ARGS[@]}" -DSANITIZE_THREAD:BOOL=ON
	fi
fi

# Build normal unsanitized binaries
cmake_configure build-cmake "${CMAKE_ARGS[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake_build build-cmake

# Build debug build
cmake_configure build-cmake-debug "${CMAKE_ARGS[@]}" -DCMAKE_BUILD_TYPE=Debug
cmake_build build-cmake-debug

# Build binaries with LTO
# Optional, some compilers don't support LTO. We only try this build if it
# succeeds cmake_configure.
LTO_BUILT=0
if cmake_configure build-cmake-lto "${CMAKE_ARGS[@]}" -DCMAKE_BUILD_TYPE=Release -DLTO=ON; then
	cmake_build build-cmake-lto
	LTO_BUILT=1
else
	echo "CMake configure failed, likely because the compiler does not support LTO. Continuing anyway!" >&2
fi

# Build binaries with webrtc
if [[ $BUILD_WEBRTC -ne 0 ]]; then
	cmake_configure build-cmake-webrtc "${CMAKE_ARGS[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUSE_STEAMWEBRTC=ON
	cmake_build build-cmake-webrtc
fi

# Build binaries with reference ed25519/curve25519
cmake_configure build-cmake-ref "${CMAKE_ARGS[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUSE_CRYPTO25519=Reference
cmake_build build-cmake-ref

# Build binaries with libsodium for ed25519/curve25519 only
cmake_configure build-cmake-sodium25519 "${CMAKE_ARGS[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUSE_CRYPTO25519=libsodium
cmake_build build-cmake-sodium25519

# Build binaries with libsodium
if [[ $BUILD_LIBSODIUM -ne 0 ]]; then
	cmake_configure build-cmake-sodium "${CMAKE_ARGS[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUSE_CRYPTO=libsodium -DUSE_CRYPTO25519=libsodium
	cmake_build build-cmake-sodium
fi

# Build specific extended tests for code correctness validation
if [[ $BUILD_SANITIZERS -ne 0 ]]; then
	cmake_build build-asan test_connection test_crypto
	cmake_build build-ubsan test_connection test_crypto
	if [[ -d build-tsan ]]; then
		cmake_build build-tsan test_connection test_crypto
	fi
fi

# Run basic tests
build-cmake-ref/bin/test_crypto
[[ $BUILD_LIBSODIUM -ne 0 ]] && build-cmake-sodium/bin/test_crypto
build-cmake-sodium25519/bin/test_crypto
build-cmake/bin/test_crypto
build-cmake/bin/test_connection
build-cmake-debug/bin/test_crypto
build-cmake-debug/bin/test_connection

# Run sanitized builds
if [[ $BUILD_SANITIZERS -ne 0 ]]; then
	for SANITIZER in asan ubsan tsan; do
		[[ -d build-${SANITIZER} ]] || continue
		build-${SANITIZER}/bin/test_crypto
		build-${SANITIZER}/bin/test_connection
	done
fi

# Run LTO binaries to ensure they work
if [[ $LTO_BUILT -ne 0 ]]; then
	build-cmake-lto/bin/test_crypto
	build-cmake-lto/bin/test_connection
fi

# FIXME Run P2P tests?

set +x

exit 0
