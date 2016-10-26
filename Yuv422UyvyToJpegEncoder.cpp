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

#include "Yuv422UyvyToJpegEncoder.h"

/**
 * \class Yuv422UyvyToJpegEncoder
 *
 * Converts YUV(UYVY) image to JPEG.
 *
 * This is slightly modified Yuv422IToJpegEncoder from Android (frameworks/base/core/jni/android/graphics/YuvToJpegEncoder.cpp).
 */

Yuv422UyvyToJpegEncoder::Yuv422UyvyToJpegEncoder(int* strides) :
        YuvToJpegEncoder(strides) {
    fNumPlanes = 1;
}

void Yuv422UyvyToJpegEncoder::compress(jpeg_compress_struct* cinfo,
        uint8_t* yuv, int* offsets) {
    SkDebugf("onFlyCompress_422");
    JSAMPROW y[16];
    JSAMPROW cb[16];
    JSAMPROW cr[16];
    JSAMPARRAY planes[3];
    planes[0] = y;
    planes[1] = cb;
    planes[2] = cr;

    int width = cinfo->image_width;
    int height = cinfo->image_height;
    uint8_t* yRows = new uint8_t [16 * width];
    uint8_t* uRows = new uint8_t [16 * (width >> 1)];
    uint8_t* vRows = new uint8_t [16 * (width >> 1)];

    uint8_t* yuvOffset = yuv + offsets[0];

    // process 16 lines of Y and 16 lines of U/V each time.
    while (cinfo->next_scanline < cinfo->image_height) {
        deinterleave(yuvOffset, yRows, uRows, vRows, cinfo->next_scanline, width, height);

        // Jpeg library ignores the rows whose indices are greater than height.
        for (int i = 0; i < 16; i++) {
            // y row
            y[i] = yRows + i * width;

            // construct u row and v row
            // width is halved because of downsampling
            int offset = i * (width >> 1);
            cb[i] = uRows + offset;
            cr[i] = vRows + offset;
        }

        jpeg_write_raw_data(cinfo, planes, 16);
    }
    delete [] yRows;
    delete [] uRows;
    delete [] vRows;
}

void Yuv422UyvyToJpegEncoder::deinterleave(uint8_t* yuv, uint8_t* yRows, uint8_t* uRows,
        uint8_t* vRows, int rowIndex, int width, int height) {
    int numRows = height - rowIndex;
    if (numRows > 16) numRows = 16;
    for (int row = 0; row < numRows; ++row) {
        uint8_t* yuvSeg = yuv + (rowIndex + row) * fStrides[0];
        for (int i = 0; i < (width >> 1); ++i) {
            int indexY = row * width + (i << 1);
            int indexU = row * (width >> 1) + i;
            yRows[indexY] = yuvSeg[1];
            yRows[indexY + 1] = yuvSeg[3];
            uRows[indexU] = yuvSeg[0];
            vRows[indexU] = yuvSeg[2];
            yuvSeg += 4;
        }
    }
}

void Yuv422UyvyToJpegEncoder::configSamplingFactors(jpeg_compress_struct* cinfo) {
    // cb and cr are horizontally downsampled and vertically downsampled as well.
    cinfo->comp_info[0].h_samp_factor = 2;
    cinfo->comp_info[0].v_samp_factor = 2;
    cinfo->comp_info[1].h_samp_factor = 1;
    cinfo->comp_info[1].v_samp_factor = 2;
    cinfo->comp_info[2].h_samp_factor = 1;
    cinfo->comp_info[2].v_samp_factor = 2;
}
