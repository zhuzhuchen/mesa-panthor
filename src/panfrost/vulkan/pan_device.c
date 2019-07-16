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

#include <fcntl.h>
#include <libsync.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <xf86drm.h>

#include "drm-uapi/panfrost_drm.h"

#include "compiler/glsl_types.h"
#include "util/debug.h"
#include "util/disk_cache.h"
#include "util/strtod.h"
#include "vk_format.h"
#include "vk_util.h"

static int
pan_device_get_cache_uuid(uint16_t family, void *uuid)
{
   uint32_t mesa_timestamp;
   uint16_t f = family;
   memset(uuid, 0, VK_UUID_SIZE);
   if (!disk_cache_get_function_timestamp(pan_device_get_cache_uuid,
                                          &mesa_timestamp))
      return -1;

   memcpy(uuid, &mesa_timestamp, 4);
   memcpy((char *) uuid + 4, &f, 2);
   snprintf((char *) uuid + 6, VK_UUID_SIZE - 10, "pan");
   return 0;
}

static void
pan_get_driver_uuid(void *uuid)
{
   memset(uuid, 0, VK_UUID_SIZE);
   snprintf(uuid, VK_UUID_SIZE, "freedreno");
}

static void
pan_get_device_uuid(void *uuid)
{
   memset(uuid, 0, VK_UUID_SIZE);
}

static void *
default_alloc_func(void *pUserData,
                   size_t size,
                   size_t align,
                   VkSystemAllocationScope allocationScope)
{
   return malloc(size);
}

static void *
default_realloc_func(void *pUserData,
                     void *pOriginal,
                     size_t size,
                     size_t align,
                     VkSystemAllocationScope allocationScope)
{
   return realloc(pOriginal, size);
}

static void
default_free_func(void *pUserData, void *pMemory)
{
   free(pMemory);
}

static const VkAllocationCallbacks default_alloc = {
   .pUserData = NULL,
   .pfnAllocation = default_alloc_func,
   .pfnReallocation = default_realloc_func,
   .pfnFree = default_free_func,
};

static const struct debug_control pan_debug_options[] = {
   { "startup", PAN_DEBUG_STARTUP },
   { "nir", PAN_DEBUG_NIR },
   { NULL, 0 }
};

static int
pan_get_instance_extension_index(const char *name)
{
   for (unsigned i = 0; i < PAN_INSTANCE_EXTENSION_COUNT; ++i) {
      if (strcmp(name, pan_instance_extensions[i].extensionName) == 0)
         return i;
   }
   return -1;
}

VkResult
pan_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkInstance *pInstance)
{
   struct pan_instance *instance;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   uint32_t client_version;
   if (pCreateInfo->pApplicationInfo &&
       pCreateInfo->pApplicationInfo->apiVersion != 0) {
      client_version = pCreateInfo->pApplicationInfo->apiVersion;
   } else {
      pan_EnumerateInstanceVersion(&client_version);
   }

   instance = vk_zalloc2(&default_alloc, pAllocator, sizeof(*instance), 8,
                         VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   instance->_loader_data.loaderMagic = ICD_LOADER_MAGIC;

   if (pAllocator)
      instance->alloc = *pAllocator;
   else
      instance->alloc = default_alloc;

   instance->api_version = client_version;
   instance->physical_device_count = -1;

   instance->debug_flags = parse_debug_string(getenv("PAN_DEBUG"),
                                              pan_debug_options);

   if (instance->debug_flags & PAN_DEBUG_STARTUP)
      pan_logi("Created an instance");

   for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      const char *ext_name = pCreateInfo->ppEnabledExtensionNames[i];
      int index = pan_get_instance_extension_index(ext_name);

      if (index < 0 || !pan_supported_instance_extensions.extensions[index]) {
         vk_free2(&default_alloc, pAllocator, instance);
         return vk_error(instance, VK_ERROR_EXTENSION_NOT_PRESENT);
      }

      instance->enabled_extensions.extensions[index] = true;
   }

   result = vk_debug_report_instance_init(&instance->debug_report_callbacks);
   if (result != VK_SUCCESS) {
      vk_free2(&default_alloc, pAllocator, instance);
      return vk_error(instance, result);
   }

   _mesa_locale_init();

   glsl_type_singleton_init_or_ref();

   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

   *pInstance = pan_instance_to_handle(instance);

   return VK_SUCCESS;
}

void
pan_DestroyInstance(VkInstance _instance,
                    const VkAllocationCallbacks *pAllocator)
{
   glsl_type_singleton_decref();
   assert(0);
}

static unsigned
panfrost_major_version(unsigned gpu_id)
{
        switch (gpu_id) {
        case 0x600:
        case 0x620:
        case 0x720:
                return 4;
        case 0x750:
        case 0x820:
        case 0x830:
        case 0x860:
        case 0x880:
                return 5;
        default:
                return gpu_id >> 12;
        }
}

static VkResult
pan_physical_device_init(struct pan_physical_device *device,
                         struct pan_instance *instance,
                         drmDevicePtr drm_device)
{
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   VkResult result = VK_SUCCESS;
   drmVersionPtr version;
   int fd;
   int master_fd = -1;

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "failed to open device %s", path);
   }

   version = drmGetVersion(fd);
   if (!version) {
      close(fd);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "failed to query kernel driver version for device %s",
                       path);
   }

   if (strcmp(version->name, "panfrost")) {
      drmFreeVersion(version);
      close(fd);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "device %s does not use the panfrost kernel driver", path);
   }

   drmFreeVersion(version);

   if (instance->debug_flags & PAN_DEBUG_STARTUP)
      pan_logi("Found compatible device '%s'.", path);

   device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   device->instance = instance;
   assert(strlen(path) < ARRAY_SIZE(device->path));
   strncpy(device->path, path, ARRAY_SIZE(device->path));

   if (instance->enabled_extensions.KHR_display) {
      master_fd = open(drm_device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
      if (master_fd >= 0) {
         /* TODO: free master_fd is accel is not working? */
      }
   }

   device->master_fd = master_fd;
   device->local_fd = fd;

   if (pan_drm_get_gpu_id(device, &device->gpu_id)) {
      if (instance->debug_flags & PAN_DEBUG_STARTUP)
         pan_logi("Could not query the GPU ID");
      result = vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                         "could not get GPU ID");
      goto fail;
   }

   device->arch = panfrost_major_version(device->gpu_id);

   memset(device->name, 0, sizeof(device->name));
   sprintf(device->name, "T%x", device->gpu_id);

   if (pan_device_get_cache_uuid(device->gpu_id, device->cache_uuid)) {
      result = vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                         "cannot generate UUID");
      goto fail;
   }

   char buf[VK_UUID_SIZE * 2 + 1];
   disk_cache_format_hex_id(buf, device->cache_uuid, VK_UUID_SIZE * 2);
   device->disk_cache = disk_cache_create(device->name, buf, 0);

   fprintf(stderr, "WARNING: panvk is not a conformant vulkan implementation, "
                   "testing use only.\n");

   pan_get_driver_uuid(&device->device_uuid);
   pan_get_device_uuid(&device->device_uuid);

   pan_fill_device_extension_table(device, &device->supported_extensions);

   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail;
   }

   result = pan_wsi_init(device);
   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail;
   }

   return VK_SUCCESS;

fail:
   close(fd);
   if (master_fd != -1)
      close(master_fd);
   return result;
}

static VkResult
pan_enumerate_devices(struct pan_instance *instance)
{
   /* TODO: Check for more devices ? */
   drmDevicePtr devices[8];
   VkResult result = VK_ERROR_INCOMPATIBLE_DRIVER;
   int max_devices;

   instance->physical_device_count = 0;

   max_devices = drmGetDevices2(0, devices, ARRAY_SIZE(devices));

   if (instance->debug_flags & PAN_DEBUG_STARTUP)
      pan_logi("Found %d drm nodes", max_devices);

   if (max_devices < 1)
      return vk_error(instance, VK_ERROR_INCOMPATIBLE_DRIVER);

   for (unsigned i = 0; i < (unsigned) max_devices; i++) {
      if (devices[i]->available_nodes & 1 << DRM_NODE_RENDER &&
          devices[i]->bustype == DRM_BUS_PLATFORM) {

         result = pan_physical_device_init(instance->physical_devices +
                                           instance->physical_device_count,
                                           instance, devices[i]);
         if (result == VK_SUCCESS)
            ++instance->physical_device_count;
         else if (result != VK_ERROR_INCOMPATIBLE_DRIVER)
            break;
      }
   }
   drmFreeDevices(devices, max_devices);

   return result;
}

VkResult
pan_EnumeratePhysicalDevices(VkInstance _instance,
                             uint32_t *pPhysicalDeviceCount,
                             VkPhysicalDevice *pPhysicalDevices)
{
   PAN_FROM_HANDLE(pan_instance, instance, _instance);
   VK_OUTARRAY_MAKE(out, pPhysicalDevices, pPhysicalDeviceCount);

   VkResult result;

   if (instance->physical_device_count < 0) {
      result = pan_enumerate_devices(instance);
      if (result != VK_SUCCESS && result != VK_ERROR_INCOMPATIBLE_DRIVER)
         return result;
   }

   for (uint32_t i = 0; i < instance->physical_device_count; ++i) {
      vk_outarray_append(&out, p)
      {
         *p = pan_physical_device_to_handle(instance->physical_devices + i);
      }
   }

   return vk_outarray_status(&out);
}

VkResult
pan_EnumeratePhysicalDeviceGroups(VkInstance _instance,
                                  uint32_t *pPhysicalDeviceGroupCount,
                                  VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_GetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                              VkPhysicalDeviceFeatures *pFeatures)
{
   memset(pFeatures, 0, sizeof(*pFeatures));

   *pFeatures = (VkPhysicalDeviceFeatures) {
      .fullDrawIndexUint32 = true,
      .independentBlend = true,
      .wideLines = true,
      .largePoints = true,
      .textureCompressionETC2 = true,
      .textureCompressionASTC_LDR = true,
      .shaderUniformBufferArrayDynamicIndexing = true,
      .shaderSampledImageArrayDynamicIndexing = true,
      .shaderStorageBufferArrayDynamicIndexing = true,
      .shaderStorageImageArrayDynamicIndexing = true,
   };
}

