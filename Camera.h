#ifndef CAMERA_H
#define CAMERA_H

#include <utils/Errors.h>
#include <hardware/camera_common.h>
#include <V4l2Device.h>
#include <hardware/camera3.h>
#include <camera/CameraMetadata.h>
#include <utils/Mutex.h>

#include "Workers.h"
#include "ImageConverter.h"
#include "DbgUtils.h"

namespace android {

class Camera: public camera3_device {
public:
    Camera();
    virtual ~Camera();

    bool isValid() { return mValid; }

    virtual status_t cameraInfo(struct camera_info *info);

    virtual int openDevice(hw_device_t **device);
    virtual int closeDevice();


protected:
    virtual camera_metadata_t * staticCharacteristics();
    virtual int initialize(const camera3_callback_ops_t *callbackOps);
    virtual int configureStreams(camera3_stream_configuration_t *streamList);
    virtual const camera_metadata_t * constructDefaultRequestSettings(int type);
    virtual int registerStreamBuffers(const camera3_stream_buffer_set_t *bufferSet);
    virtual int processCaptureRequest(camera3_capture_request_t *request);

    /* HELPERS/SUBPROCEDURES */

    void notifyShutter(uint32_t frameNumber, uint64_t timestamp);
    void processCaptureResult(uint32_t frameNumber, const camera_metadata_t *result, const Vector<camera3_stream_buffer> &buffers);

    camera_metadata_t *mStaticCharacteristics;
    camera_metadata_t *mDefaultRequestSettings[CAMERA3_TEMPLATE_COUNT];
    CameraMetadata mLastRequestSettings;

    V4l2Device *mDev;
    bool mValid;
    const camera3_callback_ops_t *mCallbackOps;

    size_t mJpegBufferSize;

private:
    ImageConverter mConverter;
    Mutex mMutex;

    /* STATIC WRAPPERS */

    static int sClose(hw_device_t *device);
    static int sInitialize(const struct camera3_device *device, const camera3_callback_ops_t *callback_ops);
    static int sConfigureStreams(const struct camera3_device *device, camera3_stream_configuration_t *stream_list);
    static int sRegisterStreamBuffers(const struct camera3_device *device, const camera3_stream_buffer_set_t *buffer_set);
    static const camera_metadata_t * sConstructDefaultRequestSettings(const struct camera3_device *device, int type);
    static int sProcessCaptureRequest(const struct camera3_device *device, camera3_capture_request_t *request);
    static void sGetMetadataVendorTagOps(const struct camera3_device *device, vendor_tag_query_ops_t* ops);
    static void sDump(const struct camera3_device *device, int fd);
    static int sFlush(const struct camera3_device *device);

    static camera3_device_ops_t sOps;
};

}; /* namespace android */

#endif // CAMERA_H
