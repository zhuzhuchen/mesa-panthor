/*
 * Copyright (C) 2021 Collabora Ltd.
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "util/macros.h"
#include "compiler/shader_enums.h"

#include "panfrost-quirks.h"
#include "pan_encoder.h"
#include "pan_pool.h"

#include "panvk_cs.h"
#include "panvk_private.h"
#include "panvk_varyings.h"

static mali_pixel_format
panvk_varying_hw_format(const struct panvk_device *dev,
                        const struct panvk_varyings_info *varyings,
                        gl_shader_stage stage, unsigned idx)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   gl_varying_slot loc = varyings->stage[stage].loc[idx];
   bool fs = stage == MESA_SHADER_FRAGMENT;

   switch (loc) {
   case VARYING_SLOT_PNTC:
   case VARYING_SLOT_PSIZ:
      return (MALI_R16F << 12) |
             (pdev->quirks & HAS_SWIZZLES ?
              panfrost_get_default_swizzle(1) :
              panfrost_bifrost_swizzle(1));
   case VARYING_SLOT_POS:
      return ((fs ? MALI_RGBA32F : MALI_SNAP_4) << 12) |
             (pdev->quirks & HAS_SWIZZLES ?
              panfrost_get_default_swizzle(4) :
              panfrost_bifrost_swizzle(4));
   default:
      assert(!panvk_varying_is_builtin(stage, loc));
      return pdev->formats[varyings->varying[loc].format].hw;
   }
}

static void
panvk_emit_varying(const struct panvk_device *dev,
                   const struct panvk_varyings_info *varyings,
                   gl_shader_stage stage, unsigned idx,
                   void *attrib)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   gl_varying_slot loc = varyings->stage[stage].loc[idx];
   bool fs = stage == MESA_SHADER_FRAGMENT;

   pan_pack(attrib, ATTRIBUTE, cfg) {
      if (!panvk_varying_is_builtin(stage, loc)) {
         cfg.buffer_index = varyings->varying[loc].buf;
         cfg.offset = varyings->varying[loc].offset;
      } else {
         cfg.buffer_index =
            panvk_varying_buf_index(varyings,
                                    panvk_varying_buf_id(fs, loc));
         cfg.offset = 0;
      }
      cfg.offset_enable = !pan_is_bifrost(pdev);
      cfg.format = panvk_varying_hw_format(dev, varyings, stage, idx);
   }
}

void
panvk_emit_varyings(const struct panvk_device *dev,
                    const struct panvk_varyings_info *varyings,
                    gl_shader_stage stage,
                    void *descs)
{
   struct mali_attribute_packed *attrib = descs;

   for (unsigned i = 0; i < varyings->stage[stage].count; i++)
      panvk_emit_varying(dev, varyings, stage, i, attrib++);
}

static void
panvk_emit_varying_buf(const struct panvk_device *dev,
                       const struct panvk_varyings_info *varyings,
                       enum panvk_varying_buf_id id, void *buf)
{
   unsigned buf_idx = panvk_varying_buf_index(varyings, id);
   enum mali_attribute_special special_id = panvk_varying_special_buf_id(id);

   pan_pack(buf, ATTRIBUTE_BUFFER, cfg) {
      if (special_id) {
         cfg.type = 0;
         cfg.special = special_id;
      } else {
         unsigned offset = varyings->buf[buf_idx].address & 63;

         cfg.stride = varyings->buf[buf_idx].stride;
         cfg.size = varyings->buf[buf_idx].size + offset;
         cfg.pointer = varyings->buf[buf_idx].address & ~63ULL;
      }
   }
}

void
panvk_emit_varying_bufs(const struct panvk_device *dev,
                        const struct panvk_varyings_info *varyings,
                        void *descs)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   struct mali_attribute_buffer_packed *buf = descs;

   for (unsigned i = 0; i < PANVK_VARY_BUF_MAX; i++) {
      if (varyings->buf_mask & (1 << i))
         panvk_emit_varying_buf(dev, varyings, i, buf++);
   }

   if (pan_is_bifrost(pdev))
      memset(buf, 0, sizeof(*buf));
}

static void
panvk_emit_attrib_buf(const struct panvk_device *dev,
                      const struct panvk_attribs_info *attribs,
                      const struct panvk_draw_info *draw,
                      unsigned idx, void *desc)
{
   ASSERTED const struct panfrost_device *pdev = &dev->physical_device->pdev;
   const struct panvk_attrib_buf *buf = &attribs->buf[idx];

   if (buf->special) {
      assert(!pan_is_bifrost(pdev));
      switch (buf->special_id) {
      case PAN_VERTEX_ID:
         panfrost_vertex_id(draw->padded_vertex_count, desc,
                            draw->instance_count > 1);
         return;
      case PAN_INSTANCE_ID:
         panfrost_instance_id(draw->padded_vertex_count, desc,
                              draw->instance_count > 1);
         return;
      default:
         unreachable("Invalid attribute ID\n");
      }
   }

   unsigned divisor = buf->per_instance ?
                      draw->padded_vertex_count : 0;
   unsigned stride = divisor && draw->instance_count == 1 ?
                     0 : buf->stride;
   mali_ptr addr = buf->address & ~63ULL;
   unsigned size = buf->size + (buf->address & 63);

   pan_pack(desc, ATTRIBUTE_BUFFER, cfg) {
      if (draw->instance_count > 1 && divisor) {
         cfg.type = MALI_ATTRIBUTE_TYPE_1D_MODULUS;
         cfg.divisor = divisor;
      }

      cfg.pointer = addr;
      cfg.stride = stride;
      cfg.size = size;
   }
}

void
panvk_emit_attrib_bufs(const struct panvk_device *dev,
                       const struct panvk_attribs_info *attribs,
                       const struct panvk_draw_info *draw,
                       void *descs)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   struct mali_attribute_buffer_packed *buf = descs;

   for (unsigned i = 0; i < attribs->buf_count; i++)
      panvk_emit_attrib_buf(dev, attribs, draw, i, buf++);

   if (pan_is_bifrost(pdev))
      memset(buf, 0, sizeof(*buf));
}

static void
panvk_emit_attrib(const struct panvk_device *dev,
                  const struct panvk_attribs_info *attribs,
                  unsigned idx, void *attrib)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;

   pan_pack(attrib, ATTRIBUTE, cfg) {
      cfg.buffer_index = attribs->attrib[idx].buf;
      cfg.offset = attribs->attrib[idx].offset +
                   (attribs->buf[cfg.buffer_index].address & 63);
      cfg.format = pdev->formats[attribs->attrib[idx].format].hw;
      cfg.offset_enable = !pan_is_bifrost(pdev);
   }
}

void
panvk_emit_attribs(const struct panvk_device *dev,
                   const struct panvk_attribs_info *attribs,
                   void *descs)
{
   struct mali_attribute_packed *attrib = descs;

   for (unsigned i = 0; i < attribs->attrib_count; i++)
      panvk_emit_attrib(dev, attribs, i, attrib++);
}

void
panvk_emit_ubos(const struct panvk_pipeline *pipeline,
                const struct panvk_descriptor_state *state,
                void *descs)
{
   struct mali_uniform_buffer_packed *ubos = descs;

   for (unsigned i = 0; i < ARRAY_SIZE(state->sets); i++) {
      const struct panvk_descriptor_set_layout *set_layout =
         pipeline->layout->sets[i].layout;
      const struct panvk_descriptor_set *set = state->sets[i].set;
      unsigned offset = pipeline->layout->sets[i].ubo_offset;

      if (!set_layout)
         continue;

      if (!set) {
         unsigned num_ubos = (set_layout->num_dynoffsets != 0) + set_layout->num_ubos;
         memset(&ubos[offset], 0, num_ubos * sizeof(*ubos));
      } else {
         memcpy(&ubos[offset], set->ubos, set_layout->num_ubos * sizeof(*ubos));
         if (set_layout->num_dynoffsets) {
            pan_pack(&ubos[offset + set_layout->num_ubos], UNIFORM_BUFFER, cfg) {
               cfg.pointer = state->sets[i].dynoffsets.gpu;
               cfg.entries = DIV_ROUND_UP(set->layout->num_dynoffsets, 16);
            }
         }
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(pipeline->sysvals); i++) {
      if (!pipeline->sysvals[i].ids.sysval_count)
         continue;

      pan_pack(&ubos[pipeline->sysvals[i].ubo_idx], UNIFORM_BUFFER, cfg) {
         cfg.pointer = pipeline->sysvals[i].ubo ? :
                       state->sysvals[i].ubo;
         cfg.entries = pipeline->sysvals[i].ids.sysval_count;
      }
   }
}

static void
panvk_prepare_draw_desc(const struct panvk_pipeline *pipeline,
                        const struct panvk_draw_info *draw,
                        gl_shader_stage stage,
                        struct MALI_DRAW *desc)
{
   desc->offset_start = draw->offset_start;
   desc->instance_size = draw->instance_count > 1 ?
                         draw->padded_vertex_count : 1;
   desc->uniform_buffers = draw->ubos;
   desc->push_uniforms = draw->push_constants[stage];
   desc->textures = draw->textures;
   desc->samplers = draw->samplers;
}

void
panvk_emit_vertex_job(const struct panvk_device *dev,
                      const struct panvk_pipeline *pipeline,
                      const struct panvk_draw_info *draw,
                      void *job)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   void *section = pan_section_ptr(job, COMPUTE_JOB, INVOCATION);

   memcpy(section, &draw->invocation, MALI_INVOCATION_LENGTH);

   pan_section_pack(job, COMPUTE_JOB, PARAMETERS, cfg) {
      cfg.job_task_split = 5;
   }

   pan_section_pack(job, COMPUTE_JOB, DRAW, cfg) {
      cfg.draw_descriptor_is_64b = true;
      if (!pan_is_bifrost(pdev))
         cfg.texture_descriptor_is_64b = true;
      cfg.state = pipeline->rsds[MESA_SHADER_VERTEX];
      cfg.attributes = draw->attributes[MESA_SHADER_VERTEX];
      cfg.attribute_buffers = draw->attribute_bufs;
      cfg.varyings = draw->varyings[MESA_SHADER_VERTEX];
      cfg.varying_buffers = draw->varying_bufs;
      cfg.thread_storage = draw->tls;
      panvk_prepare_draw_desc(pipeline, draw, PIPE_SHADER_VERTEX, &cfg);
   }

   pan_section_pack(job, COMPUTE_JOB, DRAW_PADDING, cfg);
}

void
panvk_emit_tiler_job(const struct panvk_device *dev,
                     const struct panvk_pipeline *pipeline,
                     const struct panvk_draw_info *draw,
                     void *job)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   void *section = pan_is_bifrost(pdev) ?
                   pan_section_ptr(job, BIFROST_TILER_JOB, INVOCATION) :
                   pan_section_ptr(job, MIDGARD_TILER_JOB, INVOCATION);

   memcpy(section, &draw->invocation, MALI_INVOCATION_LENGTH);

   section = pan_is_bifrost(pdev) ?
             pan_section_ptr(job, BIFROST_TILER_JOB, PRIMITIVE) :
             pan_section_ptr(job, MIDGARD_TILER_JOB, PRIMITIVE);

   pan_pack(section, PRIMITIVE, cfg) {
      cfg.draw_mode = pipeline->ia.topology;
      if (pipeline->ia.writes_point_size)
         cfg.point_size_array_format = MALI_POINT_SIZE_ARRAY_FORMAT_FP16;

//      cfg.first_provoking_vertex = true;
      cfg.first_provoking_vertex = true;
      if (pipeline->ia.primitive_restart)
         cfg.primitive_restart = MALI_PRIMITIVE_RESTART_IMPLICIT;
      cfg.job_task_split = 6;
      // TODO: indexed draws
      cfg.index_count = draw->vertex_count;
   }

   section = pan_is_bifrost(pdev) ?
             pan_section_ptr(job, BIFROST_TILER_JOB, PRIMITIVE_SIZE) :
             pan_section_ptr(job, MIDGARD_TILER_JOB, PRIMITIVE_SIZE);
   pan_pack(section, PRIMITIVE_SIZE, cfg) {
      if (pipeline->ia.writes_point_size) {
         cfg.size_array = draw->psiz;
      } else {
         cfg.constant = draw->line_width;
      }
   }

   section = pan_is_bifrost(pdev) ?
             pan_section_ptr(job, BIFROST_TILER_JOB, DRAW) :
             pan_section_ptr(job, MIDGARD_TILER_JOB, DRAW);

   pan_pack(section, DRAW, cfg) {
      cfg.four_components_per_vertex = true;
      cfg.draw_descriptor_is_64b = true;
      if (!pan_is_bifrost(pdev))
         cfg.texture_descriptor_is_64b = true;
      cfg.front_face_ccw = pipeline->rast.front_ccw;
      cfg.cull_front_face = pipeline->rast.cull_front_face;
      cfg.cull_back_face = pipeline->rast.cull_back_face;
      cfg.position = draw->position;
      cfg.state = draw->fs_rsd;
      cfg.attributes = draw->attributes[MESA_SHADER_FRAGMENT];
      cfg.attribute_buffers = draw->attribute_bufs;
      cfg.viewport = draw->viewport;
      cfg.varyings = draw->varyings[MESA_SHADER_FRAGMENT];
      cfg.varying_buffers = cfg.varyings ? draw->varying_bufs : 0;
      cfg.thread_storage = draw->tls;

      /* For all primitives but lines DRAW.flat_shading_vertex must
       * be set to 0 and the provoking vertex is selected with the
       * PRIMITIVE.first_provoking_vertex field.
       */
      if (pipeline->ia.topology == MALI_DRAW_MODE_LINES ||
          pipeline->ia.topology == MALI_DRAW_MODE_LINE_STRIP ||
          pipeline->ia.topology == MALI_DRAW_MODE_LINE_LOOP) {
         /* The logic is inverted on bifrost. */
         cfg.flat_shading_vertex = pan_is_bifrost(pdev) ?
                                   true : false;
      }

      panvk_prepare_draw_desc(pipeline, draw, PIPE_SHADER_FRAGMENT, &cfg);

      // TODO: occlusion queries
   }

   if (pan_is_bifrost(pdev)) {
      pan_section_pack(job, BIFROST_TILER_JOB, TILER, cfg) {
         cfg.address = draw->tiler;
      }
      pan_section_pack(job, BIFROST_TILER_JOB, DRAW_PADDING, padding);
      pan_section_pack(job, BIFROST_TILER_JOB, PADDING, padding);
   }
}

