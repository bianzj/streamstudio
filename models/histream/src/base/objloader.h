// ==========================================================================
// 文件：objloader.h
// 功能：ObjLoader 类声明：加载 OBJ 网格、生成背景平面/DEM 地形，输出统一的 ObjMesh。
// 主要内容：
//   1. ShapeInfo：OBJ 中每个 shape 的偏移、面数、名称与材质下标
//   2. ObjLoader 公共接口：loadModel / loadMesh / createBackground / createGridBackground /
//      creatBackgroundFromDEM / getHeightAt / interpolateZValues
//   3. 结果成员 m_objmesh、minElevation、centerElevation；私有的缓冲与 DEM 高程图状态
// 数据流：scene.cpp 创建 ObjLoader 并调用加载/生成函数 → 读取 m_objmesh 作为场景网格；
//   使用 DEM 时再用 interpolateZValues 把离地高度换算为 y 坐标。
// 与其他文件：实现见 objloader.cpp；ObjMesh / VertexAttribute 定义在 structs.h；
//   依赖 tiny_obj_loader、GDAL、OpenCV 与 nvmath。
// 约定：+X 北、+Y 向上、+Z 东；DEM 地形以场景中心为原点。
// ==========================================================================

/******************************************************************************
 * Copyright 1998-2018 NVIDIA Corp. All Rights Reserved.
 *****************************************************************************/

#pragma once
#ifdef USE_NEW_NVPRO_CORE
//#include "libInterpolate/Interpolate.hpp"
#else
#include "Interpolate.hpp"
#endif
#include "thirdparty/tiny_obj_loader.h"
#include "structs.h"
#include "nvmath/nvmath.h"
#include <array>
#include <iostream>
#include <unordered_map>
#include <vector>
// #include "libInterpolate/Interpolate.hpp"
#include "gdal.h"
#include "gdal_priv.h"
#include "opencv2/opencv.hpp"


// OBJ 中一个 shape（o/g 分组）的记录：offset = 其首个索引在 m_indices 中的位置，
// nbFacet = 面数，matIndex = 首个面的材质 id（无则 -1）。只被写入私有成员 m_shapeInfo，
// 当前没有读取处。
struct ShapeInfo
{
	uint32_t offset;
	uint32_t nbFacet;
	std::string name;
	int matIndex;
};

// 类：ObjLoader —— 每次调用一种 load / create 函数，结果放在公开成员 m_objmesh 中。
class ObjLoader
{
public:
	void loadModel(const std::string& filename);
	void loadMesh(const std::string& filename, const std::string& meshname,
	              bool exactMatch = false);
	void createBackground(glm::vec3);
	// Flat ground as a grid with about `step` spacing, so per-triangle
	// physical-texture factors can vary across it.
	void createGridBackground(glm::vec3 bgSize, float step);
	void creatBackgroundFromDEM(const std::string& filename, nvmath::vec3f sceneSize,
	                             float targetStep);

    float getHeightAt(float locX, float locY);
	void interpolateZValues(nvmath::vec3f sceneSize,  // 场景范围 (x_size, y_size, z_size)
		float* tempx, float* tempy, float* tempz,  // 输入坐标序列
		int num_points  // 坐标数量
		);
//
//	void createBackgroundFromDEM(const std::string& filename, nvmath::vec3f sceneSize_XYZ, _2D::ThinPlateSplineInterpolator<double>& interp);
//	void createBackgroundFromDEM(const std::string& filename, nvmath::vec3f sceneSize_XYZ, _2D::BilinearInterpolator<double>& interp);//�������ǰ���ԭ����DEM���ɣ�����������ʱ�볡����С��һ�£�
//    void createBackgroundFromResizedDEM(const std::string &filename, nvmath::vec3f sceneSize_XYZ, _2D::BilinearInterpolator<double> &interp);//�Ƚ�DEM��ֵΪ������С��������
//	double getShiftInterp(_2D::BilinearInterpolator<double> interp, nvmath::vec3f shift0);

	// 最近一次加载/生成的网格（顶点、索引及其数量）。
	ObjMesh m_objmesh;

	// minElevation：loadModel 统计的最低 y；createBackground / createGridBackground 置 0；
	//   DEM 路径不更新它（待核对）。centerElevation：DEM 高程参考偏移，现行 DEM 实现中恒为 0。
	float minElevation{ 0 };
	float centerElevation{ 0 };

private:
	std::vector<VertexAttribute> m_vertices;
	std::vector<uint32_t> m_indices;
    void clearCurrentInfo();

	std::vector<ShapeInfo> m_shapeInfo;

	float m_scale{1};
    float m_step{5};
    int nImgSizeX{1};
    int nImgSizeY{1};

    cv::Mat originDEM;
	// DEM 状态：m_heightmap 为已重采样并减去最低点的高程（行 = 北向 +X，列 = 东向 +Z）；
	// m_worldOrigin 为地形角点的世界坐标，m_worldSize 的 x / y 分量为北向 / 东向长度。
	cv::Mat m_heightmap; // 存储 resize 后的高程数据

	nvmath::vec3f m_worldOrigin; // 地形左上角的世界坐标
	nvmath::vec3f m_worldSize;     // 地形的世界尺寸

	nvmath::vec3f m_sceneSizeCache; // 缓存场景大小，用于坐标逆映射
};
