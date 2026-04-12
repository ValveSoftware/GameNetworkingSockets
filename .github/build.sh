#!/bin/bash
#
# CI build orchestration. This script may run multiple configurations, but each
# individual configuration is delegated to .github/build-single-config.py.
#
set -euo pipefail

has() {
	if type -P "$@" &>/dev/null; then
		return 0
	fi
	return 1
}

has_clang() {
	has clang && has clang++
}

has_gcc() {
	has gcc && has g++
}

has_python3() {
	has python3
}

msg() {
	echo "$1" >&2
}

die() {
	msg "$1"
	exit 1
}

run_single() {
	python3 .github/run-single-config.py "$@"
}

run_matrix_for_current_compiler() {
	local uname_S uname_M
	local build_sanitizers build_libsodium build_webrtc

	uname_S="$(uname -s)"
	uname_M="$(uname -m)"

	# Default build matrix options. Any exceptions should be spelled out after this
	# block.
	build_sanitizers="${BUILD_SANITIZERS:-0}"
	build_libsodium="${BUILD_LIBSODIUM:-1}"
	build_webrtc="${BUILD_WEBRTC:-1}"

	# Sanitizers aren't supported on MinGW
	[[ "${uname_S}" == MINGW* ]] && build_sanitizers=0

	# Noticed that Clang's tsan and asan don't behave well on non-x86_64 Travis
	# builders, so let's just disable them on there.
	[[ "${uname_M}" != x86_64 ]] && [[ ${CXX} == *clang* ]] && build_sanitizers=0

	# Sanitizers don't link properly with clang on Fedora Rawhide
	[[ "${IMAGE:-}" == "fedora" ]] && [[ "${IMAGE_TAG:-}" == "rawhide" ]] && [[ ${CXX} == *clang* ]] && build_sanitizers=0

	# Sanitizers don't link properly with clang on Ubuntu Rolling
	[[ "${IMAGE:-}" == "ubuntu" ]] && [[ "${IMAGE_TAG:-}" == "rolling" ]] && [[ ${CXX} == *clang* ]] && build_sanitizers=0

	# Something's wrong with the GCC -fsanitize=address build on the s390x Travis
	# builder, and it fails to link properly.
	[[ ${uname_M} == s390x ]] && build_sanitizers=0

	# clang with sanitizers results in link errors on i686 Ubuntu at any rate
	[[ ${CXX} == *clang* ]] && [[ "$(${CXX} -dumpmachine)" == i686-* ]] && build_sanitizers=0

	# Big-endian platforms can't build with WebRTC out-of-the-box.
	[[ ${uname_M} == s390x ]] && build_webrtc=0
	[[ ${uname_M} == powerpc ]] && build_webrtc=0
	[[ ${uname_M} == ppc64* ]] && build_webrtc=0

	# Foreign architecture docker containers don't support sanitizers.
	[[ ${uname_M} != x86_64 ]] && grep -q -e AuthenticAMD -e GenuineIntel /proc/cpuinfo && build_sanitizers=0

	# libsodium's AES implementation only works on x86_64
	[[ ${uname_M} != x86_64 ]] && build_libsodium=0

	msg ""
	msg "======================================================="
	msg "Platform"
	msg ""
	msg "uname -s         = ${uname_S}"
	msg "uname -m         = ${uname_M}"
	msg ""
	msg "======================================================="
	msg "Compiler"
	msg ""
	msg "CC               = ${CC}"
	msg "CXX              = ${CXX}"
	msg ""
	msg "======================================================="
	msg "Platform permits the following optional build configs:"
	msg ""
	msg "BUILD_SANITIZERS = ${build_sanitizers}"
	msg "BUILD_LIBSODIUM  = ${build_libsodium}"
	msg "BUILD_WEBRTC     = ${build_webrtc}"
	msg ""
	msg "======================================================="

	if [[ "${CC}" == "clang" ]]; then
		compiler="clang"
	else
		compiler="gcc"
	fi

	# Build some tests with sanitizers
	if [[ ${build_sanitizers} -ne 0 ]]; then
		run_single --compiler "${compiler}" --build-dir build-asan --sanitizer asan --targets test_connection test_crypto --run-tests
		run_single --compiler "${compiler}" --build-dir build-ubsan --sanitizer ubsan --targets test_connection test_crypto --run-tests
		if [[ ${CXX} == *clang* ]]; then
			run_single --compiler "${compiler}" --build-dir build-tsan --sanitizer tsan --targets test_connection test_crypto --run-tests
		fi
	fi

	# Build normal unsanitized binaries
	run_single --compiler "${compiler}" --build-dir build-cmake --build-type RelWithDebInfo --run-tests

	# Build debug build
	run_single --compiler "${compiler}" --build-dir build-cmake-debug --build-type Debug --run-tests

	# Build binaries with LTO. Optional, some compilers don't support LTO.
	if ! run_single --compiler "${compiler}" --build-dir build-cmake-lto --build-type Release --lto --run-tests; then
		msg "Single-config LTO build failed, likely because the compiler does not support LTO. Continuing anyway!"
	fi

	# Build binaries with webrtc
	if [[ ${build_webrtc} -ne 0 ]]; then
		run_single --compiler "${compiler}" --build-dir build-cmake-webrtc --build-type RelWithDebInfo --use-webrtc
	fi

	# Build binaries with reference ed25519/curve25519
	run_single --compiler "${compiler}" --build-dir build-cmake-ref --build-type RelWithDebInfo --crypto25519 Reference --run-tests --tests test_crypto

	# Build binaries with libsodium for ed25519/curve25519 only
	run_single --compiler "${compiler}" --build-dir build-cmake-sodium25519 --build-type RelWithDebInfo --crypto25519 libsodium --run-tests --tests test_crypto

	# Build binaries with libsodium
	if [[ ${build_libsodium} -ne 0 ]]; then
		run_single --compiler "${compiler}" --build-dir build-cmake-sodium --build-type RelWithDebInfo --crypto libsodium --crypto25519 libsodium --run-tests --tests test_crypto
	fi
}

