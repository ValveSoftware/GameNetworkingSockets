find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

set(GNS_PROTOS
	"common/steamnetworkingsockets_messages_certs.proto"
	"common/steamnetworkingsockets_messages.proto"
	"common/steamnetworkingsockets_messages_udp.proto"
)

set(GNS_SRCS
	"external/curve25519-donna/curve25519.c"
	"external/curve25519-donna/curve25519_VALVE_sse2.c"
	"external/ed25519-donna/ed25519_VALVE.c"
	"external/ed25519-donna/ed25519_VALVE_sse2.c"
	"common/crypto.cpp"
	"common/opensslwrapper.cpp"
	"common/steamid.cpp"
	"public/minbase/minbase_common_errors.cpp"
	"steamnetworkingsockets/certtool/steamnetworkingsockets_certtool.cpp"
	"steamnetworkingsockets/clientlib/csteamnetworkingsockets.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_flat.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_connections.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp"
	"steamnetworkingsockets/clientlib/steamnetworkingsockets_udp.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_certs.cpp"
	"steamnetworkingsockets/steamnetworkingsockets_shared.cpp"
	"tier0/cpu.cpp"
	"tier0/dbg.cpp"
	"tier0/platformtime.cpp"
	"tier1/netadr.cpp"
	"tier1/utlbuffer.cpp"
	"tier1/utlmemory.cpp"
	"vstdlib/strtools.cpp"
)

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
	#-Wextra
	#-pedantic
	-Wno-unknown-pragmas
	-Wno-sign-compare
	-Wno-unused-local-typedef
	-Wno-unused-const-variable
)

set(GNS_C_FLAGS
	-Wstrict-prototypes
)

set(GNS_CXX_FLAGS
	-fvisibility-inlines-hidden
	-Wno-reorder
	-Wno-non-virtual-dtor
	-fno-rtti
	-fno-exceptions
)

protobuf_generate_cpp(GNS_PROTO_SRCS GNS_PROTO_HDRS ${GNS_PROTOS})

macro(gamenetworkingsockets_common GNS_TARGET)

	target_sources(${GNS_TARGET} PRIVATE ${GNS_SRCS})
	target_sources(${GNS_TARGET} PRIVATE ${GNS_PROTO_SRCS})

	target_include_directories(${GNS_TARGET} PUBLIC
		"../include"
	)

	target_include_directories(${GNS_TARGET} PRIVATE
		"common"
		"public"
		${CMAKE_CURRENT_BINARY_DIR}

		# Only necessary if we're not using protobuf::libprotobuf
		# But that's not supported until CMake 3.9
		${Protobuf_INCLUDE_DIRS}
	)

	target_link_libraries(${GNS_TARGET} PRIVATE
		# protobuf::libprotobuf
		${PROTOBUF_LIBRARIES}
		OpenSSL::Crypto
		Threads::Threads
	)

	target_compile_definitions(${GNS_TARGET} PRIVATE
		STEAMDATAGRAMLIB_FOREXPORT
		STATIC_TIER0
		# needed for ed25519-donna
		ENABLE_CRYPTO_25519
		HAVE_OPENSSL
		ENABLE_OPENSSLCONNECTION
		CRYPTO_DISABLE_ENCRYPT_WITH_PASSWORD
		GOOGLE_PROTOBUF_NO_RTTI
	)

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
		target_compile_definitions(${GNS_TARGET} PRIVATE POSIX LINUX)
	elseif(CMAKE_SYSTEM_NAME MATCHES Darwin)
		target_compile_definitions(${GNS_TARGET} PRIVATE POSIX OSX)
	elseif(CMAKE_SYSTEM_NAME MATCHES Windows)
		target_compile_definitions(${GNS_TARGET} PRIVATE
			WIN32
		)
		if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
			target_compile_definitions(${GNS_TARGET} PRIVATE
				_CRT_SECURE_NO_WARNINGS
				_ITERATOR_DEBUG_LEVEL=0
				PROTOBUF_USE_DLLS
			)
			get_target_property(TARGET_TYPE ${GNS_TARGET} TYPE)
			target_compile_options(${GNS_TARGET} PRIVATE
				/EHs-c-   # Disable C++ exceptions
				/GR-      # Disable RTTI
				/wd4146   # include/google/protobuf/wire_format_lite.h(863): warning C4146: unary minus operator applied to unsigned type, result still unsigned
			)
			if(NOT TARGET_TYPE STREQUAL STATIC_LIBRARY)
				target_compile_options(${GNS_TARGET} PRIVATE
					/GL       # Enable link-time code generation
				)
				set_target_properties(${GNS_TARGET} PROPERTIES LINK_FLAGS "/LTCG /SUBSYSTEM:WINDOWS")
			endif()
		else()
			target_compile_definitions(${GNS_TARGET} PRIVATE
				__STDC_FORMAT_MACROS=1
				__USE_MINGW_ANSI_STDIO=0
			)
			target_compile_options(${GNS_TARGET} PRIVATE -fno-stack-protector)
		endif()
		target_link_libraries(${GNS_TARGET} PRIVATE ws2_32)
	else()
		message(FATAL_ERROR "Could not identify your target operating system")
	endif()

	if(NOT CMAKE_SYSTEM_NAME MATCHES Windows)
		target_compile_options(${GNS_TARGET} PRIVATE -fstack-protector-strong)
	endif()

	if(CMAKE_CXX_COMPILER_ID MATCHES "GNU"
	OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
		target_compile_definitions(${GNS_TARGET} PRIVATE GNUC GNU_COMPILER)
	endif()

	set_target_properties(${GNS_TARGET} PROPERTIES
		CXX_STANDARD 11
	)

endmacro()

add_library(GameNetworkingSockets SHARED "")
add_library(GameNetworkingSockets::shared ALIAS GameNetworkingSockets)
gamenetworkingsockets_common(GameNetworkingSockets)

add_library(GameNetworkingSockets_s STATIC "")
add_library(GameNetworkingSockets::static ALIAS GameNetworkingSockets_s)
gamenetworkingsockets_common(GameNetworkingSockets_s)
