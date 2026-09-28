#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "elan_drv.h"
#include "image.h"
#include "matcher.h"

#define MODEL_PATH   "/usr/share/elanfp/model.onnx"
#define STORAGE_DIR  "/var/lib/elanfp/templates"
#define CALIB_FILE   "/var/lib/elanfp/calibration.dat"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <username>\n", argv[0]);
        return 1;
    }
    const char *username = argv[1];

    char profile_path[512];
    snprintf(profile_path, sizeof(profile_path), "%s/%s.dat", STORAGE_DIR, username);

    FingerprintProfile profile = {0};
    if (profile_load(profile_path, &profile) < 0) {
        fprintf(stderr, "Error loading template for user '%s'\n", username);
        return 1;
    }

    if (matcher_init(MODEL_PATH) < 0 && matcher_init("model.onnx") < 0) {
        fprintf(stderr, "Error: Failed to load model.\n");
        return 1;
    }

    ElanDevice dev = {0};
    for (int retry = 0; retry < 3; retry++) {
        if (elan_init(&dev) == 0) break;
        usleep(300000);
    }
    if (!dev.handle || elan_get_dims(&dev) < 0) {
        fprintf(stderr, "Error initializing reader.\n");
        elan_close(&dev);
        matcher_cleanup();
        return 1;
    }

    FILE *f = fopen(CALIB_FILE, "rb");
    if (f) {
        dev.background = malloc(dev.stride * dev.height * sizeof(uint16_t));
        if (dev.background) {
            fread(dev.background, sizeof(uint16_t), dev.stride * dev.height, f);
        }
        fclose(f);
    }

    printf("Touch reader to verify user '%s'...\n", username);
    if (elan_wait_finger(&dev) < 0) {
        elan_close(&dev);
        matcher_cleanup();
        return 1;
    }

    uint16_t raw_buf[32768];
    int cap_res = elan_capture(&dev, raw_buf);
    int w = dev.width, h = dev.height, stride = dev.stride;

    elan_close(&dev);

    if (cap_res < 0) {
        fprintf(stderr, "Capture failed.\n");
        matcher_cleanup();
        return 1;
    }

    uint8_t *img = image_process(raw_buf, w, h, stride);
    if (!img) {
        matcher_cleanup();
        return 1;
    }

    float emb[EMBEDDING_DIM];
    int ext_res = matcher_extract(img, emb);
    free(img);
    matcher_cleanup();

    if (ext_res < 0) {
        fprintf(stderr, "Feature extraction failed.\n");
        return 1;
    }

    MatchResult res = matcher_verify(emb, &profile);
    printf("Result: %s (Score: %.4f, Consensus: %d)\n",
           res.granted ? "MATCH" : "MISMATCH",
           res.best_score,
           res.consensus_count);

    return res.granted ? 0 : 2;
}