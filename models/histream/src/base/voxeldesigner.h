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

class VoxelDesigner
{
public:
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

    float m_scale;

	struct Ellipsoid 
	{
        std::vector<nvmath::vec3i> minPos;
        int activeNum{0};
    };
};
