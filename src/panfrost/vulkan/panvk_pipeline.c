/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_pipeline.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
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

#include "panvk_cs.h"
#include "panvk_private.h"

#include "pan_bo.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "spirv/nir_spirv.h"
#include "util/debug.h"
#include "util/mesa-sha1.h"
#include "util/u_atomic.h"
#include "vk_format.h"
#include "vk_util.h"

#include "panfrost/util/pan_lower_framebuffer.h"

#include "panfrost-quirks.h"

struct panvk_pipeline_builder
{
   struct panvk_device *device;
   struct panvk_pipeline_cache *cache;
   const VkAllocationCallbacks *alloc;
   const VkGraphicsPipelineCreateInfo *create_info;
   const struct panvk_pipeline_layout *layout;

   struct panvk_shader *shaders[MESA_SHADER_STAGES];
   struct panvk_blend_shader blend_shaders[MAX_RTS];
   uint32_t shader_offsets[MESA_SHADER_STAGES];
   uint32_t blend_shader_offsets[MAX_RTS];
   uint32_t shader_total_size;
   uint32_t static_state_size;
   uint32_t rsd_offsets[MESA_SHADER_STAGES];
   uint32_t vpd_offset;
   uint32_t sysvals_offsets[MESA_SHADER_STAGES];

   bool rasterizer_discard;
   /* these states are affectd by rasterizer_discard */
   VkSampleCountFlagBits samples;
   bool use_depth_stencil_attachment;
   uint8_t active_color_attachments;
   enum pipe_format color_attachment_formats[MAX_RTS];
};

static VkResult
panvk_pipeline_builder_create_pipeline(struct panvk_pipeline_builder *builder,
                                       struct panvk_pipeline **out_pipeline)
{
   struct panvk_device *dev = builder->device;

   struct panvk_pipeline *pipeline =
      vk_object_zalloc(&dev->vk, builder->alloc,
                       sizeof(*pipeline), VK_OBJECT_TYPE_PIPELINE);
   if (!pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   pipeline->layout = builder->layout;
   *out_pipeline = pipeline;
   return VK_SUCCESS;
}

static void
panvk_pipeline_builder_finish(struct panvk_pipeline_builder *builder)
{
   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      if (!builder->shaders[i])
         continue;
      panvk_shader_destroy(builder->device, builder->shaders[i], builder->alloc);
   }
}

static gl_shader_stage
panvk_shader_stage(VkShaderStageFlagBits stage)
{
   switch (stage) {
   case VK_SHADER_STAGE_VERTEX_BIT:
      return MESA_SHADER_VERTEX;
   case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      return MESA_SHADER_TESS_CTRL;
   case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
      return MESA_SHADER_TESS_EVAL;
   case VK_SHADER_STAGE_GEOMETRY_BIT:
      return MESA_SHADER_GEOMETRY;
   case VK_SHADER_STAGE_FRAGMENT_BIT:
      return MESA_SHADER_FRAGMENT;
   case VK_SHADER_STAGE_COMPUTE_BIT:
      return MESA_SHADER_COMPUTE;
   default:
      unreachable("invalid VkShaderStageFlagBits");
      return MESA_SHADER_NONE;
   }
}

static VkResult
panvk_pipeline_builder_compile_shaders(struct panvk_pipeline_builder *builder)
{
   const VkPipelineShaderStageCreateInfo *stage_infos[MESA_SHADER_STAGES] = {
      NULL
   };
   for (uint32_t i = 0; i < builder->create_info->stageCount; i++) {
      gl_shader_stage stage = panvk_shader_stage(builder->create_info->pStages[i].stage);
      stage_infos[stage] = &builder->create_info->pStages[i];
   }

   /* compile shaders in reverse order */
   unsigned sysval_ubo = builder->layout->num_ubos;

   for (gl_shader_stage stage = MESA_SHADER_STAGES - 1;
        stage > MESA_SHADER_NONE; stage--) {
      const VkPipelineShaderStageCreateInfo *stage_info = stage_infos[stage];
      if (!stage_info)
         continue;

      struct panvk_shader *shader;

      shader = panvk_shader_create(builder->device, stage, stage_info,
                                   builder->layout, sysval_ubo,
                                   builder->alloc);
      if (!shader)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      if (shader->info.sysvals.sysval_count)
         sysval_ubo++;
 
      builder->shaders[stage] = shader;
      builder->shader_total_size = ALIGN_POT(builder->shader_total_size, 128);
      builder->shader_offsets[stage] = builder->shader_total_size;
      builder->shader_total_size +=
         util_dynarray_num_elements(&shader->binary, uint8_t);
   }

   return VK_SUCCESS;
}

static VkResult
panvk_pipeline_builder_upload_shaders(struct panvk_pipeline_builder *builder,
                                      struct panvk_pipeline *pipeline)
{
   struct panfrost_bo *bin_bo =
      panfrost_bo_create(&builder->device->physical_device->pdev,
                         builder->shader_total_size, PAN_BO_EXECUTE);

   pipeline->binary_bo = bin_bo;
   panfrost_bo_mmap(bin_bo);

   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      const struct panvk_shader *shader = builder->shaders[i];
      if (!shader)
         continue;

