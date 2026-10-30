/******************************************************************************
 * Copyright 1998-2018 NVIDIA Corp. All Rights Reserved.
 *****************************************************************************/

// This file exist only to do the implementation of tiny obj loader
#define TINYOBJLOADER_IMPLEMENTATION
#include "objloader.h"
#include "nvh/nvprint.hpp"
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
//#include "Interpolate.hpp"

namespace {
// tinyobj only warns about out-of-range face indices and still reports Valid().
const float* objVertex(const tinyobj::attrib_t& attrib, int vertexIndex, const std::string& filename)
{
    if (vertexIndex < 0 || static_cast<size_t>(vertexIndex) * 3 + 2 >= attrib.vertices.size())
        throw std::runtime_error("OBJ face references a missing vertex (" + std::to_string(vertexIndex + 1) + "): " + filename);
    return &attrib.vertices[static_cast<size_t>(vertexIndex) * 3];
}

void requireValidObj(const tinyobj::ObjReader& reader, const std::string& filename)
{
    if (!reader.Valid())
        throw std::runtime_error("Cannot load OBJ: " + filename + " " + reader.Error());
}

int firstMaterialId(const tinyobj::shape_t& shape)
{
    return shape.mesh.material_ids.empty() ? -1 : shape.mesh.material_ids[0];
}
}


//-----------------------------------------------------------------------------
// Extract the directory component from a complete path.
//
#ifdef WIN32
#define CORRECT_PATH_SEP "\\"
#define WRONG_PATH_SEP '/'
#else
#define CORRECT_PATH_SEP "/"
#define WRONG_PATH_SEP '\\'
#endif

/// <summary>
/// 把obj文件中所有的mesh都读取进去，只读取vertex和index
/// </summary>
/// <param name="filename"></param>
void ObjLoader::loadModel(const std::string& filename)
{
    clearCurrentInfo();

    tinyobj::ObjReader reader;
    // DEFERRED[中] 安全: tinyobj::ParseFromFile 内部用窄字符 std::ifstream 打开，Windows 上按 ANSI 代码页解释路径，UTF-8 编码的中文路径会打开失败；同时 reader.Warning()（越界索引、缺失 mtl 等）被完全丢弃；建议：用 std::filesystem::u8path 自行打开流后调用 ParseFromString/LoadObj(istream)，并把非空 Warning 打印出来（loadMesh 同理）。
    reader.ParseFromFile(filename);
    requireValidObj(reader, filename);

    // 对内的所有的材质信息都不读取，只读取结构信息
    // Collecting the material in the scene
    //for(const auto& material : reader.GetMaterials())
    //{
    //  MaterialObj m;
    //  m.ambient  = nvmath::vec3f(material.ambient[0], material.ambient[1], material.ambient[2]);
    //  m.diffuse  = nvmath::vec3f(material.diffuse[0], material.diffuse[1], material.diffuse[2]);
    //  m.specular = nvmath::vec3f(material.specular[0], material.specular[1], material.specular[2]);
    //  m.emission = nvmath::vec3f(material.emission[0], material.emission[1], material.emission[2]);
    //  m.transmittance = nvmath::vec3f(material.transmittance[0], material.transmittance[1],
    //                                  material.transmittance[2]);
    //  m.dissolve      = material.dissolve;
    //  m.ior           = material.ior;
    //  m.shininess     = material.shininess;
    //  m.illum          = material.illum;
    //  if(!material.diffuse_texname.empty())
    //  {
    //    m_textures.push_back(material.diffuse_texname);
    //    m.textureID = static_cast<int>(m_textures.size()) - 1;
    //  }
    //  m_materials.emplace_back(m);
    //}
    //// If there were none, add a default
    //if(m_materials.empty())
    //  m_materials.emplace_back(MaterialObj());

    const tinyobj::attrib_t& attrib = reader.GetAttrib();

    // DEFERRED[低] 性能: 每个面角都生成独立顶点（不共享），百万面 OBJ 的顶点数为 3×面数，内存与 BLAS 构建量约为共享顶点的 3–6 倍；且 m_vertices/m_indices 再整体复制到 m_objmesh，峰值内存翻倍；clearCurrentInfo() 为空实现，同一 ObjLoader 重复调用会在旧数据后追加；建议：按 vertex_index 去重并 std::move 到 m_objmesh，clearCurrentInfo 真正 clear 成员。
    minElevation = attrib.vertices.size() >= 3 ? attrib.vertices[1] : 0.0f;
    for (const auto& shape : reader.GetShapes())
    {

        ShapeInfo     shapeInfo = {};
        shapeInfo.offset = m_indices.size();
        shapeInfo.nbFacet = shape.mesh.num_face_vertices.size();
        shapeInfo.matIndex = firstMaterialId(shape);
        shapeInfo.name = shape.name;
        m_shapeInfo.emplace_back(shapeInfo);

        m_vertices.reserve(shape.mesh.indices.size() + m_vertices.size());
        m_indices.reserve(shape.mesh.indices.size() + m_indices.size());

        for (const auto& index : shape.mesh.indices)
        {
            VertexAttribute    vertex = {};

            const float* vp = objVertex(attrib, index.vertex_index, filename);
            // Unified right-handed convention: +X north, +Y up, +Z east.
            vertex.pos = { *(vp + 0), *(vp + 1), *(vp + 2) };

            if (minElevation > vertex.pos.y) minElevation = vertex.pos.y;

            /* if(!attrib.normals.empty() && index.normal_index >= 0)
             {
               const float* np = &attrib.normals[3 * index.normal_index];
               vertex.nrm      = {*(np + 0), *(np + 1), *(np + 2)};
             }

             if(!attrib.texcoords.empty() && index.texcoord_index >= 0)
             {
               const float* tp = &attrib.texcoords[2 * index.texcoord_index + 0];
               vertex.texCoord = {*tp, 1.0f - *(tp + 1)};
             }*/

             /*if(!attrib.colors.empty())
             {
               const float* vc = &attrib.colors[3 * index.vertex_index];
               vertex.color    = {*(vc + 0), *(vc + 1), *(vc + 2)};
             }*/

            m_vertices.push_back(vertex);
            m_indices.push_back(static_cast<int>(m_indices.size()));
        }
    }
    m_objmesh.nVertices = m_vertices.size();
    m_objmesh.nIndices = m_indices.size();
    m_objmesh.vertices = m_vertices;
    m_objmesh.indices = m_indices;

    // Fixing material indices
    //for(auto& mi : m_matIndx)
    //{
    //  if(mi < 0 || mi > m_materials.size())
    //    mi = 0;
    //}


    // 如果没有法线，则计算法线
    //if(attrib.normals.empty())
    //{
    //  for(size_t i = 0; i < m_indices.size(); i += 3)
    //  {
    //    VertexObj& v0 = m_vertices[m_indices[i + 0]];
    //    VertexObj& v1 = m_vertices[m_indices[i + 1]];
    //    VertexObj& v2 = m_vertices[m_indices[i + 2]];

    //    nvmath::vec3f n = nvmath::normalize(nvmath::cross((v1.pos - v0.pos), (v2.pos - v0.pos)));
    //    v0.nrm          = n;
    //    v1.nrm          = n;
    //    v2.nrm          = n;
    //  }
    //}
}



/// <summary>
/// 把obj文件中特定mesh名字的读取进去，只读取vertex和index
/// </summary>
/// <param name="filename"></param>
/// <param name="meshname"></param>
void ObjLoader::loadMesh(const std::string& filename, const std::string& meshname,
                         bool exactMatch)
{
    clearCurrentInfo();

    tinyobj::ObjReader reader;
    reader.ParseFromFile(filename);
    requireValidObj(reader, filename);

    const tinyobj::attrib_t& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    // tinyobj splits repeated `g name` blocks into separate shapes; all of
    // them belong to the requested mesh.
    const bool found = std::any_of(shapes.begin(), shapes.end(),
        [&meshname](const tinyobj::shape_t& shape) { return shape.name == meshname; });
    const bool loadAll = !found && !exactMatch;
    if (loadAll && !shapes.empty()) {
        std::cerr << "Mesh '" << meshname << "' not found in " << filename
                  << ", using the whole OBJ\n";
    }
    if (!found && exactMatch) {
        std::cerr << "Mesh '" << meshname << "' not found in " << filename << '\n';
    }

    for (const auto& shape : shapes)
    {
        if (!loadAll && shape.name != meshname) continue;
        if (shape.mesh.indices.empty()) continue;
        ShapeInfo     shapeInfo = {};
        shapeInfo.offset = m_indices.size();
        shapeInfo.nbFacet = shape.mesh.num_face_vertices.size();
        shapeInfo.matIndex = firstMaterialId(shape);
        shapeInfo.name = shape.name;
        m_shapeInfo.emplace_back(shapeInfo);

        m_vertices.reserve(shape.mesh.indices.size() + m_vertices.size());
        m_indices.reserve(shape.mesh.indices.size() + m_indices.size());

        for (const auto& index : shape.mesh.indices)
        {
            VertexAttribute    vertex = {};

            const float* vp = objVertex(attrib, index.vertex_index, filename);
            // Keep mesh-specific loading identical to whole-OBJ loading.
            vertex.pos = { *(vp + 0), *(vp + 1), *(vp + 2) };

            /*if (!attrib.normals.empty() && index.normal_index >= 0)
            {
               const float* np = &attrib.normals[3 * index.normal_index];
               vertex.nrm = { *(np + 0), *(np + 1), *(np + 2) };
            }*/

            if (index.texcoord_index >= 0
                && static_cast<size_t>(index.texcoord_index) * 2 + 1 < attrib.texcoords.size())
            {
                const float* tp = &attrib.texcoords[static_cast<size_t>(index.texcoord_index) * 2];
                vertex.texCoord = { *tp, 1.0f - *(tp + 1) };
            }

            if (static_cast<size_t>(index.vertex_index) * 3 + 2 < attrib.colors.size())
            {
                const float* vc = &attrib.colors[static_cast<size_t>(index.vertex_index) * 3];
                vertex.color = { *(vc + 0), *(vc + 1), *(vc + 2) };
            }

            m_vertices.push_back(vertex);
            m_indices.push_back(static_cast<int>(m_indices.size()));
        }
    }

    m_objmesh.nVertices = m_vertices.size();
    m_objmesh.nIndices = m_indices.size();
    m_objmesh.vertices = m_vertices;
    m_objmesh.indices = m_indices;
}

void ObjLoader::createBackground(glm::vec3 bgSize)
{
    clearCurrentInfo();

    float length = bgSize.x;
    float width = bgSize.y;

    float originx = -length / 2.0;
    float originy = -width / 2.0;
    float finalx = length / 2.0;
    float finaly = width / 2.0;

    nvmath::vec3f points[6];
    points[0].x = originx;
    points[1].x = originx;
    points[2].x = finalx;
    points[3].x = finalx;
    points[0].z = originy;
    points[1].z = finaly;
    points[2].z = finaly;
    points[3].z = originy;
    points[0].y = 0;
    points[1].y = 0;
    points[2].y = 0;
    points[3].y = 0;

    int list[] = {0,1,2,0,2,3};
    for (int i = 0; i < 6; i++)
    {
        int listi = list[i];
        VertexAttribute va{ points[listi] };
        m_objmesh.vertices.emplace_back(va);
    }

    m_objmesh.indices.emplace_back(0);
    m_objmesh.indices.emplace_back(0 + 1);
    m_objmesh.indices.emplace_back(0 + 2);
    m_objmesh.indices.emplace_back(0 + 3);
    m_objmesh.indices.emplace_back(0 + 4);
    m_objmesh.indices.emplace_back(0 + 5);

    m_objmesh.nIndices = m_objmesh.indices.size();
    m_objmesh.nVertices = m_objmesh.vertices.size();
    minElevation = 0;
}

void ObjLoader::createGridBackground(glm::vec3 bgSize, float step)
{
    clearCurrentInfo();
    // Same resolution cap as DEM ground: at most 2048 cells per side.
    const float cappedStep = std::max(std::max(step, 0.01f), std::max(bgSize.x, bgSize.y) / 2048.0f);
    const int columns = std::max(2, static_cast<int>(std::ceil(bgSize.y / cappedStep)) + 1);
    const int rows = std::max(2, static_cast<int>(std::ceil(bgSize.x / cappedStep)) + 1);
    m_vertices.clear();
    m_indices.clear();
    m_vertices.reserve(static_cast<size_t>(rows) * columns);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            const float u = static_cast<float>(c) / static_cast<float>(columns - 1);
            const float v = static_cast<float>(r) / static_cast<float>(rows - 1);
            nvmath::vec3f pos = { (0.5f - v) * bgSize.x, 0.0f, (u - 0.5f) * bgSize.y };
            m_vertices.emplace_back(VertexAttribute{ pos });
        }
    }
    for (int r = 0; r < rows - 1; ++r) {
        for (int c = 0; c < columns - 1; ++c) {
            const uint32_t topLeft = r * columns + c, topRight = topLeft + 1;
            const uint32_t bottomLeft = (r + 1) * columns + c, bottomRight = bottomLeft + 1;
            m_indices.insert(m_indices.end(), { topLeft, bottomLeft, topRight, topRight, bottomLeft, bottomRight });
        }
    }
    m_objmesh.nVertices = static_cast<uint32_t>(m_vertices.size());
    m_objmesh.nIndices = static_cast<uint32_t>(m_indices.size());
    m_objmesh.vertices = m_vertices;
    m_objmesh.indices = m_indices;
    minElevation = 0;
}


