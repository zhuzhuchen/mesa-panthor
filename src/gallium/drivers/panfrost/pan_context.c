/*
 * © Copyright 2018 Alyssa Rosenzweig
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

#include <sys/poll.h>

#include "pan_context.h"
#include "pan_swizzle.h"

#include "util/macros.h"
#include "util/u_format.h"
#include "util/u_inlines.h"
#include "util/u_surface.h"
#include "util/u_upload_mgr.h"
#include "util/u_transfer.h"
#include "util/u_transfer_helper.h"
#include "util/u_memory.h"
#include "indices/u_primconvert.h"
#include "tgsi/tgsi_parse.h"

#include "pan_screen.h"
#include "pan_blending.h"
#include "pan_blend_shaders.h"

static void
panfrost_flush(
        struct pipe_context *pipe,
        struct pipe_fence_handle **fence,
        unsigned flags);

static void
panfrost_allocate_slab(struct panfrost_context *ctx,
                       struct panfrost_memory *mem,
                       size_t pages,
                       bool mapped,
                       bool same_va,
                       int extra_flags,
                       int commit_count,
                       int extent);


static bool USE_TRANSACTION_ELIMINATION = false;

/* Don't use the mesa winsys; use our own X11 window with Xshm */
#define USE_SLOWFB

/* Do not actually send anything to the GPU; merely generate the cmdstream as fast as possible. Disables framebuffer writes */
//#define DRY_RUN

#define SET_BIT(lval, bit, cond) \
	if (cond) \
		lval |= (bit); \
	else \
		lval &= ~(bit);

/* MSAA is not supported in sw_winsys but it does make for nicer demos ;) so we
 * can force it regardless of gallium saying we don't have it */
static bool FORCE_MSAA = true;

/* Descriptor is generated along with the shader compiler */

static void
panfrost_upload_varyings_descriptor(struct panfrost_context *ctx)
{
        /* First, upload gl_Position varyings */
        mali_ptr gl_Position = panfrost_upload(&ctx->cmdstream_persistent, ctx->vs->varyings.vertex_only_varyings, sizeof(ctx->vs->varyings.vertex_only_varyings), true);

        /* Then, upload normal varyings for vertex shaders */
        panfrost_upload_sequential(&ctx->cmdstream_persistent, ctx->vs->varyings.varyings, sizeof(ctx->vs->varyings.varyings[0]) * ctx->vs->varyings.varying_count);

        /* Then, upload normal varyings for fragment shaders (duplicating) */
        mali_ptr varyings_fragment = panfrost_upload_sequential(&ctx->cmdstream_persistent, ctx->vs->varyings.varyings, sizeof(ctx->vs->varyings.varyings[0]) * ctx->vs->varyings.varying_count);

        /* Finally, upload gl_FragCoord varying */
        panfrost_upload_sequential(&ctx->cmdstream_persistent, ctx->vs->varyings.fragment_only_varyings, sizeof(ctx->vs->varyings.fragment_only_varyings[0]) * ctx->vs->varyings.fragment_only_varying_count);

        ctx->payload_vertex.postfix.varying_meta = gl_Position;
        ctx->payload_tiler.postfix.varying_meta = varyings_fragment;
}

/* TODO: Sample size, etc */

static void
panfrost_set_framebuffer_msaa(struct panfrost_context *ctx, bool enabled)
{
        enabled = false;

        SET_BIT(ctx->fragment_shader_core.unknown2_3, MALI_HAS_MSAA, enabled);
        SET_BIT(ctx->fragment_shader_core.unknown2_4, MALI_NO_MSAA, !enabled);

#ifdef SFBD
        SET_BIT(ctx->fragment_fbd.format, MALI_FRAMEBUFFER_MSAA_A | MALI_FRAMEBUFFER_MSAA_B, enabled);
#else
        SET_BIT(ctx->fragment_rts[0].format, MALI_MFBD_FORMAT_MSAA, enabled);

        SET_BIT(ctx->fragment_fbd.unk1, (1 << 4) | (1 << 1), enabled);

        /* XXX */
        ctx->fragment_fbd.rt_count_2 = enabled ? 4 : 1;
#endif
}

/* AFBC is enabled on a per-resource basis (AFBC enabling is theoretically
 * indepdent between color buffers and depth/stencil). To enable, we allocate
 * the AFBC metadata buffer and mark that it is enabled. We do -not- actually
 * edit the fragment job here. This routine should be called ONCE per
 * AFBC-compressed buffer, rather than on every frame. */

static void
panfrost_enable_afbc(struct panfrost_context *ctx, struct panfrost_resource *rsrc, bool ds)
{
#ifdef MFBD
        /* AFBC metadata is 16 bytes per tile */
        int tile_w = (rsrc->base.width0 + (MALI_TILE_LENGTH - 1)) >> MALI_TILE_SHIFT;
        int tile_h = (rsrc->base.height0 + (MALI_TILE_LENGTH - 1)) >> MALI_TILE_SHIFT;

        rsrc->stride *= 2;
        int main_size = rsrc->stride * rsrc->base.height0;
        rsrc->afbc_metadata_size = tile_w * tile_h * 16;

        /* Allocate the AFBC slab itself, large enough to hold the above */
        panfrost_allocate_slab(ctx, &rsrc->afbc_slab, (rsrc->afbc_metadata_size + main_size + 4095) / 4096, true, true, 0, 0, 0);

        rsrc->has_afbc = true;

        /* Compressed textured reads use a tagged pointer to the metadata */

        rsrc->gpu[0] = rsrc->afbc_slab.gpu | (ds ? 0 : 1);
        rsrc->cpu[0] = rsrc->afbc_slab.cpu;
#else
        printf("AFBC not supported yet on SFBD\n");
        assert(0);
#endif
}

static void
panfrost_enable_checksum(struct panfrost_context *ctx, struct panfrost_resource *rsrc)
{
        int tile_w = (rsrc->base.width0 + (MALI_TILE_LENGTH - 1)) >> MALI_TILE_SHIFT;
        int tile_h = (rsrc->base.height0 + (MALI_TILE_LENGTH - 1)) >> MALI_TILE_SHIFT;

        /* 8 byte checksum per tile */
        rsrc->checksum_stride = tile_w * 8;
        int pages = (((rsrc->checksum_stride * tile_h) + 4095) / 4096);
        panfrost_allocate_slab(ctx, &rsrc->checksum_slab, pages, false, false, 0, 0, 0);

        rsrc->has_checksum = true;
}

/* ..by contrast, this routine runs for every FRAGMENT job, but does no
 * allocation. AFBC is enabled on a per-surface basis */

static void
panfrost_set_fragment_afbc(struct panfrost_context *ctx)
{
        for (int cb = 0; cb < ctx->pipe_framebuffer.nr_cbufs; ++cb) {
                struct panfrost_resource *rsrc = (struct panfrost_resource *) ctx->pipe_framebuffer.cbufs[cb]->texture;

                /* Non-AFBC is the default */
                if (!rsrc->has_afbc)
                        continue;

                /* Enable AFBC for the render target */
                ctx->fragment_rts[0].afbc.metadata = rsrc->afbc_slab.gpu;
                ctx->fragment_rts[0].afbc.stride = 0;
                ctx->fragment_rts[0].afbc.unk = 0x30009;

                ctx->fragment_rts[0].format |= MALI_MFBD_FORMAT_AFBC;

                /* Change colourspace from RGB to BGR? */
#if 0
                ctx->fragment_rts[0].format |= 0x800000;
                ctx->fragment_rts[0].format &= ~0x20000;
#endif

                /* Point rendering to our special framebuffer */
                ctx->fragment_rts[0].framebuffer = rsrc->afbc_slab.gpu + rsrc->afbc_metadata_size;

                /* WAT? Stride is diff from the scanout case */
                ctx->fragment_rts[0].framebuffer_stride = ctx->pipe_framebuffer.width * 2 * 4;
        }

        /* Enable depth/stencil AFBC for the framebuffer (not the render target) */
        if (ctx->pipe_framebuffer.zsbuf) {
                struct panfrost_resource *rsrc = (struct panfrost_resource *) ctx->pipe_framebuffer.zsbuf->texture;

                if (rsrc->has_afbc) {
                        ctx->fragment_fbd.unk3 |= MALI_MFBD_EXTRA;

                        ctx->fragment_extra.ds_afbc.depth_stencil_afbc_metadata = rsrc->afbc_slab.gpu;
                        ctx->fragment_extra.ds_afbc.depth_stencil_afbc_stride = 0;

                        ctx->fragment_extra.ds_afbc.depth_stencil = rsrc->afbc_slab.gpu + rsrc->afbc_metadata_size;

                        ctx->fragment_extra.ds_afbc.zero1 = 0x10009;
                        ctx->fragment_extra.ds_afbc.padding = 0x1000;

                        ctx->fragment_extra.unk = 0x435; /* General 0x400 in all unks. 0x5 for depth/stencil. 0x10 for AFBC encoded depth stencil. Unclear where the 0x20 is from */

                        ctx->fragment_fbd.unk3 |= 0x400;
                }
        }

        /* For the special case of a depth-only FBO, we need to attach a dummy render target */

        if (ctx->pipe_framebuffer.nr_cbufs == 0) {
                ctx->fragment_rts[0].format = 0x80008000;
                ctx->fragment_rts[0].framebuffer = 0;
                ctx->fragment_rts[0].framebuffer_stride = 0;
        }
}

/* Framebuffer descriptor */

#ifdef SFBD
static void
panfrost_set_framebuffer_resolution(struct mali_single_framebuffer *fb, int w, int h)
{
        fb->width = MALI_POSITIVE(w);
        fb->height = MALI_POSITIVE(h);

        /* No idea why this is needed, but it's how resolution_check is
         * calculated.  It's not clear to us yet why the hardware wants this.
         * The formula itself was discovered mostly by manual bruteforce and
         * aggressive algebraic simplification. */

        fb->resolution_check = ((w + h) / 3) << 4;
}
#endif

static PANFROST_FRAMEBUFFER
panfrost_emit_fbd(struct panfrost_context *ctx)
{
#ifdef SFBD
        struct mali_single_framebuffer framebuffer = {
                .unknown2 = 0x1f,
                .format = 0x30000000,
                .clear_flags = 0x1000,
                .unknown_address_0 = ctx->scratchpad.gpu,
                .unknown_address_1 = ctx->scratchpad.gpu + 0x6000,
                .unknown_address_2 = ctx->scratchpad.gpu + 0x6200,
                .tiler_flags = 0xf0,
                .tiler_heap_free = ctx->tiler_heap.gpu,
                .tiler_heap_end = ctx->tiler_heap.gpu + ctx->tiler_heap.size,
        };

        panfrost_set_framebuffer_resolution(&framebuffer, ctx->pipe_framebuffer.width, ctx->pipe_framebuffer.height);
#else
        struct bifrost_framebuffer framebuffer = {
                .tiler_meta = 0xf00000c600,

                .width1 = MALI_POSITIVE(ctx->pipe_framebuffer.width),
                .height1 = MALI_POSITIVE(ctx->pipe_framebuffer.height),
                .width2 = MALI_POSITIVE(ctx->pipe_framebuffer.width),
                .height2 = MALI_POSITIVE(ctx->pipe_framebuffer.height),

                .unk1 = 0x1080,

                /* TODO: MRT */
                .rt_count_1 = MALI_POSITIVE(1),
                .rt_count_2 = 4,

                .unknown2 = 0x1f,

                /* Presumably corresponds to unknown_address_X of SFBD */
                .scratchpad = ctx->scratchpad.gpu,
                .tiler_scratch_start  = ctx->misc_0.gpu,
                .tiler_scratch_middle = ctx->misc_0.gpu + /*ctx->misc_0.size*/40960, /* Size depends on the size of the framebuffer and the number of vertices */

                .tiler_heap_start = ctx->tiler_heap.gpu,
                .tiler_heap_end = ctx->tiler_heap.gpu + ctx->tiler_heap.size,
        };

#endif

        return framebuffer;
}

/* Are we currently rendering to the screen (rather than an FBO)? */

static bool
panfrost_is_scanout(struct panfrost_context *ctx)
{
        /* If there is no color buffer, it's an FBO */
        if (!ctx->pipe_framebuffer.nr_cbufs)
                return false;

        /* If we're too early that no framebuffer was sent, it's scanout */
        if (!ctx->pipe_framebuffer.cbufs[0])
                return true;

        return ctx->pipe_framebuffer.cbufs[0]->texture->bind & PIPE_BIND_DISPLAY_TARGET;
}

/* The above function is for generalised fbd emission, used in both fragment as
 * well as vertex/tiler payloads. This payload is specific to fragment
 * payloads. */

