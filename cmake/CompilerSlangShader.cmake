#==================================================================================================
# compile_slang: Compile Slang shaders into C++ headers and SPIR-V binaries
#==================================================================================================
#
# This function compiles Slang shader files using slangc, generating:
#   - Embedded C++ headers (.h) with SPIR-V code as text
#   - Binary SPIR-V files (.spv)
#   - Dependency files (.dep) for incremental builds
#
# Basic usage:
#
#   set(SHADER_FILES
#     ${CMAKE_SOURCE_DIR}/shaders/shader1.slang
#     ${CMAKE_SOURCE_DIR}/shaders/shader2.slang
#   )
#
#   compile_slang(
#     "${SHADER_FILES}"              # List of shader files to compile
#     "${CMAKE_BINARY_DIR}/_autogen" # Output directory for generated files
#     HEADERS_VAR GENERATED_SHADER_HEADERS
#   )
#
#   target_sources(MyProject PRIVATE ${GENERATED_SHADER_HEADERS})
#
#==================================================================================================
# Optional arguments:
#==================================================================================================
#
# Compiler target & language:
#   LANGUAGE <lang>                - Source language: slang, glsl, hlsl (default: slang)
#   PROFILE <profile>              - Target profile (default: spirv_1_6)
#   TARGET <target>                - Code generation target (default: spirv)
#
# Capabilities:
#   CAPABILITIES <cap1> <cap2>...  - SPIR-V capabilities (joined with + for slangc)
#
# Build settings:
#   DEBUG_LEVEL <0-3>              - Debug information level (default: 1)
#                                      0 = no debug info
#                                      1 = embeds shader source for Nsight and other tools
#                                      2 = standard debug info
#                                      3 = full debug info
#   OPTIMIZATION_LEVEL <0-3>       - Optimization level (default: 0)
#                                      0 = no optimization (faster builds; also avoids spirv-opt
#                                          bugs that have broken some of our shaders)
#                                      1 = minimal optimization
#                                      2 = default optimization
#                                      3 = aggressive optimization
#
# Output variables:
#   HEADERS_VAR <variable_name>    - Variable to receive list of generated .h files
#   SPVS_VAR <variable_name>       - Variable to receive list of generated .spv files
#   At least one must be provided; output type is inferred from which are present.
#
# Other options:
#   NAME_SUFFIX <suffix>           - Append suffix to output filenames
#   EXTRA_FLAGS <flags...>         - Additional raw compiler flags (e.g. -I, -D)
#   VERBOSE ON|OFF                 - Display full command lines (default: OFF)
#   SHOW_TIMING ON|OFF             - Display compilation time per shader (default: OFF)
#
# Advanced examples:
#
#   # Generate both headers and SPIR-V binaries with capabilities
#   compile_slang(
#     "${SHADER_FILES}"
#     "${OUTPUT_DIR}"
#     HEADERS_VAR MY_HEADERS
#     SPVS_VAR MY_SPVS
#     CAPABILITIES spvInt64Atomics                     # 64-bit atomic operations
#                  spvShaderClockKHR                   # Shader clock for profiling
#                  spvRayTracingMotionBlurNV           # Motion blur for ray tracing
#                  spvRayQueryKHR                      # Ray query operations
#                  SPV_KHR_compute_shader_derivatives  # Derivatives in compute shaders
#   )
#
#   # Generate only SPIR-V binaries (no embedded headers)
#   compile_slang(
#     "${SHADER_FILES}"
#     "${OUTPUT_DIR}"
#     SPVS_VAR MY_SPVS
#   )
#
#   # Debug mode: show full commands and timing
#   compile_slang(
#     "${SHADER_FILES}"
#     "${OUTPUT_DIR}"
#     HEADERS_VAR MY_HEADERS
#     VERBOSE ON
#     SHOW_TIMING ON
#   )
#
#==================================================================================================

option(COMPILE_SHADER_VERBOSE "Show full Slang compiler command lines" OFF)
option(COMPILE_SHADER_SHOW_TIMING "Show shader compilation timing" OFF)


function(compile_slang SHADER_FILES OUTPUT_DIR)

  # Validate slangc
  if(NOT Slang_SLANGC_EXECUTABLE)
    message(FATAL_ERROR "compile_slang: Slang_SLANGC_EXECUTABLE is not defined. "
                        "Make sure FindSlang.cmake has been included and Slang package was found.")
  endif()

  if(NOT EXISTS "${Slang_SLANGC_EXECUTABLE}")
    message(FATAL_ERROR "compile_slang: Slang compiler not found at: ${Slang_SLANGC_EXECUTABLE}")
  endif()

  # Nothing to do if no shader files provided
  if(NOT DEFINED SHADER_FILES OR "${SHADER_FILES}" STREQUAL "")
    return()
  endif()

  #------------------------------------------------------------------------------------------------
  # Parse arguments
  #------------------------------------------------------------------------------------------------

  set(oneValueArgs
    LANGUAGE
    PROFILE
    TARGET
    DEBUG_LEVEL
    OPTIMIZATION_LEVEL
    HEADERS_VAR
    SPVS_VAR
    NAME_SUFFIX
    VERBOSE
    SHOW_TIMING
  )

  set(multiValueArgs
    CAPABILITIES
    EXTRA_FLAGS
  )

  cmake_parse_arguments(COMPILE_SHADER "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # Legacy compatibility: detect positional SHADER_HEADERS_VAR (third argument).
  # Old form: compile_slang("${FILES}" "${DIR}" GENERATED_HEADERS ...)
  # New form: compile_slang("${FILES}" "${DIR}" HEADERS_VAR GENERATED_HEADERS ...)
  if(COMPILE_SHADER_UNPARSED_ARGUMENTS AND NOT DEFINED COMPILE_SHADER_HEADERS_VAR)
    list(GET COMPILE_SHADER_UNPARSED_ARGUMENTS 0 _LEGACY_HEADERS_VAR)
    set(COMPILE_SHADER_HEADERS_VAR "${_LEGACY_HEADERS_VAR}")
    message(AUTHOR_WARNING
      "compile_slang: Passing the headers variable as a positional argument is deprecated. "
      "Use the named argument instead:\n"
      "  HEADERS_VAR ${_LEGACY_HEADERS_VAR}")
  endif()

  #------------------------------------------------------------------------------------------------
  # Validation
  #------------------------------------------------------------------------------------------------

  if(NOT DEFINED OUTPUT_DIR OR "${OUTPUT_DIR}" STREQUAL "")
    message(FATAL_ERROR "compile_slang: OUTPUT_DIR is not defined or empty.")
  endif()

  if(EXISTS "${OUTPUT_DIR}" AND NOT IS_DIRECTORY "${OUTPUT_DIR}")
    message(FATAL_ERROR "compile_slang: OUTPUT_DIR '${OUTPUT_DIR}' exists but is not a directory.")
  endif()

  # Determine what to generate based on which output variables were provided
  set(_GENERATE_HEADER FALSE)
  set(_GENERATE_SPV FALSE)

  if(DEFINED COMPILE_SHADER_HEADERS_VAR AND NOT "${COMPILE_SHADER_HEADERS_VAR}" STREQUAL "")
    set(_GENERATE_HEADER TRUE)
  endif()

  if(DEFINED COMPILE_SHADER_SPVS_VAR AND NOT "${COMPILE_SHADER_SPVS_VAR}" STREQUAL "")
    set(_GENERATE_SPV TRUE)
  endif()

  if(NOT _GENERATE_HEADER AND NOT _GENERATE_SPV)
    message(FATAL_ERROR "compile_slang: At least one of HEADERS_VAR or SPVS_VAR must be provided.")
  endif()

  foreach(_shader_file ${SHADER_FILES})
    if(NOT EXISTS "${_shader_file}")
      message(FATAL_ERROR "compile_slang: Shader file not found: ${_shader_file}")
    endif()
  endforeach()

  #------------------------------------------------------------------------------------------------
  # Defaults
  #------------------------------------------------------------------------------------------------

  if(NOT DEFINED COMPILE_SHADER_LANGUAGE)
    set(COMPILE_SHADER_LANGUAGE "slang")
  endif()

  if(NOT DEFINED COMPILE_SHADER_PROFILE)
    set(COMPILE_SHADER_PROFILE "spirv_1_6")
  endif()

  if(NOT DEFINED COMPILE_SHADER_TARGET)
    set(COMPILE_SHADER_TARGET "spirv")
  endif()

  # Default to debug info level 1, which embeds shader source for Nsight and other tools
  if(NOT DEFINED COMPILE_SHADER_DEBUG_LEVEL)
    set(COMPILE_SHADER_DEBUG_LEVEL 1)
  endif()

  # Default to no optimization for faster builds, and to avoid spirv-opt bugs that have
  # broken some of our shaders
  if(NOT DEFINED COMPILE_SHADER_OPTIMIZATION_LEVEL)
    set(COMPILE_SHADER_OPTIMIZATION_LEVEL 0)
  endif()

  # Join capabilities list with "+" for slangc -capability flag
  string(REPLACE ";" "+" COMPILE_SHADER_CAPABILITIES "${COMPILE_SHADER_CAPABILITIES}")

  #------------------------------------------------------------------------------------------------
  # Build compiler flags
  #------------------------------------------------------------------------------------------------

  file(MAKE_DIRECTORY "${OUTPUT_DIR}")

  set(SHADER_HEADERS "")
  set(SHADER_SPVS "")

  set(_CORE_FLAGS
    -emit-spirv-directly
    -matrix-layout-row-major
    -force-glsl-scalar-layout
    -fvk-use-entrypoint-name
  )

  set(_CONFIGURABLE_FLAGS
    -lang ${COMPILE_SHADER_LANGUAGE}
    -profile ${COMPILE_SHADER_PROFILE}
    -target ${COMPILE_SHADER_TARGET}
  )

  set(_OPTIONAL_FLAGS "")
  if(COMPILE_SHADER_CAPABILITIES)
    list(APPEND _OPTIONAL_FLAGS -capability ${COMPILE_SHADER_CAPABILITIES})
  endif()

  set(_BUILD_FLAGS
    -g${COMPILE_SHADER_DEBUG_LEVEL}
    -O${COMPILE_SHADER_OPTIMIZATION_LEVEL}
  )

  # User flags listed last so they can override earlier settings
  set(_SLANG_FLAGS
    ${_CORE_FLAGS}
    ${_CONFIGURABLE_FLAGS}
    ${_OPTIONAL_FLAGS}
    ${_BUILD_FLAGS}
    ${COMPILE_SHADER_EXTRA_FLAGS}
  )

  #------------------------------------------------------------------------------------------------
  # Process each shader file
  #------------------------------------------------------------------------------------------------

  foreach(SHADER ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    string(APPEND SHADER_NAME "${COMPILE_SHADER_NAME_SUFFIX}")

    # Generate valid C/C++ variable name (e.g. "shader.comp.slang" -> "shader_comp_slang")
    string(REPLACE "." "_" _EMBED_NAME ${SHADER_NAME})

    set(OUTPUT_FILE ${OUTPUT_DIR}/${SHADER_NAME})
    set(OUTPUT_H_FILE "${OUTPUT_FILE}.h")
    set(OUTPUT_SPV_FILE "${OUTPUT_FILE}.spv")
    set(OUTPUT_DEP_FILE "${OUTPUT_FILE}.dep")

    set(_COMMANDS)
    set(_OUTPUT_FILES)
    set(_DEPFILE_ADDED FALSE)

    # Generate embedded header (.h)
    if(_GENERATE_HEADER)
      set(_COMMAND_H ${Slang_SLANGC_EXECUTABLE}
        -depfile "${OUTPUT_DEP_FILE}"
        -source-embed-name ${_EMBED_NAME}
        -source-embed-style text
        ${_SLANG_FLAGS}
        -o "${OUTPUT_H_FILE}"
        ${SHADER}
      )
      set(_DEPFILE_ADDED TRUE)

      list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E echo "  Generating header: ${SHADER_NAME}.h")
      if(COMPILE_SHADER_VERBOSE)
        string(REPLACE ";" " " _COMMAND_H_STR "${_COMMAND_H}")
        list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E echo "  ${_COMMAND_H_STR}")
      endif()
      if(COMPILE_SHADER_SHOW_TIMING)
        list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E time ${_COMMAND_H})
      else()
        list(APPEND _COMMANDS COMMAND ${_COMMAND_H})
      endif()

      list(APPEND _OUTPUT_FILES "${OUTPUT_H_FILE}")
      list(APPEND SHADER_HEADERS "${OUTPUT_H_FILE}")
    endif()

    # Generate SPIR-V binary (.spv)
    if(_GENERATE_SPV)
      set(_COMMAND_S ${Slang_SLANGC_EXECUTABLE}
        ${_SLANG_FLAGS}
        -o "${OUTPUT_SPV_FILE}"
        ${SHADER}
      )
      if(NOT _DEPFILE_ADDED)
        list(INSERT _COMMAND_S 1 -depfile "${OUTPUT_DEP_FILE}")
      endif()

      list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E echo "  Generating SPIR-V: ${SHADER_NAME}.spv")
      if(COMPILE_SHADER_VERBOSE)
        string(REPLACE ";" " " _COMMAND_S_STR "${_COMMAND_S}")
        list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E echo "  ${_COMMAND_S_STR}")
      endif()
      if(COMPILE_SHADER_SHOW_TIMING)
        list(APPEND _COMMANDS COMMAND ${CMAKE_COMMAND} -E time ${_COMMAND_S})
      else()
        list(APPEND _COMMANDS COMMAND ${_COMMAND_S})
      endif()

      list(APPEND _OUTPUT_FILES "${OUTPUT_SPV_FILE}")
      list(APPEND SHADER_SPVS "${OUTPUT_SPV_FILE}")
    endif()

    # Single custom command per shader: all outputs built together so that
    # VS right-click "Compile" on a .slang file produces everything.
    # Cross-shader parallelism is still achieved since each shader has its own command.
    if(_OUTPUT_FILES)
      add_custom_command(
        OUTPUT ${_OUTPUT_FILES}
        ${_COMMANDS}
        MAIN_DEPENDENCY ${SHADER}
        DEPFILE "${OUTPUT_DEP_FILE}"
        COMMENT "Compiling Slang shader: ${SHADER_NAME}"
        VERBATIM
      )
    endif()

  endforeach()

  #------------------------------------------------------------------------------------------------
  # Export output variables to caller scope
  #------------------------------------------------------------------------------------------------

  if(_GENERATE_HEADER)
    set(${COMPILE_SHADER_HEADERS_VAR} ${SHADER_HEADERS} PARENT_SCOPE)
  endif()

  if(_GENERATE_SPV)
    set(${COMPILE_SHADER_SPVS_VAR} ${SHADER_SPVS} PARENT_SCOPE)
  endif()

endfunction()
