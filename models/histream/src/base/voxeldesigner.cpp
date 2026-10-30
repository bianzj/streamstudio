#include "voxeldesigner.h"
#include "voxel_boundary.h"
#include <nvh/gltfscene.cpp>
//#include "autogen/VoxelDesigner.comp.h"
// using BufferT = nanovdb::HostBuffer;

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

// 均为xyz坐标
std::vector<glm::ivec3> VoxelDesigner::createEllipsoid(float height, float width, float stepSize) // Shape.length...
{
    std::vector<glm::ivec3> discreteCoord;
    std::vector<Aabb> aabbs;

    // FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 ceil(inf) 转 int 属未定义行为（本文件所有 createXxx 同理）；下方椭球判定用体元角点 i 而非中心 i+0.5 与 size/2 比较（注释中的推导不成立），整个椭球向负方向偏移半格、形状不对称；已修复：入口校验 stepSize/height/width 有限且 >0，非法时清空 m_voxelModel 并返回空结果；角点判定未改（DEFERRED，改变形状）。
    if (!(stepSize > 0.0f) || !(height > 0.0f) || !(width > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(height) || !std::isfinite(width)) {
        m_voxelModel.centerPoints.clear();
        m_voxelModel.aabbs.clear();
        return discreteCoord;
    }
    int widthSize = std::ceil(width / stepSize);
    int heightSize = std::ceil(height / stepSize);
    int lengthSize = std::ceil(width / stepSize);

    float widthCenter = widthSize / 2.0;
    float heightCenter = heightSize / 2.0;
    float lengthCenter = lengthSize / 2.0;

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


std::vector<glm::ivec3> VoxelDesigner::createCube(float length, float width, float height, float stepSize)
{
    std::vector<glm::ivec3> discreteCoord;
    std::vector<Aabb> aabbs;

    // FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 ceil(inf) 转 int 属未定义行为（与 createEllipsoid 同源）；已修复：入口校验 stepSize/length/width/height 有限且 >0，非法时清空 m_voxelModel 并返回空结果。
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

PrimMesh VoxelDesigner::createTriVoxels(const std::vector<glm::ivec3>& voxelIds, bool solidBoundary)
{
    PrimMesh mesh{};
    mesh.meshId = 0;

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

PrimMesh VoxelDesigner::createTriCube(Shape shape, float stepSize)
{
    PrimMesh voxelTriModel;
    voxelTriModel.nVertices = 0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;

    // FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 height/stepSize 为 inf，循环上界转 int 属未定义行为（createTriCube_wall/_roof 同源）；已修复：入口校验 stepSize 与 shape 三维有限且 >0，非法时返回空 mesh。
    if (!(stepSize > 0.0f) || !(shape.width > 0.0f) || !(shape.height > 0.0f) || !(shape.length > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(shape.width) || !std::isfinite(shape.height) || !std::isfinite(shape.length)) {
        return voxelTriModel;
    }

    // DEFERRED[中] Bug: 层数循环 k < height/stepSize 等价于 ceil（非整数倍时多出一层），而顶面/最大面位置用 round(height/stepSize)；例如 height=10、step=3 时体元到 z=4，顶面却在 z=3，盒子不封闭且顶面穿过体元内部；同时 voxelIds 只在 x-z 平面循环中追加；meshcenter 以米为单位而顶点以体元为单位，stepSize≠1 时中心错误；建议：统一用 n=ceil(size/step) 作为层数与外表面位置，meshcenter 用 n/2。
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

PrimMesh VoxelDesigner::createTriCube_wall(Shape shape, float stepSize)
{
    PrimMesh voxelTriModel;
    voxelTriModel.nVertices = 0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;

    // FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 height/stepSize-1 为 inf，循环上界转 int 属未定义行为（与 createTriCube 同源）；已修复：入口校验 stepSize 与 shape 三维有限且 >0，非法时返回空 mesh。
    if (!(stepSize > 0.0f) || !(shape.width > 0.0f) || !(shape.height > 0.0f) || !(shape.length > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(shape.width) || !std::isfinite(shape.height) || !std::isfinite(shape.length)) {
        return voxelTriModel;
    }

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

PrimMesh VoxelDesigner::createTriCube_roof(Shape shape, float stepSize)
{
    PrimMesh voxelTriModel;
    voxelTriModel.nVertices = 0;
    voxelTriModel.nIndices = 0;
    voxelTriModel.meshId = 0;

    // FIXED[中] 数值: 未校验 stepSize>0 与尺寸有限，stepSize=0 时 height/stepSize-1 为 inf，转 int 属未定义行为（与 createTriCube 同源）；已修复：入口校验 stepSize 与 shape 三维有限且 >0，非法时返回空 mesh。
    if (!(stepSize > 0.0f) || !(shape.width > 0.0f) || !(shape.height > 0.0f) || !(shape.length > 0.0f) ||
        !std::isfinite(stepSize) || !std::isfinite(shape.width) || !std::isfinite(shape.height) || !std::isfinite(shape.length)) {
        return voxelTriModel;
    }

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

bool VoxelDesigner::outputPrimMesh(std::string path, PrimMesh mesh)
{
    // FIXED[低] 物理: 输出时交换 y/z（Z-up→Y-up）是镜像变换，三角形绕序被反转，导出 OBJ 中法向朝内；窄字符路径打开中文目录失败；PrimMesh 按值传参整体复制；indices.size() 不是 3 的倍数时 kk+1/kk+2 越界；建议：交换坐标时同时交换每个面的两个索引，const 引用传参，循环条件用 kk+2<size；已修复：循环条件改为 kk+2<size 并使用 std::size_t 步进，尾部不足 3 个的索引不再被读出；其余三项未改（DEFERRED）。
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
                float dist = (i - lengthCenter + 0.5) * (i - lengthCenter + 0.5) / (lengthCenter * lengthCenter) +
                             (j - widthCenter + 0.5) * (j - widthCenter + 0.5) / (widthCenter * widthCenter) +
                             (k - heightCenter + 0.5) * (k - heightCenter + 0.5) / (heightCenter * heightCenter);
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

bool VoxelDesigner::isInPloy(nvmath::vec2i testPoint, std::vector<nvmath::vec2f> ploys)
{
    // FIXED[中] 数值: 浮点叉积被隐式截断为 int，|值|<1 的结果变为 0，导致靠近边的点被判为"不在多边形内"（严格 >0/<0），且大坐标时 float→int 溢出为 UB；ploys 未检查至少 4 个点即越界读；建议：叉积用 float/double 保存，必要时用 >=0 包含边界，并校验 ploys.size()>=4；已修复：入口校验 ploys.size()>=4，不足 4 点直接返回 false，不再越界读；叉积仍为 int（DEFERRED，边界判定语义需确认）。
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
    // FIXED[低] 数值: 循环上界 i < sceneLength/stepSize 等价于 ceil，而 createBackground 用 int 截断（floor），scene.cpp 中背景格数也用截断；非整数倍时两者相差一列，背景网格与体元 ID 错位；stepSize 未校验 >0；建议：统一使用一个格数计算函数；已修复：入口校验 stepSize/sceneLength/sceneWidth 有限且 >0，非法时返回空 mesh；格数仍为循环上界与 int 截断并存（DEFERRED，改变体元数量）。
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

PrimMesh VoxelDesigner::createTriEntity(Shape shape, float stepSize)
{
    PrimMesh voxelTriModel;
    ShapeType shapeType = shape.shapetype;

    if (shapeType == ShapeType::CUBE)
    {
        voxelTriModel = createTriCube(shape, stepSize);
    }
    if (shapeType == ShapeType::ELLIPSOID)
    {
        voxelTriModel = createTriEllipsoid(shape, stepSize);
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
    // FIXED[高] 内存: 缓冲区只分配 nImgSizeX*nImgSizeY 个 float，但 RasterIO 以 nBandCount=bandcount（panBandMap=nullptr 即读全部波段）读取，多波段 TIF（如带 alpha 的 RGB/高度图）时写入 bandcount 倍数据，堆溢出；int 乘法对 >2^31 像素溢出；RasterIO 返回值未检查；new[] 在后续抛异常时泄漏；建议：只读第 1 波段（GetRasterBand(1)->RasterIO）、用 std::vector<float>(size_t) 分配并检查 CPLErr（_roof/_wall 两个函数同理）；已修复：改从 GetRasterBand(1) 读第 1 波段，缓冲区用 std::vector<FLOAT>(size_t 像素数)，校验 bandcount/尺寸/CPLErr 并在失败时 GDALClose 后返回；minReHeightScanline 的 new[] 也改用 size_t 像素数（三处），原有 delete[] 保留；_roof/_wall 的读取错误处理未改（DEFERRED）。
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

    FLOAT* minReHeightScanline = new FLOAT[static_cast<std::size_t>(nImgSizeX) * static_cast<std::size_t>(nImgSizeY)];

    float minReHeight, rightReHeight, leftReHeight, upReHeight, downReHeight;
    for (int i = 1; i < nImgSizeX - 1; i++)
    {
        for (int j = 1; j < nImgSizeY - 1; j++)
        {
            // FIXED[中] 内存: heightScanline[(j±1)*nImgSizeX+i] 等下标的乘法仍按 int 计算，超大尺寸时 j*nImgSizeX+i 超过 INT_MAX 溢出为负下标/越界（前轮补漏发现，原文件未标注）；已修复：索引乘法的每个操作数逐项 static_cast 为 std::size_t，下标值不变。
            // all the height
            float curReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);


            leftReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i - 1)] / stepSize);
            rightReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i + 1)] / stepSize);
            upReHeight = std::round(heightScanline[static_cast<std::size_t>(j - 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            downReHeight = std::round(heightScanline[static_cast<std::size_t>(j + 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            // resized height
            minReHeight = std::min({ curReHeight, leftReHeight, rightReHeight, upReHeight, downReHeight });

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

    std::vector<float> heightScanline0;
    int width,height, nband;
    // FIXED[中] 错误处理: 忽略 readImageinout1 的返回值，影像打不开时 width/height 未赋值，convertVector2Mat 用未初始化的行数、高度图为空或长度不符时越界（_wall 同源）；已修复：检查返回值与 width/height 为正、heightScanline0.size()==width*height（size_t 计算），失败时记录日志并返回空 mesh；INTER_LINEAR→INTER_NEAREST 属结果变化，保持 DEFERRED。
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

    FLOAT* minReHeightScanline = new FLOAT[static_cast<std::size_t>(nImgSizeX) * static_cast<std::size_t>(nImgSizeY)];

    float minReHeight, rightReHeight, leftReHeight, upReHeight, downReHeight;
    for (int i = 1; i < nImgSizeX - 1; i++)
    {
        for (int j = 1; j < nImgSizeY - 1; j++)
        {
            // FIXED[中] 内存: heightScanline[(j±1)*nImgSizeX+i] 等下标的乘法仍按 int 计算，超大 targetSize 时 j*nImgSizeX+i 超过 INT_MAX 溢出为负下标/越界（前轮补漏发现，原文件未标注）；已修复：索引乘法的每个操作数逐项 static_cast 为 std::size_t，下标值不变。
            // all the height
            float curReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);


            leftReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i - 1)] / stepSize);
            rightReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i + 1)] / stepSize);
            upReHeight = std::round(heightScanline[static_cast<std::size_t>(j - 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            downReHeight = std::round(heightScanline[static_cast<std::size_t>(j + 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            // resized height
            minReHeight = std::min({ curReHeight, leftReHeight, rightReHeight, upReHeight, downReHeight });

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


     std::vector<float> heightScanline0;
     int width,height, nband;
     // FIXED[中] 错误处理: 忽略 readImageinout1 的返回值，影像打不开时 width/height 未赋值，convertVector2Mat 用未初始化的行数、高度图为空或长度不符时越界（与 _roof 同源）；已修复：检查返回值与 width/height 为正、heightScanline0.size()==width*height（size_t 计算），失败时记录日志并返回空 mesh；INTER_LINEAR→INTER_NEAREST 属结果变化，保持 DEFERRED。
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

    FLOAT* minReHeightScanline = new FLOAT[static_cast<std::size_t>(nImgSizeX) * static_cast<std::size_t>(nImgSizeY)];

    float minReHeight, rightReHeight, leftReHeight, upReHeight, downReHeight;
    for (int i = 1; i < nImgSizeX - 1; i++)
    {
        for (int j = 1; j < nImgSizeY - 1; j++)
        {
            // FIXED[中] 内存: heightScanline[(j±1)*nImgSizeX+i] 等下标的乘法仍按 int 计算，超大 targetSize 时 j*nImgSizeX+i 超过 INT_MAX 溢出为负下标/越界（前轮补漏发现，原文件未标注）；已修复：索引乘法的每个操作数逐项 static_cast 为 std::size_t，下标值不变。
            // all the height
            float curReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);

            if(curReHeight>0)
            {
                int a = 10;
            }

            leftReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i - 1)] / stepSize);
            rightReHeight = std::round(heightScanline[static_cast<std::size_t>(j) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i + 1)] / stepSize);
            upReHeight = std::round(heightScanline[static_cast<std::size_t>(j - 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            downReHeight = std::round(heightScanline[static_cast<std::size_t>(j + 1) * static_cast<std::size_t>(nImgSizeX) + static_cast<std::size_t>(i)] / stepSize);
            // resized height
            minReHeight = std::min({ curReHeight, leftReHeight, rightReHeight, upReHeight, downReHeight });

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
