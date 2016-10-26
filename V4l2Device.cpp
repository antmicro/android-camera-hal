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

#define LOG_TAG "Cam-V4l2Device"
#define LOG_NDEBUG NDEBUG

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>

#include <utils/Log.h>
#include <cstring>
#include <errno.h>
#include <cstdlib>
#include <utils/misc.h>
#include <cutils/properties.h>
#include <utils/Vector.h>
#include <cassert>

#include "V4l2Device.h"

namespace android {

/******************************************************************************\
                                    Helpers
\******************************************************************************/

static inline int openFd(const char *path) {
    assert(path);
    int flags = O_RDWR;
#ifdef V4L2DEVICE_USE_POLL
    flags |= O_NONBLOCK;
#endif
    int fd = open(path, flags);
    ALOGV("open %s = %d", path, fd);
    return fd;
}

static inline void closeFd(int *fd) {
    assert(fd);
    close(*fd);
    ALOGV("close %d", *fd);
    *fd = -1;
}

/******************************************************************************\
                                   V4l2Device
\******************************************************************************/

/**
 * \class V4l2Device
 *
 * Simple wrapper for part of V4L2 camera interface.
 */

/**
 * Initializes object.
 *
 * \parameter devNode Path to V4L2 device node
 */
V4l2Device::V4l2Device(const char *devNode)
    : mFd(-1)
    , mConnected(false)
    , mStreaming(false)
    , mDevNode(devNode)
{
    memset(&mFormat, 0, sizeof(mFormat));
    mPFd.fd = -1;
    mPFd.events = POLLIN | POLLRDNORM;

#if V4L2DEVICE_FPS_LIMIT > 0
    mLastTimestamp = 0;
#endif

    /* Ignore multiple possible devices for now */
    char resStr[PROPERTY_VALUE_MAX];
    int ret;
    ret = property_get("ro.camera.v4l2device.resolution", resStr, "");
    if(ret > 0) {
        /* parse forced resolution as WIDTHxHEIGHT */
        char *heightStr = strchr(resStr, 'x');
        if(heightStr)
            *heightStr++ = '\0';

        errno = 0;
        mForcedResolution.width = strtoul(resStr, NULL, 10);
        ret = errno;
        mForcedResolution.height = strtoul(heightStr, NULL, 10);
        ret |= errno;

        if(ret) {
            mForcedResolution.width = mForcedResolution.height = 0;
        }
    }

#ifdef V4L2DEVICE_OPEN_ONCE
    connect();
#endif
}

V4l2Device::~V4l2Device() {
    if(isStreaming()) {
        iocStreamOff();
    }
    cleanup();
}

/**
 * Returns array of camera's supported resolutions.
 *
 * Resolution can be forced by setting property ro.camera.v4l2device.resolution to value WIDTHxHEIGHT (e.g. 1920x1080)
 */
const Vector<V4l2Device::Resolution> & V4l2Device::availableResolutions() {
    if(!mAvailableResolutions.isEmpty()) {
        return mAvailableResolutions;
    }

    if(mForcedResolution.width > 0 && mForcedResolution.height > 0) {
        ALOGI("Using forced resolution: %ux%u", mForcedResolution.width, mForcedResolution.height);
        mAvailableResolutions.add(mForcedResolution);
    } else {
        int fd;
        bool fdNeedsClose = false;
        Vector<V4l2Device::Resolution> formats;

        if(mFd >= 0) {
            fd = mFd;
        } else {
            fd = openFd(mDevNode);
            fdNeedsClose = true;
        }
        if(fd < 0) {
            ALOGE("Could not open %s: %s (%d)", mDevNode, strerror(errno), errno);
            return mAvailableResolutions;
        }

        struct v4l2_frmsizeenum frmSize;
        frmSize.pixel_format = V4L2DEVICE_PIXEL_FORMAT;
        frmSize.index = 0;

        errno = 0;
        while(ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmSize) == 0) {
            ALOGD("%s: Found resolution: %dx%d", mDevNode, frmSize.discrete.width, frmSize.discrete.height);
            ++frmSize.index;
            // FIXME: make it configurable or fix the out of memory problem
            if(frmSize.discrete.width > 1920 || frmSize.discrete.height > 1080) {
                ALOGD("    too big, ignoring");
                continue;
            }
            formats.add();
            formats.editTop().width = frmSize.discrete.width;
            formats.editTop().height = frmSize.discrete.height;
        }
        if(errno && errno != EINVAL) {
            ALOGW("Get available formats: %s (%d)", strerror(errno), errno);
        }

        if(fdNeedsClose) {
            closeFd(&fd);
        }

        mAvailableResolutions = formats;
    }

