/*
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
 *
 */

#include "nir.h"
#include "nir_builder.h"

/** @file nir_opt_constant_channel.c
 *
 * Optimizes simple arithmetic operations involving vector constants with
 * redundant channels. For instance, (v + vec2(f, 0)) will be optimized to
 * vec2(v.x, v.y + f). In an ideal case with a single "active" component, this
 * optimizes the vector operation into an equivalent scalar operation, aiding
 * scheduling on vector backends.
 */

static bool
nir_opt_constant_channel_block(nir_builder *b, nir_block *block)
{
   printf("Nopping\n");
   nir_foreach_instr_safe(instr, block) {
      if (instr->type == nir_instr_type_alu) {
         nir_alu_instr *alu = nir_instr_as_alu(instr);

         /* Check if it's a binary ALU instruction we know */
         if (alu->op != nir_op_fadd) continue;

         /* Identity number for the corresponding ALU, for which applying the
          * operation is a no-op and can be eliminated. */

         int identity = 0;
         printf("Found an add\n");

         /* We need one of the operands to be a constant; otherwise, there's
          * nothing to do. Search for the constan. Practically, there is only
          * one due to constant folding. */

         assert(nir_op_infos[alu->op].num_inputs == 2);

         nir_load_const_instr* load_const  = NULL;

         for (unsigned i = 0; i < 2; i++) {
            if (!alu->src[i].src.is_ssa)
               continue;

            nir_instr *src_instr = alu->src[i].src.ssa->parent_instr;

            if (src_instr->type != nir_instr_type_load_const)
               continue;

            assert(!alu->src[i].abs && !alu->src[i].negate);

            load_const = nir_instr_as_load_const(src_instr);

            /* TODO: Handle non-fp32 cases */
            if (load_const->def.bit_size != 32)
               continue;

            printf("Found a fp32 constant <");

            /* We have the constant: scan it for redundant (identity) components */

            bool active_component[4] = { false };
            int components = nir_ssa_alu_instr_src_components(alu, i);

            for (unsigned j = 0; j < components; ++j) {
               float v = load_const->value.f32[alu->src[i].swizzle[j]];
               printf("%f, ",  v);

               active_component[j] = (v != identity);
            }
            printf(">\n");

            /* Dump activity */
            for (unsigned j = 0; j < components; ++j) {
               printf("%d, ", active_component[j]);
            }
            printf("\n");

            break;
         }

         if (!load_const)
            continue;

      }
   }
   return false;
}

bool
nir_opt_constant_channel(nir_shader *shader)
{
   bool progress = false;

   nir_builder builder;

   nir_foreach_function(function, shader) {
      if (function->impl) {
         nir_builder_init(&builder, function->impl);
         nir_foreach_block_safe(block, function->impl) {
            progress |= nir_opt_constant_channel_block(&builder, block);
         }
      }
   }
   return progress;
}
