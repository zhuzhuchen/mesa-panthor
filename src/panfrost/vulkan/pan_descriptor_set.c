/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
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

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "util/mesa-sha1.h"
#include "vk_util.h"

#include "midgard_pack.h"

static int
binding_compare(const void *av, const void *bv)
{
   const VkDescriptorSetLayoutBinding *a =
      (const VkDescriptorSetLayoutBinding *) av;
   const VkDescriptorSetLayoutBinding *b =
      (const VkDescriptorSetLayoutBinding *) bv;

   return (a->binding < b->binding) ? -1 : (a->binding > b->binding) ? 1 : 0;
}

static VkDescriptorSetLayoutBinding *
create_sorted_bindings(const VkDescriptorSetLayoutBinding *bindings,
                       unsigned count)
{
   VkDescriptorSetLayoutBinding *sorted_bindings =
      malloc(count * sizeof(VkDescriptorSetLayoutBinding));
   if (!sorted_bindings)
      return NULL;

   memcpy(sorted_bindings, bindings,
          count * sizeof(VkDescriptorSetLayoutBinding));

   qsort(sorted_bindings, count, sizeof(VkDescriptorSetLayoutBinding),
         binding_compare);

   return sorted_bindings;
}

VkResult
pan_CreateDescriptorSetLayout(VkDevice _device,
                              const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                              const VkAllocationCallbacks *pAllocator,
                              VkDescriptorSetLayout *pSetLayout)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   struct pan_descriptor_set_layout *set_layout;

   assert(pCreateInfo->sType ==
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
   const VkDescriptorSetLayoutBindingFlagsCreateInfoEXT *variable_flags =
      vk_find_struct_const(
         pCreateInfo->pNext,
         DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO_EXT);

   uint32_t max_binding = 0;
   uint32_t immutable_sampler_count = 0;
   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      max_binding = MAX2(max_binding, pCreateInfo->pBindings[j].binding);
      if (pCreateInfo->pBindings[j].pImmutableSamplers)
         immutable_sampler_count += pCreateInfo->pBindings[j].descriptorCount;
   }

   uint32_t samplers_offset =
      sizeof(struct pan_descriptor_set_layout) +
      (max_binding + 1) * sizeof(set_layout->binding[0]);
   size_t size =
      samplers_offset + immutable_sampler_count * 4 * sizeof(uint32_t);

   set_layout = vk_object_zalloc(&device->vk, pAllocator, size,
                          VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT);
   if (!set_layout)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   set_layout->flags = pCreateInfo->flags;

   /* We just allocate all the samplers at the end of the struct */
   uint32_t *samplers = (uint32_t *) &set_layout->binding[max_binding + 1];
   (void) samplers; /* TODO: Use me */

   VkDescriptorSetLayoutBinding *bindings = create_sorted_bindings(
      pCreateInfo->pBindings, pCreateInfo->bindingCount);
   if (!bindings) {
      vk_object_free(&device->vk, pAllocator, set_layout);
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   set_layout->binding_count = max_binding + 1;
   set_layout->shader_stages = 0;
   set_layout->dynamic_shader_stages = 0;
   set_layout->has_immutable_samplers = false;
   set_layout->size = 0;

   memset(set_layout->binding, 0,
          size - sizeof(struct pan_descriptor_set_layout));

   uint32_t buffer_count = 0;
   uint32_t dynamic_offset_count = 0;

   for (uint32_t j = 0; j < pCreateInfo->bindingCount; j++) {
      const VkDescriptorSetLayoutBinding *binding = bindings + j;
      uint32_t b = binding->binding;
      uint32_t alignment;
      unsigned binding_buffer_count = 0;

      switch (binding->descriptorType) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         assert(!(pCreateInfo->flags &
                  VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR));
         set_layout->binding[b].dynamic_offset_count = 1;
         set_layout->dynamic_shader_stages |= binding->stageFlags;
         set_layout->binding[b].size = 0;
         binding_buffer_count = 1;
         alignment = 1;
         break;
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         set_layout->binding[b].size = 16;
         binding_buffer_count = 1;
         alignment = 16;
         break;
      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         /* main descriptor + fmask descriptor */
         set_layout->binding[b].size = 64;
         binding_buffer_count = 1;
         alignment = 32;
         break;
      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
         /* main descriptor + fmask descriptor + sampler */
         set_layout->binding[b].size = 96;
         binding_buffer_count = 1;
         alignment = 32;
         break;
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         set_layout->binding[b].size = 16;
         alignment = 16;
         break;
      default:
         unreachable("unknown descriptor type\n");
         break;
      }

      set_layout->size = ALIGN_POT(set_layout->size, alignment);
      set_layout->binding[b].type = binding->descriptorType;
      set_layout->binding[b].array_size = binding->descriptorCount;
      set_layout->binding[b].offset = set_layout->size;
      set_layout->binding[b].buffer_offset = buffer_count;
      set_layout->binding[b].dynamic_offset_offset = dynamic_offset_count;

      if (variable_flags && binding->binding < variable_flags->bindingCount &&
          (variable_flags->pBindingFlags[binding->binding] &
           VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT_EXT)) {
         assert(!binding->pImmutableSamplers); /* Terribly ill defined  how
                                                  many samplers are valid */
         assert(binding->binding == max_binding);

         set_layout->has_variable_descriptors = true;
      }

      if (binding->pImmutableSamplers) {
         set_layout->binding[b].immutable_samplers_offset = samplers_offset;
         set_layout->has_immutable_samplers = true;
      }

      set_layout->size +=
         binding->descriptorCount * set_layout->binding[b].size;
      buffer_count += binding->descriptorCount * binding_buffer_count;
      dynamic_offset_count += binding->descriptorCount *
                              set_layout->binding[b].dynamic_offset_count;
      set_layout->shader_stages |= binding->stageFlags;
   }

   free(bindings);

   set_layout->buffer_count = buffer_count;
   set_layout->dynamic_offset_count = dynamic_offset_count;

   *pSetLayout = pan_descriptor_set_layout_to_handle(set_layout);

   return VK_SUCCESS;
}

