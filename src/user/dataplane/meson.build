libssu_dataplane = shared_library(
  'ssu_dataplane',
  sources: ['ssu_dataplane.cpp'],
  include_directories: inc,
  dependencies: [ssu_headers_dep],
  install: true,
)

ssu_dataplane_dep = declare_dependency(
  include_directories: inc,
  link_with: libssu_dataplane,
)
