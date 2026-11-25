// ==========================================================================
// 文件：voxeldesigner.cpp
// 功能：VoxelDesigner 的实现——把参数化形状（长方体/椭球/背景平面）和高度栅格（tif）
//       离散成整数体元坐标，并生成体元模式使用的三角网格 PrimMesh。
// 主要模块：
//   1. 体元离散：loadModel 分派；createEllipsoid / createCube / createBackground
//   2. 体元集合 → 网格：createTriVoxels（介质：三张中心正交面；实体：外表面）
//   3. 参数化形状网格：createTriCube、createTriCube_wall / _roof（建筑墙体与屋顶）、
//      createTriEllipsoid、createTriBackground、createTriEntity（按类型分派）
//   4. 工具：outputPrimMesh（导出 OBJ 调试）、isInPloy（点在凸四边形内判定）
//   5. 高度栅格：createTriEntitiesFromTif、_roof、_wall（表层体元 + 地表/墙面面片）
//      （文件中间一段被注释掉的 DEM 背景实现已停用，仅作历史参考）
// 数据流：scene.cpp 传入 Shape / 高度图路径与 stepSize → 本文件离散并生成 PrimMesh →
//         scene.cpp 把 PrimMesh 并入场景（体元 id 写入 NanoVDB、顶点建 BLAS）。
// 约定：体元坐标 (i, j, k) 为整数，x/y/z 分别对应 length/width/height，z 向上；
//       体元的 id 是其最小角（左下）坐标，体元 AABB 为 [id, id+1]；网格顶点单位是体元（不是米），
//       输入尺寸与 stepSize 的单位为米；meshcenter 个别函数以米计，见 DEFERRED 说明。
// ==========================================================================

#include "voxeldesigner.h"
#include "voxel_boundary.h"
#include <stdexcept>
#include <string>
#include <nvh/gltfscene.cpp>
//#include "autogen/VoxelDesigner.comp.h"
// using BufferT = nanovdb::HostBuffer;

// 模块：按形状类型体元化并写入 m_voxelModel。
// 输入：shapeType（CUBE / ELLIPSOID / PLANE，其余按 CUBE）、shape 尺寸（米）、stepSize（体元边长，米）。
// 输出：无返回值；m_voxelModel.centerPoints / aabbs 被覆盖（各 create* 函数的副作用）。
void VoxelDesigner::loadModel(ShapeType shapeType, Shape shape, float stepSize)
{
    // 填充m_voxelModel内部的aabbs
    switch (shapeType)
    {
    case ShapeType::CUBE:
        createCube(shape.length, shape.width, shape.height, stepSize);
        break;
    case ShapeType::ELLIPSOID:
        createEllipsoid(shape.height, shape.width, stepSize);
        break;
    case ShapeType::PLANE:
        createBackground(shape.length, shape.width, stepSize);
        break;
    default:
        createCube(shape.length, shape.width, shape.height, stepSize);
        break;
    }
}