    return mAvailableResolutions;
}

/**
 * Returns V4l2Device::Resolution with highest possible width and highest
 * possible height. This might not to be valid camera resolution.
 */
V4l2Device::Resolution V4l2Device::sensorResolution() {
    const Vector<V4l2Device::Resolution> &formats = availableResolutions();
    V4l2Device::Resolution max = {0, 0};
    for(size_t i = 0; i < formats.size(); ++i) {
        if(formats[i].width > max.width)
            max.width = formats[i].width;
        if(formats[i].height > max.height)
            max.height = formats[i].height;
    }
    return max;
}

/**
 * Sets new resolution. The resolution must be supported by camera. If it does
 * not, false is returned. Call only with disabled streaming.
 */
bool V4l2Device::setResolution(unsigned width, unsigned height) {
    if(mFormat.fmt.pix.width == width && mFormat.fmt.pix.height == height)
        return true;

    ALOGD("New resolution: %dx%d", width, height);
    if(isConnected()) {
        #ifndef V4L2DEVICE_OPEN_ONCE
        disconnect();
        mFormat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        mFormat.fmt.pix.pixelformat = V4L2DEVICE_PIXEL_FORMAT;
        mFormat.fmt.pix.width = width;
        mFormat.fmt.pix.height = height;
        connect();
        #else
        ALOGD("Resolution change not supported");
        #endif
        return true;
    } else {
        mFormat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        mFormat.fmt.pix.pixelformat = V4L2DEVICE_PIXEL_FORMAT;
        mFormat.fmt.pix.width = width;
        mFormat.fmt.pix.height = height;
        return true;
    }
}

/**
 * Returns current resolution
 */
V4l2Device::Resolution V4l2Device::resolution() {
    Resolution res;
    res.width = mFormat.fmt.pix.width;
    res.height = mFormat.fmt.pix.height;
    return res;
}

/**
 * Connects to camera, allocates buffers, starts streaming
 */
bool V4l2Device::connect() {
    if(isConnected())
        return false;

    mFd = openFd(mDevNode);
    if(mFd < 0) {
        ALOGE("Could not open %s: %s (%d)", mDevNode, strerror(errno), errno);
        return false;
    }

    unsigned width;
    unsigned height;
    if(mFormat.type) {
        width = mFormat.fmt.pix.width;
        height = mFormat.fmt.pix.height;
    } else {
        auto resolutions = availableResolutions();
        if(resolutions.isEmpty()) {
            ALOGE("No available resolutions found, aborting");
            closeFd(&mFd);
            return false;
        }
        auto defaultRes = resolutions[0];
        width = resolutions[0].width;
        height = resolutions[0].height;
        ALOGD("Using default resolution: %dx%d", defaultRes.width, defaultRes.height);
    }
    if(!setResolutionAndAllocateBuffers(width, height)) {
        ALOGE("Could not set resolution");
        closeFd(&mFd);
        return false;
    }

    mPFd.fd = mFd;
    mPFd.revents = 0;
    mConnected = true;

    return true;
}

/**
 * Stops streaming and disconnects from camera device.
 */
bool V4l2Device::disconnect() {
    if(!isConnected())
        return false;

    setStreaming(false);
#ifndef V4L2DEVICE_OPEN_ONCE
    cleanup();
#endif

    return true;
}

bool V4l2Device::setStreaming(bool enable) {
    if(enable == mStreaming)
        return true;

    if(!isConnected())
        return !enable;

    if(enable) {
        if(!iocStreamOn()) {
            ALOGE("Could not start streaming: %s (%d)", strerror(errno), errno);
            return false;
        }
    } else {
#ifdef V4L2DEVICE_OPEN_ONCE
        return true;
#else
        if(!iocStreamOff()) {
            ALOGE("Could not stop streaming: %s (%d)", strerror(errno), errno);
            return false;
        }
#endif
    }

    mStreaming = enable;

    return true;
}