void
panvk_emit_fragment_job(const struct panvk_device *dev,
                        const struct panvk_framebuffer *fb,
                        mali_ptr fbdesc,
                        void *job)
{
   pan_section_pack(job, FRAGMENT_JOB, HEADER, header) {
      header.type = MALI_JOB_TYPE_FRAGMENT;
      header.index = 1;
   }

   pan_section_pack(job, FRAGMENT_JOB, PAYLOAD, payload) {
      payload.bound_min_x = 0;
      payload.bound_min_y = 0;

      payload.bound_max_x = (fb->width - 1) >> MALI_TILE_SHIFT;
      payload.bound_max_y = (fb->height - 1) >> MALI_TILE_SHIFT;
      payload.framebuffer = fbdesc;
   }
}

void
panvk_emit_viewport(const VkViewport *viewport, const VkRect2D *scissor,
                    void *vpd)
{
   pan_pack(vpd, VIEWPORT, cfg) {
      cfg.scissor_minimum_x = MAX2(scissor->offset.x, (int)viewport->x);
      cfg.scissor_minimum_y = MAX2(scissor->offset.y, (int)viewport->y);
      cfg.scissor_maximum_x = MIN2(scissor->offset.x + scissor->extent.width - 1,
                                   (int)(viewport->x + viewport->width - 1));
      cfg.scissor_maximum_y = MIN2(scissor->offset.y + scissor->extent.height - 1,
                                   (int)(viewport->y + viewport->height - 1));
      cfg.minimum_z = viewport->minDepth;
      cfg.maximum_z = viewport->maxDepth;
   }
}

void
panvk_sysval_upload_viewport_scale(const VkViewport *viewport,
                                   union panvk_sysval_data *data)
{
   data->f32[0] = 0.5f * viewport->width;
   data->f32[1] = 0.5f * viewport->height;
   data->f32[2] = 0.5f * (viewport->maxDepth - viewport->minDepth);
}

void
panvk_sysval_upload_viewport_offset(const VkViewport *viewport,
                                    union panvk_sysval_data *data)
{
   data->f32[0] = (0.5f * viewport->width) + viewport->x;
   data->f32[1] = (0.5f * viewport->height) + viewport->y;
   data->f32[2] = (0.5f * (viewport->maxDepth - viewport->minDepth)) + viewport->minDepth;
}

static void
panvk_prepare_bifrost_fs_rsd(const struct panvk_device *dev,
                             const struct panvk_pipeline *pipeline,
                             const struct pan_blend_state *blend,
                             struct MALI_RENDERER_STATE *rsd)
{
   if (!pipeline->fs.required) {
      rsd->properties.uniform_buffer_count = 32;
      rsd->properties.bifrost.shader_modifies_coverage = true;
      rsd->properties.bifrost.allow_forward_pixel_to_kill = true;
      rsd->properties.bifrost.allow_forward_pixel_to_be_killed = true;
      rsd->properties.bifrost.zs_update_operation = MALI_PIXEL_KILL_STRONG_EARLY;
   } else {
      bool no_blend = true;
      for (unsigned i = 0; i < blend->rt_count; i++) {
         if (pan_blend_reads_dest(blend, i) &&
             blend->rts[i].equation.color_mask) {
            no_blend = false;
            break;
         }
      }

      const struct pan_shader_info *info = &pipeline->fs.info;

      rsd->properties.bifrost.allow_forward_pixel_to_kill =
         !info->fs.can_discard && !info->fs.writes_depth && no_blend;

      const struct panfrost_device *pdev = &dev->physical_device->pdev;
      pan_shader_prepare_rsd(pdev, info, pipeline->fs.address, rsd);
   }
}

static void
panvk_prepare_fs_rsd(const struct panvk_device *dev,
                     const struct panvk_pipeline *pipeline,
                     const struct panvk_cmd_state *state,
                     struct MALI_RENDERER_STATE *rsd)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   const struct pan_blend_state *blend =
      pipeline->dynamic_state_mask & (1 << PANVK_DYNAMIC_BLEND_CONSTANTS) ?
      &state->blend : &pipeline->blend;

   if (pan_is_bifrost(pdev))
      panvk_prepare_bifrost_fs_rsd(dev, pipeline, blend, rsd);
   else
      assert(0);

   bool msaa = pipeline->ms.rast_samples > 1;
   rsd->multisample_misc.multisample_enable = msaa;
   rsd->multisample_misc.sample_mask =
      msaa ? pipeline->ms.sample_mask : UINT16_MAX;

   /* EXT_shader_framebuffer_fetch requires per-sample */
   bool per_sample = pipeline->ms.min_samples > 1 ||
                     pipeline->fs.info.fs.outputs_read;
   rsd->multisample_misc.evaluate_per_sample = msaa && per_sample;
   rsd->multisample_misc.depth_function =
      pipeline->zs.z_test ? pipeline->zs.z_compare_func : MALI_FUNC_ALWAYS;

   rsd->multisample_misc.depth_write_mask = pipeline->zs.z_write;
   rsd->multisample_misc.fixed_function_near_discard = !pipeline->rast.clamp_depth;
   rsd->multisample_misc.fixed_function_far_discard = !pipeline->rast.clamp_depth;
   rsd->multisample_misc.shader_depth_range_fixed = true;

   rsd->stencil_mask_misc.stencil_enable = pipeline->zs.s_test;
   rsd->stencil_mask_misc.alpha_to_coverage = pipeline->ms.alpha_to_coverage;
   rsd->stencil_mask_misc.alpha_test_compare_function = MALI_FUNC_ALWAYS;
   rsd->stencil_mask_misc.depth_range_1 = pipeline->rast.depth_bias.enable;
   rsd->stencil_mask_misc.depth_range_2 = pipeline->rast.depth_bias.enable;
   rsd->stencil_mask_misc.single_sampled_lines = pipeline->ms.rast_samples <= 1;

   if (pipeline->dynamic_state_mask & (1 << VK_DYNAMIC_STATE_DEPTH_BIAS)) {
      rsd->depth_units = state->rast.depth_bias.constant_factor * 2.0f;
      rsd->depth_factor = state->rast.depth_bias.slope_factor;
      rsd->depth_bias_clamp = state->rast.depth_bias.clamp;
   } else {
      rsd->depth_units = pipeline->rast.depth_bias.constant_factor * 2.0f;
      rsd->depth_factor = pipeline->rast.depth_bias.slope_factor;
      rsd->depth_bias_clamp = pipeline->rast.depth_bias.clamp;
   }

   if (pipeline->dynamic_state_mask & (1 << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
      rsd->stencil_front.mask = state->zs.s_front.compare_mask;
      rsd->stencil_back.mask = state->zs.s_back.compare_mask;
   } else {
      rsd->stencil_front.mask = pipeline->zs.s_front.compare_mask;
      rsd->stencil_back.mask = pipeline->zs.s_back.compare_mask;
   }

   if (pipeline->dynamic_state_mask & (1 << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
      rsd->stencil_mask_misc.stencil_mask_front = state->zs.s_front.write_mask;
      rsd->stencil_mask_misc.stencil_mask_back = state->zs.s_back.write_mask;
   } else {
      rsd->stencil_mask_misc.stencil_mask_front = pipeline->zs.s_front.write_mask;
      rsd->stencil_mask_misc.stencil_mask_back = pipeline->zs.s_back.write_mask;
   }

   if (pipeline->dynamic_state_mask & (1 << VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
      rsd->stencil_front.reference_value = state->zs.s_front.ref;
      rsd->stencil_back.reference_value = state->zs.s_back.ref;
   } else {
      rsd->stencil_front.reference_value = pipeline->zs.s_front.ref;
      rsd->stencil_back.reference_value = pipeline->zs.s_back.ref;
   }

   rsd->stencil_front.compare_function = pipeline->zs.s_front.compare_func;
   rsd->stencil_front.stencil_fail = pipeline->zs.s_front.fail_op;
   rsd->stencil_front.depth_fail = pipeline->zs.s_front.z_fail_op;
   rsd->stencil_front.depth_pass = pipeline->zs.s_front.pass_op;
   rsd->stencil_back.compare_function = pipeline->zs.s_back.compare_func;
   rsd->stencil_back.stencil_fail = pipeline->zs.s_back.fail_op;
   rsd->stencil_back.depth_fail = pipeline->zs.s_back.z_fail_op;
   rsd->stencil_back.depth_pass = pipeline->zs.s_back.pass_op;
}

static enum mali_bifrost_register_file_format
bifrost_blend_type_from_nir(nir_alu_type nir_type)
{
   switch(nir_type) {
   case 0: /* Render target not in use */
      return 0;
   case nir_type_float16:
      return MALI_BIFROST_REGISTER_FILE_FORMAT_F16;
   case nir_type_float32:
      return MALI_BIFROST_REGISTER_FILE_FORMAT_F32;
   case nir_type_int32:
      return MALI_BIFROST_REGISTER_FILE_FORMAT_I32;
   case nir_type_uint32:
      return MALI_BIFROST_REGISTER_FILE_FORMAT_U32;
   case nir_type_int16:
      return MALI_BIFROST_REGISTER_FILE_FORMAT_I16;
   case nir_type_uint16:
      return MALI_BIFROST_REGISTER_FILE_FORMAT_U16;
   default:
      unreachable("Unsupported blend shader type for NIR alu type");
   }
}

static void
panvk_prepare_bifrost_blend(const struct panvk_device *dev,
                            const struct panvk_pipeline *pipeline,
                            const struct pan_blend_state *blend,
                            mali_ptr blend_shader,
                            unsigned rt, void *bd)
{
   if (!blend->rt_count) {
      /* Disable blending for depth-only */
      pan_pack(bd, BLEND, cfg) {
         cfg.enable = false;
         cfg.bifrost.internal.mode = MALI_BIFROST_BLEND_MODE_OFF;
      }
      return;
   }

   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   const struct pan_blend_rt_state *rts = &blend->rts[rt];

   pan_pack(bd, BLEND, cfg) {
      if (!rts->equation.color_mask) {
         cfg.enable = false;
      } else {
         cfg.srgb = util_format_is_srgb(rts->format);
         cfg.load_destination = pan_blend_reads_dest(blend, rt);
         cfg.round_to_fb_precision = !blend->dither;
      }

      if (blend_shader) {
         assert((blend_shader & (0xffffffffull << 32)) ==
         (pipeline->fs.address & (0xffffffffull << 32)));
         cfg.bifrost.internal.shader.pc = (u32)blend_shader;
         assert(!(pipeline->fs.info.bifrost.blend[rt].return_offset & 0x7));
         if (pipeline->fs.info.bifrost.blend[rt].return_offset) {
            cfg.bifrost.internal.shader.return_value =
               (pipeline->fs.address & UINT32_MAX) +
               pipeline->fs.info.bifrost.blend[rt].return_offset;
         }
         cfg.bifrost.internal.mode = MALI_BIFROST_BLEND_MODE_SHADER;
      } else {
         const struct util_format_description *format_desc =
            util_format_description(rts->format);
         unsigned chan_size = 0;
         for (unsigned i = 0; i < format_desc->nr_channels; i++)
            chan_size = MAX2(format_desc->channel[0].size, chan_size);

         pan_blend_to_fixed_function_equation(pdev, blend, rt, &cfg.bifrost.equation);

         /* Fixed point constant */
         float fconst = pan_blend_get_constant(pdev, blend, rt);
         u16 constant = fconst * ((1 << chan_size) - 1);
         constant <<= 16 - chan_size;
         cfg.bifrost.constant = constant;

         if (pan_blend_is_opaque(blend, rt))
            cfg.bifrost.internal.mode = MALI_BIFROST_BLEND_MODE_OPAQUE;
         else
            cfg.bifrost.internal.mode = MALI_BIFROST_BLEND_MODE_FIXED_FUNCTION;

         /* If we want the conversion to work properly,
          * num_comps must be set to 4
          */
         cfg.bifrost.internal.fixed_function.num_comps = 4;
         cfg.bifrost.internal.fixed_function.conversion.memory_format =
            panfrost_format_to_bifrost_blend(pdev, format_desc, true);
         cfg.bifrost.internal.fixed_function.conversion.register_format =
            bifrost_blend_type_from_nir(pipeline->fs.info.bifrost.blend[rt].type);
         cfg.bifrost.internal.fixed_function.rt = rt;
      }
   }
}

static void
panvk_emit_blend(const struct panvk_device *dev,
                 const struct panvk_pipeline *pipeline,
                 const struct pan_blend_state *blend,
                 const mali_ptr blend_shader,
                 unsigned rt, void *bd)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;

   if (pan_is_bifrost(pdev))
      panvk_prepare_bifrost_blend(dev, pipeline, blend, blend_shader, rt, bd);
   else
      assert(0);
}

void
panvk_emit_fs_rsd(const struct panvk_device *dev,
                  const struct panvk_pipeline *pipeline,
                  const struct panvk_cmd_state *state,
                  void *rsd)
{
   pan_pack(rsd, RENDERER_STATE, cfg) {
      panvk_prepare_fs_rsd(dev, pipeline, state, &cfg);
   }

   const struct panfrost_device *pdev = &dev->physical_device->pdev;

   if (pdev->quirks & MIDGARD_SFBD)
      return;

   void *bd = rsd + MALI_RENDERER_STATE_LENGTH;
   const struct pan_blend_state *blend =
      pipeline->dynamic_state_mask & (1 << PANVK_DYNAMIC_BLEND_CONSTANTS) ?
      &state->blend : &pipeline->blend;

   for (unsigned i = 0; i < MAX2(blend->rt_count, 1); i++) {
      mali_ptr blend_shader = (state && state->blend_shaders[i]) ?
                              state->blend_shaders[i] :
                              pipeline->blend_shaders[i].address;
      panvk_emit_blend(dev, pipeline, blend, blend_shader, i, bd);
      bd += MALI_BLEND_LENGTH;
   }
}

void
panvk_emit_non_fs_rsd(const struct panvk_device *dev,
                      const struct pan_shader_info *shader_info,
                      mali_ptr shader_ptr,
                      void *rsd)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;

   assert(shader_info->stage != MESA_SHADER_FRAGMENT);

   pan_pack(rsd, RENDERER_STATE, cfg) {
      pan_shader_prepare_rsd(pdev, shader_info, shader_ptr, &cfg);
   }
}

