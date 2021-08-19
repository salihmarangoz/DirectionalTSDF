// Copyright 2014-2017 Oxford University Innovation Limited and the authors of InfiniTAM

#include "ITMVisualisationEngine_CPU.h"

#include "../Shared/ITMVisualisationEngine_Shared.h"
#include "../../Reconstruction/Shared/ITMSceneReconstructionEngine_Shared.h"

#include <vector>

using namespace ITMLib;

static int RenderPointCloud(Vector4f* locations, Vector4f* colours, const Vector4f* ptsRay,
                            const Vector6f* directionalContribution,
                            const ITMVoxel* voxelData, const typename ITMVoxelIndex::IndexData* voxelIndex,
                            bool skipPoints,
                            float voxelSize,
                            Vector2i imgSize, Vector3f lightSource)
{
	int noTotalPoints = 0;

	for (int y = 0, locId = 0; y < imgSize.y; y++)
		for (int x = 0; x < imgSize.x; x++, locId++)
		{
			Vector3f outNormal;
			float angle;
			const Vector4f& pointRay = ptsRay[locId];
			const Vector3f& point = pointRay.toVector3();
			const Vector6f* directional = directionalContribution ? &directionalContribution[locId] : nullptr;
			bool foundPoint = pointRay.w > 0;

			computeNormalAndAngle<ITMVoxel, ITMVoxelIndex>(foundPoint, point, directional, voxelData, voxelIndex, lightSource,
			                                               outNormal, angle);

			if (skipPoints && ((x % 2 == 0) || (y % 2 == 0))) foundPoint = false;

			if (foundPoint)
			{
				Vector4f tmp(0, 0, 0, 0);
				if (directionalContribution)
				{
					for (TSDFDirection_type direction = 0; direction < N_DIRECTIONS; direction++)
					{
						tmp += directional->v[direction] *
						       VoxelColorReader<ITMVoxel::hasColorInformation, ITMVoxel, ITMVoxelIndex>::interpolate(voxelData,
						                                                                                             voxelIndex,
						                                                                                             point,
						                                                                                             ITMLib::TSDFDirection(
							                                                                                             direction));
					}
				} else
				{
					tmp = VoxelColorReader<ITMVoxel::hasColorInformation, ITMVoxel, ITMVoxelIndex>::interpolate(voxelData,
					                                                                                            voxelIndex,
					                                                                                            point,
					                                                                                            ITMLib::TSDFDirection::NONE);
				}
				if (tmp.w > 0.0f)
				{
					tmp.x /= tmp.w;
					tmp.y /= tmp.w;
					tmp.z /= tmp.w;
					tmp.w = 1.0f;
				}
				colours[noTotalPoints] = tmp;

				Vector4f pt_ray_out;
				pt_ray_out.x = point.x * voxelSize;
				pt_ray_out.y = point.y * voxelSize;
				pt_ray_out.z = point.z * voxelSize;
				pt_ray_out.w = 1.0f;
				locations[noTotalPoints] = pt_ray_out;

				noTotalPoints++;
			}
		}

	return noTotalPoints;
}

//ITMRenderState* ITMVisualisationEngine_CPU<ITMVoxel, ITMVoxelIndex>::CreateRenderState(const Scene *scene, const Vector2i & imgSize) const
//{
//	return new ITMRenderState(
//		imgSize, scene->sceneParams->viewFrustum_min, scene->sceneParams->viewFrustum_max, MEMORYDEVICE_CPU
//	);
//}

ITMRenderState_VH* ITMVisualisationEngine_CPU::CreateRenderState(const Scene* scene,
                                                                 const Vector2i& imgSize) const
{
	return new ITMRenderState_VH(
		ITMVoxelBlockHash::noTotalEntries, imgSize, scene->sceneParams->viewFrustum_min,
		scene->sceneParams->viewFrustum_max, MEMORYDEVICE_CPU
	);
}

