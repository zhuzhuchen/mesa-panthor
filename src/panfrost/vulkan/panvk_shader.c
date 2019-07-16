/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_shader.c which is:
 * Copyright © 2019 Google LLC
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

#include "panvk_private.h"

#include "nir_builder.h"
#include "spirv/nir_spirv.h"
#include "util/mesa-sha1.h"

#include "panfrost-quirks.h"
#include "pan_shader.h"

static nir_shader *
panvk_spirv_to_nir(const void *code,
                   size_t codesize,
                   gl_shader_stage stage,
                   const char *entry_point_name,
                   const VkSpecializationInfo *spec_info)
{
   /* TODO these are made-up */
   const struct spirv_to_nir_options spirv_options = {
      .caps = { false },
      .ubo_addr_format = nir_address_format_32bit_index_offset,
      .ssbo_addr_format = nir_address_format_32bit_index_offset,
   };

   /* convert VkSpecializationInfo */
   struct nir_spirv_specialization *spec = NULL;
   uint32_t num_spec = 0;
   if (spec_info && spec_info->mapEntryCount) {
      spec = malloc(sizeof(*spec) * spec_info->mapEntryCount);
      if (!spec)
         return NULL;

      for (uint32_t i = 0; i < spec_info->mapEntryCount; i++) {
         const VkSpecializationMapEntry *entry = &spec_info->pMapEntries[i];
         const void *data = spec_info->pData + entry->offset;
         assert(data + entry->size <= spec_info->pData + spec_info->dataSize);
         spec[i].id = entry->constantID;

         if (entry->size == 8)
            spec[i].value.u64 = *(const uint64_t *) data;
         else
            spec[i].value.u32 = *(const uint32_t *) data;

         spec[i].defined_on_module = false;
      }

      num_spec = spec_info->mapEntryCount;
   }

   nir_shader *nir = spirv_to_nir(code, codesize / sizeof(uint32_t), spec,
                                  num_spec, stage, entry_point_name,
                                  &spirv_options, &midgard_nir_options);

   free(spec);

   assert(nir->info.stage == stage);
   nir_validate_shader(nir, "after spirv_to_nir");

   return nir;
}

static unsigned
get_fixed_sampler_index(nir_deref_instr *deref,
                        struct panvk_shader *shader,
                        const struct panvk_pipeline_layout *layout)
{
   nir_variable *var = nir_deref_instr_get_variable(deref);
   unsigned set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   const struct panvk_descriptor_set_binding_layout *bind_layout =
      &layout->sets[set].layout->bindings[binding];

   shader->active_desc_sets |= 1u << set;
   return bind_layout->sampler_idx + layout->sets[set].sampler_offset;
}

static unsigned
get_fixed_texture_index(nir_deref_instr *deref,
                        struct panvk_shader *shader,
                        const struct panvk_pipeline_layout *layout)
{
   nir_variable *var = nir_deref_instr_get_variable(deref);
   unsigned set = var->data.descriptor_set;
   unsigned binding = var->data.binding;
   const struct panvk_descriptor_set_binding_layout *bind_layout =
      &layout->sets[set].layout->bindings[binding];

   shader->active_desc_sets |= 1u << set;
   return bind_layout->tex_idx + layout->sets[set].tex_offset;
}

static bool
lower_tex(nir_builder *b, nir_tex_instr *tex,
          struct panvk_shader *shader,
          const struct panvk_pipeline_layout *layout)
{
   bool progress = false;
   int sampler_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_sampler_deref);

   if (sampler_src_idx >= 0) {
      nir_deref_instr *deref = nir_src_as_deref(tex->src[sampler_src_idx].src);
      tex->sampler_index = get_fixed_sampler_index(deref, shader, layout);
      nir_tex_instr_remove_src(tex, sampler_src_idx);
      progress = true;
   }

   int tex_src_idx = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   if (tex_src_idx >= 0) {
      nir_deref_instr *deref = nir_src_as_deref(tex->src[tex_src_idx].src);
      tex->texture_index = get_fixed_texture_index(deref, shader, layout);
      nir_tex_instr_remove_src(tex, tex_src_idx);
      progress = true;
   }

   return progress;
}

