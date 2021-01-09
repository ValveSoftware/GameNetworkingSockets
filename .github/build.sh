#!/bin/bash
#
# This is a distribution-agnostic build script. Do not use "apt-get", "dnf", or
# similar in here. Add any package installation gunk into the appropriate
# install script instead.
#
set -e

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

msg() {
	echo "$1" >&2
}

die() {
	msg "$1"
	exit 1
}

TRAVIS_BUILD=${TRAVIS_BUILD:-0}
export BUILD_SANITIZERS=0

# Only run sanitizers on a couple of the builder types.
if [[ "$IMAGE" == "ubuntu" ]] && [[ "$IMAGE_TAG" == "rolling" ]]; then
	export BUILD_SANITIZERS=1
fi
if [[ "$IMAGE" == "fedora" ]] && [[ "$IMAGE_TAG" == "rawhide" ]]; then
	export BUILD_SANITIZERS=1
fi
if [[ "$TRAVIS_BUILD" -eq 0 ]]; then
	# If we're doing a non-Travis build, we should build the sanitizers for our
	# own debugging.
	export BUILD_SANITIZERS=1
fi

msg "Image is $IMAGE:$IMAGE_TAG, sanitizers enabled: $BUILD_SANITIZERS"

# Make sure cmake is installed
has cmake || die "cmake required"

# We also need at least one compiler
has_clang || has_gcc || die "No compiler available"

export CCACHE_DIR=$PWD/build-travis-ccache
ccache -M4G

if has_clang; then
	msg "Beginning build tests with Clang"
	export CC=clang CXX=clang++
	has cmake && bash .travis/build-cmake.sh
fi

if has_gcc; then
	msg "Beginning build tests with GCC"
	export CC=gcc CXX=g++
	has cmake && bash .travis/build-cmake.sh
fi

msg "All builds and tests executed successfully."

exit 0
