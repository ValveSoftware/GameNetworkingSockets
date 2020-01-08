#!/bin/bash
#
# This is an install script for Arch Linux-specific packages.
#
set -ex

# Base build packages
PACKAGES=(
	base-devel
	clang
	cmake
	meson
)

PACKAGES+=(protobuf)
PACKAGES+=(openssl)

pacman --noconfirm -Sy "${PACKAGES[@]}"

exit 0