// 模块：椭球体元化（体元坐标 ivec3，单位为体元个数）。
// 输入：height（z 向）、width（同时用作 x 向 length，即水平截面为圆）、stepSize，均为米。
// 输出：返回落在椭球内的体元整数坐标，同时写入 m_voxelModel.centerPoints 与 aabbs
//       （aabb = [coord, coord+1]，坐标为体元左下角，不是体元中心）。
// 说明：参数非正或非有限时清空 m_voxelModel 并返回空集合，避免除零与超大循环。
// 均为xyz坐标
std::vector<glm::ivec3> VoxelDesigner::createEllipsoid(float height, float width, float stepSize) // Shape.length...
{
    std::vector<glm::ivec3> discreteCoord;
    std::vector<Aabb> aabbs;

    if (!(stepSize > 0.0f) || !(height > 0.0f) || !(width > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(height) || !std::isfinite(width)) {
        m_voxelModel.centerPoints.clear();
        m_voxelModel.aabbs.clear();
        return discreteCoord;
    }
    // 步骤 1：各方向体元数取 ceil，保证覆盖完整尺寸；椭球中心取体元数的一半。
    int widthSize = std::ceil(width / stepSize);
    int heightSize = std::ceil(height / stepSize);
    int lengthSize = std::ceil(width / stepSize);

    float widthCenter = widthSize / 2.0;
    float heightCenter = heightSize / 2.0;
    float lengthCenter = lengthSize / 2.0;

    // 步骤 2：遍历外接长方体内全部体元，用椭球方程 (x/a)^2+(y/b)^2+(z/c)^2 <= 1 判定是否保留。
    for (int i = 0; i < lengthSize; i++)
    {
        for (int j = 0; j < widthSize; j++)
        {
            for (int k = 0; k < heightSize; k++)
            {
                // move to center of 1x1 voxel
                // (coord + 0.5) - (center + 0.5) = coord - center
                float dist = (i - lengthCenter) * (i - lengthCenter) / (lengthCenter * lengthCenter) +
                             (j - widthCenter) * (j - widthCenter) / (widthCenter * widthCenter) +
                             (k - heightCenter) * (k - heightCenter) / (heightCenter * heightCenter);
                if (dist <= 1.0)
                {
                    // ellipsoid.activedNum++;
                    glm::vec3 coord = {i, j, k};
                    Aabb aabb = { {coord},
                                 {coord + glm::vec3(1)}};
                    discreteCoord.emplace_back(coord);
                    aabbs.emplace_back(aabb);
                }
            }
        }
    }

    /*if (ellipsoid.activedNum < 1)
    {
        int point[3] = {int(widthCenter), int(widthCenter), int(widthCenter)};
        ellipsoid.voxels.emplace_back(point);
        ellipsoid.activedNum++;
    }*/

    m_voxelModel.centerPoints = discreteCoord;
    m_voxelModel.aabbs = aabbs;
    return discreteCoord;
}


// 模块：长方体体元化。
// 输入：length（x）、width（y）、height（z），单位米；stepSize 体元边长（米）。
// 输出：返回 ceil(size/step) 个方向的全部体元整数坐标，并写入 m_voxelModel（同 createEllipsoid）。
// 说明：参数非正或非有限时清空 m_voxelModel 并返回空集合。
std::vector<glm::ivec3> VoxelDesigner::createCube(float length, float width, float height, float stepSize)
{
    std::vector<glm::ivec3> discreteCoord;
    std::vector<Aabb> aabbs;

    if (!(stepSize > 0.0f) || !(length > 0.0f) || !(width > 0.0f) || !(height > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(length) || !std::isfinite(width) || !std::isfinite(height)) {
        m_voxelModel.centerPoints.clear();
        m_voxelModel.aabbs.clear();
        return discreteCoord;
    }

    int widthSize = std::ceil(width / stepSize);
    int heightSize = std::ceil(height / stepSize);
    int lengthSize = std::ceil(length / stepSize);

    float widthCenter = widthSize / 2.0;
    float heightCenter = heightSize / 2.0;
    float lengthCenter = lengthSize/ 2.0;

    // i,j为层，k为列
    for (int k = 0; k < heightSize; k++)
    {
    for (int i = 0; i < lengthSize; i++)
    {
        for (int j = 0; j < widthSize; j++)
        {
            
                glm::vec3 coord = {i, j, k};
                Aabb aabb = {{coord},
                             {coord + glm::vec3(1)}};
                discreteCoord.emplace_back(coord);
                aabbs.emplace_back(aabb);
            }
        }
    }

    m_voxelModel.centerPoints = discreteCoord;
    m_voxelModel.aabbs = aabbs;
    return discreteCoord;
}

// 模块：由体元坐标集合生成三角网格。
// 输入：voxelIds（体元整数坐标）；solidBoundary 为 true 表示不透明实体（建筑/地表等），
//       false 表示参与介质（如树冠湍流体元）。
// 输出：PrimMesh（顶点单位为体元；voxelIds / faceIds / isValids 逐体元一条；
//       meshcenter 为体元中心的平均位置）。
// 说明：实体只输出集合外表面（voxel_boundary::emit）；介质则每个体元输出三张正交的
//       中心面（z+0.5、x+0.5、y+0.5），用于射线与体元的积分。
PrimMesh VoxelDesigner::createTriVoxels(const std::vector<glm::ivec3>& voxelIds, bool solidBoundary)
{
    PrimMesh mesh{};
    mesh.meshId = 0;

    // 步骤 1：四边形追加辅助函数，按 (0,1,2)(0,2,3) 拆成两个三角形，顶点不共享。
    auto appendQuad = [&mesh](const glm::vec3& p0, const glm::vec3& p1,
                              const glm::vec3& p2, const glm::vec3& p3) {
        const glm::vec3 points[4] = {p0, p1, p2, p3};
        const uint32_t order[6] = {0, 1, 2, 0, 2, 3};
        for (uint32_t index : order) {
            VertexAttribute vertex{};
            vertex.pos = points[index];
            mesh.indices.emplace_back(static_cast<uint32_t>(mesh.vertices.size()));
            mesh.vertices.emplace_back(vertex);
        }
    };

    if (solidBoundary) {
        voxel_boundary::emit(voxelIds, [&](const voxel_boundary::Quad& quad, const voxel_boundary::Cell&) {
            const auto point = [&](int i) { return glm::vec3(quad[i][0], quad[i][1], quad[i][2]); };
            appendQuad(point(0), point(1), point(2), point(3));
        });
    }
    glm::vec3 centerSum(0.0f);
    for (const glm::ivec3& id : voxelIds) {
        const glm::vec3 base(id);
        const float x = base.x;
        const float y = base.y;
        const float z = base.z;

        // Participating media retain interior integration planes. Opaque cells
        // use the closed outward boundary above, never these crossed planes.
        if (!solidBoundary) {
        appendQuad({x, y, z + 0.5f}, {x + 1.0f, y, z + 0.5f},
                   {x + 1.0f, y + 1.0f, z + 0.5f}, {x, y + 1.0f, z + 0.5f});
        appendQuad({x + 0.5f, y, z}, {x + 0.5f, y + 1.0f, z},
                   {x + 0.5f, y + 1.0f, z + 1.0f}, {x + 0.5f, y, z + 1.0f});
        appendQuad({x, y + 0.5f, z}, {x, y + 0.5f, z + 1.0f},
                   {x + 1.0f, y + 0.5f, z + 1.0f}, {x + 1.0f, y + 0.5f, z});
        }

        mesh.voxelIds.emplace_back(id);
        mesh.isValids.emplace_back(int5{1, 1, 1, 1, 1});
        mesh.faceIds.emplace_back(0);
        centerSum += base + glm::vec3(0.5f);
    }

    mesh.nVertices = static_cast<uint32_t>(mesh.vertices.size());
    mesh.nIndices = static_cast<uint32_t>(mesh.indices.size());
    mesh.meshcenter = voxelIds.empty() ? glm::vec3(0.0f)
                                      : centerSum / static_cast<float>(voxelIds.size());
    return mesh;
}

// 模块：长方体的三角网格（体元内部切面 + 外表面），顶点单位为体元。
// 输入：shape（length/width/height，米）、stepSize（米）；输出：PrimMesh（参数非法时返回空网格）。
// 说明：每个体元生成三张过其最小角的面（z=k、x=i、y=j），再补顶面与两个最大面；
//       voxelIds 只在第三个（x-z 平面）循环里追加，每个体元一条。
PrimMesh VoxelDesigner::createTriCube(Shape shape, float stepSize)
{
    PrimMesh voxelTriModel;
    voxelTriModel.nVertices = 0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;

    if (!(stepSize > 0.0f) || !(shape.width > 0.0f) || !(shape.height > 0.0f) || !(shape.length > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(shape.width) || !std::isfinite(shape.height) || !std::isfinite(shape.length)) {
        return voxelTriModel;
    }

    // DEFERRED[中] Bug: 层数循环 k < height/stepSize 等价于 ceil（非整数倍时多出一层），而顶面/最大面位置用 round(height/stepSize)；例如 height=10、step=3 时体元到 z=4，顶面却在 z=3，盒子不封闭且顶面穿过体元内部；同时 voxelIds 只在 x-z 平面循环中追加；meshcenter 以米为单位而顶点以体元为单位，stepSize≠1 时中心错误；建议：统一用 n=ceil(size/step) 作为层数与外表面位置，meshcenter 用 n/2。
    // 步骤 1：每个体元一张 z=k 的水平面（x-y 平面）；以下三个循环依次生成三个方向的面。
    // 绘制每一层x,y平面
    for (int k = 0; k < shape.height / stepSize; k++) //height,绘制每一层的面
    {
        for (int i = 0; i < shape.length / stepSize; i++)
        {
            for (int j = 0; j < shape.width / stepSize; j++)
            {
                glm::vec3 points[4];
                points[0].x = i;
                points[0].y = j;
                points[0].z = k;

                points[1].x = (i);
                points[1].y = (j + 1);
                points[1].z = k;

                points[2].x = (i + 1);
                points[2].y = (j + 1);
                points[2].z = k;

                points[3].x = (i + 1);
                points[3].y = (j);
                points[3].z = k;

                 int list[] = {0, 1, 2, 0, 2, 3};
                for (int kk = 0; kk < 6; kk++)
                {
                    int listK = list[kk];
                    VertexAttribute va{points[listK]};
                    voxelTriModel.vertices.emplace_back(va);

                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }
        }
    }
    
    // 绘制每一层y,z平面
    for (int k = 0; k < shape.height / stepSize; k++) //height,绘制每一层的面
    {
        for (int i = 0; i < shape.length / stepSize; i++)
        {
            for (int j = 0; j < shape.width / stepSize; j++)
            {
                glm::vec3 points[4];
                points[0].x = i;
                points[0].y = j;
                points[0].z = k;

                points[1].x = i;
                points[1].y = (j + 1);
                points[1].z = k;

                points[2].x = i;
                points[2].y = (j + 1);
                points[2].z = (k + 1);

                points[3].x = i;
                points[3].y = j;
                points[3].z = (k + 1);

                 int list[] = {0, 1, 2, 0, 2, 3};
                for (int kk = 0; kk < 6; kk++)
                {
                    int listK = list[kk];
                    VertexAttribute va{points[listK]};
                    voxelTriModel.vertices.emplace_back(va);

                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }
        }
    }
   
    // 绘制每一层x,z平面
    for (int k = 0; k < shape.height / stepSize; k++) //height,绘制每一层的面
    {
        for (int i = 0; i < shape.length / stepSize; i++)
        {
            for (int j = 0; j < shape.width / stepSize; j++)
            {
                glm::vec3 points[4];
                points[0].x = i;
                points[0].y = j;
                points[0].z = k;

                points[1].x = (i + 1);
                points[1].y = j;
                points[1].z = k;

                points[2].x = (i + 1);
                points[2].y = j;
                points[2].z = (k + 1);

                points[3].x = i;
                points[3].y = j ;
                points[3].z = (k + 1);

                int list[] = {0, 1, 2, 0, 2, 3};
                for (int kk = 0; kk < 6; kk++)
                {
                    int listK = list[kk];
                    VertexAttribute va{points[listK]};
                    voxelTriModel.vertices.emplace_back(va);

                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
                voxelTriModel.voxelIds.emplace_back(glm::vec3{i, j, k});
            }
        }
    }

    // 步骤 2：补三个正方向外表面（顶面 z、x 最大面、y 最大面），位置取 round(尺寸/stepSize)。
    // 绘制顶面(x,y)
    for (int i = 0; i < shape.length / stepSize; i++)
    {
        for (int j = 0; j < shape.width / stepSize; j++)
        {
            glm::vec3 points[4];
            points[0].x = i;
            points[0].y = j ;
            points[0].z = round(shape.height /stepSize);

            points[1].x = (i);
            points[1].y = (j + 1);
            points[1].z = round(shape.height/stepSize);

            points[2].x = (i + 1);
            points[2].y = (j + 1);
            points[2].z = round(shape.height/stepSize);

            points[3].x = (i + 1);
            points[3].y = (j);
            points[3].z = round(shape.height/stepSize);

            int list[] = {0, 1, 2, 0, 2, 3};
            for (int kk = 0; kk < 6; kk++)
            {
                int listK = list[kk];
                VertexAttribute va{points[listK]};
                voxelTriModel.vertices.emplace_back(va);

                voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
            }

            voxelTriModel.nVertices += 6;
            voxelTriModel.nIndices += 6;
        }
    }

    // 绘制y,z最大面
    for (int k = 0; k < shape.height / stepSize; k++) //height,绘制每一层的面
    {
            for (int j = 0; j < shape.width / stepSize; j++)
            {
                glm::vec3 points[4];
                points[0].x = round(shape.length/stepSize);
                points[0].y = j;
                points[0].z = k;

                points[1].x = round(shape.length/stepSize);
                points[1].y = (j + 1);
                points[1].z = k;

                points[2].x = round(shape.length/stepSize);
                points[2].y = (j + 1);
                points[2].z = (k + 1);

                points[3].x = round(shape.length/stepSize);
                points[3].y = j;
                points[3].z = (k + 1);

                int list[] = {0, 1, 2, 0, 2, 3};
                for (int kk = 0; kk < 6; kk++)
                {
                    int listK = list[kk];
                    VertexAttribute va{points[listK]};
                    voxelTriModel.vertices.emplace_back(va);

                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }
    }

    // 绘制x,z最大面
    for (int k = 0; k < shape.height / stepSize; k++) //height,绘制每一层的面
    {
        for (int i = 0; i < shape.length / stepSize; i++)
        {
                glm::vec3 points[4];
                points[0].x = i;
                points[0].y = round(shape.width/stepSize);
                points[0].z = k;

                points[1].x = (i + 1);
                points[1].y = round(shape.width/stepSize);
                points[1].z = k;

                points[2].x = (i + 1);
                points[2].y = round(shape.width/stepSize);
                points[2].z = (k + 1);

                points[3].x = i;
                points[3].y = round(shape.width/stepSize);
                points[3].z = (k + 1);

                int list[] = {0, 1, 2, 0, 2, 3};
                for (int kk = 0; kk < 6; kk++)
                {
                    int listK = list[kk];
                    VertexAttribute va{points[listK]};
                    voxelTriModel.vertices.emplace_back(va);

                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
        }
    }


    // 计算模型的x,y平面坐标下中心点位置
   voxelTriModel.meshcenter = glm::vec3(shape.length / 2.0, shape.width / 2.0, 0 / 2.0);

    return voxelTriModel;
}

// 模块：建筑墙体网格（长方体去掉最顶一层）。顶点单位为体元。
// 输入：shape（length/width/height，米）、stepSize（米）；输出：PrimMesh（参数非法时为空网格）。
// 说明：层循环为 k < height/stepSize - 1，即只生成下面的 H-1 层，最顶层交给 createTriCube_roof；
//       每个体元三张面（z=k、x=i、y=j），再补 x/y 方向最大面；voxelIds / isValids / faceIds
//       在 x-z 面循环里逐体元追加；meshcenter 为米单位的水平中心（与顶点单位不同，见 DEFERRED）。
PrimMesh VoxelDesigner::createTriCube_wall(Shape shape, float stepSize)
{
    PrimMesh voxelTriModel;
    voxelTriModel.nVertices = 0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;

    if (!(stepSize > 0.0f) || !(shape.width > 0.0f) || !(shape.height > 0.0f) || !(shape.length > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(shape.width) || !std::isfinite(shape.height) || !std::isfinite(shape.length)) {
        return voxelTriModel;
    }

    // 步骤 1：依次生成每个体元的 z=k（x-y）、x=i（y-z）、y=j（x-z）三个方向的面。
    // 绘制每一层x,y平面
    for (int k = 0; k < shape.height / stepSize-1; k++) //height,绘制每一层的面
    {
        for (int i = 0; i < shape.length / stepSize; i++)
        {
            for (int j = 0; j < shape.width / stepSize; j++)
            {
                glm::vec3 points[4];
                points[0].x = i;
                points[0].y = j;
                points[0].z = k;

                points[1].x = (i);
                points[1].y = (j + 1);
                points[1].z = k;

                points[2].x = (i + 1);
                points[2].y = (j + 1);
                points[2].z = k;

                points[3].x = (i + 1);
                points[3].y = (j);
                points[3].z = k;

                int list[] = {0, 1, 2, 0, 2, 3};
                for (int kk = 0; kk < 6; kk++)
                {
                    int listK = list[kk];
                    VertexAttribute va{points[listK]};
                    voxelTriModel.vertices.emplace_back(va);

                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }
        }
    }

    // 绘制每一层y,z平面
    for (int k = 0; k < shape.height / stepSize-1; k++) //height,绘制每一层的面
    {
        for (int i = 0; i < shape.length / stepSize; i++)
        {
            for (int j = 0; j < shape.width / stepSize; j++)
            {
                glm::vec3 points[4];
                points[0].x = i;
                points[0].y = j;
                points[0].z = k;

                points[1].x = i;
                points[1].y = (j + 1);
                points[1].z = k;

                points[2].x = i;
                points[2].y = (j + 1);
                points[2].z = (k + 1);

                points[3].x = i;
                points[3].y = j;
                points[3].z = (k + 1);

                int list[] = {0, 1, 2, 0, 2, 3};
                for (int kk = 0; kk < 6; kk++)
                {
                    int listK = list[kk];
                    VertexAttribute va{points[listK]};
                    voxelTriModel.vertices.emplace_back(va);

                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }
        }
    }

    // 绘制每一层x,z平面
    for (int k = 0; k < shape.height / stepSize-1; k++) //height,绘制每一层的面
    {
        for (int i = 0; i < shape.length / stepSize; i++)
        {
            for (int j = 0; j < shape.width / stepSize; j++)
            {
                glm::vec3 points[4];
                points[0].x = i;
                points[0].y = j;
                points[0].z = k;

                points[1].x = (i + 1);
                points[1].y = j;
                points[1].z = k;

                points[2].x = (i + 1);
                points[2].y = j;
                points[2].z = (k + 1);

                points[3].x = i;
                points[3].y = j ;
                points[3].z = (k + 1);

                int list[] = {0, 1, 2, 0, 2, 3};
                for (int kk = 0; kk < 6; kk++)
                {
                    int listK = list[kk];
                    VertexAttribute va{points[listK]};
                    voxelTriModel.vertices.emplace_back(va);

                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
                voxelTriModel.voxelIds.emplace_back(glm::vec3{i, j, k});
                voxelTriModel.isValids.emplace_back(int5{1, 1, 1, 1, 1});
                voxelTriModel.faceIds.emplace_back(0);
            }
        }
    }


    // 步骤 2：补 x 最大面（y-z 平面）与 y 最大面（x-z 平面），使墙体外表面封闭（见下方 DEFERRED）。
    // 绘制y,z最大面
    // DEFERRED[中] Bug: 外侧最大面直接放在 shape.length/stepSize（未取整的浮点），而 i/j 循环按 ceil 生成格子；尺寸不是步长整数倍时外墙落在最后一列体元内部（如 2.5），墙面不封闭且与 voxelIds 错位；墙体各层 k < height/step-1 也与 roof 的 k=int(height/step)-1 取整规则不同，可能缺层或重叠；建议：统一用 n=ceil(size/step)（或 round）作为格数与外表面坐标。
    for (int k = 0; k < shape.height / stepSize-1; k++) //height,绘制每一层的面
    {
        for (int j = 0; j < shape.width / stepSize; j++)
        {
            glm::vec3 points[4];
            points[0].x = shape.length/stepSize;
            points[0].y = j;
            points[0].z = k;

            points[1].x = shape.length/ stepSize;
            points[1].y = (j + 1);
            points[1].z = k;

            points[2].x = shape.length/ stepSize;
            points[2].y = (j + 1);
            points[2].z = (k + 1);

            points[3].x = shape.length/ stepSize;
            points[3].y = j;
            points[3].z = (k + 1);

            int list[] = {0, 1, 2, 0, 2, 3};
            for (int kk = 0; kk < 6; kk++)
            {
                int listK = list[kk];
                VertexAttribute va{points[listK]};
                voxelTriModel.vertices.emplace_back(va);

                voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
            }

            voxelTriModel.nVertices += 6;
            voxelTriModel.nIndices += 6;
        }
    }

    // 绘制x,z最大面
    for (int k = 0; k < shape.height / stepSize-1; k++) //height,绘制每一层的面
    {
        for (int i = 0; i < shape.length / stepSize; i++)
        {
            glm::vec3 points[4];
            points[0].x = i;
            points[0].y = shape.width/ stepSize;
            points[0].z = k;

            points[1].x = (i + 1);
            points[1].y = shape.width/ stepSize;
            points[1].z = k;

            points[2].x = (i + 1);
            points[2].y = shape.width/ stepSize;
            points[2].z = (k + 1);

            points[3].x = i;
            points[3].y = shape.width/ stepSize;
            points[3].z = (k + 1);

            int list[] = {0, 1, 2, 0, 2, 3};
            for (int kk = 0; kk < 6; kk++)
            {
                int listK = list[kk];
                VertexAttribute va{points[listK]};
                voxelTriModel.vertices.emplace_back(va);

                voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
            }

            voxelTriModel.nVertices += 6;
            voxelTriModel.nIndices += 6;
        }
    }


    // 计算模型的x,y平面坐标下中心点位置
    voxelTriModel.meshcenter = glm::vec3(shape.length / 2.0, shape.width / 2.0, 0 / 2.0);

    return voxelTriModel;
}

// 模块：建筑屋顶网格——只含最顶一层（k = height/stepSize - 1）的顶面，与 createTriCube_wall 配合。
// 输入：shape（米）、stepSize（米）；输出：PrimMesh，顶面位于 z = k+1，voxelIds 的 z 为 k。
// 说明：顶点单位为体元；meshcenter 为米单位的水平中心（与顶点单位不一致，待核对）。
PrimMesh VoxelDesigner::createTriCube_roof(Shape shape, float stepSize)
{
    PrimMesh voxelTriModel;
    voxelTriModel.nVertices = 0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;

    if (!(stepSize > 0.0f) || !(shape.width > 0.0f) || !(shape.height > 0.0f) || !(shape.length > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(shape.width) || !std::isfinite(shape.height) || !std::isfinite(shape.length)) {
        return voxelTriModel;
    }

    // 步骤 1：为最顶层每个 (i, j) 体元生成一张水平顶面，并登记该体元的 voxelId。
    // 绘制顶面(x,y)
    // DEFERRED[中] 数值: float→int 截断；height<stepSize 时 k=-1，屋顶 voxelIds 的 z 为负（插入 NanoVDB 时落在地面以下/越界），height 不是步长整数倍时屋顶高度被向下取整而墙体按 ceil 计层；建议：k=max(0, (int)std::ceil(height/step)-1) 并与墙体统一取整。
    int k = shape.height/stepSize - 1;
    for (int i = 0; i < shape.length / stepSize; i++)
    {
        for (int j = 0; j < shape.width / stepSize; j++)
        {
            glm::vec3 points[4];
            points[0].x = i;
            points[0].y = j ;
            points[0].z = k+1;

            points[1].x = (i);
            points[1].y = (j + 1);
            points[1].z = k+1;

            points[2].x = (i + 1);
            points[2].y = (j + 1);
            points[2].z = k+1;

            points[3].x = (i + 1);
            points[3].y = (j);
            points[3].z = k+1;

            int list[] = {0, 1, 2, 0, 2, 3};
            for (int kk = 0; kk < 6; kk++)
            {
                int listK = list[kk];
                VertexAttribute va{points[listK]};
                voxelTriModel.vertices.emplace_back(va);

                voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kk);
            }

            voxelTriModel.nVertices += 6;
            voxelTriModel.nIndices += 6;
            voxelTriModel.voxelIds.emplace_back(glm::vec3{i, j, k});
            voxelTriModel.isValids.emplace_back(int5{1, 1, 1, 1, 1});
            voxelTriModel.faceIds.emplace_back(0);
        }
    }

    // 计算模型的x,y平面坐标下中心点位置
    voxelTriModel.meshcenter = glm::vec3(shape.length / 2.0, shape.width / 2.0, 0 / 2.0);

    return voxelTriModel;
}

// 模块：把 PrimMesh 导出为 OBJ 文本，用于调试查看。
// 输入：path 输出文件路径；mesh 网格。输出：成功返回 true，文件无法打开返回 false。
// 说明：写出时交换 y 与 z（v x z y），即把本类的 z-up 坐标转成常见 OBJ 的 y-up；
//       面索引按 OBJ 从 1 开始；m_indicesBase 为局部变量，对单个网格恒为 0。
bool VoxelDesigner::outputPrimMesh(std::string path, PrimMesh mesh)
{
    std::ofstream outfile(path);
    if (outfile.is_open())
    {
        for(int kk=0;kk< mesh.vertices.size();kk++)
            outfile << "v "<<mesh.vertices[kk].pos.x <<  " "
                    <<mesh.vertices[kk].pos.z << " "
                    <<mesh.vertices[kk].pos.y << " "<<std::endl;

        uint32_t m_indicesBase = 0;
        for (std::size_t kk = 0; kk + 2 < mesh.indices.size(); kk = kk + 3)
            outfile << "f " << mesh.indices[kk]+1+ m_indicesBase << " "
                    << mesh.indices[kk+1]+1+ m_indicesBase << " "
                    << mesh.indices[kk+2]+1+ m_indicesBase << " " << std::endl;
        m_indicesBase += mesh.nVertices;
        outfile.close();
        return true;
    }
    return false;
}


// 模块：椭球的三角网格。顶点单位为体元。
// 输入：shape（length/width/height，米）、stepSize（米）；输出：PrimMesh（参数非法时为空网格）。
// 说明：体元中心 (i+0.5, j+0.5, k+0.5) 落在椭球内的体元被保留；每个体元输出的面片见下。
PrimMesh VoxelDesigner::createTriEllipsoid(Shape shape, float stepSize)
{
    PrimMesh voxelTriModel;

    voxelTriModel.nVertices = 0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;

    if (!(stepSize > 0.0f) || !(shape.width > 0.0f) ||
        !(shape.height > 0.0f) || !(shape.length > 0.0f)) {
        return voxelTriModel;
    }

    // Centre the physical ellipsoid in its discrete bounding box.  Using the
    // fractional physical size as the centre made any dimension smaller than
    // one voxel miss the only cell centre and produced an empty primitive.
    const int widthSize = std::max(1, static_cast<int>(std::ceil(shape.width / stepSize)));
    const int heightSize = std::max(1, static_cast<int>(std::ceil(shape.height / stepSize)));
    const int lengthSize = std::max(1, static_cast<int>(std::ceil(shape.length / stepSize)));

    const float widthCenter = widthSize / 2.0f;
    const float heightCenter = heightSize / 2.0f;
    const float lengthCenter = lengthSize / 2.0f;

    for (int i = 0; i < lengthSize; i++)
    {
        for (int j = 0; j < widthSize; j++)
        {
            for (int k = 0; k < heightSize; k++)
            {
                // 步骤 1：以体元中心 (i+0.5, j+0.5, k+0.5) 到椭球中心的归一化距离判定体元是否在椭球内。
                float dist = (i - lengthCenter + 0.5) * (i - lengthCenter + 0.5) / (lengthCenter * lengthCenter) +
                             (j - widthCenter + 0.5) * (j - widthCenter + 0.5) / (widthCenter * widthCenter) +
                             (k - heightCenter + 0.5) * (k - heightCenter + 0.5) / (heightCenter * heightCenter);
                // 步骤 2：体元在椭球内时，输出 8 个角点和 -z（bottom）、-x（left，x=i）、-y（forward，y=j）
                // 三个面；这三个面对所有保留体元都生成（含内部体元，见下方 DEFERRED），voxelIds 同时登记。
                //       正方向 +z/+y/+x 面见下面三步，只在相邻体元中心落在椭球外时才生成。
                // left, forward and bottom face
                // DEFERRED[低] 性能: 每个内部体元都生成 -x/-y/-z 三个面（含 8 个不共享顶点，其中部分未被引用），而 +x/+y/+z 只在边界生成，内部面数约为表面面数的 O(n) 倍，大椭球冠层三角形数量与 BLAS 大小显著膨胀；若内部面仅作为积分平面是有意设计，也应注明；建议：内部面复用共享顶点，或像 createTriVoxels 一样明确区分介质/实体模式。
                if (dist <= 1.0)
                {
                    glm::ivec3 voxel = glm::ivec3(i,j,k);
                    glm::vec3 point[8];

                    point[0].x = voxel.x ;
                    point[0].y = voxel.y ;
                    point[0].z = voxel.z ;

                    point[1].x = voxel.x ;
                    point[1].y = (voxel.y + 1) ;
                    point[1].z = voxel.z ;

                    point[2].x = (voxel.x + 1) ;
                    point[2].y = (voxel.y + 1) ;
                    point[2].z = voxel.z ;

                    point[3].x = (voxel.x + 1) ;
                    point[3].y = voxel.y ;
                    point[3].z = voxel.z ;

                    point[4].x = voxel.x ;
                    point[4].y = voxel.y ;
                    point[4].z = (voxel.z + 1) ;

                    point[5].x = voxel.x ;
                    point[5].y = (voxel.y + 1) ;
                    point[5].z = (voxel.z + 1) ;

                    point[6].x = voxel.x +1;
                    point[6].y = (voxel.y + 1) ;
                    point[6].z = (voxel.z + 1) ;

                    point[7].x = (voxel.x + 1) ;
                    point[7].y = voxel.y;
                    point[7].z = (voxel.z + 1) ;

                    // 顶点顺序：0-3 为底面 (z=k) 四角，4-7 为顶面 (z=k+1) 对应角点。
                    // All eight cell corners are shared by the initial faces.
                    for (int vi = 0; vi < 8; vi++)
                    {
                        VertexAttribute va;
                        va.pos = point[vi];
                        voxelTriModel.vertices.emplace_back(va);
                    }

                    // 0,1,2,3 bottom
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 1);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 2);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 2);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 3);

                    // 0,1,4,5 left
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 1);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 5);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 4);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 5);

                    // 0,3,4,7 forward
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 4);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 7);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 7);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 3);

                    voxelTriModel.nVertices += 8;
                    voxelTriModel.nIndices += 6 * 3;

                   voxelTriModel.voxelIds.emplace_back(glm::vec3{i, j, k});
                }

                // 步骤 3：+z 邻体元中心在椭球外时，补上顶面（z=k+1）。
                // up face
                float upDist = (i - lengthCenter + 0.5) * (i - lengthCenter + 0.5) / (lengthCenter * lengthCenter) +
                               (j - widthCenter + 0.5) * (j - widthCenter + 0.5) / (widthCenter * widthCenter) +
                               (k - heightCenter + 1.5) * (k - heightCenter + 1.5) / (heightCenter * heightCenter);
                if (dist <= 1.0 && upDist > 1.0)
                {
                    glm::ivec3 voxel = glm::ivec3(i, j, k);
                    glm::vec3 point[4];

                    point[0].x = voxel.x ;
                    point[0].y = voxel.y ;
                    point[0].z = (voxel.z + 1) ;

                    point[1].x = voxel.x ;
                    point[1].y = (voxel.y + 1) ;
                    point[1].z = (voxel.z + 1) ;

                    point[2].x = (voxel.x + 1) ;
                    point[2].y = (voxel.y + 1) ;
                    point[2].z = (voxel.z + 1) ;

                    point[3].x = (voxel.x + 1) ;
                    point[3].y = voxel.y ;
                    point[3].z = (voxel.z + 1) ;

                    for (int vi = 0; vi < 4; vi++)
                    {
                        VertexAttribute va;
                        va.pos = point[vi];
                        voxelTriModel.vertices.emplace_back(va);
                    }

                    // 0,1,2,3 up
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 1);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 2);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 2);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 3);

                    voxelTriModel.nVertices += 4;
                    voxelTriModel.nIndices += 2 * 3;
                }

                // 步骤 4：+y 邻体元中心在椭球外时，补上 y=j+1 的面。
                // backward face
                float backwardDist = (i - lengthCenter + 0.5) * (i - lengthCenter + 0.5) / (lengthCenter * lengthCenter) +
                                     (j - widthCenter + 1.5) * (j - widthCenter + 1.5) / (widthCenter * widthCenter) +
                                     (k - heightCenter + 0.5) * (k - heightCenter + 0.5) / (heightCenter * heightCenter);
                if (dist <= 1.0 && backwardDist > 1.0)
                {
                    glm::ivec3 voxel = glm::ivec3(i, j, k);
                    glm::vec3 point[4];

                    point[0].x = voxel.x ;
                    point[0].y = (voxel.y + 1) ;
                    point[0].z = voxel.z ;

                    point[1].x = voxel.x ;
                    point[1].y = (voxel.y + 1) ;
                    point[1].z = (voxel.z + 1) ;

                    point[2].x = (voxel.x + 1) ;
                    point[2].y = (voxel.y + 1) ;
                    point[2].z = (voxel.z + 1) ;

                    point[3].x = (voxel.x + 1) ;
                    point[3].y = (voxel.y + 1) ;
                    point[3].z = voxel.z ;

                    for (int vi = 0; vi < 4; vi++)
                    {
                        VertexAttribute va;
                        va.pos = point[vi];
                        voxelTriModel.vertices.emplace_back(va);
                    }

                    // 0,1,2,3 up
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 1);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 2);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 2);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 3);

                    voxelTriModel.nVertices += 4;
                    voxelTriModel.nIndices += 2 * 3;
                }

                // 步骤 5：+x 邻体元中心在椭球外时，补上 x=i+1 的面。
                // right face
                float rightDist = (i - lengthCenter + 1.5) * (i - lengthCenter + 1.5) / (lengthCenter * lengthCenter) +
                                  (j - widthCenter + 0.5) * (j - widthCenter + 0.5) / (widthCenter * widthCenter) +
                                  (k - heightCenter + 0.5) * (k - heightCenter + 0.5) / (heightCenter * heightCenter);
                if (dist <= 1.0 && rightDist > 1.0)
                {
                    glm::ivec3 voxel = glm::ivec3(i, j, k);
                    glm::vec3 point[4];

                    point[0].x = (voxel.x + 1) ;
                    point[0].y = voxel.y ;
                    point[0].z = voxel.z ;

                    point[1].x = (voxel.x + 1) ;
                    point[1].y = (voxel.y + 1) ;
                    point[1].z = voxel.z ;

                    point[2].x = (voxel.x + 1) ;
                    point[2].y = (voxel.y + 1) ;
                    point[2].z = (voxel.z + 1) ;

                    point[3].x = (voxel.x + 1) ;
                    point[3].y = voxel.y ;
                    point[3].z = (voxel.z + 1) ;

                    for (int vi = 0; vi < 4; vi++)
                    {
                        VertexAttribute va;
                        va.pos = point[vi];
                        voxelTriModel.vertices.emplace_back(va);
                    }

                    // 0,1,2,3 up
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 1);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 2);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 2);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + 3);

                    voxelTriModel.nVertices += 4;
                    voxelTriModel.nIndices += 2 * 3;
                }
            }
        }
    }


    voxelTriModel.meshcenter = glm::vec3(shape.length / 2.0, shape.width / 2.0, shape.height / 2.0);

    return voxelTriModel;
}

