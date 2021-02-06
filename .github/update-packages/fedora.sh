#!/bin/bash
#
# This is an install script for Fedora-specific package updates.
#
set -ex

if ! grep '^fastestmirror' /etc/dnf/dnf.conf; then
	echo 'fastestmirror=1' >> /etc/dnf/dnf.conf
fi

dnf clean all
dnf update -y

exit 0
