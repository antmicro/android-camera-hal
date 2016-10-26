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

#define LOG_TAG "Cam-HalModule"

#include <hardware/camera_common.h>
#include <cutils/log.h>
#include <utils/misc.h>
#include <cstdlib>
#include <cassert>

#include "Camera.h"

/******************************************************************************\
                                  DECLARATIONS
      Not used in any other project source files, header file is redundant
\******************************************************************************/

extern camera_module_t HAL_MODULE_INFO_SYM;

namespace android {
namespace HalModule {

/* Available cameras */
extern Camera *cams[];

static int getNumberOfCameras();
static int getCameraInfo(int cameraId, struct camera_info *info);
static int setCallbacks(const camera_module_callbacks_t *callbacks);
static void getVendorTagOps(vendor_tag_ops_t* ops);
static int openDevice(const hw_module_t *module, const char *name, hw_device_t **device);

static struct hw_module_methods_t moduleMethods = {
    .open = openDevice
};

}; /* namespace HalModule */
}; /* namespace android */

/******************************************************************************\
                                  DEFINITIONS
\******************************************************************************/

camera_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag                = HARDWARE_MODULE_TAG,
        .module_api_version = CAMERA_MODULE_API_VERSION_2_3,
        .hal_api_version    = HARDWARE_HAL_API_VERSION,
        .id                 = CAMERA_HARDWARE_MODULE_ID,
        .name               = "V4l2 Camera",
        .author             = "Antmicro Ltd.",
        .methods            = &android::HalModule::moduleMethods,
        .dso                = NULL,
        .reserved           = {0}
    },
    .get_number_of_cameras  = android::HalModule::getNumberOfCameras,
    .get_camera_info        = android::HalModule::getCameraInfo,
    .set_callbacks          = android::HalModule::setCallbacks,
};

namespace android {
namespace HalModule {

static Camera mainCamera;
Camera *cams[] = {
    &mainCamera
};

static int getNumberOfCameras() {
    return NELEM(cams);
};

static int getCameraInfo(int cameraId, struct camera_info *info) {
    if(cameraId < 0 || cameraId >= getNumberOfCameras()) {
        ALOGE("%s: invalid camera ID (%d)", __FUNCTION__, cameraId);
        return -ENODEV;
    }
    if(!cams[cameraId]->isValid()) {
        ALOGE("%s: camera %d is not initialized", __FUNCTION__, cameraId);
        return -ENODEV;
    }
    return cams[cameraId]->cameraInfo(info);
}

int setCallbacks(const camera_module_callbacks_t * /*callbacks*/) {
    /* TODO: Implement for hotplug support */
    return OK;
}

static int openDevice(const hw_module_t *module, const char *name, hw_device_t **device) {
    if (module != &HAL_MODULE_INFO_SYM.common) {
        ALOGE("%s: invalid module (%p != %p)", __FUNCTION__, module, &HAL_MODULE_INFO_SYM.common);
        return -EINVAL;
    }
    if (name == NULL) {
        ALOGE("%s: NULL name", __FUNCTION__);
        return -EINVAL;
    }
    errno = 0;
    int cameraId = (int)strtol(name, NULL, 10);
    if(errno || cameraId < 0 || cameraId >= getNumberOfCameras()) {
        ALOGE("%s: invalid camera ID (%s)", __FUNCTION__, name);
        return -EINVAL;
    }
    if(!cams[cameraId]->isValid()) {
        ALOGE("%s: camera %d is not initialized", __FUNCTION__, cameraId);
        *device = NULL;
        return -ENODEV;
    }

    return cams[cameraId]->openDevice(device);
}

}; /* namespace HalModule */
}; /* namespace android */
