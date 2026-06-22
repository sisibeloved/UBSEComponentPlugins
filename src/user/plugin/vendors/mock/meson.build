libssu_plugin_mock = shared_library(
  'ssu_plugin_mock',
  sources: ['ssu_plugin_mock.cpp', reqshim_iface_sources],
  include_directories: inc,
  dependencies: [ssu_headers_dep],
  install: true,
  install_dir: join_paths(get_option('libdir'), 'ubs-engine', 'ssu'),
)
