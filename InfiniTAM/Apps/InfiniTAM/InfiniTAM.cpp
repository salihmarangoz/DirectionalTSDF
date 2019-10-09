// Copyright 2014-2017 Oxford University Innovation Limited and the authors of InfiniTAM

#include <cstdlib>
#include <iostream>
#include <experimental/filesystem>

#include "UIEngine.h"

#include "InputSource/OpenNIEngine.h"
#include "InputSource/Kinect2Engine.h"
#include "InputSource/LibUVCEngine.h"
#include "InputSource/PicoFlexxEngine.h"
#include "InputSource/RealSenseEngine.h"
#include "InputSource/LibUVCEngine.h"
#include "InputSource/RealSense2Engine.h"
#include "InputSource/FFMPEGReader.h"
#include "ITMLib/ITMLibDefines.h"
#include "ITMLib/Core/ITMBasicEngine.h"
#include "ITMLib/Core/ITMBasicSurfelEngine.h"
#include "ITMLib/Core/ITMMultiEngine.h"
#include "ITMLib/Utils/ITMStringUtils.h"
#include "CLI/CLI11.hpp"

#include "Apps/Utils/CLIUtils.h"

using namespace InfiniTAM::Engine;
using namespace InputSource;
using namespace ITMLib;
namespace fs = std::experimental::filesystem;

int main(int argc, char** argv)
{
	AppData appData;
	int ret = ParseCLIOptions(argc, argv, appData);
	if (ret != 0)
		return ret;

	ImageSourceEngine* imageSource = appData.imageSource;
	IMUSourceEngine* imuSource = appData.imuSource;
	std::shared_ptr<ITMLibSettings> internalSettings = appData.internalSettings;

	if (not imageSource)
	{
		std::cout << "failed to open any image stream" << std::endl;
		return -1;
	}

	ITMMainEngine* mainEngine = nullptr;
	switch (internalSettings->libMode)
	{
		case ITMLibSettings::LIBMODE_BASIC:
			mainEngine = new ITMBasicEngine<ITMVoxel, ITMVoxelIndex>(internalSettings, imageSource->getCalib(),
			                                                         imageSource->getRGBImageSize(),
			                                                         imageSource->getDepthImageSize());
			break;
		case ITMLibSettings::LIBMODE_BASIC_SURFELS:
			mainEngine = new ITMBasicSurfelEngine<ITMSurfelT>(internalSettings, imageSource->getCalib(),
			                                                  imageSource->getRGBImageSize(),
			                                                  imageSource->getDepthImageSize());
			break;
		case ITMLibSettings::LIBMODE_LOOPCLOSURE:
			mainEngine = new ITMMultiEngine<ITMVoxel, ITMVoxelIndex>(internalSettings, imageSource->getCalib(),
			                                                         imageSource->getRGBImageSize(),
			                                                         imageSource->getDepthImageSize());
			break;
		default:
			throw std::runtime_error("Unsupported library mode!");
	}

	fs::create_directories(appData.outputDirectory);

	UIEngine::Instance()->Initialise(argc, argv, imageSource, imuSource, mainEngine,
	                                 appData.outputDirectory,
	                                 internalSettings->deviceType);
	UIEngine::Instance()->Run();
	UIEngine::Instance()->Shutdown();

	delete mainEngine;
	delete imageSource;
	delete imuSource;
	return 0;
}
