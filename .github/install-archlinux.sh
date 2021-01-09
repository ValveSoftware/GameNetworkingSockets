#!/bin/bash
#
# This is an install script for Arch Linux-specific packages.
#
set -ex

# Base build packages
PACKAGES=(
	base-devel
	ccache
	clang
	cmake
	git
	go
	ninja
)

PACKAGES+=(protobuf)
PACKAGES+=(openssl)
PACKAGES+=(libsodium)

pacman --noconfirm -Syu
pacman --noconfirm -Sy "${PACKAGES[@]}"

exit 0
