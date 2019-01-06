/*
 * Copyright (C) 2018 Ryan Houdek <Sonicadvance1@gmail.com>
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
 */

#ifndef __bifrost_compile_h__
#define __bifrost_compile_h__

#include "compiler/nir/nir.h"

struct bifrost_program {
};

int
bifrost_compile_shader_nir(nir_shader *nir, struct bifrost_program *program);

static const nir_shader_compiler_options bifrost_nir_options = {
        .lower_ffma = true,
        .lower_sub = true,
        .lower_fpow = true,
        .lower_scmp = true,
        .lower_flrp32 = true,
        .lower_flrp64 = true,
        .lower_ffract = true,
        .lower_fmod32 = true,
        .lower_fmod64 = true,
        .lower_fdiv = true,
        .lower_idiv = true,

        .vertex_id_zero_based = true,
        .lower_extract_byte = true,
        .lower_extract_word = true,

        .native_integers = true
};

#endif
