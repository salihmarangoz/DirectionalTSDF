// Copyright 2014-2017 Oxford University Innovation Limited and the authors of InfiniTAM

#pragma once

#include <ITMLib/ITMLibDefines.h>
#include <ITMLib/Utils/ITMMath.h>
#include <ORUtils/Image.h>

namespace ITMLib
{
/** \brief
	Stores the render state used by the SceneReconstruction
	and Visualisation engines.
	*/
class ITMRenderState
{
private:
	MemoryDeviceType memoryType;

	ORUtils::MemoryBlock<ITMIndex>* visibleBlocks = nullptr;

public:
	/** @brief
	Gives the raycasting operations an idea of the
	depth range to cover

	Each pixel contains an expected minimum and maximum
	depth. The raycasting step would use this
	information to reduce the range for searching an
	intersection with the actual surface. Should be
	updated by a ITMLib::Engine::ITMVisualisationEngine
	before any raycasting operation.
	*/
	ORUtils::Image<Vector2f>* renderingRangeImage = nullptr;

	/** @brief
	Float rendering output of the scene, containing the 3D
	locations in the world generated by the raycast.

	This is typically created as a by-product of
	raycasting operations.
	*/
	ORUtils::Image<Vector4f>* raycastResult = nullptr;

	ORUtils::Image<Vector4f>* raycastNormals = nullptr;

	ORUtils::Image<Vector4f>* forwardProjection = nullptr;
	ORUtils::Image<int>* fwdProjMissingPoints = nullptr;
	int noFwdProjMissingPoints = 0;

	ORUtils::Image<Vector4u>* renderedImage = nullptr;

	int noVisibleEntries = 0;

	ITMRenderState(const Vector2i& imgSize, float vf_min, float vf_max, MemoryDeviceType memoryType)
		: memoryType(memoryType)
	{
		renderingRangeImage = new ORUtils::Image<Vector2f>(imgSize, true, true);
		raycastResult = new ORUtils::Image<Vector4f>(imgSize, memoryType);
		raycastNormals = new ORUtils::Image<Vector4f>(imgSize, memoryType);
		forwardProjection = new ORUtils::Image<Vector4f>(imgSize, memoryType);
		fwdProjMissingPoints = new ORUtils::Image<int>(imgSize, memoryType);
		renderedImage = new ORUtils::Image<Vector4u>(imgSize, memoryType);

		ORUtils::Image<Vector2f>* buffImage = new ORUtils::Image<Vector2f>(imgSize, MEMORYDEVICE_CPU);

		Vector2f v_lims(vf_min, vf_max);
		for (int i = 0; i < imgSize.x * imgSize.y; i++) buffImage->GetData(MEMORYDEVICE_CPU)[i] = v_lims;

		if (memoryType == MEMORYDEVICE_CUDA)
		{
#ifndef COMPILE_WITHOUT_CUDA
			renderingRangeImage->SetFrom(buffImage, ORUtils::CPU_TO_CUDA);
#endif
		} else renderingRangeImage->SetFrom(buffImage, ORUtils::CPU_TO_CPU);

		delete buffImage;

		noFwdProjMissingPoints = 0;

		visibleBlocks = new ORUtils::MemoryBlock<ITMIndex>(10000, memoryType);
	}

	virtual ~ITMRenderState()
	{
		delete renderingRangeImage;
		delete raycastResult;
		delete raycastNormals;
		delete forwardProjection;
		delete fwdProjMissingPoints;
		delete renderedImage;

		delete visibleBlocks;
	}

	void Resize(size_t newSize)
	{
		if (newSize > visibleBlocks->dataSize)
		{
			noVisibleEntries = 0;
			visibleBlocks->Resize(newSize * 2);
		}
	}

	[[nodiscard]] size_t AllocatedSize() const
	{
		return visibleBlocks->dataSize;
	}

	ITMIndex* GetVisibleBlocks(void)
	{ return visibleBlocks->GetData(memoryType); }
};

}