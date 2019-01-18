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
 */

#include <mali-kbase-ioctl.h>
#include <panfrost-job.h>
#include <stdio.h>
#include <memory.h>
#include "panwrap.h"

#include "../pan_pretty_print.h"

#define MEMORY_PROP(obj, p) {\
	char *a = pointer_as_memory_reference(obj->p); \
	panwrap_prop("%s = %s", #p, a); \
	free(a); \
}

#define MEMORY_COMMENT(obj, p) {\
	char *a = pointer_as_memory_reference(obj->p); \
	panwrap_msg("%s = %s\n", #p, a); \
	free(a); \
}

#define DYN_MEMORY_PROP(obj, no, p) { \
	if (obj->p) \
		panwrap_prop("%s = %s_%d_p", #p, #p, no); \
}

#define FLAG_INFO(flag) { MALI_GL_##flag, "MALI_GL_" #flag }
static const struct panwrap_flag_info gl_enable_flag_info[] = {
        FLAG_INFO(CULL_FACE_FRONT),
        FLAG_INFO(CULL_FACE_BACK),
        FLAG_INFO(OCCLUSION_BOOLEAN),
        {}
};
#undef FLAG_INFO

#define FLAG_INFO(flag) { MALI_CLEAR_##flag, "MALI_CLEAR_" #flag }
static const struct panwrap_flag_info clear_flag_info[] = {
        FLAG_INFO(FAST),
        FLAG_INFO(SLOW),
        FLAG_INFO(SLOW_STENCIL),
        {}
};
#undef FLAG_INFO

#define FLAG_INFO(flag) { MALI_MASK_##flag, "MALI_MASK_" #flag }
static const struct panwrap_flag_info mask_flag_info[] = {
        FLAG_INFO(R),
        FLAG_INFO(G),
        FLAG_INFO(B),
        FLAG_INFO(A),
        {}
};
#undef FLAG_INFO

#define FLAG_INFO(flag) { MALI_##flag, "MALI_" #flag }
static const struct panwrap_flag_info u3_flag_info[] = {
        FLAG_INFO(HAS_MSAA),
        FLAG_INFO(CAN_DISCARD),
        FLAG_INFO(HAS_BLEND_SHADER),
        FLAG_INFO(DEPTH_TEST),
        {}
};

static const struct panwrap_flag_info u4_flag_info[] = {
        FLAG_INFO(NO_MSAA),
        FLAG_INFO(NO_DITHER),
        FLAG_INFO(DEPTH_RANGE_A),
        FLAG_INFO(DEPTH_RANGE_B),
        FLAG_INFO(STENCIL_TEST),
        FLAG_INFO(SAMPLE_ALPHA_TO_COVERAGE_NO_BLEND_SHADER),
        {}
};
#undef FLAG_INFO

#define FLAG_INFO(flag) { MALI_FRAMEBUFFER_##flag, "MALI_FRAMEBUFFER_" #flag }
static const struct panwrap_flag_info fb_fmt_flag_info[] = {
        FLAG_INFO(MSAA_A),
        FLAG_INFO(MSAA_B),
        FLAG_INFO(MSAA_8),
        {}
};
#undef FLAG_INFO

extern char *replace_fragment;
extern char *replace_vertex;

static char *
panwrap_job_type_name(enum mali_job_type type)
{
#define DEFINE_CASE(name) case JOB_TYPE_ ## name: return "JOB_TYPE_" #name

        switch (type) {
                DEFINE_CASE(NULL);
                DEFINE_CASE(SET_VALUE);
                DEFINE_CASE(CACHE_FLUSH);
                DEFINE_CASE(COMPUTE);
                DEFINE_CASE(VERTEX);
                DEFINE_CASE(TILER);
                DEFINE_CASE(FUSED);
                DEFINE_CASE(FRAGMENT);

        case JOB_NOT_STARTED:
                return "NOT_STARTED";

        default:
                panwrap_log("Warning! Unknown job type %x\n", type);
                return "!?!?!?";
        }

#undef DEFINE_CASE
}

static char *
panwrap_gl_mode_name(enum mali_gl_mode mode)
{
#define DEFINE_CASE(name) case MALI_ ## name: return "MALI_" #name

        switch (mode) {
                DEFINE_CASE(GL_NONE);
                DEFINE_CASE(GL_POINTS);
                DEFINE_CASE(GL_LINES);
                DEFINE_CASE(GL_TRIANGLES);
                DEFINE_CASE(GL_TRIANGLE_STRIP);
                DEFINE_CASE(GL_TRIANGLE_FAN);
                DEFINE_CASE(GL_LINE_STRIP);
                DEFINE_CASE(GL_LINE_LOOP);

        default:
                return "MALI_GL_TRIANGLES /* XXX: Unknown GL mode, check dump */";
        }

#undef DEFINE_CASE
}

#define DEFINE_CASE(name) case MALI_FUNC_ ## name: return "MALI_FUNC_" #name
static char *
panwrap_func_name(enum mali_func mode)
{
        switch (mode) {
                DEFINE_CASE(NEVER);
                DEFINE_CASE(LESS);
                DEFINE_CASE(EQUAL);
                DEFINE_CASE(LEQUAL);
                DEFINE_CASE(GREATER);
                DEFINE_CASE(NOTEQUAL);
                DEFINE_CASE(GEQUAL);
                DEFINE_CASE(ALWAYS);

        default:
                return "MALI_FUNC_NEVER /* XXX: Unknown function, check dump */";
        }
}
#undef DEFINE_CASE

/* Why is this duplicated? Who knows... */
#define DEFINE_CASE(name) case MALI_ALT_FUNC_ ## name: return "MALI_ALT_FUNC_" #name
static char *
panwrap_alt_func_name(enum mali_alt_func mode)
{
        switch (mode) {
                DEFINE_CASE(NEVER);
                DEFINE_CASE(LESS);
                DEFINE_CASE(EQUAL);
                DEFINE_CASE(LEQUAL);
                DEFINE_CASE(GREATER);
                DEFINE_CASE(NOTEQUAL);
                DEFINE_CASE(GEQUAL);
                DEFINE_CASE(ALWAYS);

        default:
                return "MALI_FUNC_NEVER /* XXX: Unknown function, check dump */";
        }
}
#undef DEFINE_CASE



#define DEFINE_CASE(name) case MALI_STENCIL_ ## name: return "MALI_STENCIL_" #name
static char *
panwrap_stencil_op_name(enum mali_stencil_op op)
{
        switch (op) {
                DEFINE_CASE(KEEP);
                DEFINE_CASE(REPLACE);
                DEFINE_CASE(ZERO);
                DEFINE_CASE(INVERT);
                DEFINE_CASE(INCR_WRAP);
                DEFINE_CASE(DECR_WRAP);
                DEFINE_CASE(INCR);
                DEFINE_CASE(DECR);

        default:
                return "MALI_STENCIL_KEEP /* XXX: Unknown stencil op, check dump */";
        }
}

#undef DEFINE_CASE

#define DEFINE_CASE(name) case MALI_ATTR_ ## name: return "MALI_ATTR_" #name
static char *panwrap_attr_mode_name(enum mali_attr_mode mode)
{
	switch(mode) {
	DEFINE_CASE(UNUSED);
	DEFINE_CASE(LINEAR);
	DEFINE_CASE(POT_DIVIDE);
	DEFINE_CASE(MODULO);
	DEFINE_CASE(NPOT_DIVIDE);
	default: return "MALI_ATTR_UNUSED /* XXX: Unknown stencil op, check dump */";
	}
}

#undef DEFINE_CASE

#define DEFINE_CASE(name) case MALI_CHANNEL_## name: return "MALI_CHANNEL_" #name
static char *
panwrap_channel_name(enum mali_channel channel)
{
        switch (channel) {
                DEFINE_CASE(RED);
                DEFINE_CASE(GREEN);
                DEFINE_CASE(BLUE);
                DEFINE_CASE(ALPHA);
                DEFINE_CASE(ZERO);
                DEFINE_CASE(ONE);
                DEFINE_CASE(RESERVED_0);
                DEFINE_CASE(RESERVED_1);

        default:
                return "MALI_CHANNEL_ZERO /* XXX: Unknown channel, check dump */";
        }
}
#undef DEFINE_CASE

#define DEFINE_CASE(name) case MALI_WRAP_## name: return "MALI_WRAP_" #name
static char *
panwrap_wrap_mode_name(enum mali_wrap_mode op)
{
        switch (op) {
                DEFINE_CASE(REPEAT);
                DEFINE_CASE(CLAMP_TO_EDGE);
                DEFINE_CASE(CLAMP_TO_BORDER);
                DEFINE_CASE(MIRRORED_REPEAT);

        default:
                return "MALI_WRAP_REPEAT /* XXX: Unknown wrap mode, check dump */";
        }
}
#undef DEFINE_CASE

static inline char *
panwrap_decode_fbd_type(enum mali_fbd_type type)
{
        if (type == MALI_SFBD)      return "SFBD";
        else if (type == MALI_MFBD) return "MFBD";
        else return "WTF!?";
}

static bool
panwrap_deduplicate(const struct panwrap_mapped_memory *mem, uint64_t gpu_va, const char *name, int number)
{
        if (mem->touched[(gpu_va - mem->gpu_va) / sizeof(uint32_t)]) {
                /* XXX: Is this correct? */
                panwrap_log("mali_ptr %s_%d_p = %s_%d_p;\n", name, number, name, number - 1);
                return true;
        }

        return false;
}

