include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(CheckSymbolExists)
include(CMakePushCheckState)
include(GNUInstallDirs)

find_package(Protobuf REQUIRED)
find_package(Threads REQUIRED)

set(GNS_COMMON_PROTOS
	"common/steamnetworkingsockets_messages_certs.proto"
	"common/steamnetworkingsockets_messages.proto"
	)
set(GNS_CLIENTLIB_PROTOS
	"common/steamnetworkingsockets_messages_udp.proto"
	)

set(GNS_COMMON_SRCS
	"common/steamid.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_certs.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_certstore.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_shared.cpp"
	"tier0/dbg.cpp"
	"tier0/platformtime.cpp"
	"tier0/valve_tracelogging.cpp"
	"tier1/netadr.cpp"
	"tier1/utlbuffer.cpp"
	"tier1/utlmemory.cpp"
	"tier1/ipv6text.c"
	"vstdlib/strtools.cpp"
	)

set(GNS_CLIENTLIB_SRCS
	"steamnetworkingsockets/steamnetworkingsockets_stats.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_thinker.cpp"
	"steamnetworkingsockets/clientlib/csteamnetworkingsockets.cpp"
	"steamnetworkingsockets/clientlib/csteamnetworkingmessages.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_flat.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_connections.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_p2p.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_stun.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_p2p_ice.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_udp.cpp"
)

set(GNS_CRYPTO_SRCS
	"common/crypto.cpp"
	"common/crypto_textencode.cpp"
	"common/keypair.cpp"
	)

if(USE_CRYPTO STREQUAL "BCrypt")
	set(GNS_CRYPTO_DEFINES ${GNS_CRYPTO_DEFINES} VALVE_CRYPTO_BCRYPT VALVE_CRYPTO_25519_DONNA ED25519_HASH_BCRYPT)
	set(GNS_CRYPTO_SRCS ${GNS_CRYPTO_SRCS}
		"common/crypto_bcrypt.cpp"

		# Does bcrypt have a 25519 implementation?
		# For now we'll use the reference
		"common/crypto_25519_donna.cpp"
		"external/curve25519-donna/curve25519.c"
		"external/curve25519-donna/curve25519_VALVE_sse2.c"
		"external/ed25519-donna/ed25519_VALVE.c"
		"external/ed25519-donna/ed25519_VALVE_sse2.c"
		)
endif()

if(USE_CRYPTO STREQUAL "OpenSSL")
	set(GNS_CRYPTO_DEFINES ${GNS_CRYPTO_DEFINES} VALVE_CRYPTO_OPENSSL VALVE_CRYPTO_25519_OPENSSLEVP)
	set(GNS_CRYPTO_SRCS ${GNS_CRYPTO_SRCS}
		"common/crypto_openssl.cpp"
		"common/crypto_25519_openssl.cpp"
		"common/crypto_digest_opensslevp.cpp"
		"common/crypto_symmetric_opensslevp.cpp"
		"common/opensslwrapper.cpp"
		)
endif()

if(USE_CRYPTO STREQUAL "libsodium")
	set(GNS_CRYPTO_DEFINES ${GNS_CRYPTO_DEFINES} VALVE_CRYPTO_LIBSODIUM VALVE_CRYPTO_25519_LIBSODIUM VALVE_CRYPTO_SHA1_WPA)
	set(GNS_CRYPTO_SRCS ${GNS_CRYPTO_SRCS}
		"common/crypto_libsodium.cpp"
		"common/crypto_25519_libsodium.cpp"

		# libsodium lacks SHA1 / HMAC-SHA1 implementations
		# Use a reference implementation
		"common/crypto_sha1_wpa.cpp"
		"external/sha1-wpa/sha1-internal.c"
		"external/sha1-wpa/sha1.c"
		)
endif()

