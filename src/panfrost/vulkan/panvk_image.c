/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_image.c which is:
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

#include "panvk_private.h"
#include "panfrost-quirks.h"

#include "util/debug.h"
#include "util/u_atomic.h"
#include "vk_format.h"
#include "vk_object.h"
#include "vk_util.h"
#include "drm-uapi/drm_fourcc.h"

unsigned
panvk_image_get_plane_size(const struct panvk_image *image, unsigned plane)
{
   assert(!plane);
   return image->pimage.layout.data_size;
}

unsigned
panvk_image_get_total_size(const struct panvk_image *image)
{
   assert(util_format_get_num_planes(image->pimage.layout.format) == 1);
   return image->pimage.layout.data_size;
}

static enum mali_texture_dimension
panvk_image_type_to_mali_tex_dim(VkImageType type)
{
        switch (type) {
        case VK_IMAGE_TYPE_1D: return MALI_TEXTURE_DIMENSION_1D;
        case VK_IMAGE_TYPE_2D: return MALI_TEXTURE_DIMENSION_2D;
        case VK_IMAGE_TYPE_3D: return MALI_TEXTURE_DIMENSION_3D;
        default: unreachable("Invalid image type");
        }
}

static VkResult
panvk_image_create(VkDevice _device,
                   const VkImageCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *alloc,
                   VkImage *pImage,
                   uint64_t modifier,
                   const VkSubresourceLayout *plane_layouts)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   const struct panfrost_device *pdev = &device->physical_device->pdev;
   struct panvk_image *image = NULL;
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

   assert(pCreateInfo->mipLevels > 0);
   assert(pCreateInfo->arrayLayers > 0);
   assert(pCreateInfo->samples > 0);
   assert(pCreateInfo->extent.width > 0);
   assert(pCreateInfo->extent.height > 0);
   assert(pCreateInfo->extent.depth > 0);

   image = vk_object_zalloc(&device->vk, alloc, sizeof(*image),
                            VK_OBJECT_TYPE_IMAGE);
   if (!image)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   image->type = pCreateInfo->imageType;

   image->vk_format = pCreateInfo->format;
   image->tiling = pCreateInfo->tiling;
   image->usage = pCreateInfo->usage;
   image->flags = pCreateInfo->flags;
   image->extent = pCreateInfo->extent;
   pan_image_layout_init(pdev, &image->pimage.layout, modifier,
                         vk_format_to_pipe_format(pCreateInfo->format),
                         panvk_image_type_to_mali_tex_dim(pCreateInfo->imageType),
                         pCreateInfo->extent.width, pCreateInfo->extent.height,
                         pCreateInfo->extent.depth, pCreateInfo->arrayLayers,
                         pCreateInfo->samples, pCreateInfo->mipLevels,
                         PAN_IMAGE_CRC_NONE, NULL);

   image->exclusive = pCreateInfo->sharingMode == VK_SHARING_MODE_EXCLUSIVE;
   if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT) {
      for (uint32_t i = 0; i < pCreateInfo->queueFamilyIndexCount; ++i) {
         if (pCreateInfo->pQueueFamilyIndices[i] == VK_QUEUE_FAMILY_EXTERNAL)
            image->queue_family_mask |= (1u << PANVK_MAX_QUEUE_FAMILIES) - 1u;
         else
            image->queue_family_mask |= 1u << pCreateInfo->pQueueFamilyIndices[i];
       }
   }

   if (vk_find_struct_const(pCreateInfo->pNext, EXTERNAL_MEMORY_IMAGE_CREATE_INFO))
      image->shareable = true;

   *pImage = panvk_image_to_handle(image);
   return VK_SUCCESS;
}