static void
panwrap_replay_sfbd(uint64_t gpu_va, int job_no)
{
        struct panwrap_mapped_memory *mem = panwrap_find_mapped_gpu_mem_containing(gpu_va);
        const struct mali_single_framebuffer *PANWRAP_PTR_VAR(s, mem, (mali_ptr) gpu_va);

        /* FBDs are frequently duplicated, so watch for this */
        //if (panwrap_deduplicate(mem, gpu_va, "framebuffer", job_no)) return;

        panwrap_log("struct mali_single_framebuffer framebuffer_%d = {\n", job_no);
        panwrap_indent++;

        panwrap_prop("unknown1 = 0x%" PRIx32, s->unknown1);
        panwrap_prop("unknown2 = 0x%" PRIx32, s->unknown2);

        panwrap_log(".format = ");
        panwrap_log_decoded_flags(fb_fmt_flag_info, s->format);
        panwrap_log_cont(",\n");

        panwrap_prop("width = MALI_POSITIVE(%" PRId16 ")", s->width + 1);
        panwrap_prop("height = MALI_POSITIVE(%" PRId16 ")", s->height + 1);

        MEMORY_PROP(s, framebuffer);
        panwrap_prop("stride = %d", s->stride);

        /* Earlier in the actual commandstream -- right before width -- but we
         * delay to flow nicer */

        panwrap_log(".clear_flags = ");
        panwrap_log_decoded_flags(clear_flag_info, s->clear_flags);
        panwrap_log_cont(",\n");

        if (s->depth_buffer | s->depth_buffer_enable) {
                MEMORY_PROP(s, depth_buffer);
                panwrap_prop("depth_buffer_enable = %s", DS_ENABLE(s->depth_buffer_enable));
        }

        if (s->stencil_buffer | s->stencil_buffer_enable) {
                MEMORY_PROP(s, stencil_buffer);
                panwrap_prop("stencil_buffer_enable = %s", DS_ENABLE(s->stencil_buffer_enable));
        }

        if (s->clear_color_1 | s->clear_color_2 | s->clear_color_3 | s->clear_color_4) {
                panwrap_prop("clear_color_1 = 0x%" PRIx32, s->clear_color_1);
                panwrap_prop("clear_color_2 = 0x%" PRIx32, s->clear_color_2);
                panwrap_prop("clear_color_3 = 0x%" PRIx32, s->clear_color_3);
                panwrap_prop("clear_color_4 = 0x%" PRIx32, s->clear_color_4);
        }

        if (s->clear_depth_1 != 0 || s->clear_depth_2 != 0 || s->clear_depth_3 != 0 || s->clear_depth_4 != 0) {
                panwrap_prop("clear_depth_1 = %f", s->clear_depth_1);
                panwrap_prop("clear_depth_2 = %f", s->clear_depth_2);
                panwrap_prop("clear_depth_3 = %f", s->clear_depth_3);
                panwrap_prop("clear_depth_4 = %f", s->clear_depth_4);
        }

        if (s->clear_stencil) {
                panwrap_prop("clear_stencil = 0x%x", s->clear_stencil);
        }

        MEMORY_PROP(s, unknown_address_0);
        MEMORY_PROP(s, unknown_address_1);
        MEMORY_PROP(s, unknown_address_2);

        panwrap_prop("resolution_check = 0x%" PRIx32, s->resolution_check);
        panwrap_prop("tiler_flags = 0x%" PRIx32, s->tiler_flags);

        MEMORY_PROP(s, tiler_heap_free);
        MEMORY_PROP(s, tiler_heap_end);

        panwrap_indent--;
        panwrap_log("};\n");

        panwrap_prop("zero0 = 0x%" PRIx64, s->zero0);
        panwrap_prop("zero1 = 0x%" PRIx64, s->zero1);
        panwrap_prop("zero2 = 0x%" PRIx32, s->zero2);
        panwrap_prop("zero4 = 0x%" PRIx32, s->zero4);

        int zero_sum_pun = 0;
#if 0
        zero_sum_pun += s->zero0;
        zero_sum_pun += s->zero1;
        zero_sum_pun += s->zero2;
        zero_sum_pun += s->zero4;
#endif
        printf(".zero3 = {");

        for (int i = 0; i < sizeof(s->zero3) / sizeof(s->zero3[0]); ++i)
                printf("%X, ", s->zero3[i]);

        printf("},\n");

        printf(".zero6 = {");

        for (int i = 0; i < sizeof(s->zero6) / sizeof(s->zero6[0]); ++i)
                printf("%X, ", s->zero6[i]);

        printf("},\n");

        if (zero_sum_pun)
                panwrap_msg("Zero sum tripped (%d), replay may be wrong\n", zero_sum_pun);

        TOUCH(mem, (mali_ptr) gpu_va, *s, "framebuffer", job_no, true);
}

static void
panwrap_replay_mfbd_bfr(uint64_t gpu_va, int job_no)
{
        struct panwrap_mapped_memory *mem = panwrap_find_mapped_gpu_mem_containing(gpu_va);
        const struct bifrost_framebuffer *PANWRAP_PTR_VAR(fb, mem, (mali_ptr) gpu_va);

        if (fb->sample_locations) {
                /* The blob stores all possible sample locations in a single buffer
                 * allocated on startup, and just switches the pointer when switching
                 * MSAA state. For now, we just put the data into the cmdstream, but we
                 * should do something like what the blob does with a real driver.
                 *
                 * There seem to be 32 slots for sample locations, followed by another
                 * 16. The second 16 is just the center location followed by 15 zeros
                 * in all the cases I've identified (maybe shader vs. depth/color
                 * samples?).
                 */

                struct panwrap_mapped_memory *smem = panwrap_find_mapped_gpu_mem_containing(fb->sample_locations);

                const u16 *PANWRAP_PTR_VAR(samples, smem, fb->sample_locations);

                panwrap_log("uint16_t sample_locations_%d[] = {\n", job_no);
                panwrap_indent++;

                for (int i = 0; i < 32 + 16; i++) {
                        panwrap_log("%d, %d,\n", samples[2 * i], samples[2 * i + 1]);
                }

                panwrap_indent--;
                panwrap_log("};\n");

                TOUCH_LEN(smem, fb->sample_locations, 4 * (32 + 16),
                          "sample_locations", job_no, true);
        }

        panwrap_log("struct bifrost_framebuffer framebuffer_%d = {\n", job_no);
        panwrap_indent++;

        panwrap_prop("unk0 = 0x%x", fb->unk0);

        if (fb->sample_locations)
                panwrap_prop("sample_locations = sample_locations_%d", job_no);

        /* Assume that unknown1 and tiler_meta were emitted in the last job for
         * now */
        /*panwrap_prop("unknown1 = unknown1_%d_p", job_no - 1);
        panwrap_prop("tiler_meta = tiler_meta_%d_p", job_no - 1);*/
        MEMORY_PROP(fb, unknown1);
        MEMORY_PROP(fb, tiler_meta);

        panwrap_prop("width1 = MALI_POSITIVE(%d)", fb->width1 + 1);
        panwrap_prop("height1 = MALI_POSITIVE(%d)", fb->height1 + 1);
        panwrap_prop("width2 = MALI_POSITIVE(%d)", fb->width2 + 1);
        panwrap_prop("height2 = MALI_POSITIVE(%d)", fb->height2 + 1);

        panwrap_prop("unk1 = 0x%x", fb->unk1);
        panwrap_prop("unk2 = 0x%x", fb->unk2);
        panwrap_prop("rt_count_1 = MALI_POSITIVE(%d)", fb->rt_count_1 + 1);
        panwrap_prop("rt_count_2 = %d", fb->rt_count_2);

        panwrap_prop("unk3 = 0x%x", fb->unk3);
        panwrap_prop("clear_stencil = 0x%x", fb->clear_stencil);
        panwrap_prop("clear_depth = %f", fb->clear_depth);

        panwrap_prop("unknown2 = 0x%x", fb->unknown2);
        MEMORY_PROP(fb, scratchpad);
        MEMORY_PROP(fb, tiler_scratch_start);
        MEMORY_PROP(fb, tiler_scratch_middle);
        MEMORY_PROP(fb, tiler_heap_start);
        MEMORY_PROP(fb, tiler_heap_end);

        if (fb->zero3 || fb->zero4 || fb->zero9 || fb->zero10 || fb->zero11 || fb->zero12) {
                panwrap_msg("framebuffer zeros tripped\n");
                panwrap_prop("zero3 = 0x%" PRIx32, fb->zero3);
                panwrap_prop("zero4 = 0x%" PRIx32, fb->zero4);
                panwrap_prop("zero9 = 0x%" PRIx64, fb->zero9);
                panwrap_prop("zero10 = 0x%" PRIx64, fb->zero10);
                panwrap_prop("zero11 = 0x%" PRIx64, fb->zero11);
                panwrap_prop("zero12 = 0x%" PRIx64, fb->zero12);
        }

        panwrap_indent--;
        panwrap_log("};\n");

        TOUCH(mem, (mali_ptr) gpu_va, *fb, "framebuffer", job_no, true);

        gpu_va += sizeof(struct bifrost_framebuffer);

        if (fb->unk3 & MALI_MFBD_EXTRA) {
                mem = panwrap_find_mapped_gpu_mem_containing(gpu_va);
                const struct bifrost_fb_extra *PANWRAP_PTR_VAR(fbx, mem, (mali_ptr) gpu_va);

                panwrap_log("struct bifrost_fb_extra fb_extra_%d = {\n", job_no);
                panwrap_indent++;

                MEMORY_PROP(fbx, checksum);

                if (fbx->checksum_stride)
                        panwrap_prop("checksum_stride = %d", fbx->checksum_stride);

                panwrap_prop("unk = 0x%x", fbx->unk);

                /* TODO figure out if this is actually the right way to
                 * determine whether AFBC is enabled
                 */
                if (fbx->unk & 0x10) {
                        panwrap_log(".ds_afbc = {\n");
                        panwrap_indent++;

                        MEMORY_PROP((&fbx->ds_afbc), depth_stencil_afbc_metadata);
                        panwrap_prop("depth_stencil_afbc_stride = %d",
                                     fbx->ds_afbc.depth_stencil_afbc_stride);
                        MEMORY_PROP((&fbx->ds_afbc), depth_stencil);

                        if (fbx->ds_afbc.zero1 || fbx->ds_afbc.padding) {
                                panwrap_msg("Depth/stencil AFBC zeros tripped\n");
                                panwrap_prop("zero1 = 0x%" PRIx32,
                                             fbx->ds_afbc.zero1);
                                panwrap_prop("padding = 0x%" PRIx64,
                                             fbx->ds_afbc.padding);
                        }

                        panwrap_indent--;
                        panwrap_log("},\n");
                } else {
                        panwrap_log(".ds_linear = {\n");
                        panwrap_indent++;

                        if (fbx->ds_linear.depth) {
                                MEMORY_PROP((&fbx->ds_linear), depth);
                                panwrap_prop("depth_stride = %d",
                                             fbx->ds_linear.depth_stride);
                        }

                        if (fbx->ds_linear.stencil) {
                                MEMORY_PROP((&fbx->ds_linear), stencil);
                                panwrap_prop("stencil_stride = %d",
                                             fbx->ds_linear.stencil_stride);
                        }

                        if (fbx->ds_linear.depth_stride_zero ||
                                        fbx->ds_linear.stencil_stride_zero ||
                                        fbx->ds_linear.zero1 || fbx->ds_linear.zero2) {
                                panwrap_msg("Depth/stencil zeros tripped\n");
                                panwrap_prop("depth_stride_zero = 0x%x",
                                             fbx->ds_linear.depth_stride_zero);
                                panwrap_prop("stencil_stride_zero = 0x%x",
                                             fbx->ds_linear.stencil_stride_zero);
                                panwrap_prop("zero1 = 0x%" PRIx32,
                                             fbx->ds_linear.zero1);
                                panwrap_prop("zero2 = 0x%" PRIx32,
                                             fbx->ds_linear.zero2);
                        }

                        panwrap_indent--;
                        panwrap_log("},\n");
                }

                if (fbx->zero3 || fbx->zero4) {
                        panwrap_msg("fb_extra zeros tripped\n");
                        panwrap_prop("zero3 = 0x%" PRIx64, fbx->zero3);
                        panwrap_prop("zero4 = 0x%" PRIx64, fbx->zero4);
                }

                panwrap_indent--;
                panwrap_log("};\n");

                TOUCH(mem, (mali_ptr) gpu_va, *fbx, "fb_extra", job_no, true);

                gpu_va += sizeof(struct bifrost_fb_extra);
        }


        panwrap_log("struct bifrost_render_target rts_list_%d[] = {\n", job_no);
        panwrap_indent++;

        for (int i = 0; i < MALI_NEGATIVE(fb->rt_count_1); i++) {
                mali_ptr rt_va = gpu_va + i * sizeof(struct bifrost_render_target);
                mem = panwrap_find_mapped_gpu_mem_containing(rt_va);
                const struct bifrost_render_target *PANWRAP_PTR_VAR(rt, mem, (mali_ptr) rt_va);

                panwrap_log("{\n");
                panwrap_indent++;

                panwrap_prop("unk1 = 0x%" PRIx32, rt->unk1);
                panwrap_prop("format = 0x%" PRIx32, rt->format);

                /* TODO: How the actual heck does AFBC enabling work here? */
                if (0) {
#if 0
                        MEMORY_PROP(rt, afbc_metadata);
                        panwrap_prop("afbc_stride = %d", rt->afbc_stride);
                        panwrap_prop("afbc_unk = 0x%" PRIx32, rt->afbc_unk);
#endif
                } else {
                        panwrap_log(".chunknown = {\n");
                        panwrap_indent++;

                        panwrap_prop("unk = 0x%" PRIx64, rt->chunknown.unk);

                        char *a = pointer_as_memory_reference(rt->chunknown.pointer);
                        panwrap_prop("pointer = %s", a);
                        free(a);

                        panwrap_indent--;
                        panwrap_log("},\n");
                }

                MEMORY_PROP(rt, framebuffer);
                panwrap_prop("framebuffer_stride = %d", rt->framebuffer_stride);

                if (rt->clear_color_1 | rt->clear_color_2 | rt->clear_color_3 | rt->clear_color_4) {
                        panwrap_prop("clear_color_1 = 0x%" PRIx32, rt->clear_color_1);
                        panwrap_prop("clear_color_2 = 0x%" PRIx32, rt->clear_color_2);
                        panwrap_prop("clear_color_3 = 0x%" PRIx32, rt->clear_color_3);
                        panwrap_prop("clear_color_4 = 0x%" PRIx32, rt->clear_color_4);
                }

                if (rt->zero1 || rt->zero2 || rt->zero3) {
                        panwrap_msg("render target zeros tripped\n");
                        panwrap_prop("zero1 = 0x%" PRIx64, rt->zero1);
                        panwrap_prop("zero2 = 0x%" PRIx32, rt->zero2);
                        panwrap_prop("zero3 = 0x%" PRIx32, rt->zero3);
                }

                panwrap_indent--;
                panwrap_log("},\n");
        }

        panwrap_indent--;
        panwrap_log("};\n");

        /* XXX: This is wrong but fixes a compiler error in the replay. FIXME */
        panwrap_log("struct bifrost_render_target rts_%d = rts_list_%d[0];\n", job_no, job_no);
        TOUCH_LEN(mem, (mali_ptr) gpu_va, MALI_NEGATIVE(fb->rt_count_1) * sizeof(struct bifrost_render_target), "rts", job_no, true);
}

