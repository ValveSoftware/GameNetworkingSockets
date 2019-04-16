common_deps = [ dep_threads ]
cppflags = []

light_tests= get_option('light_tests')
if light_tests
  cppflags += ['-DLIGHT_TESTS']
endif

executable('test_connection',
  'test_connection.cpp',
  dependencies: common_deps + [ dep_GameNetworkingSockets_so ],
  cpp_args: cppflags,
)

executable('test_crypto',
  'test_crypto.cpp',
  dependencies: common_deps + [ dep_GameNetworkingSockets_static ],
  cpp_args: cppflags + ['-DTEST_VECTOR_DIR="' + join_paths(meson.source_root(), 'tests/aesgcmtestvectors') + '/"'],
  include_directories: include_directories('../src', '../src/public', '../src/common')
)

# !FIXME! Ug cannot link with the static lib, because we need to #define the hardcoded key.
# So we'll need the crypto and protobuf dependencies, and those are pretty complicated.
# We need to refactor these files to get that organized.
#executable('test_pki',
#  'test_pki.cpp',
#  dependencies: common_deps,
#  cpp_args: cppflags + ['-DSTEAMNETWORKINGSOCKETS_HARDCODED_ROOT_CA_KEY="ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMPsJA3FUaQSsHkyYhWvcYt9v3XbwRBcWuVuayWGdK1"'],
#  include_directories: include_directories('../src', '../include', '../src/public', '../src/common')
#)

#incdirs = include_directories('../include')
#executable('test_flat',
#  'test_flat.c',
#  dependence: dependencies,
#  include_directories: incdirs
#)
