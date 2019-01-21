#!/bin/bash
#
# This is an install script for Fedora-specific packages.
#
set -ex

# Base build packages
PACKAGES=(
	gcc-c++
	clang
	cmake
	meson
	pkgconf-pkg-config
)

PACKAGES+=(protobuf-compiler protobuf-devel)
PACKAGES+=(openssl-devel)

dnf install -y "${PACKAGES[@]}"
