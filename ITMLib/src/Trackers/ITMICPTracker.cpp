// Copyright 2014-2017 Oxford University Innovation Limited and the authors of InfiniTAM

#include <ITMLib/Trackers/ITMICPTracker.h>
#include <ORUtils/Cholesky.h>
#include <ORUtils/EigenConversion.h>
#include <sophus/sim3.hpp>

#include <cmath>
#include <iostream>

using namespace ITMLib;

ITMICPTracker::ITMICPTracker(Vector2i imgSize, Vector2i imgSize_rgb, const Parameters& parameters,
                             const ITMLowLevelEngine* lowLevelEngine, MemoryDeviceType memoryType)
	: parameters(parameters)
{
	int noHierarchyLevels = parameters.levels.size();

	viewHierarchy_depth = new ITMImageHierarchy<ITMTemplatedHierarchyLevel<ITMFloatImage> >(imgSize,
	                                                                                        &(parameters.levels[0]),
	                                                                                        noHierarchyLevels, memoryType,
	                                                                                        true);
	sceneHierarchy = new ITMImageHierarchy<ITMSceneHierarchyLevel>(imgSize, &(parameters.levels[0]), noHierarchyLevels,
	                                                               memoryType, true);

	if (parameters.useColour)
	{
		// Do NOT skip allocation for level 0 since intensity images are only used in the tracker
		viewHierarchy_intensity = new ITMImageHierarchy<ITMIntensityHierarchyLevel>(
			imgSize_rgb, &(parameters.levels[0]), noHierarchyLevels, memoryType, false);
		reprojectedPointsHierarchy = new ITMImageHierarchy<ITMTemplatedHierarchyLevel<ITMFloat4Image>>(
			imgSize_rgb, &(parameters.levels[0]), noHierarchyLevels, memoryType, false);
		projectedIntensityHierarchy = new ITMImageHierarchy<ITMTemplatedHierarchyLevel<ITMFloatImage>>(
			imgSize_rgb, &(parameters.levels[0]), noHierarchyLevels, memoryType, false);
	}

	this->noIterationsPerLevel = new int[noHierarchyLevels];
	this->distThresh = new float[noHierarchyLevels];
	this->colourThresh = new float[noHierarchyLevels];

	SetupLevels();

	this->lowLevelEngine = lowLevelEngine;

	map = new ORUtils::HomkerMap(2);
	svmClassifier = new ORUtils::SVMClassifier(map->getDescriptorSize(4));

	//all below obtained from dataset in matlab
	float w[20];
	w[0] = -3.15813f;
	w[1] = -2.38038f;
	w[2] = 1.93359f;
	w[3] = 1.56642f;
	w[4] = 1.76306f;
	w[5] = -0.747641f;
	w[6] = 4.41852f;
	w[7] = 1.72048f;
	w[8] = -0.482545f;
	w[9] = -5.07793f;
	w[10] = 1.98676f;
	w[11] = -0.45688f;
	w[12] = 2.53969f;
	w[13] = -3.50527f;
	w[14] = -1.68725f;
	w[15] = 2.31608f;
	w[16] = 5.14778f;
	w[17] = 2.31334f;
	w[18] = -14.128f;
	w[19] = 6.76423f;

	float b = 9.334260e-01f + parameters.failureDetectorThreshold;

	mu = Vector4f(-34.9470512137603f, -33.1379108518478f, 0.195948598235857f, 0.611027292662361f);
	sigma = Vector4f(68.1654461020426f, 60.6607826748643f, 0.00343068557187040f, 0.0402595570918749f);

	svmClassifier->SetVectors(w, b);
}

ITMICPTracker::~ITMICPTracker(void)
{
	delete this->viewHierarchy_depth;
	delete this->viewHierarchy_intensity;
	delete this->sceneHierarchy;
	delete this->reprojectedPointsHierarchy;
	delete this->projectedIntensityHierarchy;

	delete[] this->noIterationsPerLevel;
	delete[] this->distThresh;

	delete map;
	delete svmClassifier;
}