static void
panwrap_replay_attributes(const struct panwrap_mapped_memory *mem,
                          mali_ptr addr, int job_no, char *suffix,
                          int count, bool varying)
{
        char *prefix = varying ? "varyings" : "attributes";

        /* Varyings in particular get duplicated between parts of the job */
        if (panwrap_deduplicate(mem, addr, prefix, job_no)) return;

        union mali_attr *attr = panwrap_fetch_gpu_mem(mem, addr, sizeof(union mali_attr) * count);

        char base[128];
        snprintf(base, sizeof(base), "%s_data_%d%s", prefix, job_no, suffix);

        for (int i = 0; i < count; ++i) {
		enum mali_attr_mode mode = attr[i].elements & 7;
		if (mode == MALI_ATTR_UNUSED)
			continue;

		mali_ptr raw_elements = attr[i].elements & ~7;

                /* gl_VertexID and gl_InstanceID do not have elements to
                 * decode; we would crash if we tried */

                if (!varying && i < MALI_SPECIAL_ATTRIBUTE_BASE && 0) {
                        /* TODO: Attributes are not necessarily float32 vectors in general;
                         * decoding like this without snarfing types from the shader is unsafe all things considered */

                        panwrap_msg("i: %d\n", i);

                        panwrap_log("float %s_%d[] = {\n", base, i);

#define MIN(a, b) ((a > b) ? b : a)

#if 0
                        struct panwrap_mapped_memory *l_mem = panwrap_find_mapped_gpu_mem_containing(raw_elements);

                        size_t vertex_count = attr[i].size / attr[i].stride;
                        size_t component_count = attr[i].stride / sizeof(float);

                        float *buffer = panwrap_fetch_gpu_mem(l_mem, raw_elements, attr[i].size);
                        panwrap_indent++;

                        for (int row = 0; row < MIN(vertex_count, 16); row++) {
                                panwrap_log_empty();

                                for (int i = 0; i < component_count; i++)
                                        panwrap_log_cont("%ff, ", buffer[i]);

                                panwrap_log_cont("\n");

                                buffer += component_count;
                        }

#endif
                        panwrap_indent--;
                        panwrap_log("};\n");

                        TOUCH_LEN(mem, raw_elements, attr[i].size, base, i, true);
                } else {
                        /* TODO: Allocate space for varyings dynamically? */

                        char *a = pointer_as_memory_reference(raw_elements);
                        panwrap_log("mali_ptr %s_%d_p = %s;\n", base, i, a);
                        free(a);
                }
        }

        panwrap_log("union mali_attr %s_%d[] = {\n", prefix, job_no);
        panwrap_indent++;

        for (int i = 0; i < count; ++i) {
                panwrap_log("{\n");
                panwrap_indent++;


		panwrap_prop("elements = (%s_%d_p) | %s", base, i, panwrap_attr_mode_name(attr[i].elements & 7));
		panwrap_prop("shift = %d", attr[i].shift);
		panwrap_prop("extra_flags = %d", attr[i].extra_flags);
                panwrap_prop("stride = 0x%" PRIx32, attr[i].stride);
                panwrap_prop("size = 0x%" PRIx32, attr[i].size);
                panwrap_indent--;
                panwrap_log("}, \n");

		if ((attr[i].elements & 7) == MALI_ATTR_NPOT_DIVIDE) {
			i++;
			panwrap_log("{\n");
			panwrap_indent++;
			panwrap_prop("unk = 0x%x", attr[i].unk);
			panwrap_prop("magic_divisor = 0x%08x", attr[i].magic_divisor);
			if (attr[i].zero != 0)
				panwrap_prop("zero = 0x%x /* XXX zero tripped */", attr[i].zero);
			panwrap_prop("divisor = %d", attr[i].divisor);
			panwrap_indent--;
			panwrap_log("}, \n");
		}

        }

        panwrap_indent--;
        panwrap_log("};\n");

        TOUCH_LEN(mem, addr, sizeof(*attr) * count, prefix, job_no, true);
}

static mali_ptr
panwrap_replay_shader_address(const char *name, mali_ptr ptr)
{
        /* TODO: Decode flags */
        mali_ptr shader_ptr = ptr & ~15;

        char *a = pointer_as_memory_reference(shader_ptr);
        panwrap_prop("%s = (%s) | %d", name, a, (int) (ptr & 15));
        free(a);

        return shader_ptr;
}

static void
panwrap_replay_stencil(const char *name, const struct mali_stencil_test *stencil)
{
        const char *func = panwrap_func_name(stencil->func);
        const char *sfail = panwrap_stencil_op_name(stencil->sfail);
        const char *dpfail = panwrap_stencil_op_name(stencil->dpfail);
        const char *dppass = panwrap_stencil_op_name(stencil->dppass);

        if (stencil->zero)
                panwrap_msg("Stencil zero tripped: %X\n", stencil->zero);

        panwrap_log(".stencil_%s = {\n", name);
        panwrap_indent++;
        panwrap_prop("ref = %d", stencil->ref);
        panwrap_prop("mask = 0x%02X", stencil->mask);
        panwrap_prop("func = %s", func);
        panwrap_prop("sfail = %s", sfail);
        panwrap_prop("dpfail = %s", dpfail);
        panwrap_prop("dppass = %s", dppass);
        panwrap_indent--;
        panwrap_log("},\n");
}

static void
panwrap_replay_blend_equation(const struct mali_blend_equation *blend, const char *suffix)
{
        if (blend->zero1)
                panwrap_msg("Blend zero tripped: %X\n", blend->zero1);

        panwrap_log(".blend_equation%s = {\n", suffix);
        panwrap_indent++;

        panwrap_prop("rgb_mode = 0x%X", blend->rgb_mode);
        panwrap_prop("alpha_mode = 0x%X", blend->alpha_mode);

        panwrap_log(".color_mask = ");
        panwrap_log_decoded_flags(mask_flag_info, blend->color_mask);
        panwrap_log_cont(",\n");

        panwrap_indent--;
        panwrap_log("},\n");
}

