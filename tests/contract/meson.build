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

control_rank_distribution = executable(
  'control_rank_distribution',
  sources: ['control_rank_distribution.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_controller_dep,
  ],
  link_with: libssu_plugin_mock,
)

test('control_rank_distribution', control_rank_distribution)

control_stale_logdev = executable(
  'control_stale_logdev',
  sources: ['control_stale_logdev.cpp'],
  include_directories: inc,
  dependencies: [
    ssu_controller_dep,
  ],
)

test('control_stale_logdev', control_stale_logdev)

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

if get_option('vendor') == 'mock'
  api_gating = executable(
    'api_gating',
    sources: ['api_gating.cpp'],
    include_directories: inc,
    dependencies: [ssu_api_dep],
  )

  test('api_gating', api_gating)

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

  api_named_idempotent = executable(
    'api_named_idempotent',
    sources: ['api_named_idempotent.cpp'],
    include_directories: inc,
    dependencies: [
      ssu_api_dep,
    ],
  )

  test('api_named_idempotent', api_named_idempotent)

  api_lba_offsets = executable(
    'api_lba_offsets',
    sources: ['api_lba_offsets.cpp'],
    include_directories: inc,
    dependencies: [
      ssu_api_dep,
    ],
  )

  test('api_lba_offsets', api_lba_offsets)

  dlopen_deps = [ssu_headers_dep]
  dl_dep = cpp.find_library('dl', required: false)
  if dl_dep.found()
    dlopen_deps += dl_dep
  endif

  api_dlopen = executable(
    'api_dlopen',
    sources: ['api_dlopen.cpp'],
    include_directories: inc,
    dependencies: dlopen_deps,
  )

  test(
    'api_dlopen',
    api_dlopen,
    args: [libssu_api.full_path()],
  )
endif

sdk_so_path = libubse_ssu_sdk.full_path()
sdk_dlopen_deps = [ssu_headers_dep]
sdk_dl_dep = cpp.find_library('dl', required: false)
if sdk_dl_dep.found()
  sdk_dlopen_deps += sdk_dl_dep
endif

sdk_dlopen = executable(
  'sdk_dlopen',
  sources: ['sdk_dlopen.cpp'],
  include_directories: inc,
  dependencies: sdk_dlopen_deps,
)

test(
  'sdk_dlopen',
  sdk_dlopen,
  args: [sdk_so_path],
  depends: [libubse_ssu_sdk],
)

sdk_diagnostics = executable(
  'sdk_diagnostics',
  sources: ['sdk_diagnostics.cpp'],
  include_directories: inc,
  dependencies: sdk_dlopen_deps,
)

test(
  'sdk_diagnostics',
  sdk_diagnostics,
  args: [sdk_so_path],
  depends: [libubse_ssu_sdk],
  timeout: 5,
)

sdk_linkage = find_program('sdk_linkage.sh')

test(
  'sdk_linkage',
  sdk_linkage,
  args: [sdk_so_path],
  depends: [libubse_ssu_sdk],
)

ubsectl_linkage = find_program('ubsectl_linkage.sh')

test(
  'ubsectl_linkage',
  ubsectl_linkage,
  args: [ubsectl.full_path()],
  depends: [ubsectl],
)

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
