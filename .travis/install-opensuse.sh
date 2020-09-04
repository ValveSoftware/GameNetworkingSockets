#!/bin/bash
#
# This is an install script for OpenSuSE-specific packages.
#
set -ex

# Base build packages
PACKAGES=(
	gcc-c++
	ccache
	clang
	cmake
	ninja
	pkgconf-pkg-config
	git
	make
	go
)

PACKAGES+=(protobuf-devel)
PACKAGES+=(libopenssl-devel)
PACKAGES+=(libsodium-devel)

zypper update -y
zypper install -y "${PACKAGES[@]}"

exit 0
