/*
 * Copyright (C) 2015-2016 Antmicro
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "Cam-Camera"
#define LOG_NDEBUG NDEBUG

#include <hardware/camera3.h>
#include <camera/CameraMetadata.h>
#include <utils/misc.h>
#include <utils/Log.h>
#include <hardware/gralloc.h>
#include <ui/Rect.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Fence.h>
#include <assert.h>

#include "DbgUtils.h"
#include "Camera.h"
#include "ImageConverter.h"

extern camera_module_t HAL_MODULE_INFO_SYM;

namespace android {
/**
 * \class Camera
 *
 * Android's Camera 3 device implementation.
 *
 * Declaration of camera capabilities, frame request handling, etc. This code
 * is what Android framework talks to.
 */

Camera::Camera()
    : mStaticCharacteristics(NULL)
    , mCallbackOps(NULL)
    , mJpegBufferSize(0) {
    DBGUTILS_AUTOLOGCALL(__func__);
    for(size_t i = 0; i < NELEM(mDefaultRequestSettings); i++) {
        mDefaultRequestSettings[i] = NULL;
    }

    common.tag      = HARDWARE_DEVICE_TAG;
    common.version  = CAMERA_DEVICE_API_VERSION_3_0;
    common.module   = &HAL_MODULE_INFO_SYM.common;
    common.close    = Camera::sClose;
    ops             = &sOps;
    priv            = NULL;

    mValid = true;
    mDev = new V4l2Device("/dev/video0");
    if(!mDev) {
        mValid = false;
    }
}

Camera::~Camera() {
    DBGUTILS_AUTOLOGCALL(__func__);
    gWorkers.stop();
    mDev->disconnect();
    delete mDev;
}

status_t Camera::cameraInfo(struct camera_info *info) {
    DBGUTILS_AUTOLOGCALL(__func__);
    Mutex::Autolock lock(mMutex);
    info->facing = CAMERA_FACING_BACK;
    info->orientation = 0;
    info->device_version = CAMERA_DEVICE_API_VERSION_3_0;
    info->static_camera_characteristics = staticCharacteristics();

    return NO_ERROR;
}

int Camera::openDevice(hw_device_t **device) {
    DBGUTILS_AUTOLOGCALL(__func__);
    Mutex::Autolock lock(mMutex);
    mDev->connect();
    *device = &common;

    gWorkers.start();

    return NO_ERROR;
}

int Camera::closeDevice() {
    DBGUTILS_AUTOLOGCALL(__func__);
    Mutex::Autolock lock(mMutex);

    gWorkers.stop();
    mDev->disconnect();

    return NO_ERROR;
}

