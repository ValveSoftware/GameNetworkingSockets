#!/bin/bash
#
# This is an install script for Alpine-specific packages.
#
set -ex

apk update

# Base build packages
PACKAGES=(
	clang
	clang-dev
	gcc
	g++
	ccache
	cmake
	ninja
	pkgconf
	git
	linux-headers
	go
)

PACKAGES+=(protobuf-dev)
PACKAGES+=(openssl-dev)
PACKAGES+=(libsodium-dev)

apk add "${PACKAGES[@]}"

exit 0