static void
panwrap_replay_swizzle(unsigned swizzle)
{
	panwrap_prop("swizzle = %s | (%s << 3) | (%s << 6) | (%s << 9)",
			panwrap_channel_name((swizzle >> 0) & 0x7),
			panwrap_channel_name((swizzle >> 3) & 0x7),
			panwrap_channel_name((swizzle >> 6) & 0x7),
			panwrap_channel_name((swizzle >> 9) & 0x7));
}

static int
panwrap_replay_attribute_meta(int job_no, int count, const struct mali_vertex_tiler_postfix *v, bool varying, char *suffix)
{
        char base[128];
        char *prefix = varying ? "varying" : "attribute";
	unsigned max_index = 0;
        snprintf(base, sizeof(base), "%s_meta", prefix);

        panwrap_log("struct mali_attr_meta %s_%d%s[] = {\n", base, job_no, suffix);
        panwrap_indent++;

        struct mali_attr_meta *attr_meta;
        mali_ptr p = varying ? (v->varying_meta & ~0xF) : v->attribute_meta;
        mali_ptr p_orig = p;

        struct panwrap_mapped_memory *attr_mem = panwrap_find_mapped_gpu_mem_containing(p);

        for (int i = 0; i < count; ++i, p += sizeof(struct mali_attr_meta)) {
                attr_meta = panwrap_fetch_gpu_mem(attr_mem, p,
                                                  sizeof(*attr_mem));

                panwrap_log("{\n");
                panwrap_indent++;
                panwrap_prop("index = %d", attr_meta->index);

		if (attr_meta->index > max_index)
			max_index = attr_meta->index;
		panwrap_replay_swizzle(attr_meta->swizzle);
		panwrap_prop("format = %s", panwrap_format_name(attr_meta->format));

                panwrap_prop("unknown1 = 0x%" PRIx64, (u64) attr_meta->unknown1);
                panwrap_prop("unknown3 = 0x%" PRIx64, (u64) attr_meta->unknown3);
                panwrap_prop("src_offset = 0x%" PRIx64, (u64) attr_meta->src_offset);
                panwrap_indent--;
                panwrap_log("},\n");

        }

        panwrap_indent--;
        panwrap_log("};\n");

        TOUCH_LEN(attr_mem, p_orig, sizeof(struct mali_attr_meta) * count, base, job_no, true);

        return max_index;
}

static void
panwrap_replay_indices(uintptr_t pindices, uint32_t index_count, int job_no)
{
        struct panwrap_mapped_memory *imem = panwrap_find_mapped_gpu_mem_containing(pindices);

        if (imem) {
                /* Indices are literally just a u32 array :) */

                uint32_t *PANWRAP_PTR_VAR(indices, imem, pindices);

                panwrap_log("uint32_t indices_%d[] = {\n", job_no);
                panwrap_indent++;

                for (unsigned i = 0; i < (index_count + 1); i += 3)
                        panwrap_log("%d, %d, %d,\n",
                                    indices[i],
                                    indices[i + 1],
                                    indices[i + 2]);

                panwrap_indent--;
                panwrap_log("};\n");

                TOUCH_LEN(imem, pindices, sizeof(uint32_t) * (index_count + 1), "indices", job_no, false);
        }
}

/* return bits [lo, hi) of word */
static u32
bits(u32 word, u32 lo, u32 hi)
{
        if (hi - lo >= 32)
                return word; // avoid undefined behavior with the shift

        return (word >> lo) & ((1 << (hi - lo)) - 1);
}

static void
panwrap_replay_vertex_tiler_prefix(struct mali_vertex_tiler_prefix *p, int job_no)
{
        panwrap_log_cont("{\n");
        panwrap_indent++;

        panwrap_prop("invocation_count = %" PRIx32, p->invocation_count);
        panwrap_prop("size_y_shift = %d", p->size_y_shift);
        panwrap_prop("size_z_shift = %d", p->size_z_shift);
        panwrap_prop("workgroups_x_shift = %d", p->workgroups_x_shift);
        panwrap_prop("workgroups_y_shift = %d", p->workgroups_y_shift);
        panwrap_prop("workgroups_z_shift = %d", p->workgroups_z_shift);
        panwrap_prop("workgroups_x_shift_2 = 0x%" PRIx32, p->workgroups_x_shift_2);

        /* Decode invocation_count. See the comment before the definition of
         * invocation_count for an explanation.
         */
        panwrap_msg("size: (%d, %d, %d)\n",
                    bits(p->invocation_count, 0, p->size_y_shift) + 1,
                    bits(p->invocation_count, p->size_y_shift, p->size_z_shift) + 1,
                    bits(p->invocation_count, p->size_z_shift,
                         p->workgroups_x_shift) + 1);
        panwrap_msg("workgroups: (%d, %d, %d)\n",
                    bits(p->invocation_count, p->workgroups_x_shift,
                         p->workgroups_y_shift) + 1,
                    bits(p->invocation_count, p->workgroups_y_shift,
                         p->workgroups_z_shift) + 1,
                    bits(p->invocation_count, p->workgroups_z_shift,
                         32) + 1);

        panwrap_prop("unknown_draw = 0x%" PRIx32, p->unknown_draw);
        panwrap_prop("workgroups_x_shift_3 = 0x%" PRIx32, p->workgroups_x_shift_3);

        panwrap_prop("draw_mode = %s", panwrap_gl_mode_name(p->draw_mode));

        /* Index count only exists for tiler jobs anyway */

        if (p->index_count)
                panwrap_prop("index_count = MALI_POSITIVE(%" PRId32 ")", p->index_count + 1);

        DYN_MEMORY_PROP(p, job_no, indices);

        if (p->zero1) {
                panwrap_msg("Zero tripped\n");
                panwrap_prop("zero1 = 0x%" PRIx32, p->zero1);
        }

        panwrap_indent--;
        panwrap_log("},\n");
}

static void
panwrap_replay_uniform_buffers(mali_ptr pubufs, int ubufs_count, int job_no)
{
        struct panwrap_mapped_memory *umem = panwrap_find_mapped_gpu_mem_containing(pubufs);

        struct mali_uniform_buffer_meta *PANWRAP_PTR_VAR(ubufs, umem, pubufs);

        for (int i = 0; i < ubufs_count; i++) {
                mali_ptr ptr = ubufs[i].ptr << 2;
                struct panwrap_mapped_memory *umem2 = panwrap_find_mapped_gpu_mem_containing(ptr);
                uint32_t *PANWRAP_PTR_VAR(ubuf, umem2, ptr);
                char name[50];
                snprintf(name, sizeof(name), "ubuf_%d", i);
                /* The blob uses ubuf 0 to upload internal stuff and
                 * uniforms that won't fit/are accessed indirectly, so
                 * it puts it in the batchbuffer.
                 */
                panwrap_log("uint32_t %s_%d[] = {\n", name, job_no);
                panwrap_indent++;

                for (int j = 0; j <= ubufs[i].size; j++) {
                        for (int k = 0; k < 4; k++) {
                                if (k == 0)
                                        panwrap_log("0x%"PRIx32", ", ubuf[4 * j + k]);
                                else
                                        panwrap_log_cont("0x%"PRIx32", ", ubuf[4 * j + k]);

                        }

                        panwrap_log_cont("\n");
                }


                panwrap_indent--;
                panwrap_log("};\n");

                TOUCH_LEN(umem2, ptr, 16 * (ubufs[i].size + 1), name, job_no, i == 0);
        }

        panwrap_log("struct mali_uniform_buffer_meta uniform_buffers_%d[] = {\n",
                    job_no);
        panwrap_indent++;

        for (int i = 0; i < ubufs_count; i++) {
                panwrap_log("{\n");
                panwrap_indent++;
                panwrap_prop("size = MALI_POSITIVE(%d)", ubufs[i].size + 1);
                panwrap_prop("ptr = ubuf_%d_%d_p >> 2", i, job_no);
                panwrap_indent--;
                panwrap_log("},\n");
        }

        panwrap_indent--;
        panwrap_log("};\n");

        TOUCH_LEN(umem, pubufs, sizeof(struct mali_uniform_buffer_meta) * ubufs_count, "uniform_buffers", job_no, true);
}

static void
panwrap_replay_scratchpad(uintptr_t pscratchpad, int job_no, char *suffix)
{

        struct panwrap_mapped_memory *mem = panwrap_find_mapped_gpu_mem_containing(pscratchpad);

        struct bifrost_scratchpad *PANWRAP_PTR_VAR(scratchpad, mem, pscratchpad);

        if (scratchpad->zero)
                panwrap_msg("XXX scratchpad zero tripped");

        panwrap_log("struct bifrost_scratchpad scratchpad_%d%s = {\n", job_no, suffix);
        panwrap_indent++;

        panwrap_prop("flags = 0x%x", scratchpad->flags);
        MEMORY_PROP(scratchpad, gpu_scratchpad);

        panwrap_indent--;
        panwrap_log("};\n");

        TOUCH(mem, pscratchpad, *scratchpad, "scratchpad", job_no, true);
}

static void
panwrap_shader_disassemble(mali_ptr shader_ptr, int shader_no, int type,
                           bool is_bifrost)
{
        /* TODO */
}