static void
panfrost_new_frag_framebuffer(struct panfrost_context *ctx)
{
        mali_ptr framebuffer = ctx->framebuffer.gpu;
        int stride;

        /* The default is upside down from OpenGL's perspective. Plus, for scanout we supply our own framebuffer / stride */
        if (panfrost_is_scanout(ctx)) {
                stride = ctx->scanout_stride;

                framebuffer += stride * (ctx->pipe_framebuffer.height - 1);
                stride = -stride;
        } else if (ctx->pipe_framebuffer.nr_cbufs > 0) {
                stride = util_format_get_stride(ctx->pipe_framebuffer.cbufs[0]->format, ctx->pipe_framebuffer.width);
        } else {
                /* Depth-only framebuffer -> dummy RT */
                framebuffer = 0;
                stride = 0;
        }

#ifdef SFBD
        struct mali_single_framebuffer fb = panfrost_emit_fbd(ctx);

        fb.framebuffer = framebuffer;
        fb.stride = stride;

        fb.format = 0xb84e0281; /* RGB32, no MSAA */
#else
        struct bifrost_framebuffer fb = panfrost_emit_fbd(ctx);

        /* XXX: MRT case */
        fb.rt_count_2 = 1;
        fb.unk3 = 0x100;

        struct bifrost_render_target rt = {
                .unk1 = 0x4000000,
                .format = 0x860a8899, /* RGBA32, no MSAA */
                .framebuffer = framebuffer,
                .framebuffer_stride = (stride / 16) & 0xfffffff,
        };

        memcpy(&ctx->fragment_rts[0], &rt, sizeof(rt));

        memset(&ctx->fragment_extra, 0, sizeof(ctx->fragment_extra));
#endif

        memcpy(&ctx->fragment_fbd, &fb, sizeof(fb));
}

/* Maps float 0.0-1.0 to int 0x00-0xFF */
static uint8_t
normalised_float_to_u8(float f)
{
        return (uint8_t) (int) (f * 255.0f);
}

static void
panfrost_clear(
        struct pipe_context *pipe,
        unsigned buffers,
        const union pipe_color_union *color,
        double depth, unsigned stencil)
{
        struct panfrost_context *ctx = panfrost_context(pipe);

        /* Save settings for FBO switch */
        ctx->last_clear.buffers = buffers;
        ctx->last_clear.color = color;
        ctx->last_clear.depth = depth;
        ctx->last_clear.depth = depth;

        bool clear_color = buffers & PIPE_CLEAR_COLOR;
        bool clear_depth = buffers & PIPE_CLEAR_DEPTH;
        bool clear_stencil = buffers & PIPE_CLEAR_STENCIL;

        /* Remember that we've done something */
        ctx->dirty |= PAN_DIRTY_DUMMY;

        /* Alpha clear only meaningful without alpha channel */
        bool has_alpha = ctx->pipe_framebuffer.nr_cbufs && util_format_has_alpha(ctx->pipe_framebuffer.cbufs[0]->format);
        float clear_alpha = has_alpha ? color->f[3] : 1.0f;

        uint32_t packed_color =
                (normalised_float_to_u8(clear_alpha) << 24) |
                (normalised_float_to_u8(color->f[2]) << 16) |
                (normalised_float_to_u8(color->f[1]) <<  8) |
                (normalised_float_to_u8(color->f[0]) <<  0);

#ifdef MFBD
        struct bifrost_render_target *buffer_color = &ctx->fragment_rts[0];
#else
        struct mali_single_framebuffer *buffer_color = &ctx->fragment_fbd;
#endif

#ifdef MFBD
        struct bifrost_framebuffer *buffer_ds = &ctx->fragment_fbd;
#else
        struct mali_single_framebuffer *buffer_ds = buffer_color;
#endif

        if (clear_color) {
                /* Fields duplicated 4x for unknown reasons. Same in Utgard,
                 * too, which is doubly weird. */

                buffer_color->clear_color_1 = packed_color;
                buffer_color->clear_color_2 = packed_color;
                buffer_color->clear_color_3 = packed_color;
                buffer_color->clear_color_4 = packed_color;
        }

        if (clear_depth) {
#ifdef SFBD
                buffer_ds->clear_depth_1 = depth;
                buffer_ds->clear_depth_2 = depth;
                buffer_ds->clear_depth_3 = depth;
                buffer_ds->clear_depth_4 = depth;
#else
                buffer_ds->clear_depth = depth;
#endif
        }

        if (clear_stencil) {
                buffer_ds->clear_stencil = stencil;
        }

        /* Setup buffers depending on MFBD/SFBD */

#ifdef MFBD

        if (clear_depth || clear_stencil) {
                /* Setup combined 24/8 depth/stencil */
                ctx->fragment_fbd.unk3 |= MALI_MFBD_EXTRA;
                //ctx->fragment_extra.unk = /*0x405*/0x404;
                ctx->fragment_extra.unk = 0x405;
                ctx->fragment_extra.ds_linear.depth = ctx->depth_stencil_buffer.gpu;
                ctx->fragment_extra.ds_linear.depth_stride = ctx->pipe_framebuffer.width * 4;
        }

#else

        if (clear_depth) {
                buffer_ds->depth_buffer = ctx->depth_stencil_buffer.gpu;
                buffer_ds->depth_buffer_enable = MALI_DEPTH_STENCIL_ENABLE;
        }

        if (clear_stencil) {
                buffer_ds->stencil_buffer = ctx->depth_stencil_buffer.gpu;
                buffer_ds->stencil_buffer_enable = MALI_DEPTH_STENCIL_ENABLE;
        }

#endif

#ifdef SFBD
        /* Set flags based on what has been cleared, for the SFBD case */
        /* XXX: What do these flags mean? */
        int clear_flags = 0x101100;

        if (clear_color && clear_depth && clear_stencil) {
                /* On a tiler like this, it's fastest to clear all three buffers at once */

                clear_flags |= MALI_CLEAR_FAST;
        } else {
                clear_flags |= MALI_CLEAR_SLOW;

                if (clear_stencil)
                        clear_flags |= MALI_CLEAR_SLOW_STENCIL;
        }

        fbd->clear_flags = clear_flags;
#endif
}

static void
panfrost_attach_vt_framebuffer(struct panfrost_context *ctx)
{
#ifdef MFBD
        mali_ptr who_knows = panfrost_reserve(&ctx->cmdstream, 1024);
#endif

        mali_ptr framebuffer_1_p = panfrost_upload(&ctx->cmdstream, &ctx->vt_framebuffer, sizeof(ctx->vt_framebuffer), true) | PANFROST_DEFAULT_FBD;

#ifdef MFBD
        /* MFBD needs a sequential semi-render target upload */

        /* What this is, is beyond me for now */
        struct bifrost_render_target rts_list[] = {
                {
                        .chunknown = {
                                .unk = 0x30005,
                                .pointer = who_knows,
                        },
                        .framebuffer = ctx->misc_0.gpu,
                        .zero2 = 0x3,
                },
        };

        panfrost_upload_sequential(&ctx->cmdstream, rts_list, sizeof(rts_list));
#endif
        ctx->payload_vertex.postfix.framebuffer = framebuffer_1_p;
        ctx->payload_tiler.postfix.framebuffer = framebuffer_1_p;
}

static void
panfrost_viewport(struct panfrost_context *ctx,
                  float depth_range_n,
                  float depth_range_f,
                  int viewport_x0, int viewport_y0,
                  int viewport_x1, int viewport_y1)
{
        /* Viewport encoding is asymmetric. Purpose of the floats is unknown? */

        struct mali_viewport ret = {
                .floats = {
                        -inff, -inff,
                                inff, inff,
                        },

                .depth_range_n = depth_range_n,
                .depth_range_f = depth_range_f,

                .viewport0 = { viewport_x0, viewport_y0 },
                .viewport1 = { MALI_POSITIVE(viewport_x1), MALI_POSITIVE(viewport_y1) },
        };

        memcpy(&ctx->viewport, &ret, sizeof(ret));
        ctx->dirty |= PAN_DIRTY_VIEWPORT;
}

/* Reset per-frame context, called on context initialisation as well as after
 * flushing a frame */

static void
panfrost_invalidate_frame(struct panfrost_context *ctx)
{
        /* Rotate cmdstream */
        ctx->cmdstream = ctx->cmdstream_rings[ctx->cmdstream_i];

        if ((++ctx->cmdstream_i) == (sizeof(ctx->cmdstream_rings) / sizeof(ctx->cmdstream_rings[0])))
                ctx->cmdstream_i = 0;

        ctx->vt_framebuffer = panfrost_emit_fbd(ctx);
        panfrost_new_frag_framebuffer(ctx);

        /* Reset varyings allocated */
        ctx->varying_height = 0;

        /* The cmdstream is dirty every frame; the only bits worth preserving
         * (textures, shaders, etc) are in other buffers anyways */
        ctx->cmdstream.stack_bottom = 0;

        /* Regenerate payloads */
        panfrost_attach_vt_framebuffer(ctx);

        if (ctx->rasterizer)
                ctx->dirty |= PAN_DIRTY_RASTERIZER;

        /* Uniforms are all discarded with the above stack discard */

        for (int i = 0; i <= PIPE_SHADER_FRAGMENT; ++i)
                ctx->constant_buffer[i].dirty = true;

        /* XXX */
        ctx->dirty |= PAN_DIRTY_SAMPLERS | PAN_DIRTY_TEXTURES;
}

/* In practice, every field of these payloads should be configurable
 * arbitrarily, which means these functions are basically catch-all's for
 * as-of-yet unwavering unknowns */

static void
panfrost_emit_vertex_payload(struct panfrost_context *ctx)
{
        struct midgard_payload_vertex_tiler payload = {
                .prefix = {
                        .workgroups_z_shift = 32,
                        .workgroups_x_shift_2 = 0x2,
                        .workgroups_x_shift_3 = 0x5,
                },
                .gl_enables = 0x6
        };

        memcpy(&ctx->payload_vertex, &payload, sizeof(payload));
}

static void
panfrost_emit_tiler_payload(struct panfrost_context *ctx)
{
        struct midgard_payload_vertex_tiler payload_1 = {
                .prefix = {
                        .workgroups_z_shift = 32,
                        .workgroups_x_shift_2 = 0x2,
                        .workgroups_x_shift_3 = 0x6,

                        .zero1 = 0xffff, /* Why is this only seen on test-quad-textured? */
                },
        };

        memcpy(&ctx->payload_tiler, &payload_1, sizeof(payload_1));
}

static unsigned
panfrost_translate_texture_swizzle(enum pipe_swizzle s)
{
        switch (s) {
        case PIPE_SWIZZLE_X:
                return MALI_CHANNEL_RED;

        case PIPE_SWIZZLE_Y:
                return MALI_CHANNEL_GREEN;

        case PIPE_SWIZZLE_Z:
                return MALI_CHANNEL_BLUE;

        case PIPE_SWIZZLE_W:
                return MALI_CHANNEL_ALPHA;

        case PIPE_SWIZZLE_0:
                return MALI_CHANNEL_ZERO;

        case PIPE_SWIZZLE_1:
                return MALI_CHANNEL_ONE;

        default:
                assert(0);
                return 0;
        }
}

static unsigned
translate_tex_wrap(enum pipe_tex_wrap w)
{
        switch (w) {
        case PIPE_TEX_WRAP_REPEAT:
                return MALI_WRAP_REPEAT;

        case PIPE_TEX_WRAP_CLAMP_TO_EDGE:
                return MALI_WRAP_CLAMP_TO_EDGE;

        case PIPE_TEX_WRAP_CLAMP_TO_BORDER:
                return MALI_WRAP_CLAMP_TO_BORDER;

        case PIPE_TEX_WRAP_MIRROR_REPEAT:
                return MALI_WRAP_MIRRORED_REPEAT;

        default:
                assert(0);
                return 0;
        }
}

static unsigned
translate_tex_filter(enum pipe_tex_filter f)
{
        switch (f) {
        case PIPE_TEX_FILTER_NEAREST:
                return MALI_GL_NEAREST;

        case PIPE_TEX_FILTER_LINEAR:
                return MALI_GL_LINEAR;

        default:
                assert(0);
                return 0;
        }
}

static unsigned
translate_mip_filter(enum pipe_tex_mipfilter f)
{
        return (f == PIPE_TEX_MIPFILTER_LINEAR) ? MALI_GL_MIP_LINEAR : 0;
}

static unsigned
panfrost_translate_compare_func(enum pipe_compare_func in)
{
        switch (in) {
        case PIPE_FUNC_NEVER:
                return MALI_FUNC_NEVER;

        case PIPE_FUNC_LESS:
                return MALI_FUNC_LESS;

        case PIPE_FUNC_EQUAL:
                return MALI_FUNC_EQUAL;

        case PIPE_FUNC_LEQUAL:
                return MALI_FUNC_LEQUAL;

        case PIPE_FUNC_GREATER:
                return MALI_FUNC_GREATER;

        case PIPE_FUNC_NOTEQUAL:
                return MALI_FUNC_NOTEQUAL;

        case PIPE_FUNC_GEQUAL:
                return MALI_FUNC_GEQUAL;

        case PIPE_FUNC_ALWAYS:
                return MALI_FUNC_ALWAYS;
        }

        assert (0);
        return 0; /* Unreachable */
}

static unsigned
panfrost_translate_alt_compare_func(enum pipe_compare_func in)
{
        switch (in) {
        case PIPE_FUNC_NEVER:
                return MALI_ALT_FUNC_NEVER;

        case PIPE_FUNC_LESS:
                return MALI_ALT_FUNC_LESS;

        case PIPE_FUNC_EQUAL:
                return MALI_ALT_FUNC_EQUAL;

        case PIPE_FUNC_LEQUAL:
                return MALI_ALT_FUNC_LEQUAL;

        case PIPE_FUNC_GREATER:
                return MALI_ALT_FUNC_GREATER;

        case PIPE_FUNC_NOTEQUAL:
                return MALI_ALT_FUNC_NOTEQUAL;

        case PIPE_FUNC_GEQUAL:
                return MALI_ALT_FUNC_GEQUAL;

        case PIPE_FUNC_ALWAYS:
                return MALI_ALT_FUNC_ALWAYS;
        }

        assert (0);
        return 0; /* Unreachable */
}

static unsigned
panfrost_translate_stencil_op(enum pipe_stencil_op in)
{
        switch (in) {
        case PIPE_STENCIL_OP_KEEP:
                return MALI_STENCIL_KEEP;

        case PIPE_STENCIL_OP_ZERO:
                return MALI_STENCIL_ZERO;

        case PIPE_STENCIL_OP_REPLACE:
                return MALI_STENCIL_REPLACE;

        case PIPE_STENCIL_OP_INCR:
                return MALI_STENCIL_INCR;

        case PIPE_STENCIL_OP_DECR:
                return MALI_STENCIL_DECR;

        case PIPE_STENCIL_OP_INCR_WRAP:
                return MALI_STENCIL_INCR_WRAP;

        case PIPE_STENCIL_OP_DECR_WRAP:
                return MALI_STENCIL_DECR_WRAP;

        case PIPE_STENCIL_OP_INVERT:
                return MALI_STENCIL_INVERT;
        }

        assert (0);
        return 0; /* Unreachable */
}

static void
panfrost_make_stencil_state(const struct pipe_stencil_state *in, struct mali_stencil_test *out)
{
        out->ref = 0; /* Gallium gets it from elsewhere */

        out->mask = in->valuemask;
        out->func = panfrost_translate_compare_func(in->func);
        out->sfail = panfrost_translate_stencil_op(in->fail_op);
        out->dpfail = panfrost_translate_stencil_op(in->zfail_op);
        out->dppass = panfrost_translate_stencil_op(in->zpass_op);
}

static void
panfrost_default_shader_backend(struct panfrost_context *ctx)
{
        struct mali_shader_meta shader = {
                .alpha_coverage = ~MALI_ALPHA_COVERAGE(0.000000),

                .unknown2_3 = MALI_DEPTH_FUNC(MALI_FUNC_ALWAYS) | 0x3010 /*| MALI_CAN_DISCARD*/,
#ifdef T8XX
                .unknown2_4 = MALI_NO_MSAA | 0x4e0,
#else
                .unknown2_4 = MALI_NO_MSAA | 0x4f0,
#endif
        };

        struct pipe_stencil_state default_stencil = {
                .enabled = 0,
                .func = PIPE_FUNC_ALWAYS,
                .fail_op = MALI_STENCIL_KEEP,
                .zfail_op = MALI_STENCIL_KEEP,
                .zpass_op = MALI_STENCIL_KEEP,
                .writemask = 0xFF,
                .valuemask = 0xFF
        };

        panfrost_make_stencil_state(&default_stencil, &shader.stencil_front);
        shader.stencil_mask_front = default_stencil.writemask;

        panfrost_make_stencil_state(&default_stencil, &shader.stencil_back);
        shader.stencil_mask_back = default_stencil.writemask;

        if (default_stencil.enabled)
                shader.unknown2_4 |= MALI_STENCIL_TEST;

        memcpy(&ctx->fragment_shader_core, &shader, sizeof(shader));
}

/* Generates a vertex/tiler job. This is, in some sense, the heart of the
 * graphics command stream. It should be called once per draw, accordding to
 * presentations. Set is_tiler for "tiler" jobs (fragment shader jobs, but in
 * Mali parlance, "fragment" refers to framebuffer writeout). Clear it for
 * vertex jobs. */

static mali_ptr
panfrost_vertex_tiler_job(struct panfrost_context *ctx, bool is_tiler)
{
        /* Each draw call corresponds to two jobs, and we want to offset to leave room for the set-value job */
        int draw_job_index = 1 + (2 * ctx->draw_count);

        struct mali_job_descriptor_header job = {
                .job_type = is_tiler ? JOB_TYPE_TILER : JOB_TYPE_VERTEX,
                .job_index = draw_job_index + (is_tiler ? 1 : 0),
#ifdef BIT64
                .job_descriptor_size = 1,
#endif
        };

        /* XXX: What is this? */
#ifdef T6XX

        if (is_tiler)
                job.unknown_flags = ctx->draw_count ? 64 : 1;

#endif

        /* Only tiler jobs have dependencies which are known at this point */

        if (is_tiler) {
                /* Tiler jobs depend on vertex jobs */

                job.job_dependency_index_1 = draw_job_index;

                /* Tiler jobs also depend on the previous tiler job */

                if (ctx->draw_count)
                        job.job_dependency_index_2 = draw_job_index - 1;
        }

        struct midgard_payload_vertex_tiler *payload = is_tiler ? &ctx->payload_tiler : &ctx->payload_vertex;

        /* There's some padding hacks on 32-bit */

#ifdef BIT64
        int offset = 0;

#else
        int offset = 4;

#endif

        mali_ptr job_p = panfrost_upload(&ctx->cmdstream, &job, sizeof(job) - offset, true);

        panfrost_upload_sequential(&ctx->cmdstream, payload, sizeof(*payload));

        return job_p;
}

/* Generates a set value job. It's unclear what exactly this does, why it's
 * necessary, and when to call it. */

static mali_ptr
panfrost_set_value_job(struct panfrost_context *ctx)
{
        struct mali_job_descriptor_header job_0 = {
                .job_type = JOB_TYPE_SET_VALUE,
                .job_descriptor_size = 1,
                .job_index = 1 + (2 * ctx->draw_count),
        };

        struct mali_payload_set_value payload_0 = {
                .out = ctx->misc_0.gpu,
                .unknown = 0x3,
        };

        mali_ptr job_0_p = panfrost_upload(&ctx->cmdstream, &job_0, sizeof(job_0), true);
        panfrost_upload_sequential(&ctx->cmdstream, &payload_0, sizeof(payload_0));

        return job_0_p;
}

/* Generate a fragment job. This should be called once per frame. (According to
 * presentations, this is supposed to correspond to eglSwapBuffers) */

static mali_ptr
panfrost_fragment_job(struct panfrost_context *ctx)
{
        /* Update fragment FBD */
        panfrost_set_fragment_afbc(ctx);

        if (ctx->pipe_framebuffer.nr_cbufs == 1) {
                struct panfrost_resource *rsrc = (struct panfrost_resource *) ctx->pipe_framebuffer.cbufs[0]->texture;

                if (rsrc->has_checksum) {
                        //ctx->fragment_fbd.unk3 |= 0xa00000;
                        ctx->fragment_fbd.unk3 = 0xa02100;
                        ctx->fragment_fbd.unk3 |= MALI_MFBD_EXTRA;
                        ctx->fragment_extra.unk = 0x420;
                        ctx->fragment_extra.checksum_stride = rsrc->checksum_stride;
                        ctx->fragment_extra.checksum = /*rsrc->checksum_slab.gpu*/ctx->framebuffer.gpu + (ctx->scanout_stride) * rsrc->base.height0;
                }
        }

        /* The frame is complete and therefore the framebuffer descriptor is
         * ready for linkage and upload */

        mali_ptr fbd = panfrost_upload(&ctx->cmdstream, &ctx->fragment_fbd, sizeof(ctx->fragment_fbd), true);

        /* Upload extra framebuffer info if necessary */
        if (ctx->fragment_fbd.unk3 & MALI_MFBD_EXTRA) {
                panfrost_upload_sequential(&ctx->cmdstream, &ctx->fragment_extra, sizeof(struct bifrost_fb_extra));
        }

        /* Upload (single) render target */
        panfrost_upload_sequential(&ctx->cmdstream, &ctx->fragment_rts[0], sizeof(struct bifrost_render_target) * 1);

        /* Generate the fragment (frame) job */

        struct mali_job_descriptor_header header = {
                .job_type = JOB_TYPE_FRAGMENT,
                .job_index = 1,
#ifdef BIT64
                .job_descriptor_size = 1
#endif
        };

        struct mali_payload_fragment payload = {
                .min_tile_coord = MALI_COORDINATE_TO_TILE_MIN(0, 0),
                .max_tile_coord = MALI_COORDINATE_TO_TILE_MAX(ctx->pipe_framebuffer.width, ctx->pipe_framebuffer.height),
                .framebuffer = fbd | PANFROST_DEFAULT_FBD | (ctx->fragment_fbd.unk3 & MALI_MFBD_EXTRA ? 2 : 0),
        };

        /* Normally, there should be no padding. However, fragment jobs are
         * shared with 64-bit Bifrost systems, and accordingly there is 4-bytes
         * of zero padding in between. */

        mali_ptr job_pointer = panfrost_upload(&ctx->cmdstream, &header, sizeof(header), true);
        panfrost_upload_sequential(&ctx->cmdstream, &payload, sizeof(payload));

        return job_pointer;
}

/* Emits attributes and varying descriptors, which should be called every draw,
 * excepting some obscure circumstances */

static void
panfrost_emit_vertex_data(struct panfrost_context *ctx)
{
        /* TODO: Only update the dirtied buffers */
        struct mali_attr attrs[PIPE_MAX_ATTRIBS];
        struct mali_attr varyings[PIPE_MAX_ATTRIBS];

        for (int i = 0; i < ctx->vertex_buffer_count; ++i) {
                struct pipe_vertex_buffer *buf = &ctx->vertex_buffers[i];
                struct panfrost_resource *rsrc = (struct panfrost_resource *) (buf->buffer.resource);

                /* Offset vertex count by draw_start to make sure we upload enough */
                attrs[i].stride = buf->stride;
                //attrs[i].size = buf->stride * (ctx->payload_vertex.draw_start + ctx->vertex_count);

                /* TODO: The above calculation is wrong. Do it better. For now, force resources */
                assert(!buf->is_user_buffer);
                attrs[i].size = buf->buffer.resource->width0 - buf->buffer_offset;

                attrs[i].elements = panfrost_upload(&ctx->cmdstream, rsrc->cpu[0] + buf->buffer_offset, attrs[i].size, false) | 1;
        }

        for (int i = 0; i < ctx->vs->varyings.varying_buffer_count; ++i) {
                varyings[i].elements = (ctx->varying_mem.gpu + ctx->varying_height) | 1;
                varyings[i].stride = ctx->vs->varyings.varyings_stride[i];

                /* XXX: Why does adding an extra ~8000 vertices fix missing triangles in glmark2-es2 -bshadow? */
                varyings[i].size = ctx->vs->varyings.varyings_stride[i] * MALI_NEGATIVE(ctx->payload_tiler.prefix.invocation_count);

                /* gl_Position varying is always last by convention */
                if ((i + 1) == ctx->vs->varyings.varying_buffer_count)
                        ctx->payload_tiler.postfix.position_varying = ctx->varying_mem.gpu + ctx->varying_height;

                /* Varyings appear to need 64-byte alignment */
                ctx->varying_height += ALIGN(varyings[i].size, 64);

                /* Ensure that we fit */
                assert(ctx->varying_height < ctx->varying_mem.size);
        }

        ctx->payload_vertex.postfix.attributes = panfrost_upload(&ctx->cmdstream, attrs, ctx->vertex_buffer_count * sizeof(struct mali_attr), false);

        mali_ptr varyings_p = panfrost_upload(&ctx->cmdstream, &varyings, ctx->vs->varyings.varying_buffer_count * sizeof(struct mali_attr), false);
        ctx->payload_vertex.postfix.varyings = varyings_p;
        ctx->payload_tiler.postfix.varyings = varyings_p;
}

/* Go through dirty flags and actualise them in the cmdstream. */