void ITMICPTracker::SetupLevels()
{
	int noHierarchyLevels = viewHierarchy_depth->GetNoLevels();

	if ((parameters.numIterationsCoarse != -1) && (parameters.numIterationsFine != -1))
	{
		float step =
			(float) (parameters.numIterationsCoarse - parameters.numIterationsFine) / (float) (noHierarchyLevels - 1);
		float val = (float) parameters.numIterationsCoarse;
		for (int levelId = noHierarchyLevels - 1; levelId >= 0; levelId--)
		{
			this->noIterationsPerLevel[levelId] = (int) round(val);
			val -= step;
		}
	}
	if ((parameters.outlierDistanceCoarse >= 0.0f) && (parameters.outlierDistanceFine >= 0.0f))
	{
		parameters.outlierDistanceCoarse = MAX(parameters.outlierDistanceCoarse, parameters.outlierDistanceFine);
		float step = (parameters.outlierDistanceCoarse - parameters.outlierDistanceFine) / (float) (noHierarchyLevels - 1);
		float val = parameters.outlierDistanceCoarse;
		for (int levelId = noHierarchyLevels - 1; levelId >= 0; levelId--)
		{
			this->distThresh[levelId] = val;
			val -= step;
		}
	}
	if (parameters.outlierColourCoarse >= 0.0f && parameters.outlierColourFine >= 0.0f)
	{
		parameters.outlierColourCoarse = MAX(parameters.outlierColourCoarse, parameters.outlierColourFine);
		float step =
			(float) (parameters.outlierColourCoarse - parameters.outlierColourFine) / (float) (noHierarchyLevels - 1);
		float val = parameters.outlierColourCoarse;
		for (int levelId = noHierarchyLevels - 1; levelId >= 0; levelId--)
		{
			this->colourThresh[levelId] = val;
			val -= step;
		}
	}
}

void ITMICPTracker::SetEvaluationData(ITMTrackingState* trackingState, const ITMView* view)
{
	this->trackingState = trackingState;
	this->view = view;

	sceneHierarchy->GetLevel(0)->intrinsics = view->calib.intrinsics_d.projectionParamsSimple.all;
	viewHierarchy_depth->GetLevel(0)->intrinsics = view->calib.intrinsics_d.projectionParamsSimple.all;

	// the image hierarchy allows pointers to external data at level 0
	viewHierarchy_depth->GetLevel(0)->data = view->depth;
	sceneHierarchy->GetLevel(0)->pointsMap = trackingState->pointCloud->locations;
	sceneHierarchy->GetLevel(0)->normalsMap = trackingState->pointCloud->normals;

	if (parameters.useColour)
	{
		viewHierarchy_intensity->GetLevel(0)->intrinsics = view->calib.intrinsics_rgb.projectionParamsSimple.all;

		if (trackingState->framesProcessed % 20 == 2)
		{
			// Convert RGB to intensity
			viewHierarchy_intensity->GetLevel(0)->intensity_prev->SetFrom(
				viewHierarchy_intensity->GetLevel(0)->intensity_current, ORUtils::CUDA_TO_CUDA);
		}
		lowLevelEngine->ConvertColourToIntensity(viewHierarchy_intensity->GetLevel(0)->intensity_current, view->rgb);

		// Compute first level gradients
		lowLevelEngine->GradientXY(viewHierarchy_intensity->GetLevel(0)->gradients,
		                           viewHierarchy_intensity->GetLevel(0)->intensity_prev);
	}

	renderedScenePose = trackingState->pose_pointCloud->GetM();
	intensityReferencePose = trackingState->pose_pointCloud->GetM();
	depthToRGBTransform = view->calib.trafo_rgb_to_depth.calib_inv;
}

