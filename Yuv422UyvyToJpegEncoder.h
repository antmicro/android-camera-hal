#ifndef YUV422UYVYTOJPEGENCODER_H
#define YUV422UYVYTOJPEGENCODER_H

#include <YuvToJpegEncoder.h>

class Yuv422UyvyToJpegEncoder: public YuvToJpegEncoder {
public:
    Yuv422UyvyToJpegEncoder(int* strides);
    virtual ~Yuv422UyvyToJpegEncoder() {}

private:
    void configSamplingFactors(jpeg_compress_struct* cinfo);
    void compress(jpeg_compress_struct* cinfo, uint8_t* yuv, int* offsets);
    void deinterleave(uint8_t* yuv, uint8_t* yRows, uint8_t* uRows,
            uint8_t* vRows, int rowIndex, int width, int height);
};

#endif // YUV422UYVYTOJPEGENCODER_H
