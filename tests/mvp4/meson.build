check = find_program('check.sh')

test(
  'mvp4_check',
  check,
  args: [meson.project_build_root()],
  depends: [ssu_smoke],
  timeout: 20,
)
