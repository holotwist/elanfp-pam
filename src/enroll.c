#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "elan_drv.h"
#include "image.h"
#include "matcher.h"

#define MODEL_PATH   "/usr/share/elanfp/model.onnx"
#define STORAGE_DIR  "/var/lib/elanfp/templates"
#define CALIB_FILE   "/var/lib/elanfp/calibration.dat"

static int capture_single_touch(uint16_t *raw_buf, int *w, int *h, int *stride, const uint16_t *bg_buf) {
    ElanDevice dev = {0};

    // Retry connection up to 3 times to allow for watchdog re-enumeration
    for (int retry = 0; retry < 3; retry++) {
        if (elan_init(&dev) == 0) break;
        usleep(300000);
    }
    if (!dev.handle) return -1;

    if (elan_get_dims(&dev) < 0) {
        elan_close(&dev);
        return -1;
    }

    if (bg_buf) {
        dev.background = malloc(dev.stride * dev.height * sizeof(uint16_t));
        if (dev.background) {
            memcpy(dev.background, bg_buf, dev.stride * dev.height * sizeof(uint16_t));
        }
    }

    *w = dev.width;
    *h = dev.height;
    *stride = dev.stride;

    int r = 0;
    if (elan_wait_finger(&dev) < 0 || elan_capture(&dev, raw_buf) < 0) {
        r = -1;
    }

    // Always release USB interface immediately
    elan_close(&dev);
    return r;
}

static int calibrate_if_needed(uint16_t *bg_buf) {
    struct stat st;
    if (stat(CALIB_FILE, &st) == 0 && st.st_size == (80 * 80 * sizeof(uint16_t))) {
        FILE *f = fopen(CALIB_FILE, "rb");
        if (f) {
            size_t r = fread(bg_buf, sizeof(uint16_t), 80 * 80, f);
            fclose(f);
            if (r == 80 * 80) return 0;
        }
    }

    printf("Calibrating sensor. Do not touch sensor...\n");

    ElanDevice dev = {0};
    if (elan_init(&dev) < 0 || elan_get_dims(&dev) < 0) return -1;
    if (elan_calibrate(&dev) < 0) {
        elan_close(&dev);
        return -1;
    }

    memcpy(bg_buf, dev.background, dev.stride * dev.height * sizeof(uint16_t));
    elan_close(&dev);

    FILE *f = fopen(CALIB_FILE, "wb");
    if (f) {
        fwrite(bg_buf, sizeof(uint16_t), 80 * 80, f);
        fclose(f);
        chmod(CALIB_FILE, 0600);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <username>\n", argv[0]);
        return 1;
    }
    const char *username = argv[1];

    if (matcher_init(MODEL_PATH) < 0 && matcher_init("model.onnx") < 0) {
        fprintf(stderr, "Error: Failed to initialize model.\n");
        return 1;
    }

    uint16_t bg_buf[80 * 80];
    int has_bg = (calibrate_if_needed(bg_buf) == 0);

    FingerprintProfile profile = {0};
    uint16_t *raw_buf = calloc(32768, sizeof(uint16_t));

    printf("Enrolling user '%s' (%d touches required)\n", username, TARGET_ENROLL_TOUCHES);

    while (profile.count < TARGET_ENROLL_TOUCHES) {
        printf("[%d/%d] Touch reader... ", profile.count + 1, TARGET_ENROLL_TOUCHES);
        fflush(stdout);

        int w = 0, h = 0, stride = 0;
        if (capture_single_touch(raw_buf, &w, &h, &stride, has_bg ? bg_buf : NULL) < 0) {
            fprintf(stderr, "\nCapture failed, retrying touch...\n");
            usleep(500000);
            continue;
        }

        uint8_t *img = image_process(raw_buf, w, h, stride);
        if (!img) continue;

        float emb[EMBEDDING_DIM];
        if (matcher_extract(img, emb) < 0) {
            free(img);
            continue;
        }
        free(img);

        int duplicate = 0;
        for (int i = 0; i < profile.count; i++) {
            float dot = 0.0f;
            for (int d = 0; d < EMBEDDING_DIM; d++) dot += emb[d] * profile.embeddings[i][d];
            if (dot > 0.985f) { duplicate = 1; break; }
        }

        if (duplicate) {
            printf("redundant touch, shift finger position slightly.\n");
            elan_wait_release();
            continue;
        }

        memcpy(profile.embeddings[profile.count], emb, sizeof(emb));
        profile.count++;
        printf("captured.\n");

        if (profile.count < TARGET_ENROLL_TOUCHES) {
            elan_wait_release();
        }
    }

    mkdir(STORAGE_DIR, 0700);

    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/%s.dat", STORAGE_DIR, username);

    if (profile_save(out_path, &profile) == 0) {
        chmod(out_path, 0600);
        printf("Enrollment complete: %s\n", out_path);
    } else {
        fprintf(stderr, "Failed to save profile: %s\n", out_path);
    }

    free(raw_buf);
    matcher_cleanup();
    return 0;
}