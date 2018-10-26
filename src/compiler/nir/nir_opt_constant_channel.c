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