static void
panwrap_replay_vertex_tiler_postfix_pre(const struct mali_vertex_tiler_postfix *p,
                                        int job_no, enum mali_job_type job_type,
                                        char *suffix, bool is_bifrost)
{
        mali_ptr shader_meta_ptr = (u64) (uintptr_t) (p->_shader_upper << 4);
        struct panwrap_mapped_memory *attr_mem;

        /* On Bifrost, since the tiler heap (for tiler jobs) and the scratchpad
         * are the only things actually needed from the FBD, vertex/tiler jobs
         * no longer reference the FBD -- instead, this field points to some
         * info about the scratchpad.
         */
        if (is_bifrost)
                panwrap_replay_scratchpad(p->framebuffer & ~FBD_TYPE, job_no, suffix);
        else if (p->framebuffer & MALI_MFBD)
                panwrap_replay_mfbd_bfr((u64) ((uintptr_t) p->framebuffer) & FBD_MASK, job_no);
        else
                panwrap_replay_sfbd((u64) (uintptr_t) p->framebuffer, job_no);

        int varying_count = 0, attribute_count = 0, uniform_count = 0, uniform_buffer_count = 0;
        int texture_count = 0, sampler_count = 0;

        if (shader_meta_ptr) {
                struct panwrap_mapped_memory *smem = panwrap_find_mapped_gpu_mem_containing(shader_meta_ptr);
                struct mali_shader_meta *PANWRAP_PTR_VAR(s, smem, shader_meta_ptr);

                panwrap_log("struct mali_shader_meta shader_meta_%d%s = {\n", job_no, suffix);
                panwrap_indent++;

                /* Save for dumps */
                attribute_count = s->attribute_count;
                varying_count = s->varying_count;
                texture_count = s->texture_count;
                sampler_count = s->sampler_count;

                if (is_bifrost) {
                        uniform_count = s->bifrost2.uniform_count;
                        uniform_buffer_count = s->bifrost1.uniform_buffer_count;
                } else {
                        uniform_count = s->midgard1.uniform_count;
                        /* TODO figure this out */
                        uniform_buffer_count = 1;
                }

                mali_ptr shader_ptr = panwrap_replay_shader_address("shader", s->shader);


                panwrap_prop("texture_count = %" PRId16, s->texture_count);
                panwrap_prop("sampler_count = %" PRId16, s->sampler_count);
                panwrap_prop("attribute_count = %" PRId16, s->attribute_count);
                panwrap_prop("varying_count = %" PRId16, s->varying_count);

                if (is_bifrost) {
                        panwrap_log(".bifrost1 = {\n");
                        panwrap_indent++;

                        panwrap_prop("uniform_buffer_count = %" PRId32, s->bifrost1.uniform_buffer_count);
                        panwrap_prop("unk1 = 0x%" PRIx32, s->bifrost1.unk1);

                        panwrap_indent--;
                        panwrap_log("},\n");
                } else {
                        panwrap_log(".midgard1 = {\n");
                        panwrap_indent++;

                        panwrap_prop("uniform_count = %" PRId16, s->midgard1.uniform_count);
                        panwrap_prop("work_count = %" PRId16, s->midgard1.work_count);
                        panwrap_prop("unknown1 = %s0x%" PRIx32,
                                     s->midgard1.unknown1 & MALI_NO_ALPHA_TO_COVERAGE ? "MALI_NO_ALPHA_TO_COVERAGE | " : "",
                                     s->midgard1.unknown1 & ~MALI_NO_ALPHA_TO_COVERAGE);
                        panwrap_prop("unknown2 = 0x%" PRIx32, s->midgard1.unknown2);

                        panwrap_indent--;
                        panwrap_log("},\n");
                }

                if (s->depth_units || s->depth_factor) {
                        if (is_bifrost)
                                panwrap_prop("depth_units = %f", s->depth_units);
                        else
                                panwrap_prop("depth_units = MALI_NEGATIVE(%f)", s->depth_units - 1.0f);

                        panwrap_prop("depth_factor = %f", s->depth_factor);
                }

                bool invert_alpha_coverage = s->alpha_coverage & 0xFFF0;
                uint16_t inverted_coverage = invert_alpha_coverage ? ~s->alpha_coverage : s->alpha_coverage;

                panwrap_prop("alpha_coverage = %sMALI_ALPHA_COVERAGE(%f)",
                             invert_alpha_coverage ? "~" : "",
                             MALI_GET_ALPHA_COVERAGE(inverted_coverage));

                panwrap_log(".unknown2_3 = ");

                int unknown2_3 = s->unknown2_3;
                int unknown2_4 = s->unknown2_4;

                /* We're not quite sure what these flags mean without the depth test, if anything */

                if (unknown2_3 & (MALI_DEPTH_TEST | MALI_DEPTH_FUNC_MASK)) {
                        const char *func = panwrap_func_name(MALI_GET_DEPTH_FUNC(unknown2_3));
                        unknown2_3 &= ~MALI_DEPTH_FUNC_MASK;

                        panwrap_log_cont("MALI_DEPTH_FUNC(%s) | ", func);
                }


                panwrap_log_decoded_flags(u3_flag_info, unknown2_3);
                panwrap_log_cont(",\n");

                panwrap_prop("stencil_mask_front = 0x%02X", s->stencil_mask_front);
                panwrap_prop("stencil_mask_back = 0x%02X", s->stencil_mask_back);

                panwrap_log(".unknown2_4 = ");
                panwrap_log_decoded_flags(u4_flag_info, unknown2_4);
                panwrap_log_cont(",\n");

                panwrap_replay_stencil("front", &s->stencil_front);
                panwrap_replay_stencil("back", &s->stencil_back);

                if (is_bifrost) {
                        panwrap_log(".bifrost2 = {\n");
                        panwrap_indent++;

                        panwrap_prop("unk3 = 0x%" PRIx32, s->bifrost2.unk3);
                        panwrap_prop("preload_regs = 0x%" PRIx32, s->bifrost2.preload_regs);
                        panwrap_prop("uniform_count = %" PRId32, s->bifrost2.uniform_count);
                        panwrap_prop("unk4 = 0x%" PRIx32, s->bifrost2.unk4);

                        panwrap_indent--;
                        panwrap_log("},\n");
                } else {
                        panwrap_log(".midgard2 = {\n");
                        panwrap_indent++;

                        panwrap_prop("unknown2_7 = 0x%" PRIx32, s->midgard2.unknown2_7);
                        panwrap_indent--;
                        panwrap_log("},\n");
                }

                panwrap_prop("unknown2_8 = 0x%" PRIx32, s->unknown2_8);

                bool blend_shader = false;

                if (!is_bifrost) {
                        if (s->unknown2_3 & MALI_HAS_BLEND_SHADER) {
                                blend_shader = true;
                                panwrap_replay_shader_address("blend_shader", s->blend_shader);
                        } else {
                                panwrap_replay_blend_equation(&s->blend_equation, "");
                        }
                }

                panwrap_indent--;
                panwrap_log("};\n");

                TOUCH(smem, shader_meta_ptr, *s, "shader_meta", job_no, false);

                /* TODO while Bifrost always uses these MRT blend fields,
                 * presumably Midgard does as well when using the MFBD. We need
                 * to figure out the bit to enable it on Midgard.
                 */

#if 0
#ifdef T8XX

                if (job_type == JOB_TYPE_TILER) {
                        panwrap_log("struct mali_blend_meta blend_meta_%d[] = {\n",
                                    job_no);
                        panwrap_indent++;

                        int i;

                        for (i = 0; i < 4; i++) {
                                /* TODO: MRT case */
                                if (i) break;

                                const struct mali_blend_meta *b = &s->blend_meta[i];
                                panwrap_log("{\n");
                                panwrap_indent++;

#if 1
                                panwrap_prop("unk1 = 0x%" PRIx64, b->unk1);
                                panwrap_replay_blend_equation(&b->blend_equation_1, "_1");
                                panwrap_replay_blend_equation(&b->blend_equation_2, "_2");

                                if (b->zero1 || b->zero2 || b->zero3) {
                                        panwrap_msg("blend zero tripped\n");
                                        panwrap_prop("zero1 = 0x%x", b->zero1);
                                        panwrap_prop("zero2 = 0x%x", b->zero2);
                                        panwrap_prop("zero3 = 0x%x", b->zero3);
                                }

#else

                                panwrap_prop("unk1 = 0x%" PRIx32, b->unk1);
                                /* TODO figure out blend shader enable bit */
                                panwrap_replay_blend_equation(&b->blend_equation);
                                panwrap_prop("unk2 = 0x%" PRIx16, b->unk2);
                                panwrap_prop("index = 0x%" PRIx16, b->index);
                                panwrap_prop("unk3 = 0x%" PRIx32, b->unk3);
#endif

                                panwrap_indent--;
                                panwrap_log("},\n");

#if 0

                                if (b->unk2 == 3)
                                        break;

#endif
                        }

                        panwrap_indent--;
                        panwrap_log("};\n");

                        /* This needs to be uploaded right after the
                         * shader_meta since it's technically part of the same
                         * (variable-size) structure.
                         */

                        TOUCH_LEN(smem, shader_meta_ptr + sizeof(struct mali_shader_meta), i * sizeof(struct mali_blend_meta), "blend_meta", job_no, false);
                }

#endif
#endif

                panwrap_shader_disassemble(shader_ptr, job_no, job_type, is_bifrost);

                if (!is_bifrost && blend_shader)
                        panwrap_shader_disassemble(s->blend_shader & ~0xF, job_no, job_type, false);

        } else
                panwrap_msg("<no shader>\n");

        if (p->viewport) {
                struct panwrap_mapped_memory *fmem = panwrap_find_mapped_gpu_mem_containing(p->viewport);
                struct mali_viewport *PANWRAP_PTR_VAR(f, fmem, p->viewport);

                panwrap_log("struct mali_viewport viewport_%d%s = {\n", job_no, suffix);
                panwrap_indent++;
                panwrap_log(".floats = {\n");
                panwrap_indent++;

                for (int i = 0; i < sizeof(f->floats) / sizeof(f->floats[0]); i += 2)
                        panwrap_log("%ff, %ff,\n", f->floats[i], f->floats[i + 1]);

                panwrap_indent--;
                panwrap_log("},\n");

                panwrap_prop("depth_range_n = %f", f->depth_range_n);
                panwrap_prop("depth_range_f = %f", f->depth_range_f);

                /* Only the higher coordinates are MALI_POSITIVE scaled */

                panwrap_prop("viewport0 = { %d, %d }",
                             f->viewport0[0], f->viewport0[1]);

                panwrap_prop("viewport1 = { MALI_POSITIVE(%d), MALI_POSITIVE(%d) }",
                             f->viewport1[0] + 1, f->viewport1[1] + 1);

                panwrap_indent--;
                panwrap_log("};\n");

                TOUCH(fmem, p->viewport, *f, "viewport", job_no, true);
        }

        if (p->attribute_meta) {
                unsigned max_attr_index = panwrap_replay_attribute_meta(job_no, attribute_count, p, false, suffix);

                attr_mem = panwrap_find_mapped_gpu_mem_containing(p->attributes);
                panwrap_replay_attributes(attr_mem, p->attributes, job_no, suffix, max_attr_index + 1, false);
        }

        /* Varyings are encoded like attributes but not actually sent; we just
         * pass a zero buffer with the right stride/size set, (or whatever)
         * since the GPU will write to it itself */

        if (p->varyings) {
                attr_mem = panwrap_find_mapped_gpu_mem_containing(p->varyings);

                /* Number of descriptors depends on whether there are
                 * non-internal varyings */

                panwrap_replay_attributes(attr_mem, p->varyings, job_no, suffix, varying_count > 1 ? 2 : 1, true);
        }

        if (p->varying_meta) {
                panwrap_replay_attribute_meta(job_no, varying_count, p, true, suffix);
        }

        if (p->uniforms) {
                int rows = uniform_count, width = 4;
                size_t sz = rows * width * sizeof(float);

                struct panwrap_mapped_memory *uniform_mem = panwrap_find_mapped_gpu_mem_containing(p->uniforms);
                panwrap_fetch_gpu_mem(uniform_mem, p->uniforms, sz);
                float *PANWRAP_PTR_VAR(uniforms, uniform_mem, p->uniforms);

                panwrap_log("float uniforms_%d%s[] = {\n", job_no, suffix);

                panwrap_indent++;

                for (int row = 0; row < rows; row++) {
                        panwrap_log_empty();

                        for (int i = 0; i < width; i++)
                                panwrap_log_cont("%ff, ", uniforms[i]);

                        panwrap_log_cont("\n");

                        uniforms += width;
                }

                panwrap_indent--;
                panwrap_log("};\n");

                TOUCH_LEN(uniform_mem, p->uniforms, sz, "uniforms", job_no, true);
        }

        if (p->uniform_buffers) {
                panwrap_replay_uniform_buffers(p->uniform_buffers, uniform_buffer_count, job_no);
        }

        if (p->texture_trampoline) {
                struct panwrap_mapped_memory *mmem = panwrap_find_mapped_gpu_mem_containing(p->texture_trampoline);

                if (mmem) {
                        mali_ptr *PANWRAP_PTR_VAR(u, mmem, p->texture_trampoline);

                        panwrap_log("uint64_t texture_trampoline_%d[] = {\n", job_no);
                        panwrap_indent++;

                        for (int tex = 0; tex < texture_count; ++tex) {
                                mali_ptr *PANWRAP_PTR_VAR(u, mmem, p->texture_trampoline + tex * sizeof(mali_ptr));
                                char *a = pointer_as_memory_reference(*u);
                                panwrap_log("%s,\n", a);
                                free(a);
                        }

                        panwrap_indent--;
                        panwrap_log("};\n");

                        //TOUCH(mmem, p->texture_trampoline, *u, "texture_trampoline", job_no, true);

                        /* Now, finally, descend down into the texture descriptor */
                        for (int tex = 0; tex < texture_count; ++tex) {
                                mali_ptr *PANWRAP_PTR_VAR(u, mmem, p->texture_trampoline + tex * sizeof(mali_ptr));
                                struct panwrap_mapped_memory *tmem = panwrap_find_mapped_gpu_mem_containing(*u);

                                if (tmem) {
                                        struct mali_texture_descriptor *PANWRAP_PTR_VAR(t, tmem, *u);

                                        panwrap_log("struct mali_texture_descriptor texture_descriptor_%d_%d = {\n", job_no, tex);
                                        panwrap_indent++;

                                        panwrap_prop("width = MALI_POSITIVE(%" PRId16 ")", t->width + 1);
                                        panwrap_prop("height = MALI_POSITIVE(%" PRId16 ")", t->height + 1);
                                        panwrap_prop("depth = MALI_POSITIVE(%" PRId16 ")", t->depth + 1);

                                        panwrap_prop("unknown3 = %" PRId16, t->unknown3);
                                        panwrap_prop("unknown3A = %" PRId8, t->unknown3A);
                                        panwrap_prop("nr_mipmap_levels = %" PRId8, t->nr_mipmap_levels);

                                        /* TODO: Should format printing be refactored */
                                        struct mali_texture_format f = t->format;

                                        panwrap_log(".format = {\n");
                                        panwrap_indent++;

                                        panwrap_replay_swizzle(f.swizzle);
					panwrap_prop("format = %s", panwrap_format_name(f.format));

                                        panwrap_prop("usage1 = 0x%" PRIx32, f.usage1);
                                        panwrap_prop("is_not_cubemap = %" PRId32, f.is_not_cubemap);
                                        panwrap_prop("usage2 = 0x%" PRIx32, f.usage2);

                                        panwrap_indent--;
                                        panwrap_log("},\n");

					panwrap_replay_swizzle(t->swizzle);

                                        if (t->swizzle_zero) {
                                                /* Shouldn't happen */
                                                panwrap_msg("Swizzle zero tripped but replay will be fine anyway");
                                                panwrap_prop("swizzle_zero = %d", t->swizzle_zero);
                                        }

                                        panwrap_prop("unknown3 = 0x%" PRIx32, t->unknown3);

                                        panwrap_prop("unknown5 = 0x%" PRIx32, t->unknown5);
                                        panwrap_prop("unknown6 = 0x%" PRIx32, t->unknown6);
                                        panwrap_prop("unknown7 = 0x%" PRIx32, t->unknown7);

                                        panwrap_log(".swizzled_bitmaps = {\n");
                                        panwrap_indent++;

                                        int bitmap_count = 1 + t->nr_mipmap_levels + t->unknown3A;
                                        int max_count = sizeof(t->swizzled_bitmaps) / sizeof(t->swizzled_bitmaps[0]);

                                        if (bitmap_count > max_count) {
                                                panwrap_msg("XXX: bitmap count tripped");
                                                bitmap_count = max_count;
                                        }

                                        for (int i = 0; i < bitmap_count; ++i) {
                                                char *a = pointer_as_memory_reference(t->swizzled_bitmaps[i]);
                                                panwrap_log("%s, \n", a);
                                                free(a);
                                        }

                                        panwrap_indent--;
                                        panwrap_log("},\n");

                                        panwrap_indent--;
                                        panwrap_log("};\n");

                                        //TOUCH(tmem, *u, *t, "texture_descriptor", job_no, false);
                                }
                        }
                }
        }

        if (p->sampler_descriptor) {
                struct panwrap_mapped_memory *smem = panwrap_find_mapped_gpu_mem_containing(p->sampler_descriptor);

                if (smem) {
                        struct mali_sampler_descriptor *s;

                        mali_ptr d = p->sampler_descriptor;

                        for (int i = 0; i < sampler_count; ++i) {
                                s = panwrap_fetch_gpu_mem(smem, d + sizeof(*s) * i, sizeof(*s));

                                panwrap_log("struct mali_sampler_descriptor sampler_descriptor_%d_%d = {\n", job_no, i);
                                panwrap_indent++;

                                /* Only the lower two bits are understood right now; the rest we display as hex */
                                panwrap_log(".filter_mode = MALI_GL_TEX_MIN(%s) | MALI_GL_TEX_MAG(%s) | 0x%" PRIx32",\n",
                                            MALI_FILTER_NAME(s->filter_mode & MALI_GL_TEX_MIN_MASK),
                                            MALI_FILTER_NAME(s->filter_mode & MALI_GL_TEX_MAG_MASK),
                                            s->filter_mode & ~3);

                                panwrap_prop("min_lod = FIXED_16(%f)", DECODE_FIXED_16(s->min_lod));
                                panwrap_prop("max_lod = FIXED_16(%f)", DECODE_FIXED_16(s->max_lod));

                                panwrap_prop("wrap_s = %s", panwrap_wrap_mode_name(s->wrap_s));
                                panwrap_prop("wrap_t = %s", panwrap_wrap_mode_name(s->wrap_t));
                                panwrap_prop("wrap_r = %s", panwrap_wrap_mode_name(s->wrap_r));

                                panwrap_prop("compare_func = %s", panwrap_alt_func_name(s->compare_func));

                                if (s->zero || s->zero2) {
                                        panwrap_msg("Zero tripped\n");
                                        panwrap_prop("zero = 0x%X, 0x%X\n", s->zero, s->zero2);
                                }

                                panwrap_prop("unknown2 = %d", s->unknown2);

                                panwrap_prop("border_color = { %f, %f, %f, %f }",
                                             s->border_color[0],
                                             s->border_color[1],
                                             s->border_color[2],
                                             s->border_color[3]);

                                panwrap_indent--;
                                panwrap_log("};\n");
                        }

                        //TOUCH(smem, p->sampler_descriptor, *s, "sampler_descriptor", job_no, false);
                }
        }
}

