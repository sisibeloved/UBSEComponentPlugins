project(
  'ubse_ssu',
  ['c', 'cpp'],
  version: '0.1.0',
  default_options: [
    'c_std=gnu11',
    'cpp_std=c++17',
    'warning_level=2',
  ],
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
inc = include_directories('include')
ssu_headers_dep = declare_dependency(include_directories: inc)

subdir('src/user/controller')
subdir('src/user/scheduler')
subdir('src/user/plugin')
subdir('src/user/api')
subdir('src/user/dataplane')
subdir('src/user/runtime')
subdir('tools')
subdir('tests')

if get_option('build_kernel').enabled()
  subdir('src/kernel/reqshim')
endif
