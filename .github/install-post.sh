#!/bin/bash
#
# This is a post-install script common to all Docker images.
#

cmake --version
g++ --version
clang++ --version
uname -a

echo "====================="
echo "GCC predefined macros"
echo "====================="
g++ -E -dM -xc /dev/null | sort

echo "======================="
echo "Clang predefined macros"
echo "======================="
clang++ -E -dM -xc /dev/null | sort

exit 0
