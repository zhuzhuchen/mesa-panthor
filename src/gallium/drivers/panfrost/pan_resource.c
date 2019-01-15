/**************************************************************************
 *
 * Copyright 2008 VMware, Inc.
 * Copyright 2014 Broadcom
 * Copyright 2018 Alyssa Rosenzweig
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

#include <panfrost-mali-base.h>
#include <mali-kbase-ioctl.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <drm_fourcc.h>

#include "state_tracker/winsys_handle.h"
#include "util/u_format.h"
#include "util/u_memory.h"
#include "util/u_surface.h"
#include "util/u_transfer.h"
#include "util/u_transfer_helper.h"

#include "pan_context.h"
#include "pan_screen.h"
#include "pan_resource.h"
#include "pan_nondrm.h"
#include "pan_swizzle.h"

static struct pipe_resource *
panfrost_resource_from_handle(struct pipe_screen *pscreen,
                              const struct pipe_resource *templat,
                              struct winsys_handle *whandle,
                              unsigned usage)
{
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct drm_mode_map_dumb map_arg;
        struct panfrost_resource *rsc;
        struct pipe_resource *prsc;
        int ret;
        unsigned gem_handle;

        assert(whandle->type == WINSYS_HANDLE_TYPE_FD);

        rsc = CALLOC_STRUCT(panfrost_resource);
        if (!rsc)
                return NULL;

        prsc = &rsc->base;

        *prsc = *templat;

        pipe_reference_init(&prsc->reference, 1);
        prsc->screen = pscreen;

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

        ret = pandev_ioctl(screen->fd, KBASE_IOCTL_MEM_IMPORT, &framebuffer_import);
        assert(ret == 0);

        rsc->gpu[0] = mmap(NULL, framebuffer_import.out.va_pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, screen->fd, framebuffer_import.out.gpu_va);

        ret = drmPrimeFDToHandle(screen->ro->kms_fd, whandle->handle, &gem_handle);
        assert(ret >= 0);

        memset(&map_arg, 0, sizeof(map_arg));
        map_arg.handle = gem_handle;

        ret = drmIoctl(screen->ro->kms_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_arg);
        assert(!ret);

        rsc->cpu[0] = mmap(NULL, framebuffer_import.out.va_pages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, screen->ro->kms_fd, map_arg.offset);

        u64 addresses[1];
        addresses[0] = rsc->gpu[0];
        struct kbase_ioctl_sticky_resource_map map = {
                .count = 1,
                .address = addresses,
        };
        ret = pandev_ioctl(screen->fd, KBASE_IOCTL_STICKY_RESOURCE_MAP, &map);
        assert(ret == 0);

        return prsc;
}

static boolean
panfrost_resource_get_handle(struct pipe_screen *pscreen,
                             struct pipe_context *ctx,
                             struct pipe_resource *pt,
                             struct winsys_handle *handle,
                             unsigned usage)
{
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_resource *rsrc = (struct panfrost_resource *) pt;
        struct renderonly_scanout *scanout = rsrc->scanout;
        int bytes_per_pixel = util_format_get_blocksize(rsrc->base.format);
        int stride = bytes_per_pixel * rsrc->base.width0; /* TODO: Alignment? */

        handle->stride = stride;
        handle->modifier = DRM_FORMAT_MOD_INVALID;

        if (handle->type == WINSYS_HANDLE_TYPE_SHARED) {
                printf("Missed shared handle\n");
                return FALSE;
        } else if (handle->type == WINSYS_HANDLE_TYPE_KMS) {
                if (renderonly_get_handle(scanout, handle)) {
                        return TRUE;
                } else {
                        printf("Missed nonrenderonly KMS handle for resource %p with scanout %p\n", pt, scanout);
                        return FALSE;
                }
        } else if (handle->type == WINSYS_HANDLE_TYPE_FD) {
                if (scanout) {
                        struct drm_prime_handle args = {
                                .handle = scanout->handle,
                                .flags = DRM_CLOEXEC,
                        };

                        int ret = pandev_ioctl(screen->ro->kms_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
                        if (ret == -1)
                                return FALSE;

                        handle->handle = args.fd;

                        return TRUE;
                } else {
                        printf("Missed nonscanout FD handle\n");
                        assert(0);
                        return FALSE;
                }
        }

        return FALSE;
}

static void
panfrost_flush_resource(struct pipe_context *pctx, struct pipe_resource *prsc)
{
        //fprintf(stderr, "TODO %s\n", __func__);
}

