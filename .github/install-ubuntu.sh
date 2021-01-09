#!/bin/bash
#
# This is an install script for Ubuntu-specific packages.
#
set -ex

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get dist-upgrade -y
apt-get install -y locales
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

apt-get install -y "${PACKAGES[@]}"

exit 0
