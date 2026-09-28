#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

    printf("No baseline calibration found.\n");
    printf("Please do not touch the sensor for 2 seconds.\n");
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

int main(int argc, char *argv[]) {
    if (geteuid() != 0) {
        fprintf(stderr, "Warning: Root privileges required for USB sensor access. Please run with sudo.\n");
    }

    const char *username = (argc > 1) ? argv[1] : getenv("USER");
    if (!username || !*username) username = "default";

    char profile_path[512];
    snprintf(profile_path, sizeof(profile_path), "/var/lib/elanfp/templates/%s.dat", username);
    if (access(profile_path, R_OK) != 0) {
        snprintf(profile_path, sizeof(profile_path), "%s.dat", username);
        if (access(profile_path, R_OK) != 0) {
            snprintf(profile_path, sizeof(profile_path), "fingerprint.dat");
        }
    }

    FingerprintProfile profile = {0};
    if (profile_load(profile_path, &profile) < 0 || profile.count == 0) {
        fprintf(stderr, "Error: No enrolled profile found for '%s' (%s).\n", username, profile_path);
        return 1;
    }

    const char *model_path = resolve_model_path();
    if (matcher_init(model_path) < 0) {
        fprintf(stderr, "Error: Failed to load model (%s).\n", model_path);
        return 1;
    }

    char calib_path[512];
    if (access("/var/lib/elanfp/calibration.dat", R_OK) == 0) {
        snprintf(calib_path, sizeof(calib_path), "/var/lib/elanfp/calibration.dat");
    } else {
        snprintf(calib_path, sizeof(calib_path), "background.dat");
    }

    uint16_t *bg_buf = load_or_calibrate_background(calib_path);
    if (!bg_buf) {
        fprintf(stderr, "Error: Baseline calibration unavailable.\n");
        matcher_cleanup();
        return 1;
    }

    uint16_t *raw_buf = calloc(32768, sizeof(uint16_t));
    int w = 0, h = 0, stride = 0;

    printf("\nVerification ('%s')\n", username);
    printf("Touch the sensor to verify identity.\n");

    if (capture_single_scan(raw_buf, &w, &h, &stride, bg_buf) < 0) {
        fprintf(stderr, "Verification scan failed.\n");
        free(raw_buf);
        free(bg_buf);
        matcher_cleanup();
        return 1;
    }

    uint8_t *img = image_process(raw_buf, w, h, stride);
    if (!img) {
        free(raw_buf);
        free(bg_buf);
        matcher_cleanup();
        return 1;
    }

    float probe_emb[EMBEDDING_DIM];
    if (matcher_extract(img, probe_emb) < 0) {
        fprintf(stderr, "Feature extraction failed.\n");
        free(img);
        free(raw_buf);
        free(bg_buf);
        matcher_cleanup();
        return 1;
    }
    free(img);
    free(raw_buf);
    free(bg_buf);

    MatchResult res = matcher_verify(probe_emb, &profile);

    printf("\nResult\n");
    printf("  Top Match Score   : %.4f (Threshold: >= %.2f)\n", res.best_score, THRESHOLD_DIRECT);
    printf("  Second Best Score : %.4f\n", res.second_score);
    printf("  Consensus Count   : %d template(s) >= %.2f\n", res.consensus_count, THRESHOLD_CONSENSUS);
    printf("  Decision          : %s\n", res.reason);

    if (res.granted) {
        printf("\nIdentity confirmed\n");
    } else {
        printf("\nRefused.\n");
    }

    matcher_cleanup();
    return res.granted ? 0 : 2;
}