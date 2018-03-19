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
#include "util/u_format.h"
#include "util/u_upload_mgr.h"
#include "panfrost_public.h"

DEBUG_GET_ONCE_BOOL_OPTION(panfrost, "GALLIUM_NOOP", FALSE)

void panfrost_init_state_functions(struct pipe_context *ctx);

struct panfrost_pipe_screen {
   struct pipe_screen	pscreen;
   struct pipe_screen	*oscreen;
};

/*
 * query
 */
struct panfrost_query {
   unsigned	query;
};
static struct pipe_query *panfrost_create_query(struct pipe_context *ctx, unsigned query_type, unsigned index)
{
   struct panfrost_query *query = CALLOC_STRUCT(panfrost_query);

   return (struct pipe_query *)query;
}

static void panfrost_destroy_query(struct pipe_context *ctx, struct pipe_query *query)
{
   FREE(query);
}

static boolean panfrost_begin_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static bool panfrost_end_query(struct pipe_context *ctx, struct pipe_query *query)
{
   return true;
}

static boolean panfrost_get_query_result(struct pipe_context *ctx,
                                     struct pipe_query *query,
                                     boolean wait,
                                     union pipe_query_result *vresult)
{
   uint64_t *result = (uint64_t*)vresult;

   *result = 0;
   return TRUE;
}

static void
panfrost_set_active_query_state(struct pipe_context *pipe, boolean enable)
{
}


/*
 * resource
 */
struct panfrost_resource {
   struct pipe_resource	base;
   unsigned		size;
   char			*data;
   struct sw_displaytarget	*dt;
};

static struct pipe_resource *panfrost_resource_create(struct pipe_screen *screen,
                                                  const struct pipe_resource *templ)
{
   struct panfrost_resource *nresource;
   unsigned stride;

   nresource = CALLOC_STRUCT(panfrost_resource);
   if (!nresource)
      return NULL;

   stride = util_format_get_stride(templ->format, templ->width0);
   nresource->base = *templ;
   nresource->base.screen = screen;
   nresource->size = stride * templ->height0 * templ->depth0;
   nresource->data = MALLOC(nresource->size);
   pipe_reference_init(&nresource->base.reference, 1);
   if (nresource->data == NULL) {
      FREE(nresource);
      return NULL;
   }
   return &nresource->base;
}

static struct pipe_resource *panfrost_resource_from_handle(struct pipe_screen *screen,
                                                       const struct pipe_resource *templ,
                                                       struct winsys_handle *handle,
                                                       unsigned usage)
{
   struct panfrost_pipe_screen *panfrost_screen = (struct panfrost_pipe_screen*)screen;
   struct pipe_screen *oscreen = panfrost_screen->oscreen;
   struct pipe_resource *result;
   struct pipe_resource *panfrost_resource;

   result = oscreen->resource_from_handle(oscreen, templ, handle, usage);
   panfrost_resource = panfrost_resource_create(screen, result);
   pipe_resource_reference(&result, NULL);
   return panfrost_resource;
}

static boolean panfrost_resource_get_handle(struct pipe_screen *pscreen,
                                        struct pipe_context *ctx,
                                        struct pipe_resource *resource,
                                        struct winsys_handle *handle,
                                        unsigned usage)
{
   struct panfrost_pipe_screen *panfrost_screen = (struct panfrost_pipe_screen*)pscreen;
   struct pipe_screen *screen = panfrost_screen->oscreen;
   struct pipe_resource *tex;
   bool result;

   /* resource_get_handle musn't fail. Just create something and return it. */
   tex = screen->resource_create(screen, resource);
   if (!tex)
      return false;

   result = screen->resource_get_handle(screen, NULL, tex, handle, usage);
   pipe_resource_reference(&tex, NULL);
   return result;
}

static void panfrost_resource_destroy(struct pipe_screen *screen,
                                  struct pipe_resource *resource)
{
   struct panfrost_resource *nresource = (struct panfrost_resource *)resource;

   FREE(nresource->data);
   FREE(resource);
}


/*
 * transfer
 */
static void *panfrost_transfer_map(struct pipe_context *pipe,
                               struct pipe_resource *resource,
                               unsigned level,
                               enum pipe_transfer_usage usage,
                               const struct pipe_box *box,
                               struct pipe_transfer **ptransfer)
{
   struct pipe_transfer *transfer;
   struct panfrost_resource *nresource = (struct panfrost_resource *)resource;

   transfer = CALLOC_STRUCT(pipe_transfer);
   if (!transfer)
      return NULL;
   pipe_resource_reference(&transfer->resource, resource);
   transfer->level = level;
   transfer->usage = usage;
   transfer->box = *box;
   transfer->stride = 1;
   transfer->layer_stride = 1;
   *ptransfer = transfer;

   return nresource->data;
}

