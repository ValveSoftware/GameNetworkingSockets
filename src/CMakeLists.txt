include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(CheckSymbolExists)
include(CMakePushCheckState)
include(GNUInstallDirs)

find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

set(GNS_PROTOS
	"common/steamnetworkingsockets_messages_certs.proto"
	"common/steamnetworkingsockets_messages.proto"
	"common/steamnetworkingsockets_messages_udp.proto"
	)

set(GNS_SRCS
	"common/crypto_textencode.cpp"
	"common/keypair.cpp"
	"common/steamid.cpp"
	"steamnetworkingsockets/clientlib/csteamnetworkingsockets.cpp"
	"steamnetworkingsockets/clientlib/csteamnetworkingmessages.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_flat.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_connections.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_p2p.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_p2p_ice.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_udp.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_certs.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_certstore.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_shared.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_stats.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_thinker.cpp"
	"tier0/dbg.cpp"
	"tier0/platformtime.cpp"
	"tier1/netadr.cpp"
	"tier1/utlbuffer.cpp"
	"tier1/utlmemory.cpp"
	"tier1/ipv6text.c"
	"vstdlib/strtools.cpp"
	)

if(USE_CRYPTO STREQUAL "BCrypt")
	set(GNS_CRYPTO_DEFINES ${GNS_CRYPTO_DEFINES} STEAMNETWORKINGSOCKETS_CRYPTO_BCRYPT ED25519_HASH_BCRYPT)
	set(GNS_SRCS ${GNS_SRCS}
		"common/crypto_bcrypt.cpp"
		)
endif()

if(USE_CRYPTO STREQUAL "OpenSSL")
	set(GNS_CRYPTO_DEFINES ${GNS_CRYPTO_DEFINES} STEAMNETWORKINGSOCKETS_CRYPTO_VALVEOPENSSL)
	set(GNS_SRCS ${GNS_SRCS}
		"common/crypto_openssl.cpp"
		"common/opensslwrapper.cpp"
		)
endif()

if(USE_CRYPTO STREQUAL "libsodium")
	set(GNS_CRYPTO_DEFINES ${GNS_CRYPTO_DEFINES} STEAMNETWORKINGSOCKETS_CRYPTO_LIBSODIUM)
	set(GNS_SRCS ${GNS_SRCS}
		"common/crypto_libsodium.cpp")
endif()

# Use OpenSSL for 25519 if possible
if(USE_CRYPTO25519 STREQUAL "OpenSSL")
	set(GNS_CRYPTO_DEFINES  ${GNS_CRYPTO_DEFINES} STEAMNETWORKINGSOCKETS_CRYPTO_25519_OPENSSL)
	set(GNS_SRCS ${GNS_SRCS}
		"common/crypto_25519_openssl.cpp"
		)
endif()

if(USE_CRYPTO25519 STREQUAL "libsodium")
	set(GNS_CRYPTO_DEFINES ${GNS_CRYPTO_DEFINES} STEAMNETWORKINGSOCKETS_CRYPTO_25519_LIBSODIUM)
	set(GNS_SRCS ${GNS_SRCS}
		"common/crypto_25519_libsodium.cpp"
		)
endif()

# Use reference 25519 crypto implementation?
if(USE_CRYPTO25519 STREQUAL "Reference")
	set(GNS_CRYPTO_DEFINES ${GNS_CRYPTO_DEFINES} VALVE_CRYPTO_25519_DONNA)
	set(GNS_SRCS ${GNS_SRCS}
		"common/crypto_25519_donna.cpp"
		"external/curve25519-donna/curve25519.c"
		"external/curve25519-donna/curve25519_VALVE_sse2.c"
		"external/ed25519-donna/ed25519_VALVE.c"
		"external/ed25519-donna/ed25519_VALVE_sse2.c"
		)
endif()

# If WebRTC enabled, build the thin wrapper library
if(USE_STEAMWEBRTC)
	add_subdirectory(external/steamwebrtc)
endif(USE_STEAMWEBRTC)

set(C99_FEATURES
	c_restrict
	c_function_prototypes)

set(CXX11_FEATURES
	cxx_constexpr
	cxx_auto_type)

set(GNS_COMMON_FLAGS
	-fvisibility=hidden
	-fno-strict-aliasing
	-Wall
	-Wno-unknown-pragmas
	-Wno-sign-compare
	-Wno-unused-local-typedef
	-Wno-unused-const-variable
	-Wno-nested-anon-types
	-Wno-format-truncation
	)

