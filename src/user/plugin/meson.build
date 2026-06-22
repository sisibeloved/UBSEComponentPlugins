subdir('vendors/mock')
subdir('vendors/lbc_mock')

if get_option('vendor') == 'mock'
  ssu_plugin_lib = libssu_plugin_mock
elif get_option('vendor') == 'lbc_mock'
  ssu_plugin_lib = libssu_plugin_lbc_mock
endif
