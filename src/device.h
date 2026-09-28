#ifndef DEVICE_H
#define DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include "matcher.h"

typedef enum {
    DEV_STATE_IDLE = 0,
    DEV_STATE_ENROLL,
    DEV_STATE_VERIFY
} DeviceState;

typedef void (*EnrollStatusCb)(const char *result, bool done, void *userdata);
typedef void (*VerifyStatusCb)(const char *result, bool done, void *userdata);

typedef struct {
    char claimed_by[128];
    char target_user[128];
    char target_finger[64];
    DeviceState state;

    EnrollStatusCb enroll_cb;
    void *enroll_cb_data;

    VerifyStatusCb verify_cb;
    void *verify_cb_data;
} DeviceContext;

int device_init(const char *model_path);
void device_cleanup(void);

DeviceState device_get_state(void);
const char *device_get_claimed_user(void);

int device_claim(const char *bus_caller, const char *target_user);
int device_release(const char *bus_caller);
void device_force_release(void);

int device_start_enroll(const char *finger, EnrollStatusCb cb, void *userdata);
int device_stop_enroll(void);

int device_start_verify(const char *finger, VerifyStatusCb cb, void *userdata);
int device_stop_verify(void);

#endif