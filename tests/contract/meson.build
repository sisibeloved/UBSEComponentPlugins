api_gating = executable(
  'api_gating',
  sources: ['api_gating.c'],
  include_directories: inc,
  dependencies: [ssu_api_dep],
)

test('api_gating', api_gating)

mock_pool = executable(
  'mock_pool',
  sources: ['mock_pool.c'],
  include_directories: inc,
  dependencies: [
    ssu_api_dep,
    ssu_controller_dep,
  ],
  link_with: libssu_plugin_mock,
)

test('mock_pool', mock_pool)
