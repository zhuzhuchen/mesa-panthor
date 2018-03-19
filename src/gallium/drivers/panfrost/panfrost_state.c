/*
 * Copyright 2010 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include <errno.h>
#include "pipe/p_defines.h"
#include "pipe/p_state.h"
#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_transfer.h"

static void panfrost_draw_vbo(struct pipe_context *ctx, const struct pipe_draw_info *info)
{
}

static void panfrost_launch_grid(struct pipe_context *ctx,
                             const struct pipe_grid_info *info)
{
}

static void panfrost_set_blend_color(struct pipe_context *ctx,
                                 const struct pipe_blend_color *state)
{
}

static void *panfrost_create_blend_state(struct pipe_context *ctx,
                                     const struct pipe_blend_state *state)
{
   return MALLOC(1);
}

static void *panfrost_create_dsa_state(struct pipe_context *ctx,
                                   const struct pipe_depth_stencil_alpha_state *state)
{
   return MALLOC(1);
}

static void *panfrost_create_rs_state(struct pipe_context *ctx,
                                  const struct pipe_rasterizer_state *state)
{
   return MALLOC(1);
}

static void *panfrost_create_sampler_state(struct pipe_context *ctx,
                                       const struct pipe_sampler_state *state)
{
   return MALLOC(1);
}

static struct pipe_sampler_view *panfrost_create_sampler_view(struct pipe_context *ctx,
                                                          struct pipe_resource *texture,
                                                          const struct pipe_sampler_view *state)
{
   struct pipe_sampler_view *sampler_view = CALLOC_STRUCT(pipe_sampler_view);

   if (!sampler_view)
      return NULL;

   /* initialize base object */
   *sampler_view = *state;
   sampler_view->texture = NULL;
   pipe_resource_reference(&sampler_view->texture, texture);
   pipe_reference_init(&sampler_view->reference, 1);
   sampler_view->context = ctx;
   return sampler_view;
}

static struct pipe_surface *panfrost_create_surface(struct pipe_context *ctx,
                                                struct pipe_resource *texture,
                                                const struct pipe_surface *surf_tmpl)
{
   struct pipe_surface *surface = CALLOC_STRUCT(pipe_surface);

   if (!surface)
      return NULL;
   pipe_reference_init(&surface->reference, 1);
   pipe_resource_reference(&surface->texture, texture);
   surface->context = ctx;
   surface->format = surf_tmpl->format;
   surface->width = texture->width0;
   surface->height = texture->height0;
   surface->texture = texture;
   surface->u.tex.first_layer = surf_tmpl->u.tex.first_layer;
   surface->u.tex.last_layer = surf_tmpl->u.tex.last_layer;
   surface->u.tex.level = surf_tmpl->u.tex.level;

   return surface;
}

static void panfrost_set_sampler_views(struct pipe_context *ctx,
                                   enum pipe_shader_type shader,
                                   unsigned start, unsigned count,
                                   struct pipe_sampler_view **views)
{
}

static void panfrost_bind_sampler_states(struct pipe_context *ctx,
                                     enum pipe_shader_type shader,
                                     unsigned start, unsigned count,
                                     void **states)
{
}

static void panfrost_set_clip_state(struct pipe_context *ctx,
                                const struct pipe_clip_state *state)
{
}

static void panfrost_set_polygon_stipple(struct pipe_context *ctx,
                                     const struct pipe_poly_stipple *state)
{
}

static void panfrost_set_sample_mask(struct pipe_context *pipe, unsigned sample_mask)
{
}

static void panfrost_set_scissor_states(struct pipe_context *ctx,
                                    unsigned start_slot,
                                    unsigned num_scissors,
                                    const struct pipe_scissor_state *state)
{
}

static void panfrost_set_stencil_ref(struct pipe_context *ctx,
                                 const struct pipe_stencil_ref *state)
{
}

static void panfrost_set_viewport_states(struct pipe_context *ctx,
                                     unsigned start_slot,
                                     unsigned num_viewports,
                                     const struct pipe_viewport_state *state)
{
}

static void panfrost_set_framebuffer_state(struct pipe_context *ctx,
                                       const struct pipe_framebuffer_state *state)
{
}

static void panfrost_set_constant_buffer(struct pipe_context *ctx,
                                     enum pipe_shader_type shader, uint index,
                                     const struct pipe_constant_buffer *cb)
{
}


static void panfrost_sampler_view_destroy(struct pipe_context *ctx,
                                      struct pipe_sampler_view *state)
{
   pipe_resource_reference(&state->texture, NULL);
   FREE(state);
}


static void panfrost_surface_destroy(struct pipe_context *ctx,
                                 struct pipe_surface *surface)
{
   pipe_resource_reference(&surface->texture, NULL);
   FREE(surface);
}

static void panfrost_bind_state(struct pipe_context *ctx, void *state)
{
}

static void panfrost_delete_state(struct pipe_context *ctx, void *state)
{
   FREE(state);
}

static void panfrost_set_vertex_buffers(struct pipe_context *ctx,
                                    unsigned start_slot, unsigned count,
                                    const struct pipe_vertex_buffer *buffers)
{
}

