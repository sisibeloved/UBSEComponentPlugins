libssu_api = shared_library(
  'ssu_api',
  sources: ['ssu_api.c'],
  include_directories: inc,
  install: true,
)

ssu_api_dep = declare_dependency(
  include_directories: inc,
  link_with: libssu_api,
)
