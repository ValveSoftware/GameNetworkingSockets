cmake_minimum_required(VERSION 3.5)
project(GameNetworkingSockets C CXX)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

find_package(Sanitizers)

if(SANITIZE_ADDRESS OR SANITIZE_THREAD OR SANITIZE_MEMORY OR SANITIZE_UNDEFINED)
	set(SANITIZE ON)
endif()

include(FlagsMSVC)
set(MSVC_RUNTIME "dynamic")
configure_msvc_runtime()
print_default_msvc_flags()

if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
	add_definitions(
		-D_CRT_SECURE_NO_WARNINGS
		-D_ITERATOR_DEBUG_LEVEL=0
		)
endif()

option(Protobuf_USE_STATIC_LIBS "Build with a static Protobuf library" OFF)
option(LIGHT_TESTS "Use smaller/shorter tests for simple integration testing (e.g. Travis)" OFF)

add_subdirectory(examples)
add_subdirectory(src)
add_subdirectory(tests)

# vim: set ts=4 sts=4 sw=4 noet:
