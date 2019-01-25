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
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/mman.h>
#include <assert.h>
#include <xf86drm.h>

#include "util/u_format.h"
#include "util/u_memory.h"

#include <panfrost-mali-base.h>
#include <mali-kbase-ioctl.h>
#include <panfrost-misc.h>
#include "pan_nondrm.h"
#include "pan_resource.h"
#include "pan_context.h"
#include "pan_swizzle.h"

/* From the kernel module */

#define BASE_MEM_MAP_TRACKING_HANDLE (3ull << 12)

struct panfrost_nondrm {
	struct panfrost_driver base;
	int fd;
};

struct panfrost_nondrm_bo {
	struct panfrost_bo base;
};

static int
pandev_ioctl(int fd, unsigned long request, void *args)
{
        return ioctl(fd, request, args);
}

static int
pandev_general_allocate(int fd, int va_pages, int commit_pages,
                        int extent, int flags,
                        u64 *out, int *out_flags)
{
        int ret;
        union kbase_ioctl_mem_alloc args = {
                .in.va_pages = va_pages,
                .in.commit_pages = commit_pages,
                .in.extent = extent,
                .in.flags = flags,
        };

        ret = ioctl(fd, KBASE_IOCTL_MEM_ALLOC, &args);
        if (ret) {
                fprintf(stderr, "panfrost: Failed to allocate memory, va_pages=%d commit_pages=%d extent=%d flags=0x%x rc=%d\n",
                        va_pages, commit_pages, extent, flags, ret);
                abort();
        }
        *out = args.out.gpu_va;
        *out_flags = args.out.flags;

        return 0;
}

static int
pandev_standard_allocate(int fd, int va_pages, int flags,
                         u64 *out, int *out_flags)
{
        return pandev_general_allocate(fd, va_pages, va_pages, 0, flags, out,
                                       out_flags);
}

static struct panfrost_bo *
panfrost_nondrm_create_bo(struct panfrost_screen *screen, const struct pipe_resource *template)
{
	struct panfrost_nondrm_bo *bo = CALLOC_STRUCT(panfrost_nondrm_bo);
        int bytes_per_pixel = util_format_get_blocksize(template->format);
        int stride = bytes_per_pixel * template->width0; /* TODO: Alignment? */
        size_t sz = stride;

        if (template->height0) sz *= template->height0;

        if (template->depth0) sz *= template->depth0;

        if ((template->bind & PIPE_BIND_RENDER_TARGET) || (template->bind & PIPE_BIND_DEPTH_STENCIL)) {
		/* TODO: Mipmapped RTs */
		//assert(template->last_level == 0);

		/* Allocate the framebuffer as its own slab of GPU-accessible memory */
		struct panfrost_memory slab;
		screen->driver->allocate_slab(screen->any_context, &slab, (sz / 4096) + 1, false, 0, 0, 0);

		/* Make the resource out of the slab */
		bo->base.cpu[0] = slab.cpu;
		bo->base.gpu[0] = slab.gpu;
	} else {
                /* TODO: For linear resources, allocate straight on the cmdstream for
                 * zero-copy operation */

                /* Tiling textures is almost always faster, unless we only use it once */
                bo->base.tiled = (template->usage != PIPE_USAGE_STREAM) && (template->bind & PIPE_BIND_SAMPLER_VIEW);

                if (bo->base.tiled) {
                        /* For tiled, we don't map directly, so just malloc any old buffer */

                        for (int l = 0; l < (template->last_level + 1); ++l) {
                                bo->base.cpu[l] = malloc(sz);
                                //sz >>= 2;
                        }
                } else {
                        /* But for linear, we can! */

                        struct pb_slab_entry *entry = pb_slab_alloc(&screen->any_context->slabs, sz, HEAP_TEXTURE);
                        struct panfrost_memory_entry *p_entry = (struct panfrost_memory_entry *) entry;
                        struct panfrost_memory *backing = (struct panfrost_memory *) entry->slab;
                        bo->base.entry[0] = p_entry;
                        bo->base.cpu[0] = backing->cpu + p_entry->offset;
                        bo->base.gpu[0] = backing->gpu + p_entry->offset;

                        /* TODO: Mipmap */
                }
	}

        return &bo->base;
}