// void ObjLoader::creatBackgroundFromDEM(const std::string& filename, nvmath::vec3f sceneSize)
// {
//     clearCurrentInfo();
//
//     CPLSetConfigOption("gdal_filename_is_utf8", "no");
//     float scale = 1.0;
//
//     std::string infile = filename;
//     GDALAllRegister();
//
//     float * pafScanblock;
//     GDALDataset* poDataset;
//     poDataset = (GDALDataset*)GDALOpen(infile.c_str(), GA_ReadOnly);
//     if (poDataset == NULL)
//     {
//         std::cout << "fail in open files!" << std::endl;
//         return;
//     }
//     int nImgSizeX = poDataset->GetRasterXSize();
//     int nImgSizeY = poDataset->GetRasterYSize();
//     int bandcount = poDataset->GetRasterCount();
//
//     int num_image_size = 0;
//     float * pafScanline = new float[nImgSizeX * nImgSizeY * bandcount];
//     poDataset->RasterIO(GF_Read, 0, 0, nImgSizeX, nImgSizeY, pafScanline, nImgSizeX, nImgSizeY, GDT_Float32, bandcount, 0, 0, 0, 0);
//
//     scale = sceneSize.x / nImgSizeX;
//
//     cv::Mat A(nImgSizeX,nImgSizeY,CV_32F, pafScanline);
//
//     int newX = nImgSizeX * scale;
//     int newY = nImgSizeY * scale;
// //    float * newImage = new float[newX*newY];
//     cv::Mat newImage(newX,newY,CV_32F,0);
//     cv::resize(A,newImage,cv::Size(newX,newY),cv::INTER_CUBIC);
// //    std::vector<double> x, y, z;
// //    for (int i = 0; i < nImgSizeX; i++)
// //    {
// //        for (int j = 0; j < nImgSizeY; j++)
// //        {
// //            x.emplace_back(i * scale + scale / 2.0);
// //            y.emplace_back(j * scale + scale / 2.0);
// //            z.emplace_back(pafScanline[i * nImgSizeY + j]);
// //           /* y.emplace_back(i * scale + scale / 2.0);
// //            x.emplace_back(j * scale + scale / 2.0);
// //            z.emplace_back(pafScanline[i * nImgSizeY + j]);*/
// //        }
// //    }
// //    _2D::ThinPlateSplineInterpolator<double> interp;
// //    interp.setData(x, y, z);
//
//
//     float step = 1/10.0;
//     delete[] pafScanline;
//     delete poDataset;
//     scale = sceneSize.x / nImgSizeX;
//
//     //scale = 1;
//     //std::vector<nvmath::vec3f> points;
//     uint32_t nVertices = 0;
//     uint32_t nIndices = 0;
// //    minElevation = newImage.at<float>(0,0);
//     minElevation = *std::min_element(newImage.begin<float>(), newImage.end<float>());
//     centerElevation = (*std::max_element(newImage.begin<float>(), newImage.end<float>()) + minElevation) / 2.0;
//     for (float j = 0.0; j < nImgSizeX; j = j + step)
//     {
//         for (float k = 0.0; k < nImgSizeY; k = k + step)
//         {
//             float jj1, kk1, jj2, kk2;
//
//             jj1 = j * scale;
//             jj2 = (j + step) * scale;
//             kk1 = k * scale;
//             kk2 = (k + step) * scale;
//
//             if (jj1 < scale / 2.0) jj1 = scale / 2.0;
//             if (jj2 < scale / 2.0) jj2 = scale / 2.0;
//             if (kk1 < scale / 2.0) kk1 = scale / 2.0;
//             if (kk2 < scale / 2.0) kk2 = scale / 2.0;
//             if (jj1 > nImgSizeX * scale - scale / 2.0) jj1 = nImgSizeX * scale - scale / 2.0 - 0.01;
//             if (jj2 > nImgSizeX * scale - scale / 2.0) jj2 = nImgSizeX * scale - scale / 2.0 - 0.01;
//             if (kk1 > nImgSizeY * scale - scale / 2.0) kk1 = nImgSizeY * scale - scale / 2.0 - 0.01;
//             if (kk2 > nImgSizeY * scale - scale / 2.0) kk2 = nImgSizeY * scale - scale / 2.0 - 0.01;
//
//             nvmath::vec3f point1 = { j * scale - sceneSize.x / 2.0, newImage.at<float>(jj1, kk1) - centerElevation, k * scale - sceneSize.y/2.0 };
//             nvmath::vec3f point2 = { j * scale - sceneSize.x / 2.0, newImage.at<float>(jj1, kk2) - centerElevation, (k + step) * scale  - sceneSize.y/2.0};
//             nvmath::vec3f point3 = { (j + step) * scale - sceneSize.x / 2.0, newImage.at<float>(jj2, kk2) - centerElevation, (k + step) * scale  - sceneSize.y/2.0};
//             nvmath::vec3f point4 = { (j + step) * scale - sceneSize.x / 2.0, newImage.at<float>(jj2, kk1) - centerElevation, k * scale  - sceneSize.y/2.0};
//
// //            if (minElevation > point1.y) minElevation = point1.y;
// //            if (minElevation > point2.y) minElevation = point2.y;
// //            if (minElevation > point3.y) minElevation = point3.y;
// //            if (minElevation > point4.y) minElevation = point4.y;
//
//             VertexAttribute va1{ point1 };
//             m_vertices.emplace_back(va1);
//             VertexAttribute va2{ point2 };
//             m_vertices.emplace_back(va2);
//             VertexAttribute va3{ point3 };
//             m_vertices.emplace_back(va3);
//             VertexAttribute va4{ point4 };
//             m_vertices.emplace_back(va4);
//
//
//             m_indices.emplace_back(nVertices);
//             m_indices.emplace_back(nVertices + 1);
//             m_indices.emplace_back(nVertices + 2);
//             m_indices.emplace_back(nVertices + 0);
//             m_indices.emplace_back(nVertices + 2);
//             m_indices.emplace_back(nVertices + 3);
//             nVertices = nVertices + 4;
//             nIndices = nIndices + 2 * 3;
//         }
//     }
//
//     m_objmesh.nVertices = nVertices;
//     m_objmesh.nIndices = nIndices;
//     m_objmesh.vertices = m_vertices;
//     m_objmesh.indices = m_indices;
//
//     m_heightmap = newImage.clone(); // 保存插值后的高程图
// }