static void
panfrost_emit_for_draw(struct panfrost_context *ctx)
{
        panfrost_emit_vertex_data(ctx);

        if (ctx->dirty & PAN_DIRTY_RASTERIZER) {
                ctx->payload_tiler.line_width = ctx->rasterizer->base.line_width;
                ctx->payload_tiler.gl_enables = ctx->rasterizer->tiler_gl_enables;

                panfrost_set_framebuffer_msaa(ctx, FORCE_MSAA || ctx->rasterizer->base.multisample);

        }

        if (ctx->dirty & PAN_DIRTY_VS) {
                assert(ctx->vs);

                /* Late shader descriptor assignments */
                ctx->vs->tripipe.texture_count = ctx->sampler_view_count[PIPE_SHADER_VERTEX];
                ctx->vs->tripipe.sampler_count = ctx->sampler_count[PIPE_SHADER_VERTEX];

                /* Who knows */
                ctx->vs->tripipe.midgard1.unknown1 = 0x2201;

                ctx->payload_vertex.postfix._shader_upper = panfrost_upload(&ctx->cmdstream_persistent, &ctx->vs->tripipe, sizeof(struct mali_shader_meta), true) >> 4;

                /* Varying descriptor is tied to the vertex shader. Also the
                 * fragment shader, I suppose, but it's generated with the
                 * vertex shader so */

                panfrost_upload_varyings_descriptor(ctx);
        }

        if (ctx->dirty & PAN_DIRTY_FS) {
                assert(ctx->fs);
#define COPY(name) ctx->fragment_shader_core.name = ctx->fs->tripipe.name

                COPY(shader);
                COPY(attribute_count);
                COPY(varying_count);
                COPY(midgard1.uniform_count);
                COPY(midgard1.work_count);
                COPY(midgard1.unknown2);

#undef COPY
                /* If there is a blend shader, work registers are shared */

                if (ctx->blend->has_blend_shader)
                        ctx->fragment_shader_core.midgard1.work_count = /*MAX2(ctx->fragment_shader_core.midgard1.work_count, ctx->blend->blend_work_count)*/16;

                /* Set late due to depending on render state */
                /* The one at the end seems to mean "1 UBO" */
                ctx->fragment_shader_core.midgard1.unknown1 = MALI_NO_ALPHA_TO_COVERAGE | 0x200 | 0x2201;

                /* Assign texture/sample count right before upload */
                ctx->fragment_shader_core.texture_count = ctx->sampler_view_count[PIPE_SHADER_FRAGMENT];
                ctx->fragment_shader_core.sampler_count = ctx->sampler_count[PIPE_SHADER_FRAGMENT];

                /* Assign the stencil refs late */
                ctx->fragment_shader_core.stencil_front.ref = ctx->stencil_ref.ref_value[0];
                ctx->fragment_shader_core.stencil_back.ref = ctx->stencil_ref.ref_value[1];

                /* CAN_DISCARD should be set if the fragment shader possibly
                 * contains a 'discard' instruction, or maybe other
                 * circumstances. It is likely this is related to optimizations
                 * related to forward-pixel kill, as per "Mali Performance 3:
                 * Is EGL_BUFFER_PRESERVED a good thing?" by Peter Harris
                 */

                if (ctx->fs->can_discard) {
                        ctx->fragment_shader_core.unknown2_3 |= MALI_CAN_DISCARD;
                        ctx->fragment_shader_core.midgard1.unknown1 &= ~MALI_NO_ALPHA_TO_COVERAGE;
                        ctx->fragment_shader_core.midgard1.unknown1 |= 0x4000;
                        ctx->fragment_shader_core.midgard1.unknown1 = 0x4200;
                }

                if (ctx->blend->has_blend_shader)
                        ctx->fragment_shader_core.blend_shader = ctx->blend->blend_shader;

                ctx->payload_tiler.postfix._shader_upper = panfrost_upload(&ctx->cmdstream_persistent, &ctx->fragment_shader_core, sizeof(struct mali_shader_meta), true) >> 4;

#ifdef T8XX
                /* Additional blend descriptor tacked on for newer systems */

                unsigned blend_count = 0;

                if (ctx->blend->has_blend_shader) {
                        /* For a blend shader, the bottom nibble corresponds to
                         * the number of work registers used, which signals the
                         * -existence- of a blend shader */

                        assert(ctx->blend->blend_work_count >= 2);
                        blend_count |= MIN2(ctx->blend->blend_work_count, 3);
                } else {
                        /* Otherwise, the bottom bit simply specifies if
                         * blending (anything other than REPLACE) is enabled */

                        /* XXX: Less ugly way to do this? */
                        bool no_blending =
                                (ctx->blend->equation.rgb_mode == 0x122) &&
                                (ctx->blend->equation.alpha_mode == 0x122) &&
                                (ctx->blend->equation.color_mask == 0xf);

                        if (!no_blending)
                                blend_count |= 0x1;
                }

                /* Second blend equation is always a simple replace */

                uint64_t replace_magic = 0xf0122122;
                struct mali_blend_equation replace_mode;
                memcpy(&replace_mode, &replace_magic, sizeof(replace_mode));

                struct mali_blend_meta blend_meta[] = {
                        {
                                .unk1 = 0x200 | blend_count,
                                .blend_equation_1 = ctx->blend->equation,
                                .blend_equation_2 = replace_mode
                        },
                };

                if (ctx->blend->has_blend_shader)
                        memcpy(&blend_meta[0].blend_equation_1, &ctx->blend->blend_shader, sizeof(ctx->blend->blend_shader));

                panfrost_upload_sequential(&ctx->cmdstream_persistent, blend_meta, sizeof(blend_meta));
#endif
        }

        if (ctx->dirty & PAN_DIRTY_VERTEX) {
                ctx->payload_vertex.postfix.attribute_meta = panfrost_upload(&
                                ctx->cmdstream_persistent, &ctx->vertex->hw,
                                sizeof(struct mali_attr_meta) * ctx->vertex->num_elements, false);
        }

        ctx->dirty |= PAN_DIRTY_VIEWPORT; /* TODO: Viewport dirty track */

        if (ctx->dirty & PAN_DIRTY_VIEWPORT) {
                ctx->payload_tiler.postfix.viewport = panfrost_upload(&ctx->cmdstream, &ctx->viewport, sizeof(struct mali_viewport), false);
        }

        if (ctx->dirty & PAN_DIRTY_SAMPLERS) {
                /* Upload samplers back to back, no padding */

                for (int t = 0; t <= PIPE_SHADER_FRAGMENT; ++t) {
                        mali_ptr samplers_base = 0;

                        for (int i = 0; i < ctx->sampler_count[t]; ++i) {
                                if (i)
                                        panfrost_upload_sequential(&ctx->cmdstream, &ctx->samplers[t][i]->hw, sizeof(struct mali_sampler_descriptor));
                                else
                                        samplers_base = panfrost_upload(&ctx->cmdstream, &ctx->samplers[t][i]->hw, sizeof(struct mali_sampler_descriptor), true);
                        }

                        if (t == PIPE_SHADER_FRAGMENT)
                                ctx->payload_tiler.postfix.sampler_descriptor = samplers_base;
                        else if (t == PIPE_SHADER_VERTEX)
                                ctx->payload_vertex.postfix.sampler_descriptor = samplers_base;
                        else
                                assert(0);
                }
        }

        if (ctx->dirty & PAN_DIRTY_TEXTURES) {
                for (int t = 0; t <= PIPE_SHADER_FRAGMENT; ++t) {
                        /* Shortcircuit */
                        if (!ctx->sampler_view_count[t]) continue;

                        uint64_t trampolines[PIPE_MAX_SHADER_SAMPLER_VIEWS];

                        for (int i = 0; i < ctx->sampler_view_count[t]; ++i) {
                                /* XXX: Why does this work? */
                                if (!ctx->sampler_views[t][i])
                                        continue;

                                struct pipe_resource *tex_rsrc = ctx->sampler_views[t][i]->base.texture;
                                struct panfrost_resource *rsrc = (struct panfrost_resource *) tex_rsrc;

                                /* Inject the address in. */
                                for (int l = 0; l < (tex_rsrc->last_level + 1); ++l)
                                        ctx->sampler_views[t][i]->hw.swizzled_bitmaps[l] = rsrc->gpu[l];

                                /* Workaround maybe-errata (?) with non-mipmaps */
                                int s = ctx->sampler_views[t][i]->hw.nr_mipmap_levels;

                                if (!rsrc->is_mipmap) {
#ifdef T6XX
                                        /* HW ERRATA, not needed after T6XX */
                                        ctx->sampler_views[t][i]->hw.swizzled_bitmaps[1] = rsrc->gpu[0];

                                        ctx->sampler_views[t][i]->hw.unknown3A = 1;
#endif
                                        ctx->sampler_views[t][i]->hw.nr_mipmap_levels = 0;
                                }

                                trampolines[i] = panfrost_upload(&ctx->cmdstream, &ctx->sampler_views[t][i]->hw, sizeof(struct mali_texture_descriptor), false);

                                /* Restore */
                                ctx->sampler_views[t][i]->hw.nr_mipmap_levels = s;
                                ctx->sampler_views[t][i]->hw.unknown3A = 0;
                        }

                        mali_ptr trampoline = panfrost_upload(&ctx->cmdstream, trampolines, sizeof(uint64_t) * ctx->sampler_view_count[t], false);

                        if (t == PIPE_SHADER_FRAGMENT)
                                ctx->payload_tiler.postfix.texture_trampoline = trampoline;
                        else if (t == PIPE_SHADER_VERTEX)
                                ctx->payload_vertex.postfix.texture_trampoline = trampoline;
                        else
                                assert(0);
                }
        }

        /* Generate the viewport vector of the form: <width/2, height/2, centerx, centery> */
        const struct pipe_viewport_state *vp = &ctx->pipe_viewport;

        float viewport_vec4[] = {
                vp->scale[0],
                fabsf(vp->scale[1]),

                vp->translate[0],
                /* -1.0 * vp->translate[1] */ fabs(1.0 * vp->scale[1]) /* XXX */
        };

        for (int i = 0; i < PIPE_SHADER_TYPES; ++i) {
                struct panfrost_constant_buffer *buf = &ctx->constant_buffer[i];

                if (buf->dirty) {
                        mali_ptr address_prefix = 0, address = 0;

                        /* Attach vertex prefix */
                        if (i == PIPE_SHADER_VERTEX)
                                address_prefix = panfrost_upload(&ctx->cmdstream, viewport_vec4, sizeof(viewport_vec4), true);

                        /* Attach uniforms */
                        if (buf->size)
                                address = panfrost_upload_sequential(&ctx->cmdstream, buf->buffer, buf->size);

                        /* Always fill out -something- */
                        if (!address)
                                address = panfrost_reserve(&ctx->cmdstream, 256);

                        /* Use whichever came first */
                        if (address_prefix)
                                address = address_prefix;

                        int uniform_count = 0;

                        struct mali_vertex_tiler_postfix *postfix;

                        switch (i) {
                        case PIPE_SHADER_VERTEX:
                                uniform_count = ctx->vs->uniform_count;
                                postfix = &ctx->payload_vertex.postfix;
                                break;

                        case PIPE_SHADER_FRAGMENT:
                                uniform_count = ctx->fs->uniform_count;
                                postfix = &ctx->payload_tiler.postfix;
                                break;

                        default:
                                printf("Unknown shader stage %d in uniform upload\n", i);
                                assert(0);
                        }

                        /* Also attach the same buffer as a UBO for extended access */


                        struct mali_uniform_buffer_meta uniform_buffers[] = {
                                {
                                        .size = MALI_POSITIVE((2 + uniform_count)),
                                        .ptr = address >> 2,
                                },
                        };

                        mali_ptr ubufs = panfrost_upload(&ctx->cmdstream, uniform_buffers, sizeof(uniform_buffers), false);
                        postfix->uniforms = address;
                        postfix->uniform_buffers = ubufs;

                        buf->dirty = 0;
                }
        }


        ctx->dirty = 0;
}

/* Corresponds to exactly one draw, but does not submit anything */

static void
panfrost_queue_draw(struct panfrost_context *ctx)
{
        /* TODO: Expand the array? */
        if (ctx->draw_count >= MAX_DRAW_CALLS) {
                printf("Job buffer overflow, ignoring draw\n");
                assert(0);
        }

        /* Handle dirty flags now */
        panfrost_emit_for_draw(ctx);

        ctx->vertex_jobs[ctx->draw_count] = panfrost_vertex_tiler_job(ctx, false);
        ctx->tiler_jobs[ctx->draw_count] = panfrost_vertex_tiler_job(ctx, true);
        ctx->draw_count++;
}

/* At the end of the frame, the vertex and tiler jobs are linked together and
 * then the fragment job is plonked at the end. Set value job is first for
 * unknown reasons. */

#define JOB_DESC(ptr) ((struct mali_job_descriptor_header *) (uintptr_t) (ptr - mem.gpu + (uintptr_t) mem.cpu))
static void
panfrost_link_job_pair(struct panfrost_memory mem, mali_ptr first, mali_ptr next)
{
        if (JOB_DESC(first)->job_descriptor_size)
                JOB_DESC(first)->next_job_64 = (u64) (uintptr_t) next;
        else
                JOB_DESC(first)->next_job_32 = (u32) (uintptr_t) next;
}

