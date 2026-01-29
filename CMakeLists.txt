cmake_minimum_required(VERSION 3.21)
set(MYPROJ vk_minimal_latest)
project(${MYPROJ} VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

option(VK_SEMAPHORE_DEBUG "Enable semaphore debugging logs" OFF)

# Use FetchContent for faster dependency management
include(FetchContent)

# Find Vulkan
find_package(Vulkan REQUIRED)

# Add cmake modules directory to module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Include shader compilation functions
include(CompilerSlangShader)
include(CompilerGlslShader)
include(DownloadPackage)

# Find or download Slang compiler
set(Slang_VERSION "2026.1")
find_package(Slang REQUIRED)

# Add the executable
add_executable(${MYPROJ} 
    src/minimal_latest.cpp 
    src/debug_util.h
    src/logger.h
    )

if(VK_SEMAPHORE_DEBUG)
  target_compile_definitions(${MYPROJ} PRIVATE NVVK_SEMAPHORE_DEBUG=1)
endif()

# Make Visual Studio using this project as the startup project
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${MYPROJ})

# Set warning level 3 for MSVC
if(MSVC)
  target_compile_options(${MYPROJ} PRIVATE /W4)
endif()

# Include GLFW
message(STATUS "Fetching GLFW...")
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
)
set(GLFW_BUILD_DOCS OFF)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_WAYLAND OFF)
set(GLFW_INSTALL OFF)
FetchContent_MakeAvailable(glfw)
message(STATUS "GLFW ready at: ${glfw_SOURCE_DIR}")

# Include GLM
message(STATUS "Fetching GLM...")
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG master
)
FetchContent_MakeAvailable(glm)
message(STATUS "GLM ready at: ${glm_SOURCE_DIR}")
set_property(TARGET glm PROPERTY FOLDER "ThirdParty")

# Include Volk
message(STATUS "Fetching Volk...")
FetchContent_Declare(
  volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG master
)
FetchContent_MakeAvailable(volk)
message(STATUS "Volk ready at: ${volk_SOURCE_DIR}")
set_property(TARGET volk PROPERTY FOLDER "ThirdParty")

# Include VMA
message(STATUS "Fetching VulkanMemoryAllocator...")
FetchContent_Declare(
  VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG master
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)
message(STATUS "VulkanMemoryAllocator ready at: ${vulkanmemoryallocator_SOURCE_DIR}")
set(VMA_SOURCE ${vulkanmemoryallocator_SOURCE_DIR}/include/vk_mem_alloc.h)
target_include_directories(${MYPROJ} PRIVATE ${vulkanmemoryallocator_SOURCE_DIR}/include)
target_sources(${MYPROJ} PRIVATE ${VMA_SOURCE})
source_group("vma" FILES ${VMA_SOURCE})

# Include ImGui
message(STATUS "Fetching ImGui (docking branch)...")
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)
message(STATUS "ImGui ready at: ${imgui_SOURCE_DIR}")
set(IMGUI_SOURCES 
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui.h
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.h
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.h
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR})
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)
target_link_libraries(imgui PUBLIC volk glfw)
set_property(TARGET imgui PROPERTY FOLDER "ThirdParty")

# Download stb_image.h
set(STB_IMAGE_URL "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h")
set(STB_IMAGE_PATH "${CMAKE_BINARY_DIR}/stb_image.h")
if(NOT EXISTS ${STB_IMAGE_PATH})
    message(STATUS "Downloading stb_image.h...")
    file(DOWNLOAD ${STB_IMAGE_URL} ${STB_IMAGE_PATH})
endif()
target_sources(${MYPROJ} PRIVATE ${STB_IMAGE_PATH})
source_group("stb" FILES ${STB_IMAGE_PATH})
target_include_directories(${MYPROJ} PRIVATE "${CMAKE_BINARY_DIR}")

# Find system libraries needed by GLFW on Linux
if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  find_package(Threads REQUIRED)
endif()

# Link libraries and include directories
target_link_libraries(${MYPROJ} PRIVATE
  glfw
  volk
  imgui
  glm
)

# Link system libraries on Linux
if(UNIX AND NOT APPLE)
  target_link_libraries(${MYPROJ} PRIVATE ${X11_LIBRARIES} Threads::Threads)
endif()

# On Windows, link with the Windows Multimedia .lib file so we can increase
# timer precision using timeBeginPeriod(). This only uses kernelbase.dll.
if(WIN32)
  target_link_libraries(${MYPROJ} PRIVATE winmm.lib)
endif()


target_include_directories(${MYPROJ} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Define VK_NO_PROTOTYPES to avoid including Vulkan prototypes
# This is necessary because we are using volk to load Vulkan functions
target_compile_definitions(${MYPROJ} PRIVATE VK_NO_PROTOTYPES)

# Option to use Slang shaders instead of GLSL (default: ON)
option(USE_SLANG "Use Slang shaders instead of GLSL" ON)
if(USE_SLANG)
  target_compile_definitions(${MYPROJ} PRIVATE USE_SLANG)
endif()


#------------------------------------------------------------------------------------------------------------------------------
# Compile shaders
file(GLOB SHADER_SLANG_FILES "shaders/*.slang")
file(GLOB SHADER_GLSL_FILES "shaders/*.glsl")
file(GLOB SHADER_H_FILES "shaders/*.h")

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_LIST_DIR}/_autogen")

compile_slang(
  "${SHADER_SLANG_FILES}"
  "${SHADER_OUTPUT_DIR}"
  GENERATED_SHADER_SLANG_HEADERS
  EXTRA_FLAGS "-I${CMAKE_CURRENT_LIST_DIR}"
)

compile_glsl(
  "${SHADER_GLSL_FILES}"
  "${SHADER_OUTPUT_DIR}"
  GENERATED_SHADER_GLSL_HEADERS
  EXTRA_FLAGS "-I${CMAKE_CURRENT_LIST_DIR}"
)


# Add shader files to the project
source_group("Shaders" FILES ${SHADER_SLANG_FILES} ${SHADER_GLSL_FILES} ${SHADER_H_FILES})
source_group("Shaders/Compiled" FILES ${GENERATED_SHADER_SLANG_HEADERS} ${GENERATED_SHADER_GLSL_HEADERS})

# Add the output shader headers (target) directly to the executable
# This allows to compile the shaders when the executable is built
target_sources(${MYPROJ} PRIVATE ${GENERATED_SHADER_SLANG_HEADERS} ${GENERATED_SHADER_GLSL_HEADERS} ${SHADER_H_FILES})


# Include the shader and _autogen directory
target_include_directories(${MYPROJ} PRIVATE "${CMAKE_BINARY_DIR}")
target_include_directories(${MYPROJ} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")


# Copy .jpg image next to the executable
file(GLOB IMAGE_FILE "${CMAKE_SOURCE_DIR}/resources/*.jpg")
add_custom_command(
  TARGET ${MYPROJ} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  ${IMAGE_FILE} $<TARGET_FILE_DIR:${MYPROJ}>
)

# Set a default installation prefix
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/_install" CACHE PATH "Default install path" FORCE)

# Define the installation directory for the executable
install(TARGETS ${MYPROJ}
    RUNTIME DESTINATION bin
)

# Installing also the image
install(FILES ${IMAGE_FILE}
    DESTINATION bin
)