static void panfrost_transfer_flush_region(struct pipe_context *pipe,
                                       struct pipe_transfer *transfer,
                                       const struct pipe_box *box)
{
}

static void panfrost_transfer_unmap(struct pipe_context *pipe,
                                struct pipe_transfer *transfer)
{
   pipe_resource_reference(&transfer->resource, NULL);
   FREE(transfer);
}

static void panfrost_buffer_subdata(struct pipe_context *pipe,
                                struct pipe_resource *resource,
                                unsigned usage, unsigned offset,
                                unsigned size, const void *data)
{
}

static void panfrost_texture_subdata(struct pipe_context *pipe,
                                 struct pipe_resource *resource,
                                 unsigned level,
                                 unsigned usage,
                                 const struct pipe_box *box,
                                 const void *data,
                                 unsigned stride,
                                 unsigned layer_stride)
{
}


/*
 * clear/copy
 */
static void panfrost_clear(struct pipe_context *ctx, unsigned buffers,
                       const union pipe_color_union *color, double depth, unsigned stencil)
{
}

static void panfrost_clear_render_target(struct pipe_context *ctx,
                                     struct pipe_surface *dst,
                                     const union pipe_color_union *color,
                                     unsigned dstx, unsigned dsty,
                                     unsigned width, unsigned height,
                                     bool render_condition_enabled)
{
}

static void panfrost_clear_depth_stencil(struct pipe_context *ctx,
                                     struct pipe_surface *dst,
                                     unsigned clear_flags,
                                     double depth,
                                     unsigned stencil,
                                     unsigned dstx, unsigned dsty,
                                     unsigned width, unsigned height,
                                     bool render_condition_enabled)
{
}

static void panfrost_resource_copy_region(struct pipe_context *ctx,
                                      struct pipe_resource *dst,
                                      unsigned dst_level,
                                      unsigned dstx, unsigned dsty, unsigned dstz,
                                      struct pipe_resource *src,
                                      unsigned src_level,
                                      const struct pipe_box *src_box)
{
}


static void panfrost_blit(struct pipe_context *ctx,
                      const struct pipe_blit_info *info)
{
}


static void
panfrost_flush_resource(struct pipe_context *ctx,
                    struct pipe_resource *resource)
{
}


/*
 * context
 */
static void panfrost_flush(struct pipe_context *ctx,
                       struct pipe_fence_handle **fence,
                       unsigned flags)
{
   if (fence)
      *fence = NULL;
}

static void panfrost_destroy_context(struct pipe_context *ctx)
{
   if (ctx->stream_uploader)
      u_upload_destroy(ctx->stream_uploader);

   FREE(ctx);
}

static boolean panfrost_generate_mipmap(struct pipe_context *ctx,
                                    struct pipe_resource *resource,
                                    enum pipe_format format,
                                    unsigned base_level,
                                    unsigned last_level,
                                    unsigned first_layer,
                                    unsigned last_layer)
{
   return true;
}

static struct pipe_context *panfrost_create_context(struct pipe_screen *screen,
                                                void *priv, unsigned flags)
{
   struct pipe_context *ctx = CALLOC_STRUCT(pipe_context);

   if (!ctx)
      return NULL;

   ctx->screen = screen;
   ctx->priv = priv;

   ctx->stream_uploader = u_upload_create_default(ctx);
   if (!ctx->stream_uploader) {
      FREE(ctx);
      return NULL;
   }
   ctx->const_uploader = ctx->stream_uploader;

   ctx->destroy = panfrost_destroy_context;
   ctx->flush = panfrost_flush;
   ctx->clear = panfrost_clear;
   ctx->clear_render_target = panfrost_clear_render_target;
   ctx->clear_depth_stencil = panfrost_clear_depth_stencil;
   ctx->resource_copy_region = panfrost_resource_copy_region;
   ctx->generate_mipmap = panfrost_generate_mipmap;
   ctx->blit = panfrost_blit;
   ctx->flush_resource = panfrost_flush_resource;
   ctx->create_query = panfrost_create_query;
   ctx->destroy_query = panfrost_destroy_query;
   ctx->begin_query = panfrost_begin_query;
   ctx->end_query = panfrost_end_query;
   ctx->get_query_result = panfrost_get_query_result;
   ctx->set_active_query_state = panfrost_set_active_query_state;
   ctx->transfer_map = panfrost_transfer_map;
   ctx->transfer_flush_region = panfrost_transfer_flush_region;
   ctx->transfer_unmap = panfrost_transfer_unmap;
   ctx->buffer_subdata = panfrost_buffer_subdata;
   ctx->texture_subdata = panfrost_texture_subdata;
   panfrost_init_state_functions(ctx);

   return ctx;
}


/*
 * pipe_screen
 */