int ITMVisualisationEngine_CPU::CountVisibleBlocks(const Scene* scene,
                                                   const ITMRenderState* renderState, int minBlockId,
                                                   int maxBlockId) const
{
	const ITMRenderState_VH* renderState_vh = (const ITMRenderState_VH*) renderState;

	int noVisibleEntries = renderState_vh->noVisibleEntries;
	const int* visibleEntryIDs = renderState_vh->GetVisibleEntryIDs();

	int ret = 0;
	for (int i = 0; i < noVisibleEntries; ++i)
	{
		int blockID = scene->index.GetEntries()[visibleEntryIDs[i]].ptr;
		if ((blockID >= minBlockId) && (blockID <= maxBlockId)) ++ret;
	}

	return ret;
}

void ITMVisualisationEngine_CPU::CreateExpectedDepths(const Scene* scene,
                                                      const ORUtils::SE3Pose* pose, const ITMIntrinsics* intrinsics,
                                                      ITMRenderState* renderState)
{
	Vector2i imgSize = renderState->renderingRangeImage->noDims;
	Vector2f* minmaxData = renderState->renderingRangeImage->GetData(MEMORYDEVICE_CPU);

	for (int locId = 0; locId < imgSize.x * imgSize.y; ++locId)
	{
		Vector2f& pixel = minmaxData[locId];
		pixel.x = FAR_AWAY;
		pixel.y = VERY_CLOSE;
	}

	float voxelSize = scene->sceneParams->voxelSize;

	std::vector<RenderingBlock> renderingBlocks(MAX_RENDERING_BLOCKS);
	int numRenderingBlocks = 0;

	ITMRenderState_VH* renderState_vh = (ITMRenderState_VH*) renderState;

	const int* visibleEntryIDs = renderState_vh->GetVisibleEntryIDs();
	int noVisibleEntries = renderState_vh->noVisibleEntries;

	//go through list of visible 8x8x8 blocks
	for (int blockNo = 0; blockNo < noVisibleEntries; ++blockNo)
	{
		const ITMHashEntry& blockData(scene->index.GetEntries()[visibleEntryIDs[blockNo]]);

		Vector2i upperLeft, lowerRight;
		Vector2f zRange;
		bool validProjection = false;
		if (blockData.ptr >= 0)
		{
			validProjection = ProjectSingleBlock(blockData.pos, pose->GetM(), intrinsics->projectionParamsSimple.all, imgSize,
			                                     voxelSize, upperLeft, lowerRight, zRange);
		}
		if (!validProjection) continue;

		Vector2i requiredRenderingBlocks(
			(int) ceilf((float) (lowerRight.x - upperLeft.x + 1) / (float) renderingBlockSizeX),
			(int) ceilf((float) (lowerRight.y - upperLeft.y + 1) / (float) renderingBlockSizeY));
		int requiredNumBlocks = requiredRenderingBlocks.x * requiredRenderingBlocks.y;

		if (numRenderingBlocks + requiredNumBlocks >= MAX_RENDERING_BLOCKS) continue;
		int offset = numRenderingBlocks;
		numRenderingBlocks += requiredNumBlocks;

		CreateRenderingBlocks(&(renderingBlocks[0]), offset, upperLeft, lowerRight, zRange);
	}

	// go through rendering blocks
	for (int blockNo = 0; blockNo < numRenderingBlocks; ++blockNo)
	{
		// fill minmaxData
		const RenderingBlock& b(renderingBlocks[blockNo]);

		for (int y = b.upperLeft.y; y <= b.lowerRight.y; ++y)
		{
			for (int x = b.upperLeft.x; x <= b.lowerRight.x; ++x)
			{
				Vector2f& pixel(minmaxData[x + y * imgSize.x]);
				if (pixel.x > b.zRange.x) pixel.x = b.zRange.x;
				if (pixel.y < b.zRange.y) pixel.y = b.zRange.y;
			}
		}
	}
}

void ITMVisualisationEngine_CPU::RenderImage(const Scene* scene,
                                             const ORUtils::SE3Pose* pose,
                                             const ITMIntrinsics* intrinsics,
                                             const ITMRenderState* renderState,
                                             ITMUChar4Image* outputImage,
                                             IITMVisualisationEngine::RenderImageType type,
                                             IITMVisualisationEngine::RenderRaycastSelection raycastType) const
{
	Vector2i imgSize = outputImage->noDims;
	Matrix4f invM = pose->GetInvM();

	bool useDirectial = this->settings->fusionParams.tsdfMode == TSDFMode::TSDFMODE_DIRECTIONAL;

	Vector4f* pointsRay, * normalsRay;
	if (raycastType == IITMVisualisationEngine::RENDER_FROM_OLD_RAYCAST)
		pointsRay = renderState->raycastResult->GetData(MEMORYDEVICE_CPU);
	else
	{
		if (raycastType == IITMVisualisationEngine::RENDER_FROM_OLD_FORWARDPROJ)
			pointsRay = renderState->forwardProjection->GetData(MEMORYDEVICE_CPU);
		else
		{
			// this one is generally done for freeview visualisation, so
			// no, do not update the list of visible blocks
			GenericRaycast(scene, imgSize, invM, intrinsics->projectionParamsSimple.all, renderState, false);
			pointsRay = renderState->raycastResult->GetData(MEMORYDEVICE_CPU);
		}
	}
	normalsRay = renderState->raycastNormals->GetData(MEMORYDEVICE_CPU);

	Vector3f lightSource = Vector3f(invM.getColumn(3)) / scene->sceneParams->voxelSize;
	Vector4u* outRendering = outputImage->GetData(MEMORYDEVICE_CPU);
	Vector6f* directionalContribution = renderState->raycastDirectionalContribution->GetData(MEMORYDEVICE_CPU);
	const ITMVoxel* voxelData = scene->localVBA.GetVoxelBlocks();
	const typename ITMVoxelIndex::IndexData* voxelIndex = scene->index.getIndexData();

	if ((type == IITMVisualisationEngine::RENDER_COLOUR_FROM_VOLUME) &&
	    (!ITMVoxel::hasColorInformation))
		type = IITMVisualisationEngine::RENDER_SHADED_GREYSCALE;

	switch (type)
	{
		case IITMVisualisationEngine::RENDER_COLOUR_FROM_VOLUME:
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
			for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
			{
				Vector4f ptRay = pointsRay[locId];
				processPixelColour<ITMVoxel, ITMVoxelIndex>(outRendering[locId], ptRay.toVector3(),
				                                            useDirectial ? &directionalContribution[locId] : nullptr,
				                                            ptRay.w > 0,
				                                            voxelData, voxelIndex, lightSource);
			}
			break;
		case IITMVisualisationEngine::RENDER_COLOUR_FROM_SDFNORMAL:
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
			for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
			{
				Vector4f ptRay = pointsRay[locId];
				processPixelNormal_SDFNormals<ITMVoxel, ITMVoxelIndex>(outRendering[locId], ptRay.toVector3(),
				                                                       useDirectial ? &directionalContribution[locId] : nullptr,
				                                                       ptRay.w > 0, voxelData, voxelIndex, lightSource);
			}
			break;
		case IITMVisualisationEngine::RENDER_COLOUR_FROM_IMAGENORMAL:
			if (intrinsics->FocalLengthSignsDiffer())
			{
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
				for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
				{
					int y = locId / imgSize.x, x = locId - y * imgSize.x;
					processPixelNormals_ImageNormals<true, true>(outRendering, pointsRay, normalsRay, imgSize, x, y,
					                                             scene->sceneParams->voxelSize, lightSource);
				}
			} else
			{
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
				for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
				{
					int y = locId / imgSize.x, x = locId - y * imgSize.x;
					processPixelNormals_ImageNormals<true, false>(outRendering, pointsRay, normalsRay, imgSize, x, y,
					                                              scene->sceneParams->voxelSize, lightSource);
				}
			}
			break;
		case IITMVisualisationEngine::RENDER_COLOUR_FROM_CONFIDENCE_SDFNORMAL:
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
			for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
			{
				Vector4f ptRay = pointsRay[locId];
				processPixelConfidence_SDFNormals<ITMVoxel, ITMVoxelIndex>(outRendering[locId], ptRay,
				                                                           useDirectial ? &directionalContribution[locId]
				                                                                        : nullptr,
				                                                           ptRay.w > 0, voxelData, voxelIndex,
				                                                           *(scene->sceneParams),
				                                                           lightSource);
			}
			break;
		case IITMVisualisationEngine::RENDER_COLOUR_FROM_CONFIDENCE_IMAGENORMAL:
			if (intrinsics->FocalLengthSignsDiffer())
			{
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
				for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
				{
					int y = locId / imgSize.x, x = locId - y * imgSize.x;
					processPixelConfidence_ImageNormals<true, true>(outRendering, pointsRay, normalsRay, imgSize, x, y,
					                                                *(scene->sceneParams), lightSource);
				}
			} else
			{
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
				for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
				{
					int y = locId / imgSize.x, x = locId - y * imgSize.x;
					processPixelConfidence_ImageNormals<true, false>(outRendering, pointsRay, normalsRay, imgSize, x, y,
					                                                 *(scene->sceneParams), lightSource);
				}
			}
			break;
		case IITMVisualisationEngine::RENDER_COLOUR_FROM_DEPTH:
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
			for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
			{
				processPixelDepth<ITMVoxel, ITMVoxelIndex>(outRendering[locId], pointsRay[locId].toVector3(),
				                                           pointsRay[locId].w > 0,
				                                           pose->GetM(), scene->sceneParams->voxelSize,
				                                           scene->sceneParams->viewFrustum_max);
			}
			break;
		case IITMVisualisationEngine::RENDER_SHADED_GREYSCALE_IMAGENORMALS:
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
			for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
			{
				int y = locId / imgSize.x;
				int x = locId - y * imgSize.x;

				if (intrinsics->FocalLengthSignsDiffer())
				{
					processPixelGrey_ImageNormals<true, true>(outRendering, pointsRay, normalsRay, imgSize, x, y,
					                                          scene->sceneParams->voxelSize, lightSource);
				} else
				{
					processPixelGrey_ImageNormals<true, false>(outRendering, pointsRay, normalsRay, imgSize, x, y,
					                                           scene->sceneParams->voxelSize, lightSource);
				}
			}
			break;
		case IITMVisualisationEngine::RENDER_SHADED_GREYSCALE:
		default:
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
			for (int locId = 0; locId < imgSize.x * imgSize.y; locId++)
			{
				Vector4f ptRay = pointsRay[locId];
				processPixelGrey_SDFNormals<ITMVoxel, ITMVoxelIndex>(outRendering[locId], ptRay.toVector3(),
				                                                     useDirectial ? &directionalContribution[locId] : nullptr,
				                                                     ptRay.w > 0, voxelData, voxelIndex, lightSource);
			}
	}
}