static void
panwrap_replay_vertex_tiler_postfix(const struct mali_vertex_tiler_postfix *p, int job_no, bool is_bifrost)
{
        panwrap_log_cont("{\n");
        panwrap_indent++;

        MEMORY_PROP(p, position_varying);
        MEMORY_COMMENT(p, position_varying);
        DYN_MEMORY_PROP(p, job_no, uniform_buffers);
        MEMORY_COMMENT(p, uniform_buffers);
        DYN_MEMORY_PROP(p, job_no, texture_trampoline);
        MEMORY_COMMENT(p, texture_trampoline);
        DYN_MEMORY_PROP(p, job_no, sampler_descriptor);
        MEMORY_COMMENT(p, sampler_descriptor);
        DYN_MEMORY_PROP(p, job_no, uniforms);
        MEMORY_COMMENT(p, uniforms);
        DYN_MEMORY_PROP(p, job_no, attributes);
        MEMORY_COMMENT(p, attributes);
        DYN_MEMORY_PROP(p, job_no, attribute_meta);
        MEMORY_COMMENT(p, attribute_meta);
        DYN_MEMORY_PROP(p, job_no, varyings);
        MEMORY_COMMENT(p, varyings);
        DYN_MEMORY_PROP(p, job_no, varying_meta);
        MEMORY_COMMENT(p, varying_meta);
        DYN_MEMORY_PROP(p, job_no, viewport);
        MEMORY_COMMENT(p, viewport);
        DYN_MEMORY_PROP(p, job_no, occlusion_counter);
        MEMORY_COMMENT(p, occlusion_counter);
        MEMORY_COMMENT(p, framebuffer & ~1);
        panwrap_msg("%" PRIx64 "\n", p->viewport);
        panwrap_msg("%" PRIx64 "\n", p->framebuffer);

        if (is_bifrost)
                panwrap_prop("framebuffer = scratchpad_%d_p", job_no);
        else
                panwrap_prop("framebuffer = framebuffer_%d_p | %s", job_no, p->framebuffer & MALI_MFBD ? "MALI_MFBD" : "0");

        panwrap_prop("_shader_upper = (shader_meta_%d_p) >> 4", job_no);
        panwrap_prop("flags = %d", p->flags);

        panwrap_indent--;
        panwrap_log("},\n");
}

