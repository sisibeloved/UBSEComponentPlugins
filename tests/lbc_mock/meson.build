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
