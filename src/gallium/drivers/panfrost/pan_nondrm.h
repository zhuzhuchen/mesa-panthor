/*
 * © Copyright 2017-2018 Alyssa Rosenzweig
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#ifndef __PANDEV_H__
#define __PANDEV_H__

#include <unistd.h>
#include <sys/mman.h>
#include <stdbool.h>
#include <panfrost-ioctl.h>
#include <panfrost-job.h>
#include <linux/ioctl.h>
#include "pan_slowfb.h"
#include "pipebuffer/pb_slab.h"

int pandev_open(void);

/* Calls used while replaying */
int pandev_raw_open(void);
u8 *pandev_map_mtp(int fd);
int pandev_ioctl(int fd, unsigned long request, void *args);

int pandev_standard_allocate(int fd, int va_pages, int flags, u64 *out);
int pandev_general_allocate(int fd, int va_pages, int commit_pages, int extent, int flags, u64 *out);

struct panfrost_context;
struct panfrost_shader_state;
void
panfrost_shader_compile(struct panfrost_context *ctx, struct mali_shader_meta *meta, const char *src, int type, struct panfrost_shader_state *state);

/* Texture memory */

#define HEAP_TEXTURE 0

/* Single-frame (transient) command stream memory, done at the block scale
 * rather than the individual cmdstream alllocation scale. We use pb_alloc for
 * pooling, but we have to implement our own logic atop the API for performance
 * reasons when considering many low-latency tiny heterogenous allocations */

#define HEAP_TRANSIENT 1

/* Represents a fat pointer for GPU-mapped memory, returned from the transient
 * allocator and not used for much else */

struct panfrost_transfer {
        uint8_t *cpu;
        mali_ptr gpu;
};

struct panfrost_memory {
        /* Subclassing slab object */
        struct pb_slab slab;

        /* Backing for the slab in memory */
        uint8_t *cpu;
        mali_ptr gpu;
        int stack_bottom;
        size_t size;
};

/* Slab entry sizes range from 2^min to 2^max. In this case, we range from 1k
 * to 16MB. Numbers are kind of arbitrary but these seem to work alright in
 * practice. */

#define MIN_SLAB_ENTRY_SIZE (10)
#define MAX_SLAB_ENTRY_SIZE (24)

struct panfrost_memory_entry {
        /* Subclass */
        struct pb_slab_entry base;

        /* Have we been freed? */
        bool freed;

        /* Offset into the slab of the entry */
        off_t offset;
};

/* Functions for replay */
mali_ptr pandev_upload(int cheating_offset, int *stack_bottom, mali_ptr base, void *base_map, const void *data, size_t sz, bool no_pad);
mali_ptr pandev_upload_sequential(mali_ptr base, void *base_map, const void *data, size_t sz);

/* Functions for the actual Galliumish driver */
mali_ptr panfrost_upload(struct panfrost_memory *mem, const void *data, size_t sz, bool no_pad);
mali_ptr panfrost_upload_sequential(struct panfrost_memory *mem, const void *data, size_t sz);

struct panfrost_transfer
panfrost_allocate_transient(struct panfrost_context *ctx, size_t sz);

mali_ptr
panfrost_upload_transient(struct panfrost_context *ctx, const void *data, size_t sz);

void *
panfrost_allocate_transfer(struct panfrost_memory *mem, size_t sz, mali_ptr *gpu);

static inline mali_ptr
panfrost_reserve(struct panfrost_memory *mem, size_t sz)
{
        mem->stack_bottom += sz;
        return mem->gpu + (mem->stack_bottom - sz);
}

#include <math.h>
#define inff INFINITY

#define R(...) #__VA_ARGS__
#define ALIGN(x, y) (((x) + ((y) - 1)) & ~((y) - 1))

#endif /* __PANDEV_H__ */
