set(airelinna_spirv_directory
  "${PROJECT_BINARY_DIR}/generated/spirv")
set(airelinna_generated_render_directory
  "${PROJECT_BINARY_DIR}/generated/airelinna/render")

set(airelinna_replica_vertex_spirv
  "${airelinna_spirv_directory}/replica.vert.spv")
set(airelinna_replica_fragment_spirv
  "${airelinna_spirv_directory}/replica.frag.spv")
set(airelinna_replica_shader_header
  "${airelinna_generated_render_directory}/replica_shaders.hpp")
set(airelinna_replica_shader_source
  "${airelinna_generated_render_directory}/replica_shaders.cpp")

add_custom_command(
  OUTPUT "${airelinna_replica_vertex_spirv}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${airelinna_spirv_directory}"
  COMMAND Vulkan::glslc
    --target-env=vulkan1.3
    -fshader-stage=vert
    -o "${airelinna_replica_vertex_spirv}"
    "${CMAKE_CURRENT_SOURCE_DIR}/replica.vert"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/replica.vert"
    Vulkan::glslc
  COMMENT "Compiling replica.vert to SPIR-V"
  VERBATIM
)

add_custom_command(
  OUTPUT "${airelinna_replica_fragment_spirv}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${airelinna_spirv_directory}"
  COMMAND Vulkan::glslc
    --target-env=vulkan1.3
    -fshader-stage=frag
    -o "${airelinna_replica_fragment_spirv}"
    "${CMAKE_CURRENT_SOURCE_DIR}/replica.frag"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/replica.frag"
    Vulkan::glslc
  COMMENT "Compiling replica.frag to SPIR-V"
  VERBATIM
)

add_custom_command(
  OUTPUT
    "${airelinna_replica_shader_header}"
    "${airelinna_replica_shader_source}"
  COMMAND "${CMAKE_COMMAND}"
    "-DAIRELINNA_VERTEX_SPIRV=${airelinna_replica_vertex_spirv}"
    "-DAIRELINNA_FRAGMENT_SPIRV=${airelinna_replica_fragment_spirv}"
    "-DAIRELINNA_OUTPUT_HEADER=${airelinna_replica_shader_header}"
    "-DAIRELINNA_OUTPUT_SOURCE=${airelinna_replica_shader_source}"
    -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
  DEPENDS
    "${airelinna_replica_vertex_spirv}"
    "${airelinna_replica_fragment_spirv}"
    "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
  COMMENT "Embedding replica SPIR-V as C++ arrays"
  VERBATIM
)

add_custom_target(airelinna_replica_shaders
  DEPENDS
    "${airelinna_replica_shader_header}"
    "${airelinna_replica_shader_source}"
)

set(AIRELINNA_REPLICA_SHADER_HEADER
  "${airelinna_replica_shader_header}" PARENT_SCOPE)
set(AIRELINNA_REPLICA_SHADER_SOURCE
  "${airelinna_replica_shader_source}" PARENT_SCOPE)
