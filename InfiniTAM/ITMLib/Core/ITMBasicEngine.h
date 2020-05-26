// Copyright 2014-2017 Oxford University Innovation Limited and the authors of InfiniTAM

#pragma once

#include "ITMDenseMapper.h"
#include "ITMMainEngine.h"
#include "ITMTrackingController.h"
#include "../Engines/LowLevel/Interface/ITMLowLevelEngine.h"
#include "../Engines/Meshing/Interface/ITMMeshingEngine.h"
#include "../Engines/ViewBuilding/Interface/ITMViewBuilder.h"
#include "../Engines/Visualisation/Interface/ITMVisualisationEngine.h"
#include "../Objects/Misc/ITMIMUCalibrator.h"

#include "../../FernRelocLib/Relocaliser.h"

namespace ITMLib
{
	template <typename TVoxel, typename TIndex>
	class ITMBasicEngine : public ITMMainEngine
	{
	private:
		std::shared_ptr<const ITMLibSettings> settings;

		bool trackingActive, fusionActive, mainProcessingActive, trackingInitialised;
		int framesProcessed, consecutiveGoodFrames, relocalisationCount;

		ITMLowLevelEngine *lowLevelEngine;
		ITMVisualisationEngine<TVoxel, TIndex> *visualisationEngine;

		ITMMeshingEngine<TVoxel, TIndex> *meshingEngine;

		ITMViewBuilder *viewBuilder;
		ITMDenseMapper<TVoxel, TIndex> *denseMapper;
		ITMTrackingController *trackingController;

		ITMScene<TVoxel, TIndex> *scene;
		ITMRenderState *renderState_live;
		ITMRenderState *renderState_freeview;

		ITMTracker *tracker;
		ITMIMUCalibrator *imuCalibrator;

		FernRelocLib::Relocaliser<float> *relocaliser;
		ITMUChar4Image *kfRaycast;

		/// Pointer for storing the current input frame
		ITMView *view;

		/// Pointer to the current camera pose and additional tracking information
		ITMTrackingState *trackingState;

	public:
		ITMView* GetView() override { return view; }
		ITMTrackingState* GetTrackingState() override { return trackingState; }

		virtual const unsigned int* GetAllocationsPerDirection() override
		{ return scene->localVBA.noAllocationsPerDirection; }

		/// Gives access to the internal world representation
		ITMScene<TVoxel, TIndex>* GetScene() { return scene; }

		ITMTrackingState::TrackingResult ProcessFrame(ITMUChar4Image *rgbImage, ITMShortImage *rawDepthImage, ITMIMUMeasurement *imuMeasurement = nullptr, const ORUtils::SE3Pose* pose = nullptr) override;

		/// Extracts a mesh from the current scene and saves it to the model file specified by the file name
		void SaveSceneToMesh(const char *fileName) override;

		/// save and load the full scene and relocaliser (if any) to/from file
		void SaveToFile() override;
		void LoadFromFile() override;

		/// Get a result image as output
		Vector2i GetImageSize() const override;

		void GetImage(ITMUChar4Image *out, GetImageType getImageType, ORUtils::SE3Pose *pose = NULL, ITMIntrinsics *intrinsics = NULL) override;

		/// switch for turning tracking on/off
		void turnOnTracking();
		void turnOffTracking();

		/// switch for turning integration on/off
		void turnOnIntegration();
		void turnOffIntegration();

		/// switch for turning main processing on/off
		void turnOnMainProcessing();
		void turnOffMainProcessing();

		/// resets the scene and the tracker
		void resetAll();

		/** \brief Constructor
			Omitting a separate image size for the depth images
			will assume same resolution as for the RGB images.
		*/
		ITMBasicEngine(const std::shared_ptr<const ITMLibSettings>& settings, const ITMRGBDCalib& calib, Vector2i imgSize_rgb, Vector2i imgSize_d = Vector2i(-1, -1));
		~ITMBasicEngine();
	};
}