static struct panfrost_bo *
panfrost_nondrm_import_bo(struct panfrost_screen *screen, struct winsys_handle *whandle)
{
	struct panfrost_nondrm_bo *bo = CALLOC_STRUCT(panfrost_nondrm_bo);
	struct panfrost_nondrm *nondrm = (struct panfrost_nondrm *)screen->driver;
        struct drm_mode_map_dumb map_arg;
        int ret;
        unsigned gem_handle;
        union kbase_ioctl_mem_import framebuffer_import = {
                .in = {
                        .phandle = (uint64_t) (uintptr_t) &whandle->handle,
                        .type = BASE_MEM_IMPORT_TYPE_UMM,
                        .flags = BASE_MEM_PROT_CPU_RD |
                                 BASE_MEM_PROT_CPU_WR |
                                 BASE_MEM_PROT_GPU_RD |
                                 BASE_MEM_PROT_GPU_WR |
                                 BASE_MEM_IMPORT_SHARED,
                }
        };

        ret = pandev_ioctl(nondrm->fd, KBASE_IOCTL_MEM_IMPORT, &framebuffer_import);
        assert(ret == 0);

        bo->base.gpu[0] = mmap(NULL, framebuffer_import.out.va_pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, nondrm->fd, framebuffer_import.out.gpu_va);

        ret = drmPrimeFDToHandle(screen->ro->kms_fd, whandle->handle, &gem_handle);
        assert(ret >= 0);

        memset(&map_arg, 0, sizeof(map_arg));
        map_arg.handle = gem_handle;

        ret = drmIoctl(screen->ro->kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg);
        assert(!ret);

        bo->base.cpu[0] = mmap(NULL, framebuffer_import.out.va_pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, screen->ro->kms_fd, map_arg.offset);

        u64 addresses[1];
        addresses[0] = bo->base.gpu[0];
        struct kbase_ioctl_sticky_resource_map map = {
                .count = 1,
                .address = addresses,
        };
        ret = pandev_ioctl(nondrm->fd, KBASE_IOCTL_STICKY_RESOURCE_MAP, &map);
        assert(ret == 0);
        
        return &bo->base;
}

static uint8_t *
panfrost_nondrm_map_bo(struct panfrost_context *ctx, struct pipe_transfer *transfer)
{
	struct panfrost_nondrm_bo *bo = (struct panfrost_nondrm_bo *)pan_resource(transfer->resource)->bo;

        /* If non-zero level, it's a mipmapped resource and needs to be treated as such */
        bo->base.is_mipmap |= transfer->level;

        if (transfer->usage & PIPE_TRANSFER_MAP_DIRECTLY && bo->base.tiled) {
                /* We cannot directly map tiled textures */
                return NULL;
        }

        if (transfer->resource->bind & PIPE_BIND_DEPTH_STENCIL) {
                /* Mipmapped readpixels?! */
                assert(transfer->level == 0);

                /* Set the CPU mapping to that of the depth/stencil buffer in memory, untiled */
                bo->base.cpu[transfer->level] = ctx->depth_stencil_buffer.cpu;
        }

        return bo->base.cpu[transfer->level];
}

