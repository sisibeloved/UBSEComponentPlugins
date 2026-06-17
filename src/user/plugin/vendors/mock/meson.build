libssu_plugin_mock = shared_library(
  'ssu_plugin_mock',
  sources: ['ssu_plugin_mock.c'],
  include_directories: inc,
  dependencies: [ssu_api_dep],
  install: true,
  install_dir: join_paths(get_option('libdir'), 'ubs-engine', 'ssu'),
)
