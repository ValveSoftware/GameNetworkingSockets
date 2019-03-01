cmake_minimum_required(VERSION 3.5)

include(CMakeDependentOption)
include(CMakePushCheckState)
include(CheckSymbolExists)

project(GameNetworkingSockets C CXX)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include(DefaultBuildType)
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
		-D_CRT_NONSTDC_NO_WARNINGS
		)
endif()

option(Protobuf_USE_STATIC_LIBS "Build with a static Protobuf library" OFF)
option(LIGHT_TESTS "Use smaller/shorter tests for simple integration testing (e.g. Travis)" OFF)

if (WIN32)
	#
	# Strip compiler flags which conflict with ones we explicitly set. If we don't
	# do this, then we get a warning on every file we compile for the library.
	#
	string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
	string(REPLACE "/GR" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

	#
	# Check whether BCrypt can be used with this SDK version
	#
	cmake_push_check_state()
	set(CMAKE_REQUIRED_LIBRARIES bcrypt)
	check_symbol_exists(BCryptEncrypt windows.h BCRYPT_AVAILABLE)
	cmake_pop_check_state()
	option(USE_BCRYPT "Use Windows BCrypt API for hashing and encryption" OFF)
	if (NOT BCRYPT_AVAILABLE AND USE_BCRYPT)
		message(FATAL_ERROR "You're on Windows but BCrypt seems to be unavailable, you will need OpenSSL")
	endif()
endif()

add_subdirectory(examples)
add_subdirectory(src)
add_subdirectory(tests)

# vim: set ts=4 sts=4 sw=4 noet:
