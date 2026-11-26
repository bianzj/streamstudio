// ==========================================================================
// 文件：faceteb/visibility_rayquery.glsl（FacetEB，硬件光线查询可见性后端的公共部分）
// 功能：沿正交视线逐像素枚举全部三角形交点，产生与 A-buffer 片元列表等价的
//   {depthBits, cameraSide} 记录（已按深度排序并去重），供 visibility_rayquery.comp
//   （建可见性图）与 direct_rayquery.comp（太阳可见比例）共用。
// 约定（与光栅路径 visibility.vert/frag 一致）：
//   - 射线由 viewProjection 的逆变换得到：深度 0 处为起点，深度 1 处为终点；命中深度 =
//     t/射线长度 ∈ [0,1]，与光栅深度同一标度。像素中心采样，亚像素抖动由矩阵平移项带入。
//   - 顶层加速结构实例 0 为中心瓦片，其余为周期副本（instanceCustomIndex = 瓦片序号）；
//     副本在 cameraSide 上置最高位 0x80000000（只遮挡/发射，不作接收面）。
//   - cameraSide = 2*facet + (dot(normal, viewDirection) >= 0 ? 0 : 1)；facet = 图元序号
//     （BLAS 为非索引三角形列表，每面元一个图元）。
//   - 几何不设 opaque，rayQuery 对每个候选三角形各返回一次，且从不 Confirm，故得到沿线全部交点。
// 去重规则（光栅用左上填充规则保证共享边只落一个三角形，光线查询可能两个都报）：
//   1. 同一 cameraSide（含副本位）且深度差 <= 1e-6：视为重复报告，丢弃；
//   2. 两条命中的重心坐标都落在三角形边上（EDGE_BIT）且深度差 <= 1e-6：视为同一共享边/顶点的
//      重复命中，只保留（深度, 侧编号）较小者。
// 排序键与光栅解析一致：（depthBits 升序, cameraSide 升序，副本位使中心片元在同深度时在前）。
// 限制：每条射线最多 MAX_FRAGMENTS 个交点；超出则 statistics[0] += 1、
//   statistics[2] = max(交点数)，主机据此回退到光栅路径。
// ==========================================================================

#extension GL_EXT_ray_query : require

#ifndef MAX_FRAGMENTS
#define MAX_FRAGMENTS 256u
#endif
#define REPLICA_BIT 0x80000000u
#define EDGE_BIT 0x40000000u
// 深度并列/合并容差（深度已归一到 [0,1]）由主机经 periodic.w 给出：缺省 1e-6，场景外接球很大时
// 收紧到不超过约 5 mm 的几何距离（见 FacetrtVulkan::rayTieDepth），避免与厚 1 cm 的地面混淆。
#define DEPTH_MERGE (pc.periodic.w)
#define DEPTH_TIE (pc.periodic.w)

struct FragmentRecord {
    uint depthBits;
    uint cameraSide;
};

layout(std430, set = 0, binding = 6) buffer Statistics {
    uint statistics[];
};
layout(set = 0, binding = 8) uniform accelerationStructureEXT topLevelAS;
// 顶点缓冲（Vertex = position[3] + normal[3]，6 个 float）；每面元 3 个顶点，法向相同。
layout(std430, set = 0, binding = 9) readonly buffer VertexData {
    float vertexData[];
};

// 推送常量与光栅路径共用：raster = (宽, 高, 每像素最大层数, 哈希容量)。
layout(push_constant) uniform VisibilityPush {
    mat4 viewProjection;
    vec4 viewDirection;
    uvec4 raster;
    vec4 periodic;
} pc;

FragmentRecord localRecords[MAX_FRAGMENTS];

uint sideKey(FragmentRecord record) { return record.cameraSide & ~EDGE_BIT; }

// a 严格排在 b 之前？深度差在 DEPTH_TIE 以内视为同深度，按侧编号（副本位使中心片元在前）定序。
// 共面重叠的面元（如同一高度的屋顶面）其深度只差舍入噪声；光栅路径的插值深度对它们常逐位相等
// 而按侧编号定序，光线查询的 t 带有几个 ulp 的误差，故用容差还原同一规则。
bool before(FragmentRecord a, FragmentRecord b)
{
    const float gap = uintBitsToFloat(a.depthBits) - uintBitsToFloat(b.depthBits);
    if (gap < -DEPTH_TIE) return true;
    if (gap > DEPTH_TIE) return false;
    return sideKey(a) < sideKey(b);
}