// 模块：判断整数点是否严格落在由前 4 个顶点围成的凸四边形内（ploys 少于 4 个点返回 false）。
// 方法：对四条边依次求叉积，四个符号全正或全负则在内部；点在边上（符号为 0）视为在外。
// 说明：叉积结果被赋给 int，浮点顶点坐标会被截断（待核对是否有意）。
bool VoxelDesigner::isInPloy(nvmath::vec2i testPoint, std::vector<nvmath::vec2f> ploys)
{
    if (ploys.size() < 4) return false;
    nvmath::vec2f A = ploys[0];
    nvmath::vec2f B = ploys[1];
    nvmath::vec2f C = ploys[2];
    nvmath::vec2f D = ploys[3];
    int y = testPoint[1];
    int x = testPoint[0];
    int a = (B.x - A.x) * (y - A.y) - (B.y - A.y) * (x - A.x);
    int b = (C.x - B.x) * (y - B.y) - (C.y - B.y) * (x - B.x);
    int c = (D.x - C.x) * (y - C.y) - (D.y - C.y) * (x - C.x);
    int d = (A.x - D.x) * (y - D.y) - (A.y - D.y) * (x - D.x);
    if ((a > 0 && b > 0 && c > 0 && d > 0) || (a < 0 && b < 0 && c < 0 && d < 0))
    {
        return true;
    }

    return false;
}



