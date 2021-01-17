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

#include <fcntl.h>
#include <libsync.h>
#include <unistd.h>

#include "util/os_time.h"

VkResult
pan_CreateFence(VkDevice _device,
                const VkFenceCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkFence *pFence)
{
   pan_finishme("unimplemented!");
   return VK_SUCCESS;
}

void
pan_DestroyFence(VkDevice _device,
                 VkFence _fence,
                 const VkAllocationCallbacks *pAllocator)
{
   pan_finishme("unimplemented!");
}

VkResult
pan_WaitForFences(VkDevice _device,
                  uint32_t fenceCount,
                  const VkFence *pFences,
                  VkBool32 waitAll,
                  uint64_t timeout)
{
   pan_finishme("unimplemented!");
   return VK_SUCCESS;
}

VkResult
pan_ResetFences(VkDevice _device, uint32_t fenceCount, const VkFence *pFences)
{
   pan_finishme("unimplemented!");
   return VK_SUCCESS;
}

VkResult
pan_GetFenceStatus(VkDevice _device, VkFence _fence)
{
   pan_finishme("unimplemented!");
   return VK_SUCCESS;
}