static void
panwrap_replay_vertex_only_bfr(struct bifrost_vertex_only *v)
{
        panwrap_log_cont("{\n");
        panwrap_indent++;

        panwrap_prop("unk2 = 0x%x", v->unk2);

        if (v->zero0 || v->zero1) {
                panwrap_msg("vertex only zero tripped");
                panwrap_prop("zero0 = 0x%" PRIx32, v->zero0);
                panwrap_prop("zero1 = 0x%" PRIx64, v->zero1);
        }

        panwrap_indent--;
        panwrap_log("}\n");
}

static void
panwrap_replay_tiler_heap_meta(mali_ptr gpu_va, int job_no)
{

        struct panwrap_mapped_memory *mem = panwrap_find_mapped_gpu_mem_containing(gpu_va);
        const struct bifrost_tiler_heap_meta *PANWRAP_PTR_VAR(h, mem, gpu_va);

        /* The tiler_heap_meta structure is modified by the GPU, and it's
         * supposed to be shared by tiler jobs corresponding to the same
         * fragment job, so be careful to deduplicate it here.
         */
        if (panwrap_deduplicate(mem, gpu_va, "tiler_heap_meta", job_no)) return;

        panwrap_log("struct mali_tiler_heap_meta tiler_heap_meta_%d = {\n", job_no);
        panwrap_indent++;

        if (h->zero) {
                panwrap_msg("tiler heap zero tripped\n");
                panwrap_prop("zero = 0x%x", h->zero);
        }

        for (int i = 0; i < 12; i++) {
                if (h->zeros[i] != 0) {
                        panwrap_msg("tiler heap zero %d tripped, value %x\n",
                                    i, h->zeros[i]);
                }
        }

        panwrap_prop("heap_size = 0x%x", h->heap_size);
        MEMORY_PROP(h, tiler_heap_start);
        MEMORY_PROP(h, tiler_heap_free);

        /* this might point to the beginning of another buffer, when it's
         * really the end of the tiler heap buffer, so we have to be careful
         * here.
         */
        char *a = pointer_as_memory_reference(h->tiler_heap_end - 1);
        panwrap_prop("tiler_heap_end = %s + 1", a);
        free(a);

        panwrap_indent--;
        panwrap_log("};\n");

        TOUCH(mem, gpu_va, *h, "tiler_heap_meta", job_no, true);
}

static void
panwrap_replay_tiler_meta(mali_ptr gpu_va, int job_no)
{
        struct panwrap_mapped_memory *mem = panwrap_find_mapped_gpu_mem_containing(gpu_va);
        const struct bifrost_tiler_meta *PANWRAP_PTR_VAR(t, mem, gpu_va);

        panwrap_replay_tiler_heap_meta(t->tiler_heap_meta, job_no);

        panwrap_log("struct mali_tiler_meta tiler_meta_%d = {\n", job_no);
        panwrap_indent++;

        if (t->zero0 || t->zero1) {
                panwrap_msg("tiler meta zero tripped");
                panwrap_prop("zero0 = 0x%" PRIx64, t->zero0);
                panwrap_prop("zero1 = 0x%" PRIx64, t->zero1);
        }

        panwrap_prop("unk = 0x%x", t->unk);
        panwrap_prop("width = MALI_POSITIVE(%d)", t->width + 1);
        panwrap_prop("height = MALI_POSITIVE(%d)", t->height + 1);
        DYN_MEMORY_PROP(t, job_no, tiler_heap_meta);

        for (int i = 0; i < 12; i++) {
                if (t->zeros[i] != 0) {
                        panwrap_msg("tiler heap zero %d tripped, value %" PRIx64 "\n",
                                    i, t->zeros[i]);
                }
        }

        panwrap_indent--;
        panwrap_log("};\n");

        TOUCH(mem, gpu_va, *t, "tiler_meta", job_no, true);
}

static void
panwrap_replay_gl_enables(uint32_t gl_enables, int job_type)
{
        panwrap_log(".gl_enables = ");

        if (job_type == JOB_TYPE_TILER) {
                panwrap_log_cont("MALI_GL_FRONT_FACE(MALI_GL_%s) | ",
                                 gl_enables & MALI_GL_FRONT_FACE(MALI_GL_CW) ? "CW" : "CCW");

                gl_enables &= ~(MALI_GL_FRONT_FACE(1));
        }

        panwrap_log_decoded_flags(gl_enable_flag_info, gl_enables);

        panwrap_log_cont(",\n");
}

static void
panwrap_replay_tiler_only_bfr(const struct bifrost_tiler_only *t, int job_no)
{
        panwrap_log_cont("{\n");
        panwrap_indent++;

        panwrap_prop("line_width = %f", t->line_width);
        DYN_MEMORY_PROP(t, job_no, tiler_meta);
        panwrap_replay_gl_enables(t->gl_enables, JOB_TYPE_TILER);

        if (t->zero0 || t->zero1 || t->zero2 || t->zero3 || t->zero4 || t->zero5
                        || t->zero6 || t->zero7 || t->zero8) {
                panwrap_msg("tiler only zero tripped");
                panwrap_prop("zero0 = 0x%" PRIx32, t->zero0);
                panwrap_prop("zero1 = 0x%" PRIx64, t->zero1);
                panwrap_prop("zero2 = 0x%" PRIx64, t->zero2);
                panwrap_prop("zero3 = 0x%" PRIx64, t->zero3);
                panwrap_prop("zero4 = 0x%" PRIx64, t->zero4);
                panwrap_prop("zero5 = 0x%" PRIx64, t->zero5);
                panwrap_prop("zero6 = 0x%" PRIx64, t->zero6);
                panwrap_prop("zero7 = 0x%" PRIx32, t->zero7);
                panwrap_prop("zero8 = 0x%" PRIx64, t->zero8);
        }

        panwrap_indent--;
        panwrap_log("},\n");
}

static int
panwrap_replay_vertex_job_bfr(const struct mali_job_descriptor_header *h,
                              const struct panwrap_mapped_memory *mem,
                              mali_ptr payload, int job_no)
{
        struct bifrost_payload_vertex *PANWRAP_PTR_VAR(v, mem, payload);

        panwrap_replay_vertex_tiler_postfix_pre(&v->postfix, job_no, h->job_type, "", true);

        panwrap_log("struct bifrost_payload_vertex payload_%d = {\n", job_no);
        panwrap_indent++;

        panwrap_log(".prefix = ");
        panwrap_replay_vertex_tiler_prefix(&v->prefix, job_no);

        panwrap_log(".vertex = ");
        panwrap_replay_vertex_only_bfr(&v->vertex);

        panwrap_log(".postfix = ");
        panwrap_replay_vertex_tiler_postfix(&v->postfix, job_no, true);

        panwrap_indent--;
        panwrap_log("};\n");

        return sizeof(*v);
}

static int
panwrap_replay_tiler_job_bfr(const struct mali_job_descriptor_header *h,
                             const struct panwrap_mapped_memory *mem,
                             mali_ptr payload, int job_no)
{
        struct bifrost_payload_tiler *PANWRAP_PTR_VAR(t, mem, payload);

        panwrap_replay_vertex_tiler_postfix_pre(&t->postfix, job_no, h->job_type, "", true);

        panwrap_replay_indices(t->prefix.indices, t->prefix.index_count, job_no);
        panwrap_replay_tiler_meta(t->tiler.tiler_meta, job_no);

        panwrap_log("struct bifrost_payload_tiler payload_%d = {\n", job_no);
        panwrap_indent++;

        panwrap_log(".prefix = ");
        panwrap_replay_vertex_tiler_prefix(&t->prefix, job_no);

        panwrap_log(".tiler = ");
        panwrap_replay_tiler_only_bfr(&t->tiler, job_no);

        panwrap_log(".postfix = ");
        panwrap_replay_vertex_tiler_postfix(&t->postfix, job_no, true);

        panwrap_indent--;
        panwrap_log("};\n");

        return sizeof(*t);
}

static int
panwrap_replay_vertex_or_tiler_job_mdg(const struct mali_job_descriptor_header *h,
                                       const struct panwrap_mapped_memory *mem,
                                       mali_ptr payload, int job_no)
{
        struct midgard_payload_vertex_tiler *PANWRAP_PTR_VAR(v, mem, payload);


        char *a = pointer_as_memory_reference(payload);
        panwrap_msg("vt payload: %s\n", a);
        free(a);

        panwrap_replay_vertex_tiler_postfix_pre(&v->postfix, job_no, h->job_type, "", false);

        panwrap_replay_indices(v->prefix.indices, v->prefix.index_count, job_no);

        panwrap_log("struct midgard_payload_vertex_tiler payload_%d = {\n", job_no);
        panwrap_indent++;

        panwrap_prop("line_width = %ff", v->line_width);
        panwrap_log(".prefix = ");
        panwrap_replay_vertex_tiler_prefix(&v->prefix, job_no);

        panwrap_replay_gl_enables(v->gl_enables, h->job_type);
        panwrap_prop("draw_start = %d", v->draw_start);

#ifdef T6XX

        if (v->zero3) {
                panwrap_msg("Zero tripped\n");
                panwrap_prop("zero3 = 0x%" PRIx32, v->zero3);
        }

#endif

        if (v->zero5) {
                panwrap_msg("Zero tripped\n");
                panwrap_prop("zero5 = 0x%" PRIx64, v->zero5);
        }

        panwrap_log(".postfix = ");
        panwrap_replay_vertex_tiler_postfix(&v->postfix, job_no, false);

        panwrap_indent--;
        panwrap_log("};\n");

        return sizeof(*v);
}

