#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>

uint8_t *image_process(const uint16_t *raw_pixels, int width, int height, int stride);
void save_pgm(const char *filename, const uint8_t *img, int width, int height);

#endif