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
   return image->layout.planes[plane].size;
}

unsigned
panvk_image_get_total_size(const struct panvk_image *image)
{
   unsigned size = 0;

   for (unsigned i = 0; i < util_format_get_num_planes(image->format); i++)
      size += image->layout.planes[i].size;

   return size;
}

static void
panvk_image_slice_layout_init(const struct panvk_image *image,
                              struct panvk_image_layout *layout,
                              unsigned plane, unsigned slice,
                              unsigned *offset, bool align_on_tile,
                              bool align_on_cacheline)
{
   assert(plane < PANVK_MAX_PLANES);
   assert(slice < PANVK_MAX_MIP_LEVELS);

   struct panvk_slice_layout *slice_layout = &layout->planes[plane].slices[slice];
   enum pipe_format format = util_format_get_plane_format(image->format, plane);
   unsigned bytes_per_pixel = util_format_get_blocksize(format);

   /* MSAA is implemented as a 3D texture with z corresponding to the
    * sample #, horrifyingly enough.
    */
   bool msaa = image->samples != VK_SAMPLE_COUNT_1_BIT;

   bool afbc = drm_is_afbc(image->modifier);
   bool tiled = image->modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED;
   bool linear = image->modifier == DRM_FORMAT_MOD_LINEAR;
   /* We don't know how to specify a 2D stride for 3D textures */
   bool can_align_stride = image->type != VK_IMAGE_TYPE_3D;

   align_on_tile = (align_on_tile || tiled || afbc) && can_align_stride;
   slice_layout->width = u_minify(image->extent.width, slice);
   slice_layout->height = u_minify(image->extent.height, slice);
   slice_layout->depth = u_minify(image->extent.depth, msaa ? 0 : slice);

   if (align_on_tile) {
      /* We don't need to align depth */
      slice_layout->width = ALIGN_POT(slice_layout->width, 16);
      slice_layout->height = ALIGN_POT(slice_layout->height, 16);
   }

   if (align_on_cacheline)
      *offset = ALIGN_POT(*offset, 64);

   slice_layout->offset = *offset;

   /* Compute the would-be stride */
   slice_layout->line_stride = bytes_per_pixel * slice_layout->width;
   if (util_format_is_compressed(image->format))
      slice_layout->line_stride /= 4;

   /* ..but cache-line align it for performance */
   if (can_align_stride && linear && align_on_cacheline)
      slice_layout->line_stride = ALIGN_POT(slice_layout->line_stride, 64);

   slice_layout->size = slice_layout->line_stride * slice_layout->height *
                        slice_layout->depth;

   if (afbc) {
      slice_layout->afbc_header_size =
         panfrost_afbc_header_size(slice_layout->width, slice_layout->height);
      *offset += slice_layout->afbc_header_size;
   }

   *offset += slice_layout->size;

   /* Add a checksum region if necessary */
   if (image->checksummed) {
      assert(slice_layout->depth == 1);
      slice_layout->checksum.offset = *offset;
      slice_layout->checksum.stride = DIV_ROUND_UP(slice_layout->width, 16);
      slice_layout->checksum.size = slice_layout->height * slice_layout->checksum.stride;
      *offset += slice_layout->checksum.size;
   }
}

static void
panvk_image_plane_layout_init(const struct panvk_image *image,
                              struct panvk_image_layout *layout,
                              unsigned plane, unsigned *offset,
                              bool align_on_tile, bool align_on_cacheline)
{
   struct panvk_plane_layout *plane_layout = &layout->planes[plane];

   if (align_on_cacheline)
      *offset = ALIGN_POT(*offset, 64);

   plane_layout->offset = *offset;

   unsigned slice_offset = 0;
   for (unsigned s = 0; s < image->level_count; s++) {
      panvk_image_slice_layout_init(image, layout, plane, s, &slice_offset,
                                    align_on_tile, align_on_cacheline);
   }

   unsigned elem_size = slice_offset;
   if (align_on_cacheline)
      elem_size = ALIGN_POT(elem_size, 64);

   plane_layout->array_stride = elem_size;
   plane_layout->size = elem_size * image->layer_count;
}

static void
panvk_image_layout_init(const struct panvk_image *image,
                        struct panvk_image_layout *layout)
{
   unsigned offset = 0;

   for (unsigned p = 0; p < util_format_get_num_planes(image->format); p++) {
      if (image->flags & VK_IMAGE_CREATE_DISJOINT_BIT)
         offset = 0;

      panvk_image_plane_layout_init(image, layout, p, &offset, true, true);
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
   image->level_count = pCreateInfo->mipLevels;
   image->layer_count = pCreateInfo->arrayLayers;
   image->samples = pCreateInfo->samples;

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

   image->format = vk_format_to_pipe_format(pCreateInfo->format);
   panvk_image_layout_init(image, &image->layout);

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
   const struct panvk_plane_layout *plane_layout = &image->layout.planes[plane];
   const struct panvk_slice_layout *slice_layout =
      &plane_layout->slices[pSubresource->mipLevel];

   /* We don't support multiplanart formats yet. */
   assert(!plane);
   pLayout->offset = slice_layout->offset + plane_layout->offset +
                     (pSubresource->arrayLayer * plane_layout->array_stride);
   pLayout->size = slice_layout->size;
   pLayout->rowPitch = slice_layout->line_stride;
   pLayout->arrayPitch = plane_layout->array_stride;
   pLayout->depthPitch = slice_layout->line_stride * slice_layout->height;
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

static unsigned
panvk_convert_swizzle(const VkComponentMapping *swizzle)
{
   unsigned ret = 0;

   const VkComponentSwizzle *comp = &swizzle->r;
   for (unsigned i = 0; i < 4; i++) {
      switch (comp[i]) {
      case VK_COMPONENT_SWIZZLE_IDENTITY:
         ret |= i << (i * 3);
         break;
      case VK_COMPONENT_SWIZZLE_ZERO:
      case VK_COMPONENT_SWIZZLE_ONE:
         ret |= (comp[i] - VK_COMPONENT_SWIZZLE_ZERO) << (i * 3);
         break;
      case VK_COMPONENT_SWIZZLE_R:
      case VK_COMPONENT_SWIZZLE_G:
      case VK_COMPONENT_SWIZZLE_B:
      case VK_COMPONENT_SWIZZLE_A:
         ret |= (comp[i] - VK_COMPONENT_SWIZZLE_R) << (i * 3);
         break;
      default:
         unreachable("Invalid swizzle");
      }
   }

   return ret;
}

static void
panvk_emit_surface_desc(const struct panvk_image_view *view,
                        unsigned plane, unsigned layer, unsigned slice,
                        unsigned sample, mali_ptr *desc)
{
   const struct panvk_image *image = view->image;
   const struct panvk_plane_layout *plane_layout = &image->layout.planes[plane];
   const struct panvk_slice_layout *slice_layout = &plane_layout->slices[slice];
   mali_ptr base;

   if (image->flags & VK_IMAGE_CREATE_DISJOINT_BIT) {
      base = image->memory.planes[plane].bo->ptr.gpu +
             image->memory.planes[plane].offset;
   } else {
      base = image->memory.planes[0].bo->ptr.gpu +
             image->memory.planes[0].offset;
   }

   base += (layer * plane_layout->array_stride) + slice_layout->offset +
           (sample * slice_layout->line_stride * slice_layout->height);
   desc[0] = base;

   bool tiled = image->modifier == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED;
   bool linear = image->modifier == DRM_FORMAT_MOD_LINEAR;
   unsigned row_stride, surf_stride;

   if (linear) {
      row_stride = slice_layout->line_stride;
      surf_stride = slice_layout->depth ?
                    row_stride * slice_layout->height : 0;
   } else if (tiled) {
      row_stride = slice_layout->height <= 16 ?
                   0 : slice_layout->line_stride * 16;
      surf_stride = row_stride * slice_layout->height;
      surf_stride = slice_layout->depth ?
                    slice_layout->line_stride * slice_layout->height : 0;
   } else {
      unreachable("TODO: AFBC");
   }

   desc[1] = row_stride | ((mali_ptr)surf_stride << 32);
}

#define MALI_SWIZZLE_R001 \
        (MALI_CHANNEL_R << 0) | \
        (MALI_CHANNEL_0 << 3) | \
        (MALI_CHANNEL_0 << 6) | \
        (MALI_CHANNEL_1 << 9)

#define MALI_SWIZZLE_A001 \
        (MALI_CHANNEL_A << 0) | \
        (MALI_CHANNEL_0 << 3) | \
        (MALI_CHANNEL_0 << 6) | \
        (MALI_CHANNEL_1 << 9)

static void
panvk_emit_midgard_image_view(const struct panvk_device *dev,
                              const struct panvk_image_view *view)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   const struct panvk_image *image = view->image;
   unsigned plane = 0;
   const struct panvk_plane_layout *plane_layout = &image->layout.planes[plane];
   const struct panvk_slice_layout *slice_layout =
      &plane_layout->slices[view->range.baseMipLevel];

   const struct util_format_description *fdesc = util_format_description(view->format);

   pan_pack(view->bo->ptr.cpu, MIDGARD_TEXTURE, cfg) {
      cfg.width = slice_layout->width;
      cfg.height = slice_layout->height;
      cfg.depth = slice_layout->depth;
      cfg.array_size = view->range.layerCount;
      cfg.format = pdev->formats[fdesc->format].hw;
      cfg.dimension = panvk_view_type_to_mali_tex_dim(view->type);
      cfg.texel_ordering = panfrost_modifier_to_layout(view->image->modifier);
      cfg.manual_stride = true;
      cfg.levels = view->range.levelCount;
      cfg.swizzle = panvk_convert_swizzle(&view->components);
   };

   mali_ptr *surf_desc = view->bo->ptr.cpu + MALI_MIDGARD_TEXTURE_LENGTH;
   for (unsigned layer = view->range.baseArrayLayer;
        layer < view->range.baseArrayLayer + view->range.layerCount; layer++) {
      for (unsigned level = view->range.baseMipLevel;
           level < view->range.baseMipLevel + view->range.levelCount; level++) {
         for (unsigned sample = 0; sample < image->samples; sample++) {
            panvk_emit_surface_desc(view, plane, layer, level, sample,
                                    surf_desc);
            surf_desc += 2;
         }
      }
   }
}

static void
panvk_emit_bifrost_image_view(const struct panvk_device *dev,
                              const struct panvk_image_view *view)
{
   const struct panfrost_device *pdev = &dev->physical_device->pdev;
   const struct panvk_image *image = view->image;
   unsigned plane = 0;
   const struct panvk_plane_layout *plane_layout = &image->layout.planes[plane];
   const struct panvk_slice_layout *slice_layout =
      &plane_layout->slices[view->range.baseMipLevel];

   const struct util_format_description *fdesc = util_format_description(view->format);

   pan_pack(&view->bifrost.tex_desc, BIFROST_TEXTURE, cfg) {
      cfg.dimension = panvk_view_type_to_mali_tex_dim(view->type);
      cfg.format = pdev->formats[fdesc->format].hw;
      cfg.width = slice_layout->width;
      cfg.height = slice_layout->height;
      cfg.swizzle = panvk_convert_swizzle(&view->components);
      cfg.texel_ordering = panfrost_modifier_to_layout(image->modifier);
      cfg.levels = view->range.levelCount;
      cfg.surfaces = view->bo->ptr.gpu;

      /* Use the sampler descriptor for LOD clamping */
      cfg.minimum_lod = 0;
      cfg.maximum_lod = view->range.levelCount - 1;
   };

   mali_ptr *surf_desc = view->bo->ptr.cpu;
   for (unsigned layer = view->range.baseArrayLayer;
        layer < view->range.baseArrayLayer + view->range.layerCount; layer++) {
      for (unsigned level = view->range.baseMipLevel;
           level < view->range.baseMipLevel + view->range.levelCount; level++) {
         for (unsigned sample = 0; sample < image->samples; sample++) {
            panvk_emit_surface_desc(view, plane, layer, level, sample,
                                    surf_desc);
            surf_desc += 2;
         }
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

   view = vk_object_alloc(&device->vk, pAllocator, sizeof(*view),
                          VK_OBJECT_TYPE_IMAGE_VIEW);
   if (view == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   view->image = image;
   view->type = pCreateInfo->viewType;
   view->vk_format = pCreateInfo->format;
   view->format = vk_format_to_pipe_format(pCreateInfo->format);
   view->components = pCreateInfo->components;
   view->range = pCreateInfo->subresourceRange;

   struct panfrost_device *pdev = &device->physical_device->pdev;
   unsigned num_surf_desc = view->range.layerCount * view->range.levelCount *
                            view->image->samples;
   unsigned bo_size = num_surf_desc * sizeof(mali_ptr) * 2;

   if (!pan_is_bifrost(pdev))
      bo_size += MALI_MIDGARD_TEXTURE_LENGTH;

   view->bo = panfrost_bo_create(pdev, bo_size, 0);

   if (pan_is_bifrost(pdev))
      panvk_emit_bifrost_image_view(device, view);
   else
      panvk_emit_midgard_image_view(device, view);

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

   pProperties->drmFormatModifier = image->modifier;
   return VK_SUCCESS;
}
