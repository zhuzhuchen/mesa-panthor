/*
 * © Copyright 2018 The Panfrost Community
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
#include <stdlib.h>
#include <string.h>
#include "pan_nondrm.h"
#include "pan_context.h"

#include "compiler/nir/nir.h"
#include "nir/tgsi_to_nir.h"
#include "midgard/midgard_compile.h"
#include "util/u_dynarray.h"

#include "tgsi/tgsi_dump.h"

void
panfrost_shader_compile(struct panfrost_context *ctx, struct mali_shader_meta *meta, const char *src, int type, struct panfrost_shader_state *state)
{
	uint8_t* dst;

	nir_shader *s;

	struct pipe_shader_state* cso = &state->base;
	
	if (cso->type == PIPE_SHADER_IR_NIR) {
		s = cso->ir.nir;
	} else {
		assert (cso->type == PIPE_SHADER_IR_TGSI);
		//tgsi_dump(cso->tokens, 0);
		s = tgsi_to_nir(cso->tokens, &midgard_nir_options);
	}

	s->info.stage = type == JOB_TYPE_VERTEX ? MESA_SHADER_VERTEX : MESA_SHADER_FRAGMENT;

	/* Call out to Midgard compiler given the above NIR */

	midgard_program program;
	midgard_compile_shader_nir(s, &program, false);

	/* Prepare the compiled binary for upload */
	int size = program.compiled.size;
	dst = program.compiled.data;

	/* Inject an external shader */
#if 0
	char buf[4096];
	if (type != JOB_TYPE_VERTEX) {
		FILE *fp = fopen("/home/alyssa/panfrost/midgard/good.bin", "rb");
		fread(buf, 1, 2816, fp);
		fclose(fp);
		dst= buf;
		size = 2816;
	}
#endif

	/* Upload the shader. The lookahead tag is ORed on as a tagged pointer.
	 * I bet someone just thought that would be a cute pun. At least,
	 * that's how I'd do it. */

	meta->shader = panfrost_upload(&ctx->shaders, dst, size, true) | program.first_tag;

	util_dynarray_fini(&program.compiled);

	meta->midgard1.uniform_count = MIN2(program.uniform_count, program.uniform_cutoff);
	meta->attribute_count = program.attribute_count;
	meta->varying_count = program.varying_count + 2;
	meta->midgard1.work_count = program.work_register_count;

	state->can_discard = program.can_discard;

	/* Separate as primary uniform count is truncated */
	state->uniform_count = program.uniform_count;

	/* gl_Position eats up an extra spot */
	if (type == JOB_TYPE_VERTEX)
		meta->varying_count += 1;

	/* gl_FragCoord does -not- eat an extra spot; it will be included in our count if we need it */


    meta->midgard1.unknown2 = 8; /* XXX */

    /* Varyings are known only through the shader. We choose to upload this
     * information with the vertex shader, though the choice is perhaps
     * arbitrary */

    if (type == JOB_TYPE_VERTEX) {
	    struct panfrost_varyings *varyings = &state->varyings;

	    /* Measured in vec4 words. Don't include gl_Position */
	    int varying_count = program.varying_count;

	    /* Setup two buffers, one for position, the other for normal
	     * varyings, as seen in traces. TODO: Are there other
	     * configurations we might use? */

	    varyings->varying_buffer_count = 2;

	    /* mediump vec4s sequentially */
	    varyings->varyings_stride[0] = (2 * sizeof(float)) * varying_count;

	    /* highp gl_Position */
	    varyings->varyings_stride[1] = 4 * sizeof(float);

	    /* Setup gl_Position and its weirdo analogue */

	    struct mali_attr_meta position_meta = {
		    .index = 1,
		    .type = 6, /* gl_Position */
		    .nr_components = MALI_POSITIVE(4),
		    .is_int_signed = 1,
		    .unknown1 = 0x1a22
	    };

	    struct mali_attr_meta position_meta_prime = {
		    .index = 0,
		    .type = 7, /* float */
		    .nr_components = MALI_POSITIVE(4),
		    .is_int_signed = 1,
		    .unknown1 = 0x2490
	    };

	    varyings->vertex_only_varyings[0] = position_meta;
	    varyings->vertex_only_varyings[1] = position_meta_prime;

	    /* Setup actual varyings. XXX: Don't assume vec4 */

	    for (int i = 0; i < varying_count; ++i) {
		    struct mali_attr_meta vec4_varying_meta = {
			    .index = 0,
			    .type = 7, /* float */
			    .nr_components = MALI_POSITIVE(4),
			    .not_normalised = 1,
			    .unknown1 = 0x1a22,

			    /* mediump => half-floats */
			    .is_int_signed = 1,

			    /* Set offset to keep everything back-to-back in
			     * the same buffer */
			    .src_offset = 8 * i,
#ifdef T6XX
			    .unknown2 = 1,
#endif
		    };

		    varyings->varyings[i] = vec4_varying_meta;
	    }

	    /* In this context, position_meta represents the implicit
	     * gl_FragCoord varying */

	    varyings->fragment_only_varyings[0] = position_meta;
	    varyings->fragment_only_varying_count = 1;

	    varyings->varying_count = varying_count - 1;
    }
}
