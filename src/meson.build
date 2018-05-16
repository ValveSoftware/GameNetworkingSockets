dependencies = []

c_compiler = meson.get_compiler('c')

protoc_bin = find_program('protoc')
protoc = generator(protoc_bin,
  output    : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
  arguments : ['-I@CURRENT_SOURCE_DIR@/common', '--proto_path=@CURRENT_SOURCE_DIR@', '--cpp_out=@BUILD_DIR@', '@INPUT@'])

dependencies += [
  dependency('threads'),
  dependency('libcrypto'),
  dependency('protobuf'),
]

incdirs = include_directories('.', '../include', 'common', 'public')

cpp_flags = [
  '-DSTEAMDATAGRAMLIB_FOREXPORT',
  '-DSTATIC_TIER0',
  
  # needed for ed25519-donna
  '-DENABLE_CRYPTO_25519',
  '-DHAVE_OPENSSL',
  '-DENABLE_OPENSSLCONNECTION',
  '-DCRYPTO_DISABLE_ENCRYPT_WITH_PASSWORD',
  '-DGOOGLE_PROTOBUF_NO_RTTI',
]

target_os = target_machine.system()

if target_os == 'linux'
  cpp_flags += ['-DPOSIX', '-DLINUX']
elif target_os == 'darwin'
  cpp_flags += ['-DPOSIX', '-DOSX']
elif target_os == 'windows'
  cpp_flags += ['-D_WIN32', '-DWIN32', '-D__STDC_FORMAT_MACROS=1', '-D__USE_MINGW_ANSI_STDIO=0']
  dependencies += [
    c_compiler.find_library('ws2_32'),
  ]
else
  error('Could not identify your target operating system')
endif

target_compiler = c_compiler.get_id()
if target_compiler == 'gcc' or target_compiler == 'clang'
  cpp_flags += ['-DGNUC', '-DGNU_COMPILER']
endif

protobuf_sources = [
  'common/steamnetworkingsockets_messages_certs.proto',
  'common/steamnetworkingsockets_messages.proto',
  'common/steamnetworkingsockets_messages_udp.proto',
]

sources = [
  'external/curve25519-donna/curve25519.c',
  'external/curve25519-donna/curve25519_VALVE_sse2.c',
  'external/ed25519-donna/ed25519_VALVE.c',
  'external/ed25519-donna/ed25519_VALVE_sse2.c',
  'common/crypto.cpp',
  'common/opensslwrapper.cpp',
  'common/steamid.cpp',
  'public/minbase/minbase_common_errors.cpp',
  'steamnetworkingsockets/certtool/steamnetworkingsockets_certtool.cpp',
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
  'tier1/netadr.cpp',
  'tier1/utlbuffer.cpp',
  'tier1/utlmemory.cpp',
  'vstdlib/strtools.cpp',
]

protobufs = protoc.process(protobuf_sources)

static_library('GameNetworkingSockets',
  sources, protobufs,
  c_args: cpp_flags,
  cpp_args: cpp_flags,
  include_directories: incdirs,
  dependencies: dependencies)

GameNetworkingSockets_so = library('GameNetworkingSockets',
  sources, protobufs,
  c_args: cpp_flags,
  cpp_args: cpp_flags,
  include_directories: incdirs,
  dependencies: dependencies)


dep_GameNetworkingSockets_so = declare_dependency(
  include_directories: include_directories( '../include' ),
  link_with: GameNetworkingSockets_so )
