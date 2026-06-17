executable(
  'ubsectl',
  sources: ['ubsectl.c'],
  include_directories: inc,
  dependencies: [
    ssu_api_dep,
    ssu_controller_dep,
  ],
  link_with: libssu_plugin_mock,
  install: true,
  install_dir: get_option('sbindir'),
)
