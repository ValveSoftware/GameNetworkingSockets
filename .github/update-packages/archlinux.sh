#!/bin/bash
#
# This is an install script for Arch Linux-specific package updates.
#
set -ex

pacman --noconfirm -Syu

exit 0
