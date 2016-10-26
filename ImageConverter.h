#ifndef IMAGECONVERTER_H
#define IMAGECONVERTER_H

#include <stdint.h>
#include "Workers.h"

namespace android {

class ImageConverter
{
public:
    ImageConverter();
    ~ImageConverter();

    uint8_t * YUY2ToRGBA(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height);
    uint8_t * YUY2ToJPEG(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height, size_t dstLen, uint8_t quality);

    uint8_t * UYVYToRGBA(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height);
    uint8_t * UYVYToJPEG(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height, size_t dstLen, uint8_t quality);

protected:
    uint8_t * splitRunWait(const uint8_t *src, uint8_t *dst, unsigned width, unsigned height, Workers::Task::Function fn);

private:
    struct ConvertTask {
        Workers::Task task;
        struct Data {
            const uint8_t  *src;
            uint8_t        *dst;
            size_t          width;
            size_t          linesNum;
        } data;
    };
};

}; /* namespace android */

#endif // IMAGECONVERTER_H