void ITMVisualisationEngine_CPU::CreatePointCloud(const Scene* scene,
                                                  const ITMView* view,
                                                  ITMTrackingState* trackingState,
                                                  ITMRenderState* renderState, bool skipPoints) const
{
	Vector2i imgSize = renderState->raycastResult->noDims;
	Matrix4f invM = trackingState->pose_d->GetInvM() * view->calib.trafo_rgb_to_depth.calib;

	bool useDirectioal = this->settings->fusionParams.tsdfMode == TSDFMode::TSDFMODE_DIRECTIONAL;

	// this one is generally done for the colour tracker, so yes, update
	// the list of visible blocks if possible
	GenericRaycast(scene, imgSize, invM, view->calib.intrinsics_rgb.projectionParamsSimple.all, renderState, true);
	trackingState->pose_pointCloud->SetFrom(trackingState->pose_d);

	trackingState->pointCloud->noTotalPoints = RenderPointCloud(
		trackingState->pointCloud->locations->GetData(MEMORYDEVICE_CPU),
		trackingState->pointCloud->colours->GetData(MEMORYDEVICE_CPU),
		renderState->raycastResult->GetData(MEMORYDEVICE_CPU),
		useDirectioal ? renderState->raycastDirectionalContribution->GetData(MEMORYDEVICE_CPU) : nullptr,
		scene->localVBA.GetVoxelBlocks(),
		scene->index.getIndexData(),
		skipPoints,
		scene->sceneParams->voxelSize,
		imgSize,
		-Vector3f(invM.getColumn(2))
	);
}