/**
 * Lock buffer and return pointer to it. After processing buffer must be
 * unlocked with V4l2Device::unlock().
 */
const V4l2Device::VBuffer * V4l2Device::readLock() {
    assert(isConnected());
    assert(isStreaming());
    int id = 0;
    if((id = dequeueBuffer()) < 0) {
        ALOGE("Could not dequeue buffer: %s (%d)", strerror(errno), errno);
        return NULL;
    }
    auto buf = &mBuf[id];
    return buf;
}

/**
 * Unlocks previously locked buffer.
 */
bool V4l2Device::unlock(const VBuffer *buf) {
    if(!buf)
        return false;

    for(unsigned i = 0; i < NELEM(mBuf); ++i) {
        if(mBuf[i].buf == buf->buf) {
            if(!queueBuffer(i)) {
                ALOGE("Could not queue buffer %d: %s (%d)", i, strerror(errno), errno);
                return false;
            }
            return true;
        }
    }
    return false;
}

/**
 * Returns buffer with specified ID to the kernel
 */
bool V4l2Device::queueBuffer(unsigned id) {
    assert(mFd >= 0);

    struct v4l2_buffer bufInfo;
    memset(&bufInfo, 0, sizeof(bufInfo));
    bufInfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufInfo.memory = V4L2_MEMORY_MMAP;
    bufInfo.index = id;

    if(ioctl(mFd, VIDIOC_QBUF, &bufInfo) < 0)
        return false;

    return true;
}

/**
 * Dequeues next available buffer and returns its ID.
 */
int V4l2Device::dequeueBuffer() {
    assert(mFd >= 0);

    struct v4l2_buffer bufInfo;

    memset(&bufInfo, 0, sizeof(bufInfo));
    bufInfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufInfo.memory = V4L2_MEMORY_MMAP;
    bufInfo.index = 0;

#if V4L2DEVICE_FPS_LIMIT > 0
    auto timestamp = systemTime();
    nsecs_t extraTime = 1000000000LL / V4L2DEVICE_FPS_LIMIT - (timestamp - mLastTimestamp);
    if(extraTime / 1000 > 0)
        usleep((unsigned)(extraTime / 1000));
    mLastTimestamp = systemTime();
#endif

    do {
#ifdef V4L2DEVICE_USE_POLL
        if((errno = 0, poll(&mPFd, 1, 5000)) <= 0) {
            errno = ETIME;
            return -1;
        }
#endif
    } while((errno = 0, ioctl(mFd, VIDIOC_DQBUF, &bufInfo)) < 0 && (errno == EINVAL || errno == EAGAIN));
    if(errno)
        return -1;

    return (int)bufInfo.index;
}

bool V4l2Device::iocStreamOff() {
    assert(mFd >= 0);
    assert(mFormat.type);

    errno = 0;
    unsigned type = mFormat.type;
    if(ioctl(mFd, VIDIOC_STREAMOFF, &type) == 0) {
        mStreaming = false;
    } else {
        ALOGV("%s: %s (%d)", __FUNCTION__, strerror(errno), errno);
    }
    return !errno;
}

bool V4l2Device::iocStreamOn() {
    assert(mFd >= 0);
    assert(mFormat.type);

    errno = 0;
    unsigned type = mFormat.type;
    if(ioctl(mFd, VIDIOC_STREAMON, &type) == 0) {
        mStreaming = true;
    } else {
        ALOGV("%s: %s (%d)", __FUNCTION__, strerror(errno), errno);
    }
    return !errno;
}

bool V4l2Device::iocSFmt(unsigned width, unsigned height) {
    assert(mFd >= 0);
    assert(!mStreaming);

    struct v4l2_format format;
    memset(&format, 0, sizeof(format));

    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.pixelformat = V4L2DEVICE_PIXEL_FORMAT;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;

    errno = 0;
    if(ioctl(mFd, VIDIOC_S_FMT, &format) == 0) {
        mFormat = format;
    } else {
        ALOGV("%s(w=%u, h=%u): %s (%d)", __FUNCTION__, width, height, strerror(errno), errno);
    }

    return !errno;
}