static void
lower_vulkan_resource_index(nir_builder *b, nir_intrinsic_instr *intr,
                            struct panvk_shader *shader,
                            const struct panvk_pipeline_layout *layout)
{
   nir_ssa_def *vulkan_idx = intr->src[0].ssa;

   unsigned set = nir_intrinsic_desc_set(intr);
   unsigned binding = nir_intrinsic_binding(intr);
   struct panvk_descriptor_set_layout *set_layout = layout->sets[set].layout;
   struct panvk_descriptor_set_binding_layout *binding_layout =
      &set_layout->bindings[binding];
   unsigned base;

   shader->active_desc_sets |= 1u << set;

   switch (binding_layout->type) {
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      base = binding_layout->ubo_idx + layout->sets[set].ubo_offset;
      break;
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
   case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      base = binding_layout->ssbo_idx + layout->sets[set].ssbo_offset;
      break;
   default:
      unreachable("Invalid descriptor type");
      break;
   }

   nir_ssa_def *idx = nir_iadd(b, nir_imm_int(b, base), vulkan_idx);
   nir_ssa_def_rewrite_uses(&intr->dest.ssa, idx);
   nir_instr_remove(&intr->instr);
}

static void
lower_load_vulkan_descriptor(nir_builder *b, nir_intrinsic_instr *intrin)
{
   /* Loading the descriptor happens as part of the load/store instruction so
    * this is a no-op.
    */
   nir_ssa_def *val = nir_vec2(b, intrin->src[0].ssa, nir_imm_int(b, 0));
   nir_ssa_def_rewrite_uses(&intrin->dest.ssa, val);
   nir_instr_remove(&intrin->instr);
}

static bool
lower_intrinsic(nir_builder *b, nir_intrinsic_instr *intr,
                struct panvk_shader *shader,
                const struct panvk_pipeline_layout *layout)
{
   switch (intr->intrinsic) {
   case nir_intrinsic_vulkan_resource_index:
      lower_vulkan_resource_index(b, intr, shader, layout);
      return true;
   case nir_intrinsic_load_vulkan_descriptor:
      lower_load_vulkan_descriptor(b, intr);
      return true;
   default:
      break;
   }

   return false;
}

static bool
lower_impl(nir_function_impl *impl, struct panvk_shader *shader,
           const struct panvk_pipeline_layout *layout)
{
   nir_builder b;
   nir_builder_init(&b, impl);
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         b.cursor = nir_before_instr(instr);
         switch (instr->type) {
	 case nir_instr_type_tex:
            progress |= lower_tex(&b, nir_instr_as_tex(instr), shader, layout);
            break;
         case nir_instr_type_intrinsic:
            progress |= lower_intrinsic(&b, nir_instr_as_intrinsic(instr), shader, layout);
            break;
         default:
            break;
         }
      }
   }

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_none);
   else
      nir_metadata_preserve(impl, nir_metadata_all);

   return progress;
}

static bool
panvk_lower(nir_shader *nir, struct panvk_shader *shader,
            const struct panvk_pipeline_layout *layout)
{
   bool progress = false;

   nir_foreach_function(function, nir) {
      if (function->impl)
         progress |= lower_impl(function->impl, shader, layout);
   }

   return progress;
}

struct panvk_shader *
panvk_shader_create(struct panvk_device *dev,
                    gl_shader_stage stage,
                    const VkPipelineShaderStageCreateInfo *stage_info,
                    const struct panvk_pipeline_layout *layout,
                    unsigned sysval_ubo,
                    const VkAllocationCallbacks *alloc)
{
   const struct panvk_shader_module *module = panvk_shader_module_from_handle(stage_info->module);
   struct panvk_shader *shader;

   shader = vk_zalloc2(&dev->vk.alloc, alloc, sizeof(*shader), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!shader)
      return NULL;

   util_dynarray_init(&shader->binary, NULL);

   /* translate SPIR-V to NIR */
   assert(module->code_size % 4 == 0);
   nir_shader *nir = panvk_spirv_to_nir(module->code,
                                        module->code_size,
                                        stage, stage_info->pName,
                                        stage_info->pSpecializationInfo);
   if (!nir) {
      vk_free2(&dev->vk.alloc, alloc, shader);
      return NULL;
   }

   /* multi step inlining procedure */
   NIR_PASS_V(nir, nir_lower_variable_initializers, nir_var_function_temp);
   NIR_PASS_V(nir, nir_lower_returns);
   NIR_PASS_V(nir, nir_inline_functions);
   NIR_PASS_V(nir, nir_copy_prop);
   NIR_PASS_V(nir, nir_opt_deref);
   foreach_list_typed_safe(nir_function, func, node, &nir->functions) {
      if (!func->is_entrypoint)
         exec_node_remove(&func->node);
   }
   assert(exec_list_length(&nir->functions) == 1);
   NIR_PASS_V(nir, nir_lower_variable_initializers, ~nir_var_function_temp);

   /* Split member structs.  We do this before lower_io_to_temporaries so that
    * it doesn't lower system values to temporaries by accident.
    */
   NIR_PASS_V(nir, nir_split_var_copies);
   NIR_PASS_V(nir, nir_split_per_member_structs);

   NIR_PASS_V(nir, nir_remove_dead_variables,
              nir_var_shader_in | nir_var_shader_out |
              nir_var_system_value | nir_var_mem_shared,
              NULL);

   NIR_PASS_V(nir, nir_lower_io_to_temporaries,
              nir_shader_get_entrypoint(nir), true, true);

   NIR_PASS_V(nir, nir_lower_indirect_derefs,
              nir_var_shader_in | nir_var_shader_out,
              UINT32_MAX);

   NIR_PASS_V(nir, nir_opt_copy_prop_vars);
   NIR_PASS_V(nir, nir_opt_combine_stores, nir_var_all);

   NIR_PASS_V(nir, nir_lower_uniforms_to_ubo, 16);
   NIR_PASS_V(nir, nir_lower_explicit_io,
              nir_var_mem_ubo | nir_var_mem_ssbo,
              nir_address_format_32bit_index_offset);

   nir_assign_io_var_locations(nir, nir_var_shader_in, &nir->num_inputs, stage);
   nir_assign_io_var_locations(nir, nir_var_shader_out, &nir->num_outputs, stage);

   NIR_PASS_V(nir, nir_lower_system_values);
   NIR_PASS_V(nir, nir_lower_compute_system_values, NULL);

   NIR_PASS_V(nir, panvk_lower, shader, layout);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   if (unlikely(dev->physical_device->instance->debug_flags & PANVK_DEBUG_NIR)) {
      fprintf(stderr, "translated nir:\n");
      nir_print_shader(nir, stderr);
   }

   struct panfrost_device *pdev = &dev->physical_device->pdev;
   struct panfrost_compile_inputs inputs = {
      .gpu_id = pdev->gpu_id,
      .no_ubo_to_push = true,
      .sysval_ubo = sysval_ubo,
   };

   pan_shader_compile(pdev, nir, &inputs, &shader->binary, &shader->info);

   /* Patch the descriptor count */
   shader->info.ubo_count =
      shader->info.sysvals.sysval_count ? sysval_ubo + 1 : layout->num_ubos;
   shader->info.sampler_count = layout->num_samplers;
   shader->info.texture_count = layout->num_textures;

   shader->sysval_ubo = sysval_ubo;

   ralloc_free(nir);

   return shader;
}

void
panvk_shader_destroy(struct panvk_device *dev,
                     struct panvk_shader *shader,
                     const VkAllocationCallbacks *alloc)
{
   util_dynarray_fini(&shader->binary);
   vk_free2(&dev->vk.alloc, alloc, shader);
}

VkResult
panvk_CreateShaderModule(VkDevice _device,
                         const VkShaderModuleCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkShaderModule *pShaderModule)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_shader_module *module;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
   assert(pCreateInfo->flags == 0);
   assert(pCreateInfo->codeSize % 4 == 0);

   module = vk_object_zalloc(&device->vk, pAllocator,
                             sizeof(*module) + pCreateInfo->codeSize,
                             VK_OBJECT_TYPE_SHADER_MODULE);
   if (module == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   module->code_size = pCreateInfo->codeSize;
   memcpy(module->code, pCreateInfo->pCode, pCreateInfo->codeSize);

   _mesa_sha1_compute(module->code, module->code_size, module->sha1);

   *pShaderModule = panvk_shader_module_to_handle(module);

   return VK_SUCCESS;
}

void
panvk_DestroyShaderModule(VkDevice _device,
                          VkShaderModule _module,
                          const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_shader_module, module, _module);

   if (!module)
      return;

   vk_object_free(&device->vk, pAllocator, module);
}