void
panvk_emit_bifrost_tiler_context(const struct panvk_device *dev,
                                 const struct panvk_framebuffer *fb,
                                 const struct panfrost_ptr *descs)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;

   pan_pack(descs->cpu + MALI_BIFROST_TILER_LENGTH, BIFROST_TILER_HEAP, cfg) {
      cfg.size = pdev->tiler_heap->size;
      cfg.base = pdev->tiler_heap->ptr.gpu;
      cfg.bottom = pdev->tiler_heap->ptr.gpu;
      cfg.top = pdev->tiler_heap->ptr.gpu + pdev->tiler_heap->size;
   }

   pan_pack(descs->cpu, BIFROST_TILER, cfg) {
      cfg.hierarchy_mask = 0x28;
      cfg.fb_width = fb->width;
      cfg.fb_height = fb->height;
      cfg.heap = descs->gpu + MALI_BIFROST_TILER_LENGTH;
   }
}

static void
panvk_emit_sfb(const struct panvk_device *dev,
               const struct panvk_batch *batch,
               const struct panvk_subpass *subpass,
               const struct panvk_pipeline *pipeline,
               const struct panvk_framebuffer *fb,
               const struct panvk_clear_value *clears,
               void *desc)
{
   assert(0);
}

static unsigned
bytes_per_pixel_tib(enum pipe_format format)
{
   if (panfrost_blend_format(format).internal) {
      /* Blendable formats are always 32-bits in the tile buffer,
       * extra bits are used as padding or to dither */
      return 4;
   } else {
      /* Non-blendable formats are raw, rounded up to the nearest
       * power-of-two size */
      unsigned bytes = util_format_get_blocksize(format);
      return util_next_power_of_two(bytes);
   }
}

