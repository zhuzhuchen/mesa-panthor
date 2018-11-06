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

#include <stdio.h>
#include "pan_blend_shaders.h"
#include "midgard/midgard_compile.h"

/*
 * Implements the command stream portion of programmatic blend shaders.
 *
 * On Midgard, common blending operations are accelerated by the fixed-function
 * blending pipeline. Panfrost supports this fast path via the code in
 * pan_blending.c. Nevertheless, uncommon blend modes (including some seemingly
 * simple modes present in ES2) require "blend shaders", a special internal
 * shader type used for programmable blending.
 *
 * Blend shaders operate during the normal blending time, but they bypass the
 * fixed-function blending pipeline and instead go straight to the Midgard
 * shader cores. The shaders themselves are essentially just fragment shaders,
 * making heavy use of uint8 arithmetic to manipulate RGB values for the
 * framebuffer.
 *
 * As is typical with Midgard, shader binaries must be accompanied by
 * information about the first tag (ORed with the bottom nibble of address,
 * like usual) and work registers. Work register count is specified in the
 * blend descriptor, as well as in the coresponding fragment shader's work
 * count. This suggests that blend shader invocation is tied to fragment shader
 * execution.
 * 
 * ---
 *
 * As for blend shaders, they use the standard ISA. 
 *
 * The source pixel colour, including alpha, is preloaded into r0 as a vec4 of
 * float32.
 *
 * The destination pixel colour must be loaded explicitly via load/store ops.
 * TODO: Investigate.
 *
 * They use fragment shader writeout; however, instead of writing a vec4 of
 * float32 for RGBA encoding, we writeout a vec4 of uint8, using 8-bit imov
 * instead of 32-bit fmov. The net result is that r0 encodes a single uint32
 * containing all four channels of the color.  Accordingly, the blend shader
 * epilogue has to scale all four channels by 255 and then type convert to a
 * uint8.
 *
 * ---
 *
 * Blend shaders hardcode constants. Naively, this requires recompilation each
 * time the blend color changes, which is a performance risk. Accordingly, we
 * 'cheat' a bit: instead of loading the constant, we compile a shader with a
 * dummy constant, exporting the offset to the immediate in the shader binary,
 * storing this generic binary and metadata in the CSO itself at CSO create
 * time.
 *
 * We then hot patch in the color into this shader at attachment / color change
 * time, allowing for CSO create to be the only expensive operation
 * (compilation).
 */ 

void
panfrost_make_blend_shader(struct panfrost_context *ctx, struct panfrost_blend_state *cso, const struct pipe_blend_color *blend_color)
{
	//const struct pipe_rt_blend_state *blend = &cso->base.rt[0];
	mali_ptr *out = &cso->blend_shader;

	/* Upload the shader */
	midgard_program program = {
		.work_register_count = 3,
		.first_tag = 9,
		.blend_patch_offset = 16
		//.blend_patch_offset = -1,
	};

	char dst[4096];

	FILE *fp = fopen("/home/alyssa/panfrost/midgard/blend.bin", "rb");
	fread(dst, 1, 2816, fp);
	fclose(fp);
	int size = 2816;

	/* Hot patch in constant color */

	if (program.blend_patch_offset >= 0) {
		float *hot_color = (float *) (dst + program.blend_patch_offset);

		for (int c = 0; c < 4; ++c)
			hot_color[c] = blend_color->color[c];
	}
	
	*out = panfrost_upload(&ctx->shaders, dst, size, true) | program.first_tag;

	/* We need to switch to shader mode */
	cso->has_blend_shader = true;
	cso->blend_work_count = program.work_register_count;
}
