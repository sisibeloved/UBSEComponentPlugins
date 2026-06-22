api_gating = executable(
  'api_gating',
  sources: ['api_gating.cpp'],
  include_directories: inc,
  dependencies: [ssu_api_dep],
)

test('api_gating', api_gating)

mock_pool = executable(
  'mock_pool',
  sources: ['mock_pool.cpp'],
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
  sources: ['control_lifecycle.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_controller_dep,
  ],
  link_with: libssu_plugin_mock,
)

test('control_lifecycle', control_lifecycle)

control_multiphys = executable(
  'control_multiphys',
  sources: ['control_multiphys.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_controller_dep,
  ],
  link_with: libssu_plugin_mock,
)

test('control_multiphys', control_multiphys)

shared_mount = executable(
  'shared_mount',
  sources: ['shared_mount.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_controller_dep,
  ],
  link_with: libssu_plugin_mock,
)

test('shared_mount', shared_mount)

api_lifecycle = executable(
  'api_lifecycle',
  sources: ['api_lifecycle.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_api_dep,
  ],
)

test('api_lifecycle', api_lifecycle)

api_extent_buffer = executable(
  'api_extent_buffer',
  sources: ['api_extent_buffer.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_api_dep,
  ],
)

test('api_extent_buffer', api_extent_buffer)

api_logical = executable(
  'api_logical',
  sources: ['api_logical.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_api_dep,
  ],
)

test('api_logical', api_logical)

dataplane_mock = executable(
  'dataplane_mock',
  sources: ['dataplane_mock.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_headers_dep,
    ssu_dataplane_dep,
  ],
)

test('dataplane_mock', dataplane_mock)

reqshim_dataplane_source = executable(
  'reqshim_dataplane_source',
  sources: ['reqshim_dataplane_source.cpp'],
)

test(
  'reqshim_dataplane_source',
  reqshim_dataplane_source,
  args: [meson.project_source_root()],
)

reqshim_control_source = executable(
  'reqshim_control_source',
  sources: ['reqshim_control_source.cpp'],
)

test(
  'reqshim_control_source',
  reqshim_control_source,
  args: [meson.project_source_root()],
)

reqshim_iface = executable(
  'reqshim_iface',
  sources: [
    'reqshim_iface.cpp',
    '../../src/user/plugin/reqshim_iface.cpp',
  ],
  include_directories: inc,
  dependencies: [ssu_headers_dep],
)

test('reqshim_iface', reqshim_iface)

runtime_error_hints = executable(
  'runtime_error_hints',
  sources: ['runtime_error_hints.cpp'],
  include_directories: inc,
  dependencies: [ssu_headers_dep],
)

test('runtime_error_hints', runtime_error_hints)

reqshim_build_script = find_program('sh')

test(
  'reqshim_build_script',
  reqshim_build_script,
  args: [
    files('reqshim_build_script.sh'),
    meson.project_source_root(),
  ],
)
