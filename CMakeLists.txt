cmake_minimum_required(VERSION 3.5)
project(GameNetworkingSockets)

find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)

set(GNS_PROTOS
  "src/common/steamnetworkingsockets_messages_certs.proto"
  "src/common/steamnetworkingsockets_messages.proto"
  "src/common/steamnetworkingsockets_messages_udp.proto"
)

set(GNS_SRCS
  "external/curve25519-donna/curve25519.c"
  "external/curve25519-donna/curve25519_VALVE_sse2.c"
  "external/ed25519-donna/ed25519_VALVE.c"
  "external/ed25519-donna/ed25519_VALVE_sse2.c"
  "external/ujson/ujson/ujson.cpp"
  "external/ujson/ujson/double-conversion.cc"
  "common/crypto.cpp"
  "common/opensslwrapper.cpp"
  "common/steamid.cpp"
  "public/minbase/minbase_common_errors.cpp"
  "steamnetworkingsockets/certtool/steamnetworkingsockets_certtool.cpp"
  "steamnetworkingsockets/clientlib/csteamnetworkingsockets.cpp"
  "steamnetworkingsockets/clientlib/steamnetworkingsockets_connections.cpp"
  "steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.cpp"
  "steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp"
  "steamnetworkingsockets/clientlib/steamnetworkingsockets_snp_debug.cpp"
  "steamnetworkingsockets/clientlib/steamnetworkingsockets_udp.cpp"
  "steamnetworkingsockets/steamnetworkingsockets_certs.cpp"
  "steamnetworkingsockets/steamnetworkingsockets_shared.cpp"
  "tier0/cpu.cpp"
  "tier0/dbg.cpp"
  "tier0/platformtime.cpp"
  "tier1/bitstring.cpp"
  "tier1/generichash.cpp"
  "tier1/netadr.cpp"
  "tier1/utlbuffer.cpp"
  "tier1/utlmemory.cpp"
  "tier1/utlstring.cpp"
  "vstdlib/strtools.cpp"
)

set(GNS_COMMON_FLAGS
  -fvisibility=hidden
  -fstack-protector-strong
  -fno-strict-aliasing
  #-Wall
  #-Wextra
  #-pedantic
  -Wno-unknown-pragmas
  -Wno-sign-compare
)

set(GNS_C_FLAGS
  -Wstrict-prototypes
)

set(GNS_CXX_FLAGS
  -fvisibility-inlines-hidden
  -Wno-reorder
  -Wno-non-virtual-dtor
)

protobuf_generate_cpp(GNS_PROTO_SRCS GNS_PROTO_HDRS ${GNS_PROTOS})

macro(gamenetworkingsockets_common GNS_TARGET)
  foreach(SRC ${GNS_SRCS})
    target_sources(${GNS_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/${SRC}")
  endforeach()

  target_sources(${GNS_TARGET} PRIVATE ${GNS_PROTO_SRCS})

  target_include_directories(${GNS_TARGET} PRIVATE
    "include"
    "src/common"
    "src/public"
    ${CMAKE_CURRENT_BINARY_DIR}

    # Only necessary if we're not using protobuf::libprotobuf
    # But that's not supported in CMake 3.5
    ${Protobuf_INCLUDE_DIRS}
  )

  target_link_libraries(${GNS_TARGET} PRIVATE
    # protobuf::libprotobuf
    ${PROTOBUF_LIBRARIES}
    OpenSSL::SSL
  )

  target_compile_definitions(${GNS_TARGET} PRIVATE
    STEAMNETWORKINGSOCKETS_OPENSOURCE
    ENABLE_CRYPTO_25519
    HAVE_OPENSSL
    ENABLE_OPENSSLCONNECTION
    CRYPTO_DISABLE_ENCRYPT_WITH_PASSWORD
  )

  include(CheckCCompilerFlag)
  include(CheckCXXCompilerFlag)

  foreach(FLAG ${GNS_COMMON_FLAGS} ${GNS_C_FLAGS})
    check_c_compiler_flag(${FLAG} FLAG_VAR)
    if(FLAG_VAR)
      target_compile_options(${GNS_TARGET} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:${FLAG}>
      )
    endif()
  endforeach()

  foreach(FLAG ${GNS_COMMON_FLAGS} ${GNS_CXX_FLAGS})
    check_cxx_compiler_flag(${FLAG} FLAG_VAR)
    if(FLAG_VAR)
      target_compile_options(${GNS_TARGET} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:${FLAG}>
      )
    endif()
  endforeach()

  if(CMAKE_SYSTEM_NAME MATCHES Linux)
    target_compile_definitions(${GNS_TARGET} PRIVATE POSIX LINUX)
  elseif(CMAKE_SYSTEM_NAME MATCHES Darwin)
    target_compile_definitions(${GNS_TARGET} PRIVATE POSIX OSX)
  elseif(CMAKE_SYSTEM_NAME MATCHES Windows)
    target_compile_definitions(${GNS_TARGET} PRIVATE WIN32)
  else()
    message(FATAL_ERROR "Could not identify your target operating system")
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU"
  OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_definitions(${GNS_TARGET} PRIVATE GNUC GNU_COMPILER)
  endif()

  set_target_properties(${GNS_TARGET} PROPERTIES
    CXX_STANDARD 11
    OUTPUT_NAME GameNetworkingSockets
  )

endmacro()

add_library(GameNetworkingSockets_shared SHARED "")
add_library(GameNetworkingSockets::shared ALIAS GameNetworkingSockets_shared)
gamenetworkingsockets_common(GameNetworkingSockets_shared)

add_library(GameNetworkingSockets_static STATIC "")
add_library(GameNetworkingSockets::static ALIAS GameNetworkingSockets_static)
gamenetworkingsockets_common(GameNetworkingSockets_static)