static void
panfrost_link_jobs(struct panfrost_context *ctx)
{
        if (ctx->draw_count) {
                /* Generate the set_value_job */
                ctx->set_value_job = panfrost_set_value_job(ctx);

                struct panfrost_memory mem = ctx->cmdstream;

                /* Have the first vertex job depend on the set value job */
                JOB_DESC(ctx->vertex_jobs[0])->job_dependency_index_1 = JOB_DESC(ctx->set_value_job)->job_index;

                /* SV -> V */
                panfrost_link_job_pair(mem, ctx->set_value_job, ctx->vertex_jobs[0]);
        }

        /* V -> V/T ; T -> T/null */
        for (int i = 0; i < ctx->draw_count; ++i) {
                bool isLast = (i + 1) == ctx->draw_count;

                panfrost_link_job_pair(ctx->cmdstream, ctx->vertex_jobs[i], isLast ? ctx->tiler_jobs[0]: ctx->vertex_jobs[i + 1]);
                panfrost_link_job_pair(ctx->cmdstream, ctx->tiler_jobs[i], isLast ? 0 : ctx->tiler_jobs[i + 1]);
        }
}

/* Use to allocate atom numbers for jobs. We probably want to overhaul this in kernel space at some point. */
uint8_t atom_counter = 0;

static uint8_t
allocate_atom()
{
        atom_counter++;

        /* Workaround quirk where atoms must be strictly positive */

        if (atom_counter == 0)
                atom_counter++;

        return atom_counter;
}

int last_fragment_id = -1;
int last_fragment_flushed = true;

/* Forces a flush, to make sure everything is consistent.
 * Bad for parallelism. Necessary for glReadPixels etc. Use cautiously.
 */

static void
force_flush_fragment(struct panfrost_context *ctx)
{
        if (!last_fragment_flushed) {
                uint8_t ev[/* 1 */ 4 + 4 + 8 + 8];

                do {
                        read(ctx->fd, ev, sizeof(ev));
                } while (ev[4] != last_fragment_id);

                last_fragment_flushed = true;
        }
}

/* The entire frame is in memory -- send it off to the kernel! */

static void
panfrost_submit_frame(struct panfrost_context *ctx, bool flush_immediate)
{
        /* Edge case if screen is cleared and nothing else */
        bool has_draws = ctx->draw_count > 0;

        /* Workaround a bizarre lockup (a hardware errata?) */
        if (!has_draws)
                flush_immediate = true;

        /* A number of jobs are batched -- this must be linked and cleared */
        panfrost_link_jobs(ctx);

        ctx->draw_count = 0;

#ifndef DRY_RUN
        /* XXX: flush_immediate was causing lock-ups wrt readpixels in dEQP. Investigate. */

        mali_external_resource framebuffer[] = {
                ctx->framebuffer.gpu | MALI_EXT_RES_ACCESS_EXCLUSIVE,
        };

        int vt_atom = allocate_atom();

        struct mali_jd_atom_v2 atoms[] = {
                {
                        .jc = ctx->set_value_job,
                        .atom_number = vt_atom,
                        .core_req = MALI_JD_REQ_CS | MALI_JD_REQ_T | MALI_JD_REQ_CF | MALI_JD_REQ_COHERENT_GROUP | MALI_JD_REQ_EVENT_NEVER | MALI_JD_REQ_SKIP_CACHE_END,
                },
                {
                        .jc = panfrost_fragment_job(ctx),
                        .nr_ext_res = 1,
                        .ext_res_list = framebuffer,
                        .atom_number = allocate_atom(),
                        .core_req = MALI_JD_REQ_FS | MALI_JD_REQ_SKIP_CACHE_START,
                },
        };

        if (last_fragment_id != -1) {
                atoms[0].pre_dep[0].atom_id = last_fragment_id;
                atoms[0].pre_dep[0].dependency_type = MALI_JD_DEP_TYPE_ORDER;
        }

        if (has_draws) {
                atoms[1].pre_dep[0].atom_id = vt_atom;
                atoms[1].pre_dep[0].dependency_type = MALI_JD_DEP_TYPE_DATA;
        }

        atoms[1].core_req |= panfrost_is_scanout(ctx) ? MALI_JD_REQ_EXTERNAL_RESOURCES : MALI_JD_REQ_FS_AFBC;

        /* Copy over core reqs for old kernels */

        for (int i = 0; i < 2; ++i)
                atoms[i].compat_core_req = atoms[i].core_req;

        struct mali_ioctl_job_submit submit = {
                .addr = atoms + (has_draws ? 0 : 1),
                .nr_atoms = has_draws ? 2 : 1,
                .stride = sizeof(struct mali_jd_atom_v2),
        };

        if (pandev_ioctl(ctx->fd, MALI_IOCTL_JOB_SUBMIT, &submit))
                printf("Error submitting\n");

        /* If visual, we can stall a frame */

        if (!flush_immediate)
                force_flush_fragment(ctx);

        last_fragment_id = atoms[1].atom_number;
        last_fragment_flushed = false;

        /* If readback, flush now (hurts the pipelined performance) */
        if (flush_immediate)
                force_flush_fragment(ctx);

#endif
}

bool dont_scanout = false;

static void
panfrost_flush(
        struct pipe_context *pipe,
        struct pipe_fence_handle **fence,
        unsigned flags)
{
        struct panfrost_context *ctx = panfrost_context(pipe);

        /* If there is nothing drawn, skip the frame */
        if (!ctx->draw_count && !(ctx->dirty & PAN_DIRTY_DUMMY)) return;

        /* Whether to stall the pipeline for immediately correct results */
        bool flush_immediate = flags & PIPE_FLUSH_END_OF_FRAME;

        /* Submit the frame itself */
        panfrost_submit_frame(ctx, flush_immediate);

        /* Prepare for the next frame */
        panfrost_invalidate_frame(ctx);

#ifdef USE_SLOWFB
#ifndef DRY_RUN

        if (panfrost_is_scanout(ctx) && !dont_scanout) {
                /* Display the frame in our cute little window */
                slowfb_update((uint8_t *) ctx->framebuffer.cpu, ctx->pipe_framebuffer.width, ctx->pipe_framebuffer.height);
        }

#endif
#endif
}

#define DEFINE_CASE(c) case PIPE_PRIM_##c: return MALI_GL_##c;

static int
g2m_draw_mode(enum pipe_prim_type mode)
{
        switch (mode) {
                DEFINE_CASE(POINTS);
                DEFINE_CASE(LINES);
                DEFINE_CASE(LINE_LOOP);
                DEFINE_CASE(LINE_STRIP);
                DEFINE_CASE(TRIANGLES);
                DEFINE_CASE(TRIANGLE_STRIP);
                DEFINE_CASE(TRIANGLE_FAN);

        default:
                printf("Illegal draw mode %d\n", mode);
                assert(0);
                return MALI_GL_LINE_LOOP;
        }
}

#undef DEFINE_CASE

static unsigned
panfrost_translate_index_size(unsigned size)
{
        switch (size) {
        case 1:
                return MALI_DRAW_INDEXED_UINT8;

        case 2:
                return MALI_DRAW_INDEXED_UINT16;

        case 4:
                return MALI_DRAW_INDEXED_UINT32;

        default:
                printf("Unknown index size %d\n", size);
                assert(0);
                return 0;
        }
}

static const uint8_t *
panfrost_get_index_buffer_raw(const struct pipe_draw_info *info)
{
        if (info->has_user_indices) {
                return (const uint8_t *) info->index.user;
        } else {
                struct panfrost_resource *rsrc = (struct panfrost_resource *) (info->index.resource);
                return (const uint8_t *) rsrc->cpu[0];
        }
}

bool needs_dummy_draw = true;

static void
panfrost_draw_vbo(
        struct pipe_context *pipe,
        const struct pipe_draw_info *info);

/* XXX: First frame w/ a draw seems to fail... so inject a fake frame */

static void
panfrost_maybe_dummy_draw(struct panfrost_context *ctx, const struct pipe_draw_info *info)
{
        if (!needs_dummy_draw)
                return;

        needs_dummy_draw = false;
        dont_scanout = true;

        panfrost_draw_vbo((struct pipe_context *) ctx, info);
        panfrost_flush((struct pipe_context *) ctx, NULL, 0);

        dont_scanout = false;
}

static void
panfrost_draw_vbo(
        struct pipe_context *pipe,
        const struct pipe_draw_info *info)
{
        struct panfrost_context *ctx = panfrost_context(pipe);

        panfrost_maybe_dummy_draw(ctx, info);

        ctx->payload_vertex.draw_start = info->start;
        ctx->payload_tiler.draw_start = info->start;

        int mode = info->mode;

        /* Fallback for non-ES draw modes */

        if (info->mode >= PIPE_PRIM_QUADS) {
                mode = PIPE_PRIM_TRIANGLE_STRIP;
                /*
                util_primconvert_save_rasterizer_state(ctx->primconvert, &ctx->rasterizer->base);
                util_primconvert_draw_vbo(ctx->primconvert, info);
                printf("Fallback\n");
                return; */
        }

        ctx->payload_tiler.prefix.draw_mode = g2m_draw_mode(mode);

        ctx->vertex_count = info->count;

        int invocation_count = info->index_size ? (info->start + ctx->vertex_count) : ctx->vertex_count;
        ctx->payload_vertex.prefix.invocation_count = MALI_POSITIVE(invocation_count);
        ctx->payload_tiler.prefix.invocation_count = MALI_POSITIVE(invocation_count);

        /* For higher amounts of vertices (greater than what fits in a 16-bit
         * short), the other value is needed, otherwise there will be bizarre
         * rendering artefacts. It's not clear what these values mean yet. */

        ctx->payload_tiler.prefix.unknown_draw &= ~(0x3000 | 0x18000);
        ctx->payload_tiler.prefix.unknown_draw |= (ctx->vertex_count > 65535) ? 0x3000 : 0x18000;

        if (info->index_size) {

                ctx->payload_vertex.draw_start = 0;
                ctx->payload_tiler.draw_start = 0;
                //ctx->payload_tiler.prefix.negative_start = -info->start;
                ctx->payload_tiler.prefix.index_count = MALI_POSITIVE(info->count);

                //assert(!info->restart_index); /* TODO: Research */
                assert(!info->index_bias);
                //assert(!info->min_index); /* TODO: Use value */

                ctx->payload_tiler.prefix.unknown_draw |= panfrost_translate_index_size(info->index_size);

                const uint8_t *ibuf8 = panfrost_get_index_buffer_raw(info);

                ctx->payload_tiler.prefix.indices = panfrost_upload(&ctx->cmdstream, ibuf8 + (info->start * info->index_size), info->count * info->index_size, true);
        } else {
                /* Index count == vertex count, if no indexing is applied, as
                 * if it is internally indexed in the expected order */

                ctx->payload_tiler.prefix.negative_start = 0;
                ctx->payload_tiler.prefix.index_count = MALI_POSITIVE(ctx->vertex_count);

                /* Reverse index state */
                ctx->payload_tiler.prefix.unknown_draw &= ~MALI_DRAW_INDEXED_UINT32;
                ctx->payload_tiler.prefix.indices = (uintptr_t) NULL;
        }

        /* Fire off the draw itself */
        panfrost_queue_draw(ctx);
}

/* CSO state */

static void
panfrost_generic_cso_delete(struct pipe_context *pctx, void *hwcso)
{
        free(hwcso);
}

static void
panfrost_set_scissor(struct panfrost_context *ctx)
{
        const struct pipe_scissor_state *ss = &ctx->scissor;

        if (ss && ctx->rasterizer && ctx->rasterizer->base.scissor && 0) {
                ctx->viewport.viewport0[0] = ss->minx;
                ctx->viewport.viewport0[1] = ss->miny;
                ctx->viewport.viewport1[0] = MALI_POSITIVE(ss->maxx);
                ctx->viewport.viewport1[1] = MALI_POSITIVE(ss->maxy);
        } else {
                ctx->viewport.viewport0[0] = 0;
                ctx->viewport.viewport0[1] = 0;
                ctx->viewport.viewport1[0] = MALI_POSITIVE(ctx->pipe_framebuffer.width);
                ctx->viewport.viewport1[1] = MALI_POSITIVE(ctx->pipe_framebuffer.height);

        }

        ctx->dirty |= PAN_DIRTY_VIEWPORT;
}

static void *
panfrost_create_rasterizer_state(
        struct pipe_context *pctx,
        const struct pipe_rasterizer_state *cso)
{
        struct panfrost_rasterizer *so = CALLOC_STRUCT(panfrost_rasterizer);

        so->base = *cso;

        /* Bitmask, unknown meaning of the start value */
#ifdef T8XX
        so->tiler_gl_enables = 0x7;
#else
        so->tiler_gl_enables = 0x105;
#endif

        so->tiler_gl_enables |= MALI_GL_FRONT_FACE(
                                        cso->front_ccw ? MALI_GL_CCW : MALI_GL_CW);

        if (cso->cull_face & PIPE_FACE_FRONT)
                so->tiler_gl_enables |= MALI_GL_CULL_FACE_FRONT;

        if (cso->cull_face & PIPE_FACE_BACK)
                so->tiler_gl_enables |= MALI_GL_CULL_FACE_BACK;

        return so;
}