static void
panfrost_tile_texture(struct panfrost_context *ctx, struct panfrost_resource *rsrc, int level)
{
	struct panfrost_nondrm_bo *bo = (struct panfrost_nondrm_bo *)rsrc->bo;
        int bytes_per_pixel = util_format_get_blocksize(rsrc->base.format);
        int stride = bytes_per_pixel * rsrc->base.width0; /* TODO: Alignment? */

        int width = rsrc->base.width0 >> level;
        int height = rsrc->base.height0 >> level;

        /* Estimate swizzled bitmap size. Slight overestimates are fine.
         * Underestimates will result in memory corruption or worse. */

        int swizzled_sz = panfrost_swizzled_size(width, height, bytes_per_pixel);

        /* Allocate the transfer given that known size but do not copy */
        struct pb_slab_entry *entry = pb_slab_alloc(&ctx->slabs, swizzled_sz, HEAP_TEXTURE);
        struct panfrost_memory_entry *p_entry = (struct panfrost_memory_entry *) entry;
        struct panfrost_memory *backing = (struct panfrost_memory *) entry->slab;
        uint8_t *swizzled = backing->cpu + p_entry->offset;

        /* Save the entry. But if there was already an entry here (from a
         * previous upload of the resource), free that one so we don't leak */

        if (bo->base.entry[level] != NULL) {
                bo->base.entry[level]->freed = true;
                pb_slab_free(&ctx->slabs, &bo->base.entry[level]->base);
        }

        bo->base.entry[level] = p_entry;
        bo->base.gpu[level] = backing->gpu + p_entry->offset;

        /* Run actual texture swizzle, writing directly to the mapped
         * GPU chunk we allocated */

        panfrost_texture_swizzle(width, height, bytes_per_pixel, stride, bo->base.cpu[level], swizzled);
}

static void
panfrost_nondrm_unmap_bo(struct panfrost_context *ctx,
                         struct pipe_transfer *transfer)
{
	struct panfrost_nondrm_bo *bo = (struct panfrost_nondrm_bo *)pan_resource(transfer->resource)->bo;

        if (transfer->usage & PIPE_TRANSFER_WRITE) {
                if (transfer->resource->target == PIPE_TEXTURE_2D) {
                        struct panfrost_resource *prsrc = (struct panfrost_resource *) transfer->resource;

                        /* Gallium thinks writeback happens here; instead, this is our cue to tile */
                        if (bo->base.has_afbc) {
                                printf("Warning: writes to afbc surface can't possibly work out well for you...\n");
                        } else if (bo->base.tiled) {
                                panfrost_tile_texture(ctx, prsrc, transfer->level);
                        }
                }
        }
}

static void
panfrost_nondrm_destroy_bo(struct panfrost_screen *screen, struct panfrost_bo *pbo)
{
        struct panfrost_context *ctx = screen->any_context;
	struct panfrost_nondrm_bo *bo = (struct panfrost_nondrm_bo *)pbo;

        if (bo->base.tiled) {
                /* CPU is all malloc'ed, so just plain ol' free needed */

                for (int l = 0; bo->base.cpu[l]; l++) {
                        free(bo->base.cpu[l]);
                }
        } else if (bo->base.entry[0] != NULL) {
                bo->base.entry[0]->freed = true;
                pb_slab_free(&ctx->slabs, &bo->base.entry[0]->base);
        } else {
                printf("--leaking main allocation--\n");
        }

        if (bo->base.has_afbc) {
                /* TODO */
                printf("--leaking afbc--\n");
        }

        if (bo->base.has_checksum) {
                /* TODO */
                printf("--leaking afbc--\n");
        }
}

static void
panfrost_nondrm_submit_job(struct panfrost_context *ctx, mali_ptr addr, int nr_atoms)
{
        struct pipe_context *gallium = (struct pipe_context *) ctx;
        struct panfrost_screen *screen = panfrost_screen(gallium->screen);
	struct panfrost_nondrm *nondrm = (struct panfrost_nondrm *)screen->driver;

        struct kbase_ioctl_job_submit submit = {
                .addr = addr,
                .nr_atoms = nr_atoms,
                .stride = sizeof(struct base_jd_atom_v2),
        };

        if (pandev_ioctl(nondrm->fd, KBASE_IOCTL_JOB_SUBMIT, &submit))
                printf("Error submitting\n");
}

/* Forces a flush, to make sure everything is consistent.
 * Bad for parallelism. Necessary for glReadPixels etc. Use cautiously.
 */