void ITMICPTracker::PrepareForEvaluation()
{
	// Create depth pyramid
	for (int i = 1; i < viewHierarchy_depth->GetNoLevels(); i++)
	{
		ITMTemplatedHierarchyLevel<ITMFloatImage>* currentLevelView = viewHierarchy_depth->GetLevel(i);
		ITMTemplatedHierarchyLevel<ITMFloatImage>* previousLevelView = viewHierarchy_depth->GetLevel(i - 1);
		lowLevelEngine->FilterSubsampleWithHoles(currentLevelView->data, previousLevelView->data);
		currentLevelView->intrinsics = previousLevelView->intrinsics * 0.5f;

		ITMSceneHierarchyLevel* currentLevelScene = sceneHierarchy->GetLevel(i);
		ITMSceneHierarchyLevel* previousLevelScene = sceneHierarchy->GetLevel(i - 1);
		//lowLevelEngine->FilterSubsampleWithHoles(currentLevelScene->pointsMap, previousLevelScene->pointsMap);
		//lowLevelEngine->FilterSubsampleWithHoles(currentLevelScene->normalsMap, previousLevelScene->normalsMap);
		currentLevelScene->intrinsics = previousLevelScene->intrinsics * 0.5f;
	}

	if (parameters.useColour)
	{
		for (int i = 1; i < viewHierarchy_intensity->GetNoLevels(); i++)
		{
			ITMIntensityHierarchyLevel* currentLevel = viewHierarchy_intensity->GetLevel(i);
			ITMIntensityHierarchyLevel* previousLevel = viewHierarchy_intensity->GetLevel(i - 1);

			lowLevelEngine->FilterSubsample(currentLevel->intensity_current, previousLevel->intensity_current);
			lowLevelEngine->FilterSubsample(currentLevel->intensity_prev, previousLevel->intensity_prev);
			lowLevelEngine->GradientXY(currentLevel->gradients, currentLevel->intensity_prev);
			currentLevel->intrinsics = previousLevel->intrinsics * 0.5f;
		}

		// Project RGB image according to the depth->rgb transform and cache it to speed up the energy computation
		for (int i = 0; i < viewHierarchy_intensity->GetNoLevels(); ++i)
		{
			ITMTemplatedHierarchyLevel<ITMFloat4Image>* pointsOut = reprojectedPointsHierarchy->GetLevel(i);
			ITMTemplatedHierarchyLevel<ITMFloatImage>* intensityOut = projectedIntensityHierarchy->GetLevel(i);

			const ITMIntensityHierarchyLevel* intensityIn = viewHierarchy_intensity->GetLevel(i);
			const ITMTemplatedHierarchyLevel<ITMFloatImage>* depthIn = viewHierarchy_depth->GetLevel(i);

			Vector4f intrinsics_rgb = intensityIn->intrinsics;
			Vector4f intrinsics_depth = depthIn->intrinsics;

			ComputeDepthPointAndIntensity(pointsOut->data, intensityOut->data,
			                              intensityIn->intensity_current, depthIn->data,
			                              intrinsics_depth, intrinsics_rgb,
			                              view->calib.trafo_rgb_to_depth.calib_inv);
		}
	}

//	if (parameters.optimizeScale)
//	{ // compute point cloud centers
//		size_t noValidPoints;
//		lowLevelEngine->ComputePointCloudCenter(scenePointCloudCenter_world, noValidPoints,
//		                                        sceneHierarchy->GetLevel(0)->pointsMap);
//		if (noValidPoints <= 0) printf("WARNING: could not determine scene point cloud center\n");
//		lowLevelEngine->ComputeDepthCloudCenter(
//			depthPointCloudCenter, noValidPoints,
//			viewHierarchy_depth->GetLevel(viewHierarchy_depth->GetNoLevels() - 1)->data,
//			viewHierarchy_depth->GetLevel(viewHierarchy_depth->GetNoLevels() - 1)->intrinsics);
//		if (noValidPoints <= 0) printf("WARNING: could not determine depth image point cloud center\n");
//	}
}

