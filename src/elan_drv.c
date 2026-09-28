#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "elan_drv.h"

#define EP_CMD_OUT 0x01
#define EP_CMD_IN  0x83
#define EP_IMG_IN  0x82

static int elan_cmd(ElanDevice *dev, const uint8_t *cmd, uint8_t *res, int res_len, uint8_t ep_in) {
    int transferred = 0;
    int r = libusb_bulk_transfer(dev->handle, EP_CMD_OUT, (uint8_t *)cmd, 2, &transferred, 500);
    if (r < 0) return r;

    if (res_len > 0 && res != NULL) {
        r = libusb_bulk_transfer(dev->handle, ep_in, res, res_len, &transferred, 80);
    }
    return r;
}

int elan_init(ElanDevice *dev) {
    if (libusb_init(&dev->ctx) < 0) return -1;

    dev->handle = libusb_open_device_with_vid_pid(dev->ctx, ELAN_VID, ELAN_PID);
    if (!dev->handle) {
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
        return -1;
    }

    if (libusb_kernel_driver_active(dev->handle, 0) == 1) {
        libusb_detach_kernel_driver(dev->handle, 0);
    }

    if (libusb_claim_interface(dev->handle, 0) < 0) {
        libusb_close(dev->handle);
        libusb_exit(dev->ctx);
        return -1;
    }

    // Clear halted endpoints from any previously interrupted transfers
    libusb_clear_halt(dev->handle, EP_CMD_OUT);
    libusb_clear_halt(dev->handle, EP_CMD_IN);
    libusb_clear_halt(dev->handle, EP_IMG_IN);

    // Initialize sensor power state
    int transferred = 0;
    uint8_t pwr_cmd[] = {0x40, 0x31};
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, pwr_cmd, sizeof(pwr_cmd), &transferred, 1000);

    return 0;
}

void elan_close(ElanDevice *dev) {
    if (!dev) return;
    if (dev->background) {
        free(dev->background);
        dev->background = NULL;
    }
    if (dev->handle) {
        libusb_release_interface(dev->handle, 0);
        libusb_close(dev->handle);
        dev->handle = NULL;
    }
    if (dev->ctx) {
        libusb_exit(dev->ctx);
        dev->ctx = NULL;
    }
}

int elan_get_dims(ElanDevice *dev) {
    uint8_t cmd[] = {0x00, 0x0c};
    uint8_t res[4] = {0};

    if (elan_cmd(dev, cmd, res, sizeof(res), EP_CMD_IN) < 0) return -1;

    dev->width = res[2];
    dev->height = res[0];
    dev->stride = (dev->width == 79) ? 80 : dev->width;
    return 0;
}

int elan_calibrate(ElanDevice *dev) {
    if (!dev->background) {
        dev->background = calloc(dev->stride * dev->height, sizeof(uint16_t));
        if (!dev->background) return -1;
    }

    uint8_t pwr_cmd[] = {0x40, 0x31};
    uint8_t cap_cmd[] = {0x00, 0x09};
    uint8_t temp_buf[65536];
    int transferred = 0;
    int usb_len = ((dev->stride * dev->height * 2 + 63) / 64) * 64;

    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, pwr_cmd, sizeof(pwr_cmd), &transferred, 1000);
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cap_cmd, sizeof(cap_cmd), &transferred, 1000);

    int r = libusb_bulk_transfer(dev->handle, EP_IMG_IN, temp_buf, usb_len, &transferred, 3000);
    if (r < 0) return r;

    // Reset sensor to idle
    uint8_t stop_cmd[] = {0x00, 0x0b};
    libusb_bulk_transfer(dev->handle, EP_CMD_OUT, stop_cmd, sizeof(stop_cmd), &transferred, 1000);

    memcpy(dev->background, temp_buf, dev->stride * dev->height * sizeof(uint16_t));
    return 0;
}

int elan_wait_finger(ElanDevice *dev) {
    uint8_t cmd[] = {0x40, 0x3f};
    uint8_t res[1] = {0};

    while (1) {
        int r = elan_cmd(dev, cmd, res, sizeof(res), EP_CMD_IN);
        if (r == LIBUSB_ERROR_NO_DEVICE) return -1;
        if (r >= 0 && res[0] == 0x55) return 0;
        usleep(10000);
    }
}

int elan_wait_finger_timeout(ElanDevice *dev, int timeout_ms) {
    uint8_t cmd[] = {0x40, 0x3f};
    uint8_t res[1] = {0};
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        int r = elan_cmd(dev, cmd, res, sizeof(res), EP_CMD_IN);
        if (r == LIBUSB_ERROR_NO_DEVICE) return -1;
        if (r >= 0 && res[0] == 0x55) return 0;
        usleep(10000);
        elapsed += 10;
    }
    return -2;
}

int elan_capture(ElanDevice *dev, uint16_t *out_buffer) {
    uint8_t cmd[] = {0x00, 0x09};
    uint8_t temp_buf[65536];
    int transferred = 0;

    int r = libusb_bulk_transfer(dev->handle, EP_CMD_OUT, cmd, sizeof(cmd), &transferred, 1000);
    if (r < 0) return r;

    int frame_bytes = dev->stride * dev->height * sizeof(uint16_t);
    int usb_aligned_len = ((frame_bytes + 63) / 64) * 64;

    r = libusb_bulk_transfer(dev->handle, EP_IMG_IN, temp_buf, usb_aligned_len, &transferred, 3000);
    if (r < 0) return r;

    int copy_bytes = (transferred < frame_bytes) ? transferred : frame_bytes;
    memcpy(out_buffer, temp_buf, copy_bytes);

    // Send stop command
    elan_stop(dev);

    if (dev->background) {
        int total = dev->stride * dev->height;
        uint32_t sum = 0;
        for (int i = 0; i < total; i++) {
            out_buffer[i] = (out_buffer[i] > dev->background[i]) ? (out_buffer[i] - dev->background[i]) : 0;
            sum += out_buffer[i];
        }
        if (sum == 0) {
            // All pixels darker than baseline, finger was touching during calibration
            return -2;
        }
    }
    return 0;
}

int elan_arm(ElanDevice *dev) {
    if (!dev || !dev->handle) return -1;
    uint8_t pwr_cmd[] = {0x40, 0x31};
    int transferred = 0;
    return libusb_bulk_transfer(dev->handle, EP_CMD_OUT, pwr_cmd, sizeof(pwr_cmd), &transferred, 500);
}

int elan_stop(ElanDevice *dev) {
    if (!dev || !dev->handle) return -1;
    uint8_t stop_cmd[] = {0x00, 0x0b};
    int transferred = 0;
    return libusb_bulk_transfer(dev->handle, EP_CMD_OUT, stop_cmd, sizeof(stop_cmd), &transferred, 500);
}

int elan_wait_release_timeout(ElanDevice *dev, int timeout_ms) {
    uint8_t cmd[] = {0x40, 0x3f};
    uint8_t res[1] = {0};
    int elapsed = 0;

    while (elapsed < timeout_ms) {
        int r = elan_cmd(dev, cmd, res, sizeof(res), EP_CMD_IN);
        if (r < 0 || res[0] != 0x55) {
            return 0;
        }
        usleep(20000);
        elapsed += 20;
    }
    return 0;
}