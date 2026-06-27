libubse_ssu_sdk = shared_library(
  'ubse_ssu_sdk',
  sources: ['ubse_ssu_sdk.cpp'],
  include_directories: inc,
  dependencies: [ssu_headers_dep],
  install: true,
)

ubse_ssu_sdk_dep = declare_dependency(
  include_directories: inc,
  link_with: libubse_ssu_sdk,
)
