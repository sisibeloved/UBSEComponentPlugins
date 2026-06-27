lbc_mock_plugin_flow = executable(
  'lbc_mock_plugin_flow',
  sources: ['lbc_mock_plugin_flow.cpp'],
  include_directories: [
    inc,
    include_directories('../../src/user/plugin/vendors/lbc_mock'),
  ],
  dependencies: [
    ssu_headers_dep,
  ],
  link_with: libssu_plugin_lbc_mock,
)

test('lbc_mock_plugin_flow', lbc_mock_plugin_flow)

if get_option('vendor') == 'lbc_mock'
  sdk_flow_deps = [ssu_headers_dep]
  sdk_flow_dl_dep = cpp.find_library('dl', required: false)
  if sdk_flow_dl_dep.found()
    sdk_flow_deps += sdk_flow_dl_dep
  endif

  sdk_flow = executable(
    'sdk_flow',
    sources: ['sdk_flow.cpp'],
    include_directories: inc,
    dependencies: sdk_flow_deps,
  )

  lbc_mock_sdk_flow = find_program('sdk_flow.sh')

  test(
    'lbc_mock_sdk_flow',
    lbc_mock_sdk_flow,
    args: [
      meson.project_build_root(),
      sdk_flow.full_path(),
      libubse_ssu_sdk.full_path(),
    ],
    depends: [sdk_flow, libubse_ssu_sdk, ssu_mgr],
    timeout: 20,
  )

  lbc_mock_ubsectl_flow = find_program('ubsectl_flow.sh')

  test(
    'lbc_mock_ubsectl_flow',
    lbc_mock_ubsectl_flow,
    args: [meson.project_build_root()],
    depends: [ubsectl, ssu_mgr],
    timeout: 20,
  )

  lbc_mock_ubsectl_error_flow = find_program('ubsectl_error_flow.sh')

  test(
    'lbc_mock_ubsectl_error_flow',
    lbc_mock_ubsectl_error_flow,
    args: [meson.project_build_root()],
    depends: [ubsectl, ssu_mgr],
    timeout: 20,
  )
endif
