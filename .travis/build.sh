#!/bin/bash
#
# This is a distribution-agnostic build script. Do not use "apt-get", "dnf", or
# similar in here. Add any package installation gunk into the appropriate
# install script instead.
#
set -ex

cleanup() {
	rm -rf build-{a,ub,t}san build-meson build-cmake
}

trap cleanup EXIT
cleanup

FOUND_CLANG=0
FOUND_GCC=0

if type -P clang && type -P clang++; then
	echo "Beginning build tests with Clang" >&2
	export CC=clang CXX=clang++
	bash .travis/build-meson.sh
	bash .travis/build-cmake.sh
	FOUND_CLANG=1
fi

if type -P gcc && type -P g++; then
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

exit 0