VkResult
panvk_CreateImage(VkDevice device,
                  const VkImageCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkImage *pImage)
{
   uint64_t modifier = DRM_FORMAT_MOD_INVALID;
   const VkSubresourceLayout *plane_layouts = NULL;

   if (pCreateInfo->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
      const VkImageDrmFormatModifierListCreateInfoEXT *mod_info =
         vk_find_struct_const(pCreateInfo->pNext,
                              IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT);
      const VkImageDrmFormatModifierExplicitCreateInfoEXT *drm_explicit_info =
         vk_find_struct_const(pCreateInfo->pNext,
                              IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);

      assert(mod_info || drm_explicit_info);

      if (mod_info) {
         modifier = DRM_FORMAT_MOD_LINEAR;
         for (unsigned i = 0; i < mod_info->drmFormatModifierCount; i++) {
            if ((mod_info->pDrmFormatModifiers[i] & DRM_FORMAT_MOD_ARM_AFBC(0)) == DRM_FORMAT_MOD_ARM_AFBC(0)) {
               modifier = mod_info->pDrmFormatModifiers[i];
               break;
            }
         }
      } else {
         modifier = drm_explicit_info->drmFormatModifier;
         assert(modifier == DRM_FORMAT_MOD_LINEAR ||
                (modifier & DRM_FORMAT_MOD_ARM_AFBC(0)) == DRM_FORMAT_MOD_ARM_AFBC(0));
         plane_layouts = drm_explicit_info->pPlaneLayouts;
      }
   } else {
      const struct wsi_image_create_info *wsi_info =
         vk_find_struct_const(pCreateInfo->pNext, WSI_IMAGE_CREATE_INFO_MESA);
      if (wsi_info && wsi_info->scanout)
         modifier = DRM_FORMAT_MOD_LINEAR;
   }

   /* TODO: mod selection */
   if (modifier == DRM_FORMAT_MOD_INVALID)
      modifier = DRM_FORMAT_MOD_LINEAR;

   return panvk_image_create(device, pCreateInfo, pAllocator, pImage, modifier, plane_layouts);
}

void
panvk_DestroyImage(VkDevice _device,
                   VkImage _image,
                   const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_image, image, _image);

   if (!image)
      return;

   vk_object_free(&device->vk, pAllocator, image);
}

static unsigned
panvk_plane_index(VkFormat format, VkImageAspectFlags aspect_mask)
{
   switch (aspect_mask) {
   default:
      return 0;
   case VK_IMAGE_ASPECT_PLANE_1_BIT:
      return 1;
   case VK_IMAGE_ASPECT_PLANE_2_BIT:
      return 2;
   case VK_IMAGE_ASPECT_STENCIL_BIT:
      return format == VK_FORMAT_D32_SFLOAT_S8_UINT;
   }
}

void
panvk_GetImageSubresourceLayout(VkDevice _device,
                                VkImage _image,
                                const VkImageSubresource *pSubresource,
                                VkSubresourceLayout *pLayout)
{
   VK_FROM_HANDLE(panvk_image, image, _image);

   unsigned plane = panvk_plane_index(image->vk_format, pSubresource->aspectMask);
   assert(plane < PANVK_MAX_PLANES);

   const struct pan_image_slice_layout *slice_layout =
      &image->pimage.layout.slices[pSubresource->mipLevel];

   pLayout->offset = slice_layout->offset +
                     (pSubresource->arrayLayer *
                      image->pimage.layout.array_stride);
   pLayout->size = slice_layout->size;
   pLayout->rowPitch = slice_layout->line_stride;
   pLayout->arrayPitch = image->pimage.layout.array_stride;
   pLayout->depthPitch = slice_layout->surface_stride;
}

static enum mali_texture_dimension
panvk_view_type_to_mali_tex_dim(VkImageViewType type)
{
   switch (type) {
   case VK_IMAGE_VIEW_TYPE_1D:
   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
      return MALI_TEXTURE_DIMENSION_1D;
   case VK_IMAGE_VIEW_TYPE_2D:
   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      return MALI_TEXTURE_DIMENSION_2D;
   case VK_IMAGE_VIEW_TYPE_3D:
      return MALI_TEXTURE_DIMENSION_3D;
   case VK_IMAGE_VIEW_TYPE_CUBE:
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      return MALI_TEXTURE_DIMENSION_CUBE;
   default:
      unreachable("Invalid view type");
   }
}

static void
panvk_convert_swizzle(const VkComponentMapping *in,
                      unsigned char *out)
{
   const VkComponentSwizzle *comp = &in->r;
   for (unsigned i = 0; i < 4; i++) {
      switch (comp[i]) {
      case VK_COMPONENT_SWIZZLE_IDENTITY:
         out[i] = PIPE_SWIZZLE_X + i;
         break;
      case VK_COMPONENT_SWIZZLE_ZERO:
         out[i] = PIPE_SWIZZLE_0;
         break;
      case VK_COMPONENT_SWIZZLE_ONE:
         out[i] = PIPE_SWIZZLE_1;
         break;
      case VK_COMPONENT_SWIZZLE_R:
         out[i] = PIPE_SWIZZLE_X;
         break;
      case VK_COMPONENT_SWIZZLE_G:
         out[i] = PIPE_SWIZZLE_Y;
         break;
      case VK_COMPONENT_SWIZZLE_B:
         out[i] = PIPE_SWIZZLE_Z;
         break;
      case VK_COMPONENT_SWIZZLE_A:
         out[i] = PIPE_SWIZZLE_W;
         break;
      default:
         unreachable("Invalid swizzle");
      }
   }
}



