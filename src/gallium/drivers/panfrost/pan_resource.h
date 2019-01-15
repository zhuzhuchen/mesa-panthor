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

#ifndef PAN_RESOURCE_H
#define PAN_RESOURCE_H

#include <panfrost-job.h>
#include "pan_nondrm.h"
#include "pan_screen.h"
#include <drm.h>

/* Corresponds to pipe_resource for our hacky pre-DRM interface */

struct sw_displaytarget;

struct panfrost_bo {
	//struct panfrost_device *dev;
	uint32_t size;
	uint32_t handle;
	uint32_t name;
	int32_t refcnt;
	uint64_t iova;
	void *map;
	//const struct fd_bo_funcs *funcs;

	enum {
		NO_CACHE = 0,
		BO_CACHE = 1,
		RING_CACHE = 2,
	} bo_reuse;

	//struct list_head list;   /* bucket-list entry */
	time_t free_time;        /* time when added to bucket-list */
};

struct panfrost_resource {
        struct pipe_resource base;

        struct panfrost_bo *bo;
        struct renderonly_scanout *scanout;

        /* Address to the resource in question */

        uint8_t *cpu[MAX_MIP_LEVELS];

        /* Not necessarily a GPU mapping of cpu! In case of texture tiling, gpu
         * points to the GPU-side, tiled texture, while cpu points to the
         * CPU-side, untiled texture from mesa */

        mali_ptr gpu[MAX_MIP_LEVELS];

        /* Memory entry corresponding to gpu above */
        struct panfrost_memory_entry *entry[MAX_MIP_LEVELS];

        /* Is something other than level 0 ever written? */
        bool is_mipmap;

        struct sw_displaytarget *dt;

        /* Set for tiled, clear for linear. */
        bool tiled;

        /* If AFBC is enabled for this resource, we lug around an AFBC
         * metadata buffer as well. The actual AFBC resource is also in
         * afbc_slab (only defined for AFBC) at position afbc_main_offset */

        bool has_afbc;
        struct panfrost_memory afbc_slab;
        int afbc_metadata_size;

        /* Similarly for TE */
        bool has_checksum;
        struct panfrost_memory checksum_slab;
        int checksum_stride;
};

static inline struct panfrost_resource *
pan_resource(struct pipe_resource *p)
{
   return (struct panfrost_resource *)p;
}

void panfrost_resource_screen_init(struct panfrost_screen *screen);

void panfrost_resource_context_init(struct pipe_context *pctx);

#endif /* PAN_RESOURCE_H */
