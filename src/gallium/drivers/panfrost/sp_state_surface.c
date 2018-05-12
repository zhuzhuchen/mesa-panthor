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

#include "sp_context.h"
#include "sp_state.h"
#include "sp_screen.h"
#include "sp_texture.h"
#include "state_tracker/sw_winsys.h"

#include "draw/draw_context.h"

#include "util/u_format.h"
#include "util/u_inlines.h"

#include "trans-builder.h"

void
softpipe_set_framebuffer_state(struct pipe_context *pipe,
                               const struct pipe_framebuffer_state *fb)
{
   struct panfrost_context *sp = softpipe_context(pipe)->panfrost;
   int i;

   for (i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
      struct pipe_surface *cb = i < fb->nr_cbufs ? fb->cbufs[i] : NULL;

      /* check if changing cbuf */
      if (sp->pipe_framebuffer.cbufs[i] != cb) {
	      if (i != 0) {
		      printf("XXX: Multiple render targets not supported before t7xx!\n");
		      break;
	      }

         /* assign new */
         pipe_surface_reference(&sp->pipe_framebuffer.cbufs[i], cb);
	 
	 struct softpipe_screen* scr = pipe->screen;
	 struct sw_winsys *winsys = scr->winsys;
	 struct pipe_surface *surf = sp->pipe_framebuffer.cbufs[i];

	 uint8_t *map = winsys->displaytarget_map(winsys, ((struct softpipe_resource*) surf->texture)->dt, PIPE_TRANSFER_WRITE);
	 trans_setup_framebuffer(sp, map, fb->width, fb->height);
      }
   }

   sp->pipe_framebuffer.nr_cbufs = fb->nr_cbufs;

   sp->pipe_framebuffer.width = fb->width;
   sp->pipe_framebuffer.height = fb->height;
   sp->pipe_framebuffer.samples = fb->samples;
   sp->pipe_framebuffer.layers = fb->layers;
}