static unsigned
get_internal_cbuf_size(const struct panvk_subpass *subpass,
                       const struct panvk_framebuffer *fb,
                       unsigned *tile_size)
{
   unsigned total_size = 0;

   *tile_size = 16 * 16;
   for (int cb = 0; cb < subpass->color_count; cb++) {
      if (subpass->color_attachments[cb].idx == VK_ATTACHMENT_UNUSED)
         continue;

      const struct panvk_attachment_info *info =
         &fb->attachments[subpass->color_attachments[cb].idx];
      const struct panvk_image_view *iview = info->iview;

      unsigned nr_samples = iview->image->samples;
      total_size += bytes_per_pixel_tib(iview->format) *
                    nr_samples * (*tile_size);
   }

   /* We have a 4KB budget, let's reduce the tile size until it fits. */
   while (total_size > 4096) {
      total_size >>= 1;
      *tile_size >>= 1;
   }

   /* Align on 1k. */
   total_size = ALIGN_POT(total_size, 1024);

   /* Minimum tile size is 4x4. */
   assert(*tile_size >= 4 * 4);
   return total_size;
}

static enum mali_mfbd_color_format
panvk_raw_format(unsigned bits)
{
   switch (bits) {
   case    8: return MALI_MFBD_COLOR_FORMAT_RAW8;
   case   16: return MALI_MFBD_COLOR_FORMAT_RAW16;
   case   24: return MALI_MFBD_COLOR_FORMAT_RAW24;
   case   32: return MALI_MFBD_COLOR_FORMAT_RAW32;
   case   48: return MALI_MFBD_COLOR_FORMAT_RAW48;
   case   64: return MALI_MFBD_COLOR_FORMAT_RAW64;
   case   96: return MALI_MFBD_COLOR_FORMAT_RAW96;
   case  128: return MALI_MFBD_COLOR_FORMAT_RAW128;
   case  192: return MALI_MFBD_COLOR_FORMAT_RAW192;
   case  256: return MALI_MFBD_COLOR_FORMAT_RAW256;
   case  384: return MALI_MFBD_COLOR_FORMAT_RAW384;
   case  512: return MALI_MFBD_COLOR_FORMAT_RAW512;
   case  768: return MALI_MFBD_COLOR_FORMAT_RAW768;
   case 1024: return MALI_MFBD_COLOR_FORMAT_RAW1024;
   case 1536: return MALI_MFBD_COLOR_FORMAT_RAW1536;
   case 2048: return MALI_MFBD_COLOR_FORMAT_RAW2048;
   default: unreachable("invalid raw bpp");
   }
}

static void
panvk_rt_set_format(const struct panvk_device *dev,
                    const struct panvk_image_view *iview,
                    struct MALI_RENDER_TARGET *rt)
{
   const struct util_format_description *desc =
      util_format_description(iview->format);

   unsigned char swizzle[4];
   panfrost_invert_swizzle(desc->swizzle, swizzle);
   rt->swizzle = panfrost_translate_swizzle_4(swizzle);

   if (desc->colorspace == UTIL_FORMAT_COLORSPACE_SRGB)
      rt->srgb = true;

   struct pan_blendable_format fmt = panfrost_blend_format(iview->format);

