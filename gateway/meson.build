cxx_compiler = meson.get_compiler('cpp')
dependencies = [
  dep_threads,
  dep_GameNetworkingSockets_static, # Declared in the other project
  dependency('jsoncpp'),
  dependency('libjsonrpccpp-common'),
  dependency('libjsonrpccpp-client'),
  dependency('zmq')
]


executable('gateway',
  'gateway.cpp',
  dependencies: dependencies,
  include_directories: include_directories('../src', '../src/public', '../src/common')
)

