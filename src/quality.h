#ifndef QUALITY_H
#define QUALITY_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    QUALITY_OK = 0,
    QUALITY_LOW_AREA,
    QUALITY_OFF_CENTER,
    QUALITY_LOW_CONTRAST
} QualityResult;

typedef struct {
    QualityResult result;
    int active_pixels;
    float center_x;
    float center_y;
    float ridge_contrast;
} FrameQuality;

// Checks active contact area, centering, and ridge contrast
FrameQuality quality_check_raw(const uint16_t *raw, int width, int height, int stride);

#endif