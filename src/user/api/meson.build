libssu_api = shared_library(
  'ssu_api',
  sources: ['ssu_api.cpp'],
  include_directories: inc,
  dependencies: [ssu_controller_dep],
  link_with: libssu_plugin_mock,
  install: true,
)

ssu_api_dep = declare_dependency(
  include_directories: inc,
  link_with: libssu_api,
)
