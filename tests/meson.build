common_deps = [ dependency('threads') ]

executable('test_connection',
  'test_connection.cpp',
  dependencies: common_deps + [ dep_GameNetworkingSockets_so ],
)

executable('test_crypto',
  'test_crypto.cpp',
  dependencies: common_deps + [ dep_GameNetworkingSockets_static ],
  include_directories: include_directories('../src', '../src/public', '../src/common')
)

#incdirs = include_directories('../include')
#executable('test_flat',
#  'test_flat.c',
#  dependence: dependencies,
#  include_directories: incdirs
#)
