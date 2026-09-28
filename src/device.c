#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <systemd/sd-bus.h>
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "device.h"
#include "elan_drv.h"
#include "image.h"
#include "storage.h"
#include "quality.h"
#include "enroll_policy.h"

#define CALIB_PATH "/var/lib/fprint/sensor_calib.bin"

static DeviceContext g_dev = {0};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_worker_th;
static atomic_bool g_cancel_req = false;
static uint16_t *g_calib_bg = NULL;

int device_init(const char *model_path) {
    if (matcher_init(model_path) < 0) return -1;
    mkdir("/var/lib/fprint", 0700);

    // Read cached calibration from disk if present
    g_calib_bg = calloc(80 * 80, sizeof(uint16_t));
    if (g_calib_bg) {
        FILE *f = fopen(CALIB_PATH, "rb");
        if (f) {
            fread(g_calib_bg, sizeof(uint16_t), 80 * 80, f);
            fclose(f);
        }
    }
    return 0;
}

void device_cleanup(void) {
    device_stop_enroll();
    device_stop_verify();
    matcher_cleanup();
    if (g_calib_bg) { free(g_calib_bg); g_calib_bg = NULL; }
}

DeviceState device_get_state(void) {
    pthread_mutex_lock(&g_lock);
    DeviceState s = g_dev.state;
    pthread_mutex_unlock(&g_lock);
    return s;
}

const char *device_get_claimed_user(void) {
    return g_dev.target_user[0] ? g_dev.target_user : NULL;
}

extern sd_bus *g_bus;

