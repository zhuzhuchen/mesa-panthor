/*
 * Copyright © 2016 Intel Corporation
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

void
pan_CmdCopyBuffer(VkCommandBuffer commandBuffer,
                  VkBuffer srcBuffer,
                  VkBuffer destBuffer,
                  uint32_t regionCount,
                  const VkBufferCopy *pRegions)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdCopyBufferToImage(VkCommandBuffer commandBuffer,
                         VkBuffer srcBuffer,
                         VkImage destImage,
                         VkImageLayout destImageLayout,
                         uint32_t regionCount,
                         const VkBufferImageCopy *pRegions)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdCopyImageToBuffer(VkCommandBuffer commandBuffer,
                         VkImage srcImage,
                         VkImageLayout srcImageLayout,
                         VkBuffer destBuffer,
                         uint32_t regionCount,
                         const VkBufferImageCopy *pRegions)
{
   pan_finishme("unimplemented!");
}

void
pan_CmdCopyImage(VkCommandBuffer commandBuffer,
                 VkImage srcImage,
                 VkImageLayout srcImageLayout,
                 VkImage destImage,
                 VkImageLayout destImageLayout,
                 uint32_t regionCount,
                 const VkImageCopy *pRegions)
{
   pan_finishme("unimplemented!");
}
