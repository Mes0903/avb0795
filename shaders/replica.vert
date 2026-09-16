// Replica vertex stage: generate a triangle over the aspect-fit viewport and
// pass local image coordinates to fragment location 0.
#version 450

layout(location = 0) out vec2 texture_coordinates;

// The renderer draws exactly three vertices with firstVertex == 0. Oversized
// clip-space positions cover the viewport without a vertex buffer or diagonal.
const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2(3.0, -1.0),
    vec2(-1.0, 3.0));

const vec2 coordinates[3] = vec2[](
    vec2(0.0, 0.0),
    vec2(2.0, 0.0),
    vec2(0.0, 2.0));

// Clip-space y and texture v increase together for the renderer's positive-height
// viewport. Viewport/scissor place the image; the fragment stage maps the crop.
void main()
{
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    texture_coordinates = coordinates[gl_VertexIndex];
}
