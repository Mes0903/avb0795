/// @file
/// Full-screen triangle for the replica's aspect-fit viewport; the push constant
/// limits texture coordinates to the valid content subrectangle of a pool slot.
#version 450

layout(location = 0) out vec2 texture_coordinates;

/// CPU-provided content/allocation ratio; pool padding must never be sampled.
layout(push_constant) uniform ReplicaPushConstants
{
    vec2 texture_scale;
} replica;

const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2(3.0, -1.0),
    vec2(-1.0, 3.0));

const vec2 coordinates[3] = vec2[](
    vec2(0.0, 0.0),
    vec2(2.0, 0.0),
    vec2(0.0, 2.0));

/// Generate one triangle without a vertex buffer; viewport/scissor enforce fit.
void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    texture_coordinates = coordinates[gl_VertexIndex] * replica.texture_scale;
}
