project('GameNetworkingSockets', 'cpp', 'c',
  default_options: [
	'buildtype=debugoptimized',
  ],
  license: 'BSD'
)

flags_common = [
  '-fno-strict-aliasing',
  '-fvisibility=hidden',
]

target_os = target_machine.system()
if target_os == 'windows'
  flags_common += [
    '-fno-stack-protector',
  ]
else
  flags_common += [
    '-fstack-protector-strong',
    '-fstack-clash-protection',
  ]
endif

flags_cxx = [
  '-fno-rtti',
  '-fno-exceptions',
  '-fvisibility-inlines-hidden',
]

warn_flags_common = [
  '-Wall',
  '-Wextra',

  # Explicitly disable noisy warnings
  '-Wno-documentation',
  '-Wno-implicit-fallthrough',
  '-Wno-padded',
  '-Wno-sign-compare',
  '-Wno-sign-conversion',
  '-Wno-signed-enum-bitfield',
  '-Wno-unknown-pragmas',
  '-Wno-unused-const-variable',
  '-Wno-unused-local-typedef',
  '-Wno-unused-parameter',
  '-Wno-nested-anon-types',
  '-Wno-format-truncation'
]

werror = get_option('Werror')
if werror
  warn_flags_common += [ '-Werror' ]
endif

use_crypto_25519 = get_option('use_crypto25519')
if use_crypto_25519 == 'Reference'
  warn_flags_common += [ '-Wno-unused-function' ]
endif

warn_flags_c = [
  '-Wimplicit',
  '-Wstrict-prototypes',
  '-Wno-missing-prototypes',
]

warn_flags_cxx = [
  '-Wno-c++98-compat',
  '-Wno-c++98-compat-pedantic',
  '-Wno-exit-time-destructors',
  '-Wno-global-constructors',
  '-Wno-non-virtual-dtor',
  '-Wno-old-style-cast',
  '-Wno-reorder',

  # These ones are prolific but don't really matter. Most are in generated
  # protobuf code.
  '-Wno-zero-as-null-pointer-constant',
  '-Wno-missing-variable-declarations',
]

c_compiler = meson.get_compiler('c')
cxx_compiler = meson.get_compiler('cpp')

c_flags = []
c_flags += c_compiler.first_supported_argument(['-std=c11', '-std=gnu99'])
foreach arg : flags_common + warn_flags_common + warn_flags_c
  if c_compiler.has_argument(arg)
    c_flags += [ arg ]
  endif
endforeach
add_project_arguments(c_flags, language: 'c')

cxx_flags = []
cxx_flags += cxx_compiler.first_supported_argument(['-std=c++11', '-std=c++0x'])
foreach arg : flags_common + warn_flags_common + flags_cxx + warn_flags_cxx
  if cxx_compiler.has_argument(arg)
    cxx_flags += [ arg ]
  endif
endforeach
add_project_arguments(cxx_flags, language: 'cpp')

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

dep_threads = dependency('threads')

subdir('src')
subdir('tests')
subdir('examples')