static void panfrost_flush_frontbuffer(struct pipe_screen *_screen,
                                   struct pipe_resource *resource,
                                   unsigned level, unsigned layer,
                                   void *context_private, struct pipe_box *box)
{
}

static const char *panfrost_get_vendor(struct pipe_screen* pscreen)
{
   return "X.Org";
}

static const char *panfrost_get_device_vendor(struct pipe_screen* pscreen)
{
   return "NONE";
}

static const char *panfrost_get_name(struct pipe_screen* pscreen)
{
   return "NOOP";
}

static int panfrost_get_param(struct pipe_screen* pscreen, enum pipe_cap param)
{
   struct pipe_screen *screen = ((struct panfrost_pipe_screen*)pscreen)->oscreen;

   return screen->get_param(screen, param);
}

static float panfrost_get_paramf(struct pipe_screen* pscreen,
                             enum pipe_capf param)
{
   struct pipe_screen *screen = ((struct panfrost_pipe_screen*)pscreen)->oscreen;

   return screen->get_paramf(screen, param);
}

static int panfrost_get_shader_param(struct pipe_screen* pscreen,
                                 enum pipe_shader_type shader,
                                 enum pipe_shader_cap param)
{
   struct pipe_screen *screen = ((struct panfrost_pipe_screen*)pscreen)->oscreen;

   return screen->get_shader_param(screen, shader, param);
}

static int panfrost_get_compute_param(struct pipe_screen *pscreen,
                                  enum pipe_shader_ir ir_type,
                                  enum pipe_compute_cap param,
                                  void *ret)
{
   struct pipe_screen *screen = ((struct panfrost_pipe_screen*)pscreen)->oscreen;

   return screen->get_compute_param(screen, ir_type, param, ret);
}

static boolean panfrost_is_format_supported(struct pipe_screen* pscreen,
                                        enum pipe_format format,
                                        enum pipe_texture_target target,
                                        unsigned sample_count,
                                        unsigned usage)
{
   struct pipe_screen *screen = ((struct panfrost_pipe_screen*)pscreen)->oscreen;

   return screen->is_format_supported(screen, format, target, sample_count, usage);
}

static uint64_t panfrost_get_timestamp(struct pipe_screen *pscreen)
{
   return 0;
}

static void panfrost_destroy_screen(struct pipe_screen *screen)
{
   struct panfrost_pipe_screen *panfrost_screen = (struct panfrost_pipe_screen*)screen;
   struct pipe_screen *oscreen = panfrost_screen->oscreen;

   oscreen->destroy(oscreen);
   FREE(screen);
}

static void panfrost_fence_reference(struct pipe_screen *screen,
                          struct pipe_fence_handle **ptr,
                          struct pipe_fence_handle *fence)
{
}

static boolean panfrost_fence_finish(struct pipe_screen *screen,
                                 struct pipe_context *ctx,
                                 struct pipe_fence_handle *fence,
                                 uint64_t timeout)
{
   return true;
}

static void panfrost_query_memory_info(struct pipe_screen *pscreen,
                                   struct pipe_memory_info *info)
{
   struct panfrost_pipe_screen *panfrost_screen = (struct panfrost_pipe_screen*)pscreen;
   struct pipe_screen *screen = panfrost_screen->oscreen;

   screen->query_memory_info(screen, info);
}

struct pipe_screen *panfrost_screen_create(struct pipe_screen *oscreen)
{
   struct panfrost_pipe_screen *panfrost_screen;
   struct pipe_screen *screen;

   if (!debug_get_option_panfrost()) {
      return oscreen;
   }

   panfrost_screen = CALLOC_STRUCT(panfrost_pipe_screen);
   if (!panfrost_screen) {
      return NULL;
   }
   panfrost_screen->oscreen = oscreen;
   screen = &panfrost_screen->pscreen;

   screen->destroy = panfrost_destroy_screen;
   screen->get_name = panfrost_get_name;
   screen->get_vendor = panfrost_get_vendor;
   screen->get_device_vendor = panfrost_get_device_vendor;
   screen->get_param = panfrost_get_param;
   screen->get_shader_param = panfrost_get_shader_param;
   screen->get_compute_param = panfrost_get_compute_param;
   screen->get_paramf = panfrost_get_paramf;
   screen->is_format_supported = panfrost_is_format_supported;
   screen->context_create = panfrost_create_context;
   screen->resource_create = panfrost_resource_create;
   screen->resource_from_handle = panfrost_resource_from_handle;
   screen->resource_get_handle = panfrost_resource_get_handle;
   screen->resource_destroy = panfrost_resource_destroy;
   screen->flush_frontbuffer = panfrost_flush_frontbuffer;
   screen->get_timestamp = panfrost_get_timestamp;
   screen->fence_reference = panfrost_fence_reference;
   screen->fence_finish = panfrost_fence_finish;
   screen->query_memory_info = panfrost_query_memory_info;

   return screen;
}
