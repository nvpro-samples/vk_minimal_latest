/*
 * Copyright (c) 2023-2026, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *
 * SPDX-FileCopyrightText: Copyright (c) 2023-2026, NVIDIA CORPORATION.
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*------------------------------------------------------------------

# vk_framework.h

This header is the reusable Vulkan elements used by `minimal_latest.cpp`.
Read it top to bottom first, then continue into the sample. Both files are
ordered small-to-large; together they present modern-Vulkan patterns from
the lowest-level helpers up to the full application.

Note: this header intentionally does NOT declare the VMA implementation.
The `VMA_IMPLEMENTATION` macro and its `#include "vk_mem_alloc.h"` live in
exactly one .cpp -- see the top of `minimal_latest.cpp`.

------------------------------------------------------------------*/


//--- Vulkan loader + memory allocator ------------------------------------------
// Volk provides runtime Vulkan function loading (no static link to the loader).
// The VMA header here brings the types only; the implementation is compiled in
// minimal_latest.cpp with VMA_IMPLEMENTATION defined exactly once.
#include "volk.h"
#include "vk_mem_alloc.h"

// Enum-to-string helper used by VK_CHECK to print VkResult values.
#include "vulkan/vk_enum_string_helper.h"

//--- GLFW (window system + monitor queries for FramePacer) ---------------------
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#include <signal.h>  // For SIGTRAP inside Context::debugCallback
#endif
#include <GLFW/glfw3.h>

//--- GLM (used by framework classes that take glm types in their APIs) ---------
#include <glm/glm.hpp>

//--- Logger + debug naming (sibling headers; included after Vulkan) ------------
#include "logger.h"
#include "debug_util.h"

//--- ImGui (utils::ImTexture / ImGui_ImplVulkan_AddTexture) --------------------
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"
#include "imgui_internal.h"  // For Docking

//--- Windows timing APIs used by FramePacer ------------------------------------
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#undef APIENTRY  // GLFW defines this but Windows tries to redefine it
#include <Windows.h>
#include <timeapi.h>
#endif

//--- Standard library ----------------------------------------------------------
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/***************************************************************
 ***************************************************************
 ********************        MACROS        ********************
 ***************************************************************
 ***************************************************************/

// ASSERT -- throws in release (NDEBUG), asserts in debug. Used pervasively.
#ifdef NDEBUG
#define ASSERT(condition, message)                                                                                     \
  do                                                                                                                   \
  {                                                                                                                    \
    if(!(condition))                                                                                                   \
    {                                                                                                                  \
      throw std::runtime_error(message);                                                                               \
    }                                                                                                                  \
  } while(false)
#else
#define ASSERT(condition, message) assert((condition) && (message))
#endif

// VK_CHECK is active in both Debug and Release builds. Release builds must still
// surface VK_ERROR_DEVICE_LOST / VK_ERROR_SURFACE_LOST_KHR / allocation failures
// from entry points like vkQueueSubmit2 or vkAcquireNextImageKHR; silently
// discarding those results is how shipped Vulkan apps hide catastrophic bugs.
// ASSERT internally decides how to terminate in debug vs release.
#define VK_CHECK(vkFnc)                                                                                                \
  do                                                                                                                   \
  {                                                                                                                    \
    if(const VkResult checkResult = (vkFnc); checkResult != VK_SUCCESS)                                                \
    {                                                                                                                  \
      const char* errMsg = string_VkResult(checkResult);                                                               \
      LOGE("Vulkan error at %s:%d: %s", __FILE__, __LINE__, errMsg);                                                   \
      ASSERT(checkResult == VK_SUCCESS, errMsg);                                                                       \
    }                                                                                                                  \
  } while(0)

namespace utils {

/***************************************************************
 ***************************************************************
 ****************        RESOURCE TYPES        ****************
 ***************************************************************
 ***************************************************************/

/*--
 * A buffer is a region of memory used to store data.
 * It is used to store vertex data, index data, uniform data, and other types of data.
 * There is a VkBuffer object that represents the buffer, and a VmaAllocation object that represents the memory allocation.
 * The address is used to access the buffer in the shader.
-*/
struct Buffer
{
  VkBuffer        buffer{};      // Vulkan Buffer
  VmaAllocation   allocation{};  // Memory associated with the buffer
  VkDeviceAddress address{};     // Address of the buffer in the shader
};

/*--
 * An image is a region of memory used to store image data.
 * It is used to store texture data, framebuffer data, and other types of data.
-*/
struct Image
{
  VkImage       image{};       // Vulkan Image
  VmaAllocation allocation{};  // Memory associated with the image
};

/*-- 
 * The image resource is an image with an image view and a layout.
 * and other information like format and extent.
-*/
struct ImageResource : Image
{
  VkExtent2D    extent{};  // Size of the image
  VkFormat      format{};  // Format of the image (e.g. VK_FORMAT_R8G8B8A8_UNORM)
  VkImageLayout layout{};  // Layout of the image (color attachment, shader read, ...)
};

/*- Not implemented here -*/
struct AccelerationStructure
{
  VkAccelerationStructureKHR accel{};
  VmaAllocation              allocation{};
  VkDeviceAddress            deviceAddress{};
  VkDeviceSize               size{};
  Buffer                     buffer;  // Underlying buffer
};

/*--
 * A queue is a sequence of commands that are executed in order.
 * The queue is used to submit command buffers to the GPU.
 * The family index is used to identify the queue family (graphic, compute, transfer, ...) .
 * The queue index is used to identify the queue in the family, multiple queues can be in the same family.
-*/
struct QueueInfo
{
  uint32_t familyIndex = ~0U;  // Family index of the queue (graphic, compute, transfer, ...)
  uint32_t queueIndex  = ~0U;  // Index of the queue in the family
  VkQueue  queue{};            // The queue object
};


/***************************************************************
 ***************************************************************
 ****************       UTILITY HELPERS        ****************
 ***************************************************************
 ***************************************************************/

//--- Math / hash ---------------------------------------------------------------

/*--
 * Aligns a value up to the next multiple of the alignment.
 * If the alignment is 0, it returns the original value.
 */
inline VkDeviceSize alignUp(VkDeviceSize value, VkDeviceSize alignment)
{
  if(alignment == 0)
  {
    return value;
  }
  return ((value + alignment - 1) / alignment) * alignment;
}

/*-- 
 * Combines hash values using the FNV-1a based algorithm 
-*/
inline std::size_t hashCombine(std::size_t seed, auto const& value)
{
  return seed ^ (std::hash<std::decay_t<decltype(value)>>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2));
}


//--- Format queries ------------------------------------------------------------

/*--
 * A helper function to find a supported format from a list of candidates.
 * For example, we can use this function to find a supported depth format.
-*/
inline VkFormat findSupportedFormat(VkPhysicalDevice             physicalDevice,
                                    const std::vector<VkFormat>& candidates,
                                    VkImageTiling                tiling,
                                    VkFormatFeatureFlags2        features)
{
  for(const VkFormat format : candidates)
  {
    VkFormatProperties2 props{.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    vkGetPhysicalDeviceFormatProperties2(physicalDevice, format, &props);

    if(tiling == VK_IMAGE_TILING_LINEAR && (props.formatProperties.linearTilingFeatures & features) == features)
    {
      return format;
    }
    if(tiling == VK_IMAGE_TILING_OPTIMAL && (props.formatProperties.optimalTilingFeatures & features) == features)
    {
      return format;
    }
  }
  ASSERT(false, "failed to find supported format!");
  return VK_FORMAT_UNDEFINED;
}

/*--
 * A helper function to find the depth format that is supported by the physical device.
-*/
inline VkFormat findDepthFormat(VkPhysicalDevice physicalDevice)
{
  return findSupportedFormat(physicalDevice,
                             {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
                             VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}


//--- Image layout helpers ------------------------------------------------------

/*--
 * Initialize a newly created image to GENERAL layout (used for color/depth buffers)
-*/
inline void cmdInitImageLayout(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT)
{
  const VkImageMemoryBarrier2 barrier{
      .sType         = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
      .srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
      .srcAccessMask = VK_ACCESS_2_NONE,
      .dstStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
      .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
      .oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout     = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image               = image,
      .subresourceRange    = {aspectMask, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};

  const VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};

  vkCmdPipelineBarrier2(cmd, &depInfo);
}

/*--
 * Transition swapchain image from UNDEFINED to GENERAL before dynamic rendering.
-*/
inline void cmdTransitionSwapchainToRenderingLayout(VkCommandBuffer cmd, VkImage image)
{
  const VkImageMemoryBarrier2 barrier{.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                      .srcStageMask        = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                      .srcAccessMask       = VK_ACCESS_2_NONE,
                                      .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      .dstAccessMask       = VK_ACCESS_2_NONE,
                                      .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
                                      .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .image               = image,
                                      .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

  const VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
  vkCmdPipelineBarrier2(cmd, &depInfo);
}

/*--
 * Transition swapchain image from GENERAL to PRESENT_SRC_KHR before present.
-*/
inline void cmdTransitionSwapchainToPresentLayout(VkCommandBuffer cmd, VkImage image)
{
  const VkImageMemoryBarrier2 barrier{.sType        = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                      .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                                      | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                      .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                                       | VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                      .dstStageMask        = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                      .dstAccessMask       = VK_ACCESS_2_NONE,
                                      .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
                                      .newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .image               = image,
                                      .subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};

  const VkDependencyInfo depInfo{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier};
  vkCmdPipelineBarrier2(cmd, &depInfo);
}


//--- Memory barriers -----------------------------------------------------------

/*-- 
 *  This helper returns the access mask for a given stage mask.
-*/
inline VkAccessFlags2 inferAccessMaskFromStage(VkPipelineStageFlags2 stage, bool src)
{
  VkAccessFlags2 access = 0;

  // Shader stages: default to READ|WRITE for src (to flush writes), READ for dst (to consume)
  const bool hasCompute  = (stage & VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) != 0;
  const bool hasFragment = (stage & VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT) != 0;
  const bool hasVertex   = (stage & VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT) != 0;
  if(hasCompute || hasFragment || hasVertex)
  {
    access |= src ? (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT) : VK_ACCESS_2_SHADER_READ_BIT;
  }

  if((stage & VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT) != 0)
    access |= VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;  // Always read-only
  if((stage & VK_PIPELINE_STAGE_2_TRANSFER_BIT) != 0)
    access |= src ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
  ASSERT(access != 0, "Missing stage implementation");
  return access;
}

/*--
 * This useful function simplifies the addition of buffer barriers, by inferring 
 * the access masks from the stage masks, and adding the buffer barrier to the command buffer.
-*/
inline void cmdBufferMemoryBarrier(VkCommandBuffer       commandBuffer,
                                   VkBuffer              buffer,
                                   VkPipelineStageFlags2 srcStageMask,
                                   VkPipelineStageFlags2 dstStageMask,
                                   VkAccessFlags2        srcAccessMask       = 0,  // Default to infer if not provided
                                   VkAccessFlags2        dstAccessMask       = 0,  // Default to infer if not provided
                                   VkDeviceSize          offset              = 0,
                                   VkDeviceSize          size                = VK_WHOLE_SIZE,
                                   uint32_t              srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   uint32_t              dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED)
{
  // Infer access masks if not explicitly provided
  if(srcAccessMask == 0)
  {
    srcAccessMask = inferAccessMaskFromStage(srcStageMask, true);
  }
  if(dstAccessMask == 0)
  {
    dstAccessMask = inferAccessMaskFromStage(dstStageMask, false);
  }

  const std::array<VkBufferMemoryBarrier2, 1> bufferBarrier{{{.sType        = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                                                              .srcStageMask = srcStageMask,
                                                              .srcAccessMask       = srcAccessMask,
                                                              .dstStageMask        = dstStageMask,
                                                              .dstAccessMask       = dstAccessMask,
                                                              .srcQueueFamilyIndex = srcQueueFamilyIndex,
                                                              .dstQueueFamilyIndex = dstQueueFamilyIndex,
                                                              .buffer              = buffer,
                                                              .offset              = offset,
                                                              .size                = size}}};

  const VkDependencyInfo depInfo{.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                 .bufferMemoryBarrierCount = uint32_t(bufferBarrier.size()),
                                 .pBufferMemoryBarriers    = bufferBarrier.data()};
  vkCmdPipelineBarrier2(commandBuffer, &depInfo);
}


//--- Shader objects ------------------------------------------------------------

/*--
 * A helper function to create a shader module from a Spir-V code.
-*/
inline VkShaderModule createShaderModule(VkDevice device, const std::span<const uint32_t>& code)
{
  const VkShaderModuleCreateInfo createInfo{.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                            .codeSize = code.size() * sizeof(uint32_t),
                                            .pCode    = static_cast<const uint32_t*>(code.data())};
  VkShaderModule                 shaderModule{};
  VK_CHECK(vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule));
  return shaderModule;
}

/*--
 * Bind a (vertex, fragment) shader-object pair with VK_NULL_HANDLE for every
 * other graphics stage.
 *
 * VK_EXT_shader_object requires that, when shader objects are in use, every
 * graphics stage on the device is explicitly bound -- even unused ones, which
 * must be cleared to VK_NULL_HANDLE. Forgetting this is a common footgun; keep
 * this helper as the canonical call site.
-*/
inline void cmdBindGraphicsShaders(VkCommandBuffer cmd, VkShaderEXT vert, VkShaderEXT frag)
{
  const std::array<VkShaderStageFlagBits, 5> stages = {
      VK_SHADER_STAGE_VERTEX_BIT,
      VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
      VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
      VK_SHADER_STAGE_GEOMETRY_BIT,
      VK_SHADER_STAGE_FRAGMENT_BIT,
  };
  const std::array<VkShaderEXT, 5> shaders = {vert, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, frag};
  vkCmdBindShadersEXT(cmd, uint32_t(stages.size()), stages.data(), shaders.data());
}


//--- Command buffer ------------------------------------------------------------

/*--
 * Create a command pool on the given queue family.
 *
 * Common flags:
 *   VK_COMMAND_POOL_CREATE_TRANSIENT_BIT            -- short-lived command buffers
 *   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT -- reset individual buffers
 *
 * Pass 0 for persistent pools that are reset whole-pool each frame (the
 * frames-in-flight pattern).
-*/
inline VkCommandPool createCommandPool(VkDevice device, uint32_t queueFamilyIndex, VkCommandPoolCreateFlags flags = 0)
{
  const VkCommandPoolCreateInfo commandPoolCreateInfo{
      .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags            = flags,
      .queueFamilyIndex = queueFamilyIndex,
  };
  VkCommandPool pool{};
  VK_CHECK(vkCreateCommandPool(device, &commandPoolCreateInfo, nullptr, &pool));
  return pool;
}

/*-- Simple helper for the creation of a temporary command buffer, use to record the commands to upload data, or transition images. -*/
inline VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool cmdPool)
{
  const VkCommandBufferAllocateInfo allocInfo{.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool        = cmdPool,
                                              .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
  VkCommandBuffer                   cmd{};
  VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &cmd));
  const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));
  return cmd;
}

/*-- 
 * Submit the temporary command buffer, wait until the command is finished, and clean up. 
 * This is a blocking function and should be used only for small operations 
--*/
inline void endSingleTimeCommands(VkCommandBuffer cmd, VkDevice device, VkCommandPool cmdPool, VkQueue queue)
{
  // Submit and clean up
  VK_CHECK(vkEndCommandBuffer(cmd));

  // Create fence for synchronization
  const VkFenceCreateInfo fenceInfo{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  std::array<VkFence, 1>  fence{};
  VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, fence.data()));

  const VkCommandBufferSubmitInfo cmdBufferInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
  const std::array<VkSubmitInfo2, 1> submitInfo{
      {{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cmdBufferInfo}}};
  VK_CHECK(vkQueueSubmit2(queue, uint32_t(submitInfo.size()), submitInfo.data(), fence[0]));
  VK_CHECK(vkWaitForFences(device, uint32_t(fence.size()), fence.data(), VK_TRUE, UINT64_MAX));

  // Cleanup
  vkDestroyFence(device, fence[0], nullptr);
  vkFreeCommandBuffers(device, cmdPool, 1, &cmd);
}


//--- pNext chaining ------------------------------------------------------------

// Helper to chain Vulkan structures to the pNext chain
// Uses VkBaseOutStructure for type-safe chaining following Vulkan conventions
template <typename MainT, typename NewT>
inline void pNextChainPushFront(MainT* mainStruct, NewT* newStruct)
{
  // Cast to VkBaseOutStructure for proper pNext handling
  auto* newBase  = reinterpret_cast<VkBaseOutStructure*>(newStruct);
  auto* mainBase = reinterpret_cast<VkBaseOutStructure*>(mainStruct);

  newBase->pNext  = mainBase->pNext;
  mainBase->pNext = newBase;
}


//--- File I/O ------------------------------------------------------------------

/*--
 * Return the path to a file if it exists in one of the search paths.
-*/
inline std::string findFile(const std::string& filename, const std::vector<std::string>& searchPaths)
{
  for(const auto& path : searchPaths)
  {
    const std::filesystem::path filePath = std::filesystem::path(path) / filename;
    if(std::filesystem::exists(filePath))
    {
      return filePath.string();
    }
  }
  LOGE("File not found: %s", filename.c_str());
  LOGI("Search under: ");
  for(const auto& path : searchPaths)
  {
    LOGI("  %s", path.c_str());
  }
  return "";
}

/***************************************************************
 ***************************************************************
 ********************       CONTEXT        ********************
 ***************************************************************
 ***************************************************************/

// Validation settings: to fine tune what is checked
struct ValidationSettings
{
  VkBool32 fine_grained_locking{VK_TRUE};
  VkBool32 validate_core{VK_TRUE};
  VkBool32 check_image_layout{VK_TRUE};
  VkBool32 check_command_buffer{VK_TRUE};
  VkBool32 check_object_in_use{VK_TRUE};
  VkBool32 check_query{VK_TRUE};
  VkBool32 check_shaders{VK_TRUE};
  VkBool32 check_shaders_caching{VK_TRUE};
  VkBool32 unique_handles{VK_TRUE};
  VkBool32 object_lifetime{VK_TRUE};
  VkBool32 stateless_param{VK_TRUE};
  std::vector<const char*> debug_action{"VK_DBG_LAYER_ACTION_LOG_MSG"};  // "VK_DBG_LAYER_ACTION_DEBUG_OUTPUT", "VK_DBG_LAYER_ACTION_BREAK"
  std::vector<const char*> report_flags{"error", "warn"};  // Enable both errors and warnings
  std::vector<const char*> message_id_filter{"WARNING-legacy-gpdp2"};  // Filter: legacy vkGetPhysicalDeviceProperties warning from third-party libs (ImGui/VMA)

  /*--
   * Build the pNext chain to enable these settings on the validation layer.
   *
   * IMPORTANT: the returned pointer is only valid for the lifetime of *this.
   * It points into m_layerSettingsCreateInfo (and transitively into
   * m_layerSettings), both of which are members. Callers MUST ensure the
   * ValidationSettings object outlives any Vulkan call that consumes the
   * chain (typically: keep it on the stack until after vkCreateInstance).
  -*/
  VkBaseInStructure* buildPNextChain()
  {
    layerSettings = std::vector<VkLayerSettingEXT>{
        {layerName, "fine_grained_locking", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &fine_grained_locking},
        {layerName, "validate_core", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &validate_core},
        {layerName, "check_image_layout", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_image_layout},
        {layerName, "check_command_buffer", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_command_buffer},
        {layerName, "check_object_in_use", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_object_in_use},
        {layerName, "check_query", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_query},
        {layerName, "check_shaders", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_shaders},
        {layerName, "check_shaders_caching", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_shaders_caching},
        {layerName, "unique_handles", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &unique_handles},
        {layerName, "object_lifetime", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &object_lifetime},
        {layerName, "stateless_param", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &stateless_param},
        {layerName, "debug_action", VK_LAYER_SETTING_TYPE_STRING_EXT, uint32_t(debug_action.size()), debug_action.data()},
        {layerName, "report_flags", VK_LAYER_SETTING_TYPE_STRING_EXT, uint32_t(report_flags.size()), report_flags.data()},
        {layerName, "message_id_filter", VK_LAYER_SETTING_TYPE_STRING_EXT, uint32_t(message_id_filter.size()),
         message_id_filter.data()},

    };
    layerSettingsCreateInfo = {
        .sType        = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
        .settingCount = uint32_t(layerSettings.size()),
        .pSettings    = layerSettings.data(),
    };

    return reinterpret_cast<VkBaseInStructure*>(&layerSettingsCreateInfo);
  }

  static constexpr const char*   layerName{"VK_LAYER_KHRONOS_validation"};
  std::vector<VkLayerSettingEXT> layerSettings;
  VkLayerSettingsCreateInfoEXT   layerSettingsCreateInfo{};
};

/*--
 * Configuration for device extensions.
 * - name: The extension name (e.g., VK_KHR_SWAPCHAIN_EXTENSION_NAME)
 * - required: If true, the application will assert if the extension is not available
 * - featureStruct: Pointer to the feature structure to enable (or nullptr if no feature struct needed)
-*/
struct ExtensionConfig
{
  const char* name          = nullptr;
  bool        required      = false;
  void*       featureStruct = nullptr;
};

/*--
 * Configuration structure for Context initialization.
 * This allows customization of instance/device extensions, layers, and features.
 * 
 * Usage:
 *   ContextCreateInfo config;
 *   config.deviceExtensions.push_back({VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, true, &rtFeatures, VK_STRUCTURE_TYPE_...});
 *   context.init(config);
-*/
struct ContextCreateInfo
{
  // Instance configuration
  std::vector<const char*> instanceExtensions = {VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
  std::vector<const char*> instanceLayers;

  // API version
  uint32_t apiVersion = VK_API_VERSION_1_4;

  // Validation layers
#ifdef NDEBUG
  bool enableValidationLayers = false;
#else
  bool enableValidationLayers = true;
#endif

  // Device extensions with their configuration
  // Note: These are the extensions that will be requested from the device
  // The Context will check availability and enable them based on the 'required' flag
  std::vector<ExtensionConfig> deviceExtensions;
};

/*--
 * The context is the main class that holds the Vulkan instance, the physical device, the logical device, and the queue.
 * The instance is the main object that is used to interact with the Vulkan library.
 * The physical device is the GPU that is used to render the scene.
 * The logical device is the interface to the physical device.
 * The queue is used to submit command buffers to the GPU.
 *
 * Extensions and features are configured externally via ContextCreateInfo.
 * The context will check availability and enable them based on the configuration.
-*/
class Context
{
public:
  Context() = default;
  ~Context() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }

  void init(const ContextCreateInfo& createInfo)
  {
    m_createInfo = createInfo;
    initInstance();
    selectPhysicalDevice();
    initLogicalDevice();
  }

  // Destroy internal resources and reset its initial state
  void deinit()
  {
    vkDeviceWaitIdle(m_device);
    if(m_createInfo.enableValidationLayers && vkDestroyDebugUtilsMessengerEXT)
    {
      vkDestroyDebugUtilsMessengerEXT(m_instance, m_callback, nullptr);
    }
    vkDestroyDevice(m_device, nullptr);
    vkDestroyInstance(m_instance, nullptr);
    *this = {};
  }

  VkDevice         getDevice() const { return m_device; }
  VkPhysicalDevice getPhysicalDevice() const { return m_physicalDevice; }
  VkInstance       getInstance() const { return m_instance; }
  const QueueInfo& getGraphicsQueue() const
  {
    assert(!m_queues.empty() && "No queues created. Call init() first.");
    return m_queues[0];
  }
  uint32_t getApiVersion() const { return m_apiVersion; }              // Instance loader version
  uint32_t getDeviceApiVersion() const { return m_deviceApiVersion; }  // Selected physical device version


private:
  //--- Vulkan Debug ------------------------------------------------------------------------------------------------------------

  /*-- Callback function to catch validation errors  -*/
  static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                      VkDebugUtilsMessageTypeFlagsEXT,
                                                      const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                                      void*)
  {
    const Logger::LogLevel level =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0   ? Logger::LogLevel::eERROR :
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 ? Logger::LogLevel::eWARNING :
                                                                            Logger::LogLevel::eINFO;
    Logger::getInstance().log(level, "%s", callbackData->pMessage);
    if((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
#if defined(_MSVC_LANG)
      __debugbreak();
#elif defined(__linux__)
      raise(SIGTRAP);
#endif
    }
    return VK_FALSE;
  }

  void initInstance()
  {
    vkEnumerateInstanceVersion(&m_apiVersion);
    LOGI("VULKAN API: %d.%d", VK_VERSION_MAJOR(m_apiVersion), VK_VERSION_MINOR(m_apiVersion));
    ASSERT(m_apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0), "Require Vulkan 1.4 loader");

    // This finds the KHR surface extensions needed to display on the right platform
    uint32_t     glfwExtensionCount = 0;
    const char** glfwExtensions     = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    getAvailableInstanceExtensions();

    const VkApplicationInfo applicationInfo{
        .pApplicationName   = "minimal_latest",
        .applicationVersion = 1,
        .pEngineName        = "minimal_latest",
        .engineVersion      = 1,
        .apiVersion         = m_apiVersion,
    };

    // Build instance extensions list from config
    std::vector<const char*> instanceExtensions = m_createInfo.instanceExtensions;

    // Add extensions requested by GLFW (required for windowing)
    for(uint32_t i = 0; i < glfwExtensionCount; i++)
    {
      instanceExtensions.push_back(glfwExtensions[i]);
    }

    // Add optional instance extensions if available
    if(extensionIsAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, m_instanceExtensionsAvailable))
      instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if(extensionIsAvailable(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME, m_instanceExtensionsAvailable))
      instanceExtensions.push_back(VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);

    // Build instance layers list from config
    std::vector<const char*> instanceLayers = m_createInfo.instanceLayers;

    // Adding the validation layer
    if(m_createInfo.enableValidationLayers)
    {
      instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
    }

    // Setting for the validation layer
    ValidationSettings validationSettings{.validate_core = VK_TRUE};  // modify default value

    const VkInstanceCreateInfo instanceCreateInfo{
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext                   = validationSettings.buildPNextChain(),
        .pApplicationInfo        = &applicationInfo,
        .enabledLayerCount       = uint32_t(instanceLayers.size()),
        .ppEnabledLayerNames     = instanceLayers.data(),
        .enabledExtensionCount   = uint32_t(instanceExtensions.size()),
        .ppEnabledExtensionNames = instanceExtensions.data(),
    };

    // Actual Vulkan instance creation
    VK_CHECK(vkCreateInstance(&instanceCreateInfo, nullptr, &m_instance));

    // Load all Vulkan functions
    volkLoadInstance(m_instance);

    // Add the debug callback
    if(m_createInfo.enableValidationLayers && vkCreateDebugUtilsMessengerEXT)
    {
      const VkDebugUtilsMessengerCreateInfoEXT dbg_messenger_create_info{
          .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
          .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
          .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
          .pfnUserCallback = Context::debugCallback,  // <-- The callback function
      };
      VK_CHECK(vkCreateDebugUtilsMessengerEXT(m_instance, &dbg_messenger_create_info, nullptr, &m_callback));
      LOGI("Validation Layers: ON");
    }
  }

  /*--
   * The physical device is the GPU that is used to render the scene.
   * We are selecting the first discrete GPU found, if there is one.
  -*/
  void selectPhysicalDevice()
  {
    size_t chosenDevice = 0;

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    ASSERT(deviceCount != 0, "failed to find GPUs with Vulkan support!");

    std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, physicalDevices.data());

    VkPhysicalDeviceProperties2 properties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    for(size_t i = 0; i < physicalDevices.size(); i++)
    {
      vkGetPhysicalDeviceProperties2(physicalDevices[i], &properties2);
      if(properties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
      {
        chosenDevice = i;
        break;
      }
    }

    m_physicalDevice = physicalDevices[chosenDevice];
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &properties2);
    m_deviceApiVersion = properties2.properties.apiVersion;
    LOGI("Selected GPU: %s", properties2.properties.deviceName);  // Show the name of the GPU
    LOGI("Driver: %d.%d.%d", VK_VERSION_MAJOR(properties2.properties.driverVersion),
         VK_VERSION_MINOR(properties2.properties.driverVersion), VK_VERSION_PATCH(properties2.properties.driverVersion));
    LOGI("Vulkan API: %d.%d.%d", VK_VERSION_MAJOR(properties2.properties.apiVersion),
         VK_VERSION_MINOR(properties2.properties.apiVersion), VK_VERSION_PATCH(properties2.properties.apiVersion));
    ASSERT(properties2.properties.apiVersion >= VK_MAKE_API_VERSION(0, 1, 4, 0), "Require Vulkan 1.4 device, update driver!");
  }

  /*--
   * The queue is used to submit command buffers to the GPU.
   * We are selecting the first queue found (graphic), which is the most common and needed for rendering graphic elements.
   * 
   * Other types of queues are used for compute, transfer, and other types of operations.
   * In a more advanced application, the user should select the queue that fits the application needs.
   * 
   * Eventually the user should create multiple queues for different types of operations.
   * 
   * Note: The queue is created with the creation of the logical device, this is the selection which are requested when creating the logical device.
   * Note: the search of the queue could be more advanced, and search for the right queue family.
  -*/
  QueueInfo getQueue(VkQueueFlagBits flags) const
  {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(m_physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties2> queueFamilies(queueFamilyCount, {.sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
    vkGetPhysicalDeviceQueueFamilyProperties2(m_physicalDevice, &queueFamilyCount, queueFamilies.data());

    QueueInfo queueInfo;
    for(uint32_t i = 0; i < queueFamilies.size(); i++)
    {
      if(queueFamilies[i].queueFamilyProperties.queueFlags & flags)
      {
        queueInfo.familyIndex = i;
        queueInfo.queueIndex  = 0;  // We only request one queue per family; for multiple
                                    // queues, raise queueCount in VkDeviceQueueCreateInfo
                                    // and pick the desired queueIndex here.
        // queueInfo.queue is filled in after creating the logical device.
        break;
      }
    }
    return queueInfo;
  }

  /*--
   * The logical device is the interface to the physical device.
   * It is used to create resources, allocate memory, and submit command buffers to the GPU.
   * The logical device is created with the physical device and the queue family that is used.
   * The logical device is created with the extensions and features configured in ContextCreateInfo.
  -*/
  void initLogicalDevice()
  {
    const float queuePriority = 1.0F;
    m_queues.clear();
    m_queues.emplace_back(getQueue(VK_QUEUE_GRAPHICS_BIT));

    // Request only one queue : graphic
    // User could request more specific queues: compute, transfer
    const VkDeviceQueueCreateInfo queueCreateInfo{
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = m_queues[0].familyIndex,
        .queueCount       = 1,
        .pQueuePriorities = &queuePriority,
    };

    // Chaining all features up to Vulkan 1.4
    pNextChainPushFront(&m_features11, &m_features12);
    pNextChainPushFront(&m_features11, &m_features13);
    pNextChainPushFront(&m_features11, &m_features14);

    /*-- 
     * Process device extensions from configuration:
     * - Check if each extension is available on the device
     * - Enable required extensions (assert if not available)
     * - Enable optional extensions (skip if not available)
     * - Link provided feature structs to the pNext chain
    -*/
    getAvailableDeviceExtensions();

    std::vector<const char*> deviceExtensions;
    for(const auto& extConfig : m_createInfo.deviceExtensions)
    {
      if(extensionIsAvailable(extConfig.name, m_deviceExtensionsAvailable))
      {
        deviceExtensions.push_back(extConfig.name);

        // Link feature struct if provided via ExtensionConfig::featureStruct
        if(extConfig.featureStruct != nullptr)
        {
          pNextChainPushFront(&m_features11, extConfig.featureStruct);
        }
      }
      else if(extConfig.required)
      {
        // Extension is required but not available - fail with error message
        LOGE("Required extension %s is not available!", extConfig.name);
        ASSERT(false, "Required device extension not available, update driver!");
      }
      else
      {
        // Extension is optional and not available - skip it
        LOGW("Optional extension %s is not available, skipping", extConfig.name);
      }
    }

    // Requesting all supported features, which will then be activated in the device
    m_deviceFeatures.pNext = &m_features11;
    vkGetPhysicalDeviceFeatures2(m_physicalDevice, &m_deviceFeatures);

    // Validate required features - these are mandatory in Vulkan 1.4, but some drivers
    // claim 1.4 support without full conformance. Check to catch non-conformant drivers early.
    ASSERT(m_features12.timelineSemaphore, "Timeline semaphore required (Vulkan 1.2 core)");
    ASSERT(m_features12.bufferDeviceAddress, "Buffer device address required (used pervasively in this sample)");
    ASSERT(m_features13.synchronization2, "Synchronization2 required (Vulkan 1.3 core)");
    ASSERT(m_features13.dynamicRendering, "Dynamic rendering required (Vulkan 1.3 core)");
    ASSERT(m_features14.maintenance5, "Maintenance5 required (Vulkan 1.4 core)");
    ASSERT(m_features14.maintenance6, "Maintenance6 required (Vulkan 1.4 core)");

    // Create the logical device
    const VkDeviceCreateInfo deviceCreateInfo{
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &m_deviceFeatures,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &queueCreateInfo,
        .enabledExtensionCount   = uint32_t(deviceExtensions.size()),
        .ppEnabledExtensionNames = deviceExtensions.data(),
    };
    VK_CHECK(vkCreateDevice(m_physicalDevice, &deviceCreateInfo, nullptr, &m_device));
    DBG_VK_NAME(m_device);

    volkLoadDevice(m_device);  // Load all Vulkan device functions

    // Debug utility to name Vulkan objects, great in debugger like NSight
    debugUtilInitialize(m_device);

    // Get the requested queues
    vkGetDeviceQueue(m_device, m_queues[0].familyIndex, m_queues[0].queueIndex, &m_queues[0].queue);
    DBG_VK_NAME(m_queues[0].queue);

    // Log the enabled extensions
    LOGI("Enabled device extensions:");
    for(const auto& ext : deviceExtensions)
    {
      LOGI("  %s", ext);
    }
  }

  /*-- 
   * Get all available extensions for the device, because we cannot request an extension that isn't 
   * supported/available. If we do, the logical device creation would fail. 
  -*/
  void getAvailableDeviceExtensions()
  {
    uint32_t count{0};
    VK_CHECK(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, nullptr));
    m_deviceExtensionsAvailable.resize(count);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, m_deviceExtensionsAvailable.data()));
  }

  void getAvailableInstanceExtensions()
  {
    uint32_t count{0};
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    m_instanceExtensionsAvailable.resize(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, m_instanceExtensionsAvailable.data());
  }

  // Work in conjunction with the above
  bool extensionIsAvailable(const std::string& name, const std::vector<VkExtensionProperties>& extensions)
  {
    for(auto& ext : extensions)
    {
      if(name == ext.extensionName)
        return true;
    }
    return false;
  }


  // --- Members ------------------------------------------------------------------------------------------------------------
  ContextCreateInfo m_createInfo{};         // Configuration provided during init()
  uint32_t          m_apiVersion{0};        // The Vulkan instance API version (from vkEnumerateInstanceVersion)
  uint32_t          m_deviceApiVersion{0};  // The selected device's API version (from VkPhysicalDeviceProperties2)

  VkInstance                         m_instance{};        // The Vulkan instance
  VkPhysicalDevice                   m_physicalDevice{};  // The physical device (GPU)
  VkDevice                           m_device{};          // The logical device (interface to the physical device)
  std::vector<QueueInfo>             m_queues;            // The queue used to submit command buffers to the GPU
  VkDebugUtilsMessengerEXT           m_callback{VK_NULL_HANDLE};  // The debug callback
  std::vector<VkExtensionProperties> m_instanceExtensionsAvailable;
  std::vector<VkExtensionProperties> m_deviceExtensionsAvailable;

  // Core features
  VkPhysicalDeviceFeatures2        m_deviceFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceVulkan11Features m_features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
  VkPhysicalDeviceVulkan12Features m_features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features m_features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan14Features m_features14{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
};

/***************************************************************
 ***************************************************************
 ********************      SWAPCHAIN       ********************
 ***************************************************************
 ***************************************************************/

/*--
 * Swapchain: presents rendered images to the screen.
 *
 * Two distinct counts must NOT be conflated:
 *
 *   imageCount      -- how many images the swapchain owns (typically 2-4).
 *                      Triple-buffering (3) is the common modern default: it
 *                      lets the GPU work on image N+1 while N is being scanned
 *                      out by the display, with a third in the present queue.
 *                      vSync (FIFO) and tear-free (MAILBOX) both benefit.
 *
 *   framesInFlight  -- how many CPU frame slots are recorded ahead of the GPU
 *                      (typically 2). This sizes the per-frame command buffers,
 *                      fences, and "image available" semaphores. Going higher
 *                      adds memory and input lag with no throughput gain.
 *
 * Resource ownership maps onto these two counts:
 *
 *   Per swapchain image  --> presentSemaphore. The semaphore that
 *     vkQueuePresentKHR waits on must follow the image, because
 *     vkAcquireNextImageKHR can return images out of order (especially with
 *     MAILBOX): a per-slot binary semaphore would race itself.
 *
 *   Per in-flight slot   --> acquireSemaphore. Consumed by acquire and
 *     reused once the slot's previous frame completes on the GPU. Also: the
 *     command buffer, command pool, and timeline-value tracker that live in
 *     MinimalLatest::m_frameData are sized by framesInFlight, not imageCount.
 *
 * Two indices, both real, both needed:
 *   m_frameResourceIndex -- cycles [0, m_framesInFlight); selects the in-flight slot.
 *   m_frameImageIndex    -- whatever vkAcquireNextImageKHR gave us; selects an image.
 *
 * Naming: these two binary semaphores are named by the Vulkan call they
 * pair with -- acquireSemaphore is signaled by vkAcquireNextImageKHR, and
 * presentSemaphore is waited on by vkQueuePresentKHR. This makes every
 * call site self-documenting ("wait on acquire, signal for present") and
 * mirrors the two API calls directly.
 *
 * Older samples and the Khronos tutorial often name them "imageAvailable"
 * and "renderFinished" instead -- describing the state each semaphore
 * represents rather than the call it pairs with. Both schemes are valid;
 * if you're porting from that convention, the mapping is one-to-one:
 *   imageAvailable  <-> acquireSemaphore   (signaled by acquire)
 *   renderFinished  <-> presentSemaphore   (waited on by present)
-*/
class Swapchain
{
public:
  Swapchain() = default;
  ~Swapchain() { assert(m_swapChain == VK_NULL_HANDLE && "Missing deinit()"); }

  void        requestRebuild() { m_needRebuild = true; }
  bool        needRebuilding() const { return m_needRebuild; }
  VkImage     getImage() const { return m_nextImages[m_frameImageIndex].image; }
  VkImageView getImageView() const { return m_nextImages[m_frameImageIndex].imageView; }
  VkFormat    getImageFormat() const { return m_imageFormat; }

  // Number of swapchain images (presentation parallelism).
  uint32_t getImageCount() const { return m_imageCount; }

  // Number of CPU frame slots (= concurrent frames in flight).
  uint32_t getFramesInFlight() const { return m_framesInFlight; }

  // The current in-flight slot; cycles [0, framesInFlight). Use this to index
  // any per-frame CPU resource (command buffers, timeline values, etc.).
  uint32_t getFrameResourceIndex() const { return m_frameResourceIndex; }

  // acquireSemaphore is per in-flight slot (consumed by acquire).
  VkSemaphore getAcquireSemaphore() const { return m_inFlightSlots[m_frameResourceIndex].acquireSemaphore; }

  // presentSemaphore semaphore is per *image* (consumed by present, must follow
  // the image because acquire can return images out of order).
  VkSemaphore getPresentSemaphore() const { return m_nextImages[m_frameImageIndex].presentSemaphore; }

  // Initialize the swapchain with the provided context and surface, then we can create and re-create it
  void init(VkPhysicalDevice physicalDevice, VkDevice device, const QueueInfo& queue, VkSurfaceKHR surface, VkCommandPool cmdPool)
  {
    m_physicalDevice = physicalDevice;
    m_device         = device;
    m_queue          = queue;
    m_surface        = surface;
    m_cmdPool        = cmdPool;
  }

  // Destroy internal resources and reset its initial state
  void deinit()
  {
    deinitResources();
    *this = {};
  }

  /*--
   * Create the swapchain using the provided context, surface, and vSync option. The actual window size is returned.
   * Queries the GPU capabilities, selects the best surface format and present mode, and creates the swapchain accordingly.
  -*/
  VkExtent2D initResources(bool vSync = true)
  {
    VkExtent2D outWindowSize;

    // Query the physical device's capabilities for the given surface.
    const VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo2{.sType   = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
                                                       .surface = m_surface};
    VkSurfaceCapabilities2KHR             capabilities2{.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilities2KHR(m_physicalDevice, &surfaceInfo2, &capabilities2));

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormats2KHR(m_physicalDevice, &surfaceInfo2, &formatCount, nullptr);
    std::vector<VkSurfaceFormat2KHR> formats(formatCount, {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR});
    vkGetPhysicalDeviceSurfaceFormats2KHR(m_physicalDevice, &surfaceInfo2, &formatCount, formats.data());

    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, nullptr);
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(m_physicalDevice, m_surface, &presentModeCount, presentModes.data());

    // Choose the best available surface format and present mode
    const VkSurfaceFormat2KHR surfaceFormat2 = selectSwapSurfaceFormat(formats);
    const VkPresentModeKHR    presentMode    = selectSwapPresentMode(presentModes, vSync);
    // Set the window size according to the surface's current extent
    outWindowSize = capabilities2.surfaceCapabilities.currentExtent;

    // Pick a swapchain image count: prefer triple-buffering, but honour the
    // surface's [minImageCount, maxImageCount] bounds.
    uint32_t minImageCount       = capabilities2.surfaceCapabilities.minImageCount;  // Vulkan-defined minimum
    uint32_t preferredImageCount = std::max(kPreferredImageCount, minImageCount);

    // Handle the maxImageCount case where 0 means "no upper limit"
    uint32_t maxImageCount = (capabilities2.surfaceCapabilities.maxImageCount == 0) ? preferredImageCount :  // No upper limit, use preferred
                                 capabilities2.surfaceCapabilities.maxImageCount;

    // Clamp preferredImageCount to valid range [minImageCount, maxImageCount]
    m_imageCount = std::clamp(preferredImageCount, minImageCount, maxImageCount);

    // Store the chosen image format
    m_imageFormat = surfaceFormat2.surfaceFormat.format;

    // Create the swapchain itself
    const VkSwapchainCreateInfoKHR swapchainCreateInfo{
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = m_surface,
        .minImageCount    = m_imageCount,
        .imageFormat      = surfaceFormat2.surfaceFormat.format,
        .imageColorSpace  = surfaceFormat2.surfaceFormat.colorSpace,
        .imageExtent      = capabilities2.surfaceCapabilities.currentExtent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = capabilities2.surfaceCapabilities.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = presentMode,
        .clipped          = VK_TRUE,
    };
    VK_CHECK(vkCreateSwapchainKHR(m_device, &swapchainCreateInfo, nullptr, &m_swapChain));
    DBG_VK_NAME(m_swapChain);

    // Retrieve the swapchain images
    {
      uint32_t imageCount = 0;
      vkGetSwapchainImagesKHR(m_device, m_swapChain, &imageCount, nullptr);
      ASSERT(m_imageCount <= imageCount, "Wrong swapchain setup");
      m_imageCount = imageCount;  // Use the number of images the swapchain actually created
    }
    std::vector<VkImage> swapImages(m_imageCount);
    vkGetSwapchainImagesKHR(m_device, m_swapChain, &m_imageCount, swapImages.data());

    // Frames-in-flight: how many CPU frame slots run concurrently with the GPU.
    // Default of 2 is the canonical modern choice (one being recorded on the
    // CPU while the previous one executes on the GPU). Capped at imageCount
    // because we can never have more frames in flight than swapchain images.
    m_framesInFlight = std::min(kPreferredFramesInFlight, m_imageCount);

    // Per-image storage: VkImage, VkImageView, and the presentSemaphore
    // (binary semaphore that present waits on; must follow the image).
    m_nextImages.resize(m_imageCount);
    VkImageViewCreateInfo imageViewCreateInfo{
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = m_imageFormat,
        .components = {.r = VK_COMPONENT_SWIZZLE_IDENTITY, .g = VK_COMPONENT_SWIZZLE_IDENTITY, .b = VK_COMPONENT_SWIZZLE_IDENTITY, .a = VK_COMPONENT_SWIZZLE_IDENTITY},
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1},
    };
    const VkSemaphoreCreateInfo semaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for(uint32_t i = 0; i < m_imageCount; i++)
    {
      m_nextImages[i].image = swapImages[i];
      DBG_VK_NAME(m_nextImages[i].image);
      imageViewCreateInfo.image = m_nextImages[i].image;
      VK_CHECK(vkCreateImageView(m_device, &imageViewCreateInfo, nullptr, &m_nextImages[i].imageView));
      DBG_VK_NAME(m_nextImages[i].imageView);
      VK_CHECK(vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &m_nextImages[i].presentSemaphore));
      DBG_VK_NAME(m_nextImages[i].presentSemaphore);
    }

    // Per-in-flight-slot storage: acquireSemaphore (consumed by acquire).
    m_inFlightSlots.resize(m_framesInFlight);
    for(size_t i = 0; i < m_framesInFlight; ++i)
    {
      VK_CHECK(vkCreateSemaphore(m_device, &semaphoreCreateInfo, nullptr, &m_inFlightSlots[i].acquireSemaphore));
      DBG_VK_NAME(m_inFlightSlots[i].acquireSemaphore);
    }

    return outWindowSize;
  }

  /*--
   * Recreate the swapchain, typically after a window resize or when it becomes invalid.
   * This waits for all rendering to be finished before destroying the old swapchain and creating a new one.
  -*/
  VkExtent2D reinitResources(bool vSync = true)
  {
    // Wait for all frames to finish rendering before recreating the swapchain
    vkQueueWaitIdle(m_queue.queue);

    m_frameResourceIndex = 0;
    m_needRebuild        = false;
    deinitResources();
    return initResources(vSync);
  }

  /*--
   * Destroy the swapchain and its associated resources.
   * This function is also called when the swapchain needs to be recreated.
  -*/
  void deinitResources()
  {
    vkDestroySwapchainKHR(m_device, m_swapChain, nullptr);
    for(auto& slot : m_inFlightSlots)
    {
      vkDestroySemaphore(m_device, slot.acquireSemaphore, nullptr);
    }
    for(auto& image : m_nextImages)
    {
      vkDestroyImageView(m_device, image.imageView, nullptr);
      vkDestroySemaphore(m_device, image.presentSemaphore, nullptr);
    }
  }

  /*--
   * Prepares the command buffer for recording rendering commands.
   * This function handles synchronization with the previous frame and acquires the next image from the swapchain.
   * The command buffer is reset, ready for new rendering commands.
  -*/
  VkResult acquireNextImage(VkDevice device)
  {
    ASSERT(m_needRebuild == false, "Swapbuffer need to call reinitResources()");

    // Acquire the next image from the swapchain. The acquireSemaphore is per
    // in-flight slot (the slot's prior frame has finished, so the semaphore is
    // unsignaled and ready to be re-signaled by the swapchain).
    const VkSemaphore signalSem = m_inFlightSlots[m_frameResourceIndex].acquireSemaphore;
    const VkResult result = vkAcquireNextImageKHR(device, m_swapChain, std::numeric_limits<uint64_t>::max(), signalSem,
                                                  VK_NULL_HANDLE, &m_frameImageIndex);
#ifdef NVVK_SEMAPHORE_DEBUG
    LOGI("AcquireNextImage: \t frameRes=%u imageIndex=%u", m_frameResourceIndex, m_frameImageIndex);
#endif
    // Handle special case if the swapchain is out of date (e.g., window resize)
    if(result == VK_ERROR_OUT_OF_DATE_KHR)
    {
      m_needRebuild = true;  // Swapchain must be rebuilt on the next frame
    }
    else
    {
      ASSERT(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "Couldn't aquire swapchain image");
    }
    return result;
  }

  /*--
   * Presents the rendered image to the screen.
   * The semaphore ensures that the image is presented only after rendering is complete.
   * Advances to the next frame in the cycle.
  -*/
  void presentFrame(VkQueue queue)
  {
    // Present must wait on the presentSemaphore that follows the image (not the
    // in-flight slot), because vkAcquireNextImageKHR can return images out of
    // order.
    const VkSemaphore waitSem = m_nextImages[m_frameImageIndex].presentSemaphore;

    const VkPresentInfoKHR presentInfo{
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,                   // Wait for rendering to finish
        .pWaitSemaphores    = &waitSem,            // Per-image semaphore
        .swapchainCount     = 1,                   // Swapchain to present the image
        .pSwapchains        = &m_swapChain,        // Pointer to the swapchain
        .pImageIndices      = &m_frameImageIndex,  // Index of the image to present
    };

    // Present the image and handle potential resizing issues
    const VkResult result = vkQueuePresentKHR(queue, &presentInfo);
#ifdef NVVK_SEMAPHORE_DEBUG
    LOGI("PresentFrame: \t\t slot=%u imageIndex=%u", m_frameResourceIndex, m_frameImageIndex);
#endif
    // If the swapchain is out of date (e.g., window resized), it needs to be rebuilt
    if(result == VK_ERROR_OUT_OF_DATE_KHR)
    {
      m_needRebuild = true;
    }
    else
    {
      ASSERT(result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR, "Couldn't present swapchain image");
    }

    // Advance to the next CPU in-flight slot (NOT the next image -- images are
    // chosen by the presentation engine).
    m_frameResourceIndex = (m_frameResourceIndex + 1) % m_framesInFlight;
  }

private:
  /*-- Per-swapchain-image resources -------------------------------------------
   * One entry per image returned by vkGetSwapchainImagesKHR. The
   * presentSemaphore lives here (not on the in-flight slot) because
   * vkQueuePresentKHR consumes it for a specific image, and the presentation
   * engine may hand images back out of order.
  -*/
  struct SwapchainImage
  {
    VkImage     image{};             // Swapchain image (owned by the swapchain)
    VkImageView imageView{};         // 2D view of the image
    VkSemaphore presentSemaphore{};  // Binary semaphore: signaled when rendering done, waited on by present
  };

  /*-- Per-in-flight-slot resources --------------------------------------------
   * One entry per "frame in flight" -- typically 2, regardless of the image
   * count. Holds resources tied to the CPU's submission cadence rather than
   * the displayed image. The acquireSemaphore is recycled here.
  -*/
  struct InFlightSlot
  {
    VkSemaphore acquireSemaphore{};  // Binary semaphore signaled by vkAcquireNextImageKHR
  };

  // We choose the format that is the most common, and that is supported by* the physical device.
  VkSurfaceFormat2KHR selectSwapSurfaceFormat(const std::vector<VkSurfaceFormat2KHR>& availableFormats) const
  {
    // If there's only one available format and it's undefined, return a default format.
    if(availableFormats.size() == 1 && availableFormats[0].surfaceFormat.format == VK_FORMAT_UNDEFINED)
    {
      VkSurfaceFormat2KHR result{.sType         = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR,
                                 .surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
      return result;
    }

    const auto preferredFormats = std::to_array<VkSurfaceFormat2KHR>({
        {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, .surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}},
        {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR, .surfaceFormat = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}},
    });

    // Check available formats against the preferred formats.
    for(const auto& preferredFormat : preferredFormats)
    {
      for(const auto& availableFormat : availableFormats)
      {
        if(availableFormat.surfaceFormat.format == preferredFormat.surfaceFormat.format
           && availableFormat.surfaceFormat.colorSpace == preferredFormat.surfaceFormat.colorSpace)
        {
          return availableFormat;  // Return the first matching preferred format.
        }
      }
    }

    // If none of the preferred formats are available, return the first available format.
    return availableFormats[0];
  }

  /*--
   * The present mode is chosen based on the vSync option
   * The FIFO mode is the most common, and is used when vSync is enabled.
   * The MAILBOX mode is used when vSync is disabled, and is the best mode for triple buffering.
   * The IMMEDIATE mode is used when vSync is disabled, and is the best mode for low latency.
  -*/
  VkPresentModeKHR selectSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool vSync = true)
  {
    if(vSync)
    {
      return VK_PRESENT_MODE_FIFO_KHR;
    }

    bool mailboxSupported = false, immediateSupported = false;

    for(VkPresentModeKHR mode : availablePresentModes)
    {
      if(mode == VK_PRESENT_MODE_MAILBOX_KHR)
        mailboxSupported = true;
      if(mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
        immediateSupported = true;
    }

    if(mailboxSupported)
    {
      return VK_PRESENT_MODE_MAILBOX_KHR;
    }

    if(immediateSupported)
    {
      return VK_PRESENT_MODE_IMMEDIATE_KHR;  // Best mode for low latency
    }

    return VK_PRESENT_MODE_FIFO_KHR;  // Fallback to FIFO if neither MAILBOX nor IMMEDIATE is available
  }

private:
  VkPhysicalDevice m_physicalDevice{};  // The physical device (GPU)
  VkDevice         m_device{};          // The logical device (interface to the physical device)
  QueueInfo        m_queue{};           // The queue used to submit command buffers to the GPU
  VkSwapchainKHR   m_swapChain{};       // The swapchain
  VkFormat         m_imageFormat{};     // The format of the swapchain images
  VkSurfaceKHR     m_surface{};         // The surface to present images to
  VkCommandPool    m_cmdPool{};         // The command pool for the swapchain

  std::vector<SwapchainImage> m_nextImages;              // Sized by m_imageCount
  std::vector<InFlightSlot>   m_inFlightSlots;           // Sized by m_framesInFlight
  uint32_t                    m_frameResourceIndex = 0;  // Cycles [0, m_framesInFlight)
  uint32_t                    m_frameImageIndex    = 0;  // Whatever the swapchain returns
  bool                        m_needRebuild        = false;

  // Default targets, clamped at runtime in initResources() to device limits.
  static constexpr uint32_t kPreferredImageCount     = 3;                         // Triple buffering for presentation
  static constexpr uint32_t kPreferredFramesInFlight = 2;                         // CPU-side double buffering
  uint32_t                  m_imageCount             = kPreferredImageCount;      // From vkGetSwapchainImagesKHR
  uint32_t                  m_framesInFlight         = kPreferredFramesInFlight;  // <= m_imageCount
};

/***************************************************************
 ***************************************************************
 ********************    FRAME PACING      ********************
 ***************************************************************
 ***************************************************************/

// This is a simple frame pacer to limit the frame rate to the target FPS.
// This significantly helps with latency (from interaction to seeing the update
// on screen) when VSync is on.
// Try this: Enable VSync and rapidly change the clear color using your mouse.
//           Now do the same without this frame pacer; you'll see that without
//           the pacer, the clear color lags behind the mouse by several frames.
//
// The core idea here is that when VSync is on, the screen only displays one
// image per VSync. So if we rendered faster, we'd either queue up too many
// frames in the swapchain (VK_PRESENT_MODE_FIFO) or use too much power by
// rendering frames that are never displayed (some VK_PRESENT_MODE_MAILBOX
// implementations). Since the compositor consumes only one image per VSync,
// we should render only at most one image per VSync.
class FramePacer
{
public:
  FramePacer()  = default;
  ~FramePacer() = default;

  // Frame rate limiting to monitor refresh rate
  void paceFrame(double targetFPS = 60.0)
  {
    const auto targetFrameTime = std::chrono::duration<double>(1.0 / targetFPS);

    // Pacing the CPU by enforcing at least `refreshInterval` seconds between
    // frames is all we need! If the GPU is fast things are OK; if the GPU is
    // slow then vkWaitSemaphores will take more time in the frame, which
    // will be counted in the CPU time.
    const auto currentTime   = std::chrono::high_resolution_clock::now();
    const auto frameDuration = currentTime - m_lastFrameTime;
    auto       sleepTime     = targetFrameTime - frameDuration;
#ifdef _WIN32
    // On Windows, we know that 1ms is just about the right time to subtract;
    // it's just under the average amount that Windows adds to the sleep call.
    // On Linux the timers are accurate enough that we don't need this.
    sleepTime -= std::chrono::duration<double>(std::chrono::milliseconds(1));
#endif
    if(sleepTime > std::chrono::duration<double>(0))
    {
#ifdef _WIN32
      // On Windows, the default timer might quantize to 15.625 ms; see
      // https://randomascii.wordpress.com/2020/10/04/windows-timer-resolution-the-great-rule-change/ .
      // We use timeBeginPeriod to temporarily increase the resolution to 1 ms.
      timeBeginPeriod(1);
#endif
      std::this_thread::sleep_for(sleepTime);
#ifdef _WIN32
      timeEndPeriod(1);
#endif
    }

    m_lastFrameTime = std::chrono::high_resolution_clock::now();
  }

private:
  std::chrono::high_resolution_clock::time_point m_lastFrameTime;
};

// Helper to return the minimum refresh rate of all monitors.
inline double getMonitorsMinRefreshRate()
{
  // We need our target frame rate. We get this once per frame in case the
  // user changes their monitor's frame rate.
  // Ideally we'd get the exact composition rate for the current swapchain;
  // VK_EXT_present_timing will hopefully give us that when it's released.
  // Currently we use GLFW; this means we don't need anything
  // platform-specific, but means we only get an integer frame rate,
  // rounded down, across monitors. We take the minimum to avoid building up
  // frame latency.
  double refreshRate = std::numeric_limits<double>::infinity();
  {
    int           numMonitors = 0;
    GLFWmonitor** monitors    = glfwGetMonitors(&numMonitors);
    for(int i = 0; i < numMonitors; i++)
    {
      const GLFWvidmode* videoMode = glfwGetVideoMode(monitors[i]);
      if(videoMode)
      {
        refreshRate = std::min(refreshRate, static_cast<double>(videoMode->refreshRate));
      }
    }
  }
  // If we have no information about the frame rate or an impossible value,
  // use a default.
  if(std::isinf(refreshRate) || refreshRate <= 0.0)
  {
    refreshRate = 60.0;
  }

  return refreshRate;
}

/***************************************************************
 ***************************************************************
 *****************    RESOURCE ALLOCATION    ******************
 ***************************************************************
 ***************************************************************/

/*--
 * Vulkan Memory Allocator (VMA) is a library that helps to manage memory in Vulkan.
 * This should be used to manage the memory of the resources instead of using the Vulkan API directly.
-*/
class ResourceAllocator
{
public:
  ResourceAllocator() = default;
  ~ResourceAllocator() { assert(m_allocator == nullptr && "Missing deinit()"); }
  operator VmaAllocator() const { return m_allocator; }

  // Initialization of VMA allocator.
  void init(VmaAllocatorCreateInfo allocatorInfo)
  {
    // VK_EXT_memory_priority / VMA_ALLOCATOR_CREATE_EXT_MEMORY_PRIORITY_BIT
    // are deliberately not enabled here; this sample's allocation pattern is
    // small and static, so the extra hint is not worth the extra extension.
    // Revisit if heavier streaming workloads are added.

    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;  // allow querying for the GPU address of a buffer
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE4_BIT;
    allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_KHR_MAINTENANCE5_BIT;  // allow using VkBufferUsageFlags2CreateInfo

    m_device = allocatorInfo.device;
    // Because we use VMA_DYNAMIC_VULKAN_FUNCTIONS
    const VmaVulkanFunctions functions = {
        .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr   = vkGetDeviceProcAddr,
    };
    allocatorInfo.pVulkanFunctions = &functions;
    VK_CHECK(vmaCreateAllocator(&allocatorInfo, &m_allocator));
  }

  // De-initialization of VMA allocator.
  void deinit()
  {
    if(!m_stagingBuffers.empty())
      LOGW("Warning: Staging buffers were not freed before destroying the allocator");
    freeStagingBuffers();
    vmaDestroyAllocator(m_allocator);
    *this = {};
  }

  /*--
   * Create a buffer.
   *
   * Buffer device address (BDA) is *opt-in*: pass
   * VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT in `usage` if you need
   * Buffer.address to be populated. Otherwise the address stays 0 and no
   * vkGetBufferDeviceAddress call is made -- which is the correct mental model
   * (BDA enables some implementation overhead and isn't always wanted, e.g.
   * staging buffers).
   *
   * Modern VMA uses VMA_MEMORY_USAGE_AUTO* + HOST_ACCESS flags; the legacy
   * VMA_MEMORY_USAGE_GPU_ONLY / CPU_TO_GPU / GPU_TO_CPU enums are deprecated.
   *
   * Examples:
   *   UBO, CPU writes / GPU reads:
   *       VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT
   *       + VMA_MEMORY_USAGE_AUTO
   *       + VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
   *
   *   SSBO, GPU-only (via staging):
   *       VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT
   *       [+ VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT for BDA]
   *       + VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE
   *
   *   SSBO, per-frame CPU writes:
   *       VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT
   *       + VMA_MEMORY_USAGE_AUTO
   *       + VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
   *
   *   Readback:
   *       VK_BUFFER_USAGE_2_TRANSFER_DST_BIT
   *       + VMA_MEMORY_USAGE_AUTO
   *       + VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT
   *
   * Allocation flags (combine as needed):
   *   VMA_ALLOCATION_CREATE_MAPPED_BIT                         -- persistent mapping
   *   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT   -- CPU streaming writes
   *   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT             -- CPU readback
   *
   * Note: the best-practices validation layer may warn about small dedicated
   * allocations; that is expected for a sample of this scale and not worth
   * working around. Production engines should configure a VmaPool with a
   * larger blockSize to sub-allocate small resources together.
  -*/
  Buffer createBuffer(VkDeviceSize             size,
                      VkBufferUsageFlags2      usage,
                      VmaMemoryUsage           memoryUsage  = VMA_MEMORY_USAGE_AUTO,
                      VmaAllocationCreateFlags flags        = {},
                      VkDeviceSize             minAlignment = {})
  {
    const bool wantsAddress = (usage & VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT) != 0;

    // VkBufferUsageFlags2CreateInfo (Maintenance5) replaces the legacy 32-bit
    // usage field with a 64-bit one. The CreateInfo's .usage stays 0; the real
    // usage flags ride in the chained struct.
    const VkBufferUsageFlags2CreateInfo bufferUsageFlags2CreateInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,
        .usage = usage,
    };

    const VkBufferCreateInfo bufferInfo{
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext       = &bufferUsageFlags2CreateInfo,
        .size        = size,
        .usage       = 0,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,  // Only one queue family will access it
    };

    // Note: we deliberately do NOT auto-promote large allocations to dedicated
    // memory. VMA's own heuristics (and the VMA_MEMORY_USAGE_AUTO* policies)
    // are far better tuned than a one-size-fits-all threshold. If you need a
    // dedicated allocation for a specific buffer, pass
    // VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT in `flags`.
    VmaAllocationCreateInfo allocInfo = {.flags = flags, .usage = memoryUsage};

    // Create the buffer
    Buffer            resultBuffer;
    VmaAllocationInfo allocInfoOut{};
    VK_CHECK(vmaCreateBufferWithAlignment(m_allocator, &bufferInfo, &allocInfo, minAlignment, &resultBuffer.buffer,
                                          &resultBuffer.allocation, &allocInfoOut));

    // Query the GPU address only if the caller asked for one (BDA opt-in).
    if(wantsAddress)
    {
      const VkBufferDeviceAddressInfo info = {.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                              .buffer = resultBuffer.buffer};
      resultBuffer.address                 = vkGetBufferDeviceAddress(m_device, &info);
    }

    {  // Find leaks
      static uint32_t counter = 0U;
      if(m_leakID == counter)
      {
#if defined(_MSVC_LANG)
        __debugbreak();
#endif
      }
      std::string allocID = std::string("allocID: ") + std::to_string(counter++);
      vmaSetAllocationName(m_allocator, resultBuffer.allocation, allocID.c_str());
    }

    return resultBuffer;
  }

  //*-- Destroy a buffer -*/
  void destroyBuffer(Buffer buffer) { vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation); }

  /*--
   * Create a staging buffer, copy data into it, and track it.
   * This method accepts data, handles the mapping, copying, and unmapping
   * automatically.
  -*/
  template <typename T>
  Buffer createStagingBuffer(const std::span<T>& vectorData)
  {
    const VkDeviceSize bufferSize = sizeof(T) * vectorData.size();

    // Create a staging buffer (host-visible, CPU-writes-then-GPU-reads).
    Buffer stagingBuffer = createBuffer(bufferSize, VK_BUFFER_USAGE_2_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                                        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT);

    // Track the staging buffer for later cleanup
    m_stagingBuffers.push_back(stagingBuffer);

    // Map and copy data to the staging buffer
    void* data = nullptr;
    vmaMapMemory(m_allocator, stagingBuffer.allocation, &data);
    memcpy(data, vectorData.data(), (size_t)bufferSize);
    vmaUnmapMemory(m_allocator, stagingBuffer.allocation);
    return stagingBuffer;
  }

  /*--
   * Create a buffer (GPU only) with data, this is done using a staging buffer
   * The staging buffer is a buffer that is used to transfer data from the CPU
   * to the GPU.
   * and cannot be freed until the data is transferred. So the command buffer
   * must be submitted, then
   * the staging buffer can be cleared using the freeStagingBuffers function.
  -*/
  template <typename T>
  Buffer createBufferAndUploadData(VkCommandBuffer          cmd,
                                   const std::span<T>&      vectorData,
                                   VkBufferUsageFlags2      usageFlags,
                                   VmaAllocationCreateFlags flags        = {},
                                   VkDeviceSize             minAlignment = {})
  {
    // Create staging buffer and upload data
    Buffer stagingBuffer = createStagingBuffer(vectorData);

    // Create the final buffer in GPU memory
    const VkDeviceSize bufferSize = sizeof(T) * vectorData.size();
    Buffer             buffer     = createBuffer(bufferSize, usageFlags | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT,
                                                 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, flags, minAlignment);

    const std::array<VkBufferCopy, 1> copyRegion{{{.size = bufferSize}}};
    vkCmdCopyBuffer(cmd, stagingBuffer.buffer, buffer.buffer, uint32_t(copyRegion.size()), copyRegion.data());

    return buffer;
  }

  /*--
   * Create an image in GPU memory. This does not adding data to the image.
   * This is only creating the image in GPU memory.
   * See createImageAndUploadData for creating an image and uploading data.
  -*/
  Image createImage(const VkImageCreateInfo& imageInfo)
  {
    const VmaAllocationCreateInfo createInfo{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};

    Image             image;
    VmaAllocationInfo allocInfo{};
    VK_CHECK(vmaCreateImage(m_allocator, &imageInfo, &createInfo, &image.image, &image.allocation, &allocInfo));
    return image;
  }

  /*-- Destroy image --*/
  void destroyImage(Image& image) { vmaDestroyImage(m_allocator, image.image, image.allocation); }

  void destroyImageResource(ImageResource& imageResource) { destroyImage(imageResource); }

  /*-- Create an image and upload data using a staging buffer --*/
  template <typename T>
  ImageResource createImageAndUploadData(VkCommandBuffer cmd, const std::span<T>& vectorData, const VkImageCreateInfo& _imageInfo, VkImageLayout finalLayout)
  {
    // Create staging buffer and upload data
    Buffer stagingBuffer = createStagingBuffer(vectorData);

    // Create image in GPU memory
    VkImageCreateInfo imageInfo = _imageInfo;
    imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;  // We will copy data to this image
    Image image = createImage(imageInfo);

    // Transition image layout for copying data
    cmdInitImageLayout(cmd, image.image);

    // Copy buffer data to the image
    const std::array<VkBufferImageCopy, 1> copyRegion{
        {{.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1}, .imageExtent = imageInfo.extent}}};

    vkCmdCopyBufferToImage(cmd, stagingBuffer.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, uint32_t(copyRegion.size()),
                           copyRegion.data());

    ImageResource resultImage(image);
    resultImage.layout = finalLayout;
    return resultImage;
  }

  /*--
   * The staging buffers are buffers that are used to transfer data from the CPU to the GPU.
   * They cannot be freed until the data is transferred. So the command buffer must be completed, then the staging buffer can be cleared.
  -*/
  void freeStagingBuffers()
  {
    for(const auto& buffer : m_stagingBuffers)
    {
      destroyBuffer(buffer);
    }
    m_stagingBuffers.clear();
  }

  /*--
   * Debug aid: trip the debugger the moment the N-th allocation happens.
   *
   * Each allocation made through this allocator is named "allocID: N" via
   * vmaSetAllocationName. On shutdown, VMA prints the leaked allocations using
   * VMA_LEAK_LOG_FORMAT (see top of file). Pick one of the printed IDs, call
   * setLeakID(N) at startup, and the next run will __debugbreak() inside
   * createBuffer() at the moment that allocation is made -- giving you the
   * exact call stack of the leak. No effect in release builds (no breakpoint).
  -*/
  void setLeakID(uint32_t id) { m_leakID = id; }

private:
  VmaAllocator        m_allocator{};
  VkDevice            m_device{};
  std::vector<Buffer> m_stagingBuffers;
  uint32_t            m_leakID = ~0U;
};


/***************************************************************
 ***************************************************************
 ********************      SAMPLERS        ********************
 ***************************************************************
 ***************************************************************/

/*--
 * SamplerPool: deduplicated sampler management.
 *
 * Vulkan limits the number of live samplers per device, so reusing the same
 * VkSampler for identical VkSamplerCreateInfo is essential at scale. This
 * class exposes TWO independent paths, each with its own backing map:
 *
 *  Legacy / VkSampler path:
 *    acquireSampler()        -> VkSampler (deduplicated by VkSamplerCreateInfo)
 *    releaseSampler()
 *  Used by callers that need an actual sampler handle, e.g. ImGui textures
 *  and the offscreen RenderTarget display sampler.
 *
 *  Descriptor-heap path:
 *    acquireSamplerDescriptor()  -> uint32_t heap slot index (ref-counted)
 *    releaseSamplerDescriptor()
 *  Used when writing into the VK_EXT_descriptor_heap sampler heap. The
 *  driver creates the underlying sampler internally inside
 *  vkWriteSamplerDescriptorsEXT; this class only allocates a stable slot
 *  index that the shader uses to address it.
 *
 * The two paths are intentionally separate (different storage, different
 * lifetimes) because their consumers are different: ImGui doesn't know about
 * the heap, and the heap doesn't expose VkSampler handles.
-*/
class SamplerPool
{
public:
  SamplerPool() = default;
  ~SamplerPool() { assert(m_device == VK_NULL_HANDLE && "Missing deinit()"); }
  // Initialize the sampler pool with the device reference, then we can later acquire samplers
  void init(VkDevice device) { m_device = device; }
  // Destroy internal resources and reset its initial state
  void deinit()
  {
    for(const auto& entry : m_samplerMap)
    {
      vkDestroySampler(m_device, entry.second, nullptr);
    }
    m_samplerMap.clear();
    *this = {};
  }
  // Get or create VkSampler based on VkSamplerCreateInfo
  VkSampler acquireSampler(const VkSamplerCreateInfo& createInfo)
  {
    if(auto it = m_samplerMap.find(createInfo); it != m_samplerMap.end())
    {
      // If found, return existing sampler
      return it->second;
    }

    // Otherwise, create a new sampler
    VkSampler newSampler     = createSampler(createInfo);
    m_samplerMap[createInfo] = newSampler;
    return newSampler;
  }

  void releaseSampler(VkSampler sampler)
  {
    for(auto it = m_samplerMap.begin(); it != m_samplerMap.end();)
    {
      if(it->second == sampler)
      {
        vkDestroySampler(m_device, it->second, nullptr);
        it = m_samplerMap.erase(it);
      }
      else
      {
        ++it;
      }
    }
  }

  // Descriptor heap variant: returns a deduplicated heap index for a given VkSamplerCreateInfo.
  // Identical create-infos share the same index (ref-counted).
  uint32_t acquireSamplerDescriptor(const VkSamplerCreateInfo& createInfo)
  {
    if(auto it = m_descriptorMap.find(createInfo); it != m_descriptorMap.end())
    {
      ++it->second.refCount;
      return it->second.index;
    }

    uint32_t newIndex{};
    if(!m_freeDescriptorIndices.empty())
    {
      newIndex = m_freeDescriptorIndices.back();
      m_freeDescriptorIndices.pop_back();
    }
    else
    {
      newIndex = m_nextDescriptorIndex++;
    }

    m_descriptorMap[createInfo]      = {newIndex, 1};
    m_descriptorReverseMap[newIndex] = createInfo;
    return newIndex;
  }

  // Release a previously acquired descriptor heap index.
  // When the last reference is released, the index is recycled for future use.
  void releaseSamplerDescriptor(uint32_t index)
  {
    auto revIt = m_descriptorReverseMap.find(index);
    ASSERT(revIt != m_descriptorReverseMap.end(), "releaseSamplerDescriptor: unknown index");

    auto fwdIt = m_descriptorMap.find(revIt->second);
    ASSERT(fwdIt != m_descriptorMap.end(), "releaseSamplerDescriptor: inconsistent state");

    if(--fwdIt->second.refCount == 0)
    {
      m_descriptorMap.erase(fwdIt);
      m_descriptorReverseMap.erase(revIt);
      m_freeDescriptorIndices.push_back(index);
    }
  }

private:
  VkDevice m_device{};

  struct SamplerCreateInfoHash
  {
    std::size_t operator()(const VkSamplerCreateInfo& info) const
    {
      std::size_t seed{0};
      seed = hashCombine(seed, info.magFilter);
      seed = hashCombine(seed, info.minFilter);
      seed = hashCombine(seed, info.mipmapMode);
      seed = hashCombine(seed, info.addressModeU);
      seed = hashCombine(seed, info.addressModeV);
      seed = hashCombine(seed, info.addressModeW);
      seed = hashCombine(seed, info.mipLodBias);
      seed = hashCombine(seed, info.anisotropyEnable);
      seed = hashCombine(seed, info.maxAnisotropy);
      seed = hashCombine(seed, info.compareEnable);
      seed = hashCombine(seed, info.compareOp);
      seed = hashCombine(seed, info.minLod);
      seed = hashCombine(seed, info.maxLod);
      seed = hashCombine(seed, info.borderColor);
      seed = hashCombine(seed, info.unnormalizedCoordinates);

      return seed;
    }
  };

  struct SamplerCreateInfoEqual
  {
    bool operator()(const VkSamplerCreateInfo& lhs, const VkSamplerCreateInfo& rhs) const
    {
      return std::memcmp(&lhs, &rhs, sizeof(VkSamplerCreateInfo)) == 0;
    }
  };

  // Stores unique samplers with their corresponding VkSamplerCreateInfo
  std::unordered_map<VkSamplerCreateInfo, VkSampler, SamplerCreateInfoHash, SamplerCreateInfoEqual> m_samplerMap;

  // --- Descriptor heap index management ---
  struct DescriptorEntry
  {
    uint32_t index{};
    uint32_t refCount{};
  };
  std::unordered_map<VkSamplerCreateInfo, DescriptorEntry, SamplerCreateInfoHash, SamplerCreateInfoEqual> m_descriptorMap;
  std::unordered_map<uint32_t, VkSamplerCreateInfo> m_descriptorReverseMap;
  std::vector<uint32_t>                             m_freeDescriptorIndices;
  uint32_t                                          m_nextDescriptorIndex = 0;

  // Internal function to create a new VkSampler
  const VkSampler createSampler(const VkSamplerCreateInfo& createInfo) const
  {
    ASSERT(m_device, "Initialization was missing");
    VkSampler sampler{};
    VK_CHECK(vkCreateSampler(m_device, &createInfo, nullptr, &sampler));
    return sampler;
  }
};


/***************************************************************
 ***************************************************************
 *****************     RENDER TARGET      ******************
 ***************************************************************
 ***************************************************************/

/*--
 * RenderTarget creation info
-*/
struct RenderTargetCreateInfo
{
  VkDevice                  device{};  // Vulkan Device
  utils::ResourceAllocator* alloc{};   // Allocator for the images
  VkExtent2D                size{};    // Width and height of the buffers
  std::vector<VkFormat>     color;     // Array of formats for each color attachment (one render target per format)
  VkFormat              depth{VK_FORMAT_UNDEFINED};  // Format of the depth buffer (VK_FORMAT_UNDEFINED for no depth)
  VkSampleCountFlagBits sampleCount{VK_SAMPLE_COUNT_1_BIT};  // MSAA sample count (default: no MSAA)
};

/***************************************************************
 ***************************************************************
 ********************    RenderTarget      ********************
 ***************************************************************
 ***************************************************************/

