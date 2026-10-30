//
// hexvoxel.cpp
//
// 异质性体元核心计算方法实现。
// 算法细节见 hexvoxel.h 头部注释。
//

#include "hexvoxel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_map>

namespace hexvoxel {

namespace {

// ---------------------------------------------------------------------------
// 基础几何
// ---------------------------------------------------------------------------

struct V3 {
    float x{0}, y{0}, z{0};
};

inline V3 operator+(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(const V3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(const V3& a, const V3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const V3& a) { return std::sqrt(dot(a, a)); }
inline V3 normalize(const V3& a) {
    const float l = length(a);
    return l > 1.0e-20f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1};
}

struct TriGeom {
    V3 p0, p1, p2;
    V3 n;      // 未归一化法向
    float minX, minY, minZ, maxX, maxY, maxZ;
};

// x 方向扫描奇偶的半开规则: 交点恰在三角边界 (投影到 (y,z) 的边上) 时,
// 仅当该边在 (y,z) 平面中"向上" (b.z>a.z, 平则 b.y>a.y) 才计一次穿越。
// 扇形剖分的共享对角线被两个三角以相反方向遍历, 恰好一个向上 => 只计一次。
bool xScanBoundaryRule(const V3& p, const V3& a, const V3& b, const V3& c)
{
    const V3 pts[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        const V3& A = pts[i];
        const V3& B = pts[(i + 1) % 3];
        const float ey = B.y - A.y, ez = B.z - A.z;
        const float len2 = ey * ey + ez * ez;
        if (len2 < 1.0e-12f) continue; // 投影退化
        const float t = ((p.y - A.y) * ey + (p.z - A.z) * ez) / len2;
        if (t < -1.0e-6f || t > 1.0f + 1.0e-6f) continue;
        const float cross = ey * (p.z - A.z) - ez * (p.y - A.y);
        if (std::abs(cross) > 1.0e-5f * std::sqrt(len2)) continue;
        if (B.z > A.z || (B.z == A.z && B.y > A.y)) return true;
    }
    return false;
}

// 段 AB 是否穿过三角面 (平面符号翻转 + 重心坐标包含测试)。
// 用于奇偶 (inside/outside) 判定; 交点恰在边界时按 xScanBoundaryRule 半开规则计一次。
bool segmentHitsTriangle(const V3& a, const V3& b, const TriGeom& t, float eps = 1.0e-5f)
{
    const float fa = dot(t.n, {a.x - t.p0.x, a.y - t.p0.y, a.z - t.p0.z});
    const float fb = dot(t.n, {b.x - t.p0.x, b.y - t.p0.y, b.z - t.p0.z});
    if (fa * fb >= 0.0f) return false; // 无严格符号翻转
    const float denom = fb - fa;
    const float s = -fa / denom;        // 参数化 a + s*(b-a)
    const V3 p{a.x + s * (b.x - a.x), a.y + s * (b.y - a.y), a.z + s * (b.z - a.z)};

    // 重心坐标包含测试
    const V3 e0 = {t.p1.x - t.p0.x, t.p1.y - t.p0.y, t.p1.z - t.p0.z};
    const V3 e1 = {t.p2.x - t.p0.x, t.p2.y - t.p0.y, t.p2.z - t.p0.z};
    const V3 w = {p.x - t.p0.x, p.y - t.p0.y, p.z - t.p0.z};
    const float d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1);
    const float d20 = dot(w, e0), d21 = dot(w, e1);
    const float denom2 = d00 * d11 - d01 * d01;
    if (std::abs(denom2) < 1.0e-18f) return false;
    const float v = (d11 * d20 - d01 * d21) / denom2;
    const float u = (d00 * d21 - d01 * d20) / denom2;
    if (u < -eps || v < -eps || (u + v) > 1.0f + eps) return false;
    if (u > 0.0f && v > 0.0f && (u + v) < 1.0f) return true;
    // 交点在边界: 半开规则 (对角线/竖直边恰好穿过扫描线的情形)
    return xScanBoundaryRule(p, t.p0, t.p1, t.p2);
}

// 线段-线段距离 (经典 Ericson 算法)
float segSegDist(const V3& p1, const V3& q1, const V3& p2, const V3& q2)
{
    const V3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = dot(d1, d1), b = dot(d1, d2), c = dot(d2, d2);
    const float d = dot(d1, r), e = dot(d2, r);
    const float denom = a * c - b * b;
    float sc{0}, sd{0};
    if (std::abs(denom) < 1.0e-12f) {
        sc = 0.0f;
        sd = c > 1.0e-12f ? e / c : 0.0f;
        if (sd < 0.0f) sd = 0.0f;
        else if (sd > 1.0f) sd = 1.0f;
    } else {
        sc = (b * e - c * d) / denom;
        if (sc < 0.0f) {
            sc = 0.0f;
            sd = e / c;
        } else if (sc > 1.0f) {
            sc = 1.0f;
            sd = (e + b) / c;
        } else {
            sd = (e + sc * b) / c;
        }
        if (sd < 0.0f) sd = 0.0f;
        else if (sd > 1.0f) sd = 1.0f;
    }
    const V3 cp1 = p1 + d1 * sc;
    const V3 cp2 = p2 + d2 * sd;
    return length(cp1 - cp2);
}

// 点是否落在三角面内 (投影到主平面做重心坐标)
bool pointInTriangle(const V3& p, const V3& a, const V3& b, const V3& cc, float eps = 1.0e-5f)
{
    const V3 e0 = b - a, e1 = cc - a, w = p - a;
    const float d00 = dot(e0, e0), d01 = dot(e0, e1), d11 = dot(e1, e1);
    const float d20 = dot(w, e0), d21 = dot(w, e1);
    const float denom = d00 * d11 - d01 * d01;
    if (std::abs(denom) < 1.0e-18f) return false;
    const float v = (d11 * d20 - d01 * d21) / denom;
    const float u = (d00 * d21 - d01 * d20) / denom;
    return u >= -eps && v >= -eps && (u + v) <= 1.0f + eps;
}

// 线段到三角面的距离:
//  1) 端点在面内 => 0
//  2) 线段穿过面内部 (符号翻转且交点在面内) => 0
//  3) 否则取三条边距离的最小值
float segTriDist(const V3& p1, const V3& q1, const TriGeom& t)
{
    const float nLen = length(t.n);
    const auto onPlane = [&](const V3& p) {
        return std::abs(dot(t.n, {p.x - t.p0.x, p.y - t.p0.y, p.z - t.p0.z})) <= 1.0e-4f * nLen;
    };
    if ((onPlane(p1) && pointInTriangle(p1, t.p0, t.p1, t.p2)) ||
        (onPlane(q1) && pointInTriangle(q1, t.p0, t.p1, t.p2))) {
        return 0.0f;
    }
    const float fa = dot(t.n, {p1.x - t.p0.x, p1.y - t.p0.y, p1.z - t.p0.z});
    const float fb = dot(t.n, {q1.x - t.p0.x, q1.y - t.p0.y, q1.z - t.p0.z});
    if (fa * fb < 0.0f) {
        const float s = fa / (fa - fb);
        const V3 p{p1.x + s * (q1.x - p1.x), p1.y + s * (q1.y - p1.y), p1.z + s * (q1.z - p1.z)};
        if (pointInTriangle(p, t.p0, t.p1, t.p2)) return 0.0f;
    }
    float d = segSegDist(p1, q1, t.p0, t.p1);
    d = std::min(d, segSegDist(p1, q1, t.p1, t.p2));
    d = std::min(d, segSegDist(p1, q1, t.p2, t.p0));
    return d;
}

// ---------------------------------------------------------------------------
// OBJ 解析
// ---------------------------------------------------------------------------

struct RawFace {
    std::uint32_t i0, i1, i2;
    std::uint32_t objIndex; // 所属对象 (当前 o/g 组)
};

} // namespace

std::uint32_t HexScene::objectOfTri(std::uint32_t triIndex) const
{
    for (std::size_t o = 0; o < objects.size(); ++o) {
        const auto& ob = objects[o];
        if (triIndex >= ob.triOffset && triIndex < ob.triOffset + ob.triCount) {
            return static_cast<std::uint32_t>(o);
        }
    }
    return 0;
}

bool loadObj(const std::string& path, float voxelSize, HexScene& out, std::string* err)
{
    // DEFERRED[中] 安全: 窄字符 std::ifstream(path) 在 Windows 上按 ANSI 代码页解释，UTF-8 中文路径的 OBJ 打不开；建议：用 std::filesystem::u8path(path) 构造流。
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "cannot open OBJ file: " + path;
        return false;
    }
    if (!(voxelSize > 0.0f)) {
        if (err) *err = "voxelSize must be > 0";
        return false;
    }

