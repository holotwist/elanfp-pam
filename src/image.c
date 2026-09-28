#include <stdio.h>
#include <stdlib.h>
#include "image.h"

uint8_t *image_process(const uint16_t *raw_pixels, int width, int height, int stride) {
    int total_pixels = width * height;
    uint8_t *img = malloc(total_pixels);
    if (!img) return NULL;

    int hist[65536] = {0};
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            hist[raw_pixels[x + y * stride]]++;
        }
    }

    int min = -1, max = -1;
    int count = 0;
    int p2 = (int)(total_pixels * 0.02f);
    int p98 = (int)(total_pixels * 0.98f);

    for (int i = 0; i < 65536; i++) {
        count += hist[i];
        if (count >= p2 && min == -1) min = i;
        if (count >= p98 && max == -1) {
            max = i;
            break;
        }
    }
    if (max <= min) max = min + 1;

    // Normalize to 8-bit and rotate 90 degrees CCW
    int out_w = height;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int val = raw_pixels[x + y * stride];
            if (val < min) val = min;
            if (val > max) val = max;

            uint8_t px = (uint8_t)(((val - min) * 255) / (max - min));
            int dest_x = y;
            int dest_y = width - 1 - x;
            img[dest_x + dest_y * out_w] = px;
        }
    }
    return img;
}

void save_pgm(const char *filename, const uint8_t *img, int width, int height) {
    FILE *f = fopen(filename, "wb");
    if (!f) return;
    fprintf(f, "P5\n%d %d\n255\n", width, height);
    fwrite(img, 1, width * height, f);
    fclose(f);
}