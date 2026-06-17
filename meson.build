project(
  'ubse_ssu',
  'c',
  version: '0.1.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
  ],
)

cc = meson.get_compiler('c')
inc = include_directories('include')

subdir('src/user/api')
subdir('src/user/controller')
subdir('src/user/scheduler')
subdir('src/user/plugin')
subdir('src/user/runtime')
subdir('tools')
subdir('tests')

if get_option('build_kernel').enabled()
  subdir('src/kernel/reqshim')
endif