CI_BUILD=${CI_BUILD:-0}
export BUILD_SANITIZERS=0

# Only run sanitizers on a couple of the builder types.
if [[ "$IMAGE" == "ubuntu" ]] && [[ "$IMAGE_TAG" == "rolling" ]]; then
	export BUILD_SANITIZERS=1
fi
if [[ "$IMAGE" == "fedora" ]] && [[ "$IMAGE_TAG" == "rawhide" ]]; then
	export BUILD_SANITIZERS=1
fi
if [[ "$CI_BUILD" -eq 0 ]]; then
	# If we're doing a non-CI build, we should build the sanitizers for our
	# own debugging.
	export BUILD_SANITIZERS=1
fi

msg "Image is ${IMAGE:-unknown}:${IMAGE_TAG:-unknown}, sanitizers enabled: $BUILD_SANITIZERS"

# Make sure cmake is installed
has cmake || die "cmake required"
has_python3 || die "python3 required"

# We also need at least one compiler
has_clang || has_gcc || die "No compiler available"

export CCACHE_DIR=$PWD/build-ci-ccache
ccache -M4G

# Mark all directories as safe so checkouts performed in CMakeLists.txt don't cause "unsafe repository" errors.
# See https://github.com/actions/checkout/issues/766
git config --global --add safe.directory '*'

# Use shallow clones of submodules for space/time efficiency.
#git submodule update --init --depth=1 2>&1 | cat

if has_clang; then
	msg "Beginning build tests with Clang"
	export CC=clang CXX=clang++
	run_matrix_for_current_compiler
fi

if has_gcc; then
	msg "Beginning build tests with GCC"
	export CC=gcc CXX=g++
	run_matrix_for_current_compiler
fi

msg "All builds and tests executed successfully."

exit 0
