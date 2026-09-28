#ifndef ELAN_DRV_H
#define ELAN_DRV_H

#include <stdint.h>
#include <libusb-1.0/libusb.h>

#define ELAN_VID 0x04f3
#define ELAN_PID 0x0903

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *handle;
    int width;
    int height;
    int stride;
    uint16_t *background;
} ElanDevice;

int elan_init(ElanDevice *dev);
void elan_close(ElanDevice *dev);
int elan_get_dims(ElanDevice *dev);
int elan_calibrate(ElanDevice *dev);
int elan_wait_finger(ElanDevice *dev);
int elan_wait_finger_timeout(ElanDevice *dev, int timeout_ms);
int elan_capture(ElanDevice *dev, uint16_t *out_buffer);
void elan_wait_release(void);

#endif