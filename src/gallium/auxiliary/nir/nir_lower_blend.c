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

#include "nir_lower_blend.h"

/* Implements fixed-function blending in software. The standard entrypoint for
 * floating point blending is nir_blending_f, called with the Gallium blend
 * state and nir_ssa_def's for the various parameters used in blending. These
 * routines may be used to construct dedicated blend shaders or appended to
 * fragment shaders; accordingly, they do not perform I/O to maximize
 * flexibility.
 *
 * Inputs are assumed to be clamped to [0, 1]. fsat instructions must be added
 * by the caller if clamping is not otherwise performed.
 *
 * TODO: sRGB, logic ops, integers, dual-source blending, advanced blending
 */

/* src and dst are vec4 */

static nir_ssa_def *
nir_blend_channel_f(nir_builder *b,
                    nir_ssa_def **src,
                    nir_ssa_def **dst,
                    nir_ssa_def *constant,
                    unsigned factor,
                    int channel)
{
   switch(factor) {
   case PIPE_BLENDFACTOR_ONE:
      return nir_imm_float(b, 1.0);
   case PIPE_BLENDFACTOR_SRC_COLOR:
      return src[channel];
   case PIPE_BLENDFACTOR_SRC_ALPHA:
      return src[3];
   case PIPE_BLENDFACTOR_DST_ALPHA:
      return dst[3];
   case PIPE_BLENDFACTOR_DST_COLOR:
      return dst[channel];
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
      return nir_channel(b, constant, channel);
   case PIPE_BLENDFACTOR_CONST_ALPHA:
      return nir_channel(b, constant, 3);
   case PIPE_BLENDFACTOR_ZERO:
      return nir_imm_float(b, 0.0);
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
                      nir_channel(b, constant, channel));
   case PIPE_BLENDFACTOR_INV_CONST_ALPHA:
      return nir_fsub(b, nir_imm_float(b, 1.0),
                      nir_channel(b, constant, 3));


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


static nir_ssa_def *
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

static void
nir_per_channel_blending_f(const struct pipe_rt_blend_state *blend, nir_builder *b, nir_ssa_def **result,
                           nir_ssa_def **src_color, nir_ssa_def **dst_color, nir_ssa_def *con)
{
   if (!blend->blend_enable) {
      for (int i = 0; i < 4; i++)
         result[i] = src_color[i];
      return;
   }

   nir_ssa_def *src_blend[4], *dst_blend[4];
   for (int i = 0; i < 4; i++) {
      int src_factor = ((i != 3) ? blend->rgb_src_factor :
                        blend->alpha_src_factor);
      int dst_factor = ((i != 3) ? blend->rgb_dst_factor :
                        blend->alpha_dst_factor);
      src_blend[i] = nir_fmul(b, src_color[i],
                              nir_blend_channel_f(b,
                                    src_color, dst_color,
                                    con, src_factor, i));
      dst_blend[i] = nir_fmul(b, dst_color[i],
                              nir_blend_channel_f(b,
                                    src_color, dst_color,
                                    con, dst_factor, i));
   }

   for (int i = 0; i < 4; i++) {
      result[i] = nir_blend_func_f(b, src_blend[i], dst_blend[i],
                                   ((i != 3) ? blend->rgb_func :
                                    blend->alpha_func));
   }
}

/* Arguments are vec4s */

nir_ssa_def *
nir_blending_f(const struct pipe_rt_blend_state *blend, nir_builder *b,
               nir_ssa_def *src_color, nir_ssa_def *dst_color,
               nir_ssa_def *constant)
{
   nir_ssa_def* result[4];

   nir_ssa_def* src_components[4] = {
      nir_channel(b, src_color, 0),
      nir_channel(b, src_color, 1),
      nir_channel(b, src_color, 2),
      nir_channel(b, src_color, 3)
   };

   nir_ssa_def* dst_components[4] = {
      nir_channel(b, dst_color, 0),
      nir_channel(b, dst_color, 1),
      nir_channel(b, dst_color, 2),
      nir_channel(b, dst_color, 3)
   };

   nir_per_channel_blending_f(blend, b, result, src_components, dst_components, constant);

   return nir_vec(b, result, 4);
}
