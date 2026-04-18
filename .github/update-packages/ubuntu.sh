#!/bin/bash
#
# This is an install script for Debian/Ubuntu-specific package updates.
#
set -ex

APT_FLAGS=(-q -oDpkg::Use-Pty=0)

export DEBIAN_FRONTEND=noninteractive

apt-get ${APT_FLAGS[@]} update

# Full distro upgrades are opt-in for canary/rolling CI lanes only.
if [[ "${CI_DIST_UPGRADE:-0}" == "1" ]]; then
	apt-get ${APT_FLAGS[@]} dist-upgrade -y
fi

exit 0