//void ObjLoader::createBackgroundFromDEM(const std::string& filename, nvmath::vec3f sceneSize_XYZ, _2D::ThinPlateSplineInterpolator<double>& interp)
//{
//    clearCurrentInfo();
//
//    CPLSetConfigOption("gdal_filename_is_utf8", "no");
//    float scale = 1.0;
//
//    std::string infile = filename;
//    GDALAllRegister();
//
//    BYTE* pafScanblock;
//    GDALDataset* poDataset;
//    poDataset = (GDALDataset*)GDALOpen(infile.c_str(), GA_ReadOnly);
//    if (poDataset == NULL)
//    {
//        std::cout << "fail in open files!" << std::endl;
//        return;
//    }
//    int nImgSizeX = poDataset->GetRasterXSize();
//    int nImgSizeY = poDataset->GetRasterYSize();
//    int bandcount = poDataset->GetRasterCount();
//
//    int num_image_size = 0;
//    /*BYTE* pafScanline = new BYTE[nImgSizeX * nImgSizeY * bandcount];
//    poDataset->RasterIO(GF_Read, 0, 0, nImgSizeX, nImgSizeY, pafScanline, nImgSizeX, nImgSizeY, GDT_Byte, bandcount, 0, 0, 0, 0);*/
//
//    FLOAT* pafScanline = new FLOAT[nImgSizeX * nImgSizeY];
//    poDataset->RasterIO(GF_Read, 0, 0, nImgSizeX, nImgSizeY, pafScanline, nImgSizeX, nImgSizeY, GDT_Float32, bandcount, 0, 0, 0, 0);
//
//    scale = sceneSize_XYZ.x / nImgSizeX;
//
//    std::vector<double> x, y, z;
//    for (int i = 0; i < nImgSizeX; i++)
//    {
//        for (int j = 0; j < nImgSizeY; j++)
//        {
//            x.emplace_back(i * scale + scale / 2.0);
//            y.emplace_back(j * scale + scale / 2.0);
//            z.emplace_back(pafScanline[i * nImgSizeY + j]);
//        }
//    }
//    interp.setData(x, y, z);
//
//    float step = 1.0 / 2.0;
//    delete[] pafScanline;
//    delete poDataset;
//
//
//    //scale = 1;
//    //std::vector<nvmath::vec3f> points;
//    uint32_t nVertices = 0;
//    uint32_t nIndices = 0;
//    minElevation = z[0];
//    for (float j = 0.0; j < nImgSizeX; j = j + step)
//    {
//        for (float k = 0.0; k < nImgSizeY; k = k + step)
//        {
//            float jj1, kk1, jj2, kk2;
//
//            jj1 = j * scale;
//            jj2 = (j + step) * scale;
//            kk1 = k * scale;
//            kk2 = (k + step) * scale;
//
//            if (jj1 < scale / 2.0) jj1 = scale / 2.0;
//            if (jj2 < scale / 2.0) jj2 = scale / 2.0;
//            if (kk1 < scale / 2.0) kk1 = scale / 2.0;
//            if (kk2 < scale / 2.0) kk2 = scale / 2.0;
//            if (jj1 > nImgSizeX * scale - scale / 2.0) jj1 = nImgSizeX * scale - scale / 2.0 - 0.01;
//            if (jj2 > nImgSizeX * scale - scale / 2.0) jj2 = nImgSizeX * scale - scale / 2.0 - 0.01;
//            if (kk1 > nImgSizeY * scale - scale / 2.0) kk1 = nImgSizeY * scale - scale / 2.0 - 0.01;
//            if (kk2 > nImgSizeY * scale - scale / 2.0) kk2 = nImgSizeY * scale - scale / 2.0 - 0.01;
//
//            nvmath::vec3f point1 = { j * scale, interp(jj1, kk1), k * scale };
//            nvmath::vec3f point2 = { j * scale, interp(jj1, kk2), (k + step) * scale };
//            nvmath::vec3f point3 = { (j + step) * scale, interp(jj2, kk2), (k + step) * scale };
//            nvmath::vec3f point4 = { (j + step) * scale, interp(jj2, kk1), k * scale };
//
//            if (minElevation > point1.y) minElevation = point1.y;
//            if (minElevation > point2.y) minElevation = point2.y;
//            if (minElevation > point3.y) minElevation = point3.y;
//            if (minElevation > point4.y) minElevation = point4.y;
//
//            VertexAttribute va1{ point1 };
//            m_vertices.emplace_back(va1);
//            VertexAttribute va2{ point2 };
//            m_vertices.emplace_back(va2);
//            VertexAttribute va3{ point3 };
//            m_vertices.emplace_back(va3);
//            VertexAttribute va4{ point4 };
//            m_vertices.emplace_back(va4);
//
//
//            m_indices.emplace_back(nVertices);
//            m_indices.emplace_back(nVertices + 1);
//            m_indices.emplace_back(nVertices + 2);
//            m_indices.emplace_back(nVertices + 0);
//            m_indices.emplace_back(nVertices + 2);
//            m_indices.emplace_back(nVertices + 3);
//            nVertices = nVertices + 4;
//            nIndices = nIndices + 2 * 3;
//        }
//    }
//
//    m_objmesh.nVertices = nVertices;
//    m_objmesh.nIndices = nIndices;
//    m_objmesh.vertices = m_vertices;
//    m_objmesh.indices = m_indices;
//}
//
//void ObjLoader:: createBackgroundFromDEM(const std::string& filename, nvmath::vec3f sceneSize_XYZ, _2D::BilinearInterpolator<double>& interp)
//{
//    clearCurrentInfo();
//
//    CPLSetConfigOption("gdal_filename_is_utf8", "no");
//
//    std::string infile = filename;
//    GDALAllRegister();
//
//    BYTE* pafScanblock;
//    GDALDataset* poDataset;
//    poDataset = (GDALDataset*)GDALOpen(infile.c_str(), GA_ReadOnly);
//    if (poDataset == NULL)
//    {
//        std::cout << "fail in open files!" << std::endl;
//        return;
//    }
//    nImgSizeX = poDataset->GetRasterXSize();
//    nImgSizeY = poDataset->GetRasterYSize();
//    int bandcount = poDataset->GetRasterCount();
//
//    int num_image_size = 0;
//
//    FLOAT* pafScanline = new FLOAT[nImgSizeX * nImgSizeY];
//    poDataset->RasterIO(GF_Read, 0, 0, nImgSizeX, nImgSizeY, pafScanline, nImgSizeX, nImgSizeY, GDT_Float32, bandcount, 0, 0, 0, 0);
//
//    m_scale = sceneSize_XYZ.x / nImgSizeX;
//    float scale = m_scale;
//
//    std::vector<double> x, y, z;
//    for (int i = 0; i < nImgSizeX; i++)
//    //for (int i = nImgSizeX-1; i >=0; i--)
//    {
//        for (int j = 0; j < nImgSizeY; j++)
//        {
//            //x.emplace_back(sceneSize_XYZ.x - (i * scale + scale / 2.0)); // pixel center location
//            //y.emplace_back(j * scale + scale / 2.0);
//            //z.emplace_back(pafScanline[i * nImgSizeY + j]);
//            x.emplace_back((i * scale + scale / 2.0)); // pixel center location
//            y.emplace_back((j * scale + scale / 2.0));
//            z.emplace_back(pafScanline[i * nImgSizeY + j]);
//        }
//    }
//
//    /*for (int i = 0; i < nImgSizeX; i++)
//    {
//        x[i] = sceneSize_XYZ.x - x[i];
//    }*/
//
//    interp.setData(x, y, z);
//
//    delete[] pafScanline;
//    delete poDataset;
//
//    float step = m_step;// / 2.0; // control DEM subdivision degree, The smaller the value, the finer the distinction
//
//    uint32_t nVertices = 0;
//    uint32_t nIndices = 0;
//    minElevation = z[0];
//    for(float j = 0.0; j < nImgSizeX; j = j + step)
//    {
//        for (float k = 0.0; k < nImgSizeY; k = k + step)
//        {
//            float jj1, kk1, jj2, kk2;
//
//            /*jj1 = j * scale;
//            jj2 = (j + step) * scale;*/
//
//            // 将 x 轴对换，与envi保持一致性
//            jj1 = sceneSize_XYZ.x - j * scale;
//            jj2 = sceneSize_XYZ.x - (j + step) * scale;
//            kk1 = k * scale;
//            kk2 = (k + step) * scale;
//
//            if (jj1 < scale / 2.0) jj1 = scale / 2.0;
//            if (jj2 < scale / 2.0) jj2 = scale / 2.0;
//            if (kk1 < scale / 2.0) kk1 = scale / 2.0;
//            if (kk2 < scale / 2.0) kk2 = scale / 2.0;
//            if (jj1 > nImgSizeX * scale - scale / 2.0) jj1 = nImgSizeX * scale - scale / 2.0 - 0.01;
//            if (jj2 > nImgSizeX * scale - scale / 2.0) jj2 = nImgSizeX * scale - scale / 2.0 - 0.01;
//            if (kk1 > nImgSizeY * scale - scale / 2.0) kk1 = nImgSizeY * scale - scale / 2.0 - 0.01;
//            if (kk2 > nImgSizeY * scale - scale / 2.0) kk2 = nImgSizeY * scale - scale / 2.0 - 0.01;
//
//            nvmath::vec3f point1 = { j * scale, interp(jj1, kk1), k * scale };
//            nvmath::vec3f point2 = { j * scale, interp(jj1, kk2), (k + step) * scale };
//            nvmath::vec3f point3 = { (j + step) * scale, interp(jj2, kk2), (k + step) * scale };
//            nvmath::vec3f point4 = { (j + step) * scale, interp(jj2, kk1), k * scale };
//
//            point1 = point1 - nvmath::vec3f({ sceneSize_XYZ.x / 2.0, 0, sceneSize_XYZ.y / 2.0 });
//            point2 = point2 - nvmath::vec3f({ sceneSize_XYZ.x / 2.0, 0, sceneSize_XYZ.y / 2.0 });
//            point3 = point3 - nvmath::vec3f({ sceneSize_XYZ.x / 2.0, 0, sceneSize_XYZ.y / 2.0 });
//            point4 = point4 - nvmath::vec3f({ sceneSize_XYZ.x / 2.0, 0, sceneSize_XYZ.y / 2.0 });
//
//            if (minElevation > point1.y) minElevation = point1.y;
//            if (minElevation > point2.y) minElevation = point2.y;
//            if (minElevation > point3.y) minElevation = point3.y;
//            if (minElevation > point4.y) minElevation = point4.y;
//
//            VertexAttribute va1{ point1 };
//            m_vertices.emplace_back(va1);
//            VertexAttribute va2{ point2 };
//            m_vertices.emplace_back(va2);
//            VertexAttribute va3{ point3 };
//            m_vertices.emplace_back(va3);
//            VertexAttribute va4{ point4 };
//            m_vertices.emplace_back(va4);
//
//
//            m_indices.emplace_back(nVertices);
//            m_indices.emplace_back(nVertices + 1);
//            m_indices.emplace_back(nVertices + 2);
//            m_indices.emplace_back(nVertices + 0);
//            m_indices.emplace_back(nVertices + 2);
//            m_indices.emplace_back(nVertices + 3);
//            nVertices = nVertices + 4;
//            nIndices = nIndices + 2 * 3;
//        }
//    }
//
//    m_objmesh.nVertices = nVertices;
//    m_objmesh.nIndices = nIndices;
//    m_objmesh.vertices = m_vertices;
//    m_objmesh.indices = m_indices;
//}
//
//void ObjLoader::createBackgroundFromResizedDEM(const std::string &filename, nvmath::vec3f sceneSize_XYZ, _2D::BilinearInterpolator<double> &interp)
//{
//    clearCurrentInfo();
//
//    CPLSetConfigOption("gdal_filename_is_utf8", "no");
//
//    std::string infile = filename;
//    GDALAllRegister();
//
//    BYTE *pafScanblock;
//    GDALDataset *poDataset;
//    poDataset = (GDALDataset *)GDALOpen(infile.c_str(), GA_ReadOnly);
//    if (poDataset == NULL)
//    {
//        std::cout << "fail in open files!" << std::endl;
//        return;
//    }
//    nImgSizeX = poDataset->GetRasterXSize();
//    nImgSizeY = poDataset->GetRasterYSize();
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
//    interp.setData(x, y, z);
//
//    delete[] pafScanline;
//    delete poDataset;
//
//    float step = m_step; // / 2.0; // control DEM subdivision degree, The smaller the value, the finer the distinction
//    //step = 5;
//
//    uint32_t nVertices = 0;
//    uint32_t nIndices = 0;
//    minElevation = z[0];
//
//    for (float j = 0.0; j < sceneSize_XYZ.x; j = j + step)
//    {
//        for (float k = 0.0; k < sceneSize_XYZ.y; k = k + step)
//        {
//            float jj1, kk1, jj2, kk2;
//
//            // 将 x 轴对换，与envi保持一致性
//            jj1 = sceneSize_XYZ.x - j;
//            jj2 = sceneSize_XYZ.x - (j + step);
//            kk1 = k;
//            kk2 = (k + step);
//
//            /*if (jj1 < step / 2.0)
//                jj1 = step / 2.0;
//            if (jj2 < step / 2.0)
//                jj2 = step / 2.0;
//            if (kk1 < step / 2.0)
//                kk1 = step / 2.0;
//            if (kk2 < step / 2.0)
//                kk2 = step / 2.0;
//            if (jj1 > sceneSize_XYZ.x - step / 2.0)
//                jj1 = sceneSize_XYZ.x - step / 2.0 - 0.01;
//            if (jj2 > sceneSize_XYZ.x - step / 2.0)
//                jj2 = sceneSize_XYZ.x - step / 2.0 - 0.01;
//            if (kk1 > sceneSize_XYZ.y - step / 2.0)
//                kk1 = sceneSize_XYZ.y - step / 2.0 - 0.01;
//            if (kk2 > sceneSize_XYZ.y - step / 2.0)
//                kk2 = sceneSize_XYZ.y - step / 2.0 - 0.01;*/
//
//            float correctValue = 0.02;
//            if (jj1 < m_scale/2.0)
//                jj1 = m_scale / 2.0 + correctValue;
//            if (jj2 < m_scale / 2.0)
//                jj2 = m_scale / 2.0 + correctValue;
//            if (kk1 < m_scale / 2.0)
//                kk1 = m_scale / 2.0 + correctValue;
//            if (kk2 < m_scale / 2.0)
//                kk2 = m_scale / 2.0 + correctValue;
//            if (jj1 > sceneSize_XYZ.x - m_scale / 2.0)
//                jj1 = sceneSize_XYZ.x - m_scale / 2.0 - correctValue;
//            if (jj2 > sceneSize_XYZ.x - m_scale / 2.0)
//                jj2 = sceneSize_XYZ.x - m_scale / 2.0 - correctValue;
//            if (kk1 > sceneSize_XYZ.y - m_scale / 2.0)
//                kk1 = sceneSize_XYZ.y - m_scale / 2.0 - correctValue;
//            if (kk2 > sceneSize_XYZ.y - m_scale / 2.0)
//                kk2 = sceneSize_XYZ.y - m_scale / 2.0 - correctValue;
//
//            nvmath::vec3f point1 = {j, interp(jj1, kk1), k};
//            nvmath::vec3f point2 = {j, interp(jj1, kk2), (k + step)};
//            nvmath::vec3f point3 = {(j + step), interp(jj2, kk2), (k + step)};
//            nvmath::vec3f point4 = {(j + step), interp(jj2, kk1), k};
//
//            point1 = point1 - nvmath::vec3f({sceneSize_XYZ.x / 2.0, 0, sceneSize_XYZ.y / 2.0});
//            point2 = point2 - nvmath::vec3f({sceneSize_XYZ.x / 2.0, 0, sceneSize_XYZ.y / 2.0});
//            point3 = point3 - nvmath::vec3f({sceneSize_XYZ.x / 2.0, 0, sceneSize_XYZ.y / 2.0});
//            point4 = point4 - nvmath::vec3f({sceneSize_XYZ.x / 2.0, 0, sceneSize_XYZ.y / 2.0});
//
//            if (minElevation > point1.y)
//                minElevation = point1.y;
//            if (minElevation > point2.y)
//                minElevation = point2.y;
//            if (minElevation > point3.y)
//                minElevation = point3.y;
//            if (minElevation > point4.y)
//                minElevation = point4.y;
//
//            VertexAttribute va1{point1};
//            m_vertices.emplace_back(va1);
//            VertexAttribute va2{point2};
//            m_vertices.emplace_back(va2);
//            VertexAttribute va3{point3};
//            m_vertices.emplace_back(va3);
//            VertexAttribute va4{point4};
//            m_vertices.emplace_back(va4);
//
//            m_indices.emplace_back(nVertices);
//            m_indices.emplace_back(nVertices + 1);
//            m_indices.emplace_back(nVertices + 2);
//            m_indices.emplace_back(nVertices + 0);
//            m_indices.emplace_back(nVertices + 2);
//            m_indices.emplace_back(nVertices + 3);
//            nVertices = nVertices + 4;
//            nIndices = nIndices + 2 * 3;
//        }
//    }
//
//    m_objmesh.nVertices = nVertices;
//    m_objmesh.nIndices = nIndices;
//    m_objmesh.vertices = m_vertices;
//    m_objmesh.indices = m_indices;
//}
//
//double ObjLoader::getShiftInterp(_2D::BilinearInterpolator<double> interp, nvmath::vec3f shift0)
//{
//    float scale = m_scale;
//
//    shift0.x = nImgSizeX * scale - shift0.x;//d与envi对齐
//
//    float correct = 1;
//    if (shift0.x < scale / 2.0)
//        shift0.x = scale / 2.0 + 1;
//    if (shift0.y < scale / 2.0)
//        shift0.y = scale / 2.0 + 1;
//    if (shift0.x > nImgSizeX * scale - scale / 2.0)
//        shift0.x = nImgSizeX * scale - scale / 2.0 - 1;
//    if (shift0.y > nImgSizeX * scale - scale / 2.0)
//        shift0.y = nImgSizeX * scale - scale / 2.0 - 1;
//
//    //float correctValue = 1;
//    ///*if (shift0.x < m_step - nImgSizeX * scale/2)
//    //    shift0.x = m_step - nImgSizeX * scale / 2 + correctValue;
//    //if (shift0.y < m_step - nImgSizeY * scale / 2)
//    //    shift0.y = m_step - nImgSizeY * scale / 2 + correctValue;
//    //if (shift0.x > nImgSizeX * scale - nImgSizeX * scale / 2 - m_step)
//    //    shift0.x = nImgSizeX * scale - nImgSizeX * scale / 2 - m_step - correctValue;
//    //if (shift0.y > shift0.y - nImgSizeY * scale / 2 - m_scale)
//    //    shift0.y = nImgSizeX * scale - nImgSizeY * scale / 2 - m_step - correctValue;*/
//    //if (shift0.x < m_step)
//    //    shift0.x = m_step + correctValue;
//    //if (shift0.y < m_step)
//    //    shift0.y = m_step + correctValue;
//    //if (shift0.x > nImgSizeX * scale - m_step)
//    //    shift0.x = nImgSizeX * scale - m_step - correctValue;
//    //if (shift0.y > nImgSizeY * scale - m_step)
//    //    shift0.y = nImgSizeY * scale - m_step - correctValue;
//
//    ////shift0.x = nImgSizeX * scale - shift0.x; //d与envi对齐
//
//    return interp(shift0.x, shift0.y);
//}