   if (fmt.internal) {
      rt->internal_format = fmt.internal;
      rt->writeback_format = fmt.writeback;
   } else {
      /* Construct RAW internal/writeback, where internal is
       * specified logarithmically (round to next power-of-two).
       * Offset specified from RAW8, where 8 = 2^3 */

      unsigned bits = desc->block.bits;
      unsigned offset = util_logbase2_ceil(bits) - 3;
      assert(offset <= 4);

      rt->internal_format =
         MALI_COLOR_BUFFER_INTERNAL_FORMAT_RAW8 + offset;

      rt->writeback_format = panvk_raw_format(bits);
   }
}

static void
panvk_rt_set_buf(const struct panvk_device *dev,
                 const struct panvk_image_view *iview,
                 struct MALI_RENDER_TARGET *rt)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;

   /* FIXME */
   rt->writeback_msaa = MALI_MSAA_SINGLE;
   mali_ptr base = iview->image->memory.planes[0].bo->ptr.gpu +
                   iview->image->memory.planes[0].offset;

   if (pdev->arch >= 7)
      rt->bifrost_v7.writeback_block_format = MALI_BLOCK_FORMAT_V7_LINEAR;
   else
      rt->midgard.writeback_block_format = MALI_BLOCK_FORMAT_LINEAR;

   rt->rgb.base = base;
   rt->rgb.row_stride = iview->image->layout.planes[0].slices[0].line_stride;
   rt->rgb.surface_stride = 0;
}

static void
panvk_emit_rt(const struct panvk_device *dev,
              const struct panvk_subpass *subpass,
              const struct panvk_pipeline *pipeline,
              const struct panvk_framebuffer *fb,
              const struct panvk_clear_value *clears,
              unsigned rt, unsigned cbuf_offset,
              void *desc)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   const struct panvk_image_view *iview = NULL;
   const struct panvk_clear_value *clear = NULL;

   if (subpass->color_attachments[rt].idx != VK_ATTACHMENT_UNUSED) {
      iview = fb->attachments[subpass->color_attachments[rt].idx].iview;
      if (subpass->color_attachments[rt].clear)
         clear = &clears[subpass->color_attachments[rt].idx];
   }

   pan_pack(desc, RENDER_TARGET, cfg) {
      cfg.clean_pixel_write_enable = true;
      if (iview) {
         cfg.write_enable = true;
         cfg.dithering_enable = true;
         cfg.internal_buffer_offset = cbuf_offset;
         /* FIXME */
         panvk_rt_set_format(dev, iview, &cfg);
         panvk_rt_set_buf(dev, iview, &cfg);
//         panfrost_mfbd_rt_set_buf(surf, &rt);
      } else {
         cfg.internal_format = MALI_COLOR_BUFFER_INTERNAL_FORMAT_R8G8B8A8;
         cfg.internal_buffer_offset = cbuf_offset;
         if (pdev->arch >= 7) {
            cfg.bifrost_v7.writeback_block_format = MALI_BLOCK_FORMAT_V7_TILED_U_INTERLEAVED;
            cfg.dithering_enable = true;
         }
      }

      if (clear) {
         cfg.clear.color_0 = clear->color[0];
         cfg.clear.color_1 = clear->color[1];
         cfg.clear.color_2 = clear->color[2];
         cfg.clear.color_3 = clear->color[3];
      }
   }
}

static void
panvk_emit_zs_crc(const struct panvk_device *dev,
                  const struct panvk_subpass *subpass,
                  const struct panvk_pipeline *pipeline,
                  const struct panvk_framebuffer *fb,
                  void *desc)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   const struct panvk_attachment_info *info =
      &fb->attachments[subpass->zs_attachment.idx];
   const struct panvk_image_view *iview = info->iview;

   /* TODO: AFBC, tiled (and a lot more to fix) */
   pan_pack(desc, ZS_CRC_EXTENSION, ext) {
      ext.zs_clean_pixel_write_enable = true;
      if (pdev->arch < 7)
         ext.zs_msaa = MALI_MSAA_SINGLE;
      else
         ext.zs_msaa_v7 = MALI_MSAA_SINGLE;
      assert(iview->image->modifier == DRM_FORMAT_MOD_LINEAR);
      mali_ptr base = iview->image->memory.planes[0].bo->ptr.gpu +
                      iview->image->memory.planes[0].offset;

      ext.zs_writeback_base = base;
      ext.zs_writeback_row_stride =
         iview->image->layout.planes[0].slices[0].line_stride;
      ext.zs_writeback_surface_stride = 0;
      if (pdev->arch >= 7)
         ext.zs_block_format_v7 = MALI_BLOCK_FORMAT_V7_LINEAR;
      else
         ext.zs_block_format = MALI_BLOCK_FORMAT_LINEAR;

      switch (iview->format) {
      case PIPE_FORMAT_Z16_UNORM:
         ext.zs_write_format = MALI_ZS_FORMAT_D16;
         break;
      case PIPE_FORMAT_Z24_UNORM_S8_UINT:
         ext.zs_write_format = MALI_ZS_FORMAT_D24S8;
         ext.s_writeback_base = ext.zs_writeback_base;
         break;
      case PIPE_FORMAT_Z24X8_UNORM:
         ext.zs_write_format = MALI_ZS_FORMAT_D24X8;
         break;
      case PIPE_FORMAT_Z32_FLOAT:
         ext.zs_write_format = MALI_ZS_FORMAT_D32;
         break;
      default:
         unreachable("Unsupported depth/stencil format.");
      }
   }
}

static enum mali_z_internal_format
get_z_internal_format(const struct panvk_subpass *subpass,
                      const struct panvk_framebuffer *fb)
{
   const struct panvk_image_view *iview =
      subpass->zs_attachment.idx != VK_ATTACHMENT_UNUSED ?
      fb->attachments[subpass->zs_attachment.idx].iview : NULL;
      
   /* Default to 24 bit depth if there's no surface. */
   if (!iview)
      return MALI_Z_INTERNAL_FORMAT_D24;

   return panfrost_get_z_internal_format(iview->format);
}

