libssu_scheduler = shared_library(
  'ssu_scheduler',
  sources: ['ssu_scheduler.c'],
  include_directories: inc,
  dependencies: [ssu_headers_dep],
  install: true,
  install_dir: join_paths(get_option('libdir'), 'ubs-engine'),
)

ssu_scheduler_dep = declare_dependency(
  include_directories: inc,
  link_with: libssu_scheduler,
)
