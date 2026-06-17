libssu_controller = shared_library(
  'ssu_controller',
  sources: ['ssu_controller.c'],
  include_directories: inc,
  dependencies: [ssu_headers_dep],
  install: true,
  install_dir: join_paths(get_option('libdir'), 'ubs-engine'),
)

ssu_controller_dep = declare_dependency(
  include_directories: inc,
  link_with: libssu_controller,
)