    out = HexScene{};
    out.voxelSize = voxelSize;

    std::vector<V3> verts;
    std::vector<std::array<long long, 3>> vertsQ; // 量化坐标 (double 精度)
    std::vector<RawFace> faces;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> edgeCount; // 暂不用, 见下
    std::string curObject;
    bool haveObject = false;
    std::uint32_t currentObjectIndex = 0;
    std::vector<std::string> objNames;             // 按 OBJ 中连续的 o/g 段保存
    std::vector<std::uint32_t> objTriStart;        // 每个对象首个三角下标

    std::string line;
    while (std::getline(f, line)) {
        // 去掉行尾 \r (Windows 换行)
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // DEFERRED[低] 解析鲁棒性: 只识别以 "v "/"f "/"o "/"g " 开头的行，行首有空白或用制表符分隔（"v\t1 2 3"）的合法 OBJ 行被静默忽略，顶点缺失后后续负索引/正索引全部错位；建议：先跳过前导空白，再按首个空白分隔的关键字判断。
        if (line.empty() || line[0] == '#') continue;
        const char* c = line.c_str();
        if (line.compare(0, 2, "v ") == 0) {
            double xd, yd, zd;
            if (std::sscanf(c + 2, "%lf %lf %lf", &xd, &yd, &zd) == 3) {
                const float inv = static_cast<float>(1.0 / voxelSize);
                // DEFERRED[中] 数值: 顶点在平移到网格原点之前就被转换为 float 体元坐标；投影坐标（如 UTM x≈5e5 m、体元 0.1 m → 5e6）时 float 量化步长达 0.5 体元，三角形被吸附、薄叶片退化，CI/rho 明显失真；且 xd*inv 乘以 1e5 量化时同样丢精度；建议：以 double 保存原始顶点，先减去 floor(min) 原点再转 float。
                verts.push_back({static_cast<float>(xd * inv),
                                 static_cast<float>(yd * inv),
                                 static_cast<float>(zd * inv)});
                vertsQ.push_back({std::llround(xd * inv * 1.0e5),
                                  std::llround(yd * inv * 1.0e5),
                                  std::llround(zd * inv * 1.0e5)});
            }
        } else if (line.compare(0, 2, "f ") == 0) {
            std::uint32_t idx[32];
            // DEFERRED[低] 解析鲁棒性: 超过 32 个顶点的多边形被静默截断；无效/越界索引被直接跳过而面仍被扇形剖分，得到与原多边形不同的错误三角形且无任何告警；扇形剖分对凹多边形也不正确；建议：用 std::vector 收集索引，遇到无效索引时丢弃整个面并计数告警。
            int n = 0;
            char* rest = const_cast<char*>(c + 2);
            while (n < 32) {
                while (*rest == ' ' || *rest == '\t') ++rest;
                if (*rest == 0) break;

                // Each OBJ face token can be v, v/vt, v//vn or v/vt/vn.
                // Parse only the leading vertex index, then advance over the
                // complete token.  The former parser stopped at the first '/'
                // and consequently rejected normal exported OBJ files.
                char* tokenEnd = rest;
                while (*tokenEnd != 0 && *tokenEnd != ' ' && *tokenEnd != '\t') {
                    ++tokenEnd;
                }
                char* end = nullptr;
                long v = std::strtol(rest, &end, 10);
                if (end == rest || v == 0) {
                    rest = tokenEnd;
                    continue;
                }
                const long resolved = v > 0
                    ? v - 1
                    : static_cast<long>(verts.size()) + v;
                if (resolved >= 0 && resolved < static_cast<long>(verts.size())) {
                    idx[n++] = static_cast<std::uint32_t>(resolved);
                }
                rest = tokenEnd;
            }
            if (n >= 3) {
                if (!haveObject) {
                    curObject = "default";
                    haveObject = true;
                    currentObjectIndex = static_cast<std::uint32_t>(objNames.size());
                    objNames.push_back(curObject);
                    objTriStart.push_back(static_cast<std::uint32_t>(faces.size()));
                }
                // n-gon 扇形剖分 (OBJ 常见为 4 顶点面)
                for (int t = 1; t + 1 < n; ++t) {
                    faces.push_back({idx[0], idx[t], idx[t + 1], currentObjectIndex});
                }
            }
        } else if (line.compare(0, 2, "o ") == 0 || line.compare(0, 2, "g ") == 0) {
            std::string name = line.substr(2);
            if (!name.empty() && name[0] == ' ') name = name.substr(1);
            if (name.empty()) name = "object" + std::to_string(objNames.size() + 1);
            // Exporters commonly emit `o Tree` immediately followed by
            // `g leaves`.  Both start at the same face, so the more specific
            // group name replaces the still-empty object segment.
            if (!objTriStart.empty() && objTriStart.back() == faces.size()) {
                objNames.back() = name;
                currentObjectIndex = static_cast<std::uint32_t>(objNames.size() - 1U);
            } else {
                currentObjectIndex = static_cast<std::uint32_t>(objNames.size());
                objNames.push_back(name);
                objTriStart.push_back(static_cast<std::uint32_t>(faces.size()));
            }
            curObject = name;
            haveObject = true;
        }
    }

    if (faces.empty()) {
        if (err) *err = "OBJ has no faces: " + path;
        return false;
    }

    // 建立对象列表与三角连续存储
    out.tris.resize(faces.size());
    out.objects.clear();
    out.objects.resize(objTriStart.size());
    for (std::size_t o = 0; o < objTriStart.size(); ++o) {
        out.objects[o].name = objNames[o];
        out.objects[o].triOffset = objTriStart[o];
        out.objects[o].triCount =
            static_cast<std::uint32_t>((o + 1 < objTriStart.size() ? objTriStart[o + 1] : faces.size()) - objTriStart[o]);
    }