macro(check_submodule SUBMODULE_PATH TESTFILE)
	if (NOT EXISTS ${TESTFILE} AND EXISTS "${CMAKE_SOURCE_DIR}/.git" )
		find_package(Git QUIET)
		if(GIT_FOUND)

			# Update submodules as needed
			message(STATUS "git submodule update --init ${SUBMODULE_PATH}")
			execute_process(COMMAND "${GIT_EXECUTABLE}" submodule update --init ${SUBMODULE_PATH}
							WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
							RESULT_VARIABLE GIT_SUBMOD_RESULT)
			if(NOT GIT_SUBMOD_RESULT EQUAL "0")
				message(FATAL_ERROR "'git submodule update --init ${SUBMODULE_PATH}' failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
			endif()
		endif()
	endif()
	if (NOT EXISTS ${TESTFILE})
		message(FATAL_ERROR "${TESTFILE} not found; submodule ${SUBMODULE_PATH} not initialized and we failed to initialized it.\nTry: 'git submodule update ${SUBMODULE_PATH}'" )
	endif()
endmacro()


# If WebRTC enabled, build the thin wrapper library
if(USE_STEAMWEBRTC)
	add_subdirectory(external/steamwebrtc)
endif(USE_STEAMWEBRTC)

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

protobuf_generate_cpp(GNS_COMMON_PROTO_SRCS GNS_COMMON_PROTO_HDRS ${GNS_COMMON_PROTOS})
protobuf_generate_cpp(GNS_CLIENTLIB_PROTO_SRCS GNS_CLIENTLIB_PROTO_HDRS ${GNS_CLIENTLIB_PROTOS})

function(gns_set_target_protobuf_properties TGT )
	target_link_libraries(${TGT} PUBLIC
		protobuf::libprotobuf
		Threads::Threads
		)
endfunction()

# Set the target properties (sources, link libs, defines) based on crypto settings
function(gns_set_target_crypto_properties TGT)
	if(USE_CRYPTO STREQUAL "OpenSSL" OR USE_CRYPTO25519 STREQUAL "OpenSSL")
		target_link_libraries(${TGT} PUBLIC
			OpenSSL::Crypto
			)
		if(WIN32 AND OPENSSL_USE_STATIC_LIBS)
			target_link_libraries(${TGT} PUBLIC
				ws2_32
				crypt32
				)
		endif()
	endif()

	if(USE_CRYPTO STREQUAL "libsodium" OR USE_CRYPTO25519 STREQUAL "libsodium")
		target_link_libraries(${TGT} PUBLIC
			sodium
			)
	endif()

	target_compile_definitions(${TGT} PRIVATE
		ENABLE_OPENSSLCONNECTION # !KLUDGE! This is for some weird 25519 thing, somebody using the wrong define
		${GNS_CRYPTO_DEFINES}
		)

	target_sources(${TGT} PRIVATE ${GNS_CRYPTO_SRCS})

	if(SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_definitions(${TGT} PRIVATE ED25519_NO_INLINE_ASM)
	endif()
endfunction()

# Set properties on a clientlib target (either shared or static library)
macro(set_clientlib_target_properties GNS_TARGET)

	target_sources(${GNS_TARGET} PRIVATE
		${GNS_COMMON_SRCS}
		${GNS_CLIENTLIB_SRCS}
		${GNS_COMMON_PROTO_SRCS}
		${GNS_CLIENTLIB_PROTO_SRCS})

	set_target_common_gns_properties( ${GNS_TARGET} )

	set_target_properties(${GNS_TARGET} PROPERTIES
		CXX_EXTENSIONS ON
	)

	target_include_directories(${GNS_TARGET} PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>"
		"$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}/GameNetworkingSockets>"
		)

	target_include_directories(${GNS_TARGET} PRIVATE
		"common"
		"public"
		${CMAKE_CURRENT_BINARY_DIR}
		)

	gns_set_target_protobuf_properties(${GNS_TARGET})
	gns_set_target_crypto_properties(${GNS_TARGET})

	target_compile_definitions(${GNS_TARGET} PRIVATE
		STEAMNETWORKINGSOCKETS_FOREXPORT )

	# Enable ICE?
	if(ENABLE_ICE)
		target_compile_definitions(${GNS_TARGET} PRIVATE STEAMNETWORKINGSOCKETS_ENABLE_ICE )
	endif()

	# Enable WebRTC as ICE client?
	if(USE_STEAMWEBRTC)
		if(NOT ENABLE_ICE)
			message(FATAL_ERROR "USE_STEAMWEBRTC requires ENABLE_ICE")
		endif()

		# Wrapper lib is always linked statically in the opensource code.
		# We might link dynamically in other environments.
		target_compile_definitions(${GNS_TARGET} PRIVATE
			STEAMWEBRTC_USE_STATIC_LIBS
			)
		target_link_libraries(${GNS_TARGET} PUBLIC
			$<BUILD_INTERFACE:steamwebrtc>
			$<INSTALL_INTERFACE:GameNetworkingSockets::steamwebrtc>
			)
	endif()

	target_compile_features(${GNS_TARGET} PUBLIC c_std_99 cxx_std_17)

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
	elseif(CMAKE_SYSTEM_NAME MATCHES FreeBSD)

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
		target_link_libraries(${GNS_TARGET} PUBLIC ws2_32 crypt32 winmm Iphlpapi)
		if(USE_CRYPTO STREQUAL "BCrypt")
			target_link_libraries(${GNS_TARGET} PUBLIC bcrypt)
		endif()
	else()
		message(FATAL_ERROR "Could not identify your target operating system")
	endif()

	add_sanitizers(${GNS_TARGET})

	install(
		TARGETS ${GNS_TARGET}
		EXPORT GameNetworkingSockets
		LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
		ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
		)

endmacro()

if (BUILD_SHARED_LIB)
	add_library(GameNetworkingSockets SHARED "")
	add_library(GameNetworkingSockets::GameNetworkingSockets ALIAS GameNetworkingSockets)
	add_library(GameNetworkingSockets::shared ALIAS GameNetworkingSockets)
	set_clientlib_target_properties(GameNetworkingSockets)
endif()

if (BUILD_STATIC_LIB)
	add_library(GameNetworkingSockets_s STATIC "")
	add_library(GameNetworkingSockets::GameNetworkingSockets_s ALIAS GameNetworkingSockets_s)
	add_library(GameNetworkingSockets::static ALIAS GameNetworkingSockets_s)
	target_compile_definitions(GameNetworkingSockets_s INTERFACE STEAMNETWORKINGSOCKETS_STATIC_LINK)
	set_clientlib_target_properties(GameNetworkingSockets_s)
endif()

#
# Cert tool
#
if (BUILD_TOOLS)

	# The cert tool requires picojson.
	set(picojson_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/external/picojson)
	check_submodule( ${picojson_SOURCE_DIR} ${picojson_SOURCE_DIR}/picojson.h )

	add_executable(steamnetworkingsockets_certtool
		"steamnetworkingsockets/certtool/steamnetworkingsockets_certtool.cpp"
		)
	target_sources(steamnetworkingsockets_certtool PRIVATE
		${GNS_COMMON_SRCS}
		${GNS_COMMON_PROTO_SRCS})

	target_compile_definitions(steamnetworkingsockets_certtool PUBLIC STEAMNETWORKINGSOCKETS_STATIC_LINK)
	set_target_common_gns_properties( steamnetworkingsockets_certtool )

	target_include_directories(steamnetworkingsockets_certtool PUBLIC
		"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>"
		"$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_INCLUDEDIR}/GameNetworkingSockets>"
		${picojson_SOURCE_DIR}
		)

	target_include_directories(steamnetworkingsockets_certtool PRIVATE
		"common"
		"public"
		${CMAKE_CURRENT_BINARY_DIR}
		)

	gns_set_target_protobuf_properties(steamnetworkingsockets_certtool)
	gns_set_target_crypto_properties(steamnetworkingsockets_certtool)

	install(TARGETS steamnetworkingsockets_certtool DESTINATION ${CMAKE_INSTALL_BINDIR} )

endif()

install(DIRECTORY ../include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/GameNetworkingSockets)

# Export file required for find_package(GameNetworkingSockets CONFIG) to work
install(
	EXPORT GameNetworkingSockets
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GameNetworkingSockets
	NAMESPACE GameNetworkingSockets::
	)

include(CMakePackageConfigHelpers)

# Ensure that variables used in GameNetworkingSocketsConfig.cmake.in have some value
# rather than an empty string.
if(NOT USE_CRYPTO)
	set(USE_CRYPTO USE_CRYPTO-NOTFOUND)
endif()

if(NOT STEAMWEBRTC_ABSL_SOURCE)
	set(STEAMWEBRTC_ABSL_SOURCE STEAMWEBRTC_ABSL_SOURCE-NOTFOUND)
endif()

configure_package_config_file(../cmake/GameNetworkingSocketsConfig.cmake.in
	${CMAKE_CURRENT_BINARY_DIR}/GameNetworkingSocketsConfig.cmake
	INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GameNetworkingSockets
	PATH_VARS CMAKE_INSTALL_FULL_INCLUDEDIR
	)

install(FILES 
	${CMAKE_CURRENT_BINARY_DIR}/GameNetworkingSocketsConfig.cmake
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GameNetworkingSockets
	)