void ITMVisualisationEngine_CPU::CreateICPMaps(const Scene* scene,
                                               const ITMView* view,
                                               ITMTrackingState* trackingState,
                                               ITMRenderState* renderState) const
{
	Vector2i imgSize = renderState->raycastResult->noDims;
	Matrix4f invM = trackingState->pose_d->GetInvM();

	// this one is generally done for the ICP tracker, so yes, update
	// the list of visible blocks if possible
	GenericRaycast(scene, imgSize, invM, view->calib.intrinsics_d.projectionParamsSimple.all, renderState, true);
	trackingState->pose_pointCloud->SetFrom(trackingState->pose_d);

	Vector3f lightSource = Vector3f(invM.getColumn(3)) / scene->sceneParams->voxelSize;
	Vector4f* normalsMap = trackingState->pointCloud->colours->GetData(MEMORYDEVICE_CPU);
	Vector4f* pointsMap = trackingState->pointCloud->locations->GetData(MEMORYDEVICE_CPU);
	Vector4f* pointsRay = renderState->raycastResult->GetData(MEMORYDEVICE_CPU);
	Vector4f* normalsRay = renderState->raycastNormals->GetData(MEMORYDEVICE_CPU);
	float voxelSize = scene->sceneParams->voxelSize;

#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
	for (int y = 0; y < imgSize.y; y++)
		for (int x = 0; x < imgSize.x; x++)
		{
			if (view->calib.intrinsics_d.FocalLengthSignsDiffer())
			{
				processPixelICP<true, true>(pointsMap, normalsMap, pointsRay, normalsRay, imgSize, x, y, voxelSize,
				                            lightSource);
			} else
			{
				processPixelICP<true, false>(pointsMap, normalsMap, pointsRay, normalsRay, imgSize, x, y, voxelSize,
				                             lightSource);
			}

		}
}

void ITMVisualisationEngine_CPU::ForwardRender(const Scene* scene,
                                               const ITMView* view,
                                               ITMTrackingState* trackingState,
                                               ITMRenderState* renderState) const
{
	Vector2i imgSize = renderState->raycastResult->noDims;
	Matrix4f M = trackingState->pose_d->GetM();
	Matrix4f invM = trackingState->pose_d->GetInvM();
	const Vector4f& projParams = view->calib.intrinsics_d.projectionParamsSimple.all;

	const Vector4f* pointsRay = renderState->raycastResult->GetData(MEMORYDEVICE_CPU);
	Vector4f* forwardProjection = renderState->forwardProjection->GetData(MEMORYDEVICE_CPU);
	float* currentDepth = view->depth->GetData(MEMORYDEVICE_CPU);
	int* fwdProjMissingPoints = renderState->fwdProjMissingPoints->GetData(MEMORYDEVICE_CPU);
	const Vector2f* minmaximg = renderState->renderingRangeImage->GetData(MEMORYDEVICE_CPU);
	float voxelSize = scene->sceneParams->voxelSize;
	const ITMVoxel* voxelData = scene->localVBA.GetVoxelBlocks();
	const typename ITMVoxelIndex::IndexData* voxelIndex = scene->index.getIndexData();

	renderState->forwardProjection->Clear();

	for (int y = 0; y < imgSize.y; y++)
		for (int x = 0; x < imgSize.x; x++)
		{
			int locId = x + y * imgSize.x;
			Vector4f pixel = pointsRay[locId];

			int locId_new = forwardProjectPixel(pixel * voxelSize, M, projParams, imgSize);
			if (locId_new >= 0) forwardProjection[locId_new] = pixel;
		}

	int noMissingPoints = 0;
	for (int y = 0; y < imgSize.y; y++)
		for (int x = 0; x < imgSize.x; x++)
		{
			int locId = x + y * imgSize.x;
			int locId2 =
				(int) floor((float) x / minmaximg_subsample) + (int) floor((float) y / minmaximg_subsample) * imgSize.x;

			Vector4f fwdPoint = forwardProjection[locId];
			Vector2f minmaxval = minmaximg[locId2];
			float depth = currentDepth[locId];

			if ((fwdPoint.w <= 0) && ((fwdPoint.x == 0 && fwdPoint.y == 0 && fwdPoint.z == 0) || (depth >= 0)) &&
			    (minmaxval.x < minmaxval.y))
				//if ((fwdPoint.w <= 0) && (minmaxval.x < minmaxval.y))
			{
				fwdProjMissingPoints[noMissingPoints] = locId;
				noMissingPoints++;
			}
		}

	renderState->noFwdProjMissingPoints = noMissingPoints;
	const Vector4f invProjParams = invertProjectionParams(projParams);

	for (int pointId = 0; pointId < noMissingPoints; pointId++)
	{
		int locId = fwdProjMissingPoints[pointId];
		int y = locId / imgSize.x, x = locId - y * imgSize.x;
		int locId2 =
			(int) floor((float) x / minmaximg_subsample) + (int) floor((float) y / minmaximg_subsample) * imgSize.x;

		castRay<ITMIndexDirectional, ITMVoxel>(forwardProjection[locId], nullptr, x, y,
		                                       ((TSDF_CPU<ITMIndexDirectional, ITMVoxel>*) scene->tsdf)->getMap(),
		                                       invM, invProjParams,
		                                       *(scene->sceneParams), minmaximg[locId2],
		                                 this->settings->fusionParams.tsdfMode == TSDFMode::TSDFMODE_DIRECTIONAL
		);
	}
}

