#version 450

layout(set = 0, binding = 0) uniform sampler2D replica_texture;
layout(location = 0) in vec2 texture_coordinates;
layout(location = 0) out vec4 output_color;

void main()
{
    output_color = texture(replica_texture, texture_coordinates);
}
