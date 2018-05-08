# Copyright © 2017 Intel Corporation
# Copyright © 2018 Alyssa Rosenzweig

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

files_panfrost = files(
  'sp_clear.c',
  'sp_clear.h',
  'sp_context.c',
  'sp_context.h',
  'sp_draw_arrays.c',
  'sp_fence.c',
  'sp_fence.h',
  'sp_flush.c',
  'sp_flush.h',
  'sp_fs.h',
  'sp_limits.h',
  'sp_public.h',
  'sp_query.c',
  'sp_state_shader.c',
  'sp_query.h',
  'sp_screen.c',
  'sp_screen.h',
  'sp_state_blend.c',
  'sp_state_clip.c',
  'sp_state.h',
  'sp_state_sampler.c',
  'sp_state_rasterizer.c',
  'sp_state_so.c',
  'sp_state_surface.c',
  'sp_state_vertex.c',
  'sp_surface.c',
  'sp_surface.h',
  'sp_texture.c',
  'sp_texture.h',

    '/home/guest/panloader/trans/pandev.c',
    '/home/guest/panloader/trans/allocate.c',
    '/home/guest/panloader/trans/assemble.c',
    '/home/guest/panloader/trans/slow-framebuffer.c',
    '/home/guest/panloader/trans/trans-builder.c',
)

libpanfrost = static_library(
  'panfrost',
  files_panfrost,
  dependencies: [cc.find_library('X11', required: true), dep_thread, idep_nir],
  include_directories : [inc_gallium_aux, inc_gallium, inc_include, inc_src, include_directories('/home/guest/panloader/trans'), include_directories('/home/guest/panloader/include'), include_directories('/home/guest/panloader/build/include')],
  c_args : [c_vis_args, c_msvc_compat_args],
)

driver_panfrost = declare_dependency(
  compile_args : '-DGALLIUM_PANFROST',
  link_with : libpanfrost
)

midgard_nir_algebraic_c = custom_target(
  'midgard_nir_algebraic.c',
  input : 'midgard/midgard_nir_algebraic.py',
  output : 'midgard_nir_algebraic.c',
  command : [
    prog_python2, '@INPUT@',
    '-p', join_paths(meson.source_root(), 'src/compiler/nir/'),
  ],
  capture : true,
  depend_files : nir_algebraic_py,
)

files_midgard = files(
  'midgard/midgard_compile.c',
  'midgard/cmdline.c',
  'midgard/cppwrap.cpp',
)

midgard_compiler = executable(
  'midgard_compiler',
  [files_midgard, midgard_nir_algebraic_c],
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