bool V4l2Device::iocReqBufs(unsigned *count) {
    assert(mFd >= 0);
    assert(count);

    struct v4l2_requestbuffers bufRequest;
    memset(&bufRequest, 0, sizeof(bufRequest));

    bufRequest.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufRequest.memory = V4L2_MEMORY_MMAP;
    bufRequest.count = *count;

    errno = 0;
    if(ioctl(mFd, VIDIOC_REQBUFS, &bufRequest) == 0) {
        *count = bufRequest.count;
    } else {
        ALOGV("%s(count=%u): %s (%d)", __FUNCTION__, *count, strerror(errno), errno);
    }

    return !errno;
}

bool V4l2Device::iocQueryBuf(unsigned id, unsigned *offset, unsigned *len) {
    assert(mFd >= 0);
    assert(offset);
    assert(len);

    struct v4l2_buffer bufInfo;
    memset(&bufInfo, 0, sizeof(bufInfo));

    bufInfo.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bufInfo.memory = V4L2_MEMORY_MMAP;
    bufInfo.index = id;

    errno = 0;
    if(ioctl(mFd, VIDIOC_QUERYBUF, &bufInfo) == 0) {
        *offset = bufInfo.m.offset;
        *len = bufInfo.length;
    } else {
        ALOGV("%s(id=%u): %s (%d)", __FUNCTION__, id, strerror(errno), errno);
    }

    return !errno;
}

bool V4l2Device::setResolutionAndAllocateBuffers(unsigned width, unsigned height) {
    assert(!mStreaming);

    for(int i = 0; i < V4L2DEVICE_BUF_COUNT; ++i) {
        mBuf[i].unmap();
    }

    if(!iocSFmt(width, height)) {
        ALOGE("Could not set pixel format to %dx%d: %s (%d)", width, height, strerror(errno), errno);
        return false;
    }

    unsigned bufCount = V4L2DEVICE_BUF_COUNT;
    if(!iocReqBufs(&bufCount)) {
        ALOGE("Could not request buffer: %s (%d)", strerror(errno), errno);
        return false;
    }

    unsigned bufLen[V4L2DEVICE_BUF_COUNT] = {0};

    for(unsigned i = 0; i < bufCount; ++i) {
        unsigned offset;
        if(!iocQueryBuf(i, &offset, &bufLen[i])) {
            ALOGE("Could not query buffer %d: %s (%d)", i, strerror(errno), errno);
            return false;
        }

        if(!mBuf[i].map(mFd, offset, bufLen[i])) {
            ALOGE("Could not allocate buffer %d (len = %d): %s (%d)", i, bufLen[i], strerror(errno), errno);
            while(i--) mBuf[i].unmap();
            return false;
        }

        if(!queueBuffer(i)) {
            ALOGE("Could not queue buffer: %s (%d)", strerror(errno), errno);
            do mBuf[i].unmap(); while(i--);
            return false;
        }
    }

    return true;
}

void V4l2Device::cleanup() {
    for(int i = 0; i < V4L2DEVICE_BUF_COUNT; ++i) {
        mBuf[i].unmap();
    }

    closeFd(&mFd);
    mPFd.fd = -1;
    mConnected = false;
}

/******************************************************************************\
                              V4l2Device::VBuffer
\******************************************************************************/

/**
 * \class V4l2Device::VBuffer
 *
 * Video buffer abstraction.
 */

V4l2Device::VBuffer::~VBuffer() {
    if(buf) {
        ALOGD("V4l2Device::VBuffer: Memory leak!");
        abort();
    }
}

bool V4l2Device::VBuffer::map(int fd, unsigned offset, unsigned len) {
    assert(!this->buf);

    errno = 0;
    this->buf = (uint8_t*)mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if(this->buf == MAP_FAILED) {
        return false;
    }
    memset(this->buf, 0, len);
    this->len = len;
    this->pixFmt = V4L2DEVICE_PIXEL_FORMAT;

    return true;
}

void V4l2Device::VBuffer::unmap() {
    if(buf) {
        munmap(buf, len);
        buf         = NULL;
        len         = 0;
    }
}

}; /* namespace android */
