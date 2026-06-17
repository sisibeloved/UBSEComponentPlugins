runtime_deps = [ssu_api_dep, ssu_controller_dep, ssu_scheduler_dep]

executable(
  'ssu-mgr',
  sources: ['ssu_runtime.c'],
  include_directories: inc,
  dependencies: runtime_deps,
  link_with: libssu_plugin_mock,
  c_args: ['-DSSU_DEFAULT_ROLE="manager"'],
  install: true,
  install_dir: get_option('sbindir'),
)

executable(
  'ssu-agent',
  sources: ['ssu_runtime.c'],
  include_directories: inc,
  dependencies: runtime_deps,
  link_with: libssu_plugin_mock,
  c_args: ['-DSSU_DEFAULT_ROLE="agent"'],
  install: true,
  install_dir: get_option('sbindir'),
)