void
pan_DestroyDescriptorSetLayout(VkDevice _device,
                               VkDescriptorSetLayout _set_layout,
                               const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

void
pan_GetDescriptorSetLayoutSupport(VkDevice device,
                                  const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
                                  VkDescriptorSetLayoutSupport *pSupport)
{
   assert(0);
}

/*
 * Pipeline layouts.  These have nothing to do with the pipeline.  They are
 * just multiple descriptor set layouts pasted together.
 */

VkResult
pan_CreatePipelineLayout(VkDevice _device,
                         const VkPipelineLayoutCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkPipelineLayout *pPipelineLayout)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   struct pan_pipeline_layout *layout;
   struct mesa_sha1 ctx;

   assert(pCreateInfo->sType ==
          VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);

   layout = vk_object_alloc(&device->vk, pAllocator, sizeof(*layout),
                            VK_OBJECT_TYPE_PIPELINE_LAYOUT);
   if (layout == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   layout->num_sets = pCreateInfo->setLayoutCount;

   unsigned dynamic_offset_count = 0;

   _mesa_sha1_init(&ctx);
   for (uint32_t set = 0; set < pCreateInfo->setLayoutCount; set++) {
      PAN_FROM_HANDLE(pan_descriptor_set_layout, set_layout,
                     pCreateInfo->pSetLayouts[set]);
      layout->set[set].layout = set_layout;

      layout->set[set].dynamic_offset_start = dynamic_offset_count;
      for (uint32_t b = 0; b < set_layout->binding_count; b++) {
         dynamic_offset_count += set_layout->binding[b].array_size *
                                 set_layout->binding[b].dynamic_offset_count;
         if (set_layout->binding[b].immutable_samplers_offset)
            _mesa_sha1_update(
               &ctx,
               pan_immutable_samplers(set_layout, set_layout->binding + b),
               set_layout->binding[b].array_size * 4 * sizeof(uint32_t));
      }
      _mesa_sha1_update(
         &ctx, set_layout->binding,
         sizeof(set_layout->binding[0]) * set_layout->binding_count);
   }

   layout->dynamic_offset_count = dynamic_offset_count;
   layout->push_constant_size = 0;

   for (unsigned i = 0; i < pCreateInfo->pushConstantRangeCount; ++i) {
      const VkPushConstantRange *range = pCreateInfo->pPushConstantRanges + i;
      layout->push_constant_size =
         MAX2(layout->push_constant_size, range->offset + range->size);
   }

   layout->push_constant_size = ALIGN_POT(layout->push_constant_size, 16);
   _mesa_sha1_update(&ctx, &layout->push_constant_size,
                     sizeof(layout->push_constant_size));
   _mesa_sha1_final(&ctx, layout->sha1);
   *pPipelineLayout = pan_pipeline_layout_to_handle(layout);

   return VK_SUCCESS;
}

void
pan_DestroyPipelineLayout(VkDevice _device,
                          VkPipelineLayout _pipelineLayout,
                          const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

static uint32_t
descriptor_size(enum VkDescriptorType type)
{
   /* STUB */
   return 128;
}

VkResult
pan_CreateDescriptorPool(VkDevice _device,
                         const VkDescriptorPoolCreateInfo *pCreateInfo,
                         const VkAllocationCallbacks *pAllocator,
                         VkDescriptorPool *pDescriptorPool)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   struct pan_descriptor_pool *pool;
   uint64_t size = sizeof(struct pan_descriptor_pool);
   uint64_t bo_size = 0, bo_count = 0, range_count = 0;

   for (unsigned i = 0; i < pCreateInfo->poolSizeCount; ++i) {
      if (pCreateInfo->pPoolSizes[i].type != VK_DESCRIPTOR_TYPE_SAMPLER)
         bo_count += pCreateInfo->pPoolSizes[i].descriptorCount;

      switch(pCreateInfo->pPoolSizes[i].type) {
      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
         range_count += pCreateInfo->pPoolSizes[i].descriptorCount;
      default:
         break;
      }

      bo_size += descriptor_size(pCreateInfo->pPoolSizes[i].type) *
                           pCreateInfo->pPoolSizes[i].descriptorCount;
   }

   if (!(pCreateInfo->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)) {
      uint64_t host_size = pCreateInfo->maxSets * sizeof(struct pan_descriptor_set);
      host_size += sizeof(struct pan_bo*) * bo_count;
      host_size += sizeof(struct pan_descriptor_range) * range_count;
      size += host_size;
   } else {
      size += sizeof(struct pan_descriptor_pool_entry) * pCreateInfo->maxSets;
   }

   pool = vk_object_zalloc(&device->vk, pAllocator, size,
                    VK_OBJECT_TYPE_DESCRIPTOR_POOL);
   if (!pool)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   if (!(pCreateInfo->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)) {
      pool->host_memory_base = (uint8_t*)pool + sizeof(struct pan_descriptor_pool);
      pool->host_memory_ptr = pool->host_memory_base;
      pool->host_memory_end = (uint8_t*)pool + size;
   }

   if (bo_size) {
      VkResult ret;

      ret = pan_bo_init_new(device, &pool->bo, bo_size, 0);
      assert(ret == VK_SUCCESS);

      ret = pan_bo_map(device, &pool->bo);
      assert(ret == VK_SUCCESS);
   }
   pool->size = ALIGN_POT(bo_size, 4096);
   pool->max_entry_count = pCreateInfo->maxSets;
   printf("%s:%i pool %p size %" PRIx64 " max entry count %d\n", __func__, __LINE__, pool, pool->size, pool->max_entry_count);

   *pDescriptorPool = pan_descriptor_pool_to_handle(pool);
   return VK_SUCCESS;
}

void
pan_DestroyDescriptorPool(VkDevice _device,
                          VkDescriptorPool _pool,
                          const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

VkResult
pan_ResetDescriptorPool(VkDevice _device,
                        VkDescriptorPool descriptorPool,
                        VkDescriptorPoolResetFlags flags)
{
   assert(0);
   return VK_SUCCESS;
}

static VkResult
pan_descriptor_set_create(struct pan_device *device,
            struct pan_descriptor_pool *pool,
            const struct pan_descriptor_set_layout *layout,
            const uint32_t *variable_count,
            struct pan_descriptor_set **out_set)
{
   struct pan_descriptor_set *set;
   uint32_t buffer_count = layout->buffer_count;
   if (variable_count) {
      unsigned stride = 1;
      if (layout->binding[layout->binding_count - 1].type == VK_DESCRIPTOR_TYPE_SAMPLER ||
          layout->binding[layout->binding_count - 1].type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT)
         stride = 0;
      buffer_count = layout->binding[layout->binding_count - 1].buffer_offset +
                     *variable_count * stride;
   }
   unsigned range_offset = sizeof(struct pan_descriptor_set) +
      sizeof(struct pan_bo *) * buffer_count;
   unsigned mem_size = range_offset +
      sizeof(struct pan_descriptor_range) * layout->dynamic_offset_count;

   if (pool->host_memory_base) {
      assert(pool->host_memory_end - pool->host_memory_ptr >= mem_size);
      if (pool->host_memory_end - pool->host_memory_ptr < mem_size)
         return vk_error(device->instance, VK_ERROR_OUT_OF_POOL_MEMORY);

      set = (struct pan_descriptor_set*)pool->host_memory_ptr;
      pool->host_memory_ptr += mem_size;
   } else {
      set = vk_alloc2(&device->vk.alloc, NULL, mem_size, 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      assert(set);
      if (!set)
         return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   memset(set, 0, mem_size);
   vk_object_base_init(&device->vk, &set->base, VK_OBJECT_TYPE_DESCRIPTOR_SET);

   if (layout->dynamic_offset_count) {
      set->dynamic_descriptors = (struct pan_descriptor_range*)((uint8_t*)set + range_offset);
   }

   set->layout = layout;
   uint32_t layout_size = layout->size;
   if (variable_count) {
      assert(layout->has_variable_descriptors);
      uint32_t stride = layout->binding[layout->binding_count - 1].size;
      if (layout->binding[layout->binding_count - 1].type == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT)
         stride = 1;

      layout_size = layout->binding[layout->binding_count - 1].offset +
                    *variable_count * stride;
   }

   printf("%s:%i pool %p size %" PRIx64 " max entry count %d\n", __func__, __LINE__, pool, pool->size, pool->max_entry_count);
   if (layout_size) {
      set->size = layout_size;

      assert(pool->host_memory_base || pool->entry_count < pool->max_entry_count);
      if (!pool->host_memory_base && pool->entry_count == pool->max_entry_count) {
         vk_object_free(&device->vk, NULL, set);
         return vk_error(device->instance, VK_ERROR_OUT_OF_POOL_MEMORY);
      }

      printf("%s:%i pool->size %" PRIx64 " current_offset %" PRIx64 " layout_size %d\n", __func__, __LINE__, pool->size, pool->current_offset, layout_size);
      /* try to allocate linearly first, so that we don't spend
       * time looking for gaps if the app only allocates &
       * resets via the pool. */
      if (pool->current_offset + layout_size <= pool->size) {
         set->mapped_ptr = (uint32_t*)(pool->bo.map + pool->current_offset);
         set->va = pool->bo.iova + pool->current_offset;
         if (!pool->host_memory_base) {
            pool->entries[pool->entry_count].offset = pool->current_offset;
            pool->entries[pool->entry_count].size = layout_size;
            pool->entries[pool->entry_count].set = set;
            pool->entry_count++;
         }
         pool->current_offset += layout_size;
      } else if (!pool->host_memory_base) {
         uint64_t offset = 0;
         int index;

         for (index = 0; index < pool->entry_count; ++index) {
            if (pool->entries[index].offset - offset >= layout_size)
               break;
            offset = pool->entries[index].offset + pool->entries[index].size;
         }

         assert(pool->size - offset >= layout_size);
         if (pool->size - offset < layout_size) {
            vk_object_free(&device->vk, NULL, set);
            return vk_error(device->instance, VK_ERROR_OUT_OF_POOL_MEMORY);
         }

         set->mapped_ptr = (uint32_t*)(pool->bo.map + offset);
         set->va = pool->bo.iova + offset;
         memmove(&pool->entries[index + 1], &pool->entries[index],
            sizeof(pool->entries[0]) * (pool->entry_count - index));
         pool->entries[index].offset = offset;
         pool->entries[index].size = layout_size;
         pool->entries[index].set = set;
         pool->entry_count++;
      } else {
	 assert(0);
         return vk_error(device->instance, VK_ERROR_OUT_OF_POOL_MEMORY);
      }
   }

   *out_set = set;
   return VK_SUCCESS;
}

VkResult
pan_AllocateDescriptorSets(VkDevice _device,
                           const VkDescriptorSetAllocateInfo *pAllocateInfo,
                           VkDescriptorSet *pDescriptorSets)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   PAN_FROM_HANDLE(pan_descriptor_pool, pool, pAllocateInfo->descriptorPool);

   VkResult result = VK_SUCCESS;
   uint32_t i;
   struct pan_descriptor_set *set = NULL;

   const VkDescriptorSetVariableDescriptorCountAllocateInfoEXT *variable_counts =
      vk_find_struct_const(pAllocateInfo->pNext, DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO_EXT);
   const uint32_t zero = 0;

   /* allocate a set of buffers for each shader to contain descriptors */
   for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
      PAN_FROM_HANDLE(pan_descriptor_set_layout, layout,
             pAllocateInfo->pSetLayouts[i]);

      const uint32_t *variable_count = NULL;
      if (variable_counts) {
         if (i < variable_counts->descriptorSetCount)
            variable_count = variable_counts->pDescriptorCounts + i;
         else
            variable_count = &zero;
      }

      assert(!(layout->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR));

      result = pan_descriptor_set_create(device, pool, layout, variable_count, &set);
      if (result != VK_SUCCESS)
         break;

      pDescriptorSets[i] = pan_descriptor_set_to_handle(set);
   }

   if (result != VK_SUCCESS) {
      pan_FreeDescriptorSets(_device, pAllocateInfo->descriptorPool,
               i, pDescriptorSets);
      for (i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
         pDescriptorSets[i] = VK_NULL_HANDLE;
      }
   }
   return result;
}

VkResult
pan_FreeDescriptorSets(VkDevice _device,
                       VkDescriptorPool descriptorPool,
                       uint32_t count,
                       const VkDescriptorSet *pDescriptorSets)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_UpdateDescriptorSets(VkDevice _device,
                         uint32_t descriptorWriteCount,
                         const VkWriteDescriptorSet *pDescriptorWrites,
                         uint32_t descriptorCopyCount,
                         const VkCopyDescriptorSet *pDescriptorCopies)
{
   assert(0);
}

VkResult
pan_CreateDescriptorUpdateTemplate(VkDevice _device,
                                   const VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
                                   const VkAllocationCallbacks *pAllocator,
                                   VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_DestroyDescriptorUpdateTemplate(VkDevice _device,
                                    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                    const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

void
pan_UpdateDescriptorSetWithTemplate(VkDevice _device,
                                    VkDescriptorSet descriptorSet,
                                    VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                    const void *pData)
{
   assert(0);
}

VkResult
pan_CreateSamplerYcbcrConversion(VkDevice device,
                                 const VkSamplerYcbcrConversionCreateInfo *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkSamplerYcbcrConversion *pYcbcrConversion)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_DestroySamplerYcbcrConversion(VkDevice device,
                                  VkSamplerYcbcrConversion ycbcrConversion,
                                  const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}