void ITMVisualisationEngine_CPU::GenericRaycast(const Scene* scene,
                                                const Vector2i& imgSize, const Matrix4f& invM,
                                                const Vector4f& projParams,
                                                const ITMRenderState* renderState,
                                                bool updateVisibleList) const
{
	const Vector2f* minmaximg = renderState->renderingRangeImage->GetData(MEMORYDEVICE_CPU);
	Vector4f* pointsRay = renderState->raycastResult->GetData(MEMORYDEVICE_CPU);
	Vector4f* normalsRay = renderState->raycastNormals->GetData(MEMORYDEVICE_CPU);
	Vector6f* directionalContribution = renderState->raycastDirectionalContribution->GetData(MEMORYDEVICE_CPU);
	const ITMVoxel* voxelData = scene->localVBA.GetVoxelBlocks();
	const typename ITMVoxelBlockHash::IndexData* voxelIndex = scene->index.getIndexData();
	HashEntryVisibilityType* entriesVisibleType = NULL;
	if (updateVisibleList && (dynamic_cast<const ITMRenderState_VH*>(renderState) != NULL))
	{
		entriesVisibleType = ((ITMRenderState_VH*) renderState)->GetEntriesVisibleType();
	}

	Vector4f invProjParams = invertProjectionParams(projParams);
#ifdef WITH_OPENMP
#pragma omp parallel for
#endif
	if (this->settings->fusionParams.tsdfMode == TSDFMode::TSDFMODE_DIRECTIONAL and DIRECTIONAL_RENDERING_MODE == 1)
	{
		InputPointClouds pointClouds;
		for (TSDFDirection_type directionIdx = 0; directionIdx < N_DIRECTIONS; directionIdx++)
		{
			pointClouds.pointCloud[directionIdx] = renderState->raycastResultDirectional[directionIdx]->GetData(
				MEMORYDEVICE_CPU);
			for (int locId = 0; locId < imgSize.x * imgSize.y; ++locId)
			{
				int y = locId / imgSize.x;
				int x = locId - y * imgSize.x;
				int locId2 =
					(int) floor((float) x / minmaximg_subsample) + (int) floor((float) y / minmaximg_subsample) * imgSize.x;

				float distance;
				castRayDefault<ITMVoxel, ITMVoxelIndex>(
					pointClouds.pointCloud[directionIdx][locId],
					distance,
					entriesVisibleType,
					x, y,
					voxelData,
					voxelIndex,
					invM,
					invProjParams,
					*(scene->sceneParams),
					minmaximg[locId2],
					TSDFDirection(directionIdx)
				);
			}

			pointClouds.pointCloudNormals[directionIdx] = renderState->raycastNormalsDirectional[directionIdx]->GetData(
				MEMORYDEVICE_CPU);
			Vector4f* normals = pointClouds.pointCloudNormals[directionIdx];
			for (int locId = 0; locId < imgSize.x * imgSize.y; ++locId)
			{
				int y = locId / imgSize.x;
				int x = locId - y * imgSize.x;

				bool foundPoint = true;
				Vector3f normal;
				computeNormal<false, false>(pointClouds.pointCloud[directionIdx], scene->sceneParams->voxelSize, imgSize, x, y,
				                            foundPoint, normal);

				if (not foundPoint)
				{
					normals[x + y * imgSize.x] = Vector4f(0, 0, 0, -1);
					continue;
				}

				normals[x + y * imgSize.x] = Vector4f(normal, 1);
			}
		}

		for (int locId = 0; locId < imgSize.x * imgSize.y; ++locId)
		{
			int y = locId / imgSize.x;
			int x = locId - y * imgSize.x;

			combineDirectionalPointClouds<true, false>(pointsRay, normalsRay, pointClouds, directionalContribution, imgSize,
			                                           invM, invProjParams, x, y, scene->sceneParams->voxelSize);
		}
	} else
	{
		for (int locId = 0; locId < imgSize.x * imgSize.y; ++locId)
		{
			int y = locId / imgSize.x;
			int x = locId - y * imgSize.x;
			int locId2 =
				(int) floor((float) x / minmaximg_subsample) + (int) floor((float) y / minmaximg_subsample) * imgSize.x;

				castRay<ITMIndexDirectional, ITMVoxel>(
				pointsRay[locId],
				&directionalContribution[locId],
				x, y,
				((TSDF_CPU<ITMIndexDirectional, ITMVoxel>*) scene->tsdf)->getMap(),
				invM,
				invProjParams,
				*(scene->sceneParams),
				minmaximg[locId2],
				this->settings->fusionParams.tsdfMode == TSDFMode::TSDFMODE_DIRECTIONAL
			);
		}

		Vector4f* normals = renderState->raycastNormals->GetData(MEMORYDEVICE_CPU);
		for (int locId = 0; locId < imgSize.x * imgSize.y; ++locId)
		{
			int y = locId / imgSize.x;
			int x = locId - y * imgSize.x;

			bool foundPoint = true;
			Vector3f normal;
			computeNormal<false, false>(pointsRay, scene->sceneParams->voxelSize, imgSize, x, y, foundPoint, normal);

			if (not foundPoint)
			{
				normals[x + y * imgSize.x] = Vector4f(0, 0, 0, -1);
				continue;
			}

			normals[x + y * imgSize.x] = Vector4f(normal, 1);
		}
	}
}

