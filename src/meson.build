add_global_arguments('-fvisibility=hidden', language: 'cpp')
add_global_arguments('-fvisibility-inlines-hidden', language: 'cpp')

dependencies = []

c_compiler = meson.get_compiler('c')
cxx_compiler = meson.get_compiler('cpp')

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

flags_common = [
  '-fno-strict-aliasing',
]

flags_cxx = [
  '-fno-rtti',
  '-fno-exceptions',
]

warn_flags_common = [
  '-Wall',
  #'-Wextra',
  #'-pedantic',
  '-Wno-unknown-pragmas',
  '-Wno-sign-compare',
]

warn_flags_c = [
  '-Wstrict-prototypes',
]

warn_flags_cxx = [
  '-Wno-reorder',
  '-Wno-non-virtual-dtor',
]

cpp_flags = [
  '-DSTEAMNETWORKINGSOCKETS_OPENSOURCE',
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
  flags_common += [
    '-fno-stack-protector',
  ]
  dependencies += [
	c_compiler.find_library('ws2_32'),
  ]
else
  error('Could not identify your target operating system')
endif

if target_os != 'windows'
  flags_common += [ '-fstack-protector-strong' ]
endif

c_flags = []
foreach arg : warn_flags_c + flags_common + warn_flags_common
  if c_compiler.has_argument(arg)
	c_flags += [ arg ]
  endif
endforeach

cxx_flags = []
cxx_flags += cxx_compiler.first_supported_argument(['-std=c++11', '-std=c++0x'])
foreach arg : warn_flags_cxx + flags_common + flags_cxx + warn_flags_common
  if cxx_compiler.has_argument(arg)
	cxx_flags += [ arg ]
  endif
endforeach

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
  'steamnetworkingsockets/clientlib/steamnetworkingsockets_connections.cpp',
  'steamnetworkingsockets/clientlib/steamnetworkingsockets_lowlevel.cpp',
  'steamnetworkingsockets/clientlib/steamnetworkingsockets_snp.cpp',
  'steamnetworkingsockets/clientlib/steamnetworkingsockets_snp_debug.cpp',
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
  c_args: c_flags + cpp_flags,
  cpp_args: cxx_flags + cpp_flags,
  include_directories: incdirs,
  dependencies: dependencies)

library('GameNetworkingSockets',
  sources, protobufs,
  c_args: c_flags + cpp_flags,
  cpp_args: cxx_flags + cpp_flags,
  include_directories: incdirs,
  dependencies: dependencies)
