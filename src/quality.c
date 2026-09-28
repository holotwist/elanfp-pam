#include "quality.h"
#include <math.h>

FrameQuality quality_check_raw(const uint16_t *raw, int width, int height, int stride) {
    FrameQuality q = {0};
    int total = width * height;

    // Build histogram to find dynamic range
    int hist[65536] = {0};
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            hist[raw[x + y * stride]]++;
        }
    }

    int p5 = 0, p95 = 0;
    int count = 0;
    int t5 = (int)(total * 0.05f);
    int t95 = (int)(total * 0.95f);
    bool set5 = false;

    for (int i = 0; i < 65536; i++) {
        count += hist[i];
        if (!set5 && count >= t5) { p5 = i; set5 = true; }
        if (count >= t95) { p95 = i; break; }
    }

    int dynamic_range = p95 - p5;
    // Minimum capacitive spread to confirm physical contact
    if (dynamic_range < 5) {
        q.result = QUALITY_LOW_AREA;
        return q;
    }

    // Centroid of contact energy
    uint16_t mid = (p5 + p95) / 2;
    double sum_x = 0, sum_y = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint16_t val = raw[x + y * stride];
            if (val > mid) {
                q.active_pixels++;
                sum_x += x;
                sum_y += y;
            }
        }
    }

    if (q.active_pixels < 120) {
        q.result = QUALITY_LOW_AREA;
        return q;
    }

    q.center_x = (float)(sum_x / q.active_pixels);
    q.center_y = (float)(sum_y / q.active_pixels);
    q.result = QUALITY_OK;
    return q;
}