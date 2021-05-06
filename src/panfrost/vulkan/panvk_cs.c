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
#include "pan_cs.h"
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
                      const struct panvk_attribs_info *info,
                      const struct panvk_draw_info *draw,
                      const struct panvk_attrib_buf *bufs,
                      unsigned buf_count,
                      unsigned idx, void *desc)
{
   ASSERTED const struct panfrost_device *pdev = &dev->physical_device->pdev;
   const struct panvk_attrib_buf_info *buf_info = &info->buf[idx];

   if (buf_info->special) {
      assert(!pan_is_bifrost(pdev));
      switch (buf_info->special_id) {
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

   assert(idx < buf_count);
   const struct panvk_attrib_buf *buf = &bufs[idx];
   unsigned divisor = buf_info->per_instance ?
                      draw->padded_vertex_count : 0;
   unsigned stride = divisor && draw->instance_count == 1 ?
                     0 : buf_info->stride;
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
                       const struct panvk_attribs_info *info,
                       const struct panvk_attrib_buf *bufs,
                       unsigned buf_count,
                       const struct panvk_draw_info *draw,
                       void *descs)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   struct mali_attribute_buffer_packed *buf = descs;

   for (unsigned i = 0; i < info->buf_count; i++)
      panvk_emit_attrib_buf(dev, info, draw, bufs, buf_count, i, buf++);

   if (pan_is_bifrost(pdev))
      memset(buf, 0, sizeof(*buf));
}

static void
panvk_emit_attrib(const struct panvk_device *dev,
                  const struct panvk_attribs_info *attribs,
                  const struct panvk_attrib_buf *bufs,
                  unsigned buf_count,
                  unsigned idx, void *attrib)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;

   pan_pack(attrib, ATTRIBUTE, cfg) {
      cfg.buffer_index = attribs->attrib[idx].buf;
      cfg.offset = attribs->attrib[idx].offset +
                   (bufs[cfg.buffer_index].address & 63);
      cfg.format = pdev->formats[attribs->attrib[idx].format].hw;
   }
}

void
panvk_emit_attribs(const struct panvk_device *dev,
                   const struct panvk_attribs_info *attribs,
                   const struct panvk_attrib_buf *bufs,
                   unsigned buf_count,
                   void *descs)
{
   struct mali_attribute_packed *attrib = descs;

   for (unsigned i = 0; i < attribs->attrib_count; i++)
      panvk_emit_attrib(dev, attribs, bufs, buf_count, i, attrib++);
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
      if (pan_is_bifrost(pdev))
         cfg.thread_storage = draw->tls;
      else
         cfg.fbd = draw->fb;

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
         cfg.address = draw->tiler_ctx->bifrost;
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
panvk_prepare_midgard_fs_rsd(const struct panvk_device *dev,
                             const struct panvk_pipeline *pipeline,
                             const struct pan_blend_state *blend,
                             struct MALI_RENDERER_STATE *rsd)
{
   if (!pipeline->fs.required) {
      rsd->shader.shader = 0x1;
      rsd->properties.midgard.work_register_count = 1;
      rsd->properties.depth_source = MALI_DEPTH_SOURCE_FIXED_FUNCTION;
      rsd->properties.midgard.force_early_z = true;
   } else {
      const struct panfrost_device *pdev = &dev->physical_device->pdev;
      const struct pan_shader_info *info = &pipeline->fs.info;

      pan_shader_prepare_rsd(pdev, info, pipeline->fs.address, rsd);

      /* Reasons to disable early-Z from a shader perspective */
      bool late_z = info->fs.can_discard || info->writes_global ||
                    info->fs.writes_depth || info->fs.writes_stencil;

      /* If either depth or stencil is enabled, discard matters */
      bool zs_enabled =
         (pipeline->zs.z_test && pipeline->zs.z_compare_func != MALI_FUNC_ALWAYS) ||
         pipeline->zs.s_test;

      bool has_blend_shader = false;

      for (unsigned i = 0; i < blend->rt_count; i++)
         has_blend_shader |= !pan_blend_can_fixed_function(pdev, blend, i);

      /* TODO: Reduce this limit? */
      if (has_blend_shader)
         rsd->properties.midgard.work_register_count = MAX2(info->work_reg_count, 8);
      else
         rsd->properties.midgard.work_register_count = info->work_reg_count;

      rsd->properties.midgard.force_early_z =
         !(late_z || pipeline->ms.alpha_to_coverage);

      /* Workaround a hardware errata where early-z cannot be enabled
       * when discarding even when the depth buffer is read-only, by
       * lying to the hardware about the discard and setting the
       * reads tilebuffer? flag to compensate */
      rsd->properties.midgard.shader_reads_tilebuffer =
         info->fs.outputs_read ||
         (!zs_enabled && info->fs.can_discard);
      rsd->properties.midgard.shader_contains_discard =
         zs_enabled && info->fs.can_discard;
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
      panvk_prepare_midgard_fs_rsd(dev, pipeline, blend, rsd);

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
            panfrost_format_to_bifrost_blend(pdev, rts->format);
         cfg.bifrost.internal.fixed_function.conversion.register_format =
            bifrost_blend_type_from_nir(pipeline->fs.info.bifrost.blend[rt].type);
         cfg.bifrost.internal.fixed_function.rt = rt;
      }
   }
}

static void
panvk_prepare_midgard_blend(const struct panvk_device *dev,
                            const struct panvk_pipeline *pipeline,
                            const struct pan_blend_state *blend,
                            mali_ptr blend_shader,
                            unsigned rt, void *bd)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   const struct pan_blend_rt_state *rts = &blend->rts[rt];

   if (!blend->rt_count || !rts) {
      /* Disable blending for depth-only */
      pan_pack(bd, BLEND, cfg) {
         cfg.midgard.equation.color_mask = 0xf;
         cfg.midgard.equation.rgb.a = MALI_BLEND_OPERAND_A_SRC;
         cfg.midgard.equation.rgb.b = MALI_BLEND_OPERAND_B_SRC;
         cfg.midgard.equation.rgb.c = MALI_BLEND_OPERAND_C_ZERO;
         cfg.midgard.equation.alpha.a = MALI_BLEND_OPERAND_A_SRC;
         cfg.midgard.equation.alpha.b = MALI_BLEND_OPERAND_B_SRC;
         cfg.midgard.equation.alpha.c = MALI_BLEND_OPERAND_C_ZERO;
      }
      return;
   }

   pan_pack(bd, BLEND, cfg) {
      if (!rts->equation.color_mask) {
         cfg.enable = false;
         continue;
      }

      cfg.srgb = util_format_is_srgb(rts->format);
      cfg.load_destination = pan_blend_reads_dest(blend, rt);
      cfg.round_to_fb_precision = !blend->dither;
      cfg.midgard.blend_shader = blend_shader != 0;
      if (blend_shader) {
         cfg.midgard.shader_pc = blend_shader;
      } else {
         pan_blend_to_fixed_function_equation(pdev, blend, rt,
                                              &cfg.midgard.equation);
         cfg.midgard.constant = pan_blend_get_constant(pdev, blend, rt);
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
      panvk_prepare_midgard_blend(dev, pipeline, blend, blend_shader, rt, bd);
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

unsigned
panvk_emit_fb(const struct panvk_device *dev,
              const struct panvk_batch *batch,
              const struct panvk_subpass *subpass,
              const struct panvk_pipeline *pipeline,
              const struct panvk_framebuffer *fb,
              const struct panvk_clear_value *clears,
              const struct pan_tls_info *tlsinfo,
              const struct pan_tiler_context *tilerctx,
              void *desc)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   struct panvk_image_view *view;
   struct pan_fb_info fbinfo = {
      .width = fb->width,
      .height = fb->height,
      .extent.maxx = fb->width - 1,
      .extent.maxy = fb->height - 1,
      .nr_samples = 1,
   };
   struct pan_image_state dummy_rt_state[8] = { 0 };
   struct pan_image_state dummy_zs_state = { 0 };
   struct pan_image_state dummy_s_state = { 0 };

   for (unsigned cb = 0; cb < subpass->color_count; cb++) {
      int idx = subpass->color_attachments[cb].idx;
      view = idx != VK_ATTACHMENT_UNUSED ?
             fb->attachments[idx].iview : NULL;
      if (!view)
         continue;
      fbinfo.rts[cb].view = &view->pview;
      fbinfo.rts[cb].state = &dummy_rt_state[cb];
      fbinfo.rts[cb].clear = subpass->color_attachments[idx].clear;
      memcpy(fbinfo.rts[cb].clear_value, clears[idx].color,
             sizeof(fbinfo.rts[cb].clear_value));
      fbinfo.nr_samples =
         MAX2(fbinfo.nr_samples, view->pview.image->layout.nr_samples);
   }

   if (subpass->zs_attachment.idx != VK_ATTACHMENT_UNUSED) {
      view = fb->attachments[subpass->zs_attachment.idx].iview;
      const struct util_format_description *fdesc =
         util_format_description(view->pview.format);

      fbinfo.nr_samples =
         MAX2(fbinfo.nr_samples, view->pview.image->layout.nr_samples);

      if (util_format_has_depth(fdesc)) {
         fbinfo.zs.clear.z = subpass->zs_attachment.clear;
         fbinfo.zs.clear_value.depth = clears[subpass->zs_attachment.idx].depth;
         fbinfo.zs.view.zs = &view->pview;
         fbinfo.zs.state.zs = &dummy_zs_state;
      }

      if (util_format_has_depth(fdesc)) {
         fbinfo.zs.clear.s = subpass->zs_attachment.clear;
         fbinfo.zs.clear_value.stencil = clears[subpass->zs_attachment.idx].depth;
         if (!fbinfo.zs.view.zs) {
            fbinfo.zs.view.s = &view->pview;
            fbinfo.zs.state.s = &dummy_s_state;
         }
      }
   }

   return pan_emit_fbd(pdev, &fbinfo, tlsinfo, tilerctx, desc);
}
