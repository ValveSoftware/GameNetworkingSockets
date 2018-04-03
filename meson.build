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
  '-Wno-unknown-pragmas',
  '-Wno-sign-compare',
  '-Wno-implicit-fallthrough',
  '-Wno-unused-parameter',
  '-Wno-unused-local-typedef',
  '-Wno-unused-const-variable',
]

warn_flags_c = [
  '-Wimplicit',
  '-Wstrict-prototypes',
]

warn_flags_cxx = [
  '-Wno-reorder',
  '-Wno-non-virtual-dtor',
  #'-Wweak-vtables',
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

subdir('src')
subdir('tests')
