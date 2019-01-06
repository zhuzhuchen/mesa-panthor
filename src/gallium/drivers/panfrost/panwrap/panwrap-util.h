/*
 * © Copyright 2017-2018 The Panfrost Community
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

/**
 * Miscellanious utilities
 */

#ifndef __PANLOADER_UTIL_H__
#define __PANLOADER_UTIL_H__

#include <panfrost-misc.h>
#include "util/macros.h"

#define __PASTE_TOKENS(a, b) a ## b
/*
 * PASTE_TOKENS(a, b):
 *
 * Expands a and b, then concatenates the resulting tokens
 */
#define PASTE_TOKENS(a, b) __PASTE_TOKENS(a, b)

#define PANLOADER_CONSTRUCTOR \
       static void __attribute__((constructor)) PASTE_TOKENS(__panloader_ctor_l, __LINE__)()

#define PANLOADER_DESTRUCTOR \
       static void __attribute__((destructor)) PASTE_TOKENS(__panloader_dtor_l, __LINE__)()

/* Semantic logging type.
 *
 * Raw: for raw messages to be printed as is.
 * Message: for helpful information to be commented out in replays.
 * Property: for properties of a struct
 *
 * Use one of panwrap_log, panwrap_msg, or panwrap_prop as syntax sugar.
 */

enum panwrap_log_type {
        PANWRAP_RAW,
        PANWRAP_MESSAGE,
        PANWRAP_PROPERTY
};

#define panwrap_log(...)  panwrap_log_typed(PANWRAP_RAW,      __VA_ARGS__)
#define panwrap_msg(...)  panwrap_log_typed(PANWRAP_MESSAGE,  __VA_ARGS__)
#define panwrap_prop(...) panwrap_log_typed(PANWRAP_PROPERTY, __VA_ARGS__)

#endif /* __PANLOADER_UTIL_H__ */