if(USE_CRYPTO25519 STREQUAL "Reference")
	# We don't use some of the 25519 functions with static linkage. Silence
	# -Wunused-function if we're including the reference ed25519/curve25519
	# stuff.
	set(GNS_COMMON_FLAGS ${GNS_COMMON_FLAGS} -Wno-unused-function)
endif()

if(WERROR)
	set(GNS_COMMON_FLAGS
		${GNS_COMMON_FLAGS}
		-Werror)
endif()

set(GNS_C_FLAGS
	-Wstrict-prototypes
	)

set(GNS_CXX_FLAGS
	-fvisibility-inlines-hidden
	-Wno-reorder
	-Wno-non-virtual-dtor
	-Wno-zero-as-null-pointer-constant
	-fno-exceptions
	)

if(NOT SANITIZE_UNDEFINED)
	set(GNS_CXX_FLAGS
		${GNS_CXX_FLAGS}
		-fno-rtti
		)
endif()

protobuf_generate_cpp(GNS_PROTO_SRCS GNS_PROTO_HDRS ${GNS_PROTOS})

macro(gamenetworkingsockets_common GNS_TARGET)

	target_sources(${GNS_TARGET} PRIVATE ${GNS_SRCS})
	target_sources(${GNS_TARGET} PRIVATE ${GNS_PROTO_SRCS})

	set_target_common_gns_properties( ${GNS_TARGET} )

	target_include_directories(${GNS_TARGET} PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>"
		"$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}/GameNetworkingSockets>"
		)

	target_include_directories(${GNS_TARGET} PRIVATE
		"common"
		"public"
		${CMAKE_CURRENT_BINARY_DIR}

		# Only necessary if we're not using protobuf::libprotobuf
		# But that's not supported until CMake 3.9
		${Protobuf_INCLUDE_DIRS}
		)

	target_link_libraries(${GNS_TARGET} PUBLIC
		# protobuf::libprotobuf
		${PROTOBUF_LIBRARIES}
		Threads::Threads
		)

	# Enable ICE?
	if(USE_STEAMWEBRTC)

		# Enable ICE
		target_compile_definitions(${GNS_TARGET} PRIVATE
			STEAMNETWORKINGSOCKETS_ENABLE_ICE )

		# Wrapper lib is always linked statically in the opensource code.
		# We might link dynamically in other environments.
		target_compile_definitions(${GNS_TARGET} PRIVATE
			STEAMWEBRTC_USE_STATIC_LIBS
			)
		target_link_libraries(${GNS_TARGET} PUBLIC
			steamwebrtc
			)
	endif()

	if(USE_CRYPTO STREQUAL "OpenSSL" OR USE_CRYPTO25519 STREQUAL "OpenSSL")
		target_link_libraries(${GNS_TARGET} PUBLIC
			OpenSSL::Crypto
			)
		if(WIN32 AND OPENSSL_USE_STATIC_LIBS)
			target_link_libraries(${GNS_TARGET} PUBLIC
				ws2_32
				crypt32
				)
		endif()
	endif()

	if(USE_CRYPTO STREQUAL "libsodium" OR USE_CRYPTO25519 STREQUAL "libsodium")
		target_link_libraries(${GNS_TARGET} PUBLIC
			sodium
			)
	endif()

	target_compile_definitions(${GNS_TARGET} PRIVATE
		STEAMNETWORKINGSOCKETS_FOREXPORT
		ENABLE_OPENSSLCONNECTION
		CRYPTO_DISABLE_ENCRYPT_WITH_PASSWORD
		${GNS_CRYPTO_DEFINES}
		)

	if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_definitions(${GNS_TARGET} PRIVATE ED25519_NO_INLINE_ASM)
	endif()

	## Needs CMake 3.8, then we could get rid of the workaround below it.
	#target_compile_features(${GNS_TARGET} PUBLIC c_std_99 cxx_std_11)
	if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
		target_compile_features(${GNS_TARGET} PUBLIC ${C99_FEATURES} ${CXX11_FEATURES})
	endif()

	if(CMAKE_CXX_COMPILER_ID MATCHES "GNU"
			OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		foreach(FLAG ${GNS_COMMON_FLAGS} ${GNS_C_FLAGS})
			string(MAKE_C_IDENTIFIER ${FLAG} FLAG_ID)
			check_c_compiler_flag(${FLAG} ${FLAG_ID}_TEST)
			if(${FLAG_ID}_TEST)
				target_compile_options(${GNS_TARGET} PRIVATE
					$<$<COMPILE_LANGUAGE:C>:${FLAG}>)
			endif()
		endforeach()

		foreach(FLAG ${GNS_COMMON_FLAGS} ${GNS_CXX_FLAGS})
			string(MAKE_C_IDENTIFIER ${FLAG} FLAG_ID)
			check_cxx_compiler_flag(${FLAG} ${FLAG_ID}_TEST)
			if(${FLAG_ID}_TEST)
				target_compile_options(${GNS_TARGET} PRIVATE
					$<$<COMPILE_LANGUAGE:CXX>:${FLAG}>
					)
			endif()
		endforeach()
	endif()

	if(CMAKE_SYSTEM_NAME MATCHES Linux)
		#if(USE_STEAMWEBRTC AND NOT STEAMWEBRTC_USE_STATIC_LIBS)
		#	target_link_libraries(${GNS_TARGET} PRIVATE dl)
		#endif()
	elseif(CMAKE_SYSTEM_NAME MATCHES Darwin)
		#if(USE_STEAMWEBRTC AND NOT STEAMWEBRTC_USE_STATIC_LIBS)
		#	target_link_libraries(${GNS_TARGET} PRIVATE dl)
		#endif()
	elseif(CMAKE_SYSTEM_NAME MATCHES Windows)
		if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
			get_target_property(TARGET_TYPE ${GNS_TARGET} TYPE)
			if(NOT TARGET_TYPE STREQUAL STATIC_LIBRARY)
				target_compile_options(${GNS_TARGET} PRIVATE
					/GL       # Enable link-time code generation
					)
				set_target_properties(${GNS_TARGET} PROPERTIES LINK_FLAGS "/LTCG /SUBSYSTEM:WINDOWS")
			endif()
		endif()
		target_link_libraries(${GNS_TARGET} PUBLIC ws2_32 crypt32)
		if(USE_CRYPTO STREQUAL "BCrypt")
			target_link_libraries(${GNS_TARGET} PUBLIC bcrypt)
		endif()
	else()
		message(FATAL_ERROR "Could not identify your target operating system")
	endif()

	add_sanitizers(${GNS_TARGET})

endmacro()

add_library(GameNetworkingSockets SHARED "")
add_library(GameNetworkingSockets::GameNetworkingSockets ALIAS GameNetworkingSockets)
add_library(GameNetworkingSockets::shared ALIAS GameNetworkingSockets)
gamenetworkingsockets_common(GameNetworkingSockets)

add_library(GameNetworkingSockets_s STATIC "")
add_library(GameNetworkingSockets::GameNetworkingSockets_s ALIAS GameNetworkingSockets_s)
add_library(GameNetworkingSockets::static ALIAS GameNetworkingSockets_s)
target_compile_definitions(GameNetworkingSockets_s INTERFACE STEAMNETWORKINGSOCKETS_STATIC_LINK)
gamenetworkingsockets_common(GameNetworkingSockets_s)

# Install rules

install(
	TARGETS 
		GameNetworkingSockets
		GameNetworkingSockets_s
	EXPORT GameNetworkingSockets
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	)

install(DIRECTORY ../include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/GameNetworkingSockets)

# Export doesn't work if we're using WebRTC and the Abseil dependency came from the submodule
if(NOT (USE_STEAMWEBRTC AND STEAMWEBRTC_ABSL_SOURCE STREQUAL submodule))
	install(
		EXPORT GameNetworkingSockets
		DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GameNetworkingSockets
		NAMESPACE GameNetworkingSockets::
		)

	include(CMakePackageConfigHelpers)

	configure_package_config_file(../cmake/GameNetworkingSocketsConfig.cmake.in
		${CMAKE_CURRENT_BINARY_DIR}/GameNetworkingSocketsConfig.cmake
		INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GameNetworkingSockets
		PATH_VARS CMAKE_INSTALL_FULL_INCLUDEDIR
		)

	install(FILES 
		${CMAKE_CURRENT_BINARY_DIR}/GameNetworkingSocketsConfig.cmake
		DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GameNetworkingSockets
		)
endif()

# vim: set ts=4 sts=4 sw=4 noet:
