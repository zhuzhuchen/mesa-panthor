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

#include "vk_format.h"
#include "vk_util.h"

static VkResult
pan_create_cmd_buffer(struct pan_device *device,
                     struct pan_cmd_pool *pool,
                     VkCommandBufferLevel level,
                     VkCommandBuffer *pCommandBuffer)
{
   struct pan_cmd_buffer *cmd_buffer;

   cmd_buffer = vk_object_zalloc(&device->vk, NULL, sizeof(*cmd_buffer),
                                 VK_OBJECT_TYPE_COMMAND_BUFFER);
   if (cmd_buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   cmd_buffer->device = device;
   cmd_buffer->pool = pool;
   cmd_buffer->level = level;

   if (pool) {
      list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);
      cmd_buffer->queue_family_index = pool->queue_family_index;

   } else {
      /* Init the pool_link so we can safely call list_del when we destroy
       * the command buffer
       */
      list_inithead(&cmd_buffer->pool_link);
      cmd_buffer->queue_family_index = PAN_QUEUE_GENERAL;
   }

   /* TODO: init what? */

   *pCommandBuffer = pan_cmd_buffer_to_handle(cmd_buffer);

   return VK_SUCCESS;
}

static void
pan_cmd_buffer_destroy(struct pan_cmd_buffer *cmd_buffer)
{
   list_del(&cmd_buffer->pool_link);

   /* TODO: finish? */

   vk_object_free(&cmd_buffer->device->vk, &cmd_buffer->pool->alloc, cmd_buffer);
}

static VkResult
pan_reset_cmd_buffer(struct pan_cmd_buffer *cmd_buffer)
{
   cmd_buffer->record_result = VK_SUCCESS;

   /* TODO: what is there to reset? */

   for (unsigned i = 0; i < MAX_BIND_POINTS; i++)
      memset(&cmd_buffer->descriptors[i].sets, 0, sizeof(cmd_buffer->descriptors[i].sets));

   cmd_buffer->status = PAN_CMD_BUFFER_STATUS_INITIAL;

   return cmd_buffer->record_result;
}

VkResult
pan_AllocateCommandBuffers(VkDevice _device,
                           const VkCommandBufferAllocateInfo *pAllocateInfo,
                           VkCommandBuffer *pCommandBuffers)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   PAN_FROM_HANDLE(pan_cmd_pool, pool, pAllocateInfo->commandPool);

   VkResult result = VK_SUCCESS;
   uint32_t i;

   for (i = 0; i < pAllocateInfo->commandBufferCount; i++) {

      if (!list_is_empty(&pool->free_cmd_buffers)) {
         struct pan_cmd_buffer *cmd_buffer = list_first_entry(
            &pool->free_cmd_buffers, struct pan_cmd_buffer, pool_link);

         list_del(&cmd_buffer->pool_link);
         list_addtail(&cmd_buffer->pool_link, &pool->cmd_buffers);

         result = pan_reset_cmd_buffer(cmd_buffer);
         cmd_buffer->level = pAllocateInfo->level;

         pCommandBuffers[i] = pan_cmd_buffer_to_handle(cmd_buffer);
      } else {
         result = pan_create_cmd_buffer(device, pool, pAllocateInfo->level,
                                       &pCommandBuffers[i]);
      }
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      pan_FreeCommandBuffers(_device, pAllocateInfo->commandPool, i,
                            pCommandBuffers);

      /* From the Vulkan 1.0.66 spec:
       *
       * "vkAllocateCommandBuffers can be used to create multiple
       *  command buffers. If the creation of any of those command
       *  buffers fails, the implementation must destroy all
       *  successfully created command buffer objects from this
       *  command, set all entries of the pCommandBuffers array to
       *  NULL and return the error."
       */
      memset(pCommandBuffers, 0,
             sizeof(*pCommandBuffers) * pAllocateInfo->commandBufferCount);
   }

   return result;
}

void
pan_FreeCommandBuffers(VkDevice device,
                       VkCommandPool commandPool,
                       uint32_t commandBufferCount,
                       const VkCommandBuffer *pCommandBuffers)
{
   pan_finishme("unimplemented!");
}

VkResult
pan_ResetCommandBuffer(VkCommandBuffer commandBuffer,
                       VkCommandBufferResetFlags flags)
{
   pan_finishme("unimplemented!");
   return VK_SUCCESS;
}

VkResult
pan_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                       const VkCommandBufferBeginInfo *pBeginInfo)
{
   pan_finishme("unimplemented!");
   return VK_SUCCESS;
}

void
pan_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                         uint32_t firstBinding,
                         uint32_t bindingCount,
                         const VkBuffer *pBuffers,
                         const VkDeviceSize *pOffsets)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                       VkBuffer buffer,
                       VkDeviceSize offset,
                       VkIndexType indexType)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdBindDescriptorSets(VkCommandBuffer commandBuffer,
                          VkPipelineBindPoint pipelineBindPoint,
                          VkPipelineLayout _layout,
                          uint32_t firstSet,
                          uint32_t descriptorSetCount,
                          const VkDescriptorSet *pDescriptorSets,
                          uint32_t dynamicOffsetCount,
                          const uint32_t *pDynamicOffsets)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdPushConstants(VkCommandBuffer commandBuffer,
                     VkPipelineLayout layout,
                     VkShaderStageFlags stageFlags,
                     uint32_t offset,
                     uint32_t size,
                     const void *pValues)
{
   pan_finishme("unimplemented!");
}

VkResult
pan_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   pan_finishme("unimplemented!");
   return VK_SUCCESS;
}

