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

/*------------------------------------------------------------------

# minimal_latest.cpp

This is the sample application. The reusable Vulkan helpers it builds on live
in `vk_framework.h`; start there if you're new.

## What You'll See
The sample renders two intersecting triangles:
- One triangle is colored and dynamically animated by a compute shader
- The other triangle is textured using an image loaded from disk
- Distance-field rendered points that flash based on time
- ImGui overlay showing frame rate, plus a Settings panel

## Main Rendering Loop (drawFrame())
1. Wait on the slot's timeline value, then acquire the next swapchain image
2. Update vertex positions via the compute shader (vkCmdPushConstants2 + BDA)
3. Record render commands:
   - Update the scene info buffer (vkCmdUpdateBuffer to a BDA-addressed buffer)
   - Bind sampler / resource descriptor heaps
   - Set every dynamic state required by shader objects once per frame
   - Bind (vertex, fragment) shader objects via vkCmdBindShadersEXT
   - Draw colored triangle, swap fragment shader, draw textured triangle
   - Render the offscreen RenderTarget into the swapchain image via ImGui
4. Submit and present (signals presentSemaphore[image], advances slot)

See detailed documentation above each function for implementation details.


There are a lot of comments; to strip all the block documentation, replace
using this regular expression:
- Visual Studio: /\*--([\r\n]|.)*?-\*\/
- Others       : /\*--[\s\S]*?-\*\/

------------------------------------------------------------------*/


//--- VMA implementation bootstrap ----------------------------------------------
// The Vulkan Memory Allocator header is "single-TU": exactly one .cpp must
// define VMA_IMPLEMENTATION before including vk_mem_alloc.h. This is that .cpp;
// every other piece of code in this project reaches VMA through vk_framework.h,
// which brings in the header (types only) wherever it is included.
//
// Volk must precede VMA so vk_mem_alloc.h sees the Vulkan types.
#include "volk.h"

#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
  {                                                                                                                    \
    printf((format), __VA_ARGS__);                                                                                     \
    printf("\n");                                                                                                      \
  }
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#pragma warning(push)
#pragma warning(disable : 4100)  // Unreferenced formal parameter
#pragma warning(disable : 4189)  // Local variable is initialized but not referenced
#pragma warning(disable : 4127)  // Conditional expression is constant
#pragma warning(disable : 4324)  // Structure was padded due to alignment specifier
#pragma warning(disable : 4505)  // Unreferenced function with internal linkage has been removed
#include "vk_mem_alloc.h"
#pragma warning(pop)
#ifdef __clang__
#pragma clang diagnostic pop
#endif

//--- Framework -----------------------------------------------------------------
// Brings in volk again (include-guarded, no-op), VMA types, GLFW, ImGui,
// logger.h, debug_util.h, STL; ASSERT / VK_CHECK macros; and the utils::
// namespace (Buffer/Image structs, barrier + layout helpers, Context,
// Swapchain, FramePacer, ResourceAllocator, SamplerPool, RenderTarget, findFile).
#include "vk_framework.h"

//--- stb_image implementation (single-TU requirement) --------------------------
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/*--
 * The shaders are compiled to SPIR-V and embedded in the C++ code by CMake.
 * Both Slang and GLSL variants are always compiled; the switch below only
 * decides which SPIR-V blobs the C++ includes and binds.
 *
 * Slang is the canonical shader path for this sample. Flip USE_SLANG to 0 to
 * build against the GLSL equivalents instead -- useful for comparing the two
 * languages compiling down to the same Vulkan behaviour, and for CI coverage.
 * CMake (-DUSE_SLANG=OFF) can override this default without editing the file.
-*/
#ifndef USE_SLANG
#define USE_SLANG 1
#endif

#if USE_SLANG
#include "_autogen/shader.comp.slang.h"
#include "_autogen/shader.rast.slang.h"
#else
#include "_autogen/shader.frag.glsl.h"
#include "_autogen/shader.vert.glsl.h"
#include "_autogen/shader.comp.glsl.h"
#endif

namespace shaderio {  // Shader IO namespace -- shared layout between C++ and shaders
using namespace glm;  // GLSL-style types without the glm:: prefix inside the namespace
#include "shaders/shader_io.h"
}  // namespace shaderio


//--- Geometry -------------------------------------------------------------------------------------------------------------

/*--
 * Structure to hold the vertex data (see in shader_io.h), consisting only of a position, color and texture coordinates
 * Later we create a buffer with this data and use it to render a triangle.
-*/
struct Vertex : public shaderio::Vertex
{
  /*--
   * The binding description tells the pipeline at which rate to load data from
   * memory throughout the vertices. Returned by const-reference to a static
   * constexpr array so we don't reconstruct it on every call.
  -*/
  static const auto& getBindingDescription()
  {
    static constexpr auto kBindings = std::to_array<VkVertexInputBindingDescription>(
        {{.binding = 0, .stride = sizeof(Vertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX}});
    return kBindings;
  }

  /*--
   * The attribute descriptions describe how to extract a vertex attribute from
   * a chunk of vertex data originating from a binding description.
   * See in the vertex shader how the location is used to access the data.
  -*/
  static const auto& getAttributeDescriptions()
  {
    static constexpr auto kAttributes = std::to_array<VkVertexInputAttributeDescription>(
        {{.location = shaderio::LVPosition, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = uint32_t(offsetof(Vertex, position))},
         {.location = shaderio::LVColor, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = uint32_t(offsetof(Vertex, color))},
         {.location = shaderio::LVTexCoord, .format = VK_FORMAT_R32G32_SFLOAT, .offset = uint32_t(offsetof(Vertex, texCoord))}});
    return kAttributes;
  }
};

// 2x3 vertices with a position, color and texCoords, make two CCW triangles
static const auto s_vertices = std::to_array<shaderio::Vertex>({
    {{0.0F, -0.5F, 0.5F}, {1.0F, 0.0F, 0.0F}, {0.5F, 0.5F}},  // Colored triangle
    {{-0.5F, 0.5F, 0.5F}, {0.0F, 0.0F, 1.0F}, {0.5F, 0.5F}},
    {{0.5F, 0.5F, 0.5F}, {0.0F, 1.0F, 0.0F}, {0.5F, 0.5F}},
    //
    {{0.1F, -0.4F, 0.75F}, {.3F, .3F, .3F}, {0.5F, 1.0F}},  // White triangle (textured)
    {{-0.4F, 0.6F, 0.25F}, {1.0F, 1.0F, 1.0F}, {1.0F, 0.0F}},
    {{0.6F, 0.6F, 0.75F}, {.7F, .7F, .7F}, {0.0F, 0.0F}},
});


// Points stored in a buffer and retrieved using buffer reference (flashing points)
static const auto s_points = std::to_array<glm::vec2>({{0.05F, 0.0F}, {-0.05F, 0.0F}, {0.0F, -0.05F}, {0.0F, 0.05F}});


//--- MinimalLatest ------------------------------------------------------------------------------------------------------------
// Main class for the sample

/*--
 * The application is the main class that is used to create the window, the Vulkan context, the swapchain, and the resources.
 *  - run the main loop.
 *  - render the scene.
-*/
class MinimalLatest
{
public:
  MinimalLatest() = default;
  MinimalLatest(VkExtent2D size = {800, 600})
      : m_windowSize(size)
  {
    // Vulkan Loader
    VK_CHECK(volkInitialize());
    // Create the GLFW window
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
#if USE_SLANG
    const char* windowTitle = "Minimal Latest (Slang)";
#else
    const char* windowTitle = "Minimal Latest (GLSL)";
#endif
    m_window = glfwCreateWindow(m_windowSize.width, m_windowSize.height, windowTitle, nullptr, nullptr);
    init();
  }

  ~MinimalLatest()
  {
    destroy();
    glfwDestroyWindow(m_window);
  }

