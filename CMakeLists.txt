cmake_minimum_required(VERSION 3.9)

# If vcpkg present as submodule, bring in the toolchain
if( EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake )
	message(STATUS "Found ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake; using it!")
	set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake
		CACHE STRING "Vcpkg toolchain file")
endif()

include(CheckIPOSupported)
include(CMakeDependentOption)
include(CMakePushCheckState)
include(CheckSymbolExists)

# CMP0069: INTERPROCEDURAL_OPTIMIZATION is enforced when enabled.
# This variable is needed for abseil, which has a different
# cmake_minimum_required version set (3.5).
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

# Put all the output from all projects into the same folder
#set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

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

option(LTO "Enable Link-Time Optimization" OFF)
option(USE_STEAMWEBRTC "Build Google's WebRTC library to get ICE support for P2P" OFF)
option(Protobuf_USE_STATIC_LIBS "Link with protobuf statically" OFF)
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

if(LTO)
	check_ipo_supported()
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
		if(WIN32 AND OPENSSL_USE_STATIC_LIBS)
			list(APPEND CMAKE_REQUIRED_LIBRARIES ws2_32 crypt32)
		endif()
		check_symbol_exists(EVP_MD_CTX_free openssl/evp.h OPENSSL_NEW_ENOUGH)
		if (NOT OPENSSL_NEW_ENOUGH)
			message(FATAL_ERROR "Cannot find EVP_MD_CTX_free in OpenSSL headers/libs for the target architecture.  Check that you're using OpenSSL 1.1.0 or later.")
		endif()
	cmake_pop_check_state()
	cmake_push_check_state()
		set(CMAKE_REQUIRED_LIBRARIES OpenSSL::Crypto)
		if(WIN32 AND OPENSSL_USE_STATIC_LIBS)
			list(APPEND CMAKE_REQUIRED_LIBRARIES ws2_32 crypt32)
		endif()
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

# We always need at least sse2 on x86
if(CMAKE_SYSTEM_PROCESSOR MATCHES "amd64.*|x86_64.*|AMD64.*|i686.*|i386.*|x86.*")
       set(TARGET_ARCH_FLAGS "-msse2")
endif()

function(set_target_common_gns_properties TGT)
	target_compile_definitions( ${TGT} PRIVATE GOOGLE_PROTOBUF_NO_RTTI )

	if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		# Reduce binary size by allowing for a pseudo-"function-level linking" analog
		target_compile_options(${TGT} PRIVATE -ffunction-sections -fdata-sections ${TARGET_ARCH_FLAGS})
	endif()

	if(CMAKE_SYSTEM_NAME MATCHES Linux)
		target_compile_definitions(${TGT} PUBLIC POSIX LINUX)
	elseif(CMAKE_SYSTEM_NAME MATCHES Darwin)
		target_compile_definitions(${TGT} PUBLIC POSIX OSX)
	elseif(CMAKE_SYSTEM_NAME MATCHES Windows)
		#target_compile_definitions(${TGT} PUBLIC WIN32)
		if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
			if(NOT Protobuf_USE_STATIC_LIBS)
				target_compile_definitions(${TGT} PRIVATE PROTOBUF_USE_DLLS)
			endif()
			target_compile_options(${TGT} PRIVATE
				/EHs-c-   # Disable C++ exceptions

				# Below are warnings we can't fix and don't want to see (mostly from protobuf, some from MSVC standard library)
				/wd4146   # include/google/protobuf/wire_format_lite.h(863): warning C4146: unary minus operator applied to unsigned type, result still unsigned
				/wd4530   # .../xlocale(319): warning C4530: C++ exception handler used, but unwind semantics are not enabled. Specify /EHsc
				/wd4244   # google/protobuf/wire_format_lite.h(935): warning C4244: 'argument': conversion from 'google::protobuf::uint64' to 'google::protobuf::uint32', possible loss of data
				/wd4251   # 'google::protobuf::io::CodedOutputStream::default_serialization_deterministic_': struct 'std::atomic<bool>' needs to have dll-interface to be used by clients of class 
				/wd4267   # google/protobuf/has_bits.h(73): warning C4267: 'argument': conversion from 'size_t' to 'int', possible loss of data
				)

			# Disable RTTI except in Debug, because we use dynamic_cast in assert_cast
			target_compile_options(${TGT} PRIVATE $<IF:$<CONFIG:Debug>,/GR,/GR->)
		else()
			target_compile_definitions(${TGT} PRIVATE
				__STDC_FORMAT_MACROS=1
				__USE_MINGW_ANSI_STDIO=0
				)
			target_compile_options(${TGT} PRIVATE -fno-stack-protector)
		endif()
	else()
		message(FATAL_ERROR "Could not identify your target operating system")
	endif()

	if(NOT CMAKE_SYSTEM_NAME MATCHES Windows)
		target_compile_options(${TGT} PRIVATE -fstack-protector-strong)
	endif()

	if(LTO)
		set_target_properties(${TGT} PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
	endif()

	set_target_properties(${TGT} PROPERTIES
		CXX_STANDARD 11
	)
endfunction()

if(GAMENETWORKINGSOCKETS_BUILD_EXAMPLES)
	add_subdirectory(examples)
endif()

if(GAMENETWORKINGSOCKETS_BUILD_TESTS)
	add_subdirectory(tests)
endif()

add_subdirectory(src)

#message(STATUS "---------------------------------------------------------")
message(STATUS "Crypto library for AES/SHA256: ${USE_CRYPTO}")
message(STATUS "Crypto library for ed25519/curve25519: ${USE_CRYPTO25519}")
message(STATUS "Link-time optimization: ${LTO}")
#message(STATUS "---------------------------------------------------------")
