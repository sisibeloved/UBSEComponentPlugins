check = find_program('check.sh')

test(
  'mvp3_check',
  check,
  args: [meson.project_build_root(), meson.project_source_root()],
  depends: [ubsectl, ssu_mgr, ssu_smoke],
  timeout: 20,
)
