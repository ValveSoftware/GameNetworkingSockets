cxx_compiler = meson.get_compiler('cpp')

dependencies = [
  dependency('threads'),
  dep_GameNetworkingSockets_so # Declared in the other project
]

cpp_flags = []

executable('test_connection',
  'connection_test.cpp',
  dependencies: dependencies,
  cpp_args: cpp_flags
)
