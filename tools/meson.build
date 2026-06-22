ubsectl = executable(
  'ubsectl',
  sources: ['ubsectl.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_api_dep,
    ssu_controller_dep,
  ],
  link_with: ssu_plugin_lib,
  install: true,
  install_dir: get_option('sbindir'),
)
