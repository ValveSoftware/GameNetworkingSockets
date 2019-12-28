#!/bin/bash
#
# This is an install script for Ubuntu-specific packages.
#
set -ex
apt-get update
apt-get install -y locales
locale-gen en_US.UTF-8

PACKAGES=(build-essential pkg-config cmake meson clang)

PACKAGES+=(libprotobuf-dev protobuf-compiler)

if [[ $CRYPTO == "libsodium" ]]; then
    PACKAGES+=(libsodium-dev)
else
    PACKAGES+=(libssl-dev)
fi

apt-get install -y "${PACKAGES[@]}"
