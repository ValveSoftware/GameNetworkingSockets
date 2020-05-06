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

add_definitions( -DVALVE_CRYPTO_ENABLE_25519 )
if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
	add_definitions(
		-D_CRT_SECURE_NO_WARNINGS
		-D_CRT_NONSTDC_NO_WARNINGS
		)
endif()

option(Protobuf_USE_STATIC_LIBS "Build with a static Protobuf library" OFF)
option(LIGHT_TESTS "Use smaller/shorter tests for simple integration testing (e.g. Travis)" OFF)
option(GAMENETWORKINGSOCKETS_BUILD_EXAMPLES "Build the included example chat program" ON)
option(GAMENETWORKINGSOCKETS_BUILD_TESTS "Build crypto, pki and network connection tests" ON)

#
# Primary crypto library (for AES, SHA256, etc)
#
set(useCryptoOptions OpenSSL libsodium BCrypt)
set(USE_CRYPTO "OpenSSL" CACHE STRING "Crypto library to use for AES/SHA256")
set_property(CACHE USE_CRYPTO PROPERTY STRINGS ${useCryptoOptions})

list(FIND useCryptoOptions ${USE_CRYPTO} useCryptoIndex)
if(useCryptoIndex EQUAL -1)
	message(FATAL_ERROR "USE_CRYPTO must be one of: ${useCryptoOptions}")
endif()
if(USE_CRYPTO STREQUAL "BCrypt" AND NOT WIN32)
	message(FATAL_ERROR "USE_CRYPTO=\"BCrypt\" is only valid on Windows")
endif()

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
	if (NOT BCRYPT_AVAILABLE AND USE_CRYPTO STREQUAL "BCrypt")
		message(FATAL_ERROR "You're on Windows but BCrypt seems to be unavailable, you will need OpenSSL")
	endif()
endif()

if (USE_CRYPTO STREQUAL "BCrypt")
	set(useCrypto25519Default "Reference")
else()
	set(useCrypto25519Default "OpenSSL")
endif()

#
# Secondary crypto library (for ed25519/curve25519).
#
set(useCrypto25519Options OpenSSL libsodium Reference)
set(USE_CRYPTO25519 "${useCrypto25519Default}" CACHE STRING "Crypto library to use for ed25519/curve25519")
set_property(CACHE USE_CRYPTO25519 PROPERTY STRINGS ${useCrypto25519Options})

list(FIND useCrypto25519Options ${USE_CRYPTO25519} useCrypto25519Index)
if(useCrypto25519Index EQUAL -1)
	message(FATAL_ERROR "USE_CRYPTO25519 must be one of: ${useCrypto25519Options}")
endif()

if (USE_CRYPTO25519 STREQUAL "OpenSSL" OR USE_CRYPTO STREQUAL "OpenSSL")
	find_package(OpenSSL REQUIRED)
	message( STATUS "OPENSSL_INCLUDE_DIR = ${OPENSSL_INCLUDE_DIR}" )

	# Ensure the OpenSSL version is recent enough. We need a bunch of EVP
	# functionality.
	cmake_push_check_state()
	set(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
	set(CMAKE_REQUIRED_LIBRARIES OpenSSL::Crypto)
	check_symbol_exists(EVP_MD_CTX_free openssl/evp.h OPENSSL_NEW_ENOUGH)
	if (NOT OPENSSL_NEW_ENOUGH)
		message(FATAL_ERROR "Cannot find EVP_MD_CTX_free in OpenSSL headers/libs for the target architecture.  Check that you're using OpenSSL 1.1.0 or later.")
	endif()
	cmake_pop_check_state()
	cmake_push_check_state()
	set(CMAKE_REQUIRED_LIBRARIES OpenSSL::Crypto)
	if(USE_CRYPTO25519 STREQUAL "OpenSSL")
		check_symbol_exists(EVP_PKEY_get_raw_public_key openssl/evp.h OPENSSL_HAS_25519_RAW)
	endif()
	cmake_pop_check_state()
endif()

if(USE_CRYPTO25519 STREQUAL "OpenSSL" AND NOT OPENSSL_HAS_25519_RAW)
	message(FATAL_ERROR "Cannot find (EVP_PKEY_get_raw_public_key in OpenSSL headers/libs for the target architecture.  Please use -DUSE_CRYPTO25519=Reference or upgrade OpenSSL to 1.1.1 or later")
endif()

if(USE_CRYPTO STREQUAL "libsodium" OR USE_CRYPTO25519 STREQUAL "libsodium")
	find_package(sodium REQUIRED)
endif()

if(USE_CRYPTO STREQUAL "libsodium")
	if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "amd64.*|x86_64.*|AMD64.*|i686.*|i386.*|x86.*")
		message(FATAL_ERROR "-DUSE_CRYPTO=libsodium invalid, libsodium AES implementation only works on x86/x86_64 CPUs")
	endif()
endif()

if(GAMENETWORKINGSOCKETS_BUILD_EXAMPLES)
	add_subdirectory(examples)
endif()

if(GAMENETWORKINGSOCKETS_BUILD_TESTS)
	add_subdirectory(tests)
endif()

add_subdirectory(src)

message(STATUS "---------------------------------------------------------")
message(STATUS "Crypto library for AES/SHA256: ${USE_CRYPTO}")
message(STATUS "Crypto library for ed25519/curve25519: ${USE_CRYPTO25519}")
message(STATUS "---------------------------------------------------------")

# vim: set ts=4 sts=4 sw=4 noet:
