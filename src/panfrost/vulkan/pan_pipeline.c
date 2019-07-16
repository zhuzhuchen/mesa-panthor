/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
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

#include "pan_private.h"

#include "nir/nir.h"
#include "nir/nir_builder.h"
#include "spirv/nir_spirv.h"
#include "util/debug.h"
#include "util/mesa-sha1.h"
#include "util/u_atomic.h"
#include "vk_format.h"
#include "vk_util.h"

struct pan_pipeline_builder
{
   struct pan_device *device;
   struct pan_pipeline_cache *cache;
   const VkAllocationCallbacks *alloc;
   const VkGraphicsPipelineCreateInfo *create_info;

   struct pan_shader *shaders[MESA_SHADER_STAGES];
   uint32_t shader_offsets[MESA_SHADER_STAGES];
   uint32_t shader_total_size;

   bool rasterizer_discard;
   /* these states are affectd by rasterizer_discard */
   VkSampleCountFlagBits samples;
   bool use_depth_stencil_attachment;
   bool use_color_attachments;
   uint32_t color_attachment_count;
   VkFormat color_attachment_formats[MAX_RTS];
};

static VkResult
pan_pipeline_builder_create_pipeline(struct pan_pipeline_builder *builder,
                                     struct pan_pipeline **out_pipeline)
{
   struct pan_device *dev = builder->device;

   struct pan_pipeline *pipeline =
      vk_zalloc2(&dev->alloc, builder->alloc, sizeof(*pipeline), 8,
                 VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!pipeline)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   *out_pipeline = pipeline;

   return VK_SUCCESS;
}

static void
pan_pipeline_builder_finish(struct pan_pipeline_builder *builder)
{
   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      if (!builder->shaders[i])
         continue;
      pan_shader_destroy(builder->device, builder->shaders[i], builder->alloc);
   }
}

static gl_shader_stage
pan_shader_stage(VkShaderStageFlagBits stage)
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
pan_pipeline_builder_compile_shaders(struct pan_pipeline_builder *builder)
{
   const VkPipelineShaderStageCreateInfo *stage_infos[MESA_SHADER_STAGES] = {
      NULL
   };
   for (uint32_t i = 0; i < builder->create_info->stageCount; i++) {
      gl_shader_stage stage = pan_shader_stage(builder->create_info->pStages[i].stage);
      stage_infos[stage] = &builder->create_info->pStages[i];
   }

   struct pan_shader_compile_options options = {
      .optimize = !(builder->create_info->flags &
                    VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT),
      .include_binning_pass = true,
   };

   /* compile shaders in reverse order */
   struct pan_shader *next_stage_shader = NULL;
   for (gl_shader_stage stage = MESA_SHADER_STAGES - 1;
        stage > MESA_SHADER_NONE; stage--) {
      const VkPipelineShaderStageCreateInfo *stage_info = stage_infos[stage];
      if (!stage_info)
         continue;

      struct pan_shader *shader;

      shader = pan_shader_create(builder->device, stage, stage_info, builder->alloc);
      if (!shader)
         return VK_ERROR_OUT_OF_HOST_MEMORY;

      /*
      VkResult result = pan_shader_compile(builder->device, shader,
                                           next_stage_shader,
                                           &options, builder->alloc);
      if (result != VK_SUCCESS)
         return result;
      */

      builder->shaders[stage] = shader;
      builder->shader_offsets[stage] = builder->shader_total_size;
      builder->shader_total_size += util_dynarray_num_elements(&shader->mprogram.compiled, uint8_t);
      builder->shader_total_size = ALIGN_POT(builder->shader_total_size, 4096);
      /*
      builder->shader_offsets[stage] = builder->shader_total_size;
      builder->shader_total_size += sizeof(uint32_t) *
                                    shader->variants[0].info.sizedwords;
      */
      next_stage_shader = shader;
   }

   /*
   if (builder->shaders[MESA_SHADER_VERTEX]->has_binning_pass) {
      const struct pan_shader *vs = builder->shaders[MESA_SHADER_VERTEX];
      builder->binning_vs_offset = builder->shader_total_size;
      builder->shader_total_size +=
         sizeof(uint32_t) * vs->variants[1].info.sizedwords;
   }
   */

   return VK_SUCCESS;
}

static VkResult
pan_pipeline_builder_upload_shaders(struct pan_pipeline_builder *builder,
                                    struct pan_pipeline *pipeline)
{
   struct pan_bo *bo = &pipeline->program.binary_bo;

   VkResult result =
      pan_bo_init_new(builder->device, bo, builder->shader_total_size, 0);
   if (result != VK_SUCCESS)
      return result;

   result = pan_bo_map(builder->device, bo);
   if (result != VK_SUCCESS)
      return result;

   for (uint32_t i = 0; i < MESA_SHADER_STAGES; i++) {
      const struct pan_shader *shader = builder->shaders[i];
      if (!shader)
         continue;

      memcpy(bo->map + builder->shader_offsets[i],
             util_dynarray_element(&shader->mprogram.compiled, uint8_t, 0),
	     util_dynarray_num_elements(&shader->mprogram.compiled, uint8_t));
   }

/*
   if (builder->shaders[MESA_SHADER_VERTEX]->has_binning_pass) {
      const struct pan_shader *vs = builder->shaders[MESA_SHADER_VERTEX];
      memcpy(bo->map + builder->binning_vs_offset, vs->binning_binary,
             sizeof(uint32_t) * vs->variants[1].info.sizedwords);
   }
*/

   return VK_SUCCESS;
}

static VkResult
pan_pipeline_builder_build(struct pan_pipeline_builder *builder,
                          struct pan_pipeline **pipeline)
{
   VkResult result = pan_pipeline_builder_create_pipeline(builder, pipeline);
   if (result != VK_SUCCESS)
      return result;

   /* compile and upload shaders */
   result = pan_pipeline_builder_compile_shaders(builder);
//   if (result == VK_SUCCESS)
//      result = pan_pipeline_builder_upload_shaders(builder, *pipeline);
   /*
   if (result != VK_SUCCESS) {
      pan_pipeline_finish(*pipeline, builder->device, builder->alloc);
      vk_free2(&builder->device->alloc, builder->alloc, *pipeline);
      *pipeline = VK_NULL_HANDLE;

      return result;
   }

   pan_pipeline_builder_parse_dynamic(builder, *pipeline);
   pan_pipeline_builder_parse_shader_stages(builder, *pipeline);
   pan_pipeline_builder_parse_vertex_input(builder, *pipeline);
   pan_pipeline_builder_parse_input_assembly(builder, *pipeline);
   pan_pipeline_builder_parse_viewport(builder, *pipeline);
   pan_pipeline_builder_parse_rasterization(builder, *pipeline);
   pan_pipeline_builder_parse_depth_stencil(builder, *pipeline);
   pan_pipeline_builder_parse_multisample_and_color_blend(builder, *pipeline);
   */

   /* we should have reserved enough space upfront such that the CS never
    * grows
    */
   //assert((*pipeline)->cs.bo_count == 1);

   return VK_SUCCESS;
}

static void
pan_pipeline_builder_init_graphics(struct pan_pipeline_builder *builder,
                                   struct pan_device *dev,
                                   struct pan_pipeline_cache *cache,
                                   const VkGraphicsPipelineCreateInfo *create_info,
                                   const VkAllocationCallbacks *alloc)
{
   *builder = (struct pan_pipeline_builder) {
      .device = dev,
      .cache = cache,
      .create_info = create_info,
      .alloc = alloc,
   };

   builder->rasterizer_discard =
      create_info->pRasterizationState->rasterizerDiscardEnable;

   if (builder->rasterizer_discard) {
      builder->samples = VK_SAMPLE_COUNT_1_BIT;
   } else {
      builder->samples = create_info->pMultisampleState->rasterizationSamples;

      const struct pan_render_pass *pass = pan_render_pass_from_handle(create_info->renderPass);
      const struct pan_subpass *subpass = &pass->subpasses[create_info->subpass];

      builder->use_depth_stencil_attachment = subpass->depth_stencil_attachment.attachment != VK_ATTACHMENT_UNUSED;

      assert(subpass->color_count == create_info->pColorBlendState->attachmentCount);
      builder->color_attachment_count = subpass->color_count;
      for (uint32_t i = 0; i < subpass->color_count; i++) {
         const uint32_t a = subpass->color_attachments[i].attachment;
         if (a == VK_ATTACHMENT_UNUSED)
            continue;

         builder->color_attachment_formats[i] = pass->attachments[a].format;
         builder->use_color_attachments = true;
      }
   }
}

VkResult
pan_CreateGraphicsPipelines(VkDevice device,
                            VkPipelineCache pipelineCache,
                            uint32_t count,
                            const VkGraphicsPipelineCreateInfo *pCreateInfos,
                            const VkAllocationCallbacks *pAllocator,
                            VkPipeline *pPipelines)
{
   PAN_FROM_HANDLE(pan_device, dev, device);
   PAN_FROM_HANDLE(pan_pipeline_cache, cache, pipelineCache);

   for (uint32_t i = 0; i < count; i++) {
      struct pan_pipeline_builder builder;
      pan_pipeline_builder_init_graphics(&builder, dev, cache,
                                        &pCreateInfos[i], pAllocator);

      struct pan_pipeline *pipeline;
      VkResult result = pan_pipeline_builder_build(&builder, &pipeline);
      pan_pipeline_builder_finish(&builder);

      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++) {
            pan_DestroyPipeline(device, pPipelines[j], pAllocator);
            pPipelines[j] = VK_NULL_HANDLE;
         }

         return result;
      }

      pPipelines[i] = pan_pipeline_to_handle(pipeline);
   }
//   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_CreateComputePipelines(VkDevice _device,
                           VkPipelineCache pipelineCache,
                           uint32_t count,
                           const VkComputePipelineCreateInfo *pCreateInfos,
                           const VkAllocationCallbacks *pAllocator,
                           VkPipeline *pPipelines)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_DestroyPipeline(VkDevice _device,
                    VkPipeline _pipeline,
                    const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}