static void
panfrost_bind_rasterizer_state(
        struct pipe_context *pctx,
        void *hwcso)
{
        struct panfrost_context *ctx = panfrost_context(pctx);
        struct pipe_rasterizer_state *cso = hwcso;

        if (!hwcso) {
                /* XXX: How to unbind rasterizer state? */
                return;
        }

        /* If scissor test has changed, we'll need to update that now */
        bool update_scissor = !ctx->rasterizer || ctx->rasterizer->base.scissor != cso->scissor;

        ctx->rasterizer = hwcso;

        /* Actualise late changes */
        if (update_scissor)
                panfrost_set_scissor(ctx);

        ctx->dirty |= PAN_DIRTY_RASTERIZER;
}

static void *
panfrost_create_vertex_elements_state(
        struct pipe_context *pctx,
        unsigned num_elements,
        const struct pipe_vertex_element *elements)
{
        struct panfrost_vertex_state *so = CALLOC_STRUCT(panfrost_vertex_state);

        so->num_elements = num_elements;
        memcpy(so->pipe, elements, sizeof(*elements) * num_elements);

        for (int i = 0; i < num_elements; ++i) {
                so->hw[i].index = elements[i].vertex_buffer_index;

                enum pipe_format fmt = elements[i].src_format;
                const struct util_format_description *desc = util_format_description(fmt);
                struct util_format_channel_description chan = desc->channel[0];

                int type = 0;

                switch (chan.type) {
                case UTIL_FORMAT_TYPE_UNSIGNED:
                case UTIL_FORMAT_TYPE_SIGNED:
                        if (chan.size == 8)
                                type = MALI_ATYPE_BYTE;
                        else if (chan.size == 16)
                                type = MALI_ATYPE_SHORT;
                        else if (chan.size == 32)
                                type = MALI_ATYPE_INT;
                        else {
                                printf("BAD INT SIZE %d\n", chan.size);
                                assert(0);
                        }

                        break;

                case UTIL_FORMAT_TYPE_FLOAT:
                        type = MALI_ATYPE_FLOAT;
                        break;

                default:
                        printf("Unknown atype %d\n", chan.type);
                        assert(0);
                }

                so->hw[i].type = type;
                so->nr_components[i] = desc->nr_channels;
                so->hw[i].nr_components = MALI_POSITIVE(4); /* XXX: Why is this needed? */
                so->hw[i].not_normalised = !chan.normalized;

                /* Bit used for both signed/unsigned and full/half designation */
                so->hw[i].is_int_signed =
                        (chan.type == UTIL_FORMAT_TYPE_SIGNED) ? 1 :
                        (chan.type == UTIL_FORMAT_TYPE_FLOAT && chan.size != 32) ? 1 :
                        0;

                so->hw[i].unknown1 = 0x2a22;
                so->hw[i].unknown2 = 0x1;

                /* The field itself should probably be shifted over */
                so->hw[i].src_offset = elements[i].src_offset;
        }

        return so;
}

static void
panfrost_bind_vertex_elements_state(
        struct pipe_context *pctx,
        void *hwcso)
{
        struct panfrost_context *ctx = panfrost_context(pctx);

        ctx->vertex = hwcso;
        ctx->dirty |= PAN_DIRTY_VERTEX;
}

static void *
panfrost_create_shader_state(
        struct pipe_context *pctx,
        const struct pipe_shader_state *cso)
{
        struct panfrost_shader_state *so = CALLOC_STRUCT(panfrost_shader_state);
        so->base = *cso;

        /* Token deep copy to prevent memory corruption */

        if (cso->type == PIPE_SHADER_IR_TGSI)
                so->base.tokens = tgsi_dup_tokens(so->base.tokens);

        return so;
}

static void
panfrost_delete_shader_state(
        struct pipe_context *pctx,
        void *so)
{
        free(so);
}

static void *
panfrost_create_sampler_state(
        struct pipe_context *pctx,
        const struct pipe_sampler_state *cso)
{
        struct panfrost_sampler_state *so = CALLOC_STRUCT(panfrost_sampler_state);
        so->base = *cso;

        /* sampler_state corresponds to mali_sampler_descriptor, which we can generate entirely here */

        struct mali_sampler_descriptor sampler_descriptor = {
                .filter_mode = MALI_GL_TEX_MIN(translate_tex_filter(cso->min_img_filter))
                | MALI_GL_TEX_MAG(translate_tex_filter(cso->mag_img_filter))
                | translate_mip_filter(cso->min_mip_filter)
                | 0x20,

                .wrap_s = translate_tex_wrap(cso->wrap_s),
                .wrap_t = translate_tex_wrap(cso->wrap_t),
                .wrap_r = translate_tex_wrap(cso->wrap_r),
                .compare_func = panfrost_translate_alt_compare_func(cso->compare_func),
                .border_color = {
                        cso->border_color.f[0],
                        cso->border_color.f[1],
                        cso->border_color.f[2],
                        cso->border_color.f[3]
                },
                .min_lod = FIXED_16(0.0),
                .max_lod = FIXED_16(31.0),
                .unknown2 = 1,
        };

        so->hw = sampler_descriptor;

        return so;
}

static void
panfrost_bind_sampler_states(
        struct pipe_context *pctx,
        enum pipe_shader_type shader,
        unsigned start_slot, unsigned num_sampler,
        void **sampler)
{
        assert(start_slot == 0);

        struct panfrost_context *ctx = panfrost_context(pctx);

        /* XXX: Should upload, not just copy? */
        ctx->sampler_count[shader] = num_sampler;
        memcpy(ctx->samplers[shader], sampler, num_sampler * sizeof (void *));

        ctx->dirty |= PAN_DIRTY_SAMPLERS;
}

static void
panfrost_bind_fs_state(
        struct pipe_context *pctx,
        void *hwcso)
{
        struct panfrost_context *ctx = panfrost_context(pctx);

        ctx->fs = hwcso;

        if (hwcso) {
                if (!ctx->fs->compiled) {
                        panfrost_shader_compile(ctx, &ctx->fs->tripipe, NULL, JOB_TYPE_TILER, hwcso);
                        ctx->fs->compiled = true;
                }
        }

        ctx->dirty |= PAN_DIRTY_FS;
}

static void
panfrost_bind_vs_state(
        struct pipe_context *pctx,
        void *hwcso)
{
        struct panfrost_context *ctx = panfrost_context(pctx);

        ctx->vs = hwcso;

        if (hwcso) {
                if (!ctx->vs->compiled) {
                        panfrost_shader_compile(ctx, &ctx->vs->tripipe, NULL, JOB_TYPE_VERTEX, hwcso);
                        ctx->vs->compiled = true;
                }
        }

        ctx->dirty |= PAN_DIRTY_VS;
}

static void
panfrost_set_vertex_buffers(
        struct pipe_context *pctx,
        unsigned start_slot,
        unsigned num_buffers,
        const struct pipe_vertex_buffer *buffers)
{
        struct panfrost_context *ctx = panfrost_context(pctx);
        assert(num_buffers <= PIPE_MAX_ATTRIBS);

        /* XXX: Dirty tracking? etc */
        if (buffers) {
                size_t sz = sizeof(buffers[0]) * num_buffers;
                ctx->vertex_buffers = malloc(sz);
                ctx->vertex_buffer_count = num_buffers;
                memcpy(ctx->vertex_buffers, buffers, sz);
        } else {
                /* XXX leak */
                ctx->vertex_buffers = NULL;
                ctx->vertex_buffer_count = 0;
        }
}

static void
panfrost_set_constant_buffer(
        struct pipe_context *pctx,
        enum pipe_shader_type shader, uint index,
        const struct pipe_constant_buffer *buf)
{
        struct panfrost_context *ctx = panfrost_context(pctx);
        struct panfrost_constant_buffer *pbuf = &ctx->constant_buffer[shader];

        size_t sz = buf ? buf->buffer_size : 0;

        /* Free previous buffer */

        pbuf->dirty = true;
        pbuf->size = sz;

        if (pbuf->buffer) {
                free(pbuf->buffer);
                pbuf->buffer = NULL;
        }

        /* If unbinding, we're done */

        if (!buf)
                return;

        /* Multiple constant buffers not yet supported */
        assert(index == 0);

        const void *cpu;

        struct panfrost_resource *rsrc = (struct panfrost_resource *) (buf->buffer);

        if (rsrc) {
                cpu = rsrc->cpu[0];
        } else if (buf->user_buffer) {
                cpu = buf->user_buffer;
        } else {
                printf("No constant buffer?\n");
                return;
        }

        /* Copy the constant buffer into the driver context for later upload */

        pbuf->buffer = malloc(sz);
        memcpy(pbuf->buffer, cpu, sz);
}

static void
panfrost_set_stencil_ref(
        struct pipe_context *pctx,
        const struct pipe_stencil_ref *ref)
{
        struct panfrost_context *ctx = panfrost_context(pctx);
        ctx->stencil_ref = *ref;

        /* Shader core dirty */
        ctx->dirty |= PAN_DIRTY_FS;
}

static struct pipe_sampler_view *
panfrost_create_sampler_view(
        struct pipe_context *pctx,
        struct pipe_resource *texture,
        const struct pipe_sampler_view *template)
{
        struct panfrost_sampler_view *so = CALLOC_STRUCT(panfrost_sampler_view);

        pipe_reference(NULL, &texture->reference);

        struct panfrost_resource *prsrc = (struct panfrost_resource *) texture;

        so->base = *template;
        so->base.texture = texture;
        so->base.reference.count = 1;
        so->base.context = pctx;

        /* sampler_views correspond to texture descriptors, minus the texture
         * (data) itself. So, we serialise the descriptor here and cache it for
         * later. */

        /* TODO: Other types of textures */
        assert(template->target == PIPE_TEXTURE_2D);

        /* Make sure it's something with which we're familiar */
        assert(prsrc->bytes_per_pixel >= 1 && prsrc->bytes_per_pixel <= 4);

        /* TODO: Detect from format better */
        bool depth = prsrc->base.format == PIPE_FORMAT_Z32_UNORM;
        bool has_alpha = true;
        bool alpha_only = prsrc->base.format == PIPE_FORMAT_A8_UNORM;

        struct mali_texture_descriptor texture_descriptor = {
                .width = MALI_POSITIVE(texture->width0),
                .height = MALI_POSITIVE(texture->height0),
                .depth = MALI_POSITIVE(texture->depth0),

                /* TODO: Decode */
                .format = {
                        .bottom = alpha_only ? 0x24 : ((depth ? 0x20 : 0x88)),
                        .unk1 = alpha_only ? 0x1 : (has_alpha ? 0x6 : 0xb),
                        .component_size = depth ? 0x5 : 0x3,
                        .nr_channels = MALI_POSITIVE((depth ? 2 : prsrc->bytes_per_pixel)),
                        .typeA = depth ? 2 : 5,

                        .usage1 = 0x0,
                        .is_not_cubemap = 1,

                        /* 0x11 - regular texture 2d, uncompressed tiled */
                        /* 0x12 - regular texture 2d, uncompressed linear */
                        /* 0x1c - AFBC compressed (internally tiled, probably) texture 2D */

                        .usage2 = prsrc->has_afbc ? 0x1c : (prsrc->tiled ? 0x11 : 0x12),
                },

                .swizzle_r = panfrost_translate_texture_swizzle(template->swizzle_r),
                .swizzle_g = panfrost_translate_texture_swizzle(template->swizzle_g),
                .swizzle_b = panfrost_translate_texture_swizzle(template->swizzle_b),
                .swizzle_a = panfrost_translate_texture_swizzle(template->swizzle_a),
        };

        /* TODO: Other base levels require adjusting dimensions / level numbers / etc */
        assert (template->u.tex.first_level == 0);

        texture_descriptor.nr_mipmap_levels = template->u.tex.last_level - template->u.tex.first_level;

        so->hw = texture_descriptor;

        return (struct pipe_sampler_view *) so;
}

static void
panfrost_set_sampler_views(
        struct pipe_context *pctx,
        enum pipe_shader_type shader,
        unsigned start_slot, unsigned num_views,
        struct pipe_sampler_view **views)
{
        struct panfrost_context *ctx = panfrost_context(pctx);

        assert(start_slot == 0);

        ctx->sampler_view_count[shader] = num_views;
        memcpy(ctx->sampler_views[shader], views, num_views * sizeof (void *));

        ctx->dirty |= PAN_DIRTY_TEXTURES;
}

static void
panfrost_sampler_view_destroy(
        struct pipe_context *pctx,
        struct pipe_sampler_view *views)
{
        //struct panfrost_context *ctx = panfrost_context(pctx);

        /* TODO */

        free(views);
}

/* TODO: Proper resource tracking depends on, well, proper resources. This
 * section will be woefully incomplete until we can sort out a proper DRM
 * driver. */