static void
panfrost_blit(struct pipe_context *pipe,
              const struct pipe_blit_info *info)
{
        /* STUB */
        printf("Skipping blit XXX\n");
        return;
}

static struct pipe_surface *
panfrost_create_surface(struct pipe_context *pipe,
                        struct pipe_resource *pt,
                        const struct pipe_surface *surf_tmpl)
{
        struct pipe_surface *ps = NULL;

        ps = CALLOC_STRUCT(pipe_surface);

        if (ps) {
                pipe_reference_init(&ps->reference, 1);
                pipe_resource_reference(&ps->texture, pt);
                ps->context = pipe;
                ps->format = surf_tmpl->format;

                if (pt->target != PIPE_BUFFER) {
                        assert(surf_tmpl->u.tex.level <= pt->last_level);
                        ps->width = u_minify(pt->width0, surf_tmpl->u.tex.level);
                        ps->height = u_minify(pt->height0, surf_tmpl->u.tex.level);
                        ps->u.tex.level = surf_tmpl->u.tex.level;
                        ps->u.tex.first_layer = surf_tmpl->u.tex.first_layer;
                        ps->u.tex.last_layer = surf_tmpl->u.tex.last_layer;
                } else {
                        /* setting width as number of elements should get us correct renderbuffer width */
                        ps->width = surf_tmpl->u.buf.last_element - surf_tmpl->u.buf.first_element + 1;
                        ps->height = pt->height0;
                        ps->u.buf.first_element = surf_tmpl->u.buf.first_element;
                        ps->u.buf.last_element = surf_tmpl->u.buf.last_element;
                        assert(ps->u.buf.first_element <= ps->u.buf.last_element);
                        assert(ps->u.buf.last_element < ps->width);
                }
        }

        return ps;
}

static void
panfrost_surface_destroy(struct pipe_context *pipe,
                         struct pipe_surface *surf)
{
        assert(surf->texture);
        pipe_resource_reference(&surf->texture, NULL);
        free(surf);
}

/* TODO: Proper resource tracking depends on, well, proper resources. This
 * section will be woefully incomplete until we can sort out a proper DRM
 * driver. */

static struct pipe_resource *
panfrost_resource_create(struct pipe_screen *screen,
                         const struct pipe_resource *template)
{
        struct panfrost_resource *so = CALLOC_STRUCT(panfrost_resource);
        struct panfrost_screen *pscreen = (struct panfrost_screen *) screen;
        int bytes_per_pixel = util_format_get_blocksize(template->format);
        int stride = bytes_per_pixel * template->width0; /* TODO: Alignment? */

        so->base = *template;
        so->base.screen = screen;

        pipe_reference_init(&so->base.reference, 1);

        size_t sz = stride;

        if (template->height0) sz *= template->height0;

        if (template->depth0) sz *= template->depth0;

        /* Make sure we're familiar */
        switch (template->target) {
                case PIPE_BUFFER:
                case PIPE_TEXTURE_1D:
                case PIPE_TEXTURE_2D:
                case PIPE_TEXTURE_RECT:
                        break;
                default:
                        fprintf(stderr, "Unknown texture target %d\n", template->target);
                        assert(0);
        }

        if ((template->bind & PIPE_BIND_RENDER_TARGET) || (template->bind & PIPE_BIND_DEPTH_STENCIL)) {
                if (template->bind & PIPE_BIND_DISPLAY_TARGET ||
                    template->bind & PIPE_BIND_SCANOUT ||
                    template->bind & PIPE_BIND_SHARED) {
                        struct pipe_resource scanout_templat = *template;
                        struct renderonly_scanout *scanout;
                        struct winsys_handle handle;

                        /* TODO: align width0 and height0? */

                        scanout = renderonly_scanout_for_resource(&scanout_templat,
                                                                  pscreen->ro, &handle);
                        if (!scanout)
                                return NULL;

                        assert(handle.type == WINSYS_HANDLE_TYPE_FD);
                        /* TODO: handle modifiers? */
                        so = pan_resource(screen->resource_from_handle(screen, template,
                                                                         &handle,
                                                                         PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE));
                        close(handle.handle);
                        if (!so)
                                return NULL;

                        so->scanout = scanout;
                        pscreen->display_target = so;
                } else {
                        /* TODO: Mipmapped RTs */
                        //assert(template->last_level == 0);

                        /* Allocate the framebuffer as its own slab of GPU-accessible memory */
                        struct panfrost_memory slab;
                        panfrost_allocate_slab(pscreen->any_context, &slab, (sz / 4096) + 1, false, 0, 0, 0);

                        /* Make the resource out of the slab */
                        so->cpu[0] = slab.cpu;
                        so->gpu[0] = slab.gpu;
                }
        } else {
                /* TODO: For linear resources, allocate straight on the cmdstream for
                 * zero-copy operation */

                /* Tiling textures is almost always faster, unless we only use it once */
                so->tiled = (template->usage != PIPE_USAGE_STREAM) && (template->bind & PIPE_BIND_SAMPLER_VIEW);

                if (so->tiled) {
                        /* For tiled, we don't map directly, so just malloc any old buffer */

                        for (int l = 0; l < (template->last_level + 1); ++l) {
                                so->cpu[l] = malloc(sz);
                                //sz >>= 2;
                        }
                } else {
                        /* But for linear, we can! */

#if 0

                        struct panfrost_memory slab;
                        panfrost_allocate_slab(pscreen->any_context,
                                               &slab, (sz / 4096) + 1,
                                               true, 0, 0, 0);

                        /* Make the resource out of the slab */
                        so->cpu[0] = slab.cpu;
                        so->gpu[0] = slab.gpu;
#endif

                        struct pb_slab_entry *entry = pb_slab_alloc(&pscreen->any_context->slabs, sz, HEAP_TEXTURE);
                        struct panfrost_memory_entry *p_entry = (struct panfrost_memory_entry *) entry;
                        struct panfrost_memory *backing = (struct panfrost_memory *) entry->slab;
                        so->entry[0] = p_entry;
                        so->cpu[0] = backing->cpu + p_entry->offset;
                        so->gpu[0] = backing->gpu + p_entry->offset;

                        /* TODO: Mipmap */
                }
        }

        printf("Created resource %p with scanout %p\n", so, so->scanout);

        return (struct pipe_resource *)so;
}

