LOCAL_PATH := $(call my-dir)

#-----------------------------------------------------------------------------
# Camera HAL module
#-----------------------------------------------------------------------------

include $(CLEAR_VARS)

LOCAL_MODULE := camera.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE_RELATIVE_PATH := hw

LOCAL_CFLAGS += -std=c++11
LOCAL_CFLAGS += -fno-short-enums
LOCAL_CFLAGS += -Wno-unused-parameter -Wno-missing-field-initializers
LOCAL_CFLAGS += -pthread

LOCAL_CFLAGS += -DV4L2DEVICE_FPS_LIMIT=60

LOCAL_CFLAGS += -DV4L2DEVICE_BUF_COUNT=4

# Camera color format
LOCAL_CFLAGS += -DV4L2DEVICE_PIXEL_FORMAT=V4L2_PIX_FMT_UYVY
#LOCAL_CFLAGS += -DV4L2DEVICE_PIXEL_FORMAT=V4L2_PIX_FMT_YUYV

# Configure and open device once on HAL start
LOCAL_CFLAGS += -DV4L2DEVICE_OPEN_ONCE

LOCAL_CFLAGS += -DV4L2DEVICE_USE_POLL


# Compile debug code - comment out to disable
#LOCAL_CFLAGS += -UNDEBUG -DDEBUG


LOCAL_STATIC_LIBRARIES := \
    libyuv_static

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libutils \
    libcutils \
    libcamera_client \
    libui \
    libjpeg \
    libcamera_metadata \
    libskia \
    libandroid_runtime

LOCAL_C_INCLUDES += \
    external/jpeg \
    external/libyuv/files/include \
    frameworks/native/include/media/hardware \
    $(call include-path-for, camera)

LOCAL_C_INCLUDES += \
    external/skia/include/core/ \
    frameworks/base/core/jni/android/graphics \
    frameworks/native/include

LOCAL_SRC_FILES += \
    HalModule.cpp \
    Camera.cpp \
    V4l2Device.cpp \
    ImageConverter.cpp \
    Workers.cpp \
    Yuv422UyvyToJpegEncoder.cpp

include $(BUILD_SHARED_LIBRARY)

#-----------------------------------------------------------------------------
# media_profiles.xml
#-----------------------------------------------------------------------------

include $(CLEAR_VARS)

LOCAL_MODULE := media_profiles.xml
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_ETC)
LOCAL_SRC_FILES := media_profiles.xml

include $(BUILD_PREBUILT)

