/**************************************************************************
 * 
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

/* Authors:  Keith Whitwell <keithw@vmware.com>
 */

#ifndef SP_STATE_H
#define SP_STATE_H

#include "pipe/p_state.h"

void
softpipe_init_blend_funcs(struct pipe_context *pipe);

void
softpipe_init_clip_funcs(struct pipe_context *pipe);

void
softpipe_init_sampler_funcs(struct pipe_context *pipe);

void
softpipe_init_rasterizer_funcs(struct pipe_context *pipe);

void
softpipe_init_shader_funcs(struct pipe_context *pipe);

void
softpipe_init_streamout_funcs(struct pipe_context *pipe);

void
softpipe_init_vertex_funcs(struct pipe_context *pipe);

void
softpipe_init_image_funcs(struct pipe_context *pipe);

void
softpipe_set_framebuffer_state(struct pipe_context *,
                               const struct pipe_framebuffer_state *);

void
softpipe_update_derived(struct softpipe_context *softpipe, unsigned prim);

void
softpipe_set_sampler_views(struct pipe_context *pipe,
                           enum pipe_shader_type shader,
                           unsigned start,
                           unsigned num,
                           struct pipe_sampler_view **views);


void
softpipe_draw_vbo(struct pipe_context *pipe,
                  const struct pipe_draw_info *info);

void
softpipe_map_texture_surfaces(struct softpipe_context *sp);

void
softpipe_unmap_texture_surfaces(struct softpipe_context *sp);


struct vertex_info *
softpipe_get_vbuf_vertex_info(struct softpipe_context *softpipe);


struct sp_fragment_shader_variant *
softpipe_find_fs_variant(struct softpipe_context *softpipe,
                         struct sp_fragment_shader *fs,
                         const struct sp_fragment_shader_variant_key *key);

void
softpipe_prepare_vertex_sampling(struct softpipe_context *ctx,
                                 unsigned num,
                                 struct pipe_sampler_view **views);
void
softpipe_cleanup_vertex_sampling(struct softpipe_context *ctx);


void
softpipe_prepare_geometry_sampling(struct softpipe_context *ctx,
                                   unsigned num,
                                   struct pipe_sampler_view **views);
void
softpipe_cleanup_geometry_sampling(struct softpipe_context *ctx);


void
softpipe_update_compute_samplers(struct softpipe_context *softpipe);
#endif
