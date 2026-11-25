// ==========================================================================
// 文件：faceteb/visibility.frag（FacetEB）
// 功能：可见性光栅化的片元着色器：为每个像素建立片元列表（A-buffer，列表内顺序不确定）。
// 主要模块/步骤：
//   1. 计算像素编号，用 atomicAdd(pixelCounts) 取得本片元在该像素列表中的层号
//   2. 层号超过上限时只记统计（溢出数、最深层数），不写记录
//   3. 否则写入 depthBits 与 cameraSide
// 数据流：visibility.vert 的 inCameraSide → 本着色器 → records/pixelCounts/statistics
//   → visibility_resolve.comp、direct_resolve.comp（排序并建立可见性图）
// 约定：无颜色/深度输出；所有片元（含被遮挡者）都要入列表。
//   statistics[0] = 溢出片元数；statistics[2] = 本轮见到的最深列表长度；
//   statistics[1] 由 visibility_resolve.comp 写边哈希表溢出次数。
// 注意：列表内顺序取决于 GPU 调度，后续着色器必须按（深度, 侧编号）重新排序。
// ==========================================================================

#version 460

// 输入（flat）：朝向相机的面元侧编号，最高位为周期复制体标记。
layout(location = 0) flat in uint inCameraSide;

// A-buffer 片元记录：depthBits = gl_FragCoord.z 的 float 位模式；cameraSide 同上。
struct FragmentRecord {
    uint depthBits;
    uint cameraSide;
};

// 缓冲区：records 按 [像素*每像素层数 + 层号] 存放片元；
// pixelCounts 每像素片元计数（可超过层数上限）；statistics 统计量（见文件头）。
layout(std430, set = 0, binding = 0) buffer FragmentBuffer {
    FragmentRecord records[];
};
layout(std430, set = 0, binding = 1) buffer PixelCounts {
    uint pixelCounts[];
};
layout(std430, set = 0, binding = 6) buffer Statistics {
    uint statistics[];
};

// 推送常量：raster = (宽, 高, 每像素最大层数, 边哈希表容量)；其余字段本着色器未用。
layout(push_constant) uniform VisibilityPush {
    mat4 viewProjection;
    vec4 viewDirection;
    uvec4 raster;
    vec4 periodic;
} pc;

// 入口：每个片元执行一次，向其所在像素的列表追加一条记录。
void main()
{
    const uint x = uint(gl_FragCoord.x);
    const uint y = uint(gl_FragCoord.y);
    if (x >= pc.raster.x || y >= pc.raster.y) {
        return;
    }

    // 步骤 1：像素线性编号，并以原子计数取得层号（列表追加位置）。
    const uint pixel = y * pc.raster.x + x;
    const uint layer = atomicAdd(pixelCounts[pixel], 1u);
    if (layer >= pc.raster.z) {
        atomicAdd(statistics[0], 1u);
        // Deepest list seen, so the host can size the next A-buffer at once
        // instead of doubling and rebuilding the visibility graph repeatedly.
        atomicMax(statistics[2], layer + 1u);
        return;
    }

    // 步骤 2：写入记录（深度用位模式；深度 ∈ [0,1]，float 位序即数值序，便于整数排序）。
    const uint index = pixel * pc.raster.z + layer;
    records[index].depthBits = floatBitsToUint(gl_FragCoord.z);
    records[index].cameraSide = inCameraSide;
}