camera_metadata_t *Camera::staticCharacteristics() {
    if(mStaticCharacteristics)
        return mStaticCharacteristics;

    CameraMetadata cm;

    auto &resolutions = mDev->availableResolutions();
    auto &previewResolutions = resolutions;
    auto sensorRes = mDev->sensorResolution();

    /***********************************\
    |* START OF CAMERA CHARACTERISTICS *|
    \***********************************/

    /* fake, but valid aspect ratio */
    const float sensorInfoPhysicalSize[] = {
        5.0f,
        5.0f * (float)sensorRes.height / (float)sensorRes.width
    };
    cm.update(ANDROID_SENSOR_INFO_PHYSICAL_SIZE, sensorInfoPhysicalSize, NELEM(sensorInfoPhysicalSize));

    /* fake */
    static const float lensInfoAvailableFocalLengths[] = {3.30f};
    cm.update(ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS, lensInfoAvailableFocalLengths, NELEM(lensInfoAvailableFocalLengths));

    static const uint8_t lensFacing = ANDROID_LENS_FACING_BACK;
    cm.update(ANDROID_LENS_FACING, &lensFacing, 1);
    const int32_t sensorInfoPixelArraySize[] = {
        (int32_t)sensorRes.width,
        (int32_t)sensorRes.height
    };
    cm.update(ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE, sensorInfoPixelArraySize, NELEM(sensorInfoPixelArraySize));

    const int32_t sensorInfoActiveArraySize[] = {
        0,                          0,
        (int32_t)sensorRes.width,   (int32_t)sensorRes.height
    };
    cm.update(ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE, sensorInfoActiveArraySize, NELEM(sensorInfoActiveArraySize));

    static const int32_t scalerAvailableFormats[] = {
        HAL_PIXEL_FORMAT_RGBA_8888,
        HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
        /* Non-preview one, must be last - see following code */
        HAL_PIXEL_FORMAT_BLOB
    };
    cm.update(ANDROID_SCALER_AVAILABLE_FORMATS, scalerAvailableFormats, NELEM(scalerAvailableFormats));

    /* Only for HAL_PIXEL_FORMAT_BLOB */
    const size_t mainStreamConfigsCount = resolutions.size();
    /* For all other supported pixel formats */
    const size_t previewStreamConfigsCount = previewResolutions.size() * (NELEM(scalerAvailableFormats) - 1);
    const size_t streamConfigsCount = mainStreamConfigsCount + previewStreamConfigsCount;

    int32_t scalerAvailableStreamConfigurations[streamConfigsCount * 4];
    int64_t scalerAvailableMinFrameDurations[streamConfigsCount * 4];

    int32_t scalerAvailableProcessedSizes[previewResolutions.size() * 2];
    int64_t scalerAvailableProcessedMinDurations[previewResolutions.size()];
    int32_t scalerAvailableJpegSizes[resolutions.size() * 2];
    int64_t scalerAvailableJpegMinDurations[resolutions.size()];

    size_t i4 = 0;
    size_t i2 = 0;
    size_t i1 = 0;
    /* Main stream configurations */
    for(size_t resId = 0; resId < resolutions.size(); ++resId) {
        scalerAvailableStreamConfigurations[i4 + 0] = HAL_PIXEL_FORMAT_BLOB;
        scalerAvailableStreamConfigurations[i4 + 1] = (int32_t)resolutions[resId].width;
        scalerAvailableStreamConfigurations[i4 + 2] = (int32_t)resolutions[resId].height;
        scalerAvailableStreamConfigurations[i4 + 3] = ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT;

        scalerAvailableMinFrameDurations[i4 + 0] = HAL_PIXEL_FORMAT_BLOB;
        scalerAvailableMinFrameDurations[i4 + 1] = (int32_t)resolutions[resId].width;
        scalerAvailableMinFrameDurations[i4 + 2] = (int32_t)resolutions[resId].height;
        scalerAvailableMinFrameDurations[i4 + 3] = 1000000000 / 60; /* TODO: read from the device */

        scalerAvailableJpegSizes[i2 + 0] = (int32_t)resolutions[resId].width;
        scalerAvailableJpegSizes[i2 + 1] = (int32_t)resolutions[resId].height;

        scalerAvailableJpegMinDurations[i1] = 1000000000 / 60; /* TODO: read from the device */

        i4 += 4;
        i2 += 2;
        i1 += 1;
    }
    i2 = 0;
    i1 = 0;
    /* Preview stream configurations */
    for(size_t resId = 0; resId < previewResolutions.size(); ++resId) {
        for(size_t fmtId = 0; fmtId < NELEM(scalerAvailableFormats) - 1; ++fmtId) {
            scalerAvailableStreamConfigurations[i4 + 0] = scalerAvailableFormats[fmtId];
            scalerAvailableStreamConfigurations[i4 + 1] = (int32_t)previewResolutions[resId].width;
            scalerAvailableStreamConfigurations[i4 + 2] = (int32_t)previewResolutions[resId].height;
            scalerAvailableStreamConfigurations[i4 + 3] = ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT;

            scalerAvailableMinFrameDurations[i4 + 0] = scalerAvailableFormats[fmtId];
            scalerAvailableMinFrameDurations[i4 + 1] = (int32_t)previewResolutions[resId].width;
            scalerAvailableMinFrameDurations[i4 + 2] = (int32_t)previewResolutions[resId].height;
            scalerAvailableMinFrameDurations[i4 + 3] = 1000000000 / 60; /* TODO: read from the device */

            i4 += 4;
        }
        scalerAvailableProcessedSizes[i2 + 0] = (int32_t)previewResolutions[resId].width;
        scalerAvailableProcessedSizes[i2 + 1] = (int32_t)previewResolutions[resId].height;

        scalerAvailableProcessedMinDurations[i1] = 1000000000 / 60; /* TODO: read from the device */

        i2 += 2;
        i1 += 1;
    }
    cm.update(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, scalerAvailableStreamConfigurations, (size_t)NELEM(scalerAvailableStreamConfigurations));
    cm.update(ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS, scalerAvailableMinFrameDurations, (size_t)NELEM(scalerAvailableMinFrameDurations));
    /* Probably fake */
    cm.update(ANDROID_SCALER_AVAILABLE_STALL_DURATIONS, scalerAvailableMinFrameDurations, (size_t)NELEM(scalerAvailableMinFrameDurations));
    cm.update(ANDROID_SCALER_AVAILABLE_JPEG_SIZES, scalerAvailableJpegSizes, (size_t)NELEM(scalerAvailableJpegSizes));
    cm.update(ANDROID_SCALER_AVAILABLE_JPEG_MIN_DURATIONS, scalerAvailableJpegMinDurations, (size_t)NELEM(scalerAvailableJpegMinDurations));
    cm.update(ANDROID_SCALER_AVAILABLE_PROCESSED_SIZES, scalerAvailableProcessedSizes, (size_t)NELEM(scalerAvailableProcessedSizes));
    cm.update(ANDROID_SCALER_AVAILABLE_PROCESSED_MIN_DURATIONS, scalerAvailableProcessedMinDurations, (size_t)NELEM(scalerAvailableProcessedMinDurations));

    /* ~8.25 bit/px (https://en.wikipedia.org/wiki/JPEG#Sample_photographs) */
    /* Use 9 bit/px, add buffer info struct size, round up to page size */
    mJpegBufferSize = sensorRes.width * sensorRes.height * 9 + sizeof(camera3_jpeg_blob);
    mJpegBufferSize = (mJpegBufferSize + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    const int32_t jpegMaxSize = (int32_t)mJpegBufferSize;
    cm.update(ANDROID_JPEG_MAX_SIZE, &jpegMaxSize, 1);

    static const int32_t jpegAvailableThumbnailSizes[] = {
        0, 0,
        320, 240
    };
    cm.update(ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, jpegAvailableThumbnailSizes, NELEM(jpegAvailableThumbnailSizes));

    static const int32_t sensorOrientation = 90;
    cm.update(ANDROID_SENSOR_ORIENTATION, &sensorOrientation, 1);

    static const uint8_t flashInfoAvailable = ANDROID_FLASH_INFO_AVAILABLE_FALSE;
    cm.update(ANDROID_FLASH_INFO_AVAILABLE, &flashInfoAvailable, 1);

    static const float scalerAvailableMaxDigitalZoom = 1;
    cm.update(ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &scalerAvailableMaxDigitalZoom, 1);

    static const uint8_t statisticsFaceDetectModes[] = {
        ANDROID_STATISTICS_FACE_DETECT_MODE_OFF
    };
    cm.update(ANDROID_STATISTICS_FACE_DETECT_MODE, statisticsFaceDetectModes, NELEM(statisticsFaceDetectModes));

    static const int32_t statisticsInfoMaxFaceCount = 0;
    cm.update(ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, &statisticsInfoMaxFaceCount, 1);

    static const uint8_t controlAvailableSceneModes[] = {
        ANDROID_CONTROL_SCENE_MODE_DISABLED
    };
    cm.update(ANDROID_CONTROL_AVAILABLE_SCENE_MODES, controlAvailableSceneModes, NELEM(controlAvailableSceneModes));

    static const uint8_t controlAvailableEffects[] = {
            ANDROID_CONTROL_EFFECT_MODE_OFF
    };
    cm.update(ANDROID_CONTROL_AVAILABLE_EFFECTS, controlAvailableEffects, NELEM(controlAvailableEffects));

    static const int32_t controlMaxRegions[] = {
        0, /* AE */
        0, /* AWB */
        0  /* AF */
    };
    cm.update(ANDROID_CONTROL_MAX_REGIONS, controlMaxRegions, NELEM(controlMaxRegions));

    static const uint8_t controlAeAvailableModes[] = {
            ANDROID_CONTROL_AE_MODE_OFF
    };
    cm.update(ANDROID_CONTROL_AE_AVAILABLE_MODES, controlAeAvailableModes, NELEM(controlAeAvailableModes));

    static const camera_metadata_rational controlAeCompensationStep = {1, 3};
    cm.update(ANDROID_CONTROL_AE_COMPENSATION_STEP, &controlAeCompensationStep, 1);

    int32_t controlAeCompensationRange[] = {-9, 9};
    cm.update(ANDROID_CONTROL_AE_COMPENSATION_RANGE, controlAeCompensationRange, NELEM(controlAeCompensationRange));

    static const int32_t controlAeAvailableTargetFpsRanges[] = {
        60, 60
    };
    cm.update(ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES, controlAeAvailableTargetFpsRanges, NELEM(controlAeAvailableTargetFpsRanges));

    static const uint8_t controlAeAvailableAntibandingModes[] = {
            ANDROID_CONTROL_AE_ANTIBANDING_MODE_OFF
    };
    cm.update(ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES, controlAeAvailableAntibandingModes, NELEM(controlAeAvailableAntibandingModes));

    static const uint8_t controlAwbAvailableModes[] = {
            ANDROID_CONTROL_AWB_MODE_AUTO,
            ANDROID_CONTROL_AWB_MODE_OFF
    };
    cm.update(ANDROID_CONTROL_AWB_AVAILABLE_MODES, controlAwbAvailableModes, NELEM(controlAwbAvailableModes));

    static const uint8_t controlAfAvailableModes[] = {
        ANDROID_CONTROL_AF_MODE_OFF
    };
    cm.update(ANDROID_CONTROL_AF_AVAILABLE_MODES, controlAfAvailableModes, NELEM(controlAfAvailableModes));

    static const uint8_t controlAvailableVideoStabilizationModes[] = {
            ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF
    };
    cm.update(ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES, controlAvailableVideoStabilizationModes, NELEM(controlAvailableVideoStabilizationModes));

    const uint8_t infoSupportedHardwareLevel = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED;
    cm.update(ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &infoSupportedHardwareLevel, 1);

    /***********************************\
    |*  END OF CAMERA CHARACTERISTICS  *|
    \***********************************/

    mStaticCharacteristics = cm.release();
    return mStaticCharacteristics;
}

int Camera::initialize(const camera3_callback_ops_t *callbackOps) {
    DBGUTILS_AUTOLOGCALL(__func__);
    Mutex::Autolock lock(mMutex);

    mCallbackOps = callbackOps;
    return NO_ERROR;
}

const camera_metadata_t * Camera::constructDefaultRequestSettings(int type) {
    DBGUTILS_AUTOLOGCALL(__func__);
    Mutex::Autolock lock(mMutex);
    /* TODO: validate type */

    if(mDefaultRequestSettings[type]) {
        return mDefaultRequestSettings[type];
    }

    CameraMetadata cm;

    static const int32_t requestId = 0;
    cm.update(ANDROID_REQUEST_ID, &requestId, 1);

    static const float lensFocusDistance = 0.0f;
    cm.update(ANDROID_LENS_FOCUS_DISTANCE, &lensFocusDistance, 1);

    auto sensorSize = mDev->sensorResolution();
    const int32_t scalerCropRegion[] = {
        0,                          0,
        (int32_t)sensorSize.width,  (int32_t)sensorSize.height
    };
    cm.update(ANDROID_SCALER_CROP_REGION, scalerCropRegion, NELEM(scalerCropRegion));

    static const int32_t jpegThumbnailSize[] = {
        0, 0
    };
    cm.update(ANDROID_JPEG_THUMBNAIL_SIZE, jpegThumbnailSize, NELEM(jpegThumbnailSize));

    static const uint8_t jpegThumbnailQuality = 50;
    cm.update(ANDROID_JPEG_THUMBNAIL_QUALITY, &jpegThumbnailQuality, 1);

    static const double jpegGpsCoordinates[] = {
        0, 0
    };
    cm.update(ANDROID_JPEG_GPS_COORDINATES, jpegGpsCoordinates, NELEM(jpegGpsCoordinates));

    static const uint8_t jpegGpsProcessingMethod[32] = "None";
    cm.update(ANDROID_JPEG_GPS_PROCESSING_METHOD, jpegGpsProcessingMethod, NELEM(jpegGpsProcessingMethod));

    static const int64_t jpegGpsTimestamp = 0;
    cm.update(ANDROID_JPEG_GPS_TIMESTAMP, &jpegGpsTimestamp, 1);

    static const int32_t jpegOrientation = 0;
    cm.update(ANDROID_JPEG_ORIENTATION, &jpegOrientation, 1);

    /** android.stats */

    static const uint8_t statisticsFaceDetectMode = ANDROID_STATISTICS_FACE_DETECT_MODE_OFF;
    cm.update(ANDROID_STATISTICS_FACE_DETECT_MODE, &statisticsFaceDetectMode, 1);

    static const uint8_t statisticsHistogramMode = ANDROID_STATISTICS_HISTOGRAM_MODE_OFF;
    cm.update(ANDROID_STATISTICS_HISTOGRAM_MODE, &statisticsHistogramMode, 1);

    static const uint8_t statisticsSharpnessMapMode = ANDROID_STATISTICS_SHARPNESS_MAP_MODE_OFF;
    cm.update(ANDROID_STATISTICS_SHARPNESS_MAP_MODE, &statisticsSharpnessMapMode, 1);

    uint8_t controlCaptureIntent = 0;
    switch (type) {
        case CAMERA3_TEMPLATE_PREVIEW:          controlCaptureIntent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;             break;
        case CAMERA3_TEMPLATE_STILL_CAPTURE:    controlCaptureIntent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;       break;
        case CAMERA3_TEMPLATE_VIDEO_RECORD:     controlCaptureIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;        break;
        case CAMERA3_TEMPLATE_VIDEO_SNAPSHOT:   controlCaptureIntent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;      break;
        case CAMERA3_TEMPLATE_ZERO_SHUTTER_LAG: controlCaptureIntent = ANDROID_CONTROL_CAPTURE_INTENT_ZERO_SHUTTER_LAG;    break;
        default:                                controlCaptureIntent = ANDROID_CONTROL_CAPTURE_INTENT_CUSTOM;              break;
    }
    cm.update(ANDROID_CONTROL_CAPTURE_INTENT, &controlCaptureIntent, 1);

    static const uint8_t controlMode = ANDROID_CONTROL_MODE_OFF;
    cm.update(ANDROID_CONTROL_MODE, &controlMode, 1);

    static const uint8_t controlEffectMode = ANDROID_CONTROL_EFFECT_MODE_OFF;
    cm.update(ANDROID_CONTROL_EFFECT_MODE, &controlEffectMode, 1);

    static const uint8_t controlSceneMode = ANDROID_CONTROL_SCENE_MODE_FACE_PRIORITY;
    cm.update(ANDROID_CONTROL_SCENE_MODE, &controlSceneMode, 1);

    static const uint8_t controlAeMode = ANDROID_CONTROL_AE_MODE_OFF;
    cm.update(ANDROID_CONTROL_AE_MODE, &controlAeMode, 1);

    static const uint8_t controlAeLock = ANDROID_CONTROL_AE_LOCK_OFF;
    cm.update(ANDROID_CONTROL_AE_LOCK, &controlAeLock, 1);

    static const int32_t controlAeRegions[] = {
        0,                          0,
        (int32_t)sensorSize.width,  (int32_t)sensorSize.height,
        1000
    };
    cm.update(ANDROID_CONTROL_AE_REGIONS, controlAeRegions, NELEM(controlAeRegions));
    cm.update(ANDROID_CONTROL_AWB_REGIONS, controlAeRegions, NELEM(controlAeRegions));
    cm.update(ANDROID_CONTROL_AF_REGIONS, controlAeRegions, NELEM(controlAeRegions));

    static const int32_t controlAeExposureCompensation = 0;
    cm.update(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &controlAeExposureCompensation, 1);

    static const int32_t controlAeTargetFpsRange[] = {
        10, 60
    };
    cm.update(ANDROID_CONTROL_AE_TARGET_FPS_RANGE, controlAeTargetFpsRange, NELEM(controlAeTargetFpsRange));

    static const uint8_t controlAeAntibandingMode = ANDROID_CONTROL_AE_ANTIBANDING_MODE_OFF;
    cm.update(ANDROID_CONTROL_AE_ANTIBANDING_MODE, &controlAeAntibandingMode, 1);

    static const uint8_t controlAwbMode = ANDROID_CONTROL_AWB_MODE_OFF;
    cm.update(ANDROID_CONTROL_AWB_MODE, &controlAwbMode, 1);

    static const uint8_t controlAwbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
    cm.update(ANDROID_CONTROL_AWB_LOCK, &controlAwbLock, 1);

    uint8_t controlAfMode = ANDROID_CONTROL_AF_MODE_OFF;
    cm.update(ANDROID_CONTROL_AF_MODE, &controlAfMode, 1);

    static const uint8_t controlAeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
    cm.update(ANDROID_CONTROL_AE_STATE, &controlAeState, 1);
    static const uint8_t controlAfState = ANDROID_CONTROL_AF_STATE_INACTIVE;
    cm.update(ANDROID_CONTROL_AF_STATE, &controlAfState, 1);
    static const uint8_t controlAwbState = ANDROID_CONTROL_AWB_STATE_INACTIVE;
    cm.update(ANDROID_CONTROL_AWB_STATE, &controlAwbState, 1);

    static const uint8_t controlVideoStabilizationMode = ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF;
    cm.update(ANDROID_CONTROL_VIDEO_STABILIZATION_MODE, &controlVideoStabilizationMode, 1);

    static const int32_t controlAePrecaptureId = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
    cm.update(ANDROID_CONTROL_AE_PRECAPTURE_ID, &controlAePrecaptureId, 1);

    static const int32_t controlAfTriggerId = 0;
    cm.update(ANDROID_CONTROL_AF_TRIGGER_ID, &controlAfTriggerId, 1);

    mDefaultRequestSettings[type] = cm.release();
    return mDefaultRequestSettings[type];
}

int Camera::configureStreams(camera3_stream_configuration_t *streamList) {
    DBGUTILS_AUTOLOGCALL(__func__);
    Mutex::Autolock lock(mMutex);

    /* TODO: sanity checks */

    ALOGV("+-------------------------------------------------------------------------------");
    ALOGV("| STREAMS FROM FRAMEWORK");
    ALOGV("+-------------------------------------------------------------------------------");
    for(size_t i = 0; i < streamList->num_streams; ++i) {
        camera3_stream_t *newStream = streamList->streams[i];
        ALOGV("| p=%p  fmt=0x%.2x  type=%u  usage=0x%.8x  size=%4ux%-4u  buf_no=%u",
              newStream,
              newStream->format,
              newStream->stream_type,
              newStream->usage,
              newStream->width,
              newStream->height,
              newStream->max_buffers);
    }
    ALOGV("+-------------------------------------------------------------------------------");

    /* TODO: do we need input stream? */
    camera3_stream_t *inStream = NULL;
    unsigned width = 0;
    unsigned height = 0;
    for(size_t i = 0; i < streamList->num_streams; ++i) {
        camera3_stream_t *newStream = streamList->streams[i];

        /* TODO: validate: null */

        if(newStream->stream_type == CAMERA3_STREAM_INPUT || newStream->stream_type == CAMERA3_STREAM_BIDIRECTIONAL) {
            if(inStream) {
                ALOGE("Only one input/bidirectional stream allowed (previous is %p, this %p)", inStream, newStream);
                return BAD_VALUE;
            }
            inStream = newStream;
        }

        /* TODO: validate format */

        if(newStream->format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
            newStream->format = HAL_PIXEL_FORMAT_RGBA_8888;
        }

        /* TODO: support ZSL */
        if(newStream->usage & GRALLOC_USAGE_HW_CAMERA_ZSL) {
            ALOGE("ZSL STREAM FOUND! It is not supported for now.");
            ALOGE("    Disable it by placing following line in /system/build.prop:");
            ALOGE("    camera.disable_zsl_mode=1");
            return BAD_VALUE;
        }

        switch(newStream->stream_type) {
            case CAMERA3_STREAM_OUTPUT:         newStream->usage = GRALLOC_USAGE_SW_WRITE_OFTEN;                                break;
            case CAMERA3_STREAM_INPUT:          newStream->usage = GRALLOC_USAGE_SW_READ_OFTEN;                                 break;
            case CAMERA3_STREAM_BIDIRECTIONAL:  newStream->usage = GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN;  break;
        }
        newStream->max_buffers = 1; /* TODO: support larger queue */

        if(newStream->width * newStream->height > width * height) {
            width = newStream->width;
            height = newStream->height;
        }

        /* TODO: store stream pointers somewhere and configure only new ones */
    }

    if(!mDev->setStreaming(false)) {
        ALOGE("Could not stop streaming");
        return NO_INIT;
    }
    if(!mDev->setResolution(width, height)) {
        ALOGE("Could not set resolution");
        return NO_INIT;
    }

    ALOGV("+-------------------------------------------------------------------------------");
    ALOGV("| STREAMS AFTER CHANGES");
    ALOGV("+-------------------------------------------------------------------------------");
    for(size_t i = 0; i < streamList->num_streams; ++i) {
        const camera3_stream_t *newStream = streamList->streams[i];
        ALOGV("| p=%p  fmt=0x%.2x  type=%u  usage=0x%.8x  size=%4ux%-4u  buf_no=%u",
              newStream,
              newStream->format,
              newStream->stream_type,
              newStream->usage,
              newStream->width,
              newStream->height,
              newStream->max_buffers);
    }
    ALOGV("+-------------------------------------------------------------------------------");

    if(!mDev->setStreaming(true)) {
        ALOGE("Could not start streaming");
        return NO_INIT;
    }

    return NO_ERROR;
}

int Camera::registerStreamBuffers(const camera3_stream_buffer_set_t *bufferSet) {
    DBGUTILS_AUTOLOGCALL(__func__);
    Mutex::Autolock lock(mMutex);
    ALOGV("+-------------------------------------------------------------------------------");
    ALOGV("| BUFFERS FOR STREAM %p", bufferSet->stream);
    ALOGV("+-------------------------------------------------------------------------------");
    for (size_t i = 0; i < bufferSet->num_buffers; ++i) {
        ALOGV("| p=%p", bufferSet->buffers[i]);
    }
    ALOGV("+-------------------------------------------------------------------------------");

    return OK;
}

int Camera::processCaptureRequest(camera3_capture_request_t *request) {
    assert(request != NULL);
    Mutex::Autolock lock(mMutex);

    BENCHMARK_HERE(120);
    FPSCOUNTER_HERE(120);

    CameraMetadata cm;
    const V4l2Device::VBuffer *frame = NULL;
    auto res = mDev->resolution();
    status_t e;
    Vector<camera3_stream_buffer> buffers;

    auto timestamp = systemTime();

    ALOGV("--- capture request --- f=%-5u in_buf=%p  out_bufs=%p[%u] --- fps %4.1f (avg %4.1f)",
          request->frame_number,
          request->input_buffer,
          request->output_buffers,
          request->num_output_buffers,
          FPSCOUNTER_VALUE(1), FPSCOUNTER_VALUE());

    if(request->settings == NULL && mLastRequestSettings.isEmpty()) {
        ALOGE("First request does not have metadata");
        return BAD_VALUE;
    }

    if(request->input_buffer) {
        /* Ignore input buffer */
        /* TODO: do we expect any input buffer? */
        request->input_buffer->release_fence = -1;
    }

    if(!request->settings) {
        cm.acquire(mLastRequestSettings);
    } else {
        cm = request->settings;
    }

    notifyShutter(request->frame_number, (uint64_t)timestamp);

    BENCHMARK_SECTION("Lock/Read") {
        frame = mDev->readLock();
    }

    if(!frame) {
        return NOT_ENOUGH_DATA;
    }

    buffers.setCapacity(request->num_output_buffers);

    uint8_t *rgbaBuffer = NULL;
    for(size_t i = 0; i < request->num_output_buffers; ++i) {
        const camera3_stream_buffer &srcBuf = request->output_buffers[i];
        uint8_t *buf = NULL;

        sp<Fence> acquireFence = new Fence(srcBuf.acquire_fence);
        e = acquireFence->wait(1000); /* FIXME: magic number */
        if(e == TIMED_OUT) {
            ALOGE("buffer %p  frame %-4u  Wait on acquire fence timed out", srcBuf.buffer, request->frame_number);
        }
        if(e == NO_ERROR) {
            const Rect rect((int)srcBuf.stream->width, (int)srcBuf.stream->height);
            e = GraphicBufferMapper::get().lock(*srcBuf.buffer, GRALLOC_USAGE_SW_WRITE_OFTEN, rect, (void **)&buf);
            if(e != NO_ERROR) {
                ALOGE("buffer %p  frame %-4u  lock failed", srcBuf.buffer, request->frame_number);
            }
        }
        if(e != NO_ERROR) {
            do GraphicBufferMapper::get().unlock(*request->output_buffers[i].buffer); while(i--);
            return NO_INIT;
        }

        switch(srcBuf.stream->format) {
            case HAL_PIXEL_FORMAT_RGBA_8888: {
                if(!rgbaBuffer) {
                    BENCHMARK_SECTION("YUV->RGBA") {
                        /* FIXME: better format detection */
                        if(frame->pixFmt == V4L2_PIX_FMT_UYVY)
                            mConverter.UYVYToRGBA(frame->buf, buf, res.width, res.height);
                        else
                            mConverter.YUY2ToRGBA(frame->buf, buf, res.width, res.height);
                        rgbaBuffer = buf;
                    }
                } else {
                    BENCHMARK_SECTION("Buf Copy") {
                        memcpy(buf, rgbaBuffer, srcBuf.stream->width * srcBuf.stream->height * 4);
                    }
                }
                break;
            }
            case HAL_PIXEL_FORMAT_BLOB: {
                BENCHMARK_SECTION("YUV->JPEG") {
                    const size_t maxImageSize = mJpegBufferSize - sizeof(camera3_jpeg_blob);
                    uint8_t jpegQuality = 95;
                    if(cm.exists(ANDROID_JPEG_QUALITY)) {
                        jpegQuality = *cm.find(ANDROID_JPEG_QUALITY).data.u8;
                    }
                    ALOGD("JPEG quality = %u", jpegQuality);

                    /* FIXME: better format detection */
                    uint8_t *bufEnd = NULL;
                    if(frame->pixFmt == V4L2_PIX_FMT_UYVY)
                        bufEnd = mConverter.UYVYToJPEG(frame->buf, buf, res.width, res.height, maxImageSize, jpegQuality);
                    else
                        bufEnd = mConverter.YUY2ToJPEG(frame->buf, buf, res.width, res.height, maxImageSize, jpegQuality);

                    if(bufEnd != buf) {
                        camera3_jpeg_blob *jpegBlob = reinterpret_cast<camera3_jpeg_blob*>(buf + maxImageSize);
                        jpegBlob->jpeg_blob_id  = CAMERA3_JPEG_BLOB_ID;
                        jpegBlob->jpeg_size     = (uint32_t)(bufEnd - buf);
                    } else {
                        ALOGE("%s: JPEG image too big!", __FUNCTION__);
                    }
                }
                break;
            }
            default:
                ALOGE("Unknown pixel format %d in buffer %p (stream %p), ignoring", srcBuf.stream->format, srcBuf.buffer, srcBuf.stream);
        }
    }

    /* Unlocking all buffers in separate loop allows to copy data from already processed buffer to not yet processed one */
    for(size_t i = 0; i < request->num_output_buffers; ++i) {
        const camera3_stream_buffer &srcBuf = request->output_buffers[i];

        GraphicBufferMapper::get().unlock(*srcBuf.buffer);
        buffers.push_back(srcBuf);
        buffers.editTop().acquire_fence = -1;
        buffers.editTop().release_fence = -1;
        buffers.editTop().status = CAMERA3_BUFFER_STATUS_OK;
    }

    BENCHMARK_SECTION("Unlock") {
        mDev->unlock(frame);
    }

    int64_t sensorTimestamp = timestamp;
    int64_t syncFrameNumber = request->frame_number;

    cm.update(ANDROID_SENSOR_TIMESTAMP, &sensorTimestamp, 1);
    cm.update(ANDROID_SYNC_FRAME_NUMBER, &syncFrameNumber, 1);

    auto result = cm.getAndLock();
    processCaptureResult(request->frame_number, result, buffers);
    cm.unlock(result);

    // Cache the settings for next time
    mLastRequestSettings.acquire(cm);

    /* Print stats */
    char bmOut[1024];
    BENCHMARK_STRING(bmOut, sizeof(bmOut), 6);
    ALOGV("    time (avg):  %s", bmOut);

    return NO_ERROR;
}

inline void Camera::notifyShutter(uint32_t frameNumber, uint64_t timestamp) {
    camera3_notify_msg_t msg;
    msg.type = CAMERA3_MSG_SHUTTER;
    msg.message.shutter.frame_number = frameNumber;
    msg.message.shutter.timestamp = timestamp;
    mCallbackOps->notify(mCallbackOps, &msg);
}

void Camera::processCaptureResult(uint32_t frameNumber, const camera_metadata_t *result, const Vector<camera3_stream_buffer> &buffers) {
    camera3_capture_result captureResult;
    captureResult.frame_number = frameNumber;
    captureResult.result = result;
    captureResult.num_output_buffers = buffers.size();
    captureResult.output_buffers = buffers.array();
    captureResult.input_buffer = NULL;
    captureResult.partial_result = 0;

    mCallbackOps->process_capture_result(mCallbackOps, &captureResult);
}

/******************************************************************************\
                                STATIC WRAPPERS
\******************************************************************************/

int Camera::sClose(hw_device_t *device) {
    /* TODO: check device module */
    Camera *thiz = static_cast<Camera *>(reinterpret_cast<camera3_device_t *>(device));
    return thiz->closeDevice();
}

int Camera::sInitialize(const camera3_device *device, const camera3_callback_ops_t *callback_ops) {
    /* TODO: check pointers */
    Camera *thiz = static_cast<Camera *>(const_cast<camera3_device *>(device));
    return thiz->initialize(callback_ops);
}

int Camera::sConfigureStreams(const camera3_device *device, camera3_stream_configuration_t *stream_list) {
    /* TODO: check pointers */
    Camera *thiz = static_cast<Camera *>(const_cast<camera3_device *>(device));
    return thiz->configureStreams(stream_list);
}

int Camera::sRegisterStreamBuffers(const camera3_device *device, const camera3_stream_buffer_set_t *buffer_set) {
    /* TODO: check pointers */
    Camera *thiz = static_cast<Camera *>(const_cast<camera3_device *>(device));
    return thiz->registerStreamBuffers(buffer_set);
}

const camera_metadata_t * Camera::sConstructDefaultRequestSettings(const camera3_device *device, int type) {
    /* TODO: check pointers */
    Camera *thiz = static_cast<Camera *>(const_cast<camera3_device *>(device));
    return thiz->constructDefaultRequestSettings(type);
}

int Camera::sProcessCaptureRequest(const camera3_device *device, camera3_capture_request_t *request) {
    /* TODO: check pointers */
    Camera *thiz = static_cast<Camera *>(const_cast<camera3_device *>(device));
    return thiz->processCaptureRequest(request);
}

void Camera::sGetMetadataVendorTagOps(const camera3_device *device, vendor_tag_query_ops_t *ops) {
    /* TODO: implement */
    ALOGD("%s: IMPLEMENT ME!", __FUNCTION__);
}

void Camera::sDump(const camera3_device *device, int fd) {
    /* TODO: implement */
    ALOGD("%s: IMPLEMENT ME!", __FUNCTION__);
}

int Camera::sFlush(const camera3_device *device) {
    /* TODO: implement */
    ALOGD("%s: IMPLEMENT ME!", __FUNCTION__);
    return -ENODEV;
}

camera3_device_ops_t Camera::sOps = {
    .initialize                         = Camera::sInitialize,
    .configure_streams                  = Camera::sConfigureStreams,
    .register_stream_buffers            = Camera::sRegisterStreamBuffers,
    .construct_default_request_settings = Camera::sConstructDefaultRequestSettings,
    .process_capture_request            = Camera::sProcessCaptureRequest,
    .get_metadata_vendor_tag_ops        = Camera::sGetMetadataVendorTagOps,
    .dump                               = Camera::sDump,
    .flush                              = Camera::sFlush,
    .reserved = {0}
};

}; /* namespace android */
