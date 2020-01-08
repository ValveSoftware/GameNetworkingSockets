#!/bin/bash
#
# This is an install script for Alpine-specific packages.
#
set -ex

apk update

# Base build packages
PACKAGES=(
	clang
	gcc
	g++
	cmake
	meson
	pkgconf
)

PACKAGES+=(protobuf-dev)
PACKAGES+=(openssl-dev)

apk add "${PACKAGES[@]}"

exit 0