void ITMICPTracker::SetEvaluationParams(int levelId)
{
	this->levelId = levelId;
	this->iterationType = viewHierarchy_depth->GetLevel(levelId)->iterationType;
}


void ITMICPTracker::ComputeDelta(Eigen::Matrix<EigenT, 6, 1>& delta, Eigen::Matrix<EigenT, 6, 1>& nabla,
                                 Eigen::Matrix<EigenT, 6, 6>& hessian, bool shortIteration) const
{
	delta.setZero();
	if (shortIteration)
	{
		Eigen::Matrix<EigenT, 3, 3> H = hessian.block<3, 3>(0, 0);
		Eigen::Matrix<EigenT, 3, 1> g = nabla.block<3, 1>(0, 0);
		delta.block<3, 1>(0, 0) = H.llt().solve(-g);
	} else
	{
		delta = hessian.llt().solve(-nabla); // negative, because error term of form || J x + r ||^2
	}
}

bool ITMICPTracker::HasConverged(const Eigen::Matrix<EigenT, 6, 1>& delta) const
{
	return (delta.norm() / 6) < parameters.smallStepSizeCriterion;
}

template<class Derived>
inline Eigen::Matrix<typename Derived::Scalar, 3, 3> VectorToSkewSymmetricMatrix(const Eigen::MatrixBase<Derived>& vec)
{
	EIGEN_STATIC_ASSERT_VECTOR_SPECIFIC_SIZE(Derived, 3);
	return (Eigen::Matrix<typename Derived::Scalar, 3, 3>() << 0.0, -vec[2], vec[1],
		vec[2], 0.0, -vec[0],
		-vec[1], vec[0], 0.0).finished();
}

void
ITMICPTracker::ApplyDelta(const Matrix4f& para_old, const Eigen::Matrix<EigenT, 6, 1>& delta, Matrix4f& para_new) const
{
	Eigen::Matrix<EigenT, 4, 4> Tinc = Eigen::Matrix<EigenT, 4, 4>::Identity();
	switch (iterationType)
	{
		case TRACKER_ITERATION_ROTATION:
			Tinc.block<3, 3>(0, 0) += VectorToSkewSymmetricMatrix(delta.block<3, 1>(0, 0));
			break;
		case TRACKER_ITERATION_TRANSLATION:
			Tinc.block<3, 1>(0, 3) = Eigen::Matrix<EigenT, 3, 1>(delta.block<3, 1>(0, 0));
			break;
		default:
		case TRACKER_ITERATION_BOTH:
			Tinc.block<3, 3>(0, 0) += VectorToSkewSymmetricMatrix(delta.block<3, 1>(0, 0));
			Tinc.block<3, 1>(0, 3) = delta.block<3, 1>(3, 0);
			break;
	}
	para_new = ORUtils::FromEigen<Matrix4f>(Tinc) * para_old;
}