static void
panfrost_nondrm_force_flush_fragment(struct panfrost_context *ctx)
{
        struct pipe_context *gallium = (struct pipe_context *) ctx;
        struct panfrost_screen *screen = panfrost_screen(gallium->screen);
	struct panfrost_nondrm *nondrm = (struct panfrost_nondrm *)screen->driver;
        struct base_jd_event_v2 event;
        int ret;

        if (!screen->last_fragment_flushed) {
                do {
                        ret = read(nondrm->fd, &event, sizeof(event));
                        if (ret != sizeof(event)) {
                            fprintf(stderr, "error when reading from mali device: %s\n", strerror(errno));
                            break;
                        }

                        if (event.event_code == BASE_JD_EVENT_JOB_INVALID) {
                            fprintf(stderr, "Job invalid\n");
                            break;
                        }
                } while (event.atom_number != screen->last_fragment_id);

                screen->last_fragment_flushed = true;
        }
}

static void
panfrost_nondrm_allocate_slab(struct panfrost_context *ctx,
		              struct panfrost_memory *mem,
		              size_t pages,
		              bool same_va,
		              int extra_flags,
		              int commit_count,
		              int extent)
{
        struct pipe_context *gallium = (struct pipe_context *) ctx;
        struct panfrost_screen *screen = panfrost_screen(gallium->screen);
	struct panfrost_nondrm *nondrm = (struct panfrost_nondrm *)screen->driver;
        int flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                    BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR;
        int out_flags;

        flags |= extra_flags;

        /* w+x are mutually exclusive */
        if (extra_flags & BASE_MEM_PROT_GPU_EX)
                flags &= ~BASE_MEM_PROT_GPU_WR;

        if (same_va)
                flags |= BASE_MEM_SAME_VA;

        if (commit_count || extent)
                pandev_general_allocate(nondrm->fd, pages,
                                        commit_count,
                                        extent, flags, &mem->gpu, &out_flags);
        else
                pandev_standard_allocate(nondrm->fd, pages, flags, &mem->gpu,
                                         &out_flags);

        mem->size = pages * 4096;

        /* The kernel can return a "cookie", long story short this means we
         * mmap
         */
        if (mem->gpu == 0x41000) {
                if ((mem->cpu = mmap(NULL, mem->size, 3, 1,
                                     nondrm->fd, mem->gpu)) == MAP_FAILED) {
                        perror("mmap");
                        abort();
                }
                mem->gpu = (mali_ptr)mem->cpu;
        }

        mem->stack_bottom = 0;
}

struct panfrost_driver *
panfrost_create_nondrm_driver(int fd)
{
	struct panfrost_nondrm *driver = CALLOC_STRUCT(panfrost_nondrm);
        struct kbase_ioctl_version_check version = { .major = 11, .minor = 11 };
        struct kbase_ioctl_set_flags set_flags = {};
        int ret;

	driver->fd = fd;

	driver->base.create_bo = panfrost_nondrm_create_bo;
	driver->base.import_bo = panfrost_nondrm_import_bo;
	driver->base.map_bo = panfrost_nondrm_map_bo;
	driver->base.unmap_bo = panfrost_nondrm_unmap_bo;
	driver->base.destroy_bo = panfrost_nondrm_destroy_bo;
	driver->base.submit_job = panfrost_nondrm_submit_job;
	driver->base.force_flush_fragment = panfrost_nondrm_force_flush_fragment;
	driver->base.allocate_slab = panfrost_nondrm_allocate_slab;

        ret = ioctl(fd, KBASE_IOCTL_VERSION_CHECK, &version);
        if (ret != 0) {
                fprintf(stderr, "Version check failed with %d (reporting UK %d.%d)\n",
                        ret, version.major, version.minor);
                abort();
        }
        printf("panfrost: Using kbase UK version %d.%d, fd %d\n", version.major, version.minor, fd);

        if (mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, BASE_MEM_MAP_TRACKING_HANDLE) == MAP_FAILED) {
                perror("mmap");
                abort();
        }
        ret = ioctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags);
        if (ret != 0) {
                fprintf(stderr, "Setting context flags failed with %d\n", ret);
                abort();
        }

        return &driver->base;
}
