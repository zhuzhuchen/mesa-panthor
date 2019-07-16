# Copyright © 2017 Intel Corporation

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

pan_entrypoints = custom_target(
  'pan_entrypoints.[ch]',
  input : ['pan_entrypoints_gen.py', vk_api_xml],
  output : ['pan_entrypoints.h', 'pan_entrypoints.c'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--outdir',
    meson.current_build_dir()
  ],
  depend_files : files('pan_extensions.py'),
)

pan_extensions_c = custom_target(
  'pan_extensions.c',
  input : ['pan_extensions.py', vk_api_xml],
  output : ['pan_extensions.c', 'pan_extensions.h'],
  command : [
    prog_python, '@INPUT0@', '--xml', '@INPUT1@', '--out-c', '@OUTPUT0@',
    '--out-h', '@OUTPUT1@'
  ],
)

libpan_files = files(
  'pan_cmd_buffer.c',
  'pan_device.c',
  'pan_descriptor_set.c',
  'pan_descriptor_set.h',
  'pan_drm.c',
  'pan_fence.c',
  'pan_formats.c',
  'pan_image.c',
  'pan_meta_blit.c',
  'pan_meta_buffer.c',
  'pan_meta_clear.c',
  'pan_meta_copy.c',
  'pan_meta_resolve.c',
  'pan_pass.c',
  'pan_pipeline.c',
  'pan_pipeline_cache.c',
  'pan_private.h',
  'pan_query.c',
  'pan_shader.c',
  'pan_util.c',
  'pan_util.h',
  'pan_wsi.c',
  'vk_format.h',
)

pan_deps = []
pan_flags = []

if with_platform_wayland
  pan_deps += dep_wayland_client
  pan_flags += '-DVK_USE_PLATFORM_WAYLAND_KHR'
  libpan_files += files('pan_wsi_wayland.c')
endif

libvulkan_panfrost = shared_library(
  'vulkan_panfrost',
  [libpan_files, pan_entrypoints, pan_extensions_c],
  include_directories : [
    inc_include,
    inc_src,
    inc_compiler,
    inc_gallium, # XXX: pipe/p_format.h
    inc_gallium_aux, # XXX: renderonly
    inc_vulkan_wsi,
    inc_panfrost,
  ],
  link_with : [
    libvulkan_wsi,
    libpanfrost_shared,
    libpanfrost_midgard,
    libpanfrost_bifrost,
    libpanfrost_lib,
    libpanfrost_util,
  ],
  dependencies : [
    dep_dl,
    dep_elf,
    dep_libdrm,
    dep_llvm,
    dep_m,
    dep_thread,
    dep_valgrind,
    idep_nir,
    pan_deps,
    idep_vulkan_util,
    idep_mesautil,
  ],
  c_args : [no_override_init_args, pan_flags],
  link_args : [ld_args_bsymbolic, ld_args_gc_sections],
  install : true,
)

panfrost_icd = custom_target(
  'panfrost_icd',
  input : 'pan_icd.py',
  output : 'panfrost_icd.@0@.json'.format(host_machine.cpu()),
  command : [
    prog_python, '@INPUT@',
    '--lib-path', join_paths(get_option('prefix'), get_option('libdir')),
    '--out', '@OUTPUT@',
  ],
  depend_files : files('pan_extensions.py'),
  build_by_default : true,
  install_dir : with_vulkan_icd_dir,
  install : true,
)

pan_dev_icd = custom_target(
  'pan_dev_icd',
  input : 'pan_icd.py',
  output : 'dev_icd.json',
  command : [
    prog_python, '@INPUT@', '--lib-path', meson.current_build_dir(),
    '--out', '@OUTPUT@'
  ],
  depend_files : files('pan_extensions.py'),
  build_by_default : true,
  install : false,
)