static void
panfrost_resource_destroy(struct pipe_screen *screen,
                          struct pipe_resource *pt)
{
        struct panfrost_screen *pscreen = panfrost_screen(screen);
        struct panfrost_context *ctx = pscreen->any_context;
        struct panfrost_resource *rsrc = (struct panfrost_resource *) pt;

        if (rsrc->tiled) {
                /* CPU is all malloc'ed, so just plain ol' free needed */

                for (int l = 0; l < (rsrc->base.last_level + 1); ++l) {
                        free(rsrc->cpu[l]);
                }
        } else if (rsrc->entry[0] != NULL) {
                rsrc->entry[0]->freed = true;
                pb_slab_free(&ctx->slabs, &rsrc->entry[0]->base);
        } else {
                printf("--leaking main allocation--\n");
        }

        if (rsrc->has_afbc) {
                /* TODO */
                printf("--leaking afbc--\n");
        }

        if (rsrc->has_checksum) {
                /* TODO */
                printf("--leaking afbc--\n");
        }
}

static void *
panfrost_transfer_map(struct pipe_context *pctx,
                      struct pipe_resource *resource,
                      unsigned level,
                      unsigned usage,  /* a combination of PIPE_TRANSFER_x */
                      const struct pipe_box *box,
                      struct pipe_transfer **out_transfer)
{
        struct panfrost_context *ctx = panfrost_context(pctx);
        struct panfrost_resource *rsrc = (struct panfrost_resource *) resource;
        int bytes_per_pixel = util_format_get_blocksize(resource->format);
        int stride = bytes_per_pixel * resource->width0; /* TODO: Alignment? */

        struct pipe_transfer *transfer = CALLOC_STRUCT(pipe_transfer);
        transfer->level = level;
        transfer->usage = usage;
        transfer->box = *box;
        transfer->stride = stride;
        assert(!transfer->box.z);

        pipe_resource_reference(&transfer->resource, resource);

        *out_transfer = transfer;

        /* If non-zero level, it's a mipmapped resource and needs to be treated as such */
        rsrc->is_mipmap |= transfer->level;

        if (transfer->usage & PIPE_TRANSFER_MAP_DIRECTLY && rsrc->tiled) {
                /* We cannot directly map tiled textures */
                return NULL;
        }

