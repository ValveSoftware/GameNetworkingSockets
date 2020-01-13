cxx_compiler = meson.get_compiler('cpp')
dependencies = [
  dep_threads,
  dep_GameNetworkingSockets_static, # Declared in the other project
  dependency('jsoncpp'),
  dependency('libjsonrpccpp-common'),
  dependency('libjsonrpccpp-client')
]

# Preprocessor defines for platform and compiler
cpp_flags = []
if target_os == 'linux'
  cpp_flags += ['-DPOSIX', '-DLINUX']
elif target_os == 'darwin'
  cpp_flags += ['-DPOSIX', '-DOSX']
elif target_os == 'windows'
  cpp_flags += ['-D_WIN32', '-DWIN32', '-D__STDC_FORMAT_MACROS=1', '-D__USE_MINGW_ANSI_STDIO=0']
else
  error('Could not identify your target operating system')
endif

add_project_arguments(cpp_flags, language: 'c')
add_project_arguments(cpp_flags, language: 'cpp')

executable('gateway',
  'gateway.cpp',
  dependencies: dependencies,
  include_directories: include_directories('..include', '../src', '../src/public', '../src/common')
)

