/*
 * Copyright © 2018 Alyssa Rosenzweig
 * Copyright © 2014 Intel Corporation
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

/* Returns the identity magic number for a given op, where performing the
 * operation is a no-op. For instance, for addition this is zero, per the
 * additive identity.
 */

static int
get_operation_identity(nir_op op, bool *incomplete)
{
   switch (op) {
      case nir_op_fadd:
         return 0;

      case nir_op_fmul:
         return 1;

      default:
         *incomplete = true;
         return -1;
   }
}

static bool
nir_opt_constant_channel_block(nir_builder *b, nir_block *block, nir_function_impl *impl)
{
   printf("Nopping\n");
   nir_foreach_instr_safe(instr, block) {
      if (instr->type == nir_instr_type_alu) {
         nir_alu_instr *alu = nir_instr_as_alu(instr);

         /* Check if it's a binary ALU instruction we know. If we know it, find the identity */

         bool unknown_op = false;
         int identity = get_operation_identity(alu->op, &unknown_op);
         if (unknown_op) continue;

         /* We need one of the operands to be a constant; otherwise, there's
          * nothing to do. Search for the constan. Practically, there is only
          * one due to constant folding. */

         assert(nir_op_infos[alu->op].num_inputs == 2);

         nir_load_const_instr* load_const  = NULL;

         unsigned active_writemask = 0, components = 0;

         for (unsigned i = 0; i < 2; i++) {
            if (!alu->src[i].src.is_ssa)
               continue;

            nir_instr *src_instr = alu->src[i].src.ssa->parent_instr;

            if (src_instr->type != nir_instr_type_load_const)
               continue;

            assert(!alu->src[i].abs && !alu->src[i].negate);

            load_const = nir_instr_as_load_const(src_instr);

            /* TODO: Handle non-SSA, non-fp32 cases */

            if (load_const->def.bit_size != 32)
               continue;

            if (!alu->dest.dest.is_ssa) continue;

            printf("Found a fp32 constant <");

            /* We have the constant: scan it for redundant (identity) components in order to construct a writemask */

            components = nir_ssa_alu_instr_src_components(alu, i);

            for (unsigned j = 0; j < components; ++j) {
               float v = load_const->value.f32[alu->src[i].swizzle[j]];
               printf("%f, ",  v);

               if (v != identity)
                  active_writemask |= (1 << j);
            }
            printf(">\n");

            break;
         }

         if (!load_const)
            continue;

         /* If all components are used, there's nothing to do */

         if (active_writemask == ((1 << components) - 1))
            continue;

         /* We need to mask out some components, which conflicts with SSA.
          * Switch to a register destination instead. */

         if (alu->dest.dest.is_ssa) {
            nir_register *reg = nir_local_reg_create(impl);
            reg->num_components = alu->dest.dest.ssa.num_components;
            reg->bit_size = alu->dest.dest.ssa.bit_size;

            nir_ssa_def_rewrite_uses(&alu->dest.dest.ssa, nir_src_for_reg(reg));

            nir_instr_rewrite_dest(&alu->instr, &alu->dest.dest,
                                   nir_dest_for_reg(reg));
         }

         printf("Writemask %X\n", active_writemask);
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
            progress |= nir_opt_constant_channel_block(&builder, block, function->impl);
         }
      }
   }
   return progress;
}