struct pipe_resource *
panfrost_resource_create_front(struct pipe_screen *screen,
                               const struct pipe_resource *template,
                               const void *map_front_private)
{
        struct panfrost_resource *so = CALLOC_STRUCT(panfrost_resource);
        struct panfrost_screen *pscreen = (struct panfrost_screen *) screen;

        so->base = *template;
        so->base.screen = screen;

        pipe_reference_init(&so->base.reference, 1);

        /* Fill out fields based on format itself */
        so->bytes_per_pixel = util_format_get_blocksize(template->format);

        /* TODO: Alignment? */
        so->stride = so->bytes_per_pixel * template->width0;

        size_t sz = so->stride;

        if (template->height0) sz *= template->height0;

        if (template->depth0) sz *= template->depth0;

        if ((template->bind & PIPE_BIND_RENDER_TARGET) || (template->bind & PIPE_BIND_DEPTH_STENCIL)) {
                if (template->bind & PIPE_BIND_DISPLAY_TARGET) {
                        /* TODO: Allocate display target surface */
                        so->cpu[0] = pscreen->any_context->framebuffer.cpu;
                        so->gpu[0] = pscreen->any_context->framebuffer.gpu;
                } else {
                        /* TODO: Mipmapped RTs */
                        //assert(template->last_level == 0);

                        /* Allocate the framebuffer as its own slab of GPU-accessible memory */
                        struct panfrost_memory slab;
                        panfrost_allocate_slab(pscreen->any_context, &slab, (sz / 4096) + 1, true, false, 0, 0, 0);

                        /* Make the resource out of the slab */
                        so->cpu[0] = slab.cpu;
                        so->gpu[0] = slab.gpu;
                }
        } else {
                /* TODO: For linear resources, allocate straight on the cmdstream for
                 * zero-copy operation */

                /* Tiling textures is almost always faster, unless we only use it once */
                so->tiled = (template->usage != PIPE_USAGE_STREAM);

                if (so->tiled) {
                        /* For tiled, we don't map directly, so just malloc any old buffer */

                        for (int l = 0; l < (template->last_level + 1); ++l) {
                                so->cpu[l] = malloc(sz);
                                //sz >>= 2;
                        }
                } else {
                        /* But for linear, we can! */

                        struct panfrost_memory slab;
                        panfrost_allocate_slab(pscreen->any_context, &slab, (sz / 4096) + 1, true, true, 0, 0, 0);

                        /* Make the resource out of the slab */
                        so->cpu[0] = slab.cpu;
                        so->gpu[0] = slab.gpu;
                }
        }

        return (struct pipe_resource *)so;
}

static struct pipe_resource *
panfrost_resource_create(struct pipe_screen *screen,
                         const struct pipe_resource *templat)
{
        return panfrost_resource_create_front(screen, templat, NULL);
}

static void
panfrost_resource_destroy(struct pipe_screen *screen,
                          struct pipe_resource *pt)
{
        printf("--resource destroy--\n");
        /* TODO */
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

        struct pipe_transfer *transfer = CALLOC_STRUCT(pipe_transfer);

        transfer->level = level;
        transfer->usage = usage;
        transfer->box = *box;
        transfer->stride = rsrc->stride;
        assert(!transfer->box.z);

        pipe_resource_reference(&transfer->resource, resource);

        *out_transfer = transfer;

        /* If non-zero level, it's a mipmapped resource and needs to be treated as such */
        rsrc->is_mipmap |= transfer->level;

        if (transfer->usage & PIPE_TRANSFER_MAP_DIRECTLY) {
                /* We cannot directly map tiled textures */

                if (rsrc->tiled)
                        return NULL;

                /* Otherwise, we're good to go! */
                rsrc->mapped_direct = true;
        }

        if (resource->bind & PIPE_BIND_DISPLAY_TARGET) {
                /* Mipmapped readpixels?! */
                assert(level == 0);

                /* Set the CPU mapping to that of the framebuffer in memory, untiled */
                rsrc->cpu[level] = ctx->framebuffer.cpu;

                /* Force a flush -- kill the pipeline */
                panfrost_flush(pctx, NULL, PIPE_FLUSH_END_OF_FRAME);
        } else if (resource->bind & PIPE_BIND_DEPTH_STENCIL) {
                /* Mipmapped readpixels?! */
                assert(level == 0);

                /* Set the CPU mapping to that of the depth/stencil buffer in memory, untiled */
                rsrc->cpu[level] = ctx->depth_stencil_buffer.cpu;
        }

        return rsrc->cpu[level] + transfer->box.x * rsrc->bytes_per_pixel + transfer->box.y * transfer->stride;
}

static void
panfrost_set_framebuffer_state(struct pipe_context *pctx,
                               const struct pipe_framebuffer_state *fb)
{
        struct panfrost_context *ctx = panfrost_context(pctx);

        /* Flush when switching away from an FBO */

        if (!panfrost_is_scanout(ctx)) {
                panfrost_flush(pctx, NULL, 0);
        }

        ctx->pipe_framebuffer.nr_cbufs = fb->nr_cbufs;
        ctx->pipe_framebuffer.samples = fb->samples;
        ctx->pipe_framebuffer.layers = fb->layers;
        ctx->pipe_framebuffer.width = MIN2(fb->width, 2048);
        ctx->pipe_framebuffer.height = MIN2(fb->height, 1280);

        for (int i = 0; i < PIPE_MAX_COLOR_BUFS; i++) {
                struct pipe_surface *cb = i < fb->nr_cbufs ? fb->cbufs[i] : NULL;

                /* check if changing cbuf */
                if (ctx->pipe_framebuffer.cbufs[i] == cb) continue;

                if (cb && (i != 0)) {
                        printf("XXX: Multiple render targets not supported before t7xx!\n");
                        assert(0);
                }

                /* assign new */
                pipe_surface_reference(&ctx->pipe_framebuffer.cbufs[i], cb);

                if (!cb)
                        continue;

                bool is_scanout = panfrost_is_scanout(ctx);

                if (is_scanout) {
                        /* Lie to use our own */
                        ((struct panfrost_resource *) ctx->pipe_framebuffer.cbufs[i]->texture)->gpu[0] = ctx->framebuffer.gpu;
                        ctx->pipe_framebuffer.width = 2048;
                        ctx->pipe_framebuffer.height = 1280;
                }

                ctx->vt_framebuffer = panfrost_emit_fbd(ctx);
                panfrost_attach_vt_framebuffer(ctx);
                panfrost_new_frag_framebuffer(ctx);
                panfrost_set_scissor(ctx);

                struct panfrost_resource *tex = ((struct panfrost_resource *) ctx->pipe_framebuffer.cbufs[i]->texture);

                if (!is_scanout && !tex->has_afbc) {
                        /* The blob is aggressive about enabling AFBC. As such,
                         * it's pretty much necessary to use it here, since we
                         * have no traces of non-compressed FBO. */

                        panfrost_enable_afbc(ctx, tex, false);
                }

                if (is_scanout && !tex->has_checksum && USE_TRANSACTION_ELIMINATION) {
                        /* Enable transaction elimination if we can */
                        panfrost_enable_checksum(ctx, tex);
                }
        }

        {
                struct pipe_surface *zb = fb->zsbuf;

                if (ctx->pipe_framebuffer.zsbuf != zb) {
                        pipe_surface_reference(&ctx->pipe_framebuffer.zsbuf, zb);

                        if (zb) {
                                /* FBO has depth */

                                ctx->vt_framebuffer = panfrost_emit_fbd(ctx);
                                panfrost_attach_vt_framebuffer(ctx);
                                panfrost_new_frag_framebuffer(ctx);
                                panfrost_set_scissor(ctx);

                                struct panfrost_resource *tex = ((struct panfrost_resource *) ctx->pipe_framebuffer.zsbuf->texture);

                                if (!tex->has_afbc && !panfrost_is_scanout(ctx))
                                        panfrost_enable_afbc(ctx, tex, true);
                        }
                }
        }

        /* Force a clear XXX wrong? */
        if (ctx->last_clear.color)
                panfrost_clear(&ctx->base, ctx->last_clear.buffers, ctx->last_clear.color, ctx->last_clear.depth, ctx->last_clear.stencil);

        /* Don't consider the buffer dirty */
        ctx->dirty &= ~PAN_DIRTY_DUMMY;
}

static void *
panfrost_create_blend_state(struct pipe_context *pipe,
                            const struct pipe_blend_state *blend)
{
        struct panfrost_context *ctx = panfrost_context(pipe);
        struct panfrost_blend_state *so = CALLOC_STRUCT(panfrost_blend_state);
        so->base = *blend;

        /* TODO: The following features are not yet implemented */
        assert(!blend->logicop_enable);
        assert(!blend->alpha_to_coverage);
        assert(!blend->alpha_to_one);

        /* Compile the blend state, first as fixed-function if we can */

        if (panfrost_make_fixed_blend_mode(&blend->rt[0], &so->equation, blend->rt[0].colormask, &ctx->blend_color))
                return so;

        /* If we can't, compile a blend shader instead */

        panfrost_make_blend_shader(ctx, so, &ctx->blend_color);

        return so;
}

static void
panfrost_bind_blend_state(struct pipe_context *pipe,
                          void *cso)
{
        struct panfrost_context *ctx = panfrost_context(pipe);
        struct pipe_blend_state *blend = (struct pipe_blend_state *) cso;
        struct panfrost_blend_state *pblend = (struct panfrost_blend_state *) cso;
        ctx->blend = pblend;

        if (!blend)
                return;

        SET_BIT(ctx->fragment_shader_core.unknown2_4, MALI_NO_DITHER, !blend->dither);

        /* TODO: Attach color */

        /* Shader itself is not dirty, but the shader core is */
        ctx->dirty |= PAN_DIRTY_FS;
}

static void
panfrost_delete_blend_state(struct pipe_context *pipe,
                            void *blend)
{
        free(blend);
}

static void
panfrost_set_blend_color(struct pipe_context *pipe,
                         const struct pipe_blend_color *blend_color)
{
        struct panfrost_context *ctx = panfrost_context(pipe);

        /* If blend_color is we're unbinding, so ctx->blend_color is now undefined -> nothing to do */

        if (blend_color) {
                ctx->blend_color = *blend_color;

                /* The blend mode depends on the blend constant color, due to the
                 * fixed/programmable split. So, we're forced to regenerate the blend
                 * equation */

                /* TODO: Attach color */
        }
}

static void *
panfrost_create_depth_stencil_state(struct pipe_context *pipe,
                                    const struct pipe_depth_stencil_alpha_state *depth_stencil)
{
        return mem_dup(depth_stencil, sizeof(*depth_stencil));
}

static void
panfrost_bind_depth_stencil_state(struct pipe_context *pipe,
                                  void *cso)
{
        struct panfrost_context *ctx = panfrost_context(pipe);
        struct pipe_depth_stencil_alpha_state *depth_stencil = cso;
        ctx->depth_stencil = depth_stencil;

        if (!depth_stencil)
                return;

        /* Alpha does not exist on ES2... */
        assert(!depth_stencil->alpha.enabled);

        /* Stencil state */
        SET_BIT(ctx->fragment_shader_core.unknown2_4, MALI_STENCIL_TEST, depth_stencil->stencil[0].enabled); /* XXX: which one? */

        panfrost_make_stencil_state(&depth_stencil->stencil[0], &ctx->fragment_shader_core.stencil_front);
        ctx->fragment_shader_core.stencil_mask_front = depth_stencil->stencil[0].writemask;

        panfrost_make_stencil_state(&depth_stencil->stencil[1], &ctx->fragment_shader_core.stencil_back);
        ctx->fragment_shader_core.stencil_mask_back = depth_stencil->stencil[1].writemask;

        /* Depth state (TODO: Refactor) */
        SET_BIT(ctx->fragment_shader_core.unknown2_3, MALI_DEPTH_TEST, depth_stencil->depth.enabled);

        int func = depth_stencil->depth.enabled ? depth_stencil->depth.func : PIPE_FUNC_ALWAYS;

        ctx->fragment_shader_core.unknown2_3 &= ~MALI_DEPTH_FUNC_MASK;
        ctx->fragment_shader_core.unknown2_3 |= MALI_DEPTH_FUNC(panfrost_translate_compare_func(func));

        /* Bounds test not implemented */
        assert(!depth_stencil->depth.bounds_test);

        ctx->dirty |= PAN_DIRTY_FS;
}

static void
panfrost_delete_depth_stencil_state(struct pipe_context *pipe, void *depth)
{
        free( depth );
}