static void *panfrost_create_vertex_elements(struct pipe_context *ctx,
                                         unsigned count,
                                         const struct pipe_vertex_element *state)
{
   return MALLOC(1);
}

static void *panfrost_create_shader_state(struct pipe_context *ctx,
                                      const struct pipe_shader_state *state)
{
   return MALLOC(1);
}

static void *panfrost_create_compute_state(struct pipe_context *ctx,
                                       const struct pipe_compute_state *state)
{
   return MALLOC(1);
}

static struct pipe_stream_output_target *panfrost_create_stream_output_target(
      struct pipe_context *ctx,
      struct pipe_resource *res,
      unsigned buffer_offset,
      unsigned buffer_size)
{
   struct pipe_stream_output_target *t = CALLOC_STRUCT(pipe_stream_output_target);
   if (!t)
      return NULL;

   pipe_reference_init(&t->reference, 1);
   pipe_resource_reference(&t->buffer, res);
   t->buffer_offset = buffer_offset;
   t->buffer_size = buffer_size;
   return t;
}

static void panfrost_stream_output_target_destroy(struct pipe_context *ctx,
                                              struct pipe_stream_output_target *t)
{
   pipe_resource_reference(&t->buffer, NULL);
   FREE(t);
}

static void panfrost_set_stream_output_targets(struct pipe_context *ctx,
                                           unsigned num_targets,
                                           struct pipe_stream_output_target **targets,
                                           const unsigned *offsets)
{
}

void panfrost_init_state_functions(struct pipe_context *ctx);

void panfrost_init_state_functions(struct pipe_context *ctx)
{
   ctx->create_blend_state = panfrost_create_blend_state;
   ctx->create_depth_stencil_alpha_state = panfrost_create_dsa_state;
   ctx->create_fs_state = panfrost_create_shader_state;
   ctx->create_rasterizer_state = panfrost_create_rs_state;
   ctx->create_sampler_state = panfrost_create_sampler_state;
   ctx->create_sampler_view = panfrost_create_sampler_view;
   ctx->create_surface = panfrost_create_surface;
   ctx->create_vertex_elements_state = panfrost_create_vertex_elements;
   ctx->create_compute_state = panfrost_create_compute_state;
   ctx->create_tcs_state = panfrost_create_shader_state;
   ctx->create_tes_state = panfrost_create_shader_state;
   ctx->create_gs_state = panfrost_create_shader_state;
   ctx->create_vs_state = panfrost_create_shader_state;
   ctx->bind_blend_state = panfrost_bind_state;
   ctx->bind_depth_stencil_alpha_state = panfrost_bind_state;
   ctx->bind_sampler_states = panfrost_bind_sampler_states;
   ctx->bind_fs_state = panfrost_bind_state;
   ctx->bind_rasterizer_state = panfrost_bind_state;
   ctx->bind_vertex_elements_state = panfrost_bind_state;
   ctx->bind_compute_state = panfrost_bind_state;
   ctx->bind_tcs_state = panfrost_bind_state;
   ctx->bind_tes_state = panfrost_bind_state;
   ctx->bind_gs_state = panfrost_bind_state;
   ctx->bind_vs_state = panfrost_bind_state;
   ctx->delete_blend_state = panfrost_delete_state;
   ctx->delete_depth_stencil_alpha_state = panfrost_delete_state;
   ctx->delete_fs_state = panfrost_delete_state;
   ctx->delete_rasterizer_state = panfrost_delete_state;
   ctx->delete_sampler_state = panfrost_delete_state;
   ctx->delete_vertex_elements_state = panfrost_delete_state;
   ctx->delete_compute_state = panfrost_delete_state;
   ctx->delete_tcs_state = panfrost_delete_state;
   ctx->delete_tes_state = panfrost_delete_state;
   ctx->delete_gs_state = panfrost_delete_state;
   ctx->delete_vs_state = panfrost_delete_state;
   ctx->set_blend_color = panfrost_set_blend_color;
   ctx->set_clip_state = panfrost_set_clip_state;
   ctx->set_constant_buffer = panfrost_set_constant_buffer;
   ctx->set_sampler_views = panfrost_set_sampler_views;
   ctx->set_framebuffer_state = panfrost_set_framebuffer_state;
   ctx->set_polygon_stipple = panfrost_set_polygon_stipple;
   ctx->set_sample_mask = panfrost_set_sample_mask;
   ctx->set_scissor_states = panfrost_set_scissor_states;
   ctx->set_stencil_ref = panfrost_set_stencil_ref;
   ctx->set_vertex_buffers = panfrost_set_vertex_buffers;
   ctx->set_viewport_states = panfrost_set_viewport_states;
   ctx->sampler_view_destroy = panfrost_sampler_view_destroy;
   ctx->surface_destroy = panfrost_surface_destroy;
   ctx->draw_vbo = panfrost_draw_vbo;
   ctx->launch_grid = panfrost_launch_grid;
   ctx->create_stream_output_target = panfrost_create_stream_output_target;
   ctx->stream_output_target_destroy = panfrost_stream_output_target_destroy;
   ctx->set_stream_output_targets = panfrost_set_stream_output_targets;
}
