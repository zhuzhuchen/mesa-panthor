/*
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

#include "pan_private.h"

#include "spirv/nir_spirv.h"
#include "util/mesa-sha1.h"

#include "midgard/midgard_compile.h"
#include "bifrost/bifrost_compile.h"

static nir_shader *
pan_spirv_to_nir(const void *code,
                 size_t codesize,
                 gl_shader_stage stage,
                 const char *entry_point_name,
                 const VkSpecializationInfo *spec_info)
{
   /* TODO these are made-up */
   const struct spirv_to_nir_options spirv_options = {
//      .lower_ubo_ssbo_access_to_offsets = true,
      .caps = { false },
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

struct pan_shader *
pan_shader_create(struct pan_device *dev,
                  gl_shader_stage stage,
                  const VkPipelineShaderStageCreateInfo *stage_info,
                  const VkAllocationCallbacks *alloc)
{
   const struct pan_shader_module *module = pan_shader_module_from_handle(stage_info->module);
   struct pan_shader *shader;

   shader = vk_zalloc2(&dev->vk.alloc, alloc, sizeof(*shader), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_COMMAND);
   if (!shader)
      return NULL;

   /* translate SPIR-V to NIR */
   assert(module->code_size % 4 == 0);
   nir_shader *nir = pan_spirv_to_nir(module->code,
                                      module->code_size,
                                      stage, stage_info->pName,
				      stage_info->pSpecializationInfo);
   if (!nir) {
      vk_free2(&dev->vk.alloc, alloc, shader);
      return NULL;
   }

   if (unlikely(dev->physical_device->instance->debug_flags & PAN_DEBUG_NIR)) {
      fprintf(stderr, "translated nir:\n");
      nir_print_shader(nir, stderr);
   }

   /*
   if (s->info.stage == MESA_SHADER_FRAGMENT) {
      if (state->alpha_state.enabled) {
         NIR_PASS_V(s, nir_lower_alpha_test, state->alpha_state.func, false);
      }
   }
   */

   struct panfrost_compile_inputs inputs = {
           .gpu_id = dev->physical_device->gpu_id,
   };

   if (dev->physical_device->arch >= 6)
           shader->mprogram = *bifrost_compile_shader_nir(NULL, nir, &inputs);
   else
           shader->mprogram = *midgard_compile_shader_nir(NULL, nir, &inputs);

   return shader;
}

void
pan_shader_destroy(struct pan_device *dev,
                   struct pan_shader *shader,
                   const VkAllocationCallbacks *alloc)
{
   util_dynarray_fini(&shader->mprogram.compiled);
   //assert(0);
}

VkResult
pan_CreateShaderModule(VkDevice _device,
                       const VkShaderModuleCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkShaderModule *pShaderModule)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   struct pan_shader_module *module;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
   assert(pCreateInfo->flags == 0);
   assert(pCreateInfo->codeSize % 4 == 0);

   module = vk_object_alloc(&device->vk, pAllocator,
                      sizeof(*module) + pCreateInfo->codeSize,
                      VK_OBJECT_TYPE_SHADER_MODULE);
   if (module == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   module->code_size = pCreateInfo->codeSize;
   memcpy(module->code, pCreateInfo->pCode, pCreateInfo->codeSize);

   _mesa_sha1_compute(module->code, module->code_size, module->sha1);

   *pShaderModule = pan_shader_module_to_handle(module);

   return VK_SUCCESS;
}

void
pan_DestroyShaderModule(VkDevice _device,
                        VkShaderModule _module,
                        const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}
