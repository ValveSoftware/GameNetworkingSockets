dependencies = []

c_compiler = meson.get_compiler('c')

protoc_bin = find_program('protoc')
protoc = generator(protoc_bin,
  output    : ['@BASENAME@.pb.cc', '@BASENAME@.pb.h'],
  arguments : ['-I@CURRENT_SOURCE_DIR@/common', '--proto_path=@CURRENT_SOURCE_DIR@', '--cpp_out=@BUILD_DIR@', '@INPUT@'])

use_bcrypt = get_option('use_bcrypt')

if not use_bcrypt
  min_libcrypto = dependency('libcrypto', version: '>=1.1.0')
  good_libcrypto = dependency('libcrypto', version: '>=1.1.1', required: false)

  if good_libcrypto.found()
    dependencies += [ good_libcrypto ]
  else
    dependencies += [ min_libcrypto ]
  endif

  code = '''#include <openssl/evp.h>
  int main(int argc, char **argv) {
    EVP_MD_CTX_free(NULL);
    return 0;
  }
  '''
  result = c_compiler.links(code, dependencies: dependencies, name: 'EVP API')
  if not result
    error('Your OpenSSL version appears to be too old. Check that you\'re using OpenSSL 1.1.0 or later.')
  endif
endif

conf_data = configuration_data()

conf_data.set('GNS_CRYPTO_AES_BCRYPT', false)
conf_data.set('GNS_CRYPTO_AES_OPENSSL', false)
conf_data.set('GNS_CRYPTO_25519_REF', false)
conf_data.set('GNS_CRYPTO_25519_OPENSSL', false)

if use_bcrypt
  conf_data.set('GNS_CRYPTO_AES_BCRYPT', true)
  conf_data.set('GNS_CRYPTO_25519_REF', true)
else
  conf_data.set('GNS_CRYPTO_AES_OPENSSL', true)
  if good_libcrypto.found()
    conf_data.set('GNS_CRYPTO_25519_OPENSSL', true)
  else
    conf_data.set('GNS_CRYPTO_25519_REF', true)
  endif
endif

configure_file(output: 'gnsconfig.h', configuration: conf_data)

dependencies += [
  dep_threads,
  dependency('protobuf', version: '>=3.0.0'),
]

incdirs = include_directories('.', '../include', 'common', 'public')

cpp_flags = [
  '-DSTEAMNETWORKINGSOCKETS_FOREXPORT',
  '-DENABLE_OPENSSLCONNECTION',
  '-DCRYPTO_DISABLE_ENCRYPT_WITH_PASSWORD',
  '-DGOOGLE_PROTOBUF_NO_RTTI',
]

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
  'common/crypto_bcrypt.cpp',
  'common/crypto_openssl.cpp',
  'common/crypto_25519_donna.cpp',
  'common/crypto_25519_openssl.cpp',
  'common/crypto_textencode.cpp',
  'common/keypair.cpp',
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

if use_bcrypt
  dependencies += [ c_compiler.find_library('bcrypt') ]
  cpp_flags += [ '-DED25519_HASH_BCRYPT' ]
else
  sources += [
    'common/opensslwrapper.cpp',
  ]
endif

# Select 25519 crypto provider
if use_bcrypt or not good_libcrypto.found()
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
  include_directories: include_directories( '../include', '.' ),
  compile_args: ['-DSTEAMNETWORKINGSOCKETS_STATIC_LINK'],
  link_with: GameNetworkingSockets_static )

GameNetworkingSockets_so = library('GameNetworkingSockets',
  sources, protobufs,
  c_args: cpp_flags,
  cpp_args: cpp_flags,
  include_directories: incdirs,
  dependencies: dependencies)

dep_GameNetworkingSockets_so = declare_dependency(
  include_directories: include_directories( '../include', '.' ),
  link_with: GameNetworkingSockets_so )

# vim: set ts=2 sts=2 sw=2 et:
