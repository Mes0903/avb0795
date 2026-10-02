# Convert compiled shader bytes into owning static C++ arrays and borrowed spans.
# This script generates build-tree files; it does not compile GLSL or validate a
# shader's semantic interface with the renderer. Keep generation separate so the
# build graph can track shader compilation and embedding as distinct dependencies.
foreach(required_variable
        AVB0795_VERTEX_SPIRV
        AVB0795_FRAGMENT_SPIRV
        AVB0795_OUTPUT_HEADER
        AVB0795_OUTPUT_SOURCE)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

# Return formatted 32-bit words and their count through the caller's scope.
# Reject truncated/non-word-aligned input and an unexpected little-endian magic
# before generating source. These checks establish the supported byte layout,
# not full SPIR-V validity. Reassemble each word explicitly from its byte order so
# the generated numeric values do not depend on CMake's host endianness.
function(_avb0795_spirv_words input output_words output_count)
  if(NOT EXISTS "${input}")
    message(FATAL_ERROR "SPIR-V input does not exist: ${input}")
  endif()

  file(SIZE "${input}" byte_count)
  math(EXPR trailing_bytes "${byte_count} % 4")
  if(NOT trailing_bytes EQUAL 0)
    message(FATAL_ERROR "SPIR-V size is not a multiple of four: ${input}")
  endif()
  if(byte_count LESS 4)
    message(FATAL_ERROR "SPIR-V input is empty or truncated: ${input}")
  endif()

  file(READ "${input}" bytes HEX)
  string(SUBSTRING "${bytes}" 0 8 magic)
  if(NOT magic STREQUAL "03022307")
    message(FATAL_ERROR "SPIR-V input has an unexpected magic number: ${input}")
  endif()

  math(EXPR word_count "${byte_count} / 4")
  math(EXPR last_word "${word_count} - 1")
  set(words)
  foreach(word_index RANGE 0 ${last_word})
    math(EXPR byte_offset "${word_index} * 8")
    string(SUBSTRING "${bytes}" ${byte_offset} 2 byte_0)
    math(EXPR byte_offset_1 "${byte_offset} + 2")
    string(SUBSTRING "${bytes}" ${byte_offset_1} 2 byte_1)
    math(EXPR byte_offset_2 "${byte_offset} + 4")
    string(SUBSTRING "${bytes}" ${byte_offset_2} 2 byte_2)
    math(EXPR byte_offset_3 "${byte_offset} + 6")
    string(SUBSTRING "${bytes}" ${byte_offset_3} 2 byte_3)
    list(APPEND words "0x${byte_3}${byte_2}${byte_1}${byte_0}u")
  endforeach()

  string(JOIN ",\n        " formatted_words ${words})
  set(${output_words} "${formatted_words}" PARENT_SCOPE)
  set(${output_count} "${word_count}" PARENT_SCOPE)
endfunction()

_avb0795_spirv_words(
  "${AVB0795_VERTEX_SPIRV}" vertex_words vertex_word_count)
_avb0795_spirv_words(
  "${AVB0795_FRAGMENT_SPIRV}" fragment_words fragment_word_count)

get_filename_component(output_header_directory
  "${AVB0795_OUTPUT_HEADER}" DIRECTORY)
get_filename_component(output_source_directory
  "${AVB0795_OUTPUT_SOURCE}" DIRECTORY)
file(MAKE_DIRECTORY
  "${output_header_directory}"
  "${output_source_directory}")

# These bracket/quoted values are generated C++ source, not executable CMake or
# documentation comments. Their literal bytes form the renderer's generated API;
# explanatory CMake comments belong outside the values.
set(header_contents [=[#pragma once

#include <cstdint>
#include <span>

namespace avb::render {

[[nodiscard]] std::span<const std::uint32_t> replica_vertex_shader() noexcept;
[[nodiscard]] std::span<const std::uint32_t> replica_fragment_shader() noexcept;

}  // namespace avb::render
]=])

set(source_contents "#include \"avb/render/replica_shaders.hpp\"\n\n")
string(APPEND source_contents "#include <array>\n\n")
string(APPEND source_contents "namespace avb::render {\nnamespace {\n\n")
string(APPEND source_contents
  "constexpr std::array<std::uint32_t, ${vertex_word_count}> replica_vertex_shader_words{\n"
  "        ${vertex_words}\n};\n\n")
string(APPEND source_contents
  "constexpr std::array<std::uint32_t, ${fragment_word_count}> replica_fragment_shader_words{\n"
  "        ${fragment_words}\n};\n\n")
string(APPEND source_contents "}  // namespace\n\n")
string(APPEND source_contents
  "std::span<const std::uint32_t> replica_vertex_shader() noexcept {\n"
  "  return replica_vertex_shader_words;\n}\n\n")
string(APPEND source_contents
  "std::span<const std::uint32_t> replica_fragment_shader() noexcept {\n"
  "  return replica_fragment_shader_words;\n}\n\n")
string(APPEND source_contents "}  // namespace avb::render\n")

file(WRITE "${AVB0795_OUTPUT_HEADER}" "${header_contents}")
file(WRITE "${AVB0795_OUTPUT_SOURCE}" "${source_contents}")
