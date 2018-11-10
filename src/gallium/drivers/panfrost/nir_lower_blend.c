/*
 * Copyright © 2015 Broadcom
 * Copyright © 2018 Alyssa Rosenzweig
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "compiler/nir/nir_builder.h"
#include "pipe/p_state.h"
#include "util/u_format.h"
#include "nir_lower_blend.h"

/* src and dst are vec4 */

nir_ssa_def *
nir_blend_channel_f(nir_builder *b,
                    nir_ssa_def *src,
                    nir_ssa_def *dst,
                    unsigned factor)
{
        switch(factor) {
        case PIPE_BLENDFACTOR_ONE:
                return nir_imm_float(b, 1.0);
        case PIPE_BLENDFACTOR_SRC_COLOR:
                return nir_channels(b, src, 0x7);
        case PIPE_BLENDFACTOR_SRC_ALPHA:
                return nir_channel(b, src, 3);
        case PIPE_BLENDFACTOR_DST_ALPHA:
                return nir_channel(b, dst, 3);
        case PIPE_BLENDFACTOR_DST_COLOR:
                return nir_channels(b, src, 0x7);
#if 0
        case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
                if (channel != 3) {
                        return nir_fmin(b,
                                        src[3],
                                        nir_fsub(b,
                                                 nir_imm_float(b, 1.0),
                                                 dst[3]));
                } else {
                        return nir_imm_float(b, 1.0);
                }
        case PIPE_BLENDFACTOR_CONST_COLOR:
                return nir_load_system_value(b,
                                             nir_intrinsic_load_blend_const_color_r_float +
                                             channel,
                                             0);
        case PIPE_BLENDFACTOR_CONST_ALPHA:
                return nir_load_blend_const_color_a_float(b);
#endif
        case PIPE_BLENDFACTOR_ZERO:
                return nir_imm_float(b, 0.0);
#if 0
        case PIPE_BLENDFACTOR_INV_SRC_COLOR:
                return nir_fsub(b, nir_imm_float(b, 1.0), src[channel]);
        case PIPE_BLENDFACTOR_INV_SRC_ALPHA:
                return nir_fsub(b, nir_imm_float(b, 1.0), src[3]);
        case PIPE_BLENDFACTOR_INV_DST_ALPHA:
                return nir_fsub(b, nir_imm_float(b, 1.0), dst[3]);
        case PIPE_BLENDFACTOR_INV_DST_COLOR:
                return nir_fsub(b, nir_imm_float(b, 1.0), dst[channel]);
        case PIPE_BLENDFACTOR_INV_CONST_COLOR:
                return nir_fsub(b, nir_imm_float(b, 1.0),
                                nir_load_system_value(b,
                                                      nir_intrinsic_load_blend_const_color_r_float +
                                                      channel,
                                                      0));
        case PIPE_BLENDFACTOR_INV_CONST_ALPHA:
                return nir_fsub(b, nir_imm_float(b, 1.0),
                                nir_load_blend_const_color_a_float(b));

#endif
        default:
        case PIPE_BLENDFACTOR_SRC1_COLOR:
        case PIPE_BLENDFACTOR_SRC1_ALPHA:
        case PIPE_BLENDFACTOR_INV_SRC1_COLOR:
        case PIPE_BLENDFACTOR_INV_SRC1_ALPHA:
                /* Unsupported. */
                fprintf(stderr, "Unknown blend factor %d\n", factor);
                return nir_imm_float(b, 1.0);
        }
}

nir_ssa_def *
nir_blend_func_f(nir_builder *b, nir_ssa_def *src, nir_ssa_def *dst,
                 unsigned func)
{
        switch (func) {
        case PIPE_BLEND_ADD:
                return nir_fadd(b, src, dst);
        case PIPE_BLEND_SUBTRACT:
                return nir_fsub(b, src, dst);
        case PIPE_BLEND_REVERSE_SUBTRACT:
                return nir_fsub(b, dst, src);
        case PIPE_BLEND_MIN:
                return nir_fmin(b, src, dst);
        case PIPE_BLEND_MAX:
                return nir_fmax(b, src, dst);

        default:
                /* Unsupported. */
                fprintf(stderr, "Unknown blend func %d\n", func);
                return src;

        }
}

/* Blend a single "unit", consisting of a function and factor pair. Either RGB
 * or A */

static nir_ssa_def *
nir_blend_unit_f(nir_builder *b, nir_ssa_def *src, nir_ssa_def *dst, unsigned func, unsigned src_factor, unsigned dst_factor, unsigned mask)
{
        /* Compile factors */
        nir_ssa_def *compiled_src_factor = nir_blend_channel_f(b, src, dst, src_factor);
        nir_ssa_def *compiled_dst_factor = nir_blend_channel_f(b, src, dst, src_factor);

        /* Apply factor to source and destination */
        nir_ssa_def *scaled_src = nir_fmul(b, nir_channels(b, src, mask), compiled_src_factor);
        nir_ssa_def *scaled_dst = nir_fmul(b, nir_channels(b, dst, mask), compiled_dst_factor);

        /* Blend together */
        return nir_blend_func_f(b, scaled_src, scaled_dst, func);
}

/* Implement floating point blending */

nir_ssa_def *
nir_blend_f(nir_builder *b, const struct pipe_rt_blend_state *blend, nir_ssa_def *src, nir_ssa_def *dst)
{
        /* TODO: Alpha */
        nir_ssa_def *blended_rgb = nir_blend_unit_f(b, src, dst, blend->rgb_func, blend->rgb_src_factor, blend->rgb_dst_factor, 0x7);
        nir_ssa_def *blended_a = nir_blend_unit_f(b, src, dst, blend->alpha_func, blend->alpha_src_factor, blend->alpha_dst_factor, 0x8);

        /* Combine */
        return nir_vec4(b,
                        nir_channel(b, blended_rgb, 0),
                        nir_channel(b, blended_rgb, 1),
                        nir_channel(b, blended_rgb, 2),
                        nir_channel(b, blended_a, 0));
}

#if 0
static void
nir_do_blending_f(struct vc4_compile *c, nir_builder *b, nir_ssa_def **result,
                  nir_ssa_def **src_color, nir_ssa_def **dst_color)
{
        struct pipe_rt_blend_state *blend = &c->fs_key->blend;

        if (!blend->blend_enable) {
                for (int i = 0; i < 4; i++)
                        result[i] = src_color[i];
                return;
        }

        /* Clamp the src color to [0, 1].  Dest is already clamped. */
        for (int i = 0; i < 4; i++)
                src_color[i] = nir_fsat(b, src_color[i]);

        nir_ssa_def *src_blend[4], *dst_blend[4];
        for (int i = 0; i < 4; i++) {
                int src_factor = ((i != 3) ? blend->rgb_src_factor :
                                  blend->alpha_src_factor);
                int dst_factor = ((i != 3) ? blend->rgb_dst_factor :
                                  blend->alpha_dst_factor);
                src_blend[i] = nir_fmul(b, src_color[i],
                                        vc4_blend_channel_f(b,
                                                            src_color, dst_color,
                                                            src_factor, i));
                dst_blend[i] = nir_fmul(b, dst_color[i],
                                        vc4_blend_channel_f(b,
                                                            src_color, dst_color,
                                                            dst_factor, i));
        }

        for (int i = 0; i < 4; i++) {
                result[i] = vc4_blend_func_f(b, src_blend[i], dst_blend[i],
                                             ((i != 3) ? blend->rgb_func :
                                              blend->alpha_func));
        }
}
#endif
