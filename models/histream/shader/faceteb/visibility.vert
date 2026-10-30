#version 460

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) flat out uint outCameraSide;

layout(push_constant) uniform VisibilityPush {
    mat4 viewProjection;
    vec4 viewDirection;
    uvec4 raster;
    vec4 periodic;
} pc;

ivec2 periodicTileOffset(uint index)
{
    const ivec2 offsets[20] = ivec2[20](
        ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1),
        ivec2(-1, -1), ivec2(-1, 1), ivec2(1, -1), ivec2(1, 1),
        ivec2(-2, 0), ivec2(2, 0), ivec2(0, -2), ivec2(0, 2),
        ivec2(-2, -1), ivec2(-2, 1), ivec2(2, -1), ivec2(2, 1),
        ivec2(-1, -2), ivec2(1, -2), ivec2(-1, 2), ivec2(1, 2));
    return index == 0u ? ivec2(0) : offsets[min(index - 1u, 19u)];
}

void main()
{
    const uint facet = uint(gl_VertexIndex) / 3u;
    const uint opposite = dot(inNormal, pc.viewDirection.xyz) >= 0.0 ? 0u : 1u;
    // The high bit marks periodic replicas: they occlude and emit, but only
    // the central tile is a receiver (visibility_resolve.comp).
    outCameraSide = facet * 2u + opposite
        | (gl_InstanceIndex == 0 ? 0u : 0x80000000u);
    const ivec2 tile = periodicTileOffset(uint(gl_InstanceIndex));
    const vec3 position = inPosition + vec3(
        float(tile.x) * pc.periodic.x, 0.0,
        float(tile.y) * pc.periodic.y);
    gl_Position = pc.viewProjection * vec4(position, 1.0);
}