void ObjLoader::clearCurrentInfo()
{
//    m_vertices.swap(std::vector<VertexAttribute>());
//    m_indices.swap(std::vector<uint32_t>());
}



// 在类中实现该函数
// FIXED[中] 未初始化: m_worldOrigin/m_worldSize 从未被赋值（nvmath::vec3f 默认不清零），范围判断与归一化使用垃圾值，m_worldSize 为 0 时还会除零；m_heightmap 为空时 at<float> 越界；已修复：getHeightAt 入口先判 m_heightmap 为空、再判 m_worldSize.x/y 非正（含 NaN），命中即返回，creatBackgroundFromDEM 中按 sceneSize 设置 m_worldOrigin={-sceneSize.x/2,0,-sceneSize.y/2}、m_worldSize=sceneSize。
float ObjLoader::getHeightAt(float worldX, float worldZ)
{
    // 注意：先判空再读 m_worldSize，避免 heightmap 尚未生成时读到未初始化成员。
    if (m_heightmap.empty() || !(m_worldSize.x > 0.0f) || !(m_worldSize.y > 0.0f))
        return minElevation - centerElevation;

    // 1. 检查是否在地形范围内
    float localX = worldX - m_worldOrigin.x;
    float localZ = worldZ - m_worldOrigin.z; // 假设Z轴对应图像的行(Row/Height)

    if (localX < 0 || localX > m_worldSize.x || localZ < 0 || localZ > m_worldSize.y) {
        // 超出范围，返回最低高度或者一个默认值
        return minElevation - centerElevation;
    }

    // GeoTIFF columns grow eastward (+Z); row zero is north (+X).
    float imgX = (localZ / m_worldSize.y) * (m_heightmap.cols - 1);
    float imgY = (1.0f - localX / m_worldSize.x) * (m_heightmap.rows - 1);

    // 3. 双线性插值 (Bilinear Interpolation)
    int x0 = (int)std::floor(imgX);
    int y0 = (int)std::floor(imgY);
    int x1 = std::min(x0 + 1, m_heightmap.cols - 1);
    int y1 = std::min(y0 + 1, m_heightmap.rows - 1);

    float dx = imgX - x0;
    float dy = imgY - y0;

    // 读取四个角的高度值
    float h00 = m_heightmap.at<float>(y0, x0);
    float h10 = m_heightmap.at<float>(y0, x1);
    float h01 = m_heightmap.at<float>(y1, x0);
    float h11 = m_heightmap.at<float>(y1, x1);

    // 插值计算
    // 先在 X 方向插值
    float h0 = h00 * (1 - dx) + h10 * dx;
    float h1 = h01 * (1 - dx) + h11 * dx;

    // 再在 Y 方向插值
    float height = h0 * (1 - dy) + h1 * dy;

    // 4. 记得减去中心偏移量 (以此匹配生成的 Mesh)
    return height - centerElevation;
}


