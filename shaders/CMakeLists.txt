set(avb0795_spirv_directory
  "${PROJECT_BINARY_DIR}/generated/spirv")
set(avb0795_generated_render_directory
  "${PROJECT_BINARY_DIR}/generated/avb/render")

set(avb0795_replica_vertex_spirv
  "${avb0795_spirv_directory}/replica.vert.spv")
set(avb0795_replica_fragment_spirv
  "${avb0795_spirv_directory}/replica.frag.spv")
set(avb0795_replica_shader_header
  "${avb0795_generated_render_directory}/replica_shaders.hpp")
set(avb0795_replica_shader_source
  "${avb0795_generated_render_directory}/replica_shaders.cpp")

add_custom_command(
  OUTPUT "${avb0795_replica_vertex_spirv}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${avb0795_spirv_directory}"
  COMMAND Vulkan::glslc
    --target-env=vulkan1.3
    -fshader-stage=vert
    -o "${avb0795_replica_vertex_spirv}"
    "${CMAKE_CURRENT_SOURCE_DIR}/replica.vert"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/replica.vert"
    Vulkan::glslc
  COMMENT "Compiling replica.vert to SPIR-V"
  VERBATIM
)

add_custom_command(
  OUTPUT "${avb0795_replica_fragment_spirv}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${avb0795_spirv_directory}"
  COMMAND Vulkan::glslc
    --target-env=vulkan1.3
    -fshader-stage=frag
    -o "${avb0795_replica_fragment_spirv}"
    "${CMAKE_CURRENT_SOURCE_DIR}/replica.frag"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/replica.frag"
    Vulkan::glslc
  COMMENT "Compiling replica.frag to SPIR-V"
  VERBATIM
)

add_custom_command(
  OUTPUT
    "${avb0795_replica_shader_header}"
    "${avb0795_replica_shader_source}"
  COMMAND "${CMAKE_COMMAND}"
    "-DAVB0795_VERTEX_SPIRV=${avb0795_replica_vertex_spirv}"
    "-DAVB0795_FRAGMENT_SPIRV=${avb0795_replica_fragment_spirv}"
    "-DAVB0795_OUTPUT_HEADER=${avb0795_replica_shader_header}"
    "-DAVB0795_OUTPUT_SOURCE=${avb0795_replica_shader_source}"
    -P "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
  DEPENDS
    "${avb0795_replica_vertex_spirv}"
    "${avb0795_replica_fragment_spirv}"
    "${PROJECT_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
  COMMENT "Embedding replica SPIR-V as C++ arrays"
  VERBATIM
)

add_custom_target(avb0795_replica_shaders
  DEPENDS
    "${avb0795_replica_shader_header}"
    "${avb0795_replica_shader_source}"
)

set(AVB0795_REPLICA_SHADER_HEADER
  "${avb0795_replica_shader_header}" PARENT_SCOPE)
set(AVB0795_REPLICA_SHADER_SOURCE
  "${avb0795_replica_shader_source}" PARENT_SCOPE)