static void
panvk_emit_bifrost_mfb_sections(const struct panvk_device *dev,
                                const struct panvk_batch *batch,
                                void *desc)
{
   struct panfrost_device *pdev = &dev->physical_device->pdev;

   pan_section_pack(desc, MULTI_TARGET_FRAMEBUFFER, BIFROST_PARAMETERS, params) {
      params.sample_locations =
         panfrost_sample_positions(pdev, MALI_SAMPLE_PATTERN_SINGLE_SAMPLED);
   }

   pan_section_pack(desc, MULTI_TARGET_FRAMEBUFFER, BIFROST_TILER_POINTER, tiler) {
      tiler.address = batch->tiler.gpu;
   }

   pan_section_pack(desc, MULTI_TARGET_FRAMEBUFFER, BIFROST_PADDING, padding);
}

static void
panvk_emit_mfb(const struct panvk_device *dev,
               const struct panvk_batch *batch,
               const struct panvk_subpass *subpass,
               const struct panvk_pipeline *pipeline,
               const struct panvk_framebuffer *fb,
               const struct panvk_clear_value *clears,
               void *desc)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   unsigned cbuf_offset = 0, tib_size;
   unsigned internal_cbuf_size = get_internal_cbuf_size(subpass, fb, &tib_size);
   void *rt_descs;

   if (subpass->zs_attachment.idx != VK_ATTACHMENT_UNUSED) {
      void *zs_crc_desc = desc + MALI_MULTI_TARGET_FRAMEBUFFER_LENGTH;

      panvk_emit_zs_crc(dev, subpass, pipeline, fb, zs_crc_desc);
      rt_descs = zs_crc_desc += MALI_ZS_CRC_EXTENSION_LENGTH;
   } else {
      rt_descs = desc + MALI_MULTI_TARGET_FRAMEBUFFER_LENGTH;
   }

   for (unsigned cb = 0; cb < subpass->color_count; cb++) {
      const struct panvk_image_view *iview =
         subpass->color_attachments[cb].idx != VK_ATTACHMENT_UNUSED ?
         fb->attachments[cb].iview : NULL;

      panvk_emit_rt(dev, subpass, pipeline, fb, clears, cb, cbuf_offset,
                    rt_descs + (cb * MALI_RENDER_TARGET_LENGTH));
      if (iview) {
         cbuf_offset += bytes_per_pixel_tib(iview->format) * tib_size *
                        iview->image->samples;
      }
   }

   if (pan_is_bifrost(pdev))
      panvk_emit_bifrost_mfb_sections(dev, batch, desc);

   pan_section_pack(desc, MULTI_TARGET_FRAMEBUFFER, PARAMETERS, params) {
      params.width = fb->width;
      params.height = fb->height;
      params.bound_max_x = fb->width - 1;
      params.bound_max_y = fb->height - 1;
      params.effective_tile_size = tib_size;
      params.tie_break_rule = MALI_TIE_BREAK_RULE_MINUS_180_IN_0_OUT;
      params.render_target_count = subpass->color_count;
      params.z_internal_format = get_z_internal_format(subpass, fb);
      if (subpass->zs_attachment.clear) {
         params.z_clear = clears[subpass->zs_attachment.idx].depth;
         params.s_clear = clears[subpass->zs_attachment.idx].stencil;
      }

      params.color_buffer_allocation = internal_cbuf_size;

      /* FIXME */
      params.sample_count = 1;
      params.sample_pattern = MALI_SAMPLE_PATTERN_SINGLE_SAMPLED;
      params.has_zs_crc_extension = subpass->zs_attachment.idx != VK_ATTACHMENT_UNUSED;
   }
}

void
panvk_emit_fb(const struct panvk_device *dev,
              const struct panvk_batch *batch,
              const struct panvk_subpass *subpass,
              const struct panvk_pipeline *pipeline,
              const struct panvk_framebuffer *fb,
              const struct panvk_clear_value *clears,
              void *desc)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   bool sfbd = pdev->quirks & MIDGARD_SFBD;

   if (sfbd)
      panvk_emit_sfb(dev, batch, subpass, pipeline, fb, clears, desc);
   else
      panvk_emit_mfb(dev, batch, subpass, pipeline, fb, clears, desc);
}

void
panvk_emit_tls(const struct panvk_device *dev,
               const struct panvk_pipeline *pipeline,
               const struct panvk_compute_dim *wg_count,
               struct pan_pool *tls_pool,
               void *desc)
{
   pan_pack(desc, LOCAL_STORAGE, cfg) {
      if (pipeline->tls_size) {
         cfg.tls_size = panfrost_get_stack_shift(pipeline->tls_size);
         cfg.tls_base_pointer =
            panfrost_pool_alloc_aligned(tls_pool,
                                        pipeline->tls_size, 4096).gpu;
      }

      unsigned compute_size = wg_count->x * wg_count->y * wg_count->z;
      if (pipeline->wls_size && compute_size) {
         unsigned instances =
            util_next_power_of_two(wg_count->x) *
            util_next_power_of_two(wg_count->y) *
            util_next_power_of_two(wg_count->z);

         unsigned wls_size =
            util_next_power_of_two(MAX2(pipeline->wls_size, 128));

         cfg.wls_instances = instances;
         cfg.wls_size_scale = util_logbase2(wls_size) + 1;
         cfg.wls_base_pointer =
            panfrost_pool_alloc_aligned(tls_pool, wls_size, 4096).gpu;
      } else {
         cfg.wls_instances = MALI_LOCAL_STORAGE_NO_WORKGROUP_MEM;
      }
   }
}
