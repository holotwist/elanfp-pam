#define PAM_SM_AUTH
#include <security/pam_modules.h>
#include <security/pam_ext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "elan_drv.h"
#include "image.h"
#include "matcher.h"

#define DEFAULT_MODEL_PATH   "/usr/share/elanfp/model.onnx"
#define DEFAULT_STORAGE_DIR  "/var/lib/elanfp/templates"
#define DEFAULT_CALIB_FILE   "/var/lib/elanfp/calibration.dat"
#define DEFAULT_TIMEOUT_MS   4000
#define MAX_ATTEMPTS         2

static int load_background(const char *path, uint16_t *bg_buf) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t r = fread(bg_buf, sizeof(uint16_t), 80 * 80, f);
    fclose(f);
    return (r > 0) ? 0 : -1;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    (void)flags;
    const char *username = NULL;
    if (pam_get_user(pamh, &username, NULL) != PAM_SUCCESS || !username || !*username) {
        return PAM_USER_UNKNOWN;
    }

    const char *model_path = DEFAULT_MODEL_PATH;
    const char *storage_dir = DEFAULT_STORAGE_DIR;
    const char *calib_path = DEFAULT_CALIB_FILE;
    int timeout_ms = DEFAULT_TIMEOUT_MS;

    for (int i = 0; i < argc; i++) {
        if (strncmp(argv[i], "model=", 6) == 0) model_path = argv[i] + 6;
        else if (strncmp(argv[i], "storage=", 8) == 0) storage_dir = argv[i] + 8;
        else if (strncmp(argv[i], "calib=", 6) == 0) calib_path = argv[i] + 6;
        else if (strncmp(argv[i], "timeout=", 8) == 0) timeout_ms = atoi(argv[i] + 8) * 1000;
    }

    char profile_path[512];
    snprintf(profile_path, sizeof(profile_path), "%s/%s.dat", storage_dir, username);

    FingerprintProfile profile = {0};
    if (profile_load(profile_path, &profile) < 0 || profile.count == 0) {
        return PAM_AUTHINFO_UNAVAIL;
    }

    uint16_t bg_buf[80 * 80];
    if (load_background(calib_path, bg_buf) != 0) {
        pam_syslog(pamh, 3, "pam_elanfp: Calibration baseline missing (%s)", calib_path);
        return PAM_AUTHINFO_UNAVAIL;
    }

    if (matcher_init(model_path) < 0) {
        pam_syslog(pamh, 3, "pam_elanfp: Failed to load ONNX model (%s)", model_path);
        return PAM_AUTHINFO_UNAVAIL;
    }

    int auth_status = PAM_AUTH_ERR;

    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        pam_info(pamh, "Touch fingerprint sensor (%d/%d).", attempt, MAX_ATTEMPTS);

        ElanDevice dev = {0};
        for (int retry = 0; retry < 3; retry++) {
            if (elan_init(&dev) == 0) break;
            usleep(200000);
        }
        if (!dev.handle || elan_get_dims(&dev) < 0) {
            elan_close(&dev);
            auth_status = PAM_AUTHINFO_UNAVAIL;
            break;
        }

        dev.background = malloc(dev.stride * dev.height * sizeof(uint16_t));
        if (dev.background) {
            memcpy(dev.background, bg_buf, dev.stride * dev.height * sizeof(uint16_t));
        }

        int wait_res = elan_wait_finger_timeout(&dev, timeout_ms);
        if (wait_res < 0) {
            elan_close(&dev);
            auth_status = PAM_AUTHINFO_UNAVAIL;
            break;
        }

        uint16_t raw_buf[32768];
        int cap_res = elan_capture(&dev, raw_buf);
        int w = dev.width, h = dev.height, stride = dev.stride;
        elan_close(&dev);

        if (cap_res < 0) {
            elan_wait_release();
            continue;
        }

        uint8_t *img = image_process(raw_buf, w, h, stride);
        if (!img) {
            elan_wait_release();
            continue;
        }

        float probe_emb[EMBEDDING_DIM];
        int ext_res = matcher_extract(img, probe_emb);
        free(img);

        if (ext_res < 0) {
            elan_wait_release();
            continue;
        }

        MatchResult res = matcher_verify(probe_emb, &profile);
        if (res.granted) {
            pam_info(pamh, "Fingerprint recognized (%s, score: %.2f)", res.reason, res.best_score);
            auth_status = PAM_SUCCESS;
            break;
        }

        if (attempt < MAX_ATTEMPTS) {
            pam_error(pamh, "Fingerprint not recognized. Lift finger and try again.");
            elan_wait_release();
        }
    }

    matcher_cleanup();
    return auth_status;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    (void)pamh; (void)flags; (void)argc; (void)argv;
    return PAM_SUCCESS;
}