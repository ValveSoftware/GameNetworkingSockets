#!/bin/bash
set -ex
apt-get update
apt-get install -y locales
locale-gen en_US.UTF-8
apt-get install -y build-essential pkg-config cmake meson clang libsodium-dev libssl-dev libprotobuf-dev protobuf-compiler
cmake --version
meson --version
g++ --version
clang++ --version