    for (std::size_t t = 0; t < faces.size(); ++t) {
        const auto& fc = faces[t];
        if (fc.i0 >= verts.size() || fc.i1 >= verts.size() || fc.i2 >= verts.size()) continue;
        out.tris[t] = Tri3{};
        out.tris[t].p0[0] = verts[fc.i0].x; out.tris[t].p0[1] = verts[fc.i0].y; out.tris[t].p0[2] = verts[fc.i0].z;
        out.tris[t].p1[0] = verts[fc.i1].x; out.tris[t].p1[1] = verts[fc.i1].y; out.tris[t].p1[2] = verts[fc.i1].z;
        out.tris[t].p2[0] = verts[fc.i2].x; out.tris[t].p2[1] = verts[fc.i2].y; out.tris[t].p2[2] = verts[fc.i2].z;
    }

    // AABB
    for (int a = 0; a < 3; ++a) { out.minPos[a] = 1.0e30f; out.maxPos[a] = -1.0e30f; }
    for (const auto& t : out.tris) {
        const float p[3][3] = {{t.p0[0], t.p0[1], t.p0[2]},
                               {t.p1[0], t.p1[1], t.p1[2]},
                               {t.p2[0], t.p2[1], t.p2[2]}};
        for (int k = 0; k < 3; ++k) {
            for (int a = 0; a < 3; ++a) {
                out.minPos[a] = std::min(out.minPos[a], p[k][a]);
                out.maxPos[a] = std::max(out.maxPos[a], p[k][a]);
            }
        }
    }

    // 平移到网格原点: 所有坐标减去 floor(minPos), 使体元网格从 (0,0,0) 开始
    const float origin[3] = {std::floor(out.minPos[0]), std::floor(out.minPos[1]), std::floor(out.minPos[2])};
    for (int a = 0; a < 3; ++a) out.gridOrigin[a] = origin[a];
    for (auto& t : out.tris) {
        float* pts[3] = {t.p0, t.p1, t.p2};
        for (int k = 0; k < 3; ++k) {
            for (int a = 0; a < 3; ++a) pts[k][a] -= origin[a];
        }
    }
    for (int a = 0; a < 3; ++a) {
        out.minPos[a] -= origin[a];
        out.maxPos[a] -= origin[a];
    }

    // 封闭性判定 (兼容未焊接重复顶点 / 重复输出的内部双面):
    // DEFERRED[中] 物理: 有向边净计数为 0 的判据要求全网格绕序一致；导出器常见的绕序不一致的封闭体（相邻面同向遍历共享边，净计数 ±2）会被判为开放，内部体元不会填充，建筑/树干变成空壳；反之重合的双面开放面（正反各一层）净计数为 0 会被判为封闭；建议：改为统计无向边出现次数（每条边恰好 2 次/偶数次）判断封闭，绕序一致性另行检查。
    // 按量化顶点位置配对几何边, 累加有向边计数 (每个面把它的三条有向边
    // 各计 +1/-1)。封闭 (含内部双面墙) 的网格所有几何边净计数为 0;
    // 开放面 (如地面) 的边界边净计数为 ±1。
    for (std::size_t o = 0; o < out.objects.size(); ++o) {
        auto& ob = out.objects[o];
        if (ob.triCount == 0) { ob.closed = false; continue; }
        struct QKey {
            long long a[3], b[3];
            bool operator<(const QKey& other) const {
                for (int i = 0; i < 3; ++i) if (a[i] != other.a[i]) return a[i] < other.a[i];
                for (int i = 0; i < 3; ++i) if (b[i] != other.b[i]) return b[i] < other.b[i];
                return false;
            }
        };
        std::map<QKey, long long> edges;
        bool ok = true;
        for (std::uint32_t t = ob.triOffset; t < ob.triOffset + ob.triCount; ++t) {
            const std::uint32_t v[3] = {faces[t].i0, faces[t].i1, faces[t].i2};
            long long q[3][3];
            for (int k = 0; k < 3; ++k) {
                q[k][0] = vertsQ[v[k]][0];
                q[k][1] = vertsQ[v[k]][1];
                q[k][2] = vertsQ[v[k]][2];
            }
            for (int k = 0; k < 3; ++k) {
                const int ia = k, ib = (k + 1) % 3;
                if (q[ia][0] == q[ib][0] && q[ia][1] == q[ib][1] && q[ia][2] == q[ib][2]) {
                    ok = false; continue; // 退化边
                }
                // 字典序决定无向边的规范方向
                int first = ia, second = ib;
                for (int ax = 0; ax < 3; ++ax) {
                    if (q[ia][ax] > q[ib][ax]) { first = ib; second = ia; break; }
                    if (q[ia][ax] < q[ib][ax]) { first = ia; second = ib; break; }
                }
                QKey key;
                for (int ax = 0; ax < 3; ++ax) key.a[ax] = q[first][ax];
                for (int ax = 0; ax < 3; ++ax) key.b[ax] = q[second][ax];
                const long long sign = (ia == first) ? +1 : -1; // 面的边是否沿规范方向
                edges[key] += sign;
            }
        }
        if (ok) {
            for (const auto& e : edges) {
                if (e.second != 0) { ok = false; break; }
            }
        }
        ob.closed = ok;
    }

    return true;
}

// ---------------------------------------------------------------------------
// 主计算
// ---------------------------------------------------------------------------

namespace {

struct Cell {
    std::vector<std::uint32_t> tris; // 与该体元 AABB 相交的三角下标
};

struct Ctx {
    const HexScene* scene;
    std::vector<TriGeom> geom; // 与 scene->tris 对齐
    std::vector<char> closedObj; // 对象是否封闭
    int gx, gy, gz;
    // 体元中心奇偶性 (1=材料内部)
    std::vector<char> parity; // [x][y][z]
    // 每个体元的三角列表 (仅存非空)
    std::vector<Cell> cells;
    long long triTests{0};
};

inline std::size_t cellIndex(const Ctx& c, int x, int y, int z)
{
    return (static_cast<std::size_t>(x) * c.gy + y) * c.gz + z;
}

// 三角 t 的 AABB 是否覆盖体元 (x,y,z)
bool triTouchesCell(const TriGeom& t, int x, int y, int z)
{
    return t.minX <= x + 1.0f + 1.0e-6f && t.maxX >= x - 1.0e-6f &&
           t.minY <= y + 1.0f + 1.0e-6f && t.maxY >= y - 1.0e-6f &&
           t.minZ <= z + 1.0f + 1.0e-6f && t.maxZ >= z - 1.0e-6f;
}


float component(const V3& p, int axis)
{
    return axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
}

std::vector<V3> clipCellPolygon(const std::vector<V3>& source,
                                int axis, float plane, bool keepGreater)
{
    std::vector<V3> result;
    if (source.empty()) return result;
    const auto inside = [axis, plane, keepGreater](const V3& point) {
        const float value = component(point, axis);
        return keepGreater ? value >= plane - 1.0e-6f
                           : value <= plane + 1.0e-6f;
    };

    V3 previous = source.back();
    bool previousInside = inside(previous);
    for (const V3& current : source) {
        const bool currentInside = inside(current);
        if (currentInside != previousInside) {
            const float denominator =
                component(current, axis) - component(previous, axis);
            if (std::abs(denominator) > 1.0e-8f) {
                const float t = std::clamp(
                    (plane - component(previous, axis)) / denominator,
                    0.0f, 1.0f);
                result.push_back(previous + (current - previous) * t);
            }
        }
        if (currentInside) result.push_back(current);
        previous = current;
        previousInside = currentInside;
    }
    return result;
}

std::vector<V3> trianglePolygonInsideCell(const TriGeom& triangle,
                                          int x, int y, int z)
{
    std::vector<V3> polygon{triangle.p0, triangle.p1, triangle.p2};
    const int lower[3] = {x, y, z};
    for (int axis = 0; axis < 3 && polygon.size() >= 3; ++axis) {
        polygon = clipCellPolygon(
            polygon, axis, static_cast<float>(lower[axis]), true);
        polygon = clipCellPolygon(
            polygon, axis, static_cast<float>(lower[axis] + 1), false);
    }
    return polygon;
}

float polygonArea(const std::vector<V3>& polygon)
{
    if (polygon.size() < 3) return 0.0f;

    float area = 0.0f;
    for (std::size_t index = 1; index + 1 < polygon.size(); ++index) {
        area += 0.5f * length(cross(
            polygon[index] - polygon[0],
            polygon[index + 1] - polygon[0]));
    }
    return area;
}

float triangleAreaInsideCell(const TriGeom& triangle, int x, int y, int z)
{
    return polygonArea(trianglePolygonInsideCell(triangle, x, y, z));
}

struct V2 {
    float x{0}, y{0};
};

bool pointInConvexPolygon2D(const std::vector<V2>& polygon, float x, float y)
{
    int sign = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const V2& a = polygon[i];
        const V2& b = polygon[(i + 1) % polygon.size()];
        const float cross2 = (b.x - a.x) * (y - a.y) -
                             (b.y - a.y) * (x - a.x);
        if (std::abs(cross2) <= 1.0e-7f) continue;
        const int current = cross2 > 0.0f ? 1 : -1;
        if (sign != 0 && current != sign) return false;
        sign = current;
    }
    return sign != 0;
}

float projectionCoverageAxis(const Ctx& c, int cellX, int cellY, int cellZ,
                             int axis, int sampleN,
                             std::vector<std::uint64_t>* outputMask = nullptr)
{
    const int n = std::max(sampleN, 1);
    std::vector<unsigned char> covered(static_cast<std::size_t>(n) * n, 0);
    const auto idx = cellIndex(c, cellX, cellY, cellZ);

    for (std::uint32_t tri : c.cells[idx].tris) {
        const std::vector<V3> clipped =
            trianglePolygonInsideCell(c.geom[tri], cellX, cellY, cellZ);
        if (clipped.size() < 3) continue;

        std::vector<V2> projected;
        projected.reserve(clipped.size());
        float minU = 1.0f, minV = 1.0f, maxU = 0.0f, maxV = 0.0f;
        for (const V3& p : clipped) {
            V2 q;
            if (axis == 0) {
                q = {p.y - cellY, p.z - cellZ};
            } else if (axis == 1) {
                q = {p.x - cellX, p.z - cellZ};
            } else {
                q = {p.x - cellX, p.y - cellY};
            }
            q.x = std::clamp(q.x, 0.0f, 1.0f);
            q.y = std::clamp(q.y, 0.0f, 1.0f);
            minU = std::min(minU, q.x); maxU = std::max(maxU, q.x);
            minV = std::min(minV, q.y); maxV = std::max(maxV, q.y);
            projected.push_back(q);
        }

        const int u0 = std::max(0, static_cast<int>(std::ceil(minU * n - 0.5f)));
        const int u1 = std::min(n - 1, static_cast<int>(std::floor(maxU * n - 0.5f)));
        const int v0 = std::max(0, static_cast<int>(std::ceil(minV * n - 0.5f)));
        const int v1 = std::min(n - 1, static_cast<int>(std::floor(maxV * n - 0.5f)));
        for (int v = v0; v <= v1; ++v) {
            const float pv = (static_cast<float>(v) + 0.5f) / n;
            for (int u = u0; u <= u1; ++u) {
                const std::size_t pixel = static_cast<std::size_t>(v) * n + u;
                if (covered[pixel]) continue;
                const float pu = (static_cast<float>(u) + 0.5f) / n;
                if (pointInConvexPolygon2D(projected, pu, pv)) covered[pixel] = 1;
            }
        }
    }

    const int blocked = static_cast<int>(std::count(
        covered.begin(), covered.end(), static_cast<unsigned char>(1)));
    if (outputMask) {
        outputMask->assign((covered.size() + 63U) / 64U, 0U);
        for (std::size_t pixel = 0; pixel < covered.size(); ++pixel) {
            if (covered[pixel]) {
                (*outputMask)[pixel / 64U] |= std::uint64_t{1} << (pixel % 64U);
            }
        }
    }
    return static_cast<float>(blocked) / static_cast<float>(n * n);
}
// 体元中心奇偶性: 先对每个 (y,z) 行做 -x 方向基准射线, 再沿 x 扫描线传播
void buildParity(Ctx& c)
{
    const auto& tris = c.scene->tris;
    // FIXED[中] 数值: gx*gy*gz 以 int 相乘，体元数 >2^31 时溢出（而 cells 用 size_t 计算），parity 被分配为错误大小，后续 cellIndex 越界写；已修复：total 改为 std::size_t 逐维强制转换后相乘，parity 按该 size_t 分配；网格总数上限（阈值）未加（DEFERRED）。
    const std::size_t total = static_cast<std::size_t>(c.gx) *
                              static_cast<std::size_t>(c.gy) *
                              static_cast<std::size_t>(c.gz);
    c.parity.assign(total, 0);

    // 基准: 对每个 (y,z), 从体元 (0,y,z) 中心向 -x 射线到 x=-0.5 (网格外)
    #pragma omp parallel for schedule(dynamic, 8)
    for (int r = 0; r < c.gy * c.gz; ++r) {
        const int y = r / c.gz;
        const int z = r % c.gz;
        {
            const V3 center{0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f};
            const V3 far{ -0.5f, center.y, center.z};
            int count = 0;
            int nTested = 0, nHits = 0;
            // DEFERRED[低] 性能/并发: 在 OpenMP 并行循环内对每一行调用 std::getenv（MSVC 标记为非线程安全，且每行都做一次环境查找）；objectOfTri 为对象数的线性扫描，在最内层循环中调用；建议：getenv 移到并行区外只读一次，预先建立 tri→object 的下标数组。
            const char* dbgRow = std::getenv("HEXDBG_BASE");
            // 射线段只可能穿过体元列 x=0 (网格从 0 开始, 三角 min>=0)
            for (int x = 0; x <= 0; ++x) {
                for (std::uint32_t t : c.cells[cellIndex(c, x, y, z)].tris) {
                    const TriGeom& g = c.geom[t];
                    if (g.minX > 0.5f + 1.0e-6f) continue; // 射线段 x∈[-0.5,0.5]
                    if (!c.closedObj[c.scene->objectOfTri(t)]) continue;
                    ++nTested;
                    if (segmentHitsTriangle(far, center, g)) {
                        ++nHits; count ^= 1;
                        if (dbgRow) { int dy = 0, dz = 0; if (std::sscanf(dbgRow, "%d,%d", &dy, &dz) == 2 && y == dy && z == dz)
                            std::fprintf(stderr, "  hit tri %u (x %g..%g y %g..%g z %g..%g obj=%u)\n", t, g.minX, g.maxX, g.minY, g.maxY, g.minZ, g.maxZ, c.scene->objectOfTri(t)); }
                    }
                }
            }
            if (dbgRow) {
                int dy = 0, dz = 0;
                if (std::sscanf(dbgRow, "%d,%d", &dy, &dz) == 2 && y == dy && z == dz)
                    std::fprintf(stderr, "HEXDBG base y=%d z=%d tested=%d hits=%d parity=%d\n", y, z, nTested, nHits, count);
            }
            c.parity[cellIndex(c, 0, y, z)] = static_cast<char>(count);
        }
    }


    // 沿 x 扫描: parity[x+1] = parity[x] XOR (中心间单位段穿过的封闭面数)
    #pragma omp parallel for schedule(dynamic, 8)
    for (int r = 0; r < c.gy * c.gz; ++r) {
        const int y = r / c.gz;
        const int z = r % c.gz;
        {
            // FIXED[高] Bug: 同一三角形的 AABB 若跨越 x 与 x+1 两个胞，会同时出现在两个胞的列表中，被测试命中一次却 cross^=1 两次而抵消，导致中心连线穿过斜面时奇偶性不翻转、封闭体内部整行填充错位；已修复：把命中的三角形按索引去重后再翻转奇偶性，hits 在行内复用避免每个体元都分配。
            std::vector<std::uint32_t> hits;
            for (int x = 0; x + 1 < c.gx; ++x) {
                const V3 a{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f};
                const V3 b{static_cast<float>(x + 1) + 0.5f, a.y, a.z};
                int cross = 0;
                hits.clear();
                for (int xx = x; xx <= x + 1; ++xx) {
                    for (std::uint32_t t : c.cells[cellIndex(c, xx, y, z)].tris) {
                        const TriGeom& g = c.geom[t];
                        if (!c.closedObj[c.scene->objectOfTri(t)]) continue;
                        if (!segmentHitsTriangle(a, b, g)) continue;
                        bool duplicated = false;
                        for (std::uint32_t seen : hits) {
                            if (seen == t) { duplicated = true; break; }
                        }
                        if (duplicated) continue;
                        hits.push_back(t);
                        cross ^= 1;
                    }
                }
                c.parity[cellIndex(c, x + 1, y, z)] =
                    static_cast<char>(c.parity[cellIndex(c, x, y, z)] ^ cross);
            }
        }
    }

}

// 点 p (在体元 (x,y,z) 内部) 是否位于封闭材料内部:
// parity(center) XOR (center->p 段穿过的封闭面数), 段在体元内 => 只查本胞三角
bool pointInside(const Ctx& c, int x, int y, int z, const V3& p)
{
    const V3 center{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f};
    int parity = c.parity[cellIndex(c, x, y, z)];
    for (std::uint32_t t : c.cells[cellIndex(c, x, y, z)].tris) {
        const TriGeom& g = c.geom[t];
        if (c.closedObj[c.scene->objectOfTri(t)] && segmentHitsTriangle(center, p, g)) {
            parity ^= 1;
        }
    }
    return parity == 1;
}

// 沿轴 a 的列: 截面点 (x,y,z 中另两轴取 u,v), 列上 M 个采样点。
// 封闭: 任一样点内部 => 遮挡; 开放: 列段与开放面相交 => 遮挡。
bool columnBlockedAxis(const Ctx& c, int cellX, int cellY, int cellZ, int axis,
                       float u, float v, int n, int m)
{
    const int x = cellX, y = cellY, z = cellZ;
    const auto idx = cellIndex(c, x, y, z);
    const float u0 = (static_cast<float>(u) + 0.5f) / n;
    const float v0 = (static_cast<float>(v) + 0.5f) / n;

    for (int k = 0; k < m; ++k) {
        const float t = (static_cast<float>(k) + 0.5f) / m;
        V3 p{0, 0, 0};
        if (axis == 0) { p.x = static_cast<float>(x) + t; p.y = static_cast<float>(y) + u0; p.z = static_cast<float>(z) + v0; }
        else if (axis == 1) { p.x = static_cast<float>(x) + u0; p.y = static_cast<float>(y) + t; p.z = static_cast<float>(z) + v0; }
        else { p.x = static_cast<float>(x) + u0; p.y = static_cast<float>(y) + v0; p.z = static_cast<float>(z) + t; }
        if (pointInside(c, x, y, z, p)) return true;
    }

    // 开放表面 (零厚度): 列段到开放面的距离 <= 容差 => 遮挡
    const float distEps = 0.02f;
    V3 segA{}, segB{};
    if (axis == 0) {
        segA = {static_cast<float>(x), static_cast<float>(y) + u0, static_cast<float>(z) + v0};
        segB = {segA.x + 1, segA.y, segA.z};
    } else if (axis == 1) {
        segA = {static_cast<float>(x) + u0, static_cast<float>(y), static_cast<float>(z) + v0};
        segB = {segA.x, segA.y + 1, segA.z};
    } else {
        segA = {static_cast<float>(x) + u0, static_cast<float>(y) + v0, static_cast<float>(z)};
        segB = {segA.x, segA.y, segA.z + 1};
    }
    // 表面距离 (对封闭与开放对象均适用: 恰好位于胞边界的面也遮挡)
    for (std::uint32_t t : c.cells[idx].tris) {
        const TriGeom& g = c.geom[t];
        if (segTriDist(segA, segB, g) <= distEps) {
            return true;
        }
    }
    return false;
}

// 任意方向 d (单位) 的面元基准真值 A_true:
// 过体元中心、垂直于 d 的截面方 (半边长 0.5) 内采样, 每列沿 d 走 ±diagR 采样;
// 只统计落在体元立方体内部的截面点。
bool columnBlockedDir(const Ctx& c, int cellX, int cellY, int cellZ, const V3& d,
                      const V3& u, const V3& v, float su, float sv,
                      int n, int m, bool& inCube)
{
    const int x = cellX, y = cellY, z = cellZ;
    const V3 center{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, static_cast<float>(z) + 0.5f};
    const V3 base = center + u * su + v * sv;

    // 截面点必须在体元立方体内 (允许 eps)
    inCube = base.x >= x - 1.0e-4f && base.x <= x + 1 + 1.0e-4f &&
             base.y >= y - 1.0e-4f && base.y <= y + 1 + 1.0e-4f &&
             base.z >= z - 1.0e-4f && base.z <= z + 1 + 1.0e-4f;
    if (!inCube) return false;

    const float diag = std::sqrt(3.0f) / 2.0f;
    const V3 a = base - d * diag;
    const V3 b = base + d * diag;
    const auto idx = cellIndex(c, x, y, z);
    const V3 centerFull = center;
    for (int k = 0; k < m; ++k) {
        const float t = (static_cast<float>(k) + 0.5f) / m;
        const V3 p = a + (b - a) * t;
        if (p.x < x - 1.0e-4f || p.x > x + 1 + 1.0e-4f ||
            p.y < y - 1.0e-4f || p.y > y + 1 + 1.0e-4f ||
            p.z < z - 1.0e-4f || p.z > z + 1 + 1.0e-4f) {
            continue; // 采样点跑出体元
        }
        if (pointInside(c, x, y, z, p)) return true;
    }
    for (std::uint32_t tri : c.cells[idx].tris) {
        const TriGeom& g = c.geom[tri];
        if (segTriDist(a, b, g) <= 0.02f) {
            return true;
        }
    }
    (void)centerFull;
    return false;
}

} // namespace

HexResult compute(const HexScene& scene, const HexConfig& cfg,
                  const float* extraDirs, int nExtraDirs,
                  const char* const* dirLabels)
{
    const auto t0 = std::chrono::steady_clock::now();
    HexResult result;
    result.voxelSize = scene.voxelSize;

    // 网格范围 (体元坐标)
    // loadObj() 已将坐标平移到 gridOrigin，网格必须覆盖 [0, floor(maxPos)]。
    // 再减一次 minPos 会在 minPos 非整数时丢掉最高一层体元。
    int gx = static_cast<int>(std::floor(scene.maxPos[0])) + 1;
    int gy = static_cast<int>(std::floor(scene.maxPos[1])) + 1;
    int gz = static_cast<int>(std::floor(scene.maxPos[2])) + 1;
    // 平铺场景 (如地面) 某维厚度 < 1 体元时仍保留 1 层
    if (gx < 1) gx = 1; if (gy < 1) gy = 1; if (gz < 1) gz = 1;
    result.gridX = gx; result.gridY = gy; result.gridZ = gz;

    Ctx c;
    c.scene = &scene;
    c.gx = gx; c.gy = gy; c.gz = gz;
    c.closedObj.resize(scene.objects.size());
    for (std::size_t o = 0; o < scene.objects.size(); ++o) c.closedObj[o] = scene.objects[o].closed ? 1 : 0;

    // 几何缓存
    c.geom.resize(scene.tris.size());
    for (std::size_t t = 0; t < scene.tris.size(); ++t) {
        const auto& tr = scene.tris[t];
        TriGeom& g = c.geom[t];
        g.p0 = {tr.p0[0], tr.p0[1], tr.p0[2]};
        g.p1 = {tr.p1[0], tr.p1[1], tr.p1[2]};
        g.p2 = {tr.p2[0], tr.p2[1], tr.p2[2]};
        g.n = cross({g.p1.x - g.p0.x, g.p1.y - g.p0.y, g.p1.z - g.p0.z},
                    {g.p2.x - g.p0.x, g.p2.y - g.p0.y, g.p2.z - g.p0.z});
        g.minX = std::min(g.p0.x, std::min(g.p1.x, g.p2.x));
        g.maxX = std::max(g.p0.x, std::max(g.p1.x, g.p2.x));
        g.minY = std::min(g.p0.y, std::min(g.p1.y, g.p2.y));
        g.maxY = std::max(g.p0.y, std::max(g.p1.y, g.p2.y));
        g.minZ = std::min(g.p0.z, std::min(g.p1.z, g.p2.z));
        g.maxZ = std::max(g.p0.z, std::max(g.p1.z, g.p2.z));
    }

    // 体元空间哈希
    // DEFERRED[中] 内存: 每个体元一个 std::vector（24 B 空壳 + 堆分配），大场景（如 1000×1000×100 = 1e8 体元）仅空壳就需 2.4 GB，且 gx/gy/gz 由场景 AABB 推出、未设任何上限，离群顶点即可导致数十 GB 分配；建议：只为非空体元建立稀疏哈希（unordered_map 或按列 CSR），并在入口限制网格总数。
    c.cells.resize(static_cast<std::size_t>(gx) * gy * gz);
    for (std::size_t t = 0; t < scene.tris.size(); ++t) {
        const TriGeom& g = c.geom[t];
        // 面恰在整数格面 (如 z=1.0) 时同时属于两侧胞:
        //   min 侧减 eps 向下扩一格 (覆盖下边界接触), max 侧保持 floor(max) (覆盖上边界接触)
        // DEFERRED[中] 数值: 1.0e-9f 小于 float 在 >≈0.01 处的 ULP，g.minX - 1e-9f == g.minX，"向下扩一格"实际不生效；恰在整数格面上的面（如 z=1.0 的地面/屋顶）只登记到上侧胞，下侧胞的覆盖率与表面距离判定缺失这一面；建议：使用与坐标量级相关的 eps（如 1e-4f 体元）或 std::nextafter 处理。
        const int x0 = std::max(0, static_cast<int>(std::floor(g.minX - 1.0e-9f)));
        const int x1 = std::min(gx - 1, static_cast<int>(std::floor(g.maxX)));
        const int y0 = std::max(0, static_cast<int>(std::floor(g.minY - 1.0e-9f)));
        const int y1 = std::min(gy - 1, static_cast<int>(std::floor(g.maxY)));
        const int z0 = std::max(0, static_cast<int>(std::floor(g.minZ - 1.0e-9f)));
        const int z1 = std::min(gz - 1, static_cast<int>(std::floor(g.maxZ)));
        for (int x = x0; x <= x1; ++x)
            for (int y = y0; y <= y1; ++y)
                for (int z = z0; z <= z1; ++z)
                    c.cells[cellIndex(c, x, y, z)].tris.push_back(static_cast<std::uint32_t>(t));
    }

    buildParity(c);

    const int n = cfg.sampleN > 0 ? cfg.sampleN : 64;
    const int m = cfg.colM > 0 ? cfg.colM : 4;
    const int k = cfg.volK > 0 ? cfg.volK : 4;
    result.sampleN = n;

    // 准备方向基准
    struct DirBasis { V3 d, u, v; };
    std::vector<DirBasis> bases;
    for (int i = 0; i < nExtraDirs; ++i) {
        V3 d{extraDirs[i * 3], extraDirs[i * 3 + 1], extraDirs[i * 3 + 2]};
        const float l = length(d);
        if (l < 1.0e-6f) continue;
        d = V3{d.x / l, d.y / l, d.z / l};
        const V3 ref = std::abs(d.y) < 0.9f ? V3{0, 1, 0} : V3{1, 0, 0};
        V3 u = normalize(cross(ref, d));
        V3 v = cross(d, u);
        bases.push_back({d, u, v});
    }

    // 调试: 打印 parity 行 / 体元详情
    if (const char* env = std::getenv("HEXDBG_ROW")) {
        int dy = 0, dz = 0;
        if (std::sscanf(env, "%d,%d", &dy, &dz) == 2 && dy >= 0 && dy < gy && dz >= 0 && dz < gz) {
            std::string row;
            for (int x = 0; x < gx; ++x) {
                row += (c.parity[cellIndex(c, x, dy, dz)] ? '1' : '0');
            }
            std::fprintf(stderr, "HEXDBG row y=%d z=%d: %s\n", dy, dz, row.c_str());
        }
    }
    if (const char* env = std::getenv("HEXDBG_CELL")) {
        int dx = 0, dy = 0, dz = 0;
        if (std::sscanf(env, "%d,%d,%d", &dx, &dy, &dz) == 3 &&
            dx >= 0 && dx < gx && dy >= 0 && dy < gy && dz >= 0 && dz < gz) {
            const auto idx = cellIndex(c, dx, dy, dz);
            std::fprintf(stderr, "HEXDBG cell %d,%d,%d: parity=%d tris=%zu\n",
                         dx, dy, dz, (int)c.parity[idx], c.cells[idx].tris.size());
            for (std::uint32_t t : c.cells[idx].tris) {
                const TriGeom& g = c.geom[t];
                std::fprintf(stderr, "  tri %u obj=%u closed=%d x[%g,%g] y[%g,%g] z[%g,%g]\n", t,
                             c.scene->objectOfTri(t),
                             (int)c.closedObj[c.scene->objectOfTri(t)],
                             g.minX, g.maxX, g.minY, g.maxY, g.minZ, g.maxZ);
            }
        }
    }

    // 逐方向累加器
    struct DirAcc { double sumEst{0}, sumTrue{0}, sumAbs{0}, sumSq{0}; int cnt{0}; };
    std::vector<DirAcc> dirAcc(bases.size());

    double sumRhoAll = 0, sumRhoActive = 0, sumA = 0;
    float maxRho = 0, maxA = 0;
    long long nFull = 0, nMixed = 0;

    // 并行计算 (OpenMP): 每线程独立累加器, 结果先写入临时网格, 退出后合并压实。
    // 未定义 _OPENMP 时退化为单线程 (逻辑不变)。
    struct ThreadAcc {
        double sumRhoAll{0}, sumRhoActive{0}, sumA{0};
        float maxRho{0}, maxA{0};
        long long nFull{0}, nMixed{0}, nEmpty{0};
        std::vector<DirAcc> dir;
    };
    // DEFERRED[中] 内存: 为整个网格分配 VoxelResult（每个约 150 B，含 3 个 std::vector 与数组），而结果只保留非空体元；1e8 体元的场景仅此临时数组就需约 15 GB；建议：每线程收集非空体元到局部 vector，最后合并排序，不分配全网格临时数组。
    std::vector<VoxelResult> tmpVr((std::size_t)gx * gy * gz);
    std::vector<char> tmpKind((std::size_t)gx * gy * gz, 0); // 0=empty 1=full 2=mixed

    #pragma omp parallel
    {
        ThreadAcc acc;
        acc.dir.resize(bases.size());

        #pragma omp for schedule(dynamic, 1)
        for (int x = 0; x < gx; ++x) {
            for (int y = 0; y < gy; ++y) {
                for (int z = 0; z < gz; ++z) {
                    const auto idx = cellIndex(c, x, y, z);
                    const auto& cell = c.cells[idx];

                    // ---- 无相交三角: 由中心奇偶判 FULL / EMPTY ----
                    if (cell.tris.empty()) {
                        const bool inside = c.parity[idx] == 1;
                        if (inside && cfg.includeInteriors) {
                            VoxelResult vr;
                            vr.ix = x; vr.iy = y; vr.iz = z;
                            vr.kind = VoxelKind::FULL;
                            // DEFERRED[中] 物理: 封闭体内部体元的 rho 固定为 1（单位 m2/m3 时与体元尺寸无关），而 MIXED 体元 rho=面积/voxelSize；voxelSize≠1 时内部与表面体元密度不在同一尺度，消光 rho*G*CI 不连续；下方 acc.maxRho/maxA 直接赋值 1.0f 而非取 max，会覆盖此前更大的统计值；建议：明确 FULL 体元的消光表示（如单独标志为不透明），统计用 std::max。
                            vr.hex = {1.0f, 1.0f, 1.0f, 1.0f};
                            vr.projectedArea = {{1.0f, 1.0f, 1.0f}};
                            vr.cover = {{1.0f, 1.0f, 1.0f}};
                            const std::size_t pixelCount = static_cast<std::size_t>(n) * n;
                            for (auto& mask : vr.coverMask) {
                                mask.assign((pixelCount + 63U) / 64U, ~std::uint64_t{0});
                                if ((pixelCount & 63U) != 0U) {
                                    mask.back() = (std::uint64_t{1} << (pixelCount & 63U)) - 1U;
                                }
                            }
                            tmpVr[idx] = vr; tmpKind[idx] = 1;
                            ++acc.nFull;
                            acc.sumRhoAll += 1.0; acc.sumRhoActive += 1.0; acc.sumA += 3.0;
                            acc.maxRho = 1.0f; acc.maxA = 1.0f;
                            for (auto& da : acc.dir) { da.sumEst += 1.0; da.sumTrue += 1.0; da.cnt++; }
                        } else {
                            ++acc.nEmpty;
                        }
                        continue;
                    }

                    // ---- 有相交三角: MIXED, 采样计算 ----
                    VoxelResult vr;
                    vr.ix = x; vr.iy = y; vr.iz = z;
                    vr.kind = VoxelKind::MIXED;



                    // 叶面积密度 rho = 体元内叶片裁剪面积 / 物理体元体积。
                    // 三角形已缩放到单位体元坐标，故 rho = area_voxel / voxelSize。
                    double leafAreaVoxel = 0.0;
                    double projectedArea[3] = {0.0, 0.0, 0.0};
                    for (std::uint32_t tri : cell.tris) {
                        const TriGeom& geometry = c.geom[tri];
                        const float area = triangleAreaInsideCell(geometry, x, y, z);
                        leafAreaVoxel += area;
                        const float normalLength = length(geometry.n);
                        if (normalLength > 1.0e-12f) {
                            projectedArea[0] += area * std::abs(geometry.n.x) / normalLength;
                            projectedArea[1] += area * std::abs(geometry.n.y) / normalLength;
                            projectedArea[2] += area * std::abs(geometry.n.z) / normalLength;
                        }
                    }
                    vr.hex.rho = static_cast<float>(
                        leafAreaVoxel / std::max(scene.voxelSize, 1.0e-6f));

                    // 投影覆盖率 C 不是 CI。先由投影并集得到空隙率 Pgap=1-C，
                    // 再以未重叠投影叶面积 P 归一化：CI=-ln(Pgap)/P。
                    // CI=1 为随机分布，CI<1 为聚集分布，CI>1 为规则分布。
                    for (int axis = 0; axis < 3; ++axis) {
                        vr.projectedArea[axis] = static_cast<float>(projectedArea[axis]);
                        const float cover = projectionCoverageAxis(
                            c, x, y, z, axis, n, &vr.coverMask[axis]);
                        vr.cover[axis] = cover;
                        float ci = 1.0f;
                        // DEFERRED[低] 数值: 覆盖率饱和（C=1）时 gap 被钳到 0.5/N²，CI=-ln(gap)/P 的值完全由采样分辨率 N 决定（N=64 时约 9/P），不同 sampleN 的结果不可比；P 很小而 C>0（掠射面）时 CI 可达极大值；建议：对 CI 设物理上限（如 ≤ 1/P·ln(N²) 并注明），或对饱和体元单独标记。
                        if (projectedArea[axis] > 1.0e-8 && cover > 0.0f) {
                            const float minimumGap = 0.5f / static_cast<float>(n * n);
                            const float gap = std::max(1.0f - cover, minimumGap);
                            ci = std::max(
                                static_cast<float>(-std::log(gap) / projectedArea[axis]),
                                0.0f);
                        }
                        if (axis == 0) vr.hex.ax = ci;
                        else if (axis == 1) vr.hex.ay = ci;
                        else vr.hex.az = ci;
                    }

                    // 任意方向: 外推估计 vs 面元基准真值
                    for (std::size_t bi = 0; bi < bases.size(); ++bi) {
                        const auto& bb = bases[bi];
                        const float aest = aExtrap(vr.hex, bb.d.x, bb.d.y, bb.d.z);
                        int blocked = 0, inCount = 0;
                        for (int su = 0; su < n; ++su) {
                            for (int sv = 0; sv < n; ++sv) {
                                const float uu = (static_cast<float>(su) + 0.5f) / n - 0.5f;
                                const float vv = (static_cast<float>(sv) + 0.5f) / n - 0.5f;
                                bool inCube = false;
                                if (columnBlockedDir(c, x, y, z, bb.d, bb.u, bb.v, uu, vv, n, m, inCube)) {
                                    if (inCube) { ++blocked; ++inCount; }
                                } else if (inCube) {
                                    ++inCount;
                                }
                            }
                        }
                        const float atrue = inCount > 0 ? static_cast<float>(blocked) / static_cast<float>(inCount) : 0.0f;
                        acc.dir[bi].sumEst += aest;
                        acc.dir[bi].sumTrue += atrue;
                        const float e = aest - atrue;
                        acc.dir[bi].sumAbs += std::abs(e);
                        acc.dir[bi].sumSq += e * e;
                        acc.dir[bi].cnt++;
                    }

                    tmpVr[idx] = vr; tmpKind[idx] = 2;
                    ++acc.nMixed;
                    acc.sumRhoAll += vr.hex.rho; acc.sumRhoActive += vr.hex.rho;
                    acc.sumA += vr.hex.ax + vr.hex.ay + vr.hex.az;
                    acc.maxRho = std::max(acc.maxRho, vr.hex.rho);
                    acc.maxA = std::max({acc.maxA, vr.hex.ax, vr.hex.ay, vr.hex.az});
                }
            }
        }

        // 合并线程累加器
        #pragma omp critical
        {
            sumRhoAll += acc.sumRhoAll;
            sumRhoActive += acc.sumRhoActive;
            sumA += acc.sumA;
            maxRho = std::max(maxRho, acc.maxRho);
            maxA = std::max(maxA, acc.maxA);
            nFull += acc.nFull;
            nMixed += acc.nMixed;
            result.nEmpty += acc.nEmpty;
            for (std::size_t bi = 0; bi < bases.size(); ++bi) {
                dirAcc[bi].sumEst += acc.dir[bi].sumEst;
                dirAcc[bi].sumTrue += acc.dir[bi].sumTrue;
                dirAcc[bi].sumAbs += acc.dir[bi].sumAbs;
                dirAcc[bi].sumSq += acc.dir[bi].sumSq;
                dirAcc[bi].cnt += acc.dir[bi].cnt;
            }
        }
    }

    // 压实: 临时网格 -> result.voxels (之后统一排序)
    for (std::size_t idx = 0; idx < tmpKind.size(); ++idx) {
        if (tmpKind[idx]) result.voxels.push_back(tmpVr[idx]);
    }

    result.nFull = nFull;
    result.nMixed = nMixed;
    const long long total = static_cast<long long>(gx) * gy * gz;
    const long long active = nFull + nMixed;
    result.meanRhoAll = total > 0 ? static_cast<float>(sumRhoAll / total) : 0.0f;
    result.meanRhoActive = active > 0 ? static_cast<float>(sumRhoActive / active) : 0.0f;
    result.meanA = active > 0 ? static_cast<float>(sumA / (3.0 * active)) : 0.0f;
    result.maxRho = maxRho;
    result.maxA = maxA;

    for (std::size_t bi = 0; bi < bases.size(); ++bi) {
        DirStats ds;
        ds.dir[0] = bases[bi].d.x; ds.dir[1] = bases[bi].d.y; ds.dir[2] = bases[bi].d.z;
        if (dirLabels && dirLabels[bi]) ds.label = dirLabels[bi];
        if (dirAcc[bi].cnt > 0) {
            const int cnt = dirAcc[bi].cnt;
            ds.meanAest = static_cast<float>(dirAcc[bi].sumEst / cnt);
            ds.meanAtrue = static_cast<float>(dirAcc[bi].sumTrue / cnt);
            ds.meanAbsErr = static_cast<float>(dirAcc[bi].sumAbs / cnt);
            ds.rmsErr = static_cast<float>(std::sqrt(dirAcc[bi].sumSq / cnt));
        }
        ds.nVoxels = dirAcc[bi].cnt;
        result.dirStats.push_back(ds);
    }

    // 按网格坐标排序, 便于阅读
    std::sort(result.voxels.begin(), result.voxels.end(),
              [](const VoxelResult& a, const VoxelResult& b) {
                  if (a.ix != b.ix) return a.ix < b.ix;
                  if (a.iy != b.iy) return a.iy < b.iy;
                  return a.iz < b.iz;
              });

    const auto t1 = std::chrono::steady_clock::now();
    result.computeMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    return result;
}