static int
panwrap_replay_fragment_job(const struct panwrap_mapped_memory *mem,
                            mali_ptr payload, int job_no,
                            bool is_bifrost)
{
        const struct mali_payload_fragment *PANWRAP_PTR_VAR(s, mem, payload);

        bool fbd_dumped = false;

        if (!is_bifrost && (s->framebuffer & FBD_TYPE) == MALI_SFBD) {
                /* Only SFBDs are understood, not MFBDs. We're speculating,
                 * based on the versioning, kernel code, etc, that the
                 * difference is between Single FrameBuffer Descriptor and
                 * Multiple FrmaeBuffer Descriptor; the change apparently lines
                 * up with multi-framebuffer support being added (T7xx onwards,
                 * including Gxx). In any event, there's some field shuffling
                 * that we haven't looked into yet. */

                panwrap_replay_sfbd(s->framebuffer & FBD_MASK, job_no);
                fbd_dumped = true;
        } else if ((s->framebuffer & FBD_TYPE) == MALI_MFBD) {
                /* We don't know if Bifrost supports SFBD's at all, since the
                 * driver never uses them. And the format is different from
                 * Midgard anyways, due to the tiler heap and scratchpad being
                 * moved out into separate structures, so it's not clear what a
                 * Bifrost SFBD would even look like without getting an actual
                 * trace, which appears impossible.
                 */

                panwrap_replay_mfbd_bfr(s->framebuffer & FBD_MASK, job_no);
                fbd_dumped = true;
        }

        uintptr_t p = (uintptr_t) s->framebuffer & FBD_MASK;

        panwrap_log("struct mali_payload_fragment payload_%d = {\n", job_no);
        panwrap_indent++;

        /* See the comments by the macro definitions for mathematical context
         * on why this is so weird */

        if (MALI_TILE_COORD_FLAGS(s->max_tile_coord) || MALI_TILE_COORD_FLAGS(s->min_tile_coord))
                panwrap_msg("Tile coordinate flag missed, replay wrong\n");

        panwrap_prop("min_tile_coord = MALI_COORDINATE_TO_TILE_MIN(%d, %d)",
                     MALI_TILE_COORD_X(s->min_tile_coord) << MALI_TILE_SHIFT,
                     MALI_TILE_COORD_Y(s->min_tile_coord) << MALI_TILE_SHIFT);

        panwrap_prop("max_tile_coord = MALI_COORDINATE_TO_TILE_MAX(%d, %d)",
                     (MALI_TILE_COORD_X(s->max_tile_coord) + 1) << MALI_TILE_SHIFT,
                     (MALI_TILE_COORD_Y(s->max_tile_coord) + 1) << MALI_TILE_SHIFT);

        /* If the FBD was just decoded, we can refer to it by pointer. If not,
         * we have to fallback on offsets. */

        const char *fbd_type = s->framebuffer & MALI_MFBD ? "MALI_MFBD" : "MALI_SFBD";

        if (fbd_dumped)
                panwrap_prop("framebuffer = framebuffer_%d_p | %s", job_no, fbd_type);
        else
                panwrap_prop("framebuffer = %s | %s", pointer_as_memory_reference(p), fbd_type);

        panwrap_indent--;
        panwrap_log("};\n");

        return sizeof(*s);
}

static int job_descriptor_number = 0;

int
panwrap_replay_jc(mali_ptr jc_gpu_va, bool bifrost)
{
        struct mali_job_descriptor_header *h;

        int start_number = 0;

        bool first = true;
        bool last_size;

        do {
                struct panwrap_mapped_memory *mem =
                        panwrap_find_mapped_gpu_mem_containing(jc_gpu_va);

                void *payload;

                h = PANWRAP_PTR(mem, jc_gpu_va, struct mali_job_descriptor_header);

                /* On Midgard, for 32-bit jobs except for fragment jobs, the
                 * high 32-bits of the 64-bit pointer are reused to store
                 * something else.
                 */
                int offset = h->job_descriptor_size == MALI_JOB_32 &&
                             h->job_type != JOB_TYPE_FRAGMENT ? 4 : 0;
                mali_ptr payload_ptr = jc_gpu_va + sizeof(*h) - offset;

                payload = panwrap_fetch_gpu_mem(mem, payload_ptr,
                                                MALI_PAYLOAD_SIZE);

                int job_no = job_descriptor_number++;

                if (first)
                        start_number = job_no;

                panwrap_log("struct mali_job_descriptor_header job_%d = {\n", job_no);
                panwrap_indent++;

                panwrap_prop("job_type = %s", panwrap_job_type_name(h->job_type));

                /* Save for next job fixing */
                last_size = h->job_descriptor_size;

                if (h->job_descriptor_size)
                        panwrap_prop("job_descriptor_size = %d", h->job_descriptor_size);

                if (h->exception_status)
                        panwrap_prop("exception_status = %d", h->exception_status);

                if (h->first_incomplete_task)
                        panwrap_prop("first_incomplete_task = %d", h->first_incomplete_task);

                if (h->fault_pointer)
                        panwrap_prop("fault_pointer = 0x%" PRIx64, h->fault_pointer);

                if (h->job_barrier)
                        panwrap_prop("job_barrier = %d", h->job_barrier);

                panwrap_prop("job_index = %d", h->job_index);

                if (h->unknown_flags)
                        panwrap_prop("unknown_flags = %d", h->unknown_flags);

                if (h->job_dependency_index_1)
                        panwrap_prop("job_dependency_index_1 = %d", h->job_dependency_index_1);

                if (h->job_dependency_index_2)
                        panwrap_prop("job_dependency_index_2 = %d", h->job_dependency_index_2);

                panwrap_indent--;
                panwrap_log("};\n");

                /* Do not touch the field yet -- decode the payload first, and
                 * don't touch that either. This is essential for the uploads
                 * to occur in sequence and therefore be dynamically allocated
                 * correctly. Do note the size, however, for that related
                 * reason. */

                int payload_size = 0;

                switch (h->job_type) {
                case JOB_TYPE_SET_VALUE: {
                        struct mali_payload_set_value *s = payload;

                        panwrap_log("struct mali_payload_set_value payload_%d = {\n", job_no);
                        panwrap_indent++;
                        MEMORY_PROP(s, out);
                        panwrap_prop("unknown = 0x%" PRIX64, s->unknown);
                        panwrap_indent--;
                        panwrap_log("};\n");

                        payload_size = sizeof(*s);

                        break;
                }

                case JOB_TYPE_TILER:
                case JOB_TYPE_VERTEX:
                case JOB_TYPE_COMPUTE:
                        if (bifrost) {
                                if (h->job_type == JOB_TYPE_TILER)
                                        payload_size = panwrap_replay_tiler_job_bfr(h, mem, payload_ptr, job_no);
                                else
                                        payload_size = panwrap_replay_vertex_job_bfr(h, mem, payload_ptr, job_no);
                        } else
                                payload_size = panwrap_replay_vertex_or_tiler_job_mdg(h, mem, payload_ptr, job_no);

                        break;

                case JOB_TYPE_FRAGMENT:
                        payload_size = panwrap_replay_fragment_job(mem, payload_ptr, job_no, bifrost);
                        break;

                default:
                        break;
                }

                /* Touch the job descriptor fields, careful about 32/64-bit */
                TOUCH_JOB_HEADER(mem, jc_gpu_va, sizeof(*h), offset, job_no);

                /* Touch the payload immediately after, sequentially */
                TOUCH_SEQUENTIAL(mem, payload_ptr, payload_size, "payload", job_no);

                /* Handle linkage */

                if (!first) {
                        panwrap_log("((struct mali_job_descriptor_header *) (uintptr_t) job_%d_p)->", job_no - 1);

                        if (last_size)
                                panwrap_log_cont("next_job_64 = job_%d_p;\n\n", job_no);
                        else
                                panwrap_log_cont("next_job_32 = (u32) (uintptr_t) job_%d_p;\n\n", job_no);
                }

                first = false;


        } while ((jc_gpu_va = h->job_descriptor_size ? h->next_job_64 : h->next_job_32));

        return start_number;
}

static void
panwrap_replay_soft_replay_payload(mali_ptr jc_gpu_va, int job_no)
{
        struct base_jd_replay_payload *v;

        struct panwrap_mapped_memory *mem =
                panwrap_find_mapped_gpu_mem_containing(jc_gpu_va);

        v = PANWRAP_PTR(mem, jc_gpu_va, struct base_jd_replay_payload);

        panwrap_log("struct base_jd_replay_payload soft_replay_payload_%d = {\n", job_no);
        panwrap_indent++;

        MEMORY_PROP(v, tiler_jc_list);
        MEMORY_PROP(v, fragment_jc);
        MEMORY_PROP(v, tiler_heap_free);

        panwrap_prop("fragment_hierarchy_mask = 0x%" PRIx32, v->fragment_hierarchy_mask);
        panwrap_prop("tiler_hierarchy_mask = 0x%" PRIx32, v->tiler_hierarchy_mask);
        panwrap_prop("hierarchy_default_weight = 0x%" PRIx32, v->hierarchy_default_weight);

        panwrap_log(".tiler_core_req = ");

        if (v->tiler_core_req)
                ioctl_log_decoded_jd_core_req(v->tiler_core_req);
        else
                panwrap_log_cont("0");

        panwrap_log_cont(",\n");

        panwrap_log(".fragment_core_req = ");

        if (v->fragment_core_req)
                ioctl_log_decoded_jd_core_req(v->fragment_core_req);
        else
                panwrap_log_cont("0");

        panwrap_log_cont(",\n");

        panwrap_indent--;
        panwrap_log("};\n");

        TOUCH(mem, jc_gpu_va, *v, "soft_replay_payload", job_no, false);
}

int
panwrap_replay_soft_replay(mali_ptr jc_gpu_va)
{
        struct base_jd_replay_jc *v;
        int start_no;
        bool first = true;

        do {
                struct panwrap_mapped_memory *mem =
                        panwrap_find_mapped_gpu_mem_containing(jc_gpu_va);

                v = PANWRAP_PTR(mem, jc_gpu_va, struct base_jd_replay_jc);

                int job_no = job_descriptor_number++;

                if (first)
                        start_no = job_no;

                first = false;

                panwrap_log("struct base_jd_replay_jc job_%d = {\n", job_no);
                panwrap_indent++;

                MEMORY_PROP(v, next);
                MEMORY_PROP(v, jc);

                panwrap_indent--;
                panwrap_log("};\n");

                panwrap_replay_soft_replay_payload(jc_gpu_va /* + sizeof(struct base_jd_replay_jc) */, job_no);

                TOUCH(mem, jc_gpu_va, *v, "job", job_no, false);
        } while ((jc_gpu_va = v->next));

        return start_no;
}
