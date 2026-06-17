kernel_src_dir = get_option('kernel_src_dir')
if kernel_src_dir == ''
  error('kernel_src_dir is required when build_kernel is enabled')
endif

reqshim_inputs = files(
  'Kbuild',
  'Makefile',
  'reqshim_blk.c',
  'reqshim_ioctl.c',
  'reqshim_internal.h',
  'reqshim_main.c',
  'reqshim_map.c',
  'reqshim_uapi.h',
)

custom_target(
  'ssu_reqshim.ko',
  output: 'ssu_reqshim.ko',
  input: reqshim_inputs,
  command: [
    find_program('sh'),
    files('../../../scripts/build_reqshim.sh'),
    kernel_src_dir,
    '@OUTDIR@',
    '@INPUT@',
  ],
  build_by_default: true,
  install: true,
  install_dir: join_paths(get_option('libdir'), 'modules', 'extra'),
)
