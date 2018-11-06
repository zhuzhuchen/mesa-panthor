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

#ifndef __PAN_BLEND_SHADERS_H__
#define __PAN_BLEND_SHADERS_H__

#include "pipe/p_state.h"
#include "pipe/p_defines.h"
#include <mali-job.h>
#include "pan_context.h"

void
panfrost_make_blend_shader(struct panfrost_context *ctx, struct panfrost_blend_state *cso, const struct pipe_blend_color *blend_color);

#endif