VkResult
panvk_CreateImageView(VkDevice _device,
                      const VkImageViewCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkImageView *pView)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_image, image, pCreateInfo->image);
   struct panvk_image_view *view;

   view = vk_object_zalloc(&device->vk, pAllocator, sizeof(*view),
                          VK_OBJECT_TYPE_IMAGE_VIEW);
   if (view == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   view->pview.format = vk_format_to_pipe_format(pCreateInfo->format);

   if (pCreateInfo->subresourceRange.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT)
      view->pview.format = util_format_get_depth_only(view->pview.format);
   else if (pCreateInfo->subresourceRange.aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT)
      view->pview.format = util_format_stencil_only(view->pview.format);

   view->pview.dim = panvk_view_type_to_mali_tex_dim(pCreateInfo->viewType);
   view->pview.first_level = pCreateInfo->subresourceRange.baseMipLevel;
   view->pview.last_level = pCreateInfo->subresourceRange.baseMipLevel +
                            pCreateInfo->subresourceRange.levelCount - 1;
   view->pview.first_layer = pCreateInfo->subresourceRange.baseArrayLayer;
   view->pview.last_layer = pCreateInfo->subresourceRange.baseArrayLayer +
                            pCreateInfo->subresourceRange.layerCount - 1;
   panvk_convert_swizzle(&pCreateInfo->components, view->pview.swizzle);
   view->pview.image = &image->pimage;
   view->vk_format = pCreateInfo->format;

   struct panfrost_device *pdev = &device->physical_device->pdev;
   unsigned bo_size =
      panfrost_estimate_texture_payload_size(pdev,
                                             view->pview.first_level,
                                             view->pview.last_level,
                                             view->pview.first_layer,
                                             view->pview.last_layer,
                                             image->pimage.layout.nr_samples,
                                             view->pview.dim,
                                             image->pimage.layout.modifier);

   unsigned surf_descs_offset = 0;
   if (!pan_is_bifrost(pdev)) {
      bo_size += MALI_MIDGARD_TEXTURE_LENGTH;
      surf_descs_offset = MALI_MIDGARD_TEXTURE_LENGTH;
   }

   view->bo = panfrost_bo_create(pdev, bo_size, 0);

   struct panfrost_ptr surf_descs = {
      .cpu = view->bo->ptr.cpu + surf_descs_offset,
      .gpu = view->bo->ptr.gpu + surf_descs_offset,
   };
   void *tex_desc = pan_is_bifrost(pdev) ?
                    &view->bifrost.tex_desc : view->bo->ptr.cpu;

   panfrost_new_texture(pdev, &view->pview, tex_desc, &surf_descs);

   *pView = panvk_image_view_to_handle(view);
   return VK_SUCCESS;
}

void
panvk_DestroyImageView(VkDevice _device,
                       VkImageView _view,
                       const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_image_view, view, _view);

   if (!view)
      return;

   vk_object_free(&device->vk, pAllocator, view);
}

VkResult
panvk_CreateBufferView(VkDevice _device,
                       const VkBufferViewCreateInfo *pCreateInfo,
                       const VkAllocationCallbacks *pAllocator,
                       VkBufferView *pView)
{
   panvk_stub();
   return VK_SUCCESS;
}

void
panvk_DestroyBufferView(VkDevice _device,
                        VkBufferView bufferView,
                        const VkAllocationCallbacks *pAllocator)
{
   panvk_stub();
}

VkResult
panvk_GetImageDrmFormatModifierPropertiesEXT(VkDevice device,
                                             VkImage _image,
                                             VkImageDrmFormatModifierPropertiesEXT *pProperties)
{
   VK_FROM_HANDLE(panvk_image, image, _image);

   assert(pProperties->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT);

   pProperties->drmFormatModifier = image->pimage.layout.modifier;
   return VK_SUCCESS;
}
