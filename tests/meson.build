cxx_compiler = meson.get_compiler('cpp')

dependencies = [
  dependency('threads'),
  dep_GameNetworkingSockets_so # Declared in the other project
]

warn_flags_tests = [
  '-Wall',
  '-Wextra',
  '-pedantic',
  '-Weverything',

  # Explicitly disable a bunch of noisy warnings.
  '-Wno-documentation',
  '-Wno-c++98-compat',
  '-Wno-c++98-compat-pedantic',
  '-Wno-old-style-cast',
  '-Wno-exit-time-destructors',
  '-Wno-global-constructors',
  '-Wno-padded',
  '-Wno-signed-enum-bitfield',
  '-Wno-sign-conversion',

  # The interface classes trigger this, but we don't want to export C++
  # symbols from the GameNetworkingSockets library.
  '-Wno-weak-vtables',
]

cxx_flags = []
foreach arg : warn_flags_tests
  if cxx_compiler.has_argument(arg)
    cxx_flags += [ arg ]
  endif
endforeach

executable('test_connection',
  'test_connection.cpp',
  dependencies: dependencies,
  cpp_args: cxx_flags
)

#incdirs = include_directories('../include')
#executable('test_flat',
#  'test_flat.c',
#  dependence: dependencies,
#  include_directories: incdirs
#)
