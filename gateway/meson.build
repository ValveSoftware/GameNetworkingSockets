cxx_compiler = meson.get_compiler('cpp')

dependencies = [
  dep_threads,
  dep_GameNetworkingSockets_so # Declared in the other project,
  jsonrpccpp-common,
  jsonrpccpp-client
]

executable('gateway',
  'gateway.cpp',
  dependencies: dependencies,
  include_directories: include_directories('../src', '../src/public', '../src/common')
)

