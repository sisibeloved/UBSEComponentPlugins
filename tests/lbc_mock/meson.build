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
  lbc_mock_ubsectl_flow = find_program('ubsectl_flow.sh')

  test(
    'lbc_mock_ubsectl_flow',
    lbc_mock_ubsectl_flow,
    args: [meson.project_build_root()],
    timeout: 20,
  )

  lbc_mock_ubsectl_error_flow = find_program('ubsectl_error_flow.sh')

  test(
    'lbc_mock_ubsectl_error_flow',
    lbc_mock_ubsectl_error_flow,
    args: [meson.project_build_root()],
    timeout: 20,
  )
endif
