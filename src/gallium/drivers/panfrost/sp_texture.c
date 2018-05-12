/**************************************************************************
 * 
 * Copyright 2006 VMware, Inc.
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
 /*
  * Authors:
  *   Keith Whitwell <keithw@vmware.com>
  *   Michel Dänzer <daenzer@vmware.com>
  */

#include "pipe/p_defines.h"
#include "util/u_inlines.h"

#include "util/u_format.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_transfer.h"
#include "util/u_surface.h"

#include "sp_texture.h"
#include "sp_screen.h"

#include "state_tracker/sw_winsys.h"

static boolean
softpipe_can_create_resource(struct pipe_screen *screen,
                             const struct pipe_resource *res)
{
   return TRUE;
}


#define __PAN_GALLIUM
#include <trans-builder.h>

/**
 * Create new pipe_resource given the template information.
 */
static struct pipe_resource *
softpipe_resource_create_front(struct pipe_screen *screen,
                               const struct pipe_resource *templat,
                               const void *map_front_private)
{
	return panfrost_resource_create_front(screen, templat, map_front_private);
}

static struct pipe_resource *
softpipe_resource_create(struct pipe_screen *screen,
                         const struct pipe_resource *templat)
{
   return softpipe_resource_create_front(screen, templat, NULL);
}

static void
softpipe_resource_destroy(struct pipe_screen *pscreen,
			  struct pipe_resource *pt)
{
   FREE(pt);
}


static struct pipe_resource *
softpipe_resource_from_handle(struct pipe_screen *screen,
                              const struct pipe_resource *templat,
                              struct winsys_handle *whandle,
                              unsigned usage)
{
	assert(0);
}


static boolean
softpipe_resource_get_handle(struct pipe_screen *screen,
                             struct pipe_context *ctx,
                             struct pipe_resource *pt,
                             struct winsys_handle *whandle,
                             unsigned usage)
{
	assert(0);
	return FALSE;
}

void
softpipe_init_screen_texture_funcs(struct pipe_screen *screen)
{
   screen->resource_create = softpipe_resource_create;
   screen->resource_create_front = softpipe_resource_create_front;
   screen->resource_destroy = softpipe_resource_destroy;
   screen->resource_from_handle = softpipe_resource_from_handle;
   screen->resource_get_handle = softpipe_resource_get_handle;
   screen->can_create_resource = softpipe_can_create_resource;
}