void
pan_GetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice,
                               VkPhysicalDeviceFeatures2 *pFeatures)
{
   vk_foreach_struct(ext, pFeatures->pNext)
   {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
         VkPhysicalDeviceVulkan11Features *features = (void *) ext;
         features->storageBuffer16BitAccess            = false;
         features->uniformAndStorageBuffer16BitAccess  = false;
         features->storagePushConstant16               = false;
         features->storageInputOutput16                = false;
         features->multiview                           = false;
         features->multiviewGeometryShader             = false;
         features->multiviewTessellationShader         = false;
         features->variablePointersStorageBuffer       = true;
         features->variablePointers                    = true;
         features->protectedMemory                     = false;
         features->samplerYcbcrConversion              = false;
         features->shaderDrawParameters                = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
         VkPhysicalDeviceVulkan12Features *features = (void *) ext;
         features->samplerMirrorClampToEdge            = false;
         features->drawIndirectCount                   = false;
         features->storageBuffer8BitAccess             = false;
         features->uniformAndStorageBuffer8BitAccess   = false;
         features->storagePushConstant8                = false;
         features->shaderBufferInt64Atomics            = false;
         features->shaderSharedInt64Atomics            = false;
         features->shaderFloat16                       = false;
         features->shaderInt8                          = false;

         features->descriptorIndexing                                 = false;
         features->shaderInputAttachmentArrayDynamicIndexing          = false;
         features->shaderUniformTexelBufferArrayDynamicIndexing       = false;
         features->shaderStorageTexelBufferArrayDynamicIndexing       = false;
         features->shaderUniformBufferArrayNonUniformIndexing         = false;
         features->shaderSampledImageArrayNonUniformIndexing          = false;
         features->shaderStorageBufferArrayNonUniformIndexing         = false;
         features->shaderStorageImageArrayNonUniformIndexing          = false;
         features->shaderInputAttachmentArrayNonUniformIndexing       = false;
         features->shaderUniformTexelBufferArrayNonUniformIndexing    = false;
         features->shaderStorageTexelBufferArrayNonUniformIndexing    = false;
         features->descriptorBindingUniformBufferUpdateAfterBind      = false;
         features->descriptorBindingSampledImageUpdateAfterBind       = false;
         features->descriptorBindingStorageImageUpdateAfterBind       = false;
         features->descriptorBindingStorageBufferUpdateAfterBind      = false;
         features->descriptorBindingUniformTexelBufferUpdateAfterBind = false;
         features->descriptorBindingStorageTexelBufferUpdateAfterBind = false;
         features->descriptorBindingUpdateUnusedWhilePending          = false;
         features->descriptorBindingPartiallyBound                    = false;
         features->descriptorBindingVariableDescriptorCount           = false;
         features->runtimeDescriptorArray                             = false;

         features->samplerFilterMinmax                 = false;
         features->scalarBlockLayout                   = false;
         features->imagelessFramebuffer                = false;
         features->uniformBufferStandardLayout         = false;
         features->shaderSubgroupExtendedTypes         = false;
         features->separateDepthStencilLayouts         = false;
         features->hostQueryReset                      = false;
         features->timelineSemaphore                   = false;
         features->bufferDeviceAddress                 = false;
         features->bufferDeviceAddressCaptureReplay    = false;
         features->bufferDeviceAddressMultiDevice      = false;
         features->vulkanMemoryModel                   = false;
         features->vulkanMemoryModelDeviceScope        = false;
         features->vulkanMemoryModelAvailabilityVisibilityChains = false;
         features->shaderOutputViewportIndex           = false;
         features->shaderOutputLayer                   = false;
         features->subgroupBroadcastDynamicId          = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES: {
         VkPhysicalDeviceVariablePointersFeatures *features = (void *) ext;
         features->variablePointersStorageBuffer = true;
         features->variablePointers = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES: {
         VkPhysicalDeviceMultiviewFeatures *features =
            (VkPhysicalDeviceMultiviewFeatures *) ext;
         features->multiview = false;
         features->multiviewGeometryShader = false;
         features->multiviewTessellationShader = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES: {
         VkPhysicalDeviceShaderDrawParametersFeatures *features =
            (VkPhysicalDeviceShaderDrawParametersFeatures *) ext;
         features->shaderDrawParameters = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES: {
         VkPhysicalDeviceProtectedMemoryFeatures *features =
            (VkPhysicalDeviceProtectedMemoryFeatures *) ext;
         features->protectedMemory = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES: {
         VkPhysicalDevice16BitStorageFeatures *features =
            (VkPhysicalDevice16BitStorageFeatures *) ext;
         features->storageBuffer16BitAccess = false;
         features->uniformAndStorageBuffer16BitAccess = false;
         features->storagePushConstant16 = false;
         features->storageInputOutput16 = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES: {
         VkPhysicalDeviceSamplerYcbcrConversionFeatures *features =
            (VkPhysicalDeviceSamplerYcbcrConversionFeatures *) ext;
         features->samplerYcbcrConversion = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT: {
         VkPhysicalDeviceDescriptorIndexingFeaturesEXT *features =
            (VkPhysicalDeviceDescriptorIndexingFeaturesEXT *) ext;
         features->shaderInputAttachmentArrayDynamicIndexing = false;
         features->shaderUniformTexelBufferArrayDynamicIndexing = false;
         features->shaderStorageTexelBufferArrayDynamicIndexing = false;
         features->shaderUniformBufferArrayNonUniformIndexing = false;
         features->shaderSampledImageArrayNonUniformIndexing = false;
         features->shaderStorageBufferArrayNonUniformIndexing = false;
         features->shaderStorageImageArrayNonUniformIndexing = false;
         features->shaderInputAttachmentArrayNonUniformIndexing = false;
         features->shaderUniformTexelBufferArrayNonUniformIndexing = false;
         features->shaderStorageTexelBufferArrayNonUniformIndexing = false;
         features->descriptorBindingUniformBufferUpdateAfterBind = false;
         features->descriptorBindingSampledImageUpdateAfterBind = false;
         features->descriptorBindingStorageImageUpdateAfterBind = false;
         features->descriptorBindingStorageBufferUpdateAfterBind = false;
         features->descriptorBindingUniformTexelBufferUpdateAfterBind = false;
         features->descriptorBindingStorageTexelBufferUpdateAfterBind = false;
         features->descriptorBindingUpdateUnusedWhilePending = false;
         features->descriptorBindingPartiallyBound = false;
         features->descriptorBindingVariableDescriptorCount = false;
         features->runtimeDescriptorArray = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT: {
         VkPhysicalDeviceConditionalRenderingFeaturesEXT *features =
            (VkPhysicalDeviceConditionalRenderingFeaturesEXT *) ext;
         features->conditionalRendering = false;
         features->inheritedConditionalRendering = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT: {
         VkPhysicalDeviceTransformFeedbackFeaturesEXT *features =
            (VkPhysicalDeviceTransformFeedbackFeaturesEXT *) ext;
         features->transformFeedback = false;
         features->geometryStreams = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT: {
         VkPhysicalDeviceIndexTypeUint8FeaturesEXT *features =
            (VkPhysicalDeviceIndexTypeUint8FeaturesEXT *)ext;
         features->indexTypeUint8 = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *features =
            (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT *)ext;
         features->vertexAttributeInstanceRateDivisor = true;
         features->vertexAttributeInstanceRateZeroDivisor = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES_EXT: {
         VkPhysicalDevicePrivateDataFeaturesEXT *features =
            (VkPhysicalDevicePrivateDataFeaturesEXT *)ext;
         features->privateData = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT: {
         VkPhysicalDeviceDepthClipEnableFeaturesEXT *features =
            (VkPhysicalDeviceDepthClipEnableFeaturesEXT *)ext;
         features->depthClipEnable = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT: {
         VkPhysicalDevice4444FormatsFeaturesEXT *features = (void *)ext;
         features->formatA4R4G4B4 = true;
         features->formatA4B4G4R4 = true;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT: {
         VkPhysicalDeviceCustomBorderColorFeaturesEXT *features = (void *) ext;
         features->customBorderColors = true;
         features->customBorderColorWithoutFormat = true;
         break;
      }
      default:
         break;
      }
   }
   return pan_GetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);
}

void
pan_GetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice,
                                VkPhysicalDeviceProperties *pProperties)
{
   PAN_FROM_HANDLE(pan_physical_device, pdevice, physicalDevice);
   VkSampleCountFlags sample_counts = 0xf;

   /* make sure that the entire descriptor set is addressable with a signed
    * 32-bit int. So the sum of all limits scaled by descriptor size has to
    * be at most 2 GiB. the combined image & samples object count as one of
    * both. This limit is for the pipeline layout, not for the set layout, but
    * there is no set limit, so we just set a pipeline limit. I don't think
    * any app is going to hit this soon. */
   size_t max_descriptor_set_size =
      ((1ull << 31) - 16 * MAX_DYNAMIC_BUFFERS) /
      (32 /* uniform buffer, 32 due to potential space wasted on alignment */ +
       32 /* storage buffer, 32 due to potential space wasted on alignment */ +
       32 /* sampler, largest when combined with image */ +
       64 /* sampled image */ + 64 /* storage image */);

   VkPhysicalDeviceLimits limits = {
      .maxImageDimension1D = (1 << 14),
      .maxImageDimension2D = (1 << 14),
      .maxImageDimension3D = (1 << 11),
      .maxImageDimensionCube = (1 << 14),
      .maxImageArrayLayers = (1 << 11),
      .maxTexelBufferElements = 128 * 1024 * 1024,
      .maxUniformBufferRange = UINT32_MAX,
      .maxStorageBufferRange = UINT32_MAX,
      .maxPushConstantsSize = MAX_PUSH_CONSTANTS_SIZE,
      .maxMemoryAllocationCount = UINT32_MAX,
      .maxSamplerAllocationCount = 64 * 1024,
      .bufferImageGranularity = 64,          /* A cache line */
      .sparseAddressSpaceSize = 0xffffffffu, /* buffer max size */
      .maxBoundDescriptorSets = MAX_SETS,
      .maxPerStageDescriptorSamplers = max_descriptor_set_size,
      .maxPerStageDescriptorUniformBuffers = max_descriptor_set_size,
      .maxPerStageDescriptorStorageBuffers = max_descriptor_set_size,
      .maxPerStageDescriptorSampledImages = max_descriptor_set_size,
      .maxPerStageDescriptorStorageImages = max_descriptor_set_size,
      .maxPerStageDescriptorInputAttachments = max_descriptor_set_size,
      .maxPerStageResources = max_descriptor_set_size,
      .maxDescriptorSetSamplers = max_descriptor_set_size,
      .maxDescriptorSetUniformBuffers = max_descriptor_set_size,
      .maxDescriptorSetUniformBuffersDynamic = MAX_DYNAMIC_UNIFORM_BUFFERS,
      .maxDescriptorSetStorageBuffers = max_descriptor_set_size,
      .maxDescriptorSetStorageBuffersDynamic = MAX_DYNAMIC_STORAGE_BUFFERS,
      .maxDescriptorSetSampledImages = max_descriptor_set_size,
      .maxDescriptorSetStorageImages = max_descriptor_set_size,
      .maxDescriptorSetInputAttachments = max_descriptor_set_size,
      .maxVertexInputAttributes = 32,
      .maxVertexInputBindings = 32,
      .maxVertexInputAttributeOffset = 2047,
      .maxVertexInputBindingStride = 2048,
      .maxVertexOutputComponents = 128,
      .maxTessellationGenerationLevel = 64,
      .maxTessellationPatchSize = 32,
      .maxTessellationControlPerVertexInputComponents = 128,
      .maxTessellationControlPerVertexOutputComponents = 128,
      .maxTessellationControlPerPatchOutputComponents = 120,
      .maxTessellationControlTotalOutputComponents = 4096,
      .maxTessellationEvaluationInputComponents = 128,
      .maxTessellationEvaluationOutputComponents = 128,
      .maxGeometryShaderInvocations = 127,
      .maxGeometryInputComponents = 64,
      .maxGeometryOutputComponents = 128,
      .maxGeometryOutputVertices = 256,
      .maxGeometryTotalOutputComponents = 1024,
      .maxFragmentInputComponents = 128,
      .maxFragmentOutputAttachments = 8,
      .maxFragmentDualSrcAttachments = 1,
      .maxFragmentCombinedOutputResources = 8,
      .maxComputeSharedMemorySize = 32768,
      .maxComputeWorkGroupCount = { 65535, 65535, 65535 },
      .maxComputeWorkGroupInvocations = 2048,
      .maxComputeWorkGroupSize = { 2048, 2048, 2048 },
      .subPixelPrecisionBits = 4 /* FIXME */,
      .subTexelPrecisionBits = 4 /* FIXME */,
      .mipmapPrecisionBits = 4 /* FIXME */,
      .maxDrawIndexedIndexValue = UINT32_MAX,
      .maxDrawIndirectCount = UINT32_MAX,
      .maxSamplerLodBias = 16,
      .maxSamplerAnisotropy = 16,
      .maxViewports = MAX_VIEWPORTS,
      .maxViewportDimensions = { (1 << 14), (1 << 14) },
      .viewportBoundsRange = { INT16_MIN, INT16_MAX },
      .viewportSubPixelBits = 8,
      .minMemoryMapAlignment = 4096, /* A page */
      .minTexelBufferOffsetAlignment = 1,
      .minUniformBufferOffsetAlignment = 4,
      .minStorageBufferOffsetAlignment = 4,
      .minTexelOffset = -32,
      .maxTexelOffset = 31,
      .minTexelGatherOffset = -32,
      .maxTexelGatherOffset = 31,
      .minInterpolationOffset = -2,
      .maxInterpolationOffset = 2,
      .subPixelInterpolationOffsetBits = 8,
      .maxFramebufferWidth = (1 << 14),
      .maxFramebufferHeight = (1 << 14),
      .maxFramebufferLayers = (1 << 10),
      .framebufferColorSampleCounts = sample_counts,
      .framebufferDepthSampleCounts = sample_counts,
      .framebufferStencilSampleCounts = sample_counts,
      .framebufferNoAttachmentsSampleCounts = sample_counts,
      .maxColorAttachments = MAX_RTS,
      .sampledImageColorSampleCounts = sample_counts,
      .sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .sampledImageDepthSampleCounts = sample_counts,
      .sampledImageStencilSampleCounts = sample_counts,
      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords = 1,
      .timestampComputeAndGraphics = true,
      .timestampPeriod = 1,
      .maxClipDistances = 8,
      .maxCullDistances = 8,
      .maxCombinedClipAndCullDistances = 8,
      .discreteQueuePriorities = 1,
      .pointSizeRange = { 0.125, 255.875 },
      .lineWidthRange = { 0.0, 7.9921875 },
      .pointSizeGranularity = (1.0 / 8.0),
      .lineWidthGranularity = (1.0 / 128.0),
      .strictLines = false, /* FINISHME */
      .standardSampleLocations = true,
      .optimalBufferCopyOffsetAlignment = 128,
      .optimalBufferCopyRowPitchAlignment = 128,
      .nonCoherentAtomSize = 64,
   };

   *pProperties = (VkPhysicalDeviceProperties) {
      .apiVersion = pan_physical_device_api_version(pdevice),
      .driverVersion = vk_get_driver_version(),
      .vendorID = 0, /* TODO */
      .deviceID = 0,
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      .limits = limits,
      .sparseProperties = { 0 },
   };

   strcpy(pProperties->deviceName, pdevice->name);
   memcpy(pProperties->pipelineCacheUUID, pdevice->cache_uuid, VK_UUID_SIZE);
}

void
pan_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                 VkPhysicalDeviceProperties2 *pProperties)
{
   PAN_FROM_HANDLE(pan_physical_device, pdevice, physicalDevice);
   pan_GetPhysicalDeviceProperties(physicalDevice, &pProperties->properties);

   vk_foreach_struct(ext, pProperties->pNext)
   {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR: {
         VkPhysicalDevicePushDescriptorPropertiesKHR *properties = (VkPhysicalDevicePushDescriptorPropertiesKHR *)ext;
         properties->maxPushDescriptors = MAX_PUSH_DESCRIPTORS;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES: {
         VkPhysicalDeviceIDProperties *properties = (VkPhysicalDeviceIDProperties *)ext;
         memcpy(properties->driverUUID, pdevice->driver_uuid, VK_UUID_SIZE);
         memcpy(properties->deviceUUID, pdevice->device_uuid, VK_UUID_SIZE);
         properties->deviceLUIDValid = false;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES: {
         VkPhysicalDeviceMultiviewProperties *properties = (VkPhysicalDeviceMultiviewProperties *)ext;
         properties->maxMultiviewViewCount = MAX_VIEWS;
         properties->maxMultiviewInstanceIndex = INT_MAX;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES: {
         VkPhysicalDevicePointClippingProperties *properties = (VkPhysicalDevicePointClippingProperties *)ext;
         properties->pointClippingBehavior =
            VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES: {
         VkPhysicalDeviceMaintenance3Properties *properties = (VkPhysicalDeviceMaintenance3Properties *)ext;
         /* Make sure everything is addressable by a signed 32-bit int, and
          * our largest descriptors are 96 bytes. */
         properties->maxPerSetDescriptors = (1ull << 31) / 96;
         /* Our buffer size fields allow only this much */
         properties->maxMemoryAllocationSize = 0xFFFFFFFFull;
         break;
      }
      default:
         break;
      }
   }
}

static const VkQueueFamilyProperties pan_queue_family_properties = {
   .queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
   .queueCount = 1,
   .timestampValidBits = 64,
   .minImageTransferGranularity = { 1, 1, 1 },
};

void
pan_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice,
                                           uint32_t *pQueueFamilyPropertyCount,
                                           VkQueueFamilyProperties *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pQueueFamilyPropertyCount);

   vk_outarray_append(&out, p) { *p = pan_queue_family_properties; }
}

void
pan_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice,
                                            uint32_t *pQueueFamilyPropertyCount,
                                            VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE(out, pQueueFamilyProperties, pQueueFamilyPropertyCount);

   vk_outarray_append(&out, p)
   {
      p->queueFamilyProperties = pan_queue_family_properties;
   }
}

static uint64_t
pan_get_system_heap_size()
{
   struct sysinfo info;
   sysinfo(&info);

   uint64_t total_ram = (uint64_t)info.totalram * info.mem_unit;

   /* We don't want to burn too much ram with the GPU.  If the user has 4GiB
    * or less, we use at most half.  If they have more than 4GiB, we use 3/4.
    */
   uint64_t available_ram;
   if (total_ram <= 4ull * 1024 * 1024 * 1024)
      available_ram = total_ram / 2;
   else
      available_ram = total_ram * 3 / 4;

   return available_ram;
}

void
pan_GetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice,
                                      VkPhysicalDeviceMemoryProperties *pMemoryProperties)
{
   pMemoryProperties->memoryHeapCount = 1;
   pMemoryProperties->memoryHeaps[0].size = pan_get_system_heap_size();
   pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

   pMemoryProperties->memoryTypeCount = 1;
   pMemoryProperties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   pMemoryProperties->memoryTypes[0].heapIndex = 0;
}

void
pan_GetPhysicalDeviceMemoryProperties2(VkPhysicalDevice physicalDevice,
                                       VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   return pan_GetPhysicalDeviceMemoryProperties(physicalDevice,
                                                &pMemoryProperties->memoryProperties);
}

static int
pan_get_device_extension_index(const char *name)
{
   for (unsigned i = 0; i < PAN_DEVICE_EXTENSION_COUNT; ++i) {
      if (strcmp(name, pan_device_extensions[i].extensionName) == 0)
         return i;
   }
   return -1;
}

static VkResult
pan_queue_init(struct pan_device *device,
              struct pan_queue *queue,
              uint32_t queue_family_index,
              int idx,
              VkDeviceQueueCreateFlags flags)
{
   queue->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   queue->device = device;
   queue->queue_family_index = queue_family_index;
   queue->queue_idx = idx;
   queue->flags = flags;

   return VK_SUCCESS;
}

static void
pan_queue_finish(struct pan_queue *queue)
{
}

VkResult
pan_CreateDevice(VkPhysicalDevice physicalDevice,
                 const VkDeviceCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkDevice *pDevice)
{
   PAN_FROM_HANDLE(pan_physical_device, physical_device, physicalDevice);
   VkResult result;
   struct pan_device *device;

   /* Check enabled features */
   if (pCreateInfo->pEnabledFeatures) {
      VkPhysicalDeviceFeatures supported_features;
      pan_GetPhysicalDeviceFeatures(physicalDevice, &supported_features);
      VkBool32 *supported_feature = (VkBool32 *) &supported_features;
      VkBool32 *enabled_feature = (VkBool32 *) pCreateInfo->pEnabledFeatures;
      unsigned num_features =
         sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
      for (uint32_t i = 0; i < num_features; i++) {
         if (enabled_feature[i] && !supported_feature[i])
            return vk_error(physical_device->instance,
                            VK_ERROR_FEATURE_NOT_PRESENT);
      }
   }

   device = vk_zalloc2(&physical_device->instance->alloc, pAllocator,
                       sizeof(*device), 8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   device->_loader_data.loaderMagic = ICD_LOADER_MAGIC;
   device->instance = physical_device->instance;
   device->physical_device = physical_device;

   if (pAllocator)
      device->alloc = *pAllocator;
   else
      device->alloc = physical_device->instance->alloc;

   for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
      const char *ext_name = pCreateInfo->ppEnabledExtensionNames[i];
      int index = pan_get_device_extension_index(ext_name);
      if (index < 0 ||
          !physical_device->supported_extensions.extensions[index]) {
         vk_free(&device->alloc, device);
         return vk_error(physical_device->instance,
                         VK_ERROR_EXTENSION_NOT_PRESENT);
      }

      device->enabled_extensions.extensions[index] = true;
   }

   for (unsigned i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *queue_create =
         &pCreateInfo->pQueueCreateInfos[i];
      uint32_t qfi = queue_create->queueFamilyIndex;
      device->queues[qfi] = vk_alloc(
         &device->alloc, queue_create->queueCount * sizeof(struct pan_queue),
         8, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!device->queues[qfi]) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      memset(device->queues[qfi], 0,
             queue_create->queueCount * sizeof(struct pan_queue));

      device->queue_count[qfi] = queue_create->queueCount;

      for (unsigned q = 0; q < queue_create->queueCount; q++) {
         result = pan_queue_init(device, &device->queues[qfi][q], qfi, q,
                                queue_create->flags);
         if (result != VK_SUCCESS)
            goto fail;
      }
   }

   VkPipelineCacheCreateInfo ci;
   ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
   ci.pNext = NULL;
   ci.flags = 0;
   ci.pInitialData = NULL;
   ci.initialDataSize = 0;
   VkPipelineCache pc;
   result =
      pan_CreatePipelineCache(pan_device_to_handle(device), &ci, NULL, &pc);
   if (result != VK_SUCCESS)
      goto fail;

   device->mem_cache = pan_pipeline_cache_from_handle(pc);

   *pDevice = pan_device_to_handle(device);
   return VK_SUCCESS;

fail:
   for (unsigned i = 0; i < PAN_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         pan_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_free(&device->alloc, device->queues[i]);
   }

   vk_free(&device->alloc, device);
   return result;
}

void
pan_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

VkResult
pan_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                     VkLayerProperties *pProperties)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_EnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice,
                                   uint32_t *pPropertyCount,
                                   VkLayerProperties *pProperties)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_GetDeviceQueue2(VkDevice _device,
                    const VkDeviceQueueInfo2 *pQueueInfo,
                    VkQueue *pQueue)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   struct pan_queue *queue;

   queue = &device->queues[pQueueInfo->queueFamilyIndex][pQueueInfo->queueIndex];
   if (pQueueInfo->flags != queue->flags) {
      /* From the Vulkan 1.1.70 spec:
       *
       * "The queue returned by vkGetDeviceQueue2 must have the same
       * flags value from this structure as that used at device
       * creation time in a VkDeviceQueueCreateInfo instance. If no
       * matching flags were specified at device creation time then
       * pQueue will return VK_NULL_HANDLE."
       */
      *pQueue = VK_NULL_HANDLE;
      return;
   }

   *pQueue = pan_queue_to_handle(queue);
}

void
pan_GetDeviceQueue(VkDevice _device,
                   uint32_t queueFamilyIndex,
                   uint32_t queueIndex,
                   VkQueue *pQueue)
{
   const VkDeviceQueueInfo2 info = (VkDeviceQueueInfo2) {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
      .queueFamilyIndex = queueFamilyIndex,
      .queueIndex = queueIndex
   };

   pan_GetDeviceQueue2(_device, &info, pQueue);
}

VkResult
pan_QueueSubmit(VkQueue _queue,
                uint32_t submitCount,
                const VkSubmitInfo *pSubmits,
                VkFence _fence)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_QueueWaitIdle(VkQueue _queue)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_DeviceWaitIdle(VkDevice _device)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                         uint32_t *pPropertyCount,
                                         VkExtensionProperties *pProperties)
{
   VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

   /* We spport no lyaers */
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   for (int i = 0; i < PAN_INSTANCE_EXTENSION_COUNT; i++) {
      if (pan_supported_instance_extensions.extensions[i]) {
         vk_outarray_append(&out, prop) { *prop = pan_instance_extensions[i]; }
      }
   }

   return vk_outarray_status(&out);
}

VkResult
pan_EnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice,
                                       const char *pLayerName,
                                       uint32_t *pPropertyCount,
                                       VkExtensionProperties *pProperties)
{
   /* We spport no lyaers */
   PAN_FROM_HANDLE(pan_physical_device, device, physicalDevice);
   VK_OUTARRAY_MAKE(out, pProperties, pPropertyCount);

   /* We spport no lyaers */
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   for (int i = 0; i < PAN_DEVICE_EXTENSION_COUNT; i++) {
      if (device->supported_extensions.extensions[i]) {
         vk_outarray_append(&out, prop) { *prop = pan_device_extensions[i]; }
      }
   }

   return vk_outarray_status(&out);
}

PFN_vkVoidFunction
pan_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   PAN_FROM_HANDLE(pan_instance, instance, _instance);

   return pan_lookup_entrypoint_checked(pName,
                                        instance ? instance->api_version : 0,
                                        instance ? &instance->enabled_extensions : NULL,
                                        NULL);
}

/* The loader wants us to expose a second GetInstanceProcAddr function
 * to work around certain LD_PRELOAD issues seen in apps.
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName);

PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return pan_GetInstanceProcAddr(instance, pName);
}

PFN_vkVoidFunction
pan_GetDeviceProcAddr(VkDevice _device, const char *pName)
{
   PAN_FROM_HANDLE(pan_device, device, _device);

   return pan_lookup_entrypoint_checked(pName, device->instance->api_version,
                                        &device->instance->enabled_extensions,
                                        &device->enabled_extensions);
}

VkResult
pan_AllocateMemory(VkDevice _device,
                   const VkMemoryAllocateInfo *pAllocateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkDeviceMemory *pMem)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   struct pan_device_memory *mem;
   VkResult result;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   if (pAllocateInfo->allocationSize == 0) {
      /* Apparently, this is allowed */
      *pMem = VK_NULL_HANDLE;
      return VK_SUCCESS;
   }

   mem = vk_alloc2(&device->alloc, pAllocator, sizeof(*mem), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (mem == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);
   if (fd_info && !fd_info->handleType)
      fd_info = NULL;

   if (fd_info) {
      assert(fd_info->handleType ==
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
             fd_info->handleType ==
                VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      /*
       * TODO Importing the same fd twice gives us the same handle without
       * reference counting.  We need to maintain a per-instance handle-to-bo
       * table and add reference count to pan_bo.
       */
      result = pan_bo_init_dmabuf(device, &mem->bo,
                                 pAllocateInfo->allocationSize, fd_info->fd);
      if (result == VK_SUCCESS) {
         /* take ownership and close the fd */
         close(fd_info->fd);
      }
   } else {
      result =
         pan_bo_init_new(device, &mem->bo, pAllocateInfo->allocationSize, 0);
   }

   if (result != VK_SUCCESS) {
      vk_free2(&device->alloc, pAllocator, mem);
      return result;
   }

   mem->size = pAllocateInfo->allocationSize;
   mem->type_index = pAllocateInfo->memoryTypeIndex;

   mem->map = NULL;
   mem->user_ptr = NULL;

   *pMem = pan_device_memory_to_handle(mem);

   return VK_SUCCESS;
}

void
pan_FreeMemory(VkDevice _device,
               VkDeviceMemory _mem,
               const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

VkResult
pan_MapMemory(VkDevice _device,
              VkDeviceMemory _memory,
              VkDeviceSize offset,
              VkDeviceSize size,
              VkMemoryMapFlags flags,
              void **ppData)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   PAN_FROM_HANDLE(pan_device_memory, mem, _memory);
   VkResult result;

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   if (mem->user_ptr) {
      *ppData = mem->user_ptr;
   } else if (!mem->map) {
      result = pan_bo_map(device, &mem->bo);
      if (result != VK_SUCCESS)
         return result;
      *ppData = mem->map = mem->bo.map;
   } else
      *ppData = mem->map;

   if (*ppData) {
      *ppData += offset;
      return VK_SUCCESS;
   }

   return vk_error(device->instance, VK_ERROR_MEMORY_MAP_FAILED);
}

void
pan_UnmapMemory(VkDevice _device, VkDeviceMemory _memory)
{
   assert(0);
}

VkResult
pan_FlushMappedMemoryRanges(VkDevice _device,
                            uint32_t memoryRangeCount,
                            const VkMappedMemoryRange *pMemoryRanges)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_InvalidateMappedMemoryRanges(VkDevice _device,
                                 uint32_t memoryRangeCount,
                                 const VkMappedMemoryRange *pMemoryRanges)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_GetBufferMemoryRequirements(VkDevice _device,
                                VkBuffer _buffer,
                                VkMemoryRequirements *pMemoryRequirements)
{
   PAN_FROM_HANDLE(pan_buffer, buffer, _buffer);

   pMemoryRequirements->memoryTypeBits = 1;
   pMemoryRequirements->alignment = 16;
   pMemoryRequirements->size = ALIGN_POT(buffer->size, 16);
}

void
pan_GetBufferMemoryRequirements2(VkDevice device,
                                 const VkBufferMemoryRequirementsInfo2 *pInfo,
                                 VkMemoryRequirements2 *pMemoryRequirements)
{
   assert(0);
}

void
pan_GetImageMemoryRequirements(VkDevice _device,
                               VkImage _image,
                               VkMemoryRequirements *pMemoryRequirements)
{
   assert(0);
}

void
pan_GetImageMemoryRequirements2(VkDevice device,
                               const VkImageMemoryRequirementsInfo2 *pInfo,
                               VkMemoryRequirements2 *pMemoryRequirements)
{
   assert(0);
}

void
pan_GetImageSparseMemoryRequirements(VkDevice device, VkImage image,
                                     uint32_t *pSparseMemoryRequirementCount,
                                     VkSparseImageMemoryRequirements *pSparseMemoryRequirements)
{
   assert(0);
}

void
pan_GetImageSparseMemoryRequirements2(VkDevice device,
                                      const VkImageSparseMemoryRequirementsInfo2 *pInfo,
                                      uint32_t *pSparseMemoryRequirementCount,
                                      VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   assert(0);
}

void
pan_GetDeviceMemoryCommitment(VkDevice device,
                              VkDeviceMemory memory,
                              VkDeviceSize *pCommittedMemoryInBytes)
{
   assert(0);
}

VkResult
pan_BindBufferMemory2(VkDevice device,
                      uint32_t bindInfoCount,
                      const VkBindBufferMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      PAN_FROM_HANDLE(pan_device_memory, mem, pBindInfos[i].memory);
      PAN_FROM_HANDLE(pan_buffer, buffer, pBindInfos[i].buffer);

      if (mem) {
         buffer->bo = &mem->bo;
         buffer->bo_offset = pBindInfos[i].memoryOffset;
      } else {
         buffer->bo = NULL;
      }
   }
   return VK_SUCCESS;
}

VkResult
pan_BindBufferMemory(VkDevice device,
                     VkBuffer buffer,
                     VkDeviceMemory memory,
                     VkDeviceSize memoryOffset)
{
   const VkBindBufferMemoryInfo info = {
      .sType = VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,
      .buffer = buffer,
      .memory = memory,
      .memoryOffset = memoryOffset
   };

   return pan_BindBufferMemory2(device, 1, &info);
}

VkResult
pan_BindImageMemory2(VkDevice device,
                     uint32_t bindInfoCount,
                     const VkBindImageMemoryInfo *pBindInfos)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_BindImageMemory(VkDevice device,
                    VkImage image,
                    VkDeviceMemory memory,
                    VkDeviceSize memoryOffset)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_QueueBindSparse(VkQueue _queue,
                    uint32_t bindInfoCount,
                    const VkBindSparseInfo *pBindInfo,
                    VkFence _fence)
{
   assert(0);
   return VK_SUCCESS;
}

// Queue semaphore functions

VkResult
pan_CreateSemaphore(VkDevice _device,
                    const VkSemaphoreCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkSemaphore *pSemaphore)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_DestroySemaphore(VkDevice _device,
                     VkSemaphore _semaphore,
                     const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

VkResult
pan_CreateEvent(VkDevice _device,
                const VkEventCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *pAllocator,
                VkEvent *pEvent)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_DestroyEvent(VkDevice _device,
                 VkEvent _event,
                 const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

VkResult
pan_GetEventStatus(VkDevice _device, VkEvent _event)
{
   assert(0);
   return VK_EVENT_RESET;
}

VkResult
pan_SetEvent(VkDevice _device, VkEvent _event)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_ResetEvent(VkDevice _device, VkEvent _event)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_CreateBuffer(VkDevice _device,
                 const VkBufferCreateInfo *pCreateInfo,
                 const VkAllocationCallbacks *pAllocator,
                 VkBuffer *pBuffer)
{
   PAN_FROM_HANDLE(pan_device, device, _device);
   struct pan_buffer *buffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);

   buffer = vk_alloc2(&device->alloc, pAllocator, sizeof(*buffer), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (buffer == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   buffer->size = pCreateInfo->size;
   buffer->usage = pCreateInfo->usage;
   buffer->flags = pCreateInfo->flags;

   *pBuffer = pan_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

void
pan_DestroyBuffer(VkDevice _device,
                  VkBuffer _buffer,
                  const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

VkResult
pan_CreateFramebuffer(VkDevice _device,
                      const VkFramebufferCreateInfo *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkFramebuffer *pFramebuffer)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_DestroyFramebuffer(VkDevice _device,
                       VkFramebuffer _fb,
                       const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

VkResult
pan_CreateSampler(VkDevice _device,
                  const VkSamplerCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator,
                  VkSampler *pSampler)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_DestroySampler(VkDevice _device,
                   VkSampler _sampler,
                   const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

/* vk_icd.h does not declare this function, so we declare it here to
 * suppress Wmissing-prototypes.
 */
PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion);

PUBLIC VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion)
{
   /* For the full details on loader interface versioning, see
    * <https://github.com/KhronosGroup/Vulkan-LoaderAndValidationLayers/blob/master/loader/LoaderAndLayerInterface.md>.
    * What follows is a condensed summary, to help you navigate the large and
    * confusing official doc.
    *
    *   - Loader interface v0 is incompatible with later versions. We don't
    *     support it.
    *
    *   - In loader interface v1:
    *       - The first ICD entrypoint called by the loader is
    *         vk_icdGetInstanceProcAddr(). The ICD must statically expose this
    *         entrypoint.
    *       - The ICD must statically expose no other Vulkan symbol unless it
    * is linked with -Bsymbolic.
    *       - Each dispatchable Vulkan handle created by the ICD must be
    *         a pointer to a struct whose first member is VK_LOADER_DATA. The
    *         ICD must initialize VK_LOADER_DATA.loadMagic to
    * ICD_LOADER_MAGIC.
    *       - The loader implements vkCreate{PLATFORM}SurfaceKHR() and
    *         vkDestroySurfaceKHR(). The ICD must be capable of working with
    *         such loader-managed surfaces.
    *
    *    - Loader interface v2 differs from v1 in:
    *       - The first ICD entrypoint called by the loader is
    *         vk_icdNegotiateLoaderICDInterfaceVersion(). The ICD must
    *         statically expose this entrypoint.
    *
    *    - Loader interface v3 differs from v2 in:
    *        - The ICD must implement vkCreate{PLATFORM}SurfaceKHR(),
    *          vkDestroySurfaceKHR(), and other API which uses VKSurfaceKHR,
    *          because the loader no longer does so.
    */
   *pSupportedVersion = MIN2(*pSupportedVersion, 3u);
   return VK_SUCCESS;
}

VkResult
pan_GetMemoryFdKHR(VkDevice _device,
                   const VkMemoryGetFdInfoKHR *pGetFdInfo,
                   int *pFd)
{
   assert(0);
   return VK_SUCCESS;
}

VkResult
pan_GetMemoryFdPropertiesKHR(VkDevice _device,
                             VkExternalMemoryHandleTypeFlagBits handleType,
                             int fd,
                             VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_GetPhysicalDeviceExternalSemaphoreProperties(VkPhysicalDevice physicalDevice,
                                                 const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
                                                 VkExternalSemaphoreProperties *pExternalSemaphoreProperties)
{
   assert(0);
}

void
pan_GetPhysicalDeviceExternalFenceProperties(VkPhysicalDevice physicalDevice,
                                             const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo,
                                             VkExternalFenceProperties *pExternalFenceProperties)
{
   assert(0);
}

VkResult
pan_CreateDebugReportCallbackEXT(VkInstance _instance,
                                 const VkDebugReportCallbackCreateInfoEXT *pCreateInfo,
                                 const VkAllocationCallbacks *pAllocator,
                                 VkDebugReportCallbackEXT *pCallback)
{
   assert(0);
   return VK_SUCCESS;
}

void
pan_DestroyDebugReportCallbackEXT(VkInstance _instance,
                                  VkDebugReportCallbackEXT _callback,
                                  const VkAllocationCallbacks *pAllocator)
{
   assert(0);
}

void
pan_DebugReportMessageEXT(VkInstance _instance,
                          VkDebugReportFlagsEXT flags,
                          VkDebugReportObjectTypeEXT objectType,
                          uint64_t object,
                          size_t location,
                          int32_t messageCode,
                          const char *pLayerPrefix,
                          const char *pMessage)
{
   assert(0);
}

void
pan_GetDeviceGroupPeerMemoryFeatures(VkDevice device,
                                     uint32_t heapIndex,
                                     uint32_t localDeviceIndex,
                                     uint32_t remoteDeviceIndex,
                                     VkPeerMemoryFeatureFlags *pPeerMemoryFeatures)
{
   assert(0);
}

uint32_t
pan_gem_new(const struct pan_device *dev, uint64_t *size, uint32_t flags)
{
   struct drm_panfrost_create_bo create_bo = { .size = *size, .flags = flags };
   int ret;

   ret = drmIoctl(dev->physical_device->local_fd, DRM_IOCTL_PANFROST_CREATE_BO,
                  &create_bo);
   assert(!ret);
   *size = create_bo.size;
   return create_bo.handle;
}

uint32_t
pan_gem_import_dmabuf(const struct pan_device *dev, int prime_fd, uint64_t size)
{
   uint32_t handle = 0;
   int ret;

   ret = drmPrimeFDToHandle(dev->physical_device->local_fd, prime_fd, &handle);
   assert(!ret);
   return handle;
}

int
pan_gem_export_dmabuf(const struct pan_device *dev, uint32_t gem_handle)
{
   int fd, ret;

   ret = drmPrimeHandleToFD(dev->physical_device->local_fd, gem_handle,
                            DRM_CLOEXEC, &fd);
   assert(!ret);
   return fd;
}

void
pan_gem_close(const struct pan_device *dev, uint32_t gem_handle)
{
   struct drm_gem_close req = {
      .handle = gem_handle,
   };
   int ret;

   ret = drmIoctl(dev->physical_device->local_fd, DRM_IOCTL_GEM_CLOSE, &req);
   assert(!ret);
}

uint64_t
pan_gem_info_offset(const struct pan_device *dev, uint32_t gem_handle)
{
   struct drm_panfrost_mmap_bo req = {
      .handle = gem_handle,
   };
   int ret;

   ret = drmIoctl(dev->physical_device->local_fd,
		  DRM_IOCTL_PANFROST_MMAP_BO, &req);
   assert(!ret);

   return req.offset;
}

uint64_t
pan_gem_info_iova(const struct pan_device *dev, uint32_t gem_handle)
{
   struct drm_panfrost_get_bo_offset req = {
      .handle = gem_handle,
   };
   int ret;

   ret = drmIoctl(dev->physical_device->local_fd,
		  DRM_IOCTL_PANFROST_GET_BO_OFFSET, &req);
   assert(!ret);

   return req.offset;
}

static VkResult
pan_bo_init(struct pan_device *dev,
            struct pan_bo *bo,
            uint32_t gem_handle,
            uint64_t size)
{
   uint64_t iova = pan_gem_info_iova(dev, gem_handle);
   if (!iova)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   *bo = (struct pan_bo) {
      .gem_handle = gem_handle,
      .size = size,
      .iova = iova,
   };

   return VK_SUCCESS;
}

VkResult
pan_bo_init_new(struct pan_device *dev, struct pan_bo *bo, uint64_t size,
                uint32_t flags)
{
   uint32_t gem_handle = pan_gem_new(dev, &size, flags);
   if (!gem_handle)
      return vk_error(dev->instance, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   VkResult result = pan_bo_init(dev, bo, gem_handle, size);
   if (result != VK_SUCCESS) {
      pan_gem_close(dev, gem_handle);
      return vk_error(dev->instance, result);
   }

   return VK_SUCCESS;
}

VkResult
pan_bo_init_dmabuf(struct pan_device *dev,
                   struct pan_bo *bo,
                   uint64_t size,
                   int fd)
{
   uint32_t gem_handle = pan_gem_import_dmabuf(dev, fd, size);
   if (!gem_handle)
      return vk_error(dev->instance, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   VkResult result = pan_bo_init(dev, bo, gem_handle, size);
   if (result != VK_SUCCESS) {
      pan_gem_close(dev, gem_handle);
      return vk_error(dev->instance, result);
   }

   return VK_SUCCESS;
}

int
pan_bo_export_dmabuf(struct pan_device *dev, struct pan_bo *bo)
{
  return pan_gem_export_dmabuf(dev, bo->gem_handle);
}

VkResult
pan_bo_map(struct pan_device *dev, struct pan_bo *bo)
{
   if (bo->map)
      return VK_SUCCESS;

   uint64_t offset = pan_gem_info_offset(dev, bo->gem_handle);
   assert(offset);
   if (!offset)
      return vk_error(dev->instance, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   /* TODO: Should we use the wrapper os_mmap() like Freedreno does? */
   void *map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                    dev->physical_device->local_fd, offset);
   assert(map != MAP_FAILED);
   if (map == MAP_FAILED)
      return vk_error(dev->instance, VK_ERROR_MEMORY_MAP_FAILED);

   bo->map = map;
   return VK_SUCCESS;
}

void
pan_bo_finish(struct pan_device *dev, struct pan_bo *bo)
{
   assert(bo->gem_handle);

   if (bo->map)
      munmap(bo->map, bo->size);

   pan_gem_close(dev, bo->gem_handle);
}
