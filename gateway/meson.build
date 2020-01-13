cxx_compiler = meson.get_compiler('cpp')
dependencies = [
  dep_threads,
  dep_GameNetworkingSockets_static, # Declared in the other project
  dependency('libjsoncpp', version: '>=0.10.7'),
  dependency('libjsonrpccpp', version: '>=1.2.0')
]

executable('gateway',
  'gateway.cpp',
  dependencies: dependencies,
  include_directories: include_directories('../src', '../src/public', '../src/common')
)

