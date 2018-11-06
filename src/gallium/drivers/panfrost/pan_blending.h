/*
 * © Copyright 2018 Alyssa Rosenzweig
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

#ifndef __PAN_BLENDING_H__
#define __PAN_BLENDING_H__

#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include <mali-job.h>

bool panfrost_make_fixed_blend_mode(const struct pipe_rt_blend_state *blend, struct mali_blend_equation *out, unsigned colormask, const struct pipe_blend_color *blend_color);

#endif
