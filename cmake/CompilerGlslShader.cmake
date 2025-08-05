# This function compiles GLSL shaders into C++ headers using glslangValidator

#   # List of shaders
#   set(SHADER_FILES
#     ${CMAKE_SOURCE_DIR}/shaders/shader1.glsl
#     ${CMAKE_SOURCE_DIR}/shaders/shader2.glsl
#   )
#   
#   # Define the output directory for generated headers
#   set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/_autogen")
#   
#   # Call the function to compile shaders
#   compile_glsl(
#     "${SHADER_FILES}"
#     "${SHADER_OUTPUT_DIR}"
#     GENERATED_SHADER_HEADERS
# ...
#     # Optional arguments
#     TARGET_ENV "vulkan1.1"
#     EXTRA_FLAGS "-I<dir>"
#   )
#   
#   # Use the generated headers as needed (example: link them to a target)
#   target_sources(MyProg PRIVATE ${GENERATED_SHADER_HEADERS})



function(compile_glsl SHADER_FILES OUTPUT_DIR SHADER_HEADERS_VAR)
  # Optional arguments for flexibility
  set(options )
  set(oneValueArgs TARGET_ENV)
  set(multiValueArgs EXTRA_FLAGS)
  cmake_parse_arguments(COMPILE_SHADER "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # Set defaults for optional arguments
  set(TARGET_ENV ${COMPILE_SHADER_TARGET_ENV})
  if(NOT TARGET_ENV)
    set(TARGET_ENV "vulkan1.3")
  endif()
  set(EXTRA_FLAGS ${COMPILE_SHADER_EXTRA_FLAGS})

  # Ensure the output directory exists
  file(MAKE_DIRECTORY ${OUTPUT_DIR})

  # Initialize the output variable
  set(SHADER_HEADERS "")
  set(SHADER_SPVS "")

  # Iterate over shader files
  foreach(SHADER ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    string(REPLACE "." "_" VN_SHADER_NAME ${SHADER_NAME})
    set(OUTPUT_HEADER "${OUTPUT_DIR}/${SHADER_NAME}.h")
    set(OUTPUT_SPV "${OUTPUT_DIR}/${SHADER_NAME}.spv")

    # Commands for header and SPIR-V
    set(COMMAND_HEADER
      ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
      -g # -gVS
      -D__GLSL__
      --target-env ${TARGET_ENV}
      --vn ${VN_SHADER_NAME}
      -o ${OUTPUT_HEADER}
      ${SHADER}
      ${EXTRA_FLAGS}
    )
    set(COMMAND_SPV
      ${Vulkan_GLSLANG_VALIDATOR_EXECUTABLE}
      -g
      -D__GLSL__
      --target-env ${TARGET_ENV}
      -o ${OUTPUT_SPV}
      ${SHADER}
      ${EXTRA_FLAGS}
    )

    # Add custom commands
    add_custom_command(
      # PRE_BUILD
      OUTPUT ${OUTPUT_HEADER} ${OUTPUT_SPV}
      COMMAND echo ${COMMAND_HEADER}
      COMMAND ${COMMAND_HEADER}
      COMMAND ${COMMAND_SPV}
      MAIN_DEPENDENCY ${SHADER}
    )



    list(APPEND SHADER_HEADERS ${OUTPUT_HEADER})
    list(APPEND SHADER_SPVS ${OUTPUT_SPV})
  endforeach()

  # Set the output variables for the caller
  set(${SHADER_HEADERS_VAR} ${SHADER_HEADERS} PARENT_SCOPE)
  set(${SHADER_SPVS_VAR} ${SHADER_SPVS} PARENT_SCOPE)
endfunction()

