// Copyright 2014-2017 Oxford University Innovation Limited and the authors of InfiniTAM

#pragma once

#include "ITMHierarchyLevel.h"
#include "TrackerIterationType.h"
#include "../../Utils/ITMMath.h"
#include "../../../ORUtils/Image.h"

namespace ITMLib
{
	class ITMSceneHierarchyLevel : public ITMHierarchyLevel
	{
	public:
		ORUtils::Image<Vector4f> *pointsMap = nullptr;
		ORUtils::Image<Vector4f> *normalsMap = nullptr;

		ITMSceneHierarchyLevel(Vector2i imgSize, int levelId, TrackerIterationType iterationType, MemoryDeviceType memoryType, bool skipAllocation = false)
		:ITMHierarchyLevel(levelId, iterationType, skipAllocation)
		{
			if (!skipAllocation) {
				this->pointsMap = new ORUtils::Image<Vector4f>(imgSize, memoryType);
				this->normalsMap = new ORUtils::Image<Vector4f>(imgSize, memoryType);
			}
		}

		void UpdateHostFromDevice()
		{ 
			this->pointsMap->UpdateHostFromDevice();
			this->normalsMap->UpdateHostFromDevice();
		}

		void UpdateDeviceFromHost()
		{ 
			this->pointsMap->UpdateDeviceFromHost();
			this->normalsMap->UpdateDeviceFromHost();
		}

		~ITMSceneHierarchyLevel(void)
		{
			if (manageData) {
				delete pointsMap;
				delete normalsMap;
			}
		}

		// Suppress the default copy constructor and assignment operator
		ITMSceneHierarchyLevel(const ITMSceneHierarchyLevel&);
		ITMSceneHierarchyLevel& operator=(const ITMSceneHierarchyLevel&);
	};
}