// 模块：背景平面体元化（z=0 的一层，j、k 分别对应 x、y 方向）。
// 输入：sceneLength、sceneWidth（米）、stepSize（米）；输出：体元整数坐标 (j, k, 0)。
// 说明：格数用 int 截断（不是 ceil），场景尺寸不是步长整数倍时最后不足一格的部分被舍去；
//       与 createCube 不同，这里不写入 m_voxelModel（对应赋值已被注释），所以 loadModel(PLANE) 不会更新
//       m_voxelModel。该路径目前没有调用点（loadModel 无人调用，场景背景用 createTriBackground 或
//       DEM 路径），因此不改取整方式，避免无依据地改变行为；若将来启用，应改为 ceil 并与背景网格对齐。
std::vector<glm::ivec3> VoxelDesigner::createBackground(float sceneLength, float sceneWidth, float stepSize)
{
    std::vector<glm::ivec3> discreteCoord;
    std::vector<Aabb> aabbs;
    if (!(stepSize > 0.0f) || !(sceneLength > 0.0f) || !(sceneWidth > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(sceneLength) || !std::isfinite(sceneWidth)) {
        return discreteCoord;
    }
    int lengthSize = sceneLength / stepSize;
    int widthSize = sceneWidth /stepSize;

    int a = 0;
    for (int j = 0; j < lengthSize; j++)
    {
        for (int k = 0; k < widthSize; k++)
        {
            glm::ivec3 coord = glm::ivec3(j, k, 0);
            Aabb aabb = {{glm::vec3(coord) },
                {glm::vec3(coord) + glm::vec3(1.0)}};
            discreteCoord.emplace_back(coord);
            aabbs.emplace_back(aabb);
        }
    }

//    m_voxelModel.centerPoints = discreteCoord;
//    m_voxelModel.aabbs = aabbs;
    return discreteCoord;
}

// 模块：背景平面三角网格（原有 doxygen 说明见下）。
// 说明：bgZ = -1，网格位于 z = bgZ+1 = 0 的平面；体元 id 记在 (i, j, -1)，即背景体元层在
//       z=-1；每个格子 6 个顶点；meshcenter、faceIds、isValids 均未设置（调用方如需使用，待核对）。
/// Create a triangle background
/// \param sceneLength
/// \param sceneWidth
/// \param stepSize
/// create mesh on upper face, voxelId is left-down point
/// \return a mesh with XYZ coordinate;
PrimMesh VoxelDesigner::createTriBackground(float sceneLength, float sceneWidth, float stepSize)
{
    PrimMesh mesh;
    mesh.meshId = 0;
    mesh.nVertices = 0;
    mesh.nIndices = 0;
    float bgZ = -1;
    if (!(stepSize > 0.0f) || !(sceneLength > 0.0f) || !(sceneWidth > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(sceneLength) || !std::isfinite(sceneWidth)) {
        return mesh;
    }
    // float bgZ = 0;


    for (int i = 0; i < sceneLength/stepSize; i++)
    {
        for (int j = 0; j < sceneWidth/stepSize; j++)
        {
            glm::vec3 points[4];
            points[0].x = i ;
            points[0].y = j ;
            points[0].z = bgZ +1;

            points[1].x = (i) ;
            points[1].y = (j + 1) ;
            points[1].z = bgZ +1;

            points[2].x = (i + 1) ;
            points[2].y = (j + 1) ;
            points[2].z = bgZ +1;

            points[3].x = (i + 1) ;
            points[3].y = (j) ;
            points[3].z = bgZ +1;

            int list[] = {0, 1, 2, 0, 2, 3};
            for (int k = 0;k < 6; k++)
            {
                int listK = list[k];
                VertexAttribute va{points[listK]};
                mesh.vertices.emplace_back(va);
                mesh.indices.emplace_back(mesh.nVertices + k);
            }

            mesh.nVertices += 6;
            mesh.nIndices += 6;
            mesh.voxelIds.emplace_back(glm::vec3{i , j , bgZ });
        }
    }

    return mesh;
}

// 模块：按 shape.shapetype 分派到 createTriCube / createTriEllipsoid 生成实体网格。
// 说明：只支持 CUBE 与 ELLIPSOID；其他形状类型（如 PLANE）不能生成实体网格，抛出异常\n//       （此前直接返回未初始化的 PrimMesh，meshId 等无默认值，后续使用属于未定义行为）。
PrimMesh VoxelDesigner::createTriEntity(Shape shape, float stepSize)
{
    PrimMesh voxelTriModel;
    ShapeType shapeType = shape.shapetype;

    if (shapeType == ShapeType::CUBE)
    {
        voxelTriModel = createTriCube(shape, stepSize);
    }
    else if (shapeType == ShapeType::ELLIPSOID)
    {
        voxelTriModel = createTriEllipsoid(shape, stepSize);
    }
    else
    {
        throw std::runtime_error("createTriEntity: unsupported shape type " + std::to_string(static_cast<int>(shapeType)) +
                                 " (only cube=0 and ellipsoid=1 can be meshed)");
    }

    return voxelTriModel;
}

//VoxelTriModel VoxelDesigner::createTriEntity_rotate(ShapeType shapeType, Shape shape, float stepSize, float rotateAngle)
//{
//    VoxelTriModel voxelTriModel;
//    if (shapeType == ShapeType::CUBE)
//    {
//        voxelTriModel = createTriCube_rotate(shape, stepSize, rotateAngle);
//    }
//    if (shapeType == ShapeType::ELLIPSOID)
//    {
//        voxelTriModel = createTriEllipsoid(shape, stepSize);
//    }
//    return voxelTriModel;
//}

//VoxelTriModel VoxelDesigner::createTriBackgroundFromDEM(const std::string &filename, glm::vec3 sceneSize_XYZ, _2D::BilinearInterpolator<double> &interp)
//{
//    VoxelTriModel voxelTriModel;
//    std::string infile = filename;
//    GDALAllRegister();
//
//    BYTE *pafScanblock;
//    GDALDataset *poDataset;
//    poDataset = (GDALDataset *)GDALOpen(infile.c_str(), GA_ReadOnly);
//    if (poDataset == NULL)
//    {
//        std::cout << "fail in open files!" << std::endl;
//        return voxelTriModel;
//    }
//    int nImgSizeX = poDataset->GetRasterXSize();
//    int nImgSizeY = poDataset->GetRasterYSize();
//    int bandcount = poDataset->GetRasterCount();
//
//    int num_image_size = 0;
//
//    FLOAT *pafScanline = new FLOAT[nImgSizeX * nImgSizeY];
//    poDataset->RasterIO(GF_Read, 0, 0, nImgSizeX, nImgSizeY, pafScanline, nImgSizeX, nImgSizeY, GDT_Float32, bandcount, 0, 0, 0, 0);
//
//    m_scale = sceneSize_XYZ.x / nImgSizeX;
//    float scale = m_scale;
//
//    std::vector<double> x, y, z;
//    for (int i = 0; i < nImgSizeX; i++)
//    {
//        for (int j = 0; j < nImgSizeY; j++)
//        {
//            x.emplace_back((i * scale + scale / 2.0)); // pixel center location
//            y.emplace_back((j * scale + scale / 2.0));
//            z.emplace_back(pafScanline[i * nImgSizeY + j]);
//        }
//    }
//
//
//    interp.setData(x, y, z);
//
//    delete[] pafScanline;
//    delete poDataset;
//
//    float step = 1; // / 2.0; // control DEM subdivision degree, The smaller the value, the finer the distinction
//
//    //找寻voxel中心点,后加上中心点包含的体
//    uint32_t nVertices = 0;
//    uint32_t nIndices = 0;
//    minElevation = z[0];
//    for(int j=0; j < sceneSize_XYZ.x; j=j+step)
//    {
//        for (int k=0; k<sceneSize_XYZ.y; k=k+step)
//        {
//            float jj = j, kk= k;
//            if (jj < scale / 2)
//                jj = scale / 2.0;
//            if (kk < scale / 2)
//                kk = scale / 2.0;
//            if (jj > sceneSize_XYZ.x - scale / 2.0)
//                jj = sceneSize_XYZ.x - scale / 2.0 - 0.01;
//            if (kk > sceneSize_XYZ.y - scale / 2.0)
//                kk = sceneSize_XYZ.y - scale / 2.0 - 0.01;
//            voxelTriModel.centerPoints.emplace_back(glm::vec3{j + 0.5, k + 0.5,
//                                                                  int(interp(jj, kk))});
//
//            
//            glm::ivec3 voxel = glm::ivec3(j, k, int(interp(jj, kk)));
//
//            if (voxel.z < minElevation)
//                minElevation = voxel.z;
//            glm::vec3 point[8];
//            int stepSize = 1;
//            point[0].x = voxel.x * stepSize;
//            point[0].y = voxel.y * stepSize;
//            point[0].z = voxel.z * stepSize;
//
//            point[1].x = voxel.x * stepSize;
//            point[1].y = (voxel.y + 1) * stepSize;
//            point[1].z = voxel.z * stepSize;
//
//            point[2].x = (voxel.x + 1) * stepSize;
//            point[2].y = (voxel.y + 1) * stepSize;
//            point[2].z = voxel.z * stepSize;
//
//            point[3].x = (voxel.x + 1) * stepSize;
//            point[3].y = voxel.y * stepSize;
//            point[3].z = voxel.z * stepSize;
//
//            point[4].x = voxel.x * stepSize;
//            point[4].y = voxel.y * stepSize;
//            point[4].z = (voxel.z + 1) * stepSize;
//
//            point[5].x = voxel.x * stepSize;
//            point[5].y = (voxel.y + 1) * stepSize;
//            point[5].z = (voxel.z + 1) * stepSize;
//
//            point[6].x = (voxel.x + 1) * stepSize;
//            point[6].y = voxel.y * stepSize;
//            point[6].z = (voxel.z + 1) * stepSize;
//
//            point[7].x = (voxel.x + 1) * stepSize;
//            point[7].y = (voxel.y + 1) * stepSize;
//            point[7].z = (voxel.z + 1) * stepSize;
//
//            int list[] = {0, 1, 2, 0, 2, 3,
//                          0, 1, 5, 0, 4, 5,
//                          1, 2, 6, 1, 5, 6,
//                          4, 5, 6, 4, 6, 7,
//                          2, 3, 6, 3, 6, 7,
//                          0, 3, 7, 0, 4, 7};
//
//
//            //顶点
//            for (int kkk = 0; kkk < 36; kkk++)
//            {
//                int listK = list[kkk];
//                VertexAttribute va{point[listK]};
//                voxelTriModel.vertices.emplace_back(va);
//
//                voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + kkk);
//
//
//                //VertexAttribute va{point[kkk]};
//                //voxelTriModel.vertices.emplace_back(va);
//            }
//            // 边
//            /*for (int kkk = 0; kkk < 36; kkk++)
//            {
//                int listK = list[kkk];
//
//                voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + listK);
//            }*/
//            voxelTriModel.nVertices += 36;
//            voxelTriModel.nIndices += 36;
//        }
//    }
//
//    return voxelTriModel;
//}

// 模块：由高度栅格（tif，单位米）生成表层体元与地表三角网格（用于地形/建筑高度图实体）。
// 输入：heightPath 高度图路径（GDAL 读第 1 波段，单位米）；stepSize 体元边长（米）。
// 输出：PrimMesh——vertices/indices 为表面面片（顶点单位为体元，z 向上）；
//       voxelIds 为表层体元左下角整数坐标，faceIds / isValids 与 voxelIds 一一对应。
// 说明：像元高度先换成体元层数 round(h/stepSize)。为节省内存只保留“表层”体元：
//       本像元不高于四邻时只留最顶一层，否则保留从最低邻居高度到本像元顶的竖向体元；
//       网格只加顶面和邻居更低一侧的侧面（不为体元建六面体）。
//       图像坐标：x 向右、y 向下，像元 (i, j) 对应数组下标 j*nImgSizeX+i；
//       循环只遍历内部像元（1..n-2），最外一圈不生成体元与面片。
PrimMesh VoxelDesigner::createTriEntitiesFromTif(std::string heightPath, float stepSize) {
    PrimMesh voxelTriModel;
    voxelTriModel.nVertices=0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;
    if (!(stepSize > 0.0f) || !std::isfinite(stepSize)) {
        return voxelTriModel;
    }
    //===================================================================
    /*! height map
     */
    // 步骤 1：用 GDAL 读高度图第 1 波段到 heightScanline（float）；打开/读取失败时返回空网格。
    CPLSetConfigOption("gdal_filename_is_utf8", "no");
    GDALAllRegister();

    GDALDataset* heightDataset;
    heightDataset = (GDALDataset*)GDALOpen(heightPath.c_str(), GA_ReadOnly);
    if (heightDataset == NULL)
    {
        std::cout << "fail in open height files!" << std::endl;
        return voxelTriModel;
    }
    int nImgSizeX = heightDataset->GetRasterXSize();
    int nImgSizeY = heightDataset->GetRasterYSize();
    int bandcount = heightDataset->GetRasterCount();
    if (nImgSizeX <= 0 || nImgSizeY <= 0 || bandcount < 1) {
        GDALClose(heightDataset);
        return voxelTriModel;
    }
    const std::size_t nPixelCount = static_cast<std::size_t>(nImgSizeX) *
                                    static_cast<std::size_t>(nImgSizeY);
    std::vector<FLOAT> heightScanline(nPixelCount, 0.0f);
    const CPLErr readErr = heightDataset->GetRasterBand(1)->RasterIO(
        GF_Read, 0, 0, nImgSizeX, nImgSizeY, heightScanline.data(),
        nImgSizeX, nImgSizeY, GDT_Float32, 0, 0);
    if (readErr != CE_None) {
        GDALClose(heightDataset);
        return voxelTriModel;
    }


    //===================================================================
    /*! create every model through its height: get the center points
     * (this center points is actually the minimum(left and down) point)
     * for edge pixel: create the vertical voxels from the lowest height around to its height.
     *              if the height of this pixel is the lowest, then establish its corresponding voxel height.
     * for non-edge pixel: establish its corresponding voxel height.
     * ENVI tif coordinate system:
     *      .--------> x
     *      |---------
     *      |----.(i, j)  (j * nImgX + i)
     *      |
     *      y
     *
     * spatial relationship:
     *      .------------------------------------------------> x
     *      |----------------- .up (back)----------------------
     *      |--- .left (left)| .cur (center) | .right (right)----
     *      |----------------- .down (forward) ----------------
     *      |-------------------------------------------------
     *      y
     */

    // 步骤 2：minReHeightScanline 保存每个像元与四邻的最小层数；本函数内只写入、未被读取，
    // 函数末尾释放（待核对其用途）。
    FLOAT* minReHeightScanline = new FLOAT[static_cast<std::size_t>(nImgSizeX) * static_cast<std::size_t>(nImgSizeY)];

    float minReHeight, rightReHeight, leftReHeight, upReHeight, downReHeight;
    // 步骤 3：逐内部像元（跳过边缘一圈）处理：取本像元与上下左右邻居的层数，生成体元与面片。
    for (int i = 1; i < nImgSizeX - 1; i++)
    {
        for (int j = 1; j < nImgSizeY - 1; j++)
        {
            // all the height
            float curReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);


            leftReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i - 1)] / stepSize);
            rightReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i + 1)] / stepSize);
            upReHeight = std::round(heightScanline[static_cast<std::size_t>(j - 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            downReHeight = std::round(heightScanline[static_cast<std::size_t>(j + 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            // resized height
            minReHeight = std::min({ curReHeight, leftReHeight, rightReHeight, upReHeight, downReHeight });

            // faceId：记录最低点所在方向，后面的 if 覆盖前面的，优先级为 up(本像元) > right > left >
            // backward(图像上方) > forward(图像下方)；取值 1 forward、2 right、3 left、4 backward、5 up。
            int faceId = 0;
            if (downReHeight == minReHeight) faceId = 1;    // forward
            if (upReHeight == minReHeight) faceId = 4;      // backward
            if (leftReHeight == minReHeight) faceId = 3;    // left
            if (rightReHeight == minReHeight) faceId = 2;   // right
            if (curReHeight == minReHeight) faceId = 5;     // up

            minReHeightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] = minReHeight;
            //===================================================================
            /*! save [voxelIds: left-down point] and [faceIds] values: NanoVDB attribution location
             * {}
             */
            // 步骤 4：登记体元：本像元为局部最低时只留顶层体元 (i, j, cur-1)（高度为 0 的像元直接跳过）；
            // 否则保留 minReHeight..cur-1 的竖向体元，并按各侧邻居高度设置 isValids（前/后/左/右/上）。
            // 说明：isValids 的侧面判据用 “体元层号 > 邻居层数”，层号等于邻居层数的体元不算（待核对）。
            if (curReHeight == minReHeight)
            {
                if (curReHeight == 0) continue;

                voxelTriModel.voxelIds.emplace_back(i, j, curReHeight - 1);
                voxelTriModel.faceIds.emplace_back(faceId);

                // isValids: 0 forward, 1 backward, 2 left, 3 right, 4 up
                int5 curIsValids = { 0, 0, 0, 0, 1 };
                voxelTriModel.isValids.emplace_back(curIsValids);
            }
            else
            {
                for (int heighti = 0; heighti < (curReHeight - minReHeight); heighti++)
                {
                    voxelTriModel.voxelIds.emplace_back(i, j, minReHeight + heighti);
                    voxelTriModel.faceIds.emplace_back(faceId);

                    // isValids: 0 forward, 1 backward, 2 left, 3 right, 4 up
                    int5 curIsValids = { 0, 0, 0, 0, 0 };
                    if (minReHeight + heighti > downReHeight) curIsValids.values[0] = 1;
                    if (minReHeight + heighti > upReHeight) curIsValids.values[1] = 1;
                    if (minReHeight + heighti > leftReHeight) curIsValids.values[2] = 1;
                    if (minReHeight + heighti > rightReHeight) curIsValids.values[3] = 1;
                    voxelTriModel.isValids.emplace_back(curIsValids);
                }
            }

            //===================================================================
            /*! save [vertices] and [indices] values: mesh
             * because add voxel wastes the memory, so add each face
             */
            // because add voxel wastes the memory, so add each face
            if (curReHeight == minReHeight)    // add up face
            {
                if (curReHeight == 0) continue;
            }
            // 步骤 5：本像元顶面（z = curReHeight），2 个三角形；顶点不共享，按 0,1,2,0,2,3 排序。
            // current pixel face
            glm::vec3 points[4];
            points[0] = { i, j, curReHeight };  // left and down point
            points[1] = { i + 1, j, curReHeight };
            points[2] = { i + 1, j + 1, curReHeight };
            points[3] = { i, j + 1, curReHeight };
            int list[] = { 0,1,2,0,2,3 };
            for (int k = 0; k < 6; k++)
            {
                int listK = list[k];
                VertexAttribute va{ points[listK] };
                voxelTriModel.vertices.emplace_back(va);
                voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
            }
            voxelTriModel.nVertices += 6;
            voxelTriModel.nIndices += 6;

            // 步骤 6：四个侧面（left / right / up / down），仅当该侧邻居层数低于本像元时生成，
            // 面片范围从邻居高度到本像元高度；up 侧的索引顺序为 0,2,1,0,3,2（法向朝外，待核对）。
            // left pixel
            if (leftReHeight < curReHeight)
            {
                glm::vec3 points[4];
                points[0] = { i, j + 1, leftReHeight };  // left and down point
                points[1] = { i, j, leftReHeight };
                points[2] = { i, j, curReHeight };
                points[3] = { i, j + 1, curReHeight };

                int list[] = { 0,1,2,0,2,3 };
                for (int k = 0; k < 6; k++)
                {
                    int listK = list[k];
                    VertexAttribute va{ points[listK] };
                    voxelTriModel.vertices.emplace_back(va);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }

            // right pixel
            if (rightReHeight < curReHeight)
            {
                glm::vec3 points[4];
                points[0] = { i + 1, j, rightReHeight };  // left and down point
                points[1] = { i + 1, j + 1, rightReHeight };
                points[2] = { i + 1, j + 1, curReHeight };
                points[3] = { i + 1, j, curReHeight };

                int list[] = { 0,1,2,0,2,3 };
                for (int k = 0; k < 6; k++)
                {
                    int listK = list[k];
                    VertexAttribute va{ points[listK] };
                    voxelTriModel.vertices.emplace_back(va);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }

            // up pixel
            if (upReHeight < curReHeight)
            {
                glm::vec3 points[4];
                points[0] = { i + 1, j, upReHeight };  // left and down point
                points[1] = { i, j, upReHeight };
                points[2] = { i, j, curReHeight };
                points[3] = { i + 1, j, curReHeight };

                int list[] = { 0,2,1,0,3,2 };
                for (int k = 0; k < 6; k++)
                {
                    int listK = list[k];
                    VertexAttribute va{ points[listK] };
                    voxelTriModel.vertices.emplace_back(va);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }

            // down face
            if (downReHeight < curReHeight)
            {
                glm::vec3 points[4];
                points[0] = { i + 1, j + 1, downReHeight };  // left and down point
                points[1] = { i, j + 1, downReHeight };
                points[2] = { i, j + 1, curReHeight };
                points[3] = { i + 1, j + 1, curReHeight };

                int list[] = { 0,1,2,0,2,3 };
                for (int k = 0; k < 6; k++)
                {
                    int listK = list[k];
                    VertexAttribute va{ points[listK] };
                    voxelTriModel.vertices.emplace_back(va);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }
        }
    }

    //delete heightDataset;
    GDALClose(heightDataset);
    delete[] minReHeightScanline;

    return voxelTriModel;
}


// 模块：屋顶——由高度栅格生成每个像元最顶层体元及其顶面三角形（建筑的屋顶部分）。
// 输入：heightPath 高度图路径（米）；targetSize 目标网格尺寸，其 x 和 z 被当作重采样后的
//       图像宽、高（体元数；y 未使用，待核对）；stepSize 体元边长（米）。
// 输出：PrimMesh——只含顶面网格，voxelIds 为 (i, j, cur-1) 顶层体元，faceIds / isValids 同序。
// 说明：与 createTriEntitiesFromTif 的区别是先用 cv::resize 重采样到 targetSize，
//       且只登记最顶层体元；墙体由 createTriEntitiesFromTif_wall 配套生成。
//       同样只遍历内部像元（1..n-2）。
PrimMesh VoxelDesigner::createTriEntitiesFromTif_roof(std::string heightPath, glm::vec3 targetSize, float stepSize) {
    // DEFERRED[中] 错误处理: targetSize.x/z 被 float→int 截断，<3 时下方循环为空静默返回空网格，且非正时（或 readImageinout1 读取失败后）cv::resize 会抛异常（_roof/_wall 的 readImageinout1 返回值检查已在本轮补上，见下方 FIXED）；resize 默认 INTER_LINEAR 会在建筑边缘插出中间高度，产生虚假台阶，属结果变化本轮不改；建议：targetSize 取整校验，按 round 取格数，高度图重采样用 INTER_NEAREST（_wall 同理）。
    PrimMesh voxelTriModel;
    voxelTriModel.nVertices=0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;
    //===================================================================
    /*! height map
     */
//    CPLSetConfigOption("gdal_filename_is_utf8", "no");
//    GDALAllRegister();
//
//    GDALDataset* heightDataset;
//    heightDataset = (GDALDataset*)GDALOpen(heightPath.c_str(), GA_ReadOnly);
//    if (heightDataset == NULL)
//    {
//        std::cout << "fail in open height files!" << std::endl;
//        return voxelTriModel;
//    }
//    int nImgSizeX = heightDataset->GetRasterXSize();
//    int nImgSizeY = heightDataset->GetRasterYSize();
//    int bandcount = heightDataset->GetRasterCount();
//    FLOAT* heightScanline = new FLOAT[nImgSizeX * nImgSizeY];
//    heightDataset->RasterIO(GF_Read, 0, 0, nImgSizeX, nImgSizeY, heightScanline, nImgSizeX, nImgSizeY, GDT_Float32, bandcount, 0, 0, 0, 0);

    // 步骤 1：用 Utils::readImageinout1 读高度图；失败或尺寸不符时返回空网格。
    std::vector<float> heightScanline0;
    int width,height, nband;
    if (!Utils::readImageinout1(heightPath,heightScanline0,width,height,nband) ||
        width <= 0 || height <= 0 ||
        heightScanline0.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        std::cout << "fail in reading height file: " << heightPath << std::endl;
        return voxelTriModel;
    }
    cv::Mat heightScanline0_cv = Utils::convertVector2Mat<float>(heightScanline0,1,height);
    cv::Mat heightScanline_cv;

    // 步骤 2：把高度图 cv::resize 到 targetSize.x × targetSize.z（线性插值，边缘可能插出中间高度，
    // 见上方 DEFERRED），再转回 vector<float> 供后续按行主序读取。
    int nImgSizeX = targetSize.x;
    int nImgSizeY = targetSize.z;
    cv::resize(heightScanline0_cv,heightScanline_cv,cv::Size(nImgSizeX,nImgSizeY));
    std::vector<float> heightScanline;
    heightScanline = Utils::convertMat2Vector<float>(heightScanline_cv);
    heightScanline0.clear();
    heightScanline0_cv.release();
    heightScanline_cv.release();

    //===================================================================
    /*! create every model through its height: get the center points
     * (this center points is actually the minimum(left and down) point)
     * for edge pixel: create the vertical voxels from the lowest height around to its height.
     *              if the height of this pixel is the lowest, then establish its corresponding voxel height.
     * for non-edge pixel: establish its corresponding voxel height.
     * ENVI tif coordinate system:
     *      .--------> x
     *      |---------
     *      |----.(i, j)  (j * nImgX + i)
     *      |
     *      y
     *
     * spatial relationship:
     *      .------------------------------------------------> x
     *      |----------------- .up (back)----------------------
     *      |--- .left (left)| .cur (center) | .right (right)----
     *      |----------------- .down (forward) ----------------
     *      |-------------------------------------------------
     *      y
     */

    // 步骤 3：minReHeightScanline 只写不读（待核对）；下面逐内部像元处理。
    FLOAT* minReHeightScanline = new FLOAT[static_cast<std::size_t>(nImgSizeX) * static_cast<std::size_t>(nImgSizeY)];

    float minReHeight, rightReHeight, leftReHeight, upReHeight, downReHeight;
    for (int i = 1; i < nImgSizeX - 1; i++)
    {
        for (int j = 1; j < nImgSizeY - 1; j++)
        {
            // all the height
            float curReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);


            leftReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i - 1)] / stepSize);
            rightReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i + 1)] / stepSize);
            upReHeight = std::round(heightScanline[static_cast<std::size_t>(j - 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            downReHeight = std::round(heightScanline[static_cast<std::size_t>(j + 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            // resized height
            minReHeight = std::min({ curReHeight, leftReHeight, rightReHeight, upReHeight, downReHeight });

            // faceId 取值同 createTriEntitiesFromTif：1 forward、2 right、3 left、4 backward、5 up。
            int faceId = 0;
            if (downReHeight == minReHeight) faceId = 1;    // forward
            if (upReHeight == minReHeight) faceId = 4;      // backward
            if (leftReHeight == minReHeight) faceId = 3;    // left
            if (rightReHeight == minReHeight) faceId = 2;   // right
            if (curReHeight == minReHeight) faceId = 5;     // up

            minReHeightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] = minReHeight;
            //===================================================================
            /*! save [voxelIds: left-down point] and [faceIds] values: NanoVDB attribution location
             * {}
             */
            // 步骤 4：登记体元：局部最低像元登记顶层体元；其余像元也只登记最顶一层 (i, j, cur-1)，
            // 侧面标志按邻居高度设置（见下方 DEFERRED）。
            if (curReHeight == minReHeight)
            {
                if (curReHeight == 0) continue;

                voxelTriModel.voxelIds.emplace_back(i, j, curReHeight - 1);
                voxelTriModel.faceIds.emplace_back(faceId);

                // isValids: 0 forward, 1 backward, 2 left, 3 right, 4 up
                int5 curIsValids = { 0, 0, 0, 0, 1 };
                voxelTriModel.isValids.emplace_back(curIsValids);
            }
            else
            {
                // only the roof voxel is added;
                // DEFERRED[低] Bug: 屋顶体元 isValids 的 4 个侧面判断用 heightt > neighbour（体元底高与邻居顶高比较），与 createTriEntitiesFromTif 中 minReHeight+heighti 的语义一致但此处只添加了顶面三角形、未添加侧墙三角形，侧面 isValids=1 却没有对应几何面；且与 _wall 同时使用时最顶层体元被两次加入（墙的循环也包含 curReHeight-1 层）；建议：明确 roof/wall 的层划分（wall 只到 curReHeight-2），侧面标志与实际几何一致。
                for (int heightt = curReHeight - 1; heightt < curReHeight; heightt++)
                {
                    voxelTriModel.voxelIds.emplace_back(i, j, heightt);
                    voxelTriModel.faceIds.emplace_back(faceId);

                    // isValids: 0 forward, 1 backward, 2 left, 3 right, 4 up
                    int5 curIsValids = { 0, 0, 0, 0, 1 };
                    if (heightt > downReHeight) curIsValids.values[0] = 1;
                    if (heightt > upReHeight) curIsValids.values[1] = 1;
                    if (heightt > leftReHeight) curIsValids.values[2] = 1;
                    if (heightt > rightReHeight) curIsValids.values[3] = 1;
                    voxelTriModel.isValids.emplace_back(curIsValids);
                }
            }

            //===================================================================
            /*! save [vertices] and [indices] values: mesh
             * because add voxel wastes the memory, so add each face
             */
            // because add voxel wastes the memory, so add each face
            if (curReHeight == minReHeight)    // add up face
            {
                if (curReHeight == 0) continue;
            }
            // 步骤 5：只生成本像元顶面（z = curReHeight），不生成侧面。
            // current pixel face
            glm::vec3 points[4];
            points[0] = { i, j, curReHeight };  // left and down point
            points[1] = { i + 1, j, curReHeight };
            points[2] = { i + 1, j + 1, curReHeight };
            points[3] = { i, j + 1, curReHeight };
            int list[] = { 0,1,2,0,2,3 };
            for (int k = 0; k < 6; k++)
            {
                int listK = list[k];
                VertexAttribute va{ points[listK] };
                voxelTriModel.vertices.emplace_back(va);
                voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
            }
            voxelTriModel.nVertices += 6;
            voxelTriModel.nIndices += 6;


        }
    }

   // delete[] heightScanline;
    //delete heightDataset;
   // GDALClose(heightDataset);
    heightScanline.clear();
    delete[] minReHeightScanline;

    return voxelTriModel;
}

// 模块：墙体——由高度栅格生成竖向体元及侧面三角形（建筑的墙面部分，不含顶面）。
// 输入/输出与重采样方式同 createTriEntitiesFromTif_roof；voxelIds 为 minReHeight..cur-1 的竖向体元，
//       网格只含邻居更低一侧的侧面。
// 说明：顶层体元在 roof 与 wall 中都会登记（见 roof 中的 DEFERRED）。
PrimMesh VoxelDesigner::createTriEntitiesFromTif_wall(std::string heightPath, glm::vec3 targetSize, float stepSize) {
    PrimMesh voxelTriModel;
    voxelTriModel.nVertices=0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;
    //===================================================================
    /*! height map
     */
//    CPLSetConfigOption("gdal_filename_is_utf8", "no");
//    GDALAllRegister();
//
//    GDALDataset* heightDataset;
//    heightDataset = (GDALDataset*)GDALOpen(heightPath.c_str(), GA_ReadOnly);
//    if (heightDataset == NULL)
//    {
//        std::cout << "fail in open height files!" << std::endl;
//        return voxelTriModel;
//    }
//    int nImgSizeX = heightDataset->GetRasterXSize();
//    int nImgSizeY = heightDataset->GetRasterYSize();
//    int bandcount = heightDataset->GetRasterCount();
//    FLOAT* heightScanline = new FLOAT[nImgSizeX * nImgSizeY];
//    heightDataset->RasterIO(GF_Read, 0, 0, nImgSizeX, nImgSizeY, heightScanline, nImgSizeX, nImgSizeY, GDT_Float32, bandcount, 0, 0, 0, 0);


     // 步骤 1：读高度图并 cv::resize 到 targetSize.x × targetSize.z（同 roof）。
     std::vector<float> heightScanline0;
     int width,height, nband;
     if (!Utils::readImageinout1(heightPath,heightScanline0,width,height,nband) ||
         width <= 0 || height <= 0 ||
         heightScanline0.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
         std::cout << "fail in reading height file: " << heightPath << std::endl;
         return voxelTriModel;
     }
     cv::Mat heightScanline0_cv = Utils::convertVector2Mat<float>(heightScanline0,1,height);
    cv::Mat heightScanline_cv;

    int nImgSizeX = targetSize.x;
    int nImgSizeY = targetSize.z;
    cv::resize(heightScanline0_cv,heightScanline_cv,cv::Size(nImgSizeX,nImgSizeY));
    std::vector<float> heightScanline;
    heightScanline = Utils::convertMat2Vector<float>(heightScanline_cv);
    heightScanline0.clear();
    heightScanline0_cv.release();
    heightScanline_cv.release();



    //===================================================================
    /*! create every model through its height: get the center points
     * (this center points is actually the minimum(left and down) point)
     * for edge pixel: create the vertical voxels from the lowest height around to its height.
     *              if the height of this pixel is the lowest, then establish its corresponding voxel height.
     * for non-edge pixel: establish its corresponding voxel height.
     * ENVI tif coordinate system:
     *      .--------> x
     *      |---------
     *      |----.(i, j)  (j * nImgX + i)
     *      |
     *      y
     *
     * spatial relationship:
     *      .------------------------------------------------> x
     *      |----------------- .up (back)----------------------
     *      |--- .left (left)| .cur (center) | .right (right)----
     *      |----------------- .down (forward) ----------------
     *      |-------------------------------------------------
     *      y
     */

    // 步骤 2：minReHeightScanline 只写不读（待核对）；下面逐内部像元处理。
    FLOAT* minReHeightScanline = new FLOAT[static_cast<std::size_t>(nImgSizeX) * static_cast<std::size_t>(nImgSizeY)];

    float minReHeight, rightReHeight, leftReHeight, upReHeight, downReHeight;
    for (int i = 1; i < nImgSizeX - 1; i++)
    {
        for (int j = 1; j < nImgSizeY - 1; j++)
        {
            // all the height
            float curReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);

            leftReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i - 1)] / stepSize);
            rightReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i + 1)] / stepSize);
            upReHeight = std::round(heightScanline[static_cast<std::size_t>(j - 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            downReHeight = std::round(heightScanline[static_cast<std::size_t>(j + 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            // resized height
            minReHeight = std::min({ curReHeight, leftReHeight, rightReHeight, upReHeight, downReHeight });

            // faceId 取值同 createTriEntitiesFromTif：1 forward、2 right、3 left、4 backward、5 up。
            int faceId = 0;
            if (downReHeight == minReHeight) faceId = 1;    // forward
            if (upReHeight == minReHeight) faceId = 4;      // backward
            if (leftReHeight == minReHeight) faceId = 3;    // left
            if (rightReHeight == minReHeight) faceId = 2;   // right
            if (curReHeight == minReHeight) faceId = 5;     // up

            minReHeightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] = minReHeight;
            //===================================================================
            /*! save [voxelIds: left-down point] and [faceIds] values: NanoVDB attribution location
             * {}
             */
            // 步骤 3：登记体元：局部最低像元登记顶层体元，其余登记 minReHeight..cur-1 的竖向体元。
            if (curReHeight == minReHeight)
            {
                if (curReHeight == 0) continue;

                voxelTriModel.voxelIds.emplace_back(i, j, curReHeight - 1);
                voxelTriModel.faceIds.emplace_back(faceId);

                // isValids: center 0, 0 forward, 1 backward, 2 left, 3 right, 4 up
                int5 curIsValids = { 0, 0, 0, 0, 1 };
                voxelTriModel.isValids.emplace_back(curIsValids);
            }
            else
            {
                for (int heighti = 0; heighti < (curReHeight - minReHeight); heighti++)
                {
                    voxelTriModel.voxelIds.emplace_back(i, j, minReHeight + heighti);
                    voxelTriModel.faceIds.emplace_back(faceId);

                    // isValids: 0 forward, 1 backward, 2 left, 3 right, 4 up
                    int5 curIsValids = { 0, 0, 0, 0, 1 };
                    if (minReHeight + heighti > downReHeight) curIsValids.values[0] = 1;
                    if (minReHeight + heighti > upReHeight) curIsValids.values[1] = 1;
                    if (minReHeight + heighti > leftReHeight) curIsValids.values[2] = 1;
                    if (minReHeight + heighti > rightReHeight) curIsValids.values[3] = 1;
                    voxelTriModel.isValids.emplace_back(curIsValids);
                }
            }

            //===================================================================
            /*! save [vertices] and [indices] values: mesh
             * because add voxel wastes the memory, so add each face
             */
            // because add voxel wastes the memory, so add each face
            if (curReHeight == minReHeight)    // add up face
            {
                if (curReHeight == 0) continue;
            }


            // 步骤 4：四个侧面（left / right / up / down），仅当邻居层数更低时生成；不生成顶面。
            // left pixel
            if (leftReHeight < curReHeight)
            {
                glm::vec3 points[4];
                points[0] = { i, j + 1, leftReHeight };  // left and down point
                points[1] = { i, j, leftReHeight };
                points[2] = { i, j, curReHeight };
                points[3] = { i, j + 1, curReHeight };

                int list[] = { 0,1,2,0,2,3 };
                for (int k = 0; k < 6; k++)
                {
                    int listK = list[k];
                    VertexAttribute va{ points[listK] };
                    voxelTriModel.vertices.emplace_back(va);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }

            // right pixel
            if (rightReHeight < curReHeight)
            {
                glm::vec3 points[4];
                points[0] = { i + 1, j, rightReHeight };  // left and down point
                points[1] = { i + 1, j + 1, rightReHeight };
                points[2] = { i + 1, j + 1, curReHeight };
                points[3] = { i + 1, j, curReHeight };

                int list[] = { 0,1,2,0,2,3 };
                for (int k = 0; k < 6; k++)
                {
                    int listK = list[k];
                    VertexAttribute va{ points[listK] };
                    voxelTriModel.vertices.emplace_back(va);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }

            // up pixel
            if (upReHeight < curReHeight)
            {
                glm::vec3 points[4];
                points[0] = { i + 1, j, upReHeight };  // left and down point
                points[1] = { i, j, upReHeight };
                points[2] = { i, j, curReHeight };
                points[3] = { i + 1, j, curReHeight };

                int list[] = { 0,2,1,0,3,2 };
                for (int k = 0; k < 6; k++)
                {
                    int listK = list[k];
                    VertexAttribute va{ points[listK] };
                    voxelTriModel.vertices.emplace_back(va);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }

            // down face
            if (downReHeight < curReHeight)
            {
                glm::vec3 points[4];
                points[0] = { i + 1, j + 1, downReHeight };  // left and down point
                points[1] = { i, j + 1, downReHeight };
                points[2] = { i, j + 1, curReHeight };
                points[3] = { i + 1, j + 1, curReHeight };

                int list[] = { 0,1,2,0,2,3 };
                for (int k = 0; k < 6; k++)
                {
                    int listK = list[k];
                    VertexAttribute va{ points[listK] };
                    voxelTriModel.vertices.emplace_back(va);
                    voxelTriModel.indices.emplace_back(voxelTriModel.nVertices + k);
                }

                voxelTriModel.nVertices += 6;
                voxelTriModel.nIndices += 6;
            }
        }
    }

    //delete[] heightScanline;
    //delete heightDataset;
   // GDALClose(heightDataset);
    heightScanline.clear();
    delete[] minReHeightScanline;

    return voxelTriModel;
}