// 作用：枚举 pixel 的全部交点，排序并去重后写入 localRecords，返回有效记录数。
uint collectFragments(uint pixel)
{
    const uint width = pc.raster.x;
    const vec2 ndc = (vec2(float(pixel % width), float(pixel / width)) + vec2(0.5)) /
                     vec2(float(pc.raster.x), float(pc.raster.y)) * 2.0 - vec2(1.0);
    const mat3 linear = mat3(pc.viewProjection);
    const vec3 shift = pc.viewProjection[3].xyz;
    const mat3 inverseLinear = inverse(linear);
    const vec3 origin = inverseLinear * (vec3(ndc, 0.0) - shift);
    const vec3 span = inverseLinear * vec3(0.0, 0.0, 1.0);
    const float spanLength = length(span);
    const vec3 direction = span / spanLength;

    uint total = 0u;
    uint stored = 0u;
    rayQueryEXT query;
    rayQueryInitializeEXT(query, topLevelAS, gl_RayFlagsNoneEXT, 0xFFu,
                          origin, 0.0, direction, spanLength);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) !=
            gl_RayQueryCandidateIntersectionTriangleEXT) {
            continue;
        }
        ++total;
        if (stored >= MAX_FRAGMENTS) {
            continue;
        }
        const uint facet = uint(rayQueryGetIntersectionPrimitiveIndexEXT(query, false));
        const float depth = clamp(rayQueryGetIntersectionTEXT(query, false) / spanLength, 0.0, 1.0);
        const uint base = facet * 18u + 3u;  // 第一个顶点的法向
        const vec3 normal = vec3(vertexData[base], vertexData[base + 1u], vertexData[base + 2u]);
        const uint opposite = dot(normal, pc.viewDirection.xyz) >= 0.0 ? 0u : 1u;
        const bool replica = rayQueryGetIntersectionInstanceCustomIndexEXT(query, false) != 0;
        const vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, false);
        const bool onEdge = min(min(bary.x, bary.y), 1.0 - bary.x - bary.y) <= 1.0e-4;
        localRecords[stored].depthBits = floatBitsToUint(depth);
        localRecords[stored].cameraSide = (facet * 2u + opposite) |
                                          (replica ? REPLICA_BIT : 0u) |
                                          (onEdge ? EDGE_BIT : 0u);
        ++stored;
    }
    if (total > MAX_FRAGMENTS) {
        atomicAdd(statistics[0], 1u);
        atomicMax(statistics[2], total);
        return 0u;
    }

    // 排序：与光栅解析相同的键，深度并列用容差（见 before）；边标志位不参与比较。
    for (uint i = 1u; i < stored; ++i) {
        const FragmentRecord value = localRecords[i];
        uint j = i;
        while (j > 0u && before(value, localRecords[j - 1u])) {
            localRecords[j] = localRecords[j - 1u];
            --j;
        }
        localRecords[j] = value;
    }

    // 去重（见文件头）。就地压缩：kept <= i，不会覆盖未读记录。
    uint kept = 0u;
    for (uint i = 0u; i < stored; ++i) {
        const FragmentRecord value = localRecords[i];
        if (kept > 0u) {
            const FragmentRecord previous = localRecords[kept - 1u];
            const float gap = uintBitsToFloat(value.depthBits) - uintBitsToFloat(previous.depthBits);
            const bool sameSide = sideKey(value) == sideKey(previous);
            const bool bothOnEdge = (value.cameraSide & previous.cameraSide & EDGE_BIT) != 0u;
            if (gap <= DEPTH_MERGE && (sameSide || bothOnEdge)) {
                continue;
            }
        }
        localRecords[kept++] = value;
    }
    for (uint i = 0u; i < kept; ++i) {
        localRecords[i].cameraSide &= ~EDGE_BIT;
    }
    return kept;
}
