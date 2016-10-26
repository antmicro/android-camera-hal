#ifndef V4L2DEVICE_H
#define V4L2DEVICE_H

#include <linux/videodev2.h>
#include <sys/poll.h>
#include <stdint.h>
#include <utils/Vector.h>
#include <utils/Log.h>
#include <utils/Mutex.h>
#include <utils/Timers.h>

#ifndef V4L2DEVICE_BUF_COUNT
# define V4L2DEVICE_BUF_COUNT 4
#endif

#ifndef V4L2DEVICE_PIXEL_FORMAT
# warning V4L2DEVICE_PIXEL_FORMAT not defined, using default value (V4L2_PIX_FMT_UYVY)
# define V4L2DEVICE_PIXEL_FORMAT V4L2_PIX_FMT_UYVY
#endif

namespace android {

class V4l2Device
{
public:
    struct Resolution {
        unsigned width;
        unsigned height;
    };

    class VBuffer {
    public:
        uint8_t *buf;
        uint32_t len;
        uint32_t pixFmt;

    private:
        VBuffer(): buf(NULL), len(0) {}
        ~VBuffer();

        bool map(int fd, unsigned offset, unsigned len);
        void unmap();

        friend class V4l2Device;
    };

    V4l2Device(const char *devNode = "/dev/video0");
    ~V4l2Device();

    const Vector<V4l2Device::Resolution> & availableResolutions();
    V4l2Device::Resolution sensorResolution();

    bool setResolution(unsigned width, unsigned height);
    V4l2Device::Resolution resolution();

    bool connect();
    bool disconnect();
    bool isConnected() const { return mFd >= 0; }

    bool setStreaming(bool enable);
    bool isStreaming() const { return mStreaming; }

    const VBuffer * readLock();
    bool unlock(const VBuffer *buf);

private:
    bool queueBuffer(unsigned id);
    int dequeueBuffer();

    bool iocStreamOff();
    bool iocStreamOn();
    bool iocSFmt(unsigned width, unsigned height);
    bool iocReqBufs(unsigned *count);
    bool iocQueryBuf(unsigned id, unsigned *offset, unsigned *len);

    bool setResolutionAndAllocateBuffers(unsigned width, unsigned height);
    void cleanup();

    int mFd;
    bool mConnected;
    bool mStreaming;
    const char *mDevNode;
    Vector<V4l2Device::Resolution> mAvailableResolutions;
    V4l2Device::Resolution mForcedResolution;
    struct v4l2_format mFormat;
    VBuffer mBuf[V4L2DEVICE_BUF_COUNT];
    struct pollfd mPFd;

#if V4L2DEVICE_FPS_LIMIT > 0
    nsecs_t mLastTimestamp;
#endif
};

}; /* namespace android */

#endif // V4L2DEVICE_H