// ---------------------------------------------------------------------------
// 输出
// ---------------------------------------------------------------------------

std::string toTsv(const HexResult& r)
{
    std::ostringstream os;
    os << "ix\tiy\tiz\tkind\tax\tay\taz\trho\n";
    for (const auto& v : r.voxels) {
        const int kind = static_cast<int>(v.kind);
        os << v.ix << '\t' << v.iy << '\t' << v.iz << '\t' << kind
           << '\t' << v.hex.ax << '\t' << v.hex.ay << '\t' << v.hex.az
           << '\t' << v.hex.rho << '\n';
    }
    return os.str();
}

std::string toJson(const HexResult& r, const std::string& objPath)
{
    std::ostringstream os;
    os << "{\n";
    os << "  \"method\": \"hex-voxel (facet projection aggregation)\",\n";
    // FIXED[低] Bug: objPath 与 label 未做 JSON 转义，Windows 路径中的反斜杠（C:\data\tree.obj）或引号会生成非法 JSON；已修复：新增本地 jsonEscape（转义反斜杠、双引号、换行、回车、制表）并用于 objPath 与每个 label；"grid" 仍输出为含一个字符串的数组，字段语义未改（DEFERRED）。
    const auto jsonEscape = [](const std::string& in) {
        std::string out;
        out.reserve(in.size());
        for (char ch : in) {
            switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += ch; break;
            }
        }
        return out;
    };
    os << "  \"obj\": \"" << jsonEscape(objPath) << "\",\n";
    os << "  \"grid\": [\"" << r.gridX << ", " << r.gridY << ", " << r.gridZ << "\"],\n";
    os << "  \"n_empty\": " << r.nEmpty << ",\n";
    os << "  \"n_full\": " << r.nFull << ",\n";
    os << "  \"n_mixed\": " << r.nMixed << ",\n";
    os << "  \"mean_rho_all\": " << r.meanRhoAll << ",\n";
    os << "  \"mean_rho_active\": " << r.meanRhoActive << ",\n";
    os << "  \"mean_A\": " << r.meanA << ",\n";
    os << "  \"max_A\": " << r.maxA << ",\n";
    os << "  \"max_rho\": " << r.maxRho << ",\n";
    os << "  \"compute_ms\": " << r.computeMs << ",\n";
    os << "  \"directions\": [\n";
    for (std::size_t i = 0; i < r.dirStats.size(); ++i) {
        const auto& d = r.dirStats[i];
        os << "    {\"dir\": [" << d.dir[0] << ", " << d.dir[1] << ", " << d.dir[2]
           << "], \"label\": \"" << jsonEscape(d.label) << "\", \"n_voxels\": " << d.nVoxels
           << ", \"mean_A_est\": " << d.meanAest << ", \"mean_A_true\": " << d.meanAtrue
           << ", \"mean_abs_err\": " << d.meanAbsErr << ", \"rms_err\": " << d.rmsErr << "}";
        if (i + 1 < r.dirStats.size()) os << ",";
        os << "\n";
    }
    os << "  ]\n";
    os << "}\n";
    return os.str();
}

}  // namespace hexvoxel
