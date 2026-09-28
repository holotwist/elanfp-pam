#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "elan_drv.h"
#include "image.h"
#include "matcher.h"

static const char* resolve_model_path(void) {
    static const char *candidates[] = {
        "/usr/share/elanfp/model.onnx",
        "model.onnx",
        "train/model.onnx",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) == 0) return candidates[i];
    }
    return candidates[0];
}

static uint16_t* load_or_calibrate_background(const char *calib_path) {
    uint16_t *bg = calloc(80 * 80, sizeof(uint16_t));
    if (!bg) return NULL;

    FILE *f = fopen(calib_path, "rb");
    if (f) {
        size_t r = fread(bg, sizeof(uint16_t), 80 * 80, f);
        fclose(f);
        if (r > 0) return bg;
    }

    // Calibrate
    printf("No baseline calibration found.\n");
    printf("Please do not touch the sensor for 2 seconds...\n");
    usleep(1500000);

    ElanDevice dev = {0};
    if (elan_init(&dev) < 0 || elan_get_dims(&dev) < 0) {
        fprintf(stderr, "Error: Failed to connect to sensor for calibration.\n");
        free(bg);
        return NULL;
    }

    if (elan_calibrate(&dev) < 0) {
        fprintf(stderr, "Error: Calibration failed.\n");
        elan_close(&dev);
        free(bg);
        return NULL;
    }

    memcpy(bg, dev.background, dev.stride * dev.height * sizeof(uint16_t));
    elan_close(&dev);

    f = fopen(calib_path, "wb");
    if (f) {
        fwrite(bg, sizeof(uint16_t), 80 * 80, f);
        fclose(f);
        chmod(calib_path, 0600);
        printf("Calibration saved to %s\n\n", calib_path);
    }
    return bg;
}

static int capture_single_scan(uint16_t *raw_buf, int *out_w, int *out_h, int *out_stride, const uint16_t *bg_buf) {
    ElanDevice dev = {0};

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

    *out_w = dev.width;
    *out_h = dev.height;
    *out_stride = dev.stride;

    if (elan_wait_finger(&dev) < 0 || elan_capture(&dev, raw_buf) < 0) {
        elan_close(&dev);
        return -1;
    }

    elan_close(&dev);
    return 0;
}

static const char* get_enroll_hint(int touch_idx) {
    switch (touch_idx) {
        case 0: return "Place the center of your finger firmly";
        case 1: return "Place the center again with natural pressure";
        case 2: return "Shift up, place the upper tip of your finger";
        case 3: return "Shift down, place the lower pad / crease";
        case 4: return "Tilt left, place the left side of your finger";
        case 5: return "Tilt right, place the right side of your finger";
        case 6: return "Slight angle: place at an angle to the left";
        case 7: return "Slight angle: place at an angle to the right";
        default: return "Touch firmly";
    }
}

int main(int argc, char *argv[]) {
    if (geteuid() != 0) {
        fprintf(stderr, "Warning: Root privileges required for USB sensor access. Please run with sudo.\n");
    }

    const char *username = (argc > 1) ? argv[1] : getenv("USER");
    if (!username || !*username) username = "default";

    const char *model_path = resolve_model_path();
    if (matcher_init(model_path) < 0) {
        fprintf(stderr, "Error: Could not load ONNX model (%s).\n", model_path);
        return 1;
    }

    // Determine paths
    char calib_path[512];
    char profile_path[512];
    if (access("/var/lib/elanfp", W_OK) == 0 || mkdir("/var/lib/elanfp", 0700) == 0) {
        mkdir("/var/lib/elanfp/templates", 0700);
        snprintf(calib_path, sizeof(calib_path), "/var/lib/elanfp/calibration.dat");
        snprintf(profile_path, sizeof(profile_path), "/var/lib/elanfp/templates/%s.dat", username);
    } else {
        snprintf(calib_path, sizeof(calib_path), "calibration.dat");
        snprintf(profile_path, sizeof(profile_path), "%s.dat", username);
    }

    uint16_t *bg_buf = load_or_calibrate_background(calib_path);
    if (!bg_buf) {
        fprintf(stderr, "Error: Background noise calibration unavailable.\n");
        matcher_cleanup();
        return 1;
    }

    FingerprintProfile profile = {0};
    uint16_t *raw_buf = calloc(32768, sizeof(uint16_t));
    int w = 0, h = 0, stride = 0;

    printf("\nEnrollment (%d touches required for '%s')\n", TARGET_ENROLL_TOUCHES, username);

    while (profile.count < TARGET_ENROLL_TOUCHES) {
        int idx = profile.count;
        printf("\n[Touch %d/%d] %s\n", idx + 1, TARGET_ENROLL_TOUCHES, get_enroll_hint(idx));

        if (capture_single_scan(raw_buf, &w, &h, &stride, bg_buf) < 0) {
            fprintf(stderr, "Capture failed. Retrying touch.\n");
            usleep(500000);
            continue;
        }

        uint8_t *img = image_process(raw_buf, w, h, stride);
        if (!img) continue;

        float emb[EMBEDDING_DIM];
        if (matcher_extract(img, emb) < 0) {
            fprintf(stderr, "Feature extraction failed. Retrying.\n");
            free(img);
            continue;
        }
        free(img);

        // Quality check, avoid saving identical duplicate touches
        int duplicate = 0;
        for (int i = 0; i < profile.count; i++) {
            float dot = 0.0f;
            for (int d = 0; d < EMBEDDING_DIM; d++) {
                dot += emb[d] * profile.embeddings[i][d];
            }
            if (dot > 0.985f) {
                duplicate = 1;
                break;
            }
        }

        if (duplicate) {
            printf("Exact duplicate area detected. Shift your finger slightly.\n");
            usleep(1200000);
            continue;
        }

        memcpy(profile.embeddings[profile.count], emb, sizeof(emb));
        profile.count++;

        int pct = (profile.count * 100) / TARGET_ENROLL_TOUCHES;
        printf("Touch registered, progress: [%d%%] (%d/%d)\n", pct, profile.count, TARGET_ENROLL_TOUCHES);
        printf("Please lift your finger.\n");
        usleep(1300000);
    }

    if (profile_save(profile_path, &profile) == 0) {
        chmod(profile_path, 0600);
        printf("\nEnrolled %d templates saved to %s\n", profile.count, profile_path);
    } else {
        fprintf(stderr, "Error saving profile to %s\n", profile_path);
    }

    matcher_cleanup();
    free(raw_buf);
    free(bg_buf);
    return 0;
}