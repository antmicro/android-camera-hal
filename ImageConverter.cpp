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

#include <YuvToJpegEncoder.h>
#include <SkStream.h>
#include <libyuv/row.h>

#include "Yuv422UyvyToJpegEncoder.h"
#include "ImageConverter.h"
#include "DbgUtils.h"

#define WORKERS_TASKS_NUM 30

namespace android {

ImageConverter::ImageConverter() {
}

ImageConverter::~ImageConverter() {
}

uint8_t *ImageConverter::YUY2ToRGBA(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height) {
    assert(gWorkers.isRunning());
    assert(src != NULL);
    assert(dst != NULL);
    assert(width > 0);
    assert(height > 0);

    Workers::Task::Function taskFn = [](void *data) {
        ConvertTask::Data *d = static_cast<ConvertTask::Data *>(data);

        SIMD_ALIGNED(uint8 rowy[kMaxStride]);
        SIMD_ALIGNED(uint8 rowu[kMaxStride]);
        SIMD_ALIGNED(uint8 rowv[kMaxStride]);

        for(size_t i = 0; i < d->linesNum; ++i) {
            libyuv::YUY2ToUV422Row_NEON(d->src, rowu, rowv, d->width);
            libyuv::YUY2ToYRow_NEON(d->src, rowy, d->width);
            /* Somehow destination format is swapped (*ABGR converts to RGBA) */
            libyuv::I422ToABGRRow_NEON(rowy, rowu, rowv, d->dst, d->width);
            d->src += d->width * 2;
            d->dst += d->width * 4;
        }
    };

    return splitRunWait(src, dst, width, height, taskFn);
}

uint8_t *ImageConverter::YUY2ToJPEG(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height, size_t dstLen, uint8_t quality) {
    assert(src != NULL);
    assert(dst != NULL);
    assert(width > 0);
    assert(height > 0);
    assert(dstLen > 0);
    assert(quality <= 100);

    /* TODO: do it parallel with libjpeg */

    int strides[] = { (int)width * 2 };
    int offsets[] = { 0 };
    Yuv422IToJpegEncoder encoder(strides);
    SkDynamicMemoryWStream stream;

    encoder.encode(&stream, (void *)src, (int)width, (int)height, offsets, quality);

    if(stream.getOffset() > dstLen)
        return dst;

    stream.copyTo(dst);
    return dst + stream.getOffset();
}

uint8_t *ImageConverter::UYVYToRGBA(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height) {
    assert(gWorkers.isRunning());
    assert(src != NULL);
    assert(dst != NULL);
    assert(width > 0);
    assert(height > 0);

    Workers::Task::Function taskFn = [](void *data) {
        ConvertTask::Data *d = static_cast<ConvertTask::Data *>(data);

        SIMD_ALIGNED(uint8 rowy[kMaxStride]);
        SIMD_ALIGNED(uint8 rowu[kMaxStride]);
        SIMD_ALIGNED(uint8 rowv[kMaxStride]);

        for(size_t i = 0; i < d->linesNum; ++i) {
            libyuv::UYVYToUV422Row_NEON(d->src, rowu, rowv, d->width);
            libyuv::UYVYToYRow_NEON(d->src, rowy, d->width);
            /* Somehow destination format is swapped (*ABGR converts to RGBA) */
            libyuv::I422ToABGRRow_NEON(rowy, rowu, rowv, d->dst, d->width);
            d->src += d->width * 2;
            d->dst += d->width * 4;
        }
    };

    return splitRunWait(src, dst, width, height, taskFn);
}

uint8_t *ImageConverter::UYVYToJPEG(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height, size_t dstLen, uint8_t quality) {
    assert(src != NULL);
    assert(dst != NULL);
    assert(width > 0);
    assert(height > 0);
    assert(dstLen > 0);
    assert(quality <= 100);

    /* TODO: do it parallel with libjpeg */

    int strides[] = { (int)width * 2 };
    int offsets[] = { 0 };
    Yuv422UyvyToJpegEncoder encoder(strides);
    SkDynamicMemoryWStream stream;

    encoder.encode(&stream, (void *)src, (int)width, (int)height, offsets, quality);

    if(stream.getOffset() > dstLen)
        return dst;

    stream.copyTo(dst);
    return dst + stream.getOffset();
}

uint8_t * ImageConverter::splitRunWait(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height, Workers::Task::Function fn) {
    ConvertTask tasks[WORKERS_TASKS_NUM];

    const uint8_t   *srcPtr = src;
    uint8_t         *dstPtr = dst;
    const size_t linesPerTask = (height + WORKERS_TASKS_NUM - 1) / WORKERS_TASKS_NUM;
    for(size_t i = 0; i < WORKERS_TASKS_NUM; ++i) {
        tasks[i].data.src       = srcPtr;
        tasks[i].data.dst       = dstPtr;
        tasks[i].data.width     = width;
        tasks[i].data.linesNum  = linesPerTask;
        if((i + 1) * linesPerTask >= height) {
            tasks[i].data.linesNum = height - i * linesPerTask;
        }

        tasks[i].task = Workers::Task(fn, (void *)&tasks[i].data);
        gWorkers.queueTask(&tasks[i].task);

        srcPtr += linesPerTask * width * 2;
        dstPtr += linesPerTask * width * 4;
    }

    for(size_t i = 0; i < WORKERS_TASKS_NUM; ++i) {
        tasks[i].task.waitForCompletion();
    }

    return dstPtr;
}

}; /* namespace android */
