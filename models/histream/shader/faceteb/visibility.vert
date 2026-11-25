// ==========================================================================
// 文件：faceteb/visibility.vert（FacetEB）
// 功能：可见性光栅化的顶点着色器：把面元三角形沿视线方向正投影，并生成周期平铺的复制体实例。
// 主要模块/步骤：
//   1. 由 gl_VertexIndex/3 得到面元号；按法线与 viewDirection 判定朝向相机的是哪一侧
//   2. 实例号 0 为中心瓦片，其余实例为 x–z 方向的周期复制体（最多 20 个）
//   3. 把顶点平移到对应瓦片并乘 viewProjection，输出裁剪坐标
// 数据流：顶点缓冲（位置、法线）→ 本着色器 → visibility.frag（逐片元写 A-buffer）
//   → visibility_resolve.comp / direct_resolve.comp
// 约定：每个面元恰好一个三角形（非索引绘制，3 个顶点/面元）；
//   侧编号 side = 2*facet + (0 正面 | 1 背面)，outCameraSide 是朝向相机的一侧；
//   outCameraSide 最高位 0x80000000 标记周期复制体：复制体参与遮挡与发射，但不作为接收面。
// 注意：法线与 viewDirection 点积 >= 0 视为正面朝向相机，即 viewDirection 指向相机一侧（待核对）。
// ==========================================================================

#version 460

// 顶点输入：位置（世界坐标，m）与法线（单位向量，决定朝向相机的一侧）。
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

// 输出（不插值）：朝向相机的面元侧编号 = 面元号*2 + (0 正面 | 1 背面)，
// 最高位置位表示周期复制体。
layout(location = 0) flat out uint outCameraSide;

// 推送常量：viewProjection 视图投影矩阵；viewDirection 视线方向（xyz）；
// raster 光栅尺寸等（本着色器未用）；periodic.xy 为周期平铺在 x、z 方向的步长。
layout(push_constant) uniform VisibilityPush {
    mat4 viewProjection;
    vec4 viewDirection;
    uvec4 raster;
    vec4 periodic;
} pc;

// 作用：实例号 → 周期瓦片偏移（以瓦片数计，x、z 方向）。
// 0 = 中心瓦片；1..20 依次为 4 个直接邻、4 个对角邻，再外一圈的 12 个复制瓦片；
// 超过 20 的实例号被限幅到最后一项。
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

// 入口：每个顶点输出裁剪坐标与（flat）朝向相机的侧编号。
void main()
{
    const uint facet = uint(gl_VertexIndex) / 3u;
    // 法线与 viewDirection 同向取 0（正面侧朝向相机），否则取 1（背面侧朝向相机）。
    const uint opposite = dot(inNormal, pc.viewDirection.xyz) >= 0.0 ? 0u : 1u;
    // 步骤 1：编码 cameraSide（面元号、朝向侧、复制体标记）。
    // The high bit marks periodic replicas: they occlude and emit, but only
    // the central tile is a receiver (visibility_resolve.comp).
    outCameraSide = facet * 2u + opposite
        | (gl_InstanceIndex == 0 ? 0u : 0x80000000u);
    // 步骤 2：按实例号平移到对应周期瓦片（y 方向不平移），再做投影。
    const ivec2 tile = periodicTileOffset(uint(gl_InstanceIndex));
    const vec3 position = inPosition + vec3(
        float(tile.x) * pc.periodic.x, 0.0,
        float(tile.y) * pc.periodic.y);
    gl_Position = pc.viewProjection * vec4(position, 1.0);
}
