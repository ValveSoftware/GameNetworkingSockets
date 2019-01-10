cmake_minimum_required(VERSION 3.5)
project(GameNetworkingSockets C CXX)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(FlagsMSVC)
set(MSVC_RUNTIME "dynamic")
configure_msvc_runtime()
print_default_msvc_flags()

option(Protobuf_USE_STATIC_LIBS "Build with a static Protobuf library" OFF)
option(USE_LIBSODIUM "Use libsodium for ed25519/curve25519" ON)

add_subdirectory(src)
add_subdirectory(tests)