  void run()
  {
    // Main rendering loop
    while(!glfwWindowShouldClose(m_window))
    {
      // Pacer: this is the frame rate limiter, it will limit the frame rate to the minimum refresh rate of the monitors.
      m_framePacer.paceFrame(m_vSync ? utils::getMonitorsMinRefreshRate() : 10000.0);

      glfwPollEvents();
      if(glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) == GLFW_TRUE)
      {
        ImGui_ImplGlfw_Sleep(10);  // Do nothing when minimized
        continue;
      }
      ImGui_ImplVulkan_NewFrame();
      ImGui_ImplGlfw_NewFrame();
      ImGui::NewFrame();

      /*--
       * IMGUI Docking
       * Create a dockspace and dock the viewport and settings window.
       * The central node is named "Viewport", which can be used later with Begin("Viewport")
       * to render the final image.
      -*/
      const ImGuiDockNodeFlags dockFlags = ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_NoDockingInCentralNode;
      ImGuiID dockID = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(), dockFlags);
      // Docking layout, must be done only if it doesn't exist
      if(!ImGui::DockBuilderGetNode(dockID)->IsSplitNode() && !ImGui::FindWindowByName("Viewport"))
      {
        ImGui::DockBuilderDockWindow("Viewport", dockID);  // Dock "Viewport" to  central node
        ImGui::DockBuilderGetCentralNode(dockID)->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;  // Remove "Tab" from the central node
        ImGuiID leftID = ImGui::DockBuilderSplitNode(dockID, ImGuiDir_Left, 0.2f, nullptr, &dockID);  // Split the central node
        ImGui::DockBuilderDockWindow("Settings", leftID);  // Dock "Settings" to the left node
      }
      // [optional] Show the menu bar
      if(ImGui::BeginMainMenuBar())
      {
        if(ImGui::BeginMenu("File"))
        {
          if(ImGui::MenuItem("vSync", "", &m_vSync))
            m_swapchain.requestRebuild();  // Recreate the swapchain with the new vSync setting
          ImGui::Separator();
          if(ImGui::MenuItem("Exit"))
            glfwSetWindowShouldClose(m_window, true);
          ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
      }
      /* END Docking */

      // We define "viewport" with no padding an retrieve the rendering area
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
      ImGui::Begin("Viewport");
      ImVec2 windowSize = ImGui::GetContentRegionAvail();
      ImGui::End();
      ImGui::PopStyleVar();

      // Verify if the viewport has a new size and resize the RenderTarget accordingly.
      const VkExtent2D viewportSize = {uint32_t(windowSize.x), uint32_t(windowSize.y)};
      if(m_viewportSize.width != viewportSize.width || m_viewportSize.height != viewportSize.height)
      {
        onViewportSizeChange(viewportSize);
      }

      // Extra ImGui windows can be added here, like the demo window.
      // ImGui::ShowDemoWindow();

      // Frame Resource Preparation - only render if preparation succeeds
      if(prepareFrameResources())
      {
        // Begin command buffer recording
        VkCommandBuffer cmd = beginCommandRecording();

        // Record rendering commands
        drawFrame(cmd);

        // Ends recording of commands for the frame
        endCommandRecording(cmd);

        // End frame and present
        endFrame(cmd);
      }
      else
      {
        // If frame preparation failed (e.g., swapchain needs rebuild), just skip this frame
        // The next frame will try again
      }

      // Update and Render additional Platform Windows (floating windows)
      ImGui::EndFrame();
      if((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0)
      {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
      }
    }
  }

private:
  /*--
   * Main initialization sequence
   *
   * 1. Vulkan context
   *    - Instance with validation layers and debug messenger
   *    - Physical device (prefers discrete GPU) and logical device
   *    - Graphics queue; feature chain asserted for every required bool
   *
   * 2. Core subsystems
   *    - VMA allocator (BDA opt-in per buffer)
   *    - Sampler pool, window surface, transient command pool
   *    - Swapchain (chooses imageCount and framesInFlight independently)
   *    - Per-slot frame data sized by framesInFlight, not imageCount
   *
   * 3. UI (Dear ImGui)
   *    - Traditional VkDescriptorPool + internal VkPipeline
   *    - Independent from this sample's descriptor heap / shader objects
   *
   * 4. Rendering path
   *    - Offscreen RenderTarget (color + depth)
   *    - Graphics: one VS + two FS shader objects (specialization variants)
   *    - Compute: traditional VkPipeline (deliberate contrast)
   *
   * 5. GPU data
   *    - Vertex, points, and scene-info buffers (all BDA-accessible)
   *    - Textures + sampler heap + resource heap (bindless descriptors)
   *
   * The "Resource model" comment below this function explains *why*
   * each resource class uses the binding mechanism it does.
  -*/
  void init()
  {
    /*-- Resource model -----------------------------------------------------------
     *
     * This sample picks a different binding mechanism per resource class. They
     * coexist freely; understanding which goes where is the key to reading
     * the rest of this file.
     *
     *   Graphics shaders --> Shader objects (VK_EXT_shader_object).
     *     There is no graphics VkPipeline at all. Three VkShaderEXT objects
     *     (one vertex + two fragment variants) are created up front; at draw
     *     time vkCmdBindShadersEXT picks the (vert, frag) pair, and every
     *     piece of state that used to live in a pipeline (rasterization,
     *     depth/stencil, blend, vertex input, ...) is set with a vkCmdSet*EXT
     *     call. See createGraphicsShaders() and setGraphicsDynamicState().
     *
     *   Compute shader --> Traditional VkPipeline + VkPipelineLayout.
     *     Compute *could* also use shader objects (VK_EXT_shader_object supports
     *     VK_SHADER_STAGE_COMPUTE_BIT), but it is intentionally kept on the
     *     traditional path so the sample shows both styles side-by-side and
     *     because compute does not benefit from shader objects' main wins
     *     (no dynamic state to make dynamic, single-shader pipeline so nothing
     *     to mix-and-match). See createComputeShaderPipeline() for the full
     *     rationale.
     *
     *   Textures / samplers --> Descriptor heap (VK_EXT_descriptor_heap).
     *     Sampler and image descriptors live in two GPU buffers (sampler heap
     *     + resource heap). Shaders index them by integer slot. The graphics
     *     shader objects carry VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT and no
     *     descriptor set layouts. See createDescriptorHeap() and
     *     recordGraphicsCommands().
     *
     *   Storage / uniform buffers --> Buffer device address (BDA).
     *     The shader holds a buffer_reference (a typed GPU pointer) and
     *     dereferences it directly. No descriptor needed. See m_vertexBuffer,
     *     m_pointsBuffer, m_sceneInfoBuffer.
     *
     *   Small per-draw data --> vkCmdPushDataEXT (graphics) or
     *                            vkCmdPushConstants2 (compute).
     *     Graphics has no pipeline layout, so traditional push constants are
     *     unavailable; vkCmdPushDataEXT writes directly to the shader's
     *     push_constant block. Compute keeps the legacy path with a real
     *     VkPushConstantRange in its pipeline layout.
     *
     *   ImGui still uses traditional VkDescriptorSet/VkDescriptorPool and its
     *   own VkPipeline internally (m_uiDescriptorPool); that path is
     *   independent from the heap and from shader objects.
     *
     * -----------------------------------------------------------------------*/

    // Vulkan feature structs - allocated on the stack
    VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR unifiedImageLayoutsFeature{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR};
    // Descriptor heap replaces traditional descriptor sets/pools with GPU buffer-based bindless descriptors.
    // Samplers and images are written into heap buffers and accessed by index in the shaders.
    VkPhysicalDeviceDescriptorHeapFeaturesEXT descriptorHeapFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT};
    // Untyped pointers: required by descriptor heap
    VkPhysicalDeviceShaderUntypedPointersFeaturesKHR untypedPtrFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR};
    // Shader objects: replace VkPipeline for graphics with linkable, reusable VkShaderEXT
    // objects bound via vkCmdBindShadersEXT. Pairs naturally with the layout=NULL design:
    // there is no graphics pipeline object at all, only shader objects + dynamic state.
    VkPhysicalDeviceShaderObjectFeaturesEXT shaderObjectFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT};
    // Extended dynamic state 3: required by shader objects for blend/rasterization
    // state that no longer lives in a pipeline object.
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamicState3Features{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    // Vertex input dynamic state: required by shader objects (vertex bindings/attributes
    // become a vkCmdSetVertexInputEXT call instead of pipeline state).
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertexInputDynamicStateFeatures{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};

    // Configure Vulkan context with required and optional extensions
    utils::ContextCreateInfo contextConfig;

    // Required extensions (with their feature struct pointers)
    contextConfig.deviceExtensions.push_back({VK_KHR_SWAPCHAIN_EXTENSION_NAME, true, nullptr});
    contextConfig.deviceExtensions.push_back({VK_KHR_UNIFIED_IMAGE_LAYOUTS_EXTENSION_NAME, true, &unifiedImageLayoutsFeature});
    contextConfig.deviceExtensions.push_back({VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME, true, &descriptorHeapFeatures});  // Bindless descriptor heap for textures and samplers
    contextConfig.deviceExtensions.push_back({VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME, true, &untypedPtrFeatures});  // Required by bindless
    contextConfig.deviceExtensions.push_back({VK_EXT_SHADER_OBJECT_EXTENSION_NAME, true, &shaderObjectFeatures});  // Graphics: shader objects instead of pipelines
    contextConfig.deviceExtensions.push_back({VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, true, &dynamicState3Features});  // Required for shader-object blend/rasterization state
    contextConfig.deviceExtensions.push_back({VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, true,
                                              &vertexInputDynamicStateFeatures});  // Required for shader-object vertex input

    // Create the Vulkan context with configuration
    m_context.init(contextConfig);

    // After context.init(), vkGetPhysicalDeviceFeatures2 has populated every
    // feature struct in the pNext chain. Assert the bools we actually depend
    // on -- vkCreateDevice will already have failed if a required feature is
    // missing, but these asserts give a clearer diagnostic on non-conformant
    // drivers and document the hard dependencies at a glance.
    ASSERT(unifiedImageLayoutsFeature.unifiedImageLayouts, "unifiedImageLayouts required (GENERAL attachment usage)");
    ASSERT(descriptorHeapFeatures.descriptorHeap, "descriptorHeap required");
    ASSERT(untypedPtrFeatures.shaderUntypedPointers, "shaderUntypedPointers required (by descriptorHeap)");
    ASSERT(shaderObjectFeatures.shaderObject, "shaderObject required (graphics path)");
    ASSERT(dynamicState3Features.extendedDynamicState3ColorBlendEnable, "extendedDynamicState3 required (shader objects)");
    ASSERT(vertexInputDynamicStateFeatures.vertexInputDynamicState, "vertexInputDynamicState required (shader objects)");

    // Initialize the VMA allocator. Pass the *device* API version (not the instance loader
    // version) clamped to the highest version VMA understands in this build (1.4).
    m_allocator.init(VmaAllocatorCreateInfo{
        .physicalDevice   = m_context.getPhysicalDevice(),
        .device           = m_context.getDevice(),
        .instance         = m_context.getInstance(),
        .vulkanApiVersion = std::min(m_context.getDeviceApiVersion(), VK_API_VERSION_1_4),
    });

    // Texture sampler pool
    m_samplerPool.init(m_context.getDevice());

    // Create the window surface
    VK_CHECK(glfwCreateWindowSurface(m_context.getInstance(), m_window, nullptr, static_cast<VkSurfaceKHR*>(&m_surface)));
    DBG_VK_NAME(m_surface);

    // Used for creating single-time command buffers
    createTransientCommandPool();

    // Create the swapchain
    m_swapchain.init(m_context.getPhysicalDevice(), m_context.getDevice(), m_context.getGraphicsQueue(), m_surface, m_transientCmdPool);
    m_windowSize = m_swapchain.initResources(m_vSync);  // Update the window size to the actual size of the surface

    // Create what is needed to submit the scene for each frame in-flight
    // m_frameData is sized by frames-in-flight (CPU parallelism), NOT by
    // imageCount (GPU/presentation parallelism).
    createFrameSubmission(m_swapchain.getFramesInFlight());

    // Create a descriptor pool for ImGui (it still needs a traditional pool for its own textures)
    createUIDescriptorPool();

    // Initializing Dear ImGui
    initImGui();

    // Create the RenderTarget (offscreen color + depth)
    {
      VkCommandBuffer cmd = utils::beginSingleTimeCommands(m_context.getDevice(), m_transientCmdPool);

      const VkFormat                depthFormat = utils::findDepthFormat(m_context.getPhysicalDevice());
      utils::RenderTargetCreateInfo rtInit{
          .device = m_context.getDevice(),
          .alloc  = &m_allocator,
          .size   = m_windowSize,
          .color  = {VK_FORMAT_R8G8B8A8_UNORM},  // Single color attachment
          .depth  = depthFormat,
      };
      m_renderTarget.init(cmd, rtInit);
      m_imTexture.init(m_renderTarget.getUiImageView(0));  // ImGui texture wrapper for the RenderTarget's color image view

      utils::endSingleTimeCommands(cmd, m_context.getDevice(), m_transientCmdPool, m_context.getGraphicsQueue().queue);
    }

    // Create graphics shader objects (VK_EXT_shader_object). No graphics pipeline
    // is built; everything that used to live in a pipeline is now dynamic state.
    createGraphicsShaders();

    // Create the compute shader pipeline and layout (compute keeps the traditional
    // pipeline path; only graphics is migrated to shader objects in this sample).
    createComputeShaderPipeline();

    // Create GPU buffers (SSBO) containing the vertex data and the point data, and the image (uploading data to GPU)
    {
      VkCommandBuffer cmd = utils::beginSingleTimeCommands(m_context.getDevice(), m_transientCmdPool);
      // Buffer of all vertices. Used as a vertex buffer by the graphics pipeline,
      // AND accessed via BDA by the compute shader that animates the first triangle
      // (see PushConstantCompute::bufferAddress). Hence both VERTEX_BUFFER and
      // SHADER_DEVICE_ADDRESS usage bits.
      m_vertexBuffer = m_allocator.createBufferAndUploadData(cmd, std::span<const shaderio::Vertex>(s_vertices),
                                                             VK_BUFFER_USAGE_2_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT
                                                                 | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT);
      DBG_VK_NAME(m_vertexBuffer.buffer);

      // Buffer of the points. Read by the fragment shader through BDA via
      // SceneInfo::dataBufferAddress (no descriptor binding).
      m_pointsBuffer = m_allocator.createBufferAndUploadData(cmd, std::span<const glm::vec2>(s_points),
                                                             VK_BUFFER_USAGE_2_STORAGE_BUFFER_BIT
                                                                 | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT);
      DBG_VK_NAME(m_pointsBuffer.buffer);

      // Load and create the images
      const std::vector<std::string> searchPaths = {".", "resources", "../resources", "../../resources"};
      std::string                    filename    = utils::findFile("image1.jpg", searchPaths);
      ASSERT(!filename.empty(), "Could not load texture image!");
      m_image[0] = loadAndCreateImage(cmd, filename);

      filename = utils::findFile("image2.jpg", searchPaths);
      ASSERT(!filename.empty(), "Could not load texture image!");
      m_image[1] = loadAndCreateImage(cmd, filename);

      // Create the descriptor heap buffers (sampler + resource) and upload them to the GPU.
      // This must happen after images are loaded, since the heap references the VkImage handles.
      createDescriptorHeap(cmd);

      utils::endSingleTimeCommands(cmd, m_context.getDevice(), m_transientCmdPool, m_context.getGraphicsQueue().queue);
    }
    m_allocator.freeStagingBuffers();  // Data is uploaded, staging buffers can be released

    // Create a buffer to store the scene information, updated once per frame via vkCmdUpdateBuffer.
    // The shader accesses it through its buffer device address (BDA), not a descriptor set.
    m_sceneInfoBuffer = m_allocator.createBuffer(sizeof(shaderio::SceneInfo),
                                                 VK_BUFFER_USAGE_2_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT
                                                     | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT,
                                                 VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    DBG_VK_NAME(m_sceneInfoBuffer.buffer);
  }

  /*--
   * Destroy all resources and the Vulkan context
  -*/
  void destroy()
  {
    VkDevice device = m_context.getDevice();
    VK_CHECK(vkDeviceWaitIdle(device));

    m_swapchain.deinit();
    m_samplerPool.deinit();
    m_imTexture.deinit();  // Destroy the ImGui texture wrapper before shutting down ImGui itself

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    vkDestroyPipeline(device, m_computePipeline, nullptr);
    vkDestroyPipelineLayout(device, m_computePipelineLayout, nullptr);
    vkDestroyShaderEXT(device, m_vertShader, nullptr);
    vkDestroyShaderEXT(device, m_fragShaderTextured, nullptr);
    vkDestroyShaderEXT(device, m_fragShaderNoTexture, nullptr);
    vkDestroyCommandPool(device, m_transientCmdPool, nullptr);
    vkDestroySurfaceKHR(m_context.getInstance(), m_surface, nullptr);
    vkDestroyDescriptorPool(device, m_uiDescriptorPool, nullptr);  // ImGui descriptor pool

    // Frame info
    for(size_t i = 0; i < m_frameData.size(); i++)
    {
      vkFreeCommandBuffers(device, m_frameData[i].cmdPool, 1, &m_frameData[i].cmdBuffer);
      vkDestroyCommandPool(device, m_frameData[i].cmdPool, nullptr);
    }
    vkDestroySemaphore(device, m_frameTimelineSemaphore, nullptr);

    m_allocator.destroyBuffer(m_vertexBuffer);
    m_allocator.destroyBuffer(m_pointsBuffer);
    m_allocator.destroyBuffer(m_sceneInfoBuffer);     // Scene info GPU buffer (accessed via BDA)
    m_allocator.destroyBuffer(m_samplerHeapBuffer);   // Descriptor heap: sampler GPU buffer
    m_allocator.destroyBuffer(m_resourceHeapBuffer);  // Descriptor heap: resource (image) GPU buffer
    for(auto& img : m_image)
    {
      m_allocator.destroyImageResource(img);
    }

    m_renderTarget.deinit();

    m_allocator.deinit();
    m_context.deinit();
  }

  /*--
   * Create a command pool for short lived operations (one-off uploads, image
   * layout transitions, ...). TRANSIENT_BIT hints the driver that command
   * buffers from this pool are short-lived.
  -*/
  void createTransientCommandPool()
  {
    m_transientCmdPool = utils::createCommandPool(m_context.getDevice(), m_context.getGraphicsQueue().familyIndex,
                                                  VK_COMMAND_POOL_CREATE_TRANSIENT_BIT);
    DBG_VK_NAME(m_transientCmdPool);
  }

  /*--
   * Creates a command pool (long life) and buffer for each frame in flight. Unlike the temporary command pool,
   * these pools persist between frames and don't use VK_COMMAND_POOL_CREATE_TRANSIENT_BIT.
   * Each frame gets its own command buffer which records all rendering commands for that frame.
  -*/
  void createFrameSubmission(uint32_t numFrames)
  {
    VkDevice device = m_context.getDevice();

    m_frameData.resize(numFrames);

    // Initialize timeline semaphore at 0. We'll use a monotonic counter (m_frameCounter) starting at 1.
    const uint64_t initialValue = 0;

    VkSemaphoreTypeCreateInfo timelineCreateInfo = {
        .sType         = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .pNext         = nullptr,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue  = initialValue,
    };

    /*-- 
     * Create timeline semaphore for GPU-CPU synchronization
     * This ensures resources aren't overwritten while still in use by the GPU
    -*/
    const VkSemaphoreCreateInfo semaphoreCreateInfo{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &timelineCreateInfo};
    VK_CHECK(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &m_frameTimelineSemaphore));
    DBG_VK_NAME(m_frameTimelineSemaphore);

    /*-- 
     * Create command pools and buffers for each frame
     * Each frame gets its own command pool to allow parallel command recording while previous frames may still be executing on the GPU
    -*/
    const uint32_t queueFamily = m_context.getGraphicsQueue().familyIndex;

    for(uint32_t i = 0; i < numFrames; i++)
    {
      m_frameData[i].lastSignalValue = initialValue;  // Initialize to timeline semaphore's initial value

      // Separate pools allow independent reset/recording of commands while other frames are still in-flight
      m_frameData[i].cmdPool = utils::createCommandPool(device, queueFamily);
      DBG_VK_NAME(m_frameData[i].cmdPool);

      const VkCommandBufferAllocateInfo commandBufferAllocateInfo = {
          .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
          .commandPool        = m_frameData[i].cmdPool,
          .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
          .commandBufferCount = 1,
      };
      VK_CHECK(vkAllocateCommandBuffers(device, &commandBufferAllocateInfo, &m_frameData[i].cmdBuffer));
      DBG_VK_NAME(m_frameData[i].cmdBuffer);
    }
  }

  /*--
   * Main frame rendering function
   * 
   * 1. Frame Setup
   *    - Waits for previous frame to complete
   *    - Acquires next swapchain image
   *    - Begins command buffer recording
   * 
   * 2. Compute Pass
   *    - Updates vertex positions through compute shader
   *    - Synchronizes compute/graphics operations
   * 
   * 3. Graphics Pass
   *    - Updates scene uniform buffer
   *    - Begins dynamic rendering
   *    - Draws animated triangle
   *    - Draws textured triangle
   * 
   * 4. Finalization
   *    - Begins dynamic rendering in swapchain
   *    - Renders UI overlay
   * 
   * 4. Frame Submission
   *    - Submits command buffer
   *    - Presents the rendered image
   * 
   * Note: Uses dynamic rendering instead of traditional render passes
  -*/
  void drawFrame(VkCommandBuffer cmd)
  {
    /*-- The ImGui code -*/

    /*-- 
     * The rendering of the scene is done using dynamic rendering into the RenderTarget (see recordGraphicsCommands).
     * The target image will be rendered/displayed using ImGui.
     * Its placement will cover the entire viewport (ImGui draws a quad with the texture we provide),
     * and the image will be displayed in the viewport.
     * There are multiple ways to display the image, but this method is the most flexible.
     * Other methods include:
     *  - Blitting the image to the swapchain image, with the UI drawn on top. However, this makes it harder 
     *    to fit the image within a specific area of the window.
     *  - Using the image as a texture in a quad and rendering it to the swapchain image. This is what ImGui 
     *    does, but we don't need to add a quad to the scene, as ImGui handles it for us.
    -*/
    // Using the dock "Viewport", this sets the window to cover the entire central viewport
    if(ImGui::Begin("Viewport"))
    {
      // !!! This is where the RenderTarget image is displayed !!!
      ImGui::Image(m_imTexture, ImGui::GetContentRegionAvail());

      // Adding overlay text on the upper left corner
      ImGui::SetCursorPos(ImVec2(0, 0));
      ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    }
    ImGui::End();

    // This is the settings window, which is docked to the left of the viewport
    if(ImGui::Begin("Settings"))
    {
      ImGui::RadioButton("Image 1", &m_imageID, 0);
      ImGui::RadioButton("Image 2", &m_imageID, 1);
      ImGui::Separator();
      ImGui::ColorPicker3("Clear Color", &m_clearColor.float32[0]);
    }

    ImGui::End();
    ImGui::Render();  // This is creating the data to draw the UI (not on GPU yet)


    /*--
     * - A compute shader is modifying the vertex position
     * - Draw commands for the triangles are recorded to the command buffer, with target image in the RenderTarget
    -*/
    recordComputeCommands(cmd);
    recordGraphicsCommands(cmd);  // Record the rendering commands for the triangles

    // Make the RenderTarget color writes visible to ImGui's fragment shader read.
    const VkMemoryBarrier2 rtToImGui{
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
    };
    const VkDependencyInfo rtToImGuiDep{
        .sType              = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers    = &rtToImGui,
    };
    vkCmdPipelineBarrier2(cmd, &rtToImGuiDep);

    // Start rendering to the swapchain
    beginDynamicRenderingToSwapchain(cmd);
    {
      // The ImGui draw commands are recorded to the command buffer, which includes the display of our RenderTarget image
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }
    endDynamicRenderingToSwapchain(cmd);
  }


  /*--
   * End the frame by submitting the command buffer to the GPU and presenting the image.
   * Adds binary semaphores to wait for the image to be available and signal when rendering is done.
   * Adds the timeline semaphore to signal when the frame is completed.
   * Moves to the next frame.
  -*/
  void endFrame(VkCommandBuffer cmd)
  {
    /*-- 
     * Prepare to submit the current frame for rendering 
     * First add the swapchain semaphore to wait for the image to be available.
    -*/
    std::vector<VkSemaphoreSubmitInfo> waitSemaphores;
    std::vector<VkSemaphoreSubmitInfo> signalSemaphores;
    waitSemaphores.push_back({
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = m_swapchain.getAcquireSemaphore(),
        .stageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    });
    signalSemaphores.push_back({
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = m_swapchain.getPresentSemaphore(),
        .stageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    });

    // Get the frame data for the current in-flight slot. The Swapchain owns the
    // frame-resource index; we use it here so both stay in lockstep by construction.
    const uint32_t frameSlot = m_swapchain.getFrameResourceIndex();
    auto&          frame     = m_frameData[frameSlot];

    /*--
     * Calculate the signal value for when this frame completes
     * Use monotonic counter that increments by 1 each frame: 1, 2, 3, 4...
    -*/
    const uint64_t signalFrameValue = m_frameCounter++;
    frame.lastSignalValue           = signalFrameValue;  // Store for next time this frame buffer is used
#ifdef NVVK_SEMAPHORE_DEBUG
    LOGI("SubmitFrame: \t\t slot=%u signalValue=%llu", frameSlot, static_cast<unsigned long long>(signalFrameValue));
#endif

    /*-- 
     * Add timeline semaphore to signal when GPU completes this frame
     * The color attachment output stage is used since that's when the frame is fully rendered
    -*/
    signalSemaphores.push_back({
        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = m_frameTimelineSemaphore,
        .value     = signalFrameValue,
        .stageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
    });

    // Note : in this sample, we only have one command buffer per frame.
    const std::array<VkCommandBufferSubmitInfo, 1> cmdBufferInfo{{{
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    }}};

    // Populate the submit info to synchronize rendering and send the command buffer
    const std::array<VkSubmitInfo2, 1> submitInfo{{{
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount   = uint32_t(waitSemaphores.size()),    //
        .pWaitSemaphoreInfos      = waitSemaphores.data(),              // Wait for the image to be available
        .commandBufferInfoCount   = uint32_t(cmdBufferInfo.size()),     //
        .pCommandBufferInfos      = cmdBufferInfo.data(),               // Command buffer to submit
        .signalSemaphoreInfoCount = uint32_t(signalSemaphores.size()),  //
        .pSignalSemaphoreInfos    = signalSemaphores.data(),            // Signal when rendering is finished
    }}};

    // Submit the command buffer to the GPU and signal when it's done
    VK_CHECK(vkQueueSubmit2(m_context.getGraphicsQueue().queue, uint32_t(submitInfo.size()), submitInfo.data(), nullptr));

    // Present the image. presentFrame() advances the swapchain's frame-resource
    // index for us, so the next call to prepareFrameResources() will pick up
    // the next slot.
    m_swapchain.presentFrame(m_context.getGraphicsQueue().queue);
  }


  /*-- 
   * Call this function if the viewport size changes 
   * This happens when the window is resized, or when the ImGui viewport window is resized.
  -*/
  void onViewportSizeChange(VkExtent2D size)
  {
    m_viewportSize = size;
    // Recreate the RenderTarget to the size of the viewport
    vkQueueWaitIdle(m_context.getGraphicsQueue().queue);

    {
      VkCommandBuffer cmd = utils::beginSingleTimeCommands(m_context.getDevice(), m_transientCmdPool);
      m_renderTarget.update(cmd, m_viewportSize);
      m_imTexture.update(m_renderTarget.getUiImageView());
      utils::endSingleTimeCommands(cmd, m_context.getDevice(), m_transientCmdPool, m_context.getGraphicsQueue().queue);
    }
  }

  /*--
   * We are using dynamic rendering, which is a more flexible way to render to the swapchain image.
   * Only ImGui is rendered to the swapchain image.
   * The scene is rendered to the RenderTarget, and the rendered image is displayed using ImGui.
  -*/
  void beginDynamicRenderingToSwapchain(VkCommandBuffer cmd) const
  {
    // Image to render to
    const std::array<VkRenderingAttachmentInfo, 1> colorAttachment{{{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = m_swapchain.getImageView(),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,   // Clear the image (see clearValue)
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,  // Store the image (keep the image)
        .clearValue  = {{{0.0f, 0.0f, 0.0f, 1.0f}}},
    }}};

    // Details of the dynamic rendering
    const VkRenderingInfo renderingInfo{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, m_windowSize},
        .layerCount           = 1,
        .colorAttachmentCount = uint32_t(colorAttachment.size()),
        .pColorAttachments    = colorAttachment.data(),
    };

    // Transition the swapchain image to general layout for use as a render target in dynamic rendering
    utils::cmdTransitionSwapchainToRenderingLayout(cmd, m_swapchain.getImage());

    vkCmdBeginRendering(cmd, &renderingInfo);
  }

  /*--
   * End of dynamic rendering.
   * The image is transitioned back to the present layout, and the rendering is ended.
  -*/
  void endDynamicRenderingToSwapchain(VkCommandBuffer cmd)
  {
    vkCmdEndRendering(cmd);

    // Transition the swapchain image back to the present layout
    utils::cmdTransitionSwapchainToPresentLayout(cmd, m_swapchain.getImage());
  }

  /*-- 
   * This is invoking the compute shader to update the Vertex in the buffer (animation of triangle).
   * Where those vertices will be then used to draw the geometry
  -*/
  void recordComputeCommands(VkCommandBuffer cmd) const
  {
    DBG_VK_SCOPE(cmd);  // <-- Helps to debug in NSight

    // Push information to the shader using Push Constant
    const shaderio::PushConstantCompute pushValues{
        .bufferAddress = m_vertexBuffer.address,           // Address of the buffer to work on
        .rotationAngle = 1.2f * ImGui::GetIO().DeltaTime,  // Rotation speed adjusted with framerate
        .numVertex     = 3,                                // We only touch the first 3 vertex (first triangle)
    };

    const VkPushConstantsInfo pushInfo{
        .sType      = VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,
        .layout     = m_computePipelineLayout,  // The compute pipeline layout only includes a push constant
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset     = 0,
        .size       = sizeof(shaderio::PushConstantCompute),
        .pValues    = &pushValues,
    };
    vkCmdPushConstants2(cmd, &pushInfo);

    // Bind the compute shader
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_computePipeline);

    // Execute the compute shader
    // The workgroup is set to 256, and we only have 3 vertex to deal with, so one group is enough
    vkCmdDispatch(cmd, 1, 1, 1);

    // Add barrier to make sure the compute shader is finished before the vertex buffer is used
    utils::cmdBufferMemoryBarrier(cmd, m_vertexBuffer.buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT);
  }

  /*-- 
   * Update the scene information buffer (GPU-side) once per frame.
   * The buffer is accessed in the shader via its buffer device address (BDA).
   * This keeps per-frame data in GPU memory, avoiding redundant push data transfers.
  -*/
  void updateSceneBuffer(VkCommandBuffer cmd) const
  {
    // Updating the data for the frame
    shaderio::SceneInfo sceneInfo{};
    float               time        = static_cast<float>(5.0 * ImGui::GetTime());
    float               sineValue   = std::sin(time) + 1.0f;    // Get the sine of the current time
    float               mappedValue = 0.5f * sineValue + 0.5f;  // Map sine value to range [0.8, 1.0]
    sceneInfo.animValue             = mappedValue;
    sceneInfo.dataBufferAddress     = m_pointsBuffer.address;
    sceneInfo.resolution            = glm::vec2(m_viewportSize.width, m_viewportSize.height);
    sceneInfo.numData               = uint32_t(s_points.size());
    sceneInfo.texId                 = m_imageID;

    // Add a barrier to make sure nothing was writing to it, before updating its content
    utils::cmdBufferMemoryBarrier(cmd, m_sceneInfoBuffer.buffer,
                                  VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                  VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    // Update the buffer with the new data
    vkCmdUpdateBuffer(cmd, m_sceneInfoBuffer.buffer, 0, sizeof(shaderio::SceneInfo), &sceneInfo);
    // Add barrier to make sure the buffer is updated before the shaders use it
    utils::cmdBufferMemoryBarrier(cmd, m_sceneInfoBuffer.buffer, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
  }

  /*--
   * Recording the commands to render the scene
  -*/
  void recordGraphicsCommands(VkCommandBuffer cmd)
  {
    DBG_VK_SCOPE(cmd);  // <-- Helps to debug in Nsight

    // Update the scene buffer once per frame (cannot be done inside dynamic rendering)
    updateSceneBuffer(cmd);

    const VkDeviceSize offsets[] = {0};
    const VkDeviceSize sizes[]   = {VK_WHOLE_SIZE};
    const VkViewport   viewport{0.0F, 0.0F, float(m_viewportSize.width), float(m_viewportSize.height), 0.0F, 1.0F};
    const VkRect2D     scissor{{0, 0}, m_viewportSize};

    /*--
     * Prepare push data: with descriptor heap, the pipeline layout is VK_NULL_HANDLE so we cannot
     * use traditional push constants or push descriptors. Instead, vkCmdPushDataEXT sends small
     * per-draw data to the shader's push_constant block. Push data carries only:
     *   - The buffer device address of the SceneInfo buffer (updated once per frame above)
     *   - The per-draw triangle color (changes between draw calls)
     * The shader reads SceneInfo from GPU memory via buffer reference, keeping push data small.
    -*/
    shaderio::GraphicsPushData pushData{};
    pushData.sceneInfoAddress = m_sceneInfoBuffer.address;  // Points to the GPU buffer updated above

    // VkPushDataInfoEXT describes where in push_constant space to write and the CPU data to upload
    VkPushDataInfoEXT pushDataInfo{
        .sType  = VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,
        .offset = 0,
        .data   = {.address = &pushData, .size = sizeof(shaderio::GraphicsPushData)},
    };

    // Image to render to
    const std::array<VkRenderingAttachmentInfo, 1> colorAttachment{{{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = m_renderTarget.getColorImageView(),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,   // Clear the image (see clearValue)
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,  // Store the image (keep the image)
        .clearValue  = {{m_clearColor}},
    }}};

    // Depth buffer to use
    const VkRenderingAttachmentInfo depthAttachment{
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = m_renderTarget.getDepthImageView(),
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,   // Clear depth buffer
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,  // Store depth buffer
        .clearValue  = {{{1.0f, 0}}},
    };

    // Details of the dynamic rendering
    const VkRenderingInfo renderingInfo{
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = {{0, 0}, m_renderTarget.getSize()},
        .layerCount           = 1,
        .colorAttachmentCount = uint32_t(colorAttachment.size()),
        .pColorAttachments    = colorAttachment.data(),
        .pDepthAttachment     = &depthAttachment,
    };

    // Begin dynamic rendering
    vkCmdBeginRendering(cmd, &renderingInfo);

    // ---- Dynamic state ----------------------------------------------------
    // With shader objects, every piece of state that used to live in the
    // graphics pipeline must be set explicitly with a vkCmdSet*EXT call before
    // each draw. We set everything once here since none of it changes between
    // the two draws below.
    setGraphicsDynamicState(cmd, viewport, scissor);

    // Bind the descriptor heap buffers so shaders can access textures and samplers by index.
    // This replaces the traditional vkCmdBindDescriptorSets2 call that was used for the texture descriptor set.
    // The sampler heap provides sampler descriptors (linear, nearest, etc.) and the resource heap provides image descriptors (loaded textures).
    // Each VkBindHeapInfoEXT describes the heap buffer address, total size, and where the mandatory reserved range sits within the buffer.
    const VkBindHeapInfoEXT samplerHeapBind{
        .sType               = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .heapRange           = {m_samplerHeapBuffer.address, m_samplerHeapSize},
        .reservedRangeOffset = m_samplerReservedOffset,
        .reservedRangeSize   = m_samplerReservedSize,
    };
    vkCmdBindSamplerHeapEXT(cmd, &samplerHeapBind);

    const VkBindHeapInfoEXT resourceHeapBind{
        .sType               = VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,
        .heapRange           = {m_resourceHeapBuffer.address, m_resourceHeapSize},
        .reservedRangeOffset = m_resourceReservedOffset,
        .reservedRangeSize   = m_resourceReservedSize,
    };
    vkCmdBindResourceHeapEXT(cmd, &resourceHeapBind);

    // Bind the vertex buffer. vkCmdBindVertexBuffers2 (Vulkan 1.3 core) extends the
    // older vkCmdBindVertexBuffers with optional pSizes and pStrides arrays. With
    // shader objects, vertex input layout is dynamic (set via vkCmdSetVertexInputEXT
    // above), so passing explicit sizes/strides here is fine.
    vkCmdBindVertexBuffers2(cmd, 0, 1, &m_vertexBuffer.buffer, offsets, sizes, nullptr);

    // Bind shader objects: vertex stage (shared) + the no-texture fragment stage.
    utils::cmdBindGraphicsShaders(cmd, m_vertShader, m_fragShaderNoTexture);

    // Push data for the first triangle (red, no texture)
    pushData.color = glm::vec3(1, 0, 0);
    vkCmdPushDataEXT(cmd, &pushDataInfo);
    vkCmdDraw(cmd, 3, 1, 0, 0);  // 3 vertices, 1 instance, 0 offset

    // Swap to the textured fragment shader; vertex stage stays bound.
    utils::cmdBindGraphicsShaders(cmd, m_vertShader, m_fragShaderTextured);

    // Push data again with different color for the second triangle (green, with texture)
    pushData.color = glm::vec3(0, 1, 0);
    vkCmdPushDataEXT(cmd, &pushDataInfo);
    vkCmdDraw(cmd, 3, 1, 3, 0);  // 3 vertices, 1 instance, 3 offset (second triangle)

    vkCmdEndRendering(cmd);
  }

  /*--
   * Set every dynamic state required by shader objects before a draw.
   *
   * Without a graphics pipeline object, all of viewport/scissor, rasterization,
   * depth/stencil, blend, multisample, vertex input, and primitive topology are
   * dynamic. The list below covers what this sample uses; tessellation, mesh
   * shading, transform feedback, conservative rasterization, etc. would each
   * add their own vkCmdSet*EXT calls (and the matching device features).
  -*/
  void setGraphicsDynamicState(VkCommandBuffer cmd, const VkViewport& viewport, const VkRect2D& scissor) const
  {
    // Viewport / scissor (counts and values are both dynamic).
    vkCmdSetViewportWithCount(cmd, 1, &viewport);
    vkCmdSetScissorWithCount(cmd, 1, &scissor);

    // Vertex input: bindings (stride, input rate) and attributes (location, format, offset).
    // VK_EXT_vertex_input_dynamic_state replaces VkPipelineVertexInputStateCreateInfo.
    const VkVertexInputBindingDescription2EXT vertexBinding{
        .sType     = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
        .binding   = 0,
        .stride    = sizeof(Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        .divisor   = 1,
    };
    const std::array<VkVertexInputAttributeDescription2EXT, 3> vertexAttributes = {{
        {.sType    = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
         .location = shaderio::LVPosition,
         .binding  = 0,
         .format   = VK_FORMAT_R32G32B32_SFLOAT,
         .offset   = uint32_t(offsetof(Vertex, position))},
        {.sType    = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
         .location = shaderio::LVColor,
         .binding  = 0,
         .format   = VK_FORMAT_R32G32B32_SFLOAT,
         .offset   = uint32_t(offsetof(Vertex, color))},
        {.sType    = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
         .location = shaderio::LVTexCoord,
         .binding  = 0,
         .format   = VK_FORMAT_R32G32_SFLOAT,
         .offset   = uint32_t(offsetof(Vertex, texCoord))},
    }};
    vkCmdSetVertexInputEXT(cmd, 1, &vertexBinding, uint32_t(vertexAttributes.size()), vertexAttributes.data());

    // Input assembly.
    vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cmd, VK_FALSE);

    // Rasterization (most of these come from VK_EXT_extended_dynamic_state_3).
    vkCmdSetRasterizerDiscardEnable(cmd, VK_FALSE);
    vkCmdSetPolygonModeEXT(cmd, VK_POLYGON_MODE_FILL);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetDepthBiasEnable(cmd, VK_FALSE);
    vkCmdSetDepthClampEnableEXT(cmd, VK_FALSE);

    // Multisampling.
    vkCmdSetRasterizationSamplesEXT(cmd, VK_SAMPLE_COUNT_1_BIT);
    const VkSampleMask sampleMask = 0xFFFFFFFF;
    vkCmdSetSampleMaskEXT(cmd, VK_SAMPLE_COUNT_1_BIT, &sampleMask);
    vkCmdSetAlphaToCoverageEnableEXT(cmd, VK_FALSE);
    // alphaToOne is required by the spec when its device feature is enabled and a
    // shader object is bound, even if we don't actually use it.
    vkCmdSetAlphaToOneEnableEXT(cmd, VK_FALSE);

    // Depth / stencil.
    vkCmdSetDepthTestEnable(cmd, VK_TRUE);
    vkCmdSetDepthWriteEnable(cmd, VK_TRUE);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_LESS_OR_EQUAL);
    vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
    vkCmdSetStencilTestEnable(cmd, VK_FALSE);

    // Color blend (for one color attachment). Match the previous pipeline's
    // alpha-blend setup; nothing varies between draws so we set it once.
    const VkBool32                blendEnable = VK_TRUE;
    const VkColorBlendEquationEXT blendEquation{
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp        = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .alphaBlendOp        = VK_BLEND_OP_ADD,
    };
    const VkColorComponentFlags colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &blendEnable);
    vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &blendEquation);
    vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &colorWriteMask);
    vkCmdSetLogicOpEnableEXT(cmd, VK_FALSE);
  }

  /*--
   * Create the graphics shader objects (VK_EXT_shader_object).
   *
   * Shader objects replace VkPipeline for graphics in this sample. Three benefits:
   *   1. No pipeline state object: every state that used to live in a pipeline
   *      (vertex input, rasterization, depth/stencil, blend, ...) is now set
   *      dynamically before each draw via vkCmdSet*EXT.
   *   2. Pairs naturally with the layout = VK_NULL_HANDLE / descriptor-heap design:
   *      both eliminate "object that bakes state ahead of time" concerns.
   *   3. Mix-and-match at draw time: one VkShaderEXT per stage, swap freely.
   *
   * We create three unlinked shader objects:
   *   - vertex shader (shared)
   *   - fragment shader with useTexture=true
   *   - fragment shader with useTexture=false
   * "Unlinked" means each is independent; we bind the right (VS, FS) pair per draw.
   * For better cross-stage optimization, an alternative is to create *linked* sets
   * with VK_SHADER_CREATE_LINK_STAGE_BIT_EXT in a single vkCreateShadersEXT call.
   *
   * VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT is the shader-object equivalent of the
   * descriptor-heap pipeline flag we used before; setLayoutCount and pPushConstantRanges
   * stay zero/null for the same reason the pipeline layout was VK_NULL_HANDLE.
  -*/
  void createGraphicsShaders()
  {
#if USE_SLANG
    const char*                     vertEntryName = "vertexMain";
    const char*                     fragEntryName = "fragmentMain";
    const std::span<const uint32_t> vertCode{shader_rast_slang, std::size(shader_rast_slang)};
    const std::span<const uint32_t> fragCode{shader_rast_slang, std::size(shader_rast_slang)};  // Same module, different entry
#else
    const char*                     vertEntryName = "main";
    const char*                     fragEntryName = "main";
    const std::span<const uint32_t> vertCode{shader_vert_glsl, std::size(shader_vert_glsl)};
    const std::span<const uint32_t> fragCode{shader_frag_glsl, std::size(shader_frag_glsl)};
#endif

    /*--
     * Specialization constants. Two distinct VkSpecializationInfo instances, each
     * pointing at its own immutable VkBool32 -- the same pattern we used with
     * pipelines, equally valid for shader objects.
    -*/
    static constexpr VkBool32      kUseTextureTrue  = VK_TRUE;
    static constexpr VkBool32      kUseTextureFalse = VK_FALSE;
    const VkSpecializationMapEntry specMapEntry     = {.constantID = 0, .offset = 0, .size = sizeof(VkBool32)};
    const VkSpecializationInfo     specInfoTextured = {
            .mapEntryCount = 1,
            .pMapEntries   = &specMapEntry,
            .dataSize      = sizeof(VkBool32),
            .pData         = &kUseTextureTrue,
    };
    const VkSpecializationInfo specInfoNoTexture = {
        .mapEntryCount = 1,
        .pMapEntries   = &specMapEntry,
        .dataSize      = sizeof(VkBool32),
        .pData         = &kUseTextureFalse,
    };

    // Three unlinked shader objects. nextStage is a hint to the driver about
    // which stage will follow at bind time -- it doesn't constrain what we
    // can actually bind.
    const VkShaderCreateFlagsEXT commonFlags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;

    const VkShaderCreateInfoEXT vertCreateInfo{
        .sType                  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
        .flags                  = commonFlags,
        .stage                  = VK_SHADER_STAGE_VERTEX_BIT,
        .nextStage              = VK_SHADER_STAGE_FRAGMENT_BIT,
        .codeType               = VK_SHADER_CODE_TYPE_SPIRV_EXT,
        .codeSize               = vertCode.size() * sizeof(uint32_t),
        .pCode                  = vertCode.data(),
        .pName                  = vertEntryName,
        .setLayoutCount         = 0,  // Descriptor heap: no descriptor set layouts
        .pSetLayouts            = nullptr,
        .pushConstantRangeCount = 0,  // Push data (vkCmdPushDataEXT) is used instead
        .pPushConstantRanges    = nullptr,
        .pSpecializationInfo    = nullptr,  // Vertex shader has no spec constants
    };

    VkShaderCreateInfoEXT fragCreateInfoTextured{
        .sType                  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
        .flags                  = commonFlags,
        .stage                  = VK_SHADER_STAGE_FRAGMENT_BIT,
        .nextStage              = 0,  // Last stage in the pipeline
        .codeType               = VK_SHADER_CODE_TYPE_SPIRV_EXT,
        .codeSize               = fragCode.size() * sizeof(uint32_t),
        .pCode                  = fragCode.data(),
        .pName                  = fragEntryName,
        .setLayoutCount         = 0,
        .pSetLayouts            = nullptr,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges    = nullptr,
        .pSpecializationInfo    = &specInfoTextured,
    };

    VkShaderCreateInfoEXT fragCreateInfoNoTexture = fragCreateInfoTextured;
    fragCreateInfoNoTexture.pSpecializationInfo   = &specInfoNoTexture;

    // Create all three in separate calls. Each creates an unlinked shader object
    // that can be bound independently.
    VK_CHECK(vkCreateShadersEXT(m_context.getDevice(), 1, &vertCreateInfo, nullptr, &m_vertShader));
    DBG_VK_NAME(m_vertShader);
    VK_CHECK(vkCreateShadersEXT(m_context.getDevice(), 1, &fragCreateInfoTextured, nullptr, &m_fragShaderTextured));
    DBG_VK_NAME(m_fragShaderTextured);
    VK_CHECK(vkCreateShadersEXT(m_context.getDevice(), 1, &fragCreateInfoNoTexture, nullptr, &m_fragShaderNoTexture));
    DBG_VK_NAME(m_fragShaderNoTexture);
  }

  /*-- Initialize ImGui -*/
  void initImGui()
  {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForVulkan(m_window, true);
    static VkFormat           imageFormats[] = {m_swapchain.getImageFormat()};
    ImGui_ImplVulkan_InitInfo initInfo       = {
              .Instance       = m_context.getInstance(),
              .PhysicalDevice = m_context.getPhysicalDevice(),
              .Device         = m_context.getDevice(),
              .QueueFamily    = m_context.getGraphicsQueue().familyIndex,
              .Queue          = m_context.getGraphicsQueue().queue,
              .DescriptorPool = m_uiDescriptorPool,
              .MinImageCount  = 2,
              .ImageCount     = m_swapchain.getImageCount(),
              .PipelineInfoMain =
            {
                      .PipelineRenderingCreateInfo =  // Dynamic rendering
                {
                          .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                          .colorAttachmentCount    = 1,
                          .pColorAttachmentFormats = imageFormats,
                },
            },
              .PipelineInfoForViewports = initInfo.PipelineInfoMain,
              .UseDynamicRendering      = true,
    };

    ImGui_ImplVulkan_Init(&initInfo);

    ImGui::GetIO().ConfigFlags = ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_ViewportsEnable;
  }


  /*--
   * Create descriptor heap buffers for bindless resource access (VK_EXT_descriptor_heap).
   *
   * Instead of traditional descriptor sets and pools, the descriptor heap stores sampler and
   * image descriptors in GPU buffers. Shaders access them by index using layout(descriptor_heap).
   *
   * The heap is split into two parts:
   *   1. Sampler heap  -- holds sampler descriptors (linear, nearest, etc.)
   *   2. Resource heap  -- holds image descriptors (texture2D views)
   *
   * Each heap buffer has a "reserved range" at the end (required by the spec), followed by the
   * user descriptors at the beginning. The host staging data is written with
   * vkWriteSamplerDescriptorsEXT / vkWriteResourceDescriptorsEXT, then uploaded to GPU buffers.
   *
   * At draw time, vkCmdBindSamplerHeapEXT / vkCmdBindResourceHeapEXT bind these buffers.
  -*/
  void createDescriptorHeap(VkCommandBuffer cmd)
  {
    // Query the descriptor heap properties: descriptor sizes, alignment, and maximum heap capacities
    VkPhysicalDeviceDescriptorHeapPropertiesEXT heapProps{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &heapProps};
    vkGetPhysicalDeviceProperties2(m_context.getPhysicalDevice(), &props2);

    // Compute how many descriptors the hardware can hold, accounting for the mandatory reserved range
    uint32_t maxSamplerCapacity = static_cast<uint32_t>(
        (heapProps.maxSamplerHeapSize - heapProps.minSamplerHeapReservedRange) / heapProps.samplerDescriptorSize);
    uint32_t maxImageCapacity = static_cast<uint32_t>(
        (heapProps.maxResourceHeapSize - heapProps.minResourceHeapReservedRange) / heapProps.imageDescriptorSize);

    // All heap buffers need these usage flags: device address for binding, transfer dst for upload,
    // and the descriptor heap bit to mark them as heap storage
    VkBufferUsageFlags2 heapUsage = VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_2_TRANSFER_DST_BIT
                                    | VK_BUFFER_USAGE_2_DESCRIPTOR_HEAP_BIT_EXT;

    // ---- Sampler Heap ----
    // We only need one sampler in this sample (linear filtering), but the heap can hold more
    uint32_t maxSamplers = std::min(1U, maxSamplerCapacity);
    VkDeviceSize samplerHeapSize = heapProps.samplerDescriptorSize * maxSamplers + heapProps.minSamplerHeapReservedRange;
    samplerHeapSize = utils::alignUp(samplerHeapSize, heapProps.samplerHeapAlignment);
    std::vector<uint8_t> samplerHeapData(samplerHeapSize, 0);  // CPU staging buffer, zero-initialized

    // Write one sampler descriptor (linear filter, repeat addressing)
    VkSamplerCreateInfo samplerCI{
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_LINEAR,
        .minFilter    = VK_FILTER_LINEAR,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
        .maxLod       = VK_LOD_CLAMP_NONE,
    };
    // Using acquireSamplerDescriptor avoid any duplicate samplers in the heap.
    uint32_t samplerIndex = m_samplerPool.acquireSamplerDescriptor(samplerCI);

    // Point into the staging buffer at the correct offset for this sampler index
    VkHostAddressRangeEXT samplerDst{
        .address = samplerHeapData.data() + static_cast<size_t>(samplerIndex) * static_cast<size_t>(heapProps.samplerDescriptorSize),
        .size = heapProps.samplerDescriptorSize,
    };
    // Write the sampler descriptor into host memory (CPU staging)
    vkWriteSamplerDescriptorsEXT(m_context.getDevice(), 1, &samplerCI, &samplerDst);

    // Create the GPU buffer for the sampler heap and upload the staging data
    m_samplerHeapBuffer =
        m_allocator.createBufferAndUploadData(cmd, std::span(samplerHeapData), heapUsage, {}, heapProps.samplerHeapAlignment);
    DBG_VK_NAME(m_samplerHeapBuffer.buffer);

    // ---- Resource (Image) Heap ----
    // The resource heap holds image descriptors. We allocate enough slots for all application textures.
    uint32_t maxResources = std::min(m_maxTextures, maxImageCapacity);
    VkDeviceSize resourceHeapSize = heapProps.imageDescriptorSize * maxResources + heapProps.minResourceHeapReservedRange;
    resourceHeapSize = utils::alignUp(resourceHeapSize, heapProps.resourceHeapAlignment);
    std::vector<uint8_t> resourceHeapData(resourceHeapSize, 0);  // CPU staging buffer, zero-initialized

    // Build arrays of descriptor info for all images, then write them in a single batched call.
    // Each struct chain must stay alive until the write: ResourceDescriptorInfo -> ImageDescriptorInfo -> ImageViewCreateInfo.
    //
    // Contract: the array index of m_image[] *is* the heap slot index, *is*
    // shaderio::SceneInfo::texId. Adding a new texture means appending to
    // m_image[] -- no other place in the code needs to know the index.
    constexpr uint32_t                                  imageCount = uint32_t(std::size(decltype(m_image){}));
    std::array<VkImageViewCreateInfo, imageCount>       viewInfos{};
    std::array<VkImageDescriptorInfoEXT, imageCount>    imageDescInfos{};
    std::array<VkResourceDescriptorInfoEXT, imageCount> resInfos{};
    std::array<VkHostAddressRangeEXT, imageCount>       resDsts{};

    for(uint32_t i = 0; i < imageCount; i++)
    {
      viewInfos[i] = {
          .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
          .image            = m_image[i].image,
          .viewType         = VK_IMAGE_VIEW_TYPE_2D,
          .format           = m_image[i].format,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
      };

      imageDescInfos[i] = {
          .sType  = VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,
          .pView  = &viewInfos[i],
          .layout = m_image[i].layout,
      };

      resInfos[i] = {
          .sType = VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,
          .type  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
          .data  = {.pImage = &imageDescInfos[i]},
      };

      resDsts[i] = {
          .address = resourceHeapData.data() + static_cast<size_t>(i) * static_cast<size_t>(heapProps.imageDescriptorSize),
          .size = heapProps.imageDescriptorSize,
      };
    }

    // Single batched write — more efficient than one call per image, especially at scale.
    vkWriteResourceDescriptorsEXT(m_context.getDevice(), imageCount, resInfos.data(), resDsts.data());

    // Create the GPU buffer for the resource heap and upload the staging data
    m_resourceHeapBuffer = m_allocator.createBufferAndUploadData(cmd, std::span(resourceHeapData), heapUsage, {},
                                                                 heapProps.resourceHeapAlignment);
    DBG_VK_NAME(m_resourceHeapBuffer.buffer);

    // Store heap metadata needed later for vkCmdBindSamplerHeapEXT / vkCmdBindResourceHeapEXT.
    // The bind info describes: total heap size, where the reserved range starts, and its size.
    m_samplerHeapSize        = samplerHeapSize;
    m_resourceHeapSize       = resourceHeapSize;
    m_samplerReservedOffset  = heapProps.samplerDescriptorSize * maxSamplers;
    m_samplerReservedSize    = heapProps.minSamplerHeapReservedRange;
    m_resourceReservedOffset = heapProps.imageDescriptorSize * maxResources;
    m_resourceReservedSize   = heapProps.minResourceHeapReservedRange;
  }

  /*--
   * Create a descriptor pool for ImGui.
   * ImGui still uses traditional descriptor sets for its font textures and RenderTarget display textures.
   * The application's own textures/samplers use the descriptor heap instead (see createDescriptorHeap).
  -*/
  void createUIDescriptorPool()
  {
    VkPhysicalDeviceProperties2 deviceProperties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    vkGetPhysicalDeviceProperties2(m_context.getPhysicalDevice(), &deviceProperties2);
    const auto& deviceProperties = deviceProperties2.properties;

    // ImGui's Vulkan backend (1.92+) uses separate SAMPLED_IMAGE + SAMPLER descriptors:
    //   * One sampler set per built-in sampler (linear + nearest)
    //   * One sampled-image set per texture: font atlas (which may grow dynamically),
    //     RenderTarget display, and every ImGui_ImplVulkan_AddTexture caller.
    // IMGUI_IMPL_VULKAN_MINIMUM_*_POOL_SIZE define the absolute floors; we pick a
    // generous multiple so the sample has comfortable headroom for additional
    // ImTexture instances. All sizes are clamped to device limits to stay safe on
    // minimum-spec hardware.
    constexpr uint32_t kUiTextureBudget = 128;  // Max simultaneous ImGui textures
    const uint32_t     samplerSize =
        std::min<uint32_t>(IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE, deviceProperties.limits.maxDescriptorSetSamplers);
    const uint32_t sampledImageSize =
        std::min<uint32_t>(std::max<uint32_t>(IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE, kUiTextureBudget),
                           deviceProperties.limits.maxDescriptorSetSampledImages);
    const uint32_t maxDescriptorSets = samplerSize + sampledImageSize;

    const std::array<VkDescriptorPoolSize, 2> poolSizes = {{
        {VK_DESCRIPTOR_TYPE_SAMPLER, samplerSize},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sampledImageSize},
    }};
    const VkDescriptorPoolCreateInfo          poolInfo  = {
                  .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                  .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                  .maxSets       = maxDescriptorSets,
                  .poolSizeCount = uint32_t(poolSizes.size()),
                  .pPoolSizes    = poolSizes.data(),
    };

    VK_CHECK(vkCreateDescriptorPool(m_context.getDevice(), &poolInfo, nullptr, &m_uiDescriptorPool));
    DBG_VK_NAME(m_uiDescriptorPool);
    LOGI("Created UI descriptor pool: %u sets (sampler=%u, sampledImage=%u)", maxDescriptorSets, samplerSize, sampledImageSize);
  }


  /*--
   * Loading an image using the stb_image library.
   * Create an image and upload the data to the GPU.
   * Create an image view (Image view are for the shaders, to access the image).
  -*/
  utils::ImageResource loadAndCreateImage(VkCommandBuffer cmd, const std::string& filename)
  {
    // Load the image from disk
    int      w = 0, h = 0, comp = 0, req_comp{4};
    stbi_uc* data = stbi_load(filename.c_str(), &w, &h, &comp, req_comp);
    ASSERT(data != nullptr, "Could not load texture image!");
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    // Define how to create the image
    const VkImageCreateInfo imageInfo = {
        .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType   = VK_IMAGE_TYPE_2D,
        .format      = format,
        .extent      = {uint32_t(w), uint32_t(h), 1},
        .mipLevels   = 1,
        .arrayLayers = 1,
        .samples     = VK_SAMPLE_COUNT_1_BIT,
        .usage       = VK_IMAGE_USAGE_SAMPLED_BIT,
    };

    // Use the VMA allocator to create the image. createStagingBuffer copies the
    // pixels into a GPU-mapped staging buffer synchronously, so we can free the
    // stb_image allocation immediately after the call returns.
    const std::span dataSpan(data, w * h * 4);
    utils::ImageResource image = m_allocator.createImageAndUploadData(cmd, dataSpan, imageInfo, VK_IMAGE_LAYOUT_GENERAL);
    stbi_image_free(data);  // stb_image data is now in the staging buffer; free the host copy.
    utils::DebugUtil::getInstance().setObjectName(image.image, "Texture " + filename);
    image.extent = {uint32_t(w), uint32_t(h)};
    image.format = format;

    return image;
  }

  /*--
   * Creates the compute pipeline (traditional VkPipeline + VkPipelineLayout).
   *
   * Note the deliberate contrast with the graphics path:
   *   - Graphics uses VK_EXT_shader_object: no VkPipeline, no pipeline layout,
   *     descriptor heap access via VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT,
   *     per-draw data sent via vkCmdPushDataEXT.
   *   - Compute keeps the traditional model: a real VkPipelineLayout with a
   *     VkPushConstantRange, populated at dispatch time with vkCmdPushConstants2.
   *
   * Why compute is NOT migrated to shader objects, even though VK_EXT_shader_object
   * fully supports VK_SHADER_STAGE_COMPUTE_BIT:
   *   1. Kkeeping one path on the traditional pattern shows both styles side-by-side. 
   *      The traditional VkPipeline + VkPipelineLayout flow is still the most common 
   *      shape in production code and most docs.
   *   2. Compute does not benefit much from shader objects' main wins:
   *        * Many dynamic states -- compute has no rasterization, blend, depth,
   *          or vertex input state, so there is nothing to make dynamic.
   *        * Mix-and-match shaders -- a compute "pipeline" is one shader; there
   *          is nothing to swap.
   *        * Layout-less story -- graphics earns it via the descriptor heap;
   *          compute in this sample only needs a push-constant range, which
   *          a normal pipeline layout already expresses cleanly.
   *   3. Push constants stay tidier with a real layout: vkCmdPushConstants2 takes
   *      a VkPipelineLayout. With shader objects you would need Maintenance6 (or
   *      a separately created layout just for the push call) for zero gain here.
   *
   * If you want a reference for the shader-object compute path, the equivalent is:
   *
   *     VkShaderCreateInfoEXT info{
   *         .sType                  = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
   *         .stage                  = VK_SHADER_STAGE_COMPUTE_BIT,
   *         .codeType               = VK_SHADER_CODE_TYPE_SPIRV_EXT,
   *         .codeSize               = ...,
   *         .pCode                  = ...,
   *         .pName                  = "main",
   *         .pushConstantRangeCount = 1,
   *         .pPushConstantRanges    = &range,
   *     };
   *     vkCreateShadersEXT(device, 1, &info, nullptr, &m_computeShader);
   *     // bind:    vkCmdBindShadersEXT(cmd, 1, &VK_SHADER_STAGE_COMPUTE_BIT, &m_computeShader);
   *     // dispatch: vkCmdDispatch(...);
  -*/
  void createComputeShaderPipeline()
  {
    // Create the pipeline layout used by the compute shader
    const std::array<VkPushConstantRange, 1> pushRanges = {
        {{.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = sizeof(shaderio::PushConstantCompute)}}};

    // The pipeline layout is used to pass data to the pipeline, anything with "layout" in the shader
    const VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount         = 0,
        .pSetLayouts            = nullptr,
        .pushConstantRangeCount = uint32_t(pushRanges.size()),
        .pPushConstantRanges    = pushRanges.data(),
    };
    VK_CHECK(vkCreatePipelineLayout(m_context.getDevice(), &pipelineLayoutInfo, nullptr, &m_computePipelineLayout));
    DBG_VK_NAME(m_computePipelineLayout);

// Creating the pipeline to run the compute shader
#if USE_SLANG
    VkShaderModule compute = utils::createShaderModule(m_context.getDevice(), {shader_comp_slang, std::size(shader_comp_slang)});
#else
    VkShaderModule compute = utils::createShaderModule(m_context.getDevice(), {shader_comp_glsl, std::size(shader_comp_glsl)});
#endif
    DBG_VK_NAME(compute);

    /*-- 
     * Compute pipeline creation using VkPipelineCreateFlags2 (Maintenance5) 
    -*/
    VkPipelineCreateFlags2CreateInfo computeCreateFlags2{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,
        .flags = 0,  // No special flags needed for this simple compute pipeline
    };

    const std::array<VkComputePipelineCreateInfo, 1> pipelineInfo{{{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = &computeCreateFlags2,  // Use the modern flags2 structure
        .stage =
            {
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = compute,
                .pName  = "main",
            },
        .layout = m_computePipelineLayout,
    }}};
    VK_CHECK(vkCreateComputePipelines(m_context.getDevice(), {}, uint32_t(pipelineInfo.size()), pipelineInfo.data(),
                                      nullptr, &m_computePipeline));
    DBG_VK_NAME(m_computePipeline);

    // Clean up the shader module
    vkDestroyShaderModule(m_context.getDevice(), compute, nullptr);
  }

  /*---
   * Prepare frame resources - the first step in the rendering process.
   * It looks if the swapchain require rebuild, which happens when the window is resized.
   * It acquires the image from the swapchain to render into.
   * Returns true if we can proceed with rendering, false otherwise.
  -*/
  bool prepareFrameResources()
  {
    // Check if swapchain needs rebuilding (this internally calls vkQueueWaitIdle())
    if(m_swapchain.needRebuilding())
    {
      m_windowSize = m_swapchain.reinitResources(m_vSync);
    }

    // Wait first, *then* acquire. Waiting on the timeline semaphore guarantees the
    // GPU has released this slot's resources (command buffer, in-flight data) before
    // we start reusing them. Acquiring first would mean we hold a swapchain image
    // while still potentially racing the GPU on per-frame resources -- and in
    // out-of-order presentation, the wrong slot's wait value would be in scope.
    auto& frame = m_frameData[m_swapchain.getFrameResourceIndex()];

    // Wait until GPU has finished processing the frame that was using these resources previously
    // Note: If swapchain was rebuilt above, this wait is essentially a no-op since vkQueueWaitIdle() was already called
    const VkSemaphoreWaitInfo waitInfo = {
        .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount = 1,
        .pSemaphores    = &m_frameTimelineSemaphore,
        .pValues        = &frame.lastSignalValue,
    };
    VK_CHECK(vkWaitSemaphores(m_context.getDevice(), &waitInfo, std::numeric_limits<uint64_t>::max()));
#ifdef NVVK_SEMAPHORE_DEBUG
    LOGI("WaitFrame: \t\t slot=%u waitValue=%llu", m_swapchain.getFrameResourceIndex(),
         static_cast<unsigned long long>(frame.lastSignalValue));
#endif

    VkResult result = m_swapchain.acquireNextImage(m_context.getDevice());
    return (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR);  // Continue only if we got a valid image
  }

  /*---
   * Begin command buffer recording for the frame
   * It resets the command pool to reuse the command buffer for recording new rendering commands for the current frame.
   * Returns the command buffer for the frame.
  -*/
  VkCommandBuffer beginCommandRecording()
  {
    VkDevice device = m_context.getDevice();

    // Get the frame data for the current in-flight slot (owned by Swapchain).
    auto& frame = m_frameData[m_swapchain.getFrameResourceIndex()];

    /*--
     * Reset the whole command pool to reuse its command buffer for recording
     * the current frame. An equivalent alternative is to create the pool with
     * VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT and call
     * vkResetCommandBuffer() per buffer; whole-pool reset is simpler when each
     * pool only contains one buffer (as here).
    -*/
    VK_CHECK(vkResetCommandPool(device, frame.cmdPool, 0));
    VkCommandBuffer cmd = frame.cmdBuffer;

    // Begin the command buffer recording for the frame
    const VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                             .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

    return cmd;
  }

  /*--
   * End command buffer recording for the frame
  -*/
  void endCommandRecording(VkCommandBuffer cmd) { VK_CHECK(vkEndCommandBuffer(cmd)); }

  //--------------------------------------------------------------------------------------------------
  GLFWwindow* m_window{};  // The window

  utils::Context           m_context;          // The Vulkan context
  utils::ResourceAllocator m_allocator;        // The VMA allocator
  utils::Swapchain         m_swapchain;        // The swapchain
  utils::Buffer            m_vertexBuffer;     // The vertex buffer (two triangles) (SSBO)
  utils::Buffer            m_pointsBuffer;     // The data buffer (SSBO)
  utils::Buffer            m_sceneInfoBuffer;  // Scene info GPU buffer, updated once per frame, accessed via BDA
  // Loaded textures. The array index *is* the descriptor-heap slot index
  // (== shaderio::SceneInfo::texId). Adding a texture is a one-place change:
  // grow this array; createDescriptorHeap() picks up the new size automatically.
  utils::ImageResource m_image[2];
  utils::SamplerPool   m_samplerPool;  // The sampler pool, used to create a sampler for the texture

  utils::RenderTarget m_renderTarget;  // Offscreen color + depth target rendered into and shown via ImGui::Image
  utils::ImTexture    m_imTexture;     // The ImGui image wrapper for the render target's color view

  VkSurfaceKHR m_surface{};               // The window surface
  VkExtent2D   m_windowSize{800, 600};    // The window size
  VkExtent2D   m_viewportSize{800, 600};  // The viewport area in the window

  // Graphics: no pipeline / no pipeline layout. We use VK_EXT_shader_object instead.
  VkShaderEXT m_vertShader{};           // Shared vertex shader
  VkShaderEXT m_fragShaderTextured{};   // Fragment shader, useTexture spec const = TRUE
  VkShaderEXT m_fragShaderNoTexture{};  // Fragment shader, useTexture spec const = FALSE

  // Compute: traditional pipeline + layout (no descriptor heap access in compute).
  VkPipelineLayout m_computePipelineLayout{};
  VkPipeline       m_computePipeline{};
  VkCommandPool    m_transientCmdPool{};
  VkDescriptorPool m_uiDescriptorPool{};  // ImGui descriptor pool (ImGui still uses traditional descriptor sets)

  // Descriptor heap (VK_EXT_descriptor_heap): replaces traditional descriptor sets for textures/samplers.
  // GPU buffers holding sampler and image descriptors, bound per-frame via vkCmdBind*HeapEXT.
  utils::Buffer m_samplerHeapBuffer{};   // GPU buffer holding sampler descriptors
  utils::Buffer m_resourceHeapBuffer{};  // GPU buffer holding image (resource) descriptors
  // Heap metadata needed for VkBindHeapInfoEXT at draw time
  VkDeviceSize m_samplerHeapSize{};         // Total aligned size of the sampler heap buffer
  VkDeviceSize m_resourceHeapSize{};        // Total aligned size of the resource heap buffer
  VkDeviceSize m_samplerReservedOffset{};   // Byte offset where the sampler reserved range starts
  VkDeviceSize m_samplerReservedSize{};     // Size of the mandatory sampler reserved range
  VkDeviceSize m_resourceReservedOffset{};  // Byte offset where the resource reserved range starts
  VkDeviceSize m_resourceReservedSize{};    // Size of the mandatory resource reserved range


  // Frame resources and synchronization
  struct FrameData
  {
    VkCommandPool   cmdPool;          // Command pool for recording commands for this frame
    VkCommandBuffer cmdBuffer;        // Command buffer containing the frame's rendering commands
    uint64_t        lastSignalValue;  // Timeline value last signaled for this frame's resources
  };
  std::vector<FrameData> m_frameData;      // Collection of per-frame resources to support multiple frames in flight
  VkSemaphore m_frameTimelineSemaphore{};  // Timeline semaphore used to synchronize CPU submission with GPU completion
  uint64_t    m_frameCounter{1};           // Monotonic timeline counter (increments each frame)
  // The swapchain owns the in-flight slot index; query it via m_swapchain.getFrameResourceIndex().
  utils::FramePacer m_framePacer;  // Utility to pace the frame rate

  bool              m_vSync{true};                           // VSync on or off
  int               m_imageID{0};                            // The current image to display
  uint32_t          m_maxTextures{10000};                    // Maximum textures allowed in the application
  VkClearColorValue m_clearColor{{0.2f, 0.2f, 0.3f, 1.0f}};  // The clear color
};

//--- Main ---------------------------------------------------------------------------------------------------------------
int main()
{
  // Get the logger instance
  utils::Logger& logger = utils::Logger::getInstance();
  // logger.enableFileOutput(false);  // Don't write log to file
  logger.setShowFlags(utils::Logger::eSHOW_TIME);
  logger.setLogLevel(utils::Logger::LogLevel::eINFO);  // Default is Warning, we show more information
  LOGI("Starting ... ");

  try
  {
    ASSERT(glfwInit() == GLFW_TRUE, "Could not initialize GLFW!");
    ASSERT(glfwVulkanSupported() == GLFW_TRUE, "GLFW: Vulkan not supported!");

    MinimalLatest app({800, 600});
    app.run();

    glfwTerminate();
  }
  catch(const std::exception& e)
  {
    LOGE("%s", e.what());
    return 1;
  }
  return 0;
}