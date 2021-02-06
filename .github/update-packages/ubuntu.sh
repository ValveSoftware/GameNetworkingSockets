#!/bin/bash
#
# This is an install script for Debian/Ubuntu-specific package updates.
#
set -ex

APT_FLAGS=(-q -oDpkg::Use-Pty=0)

export DEBIAN_FRONTEND=noninteractive

apt-get ${APT_FLAGS[@]} update
apt-get ${APT_FLAGS[@]} dist-upgrade -y

exit 0
