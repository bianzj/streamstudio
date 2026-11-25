// ==========================================================================
// 文件：voxeldesigner.h
// 功能：VoxelDesigner 的声明——把参数化形状（立方体/椭球/背景平面）或高度栅格（tif）
//       离散成体元坐标，并生成对应的三角网格（PrimMesh），供体元类模式建立场景使用。
// 主要模块：
//   1. 体元离散：createCube / createEllipsoid / createBackground，返回整数体元坐标 ivec3
//      （createCube / createEllipsoid 同时填充 m_voxelModel 的 centerPoints 与 aabbs）
//   2. 体元→三角网格：createTriVoxels / createTriCube / createTriEllipsoid /
//      createTriBackground / createTriEntity 以及屋顶、墙体、tif 高度图版本
//   3. 网格导出：outputPrimMesh
// 数据流：Shape（structs.h）+ stepSize → 本类离散 → 体元坐标 / PrimMesh → scene.cpp 中的
//         场景构建，再进入 BLAS/TLAS 建立
// 关系：实现在 voxeldesigner.cpp；PrimMesh / Shape / VoxelModel 定义于 structs.h；
//       体元边界面片提取由 voxel_boundary.h 提供；调用方主要是 scene.cpp。
// 约定：体元坐标为整数 (i, j, k)，单位为 1 个体元（stepSize 米）；下标 (i, j, k) 对应
//       x（length）、y（width）、z（height）方向；体元 (i,j,k) 的 AABB 为 [coord, coord+1]。
// ==========================================================================

#pragma once

#include <vulkan/vulkan.hpp>
#include <span>
#include "nvvk/context_vk.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/descriptorsets_vk.hpp"
#include "nvvk/debug_util_vk.hpp"
#include "nvvk/shaders_vk.hpp"
#include "nvvk/buffers_vk.hpp"

#include "nvvk/resourceallocator_vk.hpp"
#include  "nvvk/pipeline_vk.hpp"
#include "nvvk/shadermodulemanager_vk.hpp"
//#include "objloader.h"
#include "src/base/queue.h"
#include <nvmath/nvmath.h>
#include "src/base/structs.h"
#include "gdal.h"
#include "gdal_priv.h"
#include "src/base/utils.h"
#include <cmath>

///---------------------------------------------------------
/// In this class, all object is XYZ (5,5,0)
///---------------------------------------------------------

// 类：VoxelDesigner——参数化形状/高度栅格 → 体元坐标与三角网格的生成器。
// 说明：m_voxelModel 保存最近一次 createCube / createEllipsoid 的结果（体元坐标与 AABB）。
class VoxelDesigner
{
public:
    // 作用：按 shapeType 选择体元化函数（CUBE/ELLIPSOID/PLANE，其余按 CUBE 处理），
    //       结果写入 m_voxelModel；stepSize 为体元边长（米）。
    void loadModel(ShapeType shapeType, Shape shape, float stepSize);
    
	VoxelModel m_voxelModel;

	std::vector<glm::ivec3> createBackground(float sceneLength, float sceneWidth, float stepSize);

	PrimMesh createTriBackground(float sceneLength, float sceneWidth, float stepSize);
    PrimMesh createTriEntity(Shape shape, float stepSize);
    PrimMesh createTriVoxels(const std::vector<glm::ivec3>& voxelIds, bool solidBoundary = false);

//    VoxelTriModel createTriEntity_rotate(ShapeType shapeType, Shape shape,
//                                         float stepSize, float rotateAngle,std::vector<glm::ivec3> voxelIds);
	//VoxelTriModel createTriBackgroundFromDEM(const std::string &filename, nvmath::vec3f sceneSize_XYZ,
 //                                            _2D::BilinearInterpolator<double> &interp);

    PrimMesh createTriEntitiesFromTif(std::string heightPath, float stepSize);

    PrimMesh createTriEntitiesFromTif_roof(std::string heightPath, glm::vec3 targetSize, float heightStep);
    PrimMesh createTriEntitiesFromTif_wall(std::string heightPath, glm::vec3 targetSize, float heightStep);
    PrimMesh createTriCube_wall(Shape shape, float stepSize);
    PrimMesh createTriCube_roof(Shape shape, float stepSize);

	bool outputPrimMesh(std::string path, PrimMesh mesh);

    // minElevation：最低高程（体元单位）；scene.cpp 用它计算背景平移 bgShift，但本类现役代码
    // 从不赋值（只在 voxeldesigner.cpp 已注释的 DEM 代码里赋值），故恒为 0（待核对）。
    float minElevation{0};

private:
	// output: centerPoints
    std::vector<glm::ivec3> createEllipsoid(float height, float width, float stepSize);

	std::vector<glm::ivec3> createCube(float length, float width, float height, float stepSize);
	
	PrimMesh createTriCube(Shape shape, float stepSize);

    PrimMesh createTriEllipsoid(Shape shape, float stepSize);

	bool isInPloy(nvmath::vec2i testPoint, std::vector<nvmath::vec2f> ploys);


//	Mesh createTriCube_rotate(Shape shape, float stepSize,
//                                       float rotateAngle,,std::vector<glm::ivec3> voxelIds);
	/*VoxelPrimitive createCubePrimitive(Cube cube);

	VoxelPrimitive createBackgroundPrimitive(Background background);

	VoxelPrimitive createEllipsoidPrimitive(Ellipsoid ellipsoid);*/
    //std::vector<nvmath::vec3i> m_centerPoints;

    // m_scale：同上，现役代码未使用（仅出现在已注释掉的 DEM 背景代码中）。
    float m_scale;

	struct Ellipsoid 
	{
        std::vector<nvmath::vec3i> minPos;
        int activeNum{0};
    };
};