void ObjLoader::creatBackgroundFromDEM(const std::string& filename,
                                        nvmath::vec3f sceneSize,
                                        float targetStep)
{
    if (!std::isfinite(sceneSize.x) || !std::isfinite(sceneSize.y) ||
        !(sceneSize.x > 0.0f) || !(sceneSize.y > 0.0f))
        throw std::runtime_error("DEM scene extent must be finite and positive: " + filename);

    clearCurrentInfo();

    GDALAllRegister();
    GDALDataset* poDataset = static_cast<GDALDataset*>(
        GDALOpen(filename.c_str(), GA_ReadOnly));
    if (!poDataset || poDataset->GetRasterCount() < 1) {
        if (poDataset) GDALClose(poDataset);
        throw std::runtime_error("Cannot open DEM: " + filename);
    }

    const int nImgSizeX = poDataset->GetRasterXSize();
    const int nImgSizeY = poDataset->GetRasterYSize();
    if (nImgSizeX < 2 || nImgSizeY < 2) {
        GDALClose(poDataset);
        throw std::runtime_error("DEM must contain at least 2 x 2 cells: " + filename);
    }

    // Elevation is band 1 only; nodata and non-finite cells take the valid
    // minimum, as in the voxel DEM path, so they cannot lift the terrain.
    cv::Mat rawDem(nImgSizeY, nImgSizeX, CV_32F);
    GDALRasterBand* band = poDataset->GetRasterBand(1);
    int hasNoData = 0;
    const double noData = band->GetNoDataValue(&hasNoData);
    const CPLErr readResult = band->RasterIO(
        GF_Read, 0, 0, nImgSizeX, nImgSizeY, rawDem.ptr<float>(),
        nImgSizeX, nImgSizeY, GDT_Float32, 0, 0);
    GDALClose(poDataset);
    if (readResult != CE_None) throw std::runtime_error("Cannot read DEM elevations: " + filename);

    const auto isValid = [hasNoData, noData](float value) {
        return std::isfinite(value) && !(hasNoData && value == static_cast<float>(noData));
    };
    float minimumElevation = std::numeric_limits<float>::infinity();
    for (int row = 0; row < rawDem.rows; ++row) {
        const float* values = rawDem.ptr<float>(row);
        for (int column = 0; column < rawDem.cols; ++column) {
            if (isValid(values[column])) minimumElevation = std::min(minimumElevation, values[column]);
        }
    }
    if (!std::isfinite(minimumElevation)) throw std::runtime_error("DEM contains no valid elevations: " + filename);
    for (int row = 0; row < rawDem.rows; ++row) {
        float* values = rawDem.ptr<float>(row);
        for (int column = 0; column < rawDem.cols; ++column) {
            if (!isValid(values[column])) values[column] = minimumElevation;
        }
    }

    // Resample the complete DEM extent onto the configured scene extent:
    // rows run north-south along sceneSize.x, columns east along sceneSize.y.
    // FIXED[低] 数值: 未校验 sceneSize.x/y 为有限正数（NaN/inf 时 ceil(...) 转 int 属未定义行为，cv::resize 可能抛异常或分配巨量内存）；已修复：creatBackgroundFromDEM 入口检查 std::isfinite 且 >0，不满足时抛出 std::runtime_error。
    const float requestedStep = std::max(targetStep, 0.01f);
    const float cappedStep = std::max(
        requestedStep,
        std::max(sceneSize.x, sceneSize.y) / 2048.0f);
    const int newCols = std::max(
        2, static_cast<int>(std::ceil(sceneSize.y / cappedStep)) + 1);
    const int newRows = std::max(
        2, static_cast<int>(std::ceil(sceneSize.x / cappedStep)) + 1);
    cv::resize(rawDem, m_heightmap, cv::Size(newCols, newRows),
               0, 0, cv::INTER_LINEAR);

    // 以 DEM 最低点作为场景零高程，确保地表和后续导入对象使用同一基准。
    m_heightmap = m_heightmap - minimumElevation;
    centerElevation = 0.0f;

    // 保存场景尺寸供查询函数使用
    m_sceneSizeCache = sceneSize;

    // 与上面生成的网格范围一致：局部 X∈[0,sceneSize.x] 对应北向，局部 Z∈[0,sceneSize.y] 对应东向。
    m_worldOrigin = { -sceneSize.x / 2.0f, 0.0f, -sceneSize.y / 2.0f };
    m_worldSize = sceneSize;

    // 3. 生成网格 (Triangles)
    // 采用“共享顶点”方式，即 (Rows+1)*(Cols+1) 个顶点，而不是面片汤
    m_vertices.clear();
    m_indices.clear();

    // 预分配内存优化性能
    m_vertices.reserve(newRows * newCols);

    // 生成顶点数据
    for (int r = 0; r < newRows; ++r) {
        for (int c = 0; c < newCols; ++c) {
            // 计算归一化坐标 [0, 1]
            float u = (float)c / (float)(newCols - 1);
            float v = (float)r / (float)(newRows - 1);

            // GeoTIFF row zero maps to north (+X); columns map east (+Z).
            float worldX = (0.5f - v) * sceneSize.x;
            float worldZ = (u - 0.5f) * sceneSize.y;

            // 获取高度 (已经是减去均值后的了)
            float worldY = m_heightmap.at<float>(r, c);

            nvmath::vec3f pos = { worldX, worldY, worldZ };

            // 存入顶点
            VertexAttribute va { pos };
            // 如果你的 VertexAttribute 支持法线，可以在这里或后续计算法线
            m_vertices.emplace_back(va);
        }
    }

    // 生成索引数据 (GL_TRIANGLES)
    for (int r = 0; r < newRows - 1; ++r) {
        for (int c = 0; c < newCols - 1; ++c) {
            // 当前格子的四个顶点索引
            uint32_t topLeft     = r * newCols + c;
            uint32_t topRight    = r * newCols + (c + 1);
            uint32_t bottomLeft  = (r + 1) * newCols + c;
            uint32_t bottomRight = (r + 1) * newCols + (c + 1);

            // 构建两个三角形形成一个方形网格
            // Tri 1
            m_indices.push_back(topLeft);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(topRight);

            // Tri 2
            m_indices.push_back(topRight);
            m_indices.push_back(bottomLeft);
            m_indices.push_back(bottomRight);
        }
    }

    m_objmesh.nVertices = (uint32_t)m_vertices.size();
    m_objmesh.nIndices  = (uint32_t)m_indices.size();
    m_objmesh.vertices  = m_vertices;
    m_objmesh.indices   = m_indices;
}


