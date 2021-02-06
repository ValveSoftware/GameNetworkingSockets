#!/bin/bash
#
# This is an install script for Ubuntu-specific packages.
#
set -ex

APT_FLAGS=(-q -oDpkg::Use-Pty=0)

export DEBIAN_FRONTEND=noninteractive

apt-get ${APT_FLAGS[@]} install -y locales
locale-gen en_US.UTF-8

PACKAGES=(
	build-essential
	pkg-config
	ccache
	cmake
	ninja-build
	clang
	git
	golang
)

PACKAGES+=(libprotobuf-dev protobuf-compiler)
PACKAGES+=(libssl-dev)
PACKAGES+=(libsodium-dev)

apt-get ${APT_FLAGS[@]} install -y "${PACKAGES[@]}"

exit 0
