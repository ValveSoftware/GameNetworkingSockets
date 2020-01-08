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

FOUND_CLANG=0
FOUND_GCC=0

export BUILD_SANITIZERS=0

# Only run sanitizers on a couple of the builder types.
if [[ "$IMAGE" == "ubuntu" ]] && [[ "$IMAGE_TAG" == "rolling" ]]; then
	export BUILD_SANITIZERS=1
fi
if [[ "$IMAGE" == "fedora" ]] && [[ "$IMAGE_TAG" == "rawhide" ]]; then
	export BUILD_SANITIZERS=1
fi

if has clang && has clang++; then
	echo "Beginning build tests with Clang" >&2
	export CC=clang CXX=clang++
	bash .travis/build-meson.sh
	bash .travis/build-cmake.sh
	FOUND_CLANG=1
fi

if has gcc && has g++; then
	echo "Beginning build tests with GCC" >&2
	export CC=gcc CXX=g++
	bash .travis/build-meson.sh
	bash .travis/build-cmake.sh
	FOUND_GCC=1
fi

if [[ $FOUND_CLANG -eq 0 ]] && [[ $FOUND_GCC -eq 0 ]]; then
	echo "FAILED: Couldn't find either Clang or GCC, no tests were run." >&2
	exit 1
fi

echo "All builds and tests executed successfully." >&2

exit 0