void ObjLoader::interpolateZValues(nvmath::vec3f sceneSize, float* tempx, float* tempy, float* tempz, int num_points)
{
    // 场景尺寸无效时无法做归一化，保留输入中的离地高度。
    if (!std::isfinite(sceneSize.x) || !std::isfinite(sceneSize.y) ||
        !(sceneSize.x > 0.0f) || !(sceneSize.y > 0.0f)) {
        std::cerr << "Error: invalid scene size for heightmap interpolation." << std::endl;
        return;
    }

    // 1. 安全检查
    if (m_heightmap.empty()) {
        std::cerr << "Error: Heightmap not initialized." << std::endl;
        for(int i=0; i<num_points; ++i) tempz[i] = 0.0f;
        return;
    }

    int width = m_heightmap.cols;
    int height = m_heightmap.rows;

    // 最大的合法索引 (浮点数表示)，用于防止越界
    float maxImgX = (float)(width - 1) - 0.0001f;
    float maxImgY = (float)(height - 1) - 0.0001f;

    for (int i = 0; i < num_points; i++) {
        float wx = tempx[i]; // 输入范围假设: [0, sceneSize.x]
        float wy = tempy[i]; // 输入范围假设: [0, sceneSize.y]

        // 2. 场景范围检查 (改为检查 0 到 Size)
        if (wx < 0.0f || wx > sceneSize.x || wy < 0.0f || wy > sceneSize.y) {
            // 超出 DEM 范围时保留用户设置的离地高度。
            continue;
        }

        // 3. 映射：世界坐标 [0, Size] -> 归一化 UV [0.0, 1.0]
        // 不需要 +0.5 了，因为输入本身就是从 0 开始的
        // Position-file columns are north and east respectively; the engine
        // axes are +X north, +Y up and +Z east.
        // FIXED[低] 数值: sceneSize.x 或 y 为 0 时 wx==0 仍通过范围检查，0/0 得 NaN，(int)NaN 属未定义行为并作为下标访问 m_heightmap；已修复：函数开头校验 sceneSize.x/y 有限且 >0，不满足时打印错误并直接返回，不再做归一化。
        float u = wy / sceneSize.y;
        float v = 1.0f - wx / sceneSize.x;

        // 转换为图像上的浮点坐标 [0, width-1]
        float imgX = u * (width - 1);
        float imgY = v * (height - 1);

        // 4. 坐标钳制 (Clamp)
        // 即使上面做了范围检查，为了防止浮点误差导致 imgX 略微超过 maxImgX，这里必须钳制
        if (imgX < 0.0f) imgX = 0.0f;
        if (imgX > maxImgX) imgX = maxImgX;

        if (imgY < 0.0f) imgY = 0.0f;
        if (imgY > maxImgY) imgY = maxImgY;

        // 5. 双线性插值计算
        int x0 = (int)imgX;
        int y0 = (int)imgY;

        // 确保 x1, y1 不越界
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        if (x1 >= width) x1 = width - 1;
        if (y1 >= height) y1 = height - 1;

        // 计算权重 (0.0 ~ 1.0)
        float dx = imgX - (float)x0;
        float dy = imgY - (float)y0;

        // 获取高度值 (注意: m_heightmap 已经是减去均值后的数据)
        float z00 = m_heightmap.at<float>(y0, x0);
        float z10 = m_heightmap.at<float>(y0, x1);
        float z01 = m_heightmap.at<float>(y1, x0);
        float z11 = m_heightmap.at<float>(y1, x1);

        // 插值
        // X 方向
        float zTop = z00 * (1.0f - dx) + z10 * dx;
        float zBot = z01 * (1.0f - dx) + z11 * dx;

        // Y 方向
        float zInterp = zTop * (1.0f - dy) + zBot * dy;

        // 文件中的 Z 是相对地表的离地高度；DEM 只提供该位置的地表高程。
        tempz[i] += zInterp;
    }
}
