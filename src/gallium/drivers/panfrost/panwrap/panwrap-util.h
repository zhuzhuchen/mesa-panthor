/*
 * © Copyright 2017-2018 The Panfrost Community
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained
 * from Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 */

/**
 * Miscellanious utilities
 */

#ifndef __PANLOADER_UTIL_H__
#define __PANLOADER_UTIL_H__

#include <mali-int.h>
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
