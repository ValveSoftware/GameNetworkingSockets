dependencies = []

c_compiler = meson.get_compiler('c')

protoc_bin = find_program('protoc')
protoc = generator(protoc_bin,
  output    : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
  arguments : ['-I@CURRENT_SOURCE_DIR@/common', '--proto_path=@CURRENT_SOURCE_DIR@', '--cpp_out=@BUILD_DIR@', '@INPUT@'])

use_libsodium = get_option('use_libsodium')

dependencies += [
  dependency('threads'),
  dependency('libcrypto'),
  dependency('protobuf'),
]

if use_libsodium
  dependencies += [
	dependency('libsodium'),
  ]
endif

incdirs = include_directories('.', '../include', 'common', 'public')

cpp_flags = [
  '-DSTEAMDATAGRAMLIB_FOREXPORT',
  '-DHAVE_OPENSSL',
  '-DENABLE_OPENSSLCONNECTION',
  '-DCRYPTO_DISABLE_ENCRYPT_WITH_PASSWORD',
  '-DGOOGLE_PROTOBUF_NO_RTTI',
]

if use_libsodium
  cpp_flags += [ '-DUSE_LIBSODIUM' ]
endif

target_os = target_machine.system()

if target_os == 'windows'
  dependencies += [
    c_compiler.find_library('ws2_32'),
  ]
endif

protobuf_sources = [
  'common/steamnetworkingsockets_messages_certs.proto',
  'common/steamnetworkingsockets_messages.proto',
  'common/steamnetworkingsockets_messages_udp.proto',
]

sources = [
  'common/crypto.cpp',
  'common/crypto_25519.cpp',
  'common/crypto_25519_libsodium.cpp',
  'common/crypto_textencode.cpp',
  'common/keypair.cpp',
  'common/opensslwrapper.cpp',
  'common/steamid.cpp',
  'steamnetworkingsockets/clientlib/csteamnetworkingsockets.cpp',
  'steamnetworkingsockets/clientlib/steamnetworkingsockets_flat.cpp',
  'steamnetworkingsockets/clientlib/steamnetworkingsockets_connections.cpp',
  'steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.cpp',
  'steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp',
  'steamnetworkingsockets/clientlib/steamnetworkingsockets_udp.cpp',
  'steamnetworkingsockets/steamnetworkingsockets_certs.cpp',
  'steamnetworkingsockets/steamnetworkingsockets_shared.cpp',
  'tier0/cpu.cpp',
  'tier0/dbg.cpp',
  'tier0/platformtime.cpp',
  'tier1/bitstring.cpp',
  'tier1/ipv6text.c',
  'tier1/netadr.cpp',
  'tier1/utlbuffer.cpp',
  'tier1/utlmemory.cpp',
  'vstdlib/strtools.cpp',
]

if not use_libsodium
  sources += [
	'external/curve25519-donna/curve25519.c',
	'external/curve25519-donna/curve25519_VALVE_sse2.c',
	'external/ed25519-donna/ed25519_VALVE.c',
	'external/ed25519-donna/ed25519_VALVE_sse2.c',
  ]
endif

protobufs = protoc.process(protobuf_sources)

GameNetworkingSockets_static = static_library('GameNetworkingSockets',
  sources, protobufs,
  c_args: cpp_flags,
  cpp_args: cpp_flags,
  include_directories: incdirs,
  dependencies: dependencies)

dep_GameNetworkingSockets_static = declare_dependency(
  include_directories: include_directories( '../include' ),
  compile_args: ['-DSTEAMDATAGRAMLIB_STATIC_LINK'],
  link_with: GameNetworkingSockets_static )

GameNetworkingSockets_so = library('GameNetworkingSockets',
  sources, protobufs,
  c_args: cpp_flags,
  cpp_args: cpp_flags,
  include_directories: incdirs,
  dependencies: dependencies)

dep_GameNetworkingSockets_so = declare_dependency(
  include_directories: include_directories( '../include' ),
  link_with: GameNetworkingSockets_so )
