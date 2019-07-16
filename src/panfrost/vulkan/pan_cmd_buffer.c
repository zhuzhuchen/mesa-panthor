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

VkResult
pan_AllocateCommandBuffers(VkDevice _device,
                           const VkCommandBufferAllocateInfo *pAllocateInfo,
                           VkCommandBuffer *pCommandBuffers)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_FreeCommandBuffers(VkDevice device,
                       VkCommandPool commandPool,
                       uint32_t commandBufferCount,
                       const VkCommandBuffer *pCommandBuffers)
{
   assert(0);
}

VkResult
pan_ResetCommandBuffer(VkCommandBuffer commandBuffer,
                       VkCommandBufferResetFlags flags)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_BeginCommandBuffer(VkCommandBuffer commandBuffer,
                       const VkCommandBufferBeginInfo *pBeginInfo)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_CmdBindVertexBuffers(VkCommandBuffer commandBuffer,
                         uint32_t firstBinding,
                         uint32_t bindingCount,
                         const VkBuffer *pBuffers,
                         const VkDeviceSize *pOffsets)
{
   assert(0);
}

void
pan_CmdBindIndexBuffer(VkCommandBuffer commandBuffer,
                       VkBuffer buffer,
                       VkDeviceSize offset,
                       VkIndexType indexType)
{
   assert(0);
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
   assert(0);
}

void
pan_CmdPushConstants(VkCommandBuffer commandBuffer,
                     VkPipelineLayout layout,
                     VkShaderStageFlags stageFlags,
                     uint32_t offset,
                     uint32_t size,
                     const void *pValues)
{
   assert(0);
}

VkResult
pan_EndCommandBuffer(VkCommandBuffer commandBuffer)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_CmdBindPipeline(VkCommandBuffer commandBuffer,
                    VkPipelineBindPoint pipelineBindPoint,
                    VkPipeline _pipeline)
{
   assert(0);
}

void
pan_CmdSetViewport(VkCommandBuffer commandBuffer,
                   uint32_t firstViewport,
                   uint32_t viewportCount,
                   const VkViewport *pViewports)
{
   assert(0);
}

void
pan_CmdSetScissor(VkCommandBuffer commandBuffer,
                  uint32_t firstScissor,
                  uint32_t scissorCount,
                  const VkRect2D *pScissors)
{
   assert(0);
}

void
pan_CmdSetLineWidth(VkCommandBuffer commandBuffer, float lineWidth)
{
   assert(0);
}

void
pan_CmdSetDepthBias(VkCommandBuffer commandBuffer,
                    float depthBiasConstantFactor,
                    float depthBiasClamp,
                    float depthBiasSlopeFactor)
{
   assert(0);
}

void
pan_CmdSetBlendConstants(VkCommandBuffer commandBuffer,
                         const float blendConstants[4])
{
   assert(0);
}

void
pan_CmdSetDepthBounds(VkCommandBuffer commandBuffer,
                      float minDepthBounds,
                      float maxDepthBounds)
{
   assert(0);
}

void
pan_CmdSetStencilCompareMask(VkCommandBuffer commandBuffer,
                             VkStencilFaceFlags faceMask,
                             uint32_t compareMask)
{
   assert(0);
}

void
pan_CmdSetStencilWriteMask(VkCommandBuffer commandBuffer,
                           VkStencilFaceFlags faceMask,
                           uint32_t writeMask)
{
   assert(0);
}

void
pan_CmdSetStencilReference(VkCommandBuffer commandBuffer,
                           VkStencilFaceFlags faceMask,
                           uint32_t reference)
{
   assert(0);
}

void
pan_CmdExecuteCommands(VkCommandBuffer commandBuffer,
                       uint32_t commandBufferCount,
                       const VkCommandBuffer *pCmdBuffers)
{
   assert(0);
}

VkResult
pan_CreateCommandPool(VkDevice _device,
                      const VkCommandPoolCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkCommandPool *pCmdPool)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_DestroyCommandPool(VkDevice _device,
                       VkCommandPool commandPool,
                       const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

VkResult
pan_ResetCommandPool(VkDevice device,
                     VkCommandPool commandPool,
                     VkCommandPoolResetFlags flags)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_TrimCommandPool(VkDevice device,
                    VkCommandPool commandPool,
                    VkCommandPoolTrimFlags flags)
{
   assert(0);
}

void
pan_CmdBeginRenderPass(VkCommandBuffer commandBuffer,
                       const VkRenderPassBeginInfo *pRenderPassBegin,
                       VkSubpassContents contents)
{
   assert(0);
}

void
pan_CmdBeginRenderPass2KHR(VkCommandBuffer commandBuffer,
                           const VkRenderPassBeginInfo *pRenderPassBeginInfo,
                           const VkSubpassBeginInfoKHR *pSubpassBeginInfo)
{
   assert(0);
}

void
pan_CmdNextSubpass(VkCommandBuffer commandBuffer, VkSubpassContents contents)
{
   assert(0);
}

void
pan_CmdNextSubpass2KHR(VkCommandBuffer commandBuffer,
                       const VkSubpassBeginInfoKHR *pSubpassBeginInfo,
                       const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   assert(0);
}

void
pan_CmdDraw(VkCommandBuffer commandBuffer,
            uint32_t vertexCount,
            uint32_t instanceCount,
            uint32_t firstVertex,
            uint32_t firstInstance)
{
   assert(0);
}

void
pan_CmdDrawIndexed(VkCommandBuffer commandBuffer,
                   uint32_t indexCount,
                   uint32_t instanceCount,
                   uint32_t firstIndex,
                   int32_t vertexOffset,
                   uint32_t firstInstance)
{
   assert(0);
}

void
pan_CmdDrawIndirect(VkCommandBuffer commandBuffer,
                    VkBuffer _buffer,
                    VkDeviceSize offset,
                    uint32_t drawCount,
                    uint32_t stride)
{
   assert(0);
}

void
pan_CmdDrawIndexedIndirect(VkCommandBuffer commandBuffer,
                           VkBuffer _buffer,
                           VkDeviceSize offset,
                           uint32_t drawCount,
                           uint32_t stride)
{
   assert(0);
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
   assert(0);
}

void
pan_CmdDispatch(VkCommandBuffer commandBuffer,
                uint32_t x,
                uint32_t y,
                uint32_t z)
{
   assert(0);
}

void
pan_CmdDispatchIndirect(VkCommandBuffer commandBuffer,
                        VkBuffer _buffer,
                        VkDeviceSize offset)
{
   assert(0);
}

void
pan_CmdEndRenderPass(VkCommandBuffer commandBuffer)
{
   assert(0);
}

void
pan_CmdEndRenderPass2KHR(VkCommandBuffer commandBuffer,
                         const VkSubpassEndInfoKHR *pSubpassEndInfo)
{
   assert(0);
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
   assert(0);
}

void
pan_CmdSetEvent(VkCommandBuffer commandBuffer,
                VkEvent _event,
                VkPipelineStageFlags stageMask)
{
   assert(0);
}

void
pan_CmdResetEvent(VkCommandBuffer commandBuffer,
                  VkEvent _event,
                  VkPipelineStageFlags stageMask)
{
   assert(0);
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
   assert(0);
}

void
pan_CmdSetDeviceMask(VkCommandBuffer commandBuffer, uint32_t deviceMask)
{
   assert(0);
}