void ITMVisualisationEngine_CPU::FindSurface(const Scene* scene,
                                             const ORUtils::SE3Pose* pose,
                                             const ITMIntrinsics* intrinsics,
                                             const ITMRenderState* renderState) const
{
	GenericRaycast(scene, renderState->raycastResult->noDims, pose->GetInvM(), intrinsics->projectionParamsSimple.all,
	               renderState, false);
}

void ITMVisualisationEngine_CPU::RenderTrackingError(ITMUChar4Image* outRendering,
                                                     const ITMTrackingState* trackingState,
                                                     const ITMView* view) const
{
	Vector4u* data = outRendering->GetData(MEMORYDEVICE_CPU);
	const Vector4f* pointsRay = trackingState->pointCloud->locations->GetData(MEMORYDEVICE_CPU);
	const Vector4f* normalsRay = trackingState->pointCloud->colours->GetData(MEMORYDEVICE_CPU);
	const float* depthImage = view->depth->GetData(MEMORYDEVICE_CPU);
	const Matrix4f& depthImagePose = trackingState->pose_d->GetInvM();
	const Matrix4f& sceneRenderingPose = trackingState->pose_pointCloud->GetInvM();
	Vector2i imgSize = view->calib.intrinsics_d.imgSize;
	const float maxError = this->settings->sceneParams.mu;

	for (int y = 0; y < view->calib.intrinsics_d.imgSize.height; y++)
		for (int x = 0; x < view->calib.intrinsics_d.imgSize.width; x++)
		{
			processPixelError(data, pointsRay, normalsRay, depthImage, depthImagePose, sceneRenderingPose,
			                  view->calib.intrinsics_d.projectionParamsSimple.all, imgSize, maxError, x, y);
		}
}

void ITMVisualisationEngine_CPU::ComputeRenderingTSDF(const Scene* scene, const ORUtils::SE3Pose* pose,
                                                      const ITMIntrinsics* intrinsics,
                                                      ITMRenderState* renderState)
{

}