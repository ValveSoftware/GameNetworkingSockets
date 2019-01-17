#!/bin/bash
#
# This is a post-install script common to all Docker images.
#
cmake --version
meson --version
g++ --version
clang++ --version
exit 0
