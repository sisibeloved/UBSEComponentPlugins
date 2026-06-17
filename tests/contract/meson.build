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

control_lifecycle = executable(
  'control_lifecycle',
  sources: ['control_lifecycle.c'],
  include_directories: inc,
  dependencies: [
    ssu_controller_dep,
  ],
  link_with: libssu_plugin_mock,
)

test('control_lifecycle', control_lifecycle)

api_lifecycle = executable(
  'api_lifecycle',
  sources: ['api_lifecycle.c'],
  include_directories: inc,
  dependencies: [
    ssu_api_dep,
  ],
)

test('api_lifecycle', api_lifecycle)

dataplane_mock = executable(
  'dataplane_mock',
  sources: ['dataplane_mock.c'],
  include_directories: inc,
  dependencies: [
    ssu_headers_dep,
    ssu_dataplane_dep,
  ],
)

test('dataplane_mock', dataplane_mock)
