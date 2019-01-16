cxx_compiler = meson.get_compiler('cpp')

dependencies = [
  dep_threads,
  dep_GameNetworkingSockets_so # Declared in the other project
]

executable('example_chat',
  'example_chat.cpp',
  dependencies: dependencies,
)

