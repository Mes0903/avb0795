// Replica fragment stage: sample encoded SDR values through a UNORM image view.
// The renderer pairs the UNORM output attachment with nonlinear SDR presentation;
// this stage performs no transfer-function conversion or tone mapping.
#version 450

// Set 0 / binding 0 is the slot's combined image sampler. Location 0 receives
// scaled coordinates from the vertex stage and writes all four sampled channels.
layout(set = 0, binding = 0) uniform sampler2D replica_texture;
layout(location = 0) in vec2 texture_coordinates;
layout(location = 0) out vec4 output_color;

// Four vec4 blocks match ReplicaPushConstants. Local UV traverses the effective
// crop, while allocation-normalized bounds constrain sampling to its texels.
layout(push_constant) uniform ReplicaPushConstants
{
    vec4 transform;
    vec4 bounds;
    vec4 selection;
    vec4 stroke;
} replica;

// Filtering and edge behavior come from the bound sampler; alpha is passed through
// unchanged, with blending and presentation alpha configured by the renderer.
void main()
{
    vec2 uv = replica.transform.xy + texture_coordinates * replica.transform.zw;
    uv = clamp(uv, replica.bounds.xy, replica.bounds.zw);
    output_color = texture(replica_texture, uv);
    // Two physical pixels form an inner pink outline over the full-source view.
    // Each axis retains its own UV thickness as the viewport changes aspect.
    vec2 edge_distance = min(texture_coordinates - replica.selection.xy,
                             replica.selection.zw - texture_coordinates);
    bool inside = all(greaterThanEqual(edge_distance, vec2(0.0)));
    bool edge = any(lessThan(edge_distance, replica.stroke.yz));
    if (replica.stroke.x > 0.5 && inside && edge) {
        output_color = vec4(1.0, 0.25, 0.60, 1.0);
    }
}
