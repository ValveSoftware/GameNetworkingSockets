#!/bin/bash
#
# This is an install script for Fedora-specific packages.
#
set -ex

# Base build packages
PACKAGES=(
	gcc-c++
	libasan
	libubsan
	ccache
	clang
	cmake
	ninja-build
	pkgconf-pkg-config
	git
	make
	golang
)

PACKAGES+=(protobuf-compiler protobuf-devel)
PACKAGES+=(openssl-devel)
PACKAGES+=(libsodium-devel)

dnf install -y "${PACKAGES[@]}"

exit 0
