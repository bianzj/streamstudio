#version 460

layout(location = 0) flat in uint inCameraSide;

struct FragmentRecord {
    uint depthBits;
    uint cameraSide;
};

layout(std430, set = 0, binding = 0) buffer FragmentBuffer {
    FragmentRecord records[];
};
layout(std430, set = 0, binding = 1) buffer PixelCounts {
    uint pixelCounts[];
};
layout(std430, set = 0, binding = 6) buffer Statistics {
    uint statistics[];
};

layout(push_constant) uniform VisibilityPush {
    mat4 viewProjection;
    vec4 viewDirection;
    uvec4 raster;
    vec4 periodic;
} pc;

void main()
{
    const uint x = uint(gl_FragCoord.x);
    const uint y = uint(gl_FragCoord.y);
    if (x >= pc.raster.x || y >= pc.raster.y) {
        return;
    }

    const uint pixel = y * pc.raster.x + x;
    const uint layer = atomicAdd(pixelCounts[pixel], 1u);
    if (layer >= pc.raster.z) {
        atomicAdd(statistics[0], 1u);
        return;
    }

    const uint index = pixel * pc.raster.z + layer;
    records[index].depthBits = floatBitsToUint(gl_FragCoord.z);
    records[index].cameraSide = inCameraSide;
}