/*--
 * RenderTarget - Offscreen color (one or more) + optional depth attachment.
 *
 * This is *not* a deferred-rendering G-buffer (no albedo/normal/specular split);
 * it's a generic offscreen target you render the scene into. The sample displays
 * it via ImGui::Image, but the class itself is ImGui-agnostic: it exposes a
 * dedicated alpha=1-swizzled view (getUiImageView) that callers can hand to
 * utils::ImTexture, but it does not touch ImGui state itself.
 *
 * Despite the multi-color-attachment support being present in the API surface
 * here, the sample only uses a single color + depth. This class also supports:
 * - Multiple color attachments with configurable formats (unused here)
 * - Optional depth buffer
 * - MSAA sample count (default: 1)
 * - Automatic resource cleanup
 *
 * The images are created with usage = COLOR_ATTACHMENT | SAMPLED | STORAGE |
 * TRANSFER_SRC | TRANSFER_DST so they can be used as render target, sampled
 * texture, compute storage image, or copy source/destination.
-*/
class RenderTarget
{
public:
  RenderTarget() = default;
  ~RenderTarget() { assert(m_createInfo.device == VK_NULL_HANDLE && "Missing deinit()"); }

  /*--
   * Initialize the RenderTarget with the specified configuration.
  -*/
  void init(VkCommandBuffer cmd, const RenderTargetCreateInfo& createInfo)
  {
    ASSERT(m_createInfo.color.empty(), "Missing deinit()");  // The buffer must be cleared before creating a new one
    m_createInfo = createInfo;                               // Copy the creation info
    create(cmd);
  }

  // Destroy internal resources and reset its initial state
  void deinit()
  {
    destroy();
    *this = {};
  }

  void update(VkCommandBuffer cmd, VkExtent2D newSize)
  {
    if(newSize.width == m_createInfo.size.width && newSize.height == m_createInfo.size.height)
      return;

    destroy();
    m_createInfo.size = newSize;
    create(cmd);
  }


  //--- Getters for the RenderTarget resources --------------------
  VkExtent2D            getSize() const { return m_createInfo.size; }
  VkImage               getColorImage(uint32_t i = 0) const { return m_res.colorImages[i].image; }
  VkImage               getDepthImage() const { return m_res.depthImage.image; }
  VkImageView           getColorImageView(uint32_t i = 0) const { return m_res.imageViews[i]; }
  VkImageView           getUiImageView(uint32_t i = 0) const { return m_res.uiImageViews[i]; }
  VkImageView           getDepthImageView() const { return m_res.depthView; }
  VkFormat              getColorFormat(uint32_t i = 0) const { return m_createInfo.color[i]; }
  VkFormat              getDepthFormat() const { return m_createInfo.depth; }
  VkSampleCountFlagBits getSampleCount() const { return m_createInfo.sampleCount; }
  float getAspectRatio() const { return float(m_createInfo.size.width) / float(m_createInfo.size.height); }

private:
  /*--
   * Create the RenderTarget with the specified configuration
   *
   * Each color buffer is created with:
   * - Color attachment usage     : For rendering
   * - Sampled bit                : For sampling in shaders
   * - Storage bit                : For compute shader access
   * - Transfer dst bit           : For clearing/copying
   * 
   * The depth buffer is created with:
   * - Depth/Stencil attachment   : For depth testing
   * - Sampled bit                : For sampling in shaders
   *
   * All images are transitioned to GENERAL layout and cleared to black.
  -*/
  void create(VkCommandBuffer cmd)
  {
    DebugUtil&          dutil = DebugUtil::getInstance();
    const VkImageLayout layout{VK_IMAGE_LAYOUT_GENERAL};

    const auto numColor = static_cast<uint32_t>(m_createInfo.color.size());

    m_res.colorImages.resize(numColor);
    m_res.imageViews.resize(numColor);
    m_res.uiImageViews.resize(numColor);

    for(uint32_t c = 0; c < numColor; c++)
    {
      {  // Color image
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        const VkImageCreateInfo info = {
            .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType   = VK_IMAGE_TYPE_2D,
            .format      = m_createInfo.color[c],
            .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
            .mipLevels   = 1,
            .arrayLayers = 1,
            .samples     = m_createInfo.sampleCount,
            .usage       = usage,
        };
        m_res.colorImages[c] = m_createInfo.alloc->createImage(info);
        dutil.setObjectName(m_res.colorImages[c].image, "RT-Color" + std::to_string(c));
      }
      {  // Image color view
        VkImageViewCreateInfo info = {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = m_res.colorImages[c].image,
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = m_createInfo.color[c],
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
        };

        // Normal view
        VK_CHECK(vkCreateImageView(m_createInfo.device, &info, nullptr, &m_res.imageViews[c]));
        dutil.setObjectName(m_res.imageViews[c], "RT-View" + std::to_string(c));

        // UI Image color view
        info.components.a = VK_COMPONENT_SWIZZLE_ONE;  // Forcing the VIEW to have a 1 in the alpha channel
        VK_CHECK(vkCreateImageView(m_createInfo.device, &info, nullptr, &m_res.uiImageViews[c]));
        dutil.setObjectName(m_res.uiImageViews[c], "UI RT-View" + std::to_string(c));
      }
    }

    if(m_createInfo.depth != VK_FORMAT_UNDEFINED)
    {  // Depth buffer
      const VkImageCreateInfo createInfo = {
          .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
          .imageType   = VK_IMAGE_TYPE_2D,
          .format      = m_createInfo.depth,
          .extent      = {m_createInfo.size.width, m_createInfo.size.height, 1},
          .mipLevels   = 1,
          .arrayLayers = 1,
          .samples     = m_createInfo.sampleCount,
          .usage       = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
      };
      m_res.depthImage = m_createInfo.alloc->createImage(createInfo);
      dutil.setObjectName(m_res.depthImage.image, "RT-Depth");

      // Image depth view
      const VkImageViewCreateInfo viewInfo = {
          .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image            = m_res.depthImage.image,
          .viewType         = VK_IMAGE_VIEW_TYPE_2D,
          .format           = m_createInfo.depth,
          .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT, .levelCount = 1, .layerCount = 1},
      };
      VK_CHECK(vkCreateImageView(m_createInfo.device, &viewInfo, nullptr, &m_res.depthView));
      dutil.setObjectName(m_res.depthView, "RT-Depth");
    }

    {  // Change color image layout
      for(uint32_t c = 0; c < numColor; c++)
      {
        cmdInitImageLayout(cmd, m_res.colorImages[c].image);
        // Clear to avoid garbage data
        const VkClearColorValue                      clearValue = {{0.F, 0.F, 0.F, 0.F}};
        const std::array<VkImageSubresourceRange, 1> range      = {
            {{.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1}}};
        vkCmdClearColorImage(cmd, m_res.colorImages[c].image, layout, &clearValue, uint32_t(range.size()), range.data());
      }

      // Change depth image layout
      if(m_createInfo.depth != VK_FORMAT_UNDEFINED)
      {
        cmdInitImageLayout(cmd, m_res.depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);
      }
    }
  }

  /*--
   * Clean up all Vulkan resources
   * 
   * This must be called before destroying the RenderTarget or when
   * recreating with different parameters
  -*/
  void destroy()
  {
    for(utils::Image bc : m_res.colorImages)
    {
      m_createInfo.alloc->destroyImage(bc);
    }

    if(m_res.depthImage.image != VK_NULL_HANDLE)
    {
      m_createInfo.alloc->destroyImage(m_res.depthImage);
    }

    vkDestroyImageView(m_createInfo.device, m_res.depthView, nullptr);

    for(const VkImageView& view : m_res.imageViews)
    {
      vkDestroyImageView(m_createInfo.device, view, nullptr);
    }
    for(const VkImageView& view : m_res.uiImageViews)
    {
      vkDestroyImageView(m_createInfo.device, view, nullptr);
    }
  }


  /*--
   * Resources holds all Vulkan objects for the RenderTarget
   * This separation makes it easier to cleanup and recreate resources
  -*/
  struct Resources
  {
    std::vector<utils::Image> colorImages;   // Color attachments
    utils::Image              depthImage{};  // Optional depth attachment
    std::vector<VkImageView>  imageViews;    // Normal views for the color attachments
    std::vector<VkImageView>  uiImageViews;  // Special views for ImGui (alpha=1)
    VkImageView               depthView{};   // View for the depth attachment
  };

  Resources              m_res;           // All Vulkan resources
  RenderTargetCreateInfo m_createInfo{};  // Configuration
};


//---------------------------------------------------------------------------
// Wrapper around an ImGui (Vulkan backend) texture descriptor set.
//
// Internally calls ImGui_ImplVulkan_AddTexture / RemoveTexture, allocating
// from whatever descriptor pool the ImGui Vulkan backend was initialised with
// (typically m_uiDescriptorPool).
//
// Lifecycle (matches nvpro_core2 convention: explicit init/deinit, no RAII):
//   * Default-construct freely (no Vulkan calls). Safe before ImGui init.
//   * init(view, layout)  -- allocate the descriptor. Requires the helper to
//                            be empty (asserts) and view to be non-null
//                            (asserts). Must be called AFTER
//                            ImGui_ImplVulkan_Init has run.
//   * deinit()            -- free the descriptor if any (no-op when empty).
//                            Must be called before the object's destructor.
//   * update(view, layout)-- convenience shortcut equivalent to
//                            deinit(); init(view, layout);
//                            Use on resize.
//   * The destructor asserts that the object has already been deinit'd.
//   * Non-copyable and non-movable (same convention as the other resource-
//     owning classes in this file: ownership transfers are easy to lose
//     track of when debugging GPU lifetimes).
//   * Single-thread (same constraint as ImGui_ImplVulkan_AddTexture).
//---------------------------------------------------------------------------
class ImTexture
{
public:
  ImTexture() = default;
  ~ImTexture() { assert(m_set == VK_NULL_HANDLE && "Missing deinit()"); }

  ImTexture(const ImTexture&)            = delete;
  ImTexture& operator=(const ImTexture&) = delete;
  ImTexture(ImTexture&&)                 = delete;
  ImTexture& operator=(ImTexture&&)      = delete;

  // Allocate a descriptor set for the given view. Requires empty state and a non-null view.
  void init(VkImageView view, VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL)
  {
    assert(m_set == VK_NULL_HANDLE && "Missing deinit()");
    assert(view != VK_NULL_HANDLE && "ImTexture::init requires a valid VkImageView");
    m_set = ImGui_ImplVulkan_AddTexture(view, layout);
  }

  // Free the descriptor set if any. No-op when empty.
  void deinit()
  {
    if(m_set != VK_NULL_HANDLE)
    {
      ImGui_ImplVulkan_RemoveTexture(m_set);
      m_set = VK_NULL_HANDLE;
    }
  }

  // Convenience: deinit() + init(view, layout). Use on resize.
  void update(VkImageView view, VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL)
  {
    deinit();
    init(view, layout);
  }

  operator ImTextureRef() const { return ImTextureRef(reinterpret_cast<ImTextureID>(m_set)); }

private:
  VkDescriptorSet m_set{VK_NULL_HANDLE};
};


}  // namespace utils

// End of framework. The sample itself is in `minimal_latest.cpp`.
