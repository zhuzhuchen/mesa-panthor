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

#ifndef SP_CONTEXT_H
#define SP_CONTEXT_H

#include "pipe/p_context.h"
#include "util/u_blitter.h"

#include "draw/draw_vertex.h"

/** Do polygon stipple in the draw module? */
#define DO_PSTIPPLE_IN_DRAW_MODULE 0

/** Do polygon stipple with the util module? */
#define DO_PSTIPPLE_IN_HELPER_MODULE 1


struct softpipe_vbuf_render;
struct draw_context;
struct draw_stage;
struct sp_fragment_shader;
struct sp_vertex_shader;
struct sp_velems_state;
struct sp_so_state;

struct softpipe_context {
   struct pipe_context pipe;  /**< base class */

   struct pipe_context* panfrost;  /** Hacked in driver context */
};


static inline struct softpipe_context *
softpipe_context( struct pipe_context *pipe )
{
   return (struct softpipe_context *)pipe;
}


struct pipe_context *
softpipe_create_context(struct pipe_screen *, void *priv, unsigned flags);

struct pipe_resource *
softpipe_user_buffer_create(struct pipe_screen *screen,
                            void *ptr,
                            unsigned bytes,
			    unsigned bind_flags);

#define SP_UNREFERENCED         0
#define SP_REFERENCED_FOR_READ  (1 << 0)
#define SP_REFERENCED_FOR_WRITE (1 << 1)

unsigned int
softpipe_is_resource_referenced( struct pipe_context *pipe,
                                 struct pipe_resource *texture,
                                 unsigned level, int layer);

#endif /* SP_CONTEXT_H */