void
ITMICPTracker::UpdatePoseQuality(int noValidPoints_old, const Eigen::Matrix<EigenT, 6, 6>& hessian_good, float f_old)
{
	size_t noTotalPoints = viewHierarchy_depth->GetLevel(0)->data->dataSize;

	int noValidPointsMax = lowLevelEngine->CountValidDepths(view->depth);

	float normFactor_v1 = (float) noValidPoints_old / (float) noTotalPoints;
	float normFactor_v2 = (float) noValidPoints_old / (float) noValidPointsMax;

	float det_norm_v1 = 0.0f;
	float det_norm_v2 = 0.0f;
	if (iterationType == TRACKER_ITERATION_BOTH)
	{
		det_norm_v1 = (normFactor_v1 * hessian_good).llt().matrixL().determinant();
		det_norm_v2 = (normFactor_v2 * hessian_good).llt().matrixL().determinant();
	} else
	{
		det_norm_v1 = (normFactor_v1 * hessian_good.block<3, 3>(0, 0)).llt().matrixL().determinant();
		det_norm_v2 = (normFactor_v2 * hessian_good.block<3, 3>(0, 0)).llt().matrixL().determinant();
	}
	if (std::isnan(det_norm_v1)) det_norm_v1 = 0.0f;
	if (std::isnan(det_norm_v2)) det_norm_v2 = 0.0f;

	float finalResidual_v2 = sqrt(
		((float) noValidPoints_old * f_old + (float) (noValidPointsMax - noValidPoints_old) * distThresh[0]) /
		(float) noValidPointsMax);
	float percentageInliers_v2 = (float) noValidPoints_old / (float) noValidPointsMax;

	trackingState->trackerResult = ITMTrackingState::TRACKING_FAILED;
	trackingState->trackerScore = finalResidual_v2;

	if (noValidPointsMax != 0 && noTotalPoints != 0 && det_norm_v1 > 0 && det_norm_v2 > 0)
	{
		Vector4f inputVector(log(det_norm_v1), log(det_norm_v2), finalResidual_v2, percentageInliers_v2);

		Vector4f normalisedVector = (inputVector - mu) / sigma;

		float mapped[20];
		map->evaluate(mapped, normalisedVector.v, 4);

		float score = svmClassifier->Classify(mapped);

		if (score > 0) trackingState->trackerResult = ITMTrackingState::TRACKING_GOOD;
		else if (score > -10.0f) trackingState->trackerResult = ITMTrackingState::TRACKING_POOR;
	}
}

void ITMICPTracker::TrackCameraSE3(ITMTrackingState* trackingState, const ITMView* view)
{
	float f_old = 1e10, f_new;
	float f_depth, f_rgb;
	int noValidPoints_depth = 0;
	int noValidPoints_RGB = 0;
	int noValidPoints_old = 0;

	/** Approximated Hessian of error function
	 * H ~= 2 * J^T * J  (J = Jacobian)
	 */
	Eigen::Matrix<EigenT, 6, 6> hessian_good;
	hessian_good.setZero();

	/** Gradient vector of error function
	 * g = 2 * J^T * r (J = Jacobian)
	 */
	Eigen::Matrix<EigenT, 6, 1> nabla_good;
	nabla_good.setZero();

	for (int levelId = viewHierarchy_depth->GetNoLevels() - 1; levelId >= 0; levelId--)
	{
		this->SetEvaluationParams(levelId);
		if (iterationType == TRACKER_ITERATION_NONE) continue;

		Matrix4f approxInvPose = trackingState->pose_d->GetInvM();
		ORUtils::SE3Pose lastKnownGoodPose(*(trackingState->pose_d));
		f_old = 1e20f;
		noValidPoints_old = 0;

		/// Levenber-Marquardt (Fletcher) damping factor
		float lambda = 1.0;

		for (int iterNo = 0; iterNo < noIterationsPerLevel[levelId]; iterNo++)
		{
			Eigen::Matrix<float, 6, 6> hessian_new;
			Eigen::Matrix<float, 6, 1> nabla_new;

			noValidPoints_depth = this->ComputeGandH_Depth(f_depth, nabla_new.data(), hessian_new.data(), approxInvPose);

			f_new = f_depth;

			if (parameters.useColour)
			{
				Eigen::Matrix<float, 6, 6> hessian_RGB;
				Eigen::Matrix<float, 6, 1> nabla_RGB;
				noValidPoints_RGB = this->ComputeGandH_RGB(f_rgb, nabla_RGB.data(), hessian_RGB.data(), approxInvPose);
				if (noValidPoints_RGB > 0)
				{
					float normalizationFactor =  hessian_new.squaredNorm() / hessian_RGB.squaredNorm();
					hessian_new = hessian_new + normalizationFactor * parameters.colourWeight * hessian_RGB;
					nabla_new = nabla_new + std::sqrt(normalizationFactor) * std::sqrt(parameters.colourWeight) * nabla_RGB;
					f_new = f_new + parameters.colourWeight * f_rgb;
				}
			}

			// check if error increased. If so, revert
			if ((noValidPoints_depth <= 0 && noValidPoints_RGB <= 0) || (f_new > f_old))
			{
				trackingState->pose_d->SetFrom(&lastKnownGoodPose);
				approxInvPose = trackingState->pose_d->GetInvM();
				lambda *= 10.0f;
			} else
			{
				lastKnownGoodPose.SetFrom(trackingState->pose_d);
				f_old = f_new;
				noValidPoints_old = noValidPoints_depth; // use only number of depth point for computing pose quality

				hessian_good = hessian_new.cast<EigenT>();
				nabla_good = nabla_new.cast<EigenT>();
				lambda /= 10.0f;
			}
			Eigen::Matrix<EigenT, 6, 6> A = hessian_good;
			A.diagonal() *= (1 + lambda);

			// compute a new step and make sure we've got an SE3
			Eigen::Matrix<EigenT, 6, 1> delta;
			ComputeDelta(delta, nabla_good, A, iterationType != TRACKER_ITERATION_BOTH);
			ApplyDelta(approxInvPose, delta, approxInvPose);
			trackingState->pose_d->SetInvM(approxInvPose);
			trackingState->pose_d->Coerce();
			approxInvPose = trackingState->pose_d->GetInvM();

			// if step is small, assume it's going to decrease the error and finish
			if (HasConverged(delta)) break;
		}
	}

	this->UpdatePoseQuality(noValidPoints_old, hessian_good, f_old);
}