        if (resource->bind & PIPE_BIND_DISPLAY_TARGET ||
            resource->bind & PIPE_BIND_SCANOUT ||
            resource->bind & PIPE_BIND_SHARED) {
                /* Mipmapped readpixels?! */
                assert(level == 0);

                /* Set the CPU mapping to that of the framebuffer in memory, untiled */
                rsrc->cpu[level] = rsrc->cpu[0];

                /* Force a flush -- kill the pipeline */
                panfrost_flush(pctx, NULL, PIPE_FLUSH_END_OF_FRAME);
        } else if (resource->bind & PIPE_BIND_DEPTH_STENCIL) {
                /* Mipmapped readpixels?! */
                assert(level == 0);

                /* Set the CPU mapping to that of the depth/stencil buffer in memory, untiled */
                rsrc->cpu[level] = ctx->depth_stencil_buffer.cpu;
        }

        return rsrc->cpu[level] + transfer->box.x * bytes_per_pixel + transfer->box.y * stride;
}

static void
panfrost_tile_texture(struct panfrost_context *ctx, struct panfrost_resource *rsrc, int level)
{
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

        if (rsrc->entry[level] != NULL) {
                rsrc->entry[level]->freed = true;
                pb_slab_free(&ctx->slabs, &rsrc->entry[level]->base);
        }

        rsrc->entry[level] = p_entry;
        rsrc->gpu[level] = backing->gpu + p_entry->offset;

        /* Run actual texture swizzle, writing directly to the mapped
         * GPU chunk we allocated */

        panfrost_texture_swizzle(width, height, bytes_per_pixel, stride, rsrc->cpu[level], swizzled);
}

static void
panfrost_transfer_unmap(struct pipe_context *pctx,
                        struct pipe_transfer *transfer)
{
        struct panfrost_context *ctx = panfrost_context(pctx);

        if (transfer->usage & PIPE_TRANSFER_WRITE) {
                if (transfer->resource->target == PIPE_TEXTURE_2D) {
                        struct panfrost_resource *prsrc = (struct panfrost_resource *) transfer->resource;

                        /* Gallium thinks writeback happens here; instead, this is our cue to tile */
                        if (prsrc->has_afbc) {
                                printf("Warning: writes to afbc surface can't possibly work out well for you...\n");
                        } else if (prsrc->tiled) {
                                panfrost_tile_texture(ctx, prsrc, transfer->level);
                        }
                }
        }

        /* Derefence the resource */
        pipe_resource_reference(&transfer->resource, NULL);

        /* Transfer itself is CALLOCed at the moment */
        free(transfer);
}

static void
panfrost_invalidate_resource(struct pipe_context *pctx, struct pipe_resource *prsc)
{
        //fprintf(stderr, "TODO %s\n", __func__);
}

static const struct u_transfer_vtbl transfer_vtbl = {
        .resource_create          = panfrost_resource_create,
        .resource_destroy         = panfrost_resource_destroy,
        .transfer_map             = panfrost_transfer_map,
        .transfer_unmap           = panfrost_transfer_unmap,
        .transfer_flush_region    = u_default_transfer_flush_region,
        //.get_internal_format      = panfrost_resource_get_internal_format,
        //.set_stencil              = panfrost_resource_set_stencil,
        //.get_stencil              = panfrost_resource_get_stencil,
};

void
panfrost_resource_screen_init(struct panfrost_screen *pscreen)
{
        //pscreen->base.resource_create_with_modifiers =
        //        panfrost_resource_create_with_modifiers;
        pscreen->base.resource_create = u_transfer_helper_resource_create;
        pscreen->base.resource_destroy = u_transfer_helper_resource_destroy;
        pscreen->base.resource_from_handle = panfrost_resource_from_handle;
        pscreen->base.resource_get_handle = panfrost_resource_get_handle;
        pscreen->base.transfer_helper = u_transfer_helper_create(&transfer_vtbl,
                                                            true, true,
                                                            true, true);
}

void
panfrost_resource_context_init(struct pipe_context *pctx)
{
        pctx->transfer_map = u_transfer_helper_transfer_map;
        pctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;
        pctx->transfer_unmap = u_transfer_helper_transfer_unmap;
        pctx->buffer_subdata = u_default_buffer_subdata;
        pctx->create_surface = panfrost_create_surface;
        pctx->surface_destroy = panfrost_surface_destroy;
        pctx->resource_copy_region = util_resource_copy_region;
        pctx->blit = panfrost_blit;
        //pctx->generate_mipmap = panfrost_generate_mipmap;
        pctx->flush_resource = panfrost_flush_resource;
        pctx->invalidate_resource = panfrost_invalidate_resource;
        pctx->transfer_flush_region = u_transfer_helper_transfer_flush_region;
        pctx->buffer_subdata = u_default_buffer_subdata;
        pctx->texture_subdata = u_default_texture_subdata;
}