static void
panfrost_set_sample_mask(struct pipe_context *pipe,
                         unsigned sample_mask)
{
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

static void
panfrost_set_clip_state(struct pipe_context *pipe,
                        const struct pipe_clip_state *clip)
{
        //struct panfrost_context *panfrost = panfrost_context(pipe);
}

static void
panfrost_set_viewport_states(struct pipe_context *pipe,
                             unsigned start_slot,
                             unsigned num_viewports,
                             const struct pipe_viewport_state *viewports)
{
        struct panfrost_context *ctx = panfrost_context(pipe);

        assert(start_slot == 0);
        assert(num_viewports == 1);

        ctx->pipe_viewport = *viewports;

#if 0
        /* TODO: What if not centered? */
        float w = abs(viewports->scale[0]) * 2.0;
        float h = abs(viewports->scale[1]) * 2.0;

        ctx->viewport.viewport1[0] = MALI_POSITIVE((int) w);
        ctx->viewport.viewport1[1] = MALI_POSITIVE((int) h);
#endif
}

static void
panfrost_set_scissor_states(struct pipe_context *pipe,
                            unsigned start_slot,
                            unsigned num_scissors,
                            const struct pipe_scissor_state *scissors)
{
        struct panfrost_context *ctx = panfrost_context(pipe);

        assert(start_slot == 0);
        assert(num_scissors == 1);

        ctx->scissor = *scissors;

        panfrost_set_scissor(ctx);
}

static void
panfrost_set_polygon_stipple(struct pipe_context *pipe,
                             const struct pipe_poly_stipple *stipple)
{
        //struct panfrost_context *panfrost = panfrost_context(pipe);
}

static void
panfrost_set_active_query_state(struct pipe_context *pipe,
                                boolean enable)
{
        //struct panfrost_context *panfrost = panfrost_context(pipe);
}

static void
panfrost_destroy(struct pipe_context *pipe)
{
        struct panfrost_context *panfrost = panfrost_context(pipe);

        if (panfrost->blitter)
                util_blitter_destroy(panfrost->blitter);
}

static void
panfrost_tile_texture(struct panfrost_context *ctx, struct panfrost_resource *rsrc, int level)
{
        /* If we're direct mapped, we're done; don't do any swizzling / copies / etc */
        if (rsrc->mapped_direct)
                return;

        int width = rsrc->base.width0 >> level;
        int height = rsrc->base.height0 >> level;

        /* Estimate swizzled bitmap size. Slight overestimates are fine.
         * Underestimates will result in memory corruption or worse. */

        int swizzled_sz = panfrost_swizzled_size(width, height, rsrc->bytes_per_pixel);

        /* Allocate the transfer given that known size but do not copy */
        uint8_t *swizzled = panfrost_allocate_transfer(&ctx->textures, swizzled_sz, &rsrc->gpu[level]);

        if (rsrc->tiled) {
                /* Run actual texture swizzle, writing directly to the mapped
                 * GPU chunk we allocated */

                panfrost_texture_swizzle(width, height, rsrc->bytes_per_pixel, rsrc->stride, rsrc->cpu[level], swizzled);
        } else {
                /* If indirect linear, just do a dumb copy */

                memcpy(swizzled, rsrc->cpu[level], rsrc->stride * height);
        }
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
                        assert(!prsrc->has_afbc);
                        panfrost_tile_texture(ctx, prsrc, transfer->level);
                }
        }

        /* Derefence the resource */
        pipe_resource_reference(&transfer->resource, NULL);

        /* Transfer itself is CALLOCed at the moment */
        free(transfer);
}

static void
panfrost_blit(struct pipe_context *pipe,
              const struct pipe_blit_info *info)
{
        /* STUB */
        printf("Skipping blit XXX\n");
        return;
}

static void
panfrost_allocate_slab(struct panfrost_context *ctx,
                       struct panfrost_memory *mem,
                       size_t pages,
                       bool mapped,
                       bool same_va,
                       int extra_flags,
                       int commit_count,
                       int extent)
{
        int flags = MALI_MEM_PROT_CPU_RD | MALI_MEM_PROT_CPU_WR | MALI_MEM_PROT_GPU_RD | MALI_MEM_PROT_GPU_WR;

        flags |= extra_flags;

        /* w+x are mutually exclusive */
        if (extra_flags & MALI_MEM_PROT_GPU_EX)
                flags &= ~MALI_MEM_PROT_GPU_WR;

        if (same_va)
                flags |= MALI_MEM_SAME_VA;

        if (commit_count || extent)
                pandev_general_allocate(ctx->fd, pages, commit_count, extent, flags, &mem->gpu);
        else
                pandev_standard_allocate(ctx->fd, pages, flags, &mem->gpu);

        mem->size = pages * 4096;

        /* mmap for 64-bit, mmap64 for 32-bit. ironic, I know */
        if (mapped) {
                if ((mem->cpu = mmap(NULL, mem->size, 3, 1, ctx->fd, mem->gpu)) == MAP_FAILED) {
                        perror("mmap");
                        abort();
                }
        }

        mem->stack_bottom = 0;
}

/* Setups a framebuffer, either by itself (with the independent slow-fb
 * interface) or from an existing pointer (for shared memory, from the winsys)
 * */

static void
panfrost_setup_framebuffer(struct panfrost_context *ctx, int width, int height)
{
        /* drisw rounds the stride */
        int rw = 16.0 * (int) ceil((float) width / 16.0);

        size_t framebuffer_sz = rw * height * 4;
        posix_memalign((void **) &ctx->framebuffer.cpu, CACHE_LINE_SIZE, framebuffer_sz);
        struct slowfb_info info = slowfb_init((uint8_t *) (ctx->framebuffer.cpu), rw, height);

        /* May not be the same as our original alloc if we're using XShm, etc */
        ctx->framebuffer.cpu = info.framebuffer;

        struct mali_mem_import_user_buffer framebuffer_handle = { .ptr = (uint64_t) (uintptr_t) ctx->framebuffer.cpu, .length = framebuffer_sz };

        struct mali_ioctl_mem_import framebuffer_import = {
                .phandle = (uint64_t) (uintptr_t) &framebuffer_handle,
                .type = MALI_MEM_IMPORT_TYPE_USER_BUFFER,
                .flags = MALI_MEM_PROT_CPU_RD | MALI_MEM_PROT_CPU_WR | MALI_MEM_PROT_GPU_RD | MALI_MEM_PROT_GPU_WR | MALI_MEM_IMPORT_SHARED,
        };

        pandev_ioctl(ctx->fd, MALI_IOCTL_MEM_IMPORT, &framebuffer_import);

        /* It feels like this mmap is backwards :p */
        uint64_t gpu_addr = (uint64_t) mmap(NULL, framebuffer_import.va_pages * 4096, 3, 1, ctx->fd, framebuffer_import.gpu_va);

        ctx->framebuffer.gpu = gpu_addr;
        ctx->framebuffer.size = info.stride * height;
        ctx->scanout_stride = info.stride;

        ctx->pipe_framebuffer.nr_cbufs = 1;
        ctx->pipe_framebuffer.width = width;
        ctx->pipe_framebuffer.height = height;
}

static void
panfrost_setup_hardware(struct panfrost_context *ctx)
{
        ctx->fd = pandev_open();

#ifdef USE_SLOWFB
        panfrost_setup_framebuffer(ctx, 2048, 1280);
#endif

        for (int i = 0; i < sizeof(ctx->cmdstream_rings) / sizeof(ctx->cmdstream_rings[0]); ++i)
                panfrost_allocate_slab(ctx, &ctx->cmdstream_rings[i], 8 * 64 * 8 * 16, true, true, 0, 0, 0);

        panfrost_allocate_slab(ctx, &ctx->cmdstream_persistent, 8 * 64 * 8 * 2, true, true, 0, 0, 0);
        panfrost_allocate_slab(ctx, &ctx->textures, 4 * 64 * 64 * 4, true, true, 0, 0, 0);
        panfrost_allocate_slab(ctx, &ctx->scratchpad, 64, true, true, 0, 0, 0);
        panfrost_allocate_slab(ctx, &ctx->varying_mem, 16384, false, true, 0, 0, 0);
        panfrost_allocate_slab(ctx, &ctx->shaders, 4096, true, false, MALI_MEM_PROT_GPU_EX, 0, 0);
        panfrost_allocate_slab(ctx, &ctx->tiler_heap, 32768, false, false, 0, 0, 0);
        panfrost_allocate_slab(ctx, &ctx->misc_0, 128, false, false, 0, 0, 0);

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

/* New context creation, which also does hardware initialisation since I don't
 * know the better way to structure this :smirk: */

struct pipe_context *
panfrost_create_context(struct pipe_screen *screen, void *priv, unsigned flags)
{
        screen->resource_create = panfrost_resource_create;
        screen->resource_destroy = panfrost_resource_destroy;
        screen->resource_create_front = panfrost_resource_create_front;
        screen->transfer_helper = u_transfer_helper_create(&transfer_vtbl, true, true, true, true);

        struct panfrost_context *ctx = CALLOC_STRUCT(panfrost_context);
        memset(ctx, 0, sizeof(*ctx));
        struct pipe_context *gallium = (struct pipe_context *) ctx;

        struct panfrost_screen *pscreen = (struct panfrost_screen *) screen;

        if (!pscreen->any_context)
                pscreen->any_context = ctx;

        gallium->screen = screen;

        gallium->destroy = panfrost_destroy;

        gallium->set_framebuffer_state = panfrost_set_framebuffer_state;

        gallium->transfer_map = panfrost_transfer_map;
        gallium->transfer_unmap = panfrost_transfer_unmap;

        gallium->transfer_flush_region = u_transfer_helper_transfer_flush_region;
        gallium->buffer_subdata = u_default_buffer_subdata;
        gallium->texture_subdata = u_default_texture_subdata;
        gallium->clear_texture = util_clear_texture;

        gallium->create_surface = panfrost_create_surface;
        gallium->surface_destroy = panfrost_surface_destroy;

        gallium->flush = panfrost_flush;
        gallium->clear = panfrost_clear;
        gallium->draw_vbo = panfrost_draw_vbo;

        gallium->set_vertex_buffers = panfrost_set_vertex_buffers;
        gallium->set_constant_buffer = panfrost_set_constant_buffer;

        gallium->set_stencil_ref = panfrost_set_stencil_ref;

        gallium->create_sampler_view = panfrost_create_sampler_view;
        gallium->set_sampler_views = panfrost_set_sampler_views;
        gallium->sampler_view_destroy = panfrost_sampler_view_destroy;

        gallium->create_rasterizer_state = panfrost_create_rasterizer_state;
        gallium->bind_rasterizer_state = panfrost_bind_rasterizer_state;
        gallium->delete_rasterizer_state = panfrost_generic_cso_delete;

        gallium->create_vertex_elements_state = panfrost_create_vertex_elements_state;
        gallium->bind_vertex_elements_state = panfrost_bind_vertex_elements_state;
        gallium->delete_vertex_elements_state = panfrost_generic_cso_delete;

        gallium->create_fs_state = panfrost_create_shader_state;
        gallium->delete_fs_state = panfrost_delete_shader_state;
        gallium->bind_fs_state = panfrost_bind_fs_state;

        gallium->create_vs_state = panfrost_create_shader_state;
        gallium->delete_vs_state = panfrost_delete_shader_state;
        gallium->bind_vs_state = panfrost_bind_vs_state;

        gallium->create_sampler_state = panfrost_create_sampler_state;
        gallium->delete_sampler_state = panfrost_generic_cso_delete;
        gallium->bind_sampler_states = panfrost_bind_sampler_states;

        gallium->create_blend_state = panfrost_create_blend_state;
        gallium->bind_blend_state   = panfrost_bind_blend_state;
        gallium->delete_blend_state = panfrost_delete_blend_state;

        gallium->set_blend_color = panfrost_set_blend_color;

        gallium->create_depth_stencil_alpha_state = panfrost_create_depth_stencil_state;
        gallium->bind_depth_stencil_alpha_state   = panfrost_bind_depth_stencil_state;
        gallium->delete_depth_stencil_alpha_state = panfrost_delete_depth_stencil_state;

        gallium->set_sample_mask = panfrost_set_sample_mask;

        gallium->set_clip_state = panfrost_set_clip_state;
        gallium->set_viewport_states = panfrost_set_viewport_states;
        gallium->set_scissor_states = panfrost_set_scissor_states;
        gallium->set_polygon_stipple = panfrost_set_polygon_stipple;
        gallium->set_active_query_state = panfrost_set_active_query_state;

        gallium->blit = panfrost_blit;

        /* XXX: leaks */
        gallium->stream_uploader = u_upload_create_default(gallium);
        gallium->const_uploader = gallium->stream_uploader;
        assert(gallium->stream_uploader);

        ctx->primconvert = util_primconvert_create(gallium,
                           (1 << PIPE_PRIM_QUADS) - 1);
        assert(ctx->primconvert);

        ctx->blitter = util_blitter_create(gallium);
        assert(ctx->blitter);

        /* Prepare for render! */
        panfrost_setup_hardware(ctx);

        /* TODO: XXX */
        ctx->vt_framebuffer = panfrost_emit_fbd(ctx);

        panfrost_emit_vertex_payload(ctx);
        panfrost_emit_tiler_payload(ctx);
        panfrost_invalidate_frame(ctx);
        panfrost_viewport(ctx, 0.0, 1.0, 0, 0, ctx->pipe_framebuffer.width, ctx->pipe_framebuffer.height);
        panfrost_new_frag_framebuffer(ctx);
        panfrost_default_shader_backend(ctx);
        panfrost_generate_space_filler_indices();


        return gallium;
}
