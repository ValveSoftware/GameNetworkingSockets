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

bash .travis/build-meson.sh
bash .travis/build-cmake.sh

exit 0