int device_claim(const char *bus_caller, const char *target_user) {
    pthread_mutex_lock(&g_lock);
    if (g_dev.claimed_by[0] && strcmp(g_dev.claimed_by, bus_caller) != 0) {
        // Check if previous claimant died (e.g. Ctrl+C)
        if (sd_bus_get_name_creds(g_bus, g_dev.claimed_by, 0, NULL) < 0) {
            pthread_mutex_unlock(&g_lock);
            device_force_release();
            pthread_mutex_lock(&g_lock);
        } else {
            pthread_mutex_unlock(&g_lock);
            return -1;
        }
    }
    strncpy(g_dev.claimed_by, bus_caller, sizeof(g_dev.claimed_by) - 1);
    strncpy(g_dev.target_user, target_user, sizeof(g_dev.target_user) - 1);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int device_release(const char *bus_caller) {
    pthread_mutex_lock(&g_lock);
    if (strcmp(g_dev.claimed_by, bus_caller) != 0) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    DeviceState prev = g_dev.state;
    atomic_store(&g_cancel_req, true);
    g_dev.claimed_by[0] = '\0';
    g_dev.target_user[0] = '\0';
    pthread_mutex_unlock(&g_lock);

    if (prev != DEV_STATE_IDLE) {
        pthread_join(g_worker_th, NULL);
    }

    pthread_mutex_lock(&g_lock);
    g_dev.state = DEV_STATE_IDLE;
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void device_force_release(void) {
    pthread_mutex_lock(&g_lock);
    DeviceState prev = g_dev.state;
    atomic_store(&g_cancel_req, true);
    g_dev.claimed_by[0] = '\0';
    g_dev.target_user[0] = '\0';
    pthread_mutex_unlock(&g_lock);

    if (prev != DEV_STATE_IDLE) {
        pthread_join(g_worker_th, NULL);
    }

    pthread_mutex_lock(&g_lock);
    g_dev.state = DEV_STATE_IDLE;
    pthread_mutex_unlock(&g_lock);
}

static int capture_frame(uint16_t *raw_buf, int *w, int *h, int *stride) {
    ElanDevice dev = {0};
    for (int i = 0; i < 3 && !atomic_load(&g_cancel_req); i++) {
        if (elan_init(&dev) == 0) break;
        usleep(100000);
    }
    if (!dev.handle) return -1;
    if (elan_get_dims(&dev) < 0) { elan_close(&dev); return -1; }

    if (!g_calib_bg || g_calib_bg[0] == 0) {
        if (elan_calibrate(&dev) == 0) {
            if (!g_calib_bg) g_calib_bg = calloc(80 * 80, sizeof(uint16_t));
            if (g_calib_bg) {
                memcpy(g_calib_bg, dev.background, dev.stride * dev.height * sizeof(uint16_t));
                FILE *f = fopen(CALIB_PATH, "wb");
                if (f) {
                    fwrite(g_calib_bg, sizeof(uint16_t), 80 * 80, f);
                    fclose(f);
                    chmod(CALIB_PATH, 0600);
                }
            }
        }
    }

    if (g_calib_bg) {
        dev.background = malloc(dev.stride * dev.height * sizeof(uint16_t));
        if (dev.background) memcpy(dev.background, g_calib_bg, dev.stride * dev.height * sizeof(uint16_t));
    }

    *w = dev.width;
    *h = dev.height;
    *stride = dev.stride;

    // If finger is currently resting from previous touch, wait for lift
    elan_wait_release_timeout(&dev, 3000);

    // Wait for fresh touch
    while (!atomic_load(&g_cancel_req)) {
        int r = elan_wait_finger_timeout(&dev, 100);
        if (r == 0) break;
        if (r == -1) { elan_close(&dev); return -1; }
    }

    if (atomic_load(&g_cancel_req)) {
        elan_stop(&dev);
        elan_close(&dev);
        return -1;
    }

    int res = elan_capture(&dev, raw_buf);
    elan_close(&dev);
    return res;
}

static void *enroll_worker(void *arg) {
    (void)arg;
    EnrollSession session;
    enroll_session_init(&session, TARGET_ENROLL_TOUCHES);

    uint16_t raw_buf[32768];
    int w = 0, h = 0, stride = 0;

    while (!atomic_load(&g_cancel_req)) {
        if (capture_frame(raw_buf, &w, &h, &stride) < 0) continue;

        FrameQuality q = quality_check_raw(raw_buf, w, h, stride);

        uint8_t *img = image_process(raw_buf, w, h, stride);
        if (!img) {
            if (g_dev.enroll_cb) g_dev.enroll_cb("enroll-retry-scan", false, g_dev.enroll_cb_data);
            continue;
        }

        float emb[EMBEDDING_DIM];
        int ext_res = matcher_extract(img, emb);
        free(img);

        if (ext_res < 0) {
            if (g_dev.enroll_cb) g_dev.enroll_cb("enroll-retry-scan", false, g_dev.enroll_cb_data);
            continue;
        }

        bool done = false;
        const char *status = enroll_session_add(&session, emb, &q, &done);

        if (g_dev.enroll_cb) g_dev.enroll_cb(status, done, g_dev.enroll_cb_data);

        if (done) {
            storage_save_profile(g_dev.target_user, g_dev.target_finger, &session.profile);
            break;
        }
    }

    pthread_mutex_lock(&g_lock);
    g_dev.state = DEV_STATE_IDLE;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

static void *verify_worker(void *arg) {
    (void)arg;
    uint16_t raw_buf[32768];
    int w = 0, h = 0, stride = 0;

    FingerprintProfile profiles[10];
    int profile_count = 0;

    if (strcmp(g_dev.target_finger, "any") == 0) {
        char **fingers = NULL;
        size_t count = 0;
        storage_list_fingers(g_dev.target_user, &fingers, &count);
        for (size_t i = 0; i < count && profile_count < 10; i++) {
            if (storage_load_profile(g_dev.target_user, fingers[i], &profiles[profile_count]) == 0) {
                profile_count++;
            }
        }
        storage_free_list(fingers, count);
    } else {
        if (storage_load_profile(g_dev.target_user, g_dev.target_finger, &profiles[0]) == 0) {
            profile_count = 1;
        }
    }

    if (profile_count == 0) {
        if (g_dev.verify_cb) g_dev.verify_cb("verify-no-match", true, g_dev.verify_cb_data);
        pthread_mutex_lock(&g_lock);
        g_dev.state = DEV_STATE_IDLE;
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }

    while (!atomic_load(&g_cancel_req)) {
        if (capture_frame(raw_buf, &w, &h, &stride) < 0) continue;

        FrameQuality q = quality_check_raw(raw_buf, w, h, stride);
        if (q.result != QUALITY_OK) {
            if (g_dev.verify_cb) g_dev.verify_cb("verify-retry-scan", false, g_dev.verify_cb_data);
            continue;
        }

        uint8_t *img = image_process(raw_buf, w, h, stride);
        if (!img) continue;

        float emb[EMBEDDING_DIM];
        int ext_res = matcher_extract(img, emb);
        free(img);

        if (ext_res < 0) {
            if (g_dev.verify_cb) g_dev.verify_cb("verify-retry-scan", false, g_dev.verify_cb_data);
            continue;
        }

        bool matched = false;
        for (int p = 0; p < profile_count; p++) {
            MatchResult res = matcher_verify(emb, &profiles[p]);
            if (res.granted) { matched = true; break; }
        }

        if (matched) {
            if (g_dev.verify_cb) g_dev.verify_cb("verify-match", true, g_dev.verify_cb_data);
        } else {
            if (g_dev.verify_cb) g_dev.verify_cb("verify-no-match", true, g_dev.verify_cb_data);
        }
        break;
    }

    pthread_mutex_lock(&g_lock);
    g_dev.state = DEV_STATE_IDLE;
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

int device_start_enroll(const char *finger, EnrollStatusCb cb, void *userdata) {
    pthread_mutex_lock(&g_lock);
    if (g_dev.state != DEV_STATE_IDLE) { pthread_mutex_unlock(&g_lock); return -1; }

    strncpy(g_dev.target_finger, finger, sizeof(g_dev.target_finger) - 1);
    g_dev.enroll_cb = cb;
    g_dev.enroll_cb_data = userdata;
    g_dev.state = DEV_STATE_ENROLL;
    atomic_store(&g_cancel_req, false);

    pthread_create(&g_worker_th, NULL, enroll_worker, NULL);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int device_stop_enroll(void) {
    pthread_mutex_lock(&g_lock);
    if (g_dev.state != DEV_STATE_ENROLL) { pthread_mutex_unlock(&g_lock); return 0; }
    atomic_store(&g_cancel_req, true);
    pthread_mutex_unlock(&g_lock);

    pthread_join(g_worker_th, NULL);
    return 0;
}

int device_start_verify(const char *finger, VerifyStatusCb cb, void *userdata) {
    pthread_mutex_lock(&g_lock);
    if (g_dev.state != DEV_STATE_IDLE) { pthread_mutex_unlock(&g_lock); return -1; }

    strncpy(g_dev.target_finger, finger, sizeof(g_dev.target_finger) - 1);
    g_dev.verify_cb = cb;
    g_dev.verify_cb_data = userdata;
    g_dev.state = DEV_STATE_VERIFY;
    atomic_store(&g_cancel_req, false);

    pthread_create(&g_worker_th, NULL, verify_worker, NULL);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int device_stop_verify(void) {
    pthread_mutex_lock(&g_lock);
    if (g_dev.state != DEV_STATE_VERIFY) { pthread_mutex_unlock(&g_lock); return 0; }
    atomic_store(&g_cancel_req, true);
    pthread_mutex_unlock(&g_lock);

    pthread_join(g_worker_th, NULL);
    return 0;
}