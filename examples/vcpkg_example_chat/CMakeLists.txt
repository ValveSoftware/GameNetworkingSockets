#
# Example porject that uses GameNetworkingSockets, obtaining
# dependencies, etc entirely through vcpkg.
#
cmake_minimum_required(VERSION 3.10)
project( example_chat_vcpkg )

# Ask vcpkg to fetch gamenetworkingsockets and all
# its dependencies and build it
find_package(GameNetworkingSockets CONFIG REQUIRED)

# Our program only has one cpp file
add_executable(
	example_chat
	../example_chat.cpp)

# Link against shared or static depending on how vcpkg built the library.
# Pass -DGNS_LINK_STATIC=ON to link statically (e.g. with x64-windows-static triplet).
option(GNS_LINK_STATIC "Link GameNetworkingSockets statically" OFF)
if(GNS_LINK_STATIC)
	target_link_libraries(example_chat GameNetworkingSockets::static)
else()
	target_link_libraries(example_chat GameNetworkingSockets::shared)
endif()
