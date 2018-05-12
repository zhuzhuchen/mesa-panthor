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

#include "util/u_format.h"
#include "util/u_surface.h"
#include "sp_context.h"
#include "sp_surface.h"
#include "sp_query.h"

static void sp_blit(struct pipe_context *pipe,
                    const struct pipe_blit_info *info)
{
   struct softpipe_context *sp = softpipe_context(pipe);
}

static void
sp_flush_resource(struct pipe_context *pipe,
                  struct pipe_resource *resource)
{
}

static void
softpipe_clear_render_target(struct pipe_context *pipe,
                             struct pipe_surface *dst,
                             const union pipe_color_union *color,
                             unsigned dstx, unsigned dsty,
                             unsigned width, unsigned height,
                             bool render_condition_enabled)
{
   struct softpipe_context *softpipe = softpipe_context(pipe);

   if (render_condition_enabled && !softpipe_check_render_cond(softpipe))
      return;

   util_clear_render_target(pipe, dst, color,
                            dstx, dsty, width, height);
}


static void
softpipe_clear_depth_stencil(struct pipe_context *pipe,
                             struct pipe_surface *dst,
                             unsigned clear_flags,
                             double depth,
                             unsigned stencil,
                             unsigned dstx, unsigned dsty,
                             unsigned width, unsigned height,
                             bool render_condition_enabled)
{
   struct softpipe_context *softpipe = softpipe_context(pipe);

   if (render_condition_enabled && !softpipe_check_render_cond(softpipe))
      return;

   util_clear_depth_stencil(pipe, dst, clear_flags,
                            depth, stencil,
                            dstx, dsty, width, height);
}


void
sp_init_surface_functions(struct softpipe_context *sp)
{
   sp->pipe.resource_copy_region = util_resource_copy_region;
   sp->pipe.clear_render_target = softpipe_clear_render_target;
   sp->pipe.clear_depth_stencil = softpipe_clear_depth_stencil;
   sp->pipe.blit = sp_blit;
   sp->pipe.flush_resource = sp_flush_resource;
}