      memcpy(pipeline->binary_bo->ptr.cpu + builder->shader_offsets[i],
             util_dynarray_element(&shader->binary, uint8_t, 0),
             util_dynarray_num_elements(&shader->binary, uint8_t));
   }

   for (uint32_t i = 0; i < pipeline->blend.rt_count; i++) {
      if (!builder->blend_shaders[i].binary.size)
         continue;

      memcpy(pipeline->binary_bo->ptr.cpu + builder->blend_shader_offsets[i],
             builder->blend_shaders[i].binary.data,
             builder->blend_shaders[i].binary.size);
      pipeline->blend_shaders[i].address = pipeline->binary_bo->ptr.gpu +
                                           builder->blend_shader_offsets[i];
      util_dynarray_fini(&builder->blend_shaders[i].binary);
   }

   return VK_SUCCESS;
}

static bool
panvk_pipeline_static_state(struct panvk_pipeline *pipeline, uint32_t id)
{
   if (pipeline->dynamic_state_mask & (1 << id))
      return false;

   return true;
}

static bool
panvk_pipeline_static_sysval(struct panvk_pipeline *pipeline,
                             unsigned id)
{
   switch (id) {
   case PAN_SYSVAL_VIEWPORT_SCALE:
   case PAN_SYSVAL_VIEWPORT_OFFSET:
      return panvk_pipeline_static_state(pipeline, VK_DYNAMIC_STATE_VIEWPORT);
   default:
      return false;
   }
}

static void
panvk_pipeline_builder_alloc_static_state_bo(struct panvk_pipeline_builder *builder,
                                             struct panvk_pipeline *pipeline)
{
   struct panfrost_device *pdev =
      &builder->device->physical_device->pdev;
   unsigned bo_size = 0;

   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      const struct panvk_shader *shader = builder->shaders[i];
      if (!shader)
         continue;

      if (pipeline->fs.dynamic_rsd && i == MESA_SHADER_FRAGMENT)
         continue;

      bo_size = ALIGN_POT(bo_size, 64);
      builder->rsd_offsets[i] = bo_size;
      bo_size += MALI_RENDERER_STATE_LENGTH;
      if (i == MESA_SHADER_FRAGMENT)
         bo_size += MALI_BLEND_LENGTH * pipeline->blend.rt_count;
   }

   if (panvk_pipeline_static_state(pipeline, VK_DYNAMIC_STATE_VIEWPORT) &&
       panvk_pipeline_static_state(pipeline, VK_DYNAMIC_STATE_SCISSOR)) {
      bo_size = ALIGN_POT(bo_size, 16);
      builder->vpd_offset = bo_size;
      bo_size += MALI_VIEWPORT_LENGTH;
   }

   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      const struct panvk_shader *shader = builder->shaders[i];
      if (!shader || !shader->info.sysvals.sysval_count)
         continue;

      bool static_sysvals = true;
      for (unsigned s = 0; s < shader->info.sysvals.sysval_count; s++) {
         unsigned id = shader->info.sysvals.sysvals[i];
         static_sysvals &= panvk_pipeline_static_sysval(pipeline, id);
      }

      if (!static_sysvals) {
         builder->sysvals_offsets[i] = ~0;
         continue;
      }

      bo_size = ALIGN_POT(bo_size, 16);
      builder->sysvals_offsets[i] = bo_size;
      bo_size += shader->info.sysvals.sysval_count * 16;
   }

   if (bo_size) {
      pipeline->state_bo = panfrost_bo_create(pdev, bo_size, 0);
      panfrost_bo_mmap(pipeline->state_bo);
   }
}

static void
panvk_pipeline_builder_upload_sysval(struct panvk_pipeline_builder *builder,
                                     struct panvk_pipeline *pipeline,
                                     unsigned id, union panvk_sysval_data *data)
{
   switch (PAN_SYSVAL_TYPE(id)) {
   case PAN_SYSVAL_VIEWPORT_SCALE:
      panvk_sysval_upload_viewport_scale(builder->create_info->pViewportState->pViewports,
                                         data);
      break;
   case PAN_SYSVAL_VIEWPORT_OFFSET:
      panvk_sysval_upload_viewport_scale(builder->create_info->pViewportState->pViewports,
                                         data);
      break;
   default:
      unreachable("Invalid static sysval");
   }
}

static void
panvk_pipeline_builder_init_sysvals(struct panvk_pipeline_builder *builder,
                                    struct panvk_pipeline *pipeline,
                                    gl_shader_stage stage)
{
   const struct panvk_shader *shader = builder->shaders[stage];

   pipeline->sysvals[stage].ids = shader->info.sysvals;
   pipeline->sysvals[stage].ubo_idx = shader->sysval_ubo;

   if (!shader->info.sysvals.sysval_count ||
       builder->sysvals_offsets[stage] == ~0)
      return;

   union panvk_sysval_data *static_data =
      pipeline->state_bo->ptr.cpu + builder->sysvals_offsets[stage];

   pipeline->sysvals[stage].ubo =
      pipeline->state_bo->ptr.gpu + builder->sysvals_offsets[stage];

   for (unsigned i = 0; i < shader->info.sysvals.sysval_count; i++) {
      unsigned id = shader->info.sysvals.sysvals[i];

      panvk_pipeline_builder_upload_sysval(builder,
                                           pipeline,
                                           id, &static_data[i]);
   }
}

static void
panvk_pipeline_builder_init_shaders(struct panvk_pipeline_builder *builder,
                                    struct panvk_pipeline *pipeline)
{
   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      const struct panvk_shader *shader = builder->shaders[i];
      if (!shader)
         continue;

      pipeline->tls_size = MAX2(pipeline->tls_size, shader->info.tls_size);
      pipeline->wls_size = MAX2(pipeline->tls_size, shader->info.wls_size);

      if (i == MESA_SHADER_VERTEX && shader->info.vs.writes_point_size)
         pipeline->ia.writes_point_size = true;

      if (i != MESA_SHADER_FRAGMENT || !pipeline->fs.dynamic_rsd) {
         mali_ptr shader_ptr = pipeline->binary_bo->ptr.gpu +
                               builder->shader_offsets[i];

         void *rsd = pipeline->state_bo->ptr.cpu + builder->rsd_offsets[i];
         if (i != MESA_SHADER_FRAGMENT) {
            panvk_emit_non_fs_rsd(builder->device, &shader->info, shader_ptr, rsd);
         } else {
            panvk_emit_fs_rsd(builder->device, pipeline, NULL, rsd);
         }

         pipeline->rsds[i] =
            pipeline->state_bo->ptr.gpu + builder->rsd_offsets[i];
         panvk_pipeline_builder_init_sysvals(builder, pipeline, i);
      }
   }
}


static void
panvk_pipeline_builder_parse_viewport(struct panvk_pipeline_builder *builder,
                                      struct panvk_pipeline *pipeline)
{
   /* The spec says:
    *
    *    pViewportState is a pointer to an instance of the
    *    VkPipelineViewportStateCreateInfo structure, and is ignored if the
    *    pipeline has rasterization disabled.
    *
    * We leave the relevant registers stale in that case.
    */
   if (!builder->rasterizer_discard &&
       panvk_pipeline_static_state(pipeline, VK_DYNAMIC_STATE_VIEWPORT) &&
       panvk_pipeline_static_state(pipeline, VK_DYNAMIC_STATE_SCISSOR)) {
      void *vpd = pipeline->state_bo->ptr.cpu + builder->vpd_offset;
      panvk_emit_viewport(builder->create_info->pViewportState->pViewports,
		          builder->create_info->pViewportState->pScissors,
                          vpd);
      pipeline->vpd = pipeline->state_bo->ptr.gpu +
                      builder->vpd_offset;
   } else {
      if (builder->create_info->pViewportState->pViewports)
         pipeline->viewport = builder->create_info->pViewportState->pViewports[0];

      if (builder->create_info->pViewportState->pScissors)
         pipeline->scissor = builder->create_info->pViewportState->pScissors[0];
   }
}

static void
panvk_pipeline_builder_parse_dynamic(struct panvk_pipeline_builder *builder,
                                     struct panvk_pipeline *pipeline)
{
   const VkPipelineDynamicStateCreateInfo *dynamic_info =
      builder->create_info->pDynamicState;

   if (!dynamic_info)
      return;

   for (uint32_t i = 0; i < dynamic_info->dynamicStateCount; i++) {
      VkDynamicState state = dynamic_info->pDynamicStates[i];
      switch (state) {
      case VK_DYNAMIC_STATE_VIEWPORT ... VK_DYNAMIC_STATE_STENCIL_REFERENCE:
         pipeline->dynamic_state_mask |= 1 << state;
         break;
      default:
         unreachable("unsupported dynamic state");
      }
   }

}

static enum mali_draw_mode
translate_prim_topology(VkPrimitiveTopology in)
{
   switch (in) {
   case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
      return MALI_DRAW_MODE_POINTS;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
      return MALI_DRAW_MODE_LINES;
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
      return MALI_DRAW_MODE_LINE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
      return MALI_DRAW_MODE_TRIANGLES;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
      return MALI_DRAW_MODE_TRIANGLE_STRIP;
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
      return MALI_DRAW_MODE_TRIANGLE_FAN;
   case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
   case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
   default:
      unreachable("Invalid primitive type");
   }
}

static void
panvk_pipeline_builder_parse_input_assembly(struct panvk_pipeline_builder *builder,
                                            struct panvk_pipeline *pipeline)
{
   pipeline->ia.primitive_restart =
      builder->create_info->pInputAssemblyState->primitiveRestartEnable;
   pipeline->ia.topology =
      translate_prim_topology(builder->create_info->pInputAssemblyState->topology);
}

static enum pipe_logicop
translate_logicop(VkLogicOp in)
{
   switch (in) {
   case VK_LOGIC_OP_CLEAR: return PIPE_LOGICOP_CLEAR;
   case VK_LOGIC_OP_AND: return PIPE_LOGICOP_AND;
   case VK_LOGIC_OP_AND_REVERSE: return PIPE_LOGICOP_AND_REVERSE;
   case VK_LOGIC_OP_COPY: return PIPE_LOGICOP_COPY;
   case VK_LOGIC_OP_AND_INVERTED: return PIPE_LOGICOP_AND_INVERTED;
   case VK_LOGIC_OP_NO_OP: return PIPE_LOGICOP_NOOP;
   case VK_LOGIC_OP_XOR: return PIPE_LOGICOP_XOR;
   case VK_LOGIC_OP_OR: return PIPE_LOGICOP_OR;
   case VK_LOGIC_OP_NOR: return PIPE_LOGICOP_NOR;
   case VK_LOGIC_OP_EQUIVALENT: return PIPE_LOGICOP_EQUIV;
   case VK_LOGIC_OP_INVERT: return PIPE_LOGICOP_INVERT;
   case VK_LOGIC_OP_OR_REVERSE: return PIPE_LOGICOP_OR_REVERSE;
   case VK_LOGIC_OP_COPY_INVERTED: return PIPE_LOGICOP_COPY_INVERTED;
   case VK_LOGIC_OP_OR_INVERTED: return PIPE_LOGICOP_OR_INVERTED;
   case VK_LOGIC_OP_NAND: return PIPE_LOGICOP_NAND;
   case VK_LOGIC_OP_SET: return PIPE_LOGICOP_SET;
   default: unreachable("Invalid logicop\n");
   }
}

static enum blend_func
translate_blend_op(VkBlendOp in)
{
   switch (in) {
   case VK_BLEND_OP_ADD: return BLEND_FUNC_ADD;
   case VK_BLEND_OP_SUBTRACT: return BLEND_FUNC_SUBTRACT;
   case VK_BLEND_OP_REVERSE_SUBTRACT: return BLEND_FUNC_REVERSE_SUBTRACT;
   case VK_BLEND_OP_MIN: return BLEND_FUNC_MIN;
   case VK_BLEND_OP_MAX: return BLEND_FUNC_MAX;
   default: unreachable("Invalid blend op\n");
   }
}

static enum blend_factor
translate_blend_factor(VkBlendFactor in)
{
   switch (in) {
   case VK_BLEND_FACTOR_ZERO:
   case VK_BLEND_FACTOR_ONE:
      return BLEND_FACTOR_ZERO;
   case VK_BLEND_FACTOR_SRC_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
      return BLEND_FACTOR_SRC_COLOR;
   case VK_BLEND_FACTOR_DST_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
      return BLEND_FACTOR_DST_COLOR;
   case VK_BLEND_FACTOR_SRC_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
      return BLEND_FACTOR_SRC_ALPHA;
   case VK_BLEND_FACTOR_DST_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
      return BLEND_FACTOR_DST_ALPHA;
   case VK_BLEND_FACTOR_CONSTANT_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
      return BLEND_FACTOR_CONSTANT_COLOR;
   case VK_BLEND_FACTOR_CONSTANT_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
      return BLEND_FACTOR_CONSTANT_ALPHA;
   case VK_BLEND_FACTOR_SRC1_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
      return BLEND_FACTOR_SRC1_COLOR;
   case VK_BLEND_FACTOR_SRC1_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
      return BLEND_FACTOR_SRC1_ALPHA;
   case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
      return BLEND_FACTOR_SRC_ALPHA_SATURATE;
   default: unreachable("Invalid blend factor\n");
   }
}

static bool
inverted_blend_factor(VkBlendFactor in)
{
   switch (in) {
   case VK_BLEND_FACTOR_ONE:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
   case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
      return true;
   default:
      return false;
   }
}

static uint64_t
bifrost_get_blend_desc(const struct panfrost_device *dev,
                       enum pipe_format fmt, unsigned rt)
{
   const struct util_format_description *desc = util_format_description(fmt);
   uint64_t res;

   pan_pack(&res, BIFROST_INTERNAL_BLEND, cfg) {
      cfg.mode = MALI_BIFROST_BLEND_MODE_OPAQUE;
      cfg.fixed_function.num_comps = desc->nr_channels;
      cfg.fixed_function.rt = rt;

      nir_alu_type type = pan_unpacked_type_for_format(desc);
      switch (type) {
      case nir_type_float16:
         cfg.fixed_function.conversion.register_format =
            MALI_BIFROST_REGISTER_FILE_FORMAT_F16;
         break;
      case nir_type_float32:
         cfg.fixed_function.conversion.register_format =
            MALI_BIFROST_REGISTER_FILE_FORMAT_F32;
         break;
      case nir_type_int8:
      case nir_type_int16:
         cfg.fixed_function.conversion.register_format =
            MALI_BIFROST_REGISTER_FILE_FORMAT_I16;
         break;
      case nir_type_int32:
         cfg.fixed_function.conversion.register_format =
            MALI_BIFROST_REGISTER_FILE_FORMAT_I32;
         break;
      case nir_type_uint8:
      case nir_type_uint16:
         cfg.fixed_function.conversion.register_format =
            MALI_BIFROST_REGISTER_FILE_FORMAT_U16;
         break;
      case nir_type_uint32:
         cfg.fixed_function.conversion.register_format =
            MALI_BIFROST_REGISTER_FILE_FORMAT_U32;
         break;
      default:
         unreachable("Invalid format");
      }

      cfg.fixed_function.conversion.memory_format =
         panfrost_format_to_bifrost_blend(dev, fmt);
   }

   return res;
}

static void
panvk_blend_compile_shader(struct panvk_device *dev,
                           struct pan_blend_state *state,
                           unsigned rt,
                           struct panvk_blend_shader *shader)
{
   if (shader->binary.size &&
       !memcmp(state->constants, shader->constants,
               sizeof(state->constants)))
      return;
 
   struct panfrost_device *pdev = &dev->physical_device->pdev;

   memcpy(shader->constants, state->constants,
          sizeof(state->constants));

   util_dynarray_clear(&shader->binary);

   struct panfrost_compile_inputs inputs = {
      .gpu_id = pdev->gpu_id,
      .is_blend = true,
      .blend.rt = rt,
      .blend.nr_samples = state->rts[rt].nr_samples,
      .rt_formats = {state->rts[rt].format},
   };

   memcpy(inputs.blend.constants, shader->constants,
          sizeof(inputs.blend.constants));
 
   if (pan_is_bifrost(pdev)) {
      inputs.blend.bifrost_blend_desc =
         bifrost_get_blend_desc(pdev, state->rts[rt].format, rt);
   }

   pan_shader_compile(pdev, shader->nir, &inputs,
                      &shader->binary, &shader->info);
}

static void
panvk_pipeline_builder_parse_color_blend(struct panvk_pipeline_builder *builder,
                                         struct panvk_pipeline *pipeline)
{
   struct panfrost_device *pdev =
      &builder->device->physical_device->pdev;

   pipeline->blend.logicop_enable =
      builder->create_info->pColorBlendState->logicOpEnable;
   pipeline->blend.logicop_func =
      translate_logicop(builder->create_info->pColorBlendState->logicOp);
   pipeline->blend.rt_count = util_last_bit(builder->active_color_attachments);
   memcpy(pipeline->blend.constants,
          builder->create_info->pColorBlendState->blendConstants,
          sizeof(pipeline->blend.constants));

   for (unsigned i = 0; i < pipeline->blend.rt_count; i++) {
      const VkPipelineColorBlendAttachmentState *in =
         &builder->create_info->pColorBlendState->pAttachments[i];
      struct pan_blend_rt_state *out = &pipeline->blend.rts[i];

      out->format = builder->color_attachment_formats[i];
      out->nr_samples = builder->create_info->pMultisampleState->rasterizationSamples;
      out->equation.blend_enable = in->blendEnable;
      out->equation.color_mask = in->colorWriteMask;
      out->equation.rgb_func = translate_blend_op(in->colorBlendOp);
      out->equation.rgb_src_factor = translate_blend_factor(in->srcColorBlendFactor);
      out->equation.rgb_invert_src_factor = inverted_blend_factor(in->srcColorBlendFactor);
      out->equation.rgb_dst_factor = translate_blend_factor(in->dstColorBlendFactor);
      out->equation.rgb_invert_dst_factor = inverted_blend_factor(in->dstColorBlendFactor);
      out->equation.alpha_func = translate_blend_op(in->alphaBlendOp);
      out->equation.alpha_src_factor = translate_blend_factor(in->srcAlphaBlendFactor);
      out->equation.alpha_invert_src_factor = inverted_blend_factor(in->srcAlphaBlendFactor);
      out->equation.alpha_dst_factor = translate_blend_factor(in->dstAlphaBlendFactor);
      out->equation.alpha_invert_dst_factor = inverted_blend_factor(in->dstAlphaBlendFactor);
      util_dynarray_init(&builder->blend_shaders[i].binary, NULL);

      unsigned nconstants =
         util_bitcount(pan_blend_constant_mask(&pipeline->blend, i));

      /* Skip the blend shader creation if we can always use the FF path */
      if (pan_blend_can_fixed_function(pdev, &pipeline->blend, i) &&
          nconstants <= 1)
         continue;

      /* Default for Midgard */
      nir_alu_type col0_type = nir_type_float32;
      nir_alu_type col1_type = nir_type_float32;
      /* Bifrost has per-output types, respect them */
      if (pan_is_bifrost(pdev)) {
         col0_type = pipeline->fs.info.bifrost.blend[i].type;
         col1_type = pipeline->fs.info.bifrost.blend_src1_type;
      }

      /* TODO: use the blend shader cache */
      builder->blend_shaders[i].nir =
         pan_blend_create_shader(pdev, &pipeline->blend, col0_type, col1_type, i);

      if (!nconstants) {
         /* No constant involved, we can compile the shader now */
         panvk_blend_compile_shader(builder->device, &pipeline->blend, i,
                                    &builder->blend_shaders[i]);
         builder->shader_total_size = ALIGN_POT(builder->shader_total_size, 128);
         builder->blend_shader_offsets[i] = builder->shader_total_size;
         builder->shader_total_size += builder->blend_shaders[i].binary.size;
         ralloc_free(builder->blend_shaders[i].nir);
      } else {
         pipeline->blend_shaders[i].nir = builder->blend_shaders[i].nir;
      }
   }
}

static void
panvk_pipeline_builder_parse_multisample(struct panvk_pipeline_builder *builder,
                                         struct panvk_pipeline *pipeline)
{
   unsigned nr_samples =
      MAX2(builder->create_info->pMultisampleState->rasterizationSamples, 1);

   pipeline->ms.rast_samples =
      builder->create_info->pMultisampleState->rasterizationSamples;
   pipeline->ms.sample_mask =
      builder->create_info->pMultisampleState->pSampleMask ?
      builder->create_info->pMultisampleState->pSampleMask[0] : UINT16_MAX;
   pipeline->ms.min_samples =
      MAX2(builder->create_info->pMultisampleState->minSampleShading * nr_samples, 1);
}

static enum mali_func
translate_cmp_func(VkCompareOp in)
{
   switch (in) {
   case VK_COMPARE_OP_NEVER: return MALI_FUNC_NEVER;
   case VK_COMPARE_OP_LESS: return MALI_FUNC_LESS;
   case VK_COMPARE_OP_EQUAL: return MALI_FUNC_EQUAL;
   case VK_COMPARE_OP_LESS_OR_EQUAL: return MALI_FUNC_LEQUAL;
   case VK_COMPARE_OP_GREATER: return MALI_FUNC_GREATER;
   case VK_COMPARE_OP_NOT_EQUAL: return MALI_FUNC_NOT_EQUAL;
   case VK_COMPARE_OP_GREATER_OR_EQUAL: return MALI_FUNC_GEQUAL;
   case VK_COMPARE_OP_ALWAYS: return MALI_FUNC_ALWAYS;
   default: unreachable("Invalid cmp func\n");
   }
}

static enum mali_stencil_op
translate_stencil_op(VkStencilOp in)
{
   switch (in) {
   case VK_STENCIL_OP_KEEP: return MALI_STENCIL_OP_KEEP;
   case VK_STENCIL_OP_ZERO: return MALI_STENCIL_OP_ZERO;
   case VK_STENCIL_OP_REPLACE: return MALI_STENCIL_OP_REPLACE;
   case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return MALI_STENCIL_OP_INCR_SAT;
   case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return MALI_STENCIL_OP_DECR_SAT;
   case VK_STENCIL_OP_INCREMENT_AND_WRAP: return MALI_STENCIL_OP_INCR_WRAP;
   case VK_STENCIL_OP_DECREMENT_AND_WRAP: return MALI_STENCIL_OP_DECR_WRAP;
   case VK_STENCIL_OP_INVERT: return MALI_STENCIL_OP_INVERT;
   default: unreachable("Invalid stencil op");
   }
}

static void
panvk_pipeline_builder_parse_zs(struct panvk_pipeline_builder *builder,
                                struct panvk_pipeline *pipeline)
{
   pipeline->zs.z_test = builder->create_info->pDepthStencilState->depthTestEnable;
   pipeline->zs.z_write = builder->create_info->pDepthStencilState->depthWriteEnable;
   pipeline->zs.z_compare_func =
      translate_cmp_func(builder->create_info->pDepthStencilState->depthCompareOp);
   pipeline->zs.s_test = builder->create_info->pDepthStencilState->stencilTestEnable;
   pipeline->zs.s_front.fail_op =
      translate_stencil_op(builder->create_info->pDepthStencilState->front.failOp);
   pipeline->zs.s_front.pass_op =
      translate_stencil_op(builder->create_info->pDepthStencilState->front.passOp);
   pipeline->zs.s_front.z_fail_op =
      translate_stencil_op(builder->create_info->pDepthStencilState->front.depthFailOp);
   pipeline->zs.s_front.compare_func =
      translate_cmp_func(builder->create_info->pDepthStencilState->front.compareOp);
   pipeline->zs.s_front.compare_mask =
      builder->create_info->pDepthStencilState->front.compareMask;
   pipeline->zs.s_front.write_mask =
      builder->create_info->pDepthStencilState->front.writeMask;
   pipeline->zs.s_front.ref =
      builder->create_info->pDepthStencilState->front.reference;
   pipeline->zs.s_back.fail_op =
      translate_stencil_op(builder->create_info->pDepthStencilState->back.failOp);
   pipeline->zs.s_back.pass_op =
      translate_stencil_op(builder->create_info->pDepthStencilState->back.passOp);
   pipeline->zs.s_back.z_fail_op =
      translate_stencil_op(builder->create_info->pDepthStencilState->back.depthFailOp);
   pipeline->zs.s_back.compare_func =
      translate_cmp_func(builder->create_info->pDepthStencilState->back.compareOp);
   pipeline->zs.s_back.compare_mask =
      builder->create_info->pDepthStencilState->back.compareMask;
   pipeline->zs.s_back.write_mask =
      builder->create_info->pDepthStencilState->back.writeMask;
   pipeline->zs.s_back.ref =
      builder->create_info->pDepthStencilState->back.reference;
}

static void
panvk_pipeline_builder_parse_rast(struct panvk_pipeline_builder *builder,
                                  struct panvk_pipeline *pipeline)
{
   pipeline->rast.clamp_depth = builder->create_info->pRasterizationState->depthClampEnable;
   pipeline->rast.depth_bias.enable = builder->create_info->pRasterizationState->depthBiasEnable;
   pipeline->rast.depth_bias.constant_factor =
      builder->create_info->pRasterizationState->depthBiasConstantFactor;
   pipeline->rast.depth_bias.clamp = builder->create_info->pRasterizationState->depthBiasClamp;
   pipeline->rast.depth_bias.slope_factor = builder->create_info->pRasterizationState->depthBiasSlopeFactor;
   pipeline->rast.front_ccw = builder->create_info->pRasterizationState->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE;
   pipeline->rast.cull_front_face = builder->create_info->pRasterizationState->cullMode & VK_CULL_MODE_FRONT_BIT;
   pipeline->rast.cull_back_face = builder->create_info->pRasterizationState->cullMode & VK_CULL_MODE_BACK_BIT;
}

static bool
panvk_fs_required(struct panvk_pipeline *pipeline)
{
   const struct pan_shader_info *info = &pipeline->fs.info;

   /* If we generally have side effects */
   if (info->fs.sidefx)
      return true;

    /* If colour is written we need to execute */
    const struct pan_blend_state *blend = &pipeline->blend;
    for (unsigned i = 0; i < blend->rt_count; ++i) {
       if (blend->rts[i].equation.color_mask)
          return true;
    }

    /* If depth is written and not implied we need to execute.
     * TODO: Predicate on Z/S writes being enabled */
    return (info->fs.writes_depth || info->fs.writes_stencil);
}

#define PANVK_DYNAMIC_FS_RSD_MASK \
        ((1 << VK_DYNAMIC_STATE_DEPTH_BIAS) | \
         (1 << VK_DYNAMIC_STATE_BLEND_CONSTANTS) | \
         (1 << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK) | \
         (1 << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK) | \
         (1 << VK_DYNAMIC_STATE_STENCIL_REFERENCE))

static void
panvk_pipeline_builder_init_fs_state(struct panvk_pipeline_builder *builder,
                                     struct panvk_pipeline *pipeline)
{
   if (!builder->shaders[MESA_SHADER_FRAGMENT])
      return;

   pipeline->fs.dynamic_rsd =
      pipeline->dynamic_state_mask & PANVK_DYNAMIC_FS_RSD_MASK;
   pipeline->fs.address = pipeline->binary_bo->ptr.gpu +
                          builder->shader_offsets[MESA_SHADER_FRAGMENT];
   pipeline->fs.info = builder->shaders[MESA_SHADER_FRAGMENT]->info;
   pipeline->fs.required = panvk_fs_required(pipeline);
}

static void
panvk_pipeline_update_varying_slot(struct panvk_varyings_info *varyings,
                                   gl_shader_stage stage,
                                   const struct pan_shader_varying *varying,
                                   bool input)
{
   bool fs = stage == MESA_SHADER_FRAGMENT;
   gl_varying_slot loc = varying->location;
   enum panvk_varying_buf_id buf_id =
      panvk_varying_buf_id(fs, loc);

   varyings->stage[stage].loc[varyings->stage[stage].count++] = loc;

   if (panvk_varying_is_builtin(stage, loc)) {
      varyings->buf_mask |= 1 << buf_id;
      return;
   }

   assert(loc < ARRAY_SIZE(varyings->varying));

   enum pipe_format new_fmt = varying->format;
   enum pipe_format old_fmt = varyings->varying[loc].format;

   BITSET_SET(varyings->active, loc);

   /* We expect inputs to either be set by a previous stage or be built
    * in, skip the entry if that's not the case, we'll emit a const
    * varying returning zero for those entries.
    */
   if (input && old_fmt == PIPE_FORMAT_NONE)
      return;

   unsigned new_size = util_format_get_blocksize(new_fmt);
   unsigned old_size = util_format_get_blocksize(old_fmt);

   if (old_size < new_size)
      varyings->varying[loc].format = new_fmt;

   varyings->buf_mask |= 1 << buf_id;
}



static void
panvk_pipeline_builder_collect_varyings(struct panvk_pipeline_builder *builder,
                                        struct panvk_pipeline *pipeline)
{
   for (uint32_t s = 0; s < MESA_SHADER_STAGES; s++) {
      if (!builder->shaders[s])
         continue;

      const struct pan_shader_info *info = &builder->shaders[s]->info;

      for (unsigned i = 0; i < info->varyings.input_count; i++) {
         panvk_pipeline_update_varying_slot(&pipeline->varyings, s,
                                            &info->varyings.input[i],
                                            true);
      }

      for (unsigned i = 0; i < info->varyings.output_count; i++) {
         panvk_pipeline_update_varying_slot(&pipeline->varyings, s,
                                            &info->varyings.output[i],
                                            false);
      }
   }

   /* TODO: Xfb */
   gl_varying_slot loc;
   BITSET_FOREACH_SET(loc, pipeline->varyings.active, VARYING_SLOT_MAX) {
      enum panvk_varying_buf_id buf_id =
         panvk_varying_buf_id(false, loc);
      unsigned buf_idx = panvk_varying_buf_index(&pipeline->varyings, buf_id);
      unsigned varying_sz = panvk_varying_size(&pipeline->varyings, loc);

      pipeline->varyings.varying[loc].buf = buf_idx;
      pipeline->varyings.varying[loc].offset =
         pipeline->varyings.buf[buf_idx].stride;
      pipeline->varyings.buf[buf_idx].stride += varying_sz;
   }
}

static void
panvk_pipeline_builder_parse_vertex_input(struct panvk_pipeline_builder *builder,
                                          struct panvk_pipeline *pipeline)
{
   struct panvk_attribs_info *attribs = &pipeline->attribs;
   const VkPipelineVertexInputStateCreateInfo *info =
      builder->create_info->pVertexInputState;

   for (unsigned i = 0; i < info->vertexBindingDescriptionCount; i++) {
      const VkVertexInputBindingDescription *desc =
         &info->pVertexBindingDescriptions[i];
      attribs->buf_count = MAX2(desc->binding + 1, attribs->buf_count);
      attribs->buf[desc->binding].stride = desc->stride;
      attribs->buf[desc->binding].special = false;
   }

   for (unsigned i = 0; i < info->vertexAttributeDescriptionCount; i++) {
      const VkVertexInputAttributeDescription *desc =
         &info->pVertexAttributeDescriptions[i];
      attribs->attrib[desc->location].buf = desc->binding;
      attribs->attrib[desc->location].format =
         vk_format_to_pipe_format(desc->format);
      attribs->attrib[desc->location].offset = desc->offset;
   }

   const struct pan_shader_info *vs =
      &builder->shaders[MESA_SHADER_VERTEX]->info;

   if (vs->attribute_count >= PAN_VERTEX_ID) {
      attribs->buf[attribs->buf_count].special = true;
      attribs->buf[attribs->buf_count].special_id = PAN_VERTEX_ID;
      attribs->attrib[PAN_VERTEX_ID].buf = attribs->buf_count++;
      attribs->attrib[PAN_VERTEX_ID].format = PIPE_FORMAT_R32_UINT;
   }

   if (vs->attribute_count >= PAN_INSTANCE_ID) {
      attribs->buf[attribs->buf_count].special = true;
      attribs->buf[attribs->buf_count].special_id = PAN_INSTANCE_ID;
      attribs->attrib[PAN_INSTANCE_ID].buf = attribs->buf_count++;
      attribs->attrib[PAN_INSTANCE_ID].format = PIPE_FORMAT_R32_UINT;
   }

   attribs->attrib_count = MAX2(attribs->attrib_count, vs->attribute_count);
}

static VkResult
panvk_pipeline_builder_build(struct panvk_pipeline_builder *builder,
                             struct panvk_pipeline **pipeline)
{
   VkResult result = panvk_pipeline_builder_create_pipeline(builder, pipeline);
   if (result != VK_SUCCESS)
      return result;

   /* compile and upload shaders */
   result = panvk_pipeline_builder_compile_shaders(builder);

   /* TODO: make those functions return a result and handle errors */
   panvk_pipeline_builder_collect_varyings(builder, *pipeline);
   panvk_pipeline_builder_parse_dynamic(builder, *pipeline);
   panvk_pipeline_builder_parse_input_assembly(builder, *pipeline);
   panvk_pipeline_builder_parse_color_blend(builder, *pipeline);
   panvk_pipeline_builder_parse_multisample(builder, *pipeline);
   panvk_pipeline_builder_parse_zs(builder, *pipeline);
   panvk_pipeline_builder_parse_rast(builder, *pipeline);
   panvk_pipeline_builder_parse_vertex_input(builder, *pipeline);

   panvk_pipeline_builder_upload_shaders(builder, *pipeline);
   panvk_pipeline_builder_init_fs_state(builder, *pipeline);
   panvk_pipeline_builder_alloc_static_state_bo(builder, *pipeline);
   panvk_pipeline_builder_init_shaders(builder, *pipeline);
   panvk_pipeline_builder_parse_viewport(builder, *pipeline);

   return VK_SUCCESS;
}

static void
panvk_pipeline_builder_init_graphics(struct panvk_pipeline_builder *builder,
                                     struct panvk_device *dev,
                                     struct panvk_pipeline_cache *cache,
                                     const VkGraphicsPipelineCreateInfo *create_info,
                                     const VkAllocationCallbacks *alloc)
{
   VK_FROM_HANDLE(panvk_pipeline_layout, layout, create_info->layout);
   assert(layout);
   *builder = (struct panvk_pipeline_builder) {
      .device = dev,
      .cache = cache,
      .layout = layout,
      .create_info = create_info,
      .alloc = alloc,
   };

   builder->rasterizer_discard =
      create_info->pRasterizationState->rasterizerDiscardEnable;

   if (builder->rasterizer_discard) {
      builder->samples = VK_SAMPLE_COUNT_1_BIT;
   } else {
      builder->samples = create_info->pMultisampleState->rasterizationSamples;

      const struct panvk_render_pass *pass = panvk_render_pass_from_handle(create_info->renderPass);
      const struct panvk_subpass *subpass = &pass->subpasses[create_info->subpass];

      builder->use_depth_stencil_attachment =
         subpass->zs_attachment.idx != VK_ATTACHMENT_UNUSED;

      assert(subpass->color_count == create_info->pColorBlendState->attachmentCount);
      builder->active_color_attachments = 0;
      for (uint32_t i = 0; i < subpass->color_count; i++) {
         uint32_t idx = subpass->color_attachments[i].idx;
         if (idx == VK_ATTACHMENT_UNUSED)
            continue;

         builder->active_color_attachments |= 1 << i;
         builder->color_attachment_formats[i] = pass->attachments[idx].format;
      }
   }
}

VkResult
panvk_CreateGraphicsPipelines(VkDevice device,
                              VkPipelineCache pipelineCache,
                              uint32_t count,
                              const VkGraphicsPipelineCreateInfo *pCreateInfos,
                              const VkAllocationCallbacks *pAllocator,
                              VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(panvk_device, dev, device);
   VK_FROM_HANDLE(panvk_pipeline_cache, cache, pipelineCache);

   for (uint32_t i = 0; i < count; i++) {
      struct panvk_pipeline_builder builder;
      panvk_pipeline_builder_init_graphics(&builder, dev, cache,
                                           &pCreateInfos[i], pAllocator);

      struct panvk_pipeline *pipeline;
      VkResult result = panvk_pipeline_builder_build(&builder, &pipeline);
      panvk_pipeline_builder_finish(&builder);

      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            panvk_DestroyPipeline(device, pPipelines[j], pAllocator);
            pPipelines[j] = VK_NULL_HANDLE;
         }

         return result;
      }

      pPipelines[i] = panvk_pipeline_to_handle(pipeline);
   }

   return VK_SUCCESS;
}

VkResult
panvk_CreateComputePipelines(VkDevice _device,
                             VkPipelineCache pipelineCache,
                             uint32_t count,
                             const VkComputePipelineCreateInfo *pCreateInfos,
                             const VkAllocationCallbacks *pAllocator,
                             VkPipeline *pPipelines)
{
   panvk_stub();
   return VK_SUCCESS;
}

void
panvk_DestroyPipeline(VkDevice _device,
                      VkPipeline _pipeline,
                      const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_pipeline, pipeline, _pipeline);

   for (unsigned i = 0; i < ARRAY_SIZE(pipeline->blend_shaders); i++)
      ralloc_free(pipeline->blend_shaders[i].nir);

   panfrost_bo_unreference(pipeline->binary_bo);
   panfrost_bo_unreference(pipeline->state_bo);
   vk_object_free(&device->vk, pAllocator, pipeline);
}
