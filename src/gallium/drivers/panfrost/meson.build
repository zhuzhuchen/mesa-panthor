files_libpanfrost = files(
  'panfrost_pipe.c',
  'panfrost_state.c',
  'midgard/midgard_cmdline.c'
)

libpanfrost = static_library(
  'panfrost',
  files_libpanfrost,
  include_directories : [inc_common, inc_src, inc_include, inc_gallium, inc_gallium_aux, inc_gallium_drivers, include_directories('midgard')],
  dependencies : [dep_thread, idep_nir_headers],
  build_by_default : true
)

driver_panfrost = declare_dependency(
  compile_args : '-DGALLIUM_PANFROST',
  link_with : [libpanfrost],
  dependencies : idep_nir
)

midgard_compiler = executable(
  'midgard_compiler',
  'midgard/midgard_cmdline.c',
  include_directories : [inc_common, inc_src, inc_include, inc_gallium, inc_gallium_aux, include_directories('midgard')],
  dependencies : [
    dep_thread,
    idep_nir
  ],
  link_with : [
    libgallium,
    libglsl_standalone,
    libmesa_util
  ],
  build_by_default : true
)
