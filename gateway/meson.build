cxx_compiler = meson.get_compiler('cpp')

dependencies = [
  dep_threads,
  dep_GameNetworkingSockets_so # Declared in the other project
]

executable('gateway',
  'gateway.cpp',
  dependencies: dependencies,
)