void
pan_CmdBindPipeline(VkCommandBuffer commandBuffer,
                    VkPipelineBindPoint pipelineBindPoint,
                    VkPipeline _pipeline)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetViewport(VkCommandBuffer commandBuffer,
                   uint32_t firstViewport,
                   uint32_t viewportCount,
                   const VkViewport *pViewports)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetScissor(VkCommandBuffer commandBuffer,
                  uint32_t firstScissor,
                  uint32_t scissorCount,
                  const VkRect2D *pScissors)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetDepthBias(VkCommandBuffer commandBuffer,
                    float depthBiasConstantFactor,
                    float depthBiasClamp,
                    float depthBiasSlopeFactor)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetBlendConstants(VkCommandBuffer commandBuffer,
                         const float blendConstants[4])
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetDepthBounds(VkCommandBuffer commandBuffer,
                      float minDepthBounds,
                      float maxDepthBounds)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer,
                             VkStencilFaceFlags faceMask,
                             uint32_t compareMask)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer,
                           VkStencilFaceFlags faceMask,
                           uint32_t writeMask)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetStencilReference(VkCommandBuffer commandBuffer,
                           VkStencilFaceFlags faceMask,
                           uint32_t reference)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                       uint32_t commandBufferCount,
                       const VkCommandBuffer *pCmdBuffers)
{
   pan_finishme("unimplemented!");
}

VkResult
pan_CreateCommandPool(VkDevice _device,
                      const VkCommandPoolCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkCommandPool *pCmdPool)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   struct pan_cmd_pool *pool;

   pool = vk_object_alloc(&device->vk, pAllocator, sizeof(*pool),
                          VK_OBJECT_TYPE_COMMAND_POOL);
   if (pool == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   pool->alloc = pAllocator ? (*pAllocator) : device->vk.alloc;

   list_inithead(&pool->cmd_buffers);
   list_inithead(&pool->free_cmd_buffers);

   /* TODO: what kind of up front alloc do we want */

   pool->queue_family_index = pCreateInfo->queueFamilyIndex;

   *pCmdPool = pan_cmd_pool_to_handle(pool);

   return VK_SUCCESS;
}

void
pan_DestroyCommandPool(VkDevice _device,
                       VkCommandPool commandPool,
                       const VkAllocationCallbacks *pAllocator)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   PAN_FROM_HANDLE(pan_cmd_pool, pool, commandPool);

   if (!pool)
      return;

   /* TODO: free memory */

   vk_object_free(&device->vk, pAllocator, pool);
}

VkResult
pan_ResetCommandPool(VkDevice device,
                     VkCommandPool commandPool,
                     VkCommandPoolResetFlags flags)
{
   pan_finishme("unimplemented!");
   return VK_SUCCESS;
}

void
pan_TrimCommandPool(VkDevice device,
                    VkCommandPool commandPool,
                    VkCommandPoolTrimFlags flags)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                       const VkRenderPassBeginInfo *pRenderPassBegin,
                       VkSubpassContents contents)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer,
                           const VkRenderPassBeginInfo *pRenderPassBeginInfo,
                           const VkSubpassBeginInfoKHR *pSubpassBeginInfo)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdNextSubpass2KHR(VkCommandBuffer commandBuffer,
                       const VkSubpassBeginInfoKHR *pSubpassBeginInfo,
                       const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdDraw(VkCommandBuffer commandBuffer,
            uint32_t vertexCount,
            uint32_t instanceCount,
            uint32_t firstVertex,
            uint32_t firstInstance)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                   uint32_t indexCount,
                   uint32_t instanceCount,
                   uint32_t firstIndex,
                   int32_t vertexOffset,
                   uint32_t firstInstance)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                    VkBuffer _buffer,
                    VkDeviceSize offset,
                    uint32_t drawCount,
                    uint32_t stride)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                           VkBuffer _buffer,
                           VkDeviceSize offset,
                           uint32_t drawCount,
                           uint32_t stride)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdDispatchBase(VkCommandBuffer commandBuffer,
                    uint32_t base_x,
                    uint32_t base_y,
                    uint32_t base_z,
                    uint32_t x,
                    uint32_t y,
                    uint32_t z)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdDispatch(VkCommandBuffer commandBuffer,
                uint32_t x,
                uint32_t y,
                uint32_t z)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdDispatchIndirect(VkCommandBuffer commandBuffer,
                        VkBuffer _buffer,
                        VkDeviceSize offset)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdEndRenderPass2KHR(VkCommandBuffer commandBuffer,
                         const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdPipelineBarrier(VkCommandBuffer commandBuffer,
                       VkPipelineStageFlags srcStageMask,
                       VkPipelineStageFlags destStageMask,
                       VkBool32 byRegion,
                       uint32_t memoryBarrierCount,
                       const VkMemoryBarrier *pMemoryBarriers,
                       uint32_t bufferMemoryBarrierCount,
                       const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                       uint32_t imageMemoryBarrierCount,
                       const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetEvent(VkCommandBuffer commandBuffer,
                VkEvent _event,
                VkPipelineStageFlags stageMask)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdResetEvent(VkCommandBuffer commandBuffer,
                  VkEvent _event,
                  VkPipelineStageFlags stageMask)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdWaitEvents(VkCommandBuffer commandBuffer,
                  uint32_t eventCount,
                  const VkEvent *pEvents,
                  VkPipelineStageFlags srcStageMask,
                  VkPipelineStageFlags dstStageMask,
                  uint32_t memoryBarrierCount,
                  const VkMemoryBarrier *pMemoryBarriers,
                  uint32_t bufferMemoryBarrierCount,
                  const VkBufferMemoryBarrier *pBufferMemoryBarriers,
                  uint32_t imageMemoryBarrierCount,
                  const VkImageMemoryBarrier *pImageMemoryBarriers)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask)
{
   pan_finishme("unimplemented!");
}