void ITMICPTracker::TrackCameraSim3(ITMTrackingState* trackingState, const ITMView* view)
{
	float f_old = 1e10, f_new;
	float f_depth, f_rgb;
	int noValidPoints_depth = 0;
	int noValidPoints_rgb = 0;
	int noValidPoints_old = 0;

	/** Approximated Hessian of error function
	 * H ~= 2 * J^T * J  (J = Jacobian)
	 */
	Eigen::Matrix<EigenT, 7, 7> hessian_good;
	hessian_good.setZero();

	/** Gradient vector of error function
	 * g = 2 * J^T * r (J = Jacobian)
	 */
	Eigen::Matrix<EigenT, 7, 1> nabla_good;
	nabla_good.setZero();

	Sophus::Sim3<EigenT> invPose;
	{
		Eigen::Matrix<EigenT, 7, 1> tangent;
		tangent << trackingState->pose_d->GetParams()[0], trackingState->pose_d->GetParams()[1],
			trackingState->pose_d->GetParams()[2], trackingState->pose_d->GetParams()[3],
			trackingState->pose_d->GetParams()[4], trackingState->pose_d->GetParams()[5], -trackingState->scaleFactor; // invert scale, as it is applied to inverse trackingState->pose_d
		invPose = Sophus::Sim3<EigenT>::exp(tangent).inverse();
	}

	Sophus::Sim3<EigenT> lastKnownGoodInvPose(invPose);

	/// all GPU computations require ORUtils Matrix. Note, that this pose includes the scale factor!
	Matrix4f approxInvPose = ORUtils::FromEigen<Matrix4f>(invPose.matrix());

	const float weightNormalizer = 1 / (1 + parameters.colourWeight);

	for (int levelId = viewHierarchy_depth->GetNoLevels() - 1; levelId >= 0; levelId--)
	{
		this->SetEvaluationParams(levelId);

		f_old = 1e20f;
		noValidPoints_old = 0;

		/// Levenber-Marquardt (Fletcher) damping factor
		float lambda = 1.0;
		float lambdaUp = 2.0;

		for (int iterNo = 0; iterNo < noIterationsPerLevel[levelId]; iterNo++)
		{
			Eigen::Matrix<EigenT, 7, 7> hessian_new;
			Eigen::Matrix<EigenT, 7, 1> nabla_new;
			noValidPoints_depth = this->ComputeGandHSim3_Depth(f_depth, hessian_new, nabla_new, approxInvPose);

			f_new = f_depth;

			if (parameters.useColour)
			{
				Eigen::Matrix<EigenT, 7, 7> hessian_rgb;
				Eigen::Matrix<EigenT, 7, 1> nabla_rgb;
				noValidPoints_rgb = this->ComputeGandHSim3_RGB(f_rgb, hessian_rgb, nabla_rgb, approxInvPose);
				if (noValidPoints_rgb > 0)
				{
					// normalize hessian and residual s.t. same magnitude as depth
					float normalizationFactor =  hessian_new.squaredNorm() / hessian_rgb.squaredNorm();

					hessian_new = hessian_new + normalizationFactor * parameters.colourWeight * hessian_rgb;
					nabla_new = nabla_new + std::sqrt(normalizationFactor) * std::sqrt(parameters.colourWeight) * nabla_rgb;
					f_new = f_new + parameters.colourWeight * f_rgb;
				}
			}

			float fDiff = f_old - f_new;

			// check if error increased. If so, revert
			if ((noValidPoints_depth <= 0 && noValidPoints_rgb <= 0) || (f_new > f_old))
			{
				invPose = lastKnownGoodInvPose;
			} else
			{
				lastKnownGoodInvPose = invPose;
				f_old = f_new;
				noValidPoints_old = noValidPoints_depth; // use only number of depth point for computing pose quality

				hessian_good = hessian_new;
				nabla_good = nabla_new;
			}

			// compute a new step
			Eigen::Matrix<EigenT, 7, 7> A = hessian_good;
			A.diagonal() *= (1 + lambda);
			Eigen::Matrix<EigenT, 7, 1> delta = A.llt().solve(-nabla_good);

			// update invPose
			invPose = Sophus::Sim3<EigenT>::exp(delta) * invPose;
			approxInvPose = ORUtils::FromEigen<Matrix4f>(invPose.matrix());

			// update lambda (2004, Madsen, Methods For Non-Linear Least Squares Problems)
			if (fDiff > 0)
			{
				const double fDiff_predicted = 0.5 * delta.dot(delta * lambda - (nabla_good));
				float gainRatio = fDiff / fDiff_predicted; // actual error decrease vs predicted error decrease
				lambdaUp = 2;
				lambda *= std::max(1.0 / 3, 1 - std::pow(2 * gainRatio - 1, 3.0));
			} else
			{
				lambda *= lambdaUp;
				lambdaUp *= 2;
			}

			// if step is small, assume it's going to decrease the error and finish
			if (delta.norm() < parameters.smallStepSizeCriterion)
				break;
		}
	}

	// write back estimated invPose and scale
	Eigen::Matrix<EigenT, 7, 1> tangent = invPose.inverse().log();

	trackingState->scaleFactor = invPose.log()[6];
	trackingState->pose_d->SetFrom(tangent[0], tangent[1], tangent[2], tangent[3], tangent[4], tangent[5]);
	trackingState->pose_d->Coerce();

	std::cout << "scale: " << std::exp(trackingState->scaleFactor) << std::endl;
//	std::cout << trackingState->pose_d->GetInvM() << std::endl;

	this->UpdatePoseQuality(noValidPoints_old, hessian_good.block<6, 6>(0, 0), f_old);
}

void ITMICPTracker::TrackCamera(ITMTrackingState* trackingState, const ITMView* view)
{
	this->SetEvaluationData(trackingState, view); // populate evaluation data, even if no current point cloud (set previous)
	if (!trackingState->HasValidPointCloud()) return;
	this->PrepareForEvaluation();

	if (parameters.optimizeScale)
		TrackCameraSim3(trackingState, view);
	else
		TrackCameraSE3(trackingState, view);
}