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

#ifndef PAN_SCREEN_H
#define PAN_SCREEN_H

#include "pipe/p_screen.h"
#include "pipe/p_defines.h"
#include "renderonly/renderonly.h"

#include <panfrost-misc.h>
#include "pan_allocate.h"

struct panfrost_context;
struct panfrost_resource;
struct panfrost_screen;

#define DUMP_PERFORMANCE_COUNTERS

struct panfrost_driver {
	struct panfrost_bo * (*create_bo) (struct panfrost_screen *screen, const struct pipe_resource *template);
	struct panfrost_bo * (*import_bo) (struct panfrost_screen *screen, struct winsys_handle *whandle);
	uint8_t * (*map_bo) (struct panfrost_context *ctx, struct pipe_transfer *transfer);
	void (*unmap_bo) (struct panfrost_context *ctx, struct pipe_transfer *transfer);
	void (*destroy_bo) (struct panfrost_screen *screen, struct panfrost_bo *bo);

	void (*submit_job) (struct panfrost_context *ctx, mali_ptr addr, int nr_atoms);
	void (*force_flush_fragment) (struct panfrost_context *ctx);
	void (*allocate_slab) (struct panfrost_screen *screen,
		               struct panfrost_memory *mem,
		               size_t pages,
		               bool same_va,
		               int extra_flags,
		               int commit_count,
		               int extent);
	void (*enable_counters) (struct panfrost_screen *screen);
};

struct panfrost_screen {
        struct pipe_screen base;

        struct renderonly *ro;
        struct panfrost_driver *driver;

        struct panfrost_context *any_context;

        struct panfrost_memory perf_counters;
        
        /* TODO: Where? */
        struct panfrost_resource *display_target;

	int last_fragment_id;
	int last_fragment_flushed;
};

static inline struct panfrost_screen *
panfrost_screen( struct pipe_screen *pipe )
{
        return (struct panfrost_screen *)pipe;
}

#endif /* PAN_SCREEN_H */
